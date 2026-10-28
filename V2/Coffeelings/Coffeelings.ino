// ============================================================================
//  Coffeelings.ino  --  a coffee mood journal for the wrist
//  ---------------------------------------------------------------------------
//  Target : Waveshare ESP32-S3 Touch AMOLED 2.06 (410 x 502 CO5300 / FT3168 /
//           AXP2101), LVGL 8.4, launched from LauncherOS (or run standalone).
//  Build  : ESP32-S3 Dev Module, 16 MB flash, OPI PSRAM,
//           Partition Scheme "Minimal SPIFFS (large APP)", USB CDC on boot.
//           Libraries: LVGL 8.4, Arduino_GFX_Library, Arduino_DriveBus_Library,
//           XPowersLib (all present in the LauncherOS toolchain).
//
//  WHAT IT IS
//    A tiny port of the Coffeelings Chrome extension: every day you pick one of
//    five coffee-roast moods, optionally write a line about the day, and the
//    month turns into a warm little grid of colours.
//
//      * HOME face   - the time, today's date, today's roast and streak.  Tap
//                      anywhere to open the journal.
//      * MONTH view  - 7 x 6 grid, one square per day, coffee-roast palette
//                      (cinnamon -> espresso), today outlined, note dots.
//      * DAY sheet   - tap a day: pick a roast, add a note, or clear the colour.
//      * YEAR view   - the extension's signature 12 x 31 heatmap, tap a month
//                      to jump back into it.
//      * NOTES       - one short text file per day on the SD card
//                      (/WATCH/Coffeelings/NOTES/YYYY-MM-DD.txt), edited with LVGL's
//                      on-screen keyboard.
//      * STATS       - days logged, streak, favourite roast, and a .txt export
//                      of the whole year (the extension's "save journal").
//
//  SD LAYOUT  (all of it created automatically on boot, /WATCH style)
//      /WATCH/Coffeelings/NOTES/YYYY-MM-DD.txt
//      /WATCH/Coffeelings/coffeelings-YYYY.txt   (the exported journal)
//
//  DATA / TIME
//    * Moods live in NVS namespace "coffeelings" as one 366-byte blob per year
//      ("y2026"), so a year survives reboots and costs a single NVS entry.
//      Years saved under the old "cofflings" namespace are copied across once
//      at boot, and the old /WATCH journal folder is renamed, so the rename to
//      Coffeelings does not orphan anything already written.
//    * Notes live on the SD card (short files, no NVS pressure on the 20 KB
//      nvs partition this watch ships with).
//    * The wall clock comes from LauncherOS's shared "rtcsave" snapshot
//      (time contract v2, inlined below) - the same block every app under
//      V2/ carries.  Without LauncherOS the sketch falls back to its build
//      date so the grid is still usable.
//
//  BOOT / EXIT  (matches LauncherOS + GameOS protocols)
//    * Panel starts at brightness 0, the boot splash fades up, progress lines
//      run, then a 260 ms fade into the home face.
//    * Hold BOOT (GPIO0) for 1.5 s anywhere -> save, publish the shared clock,
//      repoint the bootloader at the LauncherOS partition and restart.  A
//      3-strike boot guard returns to LauncherOS if this app ever crash-loops.
//    * Screen sleeps after 15 s idle (never while writing a note); touch or
//      BOOT wakes it instantly, short power press toggles the panel.
//    * After 30 s with the screen off it drops into DEEP SLEEP, exactly like
//      LauncherOS's Sleep Now: fade out, save, publish the clock, keep the
//      RTC domain alive (GPIO0 BOOT + GPIO10 side button wake, EXT1 low).
//      A deep-sleep wake reboots straight to the home face - the splash and
//      its progress steps are skipped - so the watch still feels instant.
//    * The shared time contract is republished every 5 minutes and on exit.
//
//  STABILITY  (v1.6 - "never free anything LVGL is using")
//    * Screens are no longer created and deleted at all.  There is one
//      persistent root per view (g_screens[]), exactly like LauncherOS's
//      screens[] array; a view is rebuilt by emptying its root, never by
//      freeing it.  A screen root LVGL animates therefore always exists, which
//      removes the whole use-after-free family that rebooted the watch.
//    * Screen content is rebuilt only from loop(), never from an LVGL event.
//      build_screen() queues the request while lv_timer_handler() is running
//      (g_in_pump) and loop() performs it on the next pass -- LauncherOS's
//      pending-screen pattern, and it keeps the build off LVGL's call stack.
//    * A view that is mid-fade (disp->prev_scr) is never emptied; the request
//      waits until the fade has finished.
//    * RTC breadcrumbs: every boot names the stage (and source line) the
//      previous run died in, on the splash, on the info screen and on serial.
//      A crash is no longer invisible, so it can be diagnosed without a PC.
//    * After a crash boot the cross-fade is skipped once (hard cut), so a fade
//      problem can never trap the watch in a reboot loop.
//
//  STABILITY  (v1.5, after a Guru-Meditation reboot on every touch)
//    * A screen may only be freed once it is neither the active nor LVGL's
//      previous (animating) screen.  Freeing the faded-out screen mid-fade is a
//      use-after-free that reboots the watch.  (v1.6 went further and stopped
//      freeing screens at all.)
//    * Nothing rebuilds a screen from inside a callback.  touch_cb() queues the
//      gesture and loop() performs it, exactly like LauncherOS's pending-swipe.
//    * The year heatmap is one canvas, not 372 objects: the object version ran
//      the 64 KB LVGL heap dry and the task watchdog rebooted.
//    * esp_reset_reason() is logged on every boot and shown on the info screen
//      ("boot PANIC / task-wdt"), so a crash is no longer invisible.
//
//  This file is the whole app.  It needs a lv_conf.h from the repo (LVGL 8.4,
//  Montserrat 12/14/16/18/20/22/28/48 enabled) - exactly like every other V2
//  app.  48 is the home-face clock, and the shipped lv_conf.h has it on.
// ============================================================================

#include <Arduino.h>
#include <Wire.h>
#include <FS.h>
#include <SD_MMC.h>
#include <Preferences.h>
#include <Arduino_GFX_Library.h>
#include <Arduino_DriveBus_Library.h>
#include <lvgl.h>
#define XPOWERS_CHIP_AXP2101
#include <XPowersLib.h>
#include <esp_ota_ops.h>
#include <esp_partition.h>
#include <esp_sleep.h>
#include <esp_system.h>
#include <esp_attr.h>
#include <esp_heap_caps.h>
#include <esp32-hal-cpu.h>
#include <time.h>
#include <sys/time.h>
#include <string.h>

// The loop task carries LVGL's event dispatch and draw path *and* the SD card's
// VFS/FAT call chains.  The 8 KB default is not enough for that combination: the
// first month build (31 note checks inside an LVGL callback) overflows it, and
// a stack overflow reboots the watch with no message at all.
SET_LOOP_TASK_STACK_SIZE(16 * 1024);

#if LVGL_VERSION_MAJOR != 8 || LVGL_VERSION_MINOR != 4
#error "Coffeelings requires LVGL 8.4.x. Do not compile it with LVGL 9."
#endif

// ============================================================================
//  01. BUILD CONFIGURATION
// ============================================================================
#define COFFEELINGS_VERSION          "1.7"
#define COFFEELINGS_POWER_SAVE       1
#define COFFEELINGS_BRIGHTNESS       140        // 20..255, AMOLED (LauncherOS default)
#define COFFEELINGS_SCREEN_TIMEOUT_MS 15000UL
#define COFFEELINGS_WAKE_GRACE_MS    2000UL
#define COFFEELINGS_DEEP_SLEEP_MS    30000UL   // 30 s with the screen off -> deep sleep
#define COFFEELINGS_WAKE_FADE_MS      160      // quicker fade after a deep-sleep wake
#define COFFEELINGS_BOOT_HOLD_MS     1500UL
#define COFFEELINGS_FADE_MS           120      // screen cross-fade (old screen must outlive it)
#define COFFEELINGS_SLEEP_POLL_MS    250UL
#define COFFEELINGS_DEFAULT_YEAR     2026

// ============================================================================
//  02. HARDWARE (Waveshare ESP32-S3 Touch AMOLED 2.06)
// ============================================================================
#define LCD_WIDTH   410
#define LCD_HEIGHT  502
#define LCD_SDIO0   4
#define LCD_SDIO1   5
#define LCD_SDIO2   6
#define LCD_SDIO3   7
#define LCD_RESET   8
#define TP_RESET    9
#define PWR_BTN_PIN 10
#define LCD_SCLK    11
#define LCD_CS      12
#define IIC_SCL     14
#define IIC_SDA     15
#define TP_INT      38
#define SDMMC_CMD   1
#define SDMMC_CLK   2
#define SDMMC_DATA  3
#define BOOT_BTN    0
#define FT3168_DEVICE_ADDRESS 0x38

// ============================================================================
//  03. THE COFFEE PALETTE  (taken from cnnmon/coffeelings' popup.css)
// ============================================================================
#define C_BG        0x140E0B   // dark roast backdrop
#define C_SURFACE   0x241813
#define C_SURFACE2  0x2F1F18
#define C_LINE      0x3E2A21
#define C_CREAM     0xF4E3D0
#define C_MUTED     0xA28A7B
#define C_ACCENT    0xE9BE9B   // "lightvanilla"
#define C_GOLD      0xD9A066

#define C_AMAZING   0xBE7261   // cinnamon
#define C_OK        0x9F5754   // light
#define C_TIRED     0x7D4448   // city
#define C_SAD       0x684242   // dark
#define C_STRESSED  0x492233   // italian
#define C_EMPTY     0x4A3628

#define MOOD_COUNT  5

static const uint32_t MOOD_COLOR[MOOD_COUNT] = {
    C_AMAZING, C_OK, C_TIRED, C_SAD, C_STRESSED
};
static const char *MOOD_NAME[MOOD_COUNT] = {
    "Amazing", "Ok", "Tired", "Sad", "Stressed"
};

// ============================================================================
//  04. SHARED TIME BLOCK v2  (inlined verbatim from LauncherOS, see
//      TIME_CONTRACT.md; only the two defines above the block differ per app)
// ============================================================================
#define TIME_SERIAL Serial
#define TIME_SRC_ID TIME_SRC_MODULE

#define TIME_VALID_MIN      1700000000UL   // ~2023-11-14: "the RTC has been set"
#define TIME_ADOPT_SLACK_S  2L             // ignore sub-2 s snapshot/rtc skew
#define TIME_SRC_LAUNCHER   0
#define TIME_SRC_MODULE     1

static int32_t  g_tp_gmt   = 0;   // local offset from UTC, in seconds
static uint32_t g_tp_seq   = 0;   // last seq we read or wrote
static bool     g_tp_valid = false;   // a plausible wall clock is available

static void time_apply_tz(int32_t gmt_sec) {
    long mins = -(gmt_sec / 60);
    long a    = labs(mins);
    char tz[24];
    snprintf(tz, sizeof(tz), "UTC%c%ld:%02ld", mins < 0 ? '-' : '+', a / 60, a % 60);
    setenv("TZ", tz, 1);
    tzset();
    g_tp_gmt = gmt_sec;
}

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

static void time_publish(const char *why, bool manual) {
    time_t now; time(&now);
    if (now < (time_t)TIME_VALID_MIN) return;      // RTC not set yet
    Preferences p;
    if (!p.begin("rtcsave", false)) return;
    uint32_t prev     = p.getULong("t", 0);
    uint8_t  prev_man = p.getUChar("man", 0);
    bool machine_moved_it = !prev ||
                            labs((long)now - (long)prev) > TIME_ADOPT_SLACK_S;
    uint8_t man = (manual || (prev_man && !machine_moved_it)) ? 1 : 0;
    p.putULong("t",   (uint32_t)now);
    p.putLong ("gmt", g_tp_gmt);
    p.putUChar("man", man);
    p.putUChar("src", TIME_SRC_ID);
    g_tp_seq = p.getULong("seq", 0) + 1;
    p.putULong("seq", g_tp_seq);
    p.end();
    TIME_SERIAL.printf("[time] publish t=%lu gmt=%ld seq=%lu man=%d (%s)\n",
                       (unsigned long)now, (long)g_tp_gmt,
                       (unsigned long)g_tp_seq, man, why);
}

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
    if (snap_seq == 0 && snap_t) snap_seq = 1;

    uint8_t d = time_decide(rtc_now, snap_t, snap_man);
    if (d == TIME_DEC_ADOPT) {
        struct timeval tv = { (time_t)snap_t, 0 };
        settimeofday(&tv, NULL);
        g_tp_valid = true;
        TIME_SERIAL.printf("[time] adopted NVS t=%lu (rtc was %ld man=%d)\n",
                           (unsigned long)snap_t, (long)rtc_now, snap_man);
    } else if (d == TIME_DEC_KEEP_RTC) {
        g_tp_valid = true;
        TIME_SERIAL.printf("[time] RTC live t=%ld -- NVS kept as fallback\n",
                           (long)rtc_now);
    } else {
        TIME_SERIAL.println("[time] no persisted clock -- using build date");
    }

    if (snap_t) time_apply_tz(snap_gmt);
    g_tp_seq = snap_seq;
    time_publish("boot", false);
}
// ===== END SHARED TIME BLOCK v2 =============================================

// ============================================================================
//  05. STATE
// ============================================================================
static Arduino_DataBus *g_bus = new Arduino_ESP32QSPI(
    LCD_CS, LCD_SCLK, LCD_SDIO0, LCD_SDIO1, LCD_SDIO2, LCD_SDIO3);
static Arduino_GFX *g_gfx = new Arduino_CO5300(
    g_bus, LCD_RESET, 0, LCD_WIDTH, LCD_HEIGHT, 22, 0, 0, 0);

static std::shared_ptr<Arduino_IIC_DriveBus> g_iic =
    std::make_shared<Arduino_HWIIC>(IIC_SDA, IIC_SCL, &Wire);
void coff_touch_irq();
static std::unique_ptr<Arduino_IIC> g_touch(new Arduino_FT3x68(
    g_iic, FT3168_DEVICE_ADDRESS, DRIVEBUS_DEFAULT_VALUE, TP_INT, coff_touch_irq));

static XPowersPMU g_pmu;
static Preferences g_prefs;

static lv_disp_draw_buf_t g_draw_buf;
static lv_color_t *g_lvbuf = nullptr;
static lv_disp_drv_t g_disp_drv;
static lv_indev_drv_t g_indev_drv;

enum ScreenID : uint8_t {
    SCREEN_HOME, SCREEN_MONTH, SCREEN_YEAR, SCREEN_NOTE, SCREEN_INFO,
    SCREEN_COUNT
};
static const char *const SCREEN_NAME[SCREEN_COUNT] =
    {"home", "month", "year", "note", "info"};

// One persistent root per view, created on first use and NEVER deleted.
// This is LauncherOS's screens[] array: the object LVGL fades between always
// exists, so no pointer it holds can go stale.
static lv_obj_t *g_screens[SCREEN_COUNT] = {nullptr};
static lv_obj_t *splash_screen = nullptr;

static lv_obj_t *g_screen = nullptr;
static lv_obj_t *g_sheet = nullptr;
static lv_obj_t *g_scrim = nullptr;
static lv_obj_t *g_note_ta = nullptr;
static lv_obj_t *g_batt_lbl = nullptr;
static lv_obj_t *g_home_time = nullptr;
static lv_obj_t *g_home_date = nullptr;
static lv_obj_t *g_toast_box = nullptr;
static lv_timer_t *g_toast_timer = nullptr;

static ScreenID g_view = SCREEN_HOME;
static int g_year  = COFFEELINGS_DEFAULT_YEAR;
static int g_month = 10;
static int g_sel_day = 0;
static int g_edit_y = 0, g_edit_m = 0, g_edit_d = 0;

static uint8_t  g_moods[366];         // 0 = no entry, 1..5 = roast
static struct tm g_today = {};
static bool     g_clock_valid = false;

static bool g_sd_ok = false, g_pmu_ok = false, g_touch_ok = false;
static bool g_screen_off = false, g_touch_active = false, g_touch_block = false;
static bool g_returning = false, g_boot_release = false;
static uint8_t g_brightness = COFFEELINGS_BRIGHTNESS, g_cur_brightness = 0;
static uint32_t g_last_touch_ms = 0, g_last_wake_ms = 0;
static uint32_t g_last_batt_ms = 0, g_last_tick_ms = 0;
static uint32_t g_last_time_pub_ms = 0;
static uint32_t g_screen_off_since = 0;
static bool g_boot_guard_armed = false;
static bool g_fast_wake = false;
static int g_battery = -1; static bool g_charging = false;
static int32_t g_touch_x = 0, g_touch_y = 0, g_start_x = 0, g_start_y = 0;
static char g_last_boot[16] = "unknown";   // why the previous run ended
static bool g_boot_fault = false;           // that reason was a crash/WDT
static uint32_t g_touch_start_ms = 0;
static lv_indev_t *g_indev = nullptr;

// ---- pending navigation ----------------------------------------------------
//  Moved out of the touch callback on purpose: touch_cb() runs *inside*
//  lv_timer_handler(), so rebuilding or replacing a screen from there
//  re-enters LVGL while it is still walking its input state machine.  The
//  callback only records the intent; loop() performs it one pump later.
//  (LauncherOS does the same thing with g_pending_swipe.)
enum NavAction : uint8_t {
    NAV_NONE = 0, NAV_HOME, NAV_PREV_MONTH, NAV_NEXT_MONTH, NAV_LAUNCHER
};
static volatile NavAction g_nav = NAV_NONE;

// ---- pump guard ------------------------------------------------------------
//  True while lv_timer_handler() is executing.  Every UI mutation that could
//  free objects is refused (and queued) while this is set, because LVGL is
//  walking its own event/input/render state machines at that moment.
static bool g_in_pump = false;
static ScreenID g_pending_view = SCREEN_HOME;
static bool g_pending_valid = false;
static lv_timer_t *g_idle_clean_timer = nullptr;
static void trash_flush();      // day-sheet teardown, defined with the UI helpers

// ---- last-breath breadcrumbs (RTC memory, survives a panic) ----------------
//  A Guru Meditation on a watch with no PC attached is invisible: the only
//  symptom is "it just reboots".  Every risky step stamps a stage + source
//  line into RTC_NOINIT memory, the next boot reads it back, and the splash
//  plus the info screen spell out where the previous run died.
enum CrumbStage : uint8_t {
    CRUMB_NONE = 0, CRUMB_BOOT, CRUMB_HW, CRUMB_LVGL, CRUMB_SPLASH, CRUMB_STORAGE,
    CRUMB_MOODS, CRUMB_BUILD, CRUMB_SHEET, CRUMB_TOUCH, CRUMB_NOTE, CRUMB_EXPORT,
    CRUMB_PUMP, CRUMB_SLEEP, CRUMB_IDLE, CRUMB_COUNT
};
static const char *const CRUMB_NAME[CRUMB_COUNT] = {
    "ok", "setup", "hw-init", "lvgl-init", "splash", "sd", "moods",
    "build", "day-sheet", "touch-read", "note", "export",
    "lvgl-pump", "sleep", "loop"
};
#define CRUMB_MAGIC 0xC0FFEEu
RTC_NOINIT_ATTR static uint32_t g_crumb_magic;
RTC_NOINIT_ATTR static uint32_t g_crumb_stage;
RTC_NOINIT_ATTR static uint32_t g_crumb_line;
static bool g_prev_crash = false;
static char g_last_stage[32] = "ok";

// uint8_t rather than CrumbStage on purpose: the Arduino builder hoists its
// generated prototypes above this section, so a sketch-defined enum in a
// signature does not compile.
static void crumb(uint8_t s, int line) {
    g_crumb_magic = CRUMB_MAGIC;
    g_crumb_stage = (uint32_t)s;
    g_crumb_line  = (uint32_t)line;
}
#define CRUMB(s) crumb((uint8_t)(s), __LINE__)

// Written once an operation has finished, so a stale "build" crumb can never
// blame the wrong screen.  Anything left on the stack at reboot (build, sheet,
// touch, note, export, splash) is the stage the previous run died in.
static void crumb_mark_ok() { crumb(CRUMB_IDLE, 0); }

static void crumb_read() {
    if (g_crumb_magic != CRUMB_MAGIC) {
        g_prev_crash = false;
        snprintf(g_last_stage, sizeof(g_last_stage), "first-boot");
        return;
    }
    uint32_t st = g_crumb_stage;
    if (st >= CRUMB_COUNT) st = CRUMB_NONE;
    // "idle" means the app was sitting in loop(), "sleep" is an intentional
    // deep sleep - neither is a crash.
    g_prev_crash = (st != CRUMB_NONE && st != CRUMB_IDLE && st != CRUMB_SLEEP);
    snprintf(g_last_stage, sizeof(g_last_stage), "%s @%lu",
             CRUMB_NAME[st], (unsigned long)g_crumb_line);
}

// ============================================================================
//  06. DATE + STORAGE HELPERS
// ============================================================================
static bool is_leap(int y) {
    return (y % 4 == 0 && y % 100 != 0) || (y % 400 == 0);
}
static uint8_t days_in_month(int y, int m) {
    static const uint8_t md[12] = {31,28,31,30,31,30,31,31,30,31,30,31};
    if (m < 1 || m > 12) return 31;
    return md[m - 1] + ((m == 2 && is_leap(y)) ? 1 : 0);
}
static uint16_t day_of_year(int y, int m, int d) {
    uint16_t n = 0;
    for (int i = 1; i < m; i++) n += days_in_month(y, i);
    return n + d;                                  // 1-based
}
// 0 = Sunday ... 6 = Saturday (proleptic Gregorian)
static uint8_t weekday_of(int y, int m, int d) {
    static const int t[] = {0,3,2,5,0,3,5,1,4,6,2,4};
    if (m < 3) y -= 1;
    return (y + y/4 - y/100 + y/400 + t[m-1] + d) % 7;
}

static void date_fallback() {
    // __DATE__ is "Mmm dd yyyy".  Only used when no clock has ever been set:
    // the grid still has a "today", we just cannot promise it is accurate.
    static const char *MON[12] = {"Jan","Feb","Mar","Apr","May","Jun",
                                  "Jul","Aug","Sep","Oct","Nov","Dec"};
    char mon[4] = {0};
    int d = 1, y = COFFEELINGS_DEFAULT_YEAR;
    if (sscanf(__DATE__, "%3s %d %d", mon, &d, &y) >= 3) {
        int mi = 10;
        for (int i = 0; i < 12; i++) if (!strcmp(mon, MON[i])) mi = i;
        g_today.tm_year = y - 1900;
        g_today.tm_mon  = mi;
        g_today.tm_mday = d;
        g_year  = y;
        g_month = mi + 1;
    }
}

static void clock_refresh() {
    time_t now = time(nullptr);
    struct tm t = {};
    localtime_r(&now, &t);
    if ((time_t)now >= (time_t)TIME_VALID_MIN) {
        g_today = t;
        g_clock_valid = true;
    }
}

// Preferences namespaces.  Builds before v1.7 spelled the app "Cofflings", so a
// watch that already holds journal years under the old namespace would look
// empty after the rename; nvs_migrate_namespace() moves them across once.
#define NVS_NS          "coffeelings"
#define NVS_NS_LEGACY   "cofflings"

static void nvs_migrate_namespace() {
    Preferences oldp;
    if (!oldp.begin(NVS_NS_LEGACY, true)) return;
    if (!g_prefs.begin(NVS_NS, false)) { oldp.end(); return; }
    int moved = 0;
    uint8_t buf[366];
    for (int y = 2000; y < 2100; y++) {
        char key[10];
        snprintf(key, sizeof(key), "y%d", y);
        if (g_prefs.getBytesLength(key) == sizeof(buf)) continue;   // already here
        if (oldp.getBytesLength(key) != sizeof(buf)) continue;
        if (oldp.getBytes(key, buf, sizeof(buf)) == sizeof(buf)) {
            g_prefs.putBytes(key, buf, sizeof(buf));
            moved++;
        }
    }
    g_prefs.end();
    oldp.end();
    if (moved)
        Serial.printf("[nvs] moved %d journal year(s) %s -> %s\n",
                      moved, NVS_NS_LEGACY, NVS_NS);
}

static void moods_load() {
    memset(g_moods, 0, sizeof(g_moods));
    char key[10];
    snprintf(key, sizeof(key), "y%d", g_year);
    if (g_prefs.begin(NVS_NS, true)) {
        size_t len = g_prefs.getBytesLength(key);
        if (len == sizeof(g_moods)) g_prefs.getBytes(key, g_moods, len);
        g_prefs.end();
    }
}
static void moods_save() {
    char key[10];
    snprintf(key, sizeof(key), "y%d", g_year);
    if (!g_prefs.begin(NVS_NS, false)) return;
    g_prefs.putBytes(key, g_moods, sizeof(g_moods));
    g_prefs.end();
}
static uint8_t mood_at(int y, int m, int d) {
    if (y != g_year) return 0;
    uint16_t idx = day_of_year(y, m, d) - 1;
    if (idx >= sizeof(g_moods)) return 0;
    return g_moods[idx];
}

// Read a year straight out of NVS when it is not the loaded one (the home
// screen always reports TODAY, even if the user is browsing another year).
static int count_year(int y);
static bool read_year_blob(int y, uint8_t *out) {
    char key[10];
    snprintf(key, sizeof(key), "y%d", y);
    bool ok = false;
    if (g_prefs.begin(NVS_NS, true)) {
        if (g_prefs.getBytesLength(key) == 366 &&
            g_prefs.getBytes(key, out, 366) == 366) ok = true;
        g_prefs.end();
    }
    return ok;
}
static uint8_t mood_for(int y, int m, int d) {
    uint16_t idx = day_of_year(y, m, d) - 1;
    if (idx >= 366) return 0;
    if (y == g_year) return g_moods[idx];
    uint8_t buf[366];
    if (!read_year_blob(y, buf)) return 0;
    return buf[idx];
}
static int year_count_for(int y) {
    if (y == g_year) return count_year(y);
    uint8_t buf[366];
    if (!read_year_blob(y, buf)) return 0;
    int n = 0;
    for (int i = 0; i < 366; i++) if (buf[i]) n++;
    return n;
}

#define PATH_WATCH   "/WATCH"
#define PATH_ROOT    "/WATCH/Coffeelings"
#define PATH_NOTES   "/WATCH/Coffeelings/NOTES"

// Earlier builds created /WATCH/Cofflings, and the first pass at the rename
// created /WATCH/Coffelings.  Move whichever one is on the card to the current
// name so the notes the user already wrote stay reachable.
static void migrate_journal_folder() {
    static const char *legacy[] = {"/WATCH/Cofflings", "/WATCH/Coffelings"};
    if (SD_MMC.exists(PATH_ROOT)) return;
    for (int i = 0; i < 2; i++) {
        if (!strcmp(legacy[i], PATH_ROOT)) continue;
        if (!SD_MMC.exists(legacy[i])) continue;
        if (SD_MMC.rename(legacy[i], PATH_ROOT)) {
            Serial.printf("[sd] moved %s -> %s\n", legacy[i], PATH_ROOT);
            return;
        }
        Serial.printf("[sd] could not move %s, using it read-only\n", legacy[i]);
    }
}

static void note_path(int y, int m, int d, char *out, size_t n) {
    snprintf(out, n, PATH_NOTES "/%04d-%02d-%02d.txt", y, m, d);
}
// ---- note presence cache ---------------------------------------------------
//  The month grid used to call SD_MMC.exists() once per day: 31 FAT lookups per
//  tap, deep on the loop task's stack, the first time any card I/O happened.
//  One directory scan now fills a bitmask for the whole month, so drawing a
//  month never probes the card at all.
static uint32_t g_note_bits = 0;
static int g_note_y = -1, g_note_m = -1;

static void note_mark_dirty() { g_note_y = -1; g_note_m = -1; g_note_bits = 0; }

static void notes_scan(int y, int m) {
    g_note_y = y; g_note_m = m; g_note_bits = 0;
    if (!g_sd_ok) return;
    CRUMB(CRUMB_NOTE);
    File dir = SD_MMC.open(PATH_NOTES);
    if (!dir || !dir.isDirectory()) { if (dir) dir.close(); return; }
    for (;;) {
        File f = dir.openNextFile();
        if (!f) break;
        if (!f.isDirectory()) {
            const char *nm = f.name();
            const char *b = strrchr(nm, '/');
            b = b ? b + 1 : nm;
            int fy = 0, fm = 0, fd = 0;
            if (sscanf(b, "%4d-%2d-%2d", &fy, &fm, &fd) == 3 &&
                fy == y && fm == m && fd >= 1 && fd <= 31 && f.size() > 0)
                g_note_bits |= (1u << (fd - 1));
        }
        f.close();
    }
    dir.close();
}

static bool note_exists(int y, int m, int d) {
    if (!g_sd_ok || d < 1 || d > 31) return false;
    if (y != g_note_y || m != g_note_m) notes_scan(y, m);
    return ((g_note_bits >> (d - 1)) & 1u) != 0;
}
static String note_read(int y, int m, int d) {
    String s;
    if (!g_sd_ok) return s;
    char p[72];
    note_path(y, m, d, p, sizeof(p));
    File f = SD_MMC.open(p, FILE_READ);
    if (!f) return s;
    s = f.readString();
    f.close();
    s.trim();
    return s;
}
static bool ensure_dir_path(const char *path);
static bool note_write(int y, int m, int d, const char *txt) {
    if (!g_sd_ok) return false;
    CRUMB(CRUMB_NOTE);
    ensure_dir_path(PATH_NOTES);          // card inserted after boot?
    char p[72];
    note_path(y, m, d, p, sizeof(p));
    if (!txt || !*txt) {
        if (SD_MMC.exists(p)) SD_MMC.remove(p);
        note_mark_dirty();
        return true;
    }
    File f = SD_MMC.open(p, FILE_WRITE);
    if (!f) return false;
    f.print(txt);
    f.close();
    note_mark_dirty();
    return true;
}

// mkdir -p: SD_MMC.mkdir() only makes one level, and /WATCH/Coffeelings needs
// /WATCH to exist first.
static bool ensure_dir_path(const char *path) {
    if (!g_sd_ok) return false;
    char buf[96];
    if (strlen(path) >= sizeof(buf)) return false;
    strlcpy(buf, path, sizeof(buf));
    for (char *p = buf + 1; *p; p++) {
        if (*p != '/') continue;
        *p = 0;
        if (!SD_MMC.exists(buf)) SD_MMC.mkdir(buf);
        *p = '/';
    }
    if (!SD_MMC.exists(buf)) SD_MMC.mkdir(buf);
    return SD_MMC.exists(buf);
}

static void ensure_watch_dirs() {
    ensure_dir_path(PATH_WATCH);
    ensure_dir_path(PATH_ROOT);
    ensure_dir_path(PATH_NOTES);
}

static void storage_init() {
    CRUMB(CRUMB_STORAGE);
    SD_MMC.setPins(SDMMC_CLK, SDMMC_CMD, SDMMC_DATA);
    for (uint8_t attempt = 0; attempt < 3 && !g_sd_ok; attempt++) {
        g_sd_ok = SD_MMC.begin("/sdcard", true);
        if (!g_sd_ok) delay(120);
    }
    if (!g_sd_ok) return;
    migrate_journal_folder();
    ensure_watch_dirs();
    note_mark_dirty();
    Serial.printf("[sd] journal folder %s\n", SD_MMC.exists(PATH_ROOT) ? PATH_ROOT : "FAILED");
}

static int count_month(int y, int m) {
    int n = 0;
    uint8_t dim = days_in_month(y, m);
    for (int d = 1; d <= dim; d++) if (mood_at(y, m, d)) n++;
    return n;
}
static int count_year(int y) {
    if (y != g_year) return 0;
    int n = 0;
    for (int i = 0; i < 366; i++) if (g_moods[i]) n++;
    return n;
}
// Day numbers sit on top of the roast colour, so pick ink or cream per cell.
static uint32_t mood_ink(uint8_t mood) {
    if (!mood) return C_MUTED;
    uint32_t c = MOOD_COLOR[mood - 1];
    int r = (c >> 16) & 0xFF, g = (c >> 8) & 0xFF, b = c & 0xFF;
    int lum = (r * 299 + g * 587 + b * 114) / 1000;
    return lum > 120 ? C_BG : C_CREAM;
}

static int mood_tally(int y, int m, int mood) {
    int n = 0;
    uint8_t dim = days_in_month(y, m);
    for (int d = 1; d <= dim; d++) if (mood_at(y, m, d) == mood) n++;
    return n;
}
static void prev_date(int &y, int &m, int &d) {
    if (--d < 1) {
        if (--m < 1) { m = 12; y--; }
        d = days_in_month(y, m);
    }
}
static int streak_days() {
    // Walk real dates backwards, across months and years.  An unlogged today
    // does not break a streak that is still alive yesterday.
    int y = g_today.tm_year + 1900, m = g_today.tm_mon + 1, d = g_today.tm_mday;
    if (!mood_for(y, m, d)) prev_date(y, m, d);
    int run = 0;
    while (run < 400 && mood_for(y, m, d)) {
        run++;
        prev_date(y, m, d);
    }
    return run;
}

// ============================================================================
//  07. DISPLAY / TOUCH / POWER
// ============================================================================
static void month_step(int delta);
static void go_home();

void coff_touch_irq() { g_touch->IIC_Interrupt_Flag = true; }

static void flush_cb(lv_disp_drv_t *d, const lv_area_t *a, lv_color_t *px) {
    g_gfx->draw16bitRGBBitmap(a->x1, a->y1, (uint16_t *)&px->full,
                              lv_area_get_width(a), lv_area_get_height(a));
    lv_disp_flush_ready(d);
}

static void touch_cb(lv_indev_drv_t *, lv_indev_data_t *data) {
    data->state = LV_INDEV_STATE_REL;
    // If the FT3168 never answered, don't touch the driver at all: an
    // un-begun DriveBus device is exactly the kind of NULL dereference that
    // reboots the watch on the first lv_timer_handler().  LauncherOS refuses
    // to boot without touch; we just run read-only instead.
    if (!g_touch_ok) return;

    int32_t x = g_touch->IIC_Read_Device_Value(
        g_touch->Arduino_IIC_Touch::Value_Information::TOUCH_COORDINATE_X);
    int32_t y = g_touch->IIC_Read_Device_Value(
        g_touch->Arduino_IIC_Touch::Value_Information::TOUCH_COORDINATE_Y);
    int fingers = g_touch->IIC_Read_Device_Value(
        g_touch->Arduino_IIC_Touch::Value_Information::TOUCH_FINGER_NUMBER);
    bool pressed = fingers > 0 && fingers <= 2 && x >= 0 && x < LCD_WIDTH &&
                   y >= 0 && y < LCD_HEIGHT;

    // FT3168 occasionally reports fingers=0 for one sample mid-touch.
    static uint8_t zero = 0;
    if (pressed) zero = 0;
    else if (g_touch_active && ++zero < 3) { pressed = true; x = g_touch_x; y = g_touch_y; }

    if (g_screen_off || g_touch_block) {
        data->state = LV_INDEV_STATE_REL;
        if (pressed && g_screen_off) g_last_wake_ms = millis();
        if (!pressed) { g_touch_block = false; g_touch_active = false; }
        return;
    }
    if (pressed) {
        data->state = LV_INDEV_STATE_PR;
        data->point.x = x; data->point.y = y;
        g_last_touch_ms = millis();
        if (!g_touch_active) {
            g_touch_active = true;
            g_start_x = x; g_start_y = y; g_touch_start_ms = millis();
        }
        g_touch_x = x; g_touch_y = y;
    } else {
        data->state = LV_INDEV_STATE_REL;
        if (g_touch_active) {
            int dx = g_touch_x - g_start_x, dy = g_touch_y - g_start_y;
            uint32_t dur = millis() - g_touch_start_ms;
            g_touch_active = false;
            g_last_touch_ms = millis();
            // Queue the gesture -- never rebuild a screen from in here.
            // Horizontal flick on the calendar = previous / next month.
            if (g_view == SCREEN_MONTH && !g_sheet && dur < 800 &&
                abs(dx) > 60 && abs(dx) > abs(dy)) {
                g_nav = (dx < 0) ? NAV_NEXT_MONTH : NAV_PREV_MONTH;
            }
            // Flick down anywhere in the journal = back to the watch face.
            else if (!g_sheet && dur < 800 && abs(dy) > 60 && abs(dy) > abs(dx) &&
                     dy > 0 &&
                     (g_view == SCREEN_MONTH || g_view == SCREEN_YEAR)) {
                g_nav = NAV_HOME;
            }
        }
    }
}

// The only way this app runs LVGL.  g_in_pump marks the window in which UI
// requests from callbacks must be queued instead of executed.
static void lvgl_pump() {
    CRUMB(CRUMB_PUMP);
    g_in_pump = true;
    lv_timer_handler();
    g_in_pump = false;
    crumb_mark_ok();
}

static void fade_to(uint8_t target, uint16_t ms) {
    int from = g_cur_brightness;
    int steps = 10;
    for (int i = 1; i <= steps; i++) {
        int v = from + (target - from) * i / steps;
        static_cast<Arduino_CO5300 *>(g_gfx)->setBrightness(v);
        g_cur_brightness = v;
        lvgl_pump();
        delay(ms / steps);
    }
}

static void set_screen_off(bool off) {
    if (g_screen_off == off) return;
    g_screen_off = off;
    g_touch_block = true;
    if (off) {
        fade_to(0, 180);
        g_screen_off_since = millis();
    } else {
        lv_obj_invalidate(lv_scr_act());
        lvgl_pump();
        fade_to(g_brightness, 180);
        g_last_touch_ms = g_last_wake_ms = millis();
        g_screen_off_since = 0;
    }
}

// Deep sleep, LauncherOS style: RTC domain stays alive so the wall clock
// survives, and either physical button wakes the watch.  On wake the chip
// reboots (deep sleep is a reset), which setup() turns into a quick start.
static void enter_deep_sleep() {
    CRUMB(CRUMB_SLEEP);
    moods_save();
    time_publish("deep sleep", false);
    fade_to(0, 260);
    esp_sleep_pd_config(ESP_PD_DOMAIN_RTC_PERIPH, ESP_PD_OPTION_ON);
    esp_err_t armed = esp_sleep_enable_ext1_wakeup(
        (1ULL << BOOT_BTN) | (1ULL << PWR_BTN_PIN), ESP_EXT1_WAKEUP_ANY_LOW);
    if (armed != ESP_OK) {
        // No usable wake source would mean a watch that never wakes again, so
        // stay awake and keep the light-sleep polling instead.
        Serial.printf("[pm] deep sleep refused (ext1 %d) - staying in light sleep\n",
                      (int)armed);
        fade_to(g_brightness, 180);
        return;
    }
    Serial.println("[pm] deep sleep - BOOT or side button wakes");
    delay(60);
    esp_deep_sleep_start();
}

static void pmu_init() {
    g_pmu_ok = g_pmu.begin(Wire, AXP2101_SLAVE_ADDRESS, IIC_SDA, IIC_SCL);
    if (!g_pmu_ok) return;
    g_pmu.disableTSPinMeasure();
    g_pmu.enableBattVoltageMeasure();
    g_pmu.disableIRQ(XPOWERS_AXP2101_ALL_IRQ);
    g_pmu.clearIrqStatus();
    g_pmu.enableIRQ(XPOWERS_AXP2101_PKEY_SHORT_IRQ | XPOWERS_AXP2101_PKEY_LONG_IRQ);
    g_pmu.setChargingLedMode(XPOWERS_CHG_LED_OFF);
}

static void poll_power_button() {
    if (!g_pmu_ok) return;
    static uint32_t fire = 0;
    g_pmu.getIrqStatus();
    if (g_pmu.isPekeyShortPressIrq() && millis() - fire > 400) {
        fire = millis();
        set_screen_off(!g_screen_off);
    }
    g_pmu.clearIrqStatus();
}

// ============================================================================
//  08. UI HELPERS
// ============================================================================
// Returns the persistent root of g_view, creating it the first time only.
// Every builder sets g_view before calling this, and build_screen_now() has
// already emptied the root, so the builder always starts from a blank screen.
static lv_obj_t *ui_root() {
    ScreenID id = (g_view < SCREEN_COUNT) ? (ScreenID)g_view : SCREEN_HOME;
    lv_obj_t *s = g_screens[id];
    if (s) return s;
    s = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s, lv_color_hex(C_BG), 0);
    lv_obj_set_style_bg_opa(s, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(s, 0, 0);
    lv_obj_set_style_border_width(s, 0, 0);
    lv_obj_clear_flag(s, LV_OBJ_FLAG_SCROLLABLE);
    g_screens[id] = s;
    return s;
}

static lv_obj_t *ui_label(lv_obj_t *parent, const char *txt, int x, int y,
                          const lv_font_t *font, uint32_t color) {
    lv_obj_t *l = lv_label_create(parent);
    lv_label_set_text(l, txt);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
    lv_obj_set_pos(l, x, y);
    return l;
}

static lv_obj_t *ui_label_center(lv_obj_t *parent, const char *txt, int y,
                                 const lv_font_t *font, uint32_t color) {
    lv_obj_t *l = lv_label_create(parent);
    lv_label_set_text(l, txt);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(l, LCD_WIDTH);
    lv_obj_set_pos(l, 0, y);
    return l;
}

static lv_obj_t *ui_panel(lv_obj_t *parent, int x, int y, int w, int h,
                          uint32_t bg, int radius) {
    lv_obj_t *o = lv_obj_create(parent);
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

static lv_obj_t *ui_button(lv_obj_t *parent, const char *txt, int x, int y,
                           int w, int h, lv_event_cb_t cb, void *user,
                           uint32_t bg, uint32_t fg, const lv_font_t *font) {
    lv_obj_t *b = lv_btn_create(parent);
    lv_obj_set_pos(b, x, y);
    lv_obj_set_size(b, w, h);
    lv_obj_set_style_bg_color(b, lv_color_hex(bg), 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(C_SURFACE2), LV_STATE_PRESSED);
    lv_obj_set_style_radius(b, 13, 0);
    lv_obj_set_style_border_color(b, lv_color_hex(C_LINE), 0);
    lv_obj_set_style_border_width(b, 1, 0);
    lv_obj_set_style_shadow_width(b, 0, 0);
    lv_obj_set_style_pad_all(b, 0, 0);
    lv_obj_t *l = lv_label_create(b);
    lv_label_set_text(l, txt);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(fg), 0);
    lv_obj_center(l);
    if (cb) lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, user);
    return b;
}

static void toast_timer_cb(lv_timer_t *t);
static void show_toast(const char *msg) {
    if (g_toast_timer) { lv_timer_del(g_toast_timer); g_toast_timer = nullptr; }
    if (g_toast_box) { lv_obj_del(g_toast_box); g_toast_box = nullptr; }
    lv_obj_t *box = ui_panel(lv_layer_top(), 30, 214, 350, 74, C_SURFACE2, 16);
    lv_obj_set_style_border_color(box, lv_color_hex(C_ACCENT), 0);
    lv_obj_set_style_border_width(box, 1, 0);
    lv_obj_t *l = lv_label_create(box);
    lv_label_set_text(l, msg);
    lv_obj_set_width(l, 310);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(l, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(C_CREAM), 0);
    lv_obj_center(l);
    g_toast_box = box;
    g_toast_timer = lv_timer_create(toast_timer_cb, 1900, NULL);
}
static void toast_timer_cb(lv_timer_t *t) {
    if (g_toast_box) { lv_obj_del(g_toast_box); g_toast_box = nullptr; }
    lv_timer_del(t);
    g_toast_timer = nullptr;
}

// ---- idle screen cleaner ---------------------------------------------------
//  A view that has left the stage keeps its children until this runs, then
//  they are freed -- the root itself is kept forever.  Freed only when no
//  cross-fade is in flight, so LVGL is never drawing an object we are freeing.
static void screen_idle_clean_cb(lv_timer_t *) {
    lv_disp_t *d = lv_disp_get_default();
    if (d && (d->prev_scr || d->scr_to_load)) return;   // fade in progress
    trash_flush();
    lv_obj_t *act = lv_scr_act();
    for (int i = 0; i < SCREEN_COUNT; i++) {
        lv_obj_t *s = g_screens[i];
        if (!s || s == act) continue;
        if (g_pending_valid && (ScreenID)i == g_pending_view) continue;
        if (lv_obj_get_child_cnt(s)) lv_obj_clean(s);
    }
    if (splash_screen && splash_screen != act &&
        lv_obj_get_child_cnt(splash_screen))
        lv_obj_clean(splash_screen);
}

// ============================================================================
//  09. SCREEN BUILDERS
// ============================================================================
static void build_screen(ScreenID id);

static void month_step(int delta) {
    int m = g_month + delta, y = g_year;
    while (m < 1)  { m += 12; y--; }
    while (m > 12) { m -= 12; y++; }
    if (y < 2023 || y > 2099) return;
    if (y != g_year) { g_year = y; moods_load(); }
    g_year = y;
    g_month = m;
    build_screen(SCREEN_MONTH);
}

static void prev_month_cb(lv_event_t *) { month_step(-1); }
static void next_month_cb(lv_event_t *) { month_step(1); }
static void year_view_cb(lv_event_t *)  { build_screen(SCREEN_YEAR); }
static void home_view_cb(lv_event_t *)  { build_screen(SCREEN_HOME); }

static void set_mood(int day, uint8_t mood) {
    if (day < 1 || day > days_in_month(g_year, g_month)) return;
    uint16_t idx = day_of_year(g_year, g_month, day) - 1;
    if (idx >= sizeof(g_moods)) return;
    g_moods[idx] = mood;
    moods_save();
    Serial.printf("[coffeelings] %04d-%02d-%02d -> roast %u\n",
                  g_year, g_month, day, mood);
}

static void open_note(int y, int m, int d) {
    g_edit_y = y; g_edit_m = m; g_edit_d = d;
    build_screen(SCREEN_NOTE);
}

// ---- deferred teardown for the day sheet ----------------------------------
//  The sheet and its scrim are children of the month screen.  Freeing them from
//  inside their own click event is unsafe, and an lv_obj_del_async can land
//  *after* the screen cleaner emptied the parent -- a double free.  They are
//  hidden at once and freed here instead: from loop() or from the cleaner,
//  always outside every event, always before a screen is emptied.
#define TRASH_SLOTS 4
static lv_obj_t *g_trash[TRASH_SLOTS] = {nullptr};
static void trash_add(lv_obj_t *o) {
    if (!o) return;
    lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);      // gone from the UI immediately
    for (int i = 0; i < TRASH_SLOTS; i++) {
        if (!g_trash[i]) { g_trash[i] = o; return; }
    }
    // Queue full: leave it hidden.  It goes with its screen, and a hidden
    // object is a far better outcome than a free during an event.
}
static void trash_flush() {
    for (int i = 0; i < TRASH_SLOTS; i++) {
        lv_obj_t *o = g_trash[i];
        g_trash[i] = nullptr;
        if (o) lv_obj_del(o);
    }
}

static void close_sheet() {
    trash_add(g_scrim);
    trash_add(g_sheet);
    g_scrim = nullptr;
    g_sheet = nullptr;
}

static void scrim_cb(lv_event_t *) { close_sheet(); }
static void sheet_close_cb(lv_event_t *) { close_sheet(); }

static void mood_pick_cb(lv_event_t *e) {
    uint8_t mood = (uint8_t)(intptr_t)lv_event_get_user_data(e);
    set_mood(g_sel_day, mood);
    // No close_sheet() here: build_screen() disposes the whole old screen
    // (sheet included) in one go.  Deleting children first and the parent
    // afterwards would double-free them.
    build_screen(SCREEN_MONTH);
}
static void sheet_note_cb(lv_event_t *) {
    if (!g_sd_ok) { show_toast("SD card missing - notes unavailable."); return; }
    int d = g_sel_day;
    open_note(g_year, g_month, d);       // build_screen() drops the sheet with it
}
static void sheet_clear_cb(lv_event_t *) {
    set_mood(g_sel_day, 0);
    build_screen(SCREEN_MONTH);
}

static void open_day_sheet(int day) {
    if (g_screen == nullptr) return;
    if (g_sheet) return;
    CRUMB(CRUMB_SHEET);
    g_sel_day = day;

    char datebuf[40];
    static const char *MN[12] = {"Jan","Feb","Mar","Apr","May","Jun",
                                 "Jul","Aug","Sep","Oct","Nov","Dec"};
    snprintf(datebuf, sizeof(datebuf), "%s %d, %d", MN[g_month-1], day, g_year);

    // dim the calendar behind the sheet
    g_scrim = lv_obj_create(g_screen);
    lv_obj_set_pos(g_scrim, 0, 0);
    lv_obj_set_size(g_scrim, LCD_WIDTH, LCD_HEIGHT);
    lv_obj_set_style_bg_color(g_scrim, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(g_scrim, LV_OPA_60, 0);
    lv_obj_set_style_border_width(g_scrim, 0, 0);
    lv_obj_set_style_radius(g_scrim, 0, 0);
    lv_obj_add_flag(g_scrim, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(g_scrim, scrim_cb, LV_EVENT_CLICKED, NULL);

    g_sheet = ui_panel(g_screen, 0, 244, LCD_WIDTH, 258, C_SURFACE, 26);
    lv_obj_set_style_border_color(g_sheet, lv_color_hex(C_LINE), 0);
    lv_obj_set_style_border_width(g_sheet, 1, 0);

    lv_obj_t *grab = ui_panel(g_sheet, 183, 10, 44, 4, C_LINE, 2);
    (void)grab;

    ui_label_center(g_sheet, "How was this day?", 22, &lv_font_montserrat_18, C_CREAM);
    lv_obj_t *dl = ui_label_center(g_sheet, datebuf, 48, &lv_font_montserrat_12, C_MUTED);
    (void)dl;

    uint8_t current = mood_at(g_year, g_month, day);
    for (int i = 0; i < MOOD_COUNT; i++) {
        lv_obj_t *sw = lv_obj_create(g_sheet);
        lv_obj_set_pos(sw, 12 + i * 78, 76);
        lv_obj_set_size(sw, 70, 56);
        lv_obj_set_style_bg_color(sw, lv_color_hex(MOOD_COLOR[i]), 0);
        lv_obj_set_style_bg_opa(sw, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(sw, 14, 0);
        lv_obj_set_style_border_color(
            sw, lv_color_hex(current == i + 1 ? C_CREAM : C_LINE), 0);
        lv_obj_set_style_border_width(sw, current == i + 1 ? 3 : 1, 0);
        lv_obj_clear_flag(sw, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(sw, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(sw, mood_pick_cb, LV_EVENT_CLICKED,
                            (void *)(intptr_t)(i + 1));

        lv_obj_t *nm = lv_label_create(g_sheet);
        lv_label_set_text(nm, MOOD_NAME[i]);
        lv_obj_set_width(nm, 70);
        lv_obj_set_style_text_align(nm, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_text_font(nm, &lv_font_montserrat_12, 0);
        lv_obj_set_style_text_color(nm, lv_color_hex(C_MUTED), 0);
        lv_obj_set_pos(nm, 12 + i * 78, 138);
    }

    ui_button(g_sheet, "ADD NOTE", 12, 168, 188, 52, sheet_note_cb, NULL,
              C_SURFACE2, C_CREAM, &lv_font_montserrat_16);
    ui_button(g_sheet, "CLEAR", 210, 168, 188, 52, sheet_clear_cb, NULL,
              C_SURFACE2, C_MUTED, &lv_font_montserrat_16);
    ui_button(g_sheet, LV_SYMBOL_CLOSE, 360, 8, 38, 34, sheet_close_cb, NULL,
              C_SURFACE2, C_MUTED, &lv_font_montserrat_14);

    if (!g_sd_ok)
        ui_label(g_sheet, "No SD card: notes are off, colours still save.",
                 18, 228, &lv_font_montserrat_12, C_MUTED);
}

static void day_cell_cb(lv_event_t *e) {
    int day = (int)(intptr_t)lv_event_get_user_data(e);
    open_day_sheet(day);
}

// ---- HOME (watch face) -----------------------------------------------------
static void home_tap_cb(lv_event_t *)  { build_screen(SCREEN_MONTH); }
static void home_info_cb(lv_event_t *) { build_screen(SCREEN_INFO); }

static void build_home() {
    CRUMB(CRUMB_BUILD);
    g_view = SCREEN_HOME;
    g_screen = ui_root();
    g_home_time = nullptr;
    g_home_date = nullptr;
    g_batt_lbl = nullptr;

    // Full-screen tap target, created first so the header buttons sit on top.
    lv_obj_t *tap = lv_btn_create(g_screen);
    lv_obj_set_pos(tap, 0, 0);
    lv_obj_set_size(tap, LCD_WIDTH, LCD_HEIGHT);
    lv_obj_set_style_bg_opa(tap, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(tap, 0, 0);
    lv_obj_set_style_shadow_width(tap, 0, 0);
    lv_obj_set_style_radius(tap, 0, 0);
    lv_obj_add_event_cb(tap, home_tap_cb, LV_EVENT_CLICKED, NULL);

    ui_label(g_screen, "Coffeelings", 18, 14, &lv_font_montserrat_12, C_ACCENT);
    ui_button(g_screen, "i", 350, 10, 46, 34, home_info_cb, NULL,
              C_SURFACE, C_MUTED, &lv_font_montserrat_12);
    g_batt_lbl = ui_label(g_screen, "", 300, 19, &lv_font_montserrat_12, C_MUTED);
    if (g_battery >= 0) {
        char b[24];
        snprintf(b, sizeof(b), "%s%d%%", g_charging ? "+" : "", g_battery);
        lv_label_set_text(g_batt_lbl, b);
    }

    char hm[8] = "--:--";
    char date[48] = "clock not set";
    if (g_clock_valid) {
        strftime(hm, sizeof(hm), "%H:%M", &g_today);
        strftime(date, sizeof(date), "%A %d %B", &g_today);
    }
    g_home_time = ui_label_center(g_screen, hm, 58, &lv_font_montserrat_48, C_CREAM);
    g_home_date = ui_label_center(g_screen, date, 132, &lv_font_montserrat_16, C_MUTED);

    int ty = g_today.tm_year + 1900;
    int tm = g_today.tm_mon + 1;
    int td = g_today.tm_mday;
    uint8_t mood = mood_for(ty, tm, td);

    lv_obj_t *card = lv_obj_create(g_screen);
    lv_obj_set_pos(card, 137, 186);
    lv_obj_set_size(card, 136, 92);
    lv_obj_set_style_bg_color(card, lv_color_hex(mood ? MOOD_COLOR[mood - 1] : C_EMPTY), 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(card, 22, 0);
    lv_obj_set_style_border_color(card, lv_color_hex(mood ? C_GOLD : C_LINE), 0);
    lv_obj_set_style_border_width(card, mood ? 2 : 1, 0);
    lv_obj_set_style_pad_all(card, 0, 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_CLICKABLE);

    char dnum[4];
    snprintf(dnum, sizeof(dnum), "%d", td);
    lv_obj_t *dn = lv_label_create(card);
    lv_label_set_text(dn, dnum);
    lv_obj_set_style_text_font(dn, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(dn, lv_color_hex(mood_ink(mood)), 0);
    lv_obj_align(dn, LV_ALIGN_TOP_MID, 0, 10);
    lv_obj_t *tl = lv_label_create(card);
    lv_label_set_text(tl, "TODAY");
    lv_obj_set_style_text_font(tl, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(tl, lv_color_hex(mood_ink(mood)), 0);
    lv_obj_set_style_text_opa(tl, LV_OPA_80, 0);
    lv_obj_set_style_text_letter_space(tl, 2, 0);
    lv_obj_align(tl, LV_ALIGN_BOTTOM_MID, 0, -8);

    char status[48];
    if (mood) snprintf(status, sizeof(status), "Today feels %s", MOOD_NAME[mood - 1]);
    else snprintf(status, sizeof(status), "Tap to log today's roast");
    ui_label_center(g_screen, status, 294, &lv_font_montserrat_16,
                    mood ? C_CREAM : C_ACCENT);

    char streak[64];
    snprintf(streak, sizeof(streak), "STREAK %d   |   %d DAYS IN %d",
             streak_days(), year_count_for(ty), ty);
    ui_label_center(g_screen, streak, 324, &lv_font_montserrat_12, C_MUTED);

    // A tiny palette strip: today's roast gets the ring.
    for (int i = 0; i < MOOD_COUNT; i++) {
        int x = 155 + i * 22;
        lv_obj_t *dot = lv_obj_create(g_screen);
        lv_obj_set_pos(dot, x, 362);
        lv_obj_set_size(dot, 12, 12);
        lv_obj_set_style_bg_color(dot, lv_color_hex(MOOD_COLOR[i]), 0);
        lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_border_color(dot, lv_color_hex(C_CREAM), 0);
        lv_obj_set_style_border_width(dot, (mood == i + 1) ? 2 : 0, 0);
        lv_obj_clear_flag(dot, LV_OBJ_FLAG_CLICKABLE);
    }

    if (g_prev_crash) {
        // The previous run died somewhere: say where, on the face itself, so it
        // is readable without a PC and without hunting for the info screen.
        char bad[80];
        snprintf(bad, sizeof(bad), "last run: %s @ %s", g_last_boot, g_last_stage);
        ui_label_center(g_screen, bad, 446, &lv_font_montserrat_12, C_GOLD);
    } else {
        ui_label_center(g_screen, "TAP TO OPEN YOUR JOURNAL", 446,
                        &lv_font_montserrat_12, C_MUTED);
    }
}


// ---- MONTH -----------------------------------------------------------------
static void build_month() {
    CRUMB(CRUMB_BUILD);
    g_view = SCREEN_MONTH;
    g_screen = ui_root();
    g_batt_lbl = nullptr;

    ui_label(g_screen, "Coffeelings", 18, 8, &lv_font_montserrat_22, C_CREAM);
    ui_label(g_screen, "a little coffee diary", 20, 36, &lv_font_montserrat_12, C_MUTED);
    ui_button(g_screen, "YEAR", 246, 12, 92, 38, year_view_cb, NULL,
              C_SURFACE, C_ACCENT, &lv_font_montserrat_14);
    ui_button(g_screen, LV_SYMBOL_HOME, 346, 12, 50, 38, home_view_cb, NULL,
              C_SURFACE, C_CREAM, &lv_font_montserrat_16);

    static const char *MN[12] = {"January","February","March","April","May","June",
                                 "July","August","September","October","November","December"};
    char head[32];
    snprintf(head, sizeof(head), "%s %d", MN[g_month - 1], g_year);
    ui_label_center(g_screen, head, 58, &lv_font_montserrat_20, C_CREAM);
    ui_button(g_screen, LV_SYMBOL_LEFT, 12, 54, 46, 40, prev_month_cb, NULL,
              C_SURFACE, C_CREAM, &lv_font_montserrat_16);
    ui_button(g_screen, LV_SYMBOL_RIGHT, 352, 54, 46, 40, next_month_cb, NULL,
              C_SURFACE, C_CREAM, &lv_font_montserrat_16);

    static const char *WD[7] = {"M","T","W","T","F","S","S"};
    for (int i = 0; i < 7; i++) {
        lv_obj_t *l = ui_label(g_screen, WD[i], 12 + i * 56, 102,
                               &lv_font_montserrat_12, C_MUTED);
        lv_obj_set_width(l, 50);
        lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    }

    const int first = (weekday_of(g_year, g_month, 1) + 6) % 7;   // Monday-first
    const int dim   = days_in_month(g_year, g_month);
    const bool this_month = (g_year == g_today.tm_year + 1900 &&
                             g_month == g_today.tm_mon + 1);
    const int today = g_today.tm_mday;

    CRUMB(CRUMB_BUILD);
    for (int i = 0; i < 42; i++) {
        int day = i - first + 1;
        int col = i % 7, row = i / 7;
        int x = 12 + col * 56, y = 128 + row * 56;
        if (day < 1 || day > dim) continue;

        uint8_t mood = mood_at(g_year, g_month, day);
        bool is_today = this_month && day == today;
        bool future = this_month && day > today;

        lv_obj_t *cell = lv_obj_create(g_screen);
        lv_obj_set_pos(cell, x, y);
        lv_obj_set_size(cell, 50, 50);
        lv_obj_set_style_bg_color(cell, lv_color_hex(mood ? MOOD_COLOR[mood - 1]
                                                          : C_EMPTY), 0);
        lv_obj_set_style_bg_opa(cell, future ? LV_OPA_30 : LV_OPA_COVER, 0);
        lv_obj_set_style_radius(cell, 13, 0);
        lv_obj_set_style_border_color(cell, lv_color_hex(is_today ? C_GOLD : C_LINE), 0);
        lv_obj_set_style_border_width(cell, is_today ? 3 : (mood ? 0 : 1), 0);
        lv_obj_set_style_pad_all(cell, 0, 0);
        lv_obj_clear_flag(cell, LV_OBJ_FLAG_SCROLLABLE);
        if (!future) {
            lv_obj_add_flag(cell, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_add_event_cb(cell, day_cell_cb, LV_EVENT_CLICKED,
                                (void *)(intptr_t)day);
        }

        char dnum[4];
        snprintf(dnum, sizeof(dnum), "%d", day);
        lv_obj_t *n = lv_label_create(cell);
        lv_label_set_text(n, dnum);
        lv_obj_set_style_text_font(n, &lv_font_montserrat_12, 0);
        lv_obj_set_style_text_color(n, lv_color_hex(mood_ink(mood)), 0);
        lv_obj_set_pos(n, 6, 3);
        lv_obj_clear_flag(n, LV_OBJ_FLAG_CLICKABLE);

        if (note_exists(g_year, g_month, day)) {
            lv_obj_t *dot = ui_panel(cell, 38, 38, 6, 6, C_ACCENT, 3);
            (void)dot;
        }
    }

    CRUMB(CRUMB_BUILD);
    char stat[48];
    snprintf(stat, sizeof(stat), "STREAK %d   |   %d / %d",
             streak_days(), count_month(g_year, g_month), dim);
    ui_label(g_screen, stat, 18, 474, &lv_font_montserrat_12, C_MUTED);

    g_batt_lbl = ui_label(g_screen, "", 300, 474, &lv_font_montserrat_12, C_MUTED);
    if (g_battery >= 0) {
        char b[24];
        snprintf(b, sizeof(b), "%s%d%%", g_charging ? "+" : "", g_battery);
        lv_label_set_text(g_batt_lbl, b);
    }
}

// ---- YEAR HEATMAP ----------------------------------------------------------
static void year_month_cb(lv_event_t *e) {
    g_month = (int)(intptr_t)lv_event_get_user_data(e);
    build_screen(SCREEN_MONTH);
}
static void year_step_cb(lv_event_t *e) {
    int delta = (int)(intptr_t)lv_event_get_user_data(e);
    int y = g_year + delta;
    if (y < 2023 || y > 2099) return;
    g_year = y;
    moods_load();
    build_screen(SCREEN_YEAR);
}
static void year_back_cb(lv_event_t *) { build_screen(SCREEN_MONTH); }

// The canvas does not own its frame buffer, so free it when the screen dies.
static void year_canvas_del_cb(lv_event_t *e) {
    void *buf = lv_event_get_user_data(e);
    if (buf) heap_caps_free(buf);
}

static void build_year() {
    CRUMB(CRUMB_BUILD);
    g_view = SCREEN_YEAR;
    g_screen = ui_root();
    g_batt_lbl = nullptr;

    char title[16];
    snprintf(title, sizeof(title), "%d", g_year);
    ui_label(g_screen, title, 18, 8, &lv_font_montserrat_22, C_CREAM);
    ui_label(g_screen, "A YEAR OF ROASTS", 20, 36, &lv_font_montserrat_12, C_MUTED);

    ui_button(g_screen, LV_SYMBOL_LEFT, 150, 10, 40, 36, year_step_cb,
              (void *)(intptr_t)-1, C_SURFACE, C_CREAM, &lv_font_montserrat_14);
    ui_button(g_screen, LV_SYMBOL_RIGHT, 196, 10, 40, 36, year_step_cb,
              (void *)(intptr_t)1, C_SURFACE, C_CREAM, &lv_font_montserrat_14);
    ui_button(g_screen, LV_SYMBOL_HOME, 244, 10, 44, 38, home_view_cb, NULL,
              C_SURFACE, C_CREAM, &lv_font_montserrat_16);
    ui_button(g_screen, "MONTH", 300, 10, 94, 38, year_back_cb, NULL,
              C_SURFACE, C_ACCENT, &lv_font_montserrat_14);

    static const char *MN3[12] = {"JAN","FEB","MAR","APR","MAY","JUN",
                                  "JUL","AUG","SEP","OCT","NOV","DEC"};
    for (int m = 1; m <= 12; m++) {
        int x = 10 + (m - 1) * 32;
        lv_obj_t *hdr = ui_button(g_screen, MN3[m - 1], x, 92, 30, 20,
                                  year_month_cb, (void *)(intptr_t)m,
                                  C_BG, m == g_month ? C_ACCENT : C_MUTED,
                                  &lv_font_montserrat_12);
        lv_obj_set_style_border_width(hdr, 0, 0);
    }

    // The heatmap is one canvas, not 372 lv_obj cells.  The cell version needed
    // roughly 80 KB of a 64 KB LVGL heap: opening the year view ran the heap
    // dry, LV_ASSERT_MALLOC() spun forever and the task watchdog rebooted the
    // watch.  A canvas costs one object plus a PSRAM frame, and paints in one
    // pass.  Coordinates match the old grid exactly.
    const int HM_W = 380, HM_H = 346;
    size_t hm_bytes = LV_CANVAS_BUF_SIZE_TRUE_COLOR(HM_W, HM_H);
    uint8_t *hm = (uint8_t *)heap_caps_malloc(hm_bytes,
                                              MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!hm) hm = (uint8_t *)heap_caps_malloc(hm_bytes, MALLOC_CAP_8BIT);
    if (hm) {
        lv_obj_t *cv = lv_canvas_create(g_screen);
        lv_canvas_set_buffer(cv, hm, HM_W, HM_H, LV_IMG_CF_TRUE_COLOR);
        lv_obj_set_pos(cv, 10, 114);
        lv_obj_clear_flag(cv, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_style_border_width(cv, 0, 0);
        lv_obj_set_style_pad_all(cv, 0, 0);
        lv_obj_add_event_cb(cv, year_canvas_del_cb, LV_EVENT_DELETE, hm);
        lv_canvas_fill_bg(cv, lv_color_hex(C_BG), LV_OPA_COVER);
        lv_draw_rect_dsc_t dsc;
        lv_draw_rect_dsc_init(&dsc);
        dsc.radius       = 2;
        dsc.border_width = 0;
        for (int m = 1; m <= 12; m++) {
            int dim = days_in_month(g_year, m);
            int cx  = (m - 1) * 32;
            for (int d = 1; d <= dim; d++) {
                uint8_t mood = mood_at(g_year, m, d);
                dsc.bg_color = lv_color_hex(mood ? MOOD_COLOR[mood - 1] : C_EMPTY);
                dsc.bg_opa   = mood ? LV_OPA_COVER : LV_OPA_60;
                lv_canvas_draw_rect(cv, cx, (d - 1) * 11, 30, 9, &dsc);
            }
        }
    } else {
        ui_label(g_screen, "out of memory for the heatmap", 18, 200,
                 &lv_font_montserrat_12, C_GOLD);
    }

    char stat[64];
    snprintf(stat, sizeof(stat), "%d days logged   |   tap a month",
             count_year(g_year));
    ui_label(g_screen, stat, 18, 476, &lv_font_montserrat_12, C_MUTED);
}

// ---- NOTE EDITOR -----------------------------------------------------------
static void note_save_and_leave() {
    const char *txt = g_note_ta ? lv_textarea_get_text(g_note_ta) : "";
    bool ok = note_write(g_edit_y, g_edit_m, g_edit_d, txt);
    if (!ok) show_toast("Note not saved - SD card missing.");
    build_screen(SCREEN_MONTH);
}
static void note_back_cb(lv_event_t *) { note_save_and_leave(); }
static void note_save_cb(lv_event_t *) { note_save_and_leave(); }
static void note_ready_cb(lv_event_t *) { note_save_and_leave(); }
static void note_clear_cb(lv_event_t *) {
    if (g_note_ta) lv_textarea_set_text(g_note_ta, "");
    if (note_write(g_edit_y, g_edit_m, g_edit_d, ""))
        show_toast("Note cleared.");
    else
        show_toast("SD card missing.");
}

static void build_note() {
    CRUMB(CRUMB_NOTE);
    g_view = SCREEN_NOTE;
    g_screen = ui_root();
    g_note_ta = nullptr;
    g_batt_lbl = nullptr;

    static const char *MN[12] = {"Jan","Feb","Mar","Apr","May","Jun",
                                 "Jul","Aug","Sep","Oct","Nov","Dec"};
    char head[40];
    snprintf(head, sizeof(head), "%s %d, %d", MN[g_edit_m - 1], g_edit_d, g_edit_y);

    ui_button(g_screen, LV_SYMBOL_LEFT, 12, 10, 46, 40, note_back_cb, NULL,
              C_SURFACE, C_CREAM, &lv_font_montserrat_16);
    ui_label(g_screen, head, 70, 17, &lv_font_montserrat_18, C_CREAM);
    uint8_t mood = mood_at(g_edit_y, g_edit_m, g_edit_d);
    if (mood) {
        lv_obj_t *chip = ui_panel(g_screen, 196, 19, 34, 24,
                                  MOOD_COLOR[mood - 1], 8);
        (void)chip;
    }
    ui_button(g_screen, "CLR", 238, 10, 50, 40, note_clear_cb, NULL,
              C_SURFACE, C_MUTED, &lv_font_montserrat_12);
    ui_button(g_screen, "SAVE", 294, 10, 100, 40, note_save_cb, NULL,
              C_ACCENT, C_BG, &lv_font_montserrat_14);

    // Layout from the panel height instead of from fixed numbers: the keyboard
    // is anchored to the bottom edge and the text editor takes whatever is left
    // above it.  The old version placed a 272 px keyboard at y = 222, which put
    // the bottom of the widget (and the row of space / OK keys) at the very
    // edge of the 410 x 502 panel, where LVGL clipped it.
    const int kb_h = 322;
    const int kb_y = LCD_HEIGHT - kb_h;          // 180
    const int ta_y = 58;
    const int ta_h = kb_y - ta_y - 10;           // 112

    lv_obj_t *ta = lv_textarea_create(g_screen);
    g_note_ta = ta;
    lv_obj_set_pos(ta, 14, ta_y);
    lv_obj_set_size(ta, LCD_WIDTH - 28, ta_h);
    lv_textarea_set_one_line(ta, false);
    lv_textarea_set_max_length(ta, 180);
    lv_textarea_set_placeholder_text(ta, "How was this day?");
    lv_obj_set_style_bg_color(ta, lv_color_hex(C_SURFACE), 0);
    lv_obj_set_style_bg_opa(ta, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(ta, 16, 0);
    lv_obj_set_style_border_color(ta, lv_color_hex(C_LINE), 0);
    lv_obj_set_style_border_width(ta, 1, 0);
    lv_obj_set_style_text_color(ta, lv_color_hex(C_CREAM), 0);
    lv_obj_set_style_text_font(ta, &lv_font_montserrat_16, 0);
    lv_obj_set_style_pad_all(ta, 14, 0);
    String existing = note_read(g_edit_y, g_edit_m, g_edit_d);
    if (existing.length()) lv_textarea_set_text(ta, existing.c_str());

    lv_obj_t *kb = lv_keyboard_create(g_screen);
    // lv_keyboard's constructor does lv_obj_align(BOTTOM_MID), and lv_obj_set_pos
    // only writes the x/y *offset* of an aligned object -- it never clears
    // LV_STYLE_ALIGN.  So setting the position of a freshly created keyboard
    // added the offset to the bottom-aligned spot and pushed the widget off the
    // panel.  Clear the alignment first, then position it absolutely.
    lv_obj_set_style_align(kb, LV_ALIGN_DEFAULT, LV_PART_MAIN);
    lv_obj_set_pos(kb, 0, kb_y);
    lv_obj_set_size(kb, LCD_WIDTH, kb_h);
    lv_keyboard_set_textarea(kb, ta);
    lv_keyboard_set_mode(kb, LV_KEYBOARD_MODE_TEXT_LOWER);
    lv_obj_set_style_bg_color(kb, lv_color_hex(C_BG), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(kb, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(kb, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(kb, 2, LV_PART_MAIN);
    lv_obj_set_style_pad_row(kb, 6, LV_PART_MAIN);
    lv_obj_set_style_pad_column(kb, 4, LV_PART_MAIN);
    lv_obj_set_style_text_font(kb, &lv_font_montserrat_14, LV_PART_ITEMS);
    lv_obj_set_style_pad_all(kb, 2, LV_PART_ITEMS);
    lv_obj_set_style_bg_color(kb, lv_color_hex(C_SURFACE), LV_PART_ITEMS);
    lv_obj_set_style_bg_color(kb, lv_color_hex(C_SURFACE2),
                              (lv_style_selector_t)LV_PART_ITEMS |
                              (lv_style_selector_t)LV_STATE_PRESSED);
    lv_obj_set_style_text_color(kb, lv_color_hex(C_CREAM), LV_PART_ITEMS);
    lv_obj_set_style_border_color(kb, lv_color_hex(C_LINE), LV_PART_ITEMS);
    lv_obj_set_style_border_width(kb, 1, LV_PART_ITEMS);
    lv_obj_set_style_radius(kb, 8, LV_PART_ITEMS);
    lv_obj_add_event_cb(kb, note_ready_cb, LV_EVENT_READY, NULL);
    lv_obj_add_event_cb(kb, note_ready_cb, LV_EVENT_CANCEL, NULL);

    Serial.printf("[ui] keyboard %d,%d %dx%d (bottom %d of %d)\n",
                  (int)lv_obj_get_x(kb), (int)lv_obj_get_y(kb),
                  (int)lv_obj_get_width(kb), (int)lv_obj_get_height(kb),
                  (int)lv_obj_get_y2(kb), LCD_HEIGHT);
}

// ---- STATS / ABOUT ---------------------------------------------------------
static void export_cb(lv_event_t *) {
    if (!g_sd_ok) { show_toast("SD card missing - cannot export."); return; }
    ensure_watch_dirs();
    char path[80];
    snprintf(path, sizeof(path), PATH_ROOT "/coffeelings-%d.txt", g_year);
    File f = SD_MMC.open(path, FILE_WRITE);
    if (!f) { show_toast("Export failed."); return; }

    f.printf("COFFEELINGS JOURNAL %d\n", g_year);
    f.println("=====================");
    f.println("Date        Roast      Note");
    f.println("----------  ---------  ------------------------------");
    int written = 0;
    for (int m = 1; m <= 12; m++) {
        int dim = days_in_month(g_year, m);
        for (int d = 1; d <= dim; d++) {
            uint8_t mood = mood_at(g_year, m, d);
            String note = note_read(g_year, m, d);
            if (!mood && !note.length()) continue;
            f.printf("%04d-%02d-%02d  %-9s  ", g_year, m, d,
                     mood ? MOOD_NAME[mood - 1] : "-");
            f.println(note.length() ? note.c_str() : "");
            written++;
        }
    }
    f.printf("\n%d entries  |  %d day streak  |  exported by Coffeelings %s\n",
             written, streak_days(), COFFEELINGS_VERSION);
    f.close();
    show_toast("Saved to /WATCH/Coffeelings");
}

static void launcher_cb(lv_event_t *);
static void info_back_cb(lv_event_t *) { build_screen(SCREEN_HOME); }

static void build_info() {
    CRUMB(CRUMB_BUILD);
    g_view = SCREEN_INFO;
    g_screen = ui_root();
    g_batt_lbl = nullptr;

    ui_button(g_screen, LV_SYMBOL_LEFT, 12, 10, 46, 40, info_back_cb, NULL,
              C_SURFACE, C_CREAM, &lv_font_montserrat_16);
    ui_label(g_screen, "Coffeelings", 70, 14, &lv_font_montserrat_20, C_CREAM);
    char ver[24];
    snprintf(ver, sizeof(ver), "v%s", COFFEELINGS_VERSION);
    ui_label(g_screen, ver, 330, 22, &lv_font_montserrat_12, C_MUTED);

    lv_obj_t *card = ui_panel(g_screen, 14, 62, 382, 132, C_SURFACE, 18);
    char b[80];
    snprintf(b, sizeof(b), "%d", g_year);
    ui_label(card, b, 18, 12, &lv_font_montserrat_14, C_ACCENT);
    snprintf(b, sizeof(b), "%d days logged", count_year(g_year));
    ui_label(card, b, 18, 36, &lv_font_montserrat_22, C_CREAM);
    snprintf(b, sizeof(b), "streak %d days   |   this month %d",
             streak_days(), count_month(g_year, g_month));
    ui_label(card, b, 18, 76, &lv_font_montserrat_14, C_MUTED);
    if (g_clock_valid)
        ui_label(card, "clock shared with LauncherOS", 18, 104,
                 &lv_font_montserrat_12, C_MUTED);
    else
        ui_label(card, "clock unset - using build date", 18, 104,
                 &lv_font_montserrat_12, C_GOLD);

    ui_label(g_screen, "ROAST TALLY", 20, 204, &lv_font_montserrat_12, C_MUTED);
    for (int i = 0; i < MOOD_COUNT; i++) {
        int y = 226 + i * 30;
        ui_panel(g_screen, 20, y, 18, 18, MOOD_COLOR[i], 5);
        ui_label(g_screen, MOOD_NAME[i], 50, y, &lv_font_montserrat_14, C_CREAM);
        char n[12];
        snprintf(n, sizeof(n), "%d", mood_tally(g_year, g_month, i + 1));
        lv_obj_t *cnt = ui_label(g_screen, n, 150, y, &lv_font_montserrat_14, C_MUTED);
        (void)cnt;
    }
    ui_label(g_screen, "this month", 220, 226, &lv_font_montserrat_12, C_MUTED);

    ui_button(g_screen, "EXPORT YEAR AS .TXT", 14, 380, 382, 44, export_cb, NULL,
              C_SURFACE2, C_ACCENT, &lv_font_montserrat_14);
    ui_button(g_screen, "BACK TO LAUNCHEROS", 14, 432, 382, 44, launcher_cb, NULL,
              C_SURFACE2, C_CREAM, &lv_font_montserrat_14);
    char foot[72];
    if (g_boot_fault || g_prev_crash)
        snprintf(foot, sizeof(foot), "boot %s @ %s", g_last_boot, g_last_stage);
    else
        snprintf(foot, sizeof(foot), "%s  |  boot %s",
                 g_sd_ok ? "/WATCH/Coffeelings" : "no SD card", g_last_boot);
    ui_label(g_screen, foot, 20, 480, &lv_font_montserrat_12,
             (g_sd_ok && !g_boot_fault) ? C_MUTED : C_GOLD);
}

// Builds one view.  Only ever called from loop()/setup(), never from inside an
// LVGL callback: build_screen() below turns those calls into a queued request.
static void build_screen_now(uint8_t raw_id) {
    if (raw_id >= SCREEN_COUNT) return;
    ScreenID id = (ScreenID)raw_id;
    lv_disp_t *d = lv_disp_get_default();
    lv_obj_t *target = g_screens[id];

    // Refuse to empty a view LVGL is still cross-fading away from: it lives in
    // disp->prev_scr for the length of the fade and the refresh loop keeps
    // drawing its children.  Try again on the next pass of loop().
    if (target && d && d->prev_scr == target && lv_obj_get_child_cnt(target)) {
        g_pending_view  = id;
        g_pending_valid = true;
        return;
    }

    g_screen     = nullptr;
    g_batt_lbl   = nullptr;
    g_home_time  = nullptr;
    g_home_date  = nullptr;
    g_sheet      = nullptr;
    g_scrim      = nullptr;
    g_note_ta    = nullptr;
    g_view       = id;

    trash_flush();   // pending sheet/scrim first: they are children of target
    if (target) {
        // Same view on screen: clear the input device's pointers into the
        // objects about to disappear before freeing them.
        if (target == lv_scr_act()) lv_indev_reset(nullptr, nullptr);
        lv_obj_clean(target);
    }

    CRUMB(CRUMB_BUILD);
    switch (id) {
        case SCREEN_HOME:  build_home();  break;
        case SCREEN_MONTH: build_month(); break;
        case SCREEN_YEAR:  build_year();  break;
        case SCREEN_NOTE:  build_note();  break;
        case SCREEN_INFO:  build_info();  break;
        default: break;
    }

    if (g_screen) {
        if (g_screen == lv_scr_act()) {
            // Same root, new content: repaint it in place (no fade, LVGL would
            // ignore a load of the screen that is already active).
            lv_obj_invalidate(g_screen);
        } else if (g_prev_crash) {
            // Last boot died somewhere: hard cut instead of a fade for one
            // session, so an animation problem can never become a reboot loop.
            lv_scr_load_anim(g_screen, LV_SCR_LOAD_ANIM_NONE, 0, 0, false);
        } else {
            lv_scr_load_anim(g_screen, LV_SCR_LOAD_ANIM_FADE_ON,
                             COFFEELINGS_FADE_MS, 0, false);
        }
    }
    g_last_touch_ms = millis();
    crumb_mark_ok();

    lv_mem_monitor_t mon;
    lv_mem_monitor(&mon);
    Serial.printf("[ui] screen %s: lvgl used %u%%, %u B free (biggest %u)\n",
                  SCREEN_NAME[id], (unsigned)mon.used_pct,
                  (unsigned)mon.free_size, (unsigned)mon.free_biggest_size);
}

static void build_screen(ScreenID id) {
    // Called from inside an LVGL event (a tap, a keyboard key, ...) or from a
    // touch/gesture callback: queue it.  Rebuilding a screen there would free
    // objects LVGL is still using on its own stack.
    if (g_in_pump) {
        g_pending_view  = id;
        g_pending_valid = true;
        return;
    }
    build_screen_now(id);
}

static void go_home() { build_screen(SCREEN_HOME); }

// ============================================================================
//  10. BOOT SPLASH  (the LauncherOS boot sequence, coffee edition)
// ============================================================================
static lv_obj_t *splash_status = nullptr;
static lv_obj_t *splash_bar = nullptr;

static void anim_opa(void *v, int32_t x) {
    lv_obj_set_style_opa((lv_obj_t *)v, (lv_opa_t)x, 0);
}
static void anim_pulse(void *v, int32_t x) {
    lv_obj_set_size((lv_obj_t *)v, x, x);
    lv_obj_set_style_radius((lv_obj_t *)v, LV_RADIUS_CIRCLE, 0);
}

static void build_splash() {
    splash_screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(splash_screen, lv_color_hex(C_BG), 0);
    lv_obj_set_style_bg_opa(splash_screen, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(splash_screen, 0, 0);
    lv_obj_set_style_border_width(splash_screen, 0, 0);
    lv_obj_clear_flag(splash_screen, LV_OBJ_FLAG_SCROLLABLE);
    lv_scr_load(splash_screen);

    // --- the cup ------------------------------------------------------------
    lv_obj_t *cup = lv_obj_create(splash_screen);
    lv_obj_set_size(cup, 140, 118);
    lv_obj_align(cup, LV_ALIGN_CENTER, 0, -78);
    lv_obj_set_style_bg_opa(cup, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(cup, 0, 0);
    lv_obj_set_style_pad_all(cup, 0, 0);
    lv_obj_clear_flag(cup, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *handle = lv_obj_create(cup);
    lv_obj_set_pos(handle, 80, 32);
    lv_obj_set_size(handle, 34, 34);
    lv_obj_set_style_bg_opa(handle, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_color(handle, lv_color_hex(C_ACCENT), 0);
    lv_obj_set_style_border_width(handle, 6, 0);
    lv_obj_set_style_radius(handle, LV_RADIUS_CIRCLE, 0);
    lv_obj_clear_flag(handle, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *saucer = lv_obj_create(cup);
    lv_obj_set_pos(saucer, 8, 88);
    lv_obj_set_size(saucer, 104, 6);
    lv_obj_set_style_bg_color(saucer, lv_color_hex(C_ACCENT), 0);
    lv_obj_set_style_bg_opa(saucer, LV_OPA_60, 0);
    lv_obj_set_style_border_width(saucer, 0, 0);
    lv_obj_set_style_radius(saucer, 3, 0);

    lv_obj_t *body = lv_obj_create(cup);
    lv_obj_set_pos(body, 14, 27);
    lv_obj_set_size(body, 84, 64);
    lv_obj_set_style_bg_color(body, lv_color_hex(C_ACCENT), 0);
    lv_obj_set_style_bg_opa(body, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(body, 0, 0);
    lv_obj_set_style_radius(body, 12, 0);

    lv_obj_t *crema = lv_obj_create(body);
    lv_obj_set_pos(crema, 0, 0);
    lv_obj_set_size(crema, 84, 16);
    lv_obj_set_style_bg_color(crema, lv_color_hex(C_SAD), 0);
    lv_obj_set_style_bg_opa(crema, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(crema, 0, 0);
    lv_obj_set_style_radius(crema, 10, 0);

    for (int i = 0; i < 3; i++) {
        lv_obj_t *steam = lv_obj_create(cup);
        lv_obj_set_pos(steam, 34 + i * 16, 6 + (i % 2) * 6);
        lv_obj_set_size(steam, 6, 6);
        lv_obj_set_style_bg_color(steam, lv_color_hex(C_ACCENT), 0);
        lv_obj_set_style_bg_opa(steam, LV_OPA_50, 0);
        lv_obj_set_style_border_width(steam, 0, 0);
        lv_obj_set_style_radius(steam, LV_RADIUS_CIRCLE, 0);

        lv_anim_t a;
        lv_anim_init(&a);
        lv_anim_set_var(&a, steam);
        lv_anim_set_exec_cb(&a, anim_opa);
        lv_anim_set_values(&a, 30, 200);
        lv_anim_set_time(&a, 900 + i * 120);
        lv_anim_set_playback_time(&a, 900);
        lv_anim_set_delay(&a, i * 220);
        lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
        lv_anim_set_path_cb(&a, lv_anim_path_ease_in_out);
        lv_anim_start(&a);
    }

    lv_obj_t *glow = lv_obj_create(splash_screen);
    lv_obj_set_size(glow, 26, 26);
    lv_obj_align(glow, LV_ALIGN_CENTER, 0, -78);
    lv_obj_set_style_radius(glow, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(glow, lv_color_hex(C_ACCENT), 0);
    lv_obj_set_style_bg_opa(glow, LV_OPA_20, 0);
    lv_obj_set_style_border_width(glow, 0, 0);
    lv_obj_set_style_shadow_color(glow, lv_color_hex(C_ACCENT), 0);
    lv_obj_set_style_shadow_width(glow, 40, 0);
    lv_obj_set_style_shadow_opa(glow, 120, 0);
    lv_obj_move_background(glow);
    { lv_anim_t a; lv_anim_init(&a); lv_anim_set_var(&a, glow);
      lv_anim_set_exec_cb(&a, anim_pulse);
      lv_anim_set_values(&a, 18, 34);
      lv_anim_set_time(&a, 900);
      lv_anim_set_playback_time(&a, 900);
      lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
      lv_anim_set_path_cb(&a, lv_anim_path_ease_in_out);
      lv_anim_start(&a); }

    // --- title --------------------------------------------------------------
    lv_obj_t *title = lv_label_create(splash_screen);
    lv_label_set_text(title, "Coffeelings");
    lv_obj_set_style_text_color(title, lv_color_hex(C_CREAM), 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_28, 0);
    lv_obj_align(title, LV_ALIGN_CENTER, 0, 6);
    lv_obj_set_style_opa(title, 0, 0);
    { lv_anim_t a; lv_anim_init(&a); lv_anim_set_var(&a, title);
      lv_anim_set_exec_cb(&a, anim_opa);
      lv_anim_set_values(&a, 0, 255);
      lv_anim_set_time(&a, 400);
      lv_anim_set_delay(&a, 80);
      lv_anim_start(&a); }

    lv_obj_t *sub = lv_label_create(splash_screen);
    lv_label_set_text(sub, "J O U R N A L");
    lv_obj_set_style_text_color(sub, lv_color_hex(C_ACCENT), 0);
    lv_obj_set_style_text_font(sub, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_letter_space(sub, 5, 0);
    lv_obj_align(sub, LV_ALIGN_CENTER, 0, 46);
    lv_obj_set_style_opa(sub, 0, 0);
    { lv_anim_t a; lv_anim_init(&a); lv_anim_set_var(&a, sub);
      lv_anim_set_exec_cb(&a, anim_opa);
      lv_anim_set_values(&a, 0, 220);
      lv_anim_set_time(&a, 500);
      lv_anim_set_delay(&a, 250);
      lv_anim_start(&a); }

    // --- progress -----------------------------------------------------------
    splash_bar = lv_bar_create(splash_screen);
    lv_obj_set_size(splash_bar, 220, 3);
    lv_obj_align(splash_bar, LV_ALIGN_BOTTOM_MID, 0, -70);
    lv_obj_set_style_bg_color(splash_bar, lv_color_hex(C_SURFACE2), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(splash_bar, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(splash_bar, lv_color_hex(C_ACCENT), LV_PART_INDICATOR);
    lv_obj_set_style_border_width(splash_bar, 0, LV_PART_MAIN);
    lv_bar_set_range(splash_bar, 0, 100);

    splash_status = lv_label_create(splash_screen);
    lv_label_set_text(splash_status, "booting");
    lv_obj_set_style_text_color(splash_status, lv_color_hex(C_MUTED), 0);
    lv_obj_set_style_text_font(splash_status, &lv_font_montserrat_12, 0);
    lv_obj_align(splash_status, LV_ALIGN_BOTTOM_MID, 0, -42);

    // Footer: hardware line normally, the previous run's last breath after a
    // crash.  This is the one screen every boot shows, so a Guru Meditation is
    // readable on the watch itself, with no PC attached.
    char hwid_txt[64];
    if (g_prev_crash)
        snprintf(hwid_txt, sizeof(hwid_txt), "last run died in %s", g_last_stage);
    else
        snprintf(hwid_txt, sizeof(hwid_txt), "ESP32-S3  |  16 MB  |  8 MB PSRAM");
    lv_obj_t *hwid = lv_label_create(splash_screen);
    lv_label_set_text(hwid, hwid_txt);
    lv_obj_set_style_text_color(hwid,
        lv_color_hex(g_prev_crash ? C_GOLD : 0x6B5446), 0);
    lv_obj_set_style_text_font(hwid, &lv_font_montserrat_12, 0);
    lv_obj_align(hwid, LV_ALIGN_BOTTOM_MID, 0, -16);
    lv_obj_set_style_opa(hwid, 0, 0);
    { lv_anim_t a; lv_anim_init(&a); lv_anim_set_var(&a, hwid);
      lv_anim_set_exec_cb(&a, anim_opa);
      lv_anim_set_values(&a, 0, 180);
      lv_anim_set_time(&a, 700);
      lv_anim_set_delay(&a, 600);
      lv_anim_start(&a); }
}

static void splash_progress(int pct, const char *msg) {
    if (!splash_bar) return;
    lv_bar_set_value(splash_bar, pct, LV_ANIM_ON);
    lv_label_set_text(splash_status, msg);
    uint32_t until = millis() + 220;
    while (millis() < until) { lvgl_pump(); delay(5); }
}

// ============================================================================
//  11. LAUNCHER RETURN / BOOT BUTTON
// ============================================================================
static void accept_running_app() {
    esp_ota_mark_app_valid_cancel_rollback();
    Serial.println("[boot] Coffeelings ready, OTA image accepted");
}

// ---- crash diagnostics -----------------------------------------------------
//  A Guru Meditation on a watch with no serial attached is invisible, and the
//  only trace is that the app "just reboots".  esp_reset_reason() survives the
//  reset in RTC memory, so every boot logs what killed the last one and the
//  info screen shows it.
static void note_reset_reason() {
    esp_reset_reason_t r = esp_reset_reason();
    const char *name = "unknown";
    switch (r) {
        case ESP_RST_POWERON:   name = "power-on";   break;
        case ESP_RST_EXT:       name = "ext-reset";  break;
        case ESP_RST_SW:        name = "soft-reset"; break;
        case ESP_RST_PANIC:     name = "PANIC";      break;
        case ESP_RST_INT_WDT:   name = "int-wdt";    break;
        case ESP_RST_TASK_WDT:  name = "task-wdt";   break;
        case ESP_RST_WDT:       name = "wdt";        break;
        case ESP_RST_DEEPSLEEP: name = "deep-sleep"; break;
        case ESP_RST_BROWNOUT:  name = "brownout";   break;
        default: break;
    }
    snprintf(g_last_boot, sizeof(g_last_boot), "%s", name);
    g_boot_fault = (r == ESP_RST_PANIC || r == ESP_RST_INT_WDT ||
                    r == ESP_RST_TASK_WDT || r == ESP_RST_WDT ||
                    r == ESP_RST_BROWNOUT);
    Serial.printf("[boot] reset reason: %s (%d)\n", name, (int)r);
    if (g_prev_crash)
        Serial.printf("[boot] previous run died in %s -- last UI step started there\n",
                      g_last_stage);
    else
        Serial.printf("[boot] previous run ended cleanly (%s)\n", g_last_stage);
}

// ---- boot guard: three incomplete boots hand the watch back to LauncherOS --
//  Same idea as GameOS: a crash loop must never strand the user in this app.
//  The counter is cleared the moment setup() finishes, so a normal launch is
//  never mistaken for a failed one.
static void boot_guard_start() {
    Preferences p;
    if (!p.begin("coffguard", false)) return;
    uint8_t n = p.getUChar("n", 0) + 1;
    p.putUChar("n", n);
    p.end();
    g_boot_guard_armed = true;
    Serial.printf("[boot] Coffeelings attempt %u/3\n", n);
    if (n < 3) return;

    const esp_partition_t *running = esp_ota_get_running_partition();
    const esp_partition_t *factory = esp_partition_find_first(
        ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_FACTORY, nullptr);
    if (!factory || factory == running) {
        // Standalone build with no separate LauncherOS slot: nothing safe to
        // fall back to, so just reset the counter and let it try again.
        if (p.begin("coffguard", false)) { p.putUChar("n", 0); p.end(); }
        return;
    }
    if (esp_ota_set_boot_partition(factory) == ESP_OK) {
        Serial.println("[boot] 3 incomplete boots -> LauncherOS");
        if (p.begin("coffguard", false)) { p.putUChar("n", 0); p.end(); }
        delay(60);
        esp_restart();
    }
}
static void boot_guard_success() {
    if (!g_boot_guard_armed) return;
    g_boot_guard_armed = false;
    Preferences p;
    if (p.begin("coffguard", false)) { p.putUChar("n", 0); p.end(); }
}

// ---- back to LauncherOS (GameOS protocol) ---------------------------------
static void launcher_return_now() {
    if (g_returning) return;
    g_returning = true;
    moods_save();
    time_publish("exit", false);          // hand the clock back to the launcher
    boot_guard_success();                 // an intentional exit is never a crash

    lv_obj_t *cover = ui_panel(lv_layer_top(), 0, 0, LCD_WIDTH, LCD_HEIGHT, C_BG, 0);
    ui_label_center(cover, "SAVING JOURNAL...", 205, &lv_font_montserrat_20, C_CREAM);
    ui_label_center(cover, "RETURNING TO LAUNCHEROS", 246, &lv_font_montserrat_12, C_MUTED);
    lvgl_pump();

    const esp_partition_t *running = esp_ota_get_running_partition();
    const esp_partition_t *factory = esp_partition_find_first(
        ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_FACTORY, nullptr);
    const esp_partition_t *target = nullptr;
    if (factory && factory != running) {
        target = factory;                 // LauncherOS lives in its own slot
    } else if (!factory) {
        // No factory image at all: fall back to the other OTA slot, like GameOS.
        const esp_partition_t *other = esp_ota_get_next_update_partition(nullptr);
        if (other && other != running) target = other;
    }
    if (!target || esp_ota_set_boot_partition(target) != ESP_OK) {
        lv_obj_del(cover);
        g_returning = false;
        show_toast("No LauncherOS partition to boot into.");
        return;
    }
    fade_to(0, 280);
    crumb_mark_ok();                    // an intentional exit is not a crash
    delay(80);
    esp_restart();
}

static void launcher_cb(lv_event_t *) {
    // Defer the whole exit (cover, fade, partition switch, restart) to loop():
    // it pumps LVGL, which must never happen from inside an LVGL event.
    g_nav = NAV_LAUNCHER;
}

static void poll_boot_button() {
    static uint32_t held = 0;
    if (g_boot_release) {
        if (digitalRead(BOOT_BTN) == HIGH) g_boot_release = false;
        return;
    }
    if (digitalRead(BOOT_BTN) == LOW) {
        if (g_screen_off) { set_screen_off(false); held = millis(); }
        else if (!held) held = millis();
        else if (millis() - held >= COFFEELINGS_BOOT_HOLD_MS) launcher_return_now();
    } else {
        held = 0;
    }
}

// ============================================================================
//  12. HARDWARE INIT / SETUP / LOOP
// ============================================================================
static void hardware_init() {
    CRUMB(CRUMB_HW);
    Serial.begin(115200);
    pinMode(BOOT_BTN, INPUT_PULLUP);
    pinMode(PWR_BTN_PIN, INPUT_PULLUP);
    pinMode(TP_INT, INPUT_PULLUP);
    btStop();
    g_boot_release = digitalRead(BOOT_BTN) == LOW;

    if (!g_gfx->begin()) Serial.println("[gfx] begin failed");
    g_gfx->fillScreen(RGB565_BLACK);
    static_cast<Arduino_CO5300 *>(g_gfx)->setBrightness(0);
    g_cur_brightness = 0;

    // I2C bus recovery, then fast mode.
    pinMode(IIC_SCL, OUTPUT);
    for (int i = 0; i < 9; i++) {
        digitalWrite(IIC_SCL, LOW); delayMicroseconds(5);
        digitalWrite(IIC_SCL, HIGH); delayMicroseconds(5);
    }
    Wire.begin(IIC_SDA, IIC_SCL);
    Wire.setClock(400000);

    pinMode(TP_RESET, OUTPUT);
    digitalWrite(TP_RESET, LOW); delay(50);
    digitalWrite(TP_RESET, HIGH); delay(200);
    for (int i = 0; i < 3 && !g_touch_ok; i++) {
        g_touch_ok = g_touch->begin();
        if (!g_touch_ok) delay(80);
    }
    if (g_touch_ok) {
        g_touch->IIC_Write_Device_State(
            g_touch->Arduino_IIC_Touch::Device::TOUCH_POWER_MODE,
            g_touch->Arduino_IIC_Touch::Device_Mode::TOUCH_POWER_MONITOR);
    } else {
        Serial.println("[touch] FT3168 unavailable");
    }

    pmu_init();

    CRUMB(CRUMB_LVGL);
    lv_init();
    size_t px = LCD_WIDTH * 60;
    g_lvbuf = (lv_color_t *)heap_caps_malloc(px * sizeof(lv_color_t),
                                             MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!g_lvbuf)
        g_lvbuf = (lv_color_t *)heap_caps_malloc(px * sizeof(lv_color_t),
                                                 MALLOC_CAP_SPIRAM);
    if (!g_lvbuf)
        g_lvbuf = (lv_color_t *)heap_caps_malloc(px * sizeof(lv_color_t),
                                                 MALLOC_CAP_8BIT);
    if (!g_lvbuf) {
        Serial.println("[lvgl] fatal draw buffer allocation");
        while (true) delay(1000);
    }
    lv_disp_draw_buf_init(&g_draw_buf, g_lvbuf, nullptr, px);
    lv_disp_drv_init(&g_disp_drv);
    g_disp_drv.flush_cb = flush_cb;
    g_disp_drv.draw_buf = &g_draw_buf;
    g_disp_drv.hor_res = LCD_WIDTH;
    g_disp_drv.ver_res = LCD_HEIGHT;
    lv_disp_drv_register(&g_disp_drv);

    lv_indev_drv_init(&g_indev_drv);
    g_indev_drv.type = LV_INDEV_TYPE_POINTER;
    g_indev_drv.read_cb = touch_cb;
    g_indev = lv_indev_drv_register(&g_indev_drv);

    // Empties views that have finished their cross-fade (never frees a root).
    g_idle_clean_timer = lv_timer_create(screen_idle_clean_cb, 50, NULL);
}

void setup() {
    crumb_read();
    boot_guard_start();
    CRUMB(CRUMB_BOOT);
    esp_sleep_wakeup_cause_t wake = esp_sleep_get_wakeup_cause();
    g_fast_wake = (wake == ESP_SLEEP_WAKEUP_EXT1);
    hardware_init();
    note_reset_reason();

    time_restore_from_nvs();
    g_last_time_pub_ms = millis();
    clock_refresh();
    if (g_clock_valid) {
        // Open on the real month, not on whatever the compiled default was.
        g_year  = g_today.tm_year + 1900;
        g_month = g_today.tm_mon + 1;
    } else {
        date_fallback();
        Serial.println("[clock] build-date fallback in use");
    }

    if (!g_fast_wake) {
        build_splash();
        lvgl_pump();
        fade_to(COFFEELINGS_BRIGHTNESS, 260);
    } else {
        Serial.println("[pm] wake from deep sleep - skipping splash");
    }
    splash_progress(15, "init display");
    splash_progress(32, "opening journal");
    storage_init();
    splash_progress(56, g_sd_ok ? "mounting sd card" : "no sd card");
    nvs_migrate_namespace();
    moods_load();
    splash_progress(72, "loading roasts");
    clock_refresh();
    splash_progress(88, g_clock_valid ? "clock synced" : "clock not set");
    splash_progress(100, "ready");
    delay(360);

    if (g_pmu_ok) {
        g_battery = g_pmu.getBatteryPercent();
        g_charging = g_pmu.isCharging();
    }

    build_screen(SCREEN_HOME);
    // The splash is done: drop the handles so splash_progress() can never touch
    // stale objects again.  The screen itself is emptied by the idle cleaner,
    // which also stops its five infinite steam/glow animations.
    splash_bar    = nullptr;
    splash_status = nullptr;
    if (g_fast_wake) fade_to(COFFEELINGS_BRIGHTNESS, COFFEELINGS_WAKE_FADE_MS);
    lvgl_pump();
    boot_guard_success();
    accept_running_app();
    g_last_touch_ms = g_last_wake_ms = millis();

#if COFFEELINGS_POWER_SAVE
    setCpuFrequencyMhz(80);
    Serial.printf("[pm] CPU @ %lu MHz\n", (unsigned long)getCpuFrequencyMhz());
#endif
}

void loop() {
    lvgl_pump();

    // ---- deferred screen build --------------------------------------------
    //  Queued by any tap/key handler while LVGL was running.  Built here, in
    //  the loop task with nothing of LVGL's on the stack.
    if (g_pending_valid) {
        g_pending_valid = false;
        build_screen_now(g_pending_view);
    }

    // ---- deferred navigation (queued by touch_cb / launcher_cb) -----------
    //  Done here, after the pump has returned, so screen rebuilds, fades and
    //  the exit hand-off never re-enter LVGL from inside its own callback.
    NavAction nav = g_nav;
    if (nav != NAV_NONE) {
        g_nav = NAV_NONE;
        switch (nav) {
            case NAV_HOME:       if (!g_sheet) go_home(); break;
            case NAV_PREV_MONTH: month_step(-1); break;
            case NAV_NEXT_MONTH: month_step(1);  break;
            case NAV_LAUNCHER:   launcher_return_now(); break;
            default: break;
        }
    }

    // ---- shared clock / midnight rollover ---------------------------------
    if (millis() - g_last_tick_ms >= 1000) {
        g_last_tick_ms = millis();
        struct tm before = g_today;
        clock_refresh();
        bool flipped = before.tm_year != g_today.tm_year ||
                       before.tm_mon  != g_today.tm_mon  ||
                       before.tm_mday != g_today.tm_mday;
        // Repaint so "today" and the streak move, but never yank the user
        // out of a year they are browsing.
        if (flipped && (g_view == SCREEN_HOME || g_view == SCREEN_MONTH ||
                        g_view == SCREEN_YEAR)) {
            build_screen(g_view);
        }
        // Live watch face: only touch the labels when the text actually changes.
        if (g_view == SCREEN_HOME && g_clock_valid) {
            char hm[8];
            strftime(hm, sizeof(hm), "%H:%M", &g_today);
            if (g_home_time && strcmp(lv_label_get_text(g_home_time), hm) != 0)
                lv_label_set_text(g_home_time, hm);
            char ds[48];
            strftime(ds, sizeof(ds), "%A %d %B", &g_today);
            if (g_home_date && strcmp(lv_label_get_text(g_home_date), ds) != 0)
                lv_label_set_text(g_home_date, ds);
        }
        if (g_batt_lbl && g_battery >= 0) {
            char b[24];
            snprintf(b, sizeof(b), "%s%d%%", g_charging ? "+" : "", g_battery);
            lv_label_set_text(g_batt_lbl, b);
        }
    }

    // ---- battery ----------------------------------------------------------
    uint32_t now = millis();
    if (now - g_last_time_pub_ms >= 300000UL) {   // keep the shared clock current
        g_last_time_pub_ms = now;
        time_publish("5min", false);
    }
    if (g_pmu_ok && now - g_last_batt_ms > 10000) {
        g_last_batt_ms = now;
        g_battery = g_pmu.getBatteryPercent();
        g_charging = g_pmu.isCharging();
    }

    // ---- power ------------------------------------------------------------
    poll_power_button();
    if (!g_screen_off && g_view != SCREEN_NOTE &&
        !g_touch_active && now - g_last_wake_ms > COFFEELINGS_WAKE_GRACE_MS &&
        now - g_last_touch_ms >= COFFEELINGS_SCREEN_TIMEOUT_MS) {
        set_screen_off(true);
    }
#if COFFEELINGS_POWER_SAVE
    if (g_screen_off && g_view != SCREEN_NOTE) {
        // Light sleep keeps the display path warm so a touch wakes instantly;
        // after 30 s off it is worth the reboot to actually save the battery.
        if (g_screen_off_since &&
            now - g_screen_off_since >= COFFEELINGS_DEEP_SLEEP_MS) {
            enter_deep_sleep();
        }
        esp_sleep_enable_ext1_wakeup(1ULL << BOOT_BTN, ESP_EXT1_WAKEUP_ANY_LOW);
        esp_sleep_enable_timer_wakeup((uint64_t)COFFEELINGS_SLEEP_POLL_MS * 1000ULL);
        esp_light_sleep_start();
        auto why = esp_sleep_get_wakeup_cause();
        if (why == ESP_SLEEP_WAKEUP_EXT1 || digitalRead(TP_INT) == LOW)
            set_screen_off(false);
    }
#endif

    poll_boot_button();
    delay(5);
}
