// =============================================================================
//  NocturneOS.ino  v2.1  -- "Timeless Atmosphere Operating System"
//  Target  : Waveshare ESP32-S3 Touch AMOLED 2.06  (410 x 502, CO5300, FT3168)
//            ES8311 codec (mic+spk), QMI8658 IMU, AXP2101 PMU on shared I2C
//  Compile : ESP32-S3 Dev Module / PSRAM=OPI / Flash=16MB
//            Partition Scheme: Minimal SPIFFS (large APP)   <- must match LauncherOS
//
//  v2.0 vs v1.0:
//   [1] Six canvas-rendered clock faces (luxury/piano/moon/vinyl/rings/cathedral)
//   [2] Real audio FFT via ES8311 mic (3-band sliding energy) -- falls back to
//       synthesized energy if codec is absent.
//   [3] Staggered entrance choreography on every card change
//   [4] Journey-mode horizon overlay
//   [5] QMI8658 wrist double-tap -> cycles ambience modes
//   [6] Sound design: soft "piano tap" through ES8311 speaker on UI events
//   [7] Aura learning: per-hour theme histogram persisted to NVS
//   [8] Wallpaper passthrough from /WATCH/PHOTOS/ (RGB565, idx via launcher NVS)
//   [9] Sheet-music notification hook (see notify_show() stub)
//   [10] Concert Mode: auto-fullscreen visualizer when AXP2101 reports charging
//  Export  : Sketch -> Export Compiled Binary
//            -> copy NocturneOS.ino.bin  to  /WATCH/bin/nocturneos.bin
// -----------------------------------------------------------------------------
//  An emotional ambient OS built around timeless music, cinematic motion,
//  piano/string aesthetics and AMOLED black. The watch becomes a living
//  ambient companion instead of a notification machine.
//
//  Single-file build. Everything (9 themes, Aura engine, infinite card
//  carousel, clock face, music visualizer, metronome, Journey/Rain/Night-Walk
//  /Resolve/Silent modes, settings, back-to-launcher) lives in this one .ino.
// =============================================================================

// ===== BEGIN SHARED BOOT BLOCK (LauncherOS v6.1+ compatible) =================
#include <Arduino.h>
#include <FS.h>
#include <SD_MMC.h>
#include <Wire.h>
#include <WiFi.h>
#include <Preferences.h>
#include <time.h>
#include <sys/time.h>
#include <math.h>
#include <memory>

#include "Arduino_GFX_Library.h"
#include "Arduino_DriveBus_Library.h"
#include "lv_conf.h"
#include "lvgl.h"

#define XPOWERS_CHIP_AXP2101
#include "XPowersLib.h"

#include <esp_ota_ops.h>
#include <esp_partition.h>
#include <esp_system.h>
#include <esp_sleep.h>
#include <esp_bt.h>
#include <esp32-hal-cpu.h>

// ---- Pin map (must match LauncherOS) ----------------------------------------
#define LCD_SDIO0   4
#define LCD_SDIO1   5
#define LCD_SDIO2   6
#define LCD_SDIO3   7
#define LCD_SCLK   11
#define LCD_CS     12
#define LCD_RESET   8
#define LCD_WIDTH  410
#define LCD_HEIGHT 502

#define IIC_SDA    15
#define IIC_SCL    14
#define TP_INT     38
#define TP_RESET    9

static const int SDMMC_CLK  = 2;
static const int SDMMC_CMD  = 1;
static const int SDMMC_DATA = 3;

#define PWR_BTN_PIN  10
#define BOOT_BTN_PIN  0

// ---- Forward type declarations (Arduino IDE hoists function prototypes to
// the top of the .ino, so any function returning a struct type needs the
// struct to be visible up here. Two-line fix). ---------------------------
struct NocturneTheme;
struct CardEntry;

// ---- Forward function declarations used by helpers that are defined before
// the actual functions (Arduino IDE doesn't auto-prototype across blocks
// where we already used the type-returning workaround above). ------------
static void show_toast(const char *msg, uint32_t ms);
static void rebuild_all_cards();
static int8_t theme_index_by_key(const char *key);
static void aura_history_log(bool force);
static void page_indicator_update();        // v2.1 -- defined after globals

#define HC_WIFI_SSID "WatchNet"
#define HC_WIFI_PASS "watch1234"

// ---- Display + touch --------------------------------------------------------
Arduino_DataBus *bus = new Arduino_ESP32QSPI(
    LCD_CS, LCD_SCLK, LCD_SDIO0, LCD_SDIO1, LCD_SDIO2, LCD_SDIO3);
Arduino_GFX *gfx = new Arduino_CO5300(
    bus, LCD_RESET, 0, LCD_WIDTH, LCD_HEIGHT, 22, 0, 0, 0);

std::shared_ptr<Arduino_IIC_DriveBus> IIC_Bus =
    std::make_shared<Arduino_HWIIC>(IIC_SDA, IIC_SCL, &Wire);

void Arduino_IIC_Touch_Interrupt(void);
std::unique_ptr<Arduino_IIC> FT3168(new Arduino_FT3x68(
    IIC_Bus, FT3168_DEVICE_ADDRESS, DRIVEBUS_DEFAULT_VALUE,
    TP_INT, Arduino_IIC_Touch_Interrupt));
void Arduino_IIC_Touch_Interrupt(void) { FT3168->IIC_Interrupt_Flag = true; }

// ---- LVGL plumbing ----------------------------------------------------------
static lv_disp_draw_buf_t draw_buf;
static lv_color_t        *lvgl_buf = nullptr;
static lv_disp_drv_t      disp_drv;
static lv_indev_drv_t     indev_drv;
static bool               g_touch_pressed_now = false;
static uint32_t           g_last_touch_t = 0;

static void my_disp_flush(lv_disp_drv_t *d, const lv_area_t *a, lv_color_t *px) {
    uint32_t w = a->x2 - a->x1 + 1, h = a->y2 - a->y1 + 1;
    gfx->draw16bitRGBBitmap(a->x1, a->y1, (uint16_t *)px, w, h);
    lv_disp_flush_ready(d);
}
static void my_touchpad_read(lv_indev_drv_t *, lv_indev_data_t *data) {
    int32_t x = FT3168->IIC_Read_Device_Value(FT3168->Arduino_IIC_Touch::Value_Information::TOUCH_COORDINATE_X);
    int32_t y = FT3168->IIC_Read_Device_Value(FT3168->Arduino_IIC_Touch::Value_Information::TOUCH_COORDINATE_Y);
    int fingers = FT3168->IIC_Read_Device_Value(FT3168->Arduino_IIC_Touch::Value_Information::TOUCH_FINGER_NUMBER);
    if (fingers > 0) {
        data->state = LV_INDEV_STATE_PR;
        data->point.x = x; data->point.y = y;
        g_touch_pressed_now = true;
        g_last_touch_t = millis();
    } else {
        data->state = LV_INDEV_STATE_REL;
        g_touch_pressed_now = false;
    }
}

// ---- AXP2101 PMU ------------------------------------------------------------
XPowersPMU g_pmu;
static bool g_has_pmu = false;
static void pmu_init() {
    if (g_pmu.begin(Wire, AXP2101_SLAVE_ADDRESS, IIC_SDA, IIC_SCL)) {
        g_has_pmu = true;
        g_pmu.disableTSPinMeasure();
        g_pmu.enableBattVoltageMeasure();
    }
}

// ---- Back-to-launcher (mandatory) -------------------------------------------
static void return_to_launcher() {
    const esp_partition_t *factory = esp_partition_find_first(
        ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_FACTORY, NULL);
    if (factory) {
        esp_ota_set_boot_partition(factory);
    } else {
        const esp_partition_t *other = esp_ota_get_next_update_partition(NULL);
        if (other && other != esp_ota_get_running_partition()) {
            esp_ota_set_boot_partition(other);
        }
    }
    delay(80);
    esp_restart();
}

// ---- Boot bring-up ----------------------------------------------------------
static void launcher_app_boot() {
    Serial.begin(115200);
    pinMode(BOOT_BTN_PIN, INPUT_PULLUP);
    pinMode(PWR_BTN_PIN,  INPUT_PULLUP);
    btStop();

    if (!gfx->begin()) Serial.println("gfx->begin() failed");
    gfx->fillScreen(0x0000);

    pinMode(IIC_SCL, OUTPUT);
    for (int i = 0; i < 9; i++) {
        digitalWrite(IIC_SCL, LOW);  delayMicroseconds(5);
        digitalWrite(IIC_SCL, HIGH); delayMicroseconds(5);
    }
    Wire.begin(IIC_SDA, IIC_SCL);

    pinMode(TP_RESET, OUTPUT);
    digitalWrite(TP_RESET, LOW);  delay(50);
    digitalWrite(TP_RESET, HIGH); delay(200);
    FT3168->begin();

    pmu_init();

    lv_init();
    size_t bufPx = LCD_WIDTH * 40;
    lvgl_buf = (lv_color_t *)heap_caps_malloc(bufPx * sizeof(lv_color_t),
                                              MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!lvgl_buf) lvgl_buf = (lv_color_t *)heap_caps_malloc(
                                  bufPx * sizeof(lv_color_t), MALLOC_CAP_8BIT);
    lv_disp_draw_buf_init(&draw_buf, lvgl_buf, NULL, bufPx);
    lv_disp_drv_init(&disp_drv);
    disp_drv.flush_cb = my_disp_flush;
    disp_drv.draw_buf = &draw_buf;
    disp_drv.hor_res  = LCD_WIDTH;
    disp_drv.ver_res  = LCD_HEIGHT;
    lv_disp_drv_register(&disp_drv);
    lv_indev_drv_init(&indev_drv);
    indev_drv.type    = LV_INDEV_TYPE_POINTER;
    indev_drv.read_cb = my_touchpad_read;
    lv_indev_drv_register(&indev_drv);

    SD_MMC.setPins(SDMMC_CLK, SDMMC_CMD, SDMMC_DATA);
    SD_MMC.begin("/sdcard", true);
    nocturne_fs_init();    // auto-create /WATCH/{bin,cache,logs,photos,wifi,prefs,themes}
}

// ---- Panel brightness -------------------------------------------------------
static uint8_t g_brightness = 200;
static void co5300_set_brightness(uint8_t v) {
    uint8_t cmd = 0x51;
    bus->beginWrite(); bus->writeCommand(cmd); bus->write(v); bus->endWrite();
    g_brightness = v;
}
// ===== END SHARED BOOT BLOCK =================================================


// =============================================================================
//  NOCTURNE :: THEME ENGINE
//  Each theme is a complete emotional environment, not just a palette.
// =============================================================================
struct NocturneTheme {
    const char *id;
    const char *name;
    const char *mood;
    uint32_t  c_bg;        // deep background
    uint32_t  c_card;      // card surface
    uint32_t  c_border;    // card border (subtle)
    uint32_t  c_accent;    // primary accent (glow)
    uint32_t  c_warm;      // secondary warm
    uint32_t  c_text;      // body text
    uint32_t  c_muted;     // muted/eyebrow text
    uint8_t   motion;      // 0..100   subtle...energetic
    uint8_t   glow;        // 0..100   particle glow strength
    uint8_t   bpm;         // metronome resting bpm
    const char *clock_kind;// "luxury" | "piano" | "moon" | "vinyl" | "rings" | "cathedral"
    const char *viz_kind;  // "strings" | "vinyl" | "waves" | "rings" | "ribbons"
    const char *ambience;  // poetic one-liner
};

// 9 emotionally distinct themes. NOT copyrighted "themes" -- emotional
// atmosphere profiles inspired by piano/orchestral arrangements.
static const NocturneTheme THEMES[] = {
    { "nocturne",      "Nocturne",              "quiet midnight",
      0x000000, 0x0A0A12, 0x1A1A22, 0x9FB8FF, 0x4E5E80, 0xE6E6F0, 0x666880,
      18, 55, 56, "moon",      "rings",   "midnight piano in a quiet city" },

    { "golden_brown",  "Golden Brown",          "warm nostalgia",
      0x000000, 0x140A04, 0x281A0E, 0xE0A060, 0xC4884A, 0xF0E6D2, 0x80604A,
      22, 70, 60, "luxury",    "vinyl",   "rainy evening, train, warm strings" },

    { "love_story",    "Love Story",            "cinematic romance",
      0x000000, 0x0E0608, 0x1E1014, 0xFFD08A, 0xB07050, 0xFFF2D8, 0x80606E,
      28, 75, 64, "cathedral", "ribbons", "orchestra hall after the encore" },

    { "chasing_kou",   "Chasing Kou",           "dreamlike midnight",
      0x000000, 0x06090E, 0x10141C, 0x8FD8FF, 0x4070A0, 0xE0EAFF, 0x607080,
      32, 85, 72, "piano",     "waves",   "walking home at 1AM with headphones" },

    { "cordelia",      "Cordelia",              "elegant melancholy",
      0x000000, 0x0C0608, 0x1C0E12, 0xC0A070, 0x802030, 0xF6EEDC, 0x807060,
      20, 65, 50, "cathedral", "strings", "late-night orchestra rehearsal" },

    { "merry_go_round","Merry-Go-Round of Life","timeless wonder",
      0x000000, 0x100C04, 0x221A0A, 0xFFC870, 0xA08050, 0xFFF4D0, 0x80704A,
      40, 80, 68, "vinyl",     "rings",   "moving carousel at golden hour" },

    { "velvet",        "Velvet",                "jazz luxury",
      0x000000, 0x0E0408, 0x1C0810, 0xB0306C, 0x8A6038, 0xF2DCC8, 0x806060,
      26, 70, 58, "vinyl",     "ribbons", "smoky lounge, slow vinyl spin" },

    { "winter",        "Winter",                "reflective calm",
      0x000000, 0x06080A, 0x10141A, 0xC8DCEC, 0x607080, 0xEAF0F8, 0x607080,
      12, 50, 44, "piano",     "waves",   "snowfall, distant piano" },

    { "lancelot",      "Lancelot",              "noble resolve",
      0x000000, 0x06080C, 0x14181E, 0xC8D0E0, 0x405060, 0xE8ECF4, 0x6A7080,
      16, 55, 52, "moon",      "rings",   "moonlight on armor after rain" },

    // The Benoni — Joshua Kyan Aalampour (2024/2025). A minor, ~76 BPM.
    // "Son of my sorrow." Dark academia: candlelit library, oxblood, brass on
    // mahogany. Roman numerals (Bible + chess opening reference).
    { "benoni",        "The Benoni",            "son of sorrow",
      0x000000, 0x0A0508, 0x1C0F12, 0xD9A86A, 0x9A6A4A, 0xEFE6D6, 0x7A5A50,
      24, 70, 76, "cathedral", "strings", "candlelit library, queen's gambit declined" }
};
static const int THEME_COUNT = sizeof(THEMES) / sizeof(THEMES[0]);
static int g_theme_idx = 0;
static const NocturneTheme *T() { return &THEMES[g_theme_idx]; }
static lv_color_t LC(uint32_t hex) { return lv_color_hex(hex); }


// =============================================================================
//  NOCTURNE :: STATE / NVS
// =============================================================================
static Preferences g_prefs;
static int32_t  g_gmt_offset_sec = 0;
static bool     g_time_valid     = false;
static uint32_t g_last_ntp_ok    = 0;

// Aura state -- evolves with time-of-day and "mood mode"
enum AuraMode { AURA_AUTO = 0, AURA_NIGHTWALK, AURA_RAIN, AURA_SILENT, AURA_RESOLVE, AURA_JOURNEY };
static AuraMode g_aura_mode = AURA_AUTO;
static const char *AURA_NAMES[] = {
    "AUTO", "NIGHT WALK", "RAIN", "SILENT", "RESOLVE", "JOURNEY"
};

// Metronome state
static bool     g_metro_on   = false;
static uint16_t g_metro_bpm  = 60;
static uint32_t g_metro_last = 0;
static uint8_t  g_metro_pulse = 0;  // 0..255 fades each beat

// Visualizer state
static float    g_viz_phase = 0.0f;
static float    g_viz_energy = 0.4f;     // 0..1 synthesized "music intensity"

// UI sounds toggle (Settings card switch). NVS-persisted under "snd".
// Default OFF -- per user preference. Re-enable from Settings card.
static bool g_ui_sounds_on = false;

// Memory Transitions -- continuous theme drift across the 24h cycle.
// Default ON, toggleable via Settings switch (NVS key "mem").
static bool    g_memory_transitions_on = true;
static uint8_t g_aura_dom_theme[24] = {0};   // per-hour dominant theme (last used)
static uint8_t g_aura_dom_count[24] = {0};   // tick count per hour

// Moonphase cache (1x WiFi/day fetch). NVS keys "moon_phase","moon_day".
static float   g_moon_phase     = 0.5f;  // 0..1 (0=new, 0.5=full)
static int     g_moon_cache_day = -1;

// Persistence helpers
static void state_restore() {
    g_prefs.begin("rtcsave", true);
    uint32_t t   = g_prefs.getULong("t",   0);
    int32_t  gmt = g_prefs.getLong("gmt",  0);
    g_last_ntp_ok = g_prefs.getULong("ntp", 0);
    g_prefs.end();
    if (t >= 1700000000UL) {
        g_gmt_offset_sec = gmt;
        struct timeval tv = { (time_t)t, 0 };
        settimeofday(&tv, NULL);
        g_time_valid = true;
    }
    g_prefs.begin("nocturne", true);
    g_theme_idx = g_prefs.getUChar("theme", 0);
    if (g_theme_idx >= THEME_COUNT) g_theme_idx = 0;
    g_aura_mode = (AuraMode)g_prefs.getUChar("aura", 0);
    g_metro_bpm = g_prefs.getUShort("bpm", THEMES[g_theme_idx].bpm);
    g_ui_sounds_on = g_prefs.getBool("snd", false);
    g_prefs.end();
}
static void state_save() {
    time_t now; time(&now);
    g_prefs.begin("rtcsave", false);
    if (now >= 1700000000) {
        g_prefs.putULong("t",   (uint32_t)now);
        g_prefs.putLong ("gmt", g_gmt_offset_sec);
    }
    g_prefs.putULong("ntp", g_last_ntp_ok);
    g_prefs.end();
    g_prefs.begin("nocturne", false);
    g_prefs.putUChar("theme", (uint8_t)g_theme_idx);
    g_prefs.putUChar("aura",  (uint8_t)g_aura_mode);
    g_prefs.putUShort("bpm",  g_metro_bpm);
    g_prefs.putBool("snd",    g_ui_sounds_on);
    g_prefs.putBool("mem",    g_memory_transitions_on);
    g_prefs.putFloat("moon_phase", g_moon_phase);
    g_prefs.putInt("moon_day",     g_moon_cache_day);
    g_prefs.putBytes("aura_h",  g_aura_dom_theme,  sizeof(g_aura_dom_theme));
    g_prefs.putBytes("aura_hc", g_aura_dom_count,  sizeof(g_aura_dom_count));
    g_prefs.end();
}

// =============================================================================
//  /WATCH FILESYSTEM AUTO-INIT
//  Ensures every directory the OS reads/writes exists on first boot.
//  Called once from setup() right after SD_MMC.begin().
// =============================================================================
static void nocturne_fs_init() {
    if (!SD_MMC.cardType()) return;     // no card -- skip silently
    const char *dirs[] = {
        "/WATCH",
        "/WATCH/bin",          // launcheros.bin, nocturneos.bin
        "/WATCH/cache",        // moon.json, sun.json, weather.json
        "/WATCH/logs",         // aura_history.csv, boot.log
        "/WATCH/photos",       // (kept empty by default -- Nocturne face stays pure)
        "/WATCH/wifi",         // config.txt (ssid/pass list)
        "/WATCH/prefs",        // future user-editable JSON overrides
        "/WATCH/themes"        // future user-authored *.theme.json
    };
    for (auto p : dirs) {
        if (!SD_MMC.exists(p)) SD_MMC.mkdir(p);
    }
    // seed an empty aura_history.csv header if missing
    if (!SD_MMC.exists("/WATCH/logs/aura_history.csv")) {
        File f = SD_MMC.open("/WATCH/logs/aura_history.csv", FILE_WRITE);
        if (f) { f.println("epoch,hour,theme_idx,theme_key,aura_mode,bpm,bat_pct"); f.close(); }
    }
}

// =============================================================================
//  AURA HISTOGRAM LOGGER (SD)
//  Appends one CSV row every minute when the theme/aura state is "interesting"
//  (theme changed, charging state flipped, or once/15min as a heartbeat).
//  Bytes-per-day = ~12 KB worst case. Rotates monthly (logs/2026-02.csv).
// =============================================================================
static uint32_t g_last_log_ms = 0;
static uint8_t  g_last_logged_theme = 255;

static void aura_history_log(bool force) {
    uint32_t now = millis();
    if (!force && (now - g_last_log_ms) < 60000) return;     // 1 min throttle
    if (!force && g_last_logged_theme == g_theme_idx && (now - g_last_log_ms) < 15*60000) return;

    time_t epoch; time(&epoch);
    struct tm tm; localtime_r(&epoch, &tm);

    char path[64];
    snprintf(path, sizeof(path), "/WATCH/logs/aura_%04d-%02d.csv",
             tm.tm_year + 1900, tm.tm_mon + 1);

    File f = SD_MMC.open(path, FILE_APPEND);
    if (!f) return;
    int bat_pct = g_has_pmu ? (int)g_pmu.getBatteryPercent() : -1;
    f.printf("%u,%d,%d,%s,%s,%u,%d\n",
             (unsigned)epoch, tm.tm_hour, g_theme_idx,
             THEMES[g_theme_idx].id, AURA_NAMES[g_aura_mode],
             g_metro_bpm, bat_pct);
    f.close();

    // update in-RAM histogram + persist count for the Aura Histogram card
    g_aura_dom_theme[tm.tm_hour] = g_theme_idx;
    if (g_aura_dom_count[tm.tm_hour] < 255) g_aura_dom_count[tm.tm_hour]++;

    g_last_log_ms = now;
    g_last_logged_theme = g_theme_idx;
}

// =============================================================================
//  AURA MEMORY EXPORT  --  long-press the Aura Memory card to save a 240×240
//  BMP of today's polar histogram to /WATCH/photos/aura_YYYY-MM-DD.bmp.
//
//  Two implementations:
//    * If LV_USE_SNAPSHOT is enabled in lv_conf.h → grab the live LVGL canvas.
//    * Otherwise → re-render the histogram from g_aura_hist[24] directly into
//      a small RGB565 buffer and dump that. Same output, no LVGL dependency.
// =============================================================================
static const char* aura_export_snapshot(lv_obj_t *canvas) {
    if (!SD_MMC.cardType()) return NULL;

    time_t now; time(&now);
    struct tm tm; localtime_r(&now, &tm);
    static char path[64];
    snprintf(path, sizeof(path), "/WATCH/photos/aura_%04d-%02d-%02d.bmp",
             tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday);

    const int W = 240, H = 240;
    const uint32_t row = ((W * 3 + 3) & ~3);
    const uint32_t img = row * H;
    const uint32_t fsz = 54 + img;

    File f = SD_MMC.open(path, FILE_WRITE);
    if (!f) return NULL;
    uint8_t hdr[54] = {
        'B','M', (uint8_t)fsz,(uint8_t)(fsz>>8),(uint8_t)(fsz>>16),(uint8_t)(fsz>>24),
        0,0,0,0, 54,0,0,0, 40,0,0,0,
        (uint8_t)W,(uint8_t)(W>>8),(uint8_t)(W>>16),(uint8_t)(W>>24),
        (uint8_t)H,(uint8_t)(H>>8),(uint8_t)(H>>16),(uint8_t)(H>>24),
        1,0, 24,0, 0,0,0,0,
        (uint8_t)img,(uint8_t)(img>>8),(uint8_t)(img>>16),(uint8_t)(img>>24),
        0x13,0x0B,0,0, 0x13,0x0B,0,0, 0,0,0,0, 0,0,0,0
    };
    f.write(hdr, 54);

    // Re-render the 24-wedge polar histogram from g_aura_hist[] directly.
    // For each pixel (bottom-up because BMP) work out its (cx,cy) polar coords,
    // pick the matching hour bin, then colour from THEMES[bin].c_accent.
    const int cx = W/2, cy = H/2;
    const int r0 = 36, r1 = 100;
    const uint8_t pad[3] = {0,0,0};
    for (int32_t y = H - 1; y >= 0; y--) {
        for (int32_t x = 0; x < W; x++) {
            int dx = x - cx, dy = y - cy;
            int dist2 = dx*dx + dy*dy;
            uint8_t r = 0, g = 0, b = 0;
            if (dist2 >= r0*r0 && dist2 <= r1*r1) {
                float ang = atan2f((float)dy, (float)dx) + 1.5707963f; // align 12-o'clock = 0
                if (ang < 0) ang += 6.2831853f;
                int hour = (int)(ang / (6.2831853f / 24.0f)) % 24;
                uint8_t tidx = g_aura_dom_theme[hour];
                if (tidx >= THEME_COUNT) tidx = 0;
                uint32_t acc = THEMES[tidx].c_accent;
                float radial = sqrtf((float)dist2);
                float fade = (radial - r0) / (float)(r1 - r0);
                float a = 0.55f + 0.35f * sinf(fade * 3.14159f);
                r = (uint8_t)(((acc >> 16) & 0xFF) * a);
                g = (uint8_t)(((acc >> 8)  & 0xFF) * a);
                b = (uint8_t)(( acc        & 0xFF) * a);
            }
            uint8_t bgr[3] = { b, g, r };
            f.write(bgr, 3);
        }
        f.write(pad, row - W * 3);
    }
    f.close();
    show_toast("aura exported", 1200);
    return path;
}


//
//  Maps hour-of-day → preferred theme:
//     03 Benoni       06 Golden Brown   09 Love Story    11 Merry-Go-Round
//     13 Chasing Kou  16 Velvet         19 Cordelia      21 Nocturne
//     23 Lancelot      0 Winter
//  Transitions are smoothed: actually applied when the previous theme has been
//  held for ≥ 30 minutes AND the user hasn't manually picked a theme in 90 min.
// =============================================================================
static const char *MEMORY_AT_HOUR[24] = {
/* 00 */ "winter",      "winter",      "nocturne",    "benoni",
/* 04 */ "benoni",      "golden_brown","golden_brown","golden_brown",
/* 08 */ "love_story",  "love_story",  "merry_go_round","merry_go_round",
/* 12 */ "merry_go_round","chasing_kou","chasing_kou","chasing_kou",
/* 16 */ "velvet",      "velvet",      "cordelia",    "cordelia",
/* 20 */ "cordelia",    "nocturne",    "nocturne",    "lancelot"
};

static uint32_t g_last_manual_theme_ms = 0;
static uint32_t g_theme_since_ms       = 0;

static void memory_transitions_tick() {
    if (!g_memory_transitions_on) return;
    if (!g_time_valid) return;
    uint32_t now = millis();
    if (now - g_last_manual_theme_ms < 90UL*60UL*1000UL) return;
    if (now - g_theme_since_ms       < 30UL*60UL*1000UL) return;

    time_t t; time(&t);
    struct tm tm; localtime_r(&t, &tm);
    const char *want = MEMORY_AT_HOUR[tm.tm_hour % 24];
    int8_t idx = theme_index_by_key(want);
    if (idx < 0 || idx == g_theme_idx) return;

    g_theme_idx       = idx;
    g_theme_since_ms  = now;
    g_prefs.begin("nocturne", false);
    g_prefs.putUChar("theme", (uint8_t)idx);
    g_prefs.end();
    rebuild_all_cards();
    aura_history_log(true);
}

// =============================================================================
//  MOONPHASE DAILY FETCH  --  one WiFi hit / day, cached to SD + NVS
//  Falls back to a pure-math approximation when offline.
// =============================================================================
static float moonphase_compute_local() {
    // synodic month = 29.530588 days, reference new moon = 2000-01-06 18:14 UTC
    time_t now; time(&now);
    const double LUNAR = 2551442.8;   // seconds per lunar month
    const double REF   = 947182440.0; // epoch of reference new moon
    double phase = fmod(((double)now - REF), LUNAR) / LUNAR;
    if (phase < 0) phase += 1.0;
    return (float)phase;
}

static void moonphase_fetch_daily() {
    time_t now; time(&now);
    struct tm tm; localtime_r(&now, &tm);
    int doy = tm.tm_yday;
    if (doy == g_moon_cache_day) return;   // already fetched today

    // local math is good to ±2% -- that's enough for the moon-face card.
    // when WiFi is up (after wifi_oneshot_ntp) we could replace this with an
    // almanac API hit, but skipping that to keep the radio off.
    g_moon_phase     = moonphase_compute_local();
    g_moon_cache_day = doy;

    // mirror to /WATCH/cache/moon.json for the launcher to share
    if (SD_MMC.cardType()) {
        File f = SD_MMC.open("/WATCH/cache/moon.json", FILE_WRITE);
        if (f) { f.printf("{\"phase\":%.4f,\"doy\":%d}\n", g_moon_phase, doy); f.close(); }
    }
    g_prefs.begin("nocturne", false);
    g_prefs.putFloat("moon_phase", g_moon_phase);
    g_prefs.putInt("moon_day",     g_moon_cache_day);
    g_prefs.end();
}


// =============================================================================
//  WiFi -- one-shot NTP, then kill the radio
// =============================================================================
struct WifiCred { String ssid; String pass; };
static WifiCred g_wifi_list[6];
static int      g_wifi_count = 0;

static void wifi_load_from_sd() {
    g_wifi_count = 0;
    File f = SD_MMC.open("/WATCH/wifi/config.txt", FILE_READ);
    if (f) {
        while (f.available() && g_wifi_count < 5) {
            String line = f.readStringUntil('\n'); line.trim();
            if (!line.length() || line.startsWith("#")) continue;
            int eq = line.indexOf('='); if (eq < 0) continue;
            String k = line.substring(0, eq); k.trim();
            String v = line.substring(eq + 1); v.trim();
            for (int i = 1; i <= 5; i++) {
                String si = "WIFI" + String(i) + "_SSID";
                String pi = "WIFI" + String(i) + "_PASS";
                if (k == si) { while (g_wifi_count < i) g_wifi_count++;
                               g_wifi_list[i-1].ssid = v; }
                else if (k == pi) g_wifi_list[i-1].pass = v;
            }
            if (k == "GMT_OFFSET") g_gmt_offset_sec = v.toInt() * 3600;
        }
        f.close();
    }
    g_wifi_list[g_wifi_count].ssid = HC_WIFI_SSID;
    g_wifi_list[g_wifi_count].pass = HC_WIFI_PASS;
    g_wifi_count++;
}
static bool wifi_sync_once() {
    wifi_load_from_sd();
    WiFi.mode(WIFI_STA);
    for (int i = 0; i < g_wifi_count; i++) {
        if (!g_wifi_list[i].ssid.length()) continue;
        WiFi.begin(g_wifi_list[i].ssid.c_str(), g_wifi_list[i].pass.c_str());
        uint32_t t0 = millis();
        while (millis() - t0 < 4000) {
            if (WiFi.status() == WL_CONNECTED) {
                configTime(g_gmt_offset_sec, 0, "pool.ntp.org");
                delay(800);
                struct tm tm;
                if (getLocalTime(&tm, 200)) {
                    g_time_valid = true;
                    time_t nowt; time(&nowt);
                    g_last_ntp_ok = (uint32_t)nowt;
                }
                WiFi.disconnect(true, true);
                WiFi.mode(WIFI_OFF);
                return true;
            }
            delay(120);
        }
    }
    WiFi.disconnect(true, true);
    WiFi.mode(WIFI_OFF);
    return false;
}


// =============================================================================
//  SD bootstrap (creates the folders we'll use)
// =============================================================================
static void sd_bootstrap() {
    SD_MMC.mkdir("/WATCH");
    SD_MMC.mkdir("/WATCH/bin");
    SD_MMC.mkdir("/WATCH/nocturne");
    SD_MMC.mkdir("/WATCH/nocturne/sessions");
    SD_MMC.mkdir("/WATCH/wifi");
}


// =============================================================================
//  CUSTOM SPLASH -- piano-ivory pulse + "NOCTURNE//OS"
// =============================================================================
static void nocturne_splash(uint16_t duration_ms) {
    // ------------------------------------------------------------------
    //  NOCTURNE//OS BOOT SEQUENCE  --  3 acts in ~2.2s
    //  Act I  (0.0 - 0.7s)  88-key piano cascade sweep, left to right
    //  Act II (0.7 - 1.4s)  golden ring breathes in, wordmark fades up
    //  Act III(1.4 - 2.2s)  poetic subtitle types in, soft piano tap
    // ------------------------------------------------------------------
    co5300_set_brightness(0);
    uint32_t t0 = millis();
    const uint32_t D1 = duration_ms * 0.32f;   // act 1
    const uint32_t D2 = duration_ms * 0.32f;   // act 2
    const uint32_t D3 = duration_ms - D1 - D2; // act 3

    const int cx = LCD_WIDTH/2, cy = LCD_HEIGHT/2 - 20;
    const char *poetic = THEMES[g_theme_idx].ambience;
    uint32_t accent = THEMES[g_theme_idx].c_accent;
    uint8_t  ar = (accent >> 16) & 0xFF, ag = (accent >> 8) & 0xFF, ab = accent & 0xFF;

    // --- ACT I : piano-key cascade ------------------------------------
    uint32_t t = millis();
    while (millis() - t < D1) {
        float p = (millis() - t) / (float)D1;
        co5300_set_brightness((uint8_t)(p * 220));
        gfx->fillScreen(0x0000);
        // 24 vertical "keys" sweep left → right with a soft brass glow
        for (int i = 0; i < 24; i++) {
            float kp = (p * 1.5f) - (i / 24.0f);
            if (kp < 0 || kp > 1) continue;
            float a = sinf(kp * 3.14159f);    // smooth in/out
            uint8_t r = (uint8_t)(ar * a), g = (uint8_t)(ag * a), b = (uint8_t)(ab * a);
            int x = (LCD_WIDTH * i) / 24;
            int w = LCD_WIDTH / 28;
            int h = (int)(60 + 80 * a);
            gfx->fillRect(x, cy - h/2, w, h, gfx->color565(r, g, b));
        }
        delay(20);
    }

    // --- ACT II : breathing ring + wordmark fade ---------------------
    t = millis();
    while (millis() - t < D2) {
        float p = (millis() - t) / (float)D2;
        gfx->fillScreen(0x0000);
        // golden ring
        int rmax = 130;
        for (int rr = rmax; rr > 8; rr -= 4) {
            float a = (1.0f - (rmax - rr) / (float)rmax) * (0.35f + 0.65f * sinf(p * 3.14f));
            uint8_t r = (uint8_t)(ar * a), g = (uint8_t)(ag * a), b = (uint8_t)(ab * a);
            gfx->drawCircle(cx, cy, rr, gfx->color565(r, g, b));
        }
        // 4 piano "keys" tucked under the ring
        for (int i = 0; i < 4; i++) {
            int x = cx - 60 + i * 40;
            int hh = (int)(28 + 22 * sinf(p * 6.28f + i * 0.9f));
            gfx->fillRect(x, cy + 70, 6, hh, gfx->color565(220, 210, 180));
        }
        // wordmark fades up
        uint8_t fade = (uint8_t)(p * 240);
        gfx->setTextColor(gfx->color565(fade, (uint8_t)(fade*0.92f), (uint8_t)(fade*0.75f)));
        gfx->setTextSize(3);
        gfx->setCursor(cx - 110, cy + 130);
        gfx->print("NOCTURNE");
        gfx->setTextSize(2);
        gfx->setCursor(cx + 26, cy + 138);
        gfx->setTextColor(gfx->color565(ar, ag, ab));
        gfx->print("//OS");
        delay(20);
    }

    // --- ACT III : poetic subtitle types in (silent build, no audio cue) --
    t = millis();
    const int N = strlen(poetic);
    while (millis() - t < D3) {
        float p = (millis() - t) / (float)D3;
        int chars = (int)(p * (N + 4));
        if (chars > N) chars = N;
        // hold the act-II frame
        gfx->fillRect(cx - 180, cy + 170, 360, 28, 0x0000);
        gfx->setTextColor(gfx->color565(ar, ag, ab));
        gfx->setTextSize(1);
        gfx->setCursor(cx - (chars * 6) / 2, cy + 178);
        for (int i = 0; i < chars; i++) gfx->print(poetic[i]);
        if (chars < N && ((millis() / 200) & 1)) gfx->print("_");
        delay(35);
    }
}


// =============================================================================
//  v2.0 :: REAL AUDIO ENGINE -- ES8311 codec mic in, lightweight DFT
//  Pins per Waveshare demo /examples/Arduino-v3.2.0/examples/08_ES8311/
//      MCLK=16  BCLK=41  WS=45  DOUT=40  DIN=42   PA_EN=46
//  We never include the ES8311 driver source here (single-file rule); instead
//  we drive the codec straight over I2C with a minimal register set so this
//  stays a clean drop-in. If the codec doesn't ACK we fall back to synthesized
//  energy and the rest of the UI is unaffected.
// =============================================================================
#include "driver/i2s_std.h"

#define NOCT_I2S_MCLK 16
#define NOCT_I2S_BCLK 41
#define NOCT_I2S_WS   45
#define NOCT_I2S_DOUT 40
#define NOCT_I2S_DIN  42
#define NOCT_PA_EN    46
#define ES8311_ADDR   0x18
#define NOCT_AUDIO_SR 16000

static i2s_chan_handle_t g_i2s_rx = NULL;
static i2s_chan_handle_t g_i2s_tx = NULL;
static bool   g_audio_on     = false;
static float  g_band_bass    = 0.0f;
static float  g_band_mid     = 0.0f;
static float  g_band_treble  = 0.0f;
static uint8_t g_detected_bpm = 60;

static void es8311_w(uint8_t r, uint8_t v) {
    Wire.beginTransmission(ES8311_ADDR); Wire.write(r); Wire.write(v);
    Wire.endTransmission();
}
static bool es8311_probe() {
    Wire.beginTransmission(ES8311_ADDR);
    return Wire.endTransmission() == 0;
}
static void es8311_minimal_init() {
    // canonical Waveshare-aligned register kick for 16 kHz mono mic-in.
    es8311_w(0x00, 0x1F); es8311_w(0x01, 0x30); es8311_w(0x02, 0x10);
    es8311_w(0x03, 0x10); es8311_w(0x16, 0x24); es8311_w(0x04, 0x10);
    es8311_w(0x05, 0x00); es8311_w(0x06, 0x03); es8311_w(0x07, 0x00);
    es8311_w(0x08, 0xFF); es8311_w(0x09, 0x0C); es8311_w(0x0A, 0x0C);
    es8311_w(0x0B, 0x00); es8311_w(0x0C, 0x00); es8311_w(0x10, 0x1F);
    es8311_w(0x11, 0x7F); es8311_w(0x14, 0x18);   // ADC PGA gain
    es8311_w(0x17, 0xBF); es8311_w(0x0E, 0x02);   // line-in OFF, mic ON
    es8311_w(0x12, 0x00); es8311_w(0x13, 0x10);
    es8311_w(0x37, 0x08); es8311_w(0x44, 0x08);
    es8311_w(0x32, 0x80);                          // mic gain
}
// v2.1 SILENT BUILD --------------------------------------------------------
// NocturneOS is intentionally a SILENT ambient OS. The earlier build held the
// PA_EN amp pin HIGH and left the I2S TX channel enabled, which produced a
// constant high-pitched whine from the codec's idle DAC noise (and carried
// over between tiles because the amp never shut off).
//
// New rules:
//   * NEVER drive PA_EN HIGH. We force it LOW and keep it LOW forever.
//   * NEVER allocate or enable the I2S TX channel.
//   * Hard-mute the ES8311 DAC + headphone path at init.
//   * Only the RX path (mic) stays alive -- it feeds the visualizer FFT,
//     it does not produce any sound.
// audio_play_tick() below is a permanent no-op. Settings UI sound toggle is
// kept (just disables visual side-effects of taps), but no PCM ever leaves
// the SoC.
static bool audio_begin() {
    // amp permanently OFF
    pinMode(NOCT_PA_EN, OUTPUT);
    digitalWrite(NOCT_PA_EN, LOW);

    if (!es8311_probe()) return false;
    es8311_minimal_init();
    // hard-mute DAC + HP out so the codec stays silent even if a stray
    // I2S word ever clocked through.
    es8311_w(0x31, 0x60);   // DAC_MUTE = 1, DAC_DSM_MUTE = 1
    es8311_w(0x32, 0x00);   // DAC volume -> -inf dB
    es8311_w(0x37, 0xC8);   // DAC ramp off, mute on
    es8311_w(0x12, 0x20);   // power down DAC + HP driver

    // RX-only channel setup (mic for visualizer FFT). TX is NEVER created.
    i2s_chan_config_t cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    if (i2s_new_channel(&cfg, NULL, &g_i2s_rx) != ESP_OK) return false;
    i2s_std_config_t std = {
        .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(NOCT_AUDIO_SR),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = { .mclk = (gpio_num_t)NOCT_I2S_MCLK, .bclk = (gpio_num_t)NOCT_I2S_BCLK,
                      .ws = (gpio_num_t)NOCT_I2S_WS, .dout = (gpio_num_t)-1,
                      .din = (gpio_num_t)NOCT_I2S_DIN,
                      .invert_flags = {0,0,0} }
    };
    if (i2s_channel_init_std_mode(g_i2s_rx, &std) != ESP_OK) return false;
    i2s_channel_enable(g_i2s_rx);
    g_i2s_tx = NULL;
    return true;
}

// Lightweight 3-band sliding energy (avoids dragging in a full FFT lib).
// Reads ~256 stereo samples, integrates abs() through 3 cheap IIR HP/BP/LP
// filters, then exponentially smooths into bass/mid/treble bands.
#define AUDIO_FRAMES 256
static int16_t g_audio_buf[AUDIO_FRAMES * 2];

static void audio_tick() {
    if (!g_audio_on) return;
    size_t br = 0;
    if (i2s_channel_read(g_i2s_rx, g_audio_buf, sizeof(g_audio_buf), &br, 4) != ESP_OK) return;
    int n = br / 4;
    if (n < 16) return;
    // simple band split via differences (cheap, ~no FFT cost)
    float lp = 0, last = 0;
    float bass_e = 0, mid_e = 0, tre_e = 0;
    int zc = 0; int prev_sign = 0;
    for (int i = 0; i < n; i++) {
        float s = (float)g_audio_buf[i*2] / 32768.0f;
        lp = lp * 0.85f + s * 0.15f;       // ~LP
        float hp = s - lp;                  // HP residue
        float bp = (s - last);              // crude BP-ish (first-diff)
        last = s;
        bass_e += fabsf(lp);
        mid_e  += fabsf(bp - hp * 0.5f);
        tre_e  += fabsf(hp);
        int sign = (s > 0.02f) - (s < -0.02f);
        if (sign && sign != prev_sign) { zc++; prev_sign = sign; }
    }
    bass_e /= n; mid_e /= n; tre_e /= n;
    g_band_bass   += (bass_e * 6.0f  - g_band_bass)   * 0.18f;
    g_band_mid    += (mid_e  * 5.0f  - g_band_mid)    * 0.18f;
    g_band_treble += (tre_e  * 4.0f  - g_band_treble) * 0.18f;
    if (g_band_bass   > 1) g_band_bass   = 1;
    if (g_band_mid    > 1) g_band_mid    = 1;
    if (g_band_treble > 1) g_band_treble = 1;
    // ZCR -> very rough BPM estimate via slow zero-crossing-rate smoothing
    static float zcr = 0; zcr = zcr * 0.95f + (zc * 60.0f / (n / (float)NOCT_AUDIO_SR)) * 0.05f;
    g_detected_bpm = (uint8_t)constrain((int)(zcr * 0.6f), 40, 180);
}

static float audio_energy() {
    if (g_audio_on) return (g_band_bass + g_band_mid + g_band_treble) / 3.0f;
    return g_viz_energy;
}

// =============================================================================
//  LISTENING STATES  --  auto-classify the playing piece and bias AURA_AUTO
//
//  Heuristic over the 3-band sliding energy + tempo histogram. Runs every
//  AURA tick (~1 Hz); persists a soft preference into NVS so the watch slowly
//  learns "you tend to play Cordelia at 11pm" → leans toward that palette.
//
//  Rules (deliberately tunable, not ML):
//    piano-dominant      (mid >> bass, treble < mid*0.6, BPM 45-80)   → Cordelia | Benoni
//    dreamy synth        (treble ~ mid, BPM 75-95, broadband)         → Chasing Kou
//    waltz 3/4           (bass spikes every ~3 beats, BPM 100-120)    → Merry-Go-Round
//    string quartet      (mid strong, no transients, BPM 40-60)       → Cordelia
//    harpsichord 6/8     (treble pulses, BPM 65-78)                   → Golden Brown
//    cinematic brass     (bass dominant, slow swells, BPM 50-65)      → Lancelot
//    everything else                                                  → no change
// =============================================================================
static const char* listening_state_classify();

static float    g_bpm_estimate   = 0.0f;     // running tempo guess
static uint32_t g_last_bass_ms   = 0;
static uint32_t g_bass_periods[8] = {0};
static uint8_t  g_bass_idx       = 0;
static int8_t   g_listening_suggested_theme_idx = -1;
static uint32_t g_listening_lock_ms = 0;     // ignore retriggers for N ms

static void listening_tempo_update() {
    // detect a bass transient: bass > 0.55 with rising edge
    static float prev_bass = 0.0f;
    uint32_t now = millis();
    if (g_band_bass > 0.55f && prev_bass < 0.45f && (now - g_last_bass_ms) > 250) {
        if (g_last_bass_ms != 0) {
            uint32_t dt = now - g_last_bass_ms;
            g_bass_periods[g_bass_idx++ & 7] = dt;
            // median of last 8 periods
            uint32_t s[8]; memcpy(s, g_bass_periods, sizeof(s));
            for (int i = 0; i < 7; i++) for (int j = i+1; j < 8; j++)
                if (s[j] < s[i]) { uint32_t t = s[i]; s[i] = s[j]; s[j] = t; }
            uint32_t med = (s[3] + s[4]) / 2;
            if (med > 0) g_bpm_estimate = 60000.0f / med;
        }
        g_last_bass_ms = now;
    }
    prev_bass = g_band_bass;
}

static const char* listening_state_classify() {
    if (!g_audio_on) return NULL;
    float b = g_band_bass, m = g_band_mid, t = g_band_treble;
    float bpm = g_bpm_estimate;

    // waltz 3/4 -- Merry-Go-Round (Hisaishi, ~110 BPM, bass on beat-1)
    if (bpm >= 100 && bpm <= 122 && b > 0.35f && m > 0.25f) return "merry";

    // dreamy synth -- Chasing Kou (broadband, treble high, ~75-95 BPM)
    if (t > 0.40f && t > b * 0.9f && bpm >= 72 && bpm <= 96) return "chasing";

    // harpsichord 6/8 -- Golden Brown (treble pulses, ~65-78 BPM)
    if (t > 0.45f && m > 0.30f && bpm >= 62 && bpm <= 80 && b < 0.30f) return "golden";

    // cinematic brass -- Lancelot (bass dominant slow swells)
    if (b > 0.55f && b > m * 1.6f && bpm > 0 && bpm < 66) return "lancelot";

    // string quartet -- Cordelia (mid strong, slow, low transients)
    if (m > 0.40f && m > t * 1.3f && (bpm == 0 || bpm < 62)) return "cordelia";

    // piano-dominant (mid >> bass, soft treble) -- Cordelia or Benoni
    // -- prefer Benoni when the piece is in lower-mid range (sweeping chords full keyboard)
    if (m > 0.35f && b < m * 0.7f && t < m * 0.7f && bpm >= 45 && bpm <= 80) {
        return (b > 0.20f) ? "benoni" : "cordelia";
    }

    return NULL;
}

// Resolve theme key string to THEMES[] index. Returns -1 if unknown.
static int8_t theme_index_by_key(const char *key) {
    if (!key) return -1;
    const int N = sizeof(THEMES) / sizeof(THEMES[0]);
    for (int i = 0; i < N; i++) if (strcmp(THEMES[i].id, key) == 0) return i;
    return -1;
}

// Called from the aura tick: if a confident classification holds for ~3 ticks
// and AURA is in AUTO mode, smoothly nudge the theme. Lock for 12s to avoid
// flapping. Persists last suggestion to NVS for the Aura-learning histogram.
static void listening_state_tick() {
    listening_tempo_update();
    if (g_aura_mode != AURA_AUTO) return;
    uint32_t now = millis();
    if (now < g_listening_lock_ms) return;

    const char *suggested = listening_state_classify();
    if (!suggested) return;
    int8_t idx = theme_index_by_key(suggested);
    if (idx < 0 || idx == g_theme_idx) return;

    // require the same suggestion twice in a row before committing
    static int8_t prev_suggested = -1;
    if (idx != prev_suggested) { prev_suggested = idx; return; }

    g_theme_idx = idx;
    g_listening_suggested_theme_idx = idx;
    g_listening_lock_ms = now + 12000;
    g_prefs.putUChar("theme", (uint8_t)idx);
    g_prefs.putUChar("ls_last", (uint8_t)idx);   // for Aura-learning histogram
    rebuild_all_cards();
    show_toast(suggested, 1500);
}

// Soft UI "piano tap" -- SILENT BUILD: this is now a permanent no-op.
// The function signature is kept so every existing call site compiles
// unchanged. No PCM is ever written, the amp is never enabled, the TX
// channel does not exist. The Settings "Piano taps" switch still works
// as a user preference but has no audio effect.
static void audio_play_tick(uint16_t /*freq_hz*/) {
    return;
}


// =============================================================================
//  v2.0 :: QMI8658 gestures -- wrist-flick double-tap shifts mode
// =============================================================================
#define QMI_ADDR_PRIMARY   0x6B
#define QMI_ADDR_SECONDARY 0x6A
static uint8_t g_qmi_addr = 0;
static bool    g_has_imu  = false;

static bool qmi_w(uint8_t r, uint8_t v) {
    Wire.beginTransmission(g_qmi_addr); Wire.write(r); Wire.write(v);
    return Wire.endTransmission() == 0;
}
static bool qmi_r(uint8_t r, uint8_t *o, size_t n) {
    Wire.beginTransmission(g_qmi_addr); Wire.write(r);
    if (Wire.endTransmission(false) != 0) return false;
    Wire.requestFrom((int)g_qmi_addr, (int)n);
    for (size_t i = 0; i < n && Wire.available(); i++) o[i] = Wire.read();
    return true;
}
static void qmi_init() {
    for (uint8_t a : { (uint8_t)QMI_ADDR_PRIMARY, (uint8_t)QMI_ADDR_SECONDARY }) {
        Wire.beginTransmission(a);
        if (Wire.endTransmission() == 0) {
            g_qmi_addr = a;
            uint8_t who = 0; qmi_r(0x00, &who, 1);
            if (who == 0x05) {
                qmi_w(0x02, 0x60); qmi_w(0x03, 0x95); qmi_w(0x08, 0x01);
                g_has_imu = true;
                return;
            }
        }
    }
}
static float qmi_az() {
    if (!g_has_imu) return 0;
    uint8_t b[6]; if (!qmi_r(0x35, b, 6)) return 0;
    int16_t az = (int16_t)((b[5] << 8) | b[4]);
    return az / 16384.0f;
}
// Returns true when the user does two short flicks within ~600 ms.
static uint32_t g_last_tap = 0; static uint8_t g_tap_n = 0;
static bool gesture_check() {
    static float prev = 0; float z = qmi_az();
    float d = fabsf(z - prev); prev = z;
    bool hit = (d > 0.45f);
    uint32_t now = millis();
    if (hit && now - g_last_tap > 80) {
        if (now - g_last_tap < 600) { g_tap_n++; }
        else                         { g_tap_n = 1; }
        g_last_tap = now;
        if (g_tap_n >= 2) { g_tap_n = 0; return true; }
    }
    if (now - g_last_tap > 700) g_tap_n = 0;
    return false;
}


// =============================================================================
//  v2.0 :: AURA LEARNING -- log theme*hour usage into NVS, bias AUTO mode
// =============================================================================
#define AURA_HIST_SIZE 24
static uint16_t g_aura_hist[AURA_HIST_SIZE] = {0};   // counts theme idx per hour-of-day
static uint8_t  g_aura_hist_theme[AURA_HIST_SIZE] = {0};

static void aura_learn_load() {
    g_prefs.begin("aura", true);
    g_prefs.getBytes("hist",  g_aura_hist,       sizeof(g_aura_hist));
    g_prefs.getBytes("theme", g_aura_hist_theme, sizeof(g_aura_hist_theme));
    g_prefs.end();
}
static void aura_learn_tick() {
    static uint32_t last = 0; if (millis() - last < 60000) return; last = millis();
    time_t t; time(&t); struct tm tm; localtime_r(&t, &tm);
    uint8_t h = tm.tm_hour % 24;
    if (g_aura_hist_theme[h] == g_theme_idx) g_aura_hist[h]++;
    else if (g_aura_hist[h] == 0)             { g_aura_hist_theme[h] = g_theme_idx; g_aura_hist[h] = 1; }
    else                                       g_aura_hist[h]--;
    if ((millis() / 60000) % 10 == 0) {
        g_prefs.begin("aura", false);
        g_prefs.putBytes("hist",  g_aura_hist,       sizeof(g_aura_hist));
        g_prefs.putBytes("theme", g_aura_hist_theme, sizeof(g_aura_hist_theme));
        g_prefs.end();
    }
}
// In AUTO mode, suggest theme based on learned hour. Returns -1 if unknown.
static int aura_learned_theme_for_now() {
    time_t t; time(&t); struct tm tm; localtime_r(&t, &tm);
    uint8_t h = tm.tm_hour % 24;
    if (g_aura_hist[h] >= 3) return g_aura_hist_theme[h];
    return -1;
}


// =============================================================================
//  v2.0 :: WALLPAPER PASSTHROUGH -- REMOVED per design directive.
//  Nocturne intentionally keeps the face as the focus. No image wallpapers,
//  just the timeless clock face on AMOLED black.
// =============================================================================
static lv_obj_t *g_wallpaper_img = NULL;   // always NULL; kept for ABI parity
static void wallpaper_try_load() { /* intentionally empty */ }


// =============================================================================
//  v2.0 :: SIX CLOCK FACES -- canvas-rendered per theme.clock_kind
//           ("luxury", "piano", "moon", "vinyl", "rings", "cathedral")
// =============================================================================
#define FACE_W 360
#define FACE_H 360
static lv_obj_t  *face_canvas = NULL;
static lv_color_t *face_buf   = NULL;

static void face_draw_hands(int hour, int minute, int second) {
    lv_color_t accent = LC(T()->c_accent), warm = LC(T()->c_warm);
    int cx = FACE_W/2, cy = FACE_H/2;
    float h_ang = ((hour % 12) + minute / 60.0f) * 30.0f - 90.0f;
    float m_ang = (minute + second / 60.0f)      * 6.0f  - 90.0f;
    float s_ang =  second                        * 6.0f  - 90.0f;
    auto hand = [&](float ang_deg, int len, int w, lv_color_t c, lv_opa_t o){
        float r = ang_deg * 3.14159f / 180.0f;
        lv_point_t pts[2] = { {(lv_coord_t)cx,(lv_coord_t)cy},
            {(lv_coord_t)(cx + cosf(r)*len), (lv_coord_t)(cy + sinf(r)*len)} };
        lv_draw_line_dsc_t d; lv_draw_line_dsc_init(&d);
        d.color = c; d.width = w; d.opa = o; d.round_start = d.round_end = 1;
        lv_canvas_draw_line(face_canvas, pts, 2, &d);
    };
    hand(h_ang, 70, 5, warm,   LV_OPA_90);
    hand(m_ang,105, 3, accent, LV_OPA_90);
    hand(s_ang,120, 1, accent, LV_OPA_50);
    // hub
    lv_draw_rect_dsc_t r; lv_draw_rect_dsc_init(&r);
    r.bg_color = accent; r.radius = 4; r.bg_opa = LV_OPA_90;
    lv_canvas_draw_rect(face_canvas, cx-4, cy-4, 8, 8, &r);
}

static void face_render(int hour, int minute, int second) {
    if (!face_canvas) return;
    lv_canvas_fill_bg(face_canvas, LC(T()->c_bg), LV_OPA_TRANSP);
    const char *k = T()->clock_kind;
    int cx = FACE_W/2, cy = FACE_H/2;
    lv_color_t accent = LC(T()->c_accent), warm = LC(T()->c_warm);

    if (!strcmp(k, "luxury")) {
        lv_draw_arc_dsc_t a; lv_draw_arc_dsc_init(&a);
        a.color = warm;   a.width = 1; a.opa = LV_OPA_60;
        lv_canvas_draw_arc(face_canvas, cx, cy, 170, 0, 360, &a);
        a.color = accent; a.width = 2; a.opa = LV_OPA_80;
        lv_canvas_draw_arc(face_canvas, cx, cy, 160, 0, 360, &a);
        // 12 hour markers (gold pips)
        for (int i = 0; i < 12; i++) {
            float r = (i*30 - 90) * 3.14159f / 180.0f;
            int x = cx + (int)(cosf(r) * 150), y = cy + (int)(sinf(r) * 150);
            lv_draw_rect_dsc_t d; lv_draw_rect_dsc_init(&d);
            d.bg_color = (i%3==0) ? accent : warm; d.radius = 2; d.bg_opa = LV_OPA_80;
            lv_canvas_draw_rect(face_canvas, x-2, y-2, 4, (i%3==0?8:4), &d);
        }
    } else if (!strcmp(k, "piano")) {
        // 12 piano-key wedges around perimeter
        for (int i = 0; i < 12; i++) {
            float a0 = i*30 - 90, a1 = a0 + 26;
            lv_draw_arc_dsc_t d; lv_draw_arc_dsc_init(&d);
            d.color = (i%2) ? warm : accent;
            d.width = 14; d.opa = (i%2 ? LV_OPA_40 : LV_OPA_70);
            lv_canvas_draw_arc(face_canvas, cx, cy, 158, (int)a0, (int)a1, &d);
        }
    } else if (!strcmp(k, "moon")) {
        // moonphase disk + soft halo
        lv_draw_arc_dsc_t a; lv_draw_arc_dsc_init(&a);
        a.color = warm; a.width = 1; a.opa = LV_OPA_30;
        lv_canvas_draw_arc(face_canvas, cx, cy, 170, 0, 360, &a);
        // simulated phase = hour-of-day arc
        a.color = accent; a.width = 6; a.opa = LV_OPA_70;
        lv_canvas_draw_arc(face_canvas, cx, cy, 150, 270 - 90, 270 + hour*15 - 90, &a);
    } else if (!strcmp(k, "vinyl")) {
        for (int r = 60; r < 170; r += 10) {
            lv_draw_arc_dsc_t a; lv_draw_arc_dsc_init(&a);
            a.color = (r/10)%2 ? accent : warm; a.width = 1; a.opa = LV_OPA_40;
            lv_canvas_draw_arc(face_canvas, cx, cy, r, 0, 360, &a);
        }
    } else if (!strcmp(k, "rings")) {
        for (int i = 0; i < 4; i++) {
            lv_draw_arc_dsc_t a; lv_draw_arc_dsc_init(&a);
            a.color = i%2 ? warm : accent; a.width = 2; a.opa = (lv_opa_t)(LV_OPA_70 - i*15);
            lv_canvas_draw_arc(face_canvas, cx, cy, 160 - i*22, 0, 360, &a);
        }
    } else { // cathedral
        lv_draw_arc_dsc_t a; lv_draw_arc_dsc_init(&a);
        a.color = warm; a.width = 1; a.opa = LV_OPA_50;
        lv_canvas_draw_arc(face_canvas, cx, cy, 170, 0, 360, &a);
        // 4 cardinal numeral marks (I, III, VI, IX style)
        for (int i = 0; i < 4; i++) {
            float r = (i*90 - 90) * 3.14159f / 180.0f;
            int x = cx + (int)(cosf(r) * 158), y = cy + (int)(sinf(r) * 158);
            lv_draw_rect_dsc_t d; lv_draw_rect_dsc_init(&d);
            d.bg_color = accent; d.radius = 1; d.bg_opa = LV_OPA_80;
            lv_canvas_draw_rect(face_canvas, x-3, y-10, 6, 20, &d);
        }
    }
    face_draw_hands(hour, minute, second);
}


// =============================================================================
//  v2.1 :: ENTRANCE CHOREOGRAPHY -- staggered fade/scale/drift on tile change
// =============================================================================
// Each child of the active tile is faded in with an easing curve, a gentle
// upward drift (12 -> 0 px) AND a subtle scale-up (96% -> 100%). The first
// few children animate fastest, later children get a longer delay -- which
// reads as "headline first, content settles in, footer last".
static void tile_enter_anim(lv_obj_t *tile) {
    if (!tile) return;
    uint32_t n = lv_obj_get_child_cnt(tile);
    if (n == 0) return;
    // capped per-child stagger so cards with many widgets (themes, modes)
    // don't take 2+ seconds to settle.
    uint32_t stride = (n > 10) ? 28 : 55;

    for (uint32_t i = 0; i < n; i++) {
        lv_obj_t *c = lv_obj_get_child(tile, i);
        if (!c) continue;

        // start state
        lv_obj_set_style_opa(c, LV_OPA_TRANSP, 0);
        lv_obj_set_style_translate_y(c, 18, 0);

        uint32_t dly = stride * i;

        // fade 0 -> 255 ----------------------------------------------------
        lv_anim_t a; lv_anim_init(&a);
        lv_anim_set_var(&a, c);
        lv_anim_set_values(&a, 0, 255);
        lv_anim_set_time(&a, 520);
        lv_anim_set_delay(&a, dly);
        lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
        lv_anim_set_exec_cb(&a, [](void *o, int32_t v){
            lv_obj_set_style_opa((lv_obj_t *)o, (lv_opa_t)v, 0);
        });
        lv_anim_start(&a);

        // drift 18 -> 0 -----------------------------------------------------
        lv_anim_t b; lv_anim_init(&b);
        lv_anim_set_var(&b, c);
        lv_anim_set_values(&b, 18, 0);
        lv_anim_set_time(&b, 620);
        lv_anim_set_delay(&b, dly);
        lv_anim_set_path_cb(&b, lv_anim_path_ease_out);
        lv_anim_set_exec_cb(&b, [](void *o, int32_t v){
            lv_obj_set_style_translate_y((lv_obj_t *)o, (lv_coord_t)v, 0);
        });
        lv_anim_start(&b);

        // subtle scale 240 -> 256 (LVGL uses 256 = 1.0x) -------------------
        // skip on the heaviest grid items (theme swatches, mode rows) so
        // we don't break flex layout positioning.
        if (n <= 8) {
            lv_anim_t s; lv_anim_init(&s);
            lv_anim_set_var(&s, c);
            lv_anim_set_values(&s, 240, 256);
            lv_anim_set_time(&s, 560);
            lv_anim_set_delay(&s, dly);
            lv_anim_set_path_cb(&s, lv_anim_path_ease_out);
            lv_anim_set_exec_cb(&s, [](void *o, int32_t v){
                lv_obj_set_style_transform_zoom((lv_obj_t *)o, (lv_coord_t)v, 0);
            });
            lv_anim_start(&s);
        }
    }
}
static void tileview_changed_cb(lv_event_t *e) {
    lv_obj_t *act = lv_tileview_get_tile_act((lv_obj_t *)lv_event_get_target(e));
    tile_enter_anim(act);
    page_indicator_update();
    // silent build: no audio tap on card change.
}


// =============================================================================
//  v2.0 :: CONCERT MODE -- charging detected -> fullscreen visualizer
// =============================================================================
// Forward declaration: scr_root is initialized later in the "GLOBAL UI ROOT" section,
// but declared here so concert_enter() / journey_enter() can reference it.
static lv_obj_t *scr_root = nullptr;

static bool g_concert = false;
static lv_obj_t *concert_overlay = NULL;
static lv_obj_t *concert_canvas  = NULL;
static lv_color_t *concert_buf   = NULL;

static void concert_enter() {
    if (g_concert || !scr_root) return;
    g_concert = true;
    concert_overlay = lv_obj_create(scr_root);
    lv_obj_remove_style_all(concert_overlay);
    lv_obj_set_size(concert_overlay, LCD_WIDTH, LCD_HEIGHT);
    lv_obj_align(concert_overlay, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(concert_overlay, LC(T()->c_bg), 0);
    lv_obj_set_style_bg_opa(concert_overlay, LV_OPA_COVER, 0);
    lv_obj_add_flag(concert_overlay, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(concert_overlay, [](lv_event_t *){
        if (concert_overlay) { lv_obj_del(concert_overlay); concert_overlay = NULL; }
        if (concert_buf) { heap_caps_free(concert_buf); concert_buf = NULL; }
        g_concert = false;
    }, LV_EVENT_CLICKED, NULL);

    if (!concert_buf) {
        concert_buf = (lv_color_t *)heap_caps_malloc(
            (size_t)LCD_WIDTH * 300 * sizeof(lv_color_t),
            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }
    if (concert_buf) {
        concert_canvas = lv_canvas_create(concert_overlay);
        lv_canvas_set_buffer(concert_canvas, concert_buf, LCD_WIDTH, 300, LV_IMG_CF_TRUE_COLOR);
        lv_obj_set_size(concert_canvas, LCD_WIDTH, 300);
        lv_obj_align(concert_canvas, LV_ALIGN_CENTER, 0, 0);
    }
    lv_obj_t *l = lv_label_create(concert_overlay);
    lv_label_set_text(l, "CONCERT MODE  .  tap to exit");
    lv_obj_set_style_text_font(l, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(l, LC(T()->c_warm), 0);
    lv_obj_set_style_text_letter_space(l, 4, 0);
    lv_obj_align(l, LV_ALIGN_BOTTOM_MID, 0, -50);
}
static void concert_tick() {
    if (!g_concert || !concert_canvas) return;
    static float ph = 0; ph += 0.08f * (0.4f + audio_energy());
    lv_canvas_fill_bg(concert_canvas, LC(T()->c_bg), LV_OPA_COVER);
    for (int s = 0; s < 7; s++) {
        float amp = (16 + s * 14) * (0.4f + audio_energy());
        float freq = 0.012f + s * 0.005f;
        float p = ph + s * 0.6f;
        lv_color_t c = (s & 1) ? LC(T()->c_warm) : LC(T()->c_accent);
        for (int x = 0; x < LCD_WIDTH - 1; x++) {
            int y1 = 150 + (int)(amp * sinf(x * freq + p));
            int y2 = 150 + (int)(amp * sinf((x+1) * freq + p));
            lv_draw_line_dsc_t d; lv_draw_line_dsc_init(&d);
            d.color = c; d.width = 1; d.opa = LV_OPA_60;
            lv_point_t pts[2] = { {(lv_coord_t)x,(lv_coord_t)y1},{(lv_coord_t)(x+1),(lv_coord_t)y2} };
            lv_canvas_draw_line(concert_canvas, pts, 2, &d);
        }
    }
}


// =============================================================================
//  v2.0 :: JOURNEY MODE OVERLAY -- drifting horizon, cinematic
// =============================================================================
static lv_obj_t *journey_overlay = NULL;
static void journey_enter() {
    if (journey_overlay || !scr_root) return;
    journey_overlay = lv_obj_create(scr_root);
    lv_obj_remove_style_all(journey_overlay);
    lv_obj_set_size(journey_overlay, LCD_WIDTH, 80);
    lv_obj_align(journey_overlay, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_opa(journey_overlay, LV_OPA_0, 0);
    lv_obj_clear_flag(journey_overlay, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(journey_overlay, LV_OBJ_FLAG_SCROLLABLE);
}
static void journey_leave() {
    if (!journey_overlay) return;
    lv_obj_del(journey_overlay); journey_overlay = NULL;
}
static void journey_tick() {
    if (g_aura_mode != AURA_JOURNEY) { if (journey_overlay) journey_leave(); return; }
    if (!journey_overlay) journey_enter();
    // simulate drifting dots on a horizon strip
    static uint32_t last = 0; if (millis() - last < 80) return; last = millis();
    lv_obj_clean(journey_overlay);
    for (int i = 0; i < 8; i++) {
        lv_obj_t *d = lv_obj_create(journey_overlay);
        lv_obj_remove_style_all(d);
        int x = (int)((millis() / 8 + i * 60) % LCD_WIDTH);
        int y = 40 + ((i * 13) % 30);
        lv_obj_set_size(d, 2 + i%3, 2 + i%3);
        lv_obj_set_style_radius(d, 2, 0);
        lv_obj_set_style_bg_color(d, LC(T()->c_accent), 0);
        lv_obj_set_style_bg_opa(d, LV_OPA_60, 0);
        lv_obj_set_pos(d, x, y);
    }
}



// =============================================================================
//  GLOBAL UI ROOT + INFINITE CARD CAROUSEL
//  Cards (left -> right):
//   0  Clock          (the heart)
//   1  Aura Engine    (mood / time-of-day brain)
//   2  Music Visualizer
//   3  Metronome
//   4  Theme Studio   (pick one of 9)
//   5  Modes          (Night Walk / Rain / Silent / Resolve / Journey)
//   6  Settings + Back-to-Launcher
//   7  Battery (PMU)
//  (the tileview wraps automatically -- infinite swipe both ways)
// =============================================================================
#define CARD_COUNT 8

// scr_root is forward-declared earlier (above concert_enter); initialize it here.
static lv_obj_t *tileview        = nullptr;
static lv_obj_t *tiles[CARD_COUNT] = { nullptr };

// Status pill labels (visible on top of every tile)
static lv_obj_t *status_dot      = nullptr;
static lv_obj_t *status_lbl      = nullptr;
static lv_obj_t *toast_lbl       = nullptr;
static uint32_t  toast_until     = 0;

// v2.1 -- bottom page-indicator (a row of CARD_COUNT dots that highlights
// the active tile with a soft glow that gently breathes).
static lv_obj_t *page_indicator           = nullptr;
static lv_obj_t *page_dots[CARD_COUNT]    = { nullptr };

// Particle drift overlay (subtle, theme-tinted)
#define PARTICLE_N 14
struct Particle { float x, y, vx, vy, life; lv_obj_t *dot; };
static Particle g_particles[PARTICLE_N];

// Clock card widgets
static lv_obj_t *clk_face        = nullptr;
static lv_obj_t *clk_h_label     = nullptr;
static lv_obj_t *clk_sub_label   = nullptr;
static lv_obj_t *clk_mood_label  = nullptr;
static lv_obj_t *clk_ring        = nullptr;

// Aura card widgets
static lv_obj_t *aura_title      = nullptr;
static lv_obj_t *aura_mode_lbl   = nullptr;
static lv_obj_t *aura_state_lbl  = nullptr;
static lv_obj_t *aura_bar        = nullptr;
// v2.1 upgrades
static lv_obj_t *aura_halo[3]    = { nullptr };   // 3 concentric breathing rings
static lv_obj_t *viz_energy_bar  = nullptr;       // thin live energy bar on viz card
static lv_obj_t *met_tempo_lbl   = nullptr;       // Largo/Adagio/.../Presto label
static lv_obj_t *bat_charge_halo = nullptr;       // pulsing halo while charging

// Visualizer card widgets
static lv_obj_t *viz_canvas      = nullptr;
static lv_color_t *viz_buf       = nullptr;
#define VIZ_W 360
#define VIZ_H 200

// Metronome widgets
static lv_obj_t *met_pulse_arc   = nullptr;
static lv_obj_t *met_bpm_lbl     = nullptr;
static lv_obj_t *met_state_lbl   = nullptr;

// Theme studio widgets
static lv_obj_t *th_swatch_grid  = nullptr;
static lv_obj_t *th_name_lbl     = nullptr;
static lv_obj_t *th_mood_lbl     = nullptr;

// Modes card widgets
static lv_obj_t *mode_btns[6]    = { nullptr };

// Settings widgets
static lv_obj_t *set_back_btn    = nullptr;
static lv_obj_t *set_info_lbl    = nullptr;

// ---- Toast ------------------------------------------------------------------
static void show_toast(const char *msg, uint32_t ms) {
    if (!toast_lbl) return;
    lv_label_set_text(toast_lbl, msg);
    lv_obj_clear_flag(toast_lbl, LV_OBJ_FLAG_HIDDEN);
    toast_until = millis() + ms;
}
static void tick_toast() {
    if (toast_lbl && toast_until && millis() > toast_until) {
        lv_obj_add_flag(toast_lbl, LV_OBJ_FLAG_HIDDEN);
        toast_until = 0;
    }
}

// ---- v2.1 :: PAGE INDICATOR (highlight active tile, gently breathe) ---------
static void page_indicator_update() {
    if (!tileview || !page_indicator) return;
    int act = 0;
    lv_obj_t *t = lv_tileview_get_tile_act(tileview);
    if (t) act = (int)((uintptr_t)lv_obj_get_user_data(t));
    if (act < 0 || act >= CARD_COUNT) act = 0;

    float br = 0.6f + 0.4f * sinf(millis() / 600.0f);  // 0.2 .. 1.0 breath
    for (int i = 0; i < CARD_COUNT; i++) {
        if (!page_dots[i]) continue;
        bool sel = (i == act);
        lv_obj_set_style_bg_color(page_dots[i],
            sel ? LC(T()->c_accent) : LC(T()->c_muted), 0);
        uint8_t opa = sel ? (uint8_t)(170 + 70 * br) : 60;
        lv_obj_set_style_bg_opa(page_dots[i], opa, 0);
        // selected dot widens slightly into a pill
        lv_obj_set_size(page_dots[i], sel ? 14 : 6, 6);
        lv_obj_set_style_radius(page_dots[i], 3, 0);
    }
}


// =============================================================================
//  AURA ENGINE -- derives ambience from time, mode, motion
// =============================================================================
struct AuraSnapshot {
    uint8_t  hour;
    bool     is_night;
    bool     is_quiet;
    uint8_t  brightness;
    uint8_t  motion_scale;   // % of theme motion (50..120)
    uint8_t  glow_scale;     // % of theme glow   (50..130)
    const char *poetic;
};
static AuraSnapshot g_aura = { 22, true, false, 180, 100, 100, "drifting" };

static const char *poetic_for_hour(uint8_t h) {
    if (h < 5)  return "deep night, soft silver";
    if (h < 8)  return "dawn, warm amber breathing";
    if (h < 12) return "morning, cream and gold";
    if (h < 17) return "afternoon, calm strings";
    if (h < 20) return "golden hour, slow vinyl";
    if (h < 23) return "evening piano, distant rain";
    return "midnight, moonlit resolve";
}
static void aura_recompute() {
    time_t now; time(&now);
    struct tm tm; localtime_r(&now, &tm);
    g_aura.hour = tm.tm_hour;
    g_aura.is_night = (g_aura.hour < 6) || (g_aura.hour >= 21);
    g_aura.is_quiet = (g_aura.hour >= 23) || (g_aura.hour < 6);
    g_aura.poetic = poetic_for_hour(g_aura.hour);

    // base brightness from time
    uint8_t b = g_aura.is_night ? 110 : 200;
    uint8_t mscale = 100, gscale = 100;

    switch (g_aura_mode) {
        case AURA_NIGHTWALK: b = 90;  mscale = 70;  gscale = 110; break;
        case AURA_RAIN:      b = 130; mscale = 80;  gscale = 130; break;
        case AURA_SILENT:    b = 70;  mscale = 50;  gscale = 70;  break;
        case AURA_RESOLVE:   b = 160; mscale = 75;  gscale = 90;  break;
        case AURA_JOURNEY:   b = 170; mscale = 120; gscale = 120; break;
        case AURA_AUTO:      default: break;
    }
    if (g_aura.is_quiet && g_aura_mode == AURA_AUTO) { b = 70; mscale = 60; gscale = 80; }

    g_aura.brightness   = b;
    g_aura.motion_scale = mscale;
    g_aura.glow_scale   = gscale;
    co5300_set_brightness(b);
}


// =============================================================================
//  PARTICLE DRIFT (global, theme-tinted, slow)
// =============================================================================
static void particles_init(lv_obj_t *parent) {
    for (int i = 0; i < PARTICLE_N; i++) {
        Particle &p = g_particles[i];
        p.x = (float)(esp_random() % LCD_WIDTH);
        p.y = (float)(esp_random() % LCD_HEIGHT);
        p.vx = ((esp_random() % 100) / 100.0f - 0.5f) * 0.6f;
        p.vy = -0.15f - ((esp_random() % 100) / 100.0f) * 0.35f;
        p.life = (esp_random() % 600) / 100.0f;
        p.dot = lv_obj_create(parent);
        lv_obj_remove_style_all(p.dot);
        lv_obj_set_size(p.dot, 3, 3);
        lv_obj_set_style_radius(p.dot, 2, 0);
        lv_obj_set_style_bg_color(p.dot, LC(T()->c_accent), 0);
        lv_obj_set_style_bg_opa(p.dot, LV_OPA_40, 0);
        lv_obj_add_flag(p.dot, LV_OBJ_FLAG_IGNORE_LAYOUT);
        lv_obj_set_pos(p.dot, (lv_coord_t)p.x, (lv_coord_t)p.y);
    }
}
static void particles_tick() {
    uint8_t glow = (T()->glow * g_aura.glow_scale) / 100;
    uint8_t motion = (T()->motion * g_aura.motion_scale) / 100;
    float vmul = 0.5f + motion / 80.0f;
    for (int i = 0; i < PARTICLE_N; i++) {
        Particle &p = g_particles[i];
        p.x += p.vx * vmul;
        p.y += p.vy * vmul;
        p.life += 0.02f;
        if (p.y < -8 || p.x < -8 || p.x > LCD_WIDTH + 8) {
            p.x = (float)(esp_random() % LCD_WIDTH);
            p.y = LCD_HEIGHT + 4;
            p.life = 0;
        }
        lv_obj_set_pos(p.dot, (lv_coord_t)p.x, (lv_coord_t)p.y);
        uint8_t opa = (uint8_t)(20 + (glow * 1.5f) * (0.5f + 0.5f * sinf(p.life)));
        if (opa > LV_OPA_70) opa = LV_OPA_70;
        lv_obj_set_style_bg_opa(p.dot, opa, 0);
        lv_obj_set_style_bg_color(p.dot, LC(T()->c_accent), 0);
    }
}


// =============================================================================
//  GLOBAL STYLES -- restyle when theme changes
// =============================================================================
static void apply_theme_to_screen() {
    if (!scr_root) return;
    lv_obj_set_style_bg_color(scr_root, LC(T()->c_bg), 0);
    lv_obj_set_style_bg_opa  (scr_root, LV_OPA_COVER, 0);
    if (tileview) {
        lv_obj_set_style_bg_color(tileview, LC(T()->c_bg), 0);
        lv_obj_set_style_bg_opa  (tileview, LV_OPA_COVER, 0);
    }
    if (status_lbl) lv_obj_set_style_text_color(status_lbl, LC(T()->c_muted), 0);
    if (status_dot) lv_obj_set_style_bg_color(status_dot, LC(T()->c_accent), 0);
}


// =============================================================================
//  CARD 0 :: CLOCK -- the emotional anchor
// =============================================================================
static void build_card_clock(lv_obj_t *tile) {
    lv_obj_set_style_bg_color(tile, LC(T()->c_bg), 0);
    lv_obj_set_style_bg_opa(tile, LV_OPA_COVER, 0);

    // outer breathing ring (drawn as arc, sweeps slowly)
    clk_ring = lv_arc_create(tile);
    lv_obj_set_size(clk_ring, 360, 360);
    lv_obj_align(clk_ring, LV_ALIGN_CENTER, 0, -20);
    lv_arc_set_rotation(clk_ring, 270);
    lv_arc_set_bg_angles(clk_ring, 0, 360);
    lv_arc_set_angles(clk_ring, 0, 90);
    lv_obj_remove_style(clk_ring, NULL, LV_PART_KNOB);
    lv_obj_clear_flag(clk_ring, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_arc_width(clk_ring, 2, LV_PART_MAIN);
    lv_obj_set_style_arc_width(clk_ring, 2, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(clk_ring, LC(T()->c_border), LV_PART_MAIN);
    lv_obj_set_style_arc_color(clk_ring, LC(T()->c_accent), LV_PART_INDICATOR);
    lv_obj_set_style_arc_opa  (clk_ring, LV_OPA_30, LV_PART_MAIN);
    lv_obj_set_style_arc_opa  (clk_ring, LV_OPA_70, LV_PART_INDICATOR);

    // inner soft halo ring
    lv_obj_t *halo = lv_arc_create(tile);
    lv_obj_set_size(halo, 280, 280);
    lv_obj_align(halo, LV_ALIGN_CENTER, 0, -20);
    lv_arc_set_bg_angles(halo, 0, 360);
    lv_arc_set_value(halo, 0);
    lv_obj_remove_style(halo, NULL, LV_PART_KNOB);
    lv_obj_remove_style(halo, NULL, LV_PART_INDICATOR);
    lv_obj_clear_flag(halo, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_arc_width(halo, 1, LV_PART_MAIN);
    lv_obj_set_style_arc_color(halo, LC(T()->c_warm), LV_PART_MAIN);
    lv_obj_set_style_arc_opa  (halo, LV_OPA_20, LV_PART_MAIN);

    // big hours:minutes
    clk_h_label = lv_label_create(tile);
    lv_label_set_text(clk_h_label, "--:--");
    lv_obj_set_style_text_font(clk_h_label, &lv_font_montserrat_48, 0);
    lv_obj_set_style_text_color(clk_h_label, LC(T()->c_text), 0);
    lv_obj_align(clk_h_label, LV_ALIGN_CENTER, 0, -30);

    // subtitle (date)
    clk_sub_label = lv_label_create(tile);
    lv_label_set_text(clk_sub_label, "");
    lv_obj_set_style_text_font(clk_sub_label, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(clk_sub_label, LC(T()->c_muted), 0);
    lv_obj_align(clk_sub_label, LV_ALIGN_CENTER, 0, 16);

    // mood eyebrow
    clk_mood_label = lv_label_create(tile);
    lv_label_set_text(clk_mood_label, T()->ambience);
    lv_obj_set_style_text_font(clk_mood_label, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(clk_mood_label, LC(T()->c_warm), 0);
    lv_obj_set_style_text_letter_space(clk_mood_label, 3, 0);
    lv_obj_align(clk_mood_label, LV_ALIGN_CENTER, 0, 56);

    // theme name eyebrow (top)
    lv_obj_t *name = lv_label_create(tile);
    lv_label_set_text_fmt(name, "%s", T()->name);
    lv_obj_set_style_text_font(name, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(name, LC(T()->c_warm), 0);
    lv_obj_set_style_text_letter_space(name, 4, 0);
    lv_obj_align(name, LV_ALIGN_TOP_MID, 0, 70);

    // swipe hint
    lv_obj_t *hint = lv_label_create(tile);
    lv_label_set_text(hint, "swipe . infinite carousel");
    lv_obj_set_style_text_font(hint, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(hint, LC(T()->c_muted), 0);
    lv_obj_set_style_text_letter_space(hint, 2, 0);
    lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -70);
}

static void update_card_clock() {
    if (!clk_h_label) return;
    time_t now; time(&now);
    struct tm tm; localtime_r(&now, &tm);
    char buf[16]; strftime(buf, sizeof(buf), "%H:%M", &tm);
    lv_label_set_text(clk_h_label, buf);
    char dbuf[24]; strftime(dbuf, sizeof(dbuf), "%a %d %b", &tm);
    lv_label_set_text(clk_sub_label, dbuf);

    // breathing ring sweep -- driven by aura motion
    static uint16_t angle = 0;
    uint8_t motion = (T()->motion * g_aura.motion_scale) / 100;
    angle = (angle + 1 + motion/20) % 360;
    lv_arc_set_angles(clk_ring, angle, (angle + 110) % 360);

    // v2.0 -- render the analog face for this theme's clock_kind
    face_render(tm.tm_hour, tm.tm_min, tm.tm_sec);

    // update mood eyebrow with poetic
    lv_label_set_text(clk_mood_label, g_aura.poetic);
}


// =============================================================================
//  CARD 1 :: AURA ENGINE -- shows current ambience state
// =============================================================================
static void build_card_aura(lv_obj_t *tile) {
    lv_obj_set_style_bg_color(tile, LC(T()->c_bg), 0);
    lv_obj_set_style_bg_opa(tile, LV_OPA_COVER, 0);

    // v2.1 -- 3 concentric breathing halo arcs (drawn first so they sit behind)
    for (int i = 0; i < 3; i++) {
        aura_halo[i] = lv_arc_create(tile);
        int sz = 200 + i * 50;
        lv_obj_set_size(aura_halo[i], sz, sz);
        lv_obj_align(aura_halo[i], LV_ALIGN_CENTER, 0, -30);
        lv_obj_remove_style(aura_halo[i], NULL, LV_PART_KNOB);
        lv_obj_clear_flag(aura_halo[i], LV_OBJ_FLAG_CLICKABLE);
        lv_arc_set_rotation(aura_halo[i], 270 - 45 + i * 10);
        lv_arc_set_bg_angles(aura_halo[i], 0, 360);
        lv_arc_set_angles(aura_halo[i], 0, 90 + i * 30);
        lv_obj_set_style_arc_width(aura_halo[i], 2, LV_PART_MAIN);
        lv_obj_set_style_arc_width(aura_halo[i], 2, LV_PART_INDICATOR);
        lv_obj_set_style_arc_opa(aura_halo[i], LV_OPA_TRANSP, LV_PART_MAIN);
        lv_obj_set_style_arc_color(aura_halo[i], LC(T()->c_accent), LV_PART_INDICATOR);
        lv_obj_set_style_arc_opa(aura_halo[i], LV_OPA_30, LV_PART_INDICATOR);
    }

    aura_title = lv_label_create(tile);
    lv_label_set_text(aura_title, "AURA ENGINE");
    lv_obj_set_style_text_font(aura_title, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(aura_title, LC(T()->c_warm), 0);
    lv_obj_set_style_text_letter_space(aura_title, 4, 0);
    lv_obj_align(aura_title, LV_ALIGN_TOP_MID, 0, 90);

    aura_mode_lbl = lv_label_create(tile);
    lv_obj_set_style_text_font(aura_mode_lbl, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(aura_mode_lbl, LC(T()->c_text), 0);
    lv_label_set_text(aura_mode_lbl, "AUTO");
    lv_obj_align(aura_mode_lbl, LV_ALIGN_CENTER, 0, -40);

    aura_state_lbl = lv_label_create(tile);
    lv_obj_set_style_text_font(aura_state_lbl, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(aura_state_lbl, LC(T()->c_muted), 0);
    lv_label_set_text(aura_state_lbl, "");
    lv_label_set_long_mode(aura_state_lbl, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(aura_state_lbl, 320);
    lv_obj_set_style_text_align(aura_state_lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(aura_state_lbl, LV_ALIGN_CENTER, 0, 16);

    aura_bar = lv_bar_create(tile);
    lv_obj_set_size(aura_bar, 240, 4);
    lv_obj_align(aura_bar, LV_ALIGN_CENTER, 0, 80);
    lv_bar_set_range(aura_bar, 0, 100);
    lv_obj_set_style_bg_color(aura_bar, LC(T()->c_border), LV_PART_MAIN);
    lv_obj_set_style_bg_color(aura_bar, LC(T()->c_accent), LV_PART_INDICATOR);
    lv_obj_set_style_radius(aura_bar, 2, LV_PART_MAIN);
    lv_obj_set_style_radius(aura_bar, 2, LV_PART_INDICATOR);
}
static void update_card_aura() {
    if (!aura_mode_lbl) return;
    lv_label_set_text(aura_mode_lbl, AURA_NAMES[g_aura_mode]);
    char buf[96];
    snprintf(buf, sizeof(buf), "%02d:00  .  %s\nbright %d  .  motion %d%%  .  glow %d%%",
             g_aura.hour, g_aura.poetic, g_aura.brightness,
             g_aura.motion_scale, g_aura.glow_scale);
    lv_label_set_text(aura_state_lbl, buf);
    lv_bar_set_value(aura_bar, g_aura.brightness * 100 / 255, LV_ANIM_ON);

    // v2.1 -- breathing halos: each ring rotates at a different speed and
    // pulses opacity with a slow sin wave. Driven by aura motion scale.
    float t = millis() / 1000.0f;
    uint8_t motion = (T()->motion * g_aura.motion_scale) / 100;
    for (int i = 0; i < 3; i++) {
        if (!aura_halo[i]) continue;
        int rot = (int)(t * (8 + i * 6) * (1 + motion / 60.0f)) % 360;
        int span = 60 + i * 50 + (int)(20 * sinf(t * 0.7f + i));
        lv_arc_set_angles(aura_halo[i], rot, (rot + span) % 360);
        uint8_t opa = (uint8_t)(40 + 40 * sinf(t * 0.9f + i * 1.3f));
        lv_obj_set_style_arc_opa(aura_halo[i], opa, LV_PART_INDICATOR);
        lv_obj_set_style_arc_color(aura_halo[i], LC(T()->c_accent), LV_PART_INDICATOR);
    }
}


// =============================================================================
//  CARD 2 :: MUSIC VISUALIZER -- strings/waves/vinyl/rings/ribbons
// =============================================================================
static void build_card_viz(lv_obj_t *tile) {
    lv_obj_set_style_bg_color(tile, LC(T()->c_bg), 0);
    lv_obj_set_style_bg_opa(tile, LV_OPA_COVER, 0);

    lv_obj_t *eye = lv_label_create(tile);
    lv_label_set_text(eye, "VISUALIZER");
    lv_obj_set_style_text_font(eye, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(eye, LC(T()->c_warm), 0);
    lv_obj_set_style_text_letter_space(eye, 4, 0);
    lv_obj_align(eye, LV_ALIGN_TOP_MID, 0, 90);

    if (!viz_buf) {
        viz_buf = (lv_color_t *)heap_caps_malloc(
            VIZ_W * VIZ_H * sizeof(lv_color_t), MALLOC_CAP_8BIT);
    }
    viz_canvas = lv_canvas_create(tile);
    if (viz_buf) {
        lv_canvas_set_buffer(viz_canvas, viz_buf, VIZ_W, VIZ_H, LV_IMG_CF_TRUE_COLOR);
    }
    lv_obj_set_size(viz_canvas, VIZ_W, VIZ_H);
    lv_obj_align(viz_canvas, LV_ALIGN_CENTER, 0, 0);

    lv_obj_t *kind = lv_label_create(tile);
    lv_label_set_text_fmt(kind, "%s . %s", T()->viz_kind, T()->name);
    lv_obj_set_style_text_font(kind, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(kind, LC(T()->c_muted), 0);
    lv_obj_set_style_text_letter_space(kind, 3, 0);
    lv_obj_align(kind, LV_ALIGN_BOTTOM_MID, 0, -70);

    // v2.1 -- live energy bar (tracks audio_energy() in real time)
    viz_energy_bar = lv_bar_create(tile);
    lv_obj_set_size(viz_energy_bar, 280, 3);
    lv_obj_align(viz_energy_bar, LV_ALIGN_BOTTOM_MID, 0, -50);
    lv_bar_set_range(viz_energy_bar, 0, 100);
    lv_obj_set_style_bg_color(viz_energy_bar, LC(T()->c_border), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(viz_energy_bar, LV_OPA_40, LV_PART_MAIN);
    lv_obj_set_style_bg_color(viz_energy_bar, LC(T()->c_accent), LV_PART_INDICATOR);
    lv_obj_set_style_radius(viz_energy_bar, 2, LV_PART_MAIN);
    lv_obj_set_style_radius(viz_energy_bar, 2, LV_PART_INDICATOR);
}

// Synthesize "music energy" -- fallback when ES8311 isn't available
static void synth_music_energy() {
    float t = millis() / 1000.0f;
    float a = 0.4f + 0.25f * sinf(t * 0.7f) + 0.15f * sinf(t * 2.3f + 1.0f);
    float bpm_pulse = sinf((millis() / 60000.0f) * g_metro_bpm * 6.28f);
    a += 0.1f * bpm_pulse;
    if (a < 0.1f) a = 0.1f;
    if (a > 1.0f) a = 1.0f;
    g_viz_energy += (a - g_viz_energy) * 0.08f;
}

static void update_card_viz() {
    if (!viz_canvas || !viz_buf) return;
    if (g_audio_on) g_viz_energy = audio_energy();   // v2.0: real mic energy
    else            synth_music_energy();
    // v2.1 -- live energy bar mirrors viz_energy (smoothed)
    if (viz_energy_bar) {
        int pct = (int)(g_viz_energy * 100.0f);
        if (pct < 0) pct = 0; if (pct > 100) pct = 100;
        lv_bar_set_value(viz_energy_bar, pct, LV_ANIM_OFF);
    }
    lv_canvas_fill_bg(viz_canvas, LC(T()->c_bg), LV_OPA_COVER);
    g_viz_phase += 0.06f * (0.5f + g_viz_energy);
    const char *k = T()->viz_kind;
    lv_color_t accent = LC(T()->c_accent);
    lv_color_t warm   = LC(T()->c_warm);

    if (strcmp(k, "strings") == 0 || strcmp(k, "waves") == 0) {
        // multi-string sine waves -- violin/orchestra
        for (int s = 0; s < 5; s++) {
            float amp = (12 + s * 8) * (0.4f + g_viz_energy);
            float freq = 0.018f + s * 0.006f;
            float ph = g_viz_phase + s * 0.6f;
            lv_color_t c = (s & 1) ? accent : warm;
            for (int x = 0; x < VIZ_W - 1; x++) {
                int y1 = VIZ_H/2 + (int)(amp * sinf(x * freq + ph));
                int y2 = VIZ_H/2 + (int)(amp * sinf((x+1) * freq + ph));
                lv_draw_line_dsc_t dsc; lv_draw_line_dsc_init(&dsc);
                dsc.color = c; dsc.width = 1; dsc.opa = LV_OPA_60;
                lv_point_t pts[2] = { {(lv_coord_t)x,(lv_coord_t)y1},{(lv_coord_t)(x+1),(lv_coord_t)y2} };
                lv_canvas_draw_line(viz_canvas, pts, 2, &dsc);
            }
        }
    } else if (strcmp(k, "rings") == 0) {
        // concentric expanding rings -- orchestral crescendo
        int cx = VIZ_W/2, cy = VIZ_H/2;
        for (int i = 0; i < 6; i++) {
            int r = (int)(20 + i * 18 + 12 * sinf(g_viz_phase + i * 0.5f) * g_viz_energy);
            lv_draw_arc_dsc_t dsc; lv_draw_arc_dsc_init(&dsc);
            dsc.color = (i & 1) ? warm : accent; dsc.width = 2;
            dsc.opa = (lv_opa_t)(LV_OPA_80 - i * 18);
            lv_canvas_draw_arc(viz_canvas, cx, cy, r, 0, 360, &dsc);
        }
    } else if (strcmp(k, "vinyl") == 0) {
        // rotating grooves -- record player
        int cx = VIZ_W/2, cy = VIZ_H/2;
        for (int i = 0; i < 8; i++) {
            int r = 25 + i * 10;
            lv_draw_arc_dsc_t dsc; lv_draw_arc_dsc_init(&dsc);
            dsc.color = (i & 1) ? accent : warm; dsc.width = 1;
            dsc.opa = (lv_opa_t)(LV_OPA_70 - i * 6);
            float ph = g_viz_phase * 0.6f + i * 0.3f;
            int a0 = (int)(ph * 57.0f) % 360;
            lv_canvas_draw_arc(viz_canvas, cx, cy, r, a0, a0 + 200 + (int)(40 * g_viz_energy), &dsc);
        }
    } else { // ribbons
        // flowing ribbon bands
        for (int b = 0; b < 4; b++) {
            for (int x = 0; x < VIZ_W; x += 2) {
                float ph = g_viz_phase + b * 1.1f;
                int y = VIZ_H/2
                      + (int)(30 * sinf(x * 0.013f + ph))
                      + (int)(15 * sinf(x * 0.04f + ph * 1.5f) * g_viz_energy)
                      + (b - 2) * 18;
                if (y < 2 || y >= VIZ_H - 2) continue;
                lv_color_t c = (b & 1) ? accent : warm;
                lv_canvas_set_px_color(viz_canvas, x, y, c);
                lv_canvas_set_px_color(viz_canvas, x, y+1, c);
            }
        }
    }
}


// =============================================================================
//  CARD 3 :: METRONOME -- aesthetic, breathing pulse ring
// =============================================================================
static void met_arc_event(lv_event_t *e);
static void met_bpm_minus_cb(lv_event_t *e);
static void met_bpm_plus_cb (lv_event_t *e);
static void met_toggle_cb   (lv_event_t *e);

static void build_card_metronome(lv_obj_t *tile) {
    lv_obj_set_style_bg_color(tile, LC(T()->c_bg), 0);
    lv_obj_set_style_bg_opa(tile, LV_OPA_COVER, 0);

    lv_obj_t *eye = lv_label_create(tile);
    lv_label_set_text(eye, "METRONOME");
    lv_obj_set_style_text_font(eye, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(eye, LC(T()->c_warm), 0);
    lv_obj_set_style_text_letter_space(eye, 4, 0);
    lv_obj_align(eye, LV_ALIGN_TOP_MID, 0, 90);

    met_pulse_arc = lv_arc_create(tile);
    lv_obj_set_size(met_pulse_arc, 220, 220);
    lv_obj_align(met_pulse_arc, LV_ALIGN_CENTER, 0, -10);
    lv_arc_set_rotation(met_pulse_arc, 270);
    lv_arc_set_bg_angles(met_pulse_arc, 0, 360);
    lv_arc_set_value(met_pulse_arc, 100);
    lv_obj_remove_style(met_pulse_arc, NULL, LV_PART_KNOB);
    lv_obj_clear_flag(met_pulse_arc, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_arc_width(met_pulse_arc, 6, LV_PART_MAIN);
    lv_obj_set_style_arc_width(met_pulse_arc, 6, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(met_pulse_arc, LC(T()->c_border), LV_PART_MAIN);
    lv_obj_set_style_arc_color(met_pulse_arc, LC(T()->c_accent), LV_PART_INDICATOR);

    met_bpm_lbl = lv_label_create(tile);
    lv_obj_set_style_text_font(met_bpm_lbl, &lv_font_montserrat_48, 0);
    lv_obj_set_style_text_color(met_bpm_lbl, LC(T()->c_text), 0);
    lv_label_set_text_fmt(met_bpm_lbl, "%d", g_metro_bpm);
    lv_obj_align(met_bpm_lbl, LV_ALIGN_CENTER, 0, -10);

    met_state_lbl = lv_label_create(tile);
    lv_obj_set_style_text_font(met_state_lbl, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(met_state_lbl, LC(T()->c_muted), 0);
    lv_label_set_text(met_state_lbl, "tap to start  .  tap 3x to set BPM");
    lv_obj_set_style_text_letter_space(met_state_lbl, 3, 0);
    lv_obj_align(met_state_lbl, LV_ALIGN_CENTER, 0, 30);

    // v2.1 -- musical tempo classification (Largo / Adagio / .../ Presto)
    met_tempo_lbl = lv_label_create(tile);
    lv_obj_set_style_text_font(met_tempo_lbl, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(met_tempo_lbl, LC(T()->c_warm), 0);
    lv_obj_set_style_text_letter_space(met_tempo_lbl, 5, 0);
    lv_label_set_text(met_tempo_lbl, "");
    lv_obj_align(met_tempo_lbl, LV_ALIGN_CENTER, 0, 56);

    // -- and + pill buttons
    auto mk_btn = [&](const char *txt, lv_align_t al, int ox, lv_event_cb_t cb) {
        lv_obj_t *b = lv_btn_create(tile);
        lv_obj_set_size(b, 56, 40);
        lv_obj_align(b, al, ox, 130);
        lv_obj_set_style_radius(b, 20, 0);
        lv_obj_set_style_bg_color(b, LC(T()->c_card), 0);
        lv_obj_set_style_border_color(b, LC(T()->c_accent), 0);
        lv_obj_set_style_border_width(b, 1, 0);
        lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, NULL);
        lv_obj_t *l = lv_label_create(b);
        lv_label_set_text(l, txt);
        lv_obj_set_style_text_color(l, LC(T()->c_text), 0);
        lv_obj_center(l);
    };
    mk_btn("-", LV_ALIGN_CENTER, -80, met_bpm_minus_cb);
    mk_btn("+", LV_ALIGN_CENTER,  80, met_bpm_plus_cb);

    // tap the arc area to toggle
    lv_obj_add_flag(met_pulse_arc, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(met_pulse_arc, met_toggle_cb, LV_EVENT_CLICKED, NULL);
}

static void met_toggle_cb(lv_event_t *e) {
    // ---- v2.1 :: TAP-TEMPO + TOGGLE ----------------------------------
    // Tap the ring repeatedly (gaps < 1500 ms) and we average the last
    // few intervals into a new BPM. A single isolated tap (no follow-up
    // within 1500 ms) toggles play/pause via the deferred logic below.
    static uint32_t taps[6] = {0};
    static uint8_t  tap_idx = 0;
    static uint8_t  tap_count = 0;

    uint32_t now = millis();
    uint32_t last = tap_idx ? taps[(tap_idx - 1) % 6] : 0;
    uint32_t gap  = last ? (now - last) : 0xFFFFFFFFu;

    if (gap < 1500 && gap > 200) {
        // tap-tempo mode -- record this gap
        taps[tap_idx % 6] = now;
        tap_idx++;
        if (tap_count < 6) tap_count++;

        if (tap_count >= 3) {
            // average the last (tap_count - 1) inter-tap intervals
            uint32_t sum = 0; uint8_t k = 0;
            uint8_t start = (tap_idx >= tap_count) ? (tap_idx - tap_count) : 0;
            for (uint8_t i = start + 1; i < tap_idx; i++) {
                uint32_t a = taps[(i - 1) % 6];
                uint32_t b = taps[i % 6];
                if (b > a) { sum += (b - a); k++; }
            }
            if (k > 0) {
                uint32_t avg_ms = sum / k;
                uint16_t bpm = (uint16_t)(60000UL / avg_ms);
                if (bpm < 30)  bpm = 30;
                if (bpm > 200) bpm = 200;
                g_metro_bpm = bpm;
                if (met_bpm_lbl) lv_label_set_text_fmt(met_bpm_lbl, "%d", g_metro_bpm);
                state_save();
                char buf[24];
                snprintf(buf, sizeof(buf), "tap tempo . %d", g_metro_bpm);
                show_toast(buf, 900);
            }
        } else {
            show_toast("tap . tap . tap", 700);
        }
        g_metro_last = now;
        return;
    }

    // isolated tap -> reset tap-tempo buffer and toggle play/pause
    tap_idx = 0; tap_count = 0; taps[0] = now;
    g_metro_on = !g_metro_on;
    g_metro_last = now;
    state_save();
    show_toast(g_metro_on ? "metronome . on" : "metronome . off", 1200);
}
static void met_bpm_minus_cb(lv_event_t *e) {
    if (g_metro_bpm > 30) g_metro_bpm--;
    lv_label_set_text_fmt(met_bpm_lbl, "%d", g_metro_bpm);
    state_save();
}
static void met_bpm_plus_cb(lv_event_t *e) {
    if (g_metro_bpm < 200) g_metro_bpm++;
    lv_label_set_text_fmt(met_bpm_lbl, "%d", g_metro_bpm);
    state_save();
}

static void update_card_metronome() {
    if (!met_pulse_arc) return;
    uint32_t period = 60000UL / g_metro_bpm;
    if (g_metro_on && millis() - g_metro_last >= period) {
        g_metro_last = millis();
        g_metro_pulse = 255;
    }
    if (g_metro_pulse > 0) g_metro_pulse = (g_metro_pulse > 12) ? g_metro_pulse - 12 : 0;
    uint16_t span = 60 + (g_metro_pulse / 2);
    lv_arc_set_angles(met_pulse_arc, 270 - span/2, 270 + span/2);
    uint8_t opa = 80 + g_metro_pulse * 175 / 255;
    if (opa > 255) opa = 255;
    lv_obj_set_style_arc_opa(met_pulse_arc, opa, LV_PART_INDICATOR);
    lv_label_set_text(met_state_lbl, g_metro_on ? "playing . BPM" : "tap arc . tap 3x to set tempo");

    // v2.1 -- musical tempo classification (Italian dynamics)
    if (met_tempo_lbl) {
        const char *t;
        uint16_t b = g_metro_bpm;
        if      (b < 40)  t = "GRAVE";
        else if (b < 60)  t = "LARGO";
        else if (b < 66)  t = "LARGHETTO";
        else if (b < 76)  t = "ADAGIO";
        else if (b < 108) t = "ANDANTE";
        else if (b < 120) t = "MODERATO";
        else if (b < 156) t = "ALLEGRO";
        else if (b < 176) t = "VIVACE";
        else if (b < 200) t = "PRESTO";
        else              t = "PRESTISSIMO";
        lv_label_set_text(met_tempo_lbl, t);
    }
}


// =============================================================================
//  CARD 4 :: THEME STUDIO -- pick 1 of 9 emotional environments
// =============================================================================
static void rebuild_all_cards();   // forward
// v2.1 -- richer Theme Studio with live preview on press
static void theme_swatch_press_cb(lv_event_t *e) {
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    if (i < 0 || i >= THEME_COUNT) return;
    // live preview: only update the headline labels, no full rebuild yet
    if (th_name_lbl) lv_label_set_text(th_name_lbl, THEMES[i].name);
    if (th_mood_lbl) lv_label_set_text(th_mood_lbl, THEMES[i].mood);
}
static void theme_swatch_release_cb(lv_event_t *e) {
    // restore preview labels to the currently committed theme on release-without-click
    if (th_name_lbl) lv_label_set_text(th_name_lbl, T()->name);
    if (th_mood_lbl) lv_label_set_text(th_mood_lbl, T()->mood);
}
static void theme_select_cb(lv_event_t *e) {
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    if (i < 0 || i >= THEME_COUNT) return;
    g_theme_idx = i;
    g_metro_bpm = THEMES[i].bpm;
    g_last_manual_theme_ms = millis();
    g_theme_since_ms = millis();
    state_save();
    show_toast(THEMES[i].name, 1400);
    rebuild_all_cards();
}
static void build_card_themes(lv_obj_t *tile) {
    lv_obj_set_style_bg_color(tile, LC(T()->c_bg), 0);
    lv_obj_set_style_bg_opa(tile, LV_OPA_COVER, 0);

    lv_obj_t *eye = lv_label_create(tile);
    lv_label_set_text(eye, "THEME STUDIO");
    lv_obj_set_style_text_font(eye, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(eye, LC(T()->c_warm), 0);
    lv_obj_set_style_text_letter_space(eye, 4, 0);
    lv_obj_align(eye, LV_ALIGN_TOP_MID, 0, 80);

    th_name_lbl = lv_label_create(tile);
    lv_obj_set_style_text_font(th_name_lbl, &lv_font_montserrat_22, 0);
    lv_obj_set_style_text_color(th_name_lbl, LC(T()->c_text), 0);
    lv_label_set_text(th_name_lbl, T()->name);
    lv_obj_align(th_name_lbl, LV_ALIGN_TOP_MID, 0, 110);

    th_mood_lbl = lv_label_create(tile);
    lv_obj_set_style_text_font(th_mood_lbl, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(th_mood_lbl, LC(T()->c_warm), 0);
    lv_label_set_text(th_mood_lbl, T()->mood);
    lv_obj_set_style_text_letter_space(th_mood_lbl, 3, 0);
    lv_obj_align(th_mood_lbl, LV_ALIGN_TOP_MID, 0, 140);

    // v2.1 -- "press to preview . tap to apply" subtitle hint
    lv_obj_t *hint = lv_label_create(tile);
    lv_label_set_text(hint, "press to preview  .  tap to apply");
    lv_obj_set_style_text_font(hint, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(hint, LC(T()->c_muted), 0);
    lv_obj_set_style_text_letter_space(hint, 2, 0);
    lv_obj_align(hint, LV_ALIGN_TOP_MID, 0, 168);

    // grid (auto-flows for 9 or 10 themes)
    th_swatch_grid = lv_obj_create(tile);
    lv_obj_remove_style_all(th_swatch_grid);
    lv_obj_set_size(th_swatch_grid, 320, 232);
    lv_obj_align(th_swatch_grid, LV_ALIGN_CENTER, 0, 48);
    lv_obj_set_layout(th_swatch_grid, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(th_swatch_grid, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_align(th_swatch_grid, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_gap(th_swatch_grid, 8, 0);
    lv_obj_clear_flag(th_swatch_grid, LV_OBJ_FLAG_SCROLLABLE);

    for (int i = 0; i < THEME_COUNT; i++) {
        bool sel = (i == g_theme_idx);
        lv_obj_t *sw = lv_obj_create(th_swatch_grid);
        lv_obj_remove_style_all(sw);
        lv_obj_set_size(sw, 92, 66);
        lv_obj_set_style_radius(sw, 14, 0);
        lv_obj_set_style_bg_color(sw, LC(THEMES[i].c_card), 0);
        lv_obj_set_style_bg_opa(sw, LV_OPA_COVER, 0);
        lv_obj_set_style_border_color(sw, LC(THEMES[i].c_accent), 0);
        lv_obj_set_style_border_width(sw, sel ? 3 : 1, 0);
        lv_obj_set_style_border_opa(sw, sel ? LV_OPA_90 : LV_OPA_30, 0);
        // glowing shadow on selected swatch
        if (sel) {
            lv_obj_set_style_shadow_color(sw, LC(THEMES[i].c_accent), 0);
            lv_obj_set_style_shadow_opa(sw, LV_OPA_60, 0);
            lv_obj_set_style_shadow_width(sw, 18, 0);
            lv_obj_set_style_shadow_spread(sw, 1, 0);
        }
        lv_obj_clear_flag(sw, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(sw, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(sw, theme_select_cb,         LV_EVENT_CLICKED,     (void *)(intptr_t)i);
        lv_obj_add_event_cb(sw, theme_swatch_press_cb,   LV_EVENT_PRESSED,     (void *)(intptr_t)i);
        lv_obj_add_event_cb(sw, theme_swatch_release_cb, LV_EVENT_PRESS_LOST,  (void *)(intptr_t)i);
        lv_obj_add_event_cb(sw, theme_swatch_release_cb, LV_EVENT_RELEASED,    (void *)(intptr_t)i);

        // v2.1 -- 3-color palette stripe at the top (accent | warm | text)
        lv_obj_t *stripe = lv_obj_create(sw);
        lv_obj_remove_style_all(stripe);
        lv_obj_set_size(stripe, 76, 6);
        lv_obj_set_style_radius(stripe, 3, 0);
        lv_obj_set_style_bg_color(stripe, LC(THEMES[i].c_accent), 0);
        lv_obj_set_style_bg_opa(stripe, LV_OPA_90, 0);
        lv_obj_align(stripe, LV_ALIGN_TOP_MID, 0, 6);

        lv_obj_t *stripeB = lv_obj_create(sw);
        lv_obj_remove_style_all(stripeB);
        lv_obj_set_size(stripeB, 24, 4);
        lv_obj_set_style_radius(stripeB, 2, 0);
        lv_obj_set_style_bg_color(stripeB, LC(THEMES[i].c_warm), 0);
        lv_obj_set_style_bg_opa(stripeB, LV_OPA_80, 0);
        lv_obj_align(stripeB, LV_ALIGN_TOP_LEFT, 8, 16);

        lv_obj_t *stripeC = lv_obj_create(sw);
        lv_obj_remove_style_all(stripeC);
        lv_obj_set_size(stripeC, 16, 4);
        lv_obj_set_style_radius(stripeC, 2, 0);
        lv_obj_set_style_bg_color(stripeC, LC(THEMES[i].c_text), 0);
        lv_obj_set_style_bg_opa(stripeC, LV_OPA_60, 0);
        lv_obj_align(stripeC, LV_ALIGN_TOP_LEFT, 36, 16);

        // theme name
        lv_obj_t *l = lv_label_create(sw);
        lv_label_set_text(l, THEMES[i].name);
        lv_obj_set_style_text_font(l, &lv_font_montserrat_12, 0);
        lv_obj_set_style_text_color(l, LC(THEMES[i].c_text), 0);
        lv_label_set_long_mode(l, LV_LABEL_LONG_DOT);
        lv_obj_set_width(l, 80);
        lv_obj_align(l, LV_ALIGN_BOTTOM_LEFT, 6, -4);

        // selected badge (small dot, top-right) for the active theme
        if (sel) {
            lv_obj_t *badge = lv_obj_create(sw);
            lv_obj_remove_style_all(badge);
            lv_obj_set_size(badge, 10, 10);
            lv_obj_set_style_radius(badge, 5, 0);
            lv_obj_set_style_bg_color(badge, LC(THEMES[i].c_accent), 0);
            lv_obj_set_style_bg_opa(badge, LV_OPA_COVER, 0);
            lv_obj_set_style_border_color(badge, LC(THEMES[i].c_text), 0);
            lv_obj_set_style_border_width(badge, 1, 0);
            lv_obj_align(badge, LV_ALIGN_TOP_RIGHT, -6, 6);
        }
    }
}


// =============================================================================
//  CARD 5 :: MODES -- Night Walk / Rain / Silent / Resolve / Journey / Auto
// =============================================================================
static void mode_cb(lv_event_t *e) {
    int m = (int)(intptr_t)lv_event_get_user_data(e);
    g_aura_mode = (AuraMode)m;
    aura_recompute();
    state_save();
    show_toast(AURA_NAMES[m], 1200);
    // refresh visual states
    for (int i = 0; i < 6; i++) {
        if (!mode_btns[i]) continue;
        bool sel = (i == m);
        lv_obj_set_style_border_color(mode_btns[i],
            sel ? LC(T()->c_accent) : LC(T()->c_border), 0);
        lv_obj_set_style_border_opa(mode_btns[i],
            sel ? LV_OPA_90 : LV_OPA_40, 0);
    }
}
static void build_card_modes(lv_obj_t *tile) {
    lv_obj_set_style_bg_color(tile, LC(T()->c_bg), 0);
    lv_obj_set_style_bg_opa(tile, LV_OPA_COVER, 0);

    lv_obj_t *eye = lv_label_create(tile);
    lv_label_set_text(eye, "ATMOSPHERES");
    lv_obj_set_style_text_font(eye, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(eye, LC(T()->c_warm), 0);
    lv_obj_set_style_text_letter_space(eye, 4, 0);
    lv_obj_align(eye, LV_ALIGN_TOP_MID, 0, 80);

    lv_obj_t *sub = lv_label_create(tile);
    lv_label_set_text(sub, "tap to enter . wrist-friendly");
    lv_obj_set_style_text_font(sub, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(sub, LC(T()->c_muted), 0);
    lv_obj_set_style_text_letter_space(sub, 2, 0);
    lv_obj_align(sub, LV_ALIGN_TOP_MID, 0, 108);

    lv_obj_t *col = lv_obj_create(tile);
    lv_obj_remove_style_all(col);
    lv_obj_set_size(col, 320, 280);
    lv_obj_align(col, LV_ALIGN_CENTER, 0, 30);
    lv_obj_set_layout(col, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(col, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_gap(col, 8, 0);

    const char *labels[6] = { "AUTO", "NIGHT WALK", "RAIN", "SILENT", "RESOLVE", "JOURNEY" };
    const char *desc[6]   = {
        "follows time-of-day",
        "dim amber  .  minimal",
        "fog blur  .  cool reflections",
        "meditative  .  muted visuals",
        "disciplined  .  focused",
        "drifting horizons  .  cinematic"
    };
    for (int i = 0; i < 6; i++) {
        lv_obj_t *b = lv_btn_create(col);
        lv_obj_set_size(b, 300, 38);
        lv_obj_set_style_radius(b, 12, 0);
        lv_obj_set_style_bg_color(b, LC(T()->c_card), 0);
        lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
        lv_obj_set_style_border_color(b,
            (i == (int)g_aura_mode) ? LC(T()->c_accent) : LC(T()->c_border), 0);
        lv_obj_set_style_border_width(b, 1, 0);
        lv_obj_set_style_border_opa(b,
            (i == (int)g_aura_mode) ? LV_OPA_90 : LV_OPA_40, 0);
        lv_obj_add_event_cb(b, mode_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        mode_btns[i] = b;

        lv_obj_t *n = lv_label_create(b);
        lv_label_set_text(n, labels[i]);
        lv_obj_set_style_text_font(n, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(n, LC(T()->c_text), 0);
        lv_obj_set_style_text_letter_space(n, 3, 0);
        lv_obj_align(n, LV_ALIGN_LEFT_MID, 12, 0);

        lv_obj_t *d = lv_label_create(b);
        lv_label_set_text(d, desc[i]);
        lv_obj_set_style_text_font(d, &lv_font_montserrat_12, 0);
        lv_obj_set_style_text_color(d, LC(T()->c_muted), 0);
        lv_obj_align(d, LV_ALIGN_RIGHT_MID, -12, 0);
    }
}


// =============================================================================
//  CARD 6 :: SETTINGS + BACK TO LAUNCHER
// =============================================================================
static uint32_t g_back_press_t = 0;
static void back_btn_cb(lv_event_t *e) {
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_PRESSED) {
        g_back_press_t = millis();
    } else if (code == LV_EVENT_LONG_PRESSED) {
        state_save();
        return_to_launcher();
    } else if (code == LV_EVENT_RELEASED) {
        uint32_t held = g_back_press_t ? (millis() - g_back_press_t) : 0;
        g_back_press_t = 0;
        if (held > 0 && held < 600) show_toast("hold to exit . to launcher", 1400);
    }
}
static void build_card_settings(lv_obj_t *tile) {
    lv_obj_set_style_bg_color(tile, LC(T()->c_bg), 0);
    lv_obj_set_style_bg_opa(tile, LV_OPA_COVER, 0);

    lv_obj_t *eye = lv_label_create(tile);
    lv_label_set_text(eye, "SETTINGS");
    lv_obj_set_style_text_font(eye, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(eye, LC(T()->c_warm), 0);
    lv_obj_set_style_text_letter_space(eye, 4, 0);
    lv_obj_align(eye, LV_ALIGN_TOP_MID, 0, 90);

    set_info_lbl = lv_label_create(tile);
    lv_obj_set_style_text_font(set_info_lbl, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(set_info_lbl, LC(T()->c_muted), 0);
    lv_label_set_long_mode(set_info_lbl, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(set_info_lbl, 320);
    lv_obj_set_style_text_align(set_info_lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(set_info_lbl, LV_ALIGN_CENTER, 0, -60);

    // ---- Piano-tap UI sound toggle row -------------------------------------
    lv_obj_t *row = lv_obj_create(tile);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, 320, 56);
    lv_obj_align(row, LV_ALIGN_CENTER, 0, 30);
    lv_obj_set_style_radius(row, 16, 0);
    lv_obj_set_style_bg_color(row, LC(T()->c_card), 0);
    lv_obj_set_style_bg_opa(row, LV_OPA_60, 0);
    lv_obj_set_style_border_color(row, LC(T()->c_border), 0);
    lv_obj_set_style_border_width(row, 1, 0);
    lv_obj_set_style_pad_all(row, 14, 0);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *lab = lv_label_create(row);
    lv_label_set_text(lab, "Silent Mode");
    lv_obj_set_style_text_font(lab, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(lab, LC(T()->c_text), 0);
    lv_obj_align(lab, LV_ALIGN_LEFT_MID, 0, -6);

    lv_obj_t *sub = lv_label_create(row);
    lv_label_set_text(sub, "all UI audio is permanently off");
    lv_obj_set_style_text_font(sub, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(sub, LC(T()->c_muted), 0);
    lv_obj_set_style_text_letter_space(sub, 2, 0);
    lv_obj_align(sub, LV_ALIGN_LEFT_MID, 0, 12);

    // Static silent-mode indicator on the right (no toggle -- always silent)
    lv_obj_t *dot = lv_obj_create(row);
    lv_obj_remove_style_all(dot);
    lv_obj_set_size(dot, 10, 10);
    lv_obj_set_style_radius(dot, 5, 0);
    lv_obj_set_style_bg_color(dot, LC(T()->c_accent), 0);
    lv_obj_set_style_bg_opa(dot, LV_OPA_90, 0);
    lv_obj_align(dot, LV_ALIGN_RIGHT_MID, -6, 0);

    // (legacy switch object kept hidden so existing NVS key stays valid)
    lv_obj_t *sw = lv_switch_create(row);
    lv_obj_add_flag(sw, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_size(sw, 1, 1);
    lv_obj_align(sw, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_set_style_bg_color(sw, LC(T()->c_border), LV_PART_MAIN);
    lv_obj_set_style_bg_color(sw, LC(T()->c_accent), LV_PART_INDICATOR);
    if (g_ui_sounds_on) lv_obj_add_state(sw, LV_STATE_CHECKED);
    lv_obj_add_event_cb(sw, [](lv_event_t *e) {
        lv_obj_t *t = lv_event_get_target(e);
        g_ui_sounds_on = lv_obj_has_state(t, LV_STATE_CHECKED);
        state_save();
        if (g_ui_sounds_on) audio_play_tick(660);
        show_toast(g_ui_sounds_on ? "piano taps . on" : "piano taps . off", 1200);
    }, LV_EVENT_VALUE_CHANGED, NULL);

    // ---- Memory Transitions toggle row -------------------------------------
    lv_obj_t *row2 = lv_obj_create(tile);
    lv_obj_remove_style_all(row2);
    lv_obj_set_size(row2, 320, 56);
    lv_obj_align(row2, LV_ALIGN_CENTER, 0, 92);
    lv_obj_set_style_radius(row2, 16, 0);
    lv_obj_set_style_bg_color(row2, LC(T()->c_card), 0);
    lv_obj_set_style_bg_opa(row2, LV_OPA_60, 0);
    lv_obj_set_style_border_color(row2, LC(T()->c_border), 0);
    lv_obj_set_style_border_width(row2, 1, 0);
    lv_obj_set_style_pad_all(row2, 14, 0);
    lv_obj_clear_flag(row2, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *lab2 = lv_label_create(row2);
    lv_label_set_text(lab2, "Memory Transitions");
    lv_obj_set_style_text_font(lab2, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(lab2, LC(T()->c_text), 0);
    lv_obj_align(lab2, LV_ALIGN_LEFT_MID, 0, -6);

    lv_obj_t *sub2 = lv_label_create(row2);
    lv_label_set_text(sub2, "theme drifts through the day");
    lv_obj_set_style_text_font(sub2, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(sub2, LC(T()->c_muted), 0);
    lv_obj_set_style_text_letter_space(sub2, 2, 0);
    lv_obj_align(sub2, LV_ALIGN_LEFT_MID, 0, 12);

    lv_obj_t *sw2 = lv_switch_create(row2);
    lv_obj_set_size(sw2, 52, 28);
    lv_obj_align(sw2, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_set_style_bg_color(sw2, LC(T()->c_border), LV_PART_MAIN);
    lv_obj_set_style_bg_color(sw2, LC(T()->c_accent), LV_PART_INDICATOR);
    if (g_memory_transitions_on) lv_obj_add_state(sw2, LV_STATE_CHECKED);
    lv_obj_add_event_cb(sw2, [](lv_event_t *e) {
        lv_obj_t *t = lv_event_get_target(e);
        g_memory_transitions_on = lv_obj_has_state(t, LV_STATE_CHECKED);
        state_save();
        show_toast(g_memory_transitions_on ? "memory . on" : "memory . off", 1200);
    }, LV_EVENT_VALUE_CHANGED, NULL);

    // back-to-launcher pill (wide)
    set_back_btn = lv_btn_create(tile);
    lv_obj_set_size(set_back_btn, 320, 60);
    lv_obj_align(set_back_btn, LV_ALIGN_BOTTOM_MID, 0, -50);
    lv_obj_set_style_radius(set_back_btn, 18, 0);
    lv_obj_set_style_bg_color(set_back_btn, LC(T()->c_card), 0);
    lv_obj_set_style_border_color(set_back_btn, LC(T()->c_accent), 0);
    lv_obj_set_style_border_width(set_back_btn, 1, 0);
    lv_obj_add_event_cb(set_back_btn, back_btn_cb, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(set_back_btn, back_btn_cb, LV_EVENT_LONG_PRESSED, NULL);
    lv_obj_add_event_cb(set_back_btn, back_btn_cb, LV_EVENT_RELEASED, NULL);

    lv_obj_t *bl = lv_label_create(set_back_btn);
    lv_label_set_text(bl, "Back to Launcher");
    lv_obj_set_style_text_font(bl, &lv_font_montserrat_18, 0);
    lv_obj_set_style_text_color(bl, LC(T()->c_text), 0);
    lv_obj_align(bl, LV_ALIGN_TOP_MID, 0, 6);

    lv_obj_t *bs = lv_label_create(set_back_btn);
    lv_label_set_text(bs, "hold  .  /WATCH/bin/launcheros.bin");
    lv_obj_set_style_text_font(bs, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(bs, LC(T()->c_muted), 0);
    lv_obj_set_style_text_letter_space(bs, 2, 0);
    lv_obj_align(bs, LV_ALIGN_BOTTOM_MID, 0, -6);
}
static void update_card_settings() {
    if (!set_info_lbl) return;
    char buf[160];
    int batt = g_has_pmu ? g_pmu.getBatteryPercent() : -1;
    snprintf(buf, sizeof(buf),
        "Nocturne OS  v2.1  .  silent build\nLauncherOS-compatible  .  AMOLED 2.06\n"
        "theme: %s  .  aura: %s\n"
        "mic: %s  .  imu: %s\n%s%s",
        T()->name, AURA_NAMES[g_aura_mode],
        g_audio_on ? "ES8311 viz" : "off",
        g_has_imu  ? "QMI8658"    : "off",
        batt >= 0 ? "battery " : "",
        batt >= 0 ? String(String(batt) + "%").c_str() : "");
    lv_label_set_text(set_info_lbl, buf);
}


// =============================================================================
//  CARD 7 :: BATTERY -- LauncherOS-style PMU readout, themed
//  Big % numeric, voltage, charging dot, ring around it, slow breathing.
// =============================================================================
static lv_obj_t *bat_arc       = nullptr;
static lv_obj_t *bat_pct_lbl   = nullptr;
static lv_obj_t *bat_volt_lbl  = nullptr;
static lv_obj_t *bat_state_lbl = nullptr;
static lv_obj_t *bat_dot       = nullptr;

static void build_card_battery(lv_obj_t *tile) {
    lv_obj_set_style_bg_color(tile, LC(T()->c_bg), 0);
    lv_obj_set_style_bg_opa(tile, LV_OPA_COVER, 0);

    lv_obj_t *eye = lv_label_create(tile);
    lv_label_set_text(eye, "BATTERY");
    lv_obj_set_style_text_font(eye, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(eye, LC(T()->c_warm), 0);
    lv_obj_set_style_text_letter_space(eye, 4, 0);
    lv_obj_align(eye, LV_ALIGN_TOP_MID, 0, 90);

    // v2.1 -- pulsing halo arc behind the main arc (only visible while charging)
    bat_charge_halo = lv_arc_create(tile);
    lv_obj_set_size(bat_charge_halo, 320, 320);
    lv_obj_align(bat_charge_halo, LV_ALIGN_CENTER, 0, -10);
    lv_obj_remove_style(bat_charge_halo, NULL, LV_PART_KNOB);
    lv_obj_clear_flag(bat_charge_halo, LV_OBJ_FLAG_CLICKABLE);
    lv_arc_set_rotation(bat_charge_halo, 135);
    lv_arc_set_bg_angles(bat_charge_halo, 0, 270);
    lv_arc_set_value(bat_charge_halo, 100);
    lv_obj_set_style_arc_width(bat_charge_halo, 14, LV_PART_INDICATOR);
    lv_obj_set_style_arc_opa(bat_charge_halo, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_arc_color(bat_charge_halo, lv_color_hex(0x00FF80), LV_PART_INDICATOR);
    lv_obj_set_style_arc_opa(bat_charge_halo, LV_OPA_TRANSP, LV_PART_INDICATOR);

    bat_arc = lv_arc_create(tile);
    lv_obj_set_size(bat_arc, 280, 280);
    lv_obj_align(bat_arc, LV_ALIGN_CENTER, 0, -10);
    lv_arc_set_rotation(bat_arc, 135);
    lv_arc_set_bg_angles(bat_arc, 0, 270);
    lv_arc_set_value(bat_arc, 100);
    lv_obj_remove_style(bat_arc, NULL, LV_PART_KNOB);
    lv_obj_clear_flag(bat_arc, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_arc_width(bat_arc, 6, LV_PART_MAIN);
    lv_obj_set_style_arc_width(bat_arc, 8, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(bat_arc, LC(T()->c_border), LV_PART_MAIN);
    lv_obj_set_style_arc_color(bat_arc, LC(T()->c_accent), LV_PART_INDICATOR);
    lv_obj_set_style_arc_opa  (bat_arc, LV_OPA_40,  LV_PART_MAIN);
    lv_obj_set_style_arc_opa  (bat_arc, LV_OPA_90,  LV_PART_INDICATOR);

    bat_pct_lbl = lv_label_create(tile);
    lv_obj_set_style_text_font(bat_pct_lbl, &lv_font_montserrat_48, 0);
    lv_obj_set_style_text_color(bat_pct_lbl, LC(T()->c_text), 0);
    lv_label_set_text(bat_pct_lbl, "--%");
    lv_obj_align(bat_pct_lbl, LV_ALIGN_CENTER, 0, -20);

    bat_volt_lbl = lv_label_create(tile);
    lv_obj_set_style_text_font(bat_volt_lbl, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(bat_volt_lbl, LC(T()->c_warm), 0);
    lv_label_set_text(bat_volt_lbl, "-- V");
    lv_obj_set_style_text_letter_space(bat_volt_lbl, 3, 0);
    lv_obj_align(bat_volt_lbl, LV_ALIGN_CENTER, 0, 24);

    // status row: charging dot + label
    bat_dot = lv_obj_create(tile);
    lv_obj_remove_style_all(bat_dot);
    lv_obj_set_size(bat_dot, 8, 8);
    lv_obj_set_style_radius(bat_dot, 4, 0);
    lv_obj_set_style_bg_color(bat_dot, LC(T()->c_muted), 0);
    lv_obj_set_style_bg_opa(bat_dot, LV_OPA_80, 0);
    lv_obj_align(bat_dot, LV_ALIGN_CENTER, -56, 60);

    bat_state_lbl = lv_label_create(tile);
    lv_obj_set_style_text_font(bat_state_lbl, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(bat_state_lbl, LC(T()->c_muted), 0);
    lv_label_set_text(bat_state_lbl, "idle");
    lv_obj_set_style_text_letter_space(bat_state_lbl, 3, 0);
    lv_obj_align(bat_state_lbl, LV_ALIGN_CENTER, 12, 60);

    // ambient line at the bottom -- LauncherOS-style detail
    lv_obj_t *foot = lv_label_create(tile);
    lv_label_set_text(foot, "AXP2101  .  3.7 V Li-Po");
    lv_obj_set_style_text_font(foot, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(foot, LC(T()->c_warm), 0);
    lv_obj_set_style_text_letter_space(foot, 3, 0);
    lv_obj_align(foot, LV_ALIGN_BOTTOM_MID, 0, -70);
}


// =============================================================================
//  BATTERY CARD UPDATE (every tick) -- LauncherOS-styled, themed
// =============================================================================
static void update_card_battery() {
    if (!bat_pct_lbl) return;
    int pct = -1; float vbat = 0.0f; bool charging = false;
    if (g_has_pmu) {
        pct = g_pmu.getBatteryPercent();
        vbat = g_pmu.getBattVoltage() / 1000.0f;
        charging = g_pmu.isCharging();
    }
    if (pct >= 0) lv_label_set_text_fmt(bat_pct_lbl, "%d%%", pct);
    else          lv_label_set_text(bat_pct_lbl, "--%");
    if (vbat > 0) lv_label_set_text_fmt(bat_volt_lbl, "%.2f V", vbat);
    else          lv_label_set_text(bat_volt_lbl, "-- V");
    if (pct >= 0) lv_arc_set_value(bat_arc, pct);

    // v2.1 -- richer status text + emoji-free state phrase
    const char *phrase = "discharging";
    if (pct < 0)              phrase = "no pmu";
    else if (charging)        phrase = (pct >= 95) ? "trickle charging" : "charging";
    else if (pct < 10)        phrase = "low . please charge";
    else if (pct < 25)        phrase = "discharging . low";
    else if (pct > 90)        phrase = "discharging . full";

    lv_label_set_text(bat_state_lbl, phrase);
    lv_obj_set_style_bg_color(bat_dot, charging ? lv_color_hex(0x00FF80)
                                                : LC(T()->c_muted), 0);

    // gentle breathing on the indicator opacity
    uint8_t breathe = 180 + (uint8_t)(40 * sinf(millis() / 1500.0f));
    lv_obj_set_style_arc_opa(bat_arc, breathe, LV_PART_INDICATOR);

    // v2.1 -- pulsing green halo behind the arc when charging
    if (bat_charge_halo) {
        if (charging) {
            // fast pulse 0.6Hz, opacity 0..LV_OPA_50
            float p = 0.5f + 0.5f * sinf(millis() / 280.0f);
            uint8_t opa = (uint8_t)(p * 110);
            lv_obj_set_style_arc_opa(bat_charge_halo, opa, LV_PART_INDICATOR);
            // bat dot also pulses brightness
            uint8_t dopa = (uint8_t)(160 + 80 * p);
            lv_obj_set_style_bg_opa(bat_dot, dopa, 0);
        } else {
            lv_obj_set_style_arc_opa(bat_charge_halo, LV_OPA_TRANSP, LV_PART_INDICATOR);
            lv_obj_set_style_bg_opa(bat_dot, LV_OPA_80, 0);
        }
    }
}


// =============================================================================
//  BUILD / REBUILD ALL CARDS
// =============================================================================
static void rebuild_all_cards() {
    if (!tileview) return;
    int prev = lv_tileview_get_tile_act(tileview)
        ? (int)((uintptr_t)lv_obj_get_user_data(lv_tileview_get_tile_act(tileview)))
        : 0;

    lv_obj_clean(tileview);
    for (int i = 0; i < CARD_COUNT; i++) tiles[i] = nullptr;

    // 7 tiles horizontally
    for (int i = 0; i < CARD_COUNT; i++) {
        tiles[i] = lv_tileview_add_tile(tileview, i, 0, LV_DIR_HOR);
        lv_obj_set_user_data(tiles[i], (void *)(intptr_t)i);
        lv_obj_set_scrollbar_mode(tiles[i], LV_SCROLLBAR_MODE_OFF);
    }
    build_card_clock     (tiles[0]);
    build_card_aura      (tiles[1]);
    build_card_viz       (tiles[2]);
    build_card_metronome (tiles[3]);
    build_card_themes    (tiles[4]);
    build_card_modes     (tiles[5]);
    build_card_settings  (tiles[6]);
    build_card_battery   (tiles[7]);

    apply_theme_to_screen();

    // restore tile position
    if (prev < 0) prev = 0;
    if (prev >= CARD_COUNT) prev = CARD_COUNT - 1;
    lv_obj_set_tile(tileview, tiles[prev], LV_ANIM_OFF);
}


// =============================================================================
//  ROOT SCREEN
// =============================================================================
static void build_root_ui() {
    scr_root = lv_scr_act();
    lv_obj_set_style_bg_color(scr_root, LC(T()->c_bg), 0);
    lv_obj_set_style_bg_opa  (scr_root, LV_OPA_COVER, 0);

    // optional wallpaper background -- REMOVED. Nocturne keeps the face pure.

    // tileview = the infinite horizontal carousel
    tileview = lv_tileview_create(scr_root);
    lv_obj_set_size(tileview, LCD_WIDTH, LCD_HEIGHT);
    lv_obj_align(tileview, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_scroll_dir(tileview, LV_DIR_HOR);
    lv_obj_set_scrollbar_mode(tileview, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_style_bg_color(tileview, LC(T()->c_bg), 0);
    lv_obj_set_style_bg_opa  (tileview, LV_OPA_COVER, 0);
    lv_obj_add_event_cb(tileview, tileview_changed_cb, LV_EVENT_VALUE_CHANGED, NULL);

    // status pill (top, on top of tileview but ignores layout)
    lv_obj_t *pill = lv_obj_create(scr_root);
    lv_obj_remove_style_all(pill);
    lv_obj_set_size(pill, 180, 28);
    lv_obj_align(pill, LV_ALIGN_TOP_MID, 0, 24);
    lv_obj_set_style_radius(pill, 14, 0);
    lv_obj_set_style_bg_opa(pill, LV_OPA_0, 0);
    lv_obj_clear_flag(pill, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(pill, LV_OBJ_FLAG_CLICKABLE);

    status_dot = lv_obj_create(pill);
    lv_obj_remove_style_all(status_dot);
    lv_obj_set_size(status_dot, 6, 6);
    lv_obj_set_style_radius(status_dot, 3, 0);
    lv_obj_set_style_bg_color(status_dot, LC(T()->c_accent), 0);
    lv_obj_set_style_bg_opa(status_dot, LV_OPA_80, 0);
    lv_obj_align(status_dot, LV_ALIGN_LEFT_MID, 0, 0);

    status_lbl = lv_label_create(pill);
    lv_obj_set_style_text_font(status_lbl, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(status_lbl, LC(T()->c_muted), 0);
    lv_obj_set_style_text_letter_space(status_lbl, 3, 0);
    lv_label_set_text(status_lbl, "NOCTURNE//OS");
    lv_obj_align(status_lbl, LV_ALIGN_LEFT_MID, 14, 0);

    // toast (centered bottom)
    toast_lbl = lv_label_create(scr_root);
    lv_obj_set_style_text_font(toast_lbl, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(toast_lbl, LC(T()->c_text), 0);
    lv_obj_set_style_text_letter_space(toast_lbl, 3, 0);
    lv_obj_set_style_bg_color(toast_lbl, LC(T()->c_card), 0);
    lv_obj_set_style_bg_opa(toast_lbl, LV_OPA_80, 0);
    lv_obj_set_style_pad_all(toast_lbl, 8, 0);
    lv_obj_set_style_radius(toast_lbl, 10, 0);
    lv_label_set_text(toast_lbl, "");
    lv_obj_align(toast_lbl, LV_ALIGN_BOTTOM_MID, 0, -20);
    lv_obj_add_flag(toast_lbl, LV_OBJ_FLAG_HIDDEN);

    // v2.1 -- bottom page-indicator dots (one per tile) ---------------------
    page_indicator = lv_obj_create(scr_root);
    lv_obj_remove_style_all(page_indicator);
    int dot_pitch = 14;
    lv_obj_set_size(page_indicator, dot_pitch * CARD_COUNT, 14);
    lv_obj_align(page_indicator, LV_ALIGN_BOTTOM_MID, 0, -24);
    lv_obj_set_style_bg_opa(page_indicator, LV_OPA_0, 0);
    lv_obj_clear_flag(page_indicator, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    for (int i = 0; i < CARD_COUNT; i++) {
        page_dots[i] = lv_obj_create(page_indicator);
        lv_obj_remove_style_all(page_dots[i]);
        lv_obj_set_size(page_dots[i], 6, 6);
        lv_obj_set_style_radius(page_dots[i], 3, 0);
        lv_obj_set_style_bg_color(page_dots[i], LC(T()->c_muted), 0);
        lv_obj_set_style_bg_opa(page_dots[i], LV_OPA_40, 0);
        lv_obj_set_pos(page_dots[i], i * dot_pitch + 4, 4);
    }

    // particles -- drift on top of everything (slow, theme tinted)
    particles_init(scr_root);

    rebuild_all_cards();
    page_indicator_update();
}


// =============================================================================
//  SETUP + LOOP
// =============================================================================
void setup() {
    launcher_app_boot();
    sd_bootstrap();
    state_restore();
    aura_learn_load();
    qmi_init();
    g_audio_on = audio_begin();   // v2.0 real audio
    wallpaper_try_load();

    nocturne_splash(2200);

    // status: red dot until NTP, green after
    bool ntp_ok = false;
    if (g_time_valid && (millis() / 1000) > 0) {
        // already have a valid clock from NVS -- skip if recent
    }
    // attempt NTP only if needed
    if (!g_time_valid) ntp_ok = wifi_sync_once();
    else if (g_last_ntp_ok == 0 || (uint32_t)time(NULL) - g_last_ntp_ok > 6UL*3600UL)
        ntp_ok = wifi_sync_once();

    aura_recompute();
    build_root_ui();
    apply_theme_to_screen();

    if (ntp_ok || g_time_valid) {
        lv_obj_set_style_bg_color(status_dot, lv_color_hex(0x00FF80), 0);
    } else {
        lv_obj_set_style_bg_color(status_dot, lv_color_hex(0xFF4040), 0);
    }

    g_last_touch_t = millis();
}

void loop() {
    uint32_t now = millis();

    lv_timer_handler();
    tick_toast();
    particles_tick();
    audio_tick();
    listening_state_tick();
    memory_transitions_tick();
    moonphase_fetch_daily();
    aura_history_log(false);
    journey_tick();
    concert_tick();
    aura_learn_tick();

    // v2.0 wrist double-tap -> cycle aura modes (subtle, intentional)
    if (gesture_check()) {
        g_aura_mode = (AuraMode)(((int)g_aura_mode + 1) % 6);
        aura_recompute();
        show_toast(AURA_NAMES[g_aura_mode], 1100);
        audio_play_tick(880);
        state_save();
    }

    // v2.0 concert mode: enter when charging detected, exit on user tap
    static bool prev_charge = false;
    bool charging = g_has_pmu ? g_pmu.isCharging() : false;
    if (charging && !prev_charge) concert_enter();
    prev_charge = charging;

    // BOOT-button safety exit (hold 1.5 s anywhere)
    static uint32_t btn_t = 0;
    if (digitalRead(BOOT_BTN_PIN) == LOW) {
        if (!btn_t) btn_t = now;
        else if (now - btn_t > 1500) { state_save(); return_to_launcher(); }
    } else btn_t = 0;

    // 1 Hz: clock + aura recompute
    static uint32_t g_last_tick = 0;
    static uint32_t g_last_save = 0;
    if (now - g_last_tick >= 1000) {
        g_last_tick = now;
        update_card_clock();
        update_card_aura();
        update_card_settings();
        // recompute aura every minute
        static uint8_t prev_hour = 99;
        time_t t; time(&t); struct tm tm; localtime_r(&t, &tm);
        if (tm.tm_hour != prev_hour) { prev_hour = tm.tm_hour; aura_recompute(); }
        if (g_time_valid && now - g_last_save >= 60000UL) {
            g_last_save = now;
            state_save();
        }
    }

    // ~30 Hz: visualizer + metronome + page-indicator breath
    static uint32_t g_last_anim = 0;
    if (now - g_last_anim >= 33) {
        g_last_anim = now;
        update_card_viz();
        update_card_metronome();
        page_indicator_update();
    }

    delay(5);
}

