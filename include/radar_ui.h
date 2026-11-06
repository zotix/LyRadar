#pragma once

#include <WebServer.h>
#include <DNSServer.h>
#include <time.h>
#include "cyd_touch.h"
#include "radar_cache.h"
#include "gif_decoder.h"
#include "promptpay_qr.h"

static bool runWiFiScannerAndKeyboard();

// ============================================================================
// Backlight PWM Control
// ============================================================================
static void applyBacklight() {
    if (g_standbyMode) {
        ledcWrite(0, 30); // Standby minimum duty (~12%, clear enough to see but saves ~80 mA)
        return;
    }
    uint8_t duty = 255;
    if (g_brightMode == 1)      duty = 255; // 100%
    else if (g_brightMode == 2) duty = 140; // 55%
    else if (g_brightMode == 3) duty = 50;  // 20%
    else { // 0 = AUTO: Enhanced Smart Night Dimming with Hysteresis
        time_t now = time(nullptr);
        struct tm* ti = localtime(&now);
        bool isNight = (ti && ti->tm_year > 120 && (ti->tm_hour >= 22 || ti->tm_hour < 6));
        
        static int s_filteredLdr = -1;
        int ldrRaw = analogRead(CYD_LDR_PIN);
        if (s_filteredLdr < 0) s_filteredLdr = ldrRaw;
        else s_filteredLdr = (s_filteredLdr * 3 + ldrRaw) / 4; // Smooth out ADC noise

        static uint8_t s_lastAutoDuty = 255;
        if (isNight) {
            duty = 30; // Night dimming
        } else if (s_lastAutoDuty == 30) {
            duty = (s_filteredLdr < 2600) ? ((s_filteredLdr < 1400) ? 255 : 120) : 30;
        } else if (s_lastAutoDuty == 120) {
            if (s_filteredLdr > 2950)      duty = 30;
            else if (s_filteredLdr < 1450) duty = 255;
            else                           duty = 120;
        } else { // Current duty is 255
            if (s_filteredLdr > 2950)      duty = 30;
            else if (s_filteredLdr > 1750) duty = 120;
            else                           duty = 255;
        }
        s_lastAutoDuty = duty;
    }
    ledcWrite(0, duty);
}

// ============================================================================
// HUD & Radar Frame Rendering (16-line strip sprite -> zero flicker)
// ============================================================================
static void drawWeatherConditionIcon(TFT_eSPI& g, int x, int y, int16_t code) {
    if (code == 0) {
        // ☀️ Clear / Sunny: bright yellow sun with rays
        g.fillCircle(x + 5, y + 6, 3, TFT_YELLOW);
        g.drawPixel(x + 5, y + 2, TFT_YELLOW); // N
        g.drawPixel(x + 5, y + 10, TFT_YELLOW); // S
        g.drawPixel(x + 1, y + 6, TFT_YELLOW); // W
        g.drawPixel(x + 9, y + 6, TFT_YELLOW); // E
        g.drawPixel(x + 2, y + 3, TFT_ORANGE); // NW
        g.drawPixel(x + 8, y + 3, TFT_ORANGE); // NE
        g.drawPixel(x + 2, y + 9, TFT_ORANGE); // SW
        g.drawPixel(x + 8, y + 9, TFT_ORANGE); // SE
    } else if (code >= 1 && code <= 3) {
        // ⛅ Cloudy / Partly Cloudy: white-grey cloud with subtle sun peak
        g.fillCircle(x + 3, y + 4, 2, TFT_YELLOW);
        g.fillRoundRect(x + 1, y + 5, 9, 5, 2, TFT_LIGHTGREY);
        g.fillCircle(x + 5, y + 4, 3, TFT_WHITE);
        g.fillCircle(x + 8, y + 5, 2, TFT_WHITE);
    } else if ((code >= 51 && code <= 65) || (code >= 80 && code <= 82)) {
        // 🌧️ Rain / Drizzle: cloud + 2 rain streaks
        g.fillRoundRect(x + 1, y + 3, 9, 4, 2, TFT_LIGHTGREY);
        g.fillCircle(x + 5, y + 3, 3, TFT_WHITE);
        g.drawLine(x + 3, y + 8, x + 2, y + 11, TFT_CYAN);
        g.drawLine(x + 7, y + 8, x + 6, y + 11, TFT_CYAN);
    } else if (code >= 95 && code <= 99) {
        // ⛈️ Thunderstorm: dark cloud + yellow lightning bolt
        g.fillRoundRect(x + 1, y + 2, 9, 4, 2, 0x632C); // Dark slate
        g.fillCircle(x + 5, y + 2, 3, TFT_LIGHTGREY);
        g.drawLine(x + 5, y + 6, x + 3, y + 8, TFT_YELLOW);
        g.drawLine(x + 3, y + 8, x + 6, y + 8, TFT_YELLOW);
        g.drawLine(x + 6, y + 8, x + 4, y + 11, TFT_YELLOW);
    } else {
        // Default / Unknown: subtle cloud
        g.fillRoundRect(x + 1, y + 4, 9, 4, 2, TFT_LIGHTGREY);
        g.fillCircle(x + 5, y + 3, 2, TFT_WHITE);
    }
}

static void drawTopHudWeather(TFT_eSPI& g, int y) {
    time_t now = time(nullptr);
    struct tm* ti = localtime(&now);
    char timeStr[12];
    if (ti && ti->tm_year > 120) {
        snprintf(timeStr, sizeof(timeStr), "%02d:%02d", ti->tm_hour, ti->tm_min);
    } else {
        snprintf(timeStr, sizeof(timeStr), "--:--");
    }

    char tempStr[10];
    if (g_stationTemp > -90.0f) {
        snprintf(tempStr, sizeof(tempStr), "%.1f", g_stationTemp);
    } else {
        snprintf(tempStr, sizeof(tempStr), "--.-");
    }

    char humStr[8];
    if (g_stationHumidity >= 0) {
        snprintf(humStr, sizeof(humStr), "%d%%", (int)g_stationHumidity);
    } else {
        snprintf(humStr, sizeof(humStr), "--%%");
    }

    char rainStr[12];
    if (g_rainPct > 0) {
        char lvlChar = (g_rainLevel >= 3) ? 'H' : ((g_rainLevel == 2) ? 'M' : 'L');
        snprintf(rainStr, sizeof(rainStr), "%u%%%c", (unsigned)g_rainPct, lvlChar);
    } else {
        snprintf(rainStr, sizeof(rainStr), "0%%");
    }

    int timeW = g.textWidth(timeStr, 2);
    int tempW = g.textWidth(tempStr, 2);
    int humW  = g.textWidth(humStr, 2);
    int rainW = g.textWidth(rainStr, 2);

    const int gap = 4;
    // Time + Weather Icon: timeW + 2 (gap) + 11 (icon) = timeW + 13
    // Temp: 7 (icon) + 2 (gap) + tempW + 4 (degree) = tempW + 13
    // Hum:  7 (icon) + 2 (gap) + humW = humW + 9
    // Rain: 10 (icon) + 2 (gap) + rainW = rainW + 12
    int totalW = (timeW + 13) + gap + (tempW + 13) + gap + (humW + 9) + gap + (rainW + 12);

    // Center between left station badge (ends ~86) and right frame counter (starts ~268)
    int leftLimit  = 66;
    int rightLimit = 268;
    int startX = leftLimit + (rightLimit - leftLimit - totalW) / 2;
    if (startX < leftLimit) startX = leftLimit;

    // Solid black pill background for high contrast over radar clouds
    g.fillRect(startX - 2, y, totalW + 4, 15, TFT_BLACK);

    g.setTextDatum(TL_DATUM);

    // 1. Clock (HH:MM) + Weather Condition Micro-Icon
    int curX = startX;
    g.setTextColor(TFT_YELLOW, TFT_BLACK);
    g.drawString(timeStr, curX, y, 2);
    curX += timeW + 2;
    drawWeatherConditionIcon(g, curX, y, g_weatherCode);
    curX += 11 + gap;

    // 2. Thermometer Micro-Icon (stem + bulb) & Temperature
    g.fillRect(curX + 2, y + 2, 2, 6, TFT_ORANGE);
    g.fillCircle(curX + 2, y + 10, 2, TFT_RED);
    curX += 7 + 2;
    uint16_t tempColor = (g_stationTemp >= 35.0f) ? TFT_RED : ((g_stationTemp >= 30.0f) ? TFT_ORANGE : TFT_CYAN);
    if (g_stationTemp < -90.0f) tempColor = TFT_LIGHTGREY;
    g.setTextColor(tempColor, TFT_BLACK);
    g.drawString(tempStr, curX, y, 2);
    curX += tempW;
    // Degree circle
    g.drawCircle(curX + 2, y + 3, 1, tempColor);
    curX += 4 + gap;

    // 3. Water Droplet Micro-Icon & Relative Humidity
    g.fillTriangle(curX + 3, y + 2, curX, y + 7, curX + 6, y + 7, TFT_CYAN);
    g.fillCircle(curX + 3, y + 8, 3, TFT_CYAN);
    curX += 7 + 2;
    g.setTextColor(TFT_CYAN, TFT_BLACK);
    g.drawString(humStr, curX, y, 2);
    curX += humW + gap;

    // 4. Rain Cloud Micro-Icon & Radar Echo Rain % (with L/M/H intensity)
    g.fillRoundRect(curX, y + 2, 10, 5, 2, TFT_LIGHTGREY);
    g.fillCircle(curX + 4, y + 2, 2, TFT_WHITE);
    uint16_t rainColor = (g_rainLevel >= 3 || g_rainPct >= 20) ? TFT_RED
                       : ((g_rainLevel == 2 || g_rainPct >= 5) ? TFT_YELLOW : TFT_GREENYELLOW);
    uint16_t dropColor = (g_rainLevel >= 3) ? TFT_RED : ((g_rainLevel == 2) ? TFT_YELLOW : TFT_CYAN);
    g.drawLine(curX + 2, y + 8, curX + 1, y + 12, dropColor);
    g.drawLine(curX + 6, y + 8, curX + 5, y + 12, dropColor);
    curX += 10 + 2;
    g.setTextColor(rainColor, TFT_BLACK);
    g.drawString(rainStr, curX, y, 2);
}

static void drawClockAt(TFT_eSPI& g, int y) {
    drawTopHudWeather(g, y);
}

static void drawHudInto(TFT_eSPI& g, int yOff, const HudCtx& c) {
    const int topY = 1;
    const int dlY  = DST_HEIGHT - 18;
    const int barY = DST_HEIGHT - 2;

    if (yOff == 0) {
        g.setTextDatum(TL_DATUM);
        g.setTextColor(TFT_WHITE, TFT_BLACK);
        char modeBuf[28];
        const char* staCode = kStations[(c.preview ? g_dlStation : g_bankStation) % NUM_STATIONS].shortCode;
        if (g_paused && !c.preview) {
            snprintf(modeBuf, sizeof(modeBuf), " [PAUSED] ");
        } else if (c.zoom) {
            snprintf(modeBuf, sizeof(modeBuf), " Z:%s%s ", g_regionName, c.instantZoom ? "*" : "");
        } else {
            snprintf(modeBuf, sizeof(modeBuf), " %s ", staCode);
        }
        g.drawString(modeBuf, 4, topY, 2);

        drawTopHudWeather(g, topY);

        char buf[16];
        snprintf(buf, sizeof(buf), " %d/%d ", c.dispFrame + 1, c.totalFrames);
        g.setTextDatum(TR_DATUM);
        g.setTextColor(TFT_WHITE, (c.dispFrame == c.totalFrames - 1) ? TFT_DARKGREEN : TFT_NAVY);
        g.drawString(buf, DST_WIDTH - 4, topY, 2);
    }

    if (yOff + RENDER_LINES > dlY) {
        int stage = g_dlStage;
        uint32_t ageMs = (g_lastOkUpdateMs > 0) ? (millis() - g_lastOkUpdateMs) : 0;
        if (!g_autoCycle && (stage == DL_CONNECTING || stage == DL_DOWNLOADING || stage == DL_SD_RECROP)) {
            char buf[22];
            if (stage == DL_SD_RECROP)                          snprintf(buf, sizeof(buf), " SD CROP %d/%d ", g_dlFrames, MAX_RADAR_FRAMES);
            else if (stage == DL_CONNECTING || g_dlFrames == 0) snprintf(buf, sizeof(buf), " CHECKING ");
            else snprintf(buf, sizeof(buf), " DL %d/%d ", g_dlFrames, MAX_RADAR_FRAMES);
            g.setTextDatum(TL_DATUM);
            g.setTextColor(TFT_BLACK, TFT_CYAN);
            g.drawString(buf, 4, dlY - yOff, 2);
        } else if (ageMs > 15UL * 60UL * 1000UL) {
            char buf[22];
            snprintf(buf, sizeof(buf), " OLD %lum ", (unsigned long)(ageMs / 60000UL));
            g.setTextDatum(TL_DATUM);
            g.setTextColor(TFT_BLACK, TFT_ORANGE);
            g.drawString(buf, 4, dlY - yOff, 2);
        }

        int count = max(1, c.stepCount);
        int barW  = (constrain(c.stepPos, 0, count) * DST_WIDTH) / count;
        uint16_t barColor = c.preview ? TFT_CYAN : ((c.stepPos >= count) ? TFT_GREEN : TFT_YELLOW);
        g.fillRect(0, barY - yOff, barW, 2, barColor);
        if (barW < DST_WIDTH) g.fillRect(barW, barY - yOff, DST_WIDTH - barW, 2, TFT_BLACK);
    }
}

static void renderFrame(uint32_t flashAddr, const uint16_t* pal, const HudCtx& ctx) {
    uint16_t* px = (uint16_t*)spr.getPointer();
    if (!px) return;
    for (int y = 0; y < DST_HEIGHT; y += RENDER_LINES) {
        size_t byteCount = (size_t)RENDER_LINES * DST_WIDTH;
        if (esp_partition_read(radarPartition, flashAddr + (uint32_t)y * DST_WIDTH, lineIdxBuf, byteCount) != ESP_OK) {
            continue;
        }
        for (size_t i = 0; i < byteCount; i++) px[i] = pal[lineIdxBuf[i]];
        drawHudInto(spr, y, ctx);
        spr.pushSprite(0, y);
    }
    g_screenHasRadar = true;
}

static void renderInstantZoomFromFull(uint32_t fullFlashAddr, const uint16_t* pal, const HudCtx& ctx) {
    uint16_t* px = (uint16_t*)spr.getPointer();
    if (!px) return;
    int zx0 = g_targetZoomX;
    int zy0 = g_targetZoomY;

    int cachedFy = -1;
    for (int y = 0; y < DST_HEIGHT; y += RENDER_LINES) {
        for (int r = 0; r < RENDER_LINES; r++) {
            int dy = y + r;
            int sy = zy0 + dy;
            int fy = constrain((sy * DST_HEIGHT) / (int)g_srcHeight, 0, DST_HEIGHT - 1);
            if (fy != cachedFy) {
                esp_partition_read(radarPartition, fullFlashAddr + (uint32_t)fy * DST_WIDTH, previewRowBuf, DST_WIDTH);
                cachedFy = fy;
            }
            uint16_t* rowOut = px + r * DST_WIDTH;
            for (int dx = 0; dx < DST_WIDTH; dx++) {
                int sx = zx0 + dx;
                int fx = constrain((sx * DST_WIDTH) / (int)g_srcWidth, 0, DST_WIDTH - 1);
                rowOut[dx] = pal[previewRowBuf[fx]];
            }
        }
        drawHudInto(spr, y, ctx);
        spr.pushSprite(0, y);
    }
    g_screenHasRadar = true;
}

static void renderDownloadPreview(int frameIdx);

static bool renderZoomFrameFromSd(const char* path, int frameIdx, const HudCtx& ctx) {
    if (!g_sdReady) return false;
    File f = SD.open(path, FILE_READ);
    if (!f) return false;
    ZoomCacheHeader hdr;
    if (f.read((uint8_t*)&hdr, sizeof(hdr)) != sizeof(hdr) || hdr.magic != 0x5A4F4F4D) {
        f.close();
        return false;
    }
    if (frameIdx < 0 || frameIdx >= hdr.frameCount) {
        f.close();
        return false;
    }
    uint32_t frameOffset = sizeof(hdr) + (uint32_t)frameIdx * FRAME_BYTES;
    if (!f.seek(frameOffset)) {
        f.close();
        return false;
    }
    uint16_t* px = (uint16_t*)spr.getPointer();
    if (!px) { f.close(); return false; }

    for (int y = 0; y < DST_HEIGHT; y += RENDER_LINES) {
        size_t byteCount = (size_t)RENDER_LINES * DST_WIDTH;
        if (f.read(lineIdxBuf, byteCount) != byteCount) break;
        for (size_t i = 0; i < byteCount; i++) px[i] = hdr.palette[lineIdxBuf[i]];
        drawHudInto(spr, y, ctx);
        spr.pushSprite(0, y);
    }
    f.close();
    g_screenHasRadar = true;
    return true;
}

static void renderCurrent() {
    if (g_menuOpen || g_activeBank < 0) return;

    // Station switched: in manual mode, show the new station's frames as soon as they are decoded.
    // In AUTO mode, keep playing the current station's full 12-frame loop smoothly until the new bank commits!
    if (!g_autoCycle && g_targetStation != g_bankStation) {
        int st = g_dlStage, df = g_dlFrames;
        if ((st == DL_DOWNLOADING || st == DL_SD_RECROP) && df > 0 && g_dlStation == g_targetStation) {
            renderDownloadPreview(g_stepIdx % df);
            return;
        }
    }
    bool zoom = g_zoomMode && (g_zoomCount > 0 || g_frameCount > 0);
    bool sdZoomOk = false;
    char sdZPath[48] = "";
    if (zoom && g_sdReady) {
        getSdZoomPath(g_bankStation, sdZPath, sizeof(sdZPath));
        if (isSdZoomValid(sdZPath, g_bankStation, g_targetZoomX, g_targetZoomY)) {
            sdZoomOk = true;
        }
    }

    int totalSteps = (int)g_frameCount;
    if (totalSteps <= 0) return;
    if (g_stepIdx < 0 || g_stepIdx >= totalSteps) g_stepIdx = 0;

    int dispFrame = g_stepIdx;

    if (zoom && sdZoomOk) {
        HudCtx ctx = { dispFrame, totalSteps, g_stepIdx + 1, totalSteps, true, false, false };
        if (renderZoomFrameFromSd(sdZPath, g_stepIdx, ctx)) {
            return;
        }
    }

    bool hasNativeCrop = zoom && (g_zoomCount > 0) &&
                         (g_targetZoomX == g_bankZoomX) &&
                         (g_targetZoomY == g_bankZoomY) &&
                         (g_targetStation == g_bankStation);

    if (zoom && !hasNativeCrop) {
        // Different station still loading -> keep showing the cached map instead of a wrong upscale
        if (g_targetStation != g_bankStation) {
            HudCtx ctx = { g_stepIdx, totalSteps, g_stepIdx + 1, totalSteps, false, false, false };
            renderFrame(fullAddr(g_activeBank, min(g_stepIdx, totalSteps - 1)), g_palette, ctx);
            return;
        }
        // Instant Digital Zoom: all 12 frames smoothly from fullAddr!
        HudCtx ctx = { dispFrame, totalSteps, g_stepIdx + 1, totalSteps, true, false, true };
        renderInstantZoomFromFull(fullAddr(g_activeBank, dispFrame), g_palette, ctx);
        return;
    }

    uint32_t addr = zoom ? zoomAddr(g_activeBank, min((int)g_zoomCount - 1, g_stepIdx / 2))
                         : fullAddr(g_activeBank, g_stepIdx);
    HudCtx ctx = { dispFrame, totalSteps, g_stepIdx + 1, totalSteps, zoom, false, false };
    renderFrame(addr, g_palette, ctx);
}

static void renderDownloadPreview(int frameIdx) {
    if (g_menuOpen) return;
    int bank = g_dlTargetBank;
    if (bank < 0) return;
    HudCtx ctx = { frameIdx, MAX_RADAR_FRAMES, frameIdx + 1, MAX_RADAR_FRAMES, false, true, false };
    renderFrame(fullAddr(bank, frameIdx), g_dlPalette, ctx);
}

static void drawStatusScreen(const char* line1, const char* line2) {
    if (g_menuOpen) return;
    g_screenHasRadar = false;
    tft.fillScreen(TFT_BLACK);
    tft.setTextDatum(MC_DATUM);
    tft.setTextColor(TFT_CYAN, TFT_BLACK);
    tft.drawString(line1, DST_WIDTH / 2, DST_HEIGHT / 2 - 20, 2);
    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    tft.drawString(line2, DST_WIDTH / 2, DST_HEIGHT / 2 + 10, 2);
}

// Restores the Radar display after closing Menu or rotating screen (never leaves screen blank/stuck)
static void restoreRadarScreen() {
    g_menuOpen = false;
    if (g_activeBank >= 0) {
        renderCurrent();
    } else if (g_dlStage == DL_DOWNLOADING && g_dlFrames > 0) {
        renderDownloadPreview(g_dlFrames - 1);
    } else if (g_dlStage == DL_CONNECTING || g_dlStage == DL_DOWNLOADING) {
        drawStatusScreen("Downloading Radar Loop...", "Please wait...");
    } else {
        drawStatusScreen("LyRadar Weather Display", "Tap Top-Left for Menu / Tap to Zoom");
    }
    g_lastFrameStep = millis();
}

// ============================================================================
// Interactive 3-Point Affine Touch Calibration Screen
// ============================================================================
static void drawCrosshair(int16_t x, int16_t y, uint16_t color) {
    tft.drawCircle(x, y, 14, color);
    tft.drawCircle(x, y, 7, color);
    tft.drawLine(x - 18, y, x + 18, y, color);
    tft.drawLine(x, y - 18, x, y + 18, color);
}

static void runTouchCalibration() {
    while (digitalRead(BOOT_BTN_PIN) == LOW) delay(15);

    // Calibrate in base rotation 1 (0-deg landscape); flip180 is applied in readTouch()
    tft.setRotation(1);

    constexpr int16_t screenPts[3][2] = {
        {28, 36},
        {DST_WIDTH - 29, DST_HEIGHT / 2},
        {DST_WIDTH / 2, DST_HEIGHT - 29},
    };
    int16_t rawPts[3][2] = {};
    const char* labels[3] = {"top-left target (1/3)", "right target (2/3)", "bottom target (3/3)"};

    for (uint8_t i = 0; i < 3; ++i) {
        tft.fillScreen(TFT_BLACK);
        tft.setTextDatum(TC_DATUM);
        tft.setTextColor(TFT_YELLOW, TFT_BLACK);
        tft.drawString("Touch Calibration", DST_WIDTH / 2, 12, 4);
        tft.setTextColor(TFT_WHITE, TFT_BLACK);
        tft.drawString(String("Tap and hold the ") + labels[i], DST_WIDTH / 2, 48, 2);
        drawCrosshair(screenPts[i][0], screenPts[i][1], TFT_CYAN);

        if (!touch.waitForStableRaw(rawPts[i][0], rawPts[i][1])) {
            tft.fillScreen(TFT_BLACK);
            tft.setTextDatum(MC_DATUM);
            tft.setTextColor(TFT_RED, TFT_BLACK);
            tft.drawString("Calibration cancelled", DST_WIDTH / 2, DST_HEIGHT / 2, 4);
            delay(1000);
            tft.setRotation(g_flipScreen ? 3 : 1);
            return;
        }
        drawCrosshair(screenPts[i][0], screenPts[i][1], TFT_GREEN);
        delay(150);
    }

    CYDTouch::TouchCalibration next;
    if (CYDTouch::computeAffine(rawPts, screenPts, next)) {
        touch.cal = next;
        prefs.begin("radar_cfg", false);
        touch.saveCalibration(prefs);
        prefs.end();

        Serial.printf("[TOUCH] Affine Cal Saved: ax=%.4f bx=%.4f cx=%.1f | ay=%.4f by=%.4f cy=%.1f\n",
                      next.ax, next.bx, next.cx, next.ay, next.by, next.cy);
        tft.fillScreen(TFT_BLACK);
        tft.setTextDatum(MC_DATUM);
        tft.setTextColor(TFT_GREEN, TFT_BLACK);
        tft.drawString("Calibration saved!", DST_WIDTH / 2, DST_HEIGHT / 2, 4);
        delay(1000);
    } else {
        tft.fillScreen(TFT_BLACK);
        tft.setTextDatum(MC_DATUM);
        tft.setTextColor(TFT_RED, TFT_BLACK);
        tft.drawString("Calibration failed", DST_WIDTH / 2, DST_HEIGHT / 2, 4);
        delay(1200);
    }

    tft.setRotation(g_flipScreen ? 3 : 1);
}

// ============================================================================
// Global Physical BOOT Button Handler (Universal across ALL screens)
// 3s = Touch Calibration, 10s = Invert Display, <3s = Short-Press Action
// ============================================================================
enum GlobalBootAction {
    BOOT_ACT_NONE = 0,
    BOOT_ACT_SHORT_PRESS,
    BOOT_ACT_CALIBRATED,
    BOOT_ACT_INVERTED
};

template <typename F>
static GlobalBootAction checkGlobalBootButton(F onRedraw) {
    if (digitalRead(BOOT_BTN_PIN) != LOW) {
        return BOOT_ACT_NONE;
    }

    g_lastTouchMs = millis();
    g_lastStationCycleMs = millis();
    uint32_t pressStart = millis();
    bool shown3s = false;

    while (digitalRead(BOOT_BTN_PIN) == LOW) {
        uint32_t heldMs = millis() - pressStart;

        if (heldMs >= 3000 && !shown3s) {
            shown3s = true;
            tft.setTextDatum(MC_DATUM);
            tft.setTextColor(TFT_BLACK, TFT_YELLOW);
            tft.drawString(" [3s] RELEASE = CAL TOUCH (10s = INVERT) ", DST_WIDTH / 2, DST_HEIGHT / 2, 2);
        }

        if (heldMs >= 10000) {
            g_invertDisplay = !g_invertDisplay;
            tft.invertDisplay(g_invertDisplay);
            prefs.begin("radar_cfg", false);
            prefs.putBool("inv", g_invertDisplay);
            prefs.end();

            tft.setTextDatum(MC_DATUM);
            tft.setTextColor(TFT_WHITE, TFT_DARKGREEN);
            tft.drawString(" [10s] DISPLAY INVERTED! ", DST_WIDTH / 2, DST_HEIGHT / 2, 2);
            while (digitalRead(BOOT_BTN_PIN) == LOW) delay(15);
            onRedraw();
            return BOOT_ACT_INVERTED;
        }
        delay(15);
    }

    uint32_t totalHeld = millis() - pressStart;
    if (totalHeld >= 3000) {
        // Released between 3s and 10s -> Calibrate Touch!
        runTouchCalibration();
        onRedraw();
        return BOOT_ACT_CALIBRATED;
    }

    // Released < 3s -> Short press
    return BOOT_ACT_SHORT_PRESS;
}

// ============================================================================
// Interactive Menu Screen (Long-Press >= 500ms on Touch Screen)
// ============================================================================
static void drawMenuBtn(int x, int y, int w, int h, const char* label, bool active, uint16_t activeBg = TFT_DARKGREEN, uint16_t activeBorder = TFT_GREEN, bool doubleBorder = false) {
    uint16_t bg     = active ? activeBg : 0x18E3;
    uint16_t border = active ? activeBorder : TFT_CYAN;
    if (doubleBorder) border = TFT_YELLOW;
    tft.fillRoundRect(x, y, w, h, 5, bg);
    tft.drawRoundRect(x, y, w, h, 5, border);
    if (doubleBorder) {
        tft.drawRoundRect(x + 1, y + 1, w - 2, h - 2, 4, border);
    }
    tft.setTextDatum(MC_DATUM);
    tft.setTextColor(TFT_WHITE, bg);
    tft.drawString(label, x + w / 2, y + h / 2, 2);
}

static void drawMenuBtn2L(int x, int y, int w, int h, const char* line1, const char* line2, bool active, uint16_t activeBg, uint16_t activeBorder, uint16_t line2Color = TFT_WHITE) {
    uint16_t bg     = active ? activeBg : 0x18E3;
    uint16_t border = active ? activeBorder : TFT_CYAN;
    tft.fillRoundRect(x, y, w, h, 5, bg);
    tft.drawRoundRect(x, y, w, h, 5, border);
    tft.setTextDatum(MC_DATUM);
    tft.setTextColor(TFT_WHITE, bg);
    tft.drawString(line1, x + w / 2, y + 18, 2);
    tft.setTextColor(line2Color, bg);
    tft.drawString(line2, x + w / 2, y + 40, 1);
}

// Menu layout (320x240):
//   Header   y=0..28   : title + [WIFI] + CLOSE X
//   Row 1    y=32..72  : stations 0..3 (BMA-NK, BMA-NC, TMD-SVP, TMD-SKM)
//   Row 2    y=76..116 : stations 4..7 (TMD-RYG, TMD-CRI, TMD-KKN, TMD-SRT)
//   Row 3    y=120..160: stations 8..11 (TMD-PHS, TMD-UBN, TMD-PKT, TMD-STP)
//   Row 4    y=168..228: FULL MAP / TLAPSE / BRI / CALIB / UPDATE
static constexpr int MENU_R1_Y = 32,  MENU_R1_H = 40;
static constexpr int MENU_R2_Y = 76,  MENU_R2_H = 40;
static constexpr int MENU_R3_Y = 120, MENU_R3_H = 40;
static constexpr int MENU_R4_Y = 168, MENU_R4_H = 58;
static constexpr int MENU_COL4_W = 75;  // 4 columns: x = 4 + i*79
static constexpr int MENU_COL5_W = 60;  // 5 columns: x = 4 + i*63

static const int8_t kPresetDx[5] = { 0, -1, 0, 1, 0 };  // NORTH, WEST, CENTER, EAST, SOUTH
static const int8_t kPresetDy[5] = { -1, 0, 0, 0, 1 };
static const char*  kPresetLbl[5] = { "NORTH", "WEST", "CENTER", "EAST", "SOUTH" };

static void presetZoomFor(int preset, uint16_t& zx, uint16_t& zy) {
    uint8_t  sta = g_targetStation % NUM_STATIONS;
    uint16_t sw  = (sta == g_bankStation) ? g_srcWidth  : kStations[sta].defWidth;
    uint16_t sh  = (sta == g_bankStation) ? g_srcHeight : kStations[sta].defHeight;
    uint16_t cx, cy;
    getStationDefaultZoom(sta, sw, sh, cx, cy);
    int x = (int)cx + kPresetDx[preset] * 180;
    int y = (int)cy + kPresetDy[preset] * 170;
    zx = (uint16_t)constrain(x, 0, max(0, (int)sw - DST_WIDTH));
    zy = (uint16_t)constrain(y, 0, max(0, (int)sh - DST_HEIGHT));
}

static void drawTimelapseHud() {
    tft.fillRect(0, 0, DST_WIDTH, 22, TFT_BLACK);
    tft.setTextDatum(TL_DATUM);
    tft.setTextColor(TFT_YELLOW, TFT_BLACK);
    char buf[32];
    snprintf(buf, sizeof(buf), " [TL] %s ", kStations[g_targetStation % NUM_STATIONS].shortCode);
    tft.drawString(buf, 4, 3, 2);

    tft.setTextDatum(TC_DATUM);
    tft.setTextColor(TFT_CYAN, TFT_BLACK);
    tft.drawString(g_timelapseLabel, DST_WIDTH / 2, 3, 2);

    tft.setTextDatum(TR_DATUM);
    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    char countBuf[16];
    snprintf(countBuf, sizeof(countBuf), " %d/%d ", g_timelapseIdx + 1, g_timelapseCount);
    tft.drawString(countBuf, DST_WIDTH - 4, 3, 2);

    // Bottom progress bar
    int count = max(1, g_timelapseCount);
    int barW  = ((g_timelapseIdx + 1) * DST_WIDTH) / count;
    tft.fillRect(0, DST_HEIGHT - 3, barW, 3, TFT_CYAN);
    if (barW < DST_WIDTH) tft.fillRect(barW, DST_HEIGHT - 3, DST_WIDTH - barW, 3, TFT_BLACK);
}

static void waitTouchRelease() {
    uint32_t start = millis();
    while (touch.isTouching() && (millis() - start < 450)) {
        delay(10);
    }
    delay(20);
}

static void drawMenuScreen();

// ============================================================================
// About Me Screen & PromptPay QR (Buy Me a Coffee ☕)
// ============================================================================
static void drawAboutScreen() {
    g_screenHasRadar = false;
    tft.fillScreen(0x0821); // Sleek tech dark background

    // Header bar
    tft.fillRect(0, 0, DST_WIDTH, 26, TFT_NAVY);
    tft.drawFastHLine(0, 26, DST_WIDTH, TFT_CYAN);
    tft.setTextDatum(ML_DATUM);
    tft.setTextColor(TFT_YELLOW, TFT_NAVY);
    tft.drawString(" LyRadar CYD " APP_VERSION, 6, 13, 2);
    tft.setTextDatum(MR_DATUM);
    tft.setTextColor(TFT_WHITE, TFT_NAVY);
    tft.drawString("[ CLOSE X ]", DST_WIDTH - 6, 13, 2);

    // Left Column: Author, Hardware & License Info
    int lx = 6;
    tft.setTextDatum(TL_DATUM);
    tft.setTextColor(TFT_GREENYELLOW, 0x0821);
    tft.drawString("Author: " APP_AUTHOR, lx, 32, 2);

    tft.setTextColor(TFT_WHITE, 0x0821);
    tft.drawString("GitHub: zotix/LyRadar", lx, 50, 2);
    tft.drawString("Board: ESP32-2432S028", lx, 68, 2);
    tft.drawString("TFT: 2.8\" ST7789/ILI9341", lx, 86, 2);
    tft.drawString("Stations: 12 BMA & TMD", lx, 104, 2);
    tft.drawString("CPU: 80MHz Power-Save", lx, 122, 2);
    tft.drawString("Cache: Double Flash + SD", lx, 140, 2);

    tft.setTextColor(TFT_CYAN, 0x0821);
    tft.drawString("License: Non-Commercial", lx, 160, 2);
    tft.setTextColor(TFT_LIGHTGREY, 0x0821);
    tft.drawString("Personal: Free / Open", lx, 178, 1);
    tft.drawString("Commercial: Paid License Req.", lx, 190, 1);

    // Right Column: Support Coffee & PromptPay Card
    int rx   = 182;
    int ry   = 29;
    int boxW = 132;
    int boxH = 182;

    // Card background
    tft.fillRoundRect(rx, ry, boxW, boxH, 6, TFT_WHITE);
    tft.drawRoundRect(rx, ry, boxW, boxH, 6, 0x0113); // PromptPay Deep Blue
    tft.drawRoundRect(rx + 1, ry + 1, boxW - 2, boxH - 2, 5, 0x0113);

    // PromptPay Header banner
    tft.fillRoundRect(rx, ry, boxW, 20, 5, 0x0113);
    tft.fillRect(rx, ry + 15, boxW, 5, 0x0113);
    tft.setTextDatum(MC_DATUM);
    tft.setTextColor(TFT_WHITE, 0x0113);
    tft.drawString("PromptPay", rx + boxW / 2, ry + 10, 2);

    // 116x116 High-contrast, pixel-perfect Flash ROM QR (Hardcoded, 0 bytes RAM)
    int qrX = rx + (boxW - PROMPTPAY_QR_WIDTH) / 2;
    int qrY = ry + 22;
    tft.setSwapBytes(true);
    tft.pushImage(qrX, qrY, PROMPTPAY_QR_WIDTH, PROMPTPAY_QR_HEIGHT, promptpay_qr_img);
    tft.setSwapBytes(false);

    // Bottom Card text
    tft.setTextDatum(MC_DATUM);
    tft.setTextColor(0x0113, TFT_WHITE);
    tft.drawString("Buy Me a Coffee", rx + boxW / 2, ry + 148, 2);
    tft.setTextColor(TFT_DARKGREY, TFT_WHITE);
    tft.drawString("สนับสนุนค่ากาแฟ", rx + boxW / 2, ry + 162, 1);
    tft.drawString("© 2026 Zotix", rx + boxW / 2, ry + 172, 1);

    // Bottom Navigation Bar
    tft.fillRect(0, DST_HEIGHT - 24, DST_WIDTH, 24, TFT_NAVY);
    tft.drawFastHLine(0, DST_HEIGHT - 24, DST_WIDTH, TFT_CYAN);
    tft.setTextDatum(MC_DATUM);
    tft.setTextColor(TFT_YELLOW, TFT_NAVY);
    tft.drawString("Tap Screen or Press BOOT to Return", DST_WIDTH / 2, DST_HEIGHT - 12, 2);

    // Wait for tap or button to exit
    waitTouchRelease();
    uint32_t startMs = millis();
    while (millis() - startMs < 60000UL) {
        GlobalBootAction act = checkGlobalBootButton([&]() {
            drawAboutScreen();
        });
        if (act == BOOT_ACT_SHORT_PRESS) {
            break;
        } else if (act != BOOT_ACT_NONE) {
            return;
        }

        int tx = 0, ty = 0;
        if (touch.readTouch(tx, ty, g_flipScreen, g_touchMapMode)) {
            waitTouchRelease();
            break;
        }
        delay(20);
    }
    drawMenuScreen();
}

static void drawMenuScreen() {
    g_screenHasRadar = false;
    tft.fillScreen(TFT_BLACK);

    // Header bar
    tft.fillRect(0, 0, DST_WIDTH, 28, TFT_NAVY);
    tft.setTextDatum(ML_DATUM);
    tft.setTextColor(TFT_YELLOW, TFT_NAVY);
    tft.drawString(g_sdReady ? " LyRadar [SD]" : " LyRadar", 4, 14, 2);
    drawMenuBtn(112, 2, 60, 24, "ABOUT", false, 0x1A47);
    drawMenuBtn(176, 2, 62, 24, "WIFI", false, 0x0339);
    drawMenuBtn(242, 2, 74, 24, "CLOSE X", false, TFT_MAROON);

    // Rows 1-3: 12 Radar Stations (3 rows of 4 columns)
    int yellowCount = 0;
    for (int i = 0; i < NUM_STATIONS; i++) {
        if (g_autoStationMask & (1 << i)) yellowCount++;
        int col = i & 3;
        int row = i / 4;
        int y   = (row == 0) ? MENU_R1_Y : ((row == 1) ? MENU_R2_Y : MENU_R3_Y);
        int h   = (row == 0) ? MENU_R1_H : ((row == 1) ? MENU_R2_H : MENU_R3_H);
        uint16_t activeBg = (i < 2) ? TFT_DARKGREEN : ((i < 4) ? 0x0339 : ((i < 8) ? 0x1A47 : 0x02CD));
        bool isYellow = (g_autoStationMask & (1 << i)) != 0;
        uint16_t border = isYellow ? TFT_YELLOW : (g_targetStation == i ? TFT_GREEN : TFT_CYAN);
        drawMenuBtn(4 + col * 79, y, MENU_COL4_W, h, kStations[i].shortCode,
                    g_targetStation == i, activeBg, border, isYellow);
    }

    // Row 4: AUTO / Timelapse / Brightness / Calibrate Touch / Update Now
    uint16_t autoBg     = g_autoCycle ? TFT_DARKGREEN : 0x7800; // Green when ON, Dark Red when OFF
    uint16_t autoBorder = g_autoCycle ? (yellowCount > 0 ? TFT_YELLOW : TFT_GREEN) : TFT_RED;

    char autoSub[16];
    if (!g_autoCycle) {
        snprintf(autoSub, sizeof(autoSub), "OFF");
    } else if (yellowCount > 0) {
        snprintf(autoSub, sizeof(autoSub), "%d", yellowCount);
    } else {
        snprintf(autoSub, sizeof(autoSub), "ALL");
    }
    drawMenuBtn2L(4 + 0 * 63, MENU_R4_Y, MENU_COL5_W, MENU_R4_H, "AUTO", autoSub, true, autoBg, autoBorder,
                  (g_autoCycle && yellowCount > 0) ? TFT_YELLOW : TFT_WHITE);

    drawMenuBtn(4 + 1 * 63, MENU_R4_Y, MENU_COL5_W, MENU_R4_H, "TLAPSE",   g_timelapseMode, TFT_MAGENTA);

    const char* brSub[4] = { "AUTO", "100%", "55%", "20%" };
    drawMenuBtn2L(4 + 2 * 63, MENU_R4_Y, MENU_COL5_W, MENU_R4_H, "BRI", brSub[g_brightMode & 3], false, 0x3186, TFT_CYAN, TFT_YELLOW);

    drawMenuBtn(4 + 3 * 63, MENU_R4_Y, MENU_COL5_W, MENU_R4_H, "CALIB",    false, 0x3186);
    drawMenuBtn(4 + 4 * 63, MENU_R4_Y, MENU_COL5_W, MENU_R4_H, "UPDATE",   false, TFT_DARKCYAN);
    sdLog("[MENU] LyRadar Menu opened (12 stations + Timelapse)\n");
}

static void selectZoomRegion(uint16_t zx, uint16_t zy) {
    uint8_t  sta = g_targetStation % NUM_STATIONS;
    uint16_t sw  = (sta == g_bankStation) ? g_srcWidth  : kStations[sta].defWidth;
    uint16_t sh  = (sta == g_bankStation) ? g_srcHeight : kStations[sta].defHeight;
    zx = (uint16_t)constrain((int)zx, 0, max(0, (int)sw - DST_WIDTH));
    zy = (uint16_t)constrain((int)zy, 0, max(0, (int)sh - DST_HEIGHT));
    bool changed = (zx != g_targetZoomX) || (zy != g_targetZoomY);
    g_targetZoomX = zx;
    g_targetZoomY = zy;
    updateRegionName(zx, zy);

    prefs.begin("radar_cfg", false);
    prefs.putUShort("zx", zx);
    prefs.putUShort("zy", zy);
    prefs.end();

    if (!g_zoomMode) {
        g_zoomMode = true;
    }
    if (changed || g_targetZoomX != g_bankZoomX || g_targetZoomY != g_bankZoomY) {
        g_forceRecrop = true;
        requestRadarUpdate();
    }
}

static void handleMenuTap(int tx, int ty) {
    // Visual touch dot so user immediately sees where the tap registered
    tft.fillCircle(tx, ty, 5, TFT_YELLOW);
    delay(60);

    // Top-right Close button (generous hitbox)
    if (ty < 33 && tx >= 242) {
        restoreRadarScreen();
        return;
    }

    // Top WiFi button (x=176..241, ty < 33): manually configure WiFi anytime
    if (ty < 33 && tx >= 176 && tx < 242) {
        runWiFiScannerAndKeyboard();
        restoreRadarScreen();
        return;
    }

    // Top About button (x=108..175, ty < 33): view About & PromptPay support
    if (ty < 33 && tx >= 108 && tx < 176) {
        drawAboutScreen();
        return;
    }

    // Rows 1-3: 12 Radar Stations (3 rows of 4 columns, y=30..164)
    if (ty >= MENU_R1_Y - 3 && ty < MENU_R4_Y - 4) {
        int row = (ty < MENU_R2_Y - 2) ? 0 : ((ty < MENU_R3_Y - 2) ? 1 : 2);
        int col = constrain((tx - 2) / 79, 0, 3);
        uint8_t newSta = (uint8_t)(row * 4 + col);
        if (newSta < NUM_STATIONS) {
            Serial.printf("[MENU] Tap at (x=%d, y=%d) -> station %u (%s)\n", tx, ty, newSta, kStations[newSta].shortCode);
            if (newSta != g_targetStation) {
                g_targetStation = newSta;
                uint16_t cx, cy;
                getStationDefaultZoom(newSta, kStations[newSta].defWidth, kStations[newSta].defHeight, cx, cy);
                g_targetZoomX = cx;
                g_targetZoomY = cy;
                updateRegionName(cx, cy);
                if (g_zoomMode) g_stepIdx = g_stepIdx * 2 + 1;
                g_zoomMode = false;
                prefs.begin("radar_cfg", false);
                prefs.putUChar("sta", newSta);
                prefs.putUShort("zx", cx);
                prefs.putUShort("zy", cy);
                prefs.end();
                g_forceRecrop = true;
                requestRadarUpdate();
            }
            restoreRadarScreen();
            if (g_targetStation != g_bankStation && g_activeBank >= 0) {
                tft.setTextDatum(MC_DATUM);
                tft.setTextColor(TFT_BLACK, TFT_YELLOW);
                char msg[40];
                snprintf(msg, sizeof(msg), "  Loading %s ...  ", kStations[g_targetStation].shortCode);
                tft.drawString(msg, DST_WIDTH / 2, DST_HEIGHT / 2, 4);
            }
        }
        return;
    }

    // Row 4: AUTO / TLAPSE / Brightness / Calibrate Touch / Update Now
    if (ty >= MENU_R4_Y - 4) {
        int col = constrain((tx - 2) / 63, 0, 4);
        if (col == 0) { // AUTO Toggle
            g_autoCycle = !g_autoCycle;
            prefs.begin("radar_cfg", false);
            prefs.putBool("auto", g_autoCycle);
            prefs.end();
            if (g_autoCycle) {
                if (g_zoomMode) {
                    g_stepIdx = g_stepIdx * 2 + 1;
                    g_zoomMode = false;
                }
                g_lastStationCycleMs = millis();
            }
            drawMenuScreen();
            return;
        } else if (col == 1) { // TLAPSE
            if (!g_sdReady) {
                tft.setTextDatum(MC_DATUM);
                tft.setTextColor(TFT_RED, TFT_BLACK);
                tft.drawString(" NO SD CARD! ", DST_WIDTH / 2, DST_HEIGHT / 2, 4);
                delay(800);
                drawMenuScreen();
                return;
            }
            buildTimelapseCatalog(g_targetStation);
            if (g_timelapseCount == 0) {
                tft.setTextDatum(MC_DATUM);
                tft.setTextColor(TFT_YELLOW, TFT_BLACK);
                tft.drawString(" No History Frames Yet ", DST_WIDTH / 2, DST_HEIGHT / 2, 4);
                delay(900);
                drawMenuScreen();
                return;
            }
            g_timelapseMode = true;
            g_timelapseIdx  = 0;
            g_menuOpen      = false;
            tft.fillScreen(TFT_BLACK);
            uint16_t tPal[256];
            loadTimelapseFrame(g_targetStation, 0, tPal, g_timelapseLabel, sizeof(g_timelapseLabel));
            drawTimelapseHud();
        } else if (col == 2) { // BRI
            g_brightMode = (g_brightMode + 1) & 3;
            applyBacklight();
            prefs.begin("radar_cfg", false);
            prefs.putUChar("bri", g_brightMode);
            prefs.end();
            drawMenuScreen();
        } else if (col == 3) { // CALIB
            runTouchCalibration();
            drawMenuScreen();
        } else { // UPDATE
            if (g_timelapseMode) {
                g_timelapseMode = false;
                freeTimelapseCatalog();
            }
            g_forceRecrop = true;
            requestRadarUpdate();
            restoreRadarScreen();
        }
        return;
    }

    // Redraw menu to clear the yellow touch dot if tap landed between buttons
    drawMenuScreen();
}

// ============================================================================
// Captive Portal & WiFi Connection
// ============================================================================
static void runCaptivePortalSetup() {
    WiFi.mode(WIFI_AP);
    WiFi.softAP(CAPTIVE_PORTAL_SSID);
    IPAddress apIP = WiFi.softAPIP();

    DNSServer dnsServer;
    dnsServer.start(53, "*", apIP);

    WebServer server(80);
    bool configured = false;

    server.on("/", HTTP_GET, [&server]() {
        String html =
            "<!DOCTYPE html><html><head><meta charset='utf-8'>"
            "<meta name='viewport' content='width=device-width,initial-scale=1'>"
            "<title>ตั้งค่า WiFi LyRadar</title>"
            "<style>body{font-family:sans-serif;background:#111827;color:#f9fafb;padding:20px;text-align:center;}"
            ".card{max-width:360px;margin:auto;background:#1f2937;padding:24px;border-radius:12px;}"
            "input{width:100%;padding:10px;margin:8px 0;border-radius:6px;border:1px solid #374151;box-sizing:border-box;}"
            "button{width:100%;padding:12px;background:#10b981;color:#fff;border:none;border-radius:6px;font-size:16px;font-weight:bold;}</style>"
            "</head><body><div class='card'><h2>🌧️ LyRadar Weather Display</h2>"
            "<p>ตั้งค่า WiFi สำหรับบอร์ด CYD (LyRadar)</p>"
            "<form method='POST' action='/save'>"
            "<input name='s' placeholder='ชื่อ WiFi (SSID)' required>"
            "<input name='p' type='password' placeholder='รหัสผ่าน WiFi'>"
            "<button type='submit'>บันทึกและเชื่อมต่อ</button></form></div></body></html>";
        server.send(200, "text/html", html);
    });

    server.on("/save", HTTP_POST, [&server, &configured]() {
        String ssid = server.arg("s");
        String pass = server.arg("p");
        if (ssid.length() > 0) {
            prefs.begin("radar_cfg", false);
            prefs.putString("ssid", ssid);
            prefs.putString("pass", pass);
            prefs.end();
            server.send(200, "text/html",
                        "<meta charset='utf-8'><h2 style='font-family:sans-serif;text-align:center;'>"
                        "บันทึกสำเร็จ! บอร์ดกำลังรีสตาร์ท...</h2>");
            delay(1000);
            configured = true;
        }
    });

    server.onNotFound([&server, apIP]() {
        server.sendHeader("Location", String("http://") + apIP.toString(), true);
        server.send(302, "text/plain", "");
    });

    server.begin();

    auto redrawPortal = [&]() {
        tft.fillScreen(TFT_BLACK);
        tft.setTextColor(TFT_CYAN, TFT_BLACK);
        tft.setTextDatum(MC_DATUM);
        tft.drawString("WiFi Setup Mode", DST_WIDTH / 2, 40, 4);
        tft.setTextColor(TFT_WHITE, TFT_BLACK);
        tft.drawString("1. Connect phone WiFi to:", DST_WIDTH / 2, 95, 2);
        tft.setTextColor(TFT_YELLOW, TFT_BLACK);
        tft.drawString(CAPTIVE_PORTAL_SSID, DST_WIDTH / 2, 125, 4);
        tft.setTextColor(TFT_WHITE, TFT_BLACK);
        tft.drawString("2. Open browser at:", DST_WIDTH / 2, 170, 2);
        tft.setTextColor(TFT_GREEN, TFT_BLACK);
        tft.drawString(apIP.toString(), DST_WIDTH / 2, 200, 4);
    };

    redrawPortal();

    while (!configured) {
        checkGlobalBootButton(redrawPortal);
        dnsServer.processNextRequest();
        server.handleClient();
        delay(5);
    }
    ESP.restart();
}

static bool tryConnect(const String& ssid, const String& pass) {
    tft.setTextDatum(BC_DATUM);
    tft.setTextColor(TFT_YELLOW, TFT_BLACK);
    tft.drawString("  Connecting WiFi: " + ssid + "...  ", DST_WIDTH / 2, DST_HEIGHT - 6, 2);
    Serial.printf("[WIFI] Connecting to '%s'\n", ssid.c_str());

    WiFi.mode(WIFI_STA);
    WiFi.setSleep(false);
    WiFi.begin(ssid.c_str(), pass.c_str());
    uint32_t start = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - start < 15000) delay(250);
    if (WiFi.status() == WL_CONNECTED) {
        WiFi.setSleep(true);
        esp_wifi_set_ps(WIFI_PS_MIN_MODEM);
        return true;
    }
    WiFi.disconnect();
    return false;
}

// ============================================================================
// On-Screen Touch Keyboard (5 Rows with Dedicated Number Row 0..9)
// ============================================================================
static const char kRow0[10]       = { '1','2','3','4','5','6','7','8','9','0' };
static const char kRow1_Lower[10] = { 'q','w','e','r','t','y','u','i','o','p' };
static const char kRow1_Upper[10] = { 'Q','W','E','R','T','Y','U','I','O','P' };
static const char kRow1_Sym[10]   = { '!','@','#','$','%','^','&','*','(',')' };
static const char kRow2_Lower[9]  = { 'a','s','d','f','g','h','j','k','l' };
static const char kRow2_Upper[9]  = { 'A','S','D','F','G','H','J','K','L' };
static const char kRow2_Sym[9]    = { '~','`','+','=','{','}','[',']','\\' };
static const char kRow3_Lower[7]  = { 'z','x','c','v','b','n','m' };
static const char kRow3_Upper[7]  = { 'Z','X','C','V','B','N','M' };
static const char kRow3_Sym[7]    = { ':',';','"','\'','<','>','?' };

static void drawKeyBtn(int x, int y, int w, int h, const char* label, uint16_t bg = 0x2124, uint16_t border = TFT_CYAN, uint16_t txtColor = TFT_WHITE) {
    tft.fillRoundRect(x, y, w, h, 3, bg);
    tft.drawRoundRect(x, y, w, h, 3, border);
    tft.setTextDatum(MC_DATUM);
    tft.setTextColor(txtColor, bg);
    tft.drawString(label, x + w / 2, y + h / 2, 2);
}

static void flashKey(int x, int y, int w, int h) {
    tft.fillRoundRect(x, y, w, h, 3, TFT_WHITE);
    delay(45);
}

static void drawKeyboardInputBox(const String& pass, bool showPass) {
    tft.fillRoundRect(4, 24, 224, 21, 3, TFT_BLACK);
    tft.drawRoundRect(4, 24, 224, 21, 3, TFT_CYAN);
    tft.setTextDatum(ML_DATUM);
    tft.setTextColor(TFT_WHITE, TFT_BLACK);

    String disp = "";
    if (showPass) {
        disp = pass;
    } else {
        disp.reserve(pass.length() + 2);
        for (size_t i = 0; i < pass.length(); i++) disp += '*';
    }
    disp += '_';
    if (disp.length() > 22) {
        disp = disp.substring(disp.length() - 22);
    }
    tft.drawString(disp, 8, 34, 2);
}

static void drawFullKeyboard(const String& ssid, const String& pass, bool showPass, bool shiftMode, bool symMode) {
    tft.fillScreen(TFT_BLACK);

    // Line 1: Header (SSID name + SHOW/HIDE + CANCEL)
    tft.setTextDatum(ML_DATUM);
    tft.setTextColor(TFT_CYAN, TFT_BLACK);
    char ssidBuf[24];
    snprintf(ssidBuf, sizeof(ssidBuf), "SSID: %.15s", ssid.c_str());
    tft.drawString(ssidBuf, 4, 11, 2);

    drawKeyBtn(194, 2, 58, 20, showPass ? "HIDE" : "SHOW", 0x18E3, TFT_CYAN, TFT_YELLOW);
    drawKeyBtn(258, 2, 58, 20, "BACK", 0x4208, TFT_RED, TFT_WHITE);

    // Line 2: Input box + CONNECT button
    drawKeyboardInputBox(pass, showPass);
    drawKeyBtn(232, 24, 84, 21, "CONNECT", TFT_DARKGREEN, TFT_GREEN, TFT_WHITE);

    // Row 0: Numbers 0..9 (y=48..82, h=34)
    for (int i = 0; i < 10; i++) {
        char kStr[2] = { kRow0[i], '\0' };
        drawKeyBtn(1 + i * 32, 48, 30, 34, kStr, 0x18C6, TFT_CYAN, TFT_YELLOW);
    }

    // Row 1: QWERTY (y=85..119, h=34)
    for (int i = 0; i < 10; i++) {
        char c = symMode ? kRow1_Sym[i] : (shiftMode ? kRow1_Upper[i] : kRow1_Lower[i]);
        char kStr[2] = { c, '\0' };
        drawKeyBtn(1 + i * 32, 85, 30, 34, kStr, 0x2124, TFT_DARKCYAN, TFT_WHITE);
    }

    // Row 2: ASDF (y=122..156, h=34)
    for (int i = 0; i < 9; i++) {
        char c = symMode ? kRow2_Sym[i] : (shiftMode ? kRow2_Upper[i] : kRow2_Lower[i]);
        char kStr[2] = { c, '\0' };
        drawKeyBtn(8 + i * 34, 122, 32, 34, kStr, 0x2124, TFT_DARKCYAN, TFT_WHITE);
    }

    // Row 3: ZXCV + SHIFT + DEL (y=159..193, h=34)
    drawKeyBtn(2, 159, 44, 34, "SHIFT", shiftMode ? TFT_YELLOW : 0x2124, TFT_CYAN, shiftMode ? TFT_BLACK : TFT_WHITE);
    for (int i = 0; i < 7; i++) {
        char c = symMode ? kRow3_Sym[i] : (shiftMode ? kRow3_Upper[i] : kRow3_Lower[i]);
        char kStr[2] = { c, '\0' };
        drawKeyBtn(49 + i * 31, 159, 29, 34, kStr, 0x2124, TFT_DARKCYAN, TFT_WHITE);
    }
    drawKeyBtn(266, 159, 52, 34, "DEL", 0x5000, TFT_RED, TFT_WHITE);

    // Row 4: Symbols / Actions (y=196..234, h=38)
    drawKeyBtn(2, 196, 50, 38, symMode ? "ABC" : "SYM", symMode ? TFT_CYAN : 0x2124, TFT_CYAN, symMode ? TFT_BLACK : TFT_WHITE);
    drawKeyBtn(55, 196, 28, 38, ".", 0x2124, TFT_DARKCYAN, TFT_WHITE);
    drawKeyBtn(85, 196, 28, 38, "-", 0x2124, TFT_DARKCYAN, TFT_WHITE);
    drawKeyBtn(115, 196, 28, 38, "_", 0x2124, TFT_DARKCYAN, TFT_WHITE);
    drawKeyBtn(145, 196, 28, 38, "@", 0x2124, TFT_DARKCYAN, TFT_WHITE);
    drawKeyBtn(176, 196, 78, 38, "SPACE", 0x2124, TFT_DARKCYAN, TFT_WHITE);
    drawKeyBtn(257, 196, 60, 38, "CLEAR", 0x39C7, TFT_LIGHTGREY, TFT_WHITE);
}

static bool runTouchKeyboard(const String& ssid, String& pass) {
    bool showPass = false;
    bool shiftMode = false;
    bool symMode = false;

    drawFullKeyboard(ssid, pass, showPass, shiftMode, symMode);
    waitTouchRelease();

    while (true) {
        GlobalBootAction act = checkGlobalBootButton([&]() {
            drawFullKeyboard(ssid, pass, showPass, shiftMode, symMode);
        });
        if (act == BOOT_ACT_SHORT_PRESS) {
            return false; // Short press BOOT = Cancel / Back to scan list
        } else if (act != BOOT_ACT_NONE) {
            continue;
        }

        int tx = 0, ty = 0;
        if (touch.readTouch(tx, ty, g_flipScreen, g_touchMapMode)) {
            // 1. Top Controls (y < 24)
            if (ty < 24) {
                if (tx >= 194 && tx < 254) { // SHOW / HIDE
                    flashKey(194, 2, 58, 20);
                    showPass = !showPass;
                    waitTouchRelease();
                    drawFullKeyboard(ssid, pass, showPass, shiftMode, symMode);
                    continue;
                }
                if (tx >= 256) { // BACK / CANCEL
                    flashKey(258, 2, 58, 20);
                    waitTouchRelease();
                    return false;
                }
            }

            // 2. Input Box & CONNECT (y >= 24 && ty < 48)
            if (ty >= 24 && ty < 48) {
                if (tx >= 230) { // CONNECT
                    flashKey(232, 24, 84, 21);
                    waitTouchRelease();
                    return true;
                }
            }

            // 3. Row 0: Numbers (y >= 48 && ty < 84)
            if (ty >= 48 && ty < 84) {
                int col = (tx - 1) / 32;
                if (col >= 0 && col < 10) {
                    int kx = 1 + col * 32;
                    flashKey(kx, 48, 30, 34);
                    pass += kRow0[col];
                    waitTouchRelease();
                    char kStr[2] = { kRow0[col], '\0' };
                    drawKeyBtn(kx, 48, 30, 34, kStr, 0x18C6, TFT_CYAN, TFT_YELLOW);
                    drawKeyboardInputBox(pass, showPass);
                    continue;
                }
            }

            // 4. Row 1: QWERTY (y >= 85 && ty < 121)
            if (ty >= 85 && ty < 121) {
                int col = (tx - 1) / 32;
                if (col >= 0 && col < 10) {
                    char c = symMode ? kRow1_Sym[col] : (shiftMode ? kRow1_Upper[col] : kRow1_Lower[col]);
                    int kx = 1 + col * 32;
                    flashKey(kx, 85, 30, 34);
                    pass += c;
                    bool hadShift = shiftMode;
                    if (shiftMode) shiftMode = false;
                    waitTouchRelease();
                    if (hadShift) {
                        drawFullKeyboard(ssid, pass, showPass, shiftMode, symMode);
                    } else {
                        char kStr[2] = { c, '\0' };
                        drawKeyBtn(kx, 85, 30, 34, kStr, 0x2124, TFT_DARKCYAN, TFT_WHITE);
                        drawKeyboardInputBox(pass, showPass);
                    }
                    continue;
                }
            }

            // 5. Row 2: ASDF (y >= 122 && ty < 158)
            if (ty >= 122 && ty < 158) {
                int col = (tx - 8) / 34;
                if (col >= 0 && col < 9) {
                    char c = symMode ? kRow2_Sym[col] : (shiftMode ? kRow2_Upper[col] : kRow2_Lower[col]);
                    int kx = 8 + col * 34;
                    flashKey(kx, 122, 32, 34);
                    pass += c;
                    bool hadShift = shiftMode;
                    if (shiftMode) shiftMode = false;
                    waitTouchRelease();
                    if (hadShift) {
                        drawFullKeyboard(ssid, pass, showPass, shiftMode, symMode);
                    } else {
                        char kStr[2] = { c, '\0' };
                        drawKeyBtn(kx, 122, 32, 34, kStr, 0x2124, TFT_DARKCYAN, TFT_WHITE);
                        drawKeyboardInputBox(pass, showPass);
                    }
                    continue;
                }
            }

            // 6. Row 3: ZXCV + SHIFT + DEL (y >= 159 && ty < 195)
            if (ty >= 159 && ty < 195) {
                if (tx < 48) { // SHIFT
                    flashKey(2, 159, 44, 34);
                    shiftMode = !shiftMode;
                    waitTouchRelease();
                    drawFullKeyboard(ssid, pass, showPass, shiftMode, symMode);
                    continue;
                }
                if (tx >= 265) { // DEL
                    flashKey(266, 159, 52, 34);
                    if (pass.length() > 0) pass.remove(pass.length() - 1);
                    waitTouchRelease();
                    drawKeyBtn(266, 159, 52, 34, "DEL", 0x5000, TFT_RED, TFT_WHITE);
                    drawKeyboardInputBox(pass, showPass);
                    continue;
                }
                int col = (tx - 49) / 31;
                if (col >= 0 && col < 7) {
                    char c = symMode ? kRow3_Sym[col] : (shiftMode ? kRow3_Upper[col] : kRow3_Lower[col]);
                    int kx = 49 + col * 31;
                    flashKey(kx, 159, 29, 34);
                    pass += c;
                    bool hadShift = shiftMode;
                    if (shiftMode) shiftMode = false;
                    waitTouchRelease();
                    if (hadShift) {
                        drawFullKeyboard(ssid, pass, showPass, shiftMode, symMode);
                    } else {
                        char kStr[2] = { c, '\0' };
                        drawKeyBtn(kx, 159, 29, 34, kStr, 0x2124, TFT_DARKCYAN, TFT_WHITE);
                        drawKeyboardInputBox(pass, showPass);
                    }
                    continue;
                }
            }

            // 7. Row 4: SYM, ., -, _, @, SPACE, CLEAR (y >= 196 && ty <= 238)
            if (ty >= 196 && ty <= 238) {
                if (tx < 54) { // SYM / ABC
                    flashKey(2, 196, 50, 38);
                    symMode = !symMode;
                    waitTouchRelease();
                    drawFullKeyboard(ssid, pass, showPass, shiftMode, symMode);
                    continue;
                }
                if (tx >= 55 && tx < 83) { // .
                    flashKey(55, 196, 28, 38);
                    pass += '.';
                    waitTouchRelease();
                    drawKeyBtn(55, 196, 28, 38, ".", 0x2124, TFT_DARKCYAN, TFT_WHITE);
                    drawKeyboardInputBox(pass, showPass);
                    continue;
                }
                if (tx >= 85 && tx < 113) { // -
                    flashKey(85, 196, 28, 38);
                    pass += '-';
                    waitTouchRelease();
                    drawKeyBtn(85, 196, 28, 38, "-", 0x2124, TFT_DARKCYAN, TFT_WHITE);
                    drawKeyboardInputBox(pass, showPass);
                    continue;
                }
                if (tx >= 115 && tx < 143) { // _
                    flashKey(115, 196, 28, 38);
                    pass += '_';
                    waitTouchRelease();
                    drawKeyBtn(115, 196, 28, 38, "_", 0x2124, TFT_DARKCYAN, TFT_WHITE);
                    drawKeyboardInputBox(pass, showPass);
                    continue;
                }
                if (tx >= 145 && tx < 174) { // @
                    flashKey(145, 196, 28, 38);
                    pass += '@';
                    waitTouchRelease();
                    drawKeyBtn(145, 196, 28, 38, "@", 0x2124, TFT_DARKCYAN, TFT_WHITE);
                    drawKeyboardInputBox(pass, showPass);
                    continue;
                }
                if (tx >= 175 && tx < 255) { // SPACE
                    flashKey(176, 196, 78, 38);
                    pass += ' ';
                    waitTouchRelease();
                    drawKeyBtn(176, 196, 78, 38, "SPACE", 0x2124, TFT_DARKCYAN, TFT_WHITE);
                    drawKeyboardInputBox(pass, showPass);
                    continue;
                }
                if (tx >= 256) { // CLEAR
                    flashKey(257, 196, 60, 38);
                    pass = "";
                    waitTouchRelease();
                    drawKeyBtn(257, 196, 60, 38, "CLEAR", 0x39C7, TFT_LIGHTGREY, TFT_WHITE);
                    drawKeyboardInputBox(pass, showPass);
                    continue;
                }
            }
        }
        delay(15);
    }
}

// ============================================================================
// On-Screen WiFi Scanner (SSID Picker)
// ============================================================================
struct ScannedSsid {
    char ssid[33];
    int32_t rssi;
    bool isEncrypted;
};

static void drawScanPage(const ScannedSsid* nets, int netCount, int currentPage, int totalPages) {
    tft.fillScreen(TFT_BLACK);

    // Header
    tft.fillRect(0, 0, DST_WIDTH, 28, TFT_NAVY);
    tft.setTextDatum(ML_DATUM);
    tft.setTextColor(TFT_YELLOW, TFT_NAVY);
    tft.drawString("Select WiFi Network", 6, 14, 2);
    drawKeyBtn(196, 2, 58, 24, "RESCAN", TFT_DARKCYAN, TFT_CYAN, TFT_WHITE);
    drawKeyBtn(258, 2, 58, 24, "AP MODE", 0x4208, TFT_RED, TFT_WHITE);

    if (netCount == 0) {
        tft.setTextDatum(MC_DATUM);
        tft.setTextColor(TFT_WHITE, TFT_BLACK);
        tft.drawString("No 2.4 GHz WiFi networks found!", DST_WIDTH / 2, 100, 2);
        tft.setTextColor(TFT_YELLOW, TFT_BLACK);
        tft.drawString("Tap RESCAN or use AP MODE", DST_WIDTH / 2, 130, 2);
        return;
    }

    // Items (4 per page)
    for (int i = 0; i < 4; i++) {
        int idx = currentPage * 4 + i;
        if (idx >= netCount) break;
        int iy = 32 + i * 42;

        tft.fillRoundRect(4, iy, 312, 38, 4, 0x18E3);
        tft.drawRoundRect(4, iy, 312, 38, 4, TFT_CYAN);

        // Signal strength badge
        int pct = constrain(2 * (nets[idx].rssi + 100), 0, 100);
        char sigBuf[12];
        snprintf(sigBuf, sizeof(sigBuf), "%d%%", pct);
        uint16_t sigColor = (pct >= 60) ? TFT_GREEN : (pct >= 35 ? TFT_YELLOW : TFT_ORANGE);
        tft.fillRoundRect(8, iy + 4, 38, 30, 3, 0x0841);
        tft.setTextDatum(MC_DATUM);
        tft.setTextColor(sigColor, 0x0841);
        tft.drawString(sigBuf, 27, iy + 19, 2);

        // SSID Name
        tft.setTextDatum(ML_DATUM);
        tft.setTextColor(TFT_WHITE, 0x18E3);
        char nameBuf[24];
        strncpy(nameBuf, nets[idx].ssid, 20);
        nameBuf[20] = '\0';
        tft.drawString(nameBuf, 52, iy + 19, 2);

        // Lock / Open status
        tft.setTextDatum(MR_DATUM);
        if (nets[idx].isEncrypted) {
            tft.setTextColor(TFT_YELLOW, 0x18E3);
            tft.drawString("[SEC]", 308, iy + 19, 2);
        } else {
            tft.setTextColor(TFT_GREEN, 0x18E3);
            tft.drawString("[OPEN]", 308, iy + 19, 2);
        }
    }

    // Footer
    if (currentPage > 0) {
        drawKeyBtn(4, 204, 76, 32, "< PREV", 0x2124, TFT_CYAN, TFT_WHITE);
    }

    tft.setTextDatum(MC_DATUM);
    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    char pageBuf[32];
    snprintf(pageBuf, sizeof(pageBuf), "Page %d/%d (%d found)", currentPage + 1, totalPages, netCount);
    tft.drawString(pageBuf, DST_WIDTH / 2, 220, 2);

    if (currentPage < totalPages - 1) {
        drawKeyBtn(240, 204, 76, 32, "NEXT >", 0x2124, TFT_CYAN, TFT_WHITE);
    }
}

static bool runWiFiScannerAndKeyboard() {
    tft.fillScreen(TFT_BLACK);
    tft.setTextDatum(MC_DATUM);
    tft.setTextColor(TFT_CYAN, TFT_BLACK);
    tft.drawString("Scanning WiFi Networks...", DST_WIDTH / 2, DST_HEIGHT / 2 - 20, 4);
    tft.setTextColor(TFT_YELLOW, TFT_BLACK);
    tft.drawString("Scanning 2.4 GHz channels...", DST_WIDTH / 2, DST_HEIGHT / 2 + 15, 2);

    WiFi.mode(WIFI_STA);
    WiFi.disconnect();
    delay(100);

    ScannedSsid nets[24];
    int netCount = 0;

    auto doScan = [&]() {
        netCount = 0;
        int n = WiFi.scanNetworks(false, false);
        for (int i = 0; i < n && netCount < 24; i++) {
            String s = WiFi.SSID(i);
            if (s.length() == 0) continue;
            bool dup = false;
            for (int d = 0; d < netCount; d++) {
                if (strcmp(nets[d].ssid, s.c_str()) == 0) {
                    dup = true;
                    if (WiFi.RSSI(i) > nets[d].rssi) nets[d].rssi = WiFi.RSSI(i);
                    break;
                }
            }
            if (dup) continue;
            strncpy(nets[netCount].ssid, s.c_str(), 32);
            nets[netCount].ssid[32] = '\0';
            nets[netCount].rssi = WiFi.RSSI(i);
            nets[netCount].isEncrypted = (WiFi.encryptionType(i) != WIFI_AUTH_OPEN);
            netCount++;
        }
        WiFi.scanDelete();

        // Sort descending by RSSI
        for (int i = 0; i < netCount - 1; i++) {
            for (int j = i + 1; j < netCount; j++) {
                if (nets[j].rssi > nets[i].rssi) {
                    ScannedSsid tmp = nets[i];
                    nets[i] = nets[j];
                    nets[j] = tmp;
                }
            }
        }
    };

    doScan();
    int currentPage = 0;
    int totalPages = max(1, (netCount + 3) / 4);
    drawScanPage(nets, netCount, currentPage, totalPages);
    waitTouchRelease();

    String enteredPass = "";

    while (true) {
        GlobalBootAction act = checkGlobalBootButton([&]() {
            drawScanPage(nets, netCount, currentPage, totalPages);
        });
        if (act == BOOT_ACT_SHORT_PRESS) {
            return false; // Short press BOOT = Cancel / Back to menu
        } else if (act != BOOT_ACT_NONE) {
            continue;
        }

        int tx = 0, ty = 0;
        if (touch.readTouch(tx, ty, g_flipScreen, g_touchMapMode)) {
            // Header buttons
            if (ty < 30) {
                if (tx >= 194 && tx < 254) { // RESCAN
                    flashKey(196, 2, 58, 24);
                    waitTouchRelease();
                    tft.fillScreen(TFT_BLACK);
                    tft.setTextDatum(MC_DATUM);
                    tft.setTextColor(TFT_CYAN, TFT_BLACK);
                    tft.drawString("Rescanning WiFi...", DST_WIDTH / 2, DST_HEIGHT / 2, 4);
                    doScan();
                    currentPage = 0;
                    totalPages = max(1, (netCount + 3) / 4);
                    drawScanPage(nets, netCount, currentPage, totalPages);
                    waitTouchRelease();
                    continue;
                }
                if (tx >= 256) { // AP MODE
                    flashKey(258, 2, 58, 24);
                    waitTouchRelease();
                    runCaptivePortalSetup();
                    return false;
                }
            }

            // Footer buttons (PREV / NEXT)
            if (ty >= 200) {
                if (tx < 85 && currentPage > 0) { // PREV
                    flashKey(4, 204, 76, 32);
                    currentPage--;
                    drawScanPage(nets, netCount, currentPage, totalPages);
                    waitTouchRelease();
                    continue;
                }
                if (tx >= 235 && currentPage < totalPages - 1) { // NEXT
                    flashKey(240, 204, 76, 32);
                    currentPage++;
                    drawScanPage(nets, netCount, currentPage, totalPages);
                    waitTouchRelease();
                    continue;
                }
            }

            // Item rows (y=32..200)
            if (ty >= 32 && ty < 200) {
                int itemIdx = (ty - 32) / 42;
                int selected = currentPage * 4 + itemIdx;
                if (selected < netCount) {
                    int iy = 32 + itemIdx * 42;
                    tft.fillRoundRect(4, iy, 312, 38, 4, TFT_DARKGREEN);
                    tft.drawRoundRect(4, iy, 312, 38, 4, TFT_GREEN);
                    delay(80);
                    waitTouchRelease();

                    String chosenSsid = nets[selected].ssid;
                    bool connected = false;

                    while (!connected) {
                        bool confirmed = true;
                        if (nets[selected].isEncrypted) {
                            confirmed = runTouchKeyboard(chosenSsid, enteredPass);
                        } else {
                            enteredPass = "";
                        }
                        if (!confirmed) {
                            break; // User cancelled
                        }

                        // Attempt connection
                        tft.fillScreen(TFT_BLACK);
                        tft.setTextDatum(MC_DATUM);
                        tft.setTextColor(TFT_CYAN, TFT_BLACK);
                        tft.drawString("Connecting to WiFi...", DST_WIDTH / 2, DST_HEIGHT / 2 - 25, 4);
                        tft.setTextColor(TFT_WHITE, TFT_BLACK);
                        tft.drawString(chosenSsid, DST_WIDTH / 2, DST_HEIGHT / 2 + 5, 2);
                        tft.setTextColor(TFT_YELLOW, TFT_BLACK);
                        tft.drawString("Please wait...", DST_WIDTH / 2, DST_HEIGHT / 2 + 30, 2);

                        if (tryConnect(chosenSsid, enteredPass)) {
                            connected = true;
                            tft.fillScreen(TFT_BLACK);
                            tft.setTextDatum(MC_DATUM);
                            tft.setTextColor(TFT_GREEN, TFT_BLACK);
                            tft.drawString("Connected Successfully!", DST_WIDTH / 2, DST_HEIGHT / 2 - 15, 4);
                            tft.setTextColor(TFT_WHITE, TFT_BLACK);
                            tft.drawString("IP: " + WiFi.localIP().toString(), DST_WIDTH / 2, DST_HEIGHT / 2 + 15, 2);

                            prefs.begin("radar_cfg", false);
                            prefs.putString("ssid", chosenSsid);
                            prefs.putString("pass", enteredPass);
                            prefs.end();

                            sdLog("[WIFI] Connected via on-screen scanner: %s (IP: %s)\n", chosenSsid.c_str(), WiFi.localIP().toString().c_str());
                            delay(1500);
                            return true;
                        } else {
                            tft.fillScreen(TFT_BLACK);
                            tft.setTextDatum(MC_DATUM);
                            tft.setTextColor(TFT_RED, TFT_BLACK);
                            tft.drawString("Connection Failed!", DST_WIDTH / 2, DST_HEIGHT / 2 - 15, 4);
                            tft.setTextColor(TFT_YELLOW, TFT_BLACK);
                            tft.drawString("Wrong Password or AP Unreachable", DST_WIDTH / 2, DST_HEIGHT / 2 + 15, 2);
                            delay(2000);
                            if (!nets[selected].isEncrypted) break;
                        }
                    }

                    drawScanPage(nets, netCount, currentPage, totalPages);
                    waitTouchRelease();
                }
            }
        }
        delay(15);
    }
}

static void connectWiFi() {
    // SD-3: Check for /ly-radar/wifi.txt on SD card first
    String sdSsid, sdPass;
    checkSdWiFiConfig(sdSsid, sdPass);

    prefs.begin("radar_cfg", true);
    String savedSsid = prefs.getString("ssid", "");
    String savedPass = prefs.getString("pass", "");
    prefs.end();

    String cfgSsid = WIFI_SSID;
    bool   hasCfg  = cfgSsid.length() > 0 && cfgSsid != "YOUR_WIFI_SSID";

    bool connected = false;
    if (savedSsid.length() > 0) connected = tryConnect(savedSsid, savedPass);
    if (!connected && hasCfg && (cfgSsid != savedSsid || String(WIFI_PASSWORD) != savedPass)) {
        connected = tryConnect(cfgSsid, WIFI_PASSWORD);
    }

    if (connected) {
        sdLog("[WIFI] Connected OK, IP: %s\n", WiFi.localIP().toString().c_str());
        configTime(7 * 3600, 0, "pool.ntp.org", "time.navy.mi.th", "time.google.com");
        WiFi.setAutoReconnect(true);
        WiFi.setSleep(true);
        esp_wifi_set_ps(WIFI_PS_MIN_MODEM);
    } else {
        sdLog("[WIFI] Connection failed, launching on-screen WiFi Scanner\n");
        bool ok = runWiFiScannerAndKeyboard();
        if (ok) {
            sdLog("[WIFI] Connected OK via Scanner, IP: %s\n", WiFi.localIP().toString().c_str());
            configTime(7 * 3600, 0, "pool.ntp.org", "time.navy.mi.th", "time.google.com");
            WiFi.setAutoReconnect(true);
            WiFi.setSleep(true);
            esp_wifi_set_ps(WIFI_PS_MIN_MODEM);
        } else if (g_activeBank < 0) {
            sdLog("[WIFI] Scanner cancelled, launching Captive Portal fallback: %s\n", CAPTIVE_PORTAL_SSID);
            runCaptivePortalSetup();
        }
    }
}

