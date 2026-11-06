#pragma once

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <SD.h>
#include "radar_cache.h"

// ============================================================================
// Buffered GIF Byte Stream
//   - Source A: HTTP(S) WiFiClient (raw or chunked), optionally teeing raw GIF bytes to an SD file
//   - Source B: SD Card File (fast offline recrop, no network)
// ============================================================================
class GifByteStream {
public:
    GifByteStream(WiFiClient* client, bool isChunked, File* tee)
        : _client(client), _file(nullptr), _tee(tee), _isChunked(isChunked) {}
    explicit GifByteStream(File* src)
        : _client(nullptr), _file(src), _tee(nullptr), _isChunked(false) {}

    inline int readByte() {
        if (_pos >= _len) {
            if (!fillBuffer()) return -1;
        }
        _totalRead++;
        return httpReadBuf[_pos++];
    }

    bool readBytesExact(uint8_t* dst, size_t count) {
        size_t got = 0;
        while (got < count) {
            if (_pos >= _len) {
                if (!fillBuffer()) return false;
            }
            size_t avail = _len - _pos;
            size_t take  = (count - got < avail) ? (count - got) : avail;
            memcpy(dst + got, httpReadBuf + _pos, take);
            _pos       += take;
            got        += take;
            _totalRead += take;
        }
        return true;
    }

    bool skipBytes(size_t count) {
        size_t skipped = 0;
        while (skipped < count) {
            if (_pos >= _len) {
                if (!fillBuffer()) return false;
            }
            size_t avail = _len - _pos;
            size_t take  = (count - skipped < avail) ? (count - skipped) : avail;
            _pos       += take;
            skipped    += take;
            _totalRead += take;
        }
        return true;
    }

    size_t totalRead() const { return _totalRead; }
    bool   teeOk()     const { return _teeOk; }

private:
    bool fillBuffer() {
        if (_eof) return false;
        int r = _file ? (int)_file->read(httpReadBuf, sizeof(httpReadBuf)) : fillFromHttp();
        if (r <= 0) {
            _eof = true;
            return false;
        }
        _pos = 0;
        _len = (size_t)r;
        if (_tee && _teeOk) {
            if (_tee->write(httpReadBuf, (size_t)r) != (size_t)r) _teeOk = false;
        }
        return true;
    }

    int readRawByteBlocking() {
        uint32_t startWait = millis();
        while (_client->connected() || _client->available() > 0) {
            if (_client->available() > 0) return _client->read();
            if (millis() - startWait > 15000) return -1;
            vTaskDelay(1);
        }
        return -1;
    }

    bool readNextChunkSize() {
        char hexLine[32];
        int idx = 0;
        while (idx < (int)sizeof(hexLine) - 1) {
            int c = readRawByteBlocking();
            if (c < 0) return false;
            if (c == '\n') {
                if (idx > 0) break;
                continue;
            }
            if (c == '\r') continue;
            hexLine[idx++] = (char)c;
        }
        hexLine[idx] = '\0';
        char* semi = strchr(hexLine, ';');
        if (semi) *semi = '\0';
        _chunkRem = strtoul(hexLine, nullptr, 16);
        return _chunkRem != 0;
    }

    int fillFromHttp() {
        if (_isChunked && _chunkRem == 0) {
            if (!readNextChunkSize()) return 0;
        }
        uint32_t startWait = millis();
        while (_client->connected() || _client->available() > 0) {
            int avail = _client->available();
            if (avail > 0) {
                size_t maxRead = sizeof(httpReadBuf);
                if (_isChunked && _chunkRem < maxRead) maxRead = _chunkRem;
                if ((size_t)avail < maxRead) maxRead = (size_t)avail;
                int r = _client->read(httpReadBuf, maxRead);
                if (r > 0) {
                    if (_isChunked) _chunkRem -= (size_t)r;
                    return r;
                }
            }
            if (millis() - startWait > 15000) return 0;
            vTaskDelay(1);
        }
        return 0;
    }

    WiFiClient* _client;
    File*       _file;
    File*       _tee;
    bool        _isChunked;
    size_t      _pos       = 0;
    size_t      _len       = 0;
    size_t      _totalRead = 0;
    size_t      _chunkRem  = 0;
    bool        _eof       = false;
    bool        _teeOk     = true;
};

// Nearest Global-Color-Table entry for an RGB triple (exact match shortcut)
static uint8_t nearestGctIndex(uint8_t r, uint8_t g, uint8_t b, int gctColors) {
    int bestIdx = 0;
    int32_t bestDist = INT32_MAX;
    for (int i = 0; i < gctColors; i++) {
        int dr = (int)r - g_ws->gctRgb[i][0];
        int dg = (int)g - g_ws->gctRgb[i][1];
        int db = (int)b - g_ws->gctRgb[i][2];
        int32_t d = dr * dr + dg * dg + db * db;
        if (d < bestDist) {
            bestDist = d;
            bestIdx  = i;
            if (d == 0) break;
        }
    }
    return (uint8_t)bestIdx;
}

static void setPaletteEntry(int i, uint8_t r, uint8_t g, uint8_t b) {
    g_ws->gctRgb[i][0] = r;
    g_ws->gctRgb[i][1] = g;
    g_ws->gctRgb[i][2] = b;
    uint16_t c565  = rgb888To565(r, g, b);
    g_dlPalette[i] = (c565 >> 8) | (c565 << 8);
    g_prio[i]      = classifyColorPriority(r, g, b);
}

// ============================================================================
// Shared GIF Decoder (HTTP or SD source) -> Flash bank
//   Caller must: allocate g_ws before, free g_ws / close source after.
//   allowUnchanged: stop after Frame 0 if radar image identical to active bank.
// ============================================================================
static int decodeGifStream(GifByteStream& stream, int bank, uint16_t reqZoomX, uint16_t reqZoomY,
                           uint8_t dlStation, bool forceCrop, bool allowUnchanged, int decodeStage) {
    // 1. GIF Header (6 bytes) + Logical Screen Descriptor (7 bytes)
    uint8_t gifHdr[13];
    if (!stream.readBytesExact(gifHdr, 13) || memcmp(gifHdr, "GIF8", 4) != 0) {
        Serial.println("[GIF] Invalid GIF header");
        g_lastHttpErr = -300;
        return DL_FAIL;
    }
    uint16_t gifW = gifHdr[6] | (gifHdr[7] << 8);
    uint16_t gifH = gifHdr[8] | (gifHdr[9] << 8);
    if (gifW < DST_WIDTH || gifH < DST_HEIGHT || gifW > MAX_SRC_WIDTH || gifH > MAX_SRC_HEIGHT) {
        Serial.printf("[GIF] Unsupported dimensions %ux%u\n", gifW, gifH);
        g_lastHttpErr = -301;
        return DL_FAIL;
    }
    initScalingLookupTables(gifW, gifH);
    g_dlSrcWidth  = gifW;
    g_dlSrcHeight = gifH;

    // Clamp requested zoom crop to this GIF's dimensions (and sync target so no endless recrop loop)
    uint16_t dlZoomX = min<uint16_t>(reqZoomX, gifW - DST_WIDTH);
    uint16_t dlZoomY = min<uint16_t>(reqZoomY, gifH - DST_HEIGHT);
    if ((dlZoomX != reqZoomX || dlZoomY != reqZoomY) &&
        reqZoomX == g_targetZoomX && reqZoomY == g_targetZoomY && dlStation == g_targetStation) {
        g_targetZoomX = dlZoomX;
        g_targetZoomY = dlZoomY;
    }
    reqZoomX = dlZoomX;
    reqZoomY = dlZoomY;
    g_dlZoomX           = dlZoomX;
    g_dlZoomY           = dlZoomY;
    g_dlStation         = dlStation;
    g_dlZoomReadyFrames = 0;

    uint32_t sig = fnv1a(FNV_OFFSET, gifHdr, sizeof(gifHdr));

    // 2. Global Color Table
    for (int i = 0; i < 256; i++) g_ws->lctMap[i] = (uint8_t)i;
    int gctColors = 0;
    uint8_t packed = gifHdr[10];
    if (packed & 0x80) {
        gctColors = 1 << ((packed & 0x07) + 1);
        for (int i = 0; i < gctColors; i++) {
            uint8_t rgb[3];
            if (!stream.readBytesExact(rgb, 3)) return DL_FAIL;
            sig = fnv1a(sig, rgb, 3);
            setPaletteEntry(i, rgb[0], rgb[1], rgb[2]);
        }
    }

    // Invalidate target bank header if not already pre-erased
    if (g_bankErasedTo[bank] == 0) {
        if (esp_partition_erase_range(radarPartition, bankBase(bank), SECTOR_SIZE) != ESP_OK) return DL_FAIL;
        g_bankErasedTo[bank] = SECTOR_SIZE;
    }

    g_dlStage = decodeStage;
    int  decodedFrames    = 0;
    int  zoomCount        = 0;
    int  pendingTransIdx  = -1;
    int  subBlockCounter  = 0;
    bool abortedForRecrop = false;

    File zoomOutFile;
    if (g_sdReady) {
        if (SD.exists(SD_TMP_ZOOM_PATH)) SD.remove(SD_TMP_ZOOM_PATH);
        zoomOutFile = SD.open(SD_TMP_ZOOM_PATH, FILE_WRITE);
        if (zoomOutFile) {
            ZoomCacheHeader zHdr;
            memset(&zHdr, 0, sizeof(zHdr));
            zHdr.magic      = 0x5A4F4F4D;
            zHdr.sig        = sig;
            zHdr.frameCount = 0;
            zHdr.zoomSrcX   = dlZoomX;
            zHdr.zoomSrcY   = dlZoomY;
            zHdr.stationId  = dlStation;
            memcpy(zHdr.palette, g_dlPalette, sizeof(zHdr.palette));
            zoomOutFile.write((const uint8_t*)&zHdr, sizeof(zHdr));
        }
    }

    // 3. Parse GIF Blocks & Decode Frames
    while (decodedFrames < MAX_RADAR_FRAMES) {
        if (g_activeBank >= 0 &&
            (reqZoomX != g_targetZoomX || reqZoomY != g_targetZoomY || dlStation != g_targetStation)) {
            Serial.println("[GIF] Target zoom/station changed mid-decode -> restarting for new target");
            abortedForRecrop = true;
            break;
        }

        int blockType = stream.readByte();
        if (blockType == 0x3B || blockType < 0) break;

        if (blockType == 0x21) {
            int extLabel = stream.readByte();
            if (extLabel < 0) break;
            if (extLabel == 0xF9) {
                int subLen = stream.readByte();
                if (subLen == 4) {
                    uint8_t gce[4];
                    if (!stream.readBytesExact(gce, 4)) break;
                    pendingTransIdx = (gce[0] & 0x01) ? (int)gce[3] : -1;
                } else if (subLen > 0) {
                    if (!stream.skipBytes(subLen)) break;
                }
            }
            while (true) {
                int subLen = stream.readByte();
                if (subLen <= 0) break;
                if (!stream.skipBytes(subLen)) break;
            }
            continue;
        }

        if (blockType != 0x2C) break;

        uint8_t imgDesc[9];
        if (!stream.readBytesExact(imgDesc, 9)) break;
        if (decodedFrames == 0) sig = fnv1a(sig, imgDesc, sizeof(imgDesc));

        uint16_t ix = imgDesc[0] | (imgDesc[1] << 8);
        uint16_t iy = imgDesc[2] | (imgDesc[3] << 8);
        uint16_t iw = imgDesc[4] | (imgDesc[5] << 8);
        uint16_t ih = imgDesc[6] | (imgDesc[7] << 8);
        uint8_t  ipacked = imgDesc[8];

        // Local Color Table (TMD frames 2..12): remap to Frame-1 palette so one palette fits all frames
        if (ipacked & 0x80) {
            int lctColors = 1 << ((ipacked & 0x07) + 1);
            bool adopt = (gctColors == 0); // No GCT -> first LCT becomes the palette
            bool lctOk = true;
            for (int i = 0; i < lctColors; i++) {
                uint8_t rgb[3];
                if (!stream.readBytesExact(rgb, 3)) { lctOk = false; break; }
                if (adopt) {
                    setPaletteEntry(i, rgb[0], rgb[1], rgb[2]);
                    g_ws->lctMap[i] = (uint8_t)i;
                } else {
                    g_ws->lctMap[i] = nearestGctIndex(rgb[0], rgb[1], rgb[2], gctColors);
                }
            }
            if (!lctOk) break;
            if (adopt) gctColors = lctColors;
            for (int i = lctColors; i < 256; i++) g_ws->lctMap[i] = 0;
            if ((++subBlockCounter & 3) == 0) vTaskDelay(1);
        } else {
            for (int i = 0; i < 256; i++) g_ws->lctMap[i] = (uint8_t)i;
        }

        int lzwMinCodeSize = stream.readByte();
        if (lzwMinCodeSize < 2 || lzwMinCodeSize > 8 || iw == 0 || ih == 0) break;

        // Ensure Flash space for this frame's Full slot and Zoom/Scratch slot
        if (!ensureBankErased(bank, fullAddr(bank, decodedFrames) + FRAME_BYTES)) break;
        if (decodedFrames & 1) {
            if (!ensureBankErased(bank, zoomAddr(bank, decodedFrames / 2) + FRAME_BYTES)) break;
        } else {
            if (!eraseScratchZoomSlot()) break;
        }

        g_mapper.beginFrame(bank, decodedFrames, ix, iy, iw, ih, pendingTransIdx,
                            dlZoomX, dlZoomY, gifW, gifH);
        pendingTransIdx = -1;

        const uint16_t clearCode = 1 << lzwMinCodeSize;
        const uint16_t eoiCode   = clearCode + 1;
        uint16_t nextCode        = eoiCode + 1;
        int      codeSize        = lzwMinCodeSize + 1;
        uint16_t oldCode         = 0xFFFF;
        uint8_t  firstChar       = 0;
        uint32_t bitBuf          = 0;
        int      bitCnt          = 0;
        uint32_t pixelIdx        = 0;
        uint32_t totalPixels     = (uint32_t)iw * (uint32_t)ih;
        bool     frameDone       = false;
        bool     corrupt         = false;
        bool     streamEOF       = false;
        int      lastSubLen      = 0;

        while (true) {
            int subLen = stream.readByte();
            lastSubLen = subLen;
            if (subLen < 0)  { streamEOF = true; break; }
            if (subLen == 0) break;
            if (!stream.readBytesExact(gifSubBlock, subLen)) { streamEOF = true; break; }
            if (decodedFrames == 0) sig = fnv1a(sig, gifSubBlock, subLen);
            if ((++subBlockCounter & 31) == 0) vTaskDelay(1);
            if (frameDone) continue;

            for (int i = 0; i < subLen && !frameDone; i++) {
                bitBuf |= ((uint32_t)gifSubBlock[i] << bitCnt);
                bitCnt += 8;

                while (bitCnt >= codeSize) {
                    uint16_t code = bitBuf & ((1U << codeSize) - 1U);
                    bitBuf >>= codeSize;
                    bitCnt -= codeSize;

                    if (code == clearCode) {
                        codeSize = lzwMinCodeSize + 1;
                        nextCode = eoiCode + 1;
                        oldCode  = 0xFFFF;
                        continue;
                    }
                    if (code == eoiCode) { frameDone = true; break; }

                    if (oldCode == 0xFFFF) {
                        if (code >= clearCode) { corrupt = true; frameDone = true; break; }
                        firstChar = (uint8_t)code;
                        if (pixelIdx < totalPixels) { g_mapper.pixel(firstChar); pixelIdx++; }
                        oldCode = code;
                        continue;
                    }
                    if (code > nextCode) { corrupt = true; frameDone = true; break; }

                    uint16_t inCode = code;
                    int sp = 0;
                    if (code == nextCode) {
                        g_ws->lzw_stack[sp++] = firstChar;
                        code = oldCode;
                    }
                    while (code >= clearCode && sp < 4095) {
                        g_ws->lzw_stack[sp++] = g_ws->lzw_suffix[code];
                        code = g_ws->lzw_prefix[code];
                    }
                    firstChar = (uint8_t)code;
                    g_ws->lzw_stack[sp++] = firstChar;

                    while (sp > 0 && pixelIdx < totalPixels) {
                        g_mapper.pixel(g_ws->lzw_stack[--sp]);
                        pixelIdx++;
                    }

                    if (nextCode < 4096) {
                        g_ws->lzw_prefix[nextCode] = oldCode;
                        g_ws->lzw_suffix[nextCode] = firstChar;
                        nextCode++;
                        if ((nextCode == (1U << codeSize)) && codeSize < 12) codeSize++;
                    }
                    oldCode = inCode;
                }
            }
        }

        if (corrupt || pixelIdx < totalPixels) {
            Serial.printf("[GIF] Frame %d incomplete (%u/%u px, eof=%d) -> recovering from prev frame\n",
                          decodedFrames + 1, (unsigned)pixelIdx, (unsigned)totalPixels, (int)streamEOF);
            if (!streamEOF && lastSubLen > 0) {
                while (true) {
                    int sl = stream.readByte();
                    if (sl < 0)  { streamEOF = true; break; }
                    if (sl == 0) break;
                    if (!stream.skipBytes(sl)) { streamEOF = true; break; }
                }
            }
            if ((decodedFrames == 0 || streamEOF) && pixelIdx < totalPixels / 2) {
                break;
            }
        }

        if (!g_mapper.endFrame()) {
            Serial.println("[FLASH] Write failed");
            break;
        }

        // Single-pass change check right after Frame 0
        if (allowUnchanged && decodedFrames == 0 && !forceCrop && g_activeBank >= 0 &&
            sig == g_activeSig && g_frameCount == MAX_RADAR_FRAMES &&
            dlZoomX == g_bankZoomX && dlZoomY == g_bankZoomY && dlStation == g_bankStation &&
            dlZoomX == g_targetZoomX && dlZoomY == g_targetZoomY && dlStation == g_targetStation) {
            Serial.printf("[UPDATE] Radar unchanged (sig %08X) - stopping after Frame 0\n", (unsigned)sig);
            if (zoomOutFile) { zoomOutFile.close(); SD.remove(SD_TMP_ZOOM_PATH); }
            g_bankErasedTo[bank] = 0;
            g_lastOkUpdateMs = millis();
            return DL_UNCHANGED;
        }

        decodedFrames++;
        if ((decodedFrames & 1) == 0) zoomCount++; // Even frames: 2/12, 4/12 ... 12/12
        g_dlFrames          = decodedFrames;
        g_dlZoomReadyFrames = decodedFrames;

        // Stream this frame's 1:1 HD zoom view to SD card
        if (zoomOutFile) {
            int fIdx = decodedFrames - 1;
            uint32_t srcZoomAddr = (fIdx & 1) ? zoomAddr(bank, fIdx / 2) : SCRATCH_ZOOM_ADDR;
            for (uint32_t off = 0; off < FRAME_BYTES; off += STRIP_BYTES) {
                if (esp_partition_read(radarPartition, srcZoomAddr + off, g_ws->zoomStripBuf, STRIP_BYTES) == ESP_OK) {
                    zoomOutFile.write(g_ws->zoomStripBuf, STRIP_BYTES);
                }
            }
        }

        Serial.printf("[GIF] Frame %d/%d -> bank %d (%u bytes read)\n",
                      decodedFrames, MAX_RADAR_FRAMES, bank, (unsigned)stream.totalRead());
    }

    if (abortedForRecrop) {
        if (zoomOutFile) { zoomOutFile.close(); SD.remove(SD_TMP_ZOOM_PATH); }
        g_bankErasedTo[bank] = 0;
        return DL_FAIL;
    }

    // If only 1 frame decoded, copy Frame 0's zoom view from SCRATCH_ZOOM_ADDR
    if (decodedFrames == 1 && zoomCount == 0 && ensureBankErased(bank, zoomAddr(bank, 0) + FRAME_BYTES)) {
        bool ok = true;
        int  sc = 0;
        for (uint32_t off = 0; off < FRAME_BYTES; off += STRIP_BYTES) {
            if ((++sc & 7) == 0) vTaskDelay(1);
            if (esp_partition_read(radarPartition, SCRATCH_ZOOM_ADDR + off, g_ws->zoomStripBuf, STRIP_BYTES) != ESP_OK ||
                esp_partition_write(radarPartition, zoomAddr(bank, 0) + off, g_ws->zoomStripBuf, STRIP_BYTES) != ESP_OK) {
                ok = false;
                break;
            }
        }
        if (ok) zoomCount = 1;
    }

    // Rain echo percentage & intensity in the latest Zoom frame
    uint8_t  rainPct      = 0;
    uint8_t  maxRainLevel = 0; // 0=None, 1=LGT, 2=MOD, 3=HVY
    uint32_t rainPixels   = 0;
    if (zoomCount > 0) {
        uint32_t lastZoomAddr = zoomAddr(bank, zoomCount - 1);
        int      sc           = 0;
        for (uint32_t off = 0; off < FRAME_BYTES; off += STRIP_BYTES) {
            if ((++sc & 7) == 0) vTaskDelay(1);
            if (esp_partition_read(radarPartition, lastZoomAddr + off, g_ws->zoomStripBuf, STRIP_BYTES) == ESP_OK) {
                for (size_t i = 0; i < STRIP_BYTES; i++) {
                    uint8_t p = g_prio[g_ws->zoomStripBuf[i]];
                    if (p >= 2) {
                        rainPixels++;
                        uint8_t lvl = p - 1; // 2->1 (LGT), 3->2 (MOD), 4->3 (HVY)
                        if (lvl > maxRainLevel) maxRainLevel = lvl;
                    }
                }
            }
        }
        rainPct = (uint8_t)min(100UL, (unsigned long)((rainPixels * 100UL) / FRAME_BYTES));
    }

    if (decodedFrames < 1) {
        g_bankErasedTo[bank] = 0;
        g_lastHttpErr = -400;
        return DL_FAIL;
    }

    if (!forceCrop && decodedFrames < MAX_RADAR_FRAMES && g_activeBank >= 0 && g_frameCount >= MAX_RADAR_FRAMES) {
        Serial.printf("[GIF] Partial background update (%d/%d frames) - keeping full active bank & retrying\n",
                      decodedFrames, MAX_RADAR_FRAMES);
        g_bankErasedTo[bank] = 0;
        g_lastHttpErr = -400 - decodedFrames;
        return DL_FAIL;
    }

    g_partialBank = (decodedFrames < MAX_RADAR_FRAMES);
    if (dlZoomX == g_targetZoomX && dlZoomY == g_targetZoomY && dlStation == g_targetStation) {
        g_forceRecrop = false;
    }

    BankHeader hdr;
    memset(&hdr, 0, sizeof(hdr));
    hdr.magic      = CACHE_MAGIC;
    hdr.seq        = g_activeSeq + 1;
    hdr.sig        = sig;
    hdr.frameCount = decodedFrames;
    hdr.zoomCount  = zoomCount;
    hdr.zoomSrcX   = dlZoomX;
    hdr.zoomSrcY   = dlZoomY;
    hdr.srcWidth   = gifW;
    hdr.srcHeight  = gifH;
    hdr.stationId  = dlStation;
    hdr.rainPct    = rainPct;
    hdr.rainLevel  = maxRainLevel;
    memcpy(hdr.palette, g_dlPalette, sizeof(hdr.palette));

    if (esp_partition_write(radarPartition, bankBase(bank), &hdr, sizeof(hdr)) != ESP_OK) {
        Serial.println("[FLASH] Header commit failed");
        if (zoomOutFile) { zoomOutFile.close(); SD.remove(SD_TMP_ZOOM_PATH); }
        g_bankErasedTo[bank] = 0;
        return DL_FAIL;
    }

    // Finalize 12-frame HD Zoom cache on MicroSD
    if (zoomOutFile) {
        if (decodedFrames >= MAX_RADAR_FRAMES) {
            zoomOutFile.seek(0);
            ZoomCacheHeader zHdr;
            memset(&zHdr, 0, sizeof(zHdr));
            zHdr.magic      = 0x5A4F4F4D;
            zHdr.sig        = sig;
            zHdr.frameCount = (uint16_t)decodedFrames;
            zHdr.zoomSrcX   = dlZoomX;
            zHdr.zoomSrcY   = dlZoomY;
            zHdr.stationId  = dlStation;
            memcpy(zHdr.palette, g_dlPalette, sizeof(zHdr.palette));
            zoomOutFile.write((const uint8_t*)&zHdr, sizeof(zHdr));
            zoomOutFile.close();

            char zPath[48];
            getSdZoomPath(dlStation, zPath, sizeof(zPath));
            if (SD.exists(zPath)) SD.remove(zPath);
            if (SD.rename(SD_TMP_ZOOM_PATH, zPath)) {
                sdLog("[SD-ZOOM] Cached 12-frame HD Zoom for station %u (%s)\n", (unsigned)dlStation, kStations[dlStation].shortCode);
            }
        } else {
            zoomOutFile.close();
            SD.remove(SD_TMP_ZOOM_PATH);
        }
    }

    // SD-2 & SD-4: Save latest frame to history and log rain statistics
    if (g_sdReady && decodedFrames >= 1) {
        saveFrameToHistory(bank, decodedFrames - 1, dlStation, rainPct);
        recordRainStatsCsv(dlStation, rainPct, rainPixels);
    }

    g_bankErasedTo[bank] = 0;
    g_lastOkUpdateMs     = millis();
    g_readyBank          = bank;
    g_bankReady          = true;
    return DL_COMMITTED;
}

// ============================================================================
// HTTPS Download (+ tee raw GIF to /radar/tmp.gif on SD, then promote to sta_<id>.gif)
// ============================================================================
static int downloadAndDecodeBank(int bank) {
    g_dlStage     = DL_CONNECTING;
    g_dlFrames    = 0;
    g_partialBank = false;

    uint16_t dlZoomX   = g_targetZoomX;
    uint16_t dlZoomY   = g_targetZoomY;
    uint8_t  dlStation = g_targetStation % NUM_STATIONS;
    bool     forceCrop = g_forceRecrop;
    const RadarStationInfo& sta = kStations[dlStation];

    WiFiClientSecure client;
    client.setInsecure();
    client.setTimeout(15);

    HTTPClient http;
    http.setReuse(false);
    String url = String(sta.url) + "?t=" + String(millis());
    Serial.printf("[HTTP] GET %s (sta=%s, zoom=%u,%u, heap=%u, max=%u)\n",
                  url.c_str(), sta.shortCode, dlZoomX, dlZoomY, ESP.getFreeHeap(), ESP.getMaxAllocHeap());
    if (!http.begin(client, url)) {
        Serial.println("[HTTP] http.begin failed");
        g_lastHttpErr = -100;
        return DL_FAIL;
    }

    http.addHeader("Referer", sta.referer);
    http.addHeader("User-Agent", "Mozilla/5.0 (ESP32-CYD Radar Display)");
    const char* headerKeys[] = {"Transfer-Encoding", "Content-Type"};
    http.collectHeaders(headerKeys, 2);
    http.setTimeout(15000);

    int httpCode = http.GET();
    if (httpCode != HTTP_CODE_OK) {
        Serial.printf("[HTTP] GET failed, code: %d\n", httpCode);
        g_lastHttpErr = httpCode;
        http.end();
        return DL_FAIL;
    }

    // Allocate decode workspace AFTER TLS handshake has succeeded
    g_ws = (GifDecodeWorkspace*)malloc(sizeof(GifDecodeWorkspace));
    if (!g_ws) {
        Serial.printf("[MEM] Workspace alloc failed (heap %u, max %u)\n",
                      ESP.getFreeHeap(), ESP.getMaxAllocHeap());
        g_lastHttpErr = -200;
        http.end();
        return DL_FAIL;
    }

    // Open SD tee file only AFTER http.GET() succeeded (keeps TLS heap free)
    File teeFile;
    if (g_sdReady) {
        if (SD.exists(SD_TMP_GIF_PATH)) SD.remove(SD_TMP_GIF_PATH);
        teeFile = SD.open(SD_TMP_GIF_PATH, FILE_WRITE);
        if (!teeFile) sdLog("[SD] Cannot open %s for writing\n", SD_TMP_GIF_PATH);
    }

    bool isChunked = http.header("Transfer-Encoding").equalsIgnoreCase("chunked");
    GifByteStream stream(http.getStreamPtr(), isChunked, teeFile ? &teeFile : nullptr);

    int result = decodeGifStream(stream, bank, dlZoomX, dlZoomY, dlStation, forceCrop, true, DL_DOWNLOADING);
    int frames = g_dlFrames;
    bool teeOk = teeFile && stream.teeOk();

    free(g_ws);
    g_ws = nullptr;
    http.end();

    if (teeFile) {
        teeFile.close();
        if (result == DL_COMMITTED && frames >= MAX_RADAR_FRAMES && teeOk) {
            char path[48];
            getSdGifPath(dlStation, path, sizeof(path));
            if (SD.exists(path)) SD.remove(path);
            if (SD.rename(SD_TMP_GIF_PATH, path)) {
                sdLog("[SD] Cached %s\n", path);
            } else {
                sdLog("[SD] Rename to %s failed\n", path);
            }
        } else {
            SD.remove(SD_TMP_GIF_PATH);
        }
    }

    if (result == DL_COMMITTED || result == DL_UNCHANGED) {
        g_lastHttpOkMs[dlStation] = millis();
    }
    return result;
}

// ============================================================================
// Fast offline recrop from SD cached GIF (/radar/sta_<id>.gif)
// ============================================================================
static int recropFromSdCard(int bank) {
    uint8_t dlStation = g_targetStation % NUM_STATIONS;
    char path[48];
    getSdGifPath(dlStation, path, sizeof(path));
    if (!g_sdReady || !SD.exists(path)) return DL_FAIL;

    File f = SD.open(path, FILE_READ);
    if (!f) return DL_FAIL;

    g_ws = (GifDecodeWorkspace*)malloc(sizeof(GifDecodeWorkspace));
    if (!g_ws) {
        f.close();
        return DL_FAIL;
    }

    g_dlFrames    = 0;
    g_partialBank = false;
    g_dlStage     = DL_SD_RECROP;
    uint32_t t0   = millis();
    sdLog("[SD] Recrop %s (zoom=%u,%u)\n", path, (unsigned)g_targetZoomX, (unsigned)g_targetZoomY);

    GifByteStream stream(&f);
    int result = decodeGifStream(stream, bank, g_targetZoomX, g_targetZoomY, dlStation, true, false, DL_SD_RECROP);

    free(g_ws);
    g_ws = nullptr;
    f.close();
    sdLog("[SD] Recrop result %d in %lu ms\n", result, (unsigned long)(millis() - t0));
    return result;
}

// ============================================================================
// Background Pre-Caching to SD (/ly-radar/cache/sta_<id>.gif)
// Streams GIF directly to SD card with 1 KB stack buffer - zero DRAM overhead!
// ============================================================================
static bool prefetchStationToSd(uint8_t stationId) {
    if (!g_sdReady || WiFi.status() != WL_CONNECTED) return false;
    if (stationId >= NUM_STATIONS) return false;
    const RadarStationInfo& sta = kStations[stationId];

    char path[48];
    getSdGifPath(stationId, path, sizeof(path));

    WiFiClientSecure client;
    client.setInsecure();
    client.setTimeout(12);

    HTTPClient http;
    http.setReuse(false);
    String url = String(sta.url) + "?t=" + String(millis());
    if (!http.begin(client, url)) return false;

    http.addHeader("Referer", sta.referer);
    http.addHeader("User-Agent", "Mozilla/5.0 (ESP32-CYD Radar Display)");
    http.setTimeout(12000);

    int httpCode = http.GET();
    if (httpCode != HTTP_CODE_OK) {
        http.end();
        return false;
    }

    if (SD.exists(SD_TMP_GIF_PATH)) SD.remove(SD_TMP_GIF_PATH);
    File f = SD.open(SD_TMP_GIF_PATH, FILE_WRITE);
    if (!f) {
        http.end();
        return false;
    }

    WiFiClient* stream = http.getStreamPtr();
    uint8_t buf[1024];
    size_t totalBytes = 0;
    uint32_t startMs = millis();
    bool abort = false;

    while (http.connected() && (stream->available() > 0 || totalBytes == 0) && (millis() - startMs < 25000)) {
        if (g_forceRecrop || ulTaskNotifyTake(pdTRUE, 0)) {
            abort = true;
            break;
        }
        int avail = stream->available();
        if (avail > 0) {
            int toRead = min(avail, (int)sizeof(buf));
            int r = stream->readBytes(buf, toRead);
            if (r > 0) {
                f.write(buf, r);
                totalBytes += r;
            }
        } else {
            vTaskDelay(pdMS_TO_TICKS(15));
        }
    }

    f.close();
    http.end();

    if (!abort && totalBytes > 30000) {
        if (SD.exists(path)) SD.remove(path);
        if (SD.rename(SD_TMP_GIF_PATH, path)) {
            g_lastHttpOkMs[stationId] = millis();
            sdLog("[SD] Background pre-fetched %s (%u KB)\n", sta.shortCode, (unsigned)(totalBytes / 1024));
            return true;
        }
    } else {
        SD.remove(SD_TMP_GIF_PATH);
    }
    return false;
}

// ============================================================================
// Open-Meteo Live Weather Fetcher (Core 0 background task)
// ============================================================================
static uint32_t g_lastWeatherAttemptMs = 0;

static void fetchStationWeather(uint8_t stationId) {
    if (WiFi.status() != WL_CONNECTED || stationId >= NUM_STATIONS) return;

    g_lastWeatherAttemptMs = millis();

    // Guard heap: ensure >= 20 KB heap for HTTP client
    if (ESP.getMaxAllocHeap() < 20000) {
        sdLog("[WEATHER] Contiguous heap too low (%u), deferring\n", ESP.getMaxAllocHeap());
        return;
    }

    char url[200];
    snprintf(url, sizeof(url),
             "http://api.open-meteo.com/v1/forecast?latitude=%.4f&longitude=%.4f&current=temperature_2m,relative_humidity_2m,weather_code",
             kStations[stationId].lat, kStations[stationId].lon);

    sdLog("[WEATHER] Querying %s (%.2f, %.2f)...\n",
          kStations[stationId].shortCode, kStations[stationId].lat, kStations[stationId].lon);

    WiFiClient wClient;
    wClient.setTimeout(4);

    HTTPClient http;
    http.begin(wClient, url);
    http.setTimeout(4000);
    http.setUserAgent("ESP32-LyRadar/1.3.0");

    int code = http.GET();
    if (code == HTTP_CODE_OK) {
        String payload = http.getString();
        const char* pCur = strstr(payload.c_str(), "\"current\":");
        if (pCur) {
            const char* pTemp = strstr(pCur, "\"temperature_2m\":");
            if (pTemp) {
                g_stationTemp = (float)atof(pTemp + 17);
            }
            const char* pHum = strstr(pCur, "\"relative_humidity_2m\":");
            if (pHum) {
                g_stationHumidity = (int8_t)atoi(pHum + 23);
            }
            const char* pCode = strstr(pCur, "\"weather_code\":");
            if (pCode) {
                g_weatherCode = (int16_t)atoi(pCode + 15);
            }
            g_weatherStation = stationId;
            g_lastWeatherMs  = millis();
            sdLog("[WEATHER] OK %s: Temp=%.1f C, Hum=%d %%, Code=%d\n",
                  kStations[stationId].shortCode, g_stationTemp, (int)g_stationHumidity, (int)g_weatherCode);
        }
    } else {
        sdLog("[WEATHER] HTTP error %d\n", code);
    }
    http.end();
}

// ============================================================================
// FreeRTOS Download Task (Core 0)
// ============================================================================
static void downloadTask(void*) {
    uint32_t waitMs = 0;
    for (;;) {
        if (waitMs) ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(waitMs));
        while (g_bankReady) vTaskDelay(pdMS_TO_TICKS(20));

        int result = DL_FAIL;
        int target = (g_activeBank >= 0) ? (1 - g_activeBank) : 0;
        g_dlTargetBank = target;

        // 1. SD Card fast path: new zoom crop / station switch decoded locally (no network)
        bool sdServed = false;
        if (g_sdReady && g_forceRecrop) {
            setCpuFrequencyMhz(240);
            setStatusLed(false, true, true);
            result = recropFromSdCard(target);
            setCpuFrequencyMhz(80);
            if (result == DL_COMMITTED) {
                g_forceRecrop = false;
                sdServed      = true;
                while (g_bankReady) vTaskDelay(pdMS_TO_TICKS(20)); // wait for UI to activate it
                target         = (g_activeBank >= 0) ? (1 - g_activeBank) : 0;
                g_dlTargetBank = target;
            }
        }

        // 2. HTTPS download (skipped when SD copy was fresh)
        if (!sdServed) {
            if (WiFi.status() != WL_CONNECTED) {
                WiFi.reconnect();
                for (int i = 0; i < 40 && WiFi.status() != WL_CONNECTED; i++) vTaskDelay(pdMS_TO_TICKS(250));
            }
            if (WiFi.status() == WL_CONNECTED) {
                setCpuFrequencyMhz(240);
                esp_wifi_set_ps(WIFI_PS_NONE);
                setStatusLed(false, false, true);
                result = downloadAndDecodeBank(target);
                esp_wifi_set_ps(WIFI_PS_MIN_MODEM);
                setCpuFrequencyMhz(80);
            } else {
                g_lastHttpErr = -50;
                if (result != DL_COMMITTED) result = DL_FAIL;
            }
        }

        g_dlStage = (result == DL_FAIL && g_activeBank < 0) ? DL_ERROR : DL_IDLE;
        g_dlZoomReadyFrames = 0;
        setStatusLed(result == DL_FAIL, false, false);

        if (result == DL_FAIL && g_autoCycle && g_targetStation != g_bankStation && g_activeBank >= 0) {
            sdLog("[AUTO] Station %u failed, resuming active station %u\n", (unsigned)g_targetStation, (unsigned)g_bankStation);
            g_targetStation = g_bankStation;
            g_lastStationCycleMs = millis();
        }

        if (!g_forceRecrop) {
            if (result == DL_COMMITTED) {
                while (g_bankReady) vTaskDelay(pdMS_TO_TICKS(20));
                preEraseInactiveBank(1 - g_activeBank);
            } else if (result == DL_UNCHANGED && g_activeBank >= 0 && g_bankErasedTo[target] == 0) {
                preEraseInactiveBank(target);
            }
        }

        // 3. Update weather for target station if stale (> 15 min), changed station, or not yet fetched
        if (WiFi.status() == WL_CONNECTED && !g_forceRecrop) {
            bool weatherStale = (g_weatherStation != g_targetStation) ||
                                (millis() - g_lastWeatherMs >= 15UL * 60UL * 1000UL) ||
                                (g_stationTemp < -90.0f);
            bool canAttempt   = (millis() - g_lastWeatherAttemptMs >= 60000UL);
            if (weatherStale && canAttempt) {
                fetchStationWeather(g_targetStation);
            }
        }

        waitMs = (result == DL_FAIL || g_partialBank) ? RETRY_INTERVAL_MS : UPDATE_INTERVAL_MS;
        if (g_forceRecrop) waitMs = 300;
        sdLog("[UPDATE] Result %d (partial=%d, sd=%d), next check in %lu s (heap %u, max %u)\n",
              result, (int)g_partialBank, (int)sdServed, (unsigned long)(waitMs / 1000),
              ESP.getFreeHeap(), ESP.getMaxAllocHeap());

        // During idle wait: pre-cache other stations to SD (1 station every 15s)
        if (waitMs > 1000 && !g_forceRecrop) {
            uint32_t idleStart = millis();
            static uint8_t nextPrefetch = 0;
            while (millis() - idleStart < waitMs) {
                if (g_forceRecrop || ulTaskNotifyTake(pdTRUE, 0)) break;

                // 1. Pre-cache 12-frame HD Zoom on SD if not yet ready for current target station
                if (g_sdReady && !g_forceRecrop) {
                    char targetZPath[48];
                    getSdZoomPath(g_targetStation, targetZPath, sizeof(targetZPath));
                    if (!isSdZoomValid(targetZPath, g_targetStation, g_targetZoomX, g_targetZoomY)) {
                        char rawGifPath[48];
                        getSdGifPath(g_targetStation, rawGifPath, sizeof(rawGifPath));
                        if (SD.exists(rawGifPath)) {
                            sdLog("[SD-ZOOM] Pre-caching 12-frame HD Zoom in background (%s)\n", kStations[g_targetStation].shortCode);
                            setCpuFrequencyMhz(240);
                            recropFromSdCard(target);
                            setCpuFrequencyMhz(80);
                        }
                    }
                }

                // Refresh weather during idle if station changed or stale (at most once per 60s)
                if (WiFi.status() == WL_CONNECTED) {
                    bool weatherStale = (g_weatherStation != g_targetStation) ||
                                        (millis() - g_lastWeatherMs >= 15UL * 60UL * 1000UL) ||
                                        (g_stationTemp < -90.0f);
                    bool canAttempt   = (millis() - g_lastWeatherAttemptMs >= 60000UL);
                    if (weatherStale && canAttempt) {
                        esp_wifi_set_ps(WIFI_PS_NONE);
                        fetchStationWeather(g_targetStation);
                        esp_wifi_set_ps(WIFI_PS_MIN_MODEM);
                    }
                }

                // Try prefetching next station if SD ready & WiFi connected
                if (g_sdReady && WiFi.status() == WL_CONNECTED) {
                    nextPrefetch = (nextPrefetch + 1) % NUM_STATIONS;
                    if (nextPrefetch != g_targetStation) {
                        uint32_t age = (g_lastHttpOkMs[nextPrefetch] > 0) ? (millis() - g_lastHttpOkMs[nextPrefetch]) : 0xFFFFFFFFUL;
                        char path[48];
                        getSdGifPath(nextPrefetch, path, sizeof(path));
                        if (!SD.exists(path) || age > 12UL * 60UL * 1000UL) {
                            setCpuFrequencyMhz(240);
                            esp_wifi_set_ps(WIFI_PS_NONE);
                            prefetchStationToSd(nextPrefetch);
                            esp_wifi_set_ps(WIFI_PS_MIN_MODEM);
                            setCpuFrequencyMhz(80);
                        }
                    }
                }

                // Sleep in short 300ms intervals so user interaction breaks out immediately
                for (int s = 0; s < 50; s++) { // 50 * 300ms = 15s between prefetch attempts
                    if (g_forceRecrop || ulTaskNotifyTake(pdTRUE, 0)) break;
                    vTaskDelay(pdMS_TO_TICKS(300));
                }
            }
            waitMs = 0; // wait handled!
        }
    }
}

static void requestRadarUpdate() {
    if (g_dlTask) xTaskNotifyGive(g_dlTask);
}
