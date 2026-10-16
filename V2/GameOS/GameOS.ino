// =============================================================================
// GameOS.ino v3.0 -- 13 games, real touch edges, fixed-step gameplay
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
// ---- shared time block (spec: TIME_CONTRACT.md) -----------------------------
#define TIME_SERIAL Serial
#define TIME_SRC_ID TIME_SRC_MODULE
// ===== SHARED TIME BLOCK v2 =================================================
//  Inlined verbatim into every app in this repo.  Spec + rationale:
//  TIME_CONTRACT.md.  Only the two lines above the block may differ per app:
//      #define TIME_SERIAL  Serial          // or USBSerial
//      #define TIME_SRC_ID  TIME_SRC_MODULE // or TIME_SRC_LAUNCHER
//
//  Namespace "rtcsave" -- the ONE place the watch's wall clock lives:
//      t    uint32  unix epoch seconds, UTC
//      gmt  int32   local offset from UTC, in SECONDS   (+28800 = UTC+8)
//      seq  uint32  write counter, ++ on every publish  (diagnostics)
//      man  uint8   1 = a human set this clock, 0 = machine (NTP/boot).  A boot
//                       refresh keeps it; a machine that lands a NEW epoch clears it.
//      src  uint8   0 = launcher, 1 = module            (diagnostics)
//
//  Two different things have to survive a reboot, and only one of them does:
//    * the EPOCH lives in the chip's RTC timer.  It survives every software
//      reset and every deep sleep, so a launcher <-> module reboot inherits
//      it for free -- but power loss wipes it, and then this snapshot is the
//      only thing between the user and "00:00".
//    * the TIMEZONE lives in the C library's TZ string, which is RAM-ONLY.
//      It dies on EVERY reboot and no app can inherit it from the previous
//      one.  NVS is therefore the offset's only home.
//  => frame: epoch is inherited through the RTC, backed up here.
//     timezone is stored here and re-applied by every single boot.
// ============================================================================
#define TIME_VALID_MIN      1700000000UL   // ~2023-11-14: "the RTC has been set"
#define TIME_ADOPT_SLACK_S  2L             // ignore sub-2 s snapshot/rtc skew
#define TIME_SRC_LAUNCHER   0
#define TIME_SRC_MODULE     1

static int32_t  g_tp_gmt   = 0;   // local offset from UTC, in seconds
static uint32_t g_tp_seq   = 0;   // last seq we read or wrote
static bool     g_tp_valid = false;   // a plausible wall clock is available

// Point the C library at the local UTC offset.  TZ is RAM-only, so this must
// run on EVERY boot in EVERY app or the watch silently renders UTC.
// POSIX counts the offset backwards from local to UTC, hence the minus.
static void time_apply_tz(int32_t gmt_sec) {
    long mins = -(gmt_sec / 60);
    long a    = labs(mins);
    char tz[24];
    snprintf(tz, sizeof(tz), "UTC%c%ld:%02ld", mins < 0 ? '-' : '+', a / 60, a % 60);
    setenv("TZ", tz, 1);
    tzset();
    g_tp_gmt = gmt_sec;
}

// Is the shared NVS snapshot a better clock than the one the chip is holding?
//   nothing saved yet            -> leave everything alone
//   chip has no valid clock      -> cold boot / dead battery: adopt the NVS
//   a human set it (man=1)       -> their correction wins in EITHER direction
//   snapshot runs ahead of the   -> somebody moved the clock forward: adopt
//   chip by more than the slack
//   otherwise                    -> the RTC is the live clock, keep it
//   Returns TIME_DEC_* below (uint8_t return keeps the Arduino .ino auto
//   prototype generator, which runs before this enum is declared, happy).
enum TimeDecision { TIME_DEC_NOTHING = 0, TIME_DEC_KEEP_RTC = 1, TIME_DEC_ADOPT = 2 };
static uint8_t time_decide(time_t rtc_now, uint32_t snap_t, uint8_t snap_man) {
    if (snap_t < TIME_VALID_MIN)                       return TIME_DEC_NOTHING;
    if (rtc_now < (time_t)TIME_VALID_MIN)              return TIME_DEC_ADOPT;
    if (snap_man && labs((long)snap_t - (long)rtc_now) > TIME_ADOPT_SLACK_S)
                                                       return TIME_DEC_ADOPT;
    if ((int32_t)(snap_t - (uint32_t)rtc_now) > TIME_ADOPT_SLACK_S)
                                                       return TIME_DEC_ADOPT;
    return TIME_DEC_KEEP_RTC;
}

// WRITE half of the contract: epoch + offset + provenance.  Call it after
// every NTP sync, after a manual set, before deep sleep, and on the way out of
// the app (before esp_restart()).
static void time_publish(const char *why, bool manual) {
    time_t now; time(&now);
    if (now < (time_t)TIME_VALID_MIN) return;      // RTC not set yet
    Preferences p;
    if (!p.begin("rtcsave", false)) return;
    uint32_t prev     = p.getULong("t", 0);
    uint8_t  prev_man = p.getUChar("man", 0);
    // A human's correction stays in force while the snapshot still describes
    // the same instant.  Every boot refreshes `t` by a second or so and must
    // not quietly demote the clock back to "machine" -- otherwise the man=1
    // rule above only survives a single reboot.  A machine that lands a
    // genuinely DIFFERENT epoch (an NTP sync) does take the clock over.
    bool machine_moved_it = !prev ||
                            labs((long)now - (long)prev) > TIME_ADOPT_SLACK_S;
    uint8_t man = (manual || (prev_man && !machine_moved_it)) ? 1 : 0;
    p.putULong("t",   (uint32_t)now);
    p.putLong ("gmt", g_tp_gmt);
    p.putUChar("man", man);
    p.putUChar("src", TIME_SRC_ID);
    g_tp_seq = p.getULong("seq", 0) + 1;           // read-modify-write, session open
    p.putULong("seq", g_tp_seq);
    p.end();
    TIME_SERIAL.printf("[time] publish t=%lu gmt=%ld seq=%lu man=%d (%s)\n",
                       (unsigned long)now, (long)g_tp_gmt,
                       (unsigned long)g_tp_seq, man, why);
}

// READ half: reconcile the EPOCH, then ALWAYS re-apply the TIMEZONE, then
// republish so the snapshot never lags the running clock by more than a boot.
// Call this at the very top of setup(), before anything that shows a time.
static void time_restore_from_nvs() {
    time_t rtc_now = 0; time(&rtc_now);

    uint32_t snap_t = 0, snap_seq = 0;
    int32_t  snap_gmt = 0;
    uint8_t  snap_man = 0;
    Preferences p;
    if (p.begin("rtcsave", true)) {
        snap_t   = p.getULong("t",   0);
        snap_gmt = p.getLong ("gmt", 0);
        snap_seq = p.getULong("seq", 0);
        snap_man = p.getUChar("man", 0);
        p.end();
    }
    if (snap_seq == 0 && snap_t) snap_seq = 1;     // pre-v2 snapshot

    uint8_t d = time_decide(rtc_now, snap_t, snap_man);
    if (d == TIME_DEC_ADOPT) {
        struct timeval tv = { (time_t)snap_t, 0 };
        settimeofday(&tv, NULL);                   // writes the chip RTC as well
        g_tp_valid = true;
        TIME_SERIAL.printf("[time] adopted NVS t=%lu (rtc was %ld man=%d)\n",
                           (unsigned long)snap_t, (long)rtc_now, snap_man);
    } else if (d == TIME_DEC_KEEP_RTC) {
        g_tp_valid = true;                         // v6.2 rule: never rewind a live RTC
        TIME_SERIAL.printf("[time] RTC live t=%ld -- NVS kept as fallback\n",
                           (long)rtc_now);
    } else {
        TIME_SERIAL.println("[time] no persisted clock -- waiting for NTP");
    }

    // TZ is the part that never survives a reboot: take it from here, always.
    if (snap_t) time_apply_tz(snap_gmt);
    g_tp_seq = snap_seq;
    time_publish("boot", false);                   // keep the snapshot current
}
// ===== END SHARED TIME BLOCK v2 =============================================
static void wc_launcher_return_now() {
    time_publish("exit", false);   // hand the clock back to the launcher
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
    const char *title = "GAME//OS";
    g_gfx->setTextSize(3); g_gfx->setTextColor(g_gfx->color565(0xFF,0x30,0x30)); g_gfx->setBrightness(180);
    for (int i = 0; title[i]; i++) { g_gfx->setCursor(110 + i*18, WC_LCD_HEIGHT/2 - 12); g_gfx->print(title[i]); delay(45); }
    g_gfx->setTextSize(1); g_gfx->setTextColor(g_gfx->color565(0x8A,0x10,0x10));
    const char *l2 = "LOADING GAME MODULES...";
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
    time_restore_from_nvs();   // adopt the shared clock before anything shows a time
    esp_sleep_get_wakeup_cause();
    Serial.begin(115200);
    delay(50);
    wc_boot_guard_begin();
    wc_hw_boot();
    wc_theme_load();

    wc_boot_splash();                                 // radar splash (repo TacticalOS)
    wc_fade_brightness(WC_DEFAULT_BRIGHTNESS, 260);

    register_games();
    wc_game_manifest_load();

    build_grid();
    Serial.printf("[GameOS] v2.1 ready games=%d sd=%s\n", wc_game_count(), g_sd_ok ? "ok" : "MISSING");
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
    // Launch a tapped game here (outside the LVGL event/timer context).
    if (!g_screen_off && g_pending_game >= 0) {
        int idx = g_pending_game; g_pending_game = -1;
        if (wc_game_launch(idx)) build_grid();   // blocking; rebuilds grid on exit
    }
    if (g_launcher_return_err) { g_launcher_return_err = false; show_launcher_return_error(); }
    if (g_pending_swipe != WC_SWIPE_NONE) {
        int8_t ev = g_pending_swipe; g_pending_swipe = WC_SWIPE_NONE;
        g_last_swipe_ms = millis();
        if (g_ret_err_scr && (ev == WC_SWIPE_DOWN || ev == WC_SWIPE_RIGHT)) dismiss_launcher_return_error();
    }
    delay(3);
}
