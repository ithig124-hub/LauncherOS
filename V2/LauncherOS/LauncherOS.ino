// ============================================================================
//  LauncherOS.ino   --   DualCore Watch launcher  (LVGL 8.3 / 8.4)
//  ----------------------------------------------------------------------------
//  ----------------------------------------------------------------------------
//  v7.1 -- UNIVERSAL CLOCK (shared time contract, see TIME_CONTRACT.md).
//      * the wall clock lives in ONE place: NVS "rtcsave" (t = epoch,
//        gmt = offset in SECONDS, seq, man, src).  The SHARED TIME BLOCK
//        below is inlined verbatim into every app under V2/.
//      * THE BUG THIS FIXES: the epoch rides in the chip's RTC across a
//        launcher <-> module reboot, but the TIMEZONE lives in the C
//        library's RAM-only TZ string and dies on EVERY reboot.  Every app
//        now re-applies TZ from the snapshot at boot, so UTC+8 renders as
//        UTC+8 everywhere instead of UTC.
//      * the launcher WRITES the clock to the chip (settimeofday) + NVS at
//        every sync point, when GMT_OFFSET is (re)read, every 5 min, on
//        entering deep sleep, and immediately before it reboots into an
//        app -- and ADOPTS the snapshot on the way back when a module
//        moved it.
//
//  v7.0 (merged) -- 11 themes, card-based Settings, launcher styles, 3D
//                   carousel loader, developer info, factory reset.
//
//      * THEME ENGINE  (11 built-in themes; "OG LauncherOS" is the default
//        and reproduces the pre-merge visual identity 1:1.  The remaining
//        10 themes come from v7.0.)  Selection is persisted in NVS under
//        the "wctheme"/"idx" key.  All UI elements (cards, pills, arcs,
//        sliders, toggles, loaders) read from theme_primary() / accent /
//        bg / fg / warn / danger.
//
//      * CARD-BASED SETTINGS SCREEN.  Toggle / Slider / Dropdown / Action
//        rows for WiFi, BLE, Haptics, Brightness (20-255), Theme,
//        Launcher Style, Quick-swap Wallpaper, Open Wallpaper Picker,
//        Open Developer Info, Reconnect WiFi, Factory Reset, Deep Sleep.
//        The base's storage summary card + full-screen wallpaper picker
//        are preserved -- picker opens as a subview from the Wallpaper
//        card row.
//
//      * LAUNCHER STYLES  (persisted as "launcher_style" in NVS).
//          - PILL (default):   the base's categorised pill list with
//                              Favourites + categories.txt bucketing.
//          - COVERFLOW:        3D-style infinite carousel (from v7.0),
//                              flat list of .bins.
//          - GRID:             scrollable tile grid (from v7.0),
//                              flat list of .bins.
//
//      * 3D CAROUSEL .BIN LOADER.  The OTA copy screen now renders a
//        subtly rotating ring of card slices around the progress %.
//        Real progress, ETA, and error handling are preserved.  If PSRAM
//        allocation for the ring fails we fall back to the v6.4 bar.
//
//      * FACTORY RESET modal + DEVELOPER INFO subview  (heap, PSRAM,
//        app / wallpaper counts, active theme, OS version).
//
//      * Everything from the v6.4 base is untouched: click-guard input,
//        release hysteresis, boot-partition anchor, category-based
//        favourites-first pill list, NVS app-list cache, twice-daily
//        NTP resync, deep sleep, wake-from-BOOT, brightness fades, OTA
//        size pre-check on .bin launch.
//  ----------------------------------------------------------------------------
//  v6.4 -- Settings screen + wallpaper picker + categorised app list.
//      * NEW screen inserted between APPS and BATTERY.  Swipe order is
//        now  FACE  <->  APPS  <->  SETTINGS  <->  BATTERY.
//      * Wallpaper picker: a scrollable 2-column grid on the Settings
//        screen shows every /WATCH/PHOTOS/*.bin as a tappable tile.
//        Tap = apply + persist (writes the same NVS "wp_idx" key the
//        long-press-on-face path used).  The current wallpaper is
//        highlighted with a gold border + star badge.
//      * Watch-face is now READ-ONLY: tapping or long-pressing it no
//        longer changes the wallpaper.  The whole "tap zone" object and
//        the face_tap_cb wiring have been removed -- the only way to
//        change the wallpaper is now the Settings picker.  Swipes across
//        the face still work (they're handled at the indev level).
//      * Storage summary cards on Settings: "bin" (N apps) and "PHOTOS"
//        (N wallpapers).  Currently informational -- lays the groundwork
//        for tap-to-manage later.
//      * App list is now bucketed.  If /WATCH/bin/categories.txt exists
//        (format:  AppName = Category  per line, # for comments), the
//        Apps screen shows Favourites first, then each Category as its
//        own section header + pill run, then an "Other" bucket for
//        anything unlisted.  With no categories.txt on the card the
//        list falls back to the v6.3 flat favourites-first layout.
//  ----------------------------------------------------------------------------
//  v6.3 -- battery pass + scrollable pill app list (favourites preserved).
//      Battery (all existing features and wake paths UNCHANGED):
//        * POWER_SAVE turned ON by default: CPU 80 MHz idle, BT off, WiFi
//          off between 12 h NTP resyncs, charge LED off.
//        * Light-sleep-when-off enabled with a 250 ms touch-poll tick.
//          BOOT button (GPIO0, ext1) still wakes the watch INSTANTLY --
//          this is the exact "current boot sequence" path the user wants
//          preserved.  Manual "Sleep Now" deep sleep is unchanged.
//        * Adaptive loop() idle (driven by LVGL's next-timer hint) so the
//          SoC isn't busy-looping at 5 ms while the UI is static.
//        * PMU re-read every 15 s (or 5 s while charging) instead of 3 s.
//        * Default brightness 180 -> 140 (~25 % less AMOLED current,
//          still fully readable outdoors).
//        * I2C bus at 400 kHz so touch + PMU transactions finish ~4x faster.
//        * WiFi sync window uses modem-sleep + lower TX power.
//      App list: replaced the 3-column tile grid with a scrollable column
//      of vertical pills.  Favourite styling is intact (gold border, gold
//      name, star badge on the right) and favourites still render first
//      with the same thin divider before the rest of the apps.
//  ----------------------------------------------------------------------------
//  v6.2 -- wake-time fix + 3-column app grid + favourites-first divider.
//      * Sleep/wake time bug: after waking the panel showed the OLD time
//        (the time-of-sleep) for up to a second.  Two root causes:
//          (1) screen_on() invalidated + faded up BEFORE the 1 Hz tick
//              had a chance to refresh face_time/face_date, so the user
//              saw the stale pre-sleep frame brighten.
//          (2) time_restore_from_nvs() ran unconditionally at the top of
//              setup() and settimeofday()-ed the NVS snapshot back into
//              the RTC -- but the RTC was still ticking across deep sleep
//              (we pinned RTC_PERIPH ON), so this REWOUND the clock back
//              to the moment we slept.  Now we check the RTC first and
//              only restore from NVS on a true cold boot.
//      * App grid: replaced the v5.x card list with a 3-column rounded-
//        square tile grid.  Favourites are rendered first with a gold
//        border + gold name text, followed by a thin full-width divider,
//        then the rest of the apps in cyan-bordered tiles.  Long-press
//        still toggles starred status.  See APP_TILE_SIZE / APP_TILE_GAP
//        tunables for layout.
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

// ---- Heap helpers (esp_heap_caps for PSRAM draw-buffer + wallpaper) -------
// NOTE (LVGL 8.4 conversion): the v9 lv_*_core allocator overrides
// (lv_malloc_core / lv_realloc_core / lv_free_core / lv_mem_*_core, which
// used the v9-only lv_result_t / LV_RESULT_OK types) do NOT exist in
// LVGL 8.4 and have been removed.  In 8.4 the LVGL object pool is sized by
// LV_MEM_SIZE in lv_conf.h (LV_MEM_CUSTOM 0), exactly as the v6.4 base
// shipped.  To route LVGL's object pool into PSRAM instead, set
// LV_MEM_CUSTOM 1 in lv_conf.h and point LV_MEM_CUSTOM_ALLOC/FREE/REALLOC
// at ps_malloc/free/realloc.  The large buffers (LVGL draw buffer +
// wallpaper framebuffer) are already allocated straight from PSRAM via
// heap_caps_malloc() at their call sites.
#include "esp_heap_caps.h"
#include <string.h>

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
// v6.3 battery pass: ALL power-saving features are now ON by default.
//   * CPU drops to 80 MHz on idle screens   (~halves SoC draw)
//   * btStop() at boot                       (~5-10 mA)
//   * WiFi off after NTP, woken every 12 h   (<0.02 % radio uptime)
//   * Light sleep while screen is off        (~25 mA -> ~2-4 mA average)
//   * BOOT button always wakes instantly via ext1 (RTC-capable GPIO0)
#define POWER_SAVE              1

// Drop CPU from 240 MHz to this when sitting on a static screen.
// 80 MHz still runs the AMOLED + LVGL smoothly and roughly halves CPU draw.
#define CPU_FREQ_IDLE_MHZ      80
#define CPU_FREQ_BUSY_MHZ     240

// Default panel brightness (0-255).  AMOLED current scales ~linearly with
// pixel brightness, so dropping from 180 (~70%) to 140 (~55%) is the single
// biggest screen-on win.  Still fully readable outdoors on this panel.
#define DEFAULT_BRIGHTNESS    140

// Once NTP has synced, WiFi is turned OFF entirely.  We turn it back on
// every WIFI_RESYNC_INTERVAL_MS to refresh the clock then shut it again.
// 12 h is plenty -- the ESP32-S3 RTC drifts <2 s/day, and we re-anchor
// twice per day for a watch-quality clock at almost zero radio cost.
#define WIFI_RESYNC_INTERVAL_MS  (12UL * 60UL * 60UL * 1000UL)  // 12 hours

// When the screen is dark we drop the SoC into light sleep and only wake on
// either (a) the BOOT button via ext1 (RTC-capable GPIO0 -- instant wake,
// THIS IS THE PATH THAT MUST STAY INSTANT) or (b) a 250 ms timer tick so we
// can poll the FT3168 TP_INT line (GPIO38 is not RTC-IO-capable on every
// S3 variant, so we don't trust ext1 for it).  250 ms is the sweet spot:
// users can't perceive the touch-wake latency but standby draw drops from
// ~25 mA -> ~2-4 mA average.
#define LIGHT_SLEEP_WHEN_OFF    1
#define LIGHT_SLEEP_POLL_MS    250
// ============================================================================

// ============================================================================
//  v7.0 THEME ENGINE  --  11 built-in themes.
//
//  Index 0 is "OG LauncherOS", which reproduces the pre-merge palette
//  1:1 (cyan primary + gold accent on true-black), so an existing user
//  who never touches Settings sees zero visual change.  Indices 1..10
//  are the 10 v7.0 built-ins, in the exact order v7.0 shipped them
//  (MilSpec Green -> Nord).
//
//  Colors are stored as 0xRRGGBB.  Wrap with lv_color_hex() at the
//  call site.  All base UI now routes through these accessors, so a
//  dropdown change in Settings propagates to every screen after we
//  rebuild them once (theme_apply_and_rebuild()).
// ============================================================================
struct LTheme {
    const char *name;
    uint32_t primary, accent, bg, fg, warn, danger;
};
static const LTheme kThemes[] = {
    // OG LauncherOS -- exact v6.4 palette.
    { "OG LauncherOS", 0x00E5FF, 0xFFB300, 0x000000, 0x141414, 0xFFB300, 0xFF4040 },
    // v7.0 built-ins (order preserved from LauncherOS1.ino).
    { "MilSpec Green", 0x9BD16A, 0x5FA046, 0x0B140A, 0x2F3F26, 0xE5B14F, 0xE5484D },
    { "Amber CRT",     0xFFB300, 0xFF7A00, 0x140A00, 0x3F2600, 0xFFEA00, 0xFF3B30 },
    { "Arctic",        0xEAF6FF, 0x66C0FF, 0x081018, 0x2A3F5A, 0xFFD54F, 0xFF6B6B },
    { "Desert Tan",    0xE8C48B, 0xB07A3A, 0x191308, 0x4B3820, 0xFFC857, 0xE05B49 },
    { "Night-Vision",  0x9EFF6E, 0x39FF14, 0x040A04, 0x18331A, 0xE7FF3B, 0xFF3B30 },
    { "Blood-Red",     0xFF4A4A, 0xE81E1E, 0x140404, 0x3F1010, 0xFFB03A, 0xFF1E1E },
    { "Cyber-Cyan",    0x50E3FF, 0x00C7C7, 0x030913, 0x113344, 0xFFEE58, 0xFF476F },
    { "Synthwave",     0xFF6EC7, 0x9B5DE5, 0x0F0518, 0x3A1F55, 0xF15BB5, 0xF15BB5 },
    { "Terminal Mono", 0xEDEDED, 0x8AE234, 0x000000, 0x2A2A2A, 0xE6DB74, 0xF92672 },
    { "Nord",          0xE5E9F0, 0x88C0D0, 0x2E3440, 0x4C566A, 0xEBCB8B, 0xBF616A },
};
static const int kThemeCount = sizeof(kThemes) / sizeof(kThemes[0]);
static int  g_theme_idx = 0;

static inline const LTheme &theme() { return kThemes[g_theme_idx]; }
static inline uint32_t theme_primary() { return theme().primary; }
static inline uint32_t theme_accent()  { return theme().accent;  }
static inline uint32_t theme_bg()      { return theme().bg;      }
static inline uint32_t theme_fg()      { return theme().fg;      }
static inline uint32_t theme_warn()    { return theme().warn;    }
static inline uint32_t theme_danger()  { return theme().danger;  }
static inline int      theme_count()   { return kThemeCount;     }
static inline const char *theme_name_at(int i) {
    return (i >= 0 && i < kThemeCount) ? kThemes[i].name : "";
}

// ---- v7.0 LAUNCHER STYLE (persisted) --------------------------------------
// PILL       = base's categorised pill list w/ Favourites (default).
// COVERFLOW  = infinite 3D carousel, flat list of .bins.
// GRID       = scrollable tile grid, flat list of .bins.
enum LauncherStyle : uint8_t {
    LS_PILL      = 0,
    LS_COVERFLOW = 1,
    LS_GRID      = 2,
};
static LauncherStyle g_launcher_style = LS_PILL;

// ---- v7.0 forward declarations (definitions live near the end of file) ----
static void theme_load_from_nvs();
static void theme_save(int idx);
static void launcher_style_load_from_nvs();
static void launcher_style_save();
static void rebuild_all_screens_and_reload(int keep_current);
static void populate_apps_coverflow(lv_obj_t *parent);
static void populate_apps_grid(lv_obj_t *parent);
static void card_toggle(lv_obj_t *parent, const char *title,
                        const char *subtitle, bool initial,
                        lv_event_cb_t cb);
static void card_slider(lv_obj_t *parent, const char *title,
                        const char *subtitle, int lo, int hi, int val,
                        lv_event_cb_t cb);
static void card_dropdown(lv_obj_t *parent, const char *title,
                          const char *subtitle, const char *opts,
                          int selected, lv_event_cb_t cb);
static void card_action(lv_obj_t *parent, const char *title,
                        const char *subtitle, const char *btn_text,
                        uint32_t accent, lv_event_cb_t cb);
static void cards_on_brightness_cb(lv_event_t *e);
static void cards_on_theme_cb(lv_event_t *e);
static void cards_on_style_cb(lv_event_t *e);
static void cards_on_quick_wp_cb(lv_event_t *e);
static void cards_on_wifi_cb(lv_event_t *e);
static void cards_on_ble_cb(lv_event_t *e);
static void cards_on_haptics_cb(lv_event_t *e);
static void cards_open_wallpaper_cb(lv_event_t *e);
static void cards_open_developer_cb(lv_event_t *e);
static void cards_factory_reset_cb(lv_event_t *e);
static void cards_deep_sleep_cb(lv_event_t *e);
static void cards_reconnect_wifi_cb(lv_event_t *e);
static void show_wallpaper_picker();
static void show_developer_info();
static void show_factory_reset_modal();
static bool loader_ring_start(lv_obj_t *scr);
static void loader_ring_stop();
static void cf_goto(int i, bool animate);
// ---- v1.0 XANDER forward declarations (module defined before setup()) -----
static void xander_open_home_cb(lv_event_t *e);
static void xander_open_settings_cb(lv_event_t *e);
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

// LVGL 8.4 display + input drivers.  Partial 60-line RGB565 draw buffer;
// disp_drv / indev_drv are initialised and registered in setup().
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

// Current AMOLED brightness (0-255). Defined here (rather than next to
// fade_brightness() further down) so the card-based Settings screen --
// which is built BEFORE fade_brightness() appears in the translation unit
// -- can seed its brightness slider from this value without triggering
// "'g_cur_brightness' was not declared in this scope".
static uint8_t g_cur_brightness = DEFAULT_BRIGHTNESS;

// State backing for the v7.0 card-based Settings toggles.  Defined here
// (rather than next to their event callbacks further down) for the same
// reason as g_cur_brightness above: build_screen_settings() reads them
// while wiring the card rows, and that runs BEFORE the callbacks appear
// in the translation unit.
static bool g_wifi_toggle    = true;    // WiFi manager is on by default (12 h resync)
static bool g_ble_toggle     = false;   // BT is stopped in setup() -- default OFF
static bool g_haptics_toggle = true;    // Placeholder -- board has no vibe motor exposed

// ---- swipe queue ----------------------------------------------------------
// MiniOS-style: filled on finger-release in my_touchpad_read(), consumed
// immediately by loop().  No cooldown, no wake-grace gating.
enum SwipeEvent { SWIPE_NONE, SWIPE_LEFT, SWIPE_RIGHT, SWIPE_UP, SWIPE_DOWN };
static volatile SwipeEvent g_pending_swipe = SWIPE_NONE;

static int32_t  ts_x = 0, ts_y = 0;
static uint32_t ts_ms = 0;
static int32_t  tc_x = 0, tc_y = 0;
static bool     touchActive = false;

// LVGL 8.4 flush callback.
//   - `drv` is the registered lv_disp_drv_t.
//   - `color_p` points at the flushed area's pixels as lv_color_t.  With
//     LV_COLOR_DEPTH 16 (+ LV_COLOR_16_SWAP 0) each pixel is a native
//     little-endian RGB565 uint16_t (color_p->full), handed straight to
//     Arduino_GFX.
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
        data->state   = LV_INDEV_STATE_PRESSED;
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
        data->state = LV_INDEV_STATE_RELEASED;

        if (touchActive) {
            touchActive   = false;
            uint32_t dur  = millis() - ts_ms;
            int32_t  dx   = tc_x - ts_x;
            int32_t  dy   = tc_y - ts_y;
            // v1.0-XANDER: classify BOTH axes.  Horizontal keeps the exact
            // MiniOS-parity behaviour (launcher nav); the dominant-vertical
            // case now emits SWIPE_UP / SWIPE_DOWN so the hidden Xander layer
            // can be revealed (up from the face) and exited (down anywhere).
            bool horiz = (abs(dx) > SWIPE_THRESHOLD_PX) && (abs(dx) >= abs(dy));
            bool vert  = (abs(dy) > SWIPE_THRESHOLD_PX) && (abs(dy) >  abs(dx));
            bool gestureSwipe = (dur < SWIPE_MAX_DURATION_MS) && (horiz || vert);

            USBSerial.printf("[TOUCH] End  dx=%d dy=%d dur=%lu\n",
                             dx, dy, (unsigned long)dur);

            if (gestureSwipe) {
                // MiniOS dispatches the moment the finger lifts -- no cooldown,
                // no queue draining, no wake-grace. Match it exactly.
                // NOTE: on this panel the Y axis reads "inverted" vs a naive
                // top-left origin, so a physical UP flick yields dy > 0.  Map
                // accordingly: dy > 0 = SWIPE_UP (enter Xander), dy < 0 = DOWN.
                if (horiz) g_pending_swipe = (dx < 0) ? SWIPE_LEFT : SWIPE_RIGHT;
                else       g_pending_swipe = (dy > 0) ? SWIPE_UP   : SWIPE_DOWN;
                USBSerial.printf("[TOUCH] Swipe %s queued\n",
                                 horiz ? (dx < 0 ? "LEFT" : "RIGHT")
                                       : (dy > 0 ? "UP" : "DOWN"));
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

#if POWER_SAVE
        // v6.3: kill the charge LED -- it's a continuous 2-5 mA draw on
        // many Waveshare AMOLED watch boards, which is the same order of
        // magnitude as the SoC in light sleep.  All other AXP2101 rails
        // are left alone: this board's defaults are already minimal and
        // we don't want to risk killing a rail some app might depend on.
        g_pmu.setChargingLedMode(XPOWERS_CHG_LED_OFF);
#endif
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
// ---- shared time block (spec: TIME_CONTRACT.md) -----------------------------
#define TIME_SERIAL USBSerial
#define TIME_SRC_ID TIME_SRC_LAUNCHER
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
static uint32_t g_last_time_save_ms = 0;   // v7.1: periodic-publish timer
#define TIME_PERSIST_INTERVAL_MS  (5UL * 60UL * 1000UL)   // save every 5 min

// v7.1: kept as a thin alias so every pre-existing call site keeps working.
static void time_save_now(const char *why) { time_publish(why, false); }

// The v6.2 rule (never rewind a live RTC) is now time_decide() in the shared
// time block above, which keeps it AND adds the reverse direction -- a snapshot
// a human or a module moved wins over the live RTC.  That bidirectionality is
// what makes the clock universal instead of launcher-only.

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
#if POWER_SAVE
    // v6.3: every WiFi-on window costs current.  Drop modem-sleep on so
    // the radio idles between beacons, and lower TX power -- a watch
    // sitting next to a router doesn't need full +20 dBm.  Both still
    // let us complete the NTP round-trip in ~6 s.
    WiFi.setSleep(WIFI_PS_MIN_MODEM);
    WiFi.setTxPower(WIFI_POWER_8_5dBm);
    WiFi.persistent(false);   // don't burn flash writes on every reconnect
#endif
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
    time_apply_tz(g_gmt_offset_sec);   // pin the TZ string: the timezone is RAM-only and dies every reboot
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
    lv_obj_set_style_bg_color(dot, lv_color_hex(theme_primary()), 0);
    lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(dot, 0, 0);
    lv_obj_set_style_shadow_color(dot, lv_color_hex(theme_primary()), 0);
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
    lv_obj_set_style_text_color(sub, lv_color_hex(theme_primary()), 0);
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
    lv_obj_set_style_bg_color(splash_bar, lv_color_hex(theme_primary()), LV_PART_INDICATOR);
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
#define SCREEN_SETTINGS 2
#define SCREEN_BATTERY  3
#define NUM_SCREENS     4

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

// v6.4: watch-face wallpaper cycling by tap / long-press has been REMOVED.
// The Settings screen picker is now the only way to change the wallpaper.
// save_wp_idx() is still defined here because the Settings picker calls it.
static void save_wp_idx() {
    g_prefs.begin("dcw", false);
    g_prefs.putInt("wp_idx", g_photo_idx);
    g_prefs.end();
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

    // v6.4: tap-zone REMOVED -- watch-face taps no longer change the
    // wallpaper.  Use the Settings screen picker (swipe left twice from
    // the face) to switch wallpapers.  Swipes across the face still
    // work because they're handled at the indev level, not here.

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

    // v1.0-XANDER: subtle discovery hint -- swipe up from the face to reveal
    // the hidden Xander layer.  Kept dim so it never competes with the clock.
    lv_obj_t *xhint = lv_label_create(s);
    lv_label_set_text(xhint, LV_SYMBOL_UP "  Xander");
    lv_obj_set_style_text_color(xhint, lv_color_hex(theme_primary()), 0);
    lv_obj_set_style_text_opa(xhint, 120, 0);
    lv_obj_set_style_text_font(xhint, &lv_font_montserrat_12, 0);
    lv_obj_align(xhint, LV_ALIGN_BOTTOM_MID, 0, -10);

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
struct AppEntry { String name, path; size_t size; String category; };
#define MAX_APPS 32
static AppEntry  g_apps[MAX_APPS];
static int       g_app_count = 0;
static lv_obj_t *g_list_widget = nullptr;
static bool      load_app_from_sd(const AppEntry &app);
static void      populate_app_list();

// ============================================================================
//  v6.4 category map  --  /WATCH/bin/categories.txt
//  ----------------------------------------------------------------------------
//  Format (case-insensitive on the key, no .bin extension):
//      AppName = Category
//  Lines starting with '#' or blank lines are ignored.  Unmapped apps
//  fall into the "Other" bucket.  If the file is missing altogether we
//  behave exactly like v6.3 (flat favourites-first list).
// ============================================================================
#define MAX_CAT_ENTRIES 48
struct CatEntry { String name; String category; };
static CatEntry g_cat[MAX_CAT_ENTRIES];
static int      g_cat_count = 0;
static bool     g_cat_file_present = false;

static String cat_trim(const String &s) {
    int a = 0, b = s.length();
    while (a < b && (s[a] == ' ' || s[a] == '\t' || s[a] == '\r')) a++;
    while (b > a && (s[b-1] == ' ' || s[b-1] == '\t' || s[b-1] == '\r')) b--;
    return s.substring(a, b);
}
static String cat_lookup(const String &appname) {
    for (int i = 0; i < g_cat_count; i++) {
        if (g_cat[i].name.equalsIgnoreCase(appname)) return g_cat[i].category;
    }
    return String("Other");
}
static void load_categories_txt() {
    g_cat_count = 0;
    g_cat_file_present = false;
    if (!SD_MMC.exists("/WATCH/bin/categories.txt")) {
        USBSerial.println("[cat] categories.txt not present -- flat list");
        return;
    }
    File f = SD_MMC.open("/WATCH/bin/categories.txt", FILE_READ);
    if (!f) return;
    g_cat_file_present = true;
    String line;
    while (f.available() && g_cat_count < MAX_CAT_ENTRIES) {
        line = f.readStringUntil('\n');
        line = cat_trim(line);
        if (line.length() == 0 || line[0] == '#') continue;
        int eq = line.indexOf('=');
        if (eq <= 0) continue;
        String nm = cat_trim(line.substring(0, eq));
        String cat = cat_trim(line.substring(eq + 1));
        if (!nm.length() || !cat.length()) continue;
        g_cat[g_cat_count].name     = nm;
        g_cat[g_cat_count].category = cat;
        g_cat_count++;
    }
    f.close();
    USBSerial.printf("[cat] parsed %d category entries\n", g_cat_count);
}

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
    lv_obj_set_style_text_color(eyebrow, lv_color_hex(theme_primary()), 0);
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
    lv_obj_set_style_text_color(copy_pct, lv_color_hex(theme_primary()), 0);
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
    lv_obj_set_style_bg_color(copy_bar, lv_color_hex(theme_primary()), LV_PART_INDICATOR);
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
    lv_obj_set_style_border_color(copy_err_btn, lv_color_hex(theme_danger()), 0);
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

    // v7.0: 3D-carousel-style loader ring.  Subtle rotation only (~1 turn
    // per 6 s).  If the heap is tight we skip it and keep the plain bar.
    loader_ring_start(copy_screen);
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
    loader_ring_stop();
    lv_label_set_text(copy_title, "Couldn't load app");
    lv_obj_set_style_text_color(copy_title, lv_color_hex(theme_danger()), 0);
    lv_label_set_text(copy_sub, m.c_str());
    lv_obj_set_style_text_color(copy_sub, lv_color_hex(0xFF8080), 0);
    lv_label_set_text(copy_pct, "ERR");
    lv_obj_set_style_text_color(copy_pct, lv_color_hex(theme_danger()), 0);
    lv_label_set_text(copy_eta, "tap Back to return to the app list");
    lv_obj_set_style_bg_color(copy_bar, lv_color_hex(theme_danger()), LV_PART_INDICATOR);
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
    time_save_now("launching app");
    loader_ring_stop();
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
    lv_obj_set_style_border_color(card, lv_color_hex(theme_primary()), 0);
    lv_obj_set_style_border_width(card, 1, 0);
    lv_obj_set_style_radius(card, 22, 0);
    lv_obj_set_style_pad_all(card, 20, 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *eyebrow = lv_label_create(card);
    lv_label_set_text(eyebrow, "LAUNCH APP");
    lv_obj_set_style_text_color(eyebrow, lv_color_hex(theme_primary()), 0);
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
    lv_obj_set_style_bg_color(launch, lv_color_hex(theme_primary()), 0);
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

// ============================================================================
//  v6.3 APP LIST  --  scrollable vertical pills, favourites first,
//  thin divider between favs and the rest, gold border + gold name on
//  favourites (golden favourite styling preserved 1:1 from v6.2).
//  Each row is a capsule: full-width, ~56 px tall, big radius so the
//  ends look properly rounded -- reads as a "pill" rather than a card.
// ============================================================================
//
// Row geometry: panel is LCD_WIDTH - 30 = 380 px wide, pad_all 6 each
// side -> 368 inner.  Pills span the inner width; height 56 px gives
// a comfortable touch target (well over the 48 px Material minimum)
// without crowding -- 5 pills visible at once, the rest scroll.
#define APP_PILL_HEIGHT     56
#define APP_PILL_RADIUS     28        // h/2 -> true capsule
#define APP_PILL_GAP        10

// Full-width divider row that visually separates the favourites block
// from the rest of the apps in the vertical pill list.
static void add_grid_divider(lv_obj_t *parent) {
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, LV_PCT(100), 14);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *line = lv_obj_create(row);
    lv_obj_remove_style_all(line);
    lv_obj_set_size(line, LV_PCT(70), 1);
    lv_obj_align(line, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(line, lv_color_hex(0x2A2A2A), 0);
    lv_obj_set_style_bg_opa(line, LV_OPA_COVER, 0);
}

// One pill row.  Favourites get a gold border + gold name text on a
// slightly warmer background, otherwise it's a cyan-tinted border on
// near-black.  Star badge on the right for favourites, first-letter
// glyph in a circle on the left as a quick visual anchor.
static void add_app_tile(lv_obj_t *parent, int idx, bool isFav) {
    lv_obj_t *pill = lv_obj_create(parent);
    lv_obj_remove_style_all(pill);
    lv_obj_set_size(pill, LV_PCT(100), APP_PILL_HEIGHT);
    lv_obj_set_style_bg_color(pill, lv_color_hex(isFav ? 0x1A1408 : 0x0E0E0E), 0);
    lv_obj_set_style_bg_opa(pill, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(pill, APP_PILL_RADIUS, 0);
    lv_obj_set_style_pad_left(pill, 14, 0);
    lv_obj_set_style_pad_right(pill, 16, 0);
    lv_obj_set_style_pad_top(pill, 0, 0);
    lv_obj_set_style_pad_bottom(pill, 0, 0);
    // Gold for favourites, soft cyan for the rest (1 px keeps the row
    // light visually; favs bumped to 2 px for the "starred" emphasis).
    lv_obj_set_style_border_color(pill, lv_color_hex(isFav ? 0xFFB300 : 0x1C1C1C), 0);
    lv_obj_set_style_border_width(pill, isFav ? 2 : 1, 0);
    lv_obj_clear_flag(pill, LV_OBJ_FLAG_SCROLLABLE);
    // Pressed-state: subtle warmer bg + faint zoom for tactile feedback.
    lv_obj_set_style_bg_color(pill, lv_color_hex(isFav ? 0x2A1E10 : 0x1A1A1A), LV_STATE_PRESSED);
    lv_obj_set_style_transform_zoom(pill, 250, LV_STATE_PRESSED);  // ~0.98x

    // Circular first-letter glyph on the left.  Favs tint gold, others cyan.
    char glyph[2] = { 0, 0 };
    if (!g_apps[idx].name.isEmpty()) {
        glyph[0] = g_apps[idx].name.charAt(0);
        if (glyph[0] >= 'a' && glyph[0] <= 'z') glyph[0] -= 32;
    } else {
        glyph[0] = '?';
    }
    lv_obj_t *circ = lv_obj_create(pill);
    lv_obj_remove_style_all(circ);
    lv_obj_set_size(circ, 36, 36);
    lv_obj_align(circ, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_set_style_radius(circ, 18, 0);
    lv_obj_set_style_bg_color(circ, lv_color_hex(isFav ? 0x2A1E08 : 0x141414), 0);
    lv_obj_set_style_bg_opa(circ, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(circ, lv_color_hex(isFav ? 0xFFB300 : 0x00E5FF), 0);
    lv_obj_set_style_border_width(circ, 1, 0);
    lv_obj_clear_flag(circ, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(circ, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_t *g = lv_label_create(circ);
    lv_label_set_text(g, glyph);
    lv_obj_set_style_text_color(g, lv_color_hex(isFav ? 0xFFD060 : 0x00E5FF), 0);
    lv_obj_set_style_text_font(g, &lv_font_montserrat_20, 0);
    lv_obj_center(g);

    // App name -- single line, ellipsised; gold for favourites else white.
    lv_obj_t *name = lv_label_create(pill);
    lv_label_set_text(name, g_apps[idx].name.c_str());
    lv_obj_set_style_text_color(name, lv_color_hex(isFav ? 0xFFD060 : 0xFFFFFF), 0);
    lv_obj_set_style_text_font(name, &lv_font_montserrat_18, 0);
    // Left side reserved for the 36 px circle + 14 px gap; right side
    // reserved for ~20 px star.  Keep label area inside that envelope so
    // long names ellipsise instead of overflowing the pill.
    lv_obj_set_width(name, LCD_WIDTH - 30 - 12 - 36 - 14 - 30);
    lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);
    lv_obj_align(name, LV_ALIGN_LEFT_MID, 36 + 14, 0);

    // Star marker on favourites, right-aligned inside the pill.
    if (isFav) {
        lv_obj_t *star = lv_label_create(pill);
        lv_label_set_text(star, LV_SYMBOL_OK);
        lv_obj_set_style_text_color(star, lv_color_hex(theme_accent()), 0);
        lv_obj_set_style_text_font(star, &lv_font_montserrat_16, 0);
        lv_obj_align(star, LV_ALIGN_RIGHT_MID, 0, 0);
    }

    lv_obj_add_flag(pill, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(pill, list_event_cb, LV_EVENT_CLICKED,      (void*)(intptr_t)idx);
    lv_obj_add_event_cb(pill, list_event_cb, LV_EVENT_LONG_PRESSED, (void*)(intptr_t)idx);
}

// ---- v6.4 category section header (bigger than a divider) -----------------
static void add_section_header(lv_obj_t *parent, const char *text, uint32_t color) {
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, LV_PCT(100), 28);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *lbl = lv_label_create(row);
    lv_label_set_text(lbl, text);
    lv_obj_set_style_text_color(lbl, lv_color_hex(color), 0);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_letter_space(lbl, 2, 0);
    lv_obj_align(lbl, LV_ALIGN_LEFT_MID, 8, 4);

    lv_obj_t *line = lv_obj_create(row);
    lv_obj_remove_style_all(line);
    lv_obj_set_size(line, LV_PCT(55), 1);
    lv_obj_align(line, LV_ALIGN_RIGHT_MID, -8, 4);
    lv_obj_set_style_bg_color(line, lv_color_hex(0x1E1E1E), 0);
    lv_obj_set_style_bg_opa(line, LV_OPA_COVER, 0);
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

    // ---- Favourites first (unchanged) --------------------------------------
    bool any_favs = false;
    for (int j = 0; j < g_app_count; j++) {
        if (favs_contains(g_apps[j].name)) { any_favs = true; break; }
    }
    if (any_favs) {
        add_section_header(g_list_widget, "FAVOURITES", 0xFFB300);
        for (int i = 0; i < g_fav_count; i++) {
            for (int j = 0; j < g_app_count; j++) {
                if (g_apps[j].name == g_favs[i]) {
                    add_app_tile(g_list_widget, j, true);
                    break;
                }
            }
        }
    }

    // ---- v6.3 fallback path: no categories.txt -> flat rest of the list ----
    if (!g_cat_file_present) {
        if (any_favs) add_grid_divider(g_list_widget);
        for (int j = 0; j < g_app_count; j++) {
            if (favs_contains(g_apps[j].name)) continue;
            add_app_tile(g_list_widget, j, false);
        }
        return;
    }

    // ---- v6.4 bucketed path -----------------------------------------------
    // Collect unique category names, alphabetically, keeping "Other" last.
    String cats[MAX_CAT_ENTRIES + 1];
    int    ncats = 0;
    for (int j = 0; j < g_app_count; j++) {
        if (favs_contains(g_apps[j].name)) continue;
        String c = cat_lookup(g_apps[j].name);
        bool dup = false;
        for (int k = 0; k < ncats; k++) if (cats[k] == c) { dup = true; break; }
        if (!dup) cats[ncats++] = c;
    }
    // simple alpha sort with "Other" forced to the end
    for (int a = 0; a < ncats - 1; a++) {
        for (int b = a + 1; b < ncats; b++) {
            bool aIsOther = cats[a].equalsIgnoreCase("Other");
            bool bIsOther = cats[b].equalsIgnoreCase("Other");
            bool swap = false;
            if (aIsOther && !bIsOther) swap = true;
            else if (!aIsOther && !bIsOther && cats[a].compareTo(cats[b]) > 0) swap = true;
            if (swap) { String t = cats[a]; cats[a] = cats[b]; cats[b] = t; }
        }
    }

    for (int c = 0; c < ncats; c++) {
        // Section header (uppercase for style parity with FAVOURITES)
        String head = cats[c]; head.toUpperCase();
        add_section_header(g_list_widget, head.c_str(), 0x00E5FF);
        // Emit apps in this category, alphabetically by name.
        int idx[MAX_APPS]; int n = 0;
        for (int j = 0; j < g_app_count; j++) {
            if (favs_contains(g_apps[j].name)) continue;
            if (cat_lookup(g_apps[j].name).equalsIgnoreCase(cats[c])) idx[n++] = j;
        }
        for (int a = 0; a < n - 1; a++) {
            for (int b = a + 1; b < n; b++) {
                if (g_apps[idx[a]].name.compareTo(g_apps[idx[b]].name) > 0) {
                    int t = idx[a]; idx[a] = idx[b]; idx[b] = t;
                }
            }
        }
        for (int k = 0; k < n; k++) add_app_tile(g_list_widget, idx[k], false);
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
    lv_obj_set_style_border_color(chip, lv_color_hex(theme_primary()), 0);
    lv_obj_set_style_border_width(chip, 1, 0);
    lv_obj_clear_flag(chip, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *chip_l = lv_label_create(chip);
    char cb[24]; snprintf(cb, sizeof(cb), "%d apps", g_app_count);
    lv_label_set_text(chip_l, cb);
    lv_obj_set_style_text_color(chip_l, lv_color_hex(theme_primary()), 0);
    lv_obj_set_style_text_font(chip_l, &lv_font_montserrat_14, 0);
    lv_obj_center(chip_l);

    // Subtitle
    lv_obj_t *h2 = lv_label_create(s);
    lv_label_set_text(h2, "/WATCH/bin  ·  long-press to star");
    lv_obj_set_style_text_color(h2, lv_color_hex(0x666666), 0);
    lv_obj_set_style_text_font(h2, &lv_font_montserrat_12, 0);
    lv_obj_align(h2, LV_ALIGN_TOP_LEFT, 24, 58);

    // Scrollable single-column pill list (v6.3).
    // flex flow column + wrap-disabled so each pill is its own row;
    // pad_row = APP_PILL_GAP gives a comfortable vertical rhythm.
    // The favourites/divider/rest sequence still works the same way --
    // populate_app_list() emits favs, an LV_PCT(100) divider, then the
    // rest, and the column layout naturally stacks them top-to-bottom.
    g_list_widget = lv_obj_create(s);
    lv_obj_set_size(g_list_widget, LCD_WIDTH - 30, LCD_HEIGHT - 130);
    lv_obj_align(g_list_widget, LV_ALIGN_TOP_MID, 0, 84);
    lv_obj_set_style_bg_opa(g_list_widget, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(g_list_widget, 0, 0);
    lv_obj_set_style_pad_all(g_list_widget, 6, 0);
    lv_obj_set_style_pad_row(g_list_widget, APP_PILL_GAP, 0);
    lv_obj_set_style_pad_column(g_list_widget, 0, 0);
    lv_obj_set_flex_flow(g_list_widget, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(g_list_widget,
                          LV_FLEX_ALIGN_START,     // main axis (column top)
                          LV_FLEX_ALIGN_CENTER,    // cross axis (horizontal centre)
                          LV_FLEX_ALIGN_START);    // tracks
    lv_obj_set_scroll_dir(g_list_widget, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(g_list_widget, LV_SCROLLBAR_MODE_AUTO);
    // Subtle scrollbar styling
    lv_obj_set_style_bg_color(g_list_widget, lv_color_hex(theme_primary()), LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_opa(g_list_widget, 120, LV_PART_SCROLLBAR);
    lv_obj_set_style_radius(g_list_widget, 2, LV_PART_SCROLLBAR);
    lv_obj_set_style_width(g_list_widget, 3, LV_PART_SCROLLBAR);

    // v7.0: dispatch on active launcher style.  PILL preserves the base
    // categorised list (with Favourites + categories.txt bucketing).
    // COVERFLOW and GRID render a flat list of .bins.
    switch (g_launcher_style) {
        case LS_COVERFLOW:
            // Coverflow needs freeform positioning -- disable flex layout
            // and any scroll interception so absolute pos() calls stick.
            // LVGL 8.4 has no LV_LAYOUT_NONE enum; 0 == "no layout" and
            // clears the flex layout set on g_list_widget above so the
            // coverflow cards can be absolutely positioned by cf_layout().
            lv_obj_set_layout(g_list_widget, 0);
            lv_obj_clear_flag(g_list_widget, LV_OBJ_FLAG_SCROLLABLE);
            lv_obj_set_scrollbar_mode(g_list_widget, LV_SCROLLBAR_MODE_OFF);
            populate_apps_coverflow(g_list_widget);
            break;
        case LS_GRID:
            lv_obj_set_flex_flow(g_list_widget, LV_FLEX_FLOW_ROW_WRAP);
            lv_obj_set_style_pad_column(g_list_widget, APP_PILL_GAP, 0);
            lv_obj_set_flex_align(g_list_widget,
                                  LV_FLEX_ALIGN_CENTER,
                                  LV_FLEX_ALIGN_START,
                                  LV_FLEX_ALIGN_START);
            populate_apps_grid(g_list_widget);
            break;
        case LS_PILL:
        default:
            populate_app_list();
            break;
    }

    // Footer hint
    lv_obj_t *foot = lv_label_create(s);
    lv_label_set_text(foot, LV_SYMBOL_LEFT "  face   ·   settings  " LV_SYMBOL_RIGHT);
    lv_obj_set_style_text_color(foot, lv_color_hex(0x555555), 0);
    lv_obj_set_style_text_font(foot, &lv_font_montserrat_12, 0);
    lv_obj_align(foot, LV_ALIGN_BOTTOM_MID, 0, -10);
}

// ============================================================================
//  v6.4  SETTINGS SCREEN  --  storage summary + wallpaper picker.
//  ----------------------------------------------------------------------------
//  Swipe order:  FACE  <->  APPS  <->  SETTINGS  <->  BATTERY.
//    * Top row: "Settings" title + "wallpaper + storage" subtitle.
//    * Two summary cards side by side:  bin (N apps)  and  PHOTOS (N wp).
//    * "now: <filename>" line so the current wallpaper is unambiguous.
//    * Scrollable 2-column grid of wallpaper tiles.  Tapping a tile
//      loads that wallpaper immediately, persists it via save_wp_idx(),
//      and re-renders the grid so the star badge follows the selection.
//    * Footer swipe hint.
//  The picker writes the exact same NVS key ("wp_idx") that the old
//  long-press-on-face path used, so any app that reads it stays in sync
//  automatically.  No new preferences namespace, no new API.
// ============================================================================
static lv_obj_t *settings_wp_grid = nullptr;
static void settings_rebuild_wp_grid();      // fwd

#define WP_TILE_W        168
#define WP_TILE_H        96
#define WP_TILE_RADIUS   14
#define WP_TILE_GAP      12

static void settings_wp_tile_cb(lv_event_t *e) {
    if (millis() - lastSwipeMs < CLICK_AFTER_SWIPE_MS) return;   // post-swipe guard
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    if (idx < 0 || idx >= g_photo_count) return;
    if (idx == g_photo_idx) return;   // no-op tap on current
    g_photo_idx = idx;
    refresh_wallpaper();
    save_wp_idx();
    // Rebuild picker so the gold border + star badge follow the choice.
    // Cheaper than diffing which tiles need re-styling.
    settings_rebuild_wp_grid();
}

// Extract "sunset" from "/WATCH/PHOTOS/sunset.bin"
static String wp_display_name(int idx) {
    if (idx < 0 || idx >= g_photo_count) return String("");
    String p = g_photos[idx];
    int slash = p.lastIndexOf('/');
    String n = (slash >= 0) ? p.substring(slash + 1) : p;
    int dot = n.lastIndexOf('.');
    if (dot > 0) n = n.substring(0, dot);
    return n;
}

static void add_wp_tile(lv_obj_t *parent, int idx) {
    bool isCur = (idx == g_photo_idx);
    lv_obj_t *tile = lv_obj_create(parent);
    lv_obj_remove_style_all(tile);
    lv_obj_set_size(tile, WP_TILE_W, WP_TILE_H);
    lv_obj_set_style_bg_color(tile, lv_color_hex(isCur ? 0x1A1408 : 0x0E0E0E), 0);
    lv_obj_set_style_bg_opa(tile, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(tile, WP_TILE_RADIUS, 0);
    lv_obj_set_style_border_color(tile, lv_color_hex(isCur ? 0xFFB300 : 0x1C1C1C), 0);
    lv_obj_set_style_border_width(tile, isCur ? 2 : 1, 0);
    lv_obj_set_style_pad_all(tile, 10, 0);
    lv_obj_clear_flag(tile, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(tile, lv_color_hex(isCur ? 0x2A1E10 : 0x1A1A1A), LV_STATE_PRESSED);
    lv_obj_set_style_transform_zoom(tile, 250, LV_STATE_PRESSED);

    // Left glyph -- picture symbol
    lv_obj_t *ic = lv_label_create(tile);
    lv_label_set_text(ic, LV_SYMBOL_IMAGE);
    lv_obj_set_style_text_color(ic, lv_color_hex(isCur ? 0xFFD060 : 0x00E5FF), 0);
    lv_obj_set_style_text_font(ic, &lv_font_montserrat_22, 0);
    lv_obj_align(ic, LV_ALIGN_TOP_LEFT, 0, 0);

    // Name label
    lv_obj_t *nm = lv_label_create(tile);
    lv_label_set_text(nm, wp_display_name(idx).c_str());
    lv_obj_set_style_text_color(nm, lv_color_hex(isCur ? 0xFFD060 : 0xFFFFFF), 0);
    lv_obj_set_style_text_font(nm, &lv_font_montserrat_16, 0);
    lv_obj_set_width(nm, WP_TILE_W - 24);
    lv_label_set_long_mode(nm, LV_LABEL_LONG_DOT);
    lv_obj_align(nm, LV_ALIGN_BOTTOM_LEFT, 0, 0);

    if (isCur) {
        lv_obj_t *star = lv_label_create(tile);
        lv_label_set_text(star, LV_SYMBOL_OK);
        lv_obj_set_style_text_color(star, lv_color_hex(theme_accent()), 0);
        lv_obj_set_style_text_font(star, &lv_font_montserrat_16, 0);
        lv_obj_align(star, LV_ALIGN_TOP_RIGHT, 0, 0);
    }

    lv_obj_add_flag(tile, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(tile, settings_wp_tile_cb, LV_EVENT_CLICKED,
                        (void*)(intptr_t)idx);
}

static void settings_rebuild_wp_grid() {
    if (!settings_wp_grid) return;
    lv_obj_clean(settings_wp_grid);

    if (!g_photo_count) {
        lv_obj_t *empty = lv_label_create(settings_wp_grid);
        lv_label_set_text(empty,
            "No wallpapers on card.\nDrop .bin files into /WATCH/PHOTOS/");
        lv_obj_set_style_text_align(empty, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_text_color(empty, lv_color_hex(0x666666), 0);
        lv_obj_set_style_text_font(empty, &lv_font_montserrat_14, 0);
        lv_obj_align(empty, LV_ALIGN_CENTER, 0, 0);
        return;
    }
    for (int i = 0; i < g_photo_count; i++) add_wp_tile(settings_wp_grid, i);
}

// ============================================================================
//  v7.0 -- CARD-BASED SETTINGS SCREEN (replaces v6.4 embedded picker).
//  Storage summary cards from v6.4 are kept at the top.  Below them
//  is a scrollable column of card rows.  The full-screen wallpaper
//  picker opens from the "Wallpaper" action card.
// ============================================================================
static void build_screen_settings() {
    lv_obj_t *s = lv_obj_create(NULL);
    screens[SCREEN_SETTINGS] = s;
    lv_obj_set_style_bg_color(s, lv_color_hex(theme_bg()), 0);
    lv_obj_set_style_bg_opa(s, LV_OPA_COVER, 0);
    lv_obj_clear_flag(s, LV_OBJ_FLAG_SCROLLABLE);

    // Title
    lv_obj_t *h1 = lv_label_create(s);
    lv_label_set_text(h1, "Settings");
    lv_obj_set_style_text_color(h1, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(h1, &lv_font_montserrat_28, 0);
    lv_obj_align(h1, LV_ALIGN_TOP_LEFT, 24, 22);

    lv_obj_t *h2 = lv_label_create(s);
    lv_label_set_text(h2, "theme · style · wallpaper · storage");
    lv_obj_set_style_text_color(h2, lv_color_hex(0x666666), 0);
    lv_obj_set_style_text_font(h2, &lv_font_montserrat_12, 0);
    lv_obj_align(h2, LV_ALIGN_TOP_LEFT, 24, 58);

    // ---- Storage summary cards (from v6.4, unchanged behaviour) -----------
    lv_obj_t *row = lv_obj_create(s);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, LCD_WIDTH - 40, 64);
    lv_obj_align(row, LV_ALIGN_TOP_MID, 0, 82);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(row, 10, 0);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_CLICKABLE);

    auto make_summary_card = [](lv_obj_t *parent, const char *sym,
                                const char *primary, const char *secondary,
                                uint32_t accent) {
        lv_obj_t *card = lv_obj_create(parent);
        lv_obj_remove_style_all(card);
        lv_obj_set_size(card, LV_PCT(48), LV_PCT(100));
        lv_obj_set_style_bg_color(card, lv_color_hex(theme_fg()), 0);
        lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(card, 14, 0);
        lv_obj_set_style_border_color(card, lv_color_hex(accent), 0);
        lv_obj_set_style_border_width(card, 1, 0);
        lv_obj_set_style_pad_all(card, 10, 0);
        lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_clear_flag(card, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_t *ic = lv_label_create(card);
        lv_label_set_text(ic, sym);
        lv_obj_set_style_text_color(ic, lv_color_hex(accent), 0);
        lv_obj_set_style_text_font(ic, &lv_font_montserrat_20, 0);
        lv_obj_align(ic, LV_ALIGN_TOP_LEFT, 0, 0);
        lv_obj_t *p = lv_label_create(card);
        lv_label_set_text(p, primary);
        lv_obj_set_style_text_color(p, lv_color_hex(0xFFFFFF), 0);
        lv_obj_set_style_text_font(p, &lv_font_montserrat_16, 0);
        lv_obj_align(p, LV_ALIGN_TOP_RIGHT, 0, 0);
        lv_obj_t *sub = lv_label_create(card);
        lv_label_set_text(sub, secondary);
        lv_obj_set_style_text_color(sub, lv_color_hex(0x888888), 0);
        lv_obj_set_style_text_font(sub, &lv_font_montserrat_12, 0);
        lv_obj_align(sub, LV_ALIGN_BOTTOM_LEFT, 0, 0);
    };
    char binPrim[16], phPrim[16];
    snprintf(binPrim, sizeof(binPrim), "%d", g_app_count);
    snprintf(phPrim,  sizeof(phPrim),  "%d", g_photo_count);
    make_summary_card(row, LV_SYMBOL_LIST,  binPrim, "apps",       theme_primary());
    make_summary_card(row, LV_SYMBOL_IMAGE, phPrim,  "wallpapers", theme_accent());

    // ---- Scrollable card list ---------------------------------------------
    lv_obj_t *list = lv_obj_create(s);
    lv_obj_set_size(list, LCD_WIDTH - 30, LCD_HEIGHT - 170);
    lv_obj_align(list, LV_ALIGN_TOP_MID, 0, 158);
    lv_obj_set_style_bg_opa(list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(list, 0, 0);
    lv_obj_set_style_pad_all(list, 6, 0);
    lv_obj_set_style_pad_row(list, 10, 0);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(list, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START);
    lv_obj_set_scroll_dir(list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(list, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_set_style_bg_color(list, lv_color_hex(theme_accent()), LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_opa(list, 120, LV_PART_SCROLLBAR);
    lv_obj_set_style_width(list, 3, LV_PART_SCROLLBAR);

    // 1) Brightness slider
    card_slider(list, "Brightness", "AMOLED backlight (20-255)",
                20, 255, g_cur_brightness, cards_on_brightness_cb);

    // 2) Theme dropdown
    {
        static String theme_opts;
        theme_opts = "";
        for (int i = 0; i < theme_count(); i++) {
            if (i) theme_opts += "\n";
            theme_opts += theme_name_at(i);
        }
        card_dropdown(list, "Theme", "color palette",
                      theme_opts.c_str(), g_theme_idx, cards_on_theme_cb);
    }

    // 3) Launcher style dropdown
    card_dropdown(list, "Launcher Style", "how apps are shown",
                  "Pill list\n3D CoverFlow\nGrid",
                  (int)g_launcher_style, cards_on_style_cb);

    // 4) Quick-swap wallpaper dropdown (only when we have >0 wallpapers)
    if (g_photo_count > 0) {
        static String wp_opts;
        wp_opts = "";
        for (int i = 0; i < g_photo_count; i++) {
            if (i) wp_opts += "\n";
            wp_opts += wp_display_name(i);
        }
        card_dropdown(list, "Quick Wallpaper", "swap without opening picker",
                      wp_opts.c_str(), g_photo_idx, cards_on_quick_wp_cb);
    }

    // 5) Open Wallpaper Picker
    card_action(list, "Wallpaper", "browse all wallpapers",
                LV_SYMBOL_IMAGE "  Open", theme_accent(),
                cards_open_wallpaper_cb);

    // 5b) XANDER -- offline companion (open + setup)
    card_action(list, "Xander", "offline companion",
                LV_SYMBOL_EYE_OPEN "  Open", theme_primary(),
                xander_open_home_cb);
    card_action(list, "Xander Setup", "voice / always-on / stats",
                LV_SYMBOL_SETTINGS "  Setup", theme_accent(),
                xander_open_settings_cb);

    // 6) Toggle: WiFi
    card_toggle(list, "WiFi", "12 h NTP resync when on",
                g_wifi_toggle, cards_on_wifi_cb);
    // 7) Toggle: BLE
    card_toggle(list, "BLE", "off saves ~15 mA",
                g_ble_toggle, cards_on_ble_cb);
    // 8) Toggle: Haptics
    card_toggle(list, "Haptics", "vibrate on tap (if fitted)",
                g_haptics_toggle, cards_on_haptics_cb);

    // 9) Reconnect WiFi action
    card_action(list, "Reconnect WiFi", "walk saved networks",
                LV_SYMBOL_WIFI "  Run", theme_primary(),
                cards_reconnect_wifi_cb);

    // 10) Developer info subview
    card_action(list, "Developer Info", "heap / build / counts",
                LV_SYMBOL_EYE_OPEN "  View", theme_primary(),
                cards_open_developer_cb);

    // 11) Deep sleep
    card_action(list, "Deep Sleep", "press BOOT to wake",
                LV_SYMBOL_POWER "  Sleep", theme_primary(),
                cards_deep_sleep_cb);

    // 12) Factory reset
    card_action(list, "Factory Reset", "wipes NVS + reboots",
                LV_SYMBOL_TRASH "  Reset", theme_danger(),
                cards_factory_reset_cb);

    // Footer swipe hint
    lv_obj_t *foot = lv_label_create(s);
    lv_label_set_text(foot, LV_SYMBOL_LEFT "  apps   ·   battery  " LV_SYMBOL_RIGHT);
    lv_obj_set_style_text_color(foot, lv_color_hex(0x555555), 0);
    lv_obj_set_style_text_font(foot, &lv_font_montserrat_12, 0);
    lv_obj_align(foot, LV_ALIGN_BOTTOM_MID, 0, -6);
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

    lv_color_t col = pct<=15 ? lv_color_hex(theme_danger())
                   : pct<=30 ? lv_color_hex(theme_accent())
                             : lv_color_hex(0x00FF80);
    lv_obj_set_style_arc_color(batt_arc, col, LV_PART_INDICATOR);
    lv_obj_set_style_text_color(batt_pct_lbl, col, 0);

    if (g_charging) {
        lv_label_set_text(batt_state, LV_SYMBOL_CHARGE "  charging");
        lv_obj_set_style_text_color(batt_state, lv_color_hex(theme_primary()), 0);
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
        time_apply_tz(g_gmt_offset_sec);   // pin the TZ string: the timezone is RAM-only and dies every reboot
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
    lv_obj_set_style_text_color(sub, lv_color_hex(theme_primary()), 0);
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
    lv_obj_set_style_border_color(btn, lv_color_hex(theme_primary()), 0);
    lv_obj_set_style_border_width(btn, 1, 0);
    lv_obj_add_event_cb(btn,
        [](lv_event_t *){
            if (millis() - lastSwipeMs < CLICK_AFTER_SWIPE_MS) return;
            deep_sleep_now();
        },
        LV_EVENT_CLICKED, NULL);
    lv_obj_t *bl = lv_label_create(btn);
    lv_label_set_text(bl, LV_SYMBOL_POWER "  Sleep Now");
    lv_obj_set_style_text_color(bl, lv_color_hex(theme_primary()), 0);
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
    // v6.2: BEFORE we repaint + fade up, force the visible screen to
    // pull the CURRENT wall-clock value off the RTC.  The 1-second tick
    // in loop() only runs once a second, so without this push the user
    // sees the stale pre-sleep frame for up to a full second after wake
    // (which is the "time isn't being updated after sleep" report).
    if (current_screen == SCREEN_FACE) {
        update_face_time();
        update_face_battery();
    } else if (current_screen == SCREEN_BATTERY) {
        update_battery_screen();
    }
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
//  v7.0 ADDITIONS  --  persistence, screen rebuild, coverflow/grid apps,
//                      card-based Settings, factory reset, dev info,
//                      loader carousel ring.
// ============================================================================

// ---- Theme + launcher-style NVS persistence -------------------------------
static void theme_load_from_nvs() {
    Preferences p;
    p.begin("wctheme", true);
    int idx = p.getInt("idx", 0);
    p.end();
    if (idx < 0 || idx >= kThemeCount) idx = 0;
    g_theme_idx = idx;
    USBSerial.printf("[theme] loaded idx=%d (%s)\n", idx, theme_name_at(idx));
}
static void theme_save(int idx) {
    if (idx < 0 || idx >= kThemeCount) return;
    g_theme_idx = idx;
    Preferences p;
    p.begin("wctheme", false);
    p.putInt("idx", idx);
    p.end();
}
static void launcher_style_load_from_nvs() {
    g_prefs.begin("dcw", true);
    uint32_t v = g_prefs.getUInt("lstyle", LS_PILL);
    g_prefs.end();
    if (v > LS_GRID) v = LS_PILL;
    g_launcher_style = (LauncherStyle)v;
    USBSerial.printf("[style] loaded=%u\n", (unsigned)v);
}
static void launcher_style_save() {
    g_prefs.begin("dcw", false);
    g_prefs.putUInt("lstyle", (uint32_t)g_launcher_style);
    g_prefs.end();
}

// ---- Screen rebuild -------------------------------------------------------
// Called when the theme or launcher style changes.  We tear down and re-
// build every screen so the new palette / layout takes effect immediately.
// The FACE background is re-loaded from the current wallpaper.
static void build_screen_face();
static void build_screen_apps();
static void build_screen_settings();
static void build_screen_battery();

static void rebuild_all_screens_and_reload(int keep_current) {
    for (int i = 0; i < NUM_SCREENS; i++) {
        if (screens[i]) { lv_obj_del(screens[i]); screens[i] = nullptr; }
    }
    build_screen_face();
    build_screen_apps();
    build_screen_settings();
    build_screen_battery();
    // Restore the currently-visible screen without an animation blip.
    if (keep_current < 0 || keep_current >= NUM_SCREENS) keep_current = SCREEN_FACE;
    current_screen = keep_current;
    lv_scr_load_anim(screens[keep_current], LV_SCR_LOAD_ANIM_FADE_ON,
                     SCREEN_SWITCH_ANIM_MS, 0, false);
}

// ============================================================================
//  v7.0 -- COVERFLOW + GRID launcher styles (ported from LauncherOS1.ino).
//  These render into the same g_list_widget slot that populate_app_list()
//  uses for PILL style, so the swipe-guard + click-guard flow doesn't
//  change.
// ============================================================================
struct WcCarCard {
    lv_obj_t *root;
    int       app_idx;
};
#define COVERFLOW_MAX 24
static WcCarCard g_cf_cards[COVERFLOW_MAX];
static int       g_cf_count = 0;
static int       g_cf_index = 0;
static lv_obj_t *g_cf_root  = nullptr;

static void cf_card_tap_cb(lv_event_t *e) {
    if (millis() - lastSwipeMs < CLICK_AFTER_SWIPE_MS) return;
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    if (idx >= 0 && idx < g_app_count) show_launch_confirm(idx);
}

static void cf_anim_x(void *v, int32_t x)    { lv_obj_set_x((lv_obj_t*)v, x); }
static void cf_anim_w(void *v, int32_t w)    { lv_obj_set_width((lv_obj_t*)v, w); }
static void cf_anim_h(void *v, int32_t h)    { lv_obj_set_height((lv_obj_t*)v, h); }
static void cf_anim_opa(void *v, int32_t o)  { lv_obj_set_style_opa((lv_obj_t*)v, o, 0); }

static void cf_animate(lv_obj_t *o, lv_anim_exec_xcb_t fn,
                       int32_t from, int32_t to, uint32_t dur) {
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, o);
    lv_anim_set_exec_cb(&a, fn);
    lv_anim_set_values(&a, from, to);
    lv_anim_set_time(&a, dur);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_start(&a);
}

static void cf_layout(bool animate) {
    if (!g_cf_count) return;
    const int kActiveW  = 240;
    const int kActiveH  = 320;
    const int kSideOffX = 210;
    const int kSideDiv  = 256;   // scale factor numerator: kSideScale/256
    const int kSideScale = 200;
    const int kSideOpa  = 130;
    const uint32_t dur  = animate ? 220 : 0;
    const int list_w    = LCD_WIDTH - 30;

    for (int i = 0; i < g_cf_count; i++) {
        lv_obj_t *card = g_cf_cards[i].root;
        if (!card) continue;
        int d = i - g_cf_index;
        while (d >  g_cf_count / 2) d -= g_cf_count;
        while (d < -g_cf_count / 2) d += g_cf_count;
        if (d == 0) {
            cf_animate(card, cf_anim_x, lv_obj_get_x(card), (list_w - kActiveW) / 2, dur);
            cf_animate(card, cf_anim_w, lv_obj_get_width(card), kActiveW, dur);
            cf_animate(card, cf_anim_h, lv_obj_get_height(card), kActiveH, dur);
            cf_animate(card, cf_anim_opa, lv_obj_get_style_opa(card, LV_PART_MAIN), LV_OPA_COVER, dur);
            lv_obj_move_foreground(card);
        } else if (d == 1 || d == -1) {
            int scaled_w = (kActiveW * kSideScale) / kSideDiv;
            int scaled_h = (kActiveH * kSideScale) / kSideDiv;
            int x = (list_w - scaled_w) / 2 + (d > 0 ? kSideOffX : -kSideOffX);
            cf_animate(card, cf_anim_x, lv_obj_get_x(card), x, dur);
            cf_animate(card, cf_anim_w, lv_obj_get_width(card), scaled_w, dur);
            cf_animate(card, cf_anim_h, lv_obj_get_height(card), scaled_h, dur);
            cf_animate(card, cf_anim_opa, lv_obj_get_style_opa(card, LV_PART_MAIN), kSideOpa, dur);
        } else {
            cf_animate(card, cf_anim_x, lv_obj_get_x(card),
                       d > 0 ? list_w + 40 : -list_w - 40, dur);
            cf_animate(card, cf_anim_opa, lv_obj_get_style_opa(card, LV_PART_MAIN), LV_OPA_TRANSP, dur);
        }
    }
}

static void cf_goto(int i, bool animate) {
    if (!g_cf_count) return;
    while (i < 0)             i += g_cf_count;
    while (i >= g_cf_count)  i -= g_cf_count;
    if (i == g_cf_index) return;
    g_cf_index = i;
    cf_layout(animate);
}

static void populate_apps_coverflow(lv_obj_t *parent) {
    g_cf_count = 0;
    g_cf_index = 0;
    g_cf_root  = parent;

    if (!g_app_count) {
        lv_obj_t *lbl = lv_label_create(parent);
        lv_label_set_text(lbl, "No apps.\nDrop .bin files into /WATCH/bin/");
        lv_obj_set_style_text_align(lbl, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_text_color(lbl, lv_color_hex(0x666666), 0);
        lv_obj_set_style_text_font(lbl, &lv_font_montserrat_14, 0);
        lv_obj_align(lbl, LV_ALIGN_CENTER, 0, 0);
        return;
    }

    for (int i = 0; i < g_app_count && i < COVERFLOW_MAX; i++) {
        lv_obj_t *card = lv_obj_create(parent);
        lv_obj_remove_style_all(card);
        lv_obj_set_size(card, 240, 320);
        lv_obj_align(card, LV_ALIGN_CENTER, 0, 0);
        lv_obj_set_style_bg_color(card, lv_color_hex(theme_fg()), 0);
        lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
        lv_obj_set_style_border_color(card, lv_color_hex(theme_accent()), 0);
        lv_obj_set_style_border_width(card, 2, 0);
        lv_obj_set_style_radius(card, 18, 0);
        lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

        // Big first-letter glyph
        char gl[2] = { 0, 0 };
        if (g_apps[i].name.length()) {
            gl[0] = g_apps[i].name.charAt(0);
            if (gl[0] >= 'a' && gl[0] <= 'z') gl[0] -= 32;
        } else gl[0] = '?';
        lv_obj_t *glyph = lv_label_create(card);
        lv_label_set_text(glyph, gl);
        lv_obj_set_style_text_color(glyph, lv_color_hex(theme_primary()), 0);
        lv_obj_set_style_text_font(glyph, &lv_font_montserrat_48, 0);
        lv_obj_align(glyph, LV_ALIGN_CENTER, 0, -30);

        lv_obj_t *name = lv_label_create(card);
        lv_label_set_text(name, g_apps[i].name.c_str());
        lv_obj_set_style_text_color(name, lv_color_hex(theme_primary()), 0);
        lv_obj_set_style_text_font(name, &lv_font_montserrat_20, 0);
        lv_obj_set_style_text_align(name, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_width(name, 220);
        lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);
        lv_obj_align(name, LV_ALIGN_BOTTOM_MID, 0, -30);

        lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(card, cf_card_tap_cb, LV_EVENT_CLICKED,
                            (void *)(intptr_t)i);
        g_cf_cards[g_cf_count].root    = card;
        g_cf_cards[g_cf_count].app_idx = i;
        g_cf_count++;
    }
    cf_layout(false);
}

static void populate_apps_grid(lv_obj_t *parent) {
    if (!g_app_count) {
        lv_obj_t *lbl = lv_label_create(parent);
        lv_label_set_text(lbl, "No apps.\nDrop .bin files into /WATCH/bin/");
        lv_obj_set_style_text_align(lbl, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_text_color(lbl, lv_color_hex(0x666666), 0);
        lv_obj_set_style_text_font(lbl, &lv_font_montserrat_14, 0);
        lv_obj_align(lbl, LV_ALIGN_CENTER, 0, 0);
        return;
    }
    for (int i = 0; i < g_app_count; i++) {
        bool isFav = favs_contains(g_apps[i].name);
        lv_obj_t *tile = lv_obj_create(parent);
        lv_obj_remove_style_all(tile);
        lv_obj_set_size(tile, 108, 108);
        lv_obj_set_style_bg_color(tile,
            lv_color_hex(isFav ? theme_accent() : theme_fg()), 0);
        lv_obj_set_style_bg_opa(tile, isFav ? LV_OPA_30 : LV_OPA_COVER, 0);
        lv_obj_set_style_border_color(tile,
            lv_color_hex(isFav ? theme_accent() : theme_primary()), 0);
        lv_obj_set_style_border_width(tile, isFav ? 2 : 1, 0);
        lv_obj_set_style_radius(tile, 14, 0);
        lv_obj_set_style_pad_all(tile, 6, 0);
        lv_obj_clear_flag(tile, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_style_transform_zoom(tile, 250, LV_STATE_PRESSED);

        char gl[2] = { 0, 0 };
        if (g_apps[i].name.length()) {
            gl[0] = g_apps[i].name.charAt(0);
            if (gl[0] >= 'a' && gl[0] <= 'z') gl[0] -= 32;
        } else gl[0] = '?';
        lv_obj_t *glyph = lv_label_create(tile);
        lv_label_set_text(glyph, gl);
        lv_obj_set_style_text_color(glyph,
            lv_color_hex(isFav ? theme_accent() : theme_primary()), 0);
        lv_obj_set_style_text_font(glyph, &lv_font_montserrat_28, 0);
        lv_obj_align(glyph, LV_ALIGN_TOP_MID, 0, 6);

        lv_obj_t *name = lv_label_create(tile);
        lv_label_set_text(name, g_apps[i].name.c_str());
        lv_obj_set_style_text_color(name,
            lv_color_hex(isFav ? theme_accent() : 0xFFFFFF), 0);
        lv_obj_set_style_text_font(name, &lv_font_montserrat_12, 0);
        lv_obj_set_style_text_align(name, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_width(name, 96);
        lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);
        lv_obj_align(name, LV_ALIGN_BOTTOM_MID, 0, -4);

        lv_obj_add_flag(tile, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(tile, list_event_cb, LV_EVENT_CLICKED,
                            (void *)(intptr_t)i);
        lv_obj_add_event_cb(tile, list_event_cb, LV_EVENT_LONG_PRESSED,
                            (void *)(intptr_t)i);
    }
}

// ============================================================================
//  v7.0 -- 3D CAROUSEL LOADER RING
//  ----------------------------------------------------------------------------
//  A ring of card-shaped slices around the OTA progress %.  Subtle
//  rotation (per user pref).  If PSRAM/heap is tight the ring is skipped
//  and we fall back to the plain v6.4 progress bar.
// ============================================================================
#define LOADER_RING_COUNT   10
static lv_obj_t *g_loader_ring[LOADER_RING_COUNT] = { nullptr };
static bool      g_loader_ring_active = false;
static uint32_t  g_loader_ring_t0 = 0;
static lv_timer_t *g_loader_ring_timer = nullptr;

static void loader_ring_tick(lv_timer_t *) {
    if (!g_loader_ring_active) return;
    // Subtle rotation: full turn every ~6 s.
    uint32_t dt = millis() - g_loader_ring_t0;
    float base_deg = (dt * 360.0f) / 6000.0f;   // degrees
    const int cx = LCD_WIDTH / 2;
    const int cy = LCD_HEIGHT / 2;
    const int radius = 165;
    for (int i = 0; i < LOADER_RING_COUNT; i++) {
        if (!g_loader_ring[i]) continue;
        float ang = (base_deg + i * (360.0f / LOADER_RING_COUNT)) * (3.14159265f / 180.0f);
        int x = cx + (int)(cosf(ang) * radius) - 10;
        int y = cy + (int)(sinf(ang) * radius) - 24;
        lv_obj_set_pos(g_loader_ring[i], x, y);
        // Fade the slice closest to the "top" up brightest to give the
        // ring a sense of rotation without spinning too aggressively.
        float phase = (base_deg / 360.0f + (float)i / LOADER_RING_COUNT);
        while (phase > 1.0f) phase -= 1.0f;
        int opa = 90 + (int)(120 * (0.5f + 0.5f * sinf(phase * 6.283f)));
        if (opa > 255) opa = 255;
        lv_obj_set_style_bg_opa(g_loader_ring[i], (lv_opa_t)opa, 0);
    }
}

static bool loader_ring_start(lv_obj_t *scr) {
    // PSRAM/heap sanity check: each ring slice is a tiny LVGL obj, so
    // this is really only a safety net for extreme low-heap conditions.
    if (esp_get_free_heap_size() < 20000) return false;
    for (int i = 0; i < LOADER_RING_COUNT; i++) {
        lv_obj_t *slice = lv_obj_create(scr);
        lv_obj_remove_style_all(slice);
        lv_obj_set_size(slice, 20, 48);
        lv_obj_set_style_bg_color(slice,
            i == 0 ? lv_color_hex(theme_accent())
                   : lv_color_hex(theme_primary()), 0);
        lv_obj_set_style_bg_opa(slice, 120, 0);
        lv_obj_set_style_radius(slice, 6, 0);
        lv_obj_set_style_border_width(slice, 0, 0);
        lv_obj_clear_flag(slice, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_clear_flag(slice, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_move_background(slice);
        g_loader_ring[i] = slice;
    }
    g_loader_ring_active = true;
    g_loader_ring_t0     = millis();
    g_loader_ring_timer  = lv_timer_create(loader_ring_tick, 40, nullptr);
    loader_ring_tick(nullptr);
    return true;
}

static void loader_ring_stop() {
    if (g_loader_ring_timer) {
        lv_timer_del(g_loader_ring_timer);
        g_loader_ring_timer = nullptr;
    }
    for (int i = 0; i < LOADER_RING_COUNT; i++) g_loader_ring[i] = nullptr;
    g_loader_ring_active = false;
}

// ============================================================================
//  v7.0 -- FACTORY RESET modal
// ============================================================================
static lv_obj_t *g_factory_modal = nullptr;

static void factory_cancel_cb(lv_event_t *) {
    if (g_factory_modal) { lv_obj_del(g_factory_modal); g_factory_modal = nullptr; }
}
static void factory_confirm_cb(lv_event_t *) {
    if (g_factory_modal) { lv_obj_del(g_factory_modal); g_factory_modal = nullptr; }
    USBSerial.println("[factory] wiping NVS...");
    { Preferences p; p.begin("dcw",      false); p.clear(); p.end(); }
    { Preferences p; p.begin("wctheme",  false); p.clear(); p.end(); }
    { Preferences p; p.begin("appcache", false); p.clear(); p.end(); }
    { Preferences p; p.begin("rtcsave",  false); p.clear(); p.end(); }
    delay(120);
    esp_restart();
}

static void show_factory_reset_modal() {
    if (g_factory_modal) return;
    lv_obj_t *scr = lv_scr_act();
    g_factory_modal = lv_obj_create(scr);
    lv_obj_remove_style_all(g_factory_modal);
    lv_obj_set_size(g_factory_modal, LCD_WIDTH, LCD_HEIGHT);
    lv_obj_align(g_factory_modal, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_set_style_bg_color(g_factory_modal, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(g_factory_modal, 210, 0);
    lv_obj_clear_flag(g_factory_modal, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *card = lv_obj_create(g_factory_modal);
    lv_obj_set_size(card, LCD_WIDTH - 60, 280);
    lv_obj_align(card, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(card, lv_color_hex(theme_fg()), 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(card, lv_color_hex(theme_danger()), 0);
    lv_obj_set_style_border_width(card, 1, 0);
    lv_obj_set_style_radius(card, 22, 0);
    lv_obj_set_style_pad_all(card, 20, 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *eb = lv_label_create(card);
    lv_label_set_text(eb, "FACTORY RESET");
    lv_obj_set_style_text_color(eb, lv_color_hex(theme_danger()), 0);
    lv_obj_set_style_text_font(eb, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_letter_space(eb, 3, 0);
    lv_obj_align(eb, LV_ALIGN_TOP_MID, 0, 4);

    lv_obj_t *msg = lv_label_create(card);
    lv_label_set_text(msg,
        "This wipes theme, wallpaper index,\n"
        "favourites, app cache, and RTC\n"
        "snapshot from NVS, then reboots.");
    lv_obj_set_style_text_align(msg, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(msg, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(msg, &lv_font_montserrat_14, 0);
    lv_obj_set_width(msg, LCD_WIDTH - 120);
    lv_obj_align(msg, LV_ALIGN_TOP_MID, 0, 44);

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
    lv_obj_add_event_cb(cancel, factory_cancel_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *confirm = lv_btn_create(card);
    lv_obj_set_size(confirm, 130, 50);
    lv_obj_align(confirm, LV_ALIGN_BOTTOM_RIGHT, 0, 0);
    lv_obj_set_style_bg_color(confirm, lv_color_hex(theme_danger()), 0);
    lv_obj_set_style_border_width(confirm, 0, 0);
    lv_obj_set_style_radius(confirm, 25, 0);
    lv_obj_t *rl = lv_label_create(confirm);
    lv_label_set_text(rl, "Reset  " LV_SYMBOL_TRASH);
    lv_obj_set_style_text_color(rl, lv_color_hex(0x000000), 0);
    lv_obj_set_style_text_font(rl, &lv_font_montserrat_16, 0);
    lv_obj_center(rl);
    lv_obj_add_event_cb(confirm, factory_confirm_cb, LV_EVENT_CLICKED, NULL);
}

// ============================================================================
//  v7.0 -- DEVELOPER INFO subview  (own screen; back arrow to Settings)
// ============================================================================
static lv_obj_t *g_dev_screen = nullptr;

static void dev_back_cb(lv_event_t *) {
    if (millis() - lastSwipeMs < CLICK_AFTER_SWIPE_MS) return;
    lv_scr_load_anim(screens[SCREEN_SETTINGS], LV_SCR_LOAD_ANIM_FADE_ON,
                     SCREEN_SWITCH_ANIM_MS, 0, true);
    current_screen = SCREEN_SETTINGS;
    g_dev_screen = nullptr;
}

static void show_developer_info() {
    g_dev_screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(g_dev_screen, lv_color_hex(theme_bg()), 0);
    lv_obj_set_style_bg_opa(g_dev_screen, LV_OPA_COVER, 0);
    lv_obj_clear_flag(g_dev_screen, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *back = lv_label_create(g_dev_screen);
    lv_label_set_text(back, LV_SYMBOL_LEFT "  Settings");
    lv_obj_set_style_text_color(back, lv_color_hex(theme_primary()), 0);
    lv_obj_set_style_text_font(back, &lv_font_montserrat_18, 0);
    lv_obj_align(back, LV_ALIGN_TOP_LEFT, 20, 22);
    lv_obj_add_flag(back, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(back, dev_back_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *title = lv_label_create(g_dev_screen);
    lv_label_set_text(title, "Developer");
    lv_obj_set_style_text_color(title, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_28, 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 60);

    char buf[384];
    snprintf(buf, sizeof(buf),
        "Firmware:  LauncherOS v7.0\n"
        "Theme:     %s (#%d)\n"
        "Style:     %s\n\n"
        "Free heap:   %u KB\n"
        "Free PSRAM:  %u KB\n"
        "CPU freq:    %u MHz\n\n"
        "Apps found:      %d\n"
        "Wallpapers:      %d\n"
        "Favourites:      %d\n"
        "Categories file: %s",
        theme_name_at(g_theme_idx), g_theme_idx,
        g_launcher_style == LS_PILL      ? "Pill list" :
        g_launcher_style == LS_COVERFLOW ? "3D CoverFlow" : "Grid",
        (unsigned)(ESP.getFreeHeap() / 1024),
        (unsigned)(ESP.getFreePsram() / 1024),
        (unsigned)getCpuFrequencyMhz(),
        g_app_count, g_photo_count, g_fav_count,
        g_cat_file_present ? "yes" : "no");
    lv_obj_t *info = lv_label_create(g_dev_screen);
    lv_label_set_text(info, buf);
    lv_obj_set_style_text_color(info, lv_color_hex(0xBBBBBB), 0);
    lv_obj_set_style_text_font(info, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_line_space(info, 4, 0);
    lv_obj_align(info, LV_ALIGN_CENTER, 0, 30);

    lv_scr_load_anim(g_dev_screen, LV_SCR_LOAD_ANIM_FADE_ON,
                     SCREEN_SWITCH_ANIM_MS, 0, false);
}

// ============================================================================
//  v7.0 -- WALLPAPER PICKER subview (standalone screen; back arrow).
//  Reuses the v6.4 tile-grid code (settings_rebuild_wp_grid) but hosts
//  it on its own screen instead of embedded in Settings.
// ============================================================================
static lv_obj_t *g_wp_picker_screen = nullptr;

static void wp_picker_back_cb(lv_event_t *) {
    if (millis() - lastSwipeMs < CLICK_AFTER_SWIPE_MS) return;
    lv_scr_load_anim(screens[SCREEN_SETTINGS], LV_SCR_LOAD_ANIM_FADE_ON,
                     SCREEN_SWITCH_ANIM_MS, 0, true);
    current_screen = SCREEN_SETTINGS;
    g_wp_picker_screen = nullptr;
    settings_wp_grid   = nullptr;
}

static void show_wallpaper_picker() {
    g_wp_picker_screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(g_wp_picker_screen, lv_color_hex(theme_bg()), 0);
    lv_obj_set_style_bg_opa(g_wp_picker_screen, LV_OPA_COVER, 0);
    lv_obj_clear_flag(g_wp_picker_screen, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *back = lv_label_create(g_wp_picker_screen);
    lv_label_set_text(back, LV_SYMBOL_LEFT "  Settings");
    lv_obj_set_style_text_color(back, lv_color_hex(theme_primary()), 0);
    lv_obj_set_style_text_font(back, &lv_font_montserrat_18, 0);
    lv_obj_align(back, LV_ALIGN_TOP_LEFT, 20, 22);
    lv_obj_add_flag(back, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(back, wp_picker_back_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *title = lv_label_create(g_wp_picker_screen);
    lv_label_set_text(title, "Wallpaper");
    lv_obj_set_style_text_color(title, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_28, 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 60);

    settings_wp_grid = lv_obj_create(g_wp_picker_screen);
    lv_obj_set_size(settings_wp_grid, LCD_WIDTH - 30, LCD_HEIGHT - 120);
    lv_obj_align(settings_wp_grid, LV_ALIGN_TOP_MID, 0, 110);
    lv_obj_set_style_bg_opa(settings_wp_grid, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(settings_wp_grid, 0, 0);
    lv_obj_set_style_pad_all(settings_wp_grid, 6, 0);
    lv_obj_set_style_pad_row(settings_wp_grid, WP_TILE_GAP, 0);
    lv_obj_set_style_pad_column(settings_wp_grid, WP_TILE_GAP, 0);
    lv_obj_set_flex_flow(settings_wp_grid, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_align(settings_wp_grid,
                          LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START);
    lv_obj_set_scroll_dir(settings_wp_grid, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(settings_wp_grid, LV_SCROLLBAR_MODE_AUTO);

    settings_rebuild_wp_grid();

    lv_scr_load_anim(g_wp_picker_screen, LV_SCR_LOAD_ANIM_FADE_ON,
                     SCREEN_SWITCH_ANIM_MS, 0, false);
}

// ============================================================================
//  v7.0 -- CARD-BASED SETTINGS screen (replaces the v6.4 embedded picker).
//  Card rows: brightness slider, theme + launcher-style + quick-swap
//  wallpaper dropdowns, WiFi / BLE / Haptics toggles, action buttons for
//  Reconnect WiFi / Deep Sleep / Wallpaper Picker / Developer Info /
//  Factory Reset.  The v6.4 storage summary is kept at the top; the full
//  wallpaper picker opens as its own screen from a card action.
// ============================================================================
// State backing for toggle cards.  Wired to the real subsystems on change.
// (Actual definitions live near the top of the file alongside
// g_cur_brightness so build_screen_settings() -- which appears earlier in
// the translation unit -- can read them.)

// Deferred rebuild helpers -- lv_async_call'd from dropdown callbacks so
// we don't delete the screen (and the dropdown widget hosting the click)
// while its own event is still on the LVGL stack.
static void rebuild_async_settings(void *) {
    rebuild_all_screens_and_reload(SCREEN_SETTINGS);
}

static void cards_on_brightness_cb(lv_event_t *e) {
    if (millis() - lastSwipeMs < CLICK_AFTER_SWIPE_MS) return;
    int v = lv_slider_get_value(lv_event_get_target(e));
    if (v < 20)  v = 20;
    if (v > 255) v = 255;
    static_cast<Arduino_CO5300 *>(gfx)->setBrightness((uint8_t)v);
    g_cur_brightness = (uint8_t)v;
}
static void cards_on_theme_cb(lv_event_t *e) {
    if (millis() - lastSwipeMs < CLICK_AFTER_SWIPE_MS) return;
    int sel = lv_dropdown_get_selected(lv_event_get_target(e));
    if (sel == g_theme_idx) return;
    theme_save(sel);
    USBSerial.printf("[theme] -> %d (%s)\n", sel, theme_name_at(sel));
    lv_async_call(rebuild_async_settings, nullptr);
}
static void cards_on_style_cb(lv_event_t *e) {
    if (millis() - lastSwipeMs < CLICK_AFTER_SWIPE_MS) return;
    int sel = lv_dropdown_get_selected(lv_event_get_target(e));
    if (sel < 0 || sel > LS_GRID) return;
    if ((LauncherStyle)sel == g_launcher_style) return;
    g_launcher_style = (LauncherStyle)sel;
    launcher_style_save();
    USBSerial.printf("[style] -> %d\n", sel);
    // Only the Apps screen needs to change, but rebuild all for simplicity
    // (keeps the theme-change codepath and the style-change codepath in sync).
    lv_async_call(rebuild_async_settings, nullptr);
}
static void cards_on_quick_wp_cb(lv_event_t *e) {
    if (millis() - lastSwipeMs < CLICK_AFTER_SWIPE_MS) return;
    int sel = lv_dropdown_get_selected(lv_event_get_target(e));
    if (sel < 0 || sel >= g_photo_count) return;
    g_photo_idx = sel;
    refresh_wallpaper();
    save_wp_idx();
}
static void cards_on_wifi_cb(lv_event_t *e) {
    if (millis() - lastSwipeMs < CLICK_AFTER_SWIPE_MS) return;
    bool on = lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED);
    g_wifi_toggle = on;
    if (!on) wifi_shutdown();
    // If turned back on, the 12 h resync loop will re-connect on schedule.
}
static void cards_on_ble_cb(lv_event_t *e) {
    if (millis() - lastSwipeMs < CLICK_AFTER_SWIPE_MS) return;
    g_ble_toggle = lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED);
    if (g_ble_toggle) btStart();
    else              btStop();
}
static void cards_on_haptics_cb(lv_event_t *e) {
    if (millis() - lastSwipeMs < CLICK_AFTER_SWIPE_MS) return;
    g_haptics_toggle = lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED);
    // No physical vibe motor exposed on this board -- kept as a UI stub
    // so the setting is persistent-ready for future hardware.
}
static void cards_open_wallpaper_cb(lv_event_t *) {
    if (millis() - lastSwipeMs < CLICK_AFTER_SWIPE_MS) return;
    show_wallpaper_picker();
}
static void cards_open_developer_cb(lv_event_t *) {
    if (millis() - lastSwipeMs < CLICK_AFTER_SWIPE_MS) return;
    show_developer_info();
}
static void cards_factory_reset_cb(lv_event_t *) {
    if (millis() - lastSwipeMs < CLICK_AFTER_SWIPE_MS) return;
    show_factory_reset_modal();
}
static void cards_deep_sleep_cb(lv_event_t *) {
    if (millis() - lastSwipeMs < CLICK_AFTER_SWIPE_MS) return;
    deep_sleep_now();
}
static void cards_reconnect_wifi_cb(lv_event_t *) {
    if (millis() - lastSwipeMs < CLICK_AFTER_SWIPE_MS) return;
    batt_wifi_reconnect_all();
}

// ---- Card row builders ----------------------------------------------------
static void card_row_base(lv_obj_t *parent, lv_obj_t **out_row) {
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, LV_PCT(100), 72);
    lv_obj_set_style_bg_color(row, lv_color_hex(theme_fg()), 0);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(row, lv_color_hex(theme_primary()), 0);
    lv_obj_set_style_border_width(row, 1, 0);
    lv_obj_set_style_border_opa(row, 60, 0);
    lv_obj_set_style_radius(row, 14, 0);
    lv_obj_set_style_pad_left(row, 14, 0);
    lv_obj_set_style_pad_right(row, 14, 0);
    lv_obj_set_style_pad_top(row, 8, 0);
    lv_obj_set_style_pad_bottom(row, 8, 0);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    *out_row = row;
}

static void card_labels(lv_obj_t *row, const char *title, const char *subtitle) {
    lv_obj_t *t = lv_label_create(row);
    lv_label_set_text(t, title);
    lv_obj_set_style_text_color(t, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(t, &lv_font_montserrat_18, 0);
    lv_obj_align(t, LV_ALIGN_TOP_LEFT, 0, 0);
    if (subtitle && subtitle[0]) {
        lv_obj_t *s = lv_label_create(row);
        lv_label_set_text(s, subtitle);
        lv_obj_set_style_text_color(s, lv_color_hex(0x888888), 0);
        lv_obj_set_style_text_font(s, &lv_font_montserrat_12, 0);
        lv_obj_align(s, LV_ALIGN_BOTTOM_LEFT, 0, 0);
    }
}

static void card_toggle(lv_obj_t *parent, const char *title,
                        const char *subtitle, bool initial,
                        lv_event_cb_t cb) {
    lv_obj_t *row; card_row_base(parent, &row);
    card_labels(row, title, subtitle);
    lv_obj_t *sw = lv_switch_create(row);
    lv_obj_align(sw, LV_ALIGN_RIGHT_MID, 0, 0);
    if (initial) lv_obj_add_state(sw, LV_STATE_CHECKED);
    lv_obj_set_style_bg_color(sw, lv_color_hex(theme_accent()),
                              LV_PART_INDICATOR | LV_STATE_CHECKED);
    lv_obj_add_event_cb(sw, cb, LV_EVENT_VALUE_CHANGED, NULL);
}

static void card_slider(lv_obj_t *parent, const char *title,
                        const char *subtitle, int lo, int hi, int val,
                        lv_event_cb_t cb) {
    lv_obj_t *row; card_row_base(parent, &row);
    lv_obj_set_style_pad_bottom(row, 18, 0);
    lv_obj_set_size(row, LV_PCT(100), 88);
    lv_obj_t *t = lv_label_create(row);
    lv_label_set_text(t, title);
    lv_obj_set_style_text_color(t, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(t, &lv_font_montserrat_18, 0);
    lv_obj_align(t, LV_ALIGN_TOP_LEFT, 0, 0);
    if (subtitle && subtitle[0]) {
        lv_obj_t *s = lv_label_create(row);
        lv_label_set_text(s, subtitle);
        lv_obj_set_style_text_color(s, lv_color_hex(0x888888), 0);
        lv_obj_set_style_text_font(s, &lv_font_montserrat_12, 0);
        lv_obj_align(s, LV_ALIGN_TOP_LEFT, 0, 24);
    }
    lv_obj_t *sl = lv_slider_create(row);
    lv_obj_set_width(sl, LCD_WIDTH - 100);
    lv_obj_align(sl, LV_ALIGN_BOTTOM_MID, 0, 4);
    lv_slider_set_range(sl, lo, hi);
    lv_slider_set_value(sl, val, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(sl, lv_color_hex(0x2A2A2A), LV_PART_MAIN);
    lv_obj_set_style_bg_color(sl, lv_color_hex(theme_primary()),
                              LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(sl, lv_color_hex(theme_accent()),
                              LV_PART_KNOB);
    lv_obj_add_event_cb(sl, cb, LV_EVENT_VALUE_CHANGED, NULL);
}

static void card_dropdown(lv_obj_t *parent, const char *title,
                          const char *subtitle, const char *opts,
                          int selected, lv_event_cb_t cb) {
    lv_obj_t *row; card_row_base(parent, &row);
    card_labels(row, title, subtitle);
    lv_obj_t *dd = lv_dropdown_create(row);
    lv_dropdown_set_options(dd, opts ? opts : "");
    lv_dropdown_set_selected(dd, selected);
    lv_obj_set_width(dd, 160);
    lv_obj_align(dd, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_set_style_bg_color(dd, lv_color_hex(0x0A0A0A), 0);
    lv_obj_set_style_border_color(dd, lv_color_hex(theme_primary()), 0);
    lv_obj_set_style_text_color(dd, lv_color_hex(0xFFFFFF), 0);
    lv_obj_add_event_cb(dd, cb, LV_EVENT_VALUE_CHANGED, NULL);
}

static void card_action(lv_obj_t *parent, const char *title,
                        const char *subtitle, const char *btn_text,
                        uint32_t accent, lv_event_cb_t cb) {
    lv_obj_t *row; card_row_base(parent, &row);
    card_labels(row, title, subtitle);
    lv_obj_t *btn = lv_btn_create(row);
    lv_obj_set_size(btn, 110, 44);
    lv_obj_align(btn, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_set_style_radius(btn, 22, 0);
    lv_obj_set_style_bg_color(btn, lv_color_hex(0x0A0A0A), 0);
    lv_obj_set_style_border_color(btn, lv_color_hex(accent), 0);
    lv_obj_set_style_border_width(btn, 1, 0);
    lv_obj_t *bl = lv_label_create(btn);
    lv_label_set_text(bl, btn_text);
    lv_obj_set_style_text_color(bl, lv_color_hex(accent), 0);
    lv_obj_set_style_text_font(bl, &lv_font_montserrat_14, 0);
    lv_obj_center(bl);
    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, NULL);
}

// ============================================================================
//  SETUP
// ============================================================================
// ############################################################################
// #                                                                          #
// #   XANDER v1.0  --  offline, launcher-integrated companion                #
// #   ----------------------------------------------------------------       #
// #   A hidden layer beneath the clock face.  No AI, no radio dependency.    #
// #   Intelligence = command parsing + structured databases + watch state.   #
// #                                                                          #
// #     NAV:   FACE --swipe up--> XANDER HOME                                #
// #            HOME --swipe left--> PUZZLES   --swipe right--> HOME           #
// #            HOME --swipe right--> KNOWLEDGE --swipe left--> HOME           #
// #            (anywhere) --swipe down--> back to the clock face             #
// #                                                                          #
// #   Command entry: on-screen keyboard (typed intents) + tappable chips.    #
// #   Actions call the launcher's own functions (load_app_from_sd, WiFi/BLE).#
// #                                                                          #
// #   NOTE: needs LV_USE_KEYBOARD and LV_USE_TEXTAREA enabled in lv_conf.h   #
// #         (both are ON in the stock LVGL 8.x config).                      #
// ############################################################################

// ---- response library ------------------------------------------------------
static const char *X_R_OK[]     = { "On it.", "Consider it done.", "Right away.", "Done." };
static const char *X_R_ALREADY[]= { "We are already there.", "That is already running.", "Already set." };
static const char *X_R_ERR[]    = { "That did not work.", "Something is getting in the way.", "I could not do that." };
static const char *X_R_UNK[]    = { "I do not know that command yet.", "I did not catch that.", "That is not in my playbook yet." };
static const char *X_R_GREET[]  = { "What shall we investigate?", "Back again?", "Ready when you are.", "Let us dig in." };
static const char *xrand(const char *const *a, int n) { return a[(int)(millis() % (uint32_t)n)]; }

// ---- puzzle database -------------------------------------------------------
struct XPuzzle { int id; const char *category; uint8_t difficulty;
                 const char *question; const char *hint; const char *answer; };
static const XPuzzle kPuzzles[] = {
  {  1, "logic",       3, "Three switches downstairs control three bulbs upstairs. You may flip them freely but climb the stairs only once. How do you tell which switch controls which bulb?", "A bulb gives off more than light.", "Turn switch 1 on a few minutes then off, turn switch 2 on, go up. Lit = 2, warm = 1, cold = 3." },
  {  2, "math",        2, "I am a two-digit number. My tens digit is twice my units digit, and my digits add to 9. What am I?", "Tens = 2 x units, and they sum to 9.", "63" },
  {  3, "riddles",     2, "The more you take, the more you leave behind. What am I?", "Think about walking.", "Footsteps." },
  {  4, "physics",     3, "A feather and a hammer are dropped together on the Moon. Which lands first?", "There is no air up there.", "They land together - no atmosphere means no drag." },
  {  5, "chemistry",   3, "During the electrolysis of water, what gas is produced at the cathode?", "Reduction happens at the cathode.", "Hydrogen." },
  {  6, "coding",      3, "What is the time complexity of binary search on a sorted array of n items?", "You halve the search space each step.", "O(log n)." },
  {  7, "engineering", 3, "A 10-tooth gear drives a 30-tooth gear. If the small gear spins at 300 rpm, how fast does the large one turn?", "The tooth ratio is 1:3.", "100 rpm." },
  {  8, "patterns",    2, "What comes next: 1, 1, 2, 3, 5, 8, ?", "Each term is a sum of two others.", "13 - the Fibonacci sequence." },
  {  9, "logic",       4, "You have 8 identical-looking balls; one is slightly heavier. Using a balance scale, what is the fewest weighings that always finds it?", "Try splitting into three groups.", "2 weighings." },
  { 10, "math",        4, "What is the sum of all whole numbers from 1 to 100?", "Pair the ends: 1+100, 2+99...", "5050." },
  { 11, "riddles",     3, "I speak without a mouth and hear without ears. I have no body, but I come alive with the wind. What am I?", "You might hear me in the mountains.", "An echo." },
  { 12, "physics",     4, "If you double the voltage across a fixed resistor, what happens to the power it dissipates?", "Remember P = V squared over R.", "It quadruples - four times the power." },
  { 13, "coding",      4, "In binary, what is 1011 + 0110?", "Carry the ones just like decimal.", "10001 - which is 17 in decimal." },
  { 14, "engineering", 3, "Why does PWM let a digital pin dim an LED even though it can only output HIGH or LOW?", "It is about time, not voltage.", "It switches fast; the average on-time (duty cycle) sets the perceived brightness." },
  { 15, "chemistry",   2, "What is the chemical symbol and atomic number of the element iron?", "It rusts.", "Fe, atomic number 26." },
  { 16, "logic",       6, "A logician says: 'This statement is false.' Is that statement true or false?", "It refers to itself.", "Neither - it is the liar paradox; it cannot consistently be true or false." },
};
#define X_NPUZ ((int)(sizeof(kPuzzles)/sizeof(kPuzzles[0])))
static const char *X_PCATS[] = { "logic","math","chemistry","physics","engineering","coding","riddles","patterns" };
#define X_NPCAT ((int)(sizeof(X_PCATS)/sizeof(X_PCATS[0])))

// ---- knowledge database ----------------------------------------------------
struct XArticle { const char *id; const char *category; const char *title;
                  const char *summary; const char *details; };
static const XArticle kArticles[] = {
  { "chem_001","chemistry","Electrolysis","Electrolysis uses electrical energy to drive a chemical reaction that would not happen on its own.","A direct current is passed through an electrolyte. Positive ions travel to the cathode (reduction), negative ions to the anode (oxidation). Splitting water gives hydrogen at the cathode and oxygen at the anode." },
  { "chem_002","chemistry","Redox Reactions","Redox reactions transfer electrons: one species is oxidised (loses electrons) while another is reduced (gains them).","A handy memory aid is OIL RIG - Oxidation Is Loss, Reduction Is Gain. Rusting, combustion and batteries are all redox processes." },
  { "phys_001","physics","Ohm's Law","Ohm's law links voltage, current and resistance in a circuit: V = I x R.","Voltage (volts) equals current (amps) times resistance (ohms). Rearranged, I = V/R and R = V/I. It only holds for ohmic components at constant temperature." },
  { "phys_002","physics","Newton's Laws","Newton's three laws describe how forces change the motion of objects.","1) An object stays at rest or moves steadily unless a force acts. 2) F = m x a. 3) Every action has an equal and opposite reaction." },
  { "bio_001","biology","DNA","DNA stores the genetic instructions used to build and run living things.","It is a double helix of nucleotides using four bases: A, T, C and G. A pairs with T, C pairs with G. The order of bases spells out genes." },
  { "bio_002","biology","The Cell","The cell is the smallest unit of life; all organisms are made of one or more cells.","Animal cells have a nucleus, mitochondria, cytoplasm and a membrane. Plant cells add a cell wall and chloroplasts for photosynthesis." },
  { "math_001","maths","Pi","Pi is the ratio of a circle's circumference to its diameter, about 3.14159.","Pi is irrational - its decimals never repeat or end. Circumference = 2 x pi x r, and area = pi x r squared." },
  { "math_002","maths","Prime Numbers","A prime number is a whole number greater than 1 whose only divisors are 1 and itself.","2, 3, 5, 7, 11, 13... 2 is the only even prime. Every whole number above 1 factorises into primes in exactly one way." },
  { "eng_001","engineering","Torque","Torque is a twisting force - how strongly something is turned about a pivot.","Torque = force x distance from the pivot. A longer lever means more torque for the same push, which is why long wrenches loosen tight bolts." },
  { "elec_001","electronics","The Transistor","A transistor is a tiny switch or amplifier that controls a large current with a small one.","In digital logic it acts as an on/off switch; billions of them make up a CPU. The two common families are BJT and MOSFET." },
  { "elec_002","electronics","I2C Bus","I2C is a two-wire serial bus that lets a controller talk to many chips.","It uses SDA (data) and SCL (clock). Each device has an address, so a sensor, a touch panel and a power chip can all share the same pair of wires - exactly how this watch works." },
  { "robo_001","robotics","PID Control","A PID controller keeps a system on target by reacting to error in three ways.","Proportional reacts to the current error, Integral to accumulated past error, Derivative to how fast the error is changing. Tuning the three gains gives smooth, stable control." },
  { "robo_002","robotics","Servo Motors","A servo is a motor that moves to and holds a precise angle you command.","A control pulse tells the servo the target position; internal feedback drives it there and holds it. Hobby servos usually cover about 180 degrees." },
  { "prog_001","programming","Recursion","Recursion is when a function solves a problem by calling itself on a smaller version of it.","Every recursion needs a base case to stop, or it runs forever. Factorials, tree walks and divide-and-conquer algorithms are natural fits." },
  { "prog_002","programming","Big-O Notation","Big-O describes how an algorithm's cost grows as the input grows.","O(1) is constant, O(log n) halves each step, O(n) is linear, O(n squared) is nested loops. It ignores constants and focuses on the trend at scale." },
  { "astro_001","astronomy","Black Holes","A black hole is a region where gravity is so strong that not even light can escape.","They form when massive stars collapse. The event horizon is the point of no return; the singularity lies at the centre. We spot them by their effect on nearby matter and light." },
  { "astro_002","astronomy","Light-Year","A light-year is the distance light travels in one year - about 9.46 trillion km.","It is a measure of distance, not time. Because light takes years to reach us, looking far into space is also looking back in time." },
  { "geo_001","geography","Plate Tectonics","Earth's crust is broken into slabs that drift on the mantle, shaping continents.","Where plates pull apart, spread, or grind past one another you get ridges, volcanoes, mountains and earthquakes. They move a few centimetres a year." },
  { "gen_001","general","The Greenhouse Effect","Certain gases trap heat in the atmosphere, keeping the planet warm enough for life.","Sunlight warms the surface; the surface re-emits infrared, which gases like CO2 and water vapour absorb and re-radiate. Too much of these gases warms the planet further." },
};
#define X_NART ((int)(sizeof(kArticles)/sizeof(kArticles[0])))
static const char *X_KCATS[] = { "chemistry","physics","biology","maths","engineering","electronics","robotics","programming","astronomy","geography","general" };
#define X_NKCAT ((int)(sizeof(X_KCATS)/sizeof(X_KCATS[0])))

// ---- command template table (chips + keyword intents) ----------------------
enum XIntent {
  XI_UNKNOWN=0, XI_HELP, XI_HOME_EXIT,
  XI_OPEN_PUZZLES, XI_OPEN_KNOWLEDGE, XI_GIVE_PUZZLE, XI_TEACH,
  XI_WIFI_ON, XI_WIFI_OFF, XI_BLE_ON, XI_BLE_OFF,
  XI_GPS_ON, XI_GPS_OFF, XI_BEACON_ON, XI_BEACON_OFF,
  XI_SHOW_BATTERY, XI_SHOW_STATUS
};
struct XTmpl { const char *label; const char *keys; int intent; };
static const XTmpl kTmpl[] = {
  { "Give me a puzzle",   "puzzle riddle challenge quiz",        XI_GIVE_PUZZLE },
  { "Teach me something", "teach learn lesson knowledge",        XI_TEACH },
  { "Open puzzles",       "puzzles",                             XI_OPEN_PUZZLES },
  { "Open knowledge",     "knowledge library browse",            XI_OPEN_KNOWLEDGE },
  { "Show battery",       "battery power charge percent level",  XI_SHOW_BATTERY },
  { "Show status",        "status connection state system",      XI_SHOW_STATUS },
  { "WiFi on",            "wifi wireless on enable",             XI_WIFI_ON },
  { "WiFi off",           "wifi wireless off disable",           XI_WIFI_OFF },
  { "Bluetooth on",       "bluetooth ble on enable",             XI_BLE_ON },
  { "Bluetooth off",      "bluetooth ble off disable",           XI_BLE_OFF },
  { "GPS on",             "gps location on enable",              XI_GPS_ON },
  { "GPS off",            "gps location off disable",            XI_GPS_OFF },
  { "Beacon on",          "beacon on enable",                    XI_BEACON_ON },
  { "Beacon off",         "beacon off disable",                  XI_BEACON_OFF },
  { "Help",               "help commands",                       XI_HELP },
  { "Return home",        "home clock exit return face watch",   XI_HOME_EXIT },
};
#define X_NTMPL ((int)(sizeof(kTmpl)/sizeof(kTmpl[0])))

// ---- state -----------------------------------------------------------------
static bool       g_xander_active = false;
static lv_obj_t  *g_x_scr      = nullptr;
static lv_obj_t  *g_x_resp_lbl = nullptr;
static int        g_x_top      = 1;      // 0 = puzzles, 1 = home, 2 = knowledge
static bool       g_x_console  = false;
static int        g_x_orb      = 0;      // 0 idle, 1 success, 2 error
static String     g_x_last     = "";
// puzzle sub-state
static int        g_x_puz_view = 0;      // 0 categories, 1 puzzle card
static int        g_x_puz_id   = 0;      // index into kPuzzles
static bool       g_x_puz_hint = false, g_x_puz_reveal = false;
// knowledge sub-state
static int        g_x_kn_view  = 0;      // 0 categories, 1 article list, 2 detail
static String     g_x_kn_cat   = "";
static int        g_x_kn_art   = 0;      // index into kArticles
static bool       g_x_kn_more  = false;
// virtual hardware flags (no GPS/beacon silicon wired -> tracked in software)
static bool       g_x_gps = false, g_x_beacon = false;
// persisted stats + settings
static Preferences g_xprefs;
static bool       g_x_loaded = false;
static long       g_xs_solved=0, g_xs_correct=0, g_xs_hints=0, g_xs_streak=0, g_xs_best=0, g_xs_hard=0;
static bool       g_x_voice=false, g_x_alwayson=false;
static int        g_x_battlim=1;

// ---- module forward declarations -------------------------------------------
static void xander_enter();
static void xander_exit();
static void xander_handle_swipe(SwipeEvent ev);
static void x_render();
static void x_process(String raw);
static void x_do(int intent);
static void x_open_article(int i);
static void x_launch_app(int i);
static void show_xander_settings();
static void xander_service();                    // deferred UI worker (runs in loop, never inside an LVGL event)
static void xander_free_launcher_screens();
static void xander_rebuild_launcher_screens();

// Deferred Xander UI actions.  Xander (re)builds its screen OUTSIDE any LVGL
// event callback for two reasons:
//   1. so we never free the very widget that is currently handling a click
//      (the puzzle / knowledge / console buttons all trigger a re-render), and
//   2. so we can free the heavy launcher screens BEFORE building Xander's
//      views -- which stops the LVGL object pool from running dry (the old
//      "only the XANDER title paints, then the whole UI freezes" bug).
enum XAction { XA_NONE = 0, XA_ENTER, XA_RENDER, XA_EXIT };
static volatile XAction g_x_action = XA_NONE;
static uint32_t          g_x_free_at = 0;        // delayed 2nd free pass (0 = idle)

// ---- persistence -----------------------------------------------------------
static void x_stats_load() {
    if (g_x_loaded) return;
    g_xprefs.begin("xander", true);
    g_xs_solved  = g_xprefs.getLong("solved", 0);
    g_xs_correct = g_xprefs.getLong("correct", 0);
    g_xs_hints   = g_xprefs.getLong("hints", 0);
    g_xs_streak  = g_xprefs.getLong("streak", 0);
    g_xs_best    = g_xprefs.getLong("best", 0);
    g_xs_hard    = g_xprefs.getLong("hard", 0);
    g_x_voice    = g_xprefs.getBool("voice", false);
    g_x_alwayson = g_xprefs.getBool("aon", false);
    g_x_battlim  = g_xprefs.getInt("blim", 1);
    g_xprefs.end();
    g_x_loaded = true;
}
static void x_stats_save() {
    g_xprefs.begin("xander", false);
    g_xprefs.putLong("solved", g_xs_solved);
    g_xprefs.putLong("correct", g_xs_correct);
    g_xprefs.putLong("hints", g_xs_hints);
    g_xprefs.putLong("streak", g_xs_streak);
    g_xprefs.putLong("best", g_xs_best);
    g_xprefs.putLong("hard", g_xs_hard);
    g_xprefs.end();
}
static void x_settings_save() {
    g_xprefs.begin("xander", false);
    g_xprefs.putBool("voice", g_x_voice);
    g_xprefs.putBool("aon", g_x_alwayson);
    g_xprefs.putInt("blim", g_x_battlim);
    g_xprefs.end();
}

// ---- small helpers ---------------------------------------------------------
static String x_cap(const char *s) {
    String r = s;
    if (r.length()) r.setCharAt(0, (char)toupper(r[0]));
    return r;
}
static String x_diff(int d) {
    if (d >= 6) return String("HAWTHORNE");
    String s = "Difficulty "; s += d; s += "/5"; return s;
}
static void x_set_resp(const char *s) {
    g_x_last = s;
    if (g_x_resp_lbl) lv_label_set_text(g_x_resp_lbl, s);
}
static void x_finish_info() {   // reflect g_x_last after a non-navigating command
    if (g_x_console) { if (g_x_resp_lbl) lv_label_set_text(g_x_resp_lbl, g_x_last.c_str()); }
    else             { x_render(); }
}
// tokeniser: lowercases already-lowercased input into alnum words
static int x_tokenize(const String &in, String *out, int maxn) {
    int n = 0; String cur = "";
    for (size_t i = 0; i <= in.length(); i++) {
        char c = (i < in.length()) ? in[i] : ' ';
        if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) cur += c;
        else if (cur.length()) { if (n < maxn) out[n++] = cur; cur = ""; }
    }
    return n;
}
static bool x_tok_has(String *t, int n, const char *w) {
    for (int i = 0; i < n; i++) if (t[i] == w) return true;
    return false;
}
static bool x_is_stop(const String &w) {
    static const char *S[] = { "launch","open","start","run","go","to","the",
                               "mode","os","me","my","please","xander","a","an","show","get" };
    for (unsigned i = 0; i < sizeof(S)/sizeof(S[0]); i++) if (w == S[i]) return true;
    return false;
}
static int x_best_tmpl(String *t, int n, int &outIdx) {
    int best = 0; outIdx = -1;
    for (int i = 0; i < X_NTMPL; i++) {
        String kt[8]; int kn = x_tokenize(String(kTmpl[i].keys), kt, 8);
        int s = 0;
        for (int k = 0; k < kn; k++) if (x_tok_has(t, n, kt[k].c_str())) s++;
        if (s > best) { best = s; outIdx = i; }
    }
    return best;
}
static int x_find_app_tok(String *t, int n, int &outIdx) {
    int best = 0; outIdx = -1;
    for (int i = 0; i < g_app_count; i++) {
        String nm = g_apps[i].name; nm.toLowerCase();
        int s = 0;
        for (int k = 0; k < n; k++)
            if (t[k].length() >= 3 && !x_is_stop(t[k]) && nm.indexOf(t[k]) >= 0) s += 2;
        if (s > best) { best = s; outIdx = i; }
    }
    return best;
}
static int x_find_article(const String &q) {
    String t[12]; int n = x_tokenize(q, t, 12);
    int best = 0, bi = -1;
    for (int i = 0; i < X_NART; i++) {
        String hay = String(kArticles[i].title) + " " + kArticles[i].id + " " +
                     kArticles[i].category + " " + kArticles[i].summary;
        hay.toLowerCase();
        int s = 0;
        for (int k = 0; k < n; k++)
            if (t[k].length() >= 3 && !x_is_stop(t[k]) && hay.indexOf(t[k]) >= 0) s += 2;
        if (s > best) { best = s; bi = i; }
    }
    return best > 0 ? bi : -1;
}

// ---- UI builders -----------------------------------------------------------
static void x_header(const char *title, const char *sub) {
    lv_obj_t *t = lv_label_create(g_x_scr);
    lv_label_set_text(t, title);
    lv_obj_set_style_text_color(t, lv_color_hex(theme_primary()), 0);
    lv_obj_set_style_text_font(t, &lv_font_montserrat_20, 0);
    lv_obj_align(t, LV_ALIGN_TOP_MID, 0, 14);
    if (sub) {
        lv_obj_t *s = lv_label_create(g_x_scr);
        lv_label_set_text(s, sub);
        lv_obj_set_style_text_color(s, lv_color_hex(0x888888), 0);
        lv_obj_set_style_text_font(s, &lv_font_montserrat_12, 0);
        lv_obj_align(s, LV_ALIGN_TOP_MID, 0, 42);
    }
}
static lv_obj_t *x_list(int y, int h) {
    lv_obj_t *l = lv_obj_create(g_x_scr);
    lv_obj_set_size(l, LCD_WIDTH - 30, h);
    lv_obj_align(l, LV_ALIGN_TOP_MID, 0, y);
    lv_obj_set_style_bg_opa(l, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(l, 0, 0);
    lv_obj_set_style_pad_all(l, 6, 0);
    lv_obj_set_style_pad_row(l, 8, 0);
    lv_obj_set_flex_flow(l, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(l, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START);
    lv_obj_set_scroll_dir(l, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(l, LV_SCROLLBAR_MODE_AUTO);
    return l;
}
static lv_obj_t *x_row_btn(lv_obj_t *p, const char *txt, uint32_t border,
                           lv_event_cb_t cb, intptr_t ud) {
    lv_obj_t *b = lv_btn_create(p);
    lv_obj_remove_style_all(b);
    lv_obj_set_width(b, LV_PCT(100));
    lv_obj_set_style_bg_color(b, lv_color_hex(theme_fg()), 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(b, 12, 0);
    lv_obj_set_style_border_color(b, lv_color_hex(border), 0);
    lv_obj_set_style_border_width(b, 1, 0);
    lv_obj_set_style_pad_all(b, 12, 0);
    lv_obj_t *l = lv_label_create(b);
    lv_label_set_text(l, txt);
    lv_obj_set_style_text_color(l, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(l, &lv_font_montserrat_16, 0);
    lv_obj_align(l, LV_ALIGN_LEFT_MID, 0, 0);
    if (cb) lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, (void *)ud);
    return b;
}
static lv_obj_t *x_cbtn(lv_obj_t *parent, const char *txt, int w, uint32_t border,
                        lv_event_cb_t cb, intptr_t ud) {
    lv_obj_t *b = lv_btn_create(parent);
    lv_obj_remove_style_all(b);
    lv_obj_set_size(b, w, 50);
    lv_obj_set_style_bg_color(b, lv_color_hex(theme_fg()), 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(b, 14, 0);
    lv_obj_set_style_border_color(b, lv_color_hex(border), 0);
    lv_obj_set_style_border_width(b, 2, 0);
    lv_obj_t *l = lv_label_create(b);
    lv_label_set_text(l, txt);
    lv_obj_set_style_text_color(l, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(l, &lv_font_montserrat_16, 0);
    lv_obj_center(l);
    if (cb) lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, (void *)ud);
    return b;
}
static lv_obj_t *x_wraplbl(lv_obj_t *parent, const char *txt, int w, uint32_t color,
                           const lv_font_t *font) {
    lv_obj_t *l = lv_label_create(parent);
    lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(l, w);
    lv_label_set_text(l, txt);
    lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
    lv_obj_set_style_text_font(l, font, 0);
    return l;
}

// ---- callbacks -------------------------------------------------------------
static void x_cmd_open_cb(lv_event_t *) { g_x_console = true; g_x_last = ""; x_render(); }

static void x_kb_cb(lv_event_t *e) {
    lv_event_code_t code = lv_event_get_code(e);
    lv_obj_t *ta = (lv_obj_t *)lv_event_get_user_data(e);
    if (code == LV_EVENT_READY) {
        String txt = lv_textarea_get_text(ta);
        x_process(txt);
        if (g_x_console && ta) lv_textarea_set_text(ta, "");   // still in console -> clear
    } else if (code == LV_EVENT_CANCEL) {
        g_x_console = false; x_render();
    }
}
static void x_chip_cb(lv_event_t *e) {
    intptr_t ud = (intptr_t)lv_event_get_user_data(e);
    if (ud >= 1000) x_launch_app((int)(ud - 1000));
    else            x_do(kTmpl[ud].intent);
}
// puzzle
static void x_puz_cat_cb(lv_event_t *e) {
    int ci = (int)(intptr_t)lv_event_get_user_data(e);
    const char *cat = X_PCATS[ci];
    for (int i = 0; i < X_NPUZ; i++) if (String(kPuzzles[i].category) == cat) { g_x_puz_id = i; break; }
    g_x_puz_view = 1; g_x_puz_hint = false; g_x_puz_reveal = false; x_render();
}
static void x_puz_think_cb(lv_event_t *) { g_x_puz_reveal = !g_x_puz_reveal; x_render(); }
static void x_puz_hint_cb(lv_event_t *)  {
    if (!g_x_puz_hint) { g_x_puz_hint = true; g_xs_hints++; x_stats_save(); }
    x_render();
}
static void x_puz_next_cb(lv_event_t *) {
    String c = kPuzzles[g_x_puz_id].category;
    for (int k = 1; k <= X_NPUZ; k++) {
        int j = (g_x_puz_id + k) % X_NPUZ;
        if (String(kPuzzles[j].category) == c) { g_x_puz_id = j; break; }
    }
    g_x_puz_hint = false; g_x_puz_reveal = false; g_x_last = ""; x_render();
}
static void x_puz_cats_cb(lv_event_t *) { g_x_puz_view = 0; x_render(); }
static void x_puz_got_cb(lv_event_t *e) {
    int correct = (int)(intptr_t)lv_event_get_user_data(e);
    g_xs_solved++;
    if (correct) {
        g_xs_correct++; g_xs_streak++;
        if (g_xs_streak > g_xs_best) g_xs_best = g_xs_streak;
        if (kPuzzles[g_x_puz_id].difficulty > g_xs_hard) g_xs_hard = kPuzzles[g_x_puz_id].difficulty;
        g_x_last = "Correct. Nicely done.";
    } else {
        g_xs_streak = 0; g_x_last = "Not this time - keep at it.";
    }
    x_stats_save(); x_puz_next_cb(nullptr);
}
// knowledge
static void x_kn_cat_cb(lv_event_t *e) {
    int ci = (int)(intptr_t)lv_event_get_user_data(e);
    g_x_kn_cat = X_KCATS[ci]; g_x_kn_view = 1; x_render();
}
static void x_kn_art_cb(lv_event_t *e) {
    g_x_kn_art = (int)(intptr_t)lv_event_get_user_data(e);
    g_x_kn_view = 2; g_x_kn_more = false; x_render();
}
static void x_kn_more_cb(lv_event_t *) { g_x_kn_more = !g_x_kn_more; x_render(); }
static void x_kn_back_cb(lv_event_t *)  {
    if (g_x_kn_view == 2) { if (g_x_kn_more) g_x_kn_more = false; else g_x_kn_view = 1; }
    else g_x_kn_view = 0;
    x_render();
}

// ---- view: HOME ------------------------------------------------------------
static void x_render_home() {
    x_header("XANDER", NULL);
    // orb
    lv_obj_t *orb = lv_obj_create(g_x_scr);
    lv_obj_remove_style_all(orb);
    lv_obj_set_size(orb, 140, 140);
    lv_obj_align(orb, LV_ALIGN_TOP_MID, 0, 78);
    lv_obj_set_style_radius(orb, LV_RADIUS_CIRCLE, 0);
    uint32_t oc = (g_x_orb == 1) ? 0x00FF80 : (g_x_orb == 2) ? theme_danger() : theme_primary();
    lv_obj_set_style_bg_color(orb, lv_color_hex(theme_fg()), 0);
    lv_obj_set_style_bg_opa(orb, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(orb, lv_color_hex(oc), 0);
    lv_obj_set_style_border_width(orb, 4, 0);
    lv_obj_set_style_shadow_color(orb, lv_color_hex(oc), 0);
    lv_obj_set_style_shadow_width(orb, 28, 0);
    lv_obj_set_style_shadow_opa(orb, 110, 0);
    lv_obj_clear_flag(orb, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *g = lv_label_create(orb);
    const char *glyph = (g_x_orb == 1) ? LV_SYMBOL_OK
                      : (g_x_orb == 2) ? LV_SYMBOL_WARNING : LV_SYMBOL_EYE_OPEN;
    lv_label_set_text(g, glyph);
    lv_obj_set_style_text_color(g, lv_color_hex(oc), 0);
    lv_obj_set_style_text_font(g, &lv_font_montserrat_28, 0);
    lv_obj_center(g);
    // response caption
    lv_obj_t *r = x_wraplbl(g_x_scr, g_x_last.length() ? g_x_last.c_str()
                                                       : "What shall we investigate?",
                            LCD_WIDTH - 60, 0xFFFFFF, &lv_font_montserrat_18);
    lv_obj_set_style_text_align(r, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(r, LV_ALIGN_TOP_MID, 0, 232);
    g_x_resp_lbl = r;
    // COMMAND button
    lv_obj_t *cmd = x_cbtn(g_x_scr, LV_SYMBOL_KEYBOARD "  COMMAND", 220, theme_primary(), x_cmd_open_cb, 0);
    lv_obj_align(cmd, LV_ALIGN_TOP_MID, 0, 320);
    // footers
    lv_obj_t *f1 = lv_label_create(g_x_scr);
    lv_label_set_text(f1, LV_SYMBOL_LEFT "  puzzles     knowledge  " LV_SYMBOL_RIGHT);
    lv_obj_set_style_text_color(f1, lv_color_hex(0x777777), 0);
    lv_obj_set_style_text_font(f1, &lv_font_montserrat_14, 0);
    lv_obj_align(f1, LV_ALIGN_BOTTOM_MID, 0, -34);
    lv_obj_t *f2 = lv_label_create(g_x_scr);
    lv_label_set_text(f2, LV_SYMBOL_DOWN "  swipe down to exit");
    lv_obj_set_style_text_color(f2, lv_color_hex(0x555555), 0);
    lv_obj_set_style_text_font(f2, &lv_font_montserrat_12, 0);
    lv_obj_align(f2, LV_ALIGN_BOTTOM_MID, 0, -12);
}

// ---- view: CONSOLE (keyboard + tappable command chips) ---------------------
static void x_render_console() {
    x_header("XANDER", "type a command or tap one");
    lv_obj_t *r = x_wraplbl(g_x_scr, g_x_last.length() ? g_x_last.c_str() : "",
                            LCD_WIDTH - 40, theme_primary(), &lv_font_montserrat_14);
    lv_obj_set_style_text_align(r, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(r, LV_ALIGN_TOP_MID, 0, 58);
    g_x_resp_lbl = r;
    lv_obj_t *ta = lv_textarea_create(g_x_scr);
    lv_textarea_set_one_line(ta, true);
    lv_textarea_set_placeholder_text(ta, "e.g. give me a puzzle");
    lv_obj_set_width(ta, LCD_WIDTH - 40);
    lv_obj_set_style_text_font(ta, &lv_font_montserrat_16, 0);
    lv_obj_align(ta, LV_ALIGN_TOP_MID, 0, 82);
    lv_obj_t *list = x_list(126, 172);
    for (int i = 0; i < X_NTMPL; i++)
        x_row_btn(list, kTmpl[i].label, theme_accent(), x_chip_cb, (intptr_t)i);
    int nApp = g_app_count < 6 ? g_app_count : 6;
    for (int j = 0; j < nApp; j++) {
        String lbl = String(LV_SYMBOL_PLAY "  Launch ") + g_apps[j].name;
        x_row_btn(list, lbl.c_str(), theme_primary(), x_chip_cb, (intptr_t)(1000 + j));
    }
    lv_obj_t *kb = lv_keyboard_create(g_x_scr);
    lv_obj_set_size(kb, LCD_WIDTH, 196);
    lv_obj_align(kb, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_keyboard_set_textarea(kb, ta);
    lv_obj_add_event_cb(kb, x_kb_cb, LV_EVENT_ALL, (void *)ta);
}

// ---- view: PUZZLES ---------------------------------------------------------
static void x_render_puzzles() {
    if (g_x_puz_view == 0) {
        x_header("PUZZLES", "choose a category");
        lv_obj_t *list = x_list(72, LCD_HEIGHT - 130);
        for (int i = 0; i < X_NPCAT; i++)
            x_row_btn(list, x_cap(X_PCATS[i]).c_str(), theme_accent(), x_puz_cat_cb, (intptr_t)i);
        lv_obj_t *f = lv_label_create(g_x_scr);
        lv_label_set_text(f, LV_SYMBOL_DOWN "  exit    " LV_SYMBOL_RIGHT "  home");
        lv_obj_set_style_text_color(f, lv_color_hex(0x666666), 0);
        lv_obj_set_style_text_font(f, &lv_font_montserrat_12, 0);
        lv_obj_align(f, LV_ALIGN_BOTTOM_MID, 0, -8);
        return;
    }
    const XPuzzle &p = kPuzzles[g_x_puz_id];
    char sub[64]; snprintf(sub, sizeof(sub), "#%03d - %s", p.id, x_cap(p.category).c_str());
    x_header("PUZZLE", sub);
    lv_obj_t *list = x_list(70, LCD_HEIGHT - 200);
    lv_obj_t *dl = lv_label_create(list);
    lv_label_set_text(dl, x_diff(p.difficulty).c_str());
    lv_obj_set_style_text_color(dl, lv_color_hex(p.difficulty >= 6 ? theme_accent() : 0x999999), 0);
    lv_obj_set_style_text_font(dl, &lv_font_montserrat_14, 0);
    x_wraplbl(list, p.question, LCD_WIDTH - 54, 0xFFFFFF, &lv_font_montserrat_18);
    if (g_x_puz_hint) {
        lv_obj_t *h = x_wraplbl(list, (String("Hint: ") + p.hint).c_str(),
                                LCD_WIDTH - 54, theme_accent(), &lv_font_montserrat_16);
        (void)h;
    }
    if (g_x_puz_reveal) {
        x_wraplbl(list, (String("Answer: ") + p.answer).c_str(),
                  LCD_WIDTH - 54, 0x00FF80, &lv_font_montserrat_16);
        lv_obj_t *row = lv_obj_create(list);
        lv_obj_remove_style_all(row);
        lv_obj_set_width(row, LV_PCT(100));
        lv_obj_set_height(row, 54);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_style_pad_column(row, 8, 0);
        x_cbtn(row, "I got it", (LCD_WIDTH - 70) / 2, 0x00FF80, x_puz_got_cb, 1);
        x_cbtn(row, "Missed it", (LCD_WIDTH - 70) / 2, theme_danger(), x_puz_got_cb, 0);
    }
    if (g_x_last.length()) {
        lv_obj_t *m = x_wraplbl(list, g_x_last.c_str(), LCD_WIDTH - 54, theme_primary(),
                                &lv_font_montserrat_14);
        (void)m;
    }
    // action bar
    lv_obj_t *bar = lv_obj_create(g_x_scr);
    lv_obj_remove_style_all(bar);
    lv_obj_set_size(bar, LCD_WIDTH - 20, 50);
    lv_obj_align(bar, LV_ALIGN_BOTTOM_MID, 0, -34);
    lv_obj_set_flex_flow(bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(bar, 6, 0);
    int bw = (LCD_WIDTH - 20 - 18) / 3;
    x_cbtn(bar, g_x_puz_reveal ? "Hide" : "Think", bw, theme_primary(), x_puz_think_cb, 0);
    x_cbtn(bar, "Hint", bw, theme_accent(), x_puz_hint_cb, 0);
    x_cbtn(bar, "Next", bw, theme_primary(), x_puz_next_cb, 0);
    char sf[80];
    snprintf(sf, sizeof(sf), "Solved %ld  Streak %ld  Best %ld", g_xs_solved, g_xs_streak, g_xs_best);
    lv_obj_t *st = lv_label_create(g_x_scr);
    lv_label_set_text(st, sf);
    lv_obj_set_style_text_color(st, lv_color_hex(0x666666), 0);
    lv_obj_set_style_text_font(st, &lv_font_montserrat_12, 0);
    lv_obj_align(st, LV_ALIGN_BOTTOM_MID, 0, -8);
    lv_obj_t *pback = x_cbtn(g_x_scr, LV_SYMBOL_LEFT, 44, theme_accent(), x_puz_cats_cb, 0);
    lv_obj_align(pback, LV_ALIGN_TOP_LEFT, 8, 10);
}

// ---- view: KNOWLEDGE -------------------------------------------------------
static int x_cat_count(const char *key) {
    int c = 0; for (int i = 0; i < X_NART; i++) if (String(kArticles[i].category) == key) c++;
    return c;
}
static void x_render_knowledge() {
    if (g_x_kn_view == 0) {
        x_header("KNOWLEDGE", "choose a field");
        lv_obj_t *list = x_list(72, LCD_HEIGHT - 130);
        for (int i = 0; i < X_NKCAT; i++) {
            if (x_cat_count(X_KCATS[i]) == 0) continue;
            char lbl[48]; snprintf(lbl, sizeof(lbl), "%s  (%d)", x_cap(X_KCATS[i]).c_str(),
                                   x_cat_count(X_KCATS[i]));
            x_row_btn(list, lbl, theme_accent(), x_kn_cat_cb, (intptr_t)i);
        }
        lv_obj_t *f = lv_label_create(g_x_scr);
        lv_label_set_text(f, LV_SYMBOL_LEFT "  home    " LV_SYMBOL_DOWN "  exit");
        lv_obj_set_style_text_color(f, lv_color_hex(0x666666), 0);
        lv_obj_set_style_text_font(f, &lv_font_montserrat_12, 0);
        lv_obj_align(f, LV_ALIGN_BOTTOM_MID, 0, -8);
        return;
    }
    if (g_x_kn_view == 1) {
        x_header(x_cap(g_x_kn_cat.c_str()).c_str(), "articles");
        lv_obj_t *list = x_list(72, LCD_HEIGHT - 130);
        for (int i = 0; i < X_NART; i++)
            if (g_x_kn_cat == kArticles[i].category)
                x_row_btn(list, kArticles[i].title, theme_accent(), x_kn_art_cb, (intptr_t)i);
        lv_obj_t *kback = x_cbtn(g_x_scr, LV_SYMBOL_LEFT "  fields", 150, theme_primary(), x_kn_back_cb, 0);
        lv_obj_align(kback, LV_ALIGN_BOTTOM_MID, 0, -10);
        return;
    }
    const XArticle &a = kArticles[g_x_kn_art];
    x_header(a.title, x_cap(a.category).c_str());
    lv_obj_t *list = x_list(72, LCD_HEIGHT - 200);
    x_wraplbl(list, a.summary, LCD_WIDTH - 54, 0xFFFFFF, &lv_font_montserrat_18);
    if (g_x_kn_more)
        x_wraplbl(list, a.details, LCD_WIDTH - 54, 0xBBBBBB, &lv_font_montserrat_16);
    lv_obj_t *bar = lv_obj_create(g_x_scr);
    lv_obj_remove_style_all(bar);
    lv_obj_set_size(bar, LCD_WIDTH - 20, 50);
    lv_obj_align(bar, LV_ALIGN_BOTTOM_MID, 0, -12);
    lv_obj_set_flex_flow(bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(bar, 8, 0);
    x_cbtn(bar, LV_SYMBOL_LEFT "  Back", (LCD_WIDTH - 70) / 2, theme_primary(), x_kn_back_cb, 0);
    x_cbtn(bar, g_x_kn_more ? "Less" : "More " LV_SYMBOL_RIGHT,
           (LCD_WIDTH - 70) / 2, theme_accent(), x_kn_more_cb, 0);
}

// ---- render dispatch -------------------------------------------------------
// Each render builds a FRESH screen and swaps to it with a short fade, letting
// LVGL auto-delete the previous Xander screen *after* the animation completes.
// This is the same deferred-deletion pattern the launcher uses, and it is the
// key reason a re-render triggered from a button's own click handler is safe:
// the screen the button lives on is not freed until the current event returns.
static void x_build_into(lv_obj_t *scr) {
    g_x_resp_lbl = nullptr;
    if (g_x_console)            x_render_console();
    else if (g_x_top == 0)      x_render_puzzles();
    else if (g_x_top == 2)      x_render_knowledge();
    else                        x_render_home();
    (void)scr;
}
static lv_obj_t *x_new_screen() {
    lv_obj_t *s = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s, lv_color_hex(theme_bg()), 0);
    lv_obj_set_style_bg_opa(s, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(s, 0, 0);
    lv_obj_set_style_border_width(s, 0, 0);
    lv_obj_clear_flag(s, LV_OBJ_FLAG_SCROLLABLE);
    return s;
}
// x_render() may be called from inside an LVGL button/keyboard event.  It only
// REQUESTS a rebuild -- the actual lv_obj_clean + rebuild happens in loop() via
// xander_service(), safely outside the event that asked for it.
static void x_render() {
    if (!g_xander_active) return;
    g_x_action = XA_RENDER;
}

// ---- command execution -----------------------------------------------------
static void x_launch_app(int i) {
    if (i < 0 || i >= g_app_count) { g_x_last = xrand(X_R_ERR, 3); g_x_orb = 2; x_finish_info(); return; }
    // Real DualCore launch: OTA-copy the .bin and reboot into it.
    load_app_from_sd(g_apps[i]);   // does not return (esp_restart)
}
static void x_open_article(int i) {
    g_x_console = false; g_x_top = 2;
    g_x_kn_cat = kArticles[i].category; g_x_kn_view = 2; g_x_kn_art = i; g_x_kn_more = false;
    g_x_orb = 1; x_render();
}
static void x_open_puzzle(int i) {
    g_x_console = false; g_x_top = 0;
    g_x_puz_view = 1; g_x_puz_id = i; g_x_puz_hint = false; g_x_puz_reveal = false;
    g_x_orb = 1; x_render();
}
static void x_do(int intent) {
    switch (intent) {
    case XI_HELP:
        g_x_last = "Try: launch <app>, give me a puzzle, teach me something, "
                   "what is <topic>, battery, status, wifi on/off, bluetooth on/off. "
                   "Swipe up to enter, down to exit, left for puzzles, right for knowledge.";
        g_x_orb = 0; x_finish_info(); return;
    case XI_HOME_EXIT:      xander_exit(); return;
    case XI_OPEN_PUZZLES:   g_x_console = false; g_x_top = 0; g_x_puz_view = 0; g_x_orb = 1; x_render(); return;
    case XI_OPEN_KNOWLEDGE: g_x_console = false; g_x_top = 2; g_x_kn_view = 0; g_x_kn_cat = ""; g_x_orb = 1; x_render(); return;
    case XI_GIVE_PUZZLE:    x_open_puzzle((int)(millis() % X_NPUZ)); g_x_last = "Here is one for you."; return;
    case XI_TEACH:          x_open_article((int)(millis() % X_NART)); g_x_last = "Have a look at this."; return;
    case XI_WIFI_ON:
        if (g_wifi_toggle) { g_x_last = "WiFi is already on."; g_x_orb = 0; }
        else { g_wifi_toggle = true; g_x_last = "WiFi coming up on the next sync."; g_x_orb = 1; }
        x_finish_info(); return;
    case XI_WIFI_OFF:
        if (!g_wifi_toggle) { g_x_last = "WiFi is already off."; g_x_orb = 0; }
        else { wifi_shutdown(); g_wifi_toggle = false; g_x_last = "WiFi off."; g_x_orb = 1; }
        x_finish_info(); return;
    case XI_BLE_ON:
        if (g_ble_toggle) { g_x_last = "Bluetooth is already on."; g_x_orb = 0; }
        else { btStart(); g_ble_toggle = true; g_x_last = "Bluetooth on."; g_x_orb = 1; }
        x_finish_info(); return;
    case XI_BLE_OFF:
        if (!g_ble_toggle) { g_x_last = "Bluetooth is already off."; g_x_orb = 0; }
        else { btStop(); g_ble_toggle = false; g_x_last = "Bluetooth off."; g_x_orb = 1; }
        x_finish_info(); return;
    case XI_GPS_ON:   g_x_gps = true;  g_x_last = "GPS marked on (no module wired on this board yet)."; g_x_orb = 1; x_finish_info(); return;
    case XI_GPS_OFF:  g_x_gps = false; g_x_last = "GPS off."; g_x_orb = 1; x_finish_info(); return;
    case XI_BEACON_ON:  g_x_beacon = true;  g_x_last = "Beacon marked on (virtual until hardware is fitted)."; g_x_orb = 1; x_finish_info(); return;
    case XI_BEACON_OFF: g_x_beacon = false; g_x_last = "Beacon off."; g_x_orb = 1; x_finish_info(); return;
    case XI_SHOW_BATTERY: {
        if (g_batt_pct < 0) g_x_last = "I cannot read the battery right now.";
        else { char b[64]; snprintf(b, sizeof(b), "Battery at %d%%%s.", g_batt_pct,
                                    g_charging ? " and charging" : ""); g_x_last = b; }
        g_x_orb = 1; x_finish_info(); return;
    }
    case XI_SHOW_STATUS: {
        char b[128];
        snprintf(b, sizeof(b), "Battery %d%%%s  -  WiFi %s  -  BLE %s",
                 g_batt_pct < 0 ? 0 : g_batt_pct, g_charging ? " (chg)" : "",
                 g_wifi_connected ? "on" : "off", g_ble_toggle ? "on" : "off");
        g_x_last = b; g_x_orb = 1; x_finish_info(); return;
    }
    default: g_x_last = xrand(X_R_UNK, 3); g_x_orb = 2; x_finish_info(); return;
    }
}
static void x_process(String raw) {
    String in = raw; in.toLowerCase(); in.trim();
    if (in.length() == 0) { g_x_last = xrand(X_R_UNK, 3); g_x_orb = 2; x_finish_info(); return; }
    // knowledge-topic triggers
    static const char *trigs[] = { "what is", "what's", "whats", "what are",
                                   "tell me about", "explain", "define", "teach me about" };
    int tp = -1;
    for (unsigned i = 0; i < sizeof(trigs)/sizeof(trigs[0]); i++) {
        int idx = in.indexOf(trigs[i]);
        if (idx >= 0) { tp = idx + (int)strlen(trigs[i]); break; }
    }
    if (tp >= 0) {
        String topic = in.substring(tp); topic.replace("?", " "); topic.trim();
        int a = x_find_article(topic.length() ? topic : in);
        if (a >= 0) { x_open_article(a); g_x_last = "Here is what I have."; return; }
        g_x_last = "I do not have that in my library yet."; g_x_orb = 2; x_finish_info(); return;
    }
    String t[12]; int n = x_tokenize(in, t, 12);
    int ti; int ts = x_best_tmpl(t, n, ti);
    int ai; int as = x_find_app_tok(t, n, ai);
    bool launchVerb = x_tok_has(t, n, "launch") || x_tok_has(t, n, "start") ||
                      x_tok_has(t, n, "run") || x_tok_has(t, n, "open");
    if (as > 0 && (as > ts || (launchVerb && as >= ts))) { x_launch_app(ai); return; }
    if (ts >= 1 && ti >= 0) { x_do(kTmpl[ti].intent); return; }
    if (as > 0) { x_launch_app(ai); return; }
    g_x_last = xrand(X_R_UNK, 3); g_x_orb = 2; x_finish_info();
}

// ---- enter / exit / swipe --------------------------------------------------
static void xander_enter() {
    x_stats_load();
    g_xander_active = true;
    current_screen  = SCREEN_FACE;   // exit always returns to the watch face
    g_x_top = 1; g_x_console = false; g_x_orb = 0;
    g_x_puz_view = 0; g_x_kn_view = 0; g_x_kn_cat = "";
    g_x_last = xrand(X_R_GREET, 4);
    // The real build happens in loop() -> xander_service().  This keeps us out
    // of the LVGL event that may have called us (a Settings card tap) and lets
    // us free the launcher screens before allocating Xander's views.
    g_x_action = XA_ENTER;
    USBSerial.println("[XANDER] enter requested");
}
static void xander_exit() {
    g_xander_active = false;
    g_x_action = XA_EXIT;            // rebuild launcher + fade to face in loop()
    USBSerial.println("[XANDER] exit requested");
}

// ---- deferred Xander worker (called every loop(), NEVER from an event) ------
static void xander_free_launcher_screens() {
    // Reclaim the LVGL object pool the launcher's heavy screens hold while
    // Xander is on top.  FACE is kept (we return to it) and we never delete the
    // screen that is active right now.
    lv_obj_t *act = lv_scr_act();
    for (int i = 0; i < NUM_SCREENS; i++) {
        if (i == SCREEN_FACE) continue;
        if (screens[i] && screens[i] != act) {
            lv_obj_del(screens[i]);
            screens[i] = nullptr;
        }
    }
}
static void xander_rebuild_launcher_screens() {
    if (!screens[SCREEN_APPS])     build_screen_apps();
    if (!screens[SCREEN_SETTINGS]) build_screen_settings();
    if (!screens[SCREEN_BATTERY])  build_screen_battery();
}
static void xander_service() {
    // Delayed 2nd free pass: frees whatever screen was active at enter time
    // (e.g. Settings) once Xander has become the active screen.
    if (g_x_free_at && millis() >= g_x_free_at) {
        g_x_free_at = 0;
        if (g_xander_active) xander_free_launcher_screens();
    }
    if (g_x_action == XA_NONE) return;
    XAction act = g_x_action;
    g_x_action = XA_NONE;

    if (act == XA_ENTER) {
        // Free what we safely can BEFORE building so the pool has headroom.
        xander_free_launcher_screens();
        if (!g_x_scr) g_x_scr = x_new_screen();
        lv_obj_clean(g_x_scr);
        g_x_resp_lbl = nullptr;
        x_build_into(g_x_scr);
        lv_scr_load_anim(g_x_scr, LV_SCR_LOAD_ANIM_FADE_ON, 160, 0, false);
        g_x_free_at = millis() + 240;   // free the (now-inactive) enter screen next
    } else if (act == XA_RENDER) {
        if (g_xander_active) {
            if (!g_x_scr) g_x_scr = x_new_screen();
            bool was_active = (lv_scr_act() == g_x_scr);
            lv_obj_clean(g_x_scr);      // reference pattern: clear + redraw in place
            g_x_resp_lbl = nullptr;
            x_build_into(g_x_scr);
            if (!was_active)
                lv_scr_load_anim(g_x_scr, LV_SCR_LOAD_ANIM_FADE_ON, 90, 0, false);
        }
    } else if (act == XA_EXIT) {
        // Clear Xander's widgets FIRST (frees the pool), then rebuild the
        // launcher screens and fade back to the face.  g_x_scr is kept (empty)
        // for reuse on the next enter.
        if (g_x_scr) lv_obj_clean(g_x_scr);
        g_x_resp_lbl = nullptr;
        xander_rebuild_launcher_screens();
        current_screen = SCREEN_FACE;
        lv_scr_load_anim(screens[SCREEN_FACE], LV_SCR_LOAD_ANIM_FADE_ON, 160, 0, false);
    }
}
static void xander_handle_swipe(SwipeEvent ev) {
    if (!g_xander_active) return;
    if (g_x_console) { if (ev == SWIPE_DOWN) { g_x_console = false; x_render(); } return; }
    if (ev == SWIPE_DOWN) { xander_exit(); return; }
    if (ev == SWIPE_LEFT || ev == SWIPE_RIGHT) {
        // first pop any open sub-view before moving the top-level ring
        bool popped = false;
        if (g_x_top == 0 && g_x_puz_view > 0) { g_x_puz_view = 0; popped = true; }
        else if (g_x_top == 2 && g_x_kn_view > 0) {
            if (g_x_kn_view == 2) { if (g_x_kn_more) g_x_kn_more = false; else g_x_kn_view = 1; }
            else g_x_kn_view = 0;
            popped = true;
        }
        if (popped) { x_render(); return; }
        int ni = g_x_top + (ev == SWIPE_LEFT ? -1 : 1);
        if (ni < 0) ni = 0; if (ni > 2) ni = 2;
        if (ni != g_x_top) {
            g_x_top = ni;
            if (ni == 0) g_x_puz_view = 0;
            if (ni == 2) { g_x_kn_view = 0; g_x_kn_cat = ""; }
            g_x_orb = 0; g_x_last = "";
            x_render();
        }
    }
}

// ---- Settings -> Xander subview --------------------------------------------
static lv_obj_t *g_xset_scr = nullptr;
static void xset_back_cb(lv_event_t *) {
    if (millis() - lastSwipeMs < CLICK_AFTER_SWIPE_MS) return;
    lv_scr_load_anim(screens[SCREEN_SETTINGS], LV_SCR_LOAD_ANIM_FADE_ON,
                     SCREEN_SWITCH_ANIM_MS, 0, true);
    current_screen = SCREEN_SETTINGS;
    g_xset_scr = nullptr;
}
static void xset_voice_cb(lv_event_t *e) {
    if (millis() - lastSwipeMs < CLICK_AFTER_SWIPE_MS) return;
    g_x_voice = lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED); x_settings_save();
}
static void xset_aon_cb(lv_event_t *e) {
    if (millis() - lastSwipeMs < CLICK_AFTER_SWIPE_MS) return;
    g_x_alwayson = lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED); x_settings_save();
}
static void xset_blim_cb(lv_event_t *e) {
    if (millis() - lastSwipeMs < CLICK_AFTER_SWIPE_MS) return;
    g_x_battlim = lv_dropdown_get_selected(lv_event_get_target(e)); x_settings_save();
}
static void xset_reset_cb(lv_event_t *) {
    if (millis() - lastSwipeMs < CLICK_AFTER_SWIPE_MS) return;
    g_xs_solved = g_xs_correct = g_xs_hints = g_xs_streak = g_xs_best = g_xs_hard = 0;
    x_stats_save();
    show_xander_settings();   // rebuild to reflect zeros
}
static void show_xander_settings() {
    x_stats_load();
    g_xset_scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(g_xset_scr, lv_color_hex(theme_bg()), 0);
    lv_obj_set_style_bg_opa(g_xset_scr, LV_OPA_COVER, 0);
    lv_obj_clear_flag(g_xset_scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *h1 = lv_label_create(g_xset_scr);
    lv_label_set_text(h1, "Xander");
    lv_obj_set_style_text_color(h1, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(h1, &lv_font_montserrat_28, 0);
    lv_obj_align(h1, LV_ALIGN_TOP_LEFT, 24, 22);
    lv_obj_t *h2 = lv_label_create(g_xset_scr);
    lv_label_set_text(h2, "companion settings");
    lv_obj_set_style_text_color(h2, lv_color_hex(0x666666), 0);
    lv_obj_set_style_text_font(h2, &lv_font_montserrat_12, 0);
    lv_obj_align(h2, LV_ALIGN_TOP_LEFT, 24, 58);
    lv_obj_t *list = lv_obj_create(g_xset_scr);
    lv_obj_set_size(list, LCD_WIDTH - 30, LCD_HEIGHT - 100);
    lv_obj_align(list, LV_ALIGN_TOP_MID, 0, 88);
    lv_obj_set_style_bg_opa(list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(list, 0, 0);
    lv_obj_set_style_pad_all(list, 6, 0);
    lv_obj_set_style_pad_row(list, 10, 0);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(list, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START);
    lv_obj_set_scroll_dir(list, LV_DIR_VER);
    char sb[96];
    snprintf(sb, sizeof(sb), "Solved %ld  Correct %ld  Best streak %ld  Hardest %ld/5",
             g_xs_solved, g_xs_correct, g_xs_best, g_xs_hard);
    lv_obj_t *stat = lv_label_create(list);
    lv_label_set_text(stat, sb);
    lv_obj_set_style_text_color(stat, lv_color_hex(theme_primary()), 0);
    lv_obj_set_style_text_font(stat, &lv_font_montserrat_14, 0);
    card_toggle(list, "Voice Response", "spoken replies (v1.5)", g_x_voice, xset_voice_cb);
    card_toggle(list, "Always-On Xander", "wake-word listening (v1.5)", g_x_alwayson, xset_aon_cb);
    card_dropdown(list, "Battery Limit", "pause always-on below",
                  "20%\n30%\n40%\nNever", g_x_battlim, xset_blim_cb);
    card_action(list, "Reset Puzzle Stats", "clears all counters",
                LV_SYMBOL_TRASH "  Reset", theme_danger(), xset_reset_cb);
    card_action(list, "Back", "return to settings",
                LV_SYMBOL_LEFT "  Back", theme_primary(), xset_back_cb);
    lv_scr_load_anim(g_xset_scr, LV_SCR_LOAD_ANIM_FADE_ON, SCREEN_SWITCH_ANIM_MS, 0, false);
}

// ---- settings-card entry points --------------------------------------------
static void xander_open_home_cb(lv_event_t *) {
    if (millis() - lastSwipeMs < CLICK_AFTER_SWIPE_MS) return;
    xander_enter();
}
static void xander_open_settings_cb(lv_event_t *) {
    if (millis() - lastSwipeMs < CLICK_AFTER_SWIPE_MS) return;
    show_xander_settings();
}
// ############################################################################
// #                        END XANDER v1.0 MODULE                            #
// ############################################################################

void setup() {
    USBSerial.begin(115200);

    // Are we coming back from a deep sleep (Sleep Now)?  If so, the internal
    // RTC still has the wall clock, so skip splash + WiFi for a fast wake.
    bool from_deep_sleep =
        (esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_EXT1);

    USBSerial.printf("\n=== LauncherOS v7.0 (merged: themes + card settings + styles + carousel loader) ===  %s\n",
                     from_deep_sleep ? "(wake from deep sleep)" : "(cold boot)");
    // v7.0: pull persisted preferences BEFORE we build any screen so the
    // very first paint uses the right theme + launcher style.
    theme_load_from_nvs();
    launcher_style_load_from_nvs();
    // v6.0: re-anchor the bootloader on ourselves so any loaded app that
    // reboots (intentionally, via crash, or via power-cycle) comes back
    // to LauncherOS instead of into the last-flashed OTA slot.
    anchor_launcher_as_boot();
    // v6.1: rehydrate the wall clock from NVS *before* anything else --
    // this way the splash + face show a real time even on a cold boot
    // with no WiFi, instead of "00:00".
    time_restore_from_nvs();
    g_gmt_offset_sec = g_tp_gmt;
    if (g_tp_valid) g_ntp_synced = true;


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
    Wire.setClock(400000);   // v6.3: fast-mode I2C cuts every touch + PMU
                             // transaction to ~1/4 the time, letting the
                             // CPU return to idle (or light sleep) sooner.

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
    // LVGL 8.4 partial draw buffer: 60-line RGB565 strip (lv_color_t units).
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
        time_apply_tz(g_gmt_offset_sec);
        time_publish("wifi cfg", false);
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
        splash_progress(88, "categories");       load_categories_txt();
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
    build_screen_settings();
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

        // v1.0-XANDER gesture routing.  Xander is a hidden layer BENEATH the
        // clock face: swipe UP from the face to enter, swipe DOWN anywhere in
        // Xander to leave.  While Xander owns the screen it consumes every
        // gesture (left/right navigate Puzzles<->Home<->Knowledge).  On the
        // normal launcher screens the original horizontal nav is untouched.
        if (g_xander_active) {
            xander_handle_swipe(ev);
        } else if (current_screen == SCREEN_FACE && ev == SWIPE_UP) {
            xander_enter();
        } else if (ev == SWIPE_LEFT || ev == SWIPE_RIGHT) {
            if (current_screen == SCREEN_APPS && g_launcher_style == LS_COVERFLOW &&
                g_cf_count > 1) {
                if (ev == SWIPE_LEFT)  cf_goto(g_cf_index + 1, true);
                if (ev == SWIPE_RIGHT) cf_goto(g_cf_index - 1, true);
            } else {
                if (ev == SWIPE_LEFT)  goto_screen(current_screen + 1);
                if (ev == SWIPE_RIGHT) goto_screen(current_screen - 1);
            }
        }
    }

    // Xander builds/redraws/tears-down its screen here -- OUTSIDE the LVGL
    // event + swipe handling above -- so it can safely clean widgets and free
    // the launcher screens without freeing something mid-event.
    xander_service();

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
    // 3s/15s tick - refresh battery readings (PMU).  Battery % barely
    // moves in 3 s on a real pack, but it climbs visibly while charging,
    // so we poll fast (5 s) when on USB and slow (15 s) on battery.
    // Each PMU read is an I2C round-trip, so this alone removes ~270
    // CPU wakeups per hour while idle.
    static uint32_t tB = 0;
    uint32_t batt_period = g_charging ? 5000U : 15000U;
    if (now - tB > batt_period) {
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
    //   * the BOOT button pulls GPIO0 LOW   (ext1 wake -- INSTANT, this
    //     preserves the user's "boot button always wakes me" behaviour)
    //   * the LIGHT_SLEEP_POLL_MS timer fires so we can poll TP_INT
    //     (FT3168 IRQ -- not RTC-capable on every S3 variant, so polled)
    if (screenIsOff) {
        const uint64_t WAKE_MASK = (1ULL << 0);             // BOOT button only
        esp_sleep_enable_ext1_wakeup(WAKE_MASK, ESP_EXT1_WAKEUP_ANY_LOW);
        esp_sleep_enable_timer_wakeup((uint64_t)LIGHT_SLEEP_POLL_MS * 1000ULL);
        esp_light_sleep_start();

        esp_sleep_wakeup_cause_t why = esp_sleep_get_wakeup_cause();

        if (why == ESP_SLEEP_WAKEUP_EXT1) {
            // BOOT button pressed -- INSTANT screen-on, no touch required.
            // This is the explicit "current boot sequence" wake path the
            // user wants preserved.
            FT3168->IIC_Interrupt_Flag = true;
            lastTouchMs = millis();
            screen_on();
        } else if (why == ESP_SLEEP_WAKEUP_TIMER) {
            // Timer tick -- quickly check if the touch IRQ line is low
            // (FT3168 pulls TP_INT LOW while a finger is on the screen).
            if (digitalRead(TP_INT) == LOW) {
                FT3168->IIC_Interrupt_Flag = true;
                lastTouchMs = millis();
                screen_on();
            }
        }
        return;   // skip the delay() below -- we already slept
    }
#endif

    // v6.3 adaptive idle.  LVGL tells us how many ms until its next timer
    // fires; we sleep up to that long (capped) so we're not busy-looping
    // at 5 ms while the UI is doing nothing.  Touch + PWR button still get
    // polled fast enough because both fire IRQs that the LVGL pump above
    // already serviced this iteration.  Cap at 30 ms so animations + 1 Hz
    // tick stay smooth even if LVGL reports a huge interval.
    uint32_t idle_ms = lv_timer_handler();
    if (idle_ms > 30) idle_ms = 30;
    if (idle_ms < 2)  idle_ms = 2;
    delay(idle_ms);
}
