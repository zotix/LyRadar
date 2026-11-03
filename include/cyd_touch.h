#pragma once

#include <Arduino.h>
#include <Preferences.h>
#include <math.h>
#include "config.h"

// ============================================================================
// LyRadar CYD Touch Driver - XPT2046 Bit-Banged SPI & 3-Point Affine Matrix
// Copyright (c) 2026 Zotix. All Rights Reserved.
// Mathematical calibration based on Carlos E. Vidales (2001) public domain standard.
// ============================================================================
static constexpr uint32_t TOUCH_CAL_MAGIC = 0x43594431UL;

class CYDTouch {
public:
    static constexpr uint8_t  CMD_READ_X          = 0xD0;
    static constexpr uint8_t  CMD_READ_Y          = 0x90;
    static constexpr uint8_t  CMD_READ_Z1         = 0xB0;
    static constexpr uint8_t  CMD_READ_Z2         = 0xC0;
    static constexpr uint16_t PRESSURE_THRESHOLD  = 350;

    struct RawTouch {
        bool     down     = false;
        int16_t  x        = 0;
        int16_t  y        = 0;
        uint16_t pressure = 0;
    };

    struct TouchCalibration {
        float    ax    = 0.0f;
        float    bx    = 0.0f;
        float    cx    = 0.0f;
        float    ay    = 0.0f;
        float    by    = 0.0f;
        float    cy    = 0.0f;
        uint32_t magic = 0;
    };

    TouchCalibration cal;
    uint16_t lastZ1       = 0;
    uint16_t lastZ2       = 0;
    uint16_t lastPressure = 0;
    int16_t  lastRawX     = 0;
    int16_t  lastRawY     = 0;

    void begin() {
        pinMode(XPT2046_MOSI, OUTPUT);
        pinMode(XPT2046_MISO, INPUT);
        pinMode(XPT2046_CLK,  OUTPUT);
        pinMode(XPT2046_CS,   OUTPUT);
        pinMode(XPT2046_IRQ,  INPUT);
        digitalWrite(XPT2046_CS,  HIGH);
        digitalWrite(XPT2046_CLK, LOW);
    }

    bool hasCalibration() const {
        return cal.magic == TOUCH_CAL_MAGIC;
    }

    bool loadCalibration(Preferences& prefs) {
        if (prefs.getBytesLength("touchCal") != sizeof(TouchCalibration)) {
            cal = TouchCalibration{};
            return false;
        }
        prefs.getBytes("touchCal", &cal, sizeof(TouchCalibration));
        if (cal.magic != TOUCH_CAL_MAGIC) {
            cal = TouchCalibration{};
            return false;
        }
        return true;
    }

    void saveCalibration(Preferences& prefs) {
        prefs.putBytes("touchCal", &cal, sizeof(TouchCalibration));
    }

    uint16_t readTouchAdc(uint8_t command) {
        uint16_t raw = 0;
        digitalWrite(XPT2046_CS, LOW);

        for (int8_t bit = 7; bit >= 0; --bit) {
            digitalWrite(XPT2046_MOSI, (command & (1 << bit)) ? HIGH : LOW);
            digitalWrite(XPT2046_CLK, HIGH);
            delayMicroseconds(2);
            digitalWrite(XPT2046_CLK, LOW);
            delayMicroseconds(2);
        }

        for (uint8_t bit = 0; bit < 16; ++bit) {
            digitalWrite(XPT2046_CLK, HIGH);
            delayMicroseconds(2);
            raw = static_cast<uint16_t>((raw << 1) | (digitalRead(XPT2046_MISO) ? 1 : 0));
            digitalWrite(XPT2046_CLK, LOW);
            delayMicroseconds(2);
        }

        digitalWrite(XPT2046_CS, HIGH);
        return static_cast<uint16_t>((raw >> 3) & 0x0FFF);
    }

    RawTouch readRawTouch() {
        RawTouch t;
        const uint16_t z1 = readTouchAdc(CMD_READ_Z1);
        const uint16_t z2 = readTouchAdc(CMD_READ_Z2);
        uint16_t pressure = 0;
        if (z1 > 15 && z2 > z1) {
            pressure = static_cast<uint16_t>(z1 + 4095 - z2);
        }
        lastZ1       = z1;
        lastZ2       = z2;
        lastPressure = pressure;

        if (pressure < PRESSURE_THRESHOLD) {
            return t;
        }

        uint32_t x = 0;
        uint32_t y = 0;
        constexpr uint8_t samples = 5;
        for (uint8_t i = 0; i < samples; ++i) {
            x += readTouchAdc(CMD_READ_X);
            y += readTouchAdc(CMD_READ_Y);
            delayMicroseconds(200);
        }

        t.down     = true;
        t.x        = static_cast<int16_t>(x / samples);
        t.y        = static_cast<int16_t>(y / samples);
        t.pressure = pressure;
        lastRawX   = t.x;
        lastRawY   = t.y;
        return t;
    }

    bool isTouching() {
        return readRawTouch().down;
    }

    bool waitForStableRaw(int16_t& rawX, int16_t& rawY) {
        // Wait for any previous touch to be released first
        uint32_t relDeadline = millis() + 2000UL;
        while (readRawTouch().down && millis() < relDeadline) {
            delay(20);
        }

        const uint32_t deadline = millis() + 25000UL;
        while (millis() < deadline) {
            if (digitalRead(BOOT_BTN_PIN) == LOW) return false;
            RawTouch first = readRawTouch();
            if (!first.down) {
                delay(20);
                continue;
            }

            uint32_t sumX  = 0;
            uint32_t sumY  = 0;
            uint8_t  count = 0;
            for (uint8_t i = 0; i < 8; ++i) {
                RawTouch sample = readRawTouch();
                if (sample.down) {
                    sumX += sample.x;
                    sumY += sample.y;
                    ++count;
                }
                delay(5);
            }
            if (count >= 4) {
                rawX = static_cast<int16_t>(sumX / count);
                rawY = static_cast<int16_t>(sumY / count);
                while (readRawTouch().down && millis() < deadline) {
                    delay(15);
                }
                return true;
            }
        }
        return false;
    }

    static bool computeAffine(const int16_t raw[3][2], const int16_t screen[3][2], TouchCalibration& out) {
        const float x0 = raw[0][0], y0 = raw[0][1];
        const float x1 = raw[1][0], y1 = raw[1][1];
        const float x2 = raw[2][0], y2 = raw[2][1];
        const float denom = x0 * (y1 - y2) + x1 * (y2 - y0) + x2 * (y0 - y1);
        if (fabsf(denom) < 0.001f) {
            return false;
        }

        const float sx0 = screen[0][0], sy0 = screen[0][1];
        const float sx1 = screen[1][0], sy1 = screen[1][1];
        const float sx2 = screen[2][0], sy2 = screen[2][1];

        out.ax    = (sx0 * (y1 - y2) + sx1 * (y2 - y0) + sx2 * (y0 - y1)) / denom;
        out.bx    = (sx0 * (x2 - x1) + sx1 * (x0 - x2) + sx2 * (x1 - x0)) / denom;
        out.cx    = (sx0 * (x1 * y2 - x2 * y1) + sx1 * (x2 * y0 - x0 * y2) + sx2 * (x0 * y1 - x1 * y0)) / denom;
        out.ay    = (sy0 * (y1 - y2) + sy1 * (y2 - y0) + sy2 * (y0 - y1)) / denom;
        out.by    = (sy0 * (x2 - x1) + sy1 * (x0 - x2) + sy2 * (x1 - x0)) / denom;
        out.cy    = (sy0 * (x1 * y2 - x2 * y1) + sy1 * (x2 * y0 - x0 * y2) + sy2 * (x0 * y1 - x1 * y0)) / denom;
        out.magic = TOUCH_CAL_MAGIC;
        return true;
    }

    bool readTouch(int &screenX, int &screenY, bool flip180 = false, uint8_t /*mapMode*/ = 0) {
        RawTouch raw = readRawTouch();
        if (!raw.down) return false;

        int16_t lx = 0, ly = 0;
        if (cal.magic == TOUCH_CAL_MAGIC) {
            const float mappedX = cal.ax * raw.x + cal.bx * raw.y + cal.cx;
            const float mappedY = cal.ay * raw.x + cal.by * raw.y + cal.cy;
            lx = constrain(static_cast<int16_t>(lroundf(mappedX)), 0, DST_WIDTH - 1);
            ly = constrain(static_cast<int16_t>(lroundf(mappedY)), 0, DST_HEIGHT - 1);
        } else {
            // Default fallback before 3-point calibration is run
            lx = constrain(map(raw.x, 250, 3800, 0, DST_WIDTH - 1),  0, DST_WIDTH - 1);
            ly = constrain(map(raw.y, 250, 3800, 0, DST_HEIGHT - 1), 0, DST_HEIGHT - 1);
        }

        if (flip180) {
            lx = (DST_WIDTH - 1)  - lx;
            ly = (DST_HEIGHT - 1) - ly;
        }

        screenX = lx;
        screenY = ly;
        return true;
    }
};

static CYDTouch touch;
