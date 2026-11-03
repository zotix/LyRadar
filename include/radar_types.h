#pragma once

#include <Arduino.h>
#include <Preferences.h>
#include <TFT_eSPI.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_partition.h"
#include "esp_wifi.h"
#include "config.h"

// ============================================================================
// Multi-Station Catalog (BMA Bangkok + TMD Thai Meteorological Department)
// ============================================================================
struct RadarStationInfo {
    const char* shortCode;   // Shown on HUD & Menu button (max 8 chars)
    const char* title;       // Shown on Status screen
    const char* url;         // Animated GIF URL
    const char* referer;     // HTTP Referer header
    uint16_t    defWidth;    // Default width (overridden dynamically by GIF header)
    uint16_t    defHeight;   // Default height (overridden dynamically by GIF header)
};

static constexpr uint8_t NUM_STATIONS = 12;

static const RadarStationInfo kStations[NUM_STATIONS] = {
    { "BMA-NK",  "BMA Radar: Nong Khaem (BKK W)",
      "https://weather.bangkok.go.th/Radar/ImageHandlerNongkamAni.ashx",
      "https://weather.bangkok.go.th/Radar/RadarAnimationNk.aspx", 965, 800 },
    { "BMA-NC",  "BMA Radar: Nong Chok (BKK E)",
      "https://weather.bangkok.go.th/Radar/ImageHandlerNongchokAni.ashx",
      "https://weather.bangkok.go.th/Radar/RadarAnimation.aspx",   965, 800 },
    { "TMD-SVP", "TMD Radar: Suvarnabhumi (240km)",
      "https://weather.tmd.go.th/svp/svploop.gif",
      "https://weather.tmd.go.th/svp240loop.php",                  936, 797 },
    { "TMD-SKM", "TMD Radar: Samut Songkhram",
      "https://weather.tmd.go.th/skm/skmloop.gif",
      "https://weather.tmd.go.th/skm240loop.php",                  800, 800 },
    { "TMD-RYG", "TMD Radar: Rayong (East)",
      "https://weather.tmd.go.th/ryg/rygloop.gif",
      "https://weather.tmd.go.th/rygloop.php",                    1087, 800 },
    { "TMD-CRI", "TMD Radar: Chiang Rai (North)",
      "https://weather.tmd.go.th/cri/criloop.gif",
      "https://weather.tmd.go.th/criloop.php",                     800, 800 },
    { "TMD-KKN", "TMD Radar: Khon Kaen (NE)",
      "https://weather.tmd.go.th/kkn/kknloop.gif",
      "https://weather.tmd.go.th/kkn240loop.php",                  800, 800 },
    { "TMD-SRT", "TMD Radar: Surat Thani (South)",
      "https://weather.tmd.go.th/srt/srtloop.gif",
      "https://weather.tmd.go.th/srtloop.php",                     800, 800 },
    { "TMD-PHS", "TMD Radar: Phitsanulok (N-Central)",
      "https://weather.tmd.go.th/phs/phsloop.gif",
      "https://weather.tmd.go.th/phs240loop.php",                  800, 800 },
    { "TMD-UBN", "TMD Radar: Ubon Ratchathani (NE-S)",
      "https://weather.tmd.go.th/ubn/ubnloop.gif",
      "https://weather.tmd.go.th/ubn240loop.php",                  800, 800 },
    { "TMD-PKT", "TMD Radar: Phuket (South-Andaman)",
      "https://weather.tmd.go.th/pkt/pktloop.gif",
      "https://weather.tmd.go.th/pkt240loop.php",                  800, 800 },
    { "TMD-STP", "TMD Radar: Sathing Phra (South-Gulf)",
      "https://weather.tmd.go.th/stp/stploop.gif",
      "https://weather.tmd.go.th/stp240loop.php",                  800, 800 },
};

// ============================================================================
// Flash Cache Layout (Partition "radar_cache", subtype 0x99) - Double Buffered
// ============================================================================
static constexpr uint32_t CACHE_MAGIC        = 0x52414438; // "RAD8" (multi-station + dynamic dims)
static constexpr uint32_t SECTOR_SIZE        = 0x1000;
static constexpr uint32_t FRAME_BYTES        = (uint32_t)DST_WIDTH * DST_HEIGHT;      // 76,800
static constexpr uint32_t BANK_HDR_SIZE      = SECTOR_SIZE;
static constexpr uint32_t FULL_REGION_SIZE   = MAX_RADAR_FRAMES * FRAME_BYTES;         // 921,600 (0xE1000)
static constexpr uint32_t ZOOM_REGION_SIZE   = ZOOM_FRAMES * FRAME_BYTES;              // 460,800 (0x70800)
static constexpr uint32_t BANK_SIZE          = BANK_HDR_SIZE +
    ((FULL_REGION_SIZE + ZOOM_REGION_SIZE + SECTOR_SIZE - 1) / SECTOR_SIZE) * SECTOR_SIZE; // 0x153000
static constexpr uint32_t SCRATCH_ZOOM_ADDR  = 2 * BANK_SIZE;                          // 0x2A6000
static constexpr uint32_t SCRATCH_ZOOM_BYTES = ((FRAME_BYTES + SECTOR_SIZE - 1) / SECTOR_SIZE) * SECTOR_SIZE; // 0x13000
static constexpr int      STRIP_ROWS         = 4;                                      // 4-row Flash I/O batching
static constexpr int      TAIL_ROWS          = 32;                                     // Bottom 32 rows (>= TS_HEIGHT 25)
static constexpr int      DIRECT_ROWS        = DST_HEIGHT - TAIL_ROWS;                 // 208 rows (52 strips of 4 rows)
static constexpr int      NUM_STRIPS         = DIRECT_ROWS / STRIP_ROWS;               // 52 strips
static constexpr size_t   STRIP_BYTES        = (size_t)STRIP_ROWS * DST_WIDTH;         // 1,280 bytes per strip
static constexpr int      RENDER_LINES       = 16;

static_assert(DIRECT_ROWS % STRIP_ROWS == 0, "DIRECT_ROWS must be a multiple of STRIP_ROWS");
static_assert(TAIL_ROWS >= TS_HEIGHT, "TAIL_ROWS must cover TS_HEIGHT");
static_assert(FULL_REGION_SIZE % SECTOR_SIZE == 0, "Zoom region must start on a sector boundary");
static_assert(ZOOM_FRAMES * 2 == MAX_RADAR_FRAMES, "ZOOM_FRAMES must be half of MAX_RADAR_FRAMES");

struct BankHeader {
    uint32_t magic;
    uint32_t seq;          // Higher = newer
    uint32_t sig;          // FNV-1a hash of GIF header + palette + frame 0 data
    uint16_t frameCount;   // 1..12 full frames
    uint16_t zoomCount;    // 1..6 even zoom frames
    uint16_t zoomSrcX;     // Source X of 320x240 1:1 zoom crop
    uint16_t zoomSrcY;     // Source Y of 320x240 1:1 zoom crop
    uint16_t srcWidth;     // Source GIF width (e.g. 965, 936, 800, 1087)
    uint16_t srcHeight;    // Source GIF height (e.g. 800, 797)
    uint8_t  stationId;    // 0..7 index into kStations[]
    uint8_t  rainPct;      // 0..100% rain echo coverage in the Zoom region on latest frame
    uint16_t reserved;
    uint16_t palette[256];
};

struct HudCtx {
    int  dispFrame;   // 0-based source frame index shown in badge
    int  totalFrames; // 12
    int  stepPos;     // 1..stepCount for progress bar
    int  stepCount;   // 12 in Full mode, 6 in Zoom mode
    bool zoom;
    bool preview;     // Showing frames while downloading
    bool instantZoom; // Showing upscaled preview while 1:1 HD crop loads
};

// Temporary workspace (~43 KB) allocated on heap ONLY after TLS handshake succeeds
struct GifDecodeWorkspace {
    uint16_t lzw_prefix[4096];
    uint8_t  lzw_suffix[4096];
    uint8_t  lzw_stack[4096];
    uint8_t  lctMap[256];          // Maps Local Color Table index -> Global Color Table index
    uint8_t  gctRgb[256][3];       // Original RGB888 of Global Color Table for fast LCT matching
    uint8_t  fullStripBuf[STRIP_BYTES];
    uint8_t  zoomStripBuf[STRIP_BYTES];
    uint8_t  fullTailBuf[TAIL_ROWS * DST_WIDTH];
    uint8_t  zoomTailBuf[TAIL_ROWS * DST_WIDTH];
    uint8_t  tsCropBuf[TS_HEIGHT * TS_WIDTH];
    bool     fullStripDone[NUM_STRIPS];
    bool     zoomStripDone[NUM_STRIPS];
};

enum DlStage  { DL_IDLE = 0, DL_CONNECTING = 1, DL_DOWNLOADING = 2, DL_SD_RECROP = 3, DL_ERROR = 4 };
enum DlResult { DL_FAIL = 0, DL_UNCHANGED = 1, DL_COMMITTED = 2 };

static inline uint32_t bankBase(int bank)            { return (uint32_t)bank * BANK_SIZE; }
static inline uint32_t fullAddr(int bank, int frame) { return bankBase(bank) + BANK_HDR_SIZE + (uint32_t)frame * FRAME_BYTES; }
static inline uint32_t zoomAddr(int bank, int slot)  { return bankBase(bank) + BANK_HDR_SIZE + FULL_REGION_SIZE + (uint32_t)slot * FRAME_BYTES; }

// ============================================================================
// Timelapse & History Frame Header (SD-2)
// ============================================================================
static constexpr uint32_t RAD_FRAME_MAGIC = 0x52414446; // "RADF"

struct RadFrameHeader {
    uint32_t magic;      // RAD_FRAME_MAGIC
    uint32_t timestamp;  // epoch time_t
    uint16_t stationId;
    uint8_t  rainPct;
    uint8_t  reserved;
    uint16_t palette[256];
};

static bool     g_timelapseMode     = false;
static int      g_timelapseCount    = 0;
static int      g_timelapseIdx      = 0;
static uint32_t g_lastTimelapseStep = 0;
static char     g_timelapseLabel[32] = "";

// ============================================================================
// Global Hardware & State Instances
// ============================================================================
static TFT_eSPI               tft = TFT_eSPI();
static TFT_eSprite            spr = TFT_eSprite(&tft);
static Preferences            prefs;
static const esp_partition_t* radarPartition = nullptr;
static GifDecodeWorkspace*    g_ws           = nullptr;
static bool                   g_sdReady      = false;

// Compact static buffers (kept < 66 KB total so DRAM has > 80 KB contiguous block for TLS)
static uint32_t g_bankErasedTo[2] = { 0, 0 };
static uint16_t g_dlPalette[256];
static uint8_t  g_prio[256];
static int16_t  lutFullDx[MAX_SRC_WIDTH];
static int16_t  lutFullDy[MAX_SRC_HEIGHT];
static uint8_t  httpReadBuf[4096];
static uint8_t  gifSubBlock[256];
static uint8_t  lineIdxBuf[RENDER_LINES * DST_WIDTH];
static uint8_t  previewRowBuf[DST_WIDTH];

// Loop-owned state (Core 1)
static uint16_t g_palette[256];
static int      g_activeBank     = -1;
static uint32_t g_activeSeq      = 0;
static uint32_t g_activeSig      = 0;
static uint16_t g_frameCount     = 0;
static uint16_t g_zoomCount      = 0;
static uint16_t g_bankZoomX      = ZOOM_SRC_X;
static uint16_t g_bankZoomY      = ZOOM_SRC_Y;
static uint16_t g_srcWidth       = SRC_WIDTH;
static uint16_t g_srcHeight      = SRC_HEIGHT;
static uint8_t  g_bankStation    = 0;
static uint8_t  g_rainPct        = 0;

// User-selected target zoom region & station (saved in NVS)
static volatile uint16_t g_targetZoomX   = ZOOM_SRC_X;
static volatile uint16_t g_targetZoomY   = ZOOM_SRC_Y;
static volatile uint8_t  g_targetStation = 0; // 0..7 index into kStations[]
static volatile bool     g_forceRecrop   = false;
static char              g_regionName[12] = "CENTER";

static bool     g_zoomMode       = false;
static bool     g_autoCycle      = true;  // Station auto-cycling (slideshow)
static bool     g_standbyMode    = false; // Low-power display standby
static uint32_t g_lastStationCycleMs = 0;
static uint32_t g_lastHttpOkMs[NUM_STATIONS] = { 0 };
static constexpr uint32_t AUTO_CYCLE_INTERVAL_MS = 30000UL;      // 30s per station
static constexpr uint32_t INACTIVITY_AUTO_MS     = 3 * 60000UL;  // 3m inactivity watchdog
static bool     g_paused         = false;
static bool     g_menuOpen       = false;
static bool     g_invertDisplay  = false;
static bool     g_flipScreen     = false; // false = 0 deg (rotation 1), true = 180 deg (rotation 3)
static uint8_t  g_touchMapMode   = 0;
static uint8_t  g_brightMode     = 1;     // Default 1 = 100% (0 = AUTO, 1 = 100%, 2 = 55%, 3 = 20%)
static bool     g_screenHasRadar = false;
static int      g_stepIdx        = 0;
static uint32_t g_lastFrameStep  = 0;
static uint32_t g_lastClockTick  = 0;
static uint32_t g_lastTouchMs    = 0;
static volatile uint32_t g_lastOkUpdateMs = 0;

// Shared between downloadTask (Core 0) and loop (Core 1)
static volatile int      g_dlStage           = DL_IDLE;
static volatile int      g_dlFrames          = 0;
static volatile int      g_dlZoomReadyFrames = 0; // Number of 1:1 HD zoom frames ready in g_dlTargetBank
static volatile uint16_t g_dlZoomX           = ZOOM_SRC_X;
static volatile uint16_t g_dlZoomY           = ZOOM_SRC_Y;
static volatile uint8_t  g_dlStation         = 0;
static volatile uint16_t g_dlSrcWidth        = SRC_WIDTH;
static volatile uint16_t g_dlSrcHeight       = SRC_HEIGHT;
static volatile int      g_dlTargetBank      = -1;
static volatile int      g_lastHttpErr       = 0;
static volatile int      g_readyBank         = -1;
static volatile bool     g_bankReady         = false;
static volatile bool     g_partialBank       = false;
static TaskHandle_t      g_dlTask            = nullptr;

// ============================================================================
// Common Utility Helpers
// ============================================================================
static inline uint16_t rgb888To565(uint8_t r, uint8_t g, uint8_t b) {
    return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
}

static inline uint32_t fnv1a(uint32_t h, const uint8_t* d, size_t n) {
    while (n--) { h ^= *d++; h *= 16777619u; }
    return h;
}
static constexpr uint32_t FNV_OFFSET = 2166136261u;

static void getStationDefaultZoom(uint8_t sta, uint16_t srcW, uint16_t srcH, uint16_t& cx, uint16_t& cy) {
    if (sta <= 1 && srcW == 965 && srcH == 800) {
        cx = 380; cy = 280; // BMA radar center
    } else {
        cx = (srcW > DST_WIDTH)  ? (srcW - DST_WIDTH)  / 2 : 0;
        cy = (srcH > DST_HEIGHT) ? (srcH - DST_HEIGHT) / 2 : 0;
    }
}

static void updateRegionName(uint16_t zx, uint16_t zy) {
    uint16_t cx = 380, cy = 280;
    getStationDefaultZoom(g_targetStation, g_srcWidth, g_srcHeight, cx, cy);
    if      (abs((int)zx - (int)cx) < 40 && abs((int)zy - (int)cy) < 40) strncpy(g_regionName, "CENTER", sizeof(g_regionName));
    else if (abs((int)zx - (int)cx) < 40 && (int)zy < (int)cy - 100)     strncpy(g_regionName, "NORTH",  sizeof(g_regionName));
    else if ((int)zx > (int)cx + 100     && abs((int)zy - (int)cy) < 40) strncpy(g_regionName, "EAST",   sizeof(g_regionName));
    else if ((int)zx < (int)cx - 100     && abs((int)zy - (int)cy) < 40) strncpy(g_regionName, "WEST",   sizeof(g_regionName));
    else if (abs((int)zx - (int)cx) < 40 && (int)zy > (int)cy + 100)     strncpy(g_regionName, "SOUTH",  sizeof(g_regionName));
    else snprintf(g_regionName, sizeof(g_regionName), "%u,%u", (unsigned)zx, (unsigned)zy);
}

static uint8_t classifyColorPriority(uint8_t r, uint8_t g, uint8_t b) {
    bool isGreenEcho  = (g > r + 35 && g > b + 35);
    bool isWarmEcho   = (r > 180 && b < 100 && abs((int)r - (int)g) < 180);
    bool isPurpleEcho = (r > 180 && b > 150 && g < 150);
    if (isGreenEcho || isWarmEcho || isPurpleEcho) {
        if (!(r > 150 && r < 210 && g > 150 && g < 210 && b > 110 && b < 160)) {
            return 3;
        }
    }
    return 1;
}

static void initScalingLookupTables(uint16_t srcW = SRC_WIDTH, uint16_t srcH = SRC_HEIGHT) {
    if (srcW > MAX_SRC_WIDTH)  srcW = MAX_SRC_WIDTH;
    if (srcH > MAX_SRC_HEIGHT) srcH = MAX_SRC_HEIGHT;
    if (srcW < DST_WIDTH)      srcW = DST_WIDTH;
    if (srcH < DST_HEIGHT)     srcH = DST_HEIGHT;

    for (int sx = 0; sx < MAX_SRC_WIDTH; sx++)  lutFullDx[sx] = -1;
    for (int sy = 0; sy < MAX_SRC_HEIGHT; sy++) lutFullDy[sy] = -1;

    for (int dx = 0; dx < DST_WIDTH; dx++) {
        int centerSx = (dx * srcW + (srcW / 2)) / DST_WIDTH;
        if (centerSx < srcW) lutFullDx[centerSx] = dx;
    }
    for (int dy = 0; dy < DST_HEIGHT; dy++) {
        int centerSy = (dy * srcH + (srcH / 2)) / DST_HEIGHT;
        if (centerSy < srcH) lutFullDy[centerSy] = dy;
    }
}

static void setStatusLed(bool r, bool g, bool b) {
    digitalWrite(CYD_LED_RED,   r ? LOW : HIGH);
    digitalWrite(CYD_LED_GREEN, g ? LOW : HIGH);
    digitalWrite(CYD_LED_BLUE,  b ? LOW : HIGH);
}
