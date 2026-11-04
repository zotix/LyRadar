#pragma once

// ============================================================================
// LyRadar CYD - Version & Project Info
// ============================================================================
#define APP_NAME            "LyRadar CYD"
#define APP_VERSION         "v1.0.1"
#define APP_AUTHOR          "Zotix"
#define APP_GITHUB          "https://github.com/zotix/LyRadar"

// ============================================================================
// 1. ตั้งค่า WiFi (ใส่ตรงนี้, ใส่ใน /ly-radar/wifi.txt บน SD, หรือใช้ WiFi Scanner/AP)
// ============================================================================
#define WIFI_SSID           "YOUR_WIFI_SSID"
#define WIFI_PASSWORD       "YOUR_WIFI_PASSWORD"

// ชื่อ WiFi AP สำหรับตั้งค่าผ่านมือถือ (จะเปิดอัตโนมัติหากเชื่อมต่อ WiFi ไม่สำเร็จ)
#define CAPTIVE_PORTAL_SSID "LyRadar-AP"

// ============================================================================
// 2. ตั้งค่า URL เรดาร์สำนักการระบายน้ำ กทม. (BMA) และกรมอุตุนิยมวิทยา (TMD)
// ============================================================================
#define RADAR_GIF_URL_NK    "https://weather.bangkok.go.th/Radar/ImageHandlerNongkamAni.ashx"
#define RADAR_REFERER_NK    "https://weather.bangkok.go.th/Radar/RadarAnimationNk.aspx"

#define RADAR_GIF_URL_NC    "https://weather.bangkok.go.th/Radar/ImageHandler.ashx"
#define RADAR_REFERER_NC    "https://weather.bangkok.go.th/Radar/RadarAnimation.aspx"

#define RADAR_GIF_URL       RADAR_GIF_URL_NK
#define RADAR_REFERER       RADAR_REFERER_NK

// ============================================================================
// 3. ตั้งค่าการเล่นภาพ Loop และการอัปเดตข้อมูล
// ============================================================================
#define MAX_RADAR_FRAMES        12          // จำนวนเฟรมสูงสุดในไฟล์ GIF (12 เฟรม)
#define ZOOM_FRAMES             6           // โหมดซูมเก็บ 6 เฟรมเลขคู่ (2/12, 4/12..12/12)
#define FRAME_DELAY_MS          350         // ความเร็วเปลี่ยนเฟรมโหมดปกติ (โหมดซูมคูณ 2 = 700 ms)
#define LAST_FRAME_PAUSE_MS     1200        // หยุดค้างที่เฟรมสุดท้ายนานขึ้นเพื่อให้ดูทิศทางเมฆชัดๆ
#define UPDATE_INTERVAL_MS      (5UL * 60UL * 1000UL)  // ตรวจภาพใหม่ทุก 5 นาที
#define RETRY_INTERVAL_MS       (30UL * 1000UL)        // ลองใหม่หลังดาวน์โหลดล้มเหลว

// ============================================================================
// 4. พิกัดขนาดภาพต้นฉบับและหน้าจอแสดงผล
// ============================================================================
// ขนาดภาพต้นฉบับเริ่มต้น (กทม. 965x800, กรมอุตุฯ 800x800 / 936x797 / 1087x800)
#define SRC_WIDTH               965
#define SRC_HEIGHT              800
#define MAX_SRC_WIDTH           1100
#define MAX_SRC_HEIGHT          820

// ขนาดหน้าจอแสดงผล (แนวนอน)
#define DST_WIDTH               320
#define DST_HEIGHT              240

// พิกัดโหมดซูมเริ่มต้น (กึ่งกลางเรดาร์)
#define ZOOM_SRC_X              380
#define ZOOM_SRC_Y              280

// ขนาดป้ายวัน-เวลามุมขวาล่างของภาพต้นฉบับ
#define TS_SRC_X                826
#define TS_SRC_Y                768
#define TS_WIDTH                136
#define TS_HEIGHT               25

// ============================================================================
// 5. ขาพินสำหรับบอร์ด ESP32-2432S028 (CYD)
// ============================================================================
// ขาพินระบบสัมผัส XPT2046 (Bit-banged SPI)
#define XPT2046_IRQ             36
#define XPT2046_MOSI            32
#define XPT2046_MISO            39
#define XPT2046_CLK             25
#define XPT2046_CS              33

// ขาพินช่องเสียบ MicroSD Card (VSPI)
#define SD_CS_PIN               5
#define SD_SCK_PIN              18
#define SD_MISO_PIN             19
#define SD_MOSI_PIN             23

// ปุ่ม BOOT บนบอร์ด, เซนเซอร์วัดแสง (LDR) และไฟ RGB LED ด้านหลังบอร์ด (Active LOW)
#define BOOT_BTN_PIN            0
#define CYD_LDR_PIN             34
#define CYD_LED_RED             4
#define CYD_LED_GREEN           16
#define CYD_LED_BLUE            17
