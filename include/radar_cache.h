#pragma once

#include <SPI.h>
#include <SD.h>
#include "radar_types.h"

static SPIClass g_sdSpi(VSPI);

// ============================================================================
// MicroSD Card Initialization & Path Helper (VSPI: CS=5, SCK=18, MISO=19, MOSI=23)
// LyRadar unified folder: /ly-radar/ (cache, hist, logs, stats, wifi)
// ============================================================================
static void sdLog(const char* fmt, ...);

static void ensureSdDirs() {
    if (!g_sdReady) return;
    if (!SD.exists("/ly-radar"))       SD.mkdir("/ly-radar");
    if (!SD.exists("/ly-radar/cache")) SD.mkdir("/ly-radar/cache");
    if (!SD.exists("/ly-radar/hist"))  SD.mkdir("/ly-radar/hist");
    for (int i = 0; i < NUM_STATIONS; i++) {
        char sdir[32];
        snprintf(sdir, sizeof(sdir), "/ly-radar/hist/sta_%d", i);
        if (!SD.exists(sdir)) SD.mkdir(sdir);
    }
}

static void getSdGifPath(uint8_t stationId, char* out, size_t cap) {
    snprintf(out, cap, "/ly-radar/cache/sta_%u.gif", (unsigned)(stationId % NUM_STATIONS));
}
static const char* SD_TMP_GIF_PATH = "/ly-radar/cache/tmp.gif";

static bool isStationCacheReady(uint8_t stationId) {
    uint8_t sta = stationId % NUM_STATIONS;
    if (g_activeBank >= 0 && g_bankStation == sta) return true;
    if (g_lastHttpOkMs[sta] > 0) return true;
    if (g_sdReady) {
        char path[48];
        getSdGifPath(sta, path, sizeof(path));
        if (SD.exists(path)) {
            File f = SD.open(path, FILE_READ);
            if (f) {
                size_t sz = f.size();
                f.close();
                if (sz >= 30000) {
                    if (g_lastHttpOkMs[sta] == 0) g_lastHttpOkMs[sta] = millis();
                    return true;
                }
            }
        }
    }
    return false;
}

// SD-5: System & Error Logger (/ly-radar/log.txt)
static void sdLog(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    char buf[256];
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);

    Serial.print(buf);

    if (!g_sdReady) return;
    time_t now = time(nullptr);
    struct tm* ti = localtime(&now);
    char timePrefix[32] = "";
    if (ti && ti->tm_year > 120) {
        snprintf(timePrefix, sizeof(timePrefix), "[%04d-%02d-%02d %02d:%02d:%02d] ",
                 ti->tm_year + 1900, ti->tm_mon + 1, ti->tm_mday, ti->tm_hour, ti->tm_min, ti->tm_sec);
    } else {
        snprintf(timePrefix, sizeof(timePrefix), "[%lu s] ", (unsigned long)(millis() / 1000));
    }

    if (SD.exists("/ly-radar/log.txt")) {
        File chk = SD.open("/ly-radar/log.txt", FILE_READ);
        if (chk) {
            size_t sz = chk.size();
            chk.close();
            if (sz > 500 * 1024) {
                SD.remove("/ly-radar/log.old");
                SD.rename("/ly-radar/log.txt", "/ly-radar/log.old");
            }
        }
    }

    File f = SD.open("/ly-radar/log.txt", FILE_APPEND);
    if (f) {
        f.print(timePrefix);
        f.print(buf);
        if (buf[strlen(buf) - 1] != '\n') f.println();
        f.close();
    }
}

// SD-3: Auto WiFi Setup from /ly-radar/wifi.txt
static bool checkSdWiFiConfig(String& outSsid, String& outPass) {
    if (!g_sdReady || !SD.exists("/ly-radar/wifi.txt")) return false;
    File f = SD.open("/ly-radar/wifi.txt", FILE_READ);
    if (!f) return false;

    String ssid = f.readStringUntil('\n');
    String pass = f.readStringUntil('\n');
    f.close();

    ssid.trim();
    pass.trim();
    if (ssid.length() > 0) {
        outSsid = ssid;
        outPass = pass;
        prefs.begin("radar_cfg", false);
        prefs.putString("ssid", ssid);
        prefs.putString("pass", pass);
        prefs.end();
        sdLog("[SD-WIFI] Configured WiFi from /ly-radar/wifi.txt: '%s'\n", ssid.c_str());
        SD.remove("/ly-radar/wifi.done");
        SD.rename("/ly-radar/wifi.txt", "/ly-radar/wifi.done");
        return true;
    }
    return false;
}

// SD-4: Rain Coverage Stats Logger (/ly-radar/rain.csv)
static void recordRainStatsCsv(uint8_t stationId, uint8_t rainPct, uint32_t rainPixels) {
    if (!g_sdReady) return;
    bool writeHeader = !SD.exists("/ly-radar/rain.csv");
    File f = SD.open("/ly-radar/rain.csv", FILE_APPEND);
    if (!f) return;
    if (writeHeader) {
        f.println("timestamp,datetime,station,rain_pct,rain_pixels");
    }
    time_t now = time(nullptr);
    struct tm* ti = localtime(&now);
    char dt[32];
    if (ti && ti->tm_year > 120) {
        snprintf(dt, sizeof(dt), "%04d-%02d-%02d %02d:%02d:%02d",
                 ti->tm_year + 1900, ti->tm_mon + 1, ti->tm_mday, ti->tm_hour, ti->tm_min, ti->tm_sec);
    } else {
        snprintf(dt, sizeof(dt), "uptime_%lus", (unsigned long)(millis() / 1000));
    }
    f.printf("%lu,%s,%s,%u,%lu\n", (unsigned long)now, dt,
             kStations[stationId % NUM_STATIONS].shortCode, (unsigned)rainPct, (unsigned long)rainPixels);
    f.close();
}

// SD-2: Save Latest Decoded Frame to History (/ly-radar/hist/sta_<id>/YYYYMMDD_HHMM.rad)
static void saveFrameToHistory(int bank, int frameIdx, uint8_t stationId, uint8_t rainPct) {
    if (!g_sdReady) return;
    time_t now = time(nullptr);
    struct tm* ti = localtime(&now);
    char filePath[56];
    if (ti && ti->tm_year > 120) {
        snprintf(filePath, sizeof(filePath), "/ly-radar/hist/sta_%u/%04d%02d%02d_%02d%02d.rad",
                 (unsigned)(stationId % NUM_STATIONS),
                 ti->tm_year + 1900, ti->tm_mon + 1, ti->tm_mday, ti->tm_hour, ti->tm_min);
    } else {
        snprintf(filePath, sizeof(filePath), "/ly-radar/hist/sta_%u/%08lu.rad",
                 (unsigned)(stationId % NUM_STATIONS), (unsigned long)millis());
    }

    if (SD.exists(filePath)) return; // Already saved this minute

    File f = SD.open(filePath, FILE_WRITE);
    if (!f) return;

    RadFrameHeader rh;
    rh.magic     = RAD_FRAME_MAGIC;
    rh.timestamp = (uint32_t)now;
    rh.stationId = stationId;
    rh.rainPct   = rainPct;
    rh.reserved  = 0;
    memcpy(rh.palette, g_dlPalette, sizeof(rh.palette));

    if (f.write((const uint8_t*)&rh, sizeof(rh)) != sizeof(rh)) {
        f.close();
        SD.remove(filePath);
        return;
    }

    uint32_t srcAddr = fullAddr(bank, frameIdx);
    for (uint32_t off = 0; off < FRAME_BYTES; off += STRIP_BYTES) {
        if (esp_partition_read(radarPartition, srcAddr + off, lineIdxBuf, STRIP_BYTES) != ESP_OK ||
            f.write(lineIdxBuf, STRIP_BYTES) != STRIP_BYTES) {
            f.close();
            SD.remove(filePath);
            return;
        }
    }
    f.close();
    sdLog("[SD-HIST] Saved %s (%u KB)\n", filePath, (unsigned)(sizeof(rh) + FRAME_BYTES) / 1024);
}

// SD-2: Timelapse Frame Reader & Scanner
static constexpr int MAX_TIMELAPSE_FRAMES = 120;
struct TimelapseCatalog {
    char files[MAX_TIMELAPSE_FRAMES][36];
    int  count;
};
static TimelapseCatalog* g_tlCat = nullptr;

static int compareFileNames(const void* a, const void* b) {
    return strcmp((const char*)a, (const char*)b);
}

static void buildTimelapseCatalog(uint8_t stationId) {
    if (!g_sdReady) return;
    if (!g_tlCat) {
        g_tlCat = (TimelapseCatalog*)malloc(sizeof(TimelapseCatalog));
        if (!g_tlCat) return;
    }
    g_tlCat->count = 0;

    char dirPath[32];
    snprintf(dirPath, sizeof(dirPath), "/ly-radar/hist/sta_%u", (unsigned)(stationId % NUM_STATIONS));
    File dir = SD.open(dirPath);
    if (!dir || !dir.isDirectory()) return;

    File entry = dir.openNextFile();
    while (entry && g_tlCat->count < MAX_TIMELAPSE_FRAMES) {
        if (!entry.isDirectory()) {
            const char* name = entry.name();
            // Skip directory prefixes if present
            const char* slash = strrchr(name, '/');
            if (slash) name = slash + 1;
            if (strstr(name, ".rad")) {
                strncpy(g_tlCat->files[g_tlCat->count], name, sizeof(g_tlCat->files[0]) - 1);
                g_tlCat->files[g_tlCat->count][sizeof(g_tlCat->files[0]) - 1] = '\0';
                g_tlCat->count++;
            }
        }
        entry = dir.openNextFile();
    }
    dir.close();

    if (g_tlCat->count > 1) {
        qsort(g_tlCat->files, g_tlCat->count, sizeof(g_tlCat->files[0]), compareFileNames);
    }
    g_timelapseCount = g_tlCat->count;
    g_timelapseIdx   = 0;
    sdLog("[TIMELAPSE] Station %s cataloged %d history frames\n",
          kStations[stationId % NUM_STATIONS].shortCode, g_timelapseCount);
}

static bool loadTimelapseFrame(uint8_t stationId, int idx, uint16_t* outPal, char* outLabel, size_t labelCap) {
    if (!g_sdReady || !g_tlCat || idx < 0 || idx >= g_tlCat->count) return false;
    char path[64];
    snprintf(path, sizeof(path), "/ly-radar/hist/sta_%u/%s",
             (unsigned)(stationId % NUM_STATIONS), g_tlCat->files[idx]);

    File f = SD.open(path, FILE_READ);
    if (!f) return false;

    RadFrameHeader rh;
    if (f.read((uint8_t*)&rh, sizeof(rh)) != sizeof(rh) || rh.magic != RAD_FRAME_MAGIC) {
        f.close();
        return false;
    }

    memcpy(outPal, rh.palette, sizeof(rh.palette));

    // Extract timestamp label
    const char* fn = g_tlCat->files[idx];
    if (strlen(fn) >= 13) {
        // Format: YYYYMMDD_HHMM.rad -> YYYY-MM-DD HH:MM
        snprintf(outLabel, labelCap, "%.4s-%.2s-%.2s %.2s:%.2s",
                 fn, fn + 4, fn + 6, fn + 9, fn + 11);
    } else {
        snprintf(outLabel, labelCap, "#%d", idx + 1);
    }

    uint16_t* px = (uint16_t*)spr.getPointer();
    if (!px) { f.close(); return false; }

    for (int y = 0; y < DST_HEIGHT; y += RENDER_LINES) {
        size_t count = (size_t)RENDER_LINES * DST_WIDTH;
        if (f.read(lineIdxBuf, count) != count) break;
        for (size_t i = 0; i < count; i++) px[i] = outPal[lineIdxBuf[i]];
        spr.pushSprite(0, y);
    }
    f.close();
    return true;
}

static void freeTimelapseCatalog() {
    if (g_tlCat) {
        free(g_tlCat);
        g_tlCat = nullptr;
    }
    g_timelapseCount = 0;
    g_timelapseIdx   = 0;
}

static void initSdCard() {
    pinMode(SD_CS_PIN, OUTPUT);
    digitalWrite(SD_CS_PIN, HIGH);
    g_sdSpi.begin(SD_SCK_PIN, SD_MISO_PIN, SD_MOSI_PIN, SD_CS_PIN);
    if (SD.begin(SD_CS_PIN, g_sdSpi, 16000000)) {
        g_sdReady = true;
        ensureSdDirs();
        uint64_t cardMb = SD.cardSize() / (1024ULL * 1024ULL);
        sdLog("[SD] Mounted OK (%llu MB) - LyRadar folder ready\n", cardMb);
    } else {
        g_sdReady = false;
        Serial.println("[SD] No SD card detected -> fallback to internal Flash cache");
    }
}

// ============================================================================
// Bank Header Read / Select
// ============================================================================
static bool readBankHeader(int bank, BankHeader& hdr) {
    if (!radarPartition) return false;
    if (esp_partition_read(radarPartition, bankBase(bank), &hdr, sizeof(hdr)) != ESP_OK) return false;
    if (hdr.magic != CACHE_MAGIC) return false;
    if (hdr.frameCount == 0 || hdr.frameCount > MAX_RADAR_FRAMES) return false;
    if (hdr.zoomCount > ZOOM_FRAMES) return false;
    return true;
}

static bool activateBank(int bank) {
    BankHeader hdr;
    if (!readBankHeader(bank, hdr)) return false;
    memcpy(g_palette, hdr.palette, sizeof(g_palette));
    g_activeBank  = bank;
    g_activeSeq   = hdr.seq;
    g_activeSig   = hdr.sig;
    g_frameCount  = hdr.frameCount;
    g_zoomCount   = hdr.zoomCount;
    g_srcWidth    = (hdr.srcWidth >= DST_WIDTH && hdr.srcWidth <= MAX_SRC_WIDTH)
                        ? hdr.srcWidth : kStations[hdr.stationId % NUM_STATIONS].defWidth;
    g_srcHeight   = (hdr.srcHeight >= DST_HEIGHT && hdr.srcHeight <= MAX_SRC_HEIGHT)
                        ? hdr.srcHeight : kStations[hdr.stationId % NUM_STATIONS].defHeight;
    g_bankZoomX   = (hdr.zoomSrcX <= g_srcWidth - DST_WIDTH) ? hdr.zoomSrcX : ZOOM_SRC_X;
    g_bankZoomY   = (hdr.zoomSrcY <= g_srcHeight - DST_HEIGHT) ? hdr.zoomSrcY : ZOOM_SRC_Y;
    g_bankStation = hdr.stationId % NUM_STATIONS;
    g_rainPct     = hdr.rainPct;
    g_stepIdx     = 0;
    updateRegionName(g_targetZoomX, g_targetZoomY);
    Serial.printf("[CACHE] Active bank %d (seq %u, %u full, %u zoom @ %u,%u, dim %ux%u, sta %u, rain %u%%)\n",
                  bank, (unsigned)hdr.seq, hdr.frameCount, hdr.zoomCount,
                  (unsigned)g_bankZoomX, (unsigned)g_bankZoomY,
                  (unsigned)g_srcWidth, (unsigned)g_srcHeight, g_bankStation, g_rainPct);
    return true;
}

static void selectNewestBank() {
    BankHeader h0, h1;
    bool v0 = readBankHeader(0, h0);
    bool v1 = readBankHeader(1, h1);
    if (v0 && v1)      activateBank((h1.seq > h0.seq) ? 1 : 0);
    else if (v0)       activateBank(0);
    else if (v1)       activateBank(1);
}

// ============================================================================
// Fast Sector Eraser (respects pre-erased bank progress, yields every 8 sectors)
// ============================================================================
static bool ensureBankErased(int bank, uint32_t upToOffsetInPartition, bool abortOnRecrop = false) {
    uint32_t base = bankBase(bank);
    uint32_t cur  = base + g_bankErasedTo[bank];
    int cnt = 0;
    while (cur < upToOffsetInPartition) {
        if (abortOnRecrop && g_forceRecrop) return false;
        if (esp_partition_erase_range(radarPartition, cur, SECTOR_SIZE) != ESP_OK) {
            Serial.printf("[FLASH] Erase failed at 0x%06X\n", (unsigned)cur);
            return false;
        }
        cur += SECTOR_SIZE;
        g_bankErasedTo[bank] = cur - base;
        if ((++cnt & 7) == 0) vTaskDelay(1);
    }
    return true;
}

static bool eraseScratchZoomSlot() {
    int cnt = 0;
    for (uint32_t off = 0; off < SCRATCH_ZOOM_BYTES; off += SECTOR_SIZE) {
        if (esp_partition_erase_range(radarPartition, SCRATCH_ZOOM_ADDR + off, SECTOR_SIZE) != ESP_OK) {
            return false;
        }
        if ((++cnt & 7) == 0) vTaskDelay(1);
    }
    return true;
}

static void preEraseInactiveBank(int bank) {
    if (bank < 0 || bank > 1 || !radarPartition || g_forceRecrop) return;
    if (g_bankErasedTo[bank] >= BANK_SIZE) return;
    Serial.printf("[FLASH] Pre-erasing inactive bank %d in background...\n", bank);
    if (g_bankErasedTo[bank] == 0) {
        if (esp_partition_erase_range(radarPartition, bankBase(bank), SECTOR_SIZE) != ESP_OK) return;
        g_bankErasedTo[bank] = SECTOR_SIZE;
    }
    ensureBankErased(bank, bankBase(bank) + BANK_SIZE, true);
}

// ============================================================================
// Dual-View Pixel Mapper (4-Row Strip Batching + Dynamic GIF Dimensions + LCT Map)
// ============================================================================
struct FrameStreamMapper {
    int      frameIdx;
    int      transIdx;
    uint16_t ix, iy, iw, ih;
    uint16_t lx, ly;
    int      srcW;
    int      srcH;
    int      tsSrcX;
    int      tsSrcY;
    int      zoomSrcX;
    int      zoomSrcY;
    int      curSy;
    int16_t  curDy;
    int16_t  curZy;
    int      curFullStrip;
    int      curZoomStrip;
    int      fullRowOff;
    int      zoomRowOff;
    int      flushCounter;
    bool     curTsRow;
    uint32_t fullBase;
    uint32_t prevFullBase;
    uint32_t zoomBase;
    uint32_t prevZoomBase;

    // Odd 1-based frames (fIdx 0,2,4,6,8,10) write to SCRATCH_ZOOM_ADDR;
    // Even 1-based frames (fIdx 1,3,5,7,9,11) write to zoomAddr(bank, fIdx / 2).
    static uint32_t zoomDestFor(int bank, int fIdx) {
        return (fIdx & 1) ? zoomAddr(bank, fIdx / 2) : SCRATCH_ZOOM_ADDR;
    }

    void flushFullStrip() {
        if (curFullStrip >= 0 && curFullStrip < NUM_STRIPS) {
            esp_partition_write(radarPartition, fullBase + (uint32_t)curFullStrip * STRIP_BYTES,
                                g_ws->fullStripBuf, STRIP_BYTES);
            g_ws->fullStripDone[curFullStrip] = true;
            curFullStrip = -1;
            if ((++flushCounter & 7) == 0) vTaskDelay(1);
        }
    }

    void flushZoomStrip() {
        if (curZoomStrip >= 0 && curZoomStrip < NUM_STRIPS) {
            esp_partition_write(radarPartition, zoomBase + (uint32_t)curZoomStrip * STRIP_BYTES,
                                g_ws->zoomStripBuf, STRIP_BYTES);
            g_ws->zoomStripDone[curZoomStrip] = true;
            curZoomStrip = -1;
            if ((++flushCounter & 7) == 0) vTaskDelay(1);
        }
    }

    void beginFrame(int bank, int fIdx, uint16_t x, uint16_t y, uint16_t w, uint16_t h,
                    int tIdx, int zSrcX, int zSrcY, int sW, int sH) {
        frameIdx     = fIdx;
        transIdx     = tIdx;
        ix = x; iy = y; iw = w; ih = h;
        lx = 0; ly = 0;
        srcW         = sW;
        srcH         = sH;
        tsSrcX       = max(0, srcW - TS_WIDTH - 3);
        tsSrcY       = max(0, srcH - TS_HEIGHT - 7);
        zoomSrcX     = zSrcX;
        zoomSrcY     = zSrcY;
        curSy        = -1;
        curDy        = -1;
        curZy        = -1;
        curFullStrip = -1;
        curZoomStrip = -1;
        fullRowOff   = 0;
        zoomRowOff   = 0;
        flushCounter = 0;
        curTsRow     = false;
        fullBase     = fullAddr(bank, fIdx);
        prevFullBase = (fIdx > 0) ? fullAddr(bank, fIdx - 1) : 0;
        zoomBase     = zoomDestFor(bank, fIdx);
        prevZoomBase = (fIdx > 0) ? zoomDestFor(bank, fIdx - 1) : 0;
        memset(g_ws->fullStripDone, 0, sizeof(g_ws->fullStripDone));
        memset(g_ws->zoomStripDone, 0, sizeof(g_ws->zoomStripDone));

        if (fIdx == 0) {
            memset(g_ws->fullTailBuf, 0, sizeof(g_ws->fullTailBuf));
            memset(g_ws->zoomTailBuf, 0, sizeof(g_ws->zoomTailBuf));
            memset(g_ws->tsCropBuf,   0, sizeof(g_ws->tsCropBuf));
        }
    }

    inline void pixel(uint8_t rawIdx) {
        if (lx == 0) rowStart();
        uint16_t sx = ix + lx;
        if ((int)rawIdx != transIdx && curSy >= 0 && sx < srcW) {
            plot(sx, g_ws->lctMap[rawIdx]);
        }
        if (++lx >= iw) {
            lx = 0;
            ly++;
        }
    }

    void rowStart() {
        int sy = iy + ly;
        if (sy >= srcH || sy >= MAX_SRC_HEIGHT) {
            curSy = -1; curDy = -1; curZy = -1; curTsRow = false;
            return;
        }
        curSy = sy;
        curDy = lutFullDy[sy];
        if (curDy >= 0 && curDy < DIRECT_ROWS) {
            int s = curDy / STRIP_ROWS;
            if (s != curFullStrip) {
                flushFullStrip();
                if (frameIdx > 0) {
                    esp_partition_read(radarPartition, prevFullBase + (uint32_t)s * STRIP_BYTES,
                                       g_ws->fullStripBuf, STRIP_BYTES);
                } else {
                    memset(g_ws->fullStripBuf, 0, STRIP_BYTES);
                }
                curFullStrip = s;
            }
            fullRowOff = (curDy % STRIP_ROWS) * DST_WIDTH;
        }
        int zy = sy - zoomSrcY;
        curZy = (zy >= 0 && zy < DST_HEIGHT) ? (int16_t)zy : -1;
        if (curZy >= 0 && curZy < DIRECT_ROWS) {
            int zs = curZy / STRIP_ROWS;
            if (zs != curZoomStrip) {
                flushZoomStrip();
                if (frameIdx > 0) {
                    esp_partition_read(radarPartition, prevZoomBase + (uint32_t)zs * STRIP_BYTES,
                                       g_ws->zoomStripBuf, STRIP_BYTES);
                } else {
                    memset(g_ws->zoomStripBuf, 0, STRIP_BYTES);
                }
                curZoomStrip = zs;
            }
            zoomRowOff = (curZy % STRIP_ROWS) * DST_WIDTH;
        }
        curTsRow = (sy >= tsSrcY && sy < tsSrcY + TS_HEIGHT);
    }

    inline void plot(uint16_t sx, uint8_t c) {
        if (curDy >= 0 && sx < MAX_SRC_WIDTH) {
            int16_t dx = lutFullDx[sx];
            if (dx >= 0) {
                if (curDy < DIRECT_ROWS) g_ws->fullStripBuf[fullRowOff + dx] = c;
                else                     g_ws->fullTailBuf[(curDy - DIRECT_ROWS) * DST_WIDTH + dx] = c;
            } else if (curDy < DIRECT_ROWS && g_prio[c] == 3 && sx >= 60) {
                int mappedDx = ((int)sx * DST_WIDTH) / srcW;
                if (mappedDx >= 0 && mappedDx < DST_WIDTH) {
                    g_ws->fullStripBuf[fullRowOff + mappedDx] = c;
                }
            }
        }
        if (curZy >= 0) {
            int zx = (int)sx - zoomSrcX;
            if (zx >= 0 && zx < DST_WIDTH) {
                if (curZy < DIRECT_ROWS) g_ws->zoomStripBuf[zoomRowOff + zx] = c;
                else                     g_ws->zoomTailBuf[(curZy - DIRECT_ROWS) * DST_WIDTH + zx] = c;
            }
        }
        if (curTsRow) {
            int tx = (int)sx - tsSrcX;
            if (tx >= 0 && tx < TS_WIDTH) g_ws->tsCropBuf[(curSy - tsSrcY) * TS_WIDTH + tx] = c;
        }
    }

    bool endFrame() {
        flushFullStrip();
        flushZoomStrip();

        for (int s = 0; s < NUM_STRIPS; s++) {
            if ((s & 7) == 7) vTaskDelay(1);
            if (g_ws->fullStripDone[s]) continue;
            if (frameIdx > 0) {
                esp_partition_read(radarPartition, prevFullBase + (uint32_t)s * STRIP_BYTES,
                                   g_ws->fullStripBuf, STRIP_BYTES);
            } else {
                memset(g_ws->fullStripBuf, 0, STRIP_BYTES);
            }
            esp_partition_write(radarPartition, fullBase + (uint32_t)s * STRIP_BYTES,
                                g_ws->fullStripBuf, STRIP_BYTES);
        }
        for (int zs = 0; zs < NUM_STRIPS; zs++) {
            if ((zs & 7) == 7) vTaskDelay(1);
            if (g_ws->zoomStripDone[zs]) continue;
            if (frameIdx > 0) {
                esp_partition_read(radarPartition, prevZoomBase + (uint32_t)zs * STRIP_BYTES,
                                   g_ws->zoomStripBuf, STRIP_BYTES);
            } else {
                memset(g_ws->zoomStripBuf, 0, STRIP_BYTES);
            }
            esp_partition_write(radarPartition, zoomBase + (uint32_t)zs * STRIP_BYTES,
                                g_ws->zoomStripBuf, STRIP_BYTES);
        }
        vTaskDelay(1);

        // Stamp crisp 1:1 Timestamp Badge onto bottom-right of both Full & Zoom views
        for (int ty = 0; ty < TS_HEIGHT; ty++) {
            int tailOff = (TAIL_ROWS - TS_HEIGHT + ty) * DST_WIDTH + (DST_WIDTH - TS_WIDTH);
            memcpy(&g_ws->fullTailBuf[tailOff], &g_ws->tsCropBuf[ty * TS_WIDTH], TS_WIDTH);
            memcpy(&g_ws->zoomTailBuf[tailOff], &g_ws->tsCropBuf[ty * TS_WIDTH], TS_WIDTH);
        }

        return esp_partition_write(radarPartition, fullBase + (uint32_t)DIRECT_ROWS * DST_WIDTH,
                                   g_ws->fullTailBuf, sizeof(g_ws->fullTailBuf)) == ESP_OK &&
               esp_partition_write(radarPartition, zoomBase + (uint32_t)DIRECT_ROWS * DST_WIDTH,
                                   g_ws->zoomTailBuf, sizeof(g_ws->zoomTailBuf)) == ESP_OK;
    }
};

static FrameStreamMapper g_mapper;
