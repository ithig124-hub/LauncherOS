// ============================================================================
//  DualCoreSE.ino  --  DualCore Special Editions (single-file module)
// ----------------------------------------------------------------------------
//  A "mini module" for LauncherOS that carries three launchable experiences:
//
//      NoirOS   -- minimalist everyday OS (inbox, focus tools, world clock)
//      CrownOS  -- luxury watch faces, Watch Studio, occasions, countdowns
//      NightOS  -- ninja-dark utility OS (Shadow Mode, quiet alerts, schedule)
//
//  It follows the proven DualCore module contract exactly:
//
//    * BOOT PATTERN (GameOS / LauncherOS): QSPI CO5300 panel, FT3168 touch,
//      AXP2101 PMU, SD_MMC, LVGL 8.4 partial buffer (60 lines, INTERNAL ->
//      PSRAM -> 8-bit fallback), brightness fades, 15 s screen-off, PMU
//      short-press screen toggle.  CPU stays on (no deep sleep), so timers
//      and the screen-off/wake path behave exactly like GameOS.
//    * SHARED TIME BLOCK v2: copied verbatim from LauncherOS.ino v7.1 /
//      TIME_CONTRACT.md.  The epoch is inherited through the chip RTC, the
//      timezone is re-applied from NVS "rtcsave" on every boot, and the
//      block is published on the way out.
//    * BACK TO LAUNCHER (GameOS protocol): BOOT held 1.5 s (or the in-UI
//      buttons) -> time_publish("exit"), boot guard cleared, boot partition
//      anchored at FACTORY, esp_restart().  No SD card needed to get home.
//    * STORAGE: everything this module writes lives under /WATCH/NoirOS/,
//      /WATCH/CrownOS/, /WATCH/NightOS/, or the documented shared area
//      /WATCH/SHARED/ (inbox, timers, active-OS + power prefs).
//
//  EXPERIENCE SWITCHING is in-process: the chooser screen, then one OS at a
//  time.  Leaving an OS destroys its LVGL tree and frees its memory; the
//  shared services (inbox, clock, timers, phone link, power) stay alive, so
//  switching never loses a notification or resets a timer.  No reboot is
//  involved unless you ask for LauncherOS.
//
//  BUILD
//    Board   : Waveshare ESP32-S3 Touch AMOLED 2.06 (WOS 206A), 410x502
//    Library : Arduino_GFX, Arduino_DriveBus, XPowersLib, LVGL 8.4
//    lv_conf : LV_MEM_SIZE >= 128 KiB (LV_MEM_CUSTOM 0), and enable the
//              Montserrat fonts 10/12/14/16/18/20/22/24/28/32/48.
//    Optional: DCS_USE_BLE 1        -> NimBLE-Arduino phone link (section 6)
//              DCS_USE_SENSORLIB 1  -> SensorLib QMI8658 (NightOS motion)
//
//  KNOWN LIMITS (documented, not hidden)
//    * The phone link ships with a demo source so the module is useful with
//      no phone.  Flip DCS_USE_BLE to 1 to compile the NimBLE transport; the
//      wire protocol is documented in section 6 but is UNVERIFIED on
//      hardware and must be proven against a real phone before release.
//    * World clock uses fixed UTC offsets (no tz database in flash).
//    * There is no on-screen keyboard: events and faces use preset names.
//    * The stopwatch is monotonic and does not survive a reset; the countdown
//      is wall-clock anchored and does.
//    * No magnetometer on this board: NightOS exposes an IMU status panel and
//      never claims a compass heading.
// ============================================================================

#include <Arduino.h>
#include <FS.h>
#include <SD_MMC.h>
#include <Wire.h>
#include <Preferences.h>
#include <WiFi.h>
#include <vector>
#include <memory>
#include <time.h>
#include <sys/time.h>
#include <math.h>
#include <stdint.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

#include "Arduino_GFX_Library.h"
#include "Arduino_DriveBus_Library.h"
#include "lv_conf.h"
#include "lvgl.h"

#define XPOWERS_CHIP_AXP2101
#include "XPowersLib.h"

#include <esp_heap_caps.h>
#include <esp_system.h>
#include <esp_ota_ops.h>
#include <esp_partition.h>
#include <esp_timer.h>
#include <esp_sleep.h>

// ---------------------------------------------------------------------------
//  1.  BUILD CONFIGURATION
// ---------------------------------------------------------------------------
#define DCS_VERSION        "1.5.0"
#define DCS_MANIFEST_API   1

#ifndef DCS_USE_BLE
#define DCS_USE_BLE        0       // see section 6 -- optional, unverified
#endif
#ifndef DCS_USE_SENSORLIB
#define DCS_USE_SENSORLIB  0       // needs SensorLib (QMI8658)
#endif

#if DCS_USE_BLE
#include <NimBLEDevice.h>
#endif
#if DCS_USE_SENSORLIB
#include "SensorQMI8658.hpp"
#endif

#define DCS_SCREEN_TIMEOUT_MS   15000
#define DCS_BOOT_LONGPRESS_MS    1500
#define DCS_DEFAULT_BRIGHTNESS    140
#define DCS_MIN_BRIGHTNESS         30
#define DCS_SWIPE_THRESHOLD_PX     25
#define DCS_SWIPE_MAX_DURATION_MS 800
#define DCS_TAP_THRESHOLD_PX       20
#define DCS_TP_RELEASE_HYST         3
#define DCS_SD_MOUNT_RETRIES        3
#define DCS_NAV_DEBOUNCE_MS       180

// ---------------------------------------------------------------------------
//  2.  PIN MAP  (Waveshare ESP32-S3 Touch AMOLED 2.06 -- same as LauncherOS)
// ---------------------------------------------------------------------------
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

// ---------------------------------------------------------------------------
//  3.  STORAGE LAYOUT  (the ONLY paths this module may touch)
// ---------------------------------------------------------------------------
#define DCS_PATH_NOIR    "/WATCH/NoirOS"
#define DCS_PATH_CROWN   "/WATCH/CrownOS"
#define DCS_PATH_NIGHT   "/WATCH/NightOS"
#define DCS_PATH_SHARED  "/WATCH/SHARED"      // shared-service state, one copy
#define DCS_PATH_LAUNCHER_BIN  "/WATCH/bin/launcheros.bin"

// ---- shared time block (spec: TIME_CONTRACT.md) ---------------------------
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

// ---------------------------------------------------------------------------
//  4.  BOOT GUARD + BACK TO LAUNCHEROS  (GameOS protocol, unchanged)
// ---------------------------------------------------------------------------
//  Three strikes: every launch increments "fail" before the UI exists and
//  clears it once the chooser is up.  A module that dies during init three
//  times in a row never traps the watch -- we anchor the FACTORY partition
//  and restart into LauncherOS instead.
#define DCS_NVS_BOOT "dcseboot"
static uint8_t g_boot_strikes = 0;

static void dcs_anchor_factory_boot() {
    const esp_partition_t *factory = esp_partition_find_first(
        ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_FACTORY, NULL);
    if (factory) { esp_ota_set_boot_partition(factory); return; }
    const esp_partition_t *other = esp_ota_get_next_update_partition(NULL);
    if (other && other != esp_ota_get_running_partition())
        esp_ota_set_boot_partition(other);
}

static void dcs_boot_guard_enter() {
    Preferences p;
    if (!p.begin(DCS_NVS_BOOT, false)) return;
    g_boot_strikes = p.getUChar("fail", 0);
    if (g_boot_strikes < 250) g_boot_strikes++;
    p.putUChar("fail", g_boot_strikes);
    p.end();
    Serial.printf("[boot] strike %u/3\n", (unsigned)g_boot_strikes);
    if (g_boot_strikes >= 3) {
        Serial.println("[boot] three failed launches -- returning to LauncherOS");
        dcs_anchor_factory_boot();
        delay(80);
        esp_restart();
    }
}

// Clear the counter: a usable UI was reached, or we are leaving on purpose
// (so a deliberate launcher return is never counted as a crash next boot).
static void dcs_boot_guard_success() {
    if (g_boot_strikes == 0) return;
    Preferences p;
    if (p.begin(DCS_NVS_BOOT, false)) { p.putUChar("fail", 0); p.end(); }
    g_boot_strikes = 0;
}

static void dcs_save_all();   // forward: persistence lives in section 8

static void dcs_launcher_return_now() {
    time_publish("exit", false);        // hand the clock back to the launcher
    dcs_save_all();                     // keep inbox + settings + timers
    dcs_boot_guard_success();           // never miscount an intentional exit
    dcs_anchor_factory_boot();
    Serial.println("[exit] restarting into LauncherOS");
    delay(80);
    esp_restart();
}

// ---------------------------------------------------------------------------
//  5.  HARDWARE  -- proven LauncherOS/GameOS boot chain
// ---------------------------------------------------------------------------
static Arduino_DataBus *g_bus =
    new Arduino_ESP32QSPI(WC_LCD_CS, WC_LCD_SCLK, WC_LCD_SDIO0, WC_LCD_SDIO1,
                          WC_LCD_SDIO2, WC_LCD_SDIO3);
static Arduino_CO5300 *g_gfx = new Arduino_CO5300(
    g_bus, WC_LCD_RESET, 0, WC_LCD_WIDTH, WC_LCD_HEIGHT, 22, 0, 0, 0);
static std::shared_ptr<Arduino_IIC_DriveBus> g_iicbus =
    std::make_shared<Arduino_HWIIC>(WC_IIC_SDA, WC_IIC_SCL, &Wire);
void Arduino_IIC_Touch_Interrupt(void);
static std::unique_ptr<Arduino_IIC> g_ft(new Arduino_FT3x68(
    g_iicbus, FT3168_DEVICE_ADDRESS, DRIVEBUS_DEFAULT_VALUE, WC_TP_INT,
    Arduino_IIC_Touch_Interrupt));
void Arduino_IIC_Touch_Interrupt(void) { g_ft->IIC_Interrupt_Flag = true; }

static XPowersAXP2101 g_pmu;
static bool g_has_pmu = false;
static lv_disp_draw_buf_t g_draw_buf;
static lv_color_t        *g_lvgl_buf = nullptr;
static lv_disp_drv_t      g_disp_drv;
static lv_indev_drv_t     g_indev_drv;
static lv_indev_t        *g_touch_indev = nullptr;

static bool g_sd_ok = false;
static uint8_t g_cur_brightness = 0;

static int32_t  g_ts_x = 0, g_ts_y = 0, g_tc_x = 0, g_tc_y = 0;
static uint32_t g_ts_ms = 0;
static volatile int8_t g_pending_swipe = 0;
static bool g_touch_down = false, g_tap = false, g_touch_block = false;
static bool g_wake_request = false, g_wake_armed = true;
static int  g_release_count = 0;
static bool g_screen_off = false;
static uint32_t g_last_touch_ms = 0;
static uint32_t g_last_nav_ms = 0;
static uint8_t g_fade_from = 0, g_fade_target = 0;
static uint32_t g_fade_start = 0, g_fade_duration = 0;
static bool g_fading = false;
static bool g_boot_require_release = false;

// Survives deep sleep: which experience to reopen on wake (0xFE = chooser).
RTC_DATA_ATTR static uint8_t g_resume_os = 0xFF;

// Battery cache, refreshed by the service loop only (one poller, ever).
static int  g_batt_pct = -1;
static bool g_batt_ch = false;
static int  g_batt_mv = 0;

enum DcsSwipe { DCS_SWIPE_NONE = 0, DCS_SWIPE_LEFT = -1, DCS_SWIPE_RIGHT = 1,
                DCS_SWIPE_UP = 2, DCS_SWIPE_DOWN = 3 };

static void dcs_input_reset() {
    g_tap = false;
    g_pending_swipe = DCS_SWIPE_NONE;
    g_touch_down = false;
    g_touch_block = true;
    g_release_count = 0;
    g_tc_x = g_ts_x = WC_LCD_WIDTH / 2;
    g_tc_y = g_ts_y = WC_LCD_HEIGHT / 2;
    if (g_touch_indev) lv_indev_reset(g_touch_indev, NULL);
}
static int8_t dcs_take_swipe() { int8_t s = g_pending_swipe; g_pending_swipe = DCS_SWIPE_NONE; return s; }
static inline bool dcs_boot_pressed() { return digitalRead(WC_BOOT_BTN) == LOW; }

static void dcs_flush(lv_disp_drv_t *drv, const lv_area_t *area, lv_color_t *px) {
    uint32_t w = lv_area_get_width(area), h = lv_area_get_height(area);
    g_gfx->draw16bitRGBBitmap(area->x1, area->y1, (uint16_t *)&px->full, w, h);
    lv_disp_flush_ready(drv);
}

// Touch read: the LauncherOS v5.13 hysteresis + GameOS screen-off/tap layer.
static void dcs_touch(lv_indev_drv_t *drv, lv_indev_data_t *data) {
    int32_t x = g_ft->IIC_Read_Device_Value(
        g_ft->Arduino_IIC_Touch::Value_Information::TOUCH_COORDINATE_X);
    int32_t y = g_ft->IIC_Read_Device_Value(
        g_ft->Arduino_IIC_Touch::Value_Information::TOUCH_COORDINATE_Y);
    int fingers = g_ft->IIC_Read_Device_Value(
        g_ft->Arduino_IIC_Touch::Value_Information::TOUCH_FINGER_NUMBER);

    bool pressed = (fingers > 0 && fingers <= 2 &&
                    x >= 0 && x < WC_LCD_WIDTH && y >= 0 && y < WC_LCD_HEIGHT);
    static int zero_streak = 0;
    static bool in_drag = false;

    if (pressed && !g_screen_off) g_last_touch_ms = millis();

    if (g_screen_off || g_touch_block) {
        data->state = LV_INDEV_STATE_REL;
        in_drag = false;
        g_touch_down = false;
        g_tap = false;
        if (pressed) {
            g_release_count = 0;
            if (g_screen_off && g_wake_armed) g_wake_request = true;
        } else if (++g_release_count >= DCS_TP_RELEASE_HYST) {
            g_touch_block = false;
            g_wake_armed = true;
        }
        return;
    }

    if (pressed) zero_streak = 0;
    else if (in_drag && ++zero_streak < DCS_TP_RELEASE_HYST) { pressed = true; x = g_tc_x; y = g_tc_y; }

    if (pressed) {
        data->state = LV_INDEV_STATE_PR;
        data->point.x = x; data->point.y = y;
        if (!in_drag) {
            in_drag = true;
            g_ts_x = x; g_ts_y = y; g_ts_ms = millis();
            if (millis() - g_last_nav_ms > DCS_NAV_DEBOUNCE_MS) g_tap = true;
        }
        g_touch_down = true;
        g_tc_x = x; g_tc_y = y;
    } else {
        data->state = LV_INDEV_STATE_REL;
        g_touch_down = false;
        if (in_drag) {
            in_drag = false;
            int32_t dx = g_tc_x - g_ts_x, dy = g_tc_y - g_ts_y;
            uint32_t dur = millis() - g_ts_ms;
            if (dur < DCS_SWIPE_MAX_DURATION_MS) {
                if (abs(dx) > abs(dy) && abs(dx) > DCS_SWIPE_THRESHOLD_PX)
                    g_pending_swipe = (dx < 0) ? DCS_SWIPE_LEFT : DCS_SWIPE_RIGHT;
                else if (abs(dy) > DCS_SWIPE_THRESHOLD_PX)
                    g_pending_swipe = (dy < 0) ? DCS_SWIPE_UP : DCS_SWIPE_DOWN;
            }
        }
    }
}

static void dcs_screen_set(bool off);   // forward
static void night_flash_off();          // forward (NightOS flashlight overlay)

static void dcs_service() {
    uint32_t now = millis();
#if !defined(LV_TICK_CUSTOM) || LV_TICK_CUSTOM == 0
    static uint32_t tick = 0;
    lv_tick_inc(now - tick);
    tick = now;
#endif
    static uint32_t poll = 0, fired = 0;
    static bool have_fired = false;
    if (g_has_pmu && now - poll >= 50U) {
        poll = now;
        g_pmu.getIrqStatus();
        bool press = g_pmu.isPekeyShortPressIrq();
        g_pmu.clearIrqStatus();
        if (press && (!have_fired || now - fired >= 400U)) {
            fired = now; have_fired = true;
            dcs_screen_set(!g_screen_off);
        }
    }
    lv_timer_handler();

    if (g_wake_request) { g_wake_request = false; dcs_screen_set(false); }
    now = millis();

    if (!g_screen_off && now - g_last_touch_ms >= DCS_SCREEN_TIMEOUT_MS)
        dcs_screen_set(true);

    if (g_fading) {
        uint32_t elapsed = now - g_fade_start;
        int value = elapsed >= g_fade_duration
            ? g_fade_target
            : (int)g_fade_from + ((int)g_fade_target - (int)g_fade_from) *
                  (int32_t)elapsed / (int32_t)g_fade_duration;
        g_cur_brightness = (uint8_t)value;
        g_gfx->setBrightness(g_cur_brightness);
        if (elapsed >= g_fade_duration) g_fading = false;
    }
}

static void dcs_fade_to(uint8_t target, uint32_t dur_ms) {
    g_fade_from = g_cur_brightness;
    g_fade_target = target;
    g_fade_start = millis();
    g_fade_duration = dur_ms ? dur_ms : 1;
    g_fading = true;
}

static uint8_t dcs_brightness_base();     // section 11
static void ui_refresh_now();             // section 14

static void dcs_screen_set(bool off) {
    if (g_screen_off == off) return;
    g_wake_armed = !g_touch_down;
    g_screen_off = off;
    dcs_input_reset();
    g_wake_request = false;
    if (off) {
        night_flash_off();      // never leave a white overlay on a dark panel
        dcs_fade_to(0, 220);
    } else {
        g_last_touch_ms = millis();
        uint8_t b = dcs_brightness_base();
        g_cur_brightness = 0;
        g_gfx->setBrightness(0);
        ui_refresh_now();
        dcs_fade_to(b, 200);
    }
}

static void dcs_i2c_bus_recover() {
    pinMode(WC_IIC_SCL, OUTPUT);
    for (int i = 0; i < 9; i++) {
        digitalWrite(WC_IIC_SCL, LOW); delayMicroseconds(5);
        digitalWrite(WC_IIC_SCL, HIGH); delayMicroseconds(5);
    }
}

static bool dcs_sd_mount() {
    SD_MMC.setPins(WC_SDMMC_CLK, WC_SDMMC_CMD, WC_SDMMC_DATA);
    for (int attempt = 1; attempt <= DCS_SD_MOUNT_RETRIES; attempt++) {
        if (SD_MMC.begin("/sdcard", true)) {
            Serial.printf("[sd] mounted (attempt %d)\n", attempt);
            g_sd_ok = true;
            return true;
        }
        Serial.printf("[sd] mount fail (attempt %d/%d)\n", attempt, DCS_SD_MOUNT_RETRIES);
        delay(120);
    }
    g_sd_ok = false;
    Serial.println("[sd] no card -- running on defaults");
    return false;
}

static void dcs_hw_boot() {
    Serial.begin(115200);
    btStop();
    pinMode(WC_BOOT_BTN, INPUT_PULLUP);
    pinMode(WC_PWR_BTN_PIN, INPUT_PULLUP);

    if (!g_gfx->begin()) Serial.println("[gfx] begin FAILED");
    g_gfx->fillScreen(RGB565_BLACK);
    g_gfx->setBrightness(0);
    g_cur_brightness = 0;

    dcs_i2c_bus_recover();
    Wire.begin(WC_IIC_SDA, WC_IIC_SCL);
    Wire.setClock(400000);

    if (g_pmu.begin(Wire, AXP2101_SLAVE_ADDRESS, WC_IIC_SDA, WC_IIC_SCL)) {
        g_has_pmu = true;
        g_pmu.disableTSPinMeasure();
        g_pmu.enableBattVoltageMeasure();
        g_pmu.disableIRQ(XPOWERS_AXP2101_ALL_IRQ);
        g_pmu.clearIrqStatus();
        g_pmu.enableIRQ(XPOWERS_AXP2101_PKEY_SHORT_IRQ);
        g_pmu.setALDO1Voltage(3300); g_pmu.enableALDO1();
        g_pmu.setBLDO1Voltage(3300); g_pmu.enableBLDO1();
        delay(40);
    }

    pinMode(WC_TP_RESET, OUTPUT);
    digitalWrite(WC_TP_RESET, LOW);  delay(50);
    digitalWrite(WC_TP_RESET, HIGH); delay(200);
    pinMode(WC_TP_INT, INPUT_PULLUP);
    int t = 0;
    while (!g_ft->begin()) {
        Serial.println("[touch] FT3168 init fail");
        delay(200);
        if (++t > 50) { Serial.println("[touch] FT3168 giving up"); break; }
    }
    g_ft->IIC_Write_Device_State(
        g_ft->Arduino_IIC_Touch::Device::TOUCH_POWER_MODE,
        g_ft->Arduino_IIC_Touch::Device_Mode::TOUCH_POWER_MONITOR);

    lv_init();
    size_t px = WC_LCD_WIDTH * 60;
    g_lvgl_buf = (lv_color_t *)heap_caps_malloc(px * sizeof(lv_color_t),
                                                MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!g_lvgl_buf) {
        Serial.println("[lvgl] internal alloc failed -- PSRAM");
        g_lvgl_buf = (lv_color_t *)heap_caps_malloc(px * sizeof(lv_color_t), MALLOC_CAP_SPIRAM);
    }
    if (!g_lvgl_buf) {
        Serial.println("[lvgl] PSRAM failed -- 8BIT");
        g_lvgl_buf = (lv_color_t *)heap_caps_malloc(px * sizeof(lv_color_t), MALLOC_CAP_8BIT);
    }
    if (!g_lvgl_buf) {
        Serial.println("[lvgl] FATAL: no draw buffer -- halting");
        while (1) delay(1000);
    }
    lv_disp_draw_buf_init(&g_draw_buf, g_lvgl_buf, NULL, px);
    lv_disp_drv_init(&g_disp_drv);
    g_disp_drv.flush_cb = dcs_flush;
    g_disp_drv.draw_buf = &g_draw_buf;
    g_disp_drv.hor_res = WC_LCD_WIDTH;
    g_disp_drv.ver_res = WC_LCD_HEIGHT;
    lv_disp_drv_register(&g_disp_drv);

    lv_indev_drv_init(&g_indev_drv);
    g_indev_drv.type = LV_INDEV_TYPE_POINTER;
    g_indev_drv.read_cb = dcs_touch;
    g_touch_indev = lv_indev_drv_register(&g_indev_drv);

    g_last_touch_ms = millis();
    dcs_sd_mount();
}

// ---------------------------------------------------------------------------
//  6.  FILESYSTEM + FLAT-JSON HELPERS
// ---------------------------------------------------------------------------
//  Paths are validated, never taken from a config file verbatim.  Writes are
//  temp-file + rename so a yanked card cannot leave a half-written config.
static bool path_under_watch(const char *p) {
    if (!p) return false;
    if (strncmp(p, "/WATCH/", 7) != 0) return false;
    if (strstr(p, "..")) return false;
    return true;
}
static bool fs_exists(const char *p) { return g_sd_ok && path_under_watch(p) && SD_MMC.exists(p); }

static bool fs_ensure_dir(const char *path) {
    if (!g_sd_ok || !path_under_watch(path)) return false;
    char buf[96];
    size_t n = strlen(path);
    if (n >= sizeof(buf)) return false;
    memcpy(buf, path, n + 1);
    for (char *p = buf + 1; *p; ++p) {
        if (*p == '/') { *p = 0; SD_MMC.mkdir(buf); *p = '/'; }
    }
    SD_MMC.mkdir(buf);
    return true;
}

static bool fs_read_text(const char *path, char *buf, size_t cap) {
    if (!buf || cap < 2 || !g_sd_ok || !path_under_watch(path)) return false;
    buf[0] = 0;
    File f = SD_MMC.open(path, FILE_READ);
    if (!f || f.isDirectory()) return false;
    size_t want = cap - 1;
    if ((size_t)f.size() < want) want = (size_t)f.size();
    size_t got = f.read((uint8_t *)buf, want);
    f.close();
    buf[got] = 0;
    return got > 0;
}

static bool fs_write_atomic(const char *path, const char *data, size_t len) {
    if (!g_sd_ok || !path_under_watch(path) || !data) return false;
    char tmp[112];
    if (snprintf(tmp, sizeof(tmp), "%s.tmp", path) >= (int)sizeof(tmp)) return false;
    if (SD_MMC.exists(tmp)) SD_MMC.remove(tmp);
    File f = SD_MMC.open(tmp, FILE_WRITE);
    if (!f) return false;
    size_t wrote = f.write((const uint8_t *)data, len);
    f.flush();
    f.close();
    if (wrote != len) { SD_MMC.remove(tmp); return false; }
    if (SD_MMC.exists(path)) SD_MMC.remove(path);
    return SD_MMC.rename(tmp, path);
}

// Recursive delete, used only by the factory-reset sheet.  Names are copied
// out before recursing: the File handle must not be open while its directory
// is being modified.  Depth is bounded by the path length, never by the card.
static void fs_rm_tree(const char *path) {
    if (!g_sd_ok || !path_under_watch(path)) return;
    File d = SD_MMC.open(path);
    if (!d) return;
    if (!d.isDirectory()) { d.close(); SD_MMC.remove(path); return; }
    std::vector<String> kids;
    File f = d.openNextFile();
    while (f && kids.size() < 96) {
        String n = f.name();
        int s = n.lastIndexOf('/');
        if (s >= 0) n = n.substring(s + 1);
        if (n.length()) kids.push_back(n);
        f = d.openNextFile();
    }
    d.close();
    for (size_t i = 0; i < kids.size(); i++) {
        char child[112];
        snprintf(child, sizeof(child), "%s/%s", path, kids[i].c_str());
        if (strlen(child) >= sizeof(child) - 2) continue;
        fs_rm_tree(child);
    }
    SD_MMC.rmdir(path);
}

static bool fs_append(const char *path, const char *line) {
    if (!g_sd_ok || !path_under_watch(path)) return false;
    File f = SD_MMC.open(path, FILE_APPEND);
    if (!f) return false;
    f.print(line);
    f.close();
    return true;
}

// Flat-JSON reader: enough for our own config files (no nested objects, no
// arrays in configs).  Values are returned trimmed, strings unescaped.
static const char *jfind_key(const char *js, const char *key) {
    if (!js || !key || !*key) return NULL;
    size_t klen = strlen(key);
    const char *p = js;
    while ((p = strchr(p, '"')) != NULL) {
        if (strncmp(p + 1, key, klen) == 0 && p[1 + klen] == '"') return p + 2 + klen;
        p++;
    }
    return NULL;
}

static bool jget_str(const char *js, const char *key, char *out, size_t cap, const char *def) {
    if (!out || cap < 2) return false;
    out[0] = 0;
    if (def) { strncpy(out, def, cap - 1); out[cap - 1] = 0; }
    if (!js || !key) return false;
    const char *p = jfind_key(js, key);
    if (!p) return false;
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
    if (*p != ':') return false;
    p++;
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
    size_t o = 0;
    if (*p == '"') {
        p++;
        while (*p && *p != '"' && o + 1 < cap) {
            if (*p == '\\' && p[1]) p++;
            out[o++] = *p++;
        }
    } else {
        while (*p && *p != ',' && *p != '}' && *p != '\n' && *p != '\r' && o + 1 < cap)
            out[o++] = *p++;
        while (o && (out[o - 1] == ' ' || out[o - 1] == '\t')) o--;
    }
    out[o] = 0;
    return true;
}

static long jget_int(const char *js, const char *key, long def) {
    char b[24];
    if (!jget_str(js, key, b, sizeof(b), NULL) || !b[0]) return def;
    return strtol(b, NULL, 10);
}
static bool jget_bool(const char *js, const char *key, bool def) {
    char b[12];
    if (!jget_str(js, key, b, sizeof(b), NULL)) return def;
    if (!strcmp(b, "true") || !strcmp(b, "1")) return true;
    if (!strcmp(b, "false") || !strcmp(b, "0")) return false;
    return def;
}

// JSON writer helpers.
static void json_escape(char *dst, size_t cap, const char *src) {
    if (!dst || cap < 2) return;
    size_t o = 0;
    for (const char *p = src ? src : ""; *p && o + 2 < cap; ++p) {
        char c = *p;
        if (c == '"' || c == '\\') { dst[o++] = '\\'; dst[o++] = c; }
        else if (c == '\n' || c == '\r' || c == '\t') dst[o++] = ' ';
        else dst[o++] = c;
    }
    dst[o] = 0;
}

// ---------------------------------------------------------------------------
//  7.  DIAGNOSTICS  (bounded logs; never notification content by default)
// ---------------------------------------------------------------------------
#define DCS_LOG_MAX_BYTES 6000

static void dcs_log(const char *osdir, const char *fmt, ...) {
    char line[176];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    Serial.printf("[dcs] %s\n", line);
    if (!g_sd_ok || !osdir) return;

    char dir[64], path[96], rot[96];
    snprintf(dir, sizeof(dir), "%s/logs", osdir);
    fs_ensure_dir(dir);
    snprintf(path, sizeof(path), "%s/logs/diag.log", osdir);
    snprintf(rot, sizeof(rot), "%s/logs/diag.1", osdir);

    File f = SD_MMC.open(path, FILE_READ);
    size_t sz = f ? (size_t)f.size() : 0;
    if (f) f.close();
    if (sz > DCS_LOG_MAX_BYTES) {
        if (SD_MMC.exists(rot)) SD_MMC.remove(rot);
        SD_MMC.rename(path, rot);
    }
    char out[200];
    snprintf(out, sizeof(out), "%lu %s\n", (unsigned long)millis(), line);
    fs_append(path, out);
}
// ---------------------------------------------------------------------------
//  8.  SHARED SETTINGS + PER-OS CONFIG
// ---------------------------------------------------------------------------
//  Owner map (one file, one owner -- never duplicated per OS):
//      /WATCH/SHARED/settings.json  -> SettingsService (brightness, profile,
//                                      clock format, privacy, active OS)
//      /WATCH/SHARED/inbox.jsonl    -> NotificationService
//      /WATCH/SHARED/timers.json    -> ClockService
//      /WATCH/<OS>/config/*.json    -> that experience only
enum PwrProfile : uint8_t { PWR_PERF = 0, PWR_BALANCED = 1, PWR_ECO = 2, PWR_SURVIVAL = 3 };
static const char *kPwrName[4] = { "PERFORMANCE", "BALANCED", "ECO", "SURVIVAL" };
static const char *kPrivacyName[3] = { "FULL PREVIEW", "SENDER ONLY", "HIDDEN" };
static const char *kQuietName[3]   = { "OFF", "SENDER ONLY", "HIDDEN" };

static int g_cur_os = -1;      // -1 = chooser, 0 = NoirOS, 1 = CrownOS, 2 = NightOS
static bool g_crown_intro = false;   // one-shot signature reveal on entry

struct SharedCfg {
    uint8_t brightness;   // 20..255
    uint8_t power;        // PwrProfile
    uint8_t t24;          // 1 = 24-hour
    uint8_t privacy;      // 0 full, 1 sender only, 2 hidden
    uint8_t auto_power;   // auto low-battery profile changes
    uint8_t active_os;    // restored on boot
    uint8_t vib;
    uint8_t sleep_min;    // minutes idle before deep sleep; 0 = never
    uint8_t theme;        // 0 = each OS's own accent, 1..6 = shared override
    uint8_t wall;         // NoirOS wallpaper: 0 = none, n = photo n-1
    uint8_t auto_ntp;     // 1 = top the clock up over Wi-Fi when it is due
    uint8_t bootanim;     // 0 = off, 1 = full sequence, 2 = quick
};
static SharedCfg g_cfg = { DCS_DEFAULT_BRIGHTNESS, PWR_BALANCED, 1, 0, 1, 0, 0, 5, 0, 0, 1, 1 };

struct NoirCfg {
    uint16_t focus_min, break_min;
    uint8_t  clock_style;   // 0 digital, 1 analogue
    uint8_t  tiles;         // bitmask of enabled smart tiles
    uint8_t  vibe;
    uint8_t  privacy;       // 0..2 = tighter than global, 3 = follow global
};
static NoirCfg g_noir = { 25, 5, 0, 0x1F, 1, 3 };

struct CrownCfg {
    uint8_t face;      // index into g_faces
    uint8_t profile;   // occasion index
    uint8_t accent;    // palette index
    uint8_t bg;        // palette index
    uint8_t compl_on;     // complications on/off
    uint8_t anim;      // 1 = signature animations allowed
    uint8_t secs;      // 1 = seconds hand (0 saves redraws and power)
    uint8_t alerts;    // 1 = raise a shared notification 15 min before an event
    uint8_t glitch;    // 0 off, 1 rare (~12 s), 2 often (~5 s)
};
static CrownCfg g_crown = { 6, 2, 0, 0, 1, 1, 1, 0, 1 };

struct NightCfg {
    uint8_t shadow;
    uint8_t brightness;   // NightOS's own target (30..255)
    uint8_t quiet;        // 0 off, 1 sender only, 2 hidden
    uint8_t sched_on;
    uint8_t sched_start;  // hour 0..23
    uint8_t sched_end;
    uint8_t days;         // bit0 Sun .. bit6 Sat
    uint8_t palette;      // 0 near-black/grey, 1 crimson
};
static NightCfg g_night = { 0, 70, 1, 0, 22, 6, 0x7F, 0 };

static void sharedcfg_load() {
    char buf[768];
    if (!fs_read_text("/WATCH/SHARED/settings.json", buf, sizeof(buf))) return;
    g_cfg.brightness = (uint8_t)jget_int(buf, "brightness", g_cfg.brightness);
    g_cfg.power      = (uint8_t)jget_int(buf, "power",      g_cfg.power);
    g_cfg.t24        = (uint8_t)jget_int(buf, "t24",        g_cfg.t24);
    g_cfg.privacy    = (uint8_t)jget_int(buf, "privacy",    g_cfg.privacy);
    g_cfg.auto_power = (uint8_t)jget_int(buf, "auto_power", g_cfg.auto_power);
    g_cfg.active_os  = (uint8_t)jget_int(buf, "active_os",  g_cfg.active_os);
    g_cfg.vib        = (uint8_t)jget_int(buf, "vib",        g_cfg.vib);
    g_cfg.sleep_min  = (uint8_t)jget_int(buf, "sleep_min",  g_cfg.sleep_min);
    g_cfg.theme      = (uint8_t)jget_int(buf, "theme",      g_cfg.theme);
    g_cfg.wall       = (uint8_t)jget_int(buf, "wall",       g_cfg.wall);
    g_cfg.auto_ntp   = (uint8_t)jget_int(buf, "auto_ntp",   g_cfg.auto_ntp);
    g_cfg.bootanim   = (uint8_t)jget_int(buf, "bootanim",   g_cfg.bootanim);
    if (g_cfg.bootanim > 2) g_cfg.bootanim = 1;
    if (g_cfg.theme > 6) g_cfg.theme = 0;
    if (g_cfg.wall > 12) g_cfg.wall = 0;
    if (g_cfg.auto_ntp > 1) g_cfg.auto_ntp = 1;
    if (g_cfg.brightness < DCS_MIN_BRIGHTNESS) g_cfg.brightness = DCS_DEFAULT_BRIGHTNESS;
    if (g_cfg.power > PWR_SURVIVAL) g_cfg.power = PWR_BALANCED;
    if (g_cfg.privacy > 2) g_cfg.privacy = 0;
    if (g_cfg.active_os > 2) g_cfg.active_os = 0;
    if (g_cfg.sleep_min != 0 && g_cfg.sleep_min != 1 && g_cfg.sleep_min != 5 &&
        g_cfg.sleep_min != 15 && g_cfg.sleep_min != 30) g_cfg.sleep_min = 5;
}

static void sharedcfg_save() {
    char buf[512];
    snprintf(buf, sizeof(buf),
        "{\n"
        "  \"schema\": 3,\n"
        "  \"brightness\": %u,\n"
        "  \"power\": %u,\n"
        "  \"t24\": %u,\n"
        "  \"privacy\": %u,\n"
        "  \"auto_power\": %u,\n"
        "  \"active_os\": %u,\n"
        "  \"vib\": %u,\n"
        "  \"sleep_min\": %u,\n"
        "  \"theme\": %u,\n"
        "  \"wall\": %u,\n"
        "  \"auto_ntp\": %u,\n"
        "  \"bootanim\": %u\n"
        "}\n",
        (unsigned)g_cfg.brightness, (unsigned)g_cfg.power, (unsigned)g_cfg.t24,
        (unsigned)g_cfg.privacy, (unsigned)g_cfg.auto_power,
        (unsigned)g_cfg.active_os, (unsigned)g_cfg.vib, (unsigned)g_cfg.sleep_min,
        (unsigned)g_cfg.theme, (unsigned)g_cfg.wall, (unsigned)g_cfg.auto_ntp,
        (unsigned)g_cfg.bootanim);
    fs_write_atomic("/WATCH/SHARED/settings.json", buf, strlen(buf));
}

static void noir_load() {
    char buf[512];
    if (fs_read_text("/WATCH/NoirOS/config/settings.json", buf, sizeof(buf))) {
        g_noir.focus_min   = (uint16_t)jget_int(buf, "focus_min",   g_noir.focus_min);
        g_noir.break_min   = (uint16_t)jget_int(buf, "break_min",   g_noir.break_min);
        g_noir.clock_style = (uint8_t)jget_int(buf, "clock_style",  g_noir.clock_style);
        g_noir.tiles       = (uint8_t)jget_int(buf, "tiles",        g_noir.tiles);
        g_noir.vibe        = (uint8_t)jget_int(buf, "vibe",         g_noir.vibe);
        g_noir.privacy     = (uint8_t)jget_int(buf, "privacy",      g_noir.privacy);
    }
    if (g_noir.focus_min < 1 || g_noir.focus_min > 180) g_noir.focus_min = 25;
    if (g_noir.break_min > 60) g_noir.break_min = 5;
    if (g_noir.privacy > 3) g_noir.privacy = 3;   // 3 = follow the shared setting
}
static void noir_save() {
    char buf[320];
    snprintf(buf, sizeof(buf),
        "{\n  \"schema\": 1,\n  \"focus_min\": %u,\n  \"break_min\": %u,\n"
        "  \"clock_style\": %u,\n  \"tiles\": %u,\n  \"vibe\": %u,\n  \"privacy\": %u\n}\n",
        (unsigned)g_noir.focus_min, (unsigned)g_noir.break_min,
        (unsigned)g_noir.clock_style, (unsigned)g_noir.tiles,
        (unsigned)g_noir.vibe, (unsigned)g_noir.privacy);
    fs_write_atomic("/WATCH/NoirOS/config/settings.json", buf, strlen(buf));
}

static void crown_load() {
    char buf[512];
    if (fs_read_text("/WATCH/CrownOS/config/settings.json", buf, sizeof(buf))) {
        g_crown.face    = (uint8_t)jget_int(buf, "face",    g_crown.face);
        g_crown.profile = (uint8_t)jget_int(buf, "profile", g_crown.profile);
        g_crown.accent  = (uint8_t)jget_int(buf, "accent",  g_crown.accent);
        g_crown.bg      = (uint8_t)jget_int(buf, "bg",      g_crown.bg);
        g_crown.compl_on   = (uint8_t)jget_int(buf, "compl",   g_crown.compl_on);
        g_crown.anim    = (uint8_t)jget_int(buf, "anim",    g_crown.anim);
        g_crown.secs    = (uint8_t)jget_int(buf, "secs",    g_crown.secs);
        g_crown.alerts  = (uint8_t)jget_int(buf, "alerts",  g_crown.alerts);
        g_crown.glitch  = (uint8_t)jget_int(buf, "glitch",  g_crown.glitch);
    }
    if (g_crown.profile > 3) g_crown.profile = 2;
    if (g_crown.glitch > 2) g_crown.glitch = 1;
}
static void crown_save() {
    char buf[320];
    snprintf(buf, sizeof(buf),
        "{\n  \"schema\": 3,\n  \"face\": %u,\n  \"profile\": %u,\n  \"accent\": %u,\n"
        "  \"bg\": %u,\n  \"compl\": %u,\n  \"anim\": %u,\n  \"secs\": %u,\n  \"alerts\": %u,\n"
        "  \"glitch\": %u\n}\n",
        (unsigned)g_crown.face, (unsigned)g_crown.profile, (unsigned)g_crown.accent,
        (unsigned)g_crown.bg, (unsigned)g_crown.compl_on, (unsigned)g_crown.anim,
        (unsigned)g_crown.secs, (unsigned)g_crown.alerts, (unsigned)g_crown.glitch);
    fs_write_atomic("/WATCH/CrownOS/config/settings.json", buf, strlen(buf));
}

static void night_load() {
    char buf[512];
    if (fs_read_text("/WATCH/NightOS/config/settings.json", buf, sizeof(buf))) {
        g_night.shadow      = (uint8_t)jget_int(buf, "shadow",      g_night.shadow);
        g_night.brightness  = (uint8_t)jget_int(buf, "brightness",  g_night.brightness);
        g_night.quiet       = (uint8_t)jget_int(buf, "quiet",       g_night.quiet);
        g_night.sched_on    = (uint8_t)jget_int(buf, "sched_on",    g_night.sched_on);
        g_night.sched_start = (uint8_t)jget_int(buf, "sched_start", g_night.sched_start);
        g_night.sched_end   = (uint8_t)jget_int(buf, "sched_end",   g_night.sched_end);
        g_night.days        = (uint8_t)jget_int(buf, "days",        g_night.days);
        g_night.palette     = (uint8_t)jget_int(buf, "palette",     g_night.palette);
    }
    if (g_night.brightness < 30) g_night.brightness = 70;
    if (g_night.quiet > 2) g_night.quiet = 1;
    if (g_night.palette > 1) g_night.palette = 0;
    if (g_night.sched_start > 23) g_night.sched_start = 22;
    if (g_night.sched_end > 23)   g_night.sched_end = 6;
}
static void night_save() {
    char buf[384];
    snprintf(buf, sizeof(buf),
        "{\n  \"schema\": 3,\n  \"shadow\": %u,\n  \"brightness\": %u,\n  \"quiet\": %u,\n"
        "  \"sched_on\": %u,\n  \"sched_start\": %u,\n  \"sched_end\": %u,\n  \"days\": %u,\n"
        "  \"palette\": %u\n}\n",
        (unsigned)g_night.shadow, (unsigned)g_night.brightness, (unsigned)g_night.quiet,
        (unsigned)g_night.sched_on, (unsigned)g_night.sched_start,
        (unsigned)g_night.sched_end, (unsigned)g_night.days,
        (unsigned)g_night.palette);
    fs_write_atomic("/WATCH/NightOS/config/settings.json", buf, strlen(buf));
}

// ---- manifests -------------------------------------------------------------
//  Validated before launch: id must match, API must be understood, and the
//  directory must exist.  A missing manifest is created with defaults; a
//  malformed one produces a readable error, never a crash or a wipe.
struct Manifest { const char *dir; const char *id; const char *name; const char *features; };
static const Manifest kManifests[3] = {
    { DCS_PATH_NOIR,  "noir",  "NoirOS",
      "\"notifications\", \"media\", \"focus\", \"smart_tiles\", \"world_clock\", "
      "\"agenda\", \"weather\", \"alarms\", \"quick_sheet\", \"wallpaper\", "
      "\"time_sync\", \"themes\", \"factory_reset\", \"deep_sleep\"" },
    { DCS_PATH_CROWN, "crown", "CrownOS",
      "\"watch_faces\", \"watch_studio\", \"occasions\", \"moon_phase\", "
      "\"event_alerts\", \"alarms\", \"quick_sheet\", \"time_sync\", \"themes\", "
      "\"factory_reset\", \"deep_sleep\"" },
    { DCS_PATH_NIGHT, "night", "NightOS",
      "\"shadow_mode\", \"quiet_alerts\", \"utility_panel\", \"night_schedule\", "
      "\"orientation\", \"breathing\", \"stealth\", \"red_light\", \"wake_alarm\", "
      "\"quick_sheet\", \"time_sync\", \"themes\", \"factory_reset\", \"deep_sleep\"" },
};

static void manifest_ensure(int os) {
    if (os < 0 || os > 2 || !g_sd_ok) return;
    char path[80], buf[560];
    snprintf(path, sizeof(path), "%s/manifest.json", kManifests[os].dir);
    if (fs_exists(path)) return;
    fs_ensure_dir(kManifests[os].dir);
    snprintf(buf, sizeof(buf),
        "{\n  \"id\": \"%s\",\n  \"name\": \"%s\",\n  \"version\": \"%s\",\n"
        "  \"entry\": \"%s\",\n  \"min_firmware_api\": %d,\n  \"features\": [%s]\n}\n",
        kManifests[os].id, kManifests[os].name, DCS_VERSION,
        kManifests[os].id, DCS_MANIFEST_API, kManifests[os].features);
    fs_write_atomic(path, buf, strlen(buf));
}

static bool manifest_validate(int os, char *err, size_t errcap) {
    if (err && errcap) err[0] = 0;
    if (os < 0 || os > 2) {
        if (err) snprintf(err, errcap, "Unknown experience");
        return false;
    }
    manifest_ensure(os);
    char path[80], buf[560], id[24] = "";
    snprintf(path, sizeof(path), "%s/manifest.json", kManifests[os].dir);
    if (!fs_read_text(path, buf, sizeof(buf))) {
        // No card is not a fatal error: the OS still runs on defaults.
        if (!g_sd_ok) return true;
        if (err) snprintf(err, errcap, "manifest.json missing");
        return false;
    }
    if (!jget_str(buf, "id", id, sizeof(id), NULL) || strcmp(id, kManifests[os].id) != 0) {
        if (err) snprintf(err, errcap, "manifest id mismatch");
        return false;
    }
    long api = jget_int(buf, "min_firmware_api", DCS_MANIFEST_API);
    if (api > DCS_MANIFEST_API) {
        if (err) snprintf(err, errcap, "needs API %ld, module is %d", api, DCS_MANIFEST_API);
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
//  8.9  DAILY COUNTERS  --  the numbers the three dashboards show
// ---------------------------------------------------------------------------
//  Three files, three owners, no duplication:
//      /WATCH/SHARED/day.json                  -> screen-on time, notifications
//      /WATCH/NoirOS/data/day.json             -> water, focus time
//      /WATCH/NightOS/data/activity_sessions.json -> minutes in Shadow Mode
//  Everything rolls over at local midnight.  A rollover saves the old day
//  first, so a score is never silently erased by crossing 00:00.
#define WATER_MAX 12
static bool clock_valid();              // section 11
static bool cd_running();               // section 11
static uint32_t g_stat_on_ms     = 0;   // screen-on today (shared)
static uint16_t g_stat_notif     = 0;   // notifications received today (shared)
static uint16_t g_water          = 0;   // NoirOS: glasses today (0..WATER_MAX)
static uint32_t g_stat_focus_ms  = 0;   // NoirOS: focus time today
static uint32_t g_stat_shadow_ms = 0;   // NightOS: time in Shadow Mode today
static uint16_t g_shadow_sessions = 0;  // NightOS: shadow activations this session
static char     g_stat_date[12]  = "";
static uint32_t g_stat_prev_ms   = 0;
static uint8_t  g_batt_boot      = 0;   // battery percentage at cold boot
static uint8_t  g_batt_boot_ok   = 0;

static void stat_today(char *out, size_t n) {
    if (!clock_valid()) { snprintf(out, n, "----"); return; }
    time_t t; time(&t);
    struct tm lt; localtime_r(&t, &lt);
    snprintf(out, n, "%04d-%02d-%02d", lt.tm_year + 1900, lt.tm_mon + 1, lt.tm_mday);
}

static bool stat_same_day(const char *buf, const char *today) {
    char saved[16]; saved[0] = 0;
    if (!jget_str(buf, "date", saved, sizeof(saved), NULL)) return false;
    return !strcmp(saved, today);
}

static void stat_load() {
    stat_today(g_stat_date, sizeof(g_stat_date));
    char buf[320];
    if (fs_read_text("/WATCH/SHARED/day.json", buf, sizeof(buf)) &&
        stat_same_day(buf, g_stat_date)) {
        g_stat_on_ms = (uint32_t)jget_int(buf, "on_ms", 0);
        g_stat_notif = (uint16_t)jget_int(buf, "notif", 0);
    }
    if (fs_read_text("/WATCH/NoirOS/data/day.json", buf, sizeof(buf)) &&
        stat_same_day(buf, g_stat_date)) {
        g_water         = (uint16_t)jget_int(buf, "water", 0);
        g_stat_focus_ms = (uint32_t)jget_int(buf, "focus_ms", 0);
    }
    if (fs_read_text("/WATCH/NightOS/data/activity_sessions.json", buf, sizeof(buf)) &&
        stat_same_day(buf, g_stat_date)) {
        g_stat_shadow_ms = (uint32_t)jget_int(buf, "shadow_ms", 0);
    }
    if (g_water > WATER_MAX) g_water = WATER_MAX;
    // A counter file for a previous day is stale, not corrupt: keep the file
    // untouched (it is still the last day's record) and start from zero.
    g_stat_prev_ms = millis();
    dcs_log(DCS_PATH_SHARED, "day %s on=%lus notif=%u water=%u", g_stat_date,
            (unsigned long)(g_stat_on_ms / 1000UL), (unsigned)g_stat_notif,
            (unsigned)g_water);
}

static void stat_save() {
    if (!g_sd_ok) return;
    char buf[200], today[12];
    stat_today(today, sizeof(today));
    if (!strcmp(today, "----")) return;      // no valid clock: never date a blank day
    snprintf(buf, sizeof(buf),
             "{\n  \"schema\": 1,\n  \"date\": \"%s\",\n  \"on_ms\": %lu,\n"
             "  \"notif\": %u\n}\n",
             today, (unsigned long)g_stat_on_ms, (unsigned)g_stat_notif);
    fs_write_atomic("/WATCH/SHARED/day.json", buf, strlen(buf));

    snprintf(buf, sizeof(buf),
             "{\n  \"schema\": 1,\n  \"date\": \"%s\",\n  \"water\": %u,\n"
             "  \"focus_ms\": %lu\n}\n",
             today, (unsigned)g_water, (unsigned long)g_stat_focus_ms);
    fs_write_atomic("/WATCH/NoirOS/data/day.json", buf, strlen(buf));

    snprintf(buf, sizeof(buf),
             "{\n  \"schema\": 1,\n  \"date\": \"%s\",\n  \"shadow_ms\": %lu\n}\n",
             today, (unsigned long)g_stat_shadow_ms);
    fs_write_atomic("/WATCH/NightOS/data/activity_sessions.json", buf, strlen(buf));
}

//  Runs once per loop(), including while the panel is off.  Light sleep keeps
//  millis(), so a dark watch still accrues screen-off time correctly.
static void stat_service() {
    uint32_t now = millis();
    uint32_t dt = now - g_stat_prev_ms;
    g_stat_prev_ms = now;
    if (dt > 5000UL) dt = 5000UL;      // a long block in setup is not screen time
    if (!g_screen_off) g_stat_on_ms += dt;
    if (cd_running()) g_stat_focus_ms += dt;
    if (g_cur_os == 2 && g_night.shadow) g_stat_shadow_ms += dt;

    char today[12];
    stat_today(today, sizeof(today));
    if (strcmp(today, "----") && strcmp(today, g_stat_date)) {
        stat_save();                   // close the old day before zeroing
        g_stat_on_ms = g_stat_focus_ms = g_stat_shadow_ms = 0;
        g_stat_notif = 0;
        g_water = 0;
        snprintf(g_stat_date, sizeof(g_stat_date), "%s", today);
        dcs_log(DCS_PATH_SHARED, "day rollover -> %s", g_stat_date);
    }
    static uint32_t last_save = 0;
    if (now - last_save >= 120000UL) { last_save = now; stat_save(); }
}

static void water_add(int delta) {
    int v = (int)g_water + delta;
    if (v < 0) v = 0;
    if (v > WATER_MAX) v = WATER_MAX;
    g_water = (uint16_t)v;
    stat_save();
}

// ---------------------------------------------------------------------------
//  9.  NOTIFICATION SERVICE  (shared inbox, independent of the active OS)
// ---------------------------------------------------------------------------
#define NOTIF_MAX 24
struct Notif {
    uint32_t id;
    uint32_t ts;
    uint8_t  read;
    uint8_t  priv;       // originating privacy hint (0/1/2)
    char     pid[20];    // protocol id, for updates/removals
    char     app[20];
    char     sender[24];
    char     title[44];
    char     body[100];
};
static Notif g_nf[NOTIF_MAX];
static int g_nf_n = 0;
static uint32_t g_nf_seq = 1;
static int g_nf_overflow = 0;

static void notif_save();

static void copyf(char *dst, size_t cap, const char *src) {
    if (!dst || cap == 0) return;
    if (!src) { dst[0] = 0; return; }
    strncpy(dst, src, cap - 1);
    dst[cap - 1] = 0;
}
static void notif_clean(char *s) {
    for (; s && *s; ++s)
        if (*s == '\t' || *s == '\n' || *s == '\r' || *s == '|') *s = ' ';
}

static void notif_remove_pid(const char *pid) {
    if (!pid || !*pid) return;
    for (int i = 0; i < g_nf_n; i++) {
        if (g_nf[i].pid[0] && !strcmp(g_nf[i].pid, pid)) {
            memmove(&g_nf[i], &g_nf[i + 1], (size_t)(g_nf_n - i - 1) * sizeof(Notif));
            g_nf_n--;
            notif_save();
            return;
        }
    }
}

static int notif_add(const char *app, const char *sender, const char *title,
                     const char *body, const char *pid) {
    // Protocol updates: same pid replaces the record in place.
    if (pid && *pid) {
        for (int i = 0; i < g_nf_n; i++) {
            if (g_nf[i].pid[0] && !strcmp(g_nf[i].pid, pid)) {
                copyf(g_nf[i].app, sizeof(g_nf[i].app), app);
                copyf(g_nf[i].sender, sizeof(g_nf[i].sender), sender);
                copyf(g_nf[i].title, sizeof(g_nf[i].title), title);
                copyf(g_nf[i].body, sizeof(g_nf[i].body), body);
                notif_clean(g_nf[i].app); notif_clean(g_nf[i].sender);
                notif_clean(g_nf[i].title); notif_clean(g_nf[i].body);
                g_nf[i].ts = (uint32_t)time(NULL);
                g_nf[i].read = 0;
                notif_save();
                return i;
            }
        }
    }
    if (g_nf_n >= NOTIF_MAX) {
        // Deterministic overflow: drop the oldest read record first, otherwise
        // the actual oldest.  Counted, and surfaced as an overflow badge.
        int drop = 0;
        for (int i = 0; i < g_nf_n; i++) if (g_nf[i].read) { drop = i; break; }
        memmove(&g_nf[drop], &g_nf[drop + 1], (size_t)(g_nf_n - drop - 1) * sizeof(Notif));
        g_nf_n--;
        g_nf_overflow++;
    }
    Notif &n = g_nf[g_nf_n];
    memset(&n, 0, sizeof(n));
    n.id = g_nf_seq++;
    n.ts = (uint32_t)time(NULL);
    copyf(n.app, sizeof(n.app), app ? app : "Phone");
    copyf(n.sender, sizeof(n.sender), sender ? sender : "");
    copyf(n.title, sizeof(n.title), title ? title : "");
    copyf(n.body, sizeof(n.body), body ? body : "");
    copyf(n.pid, sizeof(n.pid), pid ? pid : "");
    notif_clean(n.app); notif_clean(n.sender); notif_clean(n.title); notif_clean(n.body);
    if (g_stat_notif < 999) g_stat_notif++;      // today's tally for the dashboards
    return g_nf_n++;
}

static int notif_unread() {
    int u = 0;
    for (int i = 0; i < g_nf_n; i++) if (!g_nf[i].read) u++;
    return u;
}
static void notif_mark_all_read() {
    for (int i = 0; i < g_nf_n; i++) g_nf[i].read = 1;
    notif_save();
}
static void notif_dismiss(int idx) {
    if (idx < 0 || idx >= g_nf_n) return;
    memmove(&g_nf[idx], &g_nf[idx + 1], (size_t)(g_nf_n - idx - 1) * sizeof(Notif));
    g_nf_n--;
    notif_save();
}
static void notif_clear_all() {
    g_nf_n = 0;
    g_nf_overflow = 0;
    notif_save();
}
static void notif_clear_read() {
    int out = 0;
    for (int i = 0; i < g_nf_n; i++)
        if (!g_nf[i].read) { if (out != i) g_nf[out] = g_nf[i]; out++; }
    if (out == g_nf_n) return;
    g_nf_n = out;
    notif_save();
}

static int split_tabs(char *s, char *out[], int max) {
    int n = 0;
    char *p = s;
    while (n < max) {
        out[n++] = p;
        char *t = strchr(p, '\t');
        if (!t) break;
        *t = 0;
        p = t + 1;
    }
    return n;
}

static void notif_save() {
    if (!g_sd_ok) return;
    static char buf[9000];
    size_t o = 0;
    for (int i = 0; i < g_nf_n; i++) {
        Notif &n = g_nf[i];
        int w = snprintf(buf + o, sizeof(buf) - o, "%lu\t%lu\t%d\t%d\t%s\t%s\t%s\t%s\t%s\n",
                         (unsigned long)n.id, (unsigned long)n.ts, n.read ? 1 : 0,
                         n.priv, n.pid, n.app, n.sender, n.title, n.body);
        if (w < 0 || o + (size_t)w >= sizeof(buf)) break;
        o += (size_t)w;
    }
    fs_write_atomic("/WATCH/SHARED/inbox.jsonl", buf, o);
}

static void notif_load() {
    static char buf[9000];
    g_nf_n = 0;
    if (!fs_read_text("/WATCH/SHARED/inbox.jsonl", buf, sizeof(buf))) return;
    char *save = NULL;
    for (char *line = strtok_r(buf, "\n", &save); line && g_nf_n < NOTIF_MAX;
         line = strtok_r(NULL, "\n", &save)) {
        if (strlen(line) < 8) continue;
        char *f[9];
        int n = split_tabs(line, f, 9);
        if (n < 9) continue;
        Notif &e = g_nf[g_nf_n];
        memset(&e, 0, sizeof(e));
        e.id = (uint32_t)strtoul(f[0], NULL, 10);
        e.ts = (uint32_t)strtoul(f[1], NULL, 10);
        e.read = (uint8_t)atoi(f[2]);
        e.priv = (uint8_t)atoi(f[3]);
        copyf(e.pid, sizeof(e.pid), f[4]);
        copyf(e.app, sizeof(e.app), f[5]);
        copyf(e.sender, sizeof(e.sender), f[6]);
        copyf(e.title, sizeof(e.title), f[7]);
        copyf(e.body, sizeof(e.body), f[8]);
        if (e.id >= g_nf_seq) g_nf_seq = e.id + 1;
        g_nf_n++;
    }
}

// Presentation policy is enforced HERE, not in the three UIs, so a
// notification can never leak through an experience that forgot to apply it.
static uint8_t notif_privacy(void) {
    uint8_t p = g_cfg.privacy;
    if (g_cur_os == 0 && g_noir.privacy < p) p = g_noir.privacy;
    if (g_cur_os == 1 && g_crown.profile == 0 && p < 1) p = 1;   // Formal
    if (g_cur_os == 2) {
        if (g_night.quiet > p) p = g_night.quiet;
        if (g_night.shadow && p < 1) p = 1;                      // Shadow Mode
    }
    return p > 2 ? 2 : p;
}

static void notif_present(int idx, char *t, size_t tn, char *b, size_t bn) {
    if (!t || !tn || !b || !bn) return;
    t[0] = 0; b[0] = 0;
    if (idx < 0 || idx >= g_nf_n) return;
    Notif &n = g_nf[idx];
    uint8_t p = notif_privacy();
    if (p == 2) {
        snprintf(t, tn, "%s", n.app);
        snprintf(b, bn, "New notification");
        return;
    }
    if (p == 1) {
        snprintf(t, tn, "%s", n.sender[0] ? n.sender : n.app);
        snprintf(b, bn, "Preview hidden");
        return;
    }
    snprintf(t, tn, "%s", n.title[0] ? n.title : n.app);
    snprintf(b, bn, "%s", n.body);
}

// ---------------------------------------------------------------------------
//  10.  MEDIA + PHONE ADAPTER
// ---------------------------------------------------------------------------
//  Gadgetbridge-style line protocol (see section 6 header note):
//    phone -> watch, newline-delimited JSON over a NUS-like RX characteristic
//      {"t":"notif","pid":"a1","app":"Messages","sender":"Alex","title":"Hi","body":"..."}
//      {"t":"notifrm","pid":"a1"}
//      {"t":"media","playing":true,"title":"Song","artist":"Artist"}
//      {"t":"time","epoch":1700000000,"gmt":28800}
//    watch -> phone: {"t":"media","cmd":"toggle"|"next"|"prev"} if needed.
struct MediaState {
    uint8_t connected;
    uint8_t playing;
    char title[64];
    char artist[48];
};
static MediaState g_media = { 0, 0, "Midnight Drive", "DualCore FM" };

// Weather is phone-supplied, never invented by the watch: an unset state says
// so instead of showing a plausible number.
struct WeatherState {
    uint8_t valid;
    int8_t  temp, hi, lo;
    char    cond[16];
};
static WeatherState g_wx = { 0, 0, 0, 0, "" };

static int wx_clamp_temp(long v) {
    if (v < -40) v = -40;
    if (v > 60) v = 60;
    return (int)v;
}

enum MediaCmd : uint8_t { MEDIA_TOGGLE = 0, MEDIA_NEXT = 1, MEDIA_PREV = 2 };

#if DCS_USE_BLE
static volatile bool g_ble_up = false;
static NimBLECharacteristic *g_ble_tx = nullptr;
static bool gb_send_media_cmd(uint8_t cmd);
// BLE callbacks only enqueue complete lines.  All parsing, inbox mutation and
// SD writes happen later in phone_service(), on the loop task -- never in the
// NimBLE task.
#define GB_QUEUE 6
static char g_gb_q[GB_QUEUE][168];
static volatile uint8_t g_gb_head = 0, g_gb_tail = 0;
static void gb_enqueue_line(const char *line);
static void gb_handle_line(const char *line);
#endif

static bool phone_media_cmd(uint8_t cmd) {
    if (!g_media.connected) return false;
#if DCS_USE_BLE
    return gb_send_media_cmd(cmd);
#else
    // Demo source so every screen can be exercised with no phone attached.
    if (cmd == MEDIA_TOGGLE) {
        g_media.playing = !g_media.playing;
    } else {
        static int track = 0;
        track = (cmd == MEDIA_NEXT) ? track + 1 : (track > 0 ? track - 1 : 0);
        snprintf(g_media.title, sizeof(g_media.title), "Track %02d", track + 1);
        g_media.playing = 1;
    }
    return true;
#endif
}

static void phone_service() {
#if !DCS_USE_BLE
    // Demo link: comes up shortly after boot so every screen has live data.
    if (!g_media.connected && millis() > 1500) g_media.connected = 1;
    if (!g_wx.valid && millis() > 2500) {
        g_wx.valid = 1; g_wx.temp = 18; g_wx.hi = 20; g_wx.lo = 11;
        copyf(g_wx.cond, sizeof(g_wx.cond), "CLOUDY");
    }
#else
    g_media.connected = g_ble_up ? 1 : 0;
    while (g_gb_tail != g_gb_head) {
        char line[168];
        copyf(line, sizeof(line), g_gb_q[g_gb_tail]);
        g_gb_tail = (uint8_t)((g_gb_tail + 1) % GB_QUEUE);
        gb_handle_line(line);
    }
#endif
}

#if DCS_USE_BLE
// ---- optional NimBLE transport (UNVERIFIED on hardware) --------------------
static NimBLEServer *g_ble_srv = nullptr;
static char g_ble_line[192];
static size_t g_ble_len = 0;

static void gb_enqueue_line(const char *line) {
    if (!line || !line[0]) return;
    uint8_t next = (uint8_t)((g_gb_head + 1) % GB_QUEUE);
    if (next == g_gb_tail) return;          // bounded: drop on overflow
    copyf(g_gb_q[g_gb_head], sizeof(g_gb_q[0]), line);
    g_gb_head = next;
}

class DcsBleRx : public NimBLECharacteristicCallbacks {
    void onWrite(NimBLECharacteristic *c) override {
        std::string v = c->getValue();
        for (size_t i = 0; i < v.size(); ++i) {
            char ch = v[i];
            if (ch == '\n' || ch == '\r') {
                if (g_ble_len) { g_ble_line[g_ble_len] = 0; gb_enqueue_line(g_ble_line); g_ble_len = 0; }
            } else if (g_ble_len + 1 < sizeof(g_ble_line)) {
                g_ble_line[g_ble_len++] = ch;
            }
        }
    }
};
class DcsBleSrv : public NimBLEServerCallbacks {
    void onConnect(NimBLEServer *s) override { (void)s; g_ble_up = true; }
    void onDisconnect(NimBLEServer *s) override {
        (void)s; g_ble_up = false;
        NimBLEDevice::startAdvertising();
    }
};

static void gb_ble_start() {
    NimBLEDevice::init("DualCore SE");
    g_ble_srv = NimBLEDevice::createServer();
    g_ble_srv->setCallbacks(new DcsBleSrv());
    NimBLEService *svc = g_ble_srv->createService("6E400001-B5A3-F393-E0A9-E50E24DCCA9E");
    g_ble_tx = svc->createCharacteristic("6E400003-B5A3-F393-E0A9-E50E24DCCA9E",
                                         NIMBLE_PROPERTY::NOTIFY);
    NimBLECharacteristic *rx = svc->createCharacteristic(
        "6E400002-B5A3-F393-E0A9-E50E24DCCA9E",
        NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR);
    rx->setCallbacks(new DcsBleRx());
    svc->start();
    NimBLEAdvertising *adv = NimBLEDevice::getAdvertising();
    adv->addServiceUUID(svc->getUUID());
    adv->start();
}

static bool gb_send_media_cmd(uint8_t cmd) {
    if (!g_ble_tx || !g_ble_up) return false;
    char j[72];
    snprintf(j, sizeof(j), "{\"t\":\"media\",\"cmd\":\"%s\"}\n",
             cmd == MEDIA_NEXT ? "next" : cmd == MEDIA_PREV ? "prev" : "toggle");
    g_ble_tx->setValue((uint8_t *)j, strlen(j));
    g_ble_tx->notify();
    return true;
}

static void gb_handle_line(const char *line) {
    if (!line || line[0] != '{') return;
    char t[10];
    if (!jget_str(line, "t", t, sizeof(t), "")) return;
    if (!strcmp(t, "notif")) {
        char app[20] = "Phone", snd[24] = "", ttl[44] = "", bod[100] = "", pid[20] = "";
        jget_str(line, "app", app, sizeof(app), "Phone");
        jget_str(line, "sender", snd, sizeof(snd), "");
        jget_str(line, "title", ttl, sizeof(ttl), "");
        jget_str(line, "body", bod, sizeof(bod), "");
        jget_str(line, "pid", pid, sizeof(pid), "");
        notif_add(app, snd, ttl, bod, pid);
    } else if (!strcmp(t, "notifrm")) {
        char pid[20] = "";
        jget_str(line, "pid", pid, sizeof(pid), "");
        notif_remove_pid(pid);
    } else if (!strcmp(t, "media")) {
        char ttl[64] = "", art[48] = "";
        jget_str(line, "title", ttl, sizeof(ttl), "");
        jget_str(line, "artist", art, sizeof(art), "");
        if (ttl[0]) copyf(g_media.title, sizeof(g_media.title), ttl);
        if (art[0]) copyf(g_media.artist, sizeof(g_media.artist), art);
        g_media.playing = jget_bool(line, "playing", g_media.playing) ? 1 : 0;
    } else if (!strcmp(t, "weather")) {
        char cd[16] = "";
        jget_str(line, "cond", cd, sizeof(cd), "");
        g_wx.temp = (int8_t)wx_clamp_temp(jget_int(line, "temp", g_wx.temp));
        g_wx.hi   = (int8_t)wx_clamp_temp(jget_int(line, "hi", g_wx.temp));
        g_wx.lo   = (int8_t)wx_clamp_temp(jget_int(line, "lo", g_wx.temp));
        if (cd[0]) copyf(g_wx.cond, sizeof(g_wx.cond), cd);
        g_wx.valid = 1;
    } else if (!strcmp(t, "time")) {
        long e = jget_int(line, "epoch", 0);
        long g = jget_int(line, "gmt", g_tp_gmt);
        if (e > (long)TIME_VALID_MIN) {
            struct timeval tv = { (time_t)e, 0 };
            settimeofday(&tv, NULL);
            time_apply_tz((int32_t)g);
            time_publish("ble sync", false);
        }
    }
}
#endif
// ---------------------------------------------------------------------------
//  11.  CLOCK, WORLD CLOCK, TIMERS, EVENTS
// ---------------------------------------------------------------------------
static int64_t mono_ms() { return esp_timer_get_time() / 1000; }
static bool clock_valid() { time_t t; time(&t); return t >= (time_t)TIME_VALID_MIN; }

static void clock_fmt_time(char *out, size_t n, bool with_secs) {
    if (!out || !n) return;
    out[0] = 0;
    if (!clock_valid()) { snprintf(out, n, "--:--"); return; }
    time_t t; time(&t);
    struct tm lt; localtime_r(&t, &lt);
    int h = lt.tm_hour;
    if (!g_cfg.t24) {
        int hh = h % 12; if (!hh) hh = 12;
        if (with_secs) snprintf(out, n, "%d:%02d:%02d %s", hh, lt.tm_min, lt.tm_sec, h < 12 ? "AM" : "PM");
        else           snprintf(out, n, "%d:%02d %s", hh, lt.tm_min, h < 12 ? "AM" : "PM");
    } else {
        if (with_secs) snprintf(out, n, "%02d:%02d:%02d", h, lt.tm_min, lt.tm_sec);
        else           snprintf(out, n, "%02d:%02d", h, lt.tm_min);
    }
}

static void clock_fmt_date(char *out, size_t n) {
    static const char *D[] = { "SUN","MON","TUE","WED","THU","FRI","SAT" };
    static const char *M[] = { "JAN","FEB","MAR","APR","MAY","JUN","JUL","AUG","SEP","OCT","NOV","DEC" };
    if (!out || !n) return;
    if (!clock_valid()) { snprintf(out, n, "CLOCK NOT SET"); return; }
    time_t t; time(&t);
    struct tm lt; localtime_r(&t, &lt);
    snprintf(out, n, "%s %d %s %d", D[lt.tm_wday % 7], lt.tm_mday, M[lt.tm_mon % 12],
             lt.tm_year + 1900);
}

static void clock_fmt_day(char *out, size_t n) {
    static const char *D[] = { "SUN", "MON", "TUE", "WED", "THU", "FRI", "SAT" };
    if (!out || !n) return;
    if (!clock_valid()) { snprintf(out, n, "---"); return; }
    time_t t; time(&t);
    struct tm lt; localtime_r(&t, &lt);
    snprintf(out, n, "%s", D[lt.tm_wday % 7]);
}

static void clock_fmt_dur(int64_t secs, char *out, size_t n) {
    if (!out || !n) return;
    if (secs < 0) secs = 0;
    int64_t d = secs / 86400;
    int h = (int)((secs % 86400) / 3600);
    int m = (int)((secs % 3600) / 60);
    int s = (int)(secs % 60);
    if (d > 0)      snprintf(out, n, "%lldd %02dh %02dm", (long long)d, h, m);
    else if (h > 0) snprintf(out, n, "%02dh %02dm", h, m);
    else            snprintf(out, n, "%02d:%02d", m, s);
}

static void clock_fmt_at_offset(int32_t off_min, char *out, size_t n) {
    if (!out || !n) return;
    if (!clock_valid()) { snprintf(out, n, "--:--"); return; }
    time_t t; time(&t);
    t += (time_t)off_min * 60;
    struct tm g; gmtime_r(&t, &g);
    if (g_cfg.t24) snprintf(out, n, "%02d:%02d", g.tm_hour, g.tm_min);
    else {
        int hh = g.tm_hour % 12; if (!hh) hh = 12;
        snprintf(out, n, "%d:%02d %s", hh, g.tm_min, g.tm_hour < 12 ? "AM" : "PM");
    }
}

// ---- world clock (fixed offsets; documented limitation) -------------------
#define WCITY_MAX 6
struct WCity { char name[16]; int16_t off_min; };
static WCity g_city[WCITY_MAX];
static int g_city_n = 0;
static const struct { const char *name; int16_t off; } kCityPick[] = {
    { "LONDON", 0 }, { "PARIS", 60 }, { "NEW YORK", -300 }, { "TOKYO", 540 },
    { "SYDNEY", 600 }, { "DUBAI", 240 }, { "LOS ANGELES", -480 }, { "UTC", 0 },
};
static const int kCityPickN = sizeof(kCityPick) / sizeof(kCityPick[0]);

static void city_save();
static void city_load() {
    g_city_n = 0;
    static char buf[600];
    if (!fs_read_text("/WATCH/NoirOS/data/world_clocks.jsonl", buf, sizeof(buf))) {
        copyf(g_city[0].name, sizeof(g_city[0].name), "LONDON");   g_city[0].off_min = 0;
        copyf(g_city[1].name, sizeof(g_city[1].name), "NEW YORK"); g_city[1].off_min = -300;
        g_city_n = 2;
        return;
    }
    char *save = NULL;
    for (char *line = strtok_r(buf, "\n", &save); line && g_city_n < WCITY_MAX;
         line = strtok_r(NULL, "\n", &save)) {
        char *bar = strchr(line, '|');
        if (!bar) continue;
        *bar = 0;
        copyf(g_city[g_city_n].name, sizeof(g_city[0].name), line);
        g_city[g_city_n].off_min = (int16_t)atoi(bar + 1);
        g_city_n++;
    }
    if (g_city_n == 0) {   // corrupt/empty file -> safe defaults, not a blank card
        copyf(g_city[0].name, sizeof(g_city[0].name), "LONDON");   g_city[0].off_min = 0;
        copyf(g_city[1].name, sizeof(g_city[1].name), "NEW YORK"); g_city[1].off_min = -300;
        g_city_n = 2;
    }
}
static void city_save() {
    char buf[420];
    size_t o = 0;
    for (int i = 0; i < g_city_n; i++) {
        int w = snprintf(buf + o, sizeof(buf) - o, "%s|%d\n", g_city[i].name, (int)g_city[i].off_min);
        if (w < 0 || o + (size_t)w >= sizeof(buf)) break;
        o += (size_t)w;
    }
    fs_write_atomic("/WATCH/NoirOS/data/world_clocks.jsonl", buf, o);
}
static void city_add(int pick) {
    if (g_city_n >= WCITY_MAX || pick < 0 || pick >= kCityPickN) return;
    for (int i = 0; i < g_city_n; i++)
        if (!strcmp(g_city[i].name, kCityPick[pick].name)) return;
    copyf(g_city[g_city_n].name, sizeof(g_city[0].name), kCityPick[pick].name);
    g_city[g_city_n].off_min = kCityPick[pick].off;
    g_city_n++;
    city_save();
}
static void city_remove_last() {
    if (g_city_n <= 1) return;
    g_city_n--;
    city_save();
}

// ---- timers ---------------------------------------------------------------
//  Countdown is wall-clock anchored, so it survives a reset.  The stopwatch
//  is monotonic (esp_timer) and deliberately resets with the chip: a
//  wall-clock jump must never be able to corrupt elapsed time.
static bool     g_cd_run = false;
static bool cd_running() { return g_cd_run; }
static uint32_t g_cd_base_ms = 25UL * 60000UL;
static int64_t  g_cd_t0 = 0;
static uint32_t g_cd_sessions = 0;
static bool     g_cd_finished = false;
static bool     g_sw_run = false;
static uint32_t g_sw_base_ms = 0;
static int64_t  g_sw_t0 = 0;

static uint32_t cd_remain_ms() {
    uint32_t used = g_cd_run ? (uint32_t)(mono_ms() - g_cd_t0) : 0;
    return used >= g_cd_base_ms ? 0 : g_cd_base_ms - used;
}
static void cd_start(uint32_t ms) {
    if (ms) g_cd_base_ms = ms;
    g_cd_finished = false;
    g_cd_t0 = mono_ms();
    g_cd_run = true;
}
static void cd_pause() {
    if (!g_cd_run) return;
    g_cd_base_ms = cd_remain_ms();
    g_cd_run = false;
}
static void cd_reset() {
    g_cd_run = false;
    g_cd_finished = false;
    g_cd_base_ms = (uint32_t)g_noir.focus_min * 60000UL;
}
static uint32_t sw_ms() {
    return g_sw_run ? g_sw_base_ms + (uint32_t)(mono_ms() - g_sw_t0) : g_sw_base_ms;
}
static void sw_start() { if (!g_sw_run) { g_sw_t0 = mono_ms(); g_sw_run = true; } }
static void sw_stop()  { if (g_sw_run) { g_sw_base_ms = sw_ms(); g_sw_run = false; } }
static void sw_reset() { g_sw_run = false; g_sw_base_ms = 0; }

static void timer_load() {
    char buf[420];
    if (!fs_read_text("/WATCH/SHARED/timers.json", buf, sizeof(buf))) return;
    g_cd_sessions = (uint32_t)jget_int(buf, "sessions", 0);
    g_cd_base_ms  = (uint32_t)jget_int(buf, "cd_base_ms", g_cd_base_ms);
    g_sw_base_ms  = (uint32_t)jget_int(buf, "sw_base_ms", 0);
    bool cd_run   = jget_bool(buf, "cd_run", false);
    long end_epoch = jget_int(buf, "cd_end", 0);
    if (cd_run) {
        time_t now = time(NULL);
        if (clock_valid() && end_epoch > (long)now) {
            g_cd_base_ms = (uint32_t)(end_epoch - (long)now) * 1000UL;
            g_cd_run = true;
            g_cd_t0 = mono_ms();
        } else {
            g_cd_run = false;
            g_cd_base_ms = (uint32_t)g_noir.focus_min * 60000UL;
        }
    }
    if (g_cd_base_ms == 0 || g_cd_base_ms > 4UL * 3600UL * 1000UL)
        g_cd_base_ms = (uint32_t)g_noir.focus_min * 60000UL;
}
static void timer_save() {
    char buf[300];
    long end_epoch = 0;
    if (g_cd_run && clock_valid()) end_epoch = (long)time(NULL) + (long)(cd_remain_ms() / 1000UL);
    snprintf(buf, sizeof(buf),
        "{\n  \"sessions\": %lu,\n  \"cd_base_ms\": %lu,\n  \"cd_run\": %s,\n"
        "  \"cd_end\": %ld,\n  \"sw_base_ms\": %lu\n}\n",
        (unsigned long)g_cd_sessions, (unsigned long)g_cd_base_ms,
        g_cd_run ? "true" : "false", end_epoch, (unsigned long)sw_ms());
    fs_write_atomic("/WATCH/SHARED/timers.json", buf, strlen(buf));
}

static void timer_service() {
    if (g_cd_run && cd_remain_ms() == 0) {
        g_cd_run = false;
        g_cd_finished = true;
        g_cd_sessions++;
        g_cd_base_ms = (uint32_t)g_noir.focus_min * 60000UL;
        char line[96];
        snprintf(line, sizeof(line), "%lu|%u|complete\n", (unsigned long)time(NULL),
                 (unsigned)g_noir.focus_min);
        fs_append("/WATCH/NoirOS/data/focus_sessions.jsonl", line);
        dcs_log(DCS_PATH_NOIR, "focus session complete (%u total)", (unsigned)g_cd_sessions);
        timer_save();
    }
}

// ---- CrownOS events -------------------------------------------------------
#define EVENT_MAX 8
struct DcsEvent { char title[24]; int64_t epoch; uint8_t alerted; };
static DcsEvent g_ev[EVENT_MAX];
static int g_ev_n = 0;

static void event_save();
static void event_load() {
    g_ev_n = 0;
    static char buf[600];
    if (!fs_read_text("/WATCH/CrownOS/data/events.jsonl", buf, sizeof(buf))) return;
    char *save = NULL;
    for (char *line = strtok_r(buf, "\n", &save); line && g_ev_n < EVENT_MAX;
         line = strtok_r(NULL, "\n", &save)) {
        char *bar = strchr(line, '|');
        if (!bar) continue;
        *bar = 0;
        long long e = atoll(bar + 1);
        if (e <= 0) continue;
        copyf(g_ev[g_ev_n].title, sizeof(g_ev[0].title), line);
        g_ev[g_ev_n].epoch = (int64_t)e;
        g_ev[g_ev_n].alerted = 0;
        g_ev_n++;
    }
}
static int event_count() { return g_ev_n; }
static void event_save() {
    char buf[520];
    size_t o = 0;
    for (int i = 0; i < g_ev_n; i++) {
        int w = snprintf(buf + o, sizeof(buf) - o, "%s|%lld\n", g_ev[i].title,
                         (long long)g_ev[i].epoch);
        if (w < 0 || o + (size_t)w >= sizeof(buf)) break;
        o += (size_t)w;
    }
    fs_write_atomic("/WATCH/CrownOS/data/events.jsonl", buf, o);
}
static void event_add(const char *title, int64_t delta_secs) {
    if (g_ev_n >= EVENT_MAX || !clock_valid()) return;
    copyf(g_ev[g_ev_n].title, sizeof(g_ev[0].title), title);
    g_ev[g_ev_n].epoch = (int64_t)time(NULL) + delta_secs;
    g_ev[g_ev_n].alerted = 0;
    g_ev_n++;
    event_save();
    dcs_log(DCS_PATH_CROWN, "event added (%d)", g_ev_n);
}
static bool event_next(char *title, size_t cap, int64_t *secs_left) {
    int best = -1;
    time_t now = time(NULL);
    for (int i = 0; i < g_ev_n; i++) {
        if (g_ev[i].epoch <= (int64_t)now) continue;
        if (best < 0 || g_ev[i].epoch < g_ev[best].epoch) best = i;
    }
    if (best < 0) { if (title && cap) title[0] = 0; return false; }
    if (title && cap) copyf(title, cap, g_ev[best].title);
    if (secs_left) *secs_left = g_ev[best].epoch - (int64_t)now;
    return true;
}

// The n-th upcoming event (0 = next), without allocating a sorted copy.
static bool event_nth(int k, char *title, size_t cap, int64_t *secs_left) {
    if (k < 0) return false;
    time_t now = time(NULL);
    int64_t floor_epoch = (int64_t)now;
    for (int pass = 0; pass <= k; pass++) {
        int best = -1;
        for (int i = 0; i < g_ev_n; i++) {
            if (g_ev[i].epoch <= floor_epoch) continue;
            if (best < 0 || g_ev[i].epoch < g_ev[best].epoch) best = i;
        }
        if (best < 0) { if (pass == 0 && title && cap) title[0] = 0; return false; }
        floor_epoch = g_ev[best].epoch;
        if (pass == k) {
            if (title && cap) copyf(title, cap, g_ev[best].title);
            if (secs_left) *secs_left = g_ev[best].epoch - (int64_t)now;
            return true;
        }
    }
    return false;
}

static void event_remove(int idx) {
    if (idx < 0 || idx >= g_ev_n) return;
    memmove(&g_ev[idx], &g_ev[idx + 1], (size_t)(g_ev_n - idx - 1) * sizeof(DcsEvent));
    g_ev_n--;
    event_save();
}

// 15-minute pre-alert, opt-in from Watch Studio.  It uses the shared inbox so
// the alert is delivered under whatever privacy policy is active, not by a
// CrownOS-only popup that could bypass it.
static void crown_event_service() {
    if (!g_crown.alerts || !clock_valid()) return;
    static uint32_t last = 0;
    uint32_t now = millis();
    if (now - last < 30000U) return;
    last = now;
    time_t t; time(&t);
    for (int i = 0; i < g_ev_n; i++) {
        if (g_ev[i].alerted) continue;
        int64_t left = g_ev[i].epoch - (int64_t)t;
        if (left > 0 && left <= 900) {
            g_ev[i].alerted = 1;
            char body[40];
            snprintf(body, sizeof(body), "Starts in %d minutes", (int)((left + 59) / 60));
            notif_add("CrownOS", "", g_ev[i].title, body, NULL);
            dcs_log(DCS_PATH_CROWN, "event alert raised");
        }
    }
}

// ---- alarms (shared: they ring over every experience and the chooser) -----
//  Three daily alarms, no day mask on purpose: a wake alarm that silently
//  skips a weekday is worse than a simple one.  Snooze is +5 min and lives in
//  RAM only -- a reset drops the snooze, never the alarm.
#define ALARM_MAX 3
struct AlarmCfg { uint8_t hh, mm, on, days; };   // days: bit0 Sun .. bit6 Sat
static AlarmCfg g_alarm[ALARM_MAX] = { { 7, 0, 0, 0x7F }, { 12, 30, 0, 0x7F },
                                       { 19, 0, 0, 0x7F } };
static uint32_t g_alarm_stamp[ALARM_MAX] = { 0, 0, 0 };  // minute-of-day last fired
static int      g_ring = -1;            // -1 idle, 0..2 alarm, -2 snooze
static time_t   g_snooze_at = 0;
static lv_obj_t *g_ring_overlay = nullptr;

static void alarm_save() {
    char buf[320];
    snprintf(buf, sizeof(buf),
        "{\n  \"schema\": 2,\n"
        "  \"a0h\": %u,  \"a0m\": %u,  \"a0on\": %u,  \"a0d\": %u,\n"
        "  \"a1h\": %u,  \"a1m\": %u,  \"a1on\": %u,  \"a1d\": %u,\n"
        "  \"a2h\": %u,  \"a2m\": %u,  \"a2on\": %u,  \"a2d\": %u\n}\n",
        (unsigned)g_alarm[0].hh, (unsigned)g_alarm[0].mm, (unsigned)g_alarm[0].on,
        (unsigned)g_alarm[0].days,
        (unsigned)g_alarm[1].hh, (unsigned)g_alarm[1].mm, (unsigned)g_alarm[1].on,
        (unsigned)g_alarm[1].days,
        (unsigned)g_alarm[2].hh, (unsigned)g_alarm[2].mm, (unsigned)g_alarm[2].on,
        (unsigned)g_alarm[2].days);
    fs_write_atomic("/WATCH/SHARED/alarms.json", buf, strlen(buf));
}

static void alarm_load() {
    char buf[420];
    if (!fs_read_text("/WATCH/SHARED/alarms.json", buf, sizeof(buf))) return;
    static const char *hk[3] = { "a0h", "a1h", "a2h" };
    static const char *mk[3] = { "a0m", "a1m", "a2m" };
    static const char *ok[3] = { "a0on", "a1on", "a2on" };
    static const char *dk[3] = { "a0d", "a1d", "a2d" };
    for (int i = 0; i < ALARM_MAX; i++) {
        long h = jget_int(buf, hk[i], g_alarm[i].hh);
        long m = jget_int(buf, mk[i], g_alarm[i].mm);
        if (h < 0 || h > 23) h = 7;
        if (m < 0 || m > 59) m = 0;
        g_alarm[i].hh = (uint8_t)h;
        g_alarm[i].mm = (uint8_t)m;
        g_alarm[i].on = jget_bool(buf, ok[i], false) ? 1 : 0;
        g_alarm[i].days = (uint8_t)(jget_int(buf, dk[i], 0x7F) & 0x7F);
        if (!g_alarm[i].days) g_alarm[i].days = 0x7F;   // never an alarm that
                                                        // cannot ring
    }
}

// Earliest enabled alarm, relative to now when the clock is valid.
static bool alarm_next_str(char *out, size_t n) {
    if (!out || !n) return false;
    out[0] = 0;
    int best = -1, best_delta = 24 * 60 + 1;
    int now_min = -1;
    if (clock_valid()) {
        time_t t; time(&t);
        struct tm lt; localtime_r(&t, &lt);
        now_min = lt.tm_hour * 60 + lt.tm_min;
    }
    for (int i = 0; i < ALARM_MAX; i++) {
        if (!g_alarm[i].on) continue;
        int m = g_alarm[i].hh * 60 + g_alarm[i].mm;
        int d = (now_min < 0) ? m : ((m - now_min + 1440) % 1440);
        if (d == 0) d = 1440;
        if (d < best_delta) { best_delta = d; best = i; }
    }
    if (best < 0) return false;
    snprintf(out, n, "%02u:%02u", (unsigned)g_alarm[best].hh, (unsigned)g_alarm[best].mm);
    return true;
}

static void alarm_ring(int idx);   // UI lives in section 14.5

static void alarm_check() {
    if (g_ring != -1) return;
    if (g_snooze_at) {
        time_t t = time(NULL);
        if (clock_valid() && t >= g_snooze_at) { g_snooze_at = 0; alarm_ring(-2); }
        return;
    }
    if (!clock_valid()) return;
    time_t t; time(&t);
    struct tm lt; localtime_r(&t, &lt);
    uint32_t stamp = (uint32_t)(lt.tm_yday * 1440 + lt.tm_hour * 60 + lt.tm_min);
    for (int i = 0; i < ALARM_MAX; i++) {
        if (!g_alarm[i].on) continue;
        if (!(g_alarm[i].days & (uint8_t)(1 << lt.tm_wday))) continue;
        if (g_alarm[i].hh != lt.tm_hour || g_alarm[i].mm != lt.tm_min) continue;
        if (g_alarm_stamp[i] == stamp) continue;
        g_alarm_stamp[i] = stamp;
        alarm_ring(i);
        return;
    }
}

// ---------------------------------------------------------------------------
//  12.  POWER MANAGER
// ---------------------------------------------------------------------------
//  One owner for brightness/profile decisions.  Experiences *request*; the
//  manager resolves against safety, active timers and the selected profile.
static uint8_t dcs_brightness_base() {
    int b = g_cfg.brightness;
    if (g_cur_os == 2) {
        b = g_night.brightness;
        if (g_night.shadow) b = b * 45 / 100;
    }
    if (g_cfg.power == PWR_ECO)      b = b * 80 / 100;
    if (g_cfg.power == PWR_SURVIVAL) b = b * 55 / 100;
    if (b < DCS_MIN_BRIGHTNESS) b = DCS_MIN_BRIGHTNESS;
    if (b > 255) b = 255;
    return (uint8_t)b;
}

static void power_apply() {
    if (g_screen_off) return;
    uint8_t b = dcs_brightness_base();
    if (b != g_cur_brightness) dcs_fade_to(b, 160);
}

static void power_set(uint8_t p) {
    if (p > PWR_SURVIVAL) return;
    g_cfg.power = p;
    sharedcfg_save();
    dcs_log(DCS_PATH_SHARED, "power profile -> %s", kPwrName[p]);
    power_apply();
}

static void batt_fmt(char *out, size_t n) {
    if (g_batt_pct < 0) snprintf(out, n, "-- %");
    else                snprintf(out, n, "%d%%", g_batt_pct);
}

static void power_auto_check() {
    if (!g_cfg.auto_power || g_batt_pct < 0 || g_batt_pct > 100) return;
    static bool eco_latch = false, surv_latch = false;
    if (g_batt_pct <= 15 && !surv_latch) {
        surv_latch = true;
        if (g_cfg.power != PWR_SURVIVAL) power_set(PWR_SURVIVAL);
    } else if (g_batt_pct <= 30 && !eco_latch && g_cfg.power < PWR_ECO) {
        eco_latch = true;
        power_set(PWR_ECO);
    }
    if (g_batt_pct >= 20) surv_latch = false;
    if (g_batt_pct >= 35) eco_latch = false;
}

// ---- runtime estimate ------------------------------------------------------
//  Derived from the 5 s battery poll: needs a 10-minute window and at least
//  1 % of discharge, and resets whenever charging.  Shown as approximate
//  because a single window is not a calibrated fuel gauge.
static float    g_est_h = -1.0f;
static int      g_est_pct = -1;
static uint32_t g_est_ms = 0;

static void batt_estimate_update() {
    static uint32_t last = 0;
    uint32_t now = millis();
    if (now - last < 60000U) return;
    last = now;
    if (g_batt_pct < 0 || g_batt_ch) { g_est_h = -1.0f; g_est_pct = -1; return; }
    if (g_est_pct < 0) { g_est_pct = g_batt_pct; g_est_ms = now; return; }
    float mins = (float)(now - g_est_ms) / 60000.0f;
    int drop = g_est_pct - g_batt_pct;
    if (mins >= 10.0f && drop >= 1) {
        g_est_h = ((float)g_batt_pct * mins) / ((float)drop * 60.0f);
        if (g_est_h > 99.0f) g_est_h = 99.0f;
        g_est_pct = g_batt_pct;
        g_est_ms = now;
    }
}

static void est_fmt(char *out, size_t n) {
    if (!out || !n) return;
    if (g_est_h <= 0) { snprintf(out, n, "--"); return; }
    if (g_est_h >= 10) snprintf(out, n, "~%.0fh", (double)g_est_h);
    else               snprintf(out, n, "~%.1fh", (double)g_est_h);
}

// ---- battery history (Power Lab) ------------------------------------------
//  One sample every five minutes, bounded to 96 (eight hours).  RAM only:
//  it exists to compare runs side by side, not to be a permanent log, and
//  rewriting an SD card every five minutes would cost more than it measures.
#define BATT_HIST_MAX 96
static uint8_t g_batt_hist[BATT_HIST_MAX];
static int     g_batt_hist_n   = 0;
static uint8_t g_batt_hist_min = 0, g_batt_hist_max = 0;

static void batt_hist_sample() {
    if (g_batt_pct < 0 || g_batt_pct > 100) return;
    static uint32_t last = 0;
    uint32_t now = millis();
    if (g_batt_hist_n && now - last < 300000UL) return;
    last = now;
    if (g_batt_hist_n >= BATT_HIST_MAX) {
        memmove(g_batt_hist, g_batt_hist + 1, BATT_HIST_MAX - 1);
        g_batt_hist[BATT_HIST_MAX - 1] = (uint8_t)g_batt_pct;
    } else {
        g_batt_hist[g_batt_hist_n++] = (uint8_t)g_batt_pct;
    }
    g_batt_hist_min = g_batt_hist_max = g_batt_hist[0];
    for (int i = 1; i < g_batt_hist_n; i++) {
        if (g_batt_hist[i] < g_batt_hist_min) g_batt_hist_min = g_batt_hist[i];
        if (g_batt_hist[i] > g_batt_hist_max) g_batt_hist_max = g_batt_hist[i];
    }
}

// ---------------------------------------------------------------------------
//  13.  SAVE-ALL + SHARED DATA INIT
// ---------------------------------------------------------------------------
static void dcs_save_all() {
    sharedcfg_save();
    noir_save();
    crown_save();
    night_save();
    notif_save();
    timer_save();
    city_save();
    event_save();
    alarm_save();
}

static void dcs_data_init() {
    if (!g_sd_ok) return;
    fs_ensure_dir(DCS_PATH_SHARED);
    fs_ensure_dir(DCS_PATH_NOIR);
    fs_ensure_dir(DCS_PATH_CROWN);
    fs_ensure_dir(DCS_PATH_NIGHT);
    fs_ensure_dir(DCS_PATH_NOIR "/config");
    fs_ensure_dir(DCS_PATH_NOIR "/assets");
    fs_ensure_dir(DCS_PATH_NOIR "/data");
    fs_ensure_dir(DCS_PATH_NOIR "/cache");
    fs_ensure_dir(DCS_PATH_NOIR "/logs");
    fs_ensure_dir(DCS_PATH_CROWN "/config");
    fs_ensure_dir(DCS_PATH_CROWN "/assets");
    fs_ensure_dir(DCS_PATH_CROWN "/data");
    fs_ensure_dir(DCS_PATH_CROWN "/cache");
    fs_ensure_dir(DCS_PATH_CROWN "/logs");
    fs_ensure_dir(DCS_PATH_NIGHT "/config");
    fs_ensure_dir(DCS_PATH_NIGHT "/assets");
    fs_ensure_dir(DCS_PATH_NIGHT "/data");
    fs_ensure_dir(DCS_PATH_NIGHT "/cache");
    fs_ensure_dir(DCS_PATH_NIGHT "/logs");
    for (int i = 0; i < 3; i++) manifest_ensure(i);
}

// Configuration load order matters: the shared file first (it carries the
// privacy default), then each experience's own overrides.
static void dcs_cfg_load() {
    sharedcfg_load();
    noir_load();
    crown_load();
    night_load();
    notif_load();
    timer_load();
    city_load();
    event_load();
    alarm_load();
}
// ---------------------------------------------------------------------------
//  14.  UI KIT  (shared by all three experiences)
// ---------------------------------------------------------------------------
static uint32_t g_bg = 0x04050A, g_panel = 0x0C0F16, g_fg = 0xF0F3F7,
                g_dim = 0x6B7280, g_accent = 0xD9B25C, g_warn = 0xD9B25C,
                g_danger = 0xD9433F;

static lv_obj_t *g_batt_lbl = nullptr, *g_conn_lbl = nullptr;
static lv_obj_t *g_clock_lbl = nullptr, *g_date_lbl = nullptr;
static lv_obj_t *g_sov_day = nullptr, *g_sov_date = nullptr;
static lv_obj_t *g_timer_lbl = nullptr, *g_sw_lbl = nullptr;
static lv_obj_t *g_hand[3] = { nullptr, nullptr, nullptr };
static lv_point_t g_hand_pts[3][2];
static int  g_dial_cx = 0, g_dial_cy = 0, g_dial_r = 0;
static uint8_t g_hand_sec = 255;
static lv_obj_t *g_city_lbl[WCITY_MAX];

static const uint32_t kThemeColors[7] = {
    0xD9B25C, 0xD9B25C, 0x7FB3E8, 0x69C9A8, 0xA98BE0, 0xC8324A, 0xD8DEE6
};
static const char *kThemeName[7] = {
    "OS DEFAULT", "CHAMPAGNE", "ICE BLUE", "MINT", "VIOLET", "CRIMSON", "PLATINUM"
};

static void ui_set_theme(int os) {
    switch (os) {
        case 0:  // NoirOS: obsidian / charcoal / silver, muted blue accent
            g_bg = 0x050608; g_panel = 0x0E1116; g_fg = 0xE8EDF2;
            g_dim = 0x6C7684; g_accent = 0x6E9BD1; g_warn = 0xD7A24A;
            g_danger = 0xD9433F;
            break;
        case 1:  // CrownOS: black lacquer / champagne gold / ivory
            g_bg = 0x0B0A07; g_panel = 0x15130E; g_fg = 0xF2EAD8;
            g_dim = 0x8A7A5E; g_accent = 0xC9A44C; g_warn = 0xC9A44C;
            g_danger = 0xB4453F;
            break;
        case 2:  // NightOS: near-black / charcoal / crimson
            if (g_night.palette) {   // crimson palette: a tint, not an overlay
                g_bg = 0x0A0305; g_panel = 0x1B070C; g_fg = 0xEBD9DC;
                g_dim = 0x8C5A63; g_accent = 0xC8324A; g_warn = 0xD84A5C;
                g_danger = 0xE0443F;
            } else {
                g_bg = 0x050407; g_panel = 0x120A10; g_fg = 0xE6DCE2;
                g_dim = 0x77636E; g_accent = 0xB32A3C; g_warn = 0xC23A4A;
                g_danger = 0xD9433F;
            }
            break;
        default: // chooser
            g_bg = 0x04050A; g_panel = 0x0C0F16; g_fg = 0xF0F3F7;
            g_dim = 0x6B7280; g_accent = 0xD9B25C; g_warn = 0xD9B25C;
            g_danger = 0xD9433F;
            break;
    }
    // Shared accent override.  Each experience still owns its own base palette
    // (and NightOS's crimson toggle), but a user who wants one colour across
    // all three sets it once instead of three times.
    if (g_cfg.theme > 0 && g_cfg.theme < 7) {
        g_accent = kThemeColors[g_cfg.theme];
        g_warn   = kThemeColors[g_cfg.theme];
    }
}

static void ui_clear_refs() {
    g_batt_lbl = g_conn_lbl = g_clock_lbl = g_date_lbl = nullptr;
    g_sov_day = g_sov_date = nullptr;
    g_timer_lbl = g_sw_lbl = nullptr;
    for (int i = 0; i < 3; i++) g_hand[i] = nullptr;
    for (int i = 0; i < WCITY_MAX; i++) g_city_lbl[i] = nullptr;
    g_dial_r = 0;
    g_hand_sec = 255;
}

static lv_obj_t *ui_screen(uint32_t bg) {
    lv_obj_t *s = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s, lv_color_hex(bg), 0);
    lv_obj_set_style_bg_opa(s, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(s, 0, 0);
    lv_obj_set_style_border_width(s, 0, 0);
    lv_obj_clear_flag(s, LV_OBJ_FLAG_SCROLLABLE);
    return s;
}

static lv_obj_t *ui_box(lv_obj_t *p, int x, int y, int w, int h, uint32_t bg, int radius) {
    lv_obj_t *o = lv_obj_create(p);
    lv_obj_set_pos(o, x, y);
    lv_obj_set_size(o, w, h);
    lv_obj_set_style_bg_color(o, lv_color_hex(bg), 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(o, radius, 0);
    lv_obj_set_style_border_width(o, 0, 0);
    lv_obj_set_style_pad_all(o, 0, 0);
    lv_obj_clear_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    return o;
}

static lv_obj_t *ui_frame(lv_obj_t *p, int x, int y, int w, int h, uint32_t bg,
                          int radius, uint32_t border, int bw) {
    lv_obj_t *o = ui_box(p, x, y, w, h, bg, radius);
    lv_obj_set_style_border_width(o, bw, 0);
    lv_obj_set_style_border_color(o, lv_color_hex(border), 0);
    return o;
}

static lv_obj_t *ui_text(lv_obj_t *p, const char *txt, uint32_t col,
                         const lv_font_t *font, int x, int y, int w) {
    lv_obj_t *l = lv_label_create(p);
    lv_label_set_text(l, txt);
    lv_obj_set_style_text_color(l, lv_color_hex(col), 0);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_pos(l, x, y);
    if (w > 0) {
        lv_obj_set_width(l, w);
        lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
    }
    return l;
}

static lv_obj_t *ui_btn(lv_obj_t *p, int x, int y, int w, int h, const char *txt,
                        uint32_t bg, uint32_t fg, int radius,
                        lv_event_cb_t cb, void *ud) {
    lv_obj_t *b = lv_btn_create(p);
    lv_obj_set_pos(b, x, y);
    lv_obj_set_size(b, w, h);
    lv_obj_set_style_bg_color(b, lv_color_hex(bg), 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(b, radius, 0);
    lv_obj_set_style_shadow_width(b, 0, 0);
    lv_obj_set_style_border_width(b, 0, 0);
    lv_obj_t *l = lv_label_create(b);
    lv_label_set_text(l, txt);
    lv_obj_set_style_text_color(l, lv_color_hex(fg), 0);
    lv_obj_center(l);
    if (cb) lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, ud);
    return b;
}

// One vertical list row: left-aligned, full width, tappable.
static lv_obj_t *ui_row(lv_obj_t *p, int x, int y, int w, int h, uint32_t bg,
                        uint32_t border, lv_event_cb_t cb, void *ud) {
    lv_obj_t *b = lv_btn_create(p);
    lv_obj_set_pos(b, x, y);
    lv_obj_set_size(b, w, h);
    lv_obj_set_style_bg_color(b, lv_color_hex(bg), 0);
    lv_obj_set_style_radius(b, 10, 0);
    lv_obj_set_style_shadow_width(b, 0, 0);
    lv_obj_set_style_border_width(b, 1, 0);
    lv_obj_set_style_border_color(b, lv_color_hex(border), 0);
    lv_obj_set_style_pad_all(b, 0, 0);
    if (cb) lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, ud);
    return b;
}

static void ui_pad(lv_obj_t *p, int y) {
    ui_box(p, 0, y, 8, 8, g_bg, 0);
}

// ---- overlays that sit above every experience (section 14.5) --------------
static void quick_sheet();
static void quick_sheet_cb(lv_event_t *e);
static void alarm_editor();
static void dcs_sleep_now(const char *why);
static void system_sheet();
static void wifi_sheet();
static void dev_sheet();
static void reset_sheet();
static void net_service();
static void photos_scan();
static bool wall_load(int idx);
static void noir_wallpaper(lv_obj_t *body);
static const char *reset_reason_str();

static lv_obj_t *ui_begin(int os, const char *title, const char *sub,
                          const char *const *tabs, int ntab, int active,
                          lv_event_cb_t tab_cb, lv_event_cb_t exit_cb) {
    ui_set_theme(os);
    ui_clear_refs();
    lv_obj_t *root = ui_screen(g_bg);

    lv_obj_t *hdr = ui_box(root, 0, 0, 410, 58, g_panel, 0);
    lv_obj_add_event_cb(hdr, quick_sheet_cb, LV_EVENT_LONG_PRESSED, NULL);
    ui_text(root, title, g_fg, &lv_font_montserrat_20, 16, 9, 0);
    ui_text(root, sub, g_dim, &lv_font_montserrat_12, 16, 35, 0);
    ui_text(root, "QUICK  (HOLD)", g_dim, &lv_font_montserrat_10, 138, 39, 0);
    ui_box(root, 0, 57, 410, 1, g_accent, 0);

    g_conn_lbl = ui_text(root, "OFF", g_dim, &lv_font_montserrat_12, 214, 22, 48);
    lv_obj_set_style_text_align(g_conn_lbl, LV_TEXT_ALIGN_RIGHT, 0);
    g_batt_lbl = ui_text(root, "-- %", g_fg, &lv_font_montserrat_16, 266, 18, 68);
    lv_obj_set_style_text_align(g_batt_lbl, LV_TEXT_ALIGN_RIGHT, 0);
    ui_btn(root, 342, 14, 54, 30, "EXIT", g_panel, g_dim, 15, exit_cb, NULL);

    ui_box(root, 0, 436, 410, 66, g_panel, 0);
    for (int i = 0; i < ntab; i++) {
        int w = 410 / ntab;
        lv_obj_t *b = ui_btn(root, i * w, 436, w, 66, tabs[i], g_panel,
                             (i == active) ? g_accent : g_dim, 0, tab_cb,
                             (void *)(intptr_t)i);
        lv_obj_set_style_text_font(lv_obj_get_child(b, 0), &lv_font_montserrat_14, 0);
        if (i == active) ui_box(root, i * w + w / 2 - 20, 436, 40, 3, g_accent, 0);
    }

    lv_obj_t *body = lv_obj_create(root);
    lv_obj_set_pos(body, 0, 58);
    lv_obj_set_size(body, 410, 378);
    lv_obj_set_style_bg_color(body, lv_color_hex(g_bg), 0);
    lv_obj_set_style_bg_opa(body, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(body, 0, 0);
    lv_obj_set_style_pad_all(body, 0, 0);
    lv_obj_clear_flag(body, LV_OBJ_FLAG_SCROLLABLE);
    return body;
}

static void ui_body_scroll(lv_obj_t *body) {
    lv_obj_add_flag(body, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(body, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(body, LV_SCROLLBAR_MODE_OFF);
}

// ---- analogue dial (shared by NoirOS's analogue layout and CrownOS) -------
static void ui_dial(lv_obj_t *p, int cx, int cy, int r, uint32_t accent,
                    uint32_t fg, bool numerals, bool secs_hand) {
    lv_obj_t *ring = lv_obj_create(p);
    lv_obj_set_pos(ring, cx - r, cy - r);
    lv_obj_set_size(ring, r * 2, r * 2);
    lv_obj_set_style_radius(ring, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(ring, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(ring, 2, 0);
    lv_obj_set_style_border_color(ring, lv_color_hex(accent), 0);
    lv_obj_clear_flag(ring, LV_OBJ_FLAG_SCROLLABLE);

    for (int i = 0; i < 12; i++) {
        float a = (float)i * 30.0f * 3.14159265f / 180.0f;
        int tx = cx + (int)(sinf(a) * (float)(r - 12));
        int ty = cy - (int)(cosf(a) * (float)(r - 12));
        lv_obj_t *tick = lv_obj_create(p);
        lv_obj_set_size(tick, i % 3 == 0 ? 4 : 2, i % 3 == 0 ? 12 : 7);
        lv_obj_set_pos(tick, tx - 2, ty - 6);
        lv_obj_set_style_bg_color(tick, lv_color_hex(i % 3 == 0 ? fg : accent), 0);
        lv_obj_set_style_bg_opa(tick, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(tick, 0, 0);
        lv_obj_set_style_radius(tick, 2, 0);
    }
    if (numerals) {
        static const char *num[4] = { "12", "3", "6", "9" };
        const int px[4] = { 0, 1, 0, -1 };
        const int py[4] = { -1, 0, 1, 0 };
        for (int i = 0; i < 4; i++) {
            lv_obj_t *l = ui_text(p, num[i], fg, &lv_font_montserrat_16,
                                  cx + px[i] * (r - 30) - 8,
                                  cy + py[i] * (r - 30) - 10, 0);
            (void)l;
        }
    }
    for (int i = 0; i < 3; i++) {
        if (i == 2 && !secs_hand) { g_hand[2] = nullptr; continue; }
        lv_obj_t *l = lv_line_create(p);
        lv_obj_set_pos(l, 0, 0);
        lv_obj_set_style_line_width(l, (i == 0) ? 5 : (i == 1) ? 3 : 2, 0);
        lv_obj_set_style_line_color(l, lv_color_hex(i == 2 ? accent : fg), 0);
        lv_obj_set_style_line_rounded(l, true, 0);
        g_hand[i] = l;
        g_hand_pts[i][0].x = cx; g_hand_pts[i][0].y = cy;
        g_hand_pts[i][1].x = cx; g_hand_pts[i][1].y = cy - 10;
        lv_line_set_points(l, g_hand_pts[i], 2);
    }
    lv_obj_t *dot = lv_obj_create(p);
    lv_obj_set_size(dot, 14, 14);
    lv_obj_set_pos(dot, cx - 7, cy - 7);
    lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(dot, lv_color_hex(accent), 0);
    lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(dot, 0, 0);

    g_dial_cx = cx; g_dial_cy = cy; g_dial_r = r; g_hand_sec = 255;
}

static void dial_update() {
    if (!g_hand[0] || g_dial_r <= 0 || !clock_valid()) return;
    time_t t; time(&t);
    struct tm lt; localtime_r(&t, &lt);
    if ((uint8_t)lt.tm_sec == g_hand_sec) return;   // only what changed
    g_hand_sec = (uint8_t)lt.tm_sec;
    float ang[3] = {
        (float)(lt.tm_hour % 12) * 30.0f + (float)lt.tm_min * 0.5f,
        (float)lt.tm_min * 6.0f + (float)lt.tm_sec * 0.1f,
        (float)lt.tm_sec * 6.0f
    };
    float len[3] = { g_dial_r * 0.52f, g_dial_r * 0.74f, g_dial_r * 0.84f };
    for (int i = 0; i < 3; i++) {
        if (!g_hand[i]) continue;
        float rad = ang[i] * 3.14159265f / 180.0f;
        g_hand_pts[i][0].x = (lv_coord_t)g_dial_cx;
        g_hand_pts[i][0].y = (lv_coord_t)g_dial_cy;
        g_hand_pts[i][1].x = (lv_coord_t)(g_dial_cx + sinf(rad) * len[i]);
        g_hand_pts[i][1].y = (lv_coord_t)(g_dial_cy - cosf(rad) * len[i]);
        lv_line_set_points(g_hand[i], g_hand_pts[i], 2);
    }
}

// ---- label refresh (single timer, every 500 ms) ---------------------------
static void ts_hhmm(uint32_t ts, char *out, size_t n) {
    time_t t = (time_t)ts;
    struct tm lt; localtime_r(&t, &lt);
    if (g_cfg.t24) snprintf(out, n, "%02d:%02d", lt.tm_hour, lt.tm_min);
    else {
        int hh = lt.tm_hour % 12; if (!hh) hh = 12;
        snprintf(out, n, "%d:%02d", hh, lt.tm_min);
    }
}

// Only touch a label when its text actually changes.  A 500 ms refresh that
// re-set identical strings would allocate/free every tick for a day straight.
static void lbl_set(lv_obj_t *l, const char *s) {
    if (!l || !s) return;
    const char *cur = lv_label_get_text(l);
    if (cur && !strcmp(cur, s)) return;
    lv_label_set_text(l, s);
}

static void night_stealth_tick();   // defined with NightOS
static void tripwire_service();     // defined with NightOS
static void crown_glitch_service(); // defined with CrownOS

static void ui_refresh_now() {
    char b[32];
    if (g_batt_lbl) {
        if (g_batt_pct < 0) snprintf(b, sizeof(b), "-- %%");
        else snprintf(b, sizeof(b), "%d%%%s", g_batt_pct, g_batt_ch ? "+" : "");
        lbl_set(g_batt_lbl, b);
        lv_obj_set_style_text_color(g_batt_lbl,
            lv_color_hex((g_batt_pct >= 0 && g_batt_pct < 15) ? g_danger : g_fg), 0);
    }
    if (g_conn_lbl) {
        lbl_set(g_conn_lbl, g_media.connected ? "LINK" : "OFF");
        lv_obj_set_style_text_color(g_conn_lbl,
            lv_color_hex(g_media.connected ? g_dim : g_warn), 0);
    }
    if (g_clock_lbl) {
        char t[16];
        clock_fmt_time(t, sizeof(t), false);
        lbl_set(g_clock_lbl, t);
    }
    if (g_date_lbl) {
        char d[32];
        clock_fmt_date(d, sizeof(d));
        lbl_set(g_date_lbl, d);
    }
    if (g_sov_day) {
        char d[8];
        clock_fmt_day(d, sizeof(d));
        lbl_set(g_sov_day, d);
    }
    if (g_sov_date && clock_valid()) {
        char d[4];
        time_t t; time(&t);
        struct tm lt; localtime_r(&t, &lt);
        snprintf(d, sizeof(d), "%02d", lt.tm_mday);
        lbl_set(g_sov_date, d);
    }
    if (g_timer_lbl) {
        uint32_t ms = cd_remain_ms();
        snprintf(b, sizeof(b), "%02u:%02u", (unsigned)(ms / 60000UL),
                 (unsigned)((ms / 1000UL) % 60UL));
        lbl_set(g_timer_lbl, b);
    }
    if (g_sw_lbl) {
        char t[24];
        clock_fmt_dur((int64_t)(sw_ms() / 1000UL), t, sizeof(t));
        lbl_set(g_sw_lbl, t);
    }
    for (int i = 0; i < WCITY_MAX; i++) {
        if (!g_city_lbl[i]) continue;
        char t[16];
        clock_fmt_at_offset(g_city[i].off_min, t, sizeof(t));
        lbl_set(g_city_lbl[i], t);
    }
    night_stealth_tick();
    dial_update();
}

static void ui_tick(lv_timer_t *t) {
    (void)t;
    if (g_screen_off) return;
    crown_glitch_service();
    ui_refresh_now();
}

// ---- navigation + modal ---------------------------------------------------
static void show_chooser();
static void os_enter(int idx);
static void os_leave();
static void noir_rebuild();
static void crown_rebuild();
static void night_rebuild();
static void noir_tile_cb(lv_event_t *e);
static void night_flash_off();
static void night_stealth_stop();
static void night_stealth_tick();
static void breathe_stop();
static void crown_reveal();
static void crown_glitch_service();

static lv_obj_t *g_modal = nullptr;

static void ui_refresh_current() {
    if (g_cur_os == 0) noir_rebuild();
    else if (g_cur_os == 1) crown_rebuild();
    else if (g_cur_os == 2) night_rebuild();
    else show_chooser();
}

static void modal_close_ev(lv_event_t *e) {
    (void)e;
    // del_async: the CLOSE button lives inside the modal we are deleting.
    if (g_modal) { lv_obj_del_async(g_modal); g_modal = nullptr; }
    dcs_input_reset();
    ui_refresh_current();
}

static lv_obj_t *modal_open(const char *title) {
    if (g_modal) { lv_obj_del_async(g_modal); g_modal = nullptr; }
    lv_obj_t *m = lv_obj_create(lv_layer_top());
    g_modal = m;
    lv_obj_set_pos(m, 0, 0);
    lv_obj_set_size(m, 410, 502);
    lv_obj_set_style_bg_color(m, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(m, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(m, 0, 0);
    lv_obj_set_style_pad_all(m, 0, 0);
    lv_obj_clear_flag(m, LV_OBJ_FLAG_SCROLLABLE);

    ui_text(m, title, g_accent, &lv_font_montserrat_20, 20, 18, 300);
    ui_btn(m, 336, 14, 60, 34, "CLOSE", g_panel, g_dim, 15, modal_close_ev, NULL);
    ui_box(m, 20, 62, 370, 1, g_panel, 0);

    lv_obj_t *c = lv_obj_create(m);
    lv_obj_set_pos(c, 8, 72);
    lv_obj_set_size(c, 394, 414);
    lv_obj_set_style_bg_opa(c, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(c, 0, 0);
    lv_obj_set_style_pad_all(c, 0, 0);
    ui_body_scroll(c);
    return c;
}

static void ui_nav(lv_obj_t *root) {
    if (!root) return;
    lv_obj_t *old = lv_scr_act();
    lv_scr_load(root);
    if (old && old != root) lv_obj_del_async(old);
    dcs_input_reset();
    g_last_nav_ms = millis();
    g_last_touch_ms = millis();
    if (g_screen_off) dcs_screen_set(false);
    ui_refresh_now();
}

static void ui_exit_cb(lv_event_t *e) {
    (void)e;
    os_leave();
}

// ---------------------------------------------------------------------------
//  14.5  QUICK SHEET + ALARMS  (above every experience)
// ---------------------------------------------------------------------------
//  The quick sheet is reached by long-pressing any experience's top bar.  It
//  edits the settings that are shared by all three, so a user never has to
//  remember which OS owns brightness or privacy.

static void alarm_overlay_close() {
    if (g_ring_overlay) {
        lv_obj_del_async(g_ring_overlay);   // may be called from its own button
        g_ring_overlay = nullptr;
    }
}

static void alarm_dismiss(bool snooze) {
    if (snooze && clock_valid()) g_snooze_at = time(NULL) + 300;
    else g_snooze_at = 0;
    int was = g_ring;
    g_ring = -1;
    alarm_overlay_close();
    dcs_log(DCS_PATH_SHARED, "alarm %s (slot %d)", snooze ? "snoozed" : "dismissed", was);
    ui_refresh_current();
}

static void alarm_btn_cb(lv_event_t *e) {
    alarm_dismiss((int)(intptr_t)lv_event_get_user_data(e) == 1);
}

static void alarm_ring(int idx) {
    if (g_ring != -1) return;
    g_ring = idx;
    g_snooze_at = 0;
    if (g_screen_off) dcs_screen_set(false);
    alarm_overlay_close();

    lv_obj_t *o = lv_obj_create(lv_layer_top());
    g_ring_overlay = o;
    lv_obj_set_pos(o, 0, 0);
    lv_obj_set_size(o, 410, 502);
    lv_obj_set_style_bg_color(o, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(o, 0, 0);
    lv_obj_set_style_pad_all(o, 0, 0);
    lv_obj_clear_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(o, LV_OBJ_FLAG_CLICKABLE);        // eat taps on the UI below

    lv_obj_t *ring = lv_obj_create(o);
    lv_obj_set_size(ring, 214, 214);
    lv_obj_align(ring, LV_ALIGN_TOP_MID, 0, 96);
    lv_obj_set_style_radius(ring, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(ring, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(ring, 3, 0);
    lv_obj_set_style_border_color(ring, lv_color_hex(g_accent), 0);
    lv_obj_clear_flag(ring, LV_OBJ_FLAG_SCROLLABLE);

    char t[16];
    clock_fmt_time(t, sizeof(t), false);
    lv_obj_t *l = ui_text(o, t, g_fg, &lv_font_montserrat_48, 0, 0, 0);
    lv_obj_align(l, LV_ALIGN_TOP_MID, 0, 168);
    lv_obj_t *w = ui_text(o, idx == -2 ? "SNOOZE" : "ALARM", g_accent,
                          &lv_font_montserrat_18, 0, 0, 0);
    lv_obj_align(w, LV_ALIGN_TOP_MID, 0, 236);
    lv_obj_t *d = ui_text(o, idx == -2 ? "five more minutes" : "daily alarm",
                          g_dim, &lv_font_montserrat_12, 0, 0, 0);
    lv_obj_align(d, LV_ALIGN_TOP_MID, 0, 266);

    ui_btn(o, 44, 330, 152, 54, "SNOOZE 5", g_panel, g_fg, 14, alarm_btn_cb,
           (void *)(intptr_t)1);
    ui_btn(o, 214, 330, 152, 54, "DISMISS", g_accent, 0x050407, 14, alarm_btn_cb,
           (void *)(intptr_t)0);
    ui_text(o, "Alarms need the module running; power-off cancels them.",
            g_dim, &lv_font_montserrat_10, 44, 408, 330);

    lv_obj_move_foreground(o);
    dcs_log(DCS_PATH_SHARED, "alarm ringing (slot %d)", idx);
}

static void alarm_edit_cb(lv_event_t *e) {
    int code = (int)(intptr_t)lv_event_get_user_data(e);
    int i = code / 10, act = code % 10;
    if (i < 0 || i >= ALARM_MAX) return;
    if (act == 0)      g_alarm[i].on = g_alarm[i].on ? 0 : 1;
    else if (act == 1) g_alarm[i].hh = (uint8_t)((g_alarm[i].hh + 23) % 24);
    else if (act == 2) g_alarm[i].hh = (uint8_t)((g_alarm[i].hh + 1) % 24);
    else if (act == 3) g_alarm[i].mm = (uint8_t)((g_alarm[i].mm + 55) % 60);
    else if (act == 4) g_alarm[i].mm = (uint8_t)((g_alarm[i].mm + 5) % 60);
    else if (act >= 5 && act <= 11) {
        uint8_t bit = (uint8_t)(1 << (act - 5));
        if ((g_alarm[i].days & bit) && g_alarm[i].days == bit) return;  // keep >= 1 day
        g_alarm[i].days ^= bit;
    }
    alarm_save();
    dcs_log(DCS_PATH_SHARED, "alarm %d edited", i);
    alarm_editor();
}

static void alarm_editor() {
    lv_obj_t *c = modal_open("ALARMS");
    ui_text(c, "Three daily alarms, shared by every experience. Snooze is 5 min.",
            g_dim, &lv_font_montserrat_12, 12, 6, 356);
    int y = 30;
    static const char *dn[7] = { "S", "M", "T", "W", "T", "F", "S" };
    for (int i = 0; i < ALARM_MAX; i++) {
        AlarmCfg &a = g_alarm[i];
        lv_obj_t *row = ui_frame(c, 0, y, 378, 90, g_panel, 10, g_panel, 1);
        char hm[12];
        snprintf(hm, sizeof(hm), "%02u:%02u", (unsigned)a.hh, (unsigned)a.mm);
        ui_text(row, hm, a.on ? g_fg : g_dim, &lv_font_montserrat_22, 12, 8, 0);
        ui_text(row, a.on ? (a.days == 0x7F ? "DAILY" : "CUSTOM") : "OFF",
                a.on ? g_accent : g_dim, &lv_font_montserrat_10, 14, 38, 0);
        ui_btn(row, 92, 10, 52, 32, a.on ? "ON" : "OFF",
               a.on ? g_accent : g_bg, a.on ? 0x050407 : g_dim, 8,
               alarm_edit_cb, (void *)(intptr_t)(i * 10 + 0));
        ui_btn(row, 148, 10, 48, 32, "H-", g_bg, g_dim, 8,
               alarm_edit_cb, (void *)(intptr_t)(i * 10 + 1));
        ui_btn(row, 200, 10, 48, 32, "H+", g_bg, g_dim, 8,
               alarm_edit_cb, (void *)(intptr_t)(i * 10 + 2));
        ui_btn(row, 252, 10, 48, 32, "M-", g_bg, g_dim, 8,
               alarm_edit_cb, (void *)(intptr_t)(i * 10 + 3));
        ui_btn(row, 304, 10, 56, 32, "M+", g_bg, g_dim, 8,
               alarm_edit_cb, (void *)(intptr_t)(i * 10 + 4));
        for (int d = 0; d < 7; d++) {
            bool on = (a.days >> d) & 1;
            ui_btn(row, 14 + d * 50, 54, 44, 24, dn[d], on ? g_accent : g_bg,
                   on ? 0x050407 : g_dim, 7, alarm_edit_cb,
                   (void *)(intptr_t)(i * 10 + 5 + d));
        }
        y += 98;
    }
    ui_text(c, "Steps are 1 hour / 5 minutes. Alarms ring only on the selected days "
               "and survive a reset; a powered-off watch cannot ring.",
            g_dim, &lv_font_montserrat_10, 12, y + 2, 350);
}

static void quick_tune_cb(lv_event_t *e) {
    int d = (int)(intptr_t)lv_event_get_user_data(e);
    int b = (int)g_cfg.brightness + d;
    if (b < DCS_MIN_BRIGHTNESS) b = DCS_MIN_BRIGHTNESS;
    if (b > 255) b = 255;
    g_cfg.brightness = (uint8_t)b;
    sharedcfg_save();
    power_apply();
    quick_sheet();
}

static void quick_power_cb(lv_event_t *e) {
    (void)e;
    power_set((uint8_t)((g_cfg.power + 1) % 4));
    quick_sheet();
}
static void quick_privacy_cb(lv_event_t *e) {
    (void)e;
    g_cfg.privacy = (uint8_t)((g_cfg.privacy + 1) % 3);
    sharedcfg_save();
    quick_sheet();
}
static void quick_alarm_cb(lv_event_t *e) {
    (void)e;
    alarm_editor();
}
static const uint8_t kSleepOpts[5] = { 0, 1, 5, 15, 30 };
static void quick_sleep_cb(lv_event_t *e) {
    (void)e;
    int cur = 0;
    for (int i = 0; i < 5; i++) if (kSleepOpts[i] == g_cfg.sleep_min) cur = i;
    g_cfg.sleep_min = kSleepOpts[(cur + 1) % 5];
    sharedcfg_save();
    quick_sheet();
}
static void quick_deep_cb(lv_event_t *e) {
    (void)e;
    dcs_sleep_now("quick sheet");
}
static void quick_off_cb(lv_event_t *e) {
    (void)e;
    if (g_modal) { lv_obj_del_async(g_modal); g_modal = nullptr; }
    dcs_screen_set(true);
}

static void quick_sheet() {
    lv_obj_t *c = modal_open("QUICK");
    char v[64];

    ui_text(c, "BRIGHTNESS", g_dim, &lv_font_montserrat_12, 12, 6, 0);
    ui_btn(c, 12, 28, 78, 42, "-", g_panel, g_fg, 10, quick_tune_cb, (void *)(intptr_t)-15);
    lv_obj_t *bv = ui_box(c, 98, 28, 182, 42, g_panel, 10);
    snprintf(v, sizeof(v), "%u / 255", (unsigned)g_cfg.brightness);
    lv_obj_t *bl = ui_text(bv, v, g_fg, &lv_font_montserrat_14, 0, 0, 0);
    lv_obj_center(bl);
    ui_btn(c, 288, 28, 78, 42, "+", g_panel, g_fg, 10, quick_tune_cb, (void *)(intptr_t)15);

    snprintf(v, sizeof(v), "POWER      %s", kPwrName[g_cfg.power]);
    ui_btn(c, 12, 84, 354, 40, v, g_panel, g_fg, 10, quick_power_cb, NULL);
    snprintf(v, sizeof(v), "PRIVACY    %s", kPrivacyName[g_cfg.privacy]);
    ui_btn(c, 12, 132, 354, 40, v, g_panel, g_fg, 10, quick_privacy_cb, NULL);

    char nxt[12];
    if (alarm_next_str(nxt, sizeof(nxt)))
        snprintf(v, sizeof(v), "ALARMS     NEXT %s", nxt);
    else
        snprintf(v, sizeof(v), "ALARMS     NONE SET");
    ui_btn(c, 12, 180, 354, 40, v, g_panel, g_accent, 10, quick_alarm_cb, NULL);

    if (g_cfg.sleep_min)
        snprintf(v, sizeof(v), "AUTO SLEEP %u MIN", (unsigned)g_cfg.sleep_min);
    else
        snprintf(v, sizeof(v), "AUTO SLEEP OFF");
    ui_btn(c, 12, 228, 354, 40, v, g_panel, g_fg, 10, quick_sleep_cb, NULL);

    ui_btn(c, 12, 276, 354, 44, "SLEEP NOW", g_panel, g_fg, 10, quick_deep_cb, NULL);
    ui_btn(c, 12, 328, 354, 44, "SCREEN OFF", g_panel, g_dim, 10, quick_off_cb, NULL);
    ui_btn(c, 12, 380, 354, 44, "SYSTEM  /  WI-FI  /  THEME  /  RESET",
           g_panel, g_accent, 10, quick_system_cb, NULL);
    ui_text(c, "Deep sleep wakes on BOOT, or by itself for the next alarm. "
               "Long-press a top bar for this sheet.",
            g_dim, &lv_font_montserrat_10, 12, 434, 350);
}

static void quick_system_cb(lv_event_t *e) {
    (void)e;
    system_sheet();
}

static void quick_sheet_cb(lv_event_t *e) {
    (void)e;
    quick_sheet();
}

// ---------------------------------------------------------------------------
//  14.6  SYSTEM  --  Wi-Fi clock sync, shared theme, wallpaper, developer
//        info and factory reset.  The LauncherOS conveniences, wired to the
//        shared services instead of to a launcher screen.
// ---------------------------------------------------------------------------
//  Same credentials file as LauncherOS (/WATCH/wifi/config.txt), plus an
//  optional module-local copy (/WATCH/SHARED/wifi.txt) that wins when present:
//
//      WIFI1_SSID=MyNetwork
//      WIFI1_PASS=secret
//      GMT_OFFSET=1            # hours, may be negative or fractional
//
//  NTP only ever runs from loop(), never inside an LVGL event: the radio needs
//  seconds, the UI must not be blocked by it, and a failed sync must not leave
//  the UI half-rebuilt.
#define DCS_NET_MAX 5
struct NetAp { char ssid[33]; char pass[65]; };
static NetAp g_nets[DCS_NET_MAX];
static uint8_t g_net_n = 0;
static bool g_net_cfg_done = false;
static bool g_net_req = false;          // manual sync requested from the UI
static volatile bool g_net_busy = false;
static char g_net_status[56] = "not checked";
static uint32_t g_net_last_ms = 0;      // last attempt (0 = never)
static uint32_t g_net_ok_ms = 0;        // last successful sync
static bool g_net_ever_ok = false;

static void net_trim(char *s) {
    if (!s) return;
    char *p = s;
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
    if (p != s) memmove(s, p, strlen(p) + 1);
    size_t n = strlen(s);
    while (n && (s[n-1] == ' ' || s[n-1] == '\t' || s[n-1] == '\r' || s[n-1] == '\n')) s[--n] = 0;
}

static void net_config_load() {
    g_net_cfg_done = true;
    g_net_n = 0;
    char buf[900];
    if (!fs_read_text("/WATCH/SHARED/wifi.txt", buf, sizeof(buf)) &&
        !fs_read_text("/WATCH/wifi/config.txt", buf, sizeof(buf))) {
        snprintf(g_net_status, sizeof(g_net_status), "no wifi.txt");
        return;
    }
    bool have_off = false;
    int32_t off_sec = 0;
    char *save = NULL;
    for (char *line = strtok_r(buf, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
        net_trim(line);
        if (!line[0] || line[0] == '#' || line[0] == ';') continue;
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = 0;
        char *k = line, *v = eq + 1;
        net_trim(k); net_trim(v);
        if (!k[0] || !v[0]) continue;
        if (!strncmp(k, "WIFI", 4) && k[4] >= '1' && k[4] <= '9') {
            int slot = k[4] - '1';
            if (slot >= DCS_NET_MAX) continue;
            if (!strcmp(k + 5, "_SSID")) {
                strncpy(g_nets[slot].ssid, v, sizeof(g_nets[slot].ssid) - 1);
                g_nets[slot].ssid[sizeof(g_nets[slot].ssid) - 1] = 0;
                if (slot + 1 > g_net_n) g_net_n = (uint8_t)(slot + 1);
            } else if (!strcmp(k + 5, "_PASS")) {
                strncpy(g_nets[slot].pass, v, sizeof(g_nets[slot].pass) - 1);
                g_nets[slot].pass[sizeof(g_nets[slot].pass) - 1] = 0;
                if (slot + 1 > g_net_n) g_net_n = (uint8_t)(slot + 1);
            }
        } else if (!strcmp(k, "GMT_OFFSET")) {
            float h = strtof(v, NULL);
            if (h >= -14.0f && h <= 14.0f) { off_sec = (int32_t)(h * 3600.0f); have_off = true; }
        }
    }
    // Compact: drop holes so every counted slot has a network name.
    uint8_t w = 0;
    for (uint8_t i = 0; i < g_net_n; i++) {
        if (!g_nets[i].ssid[0]) continue;
        if (w != i) g_nets[w] = g_nets[i];
        w++;
    }
    g_net_n = w;
    if (have_off && off_sec != g_tp_gmt) {
        time_apply_tz(off_sec);
        if (clock_valid()) time_publish("wifi.txt offset", true);
    }
    snprintf(g_net_status, sizeof(g_net_status), g_net_n ? "ready (%u network%s)" : "no networks",
             (unsigned)g_net_n, g_net_n == 1 ? "" : "s");
}

static void net_pump() {
    for (int i = 0; i < 4; i++) { lv_timer_handler(); delay(10); }
}

// Returns true when the clock was actually set from NTP.
static bool wifi_sync_now() {
    if (g_net_busy) return false;
    if (!g_net_cfg_done) net_config_load();
    if (!g_net_n) {
        snprintf(g_net_status, sizeof(g_net_status), "no network in wifi.txt");
        return false;
    }
#if DCS_USE_BLE
    if (g_media.connected) {          // never trade a live phone link for NTP
        snprintf(g_net_status, sizeof(g_net_status), "phone link busy");
        return false;
    }
#endif
    g_net_busy = true;
    g_net_last_ms = millis();
    snprintf(g_net_status, sizeof(g_net_status), "connecting...");
    dcs_log(DCS_PATH_SHARED, "wifi sync: %u networks", (unsigned)g_net_n);

    WiFi.persistent(false);
    WiFi.mode(WIFI_STA);
    WiFi.setSleep(WIFI_PS_MIN_MODEM);
    WiFi.setTxPower(WIFI_POWER_8_5dBm);

    bool joined = false;
    for (uint8_t i = 0; i < g_net_n && !joined; i++) {
        WiFi.begin(g_nets[i].ssid, g_nets[i].pass);
        uint32_t until = millis() + 7000;
        while (millis() < until) {
            if (WiFi.status() == WL_CONNECTED) { joined = true; break; }
            net_pump();
        }
        if (!joined) WiFi.disconnect(true, false);
    }

    bool ok = false;
    if (joined) {
        snprintf(g_net_status, sizeof(g_net_status), "connected, syncing");
        time_apply_tz(g_tp_gmt);       // TZ string is RAM-only; re-pin before NTP
        configTime(g_tp_gmt, 0, "pool.ntp.org", "time.cloudflare.com");
        struct tm tm;
        for (int i = 0; i < 24 && !ok; i++) {
            if (getLocalTime(&tm, 250)) ok = true;
            else net_pump();
        }
        if (ok) {
            time_publish("ntp", false);
            g_net_ok_ms = millis();
            g_net_ever_ok = true;
            snprintf(g_net_status, sizeof(g_net_status), "synced over Wi-Fi");
            dcs_log(DCS_PATH_SHARED, "ntp ok");
        } else {
            snprintf(g_net_status, sizeof(g_net_status), "no NTP reply");
        }
    } else {
        snprintf(g_net_status, sizeof(g_net_status), "no Wi-Fi connection");
        dcs_log(DCS_PATH_SHARED, "wifi sync failed: no connection");
    }

    WiFi.disconnect(true, true);
    WiFi.mode(WIFI_OFF);
    g_net_last_ms = millis();
    g_net_busy = false;
    return ok;
}

// Auto top-up: at boot when the clock is unknown, then every six hours while
// the panel is on.  A failed attempt backs off for 15 minutes rather than
// retrying on every loop pass.
static void net_service() {
    if (g_net_busy) return;
    if (g_net_req) { g_net_req = false; wifi_sync_now(); if (g_modal) wifi_sheet(); return; }
    if (!g_cfg.auto_ntp) return;
    if (g_screen_off || g_ring != -1) return;
    if (!g_net_cfg_done) { net_config_load(); return; }
    if (!g_net_n) return;
    uint32_t now = millis();
    if (!clock_valid()) {
        if (now < 4000) return;          // let the first paint happen first
        if (g_net_last_ms && now - g_net_last_ms < 15UL * 60000UL) return;
        wifi_sync_now();
        return;
    }
    // A valid clock does not need the radio during the splash: wait 30 s so a
    // deep-sleep wake shows the face before any Wi-Fi association starts.
    if (!g_net_last_ms && now < 30000) return;
    uint32_t since = g_net_ok_ms ? now - g_net_ok_ms : now - g_net_last_ms;
    if (since > 6UL * 3600000UL) wifi_sync_now();
}

// ---- wallpapers: /WATCH/PHOTOS/*.raw or *.bin, RGB565 LE, 410x502 ---------
//  Same convention as LauncherOS, so one export serves both apps.  The frame
//  lives in PSRAM and is only loaded while a wallpaper is actually selected.
#define DCS_MAX_PHOTOS 12
static char g_photo[DCS_MAX_PHOTOS][72];
static int  g_photo_n = 0;
static uint8_t *g_wall_buf = nullptr;
static lv_img_dsc_t g_wall_dsc;
static bool g_wall_ok = false;
static int  g_wall_loaded = -1;   // photo index, -1 = none

static void photos_scan() {
    g_photo_n = 0;
    if (!g_sd_ok) return;
    File dir = SD_MMC.open("/WATCH/PHOTOS");
    if (!dir || !dir.isDirectory()) return;
    File f = dir.openNextFile();
    while (f && g_photo_n < DCS_MAX_PHOTOS) {
        if (!f.isDirectory()) {
            String n = f.name();
            int s = n.lastIndexOf('/'); if (s >= 0) n = n.substring(s + 1);
            String lo = n; lo.toLowerCase();
            if (lo.endsWith(".raw") || lo.endsWith(".bin")) {
                snprintf(g_photo[g_photo_n], sizeof(g_photo[0]), "/WATCH/PHOTOS/%s", n.c_str());
                g_photo_n++;
            }
        }
        f = dir.openNextFile();
    }
    dir.close();
}

static void wall_free() {
    if (g_wall_buf) { free(g_wall_buf); g_wall_buf = nullptr; }
    g_wall_ok = false;
    g_wall_loaded = -1;
}

static bool wall_load(int idx) {
    if (idx < 0) { wall_free(); return false; }
    if (!g_sd_ok) return false;
    if (!g_photo_n) photos_scan();
    if (idx >= g_photo_n) { wall_free(); return false; }
    if (g_wall_ok && g_wall_loaded == idx) return true;
    const size_t need = (size_t)WC_LCD_WIDTH * WC_LCD_HEIGHT * 2;
    File f = SD_MMC.open(g_photo[idx], FILE_READ);
    if (!f) return false;
    size_t got = (size_t)f.size();
    if (got < need) { f.close(); dcs_log(DCS_PATH_NOIR, "wallpaper too small: %s", g_photo[idx]); return false; }
    if (!g_wall_buf) g_wall_buf = (uint8_t *)heap_caps_malloc(need, MALLOC_CAP_SPIRAM);
    if (!g_wall_buf) { f.close(); dcs_log(DCS_PATH_NOIR, "wallpaper: no PSRAM"); return false; }
    size_t rd = f.read(g_wall_buf, need);
    f.close();
    if (rd != need) return false;
    g_wall_dsc.header.cf = LV_IMG_CF_TRUE_COLOR;
    g_wall_dsc.header.always_zero = 0;
    g_wall_dsc.header.w = WC_LCD_WIDTH;
    g_wall_dsc.header.h = WC_LCD_HEIGHT;
    g_wall_dsc.data_size = (uint32_t)need;
    g_wall_dsc.data = g_wall_buf;
    g_wall_ok = true;
    g_wall_loaded = idx;
    dcs_log(DCS_PATH_NOIR, "wallpaper loaded: %s", g_photo[idx]);
    return true;
}

// Called at the top of NoirOS home.  The image goes to the back of the root
// screen and the body loses its fill, so tiles and text still sit on top and
// scrolling never drags the photo around.
static void noir_wallpaper(lv_obj_t *body) {
    if (!g_cfg.wall || !g_wall_ok) return;
    lv_obj_t *root = lv_obj_get_parent(body);
    if (!root) return;
    lv_obj_set_style_bg_opa(body, LV_OPA_TRANSP, 0);
    lv_obj_t *img = lv_img_create(root);
    lv_img_set_src(img, &g_wall_dsc);
    lv_obj_set_pos(img, 0, 0);
    lv_obj_set_style_img_recolor(img, lv_color_hex(0x000000), 0);
    lv_obj_set_style_img_recolor_opa(img, 120, 0);   // readability scrim
    lv_obj_clear_flag(img, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(img, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_move_background(img);
}

// ---- little helpers the sheets need --------------------------------------
static const char *wake_reason_str() {
    switch (esp_sleep_get_wakeup_cause()) {
        case ESP_SLEEP_WAKEUP_EXT1:  return "BOOT button";
        case ESP_SLEEP_WAKEUP_TIMER: return "timer (alarm)";
        case ESP_SLEEP_WAKEUP_TOUCHPAD: return "touch";
        default: break;
    }
    return (esp_reset_reason() == ESP_RST_POWERON) ? "power on" : "software reset";
}

static void net_theme_cb(lv_event_t *e) {
    (void)e;
    g_cfg.theme = (uint8_t)((g_cfg.theme + 1) % 7);
    sharedcfg_save();
    dcs_log(DCS_PATH_SHARED, "theme accent -> %s", kThemeName[g_cfg.theme]);
    if (g_cur_os >= 0) ui_refresh_current();
    system_sheet();
}

static void net_wall_cb(lv_event_t *e) {
    (void)e;
    if (!g_photo_n) photos_scan();
    int total = g_photo_n + 1;
    g_cfg.wall = (uint8_t)((g_cfg.wall + 1) % total);
    if (g_cfg.wall == 0) wall_free();
    else if (!wall_load(g_cfg.wall - 1)) g_cfg.wall = 0;
    sharedcfg_save();
    if (g_cur_os == 0) noir_rebuild();
    system_sheet();
}

static void net_ntp_cb(lv_event_t *e) {
    (void)e;
    g_cfg.auto_ntp = g_cfg.auto_ntp ? 0 : 1;
    sharedcfg_save();
    system_sheet();
}

static void net_off_cb(lv_event_t *e) {
    int d = (int)(intptr_t)lv_event_get_user_data(e);
    int32_t off = g_tp_gmt + (int32_t)d * 1800;
    if (off < -14 * 3600) off = -14 * 3600;
    if (off >  14 * 3600) off =  14 * 3600;
    time_apply_tz(off);
    if (clock_valid()) time_publish("offset changed", true);
    wifi_sheet();
}

static void net_sync_cb(lv_event_t *e) {
    (void)e;
    g_net_req = true;
    snprintf(g_net_status, sizeof(g_net_status), "queued - syncing next loop");
    wifi_sheet();
}

static void wifi_sheet() {
    lv_obj_t *c = modal_open("WI-FI  /  CLOCK");
    if (!g_net_cfg_done) net_config_load();
    char v[96];

    snprintf(v, sizeof(v), "STATUS     %s", g_net_busy ? "SYNCING" : g_net_status);
    ui_btn(c, 12, 4, 370, 38, v, g_panel, g_net_ever_ok ? g_accent : g_fg, 10, NULL, NULL);

    snprintf(v, sizeof(v), "UTC %+ld:%02ld", (long)(g_tp_gmt / 3600),
             labs((long)(g_tp_gmt % 3600)) / 60);
    ui_text(c, v, g_fg, &lv_font_montserrat_16, 14, 52, 0);
    ui_btn(c, 150, 46, 52, 32, "-30", g_panel, g_dim, 8, net_off_cb, (void *)(intptr_t)-1);
    ui_btn(c, 208, 46, 52, 32, "+30", g_panel, g_dim, 8, net_off_cb, (void *)(intptr_t)1);
    ui_btn(c, 268, 46, 112, 32, "SYNC NOW", g_accent, 0x050407, 8, net_sync_cb, NULL);

    snprintf(v, sizeof(v), "AUTO NTP   %s", g_cfg.auto_ntp ? "ON (6 H)" : "OFF");
    ui_btn(c, 12, 88, 370, 40, v, g_panel, g_fg, 10, net_ntp_cb, NULL);

    snprintf(v, sizeof(v), "%u network%s in wifi.txt", (unsigned)g_net_n, g_net_n == 1 ? "" : "s");
    ui_text(c, v, g_dim, &lv_font_montserrat_12, 12, 136, 360);
    for (int i = 0; i < g_net_n; i++)
        ui_text(c, g_nets[i].ssid, g_fg, &lv_font_montserrat_12, 20, 158 + i * 20, 340);

    ui_text(c, "Credentials live in /WATCH/SHARED/wifi.txt, or the LauncherOS "
               "file /WATCH/wifi/config.txt:\n\nWIFI1_SSID=MyNetwork\n"
               "WIFI1_PASS=secret\nGMT_OFFSET=1\n\n"
               "NTP runs only while the screen is on, drops the radio when it is "
               "done, and never interrupts a live phone link in a BLE build.",
            g_dim, &lv_font_montserrat_10, 12, 190 + g_net_n * 20 + 8, 360);
}

static void dev_sheet() {
    lv_obj_t *c = modal_open("DEVELOPER INFO");
    char v[420];
    uint32_t up = millis() / 1000UL;
    uint32_t sd_mb = 0;
    if (g_sd_ok) sd_mb = (uint32_t)(SD_MMC.cardSize() / (1024ULL * 1024ULL));
    snprintf(v, sizeof(v),
        "DualCore Special Editions  v%s\n"
        "manifest API %d   build: LVGL 8, Arduino-ESP32\n"
        "options: BLE %s, SensorLib %s\n\n"
        "CTRL  heap %u free / %u min\n"
        "PSRAM %u free / %u total\n"
        "SD    %s%s%u MB\n"
        "reset %s   wake %s\n"
        "uptime %luh %02lum\n"
        "boot strikes %u/3\n\n"
        "inbox %d   alarms %d   cities %d   events %d\n"
        "power %s   bright %u   deep-sleep after %u min\n"
        "accent %s   wallpaper %u/%d\nclock %s",
        DCS_VERSION, DCS_MANIFEST_API,
        DCS_USE_BLE ? "on" : "off", DCS_USE_SENSORLIB ? "on" : "off",
        (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
        (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL),
        (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
        (unsigned)heap_caps_get_total_size(MALLOC_CAP_SPIRAM),
        g_sd_ok ? "mounted, " : "not mounted", g_sd_ok ? "" : "",
        (unsigned)sd_mb,
        reset_reason_str(), wake_reason_str(),
        (unsigned long)(up / 3600UL), (unsigned long)((up / 60UL) % 60UL),
        (unsigned)g_boot_strikes,
        notif_unread(), ALARM_MAX, g_city_n, event_count(),
        kPwrName[g_cfg.power], (unsigned)g_cfg.brightness, (unsigned)g_cfg.sleep_min,
        kThemeName[g_cfg.theme], (unsigned)g_cfg.wall, g_photo_n,
        g_net_ever_ok ? g_net_status : "clock from NVS/RTC");
    ui_text(c, v, g_fg, &lv_font_montserrat_12, 12, 4, 366);
    ui_btn(c, 12, 356, 366, 40, "CLOSE", g_panel, g_fg, 10, modal_close_ev, NULL);
}

static int g_reset_scope = -1;   // -1 none, 0..2 that OS, 3 everything

static void reset_do_cb(lv_event_t *e) {
    int scope = (int)(intptr_t)lv_event_get_user_data(e);
    if (g_modal) { lv_obj_del_async(g_modal); g_modal = nullptr; }
    if (scope < 0 || scope > 3) return;

    dcs_log(DCS_PATH_SHARED, "factory reset scope %d", scope);
    if (scope == 3) {
        fs_rm_tree(DCS_PATH_NOIR);
        fs_rm_tree(DCS_PATH_CROWN);
        fs_rm_tree(DCS_PATH_NIGHT);
        fs_rm_tree(DCS_PATH_SHARED);
        wall_free();
        notif_clear_all();
        g_cfg = SharedCfg{ DCS_DEFAULT_BRIGHTNESS, PWR_BALANCED, 1, 0, 1, 0, 0, 5, 0, 0, 1 };
        g_noir = NoirCfg{ 25, 5, 0, 0x1F, 1, 3 };
        g_crown = CrownCfg{ 6, 2, 0, 0, 1, 1, 1, 0 };
        g_night = NightCfg{ 0, 70, 1, 0, 22, 6, 0x7F, 0 };
    } else {
        fs_rm_tree(kManifests[scope].dir);
        if (scope == 0) g_noir = NoirCfg{ 25, 5, 0, 0x1F, 1, 3 };
        if (scope == 1) g_crown = CrownCfg{ 6, 2, 0, 0, 1, 1, 1, 0 };
        if (scope == 2) g_night = NightCfg{ 0, 70, 1, 0, 22, 6, 0x7F, 0 };
    }
    dcs_data_init();
    dcs_save_all();
    if (scope == 0 || scope == 3) show_chooser();
    else ui_refresh_current();
}

static void reset_cancel_cb(lv_event_t *e) {
    (void)e;
    reset_sheet();
}

static void reset_ask(int scope) {
    lv_obj_t *c = modal_open("CONFIRM RESET");
    char v[220];
    if (scope == 3)
        snprintf(v, sizeof(v), "Erase ALL three experience folders and the shared "
                 "inbox/timers/settings? Watch faces, events, focus history and "
                 "the paired-state data go with them. This cannot be undone.");
    else
        snprintf(v, sizeof(v), "Erase %s/config, /data, /assets, /cache and /logs, "
                 "then reload its defaults? The other two experiences are untouched.",
                 kManifests[scope].name);
    ui_text(c, v, g_fg, &lv_font_montserrat_14, 12, 4, 360);
    ui_btn(c, 12, 150, 170, 44, "CANCEL", g_panel, g_fg, 10, reset_cancel_cb, NULL);
    ui_btn(c, 198, 150, 176, 44, "ERASE", g_danger, 0xFFFFFF, 10, reset_do_cb,
           (void *)(intptr_t)scope);
    ui_text(c, "The module itself is not touched and the chooser stays reachable.",
            g_dim, &lv_font_montserrat_10, 12, 208, 350);
}

static void reset_pick_cb(lv_event_t *e) {
    int scope = (int)(intptr_t)lv_event_get_user_data(e);
    reset_ask(scope);
}

static void reset_sheet() {
    lv_obj_t *c = modal_open("FACTORY RESET");
    ui_text(c, "Resets configuration only - logs and the firmware stay put. "
               "Manifests are recreated with defaults.",
            g_dim, &lv_font_montserrat_12, 12, 4, 360);
    int y = 40;
    for (int i = 0; i < 3; i++) {
        char v[64];
        snprintf(v, sizeof(v), "RESET  %s", kManifests[i].name);
        ui_btn(c, 12, y, 370, 42, v, g_panel, g_fg, 10, reset_pick_cb, (void *)(intptr_t)i);
        y += 50;
    }
    ui_btn(c, 12, y + 4, 370, 42, "ERASE EVERYTHING (ALL THREE)",
           g_panel, g_danger, 10, reset_pick_cb, (void *)(intptr_t)3);
    ui_text(c, "/WATCH/NoirOS, /WATCH/CrownOS, /WATCH/NightOS and /WATCH/SHARED "
               "are the only folders this module ever writes.",
            g_dim, &lv_font_montserrat_10, 12, y + 60, 360);
}

static void system_boot_cb(lv_event_t *e) {
    (void)e;
    dev_sheet();
}

// FULL -> QUICK -> OFF -> FULL.  Each OS owns the artwork; this only chooses
// how long the handover runs, so nobody has to remember three settings.
static void bootmode_cb(lv_event_t *e) {
    (void)e;
    g_cfg.bootanim = (uint8_t)(g_cfg.bootanim == 1 ? 2 : g_cfg.bootanim == 2 ? 0 : 1);
    sharedcfg_save();
    system_sheet();
}
static void system_reset_cb(lv_event_t *e) {
    (void)e;
    reset_sheet();
}
static void system_wifi_cb(lv_event_t *e) {
    (void)e;
    wifi_sheet();
}
static void system_back_cb(lv_event_t *e) {
    (void)e;
    quick_sheet();
}

static void system_sheet() {
    lv_obj_t *c = modal_open("SYSTEM");
    char v[72];

    snprintf(v, sizeof(v), "WI-FI  /  CLOCK     %s", g_net_busy ? "SYNCING" : g_net_status);
    ui_btn(c, 12, 4, 370, 42, v, g_panel, g_accent, 10, system_wifi_cb, NULL);
    snprintf(v, sizeof(v), "THEME ACCENT        %s", kThemeName[g_cfg.theme]);
    ui_btn(c, 12, 54, 370, 42, v, g_panel, g_fg, 10, net_theme_cb, NULL);
    if (!g_photo_n) photos_scan();
    if (g_cfg.wall == 0) snprintf(v, sizeof(v), "NOIR WALLPAPER    OFF  (%d photos)", g_photo_n);
    else snprintf(v, sizeof(v), "NOIR WALLPAPER    %d / %d", (int)g_cfg.wall, g_photo_n);
    ui_btn(c, 12, 104, 370, 42, v, g_panel, g_fg, 10, net_wall_cb, NULL);

    static const char *kBootMode[3] = { "OFF", "FULL", "QUICK" };
    snprintf(v, sizeof(v), "BOOT SEQUENCE        %s", kBootMode[g_cfg.bootanim]);
    ui_btn(c, 12, 154, 370, 42, v, g_panel,
           g_cfg.bootanim ? g_accent : g_dim, 10, bootmode_cb, NULL);

    snprintf(v, sizeof(v), "DEVELOPER INFO      v%s", DCS_VERSION);
    ui_btn(c, 12, 204, 370, 42, v, g_panel, g_fg, 10, system_boot_cb, NULL);
    ui_btn(c, 12, 254, 370, 42, "FACTORY RESET", g_panel, g_danger, 10, system_reset_cb, NULL);

    snprintf(v, sizeof(v), "AUTO NTP %s   /   DEEP SLEEP %s",
             g_cfg.auto_ntp ? "ON" : "OFF",
             g_cfg.sleep_min ? "ON" : "OFF");
    ui_text(c, v, g_dim, &lv_font_montserrat_10, 12, 308, 360);
    ui_btn(c, 12, 330, 370, 40, "BACK TO QUICK SHEET", g_panel, g_fg, 10, system_back_cb, NULL);
    ui_text(c, "Boot sequence: FULL plays each OS's two-second handover, QUICK is the "
               "half-second form used on deep-sleep resume, OFF cuts straight in. "
               "A tap during the sequence skips it.",
            g_dim, &lv_font_montserrat_10, 12, 380, 360);
    ui_text(c, "Wallpapers: /WATCH/PHOTOS/*.raw or *.bin, 410x502 RGB565 little-endian "
               "- the same export LauncherOS uses. The frame is held in PSRAM and "
               "released when NOIR WALLPAPER goes back to OFF.",
            g_dim, &lv_font_montserrat_10, 12, 432, 360);
}

// ---------------------------------------------------------------------------
//  14.9  BOOT SEQUENCE  (one per experience, none of them a reboot)
// ---------------------------------------------------------------------------
//  Switching experiences is a context change: the old tree is torn down, a
//  different palette loads, another screen tree is built.  Each OS gets its
//  own sequence so the change reads as intentional, and every stage names
//  work that genuinely happened by the time it appears (manifest validated,
//  config loaded, shared inbox read, tree built).
//
//  Driven by an lv_timer, never by a blocking delay: touch stays live, a tap
//  skips to the end, and a slow SD card cannot wedge the UI.  Deep-sleep
//  resume always takes the short form -- a wake should feel instant.
#define BOOT_STAGES 4

struct BootSkin {
    uint32_t bg, accent, text, dim;
    const char *mark;                  // small line above the wordmark
    const char *title;
    const char *tag;
    const char *stage[BOOT_STAGES];
    uint8_t  flourish;                 // 0 progress bar, 1 arc, 2 scanline
};

static const BootSkin kBoot[3] = {
    {   0x04050A, 0x6E9BD1, 0xE8EDF2, 0x5C6672,
        "OS  01", "NOIR", "EVERYDAY  /  MINIMAL",
        { "mount /WATCH/NoirOS", "settings + smart tiles", "shared inbox",
          "building surface" }, 0 },
    {   0x0B0A07, 0xC9A44C, 0xF2EAD8, 0x8A7A5E,
        "OS  02", "CROWN", "MAISON  DUALCORE",
        { "mount /WATCH/CrownOS", "face library", "occasion profile",
          "case + dial" }, 1 },
    {   0x0A0305, 0xC8324A, 0xEBD9DC, 0x8C5A63,
        "OS  03", "NIGHT", "SHADOW  /  UTILITY",
        { "mount /WATCH/NightOS", "shadow profile", "quiet policy",
          "night surface" }, 2 },
};

static lv_obj_t   *g_boot_scr = nullptr;
static lv_obj_t   *g_boot_bar = nullptr;
static lv_obj_t   *g_boot_arc = nullptr;
static lv_obj_t   *g_boot_msg = nullptr;
static lv_obj_t   *g_boot_pct = nullptr;
static lv_timer_t *g_boot_tm  = nullptr;
static int   g_boot_os    = -1;
static int   g_boot_prog  = 0;
static int   g_boot_stage = -1;
static bool  g_boot_skip  = false;
static bool  g_boot_fast  = false;
static bool  g_boot_resume = false;    // next os_enter() uses the short form

static void boot_anim_opa(void *v, int32_t x) {
    lv_obj_set_style_text_opa((lv_obj_t *)v, (lv_opa_t)x, 0);
}
static void boot_anim_y(void *v, int32_t x) { lv_obj_set_y((lv_obj_t *)v, x); }
static void boot_anim_w(void *v, int32_t x) { lv_obj_set_width((lv_obj_t *)v, x); }

static void boot_skip_cb(lv_event_t *e) {
    (void)e;
    if (g_boot_prog > 4) g_boot_skip = true;   // ignore a carried-over release
}

static void boot_cancel() {
    if (g_boot_tm) { lv_timer_del(g_boot_tm); g_boot_tm = nullptr; }
    g_boot_os = -1;
    g_boot_scr = nullptr;      // whoever replaces the screen disposes of it
}

static void boot_finish() {
    if (g_boot_tm) { lv_timer_del(g_boot_tm); g_boot_tm = nullptr; }
    g_boot_scr = nullptr;      // noir/crown/night_rebuild() -> ui_nav() deletes it
    int os = g_boot_os;
    g_boot_os = -1;
    lv_refr_now(NULL);         // the last frame lands before the tree swap
    if (os == 0)      noir_rebuild();
    else if (os == 1) crown_rebuild();
    else if (os == 2) night_rebuild();
}

static void boot_tick(lv_timer_t *t) {
    (void)t;
    if (g_boot_os < 0 || !g_boot_scr) {
        if (g_boot_tm) { lv_timer_del(g_boot_tm); g_boot_tm = nullptr; }
        return;
    }
    if (g_boot_skip) g_boot_prog = 100;
    else g_boot_prog += g_boot_fast ? 11 : 2;      // 40 ms tick: ~0.4 s / ~2.0 s
    if (g_boot_prog > 100) g_boot_prog = 100;

    int stage = (g_boot_prog * BOOT_STAGES) / 100;
    if (stage >= BOOT_STAGES) stage = BOOT_STAGES - 1;
    if (stage != g_boot_stage) {
        g_boot_stage = stage;
        if (g_boot_msg) {
            lv_label_set_text(g_boot_msg, kBoot[g_boot_os].stage[stage]);
            lv_anim_t a;
            lv_anim_init(&a);
            lv_anim_set_var(&a, g_boot_msg);
            lv_anim_set_exec_cb(&a, boot_anim_opa);
            lv_anim_set_values(&a, 40, 235);
            lv_anim_set_time(&a, 180);
            lv_anim_start(&a);
        }
    }
    if (g_boot_bar) lv_bar_set_value(g_boot_bar, g_boot_prog, LV_ANIM_OFF);
    if (g_boot_arc) lv_arc_set_value(g_boot_arc, g_boot_prog);
    if (g_boot_pct) {
        char p[8];
        snprintf(p, sizeof(p), "%d%%", g_boot_prog);
        lbl_set(g_boot_pct, p);
    }
    if (g_boot_prog >= 100) boot_finish();
}

static void boot_begin(int idx, bool fast) {
    if (idx < 0 || idx > 2) return;
    boot_cancel();
    const BootSkin &s = kBoot[idx];
    g_boot_os = idx;
    g_boot_prog = 0;
    g_boot_stage = -1;
    g_boot_skip = false;
    g_boot_fast = fast;
    g_boot_bar = g_boot_arc = g_boot_msg = g_boot_pct = nullptr;

    // The chooser (or the previous experience) is about to be deleted, and the
    // shared widgets it owned are gone with it.  ui_tick runs once a second and
    // would otherwise write through those stale pointers for the length of the
    // sequence -- a use-after-free that only exists because of this screen.
    ui_clear_refs();

    lv_obj_t *scr = ui_screen(s.bg);
    g_boot_scr = scr;
    lv_obj_set_scroll_dir(scr, LV_DIR_NONE);
    lv_obj_add_flag(scr, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(scr, boot_skip_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *mark = ui_text(scr, s.mark, s.dim, &lv_font_montserrat_10, 0, 0, 0);
    lv_obj_align(mark, LV_ALIGN_TOP_MID, 0, 128);
    lv_obj_set_style_text_letter_space(mark, 4, 0);

    lv_obj_t *title = ui_text(scr, s.title, s.text, &lv_font_montserrat_44, 0, 0, 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 146);
    lv_obj_set_style_text_letter_space(title, 9, 0);

    lv_obj_t *tag = ui_text(scr, s.tag, s.accent, &lv_font_montserrat_10, 0, 0, 0);
    lv_obj_align(tag, LV_ALIGN_TOP_MID, 0, 202);
    lv_obj_set_style_text_letter_space(tag, 3, 0);
    ui_box(scr, 175, 224, 60, 1, s.accent, 0);

    // ---- per-OS flourish -------------------------------------------------
    if (s.flourish == 1) {
        // CrownOS: a sweeping arc, gold on black lacquer.
        g_boot_arc = lv_arc_create(scr);
        lv_obj_set_size(g_boot_arc, 128, 128);
        lv_obj_align(g_boot_arc, LV_ALIGN_TOP_MID, 0, 250);
        lv_arc_set_rotation(g_boot_arc, 270);
        lv_arc_set_bg_angles(g_boot_arc, 0, 360);
        lv_arc_set_range(g_boot_arc, 0, 100);
        lv_arc_set_value(g_boot_arc, 0);
        lv_obj_set_style_bg_opa(g_boot_arc, LV_OPA_TRANSP, LV_PART_KNOB);  // hide knob
        lv_obj_clear_flag(g_boot_arc, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_style_arc_width(g_boot_arc, 2, LV_PART_MAIN);
        lv_obj_set_style_arc_color(g_boot_arc, lv_color_hex(0x25211A), LV_PART_MAIN);
        lv_obj_set_style_arc_width(g_boot_arc, 3, LV_PART_INDICATOR);
        lv_obj_set_style_arc_color(g_boot_arc, lv_color_hex(s.accent), LV_PART_INDICATOR);
        // A three-point crown inside the arc: base bar plus three merlons.
        ui_box(scr, 177, 322, 56, 5, s.accent, 1);
        ui_box(scr, 181, 300, 12, 22, s.accent, 2);
        ui_box(scr, 199, 292, 12, 30, s.accent, 2);
        ui_box(scr, 217, 300, 12, 22, s.accent, 2);
    } else if (s.flourish == 2) {
        // NightOS: a scan line inside a dim frame, plus a breathing dot.
        ui_box(scr, 145, 250, 120, 120, s.bg, 6);
        lv_obj_t *fr = ui_box(scr, 145, 250, 120, 120, 0x000000, 6);
        lv_obj_set_style_bg_opa(fr, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(fr, 1, 0);
        lv_obj_set_style_border_color(fr, lv_color_hex(0x3A141B), 0);
        lv_obj_t *scan = ui_box(scr, 147, 254, 116, 2, s.accent, 1);
        lv_obj_set_style_bg_opa(scan, 140, 0);
        lv_anim_t sc;
        lv_anim_init(&sc);
        lv_anim_set_var(&sc, scan);
        lv_anim_set_exec_cb(&sc, boot_anim_y);
        lv_anim_set_values(&sc, 254, 364);
        lv_anim_set_time(&sc, 900);
        lv_anim_set_playback_time(&sc, 900);
        lv_anim_set_repeat_count(&sc, LV_ANIM_REPEAT_INFINITE);
        lv_anim_start(&sc);
        lv_obj_t *dot = ui_box(scr, 199, 304, 12, 12, s.accent, LV_RADIUS_CIRCLE);
        lv_obj_set_style_shadow_width(dot, 18, 0);
        lv_obj_set_style_shadow_color(dot, lv_color_hex(s.accent), 0);
        lv_obj_set_style_shadow_opa(dot, 150, 0);
    } else {
        // NoirOS: a single rule that grows, nothing rotates, nothing pulses.
        ui_box(scr, 145, 300, 120, 1, 0x1B2029, 0);
        lv_obj_t *grow = ui_box(scr, 145, 300, 4, 1, s.accent, 0);
        lv_anim_t ga;
        lv_anim_init(&ga);
        lv_anim_set_var(&ga, grow);
        lv_anim_set_exec_cb(&ga, boot_anim_w);
        lv_anim_set_values(&ga, 4, 120);
        lv_anim_set_time(&ga, 1200);
        lv_anim_set_path_cb(&ga, lv_anim_path_ease_in_out);
        lv_anim_start(&ga);
    }

    g_boot_msg = ui_text(scr, s.stage[0], s.dim, &lv_font_montserrat_12, 0, 0, 0);
    lv_obj_align(g_boot_msg, LV_ALIGN_TOP_MID, 0, 392);

    g_boot_bar = lv_bar_create(scr);
    lv_obj_set_size(g_boot_bar, 240, 3);
    lv_obj_align(g_boot_bar, LV_ALIGN_TOP_MID, 0, 418);
    lv_bar_set_range(g_boot_bar, 0, 100);
    lv_bar_set_value(g_boot_bar, 0, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(g_boot_bar, lv_color_hex(0x1A1D22), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(g_boot_bar, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(g_boot_bar, lv_color_hex(s.accent), LV_PART_INDICATOR);
    lv_obj_set_style_radius(g_boot_bar, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(g_boot_bar, 0, LV_PART_INDICATOR);

    g_boot_pct = ui_text(scr, "0%", s.dim, &lv_font_montserrat_10, 0, 0, 0);
    lv_obj_align(g_boot_pct, LV_ALIGN_TOP_MID, 0, 430);

    lv_obj_t *foot = ui_text(scr, "DUALCORE  SPECIAL  EDITIONS   v" DCS_VERSION,
                             s.dim, &lv_font_montserrat_10, 0, 0, 0);
    lv_obj_align(foot, LV_ALIGN_BOTTOM_MID, 0, -14);

    lv_obj_t *old = lv_scr_act();
    lv_scr_load(scr);
    if (old && old != scr) lv_obj_del_async(old);
    dcs_input_reset();
    g_last_nav_ms = millis();

    // The panel fades up into the sequence instead of cutting to it.
    g_cur_brightness = 0;
    g_gfx->setBrightness(0);
    dcs_fade_to(dcs_brightness_base(), 260);

    g_boot_tm = lv_timer_create(boot_tick, 40, NULL);
    dcs_log(kManifests[idx].dir, "boot sequence (%s)",
            fast ? "short" : (g_cfg.bootanim == 2 ? "quick" : "full"));
}

// ---------------------------------------------------------------------------
//  15.  EXPERIENCE MANAGER  (chooser + lifecycle)
// ---------------------------------------------------------------------------
//  States: UNLOADED -> LOADING -> ACTIVE -> (SUSPENDING) -> UNLOADED.
//  A screen tree is built only for the active experience and destroyed on
//  exit, which is what keeps three feature-rich OSes inside one heap.
enum ExpState : uint8_t { EXP_UNLOADED = 0, EXP_LOADING, EXP_ACTIVE, EXP_ERROR };
static ExpState g_exp_state = EXP_UNLOADED;

static void chooser_card_cb(lv_event_t *e) {
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    os_enter(idx);
}

static void chooser_launcher_cb(lv_event_t *e) {
    (void)e;
    dcs_launcher_return_now();
}

static void chooser_diag_cb(lv_event_t *e);
static void show_diagnostics();

static void show_chooser() {
    boot_cancel();              // a pending switch must not land on the chooser
    night_flash_off();          // the flashlight lives on the top layer
    night_stealth_stop();
    breathe_stop();
    g_cur_os = -1;
    g_exp_state = EXP_UNLOADED;
    if (g_modal) { lv_obj_del(g_modal); g_modal = nullptr; }
    ui_set_theme(3);
    ui_clear_refs();

    lv_obj_t *root = ui_screen(g_bg);
    ui_text(root, "DUALCORE", g_fg, &lv_font_montserrat_28, 20, 16, 0);
    lv_obj_t *sp = ui_text(root, "SPECIAL EDITIONS", g_accent, &lv_font_montserrat_12, 22, 52, 0);
    lv_obj_set_style_text_letter_space(sp, 3, 0);

    g_batt_lbl = ui_text(root, "-- %", g_fg, &lv_font_montserrat_16, 262, 28, 128);
    lv_obj_set_style_text_align(g_batt_lbl, LV_TEXT_ALIGN_RIGHT, 0);
    g_date_lbl = ui_text(root, "", g_dim, &lv_font_montserrat_12, 262, 50, 128);
    lv_obj_set_style_text_align(g_date_lbl, LV_TEXT_ALIGN_RIGHT, 0);

    char st[96], an[12];
    if (alarm_next_str(an, sizeof(an)))
        snprintf(st, sizeof(st), "%d UNREAD   /   ALARM %s   /   %s", notif_unread(), an,
                 g_media.connected ? "LINK UP" : "PHONE OFFLINE");
    else
        snprintf(st, sizeof(st), "%d UNREAD   /   NO ALARM   /   %s", notif_unread(),
                 g_media.connected ? "LINK UP" : "PHONE OFFLINE");
    ui_text(root, st, g_dim, &lv_font_montserrat_10, 22, 70, 366);

    struct CardDef { const char *name; const char *line1; const char *line2; uint32_t accent; };
    static const CardDef cards[3] = {
        { "NoirOS",  "EVERYDAY MINIMAL",  "inbox / focus / world clock", 0x6E9BD1 },
        { "CrownOS", "LUXURY FACES",      "studio / occasions / events", 0xC9A44C },
        { "NightOS", "NINJA UTILITY",     "shadow / quiet / schedule",   0xB32A3C },
    };
    for (int i = 0; i < 3; i++) {
        int y = 92 + i * 108;
        lv_obj_t *card = ui_row(root, 18, y, 374, 98, g_panel, g_panel, chooser_card_cb,
                                (void *)(intptr_t)i);
        ui_box(card, 0, 12, 4, 74, cards[i].accent, 2);
        ui_text(card, cards[i].name, g_fg, &lv_font_montserrat_22, 22, 14, 0);
        ui_text(card, cards[i].line1, cards[i].accent, &lv_font_montserrat_12, 22, 48, 0);
        ui_text(card, cards[i].line2, g_dim, &lv_font_montserrat_12, 22, 68, 0);
        ui_text(card, "LAUNCH", cards[i].accent, &lv_font_montserrat_12, 292, 40, 0);
        if (g_cfg.active_os == (uint8_t)i)
            ui_text(card, "LAST", g_dim, &lv_font_montserrat_10, 300, 68, 0);
    }

    ui_btn(root, 18, 424, 176, 44, "TO LAUNCHER", g_panel, g_fg, 12,
           chooser_launcher_cb, NULL);
    ui_btn(root, 210, 424, 182, 44, "DIAGNOSTICS", g_panel, g_dim, 12,
           chooser_diag_cb, NULL);

    char foot[64];
    snprintf(foot, sizeof(foot), "HOLD BOOT %.1fs = LAUNCHEROS%s",
             (double)(DCS_BOOT_LONGPRESS_MS / 1000.0),
             g_sd_ok ? "" : "   /   NO SD");
    ui_text(root, foot, g_dim, &lv_font_montserrat_12, 20, 478, 370);

    ui_nav(root);
    power_apply();
    dcs_log(DCS_PATH_SHARED, "chooser shown (v%s)", DCS_VERSION);
}

static void os_enter(int idx) {
    if (idx < 0 || idx > 2) return;
    char err[64];
    if (!manifest_validate(idx, err, sizeof(err))) {
        dcs_log(kManifests[idx].dir, "launch refused: %s", err);
        lv_obj_t *c = modal_open("CANNOT LAUNCH");
        ui_text(c, kManifests[idx].name, g_fg, &lv_font_montserrat_22, 12, 8, 340);
        ui_text(c, err, g_danger, &lv_font_montserrat_16, 12, 46, 340);
        ui_text(c, "The chooser is still available. Fix or delete the manifest "
                   "in that OS folder, or keep using another experience.",
                g_dim, &lv_font_montserrat_14, 12, 80, 340);
        return;
    }
    g_exp_state = EXP_LOADING;
    g_cur_os = idx;
    g_cfg.active_os = (uint8_t)idx;
    sharedcfg_save();
    dcs_log(kManifests[idx].dir, "launch");
    g_exp_state = EXP_ACTIVE;
    if (idx == 1) g_crown_intro = true;
    power_apply();

    // Boot sequence: the OS's own two-second handover, skipped entirely when
    // the user turned it off, and short-formed for a deep-sleep resume.
    bool fast = g_boot_resume;
    g_boot_resume = false;
    if (g_cfg.bootanim == 0) {
        if (idx == 0) noir_rebuild();
        else if (idx == 1) crown_rebuild();
        else night_rebuild();
    } else {
        boot_begin(idx, fast || g_cfg.bootanim == 2);
    }
}

static void os_leave() {
    night_flash_off();
    night_stealth_stop();
    breathe_stop();
    if (g_cur_os == 0) noir_save();
    else if (g_cur_os == 1) crown_save();
    else if (g_cur_os == 2) night_save();
    timer_save();
    dcs_log(g_cur_os == 0 ? DCS_PATH_NOIR :
            g_cur_os == 1 ? DCS_PATH_CROWN :
            g_cur_os == 2 ? DCS_PATH_NIGHT : DCS_PATH_SHARED, "exit to chooser");
    g_cur_os = -1;
    show_chooser();
}

// ---------------------------------------------------------------------------
//  16.  NoirOS
// ---------------------------------------------------------------------------
static int g_noir_tab = 0;
static const char *kNoirTabs[4] = { "HOME", "INBOX", "TOOLS", "DAY" };

static void noir_tab_cb(lv_event_t *e) {
    g_noir_tab = (int)(intptr_t)lv_event_get_user_data(e);
    noir_rebuild();
}

static void noir_to_tab(int tab) {
    g_noir_tab = tab;
    noir_rebuild();
}

static void noir_music_cb(lv_event_t *e) {
    uint8_t cmd = (uint8_t)(intptr_t)lv_event_get_user_data(e);
    if (!phone_media_cmd(cmd))
        dcs_log(DCS_PATH_NOIR, "media command refused (phone offline)");
    noir_rebuild();
}
static void noir_timer_cb(lv_event_t *e) {
    int what = (int)(intptr_t)lv_event_get_user_data(e);
    if (what == 0)      { if (g_cd_run) cd_pause(); else cd_start(0); }
    else if (what == 1) cd_reset();
    else if (what == 2) { if (g_sw_run) sw_stop(); else sw_start(); }
    else if (what == 3) sw_reset();
    timer_save();
    noir_rebuild();
}
static void noir_focus_len_cb(lv_event_t *e) {
    int delta = (int)(intptr_t)lv_event_get_user_data(e);
    int m = (int)g_noir.focus_min + delta;
    if (m < 5) m = 5;
    if (m > 90) m = 90;
    g_noir.focus_min = (uint16_t)m;
    if (!g_cd_run) g_cd_base_ms = (uint32_t)g_noir.focus_min * 60000UL;
    noir_save();
    noir_rebuild();
}
static void noir_t24_cb(lv_event_t *e) {
    (void)e;
    g_cfg.t24 = g_cfg.t24 ? 0 : 1;
    sharedcfg_save();
    noir_rebuild();
}
static void noir_privacy_cb(lv_event_t *e) {
    (void)e;
    g_cfg.privacy = (uint8_t)((g_cfg.privacy + 1) % 3);
    sharedcfg_save();
    noir_rebuild();
}
static void noir_power_cb(lv_event_t *e) {
    (void)e;
    power_set((uint8_t)((g_cfg.power + 1) % 4));
    noir_rebuild();
}
static void noir_demo_cb(lv_event_t *e) {
    (void)e;
    static int n = 0;
    char body[80];
    snprintf(body, sizeof(body), "Simulated alert %d. Phone link not required.", ++n);
    notif_add("Demo", "Test", "Injected notification", body, NULL);
    noir_rebuild();
}
static void noir_city_add_cb(lv_event_t *e) {
    city_add((int)(intptr_t)lv_event_get_user_data(e));
    noir_rebuild();
}
static void noir_city_remove_cb(lv_event_t *e) {
    (void)e;
    city_remove_last();
    noir_rebuild();
}
static void noir_mark_read_cb(lv_event_t *e) {
    (void)e;
    notif_mark_all_read();
    noir_rebuild();
}
static void noir_clear_read_cb(lv_event_t *e) {
    (void)e;
    notif_clear_read();
    noir_rebuild();
}
static void noir_dismiss_cb(lv_event_t *e) {
    notif_dismiss((int)(intptr_t)lv_event_get_user_data(e));
    noir_rebuild();
}
static void noir_detail_dismiss_cb(lv_event_t *e) {
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    if (g_modal) { lv_obj_del_async(g_modal); g_modal = nullptr; }
    notif_dismiss(idx);
    noir_rebuild();
}
// Timer presets, so a focus block is two taps instead of a +/- ladder.
static void noir_preset_cb(lv_event_t *e) {
    int m = (int)(intptr_t)lv_event_get_user_data(e);
    if (m <= 0) return;
    cd_start((uint32_t)m * 60000UL);
    timer_save();
    dcs_log(DCS_PATH_NOIR, "focus preset %d min", m);
    noir_rebuild();
}

static void noir_notif_cb(lv_event_t *e) {
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    if (idx < 0 || idx >= g_nf_n) return;
    lv_obj_t *c = modal_open("NOTIFICATION");
    char t[64], b[128];
    notif_present(idx, t, sizeof(t), b, sizeof(b));
    Notif &n = g_nf[idx];
    ui_text(c, n.app, g_accent, &lv_font_montserrat_14, 12, 6, 340);
    ui_text(c, t, g_fg, &lv_font_montserrat_22, 12, 30, 340);
    ui_text(c, b, g_fg, &lv_font_montserrat_16, 12, 74, 340);
    char ts[24];
    ts_hhmm(n.ts, ts, sizeof(ts));
    ui_text(c, ts, g_dim, &lv_font_montserrat_12, 12, 140, 340);
    n.read = 1;
    notif_save();
    ui_btn(c, 12, 176, 170, 40, "DISMISS", g_panel, g_danger, 10,
           noir_detail_dismiss_cb, (void *)(intptr_t)idx);
}

static void noir_build_home(lv_obj_t *b) {
    noir_wallpaper(b);       // optional /WATCH/PHOTOS frame, drawn behind everything
    ui_body_scroll(b);
    if (g_noir.clock_style == 1) {
        ui_dial(b, 205, 128, 104, g_accent, g_fg, true, true);
        g_date_lbl = ui_text(b, "", g_dim, &lv_font_montserrat_14, 130, 238, 0);
        lv_obj_set_style_text_align(g_date_lbl, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_width(g_date_lbl, 150);
        g_clock_lbl = nullptr;
    } else {
        g_clock_lbl = ui_text(b, "", g_fg, &lv_font_montserrat_48, 22, 4, 0);
        g_date_lbl = ui_text(b, "", g_dim, &lv_font_montserrat_16, 24, 74, 0);
    }

    int y = (g_noir.clock_style == 1) ? 266 : 108;
    char line[64], bs[12], wx[28];
    batt_fmt(bs, sizeof(bs));
    snprintf(line, sizeof(line), "%s  %s", bs, g_batt_ch ? "CHARGING" : "BATTERY");
    ui_text(b, line, g_accent, &lv_font_montserrat_14, 24, y, 0);
    if (g_wx.valid)
        snprintf(wx, sizeof(wx), "%dC %s", (int)g_wx.temp, g_wx.cond);
    else
        snprintf(wx, sizeof(wx), "NO WEATHER");
    ui_text(b, wx, g_dim, &lv_font_montserrat_14, 204, y, 0);

    y += 30;
    lv_obj_t *ev = ui_frame(b, 18, y, 374, 88, g_panel, 10, g_panel, 1);
    ui_text(ev, "NEXT EVENTS", g_dim, &lv_font_montserrat_12, 12, 6, 0);
    bool any_ev = false;
    for (int i = 0; i < 3; i++) {
        char ti[24];
        int64_t left = 0;
        if (!event_nth(i, ti, sizeof(ti), &left)) break;
        any_ev = true;
        ui_text(ev, ti, g_fg, &lv_font_montserrat_14, 12, 24 + i * 20, 226);
        char d[24];
        clock_fmt_dur(left, d, sizeof(d));
        ui_text(ev, d, g_accent, &lv_font_montserrat_12, 250, 26 + i * 20, 0);
    }
    if (!any_ev)
        ui_text(ev, "Nothing scheduled", g_dim, &lv_font_montserrat_14, 12, 30, 300);

    y += 102;
    static const char *tile_name[4] = { "INBOX", "FOCUS", "WORLD", "MUSIC" };
    for (int i = 0; i < 4; i++) {
        int tx = 18 + (i % 2) * 192;
        int ty = y + (i / 2) * 84;
        lv_obj_t *tile = ui_row(b, tx, ty, 182, 76, g_panel, g_panel,
                                noir_tile_cb, (void *)(intptr_t)i);
        ui_text(tile, tile_name[i], g_fg, &lv_font_montserrat_18, 14, 12, 0);
        if (i == 0) {
            char u[16];
            snprintf(u, sizeof(u), "%d UNREAD", notif_unread());
            ui_text(tile, u, notif_unread() ? g_accent : g_dim,
                    &lv_font_montserrat_12, 14, 42, 0);
        } else if (i == 1) {
            char u[16];
            snprintf(u, sizeof(u), "%u MIN", (unsigned)g_noir.focus_min);
            ui_text(tile, u, g_dim, &lv_font_montserrat_12, 14, 42, 0);
        } else if (i == 2) {
            char u[16];
            snprintf(u, sizeof(u), "%d CITY", g_city_n);
            ui_text(tile, u, g_dim, &lv_font_montserrat_12, 14, 42, 0);
        } else {
            ui_text(tile, g_media.connected ? (g_media.playing ? "PLAYING" : "PAUSED")
                                            : "OFFLINE",
                    g_dim, &lv_font_montserrat_12, 14, 42, 0);
        }
    }
    ui_pad(b, y + 180);
}

// Tile taps are declared before use by the Arduino preprocessor; the
// prototype block at the end of the file covers the rest.
static void noir_tile_cb(lv_event_t *e) {
    int which = (int)(intptr_t)lv_event_get_user_data(e);
    if (which == 0) noir_to_tab(1);
    else noir_to_tab(2);
}

static void noir_build_inbox(lv_obj_t *b) {
    lv_obj_t *hdr = ui_box(b, 0, 0, 410, 44, g_bg, 0);
    char u[40];
    snprintf(u, sizeof(u), "%d UNREAD / %d%s", notif_unread(), g_nf_n,
             g_nf_overflow ? "  OVERFLOW" : "");
    ui_text(hdr, u, g_dim, &lv_font_montserrat_12, 14, 16, 0);
    ui_btn(hdr, 190, 8, 104, 28, "READ ALL", g_panel,
           notif_unread() ? g_accent : g_dim, 12, noir_mark_read_cb, NULL);
    ui_btn(hdr, 300, 8, 100, 28, "CLEAR READ", g_panel, g_dim, 12,
           noir_clear_read_cb, NULL);

    if (g_nf_n == 0) {
        ui_text(b, "INBOX EMPTY", g_fg, &lv_font_montserrat_22, 24, 90, 0);
        ui_text(b, "Notifications arrive from the phone link and stay here "
                   "across experience switches.", g_dim, &lv_font_montserrat_14,
                24, 130, 330);
        ui_btn(b, 24, 216, 240, 46, "SIMULATE ALERT", g_panel, g_accent, 12,
               noir_demo_cb, NULL);
        ui_text(b, "No phone required for the demo path.", g_dim,
                &lv_font_montserrat_12, 24, 272, 300);
        return;
    }

    ui_body_scroll(b);
    int y = 52;
    for (int i = 0; i < g_nf_n; i++) {
        Notif &n = g_nf[i];
        char t[64], body[128], when[12];
        notif_present(i, t, sizeof(t), body, sizeof(body));
        ts_hhmm(n.ts, when, sizeof(when));
        lv_obj_t *row = ui_row(b, 12, y, 386, 68, g_panel,
                               n.read ? g_panel : g_accent, noir_notif_cb,
                               (void *)(intptr_t)i);
        if (!n.read) ui_box(row, 10, 30, 6, 6, g_accent, 3);
        ui_text(row, t, n.read ? g_dim : g_fg, &lv_font_montserrat_16, 24, 10, 296);
        lv_obj_t *bl = ui_text(row, body, g_dim, &lv_font_montserrat_12, 24, 38, 292);
        lv_label_set_long_mode(bl, LV_LABEL_LONG_DOT);
        ui_text(row, when, g_dim, &lv_font_montserrat_12, 330, 12, 0);
        ui_text(row, n.app, g_dim, &lv_font_montserrat_12, 24, 54, 300);
        ui_btn(row, 336, 40, 34, 22, "X", g_bg, g_dim, 6, noir_dismiss_cb,
               (void *)(intptr_t)i);
        y += 74;
    }
    ui_text(b, "X dismisses one alert; opening it marks it read.", g_dim,
            &lv_font_montserrat_10, 16, y + 4, 340);
    ui_pad(b, y + 28);
}

static void noir_build_tools(lv_obj_t *b) {
    ui_body_scroll(b);
    int y = 10;

    // ---- music (phone media, not local audio) ----
    lv_obj_t *m = ui_frame(b, 12, y, 386, 132, g_panel, 12, g_panel, 1);
    ui_text(m, "PHONE MEDIA", g_dim, &lv_font_montserrat_12, 16, 10, 0);
    ui_text(m, g_media.connected ? (g_media.playing ? "PLAYING" : "PAUSED") : "OFFLINE",
            g_media.connected ? g_accent : g_warn, &lv_font_montserrat_12, 288, 10, 0);
    ui_text(m, g_media.title, g_fg, &lv_font_montserrat_18, 16, 34, 340);
    ui_text(m, g_media.artist, g_dim, &lv_font_montserrat_12, 16, 60, 340);
    ui_btn(m, 16, 88, 60, 34, "PREV", g_bg, g_fg, 10, noir_music_cb, (void *)(intptr_t)MEDIA_PREV);
    ui_btn(m, 86, 88, 216, 34, g_media.playing ? "PAUSE" : "PLAY", g_bg, g_accent, 10,
           noir_music_cb, (void *)(intptr_t)MEDIA_TOGGLE);
    ui_btn(m, 312, 88, 60, 34, "NEXT", g_bg, g_fg, 10, noir_music_cb, (void *)(intptr_t)MEDIA_NEXT);
    y += 144;

    // ---- weather (phone-supplied; never invented on the watch) ----
    lv_obj_t *wxcard = ui_frame(b, 12, y, 386, 72, g_panel, 12, g_panel, 1);
    ui_text(wxcard, "WEATHER", g_dim, &lv_font_montserrat_12, 16, 8, 0);
    ui_text(wxcard, "from phone", g_dim, &lv_font_montserrat_10, 286, 9, 0);
    if (g_wx.valid) {
        char t[10], hilo[24];
        snprintf(t, sizeof(t), "%dC", (int)g_wx.temp);
        snprintf(hilo, sizeof(hilo), "H%d  L%d", (int)g_wx.hi, (int)g_wx.lo);
        ui_text(wxcard, t, g_fg, &lv_font_montserrat_28, 16, 28, 0);
        ui_text(wxcard, g_wx.cond, g_accent, &lv_font_montserrat_14, 92, 38, 0);
        ui_text(wxcard, hilo, g_dim, &lv_font_montserrat_12, 270, 38, 0);
    } else {
        ui_text(wxcard, "No weather data yet.", g_dim, &lv_font_montserrat_14, 16, 32, 300);
    }
    y += 84;

    // ---- focus timer + stopwatch ----
    lv_obj_t *f = ui_frame(b, 12, y, 386, 214, g_panel, 12, g_panel, 1);
    ui_text(f, "FOCUS", g_dim, &lv_font_montserrat_12, 16, 10, 0);
    g_timer_lbl = ui_text(f, "25:00", g_fg, &lv_font_montserrat_48, 16, 28, 0);
    char ses[40];
    snprintf(ses, sizeof(ses), "%u min   /   %u sessions", (unsigned)g_noir.focus_min,
             (unsigned)g_cd_sessions);
    ui_text(f, ses, g_dim, &lv_font_montserrat_12, 180, 40, 0);
    ui_btn(f, 16, 96, 78, 32, "-5", g_bg, g_fg, 9, noir_focus_len_cb, (void *)(intptr_t)-5);
    ui_btn(f, 100, 96, 78, 32, "+5", g_bg, g_fg, 9, noir_focus_len_cb, (void *)(intptr_t)5);
    ui_btn(f, 192, 96, 82, 32, g_cd_run ? "PAUSE" : "START", g_bg, g_accent, 9,
           noir_timer_cb, (void *)(intptr_t)0);
    ui_btn(f, 282, 96, 88, 32, "RESET", g_bg, g_dim, 9, noir_timer_cb, (void *)(intptr_t)1);
    static const int preset_min[4] = { 5, 15, 25, 45 };
    for (int i = 0; i < 4; i++) {
        char pl[12];
        snprintf(pl, sizeof(pl), "%d MIN", preset_min[i]);
        ui_btn(f, 16 + i * 92, 136, 84, 28, pl, g_bg, g_dim, 8, noir_preset_cb,
               (void *)(intptr_t)preset_min[i]);
    }
    ui_text(f, "STOPWATCH", g_dim, &lv_font_montserrat_12, 16, 184, 0);
    g_sw_lbl = ui_text(f, "00:00", g_fg, &lv_font_montserrat_16, 100, 182, 0);
    ui_btn(f, 192, 180, 82, 26, g_sw_run ? "STOP" : "GO", g_bg, g_fg, 8,
           noir_timer_cb, (void *)(intptr_t)2);
    ui_btn(f, 282, 180, 88, 26, "RESET", g_bg, g_dim, 8, noir_timer_cb, (void *)(intptr_t)3);
    y += 226;

    // ---- world clock ----
    lv_obj_t *w = ui_frame(b, 12, y, 386, 60 + g_city_n * 30, g_panel, 12, g_panel, 1);
    ui_text(w, "WORLD CLOCK", g_dim, &lv_font_montserrat_12, 16, 8, 0);
    ui_text(w, "fixed UTC offset", g_dim, &lv_font_montserrat_10, 250, 9, 0);
    for (int i = 0; i < g_city_n; i++) {
        ui_text(w, g_city[i].name, g_fg, &lv_font_montserrat_14, 16, 32 + i * 28, 0);
        g_city_lbl[i] = ui_text(w, "--:--", g_accent, &lv_font_montserrat_14, 250, 32 + i * 28, 0);
    }
    int by = 32 + g_city_n * 30;
    for (int i = 0; i < 4 && i < kCityPickN; i++)
        ui_btn(w, 16 + i * 58, by, 52, 26, kCityPick[i].name, g_bg, g_dim, 8,
               noir_city_add_cb, (void *)(intptr_t)i);
    ui_btn(w, 310, by, 60, 26, "DEL", g_bg, g_dim, 8, noir_city_remove_cb, NULL);
    y += 60 + g_city_n * 30 + 42;

    // ---- quick settings ----
    lv_obj_t *q = ui_frame(b, 12, y, 386, 238, g_panel, 12, g_panel, 1);
    ui_text(q, "QUICK SETTINGS", g_dim, &lv_font_montserrat_12, 16, 8, 0);
    char v[48], an[12];
    snprintf(v, sizeof(v), "CLOCK      %s", g_cfg.t24 ? "24 HOUR" : "12 HOUR");
    ui_btn(q, 16, 30, 354, 34, v, g_bg, g_fg, 9, noir_t24_cb, NULL);
    snprintf(v, sizeof(v), "PRIVACY    %s", kPrivacyName[g_cfg.privacy]);
    ui_btn(q, 16, 70, 354, 34, v, g_bg, g_fg, 9, noir_privacy_cb, NULL);
    snprintf(v, sizeof(v), "POWER      %s", kPwrName[g_cfg.power]);
    ui_btn(q, 16, 110, 354, 34, v, g_bg, g_fg, 9, noir_power_cb, NULL);
    if (alarm_next_str(an, sizeof(an)))
        snprintf(v, sizeof(v), "ALARMS     NEXT %s", an);
    else
        snprintf(v, sizeof(v), "ALARMS     NONE SET");
    ui_btn(q, 16, 150, 354, 34, v, g_bg, g_accent, 9, quick_alarm_cb, NULL);
    ui_btn(q, 16, 190, 354, 34, "SIMULATE NOTIFICATION", g_bg, g_accent, 9, noir_demo_cb, NULL);
    y += 250;
    ui_pad(b, y);
    (void)m;
}

// ---- DAY: the numbers, not another menu --------------------------------
//  Everything on this tab is measured: screen-on time and notifications come
//  from the shared counters, focus time and water from NoirOS's own file.
//  Nothing here is estimated from appearance.
static void noir_water_cb(lv_event_t *e) {
    water_add((int)(intptr_t)lv_event_get_user_data(e));
    noir_rebuild();
}

static void noir_day_diag_cb(lv_event_t *e) {
    (void)e;
    show_diagnostics();
}

static void noir_build_day(lv_obj_t *b) {
    ui_body_scroll(b);
    int y = 10;

    char d[40];
    clock_fmt_date(d, sizeof(d));
    ui_text(b, "TODAY", g_dim, &lv_font_montserrat_12, 16, y, 0);
    ui_text(b, d, g_fg, &lv_font_montserrat_16, 16, y + 18, 300);
    y += 52;

    // ---- water ----
    lv_obj_t *w = ui_frame(b, 12, y, 386, 146, g_panel, 12, g_panel, 1);
    ui_text(w, "WATER", g_dim, &lv_font_montserrat_12, 16, 8, 0);
    char v[32];
    snprintf(v, sizeof(v), "%u / %u", (unsigned)g_water, WATER_MAX);
    ui_text(w, v, g_water ? g_accent : g_dim, &lv_font_montserrat_14, 296, 7, 0);
    for (int i = 0; i < WATER_MAX; i++) {
        lv_obj_t *dot = ui_box(w, 16 + i * 22, 32, 14, 14,
                               i < (int)g_water ? g_accent : g_panel, LV_RADIUS_CIRCLE);
        if (i >= (int)g_water) {
            lv_obj_set_style_border_width(dot, 1, 0);
            lv_obj_set_style_border_color(dot, lv_color_hex(g_dim), 0);
        }
    }
    ui_btn(w, 16, 58, 96, 34, "- 1", g_bg, g_dim, 9, noir_water_cb, (void *)(intptr_t)-1);
    ui_btn(w, 122, 58, 134, 34, "+ 1 GLASS", g_bg, g_accent, 9, noir_water_cb, (void *)(intptr_t)1);
    ui_btn(w, 266, 58, 104, 34, "RESET", g_bg, g_dim, 9, noir_water_cb, (void *)(intptr_t)-99);
    ui_text(w, "Counted per day; resets at local midnight.", g_dim,
            &lv_font_montserrat_10, 16, 104, 340);
    y += 158;

    // ---- measured stats ----
    lv_obj_t *s = ui_frame(b, 12, y, 386, 186, g_panel, 12, g_panel, 1);
    ui_text(s, "TODAY IN NUMBERS", g_dim, &lv_font_montserrat_12, 16, 8, 0);
    const char *labels[4] = { "SCREEN ON", "FOCUS", "NOTIFICATIONS", "BATTERY" };
    char vals[4][28];
    char dur[24];
    clock_fmt_dur((int64_t)(g_stat_on_ms / 1000UL), dur, sizeof(dur));
    snprintf(vals[0], sizeof(vals[0]), "%s", dur);
    snprintf(vals[1], sizeof(vals[1]), "%lu min",
             (unsigned long)(g_stat_focus_ms / 60000UL));
    snprintf(vals[2], sizeof(vals[2]), "%u", (unsigned)g_stat_notif);
    if (g_batt_boot_ok)
        snprintf(vals[3], sizeof(vals[3]), "%d%%  from %u%%", g_batt_pct, (unsigned)g_batt_boot);
    else
        snprintf(vals[3], sizeof(vals[3]), "%d%%", g_batt_pct);
    for (int i = 0; i < 4; i++) {
        ui_text(s, labels[i], g_dim, &lv_font_montserrat_14, 16, 34 + i * 34, 0);
        lv_obj_t *vl = ui_text(s, vals[i], g_fg, &lv_font_montserrat_16, 190, 32 + i * 34, 170);
        lv_obj_set_style_text_align(vl, LV_TEXT_ALIGN_RIGHT, 0);
    }
    y += 198;

    ui_btn(b, 12, y, 190, 42, "DIAGNOSTICS", g_panel, g_fg, 10, noir_day_diag_cb, NULL);
    ui_btn(b, 208, y, 190, 42, "FOCUS TOOLS", g_panel, g_accent, 10, noir_tab_cb,
           (void *)(intptr_t)2);
    y += 54;
    ui_text(b, "Screen-on time, focus minutes and water persist per day. The counters "
               "roll over at midnight and the previous day's file is kept as written.",
            g_dim, &lv_font_montserrat_10, 16, y, 380);
    y += 40;
    ui_pad(b, y);
}

static void noir_rebuild() {
    if (g_cur_os != 0) return;
    lv_obj_t *b = ui_begin(0, "NoirOS", "EVERYDAY / FOCUS", kNoirTabs, 4,
                           g_noir_tab, noir_tab_cb, ui_exit_cb);
    lv_obj_t *root = lv_obj_get_parent(b);
    if (g_noir_tab == 0)      noir_build_home(b);
    else if (g_noir_tab == 1) noir_build_inbox(b);
    else if (g_noir_tab == 2) noir_build_tools(b);
    else                      noir_build_day(b);
    ui_nav(root);
}
// ---------------------------------------------------------------------------
//  17.  CrownOS  --  watch-face engine, Watch Studio, occasions, events
// ---------------------------------------------------------------------------
//  A face is data, not code: style + accent + background + complications.
//  Rendering is shared, so a missing asset or unknown style falls back to a
//  safe classic dial instead of crashing.
#define FACE_MAX 16
#define FACE_STYLES 11
struct FaceDef {
    char name[16];
    uint8_t style;     // 0 classic, 1 minimal digital, 2 dual time
    uint8_t accent;    // index into kAccentCol
    uint8_t bg;        // index into kBgCol
    uint8_t compl_on;  // 1 = complications visible
    uint8_t user;      // 1 = saved by Watch Studio
};
static FaceDef g_faces[FACE_MAX];
static int g_face_n = 0;

static const char *kAccentName[5] = { "GOLD", "SILVER", "IVORY", "MIDNIGHT", "CRIMSON" };
static const uint32_t kAccentCol[5] = { 0xC9A44C, 0xC8CCD4, 0xEFE6D2, 0x3E5C8A, 0xB4453F };
static const uint32_t kBgCol[4] = { 0x0B0A07, 0x0A0F18, 0x14110D, 0x120C16 };
// Every style is drawn from primitives -- there are no bitmap dials except the
// user's own PHOTO dial, so a missing asset degrades to a drawn face instead of
// an empty screen.
static const char *kStyleName[FACE_STYLES] = {
    "CLASSIC", "MINIMAL", "DUAL TIME", "ROMAN", "MOON", "SOVEREIGN",
    "SERPENT", "CHRONO", "SKELETON", "DIVE", "PHOTO"
};

static const char *face_style_name(uint8_t s) { return s < FACE_STYLES ? kStyleName[s] : "CLASSIC"; }

// ---- moon phase -----------------------------------------------------------
//  Pure arithmetic from a known new moon: no asset, no network, no animation.
//  Illumination is approximate (circular terminator), which is exactly what a
//  watch complication needs; the face never claims an ephemeris.
static double moon_cycle_frac() {
    const double synodic = 29.530588853;
    double days = ((double)time(NULL) - 947182440.0) / 86400.0;
    double p = fmod(days, synodic);
    if (p < 0) p += synodic;
    return p / synodic;
}
static const char *moon_name(double p) {
    static const char *n[8] = { "NEW", "WAX CRESCENT", "FIRST QTR", "WAX GIBBOUS",
                                "FULL", "WAN GIBBOUS", "LAST QTR", "WAN CRESCENT" };
    int i = (int)(p * 8.0 + 0.5) % 8;
    return n[i];
}
// Lit disc with a clipped shadow disc: off = 0 is new, 2r is full.
static void moon_draw(lv_obj_t *p, int cx, int cy, int r, uint32_t lit, uint32_t dark) {
    lv_obj_t *base = ui_box(p, cx - r, cy - r, r * 2, r * 2, lit, LV_RADIUS_CIRCLE);
    lv_obj_set_style_clip_corner(base, true, 0);
    double t = moon_cycle_frac();
    bool waxing = t < 0.5;
    if (!waxing) t = 1.0 - t;             // 0 new .. 0.5 full
    int off = (int)(4.0 * (double)r * t);
    if (off <= 0) off = 1;                // never a zero-width child
    ui_box(base, waxing ? -off : off, 0, r * 2, r * 2, dark, LV_RADIUS_CIRCLE);
}
static void ui_roman_numerals(lv_obj_t *p, int cx, int cy, int r, uint32_t fg) {
    static const char *rn[12] = { "XII", "I", "II", "III", "IV", "V",
                                  "VI", "VII", "VIII", "IX", "X", "XI" };
    for (int i = 0; i < 12; i++) {
        float a = (float)i * 30.0f * 3.14159265f / 180.0f;
        int tx = cx + (int)(sinf(a) * (float)(r - 30));
        int ty = cy - (int)(cosf(a) * (float)(r - 30));
        lv_obj_t *l = ui_text(p, rn[i], fg, &lv_font_montserrat_10, tx - 17, ty - 7, 34);
        lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    }
}

static void face_save_users() {
    char buf[800];
    size_t o = 0;
    for (int i = 0; i < g_face_n; i++) {
        if (!g_faces[i].user) continue;
        int w = snprintf(buf + o, sizeof(buf) - o, "%s|%u|%u|%u|%u\n", g_faces[i].name,
                         (unsigned)g_faces[i].style, (unsigned)g_faces[i].accent,
                         (unsigned)g_faces[i].bg, (unsigned)g_faces[i].compl_on);
        if (w < 0 || o + (size_t)w >= sizeof(buf)) break;
        o += (size_t)w;
    }
    fs_write_atomic("/WATCH/CrownOS/data/saved_faces.jsonl", buf, o);
}

static void face_load() {
    g_face_n = 0;
    const char *names[12] = { "LAQUEAR", "MERIDIAN", "MINUIT", "VOYAGE", "ROMAIN",
                              "LUNE", "SOVEREIGN", "SERPENT", "CHRONO", "SKELETON",
                              "DIVE", "PHOTO" };
    const uint8_t st[12] = { 0, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10 };
    const uint8_t ac[12] = { 0, 1, 2, 3, 0, 2, 0, 0, 1, 4, 3, 2 };
    const uint8_t bg[12] = { 0, 1, 0, 1, 2, 3, 0, 0, 2, 0, 0, 0 };
    for (int i = 0; i < 12; i++) {
        memset(&g_faces[g_face_n], 0, sizeof(FaceDef));
        copyf(g_faces[g_face_n].name, sizeof(g_faces[0].name), names[i]);
        g_faces[g_face_n].style = st[i];
        g_faces[g_face_n].accent = ac[i];
        g_faces[g_face_n].bg = bg[i];
        g_faces[g_face_n].compl_on = 1;
        g_face_n++;
    }
    static char buf[900];
    if (!fs_read_text("/WATCH/CrownOS/data/saved_faces.jsonl", buf, sizeof(buf))) return;
    char *save = NULL;
    for (char *line = strtok_r(buf, "\n", &save); line && g_face_n < FACE_MAX;
         line = strtok_r(NULL, "\n", &save)) {
        int v[4] = { 0, 0, 0, 1 };
        char name[16] = "";
        if (sscanf(line, "%15[^|]|%d|%d|%d|%d", name, &v[0], &v[1], &v[2], &v[3]) < 2) continue;
        if (!name[0]) continue;
        FaceDef &f = g_faces[g_face_n];
        memset(&f, 0, sizeof(f));
        copyf(f.name, sizeof(f.name), name);
        f.style = (uint8_t)(v[0] % FACE_STYLES);
        f.accent = (uint8_t)(v[1] % 5);
        f.bg = (uint8_t)(v[2] % 4);
        f.compl_on = v[3] ? 1 : 0;
        f.user = 1;
        g_face_n++;
    }
}

static void face_clamp() {
    if (g_face_n <= 0) { face_load(); }
    if (g_crown.face >= g_face_n) g_crown.face = 0;
    if (g_crown.accent > 4) g_crown.accent = 0;
    if (g_crown.bg > 3) g_crown.bg = 0;
}

// ---- occasion profiles ----------------------------------------------------
struct OccasionDef {
    const char *name;
    uint8_t face, accent, bg, power, privacy;
};
static const OccasionDef kOcc[4] = {
    { "FORMAL",      0, 0, 0, PWR_ECO,      1 },
    { "CELEBRATION", 1, 4, 2, PWR_BALANCED, 0 },
    { "EVERYDAY",    2, 1, 1, PWR_BALANCED, 0 },
    { "TRAVEL",      3, 3, 1, PWR_ECO,      1 },
};

static int g_crown_tab = 0;
static const char *kCrownTabs[4] = { "FACE", "STUDIO", "EVENTS", "ALMANAC" };
static bool g_crown_chrome = false;          // header + tab bar over the dial
static lv_obj_t *g_crown_face_host = nullptr;   // the full-bleed dial canvas

static void crown_tab_cb(lv_event_t *e) {
    g_crown_tab = (int)(intptr_t)lv_event_get_user_data(e);
    if (g_crown_tab != 0) g_crown_chrome = false;
    crown_rebuild();
}

// Short, interruptible signature reveal.  Reduced-motion and Eco/Survival
// simply skip it -- the face is already usable underneath.
static void crown_reveal() {
    if (!g_crown.anim || g_cfg.power >= PWR_ECO) return;
    lv_obj_t *o = lv_obj_create(lv_layer_top());
    lv_obj_set_pos(o, 0, 0);
    lv_obj_set_size(o, 410, 502);
    lv_obj_set_style_bg_color(o, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(o, 0, 0);
    lv_obj_set_style_pad_all(o, 0, 0);
    lv_obj_clear_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(o, LV_OBJ_FLAG_CLICKABLE);   // never eat the first tap
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, o);
    lv_anim_set_exec_cb(&a, [](void *v, int32_t x) {
        lv_obj_set_style_bg_opa((lv_obj_t *)v, (lv_opa_t)x, 0);
    });
    lv_anim_set_values(&a, 255, 0);
    lv_anim_set_time(&a, 220);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_set_ready_cb(&a, [](lv_anim_t *an) { lv_obj_del((lv_obj_t *)an->var); });
    lv_anim_start(&a);
}

static void crown_apply_face(int idx) {
    if (idx < 0 || idx >= g_face_n) return;
    g_crown.face = (uint8_t)idx;
    g_crown.accent = g_faces[idx].accent;
    g_crown.bg = g_faces[idx].bg;
    crown_save();
    dcs_log(DCS_PATH_CROWN, "face applied: %s", g_faces[idx].name);
    crown_rebuild();
}

static void crown_face_row_cb(lv_event_t *e) {
    crown_apply_face((int)(intptr_t)lv_event_get_user_data(e));
}

static void crown_accent_cb(lv_event_t *e) {
    g_crown.accent = (uint8_t)(intptr_t)lv_event_get_user_data(e);
    crown_save();
    crown_rebuild();
}
static void crown_bg_cb(lv_event_t *e) {
    g_crown.bg = (uint8_t)(intptr_t)lv_event_get_user_data(e);
    crown_save();
    crown_rebuild();
}
static void crown_compl_cb(lv_event_t *e) {
    (void)e;
    g_crown.compl_on = g_crown.compl_on ? 0 : 1;
    crown_save();
    crown_rebuild();
}
static void crown_anim_cb(lv_event_t *e) {
    (void)e;
    g_crown.anim = g_crown.anim ? 0 : 1;
    crown_save();
    crown_rebuild();
}
static void crown_secs_cb(lv_event_t *e) {
    (void)e;
    g_crown.secs = g_crown.secs ? 0 : 1;
    crown_save();
    crown_rebuild();
}
static void crown_alerts_cb(lv_event_t *e) {
    (void)e;
    g_crown.alerts = g_crown.alerts ? 0 : 1;
    crown_save();
    crown_rebuild();
}

static void crown_save_new_cb(lv_event_t *e) {
    (void)e;
    if (g_face_n >= FACE_MAX) return;
    int users = 0;
    for (int i = 0; i < g_face_n; i++) if (g_faces[i].user) users++;
    FaceDef &f = g_faces[g_face_n];
    memset(&f, 0, sizeof(f));
    snprintf(f.name, sizeof(f.name), "CUSTOM %d", users + 1);
    f.style = g_faces[g_crown.face].style;
    f.accent = g_crown.accent;
    f.bg = g_crown.bg;
    f.compl_on = g_crown.compl_on;
    f.user = 1;
    g_crown.face = (uint8_t)g_face_n;
    g_face_n++;
    face_save_users();
    crown_save();
    dcs_log(DCS_PATH_CROWN, "face saved");
    crown_rebuild();
}

static void crown_overwrite_cb(lv_event_t *e) {
    (void)e;
    FaceDef &f = g_faces[g_crown.face];
    if (!f.user) return;             // presets are never overwritten
    f.accent = g_crown.accent;
    f.bg = g_crown.bg;
    f.compl_on = g_crown.compl_on;
    face_save_users();
    crown_save();
    crown_rebuild();
}

static void crown_occasion_cb(lv_event_t *e) {
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    if (i < 0 || i > 3) return;
    g_crown.profile = (uint8_t)i;
    g_crown.face = kOcc[i].face;
    g_crown.accent = kOcc[i].accent;
    g_crown.bg = kOcc[i].bg;
    power_set(kOcc[i].power);
    crown_save();
    dcs_log(DCS_PATH_CROWN, "occasion -> %s", kOcc[i].name);
    crown_rebuild();
}

static void crown_ev_add_cb(lv_event_t *e) {
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    static const struct { const char *title; int64_t delta; } pre[4] = {
        { "DINNER",  2 * 3600LL },
        { "MEETING", 86400LL },
        { "FLIGHT",  3 * 86400LL },
        { "PARTY",   7 * 86400LL },
    };
    if (i < 0 || i > 3) return;
    event_add(pre[i].title, pre[i].delta);
    crown_rebuild();
}
static void crown_ev_clear_cb(lv_event_t *e) {
    (void)e;
    g_ev_n = 0;
    event_save();
    crown_rebuild();
}
static void crown_ev_del_cb(lv_event_t *e) {
    event_remove((int)(intptr_t)lv_event_get_user_data(e));
    crown_rebuild();
}

static void crown_face_long_cb(lv_event_t *e) {
    (void)e;
    g_crown_tab = 1;
    crown_rebuild();
}

// ---- face rendering -------------------------------------------------------
static void ui_ring(lv_obj_t *p, int cx, int cy, int r, uint32_t col, int w) {
    lv_obj_t *o = lv_obj_create(p);
    lv_obj_set_pos(o, cx - r, cy - r);
    lv_obj_set_size(o, r * 2, r * 2);
    lv_obj_set_style_radius(o, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(o, w, 0);
    lv_obj_set_style_border_color(o, lv_color_hex(col), 0);
    lv_obj_clear_flag(o, LV_OBJ_FLAG_SCROLLABLE);
}

// ---------------------------------------------------------------------------
//  17.5  Full-bleed watch face
// ---------------------------------------------------------------------------
//  The FACE page is the whole watch: no header, no tab bar, no card.  Tap
//  brings the chrome back on top of the dial; a horizontal swipe pages to
//  Studio / Events / Almanac and back.  The dial is drawn edge to edge at
//  410 x 502, so what the user sees is a watch, not a dashboard.
//
//  Every style is a *reading* of a watchmaking idea -- fluted gold, a
//  moonwatch, a skeletonised movement, a dive bezel -- drawn with DualCore's
//  own marks and names.  Nothing here reproduces a brand's artwork or logo;
//  the only bitmap dial is the one the user supplies themselves.
static const int kFCx = 205, kFCy = 250;   // dial centre
static const int kFCR = 190;               // case outer radius
static const int kFCD = 174;               // dial aperture radius

// Applied markers.  A real watch turns each marker to face the centre, so
// these are lines along the radius, not upright bars.
static void fc_markers(lv_obj_t *p, int cx, int cy, int r, uint32_t col,
                       int len, int thick, bool quarters_only) {
    static lv_point_t pts[12][2];
    for (int i = 0; i < 12; i++) {
        if (quarters_only && (i % 3)) continue;
        float a = (float)i * 30.0f * 3.14159265f / 180.0f;
        int L = (i % 3 == 0) ? len : (len * 2) / 3;
        int T = (i % 3 == 0) ? thick : (thick > 2 ? thick - 1 : 2);
        pts[i][0].x = (lv_coord_t)(cx + sinf(a) * (float)(r - L));
        pts[i][0].y = (lv_coord_t)(cy - cosf(a) * (float)(r - L));
        pts[i][1].x = (lv_coord_t)(cx + sinf(a) * (float)r);
        pts[i][1].y = (lv_coord_t)(cy - cosf(a) * (float)r);
        lv_obj_t *l = lv_line_create(p);
        lv_obj_set_style_line_width(l, T, 0);
        lv_obj_set_style_line_color(l, lv_color_hex(col), 0);
        lv_obj_set_style_line_rounded(l, true, 0);
        lv_line_set_points(l, pts[i], 2);
    }
}

// 60-second track: hairlines, longer every fifth.
static void fc_minutes(lv_obj_t *p, int cx, int cy, int r, uint32_t col) {
    static lv_point_t pts[60][2];
    for (int i = 0; i < 60; i++) {
        float a = (float)i * 6.0f * 3.14159265f / 180.0f;
        int L = (i % 5 == 0) ? 11 : 5;
        pts[i][0].x = (lv_coord_t)(cx + sinf(a) * (float)(r - L));
        pts[i][0].y = (lv_coord_t)(cy - cosf(a) * (float)(r - L));
        pts[i][1].x = (lv_coord_t)(cx + sinf(a) * (float)r);
        pts[i][1].y = (lv_coord_t)(cy - cosf(a) * (float)r);
        lv_obj_t *l = lv_line_create(p);
        lv_obj_set_style_line_width(l, 1, 0);
        lv_obj_set_style_line_color(l, lv_color_hex(col), 0);
        lv_line_set_points(l, pts[i], 2);
    }
}

// Tachymeter scale: units per hour against the seconds hand.  The numbers are
// the scale itself, not a claim that this watch measures anything.
static void fc_tachy(lv_obj_t *p, int cx, int cy, int r, uint32_t col) {
    static const char *v[12] = { "400", "300", "240", "200", "160", "140",
                                 "120", "100", "80", "70", "65", "60" };
    for (int i = 0; i < 12; i++) {
        float a = (float)i * 30.0f * 3.14159265f / 180.0f;
        int tx = cx + (int)(sinf(a) * (float)r);
        int ty = cy - (int)(cosf(a) * (float)r);
        lv_obj_t *l = ui_text(p, v[i], col, &lv_font_montserrat_10, tx - 14, ty - 6, 28);
        lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    }
}

// A small dial-in-dial: ring, twelve marks, a caption and a value.
static void fc_subdial(lv_obj_t *p, int cx, int cy, int r, uint32_t ring,
                       uint32_t fg, const char *cap, const char *val) {
    ui_ring(p, cx, cy, r, ring, 2);
    for (int i = 0; i < 12; i++) {
        float a = (float)i * 30.0f * 3.14159265f / 180.0f;
        int tx = cx + (int)(sinf(a) * (float)(r - 7));
        int ty = cy - (int)(cosf(a) * (float)(r - 7));
        ui_box(p, tx - 1, ty - 1, (i % 3 == 0) ? 3 : 2, (i % 3 == 0) ? 3 : 2, fg, 2);
    }
    lv_obj_t *c = ui_text(p, cap, fg, &lv_font_montserrat_10, cx - r, cy - 5, r * 2);
    lv_obj_set_style_text_align(c, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_t *v2 = ui_text(p, val, fg, &lv_font_montserrat_12, cx - r, cy - 22, r * 2);
    lv_obj_set_style_text_align(v2, LV_TEXT_ALIGN_CENTER, 0);
}

// Guilloche: concentric hairlines plus a sunburst.  Drawn once, never animated.
static void fc_guilloche(lv_obj_t *p, int cx, int cy, int r, uint32_t col) {
    for (int k = 1; k <= 4; k++) ui_ring(p, cx, cy, r * k / 5, col, 1);
    static lv_point_t ray[72][2];
    for (int i = 0; i < 72; i++) {
        float a = (float)i * 5.0f * 3.14159265f / 180.0f;
        ray[i][0].x = (lv_coord_t)(cx + sinf(a) * 26.0f);
        ray[i][0].y = (lv_coord_t)(cy - cosf(a) * 26.0f);
        ray[i][1].x = (lv_coord_t)(cx + sinf(a) * (float)(r - 10));
        ray[i][1].y = (lv_coord_t)(cy - cosf(a) * (float)(r - 10));
        lv_obj_t *l = lv_line_create(p);
        lv_obj_set_style_line_width(l, 1, 0);
        lv_obj_set_style_line_color(l, lv_color_hex(col), 0);
        lv_line_set_points(l, ray[i], 2);
    }
}

// One gear of the exposed movement: toothed rim, three spokes, a jewel.
static void fc_gear(lv_obj_t *p, int cx, int cy, int r, uint32_t metal,
                    uint32_t dark, int teeth, uint32_t jewel) {
    ui_box(p, cx - r, cy - r, r * 2, r * 2, dark, LV_RADIUS_CIRCLE);
    ui_ring(p, cx, cy, r, metal, 2);
    for (int i = 0; i < teeth; i++) {
        float a = (float)i * 360.0f / (float)teeth * 3.14159265f / 180.0f;
        int tx = cx + (int)(sinf(a) * (float)(r + 3));
        int ty = cy - (int)(cosf(a) * (float)(r + 3));
        ui_box(p, tx - 2, ty - 2, 4, 4, metal, 1);
    }
    static lv_point_t sp[3][2];
    for (int i = 0; i < 3; i++) {
        float a = (float)i * 120.0f * 3.14159265f / 180.0f;
        sp[i][0].x = (lv_coord_t)(cx - sinf(a) * (float)(r - 6));
        sp[i][0].y = (lv_coord_t)(cy + cosf(a) * (float)(r - 6));
        sp[i][1].x = (lv_coord_t)(cx + sinf(a) * (float)(r - 6));
        sp[i][1].y = (lv_coord_t)(cy - cosf(a) * (float)(r - 6));
        lv_obj_t *l = lv_line_create(p);
        lv_obj_set_style_line_width(l, 3, 0);
        lv_obj_set_style_line_color(l, lv_color_hex(metal), 0);
        lv_line_set_points(l, sp[i], 2);
    }
    ui_ring(p, cx, cy, r / 4, metal, 2);
    ui_box(p, cx - 5, cy - 5, 10, 10, jewel, LV_RADIUS_CIRCLE);
}

// A coiled serpent in raised gold: overlapping discs along a spiral, two
// shades alternating so the body reads as engraved scales, plus a head.
static void fc_serpent(lv_obj_t *p, int cx, int cy, uint32_t gold, uint32_t shade) {
    const int n = 40;
    for (int i = 0; i < n; i++) {
        float t = (float)i / (float)(n - 1);
        float ang = t * 4.6f * 2.0f * 3.14159265f + 0.6f;
        int rad = (int)(34.0f + t * 116.0f);
        int bx = cx + (int)(sinf(ang) * (float)rad);
        int by = cy - (int)(cosf(ang) * (float)rad);
        int sz = (int)(17.0f - t * 8.0f);
        ui_box(p, bx - sz / 2, by - sz / 2, sz, sz, (i % 2) ? gold : shade,
               LV_RADIUS_CIRCLE);
    }
    // head at the end of the coil, with two horns and an eye
    float ang = 4.6f * 2.0f * 3.14159265f + 0.6f;
    int hx = cx + (int)(sinf(ang) * 150.0f);
    int hy = cy - (int)(cosf(ang) * 150.0f);
    ui_box(p, hx - 13, hy - 10, 26, 20, gold, 9);
    ui_box(p, hx - 4, hy - 20, 4, 12, gold, 2);
    ui_box(p, hx + 3, hy - 22, 4, 14, gold, 2);
    ui_box(p, hx - 6, hy - 4, 5, 5, 0x1A1206, 3);
    // a single claw reaching across the dial
    static lv_point_t claw[3][2];
    for (int i = 0; i < 3; i++) {
        claw[i][0].x = (lv_coord_t)(cx - 60 + i * 8);
        claw[i][0].y = (lv_coord_t)(cy + 40);
        claw[i][1].x = (lv_coord_t)(cx - 130 + i * 20);
        claw[i][1].y = (lv_coord_t)(cy + 96 - i * 10);
        lv_obj_t *l = lv_line_create(p);
        lv_obj_set_style_line_width(l, 3, 0);
        lv_obj_set_style_line_color(l, lv_color_hex(gold), 0);
        lv_obj_set_style_line_rounded(l, true, 0);
        lv_line_set_points(l, claw[i], 2);
    }
}

// ---- the case -------------------------------------------------------------
//  Shared by every drawn style: a rim, an optional fluted bezel, the dial
//  plate and the minute track.  This is what makes the face fill the screen.
static void fc_case(lv_obj_t *p, uint32_t rim, uint32_t dial, bool fluted) {
    if (fluted) {
        static lv_point_t flute[72][2];
        for (int i = 0; i < 72; i++) {
            float a = (float)i * 5.0f * 3.14159265f / 180.0f;
            flute[i][0].x = (lv_coord_t)(kFCx + sinf(a) * (float)(kFCR - 14));
            flute[i][0].y = (lv_coord_t)(kFCy - cosf(a) * (float)(kFCR - 14));
            flute[i][1].x = (lv_coord_t)(kFCx + sinf(a) * (float)kFCR);
            flute[i][1].y = (lv_coord_t)(kFCy - cosf(a) * (float)kFCR);
            lv_obj_t *l = lv_line_create(p);
            lv_obj_set_style_line_width(l, 2, 0);
            lv_obj_set_style_line_color(l, lv_color_hex(i % 2 ? rim : 0x6B5526), 0);
            lv_line_set_points(l, flute[i], 2);
        }
    }
    ui_ring(p, kFCx, kFCy, kFCR, rim, 3);
    ui_box(p, kFCx - kFCD, kFCy - kFCD, kFCD * 2, kFCD * 2, dial, LV_RADIUS_CIRCLE);
    ui_ring(p, kFCx, kFCy, kFCD, rim, 2);
}

static void fc_brand(lv_obj_t *p, const char *top, const char *sub, uint32_t col, int y) {
    lv_obj_t *b = ui_text(p, top, col, &lv_font_montserrat_16, 0, 0, 0);
    lv_obj_set_style_text_letter_space(b, 5, 0);
    lv_obj_align(b, LV_ALIGN_TOP_MID, 0, y);
    if (sub) {
        lv_obj_t *s = ui_text(p, sub, col, &lv_font_montserrat_10, 0, 0, 0);
        lv_obj_set_style_text_letter_space(s, 3, 0);
        lv_obj_align(s, LV_ALIGN_TOP_MID, 0, y + 20);
    }
}

// ---- style painters -------------------------------------------------------
static void face_classic(lv_obj_t *p, uint32_t ac, uint32_t bg, int style) {
    fc_case(p, ac, bg, false);
    fc_minutes(p, kFCx, kFCy, kFCD - 8, ac);
    if (style == 3) ui_roman_numerals(p, kFCx, kFCy, kFCD - 10, g_fg);
    else fc_markers(p, kFCx, kFCy, kFCD - 22, ac, 16, 5, false);
    ui_dial(p, kFCx, kFCy, 148, ac, g_fg, style == 0, g_crown.secs != 0);
    g_date_lbl = ui_text(p, "", g_fg, &lv_font_montserrat_14, kFCx - 70, kFCy + 66, 140);
    lv_obj_set_style_text_align(g_date_lbl, LV_TEXT_ALIGN_CENTER, 0);
    fc_brand(p, "DUALCORE", face_style_name((uint8_t)style), ac, kFCy - 150);
}

static void face_minimal(lv_obj_t *p, uint32_t ac) {
    ui_ring(p, kFCx, kFCy, kFCD - 4, ac, 2);
    ui_ring(p, kFCx, kFCy, kFCD - 34, 0x2A2A2A, 1);
    g_clock_lbl = ui_text(p, "", g_fg, &lv_font_montserrat_48, 0, 0, 410);
    lv_obj_set_style_text_align(g_clock_lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(g_clock_lbl, LV_ALIGN_CENTER, 0, -26);
    g_date_lbl = ui_text(p, "", g_dim, &lv_font_montserrat_14, 0, 0, 410);
    lv_obj_set_style_text_align(g_date_lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(g_date_lbl, LV_ALIGN_CENTER, 0, 34);
    lv_obj_t *q = ui_text(p, "DUALCORE  QUARTZ", ac, &lv_font_montserrat_10, 0, 0, 0);
    lv_obj_set_style_text_letter_space(q, 4, 0);
    lv_obj_align(q, LV_ALIGN_CENTER, 0, 74);
    fc_brand(p, "MINUIT", "NOIR SERIES", g_dim, 78);
}

static void face_moon(lv_obj_t *p, uint32_t ac, uint32_t bg) {
    fc_case(p, ac, bg, false);
    fc_minutes(p, kFCx, kFCy, kFCD - 10, 0x6A5B3E);
    moon_draw(p, kFCx, kFCy - 46, 86, 0xEFE6D2, bg);
    lv_obj_t *mn = ui_text(p, moon_name(moon_cycle_frac()), ac, &lv_font_montserrat_12, 0, 0, 0);
    lv_obj_set_style_text_letter_space(mn, 3, 0);
    lv_obj_align(mn, LV_ALIGN_CENTER, 0, 62);
    g_clock_lbl = ui_text(p, "", g_fg, &lv_font_montserrat_44, 0, 0, 410);
    lv_obj_set_style_text_align(g_clock_lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(g_clock_lbl, LV_ALIGN_CENTER, 0, 116);
    g_date_lbl = ui_text(p, "", g_dim, &lv_font_montserrat_12, 0, 0, 410);
    lv_obj_set_style_text_align(g_date_lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(g_date_lbl, LV_ALIGN_CENTER, 0, 158);
    fc_brand(p, "DUALCORE", "LUNE", ac, kFCy - 158);
}

// SOVEREIGN: a fluted-bezel, champagne-dial luxury sports watch with a
// digital heart.  Homage in layout only -- every mark is DualCore's own.
static void face_sovereign(lv_obj_t *p, uint32_t ac) {
    fc_case(p, ac, 0x1E1709, true);
    fc_guilloche(p, kFCx, kFCy, kFCD - 14, 0x4B3B19);
    fc_markers(p, kFCx, kFCy, kFCD - 24, ac, 20, 6, false);

    // day aperture at 12
    lv_obj_t *day = ui_box(p, kFCx - 44, kFCy - 148, 88, 28, 0xF3E9D2, 4);
    lv_obj_set_style_border_width(day, 1, 0);
    lv_obj_set_style_border_color(day, lv_color_hex(ac), 0);
    char ds[8];
    clock_fmt_day(ds, sizeof(ds));
    lv_obj_t *dl = ui_text(day, ds, 0x171006, &lv_font_montserrat_14, 0, 0, 0);
    lv_obj_center(dl);
    g_sov_day = dl;

    lv_obj_t *brand = ui_text(p, "DUALCORE", ac, &lv_font_montserrat_10, 0, 0, 0);
    lv_obj_set_style_text_letter_space(brand, 5, 0);
    lv_obj_align(brand, LV_ALIGN_CENTER, 0, -108);

    g_clock_lbl = ui_text(p, "", g_fg, &lv_font_montserrat_48, 0, 0, 410);
    lv_obj_set_style_text_align(g_clock_lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(g_clock_lbl, LV_ALIGN_CENTER, 0, -66);

    lv_obj_t *model = ui_text(p, "SOVEREIGN", ac, &lv_font_montserrat_12, 0, 0, 0);
    lv_obj_set_style_text_letter_space(model, 4, 0);
    lv_obj_align(model, LV_ALIGN_CENTER, 0, 6);
    lv_obj_t *cert = ui_text(p, "CHRONOMETER", 0x8A7442, &lv_font_montserrat_10, 0, 0, 0);
    lv_obj_set_style_text_letter_space(cert, 2, 0);
    lv_obj_align(cert, LV_ALIGN_CENTER, 0, 26);

    // date at 3, under a cyclops rim
    lv_obj_t *dwin = ui_box(p, kFCx + 96, kFCy - 16, 62, 32, 0xF3E9D2, 4);
    lv_obj_set_style_border_width(dwin, 1, 0);
    lv_obj_set_style_border_color(dwin, lv_color_hex(ac), 0);
    char dd[4] = "--";
    if (clock_valid()) {
        time_t t; time(&t);
        struct tm lt; localtime_r(&t, &lt);
        snprintf(dd, sizeof(dd), "%02d", lt.tm_mday);
    }
    lv_obj_t *dnum = ui_text(dwin, dd, 0x171006, &lv_font_montserrat_16, 0, 0, 0);
    lv_obj_center(dnum);
    g_sov_date = dnum;
    ui_ring(p, kFCx + 127, kFCy, 25, ac, 1);

    lv_obj_t *six = ui_text(p, "VI", ac, &lv_font_montserrat_12, 0, 0, 0);
    lv_obj_align(six, LV_ALIGN_CENTER, 0, 116);
    lv_obj_t *nine = ui_text(p, "IX", ac, &lv_font_montserrat_12, 0, 0, 0);
    lv_obj_align(nine, LV_ALIGN_CENTER, -136, 0);
}

// SERPENT: black lacquer, guilloche, a raised gold coil.  The ornate one.
static void face_serpent(lv_obj_t *p, uint32_t ac) {
    fc_case(p, ac, 0x0A0806, true);
    fc_guilloche(p, kFCx, kFCy, kFCD - 12, 0x3A2C12);
    fc_serpent(p, kFCx, kFCy, 0xD8B45A, 0x9C7A2C);
    fc_markers(p, kFCx, kFCy, kFCD - 26, ac, 18, 5, false);
    lv_obj_t *brand = ui_text(p, "DUALCORE", ac, &lv_font_montserrat_10, 0, 0, 0);
    lv_obj_set_style_text_letter_space(brand, 5, 0);
    lv_obj_align(brand, LV_ALIGN_CENTER, 0, -132);
    // the digital heart, on a smoked plate so it survives the gold
    lv_obj_t *plate = ui_box(p, kFCx - 104, kFCy - 34, 208, 68, 0x070604, 12);
    lv_obj_set_style_bg_opa(plate, 224, 0);
    g_clock_lbl = ui_text(p, "", 0xF6E9C8, &lv_font_montserrat_44, 0, 0, 208);
    lv_obj_set_style_text_align(g_clock_lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align_to(g_clock_lbl, plate, LV_ALIGN_CENTER, 0, 0);
    lv_obj_t *model = ui_text(p, "SERPENT", ac, &lv_font_montserrat_12, 0, 0, 0);
    lv_obj_set_style_text_letter_space(model, 4, 0);
    lv_obj_align(model, LV_ALIGN_CENTER, 0, 62);
    g_date_lbl = ui_text(p, "", 0xB9A472, &lv_font_montserrat_12, 0, 0, 410);
    lv_obj_set_style_text_align(g_date_lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(g_date_lbl, LV_ALIGN_CENTER, 0, 140);
}

// CHRONO: ivory dial, black hands, three registers, tachymeter.  The only
// light face in the collection -- the power manager already knows that a
// bright dial costs more at the same brightness setting.
static void face_chrono(lv_obj_t *p) {
    ui_ring(p, kFCx, kFCy, kFCR, 0xB9BCC2, 4);
    ui_box(p, kFCx - kFCD, kFCy - kFCD, kFCD * 2, kFCD * 2, 0xF1EFE9, LV_RADIUS_CIRCLE);
    fc_tachy(p, kFCx, kFCy, kFCD - 14, 0x2A2A2A);
    fc_minutes(p, kFCx, kFCy, kFCD - 30, 0x1C1C1C);
    fc_markers(p, kFCx, kFCy, kFCD - 40, 0x101010, 20, 6, false);

    fc_subdial(p, kFCx - 92, kFCy - 24, 44, 0x2A2A2A, 0x1A1A1A, "CHRONO", "60");
    fc_subdial(p, kFCx + 92, kFCy - 24, 44, 0x2A2A2A, 0x1A1A1A, "MIN", "30");
    char sec[4] = "--";
    if (clock_valid()) {
        time_t t; time(&t);
        struct tm lt; localtime_r(&t, &lt);
        snprintf(sec, sizeof(sec), "%02d", lt.tm_sec);
    }
    fc_subdial(p, kFCx, kFCy + 104, 44, 0x2A2A2A, 0x1A1A1A, "SEC", sec);

    ui_dial(p, kFCx, kFCy, 132, 0x1C1C1C, 0x111111, false, g_crown.secs != 0);
    lv_obj_t *brand = ui_text(p, "DUALCORE", 0x111111, &lv_font_montserrat_16, 0, 0, 0);
    lv_obj_set_style_text_letter_space(brand, 4, 0);
    lv_obj_align(brand, LV_ALIGN_CENTER, 0, -122);
    lv_obj_t *sub = ui_text(p, "CHRONOGRAPH  AUTOMATIC", 0x4A4A4A,
                            &lv_font_montserrat_10, 0, 0, 0);
    lv_obj_set_style_text_letter_space(sub, 2, 0);
    lv_obj_align(sub, LV_ALIGN_CENTER, 0, -102);
    g_date_lbl = ui_text(p, "", 0x2A2A2A, &lv_font_montserrat_12, 0, 0, 410);
    lv_obj_set_style_text_align(g_date_lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(g_date_lbl, LV_ALIGN_CENTER, 0, 158);
}

// SKELETON: the movement itself is the dial -- bridges, gear train, jewels.
static void face_skeleton(lv_obj_t *p, uint32_t ac) {
    ui_ring(p, kFCx, kFCy, kFCR, 0x5A5F66, 3);
    ui_box(p, kFCx - kFCD, kFCy - kFCD, kFCD * 2, kFCD * 2, 0x131418, LV_RADIUS_CIRCLE);

    static lv_point_t br[4][2];
    const int bco[4] = { -120, 118, -70, 96 };
    for (int i = 0; i < 4; i++) {
        br[i][0].x = (lv_coord_t)(kFCx + bco[i]);
        br[i][0].y = (lv_coord_t)(kFCy - 150 + (i % 2) * 40);
        br[i][1].x = (lv_coord_t)(kFCx + bco[i] / 3);
        br[i][1].y = (lv_coord_t)(kFCy + 160 - (i % 2) * 50);
        lv_obj_t *l = lv_line_create(p);
        lv_obj_set_style_line_width(l, 16, 0);
        lv_obj_set_style_line_color(l, lv_color_hex(0x3A3E45), 0);
        lv_obj_set_style_line_rounded(l, true, 0);
        lv_line_set_points(l, br[i], 2);
    }
    fc_gear(p, kFCx - 96, kFCy - 86, 48, 0x8D939C, 0x1B1D22, 22, 0x9B2C4A);
    fc_gear(p, kFCx + 104, kFCy - 62, 34, 0x8D939C, 0x1B1D22, 16, 0x2C5A9B);
    fc_gear(p, kFCx + 58, kFCy + 108, 58, 0xB0B6BE, 0x1B1D22, 26, 0x9B2C4A);
    fc_gear(p, kFCx - 84, kFCy + 96, 26, 0x8D939C, 0x1B1D22, 14, 0x2C9B7A);

    lv_obj_t *plate = ui_box(p, kFCx - 96, kFCy - 40, 192, 76, 0x000000, 14);
    lv_obj_set_style_bg_opa(plate, 208, 0);
    g_clock_lbl = ui_text(p, "", 0xF2F4F7, &lv_font_montserrat_44, 0, 0, 192);
    lv_obj_set_style_text_align(g_clock_lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align_to(g_clock_lbl, plate, LV_ALIGN_CENTER, 0, 0);

    lv_obj_t *brand = ui_text(p, "DUALCORE", ac, &lv_font_montserrat_10, 0, 0, 0);
    lv_obj_set_style_text_letter_space(brand, 5, 0);
    lv_obj_align(brand, LV_ALIGN_CENTER, 0, -140);
    lv_obj_t *model = ui_text(p, "SQUELETTE", 0x8D939C, &lv_font_montserrat_10, 0, 0, 0);
    lv_obj_set_style_text_letter_space(model, 3, 0);
    lv_obj_align(model, LV_ALIGN_CENTER, 0, 52);
    g_date_lbl = ui_text(p, "", 0x8D939C, &lv_font_montserrat_12, 0, 0, 410);
    lv_obj_set_style_text_align(g_date_lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(g_date_lbl, LV_ALIGN_CENTER, 0, 148);
}

// DIVE: chunky bezel, lume pip, heavy markers, a red sweep hand.
static void face_dive(lv_obj_t *p, uint32_t ac) {
    (void)ac;
    ui_ring(p, kFCx, kFCy, kFCR + 4, 0x22262A, 8);
    for (int i = 0; i < 60; i++) {
        float a = (float)i * 6.0f * 3.14159265f / 180.0f;
        int tx = kFCx + (int)(sinf(a) * (float)(kFCR - 6));
        int ty = kFCy - (int)(cosf(a) * (float)(kFCR - 6));
        bool maj = (i % 5) == 0;
        ui_box(p, tx - (maj ? 2 : 1), ty - (maj ? 6 : 3), maj ? 4 : 2, maj ? 12 : 6,
               maj ? 0xE8EEF2 : 0x6E767C, 1);
    }
    ui_box(p, kFCx - kFCD, kFCy - kFCD, kFCD * 2, kFCD * 2, 0x090B0D, LV_RADIUS_CIRCLE);
    ui_box(p, kFCx - 5, kFCy - kFCD + 8, 10, 10, 0xD8B45A, 5);   // lume pip
    fc_minutes(p, kFCx, kFCy, kFCD - 10, 0x4A5054);
    fc_markers(p, kFCx, kFCy, kFCD - 26, 0xE8EEF2, 26, 8, false);
    ui_dial(p, kFCx, kFCy, 146, 0xC8323C, 0xF2F5F7, false, true);

    // day/date window at 3: two apertures side by side
    lv_obj_t *w = ui_box(p, kFCx + 92, kFCy - 15, 74, 30, 0xF2F5F7, 3);
    char dd[4] = "--";
    if (clock_valid()) {
        time_t t; time(&t);
        struct tm lt; localtime_r(&t, &lt);
        snprintf(dd, sizeof(dd), "%02d", lt.tm_mday);
    }
    lv_obj_t *dl = ui_text(w, dd, 0x08090B, &lv_font_montserrat_16, 38, 5, 0);
    char ds[8];
    clock_fmt_day(ds, sizeof(ds));
    ui_text(w, ds, 0x08090B, &lv_font_montserrat_10, 6, 9, 0);
    g_sov_date = dl;

    lv_obj_t *brand = ui_text(p, "DUALCORE", 0xC8323C, &lv_font_montserrat_16, 0, 0, 0);
    lv_obj_set_style_text_letter_space(brand, 4, 0);
    lv_obj_align(brand, LV_ALIGN_CENTER, 0, -120);
    lv_obj_t *sub = ui_text(p, "DIVER  200 m", 0xE8EEF2, &lv_font_montserrat_10, 0, 0, 0);
    lv_obj_set_style_text_letter_space(sub, 3, 0);
    lv_obj_align(sub, LV_ALIGN_CENTER, 0, -100);
    g_date_lbl = ui_text(p, "", 0x9AA3A9, &lv_font_montserrat_12, 0, 0, 410);
    lv_obj_set_style_text_align(g_date_lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(g_date_lbl, LV_ALIGN_CENTER, 0, 132);
}

// ---- PHOTO: the user's own dial -------------------------------------------
//  /WATCH/CrownOS/assets/faces/dial.raw -- RGB565 little endian, 410x502.
//  It lives in PSRAM only while a PHOTO face is on screen.
static uint8_t       *g_dial_buf = nullptr;
static lv_img_dsc_t   g_dial_dsc;
static int8_t         g_dial_state = -1;     // -1 untried, 0 unusable, 1 loaded

static void dial_free() {
    if (g_dial_buf) { free(g_dial_buf); g_dial_buf = nullptr; }
    g_dial_state = -1;
}

static bool dial_load() {
    if (g_dial_state == 1) return true;
    if (g_dial_state == 0) return false;
    g_dial_state = 0;
    if (!g_sd_ok) return false;
    const size_t need = (size_t)WC_LCD_WIDTH * WC_LCD_HEIGHT * 2;
    File f = SD_MMC.open("/WATCH/CrownOS/assets/faces/dial.raw", FILE_READ);
    if (!f) return false;
    if ((size_t)f.size() < need) { f.close(); return false; }
    if (!g_dial_buf) g_dial_buf = (uint8_t *)heap_caps_malloc(need, MALLOC_CAP_SPIRAM);
    if (!g_dial_buf) { f.close(); dcs_log(DCS_PATH_CROWN, "dial: no PSRAM"); return false; }
    size_t rd = f.read(g_dial_buf, need);
    f.close();
    if (rd != need) return false;
    g_dial_dsc.header.cf = LV_IMG_CF_TRUE_COLOR;
    g_dial_dsc.header.always_zero = 0;
    g_dial_dsc.header.w = WC_LCD_WIDTH;
    g_dial_dsc.header.h = WC_LCD_HEIGHT;
    g_dial_dsc.data_size = (uint32_t)need;
    g_dial_dsc.data = g_dial_buf;
    g_dial_state = 1;
    dcs_log(DCS_PATH_CROWN, "photo dial loaded");
    return true;
}

static void face_photo(lv_obj_t *p) {
    if (!dial_load()) {
        // safe fallback: a drawn classic face and an honest note
        face_classic(p, kAccentCol[g_crown.accent % 5], kBgCol[g_crown.bg % 4], 0);
        lv_obj_t *n = ui_text(p, "NO DIAL IMAGE\nassets/faces/dial.raw", 0xC8323C,
                              &lv_font_montserrat_12, 0, 0, 340);
        lv_obj_set_style_text_align(n, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_align(n, LV_ALIGN_BOTTOM_MID, 0, -18);
        return;
    }
    lv_obj_t *img = lv_img_create(p);
    lv_img_set_src(img, &g_dial_dsc);
    lv_obj_set_pos(img, 0, 0);
    lv_obj_clear_flag(img, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(img, LV_OBJ_FLAG_SCROLLABLE);
    // smoked scrim behind the time so any photograph stays readable
    lv_obj_t *sc = ui_box(p, 0, 386, 410, 116, 0x000000, 0);
    lv_obj_set_style_bg_opa(sc, 150, 0);
    g_clock_lbl = ui_text(p, "", 0xFFFFFF, &lv_font_montserrat_48, 0, 400, 410);
    lv_obj_set_style_text_align(g_clock_lbl, LV_TEXT_ALIGN_CENTER, 0);
    g_date_lbl = ui_text(p, "", 0xD8DEE6, &lv_font_montserrat_12, 0, 460, 410);
    lv_obj_set_style_text_align(g_date_lbl, LV_TEXT_ALIGN_CENTER, 0);
}

// ---- glitch ---------------------------------------------------------------
//  The one piece of motion on the face: every so often the dial tears for a
//  few frames -- offset slices and a one-pixel tracking shift.  That is the
//  watch's only tell that it is a computer.  Off / Rare / Often, and it never
//  runs in Eco or Survival, or with animations disabled.
#define GLITCH_SLICES 7
static lv_obj_t   *g_gl_slice[GLITCH_SLICES] = { 0 };
static lv_obj_t   *g_gl_owner = nullptr;
static lv_timer_t *g_gl_timer = nullptr;
static int         g_gl_frames = 0;
static uint32_t    g_gl_next = 0;

static void glitch_clear_slices() {
    for (int i = 0; i < GLITCH_SLICES; i++) {
        if (g_gl_slice[i]) { lv_obj_del(g_gl_slice[i]); g_gl_slice[i] = nullptr; }
    }
}

static void glitch_stop() {
    glitch_clear_slices();
    if (g_gl_timer) { lv_timer_del(g_gl_timer); g_gl_timer = nullptr; }
    g_gl_owner = nullptr;
    g_gl_frames = 0;
}

static void glitch_arm(uint32_t now) {
    if (g_crown.glitch == 0) { g_gl_next = now + 60000UL; return; }
    uint32_t span = (g_crown.glitch == 2) ? 5000UL : 12000UL;
    g_gl_next = now + span + (uint32_t)random(0, 4000);
}

static void glitch_tick(lv_timer_t *t) {
    (void)t;
    if (!g_gl_owner || g_gl_owner != g_crown_face_host || g_screen_off) { glitch_stop(); return; }
    if (--g_gl_frames <= 0) {
        glitch_clear_slices();
        lv_obj_set_x(g_gl_owner, 0);
        if (g_gl_timer) { lv_timer_del(g_gl_timer); g_gl_timer = nullptr; }
        glitch_arm(millis());
        return;
    }
    lv_obj_set_x(g_gl_owner, (lv_coord_t)random(-3, 4));
    for (int i = 0; i < GLITCH_SLICES; i++) {
        if (!g_gl_slice[i]) continue;
        int w = (int)random(30, 230);
        int y = (int)random(kFCy - kFCD, kFCy + kFCD);
        lv_obj_set_pos(g_gl_slice[i], (lv_coord_t)random(-24, 24), (lv_coord_t)y);
        lv_obj_set_size(g_gl_slice[i], (lv_coord_t)w, (lv_coord_t)(random(0, 3) ? 2 : 5));
        lv_obj_set_style_bg_opa(g_gl_slice[i], (lv_opa_t)random(90, 210), 0);
        lv_obj_set_style_bg_color(g_gl_slice[i],
            lv_color_hex((i % 3 == 0) ? kAccentCol[g_crown.accent % 5] : g_fg), 0);
    }
}

// Called from the shared 500 ms tick: fires a burst when one is due.  All the
// guards live here so an idle watch face costs one compare per tick.
static void crown_glitch_service() {
    if (g_cur_os != 1 || g_crown_tab != 0) return;
    if (g_gl_next && millis() >= g_gl_next) { glitch_burst(); glitch_arm(millis()); }
}

static void glitch_burst() {
    if (!g_crown.anim || g_crown.glitch == 0) return;
    if (g_cfg.power >= PWR_ECO) return;
    if (g_cur_os != 1 || g_crown_tab != 0 || g_screen_off) return;
    if (!g_crown_face_host) return;
    glitch_stop();
    g_gl_owner = g_crown_face_host;
    for (int i = 0; i < GLITCH_SLICES; i++) {
        g_gl_slice[i] = ui_box(g_gl_owner, 0, 0, 60, 2, kAccentCol[g_crown.accent % 5], 0);
        lv_obj_clear_flag(g_gl_slice[i], LV_OBJ_FLAG_CLICKABLE);
        lv_obj_clear_flag(g_gl_slice[i], LV_OBJ_FLAG_SCROLLABLE);
    }
    g_gl_frames = 5;
    g_gl_timer = lv_timer_create(glitch_tick, 60, NULL);
}

// ---- the face screen ------------------------------------------------------
static lv_coord_t g_tap_x = 0, g_tap_y = 0;
static uint32_t   g_tap_ms = 0;

static void crown_paint(lv_obj_t *p) {
    FaceDef &f = g_faces[g_crown.face];
    uint32_t ac = kAccentCol[g_crown.accent % 5];
    uint32_t bg = kBgCol[g_crown.bg % 4];
    switch (f.style) {
        case 1:  face_minimal(p, ac); break;
        case 4:  face_moon(p, ac, bg); break;
        case 5:  face_sovereign(p, ac); break;
        case 6:  face_serpent(p, ac); break;
        case 7:  face_chrono(p); break;
        case 8:  face_skeleton(p, ac); break;
        case 9:  face_dive(p, ac); break;
        case 10: face_photo(p); break;
        default: face_classic(p, ac, bg, f.style); break;
    }
}

static void crown_step_tab(int d) {
    g_crown_tab = (g_crown_tab + d + 4) % 4;
    g_crown_chrome = false;
    crown_rebuild();
}

static void crown_chrome_toggle() {
    g_crown_chrome = !g_crown_chrome;
    crown_rebuild();
}

static void crown_hide_cb(lv_event_t *e) {
    (void)e;
    crown_chrome_toggle();
}

static void crown_face_press_cb(lv_event_t *e) {
    (void)e;
    lv_indev_t *ind = lv_indev_get_act();
    if (!ind) return;
    lv_point_t p;
    lv_indev_get_point(ind, &p);
    g_tap_x = p.x; g_tap_y = p.y; g_tap_ms = millis();
}

// Tap = chrome, swipe = page.  Hand-rolled from press/release so it does not
// depend on the gesture module being enabled in lv_conf.
static void crown_face_release_cb(lv_event_t *e) {
    (void)e;
    lv_indev_t *ind = lv_indev_get_act();
    if (!ind) return;
    lv_point_t p;
    lv_indev_get_point(ind, &p);
    int dx = (int)p.x - (int)g_tap_x;
    int dy = (int)p.y - (int)g_tap_y;
    uint32_t dt = millis() - g_tap_ms;
    if (abs(dx) < 26 && abs(dy) < 26 && dt < 600) { crown_chrome_toggle(); return; }
    if (abs(dx) > 60 && abs(dx) > abs(dy)) { crown_step_tab(dx < 0 ? 1 : -1); return; }
    if (dy < -70) { if (!g_crown_chrome) crown_chrome_toggle(); return; }
    if (dy > 70)  { if (g_crown_chrome) crown_chrome_toggle(); return; }
}

static void crown_face_long_cb2(lv_event_t *e) {
    (void)e;
    g_crown_tab = 1;
    g_crown_chrome = false;
    crown_rebuild();
}

// Chrome: the header and tab bar drawn *over* the dial, so the watch face is
// never cropped and never rebuilt when the chrome appears.
static void crown_build_chrome(lv_obj_t *root) {
    lv_obj_t *hdr = ui_box(root, 0, 0, 410, 58, g_panel, 0);
    lv_obj_set_style_bg_opa(hdr, 240, 0);
    lv_obj_add_event_cb(hdr, quick_sheet_cb, LV_EVENT_LONG_PRESSED, NULL);
    ui_text(root, "CrownOS", g_fg, &lv_font_montserrat_20, 16, 9, 0);
    char sub[48];
    snprintf(sub, sizeof(sub), "%s  /  %s", g_faces[g_crown.face].name,
             kOcc[g_crown.profile % 4].name);
    ui_text(root, sub, g_dim, &lv_font_montserrat_12, 16, 35, 0);
    ui_box(root, 0, 57, 410, 1, g_accent, 0);

    g_conn_lbl = ui_text(root, "OFF", g_dim, &lv_font_montserrat_12, 176, 22, 48);
    lv_obj_set_style_text_align(g_conn_lbl, LV_TEXT_ALIGN_RIGHT, 0);
    g_batt_lbl = ui_text(root, "-- %", g_fg, &lv_font_montserrat_16, 228, 18, 62);
    lv_obj_set_style_text_align(g_batt_lbl, LV_TEXT_ALIGN_RIGHT, 0);
    ui_btn(root, 296, 14, 54, 30, "HIDE", g_panel, g_dim, 15, crown_hide_cb, NULL);
    ui_btn(root, 354, 14, 44, 30, "\xE2\x9C\x95", g_panel, g_dim, 15, ui_exit_cb, NULL);

    lv_obj_t *tabbg = ui_box(root, 0, 436, 410, 66, g_panel, 0);
    lv_obj_set_style_bg_opa(tabbg, 240, 0);
    for (int i = 0; i < 4; i++) {
        int w = 410 / 4;
        lv_obj_t *b = ui_btn(root, i * w, 436, w, 66, kCrownTabs[i], g_panel,
                             (i == 0) ? g_accent : g_dim, 0, crown_tab_cb,
                             (void *)(intptr_t)i);
        lv_obj_set_style_text_font(lv_obj_get_child(b, 0), &lv_font_montserrat_14, 0);
        if (i == 0) ui_box(root, i * w + w / 2 - 20, 436, 40, 3, g_accent, 0);
    }
}

static void crown_build_full() {
    ui_set_theme(1);
    ui_clear_refs();

    lv_obj_t *root = ui_screen(0x000000);
    lv_obj_t *host = lv_obj_create(root);
    lv_obj_set_pos(host, 0, 0);
    lv_obj_set_size(host, 410, 502);
    lv_obj_set_style_bg_opa(host, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(host, 0, 0);
    lv_obj_set_style_pad_all(host, 0, 0);
    lv_obj_clear_flag(host, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(host, LV_OBJ_FLAG_CLICKABLE);
    g_crown_face_host = host;

    crown_paint(host);

    lv_obj_add_event_cb(host, crown_face_press_cb, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(host, crown_face_release_cb, LV_EVENT_RELEASED, NULL);
    lv_obj_add_event_cb(host, crown_face_long_cb2, LV_EVENT_LONG_PRESSED, NULL);

    if (g_crown_chrome) crown_build_chrome(root);

    // a fading hint on the first moment, so the gesture is discoverable
    if (!g_crown_chrome) {
        lv_obj_t *h = ui_text(root, "TAP  \xC2\xB7  SWIPE", g_accent, &lv_font_montserrat_10,
                              0, 0, 0);
        lv_obj_set_style_text_letter_space(h, 4, 0);
        lv_obj_align(h, LV_ALIGN_BOTTOM_MID, 0, -6);
        lv_obj_set_style_text_opa(h, 190, 0);
        // If this label is deleted by a rebuild mid-fade, the animation must
        // die with it: LVGL does not clear anims on object deletion.
        lv_obj_add_event_cb(h, [](lv_event_t *ev) {
            lv_anim_del(lv_event_get_target(ev), NULL);
        }, LV_EVENT_DELETE, NULL);
        lv_anim_t a;
        lv_anim_init(&a);
        lv_anim_set_var(&a, h);
        lv_anim_set_exec_cb(&a, [](void *v, int32_t x) {
            lv_obj_set_style_text_opa((lv_obj_t *)v, (lv_opa_t)x, 0);
        });
        lv_anim_set_values(&a, 190, 0);
        lv_anim_set_time(&a, 600);
        lv_anim_set_delay(&a, 2200);
        lv_anim_start(&a);   // fades and stays invisible; never deletes itself
    }

    ui_nav(root);
    if (g_crown_intro) { g_crown_intro = false; crown_reveal(); }
    glitch_arm(millis());
}

static void crown_glitch_cb(lv_event_t *e) {
    (void)e;
    g_crown.glitch = (uint8_t)((g_crown.glitch + 1) % 3);
    crown_save();
    crown_rebuild();
}

static void crown_show_face_cb(lv_event_t *e) {
    (void)e;
    g_crown_tab = 0;
    g_crown_chrome = false;
    crown_rebuild();
}

static void crown_build_studio(lv_obj_t *b) {
    face_clamp();
    ui_body_scroll(b);
    int y = 8;
    ui_btn(b, 12, y, 386, 38, "\xE2\x86\x90  OPEN THE WATCH FACE", g_panel, g_accent, 10,
           crown_show_face_cb, NULL);
    y += 46;
    ui_text(b, "SAVED FACES", g_dim, &lv_font_montserrat_12, 16, y, 0);
    y += 22;
    for (int i = 0; i < g_face_n && i < FACE_MAX; i++) {
        lv_obj_t *row = ui_row(b, 12, y, 386, 46, g_panel,
                               i == g_crown.face ? g_accent : g_panel,
                               crown_face_row_cb, (void *)(intptr_t)i);
        ui_text(row, g_faces[i].name, i == g_crown.face ? g_accent : g_fg,
                &lv_font_montserrat_16, 12, 8, 0);
        ui_text(row, face_style_name(g_faces[i].style), g_dim,
                &lv_font_montserrat_12, 12, 27, 0);
        if (g_faces[i].user) ui_text(row, "USER", g_dim, &lv_font_montserrat_10, 320, 16, 0);
        if (i == g_crown.face) ui_text(row, "ACTIVE", g_accent, &lv_font_montserrat_10, 268, 16, 0);
        y += 52;
    }
    y += 6;

    ui_text(b, "ACCENT", g_dim, &lv_font_montserrat_12, 16, y, 0);
    y += 20;
    for (int i = 0; i < 5; i++) {
        lv_obj_t *sw = ui_box(b, 16 + i * 52, y, 42, 42, kAccentCol[i], 21);
        lv_obj_add_flag(sw, LV_OBJ_FLAG_CLICKABLE);
        if (i == g_crown.accent) lv_obj_set_style_border_width(sw, 3, 0);
        if (i == g_crown.accent) lv_obj_set_style_border_color(sw, lv_color_hex(g_fg), 0);
        lv_obj_add_event_cb(sw, crown_accent_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
    }
    y += 52;
    ui_text(b, "BACKGROUND", g_dim, &lv_font_montserrat_12, 16, y, 0);
    y += 20;
    for (int i = 0; i < 4; i++) {
        lv_obj_t *sw = ui_box(b, 16 + i * 52, y, 42, 42, kBgCol[i], 12);
        lv_obj_set_style_border_width(sw, i == g_crown.bg ? 3 : 1, 0);
        lv_obj_set_style_border_color(sw, lv_color_hex(i == g_crown.bg ? g_accent : g_dim), 0);
        lv_obj_add_flag(sw, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(sw, crown_bg_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
    }
    y += 56;

    char v[48];
    snprintf(v, sizeof(v), "COMPLICATIONS   %s", g_crown.compl_on ? "ON" : "OFF");
    ui_btn(b, 12, y, 386, 36, v, g_panel, g_fg, 10, crown_compl_cb, NULL);
    y += 42;
    snprintf(v, sizeof(v), "SIGNATURE ANIM   %s", g_crown.anim ? "ON" : "OFF");
    ui_btn(b, 12, y, 386, 36, v, g_panel, g_fg, 10, crown_anim_cb, NULL);
    y += 42;
    snprintf(v, sizeof(v), "SECONDS HAND     %s", g_crown.secs ? "ON" : "OFF");
    ui_btn(b, 12, y, 386, 36, v, g_panel, g_fg, 10, crown_secs_cb, NULL);
    y += 42;
    snprintf(v, sizeof(v), "EVENT ALERTS     %s", g_crown.alerts ? "ON" : "OFF");
    ui_btn(b, 12, y, 386, 36, v, g_panel, g_fg, 10, crown_alerts_cb, NULL);
    y += 42;
    static const char *gl[3] = { "OFF", "RARE", "OFTEN" };
    snprintf(v, sizeof(v), "DIAL GLITCH      %s", gl[g_crown.glitch % 3]);
    ui_btn(b, 12, y, 386, 36, v, g_panel, g_crown.glitch ? g_accent : g_fg, 10,
           crown_glitch_cb, NULL);
    y += 48;

    ui_text(b, "OCCASION", g_dim, &lv_font_montserrat_12, 16, y, 0);
    y += 20;
    for (int i = 0; i < 4; i++) {
        lv_obj_t *o = ui_btn(b, 12 + (i % 2) * 196, y + (i / 2) * 42, 190, 36,
                             kOcc[i].name,
                             g_crown.profile == (uint8_t)i ? g_panel : g_bg,
                             g_crown.profile == (uint8_t)i ? g_accent : g_dim,
                             10, crown_occasion_cb, (void *)(intptr_t)i);
        (void)o;
    }
    y += 92;

    ui_btn(b, 12, y, 190, 42, "SAVE AS NEW", g_panel, g_accent, 12, crown_save_new_cb, NULL);
    bool can_over = g_faces[g_crown.face].user != 0;
    ui_btn(b, 208, y, 190, 42, can_over ? "OVERWRITE" : "PRESET LOCKED",
           g_panel, can_over ? g_fg : g_dim, 12, can_over ? crown_overwrite_cb : NULL, NULL);
    y += 56;
    ui_pad(b, y);
}

static void crown_build_events(lv_obj_t *b) {
    ui_body_scroll(b);
    int y = 8;
    ui_text(b, "EVENT COUNTDOWNS", g_dim, &lv_font_montserrat_12, 16, y, 0);
    ui_btn(b, 300, y - 4, 94, 26, "CLEAR", g_panel, g_dim, 9, crown_ev_clear_cb, NULL);
    y += 28;

    if (g_ev_n == 0) {
        ui_text(b, "No events yet.", g_fg, &lv_font_montserrat_16, 16, y, 0);
        y += 26;
        ui_text(b, "Add one of the presets below; the countdown uses the shared "
                   "clock service.", g_dim, &lv_font_montserrat_12, 16, y, 340);
        y += 40;
    }
    time_t now = time(NULL);
    for (int i = 0; i < g_ev_n; i++) {
        int64_t left = g_ev[i].epoch - (int64_t)now;
        char when[40], cd[24];
        time_t et = (time_t)g_ev[i].epoch;
        struct tm lt; localtime_r(&et, &lt);
        snprintf(when, sizeof(when), "%02d/%02d %02d:%02d", lt.tm_mday, lt.tm_mon + 1,
                 lt.tm_hour, lt.tm_min);
        clock_fmt_dur(left > 0 ? left : 0, cd, sizeof(cd));
        lv_obj_t *row = ui_frame(b, 12, y, 386, 54, g_panel, 10, g_panel, 1);
        ui_text(row, g_ev[i].title, g_fg, &lv_font_montserrat_16, 12, 8, 200);
        ui_text(row, when, g_dim, &lv_font_montserrat_12, 12, 30, 0);
        ui_text(row, left > 0 ? cd : "PASSED", left > 0 ? g_accent : g_dim,
                &lv_font_montserrat_12, 240, 18, 0);
        ui_btn(row, 342, 14, 32, 26, "X", g_bg, g_dim, 6, crown_ev_del_cb,
               (void *)(intptr_t)i);
        y += 60;
    }

    y += 8;
    ui_text(b, "ADD PRESET (bounded to 8)", g_dim, &lv_font_montserrat_12, 16, y, 0);
    y += 22;
    static const char *labels[4] = { "DINNER +2H", "MEETING +1D", "FLIGHT +3D", "PARTY +7D" };
    for (int i = 0; i < 4; i++) {
        ui_btn(b, 12 + (i % 2) * 196, y + (i / 2) * 44, 190, 38, labels[i],
               g_panel, g_fg, 10, crown_ev_add_cb, (void *)(intptr_t)i);
    }
    y += 96;
    ui_pad(b, y);
}

// ---- ALMANAC: the complications a mechanical watch cannot show -------------
//  Perpetual calendar, moon, power reserve and the next event.  All of it is
//  arithmetic on the shared clock and the real battery -- no network, no
//  animation, nothing invented.
static void crown_build_almanac(lv_obj_t *b) {
    ui_body_scroll(b);
    int y = 8;
    bool ok = clock_valid();
    time_t now = time(NULL);
    struct tm lt;
    localtime_r(&now, &lt);

    // ---- moon ----
    double ph = moon_cycle_frac();
    int illum = (int)((1.0 - cos(2.0 * 3.14159265358979 * ph)) * 50.0 + 0.5);
    double to_full = (ph < 0.5 ? 0.5 - ph : 1.5 - ph) * 29.530588853;
    double to_new  = (1.0 - ph) * 29.530588853;
    lv_obj_t *m = ui_frame(b, 12, y, 386, 150, g_panel, 12, g_panel, 1);
    ui_text(m, "MOON", g_dim, &lv_font_montserrat_10, 16, 8, 0);
    ui_text(m, "synodic approximation", g_dim, &lv_font_montserrat_10, 232, 8, 0);
    moon_draw(m, 66, 74, 40, g_fg, g_bg);
    ui_text(m, moon_name(ph), g_accent, &lv_font_montserrat_20, 124, 30, 0);
    char l[64];
    snprintf(l, sizeof(l), "%d%% illuminated", illum);
    ui_text(m, l, g_fg, &lv_font_montserrat_14, 124, 58, 0);
    snprintf(l, sizeof(l), "full in %.0f d    new in %.0f d", to_full, to_new);
    ui_text(m, l, g_dim, &lv_font_montserrat_12, 124, 82, 250);
    y += 162;

    // ---- perpetual calendar ----
    static const char *kMon[12] = { "JANUARY", "FEBRUARY", "MARCH", "APRIL", "MAY", "JUNE",
                                    "JULY", "AUGUST", "SEPTEMBER", "OCTOBER", "NOVEMBER",
                                    "DECEMBER" };
    static const char *kDay[7] = { "SUNDAY", "MONDAY", "TUESDAY", "WEDNESDAY", "THURSDAY",
                                   "FRIDAY", "SATURDAY" };
    lv_obj_t *c = ui_frame(b, 12, y, 386, 168, g_panel, 12, g_panel, 1);
    ui_text(c, "PERPETUAL CALENDAR", g_dim, &lv_font_montserrat_10, 16, 8, 0);
    if (ok) {
        int year = lt.tm_year + 1900;
        int yday = lt.tm_yday + 1;
        int monday0 = (lt.tm_wday + 6) % 7;         // 0 = Monday
        int week = (yday - monday0 + 10) / 7;
        if (week < 1) week = 1;
        if (week > 53) week = 53;
        bool leap = (year % 4 == 0 && (year % 100 != 0 || year % 400 == 0));
        int total = leap ? 366 : 365;
        ui_text(c, kDay[lt.tm_wday], g_fg, &lv_font_montserrat_16, 16, 26, 0);
        snprintf(l, sizeof(l), "%02d %s %d", lt.tm_mday, kMon[lt.tm_mon], year);
        ui_text(c, l, g_accent, &lv_font_montserrat_16, 16, 50, 0);
        snprintf(l, sizeof(l), "week %02d        day %03d / %d        %d left",
                 week, yday, total, total - yday);
        ui_text(c, l, g_dim, &lv_font_montserrat_12, 16, 78, 360);
        snprintf(l, sizeof(l), "%s%s", leap ? "leap year" : "common year",
                 g_crown.profile < 4 && kOcc[g_crown.profile].name ?
                     "     occasion profile active" : "");
        ui_text(c, l, g_dim, &lv_font_montserrat_10, 16, 100, 360);
    } else {
        ui_text(c, "Clock not set.", g_warn, &lv_font_montserrat_16, 16, 30, 340);
        ui_text(c, "Sync over Wi-Fi from the quick sheet; the calendar needs a real date.",
                g_dim, &lv_font_montserrat_12, 16, 58, 350);
    }
    y += 180;

    // ---- power reserve: a subdial reading, not a percentage alone ----
    lv_obj_t *r = ui_frame(b, 12, y, 386, 132, g_panel, 12, g_panel, 1);
    ui_text(r, "POWER RESERVE", g_dim, &lv_font_montserrat_10, 16, 8, 0);
    int pct = g_batt_pct < 0 ? 0 : g_batt_pct;
    char est[12];
    est_fmt(est, sizeof(est));
    snprintf(l, sizeof(l), "%d%%", pct);
    ui_text(r, l, g_accent, &lv_font_montserrat_24, 300, 4, 0);
    ui_box(r, 16, 44, 354, 12, g_bg, 6);
    int fill = 354 * pct / 100;
    if (pct > 0 && fill < 8) fill = 8;
    ui_box(r, 16, 44, fill, 12, pct < 20 ? g_danger : g_accent, 6);
    for (int i = 0; i <= 10; i++)
        ui_box(r, 15 + i * 35, 60, 1, 6, g_dim, 0);
    snprintf(l, sizeof(l), "estimated runtime %s    profile %s", est, kPwrName[g_cfg.power]);
    ui_text(r, l, g_fg, &lv_font_montserrat_12, 16, 74, 360);
    if (g_batt_hist_n > 1)
        snprintf(l, sizeof(l), "8 h window: min %u%%  max %u%%",
                 (unsigned)g_batt_hist_min, (unsigned)g_batt_hist_max);
    else
        snprintf(l, sizeof(l), "history fills in at one sample per five minutes");
    ui_text(r, l, g_dim, &lv_font_montserrat_10, 16, 98, 360);
    y += 144;

    // ---- next event ----
    char ti[24];
    int64_t left = 0;
    lv_obj_t *e = ui_frame(b, 12, y, 386, 74, g_panel, 12, g_panel, 1);
    ui_text(e, "NEXT ENGAGEMENT", g_dim, &lv_font_montserrat_10, 16, 8, 0);
    if (event_nth(0, ti, sizeof(ti), &left) && left > 0) {
        char cd[24];
        clock_fmt_dur(left, cd, sizeof(cd));
        ui_text(e, ti, g_fg, &lv_font_montserrat_16, 16, 28, 240);
        ui_text(e, cd, g_accent, &lv_font_montserrat_14, 270, 30, 0);
    } else {
        ui_text(e, "Nothing scheduled.", g_dim, &lv_font_montserrat_14, 16, 30, 300);
    }
    y += 86;
    ui_pad(b, y);
}

static void crown_rebuild() {
    if (g_cur_os != 1) return;
    face_clamp();
    glitch_stop();
    g_crown_face_host = nullptr;
    // The PHOTO dial holds ~400 KB of PSRAM.  Keep it only while it is on screen.
    if (g_faces[g_crown.face].style != 10) dial_free();

    if (g_crown_tab == 0) { crown_build_full(); return; }

    lv_obj_t *b = ui_begin(1, "CrownOS", "FACES / OCCASIONS",
                           kCrownTabs, 4, g_crown_tab, crown_tab_cb, ui_exit_cb);
    lv_obj_t *root = lv_obj_get_parent(b);
    if (g_crown_tab == 1)      crown_build_studio(b);
    else if (g_crown_tab == 2) crown_build_events(b);
    else                       crown_build_almanac(b);
    ui_nav(root);
    if (g_crown_intro) { g_crown_intro = false; crown_reveal(); }
}
// ---------------------------------------------------------------------------
//  18.  NightOS  --  shadow mode, quiet alerts, utility panel, schedule
// ---------------------------------------------------------------------------
static int g_night_tab = 0;
static uint8_t g_night_auto = 0;     // 1 = shadow engaged by the schedule
static const char *kNightTabs[4] = { "HOME", "SHADOW", "TOOLS", "OPS" };

static lv_obj_t *g_flash = nullptr;
static uint8_t   g_flash_mode = 0;      // 0 off, 1 white, 2 red
static lv_obj_t *g_stealth = nullptr, *g_stealth_clock = nullptr;

static void night_flash_off() {
    if (g_flash) {
        lv_obj_del_async(g_flash);   // may be called from the overlay's own click
        g_flash = nullptr;
        if (!g_screen_off) dcs_fade_to(dcs_brightness_base(), 150);
    }
    // g_flash_mode is kept: it is the next torch in the cycle, so a tap after
    // white offers red rather than starting over at white.
}

static void night_stealth_tick() {
    if (!g_stealth_clock) return;
    char t[16];
    clock_fmt_time(t, sizeof(t), false);
    lbl_set(g_stealth_clock, t);
}

static void night_stealth_stop() {
    if (g_stealth) {
        lv_obj_del_async(g_stealth);
        g_stealth = nullptr;
        g_stealth_clock = nullptr;
        if (!g_screen_off) { g_fading = false; dcs_fade_to(dcs_brightness_base(), 180); }
        dcs_log(DCS_PATH_NIGHT, "stealth off");
    }
}

static void night_tab_cb(lv_event_t *e) {
    g_night_tab = (int)(intptr_t)lv_event_get_user_data(e);
    night_rebuild();
}

static void night_shadow_cb(lv_event_t *e) {
    (void)e;
    g_night.shadow = g_night.shadow ? 0 : 1;
    if (g_night.shadow && g_shadow_sessions < 999) g_shadow_sessions++;
    g_night_auto = 0;
    night_save();
    power_apply();
    dcs_log(DCS_PATH_NIGHT, "shadow %s", g_night.shadow ? "on" : "off");
    night_rebuild();
}
static void night_quiet_cb(lv_event_t *e) {
    (void)e;
    g_night.quiet = (uint8_t)((g_night.quiet + 1) % 3);
    night_save();
    night_rebuild();
}
static void night_bright_cb(lv_event_t *e) {
    int d = (int)(intptr_t)lv_event_get_user_data(e);
    int b = (int)g_night.brightness + d;
    if (b < 30) b = 30;
    if (b > 255) b = 255;
    g_night.brightness = (uint8_t)b;
    night_save();
    power_apply();
    night_rebuild();
}
static void night_sched_cb(lv_event_t *e) {
    (void)e;
    g_night.sched_on = g_night.sched_on ? 0 : 1;
    g_night_auto = 0;
    night_save();
    night_rebuild();
}
static void night_hour_cb(lv_event_t *e) {
    int which = (int)(intptr_t)lv_event_get_user_data(e);
    if (which == 0) g_night.sched_start = (uint8_t)((g_night.sched_start + 23) % 24);
    else if (which == 1) g_night.sched_start = (uint8_t)((g_night.sched_start + 1) % 24);
    else if (which == 2) g_night.sched_end = (uint8_t)((g_night.sched_end + 23) % 24);
    else g_night.sched_end = (uint8_t)((g_night.sched_end + 1) % 24);
    night_save();
    night_rebuild();
}
static void night_day_cb(lv_event_t *e) {
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    if (i < 0 || i > 6) return;
    g_night.days ^= (uint8_t)(1 << i);
    night_save();
    night_rebuild();
}
static void night_flash_set(uint8_t mode) {
    night_flash_off();
    if (mode == 0) return;             // caller rebuilds the panel
    // White is a torch; red is a night-safe torch.  On AMOLED a red fill
    // really does light only the red subpixels, so this is not a tint.
    uint32_t col = (mode == 2) ? 0x9A0010 : 0xFFFFFF;
    g_flash = ui_box(lv_layer_top(), 0, 0, 410, 502, col, 0);
    lv_obj_add_flag(g_flash, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(g_flash, [](lv_event_t *) {
        night_flash_off();
        night_rebuild();
    }, LV_EVENT_CLICKED, NULL);
    g_fading = false;           // cancel any fade that would overwrite the level
    uint8_t b = (mode == 2) ? 120 : 255;
    g_gfx->setBrightness(b);
    g_cur_brightness = b;
    g_flash_mode = mode;
    lv_obj_move_foreground(g_flash);
    dcs_log(DCS_PATH_NIGHT, "flashlight on (%s)", mode == 2 ? "red" : "white");
}

static void night_flash_cb(lv_event_t *e) {
    (void)e;
    uint8_t next = (g_flash_mode == 1) ? 2 : 1;
    night_flash_set(next);
    night_rebuild();
}

static void night_stealth_cb(lv_event_t *e) {
    (void)e;
    night_flash_off();
    if (g_stealth) { night_stealth_stop(); night_rebuild(); return; }

    lv_obj_t *o = lv_obj_create(lv_layer_top());
    g_stealth = o;
    lv_obj_set_pos(o, 0, 0);
    lv_obj_set_size(o, 410, 502);
    lv_obj_set_style_bg_color(o, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(o, 0, 0);
    lv_obj_set_style_pad_all(o, 0, 0);
    lv_obj_clear_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(o, LV_OBJ_FLAG_CLICKABLE);

    g_stealth_clock = ui_text(o, "", 0x8E2431, &lv_font_montserrat_48, 0, 0, 0);
    lv_obj_align(g_stealth_clock, LV_ALIGN_CENTER, 0, -16);
    lv_obj_t *d = ui_text(o, "", 0x3E2A2F, &lv_font_montserrat_12, 0, 0, 0);
    lv_obj_align(d, LV_ALIGN_CENTER, 0, 38);
    char ds[32];
    clock_fmt_date(ds, sizeof(ds));
    lv_label_set_text(d, ds);
    lv_obj_t *h = ui_text(o, "STEALTH  /  TAP TO WAKE", 0x3A2328,
                          &lv_font_montserrat_10, 0, 0, 0);
    lv_obj_align(h, LV_ALIGN_BOTTOM_MID, 0, -34);

    lv_obj_add_event_cb(o, [](lv_event_t *) {
        night_stealth_stop();
        night_rebuild();
    }, LV_EVENT_CLICKED, NULL);

    g_fading = false;
    g_gfx->setBrightness(28);
    g_cur_brightness = 28;
    lv_obj_move_foreground(o);
    dcs_log(DCS_PATH_NIGHT, "stealth display on");
}
static void night_power_cb(lv_event_t *e) {
    (void)e;
    power_set((uint8_t)((g_cfg.power + 1) % 4));
    night_rebuild();
}
static void night_palette_cb(lv_event_t *e) {
    (void)e;
    g_night.palette = g_night.palette ? 0 : 1;
    night_save();
    dcs_log(DCS_PATH_NIGHT, "palette -> %s", g_night.palette ? "crimson" : "night");
    night_rebuild();
}
static void night_alarm_cb(lv_event_t *e) {
    (void)e;
    alarm_editor();
}

// ---- breathing pacer (4-7-8) ---------------------------------------------
//  A guided session is an explicit user action, so it may animate.  It is
//  interruptible at any time and it holds the screen awake only while open.
static lv_obj_t *g_breathe = nullptr, *g_breathe_ring = nullptr;
static lv_obj_t *g_breathe_phase = nullptr, *g_breathe_count = nullptr;
static lv_timer_t *g_breathe_timer = nullptr;
static uint32_t g_breathe_t0 = 0;

static void breathe_stop() {
    if (g_breathe_timer) { lv_timer_del(g_breathe_timer); g_breathe_timer = nullptr; }
    if (g_breathe) { lv_obj_del_async(g_breathe); g_breathe = nullptr; }
    g_breathe_ring = g_breathe_phase = g_breathe_count = nullptr;
}

static void breathe_tick(lv_timer_t *) {
    if (!g_breathe || !g_breathe_ring) { breathe_stop(); return; }
    g_last_touch_ms = millis();          // an active session keeps the panel on
    uint32_t el = millis() - g_breathe_t0;
    int phase;
    uint32_t rem;
    float p;
    if (el < 4000) {
        phase = 0; p = (float)el / 4000.0f; rem = (4000 - el + 999) / 1000;
    } else if (el < 11000) {
        phase = 1; p = 1.0f; rem = (11000 - el + 999) / 1000;
    } else if (el < 19000) {
        phase = 2; p = 1.0f - (float)(el - 11000) / 8000.0f;
        rem = (19000 - el + 999) / 1000;
    } else {
        g_breathe_t0 = millis();       // loop the cycle
        phase = 0; p = 0.0f; rem = 4;
    }
    int size = 104 + (int)(112.0f * p);
    lv_obj_set_size(g_breathe_ring, size, size);
    lv_obj_align(g_breathe_ring, LV_ALIGN_CENTER, 0, -34);
    static const char *nm[3] = { "INHALE", "HOLD", "EXHALE" };
    lbl_set(g_breathe_phase, nm[phase]);
    char c[8];
    snprintf(c, sizeof(c), "%u", (unsigned)rem);
    lbl_set(g_breathe_count, c);
}

static void night_breathe_cb(lv_event_t *e) {
    (void)e;
    breathe_stop();
    lv_obj_t *o = lv_obj_create(lv_layer_top());
    g_breathe = o;
    lv_obj_set_pos(o, 0, 0);
    lv_obj_set_size(o, 410, 502);
    lv_obj_set_style_bg_color(o, lv_color_hex(0x050407), 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(o, 0, 0);
    lv_obj_set_style_pad_all(o, 0, 0);
    lv_obj_clear_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(o, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *t = ui_text(o, "4 - 7 - 8", g_accent, &lv_font_montserrat_18, 0, 0, 0);
    lv_obj_align(t, LV_ALIGN_TOP_MID, 0, 48);
    lv_obj_t *s = ui_text(o, "BREATHE WITH THE RING", g_dim, &lv_font_montserrat_10, 0, 0, 0);
    lv_obj_align(s, LV_ALIGN_TOP_MID, 0, 76);

    g_breathe_ring = lv_obj_create(o);
    lv_obj_set_size(g_breathe_ring, 104, 104);
    lv_obj_align(g_breathe_ring, LV_ALIGN_CENTER, 0, -34);
    lv_obj_set_style_radius(g_breathe_ring, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(g_breathe_ring, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(g_breathe_ring, 3, 0);
    lv_obj_set_style_border_color(g_breathe_ring, lv_color_hex(g_accent), 0);
    lv_obj_clear_flag(g_breathe_ring, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(g_breathe_ring, LV_OBJ_FLAG_CLICKABLE);

    g_breathe_phase = ui_text(o, "INHALE", g_fg, &lv_font_montserrat_22, 0, 0, 0);
    lv_obj_align(g_breathe_phase, LV_ALIGN_CENTER, 0, 132);
    g_breathe_count = ui_text(o, "4", g_accent, &lv_font_montserrat_32, 0, 0, 0);
    lv_obj_align(g_breathe_count, LV_ALIGN_CENTER, 0, 172);

    ui_btn(o, 125, 396, 160, 50, "STOP", g_panel, g_fg, 14, [](lv_event_t *) {
        breathe_stop();
        night_rebuild();
    }, NULL);
    ui_text(o, "Tap STOP any time; the screen stays on for the session.",
            g_dim, &lv_font_montserrat_10, 40, 460, 330);

    g_breathe_t0 = millis();
    g_breathe_timer = lv_timer_create(breathe_tick, 100, NULL);
    lv_obj_move_foreground(o);
    dcs_log(DCS_PATH_NIGHT, "breathing pacer started");
}
static void night_timer_cb(lv_event_t *e) {
    int what = (int)(intptr_t)lv_event_get_user_data(e);
    if (what == 0)      cd_start(5UL * 60000UL);
    else if (what == 1) cd_start((uint32_t)g_noir.focus_min * 60000UL);
    else if (what == 2) cd_pause();
    else if (what == 3) { if (g_sw_run) sw_stop(); else sw_start(); }
    else if (what == 4) sw_reset();
    timer_save();
    night_rebuild();
}
static void night_media_cb(lv_event_t *e) {
    phone_media_cmd((uint8_t)(intptr_t)lv_event_get_user_data(e));
    night_rebuild();
}
static void night_exit_cb(lv_event_t *e) {
    (void)e;
    os_leave();
}

// ---- optional IMU (SensorLib QMI8658).  Never claims a compass heading. ---
static float g_imu[6] = { 0, 0, 0, 0, 0, 0 };
static bool  g_imu_ok = false, g_imu_tried = false;

#if DCS_USE_SENSORLIB
static SensorQMI8658 g_qmi;
static bool imu_init() {
    if (g_imu_tried) return g_imu_ok;
    g_imu_tried = true;
    if (!g_qmi.begin(Wire, QMI8658_L_SLAVE_ADDRESS, WC_IIC_SDA, WC_IIC_SCL))
        return (g_imu_ok = false);
    g_qmi.configAccelerometer(SensorQMI8658::ACC_RANGE_4G, SensorQMI8658::ACC_ODR_125Hz);
    g_qmi.enableAccelerometer();
    g_qmi.configGyroscope(SensorQMI8658::GYR_RANGE_512DPS, SensorQMI8658::GYR_ODR_112_1Hz);
    g_qmi.enableGyroscope();
    return (g_imu_ok = true);
}
static void imu_read() {
    if (!imu_init()) return;
    g_qmi.getAccelerometer(g_imu[0], g_imu[1], g_imu[2]);
    g_qmi.getGyroscope(g_imu[3], g_imu[4], g_imu[5]);
}
#else
static bool imu_init() { g_imu_tried = true; g_imu_ok = false; return false; }
static void imu_read() { }
#endif

static void night_sensor_cb(lv_event_t *e) {
    (void)e;
    imu_read();
    night_rebuild();
}

// ---- scheduled shadow mode -------------------------------------------------
//  Checked once every 10 s.  Honest about limits: a schedule cannot run while
//  the watch is powered off, and this module never deep-sleeps on its own.
static void night_schedule_service() {
    static uint32_t last = 0;
    uint32_t now = millis();
    if (now - last < 10000U) return;
    last = now;
    if (!g_night.sched_on || !clock_valid()) return;
    time_t t; time(&t);
    struct tm lt; localtime_r(&t, &lt);
    if (!(g_night.days & (uint8_t)(1 << lt.tm_wday))) return;
    int h = lt.tm_hour;
    bool inside;
    if (g_night.sched_start <= g_night.sched_end)
        inside = (h >= g_night.sched_start && h < g_night.sched_end);
    else
        inside = (h >= g_night.sched_start || h < g_night.sched_end);

    if (inside && !g_night.shadow) {
        g_night.shadow = 1;
        if (g_shadow_sessions < 999) g_shadow_sessions++;
        g_night_auto = 1;
        night_save();
        power_apply();
        dcs_log(DCS_PATH_NIGHT, "schedule engaged shadow");
        if (g_cur_os == 2) night_rebuild();
    } else if (!inside && g_night.shadow && g_night_auto) {
        g_night.shadow = 0;
        g_night_auto = 0;
        night_save();
        power_apply();
        dcs_log(DCS_PATH_NIGHT, "schedule released shadow");
        if (g_cur_os == 2) night_rebuild();
    }
}

// ---- NightOS screens ------------------------------------------------------
static void night_build_home(lv_obj_t *b) {
    g_clock_lbl = ui_text(b, "", g_fg, &lv_font_montserrat_48, 22, 4, 0);
    g_date_lbl = ui_text(b, "", g_dim, &lv_font_montserrat_16, 24, 74, 0);

    char s[64], bs[12];
    batt_fmt(bs, sizeof(bs));
    snprintf(s, sizeof(s), "%s  %s", bs, g_batt_ch ? "CHARGING" : "BATTERY");
    ui_text(b, s, g_accent, &lv_font_montserrat_14, 24, 108, 0);
    ui_text(b, g_night.shadow ? "SHADOW ON" : "SHADOW OFF",
            g_night.shadow ? g_accent : g_dim, &lv_font_montserrat_14, 220, 108, 0);

    lv_obj_t *sh = ui_frame(b, 18, 140, 374, 76, g_panel, 12, g_panel, 1);
    ui_text(sh, "SHADOW MODE", g_dim, &lv_font_montserrat_12, 16, 10, 0);
    ui_text(sh, g_night.shadow ? "quiet / reduced light / no decoration"
                              : "normal night operation",
            g_fg, &lv_font_montserrat_14, 16, 32, 250);
    ui_btn(sh, 286, 20, 74, 36, g_night.shadow ? "ON" : "OFF",
           g_night.shadow ? g_accent : g_bg,
           g_night.shadow ? 0x120A10 : g_fg, 10, night_shadow_cb, NULL);

    lv_obj_t *nx = ui_frame(b, 18, 228, 374, 66, g_panel, 12, g_panel, 1);
    ui_text(nx, g_cd_run ? "TIMER RUNNING" : "NEXT", g_dim, &lv_font_montserrat_12, 16, 8, 0);
    if (g_cd_run) {
        g_timer_lbl = ui_text(nx, "00:00", g_fg, &lv_font_montserrat_24, 16, 26, 0);
    } else {
        char an[12];
        if (alarm_next_str(an, sizeof(an))) {
            char al[24];
            snprintf(al, sizeof(al), "Alarm %s", an);
            ui_text(nx, al, g_accent, &lv_font_montserrat_16, 16, 30, 0);
        } else {
            ui_text(nx, "Nothing scheduled", g_dim, &lv_font_montserrat_14, 16, 28, 0);
        }
    }
    char sched[48];
    if (g_night.sched_on) {
        snprintf(sched, sizeof(sched), "%02u:00 - %02u:00", (unsigned)g_night.sched_start,
                 (unsigned)g_night.sched_end);
    } else {
        snprintf(sched, sizeof(sched), "OFF");
    }
    ui_text(nx, "SCHEDULE", g_dim, &lv_font_montserrat_10, 230, 8, 0);
    ui_text(nx, sched, g_accent, &lv_font_montserrat_14, 230, 26, 0);

    char q[64];
    snprintf(q, sizeof(q), "QUIET ALERTS   %s", kQuietName[g_night.quiet]);
    ui_btn(b, 18, 306, 374, 44, q, g_panel, g_fg, 12, night_quiet_cb, NULL);
    ui_text(b, g_night.shadow ? "Essential alerts only; sender previews."
                              : "Full previews follow the shared privacy setting.",
            g_dim, &lv_font_montserrat_12, 20, 356, 360);
}

static void night_build_shadow(lv_obj_t *b) {
    ui_body_scroll(b);
    int y = 10;
    ui_btn(b, 12, y, 386, 56, g_night.shadow ? "SHADOW MODE: ON" : "SHADOW MODE: OFF",
           g_night.shadow ? g_accent : g_panel,
           g_night.shadow ? 0x120A10 : g_fg, 12, night_shadow_cb, NULL);
    y += 66;

    char v[64];
    snprintf(v, sizeof(v), "QUIET ALERTS: %s", kQuietName[g_night.quiet]);
    ui_btn(b, 12, y, 386, 40, v, g_panel, g_fg, 10, night_quiet_cb, NULL);
    y += 50;

    ui_text(b, "NIGHT BRIGHTNESS", g_dim, &lv_font_montserrat_12, 16, y, 0);
    y += 20;
    ui_btn(b, 12, y, 60, 40, "-", g_panel, g_fg, 10, night_bright_cb, (void *)(intptr_t)-20);
    lv_obj_t *bb = ui_box(b, 80, y, 240, 40, g_panel, 10);
    snprintf(v, sizeof(v), "%u / 255", (unsigned)g_night.brightness);
    lv_obj_t *bl = ui_text(bb, v, g_fg, &lv_font_montserrat_14, 0, 0, 0);
    lv_obj_center(bl);
    ui_btn(b, 328, y, 60, 40, "+", g_panel, g_fg, 10, night_bright_cb, (void *)(intptr_t)20);
    y += 52;

    snprintf(v, sizeof(v), "SCHEDULE: %s", g_night.sched_on ? "ON" : "OFF");
    snprintf(v, sizeof(v), "PALETTE: %s", g_night.palette ? "CRIMSON" : "NIGHT");
    ui_btn(b, 12, y, 386, 40, v, g_panel, g_night.palette ? g_accent : g_fg, 10,
           night_palette_cb, NULL);
    y += 50;

    snprintf(v, sizeof(v), "SCHEDULE: %s", g_night.sched_on ? "ON" : "OFF");
    ui_btn(b, 12, y, 386, 40, v, g_panel, g_night.sched_on ? g_accent : g_fg, 10,
           night_sched_cb, NULL);
    y += 50;

    char hb[32];
    snprintf(hb, sizeof(hb), "ACTIVATE  %02u:00", (unsigned)g_night.sched_start);
    ui_text(b, hb, g_fg, &lv_font_montserrat_14, 16, y + 10, 0);
    ui_btn(b, 190, y, 92, 38, "- 1 H", g_panel, g_dim, 9, night_hour_cb, (void *)(intptr_t)0);
    ui_btn(b, 292, y, 92, 38, "+ 1 H", g_panel, g_dim, 9, night_hour_cb, (void *)(intptr_t)1);
    y += 46;
    snprintf(hb, sizeof(hb), "EXIT      %02u:00", (unsigned)g_night.sched_end);
    ui_text(b, hb, g_fg, &lv_font_montserrat_14, 16, y + 10, 0);
    ui_btn(b, 190, y, 92, 38, "- 1 H", g_panel, g_dim, 9, night_hour_cb, (void *)(intptr_t)2);
    ui_btn(b, 292, y, 92, 38, "+ 1 H", g_panel, g_dim, 9, night_hour_cb, (void *)(intptr_t)3);
    y += 50;

    ui_text(b, "DAYS", g_dim, &lv_font_montserrat_12, 16, y, 0);
    y += 20;
    static const char *dn[7] = { "S", "M", "T", "W", "T", "F", "S" };
    for (int i = 0; i < 7; i++) {
        bool on = (g_night.days >> i) & 1;
        ui_btn(b, 12 + i * 55, y, 48, 40, dn[i], on ? g_accent : g_panel,
               on ? 0x120A10 : g_dim, 10, night_day_cb, (void *)(intptr_t)i);
    }
    y += 56;

    ui_text(b, "The schedule uses the shared clock. It cannot activate while the "
               "watch is powered off, and this module does not deep-sleep on its "
               "own, so timers and the link stay alive.",
            g_dim, &lv_font_montserrat_12, 16, y, 370);
    y += 70;
    ui_pad(b, y);
}

static void night_build_tools(lv_obj_t *b) {
    ui_body_scroll(b);
    int y = 10;

    const char *fl = !g_flash ? "FLASHLIGHT (WHITE > RED)" :
                     g_flash_mode == 2 ? "FLASHLIGHT: RED  /  TAP" :
                     "FLASHLIGHT: WHITE  /  TAP";
    ui_btn(b, 12, y, 386, 46, fl,
           !g_flash ? g_panel : (g_flash_mode == 2 ? 0x9A0010 : 0xFFFFFF),
           !g_flash ? g_fg : (g_flash_mode == 2 ? 0xFFD9DC : 0x120A10), 12,
           night_flash_cb, NULL);
    y += 56;

    char v[64], nxt[12];
    if (alarm_next_str(nxt, sizeof(nxt)))
        snprintf(v, sizeof(v), "WAKE ALARM: %s", nxt);
    else
        snprintf(v, sizeof(v), "WAKE ALARM: NONE");
    ui_btn(b, 12, y, 386, 44, v, g_panel,
           alarm_next_str(nxt, sizeof(nxt)) ? g_accent : g_fg, 12, night_alarm_cb, NULL);
    y += 54;

    ui_btn(b, 12, y, 386, 44, "BREATHE   4 - 7 - 8", g_panel, g_fg, 12,
           night_breathe_cb, NULL);
    y += 54;

    ui_btn(b, 12, y, 386, 44, g_stealth ? "STEALTH: ON  /  TAP" : "STEALTH DISPLAY",
           g_stealth ? 0x1A0A0E : g_panel, g_stealth ? 0x8E2431 : g_fg, 12,
           night_stealth_cb, NULL);
    y += 54;

    snprintf(v, sizeof(v), "POWER PROFILE: %s", kPwrName[g_cfg.power]);
    ui_btn(b, 12, y, 386, 42, v, g_panel, g_fg, 10, night_power_cb, NULL);
    y += 54;

    lv_obj_t *sw = ui_frame(b, 12, y, 386, 102, g_panel, 12, g_panel, 1);
    ui_text(sw, "STOPWATCH", g_dim, &lv_font_montserrat_12, 16, 8, 0);
    g_sw_lbl = ui_text(sw, "00:00", g_fg, &lv_font_montserrat_24, 16, 28, 0);
    ui_btn(sw, 16, 66, 100, 28, g_sw_run ? "STOP" : "START", g_bg, g_accent, 8,
           night_timer_cb, (void *)(intptr_t)3);
    ui_btn(sw, 126, 66, 100, 28, "RESET", g_bg, g_dim, 8, night_timer_cb, (void *)(intptr_t)4);
    y += 114;

    lv_obj_t *tm = ui_frame(b, 12, y, 386, 118, g_panel, 12, g_panel, 1);
    ui_text(tm, "COUNTDOWN", g_dim, &lv_font_montserrat_12, 16, 8, 0);
    g_timer_lbl = ui_text(tm, "00:00", g_fg, &lv_font_montserrat_24, 16, 28, 0);
    ui_btn(tm, 130, 34, 76, 30, "5 MIN", g_bg, g_fg, 8, night_timer_cb, (void *)(intptr_t)0);
    ui_btn(tm, 214, 34, 98, 30, "FOCUS", g_bg, g_accent, 8, night_timer_cb, (void *)(intptr_t)1);
    ui_btn(tm, 16, 76, 354, 30, "STOP", g_bg, g_dim, 8, night_timer_cb, (void *)(intptr_t)2);
    y += 130;

    lv_obj_t *sn = ui_frame(b, 12, y, 386, 132, g_panel, 12, g_panel, 1);
    ui_text(sn, "IMU / ORIENTATION", g_dim, &lv_font_montserrat_12, 16, 8, 0);
    if (g_imu_ok) {
        char l1[64], l2[64];
        snprintf(l1, sizeof(l1), "A  %+.2f  %+.2f  %+.2f", g_imu[0], g_imu[1], g_imu[2]);
        snprintf(l2, sizeof(l2), "G  %+.1f  %+.1f  %+.1f", g_imu[3], g_imu[4], g_imu[5]);
        ui_text(sn, l1, g_fg, &lv_font_montserrat_14, 16, 34, 300);
        ui_text(sn, l2, g_fg, &lv_font_montserrat_14, 16, 58, 300);
        ui_text(sn, "No magnetometer: heading not available.",
                g_dim, &lv_font_montserrat_10, 16, 88, 320);
    } else {
        ui_text(sn, "Sensor driver not linked.", g_warn, &lv_font_montserrat_14, 16, 32, 330);
        ui_text(sn, "Set DCS_USE_SENSORLIB 1 with SensorLib installed. "
                    "No compass on this board.",
                g_dim, &lv_font_montserrat_10, 16, 56, 330);
    }
    ui_btn(sn, 250, 96, 120, 28, "READ IMU", g_bg, g_dim, 8, night_sensor_cb, NULL);
    y += 142;

    lv_obj_t *ph = ui_frame(b, 12, y, 386, 130, g_panel, 12, g_panel, 1);
    ui_text(ph, "PHONE LINK", g_dim, &lv_font_montserrat_12, 16, 8, 0);
    ui_text(ph, g_media.connected ? "CONNECTED" : "OFFLINE",
            g_media.connected ? g_accent : g_warn, &lv_font_montserrat_12, 270, 8, 0);
    ui_text(ph, g_media.title, g_fg, &lv_font_montserrat_14, 16, 32, 340);
    ui_text(ph, g_media.artist, g_dim, &lv_font_montserrat_12, 16, 54, 340);
    ui_btn(ph, 16, 82, 60, 32, "PREV", g_bg, g_fg, 8, night_media_cb, (void *)(intptr_t)MEDIA_PREV);
    ui_btn(ph, 86, 82, 200, 32, g_media.playing ? "PAUSE" : "PLAY", g_bg, g_accent, 8,
           night_media_cb, (void *)(intptr_t)MEDIA_TOGGLE);
    ui_btn(ph, 296, 82, 74, 32, "NEXT", g_bg, g_fg, 8, night_media_cb, (void *)(intptr_t)MEDIA_NEXT);
    y += 142;

    ui_btn(b, 12, y, 386, 46, "EXIT TO CHOOSER", g_panel, g_dim, 12, night_exit_cb, NULL);
    y += 58;
    ui_pad(b, y);
}

// ---- TRIPWIRE -------------------------------------------------------------
//  Motion watch on the existing six-axis IMU: arm it, leave the watch on a
//  bag or a door handle, and any movement beyond the threshold raises a
//  full-screen alert.  Disarms itself when it fires, is never persisted, and
//  is unavailable -- stated plainly -- when no IMU driver is linked.
static uint8_t  g_trip_on   = 0;      // deliberately not persisted
static uint8_t  g_trip_sens = 1;      // 0 low, 1 medium, 2 high
static uint8_t  g_trip_hits = 0;
static float    g_trip_base = 0.0f;
static uint32_t g_trip_last = 0;
static uint32_t g_trip_count = 0;

static void tripwire_fire() {
    g_trip_on = 0;
    g_trip_hits = 0;
    g_trip_count++;
    dcs_log(DCS_PATH_NIGHT, "tripwire fired (sensitivity %u)", (unsigned)g_trip_sens);
    if (g_screen_off) dcs_screen_set(false);
    lv_obj_t *c = modal_open("TRIPWIRE");
    ui_text(c, "MOVEMENT DETECTED", g_danger, &lv_font_montserrat_22, 12, 8, 340);
    char l[80];
    snprintf(l, sizeof(l), "threshold %s   /   %lu alert%s today",
             g_trip_sens == 0 ? "low" : g_trip_sens == 1 ? "medium" : "high",
             (unsigned long)g_trip_count, g_trip_count == 1 ? "" : "s");
    ui_text(c, l, g_dim, &lv_font_montserrat_12, 12, 46, 340);
    ui_text(c, "The watch has disarmed itself. Re-arm from OPS when you are ready.",
            g_fg, &lv_font_montserrat_14, 12, 76, 340);
    ui_btn(c, 12, 130, 366, 48, "ACKNOWLEDGE", g_panel, g_accent, 12, modal_close_ev, NULL);
}

static void tripwire_service() {
    if (!g_trip_on || !g_imu_ok) return;
    uint32_t now = millis();
    if (now - g_trip_last < 100U) return;
    g_trip_last = now;
    imu_read();
    float mag = sqrtf(g_imu[0] * g_imu[0] + g_imu[1] * g_imu[1] + g_imu[2] * g_imu[2]);
    if (g_trip_base <= 0.0f) { g_trip_base = mag; return; }
    float thr = g_trip_sens == 0 ? 0.30f : g_trip_sens == 1 ? 0.15f : 0.07f;
    if (fabsf(mag - g_trip_base) > thr) {
        if (g_trip_hits < 3) g_trip_hits++;
        if (g_trip_hits >= 3) tripwire_fire();
    } else if (g_trip_hits) {
        g_trip_hits--;
    }
}

static void night_trip_cb(lv_event_t *e) {
    (void)e;
    if (!g_imu_ok) return;
    g_trip_on = g_trip_on ? 0 : 1;
    g_trip_hits = 0;
    g_trip_base = 0.0f;
    dcs_log(DCS_PATH_NIGHT, "tripwire %s", g_trip_on ? "armed" : "disarmed");
    night_rebuild();
}
static void night_trip_sens_cb(lv_event_t *e) {
    (void)e;
    g_trip_sens = (uint8_t)((g_trip_sens + 1) % 3);
    g_trip_hits = 0;
    g_trip_base = 0.0f;
    night_rebuild();
}
static void night_dark_cb(lv_event_t *e) {
    (void)e;
    dcs_log(DCS_PATH_NIGHT, "manual go-dark");
    dcs_screen_set(true);
}

static void night_build_ops(lv_obj_t *b) {
    ui_body_scroll(b);
    int y = 10;

    lv_obj_t *t = ui_frame(b, 12, y, 386, 176, g_panel, 12, g_panel, 1);
    ui_text(t, "TRIPWIRE", g_dim, &lv_font_montserrat_10, 16, 8, 0);
    ui_text(t, !g_imu_ok ? "NO IMU" : (g_trip_on ? "ARMED" : "DISARMED"),
            !g_imu_ok ? g_warn : (g_trip_on ? g_accent : g_dim),
            &lv_font_montserrat_12, 290, 8, 0);
    ui_text(t, g_trip_on ? "Any movement raises a full-screen alert."
                         : "Arm to watch for movement.",
            g_fg, &lv_font_montserrat_14, 16, 30, 350);
    if (g_imu_ok) {
        ui_btn(t, 16, 58, 240, 42, g_trip_on ? "DISARM" : "ARM TRIPWIRE",
               g_trip_on ? 0x1A0A0E : g_bg, g_trip_on ? 0xD84A5C : g_accent, 10,
               night_trip_cb, NULL);
        char v[40];
        snprintf(v, sizeof(v), "SENS %s",
                 g_trip_sens == 0 ? "LOW" : g_trip_sens == 1 ? "MED" : "HIGH");
        ui_btn(t, 266, 58, 104, 42, v, g_bg, g_fg, 10, night_trip_sens_cb, NULL);
        snprintf(v, sizeof(v), "%lu alert%s this session", (unsigned long)g_trip_count,
                 g_trip_count == 1 ? "" : "s");
        ui_text(t, v, g_dim, &lv_font_montserrat_10, 16, 108, 300);
        ui_text(t, "Runs at 10 Hz only while armed; never persisted across reboot.",
                g_dim, &lv_font_montserrat_10, 16, 126, 350);
    } else {
        ui_text(t, "Needs the QMI8658 driver (DCS_USE_SENSORLIB 1). The watch cannot "
                   "detect motion without it, and no software fallback is claimed.",
                g_dim, &lv_font_montserrat_12, 16, 60, 350);
    }
    y += 188;

    ui_btn(b, 12, y, 386, 46, "GO DARK   (PANEL OFF, BOOT WAKES)", g_panel, g_fg, 12,
           night_dark_cb, NULL);
    y += 58;

    lv_obj_t *s = ui_frame(b, 12, y, 386, 150, g_panel, 12, g_panel, 1);
    ui_text(s, "NIGHT LOG", g_dim, &lv_font_montserrat_10, 16, 8, 0);
    const char *labels[3] = { "IN SHADOW", "SCREEN ON", "NOTIFICATIONS" };
    char vals[3][28], v[28];
    clock_fmt_dur((int64_t)(g_stat_shadow_ms / 1000UL), v, sizeof(v));
    snprintf(vals[0], sizeof(vals[0]), "%s", v);
    clock_fmt_dur((int64_t)(g_stat_on_ms / 1000UL), v, sizeof(v));
    snprintf(vals[1], sizeof(vals[1]), "%s", v);
    snprintf(vals[2], sizeof(vals[2]), "%u", (unsigned)g_stat_notif);
    for (int i = 0; i < 3; i++) {
        ui_text(s, labels[i], g_dim, &lv_font_montserrat_12, 16, 32 + i * 30, 0);
        lv_obj_t *lv = ui_text(s, vals[i], g_fg, &lv_font_montserrat_14, 190, 30 + i * 30, 170);
        lv_obj_set_style_text_align(lv, LV_TEXT_ALIGN_RIGHT, 0);
    }
    snprintf(v, sizeof(v), "%u shadow switch%s this session", (unsigned)g_shadow_sessions,
             g_shadow_sessions == 1 ? "" : "es");
    ui_text(s, v, g_dim, &lv_font_montserrat_10, 16, 122, 300);
    y += 162;

    char est[12];
    est_fmt(est, sizeof(est));
    ui_text(b, "Reserve is measured, not guessed: the estimate only appears after a "
               "10-minute window with at least 1% of discharge.",
            g_dim, &lv_font_montserrat_10, 16, y, 380);
    y += 34;
    ui_text(b, est, g_accent, &lv_font_montserrat_14, 16, y, 0);
    ui_text(b, "estimated runtime", g_dim, &lv_font_montserrat_10, 90, y + 4, 0);
    y += 30;
    ui_pad(b, y);
}

static void night_rebuild() {
    if (g_cur_os != 2) return;
    lv_obj_t *b = ui_begin(2, "NightOS", "SHADOW / UTILITY", kNightTabs, 4,
                           g_night_tab, night_tab_cb, ui_exit_cb);
    lv_obj_t *root = lv_obj_get_parent(b);
    if (g_night_tab == 0)      night_build_home(b);
    else if (g_night_tab == 1) night_build_shadow(b);
    else if (g_night_tab == 2) night_build_tools(b);
    else                       night_build_ops(b);
    ui_nav(root);
}

// ---------------------------------------------------------------------------
//  18.5  SLEEP  (light sleep with the panel off, deep sleep when idle)
// ---------------------------------------------------------------------------
//  LauncherOS behaves the same way: the panel-off state sleeps the SoC and
//  BOOT wakes it.  The difference here is that the wake source also includes
//  the timer, so an alarm or a countdown can bring the watch back on its own.
//  Deep sleep is a reboot: RAM is lost, so everything is saved first and the
//  experience is reopened from RTC memory on wake.

static bool next_wake_epoch(time_t *out) {
    if (!out || !clock_valid()) return false;
    time_t now = time(NULL);
    time_t best = 0;
    struct tm lt; localtime_r(&now, &lt);

    for (int i = 0; i < ALARM_MAX; i++) {
        if (!g_alarm[i].on) continue;
        for (int d = 0; d < 8; d++) {
            struct tm cand = lt;
            cand.tm_hour = g_alarm[i].hh;
            cand.tm_min = g_alarm[i].mm;
            cand.tm_sec = 0;
            cand.tm_mday += d;
            cand.tm_isdst = -1;
            time_t t = mktime(&cand);
            if (t <= now) continue;
            struct tm c2; localtime_r(&t, &c2);
            if (!(g_alarm[i].days & (uint8_t)(1 << c2.tm_wday))) continue;
            if (!best || t < best) best = t;
            break;
        }
    }

    if (g_night.sched_on) {
        for (int d = 0; d < 8; d++) {
            for (int which = 0; which < 2; which++) {
                struct tm cand = lt;
                cand.tm_hour = which ? g_night.sched_end : g_night.sched_start;
                cand.tm_min = 0;
                cand.tm_sec = 0;
                cand.tm_mday += d;
                cand.tm_isdst = -1;
                time_t t = mktime(&cand);
                if (t <= now + 60) continue;
                struct tm c2; localtime_r(&t, &c2);
                if (!(g_night.days & (uint8_t)(1 << c2.tm_wday))) continue;
                if (!best || t < best) best = t;
            }
        }
    }

    if (g_cd_run) {
        time_t end = now + (time_t)(cd_remain_ms() / 1000UL);
        if (end > now && (!best || end < best)) best = end;
    }

    if (!best) return false;
    *out = best;
    return true;
}

static void dcs_sleep_now(const char *why) {
    dcs_log(DCS_PATH_SHARED, "deep sleep: %s", why ? why : "manual");
    time_publish("sleep", false);
    dcs_save_all();
    dcs_boot_guard_success();       // waking must not count as a failed launch
    g_resume_os = (g_cur_os >= 0 && g_cur_os <= 2) ? (uint8_t)g_cur_os : 0xFE;

    g_gfx->fillScreen(RGB565_BLACK);
    g_gfx->setBrightness(0);
    g_cur_brightness = 0;

    esp_sleep_pd_config(ESP_PD_DOMAIN_RTC_PERIPH, ESP_PD_OPTION_ON);
    esp_sleep_enable_ext1_wakeup(1ULL << WC_BOOT_BTN, ESP_EXT1_WAKEUP_ANY_LOW);

    time_t wake = 0;
    if (next_wake_epoch(&wake)) {
        int64_t d = (int64_t)wake - (int64_t)time(NULL);
        if (d > 1) d -= 1;               // land a second early, never late
        if (d < 1) d = 1;
        if (d > 7LL * 86400LL) d = 7LL * 86400LL;
        esp_sleep_enable_timer_wakeup((uint64_t)d * 1000000ULL);
    }

    delay(120);
    esp_deep_sleep_start();             // never returns
}

//  Deep sleep only when nothing the user asked for is in flight: an alarm
//  ringing, a countdown, an open session overlay, the flashlight, or charging.
static void sleep_service() {
    if (!g_cfg.sleep_min || !g_screen_off) return;
    if (g_batt_ch || g_cd_run) return;
    if (g_ring != -1 || g_stealth || g_breathe || g_flash) return;
    if (millis() - g_last_touch_ms < (uint32_t)g_cfg.sleep_min * 60000UL) return;
    dcs_sleep_now("inactivity");
}

// ---------------------------------------------------------------------------
//  19.  DIAGNOSTICS / POWER LAB
// ---------------------------------------------------------------------------
static const char *reset_reason_str() {
    switch (esp_reset_reason()) {
        case ESP_RST_POWERON:   return "POWERON";
        case ESP_RST_SW:        return "SOFTWARE";
        case ESP_RST_PANIC:     return "PANIC";
        case ESP_RST_TASK_WDT:  return "TASK_WDT";
        case ESP_RST_INT_WDT:   return "INT_WDT";
        case ESP_RST_BROWNOUT:  return "BROWNOUT";
        case ESP_RST_DEEPSLEEP: return "DEEPSLEEP";
        default:                return "OTHER";
    }
}

static void powerlab_snapshot() {
    char line[240];
    snprintf(line, sizeof(line),
             "%lu|%s|%s|%d|%d|%d|%u|%u|%u|%lu|%s|%d\n",
             (unsigned long)time(NULL),
             g_cur_os == 0 ? "NoirOS" : g_cur_os == 1 ? "CrownOS" :
             g_cur_os == 2 ? "NightOS" : "chooser",
             kPwrName[g_cfg.power], g_batt_pct, g_batt_mv, g_batt_ch ? 1 : 0,
             (unsigned)g_cur_brightness,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
             (unsigned long)(millis() / 1000UL), reset_reason_str(),
             g_media.connected ? 1 : 0);
    const char *dir = g_cur_os == 0 ? DCS_PATH_NOIR : g_cur_os == 1 ? DCS_PATH_CROWN :
                      g_cur_os == 2 ? DCS_PATH_NIGHT : DCS_PATH_SHARED;
    char path[96];
    snprintf(path, sizeof(path), "%s/logs/power.log", dir);
    fs_append(path, line);
    dcs_log(dir, "powerlab snapshot written");
}

static void diag_snapshot_cb(lv_event_t *e) {
    (void)e;
    powerlab_snapshot();
    lv_obj_t *c = modal_open("SNAPSHOT SAVED");
    ui_text(c, "Written to the active experience's logs/power.log.", g_fg,
            &lv_font_montserrat_14, 12, 8, 340);
    ui_text(c, "One line: epoch | OS | profile | battery | mV | charging | "
               "brightness | heap | PSRAM | uptime | reset | link",
            g_dim, &lv_font_montserrat_12, 12, 46, 340);
}

static void show_diagnostics() {
    lv_obj_t *c = modal_open("DIAGNOSTICS");
    char l[112];
    int y = 4;
    ui_text(c, "DUALCORE SPECIAL EDITIONS", g_accent, &lv_font_montserrat_12, 10, y, 0);
    y += 18;
    snprintf(l, sizeof(l), "module %s   manifest api %d", DCS_VERSION, DCS_MANIFEST_API);
    ui_text(c, l, g_fg, &lv_font_montserrat_12, 10, y, 350); y += 22;
    snprintf(l, sizeof(l), "experience %s", g_cur_os == 0 ? "NoirOS" :
             g_cur_os == 1 ? "CrownOS" : g_cur_os == 2 ? "NightOS" : "chooser");
    ui_text(c, l, g_fg, &lv_font_montserrat_12, 10, y, 350); y += 18;
    snprintf(l, sizeof(l), "power %s   brightness %u", kPwrName[g_cfg.power],
             (unsigned)g_cur_brightness);
    ui_text(c, l, g_fg, &lv_font_montserrat_12, 10, y, 350); y += 18;
    char est[12];
    est_fmt(est, sizeof(est));
    snprintf(l, sizeof(l), "battery %d%%  %d mV  %s  est %s", g_batt_pct, g_batt_mv,
             g_batt_ch ? "charging" : "discharging", est);
    ui_text(c, l, g_fg, &lv_font_montserrat_12, 10, y, 350); y += 18;
    int alarms_on = 0;
    for (int i = 0; i < ALARM_MAX; i++) if (g_alarm[i].on) alarms_on++;
    snprintf(l, sizeof(l), "alarms %d on   auto sleep %u min   wake BOOT+timer",
             alarms_on, (unsigned)g_cfg.sleep_min);
    ui_text(c, l, g_fg, &lv_font_montserrat_12, 10, y, 350); y += 18;
    if (g_wx.valid)
        snprintf(l, sizeof(l), "weather %dC %s  H%d L%d", (int)g_wx.temp, g_wx.cond,
                 (int)g_wx.hi, (int)g_wx.lo);
    else
        snprintf(l, sizeof(l), "weather no data (phone link supplies it)");
    ui_text(c, l, g_fg, &lv_font_montserrat_12, 10, y, 350); y += 18;
    snprintf(l, sizeof(l), "heap %u   psram %u",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    ui_text(c, l, g_fg, &lv_font_montserrat_12, 10, y, 350); y += 18;
    snprintf(l, sizeof(l), "uptime %lu s   reset %s",
             (unsigned long)(millis() / 1000UL), reset_reason_str());
    ui_text(c, l, g_fg, &lv_font_montserrat_12, 10, y, 350); y += 18;
    snprintf(l, sizeof(l), "SD %s   inbox %d (%d unread)   link %s",
             g_sd_ok ? "mounted" : "absent", g_nf_n, notif_unread(),
             g_media.connected ? "up" : "down");
    ui_text(c, l, g_fg, &lv_font_montserrat_12, 10, y, 350); y += 24;

    // ---- battery history: the same shape every run, so profiles compare ----
    ui_text(c, "BATTERY HISTORY   one sample / 5 min", g_dim,
            &lv_font_montserrat_10, 10, y, 300);
    y += 16;
    if (g_batt_hist_n == 0) {
        ui_text(c, "No samples yet.", g_dim, &lv_font_montserrat_12, 10, y, 300);
        y += 22;
    } else {
        int shown = g_batt_hist_n < 64 ? g_batt_hist_n : 64;
        int start = g_batt_hist_n - shown;
        int gx = 10 + (370 - shown * 5) / 2;
        if (gx < 10) gx = 10;
        ui_box(c, gx, y + 46, shown * 5, 1, g_panel, 0);      // baseline
        for (int i = 0; i < shown; i++) {
            int pct = g_batt_hist[start + i];
            int h = pct * 44 / 100;
            if (h < 2) h = 2;
            uint32_t col = pct < 20 ? g_danger : (pct < 40 ? g_warn : g_accent);
            ui_box(c, gx + i * 5, y + 46 - h, 4, h, col, 1);
        }
        y += 54;
        snprintf(l, sizeof(l), "min %u%%   max %u%%   last %d%%   %s",
                 (unsigned)g_batt_hist_min, (unsigned)g_batt_hist_max, g_batt_pct, est);
        ui_text(c, l, g_fg, &lv_font_montserrat_12, 10, y, 350);
        y += 24;
    }

    ui_text(c, "SCHEDULE", g_dim, &lv_font_montserrat_10, 10, y, 0); y += 16;
    if (g_night.sched_on) {
        snprintf(l, sizeof(l), "%02u:00 to %02u:00, days mask 0x%02X",
                 (unsigned)g_night.sched_start, (unsigned)g_night.sched_end,
                 g_night.days);
    } else {
        snprintf(l, sizeof(l), "off");
    }
    ui_text(c, l, g_fg, &lv_font_montserrat_12, 10, y, 350); y += 24;

    ui_btn(c, 10, y, 170, 38, "WRITE SNAPSHOT", g_panel, g_accent, 10, diag_snapshot_cb, NULL);
    ui_btn(c, 190, y, 170, 38, "RETURN", g_panel, g_dim, 10, modal_close_ev, NULL);
    y += 50;
    ui_text(c, "Notification contents are never written to logs.", g_dim,
            &lv_font_montserrat_10, 10, y, 350);
}

static void chooser_diag_cb(lv_event_t *e) {
    (void)e;
    show_diagnostics();
}

// ---------------------------------------------------------------------------
//  20.  BOOT SPLASH, BOOT BUTTON, SETUP, LOOP
// ---------------------------------------------------------------------------
static void dcs_boot_splash() {
    g_gfx->fillScreen(RGB565_BLACK);
    g_gfx->setTextSize(2);
    g_gfx->setTextColor(g_gfx->color565(0xC9, 0xA4, 0x4C));
    g_gfx->setCursor(96, 214);
    g_gfx->print("DUALCORE");
    g_gfx->setTextSize(1);
    g_gfx->setTextColor(g_gfx->color565(0x88, 0x88, 0x88));
    g_gfx->setCursor(112, 250);
    g_gfx->print("SPECIAL EDITIONS  ");
    g_gfx->setTextColor(g_gfx->color565(0x6E, 0x9B, 0xD1));
    g_gfx->print("v" DCS_VERSION);
    g_gfx->setBrightness(180);
    g_cur_brightness = 180;
    delay(420);
    g_gfx->setBrightness(0);
    g_cur_brightness = 0;
}

static void dcs_boot_poll() {
    static uint32_t hold = 0;
    if (g_boot_require_release) {
        hold = 0;
        if (!dcs_boot_pressed()) g_boot_require_release = false;
        return;
    }
    if (dcs_boot_pressed()) {
        if (!hold) hold = millis();
        else if (millis() - hold > DCS_BOOT_LONGPRESS_MS) {
            hold = 0;
            dcs_launcher_return_now();   // never returns
        }
    } else {
        hold = 0;
    }
}

void setup() {
    Serial.begin(115200);

    // A deep-sleep wake is a reboot: reopen what the user was in instead of
    // dropping them back on the chooser.  The flag lives in RTC memory.
    esp_sleep_wakeup_cause_t wake_cause = esp_sleep_get_wakeup_cause();
    bool resumed = (wake_cause == ESP_SLEEP_WAKEUP_EXT1 ||
                    wake_cause == ESP_SLEEP_WAKEUP_TIMER);
    uint8_t resume_os = g_resume_os;
    g_resume_os = 0xFF;

    dcs_boot_guard_enter();          // 3-strike guard, before anything can hang
    time_restore_from_nvs();         // shared time contract, before any UI

    dcs_hw_boot();
    pinMode(WC_BOOT_BTN, INPUT_PULLUP);   // clean idle level for light sleep
    pinMode(WC_TP_INT, INPUT_PULLUP);

    if (resumed) {
        g_gfx->fillScreen(RGB565_BLACK);
        g_gfx->setBrightness(0);
        g_cur_brightness = 0;
    } else {
        dcs_boot_splash();
    }
    dcs_data_init();
    dcs_cfg_load();
    if (g_cfg.wall) { photos_scan(); if (!wall_load(g_cfg.wall - 1)) g_cfg.wall = 0; }

    bool launched = false;
    if (resumed && resume_os <= 2) {
        char err[64];
        if (manifest_validate(resume_os, err, sizeof(err))) {
            g_boot_resume = true;        // wake straight into the OS, short form
            os_enter((int)resume_os);
            launched = true;
        }
    }
    if (!launched) show_chooser();
    lv_timer_create(ui_tick, 500, NULL);
    g_cur_brightness = 0;
    g_gfx->setBrightness(0);
    dcs_fade_to(dcs_brightness_base(), 260);
    dcs_boot_guard_success();        // a usable UI exists -> clear the strikes

    dcs_log(DCS_PATH_SHARED, "boot ok v%s heap=%u psram=%u sd=%d",
            DCS_VERSION,
            (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
            (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
            g_sd_ok ? 1 : 0);
}

void loop() {
    dcs_service();            // LVGL tick, touch/PMU, screen-off, fades
    stat_service();           // today's counters (screen-on, focus, shadow)
    tripwire_service();       // NightOS motion watch, only while armed
    dcs_boot_poll();          // BOOT 1.5 s -> LauncherOS
    net_service();             // Wi-Fi NTP top-up, only ever outside LVGL events
    phone_service();          // link state (demo or BLE)
    timer_service();          // countdown completion cue
    alarm_check();            // shared alarms ring over any experience
    crown_event_service();    // opt-in 15-minute CrownOS event alerts
    night_schedule_service(); // scheduled Shadow Mode

    // One battery poller for the whole module (never per-screen).
    static uint32_t last_batt = 0;
    uint32_t now = millis();
    if (g_has_pmu && now - last_batt >= 5000U) {
        last_batt = now;
        int pct = g_pmu.getBatteryPercent();
        g_batt_pct = (pct > 100) ? -1 : pct;
        g_batt_ch = g_pmu.isCharging();
        g_batt_mv = g_pmu.getBattVoltage();
        if (g_batt_pct >= 0 && !g_batt_boot_ok) {
            g_batt_boot = (uint8_t)g_batt_pct;   // "started the day at"
            g_batt_boot_ok = 1;
        }
        power_auto_check();
        batt_hist_sample();
    }
    batt_estimate_update();
    sleep_service();          // deep sleep once idle, if enabled

    // Light sleep while the panel is off, as LauncherOS does: BOOT wakes the
    // SoC instantly; TP_INT is not reliably RTC-capable on every S3 board, so
    // a 250 ms timer polls it instead.
    if (g_screen_off && !g_fading && g_ring == -1 && !g_breathe && !g_stealth &&
        !g_flash) {
#if DCS_USE_BLE
        if (!g_media.connected)
#endif
        {
            esp_sleep_enable_ext1_wakeup(1ULL << WC_BOOT_BTN, ESP_EXT1_WAKEUP_ANY_LOW);
            esp_sleep_enable_timer_wakeup(250ULL * 1000ULL);
            esp_light_sleep_start();
            esp_sleep_wakeup_cause_t why = esp_sleep_get_wakeup_cause();
            if (why == ESP_SLEEP_WAKEUP_EXT1 ||
                (why == ESP_SLEEP_WAKEUP_TIMER && digitalRead(WC_TP_INT) == LOW)) {
                g_last_touch_ms = millis();
                dcs_screen_set(false);
            }
        }
    }
    delay(2);
}
