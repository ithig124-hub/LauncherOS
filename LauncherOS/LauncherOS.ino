// ============================================================================
//  LauncherOS.ino   --   DualCore Watch launcher  (LVGL 8.3 / 8.4)
//  ----------------------------------------------------------------------------
//  v5.14 -- "Post-swipe click-guard" (kills accidental app launch).
//      * Real cause of "screen turns off on second swipe": rapid flicks
//        that don't travel >25 px register as taps (dx=0 dy=0).  On the
//        Apps screen those taps hit an app tile -> load_app_from_sd ->
//        the panel shows the "Loading..." screen, fades down, reboots.
//        From the user's POV that looks identical to "the screen turned
//        off."
//      * Fix: CLICK_AFTER_SWIPE_MS = 350 ms guard.  After any swipe is
//        consumed, all LVGL CLICKED events on the watch face, app list,
//        WiFi reconnect button and Sleep Now button are suppressed for
//        350 ms.  A genuine deliberate tap still works (just wait 1/3 s
//        after the swipe); a rapid follow-up flick can no longer
//        accidentally launch anything.
//      * Log line "[TOUCH] click suppressed (post-swipe guard)" so you
//        can confirm in the serial monitor that it's firing.
//  ----------------------------------------------------------------------------
//  v5.13 -- release hysteresis (FT3168 single-sample fingers=0 flicker).
//      * THE real root cause of tap-to-off: the FT3168 occasionally
//        returns fingers=0 for a single sample mid-touch (I2C flicker /
//        capacitive noise).  Without hysteresis, that one bad sample
//        fires a false release, touchActive flips to false, and the
//        screen-off countdown starts running even though the finger
//        never actually lifted.
//      * Fix: require RELEASE_HYSTERESIS = 3 consecutive fingers=0 reads
//        before declaring the finger gone.  At LVGL's ~30 ms indev
//        poll that's ~90 ms -- imperceptible to humans but easily long
//        enough to outlast any single-sample blip.
//      * Dropped the v5.12 `digitalRead(TP_INT) == LOW` keep-alive layer:
//        TP_INT pulses low on each new-data IRQ rather than holding low
//        while a finger is on the panel, so it was an unreliable signal.
//        Now that touchActive is hysteresis-backed, it's reliable alone.
//        (TP_INT is still used for the WAKE-from-off path, which only
//        needs to catch the pulse, not measure duration.)
//  ----------------------------------------------------------------------------
//  v5.12 -- belt + braces keep-alive (superseded by v5.13 hysteresis).
//      * SCREEN_OFF_TIMEOUT_MS bumped 5000 -> 15000.  5 s was too short
//        for a watch face; 15 s matches typical smartwatch behaviour.
//      * Three INDEPENDENT keep-alive layers, ANY of which suspends the
//        auto-timeout:
//          (1) touchActive flag (from indev callback)
//          (2) digitalRead(TP_INT) == LOW  -- the hardware "finger on
//              panel" line.  Bypasses I2C-polling flicker where the
//              FT3168 momentarily reports fingers=0 mid-touch and the
//              indev callback fires a spurious release.
//          (3) WAKE_GRACE_MS = 2000 ms post-wake window during which
//              the auto-timeout cannot fire at all.  Stops any "tap to
//              wake -> immediately falls asleep again" edge case.
//      * Net effect: while ANY of (touch-active OR TP-INT-low OR within
//        2 s of wake) is true, lastTouchMs is forced to `now` -- the
//        timer literally cannot count down.
//  ----------------------------------------------------------------------------
//  v5.11 -- side PWR button toggle restored (it wasn't the tap-off cause).
//      * Side button short press goes back to toggling screen on/off, but
//        the 400 ms debounce from v5.10 is kept so a noisy IRQ line can't
//        double-fire and immediately toggle back.
//  ----------------------------------------------------------------------------
//  v5.10 -- attempted fix (REVERTED): made the side PWR button wake-only,
//        thinking the AXP2101 PKEY IRQ was firing falsely from touch noise.
//        User confirmed the tap-to-off bug persisted with the side button
//        disabled, so the side button was NOT the cause -- toggle restored.
//  ----------------------------------------------------------------------------
//  v5.9 -- "Pause-while-touching" pass.
//      * Screen-off countdown is now LITERALLY paused while a finger is on
//        the panel (touchActive == true).  Before, only press *samples*
//        reset the timer -- if the indev poll skipped a beat under load,
//        a long swipe that straddled the deadline could fire screen_off()
//        mid-gesture.  Now the timer cannot tick down at all while you're
//        touching the watch, no matter how slow the swipe.
//  ----------------------------------------------------------------------------
//  v5.8 -- "Smooth everywhere" pass.
//      * New fade_brightness(target, dur_ms) helper -- ramps the AMOLED
//        in 12 steps and pumps LVGL between each step.  No more snap-bright
//        / snap-black anywhere on the device.
//      * screen_on  fades 0 -> 180 in 200 ms (after repainting the new
//        frame first, so users never see a stale buffer brighten).
//      * screen_off fades 180 -> 0 in 220 ms instead of slamming to black.
//      * App launch: copy-screen loads with a 180 ms FADE_ON cross-fade,
//        then the panel fades down to 0 over 280 ms BEFORE esp_restart()
//        so the chip-reboot moment looks intentional, not crashy.
//      * Boot: panel starts at brightness 0; we render the first splash
//        frame then fade up to 180 over 260 ms.  Power-on is a soft rise.
//      * Splash -> watch face transitions with a 260 ms FADE_ON instead
//        of the old hard cut.
//      * deep_sleep_now() fades to black before powering down.
//  ----------------------------------------------------------------------------
//  v5.7 -- "Calmer boot + bulletproof tap-timeout" pass.
//      * Splash redesigned: dropped the 3 concentric pulsing/rotating arcs
//        and the top-to-bottom scanline sweep (felt like "huge cycles").
//        Replaced with a single soft pulsing dot, static title, thin
//        progress bar, subtle hardware-ID footer.  Reads as calm + premium.
//      * Removed letter-by-letter slide-in -- title fades in cleanly instead.
//      * Screen-off countdown is now explicitly "5 s timer that resets on
//        every tap": press samples, swipe-release, screen switches and
//        wake-taps all stamp `lastTouchMs`, closing the edge case where a
//        flick landing near the deadline could fire screen_off() mid-swipe.
//  ----------------------------------------------------------------------------
//  v5.6 -- "MiniOS-parity smooth switching" pass.
//      * Removed NAV_COOLDOWN_MS gating in goto_screen.  Back-to-back swipes
//        now fire at the speed of the finger, just like S3_MiniOS_206.
//      * Removed g_suppress_touch_until / WAKE_TOUCH_SUPPRESS_MS dead-weight
//        (it was already 0 -- the dispatch branch is gone entirely).
//      * Removed rounder_event_cb.  MiniOS doesn't snap flush regions and
//        the CO5300 panel handles unaligned areas fine -- saves a few cycles
//        on every flush, so the post-swipe redraw lands faster.
//      * Screen transition changed from LV_SCR_LOAD_ANIM_NONE (jarring) to
//        a 120 ms LV_SCR_LOAD_ANIM_FADE_ON cross-fade.  Reads as "instant"
//        to the eye but masks the swap -- this is the MiniOS feel.
//      * Tap detection consolidated: PR -> REL drives LVGL clickable widgets
//        directly (matches MiniOS, no parallel tap event needed).
//      * TAP_THRESHOLD_PX = 20, SWIPE_THRESHOLD_PX = 25  (1:1 with MiniOS).
//  ----------------------------------------------------------------------------
//  v5.2 fixes (swipe-not-working repair):
//      * my_touchpad_read no longer clears FT3168->IIC_Interrupt_Flag.
//        That stomp was preventing the Arduino_FT3x68 driver from
//        returning fresh X/Y mid-drag, so swipes stalled at the start
//        coordinate (dx/dy ~ 0) and never crossed the threshold.
//        Tap-to-wake kept working (a single PR event was registered)
//        but the carousel never advanced. Now matches the proven
//        S3_MiniOS_206 indev pattern exactly.
//      * goto_screen() normalises the target index BEFORE the
//        same-screen guard, so 2 -> 0 and 0 -> 2 transitions work.
//      * SWIPE_THRESHOLD 30 -> 25 px to match the MiniOS feel.
//      * USBSerial swipe debug lines kept (TOUCH Start / End / Swipe).
//  ----------------------------------------------------------------------------
//  v5 changes (unchanged):
//      - tileview removed; three independent screens.
//      - Screens: 0 = WATCH FACE, 1 = APPS, 2 = BATTERY.
//      - Manual touch-delta swipe detection inside the indev callback.
//      - Battery screen with big percentage arc + "X h Y min" estimate.
//      - Boot splash with letter-by-letter title slide.
//      - Favourites, wallpapers, NTP, BOOT return helper, OTA loader,
//        screen-off, etc.
// ============================================================================

#include <Arduino.h>
#include <FS.h>
#include <SD_MMC.h>
#include <Wire.h>
#include <WiFi.h>
#include <Preferences.h>
#include <time.h>
#include <sys/time.h>
#include <memory>

#include "Arduino_GFX_Library.h"
#include "Arduino_DriveBus_Library.h"
#include "lv_conf.h"
#include "lvgl.h"

#define XPOWERS_CHIP_AXP2101
#include "XPowersLib.h"

#include <esp_ota_ops.h>
#include <esp_partition.h>
#include <esp_image_format.h>
#include <esp_system.h>
#include <esp_sleep.h>
#include <esp_bt.h>
#include <esp32-hal-cpu.h>

#include "HWCDC.h"
HWCDC USBSerial;

// ============================================================================
//  POWER-SAVING TUNABLES   (set POWER_SAVE 0 to disable everything below)
// ============================================================================
#define POWER_SAVE              0

// Drop CPU from 240 MHz to this when sitting on a static screen.
// 80 MHz still runs the AMOLED + LVGL smoothly and roughly halves CPU draw.
#define CPU_FREQ_IDLE_MHZ      80
#define CPU_FREQ_BUSY_MHZ     240

// Default panel brightness (0-255).  Anything below ~70% is barely noticeable
// outdoors but saves real current on an AMOLED.
#define DEFAULT_BRIGHTNESS    180

// Once NTP has synced, WiFi is turned OFF entirely.  We turn it back on
// every WIFI_RESYNC_INTERVAL_MS to refresh the clock then shut it again.
// 6 h is a good balance: ~6 s of WiFi every 6 hours = <0.05 % radio uptime.
#define WIFI_RESYNC_INTERVAL_MS  (6UL * 60UL * 60UL * 1000UL)   // 6 hours

// When the AMOLED is off we just stop rendering and call displayOff().
// CPU stays awake but the loop is mostly idle so draw is modest (~25 mA).
// For a real "shut me down until I touch the BOOT button" mode, use the
// "Sleep Now" button on the Battery card -- it enters esp_deep_sleep with
// the internal RTC retained.  Set this to 0 to disable the auto-flickery
// light sleep entirely (recommended, it was annoying).
#define LIGHT_SLEEP_WHEN_OFF    0
// ============================================================================

// ============================================================================
//  TUNABLES
// ============================================================================
#define USE_CUSTOM_FONT          0
#if USE_CUSTOM_FONT
    extern const lv_font_t oswald_72;
    #define FACE_TIME_FONT  oswald_72
#else
    #define FACE_TIME_FONT  lv_font_montserrat_48
#endif

#define SCREEN_OFF_TIMEOUT_MS   15000     // 15 s of no touch -> screen off
#define WAKE_GRACE_MS            2000     // for 2 s after wake the auto-timeout is suspended
#define CLICK_AFTER_SWIPE_MS      350     // ignore clicks for 350 ms after a swipe (kills the "rapid-flick tapped an app tile" bug)
// --- MiniOS-parity swipe tuning (v5.6) ------------------------------------
// MiniOS dispatches NAV events the instant the finger releases -- no cooldown,
// no wake suppression, no rounder pass. We mirror that exactly here.
#define SWIPE_THRESHOLD_PX         25     // pixels of horizontal travel to count (matches MiniOS)
#define SWIPE_MAX_DURATION_MS     800     // anything longer is ignored
#define TAP_THRESHOLD_PX           20     // matches MiniOS TAP_THRESHOLD (tap vs swipe split)
// Screen-switch transition: 120 ms cross-fade reads as "instant + smooth"
// to the eye -- LV_SCR_LOAD_ANIM_NONE is jarring, a slide animation is too slow.
#define SCREEN_SWITCH_ANIM_MS     120

// Battery runtime estimate
#define BATT_CAPACITY_MAH         400     // tune to your pack
#define BATT_DRAW_ACTIVE_MA       120     // typical with AMOLED + WiFi
#define BATT_DRAW_SCREENOFF_MA     35

// ---- hardcoded WiFi fallback (matches S3_MiniOS_206) ----------------------
static const char *HC_WIFI_SSID = "Optus_9D2E3D";
static const char *HC_WIFI_PASS = "snucktemptGLeQU";

// ============================================================================
//  PIN MAP
// ============================================================================
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

// Side power / sleep button -- GPIO 10 on this board (idles HIGH, LOW when pressed).
#define PWR_BTN_PIN  10

// ============================================================================
//  Display + touch + LVGL
// ============================================================================
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

static uint32_t lastTouchMs = 0;
static uint32_t lastWakeMs  = 0;              // set by screen_on(); enables WAKE_GRACE_MS window
static uint32_t lastSwipeMs = 0;              // set on every successful swipe -- powers CLICK_AFTER_SWIPE_MS guard
static bool     screenIsOff = false;

// Forward-declared so load_app_from_sd() (defined earlier in the file) can
// fade the panel down before esp_restart().
static void fade_brightness(uint8_t target, uint32_t dur_ms);
// v6.0: anchored at setup() so reboots from loaded apps land back here.
static void anchor_launcher_as_boot();

// ---- swipe queue ----------------------------------------------------------
// MiniOS-style: filled on finger-release in my_touchpad_read(), consumed
// immediately by loop().  No cooldown, no wake-grace gating.
enum SwipeEvent { SWIPE_NONE, SWIPE_LEFT, SWIPE_RIGHT };
static volatile SwipeEvent g_pending_swipe = SWIPE_NONE;

static int32_t  ts_x = 0, ts_y = 0;
static uint32_t ts_ms = 0;
static int32_t  tc_x = 0, tc_y = 0;
static bool     touchActive = false;

static void my_disp_flush(lv_disp_drv_t *drv, const lv_area_t *area,
                          lv_color_t *color_p) {
    uint32_t w = lv_area_get_width(area);
    uint32_t h = lv_area_get_height(area);
    gfx->draw16bitRGBBitmap(area->x1, area->y1, (uint16_t *)&color_p->full, w, h);
    lv_disp_flush_ready(drv);
}

static void my_touchpad_read(lv_indev_drv_t *drv, lv_indev_data_t *data) {
    // ------------------------------------------------------------------
    //  v5.6 swipe handler -- MiniOS-parity, no cooldown, no suppression.
    //
    //  Read order matches S3_MiniOS_206 exactly: X, Y, FINGER_NUMBER.
    //  The Arduino_FT3x68 driver caches the register block on the
    //  FIRST IIC_Read call, so reading coordinates first guarantees
    //  the X/Y match the finger-count we then read.
    //
    //  On release we classify the gesture exactly like MiniOS:
    //      |dx| > SWIPE_THRESHOLD_PX && |dx| > |dy|  -> NAV swipe
    //      |dx| < TAP_THRESHOLD_PX && |dy| < TAP_THRESHOLD_PX -> tap
    //      (taps are forwarded to LVGL via the PR/REL state machine,
    //       so LVGL clickable widgets fire LV_EVENT_CLICKED unchanged.)
    // ------------------------------------------------------------------
    int32_t x       = FT3168->IIC_Read_Device_Value(FT3168->Arduino_IIC_Touch::Value_Information::TOUCH_COORDINATE_X);
    int32_t y       = FT3168->IIC_Read_Device_Value(FT3168->Arduino_IIC_Touch::Value_Information::TOUCH_COORDINATE_Y);
    int     fingers = FT3168->IIC_Read_Device_Value(FT3168->Arduino_IIC_Touch::Value_Information::TOUCH_FINGER_NUMBER);

    bool pressed = (fingers > 0);

    // ------------------------------------------------------------------
    //  v5.13 -- release hysteresis.
    //  The FT3168 occasionally returns fingers=0 for a single sample
    //  mid-touch (I2C flicker / capacitive noise).  Without hysteresis
    //  that one bad sample fires a false release -> touchActive=false,
    //  lastTouchMs frozen, and the 15-second screen-off clock starts
    //  running even though the finger never actually lifted.  This was
    //  the real "tap-to-off" bug.
    //
    //  Fix: require RELEASE_HYSTERESIS consecutive fingers=0 reads
    //  before we believe the finger is actually gone.  At LVGL's
    //  default 30 ms indev poll, 3 in a row = ~90 ms -- imperceptible
    //  to humans but easily long enough to outlast any single-sample
    //  blip from the touch controller.
    // ------------------------------------------------------------------
    static int zero_streak = 0;
    const int  RELEASE_HYSTERESIS = 3;
    if (pressed) {
        zero_streak = 0;
    } else if (touchActive) {
        zero_streak++;
        if (zero_streak < RELEASE_HYSTERESIS) {
            // Treat as still pressed -- hold position, keep timer alive.
            pressed = true;
            x = tc_x;
            y = tc_y;
        }
    }

    if (pressed) {
        data->state   = LV_INDEV_STATE_PR;
        data->point.x = x;
        data->point.y = y;
        lastTouchMs   = millis();

        if (!touchActive) {
            touchActive = true;
            ts_x = x; ts_y = y; ts_ms = millis();
            USBSerial.printf("[TOUCH] Start at (%d, %d)\n", x, y);
        }
        tc_x = x; tc_y = y;
    } else {
        data->state = LV_INDEV_STATE_REL;

        if (touchActive) {
            touchActive   = false;
            uint32_t dur  = millis() - ts_ms;
            int32_t  dx   = tc_x - ts_x;
            int32_t  dy   = tc_y - ts_y;
            bool gestureSwipe = (dur < SWIPE_MAX_DURATION_MS) &&
                                (abs(dx) > SWIPE_THRESHOLD_PX) &&
                                (abs(dx) > abs(dy));

            USBSerial.printf("[TOUCH] End  dx=%d dy=%d dur=%lu\n",
                             dx, dy, (unsigned long)dur);

            if (gestureSwipe) {
                // MiniOS dispatches the moment the finger lifts -- no cooldown,
                // no queue draining, no wake-grace. Match it exactly.
                g_pending_swipe = (dx < 0) ? SWIPE_LEFT : SWIPE_RIGHT;
                USBSerial.printf("[TOUCH] Swipe %s queued\n",
                                 (dx < 0) ? "LEFT" : "RIGHT");
            }
            // Re-stamp the activity timer on release.  The screen-off
            // countdown is a 5-second timer that any touch -- press OR
            // release -- resets back to zero.  Stamping here closes the
            // edge case where a fast flick that lifts very close to the
            // SCREEN_OFF_TIMEOUT_MS deadline could fire screen_off()
            // between the release and the next press sample.
            lastTouchMs = millis();
            // Tap path (abs(dx) < TAP_THRESHOLD_PX && abs(dy) < TAP_THRESHOLD_PX)
            // needs no extra work: the PR -> REL transition above already drove
            // LVGL, so any clickable widget under the finger has fired its
            // LV_EVENT_CLICKED handler in the normal LVGL pipeline.
        }
    }
}

// rounder_event_cb removed in v5.6 -- MiniOS doesn't snap flush regions and
// the panel handles unaligned regions fine.  Dropping it shaves a few cycles
// off every LVGL flush, helping the post-swipe redraw feel snappier.

static void lvgl_pump() { lv_timer_handler(); }

// ============================================================================
//  AXP2101 PMU
// ============================================================================
XPowersPMU g_pmu;
static bool g_has_pmu = false;
static int  g_batt_pct = -1;
static bool g_charging = false;
static int  g_batt_mv  = 0;

static void pmu_init() {
    if (g_pmu.begin(Wire, AXP2101_SLAVE_ADDRESS, IIC_SDA, IIC_SCL)) {
        g_has_pmu = true;
        g_pmu.disableTSPinMeasure();
        g_pmu.enableBattVoltageMeasure();

        // --- Side PWR button (wired to AXP2101 PWRKEY, not a GPIO) ---------
        // Mirrors the official Waveshare 05_LVGL_AXP2101_ADC_Data demo.
        // We arm the short-press IRQ and clear any stale state so the very
        // first poll in loop() doesn't immediately trigger.
        g_pmu.disableIRQ(XPOWERS_AXP2101_ALL_IRQ);
        g_pmu.clearIrqStatus();
        g_pmu.enableIRQ(XPOWERS_AXP2101_PKEY_SHORT_IRQ |
                        XPOWERS_AXP2101_PKEY_LONG_IRQ);
    }
}
static void pmu_refresh() {
    if (!g_has_pmu) return;
    g_batt_pct = g_pmu.getBatteryPercent();
    g_charging = g_pmu.isCharging();
    g_batt_mv  = g_pmu.getBattVoltage();
}

// Returns minutes remaining, or -1 when charging / unknown.
static int batt_minutes_left() {
    if (!g_has_pmu || g_batt_pct < 0 || g_charging) return -1;
    float draw = screenIsOff ? BATT_DRAW_SCREENOFF_MA : BATT_DRAW_ACTIVE_MA;
    float mah_left = BATT_CAPACITY_MAH * (g_batt_pct / 100.0f);
    return (int)((mah_left / draw) * 60.0f);
}

// ============================================================================
//  WiFi / NTP
// ============================================================================
struct WifiNet { String ssid, pass; };
static WifiNet g_nets[5];
static int     g_net_count = 0;
static int     g_gmt_offset_sec = 0;
static bool    g_wifi_connected = false;
static bool    g_ntp_synced     = false;
static uint32_t g_last_sync_ms  = 0;

// ---- v6.1: persistent wall-clock (survives full power loss) ---------------
// The ESP32 internal RTC counts time across deep sleep, but only while the
// chip stays powered.  A full unplug / dead battery wipes it.  We therefore
// also snapshot the current unix time to NVS at every sync point and on
// every periodic tick, so a cold boot can resume the wall clock instantly --
// no NTP round-trip needed for the face to show a sensible time.
static Preferences g_timeprefs;
static uint32_t    g_last_time_save_ms = 0;
#define TIME_PERSIST_INTERVAL_MS  (5UL * 60UL * 1000UL)   // save every 5 min

static void time_save_now(const char *why) {
    time_t now;
    time(&now);
    if (now < 1700000000) return;     // pre-2023 -> RTC not set yet, nothing useful to save
    g_timeprefs.begin("rtcsave", false);
    g_timeprefs.putULong("t", (uint32_t)now);
    g_timeprefs.putLong("gmt", g_gmt_offset_sec);
    g_timeprefs.end();
    g_last_time_save_ms = millis();
    USBSerial.printf("[rtc] persisted t=%lu (%s)\n", (unsigned long)now, why);
}

// Pull the last-known wall clock out of NVS and shove it into the RTC.
// Called at the very top of setup() -- guarantees the watch face shows
// a plausible time even if the battery was dead, before WiFi/NTP runs.
static void time_restore_from_nvs() {
    g_timeprefs.begin("rtcsave", true);
    uint32_t t   = g_timeprefs.getULong("t", 0);
    int32_t  gmt = g_timeprefs.getLong("gmt", 0);
    g_timeprefs.end();
    if (t < 1700000000) {
        USBSerial.println("[rtc] no persisted time -- waiting for NTP");
        return;
    }
    g_gmt_offset_sec = gmt;
    struct timeval tv = { (time_t)t, 0 };
    settimeofday(&tv, NULL);
    g_ntp_synced = true;   // show a real time on the face immediately
    USBSerial.printf("[rtc] restored t=%lu gmt=%ld from NVS\n",
                     (unsigned long)t, (long)gmt);
}

static void wifi_shutdown() {
    // Disconnect, drop the radio, stop the WiFi driver.
    // Saves ~70-100 mA continuous when we don't need it.
    WiFi.disconnect(true, true);
    WiFi.mode(WIFI_OFF);
    g_wifi_connected = false;
}

static void parse_wifi_config() {
    File f = SD_MMC.open("/WATCH/wifi/config.txt", FILE_READ);
    if (!f) return;
    while (f.available()) {
        String line = f.readStringUntil('\n');
        line.trim();
        if (!line.length() || line.startsWith("#")) continue;
        int eq = line.indexOf('=');
        if (eq < 0) continue;
        String key = line.substring(0, eq); key.trim();
        String val = line.substring(eq + 1); val.trim();
        for (int i = 1; i <= 5; i++) {
            if (key == String("WIFI") + i + "_SSID" && val.length() &&
                val != "YourHomeNetwork" && val != "YourWorkNetwork") {
                if (g_net_count < 5) g_nets[g_net_count].ssid = val;
            }
            if (key == String("WIFI") + i + "_PASS" && val.length()) {
                int idx = g_net_count;
                if (idx < 5 && g_nets[idx].ssid.length() &&
                    !g_nets[idx].pass.length()) {
                    g_nets[idx].pass = val;
                    g_net_count++;
                }
            }
        }
        if (key == "GMT_OFFSET") g_gmt_offset_sec = val.toInt() * 3600;
    }
    f.close();
}

static void wifi_connect_and_sync() {
    WiFi.mode(WIFI_STA);
    for (int i = 0; i < g_net_count && !g_wifi_connected; i++) {
        WiFi.begin(g_nets[i].ssid.c_str(), g_nets[i].pass.c_str());
        uint32_t u = millis() + 6000;
        while (millis() < u && WiFi.status() != WL_CONNECTED) delay(100);
        g_wifi_connected = (WiFi.status() == WL_CONNECTED);
    }
    if (!g_wifi_connected) {
        WiFi.begin(HC_WIFI_SSID, HC_WIFI_PASS);
        uint32_t u = millis() + 6000;
        while (millis() < u && WiFi.status() != WL_CONNECTED) delay(100);
        g_wifi_connected = (WiFi.status() == WL_CONNECTED);
    }
    if (g_wifi_connected) {
        configTime(g_gmt_offset_sec, 0, "pool.ntp.org");
        struct tm tm;
        for (int i = 0; i < 30 && !g_ntp_synced; i++)
            if (getLocalTime(&tm, 200)) g_ntp_synced = true;
        if (g_ntp_synced) time_save_now("ntp ok");
    }

#if POWER_SAVE
    // Once the clock is set (or even if it failed), drop WiFi entirely.
    // It'll come back periodically via wifi_resync_if_due().
    g_last_sync_ms = millis();
    wifi_shutdown();
#endif
}

#if POWER_SAVE
// Re-wake WiFi every few hours, top up the RTC, drop it again.  Runs from
// loop() so it never blocks user input for more than a fraction of a second
// at a time.  Returns immediately if the interval hasn't elapsed.
static void wifi_resync_if_due() {
    if (millis() - g_last_sync_ms < WIFI_RESYNC_INTERVAL_MS) return;
    USBSerial.println("[ntp] periodic resync...");
    g_wifi_connected = false;
    wifi_connect_and_sync();          // re-connects, syncs, and shuts down
    g_last_sync_ms = millis();        // even on failure, back-off the next try
}
#endif

// ============================================================================
//  Favourites (NVS-backed)
// ============================================================================
Preferences g_prefs;
#define MAX_FAVS 16
static String g_favs[MAX_FAVS];
static int    g_fav_count = 0;

static void favs_load() {
    g_fav_count = 0;
    g_prefs.begin("dcw", true);
    String csv = g_prefs.getString("favs", "");
    g_prefs.end();
    int from = 0;
    while (from < (int)csv.length() && g_fav_count < MAX_FAVS) {
        int c = csv.indexOf(',', from);
        if (c < 0) c = csv.length();
        String it = csv.substring(from, c); it.trim();
        if (it.length()) g_favs[g_fav_count++] = it;
        from = c + 1;
    }
}
static void favs_save() {
    String csv;
    for (int i = 0; i < g_fav_count; i++) { if (i) csv += ","; csv += g_favs[i]; }
    g_prefs.begin("dcw", false); g_prefs.putString("favs", csv); g_prefs.end();
}
static bool favs_contains(const String &n){for(int i=0;i<g_fav_count;i++)if(g_favs[i]==n)return true;return false;}
static void favs_toggle(const String &n) {
    for (int i = 0; i < g_fav_count; i++) if (g_favs[i] == n) {
        for (int j=i;j<g_fav_count-1;j++) g_favs[j]=g_favs[j+1];
        g_fav_count--; favs_save(); return;
    }
    if (g_fav_count < MAX_FAVS) { g_favs[g_fav_count++] = n; favs_save(); }
}

// ============================================================================
//  ============      MINIMAL  "DualCore Watch"  splash  (v5.7)     ============
//  Boot screen redesigned: dropped the 3 pulsing/rotating arcs + the scanline
//  sweep (felt busy / "huge cycles").  Replaced with a single calm pulsing
//  dot, a thin progress bar, and the title.  Loads faster, reads cleaner.
// ============================================================================
static lv_obj_t *splash_screen = nullptr;
static lv_obj_t *splash_status = nullptr;
static lv_obj_t *splash_bar    = nullptr;

static void anim_opa(void *v, int32_t x){lv_obj_set_style_text_opa((lv_obj_t*)v,x,0);}
static void anim_scale(void *v, int32_t x){
    lv_obj_t *o = (lv_obj_t *)v;
    lv_obj_set_size(o, x, x);
    lv_obj_align(o, LV_ALIGN_CENTER, 0, -60);
}

static void build_splash() {
    splash_screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(splash_screen, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(splash_screen, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(splash_screen, 0, 0);
    lv_obj_set_style_border_width(splash_screen, 0, 0);
    lv_scr_load(splash_screen);

    // --- single calm pulsing dot (replaces 3 spinning arcs) ----------------
    lv_obj_t *dot = lv_obj_create(splash_screen);
    lv_obj_set_size(dot, 22, 22);
    lv_obj_align(dot, LV_ALIGN_CENTER, 0, -60);
    lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(dot, lv_color_hex(0x00E5FF), 0);
    lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(dot, 0, 0);
    lv_obj_set_style_shadow_color(dot, lv_color_hex(0x00E5FF), 0);
    lv_obj_set_style_shadow_width(dot, 24, 0);
    lv_obj_set_style_shadow_opa(dot, 180, 0);

    { lv_anim_t a; lv_anim_init(&a); lv_anim_set_var(&a, dot);
      lv_anim_set_exec_cb(&a, anim_scale);
      lv_anim_set_values(&a, 18, 30);
      lv_anim_set_time(&a, 700);
      lv_anim_set_playback_time(&a, 700);
      lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
      lv_anim_set_path_cb(&a, lv_anim_path_ease_in_out);
      lv_anim_start(&a); }

    // --- title (static, no per-letter slide -- simpler) --------------------
    lv_obj_t *title = lv_label_create(splash_screen);
    lv_label_set_text(title, "DualCore");
    lv_obj_set_style_text_color(title, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_28, 0);
    lv_obj_align(title, LV_ALIGN_CENTER, 0, 10);
    lv_obj_set_style_text_opa(title, 0, 0);
    { lv_anim_t a; lv_anim_init(&a); lv_anim_set_var(&a, title);
      lv_anim_set_exec_cb(&a, anim_opa);
      lv_anim_set_values(&a, 0, 255);
      lv_anim_set_time(&a, 400);
      lv_anim_set_delay(&a, 80);
      lv_anim_start(&a); }

    // --- subtitle ----------------------------------------------------------
    lv_obj_t *sub = lv_label_create(splash_screen);
    lv_label_set_text(sub, "W A T C H");
    lv_obj_set_style_text_color(sub, lv_color_hex(0x00E5FF), 0);
    lv_obj_set_style_text_font(sub, &lv_font_montserrat_18, 0);
    lv_obj_set_style_text_letter_space(sub, 6, 0);
    lv_obj_align(sub, LV_ALIGN_CENTER, 0, 52);
    lv_obj_set_style_text_opa(sub, 0, 0);
    { lv_anim_t a; lv_anim_init(&a); lv_anim_set_var(&a, sub);
      lv_anim_set_exec_cb(&a, anim_opa);
      lv_anim_set_values(&a, 0, 220);
      lv_anim_set_time(&a, 500);
      lv_anim_set_delay(&a, 250);
      lv_anim_start(&a); }

    // --- progress bar (thin, monochrome accent) ----------------------------
    splash_bar = lv_bar_create(splash_screen);
    lv_obj_set_size(splash_bar, 220, 3);
    lv_obj_align(splash_bar, LV_ALIGN_BOTTOM_MID, 0, -70);
    lv_obj_set_style_bg_color(splash_bar, lv_color_hex(0x111111), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(splash_bar, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(splash_bar, lv_color_hex(0x00E5FF), LV_PART_INDICATOR);
    lv_obj_set_style_border_width(splash_bar, 0, LV_PART_MAIN);
    lv_bar_set_range(splash_bar, 0, 100);

    splash_status = lv_label_create(splash_screen);
    lv_label_set_text(splash_status, "booting");
    lv_obj_set_style_text_color(splash_status, lv_color_hex(0x888888), 0);
    lv_obj_set_style_text_font(splash_status, &lv_font_montserrat_14, 0);
    lv_obj_align(splash_status, LV_ALIGN_BOTTOM_MID, 0, -42);

    // --- hardware-ID footer (fades in late, very subtle) -------------------
    lv_obj_t *hwid = lv_label_create(splash_screen);
    lv_label_set_text(hwid, "ESP32-S3  ·  16 MB  ·  8 MB PSRAM");
    lv_obj_set_style_text_color(hwid, lv_color_hex(0x555555), 0);
    lv_obj_set_style_text_font(hwid, &lv_font_montserrat_12, 0);
    lv_obj_align(hwid, LV_ALIGN_BOTTOM_MID, 0, -16);
    lv_obj_set_style_text_opa(hwid, 0, 0);
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
    uint32_t until = millis() + 250;
    while (millis() < until) { lvgl_pump(); delay(5); }
}

// ============================================================================
//  Wallpaper loader  -- /WATCH/PHOTOS/*.raw or *.bin (RGB565 LE, 410x502)
// ============================================================================
#define MAX_PHOTOS 24
static String g_photos[MAX_PHOTOS];
static int    g_photo_count = 0;
static int    g_photo_idx   = 0;
static lv_img_dsc_t g_face_img_dsc = { };
static uint8_t     *g_face_pixels  = nullptr;
static size_t       g_face_alloc   = 0;

static void scan_photos() {
    g_photo_count = 0;
    File dir = SD_MMC.open("/WATCH/PHOTOS");
    if (!dir || !dir.isDirectory()) return;
    File f = dir.openNextFile();
    while (f && g_photo_count < MAX_PHOTOS) {
        if (!f.isDirectory()) {
            String n = f.name();
            int s = n.lastIndexOf('/'); if (s >= 0) n = n.substring(s+1);
            String lo = n; lo.toLowerCase();
            if (lo.endsWith(".raw") || lo.endsWith(".bin"))
                g_photos[g_photo_count++] = String("/WATCH/PHOTOS/") + n;
        }
        f = dir.openNextFile();
    }
}
static bool load_photo(int idx) {
    if (!g_photo_count) return false;
    if (idx < 0) idx = g_photo_count - 1;
    if (idx >= g_photo_count) idx = 0;
    File f = SD_MMC.open(g_photos[idx].c_str(), FILE_READ);
    if (!f) return false;
    size_t need = (size_t)LCD_WIDTH * LCD_HEIGHT * 2;
    if (!g_face_pixels || g_face_alloc < need) {
        if (g_face_pixels) heap_caps_free(g_face_pixels);
        g_face_pixels = (uint8_t *)heap_caps_malloc(need, MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
        if (!g_face_pixels) g_face_pixels = (uint8_t *)heap_caps_malloc(need, MALLOC_CAP_8BIT);
        if (!g_face_pixels) { f.close(); return false; }
        g_face_alloc = need;
    }
    memset(g_face_pixels, 0, need);
    size_t fsize = f.size();
    // Auto-detect & skip the 4-byte LVGL image header.
    //   * Headerless raw  : fsize == need  (410*502*2 = 411640)
    //   * LVGL .bin       : fsize == need + 4
    // The LVGL v8 header layout puts cf in the low 5 bits of byte 0 and
    // 'always_zero' in the next 3 bits, so byte 0 == LV_IMG_CF_TRUE_COLOR
    // (== 4) on a well-formed file.  We don't need to validate it strictly
    // -- the size delta is enough.
    if (fsize == need + 4) {
        f.seek(4);
        fsize -= 4;
    }
    f.read(g_face_pixels, fsize < need ? fsize : need);
    f.close();
    g_face_img_dsc.header.cf = LV_IMG_CF_TRUE_COLOR;
    g_face_img_dsc.header.always_zero = 0;
    g_face_img_dsc.header.w = LCD_WIDTH;
    g_face_img_dsc.header.h = LCD_HEIGHT;
    g_face_img_dsc.data_size = need;
    g_face_img_dsc.data = g_face_pixels;
    g_photo_idx = idx;
    return true;
}

// ============================================================================
//  Three independent screens swapped instantly (no animation)
// ============================================================================
#define SCREEN_FACE     0
#define SCREEN_APPS     1
#define SCREEN_BATTERY  2
#define NUM_SCREENS     3

static lv_obj_t *screens[NUM_SCREENS] = { nullptr };
static int        current_screen = SCREEN_FACE;
static uint32_t   last_nav_ms = 0;

// MiniOS-parity screen switch (v5.6).
//   * No cooldown -- a fresh swipe always fires the moment the finger lifts.
//   * Wrap index BEFORE the same-screen guard so edge transitions 2->0 / 0->2 work.
//   * 120 ms cross-fade reads as "instant + smooth" -- _NONE is jarring,
//     a slide is too slow.
static void goto_screen(int s) {
    // 1) wrap index BEFORE any equality check (fixes edge transitions)
    while (s < 0)            s += NUM_SCREENS;
    while (s >= NUM_SCREENS) s -= NUM_SCREENS;

    // 2) same-screen guard now operates on the normalised index
    if (s == current_screen) return;

    current_screen  = s;
    last_nav_ms     = millis();                     // kept for telemetry / future use
    lastTouchMs     = millis();                     // a screen switch counts as activity
    g_pending_swipe = SWIPE_NONE;                   // drop any stale queued swipe

    // Smooth fade -- short enough to feel instant, long enough to mask the swap.
    lv_scr_load_anim(screens[s], LV_SCR_LOAD_ANIM_FADE_ON,
                     SCREEN_SWITCH_ANIM_MS, 0, false);
}

// ---- watch-face widgets ---------------------------------------------------
static lv_obj_t *face_bg_img=nullptr, *face_bg_fill=nullptr;
static lv_obj_t *face_time=nullptr, *face_time_shadow=nullptr, *face_date=nullptr;
static lv_obj_t *face_batt_icon=nullptr, *face_batt_fill=nullptr;
static lv_obj_t *face_batt_bolt=nullptr, *face_batt_lbl=nullptr;
static lv_obj_t *face_status=nullptr;

static void refresh_wallpaper() {
    if (!g_photo_count) {
        if (face_bg_img)  lv_obj_add_flag(face_bg_img, LV_OBJ_FLAG_HIDDEN);
        if (face_bg_fill) lv_obj_clear_flag(face_bg_fill, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    if (!load_photo(g_photo_idx)) return;
    if (face_bg_fill) lv_obj_add_flag(face_bg_fill, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(face_bg_img, LV_OBJ_FLAG_HIDDEN);
    lv_img_set_src(face_bg_img, &g_face_img_dsc);
    lv_obj_invalidate(face_bg_img);
}

// ---- wallpaper persistence + long-press cycling -----------------------
// wp_idx is stashed in the same "dcw" Preferences namespace already used
// by the favourites list, so it survives reboots AND is visible to any
// other app that wants to honour the last-chosen wallpaper.
static const uint32_t FACE_LONG_PRESS_MS = 1000;
static uint32_t g_face_press_ms   = 0;
static bool     g_face_long_fired = false;

static void save_wp_idx() {
    g_prefs.begin("dcw", false);
    g_prefs.putInt("wp_idx", g_photo_idx);
    g_prefs.end();
}

static void cycle_wallpaper_and_save() {
    if (!g_photo_count) return;
    g_photo_idx = (g_photo_idx + 1) % g_photo_count;
    refresh_wallpaper();
    save_wp_idx();
}

// Single handler subscribed to LV_EVENT_ALL.  Short click and ~1 s
// tap-and-hold both cycle the wallpaper forward and persist wp_idx.
// The long-press path latches g_face_long_fired so the trailing
// CLICKED on release doesn't double-cycle.
static void face_tap_cb(lv_event_t *e) {
    lv_event_code_t code = lv_event_get_code(e);

    if (code == LV_EVENT_PRESSED) {
        g_face_press_ms   = millis();
        g_face_long_fired = false;
    } else if (code == LV_EVENT_PRESSING) {
        if (!g_face_long_fired && g_face_press_ms &&
            millis() - g_face_press_ms >= FACE_LONG_PRESS_MS) {
            g_face_long_fired = true;
            cycle_wallpaper_and_save();
        }
    } else if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
        g_face_press_ms = 0;
    } else if (code == LV_EVENT_CLICKED) {
        if (millis() - lastSwipeMs < CLICK_AFTER_SWIPE_MS) return;   // post-swipe guard
        if (g_face_long_fired) { g_face_long_fired = false; return; } // long-press already cycled
        cycle_wallpaper_and_save();
    }
}

static void update_face_time() {
    if (!face_time) return;
    char hm[8] = "--:--", dt[32] = "no wifi";
    struct tm tm;
    if (g_ntp_synced && getLocalTime(&tm, 5)) {
        strftime(hm, sizeof(hm), "%H:%M", &tm);
        strftime(dt, sizeof(dt), "%a  %d %b", &tm);
    } else if (g_wifi_connected) {
        strcpy(dt, "syncing...");
    }
    lv_label_set_text(face_time, hm);
    lv_label_set_text(face_time_shadow, hm);
    lv_label_set_text(face_date, dt);
    lv_color_t dot = g_ntp_synced     ? lv_color_hex(0x00FF80)
                   : g_wifi_connected ? lv_color_hex(0xFFB300)
                                      : lv_color_hex(0xFF4040);
    lv_obj_set_style_bg_color(face_status, dot, 0);
}

static void update_face_battery() {
    if (!face_batt_icon) return;
    if (!g_has_pmu || g_batt_pct < 0) {
        lv_obj_add_flag(face_batt_icon, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(face_batt_fill, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(face_batt_bolt, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(face_batt_lbl, "");
        return;
    }
    lv_obj_clear_flag(face_batt_icon, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(face_batt_fill, LV_OBJ_FLAG_HIDDEN);
    int pct = g_batt_pct; if (pct<0)pct=0; if (pct>100)pct=100;
    int fw = (28*pct)/100; if (fw<1 && pct>0) fw=1;
    lv_obj_set_width(face_batt_fill, fw);
    lv_color_t col = pct<=15 ? lv_color_hex(0xFF4040)
                   : pct<=30 ? lv_color_hex(0xFFB300)
                             : lv_color_hex(0xFFFFFF);
    lv_obj_set_style_bg_color(face_batt_fill, col, 0);
    if (g_charging) lv_obj_clear_flag(face_batt_bolt, LV_OBJ_FLAG_HIDDEN);
    else            lv_obj_add_flag(face_batt_bolt, LV_OBJ_FLAG_HIDDEN);
    char b[8]; snprintf(b, 8, "%d%%", pct); lv_label_set_text(face_batt_lbl, b);
}

static void build_screen_face() {
    lv_obj_t *s = lv_obj_create(NULL);
    screens[SCREEN_FACE] = s;
    lv_obj_set_style_bg_color(s, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(s, LV_OPA_COVER, 0);
    lv_obj_clear_flag(s, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_pad_all(s, 0, 0);

    // Fallback gradient
    face_bg_fill = lv_obj_create(s);
    lv_obj_set_size(face_bg_fill, LCD_WIDTH, LCD_HEIGHT);
    lv_obj_align(face_bg_fill, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_set_style_radius(face_bg_fill, 0, 0);
    lv_obj_set_style_border_width(face_bg_fill, 0, 0);
    lv_obj_set_style_bg_color(face_bg_fill,      lv_color_hex(0x05060A), 0);
    lv_obj_set_style_bg_grad_color(face_bg_fill, lv_color_hex(0x0A1A2A), 0);
    lv_obj_set_style_bg_grad_dir(face_bg_fill,   LV_GRAD_DIR_VER, 0);
    lv_obj_clear_flag(face_bg_fill, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(face_bg_fill, LV_OBJ_FLAG_CLICKABLE);

    // Photo wallpaper
    face_bg_img = lv_img_create(s);
    lv_obj_align(face_bg_img, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_add_flag(face_bg_img, LV_OBJ_FLAG_HIDDEN);

    // Tap zone (lower 3/4 -- cycles wallpaper)
    lv_obj_t *tap = lv_obj_create(s);
    lv_obj_set_size(tap, LCD_WIDTH, LCD_HEIGHT - 70);
    lv_obj_align(tap, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_opa(tap, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(tap, 0, 0);
    lv_obj_add_flag(tap, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(tap, LV_OBJ_FLAG_SCROLLABLE);
    // LV_EVENT_ALL so we see PRESSED / PRESSING / RELEASED / CLICKED --
    // tap cycles wallpaper, ~1 s tap-and-hold also cycles + persists.
    lv_obj_add_event_cb(tap, face_tap_cb, LV_EVENT_ALL, NULL);

    // Translucent time
    face_time = lv_label_create(s);
    lv_label_set_text(face_time, "18:15");
    lv_obj_set_style_text_color(face_time, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(face_time, &FACE_TIME_FONT, 0);
    lv_obj_set_style_text_opa(face_time, 220, 0);
    lv_obj_set_style_text_letter_space(face_time, 2, 0);
    lv_obj_align(face_time, LV_ALIGN_TOP_MID, 0, 60);

    // Shadow behind time (for legibility on bright photos)
    face_time_shadow = lv_label_create(s);
    lv_label_set_text(face_time_shadow, "18:15");
    lv_obj_set_style_text_color(face_time_shadow, lv_color_hex(0x000000), 0);
    lv_obj_set_style_text_font(face_time_shadow, &FACE_TIME_FONT, 0);
    lv_obj_set_style_text_opa(face_time_shadow, 90, 0);
    lv_obj_set_style_text_letter_space(face_time_shadow, 2, 0);
    lv_obj_align(face_time_shadow, LV_ALIGN_TOP_MID, 2, 62);
    lv_obj_move_background(face_time_shadow);

    face_date = lv_label_create(s);
    lv_label_set_text(face_date, "");
    lv_obj_set_style_text_color(face_date, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(face_date, &lv_font_montserrat_18, 0);
    lv_obj_set_style_text_opa(face_date, 160, 0);
    lv_obj_align(face_date, LV_ALIGN_TOP_MID, 0, 145);

    // Battery indicator (top-left)
    face_batt_icon = lv_obj_create(s);
    lv_obj_set_size(face_batt_icon, 32, 14);
    lv_obj_align(face_batt_icon, LV_ALIGN_TOP_LEFT, 18, 18);
    lv_obj_set_style_radius(face_batt_icon, 3, 0);
    lv_obj_set_style_border_color(face_batt_icon, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_border_width(face_batt_icon, 1, 0);
    lv_obj_set_style_border_opa(face_batt_icon, 180, 0);
    lv_obj_set_style_bg_opa(face_batt_icon, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(face_batt_icon, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(face_batt_icon, LV_OBJ_FLAG_CLICKABLE);
    face_batt_fill = lv_obj_create(face_batt_icon);
    lv_obj_set_size(face_batt_fill, 0, 10);
    lv_obj_align(face_batt_fill, LV_ALIGN_LEFT_MID, 1, 0);
    lv_obj_set_style_radius(face_batt_fill, 2, 0);
    lv_obj_set_style_border_width(face_batt_fill, 0, 0);
    lv_obj_set_style_bg_color(face_batt_fill, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_bg_opa(face_batt_fill, 200, 0);
    face_batt_bolt = lv_label_create(s);
    lv_label_set_text(face_batt_bolt, LV_SYMBOL_CHARGE);
    lv_obj_set_style_text_color(face_batt_bolt, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(face_batt_bolt, &lv_font_montserrat_14, 0);
    lv_obj_align_to(face_batt_bolt, face_batt_icon, LV_ALIGN_CENTER, 0, 0);
    lv_obj_add_flag(face_batt_bolt, LV_OBJ_FLAG_HIDDEN);
    face_batt_lbl = lv_label_create(s);
    lv_label_set_text(face_batt_lbl, "");
    lv_obj_set_style_text_color(face_batt_lbl, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(face_batt_lbl, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_opa(face_batt_lbl, 180, 0);
    lv_obj_align_to(face_batt_lbl, face_batt_icon, LV_ALIGN_OUT_RIGHT_MID, 8, 0);

    face_status = lv_obj_create(s);
    lv_obj_set_size(face_status, 10, 10);
    lv_obj_align(face_status, LV_ALIGN_TOP_RIGHT, -20, 22);
    lv_obj_set_style_radius(face_status, 5, 0);
    lv_obj_set_style_border_width(face_status, 0, 0);
    lv_obj_set_style_bg_color(face_status, lv_color_hex(0xFF4040), 0);
    lv_obj_set_style_bg_opa(face_status, 200, 0);

    refresh_wallpaper();
    update_face_time();
    update_face_battery();
}

// ============================================================================
//  App selector screen   --   v6.0 loader UX overhaul
//  ----------------------------------------------------------------------------
//  v6.0 changes (loader-only, hardware/pins/config untouched):
//    * Card-style list: each row is a rounded "tile" with a big app name,
//      a small dim size line, a right-aligned chevron, and a left-side
//      colour bar (favourite = gold, normal = cyan).  Section headers
//      ("Favourites" / "All apps") are pill-styled.
//    * Empty state is a centred icon + two-line hint, not raw text.
//    * Launch flow now goes through a confirmation modal -- a stray
//      tile-tap can no longer trigger an OTA copy + reboot by accident.
//    * Copy screen rewritten: smooth bar, animated %, "X.X / Y.Y KB",
//      live KB/s speed, live ETA in MM:SS, OK status colour shift.
//    * Errors no longer leave the user stranded on the copy screen --
//      they show a red banner with the failure reason and a "Back"
//      button to return to the app list.
//    * NVS cache ("appcache") for the scanned app list.  A directory
//      "signature" (file count XOR'd with summed sizes) is stored
//      alongside it; on next boot, if the signature still matches,
//      we skip the per-file stat loop and rehydrate g_apps[] from
//      Preferences in a handful of milliseconds.
//    * "Back-to-launcher" recovery: on every boot, we mark the running
//      partition (= the launcher) as the bootable one.  So any loaded
//      app that calls esp_restart() / crashes / is power-cycled comes
//      back to LauncherOS automatically, instead of into the last app
//      that was flashed into the spare OTA slot.
// ============================================================================
struct AppEntry { String name, path; size_t size; };
#define MAX_APPS 32
static AppEntry  g_apps[MAX_APPS];
static int       g_app_count = 0;
static lv_obj_t *g_list_widget = nullptr;
static bool      load_app_from_sd(const AppEntry &app);
static void      populate_app_list();

// ---- back-to-launcher recovery --------------------------------------------
// Anchors the bootloader on the partition we're currently running from
// (= LauncherOS).  Loaded apps that reboot for any reason therefore land
// back here instead of into whichever OTA slot was last flashed.
static void anchor_launcher_as_boot() {
    const esp_partition_t *self = esp_ota_get_running_partition();
    if (!self) { USBSerial.println("[boot] no running partition?"); return; }
    esp_err_t e = esp_ota_set_boot_partition(self);
    USBSerial.printf("[boot] anchored launcher to %s (err=%d)\n",
                     self->label, (int)e);
}

// ---- NVS app-list cache ----------------------------------------------------
// Saves the result of scan_apps() to Preferences so the next boot can
// rehydrate the list without re-walking the SD directory.  A simple
// signature (count ^ sum-of-sizes) lets us detect whether the SD has
// changed since the cache was written.
static Preferences g_app_cache;
static uint32_t apps_signature(File &dir) {
    uint32_t sig = 0; int n = 0;
    File f = dir.openNextFile();
    while (f && n < MAX_APPS) {
        if (!f.isDirectory()) {
            String nm = f.name(); nm.toLowerCase();
            if (nm.endsWith(".bin")) { sig ^= (uint32_t)f.size() + (uint32_t)n * 2654435761u; n++; }
        }
        f = dir.openNextFile();
    }
    return sig ^ (uint32_t)n;
}
static bool apps_cache_load(uint32_t expected_sig) {
    g_app_cache.begin("appcache", true);
    uint32_t sig = g_app_cache.getUInt("sig", 0);
    int cnt = g_app_cache.getInt("cnt", 0);
    if (sig != expected_sig || cnt <= 0 || cnt > MAX_APPS) { g_app_cache.end(); return false; }
    g_app_count = 0;
    char key[12];
    for (int i = 0; i < cnt; i++) {
        snprintf(key, sizeof(key), "n%d", i);
        String nm = g_app_cache.getString(key, "");
        snprintf(key, sizeof(key), "s%d", i);
        uint32_t sz = g_app_cache.getUInt(key, 0);
        if (!nm.length()) continue;
        g_apps[g_app_count].name = nm;
        g_apps[g_app_count].path = String("/WATCH/bin/") + nm + ".bin";
        g_apps[g_app_count].size = sz;
        g_app_count++;
    }
    g_app_cache.end();
    USBSerial.printf("[cache] rehydrated %d apps from NVS\n", g_app_count);
    return g_app_count > 0;
}
static void apps_cache_save(uint32_t sig) {
    g_app_cache.begin("appcache", false);
    g_app_cache.clear();
    g_app_cache.putUInt("sig", sig);
    g_app_cache.putInt("cnt", g_app_count);
    char key[12];
    for (int i = 0; i < g_app_count; i++) {
        snprintf(key, sizeof(key), "n%d", i);
        g_app_cache.putString(key, g_apps[i].name);
        snprintf(key, sizeof(key), "s%d", i);
        g_app_cache.putUInt(key, (uint32_t)g_apps[i].size);
    }
    g_app_cache.end();
    USBSerial.printf("[cache] saved %d apps (sig=%08x)\n", g_app_count, sig);
}

// ---- OTA copy screen -------------------------------------------------------
static lv_obj_t *copy_screen = nullptr;
static lv_obj_t *copy_title  = nullptr;
static lv_obj_t *copy_sub    = nullptr;   // size + speed line
static lv_obj_t *copy_bar    = nullptr;
static lv_obj_t *copy_pct    = nullptr;
static lv_obj_t *copy_eta    = nullptr;
static lv_obj_t *copy_err_btn= nullptr;
static bool      copy_aborted = false;

static void copy_back_cb(lv_event_t *) {
    // User dismissed an error -- return to the app list.
    lv_scr_load_anim(screens[SCREEN_APPS], LV_SCR_LOAD_ANIM_FADE_ON, 220, 0, false);
    current_screen = SCREEN_APPS;
}

static void build_copy_screen(const String &n, size_t total_bytes) {
    copy_aborted = false;
    copy_screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(copy_screen, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(copy_screen, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(copy_screen, 0, 0);
    lv_obj_set_style_border_width(copy_screen, 0, 0);
    lv_obj_clear_flag(copy_screen, LV_OBJ_FLAG_SCROLLABLE);

    // "LOADING" eyebrow
    lv_obj_t *eyebrow = lv_label_create(copy_screen);
    lv_label_set_text(eyebrow, "LOADING APP");
    lv_obj_set_style_text_color(eyebrow, lv_color_hex(0x00E5FF), 0);
    lv_obj_set_style_text_font(eyebrow, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_letter_space(eyebrow, 3, 0);
    lv_obj_align(eyebrow, LV_ALIGN_CENTER, 0, -120);

    // Big app name
    copy_title = lv_label_create(copy_screen);
    lv_label_set_text(copy_title, n.c_str());
    lv_obj_set_style_text_align(copy_title, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(copy_title, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(copy_title, &lv_font_montserrat_28, 0);
    lv_obj_set_width(copy_title, LCD_WIDTH - 60);
    lv_label_set_long_mode(copy_title, LV_LABEL_LONG_DOT);
    lv_obj_align(copy_title, LV_ALIGN_CENTER, 0, -80);

    // Big %
    copy_pct = lv_label_create(copy_screen);
    lv_label_set_text(copy_pct, "0%");
    lv_obj_set_style_text_color(copy_pct, lv_color_hex(0x00E5FF), 0);
    lv_obj_set_style_text_font(copy_pct, &lv_font_montserrat_48, 0);
    lv_obj_align(copy_pct, LV_ALIGN_CENTER, 0, -20);

    // Progress bar
    copy_bar = lv_bar_create(copy_screen);
    lv_obj_set_size(copy_bar, LCD_WIDTH - 80, 8);
    lv_obj_align(copy_bar, LV_ALIGN_CENTER, 0, 40);
    lv_obj_set_style_bg_color(copy_bar, lv_color_hex(0x141414), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(copy_bar, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(copy_bar, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(copy_bar, 4, LV_PART_MAIN);
    lv_obj_set_style_bg_color(copy_bar, lv_color_hex(0x00E5FF), LV_PART_INDICATOR);
    lv_obj_set_style_bg_grad_color(copy_bar, lv_color_hex(0x4DFFFF), LV_PART_INDICATOR);
    lv_obj_set_style_bg_grad_dir(copy_bar, LV_GRAD_DIR_HOR, LV_PART_INDICATOR);
    lv_obj_set_style_radius(copy_bar, 4, LV_PART_INDICATOR);
    lv_bar_set_range(copy_bar, 0, 1000);    // 0.1% resolution -> animation looks smooth
    lv_bar_set_value(copy_bar, 0, LV_ANIM_OFF);

    // Size / speed sub-line
    copy_sub = lv_label_create(copy_screen);
    char b[64];
    snprintf(b, sizeof(b), "0.0 / %.1f KB", total_bytes / 1024.0f);
    lv_label_set_text(copy_sub, b);
    lv_obj_set_style_text_color(copy_sub, lv_color_hex(0xBBBBBB), 0);
    lv_obj_set_style_text_font(copy_sub, &lv_font_montserrat_16, 0);
    lv_obj_align(copy_sub, LV_ALIGN_CENTER, 0, 70);

    // ETA line
    copy_eta = lv_label_create(copy_screen);
    lv_label_set_text(copy_eta, "preparing...");
    lv_obj_set_style_text_color(copy_eta, lv_color_hex(0x666666), 0);
    lv_obj_set_style_text_font(copy_eta, &lv_font_montserrat_14, 0);
    lv_obj_align(copy_eta, LV_ALIGN_CENTER, 0, 100);

    // (hidden until error) Back button
    copy_err_btn = lv_btn_create(copy_screen);
    lv_obj_set_size(copy_err_btn, 160, 44);
    lv_obj_align(copy_err_btn, LV_ALIGN_BOTTOM_MID, 0, -30);
    lv_obj_set_style_bg_color(copy_err_btn, lv_color_hex(0x222222), 0);
    lv_obj_set_style_border_color(copy_err_btn, lv_color_hex(0xFF4040), 0);
    lv_obj_set_style_border_width(copy_err_btn, 1, 0);
    lv_obj_set_style_radius(copy_err_btn, 22, 0);
    lv_obj_t *bl = lv_label_create(copy_err_btn);
    lv_label_set_text(bl, LV_SYMBOL_LEFT "  Back");
    lv_obj_set_style_text_color(bl, lv_color_hex(0xFF8080), 0);
    lv_obj_set_style_text_font(bl, &lv_font_montserrat_16, 0);
    lv_obj_center(bl);
    lv_obj_add_event_cb(copy_err_btn, copy_back_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_add_flag(copy_err_btn, LV_OBJ_FLAG_HIDDEN);

    // Smooth fade INTO the loading screen (180 ms reads as deliberate).
    lv_scr_load_anim(copy_screen, LV_SCR_LOAD_ANIM_FADE_ON, 180, 0, false);
    uint32_t fade_start = millis();
    while (millis() - fade_start < 200) { lvgl_pump(); delay(5); }
}

static void copy_progress(size_t done, size_t total, uint32_t elapsed_ms) {
    int permil = total ? (int)((uint64_t)done * 1000 / total) : 0;
    if (permil > 1000) permil = 1000;
    // Animate the bar smoothly to the new value.
    lv_bar_set_value(copy_bar, permil, LV_ANIM_ON);
    char b[64];
    snprintf(b, sizeof(b), "%d%%", permil / 10);
    lv_label_set_text(copy_pct, b);
    snprintf(b, sizeof(b), "%.1f / %.1f KB", done / 1024.0f, total / 1024.0f);
    lv_label_set_text(copy_sub, b);
    if (elapsed_ms > 250 && done > 0) {
        float kbps = (done / 1024.0f) * 1000.0f / (float)elapsed_ms;
        size_t remaining = (total > done) ? (total - done) : 0;
        float eta_s = (kbps > 0.1f) ? ((remaining / 1024.0f) / kbps) : 0.0f;
        int m = (int)eta_s / 60;
        int s = (int)eta_s % 60;
        snprintf(b, sizeof(b), "%.0f KB/s   ETA %d:%02d", kbps, m, s);
        lv_label_set_text(copy_eta, b);
    }
    lvgl_pump();
}

static void copy_error(const String &m) {
    copy_aborted = true;
    lv_label_set_text(copy_title, "Couldn't load app");
    lv_obj_set_style_text_color(copy_title, lv_color_hex(0xFF4040), 0);
    lv_label_set_text(copy_sub, m.c_str());
    lv_obj_set_style_text_color(copy_sub, lv_color_hex(0xFF8080), 0);
    lv_label_set_text(copy_pct, "ERR");
    lv_obj_set_style_text_color(copy_pct, lv_color_hex(0xFF4040), 0);
    lv_label_set_text(copy_eta, "tap Back to return to the app list");
    lv_obj_set_style_bg_color(copy_bar, lv_color_hex(0xFF4040), LV_PART_INDICATOR);
    lv_obj_set_style_bg_grad_color(copy_bar, lv_color_hex(0xFF8080), LV_PART_INDICATOR);
    lv_obj_clear_flag(copy_err_btn, LV_OBJ_FLAG_HIDDEN);
    lvgl_pump();
}

static bool load_app_from_sd(const AppEntry &app) {
    build_copy_screen(app.name, app.size);
#if POWER_SAVE
    setCpuFrequencyMhz(CPU_FREQ_BUSY_MHZ);   // OTA flash writes need speed
#endif
    const esp_partition_t *ota = esp_ota_get_next_update_partition(NULL);
    if (!ota) { copy_error("No free OTA partition"); return false; }
    File f = SD_MMC.open(app.path.c_str(), FILE_READ);
    if (!f) { copy_error("Cannot open .bin on SD"); return false; }
    esp_ota_handle_t h;
    if (esp_ota_begin(ota, app.size, &h) != ESP_OK) {
        copy_error("esp_ota_begin failed"); f.close(); return false;
    }
    uint8_t *buf = (uint8_t *)heap_caps_malloc(4096, MALLOC_CAP_8BIT);
    if (!buf) { esp_ota_end(h); f.close(); copy_error("Out of memory"); return false; }

    size_t written = 0;
    uint32_t t0 = millis();
    uint32_t lastUiMs = 0;
    while (true) {
        int n = f.read(buf, 4096);
        if (n <= 0) break;
        if (esp_ota_write(h, buf, n) != ESP_OK) {
            free(buf); f.close();
            copy_error("Flash write failed"); return false;
        }
        written += n;
        // Throttle UI updates to ~33 fps -- LVGL animations stay smooth
        // and we don't bottleneck the OTA write on label re-layout.
        uint32_t now = millis();
        if (now - lastUiMs >= 30 || written == app.size) {
            copy_progress(written, app.size, now - t0);
            lastUiMs = now;
        }
    }
    free(buf); f.close();
    if (esp_ota_end(h) != ESP_OK) { copy_error("esp_ota_end failed"); return false; }
    if (esp_ota_set_boot_partition(ota) != ESP_OK) {
        copy_error("Set boot partition failed"); return false;
    }
    copy_progress(app.size, app.size, millis() - t0);
    lv_label_set_text(copy_title, "Launching...");
    lv_obj_set_style_text_color(copy_title, lv_color_hex(0x00FF80), 0);
    lv_label_set_text(copy_eta, "");
    lv_obj_set_style_bg_color(copy_bar, lv_color_hex(0x00FF80), LV_PART_INDICATOR);
    lv_obj_set_style_bg_grad_color(copy_bar, lv_color_hex(0x80FFC0), LV_PART_INDICATOR);
    lvgl_pump();
    delay(350);
    // Fade the panel down before the reboot so chip-restart looks deliberate.
    fade_brightness(0, 280);
    esp_restart();
    return true;
}

// ---- Confirmation modal ----------------------------------------------------
// A stray double-tap on a tile used to immediately start an OTA copy +
// reboot.  Now we surface a "Launch <name>?" dialog with explicit
// Cancel / Launch buttons so the destructive action requires intent.
static lv_obj_t *confirm_modal = nullptr;
static int g_confirm_idx = -1;

static void confirm_cancel_cb(lv_event_t *) {
    if (confirm_modal) { lv_obj_del(confirm_modal); confirm_modal = nullptr; }
    g_confirm_idx = -1;
}
static void confirm_launch_cb(lv_event_t *) {
    int idx = g_confirm_idx;
    if (confirm_modal) { lv_obj_del(confirm_modal); confirm_modal = nullptr; }
    g_confirm_idx = -1;
    if (idx >= 0 && idx < g_app_count) load_app_from_sd(g_apps[idx]);
}

static void show_launch_confirm(int idx) {
    if (idx < 0 || idx >= g_app_count) return;
    if (confirm_modal) return;   // already open
    g_confirm_idx = idx;

    lv_obj_t *scr = lv_scr_act();
    // dim backdrop
    confirm_modal = lv_obj_create(scr);
    lv_obj_remove_style_all(confirm_modal);
    lv_obj_set_size(confirm_modal, LCD_WIDTH, LCD_HEIGHT);
    lv_obj_align(confirm_modal, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_set_style_bg_color(confirm_modal, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(confirm_modal, 200, 0);
    lv_obj_clear_flag(confirm_modal, LV_OBJ_FLAG_SCROLLABLE);

    // card
    lv_obj_t *card = lv_obj_create(confirm_modal);
    lv_obj_set_size(card, LCD_WIDTH - 60, 280);
    lv_obj_align(card, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(card, lv_color_hex(0x141414), 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(card, lv_color_hex(0x00E5FF), 0);
    lv_obj_set_style_border_width(card, 1, 0);
    lv_obj_set_style_radius(card, 22, 0);
    lv_obj_set_style_pad_all(card, 20, 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *eyebrow = lv_label_create(card);
    lv_label_set_text(eyebrow, "LAUNCH APP");
    lv_obj_set_style_text_color(eyebrow, lv_color_hex(0x00E5FF), 0);
    lv_obj_set_style_text_font(eyebrow, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_letter_space(eyebrow, 3, 0);
    lv_obj_align(eyebrow, LV_ALIGN_TOP_MID, 0, 4);

    lv_obj_t *name = lv_label_create(card);
    lv_label_set_text(name, g_apps[idx].name.c_str());
    lv_obj_set_style_text_color(name, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(name, &lv_font_montserrat_22, 0);
    lv_obj_set_style_text_align(name, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(name, LCD_WIDTH - 120);
    lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);
    lv_obj_align(name, LV_ALIGN_TOP_MID, 0, 36);

    lv_obj_t *info = lv_label_create(card);
    char b[64];
    snprintf(b, sizeof(b), "%.1f KB  ·  flashes to OTA slot", g_apps[idx].size / 1024.0f);
    lv_label_set_text(info, b);
    lv_obj_set_style_text_color(info, lv_color_hex(0x888888), 0);
    lv_obj_set_style_text_font(info, &lv_font_montserrat_14, 0);
    lv_obj_align(info, LV_ALIGN_TOP_MID, 0, 76);

    lv_obj_t *hint = lv_label_create(card);
    lv_label_set_text(hint, "Reboot to return here.");
    lv_obj_set_style_text_color(hint, lv_color_hex(0x555555), 0);
    lv_obj_set_style_text_font(hint, &lv_font_montserrat_12, 0);
    lv_obj_align(hint, LV_ALIGN_TOP_MID, 0, 104);

    // Cancel
    lv_obj_t *cancel = lv_btn_create(card);
    lv_obj_set_size(cancel, 130, 50);
    lv_obj_align(cancel, LV_ALIGN_BOTTOM_LEFT, 0, 0);
    lv_obj_set_style_bg_color(cancel, lv_color_hex(0x222222), 0);
    lv_obj_set_style_border_width(cancel, 0, 0);
    lv_obj_set_style_radius(cancel, 25, 0);
    lv_obj_t *cl = lv_label_create(cancel);
    lv_label_set_text(cl, "Cancel");
    lv_obj_set_style_text_color(cl, lv_color_hex(0xCCCCCC), 0);
    lv_obj_set_style_text_font(cl, &lv_font_montserrat_16, 0);
    lv_obj_center(cl);
    lv_obj_add_event_cb(cancel, confirm_cancel_cb, LV_EVENT_CLICKED, NULL);

    // Launch
    lv_obj_t *launch = lv_btn_create(card);
    lv_obj_set_size(launch, 130, 50);
    lv_obj_align(launch, LV_ALIGN_BOTTOM_RIGHT, 0, 0);
    lv_obj_set_style_bg_color(launch, lv_color_hex(0x00E5FF), 0);
    lv_obj_set_style_bg_grad_color(launch, lv_color_hex(0x00FF80), 0);
    lv_obj_set_style_bg_grad_dir(launch, LV_GRAD_DIR_HOR, 0);
    lv_obj_set_style_border_width(launch, 0, 0);
    lv_obj_set_style_radius(launch, 25, 0);
    lv_obj_t *ll = lv_label_create(launch);
    lv_label_set_text(ll, "Launch  " LV_SYMBOL_PLAY);
    lv_obj_set_style_text_color(ll, lv_color_hex(0x000000), 0);
    lv_obj_set_style_text_font(ll, &lv_font_montserrat_16, 0);
    lv_obj_center(ll);
    lv_obj_add_event_cb(launch, confirm_launch_cb, LV_EVENT_CLICKED, NULL);
}

// ---- Directory scan --------------------------------------------------------
static void scan_apps() {
    g_app_count = 0;
    File dir = SD_MMC.open("/WATCH/bin");
    if (!dir || !dir.isDirectory()) {
        USBSerial.println("[scan] /WATCH/bin missing");
        return;
    }
    uint32_t sig = apps_signature(dir);
    dir.close();
    if (apps_cache_load(sig)) return;   // cache hit -> skip per-file stat loop

    // Cache miss: do a fresh directory walk and stat each file.
    dir = SD_MMC.open("/WATCH/bin");
    File f = dir.openNextFile();
    while (f && g_app_count < MAX_APPS) {
        if (!f.isDirectory()) {
            String n = f.name();
            int s = n.lastIndexOf('/'); if (s >= 0) n = n.substring(s+1);
            String lo = n; lo.toLowerCase();
            if (lo.endsWith(".bin")) {
                g_apps[g_app_count].path = String("/WATCH/bin/") + n;
                g_apps[g_app_count].name = n.substring(0, n.length() - 4);
                g_apps[g_app_count].size = f.size();
                g_app_count++;
            }
        }
        f = dir.openNextFile();
    }
    USBSerial.printf("[scan] found %d apps (fresh)\n", g_app_count);
    apps_cache_save(sig);
}

// ---- List interactions -----------------------------------------------------
static void list_event_cb(lv_event_t *e) {
    lv_event_code_t code = lv_event_get_code(e);
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    if (idx < 0 || idx >= g_app_count) return;
    // Post-swipe click-guard (preserved from v5.14).
    if (code == LV_EVENT_CLICKED && millis() - lastSwipeMs < CLICK_AFTER_SWIPE_MS) {
        USBSerial.println("[TOUCH] click suppressed (post-swipe guard)");
        return;
    }
    if (code == LV_EVENT_CLICKED) show_launch_confirm(idx);
    else if (code == LV_EVENT_LONG_PRESSED) { favs_toggle(g_apps[idx].name); populate_app_list(); }
}

static void compute_order(int *out) {
    int n = 0;
    for (int i = 0; i < g_fav_count; i++)
        for (int j = 0; j < g_app_count; j++)
            if (g_apps[j].name == g_favs[i]) { out[n++] = j; break; }
    for (int j = 0; j < g_app_count; j++)
        if (!favs_contains(g_apps[j].name)) out[n++] = j;
}

// One styled section header (pill).
static void add_section_header(lv_obj_t *parent, const char *txt, uint32_t accent) {
    lv_obj_t *wrap = lv_obj_create(parent);
    lv_obj_remove_style_all(wrap);
    lv_obj_set_size(wrap, LV_PCT(100), 28);
    lv_obj_set_style_pad_left(wrap, 14, 0);
    lv_obj_set_style_pad_top(wrap, 6, 0);
    lv_obj_clear_flag(wrap, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *dot = lv_obj_create(wrap);
    lv_obj_remove_style_all(dot);
    lv_obj_set_size(dot, 6, 6);
    lv_obj_align(dot, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_set_style_radius(dot, 3, 0);
    lv_obj_set_style_bg_color(dot, lv_color_hex(accent), 0);
    lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);

    lv_obj_t *l = lv_label_create(wrap);
    lv_label_set_text(l, txt);
    lv_obj_set_style_text_color(l, lv_color_hex(accent), 0);
    lv_obj_set_style_text_font(l, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_letter_space(l, 2, 0);
    lv_obj_align(l, LV_ALIGN_LEFT_MID, 14, 0);
}

// One app row -- styled card with accent bar, name, size, chevron.
static void add_app_card(lv_obj_t *parent, int idx, bool isFav) {
    lv_obj_t *card = lv_obj_create(parent);
    lv_obj_remove_style_all(card);
    lv_obj_set_size(card, LV_PCT(100), 64);
    lv_obj_set_style_bg_color(card, lv_color_hex(isFav ? 0x1A1408 : 0x0E0E0E), 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(card, 14, 0);
    lv_obj_set_style_pad_all(card, 12, 0);
    // (Row spacing is handled by the parent flex container's pad_row.)
    lv_obj_set_style_border_color(card, lv_color_hex(isFav ? 0x3A2810 : 0x1C1C1C), 0);
    lv_obj_set_style_border_width(card, 1, 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    // Pressed-state feedback
    lv_obj_set_style_bg_color(card, lv_color_hex(isFav ? 0x2A1E10 : 0x1A1A1A), LV_STATE_PRESSED);
    lv_obj_set_style_transform_zoom(card, 250, LV_STATE_PRESSED);   // ~0.97x

    // Left accent bar
    lv_obj_t *bar = lv_obj_create(card);
    lv_obj_remove_style_all(bar);
    lv_obj_set_size(bar, 4, 40);
    lv_obj_align(bar, LV_ALIGN_LEFT_MID, -4, 0);
    lv_obj_set_style_radius(bar, 2, 0);
    lv_obj_set_style_bg_color(bar, lv_color_hex(isFav ? 0xFFB300 : 0x00E5FF), 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);

    // Name
    lv_obj_t *name = lv_label_create(card);
    lv_label_set_text(name, g_apps[idx].name.c_str());
    lv_obj_set_style_text_color(name, lv_color_hex(isFav ? 0xFFD060 : 0xFFFFFF), 0);
    lv_obj_set_style_text_font(name, &lv_font_montserrat_18, 0);
    lv_obj_set_width(name, LCD_WIDTH - 140);
    lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);
    lv_obj_align(name, LV_ALIGN_LEFT_MID, 12, -10);

    // Size
    lv_obj_t *sz = lv_label_create(card);
    char b[40];
    if (g_apps[idx].size >= 1024 * 1024)
        snprintf(b, sizeof(b), "%.2f MB", g_apps[idx].size / 1048576.0f);
    else
        snprintf(b, sizeof(b), "%.1f KB", g_apps[idx].size / 1024.0f);
    if (isFav) { strncat(b, "  ·  starred", sizeof(b) - strlen(b) - 1); }
    lv_label_set_text(sz, b);
    lv_obj_set_style_text_color(sz, lv_color_hex(isFav ? 0xBB8830 : 0x707070), 0);
    lv_obj_set_style_text_font(sz, &lv_font_montserrat_12, 0);
    lv_obj_align(sz, LV_ALIGN_LEFT_MID, 12, 12);

    // Chevron
    lv_obj_t *chev = lv_label_create(card);
    lv_label_set_text(chev, LV_SYMBOL_RIGHT);
    lv_obj_set_style_text_color(chev, lv_color_hex(isFav ? 0xFFB300 : 0x00E5FF), 0);
    lv_obj_set_style_text_font(chev, &lv_font_montserrat_18, 0);
    lv_obj_align(chev, LV_ALIGN_RIGHT_MID, 0, 0);

    lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(card, list_event_cb, LV_EVENT_CLICKED,      (void*)(intptr_t)idx);
    lv_obj_add_event_cb(card, list_event_cb, LV_EVENT_LONG_PRESSED, (void*)(intptr_t)idx);
}

static void populate_app_list() {
    if (!g_list_widget) return;
    lv_obj_clean(g_list_widget);

    if (!g_app_count) {
        // Empty-state: centred icon + helpful two-line hint.
        lv_obj_t *wrap = lv_obj_create(g_list_widget);
        lv_obj_remove_style_all(wrap);
        lv_obj_set_size(wrap, LV_PCT(100), LV_PCT(100));
        lv_obj_clear_flag(wrap, LV_OBJ_FLAG_SCROLLABLE);

        lv_obj_t *ico = lv_label_create(wrap);
        lv_label_set_text(ico, LV_SYMBOL_SD_CARD);
        lv_obj_set_style_text_color(ico, lv_color_hex(0x333333), 0);
        lv_obj_set_style_text_font(ico, &lv_font_montserrat_48, 0);
        lv_obj_align(ico, LV_ALIGN_CENTER, 0, -50);

        lv_obj_t *t1 = lv_label_create(wrap);
        lv_label_set_text(t1, "No apps yet");
        lv_obj_set_style_text_color(t1, lv_color_hex(0xCCCCCC), 0);
        lv_obj_set_style_text_font(t1, &lv_font_montserrat_22, 0);
        lv_obj_align(t1, LV_ALIGN_CENTER, 0, 10);

        lv_obj_t *t2 = lv_label_create(wrap);
        lv_label_set_text(t2, "Drop compiled .bin files into\n/WATCH/bin/ on the SD card.");
        lv_obj_set_style_text_align(t2, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_text_color(t2, lv_color_hex(0x666666), 0);
        lv_obj_set_style_text_font(t2, &lv_font_montserrat_14, 0);
        lv_obj_align(t2, LV_ALIGN_CENTER, 0, 60);
        return;
    }

    int order[MAX_APPS]; compute_order(order);
    if (g_fav_count > 0) {
        add_section_header(g_list_widget, "FAVOURITES", 0xFFB300);
    }
    int sf = 0;
    for (int k = 0; k < g_app_count; k++) {
        int i = order[k];
        bool isFav = favs_contains(g_apps[i].name);
        if (g_fav_count > 0 && sf == g_fav_count && !isFav) {
            add_section_header(g_list_widget, "ALL APPS", 0x00E5FF);
        }
        if (isFav) sf++;
        add_app_card(g_list_widget, i, isFav);
    }
}

static void build_screen_apps() {
    lv_obj_t *s = lv_obj_create(NULL);
    screens[SCREEN_APPS] = s;
    lv_obj_set_style_bg_color(s, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(s, LV_OPA_COVER, 0);
    lv_obj_clear_flag(s, LV_OBJ_FLAG_SCROLLABLE);

    // Title
    lv_obj_t *h1 = lv_label_create(s);
    lv_label_set_text(h1, "Apps");
    lv_obj_set_style_text_color(h1, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(h1, &lv_font_montserrat_28, 0);
    lv_obj_align(h1, LV_ALIGN_TOP_LEFT, 24, 22);

    // Counter chip on the right (e.g. "12 apps")
    lv_obj_t *chip = lv_obj_create(s);
    lv_obj_remove_style_all(chip);
    lv_obj_set_size(chip, 90, 26);
    lv_obj_align(chip, LV_ALIGN_TOP_RIGHT, -24, 28);
    lv_obj_set_style_radius(chip, 13, 0);
    lv_obj_set_style_bg_color(chip, lv_color_hex(0x141414), 0);
    lv_obj_set_style_bg_opa(chip, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(chip, lv_color_hex(0x00E5FF), 0);
    lv_obj_set_style_border_width(chip, 1, 0);
    lv_obj_clear_flag(chip, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *chip_l = lv_label_create(chip);
    char cb[24]; snprintf(cb, sizeof(cb), "%d apps", g_app_count);
    lv_label_set_text(chip_l, cb);
    lv_obj_set_style_text_color(chip_l, lv_color_hex(0x00E5FF), 0);
    lv_obj_set_style_text_font(chip_l, &lv_font_montserrat_14, 0);
    lv_obj_center(chip_l);

    // Subtitle
    lv_obj_t *h2 = lv_label_create(s);
    lv_label_set_text(h2, "/WATCH/bin  ·  long-press to star");
    lv_obj_set_style_text_color(h2, lv_color_hex(0x666666), 0);
    lv_obj_set_style_text_font(h2, &lv_font_montserrat_12, 0);
    lv_obj_align(h2, LV_ALIGN_TOP_LEFT, 24, 58);

    // Scrollable card list
    g_list_widget = lv_obj_create(s);
    lv_obj_set_size(g_list_widget, LCD_WIDTH - 30, LCD_HEIGHT - 130);
    lv_obj_align(g_list_widget, LV_ALIGN_TOP_MID, 0, 84);
    lv_obj_set_style_bg_opa(g_list_widget, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(g_list_widget, 0, 0);
    lv_obj_set_style_pad_all(g_list_widget, 6, 0);
    lv_obj_set_style_pad_row(g_list_widget, 10, 0);
    lv_obj_set_flex_flow(g_list_widget, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scroll_dir(g_list_widget, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(g_list_widget, LV_SCROLLBAR_MODE_AUTO);
    // Subtle scrollbar styling
    lv_obj_set_style_bg_color(g_list_widget, lv_color_hex(0x00E5FF), LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_opa(g_list_widget, 120, LV_PART_SCROLLBAR);
    lv_obj_set_style_radius(g_list_widget, 2, LV_PART_SCROLLBAR);
    lv_obj_set_style_width(g_list_widget, 3, LV_PART_SCROLLBAR);

    populate_app_list();

    // Footer hint
    lv_obj_t *foot = lv_label_create(s);
    lv_label_set_text(foot, LV_SYMBOL_LEFT "  face   ·   battery  " LV_SYMBOL_RIGHT);
    lv_obj_set_style_text_color(foot, lv_color_hex(0x555555), 0);
    lv_obj_set_style_text_font(foot, &lv_font_montserrat_12, 0);
    lv_obj_align(foot, LV_ALIGN_BOTTOM_MID, 0, -10);
}

// ============================================================================
//  BATTERY SCREEN  --  big % arc + remaining time + voltage
// ============================================================================
static lv_obj_t *batt_arc=nullptr, *batt_pct_lbl=nullptr;
static lv_obj_t *batt_state=nullptr, *batt_remain=nullptr;
static lv_obj_t *batt_volt=nullptr;
static lv_obj_t *batt_wifi_btn=nullptr, *batt_wifi_lbl=nullptr;
static lv_obj_t *batt_wifi_status=nullptr;
static volatile bool g_wifi_scan_busy = false;

// forward decl -- defined further down (after screen_off/screen_on)
static void deep_sleep_now();

static void update_battery_screen() {
    if (!batt_arc) return;
    if (!g_has_pmu || g_batt_pct < 0) {
        lv_label_set_text(batt_pct_lbl, "--%");
        lv_label_set_text(batt_state, "no PMU");
        lv_label_set_text(batt_remain, "");
        lv_label_set_text(batt_volt, "");
        return;
    }
    int pct = g_batt_pct; if (pct<0) pct=0; if (pct>100) pct=100;
    lv_arc_set_value(batt_arc, pct);
    char b[16]; snprintf(b, 16, "%d%%", pct);
    lv_label_set_text(batt_pct_lbl, b);

    lv_color_t col = pct<=15 ? lv_color_hex(0xFF4040)
                   : pct<=30 ? lv_color_hex(0xFFB300)
                             : lv_color_hex(0x00FF80);
    lv_obj_set_style_arc_color(batt_arc, col, LV_PART_INDICATOR);
    lv_obj_set_style_text_color(batt_pct_lbl, col, 0);

    if (g_charging) {
        lv_label_set_text(batt_state, LV_SYMBOL_CHARGE "  charging");
        lv_obj_set_style_text_color(batt_state, lv_color_hex(0x00E5FF), 0);
        lv_label_set_text(batt_remain, "");
    } else {
        lv_label_set_text(batt_state, "on battery");
        lv_obj_set_style_text_color(batt_state, lv_color_hex(0xBBBBBB), 0);
        int mins = batt_minutes_left();
        if (mins < 0) lv_label_set_text(batt_remain, "");
        else {
            char r[48];
            int h = mins / 60, m = mins % 60;
            if (h > 0) snprintf(r, 48, "~ %d h %02d min remaining", h, m);
            else       snprintf(r, 48, "~ %d min remaining", m);
            lv_label_set_text(batt_remain, r);
        }
    }
    char v[32]; snprintf(v, 32, "%.2f V", g_batt_mv / 1000.0f);
    lv_label_set_text(batt_volt, v);
}

static void batt_wifi_set_status(const char *txt, uint32_t color = 0xBBBBBB) {
    if (!batt_wifi_status) return;
    lv_label_set_text(batt_wifi_status, txt);
    lv_obj_set_style_text_color(batt_wifi_status, lv_color_hex(color), 0);
    lv_timer_handler();   // force a paint so the user sees the update
}

// Try one (ssid, pass) with up to 2 attempts.  Returns true on success.
// Pumps LVGL on every tick so the UI doesn't freeze during the ~6s wait.
static bool batt_wifi_try_one(const char *ssid, const char *pass) {
    if (!ssid || !*ssid) return false;
    for (int attempt = 1; attempt <= 2; attempt++) {
        char line[96];
        snprintf(line, sizeof(line), "Trying %s  (%d/2)...", ssid, attempt);
        batt_wifi_set_status(line, 0x00E5FF);
        USBSerial.printf("[wifi-reconnect] %s\n", line);

        WiFi.disconnect(true, false);
        delay(50);
        WiFi.begin(ssid, pass);

        uint32_t deadline = millis() + 6000;
        while (millis() < deadline && WiFi.status() != WL_CONNECTED) {
            lv_timer_handler();
            delay(50);
        }
        if (WiFi.status() == WL_CONNECTED) return true;
    }
    return false;
}

// The button handler -- walks every known network (SD card list first, then
// the hardcoded fallback) with 2 tries each, then runs an NTP top-up on
// success.  Re-entrancy guarded by g_wifi_scan_busy.
static void batt_wifi_reconnect_all() {
    if (g_wifi_scan_busy) return;
    g_wifi_scan_busy = true;

    if (batt_wifi_btn) lv_obj_add_state(batt_wifi_btn, LV_STATE_DISABLED);
    if (batt_wifi_lbl) lv_label_set_text(batt_wifi_lbl, LV_SYMBOL_WIFI "  scanning...");

    g_wifi_connected = false;
    g_ntp_synced     = false;
    WiFi.mode(WIFI_STA);

    bool ok = false;

    // 1) SD-card configured networks (up to 5)
    for (int i = 0; i < g_net_count && !ok; i++) {
        ok = batt_wifi_try_one(g_nets[i].ssid.c_str(),
                               g_nets[i].pass.c_str());
    }
    // 2) Hardcoded fallback
    if (!ok) ok = batt_wifi_try_one(HC_WIFI_SSID, HC_WIFI_PASS);

    g_wifi_connected = ok;

    if (ok) {
        batt_wifi_set_status("Connected, syncing time...", 0x00FF80);
        configTime(g_gmt_offset_sec, 0, "pool.ntp.org");
        struct tm tm;
        for (int i = 0; i < 30 && !g_ntp_synced; i++) {
            if (getLocalTime(&tm, 200)) g_ntp_synced = true;
            lv_timer_handler();
        }
        char done[96];
        snprintf(done, sizeof(done),
                 g_ntp_synced ? "Connected: %s  (time OK)"
                              : "Connected: %s  (NTP failed)",
                 WiFi.SSID().c_str());
        batt_wifi_set_status(done, g_ntp_synced ? 0x00FF80 : 0xFFB300);
        if (g_ntp_synced) time_save_now("ntp ok (battery btn)");
    } else {
        batt_wifi_set_status("All networks failed", 0xFF4040);
    }

    if (batt_wifi_lbl) lv_label_set_text(batt_wifi_lbl, LV_SYMBOL_WIFI "  Reconnect");
    if (batt_wifi_btn) lv_obj_clear_state(batt_wifi_btn, LV_STATE_DISABLED);

    lastTouchMs = millis();   // pretend the user just touched -- prevent immediate sleep
    g_wifi_scan_busy = false;
}

static void build_screen_battery() {
    lv_obj_t *s = lv_obj_create(NULL);
    screens[SCREEN_BATTERY] = s;
    lv_obj_set_style_bg_color(s, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(s, LV_OPA_COVER, 0);
    lv_obj_clear_flag(s, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title = lv_label_create(s);
    lv_label_set_text(title, "Battery");
    lv_obj_set_style_text_color(title, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_22, 0);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 24, 22);

    lv_obj_t *sub = lv_label_create(s);
    lv_label_set_text(sub, "AXP2101");
    lv_obj_set_style_text_color(sub, lv_color_hex(0x00E5FF), 0);
    lv_obj_set_style_text_font(sub, &lv_font_montserrat_14, 0);
    lv_obj_align(sub, LV_ALIGN_TOP_LEFT, 24, 50);

    // The big percent arc
    batt_arc = lv_arc_create(s);
    lv_obj_set_size(batt_arc, 280, 280);
    lv_obj_align(batt_arc, LV_ALIGN_CENTER, 0, -10);
    lv_arc_set_rotation(batt_arc, 135);
    lv_arc_set_bg_angles(batt_arc, 0, 270);
    lv_arc_set_range(batt_arc, 0, 100);
    lv_arc_set_value(batt_arc, 0);
    lv_obj_remove_style(batt_arc, NULL, LV_PART_KNOB);
    lv_obj_clear_flag(batt_arc, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_arc_color(batt_arc, lv_color_hex(0x141414), LV_PART_MAIN);
    lv_obj_set_style_arc_width(batt_arc, 14, LV_PART_MAIN);
    lv_obj_set_style_arc_color(batt_arc, lv_color_hex(0x00FF80), LV_PART_INDICATOR);
    lv_obj_set_style_arc_width(batt_arc, 14, LV_PART_INDICATOR);

    batt_pct_lbl = lv_label_create(s);
    lv_label_set_text(batt_pct_lbl, "--%");
    lv_obj_set_style_text_color(batt_pct_lbl, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(batt_pct_lbl, &FACE_TIME_FONT, 0);
    lv_obj_align(batt_pct_lbl, LV_ALIGN_CENTER, 0, -10);

    batt_state = lv_label_create(s);
    lv_label_set_text(batt_state, "");
    lv_obj_set_style_text_color(batt_state, lv_color_hex(0xBBBBBB), 0);
    lv_obj_set_style_text_font(batt_state, &lv_font_montserrat_18, 0);
    lv_obj_align(batt_state, LV_ALIGN_CENTER, 0, 90);

    batt_remain = lv_label_create(s);
    lv_label_set_text(batt_remain, "");
    lv_obj_set_style_text_color(batt_remain, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(batt_remain, &lv_font_montserrat_16, 0);
    lv_obj_align(batt_remain, LV_ALIGN_CENTER, 0, 125);

    batt_volt = lv_label_create(s);
    lv_label_set_text(batt_volt, "");
    lv_obj_set_style_text_color(batt_volt, lv_color_hex(0x666666), 0);
    lv_obj_set_style_text_font(batt_volt, &lv_font_montserrat_14, 0);
    lv_obj_align(batt_volt, LV_ALIGN_CENTER, 0, 160);

    // ---- WiFi reconnect button (left)  + Sleep Now button (right) ------
    // Both sit on the bottom row with a small status label above them.
    batt_wifi_status = lv_label_create(s);
    lv_label_set_text(batt_wifi_status, "");
    lv_obj_set_style_text_color(batt_wifi_status, lv_color_hex(0xBBBBBB), 0);
    lv_obj_set_style_text_font(batt_wifi_status, &lv_font_montserrat_12, 0);
    lv_obj_set_width(batt_wifi_status, LCD_WIDTH - 40);
    lv_obj_set_style_text_align(batt_wifi_status, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(batt_wifi_status, LV_ALIGN_BOTTOM_MID, 0, -95);

    // WiFi reconnect button (left)
    batt_wifi_btn = lv_btn_create(s);
    lv_obj_set_size(batt_wifi_btn, 175, 44);
    lv_obj_align(batt_wifi_btn, LV_ALIGN_BOTTOM_LEFT, 15, -40);
    lv_obj_set_style_radius(batt_wifi_btn, 22, 0);
    lv_obj_set_style_bg_color(batt_wifi_btn, lv_color_hex(0x141414), 0);
    lv_obj_set_style_border_color(batt_wifi_btn, lv_color_hex(0x00FF80), 0);
    lv_obj_set_style_border_width(batt_wifi_btn, 1, 0);
    lv_obj_add_event_cb(batt_wifi_btn,
        [](lv_event_t *){
            if (millis() - lastSwipeMs < CLICK_AFTER_SWIPE_MS) return;
            batt_wifi_reconnect_all();
        },
        LV_EVENT_CLICKED, NULL);
    batt_wifi_lbl = lv_label_create(batt_wifi_btn);
    lv_label_set_text(batt_wifi_lbl, LV_SYMBOL_WIFI "  Reconnect");
    lv_obj_set_style_text_color(batt_wifi_lbl, lv_color_hex(0x00FF80), 0);
    lv_obj_set_style_text_font(batt_wifi_lbl, &lv_font_montserrat_16, 0);
    lv_obj_center(batt_wifi_lbl);

    // ---- "Sleep Now" button (right) -- enters deep sleep (RTC kept alive)
    lv_obj_t *btn = lv_btn_create(s);
    lv_obj_set_size(btn, 175, 44);
    lv_obj_align(btn, LV_ALIGN_BOTTOM_RIGHT, -15, -40);
    lv_obj_set_style_radius(btn, 22, 0);
    lv_obj_set_style_bg_color(btn, lv_color_hex(0x141414), 0);
    lv_obj_set_style_border_color(btn, lv_color_hex(0x00E5FF), 0);
    lv_obj_set_style_border_width(btn, 1, 0);
    lv_obj_add_event_cb(btn,
        [](lv_event_t *){
            if (millis() - lastSwipeMs < CLICK_AFTER_SWIPE_MS) return;
            deep_sleep_now();
        },
        LV_EVENT_CLICKED, NULL);
    lv_obj_t *bl = lv_label_create(btn);
    lv_label_set_text(bl, LV_SYMBOL_POWER "  Sleep Now");
    lv_obj_set_style_text_color(bl, lv_color_hex(0x00E5FF), 0);
    lv_obj_set_style_text_font(bl, &lv_font_montserrat_16, 0);
    lv_obj_center(bl);

    lv_obj_t *foot = lv_label_create(s);
    lv_label_set_text(foot, "press BOOT to wake from deep sleep");
    lv_obj_set_style_text_color(foot, lv_color_hex(0x444444), 0);
    lv_obj_set_style_text_font(foot, &lv_font_montserrat_12, 0);
    lv_obj_align(foot, LV_ALIGN_BOTTOM_MID, 0, -10);
}

// ============================================================================
//  Screen-off  --  smooth AMOLED brightness fade (v5.8)
//  Using setBrightness instead of displayOff/displayOn because the CO5300
//  sometimes comes back in a flaky state after a DISPOFF/DISPON cycle.
//
//  fade_brightness() ramps the panel in ~12 steps over `dur_ms` and pumps
//  LVGL between each step so any in-flight animation keeps rendering.
//  This is what removes the "snap to black / snap to bright" feel.
// ============================================================================
static uint8_t g_cur_brightness = DEFAULT_BRIGHTNESS;

static void fade_brightness(uint8_t target, uint32_t dur_ms) {
    int from = g_cur_brightness;
    int to   = target;
    if (from == to) return;
    const int steps = 12;
    int step_ms = (int)dur_ms / steps;
    if (step_ms < 1) step_ms = 1;
    for (int i = 1; i <= steps; i++) {
        int b = from + ((to - from) * i) / steps;
        if (b < 0)   b = 0;
        if (b > 255) b = 255;
        static_cast<Arduino_CO5300 *>(gfx)->setBrightness((uint8_t)b);
        g_cur_brightness = (uint8_t)b;
        lv_timer_handler();                 // keep LVGL anims fluid mid-fade
        delay(step_ms);
    }
    static_cast<Arduino_CO5300 *>(gfx)->setBrightness(target);
    g_cur_brightness = target;
}

static void screen_off() {
    if (screenIsOff) return;
    screenIsOff = true;
    fade_brightness(0, 220);                // smooth ~220 ms ramp to black
    // Clear the sticky touch interrupt flag.  The FT3168 ISR sets this on
    // every TP_INT falling-edge but nothing clears it during normal swipes
    // (that was an intentional v5.2 fix).  If we don't drop it here, the
    // wake check in loop() sees a permanently-true flag and turns the
    // screen back on immediately -- the "5-second flash off" symptom.
    FT3168->IIC_Interrupt_Flag = false;
}
static void screen_on() {
    if (!screenIsOff) return;
    screenIsOff = false;
    // Repaint first, then fade UP -- otherwise the user sees the stale
    // pre-sleep frame brighten before the new one lands.
    lv_obj_invalidate(lv_scr_act());
    lv_timer_handler();
    fade_brightness(DEFAULT_BRIGHTNESS, 200);
    lastTouchMs = millis();
    lastWakeMs  = millis();                    // arm the 2 s post-wake grace
}

// ============================================================================
//  DEEP SLEEP  --  manual "Sleep Now" from the Battery card.
//  Powers down everything except the internal RTC (which keeps the wall
//  clock ticking) and the GPIO0 / BOOT wake input.  Press BOOT to wake.
//  Draw drops to ~150-300 uA (about 50 days on a 400 mAh pack).
// ============================================================================
static void deep_sleep_now() {
    USBSerial.println("[pm] entering deep sleep -- press BOOT to wake");
    // v6.1: snapshot the wall clock so a full power loss while "asleep"
    // (battery yanked, contact lost, etc.) still leaves us with the time.
    time_save_now("entering deep sleep");
    // Quick visual confirmation before we go dark.
    gfx->fillScreen(RGB565_BLACK);
    gfx->setCursor(LCD_WIDTH/2 - 60, LCD_HEIGHT/2);
    gfx->setTextColor(0xFFFF);
    gfx->setTextSize(2);
    gfx->print("sleeping...");
    delay(280);
    fade_brightness(0, 260);                  // graceful fade-out into deep sleep
    // Make sure WiFi + BT are fully off so wake-from-deep-sleep is clean.
    WiFi.disconnect(true, true);
    WiFi.mode(WIFI_OFF);
    btStop();

    // -------------------------------------------------------------------
    // Keep the RTC alive across deep sleep so the wall clock keeps ticking.
    //
    // On Arduino-ESP32 core 3.x (IDF v5.x) the RTC slow/fast memory
    // domains are always retained automatically and the enums for them
    // were removed -- only RTC_PERIPH remains in esp_sleep_pd_config().
    // Pinning RTC_PERIPH ON guarantees the RTC main clock keeps running
    // so getLocalTime() returns the right time on wake without an NTP
    // round-trip.
    // -------------------------------------------------------------------
    esp_sleep_pd_config(ESP_PD_DOMAIN_RTC_PERIPH, ESP_PD_OPTION_ON);

    // Only the BOOT button (GPIO0) is RTC-capable, so only it can wake us.
    esp_sleep_enable_ext1_wakeup((1ULL << 0), ESP_EXT1_WAKEUP_ANY_LOW);
    esp_deep_sleep_start();   // never returns
}

// ============================================================================
//  SETUP
// ============================================================================
void setup() {
    USBSerial.begin(115200);

    // Are we coming back from a deep sleep (Sleep Now)?  If so, the internal
    // RTC still has the wall clock, so skip splash + WiFi for a fast wake.
    bool from_deep_sleep =
        (esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_EXT1);

    USBSerial.printf("\n=== LauncherOS v6.1 (loader UX + persistent RTC) ===  %s\n",
                     from_deep_sleep ? "(wake from deep sleep)" : "(cold boot)");
    // v6.0: re-anchor the bootloader on ourselves so any loaded app that
    // reboots (intentionally, via crash, or via power-cycle) comes back
    // to LauncherOS instead of into the last-flashed OTA slot.
    anchor_launcher_as_boot();
    // v6.1: rehydrate the wall clock from NVS *before* anything else --
    // this way the splash + face show a real time even on a cold boot
    // with no WiFi, instead of "00:00".
    time_restore_from_nvs();


#if POWER_SAVE
    // Kill Bluetooth radio -- we never use it.  Saves ~5-10 mA.
    btStop();
    // Make sure GPIO0 (BOOT / power button) has its internal pull-up
    // enabled so it reads HIGH at rest -- light-sleep wake on LOW depends
    // on a clean idle level.
    pinMode(0, INPUT_PULLUP);
    pinMode(PWR_BTN_PIN, INPUT_PULLUP);   // side power button
#endif

    if (!gfx->begin()) USBSerial.println("gfx->begin() failed");
    gfx->fillScreen(RGB565_BLACK);
    // Start the panel completely dark.  We fade up AFTER build_splash() has
    // rendered the first frame, so the user never sees a stale buffer or
    // a hard "BANG, bright!" moment at power-on.
    static_cast<Arduino_CO5300 *>(gfx)->setBrightness(0);
    g_cur_brightness = 0;

    // I2C bus recovery: clock out any stuck transaction before Wire.begin()
    pinMode(IIC_SCL, OUTPUT);
    for (int i = 0; i < 9; i++) {
        digitalWrite(IIC_SCL, LOW); delayMicroseconds(5);
        digitalWrite(IIC_SCL, HIGH); delayMicroseconds(5);
    }

    Wire.begin(IIC_SDA, IIC_SCL);

    // Touch controller reset pulse
    pinMode(TP_RESET, OUTPUT);
    digitalWrite(TP_RESET, LOW);
    delay(50);
    digitalWrite(TP_RESET, HIGH);
    delay(200);

    pinMode(TP_INT, INPUT_PULLUP);
    while (FT3168->begin() == false) { USBSerial.println("FT3168 init fail"); delay(500); }
    FT3168->IIC_Write_Device_State(
        FT3168->Arduino_IIC_Touch::Device::TOUCH_POWER_MODE,
        FT3168->Arduino_IIC_Touch::Device_Mode::TOUCH_POWER_MONITOR);

    pmu_init();
    pmu_refresh();
    favs_load();

    lv_init();
    size_t bufPx = LCD_WIDTH * 60;
    lvgl_buf = (lv_color_t *)heap_caps_malloc(bufPx*sizeof(lv_color_t),
                                              MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT);
    if (!lvgl_buf) lvgl_buf = (lv_color_t *)heap_caps_malloc(bufPx*sizeof(lv_color_t), MALLOC_CAP_8BIT);
    lv_disp_draw_buf_init(&draw_buf, lvgl_buf, NULL, bufPx);

    lv_disp_drv_init(&disp_drv);
    disp_drv.flush_cb   = my_disp_flush;
    disp_drv.draw_buf   = &draw_buf;
    disp_drv.hor_res    = LCD_WIDTH;
    disp_drv.ver_res    = LCD_HEIGHT;
    lv_disp_drv_register(&disp_drv);

    lv_indev_drv_init(&indev_drv);
    indev_drv.type    = LV_INDEV_TYPE_POINTER;
    indev_drv.read_cb = my_touchpad_read;
    lv_indev_drv_register(&indev_drv);

    build_splash();
    lv_timer_handler();                      // render the first splash frame
    fade_brightness(DEFAULT_BRIGHTNESS, 260); // soft fade-up out of power-on black
    splash_progress(10, "init display");
    splash_progress(25, "mounting SD");
    SD_MMC.setPins(SDMMC_CLK, SDMMC_CMD, SDMMC_DATA);
    bool sd_ok = SD_MMC.begin("/sdcard", true);
    if (sd_ok) {
        splash_progress(40, "wifi config");      parse_wifi_config();
        if (from_deep_sleep) {
            // RTC kept time -- skip WiFi entirely on this wake.
            struct tm tm;
            if (getLocalTime(&tm, 5)) g_ntp_synced = true;
            splash_progress(70, "skipping wifi (RTC retained)");
        } else {
            splash_progress(55, "connecting wifi");
            wifi_connect_and_sync();
        }
        splash_progress(82, "scanning apps");    scan_apps();
        splash_progress(92, "scanning photos");  scan_photos();
        // Restore the last-used wallpaper index so the watch face -- and
        // any app that reads "wp_idx" from the same Preferences namespace
        // -- picks up where we left off.
        {
            g_prefs.begin("dcw", true);
            int saved_wp = g_prefs.getInt("wp_idx", 0);
            g_prefs.end();
            if (g_photo_count > 0) {
                if (saved_wp < 0 || saved_wp >= g_photo_count) saved_wp = 0;
                g_photo_idx = saved_wp;
            }
        }
    } else {
        splash_progress(50, "SD mount failed");  delay(800);
    }
    splash_progress(100, "ready");
    delay(from_deep_sleep ? 100 : 400);   // quick wake when coming back

    // Build all three screens once, then cross-fade from the splash into
    // the watch face.  A 260 ms fade reads as "boot finished, here's the
    // watch" rather than a hard cut.
    build_screen_face();
    build_screen_apps();
    build_screen_battery();
    current_screen = SCREEN_FACE;
    lv_scr_load_anim(screens[SCREEN_FACE], LV_SCR_LOAD_ANIM_FADE_ON, 260, 0, false);
    lastTouchMs = millis();

#if POWER_SAVE
    // Drop the CPU to 80 MHz now that boot/WiFi/SD are done.  Plenty fast
    // for LVGL on this panel.  Bumped back up to 240 only during OTA copy.
    setCpuFrequencyMhz(CPU_FREQ_IDLE_MHZ);
    USBSerial.printf("[pm] CPU @ %d MHz\n", CPU_FREQ_IDLE_MHZ);
#endif
}

// ============================================================================
//  LOOP
// ============================================================================
void loop() {
    // LVGL tick -- skipped when LV_TICK_CUSTOM == 1 (our lv_conf.h sets it).
    static uint32_t lastMs = 0;
    uint32_t now = millis();
#if !defined(LV_TICK_CUSTOM) || (LV_TICK_CUSTOM == 0)
    lv_tick_inc(now - lastMs);
#endif
    lastMs = now;

    // v5.5: ALWAYS pump LVGL.  When the panel is dark we still want the
    // LVGL timer chain and the indev callback to keep running -- stopping
    // it caused the AXP2101 PWRKEY watchdog / task watchdog to bite after
    // a few seconds of inactivity, which is what was rebooting the watch
    // back into the splash screen every time it dimmed.
    lvgl_pump();

    // Consume any queued swipe (set by my_touchpad_read on release).
    if (!screenIsOff && g_pending_swipe != SWIPE_NONE) {
        SwipeEvent ev = g_pending_swipe;
        g_pending_swipe = SWIPE_NONE;
        lastSwipeMs = now;                          // arm the post-swipe click-guard
        if (ev == SWIPE_LEFT)  goto_screen(current_screen + 1);
        if (ev == SWIPE_RIGHT) goto_screen(current_screen - 1);
    }

    // -------------------------------------------------------------------
    // Side PWR button -- routed through the AXP2101 PMU (not a GPIO).
    // Short press toggles the screen on/off; long press (>6 s) triggers
    // the AXP2101's hardware power-off.  A 400 ms debounce window guards
    // against any IRQ noise that might otherwise double-fire.
    // -------------------------------------------------------------------
    if (g_has_pmu) {
        static uint32_t pwr_last_ms = 0;
        static uint32_t pwr_last_fire_ms = 0;
        if (now - pwr_last_ms > 50) {           // 50 ms poll is plenty
            pwr_last_ms = now;
            g_pmu.getIrqStatus();
            if (g_pmu.isPekeyShortPressIrq()) {
                if (now - pwr_last_fire_ms > 400) {   // debounce
                    pwr_last_fire_ms = now;
                    USBSerial.println("[PWR] AXP2101 PKEY short press");
                    if (screenIsOff) {
                        screen_on();
                        lastTouchMs = now;
                    } else {
                        screen_off();
                    }
                } else {
                    USBSerial.println("[PWR] PKEY debounced (noise)");
                }
            }
            g_pmu.clearIrqStatus();
        }
    }

    // 1s tick - update only the visible screen.
    static uint32_t tT = 0;
    if (now - tT > 1000) {
        tT = now;
        if (!screenIsOff) {
            if (current_screen == SCREEN_FACE)    update_face_time();
            if (current_screen == SCREEN_BATTERY) update_battery_screen();
        }
    }
    // 3s tick - refresh battery readings (PMU) regardless of screen.
    static uint32_t tB = 0;
    if (now - tB > 3000) {
        tB = now;
        if (!screenIsOff) {
            pmu_refresh();
            if (current_screen == SCREEN_FACE)    update_face_battery();
            if (current_screen == SCREEN_BATTERY) update_battery_screen();
        }
    }

    // v6.1: persist the wall clock to NVS every TIME_PERSIST_INTERVAL_MS
    // (default 5 min) so a sudden power loss never costs more than ~5 min
    // of accuracy on the next boot.
    if (g_ntp_synced && (now - g_last_time_save_ms) > TIME_PERSIST_INTERVAL_MS) {
        time_save_now("periodic");
    }

    // Screen-off countdown (v5.13 -- hysteresis-backed).
    //   Now that my_touchpad_read uses release hysteresis (3 consecutive
    //   fingers=0 reads required before declaring release), `touchActive`
    //   is reliable on its own.  Two keep-alive layers remain:
    //     1. touchActive          -- finger is genuinely on the panel
    //     2. WAKE_GRACE_MS window -- 2 s after every screen_on(),
    //        the auto-timeout cannot fire (prevents wake -> immediately
    //        re-sleep race conditions).
    //   The old TP_INT level check was dropped because that line PULSES
    //   low on each new-data interrupt rather than staying low while a
    //   finger is down -- it was a misleading signal here.
    //   For the screen-off WAKE path we still use digitalRead(TP_INT):
    //   we only need to catch the pulse moment to know "user just
    //   touched the panel", which is exactly what TP_INT gives us.
    bool in_wake_grace = (now - lastWakeMs < WAKE_GRACE_MS);
    if (touchActive || in_wake_grace) {
        lastTouchMs = now;
    }
    if (!screenIsOff && (now - lastTouchMs > SCREEN_OFF_TIMEOUT_MS)) {
        screen_off();
    } else if (screenIsOff && digitalRead(TP_INT) == LOW) {
        lastTouchMs = now;                          // wake-tap resets the countdown
        screen_on();
    }

#if POWER_SAVE
    // Periodic NTP re-sync (briefly turns WiFi back on, syncs, drops it).
    if (!screenIsOff) wifi_resync_if_due();
#endif

#if POWER_SAVE && LIGHT_SLEEP_WHEN_OFF
    // When the screen is off, sleep the SoC until either:
    //   * the BOOT/power button pulls GPIO0 LOW   (ext1 wake -- instant)
    //   * the 80 ms timer wakes us so we can poll the touch line
    //     (TP_INT / GPIO38 is NOT an RTC IO on the ESP32-S3, so it can't
    //      directly wake the chip -- we poll it with digitalRead instead)
    if (screenIsOff) {
        const uint64_t WAKE_MASK = (1ULL << 0);             // BOOT button only
        esp_sleep_enable_ext1_wakeup(WAKE_MASK, ESP_EXT1_WAKEUP_ANY_LOW);
        esp_sleep_enable_timer_wakeup(80ULL * 1000ULL);     // 80 ms touch poll
        esp_light_sleep_start();

        esp_sleep_wakeup_cause_t why = esp_sleep_get_wakeup_cause();
        bool wake = false;

        if (why == ESP_SLEEP_WAKEUP_EXT1) {
            // BOOT button pressed
            wake = true;
        } else if (why == ESP_SLEEP_WAKEUP_TIMER) {
            // Timer tick -- quickly check if the touch IRQ line is low
            // (FT3168 pulls TP_INT LOW while a finger is on the screen).
            if (digitalRead(TP_INT) == LOW) wake = true;
        }

        if (wake) {
            FT3168->IIC_Interrupt_Flag = true;
            lastTouchMs = millis();
        }
        return;   // skip the delay() below -- we already slept
    }
#endif

    delay(5);
}
