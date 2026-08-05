// =============================================================================
//  GameOS.ino v2.0  --  on-demand game loader (manifest, WDT, heap sanity)
//  Target : Waveshare ESP32-S3 Touch AMOLED 2.06 (410 x 502)
//  Framework: Arduino-ESP32, LVGL 8.4, FreeRTOS
//  SINGLE-FILE Arduino sketch -- no headers, no libwatchcore folder.
// =============================================================================
// ===========================================================================
//  BEGIN wc_core (inlined libwatchcore -- theme + wallpaper + carousel +
//  settings sheet + splash + BOOT-return + battery).  Kept in this .ino
//  per the "single .ino per folder" rule.
// ===========================================================================
#include <Arduino.h>
#include <FS.h>
#include <SD_MMC.h>
#include <Wire.h>
#include <Preferences.h>
#include <memory>
#include <Update.h>

#include "Arduino_GFX_Library.h"
#include "Arduino_DriveBus_Library.h"
#include "lv_conf.h"
#include "lvgl.h"

#define XPOWERS_CHIP_AXP2101
#include "XPowersLib.h"

#include <esp_system.h>
#include <esp_sleep.h>
#include <esp_heap_caps.h>
#include <esp_task_wdt.h>
#include <esp_ota_ops.h>
#include <esp_partition.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>

#define WC_LCD_WIDTH   410
#define WC_LCD_HEIGHT  502
#define WC_LCD_SDIO0     4
#define WC_LCD_SDIO1     5
#define WC_LCD_SDIO2     6
#define WC_LCD_SDIO3     7
#define WC_LCD_SCLK     11
#define WC_LCD_CS       12
#define WC_LCD_RESET     8
#define WC_IIC_SDA      15
#define WC_IIC_SCL      14
#define WC_TP_INT       38
#define WC_TP_RESET      9
#define WC_SDMMC_CLK     2
#define WC_SDMMC_CMD     1
#define WC_SDMMC_DATA    3
#define WC_BOOT_BTN      0
#define WC_PWR_BTN_PIN  10
#define WC_SWIPE_THRESHOLD_PX     25
#define WC_SWIPE_MAX_DURATION_MS 800
#define WC_TAP_THRESHOLD_PX       20
#define WC_SCREEN_ANIM_MS        120
#define WC_CLICK_AFTER_SWIPE_MS  250   // v2.1: ported from LauncherOS (was 350)
#define WC_BOOT_LONGPRESS_MS    1500
#define WC_DEFAULT_BRIGHTNESS    140
#define WC_TP_RELEASE_HYST        3    // v2.1: touch-release samples before REL (LauncherOS parity)
#define WC_SD_MOUNT_RETRIES       3    // v2.1: graceful SD mount
#define WC_PATH_BIN         "/WATCH/bin"
#define WC_PATH_PHOTOS      "/WATCH/PHOTOS"
#define WC_PATH_THEMES      "/WATCH/THEMES"
#define WC_PATH_LAUNCHER    "/WATCH/bin/launcheros.bin"
#define WC_PATH_GAMES_MFT   "/GAMES/manifest.json"
#define WC_PATH_BOOKOS      "/WATCH/BookOS"
#define WC_NVS_LAUNCHER     "dcw"
#define WC_NVS_THEME        "wctheme"
#define WC_NVS_BOOKOS       "bookos"
#define WC_NVS_TACTICAL     "tacos"



// v2.1: POWER_SAVE = 1 drops CPU to 80 MHz once boot is complete (LauncherOS parity).
#ifndef POWER_SAVE
#define POWER_SAVE 1
#endif
 // ---- forward declarations for Arduino IDE auto-prototype hoisting --------
struct WcCard;
struct WcCarSpec;
struct WcCar;
struct WcGameEntry;

typedef void (*wc_toggle_cb_t)(bool, void*);
typedef void (*wc_slider_cb_t)(int, void*);
typedef void (*wc_dropdown_cb_t)(int, void*);
typedef void (*wc_action_cb_t)(void*);
typedef void (*wc_subview_build_t)(lv_obj_t*, void*);
typedef lv_obj_t* (*wc_card_factory_t)(lv_obj_t*, void*);
typedef void (*wc_game_fn_t)(void);

// v2.1: SD-mount status exposed to app code so SD-required apps can dim.
static bool g_sd_ok = false;
static uint8_t g_cur_brightness = 0;

// -------------------------------------------
enum WcSwipe : int8_t { WC_SWIPE_NONE = 0, WC_SWIPE_LEFT = -1, WC_SWIPE_RIGHT = 1, WC_SWIPE_UP = 2, WC_SWIPE_DOWN = 3 };

// ---- theme engine (10 built-ins) ------------------------------------------
struct WcTheme { const char *name; uint32_t primary, accent, bg, fg, warn, danger; const char *wallpaper; };
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
static const int kBuiltinThemesN = sizeof(kBuiltinThemes)/sizeof(kBuiltinThemes[0]);
static int g_theme_idx = 0;
static const WcTheme &wc_theme_active() { return kBuiltinThemes[g_theme_idx]; }
static uint32_t wc_theme_primary() { return wc_theme_active().primary; }
static uint32_t wc_theme_accent()  { return wc_theme_active().accent;  }
static uint32_t wc_theme_bg()      { return wc_theme_active().bg;      }
static uint32_t wc_theme_fg()      { return wc_theme_active().fg;      }
static uint32_t wc_theme_warn()    { return wc_theme_active().warn;    }
static uint32_t wc_theme_danger()  { return wc_theme_active().danger;  }
static int  wc_theme_count() { return kBuiltinThemesN; }
static const char *wc_theme_name_at(int i){ return (i>=0&&i<kBuiltinThemesN) ? kBuiltinThemes[i].name : ""; }
static void wc_theme_load() {
    Preferences p; p.begin(WC_NVS_THEME, true); g_theme_idx = p.getInt("idx", 0); p.end();
    if (g_theme_idx < 0 || g_theme_idx >= kBuiltinThemesN) g_theme_idx = 0;
}
static void wc_theme_set(int i) {
    if (i < 0 || i >= kBuiltinThemesN) return;
    g_theme_idx = i;
    Preferences p; p.begin(WC_NVS_THEME, false); p.putInt("idx", i); p.end();
}

// ---- wallpaper (RGB565 .bin from /WATCH/PHOTOS/) --------------------------
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
static int wc_wallpaper_count() { return g_photo_count; }
static int wc_wallpaper_active_index() { return g_photo_idx; }
static const char *wc_wallpaper_name_at(int i) { return (i>=0&&i<g_photo_count) ? g_photos[i].c_str() : ""; }

// ---- BOOT return -- partition-switch model --------------------------------
// v2.1: This board's partition layout is
//   factory  = LauncherOS  (permanent, never overwritten)
//   ota_0    = scratchpad slot (holds this sketch: GameOS)
// So "return to launcher" is a pure OTA-boot swap -- NOT an SD reflash.
// Ported verbatim from BookOS.ino / NoteOS.ino which use the same watch.
// This is what actually kills the v2.0 reboot loop: no more SD read, no
// more Update.begin/write/end -- the launcher partition is always there.
static inline bool wc_boot_pressed() { return digitalRead(WC_BOOT_BTN) == LOW; }
static volatile bool g_launcher_return_err = false;
static const char   *g_launcher_return_msg = "";
static void wc_launcher_return_now() {
    const esp_partition_t *factory = esp_partition_find_first(
        ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_FACTORY, NULL);
    if (factory) {
        esp_ota_set_boot_partition(factory);
    } else {
        // Fallback: no factory found (shouldn't happen with the shipped
        // partitions.csv) -- flip to the other OTA slot if any.
        const esp_partition_t *other = esp_ota_get_next_update_partition(NULL);
        if (other && other != esp_ota_get_running_partition()) {
            esp_ota_set_boot_partition(other);
        } else {
            g_launcher_return_msg = "no launcher partition found";
            g_launcher_return_err = true;
            return;
        }
    }
    delay(80);
    esp_restart();
}
static void wc_boot_button_poll() {
    static uint32_t hold = 0;
    uint32_t now = millis();
    if (wc_boot_pressed()) {
        if (!hold) hold = now;
        else if (now - hold > WC_BOOT_LONGPRESS_MS) { hold = now; wc_launcher_return_now(); }
    } else hold = 0;
}

// ---- battery pill ---------------------------------------------------------
static int  g_batt_pct = -1; static bool g_batt_ch = false; static int g_batt_mv = 0;
static void wc_battery_set_sample(int pct, bool ch, int mv) { g_batt_pct = pct; g_batt_ch = ch; g_batt_mv = mv; }
static int  wc_battery_pct() { return g_batt_pct; }
static void wc_battery_pill_tick(lv_timer_t *t) {
    lv_obj_t *lbl = (lv_obj_t*)t->user_data; if (!lbl) return;
    char b[16];
    if (g_batt_pct < 0) snprintf(b, sizeof(b), "-- %%");
    else                snprintf(b, sizeof(b), "%d%%%s", g_batt_pct, g_batt_ch ? "+" : "");
    lv_label_set_text(lbl, b);
    lv_color_t col = lv_color_hex(g_batt_pct < 15 ? wc_theme_danger() : g_batt_pct < 30 ? wc_theme_warn() : wc_theme_primary());
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

// ---- splash --------------------------------------------------------------
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
    if (g_wc_splash_scr) { lv_obj_del_async(g_wc_splash_scr); g_wc_splash_scr = nullptr; g_wc_splash_bar = nullptr; g_wc_splash_status = nullptr; }
}

// ---- settings sheet -------------------------------------------------------
enum WcCardKind { WC_CARD_TOGGLE, WC_CARD_SLIDER, WC_CARD_DROPDOWN, WC_CARD_ACTION, WC_CARD_SUBVIEW };

struct WcCard {
    WcCardKind kind; const char *title; const char *subtitle;
    bool toggle_on;
    int  slider_min, slider_max, slider_val;
    const char *dropdown_items; int dropdown_idx;
    wc_toggle_cb_t on_toggle; wc_slider_cb_t on_slider; wc_dropdown_cb_t on_dropdown;
    wc_action_cb_t on_action; wc_subview_build_t on_subview; void *user;
};
#define WC_MAX_CARDS 24
static WcCard g_cards[WC_MAX_CARDS]; static int g_card_count = 0;
static bool g_sheet_open = false;
static lv_obj_t *g_sheet_scr = nullptr, *g_sheet_list = nullptr, *g_sheet_subview = nullptr, *g_prev_scr = nullptr;
static void wc_settings_clear() { g_card_count = 0; }
static void wc_settings_add(const WcCard &c) { if (g_card_count < WC_MAX_CARDS) g_cards[g_card_count++] = c; }
static void wc_settings_pop_subview();
static void wc_settings_push_subview(const char *title, wc_subview_build_t build, void *user);
static void wc_settings_toggle_cb(lv_event_t *e) { lv_obj_t *sw = lv_event_get_target(e); WcCard *c = (WcCard*)lv_event_get_user_data(e); bool on = lv_obj_has_state(sw, LV_STATE_CHECKED); c->toggle_on = on; if (c->on_toggle) c->on_toggle(on, c->user); }
static void wc_settings_slider_cb(lv_event_t *e) { lv_obj_t *sl = lv_event_get_target(e); WcCard *c = (WcCard*)lv_event_get_user_data(e); int v = lv_slider_get_value(sl); c->slider_val = v; if (c->on_slider) c->on_slider(v, c->user); }
static void wc_settings_dd_cb(lv_event_t *e)     { lv_obj_t *dd = lv_event_get_target(e); WcCard *c = (WcCard*)lv_event_get_user_data(e); int idx = lv_dropdown_get_selected(dd); c->dropdown_idx = idx; if (c->on_dropdown) c->on_dropdown(idx, c->user); }
static void wc_settings_action_cb(lv_event_t *e) { WcCard *c = (WcCard*)lv_event_get_user_data(e); if (c->on_action) c->on_action(c->user); }
static void wc_settings_subview_cb(lv_event_t *e){ WcCard *c = (WcCard*)lv_event_get_user_data(e); wc_settings_push_subview(c->title, c->on_subview, c->user); }
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
    lv_obj_set_style_text_font(back, &lv_font_montserrat_20, 0);
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

// ---- infinite carousel ----------------------------------------------------
#define WC_CAR_MAX 12

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
// ===========================================================================
//  END wc_core
// ===========================================================================
// ---- GameOS on-demand loader + manifest -----------------------------------
#define WC_GAME_MAX          48
#define WC_GAME_HEAP_MIN_FREE (48*1024)
struct WcGameEntry {
    const char *id; const char *display_name; const char *category; uint32_t icon_color;
    wc_game_fn_t init; wc_game_fn_t loop; wc_game_fn_t exit_;
};
static WcGameEntry g_gl_reg[WC_GAME_MAX];
static bool        g_gl_enabled[WC_GAME_MAX];
static int         g_gl_count = 0;
static volatile bool g_gl_running = false;
static volatile bool g_gl_want_exit = false;
static void wc_game_register(const WcGameEntry &e) { if (g_gl_count >= WC_GAME_MAX) return; g_gl_reg[g_gl_count] = e; g_gl_enabled[g_gl_count] = true; g_gl_count++; }
static int  wc_game_count() { return g_gl_count; }
static const WcGameEntry *wc_game_at(int i) { return (i>=0&&i<g_gl_count) ? &g_gl_reg[i] : nullptr; }
static bool wc_game_enabled(int i) { return (i>=0&&i<g_gl_count) && g_gl_enabled[i]; }
static bool wc_game_is_running() { return g_gl_running; }
static void wc_game_request_exit() { g_gl_want_exit = true; }
static void wc_game_manifest_load() {
    for (int i = 0; i < g_gl_count; i++) g_gl_enabled[i] = true;
    // v2.1: skip manifest read entirely on SD-less boot (was #12 in RCA).
    if (!g_sd_ok) { Serial.println("[manifest] SD missing -- all games enabled by default"); return; }
    File f = SD_MMC.open(WC_PATH_GAMES_MFT, FILE_READ);
    if (!f) return;
    String s; s.reserve(f.size() + 1);
    while (f.available()) s += (char)f.read();
    f.close();
    int eids = s.indexOf("\"enabled_ids\"");
    if (eids < 0) return;
    int lb = s.indexOf('[', eids), rb = s.indexOf(']', lb);
    if (lb < 0 || rb < 0) return;
    String list = s.substring(lb + 1, rb);
    for (int i = 0; i < g_gl_count; i++) g_gl_enabled[i] = false;
    int p = 0;
    while (p < (int)list.length()) {
        int q1 = list.indexOf('"', p); if (q1 < 0) break;
        int q2 = list.indexOf('"', q1 + 1); if (q2 < 0) break;
        String id = list.substring(q1 + 1, q2);
        for (int i = 0; i < g_gl_count; i++) if (id == g_gl_reg[i].id) { g_gl_enabled[i] = true; break; }
        p = q2 + 1;
    }
}
static bool wc_game_launch(int idx) {
    // v2.1 bootloop-fix: additional guards beyond v2.0's heap check --
    // reject any launch attempt with null init/loop/exit_ pointers so a
    // half-populated registry can't crash the OS.
    if (g_gl_running || idx < 0 || idx >= g_gl_count || !g_gl_enabled[idx]) return false;
    const WcGameEntry *g = &g_gl_reg[idx];
    if (!g->init || !g->loop || !g->exit_) {
        Serial.printf("[wc_game] entry %d has null fn pointer(s) -- refusing launch\n", idx);
        return false;
    }
    size_t free_int = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    if (free_int < WC_GAME_HEAP_MIN_FREE) { Serial.printf("[wc_game] heap too low (%u) -- refusing launch\n", (unsigned)free_int); return false; }
    g_gl_running = true; g_gl_want_exit = false;
    #if defined(ESP_IDF_VERSION_MAJOR) && ESP_IDF_VERSION_MAJOR >= 5
    esp_task_wdt_config_t wdt_cfg = {
        .timeout_ms = 30000,
        .idle_core_mask = 0,
        .trigger_panic = true
    };
    esp_task_wdt_init(&wdt_cfg);
#else
    esp_task_wdt_init(30, true);
#endif
    esp_task_wdt_add(NULL);
    g->init();                                     // null-checked above
    while (!g_gl_want_exit) {
        // v2.1: try-guard shell -- if the game's screen was torn down or
        // a wild pointer nullified it, bail cleanly back to the grid.
        if (!lv_scr_act()) { Serial.println("[wc_game] active screen went NULL -- aborting"); g_gl_want_exit = true; break; }
        g->loop();
        lv_timer_handler(); esp_task_wdt_reset();
        static uint32_t hold = 0;
        if (digitalRead(WC_BOOT_BTN) == LOW) { if (!hold) hold = millis(); else if (millis()-hold > 800) g_gl_want_exit = true; }
        else hold = 0;
        delay(2);
    }
    g->exit_();
    esp_task_wdt_delete(NULL);
    g_gl_running = false;
    return true;
}
// ---- shared HW globals + swipe detector -----------------------------------
static Arduino_DataBus *g_bus =
    new Arduino_ESP32QSPI(WC_LCD_CS, WC_LCD_SCLK, WC_LCD_SDIO0, WC_LCD_SDIO1, WC_LCD_SDIO2, WC_LCD_SDIO3);
static Arduino_CO5300 *g_gfx = new Arduino_CO5300(
    g_bus, WC_LCD_RESET, 0, WC_LCD_WIDTH, WC_LCD_HEIGHT, 20, 0, 0, 0);
static std::shared_ptr<Arduino_IIC_DriveBus> g_iicbus =
    std::make_shared<Arduino_HWIIC>(WC_IIC_SDA, WC_IIC_SCL, &Wire);
void Arduino_IIC_Touch_Interrupt(void);
static std::unique_ptr<Arduino_IIC> g_ft(new Arduino_FT3x68(
    g_iicbus, FT3168_DEVICE_ADDRESS, WC_TP_RESET, WC_TP_INT, Arduino_IIC_Touch_Interrupt));
void Arduino_IIC_Touch_Interrupt(void) { g_ft->IIC_Interrupt_Flag = true; }
static XPowersAXP2101 g_pmu;
static bool g_has_pmu = false;
static lv_disp_draw_buf_t g_draw_buf;
static lv_color_t        *g_lvgl_buf = nullptr;
static lv_disp_drv_t      g_disp_drv;
static lv_indev_drv_t     g_indev_drv;
static int32_t  g_ts_x=0, g_ts_y=0, g_tc_x=0, g_tc_y=0;
static uint32_t g_ts_ms=0;
static volatile int8_t g_pending_swipe = WC_SWIPE_NONE;
static uint32_t g_last_swipe_ms = 0;

static void wc_flush(lv_disp_drv_t *drv, const lv_area_t *area, lv_color_t *px) {
    uint32_t w = area->x2 - area->x1 + 1, h = area->y2 - area->y1 + 1;
    g_gfx->draw16bitRGBBitmap(area->x1, area->y1, (uint16_t*)px, w, h);
    lv_disp_flush_ready(drv);
}
static void wc_touch(lv_indev_drv_t *drv, lv_indev_data_t *data) {
    static uint8_t rel_hyst = 0;
  int x = g_ft->IIC_Read_Device_Value(Arduino_IIC_Touch::Value_Information::TOUCH_COORDINATE_X);
int y = g_ft->IIC_Read_Device_Value(Arduino_IIC_Touch::Value_Information::TOUCH_COORDINATE_Y);
int fingers = (x > 0 && y > 0 && x < WC_LCD_WIDTH && y < WC_LCD_HEIGHT) ? 1 : 0;
    if (fingers > 0) { rel_hyst = 0; data->point.x = x; data->point.y = y; data->state = LV_INDEV_STATE_PR; g_tc_x = x; g_tc_y = y; }
    else { if (++rel_hyst >= WC_TP_RELEASE_HYST) data->state = LV_INDEV_STATE_REL; else data->state = LV_INDEV_STATE_PR; }
    static bool inDrag = false;
    if (data->state == LV_INDEV_STATE_PR) {
        if (!inDrag) { inDrag = true; g_ts_x = x; g_ts_y = y; g_ts_ms = millis(); }
    } else if (inDrag) {
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

// v2.1: anchor_launcher_as_boot() -- point OTADATA at the FACTORY
// (LauncherOS) partition so any watchdog / brownout / panic reset
// falls back to LauncherOS instead of re-launching GameOS in a loop.
// Correct for the partition-switch model: GameOS lives in ota_0,
// launcher lives in factory. Anchoring SELF (the earlier v2.1 draft)
// was wrong -- it would trap the user in GameOS across resets and
// re-trigger the very bootloop the RCA was trying to kill.
static void anchor_launcher_as_boot() {
    const esp_partition_t *factory = esp_partition_find_first(
        ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_FACTORY, NULL);
    if (!factory) { Serial.println("[boot] factory partition not found -- skip anchor"); return; }
    esp_err_t e = esp_ota_set_boot_partition(factory);
    Serial.printf("[boot] anchored bootloader to '%s' (err=%d)\n", factory->label, (int)e);
}

// v2.1: fade_brightness() -- LauncherOS's smooth 12-step ramp. Keeps
// LVGL pumping mid-fade so any splash animation stays fluid.
static void wc_fade_brightness(uint8_t target, uint32_t dur_ms) {
    int from = g_cur_brightness;
    int to   = target;
    if (from == to) return;
    const int steps = 12;
    int step_ms = (int)dur_ms / steps;
    if (step_ms < 1) step_ms = 1;
    for (int i = 1; i <= steps; i++) {
        int b = from + ((to - from) * i) / steps;
        if (b < 0) b = 0; if (b > 255) b = 255;
        g_gfx->setBrightness((uint8_t)b);
        g_cur_brightness = (uint8_t)b;
        lv_timer_handler();
        delay(step_ms);
    }
    g_gfx->setBrightness(target);
    g_cur_brightness = target;
}

// v2.1: LauncherOS-style I2C bus recovery -- clocks out any stuck
// transaction before Wire.begin().
static void wc_i2c_bus_recover() {
    pinMode(WC_IIC_SCL, OUTPUT);
    for (int i = 0; i < 9; i++) {
        digitalWrite(WC_IIC_SCL, LOW);  delayMicroseconds(5);
        digitalWrite(WC_IIC_SCL, HIGH); delayMicroseconds(5);
    }
}

// v2.1: Fix #5 in RCA -- SD mount with 3-retry loop + graceful degrade.
// Returns true if mounted; sets g_sd_ok globally. Never blocks boot.
static bool wc_sd_mount_with_retry() {
    SD_MMC.setPins(WC_SDMMC_CLK, WC_SDMMC_CMD, WC_SDMMC_DATA);
    for (int attempt = 1; attempt <= WC_SD_MOUNT_RETRIES; attempt++) {
        if (SD_MMC.begin("/sdcard", true)) {
            Serial.printf("[sd] mounted (attempt %d)\n", attempt);
            g_sd_ok = true;
            return true;
        }
        Serial.printf("[sd] mount fail (attempt %d/%d)\n", attempt, WC_SD_MOUNT_RETRIES);
        delay(120);
    }
    g_sd_ok = false;
    Serial.println("[sd] no card -- running in SD-less mode (empty games list)");
    return false;
}

// v2.1: LauncherOS-mirrored boot chain. The exact order in the RCA:
//   serial -> anchor -> nvs (caller) -> btStop -> gfx dark -> i2c ->
//   touch -> lv_init -> heap_caps_malloc INTERNAL(fallback SPIRAM) ->
//   register drivers -> (splash after registration!) -> SD retry.
static void wc_hw_boot() {
    Serial.begin(115200);
    anchor_launcher_as_boot();                        // Fix #3 (partition-switch model)
    btStop();
    pinMode(WC_BOOT_BTN, INPUT_PULLUP);
    pinMode(WC_PWR_BTN_PIN, INPUT_PULLUP);
    if (!g_gfx->begin()) Serial.println("[gfx] begin FAILED");
    g_gfx->fillScreen(RGB565_BLACK);   // preferred, matches the lib                         // render dark first
    g_gfx->setBrightness(0);                     // then fade up later
    g_cur_brightness = 0;
    wc_i2c_bus_recover();
    Wire.begin(WC_IIC_SDA, WC_IIC_SCL); Wire.setClock(400000);
    if (g_pmu.begin(Wire, AXP2101_SLAVE_ADDRESS, WC_IIC_SDA, WC_IIC_SCL)) {
        g_has_pmu = true;
        g_pmu.setALDO1Voltage(3300); g_pmu.enableALDO1();
        g_pmu.setBLDO1Voltage(3300); g_pmu.enableBLDO1();
        delay(40);
    }
    pinMode(WC_TP_RESET, OUTPUT);
    digitalWrite(WC_TP_RESET, LOW);  delay(50);
    digitalWrite(WC_TP_RESET, HIGH); delay(200);
    pinMode(WC_TP_INT, INPUT_PULLUP);
    for (int i = 0; i < 20 && !g_ft->begin(); i++) { Serial.println("[touch] FT3168 init fail"); delay(80); }
    // Fix #1: LVGL draw buffer OOM on internal RAM caused Guru Meditation.
    // Try INTERNAL first, fall back to PSRAM (SPIRAM), then last-resort
    // 8BIT. lv_init() is fine even if internal alloc fails because we
    // catch it here and provide a PSRAM buffer before lv_disp_drv_init.
    lv_init();
    size_t px = WC_LCD_WIDTH * 60;
    g_lvgl_buf = (lv_color_t*)heap_caps_malloc(px * sizeof(lv_color_t),
                                               MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!g_lvgl_buf) {
        Serial.println("[lvgl] internal alloc failed -- falling back to PSRAM");
        g_lvgl_buf = (lv_color_t*)heap_caps_malloc(px * sizeof(lv_color_t), MALLOC_CAP_SPIRAM);
    }
    if (!g_lvgl_buf) {
        Serial.println("[lvgl] PSRAM alloc failed -- last resort MALLOC_CAP_8BIT");
        g_lvgl_buf = (lv_color_t*)heap_caps_malloc(px * sizeof(lv_color_t), MALLOC_CAP_8BIT);
    }
    lv_disp_draw_buf_init(&g_draw_buf, g_lvgl_buf, NULL, px);
    // Fix #4 & #10: register drivers BEFORE any lv_obj_create -- was
    // null-ptr deref cause in v2.0 splash. Splash is now built by the
    // caller AFTER wc_hw_boot() returns, so this ordering is correct.
    lv_disp_drv_init(&g_disp_drv);
    g_disp_drv.flush_cb = wc_flush; g_disp_drv.draw_buf = &g_draw_buf;
    g_disp_drv.hor_res = WC_LCD_WIDTH; g_disp_drv.ver_res = WC_LCD_HEIGHT;
    lv_disp_drv_register(&g_disp_drv);
    lv_indev_drv_init(&g_indev_drv);
    g_indev_drv.type = LV_INDEV_TYPE_POINTER;
    g_indev_drv.read_cb = wc_touch;
    lv_indev_drv_register(&g_indev_drv);
    // Fix #5: SD mount retry (3x) with safe SD-less fallback.
    wc_sd_mount_with_retry();
}
static void wc_pmu_refresh() {
    if (!g_has_pmu) return;
    wc_battery_set_sample(g_pmu.getBatteryPercent(), g_pmu.isCharging(), g_pmu.getBattVoltage());
}
// ===========================================================================
//  BEGIN GameOS-specific code
// ===========================================================================

// Forward-declare the 3 demo games so we can register them BEFORE they're
// defined further down (Arduino IDE hoists top-level function prototypes).
static void pong_init();  static void pong_loop();  static void pong_exit();
static void snake_init(); static void snake_loop(); static void snake_exit();
static void flap_init();  static void flap_loop();  static void flap_exit();

static void register_games() {
    WcGameEntry e;
    e = { "pong",   "Pong",   "Arcade", 0x9BD16A, pong_init,  pong_loop,  pong_exit  }; wc_game_register(e);
    e = { "snake",  "Snake",  "Retro",  0x39FF14, snake_init, snake_loop, snake_exit }; wc_game_register(e);
    e = { "flappy", "Flappy", "Arcade", 0xFFB300, flap_init,  flap_loop,  flap_exit  }; wc_game_register(e);
    // (Add more games: define _init/_loop/_exit above, register them here.)
}

// ---- Pong ------------------------------------------------------------------
static lv_obj_t *g_pong_scr, *g_pong_ball, *g_pong_pad, *g_pong_score_lbl;
static float g_pong_bx, g_pong_by, g_pong_vx, g_pong_vy;
static int   g_pong_score;
static void pong_init() {
    g_pong_scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(g_pong_scr, lv_color_hex(0x0b140a), 0);
    lv_scr_load(g_pong_scr);
    g_pong_ball = lv_obj_create(g_pong_scr);
    lv_obj_set_size(g_pong_ball, 18, 18);
    lv_obj_set_style_bg_color(g_pong_ball, lv_color_hex(wc_theme_accent()), 0);
    lv_obj_set_style_radius(g_pong_ball, 9, 0);
    g_pong_pad = lv_obj_create(g_pong_scr);
    lv_obj_set_size(g_pong_pad, 80, 12);
    lv_obj_set_style_bg_color(g_pong_pad, lv_color_hex(wc_theme_primary()), 0);
    lv_obj_align(g_pong_pad, LV_ALIGN_BOTTOM_MID, 0, -20);
    g_pong_score_lbl = lv_label_create(g_pong_scr);
    lv_label_set_text(g_pong_score_lbl, "0");
    lv_obj_set_style_text_color(g_pong_score_lbl, lv_color_hex(wc_theme_primary()), 0);
    lv_obj_set_style_text_font(g_pong_score_lbl, &lv_font_montserrat_32, 0);
    lv_obj_align(g_pong_score_lbl, LV_ALIGN_TOP_MID, 0, 12);
    g_pong_bx = 200; g_pong_by = 100; g_pong_vx = 2.4f; g_pong_vy = 3.1f; g_pong_score = 0;
}
static void pong_loop() {
    g_pong_bx += g_pong_vx; g_pong_by += g_pong_vy;
    if (g_pong_bx < 9 || g_pong_bx > WC_LCD_WIDTH-9) g_pong_vx = -g_pong_vx;
    if (g_pong_by < 9) g_pong_vy = -g_pong_vy;
    int px_ = g_tc_x - 40;
    if (px_ < 0) px_ = 0; if (px_ > WC_LCD_WIDTH-80) px_ = WC_LCD_WIDTH-80;
    lv_obj_set_x(g_pong_pad, px_);
    int py_ = WC_LCD_HEIGHT - 32;
    if (g_pong_by > py_-9 && g_pong_bx > px_ && g_pong_bx < px_+80) {
        g_pong_vy = -fabs(g_pong_vy); g_pong_score++;
        char b[8]; snprintf(b,8,"%d",g_pong_score); lv_label_set_text(g_pong_score_lbl,b);
    }
    if (g_pong_by > WC_LCD_HEIGHT) { wc_game_request_exit(); return; }
    lv_obj_set_pos(g_pong_ball, (int)g_pong_bx-9, (int)g_pong_by-9);
}
static void pong_exit() { if (g_pong_scr) { lv_obj_del(g_pong_scr); g_pong_scr = nullptr; } }

// ---- Snake -----------------------------------------------------------------
static lv_obj_t *g_snk_scr, *g_snk_lbl;
static int g_snk_ex, g_snk_ey, g_snk_dx, g_snk_dy, g_snk_len;
static int g_snk_bx[64], g_snk_by[64];
static uint32_t g_snk_last;
static void snake_init() {
    g_snk_scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(g_snk_scr, lv_color_hex(0x040A04), 0);
    lv_scr_load(g_snk_scr);
    g_snk_lbl = lv_label_create(g_snk_scr);
    lv_label_set_text(g_snk_lbl, "SNAKE\nswipe to steer");
    lv_obj_set_style_text_color(g_snk_lbl, lv_color_hex(0x9EFF6E), 0);
    lv_obj_set_style_text_font(g_snk_lbl, &lv_font_montserrat_18, 0);
    lv_obj_align(g_snk_lbl, LV_ALIGN_TOP_MID, 0, 16);
    g_snk_len = 3; g_snk_dx = 1; g_snk_dy = 0; g_snk_ex = 10; g_snk_ey = 10;
    for (int i = 0; i < g_snk_len; i++) { g_snk_bx[i] = 5-i; g_snk_by[i] = 5; }
    g_snk_last = 0;
}
static void snake_loop() {
    // Steer with swipe: peek g_pending_swipe (we don't consume it since the
    // OS-level loop already grabs it; instead we sample direction from
    // g_ts_x / g_tc_x diff).
    int dx = g_tc_x - g_ts_x, dy = g_tc_y - g_ts_y;
    if (abs(dx) > abs(dy)) { g_snk_dx = dx > 0 ? 1 : -1; g_snk_dy = 0; }
    else if (dy != 0)      { g_snk_dy = dy > 0 ? 1 : -1; g_snk_dx = 0; }
    if (millis() - g_snk_last < 220) return;
    g_snk_last = millis();
    for (int i = g_snk_len-1; i > 0; i--) { g_snk_bx[i]=g_snk_bx[i-1]; g_snk_by[i]=g_snk_by[i-1]; }
    g_snk_bx[0]+=g_snk_dx; g_snk_by[0]+=g_snk_dy;
    if (g_snk_bx[0]<0||g_snk_by[0]<0||g_snk_bx[0]>=25||g_snk_by[0]>=30) { wc_game_request_exit(); return; }
    if (g_snk_bx[0]==g_snk_ex && g_snk_by[0]==g_snk_ey && g_snk_len < 64) { g_snk_len++; g_snk_ex = random(25); g_snk_ey = random(30); }
    lv_obj_clean(g_snk_scr);
    for (int i = 0; i < g_snk_len; i++) {
        lv_obj_t *c = lv_obj_create(g_snk_scr);
        lv_obj_set_size(c, 14, 14);
        lv_obj_set_style_bg_color(c, lv_color_hex(0x39FF14), 0);
        lv_obj_set_pos(c, g_snk_bx[i]*16, 60 + g_snk_by[i]*14);
    }
    lv_obj_t *e = lv_obj_create(g_snk_scr);
    lv_obj_set_size(e, 14, 14);
    lv_obj_set_style_bg_color(e, lv_color_hex(0xE81E1E), 0);
    lv_obj_set_pos(e, g_snk_ex*16, 60 + g_snk_ey*14);
}
static void snake_exit() { if (g_snk_scr) { lv_obj_del(g_snk_scr); g_snk_scr = nullptr; } }

// ---- Flappy ----------------------------------------------------------------
static lv_obj_t *g_fl_scr, *g_fl_bird, *g_fl_pipe, *g_fl_score;
static float g_fl_by, g_fl_vy;
static int   g_fl_px, g_fl_gap_y, g_fl_sc;
static void flap_init() {
    g_fl_scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(g_fl_scr, lv_color_hex(0x081018), 0);
    lv_scr_load(g_fl_scr);
    g_fl_bird = lv_obj_create(g_fl_scr);
    lv_obj_set_size(g_fl_bird, 22, 22);
    lv_obj_set_style_radius(g_fl_bird, 11, 0);
    lv_obj_set_style_bg_color(g_fl_bird, lv_color_hex(0xFFB300), 0);
    g_fl_pipe = lv_obj_create(g_fl_scr);
    lv_obj_set_size(g_fl_pipe, 40, WC_LCD_HEIGHT);
    lv_obj_set_style_bg_color(g_fl_pipe, lv_color_hex(0x66C0FF), 0);
    g_fl_score = lv_label_create(g_fl_scr);
    lv_label_set_text(g_fl_score, "0");
    lv_obj_set_style_text_color(g_fl_score, lv_color_hex(0xEAF6FF), 0);
    lv_obj_set_style_text_font(g_fl_score, &lv_font_montserrat_28, 0);
    lv_obj_align(g_fl_score, LV_ALIGN_TOP_MID, 0, 12);
    g_fl_by = 200; g_fl_vy = 0; g_fl_px = WC_LCD_WIDTH; g_fl_gap_y = 200; g_fl_sc = 0;
}
static void flap_loop() {
    g_fl_vy += 0.25f; g_fl_by += g_fl_vy; g_fl_px -= 3;
    if (g_fl_px < -40) {
        g_fl_px = WC_LCD_WIDTH; g_fl_gap_y = 80 + random(300); g_fl_sc++;
        char b[8]; snprintf(b,8,"%d",g_fl_sc); lv_label_set_text(g_fl_score,b);
    }
    static uint32_t last = 0;
    if (millis() - last > 200 && g_tc_y > 0 && g_tc_y < WC_LCD_HEIGHT/2) { g_fl_vy = -5.5f; last = millis(); }
    if (g_fl_by > WC_LCD_HEIGHT || g_fl_by < 0) { wc_game_request_exit(); return; }
    lv_obj_set_pos(g_fl_bird, 80, (int)g_fl_by);
    lv_obj_set_pos(g_fl_pipe, g_fl_px, 0);
}
static void flap_exit() { if (g_fl_scr) { lv_obj_del(g_fl_scr); g_fl_scr = nullptr; } }

// ---- grid ------------------------------------------------------------------
static lv_obj_t *g_grid_scr = nullptr;
static void tap_game(lv_event_t *e) {
    if (millis() - g_last_swipe_ms < WC_CLICK_AFTER_SWIPE_MS) return;
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    if (wc_game_launch(idx)) { extern void build_grid(); build_grid(); }
}
void build_grid() {
    if (g_grid_scr) { lv_obj_del(g_grid_scr); g_grid_scr = nullptr; }
    lv_obj_t *s = lv_obj_create(NULL);
    lv_obj_clear_flag(s, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(s, lv_color_hex(wc_theme_bg()), 0);
    g_grid_scr = s;

    lv_obj_t *hdr = lv_label_create(s);
    char hbuf[32]; snprintf(hbuf, sizeof(hbuf), "GAME//OS  %d titles", wc_game_count());
    lv_label_set_text(hdr, hbuf);
    lv_obj_set_style_text_color(hdr, lv_color_hex(wc_theme_primary()), 0);
    lv_obj_set_style_text_font(hdr, &lv_font_montserrat_20, 0);
    lv_obj_align(hdr, LV_ALIGN_TOP_LEFT, 20, 16);

    lv_obj_t *grid = lv_obj_create(s);
    lv_obj_set_size(grid, WC_LCD_WIDTH - 20, WC_LCD_HEIGHT - 70);
    lv_obj_align(grid, LV_ALIGN_TOP_MID, 0, 60);
    lv_obj_set_style_bg_opa(grid, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(grid, 0, 0);
    lv_obj_set_flex_flow(grid, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_gap(grid, 10, 0);
    lv_obj_set_style_pad_all(grid, 8, 0);

    for (int i = 0; i < wc_game_count(); i++) {
        if (!wc_game_enabled(i)) continue;
        const WcGameEntry *g = wc_game_at(i);
        lv_obj_t *tile = lv_btn_create(grid);
        lv_obj_set_size(tile, 110, 110);
        lv_obj_set_style_bg_color(tile, lv_color_hex(g->icon_color), 0);
        lv_obj_set_style_bg_opa(tile, LV_OPA_70, 0);
        lv_obj_set_style_border_color(tile, lv_color_hex(wc_theme_accent()), 0);
        lv_obj_set_style_border_width(tile, 2, 0);
        lv_obj_set_style_radius(tile, 12, 0);
        lv_obj_t *lbl = lv_label_create(tile);
        lv_label_set_text(lbl, g->display_name);
        lv_obj_set_style_text_color(lbl, lv_color_hex(wc_theme_primary()), 0);
        lv_obj_set_style_text_font(lbl, &lv_font_montserrat_14, 0);
        lv_obj_center(lbl);
        lv_obj_add_event_cb(tile, tap_game, LV_EVENT_CLICKED, (void*)(intptr_t)i);
    }
    lv_scr_load_anim(s, LV_SCR_LOAD_ANIM_FADE_ON, WC_SCREEN_ANIM_MS, 0, false);
}

// v2.1: modal error card shown when launcheros.bin is missing/empty.
// This is the fix for issue #2 in the RCA -- kills the infinite restart
// loop by refusing to esp_restart() unless the file is valid.
static lv_obj_t *g_ret_err_scr = nullptr;
static void show_launcher_return_error() {
    if (g_ret_err_scr) return;
    g_ret_err_scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(g_ret_err_scr, lv_color_hex(wc_theme_bg()), 0);
    lv_obj_clear_flag(g_ret_err_scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *t = lv_label_create(g_ret_err_scr);
    lv_label_set_text(t, "LAUNCHER RETURN FAILED");
    lv_obj_set_style_text_color(t, lv_color_hex(wc_theme_danger()), 0);
    lv_obj_set_style_text_font(t, &lv_font_montserrat_20, 0);
    lv_obj_align(t, LV_ALIGN_CENTER, 0, -50);
    lv_obj_t *m = lv_label_create(g_ret_err_scr);
    lv_label_set_text(m, g_launcher_return_msg && g_launcher_return_msg[0] ? g_launcher_return_msg : "reason unknown");
    lv_obj_set_style_text_color(m, lv_color_hex(wc_theme_fg()), 0);
    lv_obj_set_style_text_font(m, &lv_font_montserrat_14, 0);
    lv_obj_align(m, LV_ALIGN_CENTER, 0, -10);
    lv_obj_t *tip = lv_label_create(g_ret_err_scr);
    lv_label_set_text(tip, "swipe DOWN to dismiss --\ncopy launcheros.bin to /WATCH/bin/");
    lv_obj_set_style_text_color(tip, lv_color_hex(wc_theme_accent()), 0);
    lv_obj_set_style_text_font(tip, &lv_font_montserrat_14, 0);
    lv_obj_align(tip, LV_ALIGN_CENTER, 0, 40);
    lv_scr_load_anim(g_ret_err_scr, LV_SCR_LOAD_ANIM_FADE_ON, WC_SCREEN_ANIM_MS, 0, false);
}
static void dismiss_launcher_return_error() {
    if (!g_ret_err_scr) return;
    lv_obj_del(g_ret_err_scr); g_ret_err_scr = nullptr;
    build_grid();
}

// v2.1: helper used by tap_game() when a launched game requests exit.
// Rebuilds the grid so the user lands somewhere sane instead of a
// blank screen (was another vector for the "app looks frozen" complaint).
static void back_to_grid() { build_grid(); }

void setup() {
    // v2.1 bootloop-fix: LauncherOS-mirrored boot chain. Each step is a
    // numbered fix from the RCA in the spec.
    //   1  USBSerial.begin + wake-cause read  (below)
    //   2  anchor_launcher_as_boot            (inside wc_hw_boot)
    //   3  theme_load_from_nvs                (below)
    //   4  time_restore_from_nvs              (skipped -- GameOS has no clock)
    //   5  btStop + pinModes                  (inside wc_hw_boot)
    //   6  gfx->begin -> BLACK -> brightness=0 (inside wc_hw_boot)
    //   7  Wire.begin fast + i2c recover      (inside wc_hw_boot)
    //   8  Touch reset + FT3168               (inside wc_hw_boot)
    //   9  lv_init + INTERNAL->SPIRAM buffer  (inside wc_hw_boot)  <-- FIX #1
    //  10  register disp + indev drv          (inside wc_hw_boot)  <-- FIX #4
    //  11  build_splash AFTER LVGL registered (below)              <-- FIX #4
    //  12  SD 3-retry, safe SD-less fallback  (inside wc_hw_boot)  <-- FIX #5
    //  13  register_games + manifest guarded  (below)
    //  14  build_grid + lv_scr_load_anim FADE
    //  15  fade_brightness up
    //  16  setCpuFrequencyMhz(80) if POWER_SAVE
    esp_sleep_get_wakeup_cause();
    wc_hw_boot();
    wc_theme_load();                                 // step 3
    if (g_sd_ok) wc_wallpaper_scan();

    wc_splash_show("GameOS", "v2.1 // bootloop-fix");
    lv_timer_handler();                              // paint first frame
    wc_fade_brightness(WC_DEFAULT_BRIGHTNESS, 260);  // step 15 (partial: for splash)

    register_games();                                // step 13
    wc_game_manifest_load();                         // step 13 (SD-guarded)
    for (int p = 0; p <= 100; p += 20) {
        const char *msg = "ready";
        if (p == 20) msg = g_sd_ok ? "sd ok" : "no sd -- default titles";
        if (p == 60) msg = "loading manifest";
        wc_splash_step(p, msg);
        delay(20);
    }
    wc_splash_dismiss();

    build_grid();                                    // step 14
    Serial.printf("[GameOS] v2.1 ready  games=%d  sd=%s\n", wc_game_count(), g_sd_ok ? "ok" : "MISSING");

#if defined(POWER_SAVE) && POWER_SAVE
    setCpuFrequencyMhz(80);                          // step 16
#endif
}
void loop() {
    lv_timer_handler();
    wc_pmu_refresh();
    wc_boot_button_poll();
    // v2.1 bootloop-fix: handle the launcher-return error out here so we
    // never reboot from inside a button poll while LVGL is mid-frame.
    if (g_launcher_return_err) { g_launcher_return_err = false; show_launcher_return_error(); }
    if (g_pending_swipe != WC_SWIPE_NONE) {
        int8_t ev = g_pending_swipe; g_pending_swipe = WC_SWIPE_NONE;
        g_last_swipe_ms = millis();
        if (g_ret_err_scr && (ev == WC_SWIPE_DOWN || ev == WC_SWIPE_RIGHT)) {
            dismiss_launcher_return_error();
        }
    }
    delay(3);
}
