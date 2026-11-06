#include <Arduino.h>
#include <SPI.h>
#include "soc/soc.h"
#include "soc/rtc_cntl_reg.h"
#include "config.h"
#include "radar_types.h"
#include "cyd_touch.h"
#include "radar_cache.h"
#include "gif_decoder.h"
#include "radar_ui.h"

// loopTask (Core 1) renders the display, handles touch and runs the captive portal.
// TLS + HTTP + GIF decoding run in downloadTask on Core 0.
SET_LOOP_TASK_STACK_SIZE(8 * 1024);

// ============================================================================
// Touch & Button Input Handler
// ============================================================================
static void handleUserInput() {
    // 1. Physical BOOT Button (GPIO 0): Universal Handler
    if (digitalRead(BOOT_BTN_PIN) == LOW && g_standbyMode) {
        g_standbyMode = false;
        applyBacklight();
        g_lastTouchMs = millis();
        g_lastStationCycleMs = millis();
        while (digitalRead(BOOT_BTN_PIN) == LOW) delay(15);
        return;
    }

    GlobalBootAction bootAct = checkGlobalBootButton([&]() {
        if (g_menuOpen) drawMenuScreen();
        else            restoreRadarScreen();
    });

    if (bootAct == BOOT_ACT_SHORT_PRESS) {
        // Released < 3s -> If menu or timelapse is open, close it; otherwise Rotate screen 0° / 180°
        if (g_menuOpen || g_timelapseMode) {
            if (g_timelapseMode) {
                g_timelapseMode = false;
                freeTimelapseCatalog();
            }
            restoreRadarScreen();
            return;
        }
        g_flipScreen = !g_flipScreen;
        tft.setRotation(g_flipScreen ? 3 : 1);
        prefs.begin("radar_cfg", false);
        prefs.putBool("flip", g_flipScreen);
        prefs.end();
        restoreRadarScreen();
        return;
    } else if (bootAct != BOOT_ACT_NONE) {
        return;
    }

    // 2. XPT2046 Resistive Touch Panel
    int tx = 0, ty = 0;
    if (touch.readTouch(tx, ty, g_flipScreen, g_touchMapMode)) {
        if (g_standbyMode) {
            g_standbyMode = false;
            applyBacklight();
            g_lastTouchMs = millis();
            g_lastStationCycleMs = millis();
            while (touch.isTouching()) delay(15);
            return;
        }
        if (millis() - g_lastTouchMs < 260) return;

        // Lock initial firm touch coordinates (tx, ty) so finger lift-off noise never shifts the tap!
        uint32_t downMs    = millis();
        uint32_t lastSeen  = downMs;
        bool     longFired = false;

        while (millis() - downMs < 2000) {
            if (touch.isTouching()) {
                lastSeen = millis();
                if (!longFired && (millis() - downMs >= 380)) {
                    longFired  = true;
                    if (g_menuOpen && ty >= MENU_R1_Y - 3 && ty < MENU_R4_Y - 4) {
                        int row = (ty < MENU_R2_Y - 2) ? 0 : ((ty < MENU_R3_Y - 2) ? 1 : 2);
                        int col = constrain((tx - 2) / 79, 0, 3);
                        uint8_t targetSta = (uint8_t)(row * 4 + col);
                        if (targetSta < NUM_STATIONS) {
                            g_autoStationMask ^= (1 << targetSta);
                            prefs.begin("radar_cfg", false);
                            prefs.putUShort("sta_mask", g_autoStationMask);
                            prefs.end();
                            drawMenuScreen();
                        }
                    } else {
                        g_menuOpen = !g_menuOpen;
                        if (g_menuOpen) drawMenuScreen();
                        else            restoreRadarScreen();
                    }
                    break;
                }
            } else if (millis() - lastSeen > 70) {
                break;
            }
            delay(10);
        }

        // If long-press triggered Menu toggle, wait briefly for finger release so it doesn't tap a button
        if (longFired) {
            uint32_t relWait = millis();
            while (touch.isTouching() && (millis() - relWait < 1000)) delay(15);
            g_lastTouchMs   = millis();
            g_lastFrameStep = millis();
            return;
        }

        g_lastTouchMs        = millis();
        g_lastFrameStep      = millis();
        g_lastStationCycleMs = millis();

        // Short tap inside Menu
        if (g_menuOpen) {
            handleMenuTap(tx, ty);
            return;
        }

        // Tap during Timelapse mode: exit and return to live radar!
        if (g_timelapseMode) {
            g_timelapseMode = false;
            freeTimelapseCatalog();
            restoreRadarScreen();
            return;
        }

        // Short tap on Radar Screen:
        // A1. Top-left station/mode badge (x < 110, y < 32): Quick-Tap to Open Radar Menu!
        if (tx < 110 && ty < 32) {
            g_menuOpen = true;
            drawMenuScreen();
            return;
        }

        // A2. Top-right frame badge (x > 264, y < 24): Toggle Pause
        if (tx > DST_WIDTH - 56 && ty < 24) {
            g_paused = !g_paused;
            renderCurrent();
            return;
        }

        // B. Touch-to-Zoom on Map:
        //    - In FULL MAP mode: tap any point (tx, ty) to zoom into that exact coordinate!
        //    - In ZOOM mode: short tap returns to FULL MAP.
        if (!g_zoomMode) {
            const int sw = g_srcWidth;   // Dimensions of the GIF currently on screen
            const int sh = g_srcHeight;  // (965x800 BMA, 936x797 SVP, 800x800, 1087x800 RYG)
            int centerSx = (tx * sw) / DST_WIDTH;
            int centerSy = (ty * sh) / DST_HEIGHT;
            int newZx    = constrain(centerSx - DST_WIDTH  / 2, 0, max(0, sw - DST_WIDTH));
            int newZy    = constrain(centerSy - DST_HEIGHT / 2, 0, max(0, sh - DST_HEIGHT));

            // Draw exact target crop box on Full Map for visual confirmation before zooming
            int bx   = (newZx * DST_WIDTH)  / sw;
            int by   = (newZy * DST_HEIGHT) / sh;
            int boxW = (DST_WIDTH  * DST_WIDTH)  / sw;
            int boxH = (DST_HEIGHT * DST_HEIGHT) / sh;
            tft.drawRect(bx, by, boxW, boxH, TFT_YELLOW);
            tft.drawRect(bx + 1, by + 1, boxW - 2, boxH - 2, TFT_RED);
            tft.fillCircle(tx, ty, 3, TFT_YELLOW);
            delay(120);

            selectZoomRegion((uint16_t)newZx, (uint16_t)newZy);
        } else {
            g_zoomMode = false;
        }
        restoreRadarScreen();
    }
}

// ============================================================================
// Arduino Setup & Main Loop
// ============================================================================
void setup() {
    WRITE_PERI_REG(RTC_CNTL_BROWN_OUT_REG, 0);
    setCpuFrequencyMhz(80); // 80 MHz base for maximum power saving & coolest running (APB bus stays 80 MHz)

    initScalingLookupTables();

    pinMode(BOOT_BTN_PIN, INPUT_PULLUP);
    pinMode(CYD_LDR_PIN,  INPUT);
    pinMode(CYD_LED_RED,   OUTPUT);
    pinMode(CYD_LED_GREEN, OUTPUT);
    pinMode(CYD_LED_BLUE,  OUTPUT);
    setStatusLed(false, false, false);

    tft.init();

    // Backlight PWM setup AFTER tft.init() so TFT_eSPI doesn't detach GPIO 21 from LEDC
    ledcSetup(0, 5000, 8);
    ledcAttachPin(21, 0);
    ledcAttachPin(27, 0);
    ledcWrite(0, 255);

    // Wait 1.2s before starting Serial output so Windows Serenum never mistakes ESP32 for a Serial Mouse
    delay(1200);
    Serial.begin(115200);
    sdLog("\n[BOOT] Starting LyRadar CYD (Reset reason: %d)\n", (int)esp_reset_reason());

    prefs.begin("radar_cfg", true);
    g_invertDisplay = prefs.getBool("inv",   false);
    g_flipScreen    = prefs.getBool("flip",  false);
    g_brightMode    = prefs.getUChar("bri",  1); // Default 100%
    g_targetStation = prefs.getUChar("sta",  0);
    g_targetZoomX   = prefs.getUShort("zx",  ZOOM_SRC_X);
    g_targetZoomY   = prefs.getUShort("zy",  ZOOM_SRC_Y);
    g_touchMapMode  = prefs.getUChar("tmap", 0);
    g_autoCycle     = prefs.getBool("auto",  true);
    g_autoStationMask = prefs.getUShort("sta_mask", 0);
    touch.loadCalibration(prefs);
    prefs.end();

    updateRegionName(g_targetZoomX, g_targetZoomY);
    applyBacklight();
    tft.setRotation(g_flipScreen ? 3 : 1);
    tft.invertDisplay(g_invertDisplay);
    tft.fillScreen(TFT_BLACK);

    spr.setColorDepth(16);
    if (!spr.createSprite(DST_WIDTH, RENDER_LINES)) {
        Serial.println("[BOOT] Sprite allocation failed!");
    }

    touch.begin();
    if (digitalRead(BOOT_BTN_PIN) == LOW) {
        Serial.println("[BOOT] BOOT button held at startup -> Launching Touch Calibration!");
        runTouchCalibration();
    }
    initSdCard(); // MicroSD on VSPI (CS=5) - separate bus from TFT (HSPI) and touch (bit-bang)

    radarPartition = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, (esp_partition_subtype_t)0x99, "radar_cache");
    if (!radarPartition || radarPartition->size < SCRATCH_ZOOM_ADDR + SCRATCH_ZOOM_BYTES) {
        drawStatusScreen("radar_cache partition too small!", "Re-upload firmware (partitions.csv)");
        while (true) delay(1000);
    }

    selectNewestBank();
    if (g_activeBank >= 0) {
        renderCurrent();
    } else {
        drawStatusScreen(kStations[g_targetStation % NUM_STATIONS].title, "Tap Top-Left for Menu / Tap to Zoom");
    }

    connectWiFi();
    applyBacklight();

    xTaskCreatePinnedToCore(downloadTask, "radar_dl", 16 * 1024, nullptr, 1, &g_dlTask, 0);
    g_lastFrameStep      = millis();
    g_lastTouchMs        = millis();
    g_lastStationCycleMs = millis();
}

void loop() {
    handleUserInput();

    // Serial debug command: send 'm' to toggle the Radar Menu (in case touch is not responding)
    if (Serial.available()) {
        int c = Serial.read();
        if (c == 'm' || c == 'M') {
            g_menuOpen = !g_menuOpen;
            if (g_menuOpen) drawMenuScreen();
            else            restoreRadarScreen();
        }
    }

    // 0. Timelapse animation playback (when g_timelapseMode is active)
    if (g_timelapseMode && !g_menuOpen) {
        if (millis() - g_lastTimelapseStep >= 250) {
            g_lastTimelapseStep = millis();
            uint16_t tPal[256];
            if (loadTimelapseFrame(g_targetStation, g_timelapseIdx, tPal, g_timelapseLabel, sizeof(g_timelapseLabel))) {
                drawTimelapseHud();
            }
            g_timelapseIdx = (g_timelapseIdx + 1) % max(1, g_timelapseCount);
        }
        delay(5);
        return;
    }

    // 1. Switch to a freshly committed bank
    if (g_bankReady) {
        activateBank(g_readyBank);
        g_bankReady = false;
        if (!g_menuOpen) renderCurrent();
        g_lastFrameStep = millis();
        g_lastStationCycleMs = millis(); // Reset 30s playback timer when new station ACTUALLY starts playing!
    }

    // 2. Reflect background download progress & errors on screen
    static int lastStage = -1, lastDlFrames = -1;
    int stage = g_dlStage, dlFrames = g_dlFrames;
    if (stage != lastStage || dlFrames != lastDlFrames) {
        lastStage    = stage;
        lastDlFrames = dlFrames;
        if (!g_menuOpen) {
            if (g_activeBank < 0) {
                if (stage == DL_CONNECTING) {
                    drawStatusScreen("Connecting to BMA Radar Server...", "HTTPS Handshake...");
                } else if (stage == DL_DOWNLOADING && dlFrames == 1) {
                    g_stepIdx = 0;
                    renderDownloadPreview(0);
                    g_lastFrameStep = millis();
                } else if (stage == DL_DOWNLOADING && dlFrames == 0) {
                    drawStatusScreen("Downloading Radar Loop...", "Decoding Frame 1/12...");
                } else if (stage == DL_ERROR) {
                    char errBuf[48];
                    snprintf(errBuf, sizeof(errBuf), "Download Error (%d) - Retrying in 30s", (int)g_lastHttpErr);
                    drawStatusScreen("LyRadar Connection Failed", errBuf);
                }
            } else if (g_paused) {
                renderCurrent();
            }
        }
    }

    // 3. Top-center real-time clock & Auto Backlight check every second
    if (millis() - g_lastClockTick >= 1000) {
        g_lastClockTick = millis();
        if (g_brightMode == 0 || g_standbyMode) applyBacklight();
        // Weather HUD is rendered inside the sprite every 350ms during live animation (zero flicker).
        // Only push directly to TFT when paused so time & weather still update without tearing.
        if (g_paused && g_screenHasRadar && !g_menuOpen) {
            drawTopHudWeather(tft, 1);
        }
    }

    // 4. Animation (Full mode: 12 frames @ 350ms; Zoom mode: 6 even frames @ 700ms = 2x delay)
    if (!g_menuOpen) {
        if (g_activeBank >= 0 && !g_paused) {
            int totalSteps = (int)g_frameCount;
            if (totalSteps > 0) {
                uint32_t waitMs = (g_stepIdx >= totalSteps - 1) ? LAST_FRAME_PAUSE_MS : FRAME_DELAY_MS;
                if (millis() - g_lastFrameStep >= waitMs) {
                    g_lastFrameStep = millis();
                    g_stepIdx       = (g_stepIdx + 1) % totalSteps;
                    renderCurrent();
                }
            }
        } else if (g_activeBank < 0 && stage == DL_DOWNLOADING && dlFrames >= 2 && !g_paused) {
            if (millis() - g_lastFrameStep >= FRAME_DELAY_MS) {
                g_lastFrameStep = millis();
                g_stepIdx       = (g_stepIdx + 1) % dlFrames;
                renderDownloadPreview(g_stepIdx);
            }
        }
    }

    // 5. Inactivity Watchdog: 3 minutes of no interaction -> return to full map, close menu/timelapse, resume AUTO mode
    if (millis() - g_lastTouchMs >= INACTIVITY_AUTO_MS) {
        bool resetNeeded = false;
        if (!g_autoCycle) {
            g_autoCycle = true;
            resetNeeded = true;
        }
        if (g_zoomMode) {
            g_zoomMode = false;
            resetNeeded = true;
        }
        if (g_menuOpen) {
            g_menuOpen = false;
            resetNeeded = true;
        }
        if (g_timelapseMode) {
            g_timelapseMode = false;
            freeTimelapseCatalog();
            resetNeeded = true;
        }
        if (resetNeeded) {
            g_lastStationCycleMs = millis();
            g_lastTouchMs        = millis();
            restoreRadarScreen();
        }
    }

    // 5b. Smart Display Eco Standby (3m inactivity & no rain -> dim to duty 30 / ~12%, wake on touch or rain)
    static constexpr uint32_t STANDBY_TIMEOUT_MS = 3 * 60000UL;
    if (!g_standbyMode && (millis() - g_lastTouchMs >= STANDBY_TIMEOUT_MS) && g_rainPct < 1) {
        g_standbyMode = true;
        applyBacklight();
        sdLog("[PWR] Inactivity > 3m & no rain -> entering Eco Standby mode (duty 30)\n");
    }
    if (g_standbyMode && g_rainPct >= 1) {
        g_standbyMode = false;
        applyBacklight();
        g_lastTouchMs = millis();
        sdLog("[PWR] Rain detected (%d%%) -> waking up from Standby!\n", (int)g_rainPct);
    }

    // 6. Station Auto-Cycling (30s per station, paused when in Zoom / Menu / Timelapse / Paused)
    //    Seamlessly advances through all 12 stations across Thailand!
    if (g_autoCycle && !g_zoomMode && !g_menuOpen && !g_timelapseMode && !g_paused) {
        // Only cycle when current bank matches target station and has played for full 30s
        if (g_bankStation == g_targetStation && (millis() - g_lastStationCycleMs >= AUTO_CYCLE_INTERVAL_MS)) {
            g_lastStationCycleMs = millis();
            uint8_t nextSta = (g_targetStation + 1) % NUM_STATIONS;
            if (g_autoStationMask != 0) {
                for (int i = 1; i <= NUM_STATIONS; i++) {
                    uint8_t cand = (g_targetStation + i) % NUM_STATIONS;
                    if (g_autoStationMask & (1 << cand)) {
                        nextSta = cand;
                        break;
                    }
                }
            }
            g_targetStation = nextSta;
            uint16_t cx, cy;
            getStationDefaultZoom(nextSta, kStations[nextSta].defWidth, kStations[nextSta].defHeight, cx, cy);
            g_targetZoomX = cx;
            g_targetZoomY = cy;
            updateRegionName(cx, cy);
            g_forceRecrop = true;
            requestRadarUpdate();
            sdLog("[AUTO] Cycling to station %u (%s)\n", (unsigned)nextSta, kStations[nextSta].shortCode);
        }
    }

    delay(5);
}
