<div align="center">

# LyRadar CYD 🌦️📡

**Real-Time Animated Weather Radar Display for ESP32-2432S028 (Cheap Yellow Display - CYD)**

[ 🇬🇧 English ](README.md) | [ 🇹🇭 ภาษาไทย ](README_TH.md)

[![License: Non-Commercial](https://img.shields.io/badge/License-Non--Commercial-red.svg)](LICENSE)
[![Platform: ESP32](https://img.shields.io/badge/Platform-ESP32--2432S028-yellow.svg)](https://docs.platformio.org/page/boards/espressif32/esp32dev.html)
[![Framework: Arduino](https://img.shields.io/badge/Framework-Arduino-blue.svg)](https://www.arduino.cc/)
[![Version: v1.3.0](https://img.shields.io/badge/Version-v1.3.0-green.svg)](https://github.com/zotix/LyRadar/releases)

</div>

---

<div align="center">
  <img src="docs/images/cyd_board_v130.jpg" width="48%" alt="LyRadar CYD Live Radar View" />
  <img src="docs/images/cyd_board_v130_2.jpg" width="48%" alt="LyRadar CYD Touch Menu & Playlist" />
  <p><em>Live weather radar animation (TMD-SVP Suvarnabhumi 240km) and 12-station touch menu with custom playlist filter on the ESP32-2432S028 (CYD) board.</em></p>
</div>

---

## 📖 Overview

**LyRadar CYD** is a specialized, high-performance weather radar animation firmware designed for the popular **ESP32-2432S028 (CYD)** 2.8" color touch display.

Engineered at the bare-metal level using **Dual-Core FreeRTOS**, **Double-Buffered Flash Cache (2.75 MB)**, and **MicroSD Card Pre-Caching**, it delivers buttery-smooth 12-frame radar loop playback at 350ms per frame with zero screen tearing and instant station transitions. Version 1.3.0 brings **Deep Power Optimization**, reducing overall current draw by ~65% while keeping the ESP32 running cool at ambient temperature.

---

## ✨ Key Features

- 📡 **12 Weather Radar Stations Across Thailand**:
  - **Bangkok Metropolitan (BMA)**: Nong Khaem (`NK`), Nong Chok (`NC`)
  - **Thai Meteorological Department (TMD)**: Suvarnabhumi 240km (`SVP`), Samut Songkhram (`SKM`), Rayong (`RYG`), Chiang Rai (`CRI`), Khon Kaen (`KKN`), Ubon Ratchathani (`UBN`), Phitsanulok (`PHS`), Surat Thani (`SRT`), Phuket (`PKT`), Sathing Phra Songkhla (`STP`)
- 🔍 **Full 12-Frame HD Zoom & Instant Digital Zoom**:
  - Tap anywhere on the radar map to instantly zoom into that exact location.
  - Plays all 12 frames (`1/12` to `12/12`) smoothly at 350ms with no frame jumping.
  - Background worker automatically pre-caches 1:1 HD zoom binary files (`.z12`) to the MicroSD card during idle time for pixel-perfect sharpness.
- 🌤️ **Top HUD Live Weather Bar**:
  - Fetches real-time weather data from Open-Meteo API using station coordinates.
  - Custom pixel micro-icons (☀️, ⛅, 🌧️, ⛈️), ambient temperature, humidity, and rain echo percentage with intensity badges (`L`, `M`, `H`).
  - Rendered inside the 16-line sprite buffer every 350ms for zero-flicker graphics.
- 🎯 **Station Playlist Filter (Yellow Borders)**:
  - Long-press (≥380ms) on any station button in the menu to add or remove it from your custom playback playlist (highlighted with double yellow borders).
  - AUTO mode cycles exclusively through selected playlist stations and displays the count directly on the `AUTO` button.
- 🔘 **2-Line Smart Action Buttons**:
  - `AUTO` button: Shows cycling status and station count (`AUTO 5`, `AUTO ALL`, `AUTO OFF`).
  - `BRI` button: Displays current brightness level (`AUTO`, `100%`, `55%`, `20%`).
- 🚀 **Background Pre-Caching to MicroSD**:
  - Core 0 silently downloads and stores upcoming radar loops in `/ly-radar/cache/sta_<id>.gif`.
  - Switching between stations occurs almost instantaneously (**< 0.3s**) directly from the SD card without network delays.
- 🔄 **Kiosk Slideshow AUTO Mode**:
  - Automatically advances through radar stations every 30 seconds.
  - Automatically pauses on touch or zoom and resumes via a 3-minute inactivity watchdog.
- 🍃 **Deep Power Optimization (v1.3.0)**:
  - **WiFi Modem-Sleep (`WIFI_PS_MIN_MODEM`)**: Radio sleeps between AP DTIM beacon intervals, cutting WiFi idle draw from ~100 mA down to **~15–20 mA** without dropping connection.
  - **Hybrid Clock Scaling (80 ↔ 240 MHz)**: Runs at 80 MHz base clock during playback and standby (maintaining full 80 MHz APB bus for high-speed SPI display transfers) and boosts dynamically to 240 MHz during TLS downloads and GIF decompression.
  - **Smart Eco Standby Dimming**: Automatically dims backlight to duty 30 (~12%) after 3 minutes of inactivity when no rain is detected (`rain < 1%`). Wakes immediately on touch, BOOT button press, or rain detection (`rain >= 1%`).
  - **~65% Total Operational Power Reduction**: Operating power drops from ~250–280 mA to **~75–90 mA @ 5V**, keeping the board completely cool.
- ⌨️ **On-Screen WiFi Scanner & 5-Row Touch Keyboard**:
  - Scans 2.4 GHz WiFi networks with an intuitive 5-row touch keyboard featuring a dedicated `0–9` number row.
  - Supports automated WiFi setup via `/ly-radar/wifi.txt` on the MicroSD card.
- 🔘 **Physical BOOT Button (GPIO 0) Shortcuts**:
  - Short press (< 3s): Close open menu or rotate screen 0° / 180°.
  - Long press (3s..9s): Launch 3-point touch screen calibration.
  - Extra-long press (≥ 10s): Toggle screen color inversion.

---

## 🛠️ Supported Hardware

- **ESP32-2432S028** (Cheap Yellow Display - CYD 2.8" 320×240 TFT Touch)
  - 2-USB version (ST7789 display controller, XPT2046 touch) — *Default environment: `cyd_2usb_st7789`*
  - 1-USB version (ILI9341 display controller, XPT2046 touch) — *Environment: `cyd_1usb_ili9341`*
- **MicroSD Card**: Formatted as FAT32, inserted into the rear slot (connected via VSPI bus).

### 🛒 Where to Buy Hardware (Shopee & Lazada)

- 📟 **ESP32-2432S028 CYD Board (2.8" Touch)**:
  - Shopee: https://s.shopee.co.th/6VPDgpaRX9
  - Lazada: https://s.lazada.co.th/s.ZnQhuQ
- 🖨️ **Enclosure Case**:
  - Shopee: https://s.shopee.co.th/7AeuU66ION
  - Lazada: https://s.lazada.co.th/s.ZnQhHR
- 💾 **MicroSD Card (FAT32)**:
  - Shopee: https://s.shopee.co.th/1gJxwPmLya
  - Lazada: https://s.lazada.co.th/s.ZnQhrR

See [docs/hardware_notes.md](docs/hardware_notes.md) for full GPIO pinout mappings.

---

## 📦 MicroSD Card Directory Structure (`/ly-radar/`)

```text
SD Card Root/
└── ly-radar/
    ├── wifi.txt          # Automatic WiFi configuration (Line 1 = SSID, Line 2 = Password)
    ├── log.txt           # Boot diagnostics and runtime logs
    ├── rain.csv          # Accumulated rain statistics
    ├── zoom/             # Cached 12-frame 1:1 HD zoom binaries (sta_<id>.z12)
    └── cache/            # Cached full-frame radar loops (sta_0.gif .. sta_11.gif)
```

---

## 💻 Building and Flashing

This project is built using **PlatformIO**. Install PlatformIO IDE (VS Code extension) or PlatformIO Core CLI:

```bash
# 1. Clone the repository
git clone https://github.com/zotix/LyRadar.git
cd LyRadar

# 2. Build and upload for 2-USB model (ST7789)
pio run -e cyd_2usb_st7789 -t upload

# Or build and upload for 1-USB model (ILI9341)
pio run -e cyd_1usb_ili9341 -t upload
```

---

## 📜 License Terms

Software copyright is owned by **© 2026 Zotix. All Rights Reserved.**

### 1. Personal / Non-Commercial Use — 100% Free
Individuals are free to download, modify, compile, and flash this firmware onto their own hardware for personal, non-commercial, and educational purposes.

### 2. Commercial Use — ⚠️ License Required
**Commercial use, including pre-flashing onto boards for commercial sale (e.g. Shopee, Lazada, TikTok Shop, Amazon, retail stores) or any monetization, is strictly prohibited without written consent.**

> **Interested in commercial distribution?**  
> To obtain commercial licensing rights for distributing pre-flashed boards, **contact the developer (Zotix)** via GitHub Issues:  
> [https://github.com/zotix/LyRadar/issues](https://github.com/zotix/LyRadar/issues)

See [LICENSE](LICENSE) for full details.

---

## ☕ Support the Developer (Buy Me a Coffee)

If you enjoy **LyRadar CYD** and would like to support future development, you can scan the PromptPay QR code below:

<div align="center">
  <img src="docs/images/promptpay_qr.png" width="220" alt="PromptPay QR Code">
  <br>
  <em>Scan with any Thai mobile banking app to support the developer (Zotix) ☕</em>
</div>

- You can also scan the QR code directly from the on-device **`[ABOUT]`** screen.

---

## 🙏 Acknowledgements

- **Bangkok Metropolitan Administration (BMA)**: Weather radar data for Nong Khaem and Nong Chok stations.
- **Thai Meteorological Department (TMD)**: Regional radar imagery covering nationwide stations.
- **Bodmer / TFT_eSPI**: Embedded graphics rendering engine and sprite buffering.
- **Espressif Systems**: ESP32 SoC, FreeRTOS implementation, and Arduino Core.
