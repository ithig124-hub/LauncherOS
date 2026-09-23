// =============================================================================
// VoyagerOS v2.1 -- Voyager-only software on the proven GameOS board runtime
// 15-second touch inactivity timeout; AXP2101 short power press toggles screen.
// Display-off pauses play; wake then tap RESUME. CPU stays on (no deep sleep).
// LVGL 8.4: enable Montserrat fonts used below; fixed memory pool >=128 KiB.
// Target : Waveshare ESP32-S3 Touch AMOLED 2.06 (410 x 502), LVGL 8.4
// SINGLE-FILE Arduino sketch.
// =============================================================================
#include <Arduino.h>
#include <FS.h>
#include <SD_MMC.h>
#include <Wire.h>
#include <Preferences.h>
#include <memory>
#include <Update.h>
#include <SensorPCF85063.hpp>
#include <SensorQMI8658.hpp>
#include <ESP_I2S.h>
#include <time.h>

#include "Arduino_GFX_Library.h"
#include "Arduino_DriveBus_Library.h"
#include "lv_conf.h"
#include "lvgl.h"

#define XPOWERS_CHIP_AXP2101
#include "XPowersLib.h"

#include <math.h>
#include <esp_system.h>
#include <esp_sleep.h>
#include <esp_heap_caps.h>
#include <esp_task_wdt.h>
#include <esp_ota_ops.h>
#include <esp_partition.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

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
#define WC_CLICK_AFTER_SWIPE_MS  250
#define WC_BOOT_LONGPRESS_MS    1500
#define WC_DEFAULT_BRIGHTNESS    140
#define WC_TP_RELEASE_HYST         3
#define WC_SD_MOUNT_RETRIES        3
#define WC_PATH_BIN        "/WATCH/bin"
#define WC_PATH_PHOTOS     "/WATCH/PHOTOS"
#define WC_PATH_THEMES     "/WATCH/THEMES"
#define WC_PATH_LAUNCHER   "/WATCH/bin/launcheros.bin"
#define WC_PATH_GAMES_MFT  "/GAMES/manifest.json"
#define WC_NVS_LAUNCHER    "dcw"
#define WC_NVS_THEME       "wctheme"

#ifndef POWER_SAVE
#define POWER_SAVE 1
#endif
static void wc_boot_button_poll();
static void wc_launcher_return_now();
static bool wc_boot_pressed();   // add this too if you also get "wc_boot_pressed not declared"
static void wc_boot_guard_success();   // fwd-decl: clear 3-strike fail counter (defined below)

struct WcCard; struct WcCarSpec; struct WcCar; struct WcGameEntry;
struct WcTheme; struct DraftCfg;
static void wc_service();
static void wc_input_reset();
static void wc_game_home_button();
static void wc_park_screen();
static bool wc_take_tap();
static uint32_t g_play_ms=0;
static bool g_screen_off=false, g_paused=false;
static uint32_t g_last_touch_ms=0;
static bool g_boot_require_release=false;
typedef void (*wc_toggle_cb_t)(bool, void*);
typedef void (*wc_slider_cb_t)(int, void*);
typedef void (*wc_dropdown_cb_t)(int, void*);
typedef void (*wc_action_cb_t)(void*);
typedef void (*wc_subview_build_t)(lv_obj_t*, void*);
typedef lv_obj_t* (*wc_card_factory_t)(lv_obj_t*, void*);
typedef void (*wc_game_fn_t)(void);

// Voyager is registered as an application inside the proven GameOS runtime.
static void voyager_app_init();
static void voyager_app_loop();
static void voyager_app_exit();

static bool g_sd_ok = false;
static uint8_t g_cur_brightness = 0;

enum WcSwipe : int8_t { WC_SWIPE_NONE = 0, WC_SWIPE_LEFT = -1, WC_SWIPE_RIGHT = 1, WC_SWIPE_UP = 2, WC_SWIPE_DOWN = 3 };

// ---- theme engine ---------------------------------------------------------
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
static const char *wc_theme_name_at(int i){ return (i>=0 && i<kBuiltinThemesN) ? kBuiltinThemes[i].name : ""; }
static void wc_theme_load() {
    Preferences p; p.begin(WC_NVS_THEME, true);
    g_theme_idx = p.getInt("idx", 0); p.end();
    if (g_theme_idx < 0 || g_theme_idx >= kBuiltinThemesN) g_theme_idx = 0;
}
static void wc_theme_set(int i) {
    if (i < 0 || i >= kBuiltinThemesN) return;
    g_theme_idx = i;
    Preferences p; p.begin(WC_NVS_THEME, false); p.putInt("idx", i); p.end();
}

// ---- wallpaper ------------------------------------------------------------
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
static const char *wc_wallpaper_name_at(int i) { return (i>=0 && i<g_photo_count) ? g_photos[i].c_str() : ""; }

// ---- boot return (issue #2 fix: no reboot loop when bin missing) ----------
static volatile bool g_launcher_return_err = false;
static const char   *g_launcher_return_msg = "";
static inline bool wc_boot_pressed() { return digitalRead(WC_BOOT_BTN) == LOW; }
static void wc_launcher_return_now() {
    // Boot back into the launcher/factory app partition (NoteOS-style, no SD needed).
    // Boot-guard: clear the 3-strike fail counter FIRST so an intentional launcher
    // return is never miscounted as a crash / boot-loop on the next power-cycle.
    wc_boot_guard_success();
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

// ---- v2.0 PORTED (BookOS): BOOT-button long-press -> launcher return -------
//  Fires WHILE HELD at WC_BOOT_LONGPRESS_MS (1500ms); no release needed.
//  Only runs from the home grid: the in-game loop owns the button (800ms exit)
//  and returns to loop() before this poll runs again, so the two never fight.
//  `fired` latch guards against a double-trigger if esp_restart() is delayed.
static void wc_boot_button_poll() {
    static uint32_t hold = 0;
    static bool     fired = false;
    uint32_t now = millis();
    if(g_boot_require_release){
        hold=0;fired=false;if(!wc_boot_pressed())g_boot_require_release=false;return;
    }
    if (wc_boot_pressed()) {
        if (!hold) hold = now;
        else if (!fired && now - hold > WC_BOOT_LONGPRESS_MS) { fired = true; wc_launcher_return_now(); }
    } else { hold = 0; fired = false; }
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

// ---- GameOS loader + registry ---------------------------------------------
#define WC_GAME_MAX 48
#define WC_GAME_HEAP_MIN_FREE (48*1024)
struct WcGameEntry {
    const char *id; const char *display_name; const char *category; uint32_t icon_color;
    wc_game_fn_t init; wc_game_fn_t loop; wc_game_fn_t exit_;
};
static WcGameEntry g_gl_reg[WC_GAME_MAX];
static bool g_gl_enabled[WC_GAME_MAX];
static int  g_gl_count = 0;
static volatile bool g_gl_running = false;
static volatile bool g_gl_want_exit = false;
static void wc_game_register(const WcGameEntry &e) { if (g_gl_count >= WC_GAME_MAX) return; g_gl_reg[g_gl_count] = e; g_gl_enabled[g_gl_count] = true; g_gl_count++; }
static int  wc_game_count() { return g_gl_count; }
static const WcGameEntry *wc_game_at(int i) { return (i>=0 && i<g_gl_count) ? &g_gl_reg[i] : nullptr; }
static bool wc_game_enabled(int i) { return (i>=0 && i<g_gl_count) ? g_gl_enabled[i] : false; }
static void wc_game_request_exit() { g_gl_want_exit = true; }

// ---- high scores + game-over/retry (v2.2) ----
static Preferences g_hsprefs;
static int  wc_hs_get(const char *id) { g_hsprefs.begin("ghs", true); int v = g_hsprefs.getInt(id, 0); g_hsprefs.end(); return v; }
static void wc_hs_put(const char *id, int v) { g_hsprefs.begin("ghs", false); g_hsprefs.putInt(id, v); g_hsprefs.end(); }
static bool g_game_died = false;
static char g_die_id[16] = {0};
static int  g_die_score  = 0;
static void game_over(const char *id, int score) {
    strncpy(g_die_id, id, sizeof(g_die_id)-1); g_die_id[sizeof(g_die_id)-1] = 0;
    g_die_score = score; g_game_died = true; g_gl_want_exit = true;
}
static bool show_game_over(const char *id, int score, int best, bool is_best);  // defined below
static bool wc_game_launch(int idx) {
    if (idx < 0 || idx >= g_gl_count || !g_gl_enabled[idx]) return false;
    const WcGameEntry *g = &g_gl_reg[idx];
    if (!g->init || !g->loop || !g->exit_) {
        Serial.printf("[wc_game] entry %d has null fn pointer(s) -- refusing launch\n", idx);
        return false;
    }
    size_t free_int = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    if (free_int < WC_GAME_HEAP_MIN_FREE) { Serial.printf("[wc_game] heap too low (%u) -- refusing launch\n", (unsigned)free_int); return false; }
    g_gl_running = true;
#if defined(ESP_IDF_VERSION_MAJOR) && ESP_IDF_VERSION_MAJOR >= 5
    esp_task_wdt_config_t wdt_cfg = { .timeout_ms = 30000, .idle_core_mask = 0, .trigger_panic = true };
    esp_task_wdt_init(&wdt_cfg);
#else
    esp_task_wdt_init(30, true);
#endif
    esp_task_wdt_add(NULL);
    bool replay = true;
    while (replay) {
        replay = false;
        g_game_died = false; g_gl_want_exit = false;
        g_play_ms=0; g_paused=false; wc_input_reset();
        g->init();
        lv_obj_set_style_pad_all(lv_scr_act(),0,0);
        lv_obj_set_style_border_width(lv_scr_act(),0,0);
        lv_obj_clear_flag(lv_scr_act(),LV_OBJ_FLAG_SCROLLABLE);
        wc_game_home_button();
        uint32_t frame=millis(),play_tick=frame;
        uint32_t hold = 0;
        while (!g_gl_want_exit) {
            if (!lv_scr_act()) { Serial.println("[wc_game] active screen went NULL -- aborting"); g_gl_want_exit = true; break; }
            bool was_playing=!g_screen_off && !g_paused;
            wc_service(); esp_task_wdt_reset();
            uint32_t now=millis();
            if(was_playing && !g_screen_off && !g_paused)g_play_ms+=now-play_tick;
            play_tick=now;
            if(g_screen_off || g_paused) frame=now;
            else if(!g_gl_want_exit && now-frame>=16U){
                frame=now; g->loop();
            }
            if (digitalRead(WC_BOOT_BTN) == LOW) { if (!hold) hold = millis(); else if (millis()-hold > 800) g_gl_want_exit = true; }
            else hold = 0;
            delay(2);
        }
        wc_park_screen(); g->exit_();
        if (g_game_died) {
            int best = wc_hs_get(g_die_id);
            bool is_best = g_die_score > best;
            if (is_best) { best = g_die_score; wc_hs_put(g_die_id, best); }
            replay = show_game_over(g_die_id, g_die_score, best, is_best);
        }
    }
    esp_task_wdt_delete(NULL);
    g_gl_running = false;
    g_boot_require_release=wc_boot_pressed();
    g_paused=false; wc_input_reset();
    return true;
}
static void wc_game_manifest_load() {
    if (!g_sd_ok) return;
    File f = SD_MMC.open(WC_PATH_GAMES_MFT, FILE_READ);
    if (!f) return;                     // no manifest -> keep all built-ins enabled
    while (f.available()) {
        String line = f.readStringUntil('\n'); line.trim();
        if (!line.length() || line[0] != '-') continue;    // "-id" disables a game
        String id = line.substring(1); id.trim();
        for (int i = 0; i < g_gl_count; i++)
            if (id == g_gl_reg[i].id) g_gl_enabled[i] = false;
    }
    f.close();
}

// ---- shared HW globals + swipe detector -----------------------------------
static Arduino_DataBus *g_bus =
    new Arduino_ESP32QSPI(WC_LCD_CS, WC_LCD_SCLK, WC_LCD_SDIO0, WC_LCD_SDIO1, WC_LCD_SDIO2, WC_LCD_SDIO3);
static Arduino_CO5300 *g_gfx = new Arduino_CO5300(
    g_bus, WC_LCD_RESET, 0, WC_LCD_WIDTH, WC_LCD_HEIGHT, 22, 0, 0, 0);      // FIX 20->22
static std::shared_ptr<Arduino_IIC_DriveBus> g_iicbus =
    std::make_shared<Arduino_HWIIC>(WC_IIC_SDA, WC_IIC_SCL, &Wire);
void Arduino_IIC_Touch_Interrupt(void);
static std::unique_ptr<Arduino_IIC> g_ft(new Arduino_FT3x68(
    g_iicbus, FT3168_DEVICE_ADDRESS, DRIVEBUS_DEFAULT_VALUE, WC_TP_INT, Arduino_IIC_Touch_Interrupt)); // FIX reset
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
static lv_indev_t *g_touch_indev=nullptr;
static lv_obj_t *g_park=nullptr, *g_pause_box=nullptr;
static bool g_touch_down=false, g_tap=false, g_touch_block=false;
static bool g_wake_request=false, g_wake_armed=true;
static int g_release_count=0;
static uint8_t g_fade_from=0, g_fade_target=0;
static uint32_t g_fade_start=0, g_fade_duration=0;
static bool g_fading=false;
static void wc_input_reset(){
    g_tap=false;g_pending_swipe=WC_SWIPE_NONE;g_touch_down=false;
    g_touch_block=true;g_release_count=0;
    g_tc_x=g_ts_x=WC_LCD_WIDTH/2;g_tc_y=g_ts_y=WC_LCD_HEIGHT/2;
    if(g_touch_indev)lv_indev_reset(g_touch_indev,NULL);
}
static bool wc_take_tap(){bool tap=g_tap;g_tap=false;return tap;}
static void wc_pause_overlay(){
    if(g_pause_box || !g_gl_running)return;
    g_paused=true;
    g_pause_box=lv_obj_create(lv_layer_top());lv_obj_set_size(g_pause_box,WC_LCD_WIDTH,WC_LCD_HEIGHT);
    lv_obj_set_style_bg_color(g_pause_box,lv_color_hex(0x080E18),0);
    lv_obj_set_style_bg_opa(g_pause_box,LV_OPA_COVER,0);
    lv_obj_t *l=lv_label_create(g_pause_box);lv_label_set_text(l,"PAUSED\nYour game is safe");
    lv_obj_set_style_text_color(l,lv_color_hex(0xFFFFFF),0);lv_obj_align(l,LV_ALIGN_TOP_MID,0,100);
    for(int i=0;i<2;i++){
        lv_obj_t *b=lv_btn_create(g_pause_box);lv_obj_set_size(b,260,60);lv_obj_align(b,LV_ALIGN_TOP_MID,0,200+i*90);
        l=lv_label_create(b);lv_label_set_text(l,i?"HOME":"RESUME");lv_obj_center(l);
        lv_obj_add_event_cb(b,[](lv_event_t *e){
            bool home=(intptr_t)lv_event_get_user_data(e)!=0;
            lv_obj_del_async(g_pause_box);g_pause_box=nullptr;g_paused=false;wc_input_reset();
            // CLICKED is delivered after release; no second release wait is
            // needed here (it would swallow a quick next tap on Home).
            g_touch_block=false;
            if(home)wc_game_request_exit();
        },LV_EVENT_CLICKED,(void*)(intptr_t)i);
    }
}
static void wc_game_home_button(){
    lv_obj_t *b=lv_btn_create(lv_scr_act());lv_obj_set_size(b,76,36);lv_obj_set_pos(b,326,6);
    lv_obj_t *l=lv_label_create(b);lv_label_set_text(l,"HOME");lv_obj_center(l);
    lv_obj_add_event_cb(b,[](lv_event_t*){wc_game_request_exit();},LV_EVENT_CLICKED,NULL);
}
static void wc_park_screen(){
    if(g_pause_box){lv_obj_del(g_pause_box);g_pause_box=nullptr;}
    if(!g_park){g_park=lv_obj_create(NULL);lv_obj_set_style_bg_color(g_park,lv_color_hex(0),0);}
    lv_scr_load(g_park);
}
static void wc_screen_set(bool off){
    if(g_screen_off==off)return;
    g_wake_armed=!g_touch_down;
    g_screen_off=off;wc_input_reset();g_wake_request=false;
    if(off)wc_pause_overlay();
    else {g_last_touch_ms=millis();lv_obj_invalidate(lv_scr_act());}
    g_fade_from=g_cur_brightness;g_fade_target=off?0:WC_DEFAULT_BRIGHTNESS;
    g_fade_start=millis();g_fade_duration=off?220:200;g_fading=true;
}
static void wc_service(){
    uint32_t now=millis();
#if !defined(LV_TICK_CUSTOM) || LV_TICK_CUSTOM == 0
    static uint32_t tick=now;lv_tick_inc(now-tick);tick=now;
#endif
    static uint32_t poll=0, fired=0;static bool have_fired=false;
    if(g_has_pmu && now-poll>=50U){
        poll=now;g_pmu.getIrqStatus();bool press=g_pmu.isPekeyShortPressIrq();g_pmu.clearIrqStatus();
        if(press && (!have_fired || now-fired>=400U)){
            fired=now;have_fired=true;wc_screen_set(!g_screen_off);
        }
    }
    lv_timer_handler();
    if(g_wake_request)wc_screen_set(false);
    now=millis();
    if(!g_screen_off && now-g_last_touch_ms>=15000U)wc_screen_set(true);
    if(g_fading){
        uint32_t elapsed=now-g_fade_start;
        int value=elapsed>=g_fade_duration?g_fade_target:
            (int)g_fade_from+((int)g_fade_target-(int)g_fade_from)*(int32_t)elapsed/(int32_t)g_fade_duration;
        g_cur_brightness=(uint8_t)value;g_gfx->setBrightness(g_cur_brightness);
        if(elapsed>=g_fade_duration)g_fading=false;
    }
}

// FIX B: LauncherOS flush form
static void wc_flush(lv_disp_drv_t *drv, const lv_area_t *area, lv_color_t *px) {
    uint32_t w = lv_area_get_width(area), h = lv_area_get_height(area);
    g_gfx->draw16bitRGBBitmap(area->x1, area->y1, (uint16_t*)&px->full, w, h);
    lv_disp_flush_ready(drv);
}
// FIX C: read real FINGER_NUMBER + release hysteresis
static void wc_touch(lv_indev_drv_t *drv, lv_indev_data_t *data) {
    int32_t x = g_ft->IIC_Read_Device_Value(g_ft->Arduino_IIC_Touch::Value_Information::TOUCH_COORDINATE_X);
    int32_t y = g_ft->IIC_Read_Device_Value(g_ft->Arduino_IIC_Touch::Value_Information::TOUCH_COORDINATE_Y);
    int fingers = g_ft->IIC_Read_Device_Value(g_ft->Arduino_IIC_Touch::Value_Information::TOUCH_FINGER_NUMBER);
    bool pressed = (fingers > 0 && fingers<=2 && x>=0 && x<WC_LCD_WIDTH && y>=0 && y<WC_LCD_HEIGHT);
    static int zero_streak = 0;
    static bool inDrag = false;
    if(pressed && !g_screen_off)g_last_touch_ms=millis();
    if(g_screen_off || g_touch_block){
        data->state=LV_INDEV_STATE_REL;inDrag=false;g_touch_down=false;g_tap=false;
        if(pressed){
            g_release_count=0;
            if(g_screen_off && g_wake_armed)g_wake_request=true;
        }else if(++g_release_count>=WC_TP_RELEASE_HYST){g_touch_block=false;g_wake_armed=true;}
        return;
    }
    if (pressed) zero_streak = 0;
    else if (inDrag && ++zero_streak < WC_TP_RELEASE_HYST) { pressed = true; x = g_tc_x; y = g_tc_y; }
    if (pressed) {
        data->state = LV_INDEV_STATE_PR; data->point.x = x; data->point.y = y;
        if (!inDrag) { inDrag = true; g_ts_x = x; g_ts_y = y; g_ts_ms = millis();
            if(g_gl_running && !g_paused && !(x>=320 && y<50))g_tap=true;
        }
        g_touch_down=true;
        g_tc_x = x; g_tc_y = y;
    } else {
        data->state = LV_INDEV_STATE_REL;
        g_touch_down=false;
        if (inDrag) {
            inDrag = false;
            int32_t dx = g_tc_x - g_ts_x, dy = g_tc_y - g_ts_y;
            uint32_t dur = millis() - g_ts_ms;
            if (dur < WC_SWIPE_MAX_DURATION_MS) {
                if (abs(dx) > abs(dy) && abs(dx) > WC_SWIPE_THRESHOLD_PX)
                    g_pending_swipe = (dx < 0) ? WC_SWIPE_LEFT : WC_SWIPE_RIGHT;
                else if (abs(dy) > WC_SWIPE_THRESHOLD_PX)
                    g_pending_swipe = (dy < 0) ? WC_SWIPE_UP : WC_SWIPE_DOWN;
                if(g_pending_swipe!=WC_SWIPE_NONE)g_last_swipe_ms=millis();
            }
        }
    }
}

static void anchor_launcher_as_boot() {
    const esp_partition_t *factory = esp_partition_find_first(
        ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_FACTORY, NULL);
    if (!factory) { Serial.println("[boot] factory partition not found -- skip anchor"); return; }
    esp_err_t e = esp_ota_set_boot_partition(factory);
    Serial.printf("[boot] anchored bootloader to '%s' (err=%d)\n", factory->label, (int)e);
}
static void wc_fade_brightness(uint8_t target, uint32_t dur_ms) {
    int from = g_cur_brightness, to = target;
    if (from == to) return;
    const int steps = 12; int step_ms = (int)dur_ms / steps; if (step_ms < 1) step_ms = 1;
    for (int i = 1; i <= steps; i++) {
        int b = from + ((to - from) * i) / steps; if (b < 0) b = 0; if (b > 255) b = 255;
        g_gfx->setBrightness((uint8_t)b); g_cur_brightness = (uint8_t)b;
        lv_timer_handler(); delay(step_ms);
    }
    g_gfx->setBrightness(target); g_cur_brightness = target;
}
static void wc_i2c_bus_recover() {
    pinMode(WC_IIC_SCL, OUTPUT);
    for (int i = 0; i < 9; i++) { digitalWrite(WC_IIC_SCL, LOW); delayMicroseconds(5);
                                  digitalWrite(WC_IIC_SCL, HIGH); delayMicroseconds(5); }
}
static bool wc_sd_mount_with_retry() {
    SD_MMC.setPins(WC_SDMMC_CLK, WC_SDMMC_CMD, WC_SDMMC_DATA);
    for (int attempt = 1; attempt <= WC_SD_MOUNT_RETRIES; attempt++) {
        if (SD_MMC.begin("/sdcard", true)) { Serial.printf("[sd] mounted (attempt %d)\n", attempt); g_sd_ok = true; return true; }
        Serial.printf("[sd] mount fail (attempt %d/%d)\n", attempt, WC_SD_MOUNT_RETRIES); delay(120);
    }
    g_sd_ok = false; Serial.println("[sd] no card -- SD-less mode"); return false;
}
// FIX D + E inside boot chain
static void wc_hw_boot() {
    Serial.begin(115200);
    // anchor_launcher_as_boot();  // 3-strike guard handles fallback instead
    btStop();
    pinMode(WC_BOOT_BTN, INPUT_PULLUP);
    pinMode(WC_PWR_BTN_PIN, INPUT_PULLUP);
    if (!g_gfx->begin()) Serial.println("[gfx] begin FAILED");
    g_gfx->fillScreen(RGB565_BLACK);
    g_gfx->setBrightness(0); g_cur_brightness = 0;
    wc_i2c_bus_recover();
    Wire.begin(WC_IIC_SDA, WC_IIC_SCL); Wire.setClock(400000);
    if (g_pmu.begin(Wire, AXP2101_SLAVE_ADDRESS, WC_IIC_SDA, WC_IIC_SCL)) {
        g_has_pmu = true;
        g_pmu.disableTSPinMeasure();          // FIX D
        g_pmu.enableBattVoltageMeasure();     // FIX D
        g_pmu.disableIRQ(XPOWERS_AXP2101_ALL_IRQ);g_pmu.clearIrqStatus();
        g_pmu.enableIRQ(XPOWERS_AXP2101_PKEY_SHORT_IRQ);
        g_pmu.setALDO1Voltage(3300); g_pmu.enableALDO1();
        g_pmu.setBLDO1Voltage(3300); g_pmu.enableBLDO1();
        delay(40);
    }
    pinMode(WC_TP_RESET, OUTPUT);
    digitalWrite(WC_TP_RESET, LOW);  delay(50);
    digitalWrite(WC_TP_RESET, HIGH); delay(200);
    pinMode(WC_TP_INT, INPUT_PULLUP);
    { int t = 0; while (!g_ft->begin()) { Serial.println("[touch] FT3168 init fail"); delay(200);   // FIX D
        if (++t > 50) { Serial.println("[touch] FT3168 giving up"); break; } } }
    g_ft->IIC_Write_Device_State(
        g_ft->Arduino_IIC_Touch::Device::TOUCH_POWER_MODE,
        g_ft->Arduino_IIC_Touch::Device_Mode::TOUCH_POWER_MONITOR);
    lv_init();
    size_t px = WC_LCD_WIDTH * 60;
    g_lvgl_buf = (lv_color_t*)heap_caps_malloc(px * sizeof(lv_color_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!g_lvgl_buf) { Serial.println("[lvgl] internal alloc failed -- PSRAM");
        g_lvgl_buf = (lv_color_t*)heap_caps_malloc(px * sizeof(lv_color_t), MALLOC_CAP_SPIRAM); }
    if (!g_lvgl_buf) { Serial.println("[lvgl] PSRAM failed -- 8BIT");
        g_lvgl_buf = (lv_color_t*)heap_caps_malloc(px * sizeof(lv_color_t), MALLOC_CAP_8BIT); }
    if (!g_lvgl_buf) { Serial.println("[lvgl] FATAL: no draw buffer -- halting"); while (1) delay(1000); }  // FIX E
    lv_disp_draw_buf_init(&g_draw_buf, g_lvgl_buf, NULL, px);
    lv_disp_drv_init(&g_disp_drv);
    g_disp_drv.flush_cb = wc_flush; g_disp_drv.draw_buf = &g_draw_buf;
    g_disp_drv.hor_res = WC_LCD_WIDTH; g_disp_drv.ver_res = WC_LCD_HEIGHT;
    lv_disp_drv_register(&g_disp_drv);
    lv_indev_drv_init(&g_indev_drv);
    g_indev_drv.type = LV_INDEV_TYPE_POINTER;
    g_indev_drv.read_cb = wc_touch;
    g_touch_indev=lv_indev_drv_register(&g_indev_drv);
    wc_sd_mount_with_retry();
}
static void wc_pmu_refresh() {
    if (!g_has_pmu) return;
    wc_battery_set_sample(g_pmu.getBatteryPercent(), g_pmu.isCharging(), g_pmu.getBattVoltage());
}

// ---- RADAR BOOT SPLASH (ported from LauncherOS/TacticalOS repo) -----------
static void wc_boot_splash() {
    g_gfx->fillScreen(RGB565_BLACK);
    for (int r = 60; r <= 240; r += 60) { g_gfx->drawCircle(WC_LCD_WIDTH/2, WC_LCD_HEIGHT/2, r, g_gfx->color565(0x60,0x10,0x10)); delay(80); }
    g_gfx->drawLine(WC_LCD_WIDTH/2, 60, WC_LCD_WIDTH/2, WC_LCD_HEIGHT-60, g_gfx->color565(0x40,0x08,0x08));
    g_gfx->drawLine(40, WC_LCD_HEIGHT/2, WC_LCD_WIDTH-40, WC_LCD_HEIGHT/2, g_gfx->color565(0x40,0x08,0x08));
    const char *title = "VOYAGER//OS";
    g_gfx->setTextSize(3); g_gfx->setTextColor(g_gfx->color565(0xFF,0x30,0x30)); g_gfx->setBrightness(180);
    for (int i = 0; title[i]; i++) { g_gfx->setCursor(110 + i*18, WC_LCD_HEIGHT/2 - 12); g_gfx->print(title[i]); delay(45); }
    g_gfx->setTextSize(1); g_gfx->setTextColor(g_gfx->color565(0x8A,0x10,0x10));
    const char *l2 = "LOADING VOYAGER...";
    for (int i = 0; l2[i]; i++) { g_gfx->setCursor(105 + i*6, WC_LCD_HEIGHT/2 + 30); g_gfx->print(l2[i]); delay(18); }
    for (int s = 0; s < 360; s += 12) { float a = s*M_PI/180.0f;
        g_gfx->drawLine(WC_LCD_WIDTH/2, WC_LCD_HEIGHT/2, WC_LCD_WIDTH/2+(int)(220*sinf(a)), WC_LCD_HEIGHT/2-(int)(220*cosf(a)), g_gfx->color565(0x60,0x10,0x10)); delay(8); }
    g_cur_brightness = 180;
}

// ===========================================================================
// GAMES
// ===========================================================================
static void pong_init(); static void pong_loop(); static void pong_exit();
static void snake_init(); static void snake_loop(); static void snake_exit();
static void flap_init(); static void flap_loop(); static void flap_exit();
static void breakout_init(); static void breakout_loop(); static void breakout_exit();
static void dino_init(); static void dino_loop(); static void dino_exit();
static void t2048_init(); static void t2048_loop(); static void t2048_exit();
static void mole_init(); static void mole_loop(); static void mole_exit();
static void dodge_init(); static void dodge_loop(); static void dodge_exit();
static void xi_init(); static void player_init(); static void draft_loop(); static void draft_exit();
static void memory_init(); static void memory_loop(); static void puzzle_exit();
static void lights_init(); static void lights_loop();
static void reaction_init(); static void reaction_loop();

static void register_games() {
    WcGameEntry e;
    e = { "pong",     "Pong",         "Arcade", 0x9BD16A, pong_init, pong_loop, pong_exit }; wc_game_register(e);
    e = { "snake",    "Snake",        "Retro",  0x39FF14, snake_init, snake_loop, snake_exit }; wc_game_register(e);
    e = { "flappy",   "Flappy",       "Arcade", 0xFFB300, flap_init, flap_loop, flap_exit }; wc_game_register(e);
    e = { "breakout", "Breakout",     "Arcade", 0xFF476F, breakout_init, breakout_loop, breakout_exit }; wc_game_register(e);
    e = { "dino",     "Dino Run",     "Arcade", 0x9BD16A, dino_init, dino_loop, dino_exit }; wc_game_register(e);
    e = { "2048",     "2048",         "Puzzle", 0xEDC22E, t2048_init, t2048_loop, t2048_exit }; wc_game_register(e);
    e = { "mole",     "Whack-a-Mole", "Arcade", 0xFFB300, mole_init, mole_loop, mole_exit }; wc_game_register(e);
    e = { "dodge",    "Space Dodge",  "Arcade", 0x50E3FF, dodge_init, dodge_loop, dodge_exit }; wc_game_register(e);
    e = { "draftxi",  "Draft XI",     "Footy",  0x9BD16A, xi_init,     draft_loop, draft_exit }; wc_game_register(e);
    e = { "buildpl",  "Build Player", "Footy",  0xE5B14F, player_init, draft_loop, draft_exit }; wc_game_register(e);
    e = { "memory", "Memory Match", "Puzzle", 0xBB86FC, memory_init, memory_loop, puzzle_exit }; wc_game_register(e);
    e = { "lights", "Lights Out", "Puzzle", 0x50E3FF, lights_init, lights_loop, puzzle_exit }; wc_game_register(e);
    e = { "reaction", "Reaction Test", "Skill", 0xFFB300, reaction_init, reaction_loop, puzzle_exit }; wc_game_register(e);
    e = { "voyager", "Voyager", "Travel", 0x00C7C7, voyager_app_init, voyager_app_loop, voyager_app_exit }; wc_game_register(e);
}

// ---- Pong ----
static lv_obj_t *g_pong_scr, *g_pong_ball, *g_pong_pad, *g_pong_score_lbl;
static float g_pong_bx, g_pong_by, g_pong_vx, g_pong_vy; static int g_pong_score;
static void pong_init() {
    g_pong_scr = lv_obj_create(NULL); lv_obj_set_style_bg_color(g_pong_scr, lv_color_hex(0x0b140a), 0); lv_scr_load(g_pong_scr);
    g_pong_ball = lv_obj_create(g_pong_scr); lv_obj_set_size(g_pong_ball, 18, 18);
    lv_obj_set_style_bg_color(g_pong_ball, lv_color_hex(wc_theme_accent()), 0); lv_obj_set_style_radius(g_pong_ball, 9, 0);
    g_pong_pad = lv_obj_create(g_pong_scr); lv_obj_set_size(g_pong_pad, 80, 12);
    lv_obj_set_style_bg_color(g_pong_pad, lv_color_hex(wc_theme_primary()), 0); lv_obj_align(g_pong_pad, LV_ALIGN_BOTTOM_MID, 0, -20);
    g_pong_score_lbl = lv_label_create(g_pong_scr); lv_label_set_text(g_pong_score_lbl, "0");
    lv_obj_set_style_text_color(g_pong_score_lbl, lv_color_hex(wc_theme_primary()), 0);
    lv_obj_set_style_text_font(g_pong_score_lbl, &lv_font_montserrat_32, 0); lv_obj_align(g_pong_score_lbl, LV_ALIGN_TOP_MID, 0, 12);
    g_pong_bx = 200; g_pong_by = 100; g_pong_vx = 2.4f; g_pong_vy = 3.1f; g_pong_score = 0;
}
static void pong_loop() {
    float old_y=g_pong_by;
    g_pong_bx += g_pong_vx; g_pong_by += g_pong_vy;
    if(g_pong_bx<9){g_pong_bx=9;g_pong_vx=fabsf(g_pong_vx);}
    if(g_pong_bx>WC_LCD_WIDTH-9){g_pong_bx=WC_LCD_WIDTH-9;g_pong_vx=-fabsf(g_pong_vx);}
    if (g_pong_by < 9) g_pong_vy = -g_pong_vy;
    int px_ = g_tc_x - 40; if (px_ < 0) px_ = 0; if (px_ > WC_LCD_WIDTH-80) px_ = WC_LCD_WIDTH-80;
    lv_obj_set_x(g_pong_pad, px_);
    int py_ = WC_LCD_HEIGHT - 32;
    if (g_pong_vy>0 && old_y<=py_-9 && g_pong_by>=py_-9 && g_pong_bx>=px_-9 && g_pong_bx<=px_+89) {
        g_pong_by=py_-9;g_pong_vy = -fabs(g_pong_vy); g_pong_score++;
        g_pong_vx=((g_pong_bx-(px_+40))/40.0f)*5.0f;
        g_pong_vx *= 1.04f; g_pong_vy *= 1.04f;                 // difficulty ramp
        if (g_pong_vy < -9.0f) g_pong_vy = -9.0f;
        if (g_pong_vx > 9.0f) g_pong_vx = 9.0f; if (g_pong_vx < -9.0f) g_pong_vx = -9.0f;
        char b[8]; snprintf(b,8,"%d",g_pong_score); lv_label_set_text(g_pong_score_lbl,b);
    }
    if (g_pong_by > WC_LCD_HEIGHT) { game_over("pong", g_pong_score); return; }
    lv_obj_set_pos(g_pong_ball, (int)g_pong_bx-9, (int)g_pong_by-9);
}
static void pong_exit() { if (g_pong_scr) { lv_obj_del(g_pong_scr); g_pong_scr = nullptr; } }

// ---- Snake ----
static lv_obj_t *g_snk_scr, *g_snk_lbl;
static int g_snk_ex, g_snk_ey, g_snk_dx, g_snk_dy, g_snk_len;
static int g_snk_bx[64], g_snk_by[64]; static uint32_t g_snk_last;
static lv_obj_t *g_snk_segments[64], *g_snk_food;
static void snake_food(){
    bool occupied;
    do{
        g_snk_ex=random(25);g_snk_ey=random(30);occupied=false;
        for(int i=0;i<g_snk_len;i++)if(g_snk_bx[i]==g_snk_ex && g_snk_by[i]==g_snk_ey)occupied=true;
    }while(occupied);
}
static void snake_init() {
    g_snk_scr = lv_obj_create(NULL); lv_obj_set_style_bg_color(g_snk_scr, lv_color_hex(0x040A04), 0); lv_scr_load(g_snk_scr);
    g_snk_lbl = lv_label_create(g_snk_scr); lv_label_set_text(g_snk_lbl, "SNAKE\nswipe to steer");
    lv_obj_set_style_text_color(g_snk_lbl, lv_color_hex(0x9EFF6E), 0);
    lv_obj_set_style_text_font(g_snk_lbl, &lv_font_montserrat_18, 0); lv_obj_align(g_snk_lbl, LV_ALIGN_TOP_MID, 0, 16);
    g_snk_len = 3; g_snk_dx = 1; g_snk_dy = 0; g_snk_ex = 10; g_snk_ey = 10;
    for (int i = 0; i < g_snk_len; i++) { g_snk_bx[i] = 5-i; g_snk_by[i] = 5; }
    g_snk_last = 0;
    for(int i=0;i<64;i++){
        g_snk_segments[i]=lv_obj_create(g_snk_scr);lv_obj_set_size(g_snk_segments[i],14,14);
        lv_obj_set_style_bg_color(g_snk_segments[i],lv_color_hex(i?0x39FF14:0xEAF6FF),0);
        lv_obj_set_style_border_width(g_snk_segments[i],0,0);lv_obj_add_flag(g_snk_segments[i],LV_OBJ_FLAG_HIDDEN);
    }
    g_snk_food=lv_obj_create(g_snk_scr);lv_obj_set_size(g_snk_food,14,14);
    lv_obj_set_style_bg_color(g_snk_food,lv_color_hex(0xFF476F),0);snake_food();
}
static void snake_loop() {
    int step=max(90,220-(g_snk_len-3)*6);
    if(g_play_ms-g_snk_last<(uint32_t)step)return;
    g_snk_last=g_play_ms;
    int dx=g_snk_dx,dy=g_snk_dy;
    if(g_pending_swipe==WC_SWIPE_LEFT){dx=-1;dy=0;}
    if(g_pending_swipe==WC_SWIPE_RIGHT){dx=1;dy=0;}
    if(g_pending_swipe==WC_SWIPE_UP){dx=0;dy=-1;}
    if(g_pending_swipe==WC_SWIPE_DOWN){dx=0;dy=1;}
    g_pending_swipe=WC_SWIPE_NONE;
    if(dx!=-g_snk_dx || dy!=-g_snk_dy){g_snk_dx=dx;g_snk_dy=dy;}
    int nx=g_snk_bx[0]+g_snk_dx,ny=g_snk_by[0]+g_snk_dy;
    bool eat=nx==g_snk_ex && ny==g_snk_ey;
    if(nx<0||ny<0||nx>=25||ny>=30){game_over("snake",g_snk_len-3);return;}
    for(int i=0;i<g_snk_len-(eat?0:1);i++)
        if(nx==g_snk_bx[i] && ny==g_snk_by[i]){game_over("snake",g_snk_len-3);return;}
    if(eat && g_snk_len<64)g_snk_len++;
    for(int i=g_snk_len-1;i>0;i--){g_snk_bx[i]=g_snk_bx[i-1];g_snk_by[i]=g_snk_by[i-1];}
    g_snk_bx[0]=nx;g_snk_by[0]=ny;
    if(eat){if(g_snk_len==64){game_over("snake",61);return;}snake_food();}
    for(int i=0;i<g_snk_len;i++){lv_obj_clear_flag(g_snk_segments[i],LV_OBJ_FLAG_HIDDEN);lv_obj_set_pos(g_snk_segments[i],5+g_snk_bx[i]*16,70+g_snk_by[i]*14);}
    lv_obj_set_pos(g_snk_food,5+g_snk_ex*16,70+g_snk_ey*14);
    char text[40];snprintf(text,sizeof(text),"SNAKE  %d\nSwipe to steer",g_snk_len-3);lv_label_set_text(g_snk_lbl,text);
}
static void snake_exit() { if (g_snk_scr) { lv_obj_del(g_snk_scr); g_snk_scr = nullptr; } }

// ---- Flappy ----
static lv_obj_t *g_fl_scr, *g_fl_bird, *g_fl_pipe, *g_fl_pipe_bottom, *g_fl_score;
static float g_fl_by, g_fl_vy; static int g_fl_px, g_fl_gap_y, g_fl_sc;
static void flap_init() {
    g_fl_scr = lv_obj_create(NULL); lv_obj_set_style_bg_color(g_fl_scr, lv_color_hex(0x081018), 0); lv_scr_load(g_fl_scr);
    g_fl_bird = lv_obj_create(g_fl_scr); lv_obj_set_size(g_fl_bird, 22, 22); lv_obj_set_style_radius(g_fl_bird, 11, 0);
    lv_obj_set_style_bg_color(g_fl_bird, lv_color_hex(0xFFB300), 0);
    g_fl_pipe = lv_obj_create(g_fl_scr); lv_obj_set_size(g_fl_pipe, 40, WC_LCD_HEIGHT);
    lv_obj_set_style_bg_color(g_fl_pipe, lv_color_hex(0x66C0FF), 0);
    g_fl_pipe_bottom=lv_obj_create(g_fl_scr);lv_obj_set_style_bg_color(g_fl_pipe_bottom,lv_color_hex(0x66C0FF),0);
    g_fl_score = lv_label_create(g_fl_scr); lv_label_set_text(g_fl_score, "0");
    lv_obj_set_style_text_color(g_fl_score, lv_color_hex(0xEAF6FF), 0);
    lv_obj_set_style_text_font(g_fl_score, &lv_font_montserrat_28, 0); lv_obj_align(g_fl_score, LV_ALIGN_TOP_MID, 0, 12);
    g_fl_by = 200; g_fl_vy = 0; g_fl_px = WC_LCD_WIDTH; g_fl_gap_y = 200; g_fl_sc = 0;
}
static void flap_loop() {
    if(wc_take_tap())g_fl_vy=-5.5f;
    g_fl_vy += 0.25f; g_fl_by += g_fl_vy; g_fl_px -= min(7,3+g_fl_sc/4);
    if (g_fl_px < -40) { g_fl_px = WC_LCD_WIDTH; g_fl_gap_y = 125 + random(240); g_fl_sc++;
        char b[8]; snprintf(b,8,"%d",g_fl_sc); lv_label_set_text(g_fl_score,b); }
    int half=max(60,90-g_fl_sc*2);
    if (g_fl_by+22 > WC_LCD_HEIGHT || g_fl_by < 0 ||
        (102>g_fl_px && 80<g_fl_px+40 && (g_fl_by<g_fl_gap_y-half || g_fl_by+22>g_fl_gap_y+half))) {
        game_over("flappy", g_fl_sc); return;
    }
    lv_obj_set_size(g_fl_pipe,40,g_fl_gap_y-half);
    lv_obj_set_size(g_fl_pipe_bottom,40,WC_LCD_HEIGHT-g_fl_gap_y-half);
    lv_obj_set_pos(g_fl_pipe_bottom,g_fl_px,g_fl_gap_y+half);
    lv_obj_set_pos(g_fl_bird, 80, (int)g_fl_by); lv_obj_set_pos(g_fl_pipe, g_fl_px, 0);
}
static void flap_exit() { if (g_fl_scr) { lv_obj_del(g_fl_scr); g_fl_scr = nullptr; } }

// ---- Breakout ----
static lv_obj_t *g_bk_scr, *g_bk_ball, *g_bk_pad, *g_bk_score_lbl, *g_bk_bricks[24];
static float g_bk_bx, g_bk_by, g_bk_vx, g_bk_vy; static int g_bk_score, g_bk_left;
static void breakout_init() {
    g_bk_scr = lv_obj_create(NULL); lv_obj_set_style_bg_color(g_bk_scr, lv_color_hex(0x05070f), 0); lv_scr_load(g_bk_scr);
    g_bk_score = 0; g_bk_left = 24;
    int bw = 60, bh = 22, gap = 6, x0 = (WC_LCD_WIDTH-(6*bw+5*gap))/2, y0 = 70;
    uint32_t rc[4] = {0xFF476F,0xFFB300,0x39FF14,0x66C0FF};
    for (int r = 0; r < 4; r++) for (int c = 0; c < 6; c++) {
        lv_obj_t *b = lv_obj_create(g_bk_scr); lv_obj_set_size(b, bw, bh);
        lv_obj_set_pos(b, x0+c*(bw+gap), y0+r*(bh+gap));
        lv_obj_set_style_bg_color(b, lv_color_hex(rc[r]), 0); lv_obj_set_style_border_width(b, 0, 0); lv_obj_set_style_radius(b, 4, 0);
        g_bk_bricks[r*6+c] = b;
    }
    g_bk_pad = lv_obj_create(g_bk_scr); lv_obj_set_size(g_bk_pad, 90, 14);
    lv_obj_set_style_bg_color(g_bk_pad, lv_color_hex(0xEDEDED), 0); lv_obj_set_style_radius(g_bk_pad, 7, 0);
    lv_obj_align(g_bk_pad, LV_ALIGN_BOTTOM_MID, 0, -30);
    g_bk_ball = lv_obj_create(g_bk_scr); lv_obj_set_size(g_bk_ball, 16, 16);
    lv_obj_set_style_radius(g_bk_ball, 8, 0); lv_obj_set_style_bg_color(g_bk_ball, lv_color_hex(0xFFFFFF), 0);
    g_bk_score_lbl = lv_label_create(g_bk_scr); lv_label_set_text(g_bk_score_lbl, "0");
    lv_obj_set_style_text_color(g_bk_score_lbl, lv_color_hex(0xEDEDED), 0);
    lv_obj_set_style_text_font(g_bk_score_lbl, &lv_font_montserrat_24, 0); lv_obj_align(g_bk_score_lbl, LV_ALIGN_TOP_MID, 0, 14);
    g_bk_bx = WC_LCD_WIDTH/2; g_bk_by = WC_LCD_HEIGHT/2+60; g_bk_vx = 3.0f; g_bk_vy = -3.4f;
}
static void breakout_loop() {
    int px = g_tc_x-45; if (px < 0) px = 0; if (px > WC_LCD_WIDTH-90) px = WC_LCD_WIDTH-90;
    lv_obj_set_x(g_bk_pad, px); int pady = WC_LCD_HEIGHT-44;
    g_bk_bx += g_bk_vx; g_bk_by += g_bk_vy;
    if (g_bk_bx < 8) { g_bk_bx = 8; g_bk_vx = -g_bk_vx; }
    if (g_bk_bx > WC_LCD_WIDTH-8) { g_bk_bx = WC_LCD_WIDTH-8; g_bk_vx = -g_bk_vx; }
    if (g_bk_by < 8) { g_bk_by = 8; g_bk_vy = -g_bk_vy; }
    if (g_bk_by > pady-8 && g_bk_by < pady+8 && g_bk_bx > px && g_bk_bx < px+90 && g_bk_vy > 0) {
        g_bk_by=pady-8;g_bk_vy = -fabs(g_bk_vy); g_bk_vx = ((g_bk_bx-(px+45))/45.0f)*4.0f;
    }
    for (int i = 0; i < 24; i++) if (g_bk_bricks[i]) {
        int bx = lv_obj_get_x(g_bk_bricks[i]), by = lv_obj_get_y(g_bk_bricks[i]);
        if (g_bk_bx > bx-8 && g_bk_bx < bx+68 && g_bk_by > by-8 && g_bk_by < by+30) {
            lv_obj_del(g_bk_bricks[i]); g_bk_bricks[i] = nullptr; g_bk_vy = -g_bk_vy; g_bk_score += 10; g_bk_left--;
            char b[8]; snprintf(b,8,"%d",g_bk_score); lv_label_set_text(g_bk_score_lbl,b); break;
        }
    }
    if (g_bk_by > WC_LCD_HEIGHT || g_bk_left <= 0) { game_over("breakout", g_bk_score); return; }
    lv_obj_set_pos(g_bk_ball, (int)g_bk_bx-8, (int)g_bk_by-8);
}
static void breakout_exit() { if (g_bk_scr) { lv_obj_del(g_bk_scr); g_bk_scr = nullptr; } }

// ---- Dino Run ----
static lv_obj_t *g_dino_scr, *g_dino_ch, *g_dino_ob, *g_dino_score_lbl;
static float g_dino_y, g_dino_vy; static int g_dino_obx, g_dino_score; static uint32_t g_dino_last_jump;
static void dino_init() {
    g_dino_scr = lv_obj_create(NULL); lv_obj_set_style_bg_color(g_dino_scr, lv_color_hex(0x101418), 0); lv_scr_load(g_dino_scr);
    lv_obj_t *gr = lv_obj_create(g_dino_scr); lv_obj_set_size(gr, WC_LCD_WIDTH, 4);
    lv_obj_set_style_bg_color(gr, lv_color_hex(0x555555), 0); lv_obj_align(gr, LV_ALIGN_TOP_LEFT, 0, WC_LCD_HEIGHT-120);
    g_dino_ch = lv_obj_create(g_dino_scr); lv_obj_set_size(g_dino_ch, 30, 30);
    lv_obj_set_style_bg_color(g_dino_ch, lv_color_hex(0x9BD16A), 0); lv_obj_set_style_radius(g_dino_ch, 6, 0);
    g_dino_ob = lv_obj_create(g_dino_scr); lv_obj_set_size(g_dino_ob, 24, 44); lv_obj_set_style_bg_color(g_dino_ob, lv_color_hex(0xE5484D), 0);
    g_dino_score_lbl = lv_label_create(g_dino_scr); lv_label_set_text(g_dino_score_lbl, "0");
    lv_obj_set_style_text_color(g_dino_score_lbl, lv_color_hex(0xEDEDED), 0);
    lv_obj_set_style_text_font(g_dino_score_lbl, &lv_font_montserrat_24, 0); lv_obj_align(g_dino_score_lbl, LV_ALIGN_TOP_MID, 0, 14);
    g_dino_y = 0; g_dino_vy = 0; g_dino_obx = WC_LCD_WIDTH; g_dino_score = 0; g_dino_last_jump = 0;
}
static void dino_loop() {
    int gY = WC_LCD_HEIGHT-120;
    bool tap=wc_take_tap();
    if (g_dino_y == 0 && tap) { g_dino_vy = -11.0f; g_dino_last_jump = g_play_ms; }
    g_dino_vy += 0.6f; g_dino_y += g_dino_vy; if (g_dino_y > 0) { g_dino_y = 0; g_dino_vy = 0; }
    lv_obj_set_pos(g_dino_ch, 60, gY-30+(int)g_dino_y);
    g_dino_obx -= min(12,6+g_dino_score/6);
    if (g_dino_obx < -24) { g_dino_obx = WC_LCD_WIDTH+random(200); g_dino_score++;
        char b[12]; snprintf(b,12,"%d",g_dino_score); lv_label_set_text(g_dino_score_lbl,b); }
    lv_obj_set_pos(g_dino_ob, g_dino_obx, gY-44);
    if (g_dino_obx < 90 && g_dino_obx+24 > 60 && g_dino_y > -44) { game_over("dino", g_dino_score); return; }
}
static void dino_exit() { if (g_dino_scr) { lv_obj_del(g_dino_scr); g_dino_scr = nullptr; } }

// ---- 2048 ----
static lv_obj_t *g_2048_scr, *g_2048_cells[16], *g_2048_score_lbl;
static int g_2048_grid[16], g_2048_score;
static int g_2048_undo[16],g_2048_undo_score;
static bool g_2048_can_undo=false;
static void t2048_draw() {
    const uint32_t pal[12] = {0x1b2430,0xEEE4DA,0xEDE0C8,0xF2B179,0xF59563,0xF67C5F,0xF65E3B,0xEDCF72,0xEDCC61,0xEDC850,0xEDC53F,0xEDC22E};
    for (int i = 0; i < 16; i++) {
        int v = g_2048_grid[i], p = 0, t = v; while (t > 1 && p < 11) { t >>= 1; p++; }
        lv_obj_set_style_bg_color(g_2048_cells[i], lv_color_hex(v ? pal[p] : pal[0]), 0);
        lv_obj_t *lbl = lv_obj_get_child(g_2048_cells[i], 0);
        if (v) { char b[8]; snprintf(b,8,"%d",v); lv_label_set_text(lbl,b); } else lv_label_set_text(lbl,"");
    }
    char s[24]; snprintf(s,24,"%d",g_2048_score); lv_label_set_text(g_2048_score_lbl,s);
}
static void t2048_spawn() {
    int em[16], n = 0; for (int i = 0; i < 16; i++) if (!g_2048_grid[i]) em[n++] = i;
    if (n) g_2048_grid[em[random(n)]] = (random(10) < 9) ? 2 : 4;
}
static bool t2048_slide(int dir) {
    bool moved = false;
    for (int line = 0; line < 4; line++) {
        int idx[4];
        for (int k = 0; k < 4; k++) idx[k] = (dir==0)?line*4+k : (dir==1)?line*4+(3-k) : (dir==2)?k*4+line : (3-k)*4+line;
        int vals[4], m = 0; for (int k = 0; k < 4; k++) if (g_2048_grid[idx[k]]) vals[m++] = g_2048_grid[idx[k]];
        int out[4] = {0,0,0,0}, o = 0;
        for (int k = 0; k < m; k++) { if (k+1 < m && vals[k]==vals[k+1]) { out[o++]=vals[k]*2; g_2048_score+=vals[k]*2; k++; } else out[o++]=vals[k]; }
        for (int k = 0; k < 4; k++) { if (g_2048_grid[idx[k]]!=out[k]) moved = true; g_2048_grid[idx[k]] = out[k]; }
    }
    return moved;
}
static void t2048_init() {
    g_2048_scr = lv_obj_create(NULL); lv_obj_set_style_bg_color(g_2048_scr, lv_color_hex(0x0b0e13), 0);
    lv_obj_clear_flag(g_2048_scr, LV_OBJ_FLAG_SCROLLABLE); lv_scr_load(g_2048_scr);
    g_2048_score_lbl = lv_label_create(g_2048_scr); lv_obj_set_style_text_color(g_2048_score_lbl, lv_color_hex(0xEDEDED), 0);
    lv_obj_set_style_text_font(g_2048_score_lbl, &lv_font_montserrat_24, 0); lv_obj_align(g_2048_score_lbl, LV_ALIGN_TOP_MID, 0, 12);
    int cs = 88, gap = 8, x0 = (WC_LCD_WIDTH-(4*cs+3*gap))/2, y0 = 70;
    for (int i = 0; i < 16; i++) {
        lv_obj_t *c = lv_obj_create(g_2048_scr); lv_obj_set_size(c, cs, cs);
        lv_obj_set_pos(c, x0+(i%4)*(cs+gap), y0+(i/4)*(cs+gap));
        lv_obj_set_style_radius(c, 6, 0); lv_obj_set_style_border_width(c, 0, 0); lv_obj_clear_flag(c, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_t *lbl = lv_label_create(c); lv_obj_set_style_text_color(lbl, lv_color_hex(0x222222), 0);
        lv_obj_set_style_text_font(lbl, &lv_font_montserrat_28, 0); lv_obj_center(lbl); g_2048_cells[i] = c;
    }
    for (int i = 0; i < 16; i++) g_2048_grid[i] = 0; g_2048_score = 0;
    t2048_spawn(); t2048_spawn(); t2048_draw();
    g_2048_can_undo=false;
    lv_obj_t *undo=lv_btn_create(g_2048_scr);lv_obj_set_size(undo,140,36);lv_obj_set_pos(undo,135,458);
    lv_obj_t *label=lv_label_create(undo);lv_label_set_text(label,"UNDO MOVE");lv_obj_center(label);
    lv_obj_add_event_cb(undo,[](lv_event_t*){
        if(!g_2048_can_undo)return;
        memcpy(g_2048_grid,g_2048_undo,sizeof(g_2048_grid));g_2048_score=g_2048_undo_score;
        g_2048_can_undo=false;t2048_draw();
    },LV_EVENT_CLICKED,NULL);
}
static bool t2048_has_move() {
    for (int i = 0; i < 16; i++) if (!g_2048_grid[i]) return true;
    for (int r = 0; r < 4; r++) for (int c = 0; c < 4; c++) {
        int v = g_2048_grid[r*4+c];
        if (c < 3 && g_2048_grid[r*4+c+1] == v) return true;
        if (r < 3 && g_2048_grid[(r+1)*4+c] == v) return true;
    }
    return false;
}
static void t2048_loop() {
    if (g_pending_swipe != WC_SWIPE_NONE) {
        int8_t ev = g_pending_swipe; g_pending_swipe = WC_SWIPE_NONE;
        int dir = (ev==WC_SWIPE_LEFT)?0 : (ev==WC_SWIPE_RIGHT)?1 : (ev==WC_SWIPE_UP)?2 : (ev==WC_SWIPE_DOWN)?3 : -1;
        int before[16];memcpy(before,g_2048_grid,sizeof(before));int score=g_2048_score;
        if (dir >= 0 && t2048_slide(dir)) {
            memcpy(g_2048_undo,before,sizeof(before));g_2048_undo_score=score;g_2048_can_undo=true;
            t2048_spawn(); t2048_draw();
        }
        if (!t2048_has_move()) { game_over("2048", g_2048_score); return; }
    }
}
static void t2048_exit() { if (g_2048_scr) { lv_obj_del(g_2048_scr); g_2048_scr = nullptr; } }

// ---- Whack-a-Mole ----
static lv_obj_t *g_mole_scr, *g_mole_holes[9], *g_mole_score_lbl;
static int g_mole_active, g_mole_score; static uint32_t g_mole_next, g_mole_last_hit, g_mole_start;
static void mole_init() {
    g_mole_scr = lv_obj_create(NULL); lv_obj_set_style_bg_color(g_mole_scr, lv_color_hex(0x1a120a), 0);
    lv_obj_clear_flag(g_mole_scr, LV_OBJ_FLAG_SCROLLABLE); lv_scr_load(g_mole_scr);
    g_mole_score_lbl = lv_label_create(g_mole_scr); lv_label_set_text(g_mole_score_lbl, "0");
    lv_obj_set_style_text_color(g_mole_score_lbl, lv_color_hex(0xFFB300), 0);
    lv_obj_set_style_text_font(g_mole_score_lbl, &lv_font_montserrat_24, 0); lv_obj_align(g_mole_score_lbl, LV_ALIGN_TOP_MID, 0, 12);
    int cs = 100, gap = 14, x0 = (WC_LCD_WIDTH-(3*cs+2*gap))/2, y0 = 80;
    for (int i = 0; i < 9; i++) {
        lv_obj_t *h = lv_obj_create(g_mole_scr); lv_obj_set_size(h, cs, cs);
        lv_obj_set_pos(h, x0+(i%3)*(cs+gap), y0+(i/3)*(cs+gap));
        lv_obj_set_style_radius(h, cs/2, 0); lv_obj_set_style_bg_color(h, lv_color_hex(0x3a2a18), 0); lv_obj_set_style_border_width(h, 0, 0);
        g_mole_holes[i] = h;
    }
    g_mole_active = -1; g_mole_score = 0; g_mole_next = 0; g_mole_last_hit = 0; g_mole_start = g_play_ms;
}
static void mole_loop() {
    uint32_t now = g_play_ms;
    if (now - g_mole_start > 30000) { game_over("mole", g_mole_score); return; }   // 30s round
    if (now > g_mole_next) {
        if (g_mole_active >= 0) lv_obj_set_style_bg_color(g_mole_holes[g_mole_active], lv_color_hex(0x3a2a18), 0);
        uint32_t win = (g_mole_score*12 < 520) ? (900 - g_mole_score*12) : 380;   // ramp: faster moles
        g_mole_active = random(9); lv_obj_set_style_bg_color(g_mole_holes[g_mole_active], lv_color_hex(0x39FF14), 0); g_mole_next = now+win;
    }
    bool tap=wc_take_tap();
    if (g_mole_active >= 0 && tap) {
        int hx = lv_obj_get_x(g_mole_holes[g_mole_active]), hy = lv_obj_get_y(g_mole_holes[g_mole_active]);
        if (g_tc_x > hx && g_tc_x < hx+100 && g_tc_y > hy && g_tc_y < hy+100) {
            g_mole_score++; g_mole_last_hit = now;
            char b[12]; snprintf(b,12,"%d",g_mole_score); lv_label_set_text(g_mole_score_lbl,b);
            lv_obj_set_style_bg_color(g_mole_holes[g_mole_active], lv_color_hex(0x3a2a18), 0); g_mole_active = -1; g_mole_next = now+400;
        }
    }
}
static void mole_exit() { if (g_mole_scr) { lv_obj_del(g_mole_scr); g_mole_scr = nullptr; } }

// ---- Space Dodge ----
static lv_obj_t *g_dg_scr, *g_dg_ship, *g_dg_rock[3], *g_dg_score_lbl;
static int g_dg_ry[3], g_dg_rx[3], g_dg_score;
static void dodge_init() {
    g_dg_scr = lv_obj_create(NULL); lv_obj_set_style_bg_color(g_dg_scr, lv_color_hex(0x03040a), 0); lv_scr_load(g_dg_scr);
    g_dg_ship = lv_obj_create(g_dg_scr); lv_obj_set_size(g_dg_ship, 34, 20); lv_obj_set_style_radius(g_dg_ship, 6, 0);
    lv_obj_set_style_bg_color(g_dg_ship, lv_color_hex(0x50E3FF), 0);
    for (int i = 0; i < 3; i++) {
        g_dg_rock[i] = lv_obj_create(g_dg_scr); lv_obj_set_size(g_dg_rock[i], 30, 30);
        lv_obj_set_style_radius(g_dg_rock[i], 15, 0); lv_obj_set_style_bg_color(g_dg_rock[i], lv_color_hex(0xFF476F), 0);
        g_dg_rx[i] = random(WC_LCD_WIDTH-30); g_dg_ry[i] = -random(500)-i*160;
    }
    g_dg_score_lbl = lv_label_create(g_dg_scr); lv_label_set_text(g_dg_score_lbl, "0");
    lv_obj_set_style_text_color(g_dg_score_lbl, lv_color_hex(0xEDEDED), 0);
    lv_obj_set_style_text_font(g_dg_score_lbl, &lv_font_montserrat_24, 0); lv_obj_align(g_dg_score_lbl, LV_ALIGN_TOP_MID, 0, 12);
    g_dg_score = 0;
}
static void dodge_loop() {
    int sx = g_tc_x-17; if (sx < 0) sx = 0; if (sx > WC_LCD_WIDTH-34) sx = WC_LCD_WIDTH-34;
    int sy = WC_LCD_HEIGHT-60; lv_obj_set_pos(g_dg_ship, sx, sy);
    for (int i = 0; i < 3; i++) {
        g_dg_ry[i] += min(14,6+g_dg_score/8);
        if (g_dg_ry[i] > WC_LCD_HEIGHT) { g_dg_ry[i] = -30; g_dg_rx[i] = random(WC_LCD_WIDTH-30); g_dg_score++;
            char b[12]; snprintf(b,12,"%d",g_dg_score); lv_label_set_text(g_dg_score_lbl,b); }
        lv_obj_set_pos(g_dg_rock[i], g_dg_rx[i], g_dg_ry[i]);
        if (g_dg_ry[i] > sy-30 && g_dg_ry[i] < sy+20 && g_dg_rx[i] < sx+34 && g_dg_rx[i]+30 > sx) { game_over("dodge", g_dg_score); return; }
    }
}
static void dodge_exit() { if (g_dg_scr) { lv_obj_del(g_dg_scr); g_dg_scr = nullptr; } }

// ===== v2.2 game-over / retry modal ==========================================
static volatile int g_go_result = 0;   // 0 pending, 1 retry, 2 exit
static void go_btn_cb(lv_event_t *e) { g_go_result = (int)(intptr_t)lv_event_get_user_data(e); }
static bool show_game_over(const char *id, int score, int best, bool is_best) {
    g_go_result = 0;
    lv_obj_t *s = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s, lv_color_hex(0x0a0a0f), 0);
    lv_obj_clear_flag(s, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *t = lv_label_create(s); lv_label_set_text(t, "GAME OVER");
    lv_obj_set_style_text_color(t, lv_color_hex(wc_theme_danger()), 0);
    lv_obj_set_style_text_font(t, &lv_font_montserrat_32, 0);
    lv_obj_align(t, LV_ALIGN_TOP_MID, 0, 60);
    char sb[40]; snprintf(sb, sizeof(sb), "SCORE  %d", score);
    lv_obj_t *sl = lv_label_create(s); lv_label_set_text(sl, sb);
    lv_obj_set_style_text_color(sl, lv_color_hex(wc_theme_primary()), 0);
    lv_obj_set_style_text_font(sl, &lv_font_montserrat_28, 0);
    lv_obj_align(sl, LV_ALIGN_TOP_MID, 0, 130);
    char bb[40]; snprintf(bb, sizeof(bb), is_best ? "NEW BEST!  %d" : "BEST  %d", best);
    lv_obj_t *bl = lv_label_create(s); lv_label_set_text(bl, bb);
    lv_obj_set_style_text_color(bl, lv_color_hex(is_best ? 0x39FF14 : wc_theme_accent()), 0);
    lv_obj_set_style_text_font(bl, &lv_font_montserrat_24, 0);
    lv_obj_align(bl, LV_ALIGN_TOP_MID, 0, 180);
    lv_obj_t *rb = lv_btn_create(s); lv_obj_set_size(rb, 220, 64);
    lv_obj_align(rb, LV_ALIGN_CENTER, 0, 40);
    lv_obj_set_style_bg_color(rb, lv_color_hex(wc_theme_accent()), 0); lv_obj_set_style_radius(rb, 12, 0);
    lv_obj_t *rl = lv_label_create(rb); lv_label_set_text(rl, "RETRY");
    lv_obj_set_style_text_font(rl, &lv_font_montserrat_24, 0); lv_obj_center(rl);
    lv_obj_add_event_cb(rb, go_btn_cb, LV_EVENT_CLICKED, (void*)(intptr_t)1);
    lv_obj_t *eb = lv_btn_create(s); lv_obj_set_size(eb, 220, 56);
    lv_obj_align(eb, LV_ALIGN_CENTER, 0, 120);
    lv_obj_set_style_bg_color(eb, lv_color_hex(0x222833), 0); lv_obj_set_style_radius(eb, 12, 0);
    lv_obj_t *el = lv_label_create(eb); lv_label_set_text(el, "MENU");
    lv_obj_set_style_text_font(el, &lv_font_montserrat_20, 0); lv_obj_center(el);
    lv_obj_add_event_cb(eb, go_btn_cb, LV_EVENT_CLICKED, (void*)(intptr_t)2);
    lv_scr_load(s);
    uint32_t hold = 0;
    while (g_go_result == 0) {
        wc_service(); esp_task_wdt_reset();
        if (digitalRead(WC_BOOT_BTN) == LOW) { if (!hold) hold = millis(); else if (millis()-hold > 800) g_go_result = 2; }
        else hold = 0;
        delay(5);
    }
    wc_park_screen();lv_obj_del(s);wc_input_reset();
    return g_go_result == 1;
}

// ===== v2.2 football draft games (inspired by 38-0-0.com) ====================
static const char *kFN[] = {
    "SILVA","KANE","MBAPPE","HAALAND","MODRIC","DE BRUYNE","VAN DIJK","SALAH",
    "MESSI","NEYMAR","SON","FODEN","RICE","SAKA","VINI JR","BELLINGHAM",
    "KROOS","RAMOS","ALISSON","EDERSON","CANCELO","MARTINEZ","GRIEZMANN","LEWA",
    "PEDRI","GAVI","OSIMHEN","RASHFORD","ODEGAARD","DIAS" };
static const int kFN_N = sizeof(kFN)/sizeof(kFN[0]);
static const char *kXIpos[11] = {"GK","LB","CB","CB","RB","CM","CM","CM","LW","ST","RW"};
static const char *kATTR[6]   = {"PACE","SHOOTING","PASSING","DRIBBLING","DEFENCE","PHYSICAL"};
static const char *rating_tier(int r) {
    if (r >= 90) return "38-0-0 INVINCIBLES";
    if (r >= 85) return "TITLE WINNERS";
    if (r >= 80) return "TOP FOUR";
    if (r >= 74) return "MID-TABLE";
    if (r >= 68) return "RELEGATION SCRAP";
    return "STRAIGHT DOWN";
}
struct DraftCfg { const char *title; const char *hsid; const char **slots; int nslots; bool players; };
static const DraftCfg kXICfg  = { "DRAFT XI",       "hs_xi",  kXIpos, 11, true  };
static const DraftCfg kPLCfg  = { "BUILD A PLAYER", "hs_ply", kATTR,  6,  false };
static const DraftCfg *g_dcfg = &kXICfg;

static lv_obj_t *g_dr_scr, *g_dr_head, *g_dr_sub, *g_dr_btn[3], *g_dr_total_lbl, *g_dr_again;
static int g_dr_round, g_dr_total, g_dr_cand[3];
static volatile int g_dr_pick = -1;
static bool g_dr_used[kFN_N];
static int g_dr_names[3];

static void dr_pick_cb(lv_event_t *e) { g_dr_pick = (int)(intptr_t)lv_event_get_user_data(e); }
static void dr_again_cb(lv_event_t *) { g_dr_pick = 99; }   // 99 = restart

static void dr_build_round() {
    // fresh 3 candidates for the current slot
    lv_obj_add_flag(g_dr_again, LV_OBJ_FLAG_HIDDEN);
    char h[40]; snprintf(h, sizeof(h), "%s  %d/%d", g_dcfg->title, g_dr_round+1, g_dcfg->nslots);
    lv_label_set_text(g_dr_head, h);
    char sub[40];
    if (g_dcfg->players) snprintf(sub, sizeof(sub), "PICK YOUR %s", g_dcfg->slots[g_dr_round]);
    else                 snprintf(sub, sizeof(sub), "DRAFT %s", g_dcfg->slots[g_dr_round]);
    lv_label_set_text(g_dr_sub, sub);
    for (int i = 0; i < 3; i++) {
        int rating = 62 + random(34);              // 62..95
        g_dr_cand[i] = rating;
        char b[48];
        if (g_dcfg->players){
            int name;bool duplicate;
            do{name=random(kFN_N);duplicate=g_dr_used[name];for(int j=0;j<i;j++)if(g_dr_names[j]==name)duplicate=true;}while(duplicate);
            g_dr_names[i]=name;snprintf(b,sizeof(b),"%s\nOVR %d",kFN[name],rating);
        }
        else                 snprintf(b, sizeof(b), "%s\n%d", g_dcfg->slots[g_dr_round], rating);
        lv_obj_t *lbl = lv_obj_get_child(g_dr_btn[i], 0);
        lv_label_set_text(lbl, b);
        lv_obj_clear_flag(g_dr_btn[i], LV_OBJ_FLAG_HIDDEN);
    }
}
static void dr_show_result() {
    int avg = g_dr_total / g_dcfg->nslots;
    int best = wc_hs_get(g_dcfg->hsid);
    bool nb = avg > best;
    if (nb) { best = avg; wc_hs_put(g_dcfg->hsid, best); }
    for (int i = 0; i < 3; i++) lv_obj_add_flag(g_dr_btn[i], LV_OBJ_FLAG_HIDDEN);
    char h[40]; snprintf(h, sizeof(h), g_dcfg->players ? "SQUAD RATING  %d" : "PLAYER OVR  %d", avg);
    lv_label_set_text(g_dr_head, h);
    char sub[64]; snprintf(sub, sizeof(sub), "%s\n%s %d", rating_tier(avg), nb ? "NEW BEST!" : "BEST", best);
    lv_label_set_text(g_dr_sub, sub);
    lv_obj_clear_flag(g_dr_again, LV_OBJ_FLAG_HIDDEN);
}
static void draft_init() {
    g_dr_round = 0; g_dr_total = 0; g_dr_pick = -1;
    memset(g_dr_used,0,sizeof(g_dr_used));
    g_dr_scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(g_dr_scr, lv_color_hex(0x061a0b), 0);
    lv_obj_clear_flag(g_dr_scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_scr_load(g_dr_scr);
    g_dr_head = lv_label_create(g_dr_scr);
    lv_obj_set_style_text_color(g_dr_head, lv_color_hex(0x9BD16A), 0);
    lv_obj_set_style_text_font(g_dr_head, &lv_font_montserrat_24, 0);
    lv_obj_align(g_dr_head, LV_ALIGN_TOP_MID, 0, 20);
    g_dr_sub = lv_label_create(g_dr_scr);
    lv_obj_set_style_text_color(g_dr_sub, lv_color_hex(0xE5B14F), 0);
    lv_obj_set_style_text_font(g_dr_sub, &lv_font_montserrat_18, 0);
    lv_obj_set_style_text_align(g_dr_sub, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(g_dr_sub, LV_ALIGN_TOP_MID, 0, 60);
    for (int i = 0; i < 3; i++) {
        lv_obj_t *b = lv_btn_create(g_dr_scr);
        lv_obj_set_size(b, 320, 90);
        lv_obj_align(b, LV_ALIGN_TOP_MID, 0, 120 + i*104);
        lv_obj_set_style_bg_color(b, lv_color_hex(0x123d1c), 0);
        lv_obj_set_style_border_color(b, lv_color_hex(0x9BD16A), 0);
        lv_obj_set_style_border_width(b, 2, 0);
        lv_obj_set_style_radius(b, 12, 0);
        lv_obj_t *lbl = lv_label_create(b);
        lv_obj_set_style_text_font(lbl, &lv_font_montserrat_22, 0);
        lv_obj_set_style_text_align(lbl, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_center(lbl);
        lv_obj_add_event_cb(b, dr_pick_cb, LV_EVENT_CLICKED, (void*)(intptr_t)i);
        g_dr_btn[i] = b;
    }
    g_dr_again = lv_btn_create(g_dr_scr);
    lv_obj_set_size(g_dr_again, 240, 70);
    lv_obj_align(g_dr_again, LV_ALIGN_BOTTOM_MID, 0, -30);
    lv_obj_set_style_bg_color(g_dr_again, lv_color_hex(0x9BD16A), 0);
    lv_obj_set_style_radius(g_dr_again, 12, 0);
    lv_obj_t *al = lv_label_create(g_dr_again); lv_label_set_text(al, "PLAY AGAIN");
    lv_obj_set_style_text_color(al, lv_color_hex(0x061a0b), 0);
    lv_obj_set_style_text_font(al, &lv_font_montserrat_22, 0); lv_obj_center(al);
    lv_obj_add_event_cb(g_dr_again, dr_again_cb, LV_EVENT_CLICKED, NULL);
    dr_build_round();
}
static void draft_loop() {
    if (g_dr_pick < 0) return;
    int p = g_dr_pick; g_dr_pick = -1;
    if (p == 99) { g_dr_round = 0; g_dr_total = 0; memset(g_dr_used,0,sizeof(g_dr_used));dr_build_round(); return; }
    if (p < 0 || p > 2) return;
    g_dr_total += g_dr_cand[p];
    if(g_dcfg->players)g_dr_used[g_dr_names[p]]=true;
    g_dr_round++;
    if (g_dr_round < g_dcfg->nslots) dr_build_round();
    else dr_show_result();
}
static void draft_exit() { if (g_dr_scr) { lv_obj_del(g_dr_scr); g_dr_scr = nullptr; } }
static void xi_init()     { g_dcfg = &kXICfg;  draft_init(); }
static void player_init() { g_dcfg = &kPLCfg;  draft_init(); }

// ---- Memory Match / Lights Out / Reaction Test ----------------------------
// Shared board storage is reused only after the previous game is destroyed.
static lv_obj_t *g_pz_scr=nullptr,*g_pz_status=nullptr,*g_pz_cells[16];
static int g_pz_pick=-1,g_pz_values[16],g_pz_moves=0;
static bool g_pz_matched[16];
static int g_mem_first=-1,g_mem_second=-1,g_mem_pairs=0;
static uint32_t g_mem_hide=0;
static int g_react_state=0,g_react_round=0,g_react_total=0;
static uint32_t g_react_due=0,g_react_go=0;
static lv_obj_t *puzzle_screen(const char *title,const char *help){
    g_pz_scr=lv_obj_create(NULL);lv_obj_set_style_bg_color(g_pz_scr,lv_color_hex(0x081018),0);
    lv_obj_clear_flag(g_pz_scr,LV_OBJ_FLAG_SCROLLABLE);lv_scr_load(g_pz_scr);
    lv_obj_t *l=lv_label_create(g_pz_scr);lv_label_set_text(l,title);
    lv_obj_set_style_text_font(l,&lv_font_montserrat_24,0);
    lv_obj_set_style_text_color(l,lv_color_hex(0x50E3FF),0);lv_obj_set_pos(l,20,16);
    l=lv_label_create(g_pz_scr);lv_label_set_text(l,help);lv_obj_set_width(l,370);
    lv_obj_set_style_text_color(l,lv_color_hex(0xEAF6FF),0);lv_obj_set_pos(l,20,60);
    g_pz_status=lv_label_create(g_pz_scr);lv_obj_set_style_text_color(g_pz_status,lv_color_hex(0xFFB300),0);
    lv_obj_align(g_pz_status,LV_ALIGN_BOTTOM_MID,0,-20);
    g_pz_pick=-1;g_pz_moves=0;
    return g_pz_scr;
}
static void puzzle_board(){
    for(int i=0;i<16;i++){
        lv_obj_t *b=lv_btn_create(g_pz_scr);lv_obj_set_size(b,82,72);lv_obj_set_pos(b,26+(i%4)*92,108+(i/4)*82);
        lv_obj_set_style_radius(b,12,0);
        lv_obj_t *l=lv_label_create(b);lv_label_set_text(l,"");lv_obj_center(l);
        lv_obj_set_style_text_font(l,&lv_font_montserrat_24,0);
        lv_obj_add_event_cb(b,[](lv_event_t *e){g_pz_pick=(int)(intptr_t)lv_event_get_user_data(e);},LV_EVENT_CLICKED,(void*)(intptr_t)i);
        g_pz_cells[i]=b;
    }
}
static void memory_draw(){
    for(int i=0;i<16;i++){
        bool show=g_pz_matched[i] || i==g_mem_first || i==g_mem_second;
        char text[8];snprintf(text,sizeof(text),"%d",g_pz_values[i]+1);
        lv_label_set_text(lv_obj_get_child(g_pz_cells[i],0),show?text:"?");
        lv_obj_set_style_bg_color(g_pz_cells[i],lv_color_hex(g_pz_matched[i]?0x236B42:show?0x65439B:0x20334B),0);
    }
    char status[48];snprintf(status,sizeof(status),"Pairs %d/8 | Turns %d",g_mem_pairs,g_pz_moves);lv_label_set_text(g_pz_status,status);
}
static void memory_init(){
    puzzle_screen("MEMORY","Match eight pairs. Fewer turns = more points.");puzzle_board();
    g_mem_first=g_mem_second=-1;g_mem_pairs=0;g_mem_hide=0;
    for(int i=0;i<16;i++){g_pz_values[i]=i/2;g_pz_matched[i]=false;}
    for(int i=15;i>0;i--){int j=random(i+1),v=g_pz_values[i];g_pz_values[i]=g_pz_values[j];g_pz_values[j]=v;}
    memory_draw();
}
static void memory_loop(){
    int pick=g_pz_pick;g_pz_pick=-1;
    if(g_mem_second>=0){
        if(g_play_ms-g_mem_hide>=700U){g_mem_first=g_mem_second=-1;memory_draw();}
        return;
    }
    if(pick<0 || pick>=16 || g_pz_matched[pick] || pick==g_mem_first)return;
    if(g_mem_first<0)g_mem_first=pick;
    else{
        g_mem_second=pick;g_pz_moves++;
        if(g_pz_values[pick]==g_pz_values[g_mem_first]){
            g_pz_matched[pick]=g_pz_matched[g_mem_first]=true;g_mem_pairs++;
            g_mem_first=g_mem_second=-1;
        }else g_mem_hide=g_play_ms;
    }
    memory_draw();
    if(g_mem_pairs==8)game_over("memory",max(100,2000-g_pz_moves*50));
}
static void lights_toggle(int cell){
    int row=cell/4,col=cell%4;
    for(int i=0;i<16;i++)if(abs(i/4-row)+abs(i%4-col)<=1)g_pz_values[i]^=1;
}
static void lights_draw(){
    for(int i=0;i<16;i++){
        lv_obj_set_style_bg_color(g_pz_cells[i],lv_color_hex(g_pz_values[i]?0xE5B14F:0x182435),0);
        lv_label_set_text(lv_obj_get_child(g_pz_cells[i],0),g_pz_values[i]?"+":".");
    }
    char status[40];snprintf(status,sizeof(status),"Moves %d | Turn every light off",g_pz_moves);lv_label_set_text(g_pz_status,status);
}
static void lights_init(){
    puzzle_screen("LIGHTS OUT","Tap flips a tile and its neighbours.");puzzle_board();
    for(int i=0;i<16;i++)g_pz_values[i]=0;
    // Scramble by legal moves from a solved board: every puzzle is solvable.
    for(int i=0;i<12;i++)lights_toggle(random(16));
    int on=0;for(int i=0;i<16;i++)on+=g_pz_values[i];if(!on)lights_toggle(5);
    lights_draw();
}
static void lights_loop(){
    int pick=g_pz_pick;g_pz_pick=-1;if(pick<0 || pick>=16)return;
    lights_toggle(pick);g_pz_moves++;lights_draw();
    int on=0;for(int i=0;i<16;i++)on+=g_pz_values[i];
    if(!on)game_over("lights",max(100,3000-g_pz_moves*50));
}
static void reaction_wait(){
    g_react_state=0;g_react_due=g_play_ms+1200U+(uint32_t)random(2800);
    lv_obj_set_style_bg_color(g_pz_cells[0],lv_color_hex(0x91394C),0);
    lv_label_set_text(lv_obj_get_child(g_pz_cells[0],0),"WAIT...");
}
static void reaction_init(){
    puzzle_screen("REACTION","Five rounds. Tap only when the pad is green.");
    g_pz_cells[0]=lv_btn_create(g_pz_scr);lv_obj_set_size(g_pz_cells[0],340,240);lv_obj_set_pos(g_pz_cells[0],35,130);
    lv_obj_t *l=lv_label_create(g_pz_cells[0]);lv_obj_set_style_text_font(l,&lv_font_montserrat_28,0);lv_obj_center(l);
    lv_obj_add_event_cb(g_pz_cells[0],[](lv_event_t*){g_pz_pick=0;},LV_EVENT_PRESSED,NULL);
    g_react_round=0;g_react_total=0;reaction_wait();lv_label_set_text(g_pz_status,"Round 1/5");
}
static void reaction_loop(){
    int pick=g_pz_pick;g_pz_pick=-1;
    if(g_react_state==2){if(g_play_ms-g_react_go>=1000U)reaction_wait();return;}
    if(pick==0){
        if(g_react_state==0){
            lv_label_set_text(g_pz_status,"Too soon! 1000 ms penalty.");g_react_total+=1000;
            g_react_state=2;g_react_go=g_play_ms;return;
        }
        int elapsed=(int)(g_play_ms-g_react_go);g_react_total+=elapsed;g_react_round++;
        char status[64];snprintf(status,sizeof(status),"%d ms | Completed %d/5",elapsed,g_react_round);lv_label_set_text(g_pz_status,status);
        if(g_react_round==5){game_over("reaction",max(0,5000-g_react_total/5));return;}
        g_react_state=2;g_react_go=g_play_ms;lv_label_set_text(lv_obj_get_child(g_pz_cells[0],0),"NICE!");return;
    }
    if(g_react_state==0 && (int32_t)(g_play_ms-g_react_due)>=0){
        g_react_state=1;g_react_go=g_play_ms;
        lv_obj_set_style_bg_color(g_pz_cells[0],lv_color_hex(0x247D45),0);
        lv_label_set_text(lv_obj_get_child(g_pz_cells[0],0),"TAP!");
    }
}
static void puzzle_exit(){if(g_pz_scr){lv_obj_del(g_pz_scr);g_pz_scr=nullptr;}}

// ---- grid launcher ----
static lv_obj_t *g_grid_scr = nullptr;
static volatile int g_pending_game = -1;   // set by tap_game, launched from loop()
static void tap_game(lv_event_t *e) {
    if (millis() - g_last_swipe_ms < WC_CLICK_AFTER_SWIPE_MS) return;
    // Defer the launch to loop(): wc_game_launch() runs a blocking game loop
    // that itself calls lv_timer_handler(). Doing that here (inside an LVGL
    // CLICKED callback, i.e. from within lv_timer_handler) would re-enter LVGL
    // recursively and can hang/crash the launch -- so just record the index.
    g_pending_game = (int)(intptr_t)lv_event_get_user_data(e);
}
void build_grid() {
    wc_park_screen();
    if (g_grid_scr) { lv_obj_del(g_grid_scr); g_grid_scr = nullptr; }
    lv_obj_t *s = lv_obj_create(NULL);
    lv_obj_clear_flag(s, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(s, lv_color_hex(wc_theme_bg()), 0);
    g_grid_scr = s;
    lv_obj_t *hdr = lv_label_create(s);
    char hbuf[40]; snprintf(hbuf, sizeof(hbuf), "GAME//OS 3.0 | %d games", wc_game_count());
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
        char caption[80];snprintf(caption,sizeof(caption),"%s\n%s\nBest %d",g->display_name,g->category,
            wc_hs_get(strcmp(g->id,"draftxi")==0?"hs_xi":strcmp(g->id,"buildpl")==0?"hs_ply":g->id));
        lv_label_set_text(lbl, caption);
        lv_obj_set_style_text_color(lbl, lv_color_hex(0xFFFFFF), 0);
        lv_obj_set_width(lbl,100);lv_label_set_long_mode(lbl,LV_LABEL_LONG_WRAP);
        lv_obj_set_style_text_align(lbl,LV_TEXT_ALIGN_CENTER,0);
        lv_obj_set_style_text_font(lbl, &lv_font_montserrat_14, 0);
        lv_obj_center(lbl);
        lv_obj_add_event_cb(tile, tap_game, LV_EVENT_CLICKED, (void*)(intptr_t)i);
    }
    lv_scr_load(s);wc_input_reset();
}

// ---- launcher-return error modal (issue #2 fix) ----
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
    lv_label_set_text(m, (g_launcher_return_msg && g_launcher_return_msg[0]) ? g_launcher_return_msg : "reason unknown");
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
    wc_park_screen();lv_obj_del(g_ret_err_scr); g_ret_err_scr = nullptr;
    build_grid();
}

// ---- 3-strike boot guard ----
static Preferences g_bootprefs;
static void wc_boot_guard_begin() {
    g_bootprefs.begin("gameos", false);
    uint8_t fails = g_bootprefs.getUChar("boot_fail", 0);
    fails++; g_bootprefs.putUChar("boot_fail", fails);
    Serial.printf("[boot] attempt fail-count=%u\n", fails);
    if (fails >= 3) {
        Serial.println("[boot] 3 failed boots -- falling back to LauncherOS");
        g_bootprefs.putUChar("boot_fail", 0);
        const esp_partition_t *factory = esp_partition_find_first(
            ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_FACTORY, NULL);
        if (factory) esp_ota_set_boot_partition(factory);
        delay(80); esp_restart();
    }
}
static void wc_boot_guard_success() {
    g_bootprefs.putUChar("boot_fail", 0);
    Serial.println("[boot] marked boot successful");
}

void setup() {
    esp_sleep_get_wakeup_cause();
    Serial.begin(115200);
    delay(50);
    wc_boot_guard_begin();
    wc_hw_boot();
    wc_theme_load();

    wc_boot_splash();                                 // radar splash (repo TacticalOS)
    wc_fade_brightness(WC_DEFAULT_BRIGHTNESS, 260);

    voyager_app_init();
    Serial.printf("[VoyagerOS] direct boot ready sd=%s\n", g_sd_ok ? "ok" : "MISSING");
    wc_boot_guard_success();
    randomSeed(esp_random());g_last_touch_ms=millis();

#if defined(POWER_SAVE) && POWER_SAVE
    setCpuFrequencyMhz(80);
#endif
}

void loop() {
    wc_service();
    static uint32_t batt=0;
    if(millis()-batt>=1000U){batt=millis();wc_pmu_refresh();}
    wc_boot_button_poll();
    voyager_app_loop();
    delay(3);
}


// =============================================================================
// VOYAGER 2.0 FEATURE APPLICATION
// The GameOS hardware/runtime above remains the sole board owner.
// =============================================================================
namespace VoyagerApp {
// =============================================================================
// 01. BUILD CONFIGURATION
// =============================================================================
#define VOYAGER_VERSION             "2.1.0"
#define VOYAGER_SAVE_VERSION        1
#define VOYAGER_POWER_SAVE          1
#define VOYAGER_AUDIO               1
#define VOYAGER_SCREEN_TIMEOUT_MS   15000UL
#define VOYAGER_WAKE_GRACE_MS       2000UL
#define VOYAGER_BOOT_HOLD_MS        1500UL
#define VOYAGER_LIGHT_SLEEP_POLL_MS 250UL
#define VOYAGER_DEFAULT_BRIGHTNESS  140
#define VOYAGER_MIN_FREE_BYTES      (8ULL * 1024ULL * 1024ULL)
#define VOYAGER_CRITICAL_FREE_BYTES (2ULL * 1024ULL * 1024ULL)

// =============================================================================
// 02. HARDWARE DEFINITIONS
// =============================================================================
#define LCD_WIDTH 410
#define LCD_HEIGHT 502
#define LCD_SDIO0 4
#define LCD_SDIO1 5
#define LCD_SDIO2 6
#define LCD_SDIO3 7
#define LCD_RESET 8
#define TP_RESET 9
#define PWR_BTN_PIN 10
#define LCD_SCLK 11
#define LCD_CS 12
#define IIC_SCL 14
#define IIC_SDA 15
#define AUDIO_DOUT 40
#define SDMMC_CMD 1
#define SDMMC_CLK 2
#define SDMMC_DATA 3
#define TP_INT 38
#define AUDIO_BCLK 41
#define AUDIO_DIN 42
#define AUDIO_MCLK 16
#define AUDIO_LRCK 45
#define AUDIO_PA 46
#define BOOT_BTN 0
#define FT3168_DEVICE_ADDRESS 0x38

static const char *PATH_ROOT       = "/VOYAGER";
static const char *PATH_JOURNAL    = "/VOYAGER/JOURNAL";
static const char *PATH_VOICE      = "/VOYAGER/VOICE";
static const char *PATH_JOURNEYS   = "/VOYAGER/JOURNEYS";
static const char *PATH_GAMES      = "/VOYAGER/GAMES";
static const char *PATH_LOCKER     = "/VOYAGER/LOCKER";
static const char *PATH_RECOVERY   = "/VOYAGER/RECOVERY";

// =============================================================================
// 03. CONSTANTS / ENUMS
// =============================================================================
enum ScreenID : uint8_t {
  SCREEN_HOME, SCREEN_JOURNAL, SCREEN_RECORD, SCREEN_TIMELINE,
  SCREEN_DAILY,
  SCREEN_JOURNEY, SCREEN_COMPANION, SCREEN_QUESTS, SCREEN_CHINESE,
  SCREEN_CHINESE_LESSON, SCREEN_CHINESE_TRACE, SCREEN_CHINESE_EXERCISE,
  SCREEN_CHINESE_DICTIONARY, SCREEN_CHINESE_PROGRESS, SCREEN_KNOWLEDGE,
  SCREEN_KNOWLEDGE_QUIZ, SCREEN_JOURNEY_HISTORY, SCREEN_BARGAIN,
  SCREEN_CURRENCY, SCREEN_PHRASES, SCREEN_LOCKER, SCREEN_PACKING,
  SCREEN_GAMES, SCREEN_RPG, SCREEN_SPACE, SCREEN_KINGDOM,
  SCREEN_CREATURES, SCREEN_HEALTH, SCREEN_SETTINGS, SCREEN_TIME,
  SCREEN_STORAGE, SCREEN_DIAGNOSTICS, SCREEN_PASS, SCREEN_TORCH, SCREEN_EMERGENCY,
  SCREEN_SAFE_MODE, SCREEN_COUNT
};

enum SleepLock : uint8_t { LOCK_NONE=0, LOCK_RECORDING=1, LOCK_TORCH=2, LOCK_GAME=4 };
enum Tone : uint8_t { TONE_1=1, TONE_2, TONE_3, TONE_4, TONE_NEUTRAL };
enum QuestKind : uint8_t { QUEST_JOURNAL, QUEST_CHINESE, QUEST_STEPS, QUEST_EXPLORE };

// =============================================================================
// 04. DATA STRUCTURES -- all types precede all globals that use them
// =============================================================================
struct SaveHeader {
  uint32_t magic;
  uint16_t version;
  uint16_t payloadSize;
  uint32_t checksum;
};

struct JourneyState {
  char name[32];
  char country[24];
  uint32_t startEpoch;
  uint16_t plannedDays;
  bool active;
};

struct CompanionState {
  char name[16];
  uint16_t xp;
  uint8_t level;
  uint8_t mood;
  uint16_t memories;
  uint8_t energy;
  uint16_t knowledge;
  uint16_t exploration;
  uint16_t adventure;
  uint16_t strategy;
  uint8_t evolutionStage;
  uint32_t lastInteraction;
};

struct ChineseWord {
  const char *hanzi;
  const char *pinyin;
  const char *sound;
  const char *english;
  uint8_t tone;
  const char *numberedPinyin=nullptr;
  uint8_t category=0;
  uint16_t frequencyRank=0;
};

struct SrsCard {
  uint16_t wordIndex;
  uint8_t box;
  uint16_t correct;
  uint16_t wrong;
  uint32_t dueEpoch;
};

struct JournalDay {
  uint32_t date;
  uint8_t mood;
  uint16_t activityFlags;
  uint32_t steps;
  uint16_t voiceCount;
  uint16_t noteCount;
  uint16_t questsCompleted;
  uint16_t chineseReviews;
  uint16_t gameMinutes;
  bool completed;
};

struct CurrencyRate { char code[4]; float rate; uint32_t lastSetEpoch; };

struct QuestState {
  QuestKind kind;
  const char *title;
  uint16_t target;
  uint16_t progress;
  bool claimed;
};

struct GameSave {
  uint32_t coins;
  uint16_t level;
  uint16_t energy;
  uint16_t score;
};

struct TinyQuestState {uint16_t hp,maxHp,xp,gold;uint8_t level,location,weapon,armour,potions,enemyHp,bosses;uint16_t questProgress;};
struct SpaceTraderState {uint32_t credits;uint16_t fuel,cargoFood,cargoTech,cargoOre,reputation;uint8_t planet,ship,cargoCapacity,missions,piratesDefeated;uint32_t marketSeed;};
struct KingdomState {uint32_t food,wood,stone,gold,population,army;uint8_t happiness,farm,lumberyard,quarry,barracks,castle;uint32_t lastTick;};
struct CreatureState {uint32_t discovered,captured;uint16_t xp,wins,stepsBank;uint8_t activeSpecies,level,hp,maxHp,attack,defence,bait,evolution;};

struct HealthState {
  uint32_t bootCount;
  uint32_t recoveredRecordings;
  uint32_t lastError;
  uint32_t lifetimeJournalEntries;
};

struct NavState {
  ScreenID stack[12];
  uint8_t depth;
  ScreenID current;
};

// =============================================================================
// 05. STATIC CONTENT
// =============================================================================
static const ChineseWord WORDS[] = {
  {"你好","nǐ hǎo","nee how","hello",3}, {"谢谢","xiè xie","shyeh shyeh","thank you",4},
  {"请","qǐng","cheeng","please",3}, {"多少钱","duō shǎo qián","dwaw shaow chyen","how much",2},
  {"太贵了","tài guì le","tie gway luh","too expensive",4}, {"便宜一点","pián yi yì diǎn","pyen yee ee dyen","a little cheaper",2},
  {"不要","bú yào","boo yaow","do not want",2}, {"可以","kě yǐ","kuh yee","can / okay",3},
  {"水","shuǐ","shway","water",3}, {"厕所","cè suǒ","tsuh swaw","toilet",4},
  {"医院","yī yuàn","ee ywen","hospital",1}, {"帮助","bāng zhù","bahng joo","help",1},
  {"我迷路了","wǒ mí lù le","waw mee loo luh","I am lost",3}, {"在哪里","zài nǎ lǐ","dzai nah lee","where is",4},
  {"火车站","huǒ chē zhàn","hwaw chuh jan","train station",3}, {"机场","jī chǎng","jee chahng","airport",1},
  {"酒店","jiǔ diàn","jyo dyen","hotel",3}, {"菜单","cài dān","tsai dahn","menu",4},
  {"好吃","hǎo chī","how chir","delicious",3}, {"不辣","bú là","boo lah","not spicy",2},
  {"一","yī","ee","one",1}, {"二","èr","ar","two",4}, {"三","sān","sahn","three",1},
  {"四","sì","suh","four",4}, {"五","wǔ","woo","five",3}, {"六","liù","lyoh","six",4},
  {"七","qī","chee","seven",1}, {"八","bā","bah","eight",1}, {"九","jiǔ","jyoh","nine",3},
  {"十","shí","shir","ten",2}, {"今天","jīn tiān","jeen tyen","today",1},
  {"明天","míng tiān","meeng tyen","tomorrow",2}, {"朋友","péng you","pung yoh","friend",2},
  {"旅行","lǚ xíng","lyoo sheeng","travel",3}, {"安全","ān quán","ahn chwen","safe",1},
  {"现金","xiàn jīn","shyen jeen","cash",4}, {"刷卡","shuā kǎ","shwah kah","pay by card",1},
  {"左边","zuǒ biān","dzwaw byen","left",3}, {"右边","yòu biān","yoh byen","right",4},
  {"直走","zhí zǒu","jir dzoh","go straight",2}
};
static const uint16_t WORD_COUNT = sizeof(WORDS)/sizeof(WORDS[0]);

static const char *KNOWLEDGE_TITLES[] = {
  "Cambodia","China","Temple etiquette","Street food safety","Tuk-tuks",
  "Tone basics","Travel scams","Offline navigation","Jet lag","Local markets"
};
static const char *KNOWLEDGE_TEXT[] = {
  "Cambodia's capital is Phnom Penh. Khmer is the official language and the riel is used alongside US dollars.",
  "China spans one official time zone. Mandarin uses characters and four main tones plus a neutral tone.",
  "Cover shoulders and knees at active temples. Remove shoes where requested and avoid touching sacred objects.",
  "Choose busy stalls, freshly cooked hot food and sealed water. Wash hands before eating.",
  "Agree the fare and destination before leaving. Keep small notes and confirm whether the price is per person.",
  "Tone changes meaning. First is high, second rises, third dips, fourth falls sharply, and neutral is light.",
  "Pause before paying, verify tickets and change, and reject urgency. Keep passport and cards separated.",
  "Save maps and addresses before departure. Carry the destination written in the local language.",
  "Use daylight, food and movement to shift your clock. Avoid a long late-afternoon nap.",
  "Start below your true maximum, stay friendly and be willing to walk away. Never bargain over essentials after agreeing."
};
static const uint8_t KNOWLEDGE_COUNT = 10;

static const char *PHRASE_LOCAL[] = {"សួស្តី","អរគុណ","តម្លៃប៉ុន្មាន?","ថ្លៃពេក","បង្គន់នៅឯណា?","ជួយខ្ញុំផង","ខ្ញុំវង្វេងផ្លូវ"};
static const char *PHRASE_SOUND[] = {"suo-s'dei","aw-kun","tlai pon-man","tlai pek","bong-kon nov ae-na","chuoy khnyom pong","khnyom vong-veng plov"};
static const char *PHRASE_EN[] = {"Hello","Thank you","How much?","Too expensive","Where is the toilet?","Please help me","I am lost"};
static const uint8_t PHRASE_COUNT = 7;
static const char *PHRASE_ZH[] = {"你好","谢谢","多少钱？","太贵了","厕所在哪里？","请帮助我","我迷路了"};
static const char *PHRASE_ZH_SOUND[] = {"nee how","shyeh shyeh","dwaw shaow chyen","tie gway luh","tsuh swaw dzai nah lee","cheeng bahng joo waw","waw mee loo luh"};
static const char *PHRASE_JA[] = {"こんにちは","ありがとう","いくらですか？","高すぎます","トイレはどこ？","助けてください","道に迷いました"};
static const char *PHRASE_JA_SOUND[] = {"kon-nee-chee-wa","ah-ree-gah-toh","ee-koo-rah des-kah","tah-kah-soo-gee-mas","toy-reh wa doh-ko","tah-soo-keh-teh","mee-chee nee mah-yoy-mah-shta"};

// =============================================================================
// =============================================================================
// 06. VOYAGER APPLICATION STATE -- no duplicate display/touch/PMU runtime
// =============================================================================
static std::unique_ptr<SensorPCF85063> g_rtc;
static std::unique_ptr<SensorQMI8658> g_imu;
static std::unique_ptr<I2SClass> g_i2s;
static Preferences g_prefs;

static lv_obj_t *g_screen = nullptr;
static lv_obj_t *g_header_time = nullptr;
static lv_obj_t *g_status = nullptr;

static bool g_pmu_ok=false, g_rtc_ok=false, g_audio_ok=false, g_imu_ok=false;
static bool g_safe_mode=false;
static bool g_recording=false, g_record_paused=false;
static uint8_t g_sleep_locks=LOCK_NONE, g_brightness=VOYAGER_DEFAULT_BRIGHTNESS, g_current_brightness=WC_DEFAULT_BRIGHTNESS;
static uint8_t g_power_profile=0;
static uint32_t g_screen_timeout_ms=VOYAGER_SCREEN_TIMEOUT_MS;
static uint32_t g_last_touch_ms=0, g_last_wake_ms=0, g_record_started=0;
static uint32_t g_record_paused_ms=0, g_record_pause_started=0;
static uint16_t g_record_bookmarks=0;
static uint8_t g_audio_level=0, g_home_page=0;
static uint32_t g_last_save_ms=0, g_last_battery_ms=0;
static int g_battery=-1; static bool g_charging=false;
static int32_t g_trace_x=-100,g_trace_y=-100;static uint8_t g_trace_dots=0;
static File g_record_file;
static char g_record_path[96]={0}, g_record_final_path[96]={0};
static NavState g_nav={{SCREEN_HOME},1,SCREEN_HOME};
static JourneyState g_journey={"My Journey","Destination",0,14,false};
static CompanionState g_companion={"Mochi",0,1,75,0,80,0,0,0,0,0,0};
static HealthState g_health={0,0,0,0};
static SrsCard g_srs[WORD_COUNT];
static GameSave g_games[4]={{100,1,100,0},{250,1,100,0},{80,1,100,0},{20,1,100,0}};
static TinyQuestState g_rpg={30,30,0,40,1,0,1,0,2,0,0,0};
static SpaceTraderState g_space={500,100,0,0,0,0,0,1,12,0,0,0x51A7E123UL};
static KingdomState g_kingdom={120,80,40,60,12,0,70,1,1,0,0,0,0};
static CreatureState g_creatures={1,1,0,0,0,0,1,20,20,5,3,3,0};
static JournalDay g_today={0,3,0,0,0,0,0,0,0,false};
static uint16_t g_packing_mask=0;
static float g_currency_rate=4100.0f;
static CurrencyRate g_rates[]={{"USD",1.0f,0},{"KHR",4100.0f,0},{"CNY",7.20f,0},{"JPY",150.0f,0},{"SGD",1.35f,0},{"THB",36.0f,0},{"AUD",1.52f,0}};
static const uint8_t RATE_COUNT=sizeof(g_rates)/sizeof(g_rates[0]);
static uint16_t g_chinese_index=0;
static int g_selected_knowledge=0;
static uint8_t g_phrase_language=0, g_quiz_answer=0, g_exercise_mode=0;
static uint32_t g_knowledge_favorites=0;
static uint8_t g_quest_claimed=0;
static uint32_t g_daily_key=0;
static bool g_initialized=false;
static bool g_optional_done=false;
static uint32_t g_app_started_ms=0;

static void fade_to(uint8_t target,uint16_t duration_ms){
  int from=g_current_brightness;const int steps=10;
  for(int i=1;i<=steps;i++){int value=from+(target-from)*i/steps;::g_gfx->setBrightness((uint8_t)value);g_current_brightness=(uint8_t)value;lv_timer_handler();delay(max(1,(int)duration_ms/steps));}
}

// =============================================================================
// 07. FORWARD DECLARATIONS
// =============================================================================
static void build_screen(ScreenID id);
static void nav_open(ScreenID id, bool push=true);
static void nav_back();
static void service_audio();
static void service_header();
static void save_all();
static void load_all();
static void show_toast(const char *msg);
static void launcher_cb(lv_event_t*);
static void home_cb(lv_event_t*){g_nav.depth=1;g_nav.stack[0]=SCREEN_HOME;g_nav.current=SCREEN_HOME;build_screen(SCREEN_HOME);}

// 08. UTILITY / STORAGE
// =============================================================================
static uint32_t checksum32(const uint8_t *p,size_t n){uint32_t c=2166136261UL;while(n--){c^=*p++;c*=16777619UL;}return c;}
static uint32_t now_epoch(){
  struct tm t={};
  if(getLocalTime(&t,20))return (uint32_t)mktime(&t);
  return millis()/1000;
}
static void date_key(char *out,size_t n){
  struct tm t={}; if(getLocalTime(&t,20))strftime(out,n,"%Y-%m-%d",&t); else snprintf(out,n,"DAY-%lu",millis()/86400000UL);
}
static void time_key(char *out,size_t n){
  struct tm t={}; if(getLocalTime(&t,20))strftime(out,n,"%H%M%S",&t); else snprintf(out,n,"%08lu",millis()/1000UL);
}
static bool ensure_dir(const char *p){if(!g_sd_ok)return false;if(SD_MMC.exists(p))return true;return SD_MMC.mkdir(p);}
static bool append_line(const char *path,const String &line){
  if(!g_sd_ok)return false;File f=SD_MMC.open(path,FILE_APPEND);if(!f)return false;f.println(line);f.flush();f.close();return true;
}
static bool save_blob(const char *path,uint32_t magic,const void *payload,uint16_t size){
  if(!g_sd_ok)return false;File f=SD_MMC.open(path,FILE_WRITE);if(!f)return false;SaveHeader h={magic,VOYAGER_SAVE_VERSION,size,checksum32((const uint8_t*)payload,size)};bool ok=f.write((const uint8_t*)&h,sizeof(h))==sizeof(h)&&f.write((const uint8_t*)payload,size)==size;f.flush();f.close();return ok;
}
static bool load_blob(const char *path,uint32_t magic,void *payload,uint16_t size){
  if(!g_sd_ok)return false;File f=SD_MMC.open(path,FILE_READ);if(!f)return false;SaveHeader h={};bool ok=f.read((uint8_t*)&h,sizeof(h))==sizeof(h)&&h.magic==magic&&h.version<=VOYAGER_SAVE_VERSION&&h.payloadSize==size&&f.read((uint8_t*)payload,size)==size&&h.checksum==checksum32((const uint8_t*)payload,size);f.close();return ok;
}
static void storage_init(){
  if(!::g_sd_ok)return;
  ensure_dir(PATH_ROOT);ensure_dir(PATH_JOURNAL);ensure_dir(PATH_VOICE);ensure_dir(PATH_JOURNEYS);
  ensure_dir(PATH_GAMES);ensure_dir(PATH_LOCKER);ensure_dir(PATH_RECOVERY);
}
static void save_all(){
  g_prefs.begin("voyager",false);
  g_prefs.putBytes("journey",&g_journey,sizeof(g_journey));
  g_prefs.putBytes("companion",&g_companion,sizeof(g_companion));
  g_prefs.putBytes("health",&g_health,sizeof(g_health));
  g_prefs.putBytes("srs",g_srs,sizeof(g_srs));g_prefs.putBytes("games",g_games,sizeof(g_games));
  g_prefs.putBytes("rpg2",&g_rpg,sizeof(g_rpg));g_prefs.putBytes("space2",&g_space,sizeof(g_space));g_prefs.putBytes("king2",&g_kingdom,sizeof(g_kingdom));g_prefs.putBytes("creature2",&g_creatures,sizeof(g_creatures));
  g_prefs.putFloat("rate",g_currency_rate);g_prefs.putBytes("rates",g_rates,sizeof(g_rates));g_prefs.putUInt("favorites",g_knowledge_favorites);g_prefs.putUChar("quests",g_quest_claimed);g_prefs.putUInt("daykey",g_daily_key);g_prefs.putUChar("bright",g_brightness);g_prefs.putUInt("timeout",g_screen_timeout_ms);g_prefs.putUChar("profile",g_power_profile);g_prefs.putUShort("packing",g_packing_mask);g_prefs.putBytes("today",&g_today,sizeof(g_today));g_prefs.end();
  if(g_sd_ok){for(uint8_t i=0;i<4;i++){char p[48];snprintf(p,sizeof(p),"%s/GAME%u.DAT",PATH_GAMES,i);save_blob(p,0x5647414DUL+i,&g_games[i],sizeof(GameSave));}save_blob("/VOYAGER/GAMES/RPG.DAT",0x52504732UL,&g_rpg,sizeof(g_rpg));save_blob("/VOYAGER/GAMES/SPACE.DAT",0x53504332UL,&g_space,sizeof(g_space));save_blob("/VOYAGER/GAMES/KINGDOM.DAT",0x4B4E4732UL,&g_kingdom,sizeof(g_kingdom));save_blob("/VOYAGER/GAMES/CREATURE.DAT",0x43525432UL,&g_creatures,sizeof(g_creatures));}g_last_save_ms=millis();
}
static void load_all(){
  g_prefs.begin("voyager",false);
  if(g_prefs.getBytesLength("journey")==sizeof(g_journey))g_prefs.getBytes("journey",&g_journey,sizeof(g_journey));
  if(g_prefs.getBytesLength("companion")==sizeof(g_companion))g_prefs.getBytes("companion",&g_companion,sizeof(g_companion));
  if(g_prefs.getBytesLength("health")==sizeof(g_health))g_prefs.getBytes("health",&g_health,sizeof(g_health));
  if(g_prefs.getBytesLength("srs")==sizeof(g_srs))g_prefs.getBytes("srs",g_srs,sizeof(g_srs));else for(uint16_t i=0;i<WORD_COUNT;i++){g_srs[i]={i,0,0,0,0};}
  if(g_prefs.getBytesLength("games")==sizeof(g_games))g_prefs.getBytes("games",g_games,sizeof(g_games));
  if(g_prefs.getBytesLength("rpg2")==sizeof(g_rpg))g_prefs.getBytes("rpg2",&g_rpg,sizeof(g_rpg));if(g_prefs.getBytesLength("space2")==sizeof(g_space))g_prefs.getBytes("space2",&g_space,sizeof(g_space));if(g_prefs.getBytesLength("king2")==sizeof(g_kingdom))g_prefs.getBytes("king2",&g_kingdom,sizeof(g_kingdom));if(g_prefs.getBytesLength("creature2")==sizeof(g_creatures))g_prefs.getBytes("creature2",&g_creatures,sizeof(g_creatures));
  g_currency_rate=g_prefs.getFloat("rate",4100.0f);if(g_prefs.getBytesLength("rates")==sizeof(g_rates))g_prefs.getBytes("rates",g_rates,sizeof(g_rates));g_rates[1].rate=g_currency_rate;g_knowledge_favorites=g_prefs.getUInt("favorites",0);g_quest_claimed=g_prefs.getUChar("quests",0);g_daily_key=g_prefs.getUInt("daykey",0);g_brightness=g_prefs.getUChar("bright",VOYAGER_DEFAULT_BRIGHTNESS);g_screen_timeout_ms=g_prefs.getUInt("timeout",VOYAGER_SCREEN_TIMEOUT_MS);g_power_profile=g_prefs.getUChar("profile",0);g_packing_mask=g_prefs.getUShort("packing",0);if(g_prefs.getBytesLength("today")==sizeof(g_today))g_prefs.getBytes("today",&g_today,sizeof(g_today));
  uint32_t today=now_epoch()/86400UL;if(today!=g_daily_key){g_daily_key=today;g_quest_claimed=0;g_today={today,3,0,0,0,0,0,0,0,false};}
  g_health.bootCount++;g_prefs.end();if(g_sd_ok){for(uint8_t i=0;i<4;i++){char p[48];snprintf(p,sizeof(p),"%s/GAME%u.DAT",PATH_GAMES,i);GameSave tmp;if(load_blob(p,0x5647414DUL+i,&tmp,sizeof(tmp)))g_games[i]=tmp;}load_blob("/VOYAGER/GAMES/RPG.DAT",0x52504732UL,&g_rpg,sizeof(g_rpg));load_blob("/VOYAGER/GAMES/SPACE.DAT",0x53504332UL,&g_space,sizeof(g_space));load_blob("/VOYAGER/GAMES/KINGDOM.DAT",0x4B4E4732UL,&g_kingdom,sizeof(g_kingdom));load_blob("/VOYAGER/GAMES/CREATURE.DAT",0x43525432UL,&g_creatures,sizeof(g_creatures));}if(!g_kingdom.lastTick)g_kingdom.lastTick=now_epoch();uint32_t elapsed=min(21600UL,now_epoch()>g_kingdom.lastTick?now_epoch()-g_kingdom.lastTick:0UL);if(elapsed>=60){uint32_t turns=elapsed/60;g_kingdom.food+=turns*g_kingdom.farm;g_kingdom.wood+=turns*g_kingdom.lumberyard;g_kingdom.stone+=turns*g_kingdom.quarry;g_kingdom.gold+=turns*(1+g_kingdom.castle);g_kingdom.lastTick=now_epoch();}save_all();
}
static uint64_t free_sd(){return g_sd_ok?(SD_MMC.totalBytes()-SD_MMC.usedBytes()):0;}

// =============================================================================
// 10. RTC / AUDIO
// =============================================================================
static void rtc_init(){
  if(!g_rtc)g_rtc.reset(new SensorPCF85063());
  if(!g_rtc){g_rtc_ok=false;g_health.lastError=421;return;}
  g_rtc_ok=g_rtc->begin(Wire,IIC_SDA,IIC_SCL);
  if(!g_rtc_ok)return;
  auto dt=g_rtc->getDateTime();struct tm t={};t.tm_year=dt.getYear()-1900;t.tm_mon=dt.getMonth()-1;t.tm_mday=dt.getDay();t.tm_hour=dt.getHour();t.tm_min=dt.getMinute();t.tm_sec=dt.getSecond();
  time_t e=mktime(&t);if(e>1700000000){timeval tv={e,0};settimeofday(&tv,nullptr);}
}
static bool leap_year(int y){return (y%4==0&&y%100!=0)||(y%400==0);}
static bool valid_date(int y,int mo,int d){static const uint8_t md[]={31,28,31,30,31,30,31,31,30,31,30,31};if(y<2024||y>2099||mo<1||mo>12||d<1)return false;int limit=md[mo-1]+((mo==2&&leap_year(y))?1:0);return d<=limit;}
static bool rtc_write(int y,int mo,int d,int h,int mi){
  if(!g_rtc_ok||!valid_date(y,mo,d)||h<0||h>23||mi<0||mi>59)return false;g_rtc->setDateTime(y,mo,d,h,mi,0);delay(25);auto v=g_rtc->getDateTime();
  if(v.getYear()!=y||v.getMonth()!=mo||v.getDay()!=d||v.getHour()!=h||v.getMinute()!=mi)return false;struct tm t={};t.tm_year=y-1900;t.tm_mon=mo-1;t.tm_mday=d;t.tm_hour=h;t.tm_min=mi;time_t e=mktime(&t);timeval tv={e,0};settimeofday(&tv,nullptr);return true;
}
static void audio_init(){
#if VOYAGER_AUDIO
  if(!g_i2s)g_i2s.reset(new I2SClass());
  if(!g_i2s){g_audio_ok=false;g_health.lastError=423;return;}
  pinMode(AUDIO_PA,OUTPUT);digitalWrite(AUDIO_PA,HIGH);g_i2s->setPins(AUDIO_BCLK,AUDIO_LRCK,AUDIO_DOUT,AUDIO_DIN,AUDIO_MCLK);
  g_audio_ok=g_i2s->begin(I2S_MODE_STD,16000,I2S_DATA_BIT_WIDTH_16BIT,I2S_SLOT_MODE_STEREO,I2S_STD_SLOT_BOTH);
#endif
}
static void imu_init(){
  if(!g_imu)g_imu.reset(new SensorQMI8658());
  if(!g_imu){g_imu_ok=false;g_health.lastError=422;return;}
  g_imu_ok=g_imu->begin(Wire,QMI8658_L_SLAVE_ADDRESS,IIC_SDA,IIC_SCL);if(!g_imu_ok)return;
  g_imu->configAccelerometer(SensorQMI8658::ACC_RANGE_2G,SensorQMI8658::ACC_ODR_62_5Hz);g_imu->enableAccelerometer();
  g_imu->configPedometer(50,200,100,200,20,10,0,4);g_imu_ok=g_imu->enablePedometer(SensorQMI8658::INTERRUPT_PIN_1);
}
static void service_imu(){
  static uint32_t last=0;if(!g_imu_ok||millis()-last<2000)return;last=millis();g_imu->update();uint32_t steps=g_imu->getPedometerCounter();if(steps>=g_today.steps)g_today.steps=steps;
}
static bool recording_start(){
  if(!g_audio_ok)audio_init();
  if(!g_sd_ok||!g_audio_ok||g_recording||SD_MMC.totalBytes()==0||free_sd()*100ULL/SD_MMC.totalBytes()<5ULL)return false;char d[20],t[16];date_key(d,sizeof(d));time_key(t,sizeof(t));
  char dir[72];snprintf(dir,sizeof(dir),"%s/%s",PATH_VOICE,d);ensure_dir(dir);snprintf(g_record_path,sizeof(g_record_path),"%s/%s.tmp",dir,t);snprintf(g_record_final_path,sizeof(g_record_final_path),"%s/%s.raw",dir,t);
  g_record_file=SD_MMC.open(g_record_path,FILE_WRITE);if(!g_record_file)return false;
  char marker[96];snprintf(marker,sizeof(marker),"%s/active.rec",PATH_RECOVERY);File m=SD_MMC.open(marker,FILE_WRITE);if(m){m.println(g_record_path);m.close();}
  g_recording=true;g_record_paused=false;g_record_paused_ms=0;g_record_bookmarks=0;g_record_started=millis();g_sleep_locks|=LOCK_RECORDING;return true;
}
static void recording_stop(){
  if(!g_recording)return;if(g_record_paused)g_record_paused_ms+=millis()-g_record_pause_started;g_record_file.flush();g_record_file.close();g_recording=false;g_record_paused=false;g_sleep_locks&=~LOCK_RECORDING;
  bool valid=false;if(SD_MMC.exists(g_record_path)){File check=SD_MMC.open(g_record_path,FILE_READ);valid=check&&check.size()>0;check.close();}if(valid){SD_MMC.remove(g_record_final_path);valid=SD_MMC.rename(g_record_path,g_record_final_path);}if(!valid){g_health.lastError=102;strncpy(g_record_final_path,g_record_path,sizeof(g_record_final_path));}
  SD_MMC.remove("/VOYAGER/RECOVERY/active.rec");char d[20],path[72];date_key(d,sizeof(d));snprintf(path,sizeof(path),"%s/%s.txt",PATH_JOURNAL,d);
  append_line(path,String("VOICE|")+g_record_final_path+"|"+String((millis()-g_record_started-g_record_paused_ms)/1000)+"s|BOOKMARKS="+String(g_record_bookmarks));g_today.voiceCount++;g_health.lifetimeJournalEntries++;save_all();g_last_touch_ms=millis();
}
static void service_audio(){
  if(!g_recording||g_record_paused||!g_i2s)return;if(SD_MMC.totalBytes()&&free_sd()*100ULL/SD_MMC.totalBytes()<5ULL){g_health.lastError=103;recording_stop();return;}static uint8_t buf[2048];size_t n=g_i2s->readBytes((char*)buf,sizeof(buf));if(n){if(g_record_file.write(buf,n)!=n){g_health.lastError=101;recording_stop();}}
  if(n){int16_t *s=(int16_t*)buf;uint16_t peak=0;for(size_t i=0;i<n/2;i++){uint16_t a=abs((int)s[i]);if(a>peak)peak=a;}g_audio_level=(uint8_t)min(100,(int)(peak*100UL/32767UL));}
}
static void recover_recording(){
  if(!g_sd_ok||!SD_MMC.exists("/VOYAGER/RECOVERY/active.rec"))return;File f=SD_MMC.open("/VOYAGER/RECOVERY/active.rec",FILE_READ);String p=f.readStringUntil('\n');f.close();p.trim();
  if(p.length()&&SD_MMC.exists(p)){String recovered=p;recovered.replace(".tmp",".recovered.raw");SD_MMC.rename(p,recovered);g_health.recoveredRecordings++;}SD_MMC.remove("/VOYAGER/RECOVERY/active.rec");save_all();
}

// =============================================================================
// 11. UI HELPERS
// =============================================================================
static lv_obj_t *label(lv_obj_t *p,const char *txt,int x,int y,const lv_font_t *font=&lv_font_montserrat_16,uint32_t color=0xEAF2FF){
  lv_obj_t *o=lv_label_create(p);lv_label_set_text(o,txt);lv_obj_set_style_text_font(o,font,0);lv_obj_set_style_text_color(o,lv_color_hex(color),0);lv_obj_set_pos(o,x,y);return o;
}
static lv_obj_t *button(lv_obj_t *p,const char *txt,int x,int y,int w,int h,lv_event_cb_t cb,void *ud=nullptr,uint32_t color=0x173D52){
  lv_obj_t *b=lv_btn_create(p);lv_obj_set_pos(b,x,y);lv_obj_set_size(b,w,h);lv_obj_set_style_radius(b,16,0);lv_obj_set_style_bg_color(b,lv_color_hex(color),0);lv_obj_set_style_border_color(b,lv_color_hex(0x36D7FF),0);lv_obj_set_style_border_width(b,1,0);
  lv_obj_t *l=lv_label_create(b);lv_label_set_text(l,txt);lv_obj_set_style_text_font(l,&lv_font_montserrat_14,0);lv_obj_set_style_text_color(l,lv_color_hex(0xFFFFFF),0);lv_obj_center(l);if(cb)lv_obj_add_event_cb(b,cb,LV_EVENT_CLICKED,ud);return b;
}
static lv_obj_t *base_screen(const char *title){
  lv_obj_t *s=lv_obj_create(NULL);lv_obj_set_style_bg_color(s,lv_color_hex(0x071018),0);lv_obj_clear_flag(s,LV_OBJ_FLAG_SCROLLABLE);
  if(g_nav.depth>1)button(s,"<",10,12,48,42,[](lv_event_t*){nav_back();});label(s,title,70,18,&lv_font_montserrat_20,0xFFFFFF);
  g_header_time=label(s,"--:--",308,22,&lv_font_montserrat_14,0x67E8F9);button(s,"L",360,12,40,42,[](lv_event_t*){save_all();::wc_launcher_return_now();},nullptr,0x164E63);
  lv_obj_t *line=lv_obj_create(s);lv_obj_set_pos(line,14,65);lv_obj_set_size(line,382,2);lv_obj_set_style_bg_color(line,lv_color_hex(0x164E63),0);lv_obj_set_style_border_width(line,0,0);
  return s;
}
static void toast_close_cb(lv_event_t *e){lv_obj_t *b=lv_event_get_target(e);lv_obj_t *panel=lv_obj_get_parent(b);if(panel)lv_obj_del_async(panel);}
static void show_toast(const char *msg){
  if(!g_screen)return;lv_obj_t *m=lv_obj_create(g_screen);lv_obj_set_size(m,350,260);lv_obj_center(m);lv_obj_set_style_bg_color(m,lv_color_hex(0x111827),0);lv_obj_set_style_border_color(m,lv_color_hex(0x38BDF8),0);lv_obj_set_style_border_width(m,2,0);lv_obj_set_style_radius(m,18,0);lv_obj_move_foreground(m);lv_obj_t *t=label(m,"VOYAGER",12,8,&lv_font_montserrat_20,0x67E8F9);(void)t;lv_obj_t *body=label(m,msg,12,45,&lv_font_montserrat_14,0xF8FAFC);lv_obj_set_width(body,315);lv_label_set_long_mode(body,LV_LABEL_LONG_WRAP);button(m,"CLOSE",95,195,130,45,toast_close_cb,nullptr,0x334155);
}
static void nav_open(ScreenID id,bool push){
  if(push&&g_nav.depth<12)g_nav.stack[g_nav.depth++]=id;else if(!push)g_nav.stack[g_nav.depth-1]=id;g_nav.current=id;build_screen(id);
}
static void nav_back(){if(g_nav.depth<=1)return;if(g_nav.current==SCREEN_TORCH){g_sleep_locks&=~LOCK_TORCH;g_brightness=VOYAGER_DEFAULT_BRIGHTNESS;}if(g_nav.current>=SCREEN_RPG&&g_nav.current<=SCREEN_CREATURES)g_sleep_locks&=~LOCK_GAME;g_nav.depth--;g_nav.current=g_nav.stack[g_nav.depth-1];build_screen(g_nav.current);}
static void menu_cb(lv_event_t *e){nav_open((ScreenID)(intptr_t)lv_event_get_user_data(e));}
static void service_header(){
  static uint32_t last=0;if(millis()-last<1000)return;last=millis();if(g_header_time){char b[12];struct tm t={};if(getLocalTime(&t,10))strftime(b,sizeof(b),"%H:%M",&t);else strcpy(b,"--:--");lv_label_set_text(g_header_time,b);}if(g_recording&&g_status){uint32_t elapsed=millis()-g_record_started-g_record_paused_ms-(g_record_paused?millis()-g_record_pause_started:0);char d[28];snprintf(d,sizeof(d),"%02lu:%02lu  LEVEL %u%%",(unsigned long)(elapsed/60000UL),(unsigned long)(elapsed/1000UL%60UL),g_audio_level);lv_label_set_text(g_status,d);}
}

// =============================================================================
// 12. FEATURE SCREENS
// =============================================================================
static void build_home(){
  g_screen=base_screen("VOYAGER");char b[160];struct tm t={};char date[32]="OFFLINE";if(getLocalTime(&t,10))strftime(date,sizeof(date),"%a %d %b",&t);
  uint32_t day=g_journey.active?max(1UL,(now_epoch()-g_journey.startEpoch)/86400UL+1):0;
  snprintf(b,sizeof(b),"%s\n%s  %s\nDAY %lu / %u",date,g_journey.active?"TRAVELLING":"READY",g_journey.country,(unsigned long)day,g_journey.plannedDays);
  label(g_screen,b,22,84,&lv_font_montserrat_20,0xFFFFFF);
  if(g_home_page==0){button(g_screen,"Journal",18,185,116,70,menu_cb,(void*)SCREEN_JOURNAL,0x0F766E);button(g_screen,"Chinese",147,185,116,70,menu_cb,(void*)SCREEN_CHINESE,0x7C3AED);button(g_screen,"Travel",276,185,116,70,menu_cb,(void*)SCREEN_BARGAIN,0xB45309);button(g_screen,"Journey",18,270,116,70,menu_cb,(void*)SCREEN_JOURNEY);button(g_screen,"Companion",147,270,116,70,menu_cb,(void*)SCREEN_COMPANION);button(g_screen,"Games",276,270,116,70,menu_cb,(void*)SCREEN_GAMES);button(g_screen,"Knowledge",18,355,116,70,menu_cb,(void*)SCREEN_KNOWLEDGE);button(g_screen,"Health",147,355,116,70,menu_cb,(void*)SCREEN_HEALTH);button(g_screen,"Settings",276,355,116,70,menu_cb,(void*)SCREEN_SETTINGS);}
  else if(g_home_page==1){snprintf(b,sizeof(b),"TODAY\n%lu steps\n%u notes • %u voice\n%u Chinese reviews",(unsigned long)g_today.steps,g_today.noteCount,g_today.voiceCount,g_today.chineseReviews);label(g_screen,b,35,185,&lv_font_montserrat_20,0xD1FAE5);button(g_screen,"Today",35,350,160,58,menu_cb,(void*)SCREEN_DAILY,0x0F766E);button(g_screen,"Quests",215,350,160,58,menu_cb,(void*)SCREEN_QUESTS,0x6D28D9);}
  else{label(g_screen,"QUICK ACTIONS",120,165,&lv_font_montserrat_18,0xFDE68A);button(g_screen,"Record",20,205,115,55,menu_cb,(void*)SCREEN_RECORD,0x9F1239);button(g_screen,"Bargain",148,205,115,55,menu_cb,(void*)SCREEN_BARGAIN,0xB45309);button(g_screen,"Chinese",276,205,115,55,menu_cb,(void*)SCREEN_CHINESE_EXERCISE,0x7C3AED);button(g_screen,"Torch",20,275,115,55,menu_cb,(void*)SCREEN_TORCH,0xB45309);button(g_screen,"Random fact",148,275,115,55,menu_cb,(void*)SCREEN_KNOWLEDGE_QUIZ,0x155E75);button(g_screen,"Emergency",276,275,115,55,menu_cb,(void*)SCREEN_EMERGENCY,0x991B1B);button(g_screen,"BACK TO LAUNCHER",70,350,270,52,launcher_cb,nullptr,0x9F1239);}
  snprintf(b,sizeof(b),"%s   %s   %s",g_home_page==0?"●":"○",g_home_page==1?"●":"○",g_home_page==2?"●":"○");label(g_screen,b,170,438,&lv_font_montserrat_14,0x67E8F9);
  snprintf(b,sizeof(b),"BAT %s%d%%   SD %s   %s",g_charging?"+":"",g_battery,g_sd_ok?"OK":"NO",g_recording?"REC":"OFFLINE");label(g_screen,b,25,462,&lv_font_montserrat_12,0x7DD3FC);
}

static void journal_save_cb(lv_event_t *e){
  lv_obj_t *ta=(lv_obj_t*)lv_event_get_user_data(e);const char *txt=lv_textarea_get_text(ta);if(!txt||!*txt){show_toast("Write something first.");return;}
  char d[20],p[72],tm[12];date_key(d,sizeof(d));time_key(tm,sizeof(tm));snprintf(p,sizeof(p),"%s/%s.txt",PATH_JOURNAL,d);
  if(append_line(p,String("TEXT|")+tm+"|"+txt)){g_today.noteCount++;g_health.lifetimeJournalEntries++;g_companion.xp+=5;lv_textarea_set_text(ta,"");save_all();show_toast("Journal entry saved.");}else show_toast("Save failed: check SD card.");
}
static void build_journal(){
  g_screen=base_screen("Journal");lv_obj_t *ta=lv_textarea_create(g_screen);lv_obj_set_pos(ta,18,82);lv_obj_set_size(ta,374,190);lv_textarea_set_placeholder_text(ta,"What happened today?");lv_textarea_set_max_length(ta,220);
  button(g_screen,"Save entry",18,286,178,55,journal_save_cb,ta,0x0F766E);button(g_screen,"Voice",214,286,178,55,menu_cb,(void*)SCREEN_RECORD,0x9F1239);
  button(g_screen,"Today",18,355,112,55,menu_cb,(void*)SCREEN_DAILY);button(g_screen,"Timeline",148,355,112,55,menu_cb,(void*)SCREEN_TIMELINE);button(g_screen,"Prompts",278,355,112,55,[](lv_event_t*){show_toast("Best thing today?\nWhat surprised you?\nWhat made you laugh?\nWhat did you learn?\nWhat was difficult?\nWhat food did you try?\nWhere did you go?\nWho were you with?\nWhat should you remember?\nWhat would you change?\nTomorrow?");});
  label(g_screen,"Entries are grouped by date and stored on SD.",22,430,&lv_font_montserrat_12,0x94A3B8);
}
static void mood_cb(lv_event_t *e){g_today.mood=(intptr_t)lv_event_get_user_data(e);g_today.date=now_epoch()/86400UL;save_all();build_screen(SCREEN_DAILY);}
static void activity_cb(lv_event_t *e){uint8_t i=(intptr_t)lv_event_get_user_data(e);if(lv_obj_has_state(lv_event_get_target(e),LV_STATE_CHECKED))g_today.activityFlags|=(1U<<i);else g_today.activityFlags&=~(1U<<i);save_all();}
static void daily_done_cb(lv_event_t*){g_today.completed=true;g_companion.xp+=20;save_all();show_toast("Today saved. +20 XP");}
static void build_daily(){
  g_screen=base_screen("Today");label(g_screen,"HOW WAS TODAY?",95,88,&lv_font_montserrat_20,0xFFFFFF);const char *m[]={"AWFUL","LOW","OK","GOOD","GREAT"};
  for(int i=0;i<5;i++)button(g_screen,m[i],12+i*79,130,70,50,mood_cb,(void*)(intptr_t)(i+1),g_today.mood==i+1?0x047857:0x334155);
  label(g_screen,"TODAY I...",25,210,&lv_font_montserrat_18,0xFDE68A);const char *acts[]={"Explored","Travelled","Ate somewhere new","Learned","Relaxed"};for(int i=0;i<5;i++){lv_obj_t *c=lv_checkbox_create(g_screen);lv_checkbox_set_text(c,acts[i]);lv_obj_set_pos(c,28,246+i*36);if(g_today.activityFlags&(1U<<i))lv_obj_add_state(c,LV_STATE_CHECKED);lv_obj_add_event_cb(c,activity_cb,LV_EVENT_VALUE_CHANGED,(void*)(intptr_t)i);}
  button(g_screen,"SAVE DAY",110,430,190,52,daily_done_cb,nullptr,0x0F766E);
}
static void record_toggle_cb(lv_event_t*){if(g_recording){recording_stop();build_screen(SCREEN_RECORD);}else if(recording_start())build_screen(SCREEN_RECORD);else show_toast("Recorder unavailable. Check SD and audio.");}
static void record_pause_cb(lv_event_t*){if(!g_recording)return;if(g_record_paused){g_record_paused_ms+=millis()-g_record_pause_started;g_record_paused=false;}else{g_record_paused=true;g_record_pause_started=millis();}build_screen(SCREEN_RECORD);}
static void record_bookmark_cb(lv_event_t*){if(g_recording){g_record_bookmarks++;show_toast("Bookmark added.");}}
static void build_record(){
  g_screen=base_screen("Voice Journal");label(g_screen,g_recording?(g_record_paused?"PAUSED":"RECORDING"):"READY",110,105,&lv_font_montserrat_28,g_recording?0xFB7185:0x5EEAD4);
  char b[120];snprintf(b,sizeof(b),"16 kHz / 16-bit RAW\n%s",g_recording?g_record_path:(g_audio_ok?"Tap to begin":"Audio init failed"));label(g_screen,b,48,170,&lv_font_montserrat_14,0xCBD5E1);
  g_status=label(g_screen,"00:00",165,225,&lv_font_montserrat_20,0xFFFFFF);button(g_screen,g_recording?"STOP & SAVE":"START RECORDING",60,275,290,70,record_toggle_cb,nullptr,g_recording?0xBE123C:0x047857);
  if(g_recording){button(g_screen,g_record_paused?"RESUME":"PAUSE",60,355,135,52,record_pause_cb);button(g_screen,"BOOKMARK",215,355,135,52,record_bookmark_cb);}
  label(g_screen,"The screen stays awake while recording.\nA recovery marker protects interrupted files.",43,425,&lv_font_montserrat_12,0x94A3B8);
}
static void build_timeline(){
  g_screen=base_screen("Timeline");lv_obj_t *box=lv_obj_create(g_screen);lv_obj_set_pos(box,14,76);lv_obj_set_size(box,382,405);lv_obj_set_scroll_dir(box,LV_DIR_VER);
  char d[20],p[72];date_key(d,sizeof(d));snprintf(p,sizeof(p),"%s/%s.txt",PATH_JOURNAL,d);String all="TODAY\n\n";
  if(g_sd_ok){File f=SD_MMC.open(p,FILE_READ);if(f){while(f.available()&&all.length()<1800)all+=(char)f.read();f.close();}}if(all.length()<10)all+="No entries yet.";label(box,all.c_str(),8,8,&lv_font_montserrat_14,0xE2E8F0);
}

static void journey_toggle_cb(lv_event_t*){if(g_journey.active){if(g_sd_ok){char p[96],d[20];date_key(d,sizeof(d));snprintf(p,sizeof(p),"%s/%s-%s.capsule",PATH_JOURNEYS,d,g_journey.name);File f=SD_MMC.open(p,FILE_WRITE);if(f){uint32_t days=max(1UL,(now_epoch()-g_journey.startEpoch)/86400UL+1);f.printf("VOYAGER CAPSULE\n%s\n%s\nDays: %lu\nVoice: %u\nNotes: %u\nChinese reviews: %u\nQuests: %u\nCompanion XP: %u\n",g_journey.name,g_journey.country,(unsigned long)days,g_today.voiceCount,g_today.noteCount,g_today.chineseReviews,g_today.questsCompleted,g_companion.xp);f.close();}}g_journey.active=false;}else{g_journey.active=true;g_journey.startEpoch=now_epoch();g_companion.exploration+=5;}save_all();build_screen(SCREEN_JOURNEY);}
static void journey_save_cb(lv_event_t *e){lv_obj_t **a=(lv_obj_t**)lv_event_get_user_data(e);strncpy(g_journey.name,lv_textarea_get_text(a[0]),sizeof(g_journey.name)-1);g_journey.name[sizeof(g_journey.name)-1]=0;strncpy(g_journey.country,lv_textarea_get_text(a[1]),sizeof(g_journey.country)-1);g_journey.country[sizeof(g_journey.country)-1]=0;int days=atoi(lv_textarea_get_text(a[2]));g_journey.plannedDays=constrain(days,1,365);save_all();show_toast("Journey details saved.");}
static void build_journey(){
  g_screen=base_screen("Journey");char b[180];uint32_t day=g_journey.active?max(1UL,(now_epoch()-g_journey.startEpoch)/86400UL+1):0;
  snprintf(b,sizeof(b),"%s • %s\nDay %lu of %u • %s",g_journey.name,g_journey.country,(unsigned long)day,g_journey.plannedDays,g_journey.active?"ACTIVE":"READY");label(g_screen,b,25,82,&lv_font_montserrat_18,0xFFFFFF);
  static lv_obj_t *a[3];a[0]=lv_textarea_create(g_screen);lv_obj_set_pos(a[0],25,145);lv_obj_set_size(a[0],175,52);lv_textarea_set_one_line(a[0],true);lv_textarea_set_text(a[0],g_journey.name);a[1]=lv_textarea_create(g_screen);lv_obj_set_pos(a[1],215,145);lv_obj_set_size(a[1],170,52);lv_textarea_set_one_line(a[1],true);lv_textarea_set_text(a[1],g_journey.country);a[2]=lv_textarea_create(g_screen);lv_obj_set_pos(a[2],25,210);lv_obj_set_size(a[2],100,52);lv_textarea_set_one_line(a[2],true);lv_textarea_set_accepted_chars(a[2],"0123456789");char days[8];snprintf(days,sizeof(days),"%u",g_journey.plannedDays);lv_textarea_set_text(a[2],days);button(g_screen,"Save details",145,210,240,52,journey_save_cb,a);
  button(g_screen,g_journey.active?"Complete & make capsule":"Start journey",35,285,340,58,journey_toggle_cb,nullptr,g_journey.active?0x9F1239:0x047857);
  button(g_screen,"Packing",25,365,110,55,menu_cb,(void*)SCREEN_PACKING);button(g_screen,"Trip locker",150,365,110,55,menu_cb,(void*)SCREEN_LOCKER);button(g_screen,"History",275,365,110,55,menu_cb,(void*)SCREEN_JOURNEY_HISTORY);
}
static void build_journey_history(){g_screen=base_screen("Journey History");lv_obj_t *box=lv_obj_create(g_screen);lv_obj_set_pos(box,14,76);lv_obj_set_size(box,382,405);lv_obj_set_scroll_dir(box,LV_DIR_VER);String list="SAVED CAPSULES\n\n";if(g_sd_ok){File d=SD_MMC.open(PATH_JOURNEYS);if(d){File f=d.openNextFile();while(f&&list.length()<1800){if(!f.isDirectory()){list+=f.name();list+="  •  ";list+=String((unsigned long)f.size());list+=" bytes\n";}f=d.openNextFile();}d.close();}}if(list.length()<20)list+="No completed journeys yet.";lv_obj_t *l=label(box,list.c_str(),8,8,&lv_font_montserrat_14,0xE2E8F0);lv_obj_set_width(l,345);lv_label_set_long_mode(l,LV_LABEL_LONG_WRAP);}
static void build_companion(){
  g_screen=base_screen("Companion");char b[300];uint8_t level=max((int)g_companion.level,(int)(g_companion.xp/100+1));const char *form="TRAVELLER";uint16_t high=g_companion.knowledge;if(g_companion.adventure>high){high=g_companion.adventure;form="ADVENTURER";}if(g_companion.exploration>high){high=g_companion.exploration;form="EXPLORER";}if(g_companion.strategy>high)form="STRATEGIST";else if(g_companion.knowledge==high&&high>10)form="SCHOLAR";
  snprintf(b,sizeof(b),"       /\\_/\\\n      ( o.o )\n       > ^ <\n\n%s  •  Level %u\n%s FORM\nXP %u / 100\nMood %u%%  Energy %u%%\nMemories %u",g_companion.name,level,form,g_companion.xp%100,g_companion.mood,g_companion.energy,g_companion.memories);label(g_screen,b,72,85,&lv_font_montserrat_18,0xFDE68A);
  button(g_screen,"Quests",65,385,280,60,menu_cb,(void*)SCREEN_QUESTS,0x7C3AED);
}
static void quest_claim(lv_event_t *e){int i=(intptr_t)lv_event_get_user_data(e);if(g_quest_claimed&(1U<<i)){show_toast("Already claimed today.");return;}uint16_t progress[]={g_today.noteCount,g_today.chineseReviews,(uint16_t)min(65535UL,g_today.steps),(uint16_t)__builtin_popcount(g_today.activityFlags)};uint16_t target[]={1,5,1000,1};if(progress[i]<target[i]){show_toast("Quest not complete yet.");return;}g_quest_claimed|=(1U<<i);g_today.questsCompleted++;g_companion.xp+=10+i*5;g_companion.memories++;g_companion.adventure+=2;g_companion.lastInteraction=now_epoch();save_all();build_screen(SCREEN_QUESTS);}
static void build_quests(){
  g_screen=base_screen("Daily Quests");const char *q[]={"Write one journal entry","Review five Chinese words","Walk 1,000 steps","Log one activity"};uint16_t p[]={g_today.noteCount,g_today.chineseReviews,(uint16_t)min(65535UL,g_today.steps),(uint16_t)__builtin_popcount(g_today.activityFlags)};uint16_t t[]={1,5,1000,1};
  for(int i=0;i<4;i++){char b[100];snprintf(b,sizeof(b),"%s\n%u/%u  %s",q[i],p[i],t[i],g_quest_claimed&(1U<<i)?"CLAIMED":"CLAIM");button(g_screen,b,25,88+i*82,360,62,quest_claim,(void*)(intptr_t)i,p[i]>=t[i]?0x0F766E:0x4338CA);}
  char chain[90];snprintf(chain,sizeof(chain),"STORYTELLER CHAIN  %u memories\nI: 3  •  II: 10  •  III: 30",g_today.voiceCount);label(g_screen,chain,55,430,&lv_font_montserrat_12,0xFDE68A);
}

static void chinese_grade_cb(lv_event_t *e){
  int grade=(intptr_t)lv_event_get_user_data(e);SrsCard &c=g_srs[g_chinese_index];if(grade==0){c.box=0;c.wrong++;c.dueEpoch=now_epoch()+600UL;}else{c.box=min(5,(int)c.box+1);c.correct++;uint32_t delays[]={600UL,86400UL,259200UL,604800UL,1209600UL,2592000UL};c.dueEpoch=now_epoch()+delays[c.box];}g_today.chineseReviews++;g_companion.xp+=grade?3:1;g_companion.knowledge+=grade?2:1;g_companion.lastInteraction=now_epoch();save_all();g_chinese_index=(g_chinese_index+1)%WORD_COUNT;build_screen(SCREEN_CHINESE_LESSON);
}
static void build_chinese(){
  g_screen=base_screen("Chinese");uint16_t due=0;for(auto &c:g_srs)if(c.dueEpoch<=now_epoch())due++;
  char b[140];snprintf(b,sizeof(b),"%u travel words\n%u reviews due\nOffline • Hanzi • Pinyin • Sound",WORD_COUNT,due);label(g_screen,b,38,100,&lv_font_montserrat_20,0xDDD6FE);
  button(g_screen,"Start lesson",25,205,175,58,menu_cb,(void*)SCREEN_CHINESE_LESSON,0x6D28D9);button(g_screen,"Quick exercise",210,205,175,58,menu_cb,(void*)SCREEN_CHINESE_EXERCISE,0x7C3AED);
  button(g_screen,"Trace Hanzi",25,278,175,55,menu_cb,(void*)SCREEN_CHINESE_TRACE,0x4338CA);button(g_screen,"Dictionary",210,278,175,55,menu_cb,(void*)SCREEN_CHINESE_DICTIONARY,0x155E75);button(g_screen,"Progress",25,348,175,55,menu_cb,(void*)SCREEN_CHINESE_PROGRESS,0x0F766E);button(g_screen,"Phrasebook",210,348,175,55,menu_cb,(void*)SCREEN_PHRASES);
}
static void trace_next_cb(lv_event_t*){g_chinese_index=(g_chinese_index+1)%WORD_COUNT;build_screen(SCREEN_CHINESE_TRACE);}
static void build_chinese_trace(){g_screen=base_screen("Trace Hanzi");g_sleep_locks|=LOCK_GAME;g_trace_dots=0;g_trace_x=g_trace_y=-100;const ChineseWord &w=WORDS[g_chinese_index];lv_obj_t *c=label(g_screen,w.hanzi,120,105,&lv_font_montserrat_48,0x475569);lv_obj_set_style_text_opa(c,LV_OPA_60,0);label(g_screen,"Trace the reference with your finger.\nV1 provides practice without recognition.",38,220,&lv_font_montserrat_14,0xCBD5E1);button(g_screen,"CLEAR",45,350,140,60,[](lv_event_t*){build_screen(SCREEN_CHINESE_TRACE);});button(g_screen,"NEXT",225,350,140,60,trace_next_cb,nullptr,0x6D28D9);}
static void build_chinese_lesson(){
  g_screen=base_screen("Chinese Lesson");const ChineseWord &w=WORDS[g_chinese_index];char b[256];snprintf(b,sizeof(b),"%s\n\n%s\n%s\n\n%s\nTone %u • Box %u",w.hanzi,w.pinyin,w.sound,w.english,w.tone,g_srs[g_chinese_index].box);lv_obj_t *l=label(g_screen,b,0,95,&lv_font_montserrat_20,0xFFFFFF);lv_obj_set_width(l,410);lv_obj_set_style_text_align(l,LV_TEXT_ALIGN_CENTER,0);
  button(g_screen,"Again",24,365,105,58,chinese_grade_cb,(void*)0,0x9F1239);button(g_screen,"Good",152,365,105,58,chinese_grade_cb,(void*)1,0x047857);button(g_screen,"Easy",280,365,105,58,chinese_grade_cb,(void*)2,0x0369A1);
}
static void chinese_exercise_answer(lv_event_t *e){uint8_t a=(intptr_t)lv_event_get_user_data(e);SrsCard &c=g_srs[g_chinese_index];bool correct=a==g_quiz_answer;if(correct){c.box=min(5,(int)c.box+1);c.correct++;c.dueEpoch=now_epoch()+86400UL;}else{c.box=0;c.wrong++;c.dueEpoch=now_epoch()+600UL;}g_today.chineseReviews++;g_companion.knowledge+=correct?2:1;save_all();show_toast(correct?"Correct! SRS progress saved.":"Not quite. Card scheduled again.");}
static void build_chinese_exercise(){g_screen=base_screen("Chinese Exercise");g_quiz_answer=esp_random()%4;g_chinese_index=esp_random()%WORD_COUNT;g_exercise_mode=esp_random()%6;const ChineseWord &w=WORDS[g_chinese_index];const char *modes[]={"MEANING","HANZI RECOGNITION","PINYIN","ENGLISH TO CHINESE","TONE","SENTENCE ORDER"};char q[140];if(g_exercise_mode==0)snprintf(q,sizeof(q),"%s\nWhat does %s mean?",modes[g_exercise_mode],w.hanzi);else if(g_exercise_mode==1)snprintf(q,sizeof(q),"%s\nWhich means %s?",modes[g_exercise_mode],w.english);else if(g_exercise_mode==2)snprintf(q,sizeof(q),"%s\nChoose the pinyin for %s",modes[g_exercise_mode],w.hanzi);else if(g_exercise_mode==3)snprintf(q,sizeof(q),"%s\nChoose Chinese for '%s'",modes[g_exercise_mode],w.english);else if(g_exercise_mode==4)snprintf(q,sizeof(q),"%s\nWhat tone is %s?",modes[g_exercise_mode],w.pinyin);else snprintf(q,sizeof(q),"%s\nChoose a valid travel phrase",modes[g_exercise_mode]);label(g_screen,q,35,85,&lv_font_montserrat_18,0xDDD6FE);for(int i=0;i<4;i++){uint16_t wi=i==g_quiz_answer?g_chinese_index:(g_chinese_index+i+7)%WORD_COUNT;char option[90];if(g_exercise_mode==0)snprintf(option,sizeof(option),"%s",WORDS[wi].english);else if(g_exercise_mode==1||g_exercise_mode==3)snprintf(option,sizeof(option),"%s",WORDS[wi].hanzi);else if(g_exercise_mode==2)snprintf(option,sizeof(option),"%s",WORDS[wi].pinyin);else if(g_exercise_mode==4)snprintf(option,sizeof(option),"Tone %d",i+1);else snprintf(option,sizeof(option),i==g_quiz_answer?"我 → 喜欢 → 茶":"茶 → 我 → 喜欢");if(g_exercise_mode==4)g_quiz_answer=w.tone>4?0:w.tone-1;button(g_screen,option,35,185+i*65,340,50,chinese_exercise_answer,(void*)(intptr_t)i,0x4338CA);}}
static void build_chinese_dictionary(){g_screen=base_screen("Chinese Dictionary");lv_obj_t *box=lv_obj_create(g_screen);lv_obj_set_pos(box,12,76);lv_obj_set_size(box,386,410);lv_obj_set_flex_flow(box,LV_FLEX_FLOW_COLUMN);for(uint16_t i=0;i<WORD_COUNT;i++){char b[120];snprintf(b,sizeof(b),"%s  %s  —  %s",WORDS[i].hanzi,WORDS[i].pinyin,WORDS[i].english);lv_obj_t *l=label(box,b,0,0,&lv_font_montserrat_14,0xE2E8F0);lv_obj_set_width(l,345);}}
static void build_chinese_progress(){g_screen=base_screen("Chinese Progress");uint32_t correct=0,wrong=0;uint16_t learned=0,due=0;for(auto &c:g_srs){correct+=c.correct;wrong+=c.wrong;if(c.box)learned++;if(c.dueEpoch<=now_epoch())due++;}char b[260];snprintf(b,sizeof(b),"Words seen        %u / %u\nReviews due       %u\nCorrect answers   %lu\nRetry answers     %lu\nToday's reviews   %u\n\nKeep short daily sessions.\nBoxes increase review spacing.",learned,WORD_COUNT,due,(unsigned long)correct,(unsigned long)wrong,g_today.chineseReviews);label(g_screen,b,42,105,&lv_font_montserrat_18,0xD1FAE5);}

static void knowledge_pick(lv_event_t *e){g_selected_knowledge=(intptr_t)lv_event_get_user_data(e);show_toast(KNOWLEDGE_TEXT[g_selected_knowledge]);}
static void knowledge_favorite(lv_event_t*){g_knowledge_favorites^=(1UL<<g_selected_knowledge);save_all();show_toast(g_knowledge_favorites&(1UL<<g_selected_knowledge)?"Saved to favorites.":"Removed from favorites.");}
static void knowledge_search(lv_event_t *e){lv_obj_t *ta=(lv_obj_t*)lv_event_get_user_data(e);String q=lv_textarea_get_text(ta);q.toLowerCase();String hits="MATCHES\n";for(int i=0;i<KNOWLEDGE_COUNT;i++){String t=KNOWLEDGE_TITLES[i];String x=KNOWLEDGE_TEXT[i];t.toLowerCase();x.toLowerCase();if(!q.length()||t.indexOf(q)>=0||x.indexOf(q)>=0){hits+=KNOWLEDGE_TITLES[i];hits+="\n";}}show_toast(hits.c_str());}
static void build_knowledge(){
  g_screen=base_screen("Knowledge");lv_obj_t *search=lv_textarea_create(g_screen);lv_obj_set_pos(search,12,76);lv_obj_set_size(search,240,48);lv_textarea_set_one_line(search,true);lv_textarea_set_placeholder_text(search,"Search offline topics");button(g_screen,"Search",262,76,64,48,knowledge_search,search,0x155E75);button(g_screen,"Quiz",334,76,64,48,menu_cb,(void*)SCREEN_KNOWLEDGE_QUIZ,0x6D28D9);lv_obj_t *box=lv_obj_create(g_screen);lv_obj_set_pos(box,12,132);lv_obj_set_size(box,386,354);lv_obj_set_flex_flow(box,LV_FLEX_FLOW_COLUMN);lv_obj_set_style_pad_row(box,8,0);
  for(int i=0;i<KNOWLEDGE_COUNT;i++){char b[64];snprintf(b,sizeof(b),"%s%s",g_knowledge_favorites&(1UL<<i)?"★ ":"",KNOWLEDGE_TITLES[i]);lv_obj_t *o=button(box,b,0,0,350,52,knowledge_pick,(void*)(intptr_t)i);lv_obj_set_pos(o,0,0);}
}
static void knowledge_quiz_answer(lv_event_t *e){uint8_t a=(intptr_t)lv_event_get_user_data(e);show_toast(a==g_quiz_answer?"Correct!":"Try again, then open the topic to review.");if(a==g_quiz_answer){g_companion.knowledge+=2;save_all();}}
static void build_knowledge_quiz(){g_screen=base_screen("Knowledge Quiz");g_selected_knowledge=esp_random()%KNOWLEDGE_COUNT;g_quiz_answer=esp_random()%3;char q[120];snprintf(q,sizeof(q),"Which topic matches this fact?\n\n%.80s",KNOWLEDGE_TEXT[g_selected_knowledge]);label(g_screen,q,25,85,&lv_font_montserrat_16,0xE2E8F0);for(int i=0;i<3;i++){int k=i==g_quiz_answer?g_selected_knowledge:(g_selected_knowledge+i+3)%KNOWLEDGE_COUNT;button(g_screen,KNOWLEDGE_TITLES[k],35,265+i*62,340,50,knowledge_quiz_answer,(void*)(intptr_t)i,0x4338CA);}button(g_screen,"Favorite topic",110,450,190,38,knowledge_favorite,nullptr,0xB45309);}
static void bargain_calc(lv_event_t *e){
  lv_obj_t **a=(lv_obj_t**)lv_event_get_user_data(e);float ask=atof(lv_textarea_get_text(a[0]));float pct=atof(lv_textarea_get_text(a[1]));char b[160];snprintf(b,sizeof(b),"Start: %.0f\nTarget: %.0f\nWalk-away: %.0f",ask*pct/100.0f,ask*(pct+15)/100.0f,ask*(pct+25)/100.0f);lv_label_set_text(a[2],b);
}
static void build_bargain(){
  g_screen=base_screen("Bargaining");lv_obj_t *ask=lv_textarea_create(g_screen);lv_obj_set_pos(ask,25,95);lv_obj_set_size(ask,170,55);lv_textarea_set_one_line(ask,true);lv_textarea_set_accepted_chars(ask,"0123456789.");lv_textarea_set_placeholder_text(ask,"Asking price");
  lv_obj_t *pct=lv_textarea_create(g_screen);lv_obj_set_pos(pct,215,95);lv_obj_set_size(pct,170,55);lv_textarea_set_one_line(pct,true);lv_textarea_set_text(pct,"45");lv_textarea_set_accepted_chars(pct,"0123456789");
  lv_obj_t *out=label(g_screen,"Enter the asking price.",45,205,&lv_font_montserrat_20,0xFDE68A);static lv_obj_t *args[3];args[0]=ask;args[1]=pct;args[2]=out;button(g_screen,"Calculate",25,165,360,55,bargain_calc,args,0xB45309);
  button(g_screen,"Currency",25,360,170,55,menu_cb,(void*)SCREEN_CURRENCY);button(g_screen,"Phrases",215,360,170,55,menu_cb,(void*)SCREEN_PHRASES);
}
static void currency_calc(lv_event_t *e){lv_obj_t **a=(lv_obj_t**)lv_event_get_user_data(e);float amount=atof(lv_textarea_get_text(a[0]));uint8_t from=lv_dropdown_get_selected(a[2]),to=lv_dropdown_get_selected(a[3]);float result=amount/g_rates[from].rate*g_rates[to].rate;char b[100];snprintf(b,sizeof(b),"%.2f %s\n= %.2f %s",amount,g_rates[from].code,result,g_rates[to].code);lv_label_set_text(a[1],b);}
static void currency_rate_save(lv_event_t *e){lv_obj_t **a=(lv_obj_t**)lv_event_get_user_data(e);uint8_t i=lv_dropdown_get_selected(a[0]);float v=atof(lv_textarea_get_text(a[1]));if(i==0||v<=0){show_toast("Choose a non-USD currency and positive rate.");return;}g_rates[i].rate=v;g_rates[i].lastSetEpoch=now_epoch();g_currency_rate=g_rates[1].rate;save_all();show_toast("Offline rate saved. It is not live.");}
static void build_currency(){
  g_screen=base_screen("Currency");const char *opts="USD\nKHR\nCNY\nJPY\nSGD\nTHB\nAUD";label(g_screen,"OFFLINE RATES PER 1 USD",105,76,&lv_font_montserrat_14,0xFDE68A);lv_obj_t *rateCur=lv_dropdown_create(g_screen);lv_obj_set_pos(rateCur,25,105);lv_obj_set_size(rateCur,100,50);lv_dropdown_set_options(rateCur,opts);lv_dropdown_set_selected(rateCur,1);lv_obj_t *rate=lv_textarea_create(g_screen);lv_obj_set_pos(rate,135,105);lv_obj_set_size(rate,140,50);lv_textarea_set_one_line(rate,true);lv_textarea_set_accepted_chars(rate,"0123456789.");char rb[32];snprintf(rb,sizeof(rb),"%.2f",g_rates[1].rate);lv_textarea_set_text(rate,rb);static lv_obj_t *ra[2];ra[0]=rateCur;ra[1]=rate;button(g_screen,"Save",285,105,100,50,currency_rate_save,ra,0x0F766E);
  lv_obj_t *in=lv_textarea_create(g_screen);lv_obj_set_pos(in,25,185);lv_obj_set_size(in,360,55);lv_textarea_set_one_line(in,true);lv_textarea_set_accepted_chars(in,"0123456789.");lv_textarea_set_placeholder_text(in,"Amount");lv_obj_t *from=lv_dropdown_create(g_screen);lv_obj_set_pos(from,25,252);lv_obj_set_size(from,150,48);lv_dropdown_set_options(from,opts);lv_obj_t *to=lv_dropdown_create(g_screen);lv_obj_set_pos(to,235,252);lv_obj_set_size(to,150,48);lv_dropdown_set_options(to,opts);lv_dropdown_set_selected(to,1);lv_obj_t *out=label(g_screen,"",80,380,&lv_font_montserrat_20,0xFDE68A);static lv_obj_t *a[4];a[0]=in;a[1]=out;a[2]=from;a[3]=to;button(g_screen,"CONVERT",105,315,200,52,currency_calc,a,0x0369A1);label(g_screen,"All rates are editable, local, and may be outdated.",45,450,&lv_font_montserrat_12,0x94A3B8);
}
static void phrase_show(lv_event_t *e){int i=(intptr_t)lv_event_get_user_data(e);const char **local=g_phrase_language==0?PHRASE_LOCAL:g_phrase_language==1?PHRASE_ZH:PHRASE_JA;const char **sound=g_phrase_language==0?PHRASE_SOUND:g_phrase_language==1?PHRASE_ZH_SOUND:PHRASE_JA_SOUND;char b[220];snprintf(b,sizeof(b),"%s\n\n%s\n\n%s",local[i],sound[i],PHRASE_EN[i]);show_toast(b);}
static void phrase_language_cb(lv_event_t *e){g_phrase_language=lv_dropdown_get_selected(lv_event_get_target(e));build_screen(SCREEN_PHRASES);}
static void build_phrases(){g_screen=base_screen("Phrasebook");lv_obj_t *dd=lv_dropdown_create(g_screen);lv_obj_set_pos(dd,25,76);lv_obj_set_size(dd,360,48);lv_dropdown_set_options(dd,"Khmer\nMandarin\nJapanese");lv_dropdown_set_selected(dd,g_phrase_language);lv_obj_add_event_cb(dd,phrase_language_cb,LV_EVENT_VALUE_CHANGED,nullptr);const char **sound=g_phrase_language==0?PHRASE_SOUND:g_phrase_language==1?PHRASE_ZH_SOUND:PHRASE_JA_SOUND;for(int i=0;i<PHRASE_COUNT;i++){char b[96];snprintf(b,sizeof(b),"%s  •  %s",PHRASE_EN[i],sound[i]);button(g_screen,b,24,132+i*50,362,43,phrase_show,(void*)(intptr_t)i,0x155E75);}}

static void packing_cb(lv_event_t *e){uint8_t i=(intptr_t)lv_event_get_user_data(e);if(lv_obj_has_state(lv_event_get_target(e),LV_STATE_CHECKED))g_packing_mask|=(1U<<i);else g_packing_mask&=~(1U<<i);save_all();}
static void build_packing(){
  g_screen=base_screen("Packing List");const char *items[]={"Passport + copies","Cards + cash","Medicines","Chargers + adapter","Water bottle","Offline maps","Light layer"};
  for(int i=0;i<7;i++){lv_obj_t *c=lv_checkbox_create(g_screen);lv_checkbox_set_text(c,items[i]);lv_obj_set_pos(c,35,85+i*52);lv_obj_set_style_text_font(c,&lv_font_montserrat_16,0);if(g_packing_mask&(1U<<i))lv_obj_add_state(c,LV_STATE_CHECKED);lv_obj_add_event_cb(c,packing_cb,LV_EVENT_VALUE_CHANGED,(void*)(intptr_t)i);}
}
static void locker_save(lv_event_t *e){if(!g_sd_ok){show_toast("SD card unavailable.");return;}lv_obj_t *ta=(lv_obj_t*)lv_event_get_user_data(e);File f=SD_MMC.open("/VOYAGER/LOCKER/notes.txt",FILE_WRITE);if(!f){show_toast("Locker save failed.");return;}f.print(lv_textarea_get_text(ta));f.close();show_toast("Trip references saved.");}
static void build_locker(){g_screen=base_screen("Trip Locker");label(g_screen,"References only — never PINs or passwords.",28,78,&lv_font_montserrat_14,0xFDE68A);lv_obj_t *ta=lv_textarea_create(g_screen);lv_obj_set_pos(ta,20,115);lv_obj_set_size(ta,370,270);lv_textarea_set_placeholder_text(ta,"FLIGHTS\nHOTELS\nBOOKING REFERENCES\nADDRESSES\nCONTACTS\nTRANSPORT\nEMERGENCY\nOTHER");lv_textarea_set_max_length(ta,900);if(g_sd_ok){File f=SD_MMC.open("/VOYAGER/LOCKER/notes.txt",FILE_READ);if(f){String s=f.readString();f.close();lv_textarea_set_text(ta,s.c_str());}}button(g_screen,"SAVE LOCKER",75,405,260,55,locker_save,ta,0x0F766E);}

static void rpg_action(lv_event_t *e){int a=(intptr_t)lv_event_get_user_data(e);String msg;if(a==0){g_rpg.location=(g_rpg.location+1)%6;g_rpg.enemyHp=8+g_rpg.location*4+g_rpg.level*2;msg="An enemy blocks the road!";}else if(a==1&&g_rpg.enemyHp){uint8_t hit=3+g_rpg.weapon*2+esp_random()%5;g_rpg.enemyHp=hit>=g_rpg.enemyHp?0:g_rpg.enemyHp-hit;if(!g_rpg.enemyHp){uint16_t reward=8+g_rpg.location*5;g_rpg.xp+=reward;g_rpg.gold+=reward/2;g_rpg.questProgress++;if(g_rpg.location==5)g_rpg.bosses++;msg="Victory! XP and gold earned.";}else{uint8_t hurt=max(1,4+(int)g_rpg.location-(int)g_rpg.armour);g_rpg.hp=hurt>=g_rpg.hp?0:g_rpg.hp-hurt;if(!g_rpg.hp){g_rpg.hp=g_rpg.maxHp;g_rpg.gold/=2;g_rpg.location=0;g_rpg.enemyHp=0;msg="Defeated. You wake in town with half your gold.";}}}else if(a==2&&g_rpg.potions){g_rpg.potions--;g_rpg.hp=min(g_rpg.maxHp,(uint16_t)(g_rpg.hp+15));msg="Potion used.";}else if(a==3&&g_rpg.gold>=25){g_rpg.gold-=25;g_rpg.weapon=min(8,(int)g_rpg.weapon+1);msg="Weapon upgraded.";}else if(a==4&&g_rpg.gold>=20){g_rpg.gold-=20;g_rpg.potions++;msg="Potion purchased.";}else msg="You cannot do that yet.";while(g_rpg.xp>=g_rpg.level*40){g_rpg.xp-=g_rpg.level*40;g_rpg.level++;g_rpg.maxHp+=5;g_rpg.hp=g_rpg.maxHp;}g_games[0].level=g_rpg.level;g_games[0].coins=g_rpg.gold;g_games[0].score=g_rpg.questProgress;g_companion.adventure++;save_all();build_screen(SCREEN_RPG);show_toast(msg.c_str());}
static void rpg_special(lv_event_t *e){int a=(intptr_t)lv_event_get_user_data(e);String msg;if(a==0&&g_rpg.location==0&&!g_rpg.enemyHp){g_rpg.hp=g_rpg.maxHp;msg="Rested safely at the Willow Inn.";}else if(a==1&&g_rpg.gold>=30){g_rpg.gold-=30;g_rpg.armour=min(8,(int)g_rpg.armour+1);msg="Armour upgraded at the forge.";}else if(a==2&&g_rpg.questProgress>=5){g_rpg.questProgress-=5;g_rpg.gold+=60;g_rpg.xp+=30;msg="Guild contract completed: +60 gold, +30 XP.";}else msg=a==0?"Return to Willow Town to rest.":a==1?"Armour costs 30 gold.":"Defeat five enemies to complete a guild contract.";save_all();build_screen(SCREEN_RPG);show_toast(msg.c_str());}
static void build_rpg(){g_screen=base_screen("TinyQuest");g_sleep_locks|=LOCK_GAME;const char *places[]={"Willow Town","Whisper Wood","Old Mine","Frost Pass","Ruined Keep","Dragon Peak"};char b[300];snprintf(b,sizeof(b),"%s\nLV %u  HP %u/%u  XP %u/%u\nGold %u  Sword +%u  Armour +%u\nPotions %u  Contract %u/5  Bosses %u\n%s",places[g_rpg.location],g_rpg.level,g_rpg.hp,g_rpg.maxHp,g_rpg.xp,g_rpg.level*40,g_rpg.gold,g_rpg.weapon,g_rpg.armour,g_rpg.potions,g_rpg.questProgress,g_rpg.bosses,g_rpg.enemyHp?"ENEMY ENGAGED":"Choose exploration, preparation, or guild work");label(g_screen,b,22,78,&lv_font_montserrat_14,g_rpg.enemyHp?0xFCA5A5:0xD1FAE5);if(g_rpg.enemyHp){char enemy[32];snprintf(enemy,sizeof(enemy),"ATTACK • ENEMY %u HP",g_rpg.enemyHp);button(g_screen,enemy,25,245,360,52,rpg_action,(void*)1,0x9F1239);}else button(g_screen,"EXPLORE NEXT REGION",25,245,360,52,rpg_action,(void*)0,0x047857);button(g_screen,"Potion",20,310,115,45,rpg_action,(void*)2);button(g_screen,"Sword 25g",148,310,115,45,rpg_action,(void*)3,0x6D28D9);button(g_screen,"Buy potion",276,310,115,45,rpg_action,(void*)4,0xB45309);button(g_screen,"Rest",20,370,115,45,rpg_special,(void*)0,0x155E75);button(g_screen,"Armour 30g",148,370,115,45,rpg_special,(void*)1,0x4338CA);button(g_screen,"Claim guild",276,370,115,45,rpg_special,(void*)2,0x0F766E);}

static uint16_t space_price(uint8_t good){uint32_t x=g_space.marketSeed+g_space.planet*977+good*331;x^=x<<13;x^=x>>17;x^=x<<5;return (good==0?12:good==1?42:25)+(x%20);}
static void space_action(lv_event_t *e){int a=(intptr_t)lv_event_get_user_data(e);uint16_t used=g_space.cargoFood+g_space.cargoTech+g_space.cargoOre;String msg;uint8_t good=a%3;uint16_t price=space_price(good);if(a<3){if(used>=g_space.cargoCapacity||g_space.credits<price)msg="No cargo space or credits.";else{g_space.credits-=price;if(good==0)g_space.cargoFood++;else if(good==1)g_space.cargoTech++;else g_space.cargoOre++;msg="Cargo purchased.";}}else if(a<6){uint16_t *qty=good==0?&g_space.cargoFood:good==1?&g_space.cargoTech:&g_space.cargoOre;if(!*qty)msg="None in cargo.";else{(*qty)--;g_space.credits+=price;g_space.reputation++;msg="Cargo sold.";}}else if(a==6){uint16_t cost=18+g_space.planet*3;if(g_space.fuel<cost)msg="Not enough fuel.";else{g_space.fuel-=cost;g_space.planet=(g_space.planet+1)%5;g_space.marketSeed=esp_random();if(esp_random()%5==0){uint16_t loss=min((uint32_t)80,g_space.credits);g_space.credits-=loss;msg="Pirates raided the ship!";}else{g_space.missions++;g_space.reputation+=3;msg="Jump complete. Market prices changed.";}}}else if(a==7&&g_space.credits>=60){g_space.credits-=60;g_space.fuel=min(100,(int)g_space.fuel+40);msg="Fuel tanks replenished.";}else if(a==8&&g_space.credits>=250){g_space.credits-=250;g_space.ship++;g_space.cargoCapacity+=4;msg="Ship cargo bay upgraded.";}else msg="Insufficient credits.";g_games[1].coins=g_space.credits;g_games[1].score=g_space.reputation;g_companion.strategy++;save_all();build_screen(SCREEN_SPACE);show_toast(msg.c_str());}
static void space_contract(lv_event_t *e){int a=(intptr_t)lv_event_get_user_data(e);String msg;if(a==0){uint16_t cost=12+g_space.ship*3;if(g_space.fuel<cost)msg="Not enough fuel for a pirate hunt.";else{g_space.fuel-=cost;uint8_t danger=1+esp_random()%6;if(g_space.ship+esp_random()%5>=danger){g_space.piratesDefeated++;g_space.credits+=90+danger*20;g_space.reputation+=8;msg="Pirate defeated. Bounty transferred.";}else{uint32_t loss=min((uint32_t)120,g_space.credits);g_space.credits-=loss;msg="Hull damaged. Emergency repairs paid.";}}}else if(a==1){if(g_space.missions>=3){g_space.missions-=3;g_space.credits+=180;g_space.reputation+=12;msg="Three delivery stamps exchanged for a contract bonus.";}else msg="Complete three safe jumps for a delivery bonus.";}else{uint16_t total=g_space.cargoFood+g_space.cargoTech+g_space.cargoOre;if(total<3)msg="Carry at least three cargo units.";else{uint32_t value=g_space.cargoFood*space_price(0)+g_space.cargoTech*space_price(1)+g_space.cargoOre*space_price(2);g_space.credits+=value;g_space.reputation+=total;g_space.cargoFood=g_space.cargoTech=g_space.cargoOre=0;msg="Entire cargo manifest sold at current prices.";}}save_all();build_screen(SCREEN_SPACE);show_toast(msg.c_str());}
static void build_space(){g_screen=base_screen("Space Trader");g_sleep_locks|=LOCK_GAME;const char *p[]={"Terra","Cinder","Pelagos","Nimbus","Frontier"};char b[320];snprintf(b,sizeof(b),"ORBIT %s • Ship Mk %u • Rep %u\nCredits %lu  Fuel %u/100  Cargo %u/%u\nFood %u @%u  Tech %u @%u  Ore %u @%u\nDelivery stamps %u  Pirate wins %u",p[g_space.planet],g_space.ship,g_space.reputation,(unsigned long)g_space.credits,g_space.fuel,g_space.cargoFood+g_space.cargoTech+g_space.cargoOre,g_space.cargoCapacity,g_space.cargoFood,space_price(0),g_space.cargoTech,space_price(1),g_space.cargoOre,space_price(2),g_space.missions,g_space.piratesDefeated);label(g_screen,b,20,78,&lv_font_montserrat_14,0xBAE6FD);const char *buy[]={"BUY FOOD","BUY TECH","BUY ORE"};const char *sell[]={"SELL FOOD","SELL TECH","SELL ORE"};for(int i=0;i<3;i++){button(g_screen,buy[i],20+i*130,205,115,42,space_action,(void*)(intptr_t)i,0x155E75);button(g_screen,sell[i],20+i*130,255,115,42,space_action,(void*)(intptr_t)(i+3),0x0F766E);}button(g_screen,"JUMP",20,310,115,44,space_action,(void*)6,0x6D28D9);button(g_screen,"REFUEL",148,310,115,44,space_action,(void*)7,0xB45309);button(g_screen,"SHIP +",276,310,115,44,space_action,(void*)8,0x4338CA);button(g_screen,"PIRATE HUNT",20,365,115,44,space_contract,(void*)0,0x9F1239);button(g_screen,"CONTRACT",148,365,115,44,space_contract,(void*)1,0x7C3AED);button(g_screen,"SELL ALL",276,365,115,44,space_contract,(void*)2,0x047857);}

static void kingdom_action(lv_event_t *e){int a=(intptr_t)lv_event_get_user_data(e);String msg;if(a==0){g_kingdom.food+=15;g_kingdom.wood+=8;g_kingdom.stone+=4;msg="Workers gathered supplies.";}else if(a==1&&g_kingdom.wood>=40&&g_kingdom.stone>=20){g_kingdom.wood-=40;g_kingdom.stone-=20;g_kingdom.farm++;msg="Farm upgraded: more offline food.";}else if(a==2&&g_kingdom.wood>=35&&g_kingdom.gold>=20){g_kingdom.wood-=35;g_kingdom.gold-=20;g_kingdom.barracks++;g_kingdom.army+=5;msg="Barracks trained five guards.";}else if(a==3&&g_kingdom.food>=30){g_kingdom.food-=30;g_kingdom.happiness=min(100,(int)g_kingdom.happiness+8);g_kingdom.population+=2;msg="Festival raised happiness and population.";}else if(a==4&&g_kingdom.stone>=80&&g_kingdom.gold>=100){g_kingdom.stone-=80;g_kingdom.gold-=100;g_kingdom.castle++;msg="Castle expanded. Gold production increased.";}else msg="Not enough resources.";g_kingdom.lastTick=now_epoch();g_games[2].coins=g_kingdom.gold;g_games[2].score=g_kingdom.population;g_companion.strategy++;save_all();build_screen(SCREEN_KINGDOM);show_toast(msg.c_str());}
static void kingdom_develop(lv_event_t *e){int a=(intptr_t)lv_event_get_user_data(e);String msg;if(a==0&&g_kingdom.wood>=35&&g_kingdom.gold>=15){g_kingdom.wood-=35;g_kingdom.gold-=15;g_kingdom.lumberyard++;msg="Lumberyard upgraded: more offline wood.";}else if(a==1&&g_kingdom.wood>=25&&g_kingdom.gold>=25){g_kingdom.wood-=25;g_kingdom.gold-=25;g_kingdom.quarry++;msg="Quarry upgraded: more offline stone.";}else if(a==2){uint32_t tax=g_kingdom.population*(2+g_kingdom.castle);g_kingdom.gold+=tax;g_kingdom.happiness=g_kingdom.happiness>5?g_kingdom.happiness-5:0;msg="Taxes collected. Happiness fell by 5.";}else if(a==3&&g_kingdom.food>=20&&g_kingdom.gold>=10){g_kingdom.food-=20;g_kingdom.gold-=10;g_kingdom.army+=3+g_kingdom.barracks;msg="New guards joined the army.";}else msg="Not enough resources for this development.";g_kingdom.lastTick=now_epoch();save_all();build_screen(SCREEN_KINGDOM);show_toast(msg.c_str());}
static void build_kingdom(){g_screen=base_screen("Pocket Kingdom");g_sleep_locks|=LOCK_GAME;char b[340];snprintf(b,sizeof(b),"REALM TIER %u • Pop %lu • Happy %u%% • Army %lu\nFood %lu  Wood %lu  Stone %lu  Gold %lu\nProduction/min: +%u food +%u wood +%u stone +%u gold\nFarm %u  Lumber %u  Quarry %u  Barracks %u  Castle %u",g_kingdom.castle+1,(unsigned long)g_kingdom.population,g_kingdom.happiness,(unsigned long)g_kingdom.army,(unsigned long)g_kingdom.food,(unsigned long)g_kingdom.wood,(unsigned long)g_kingdom.stone,(unsigned long)g_kingdom.gold,g_kingdom.farm,g_kingdom.lumberyard,g_kingdom.quarry,1+g_kingdom.castle,g_kingdom.farm,g_kingdom.lumberyard,g_kingdom.quarry,g_kingdom.barracks,g_kingdom.castle);label(g_screen,b,18,78,&lv_font_montserrat_14,0xFDE68A);button(g_screen,"Gather",20,225,115,42,kingdom_action,(void*)0,0x0F766E);button(g_screen,"Farm",148,225,115,42,kingdom_action,(void*)1,0x047857);button(g_screen,"Festival",276,225,115,42,kingdom_action,(void*)3,0xB45309);button(g_screen,"Lumber",20,278,115,42,kingdom_develop,(void*)0,0x155E75);button(g_screen,"Quarry",148,278,115,42,kingdom_develop,(void*)1,0x334155);button(g_screen,"Recruit",276,278,115,42,kingdom_develop,(void*)3,0x4338CA);button(g_screen,"Barracks",20,335,115,42,kingdom_action,(void*)2,0x4338CA);button(g_screen,"Collect tax",148,335,115,42,kingdom_develop,(void*)2,0x9F1239);button(g_screen,"Castle",276,335,115,42,kingdom_action,(void*)4,0x6D28D9);label(g_screen,"Offline gains cap at six hours; clock rollback gives no reward.",34,410,&lv_font_montserrat_12,0x94A3B8);}

static void creature_action(lv_event_t *e){int a=(intptr_t)lv_event_get_user_data(e);String msg;if(a==0){uint32_t delta=g_today.steps>g_creatures.stepsBank?g_today.steps-g_creatures.stepsBank:0;if(delta<100&&!g_creatures.bait){msg="Walk 100 more steps or earn bait in quests.";}else{if(delta>=100)g_creatures.stepsBank+=100;else g_creatures.bait--;uint8_t species=esp_random()%16;g_creatures.discovered|=(1UL<<species);if(esp_random()%100<55+g_creatures.level*2){g_creatures.captured|=(1UL<<species);g_creatures.activeSpecies=species;msg="Creature captured and made active!";}else msg="Discovered, but it escaped.";}}else if(a==1){uint8_t foe=2+esp_random()%12;uint8_t hit=max(1,(int)g_creatures.attack-(int)(foe/3));uint8_t hurt=max(1,(int)foe-(int)g_creatures.defence);if(hit*3>=foe*2){g_creatures.wins++;g_creatures.xp+=12+foe;g_creatures.hp=g_creatures.hp>hurt?g_creatures.hp-hurt:1;msg="Battle won!";}else{g_creatures.hp=g_creatures.hp>hurt*2?g_creatures.hp-hurt*2:1;msg="The wild creature was stronger.";}}else if(a==2){g_creatures.hp=g_creatures.maxHp;msg="Active creature rested.";}else if(a==3&&g_creatures.level>=5&&g_creatures.evolution<3){g_creatures.evolution++;g_creatures.attack+=3;g_creatures.defence+=2;msg="Evolution complete!";}else msg="Reach level 5 or next evolution threshold.";while(g_creatures.xp>=g_creatures.level*30){g_creatures.xp-=g_creatures.level*30;g_creatures.level++;g_creatures.maxHp+=4;g_creatures.hp=g_creatures.maxHp;g_creatures.attack++;if(g_creatures.level%5==0)g_creatures.bait++;}g_games[3].level=g_creatures.level;g_games[3].score=g_creatures.wins;g_companion.exploration++;save_all();build_screen(SCREEN_CREATURES);show_toast(msg.c_str());}
static void creature_manage(lv_event_t *e){int a=(intptr_t)lv_event_get_user_data(e);String msg;if(a==0){uint8_t start=g_creatures.activeSpecies;do{g_creatures.activeSpecies=(g_creatures.activeSpecies+1)%16;}while(!(g_creatures.captured&(1UL<<g_creatures.activeSpecies))&&g_creatures.activeSpecies!=start);msg=g_creatures.activeSpecies==start?"Capture another species to switch.":"Active creature changed.";}else if(a==1&&g_creatures.bait){g_creatures.bait--;g_creatures.xp+=20;g_creatures.attack++;msg="Training consumed one bait: +20 XP, +1 attack.";}else if(a==2&&g_creatures.wins>=5){g_creatures.wins-=5;g_creatures.bait+=2;g_creatures.defence++;msg="Arena reward: two bait and +1 defence.";}else msg=a==1?"Training requires bait.":"Win five battles to claim an arena reward.";save_all();build_screen(SCREEN_CREATURES);show_toast(msg.c_str());}
static void build_creatures(){g_screen=base_screen("Creature Collector");g_sleep_locks|=LOCK_GAME;static const char *species[]={"Mossling","Emberfox","Tidefin","Cloudlet","Pebbleon","Voltkit","Moonmoth","Thorncub","Glider","Frostbit","Sunscale","Echoowl","Duneling","Bloomtail","Starhorn","Voidpup"};uint32_t available=g_today.steps>g_creatures.stepsBank?g_today.steps-g_creatures.stepsBank:0;char b[340];snprintf(b,sizeof(b),"ACTIVE %s • Evo %u • LV %u\nHP %u/%u  XP %u/%u  ATK %u  DEF %u\nWins %u  Bait %u  Walk credit %lu/100\nCollection: %u discovered • %u captured",species[g_creatures.activeSpecies%16],g_creatures.evolution,g_creatures.level,g_creatures.hp,g_creatures.maxHp,g_creatures.xp,g_creatures.level*30,g_creatures.attack,g_creatures.defence,g_creatures.wins,g_creatures.bait,(unsigned long)min(available,100UL),__builtin_popcount(g_creatures.discovered),__builtin_popcount(g_creatures.captured));label(g_screen,b,20,78,&lv_font_montserrat_14,0xDDD6FE);button(g_screen,"SEARCH",20,225,115,45,creature_action,(void*)0,0x0F766E);button(g_screen,"BATTLE",148,225,115,45,creature_action,(void*)1,0x9F1239);button(g_screen,"REST",276,225,115,45,creature_action,(void*)2,0x155E75);button(g_screen,"NEXT",20,285,115,45,creature_manage,(void*)0,0x4338CA);button(g_screen,"TRAIN",148,285,115,45,creature_manage,(void*)1,0xB45309);button(g_screen,"EVOLVE",276,285,115,45,creature_action,(void*)3,0x7C3AED);button(g_screen,"CLAIM ARENA REWARD",55,350,300,48,creature_manage,(void*)2,0x6D28D9);label(g_screen,"Search uses 100 new steps or one bait.",70,420,&lv_font_montserrat_12,0x94A3B8);}

static void kingdom_refresh(){uint32_t now=now_epoch(),elapsed=min(21600UL,now>g_kingdom.lastTick?now-g_kingdom.lastTick:0UL);if(elapsed>=60){uint32_t turns=elapsed/60;g_kingdom.food+=turns*g_kingdom.farm;g_kingdom.wood+=turns*g_kingdom.lumberyard;g_kingdom.stone+=turns*g_kingdom.quarry;g_kingdom.gold+=turns*(1+g_kingdom.castle);g_kingdom.lastTick=now;save_all();}}
static void build_game(int i,const char*,const char*){if(i==0)build_rpg();else if(i==1)build_space();else if(i==2){kingdom_refresh();build_kingdom();}else build_creatures();}
static void game_open(lv_event_t *e){nav_open((ScreenID)(SCREEN_RPG+(intptr_t)lv_event_get_user_data(e)));}
static void build_games(){g_sleep_locks&=~LOCK_GAME;g_screen=base_screen("Games");char a[80],b[80],c[80],d[80];snprintf(a,sizeof(a),"TinyQuest • LV %u • %u contracts",g_rpg.level,g_rpg.questProgress);snprintf(b,sizeof(b),"Space Trader • %lu credits • Rep %u",(unsigned long)g_space.credits,g_space.reputation);snprintf(c,sizeof(c),"Pocket Kingdom • Pop %lu • Tier %u",(unsigned long)g_kingdom.population,g_kingdom.castle+1);snprintf(d,sizeof(d),"Creatures • %u/16 • LV %u",__builtin_popcount(g_creatures.captured),g_creatures.level);const char *n[]={a,b,c,d};for(int i=0;i<4;i++)button(g_screen,n[i],30,82+i*75,350,56,game_open,(void*)(intptr_t)i,0x4338CA);button(g_screen,"Pass the Watch",30,390,350,56,menu_cb,(void*)SCREEN_PASS,0x155E75);}

static void build_health(){
  g_screen=base_screen("Watch Health");char b[500];uint64_t total=g_sd_ok?SD_MMC.totalBytes():0,free=free_sd();
  snprintf(b,sizeof(b),"Battery       %d%% %s\nSD card       %s\nSD free       %.1f MB / %.1f MB\nHeap free     %lu KB\nPSRAM free    %lu KB\nBoot count    %lu\nRecovered RAW %lu\nJournal items %lu\nAudio         %s\nRTC           %s\nLast error    %lu",g_battery,g_charging?"CHARGING":"",g_sd_ok?"OK":"MISSING",free/1048576.0,total/1048576.0,(unsigned long)(ESP.getFreeHeap()/1024),(unsigned long)(ESP.getFreePsram()/1024),(unsigned long)g_health.bootCount,(unsigned long)g_health.recoveredRecordings,(unsigned long)g_health.lifetimeJournalEntries,g_audio_ok?"READY":"OFFLINE",g_rtc_ok?"READY":"OFFLINE",(unsigned long)g_health.lastError);label(g_screen,b,32,82,&lv_font_montserrat_16,0xD1FAE5);button(g_screen,"Diagnostics",25,420,170,50,menu_cb,(void*)SCREEN_DIAGNOSTICS);button(g_screen,"Storage",215,420,170,50,menu_cb,(void*)SCREEN_STORAGE);
}
static uint64_t dir_bytes(const char *path){if(!g_sd_ok)return 0;File d=SD_MMC.open(path);if(!d)return 0;uint64_t total=0;File f=d.openNextFile();while(f){if(f.isDirectory())total+=dir_bytes(f.path());else total+=f.size();f=f.openNextFile();}d.close();return total;}
static void build_storage(){g_screen=base_screen("Storage");uint64_t total=g_sd_ok?SD_MMC.totalBytes():0,free=free_sd(),j=dir_bytes(PATH_JOURNAL)+dir_bytes(PATH_VOICE),ga=dir_bytes(PATH_GAMES),lo=dir_bytes(PATH_LOCKER);float pct=total?free*100.0f/total:0;char b[360];snprintf(b,sizeof(b),"Journal + voice  %.2f MB\nGames + saves    %.2f MB\nTrip locker      %.2f MB\n\nFREE             %.2f MB (%.1f%%)\n\n%s",j/1048576.0,ga/1048576.0,lo/1048576.0,free/1048576.0,pct,pct<5?"CRITICAL: VOICE RECORDING DISABLED":pct<10?"SERIOUS STORAGE WARNING":pct<20?"STORAGE WARNING":"Storage level healthy");label(g_screen,b,32,100,&lv_font_montserrat_18,pct<10?0xFDA4AF:0xBAE6FD);}
static void diagnostic_test(lv_event_t *e){int i=(intptr_t)lv_event_get_user_data(e);char b[180];if(i==0)snprintf(b,sizeof(b),"STORAGE: %s\nFree: %.1f MB",g_sd_ok?"PASS":"FAIL",free_sd()/1048576.0);else if(i==1)snprintf(b,sizeof(b),"SENSORS\nPMU %s\nRTC %s\nIMU %s",g_pmu_ok?"PASS":"FAIL",g_rtc_ok?"PASS":"FAIL",g_imu_ok?"PASS":"FAIL");else snprintf(b,sizeof(b),"AUDIO %s\nTouch input is active if you opened this test.\nDisplay rendering: PASS",g_audio_ok?"PASS":"FAIL");show_toast(b);}
static void diagnostic_retry_modules(lv_event_t*){Preferences p;p.begin("voyinit",false);p.clear();p.end();show_toast("Module isolation cleared. Reboot to retry all hardware.");}
static void build_diagnostics(){g_screen=base_screen("Diagnostics");char b[420];snprintf(b,sizeof(b),"Firmware %s • LVGL %d.%d.%d\nCPU %lu MHz • Heap %lu KB • PSRAM %lu KB\nSD %s • PMU %s • RTC %s • IMU %s • I2S %s",VOYAGER_VERSION,LVGL_VERSION_MAJOR,LVGL_VERSION_MINOR,LVGL_VERSION_PATCH,(unsigned long)getCpuFrequencyMhz(),(unsigned long)(ESP.getFreeHeap()/1024),(unsigned long)(ESP.getFreePsram()/1024),g_sd_ok?"OK":"FAIL",g_pmu_ok?"OK":"FAIL",g_rtc_ok?"OK":"FAIL",g_imu_ok?"OK":"FAIL",g_audio_ok?"OK":"FAIL");lv_obj_t *l=label(g_screen,b,25,85,&lv_font_montserrat_14,0xBAE6FD);lv_obj_set_width(l,360);button(g_screen,"Test storage",35,225,340,48,diagnostic_test,(void*)0);button(g_screen,"Test sensors",35,283,340,48,diagnostic_test,(void*)1);button(g_screen,"Test display / touch / audio",35,341,340,48,diagnostic_test,(void*)2,0x0F766E);button(g_screen,"Retry isolated modules",35,409,340,48,diagnostic_retry_modules,nullptr,0xB45309);}

static void pass_action(lv_event_t *e){int i=(intptr_t)lv_event_get_user_data(e);char b[180];switch(i){case 0:snprintf(b,sizeof(b),"DICE: %u",(unsigned)(esp_random()%6+1));break;case 1:snprintf(b,sizeof(b),"COIN: %s",esp_random()&1?"HEADS":"TAILS");break;case 2:snprintf(b,sizeof(b),"NUMBER: %u",(unsigned)(esp_random()%100+1));break;case 3:{const char *c[]={"Act like a tour guide","Mime an animal","Describe a place without naming it","Name five foods","Tell a two-line story"};snprintf(b,sizeof(b),"CHALLENGE\n%s",c[esp_random()%5]);break;}default:{const char *q[]={"Would you rather explore space or the ocean?","Who am I? Choose a famous person.","Name three things beginning with S.","What fact surprised you today?"};snprintf(b,sizeof(b),"%s",q[esp_random()%4]);break;}}show_toast(b);}
static void build_pass(){g_screen=base_screen("Pass the Watch");const char *n[]={"Roll dice","Flip coin","Random 1–100","Quick challenge","Trivia / categories"};for(int i=0;i<5;i++)button(g_screen,n[i],35,90+i*70,340,55,pass_action,(void*)(intptr_t)i,i<3?0x155E75:0x6D28D9);}

static void time_save_cb(lv_event_t *e){lv_obj_t **a=(lv_obj_t**)lv_event_get_user_data(e);int y=atoi(lv_textarea_get_text(a[0])),mo=atoi(lv_textarea_get_text(a[1])),d=atoi(lv_textarea_get_text(a[2])),h=atoi(lv_textarea_get_text(a[3])),mi=atoi(lv_textarea_get_text(a[4]));if(!valid_date(y,mo,d)||h<0||h>23||mi<0||mi>59){show_toast("Invalid date or time.");return;}if(rtc_write(y,mo,d,h,mi))show_toast("RTC updated and verified.");else{g_health.lastError=202;show_toast("RTC write verification failed.");}}
static void build_time(){
  g_screen=base_screen("Date & Time");struct tm t={};getLocalTime(&t,10);const char *ph[]={"YYYY","MM","DD","HH","MIN"};static lv_obj_t *a[5];int vals[]={t.tm_year+1900,t.tm_mon+1,t.tm_mday,t.tm_hour,t.tm_min};
  for(int i=0;i<5;i++){a[i]=lv_textarea_create(g_screen);lv_obj_set_pos(a[i],20+i*76,125);lv_obj_set_size(a[i],66,58);lv_textarea_set_one_line(a[i],true);lv_textarea_set_accepted_chars(a[i],"0123456789");char b[8];snprintf(b,sizeof(b),i?"%02d":"%04d",vals[i]);lv_textarea_set_text(a[i],b);label(g_screen,ph[i],22+i*76,100,&lv_font_montserrat_12,0x94A3B8);}
  button(g_screen,"Save to RTC",45,225,320,65,time_save_cb,a,0x047857);label(g_screen,"Changing time may make SRS reviews immediately due.\nGame saves use elapsed progress, not wall-clock rewards.",30,330,&lv_font_montserrat_14,0xFDE68A);
}
static void torch_exit(lv_event_t*){g_sleep_locks&=~LOCK_TORCH;g_brightness=VOYAGER_DEFAULT_BRIGHTNESS;nav_back();}
static void build_torch(){g_sleep_locks|=LOCK_TORCH;g_screen=lv_obj_create(NULL);lv_obj_set_style_bg_color(g_screen,lv_color_hex(0xFFFFFF),0);button(g_screen,"EXIT TORCH",95,420,220,55,torch_exit,nullptr,0x111827);static_cast<Arduino_CO5300*>(g_gfx)->setBrightness(255);g_current_brightness=255;}
static void emergency_save(lv_event_t *e){if(!g_sd_ok){show_toast("SD card unavailable.");return;}File f=SD_MMC.open("/VOYAGER/LOCKER/emergency.txt",FILE_WRITE);if(!f){show_toast("Emergency info save failed.");return;}f.print(lv_textarea_get_text((lv_obj_t*)lv_event_get_user_data(e)));f.close();show_toast("Emergency information saved offline.");}
static void build_emergency(){g_screen=base_screen("EMERGENCY");label(g_screen,"112 / LOCAL EMERGENCY SERVICES",45,78,&lv_font_montserrat_16,0xFCA5A5);lv_obj_t *ta=lv_textarea_create(g_screen);lv_obj_set_pos(ta,20,112);lv_obj_set_size(ta,370,220);lv_textarea_set_placeholder_text(ta,"Name, medical needs, emergency contact, hotel/address, insurance reference");lv_textarea_set_max_length(ta,700);if(g_sd_ok){File f=SD_MMC.open("/VOYAGER/LOCKER/emergency.txt",FILE_READ);if(f){String s=f.readString();f.close();lv_textarea_set_text(ta,s.c_str());}}button(g_screen,"SAVE INFO",20,350,175,55,emergency_save,ta,0x0F766E);button(g_screen,"TORCH",215,350,175,55,menu_cb,(void*)SCREEN_TORCH,0xB91C1C);label(g_screen,"Keep critical details brief and readable.",55,430,&lv_font_montserrat_12,0xFDE68A);}

static void safe_continue(lv_event_t*){g_safe_mode=false;build_screen(SCREEN_HOME);}
static void build_safe_mode(){g_screen=base_screen("SAFE MODE");label(g_screen,"Voyager detected repeated incomplete boots.\n\nStorage and saved data were not erased.\nUse Continue to try the app with diagnostics,\nor return to LauncherOS.",30,100,&lv_font_montserrat_16,0xFDE68A);button(g_screen,"CONTINUE",35,300,340,58,safe_continue,nullptr,0x0F766E);button(g_screen,"DIAGNOSTICS",35,370,160,55,menu_cb,(void*)SCREEN_DIAGNOSTICS);button(g_screen,"LAUNCHEROS",215,370,160,55,launcher_cb,nullptr,0x9F1239);}

static void launcher_cb(lv_event_t*){save_all();::wc_launcher_return_now();}
static void deep_sleep_cb(lv_event_t*){save_all();if(g_recording)recording_stop();fade_to(0,220);esp_sleep_pd_config(ESP_PD_DOMAIN_RTC_PERIPH,ESP_PD_OPTION_ON);esp_sleep_enable_ext1_wakeup(1ULL<<BOOT_BTN,ESP_EXT1_WAKEUP_ANY_LOW);esp_deep_sleep_start();}
static void brightness_cb(lv_event_t *e){g_brightness=lv_slider_get_value(lv_event_get_target(e));g_current_brightness=g_brightness;static_cast<Arduino_CO5300*>(g_gfx)->setBrightness(g_brightness);save_all();}
static void timeout_cb(lv_event_t *e){static const uint32_t opts[]={10000,15000,30000,60000};g_screen_timeout_ms=opts[lv_dropdown_get_selected(lv_event_get_target(e))];save_all();}
static void profile_cb(lv_event_t *e){g_power_profile=lv_dropdown_get_selected(lv_event_get_target(e));if(g_power_profile==0){g_brightness=140;g_screen_timeout_ms=15000;}else if(g_power_profile==1){g_brightness=100;g_screen_timeout_ms=15000;}else{g_brightness=70;g_screen_timeout_ms=10000;}g_current_brightness=g_brightness;static_cast<Arduino_CO5300*>(g_gfx)->setBrightness(g_brightness);save_all();build_screen(SCREEN_SETTINGS);}
static void build_settings(){
  g_screen=base_screen("Settings");label(g_screen,"Brightness",26,78,&lv_font_montserrat_14,0xFFFFFF);lv_obj_t *sl=lv_slider_create(g_screen);lv_obj_set_pos(sl,25,105);lv_obj_set_size(sl,360,18);lv_slider_set_range(sl,20,255);lv_slider_set_value(sl,g_brightness,LV_ANIM_OFF);lv_obj_add_event_cb(sl,brightness_cb,LV_EVENT_VALUE_CHANGED,nullptr);
  lv_obj_t *to=lv_dropdown_create(g_screen);lv_obj_set_pos(to,25,140);lv_obj_set_size(to,170,48);lv_dropdown_set_options(to,"10 seconds\n15 seconds\n30 seconds\n60 seconds");uint8_t ti=g_screen_timeout_ms==10000?0:g_screen_timeout_ms==30000?2:g_screen_timeout_ms==60000?3:1;lv_dropdown_set_selected(to,ti);lv_obj_add_event_cb(to,timeout_cb,LV_EVENT_VALUE_CHANGED,nullptr);
  lv_obj_t *pr=lv_dropdown_create(g_screen);lv_obj_set_pos(pr,215,140);lv_obj_set_size(pr,170,48);lv_dropdown_set_options(pr,"Normal\nTravel\nMaximum");lv_dropdown_set_selected(pr,g_power_profile);lv_obj_add_event_cb(pr,profile_cb,LV_EVENT_VALUE_CHANGED,nullptr);
  button(g_screen,"Date & time",25,205,170,50,menu_cb,(void*)SCREEN_TIME);button(g_screen,"Storage",215,205,170,50,menu_cb,(void*)SCREEN_STORAGE);
  button(g_screen,"Emergency",25,270,170,50,menu_cb,(void*)SCREEN_EMERGENCY,0x991B1B);button(g_screen,"Deep sleep",215,270,170,50,deep_sleep_cb);
  button(g_screen,"BACK TO LAUNCHER",25,340,360,62,launcher_cb,nullptr,0x9F1239);label(g_screen,"Hold BOOT 1.5 s anywhere for LauncherOS.",65,430,&lv_font_montserrat_12,0x94A3B8);
}

static void build_screen(ScreenID id){
  g_sleep_locks&=~LOCK_GAME;lv_obj_t *old=g_screen;g_screen=nullptr;g_header_time=nullptr;g_status=nullptr;
  switch(id){
    case SCREEN_HOME:build_home();break;case SCREEN_JOURNAL:build_journal();break;case SCREEN_RECORD:build_record();break;case SCREEN_TIMELINE:build_timeline();break;case SCREEN_DAILY:build_daily();break;
    case SCREEN_JOURNEY:build_journey();break;case SCREEN_COMPANION:build_companion();break;case SCREEN_QUESTS:build_quests();break;case SCREEN_CHINESE:build_chinese();break;case SCREEN_CHINESE_LESSON:build_chinese_lesson();break;case SCREEN_CHINESE_TRACE:build_chinese_trace();break;case SCREEN_CHINESE_EXERCISE:build_chinese_exercise();break;case SCREEN_CHINESE_DICTIONARY:build_chinese_dictionary();break;case SCREEN_CHINESE_PROGRESS:build_chinese_progress();break;
    case SCREEN_KNOWLEDGE:build_knowledge();break;case SCREEN_KNOWLEDGE_QUIZ:build_knowledge_quiz();break;case SCREEN_JOURNEY_HISTORY:build_journey_history();break;case SCREEN_BARGAIN:build_bargain();break;case SCREEN_CURRENCY:build_currency();break;case SCREEN_PHRASES:build_phrases();break;case SCREEN_LOCKER:build_locker();break;case SCREEN_PACKING:build_packing();break;
    case SCREEN_GAMES:build_games();break;case SCREEN_RPG:build_game(0,"TinyQuest","Explore");break;case SCREEN_SPACE:build_game(1,"Space Trader","Trade route");break;case SCREEN_KINGDOM:build_game(2,"Pocket Kingdom","Build");break;case SCREEN_CREATURES:build_game(3,"Creatures","Search");break;
    case SCREEN_HEALTH:build_health();break;case SCREEN_SETTINGS:build_settings();break;case SCREEN_TIME:build_time();break;case SCREEN_STORAGE:build_storage();break;case SCREEN_DIAGNOSTICS:build_diagnostics();break;case SCREEN_PASS:build_pass();break;case SCREEN_TORCH:build_torch();break;case SCREEN_EMERGENCY:build_emergency();break;case SCREEN_SAFE_MODE:build_safe_mode();break;default:build_home();break;
  }
  lv_scr_load_anim(g_screen,LV_SCR_LOAD_ANIM_FADE_ON,120,0,old&&old!=g_screen);g_last_touch_ms=millis();
}

// =============================================================================

// =============================================================================
// VOYAGER APPLICATION LIFECYCLE -- called by the GameOS registry
// =============================================================================
static void app_init(){
  g_pmu_ok=::g_has_pmu;
  g_battery=::g_batt_pct;g_charging=::g_batt_ch;
  storage_init();
  load_all();
  recover_recording();
  g_nav={{SCREEN_HOME},1,SCREEN_HOME};g_home_page=0;
  g_last_touch_ms=g_last_wake_ms=millis();
  build_screen(SCREEN_HOME);
  g_app_started_ms=millis();g_optional_done=false;g_initialized=true;
}
static void optional_init(){
  Preferences guard;guard.begin("voyopt",false);
  uint8_t crashed=guard.getUChar("stage",0);
  uint8_t disabled=guard.getUChar("disabled",0);
  if(crashed==1)disabled|=1U;
  else if(crashed==2)disabled|=2U;
  guard.putUChar("disabled",disabled);guard.putUChar("stage",0);
  if(!(disabled&1U)){guard.putUChar("stage",1);rtc_init();guard.putUChar("stage",0);}
  else{g_rtc_ok=false;g_health.lastError=411;}
  if(!(disabled&2U)){guard.putUChar("stage",2);imu_init();guard.putUChar("stage",0);}
  else{g_imu_ok=false;g_health.lastError=412;}
  guard.end();g_optional_done=true;
}
static void app_loop(){
  if(!g_initialized)return;
  uint32_t now=millis();
  if(!g_optional_done&&now-g_app_started_ms>=600U)optional_init();
  service_audio();service_imu();service_header();
  if(now-g_last_battery_ms>=1000U){g_last_battery_ms=now;g_battery=::g_batt_pct;g_charging=::g_batt_ch;}
  if(now-g_last_save_ms>=300000UL)save_all();
  if(::g_sd_ok&&free_sd()<VOYAGER_CRITICAL_FREE_BYTES&&!g_recording)g_health.lastError=301;
  if(g_nav.current==SCREEN_CHINESE_TRACE&&::g_touch_down&&g_screen&&g_trace_dots<100){
    int32_t x=::g_tc_x,y=::g_tc_y;
    if(abs(x-g_trace_x)>5||abs(y-g_trace_y)>5){lv_obj_t *dot=lv_obj_create(g_screen);lv_obj_set_size(dot,9,9);lv_obj_set_pos(dot,x-4,y-4);lv_obj_set_style_radius(dot,LV_RADIUS_CIRCLE,0);lv_obj_set_style_bg_color(dot,lv_color_hex(0xA78BFA),0);lv_obj_set_style_border_width(dot,0,0);lv_obj_clear_flag(dot,LV_OBJ_FLAG_SCROLLABLE);g_trace_x=x;g_trace_y=y;g_trace_dots++;}
  }
  if(::g_pending_swipe!=WC_SWIPE_NONE){
    int8_t swipe=::g_pending_swipe;::g_pending_swipe=WC_SWIPE_NONE;
    if(swipe==WC_SWIPE_LEFT&&g_nav.current==SCREEN_HOME&&g_home_page<2){g_home_page++;build_screen(SCREEN_HOME);}
    else if(swipe==WC_SWIPE_RIGHT&&g_nav.current==SCREEN_HOME&&g_home_page>0){g_home_page--;build_screen(SCREEN_HOME);}
    else if(swipe==WC_SWIPE_RIGHT&&g_nav.depth>1)nav_back();
  }
}
static void app_exit(){
  if(g_recording)recording_stop();
  save_all();
  g_sleep_locks=LOCK_NONE;
  ::g_gfx->setBrightness(WC_DEFAULT_BRIGHTNESS);
  g_current_brightness=WC_DEFAULT_BRIGHTNESS;
  if(g_screen)lv_obj_del(g_screen);
  g_screen=nullptr;g_header_time=nullptr;g_status=nullptr;
}
} // namespace VoyagerApp

static void voyager_app_init(){VoyagerApp::app_init();}
static void voyager_app_loop(){VoyagerApp::app_loop();}
static void voyager_app_exit(){VoyagerApp::app_exit();}
