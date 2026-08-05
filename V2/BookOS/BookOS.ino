// =============================================================================
//  BookOS.ino  --  MERGED v1.0 (base) + v2.0 (ported)
//  Target : Waveshare ESP32-S3 Touch AMOLED 2.06  (410 x 502, CO5300, FT3168)
//  Framework: Arduino-ESP32, LVGL 8.4, FreeRTOS
//  Compile : ESP32-S3 Dev Module / PSRAM=OPI / Flash=16MB / partition with OTA
//  Export  : Sketch -> Export Compiled Binary -> copy BookOS.ino.bin to
//            /WATCH/bin/bookos.bin on SD (LauncherOS-compatible).
// -----------------------------------------------------------------------------
//  v1.0 BASE preserved: shared boot block, splash overlay, library carousel,
//  reader (top/bottom bar, clock, battery), quick menu, focus mode, session
//  save/restore, file-based bookmarks, 5-theme engine, folder bootstrap, demo
//  book, BOOT long-press return, LauncherOS return.
//
//  v2.0 PORTED IN: lazy page streamer (FreeRTOS ring buffer + prefetch), NVS
//  bookmarks (dual-write with file), wc_core (WcTheme + wallpaper + battery
//  pill + settings sheet with cards + infinite carousel + global swipe
//  detector), wc_hw_boot() init body (wrapped by launcher_app_boot()),
//  count_pages_lazy(), wallpaper background on library.
//
//  Sections banner-tagged below so future edits stay traceable.
// =============================================================================

// ===== v1.0 BASE: SHARED BOOT BLOCK + INCLUDES (union with v2.0) =============
#include <Arduino.h>
#include <FS.h>
#include <SD_MMC.h>
#include <Wire.h>
#include <Preferences.h>
#include <time.h>
#include <sys/time.h>
#include <memory>
#include <limits.h>

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
#include <esp_heap_caps.h>
#include <esp_task_wdt.h>
#include <esp32-hal-cpu.h>
#include <Update.h>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>

// ---- Forward type declarations ---------------------------------------------
//  Arduino IDE hoists generated function prototypes to the very top of the
//  .ino, BEFORE any user-defined type. Any struct / typedef used in a return
//  or arg type of a helper below MUST be declared here so the hoisted
//  prototypes still compile.
struct Theme;
struct Book;
struct WcTheme;
struct WcCard;
struct WcCar;

// v2.0 wc_core callback typedefs (need to be visible to hoisted prototypes)
typedef void (*wc_toggle_cb_t)(bool, void*);
typedef void (*wc_slider_cb_t)(int, void*);
typedef void (*wc_dropdown_cb_t)(int, void*);
typedef void (*wc_action_cb_t)(void*);
typedef void (*wc_subview_build_t)(lv_obj_t*, void*);
typedef lv_obj_t* (*wc_card_factory_t)(lv_obj_t*, void*);

// ---- Pin map (v1.0 names, canonical) ---------------------------------------
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

// ---- WC_* aliases (v2.0 naming) so ported code compiles unchanged ----------
#define WC_LCD_WIDTH        LCD_WIDTH
#define WC_LCD_HEIGHT       LCD_HEIGHT
#define WC_LCD_SDIO0        LCD_SDIO0
#define WC_LCD_SDIO1        LCD_SDIO1
#define WC_LCD_SDIO2        LCD_SDIO2
#define WC_LCD_SDIO3        LCD_SDIO3
#define WC_LCD_SCLK         LCD_SCLK
#define WC_LCD_CS           LCD_CS
#define WC_LCD_RESET        LCD_RESET
#define WC_IIC_SDA          IIC_SDA
#define WC_IIC_SCL          IIC_SCL
#define WC_TP_INT           TP_INT
#define WC_TP_RESET         TP_RESET
#define WC_SDMMC_CLK        SDMMC_CLK
#define WC_SDMMC_CMD        SDMMC_CMD
#define WC_SDMMC_DATA       SDMMC_DATA
#define WC_BOOT_BTN         BOOT_BTN_PIN
#define WC_PWR_BTN_PIN      PWR_BTN_PIN

#define WC_SWIPE_THRESHOLD_PX      25
#define WC_SWIPE_MAX_DURATION_MS  800
#define WC_TAP_THRESHOLD_PX        20
#define WC_SCREEN_ANIM_MS         120
#define WC_CLICK_AFTER_SWIPE_MS   350
#define WC_BOOT_LONGPRESS_MS     1500
#define WC_DEFAULT_BRIGHTNESS     140

#define WC_PATH_BIN         "/WATCH/bin"
#define WC_PATH_PHOTOS      "/WATCH/PHOTOS"
#define WC_PATH_THEMES      "/WATCH/THEMES"
#define WC_PATH_LAUNCHER    "/WATCH/bin/launcheros.bin"
#define WC_PATH_BOOKOS      "/WATCH/BookOS"
#define WC_NVS_LAUNCHER     "dcw"
#define WC_NVS_THEME        "wctheme"
#define WC_NVS_BOOKOS       "bookos"

enum WcSwipe : int8_t {
    WC_SWIPE_NONE  =  0,
    WC_SWIPE_LEFT  = -1,
    WC_SWIPE_RIGHT =  1,
    WC_SWIPE_UP    =  2,
    WC_SWIPE_DOWN  =  3
};

// ---- v1.0 HW globals (canonical) -------------------------------------------
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

static lv_disp_draw_buf_t draw_buf;
static lv_color_t        *lvgl_buf = nullptr;
static lv_disp_drv_t      disp_drv;
static lv_indev_drv_t     indev_drv;

XPowersPMU g_pmu;
static bool g_has_pmu = false;

// ---- Global swipe detector state (v2.0 PORTED, wired into touch cb) --------
static int32_t  g_ts_x = 0, g_ts_y = 0;
static int32_t  g_tc_x = 0, g_tc_y = 0;
static uint32_t g_ts_ms = 0;
static volatile int8_t g_pending_swipe = WC_SWIPE_NONE;
static uint32_t g_last_swipe_ms = 0;

// ---- LVGL flush + touch (v1.0 base, extended with swipe detection) ---------
static void my_disp_flush(lv_disp_drv_t *d, const lv_area_t *a, lv_color_t *px) {
    uint32_t w = a->x2 - a->x1 + 1, h = a->y2 - a->y1 + 1;
    gfx->draw16bitRGBBitmap(a->x1, a->y1, (uint16_t *)px, w, h);
    lv_disp_flush_ready(d);
}

static void my_touchpad_read(lv_indev_drv_t *, lv_indev_data_t *data) {
    int32_t x = FT3168->IIC_Read_Device_Value(FT3168->Arduino_IIC_Touch::Value_Information::TOUCH_COORDINATE_X);
    int32_t y = FT3168->IIC_Read_Device_Value(FT3168->Arduino_IIC_Touch::Value_Information::TOUCH_COORDINATE_Y);
    int fingers = FT3168->IIC_Read_Device_Value(FT3168->Arduino_IIC_Touch::Value_Information::TOUCH_FINGER_NUMBER);

    static bool inDrag = false;
    if (fingers > 0) {
        data->state = LV_INDEV_STATE_PR;
        data->point.x = x; data->point.y = y;
        g_tc_x = x; g_tc_y = y;
        if (!inDrag) { inDrag = true; g_ts_x = x; g_ts_y = y; g_ts_ms = millis(); }
    } else {
        data->state = LV_INDEV_STATE_REL;
        if (inDrag) {
            inDrag = false;
            int32_t dx = g_tc_x - g_ts_x, dy = g_tc_y - g_ts_y;
            uint32_t dur = millis() - g_ts_ms;
            if (dur < WC_SWIPE_MAX_DURATION_MS) {
                if (abs(dx) > abs(dy) && abs(dx) > WC_SWIPE_THRESHOLD_PX)
                    g_pending_swipe = (dx < 0) ? WC_SWIPE_LEFT : WC_SWIPE_RIGHT;
                else if (abs(dy) > WC_SWIPE_THRESHOLD_PX)
                    g_pending_swipe = (dy < 0) ? WC_SWIPE_UP : WC_SWIPE_DOWN;
            }
        }
    }
}

// ---- PMU init (v1.0) -------------------------------------------------------
static void pmu_init() {
    if (g_pmu.begin(Wire, AXP2101_SLAVE_ADDRESS, IIC_SDA, IIC_SCL)) {
        g_has_pmu = true;
        g_pmu.disableTSPinMeasure();
        g_pmu.enableBattVoltageMeasure();
    }
}

// ---- LauncherOS return -----------------------------------------------------
//  Ported verbatim from NoteOS.ino — pure partition switch (no SD-side OTA
//  load, which was hanging on some setups). Boots the FACTORY partition if
//  present, else flips to the other OTA slot, then esp_restart(). This is
//  what your LauncherOS install actually uses.
static void return_to_launcher() {
    const esp_partition_t *factory = esp_partition_find_first(
        ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_FACTORY, NULL);
    if (factory) {
        esp_ota_set_boot_partition(factory);
    } else {
        const esp_partition_t *other = esp_ota_get_next_update_partition(NULL);
        if (other && other != esp_ota_get_running_partition())
            esp_ota_set_boot_partition(other);
    }
    delay(80);
    esp_restart();
}

// ---- Panel brightness (v1.0) -----------------------------------------------
static uint8_t g_brightness = 200;
static void co5300_set_brightness(uint8_t v) {
    uint8_t cmd = 0x51;
    bus->beginWrite(); bus->writeCommand(cmd); bus->write(v); bus->endWrite();
    g_brightness = v;
}
static void panel_on(uint8_t b) {
    uint8_t cmd = 0x29;
    bus->beginWrite(); bus->writeCommand(cmd); bus->endWrite();
    co5300_set_brightness(b);
}

// ===== v2.0 PORTED: wc_hw_boot() — canonical init body =======================
//  Replaces v1.0 launcher_app_boot() internals. launcher_app_boot() below is a
//  thin wrapper so LauncherOS boot flow still works.
static void wc_hw_boot() {
    Serial.begin(115200);
    pinMode(BOOT_BTN_PIN, INPUT_PULLUP);
    pinMode(PWR_BTN_PIN,  INPUT_PULLUP);
    btStop();

    if (!gfx->begin()) Serial.println("gfx->begin() failed");
    gfx->fillScreen(0x0000);

    // IIC bus recovery (v1.0)
    pinMode(IIC_SCL, OUTPUT);
    for (int i = 0; i < 9; i++) {
        digitalWrite(IIC_SCL, LOW);  delayMicroseconds(5);
        digitalWrite(IIC_SCL, HIGH); delayMicroseconds(5);
    }
    Wire.begin(IIC_SDA, IIC_SCL);
    Wire.setClock(400000);

    // Touch reset (v1.0)
    pinMode(TP_RESET, OUTPUT);
    digitalWrite(TP_RESET, LOW);  delay(50);
    digitalWrite(TP_RESET, HIGH); delay(200);
    FT3168->begin();

    // PMU (v1.0 XPowersPMU wrapper)
    pmu_init();

    // LVGL init with larger buffer (v2.0: 60 rows)
    lv_init();
    size_t bufPx = LCD_WIDTH * 60;
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
}

// Thin wrapper — keeps LauncherOS entry symbol intact
static void launcher_app_boot() { wc_hw_boot(); }
// ===== END SHARED BOOT BLOCK =================================================

// ===== v2.0 PORTED: wc_core — WcTheme palette (10 built-ins) =================
//  Parallel to v1.0 THEMES[]. Used by wc_core UI pieces (settings sheet,
//  wc_splash, battery pill, wallpaper accents). g_wc_theme_idx is separate
//  from v1.0 g_theme_idx to avoid clashing semantics.
struct WcTheme {
    const char *name;
    uint32_t primary, accent, bg, fg, warn, danger;
    const char *wallpaper;
};
static const WcTheme kBuiltinThemes[] = {
    { "MilSpec Green", 0x9BD16A, 0x5FA046, 0x0B140A, 0x2F3F26, 0xE5B14F, 0xE5484D, "milspec.bin"   },
    { "Amber CRT",     0xFFB300, 0xFF7A00, 0x140A00, 0x3F2600, 0xFFEA00, 0xFF3B30, "amber_crt.bin" },
    { "Arctic",        0xEAF6FF, 0x66C0FF, 0x081018, 0x2A3F5A, 0xFFD54F, 0xFF6B6B, "arctic.bin"    },
    { "Desert Tan",    0xE8C48B, 0xB07A3A, 0x191308, 0x4B3820, 0xFFC857, 0xE05B49, "desert.bin"    },
    { "Night-Vision",  0x9EFF6E, 0x39FF14, 0x040A04, 0x18331A, 0xE7FF3B, 0xFF3B30, "nightvis.bin"  },
    { "Blood-Red",     0xFF4A4A, 0xE81E1E, 0x140404, 0x3F1010, 0xFFB03A, 0xFF1E1E, "blood.bin"     },
    { "Cyber-Cyan",    0x50E3FF, 0x00C7C7, 0x030913, 0x113344, 0xFFEE58, 0xFF476F, "cybercyan.bin" },
    { "Synthwave",     0xFF6EC7, 0x9B5DE5, 0x0F0518, 0x3A1F55, 0xF15BB5, 0xF15BB5, "synthwave.bin" },
    { "Terminal Mono", 0xEDEDED, 0x8AE234, 0x000000, 0x2A2A2A, 0xE6DB74, 0xF92672, ""              },
    { "Nord",          0xE5E9F0, 0x88C0D0, 0x2E3440, 0x4C566A, 0xEBCB8B, 0xBF616A, "nord.bin"      },
};
static const int kBuiltinThemesN = sizeof(kBuiltinThemes) / sizeof(kBuiltinThemes[0]);
static int g_wc_theme_idx = 0;
static const WcTheme &wc_theme_active() { return kBuiltinThemes[g_wc_theme_idx]; }
static uint32_t wc_theme_primary() { return wc_theme_active().primary; }
static uint32_t wc_theme_accent()  { return wc_theme_active().accent;  }
static uint32_t wc_theme_bg()      { return wc_theme_active().bg;      }
static uint32_t wc_theme_fg()      { return wc_theme_active().fg;      }
static uint32_t wc_theme_warn()    { return wc_theme_active().warn;    }
static uint32_t wc_theme_danger()  { return wc_theme_active().danger;  }
static int  wc_theme_count() { return kBuiltinThemesN; }
static const char *wc_theme_name_at(int i) { return (i>=0 && i<kBuiltinThemesN) ? kBuiltinThemes[i].name : ""; }
static void wc_theme_load() {
    Preferences p; p.begin(WC_NVS_THEME, true);
    g_wc_theme_idx = p.getInt("idx", 0); p.end();
    if (g_wc_theme_idx < 0 || g_wc_theme_idx >= kBuiltinThemesN) g_wc_theme_idx = 0;
}
static void wc_theme_set(int i) {
    if (i < 0 || i >= kBuiltinThemesN) return;
    g_wc_theme_idx = i;
    Preferences p; p.begin(WC_NVS_THEME, false); p.putInt("idx", i); p.end();
}

// ===== v2.0 PORTED: wc_core — wallpaper (RGB565 .bin from /WATCH/PHOTOS) =====
#define WC_WP_MAX 32
static String       g_photos[WC_WP_MAX];
static int          g_photo_count = 0;
static int          g_photo_idx   = 0;
static lv_color_t  *g_wp_pixels   = nullptr;
static lv_img_dsc_t g_wp_dsc;

static bool wc_wallpaper_scan() {
    g_photo_count = 0;
    File d = SD_MMC.open(WC_PATH_PHOTOS);
    if (!d || !d.isDirectory()) return false;
    File f = d.openNextFile();
    while (f && g_photo_count < WC_WP_MAX) {
        String n = f.name(); int sl = n.lastIndexOf('/'); if (sl >= 0) n = n.substring(sl+1);
        String low = n; low.toLowerCase();
        if (!f.isDirectory() && low.endsWith(".bin")) g_photos[g_photo_count++] = n;
        f = d.openNextFile();
    }
    Preferences p; p.begin(WC_NVS_LAUNCHER, true); g_photo_idx = p.getInt("wp_idx", 0); p.end();
    if (g_photo_idx < 0 || g_photo_idx >= g_photo_count) g_photo_idx = 0;
    return g_photo_count > 0;
}
static bool wc_wallpaper_load(int i) {
    if (i < 0 || i >= g_photo_count) return false;
    const size_t bytes = (size_t)WC_LCD_WIDTH * WC_LCD_HEIGHT * 2;
    if (!g_wp_pixels) {
        g_wp_pixels = (lv_color_t*)heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM);
        if (!g_wp_pixels) g_wp_pixels = (lv_color_t*)heap_caps_malloc(bytes, MALLOC_CAP_8BIT);
        if (!g_wp_pixels) return false;
    }
    String path = String(WC_PATH_PHOTOS) + "/" + g_photos[i];
    File f = SD_MMC.open(path.c_str(), FILE_READ);
    if (!f) return false;
    size_t got = f.read((uint8_t*)g_wp_pixels, bytes);
    f.close();
    if (got != bytes) return false;
    g_wp_dsc.header.always_zero = 0;
    g_wp_dsc.header.w = WC_LCD_WIDTH;
    g_wp_dsc.header.h = WC_LCD_HEIGHT;
    g_wp_dsc.header.cf = LV_IMG_CF_TRUE_COLOR;
    g_wp_dsc.data_size = bytes;
    g_wp_dsc.data = (const uint8_t*)g_wp_pixels;
    return true;
}
static void wc_wallpaper_apply_to(lv_obj_t *img_widget) {
    if (!img_widget) return;
    if (!g_photo_count) { lv_obj_add_flag(img_widget, LV_OBJ_FLAG_HIDDEN); return; }
    if (!wc_wallpaper_load(g_photo_idx)) { lv_obj_add_flag(img_widget, LV_OBJ_FLAG_HIDDEN); return; }
    lv_obj_clear_flag(img_widget, LV_OBJ_FLAG_HIDDEN);
    lv_img_set_src(img_widget, &g_wp_dsc);
    lv_obj_invalidate(img_widget);
}
static void wc_wallpaper_set(int i) {
    if (!g_photo_count) return;
    while (i < 0)             i += g_photo_count;
    while (i >= g_photo_count) i -= g_photo_count;
    if (i == g_photo_idx) return;
    g_photo_idx = i;
    Preferences p; p.begin(WC_NVS_LAUNCHER, false); p.putInt("wp_idx", g_photo_idx); p.end();
}
static int  wc_wallpaper_count()        { return g_photo_count; }
static int  wc_wallpaper_active_index() { return g_photo_idx; }
static const char *wc_wallpaper_name_at(int i) { return (i>=0&&i<g_photo_count) ? g_photos[i].c_str() : ""; }

// ===== v2.0 PORTED: wc_core — BOOT-button poll ===============================
static inline bool wc_boot_pressed() { return digitalRead(WC_BOOT_BTN) == LOW; }
static void wc_boot_button_poll() {
    static uint32_t hold = 0;
    uint32_t now = millis();
    if (wc_boot_pressed()) {
        if (!hold) hold = now;
        else if (now - hold > WC_BOOT_LONGPRESS_MS) return_to_launcher();
    } else hold = 0;
}

// ===== v2.0 PORTED: wc_core — battery pill (LVGL widget + tick) ==============
static int  g_batt_pct = -1;
static bool g_batt_ch  = false;
static int  g_batt_mv  = 0;
static void wc_battery_set_sample(int pct, bool ch, int mv) { g_batt_pct = pct; g_batt_ch = ch; g_batt_mv = mv; }
static int  wc_battery_pct() { return g_batt_pct; }
static void wc_battery_pill_tick(lv_timer_t *t) {
    lv_obj_t *lbl = (lv_obj_t*)t->user_data; if (!lbl) return;
    char b[16];
    if (g_batt_pct < 0) snprintf(b, sizeof(b), "-- %%");
    else                snprintf(b, sizeof(b), "%d%%%s", g_batt_pct, g_batt_ch ? "+" : "");
    lv_label_set_text(lbl, b);
    lv_color_t col = lv_color_hex(g_batt_pct < 15 ? wc_theme_danger()
                                 : g_batt_pct < 30 ? wc_theme_warn()
                                                    : wc_theme_primary());
    lv_obj_set_style_text_color(lbl, col, 0);
}
static lv_obj_t *wc_battery_pill(lv_obj_t *parent) {
    lv_obj_t *pill = lv_obj_create(parent);
    lv_obj_set_size(pill, 74, 28);
    lv_obj_set_style_bg_color(pill, lv_color_hex(wc_theme_fg()), 0);
    lv_obj_set_style_bg_opa(pill, LV_OPA_50, 0);
    lv_obj_set_style_border_color(pill, lv_color_hex(wc_theme_accent()), 0);
    lv_obj_set_style_border_width(pill, 1, 0);
    lv_obj_set_style_radius(pill, 14, 0);
    lv_obj_set_style_pad_all(pill, 2, 0);
    lv_obj_clear_flag(pill, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *lbl = lv_label_create(pill);
    lv_label_set_text(lbl, "-- %");
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_14, 0);
    lv_obj_center(lbl);
    lv_timer_create(wc_battery_pill_tick, 1000, lbl);
    return pill;
}
static void wc_pmu_refresh() {
    if (!g_has_pmu) return;
    int pct = g_pmu.getBatteryPercent();
    bool ch = g_pmu.isCharging();
    int mv  = g_pmu.getBattVoltage();
    wc_battery_set_sample(pct, ch, mv);
}

// ===== v2.0 PORTED: wc_core — splash screen (separate LV screen) =============
//  Not used as primary boot splash (v1.0's build_splash_ui is primary), but
//  kept available for future use / theming preview.
static lv_obj_t *g_wc_splash_scr = nullptr;
static lv_obj_t *g_wc_splash_bar = nullptr;
static lv_obj_t *g_wc_splash_status = nullptr;
static void wc_splash_show(const char *os_name, const char *subtitle) {
    g_wc_splash_scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(g_wc_splash_scr, lv_color_hex(wc_theme_bg()), 0);
    lv_obj_set_style_bg_opa(g_wc_splash_scr, LV_OPA_COVER, 0);
    lv_obj_clear_flag(g_wc_splash_scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *t = lv_label_create(g_wc_splash_scr);
    lv_label_set_text(t, os_name ? os_name : "");
    lv_obj_set_style_text_color(t, lv_color_hex(wc_theme_primary()), 0);
    lv_obj_set_style_text_font(t, &lv_font_montserrat_48, 0);
    lv_obj_align(t, LV_ALIGN_CENTER, 0, -60);
    lv_obj_t *sub = lv_label_create(g_wc_splash_scr);
    lv_label_set_text(sub, subtitle ? subtitle : "");
    lv_obj_set_style_text_color(sub, lv_color_hex(wc_theme_accent()), 0);
    lv_obj_set_style_text_font(sub, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_letter_space(sub, 3, 0);
    lv_obj_align(sub, LV_ALIGN_CENTER, 0, -10);
    g_wc_splash_bar = lv_bar_create(g_wc_splash_scr);
    lv_obj_set_size(g_wc_splash_bar, 260, 4);
    lv_obj_align(g_wc_splash_bar, LV_ALIGN_CENTER, 0, 40);
    lv_obj_set_style_bg_color(g_wc_splash_bar, lv_color_hex(wc_theme_fg()), LV_PART_MAIN);
    lv_obj_set_style_bg_color(g_wc_splash_bar, lv_color_hex(wc_theme_accent()), LV_PART_INDICATOR);
    lv_bar_set_range(g_wc_splash_bar, 0, 100);
    lv_bar_set_value(g_wc_splash_bar, 0, LV_ANIM_OFF);
    g_wc_splash_status = lv_label_create(g_wc_splash_scr);
    lv_label_set_text(g_wc_splash_status, "boot");
    lv_obj_set_style_text_color(g_wc_splash_status, lv_color_hex(wc_theme_fg()), 0);
    lv_obj_set_style_text_font(g_wc_splash_status, &lv_font_montserrat_12, 0);
    lv_obj_align(g_wc_splash_status, LV_ALIGN_CENTER, 0, 68);
    lv_scr_load(g_wc_splash_scr);
}
static void wc_splash_step(int percent, const char *status) {
    if (percent < 0) percent = 0; if (percent > 100) percent = 100;
    if (g_wc_splash_bar) lv_bar_set_value(g_wc_splash_bar, percent, LV_ANIM_OFF);
    if (g_wc_splash_status && status) lv_label_set_text(g_wc_splash_status, status);
    lv_timer_handler();
}
static void wc_splash_dismiss() {
    if (g_wc_splash_scr) {
        lv_obj_del_async(g_wc_splash_scr);
        g_wc_splash_scr = nullptr;
        g_wc_splash_bar = nullptr;
        g_wc_splash_status = nullptr;
    }
}

// ===== v2.0 PORTED: wc_core — settings sheet (WcCard: toggle/slider/etc.) ====
enum WcCardKind { WC_CARD_TOGGLE, WC_CARD_SLIDER, WC_CARD_DROPDOWN, WC_CARD_ACTION, WC_CARD_SUBVIEW };
// callback typedefs are declared at top of file (Arduino prototype hoisting)

struct WcCard {
    WcCardKind kind;
    const char *title;
    const char *subtitle;
    bool toggle_on;
    int  slider_min, slider_max, slider_val;
    const char *dropdown_items;
    int dropdown_idx;
    wc_toggle_cb_t   on_toggle;
    wc_slider_cb_t   on_slider;
    wc_dropdown_cb_t on_dropdown;
    wc_action_cb_t   on_action;
    wc_subview_build_t on_subview;
    void *user;
};
#define WC_MAX_CARDS 24
static WcCard g_cards[WC_MAX_CARDS];
static int    g_card_count = 0;
static bool   g_sheet_open = false;
static lv_obj_t *g_sheet_scr = nullptr, *g_sheet_list = nullptr, *g_sheet_subview = nullptr, *g_prev_scr = nullptr;

static void wc_settings_clear() { g_card_count = 0; }
static void wc_settings_add(const WcCard &c) { if (g_card_count < WC_MAX_CARDS) g_cards[g_card_count++] = c; }
static void wc_settings_pop_subview();
static void wc_settings_push_subview(const char *title, wc_subview_build_t build, void *user);

static void wc_settings_toggle_cb(lv_event_t *e) {
    lv_obj_t *sw = lv_event_get_target(e);
    WcCard *c = (WcCard*)lv_event_get_user_data(e);
    bool on = lv_obj_has_state(sw, LV_STATE_CHECKED);
    c->toggle_on = on;
    if (c->on_toggle) c->on_toggle(on, c->user);
}
static void wc_settings_slider_cb(lv_event_t *e) {
    lv_obj_t *sl = lv_event_get_target(e);
    WcCard *c = (WcCard*)lv_event_get_user_data(e);
    int v = lv_slider_get_value(sl);
    c->slider_val = v;
    if (c->on_slider) c->on_slider(v, c->user);
}
static void wc_settings_dd_cb(lv_event_t *e) {
    lv_obj_t *dd = lv_event_get_target(e);
    WcCard *c = (WcCard*)lv_event_get_user_data(e);
    int idx = lv_dropdown_get_selected(dd);
    c->dropdown_idx = idx;
    if (c->on_dropdown) c->on_dropdown(idx, c->user);
}
static void wc_settings_action_cb(lv_event_t *e) {
    WcCard *c = (WcCard*)lv_event_get_user_data(e);
    if (c->on_action) c->on_action(c->user);
}
static void wc_settings_subview_cb(lv_event_t *e) {
    WcCard *c = (WcCard*)lv_event_get_user_data(e);
    wc_settings_push_subview(c->title, c->on_subview, c->user);
}
static void wc_settings_rebuild() {
    if (!g_sheet_list) return;
    lv_obj_clean(g_sheet_list);
    for (int i = 0; i < g_card_count; i++) {
        WcCard *c = &g_cards[i];
        lv_obj_t *row = lv_obj_create(g_sheet_list);
        lv_obj_set_size(row, LV_PCT(100), 76);
        lv_obj_set_style_bg_color(row, lv_color_hex(wc_theme_fg()), 0);
        lv_obj_set_style_bg_opa(row, LV_OPA_30, 0);
        lv_obj_set_style_border_color(row, lv_color_hex(wc_theme_accent()), 0);
        lv_obj_set_style_border_width(row, 1, 0);
        lv_obj_set_style_radius(row, 10, 0);
        lv_obj_set_style_pad_all(row, 10, 0);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_t *t = lv_label_create(row);
        lv_label_set_text(t, c->title);
        lv_obj_set_style_text_color(t, lv_color_hex(wc_theme_primary()), 0);
        lv_obj_set_style_text_font(t, &lv_font_montserrat_18, 0);
        lv_obj_align(t, LV_ALIGN_TOP_LEFT, 2, 2);
        if (c->subtitle && c->subtitle[0]) {
            lv_obj_t *s = lv_label_create(row);
            lv_label_set_text(s, c->subtitle);
            lv_obj_set_style_text_color(s, lv_color_hex(wc_theme_fg()), 0);
            lv_obj_set_style_text_font(s, &lv_font_montserrat_12, 0);
            lv_obj_align(s, LV_ALIGN_BOTTOM_LEFT, 2, -2);
        }
        switch (c->kind) {
        case WC_CARD_TOGGLE: {
            lv_obj_t *sw = lv_switch_create(row);
            lv_obj_align(sw, LV_ALIGN_RIGHT_MID, -4, 0);
            if (c->toggle_on) lv_obj_add_state(sw, LV_STATE_CHECKED);
            lv_obj_add_event_cb(sw, wc_settings_toggle_cb, LV_EVENT_VALUE_CHANGED, c);
        } break;
        case WC_CARD_SLIDER: {
            lv_obj_t *sl = lv_slider_create(row);
            lv_obj_set_size(sl, 120, 8);
            lv_obj_align(sl, LV_ALIGN_RIGHT_MID, -8, 0);
            lv_slider_set_range(sl, c->slider_min, c->slider_max);
            lv_slider_set_value(sl, c->slider_val, LV_ANIM_OFF);
            lv_obj_add_event_cb(sl, wc_settings_slider_cb, LV_EVENT_VALUE_CHANGED, c);
        } break;
        case WC_CARD_DROPDOWN: {
            lv_obj_t *dd = lv_dropdown_create(row);
            lv_dropdown_set_options(dd, c->dropdown_items ? c->dropdown_items : "");
            lv_dropdown_set_selected(dd, c->dropdown_idx);
            lv_obj_set_width(dd, 140);
            lv_obj_align(dd, LV_ALIGN_RIGHT_MID, -8, 0);
            lv_obj_add_event_cb(dd, wc_settings_dd_cb, LV_EVENT_VALUE_CHANGED, c);
        } break;
        case WC_CARD_ACTION: {
            lv_obj_t *btn = lv_btn_create(row);
            lv_obj_set_size(btn, 90, 40);
            lv_obj_align(btn, LV_ALIGN_RIGHT_MID, -8, 0);
            lv_obj_t *bl = lv_label_create(btn); lv_label_set_text(bl, "Run"); lv_obj_center(bl);
            lv_obj_add_event_cb(btn, wc_settings_action_cb, LV_EVENT_CLICKED, c);
        } break;
        case WC_CARD_SUBVIEW: {
            lv_obj_t *arrow = lv_label_create(row);
            lv_label_set_text(arrow, LV_SYMBOL_RIGHT);
            lv_obj_align(arrow, LV_ALIGN_RIGHT_MID, -8, 0);
            lv_obj_set_style_text_color(arrow, lv_color_hex(wc_theme_accent()), 0);
            lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_add_event_cb(row, wc_settings_subview_cb, LV_EVENT_CLICKED, c);
        } break;
        }
    }
}
static void wc_settings_begin() {
    g_sheet_scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(g_sheet_scr, lv_color_hex(wc_theme_bg()), 0);
    lv_obj_set_style_bg_opa(g_sheet_scr, LV_OPA_COVER, 0);
    lv_obj_clear_flag(g_sheet_scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *hdr = lv_label_create(g_sheet_scr);
    lv_label_set_text(hdr, "Settings");
    lv_obj_set_style_text_color(hdr, lv_color_hex(wc_theme_primary()), 0);
    lv_obj_set_style_text_font(hdr, &lv_font_montserrat_28, 0);
    lv_obj_align(hdr, LV_ALIGN_TOP_LEFT, 24, 20);
    lv_obj_t *hint = lv_label_create(g_sheet_scr);
    lv_label_set_text(hint, "swipe down to dismiss");
    lv_obj_set_style_text_color(hint, lv_color_hex(wc_theme_fg()), 0);
    lv_obj_set_style_text_font(hint, &lv_font_montserrat_12, 0);
    lv_obj_align(hint, LV_ALIGN_TOP_LEFT, 24, 58);
    g_sheet_list = lv_obj_create(g_sheet_scr);
    lv_obj_set_size(g_sheet_list, WC_LCD_WIDTH - 20, WC_LCD_HEIGHT - 100);
    lv_obj_align(g_sheet_list, LV_ALIGN_TOP_MID, 0, 90);
    lv_obj_set_flex_flow(g_sheet_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_bg_opa(g_sheet_list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(g_sheet_list, 0, 0);
    lv_obj_set_style_pad_all(g_sheet_list, 8, 0);
    lv_obj_set_style_pad_row(g_sheet_list, 10, 0);
    lv_obj_set_scroll_dir(g_sheet_list, LV_DIR_VER);
}
static void wc_settings_open() {
    if (g_sheet_open) return;
    if (!g_sheet_scr) wc_settings_begin();
    g_prev_scr = lv_scr_act();
    wc_settings_rebuild();
    g_sheet_open = true;
    lv_scr_load_anim(g_sheet_scr, LV_SCR_LOAD_ANIM_MOVE_TOP, 250, 0, false);
}
static void wc_settings_close() {
    if (!g_sheet_open) return;
    g_sheet_open = false;
    if (g_sheet_subview) { lv_obj_del(g_sheet_subview); g_sheet_subview = nullptr; }
    if (g_prev_scr) lv_scr_load_anim(g_prev_scr, LV_SCR_LOAD_ANIM_MOVE_BOTTOM, 250, 0, false);
}
static bool wc_settings_is_open() { return g_sheet_open; }
static void wc_settings_pop_subview() {
    if (!g_sheet_subview) return;
    lv_obj_t *sv = g_sheet_subview; g_sheet_subview = nullptr;
    lv_anim_t a; lv_anim_init(&a);
    lv_anim_set_var(&a, sv);
    lv_anim_set_exec_cb(&a, (lv_anim_exec_xcb_t)lv_obj_set_x);
    lv_anim_set_values(&a, 0, WC_LCD_WIDTH);
    lv_anim_set_time(&a, 200);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_set_ready_cb(&a, [](lv_anim_t *aa){ lv_obj_del((lv_obj_t*)aa->var); });
    lv_anim_start(&a);
}
static void wc_settings_push_subview(const char *title, wc_subview_build_t build, void *user) {
    if (!g_sheet_scr || !build) return;
    if (g_sheet_subview) { lv_obj_del(g_sheet_subview); g_sheet_subview = nullptr; }
    g_sheet_subview = lv_obj_create(g_sheet_scr);
    lv_obj_set_size(g_sheet_subview, WC_LCD_WIDTH, WC_LCD_HEIGHT);
    lv_obj_set_style_bg_color(g_sheet_subview, lv_color_hex(wc_theme_bg()), 0);
    lv_obj_set_style_bg_opa(g_sheet_subview, LV_OPA_COVER, 0);
    lv_obj_clear_flag(g_sheet_subview, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *back = lv_label_create(g_sheet_subview);
    lv_label_set_text_fmt(back, "%s  %s", LV_SYMBOL_LEFT, title ? title : "");
    lv_obj_set_style_text_color(back, lv_color_hex(wc_theme_primary()), 0);
    lv_obj_set_style_text_font(back, &lv_font_montserrat_18, 0);
    lv_obj_align(back, LV_ALIGN_TOP_LEFT, 20, 20);
    lv_obj_add_flag(back, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(back, [](lv_event_t*){ wc_settings_pop_subview(); }, LV_EVENT_CLICKED, nullptr);
    lv_obj_t *area = lv_obj_create(g_sheet_subview);
    lv_obj_set_size(area, WC_LCD_WIDTH - 20, WC_LCD_HEIGHT - 80);
    lv_obj_align(area, LV_ALIGN_TOP_MID, 0, 60);
    lv_obj_set_style_bg_opa(area, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(area, 0, 0);
    build(area, user);
    lv_obj_set_x(g_sheet_subview, WC_LCD_WIDTH);
    lv_anim_t a; lv_anim_init(&a);
    lv_anim_set_var(&a, g_sheet_subview);
    lv_anim_set_exec_cb(&a, (lv_anim_exec_xcb_t)lv_obj_set_x);
    lv_anim_set_values(&a, WC_LCD_WIDTH, 0);
    lv_anim_set_time(&a, 220);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_start(&a);
}
static bool wc_settings_handle_swipe(int8_t dir) {
    if (!g_sheet_open) return false;
    if (g_sheet_subview) { if (dir == WC_SWIPE_RIGHT) { wc_settings_pop_subview(); return true; } return false; }
    if (dir == WC_SWIPE_DOWN) { wc_settings_close(); return true; }
    return false;
}

// ===== v2.0 PORTED: wc_core — infinite carousel (WcCar) ======================
#define WC_CAR_MAX 12
// wc_card_factory_t typedef declared at top of file
struct WcCarSpec { wc_card_factory_t factory; void *user; };
struct WcCar {
    lv_obj_t *root = nullptr;
    WcCarSpec specs[WC_CAR_MAX];
    lv_obj_t *cards[WC_CAR_MAX] = {};
    int card_count = 0;
    int index = 0;
    void (*on_change)(int) = nullptr;
    static const int kActiveW = 320, kActiveH = 380, kSideOffX = 300, kSideScale = 218, kSideOpa = 140, kAnimMs = 220;
};
static void wc_car_set_x(void *o, int32_t v)   { lv_obj_set_x((lv_obj_t*)o, v); }
static void wc_car_set_w(void *o, int32_t v)   { lv_obj_set_width((lv_obj_t*)o, v); }
static void wc_car_set_h(void *o, int32_t v)   { lv_obj_set_height((lv_obj_t*)o, v); }
static void wc_car_set_opa(void *o, int32_t v) { lv_obj_set_style_opa((lv_obj_t*)o, v, 0); }
static void wc_car_anim(lv_obj_t *obj, lv_anim_exec_xcb_t exec, int32_t from, int32_t to, uint32_t dur) {
    lv_anim_t a; lv_anim_init(&a);
    lv_anim_set_var(&a, obj); lv_anim_set_exec_cb(&a, exec);
    lv_anim_set_values(&a, from, to); lv_anim_set_time(&a, dur);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_start(&a);
}
static void wc_car_begin(WcCar &c, lv_obj_t *parent) {
    c.root = lv_obj_create(parent);
    lv_obj_set_size(c.root, LV_PCT(100), LV_PCT(100));
    lv_obj_clear_flag(c.root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_opa(c.root, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(c.root, 0, 0);
    lv_obj_set_style_pad_all(c.root, 0, 0);
}
static void wc_car_add(WcCar &c, wc_card_factory_t f, void *u) { if (c.card_count < WC_CAR_MAX) c.specs[c.card_count++] = {f, u}; }
static void wc_car_layout(WcCar &c, bool animate) {
    if (!c.card_count) return;
    uint32_t dur = animate ? WcCar::kAnimMs : 0;
    for (int i = 0; i < c.card_count; i++) {
        lv_obj_t *card = c.cards[i]; if (!card) continue;
        int d = i - c.index;
        while (d >  c.card_count/2) d -= c.card_count;
        while (d < -c.card_count/2) d += c.card_count;
        if (d == 0) {
            wc_car_anim(card, wc_car_set_x, lv_obj_get_x(card), (WC_LCD_WIDTH - WcCar::kActiveW)/2, dur);
            wc_car_anim(card, wc_car_set_w, lv_obj_get_width(card), WcCar::kActiveW, dur);
            wc_car_anim(card, wc_car_set_h, lv_obj_get_height(card), WcCar::kActiveH, dur);
            wc_car_anim(card, wc_car_set_opa, lv_obj_get_style_opa(card, 0), LV_OPA_COVER, dur);
            lv_obj_move_foreground(card);
        } else if (d == 1 || d == -1) {
            int x = (WC_LCD_WIDTH - (WcCar::kActiveW*WcCar::kSideScale/256))/2 + (d>0 ? WcCar::kSideOffX : -WcCar::kSideOffX);
            wc_car_anim(card, wc_car_set_x, lv_obj_get_x(card), x, dur);
            wc_car_anim(card, wc_car_set_w, lv_obj_get_width(card), WcCar::kActiveW*WcCar::kSideScale/256, dur);
            wc_car_anim(card, wc_car_set_h, lv_obj_get_height(card), WcCar::kActiveH*WcCar::kSideScale/256, dur);
            wc_car_anim(card, wc_car_set_opa, lv_obj_get_style_opa(card, 0), WcCar::kSideOpa, dur);
        } else {
            wc_car_anim(card, wc_car_set_x, lv_obj_get_x(card), d>0 ? WC_LCD_WIDTH+40 : -WC_LCD_WIDTH-40, dur);
            wc_car_anim(card, wc_car_set_opa, lv_obj_get_style_opa(card, 0), LV_OPA_TRANSP, dur);
        }
    }
}
static void wc_car_rebuild(WcCar &c) {
    for (int i = 0; i < c.card_count; i++) {
        if (c.cards[i]) { lv_obj_del(c.cards[i]); c.cards[i] = nullptr; }
        c.cards[i] = c.specs[i].factory(c.root, c.specs[i].user);
        lv_obj_set_size(c.cards[i], WcCar::kActiveW, WcCar::kActiveH);
        lv_obj_align(c.cards[i], LV_ALIGN_CENTER, 0, 0);
    }
    wc_car_layout(c, false);
    if (c.on_change && c.card_count) c.on_change(c.index);
}
static void wc_car_goto(WcCar &c, int i, bool animate) {
    if (!c.card_count) return;
    while (i < 0)             i += c.card_count;
    while (i >= c.card_count) i -= c.card_count;
    if (i == c.index) return;
    c.index = i;
    wc_car_layout(c, animate);
    if (c.on_change) c.on_change(c.index);
}
static void wc_car_next(WcCar &c) { wc_car_goto(c, c.index+1, true); }
static void wc_car_prev(WcCar &c) { wc_car_goto(c, c.index-1, true); }
// ===== END wc_core ===========================================================

// ===== v2.0 PORTED: bookstream — FreeRTOS ring buffer page streamer ==========
#define WC_PAGE_RING       15
#define WC_PAGE_PREFETCH    5
#define WC_PAGE_LOOK_AHEAD  3
#define WC_PAGE_BODY_MAX 1600

struct WcRingSlot { int page; char text[WC_PAGE_BODY_MAX]; };
static WcRingSlot        g_bs_ring[WC_PAGE_RING];
static SemaphoreHandle_t g_bs_lock = nullptr;
static TaskHandle_t      g_bs_task = nullptr;
static volatile bool     g_bs_run  = false;
static char              g_bs_folder[64] = {0};
static int               g_bs_total = 0;
static volatile int      g_bs_cur   = 1;

static uint32_t wc_hash32(const char *s) {
    uint32_t h = 2166136261u;
    while (*s) { h ^= (uint8_t)*s++; h *= 16777619u; }
    return h;
}
static void wc_nvs_key_for(const char *folder, char out[24]) {
    uint32_t h = wc_hash32(folder);
    snprintf(out, 24, "%08x", (unsigned)h);
}
static bool wc_bs_load_file(int page, char *out, size_t sz) {
    char path[160];
    snprintf(path, sizeof(path), "%s/Books/%s/%04d.txt", WC_PATH_BOOKOS, g_bs_folder, page);
    File f = SD_MMC.open(path, FILE_READ);
    if (!f) return false;
    size_t n = f.read((uint8_t*)out, sz - 1);
    out[n] = 0; f.close(); return true;
}
static int wc_bs_slot_for(int page) {
    for (int i = 0; i < WC_PAGE_RING; i++) if (g_bs_ring[i].page == page) return i;
    return -1;
}
static int wc_bs_lru() {
    int worst = 0, worst_d = -1;
    for (int i = 0; i < WC_PAGE_RING; i++) {
        int d = g_bs_ring[i].page < 0 ? INT_MAX : abs(g_bs_ring[i].page - g_bs_cur);
        if (d > worst_d) { worst_d = d; worst = i; }
    }
    return worst;
}
static void wc_bs_task_fn(void *) {
    while (g_bs_run) {
        int cur = g_bs_cur;
        int hi = cur - 1;
        for (int i = 0; i < WC_PAGE_RING; i++)
            if (g_bs_ring[i].page >= cur && g_bs_ring[i].page > hi) hi = g_bs_ring[i].page;
        if (hi < g_bs_total && (hi - cur) < WC_PAGE_LOOK_AHEAD + WC_PAGE_PREFETCH) {
            for (int i = 1; i <= WC_PAGE_PREFETCH; i++) {
                int p = hi + i; if (p > g_bs_total) break;
                if (wc_bs_slot_for(p) >= 0) continue;
                xSemaphoreTake(g_bs_lock, portMAX_DELAY);
                int s = wc_bs_lru(); g_bs_ring[s].page = -1;
                xSemaphoreGive(g_bs_lock);
                char buf[WC_PAGE_BODY_MAX];
                if (wc_bs_load_file(p, buf, sizeof(buf))) {
                    xSemaphoreTake(g_bs_lock, portMAX_DELAY);
                    memcpy(g_bs_ring[s].text, buf, sizeof(buf));
                    g_bs_ring[s].page = p;
                    xSemaphoreGive(g_bs_lock);
                }
            }
        }
        vTaskDelay(pdMS_TO_TICKS(80));
    }
    g_bs_task = nullptr;
    vTaskDelete(NULL);
}
static bool wc_bookstream_open(const char *folder, int total_pages, int start_page) {
    if (g_bs_task) {
        g_bs_run = false;
        for (int i = 0; i < 40 && g_bs_task; i++) delay(5);
        if (g_bs_task) { vTaskDelete(g_bs_task); g_bs_task = nullptr; }
        if (g_bs_lock) { vSemaphoreDelete(g_bs_lock); g_bs_lock = nullptr; }
    }
    strncpy(g_bs_folder, folder, sizeof(g_bs_folder) - 1);
    g_bs_folder[sizeof(g_bs_folder) - 1] = 0;
    g_bs_total = total_pages;
    g_bs_cur = start_page < 1 ? 1 : start_page;
    for (int i = 0; i < WC_PAGE_RING; i++) g_bs_ring[i].page = -1;
    g_bs_lock = xSemaphoreCreateMutex();
    if (!g_bs_lock) return false;
    int prime = 10 < total_pages ? 10 : total_pages;
    for (int i = 0; i < prime; i++) {
        int p = g_bs_cur + i; if (p > total_pages) break;
        if (wc_bs_slot_for(p) >= 0) continue;
        int s = wc_bs_lru();
        if (wc_bs_load_file(p, g_bs_ring[s].text, sizeof(g_bs_ring[s].text)))
            g_bs_ring[s].page = p;
    }
    g_bs_run = true;
    xTaskCreatePinnedToCore(wc_bs_task_fn, "wc_pagestream", 4096, nullptr, 1, &g_bs_task, 0);
    return true;
}
static const char *wc_bookstream_get(int page) {
    if (!g_bs_lock) return nullptr;
    xSemaphoreTake(g_bs_lock, portMAX_DELAY);
    int s = wc_bs_slot_for(page);
    const char *res = (s >= 0) ? g_bs_ring[s].text : nullptr;
    xSemaphoreGive(g_bs_lock);
    return res;
}
// Safe copy variant — used by reader to avoid stale pointer if streamer
// replaces the slot between get() and label render.
static bool wc_bookstream_get_copy(int page, char *out, size_t sz) {
    if (!g_bs_lock) return false;
    xSemaphoreTake(g_bs_lock, portMAX_DELAY);
    int s = wc_bs_slot_for(page);
    bool ok = false;
    if (s >= 0) {
        size_t n = strnlen(g_bs_ring[s].text, sizeof(g_bs_ring[s].text));
        if (n >= sz) n = sz - 1;
        memcpy(out, g_bs_ring[s].text, n);
        out[n] = 0;
        ok = true;
    }
    xSemaphoreGive(g_bs_lock);
    return ok;
}
static void wc_bookstream_seek(int page) {
    if (page < 1) page = 1;
    if (page > g_bs_total) page = g_bs_total;
    g_bs_cur = page;
}
static void wc_bookstream_bookmark(int page) {
    if (!g_bs_folder[0]) return;
    char key[24]; wc_nvs_key_for(g_bs_folder, key);
    Preferences p; p.begin(WC_NVS_BOOKOS, false); p.putInt(key, page); p.end();
}
static int wc_bookstream_bookmark_get(const char *folder) {
    if (!folder || !folder[0]) return 0;
    char key[24]; wc_nvs_key_for(folder, key);
    Preferences p; p.begin(WC_NVS_BOOKOS, true);
    int page = p.getInt(key, 0);
    p.end();
    return page;
}
static void wc_bookstream_close() {
    g_bs_run = false;
    for (int i = 0; i < 40 && g_bs_task; i++) delay(5);
    if (g_bs_task) { vTaskDelete(g_bs_task); g_bs_task = nullptr; }
    if (g_bs_lock) { vSemaphoreDelete(g_bs_lock); g_bs_lock = nullptr; }
    for (int i = 0; i < WC_PAGE_RING; i++) g_bs_ring[i].page = -1;
    g_bs_folder[0] = 0; g_bs_total = 0;
}
// ===== END bookstream ========================================================

// ===== v1.0 BASE: BookOS palette / themes ====================================
struct Theme {
    const char *name;
    uint32_t bg;
    uint32_t surface;
    uint32_t border;
    uint32_t txt;
    uint32_t mute;
    uint32_t accent;
};
static const Theme THEMES[] = {
    { "WARM PAPER",      0x1C1812, 0x2A241C, 0x3A3328, 0xF0E6D2, 0x9E927C, 0xD9B26B },
    { "OLED BLACK",      0x000000, 0x0A0A0A, 0x1C1C1C, 0xEEEEEE, 0x707070, 0xFFFFFF },
    { "FOREST TERMINAL", 0x05140A, 0x0B2014, 0x163020, 0xB6FFC8, 0x4F8A66, 0x4FFFA0 },
    { "DEEP NAVY",       0x06101F, 0x0C1A30, 0x182944, 0xE0E8F4, 0x6E7E9A, 0x4FB3FF },
    { "CRIMSON ARCHIVE", 0x140606, 0x200A0A, 0x301414, 0xF4E0E0, 0xA07070, 0xFF4F6E },
};
#define NUM_THEMES 5
static int g_theme_idx = 1;   // default OLED BLACK

#define CYAN   0x00E5FF

static inline lv_color_t TC(uint32_t hex) { return lv_color_hex(hex); }
static inline const Theme &TH() { return THEMES[g_theme_idx]; }

// ===== v1.0 BASE: Book model (merged — total_pages semantics unified) ========
#define MAX_BOOKS      32
#define MAX_NAME_LEN   48
#define MAX_TITLE_LEN  64
#define MAX_AUTHOR_LEN 48
#define MAX_PAGE_BYTES 2200

struct Book {
    char folder[MAX_NAME_LEN];
    char title[MAX_TITLE_LEN];
    char author[MAX_AUTHOR_LEN];
    int  total_pages;
    int  current_page;
    uint32_t last_opened;
};

static Book g_books[MAX_BOOKS];
static int  g_book_count = 0;
static int  g_lib_idx    = 0;
static int  g_reading_book = -1;
static char g_page_buf[MAX_PAGE_BYTES + 1];

// ===== v1.0 BASE + v2.0 PORTED: Persistence (NVS namespace "bookos") =========
static Preferences g_prefs;
static void prefs_load() {
    g_prefs.begin("bookos", true);
    g_theme_idx = g_prefs.getUInt("theme", 1);
    if (g_theme_idx < 0 || g_theme_idx >= NUM_THEMES) g_theme_idx = 1;
    g_prefs.end();
}
static void prefs_save_theme() {
    g_prefs.begin("bookos", false);
    g_prefs.putUInt("theme", g_theme_idx);
    g_prefs.end();
}
static uint16_t book_key_hash(const char *s) {
    uint32_t h = 2166136261u;
    while (*s) { h ^= (uint8_t)*s++; h *= 16777619u; }
    return (uint16_t)(h & 0xFFFF);
}
static void save_book_page(const Book &b) {
    g_prefs.begin("bookos", false);
    char k[12]; snprintf(k, sizeof(k), "p%04X", book_key_hash(b.folder));
    g_prefs.putUInt(k, (uint32_t)b.current_page);
    g_prefs.end();
}
// Bookmark read priority: NVS (v2.0 wc_bookstream_bookmark_get) → v1.0 NVS
// short-hash → 1. This makes NVS the primary source per merge decision, while
// file bookmarks remain as a mirror (see toggle_bookmark).
static int load_book_page(const Book &b) {
    int p = wc_bookstream_bookmark_get(b.folder);
    if (p <= 0) {
        g_prefs.begin("bookos", true);
        char k[12]; snprintf(k, sizeof(k), "p%04X", book_key_hash(b.folder));
        p = (int)g_prefs.getUInt(k, 1);
        g_prefs.end();
    }
    if (p < 1) p = 1;
    // Cap only if total_pages is known (may be 0 during fast-scan)
    if (b.total_pages > 0 && p > b.total_pages) p = b.total_pages;
    return p;
}

// ===== v1.0 BASE: Folder bootstrap + demo book ===============================
static void ensure_dirs() {
    SD_MMC.mkdir("/WATCH");
    SD_MMC.mkdir("/WATCH/BookOS");
    SD_MMC.mkdir("/WATCH/BookOS/Books");
    SD_MMC.mkdir("/WATCH/BookOS/Saves");
    SD_MMC.mkdir("/WATCH/BookOS/Saves/bookmarks");
    SD_MMC.mkdir("/WATCH/BookOS/Themes");
    SD_MMC.mkdir("/WATCH/BookOS/Quotes");
    SD_MMC.mkdir("/WATCH/BookOS/System");

    if (!SD_MMC.exists("/WATCH/BookOS/System/version.txt")) {
        File f = SD_MMC.open("/WATCH/BookOS/System/version.txt", FILE_WRITE);
        if (f) { f.println("BookOS v1.0+v2.0 merged"); f.close(); }
    }
    if (!SD_MMC.exists("/WATCH/BookOS/README.txt")) {
        File f = SD_MMC.open("/WATCH/BookOS/README.txt", FILE_WRITE);
        if (f) {
            f.println("BookOS - drop a folder per book inside Books/");
            f.println("Each folder needs:");
            f.println("  meta.txt   (TITLE=, AUTHOR=, GENRE=)");
            f.println("  0001.txt, 0002.txt, ...  (one TXT = one page)");
            f.println("Recommended: 80-100 words per page, manual line breaks.");
            f.close();
        }
    }
}
static void seed_demo_book() {
    const char *root = "/WATCH/BookOS/Books/Welcome_To_BookOS";
    if (SD_MMC.exists(root)) return;
    SD_MMC.mkdir(root);
    {
        File f = SD_MMC.open("/WATCH/BookOS/Books/Welcome_To_BookOS/meta.txt", FILE_WRITE);
        if (f) {
            f.println("TITLE=Welcome to BookOS");
            f.println("AUTHOR=The Archive");
            f.println("GENRE=Manual");
            f.println("THEME=Navy");
            f.close();
        }
    }
    const char *pages[] = {
        "BOOKOS\n\nA library trapped\ninside a watch.\n\nNo internet.\nNo accounts.\nNo syncing.\n\nJust folders and text.\n\nTap RIGHT to continue.",
        "Each book lives as a folder\nunder /WATCH/BookOS/Books/.\n\nEvery TXT file in that folder\nis one page of the book.\n\n0001.txt   first page\n0002.txt   second page\n0003.txt   ...\n\nDrag and drop. Done.",
        "Controls\n\nTap LEFT  -- previous page\nTap RIGHT -- next page\nSwipe -- turn pages / library\nHold CENTER -- quick menu\nDouble tap -- focus mode\n\nHold BOOT for 1.5s to exit.",
        "Each book remembers\nwhere you stopped.\n\nWhen BookOS boots\nyour last book is already\nselected in the carousel,\nwith a Continue hint\nshowing the exact page.\n\nOne tap and you're back\ninside the story.",
        "Themes\n\nWarm Paper\nOLED Black\nForest Terminal\nDeep Navy\nCrimson Archive\n\nHold center, choose theme.\nThe whole archive shifts.\n\nThat's BookOS.\nWelcome home.",
    };
    char path[80];
    for (int i = 0; i < 5; i++) {
        snprintf(path, sizeof(path),
                 "/WATCH/BookOS/Books/Welcome_To_BookOS/%04d.txt", i + 1);
        File f = SD_MMC.open(path, FILE_WRITE);
        if (f) { f.print(pages[i]); f.close(); }
    }
}

// ===== v1.0 BASE + v2.0 PORTED: Library scan (count_pages_lazy) ==============
static void parse_meta(Book &b) {
    char path[120];
    snprintf(path, sizeof(path), "/WATCH/BookOS/Books/%s/meta.txt", b.folder);
    File f = SD_MMC.open(path, FILE_READ);
    if (!f) {
        strncpy(b.title, b.folder, sizeof(b.title) - 1);
        b.title[sizeof(b.title) - 1] = 0;
        strcpy(b.author, "Unknown");
        return;
    }
    b.title[0] = 0; b.author[0] = 0;
    while (f.available()) {
        String line = f.readStringUntil('\n'); line.trim();
        if (!line.length() || line.startsWith("#")) continue;
        int eq = line.indexOf('='); if (eq < 0) continue;
        String k = line.substring(0, eq); k.trim();
        String v = line.substring(eq + 1); v.trim();
        if (k.equalsIgnoreCase("TITLE")) {
            strncpy(b.title, v.c_str(), sizeof(b.title) - 1);
            b.title[sizeof(b.title) - 1] = 0;
        } else if (k.equalsIgnoreCase("AUTHOR")) {
            strncpy(b.author, v.c_str(), sizeof(b.author) - 1);
            b.author[sizeof(b.author) - 1] = 0;
        }
    }
    f.close();
    if (!b.title[0]) {
        strncpy(b.title, b.folder, sizeof(b.title) - 1);
        b.title[sizeof(b.title) - 1] = 0;
    }
    if (!b.author[0]) strcpy(b.author, "Unknown");
}

// v2.0 name — same algorithm as v1.0 count_pages(); replaces it per merge spec.
static int count_pages_lazy(const char *folder) {
    char path[160];
    snprintf(path, sizeof(path), "%s/Books/%s", WC_PATH_BOOKOS, folder);
    File d = SD_MMC.open(path);
    if (!d || !d.isDirectory()) return 0;
    int n = 0;
    File f = d.openNextFile();
    while (f) {
        String name = f.name(); int sl = name.lastIndexOf('/'); if (sl>=0) name = name.substring(sl+1);
        name.toLowerCase();
        if (!f.isDirectory() && name.endsWith(".txt") && name.length() >= 8) {
            bool digits = true;
            for (int i = 0; i < 4; i++) if (!isdigit(name[i])) { digits = false; break; }
            if (digits) n++;
        }
        f = d.openNextFile();
    }
    return n;
}

// FAST BOOT: scan_books() only reads meta.txt per book — page-file counting
// is DEFERRED to open_book_at() (see ensure_book_pages_counted). This makes
// splash → library reveal effectively instant even with many books on the SD.
static void scan_books() {
    g_book_count = 0;
    File dir = SD_MMC.open("/WATCH/BookOS/Books");
    if (!dir || !dir.isDirectory()) return;
    File f = dir.openNextFile();
    while (f && g_book_count < MAX_BOOKS) {
        if (f.isDirectory()) {
            String n = f.name();
            int slash = n.lastIndexOf('/');
            if (slash >= 0) n = n.substring(slash + 1);
            if (n.length() && !n.startsWith(".")) {
                Book &b = g_books[g_book_count];
                memset(&b, 0, sizeof(b));
                strncpy(b.folder, n.c_str(), sizeof(b.folder) - 1);
                parse_meta(b);
                b.total_pages  = 0;                // counted on first open
                b.current_page = load_book_page(b); // may be 0/1; capped later
                if (b.current_page < 1) b.current_page = 1;
                g_book_count++;
            }
        }
        f = dir.openNextFile();
    }
}

// Called by open_book_at() and (optionally) restore path. Counts .txt pages
// for a single book on demand — cheap for one book, unlike the old boot-time
// walk over ALL books.
static void ensure_book_pages_counted(Book &b) {
    if (b.total_pages > 0) return;
    b.total_pages = count_pages_lazy(b.folder);
    if (b.total_pages < 1) b.total_pages = 0;
    if (b.current_page > b.total_pages && b.total_pages > 0)
        b.current_page = b.total_pages;
    if (b.current_page < 1) b.current_page = 1;
}

// ===== v1.0 BASE: Session save / restore =====================================
static int g_bookmarks_restored = 0;

static void save_session() {
    File f = SD_MMC.open("/WATCH/BookOS/Saves/last_session.txt", FILE_WRITE);
    if (!f) return;
    if (g_reading_book >= 0 && g_reading_book < g_book_count) {
        f.printf("BOOK=%s\n", g_books[g_reading_book].folder);
        f.printf("PAGE=%d\n", g_books[g_reading_book].current_page);
    } else if (g_lib_idx >= 0 && g_lib_idx < g_book_count) {
        f.printf("BOOK=%s\n", g_books[g_lib_idx].folder);
        f.printf("PAGE=%d\n", g_books[g_lib_idx].current_page);
    }
    f.close();
}
static void restore_session_select() {
    File f = SD_MMC.open("/WATCH/BookOS/Saves/last_session.txt", FILE_READ);
    if (!f) return;
    String wantFolder; int wantPage = 1;
    while (f.available()) {
        String line = f.readStringUntil('\n'); line.trim();
        int eq = line.indexOf('='); if (eq < 0) continue;
        String k = line.substring(0, eq);
        String v = line.substring(eq + 1);
        if (k == "BOOK") wantFolder = v;
        if (k == "PAGE") wantPage = v.toInt();
    }
    f.close();
    if (wantFolder.length()) {
        for (int i = 0; i < g_book_count; i++) {
            if (wantFolder == g_books[i].folder) {
                g_lib_idx = i;
                // total_pages may be 0 (deferred). Just remember the target
                // page; open_book_at() → ensure_book_pages_counted() will clamp.
                if (wantPage >= 1) g_books[i].current_page = wantPage;
                break;
            }
        }
    }
    File d = SD_MMC.open("/WATCH/BookOS/Saves/bookmarks");
    if (d && d.isDirectory()) {
        File ff = d.openNextFile();
        while (ff) {
            String nn = ff.name(); nn.toLowerCase();
            if (nn.endsWith(".txt")) g_bookmarks_restored++;
            ff = d.openNextFile();
        }
    }
}

// ===== v1.0 BASE + v2.0 PORTED: Bookmarks — DUAL-WRITE (file + NVS) ==========
//  Toggle writes to BOTH:
//    - file (/WATCH/BookOS/Saves/bookmarks/<folder>.txt) — v1.0 behavior
//    - NVS via wc_bookstream_bookmark() using v2.0 short-hash key
//  Read priority (load_book_page above): NVS first, file/legacy NVS fallback.
static void toggle_bookmark(const Book &b) {
    char path[120];
    snprintf(path, sizeof(path), "/WATCH/BookOS/Saves/bookmarks/%s.txt", b.folder);
    int marks[64]; int n = 0;
    File f = SD_MMC.open(path, FILE_READ);
    if (f) {
        while (f.available() && n < 64) {
            String line = f.readStringUntil('\n'); line.trim();
            if (!line.length()) continue;
            int v = line.toInt();
            if (v > 0) marks[n++] = v;
        }
        f.close();
    }
    bool found = false; int idx = -1;
    for (int i = 0; i < n; i++) if (marks[i] == b.current_page) { found = true; idx = i; break; }
    if (found) {
        for (int i = idx; i < n - 1; i++) marks[i] = marks[i + 1];
        n--;
    } else if (n < 64) {
        marks[n++] = b.current_page;
    }
    File g = SD_MMC.open(path, FILE_WRITE);
    if (g) {
        for (int i = 0; i < n; i++) g.printf("%d\n", marks[i]);
        g.close();
    }
    // v2.0 NVS bookmark mirror — always store the current page as the primary
    // "resume here" marker (independent of the multi-mark file toggle list).
    char key[24]; wc_nvs_key_for(b.folder, key);
    Preferences p; p.begin(WC_NVS_BOOKOS, false); p.putInt(key, b.current_page); p.end();
}

// ===== v1.0 BASE: Page loading (kept for fallback / non-streamed reads) ======
static bool load_page(const Book &b, int page) {
    if (page < 1 || page > b.total_pages) return false;
    char path[140];
    snprintf(path, sizeof(path), "/WATCH/BookOS/Books/%s/%04d.txt", b.folder, page);
    File f = SD_MMC.open(path, FILE_READ);
    if (!f) {
        snprintf(g_page_buf, sizeof(g_page_buf), "[ page %d missing ]", page);
        return false;
    }
    size_t sz = f.size();
    if (sz > MAX_PAGE_BYTES) sz = MAX_PAGE_BYTES;
    size_t r = f.read((uint8_t *)g_page_buf, sz);
    g_page_buf[r] = 0;
    f.close();
    return true;
}

// ===== v1.0 BASE: Time / battery helpers (reader bottom bar) =================
static int g_battery_pct = -1;
static void update_battery() {
    if (!g_has_pmu) { g_battery_pct = -1; return; }
    int p = g_pmu.getBatteryPercent();
    if (p < 0) p = 0; if (p > 100) p = 100;
    g_battery_pct = p;
}
static void fmt_time(char *out, size_t n) {
    struct tm tm;
    if (getLocalTime(&tm, 5)) {
        snprintf(out, n, "%02d:%02d", tm.tm_hour, tm.tm_min);
    } else {
        snprintf(out, n, "--:--");
    }
}

// ===== v1.0 BASE: UI — LVGL objects ==========================================
static lv_obj_t *scr_root;

static lv_obj_t *splash_layer = nullptr;
static lv_obj_t *splash_title = nullptr;
static lv_obj_t *splash_sub   = nullptr;
static lv_obj_t *splash_meta1 = nullptr;
static lv_obj_t *splash_meta2 = nullptr;

static lv_obj_t *lib_layer       = nullptr;
static lv_obj_t *lib_wp_img      = nullptr;   // v2.0 wallpaper background
static lv_obj_t *lib_card_left   = nullptr;
static lv_obj_t *lib_card_center = nullptr;
static lv_obj_t *lib_card_right  = nullptr;
static lv_obj_t *lib_title_lbl   = nullptr;
static lv_obj_t *lib_author_lbl  = nullptr;
static lv_obj_t *lib_progress_lbl= nullptr;
static lv_obj_t *lib_continue_lbl= nullptr;
static lv_obj_t *lib_dots_row    = nullptr;
static lv_obj_t *lib_back_btn    = nullptr;
static lv_obj_t *lib_theme_lbl   = nullptr;
static lv_obj_t *lib_empty_lbl   = nullptr;

static lv_obj_t *read_layer    = nullptr;
static lv_obj_t *read_top_bar  = nullptr;
static lv_obj_t *read_title    = nullptr;
static lv_obj_t *read_pageinfo = nullptr;
static lv_obj_t *read_text     = nullptr;
static lv_obj_t *read_bot_bar  = nullptr;
static lv_obj_t *read_clock    = nullptr;
static lv_obj_t *read_battery  = nullptr;

static lv_obj_t *menu_layer    = nullptr;

static bool g_focus_mode = false;

static void refresh_wallpaper() { wc_wallpaper_apply_to(lib_wp_img); }

// ===== v1.0 BASE: apply_theme ================================================
static void apply_theme() {
    const Theme &t = TH();
    if (scr_root) {
        lv_obj_set_style_bg_color(scr_root, TC(t.bg), 0);
        lv_obj_set_style_bg_opa(scr_root, LV_OPA_COVER, 0);
    }
    if (lib_layer) lv_obj_set_style_bg_color(lib_layer, TC(t.bg), 0);
    auto card_style = [&](lv_obj_t *c, bool center) {
        if (!c) return;
        lv_obj_set_style_bg_color(c, TC(t.surface), 0);
        lv_obj_set_style_bg_opa(c, LV_OPA_COVER, 0);
        lv_obj_set_style_border_color(c, TC(center ? t.accent : t.border), 0);
        lv_obj_set_style_border_width(c, center ? 2 : 1, 0);
        lv_obj_set_style_radius(c, 14, 0);
    };
    card_style(lib_card_left, false);
    card_style(lib_card_right, false);
    card_style(lib_card_center, true);

    if (lib_title_lbl)    lv_obj_set_style_text_color(lib_title_lbl, TC(t.txt), 0);
    if (lib_author_lbl)   lv_obj_set_style_text_color(lib_author_lbl, TC(t.mute), 0);
    if (lib_progress_lbl) lv_obj_set_style_text_color(lib_progress_lbl, TC(t.accent), 0);
    if (lib_continue_lbl) lv_obj_set_style_text_color(lib_continue_lbl, TC(t.mute), 0);
    if (lib_theme_lbl)    lv_obj_set_style_text_color(lib_theme_lbl, TC(t.mute), 0);
    if (lib_empty_lbl)    lv_obj_set_style_text_color(lib_empty_lbl, TC(t.mute), 0);

    if (read_layer)    lv_obj_set_style_bg_color(read_layer, TC(t.bg), 0);
    if (read_top_bar)  lv_obj_set_style_bg_color(read_top_bar, TC(t.bg), 0);
    if (read_bot_bar)  lv_obj_set_style_bg_color(read_bot_bar, TC(t.bg), 0);
    if (read_title)    lv_obj_set_style_text_color(read_title, TC(t.mute), 0);
    if (read_pageinfo) lv_obj_set_style_text_color(read_pageinfo, TC(t.mute), 0);
    if (read_text)     lv_obj_set_style_text_color(read_text, TC(t.txt), 0);
    if (read_clock)    lv_obj_set_style_text_color(read_clock, TC(t.mute), 0);
    if (read_battery)  lv_obj_set_style_text_color(read_battery, TC(t.mute), 0);
}

// ===== v1.0 BASE: Library UI (with v2.0 wallpaper background) ================
static void update_library_view();
static void update_reader_view();
static void reader_advance(int delta);
static void open_quick_menu();
static void close_quick_menu();
static void open_settings_sheet();

static void on_back_btn(lv_event_t *e) { (void)e; return_to_launcher(); }

static void open_book_at(int idx) {
    if (idx < 0 || idx >= g_book_count) return;
    g_reading_book = idx;
    Book &b = g_books[idx];
    // Fast-boot: page count was deferred — count now, on first open of this book.
    ensure_book_pages_counted(b);
    if (b.total_pages <= 0) { g_reading_book = -1; return; }  // empty folder
    if (b.current_page < 1) b.current_page = 1;
    if (b.current_page > b.total_pages) b.current_page = b.total_pages;
    // Start the lazy streamer for this book (v2.0). It primes 10 pages
    // synchronously here, then the FreeRTOS task rolls the next 5 during
    // runtime (WC_PAGE_PREFETCH=5, WC_PAGE_LOOK_AHEAD=3).
    wc_bookstream_open(b.folder, b.total_pages, b.current_page);
    save_session();
    lv_obj_add_flag(lib_layer, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(read_layer, LV_OBJ_FLAG_HIDDEN);
    update_reader_view();
    // Refresh the library card too so the progress line picks up total_pages
    // the next time the user returns to library.
    update_library_view();
}

static lv_point_t g_press_start = {0, 0};
static uint32_t   g_press_t0    = 0;
static bool       g_swiped      = false;
static uint32_t   g_last_tap_t  = 0;

static void lib_input_cb(lv_event_t *e) {
    lv_event_code_t code = lv_event_get_code(e);
    lv_indev_t *indev = lv_indev_get_act();
    if (!indev) return;
    lv_point_t p; lv_indev_get_point(indev, &p);

    if (code == LV_EVENT_PRESSED) {
        g_press_start = p; g_press_t0 = millis(); g_swiped = false;
    } else if (code == LV_EVENT_RELEASED) {
        uint32_t dt = millis() - g_press_t0;
        int dx = p.x - g_press_start.x, dy = p.y - g_press_start.y;
        if (g_swiped) return;
        if (millis() - g_last_swipe_ms < WC_CLICK_AFTER_SWIPE_MS) return;
        if (abs(dx) < 30 && abs(dy) < 30 && dt < 600) {
            if (p.x > 120 && p.x < LCD_WIDTH - 120 && p.y > 120 && p.y < 380) {
                open_book_at(g_lib_idx);
            }
        }
    }
}

static void build_library_ui() {
    lib_layer = lv_obj_create(scr_root);
    lv_obj_remove_style_all(lib_layer);
    lv_obj_set_size(lib_layer, LCD_WIDTH, LCD_HEIGHT);
    lv_obj_set_pos(lib_layer, 0, 0);
    lv_obj_set_style_bg_opa(lib_layer, LV_OPA_COVER, 0);
    lv_obj_clear_flag(lib_layer, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(lib_layer, lib_input_cb, LV_EVENT_PRESSED,  NULL);
    lv_obj_add_event_cb(lib_layer, lib_input_cb, LV_EVENT_RELEASED, NULL);

    // v2.0 PORTED: wallpaper background (subtle)
    lib_wp_img = lv_img_create(lib_layer);
    lv_obj_set_size(lib_wp_img, LCD_WIDTH, LCD_HEIGHT);
    lv_obj_align(lib_wp_img, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_opa(lib_wp_img, LV_OPA_20, 0);
    lv_obj_add_flag(lib_wp_img, LV_OBJ_FLAG_HIDDEN);

    lib_theme_lbl = lv_label_create(lib_layer);
    lv_label_set_text(lib_theme_lbl, "BOOKOS");
    lv_obj_set_style_text_font(lib_theme_lbl, &lv_font_montserrat_14, 0);
    lv_obj_align(lib_theme_lbl, LV_ALIGN_TOP_LEFT, 22, 18);

    lib_empty_lbl = lv_label_create(lib_layer);
    lv_label_set_text(lib_empty_lbl,
        "Library is empty.\n\nAdd a folder under\n/WATCH/BookOS/Books/\nwith numbered TXT pages.");
    lv_obj_set_style_text_align(lib_empty_lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(lib_empty_lbl, &lv_font_montserrat_16, 0);
    lv_obj_align(lib_empty_lbl, LV_ALIGN_CENTER, 0, -20);
    lv_obj_add_flag(lib_empty_lbl, LV_OBJ_FLAG_HIDDEN);

    int cardW = 180, cardH = 240;
    int cy = (LCD_HEIGHT - cardH) / 2 - 20;
    lib_card_left = lv_obj_create(lib_layer);
    lv_obj_remove_style_all(lib_card_left);
    lv_obj_set_size(lib_card_left, 120, 200);
    lv_obj_set_pos(lib_card_left, -40, cy + 20);
    lv_obj_set_style_bg_opa(lib_card_left, LV_OPA_50, 0);
    lv_obj_clear_flag(lib_card_left, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);

    lib_card_right = lv_obj_create(lib_layer);
    lv_obj_remove_style_all(lib_card_right);
    lv_obj_set_size(lib_card_right, 120, 200);
    lv_obj_set_pos(lib_card_right, LCD_WIDTH - 80, cy + 20);
    lv_obj_set_style_bg_opa(lib_card_right, LV_OPA_50, 0);
    lv_obj_clear_flag(lib_card_right, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);

    lib_card_center = lv_obj_create(lib_layer);
    lv_obj_remove_style_all(lib_card_center);
    lv_obj_set_size(lib_card_center, cardW, cardH);
    lv_obj_set_pos(lib_card_center, (LCD_WIDTH - cardW) / 2, cy);
    lv_obj_set_style_bg_opa(lib_card_center, LV_OPA_COVER, 0);
    lv_obj_clear_flag(lib_card_center, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);

    lib_title_lbl = lv_label_create(lib_layer);
    lv_label_set_long_mode(lib_title_lbl, LV_LABEL_LONG_DOT);
    lv_obj_set_width(lib_title_lbl, LCD_WIDTH - 40);
    lv_obj_set_style_text_align(lib_title_lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(lib_title_lbl, &lv_font_montserrat_22, 0);
    lv_obj_align(lib_title_lbl, LV_ALIGN_TOP_MID, 0, cy + cardH + 12);

    lib_author_lbl = lv_label_create(lib_layer);
    lv_label_set_long_mode(lib_author_lbl, LV_LABEL_LONG_DOT);
    lv_obj_set_width(lib_author_lbl, LCD_WIDTH - 40);
    lv_obj_set_style_text_align(lib_author_lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(lib_author_lbl, &lv_font_montserrat_14, 0);
    lv_obj_align(lib_author_lbl, LV_ALIGN_TOP_MID, 0, cy + cardH + 42);

    lib_progress_lbl = lv_label_create(lib_layer);
    lv_obj_set_style_text_font(lib_progress_lbl, &lv_font_montserrat_14, 0);
    lv_obj_align(lib_progress_lbl, LV_ALIGN_TOP_MID, 0, cy + cardH + 62);

    lib_continue_lbl = lv_label_create(lib_layer);
    lv_obj_set_style_text_font(lib_continue_lbl, &lv_font_montserrat_14, 0);
    lv_obj_align(lib_continue_lbl, LV_ALIGN_TOP_MID, 0, cy + cardH + 82);

    lib_back_btn = lv_btn_create(lib_layer);
    lv_obj_set_size(lib_back_btn, LCD_WIDTH - 60, 46);
    lv_obj_align(lib_back_btn, LV_ALIGN_BOTTOM_MID, 0, -14);
    lv_obj_set_style_bg_color(lib_back_btn, lv_color_hex(0x05161B), 0);
    lv_obj_set_style_bg_opa(lib_back_btn, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(lib_back_btn, lv_color_hex(CYAN), 0);
    lv_obj_set_style_border_width(lib_back_btn, 2, 0);
    lv_obj_set_style_radius(lib_back_btn, 23, 0);
    lv_obj_t *bl = lv_label_create(lib_back_btn);
    lv_label_set_text(bl, "BACK TO LAUNCHER");
    lv_obj_set_style_text_color(bl, lv_color_hex(CYAN), 0);
    lv_obj_set_style_text_font(bl, &lv_font_montserrat_14, 0);
    lv_obj_center(bl);
    lv_obj_add_event_cb(lib_back_btn, on_back_btn, LV_EVENT_CLICKED, NULL);
}

static void update_library_view() {
    if (!lib_layer) return;
    if (g_book_count == 0) {
        lv_obj_clear_flag(lib_empty_lbl, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(lib_card_left,   LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(lib_card_center, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(lib_card_right,  LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(lib_title_lbl, "");
        lv_label_set_text(lib_author_lbl, "");
        lv_label_set_text(lib_progress_lbl, "");
        lv_label_set_text(lib_continue_lbl, "");
        return;
    }
    lv_obj_add_flag(lib_empty_lbl, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(lib_card_center, LV_OBJ_FLAG_HIDDEN);

    if (g_lib_idx > 0) lv_obj_clear_flag(lib_card_left, LV_OBJ_FLAG_HIDDEN);
    else                lv_obj_add_flag(lib_card_left, LV_OBJ_FLAG_HIDDEN);
    if (g_lib_idx < g_book_count - 1) lv_obj_clear_flag(lib_card_right, LV_OBJ_FLAG_HIDDEN);
    else                              lv_obj_add_flag(lib_card_right, LV_OBJ_FLAG_HIDDEN);

    const Book &b = g_books[g_lib_idx];
    lv_label_set_text(lib_title_lbl, b.title);
    lv_label_set_text(lib_author_lbl, b.author);

    char buf[64];
    if (b.total_pages > 0) {
        int pct = (b.current_page * 100) / b.total_pages;
        snprintf(buf, sizeof(buf), "%d%%  -  page %d / %d", pct, b.current_page, b.total_pages);
    } else {
        // Fast-boot: pages not counted yet. Show a lighter hint until open.
        if (b.current_page > 1) snprintf(buf, sizeof(buf), "resume p%d", b.current_page);
        else                    snprintf(buf, sizeof(buf), "pages count on open");
    }
    lv_label_set_text(lib_progress_lbl, buf);

    if (b.current_page > 1)
        snprintf(buf, sizeof(buf), "Continue - page %d", b.current_page);
    else
        snprintf(buf, sizeof(buf), "Tap to begin");
    lv_label_set_text(lib_continue_lbl, buf);

    char tag[64];
    snprintf(tag, sizeof(tag), "BOOKOS  -  %d of %d  -  %s",
             g_lib_idx + 1, g_book_count, TH().name);
    lv_label_set_text(lib_theme_lbl, tag);
}

// ===== v1.0 BASE: Reader UI (page reads route through wc_bookstream) =========
static void reader_input_cb(lv_event_t *e) {
    lv_event_code_t code = lv_event_get_code(e);
    lv_indev_t *indev = lv_indev_get_act();
    if (!indev) return;
    lv_point_t p; lv_indev_get_point(indev, &p);

    if (code == LV_EVENT_PRESSED) {
        g_press_start = p; g_press_t0 = millis(); g_swiped = false;
    } else if (code == LV_EVENT_RELEASED) {
        uint32_t dt = millis() - g_press_t0;
        int dx = p.x - g_press_start.x;
        int dy = p.y - g_press_start.y;
        if (g_swiped) return;
        // Suppress tap if a swipe was just detected globally
        if (millis() - g_last_swipe_ms < WC_CLICK_AFTER_SWIPE_MS) return;
        if (abs(dx) < 25 && abs(dy) < 25) {
            if (dt > 600) {
                open_quick_menu();
                return;
            }
            uint32_t now = millis();
            if (now - g_last_tap_t < 320 && p.x > LCD_WIDTH / 3 && p.x < 2 * LCD_WIDTH / 3) {
                g_focus_mode = !g_focus_mode;
                if (g_focus_mode) {
                    lv_obj_add_flag(read_top_bar, LV_OBJ_FLAG_HIDDEN);
                    lv_obj_add_flag(read_bot_bar, LV_OBJ_FLAG_HIDDEN);
                } else {
                    lv_obj_clear_flag(read_top_bar, LV_OBJ_FLAG_HIDDEN);
                    lv_obj_clear_flag(read_bot_bar, LV_OBJ_FLAG_HIDDEN);
                }
                g_last_tap_t = 0;
                return;
            }
            g_last_tap_t = now;
            if (p.x < LCD_WIDTH / 3)          reader_advance(-1);
            else if (p.x > 2 * LCD_WIDTH / 3) reader_advance(+1);
        }
    }
}

static void build_reader_ui() {
    read_layer = lv_obj_create(scr_root);
    lv_obj_remove_style_all(read_layer);
    lv_obj_set_size(read_layer, LCD_WIDTH, LCD_HEIGHT);
    lv_obj_set_pos(read_layer, 0, 0);
    lv_obj_set_style_bg_opa(read_layer, LV_OPA_COVER, 0);
    lv_obj_clear_flag(read_layer, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(read_layer, reader_input_cb, LV_EVENT_PRESSED,  NULL);
    lv_obj_add_event_cb(read_layer, reader_input_cb, LV_EVENT_RELEASED, NULL);

    read_top_bar = lv_obj_create(read_layer);
    lv_obj_remove_style_all(read_top_bar);
    lv_obj_set_size(read_top_bar, LCD_WIDTH, 38);
    lv_obj_set_pos(read_top_bar, 0, 0);
    lv_obj_set_style_bg_opa(read_top_bar, LV_OPA_COVER, 0);
    lv_obj_clear_flag(read_top_bar, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);

    read_title = lv_label_create(read_top_bar);
    lv_label_set_long_mode(read_title, LV_LABEL_LONG_DOT);
    lv_obj_set_width(read_title, LCD_WIDTH - 140);
    lv_obj_set_style_text_font(read_title, &lv_font_montserrat_14, 0);
    lv_obj_align(read_title, LV_ALIGN_LEFT_MID, 18, 0);

    read_pageinfo = lv_label_create(read_top_bar);
    lv_obj_set_style_text_font(read_pageinfo, &lv_font_montserrat_14, 0);
    lv_obj_align(read_pageinfo, LV_ALIGN_RIGHT_MID, -18, 0);

    read_text = lv_label_create(read_layer);
    lv_label_set_long_mode(read_text, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(read_text, 340);
    lv_obj_set_pos(read_text, (LCD_WIDTH - 340) / 2, 64);
    lv_obj_set_height(read_text, 380);
    lv_obj_set_style_text_font(read_text, &lv_font_montserrat_22, 0);
    lv_obj_set_style_text_line_space(read_text, 6, 0);

    read_bot_bar = lv_obj_create(read_layer);
    lv_obj_remove_style_all(read_bot_bar);
    lv_obj_set_size(read_bot_bar, LCD_WIDTH, 36);
    lv_obj_set_pos(read_bot_bar, 0, LCD_HEIGHT - 36);
    lv_obj_set_style_bg_opa(read_bot_bar, LV_OPA_COVER, 0);
    lv_obj_clear_flag(read_bot_bar, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);

    read_clock = lv_label_create(read_bot_bar);
    lv_obj_set_style_text_font(read_clock, &lv_font_montserrat_14, 0);
    lv_obj_align(read_clock, LV_ALIGN_LEFT_MID, 18, 0);

    read_battery = lv_label_create(read_bot_bar);
    lv_obj_set_style_text_font(read_battery, &lv_font_montserrat_14, 0);
    lv_obj_align(read_battery, LV_ALIGN_RIGHT_MID, -18, 0);

    lv_obj_add_flag(read_layer, LV_OBJ_FLAG_HIDDEN);
}

static void update_reader_view() {
    if (g_reading_book < 0) return;
    Book &b = g_books[g_reading_book];
    // v2.0 streamed read (preferred). Falls back to v1.0 load_page() if the
    // ring hasn't cached this page yet (rare — first-hit of a jump).
    if (!wc_bookstream_get_copy(b.current_page, g_page_buf, sizeof(g_page_buf))) {
        load_page(b, b.current_page);
    }
    lv_label_set_text(read_text, g_page_buf);
    lv_label_set_text(read_title, b.title);

    char info[40];
    snprintf(info, sizeof(info), "%d / %d", b.current_page, b.total_pages);
    lv_label_set_text(read_pageinfo, info);

    char tt[16]; fmt_time(tt, sizeof(tt));
    lv_label_set_text(read_clock, tt);
    update_battery();
    char bb[16];
    if (g_battery_pct >= 0) snprintf(bb, sizeof(bb), "BAT %d%%", g_battery_pct);
    else                    snprintf(bb, sizeof(bb), "BAT --");
    lv_label_set_text(read_battery, bb);
}

static void reader_advance(int delta) {
    if (g_reading_book < 0) return;
    Book &b = g_books[g_reading_book];
    int p = b.current_page + delta;
    if (p < 1 || p > b.total_pages) return;
    b.current_page = p;
    // v2.0 tell the streamer where the reader now is (drives look-ahead)
    wc_bookstream_seek(p);
    save_book_page(b);
    save_session();
    update_reader_view();
}

// ===== v1.0 BASE: Quick menu + v2.0 PORTED: Settings sheet action ============
static void qm_close_cb(lv_event_t *) { close_quick_menu(); }

static void qm_theme_cb(lv_event_t *) {
    g_theme_idx = (g_theme_idx + 1) % NUM_THEMES;
    prefs_save_theme();
    apply_theme();
}
static void qm_bookmark_cb(lv_event_t *) {
    if (g_reading_book >= 0) toggle_bookmark(g_books[g_reading_book]);
    close_quick_menu();
}
static void qm_exit_cb(lv_event_t *) {
    save_session();
    // Persist NVS bookmark for the book we're leaving (v2.0)
    if (g_reading_book >= 0) {
        Book &b = g_books[g_reading_book];
        wc_bookstream_bookmark(b.current_page);
    }
    wc_bookstream_close();
    g_reading_book = -1;
    g_focus_mode = false;
    lv_obj_clear_flag(read_top_bar, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(read_bot_bar, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(read_layer, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(lib_layer, LV_OBJ_FLAG_HIDDEN);
    update_library_view();
    close_quick_menu();
}
static void qm_launcher_cb(lv_event_t *) { return_to_launcher(); }
static void qm_settings_cb(lv_event_t *) { close_quick_menu(); open_settings_sheet(); }

static void open_quick_menu() {
    if (menu_layer) return;
    const Theme &t = TH();
    menu_layer = lv_obj_create(scr_root);
    lv_obj_remove_style_all(menu_layer);
    lv_obj_set_size(menu_layer, LCD_WIDTH, LCD_HEIGHT);
    lv_obj_set_pos(menu_layer, 0, 0);
    lv_obj_set_style_bg_color(menu_layer, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(menu_layer, LV_OPA_70, 0);
    lv_obj_clear_flag(menu_layer, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(menu_layer, qm_close_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *panel = lv_obj_create(menu_layer);
    lv_obj_remove_style_all(panel);
    lv_obj_set_size(panel, 320, 430);
    lv_obj_align(panel, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(panel, TC(t.surface), 0);
    lv_obj_set_style_bg_opa(panel, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(panel, TC(t.border), 0);
    lv_obj_set_style_border_width(panel, 1, 0);
    lv_obj_set_style_radius(panel, 16, 0);
    lv_obj_set_style_pad_all(panel, 18, 0);
    lv_obj_clear_flag(panel, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title = lv_label_create(panel);
    lv_label_set_text(title, "QUICK MENU");
    lv_obj_set_style_text_color(title, TC(t.mute), 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_14, 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 0);

    auto make_btn = [&](const char *label, int y, lv_event_cb_t cb, uint32_t border) {
        lv_obj_t *b = lv_btn_create(panel);
        lv_obj_set_size(b, 270, 48);
        lv_obj_align(b, LV_ALIGN_TOP_MID, 0, y);
        lv_obj_set_style_bg_color(b, TC(t.bg), 0);
        lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
        lv_obj_set_style_border_color(b, lv_color_hex(border), 0);
        lv_obj_set_style_border_width(b, 1, 0);
        lv_obj_set_style_radius(b, 12, 0);
        lv_obj_t *l = lv_label_create(b);
        lv_label_set_text(l, label);
        lv_obj_set_style_text_color(l, TC(t.txt), 0);
        lv_obj_set_style_text_font(l, &lv_font_montserrat_16, 0);
        lv_obj_center(l);
        lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, NULL);
        return b;
    };

    char tbuf[48]; snprintf(tbuf, sizeof(tbuf), "THEME: %s", t.name);
    make_btn(tbuf,             38,  qm_theme_cb,    t.accent);
    make_btn("TOGGLE BOOKMARK",  92, qm_bookmark_cb, t.border);
    make_btn("SETTINGS",        146, qm_settings_cb, t.accent);
    make_btn("BACK TO LIBRARY", 200, qm_exit_cb,     t.border);
    make_btn("BACK TO LAUNCHER",254, qm_launcher_cb, CYAN);

    lv_obj_t *hint = lv_label_create(panel);
    lv_label_set_text(hint, "tap outside to close");
    lv_obj_set_style_text_color(hint, TC(t.mute), 0);
    lv_obj_set_style_text_font(hint, &lv_font_montserrat_14, 0);
    lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, 0);
}
static void close_quick_menu() {
    if (!menu_layer) return;
    lv_obj_del(menu_layer);
    menu_layer = nullptr;
}

// ===== v2.0 PORTED: Settings sheet population (brightness/wallpaper/theme) ===
static char g_wc_theme_dd_items[512] = {0};

static void wc_settings_brightness_cb(int v, void *) {
    if (v < 10)  v = 10;
    if (v > 255) v = 255;
    co5300_set_brightness((uint8_t)v);
}
static void wc_settings_wallpaper_next_cb(void *) {
    if (!wc_wallpaper_count()) return;
    wc_wallpaper_set(wc_wallpaper_active_index() + 1);
    refresh_wallpaper();
}
static void wc_settings_wc_theme_cb(int idx, void *) {
    wc_theme_set(idx);
    // wc_core theme changes affect settings sheet & battery pill only. To sync
    // the classic v1.0 reader theme too, map first 5 wc themes onto NUM_THEMES.
    if (idx >= 0 && idx < NUM_THEMES) {
        g_theme_idx = idx;
        prefs_save_theme();
        apply_theme();
    }
}
static void wc_settings_v1_theme_cb(int idx, void *) {
    if (idx < 0 || idx >= NUM_THEMES) return;
    g_theme_idx = idx;
    prefs_save_theme();
    apply_theme();
}
static void wc_settings_return_launcher_cb(void *) { return_to_launcher(); }

static void open_settings_sheet() {
    // Build the dropdown items strings on demand
    g_wc_theme_dd_items[0] = 0;
    for (int i = 0; i < wc_theme_count(); i++) {
        strncat(g_wc_theme_dd_items, wc_theme_name_at(i),
                sizeof(g_wc_theme_dd_items) - strlen(g_wc_theme_dd_items) - 2);
        if (i < wc_theme_count() - 1)
            strncat(g_wc_theme_dd_items, "\n",
                    sizeof(g_wc_theme_dd_items) - strlen(g_wc_theme_dd_items) - 1);
    }
    static char v1_theme_items[256];
    v1_theme_items[0] = 0;
    for (int i = 0; i < NUM_THEMES; i++) {
        strncat(v1_theme_items, THEMES[i].name, sizeof(v1_theme_items) - strlen(v1_theme_items) - 2);
        if (i < NUM_THEMES - 1) strncat(v1_theme_items, "\n", sizeof(v1_theme_items) - strlen(v1_theme_items) - 1);
    }

    wc_settings_clear();

    WcCard c_bright = {};
    c_bright.kind = WC_CARD_SLIDER;
    c_bright.title = "Brightness";
    c_bright.subtitle = "panel backlight";
    c_bright.slider_min = 10; c_bright.slider_max = 255;
    c_bright.slider_val = g_brightness;
    c_bright.on_slider = wc_settings_brightness_cb;
    wc_settings_add(c_bright);

    WcCard c_theme = {};
    c_theme.kind = WC_CARD_DROPDOWN;
    c_theme.title = "Reader Theme";
    c_theme.subtitle = "v1 palette (5)";
    c_theme.dropdown_items = v1_theme_items;
    c_theme.dropdown_idx = g_theme_idx;
    c_theme.on_dropdown = wc_settings_v1_theme_cb;
    wc_settings_add(c_theme);

    WcCard c_wctheme = {};
    c_wctheme.kind = WC_CARD_DROPDOWN;
    c_wctheme.title = "System Theme";
    c_wctheme.subtitle = "wc_core palette (10)";
    c_wctheme.dropdown_items = g_wc_theme_dd_items;
    c_wctheme.dropdown_idx = g_wc_theme_idx;
    c_wctheme.on_dropdown = wc_settings_wc_theme_cb;
    wc_settings_add(c_wctheme);

    WcCard c_wp = {};
    c_wp.kind = WC_CARD_ACTION;
    c_wp.title = "Next Wallpaper";
    char wpsub[48];
    if (wc_wallpaper_count()) snprintf(wpsub, sizeof(wpsub), "%d in /WATCH/PHOTOS", wc_wallpaper_count());
    else                      snprintf(wpsub, sizeof(wpsub), "no .bin files found");
    c_wp.subtitle = wpsub;
    c_wp.on_action = wc_settings_wallpaper_next_cb;
    wc_settings_add(c_wp);

    WcCard c_ret = {};
    c_ret.kind = WC_CARD_ACTION;
    c_ret.title = "Return to Launcher";
    c_ret.subtitle = "boot LauncherOS.bin";
    c_ret.on_action = wc_settings_return_launcher_cb;
    wc_settings_add(c_ret);

    wc_settings_open();
}

// ===== v1.0 BASE: Boot splash ("ARCHIVE RECOVERY") ==========================
static void build_splash_ui() {
    splash_layer = lv_obj_create(scr_root);
    lv_obj_remove_style_all(splash_layer);
    lv_obj_set_size(splash_layer, LCD_WIDTH, LCD_HEIGHT);
    lv_obj_set_pos(splash_layer, 0, 0);
    lv_obj_set_style_bg_color(splash_layer, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(splash_layer, LV_OPA_COVER, 0);
    lv_obj_clear_flag(splash_layer, LV_OBJ_FLAG_SCROLLABLE);

    splash_title = lv_label_create(splash_layer);
    lv_label_set_text(splash_title, "BOOKOS");
    lv_obj_set_style_text_color(splash_title, lv_color_hex(0xEEEEEE), 0);
    lv_obj_set_style_text_font(splash_title, &lv_font_montserrat_32, 0);
    lv_obj_set_style_text_letter_space(splash_title, 6, 0);
    lv_obj_align(splash_title, LV_ALIGN_CENTER, 0, -50);

    splash_sub = lv_label_create(splash_layer);
    lv_label_set_text(splash_sub, "");
    lv_obj_set_style_text_color(splash_sub, lv_color_hex(0x707070), 0);
    lv_obj_set_style_text_font(splash_sub, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_letter_space(splash_sub, 2, 0);
    lv_obj_align(splash_sub, LV_ALIGN_CENTER, 0, -10);

    splash_meta1 = lv_label_create(splash_layer);
    lv_label_set_text(splash_meta1, "");
    lv_obj_set_style_text_color(splash_meta1, lv_color_hex(0x4FB3FF), 0);
    lv_obj_set_style_text_font(splash_meta1, &lv_font_montserrat_14, 0);
    lv_obj_align(splash_meta1, LV_ALIGN_CENTER, 0, 40);

    splash_meta2 = lv_label_create(splash_layer);
    lv_label_set_text(splash_meta2, "");
    lv_obj_set_style_text_color(splash_meta2, lv_color_hex(0x4FB3FF), 0);
    lv_obj_set_style_text_font(splash_meta2, &lv_font_montserrat_14, 0);
    lv_obj_align(splash_meta2, LV_ALIGN_CENTER, 0, 70);

    lv_obj_t *ver = lv_label_create(splash_layer);
    lv_label_set_text(ver, "v1.0 + v2.0 merged  -  offline archive");
    lv_obj_set_style_text_color(ver, lv_color_hex(0x404040), 0);
    lv_obj_set_style_text_font(ver, &lv_font_montserrat_14, 0);
    lv_obj_align(ver, LV_ALIGN_BOTTOM_MID, 0, -22);
}

static void splash_set(const char *sub, const char *m1, const char *m2) {
    if (splash_sub)   lv_label_set_text(splash_sub,   sub ? sub : "");
    if (splash_meta1) lv_label_set_text(splash_meta1, m1 ? m1 : "");
    if (splash_meta2) lv_label_set_text(splash_meta2, m2 ? m2 : "");
    lv_timer_handler();
}

static void splash_run_and_dismiss() {
    // Fast-boot: work is done off-splash; keep splash short & readable.
    splash_set("Recovering archive...", "", "");
    for (int t = 0; t < 4; t++) { delay(30); wc_boot_button_poll(); lv_timer_handler(); }

    char m1[40], m2[40];
    snprintf(m1, sizeof(m1), "%d books found", g_book_count);
    snprintf(m2, sizeof(m2), "%d bookmarks restored", g_bookmarks_restored);
    splash_set("Scanning library...", m1, m2);
    for (int t = 0; t < 6; t++) { delay(30); wc_boot_button_poll(); lv_timer_handler(); }

    splash_set("Restoring last session...", m1, m2);
    for (int t = 0; t < 4; t++) { delay(30); wc_boot_button_poll(); lv_timer_handler(); }

    lv_obj_clear_flag(lib_layer, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(splash_layer);

    for (int o = LV_OPA_COVER; o >= 0; o -= 24) {
        lv_obj_set_style_bg_opa(splash_layer, o < 0 ? 0 : o, 0);
        lv_obj_set_style_text_opa(splash_title, o < 0 ? 0 : o, 0);
        lv_obj_set_style_text_opa(splash_sub,   o < 0 ? 0 : o, 0);
        lv_obj_set_style_text_opa(splash_meta1, o < 0 ? 0 : o, 0);
        lv_obj_set_style_text_opa(splash_meta2, o < 0 ? 0 : o, 0);
        lv_timer_handler();
        delay(24);
    }
    lv_obj_add_flag(splash_layer, LV_OBJ_FLAG_HIDDEN);
}

// ===== setup() / loop() ======================================================
static void build_ui_root() {
    scr_root = lv_scr_act();
    lv_obj_set_style_bg_color(scr_root, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(scr_root, LV_OPA_COVER, 0);
    lv_obj_clear_flag(scr_root, LV_OBJ_FLAG_SCROLLABLE);
}

void setup() {
    // Order per merge spec:
    //   wc_hw_boot -> panel on -> splash UI -> SD init -> ensure_dirs ->
    //   seed_demo_book -> prefs_load -> scan_books -> restore_session_select ->
    //   build library + reader UIs -> apply_theme -> refresh_wallpaper ->
    //   splash_run_and_dismiss -> reveal library -> set CPU freq.
    launcher_app_boot();     // == wc_hw_boot() (SD_MMC.begin() done inside)
    panel_on(220);

    build_ui_root();
    build_splash_ui();
    lv_timer_handler();

    ensure_dirs();
    seed_demo_book();

    prefs_load();
    wc_theme_load();
    wc_wallpaper_scan();

    scan_books();
    restore_session_select();

    build_library_ui();
    build_reader_ui();
    wc_settings_begin();     // create settings sheet screen (not shown yet)

    lv_obj_add_flag(lib_layer, LV_OBJ_FLAG_HIDDEN);
    apply_theme();
    refresh_wallpaper();
    update_library_view();
    lv_timer_handler();

    splash_run_and_dismiss();
    lv_obj_clear_flag(lib_layer, LV_OBJ_FLAG_HIDDEN);

    setCpuFrequencyMhz(160);
    Serial.printf("[BookOS] merged v1.0+v2.0 ready. books=%d bookmarks=%d\n",
                  g_book_count, g_bookmarks_restored);
}

void loop() {
    lv_timer_handler();
    wc_pmu_refresh();
    wc_boot_button_poll();

    // ---- Global swipe dispatch (v2.0 PORTED) --------------------------------
    if (g_pending_swipe != WC_SWIPE_NONE) {
        int8_t ev = g_pending_swipe;
        g_pending_swipe = WC_SWIPE_NONE;
        g_last_swipe_ms = millis();
        g_swiped = true;

        if (wc_settings_is_open()) {
            wc_settings_handle_swipe(ev);
        } else if (menu_layer) {
            if (ev == WC_SWIPE_DOWN || ev == WC_SWIPE_UP) close_quick_menu();
        } else if (g_reading_book >= 0) {
            if      (ev == WC_SWIPE_LEFT)  reader_advance(+1);
            else if (ev == WC_SWIPE_RIGHT) reader_advance(-1);
            else if (ev == WC_SWIPE_DOWN)  { /* back to library via quick menu path */
                save_session();
                Book &b = g_books[g_reading_book];
                wc_bookstream_bookmark(b.current_page);
                wc_bookstream_close();
                g_reading_book = -1;
                g_focus_mode = false;
                lv_obj_clear_flag(read_top_bar, LV_OBJ_FLAG_HIDDEN);
                lv_obj_clear_flag(read_bot_bar, LV_OBJ_FLAG_HIDDEN);
                lv_obj_add_flag(read_layer, LV_OBJ_FLAG_HIDDEN);
                lv_obj_clear_flag(lib_layer, LV_OBJ_FLAG_HIDDEN);
                update_library_view();
            }
        } else {
            // library
            if (g_book_count) {
                if (ev == WC_SWIPE_LEFT)  { if (g_lib_idx < g_book_count - 1) { g_lib_idx++; update_library_view(); save_session(); } }
                else if (ev == WC_SWIPE_RIGHT) { if (g_lib_idx > 0) { g_lib_idx--; update_library_view(); save_session(); } }
                else if (ev == WC_SWIPE_UP)    { open_settings_sheet(); }
            }
        }
    }

    // ---- Periodic clock / battery refresh (v1.0) ----------------------------
    static uint32_t tick_t = 0;
    if (millis() - tick_t > 5000) {
        tick_t = millis();
        if (g_reading_book >= 0 && !g_focus_mode) {
            char tt[16]; fmt_time(tt, sizeof(tt));
            lv_label_set_text(read_clock, tt);
            update_battery();
            char bb[16];
            if (g_battery_pct >= 0) snprintf(bb, sizeof(bb), "BAT %d%%", g_battery_pct);
            else                    snprintf(bb, sizeof(bb), "BAT --");
            lv_label_set_text(read_battery, bb);
        }
    }

    delay(3);
}
