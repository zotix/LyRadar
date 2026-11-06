# ESP32-2432S028 (CYD) Hardware Specifications

## 1. Pinout Map

| Function | Pin (GPIO) | Bus / Peripheral | Notes |
|---|---|---|---|
| **TFT Display** | | | ST7789 (2-USB) / ILI9341 (1-USB) |
| TFT MOSI | GPIO 13 | HSPI | 40 MHz SPI Frequency |
| TFT MISO | GPIO 12 | HSPI | |
| TFT SCLK | GPIO 14 | HSPI | |
| TFT CS | GPIO 15 | HSPI | Active LOW |
| TFT DC | GPIO 2 | HSPI | Data/Command select |
| TFT Backlight | GPIO 21 & GPIO 27 | LEDC Channel 0 | 5 kHz PWM dimming |
| **MicroSD Card** | | | VSPI Bus |
| SD CS | GPIO 5 | VSPI | Active LOW |
| SD SCK | GPIO 18 | VSPI | 16 MHz SPI |
| SD MISO | GPIO 19 | VSPI | |
| SD MOSI | GPIO 23 | VSPI | |
| **XPT2046 Touch** | | | Dedicated Bit-Banged SPI |
| Touch CS | GPIO 33 | Bit-Bang | Active LOW |
| Touch CLK | GPIO 25 | Bit-Bang | |
| Touch MOSI | GPIO 32 | Bit-Bang | |
| Touch MISO | GPIO 39 | Bit-Bang | Input only |
| Touch IRQ | GPIO 36 | Bit-Bang | Input only |
| **Peripherals** | | | |
| BOOT Button | GPIO 0 | Input Pullup | Active LOW |
| LDR Light Sensor | GPIO 34 | ADC1_CH6 | 0 (bright) .. 4095 (dark) |
| RGB LED (Red) | GPIO 4 | Digital Output | Active LOW |
| RGB LED (Green) | GPIO 16 | Digital Output | Active LOW |
| RGB LED (Blue) | GPIO 17 | Digital Output | Active LOW |

---

## 2. Flash Partition Layout (4MB Flash)

Defined in [`partitions.csv`](../partitions.csv):
- `nvs`: 0x9000 (20 KB)
- `otadata`: 0x2000 (8 KB)
- `app0`: 0x130000 (1.21 MB) — Firmware binary
- `radar_cache`: 0x2C0000 (2.75 MB, Subtype 0x99) — Double-buffered Flash Cache:
  - Bank 0: 0x000000..0x153000 (12 Full Frames + 6 Zoom Frames + Header)
  - Bank 1: 0x153000..0x2A6000 (12 Full Frames + 6 Zoom Frames + Header)
  - Scratch: 0x2A6000..0x2C0000 (Temporary recrop buffer)

---

## 3. MicroSD File Structure (`/ly-radar/`)

- `/ly-radar/cache/`: Cached animated GIF files (`sta_0.gif` .. `sta_11.gif`)
- `/ly-radar/zoom/`: Cached 12-frame HD Zoom binary files (`sta_<id>.z12`)
- `/ly-radar/hist/`: Historical radar captures (`sta_<id>/YYYYMMDD_HHMM.rad`)
- `/ly-radar/wifi.txt`: Automatic WiFi configuration (auto-deleted to `wifi.done`)
- `/ly-radar/log.txt`: System log & reset diagnostics
