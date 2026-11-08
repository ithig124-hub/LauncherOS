// ============================================================================
//  LauncherOS.ino   --   DualCore Watch launcher  (LVGL 8.3 / 8.4)
//  ----------------------------------------------------------------------------
//  ----------------------------------------------------------------------------
//  v1.2-XANDER -- PUSH-TO-TALK (one press, executes by itself).
//      * press BOOT anywhere in the launcher -> the mic opens and ESP_SR
//        MultiNet listens in COMMAND mode (no wake word needed)
//      * the match runs on its own: immediately after a "HEARD:" flash, or
//        when the ~6 s window closes if it heard nothing
//      * a second press only shortens the wait; the 6 s cap guarantees a
//        session can never hold the mic
//      * needs the 'model' partition in partitions.csv and srmodels.bin
//        flashed at 0x910000; without it voice disables itself and the
//        rest of the launcher is unaffected
//  ----------------------------------------------------------------------------
//  v1.1-XANDER -- VOICE-READY COMMAND LAYER (this pass).
//      * Xander's console now ACTS on its commands.  Every intent in the
//        voice spec (clock, brightness, radios, battery, memory, flash, SD,
//        system, restart/shutdown) calls a real launcher function instead
//        of printing a canned sentence.
//      * All ten OS .bin names are launchable from voice-shaped phrases:
//        "open AstroOS", "spotifyos", "go to dual link".  Names resolve
//        against the SD scan from boot, so an app that is not on the card
//        gets an honest answer instead of a dead tap or a bad flash.
//      * Mood handles on top of the names: "it's go time" -> SportOS,
//        "I'm bored" -> GameOS, "boot some tunes" -> SpotifyOS,
//        "take me to the stars" -> AstroOS, and ~60 more in kOs.
//      * kPhrase is written in the shape a recogniser returns (plain words,
//        no punctuation), so a speech front-end only has to hand over text.
//      * 12/24-hour clock, brightness and the wake level persist under
//        "dcw" (t24 / bright).  screen_on() now restores the saved level
//        instead of snapping back to 140 -- that was a latent bug.
//
//      NOTE ON AUDIO: the wake-word model is NOT compiled into the launcher.
//      The factory slot is 3 MB and the EI WATCH model alone is 2.5 MB, so
//      the voice front-end stays in the separate Xander sketch (Xander/);
//      this layer is what that front-end will hand recognised intents to.
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

// v1.2 push-to-talk. These pull in ESP-IDF headers via sdkconfig, so they go
// LAST: everything above is already compiled against the Arduino macros it
// expects, and the IDF includes cannot disturb LVGL/GFX.
#include "ESP_I2S.h"
#include "ESP_SR.h"
#include "esp_heap_caps.h"

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

// v1.6 wake word.  The Edge Impulse "WATCH" model (installed as
// WATCH_inferencing 1.0.1) supplies the one thing MultiNet cannot: a reliable
// "Xander".  Class index is resolved by name at boot, so a re-export with a
// different class order still works.  No .ino of its own -- it is a library.
#include <WATCH_inferencing.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <esp_ota_ops.h>
#include <esp_partition.h>
#include <esp_image_format.h>
#include <esp_app_format.h>      // ESP_APP_DESC_MAGIC_WORD
#include <esp_task_wdt.h>        // fed during the swap flash loops
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
// either (a) BOOT (GPIO0) or the side POWER button (GPIO10, the AXP2101 IRQ
// line) via ext1 -- instant wake, no touch and no poll needed -- or (b) a
// 250 ms timer tick so we can poll the FT3168 TP_INT line (GPIO38 is not
// RTC-IO-capable on every S3 variant, so we don't trust ext1 for it).  250 ms
// is the sweet spot: users can't perceive the touch-wake latency but standby
// draw drops from ~25 mA -> ~2-4 mA average.
#define LIGHT_SLEEP_WHEN_OFF    1
#define LIGHT_SLEEP_POLL_MS    500     // v2.11: 250 -> 500 ms; the dark tick was the ~3 mA

// v2.13d: peripherals that were awake for no reason.
//   * There is NO IMU driver in this sketch -- the only "IMU" strings in it are
//     inside two knowledge articles -- so there is nothing to switch off there.
//   * Bluetooth: btStop() in setup().  WiFi: WIFI_OFF until a sync is asked
//     for.  Mic / speaker codec: started only for a PTT window (and now powered
//     back down when the window closes, see es7210_sleep()).
//   * The SD card was the exception: mounted at boot and left mounted through
//     every dark phase.  Set this to 0 to keep it mounted (old behaviour) if a
//     card ever fails to come back after a wake.
#define SD_SLEEP_WHEN_DARK      1

// v2.0 boot protocol types.  Same placement rule as PmState just below: the
// Arduino builder injects its generated prototypes immediately before the first
// function definition, and bk_rec_save()/bk_install()/bk_rollback() take a
// BootRec &, so this type has to be nameable above that point.  Left down beside
// the engine it fails the whole sketch with "'BootRec' does not name a type".
#define BK_NS        "dualboot"
#define BK_SV        1
#define BK_ATT_OTA   2            // budget for otadata-only phases
#define BK_ATT_WRITE 3            // budget for write phases

enum : uint8_t { BK_IDLE = 0, BK_PREPARED, BK_ARMED, BK_INSTALLING, BK_INSTALLED };
enum : uint8_t {
    BK_WHY_NONE = 0, BK_WHY_NO_CARD, BK_WHY_NO_IMAGE, BK_WHY_BAD_IMAGE,
    BK_WHY_BATTERY, BK_WHY_CLONE_UNBOOTABLE, BK_WHY_ORPHAN_HOME,
    BK_WHY_WRITE, BK_WHY_VERIFY, BK_WHY_BOOTSLOT,
    BK_WHY_RB_VERIFY, BK_WHY_RB_WRITE, BK_WHY_RESUME
};

//  Forward declarations for the Xander record types.  The Arduino builder
//  splices the generated prototypes in above the first function definition,
//  so every type named in a signature has to be visible this early even though
//  the full definitions sit with the article/puzzle library further down.
struct XPuzzle;
struct XArticle;

struct BootRec {
    uint8_t  phase = BK_IDLE;
    uint32_t tx = 0, ilen = 0, icrc = 0, clen = 0, ccrc = 0;
    uint8_t  att = 0, why = BK_WHY_NONE;
    String   path;
};

// v1.6 power states.  Declared here -- above the first function definition --
// because the Arduino builder injects generated prototypes at that point, and
// any prototype that mentions PmState has to see this enum first.
enum PmState {
    PM_ACTIVE = 0,
    PM_IDLE,            // reserved: v2.1 never enters it (no auto-dim)
    PM_NIGHT,
    PM_CHARGING,
    PM_LOW_BATT,
    PM_CRITICAL,
    PM_SCREEN_OFF,
    PM_STATE_COUNT
};

// Profiles live up here for the same reason: pm_prof() returns this type, so
// its generated prototype has to be able to name it.
enum PmProfileId { PMP_SAVER = 0, PMP_BALANCED = 1, PMP_PERFORMANCE = 2, PMP_COUNT = 3 };
struct PmProfileDef {
    const char *name;
    uint8_t  idle_bright;
    uint8_t  night_bright;
    uint8_t  low_bright;
    uint16_t cpu_active_mhz;
    uint16_t cpu_idle_mhz;
    uint16_t cpu_off_mhz;
    bool     wifi_ok;
    // v1.9: dark-screen time before the SoC parks in deep sleep.  0 = never.
    // Deep sleep costs a full reboot to answer (see pm_auto_deep), so the
    // performance profile does not do it.
    uint32_t deep_after_ms;
};
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
// v1.6 power manager rows
static void cards_on_power_profile_cb(lv_event_t *e);
static void cards_on_timeout_cb(lv_event_t *e);
static void cards_on_night_cb(lv_event_t *e);
static void cards_on_night_sched_cb(lv_event_t *e);
static void cards_on_deep_cb(lv_event_t *e);
static void cards_open_wallpaper_cb(lv_event_t *e);
static void cards_open_developer_cb(lv_event_t *e);
static void cards_factory_reset_cb(lv_event_t *e);
static void cards_deep_sleep_cb(lv_event_t *e);
static void cards_reconnect_wifi_cb(lv_event_t *e);
static void cards_resync_sd_cb(lv_event_t *e);          // v2.8 remount + rescan
static void ptt_flash(const String &msg, uint32_t holdMs);   // toast, defined below
static bool sd_mount();                                 // v2.8, defined above setup()
static void sd_sleep(const char *why);                  // v2.13d, defined below
static void sd_wake();                                  // v2.13d
static const char *swap_boot_name(const esp_partition_t *p);   // v2.10, defined below
static const esp_partition_t *bk_home();                       // v2.10
static const esp_partition_t *bk_transit();                    // v2.10
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

#define SCREEN_OFF_TIMEOUT_MS   10000     // v2.11: 10 s of no touch -> screen off
#define WAKE_GRACE_MS            2000     // for 2 s after wake the auto-timeout is suspended
#define CLICK_AFTER_SWIPE_MS      180     // ignore clicks this soon after a swipe.  v2.14.3: stamping moved into the touch read, so this now covers the flick itself rather than the next tap -- 350 ms of dead input was the cost of guarding the wrong event.
// --- MiniOS-parity swipe tuning (v5.6) ------------------------------------
// MiniOS dispatches NAV events the instant the finger releases -- no cooldown,
// no wake suppression, no rounder pass. We mirror that exactly here.
#define SWIPE_THRESHOLD_PX         25     // pixels of horizontal travel to count (matches MiniOS)
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
//
// v2.14.2: this is the pin the AXP2101 drives for the side button.  The button
// itself is the PMU's PWRKEY, and the only way the sketch ever hears about it is
// by reading the IRQ status over I2C -- which needs a running CPU.  That made
// the power button useless in deep sleep, where there is no CPU to poll: the
// watch sat dark until you found BOOT.  GPIO10 is RTC-capable on the S3 (RTC
// GPIOs are 0..21), so it can wake the SoC by itself, the same way GPIO0 does.
#define PWR_BTN_PIN  10
#define PWR_WAKE_PIN PWR_BTN_PIN
// Both lines are active LOW -- but they are NOT equally trustworthy.
//
// BOOT (GPIO0) is an unconditional wake input: it is always in the mask, in
// both sleep stages, and no runtime condition can take it out.
//
// GPIO10 is the AXP2101's open-drain IRQ.  Putting a LOW-level pin in an ext1
// mask is only safe while that pin idles HIGH: a line stuck LOW makes ANY_LOW
// fire immediately, on every entry, which spins the SoC out of sleep and looks
// exactly like "BOOT stopped waking it".  So the pin is read at rest before it
// is ever trusted (wake_mask()), and a wake with no PKEY event behind it
// demotes the mask back to BOOT-only for the rest of the session.
#define WAKE_PIN_MASK ((1ULL << 0) | (1ULL << PWR_WAKE_PIN))   // BOOT + POWER
#define WAKE_BOOT_ONLY (1ULL << 0)

static int g_wake_sel = -1;              // -1 unknown, 0 BOOT only, 1 BOOT + POWER

static void wake_drop_pwr(const char *why) {
    if (g_wake_sel == 0) return;
    g_wake_sel = 0;
    USBSerial.printf("[pm] POWER wake disabled (%s) -- BOOT still wakes it\n",
                     why ? why : "untrusted line");
}
// Read once, at the first sleep.  Anything but a stable HIGH means we leave the
// pin out of the mask; BOOT is unaffected either way.
static uint64_t wake_mask() {
    if (g_wake_sel < 0) {
        pinMode(PWR_BTN_PIN, INPUT_PULLUP);
        delay(2);
        bool high = (digitalRead(PWR_BTN_PIN) == HIGH);
        for (int i = 0; i < 4 && high; i++) {   // must HOLD high, not just blip
            delay(1);
            if (digitalRead(PWR_BTN_PIN) != HIGH) high = false;
        }
        g_wake_sel = high ? 1 : 0;
        USBSerial.printf("[pm] wake pins: BOOT always%s (gpio%d idles %s)\n",
                         g_wake_sel ? " + POWER" : "",
                         PWR_BTN_PIN, g_wake_sel ? "HIGH" : "LOW - not armed");
    }
    return (g_wake_sel == 1) ? WAKE_PIN_MASK : WAKE_BOOT_ONLY;
}

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

// Command hold.  A phrase can start something that outlives a single loop()
// pass -- copy an app from the card into OTA and reboot, run a WiFi sync,
// restart -- and the 15 s idle countdown must not fire underneath it.  While a
// hold is live the countdown is paused (loop() refreshes lastTouchMs), and
// screen_hold_release() restarts it from zero so the result is never painted
// onto a panel that went dark half way through.
static uint32_t g_screenHoldUntil = 0;          // millis deadline, 0 = no hold
static inline bool screen_held(uint32_t now) {
    return g_screenHoldUntil && (int32_t)(g_screenHoldUntil - now) > 0;
}
static void screen_hold(uint32_t ms)         { g_screenHoldUntil = millis() + ms; }
static void screen_hold_release()            { g_screenHoldUntil = 0; lastTouchMs = millis(); }

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

// v1.1: the level screen_on() restores.  g_cur_brightness tracks the panel
// LIVE (so it reads 0 while the screen is off) and is therefore unusable as a
// wake target -- waking would fade up to black.  Persisted under "dcw"/"bright"
// by the Settings slider and by Xander's brightness commands.
static uint8_t g_brightness_target = DEFAULT_BRIGHTNESS;

// ============================================================================
//  v1.6 POWER MANAGER  --  configuration + declarations
//  ----------------------------------------------------------------------------
//  One state machine owns every power decision in the launcher: panel
//  brightness, CPU clock, screen timeout, WiFi permission and the battery
//  policy.  Nothing else changes the clock or the panel on a schedule, so two
//  features can never fight over the same hardware knob.
//
//  States, highest priority first:
//      CRITICAL   <= PM_BATT_CRIT_PCT on battery  -> minimum brightness,
//                                                    WiFi blocked, one warning
//      LOW_BATT   <= PM_BATT_LOW_PCT  on battery  -> capped brightness,
//                                                    WiFi blocked, one warning
//      SCREEN_OFF                                  -> panel dark, light sleep
//      NIGHT      manual toggle or 22:00-07:00     -> dim profile
//      CHARGING                                    -> normal profile, no
//                                                    battery-derived limits
//      IDLE       no input for a few seconds       -> profile's idle level
//      ACTIVE                                      -> the user's own level
//
//  User activity is `lastTouchMs` and nothing else.  It is stamped by touch
//  (press and release), the BOOT button, the side PWR button, a recognised
//  Xander command and every screen_on(), so a redraw, a sensor sample or a
//  battery poll can never keep the panel awake on its own.
//
//  The CPU table deliberately keeps the awake clock at 80 MHz for Balanced
//  and Power Saver: that is what the firmware already ran at, and raising it
//  would spend battery to make an interface nobody called slow feel faster.
//  Only the Performance profile pays for 240/160 MHz.
// ============================================================================

// ---- power configuration ---------------------------------------------------
#define PM_BATT_LOW_PCT            20    // % at which LOW_BATT starts
#define PM_BATT_CRIT_PCT            7    // % at which CRITICAL starts
#define PM_BATT_OFF_PCT             2    // % at which the watch protects the cell
#define PM_BATT_HYST                3    // % of hysteresis before a level clears
#define PM_BATT_OFF_CONFIRMS        2    // consecutive readings needed to sleep
#define PM_BATT_OFF_SETTLE_MS   60000UL  // ...and they must be this far apart
#define PM_RAMP_INTERVAL_MS        24    // brightness step period
#define PM_NOTICE_MS             2600    // how long a power notice stays up
#define PM_LOG_INTERVAL_MS    600000UL   // diagnostics line every 10 min
#define PM_ENERGY_DUMP_MS     1800000UL  // measured energy table every 30 min
#define PM_ENERGY_MAX_DPCT         25    // a bigger % step is a bad read, not drain
#define PM_NIGHT_START_HOUR        22
#define PM_NIGHT_END_HOUR           7
#define PM_QUIET_FACE               1    // hide the wallpaper in the dim states
#define PM_DARK_SAMPLE_MS      300000UL  // PMIC read while the panel is dark (5 min)
#define PM_LOW_HOLD_MS           8000    // screen timeout cap at LOW_BATT
#define PM_CRIT_HOLD_MS          5000    // ...and at CRITICAL

// v2.1: no auto-dim.  While the panel is lit it holds the user's brightness
// for the whole screen-on window and goes straight to black at the timeout,
// which is how the pre-v1.6 firmware behaved.  The dim step in the middle of
// that window read as the watch falling asleep mid-use, and the quiet face
// that came with it swapped the wallpaper out and back on every glance.  Both
// are gone; PM_IDLE below is now reserved rather than reached.

// ---- power states ----------------------------------------------------------
// (enum PmState lives up in POWER-SAVING TUNABLES -- see the note there.)

// ---- profiles --------------------------------------------------------------
// (PmProfileId / PmProfileDef are declared in POWER-SAVING TUNABLES; the table
// itself is safe here because no generated prototype mentions it.)
//
// Brightness caps are absolute panel levels (0-255), applied as
// min(user_target, cap) so a user who dimmed to 60 keeps 60 in every state.
static const PmProfileDef kPmProfiles[PMP_COUNT] = {
    // name             idle night low   cpu: act  idle  off  wifi  deep-after
    // v2.1: every profile runs the SoC at 240 MHz while the panel is lit.  The
    // 80 MHz active clock was the lag -- LVGL and the QSPI flush both ride the
    // CPU, so a 3x downclock is a visibly choppy UI -- and the panel (~145 mA)
    // dominates the draw anyway, which makes the 10-15 mA it saved a bad trade.
    { "Power Saver",     45,   25,  30,         240,   80, 80,  false,   180000 },
    { "Balanced",        70,   35,  40,         240,   80, 80,  true,    180000 },
    { "Performance",    100,   50,  50,         240,  160, 80,  true,         0 },
};

// ---- runtime status --------------------------------------------------------
static PmState   g_pmState       = PM_ACTIVE;
static bool      g_pmReady       = false;
static uint32_t  g_pmStateSince  = 0;                       // millis of last transition
static uint32_t  g_pmStateMs[PM_STATE_COUNT] = { 0 };       // dwell time per state
static uint32_t  g_pmTransitions = 0;
static uint8_t   g_pmProfileIdx  = PMP_BALANCED;
static bool      g_pmNightManual = false;                   // settings toggle
static bool      g_pmNightSched  = true;                    // 22:00-07:00 window
static uint16_t  g_pmTimeoutMs   = 10000;                   // screen-off countdown (v2.11: 10 s)
static uint8_t   g_pmBattLatch   = 0;                       // 0 none, 1 low, 2 critical
static uint32_t  g_pmBattSampleMs = 0;                      // freshness of g_batt_pct
static uint8_t   g_pmOffStreak   = 0;                       // PM_BATT_OFF_PCT confirmations

// ---- energy ledger ---------------------------------------------------------
// Drain is measured, not modelled: the AXP2101 percentage is read at every
// state transition, so each sample covers exactly ONE state and the mAh can be
// attributed without guessing.  (Reading on a fixed timer instead would smear
// ACTIVE and SCREEN_OFF into the same window -- a 145 mA state and a 3 mA one
// averaged together is worse than no number at all.)
//
// No extra I2C traffic while nothing happens: the poll below only runs on a
// window boundary, and a dark screen produces none until the wake.
static float    g_pmEnMah[PM_STATE_COUNT];                  // mAh attributed per state
static uint32_t g_pmEnMeasMs[PM_STATE_COUNT];               // time those mAh were measured over
static float    g_pmEnTotalMah   = 0.0f;
static int      g_pmEnLastPct   = -1;                       // -1 = re-baseline next read
static uint32_t g_pmEnLastMs    = 0;
static uint32_t g_pmEnSamples   = 0;
static uint32_t g_pmEnDropped   = 0;
static uint32_t g_pmEnDumpNext  = 0;
static uint32_t  g_pmOffFirstMs  = 0;
static bool      g_pmOffDone     = false;                   // never shut down twice
static uint32_t  g_pmRampNext    = 0;                       // next brightness step
static uint32_t  g_pmLogNext     = 0;
static uint8_t   g_pmPctAtBoot   = 0;
static uint32_t  g_pmBootMs      = 0;
static bool      g_pmMicBusy     = false;   // ESP-SR bring-up holds a 240 MHz boost
static bool      g_pmQuietFace   = false;   // dim state: wallpaper hidden (see refresh_wallpaper)
static bool      g_pmWpDirty     = false;   // wallpaper changed while the face was quiet
static bool      g_pmDeepOn      = true;    // settings: allow auto deep sleep
static bool      g_pmVoiceArmed  = false;   // wake word is listening; set by aon_service()
// v2.0: millis() at which the panel went dark, 0 while it is lit.  The auto
// deep-sleep clock runs off THIS rather than the touch timer, so the dark
// phase is exactly "10 minutes of continuous darkness": every screen_on()
// (tap, BOOT, PWR, a voice command, a battery alert) restarts the countdown,
// and only a panel that has been dark for the whole interval may park the SoC.
static uint32_t  g_pmDarkSince   = 0;

static const uint16_t kPmTimeouts[] = { 10000, 15000, 30000, 60000 };
#define PM_TIMEOUT_CHOICES ((int)(sizeof(kPmTimeouts) / sizeof(kPmTimeouts[0])))

// ---- forward declarations (definitions live beside screen_off/deep_sleep) --
static void        pm_init();
static void        pm_service(uint32_t now);
static void        pm_activity(const char *why);
static void        pm_note_battery(uint32_t now);
static bool        pm_wifi_allowed();
static void        pm_restore_cpu();
static void        pm_log(uint32_t now);
static int         pm_profile_index();
static int         pm_timeout_index();
static const char *pm_state_name(PmState s);
static uint8_t     pm_wake_brightness();

// v1.1: 12/24-hour clock, persisted under "dcw"/"t24".  24-hour stays the
// default so the face looks exactly as it always has until the user asks.
static bool    g_time_24h = false;

// v1.1: set once SD_MMC.begin() succeeds, so the Xander status answers can
// describe the card without re-probing it (a probe on a missing card stalls).
static bool    g_sd_ok = false;

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

// ---- v2.14.3: is this point over a vertically scrollable list? -------------
// A vertical drag that STARTS on a list belongs to the list -- the finger is
// scrolling it -- so it must not also change screen when it lifts.  Lists are
// tagged LV_OBJ_FLAG_USER_1 by x_list_in(); we hit-test the live widget tree
// instead of hand-tracking rectangles, which cannot go stale when a screen is
// rebuilt.  Two levels covers every list in this sketch (a list inside the
// screen, or inside one container).
static bool x_hit_scroll_list(lv_obj_t *o, const lv_point_t *p, int depth) {
    if (!o) return false;
    for (uint32_t i = 0; i < lv_obj_get_child_cnt(o); i++) {
        lv_obj_t *c = lv_obj_get_child(o, i);
        if (!c) continue;
        if (lv_obj_has_flag(c, LV_OBJ_FLAG_USER_1)) {
            lv_area_t a;
            lv_obj_get_coords(c, &a);
            // Plain bounds test, not lv_area_is_point_on(): that helper is
            // internal in LVGL 8.4 (it is _lv_area_is_point_on, and calling the
            // public-looking name is a compile error).  Four comparisons cost
            // nothing and cannot drift with the library version.
            if (p->x >= a.x1 && p->x <= a.x2 &&
                p->y >= a.y1 && p->y <= a.y2) return true;
        }
        if (depth < 2 && x_hit_scroll_list(c, p, depth + 1)) return true;
    }
    return false;
}
static bool point_on_scroll_list(int32_t px, int32_t py) {
    lv_point_t p;
    p.x = (lv_coord_t)px;
    p.y = (lv_coord_t)py;
    lv_obj_t *scr = lv_scr_act();
    return scr && x_hit_scroll_list(scr, &p, 0);
}

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
            // v2.14.3: NO time limit.  A swipe is a journey of more than 25 px
            // in one direction, not a velocity.  Gating on `dur` meant a slow,
            // deliberate drag -- the one people make when the UI feels laggy,
            // or when they are being careful -- was thrown away and then handed
            // to LVGL as a CLICK on whatever was under the finger.  Distance
            // and direction decide; how long you took does not.
            //
            // The one exception is vertical over a list: that drag is a scroll,
            // so it is left to LVGL and the screen stays put.  Horizontal always
            // navigates -- nothing in this UI scrolls sideways except the
            // carousel, which handles its own swipes.
            bool gestureSwipe = horiz || (vert && !point_on_scroll_list(ts_x, ts_y));

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
                // v2.14.3: stamp the post-swipe click-guard HERE, not in
                // loop().  LVGL dispatches this release (and any CLICKED it
                // implies) inside lv_timer_handler(), which runs BEFORE loop()
                // consumes g_pending_swipe -- so stamping it there was always
                // one step too late: the flick's own click went through and
                // launched whatever was under the finger, while the guard then
                // swallowed a legitimate tap for the next 350 ms.  Stamped
                // here, the guard covers exactly the release that caused it.
                lastSwipeMs = millis();
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

        // v2.8: THE SD CARD'S POWER RAIL.  The TF slot on this board is fed
        // from AXP2101 ALDO3, and nothing in this sketch ever enabled it --
        // so the card was unpowered and SD_MMC.begin() could not have found
        // it at any pin assignment.  DualBoot's init_pmu() enables it as
        // "SD VDD", and Waveshare's own 07_LVGL_SD_Test does the same.
        g_pmu.setALDO3Voltage(3300);
        g_pmu.enableALDO3();

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
#define WIFI_SD_MAX 8                      // networks honoured from config.txt
static WifiNet g_nets[WIFI_SD_MAX];

// ---- v1.9 saved networks (NVS) ---------------------------------------------
// A network typed in on the watch beats both the SD list and the hardcoded
// fallback: the user chose it on purpose, so it is the network they mean.
// Slot 0 is the most recently used.  Declared up here because the connect
// paths (wifi_connect_and_sync, batt_wifi_reconnect_all) read them.
#define WIFI_SAVED_MAX 4
static String g_ws_ssid[WIFI_SAVED_MAX];
static String g_ws_pass[WIFI_SAVED_MAX];
static int    g_ws_n      = 0;
static bool   g_ws_loaded = false;

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

// ---- v2.1 WATCH/wifi/config.txt -------------------------------------------
// Accepts BOTH styles people actually write, because the old parser understood
// only the first and silently read the second as nothing at all:
//
//     WIFI1_SSID=...    WIFI1_PASS=...        numbered keys
//     SSID=...          PASSWORD=...          repeated plain pairs
//                                             (the "Fusion Labs compatible"
//                                              file shipped on the card)
//
// That second style parsed to zero networks, so the file on the card was dead
// weight: the board fell back to "no SD networks" and tried the one hardcoded
// AP.  A watch with its own config.txt therefore could not join anything the
// hardcoded AP didn't cover, even though the radio saw the networks fine.
static bool wifi_placeholder_value(const String &v) {
    return !v.length() ||
           v == "YourNetworkName" || v == "YourPassword" ||
           v == "YourHomeNetwork" || v == "YourWorkNetwork" ||
           v == "YourSSID" || v == "changeme";
}
static void wifi_sd_add(const String &ssid, const String &pass) {
    if (wifi_placeholder_value(ssid)) return;        // template line: skip
    for (int i = 0; i < g_net_count; i++) {
        if (g_nets[i].ssid == ssid) {                // one entry per SSID
            if (!g_nets[i].pass.length() && pass.length()) g_nets[i].pass = pass;
            return;
        }
    }
    if (g_net_count >= WIFI_SD_MAX) return;
    g_nets[g_net_count].ssid = ssid;
    g_nets[g_net_count].pass = wifi_placeholder_value(pass) ? String("") : pass;
    g_net_count++;
}
static void parse_wifi_config() {
    g_net_count = 0;
    File f = SD_MMC.open("/WATCH/wifi/config.txt", FILE_READ);
    if (!f) {
        USBSerial.println("[wifi] WATCH/wifi/config.txt not found");
        return;
    }
    String pend_ssid = "";
    String pend_pass = "";
    bool   pend_have_pass = false;
    while (f.available()) {
        String line = f.readStringUntil('\n');
        line.trim();
        if (!line.length() || line.startsWith("#") || line.startsWith(";")) continue;
        int eq = line.indexOf('=');
        if (eq < 0) continue;
        String key = line.substring(0, eq); key.trim(); key.toUpperCase();
        String val = line.substring(eq + 1); val.trim();
        if (key == "GMT_OFFSET") { g_gmt_offset_sec = val.toInt() * 3600; continue; }
        if (key == "SSID") {                          // plain-pair style
            if (pend_ssid.length())
                wifi_sd_add(pend_ssid, pend_have_pass ? pend_pass : String(""));
            pend_ssid = val; pend_pass = ""; pend_have_pass = false;
            continue;
        }
        if (key == "PASSWORD" || key == "PASS") {
            pend_pass = val; pend_have_pass = true;
            if (pend_ssid.length()) { wifi_sd_add(pend_ssid, pend_pass); pend_ssid = ""; }
            continue;
        }
        if (key.startsWith("WIFI") && key.indexOf('_') > 0) {
            const bool is_ssid = key.endsWith("_SSID");
            const bool is_pass = key.endsWith("_PASS") || key.endsWith("_PASSWORD");
            if (!is_ssid && !is_pass) continue;
            if (is_ssid) {
                wifi_sd_add(val, String(""));
            } else {
                for (int i = g_net_count - 1; i >= 0; i--)
                    if (!g_nets[i].pass.length()) { g_nets[i].pass = val; break; }
            }
        }
    }
    if (pend_ssid.length())
        wifi_sd_add(pend_ssid, pend_have_pass ? pend_pass : String(""));
    f.close();
    USBSerial.printf("[wifi] config.txt: %d network(s)\n", g_net_count);
    for (int i = 0; i < g_net_count; i++)
        USBSerial.printf("[wifi]   %d: %s (%s)\n", i + 1, g_nets[i].ssid.c_str(),
                         g_nets[i].pass.length() ? "password" : "open");
}

// ---- v1.9 saved networks ("net" namespace) --------------------------------
static void wifi_saved_load() {
    if (g_ws_loaded) return;
    g_ws_loaded = true;
    g_ws_n = 0;
    Preferences p;
    p.begin("net", true);
    int n = p.getInt("n", 0);
    if (n < 0) n = 0;
    if (n > WIFI_SAVED_MAX) n = WIFI_SAVED_MAX;
    for (int i = 0; i < n; i++) {
        String k = String("s") + i, w = String("p") + i;
        String ss = p.getString(k.c_str(), "");
        String pw = p.getString(w.c_str(), "");
        if (ss.length()) { g_ws_ssid[g_ws_n] = ss; g_ws_pass[g_ws_n] = pw; g_ws_n++; }
    }
    p.end();
}
static void wifi_saved_store() {
    Preferences p;
    p.begin("net", false);
    p.putInt("n", g_ws_n);
    for (int i = 0; i < WIFI_SAVED_MAX; i++) {
        String k = String("s") + i, w = String("p") + i;
        p.putString(k.c_str(), i < g_ws_n ? g_ws_ssid[i] : String(""));
        p.putString(w.c_str(), i < g_ws_n ? g_ws_pass[i] : String(""));
    }
    p.end();
}
// Newest first, one entry per SSID, oldest dropped past WIFI_SAVED_MAX.
static void wifi_saved_put(const String &ssid, const String &pass) {
    if (!ssid.length()) return;
    wifi_saved_load();
    int at = -1;
    for (int i = 0; i < g_ws_n; i++) if (g_ws_ssid[i] == ssid) { at = i; break; }
    if (at == 0) {
        if (pass.length()) g_ws_pass[0] = pass;
    } else if (at > 0) {
        String ss = g_ws_ssid[at], pw = g_ws_pass[at];
        for (int i = at; i > 0; i--) {
            g_ws_ssid[i] = g_ws_ssid[i - 1];
            g_ws_pass[i] = g_ws_pass[i - 1];
        }
        g_ws_ssid[0] = ss;
        g_ws_pass[0] = pass.length() ? pass : pw;
    } else {
        int last = (g_ws_n < WIFI_SAVED_MAX) ? g_ws_n : (WIFI_SAVED_MAX - 1);
        for (int i = last; i > 0; i--) {
            g_ws_ssid[i] = g_ws_ssid[i - 1];
            g_ws_pass[i] = g_ws_pass[i - 1];
        }
        g_ws_ssid[0] = ssid;
        g_ws_pass[0] = pass;
        if (g_ws_n < WIFI_SAVED_MAX) g_ws_n++;
    }
    wifi_saved_store();
}
static void wifi_saved_erase(int i) {
    wifi_saved_load();
    if (i < 0 || i >= g_ws_n) return;
    for (int k = i; k + 1 < g_ws_n; k++) {
        g_ws_ssid[k] = g_ws_ssid[k + 1];
        g_ws_pass[k] = g_ws_pass[k + 1];
    }
    g_ws_n--;
    wifi_saved_store();
}
static bool wifi_join(const char *ssid, const char *pass, uint32_t timeout_ms) {
    // v2.10: this is the reason "sync" walked every saved network and joined
    // none of them even with a good AP in range.
    //
    // Two things were wrong with the old version:
    //   * it reused one station for the whole list, so after the first
    //     association failed the driver sat in a dead state and every later
    //     begin() ran against it -- the first failure poisoned the rest;
    //   * the POWER_SAVE block above had already dropped TX to 8.5 dBm and
    //     enabled modem sleep, which the vendor demo never does while
    //     connecting (its connectWifi() just applies the STA config and lets
    //     the reconnect run at full power).
    //
    // So every attempt now gets its own clean radio: off, back on, full power,
    // no power save, config, begin.  Power save is restored by the caller once
    // a link is up, because the point of lowering it is idle current, not the
    // handful of seconds a join takes.
    USBSerial.printf("[wifi] join '%s' (%u s budget)\n", ssid,
                     (unsigned)(timeout_ms / 1000));
    WiFi.persistent(false);
    WiFi.mode(WIFI_OFF);                 // full radio reset between candidates
    delay(120);
    WiFi.mode(WIFI_STA);
    WiFi.setSleep(WIFI_PS_NONE);         // modem sleep can stall the WPA2 exchange
    WiFi.setTxPower(WIFI_POWER_19_5dBm); // an 8.5 dBm PA is not enough to associate
    WiFi.disconnect(false, false);
    delay(40);
    WiFi.begin(ssid, pass);

    uint32_t t0 = millis();
    while (millis() - t0 < timeout_ms) {
        if (WiFi.status() == WL_CONNECTED) {
            USBSerial.printf("[wifi] joined %s ip=%s rssi=%d\n", ssid,
                             WiFi.localIP().toString().c_str(), (int)WiFi.RSSI());
            return true;
        }
        lv_timer_handler();              // keep the splash/screen animating
        delay(50);
    }
    // Log what the driver thought, so a log line says WHY instead of only
    // that it did not work (3 = NO_AP_FOUND, 4 = CONNECT_FAIL, 6 = DISCONNECTED).
    USBSerial.printf("[wifi] could not join %s - status %d, rssi %d\n",
                     ssid, (int)WiFi.status(), (int)WiFi.RSSI());
    WiFi.disconnect(false, false);
    WiFi.mode(WIFI_OFF);                 // leave nothing half-open for the next one
    delay(80);
    return false;
}

// ---- v2.1 one ordered credential list -------------------------------------
// Order is the user's intent: what they typed in on the watch, then everything
// in WATCH/wifi/config.txt, then the hardcoded fallback.  EVERY connect path
// walks this whole list.  The old code treated the saved list as either/or with
// the SD file ("if (g_ws_n) ... else ..."), so a single saved network disabled
// config.txt and the fallback for good -- the other half of why a second watch
// with a perfectly good config.txt could not get online.
#define WIFI_CAND_MAX (WIFI_SAVED_MAX + WIFI_SD_MAX + 1)

static int wifi_candidates(String *ssid, String *pass, int max) {
    wifi_saved_load();
    if (!g_net_count) parse_wifi_config();       // nothing on the card is fine
    int n = 0;
    // v2.9: THE HARDCODED NETWORK GOES FIRST.  It is the one that must be used
    // on every boot, and it is the only entry that needs neither the SD card
    // nor NVS -- so even a wiped watch gets online.  Saved networks typed on
    // the watch follow, then WATCH/wifi/config.txt.  The dedupe loops below
    // drop any later entry that repeats it.
    if (n < max) { ssid[n] = HC_WIFI_SSID; pass[n] = HC_WIFI_PASS; n++; }
    for (int i = 0; i < g_ws_n && n < max; i++) {
        if (!g_ws_ssid[i].length()) continue;
        bool dup = false;
        for (int k = 0; k < n; k++) if (ssid[k] == g_ws_ssid[i]) { dup = true; break; }
        if (dup) continue;
        ssid[n] = g_ws_ssid[i]; pass[n] = g_ws_pass[i]; n++;
    }
    for (int i = 0; i < g_net_count && n < max; i++) {
        bool dup = false;
        for (int k = 0; k < n; k++) if (ssid[k] == g_nets[i].ssid) { dup = true; break; }
        if (dup) continue;
        ssid[n] = g_nets[i].ssid; pass[n] = g_nets[i].pass; n++;
    }
    return n;
}
// v2.12: THE WAIT THAT WAS NEVER THERE.  This used to loop `while
// (!g_ntp_synced)`, and g_ntp_synced is ALREADY true on any watch that booted
// with a clock (setup(): "if (g_tp_valid) g_ntp_synced = true;").  So the loop
// ran ZERO times, time_save_now() republished the OLD time, and the caller
// switched the radio off a few microseconds after configTime() -- the SNTP
// reply never had a chance to arrive.  That is exactly "it joins the network
// but the time does not sync": the join worked, the sync was never waited for.
// The wait is measured against the clock itself now: SNTP STEPS the epoch, so
// a jump of 2 s or more against the monotonic baseline is proof it answered.
#define NTP_WAIT_MS 9000
static bool ntp_sync_now() {
    time_apply_tz(g_gmt_offset_sec);
    const time_t   t0  = time(nullptr);
    const bool     had = (t0 >= (time_t)TIME_VALID_MIN);   // clock was already sane
    const uint32_t m0  = millis();
    configTime(g_gmt_offset_sec, 0, "pool.ntp.org");
    bool stepped = false, rearmed = false;
    while (millis() - m0 < NTP_WAIT_MS) {
        if (!rearmed && (millis() - m0) >= (NTP_WAIT_MS / 2)) {
            // One lost UDP packet should not cost the whole sync: re-init the
            // client, which issues a fresh request to the same server.
            configTime(g_gmt_offset_sec, 0, "pool.ntp.org");
            rearmed = true;
        }
        lv_timer_handler();                    // keep the screen alive while we wait
        const time_t t = time(nullptr);
        if (t >= (time_t)TIME_VALID_MIN) {
            const long elapsed = (long)((millis() - m0) / 1000UL);
            if (labs((long)t - (long)t0 - elapsed) >= 2) { stepped = true; break; }
        }
        delay(40);
    }
    const bool ok = stepped || had;           // a clock that was right already IS right
    g_ntp_synced = ok;
    USBSerial.printf("[ntp] %s after %lu ms%s\n",
                     ok ? (stepped ? "clock SET" : "clock already set") : "no answer",
                     (unsigned long)(millis() - m0), rearmed ? ", retried" : "");
    if (ok) time_save_now(stepped ? "ntp set" : "ntp ok");
    return ok;
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
    // v1.9: networks saved on the watch come first, and when there are any the
    // SD list and the hardcoded fallback are skipped entirely -- someone who
    // typed their own password is not asking to be put on somebody else's
    // network, and walking five dead entries first cost up to 36 s.
    String cs[WIFI_CAND_MAX], cp[WIFI_CAND_MAX];
    const int nc = wifi_candidates(cs, cp, WIFI_CAND_MAX);
    USBSerial.printf("[wifi] %d credential(s) to try\n", nc);
    // v2.8: 12 s per network, DualBoot's WIFI_TRY_MS.  Six seconds is not
    // enough for a WPA2 handshake plus DHCP on a slow AP, so a perfectly good
    // password used to be recorded as a failure.
    const uint32_t budget_end = millis() + 32000;   // v2.9: boot cannot stall
    for (int i = 0; i < nc && !g_wifi_connected; i++) {
        if ((int32_t)(millis() - budget_end) >= 0) {
            USBSerial.println("[wifi] boot budget spent, giving up for now");
            break;
        }
        USBSerial.printf("[wifi] trying %s\n", cs[i].c_str());
        g_wifi_connected = wifi_join(cs[i].c_str(), cp[i].c_str(), 12000);
    }
    time_apply_tz(g_gmt_offset_sec);   // pin the TZ string: the timezone is RAM-only and dies every reboot
    if (g_wifi_connected) {
        // v2.10: wifi_join() connects at full power with no power save.  Now
        // that the link is up, go back to the low-current idle the POWER_SAVE
        // block asks for -- the NTP round trip is over in a few seconds.
#if POWER_SAVE
        WiFi.setSleep(WIFI_PS_MIN_MODEM);
        WiFi.setTxPower(WIFI_POWER_8_5dBm);
#endif
        ntp_sync_now();     // v2.12: the one real wait, shared by every path
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
    lv_obj_set_style_text_color(hwid, lv_color_hex(0x7A7A7A), 0);
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

// v1.8: what the clock last painted.  The face shows HH:MM, so 59 of every 60
// one-second ticks would repaint identical pixels -- a label invalidate plus a
// panel refresh per second, in every screen-on state, for nothing.
static char    g_faceLastHm[8]  = "";
static char    g_faceLastDt[32] = "";
static uint8_t g_faceLastDot    = 0xFF;

static void refresh_wallpaper() {
    // v1.8: a dim power state (IDLE / NIGHT / LOW_BATT / CRITICAL) shows a
    // pure-black face instead of the wallpaper.  On an AMOLED every lit pixel
    // is current, so a photo at the same brightness level costs several times
    // what the clock alone does -- this is the one place that decides whether
    // the background is drawn, so the rule lives here rather than in the PM.
    if (g_pmQuietFace) {
        if (face_bg_img)  lv_obj_add_flag(face_bg_img, LV_OBJ_FLAG_HIDDEN);
        if (face_bg_fill) lv_obj_add_flag(face_bg_fill, LV_OBJ_FLAG_HIDDEN);
        g_pmWpDirty = true;          // paint the new one when the face wakes
        return;
    }
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
        strftime(hm, sizeof(hm), g_time_24h ? "%H:%M" : "%I:%M", &tm);
        strftime(dt, sizeof(dt), "%a  %d %b", &tm);
    } else if (g_wifi_connected) {
        strcpy(dt, "syncing...");
    }
    uint8_t dotKey = g_ntp_synced ? 1 : (g_wifi_connected ? 2 : 3);
    if (!strcmp(g_faceLastHm, hm) && !strcmp(g_faceLastDt, dt) &&
        g_faceLastDot == dotKey) return;          // same pixels: do not repaint
    strcpy(g_faceLastHm, hm);
    strcpy(g_faceLastDt, dt);
    g_faceLastDot = dotKey;
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
    g_faceLastHm[0] = 0;              // fresh widgets: force the next tick to paint
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
//    * No saved app list.  Every boot walks every candidate folder (and one
//      level of subfolders) so the grid always matches the card.  The old NVS
//      "appcache" signature list was removed: it rebuilt each path as
//      <folder>/<stem>.bin, so a bin in a subfolder or in a second folder
//      disappeared on the next boot.
//    * "Back-to-launcher" recovery: on every boot, we mark the running
//      partition (= the launcher) as the bootable one.  So any loaded
//      app that calls esp_restart() / crashes / is power-cycled comes
//      back to LauncherOS automatically, instead of into the last app
//      that was flashed into the spare OTA slot.
// ============================================================================
struct AppEntry { String name, path; size_t size; String category; };
// 32 was tight on a card that also keeps a bin per OS version; 48 costs 1.6 KB
// of static RAM and is still far more than a 16 MB card's usable bin count.
#define MAX_APPS 48
static AppEntry  g_apps[MAX_APPS];
static int       g_app_count = 0;

// ---- v2.7: the last twelve messages, so a failure outlives the pill -------
static String    g_msg_ring[12];
static int       g_msg_n = 0;
static void log_msg(const String &m) {
    if (!m.length()) return;
    String e = m;
    e += "  [";
    e += String((unsigned long)(millis() / 1000UL));
    e += "s]";
    g_msg_ring[g_msg_n % 12] = e;
    g_msg_n++;
}
static String log_tail() {
    if (!g_msg_n) return String("(nothing yet)");
    String out;
    int n = g_msg_n < 6 ? g_msg_n : 6;
    for (int k = 0; k < n; k++) {
        int i = (g_msg_n - 1 - k + 120) % 12;
        if (!g_msg_ring[i].length()) continue;
        if (out.length()) out += "\n";
        out += g_msg_ring[i];
    }
    return out;
}

// ---- v2.7: the last three apps actually launched ---------------------------
static int       g_recent[3] = { -1, -1, -1 };
static int       g_recent_n = 0;
static void recent_push(int idx) {
    if (idx < 0 || idx >= g_app_count) return;
    int out[3], n = 0;
    out[n++] = idx;
    for (int i = 0; i < g_recent_n && n < 3; i++)
        if (g_recent[i] != idx) out[n++] = g_recent[i];
    for (int i = 0; i < n; i++) g_recent[i] = out[i];
    g_recent_n = n;
}
static String recent_line() {
    String s;
    for (int i = 0; i < g_recent_n; i++) {
        int idx = g_recent[i];
        if (idx < 0 || idx >= g_app_count) continue;
        if (s.length()) s += "  \xC2\xB7  ";
        s += g_apps[idx].name;
    }
    if (s.length()) s = String("recent:  ") + s;
    return s;
}
static lv_obj_t *g_list_widget = nullptr;
static String    bin_norm(const String &s);        // defined below the scanner
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
// ---- The one place the app folder is named --------------------------------
// Every card is formatted by a different computer, and a FAT directory that was
// created as "BIN" does not always answer to "bin".  The first spelling that
// really exists wins, and every path below is built from it, so a card can
// never be half-scanned because one string literal disagreed with the card.
static String g_apps_dir = "";
static const char *kAppDirCands[] = {
    "/WATCH/bin", "/WATCH/BIN", "/WATCH/Bin", "/WATCH/bins",
    "/WATCH/apps", "/WATCH/Apps", "/WATCH/APPS",
    // A card that was filled by dragging the repo's own `bin/` folder onto it
    // ends up with the .bin files one level higher than the launcher used to
    // look, so /bin and /apps are part of the search now.
    "/bin", "/BIN", "/Bin", "/apps", "/APPS",
};
#define APP_DIR_CANDS ((int)(sizeof(kAppDirCands) / sizeof(kAppDirCands[0])))

static bool app_dir_isdir(const char *path) {
    File d = SD_MMC.open(path);
    const bool ok = (d && d.isDirectory());
    if (d) d.close();
    return ok;
}

// `srmodels.bin` is the ESP-SR phrase model, not an app.  It has to sit in
// /WATCH/bin (sd_provision_model() copies it from there into the `model`
// partition on a blank chip), and the scan used to treat it like any other
// image: a tile called "srmodels" that, tapped, copied the language model over
// the launcher.  It is skipped everywhere an app is collected.
static bool app_is_voice_model(const String &stem) {
    String k = stem; k.trim(); k.toLowerCase();
    if (!k.length()) return false;
    if (k.startsWith("srmodels")) return true;      // srmodels, srmodels_v2, srmodels(1)
    if (k == "srmodel" || k == "srmodelbin") return true;
    return false;
}

// How many files in `path` would scan_apps() actually accept?  A card can hold
// both the right folder and a stale decoy (an empty /WATCH/bin next to the /bin
// just copied over), and "the folder exists" alone would pick the empty one.
static int app_dir_bin_count(const char *path) {
    if (!app_dir_isdir(path)) return 0;
    File d = SD_MMC.open(path);
    if (!d) return 0;
    int n = 0, seen = 0;
    File f = d.openNextFile();
    while (f && seen < 256) {
        if (!f.isDirectory()) {
            String nm = f.name();
            int sl = nm.lastIndexOf('/'); if (sl >= 0) nm = nm.substring(sl + 1);
            String lo = nm; lo.toLowerCase();
            if (lo.endsWith(".bin") && nm.length() > 4 && nm[0] != '.' && f.size() > 0 &&
                !app_is_voice_model(nm.substring(0, nm.length() - 4))) n++;
        }
        seen++;
        f = d.openNextFile();
    }
    d.close();
    return n;
}

static String apps_dir() {
    if (g_apps_dir.length()) return g_apps_dir;
    // 1. A known spelling that really holds .bin files wins immediately.
    String empty = "";
    for (int i = 0; i < APP_DIR_CANDS; i++) {
        if (!app_dir_isdir(kAppDirCands[i])) continue;
        if (app_dir_bin_count(kAppDirCands[i]) > 0) {
            g_apps_dir = kAppDirCands[i];
            if (g_apps_dir != "/WATCH/bin")
                USBSerial.printf("[scan] app folder is %s\n", g_apps_dir.c_str());
            return g_apps_dir;
        }
        if (!empty.length()) empty = kAppDirCands[i];
    }
    // 2. Unknown spelling: ask /WATCH and then the card root what they hold.  A
    //    card formatted on a Mac or Windows sometimes carries "BIn" / "bins " /
    //    "Bin.", and those would otherwise all look like a missing folder.
    const char *parents[] = { "/WATCH", "/" };
    for (int p = 0; p < 2; p++) {
        File d = SD_MMC.open(parents[p]);
        if (!d || !d.isDirectory()) { if (d) d.close(); continue; }
        File e = d.openNextFile();
        while (e) {
            if (e.isDirectory()) {
                String n = e.name();
                int sl = n.lastIndexOf('/'); if (sl >= 0) n = n.substring(sl + 1);
                String lo = n; lo.toLowerCase(); lo.trim();
                if (lo == "bin" || lo == "bins" || lo == "apps") {
                    String path = parents[p];
                    if (!path.endsWith("/")) path += "/";
                    path += n;
                    if (app_dir_bin_count(path.c_str()) > 0) {
                        g_apps_dir = path;
                        e.close(); d.close();
                        USBSerial.printf("[scan] app folder is %s\n", g_apps_dir.c_str());
                        return g_apps_dir;
                    }
                    if (!empty.length()) empty = path;
                }
            }
            e = d.openNextFile();
        }
        d.close();
    }
    if (empty.length()) {
        g_apps_dir = empty;
        USBSerial.printf("[scan] app folder is %s (no .bin inside)\n", g_apps_dir.c_str());
        return g_apps_dir;
    }
    // Not found: report the canonical path but do NOT cache the failure, so a
    // card inserted later is still probed again.
    return String(kAppDirCands[0]);
}

// When the walk finds nothing, print what the card actually holds.  One boot
// log then answers "which folder are the bins really in?" without guesswork.
static void scan_log_root() {
    File r = SD_MMC.open("/");
    if (!r || !r.isDirectory()) { if (r) r.close(); USBSerial.println("[scan] card root unreadable"); return; }
    USBSerial.print("[scan] card root holds:");
    File e = r.openNextFile();
    int n = 0;
    while (e && n < 16) {
        String nm = e.name();
        int sl = nm.lastIndexOf('/'); if (sl >= 0) nm = nm.substring(sl + 1);
        USBSerial.printf(" %s%s", nm.c_str(), e.isDirectory() ? "/" : "");
        n++;
        e = r.openNextFile();
    }
    if (!n) USBSerial.print(" (empty)");
    USBSerial.println();
    r.close();
}

static void load_categories_txt() {
    g_cat_count = 0;
    g_cat_file_present = false;
    const String cat_path = apps_dir() + "/categories.txt";
    if (!SD_MMC.exists(cat_path)) {
        USBSerial.println("[cat] categories.txt not present -- flat list");
        return;
    }
    File f = SD_MMC.open(cat_path, FILE_READ);
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
    lv_obj_set_style_text_color(copy_eta, lv_color_hex(0x8A8A8A), 0);
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
    for (int i = 0; i < g_app_count; i++)
        if (g_apps[i].path == app.path) { recent_push(i); break; }
    build_copy_screen(app.name, app.size);
#if POWER_SAVE
    setCpuFrequencyMhz(CPU_FREQ_BUSY_MHZ);   // OTA flash writes need speed
#endif
    const esp_partition_t *ota = esp_ota_get_next_update_partition(NULL);
    if (!ota) { copy_error("No free OTA partition"); return false; }
    // esp_ota_begin() would only say "failed" here, which reads as a corrupt file
    // rather than "this image cannot fit the slot it has to run from".
    if (app.size > ota->size) {
        copy_error(String("App is ") + String((unsigned)(app.size / 1024)) +
                   " KB, the slot is " + String((unsigned)(ota->size / 1024)) + " KB");
        return false;
    }
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
    lv_obj_set_style_text_color(hint, lv_color_hex(0x7A7A7A), 0);
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
// One walker for the whole card.  This used to read a single folder -- whichever
// spelling of /WATCH/bin won -- so a card holding a flat /WATCH/bin and a few
// more bins in /WATCH/apps, or in /WATCH/bin/<App>/<App>.bin, listed only what
// happened to sit in the winning folder.  That reads as "some of the bins are
// missing".  Every candidate folder is scanned now, one level of subfolders
// included, and a name found twice is kept once (first folder wins).
static int app_walk(const String &dir, int depth) {
    File d = SD_MMC.open(dir);
    if (!d || !d.isDirectory()) { if (d) d.close(); return 0; }
    String subs[16];
    int subn = 0, added = 0, seen = 0;
    File f = d.openNextFile();
    while (f && seen < 512) {
        String n = f.name();
        int sl = n.lastIndexOf('/'); if (sl >= 0) n = n.substring(sl + 1);
        const bool isdir = f.isDirectory();
        const size_t sz = isdir ? 0 : (size_t)f.size();
        if (isdir) {
            if (depth > 0 && n.length() && n[0] != '.' && subn < 16) subs[subn++] = n;
        } else if (sz > 0 && n.length() > 4 && n[0] != '.') {
            String lo = n; lo.toLowerCase();
            // "._AstroOS.bin" is AppleDouble metadata, not an image; flashing it
            // would brick the boot into the OTA slot.  Same for dotfiles.
            if (lo.endsWith(".bin")) {
                const String stem = n.substring(0, n.length() - 4);
                if (app_is_voice_model(stem)) {
                    USBSerial.printf("[scan]   %-14s voice model, not listed\n", n.c_str());
                    f = d.openNextFile(); seen++;
                    continue;
                }
                // bin_norm, not lower-case: "Dual_Link.bin" vs "DualLink.bin" is
                // one app, and app_resolve() already treats them as one, so the
                // grid must not offer two tiles that launch the same file.
                const String k = bin_norm(stem);
                bool dup = false;
                for (int i = 0; i < g_app_count; i++) {
                    if (bin_norm(g_apps[i].name) == k) { dup = true; break; }
                }
                if (!dup && g_app_count >= MAX_APPS) {
                    USBSerial.printf("[scan] %s skipped, all %d app slots are full\n",
                                     n.c_str(), MAX_APPS);
                } else if (!dup) {
                    g_apps[g_app_count].path = dir + "/" + n;
                    g_apps[g_app_count].name = stem;
                    g_apps[g_app_count].size = sz;
                    USBSerial.printf("[scan]   %-14s %6.1f KB  %s\n",
                                     stem.c_str(), sz / 1024.0f,
                                     g_apps[g_app_count].path.c_str());
                    g_app_count++; added++;
                }
            }
        }
        f = d.openNextFile();
        seen++;
    }
    d.close();
    for (int i = 0; i < subn; i++) added += app_walk(dir + "/" + subs[i], depth - 1);
    return added;
}

static void scan_apps() {
    g_app_count = 0;
    const String primary = apps_dir();
    const uint32_t t0 = millis();
    // /WATCH/bin is the documented drop folder, so it gets two levels:
    // /WATCH/bin/App.bin, /WATCH/bin/App/App.bin and /WATCH/bin/v1.8/App.bin
    // all land in the list.  The other candidates only get one, they are
    // fallbacks for cards filled outside the documented layout.
    const int in_primary = app_walk(primary, 2);
    USBSerial.printf("[scan] %s: %d apps\n", primary.c_str(), in_primary);
    for (int i = 0; i < APP_DIR_CANDS; i++) {
        const String c = kAppDirCands[i];
        if (c == primary || !app_dir_isdir(c.c_str())) continue;  // same folder, or absent
        const int n = app_walk(c, 1);
        if (n) USBSerial.printf("[scan] %s: %d more\n", c.c_str(), n);
    }
    USBSerial.printf("[scan] found %d apps in %u ms (fresh)\n",
                     g_app_count, (unsigned)(millis() - t0));
    if (g_app_count == 0) scan_log_root();
}

// ---- one lookup for every "launch <bin>" path ------------------------------
// /WATCH/bin is filled by whatever computer the card was last plugged into, so
// the file may be "Dual_Link.bin", "dual link.bin" or "DUALLINK.BIN" while the
// command (voice or tap) says "Dual_Link".  A bin can sit in any of the scanned folders,
// not just the winning one.  Resolving in one place means both the app grid and the voice
// paths agree on what "on the card" means.
static String bin_stem(const String &s) {
    String t = s;
    t.trim();
    if (t.length() > 4) {
        String tail = t.substring(t.length() - 4);
        tail.toLowerCase();
        if (tail == ".bin") t = t.substring(0, t.length() - 4);
    }
    return t;
}
static String bin_norm(const String &s) {
    String out; out.reserve(s.length());
    for (size_t i = 0; i < s.length(); i++) {
        char ch = s[i];
        if (ch >= 'A' && ch <= 'Z') ch = (char)(ch - 'A' + 'a');
        if ((ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9')) out += ch;
    }
    return out;
}
// Walks `dir` for a .bin whose normalised stem equals `w`.  `depth` > 0 also
// looks one level down, because some cards are copied as
// /WATCH/bin/<App>/<App>.bin instead of a flat folder.
static bool app_probe_in(const String &dir, const String &w, int depth) {
    File d = SD_MMC.open(dir);
    if (!d || !d.isDirectory()) { if (d) d.close(); return false; }
    File f = d.openNextFile();
    while (f) {
        String n = f.name();
        int sl = n.lastIndexOf('/'); if (sl >= 0) n = n.substring(sl + 1);
        if (!f.isDirectory() && n.length() > 4 && n[0] != '.') {
            String lo = n; lo.toLowerCase();
            if (lo.endsWith(".bin") && f.size() > 0 && g_app_count < MAX_APPS) {
                String stem = n.substring(0, n.length() - 4);
                if (bin_norm(stem) == w && !app_is_voice_model(stem)) {
                    g_apps[g_app_count].name = stem;
                    g_apps[g_app_count].path = dir + "/" + n;
                    g_apps[g_app_count].size = (size_t)f.size();
                    g_app_count++;
                    d.close();
                    return true;
                }
            }
        }
        f = d.openNextFile();
    }
    d.close();
    if (depth <= 0) return false;
    d = SD_MMC.open(dir);
    if (!d || !d.isDirectory()) { if (d) d.close(); return false; }
    f = d.openNextFile();
    while (f) {
        String n = f.name();
        int sl = n.lastIndexOf('/'); if (sl >= 0) n = n.substring(sl + 1);
        const bool isdir = f.isDirectory();
        f = d.openNextFile();            // release this entry before recursing
        if (isdir && n.length() && n[0] != '.') {
            if (app_probe_in(dir + "/" + n, w, depth - 1)) { d.close(); return true; }
        }
    }
    d.close();
    return false;
}
// Index into g_apps[], or -1.  Trying, in order: the boot scan, a forced
// rescan (card updated since boot), then the card itself -- which also appends
// the late find to g_apps so the rest of the launch path works unchanged.
static int app_resolve(const char *wantName) {
    String want = bin_stem(String(wantName));
    const String w = bin_norm(want);
    if (!w.length()) return -1;
    for (int pass = 0; pass < 2; pass++) {
        for (int i = 0; i < g_app_count; i++)
            if (bin_norm(g_apps[i].name) == w) return i;
        if (pass == 0) scan_apps();     // a bin may have landed since boot
    }
    if (app_probe_in(apps_dir(), w, 1)) {
        USBSerial.printf("[scan] %s picked up from %s after boot\n",
                         want.c_str(), apps_dir().c_str());
        return g_app_count - 1;
    }
    return -1;
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
    // The only scan used to be the one during the splash, so a card pushed in
    // after boot left this screen empty forever.  Re-walk now that the user is
    // actually looking at the list (only when it is empty -- a populated list is
    // refreshed through app_resolve / the scan at boot).
    if (!g_app_count && g_sd_ok) scan_apps();
    lv_obj_clean(g_list_widget);

    if (!g_app_count) {
        // Empty-state: centred icon + helpful two-line hint.
        lv_obj_t *wrap = lv_obj_create(g_list_widget);
        lv_obj_remove_style_all(wrap);
        lv_obj_set_size(wrap, LV_PCT(100), LV_PCT(100));
        lv_obj_clear_flag(wrap, LV_OBJ_FLAG_SCROLLABLE);

        lv_obj_t *ico = lv_label_create(wrap);
        lv_label_set_text(ico, LV_SYMBOL_SD_CARD);
        lv_obj_set_style_text_color(ico, lv_color_hex(0x4E4E4E), 0);
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
        lv_obj_set_style_text_color(t2, lv_color_hex(0x8A8A8A), 0);
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
    {
        String rl = recent_line();
        lv_label_set_text(h2, rl.length() ? rl.c_str() : "/WATCH/bin  ·  long-press to star");
    }
    lv_obj_set_style_text_color(h2, lv_color_hex(0x8A8A8A), 0);
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
    lv_obj_set_style_text_color(foot, lv_color_hex(0x7A7A7A), 0);
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
        lv_obj_set_style_text_color(empty, lv_color_hex(0x8A8A8A), 0);
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
    lv_obj_set_style_text_color(h2, lv_color_hex(0x8A8A8A), 0);
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

    // 1b) v1.6 power manager: profile, timeout, night mode
    card_dropdown(list, "Power Profile", "night + low-battery brightness",
                  "Power Saver\nBalanced\nPerformance",
                  pm_profile_index(), cards_on_power_profile_cb);
    card_dropdown(list, "Screen Timeout", "idle seconds before dark",
                  "10 s\n15 s\n30 s\n60 s",
                  pm_timeout_index(), cards_on_timeout_cb);
    card_toggle(list, "Night Mode", "dim now until switched off",
                g_pmNightManual, cards_on_night_cb);
    card_toggle(list, "Night Schedule", "22:00 - 07:00 auto dim",
                g_pmNightSched, cards_on_night_sched_cb);
    card_toggle(list, "Auto Sleep", "10 min dark -> deep sleep",
                g_pmDeepOn, cards_on_deep_cb);

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
    card_action(list, "Xander", "companion - swipe up from the face",
                LV_SYMBOL_EYE_OPEN "  Open", theme_primary(),
                xander_open_home_cb);
    card_action(list, "Xander Setup", "voice, always-on, stats",
                LV_SYMBOL_SETTINGS "  Setup", theme_accent(),
                xander_open_settings_cb);

    // 6) Toggle: WiFi
    card_toggle(list, "WiFi radio", "resyncs the clock every 12 h",
                g_wifi_toggle, cards_on_wifi_cb);
    // 7) Toggle: BLE
    card_toggle(list, "BLE", "off saves ~15 mA",
                g_ble_toggle, cards_on_ble_cb);
    // 8) Haptics -- hidden rather than merely labelled.  The board exposes no
    // vibration motor, so this switch could only ever lie.  Flip the constant
    // when the hardware lands; the handler and the NVS key are already here.
    static const bool kHapticsFitted = false;
    if (kHapticsFitted)
        card_toggle(list, "Haptics", "vibrate on tap",
                    g_haptics_toggle, cards_on_haptics_cb);

    // 9) WiFi networks (v1.9): scan, enter a password, save it, sync
    card_action(list, "Join a network", "scan, password, sync time",
                LV_SYMBOL_WIFI "  Open", theme_primary(),
                cards_open_wifi_cb);
    // 9b) Reconnect using what is already saved
    card_action(list, "Reconnect", "uses the networks already saved",
                LV_SYMBOL_WIFI "  Run", theme_primary(),
                cards_reconnect_wifi_cb);
    // 9c) v2.8 Resync SD: remount the slot and rebuild the lists.  A card
    // pushed in after boot used to need a power cycle before the launcher
    // would admit it existed.
    card_action(list, "Resync SD card", "remount + rescan apps",
                LV_SYMBOL_REFRESH "  Run", theme_primary(),
                cards_resync_sd_cb);

    // 10) Developer info subview
    card_action(list, "Developer Info", "heap / build / counts",
                LV_SYMBOL_EYE_OPEN "  View", theme_primary(),
                cards_open_developer_cb);

    // 11) Deep sleep
    card_action(list, "Deep Sleep", "POWER or BOOT wakes it",
                LV_SYMBOL_POWER "  Sleep", theme_primary(),
                cards_deep_sleep_cb);

    // 12) Factory reset
    card_action(list, "Factory Reset", "wipes NVS + reboots",
                LV_SYMBOL_TRASH "  Reset", theme_danger(),
                cards_factory_reset_cb);

    // Footer swipe hint
    lv_obj_t *foot = lv_label_create(s);
    lv_label_set_text(foot, LV_SYMBOL_LEFT "  apps   ·   battery  " LV_SYMBOL_RIGHT);
    lv_obj_set_style_text_color(foot, lv_color_hex(0x7A7A7A), 0);
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

    // v2.1: every credential we have, in order -- saved, then config.txt, then
    // the hardcoded fallback.  No either/or: a saved network no longer hides
    // the networks in the file.
    String cs[WIFI_CAND_MAX], cp[WIFI_CAND_MAX];
    const int nc = wifi_candidates(cs, cp, WIFI_CAND_MAX);
    for (int i = 0; i < nc && !ok; i++)
        ok = batt_wifi_try_one(cs[i].c_str(), cp[i].c_str());

    g_wifi_connected = ok;

    if (ok) {
        time_apply_tz(g_gmt_offset_sec);   // pin the TZ string: the timezone is RAM-only and dies every reboot
        batt_wifi_set_status("Connected, syncing time...", 0x00FF80);
        ntp_sync_now();     // v2.12: waits for the reply before the radio drops
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

    // v1.4: the manual clock lives one swipe above this screen.  The hint is
    // dim so it never competes with the battery number, but it is the only
    // clue that the card exists.
    lv_obj_t *hint = lv_label_create(s);
    lv_label_set_text(hint, LV_SYMBOL_UP "  set time");
    lv_obj_set_style_text_color(hint, lv_color_hex(theme_primary()), 0);
    lv_obj_set_style_text_opa(hint, 140, 0);
    lv_obj_set_style_text_font(hint, &lv_font_montserrat_12, 0);
    lv_obj_align(hint, LV_ALIGN_TOP_RIGHT, -24, 32);

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
    lv_obj_set_style_text_color(batt_volt, lv_color_hex(0x8A8A8A), 0);
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
            wifi_screen_open(SCREEN_BATTERY);      // v1.9: scan / password / sync
        },
        LV_EVENT_CLICKED, NULL);
    batt_wifi_lbl = lv_label_create(batt_wifi_btn);
    lv_label_set_text(batt_wifi_lbl, LV_SYMBOL_WIFI "  WiFi");
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
    lv_label_set_text(foot, "press POWER or BOOT to wake");
    lv_obj_set_style_text_color(foot, lv_color_hex(0x444444), 0);
    lv_obj_set_style_text_font(foot, &lv_font_montserrat_12, 0);
    lv_obj_align(foot, LV_ALIGN_BOTTOM_MID, 0, -10);
}

// ============================================================================
//  MANUAL CLOCK  --  swipe UP on the Battery screen
//  ----------------------------------------------------------------------------
//  WiFi/NTP is the normal source of the wall clock, but a watch with no
//  network -- or one whose internal RTC was wiped by a dead battery -- has no
//  way to be told the time at all.  This is that way: a card with hour and
//  minute steppers, opened by a swipe up on the Battery screen.
//
//  Save writes the clock to all three places it has to reach:
//    * the chip: settimeofday() sets the ESP32-S3's internal RTC, which is the
//      only clock this board has (there is no separate RTC chip on it) and
//      which keeps counting through deep sleep;
//    * NVS: time_publish(..., manual = true) stores t / gmt / man=1 in
//      "rtcsave", so time_decide() ADOPTS this clock on the next boot instead
//      of overwinding it with an older snapshot.  That is the "no wifi" case
//      surviving a reboot;
//    * the face: g_ntp_synced is set so the 1 Hz tick renders it immediately.
// ============================================================================
static lv_obj_t *g_tc_dim   = nullptr;   // full-screen scrim on lv_layer_top()
static lv_obj_t *g_tc_card  = nullptr;
static lv_obj_t *g_tc_big   = nullptr;
static lv_obj_t *g_tc_note  = nullptr;
static int       g_tc_h     = 12;
static int       g_tc_m     = 0;
// v2.10: the DATE is editable too, in the same card as the time.  It used to
// be taken from the running clock (or the firmware build date when there was
// no clock), which meant a watch with a wrong day could only be corrected by
// connecting to WiFi -- useless when the reason you are here is that WiFi will
// not connect.
static int       g_tc_d     = 1;      // day of month, 1..31
static int       g_tc_mo    = 0;      // month, 0..11
static int       g_tc_y     = 2026;   // full year
static lv_obj_t *g_tc_vd    = nullptr;   // "22"
static lv_obj_t *g_tc_vmo   = nullptr;   // "Oct"
static lv_obj_t *g_tc_vy    = nullptr;   // "2026"
static bool      g_tc_have_date = false; // the chip already holds a date
static bool      g_tc_open  = false;

static bool time_card_is_open() { return g_tc_open; }

static const char kTcMon[] = "JanFebMarAprMayJunJulAugSepOctNovDec";

static void time_card_paint() {
    if (!g_tc_big) return;
    char b[16];
    if (g_time_24h) {
        snprintf(b, sizeof(b), "%02d:%02d", g_tc_h, g_tc_m);
    } else {
        int h12 = g_tc_h % 12; if (h12 == 0) h12 = 12;
        snprintf(b, sizeof(b), "%d:%02d", h12, g_tc_m);
    }
    lv_label_set_text(g_tc_big, b);
    if (g_tc_vd) {
        char d[8];
        snprintf(d, sizeof(d), "%d", g_tc_d);
        lv_label_set_text(g_tc_vd, d);
    }
    if (g_tc_vmo) {
        char mo[4];
        snprintf(mo, sizeof(mo), "%.3s", kTcMon + g_tc_mo * 3);
        lv_label_set_text(g_tc_vmo, mo);
    }
    if (g_tc_vy) {
        char y[8];
        snprintf(y, sizeof(y), "%04d", g_tc_y);
        lv_label_set_text(g_tc_vy, y);
    }
}

// One stepper button carries its unit (0 = hours, 1 = minutes) in bits 8+ and
// a signed step in the low byte, through LVGL's user_data pointer.
static void time_card_step_cb(lv_event_t *e) {
    if (millis() - lastSwipeMs < CLICK_AFTER_SWIPE_MS) return;
    int code = (int)(intptr_t)lv_event_get_user_data(e);
    const int unit = (code >> 8) & 0xFF;
    const int step = (int)(int8_t)(code & 0xFF);
    if (unit == 0) g_tc_h = (g_tc_h + step + 24) % 24;
    else           g_tc_m = (g_tc_m + step + 60) % 60;
    time_card_paint();
    lastTouchMs = millis();
}

static void time_card_mk_step(lv_obj_t *card, const char *sym, int x, int y,
                              int unit, int step) {
    lv_obj_t *b = lv_btn_create(card);
    lv_obj_set_size(b, 64, 46);
    lv_obj_align(b, LV_ALIGN_TOP_MID, x, y);
    lv_obj_set_style_radius(b, 12, 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(0x0A0A0A), 0);
    lv_obj_set_style_border_color(b, lv_color_hex(theme_primary()), 0);
    lv_obj_set_style_border_width(b, 1, 0);
    lv_obj_t *l = lv_label_create(b);
    lv_label_set_text(l, sym);
    lv_obj_set_style_text_color(l, lv_color_hex(theme_primary()), 0);
    lv_obj_set_style_text_font(l, &lv_font_montserrat_20, 0);
    lv_obj_center(l);
    lv_obj_add_event_cb(b, time_card_step_cb, LV_EVENT_CLICKED,
                        (void *)(intptr_t)(((unit & 0xFF) << 8) | (step & 0xFF)));
}

// ---- v2.10 date steppers ---------------------------------------------------
// Units 2/3/4 = day / month / year, same (unit<<8 | step) encoding the hour and
// minute steppers use.  The day clamps to the length of the month it lands in,
// so 31 Jan -> next month on "Mon +" becomes 28/29 Feb instead of an
// impossible 31 February, and a leap year is handled by the same rule.
static int time_card_days_in(int y, int mo) {
    static const int dm[12] = {31,28,31,30,31,30,31,31,30,31,30,31};
    if (mo < 0 || mo > 11) return 31;
    if (mo == 1) {
        const bool leap = (y % 4 == 0 && y % 100 != 0) || (y % 400 == 0);
        return leap ? 29 : 28;
    }
    return dm[mo];
}

static void time_card_clamp_day() {
    const int dim = time_card_days_in(g_tc_y, g_tc_mo);
    if (g_tc_d > dim) g_tc_d = dim;
    if (g_tc_d < 1)   g_tc_d = 1;
}

static void time_card_date_step_cb(lv_event_t *e) {
    if (millis() - lastSwipeMs < CLICK_AFTER_SWIPE_MS) return;
    int code = (int)(intptr_t)lv_event_get_user_data(e);
    const int unit = (code >> 8) & 0xFF;
    const int step = (int)(int8_t)(code & 0xFF);
    if (unit == 2) {
        g_tc_d += step;
        const int dim = time_card_days_in(g_tc_y, g_tc_mo);
        while (g_tc_d > dim) g_tc_d -= dim;       // 31 -> 1 next month
        while (g_tc_d < 1)   g_tc_d += dim;       // 1 -> last day previous
    } else if (unit == 3) {
        g_tc_mo = (g_tc_mo + step + 12) % 12;
        time_card_clamp_day();
    } else {
        g_tc_y += step;
        if (g_tc_y < 2024) g_tc_y = 2024;
        if (g_tc_y > 2099) g_tc_y = 2099;
        time_card_clamp_day();
    }
    time_card_paint();
    lastTouchMs = millis();
}

static void time_card_mk_small(lv_obj_t *card, const char *sym, int x, int y,
                               int unit, int step) {
    lv_obj_t *b = lv_btn_create(card);
    lv_obj_set_size(b, 104, 34);
    lv_obj_align(b, LV_ALIGN_TOP_MID, x, y);
    lv_obj_set_style_radius(b, 10, 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(0x0A0A0A), 0);
    lv_obj_set_style_border_color(b, lv_color_hex(theme_primary()), 0);
    lv_obj_set_style_border_width(b, 1, 0);
    lv_obj_t *l = lv_label_create(b);
    lv_label_set_text(l, sym);
    lv_obj_set_style_text_color(l, lv_color_hex(theme_primary()), 0);
    lv_obj_set_style_text_font(l, &lv_font_montserrat_16, 0);
    lv_obj_center(l);
    lv_obj_add_event_cb(b, time_card_date_step_cb, LV_EVENT_CLICKED,
                        (void *)(intptr_t)(((unit & 0xFF) << 8) | (step & 0xFF)));
}

// One date column: header, [up], value, [down].
static void time_card_date_col(lv_obj_t *card, const char *hdr, int x, int unit,
                               lv_obj_t **value_out) {
    lv_obj_t *h = lv_label_create(card);
    lv_label_set_text(h, hdr);
    lv_obj_set_width(h, 104);
    lv_obj_set_style_text_align(h, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(h, lv_color_hex(0x888888), 0);
    lv_obj_set_style_text_font(h, &lv_font_montserrat_12, 0);
    lv_obj_align(h, LV_ALIGN_TOP_MID, x, 178);

    time_card_mk_small(card, LV_SYMBOL_UP, x, 200, unit, 1);

    lv_obj_t *v = lv_label_create(card);
    lv_label_set_text(v, "--");
    lv_obj_set_width(v, 104);
    lv_obj_set_style_text_align(v, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(v, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(v, &lv_font_montserrat_20, 0);
    lv_obj_align(v, LV_ALIGN_TOP_MID, x, 236);
    *value_out = v;

    time_card_mk_small(card, LV_SYMBOL_DOWN, x, 266, unit, -1);
}

static void time_card_close(bool repaint) {
    g_tc_open = false;
    if (g_tc_dim) { lv_obj_del(g_tc_dim); g_tc_dim = nullptr; }
    g_tc_card = g_tc_big = g_tc_note = nullptr;
    g_tc_vd = g_tc_vmo = g_tc_vy = nullptr;   // v2.10: deleted with the card
    if (repaint) {
        update_face_time();
        if (batt_arc) update_battery_screen();
        lv_timer_handler();
    }
}

// %DATE% is "Mmm dd yyyy" ("Oct 22 2026").  Used only when the chip has no
// clock at all: better a firmware-build date the user can correct than 1970,
// which is below TIME_VALID_MIN and would refuse to be saved.
static bool time_card_build_date(struct tm &t) {
    static const char kMon[] = "JanFebMarAprMayJunJulAugSepOctNovDec";
    char mon[4] = {0};
    int  day = 0, year = 0;
    if (sscanf(__DATE__, "%3s %d %d", mon, &day, &year) != 3) return false;
    const char *p = strstr(kMon, mon);
    if (!p || day < 1 || day > 31) return false;
    memset(&t, 0, sizeof(t));
    t.tm_year  = year - 1900;
    t.tm_mon   = (int)((p - kMon) / 3);
    t.tm_mday  = day;
    t.tm_isdst = 0;
    return true;
}

static void time_card_apply() {
    // v2.10: every field comes from the card now, date included.  Nothing is
    // inherited from the running clock, so this works on a watch whose RTC was
    // wiped -- the case the card was built for.
    struct tm t;
    memset(&t, 0, sizeof(t));
    t.tm_year  = g_tc_y - 1900;
    t.tm_mon   = g_tc_mo;
    t.tm_mday  = g_tc_d;
    t.tm_hour  = g_tc_h;
    t.tm_min   = g_tc_m;
    t.tm_sec   = 0;
    t.tm_isdst = 0;

    time_apply_tz(g_tp_gmt);        // mktime() reads the offset out of TZ
    time_t epoch = mktime(&t);      // tm is LOCAL time -> epoch is UTC
    if (epoch < (time_t)TIME_VALID_MIN) {
        USBSerial.println("[time] manual set failed: computed epoch is implausible");
        time_card_close(false);
        return;
    }

    struct timeval tv = { epoch, 0 };
    settimeofday(&tv, nullptr);     // the chip RTC is now set
    g_ntp_synced = true;            // "there is a real clock", whatever its source
    g_tp_valid   = true;
    time_publish("manual set", true);       // NVS, man=1 -> adopted on next boot

    USBSerial.printf("[time] manual set %04d-%02d-%02d %02d:%02d local "
                     "(epoch %ld, gmt %+ld s)\n",
                     g_tc_y, g_tc_mo + 1, g_tc_d, g_tc_h, g_tc_m,
                     (long)epoch, (long)g_tp_gmt);
    time_card_close(true);
}

static void time_card_open() {
    if (g_tc_open || !screens[SCREEN_BATTERY]) return;

    struct tm t;
    memset(&t, 0, sizeof(t));
    g_tc_have_date = (time(nullptr) >= (time_t)TIME_VALID_MIN) && getLocalTime(&t, 5);
    if (!g_tc_have_date && !time_card_build_date(t)) {
        memset(&t, 0, sizeof(t));
        t.tm_year = 2026 - 1900; t.tm_mon = 0; t.tm_mday = 1;   // last resort
    }
    if (g_tc_have_date) { g_tc_h = t.tm_hour; g_tc_m = t.tm_min; }
    else                { g_tc_h = 12;        g_tc_m = 0; }
    g_tc_d  = t.tm_mday;
    g_tc_mo = t.tm_mon;
    g_tc_y  = t.tm_year + 1900;
    time_card_clamp_day();

    g_tc_dim = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(g_tc_dim);
    lv_obj_set_size(g_tc_dim, LCD_WIDTH, LCD_HEIGHT);
    lv_obj_set_style_bg_color(g_tc_dim, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(g_tc_dim, 180, 0);
    lv_obj_clear_flag(g_tc_dim, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(g_tc_dim, LV_OBJ_FLAG_CLICKABLE);   // the scrim eats stray taps

    g_tc_card = lv_obj_create(g_tc_dim);
    lv_obj_remove_style_all(g_tc_card);
    lv_obj_set_size(g_tc_card, 380, 430);   // v2.10: room for the date row
    lv_obj_center(g_tc_card);
    lv_obj_set_style_bg_color(g_tc_card, lv_color_hex(0x111111), 0);
    lv_obj_set_style_bg_opa(g_tc_card, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(g_tc_card, 24, 0);
    lv_obj_set_style_border_color(g_tc_card, lv_color_hex(theme_primary()), 0);
    lv_obj_set_style_border_width(g_tc_card, 1, 0);
    lv_obj_clear_flag(g_tc_card, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *eb = lv_label_create(g_tc_card);
    lv_label_set_text(eb, "SET TIME");
    lv_obj_set_style_text_color(eb, lv_color_hex(theme_primary()), 0);
    lv_obj_set_style_text_font(eb, &lv_font_montserrat_12, 0);
    lv_obj_align(eb, LV_ALIGN_TOP_MID, 0, 14);

    g_tc_big = lv_label_create(g_tc_card);
    lv_obj_set_style_text_color(g_tc_big, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(g_tc_big, &lv_font_montserrat_48, 0);
    lv_obj_align(g_tc_big, LV_ALIGN_TOP_MID, 0, 40);

    time_card_mk_step(g_tc_card, LV_SYMBOL_UP,   -120,  44, 0,  1);
    time_card_mk_step(g_tc_card, LV_SYMBOL_DOWN, -120, 100, 0, -1);
    time_card_mk_step(g_tc_card, LV_SYMBOL_UP,    120,  44, 1,  5);
    time_card_mk_step(g_tc_card, LV_SYMBOL_DOWN,  120, 100, 1, -5);

    lv_obj_t *ch = lv_label_create(g_tc_card);
    lv_label_set_text(ch, "HOUR");
    lv_obj_set_style_text_color(ch, lv_color_hex(0x888888), 0);
    lv_obj_set_style_text_font(ch, &lv_font_montserrat_12, 0);
    lv_obj_align(ch, LV_ALIGN_TOP_MID, -120, 154);

    lv_obj_t *cm = lv_label_create(g_tc_card);
    lv_label_set_text(cm, "MIN");
    lv_obj_set_style_text_color(cm, lv_color_hex(0x888888), 0);
    lv_obj_set_style_text_font(cm, &lv_font_montserrat_12, 0);
    lv_obj_align(cm, LV_ALIGN_TOP_MID, 120, 154);

    // v2.10: the date lives in the SAME card as the time.
    time_card_date_col(g_tc_card, "DAY",   -120, 2, &g_tc_vd);
    time_card_date_col(g_tc_card, "MONTH",    0, 3, &g_tc_vmo);
    time_card_date_col(g_tc_card, "YEAR",   120, 4, &g_tc_vy);

    g_tc_note = lv_label_create(g_tc_card);
    lv_label_set_text(g_tc_note, g_tc_have_date
        ? "Date + time save to the RTC and NVS."
        : "No clock yet - set the date and time here.");
    lv_obj_set_style_text_color(g_tc_note, lv_color_hex(0x777777), 0);
    lv_obj_set_style_text_font(g_tc_note, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_align(g_tc_note, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(g_tc_note, 340);
    lv_obj_align(g_tc_note, LV_ALIGN_TOP_MID, 0, 308);

    lv_obj_t *cancel = lv_btn_create(g_tc_card);
    lv_obj_set_size(cancel, 150, 46);
    lv_obj_align(cancel, LV_ALIGN_BOTTOM_LEFT, 20, -18);
    lv_obj_set_style_radius(cancel, 23, 0);
    lv_obj_set_style_bg_color(cancel, lv_color_hex(0x0A0A0A), 0);
    lv_obj_set_style_border_color(cancel, lv_color_hex(0x777777), 0);
    lv_obj_set_style_border_width(cancel, 1, 0);
    lv_obj_add_event_cb(cancel,
        [](lv_event_t *){
            if (millis() - lastSwipeMs < CLICK_AFTER_SWIPE_MS) return;
            time_card_close(true);
        }, LV_EVENT_CLICKED, NULL);
    lv_obj_t *cl = lv_label_create(cancel);
    lv_label_set_text(cl, "Cancel");
    lv_obj_set_style_text_color(cl, lv_color_hex(0xCCCCCC), 0);
    lv_obj_set_style_text_font(cl, &lv_font_montserrat_16, 0);
    lv_obj_center(cl);

    lv_obj_t *save = lv_btn_create(g_tc_card);
    lv_obj_set_size(save, 150, 46);
    lv_obj_align(save, LV_ALIGN_BOTTOM_RIGHT, -20, -18);
    lv_obj_set_style_radius(save, 23, 0);
    lv_obj_set_style_bg_color(save, lv_color_hex(0x0A0A0A), 0);
    lv_obj_set_style_border_color(save, lv_color_hex(0x00FF80), 0);
    lv_obj_set_style_border_width(save, 1, 0);
    lv_obj_add_event_cb(save,
        [](lv_event_t *){
            if (millis() - lastSwipeMs < CLICK_AFTER_SWIPE_MS) return;
            time_card_apply();
        }, LV_EVENT_CLICKED, NULL);
    lv_obj_t *sl = lv_label_create(save);
    lv_label_set_text(sl, "Save");
    lv_obj_set_style_text_color(sl, lv_color_hex(0x00FF80), 0);
    lv_obj_set_style_text_font(sl, &lv_font_montserrat_16, 0);
    lv_obj_center(sl);

    g_tc_open = true;
    lastTouchMs = millis();
    time_card_paint();
    lv_timer_handler();
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
    // Start the dark phase.  millis() is well past 0 by the time a timeout can
    // fire, so the "0 = no dark phase yet" sentinel in pm_auto_deep() is safe.
    g_pmDarkSince = millis();
    fade_brightness(0, 220);                // smooth ~220 ms ramp to black
    // Clear the sticky touch interrupt flag.  The FT3168 ISR sets this on
    // every TP_INT falling-edge but nothing clears it during normal swipes
    // (that was an intentional v5.2 fix).  If we don't drop it here, the
    // wake check in loop() sees a permanently-true flag and turns the
    // screen back on immediately -- the "5-second flash off" symptom.
    FT3168->IIC_Interrupt_Flag = false;
    sd_sleep("screen off");
}
static void screen_on() {
    if (!screenIsOff) return;
    screenIsOff = false;
    g_pmDarkSince = 0;                      // lit again: the dark phase is over
    // v2.1: back to the lit clock BEFORE anything is painted.  The screen-off
    // policy leaves the SoC at 80 MHz, and the repaint plus 200 ms fade below
    // both run on this core -- a wake that spent them at 80 MHz was the
    // choppiest moment in the whole UI.
#if POWER_SAVE
    if (pm_prof().cpu_active_mhz && getCpuFrequencyMhz() != pm_prof().cpu_active_mhz)
        setCpuFrequencyMhz(pm_prof().cpu_active_mhz);
#endif
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
    // v1.6: restore the user's level, capped by whatever context is active
    // (night, low or critical battery) so a wake never flashes full brightness
    // in the dark and then ramps back down.
    fade_brightness(pm_wake_brightness(), 200);
    lastTouchMs = millis();
    lastWakeMs  = millis();                    // arm the 2 s post-wake grace
    // v2.13d: AFTER the fade, not before -- the panel is already showing the
    // repainted face, so the remount costs wake latency nothing the eye sees.
    sd_wake();
}

// ============================================================================
//  DEEP SLEEP  --  manual "Sleep Now" from the Battery card.
//  Powers down everything except the internal RTC (which keeps the wall
//  clock ticking) and the two wake inputs: BOOT (GPIO0) and the side POWER
//  button (GPIO10).  Either one wakes it.
//  Draw drops to ~150-300 uA (about 50 days on a 400 mAh pack).
// ============================================================================
static void deep_sleep_now() {
    USBSerial.println("[pm] entering deep sleep -- POWER or BOOT wakes");
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

    // BOOT (GPIO0) and the side POWER button (GPIO10, the AXP2101 IRQ line)
    // are both RTC-capable on the S3, so both can wake us -- that is the whole
    // point of PWR_WAKE_PIN.  Clear the PMU latch first: an IRQ still asserted
    // at this moment would be a LOW line in the mask, and the watch would wake
    // itself the instant it slept instead of when the button was pressed.
    if (g_has_pmu) { g_pmu.getIrqStatus(); g_pmu.clearIrqStatus(); }
    esp_sleep_enable_ext1_wakeup(wake_mask(), ESP_EXT1_WAKEUP_ANY_LOW);
    esp_deep_sleep_start();   // never returns
}

// ============================================================================
//  v1.6 POWER MANAGER  --  state machine, brightness, CPU, battery policy,
//                          profiles, night mode, diagnostics
//  ----------------------------------------------------------------------------
//  Everything here runs from loop() and is non-blocking except the existing
//  fade helpers it reuses.  No new task, timer or polling loop is created.
// ============================================================================

static const char *pm_state_name(PmState s) {
    switch (s) {
        case PM_ACTIVE:     return "ACTIVE";
        case PM_IDLE:       return "IDLE";
        case PM_NIGHT:      return "NIGHT";
        case PM_CHARGING:   return "CHARGING";
        case PM_LOW_BATT:   return "LOW_BATT";
        case PM_CRITICAL:   return "CRITICAL";
        case PM_SCREEN_OFF: return "SCREEN_OFF";
        default:            return "?";
    }
}

static const PmProfileDef &pm_prof() {
    if (g_pmProfileIdx >= PMP_COUNT) g_pmProfileIdx = PMP_BALANCED;
    return kPmProfiles[g_pmProfileIdx];
}
static int pm_profile_index() { return (int)g_pmProfileIdx; }

static int pm_timeout_index() {
    int best = 1;                       // 15 s default
    int bestDelta = 0x7FFFFFFF;
    for (int i = 0; i < PM_TIMEOUT_CHOICES; i++) {
        int d = abs((int)kPmTimeouts[i] - (int)g_pmTimeoutMs);
        if (d < bestDelta) { bestDelta = d; best = i; }
    }
    return best;
}

static void pm_set_timeout_ms(uint16_t ms, bool persist) {
    int best = 1;
    int bestDelta = 0x7FFFFFFF;
    for (int i = 0; i < PM_TIMEOUT_CHOICES; i++) {
        int d = abs((int)kPmTimeouts[i] - (int)ms);
        if (d < bestDelta) { bestDelta = d; best = i; }
    }
    g_pmTimeoutMs = kPmTimeouts[best];
    if (persist) {
        g_prefs.begin("pwr", false);
        g_prefs.putUShort("toff", g_pmTimeoutMs);
        g_prefs.end();
    }
}

// ---- persistence -----------------------------------------------------------
static void pm_save() {
    g_prefs.begin("pwr", false);
    g_prefs.putUChar("prof",    g_pmProfileIdx);
    g_prefs.putBool("night",    g_pmNightManual);
    g_prefs.putBool("nsched",   g_pmNightSched);
    g_prefs.putUShort("toff",   g_pmTimeoutMs);
    g_prefs.putBool("deep",     g_pmDeepOn);
    g_prefs.end();
}

static void pm_load() {
    g_prefs.begin("pwr", true);
    int  prof  = (int)g_prefs.getUChar("prof", PMP_BALANCED);
    bool night = g_prefs.getBool("night",  false);
    bool sched = g_prefs.getBool("nsched", true);
    bool deep  = g_prefs.getBool("deep",   true);
    int  toff  = (int)g_prefs.getUShort("toff", 15000);
    g_prefs.end();
    if (prof < 0 || prof >= PMP_COUNT) prof = PMP_BALANCED;
    g_pmProfileIdx  = (uint8_t)prof;
    g_pmNightManual = night;
    g_pmNightSched  = sched;
    g_pmDeepOn      = deep;
    pm_set_timeout_ms((uint16_t)toff, false);
}

// ---- night mode ------------------------------------------------------------
// The RTC is read with a zero timeout: on a watch with no sync this returns
// false immediately instead of blocking, and the schedule simply stays off.
static bool pm_clock_hour(int *hour) {
    if (!g_ntp_synced && !g_tp_valid) return false;   // no trustworthy wall clock
    struct tm t;
    if (!getLocalTime(&t, 0)) return false;
    if (t.tm_hour < 0 || t.tm_hour > 23) return false;
    *hour = t.tm_hour;
    return true;
}

static bool pm_night_scheduled() {
    if (!g_pmNightSched) return false;
    int h = 0;
    if (!pm_clock_hour(&h)) return false;
    if (PM_NIGHT_START_HOUR <= PM_NIGHT_END_HOUR)          // same-day window
        return (h >= PM_NIGHT_START_HOUR && h < PM_NIGHT_END_HOUR);
    return (h >= PM_NIGHT_START_HOUR || h < PM_NIGHT_END_HOUR);   // wraps midnight
}

static bool pm_night_active() {
    return g_pmNightManual || pm_night_scheduled();
}

// The level a wake fades up to.  Waking at night or on a flat pack must not
// flash the full user level and then dim it again, so screen_on() asks for
// this instead of g_brightness_target.  The user's own level is still the
// ceiling -- a watch dimmed to 60 wakes at 60, never brighter.
static uint8_t pm_wake_brightness() {
    uint8_t t   = g_brightness_target;
    uint8_t cap = 255;
    if (g_pmBattLatch == 2)      cap = 20;
    else if (g_pmBattLatch == 1) cap = pm_prof().low_bright;
    else if (pm_night_active())  cap = pm_prof().night_bright;
    return (t < cap) ? t : cap;
}

// ---- quiet face ------------------------------------------------------------
// v1.8: a dim state draws the clock on black instead of the wallpaper.  On an
// AMOLED every lit pixel is current, so at the same brightness a full-screen
// photo costs several times what a few glyphs do -- and black pixels cost
// nothing at all.  The user's wallpaper is untouched: this only decides
// whether it is drawn, and refresh_wallpaper() owns that decision.
//
// Never runs while the panel is dark -- there is nothing to save on a black
// screen, and toggling would force a repaint during the fade out.
static void pm_face_quiet(bool quiet) {
#if !PM_QUIET_FACE
    quiet = false;                               // PM_QUIET_FACE 0 keeps the wallpaper
#endif
    if (quiet == g_pmQuietFace) return;
    g_pmQuietFace = quiet;
    if (!face_bg_img) return;                    // face not built yet
    if (quiet) {
        lv_obj_add_flag(face_bg_img, LV_OBJ_FLAG_HIDDEN);
        if (face_bg_fill) lv_obj_add_flag(face_bg_fill, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    if (g_pmWpDirty) {                           // wallpaper changed while dim
        g_pmWpDirty = false;
        refresh_wallpaper();
        return;
    }
    if (g_photo_count) {                         // reuse the loaded image
        lv_obj_clear_flag(face_bg_img, LV_OBJ_FLAG_HIDDEN);
        if (face_bg_fill) lv_obj_add_flag(face_bg_fill, LV_OBJ_FLAG_HIDDEN);
    } else if (face_bg_fill) {
        lv_obj_clear_flag(face_bg_fill, LV_OBJ_FLAG_HIDDEN);
    }
}

static void pm_face_policy() {
    if (screenIsOff) return;
    bool quiet = (g_pmState == PM_IDLE    || g_pmState == PM_NIGHT ||
                  g_pmState == PM_LOW_BATT || g_pmState == PM_CRITICAL);
    pm_face_quiet(quiet);
}

// ---- display ---------------------------------------------------------------
static uint8_t pm_effective_brightness() {
    uint8_t tgt = g_brightness_target;
    uint8_t cap = 255;
    switch (g_pmState) {
        case PM_SCREEN_OFF: cap = 0;                              break;
        case PM_CRITICAL:   cap = 20;                             break;
        case PM_LOW_BATT:   cap = pm_prof().low_bright;           break;
        case PM_NIGHT:      cap = pm_prof().night_bright;         break;
        case PM_IDLE:       cap = pm_prof().idle_bright;          break;
        default:            cap = 255;                            break;
    }
    return (tgt < cap) ? tgt : cap;
}

// Gradual, non-blocking ramp.  Dimming steps are larger than brightening ones
// so the panel settles quickly when you stop touching it, but a wake still
// reads as a fade rather than a flash.  Never touches g_brightness_target, so
// the user's slider level survives every state change.
static void pm_brightness_step(uint32_t now) {
    if (screenIsOff) return;
    uint8_t want = pm_effective_brightness();
    if (g_cur_brightness == want) return;
    if ((int32_t)(now - g_pmRampNext) < 0) return;
    g_pmRampNext = now + PM_RAMP_INTERVAL_MS;

    bool up  = (want > g_cur_brightness);
    int  step = up ? 12 : 8;
    int  v    = (int)g_cur_brightness + (up ? step : -step);
    if (up  && v > (int)want) v = (int)want;
    if (!up && v < (int)want) v = (int)want;
    if (v < 0)   v = 0;
    if (v > 255) v = 255;

    static_cast<Arduino_CO5300 *>(gfx)->setBrightness((uint8_t)v);
    g_cur_brightness = (uint8_t)v;
}

// ---- CPU -------------------------------------------------------------------
static uint16_t pm_cpu_for_state() {
    switch (g_pmState) {
        case PM_SCREEN_OFF: return pm_prof().cpu_off_mhz;
        case PM_IDLE:       return pm_prof().cpu_idle_mhz;
        default:            return pm_prof().cpu_active_mhz;
    }
}

// A running command may copy an app from the card into OTA (which already
// sets 240 MHz itself) or hold a WiFi round-trip; changing the clock in the
// middle of either is the one thing that can break them.
static bool pm_cpu_busy(uint32_t now) {
    if (screen_held(now)) return true;
    if (g_wifi_scan_busy) return true;
    if (g_pmMicBusy) return true;      // ESP-SR bring-up is running at 240
    return false;
}

static void pm_apply_cpu(uint32_t now) {
#if POWER_SAVE
    if (pm_cpu_busy(now)) return;
    uint16_t want = pm_cpu_for_state();
    if (!want) return;
    if (getCpuFrequencyMhz() == want) return;
    setCpuFrequencyMhz(want);
#endif
}

// Restores the clock for the CURRENT state.  Used where the old code hard-coded
// CPU_FREQ_IDLE_MHZ on the way out of a temporary boost (PTT bring-up).
static void pm_restore_cpu() {
#if POWER_SAVE
    uint16_t want = pm_cpu_for_state();
    if (want && getCpuFrequencyMhz() != want) setCpuFrequencyMhz(want);
#endif
}

// ---- notices ---------------------------------------------------------------
static lv_obj_t *g_pmNotice = nullptr;
static uint32_t  g_pmNoticeUntil = 0;

static void pm_notice(const char *msg, uint32_t hold_ms) {
    if (!g_pmNotice) {
        g_pmNotice = lv_label_create(lv_layer_top());
        lv_obj_set_style_bg_color(g_pmNotice, lv_color_hex(0x000000), 0);
        lv_obj_set_style_bg_opa(g_pmNotice, 220, 0);
        lv_obj_set_style_radius(g_pmNotice, 14, 0);
        lv_obj_set_style_pad_hor(g_pmNotice, 14, 0);
        lv_obj_set_style_pad_ver(g_pmNotice, 8, 0);
        lv_obj_set_style_border_color(g_pmNotice, lv_color_hex(theme_accent()), 0);
        lv_obj_set_style_border_width(g_pmNotice, 1, 0);
        lv_obj_set_style_text_color(g_pmNotice, lv_color_hex(0xFFFFFF), 0);
        lv_obj_set_style_text_font(g_pmNotice, &lv_font_montserrat_14, 0);
        lv_obj_clear_flag(g_pmNotice, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_clear_flag(g_pmNotice, LV_OBJ_FLAG_CLICKABLE);
    }
    lv_label_set_text(g_pmNotice, msg);
    lv_obj_clear_flag(g_pmNotice, LV_OBJ_FLAG_HIDDEN);
    lv_obj_align(g_pmNotice, LV_ALIGN_BOTTOM_MID, 0, -44);
    g_pmNoticeUntil = millis() + hold_ms;
}

static void pm_alert(const char *msg, bool wake) {
    if (screenIsOff) {
        if (!wake) {                // a dark panel is not worth lighting for a notice
            USBSerial.printf("[pm] notice (screen off): %s\n", msg);
            return;
        }
        screen_on();                // a threshold crossing is worth one wake
    }
    pm_notice(msg, PM_NOTICE_MS + 1200);
}

static void pm_notice_service(uint32_t now) {
    if (!g_pmNotice || !g_pmNoticeUntil) return;
    if ((int32_t)(now - g_pmNoticeUntil) < 0) return;
    lv_obj_add_flag(g_pmNotice, LV_OBJ_FLAG_HIDDEN);
    g_pmNoticeUntil = 0;
}

// ---- battery ---------------------------------------------------------------
// Attribute the charge that left the pack since the last reading to the state
// that was running during it.  Callers guarantee that only one state elapsed
// (pm_note_battery() is invoked from pm_enter() before the new state is set),
// so no proportional split is needed anywhere in here.
static void pm_energy_note(uint32_t now) {
    if (!g_has_pmu) return;
    if (g_batt_pct < 0 || g_batt_pct > 100) return;

    // While charging the pack is filling, so a delta is not drain.  Drop the
    // baseline and start again from the next reading on battery.
    if (g_charging) { g_pmEnLastPct = -1; return; }

    int pct = (int)g_batt_pct;
    if (g_pmEnLastPct < 0) {                       // first reading, or after a charge
        g_pmEnLastPct = pct;
        g_pmEnLastMs  = now;
        return;
    }

    uint32_t dt   = (uint32_t)(now - g_pmEnLastMs);
    int      dpct = g_pmEnLastPct - pct;           // positive == consumed

    if (dpct < 0 || dpct > PM_ENERGY_MAX_DPCT) {   // nonsense reading: resync, don't smear
        g_pmEnLastPct = pct;
        g_pmEnLastMs  = now;
        g_pmEnDropped++;
        return;
    }

    // Under 1 % the PMIC cannot tell us anything: 1 % of a 400 mAh pack is
    // 4 mAh, which is ~25 s of ACTIVE.  Short windows simply carry nothing and
    // the charge shows up in the next one -- the total telescopes, so no energy
    // is invented and none is lost.
    if (dpct == 0 || dt < 1000UL) {
        g_pmEnLastPct = pct;
        g_pmEnLastMs  = now;
        return;
    }

    float mah = (float)dpct * (float)BATT_CAPACITY_MAH / 100.0f;
    if (g_pmState < PM_STATE_COUNT) {
        g_pmEnMah[g_pmState]    += mah;
        g_pmEnMeasMs[g_pmState] += dt;
    }
    g_pmEnTotalMah += mah;
    g_pmEnSamples++;
    g_pmEnLastPct = pct;
    g_pmEnLastMs  = now;
}

static bool pm_batt_fresh(uint32_t now) {
    if (!g_has_pmu || !g_pmBattSampleMs) return false;
    return (now - g_pmBattSampleMs) < 60000UL;
}

static void pm_note_battery(uint32_t now) {
    if (!g_has_pmu) return;
    if (g_batt_pct < 0 || g_batt_pct > 100) return;    // keep the last valid stamp
    g_pmBattSampleMs = now;
    pm_energy_note(now);                               // v1.7: measured drain ledger
}

// Thresholds are latched with hysteresis so a pack that sags under AMOLED
// load cannot flip the watch between LOW and normal every few seconds.
static void pm_batt_policy(uint32_t now) {
    static uint32_t lastSample = 0;
    if (!pm_batt_fresh(now)) return;
    if (g_pmBattSampleMs == lastSample) return;        // no new reading yet
    lastSample = g_pmBattSampleMs;

    if (g_charging) {
        if (g_pmBattLatch) USBSerial.println("[pm] charging - battery limits cleared");
        g_pmBattLatch  = 0;
        g_pmOffStreak  = 0;
        return;
    }

    int pct = g_batt_pct;
    uint8_t before = g_pmBattLatch;
    if (pct <= PM_BATT_CRIT_PCT) {
        g_pmBattLatch = 2;
    } else if (g_pmBattLatch == 2) {
        if (pct > PM_BATT_CRIT_PCT + PM_BATT_HYST) g_pmBattLatch = 1;
    } else if (pct <= PM_BATT_LOW_PCT) {
        g_pmBattLatch = 1;
    } else if (g_pmBattLatch == 1 && pct > PM_BATT_LOW_PCT + PM_BATT_HYST) {
        g_pmBattLatch = 0;
    }

    if (g_pmBattLatch != before) {
        char b[48];
        if (g_pmBattLatch == 2) {
            snprintf(b, sizeof(b), "Battery %d%% - charge now", pct);
            pm_alert(b, true);          // 7 % is worth one wake
        } else if (g_pmBattLatch == 1) {
            snprintf(b, sizeof(b), "Battery %d%% - power saver", pct);
            pm_alert(b, false);         // 20 % can wait for the next glance
        } else if (!screenIsOff) {
            pm_notice("Battery OK - limits cleared", PM_NOTICE_MS);
        }
    }

    // Cell protection: two readings at or below PM_BATT_OFF_PCT, at least
    // PM_BATT_OFF_SETTLE_MS apart.  Never repeats, never fights a charger.
    if (pct <= PM_BATT_OFF_PCT) {
        if (!g_pmOffStreak) {
            g_pmOffStreak  = 1;
            g_pmOffFirstMs = now;
        } else if (g_pmOffStreak < 255) {
            g_pmOffStreak++;
        }
        if (!g_pmOffDone && g_pmOffStreak >= PM_BATT_OFF_CONFIRMS &&
            (uint32_t)(now - g_pmOffFirstMs) >= PM_BATT_OFF_SETTLE_MS &&
            !screen_held(now)) {          // never cut an app copy in half
            g_pmOffDone = true;
            USBSerial.printf("[pm] %d%% confirmed twice - protecting the cell\n", pct);
            pm_alert("Battery empty - sleeping", true);
            time_save_now("battery critical");
            delay(400);                        // let the notice paint
            deep_sleep_now();                  // saves the clock again, then sleeps
        }
    } else {
        g_pmOffStreak = 0;
    }
}

// ---- states ----------------------------------------------------------------
// The user's timeout is a ceiling, not a floor.  On a low pack the watch keeps
// the panel lit for at most PM_LOW_HOLD_MS (8 s) and at critical for
// PM_CRIT_HOLD_MS (5 s), whatever the Settings slider says -- a 60 s hold at
// 7 % is how a watch dies mid-evening.  Charging clears the latch, so this
// never fights a user who is plugged in.
static uint32_t pm_hold_ms() {
    uint32_t hold = g_pmTimeoutMs;
    if (g_pmBattLatch == 2) {
        if (hold > PM_CRIT_HOLD_MS) hold = PM_CRIT_HOLD_MS;
    } else if (g_pmBattLatch == 1) {
        if (hold > PM_LOW_HOLD_MS) hold = PM_LOW_HOLD_MS;
    }
    return hold;
}

// ---- auto deep sleep (v2.0) ------------------------------------------------
// The dark phase is two-stage, and the split is what makes the watch usable:
//
//   0 .. limit      LIGHT SLEEP.  The panel is dark, the SoC wakes every
//                   LIGHT_SLEEP_POLL_MS to watch TP_INT, so a tap puts the
//                   same interface back on screen instantly -- no boot, no
//                   splash, no lost screen, nothing re-initialised.
//   limit ..        DEEP SLEEP.  ~0.2 mA where light sleep still costs ~3 mA,
//                   at the price of a full reboot to answer, because only
//                   BOOT can wake it (TP_INT is GPIO38, not an RTC pad, so
//                   ext1 cannot see the touch controller and no code here
//                   changes that).
//
// The clock for that is g_pmDarkSince -- the moment the panel went dark -- and
// not the touch timer.  That distinction is the whole fix: "dark for ten
// minutes" is the promise made in Settings, and it has to survive a wake that
// was not a touch (a battery alert, a voice command holding the screen, the
// PWR button).  Any screen_on() zeroes it, so the ten minutes start over.
static void pm_auto_deep(uint32_t now) {
    if (!g_pmDeepOn) return;
    uint32_t limit = pm_prof().deep_after_ms;
    if (!limit) return;                        // this profile never auto-sleeps
    if (!g_pmReady || !screenIsOff) return;    // a lit panel is not idle
    if (!g_pmDarkSince) return;                // no dark phase has started yet
    // Signed: screen_off() stamps g_pmDarkSince from millis() a fraction AFTER
    // the `now` this pass was handed, so the unsigned form wrapped to ~49 days
    // and every screen-off crossed straight into deep sleep. That is why the tap
    // window never existed and the watch only ever came back with a full boot.
    if ((int32_t)(now - g_pmDarkSince) < (int32_t)limit) return;
    if (g_charging) return;                    // pointless, and hides the charge
    // On USB with the pack full the AXP2101 reports "not charging", so ask the
    // PMIC directly: a docked watch must not reboot itself every ten minutes.
    // One extra register read, only at the moment of the decision.
    if (g_has_pmu && g_pmu.isVbusIn()) return;
    if (pm_cpu_busy(now)) return;              // app copy, scan or mic bring-up
    if (g_pmVoiceArmed) return;                // the wake word would die with us

    USBSerial.printf("[pm] dark %lu min on %s -> deep sleep (POWER/BOOT wakes)\n",
                     (unsigned long)((now - g_pmDarkSince) / 60000UL), pm_prof().name);
    deep_sleep_now();                          // saves the clock, never returns
}

static PmState pm_desired_state(uint32_t now) {
    if (screenIsOff) return PM_SCREEN_OFF;

    bool charging = (g_has_pmu && g_charging);
    if (!charging && g_pmBattLatch == 2) return PM_CRITICAL;
    if (!charging && g_pmBattLatch == 1) return PM_LOW_BATT;
    // A command that holds the screen (voice phrase, app copy, update screen)
    // gets readable brightness even at night or when dimmed; releasing the
    // hold drops straight back into the profile.
    if (screen_held(now))                return PM_ACTIVE;
    if (pm_night_active())               return PM_NIGHT;
    if (charging)                        return PM_CHARGING;
    // No auto-dim: a lit panel keeps the user's brightness until the timeout
    // takes it to black.
    return PM_ACTIVE;
}

static void pm_enter(PmState s, uint32_t now) {
    if (s == g_pmState) return;

    // Close the energy window here, while g_pmState still names the outgoing
    // state.  One I2C read per transition is what lets every sample be
    // attributed to a single mode instead of a blend of two.
    if (g_has_pmu) {
        pmu_refresh();
        pm_note_battery(now);
    }

    PmState prev = g_pmState;
    if (g_pmStateSince) g_pmStateMs[prev] += (uint32_t)(now - g_pmStateSince);
    g_pmState      = s;
    g_pmStateSince = now;
    g_pmTransitions++;
    pm_apply_cpu(now);
    pm_face_policy();               // wallpaper only in the bright states

    // One line per transition, at most one per second, so an idle dim / wake
    // pair cannot flood the monitor at 80 MHz.
    static uint32_t lastPrint = 0;
    if ((uint32_t)(now - lastPrint) >= 1000) {
        lastPrint = now;
        USBSerial.printf("[pm] %s -> %s  cpu=%u MHz  bright=%u  batt=%d%%%s\n",
                         pm_state_name(prev), pm_state_name(s),
                         (unsigned)getCpuFrequencyMhz(),
                         (unsigned)pm_effective_brightness(),
                         (g_has_pmu ? g_batt_pct : -1),
                         (g_has_pmu && g_charging) ? " chg" : "");
    }
}

static void pm_service(uint32_t now) {
    if (!g_pmReady) return;

    // Same two keep-alive signals the pre-PM loop used: a finger on the panel,
    // the post-wake grace, and a command holding the screen.  Nothing else is
    // allowed to count as activity.
    bool in_wake_grace = (int32_t)(now - lastWakeMs) < (int32_t)WAKE_GRACE_MS;
    if (touchActive || in_wake_grace || screen_held(now)) lastTouchMs = now;

    if (screenIsOff && digitalRead(TP_INT) == LOW) {
        lastTouchMs = now;
        screen_on();                    // stamps lastWakeMs -> grace window
    }

    pm_batt_policy(now);

    // Signed for the same reason: screen_on() and pm_alert() stamp lastTouchMs
    // from millis(), which can be a few ms ahead of the `now` this pass carries.
    // Unsigned, that wrapped and fired screen_off() inside the very call that lit
    // the panel -- the "wake, then black a moment later" report.
    if (!screenIsOff && (int32_t)(now - lastTouchMs) >= (int32_t)pm_hold_ms()) {
        screen_off();                   // fades, clears the touch IRQ latch
        pm_enter(PM_SCREEN_OFF, now);
    } else {
        PmState want = pm_desired_state(now);
        if (want != g_pmState) pm_enter(want, now);
    }

    pm_brightness_step(now);
    pm_notice_service(now);
    pm_log(now);
    pm_auto_deep(now);              // last: everything above may have woken us
}

static void pm_activity(const char *why) {
    lastTouchMs = millis();
    USBSerial.printf("[pm] activity: %s\n", why ? why : "?");
}

static bool pm_wifi_allowed() {
    if (!g_wifi_toggle) return false;
    if (screenIsOff)    return false;
    if (!pm_prof().wifi_ok) return false;
    if (g_pmBattLatch >= 1 && !g_charging) return false;    // radio is optional
    if (screen_held(millis())) return false;
    return true;
}

// ---- diagnostics -----------------------------------------------------------
static uint32_t pm_dwell_ms(PmState s, uint32_t now) {
    uint32_t t = g_pmStateMs[s];
    if (s == g_pmState && g_pmStateSince) t += (uint32_t)(now - g_pmStateSince);
    return t;
}

// Measured mA for a state, or -1 when no window has landed in it yet.
static float pm_energy_ma(PmState s) {
    if (s >= PM_STATE_COUNT) return -1.0f;
    if (g_pmEnMeasMs[s] < 1000UL) return -1.0f;
    return g_pmEnMah[s] * 3600000.0f / (float)g_pmEnMeasMs[s];
}

// The per-mode table, from the pack itself.  One row per state that has been
// entered, plus the share of everything measured so far.
static void pm_energy_dump() {
    if (!g_has_pmu) {
        USBSerial.println("[pm] energy: no PMU on this board - nothing to measure");
        return;
    }
    if (!g_pmEnSamples) {
        USBSerial.println("[pm] energy: no drain measured yet (needs one full % step)");
        return;
    }

    float sum = 0.0f;
    for (int i = 0; i < PM_STATE_COUNT; i++) sum += g_pmEnMah[i];

    USBSerial.printf("[pm] energy: %lu windows, %.1f mAh measured, capacity %u mAh\n",
                     (unsigned long)g_pmEnSamples, sum, (unsigned)BATT_CAPACITY_MAH);

    uint32_t now = millis();
    for (int i = 0; i < PM_STATE_COUNT; i++) {
        uint32_t dwell = pm_dwell_ms((PmState)i, now);
        if (!dwell && !g_pmEnMah[i]) continue;
        float ma    = pm_energy_ma((PmState)i);
        float share = (sum > 0.0f) ? g_pmEnMah[i] * 100.0f / sum : 0.0f;
        if (ma < 0.0f) {
            USBSerial.printf("[pm]   %-11s %6.2fh %8.1fmAh      - %4.0f%%\n",
                             pm_state_name((PmState)i),
                             (float)dwell / 3600000.0f, g_pmEnMah[i], share);
        } else {
            USBSerial.printf("[pm]   %-11s %6.2fh %8.1fmAh %5.0fmA %4.0f%%\n",
                             pm_state_name((PmState)i),
                             (float)dwell / 3600000.0f, g_pmEnMah[i], ma, share);
        }
    }
    if (g_pmEnDropped) {
        USBSerial.printf("[pm]   %lu readings discarded as invalid\n",
                         (unsigned long)g_pmEnDropped);
    }
}

static void pm_log(uint32_t now) {
    if (!g_pmLogNext) { g_pmLogNext = now + PM_LOG_INTERVAL_MS; return; }
    if ((int32_t)(now - g_pmLogNext) < 0) return;
    g_pmLogNext = now + PM_LOG_INTERVAL_MS;

    uint32_t up = now - g_pmBootMs;
    USBSerial.printf("[pm] %s  batt=%d%% %dmV%s  cpu=%u MHz  bright=%u/%u  "
                     "awake=%lus off=%lus  xitions=%lu  heap=%uK\n",
                     pm_state_name(g_pmState),
                     g_has_pmu ? g_batt_pct : -1, g_batt_mv,
                     (g_has_pmu && g_charging) ? " chg" : "",
                     (unsigned)getCpuFrequencyMhz(),
                     (unsigned)g_cur_brightness, (unsigned)g_brightness_target,
                     (unsigned long)(pm_dwell_ms(PM_ACTIVE, now) / 1000UL),
                     (unsigned long)(pm_dwell_ms(PM_SCREEN_OFF, now) / 1000UL),
                     (unsigned long)g_pmTransitions,
                     (unsigned)(ESP.getFreeHeap() / 1024));
    if (up > 60000UL && g_has_pmu && g_batt_pct >= 0 && g_pmPctAtBoot) {
        int drop = (int)g_pmPctAtBoot - g_batt_pct;
        if (drop > 0) {
            USBSerial.printf("[pm] %d%% used in %lu min  (~%.2f %%/h)\n",
                             drop, (unsigned long)(up / 60000UL),
                             (float)drop * 3600000.0f / (float)up);
        }
    }

    // v1.7: the measured per-mode table, every 30 min.
    if (!g_pmEnDumpNext) {
        g_pmEnDumpNext = now + PM_ENERGY_DUMP_MS;
    } else if ((int32_t)(now - g_pmEnDumpNext) >= 0) {
        g_pmEnDumpNext = now + PM_ENERGY_DUMP_MS;
        pm_energy_dump();
    }
}

// ---- startup ---------------------------------------------------------------
static void pm_init() {
    g_pmBootMs      = millis();
    g_pmState       = PM_ACTIVE;
    g_pmStateSince  = g_pmBootMs;
    g_pmTransitions = 0;
    for (int i = 0; i < PM_STATE_COUNT; i++) {
        g_pmStateMs[i]  = 0;
        g_pmEnMah[i]    = 0.0f;
        g_pmEnMeasMs[i] = 0;
    }
    g_pmEnTotalMah = 0.0f;
    g_pmEnLastPct  = -1;
    g_pmEnLastMs   = 0;
    g_pmEnSamples  = 0;
    g_pmEnDropped  = 0;
    g_pmEnDumpNext = 0;
    g_pmPctAtBoot  = (g_has_pmu && g_batt_pct >= 0) ? (uint8_t)g_batt_pct : 0;
    g_pmBattLatch  = 0;
    g_pmOffStreak  = 0;
    g_pmOffDone    = false;
    g_pmNoticeUntil = 0;
    // A boot that starts with the panel already dark (a woke-from-sleep path
    // that came up black) begins its dark phase now; a lit boot begins it in
    // screen_off(), so the ten minutes are always measured from real darkness.
    g_pmDarkSince  = screenIsOff ? (g_pmBootMs ? g_pmBootMs : 1) : 0;
    if (g_has_pmu) pm_note_battery(g_pmBootMs);
    g_pmReady = true;

    pm_apply_cpu(g_pmBootMs);
    USBSerial.printf("[pm] profile %s | timeout %us | no auto-dim | night %s%s\n",
                     pm_prof().name, (unsigned)(g_pmTimeoutMs / 1000),
                     g_pmNightManual ? "manual" : "off",
                     (g_pmNightSched && !g_pmNightManual) ? " + schedule" : "");
    USBSerial.printf("[pm] cpu %u MHz  brightness %u/%u  batt %d%%%s\n",
                     (unsigned)getCpuFrequencyMhz(),
                     (unsigned)g_cur_brightness, (unsigned)g_brightness_target,
                     g_has_pmu ? g_batt_pct : -1,
                     (g_has_pmu && g_charging) ? " (charging)" : "");
    uint32_t dpark = g_pmDeepOn ? pm_prof().deep_after_ms : 0;
    if (dpark)
        USBSerial.printf("[pm] dark phase: tap wakes for %lu min, then deep sleep (POWER/BOOT wakes)\n",
                         (unsigned long)(dpark / 60000UL));
    else
        USBSerial.println("[pm] dark phase: light sleep only - never parks in deep sleep");
}

// ---- Settings rows ---------------------------------------------------------
static void cards_on_power_profile_cb(lv_event_t *e) {
    if (millis() - lastSwipeMs < CLICK_AFTER_SWIPE_MS) return;
    int sel = lv_dropdown_get_selected(lv_event_get_target(e));
    if (sel < 0 || sel >= PMP_COUNT) return;
    if (sel == (int)g_pmProfileIdx) return;
    g_pmProfileIdx = (uint8_t)sel;
    pm_save();
    USBSerial.printf("[pm] profile -> %s\n", kPmProfiles[sel].name);
    pm_apply_cpu(millis());                 // idle clock may change with the profile
}

static void cards_on_timeout_cb(lv_event_t *e) {
    if (millis() - lastSwipeMs < CLICK_AFTER_SWIPE_MS) return;
    int sel = lv_dropdown_get_selected(lv_event_get_target(e));
    if (sel < 0 || sel >= PM_TIMEOUT_CHOICES) return;
    pm_set_timeout_ms(kPmTimeouts[sel], true);
    USBSerial.printf("[pm] screen timeout -> %us\n", (unsigned)(g_pmTimeoutMs / 1000));
}

static void cards_on_night_cb(lv_event_t *e) {
    if (millis() - lastSwipeMs < CLICK_AFTER_SWIPE_MS) return;
    g_pmNightManual = lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED);
    pm_save();
    USBSerial.printf("[pm] night mode %s\n", g_pmNightManual ? "on" : "off");
}

static void cards_on_night_sched_cb(lv_event_t *e) {
    if (millis() - lastSwipeMs < CLICK_AFTER_SWIPE_MS) return;
    g_pmNightSched = lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED);
    pm_save();
    USBSerial.printf("[pm] night schedule %s\n", g_pmNightSched ? "on" : "off");
}

static void cards_on_deep_cb(lv_event_t *e) {
    if (millis() - lastSwipeMs < CLICK_AFTER_SWIPE_MS) return;
    g_pmDeepOn = lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED);
    pm_save();
    USBSerial.printf("[pm] auto deep sleep %s\n", g_pmDeepOn ? "on" : "off");
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
        lv_obj_set_style_text_color(lbl, lv_color_hex(0x8A8A8A), 0);
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
        lv_obj_set_style_text_color(lbl, lv_color_hex(0x8A8A8A), 0);
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
    { Preferences p; p.begin("appcache", false); p.clear(); p.end(); }   // retired app-list cache
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

    // v1.7: opening this screen is the on-demand trigger for the measured
    // per-mode drain table, so it can be pulled without waiting for the
    // 30-minute summary.
    pm_energy_dump();

    char clock_txt[48];
    {
        uint32_t up = millis(), ls = g_last_sync_ms;
        if (!ls) snprintf(clock_txt, sizeof(clock_txt), "never synced this boot");
        else {
            uint32_t mins = (up - ls) / 60000UL;
            if (mins < 1)       snprintf(clock_txt, sizeof(clock_txt), "synced just now");
            else if (mins < 90) snprintf(clock_txt, sizeof(clock_txt), "synced %lu min ago", (unsigned long)mins);
            else                snprintf(clock_txt, sizeof(clock_txt), "synced %lu h ago", (unsigned long)(mins / 60));
        }
    }
    // v2.10: which app slots exist, right on the screen -- this is the answer
    // to "is the factory table actually flashed?" without a serial monitor.
    char slot_txt[112];
    snprintf(slot_txt, sizeof(slot_txt), "%s home, %s scratch",
             swap_boot_name(bk_home()), swap_boot_name(bk_transit()));
    char buf[900];
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
        "Categories file: %s\n\n"
        "Power:     %s (%s)\n"
        "Awake %lum  idle %lum  dark %lum\n"
        "Batt: %d%%  %dmV%s\n\n"
        "Clock:  %s\n"
        "Slots:  %s\n"
        "Last messages:\n%s",
        theme_name_at(g_theme_idx), g_theme_idx,
        g_launcher_style == LS_PILL      ? "Pill list" :
        g_launcher_style == LS_COVERFLOW ? "3D CoverFlow" : "Grid",
        (unsigned)(ESP.getFreeHeap() / 1024),
        (unsigned)(ESP.getFreePsram() / 1024),
        (unsigned)getCpuFrequencyMhz(),
        g_app_count, g_photo_count, g_fav_count,
        g_cat_file_present ? "yes" : "no",
        pm_state_name(g_pmState), pm_prof().name,
        (unsigned long)(pm_dwell_ms(PM_ACTIVE, millis()) / 60000UL),
        (unsigned long)(pm_dwell_ms(PM_IDLE,   millis()) / 60000UL),
        (unsigned long)(pm_dwell_ms(PM_SCREEN_OFF, millis()) / 60000UL),
        g_has_pmu ? g_batt_pct : -1, g_batt_mv,
        (g_has_pmu && g_charging) ? " charging" : "",
        clock_txt, slot_txt, log_tail().c_str());
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
    g_cur_brightness    = (uint8_t)v;
    g_brightness_target = (uint8_t)v;
    g_prefs.begin("dcw", false);
    g_prefs.putInt("bright", v);
    g_prefs.end();
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

// v2.8: remount the card at runtime.  Apps, categories, wallpapers and the
// WATCH/wifi/config.txt network list all come off the SD card, so a card that
// arrived after boot was invisible until a power cycle.  Unmount first --
// SD_MMC.begin() on a live mount is not re-entrant -- then run the same scans
// setup() runs, so the launcher state matches the card that is actually there.
static void cards_resync_sd_cb(lv_event_t *) {
    if (millis() - lastSwipeMs < CLICK_AFTER_SWIPE_MS) return;
    ptt_flash("SD: rescanning...", 1200);
    SD_MMC.end();
    g_sd_ok = false;
    delay(120);
    g_sd_ok = sd_mount();
    if (g_sd_ok) {
        scan_apps();
        load_categories_txt();
        scan_photos();
        parse_wifi_config();          // the network list may have changed too
        char sbuf[48];
        snprintf(sbuf, sizeof(sbuf), "SD ok: %d apps", g_app_count);
        ptt_flash(sbuf, 1600);
    } else {
        ptt_flash("SD: no card found", 2200);
    }
    // The app / wallpaper counts are printed on this screen, so rebuild it.
    lv_obj_t *old = screens[SCREEN_SETTINGS];
    build_screen_settings();
    if (current_screen == SCREEN_SETTINGS) lv_scr_load(screens[SCREEN_SETTINGS]);
    if (old) lv_obj_del(old);
}
static void cards_open_wifi_cb(lv_event_t *) {
    if (millis() - lastSwipeMs < CLICK_AFTER_SWIPE_MS) return;
    wifi_screen_open(SCREEN_SETTINGS);
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

// ---- v1.9 identity: one colour, one dial, one voice -----------------------
// Xander took theme_primary() everywhere, so he looked like whichever of the
// eleven palettes was active -- grey in Terminal Mono, pink in Synthwave.  This
// one warm accent is HIS: wordmark, orb, live pip, reply bubble border.  Every
// other surface stays themed, so the layer still belongs to the launcher.
#define X_ACCENT 0xFFC24B

// ---- v2.13j: the Hawthorne palette ----------------------------------------
// Xander is Xander Hawthorne, so the layer stops being "a dark UI with an amber
// accent" and becomes an estate's stationery: ink paper, brass, oxblood, and
// parchment for anything he says.  There are no bitmaps in this sketch -- the
// fonts and the letter-spacing are the whole visual language -- so the palette
// and the monogram do the identity work.
#define X_INK        0x0B0A0C   // paper
#define X_BRASS      X_ACCENT   // the family colour, already in place
#define X_OXBLOOD    0x8E2A38   // the seal, and a softer error tone
#define X_PARCHMENT  0xF3EAD9   // his voice
#define X_SMOKE      0x8B8175   // microcaps, quiet labels
// v2.15: failure got a seal (X_OXBLOOD) but success was still X_MOSS -- a
// status LED.  One warm green, sitting beside the brass the way the seal sits
// beside the ink, so a right answer reads as gilt rather than as neon.
#define X_MOSS       0x9CCB8C   // a door that opened
#define X_NBANK(a) ((int)(sizeof(a) / sizeof((a)[0])))

// Charm: 0 Dry (the plainest line only), 1 Charming, 2 Full Xander.  Persisted,
// and deliberately in the settings card: a personality with no volume control
// stops being charming in week two.
static int    g_x_charm = 1;
static String g_x_nick  = "";          // his name for you, earned from your habits

// ---- response library ------------------------------------------------------
//  The plainest line sits first because Charm = Dry returns bank[0] and
//  nothing else.  Anything that can land in the voice pill should stay under
//  ~60 characters -- the pill holds 1.1 s + 32 ms per character, and a long
//  punchline is five seconds of reading at arm's length.
static const char *X_R_OK[]       = { "Done.",
                                   "Consider the door open.",
                                   "Handled. You are welcome.",
                                   "Done. Barely had to cheat.",
                                   "Ask me for something harder." };
static const char *X_R_ALREADY[]  = { "Already there.",
                                        "Already done. You are slipping.",
                                        "That door was open before you knocked.",
                                        "I got there first. It is a habit." };
static const char *X_R_ERR[]      = { "That did not work.",
                                    "Well. That went badly.",
                                    "Something is in the way. Give me a second.",
                                    "A locked door. Give me a minute and a hairpin." };
static const char *X_R_UNK[]      = { "I do not know that one yet.",
                                    "You found a door I have not opened.",
                                    "Not in my playbook. Yet.",
                                    "I would be guessing, and I do not guess.",
                                    "Ask me something I can win." };
static const char *X_R_GREET[]    = { "What shall we investigate?",
                                      "Back again? Good.",
                                      "Pick a door. I will not say which.",
                                      "Another game, then.",
                                      "The house is quiet. Let us fix that.",
                                      "Ready when you are. I usually am." };
static const char *X_R_RIGHT[]    = { "Correct.",
                                      "Got it in one.",
                                      "One door down.",
                                      "Correct. Obviously.",
                                      "There it is. Keep moving.",
                                      "You are getting dangerous." };
static const char *X_R_SASS[]     = { "Wrong. But bold.",
                                     "Not quite. I would have had it.",
                                     "Wrong. The old man would have laughed.",
                                     "Hm. Try the other door.",
                                     "No. But I like that you tried.",
                                     "Wrong. Do not make it a habit." };
static const char *X_R_GIVE[]     = { "Here is one for you.",
                                     "Try this one.",
                                     "Fresh trial. No keys.",
                                     "One door. One way through. Go.",
                                     "A trial, then. No borrowing." };

// ---- secrets (typed only) ---------------------------------------------------
//  The spoken vocabulary is frozen by srmodels.bin (30 MultiNet phrases), so
//  this is the one place new words can live.  Matched against the lower-cased
//  input, so "are you real?" lands on the same entry as "are you real".
struct XSecret { const char *key; const char *say; };
static const XSecret kSecrets[] = {
  { "hawthorne",          "Four brothers, one fortune, and a house full of doors that do not open the way you think." },
  { "hawthorne house",    "Doors that open the wrong way. Hallways that should not exist. Ask me again when nobody is listening." },
  { "old man",            "The old man built the house. The house keeps the game. I keep score." },
  { "four brothers",      "Four of us. One fortune. Absolutely no patience for a boring answer." },
  { "secret passage",     "Thirteen, if you count the one behind the library. I count it." },
  { "the foundation",     "Their money, my games. The distinction matters more to me than to them." },
  { "grayson",            "The responsible one. Do not tell him he is my favourite." },
  { "jameson",            "He jumps first. I am already at the bottom, taking notes." },
  { "nash",               "He sees everything and says a third of it. The third is usually the useful part." },
  { "tobias",             "Grandfather left puzzles instead of letters. I took the hint." },
  { "who are you",        "The youngest. The loudest. The one who reads the rules and then bends them." },
  { "the stakes",         "Everything, eventually. Start with a streak and work up." },
  { "behind the door",    "Another door. That is the whole trick, and you fell for it beautifully." },
  { "make a bet",         "You are on. I take riddles, you take physics, and we both pretend it is fair." },
  { "bored",              "Then pick a trial. Boredom is a puzzle you have not found yet." },
  { "do you like me",     "I like anyone who keeps score. You keep score." },
  { "the will",           "Left as a game. Everything my family has, it won or lost by answering a question." },
  { "riddle me this",     "I am the thing you solve with nothing but time. Ask me for a trial instead." },
  { "what are you hiding","A second door. You already know about it - you built it." },
  { "whose watch is this","Yours. I just live in it." },
  { "tell me a secret",   "The trials get harder on purpose. So do I." },
  { "are you real",       "Real enough to keep score." },
};
#define X_NSECRET X_NBANK(kSecrets)

static const char *x_say(const char *const *a, int n) {
    if (!a || n <= 0) return "";
    if (g_x_charm <= 0) return a[0];                  // Dry: one line, no theatre
    // v2.5: the dial was saved, drawn and ignored above Dry -- Charming and
    // Full Xander returned the same rotation.  Charming now stays inside the
    // first three lines of a bank (the polite ones); Full Xander reaches the
    // whole bank, which is where the sharp lines live.
    int top = (g_x_charm >= 2) ? n : (n < 3 ? n : 3);
    int k = (int)((millis() >> 3) % (uint32_t)top);
    if (k < 0) k = 0;
    return a[k];
}
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

// ---- v2.12: multiple choice ------------------------------------------------
//  A 502 px watch with a 196 px keyboard was the wrong place to ask someone to
//  type "They land together".  Each trial now shows three doors on its card.
//  Kept as a PARALLEL table so the 16 trial rows above stay readable, with a
//  compile-time check that keeps the two tables the same length -- a trial
//  added without options is a build error, not a blank button.
//  `right` is the index of the true door; it is spread across 0/1/2 so the
//  answer is not always the first row.
struct XPuzOpts { const char *o[3]; uint8_t right; };
static const XPuzOpts kPuzOpts[] = {
    { { "Warm = 1, lit = 2, cold = 3", "Lit = 1, warm = 2, cold = 3", "All three feel the same" }, 0 },
    { { "36", "63", "54" }, 1 },
    { { "Shadows", "Memories", "Footsteps" }, 2 },
    { { "They land together", "The hammer", "The feather" }, 0 },
    { { "Oxygen", "Hydrogen", "Nitrogen" }, 1 },
    { { "O(n)", "O(n log n)", "O(log n)" }, 2 },
    { { "100 rpm", "300 rpm", "900 rpm" }, 0 },
    { { "11", "13", "16" }, 1 },
    { { "4 weighings", "3 weighings", "2 weighings" }, 2 },
    { { "5000", "5050", "5100" }, 1 },
    { { "An echo", "The wind", "A bell" }, 0 },
    { { "It doubles", "It quadruples", "It stays the same" }, 1 },
    { { "10001 (17)", "10101 (21)", "11001 (25)" }, 0 },
    { { "Duty cycle sets brightness", "It lowers the voltage", "It stores charge" }, 0 },
    { { "Ir, 77", "In, 49", "Fe, 26" }, 2 },
    { { "True", "Neither - it is a paradox", "False" }, 1 },
};
static_assert(sizeof(kPuzOpts) / sizeof(kPuzOpts[0]) == (size_t)X_NPUZ,
              "kPuzOpts needs exactly one row per trial in kPuzzles");

// ---- knowledge database ----------------------------------------------------
struct XArticle { const char *id; const char *category; const char *title;
                  const char *summary; const char *details;
                  const char *terms; };   // v2.2: spoken-language synonyms
// v2.2: the library, 19 -> 55 articles.  Every entry carries a `terms` string -
// the words a person actually says ("how do gears work", "why is the sky blue")
// - because retrieval is the whole trick: an article nobody can reach is not
// knowledge, it is a filing cabinet.  x_find_article() weights a terms hit
// above a title word so "batteries" lands on Batteries and not on Redox.
static const XArticle kArticles[] = {
  { "chem_001","chemistry","Electrolysis","Electrolysis uses electrical energy to drive a chemical reaction that would not happen on its own.","A direct current is passed through an electrolyte. Positive ions travel to the cathode (reduction), negative ions to the anode (oxidation). Splitting water gives hydrogen at the cathode and oxygen at the anode.","electrolysis electrolyte cathode anode splitting water hydrogen oxygen electric current ions" },
  { "chem_002","chemistry","Redox Reactions","Redox reactions transfer electrons: one species is oxidised (loses electrons) while another is reduced (gains them).","A handy memory aid is OIL RIG - Oxidation Is Loss, Reduction Is Gain. Rusting, combustion and batteries are all redox processes.","redox oxidation reduction electrons oil rig rusting combustion battery corrosion" },
  { "phys_001","physics","Ohm's Law","Ohm's law links voltage, current and resistance in a circuit: V = I x R.","Voltage (volts) equals current (amps) times resistance (ohms). Rearranged, I = V/R and R = V/I. It only holds for ohmic components at constant temperature.","ohm voltage current resistance volts amps ohms circuit resistor formula" },
  { "phys_002","physics","Newton's Laws","Newton's three laws describe how forces change the motion of objects.","1) An object stays at rest or moves steadily unless a force acts. 2) F = m x a. 3) Every action has an equal and opposite reaction.","newton force motion inertia acceleration action reaction mass gravity push pull" },
  { "bio_001","biology","DNA","DNA stores the genetic instructions used to build and run living things.","It is a double helix of nucleotides using four bases: A, T, C and G. A pairs with T, C pairs with G. The order of bases spells out genes.","dna genes genetic helix nucleotides bases chromosome heredity inherit code" },
  { "bio_002","biology","The Cell","The cell is the smallest unit of life; all organisms are made of one or more cells.","Animal cells have a nucleus, mitochondria, cytoplasm and a membrane. Plant cells add a cell wall and chloroplasts for photosynthesis.","cell nucleus mitochondria membrane cytoplasm organelle plant animal biology tissue" },
  { "math_001","maths","Pi","Pi is the ratio of a circle's circumference to its diameter, about 3.14159.","Pi is irrational - its decimals never repeat or end. Circumference = 2 x pi x r, and area = pi x r squared.","pi circle circumference diameter radius area irrational 3.14 round" },
  { "math_002","maths","Prime Numbers","A prime number is a whole number greater than 1 whose only divisors are 1 and itself.","2, 3, 5, 7, 11, 13... 2 is the only even prime. Every whole number above 1 factorises into primes in exactly one way.","prime number factor divisor composite factorisation sieve whole" },
  { "eng_001","engineering","Torque","Torque is a twisting force - how strongly something is turned about a pivot.","Torque = force x distance from the pivot. A longer lever means more torque for the same push, which is why long wrenches loosen tight bolts.","torque lever wrench bolt force distance pivot rotation twist spanner" },
  { "elec_001","electronics","The Transistor","A transistor is a tiny switch or amplifier that controls a large current with a small one.","In digital logic it acts as an on/off switch; billions of them make up a CPU. The two common families are BJT and MOSFET.","transistor switch amplifier mosfet bjt cpu chip semiconductor logic gate" },
  { "elec_002","electronics","I2C Bus","I2C is a two-wire serial bus that lets a controller talk to many chips.","It uses SDA (data) and SCL (clock). Each device has an address, so a sensor, a touch panel and a power chip can all share the same pair of wires - exactly how this watch works.","i2c sda scl bus address sensor touch panel wire serial chips watch" },
  { "robo_001","robotics","PID Control","A PID controller keeps a system on target by reacting to error in three ways.","Proportional reacts to the current error, Integral to accumulated past error, Derivative to how fast the error is changing. Tuning the three gains gives smooth, stable control.","pid controller proportional integral derivative gain tuning error motor drone stable" },
  { "robo_002","robotics","Servo Motors","A servo is a motor that moves to and holds a precise angle you command.","A control pulse tells the servo the target position; internal feedback drives it there and holds it. Hobby servos usually cover about 180 degrees.","servo motor angle pulse pwm feedback hobby 180 degrees robot arm position" },
  { "prog_001","programming","Recursion","Recursion is when a function solves a problem by calling itself on a smaller version of it.","Every recursion needs a base case to stop, or it runs forever. Factorials, tree walks and divide-and-conquer algorithms are natural fits.","recursion recursive function base case factorial tree stack overflow calls itself" },
  { "prog_002","programming","Big-O Notation","Big-O describes how an algorithm's cost grows as the input grows.","O(1) is constant, O(log n) halves each step, O(n) is linear, O(n squared) is nested loops. It ignores constants and focuses on the trend at scale.","big o complexity algorithm runtime grows linear log quadratic performance speed" },
  { "astro_001","astronomy","Black Holes","A black hole is a region where gravity is so strong that not even light can escape.","They form when massive stars collapse. The event horizon is the point of no return; the singularity lies at the centre. We spot them by their effect on nearby matter and light.","black hole gravity event horizon singularity collapse star space light escape" },
  { "astro_002","astronomy","Light-Year","A light-year is the distance light travels in one year - about 9.46 trillion km.","It is a measure of distance, not time. Because light takes years to reach us, looking far into space is also looking back in time.","light year distance speed of light trillion km space time telescope look back" },
  { "geo_001","geography","Plate Tectonics","Earth's crust is broken into slabs that drift on the mantle, shaping continents.","Where plates pull apart, spread, or grind past one another you get ridges, volcanoes, mountains and earthquakes. They move a few centimetres a year.","plate tectonics earthquake volcano mountain crust mantle continental drift fault" },
  { "gen_001","general","The Greenhouse Effect","Certain gases trap heat in the atmosphere, keeping the planet warm enough for life.","Sunlight warms the surface; the surface re-emits infrared, which gases like CO2 and water vapour absorb and re-radiate. Too much of these gases warms the planet further.","greenhouse effect co2 carbon dioxide climate warming atmosphere infrared heat planet" },
  { "chem_003","chemistry","The Periodic Table","The periodic table arranges every element by its number of protons, and the columns share behaviour.","Rows are called periods, columns are groups. Elements in a group have the same number of outer electrons, which is why they react alike. Hydrogen sits alone at the top left.","periodic table element group period atomic number protons mendeleev rows columns metal" },
  { "chem_004","chemistry","Acids and Bases","An acid gives up hydrogen ions and a base takes them; the pH scale from 0 to 14 measures which you have.","Below 7 is acidic, 7 is neutral, above 7 is basic. Mixing an acid with a base neutralises both, leaving water and a salt. Lemon juice is about pH 2, soap about 10.","acid base ph litmus neutralisation salt alkali hydrogen ions sour soap lemon" },
  { "phys_003","physics","Friction","Friction is the force that resists one surface sliding over another, and it always turns motion into heat.","Static friction holds a still object and is stronger than kinetic friction once it slides. It comes from microscopic roughness gripping. Without it you could not walk, brake or hold a pencil.","friction static kinetic surface heat rubbing grip traction brakes slide rough" },
  { "phys_004","physics","Pressure","Pressure is force spread over an area, measured in pascals - one newton per square metre.","The same force on a smaller area gives more pressure, which is why a sharp knife cuts and snowshoes float. In a fluid pressure rises with depth: every 10 m of water adds about one atmosphere.","pressure force area pascal fluid depth atmospheric bar knife snowshoe tyre" },
  { "phys_005","physics","Energy and Work","Energy is the capacity to do work, measured in joules; work is a force moving something through a distance.","Kinetic energy is motion (half m v squared), potential energy is stored height or strain. Energy is never made or destroyed, only converted - and some always ends up as low-grade heat.","energy work joule kinetic potential conservation power watt stored motion heat" },
  { "phys_006","physics","Sound Waves","Sound is a travelling pressure wave, and its frequency is what you hear as pitch.","It needs a medium, so it cannot cross a vacuum. In air it moves about 343 m/s - roughly a kilometre every three seconds, which is why thunder arrives after the lightning. Loudness is amplitude, in decibels.","sound wave frequency pitch decibel echo speed air vibration thunder loud medium" },
  { "bio_003","biology","Photosynthesis","Plants use sunlight to turn carbon dioxide and water into sugar, releasing oxygen as waste.","Chlorophyll in the leaves absorbs red and blue light and reflects green, which is why plants look green. The sugar feeds the plant; the oxygen is what we breathe.","photosynthesis chlorophyll sunlight glucose oxygen leaves plant carbon dioxide green" },
  { "bio_004","biology","The Heart","The heart is a muscular pump with four chambers that pushes blood around the body.","The right side sends blood to the lungs for oxygen; the left side drives it out to the body, which is why the left wall is thicker. A resting adult beats 60-100 times a minute, moving about five litres every minute.","heart blood pump chambers oxygen lungs veins arteries circulation pulse beat" },
  { "bio_005","biology","Evolution","Evolution is the change in the inherited features of a population over many generations.","Individuals vary, more young are born than can survive, and those with helpful variations leave more offspring. Repeat that for thousands of generations and species change shape. It is selection, not effort.","evolution natural selection darwin adaptation species mutation generations survival fittest" },
  { "math_003","maths","The Fibonacci Sequence","Each Fibonacci number is the sum of the two before it: 1, 1, 2, 3, 5, 8, 13, 21...","The ratio of neighbouring terms closes in on the golden ratio, about 1.618. Petal counts, pinecone spirals and sunflower seeds often land on these numbers because that spacing packs most efficiently.","fibonacci sequence golden ratio spiral sunflower pinecone rabbit 1 2 3 5 8 13" },
  { "math_004","maths","Probability","Probability measures how likely an event is, on a scale from 0 (never) to 1 (certain).","For equally likely outcomes it is favourable outcomes over total outcomes, so a die shows a three one time in six. Independent events multiply: two heads in a row is a half times a half.","probability chance odds coin dice random independent expected likely percent" },
  { "math_005","maths","Pythagoras' Theorem","In a right-angled triangle the square of the longest side equals the sum of the squares of the other two.","That is a squared plus b squared equals c squared, where c is the hypotenuse. The 3-4-5 triangle is the simplest whole-number example, and builders still use it to square a corner.","pythagoras triangle right angle hypotenuse square theorem 3 4 5 diagonal geometry" },
  { "eng_002","engineering","Gears","Gears trade speed for turning force: a small gear driving a big one turns slower but with more torque.","The ratio of teeth sets the trade. Two gears in mesh turn in opposite directions; a chain or belt lets them turn the same way. Bicycle gears are the everyday version of this.","gear ratio teeth torque speed rotation bicycle chain transmission cog mesh" },
  { "eng_003","engineering","Bridges","Bridges carry loads to the ground using compression, tension, or a balance of both.","A beam bridge is simplest and sags under load; an arch turns the load into compression along its curve; a suspension bridge hangs the deck from cables in tension. Longer spans need the stronger shapes.","bridge beam arch suspension compression tension span load cable structure" },
  { "elec_003","electronics","Batteries","A battery turns chemical energy into electrical energy, one cell at a time.","Each cell has an anode, a cathode and an electrolyte, and gives a fixed voltage set by its chemistry - about 3.7 V for lithium. Cells in series add voltage, in parallel add capacity, measured in mAh.","battery cell voltage lithium anode cathode electrolyte charge mah series parallel" },
  { "elec_004","electronics","PWM","Pulse-width modulation switches a signal on and off fast so the average looks like a value between the rails.","The fraction of time it is on is the duty cycle, so 50 percent gives half power. It dims LEDs and controls motor speed without wasting the energy a resistor would burn as heat.","pwm duty cycle dim led motor speed average pulse width modulation brightness" },
  { "elec_005","electronics","Resistors","A resistor limits current, and its value in ohms is printed as coloured bands.","In series, resistances add. In parallel, the total is smaller than the smallest one - two equal resistors give half the value. A voltage divider uses two of them to scale a signal down.","resistor ohm colour code bands series parallel divider current limit value" },
  { "prog_003","programming","Arrays","An array is a numbered row of values of the same type, reached by index.","Indexes start at 0, so the first element is a[0] and the last is a[n-1]. Writing past the end does not stop - it corrupts whatever is next in memory, which is where a lot of crashes come from.","array index list elements zero based memory length out of bounds" },
  { "prog_004","programming","Boolean Logic","Boolean logic works with two values, true and false, combined by AND, OR and NOT.","AND is true only if both inputs are; OR if either is; NOT flips one. Every chip in a computer is built from these, wired as gates. A truth table lists every combination so you can check the result.","boolean logic and or not true false gate truth table bit binary" },
  { "prog_005","programming","Version Control","Version control records every change to a set of files so you can see what changed, when, and undo it.","Git stores snapshots called commits, each pointing at the one before, forming a history. Branches let you try something risky and merge it back only if it works. The commit message is a note to your future self.","git version control commit branch repository diff history undo merge" },
  { "robo_003","robotics","Sensors","A sensor turns something physical - motion, light, pressure, magnetism - into a signal a computer can read.","An accelerometer measures acceleration, including gravity, so it also tells you which way is down. A gyroscope measures rotation; a magnetometer measures magnetic fields, so it acts as a compass. This watch combines the first two.","sensor accelerometer gyroscope magnetometer imu tilt step compass motion watch" },
  { "astro_003","astronomy","The Solar System","Eight planets orbit the Sun, held by gravity, all in roughly the same flat plane.","In order: Mercury, Venus, Earth, Mars, Jupiter, Saturn, Uranus, Neptune. The inner four are small and rocky, the outer four are giants of gas and ice. Sunlight takes about eight minutes to reach us.","solar system planets mercury venus earth mars jupiter saturn uranus neptune orbit sun" },
  { "astro_004","astronomy","The Moon","The Moon is Earth's only natural satellite, about 384,000 km away, and it is tidally locked to us.","That lock is why we always see the same face. Its gravity pulls the oceans into two tides a day. It drifts away about 3.8 cm a year, and probably formed from debris of a giant impact.","moon tides phases lunar orbit eclipse gravity crater 384000 face satellite" },
  { "astro_005","astronomy","Stars","A star is a ball of gas so massive that its core fuses hydrogen into helium and shines.","The outward push of that fusion balances gravity, which keeps it steady for billions of years. When the hydrogen runs out, a sun-like star swells and sheds its layers; a heavy one can collapse and explode as a supernova.","star fusion hydrogen helium supernova main sequence sun brightness core nova" },
  { "geo_002","geography","The Water Cycle","Water evaporates, condenses into clouds, falls as rain, and runs back to the sea - endlessly.","The Sun supplies the energy and gravity does the return trip. Only about 0.3 percent of Earth's water is fresh liquid on the surface, which is why rivers and aquifers matter so much.","water cycle evaporation condensation precipitation rain cloud river groundwater fresh" },
  { "geo_003","geography","Mountains","Mountains form where the crust is pushed up, folded or erupted, and they erode almost as fast as they rise.","Fold mountains like the Himalayas rise where continents collide; volcanic ones build from erupted rock. Everest is 8,849 m. Air thins with height, so the summit has about a third of sea-level pressure.","mountain everest fold volcanic range altitude summit himalaya erosion peak" },
  { "gen_002","general","Mains Electricity","Mains power is alternating current at a fixed voltage and frequency - 230 V at 50 Hz in Australia.","It alternates because AC is easy to transform up for transport and back down for use. The frequency sets the hum you hear near transformers. Never open a mains plug: the current that can stop a heart is far smaller than the one that trips a breaker.","mains electricity volts ac dc frequency 50 hz plug socket safety shock transformer" },
  { "gen_003","general","How Touchscreens Work","A capacitive touchscreen senses the tiny charge your fingertip steals from a grid inside the glass.","The panel carries an invisible lattice of electrodes; a finger changes the capacitance at one point and the controller reports the coordinates. It needs a conductive touch, which is why a gloved finger or a plastic stylus does nothing.","touchscreen capacitive glass finger charge sensor panel display grid stylus glove" },
  { "hist_001","history","The Printing Press","Moveable metal type, refined by Gutenberg around 1440, made books cheap enough for ideas to spread.","Before it a book was copied by hand over months. Within fifty years millions of volumes existed, and literacy, science and religion were all reshaped by it. It is the ancestor of every copy and paste.","printing press gutenberg 1440 books movable type invention copy spread literacy" },
  { "hist_002","history","The Industrial Revolution","From the 1760s, steam power and factories moved production out of homes and into cities.","Coal, iron and the steam engine let machines outwork muscle for the first time. Output soared and cities swelled, and working conditions were often brutal - which is where modern labour law came from.","industrial revolution steam engine factory coal iron britain 1760 machinery cities" },
  { "hist_003","history","The Space Race","The Soviet Union and the United States raced to space between 1957 and 1975.","Sputnik, the first satellite, went up in 1957; Gagarin orbited in 1961; Apollo 11 landed two people on the Moon in 1969. The race gave us weather satellites, global communication and a great deal of spin-off technology.","space race sputnik apollo 11 moon landing 1969 nasa soviet gagarin satellite cold war" },
  { "hlth_001","health","Sleep","Sleep runs in cycles of about ninety minutes, alternating deep sleep and dreaming REM sleep.","Deep sleep repairs the body, and memory consolidates largely in REM. Teenagers need eight to ten hours. Losing sleep degrades judgement before you notice it, which is why it feels fine while it is going wrong.","sleep stages rem deep memory rest hours bedtime brain tired nap" },
  { "hlth_002","health","Water in the Body","About 60 percent of an adult body is water, and it carries everything the body moves.","Blood, digestion and temperature control all depend on it. Thirst arrives late, so dark urine is the more reliable sign. You lose water breathing, sweating and simply existing, so it needs replacing steadily rather than in one go.","water hydration body dehydration thirst urine drink percent sweat temperature" },
  { "hlth_003","health","The Immune System","The immune system recognises what belongs to you and attacks what does not.","White blood cells patrol and engulf invaders; others manufacture antibodies that lock onto one specific shape. A vaccine shows the body that shape in a harmless form so the response is ready in advance. Fever is the body raising its own temperature to slow invaders.","immune system white blood cells antibodies vaccine infection fever defence germ" },
  { "lang_001","language","Where English Comes From","English is a Germanic language that absorbed a huge amount of French, Latin and Greek.","The Anglo-Saxon core gives everyday words like house and eat; the Norman conquest of 1066 layered French over it, which is why a cow in a field becomes beef on a plate. Greek and Latin supply most scientific vocabulary.","english language latin greek norman french germanic root word origin etymology history" },
  { "lang_002","language","Mnemonics","A mnemonic is a hook that makes a list stick - a rhyme, an acronym, or a vivid image.","Chunking works the same way: seven digits is easier as 04 12 987 than as one long run. It works because memory is better at patterns and pictures than at raw sequence. OIL RIG and Every Good Boy Deserves Fudge are old examples.","mnemonic memory chunking acronym remember study technique rhyme list forget" },
};
#define X_NART ((int)(sizeof(kArticles)/sizeof(kArticles[0])))
// ---- v2.6: knowledge on the card -------------------------------------------
//  /WATCH/Xander/know.txt, one article per line:
//      id|category|title|summary|details|terms
//  Loaded into RAM at entry, eight at most (the read mask is 64 bits and the
//  compiled table already owns 55 of them).  No card, no file, or a broken line
//  simply means the built-in library -- the flash articles always work.
#define X_SD_MAX 8
static const int X_NART_FIXED = (int)(sizeof(kArticles) / sizeof(kArticles[0]));
static XArticle g_xsd[X_SD_MAX];
static char     g_xsd_buf[X_SD_MAX][6][56];
static int      g_xsd_n = 0;
static int x_nart() { return X_NART_FIXED + g_xsd_n; }
static const XArticle &x_art(int i) {
    static const XArticle none = { "", "", "?", "?", "?", "" };
    if (i >= 0 && i < X_NART_FIXED) return kArticles[i];
    if (i >= X_NART_FIXED && i < x_nart()) return g_xsd[i - X_NART_FIXED];
    return none;
}
static void x_sd_load() {
    g_xsd_n = 0;
    if (!g_sd_ok) return;
    File f = SD_MMC.open("/WATCH/Xander/know.txt", FILE_READ);
    if (!f) return;
    while (f.available() && g_xsd_n < X_SD_MAX) {
        String line = f.readStringUntil('\n');
        line.trim();
        if (!line.length() || line[0] == '#') continue;
        String parts[6];
        int from = 0, got = 0;
        while (got < 5) {
            int bar = line.indexOf('|', from);
            if (bar < 0) break;
            parts[got++] = line.substring(from, bar);
            from = bar + 1;
        }
        if (got < 5) continue;
        parts[5] = line.substring(from);
        for (int k = 0; k < 6; k++) {
            parts[k].trim();
            if (parts[k].length() > 55) parts[k] = parts[k].substring(0, 55);
            strncpy(g_xsd_buf[g_xsd_n][k], parts[k].c_str(), 55);
            g_xsd_buf[g_xsd_n][k][55] = 0;
        }
        if (!strlen(g_xsd_buf[g_xsd_n][2])) continue;
        g_xsd[g_xsd_n].id       = g_xsd_buf[g_xsd_n][0];
        g_xsd[g_xsd_n].category = g_xsd_buf[g_xsd_n][1];
        g_xsd[g_xsd_n].title    = g_xsd_buf[g_xsd_n][2];
        g_xsd[g_xsd_n].summary  = g_xsd_buf[g_xsd_n][3];
        g_xsd[g_xsd_n].details  = g_xsd_buf[g_xsd_n][4];
        g_xsd[g_xsd_n].terms    = g_xsd_buf[g_xsd_n][5];
        g_xsd_n++;
    }
    f.close();
}
static_assert(X_NART + X_SD_MAX <= 64, "read mask is 64 bits wide");

static_assert(X_NART <= 64, "x_read_mask is 64 bits wide");
static const char *X_KCATS[] = { "chemistry","physics","biology","maths","engineering",
    "electronics","robotics","programming","astronomy","geography","history","health",
    "language","general" };
#define X_NKCAT ((int)(sizeof(X_KCATS)/sizeof(X_KCATS[0])))

// ---- command template table (chips + keyword intents) ----------------------
enum XIntent {
  XI_UNKNOWN=0, XI_HELP, XI_HOME_EXIT,
  XI_OPEN_PUZZLES, XI_OPEN_KNOWLEDGE, XI_OPEN_STORIES, XI_RESET_ALL, XI_GIVE_PUZZLE, XI_TEACH,
  XI_WIFI_ON, XI_WIFI_OFF, XI_BLE_ON, XI_BLE_OFF,
  XI_GPS_ON, XI_GPS_OFF, XI_BEACON_ON, XI_BEACON_OFF,
  XI_SHOW_BATTERY, XI_SHOW_STATUS,
  // v1.1 -- clock / display / system intents (phrase table below)
  XI_SYNC_TIME, XI_SYNC_DATE, XI_TIME_STATUS, XI_GET_TIME, XI_GET_DATE, XI_GET_DAY,
  XI_OPEN_CLOCK_SETTINGS, XI_TIME_12H, XI_TIME_24H,
  XI_BRIGHT_UP, XI_BRIGHT_DOWN, XI_BRIGHT_MAX, XI_BRIGHT_MIN, XI_BRIGHT_50,
  XI_RESTART, XI_SHUTDOWN,
  XI_GET_MEMORY, XI_GET_FREE_RAM, XI_GET_FLASH_STORAGE, XI_GET_SD_STATUS,
  XI_GET_FREE_STORAGE, XI_GET_SYSTEM_STATUS,
  XI_SWAP_SLOT, XI_SWAP_CANCEL,     // v1.7.1: DualBoot slot swap + its abort
  XI_SWAP_BACK                      // boot the DualBoot image already in ota_0
};
struct XTmpl { const char *label; const char *keys; int intent; };
static const XTmpl kTmpl[] = {
  { "Give me a puzzle",   "puzzle riddle challenge quiz",        XI_GIVE_PUZZLE },
  { "Teach me something", "teach learn lesson knowledge",        XI_TEACH },
  { "Open puzzles",       "puzzles",                             XI_OPEN_PUZZLES },
  { "Open knowledge",     "knowledge library browse",            XI_OPEN_KNOWLEDGE },
  { "Open stories",       "stories story tale night",             XI_OPEN_STORIES },
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

// ============================================================================
//  v1.1  PHRASE TABLE  --  the fixed spoken commands
//  ----------------------------------------------------------------------------
//  Every entry is matched against the utterance AFTER it has been lower-cased
//  and reduced to alphanumeric words.  Matching is on the WORD SEQUENCE, with
//  filler ("xander", "please", "the", "now", ...) ignored, so the same table
//  serves the on-screen keyboard today and a speech recogniser later without a
//  rewrite.  Longer phrases win, which is what separates "sync time" from
//  "check time status".
//
//  Keep entries in the shape a recogniser returns them: plain words, no
//  punctuation, no case tricks, no hyphenated "Wi-Fi" (the normaliser folds
//  "wi fi" -> "wifi" and "blue tooth" -> "bluetooth" before matching).
// ============================================================================
struct XPhrase { const char *words; uint8_t intent; };
static const XPhrase kPhrase[] = {
    // ---- v1.7.1 slot swap (DualBoot) -------------------------------------
    // Voice goes through kPtt below; these make the same four phrases work when
    // typed into Xander's console.
    { "dual boot",                       XI_SWAP_SLOT   },
    { "dual",                            XI_SWAP_SLOT   },
    { "hero time",                       XI_SWAP_SLOT   },
    { "hero",                            XI_SWAP_SLOT   },
    { "cancel",                          XI_SWAP_CANCEL },
    // "back to dual" is deliberately absent: the way back is voice-only (kPtt
    // below).  Xander is a hidden layer, so a slot change should not be reachable
    // by typing into its console -- a stray line of text must not relink the
    // bootloader.  The Dual Boot install stays typed as well as spoken.
    // ---- clock -----------------------------------------------------------
    { "sync time",                       XI_SYNC_TIME },
    { "sync clock",                      XI_SYNC_TIME },
    { "update the time",                 XI_SYNC_TIME },
    { "update time",                     XI_SYNC_TIME },
    { "sync date",                       XI_SYNC_DATE },
    { "check time status",               XI_TIME_STATUS },
    { "time status",                     XI_TIME_STATUS },
    { "what time is it",                 XI_GET_TIME },
    { "what is the time",                XI_GET_TIME },
    { "what s the time",                 XI_GET_TIME },
    { "whats the time",                  XI_GET_TIME },
    { "what time",                       XI_GET_TIME },
    { "what s today s date",             XI_GET_DATE },
    { "whats today s date",              XI_GET_DATE },
    { "what is today s date",            XI_GET_DATE },
    { "what is the date",                XI_GET_DATE },
    { "what s the date",                 XI_GET_DATE },
    { "whats the date",                  XI_GET_DATE },
    { "what day is it",                  XI_GET_DAY },
    { "what s the day",                  XI_GET_DAY },
    { "whats the day",                   XI_GET_DAY },
    // bare one-word asks (safe: "sync time" leaves "sync" over, not filler)
    { "time",                            XI_GET_TIME },
    { "date",                            XI_GET_DATE },
    { "day",                             XI_GET_DAY },
    { "open clock settings",             XI_OPEN_CLOCK_SETTINGS },
    { "clock settings",                  XI_OPEN_CLOCK_SETTINGS },
    { "use twelve hour time",            XI_TIME_12H },
    { "twelve hour time",                XI_TIME_12H },
    { "12 hour time",                    XI_TIME_12H },
    { "use twenty four hour time",       XI_TIME_24H },
    { "twenty four hour time",           XI_TIME_24H },
    { "24 hour time",                    XI_TIME_24H },
    // ---- display ----------------------------------------------------------
    { "brightness up",                   XI_BRIGHT_UP },
    { "brighter",                        XI_BRIGHT_UP },
    { "brightness down",                 XI_BRIGHT_DOWN },
    { "dimmer",                          XI_BRIGHT_DOWN },
    { "maximum brightness",              XI_BRIGHT_MAX },
    { "max brightness",                  XI_BRIGHT_MAX },
    { "minimum brightness",              XI_BRIGHT_MIN },
    { "min brightness",                  XI_BRIGHT_MIN },
    { "set brightness to fifty percent", XI_BRIGHT_50 },
    { "brightness fifty percent",        XI_BRIGHT_50 },
    // ---- radios ------------------------------------------------------------
    { "turn wifi on",                    XI_WIFI_ON },
    { "wifi on",                         XI_WIFI_ON },
    { "enable wifi",                     XI_WIFI_ON },
    { "turn wifi off",                   XI_WIFI_OFF },
    { "wifi off",                        XI_WIFI_OFF },
    { "disable wifi",                    XI_WIFI_OFF },
    { "turn bluetooth on",               XI_BLE_ON },
    { "bluetooth on",                    XI_BLE_ON },
    { "enable bluetooth",                XI_BLE_ON },
    { "turn bluetooth off",              XI_BLE_OFF },
    { "bluetooth off",                   XI_BLE_OFF },
    { "disable bluetooth",               XI_BLE_OFF },
    // ---- power / system ----------------------------------------------------
    { "restart the watch",               XI_RESTART },
    { "restart watch",                   XI_RESTART },
    { "reboot the watch",                XI_RESTART },
    { "shut down the watch",             XI_SHUTDOWN },
    { "shutdown the watch",              XI_SHUTDOWN },
    { "power off the watch",             XI_SHUTDOWN },
    { "battery status",                  XI_SHOW_BATTERY },
    { "how much battery is left",        XI_SHOW_BATTERY },
    { "battery left",                    XI_SHOW_BATTERY },
    { "show memory usage",               XI_GET_MEMORY },
    { "memory usage",                    XI_GET_MEMORY },
    { "how much ram is free",            XI_GET_FREE_RAM },
    { "free ram",                        XI_GET_FREE_RAM },
    { "show flash storage",              XI_GET_FLASH_STORAGE },
    { "flash storage",                   XI_GET_FLASH_STORAGE },
    { "show sd card status",             XI_GET_SD_STATUS },
    { "sd card status",                  XI_GET_SD_STATUS },
    { "how much storage is left",        XI_GET_FREE_STORAGE },
    { "free storage",                    XI_GET_FREE_STORAGE },
    { "system status",                   XI_GET_SYSTEM_STATUS },
};
#define X_NPHASE ((int)(sizeof(kPhrase)/sizeof(kPhrase[0])))

// ---- tall on-screen command chips (short labels, same intents) -------------
struct XChip { const char *label; uint8_t intent; };
static const XChip kChips[] = {
    { "What time is it?", XI_GET_TIME },
    { "Sync time",        XI_SYNC_TIME },
    { "Brightness up",    XI_BRIGHT_UP },
    { "Brightness down",  XI_BRIGHT_DOWN },
    { "System status",    XI_GET_SYSTEM_STATUS },
    { "Start over",       XI_RESET_ALL },
    { "Shut down",        XI_SHUTDOWN },
};
#define X_NCHIP ((int)(sizeof(kChips)/sizeof(kChips[0])))

// ============================================================================
//  v1.1  OS LAUNCHER  --  spoken name OR mood -> the .bin the card holds
//  ----------------------------------------------------------------------------
//  Keys are lower-case with NO spaces: the matcher joins the spoken tokens
//  ("astro" + "os" -> "astroos", "it's go time" -> "itsgotime") before looking
//  one up, which is also what an ASR result looks like.  Multi-word entries are
//  the natural-language handles (mood, intent) for the same app; a key of 6+
//  characters stands on its own, while short keys like "music" still need a
//  launch verb or an otherwise-empty sentence so "music status" cannot yank
//  you into SpotifyOS.
//
//  The .bin name is resolved against g_apps (scanned from /WATCH/bin at boot),
//  NOT hard-coded, so a missing app gets an honest "not on the card" answer
//  instead of a dead tap or a bad flash.
// ============================================================================
struct XOsAlias { const char *say; const char *bin; };
static const XOsAlias kOs[] = {
    // ---- WidgetOS / FusionOS (v1.7) --------------------------------------
    // "Widget" is the command-widget OS and its image is MiniOS.bin; both names
    // resolve to the .bin the card holds.  load_app_from_sd() writes it into the
    // NEXT OTA slot (esp_ota_get_next_update_partition), so the slot the
    // launcher is running from is never the one being overwritten.  Longest key
    // first: the matcher is a substring test on the joined tokens.
    { "widgetos",   "MiniOS"     },
    { "widget",     "MiniOS"     },
    { "minios",     "MiniOS"     },
    { "fusionos",   "FusionOS"   },
    { "fusion",     "FusionOS"   },
    // ---- by name ---------------------------------------------------------
    { "astroos",    "AstroOS"    },
    { "spotifyos",  "SpotifyOS"  },
    { "tkdos",      "TKDOS"      },
    { "sportos",    "SportOS"    },
    { "gameos",     "GameOS"     },
    { "exploreros", "ExplorerOS" },
    { "tacticalos", "TacticalOS" },
    { "bookos",     "BookOS"     },
    { "waypointos", "WaypointOS" },
    { "duallink",   "Dual_Link"  },
    // ---- short names (people drop the "OS" when talking) -----------------
    { "astro",      "AstroOS"    },
    { "spotify",    "SpotifyOS"  },
    { "tkd",        "TKDOS"      },
    { "sport",      "SportOS"    },
    { "game",       "GameOS"     },
    { "explorer",   "ExplorerOS" },
    { "tactical",   "TacticalOS" },
    { "book",       "BookOS"     },
    { "waypoint",   "WaypointOS" },
    // v1.7.1: bare "dual" now arms the DualBoot slot swap (see kPhrase/kPtt);
    // Dual_Link keeps the unambiguous "dual link" / "duallink" names above.
    // { "dual",    "Dual_Link"  },
    // ---- moods and intents (the fun part) --------------------------------
    { "itsgotime",          "SportOS"    },   // "it's go time"
    { "gymtime",            "SportOS"    },
    { "workouttime",        "SportOS"    },
    { "letstrain",          "SportOS"    },
    { "letssweat",          "SportOS"    },
    { "workout",            "SportOS"    },
    { "exercise",           "SportOS"    },
    { "imbored",            "GameOS"     },   // "I'm bored"
    { "iwanttoplay",        "GameOS"     },
    { "playagame",          "GameOS"     },
    { "gametime",           "GameOS"     },
    { "havesomefun",        "GameOS"     },
    { "killtime",           "GameOS"     },
    { "bored",              "GameOS"     },
    { "bootsometunes",      "SpotifyOS"  },   // "boot some tunes"
    { "bootuptunes",        "SpotifyOS"  },
    { "playmusic",          "SpotifyOS"  },
    { "listentomusic",      "SpotifyOS"  },   // "listen to music"
    { "spotifytime",        "SpotifyOS"  },
    { "tunein",             "SpotifyOS"  },
    { "tunes",              "SpotifyOS"  },
    { "tune",               "SpotifyOS"  },
    { "songs",              "SpotifyOS"  },
    { "song",               "SpotifyOS"  },
    { "music",              "SpotifyOS"  },
    { "takemetothestars",   "AstroOS"    },
    { "stargazing",         "AstroOS"    },
    { "spacestuff",         "AstroOS"    },
    { "astronomytime",      "AstroOS"    },
    { "lookatthesky",       "AstroOS"    },
    { "stars",              "AstroOS"    },
    { "dosometkd",          "TKDOS"      },
    { "taekwondo",          "TKDOS"      },
    { "tkdtraining",        "TKDOS"      },
    { "practicemartialarts","TKDOS"      },
    { "martialarts",        "TKDOS"      },
    { "letsexplore",        "ExplorerOS" },
    { "goexploring",        "ExplorerOS" },
    { "adventuretime",      "ExplorerOS" },
    { "takeawalk",          "ExplorerOS" },
    { "getoutside",         "ExplorerOS" },
    { "explore",            "ExplorerOS" },
    { "hike",               "ExplorerOS" },
    { "fieldmode",          "TacticalOS" },
    { "tacticalmode",       "TacticalOS" },
    { "godark",             "TacticalOS" },
    { "missiontime",        "TacticalOS" },
    { "whatstheplan",       "TacticalOS" },
    { "lockitin",           "TacticalOS" },
    { "iwanttoread",        "BookOS"     },
    { "readabook",          "BookOS"     },
    { "readingtime",        "BookOS"     },
    { "booktime",           "BookOS"     },
    { "readmeastory",       "BookOS"     },
    { "read",               "BookOS"     },
    { "whereami",           "WaypointOS" },   // "where am I"
    { "markthisspot",       "WaypointOS" },
    { "setawaypoint",       "WaypointOS" },
    { "ineeddirections",    "WaypointOS" },
    { "howdoigetthere",     "WaypointOS" },
    { "navigate",           "WaypointOS" },
    { "linkup",             "Dual_Link"  },
    { "pairwithmyphone",    "Dual_Link"  },
    { "connecttomyphone",   "Dual_Link"  },
    { "syncwithmyphone",    "Dual_Link"  },
    { "connectmyphone",     "Dual_Link"  },
};
#define X_NOS ((int)(sizeof(kOs)/sizeof(kOs[0])))

// ---- state -----------------------------------------------------------------
static bool       g_xander_active = false;
static lv_obj_t  *g_x_scr      = nullptr;
static lv_obj_t  *g_x_resp_lbl = nullptr;
static int        g_x_top      = 1;      // 0 = puzzles, 1 = home, 2 = knowledge, 3 = stories
// ---- v2.14: the stories, and where you left each one ----------------------
//  Declared up here because x_depth() and x_back() are defined a hundred lines
//  before the story tables, and a fourth page that back does not know about is
//  a page you cannot leave.
#define X_NST 3
static int        g_x_st_view = 0;      // 0 shelf, 1 reading
static int        g_x_st_id   = 0;
static int        g_x_st_node = 0;      // absolute node index
static int        g_xst_pos[X_NST] = { 0, 6, 12 };
static uint8_t    g_xst_done  = 0;      // bit per story, 1 = reached an ending
// v2.15: a story has TWO endings, and the finished bit threw the second one
// away -- replaying for the other door still just read "finished".  One bit per
// (story, ending), so the shelf can say "1 of 2 doors" and mean it.
static uint8_t    g_xst_ends  = 0;
// v2.15: the layer is alive while it is open.
static lv_obj_t  *g_x_strip    = nullptr;   // the header's clock/mic/battery label
static uint32_t   g_x_strip_ms = 0;
static uint32_t   g_x_read_ms  = 0;         // a deep page is open (screen hold)
static String     g_x_greet    = "";        // the line the home bubble is showing
static uint32_t   g_x_greet_ms = 0;
#define X_READ_HOLD_MS 180000UL             // longest a reading page keeps the panel lit
static uint8_t    g_xst_hist[X_NST][8];      // v2.15: the pages a night came through
static uint8_t    g_xst_hn[X_NST];
static uint32_t   g_x_kill_arm = 0;          // v2.15: first tap on Shut down
static bool       g_x_save_done = false;     // v2.15: card import attempted
static int        g_x_st_last  = -1;         // v2.16: the night you were last in
static uint32_t   g_x_reset_arm = 0;         // v2.16: first tap on Start over
static bool       g_x_console  = false;
static int        g_x_orb      = 0;      // 0 idle, 1 success, 2 error
static String     g_x_last     = "";
// puzzle sub-state
static int        g_x_puz_view = 0;      // 0 categories, 1 puzzle card
static bool       g_x_ans_mode = false; // v2.6: the console is taking an answer
static int        g_x_puz_id   = 0;      // index into kPuzzles
static bool       g_x_puz_hint = false, g_x_puz_reveal = false;
// knowledge sub-state
static int        g_x_kn_view  = 0;      // 0 categories, 1 article list, 2 detail
static String     g_x_kn_cat   = "";
static int        g_x_kn_art   = 0;      // index into kArticles
static int        g_x_kn_last  = -1;     // v2.13: the bookmark (last page opened)
static bool       g_x_kn_more  = false;
// virtual hardware flags (no GPS/beacon silicon wired -> tracked in software)
static bool       g_x_gps = false, g_x_beacon = false;
// persisted stats + settings
static Preferences g_xprefs;
static bool       g_x_loaded = false;
static long       g_xs_solved=0, g_xs_correct=0, g_xs_hints=0, g_xs_streak=0, g_xs_best=0, g_xs_hard=0;
static long       g_xs_bits  = 0;      // v2.6: the currency hints are bought with
static bool       g_x_voice=false, g_x_alwayson=false;
// v1.6: BOOT-to-talk is a setting, not a given.  Off means the physical button
// is left alone entirely -- no mic, no window, no overlay -- for anyone who
// bumps that corner of the watch (it sits right where a thumb rests) or who
// would rather the only way in is the always-on wake word.
static bool       g_x_bootmic=true;
static int        g_x_battlim=1;
// v1.9: the scoreboard.  A mask per trial and per article means "never hand the
// same one back", which is the difference between a gamesmaster and a shuffle.
static long       g_xs_shen        = 0;      // things he found funny
static uint32_t   g_xsolved_mask   = 0;      // bit per kPuzzles index
static uint64_t   g_xread_mask     = 0;      // bit per kArticles index (x_nart() <= 64)
static String     g_x_echo         = "";     // the line you typed, echoed back
static lv_obj_t  *g_x_orb_obj      = nullptr;
static int        g_x_hint_run     = 0;      // consecutive hints taken

// ---- v1.8 interface state ---------------------------------------------------
// The console's "did you mean" chips.  A miss used to end at a random apology;
// the two best near-matches out of the tables above are kept here instead, so a
// typo or a half-remembered name is one tap from the command it meant.
struct XSugg { String label; int intent; String bin; };   // intent < 0 -> launch bin
static XSugg      g_x_sugg[2];
static int        g_x_sugg_sc[2] = { 0, 0 };
static int        g_x_sugg_n = 0;
static bool       g_x_kb = true;            // console: software keyboard is up
static bool       g_x_voice_show = false;   // console: the spoken-phrase list is open

// ---- module forward declarations -------------------------------------------
static void xander_enter();
static void xander_exit();
static void xander_handle_swipe(SwipeEvent ev);
static void x_render();
static void x_process(String raw);
static void x_do(int intent);
// ---- v1.7.1 slot swap ("Dual Boot" / "Hero Time") --------------------------
// The state lives up here because x_do() and x_process() consult it long before
// the swap machinery itself, which is placed just above setup() so it can use
// the flash and overlay helpers without forward declarations of its own.
static uint32_t    g_swapArmMs   = 0;      // non-zero while the cancel window runs
static const char *g_swapImgPath = nullptr;
static bool        g_swapBack    = false;    // the window is for the way back
static void swap_arm();
static void swap_arm_back();
static bool swap_abort(const char *why);
static void swap_service();
static void boot_resolve();
static bool boot_prepare(const char *path);
static bool swap_back_to_dualboot(const char *bin_on_card, const char **why);
static void x_open_article(int i);
static void x_puz_grade(int correct);
static void x_launch_app(int i);
static void show_xander_settings();
static void xander_service();                    // deferred UI worker (runs in loop, never inside an LVGL event)
static void xander_free_launcher_screens();
static void xander_rebuild_launcher_screens();

// ---- v1.8: one back path, and the "did you mean" chips ----------------------
// x_back() is the single pop used by the header chevron, the knowledge back
// button and the right-swipe.  Before this, three separate copies of the "which
// sub-view is open" logic could disagree about where back went.
static String x_cap(const char *s);                 // text helpers, further down
static void   x_finish_info();
static int    x_voice_count();                      // push-to-talk module, further down
static const char *x_voice_label(int i);
static bool   x_voice_run(int i);
static bool   x_voice_tappable(int i);

static bool x_depth() {
    return g_x_console || (g_x_top == 0 && g_x_puz_view > 0) ||
           (g_x_top == 2 && g_x_kn_view > 0) ||
           (g_x_top == 3 && g_x_st_view > 0);
}
static bool x_back() {
    if (g_x_console) {
        // The console nests three levels: the phrase list, the keyboard, then
        // the console itself, so back peels one layer at a time.
        if (g_x_voice_show)      g_x_voice_show = false;
        else if (g_x_kb)         g_x_kb = false;
        else                   { g_x_console = false; g_x_kb = true; }
        x_render();
        return true;
    }
    if (g_x_top == 0 && g_x_puz_view > 0) { g_x_puz_view = 0; x_render(); return true; }
    if (g_x_top == 3 && g_x_st_view > 0) { g_x_st_view = 0; x_render(); return true; }
    if (g_x_top == 2 && g_x_kn_view > 0) {
        if (g_x_kn_view == 2) { if (g_x_kn_more) g_x_kn_more = false; else g_x_kn_view = 1; }
        else                    g_x_kn_view = 0;
        x_render();
        return true;
    }
    return false;
}
static void x_back_cb(lv_event_t *) {
    if (millis() - lastSwipeMs < CLICK_AFTER_SWIPE_MS) return;
 x_back(); }

static void x_sugg_clear() {
    g_x_sugg_n = 0;
    for (int i = 0; i < 2; i++) {
        g_x_sugg[i].label = ""; g_x_sugg[i].bin = ""; g_x_sugg[i].intent = -99;
        g_x_sugg_sc[i] = 0;
    }
}
// Lower-case, alphanumeric only -- the same shape the phrase tables use, so a
// typed "what time is it?" and the chip label "What time is it?" compare equal.
static String x_key_of(const char *s) {
    String r;
    for (const char *c = s; c && *c; c++) {
        char ch = *c;
        if (ch >= 'A' && ch <= 'Z') ch = (char)(ch - 'A' + 'a');
        if ((ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9')) r += ch;
    }
    return r;
}
// exact > contained ("open astro" holds "astro") > prefix ("spot" -> "spotify")
// > a shared opening run of 3+ characters.
static int x_sugg_score(const String &q, const String &key) {
    if (key.length() < 3 || q.length() < 2) return 0;
    if (q == key)            return (int)key.length() + 10;
    if (q.indexOf(key) >= 0) return (int)key.length() + 3;
    if (key.indexOf(q) >= 0) return (int)q.length() + 2;
    int m = q.length() < key.length() ? (int)q.length() : (int)key.length();
    int i = 0; while (i < m && q[i] == key[i]) i++;
    if (i >= 3) return i;
    // v2.15: a transposed pair ("whta time is it") scored zero and suggested
    // nothing -- the commonest typo there is.  One edit in the opening run.
    if (m >= 4) {
        int diff = 0;
        for (int k = 0; k < m && diff < 2; ) {
            if (q[k] == key[k]) { k++; continue; }
            if (k + 1 < m && q[k] == key[k + 1] && q[k + 1] == key[k]) { diff++; k += 2; }
            else { diff++; k++; }
        }
        if (diff <= 1) return m;
    }
    return 0;
}
static void x_sugg_add(const String &q, const String &key, const String &label,
                       int intent, const String &bin) {
    const int sc = x_sugg_score(q, key);
    if (sc < 4 || !label.length()) return;
    for (int j = 0; j < g_x_sugg_n; j++) if (g_x_sugg[j].label == label) return;
    int slot;
    if      (g_x_sugg_n < 2)      slot = g_x_sugg_n;
    else if (sc > g_x_sugg_sc[1]) slot = 1;
    else                          return;
    g_x_sugg[slot].label = label; g_x_sugg[slot].intent = intent; g_x_sugg[slot].bin = bin;
    g_x_sugg_sc[slot] = sc;
    if (g_x_sugg_n <= slot) g_x_sugg_n = slot + 1;
    if (g_x_sugg_n == 2 && g_x_sugg_sc[1] > g_x_sugg_sc[0]) {   // keep [0] the best
        XSugg t = g_x_sugg[0]; g_x_sugg[0] = g_x_sugg[1]; g_x_sugg[1] = t;
        int  ts = g_x_sugg_sc[0]; g_x_sugg_sc[0] = g_x_sugg_sc[1]; g_x_sugg_sc[1] = ts;
    }
}
static void x_sugg_from(const String &raw) {
    x_sugg_clear();
    const String q = x_key_of(raw.c_str());
    if (q.length() < 2) return;
    for (int i = 0; i < X_NTMPL; i++)
        x_sugg_add(q, x_key_of(kTmpl[i].label), kTmpl[i].label, kTmpl[i].intent, "");
    for (int i = 0; i < X_NCHIP; i++)
        x_sugg_add(q, x_key_of(kChips[i].label), kChips[i].label, kChips[i].intent, "");
    for (int i = 0; i < X_NOS; i++)   // keys are already joined and lower-case
        x_sugg_add(q, kOs[i].say, String("Open ") + kOs[i].bin, -1, kOs[i].bin);
    // v2.15: the layer's own pages were not suggestable at all, so a near miss
    // for "stories" offered a radio command instead of the door.
    x_sugg_add(q, "stories", "Stories", XI_OPEN_STORIES,   "");
    x_sugg_add(q, "trials",  "Trials",  XI_OPEN_PUZZLES,   "");
    x_sugg_add(q, "library", "Library", XI_OPEN_KNOWLEDGE, "");
}
static void x_sugg_cb(lv_event_t *e) {
    if (millis() - lastSwipeMs < CLICK_AFTER_SWIPE_MS) return;
    const int i = (int)(intptr_t)lv_event_get_user_data(e);
    if (i < 0 || i >= g_x_sugg_n) return;
    const XSugg sg = g_x_sugg[i];
    x_sugg_clear();
    if (sg.intent >= 0) { x_do(sg.intent); return; }
    const int idx = app_resolve(sg.bin.c_str());
    if (idx >= 0) { x_launch_app(idx); return; }
    g_x_last = sg.bin + " is not on the card."; g_x_orb = 2; x_finish_info();
}

// Deferred Xander UI actions.  Xander (re)builds its screen OUTSIDE any LVGL
// event callback for two reasons:
//   1. so we never free the very widget that is currently handling a click
//      (the puzzle / knowledge / console buttons all trigger a re-render), and
//   2. so we can free the heavy launcher screens BEFORE building Xander's
//      views -- which stops the LVGL object pool from running dry (the old
//      "only the XANDER title paints, then the whole UI freezes" bug).
enum XAction { XA_NONE = 0, XA_ENTER, XA_RENDER, XA_EXIT };
static volatile XAction g_x_action = XA_NONE;
// v1.1: OPEN_CLOCK_SETTINGS leaves Xander and lands on the Settings screen
// instead of the clock face.  Handled in the deferred worker, never inline.
static bool              g_x_goto_settings = false;
// v1.1: an NTP round-trip blocks for up to ~10 s.  Commands only ARM it here
// (g_x_sync_at = deadline); xander_service() runs it from loop() one paint
// later, so "Syncing..." is actually on screen first and we never call into
// LVGL recursively from an event callback.
static uint32_t          g_x_sync_at = 0;
static bool              g_x_sync_date = false;
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
    g_xs_bits    = g_xprefs.getLong("bits", 10);
    g_xs_shen    = g_xprefs.getLong("shen", 0);
    g_xsolved_mask = (uint32_t)g_xprefs.getULong("smask", 0);
    g_xread_mask   = (uint64_t)(uint32_t)g_xprefs.getULong("rmask", 0) |
                     ((uint64_t)(uint32_t)g_xprefs.getULong("rmask_hi", 0) << 32);
    g_x_kn_last    = (int)g_xprefs.getInt("last", -1);   // v2.13: bookmark
    g_x_charm      = g_xprefs.getInt("charm", 1);
    g_x_nick       = g_xprefs.getString("nick", "");
    g_x_st_last    = g_xprefs.getInt("stcur", -1);   // v2.16: the Resume row
    // v2.14: the stories remember the page, not just the score.  Packed one byte
    // per story because a node index never exceeds 255 in a table this size.
    {
        const uint32_t sp = g_xprefs.getULong("stpos", 0);
        g_xst_done = (uint8_t)g_xprefs.getULong("stdone", 0);
        g_xst_ends = (uint8_t)(g_xprefs.getULong("stend", 0) & 0x3F);
        for (int i = 0; i < X_NST; i++)
            g_xst_pos[i] = (int)((sp >> (8 * i)) & 0xFFu);
        // Deliberately NOT range-checked here: the story table is declared far
        // below this point, and naming it up here is a compile error.  A never-
        // opened story reads back as 0, which is only page one of story one, so
        // every place that reads a bookmark clamps it against its own story
        // and writes the fixed value back.
    }
    g_x_voice    = g_xprefs.getBool("voice", false);
    g_x_alwayson = g_xprefs.getBool("aon", false);
    g_x_bootmic  = g_xprefs.getBool("bootmic", true);   // default: on
    g_x_battlim  = g_xprefs.getInt("blim", 1);
    g_xprefs.end();
    g_x_loaded = true;
    // v2.15: only when NVS looks untouched.  A watch with real progress never
    // has the card file read over it.
    if (!g_x_save_done) {
        g_x_save_done = true;
        if (g_xs_solved == 0 && g_xs_correct == 0 && g_xread_mask == 0) x_stats_import();
    }
}
static void x_stats_save() {
    g_xprefs.begin("xander", false);
    g_xprefs.putLong("solved", g_xs_solved);
    g_xprefs.putLong("correct", g_xs_correct);
    g_xprefs.putLong("hints", g_xs_hints);
    g_xprefs.putLong("streak", g_xs_streak);
    g_xprefs.putLong("best", g_xs_best);
    g_xprefs.putLong("hard", g_xs_hard);
    g_xprefs.putLong("bits", g_xs_bits);
    g_xprefs.putLong("shen", g_xs_shen);
    g_xprefs.putULong("smask", (uint32_t)g_xsolved_mask);
    g_xprefs.putULong("rmask",    (uint32_t)(g_xread_mask & 0xFFFFFFFFull));
    g_xprefs.putULong("rmask_hi", (uint32_t)(g_xread_mask >> 32));
    g_xprefs.putInt("last", g_x_kn_last);        // v2.13: bookmark survives reboot
    g_xprefs.end();
}

// ---- v2.15: progress lives on the card too --------------------------------
// NVS survives a normal Upload but not Tools -> Erase All Flash, which this
// sketch's own notes name as the thing that eats the model partition.  One
// key=value per line, written when you leave the layer and read back only when
// NVS looks untouched -- so a wipe costs a reflash, not every door opened.
#define X_SAVE_PATH "/WATCH/Xander/save.txt"
static void x_stats_export() {
    if (!g_sd_ok) return;
    File f = SD_MMC.open(X_SAVE_PATH, FILE_WRITE);
    if (!f) { USBSerial.println("[xander] save.txt not writable"); return; }
    f.printf("v=1\nsolved=%ld\ncorrect=%ld\nhints=%ld\nstreak=%ld\nbest=%ld\n"
             "hard=%ld\nbits=%ld\nshen=%ld\nsmask=%lu\nrmask=%lu\nrmask_hi=%lu\n"
             "last=%d\nstpos=%lu\nstdone=%lu\nstend=%lu\n",
             g_xs_solved, g_xs_correct, g_xs_hints, g_xs_streak, g_xs_best, g_xs_hard,
             g_xs_bits, g_xs_shen,
             (unsigned long)g_xsolved_mask,
             (unsigned long)(uint32_t)(g_xread_mask & 0xFFFFFFFFull),
             (unsigned long)(uint32_t)(g_xread_mask >> 32),
             g_x_kn_last,
             (unsigned long)x_st_pack_pos(),
             (unsigned long)g_xst_done, (unsigned long)g_xst_ends);
    f.close();
    USBSerial.printf("[xander] progress -> %s\n", X_SAVE_PATH);
}
static void x_stats_import() {
    if (!g_sd_ok) return;
    File f = SD_MMC.open(X_SAVE_PATH, FILE_READ);
    if (!f) return;
    int applied = 0;
    while (f.available()) {
        String line = f.readStringUntil('\n');
        line.trim();
        const int eq = line.indexOf('=');
        if (eq <= 0) continue;
        const String k = line.substring(0, eq);
        const long   v = line.substring(eq + 1).toInt();
        if      (k == "solved")   { g_xs_solved  = v; applied++; }
        else if (k == "correct")  { g_xs_correct = v; applied++; }
        else if (k == "hints")    { g_xs_hints   = v; applied++; }
        else if (k == "streak")   { g_xs_streak  = v; applied++; }
        else if (k == "best")     { g_xs_best    = v; applied++; }
        else if (k == "hard")     { g_xs_hard    = v; applied++; }
        else if (k == "bits")     { g_xs_bits    = v; applied++; }
        else if (k == "shen")     { g_xs_shen    = v; applied++; }
        else if (k == "smask")    { g_xsolved_mask = (uint32_t)v; applied++; }
        else if (k == "rmask")    { g_xread_mask = (g_xread_mask & 0xFFFFFFFF00000000ull) |
                                                    (uint64_t)(uint32_t)v; applied++; }
        else if (k == "rmask_hi") { g_xread_mask = (g_xread_mask & 0xFFFFFFFFull) |
                                                    ((uint64_t)(uint32_t)v << 32); applied++; }
        else if (k == "last")     { g_x_kn_last = (int)v; applied++; }
        else if (k == "stpos")    { for (int i = 0; i < X_NST; i++)
                                        g_xst_pos[i] = (int)((v >> (8 * i)) & 0xFF); applied++; }
        else if (k == "stdone")   { g_xst_done = (uint8_t)v; applied++; }
        else if (k == "stend")    { g_xst_ends = (uint8_t)(v & 0x3F); applied++; }
    }
    f.close();
    if (!applied) { USBSerial.println("[xander] save.txt held nothing usable"); return; }
    x_stats_save();
    x_st_save();
    USBSerial.printf("[xander] restored %d field(s) from the card\n", applied);
}
static void x_settings_save() {
    g_xprefs.begin("xander", false);
    g_xprefs.putBool("voice", g_x_voice);
    g_xprefs.putBool("aon", g_x_alwayson);
    g_xprefs.putBool("bootmic", g_x_bootmic);
    g_xprefs.putInt("blim", g_x_battlim);
    g_xprefs.putInt("charm", g_x_charm);
    g_xprefs.end();
}

// ---- v1.9: scoreboard, nickname, greeting, secrets -------------------------
static bool x_solved_bit(int i) { return ((g_xsolved_mask >> (i & 31)) & 1u) != 0; }
// v2.2: the article mask is 64 bits wide, so stop folding the index with & 31 -
// that made article 33 alias article 1.
static bool x_read_bit(int i) { return i >= 0 && i < 64 && ((g_xread_mask >> i) & 1ull) != 0; }
static int  x_vault_count() {
    int c = 0;
    for (int i = 0; i < X_NPUZ; i++) if (x_solved_bit(i)) c++;
    return c;
}
static int x_pick_unsolved(int prefer) {
    for (int k = 0; k < X_NPUZ; k++) {
        int j = (prefer + k) % X_NPUZ;
        if (!x_solved_bit(j)) return j;
    }
    return (int)(millis() % (uint32_t)X_NPUZ);
}
static int x_pick_unread(int prefer) {
    for (int k = 0; k < x_nart(); k++) {
        int j = (prefer + k) % x_nart();
        if (!x_read_bit(j)) return j;
    }
    return (int)(millis() % (uint32_t)x_nart());
}
// ---- v2.3: he names you by what you have actually done, and the name grows ----
//  The first version of this picked a title once, on first entry, then returned
//  early forever -- so whatever you were after three articles you stayed.  The
//  ladder is monotone (reads and solves never go down), so it is safe to re-rank
//  on every entry and only write NVS when the rung actually changed.
static int x_read_count() {
    int n = 0;
    for (int i = 0; i < x_nart(); i++) if (x_read_bit(i)) n++;
    return n;
}
static int x_solved_count() {
    int n = 0;
    for (int i = 0; i < X_NPUZ; i++) if (x_solved_bit(i)) n++;
    return n;
}
static void x_nick_pick() {
    const int rd = x_read_count(), sv = x_solved_count();
    const char *n = "Captain";                  // walked in the door
    if      (g_xs_hints > 6)      n = "Riddler";     // asks for the hint
    if      (g_xs_correct >= 8)   n = "Ace";         // gets them right
    if      (sv >= 10)            n = "Owl";
    if      (rd >= 10)            n = "Scholar";
    if      (rd >= 25)            n = "Archivist";
    if      (rd >= 40)            n = "Oracle";
    if      (rd >= x_nart() && sv >= X_NPUZ) n = "The Mentalist";   // the whole library, every trial
    // String on the left: Arduino's String defines operator==(const char*),
    // not the other way round, so "const char* == String" does not compile.
    if (g_x_nick == n) return;
    g_x_nick = n;
    g_xprefs.begin("xander", false);
    g_xprefs.putString("nick", g_x_nick);
    g_xprefs.end();
}
// Time, battery, your name, then his line -- the same shape a person uses.
static String x_greet_line() {
    String s;
    struct tm t;
    if (getLocalTime(&t, 5)) {
        if      (t.tm_hour < 5)  s = "Late. Very late.";
        else if (t.tm_hour < 12) s = "Morning.";
        else if (t.tm_hour < 18) s = "Afternoon.";
        else                     s = "Evening.";
    } else s = "The clock is not set.";
    if (g_x_nick.length()) { s += " "; s += g_x_nick; s += "."; }
    if (g_batt_pct >= 0)   { s += " "; s += g_batt_pct; s += "%."; }
    s += " ";
    s += x_say(X_R_GREET, X_NBANK(X_R_GREET));
    return s;
}
// A miss is not a dead end: the closest command he just worked out gets named.
static String x_miss_line() {
    String s = x_say(X_R_UNK, X_NBANK(X_R_UNK));
    if (g_x_sugg_n > 0 && g_x_sugg[0].label.length()) {
        s += " My money is on \"";
        s += g_x_sugg[0].label;
        s += "\".";
    }
    return s;
}
static bool x_secret(const String &in) {
    for (int i = 0; i < X_NSECRET; i++) {
        if (in.indexOf(kSecrets[i].key) >= 0) {
            g_x_last = kSecrets[i].say;
            g_x_orb  = 4;                       // the wink
            g_xs_shen++;
            x_stats_save();
            x_finish_info();
            return true;
        }
    }
    return false;
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

// ---- v1.1 phrase/alias matcher helpers -------------------------------------
// Words a speaker (or a recogniser) sprinkles around a command.  They are
// ignored when deciding whether the utterance was ONLY "sync time" etc.
static bool x_is_filler(const String &w) {
    static const char *F[] = { "xander","hey","ok","okay","please","now","for",
                               "me","the","a","an","to","just","can","you","i",
                               "want","and","my","is","it","what","whats","s",
                               // v1.1 moods: "i am bored", "do some tkd",
                               // "put on some music", "tune in"
                               "am","some","on","up","in","do" };
    for (unsigned i = 0; i < sizeof(F)/sizeof(F[0]); i++) if (w == F[i]) return true;
    return false;
}
// Token equality with a forgiving prefix rule, so "todays"/"today" and
// "astro"/"astroos" still line up without opening the door to 2-letter noise.
static bool x_tok_eq(const String &a, const String &b) {
    if (a == b) return true;
    int m = a.length() < b.length() ? a.length() : b.length();
    if (m < 4) return false;
    return a.substring(0, m) == b.substring(0, m);
}
// Lower-case, alphanumeric-only tokeniser with the word splits a recogniser
// loves to produce folded back into one token.
static int x_norm_tokens(const String &in, String *out, int maxn) {
    String s;
    s.reserve(in.length() + 8);
    for (size_t i = 0; i < in.length(); i++) {
        char c = (char)tolower((unsigned char)in[i]);
        bool alnum = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
        s += alnum ? c : ' ';
    }
    // Pad so the phrase rewrites below can require word boundaries.  The
    // apostrophe folds are what let "I'm bored" and "It's go time" reach the
    // single-token mood keys -- but "what's" is NOT folded, because the clock
    // phrases are written "what s ..." and must keep matching.
    s = " " + s + " ";
    s.replace(" i m ",        " im ");
    s.replace(" it s ",       " its ");
    s.replace(" let s ",      " lets ");
    s.replace(" wi fi ",      " wifi ");
    s.replace(" blue tooth ", " bluetooth ");
    s.replace(" dual link ",  " duallink ");
    return x_tokenize(s, out, maxn);
}
// Longest full phrase present in the utterance, IN ORDER, skipping filler
// words between its own words -- that is what makes "hey xander, sync the
// time please" hit "sync time" without needing an entry for every polite
// insertion.  Words outside the match must be filler too, otherwise a stray
// "time" phrase could be stolen by an unrelated command.
static bool x_phrase_match(const String *t, int n, int &outIntent) {
    int bestLen = 0;
    outIntent = -1;
    String pt[8];
    for (int p = 0; p < X_NPHASE; p++) {
        int pn = x_tokenize(String(kPhrase[p].words), pt, 8);
        if (pn == 0 || pn > n) continue;
        for (int start = 0; start < n; start++) {
            int k = 0, i = start;
            while (i < n && k < pn) {
                if (x_tok_eq(t[i], pt[k])) { k++; i++; }
                else if (x_is_filler(t[i])) { i++; }
                else break;
            }
            if (k < pn) continue;
            bool fillerOnly = true;
            for (int j = 0; j < n && fillerOnly; j++) {
                if (j >= start && j < i) continue;
                if (!x_is_filler(t[j])) fillerOnly = false;
            }
            if (!fillerOnly) continue;
            if (pn > bestLen) { bestLen = pn; outIntent = kPhrase[p].intent; }
        }
    }
    return outIntent >= 0;
}
// "open AstroOS" / "spotifyos" / "go to dual link" -> real .bin on the card.
// Returns true when it owned the utterance (including the honest "not on the
// card" reply), false to let the other matchers run.
static bool x_try_launch_os(String *t, int n) {
    bool verb = x_tok_has(t, n, "open")  || x_tok_has(t, n, "launch") ||
                x_tok_has(t, n, "start") || x_tok_has(t, n, "run")    ||
                x_tok_has(t, n, "boot")  || x_tok_has(t, n, "go")     ||
                x_tok_has(t, n, "play")  || x_tok_has(t, n, "put");
    String join;
    for (int i = 0; i < n; i++) join += t[i];
    // v1.7.1: an utterance that IS the name ("music", "song", "stars") launches
    // outright.  The substring pass below makes short keys require a verb to
    // stop "music status" yanking you into SpotifyOS, so a bare "music" only
    // worked because it happened to be the whole sentence.  Say so explicitly.
    for (int i = 0; i < X_NOS; i++) {
        if (join != kOs[i].say) continue;
        int eidx = app_resolve(kOs[i].bin);
        if (eidx < 0) {
            g_x_last = String(kOs[i].bin) + " is not on the card.";
            g_x_orb = 2; x_finish_info(); return true;
        }
        USBSerial.printf("[XANDER] exact launch -> %s (%s)\n",
                         kOs[i].bin, g_apps[eidx].path.c_str());
        x_launch_app(eidx);
        return true;
    }
    for (int i = 0; i < X_NOS; i++) {
        if (join.indexOf(kOs[i].say) < 0) continue;
        // A multi-word mood key ("itsgotime", "imbored", "bootsometunes") is
        // specific enough to stand alone; a short key ("music", "read",
        // "stars") still needs a verb or an otherwise-empty sentence, so
        // "music status" cannot yank you into SpotifyOS.
        if (!verb && strlen(kOs[i].say) < 6) {
            // No verb: the utterance must be the name itself ("astro os").
            bool pure = true;
            String kt[4]; int kn = x_tokenize(String(kOs[i].say), kt, 4);
            for (int a = 0; a < n && pure; a++) {
                if (x_is_filler(t[a])) continue;
                bool part = false;
                for (int b = 0; b < kn && !part; b++) part = x_tok_eq(t[a], kt[b]);
                if (!part) pure = false;
            }
            if (!pure) continue;
        }
        int idx = app_resolve(kOs[i].bin);
        if (idx < 0) {
            g_x_last = String(kOs[i].bin) + " is not on the card.";
            g_x_orb = 2;
            x_finish_info();
            return true;
        }
        USBSerial.printf("[XANDER] voice launch -> %s (%s)\n",
                         kOs[i].bin, g_apps[idx].path.c_str());
        x_launch_app(idx);          // OTA-copies the .bin and reboots
        return true;
    }
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
    for (int i = 0; i < x_nart(); i++) {
        String hay = String(x_art(i).title) + " " + x_art(i).id + " " +
                     x_art(i).category + " " + x_art(i).summary;
        String kw  = String(x_art(i).terms);
        hay.toLowerCase(); kw.toLowerCase();
        int s = 0;
        for (int k = 0; k < n; k++) {
            if (t[k].length() < 3 || x_is_stop(t[k])) continue;
            if (hay.indexOf(t[k]) >= 0) s += 2;
            // v2.2: a written synonym is the surer sign this is the article meant,
            // so it outscores an incidental word in a title.
            else if (kw.indexOf(t[k]) >= 0) s += 3;
        }
        if (s > best) { best = s; bi = i; }
    }
    return best > 0 ? bi : -1;
}

// ---- UI builders -----------------------------------------------------------
// v2.15: this label was painted once per render, so the clock on all four
// Xander pages read the time you arrived.  Refreshing the text in place is one
// label write -- no rebuild, no fade, no object-pool cost -- and it is the only
// clock the layer has, so it should not lie.
static void x_strip_refresh() {
    if (!g_x_strip) return;
    char clock[16];
    struct tm now_tm;
    if (getLocalTime(&now_tm, 5)) {
        if (g_time_24h) snprintf(clock, sizeof(clock), "%02d:%02d",
                                 now_tm.tm_hour, now_tm.tm_min);
        else {
            int h12 = now_tm.tm_hour % 12; if (!h12) h12 = 12;
            snprintf(clock, sizeof(clock), "%d:%02d%s", h12, now_tm.tm_min,
                     now_tm.tm_hour < 12 ? "a" : "p");
        }
    } else snprintf(clock, sizeof(clock), "--:--");
    const bool mic_on = g_x_alwayson || g_x_bootmic;
    char strip[40];
    snprintf(strip, sizeof(strip), "%s %s %d%%", clock,
             mic_on ? LV_SYMBOL_AUDIO : LV_SYMBOL_MUTE,
             g_batt_pct < 0 ? 0 : g_batt_pct);
    lv_label_set_text(g_x_strip, strip);
    lv_obj_set_style_text_color(g_x_strip, lv_color_hex(g_x_alwayson ? X_MOSS : X_SMOKE), 0);
    lv_obj_set_style_text_font(g_x_strip, &lv_font_montserrat_12, 0);
}

static void x_header(const char *title, const char *sub) {
    // v1.8: any screen with a sub-view behind it gets a real back target.  The
    // puzzle card used to be the only one, with its own button in the same
    // corner; this is the single affordance every depth now shares.
    if (x_depth()) {
        lv_obj_t *bk = lv_btn_create(g_x_scr);
        lv_obj_remove_style_all(bk);
        lv_obj_clear_flag(bk, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_size(bk, 46, 40);
        lv_obj_align(bk, LV_ALIGN_TOP_LEFT, 8, 8);
        lv_obj_set_style_bg_opa(bk, LV_OPA_TRANSP, 0);
        lv_obj_t *bl = lv_label_create(bk);
        lv_label_set_text(bl, LV_SYMBOL_LEFT);
        lv_obj_set_style_text_color(bl, lv_color_hex(theme_primary()), 0);
        lv_obj_set_style_text_font(bl, &lv_font_montserrat_16, 0);
        lv_obj_center(bl);
        lv_obj_add_event_cb(bk, x_back_cb, LV_EVENT_CLICKED, NULL);
    }
    // v2.13j: the Hawthorne crest, at 34 px, in the corner the back chevron
    // uses on sub-screens -- so the home pages carry a mark and the deep pages
    // carry a way out, never both fighting for the same square.
    if (!x_depth()) {
        lv_obj_t *cr = lv_obj_create(g_x_scr);
        lv_obj_remove_style_all(cr);
        lv_obj_clear_flag(cr, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_size(cr, 34, 34);
        lv_obj_align(cr, LV_ALIGN_TOP_LEFT, 12, 8);
        lv_obj_set_style_radius(cr, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(cr, lv_color_hex(X_INK), 0);
        lv_obj_set_style_bg_opa(cr, LV_OPA_COVER, 0);
        lv_obj_set_style_border_color(cr, lv_color_hex(X_BRASS), 0);
        lv_obj_set_style_border_width(cr, 1, 0);
        lv_obj_t *mono = lv_label_create(cr);
        lv_label_set_text(mono, "X");
        lv_obj_set_style_text_color(mono, lv_color_hex(X_BRASS), 0);
        lv_obj_set_style_text_font(mono, &lv_font_montserrat_16, 0);
        lv_obj_center(mono);
        // an inner hairline: engraved, not printed
        lv_obj_t *gring = lv_obj_create(cr);
        lv_obj_remove_style_all(gring);
        lv_obj_clear_flag(gring, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_size(gring, 26, 26);
        lv_obj_center(gring);
        lv_obj_set_style_radius(gring, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_border_color(gring, lv_color_hex(X_BRASS), 0);
        lv_obj_set_style_border_opa(gring, 60, 0);
        lv_obj_set_style_border_width(gring, 1, 0);
        lv_obj_set_style_bg_opa(gring, LV_OPA_TRANSP, 0);
    }
    lv_obj_t *t = lv_label_create(g_x_scr);
    lv_label_set_text(t, title);
    lv_obj_set_style_text_color(t, lv_color_hex(X_ACCENT), 0);
    lv_obj_set_style_text_font(t, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_letter_space(t, 2, 0);   // wordmark tracking
    lv_obj_align(t, LV_ALIGN_TOP_MID, 0, 14);
    // v1.9: a masthead, not a label.  A hairline rule under the wordmark and the
    // subtitle promoted to the same microcaps the section headers use, so all
    // four pages share one typographic voice.
    lv_obj_t *rule = lv_obj_create(g_x_scr);
    lv_obj_remove_style_all(rule);
    lv_obj_clear_flag(rule, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(rule, 76, 2);
    lv_obj_set_style_bg_color(rule, lv_color_hex(X_ACCENT), 0);
    lv_obj_set_style_bg_opa(rule, 200, 0);
    lv_obj_set_style_radius(rule, 1, 0);
    lv_obj_align(rule, LV_ALIGN_TOP_MID, 0, 38);
    if (sub) {
        lv_obj_t *s2 = lv_label_create(g_x_scr);
        lv_label_set_text(s2, sub);
        lv_obj_set_style_text_color(s2, lv_color_hex(X_SMOKE), 0);
        lv_obj_set_style_text_font(s2, &lv_font_montserrat_12, 0);
        lv_obj_set_style_text_letter_space(s2, 2, 0);
        lv_obj_align(s2, LV_ALIGN_TOP_MID, 0, 44);
    }
    // v2.15: one formatter, one label, kept current by xander_service().
    g_x_strip = lv_label_create(g_x_scr);
    x_strip_refresh();
    lv_obj_align(g_x_strip, LV_ALIGN_TOP_RIGHT, -14, 18);
}

// v2.13c: the parent is a parameter, not an assumption.  The WiFi screen is a
// separate lv_obj loaded over the launcher, so a list built with g_x_scr as its
// parent was painted onto the screen *behind* the modal: the message and the
// Scan button showed, the rows did not, and nothing could be tapped.
static lv_obj_t *x_list_in(lv_obj_t *parent, int y, int h) {
    lv_obj_t *l = lv_obj_create(parent);
    // v2.14.3: mark it for point_on_scroll_list() -- a vertical drag that starts
    // here scrolls the list and must not also navigate.
    lv_obj_add_flag(l, LV_OBJ_FLAG_USER_1);
    lv_obj_set_size(l, LCD_WIDTH - 30, h);
    lv_obj_align(l, LV_ALIGN_TOP_MID, 0, y);
    lv_obj_set_style_bg_opa(l, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(l, 0, 0);
    lv_obj_set_style_pad_all(l, 6, 0);
    lv_obj_set_style_pad_row(l, 10, 0);
    lv_obj_set_style_pad_right(l, 2, 0);
    // A hairline accent scrollbar instead of the default grey slab: the list is
    // the main surface on three of the four Xander screens.
    lv_obj_set_style_width(l, 3, LV_PART_SCROLLBAR);
    lv_obj_set_style_radius(l, 2, LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_color(l, lv_color_hex(theme_primary()), LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_opa(l, LV_OPA_40, LV_PART_SCROLLBAR);
    lv_obj_set_flex_flow(l, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(l, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START);
    lv_obj_set_scroll_dir(l, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(l, LV_SCROLLBAR_MODE_AUTO);
    return l;
}
static lv_obj_t *x_list(int y, int h) { return x_list_in(g_x_scr, y, h); }
static lv_obj_t *x_row_btn(lv_obj_t *p, const char *txt, uint32_t border,
                           lv_event_cb_t cb, intptr_t ud) {
    lv_obj_t *b = lv_btn_create(p);
    lv_obj_remove_style_all(b);
    lv_obj_set_width(b, LV_PCT(100));
    lv_obj_set_height(b, 52);
    // v1.7.1: a gradient plus a chevron makes a row read as a row instead of a
    // flat bordered rectangle, and the fixed-height label ellipsises so a long
    // app or article title stops running under the arrow.
    uint32_t fg = theme_fg();
    lv_obj_set_style_bg_color(b, lv_color_hex(fg), 0);
    lv_obj_set_style_bg_grad_color(b, lv_color_hex((fg & 0xFEFEFE) >> 1), 0);
    lv_obj_set_style_bg_grad_dir(b, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(b, 14, 0);
    lv_obj_set_style_border_color(b, lv_color_hex(border), 0);
    lv_obj_set_style_border_width(b, 1, 0);
    lv_obj_set_style_pad_hor(b, 14, 0);
    lv_obj_set_style_pad_ver(b, 12, 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_60, LV_STATE_PRESSED);
    lv_obj_t *l = lv_label_create(b);
    lv_label_set_text(l, txt);
    lv_label_set_long_mode(l, LV_LABEL_LONG_DOT);
    lv_obj_set_width(l, LCD_WIDTH - 92);
    lv_obj_set_style_text_color(l, lv_color_hex(X_PARCHMENT), 0);
    lv_obj_set_style_text_font(l, &lv_font_montserrat_16, 0);
    lv_obj_align(l, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_t *ch = lv_label_create(b);
    lv_label_set_text(ch, LV_SYMBOL_RIGHT);
    lv_obj_set_style_text_color(ch, lv_color_hex(X_SMOKE), 0);
    lv_obj_set_style_text_font(ch, &lv_font_montserrat_14, 0);
    lv_obj_align(ch, LV_ALIGN_RIGHT_MID, 0, 0);
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
    lv_obj_set_style_text_color(l, lv_color_hex(X_PARCHMENT), 0);
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

// ---- navigation pips (v1.7) ------------------------------------------------
// The three-page ring was documented by a text footer that was wrong on two of
// the three pages, and swipe was the only way to move.  These are tappable: a
// pip jumps to that page, the down-arrow leaves Xander.  Drawn only at the top
// of each page, because the puzzle/knowledge detail views own that strip with
// their own action bars.
static void x_goto_top(int top) {
    g_x_top = top;
    if (top == 0) { g_x_puz_view = 0; g_x_puz_hint = false; g_x_puz_reveal = false; }
    if (top == 2) { g_x_kn_view = 0; g_x_kn_cat = ""; }
    if (top == 3) { g_x_st_view = 0; }
    g_x_orb = 0; g_x_last = "";
    x_render();
}
static void x_pip_cb(lv_event_t *e) {
    if (millis() - lastSwipeMs < CLICK_AFTER_SWIPE_MS) return;
    int t = (int)(intptr_t)lv_event_get_user_data(e);
    if (t == g_x_top) return;
    // v2.15: this forced the keyboard back on for every pip jump, so the
    // console you returned to was never the one you left -- and hiding it is
    // what gives the list fifteen rows instead of three.
    g_x_console = false; g_x_voice_show = false;
    x_goto_top(t);
}
static void x_pager_exit_cb(lv_event_t *) {
    if (millis() - lastSwipeMs < CLICK_AFTER_SWIPE_MS) return;
    xander_exit();
}
static void x_pager() {
    if (g_x_top == 0 && g_x_puz_view != 0) return;
    if (g_x_top == 2 && g_x_kn_view != 0) return;
    if (g_x_top == 3 && g_x_st_view != 0) return;
    lv_obj_t *bar = lv_obj_create(g_x_scr);
    lv_obj_remove_style_all(bar);
    lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(bar, 360, 44);
    lv_obj_align(bar, LV_ALIGN_BOTTOM_MID, 0, -2);
    lv_obj_set_flex_flow(bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(bar, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    for (int i = 0; i < 4; i++) {
        lv_obj_t *hit = lv_btn_create(bar);
        lv_obj_remove_style_all(hit);
        lv_obj_clear_flag(hit, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_size(hit, 60, 44);
        lv_obj_set_style_bg_opa(hit, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(hit, 0, 0);
        lv_obj_t *dot = lv_obj_create(hit);
        lv_obj_remove_style_all(dot);
        lv_obj_clear_flag(dot, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_size(dot, i == g_x_top ? 24 : 8, 8);
        lv_obj_set_style_radius(dot, 4, 0);
        lv_obj_set_style_bg_color(dot, lv_color_hex(i == g_x_top ? X_ACCENT : 0x4E4E4E), 0);
        lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
        lv_obj_center(dot);
        lv_obj_add_event_cb(hit, x_pip_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
    }
    lv_obj_t *ex = lv_btn_create(bar);
    lv_obj_remove_style_all(ex);
    lv_obj_clear_flag(ex, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(ex, 70, 44);
    lv_obj_set_style_bg_opa(ex, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(ex, 0, 0);
    lv_obj_t *el = lv_label_create(ex);
    lv_label_set_text(el, LV_SYMBOL_DOWN "  exit");
    lv_obj_set_style_text_color(el, lv_color_hex(X_SMOKE), 0);
    lv_obj_set_style_text_font(el, &lv_font_montserrat_12, 0);
    lv_obj_center(el);
    lv_obj_add_event_cb(ex, x_pager_exit_cb, LV_EVENT_CLICKED, NULL);
}

// ---- callbacks -------------------------------------------------------------
static void x_cmd_open_cb(lv_event_t *) {
    if (millis() - lastSwipeMs < CLICK_AFTER_SWIPE_MS) return;
 g_x_console = true; g_x_last = ""; x_render(); }

static void x_kb_cb(lv_event_t *e) {
    lv_event_code_t code = lv_event_get_code(e);
    lv_obj_t *ta = (lv_obj_t *)lv_event_get_user_data(e);
    if (code == LV_EVENT_READY) {
        String txt = lv_textarea_get_text(ta);
        g_x_echo = txt;
        x_process(txt);
        if (g_x_console && ta) lv_textarea_set_text(ta, "");   // still in console -> clear
    } else if (code == LV_EVENT_CANCEL) {
        g_x_console = false; x_render();
    }
}
static void x_chip_cb(lv_event_t *e) {
    if (millis() - lastSwipeMs < CLICK_AFTER_SWIPE_MS) return;
    // <1000: legacy template chip | 1000..1999: app | >=2000: v1.1 command chip
    g_x_echo = "";                     // a tapped chip is not something you said
    intptr_t ud = (intptr_t)lv_event_get_user_data(e);
    if (ud >= 2000) {
        const uint8_t it = kChips[ud - 2000].intent;
        // v2.15: "Shut down" sat one row under "System status" and fired on the
        // first tap.  Everything else that throws state away is behind the
        // settings card; this one asks once, four seconds to answer.
        if (it == XI_SHUTDOWN && (!g_x_kill_arm || millis() - g_x_kill_arm > 4000UL)) {
            g_x_kill_arm = millis() ? millis() : 1;
            g_x_last = "Tap Shut down again to power off.";
            g_x_orb  = 0;
            x_render();
            return;
        }
        g_x_kill_arm = 0;
        // v2.16: the one chip whose damage tapping again cannot undo.
        if (it == XI_RESET_ALL && (!g_x_reset_arm || millis() - g_x_reset_arm > 4000UL)) {
            g_x_reset_arm = millis() ? millis() : 1;
            g_x_last = "Tap Start over again to erase all progress.";
            g_x_orb  = 2;
            x_render();
            return;
        }
        g_x_reset_arm = 0;
        x_do(it);
    }
    else if (ud >= 1000) x_launch_app((int)(ud - 1000));
    else                 x_do(kTmpl[ud].intent);
}
// puzzle
static void x_puz_cat_cb(lv_event_t *e) {
    if (millis() - lastSwipeMs < CLICK_AFTER_SWIPE_MS) return;
    int ci = (int)(intptr_t)lv_event_get_user_data(e);
    const char *cat = X_PCATS[ci];
    for (int i = 0; i < X_NPUZ; i++) if (String(kPuzzles[i].category) == cat) { g_x_puz_id = i; break; }
    g_x_puz_view = 1; g_x_puz_hint = false; g_x_puz_reveal = false; x_render();
}
static void x_puz_think_cb(lv_event_t *) {
    if (millis() - lastSwipeMs < CLICK_AFTER_SWIPE_MS) return;
 g_x_puz_reveal = !g_x_puz_reveal; x_render(); }
// v2.6: the way into an honest trial -- type the answer and let him judge it.
static void x_puz_type_cb(lv_event_t *) {
    g_x_ans_mode = true;
    g_x_console  = true;
    g_x_kb       = true;
    g_x_echo     = "";
    g_x_last     = "Type it. I will know.";
    x_render();
}
// v2.6: a key costs bits, and the second key is a different key.  Before this,
// Hint was free and re-tapping it re-showed the same sentence.
static const char *x_hint_text(const XPuzzle &p) {
    static char buf[96];
    if (g_x_hint_run <= 1) return p.hint;
    String a = String(p.answer);
    a.trim();
    int k = 0;
    while (k < (int)a.length() && !isalnum((unsigned char)a[k])) k++;
    char c = (k < (int)a.length()) ? a[k] : '?';
    if (g_x_hint_run == 2) {
        snprintf(buf, sizeof(buf), "Starts with \"%c\". %d characters, start to finish.",
                 c, (int)a.length());
    } else {
        String head6 = a.substring(0, a.length() < 6 ? a.length() : 6);
        snprintf(buf, sizeof(buf), "It opens: \"%s...\"", head6.c_str());
    }
    return buf;
}
static void x_puz_hint_cb(lv_event_t *)  {
    if (millis() - lastSwipeMs < CLICK_AFTER_SWIPE_MS) return;
    if (g_xs_bits < 5) {
        g_x_last = String("Not enough bits: ") + g_xs_bits +
                   ". A key costs 5 - clear a trial to earn some.";
        x_render();
        return;
    }
    g_xs_bits -= 5;
    g_xs_hints++;
    g_x_hint_run++;
    g_x_puz_hint = true;
    if (g_x_hint_run == 3)
        g_x_last = "Third key. I am starting to think you like me better than the puzzle.";
    x_stats_save();
    x_render();
}
// Never hands back a trial you already cleared.  Local category first, so a
// run through "logic" stays in logic until that door is empty.
static int x_puz_next_unsolved(int from, const char *cat) {
    for (int k = 1; k <= X_NPUZ; k++) {
        int j = (from + k) % X_NPUZ;
        if (x_solved_bit(j)) continue;
        if (cat && String(kPuzzles[j].category) != cat) continue;
        return j;
    }
    return -1;
}
static void x_puz_next_cb(lv_event_t *) {
    int j = x_puz_next_unsolved(g_x_puz_id, kPuzzles[g_x_puz_id].category);
    if (j < 0) j = x_puz_next_unsolved(g_x_puz_id, nullptr);
    if (j < 0) {
        g_x_last = "Every trial is cleared. Show off.";
        g_x_orb  = 4;
        x_render();
        return;
    }
    g_x_puz_id   = j;
    g_x_puz_hint = false; g_x_puz_reveal = false;
    g_x_last = "";
    x_render();
}
static void x_puz_got_cb(lv_event_t *e) {
    if (millis() - lastSwipeMs < CLICK_AFTER_SWIPE_MS) return;
    x_puz_grade((int)(intptr_t)lv_event_get_user_data(e) != 0);
}
static void x_puz_grade(int correct) {
    // v2.15: revealing the answer counts as assisted too.  It did not, so
    // Answer -> "I got it" was six taps to a full streak, +4 bits and a vault
    // tick -- with the answer printed on the card at the time.
    const bool assisted = g_x_puz_hint || g_x_puz_reveal;
    String msg;
    int orb;
    if (correct) {
        g_xs_correct++;
        g_xs_solved++;                      // v2.5: moved in here - a miss used to count
        g_xsolved_mask |= (1u << (g_x_puz_id & 31));
        if (!assisted) {
            g_xs_streak++;
            if (g_xs_streak > g_xs_best) g_xs_best = g_xs_streak;
        }
        if (kPuzzles[g_x_puz_id].difficulty > g_xs_hard)
            g_xs_hard = kPuzzles[g_x_puz_id].difficulty;
        g_xs_bits += assisted ? 2 : 4;       // v2.6: a hint costs 5, so keys pay for themselves
        orb = 4;                              // the wink
        msg = assisted ? String("You took the key. It counts as assisted.")
                       : String(x_say(X_R_RIGHT, X_NBANK(X_R_RIGHT)));
    } else {
        g_xs_streak = 0;
        orb = 2;
        msg = String(x_say(X_R_SASS, X_NBANK(X_R_SASS)));
    }
    g_x_hint_run = 0;
    x_stats_save();
    x_puz_next_cb(nullptr);
    // x_puz_next_cb clears the line on its way to the next card, so restore it
    // afterwards -- before this, "Correct. Nicely done." was wiped a line later
    // and never actually reached the screen.
    g_x_last = msg;
    g_x_orb  = orb;
}
// v2.12: a door on the trial card -- letter badge, option, accent hairline.
// 14 px text (not 16) so a full option sentence still fits inside 294 px, and
// LV_LABEL_LONG_DOT ellipsises anything that does not.
static lv_obj_t *x_opt_btn(lv_obj_t *p, const char *tag, const char *txt,
                           uint32_t border, lv_event_cb_t cb, intptr_t ud) {
    lv_obj_t *b = lv_btn_create(p);
    lv_obj_remove_style_all(b);
    lv_obj_clear_flag(b, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_width(b, LV_PCT(100));
    lv_obj_set_height(b, 46);
    lv_obj_set_style_bg_color(b, lv_color_hex(theme_fg()), 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_70, 0);
    lv_obj_set_style_radius(b, 12, 0);
    lv_obj_set_style_border_color(b, lv_color_hex(border), 0);
    lv_obj_set_style_border_width(b, 1, 0);
    lv_obj_set_style_pad_hor(b, 10, 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, LV_STATE_PRESSED);
    lv_obj_t *t = lv_label_create(b);
    lv_label_set_text(t, tag);
    lv_obj_set_style_text_color(t, lv_color_hex(X_ACCENT), 0);
    lv_obj_set_style_text_font(t, &lv_font_montserrat_14, 0);
    lv_obj_align(t, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_t *l = lv_label_create(b);
    lv_label_set_text(l, txt);
    lv_label_set_long_mode(l, LV_LABEL_LONG_DOT);
    lv_obj_set_width(l, LCD_WIDTH - 118);
    lv_obj_set_style_text_color(l, lv_color_hex(X_PARCHMENT), 0);
    lv_obj_set_style_text_font(l, &lv_font_montserrat_14, 0);
    lv_obj_align(l, LV_ALIGN_LEFT_MID, 24, 0);
    if (cb) lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, (void *)ud);
    return b;
}
// The option index and the trial index travel in one pointer: (puzzle << 2) | door.
static void x_puz_pick_cb(lv_event_t *e) {
    if (millis() - lastSwipeMs < CLICK_AFTER_SWIPE_MS) return;
    const int code = (int)(intptr_t)lv_event_get_user_data(e);
    const int pi   = code >> 2;
    const int door = code & 3;
    if (pi < 0 || pi >= X_NPUZ || door > 2) return;
    if (pi != g_x_puz_id) return;                 // card changed under the tap
    const bool hit = (door == (int)kPuzOpts[pi].right);
    x_puz_grade(hit ? 1 : 0);
    if (!hit) {
        // Same shape as the typed path: he says his line, then the answer.
        g_x_last = String(x_say(X_R_SASS, X_NBANK(X_R_SASS))) +
                   " It was: " + kPuzzles[pi].answer;
        g_x_orb  = 2;
    }
    x_render();
}
// knowledge
static void x_kn_cat_cb(lv_event_t *e) {
    if (millis() - lastSwipeMs < CLICK_AFTER_SWIPE_MS) return;
    int ci = (int)(intptr_t)lv_event_get_user_data(e);
    g_x_kn_cat = X_KCATS[ci]; g_x_kn_view = 1; x_render();
}
static void x_kn_art_cb(lv_event_t *e) {
    if (millis() - lastSwipeMs < CLICK_AFTER_SWIPE_MS) return;
    g_x_kn_art = (int)(intptr_t)lv_event_get_user_data(e);
    g_x_kn_view = 2; g_x_kn_more = false; x_render();
}
static void x_kn_more_cb(lv_event_t *) {
    if (millis() - lastSwipeMs < CLICK_AFTER_SWIPE_MS) return;
 g_x_kn_more = !g_x_kn_more; x_render(); }

// ---- v2.13: a library, not a file drawer ----------------------------------
//  A bookcase tells you three things a list does not: what is on each shelf,
//  how much of it you have read, and where you last were.  So the field list
//  became SHELVES with a read count per shelf and a BOOKMARK row on top, the
//  pages on a shelf sort alphabetically instead of in table order, an unread
//  page keeps the accent spine while a read one goes quiet grey, and the page
//  itself can be put back as unread for the ones worth finding again.
static int x_cat_read(const char *key) {
    int c = 0;
    for (int i = 0; i < x_nart(); i++)
        if (String(x_art(i).category) == key && x_read_bit(i)) c++;
    return c;
}
// Own comparator: strcasecmp lives in <strings.h>, which this sketch does not
// include, and a C++ implicit declaration is a hard error rather than a warning.
static int x_title_cmp(const char *a, const char *b) {
    while (*a && *b) {
        const int ca = tolower((unsigned char)*a);
        const int cb = tolower((unsigned char)*b);
        if (ca != cb) return ca - cb;
        a++; b++;
    }
    return (int)(unsigned char)*a - (int)(unsigned char)*b;
}
static void x_kn_resume_cb(lv_event_t *e) {
    if (millis() - lastSwipeMs < CLICK_AFTER_SWIPE_MS) return;
    const int i = (int)(intptr_t)lv_event_get_user_data(e);
    if (i < 0 || i >= x_nart()) return;
    x_open_article(i);                 // marks read again, opens the page
}
static void x_kn_search_cb(lv_event_t *) {
    if (millis() - lastSwipeMs < CLICK_AFTER_SWIPE_MS) return;
    g_x_echo     = "";
    g_x_ans_mode = false;
    g_x_console  = true;
    g_x_kb       = true;
    g_x_last     = "Name a thing. I will find the shelf.";
    x_render();
}
static void x_kn_toggle_cb(lv_event_t *) {
    if (millis() - lastSwipeMs < CLICK_AFTER_SWIPE_MS) return;
    if (g_x_kn_art < 0 || g_x_kn_art >= x_nart()) return;
    g_xread_mask ^= (1ull << (g_x_kn_art & 63));
    x_stats_save();
    x_render();
}
static void x_kn_back_cb(lv_event_t *)  {
    if (millis() - lastSwipeMs < CLICK_AFTER_SWIPE_MS) return;
    // One back path for the whole layer (v1.8): this button, the header chevron
    // and the right-swipe all call x_back(), so they cannot drift apart.
    x_back();
}

// ---- v1.9: the orb breathes while it works --------------------------------
static void x_orb_shadow_cb(void *obj, int32_t v) {
    lv_obj_set_style_shadow_width((lv_obj_t *)obj, v, 0);
}
static void x_orb_anim_stop() {
    if (!g_x_orb_obj) return;
    lv_anim_del(g_x_orb_obj, x_orb_shadow_cb);
    g_x_orb_obj = nullptr;
}
static void x_orb_breathe() {
    if (!g_x_orb_obj) return;
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, g_x_orb_obj);
    lv_anim_set_exec_cb(&a, x_orb_shadow_cb);
    lv_anim_set_values(&a, 18, 46);
    lv_anim_set_time(&a, 900);
    lv_anim_set_playback_time(&a, 900);
    lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_in_out);
    lv_anim_start(&a);
}
// The home caption is a fixed 112 px box with LONG_DOT and "system status" is
// ~170 characters, so its tail was unreachable.  A tap shows the whole line in
// the pill, which already wraps and scales its hold with the text.
static void x_caption_cb(lv_event_t *) {
    if (millis() - lastSwipeMs < CLICK_AFTER_SWIPE_MS) return;
    if (g_x_last.length()) ptt_flash(g_x_last, 6500);
}

// ---- v2.14: THE STORIES ----------------------------------------------------
//  The estate in those books is a set of games with a family inside them, and a
//  layer that only offers trials and articles has the furniture but not the
//  evening.  A story is six pages: an opening, two ways in, a hinge where the
//  two ways meet, and two endings.  Which way you went is remembered per story
//  in NVS, so leaving the page is not the same as leaving the story.
//
//  One flat node table with ABSOLUTE jump targets: a story is a range in it
//  (first .. first+pages-1) and a choice is simply a node index.  Nothing is
//  allocated or parsed at runtime, a bad index falls back to page one instead of
//  crashing, and the prose lives in flash.  An ending is the node with no
//  choices (n == 0) -- that is the whole format.
struct XStoryNode { const char *text; const char *a[3]; uint8_t to[3]; uint8_t n; };
static const XStoryNode kStory[] = {
    // ---- 1. The Brass Key (nodes 0-5) --------------------------------------
    { "The east corridor, 2 a.m. A brass key on the hall table and a card in his handwriting: "
      "One of these doors is honest. The other three are only locked. You have until the clocks disagree.",
      { "The door with scratched paint", "The door with new varnish", "" }, { 1, 2, 0 }, 2 },
    { "Scratched paint, dozens of times over, by someone who kept trying. The key turns. Inside: "
      "a chair facing a mirror, and a second key on the seat. Whoever sat here was watching the "
      "door you just came through.",
      { "Take the second key", "", "" }, { 3, 0, 0 }, 1 },
    { "The varnish is an hour old and the door is warm. There is no lock on this side at all, only "
      "a bolt, and it has been drawn from the corridor. Someone wanted this room shut from the "
      "outside.",
      { "Draw the bolt anyway", "", "" }, { 3, 0, 0 }, 1 },
    { "Both keys are the same key. The mirror room and the warm room share one wall, and the wall "
      "has a door painted to match the paper. His voice, from the corridor: so you found the honest "
      "one. Now decide what it was honest about.",
      { "Open the painted door", "Leave it shut", "" }, { 4, 5, 0 }, 2 },
    { "Behind it: the study, the desk, and a third key, worn smooth where a thumb has turned it a "
      "thousand times. He is not here. He is never here. But the game is still running, and now you "
      "know which doors answer.",
      { "", "", "" }, { 0, 0, 0 }, 0 },
    { "You leave it shut, and the corridor lights go out one by one behind you, politely, like a "
      "house saying goodnight. In the morning the key is back on the table. Some doors wait better "
      "than people do.",
      { "", "", "" }, { 0, 0, 0 }, 0 },
    // ---- 2. Four Minutes Late (nodes 6-11) ---------------------------------
    { "The hall clock is four minutes fast. Every clock in the house is four minutes fast except the "
      "one in the kitchen, which is right. He used to say a house should keep its own time. The note "
      "under the clock says: correct me.",
      { "Set the hall clock back", "Leave it, and watch", "" }, { 7, 8, 0 }, 2 },
    { "You set it back four minutes. The house answers: a bell rings in a room that has no bell, and "
      "the kitchen clock, the honest one, jumps forward four minutes to match the hall.",
      { "Set the kitchen clock back instead", "", "" }, { 9, 0, 0 }, 1 },
    { "You wait. At exactly four minutes past the hour the clock ticks twice in one second, as if "
      "catching up to something. Nothing else happens. Nothing else has to.",
      { "Follow the sound", "", "" }, { 9, 0, 0 }, 1 },
    { "The tick came from the panelling, not the clock. Inside: a folded page dated the day he died, "
      "one line in his hand. Time is the only thing in this house I never learned to give away.",
      { "Keep the page", "Put it back", "" }, { 10, 11, 0 }, 2 },
    { "You keep it. The clocks stay four minutes fast and you stop correcting them, because now you "
      "know they are not wrong. They are early for something, and they have been for years.",
      { "", "", "" }, { 0, 0, 0 }, 0 },
    { "You put it back and close the panelling. The clocks keep their four minutes. In a house like "
      "this, being early is the same as being warned.",
      { "", "", "" }, { 0, 0, 0 }, 0 },
    // ---- 3. The Fifth Door (nodes 12-17) -----------------------------------
    { "The floor plan on the landing shows four doors on the west wing. Standing in the west wing you "
      "count five. The fifth is narrow, waist-high, and has no handle, only a slot the size of a coin.",
      { "Try the brass key", "Look through the slot", "" }, { 13, 14, 0 }, 2 },
    { "The key goes in to the hilt and stops. Something inside turns it back out, politely, and the "
      "door does not move. It is not locked. It is occupied.",
      { "Knock", "", "" }, { 15, 0, 0 }, 1 },
    { "Through the slot: a corridor, longer than the wing allows, with a light at the end that is not "
      "on the plan either. Someone is standing in it, facing away, exactly your height.",
      { "Say his name", "", "" }, { 15, 0, 0 }, 1 },
    { "Whatever answers does not use words. Three taps, the pattern the brothers used on each other's "
      "doors as children. Then silence, and the sound of a chair being pulled out. Yours to decide "
      "whether that was an invitation.",
      { "Tap back", "Walk away", "" }, { 16, 17, 0 }, 2 },
    { "You tap three times. The door opens on a room that is your own room, furnished a year from "
      "now, and the chair is already warm. The fifth door was never a secret. It was a rehearsal.",
      { "", "", "" }, { 0, 0, 0 }, 0 },
    { "You walk away. In the morning the plan on the landing shows five doors, printed cleanly, as if "
      "it always had. Nobody will believe you, and the house is not asking you to be believed.",
      { "", "", "" }, { 0, 0, 0 }, 0 },
};
#define X_NNODE ((int)(sizeof(kStory) / sizeof(kStory[0])))
struct XStory { const char *title; uint8_t first; uint8_t pages; };
static const XStory kStories[] = {
    { "The Brass Key",     0,  6 },
    { "Four Minutes Late", 6,  6 },
    { "The Fifth Door",   12,  6 },
};
static_assert(sizeof(kStories) / sizeof(kStories[0]) == (size_t)X_NST,
              "kStories and the story state arrays must agree or a shelf row has no pages");
// Every jump target must land inside the table.  A typo in a `to` column is a
// dead button rather than a crash: the click is dropped and the page stays.
static inline bool x_st_target_ok(int n) { return n >= 0 && n < X_NNODE; }

static uint32_t x_st_pack_pos() {
    uint32_t packed = 0;
    for (int i = 0; i < X_NST; i++)
        packed |= ((uint32_t)(g_xst_pos[i] & 0xFF)) << (8 * i);
    return packed;
}
static void x_st_save() {
    const uint32_t packed = x_st_pack_pos();
    g_xprefs.begin("xander", false);
    g_xprefs.putULong("stpos",   packed);
    g_xprefs.putULong("stdone", (uint32_t)g_xst_done);
    g_xprefs.putULong("stend",  (uint32_t)g_xst_ends);
    g_xprefs.putInt("stcur", g_x_st_last);
    g_xprefs.end();
    x_st_save_trail();
}
// v2.16: the trail lived only in RAM, so a night resumed after a reboot had a
// page but no way back.  One tiny string per story: "12,7,3".
static void x_st_save_trail() {
    if (g_x_st_id < 0 || g_x_st_id >= X_NST) return;
    char key[8], buf[48];
    snprintf(key, sizeof(key), "tr%d", g_x_st_id);
    int n = 0;
    for (int k = 0; k < g_xst_hn[g_x_st_id] && n < (int)sizeof(buf) - 5; k++)
        n += snprintf(buf + n, sizeof(buf) - n, k ? ",%u" : "%u",
                      (unsigned)g_xst_hist[g_x_st_id][k]);
    buf[n] = 0;
    g_xprefs.begin("xander", false);
    g_xprefs.putString(key, buf);
    g_xprefs.end();
}
static void x_st_load_trail(int i) {
    if (i < 0 || i >= X_NST) return;
    char key[8];
    snprintf(key, sizeof(key), "tr%d", i);
    g_xprefs.begin("xander", false);
    const String v = g_xprefs.getString(key, "");
    g_xprefs.end();
    g_xst_hn[i] = 0;
    for (int k = 0; k < (int)v.length() && g_xst_hn[i] < 8; ) {
        int e = v.indexOf(',', k);
        if (e < 0) e = (int)v.length();
        const int n = v.substring(k, e).toInt();
        if (x_st_target_ok(n)) g_xst_hist[i][g_xst_hn[i]++] = (uint8_t)n;
        k = e + 1;
    }
}
// v2.16: nothing on the watch could erase progress -- the only way to start over
// was a serial command.  "Start over" clears the whole namespace and puts the
// settings (and the nickname) back, so only progress is gone.
static void x_stats_wipe() {
    const String keep_nick = g_x_nick;
    g_xs_solved = g_xs_correct = g_xs_hints = g_xs_streak = g_xs_best = 0;
    g_xs_hard = 0; g_xs_bits = 0; g_xs_shen = 0;
    g_xsolved_mask = 0; g_xread_mask = 0; g_x_kn_last = 0;
    g_xst_done = 0; g_xst_ends = 0; g_x_st_last = 0;
    for (int i = 0; i < X_NST; i++) {
        g_xst_pos[i] = (int)kStories[i].first;
        g_xst_hn[i]  = 0;
    }
    g_x_st_id = 0; g_x_st_node = 0; g_x_st_view = 0;
    g_xprefs.begin("xander", false);
    g_xprefs.clear();
    g_xprefs.end();
    g_x_nick = keep_nick;
    x_settings_save();
    g_xprefs.begin("xander", false);
    g_xprefs.putString("nick", g_x_nick);
    g_xprefs.end();
    x_st_save();                       // bookmarks back to the first page
    if (g_sd_ok && SD_MMC.exists(X_SAVE_PATH)) SD_MMC.remove(X_SAVE_PATH);
}
static int x_st_first(int i) { return (int)kStories[i].first; }
// v2.15: two endings per story, in table order.  These three walk the story's
// own range rather than a second table, so adding pages cannot desync them.
static int x_st_end_count(int i) {
    const int first = (int)kStories[i].first, last = first + (int)kStories[i].pages - 1;
    int n = 0;
    for (int k = first; k <= last && k < X_NNODE; k++) if (kStory[k].n == 0) n++;
    return n;
}
static int x_st_end_slot(int i, int node) {          // which ending, or -1
    const int first = (int)kStories[i].first, last = first + (int)kStories[i].pages - 1;
    int slot = 0;
    for (int k = first; k <= last && k < X_NNODE; k++) {
        if (kStory[k].n != 0) continue;
        if (k == node) return slot;
        slot++;
    }
    return -1;
}
// v2.16: the ledger knew which door you took and would not say it.
static int x_st_door_letter(int i) {
    const int total = x_st_end_count(i);
    if (total < 2 || x_st_ends_seen(i) != 1) return 0;
    for (int s = 0; s < total && s < 2; s++)
        if (g_xst_ends & (1u << (i * 2 + s))) return 'A' + s;
    return 0;
}
static int x_st_ends_seen(int i) {
    const int total = x_st_end_count(i);
    int n = 0;
    for (int s = 0; s < total && s < 2; s++)
        if (g_xst_ends & (1u << (i * 2 + s))) n++;
    return n;
}
// Reaching an ending is the only thing a story can "finish".  It rewinds the
// bookmark to page one so the shelf offers the story again, and keeps the
// finished mark so the shelf admits you have been.
static void x_st_choice_cb(lv_event_t *e) {
    if (millis() - lastSwipeMs < CLICK_AFTER_SWIPE_MS) return;
    const int which = (int)(intptr_t)lv_event_get_user_data(e);
    if (g_x_st_node < 0 || g_x_st_node >= X_NNODE) return;
    const XStoryNode &nd = kStory[g_x_st_node];
    if (which < 0 || which >= (int)nd.n) return;
    const int nx = (int)nd.to[which];
    if (!x_st_target_ok(nx)) return;
    // v2.15: choices are absolute jumps and nothing recorded the way in, so a
    // mis-tap committed you for the rest of the night.
    if (g_xst_hn[g_x_st_id] < 8)
        g_xst_hist[g_x_st_id][g_xst_hn[g_x_st_id]++] = (uint8_t)g_x_st_node;
    g_x_st_node = nx;
    if (kStory[nx].n == 0) {
        g_xst_done |= (uint8_t)(1u << g_x_st_id);
        const int slot = x_st_end_slot(g_x_st_id, nx);   // v2.15: which door this was
        if (slot >= 0 && slot < 2) g_xst_ends |= (uint8_t)(1u << (g_x_st_id * 2 + slot));
    }
    g_xst_pos[g_x_st_id] = g_x_st_node;
    x_st_save();
    // v2.16: the card copy used to be as old as your last exit, so a flat
    // battery could cost you the ending you had just reached.
    if (kStory[nx].n == 0) x_stats_export();
    x_render();
}
static void x_st_pick_cb(lv_event_t *e) {
    if (millis() - lastSwipeMs < CLICK_AFTER_SWIPE_MS) return;
    const int i = (int)(intptr_t)lv_event_get_user_data(e);
    if (i < 0 || i >= X_NST) return;
    g_x_st_id = i;
    g_x_st_last = i;
    x_st_load_trail(i);
    int p = g_xst_pos[i];
    const int first = x_st_first(i), last = first + (int)kStories[i].pages - 1;
    if (p < first || p > last) p = first;
    g_x_st_node = p;
    g_x_st_view = 1;
    g_x_console = false; g_x_top = 3;
    g_x_orb = 1; x_render();
}
// v2.15: one page back, from the trail the choices leave behind.
static void x_st_back_cb(lv_event_t *) {
    if (millis() - lastSwipeMs < CLICK_AFTER_SWIPE_MS) return;
    if (g_x_st_id < 0 || g_x_st_id >= X_NST || !g_xst_hn[g_x_st_id]) return;
    g_x_st_node = (int)g_xst_hist[g_x_st_id][--g_xst_hn[g_x_st_id]];
    g_xst_pos[g_x_st_id] = g_x_st_node;
    x_st_save();
    x_render();
}
// v2.16: every story has two doors and the trail's first page is the first
// choice, so a finished night can offer the way not taken.
static void x_st_other_cb(lv_event_t *) {
    if (millis() - lastSwipeMs < CLICK_AFTER_SWIPE_MS) return;
    if (g_x_st_id < 0 || g_x_st_id >= X_NST || !g_xst_hn[g_x_st_id]) return;
    g_x_st_node = (int)g_xst_hist[g_x_st_id][0];
    g_xst_pos[g_x_st_id] = g_x_st_node;
    x_st_save();
    g_x_orb = 1;
    x_render();
}
static void x_st_shelf_cb(lv_event_t *) {
    if (millis() - lastSwipeMs < CLICK_AFTER_SWIPE_MS) return;
    // Leaving an ending rewinds that story, so tapping it again starts the night
    // over instead of dropping you back on "The end".
    if (g_x_st_id >= 0 && g_x_st_id < X_NST && g_x_st_node >= 0 &&
        g_x_st_node < X_NNODE && kStory[g_x_st_node].n == 0) {
        g_xst_pos[g_x_st_id] = (int)kStories[g_x_st_id].first;
        g_xst_hn[g_x_st_id] = 0;                 // a rewound night forgets the path
        x_st_save();
    }
    g_x_st_view = 0;
    x_render();
}
static void x_render_stories() {
    if (g_x_st_view == 0) {
        int done = 0, doors = 0, allDoors = 0;
        for (int i = 0; i < X_NST; i++) {
            const int h = x_st_ends_seen(i), a = x_st_end_count(i);
            doors += h; allDoors += a;
            if (a > 0 && h >= a) done++;
        }
        char sub[64];
        snprintf(sub, sizeof(sub), "%d of %d nights - %d of %d doors", done, X_NST,
                 doors, allDoors);
        x_header("THE STORIES", sub);
        lv_obj_t *list = x_list(72, LCD_HEIGHT - 130);
        x_group_hdr(list, "PICK ONE");
        // v2.16: the shelf made you find your own place again every time.
        if (g_x_st_last >= 0 && g_x_st_last < X_NST) {
            char cl[72];
            snprintf(cl, sizeof(cl), "Resume: %s", kStories[g_x_st_last].title);
            x_row_btn(list, cl, X_PARCHMENT, x_st_pick_cb, (intptr_t)g_x_st_last);
        }
        for (int i = 0; i < X_NST; i++) {
            const int first = x_st_first(i);
            const int last  = first + (int)kStories[i].pages - 1;
            int pos = g_xst_pos[i];
            if (pos < first || pos > last) { pos = first; g_xst_pos[i] = pos; }
            const int page = pos - first + 1;
            const bool fin  = (g_xst_done & (1u << i)) != 0;
            char lbl[72];
            const int have = x_st_ends_seen(i), all = x_st_end_count(i);
            const int door = x_st_door_letter(i);        // v2.16: A, B, or 0
            const bool both = (all > 0 && have >= all);
            if (both)      snprintf(lbl, sizeof(lbl), "%s  -  finished, both doors",
                                    kStories[i].title);
            else if (have && door) snprintf(lbl, sizeof(lbl), "%s  -  door %c of %d",
                                    kStories[i].title, door, all);
            else if (have) snprintf(lbl, sizeof(lbl), "%s  -  %d of %d doors",
                                    kStories[i].title, have, all);
            else           snprintf(lbl, sizeof(lbl), "%s  -  page %d/%d",
                                    kStories[i].title, page, kStories[i].pages);
            x_row_btn(list, lbl, both ? X_SMOKE : X_ACCENT, x_st_pick_cb, (intptr_t)i);
            (void)fin;
        }
        x_pager();
        return;
    }
    const XStory &st = kStories[g_x_st_id];
    const int first = (int)st.first, last = first + (int)st.pages - 1;
    if (g_x_st_node < first || g_x_st_node > last) g_x_st_node = first;
    const XStoryNode &nd = kStory[g_x_st_node];
    const bool end = (nd.n == 0);
    char sub[48];
    if (end) snprintf(sub, sizeof(sub), "the end");
    else     snprintf(sub, sizeof(sub), "page %d of %d", g_x_st_node - first + 1, (int)st.pages);
    x_header(st.title, sub);
    // The page is a card, not a label on bare glass: the story reads as something
    // handed to you.  56 .. 286, then the choices start at 296.
    lv_obj_t *box = lv_obj_create(g_x_scr);
    lv_obj_remove_style_all(box);
    lv_obj_set_size(box, LCD_WIDTH - 24, 230);
    lv_obj_align(box, LV_ALIGN_TOP_MID, 0, 56);
    lv_obj_set_style_bg_color(box, lv_color_hex(theme_fg()), 0);
    lv_obj_set_style_bg_grad_color(box, lv_color_hex((theme_fg() & 0xFEFEFE) >> 1), 0);
    lv_obj_set_style_bg_grad_dir(box, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_bg_opa(box, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(box, 16, 0);
    lv_obj_set_style_border_color(box, lv_color_hex(X_ACCENT), 0);
    lv_obj_set_style_border_width(box, 1, 0);
    lv_obj_set_style_pad_all(box, 12, 0);
    // v2.15: 16 pt was the same size as a button caption.  The longest page is
    // 225 characters -- seven lines at 18 pt, about 168 of the card's 206 px.
    x_wraplbl(box, nd.text, LCD_WIDTH - 52, X_PARCHMENT, &lv_font_montserrat_18);
    if (end) {
        lv_obj_t *b = x_cbtn(g_x_scr, "Back to the shelf", LCD_WIDTH - 32, X_ACCENT,
                             x_st_shelf_cb, 0);
        lv_obj_set_height(b, 46);
        lv_obj_align(b, LV_ALIGN_TOP_MID, 0, 296);
        if (g_xst_hn[g_x_st_id] > 0) {
            lv_obj_t *o = x_cbtn(g_x_scr, "the other way", LCD_WIDTH - 32, X_SMOKE,
                                 x_st_other_cb, 0);
            lv_obj_set_height(o, 44);
            lv_obj_align(o, LV_ALIGN_TOP_MID, 0, 352);
        }
    } else {
        for (int i = 0; i < (int)nd.n && i < 3; i++) {
            lv_obj_t *b = x_cbtn(g_x_scr, nd.a[i], LCD_WIDTH - 32, X_ACCENT,
                                 x_st_choice_cb, (intptr_t)i);
            lv_obj_set_height(b, 46);
            lv_obj_align(b, LV_ALIGN_TOP_MID, 0, 296 + i * 50);
        }
        // The choices end at 442; the strip below them is free on this page.
        if (g_xst_hn[g_x_st_id] > 0) {
            lv_obj_t *bb = x_cbtn(g_x_scr, LV_SYMBOL_LEFT "  back a page",
                                  LCD_WIDTH - 32, X_SMOKE, x_st_back_cb, 0);
            lv_obj_set_height(bb, 40);
            lv_obj_align(bb, LV_ALIGN_BOTTOM_MID, 0, -8);
        }
    }
}

// ---- view: HOME ------------------------------------------------------------
// v2.5: home shortcuts.  These call the same intents the console does, so the
// chips and the spoken commands can never drift apart.
static void x_qa_cb(lv_event_t *e) {
    if (millis() - lastSwipeMs < CLICK_AFTER_SWIPE_MS) return;
    int which = (int)(intptr_t)lv_event_get_user_data(e);
    if (which == 0) { x_do(XI_TEACH); return; }
    if (which == 1) { x_do(XI_GIVE_PUZZLE); return; }
    if (which == 3) {
        g_x_console = false; g_x_top = 3; g_x_st_view = 0; g_x_orb = 1; x_render();
        return;
    }
    g_x_console = false; g_x_top = 2; g_x_kn_view = 0; g_x_kn_cat = ""; g_x_orb = 1; x_render();
}
// The orb picks for you: an unread article while any are left, then an unsolved
// trial, and only when both are finished does it admit there is nothing left.
static void x_orb_tap_cb(lv_event_t *) {
    if (millis() - lastSwipeMs < CLICK_AFTER_SWIPE_MS) return;
    for (int i = 0; i < x_nart(); i++) if (!x_read_bit(i)) { x_do(XI_TEACH); return; }
    for (int i = 0; i < X_NPUZ; i++) if (!x_solved_bit(i)) { x_do(XI_GIVE_PUZZLE); return; }
    g_x_last = "Every door is open. You did that.";
    g_x_orb  = 4;
    x_finish_info();
}

static void x_render_home() {
    x_header("XANDER", NULL);
    // orb
    lv_obj_t *orb = lv_obj_create(g_x_scr);
    lv_obj_remove_style_all(orb);
    // v2.13i: 52 px, top-left, beside the bubble.  At 96 px centred it was a
    // logo floating above a stack of unrelated bands; attached to the speech it
    // becomes the speaker, and the page gets a single focal point.
    lv_obj_set_size(orb, 52, 52);
    lv_obj_align(orb, LV_ALIGN_TOP_LEFT, 18, 58);
    lv_obj_set_style_radius(orb, LV_RADIUS_CIRCLE, 0);
    // v1.9: idle and amused share his accent, so the orb stays recognisably his
    // in every palette; busy keeps the theme colour so a long NTP still reads
    // as "working" rather than "pleased".
    uint32_t oc = (g_x_orb == 1) ? X_MOSS : (g_x_orb == 2) ? theme_danger()
                : (g_x_orb == 3) ? theme_primary() : X_ACCENT;
    g_x_orb_obj = orb;
    lv_obj_set_style_bg_color(orb, lv_color_hex(theme_fg()), 0);
    lv_obj_set_style_bg_opa(orb, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(orb, lv_color_hex(oc), 0);
    lv_obj_set_style_border_width(orb, 3, 0);
    lv_obj_set_style_shadow_color(orb, lv_color_hex(oc), 0);
    lv_obj_set_style_shadow_width(orb, 18, 0);
    lv_obj_set_style_shadow_opa(orb, 110, 0);
    lv_obj_clear_flag(orb, LV_OBJ_FLAG_SCROLLABLE);
    // An inner ring reads as a lens and gives the orb depth without a second
    // bitmap; it also widens the coloured area when the state colour changes.
    lv_obj_t *ring = lv_obj_create(orb);
    lv_obj_remove_style_all(ring);
    lv_obj_clear_flag(ring, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(ring, 38, 38);
    lv_obj_center(ring);
    lv_obj_set_style_radius(ring, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_color(ring, lv_color_hex(oc), 0);
    lv_obj_set_style_border_opa(ring, 70, 0);
    lv_obj_set_style_border_width(ring, 1, 0);
    lv_obj_set_style_bg_opa(ring, LV_OPA_TRANSP, 0);
    lv_obj_t *g = lv_label_create(orb);
    // state 4 is the wink: a closed eye while the line that earned it is still
    // on screen.  One glyph, no timer.
    const char *glyph = (g_x_orb == 1) ? LV_SYMBOL_OK
                      : (g_x_orb == 2) ? LV_SYMBOL_WARNING
                      : (g_x_orb == 3) ? LV_SYMBOL_REFRESH
                      : (g_x_orb == 4) ? LV_SYMBOL_EYE_CLOSE : LV_SYMBOL_EYE_OPEN;
    lv_label_set_text(g, glyph);
    lv_obj_set_style_text_color(g, lv_color_hex(oc), 0);
    lv_obj_set_style_text_font(g, &lv_font_montserrat_18, 0);
    lv_obj_center(g);
    // v2.5: 96 px of screen that did nothing.  No idea what you want? He picks.
    lv_obj_add_flag(orb, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(orb, x_orb_tap_cb, LV_EVENT_CLICKED, NULL);
    // response caption -- a fixed 112 px window, ellipsised, so a long answer
    // (system status is up to 168 chars) can never grow into the COMMAND button.
    lv_obj_t *box = lv_obj_create(g_x_scr);
    lv_obj_remove_style_all(box);
    lv_obj_clear_flag(box, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(box, 312, 154);
    lv_obj_align(box, LV_ALIGN_TOP_LEFT, 80, 58);
    // v2.13h: this caption used to be white text floating in an invisible
    // 112 px rectangle -- measured against the rest of the page it was the
    // emptiest surface on the screen, and it read as a status line on bare
    // glass.  It is now the same bubble the console draws (gradient, accent
    // hairline, 16 radius), with a microcaps state label, so the answer reads
    // as something he said.  Same box, same coordinates: nothing else moved.
    lv_obj_set_style_bg_color(box, lv_color_hex(theme_fg()), 0);
    lv_obj_set_style_bg_grad_color(box, lv_color_hex((theme_fg() & 0xFEFEFE) >> 1), 0);
    lv_obj_set_style_bg_grad_dir(box, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_bg_opa(box, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(box, 16, 0);
    lv_obj_set_style_border_color(box, lv_color_hex(X_ACCENT), 0);
    lv_obj_set_style_border_width(box, 1, 0);
    lv_obj_set_style_pad_all(box, 9, 0);
    const char *state = (g_x_orb == 1) ? "SAID"
                      : (g_x_orb == 2) ? "PROBLEM"
                      : (g_x_orb == 3) ? "WORKING"
                      : (g_x_orb == 4) ? "MM-HM" : "XANDER";
    lv_obj_t *who = lv_label_create(box);
    lv_label_set_text(who, state);
    lv_obj_set_style_text_color(who, lv_color_hex(X_ACCENT), 0);
    lv_obj_set_style_text_font(who, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_letter_space(who, 2, 0);
    lv_obj_align(who, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_t *r = x_wraplbl(box, g_x_last.length() ? g_x_last.c_str()
                                                   : "Your move. Ask me anything.",
                            292, X_PARCHMENT, &lv_font_montserrat_16);
    lv_label_set_long_mode(r, LV_LABEL_LONG_DOT);
    lv_obj_align(r, LV_ALIGN_TOP_LEFT, 0, 20);
    lv_obj_add_flag(r, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(r, x_caption_cb, LV_EVENT_CLICKED, NULL);
    g_x_resp_lbl = r;
    // v1.9: the door, not the furniture.
    // v2.13i: the only FILLED element on the page.  It used to be an outline
    // button of exactly the same weight as the three doors above it, which is
    // why nothing here read as "start here".  Dark label because white on
    // 0xFFC24B is barely legible.
    lv_obj_t *cmd = lv_btn_create(g_x_scr);
    lv_obj_remove_style_all(cmd);
    lv_obj_clear_flag(cmd, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(cmd, LCD_WIDTH - 32, 56);
    lv_obj_align(cmd, LV_ALIGN_TOP_MID, 0, 352);
    lv_obj_set_style_bg_color(cmd, lv_color_hex(X_ACCENT), 0);
    lv_obj_set_style_bg_opa(cmd, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(cmd, 18, 0);
    lv_obj_set_style_shadow_color(cmd, lv_color_hex(X_ACCENT), 0);
    lv_obj_set_style_shadow_width(cmd, 22, 0);
    lv_obj_set_style_shadow_opa(cmd, 70, 0);
    lv_obj_t *cl = lv_label_create(cmd);
    lv_label_set_text(cl, LV_SYMBOL_KEYBOARD "  ASK ME SOMETHING");
    lv_obj_set_style_text_color(cl, lv_color_hex(0x141414), 0);
    lv_obj_set_style_text_font(cl, &lv_font_montserrat_16, 0);
    lv_obj_center(cl);
    lv_obj_add_event_cb(cmd, x_cmd_open_cb, LV_EVENT_CLICKED, NULL);
    // v2.5: progress, not a grey footnote.  Two hairlines say more than three
    // numbers did, and they are honest now that the read mask is real.
    const int rd = x_read_count(), sv = x_vault_count();
    // v2.15: the fourth door had no presence on the page you land on, and the
    // purse was invisible except on the card that spends it.
    int nights = 0;
    for (int i = 0; i < X_NST; i++)
        if (x_st_ends_seen(i) >= x_st_end_count(i) && x_st_end_count(i) > 0) nights++;
    char l1[48], l2[48];
    snprintf(l1, sizeof(l1), "READ %d/%d   NIGHTS %d/%d", rd, x_nart(), nights, X_NST);
    snprintf(l2, sizeof(l2), "TRIALS %d/%d   BITS %ld", sv, X_NPUZ, g_xs_bits);
    lv_obj_t *p1 = lv_label_create(g_x_scr);
    lv_label_set_text(p1, l1);
    lv_obj_set_style_text_color(p1, lv_color_hex(X_SMOKE), 0);
    lv_obj_set_style_text_font(p1, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_letter_space(p1, 1, 0);
    lv_obj_align(p1, LV_ALIGN_TOP_LEFT, 18, 222);
    lv_obj_t *b1 = lv_bar_create(g_x_scr);
    lv_obj_set_size(b1, 150, 4);
    lv_obj_align(b1, LV_ALIGN_TOP_LEFT, 18, 242);
    lv_bar_set_range(b1, 0, x_nart());
    lv_bar_set_value(b1, rd, LV_ANIM_OFF);
    lv_obj_set_style_radius(b1, 2, LV_PART_MAIN);
    lv_obj_set_style_bg_color(b1, lv_color_hex(0x242424), LV_PART_MAIN);
    lv_obj_set_style_radius(b1, 2, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(b1, lv_color_hex(X_ACCENT), LV_PART_INDICATOR);
    lv_obj_t *p2 = lv_label_create(g_x_scr);
    lv_label_set_text(p2, l2);
    lv_obj_set_style_text_color(p2, lv_color_hex(X_SMOKE), 0);
    lv_obj_set_style_text_font(p2, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_letter_space(p2, 1, 0);
    lv_obj_align(p2, LV_ALIGN_TOP_RIGHT, -18, 222);
    lv_obj_t *b2 = lv_bar_create(g_x_scr);
    lv_obj_set_size(b2, 150, 4);
    lv_obj_align(b2, LV_ALIGN_TOP_RIGHT, -18, 242);
    lv_bar_set_range(b2, 0, X_NPUZ);
    lv_bar_set_value(b2, sv, LV_ANIM_OFF);
    lv_obj_set_style_radius(b2, 2, LV_PART_MAIN);
    lv_obj_set_style_bg_color(b2, lv_color_hex(0x242424), LV_PART_MAIN);
    lv_obj_set_style_radius(b2, 2, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(b2, lv_color_hex(X_ACCENT), LV_PART_INDICATOR);
    // three doors in the band that used to be empty: this page had one way in
    // for a layer with three destinations.
    lv_obj_t *qa = lv_obj_create(g_x_scr);
    lv_obj_remove_style_all(qa);
    lv_obj_clear_flag(qa, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(qa, LCD_WIDTH - 16, 52);
    lv_obj_align(qa, LV_ALIGN_TOP_MID, 0, 262);
    lv_obj_set_style_pad_all(qa, 0, 0);
    lv_obj_set_flex_flow(qa, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(qa, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    // v2.14: four doors, and a fourth way in -- the stories.  94 px each leaves
    // 6 px of flex gap inside 394, so "Library" and "Stories" both still fit at
    // 16 pt instead of ellipsising into each other.
    const int qw = (LCD_WIDTH - 16 - 18) / 4;
    x_cbtn(qa, "Teach", qw, X_ACCENT, x_qa_cb, 0);
    x_cbtn(qa, "Trial", qw, X_ACCENT, x_qa_cb, 1);
    x_cbtn(qa, "Library", qw, X_ACCENT, x_qa_cb, 2);
    x_cbtn(qa, "Stories", qw, X_ACCENT, x_qa_cb, 3);
    if (g_x_orb == 3) x_orb_breathe();
    // navigation: pips + an exit target (v1.7)
    x_pager();
}

// ---- view: CONSOLE (keyboard + tappable command chips) ---------------------
// v1.7: the textarea's pointer is kept so the RUN button can submit it.  The
// keyboard's enter key used to be the only way to run what you typed, which is
// the least discoverable control on a watch keyboard.
// ---- v2.6: he judges the answer --------------------------------------------
//  Trials used to be self-graded: reveal the answer, then tap "I got it".  Now
//  Type opens the console, your line is compared with the stored answer, and he
//  says whether it lands.  Tolerant on purpose: case, punctuation and phrasing
//  should not lose a right answer.
static long x_first_num(const String &t, bool *ok) {
    long v = 0; bool got = false;
    for (int i = 0; i < (int)t.length(); i++) {
        char c = t[i];
        if (c >= '0' && c <= '9') { got = true; v = v * 10 + (c - '0'); }
        else if (got) break;
    }
    if (ok) *ok = got;
    return v;
}
static bool x_ans_match(const String &input, const String &answer) {
    String a = input, b = answer;
    a.toLowerCase(); b.toLowerCase();
    const char *junk[] = { "?", ".", ",", "!", "\"", "'", "-", "(", ")", ":", ";" };
    for (unsigned i = 0; i < sizeof(junk) / sizeof(junk[0]); i++) {
        a.replace(junk[i], " ");
        b.replace(junk[i], " ");
    }
    a.trim(); b.trim();
    if (!a.length()) return false;
    if (a == b) return true;
    if (a.length() >= 3 && b.indexOf(a) >= 0) return true;
    bool na = false, nb = false;
    long va = x_first_num(a, &na), vb = x_first_num(b, &nb);
    if (na && nb && va == vb) return true;
    if (nb) return false;                 // the answer is a number: nothing else counts
    int start = 0;
    while (start < (int)b.length()) {
        int sp = b.indexOf(' ', start);
        if (sp < 0) sp = b.length();
        String w = b.substring(start, sp);
        if (w.length() >= 4 && a.indexOf(w) >= 0) return true;
        start = sp + 1;
    }
    return false;
}
static void x_check_answer(const String &txt) {
    String ans = String(kPuzzles[g_x_puz_id].answer);
    bool hit = x_ans_match(txt, ans);
    g_x_echo     = txt;
    g_x_ans_mode = false;
    g_x_console  = false;
    g_x_puz_view = 1;
    x_puz_grade(hit ? 1 : 0);
    if (!hit) g_x_last = String(x_say(X_R_SASS, X_NBANK(X_R_SASS))) + " It was: " + ans;
    x_render();
}
static lv_obj_t *g_x_ta = nullptr;
static void x_run_cb(lv_event_t *) {
    if (millis() - lastSwipeMs < CLICK_AFTER_SWIPE_MS) return;
    if (!g_x_ta) return;
    String txt = lv_textarea_get_text(g_x_ta);
    lv_textarea_set_text(g_x_ta, "");
    g_x_echo = txt;                    // v1.9: the bubble names the exchange
    if (!txt.length()) return;
    if (g_x_ans_mode) { x_check_answer(txt); return; }   // v2.6: a trial answer
    x_process(txt);
}
static void x_kb_toggle_cb(lv_event_t *)    {
    if (millis() - lastSwipeMs < CLICK_AFTER_SWIPE_MS) return;
 g_x_kb = !g_x_kb; x_render(); }
static void x_voice_toggle_cb(lv_event_t *) { g_x_voice_show = !g_x_voice_show; x_render(); }
static void x_voice_pick_cb(lv_event_t *e) {
    const int i = (int)(intptr_t)lv_event_get_user_data(e);
    if (x_voice_run(i)) return;                       // an app row reboots
    g_x_last = "That one is voice only."; g_x_orb = 2; x_finish_info();
}
static void x_group_hdr(lv_obj_t *list, const char *txt) {
    lv_obj_t *l = lv_label_create(list);
    lv_label_set_text(l, txt);
    lv_obj_set_style_text_color(l, lv_color_hex(X_SMOKE), 0);
    lv_obj_set_style_text_font(l, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_letter_space(l, 2, 0);
    lv_obj_set_style_pad_top(l, 8, 0);
}
static void x_render_console() {
    x_header("XANDER", g_x_voice_show ? "what the mic accepts" : "type, tap, or say");
    // v1.9: a bubble, not a system notice -- same gradient / 14-radius language
    // as x_row_btn with the accent hairline, so the eye knows this is him
    // talking.  "YOU -> XANDER" appears whenever a typed line was echoed.
    lv_obj_t *box = lv_obj_create(g_x_scr);
    lv_obj_remove_style_all(box);
    lv_obj_clear_flag(box, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(box, LCD_WIDTH - 30, 84);
    lv_obj_align(box, LV_ALIGN_TOP_MID, 0, 52);
    lv_obj_set_style_bg_color(box, lv_color_hex(theme_fg()), 0);
    lv_obj_set_style_bg_grad_color(box, lv_color_hex((theme_fg() & 0xFEFEFE) >> 1), 0);
    lv_obj_set_style_bg_grad_dir(box, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_bg_opa(box, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(box, 14, 0);
    lv_obj_set_style_border_color(box, lv_color_hex(X_ACCENT), 0);
    lv_obj_set_style_border_width(box, 1, 0);
    lv_obj_set_style_pad_all(box, 10, 0);
    lv_obj_t *who = lv_label_create(box);
    lv_label_set_text(who, g_x_echo.length() ? "YOU  ->  XANDER" : "XANDER");
    lv_obj_set_style_text_color(who, lv_color_hex(X_ACCENT), 0);
    lv_obj_set_style_text_font(who, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_letter_space(who, 2, 0);
    lv_obj_align(who, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_t *r = x_wraplbl(box, g_x_last.length() ? g_x_last.c_str() : "Ask me something.",
                            LCD_WIDTH - 56, X_PARCHMENT, &lv_font_montserrat_14);
    // v2.15: the bubble is 84 px and children are clipped to their parent, so a
    // long answer ("system status") was cut mid-sentence with nothing to say so.
    // Three lines, a dotted tail, and a tap for the whole thing -- the same
    // treatment the home caption has had since v2.13g.
    lv_label_set_long_mode(r, LV_LABEL_LONG_DOT);
    lv_obj_add_flag(r, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(r, x_caption_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_align(r, LV_ALIGN_TOP_LEFT, 0, 20);
    g_x_resp_lbl = r;

    if (g_x_voice_show) {
        // The phrase list owns the screen: a keyboard under it would cover half
        // of what you are reading, and a spoken phrase cannot be typed anyway.
        g_x_kb = false;
    } else {
        lv_obj_t *ta = lv_textarea_create(g_x_scr);
        lv_textarea_set_one_line(ta, true);
        lv_textarea_set_placeholder_text(ta, "e.g. give me a puzzle");
        lv_obj_set_width(ta, LCD_WIDTH - 176);
        lv_obj_set_style_text_font(ta, &lv_font_montserrat_16, 0);
        lv_obj_align(ta, LV_ALIGN_TOP_LEFT, 20, 82);
        g_x_ta = ta;
        // v1.8: the keyboard is a toggle, not furniture.  It used to eat 196 of
        // 502 px all the time, leaving ~3 commands visible on a screen where
        // typing is the rare path -- voice and the chips are the normal ones.
        lv_obj_t *kbt = x_cbtn(g_x_scr, LV_SYMBOL_KEYBOARD, 52, theme_accent(), x_kb_toggle_cb, 0);
        lv_obj_set_style_bg_opa(kbt, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(kbt, 0, 0);
        lv_obj_set_style_text_color(lv_obj_get_child(kbt, 0),
                                    lv_color_hex(g_x_kb ? theme_primary() : X_SMOKE), 0);
        lv_obj_align(kbt, LV_ALIGN_TOP_RIGHT, -92, 80);
        // Filled, not outlined: RUN is the primary action on this screen and
        // should not look like the outlined COMMAND button on the home page.
        lv_obj_t *run = x_cbtn(g_x_scr, LV_SYMBOL_PLAY, 64, theme_primary(), x_run_cb, 0);
        lv_obj_set_style_bg_color(run, lv_color_hex(theme_primary()), 0);
        lv_obj_set_style_border_width(run, 0, 0);
        lv_obj_set_style_text_color(lv_obj_get_child(run, 0), lv_color_hex(0x000000), 0);
        lv_obj_align(run, LV_ALIGN_TOP_RIGHT, -20, 80);
    }

    // v1.9: the spoken list is the coolest thing in the layer and it used to be
    // the last row of a thirty-row scroll.  It gets the band under the bubble.
    if (!g_x_kb) {
        char sl[64];
        snprintf(sl, sizeof(sl), LV_SYMBOL_AUDIO "  SAY IT - %d PHRASES", x_voice_count());
        lv_obj_t *sb = x_cbtn(g_x_scr, sl, LCD_WIDTH - 40, X_ACCENT, x_voice_toggle_cb, 0);
        lv_obj_align(sb, LV_ALIGN_TOP_MID, 0, 144);
    }
    // With the keyboard up the list keeps its old window; without it the list
    // owns everything down to the pager.
    lv_obj_t *list = x_list(g_x_kb ? 138 : 204, g_x_kb ? 160 : (LCD_HEIGHT - 260));

    // A miss is no longer a dead end: the two closest commands sit at the top.
    if (g_x_sugg_n) {
        x_group_hdr(list, "DID YOU MEAN");
        for (int i = 0; i < g_x_sugg_n; i++)
            x_row_btn(list, g_x_sugg[i].label.c_str(), theme_accent(), x_sugg_cb, (intptr_t)i);
    }

    if (g_x_voice_show) {
        x_group_hdr(list, "SAY ONE OF THESE");
        for (int i = 0; i < x_voice_count(); i++) {
            if (!x_voice_tappable(i)) continue;       // wake gate + slot swap stay voice-only
            x_row_btn(list, x_cap(x_voice_label(i)).c_str(), theme_primary(),
                      x_voice_pick_cb, (intptr_t)i);
        }
    } else {
        x_group_hdr(list, "SHORTCUTS");
        for (int i = 0; i < X_NTMPL; i++)
            x_row_btn(list, kTmpl[i].label, theme_accent(), x_chip_cb, (intptr_t)i);
        x_group_hdr(list, "COMMANDS");
        for (int i = 0; i < X_NCHIP; i++)
            x_row_btn(list, kChips[i].label, theme_primary(), x_chip_cb, (intptr_t)(2000 + i));
        if (g_app_count) x_group_hdr(list, "APPS");
        int nApp = g_app_count < 12 ? g_app_count : 12;
        for (int j = 0; j < nApp; j++) {
            String lbl = String(LV_SYMBOL_PLAY "  Launch ") + g_apps[j].name;
            x_row_btn(list, lbl.c_str(), theme_primary(), x_chip_cb, (intptr_t)(1000 + j));
        }
    }

    if (g_x_kb) {
        lv_obj_t *kb = lv_keyboard_create(g_x_scr);
        lv_obj_set_size(kb, LCD_WIDTH, 196);
        lv_obj_align(kb, LV_ALIGN_BOTTOM_MID, 0, 0);
        lv_keyboard_set_textarea(kb, g_x_ta);
        lv_obj_add_event_cb(kb, x_kb_cb, LV_EVENT_ALL, (void *)g_x_ta);
    } else {
        // v1.8: the console was the one surface with no visible way out -- it
        // drew no pips and no exit target.  They come with the list now.
        x_pager();
    }
}

// ---- view: PUZZLES ---------------------------------------------------------
static void x_render_puzzles() {
    if (g_x_puz_view == 0) {
        x_header("THE GAMES", "16 trials - none repeated");
        lv_obj_t *list = x_list(72, LCD_HEIGHT - 130);
        x_group_hdr(list, "PICK A DOOR");
        for (int i = 0; i < X_NPCAT; i++)
            x_row_btn(list, x_cap(X_PCATS[i]).c_str(), theme_accent(), x_puz_cat_cb, (intptr_t)i);
        x_pager();
        return;
    }
    const XPuzzle &p = kPuzzles[g_x_puz_id];
    char sub[64]; snprintf(sub, sizeof(sub), "trial %02d of %d  -  %s",
                           p.id, X_NPUZ, x_cap(p.category).c_str());
    x_header("THE GAMES", sub);
    lv_obj_t *list = x_list(70, LCD_HEIGHT - 200);
    // v1.9: difficulty as five pips instead of a sentence -- the same pip
    // language as the pager, so the layer has one visual grammar, not two.
    lv_obj_t *drow = lv_obj_create(list);
    lv_obj_remove_style_all(drow);
    lv_obj_clear_flag(drow, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_width(drow, LV_PCT(100));
    lv_obj_set_height(drow, 16);
    lv_obj_set_flex_flow(drow, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(drow, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_column(drow, 6, 0);
    for (int k = 0; k < 5; k++) {
        lv_obj_t *d = lv_obj_create(drow);
        lv_obj_remove_style_all(d);
        lv_obj_clear_flag(d, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_size(d, 10, 10);
        lv_obj_set_style_radius(d, 5, 0);
        lv_obj_set_style_bg_color(d, lv_color_hex(k < p.difficulty ? X_ACCENT : 0x2E2E2E), 0);
        lv_obj_set_style_bg_opa(d, LV_OPA_COVER, 0);
    }
    if (p.difficulty >= 6) {
        lv_obj_t *hd = lv_label_create(drow);
        lv_label_set_text(hd, "HAWTHORNE");
        lv_obj_set_style_text_color(hd, lv_color_hex(X_ACCENT), 0);
        lv_obj_set_style_text_font(hd, &lv_font_montserrat_12, 0);
        lv_obj_set_style_text_letter_space(hd, 2, 0);
    }
    x_wraplbl(list, p.question, LCD_WIDTH - 54, X_PARCHMENT, &lv_font_montserrat_18);
    // v2.12: A / B / C.  Tapping one grades the trial exactly like a typed
    // answer does (same bits, same streak rule, same "it was:" line).
    for (int c = 0; c < 3; c++) {
        char tag[2] = { (char)('A' + c), 0 };
        // Answer paints the true door green: the right option is written under
        // the question (and again in the "Answer:" line below the doors).
        const bool isright = (c == (int)kPuzOpts[g_x_puz_id].right);
        const uint32_t bord = (g_x_puz_reveal && isright) ? X_MOSS : theme_accent();
        x_opt_btn(list, tag, kPuzOpts[g_x_puz_id].o[c], bord,
                  x_puz_pick_cb, (intptr_t)(g_x_puz_id * 4 + c));
    }
    if (g_x_puz_hint) {
        lv_obj_t *h = x_wraplbl(list, (String("Hint: ") + x_hint_text(p)).c_str(),
                                LCD_WIDTH - 54, theme_accent(), &lv_font_montserrat_16);
        (void)h;
    }
    if (g_x_puz_reveal) {
        x_wraplbl(list, (String("Answer: ") + p.answer).c_str(),
                  LCD_WIDTH - 54, X_MOSS, &lv_font_montserrat_16);
        lv_obj_t *row = lv_obj_create(list);
        lv_obj_remove_style_all(row);
        lv_obj_set_width(row, LV_PCT(100));
        lv_obj_set_height(row, 54);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_style_pad_column(row, 8, 0);
        x_cbtn(row, "I got it", (LCD_WIDTH - 70) / 2, X_MOSS, x_puz_got_cb, 1);
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
    int bw = (LCD_WIDTH - 20 - 24) / 4;
    x_cbtn(bar, g_x_puz_reveal ? "Hide" : "Answer", bw, theme_primary(), x_puz_think_cb, 0);
    x_cbtn(bar, "Hint 5", bw, theme_accent(), x_puz_hint_cb, 0);
    x_cbtn(bar, "Type", bw, X_ACCENT, x_puz_type_cb, 0);
    x_cbtn(bar, "Next", bw, theme_primary(), x_puz_next_cb, 0);
    char sf[80];
    snprintf(sf, sizeof(sf), "Solved %ld  Streak %ld  Best %ld  Bits %ld", g_xs_solved, g_xs_streak, g_xs_best, g_xs_bits);
    lv_obj_t *st = lv_label_create(g_x_scr);
    lv_label_set_text(st, sf);
    lv_obj_set_style_text_color(st, lv_color_hex(X_SMOKE), 0);
    lv_obj_set_style_text_font(st, &lv_font_montserrat_12, 0);
    lv_obj_align(st, LV_ALIGN_BOTTOM_MID, 0, -8);
    // v1.8: the back target is the header chevron (x_header draws it whenever
    // there is a sub-view behind), so the card no longer duplicates it here.
}

// ---- view: KNOWLEDGE -------------------------------------------------------
static int x_cat_count(const char *key) {
    int c = 0; for (int i = 0; i < x_nart(); i++) if (String(x_art(i).category) == key) c++;
    return c;
}
static void x_render_knowledge() {
    if (g_x_kn_view == 0) {
        char sub[48];
        snprintf(sub, sizeof(sub), "%d volumes - %d read", x_nart(), x_read_count());
        x_header("LIBRARY", sub);
        lv_obj_t *list = x_list(72, LCD_HEIGHT - 130);
        if (g_x_kn_last >= 0 && g_x_kn_last < x_nart()) {
            char cl[80];
            snprintf(cl, sizeof(cl), "Continue: %s", x_art(g_x_kn_last).title);
            x_group_hdr(list, "BOOKMARK");
            x_row_btn(list, cl, X_MOSS, x_kn_resume_cb, (intptr_t)g_x_kn_last);
        }
        x_group_hdr(list, "SHELVES");
        for (int i = 0; i < X_NKCAT; i++) {
            const int n = x_cat_count(X_KCATS[i]);
            if (!n) continue;
            char lbl[64];
            const int r = x_cat_read(X_KCATS[i]);
            if (r) snprintf(lbl, sizeof(lbl), "%s  (%d)   %d read",
                            x_cap(X_KCATS[i]).c_str(), n, r);
            else   snprintf(lbl, sizeof(lbl), "%s  (%d)",
                            x_cap(X_KCATS[i]).c_str(), n);
            x_row_btn(list, lbl, theme_accent(), x_kn_cat_cb, (intptr_t)i);
        }
        x_row_btn(list, "Search the stacks", X_ACCENT, x_kn_search_cb, 0);
        x_pager();
        return;
    }
    if (g_x_kn_view == 1) {
        // A shelf reads A-Z, not in the order the table happens to be typed.
        static int idx[64];                 // x_nart() <= 64 (x_read_mask is 64 bits)
        int n = 0;
        for (int i = 0; i < x_nart() && n < 64; i++)
            if (g_x_kn_cat == x_art(i).category) idx[n++] = i;
        for (int a = 1; a < n; a++) {        // insertion sort: a shelf is small
            const int key = idx[a];
            int b = a - 1;
            while (b >= 0 && x_title_cmp(x_art(idx[b]).title, x_art(key).title) > 0) {
                idx[b + 1] = idx[b];
                b--;
            }
            idx[b + 1] = key;
        }
        char sub[56];
        snprintf(sub, sizeof(sub), "%d on this shelf - %d read",
                 n, x_cat_read(g_x_kn_cat.c_str()));
        x_header(x_cap(g_x_kn_cat.c_str()).c_str(), sub);
        lv_obj_t *list = x_list(72, LCD_HEIGHT - 130);
        for (int k = 0; k < n; k++) {
            const int i = idx[k];
            // An unread spine keeps the accent hairline; a read one goes quiet.
            x_row_btn(list, x_art(i).title, x_read_bit(i) ? 0x3A3A3A : theme_accent(),
                      x_kn_art_cb, (intptr_t)i);
        }
        lv_obj_t *kback = x_cbtn(g_x_scr, LV_SYMBOL_LEFT "  shelves", 150, theme_primary(), x_kn_back_cb, 0);
        lv_obj_align(kback, LV_ALIGN_BOTTOM_MID, 0, -10);
        return;
    }
    const XArticle &a = x_art(g_x_kn_art);
    x_header(a.title, x_cap(a.category).c_str());
    lv_obj_t *list = x_list(72, LCD_HEIGHT - 200);
    x_wraplbl(list, a.summary, LCD_WIDTH - 54, X_PARCHMENT, &lv_font_montserrat_18);
    if (g_x_kn_more)
        x_wraplbl(list, a.details, LCD_WIDTH - 54, X_PARCHMENT, &lv_font_montserrat_16);
    lv_obj_t *bar = lv_obj_create(g_x_scr);
    lv_obj_remove_style_all(bar);
    lv_obj_set_size(bar, LCD_WIDTH - 20, 50);
    lv_obj_align(bar, LV_ALIGN_BOTTOM_MID, 0, -12);
    lv_obj_set_flex_flow(bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(bar, 8, 0);
    const int bw3 = (LCD_WIDTH - 20 - 16) / 3;
    x_cbtn(bar, LV_SYMBOL_LEFT "  Back", bw3, theme_primary(), x_kn_back_cb, 0);
    x_cbtn(bar, x_read_bit(g_x_kn_art) ? "Unread" : "Read it", bw3,
           theme_accent(), x_kn_toggle_cb, 0);
    x_cbtn(bar, g_x_kn_more ? "Less" : "More", bw3, theme_accent(), x_kn_more_cb, 0);
}

// ---- render dispatch -------------------------------------------------------
// Each render builds a FRESH screen and swaps to it with a short fade, letting
// LVGL auto-delete the previous Xander screen *after* the animation completes.
// This is the same deferred-deletion pattern the launcher uses, and it is the
// key reason a re-render triggered from a button's own click handler is safe:
// the screen the button lives on is not freed until the current event returns.
static void x_build_into(lv_obj_t *scr) {
    g_x_resp_lbl = nullptr;
    g_x_ta       = nullptr;   // the old textarea is freed with the screen below
    g_x_strip    = nullptr;   // ...and so is the header strip
    if (g_x_console)            x_render_console();
    else if (g_x_top == 0)      x_render_puzzles();
    else if (g_x_top == 2)      x_render_knowledge();
    else if (g_x_top == 3)      x_render_stories();
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
    if (i < 0 || i >= x_nart()) return;
    // v2.2: the missing half of v1.9.  The mask was declared and persisted,
    // but nothing ever set a bit, so x_pick_unread() returned the same
    // article forever and "teach me something" never moved on.
    if (!x_read_bit(i)) {
        g_xread_mask |= (1ull << i);
        g_xs_bits += 1;              // v2.15: the library feeds the purse
        x_stats_save();
    }
    g_x_console = false; g_x_top = 2;
    g_x_kn_cat = x_art(i).category; g_x_kn_view = 2; g_x_kn_art = i; g_x_kn_more = false;
    // v2.13: a library remembers where you were.
    if (g_x_kn_last != i) { g_x_kn_last = i; x_stats_save(); }
    g_x_orb = 1; x_render();
}
static void x_open_puzzle(int i) {
    g_x_console = false; g_x_top = 0;
    g_x_puz_view = 1; g_x_puz_id = i; g_x_puz_hint = false; g_x_puz_reveal = false;
    g_x_ans_mode = false;
    g_x_orb = 1; x_render();
}
// v1.1: one place that changes the panel level so voice, the Settings slider
// and the persisted wake target can never disagree.
static void x_bright_apply(int v) {
    if (v < 20)  v = 20;
    if (v > 255) v = 255;
    if (screenIsOff) screen_on();         // never adjust an invisible panel
    g_brightness_target = (uint8_t)v;
    // Direct set, NOT fade_brightness(): this runs inside an LVGL event and
    // the fade helper pumps lv_timer_handler, which must never be re-entered
    // from an event.  The Settings slider behaves identically.
    static_cast<Arduino_CO5300 *>(gfx)->setBrightness((uint8_t)v);
    g_cur_brightness = (uint8_t)v;
    g_prefs.begin("dcw", false);
    g_prefs.putInt("bright", v);
    g_prefs.end();
}
static String x_bright_msg(uint8_t v) {
    char b[48];
    snprintf(b, sizeof(b), "Brightness %d%%.", (int)(((int)v * 100) / 255));
    return String(b);
}

static void x_do(int intent) {
    // A recognised command is user activity (v1.6): it must not be treated as
    // an idle period just because the input came from voice or a chip.
    pm_activity("xander command");
    x_sugg_clear();          // a real command retires the "did you mean" chips
    // Cancelling is always the safe direction, so a different command during the
    // swap's cancel window means "not now".
    if (intent != XI_SWAP_SLOT && intent != XI_SWAP_CANCEL &&
        intent != XI_SWAP_BACK) swap_abort("new command");
    switch (intent) {
    case XI_HELP:
        g_x_last = "Try: open AstroOS, sync time, what time is it, brightness up, "
                   "battery status, system status, wifi on/off, restart, shut down. "
                   "Swipe up to enter, down to exit, left for puzzles, right for knowledge.";
        g_x_orb = 0; x_finish_info(); return;
    case XI_HOME_EXIT:      xander_exit(); return;
    case XI_SWAP_SLOT:      swap_arm(); return;
    case XI_SWAP_BACK:      swap_arm_back(); return;
    case XI_SWAP_CANCEL:
        if (!swap_abort("you said cancel")) {
            g_x_last = "Nothing to cancel."; g_x_orb = 0; x_finish_info();
        }
        return;
    case XI_OPEN_PUZZLES:   g_x_console = false; g_x_top = 0; g_x_puz_view = 0; g_x_orb = 1; x_render(); return;
    case XI_OPEN_KNOWLEDGE: g_x_console = false; g_x_top = 2; g_x_kn_view = 0; g_x_kn_cat = ""; g_x_orb = 1; x_render(); return;
    case XI_OPEN_STORIES:   g_x_console = false; g_x_top = 3; g_x_st_view = 0; g_x_orb = 1; x_render(); return;
    case XI_GIVE_PUZZLE:
        x_open_puzzle(x_pick_unsolved((int)(millis() % X_NPUZ)));
        g_x_last = x_say(X_R_GIVE, X_NBANK(X_R_GIVE));
        return;
    case XI_TEACH:
        x_open_article(x_pick_unread((int)(millis() % x_nart())));
        g_x_last = "Read this one.";
        return;
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
    // ---- v1.1 clock -------------------------------------------------------
    case XI_SYNC_TIME:
    case XI_SYNC_DATE: {
        if (!g_wifi_toggle) {
            g_x_last = "WiFi is off - turn it on first.";
            g_x_orb = 2; x_finish_info(); return;
        }
        // Blocking by design, but never from inside this event: arm the
        // deferred worker one paint from now so "Syncing..." is visible and
        // LVGL is not re-entered from a callback.
        g_x_last = "Syncing over WiFi..."; g_x_orb = 3; x_finish_info();   // orb 3 = busy
        g_x_sync_date = (intent == XI_SYNC_DATE);
        g_x_sync_at   = millis() + 80;
        return;
    }
    case XI_TIME_STATUS: {
        char b[128];
        long oh = (long)(g_tp_gmt / 3600);
        long om = (long)(labs((long)g_tp_gmt % 3600L) / 60);
        snprintf(b, sizeof(b), "Clock %s - %s - UTC%s%ld:%02ld",
                 g_ntp_synced ? "synced" : "not synced",
                 g_time_24h ? "24-hour" : "12-hour",
                 g_tp_gmt < 0 ? "-" : "+", labs(oh), om);
        g_x_last = b; g_x_orb = g_ntp_synced ? 1 : 0; x_finish_info(); return;
    }
    case XI_GET_TIME: {
        struct tm tm;
        if (!getLocalTime(&tm, 5)) { g_x_last = "The clock is not set yet."; g_x_orb = 2; }
        else {
            char b[32];
            strftime(b, sizeof(b), g_time_24h ? "It is %H:%M." : "It is %I:%M %p.", &tm);
            g_x_last = b; g_x_orb = 1;
        }
        x_finish_info(); return;
    }
    case XI_GET_DATE: {
        struct tm tm;
        if (!getLocalTime(&tm, 5)) { g_x_last = "The clock is not set yet."; g_x_orb = 2; }
        else {
            char b[64];
            strftime(b, sizeof(b), "Today is %A %d %B %Y.", &tm);
            g_x_last = b; g_x_orb = 1;
        }
        x_finish_info(); return;
    }
    case XI_GET_DAY: {
        struct tm tm;
        if (!getLocalTime(&tm, 5)) { g_x_last = "The clock is not set yet."; g_x_orb = 2; }
        else {
            char b[48];
            strftime(b, sizeof(b), "It is %A.", &tm);
            g_x_last = b; g_x_orb = 1;
        }
        x_finish_info(); return;
    }
    case XI_OPEN_CLOCK_SETTINGS:
        // Leave through the deferred worker so the Settings screen is built
        // from loop(), not from inside the LVGL event that got us here.
        g_x_goto_settings = true;
        xander_exit();
        return;
    case XI_TIME_12H:
    case XI_TIME_24H: {
        g_time_24h = (intent == XI_TIME_24H);
        g_prefs.begin("dcw", false);
        g_prefs.putBool("t24", g_time_24h);
        g_prefs.end();
        update_face_time();
        g_x_last = g_time_24h ? "Clock set to 24-hour time." : "Clock set to 12-hour time.";
        g_x_orb = 1; x_finish_info(); return;
    }
    // ---- v1.1 display -----------------------------------------------------
    case XI_BRIGHT_UP:
        x_bright_apply((int)g_brightness_target + 30);
        g_x_last = x_bright_msg(g_brightness_target); g_x_orb = 1; x_finish_info(); return;
    case XI_BRIGHT_DOWN:
        x_bright_apply((int)g_brightness_target - 30);
        g_x_last = x_bright_msg(g_brightness_target); g_x_orb = 1; x_finish_info(); return;
    case XI_BRIGHT_MAX:
        x_bright_apply(255);
        g_x_last = "Brightness at maximum."; g_x_orb = 1; x_finish_info(); return;
    case XI_BRIGHT_MIN:
        x_bright_apply(20);
        g_x_last = "Brightness at minimum."; g_x_orb = 1; x_finish_info(); return;
    case XI_BRIGHT_50:
        x_bright_apply(128);
        g_x_last = "Brightness at 50 percent."; g_x_orb = 1; x_finish_info(); return;
    // ---- v1.1 power / system ----------------------------------------------
    case XI_RESTART:
        time_save_now("voice restart");
        g_x_last = "Restarting."; g_x_orb = 1; x_finish_info();
        delay(250);          // no lvgl_pump() here: this is inside an event
        esp_restart();
        return;
    case XI_RESET_ALL:
        x_stats_wipe();
        g_x_last = "Everything is back to the first page.";
        g_x_orb  = 1; g_x_console = false; g_x_top = 0;
        x_render();
        return;
    case XI_SHUTDOWN:
        time_save_now("voice shutdown");
        g_x_last = "Shutting down."; g_x_orb = 1; x_finish_info();
        delay(250);
        WiFi.disconnect(true, true);
        WiFi.mode(WIFI_OFF);
        btStop();
        if (g_has_pmu) g_pmu.shutdown();   // AXP2101 hard power-off (press PWR to boot)
        deep_sleep_now();                  // only reached if the PMU refused
        return;
    case XI_GET_MEMORY: {
        char b[128];
        snprintf(b, sizeof(b), "Heap %u KB free of %u KB. PSRAM %u KB free of %u KB.",
                 (unsigned)(ESP.getFreeHeap() / 1024), (unsigned)(ESP.getHeapSize() / 1024),
                 (unsigned)(ESP.getFreePsram() / 1024), (unsigned)(ESP.getPsramSize() / 1024));
        g_x_last = b; g_x_orb = 1; x_finish_info(); return;
    }
    case XI_GET_FREE_RAM: {
        char b[112];
        snprintf(b, sizeof(b), "%u KB RAM and %u KB PSRAM free.",
                 (unsigned)(ESP.getFreeHeap() / 1024), (unsigned)(ESP.getFreePsram() / 1024));
        g_x_last = b; g_x_orb = 1; x_finish_info(); return;
    }
    case XI_GET_FLASH_STORAGE: {
        const esp_partition_t *p = esp_ota_get_running_partition();
        char b[128];
        snprintf(b, sizeof(b), "Flash %u MB. Firmware %u KB in a %u KB slot.",
                 (unsigned)(ESP.getFlashChipSize() / (1024u * 1024u)),
                 (unsigned)(ESP.getSketchSize() / 1024u),
                 (unsigned)((p ? p->size : 0) / 1024u));
        g_x_last = b; g_x_orb = 1; x_finish_info(); return;
    }
    case XI_GET_SD_STATUS: {
        char b[112];
        if (!g_sd_ok) { g_x_last = "No SD card mounted."; g_x_orb = 2; }
        else {
            uint8_t t = (uint8_t)SD_MMC.cardType();
            snprintf(b, sizeof(b), "SD %s, %u MB, %u MB used.",
                     t == CARD_NONE ? "not present" : "mounted",
                     (unsigned)(SD_MMC.cardSize() / (1024ULL * 1024ULL)),
                     (unsigned)(SD_MMC.usedBytes() / (1024ULL * 1024ULL)));
            g_x_last = b; g_x_orb = 1;
        }
        x_finish_info(); return;
    }
    case XI_GET_FREE_STORAGE: {
        char b[112];
        if (!g_sd_ok) { g_x_last = "No SD card mounted."; g_x_orb = 2; }
        else {
            uint64_t total = SD_MMC.totalBytes();
            uint64_t used  = SD_MMC.usedBytes();
            if (used > total) used = total;
            snprintf(b, sizeof(b), "%u MB free of %u MB on the card.",
                     (unsigned)((total - used) / (1024ULL * 1024ULL)),
                     (unsigned)(total / (1024ULL * 1024ULL)));
            g_x_last = b; g_x_orb = 1;
        }
        x_finish_info(); return;
    }
    case XI_GET_SYSTEM_STATUS: {
        char b[168];
        snprintf(b, sizeof(b), "Battery %d%%%s  WiFi %s  BLE %s  SD %s  Heap %u KB  up %lumin",
                 g_batt_pct < 0 ? 0 : g_batt_pct, g_charging ? " chg" : "",
                 g_wifi_connected ? "on" : "off", g_ble_toggle ? "on" : "off",
                 g_sd_ok ? "ok" : "none",
                 (unsigned)(ESP.getFreeHeap() / 1024),
                 (unsigned long)(millis() / 60000UL));
        g_x_last = b; g_x_orb = 1; x_finish_info(); return;
    }
    default: g_x_last = x_miss_line(); g_x_orb = 2; x_finish_info(); return;
    }
}
static void x_process(String raw) {
    String in = raw; in.toLowerCase(); in.trim();
    if (in.length() == 0) { x_sugg_clear(); g_x_last = x_miss_line(); g_x_orb = 2; x_finish_info(); return; }

    String t[12]; int n = x_norm_tokens(in, t, 12);

    // Typing anything while a slot swap is waiting is a "no" (v1.7.1).
    swap_abort("new command");

    // 0) secrets -- typed only.  The spoken vocabulary is fixed by the model in
    //    srmodels.bin, so the console is the one place new words can live.
    if (x_secret(in)) return;

    // 1) A named OS on the card ("open AstroOS", "spotifyos", "go to dual link").
    if (x_try_launch_os(t, n)) return;

    // 2) The fixed v1.1 phrase table.  This runs BEFORE the "what is" knowledge
    //    trigger, otherwise "what is the time" would be answered with "I do not
    //    have that in my library yet".
    int pi = -1;
    if (x_phrase_match(t, n, pi)) { x_do(pi); return; }

    // 3) knowledge-topic triggers
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
    int ti; int ts = x_best_tmpl(t, n, ti);
    int ai; int as = x_find_app_tok(t, n, ai);
    bool launchVerb = x_tok_has(t, n, "launch") || x_tok_has(t, n, "start") ||
                      x_tok_has(t, n, "run") || x_tok_has(t, n, "open");
    if (as > 0 && (as > ts || (launchVerb && as >= ts))) { x_launch_app(ai); return; }
    if (ts >= 1 && ti >= 0) { x_do(kTmpl[ti].intent); return; }
    if (as > 0) { x_launch_app(ai); return; }
    x_sugg_from(in);   // offer the two closest commands instead of a dead end
    g_x_last = x_miss_line(); g_x_orb = 2; x_finish_info();
}

// ---- enter / exit / swipe --------------------------------------------------
static void xander_enter() {
    x_stats_load();
    x_sd_load();                    // v2.6: /WATCH/Xander/know.txt, if the card has one
    g_xander_active = true;
    current_screen  = SCREEN_FACE;   // exit always returns to the watch face
    g_x_top = 1; g_x_console = false; g_x_orb = 0; g_x_voice_show = false; g_x_kb = true;
    g_x_puz_view = 0; g_x_kn_view = 0; g_x_kn_cat = "";
    x_nick_pick();
    g_x_last     = x_greet_line();
    g_x_greet    = g_x_last;      // v2.15: only this line is ever refreshed
    g_x_greet_ms = millis();
    g_x_echo = "";
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
    // v1.1 deferred NTP sync (armed by XI_SYNC_TIME / XI_SYNC_DATE).  Runs in
    // loop() context, one paint after the "Syncing..." screen was built, so
    // the ~10 s round-trip cannot re-enter LVGL from an event callback.
    if (g_x_sync_at && millis() >= g_x_sync_at) {
        g_x_sync_at = 0;
        g_ntp_synced     = false;     // honest result, even if it was true before
        g_wifi_connected = false;
        wifi_connect_and_sync();      // connects, NTPs, then drops WiFi again
        lastTouchMs = millis();       // the round-trip can outlast the sleep timer
        if (g_ntp_synced) {
            time_save_now("voice sync");
            g_x_last = g_x_sync_date ? "Date and time synced." : "Time synced.";
            g_x_orb  = 1;
        } else {
            g_x_last = "Sync failed - no network reachable.";
            g_x_orb  = 2;
        }
        if (current_screen == SCREEN_FACE) update_face_time();
        if (g_xander_active) { g_x_action = XA_RENDER; }
    }

    // v2.15: the layer is alive while it is open.  Two things follow: the
    // header strip is refreshed in place (its clock used to be the arrival
    // time), and a deep page holds the panel lit -- a story page takes longer
    // to read than the 10 s idle countdown allows, so it used to go dark
    // mid-sentence.  The hold is bounded, so a page left open still sleeps.
    if (g_xander_active) {
        if (g_x_strip && millis() - g_x_strip_ms >= 1000) {
            g_x_strip_ms = millis();
            x_strip_refresh();
        }
        if (x_depth()) {
            if (!g_x_read_ms) g_x_read_ms = millis();
            if (millis() - g_x_read_ms < X_READ_HOLD_MS) lastTouchMs = millis();
        } else g_x_read_ms = 0;
        if (g_x_top == 1 && !g_x_console && g_x_greet.length() &&
            g_x_last == g_x_greet && millis() - g_x_greet_ms >= 60000UL) {
            g_x_greet_ms = millis();
            g_x_greet    = x_greet_line();
            g_x_last     = g_x_greet;
            if (g_x_resp_lbl) lv_label_set_text(g_x_resp_lbl, g_x_greet.c_str());
        }
    }
    if (g_x_action == XA_NONE) return;
    XAction act = g_x_action;
    g_x_action = XA_NONE;

    if (act == XA_ENTER) {
        // Free what we safely can BEFORE building so the pool has headroom.
        xander_free_launcher_screens();
        if (!g_x_scr) g_x_scr = x_new_screen();
        x_orb_anim_stop();
        lv_obj_clean(g_x_scr);
        g_x_resp_lbl = nullptr;
        x_build_into(g_x_scr);
        lv_scr_load_anim(g_x_scr, LV_SCR_LOAD_ANIM_FADE_ON, 160, 0, false);
        g_x_free_at = millis() + 240;   // free the (now-inactive) enter screen next
    } else if (act == XA_RENDER) {
        if (g_xander_active) {
            if (!g_x_scr) g_x_scr = x_new_screen();
            bool was_active = (lv_scr_act() == g_x_scr);
            x_orb_anim_stop();          // the orb it targets is about to be freed
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
        x_orb_anim_stop();
        if (g_x_scr) lv_obj_clean(g_x_scr);
        g_x_resp_lbl = nullptr;
        g_x_ta       = nullptr;
        x_stats_export();               // v2.15: the card copy outlives a flash erase
        xander_rebuild_launcher_screens();
        // v1.1: "open clock settings" lands on Settings; every other exit
        // (swipe down, "return home", help) lands on the clock face as before.
        bool to_settings = g_x_goto_settings;
        g_x_goto_settings = false;
        current_screen = to_settings ? SCREEN_SETTINGS : SCREEN_FACE;
        lastTouchMs = millis();
        lv_scr_load_anim(screens[current_screen], LV_SCR_LOAD_ANIM_FADE_ON, 160, 0, false);
    }
}
static void xander_handle_swipe(SwipeEvent ev) {
    if (!g_xander_active) return;
    if (g_x_console) { if (ev == SWIPE_DOWN) x_back(); return; }
    if (ev == SWIPE_DOWN) { xander_exit(); return; }
    if (ev == SWIPE_LEFT || ev == SWIPE_RIGHT) {
        // first pop any open sub-view before moving the top-level ring
        if (x_back()) return;
        int ni = g_x_top + (ev == SWIPE_LEFT ? -1 : 1);
        if (ni < 0) ni = 0; if (ni > 3) ni = 3;
        if (ni != g_x_top) {
            g_x_top = ni;
            if (ni == 0) g_x_puz_view = 0;
            if (ni == 3) g_x_st_view = 0;
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
// Declared here because the Always-On toggle is built long before the
// push-to-talk engine (and its overlay) that these reach into.
static void aon_restart();
static void aon_wake_disarm(bool wait);
static void ptt_flash(const String &msg, uint32_t holdMs = 0);   // 0 = hold scales with the text

static void xset_aon_cb(lv_event_t *e) {
    if (millis() - lastSwipeMs < CLICK_AFTER_SWIPE_MS) return;
    g_x_alwayson = lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED); x_settings_save();
    aon_restart();          // apply immediately, no reboot needed
    ptt_flash(g_x_alwayson ? "Always-on on" : "Always-on off", 1200);
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
// Defined with the push-to-talk module far below; declared here because the
// settings card is built before that code.
static void ptt_mictest();
static void ptt_boot_off();
static void xset_mictest_cb(lv_event_t *e) { (void)e; ptt_mictest(); }

// BOOT-to-talk: on = a press opens the mic (the default), off = the button is
// ignored by voice entirely.  Turning it off mid-window closes the mic rather
// than leaving a session open that no press can end.
static void xset_bootmic_cb(lv_event_t *e) {
    if (millis() - lastSwipeMs < CLICK_AFTER_SWIPE_MS) return;
    g_x_bootmic = lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED);
    x_settings_save();
    if (!g_x_bootmic) ptt_boot_off();
    ptt_flash(g_x_bootmic ? "BOOT to talk on" : "BOOT to talk off", 1200);
}


static void xset_charm_cb(lv_event_t *e) {
    if (millis() - lastSwipeMs < CLICK_AFTER_SWIPE_MS) return;
    g_x_charm = (int)lv_dropdown_get_selected(lv_event_get_target(e));
    x_settings_save();
}
static void xset_wifi_cb(lv_event_t *e) {
    (void)e;
    if (millis() - lastSwipeMs < CLICK_AFTER_SWIPE_MS) return;
    wifi_screen_open(SCREEN_SETTINGS);
}
static void show_xander_settings() {
    x_stats_load();
    g_xset_scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(g_xset_scr, lv_color_hex(theme_bg()), 0);
    lv_obj_set_style_bg_opa(g_xset_scr, LV_OPA_COVER, 0);
    lv_obj_clear_flag(g_xset_scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *h1 = lv_label_create(g_xset_scr);
    lv_label_set_text(h1, "Xander");
    lv_obj_set_style_text_color(h1, lv_color_hex(X_PARCHMENT), 0);
    lv_obj_set_style_text_font(h1, &lv_font_montserrat_28, 0);
    lv_obj_align(h1, LV_ALIGN_TOP_LEFT, 24, 22);
    lv_obj_t *h2 = lv_label_create(g_xset_scr);
    lv_label_set_text(h2, "companion settings");
    lv_obj_set_style_text_color(h2, lv_color_hex(X_SMOKE), 0);
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
    // v1.9: a dossier, not a spreadsheet.  g_xs_hints was counted and saved for
    // months without ever being shown; the vault count and the bits are new.
    char sb[160];
    snprintf(sb, sizeof(sb),
             "Vault %d/%d  -  right %ld  -  hints %ld\nbest streak %ld  -  hardest %ld/5  -  bits %ld  -  found %ld",
             x_vault_count(), X_NPUZ, g_xs_correct, g_xs_hints,
             g_xs_best, g_xs_hard, g_xs_bits, g_xs_shen);
    lv_obj_t *stat = lv_label_create(list);
    lv_label_set_text(stat, sb);
    lv_obj_set_style_text_color(stat, lv_color_hex(theme_primary()), 0);
    lv_obj_set_style_text_font(stat, &lv_font_montserrat_14, 0);
    card_action(list, "Test Microphone", "1 s level check, result on serial",
                LV_SYMBOL_AUDIO "  Test", theme_primary(), xset_mictest_cb);
    card_toggle(list, "BOOT to Talk", "press BOOT to open the mic", g_x_bootmic, xset_bootmic_cb);
    card_toggle(list, "Voice Response", "spoken replies (v1.5)", g_x_voice, xset_voice_cb);
    card_toggle(list, "Always-On Xander", "wake-word listening (v1.5)", g_x_alwayson, xset_aon_cb);
    card_dropdown(list, "Charm", "how much Xander you get",
                  "Dry\nCharming\nFull Xander", g_x_charm, xset_charm_cb);
    card_action(list, "Join a network", "scan, password, sync time",
                LV_SYMBOL_WIFI "  Open", theme_primary(), xset_wifi_cb);
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
// ============================================================================
//  v1.9  WIFI SCREEN -- scan, password, save to NVS, sync the clock
// ----------------------------------------------------------------------------
//  Until now a network could only come from the SD list or the hardcoded
//  fallback; there was no way to type one in on the watch.  This screen scans,
//  takes the password on the on-screen keyboard, stores the pair in the "net"
//  NVS namespace and syncs the clock.  Every other sync path then uses the
//  saved network first (wifi_connect_and_sync, batt_wifi_reconnect_all).
//
//  Scanning (~2 s) and joining (~12 s) both block, so neither runs inside an
//  LVGL event: a tap arms a deadline and wifi_screen_service(), called from
//  loop(), does the work with the busy line already painted.
// ============================================================================
static lv_obj_t *g_ws_scr      = nullptr;
static lv_obj_t *g_ws_ta       = nullptr;
static int       g_ws_view     = 0;      // 0 list, 1 password, 2 busy, 3 result
static int       g_ws_return   = SCREEN_BATTERY;
static int       g_ws_action   = 0;      // 0 join + save, 1 sync only
static bool      g_ws_passshow = false;
static String    g_ws_pick_ssid;
static String    g_ws_pick_pass;
static String    g_ws_msg;
static uint32_t  g_ws_msg_col  = 0xBBBBBB;
static uint32_t  g_ws_scan_at  = 0;
static uint32_t  g_ws_join_at  = 0;
// v2.13: the WiFi screen is a modal overlay, and the network name is a field
// rather than a caption, so a hidden or out-of-range network can be joined too.
static bool      g_ws_open      = false;
static bool      g_ws_ssid_edit = false;
static lv_obj_t *g_ws_ssid_ta   = nullptr;
static lv_obj_t *g_ws_kb        = nullptr;

#define WS_SCAN_MAX 16
static String g_ws_scan_ssid[WS_SCAN_MAX];
static int    g_ws_scan_rssi[WS_SCAN_MAX];
static int    g_ws_scan_n = 0;

static void wifi_screen_render();

static void wifi_screen_close() {
    if (g_ws_scr) lv_obj_clean(g_ws_scr);
    g_ws_ta      = nullptr;
    g_ws_ssid_ta = nullptr;
    g_ws_kb      = nullptr;
    g_ws_open    = false;
    int back = g_ws_return;
    if (back < 0 || back >= NUM_SCREENS || !screens[back]) back = SCREEN_FACE;
    current_screen = back;
    lv_scr_load_anim(screens[back], LV_SCR_LOAD_ANIM_FADE_ON, SCREEN_SWITCH_ANIM_MS, 0, false);
}
static void wifi_screen_open(int from_screen) {
    g_ws_return  = from_screen;
    g_ws_view    = 0;
    g_ws_msg_col = 0xBBBBBB;
    g_ws_open    = true;
    g_ws_ssid_edit = false;
    lastTouchMs  = millis();
    if (!g_ws_scr) {
        g_ws_scr = lv_obj_create(NULL);
        lv_obj_set_style_bg_color(g_ws_scr, lv_color_hex(theme_bg()), 0);
        lv_obj_set_style_bg_opa(g_ws_scr, LV_OPA_COVER, 0);
        lv_obj_clear_flag(g_ws_scr, LV_OBJ_FLAG_SCROLLABLE);
    }
    wifi_saved_load();
    g_ws_msg = g_ws_n ? "Pick a saved network, or scan for a new one."
                      : "Tap Scan to look for networks.";
    wifi_screen_render();
    lv_scr_load_anim(g_ws_scr, LV_SCR_LOAD_ANIM_FADE_ON, SCREEN_SWITCH_ANIM_MS, 0, false);
}
static void wifi_back_cb(lv_event_t *) {
    if (millis() - lastSwipeMs < CLICK_AFTER_SWIPE_MS) return;
    if (g_ws_view == 1) { g_ws_view = 0; g_ws_msg = ""; wifi_screen_render(); return; }
    wifi_screen_close();
}
static void wifi_scan_cb(lv_event_t *) {
    if (millis() - lastSwipeMs < CLICK_AFTER_SWIPE_MS) return;
    if (g_ws_scan_at || g_ws_join_at) return;
    g_ws_view    = 2;
    g_ws_scan_n  = 0;                  // drop the previous scan's rows
    g_ws_msg     = "Scanning for networks...";
    g_ws_msg_col = 0xBBBBBB;
    wifi_screen_render();
    g_ws_scan_at = millis() + 150;
}
static void wifi_pick_cb(lv_event_t *e) {
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    if (i < 0 || i >= g_ws_scan_n) return;
    wifi_saved_load();
    g_ws_pick_ssid = g_ws_scan_ssid[i];
    g_ws_pick_pass = "";
    for (int k = 0; k < g_ws_n; k++)
        if (g_ws_ssid[k] == g_ws_pick_ssid) g_ws_pick_pass = g_ws_pass[k];
    g_ws_view      = 1;
    g_ws_passshow  = false;
    g_ws_ssid_edit = false;
    g_ws_msg       = g_ws_pick_pass.length() ? "Saved password - connect or change it." : "";
    g_ws_msg_col  = 0xBBBBBB;
    wifi_screen_render();
}
static void wifi_saved_cb(lv_event_t *e) {
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    wifi_saved_load();
    if (i < 0 || i >= g_ws_n) return;
    g_ws_pick_ssid = g_ws_ssid[i];
    g_ws_pick_pass = g_ws_pass[i];
    g_ws_action    = 0;
    g_ws_view      = 2;
    g_ws_msg       = String("Connecting to ") + g_ws_pick_ssid + "...";
    g_ws_msg_col   = 0xBBBBBB;
    wifi_screen_render();
    g_ws_join_at = millis() + 150;
}
static void wifi_forget_cb(lv_event_t *e) {
    wifi_saved_erase((int)(intptr_t)lv_event_get_user_data(e));
    g_ws_view    = 0;
    g_ws_msg     = "Forgotten.";
    g_ws_msg_col = 0xFFB300;
    wifi_screen_render();
}
static void wifi_join_cb(lv_event_t *) {
    if (millis() - lastSwipeMs < CLICK_AFTER_SWIPE_MS) return;
    if (g_ws_join_at) return;
    if (g_ws_ssid_ta) {
        g_ws_pick_ssid = lv_textarea_get_text(g_ws_ssid_ta);
        g_ws_pick_ssid.trim();
    }
    if (g_ws_ta) g_ws_pick_pass = lv_textarea_get_text(g_ws_ta);
    if (!g_ws_pick_ssid.length()) return;
    g_ws_action  = 0;
    g_ws_view    = 2;
    g_ws_msg     = String("Connecting to ") + g_ws_pick_ssid + "...";
    g_ws_msg_col = 0xBBBBBB;
    wifi_screen_render();
    g_ws_join_at = millis() + 150;
}
static void wifi_sync_cb(lv_event_t *) {
    if (millis() - lastSwipeMs < CLICK_AFTER_SWIPE_MS) return;
    if (g_ws_join_at) return;
    g_ws_action  = 1;
    g_ws_view    = 2;
    g_ws_msg     = "Syncing: trying every known network...";
    g_ws_msg_col = 0xBBBBBB;
    wifi_screen_render();
    g_ws_join_at = millis() + 150;
}
static void wifi_pass_toggle_cb(lv_event_t *) {
    g_ws_passshow = !g_ws_passshow;
    if (g_ws_ta) lv_textarea_set_password_mode(g_ws_ta, !g_ws_passshow);
}

// Runs from loop(), never from a tap: scanNetworks() and the join wait both
// block for seconds and would freeze the frame mid-animation.
static void wifi_screen_service() {
    if (g_ws_scan_at && (int32_t)(millis() - g_ws_scan_at) >= 0) {
        g_ws_scan_at = 0;
        WiFi.mode(WIFI_STA);
        WiFi.disconnect(false, false);
        delay(60);             // the radio needs a beat after the last WIFI_OFF
        WiFi.scanDelete();     // never mix one scan's rows with another's
        int raw = WiFi.scanNetworks();
        // -2 means "a scan is already running" (the 12 h resync or the battery
        // button) and -1 means the driver refused.  Neither is "no networks",
        // and reporting them as an empty list is what made this screen look
        // dead.  Wait for the other scan to finish and ask once more.
        if (raw < 0) { delay(500); raw = WiFi.scanNetworks(); }
        USBSerial.printf("[wifi] scan returned %d\n", raw);
        if (raw < 0) {
            g_ws_scan_n  = 0;
            g_ws_view    = 0;
            g_ws_msg     = "Radio busy - tap Scan again.";
            g_ws_msg_col = 0xFFB300;
            lastTouchMs  = millis();
            wifi_screen_render();
            return;
        }
        // v2.1: collapse duplicates (every mesh node advertises the same SSID),
        // drop hidden ones with no name, keep the strongest copy, strongest
        // first -- so the list is the actual networks in range rather than the
        // first twelve rows the driver happened to return.
        int found = 0;
        for (int i = 0; i < raw; i++) {
            String ss = WiFi.SSID(i);
            ss.trim();
            if (!ss.length()) {                      // hidden AP: nothing to tap
                USBSerial.printf("[wifi]   %d: <hidden> %d dBm\n", i, WiFi.RSSI(i));
                continue;
            }
            const int r = WiFi.RSSI(i);
            USBSerial.printf("[wifi]   %d: \"%s\" %d dBm\n", i, ss.c_str(), r);
            int at = -1;
            for (int k = 0; k < found; k++)
                if (g_ws_scan_ssid[k] == ss) { at = k; break; }
            if (at >= 0) {
                if (r > g_ws_scan_rssi[at]) g_ws_scan_rssi[at] = r;
                continue;
            }
            if (found >= WS_SCAN_MAX) continue;
            int pos = found;
            while (pos > 0 && g_ws_scan_rssi[pos - 1] < r) {
                g_ws_scan_ssid[pos] = g_ws_scan_ssid[pos - 1];
                g_ws_scan_rssi[pos] = g_ws_scan_rssi[pos - 1];
                pos--;
            }
            g_ws_scan_ssid[pos] = ss;
            g_ws_scan_rssi[pos] = r;
            found++;
        }
        g_ws_scan_n = found;
        WiFi.scanDelete();
        g_ws_view = 0;
        if (g_ws_scan_n) {
            char m[48];
            snprintf(m, sizeof(m), "%d found - tap one to connect", g_ws_scan_n);
            g_ws_msg     = m;
            g_ws_msg_col = 0x00FF80;
        } else {
            g_ws_msg     = "No networks found. Try again near the router.";
            g_ws_msg_col = 0xFFB300;
        }
        lastTouchMs = millis();
        wifi_screen_render();
        return;
    }
    if (g_ws_join_at && (int32_t)(millis() - g_ws_join_at) >= 0) {
        g_ws_join_at = 0;
        bool ok = false;
        String got = g_ws_pick_ssid;
        if (g_ws_action == 0) {
            ok = wifi_join(g_ws_pick_ssid.c_str(), g_ws_pick_pass.c_str(), 12000);
            if (ok) {
                String real = WiFi.SSID();
                if (real.length()) got = real;
                wifi_saved_put(got, g_ws_pick_pass);
                g_wifi_toggle = true;      // the 12 h resync can use it now
            }
        } else {
            // v2.1: walk saved -> config.txt -> hardcoded, showing which one the
            // radio is on right now instead of silently retrying the newest
            // saved entry and calling that "sync".
            String cs[WIFI_CAND_MAX], cp[WIFI_CAND_MAX];
            const int nc = wifi_candidates(cs, cp, WIFI_CAND_MAX);
            for (int i = 0; i < nc && !ok; i++) {
                g_ws_msg     = String("Trying ") + (i + 1) + "/" + nc + ": " + cs[i] + "...";
                g_ws_msg_col = 0xBBBBBB;
                wifi_screen_render();
                ok = wifi_join(cs[i].c_str(), cp[i].c_str(), 8000);
                if (ok) got = cs[i];
                if (ok && WiFi.SSID().length()) got = WiFi.SSID();
            }
        }
        if (ok) {
            ntp_sync_now();
            g_ws_msg = g_ntp_synced
                     ? String("Connected: ") + got + "  - clock synced."
                     : String("Connected: ") + got + "  - NTP failed.";
            g_ws_msg_col = g_ntp_synced ? 0x00FF80 : 0xFFB300;
        } else {
            g_ws_msg     = String("Could not join ") + got + ". Check the password.";
            g_ws_msg_col = 0xFF4040;
        }
        // The clock was the point; don't leave the radio running for it.
        WiFi.disconnect(false, false);
        WiFi.mode(WIFI_OFF);
        g_wifi_connected = false;
#if POWER_SAVE
        g_last_sync_ms = millis();
#endif
        g_ws_view   = 3;
        lastTouchMs = millis();
        wifi_screen_render();
        return;
    }
}

// v2.9: the hardcoded network is a real entry in the list, not a hidden
// fallback.  Tapping it opens the password page pre-filled with the built-in
// password, so it can be corrected on the watch and saved to NVS like any
// other -- which is what makes it stick for next time.
static void wifi_hc_pick_cb(lv_event_t *) {
    if (millis() - lastSwipeMs < CLICK_AFTER_SWIPE_MS) return;
    g_ws_pick_ssid = HC_WIFI_SSID;
    g_ws_pick_pass = HC_WIFI_PASS;
    g_ws_passshow  = false;
    g_ws_ssid_edit = false;
    g_ws_view      = 1;               // password page
    g_ws_msg       = "Built-in network - password is pre-filled.";
    g_ws_msg_col   = 0xBBBBBB;
    wifi_screen_render();
}

// v2.13: a network that the scan cannot see -- hidden SSID, 5 GHz-only AP,
// out of range right now -- is still joinable: the name is typed on the same
// page as the password instead of being a caption you cannot change.
static void wifi_other_cb(lv_event_t *) {
    if (millis() - lastSwipeMs < CLICK_AFTER_SWIPE_MS) return;
    g_ws_pick_ssid = "";
    g_ws_pick_pass = "";
    g_ws_passshow  = false;
    g_ws_ssid_edit = true;
    g_ws_view      = 1;
    g_ws_msg       = "Type the network name, then its password.";
    g_ws_msg_col   = 0xBBBBBB;
    wifi_screen_render();
}
// The keyboard drives one textarea at a time; tapping the password field moves
// it there, so the name and the password can both be typed on this page.
static void wifi_pass_focus_cb(lv_event_t *) {
    if (g_ws_kb && g_ws_ta) lv_keyboard_set_textarea(g_ws_kb, g_ws_ta);
}

static void wifi_screen_render() {
    if (!g_ws_scr) return;
    lv_obj_clean(g_ws_scr);
    g_ws_ta      = nullptr;
    g_ws_ssid_ta = nullptr;
    g_ws_kb      = nullptr;

    lv_obj_t *bk = lv_btn_create(g_ws_scr);
    lv_obj_remove_style_all(bk);
    lv_obj_clear_flag(bk, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(bk, 46, 40);
    lv_obj_align(bk, LV_ALIGN_TOP_LEFT, 8, 8);
    lv_obj_set_style_bg_opa(bk, LV_OPA_TRANSP, 0);
    lv_obj_t *bl = lv_label_create(bk);
    lv_label_set_text(bl, LV_SYMBOL_LEFT);
    lv_obj_set_style_text_color(bl, lv_color_hex(X_ACCENT), 0);
    lv_obj_set_style_text_font(bl, &lv_font_montserrat_16, 0);
    lv_obj_center(bl);
    lv_obj_add_event_cb(bk, wifi_back_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *t = lv_label_create(g_ws_scr);
    lv_label_set_text(t, "WIFI");
    lv_obj_set_style_text_color(t, lv_color_hex(X_ACCENT), 0);
    lv_obj_set_style_text_font(t, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_letter_space(t, 2, 0);
    lv_obj_align(t, LV_ALIGN_TOP_MID, 0, 14);

    lv_obj_t *rule = lv_obj_create(g_ws_scr);
    lv_obj_remove_style_all(rule);
    lv_obj_clear_flag(rule, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(rule, 76, 2);
    lv_obj_set_style_bg_color(rule, lv_color_hex(X_ACCENT), 0);
    lv_obj_set_style_bg_opa(rule, 200, 0);
    lv_obj_set_style_radius(rule, 1, 0);
    lv_obj_align(rule, LV_ALIGN_TOP_MID, 0, 38);

    lv_obj_t *msg = x_wraplbl(g_ws_scr, g_ws_msg.length() ? g_ws_msg.c_str() : "",
                              LCD_WIDTH - 60, g_ws_msg_col, &lv_font_montserrat_12);
    lv_obj_set_style_text_align(msg, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(msg, LV_ALIGN_TOP_MID, 0, 46);

    if (g_ws_view == 1) {
        if (g_ws_ssid_edit) {
            g_ws_ssid_ta = lv_textarea_create(g_ws_scr);
            lv_textarea_set_one_line(g_ws_ssid_ta, true);
            lv_textarea_set_placeholder_text(g_ws_ssid_ta, "network name");
            lv_textarea_set_text(g_ws_ssid_ta, g_ws_pick_ssid.c_str());
            lv_obj_set_width(g_ws_ssid_ta, LCD_WIDTH - 40);
            lv_obj_set_style_text_font(g_ws_ssid_ta, &lv_font_montserrat_16, 0);
            lv_obj_align(g_ws_ssid_ta, LV_ALIGN_TOP_LEFT, 20, 74);
        } else {
            char sl[80];
            snprintf(sl, sizeof(sl), "Network: %s", g_ws_pick_ssid.c_str());
            lv_obj_t *nl = lv_label_create(g_ws_scr);
            lv_label_set_text(nl, sl);
            lv_obj_set_style_text_color(nl, lv_color_hex(0xFFFFFF), 0);
            lv_obj_set_style_text_font(nl, &lv_font_montserrat_16, 0);
            lv_obj_set_style_text_letter_space(nl, 1, 0);
            lv_obj_align(nl, LV_ALIGN_TOP_LEFT, 20, 78);
        }

        g_ws_ta = lv_textarea_create(g_ws_scr);
        lv_textarea_set_one_line(g_ws_ta, true);
        lv_textarea_set_password_mode(g_ws_ta, !g_ws_passshow);
        lv_textarea_set_placeholder_text(g_ws_ta, "password");
        lv_textarea_set_text(g_ws_ta, g_ws_pick_pass.c_str());
        lv_obj_set_width(g_ws_ta, LCD_WIDTH - 40);
        lv_obj_set_style_text_font(g_ws_ta, &lv_font_montserrat_16, 0);
        lv_obj_align(g_ws_ta, LV_ALIGN_TOP_LEFT, 20, g_ws_ssid_edit ? 132 : 108);
        lv_obj_add_event_cb(g_ws_ta, wifi_pass_focus_cb, LV_EVENT_CLICKED, NULL);

        lv_obj_t *bar = lv_obj_create(g_ws_scr);
        lv_obj_remove_style_all(bar);
        lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_size(bar, LCD_WIDTH - 20, 50);
        lv_obj_align(bar, LV_ALIGN_TOP_MID, 0, g_ws_ssid_edit ? 194 : 170);
        lv_obj_set_flex_flow(bar, LV_FLEX_FLOW_ROW);
        lv_obj_set_style_pad_column(bar, 8, 0);
        x_cbtn(bar, g_ws_passshow ? "Hide" : "Show", 96, theme_accent(), wifi_pass_toggle_cb, 0);
        x_cbtn(bar, LV_SYMBOL_WIFI "  Connect", 178, X_ACCENT, wifi_join_cb, 0);

        lv_obj_t *kb = lv_keyboard_create(g_ws_scr);
        lv_obj_set_size(kb, LCD_WIDTH, 196);
        lv_obj_align(kb, LV_ALIGN_BOTTOM_MID, 0, 0);
        lv_keyboard_set_mode(kb, LV_KEYBOARD_MODE_TEXT_LOWER);
        g_ws_kb = kb;
        lv_keyboard_set_textarea(kb, (g_ws_ssid_edit && g_ws_ssid_ta) ? g_ws_ssid_ta : g_ws_ta);
        return;
    }

    // 76 clears a two-line message; 146 leaves the Scan/Sync bar its 50 px.
    lv_obj_t *list = x_list_in(g_ws_scr, 76, LCD_HEIGHT - 146);
    if (g_ws_view == 2) {
        x_group_hdr(list, "WORKING");
        x_wraplbl(list, "Hold still - the radio is busy.",
                  LCD_WIDTH - 54, 0x888888, &lv_font_montserrat_14);
    } else {
        // FOUND first: after a scan that is what the user is looking for, and
        // it used to sit under the saved rows where the list's own scroll
        // position could leave it entirely off-screen -- the "it only shows how
        // many there are" symptom.
        if (g_ws_scan_n) {
            x_group_hdr(list, "FOUND");
            for (int i = 0; i < g_ws_scan_n; i++) {
                char lbl[96];
                int pct = (g_ws_scan_rssi[i] <= -100) ? 0
                        : (g_ws_scan_rssi[i] >= -50) ? 100
                        : 2 * (g_ws_scan_rssi[i] + 100);
                bool known = false;
                for (int k = 0; k < g_ws_n; k++)
                    if (g_ws_ssid[k] == g_ws_scan_ssid[i]) { known = true; break; }
                snprintf(lbl, sizeof(lbl), "%s  %d%%%s", g_ws_scan_ssid[i].c_str(), pct,
                         known ? "  (saved)" : "");
                x_row_btn(list, lbl, known ? X_ACCENT : theme_primary(),
                          wifi_pick_cb, (intptr_t)i);
            }
        }
        {
            x_group_hdr(list, "BUILT-IN");
            char hcl[96];
            snprintf(hcl, sizeof(hcl), "%s  (hardcoded)", HC_WIFI_SSID);
            x_row_btn(list, hcl, theme_primary(), wifi_hc_pick_cb, (intptr_t)0);
        }
        {
            x_group_hdr(list, "OTHER");
            x_row_btn(list, "Type a network name", theme_primary(), wifi_other_cb, 0);
        }
        if (g_ws_n) {
            x_group_hdr(list, "SAVED");
            for (int i = 0; i < g_ws_n; i++) {
                lv_obj_t *row = lv_obj_create(list);
                lv_obj_remove_style_all(row);
                lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
                lv_obj_set_width(row, LV_PCT(100));
                lv_obj_set_height(row, 52);
                lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
                lv_obj_set_style_pad_column(row, 6, 0);
                lv_obj_t *b = x_row_btn(row, g_ws_ssid[i].c_str(), X_ACCENT,
                                        wifi_saved_cb, (intptr_t)i);
                lv_obj_set_width(b, LCD_WIDTH - 118);
                x_cbtn(row, LV_SYMBOL_CLOSE, 48, theme_danger(), wifi_forget_cb, (intptr_t)i);
            }
        }
        if (!g_ws_n && !g_ws_scan_n)
            x_wraplbl(list, "Nothing saved yet. Scan, then type the password.",
                      LCD_WIDTH - 54, 0x777777, &lv_font_montserrat_14);
        lv_obj_update_layout(list);
        lv_obj_scroll_to_y(list, 0, LV_ANIM_OFF);
    }

    lv_obj_t *bar = lv_obj_create(g_ws_scr);
    lv_obj_remove_style_all(bar);
    lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(bar, LCD_WIDTH - 20, 50);
    lv_obj_align(bar, LV_ALIGN_BOTTOM_MID, 0, -14);
    lv_obj_set_flex_flow(bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(bar, 8, 0);
    x_cbtn(bar, LV_SYMBOL_WIFI "  Scan", (LCD_WIDTH - 46) / 2, X_ACCENT, wifi_scan_cb, 0);
    x_cbtn(bar, LV_SYMBOL_REFRESH "  Sync time", (LCD_WIDTH - 46) / 2, theme_primary(), wifi_sync_cb, 0);
}
// ############################################################################
// #                        END XANDER v1.0 MODULE                            #
// ############################################################################

// ============================================================================
//  v1.2  PUSH-TO-TALK VOICE
//  ----------------------------------------------------------------------------
//  PRESS BOOT (GPIO0) anywhere in the launcher to start listening.  ONE press,
//  no second press:
//      press  -> the mic opens, ESP_SR MultiNet listens in COMMAND mode
//      match  -> the phrase runs (launch OS / system intent) on its own, after
//                a short "HEARD: ..." flash so a wrong match is visible
//      6 s    -> the window closes and whatever was heard runs; a press that
//                heard nothing simply ends
//
//  The button IS the wake word, so "Xander" is not required and is not in the
//  phrase list.
//
//  Not hold-to-talk: you can lower your arm and keep talking, and the window is
//  timed rather than gated on the button staying down.  A second press during
//  the window still ends it early and runs the match, so the wait is optional.
//
//  Why it is shaped this way:
//   * ESP_SR calls onEvent() FROM ITS OWN TASK.  The handler only records the
//     id; teardown, UI and launching all happen in loop() -- no LVGL from a
//     foreign task, and no I2S.end() while a feed task is still reading.
//   * No 'model' partition -> the feature disables itself with one clear log
//     line and every other launcher path behaves exactly as before.
//   * The listener never coexists with another I2S user (the launcher has none).
// ============================================================================
#define PTT_PIN            0      // onboard BOOT, active LOW
#define PTT_DEBOUNCE_MS    25

// ============================================================================
//  MICROPHONE -- read this before touching the pin numbers
// ============================================================================
//  This board has NO raw I2S microphone.  The 2.06 uses an ES7210 ADC with two
//  digital mics feeding it, and an ES8311 for playback; both codecs sit on the
//  SAME I2S clocks (the ESP32-S3 is master and must generate MCLK):
//
//      BCLK 41   LRCK/WS 45   MCLK 16   codec -> ESP32 (DIN) 42
//
//  Those numbers are not guesses: they are BSP_I2S_SCLK / LCLK / MCLK / DSIN in
//  the vendor's own board support package for this exact panel
//  (waveshare/esp32_s3_touch_amoled_2_06).  Pin 40 is the ESP32 -> codec data
//  line and is not used here because the launcher does not play audio.
//
//  A raw-mic setup on any other pins reads nothing at all: the ESP32 happily
//  clocks an idle bus and every sample is 0 -> "-99 dBFS, peak 0".  That was
//  the v7.0 behaviour (it looked for an INMP441 on 17/18/16).
//
//  The ES7210 also has to be woken over I2C before it puts anything on SDOUT.
//  Reset defaults leave its ADC powered down, so without es7210_start() the
//  data line stays silent no matter which pins are used.  The sequence below is
//  the vendor's (esp_codec_dev es7210_open + es7210_enable) in slave mode, MIC1 +
//  MIC2 enabled, 16-bit, plain I2S -- i.e. left slot = MIC1, right = MIC2, which
//  is why a mono-left read gets a single clean mic.
// ============================================================================
#define PTT_I2S_BCLK_PIN   41     // shared with the ES8311
#define PTT_I2S_WS_PIN     45     // LRCK
#define PTT_I2S_DIN_PIN    42     // ES7210 SDOUT -> ESP32
#define PTT_I2S_MCLK_PIN   16     // ESP32 -> both codecs, 256 * fs
#define ES7210_GAIN_CODE   10     // 0..14 = 0..37.5 dB in 3 dB steps; 10 = 30 dB

static uint8_t g_micAddr  = 0;      // 7-bit address that answered, 0 = none
static bool    g_micReady = false;  // registers written, ADC running

static bool mic_i2c_probe(uint8_t a) {
    Wire.beginTransmission(a);
    return Wire.endTransmission() == 0;
}

static bool es7210_write(uint8_t reg, uint8_t val) {
    Wire.beginTransmission(g_micAddr);
    Wire.write(reg);
    Wire.write(val);
    return Wire.endTransmission() == 0;
}

static bool es7210_update(uint8_t reg, uint8_t mask, uint8_t val) {
    Wire.beginTransmission(g_micAddr);
    Wire.write(reg);
    if (Wire.endTransmission(false) != 0) return false;
    if (Wire.requestFrom((int)g_micAddr, 1) != 1) return false;
    uint8_t regv = Wire.read();
    return es7210_write(reg, (uint8_t)((regv & ~mask) | (mask & val)));
}

// One register script that leaves the ES7210 capturing.  Call only while the
// I2S clocks are already running: the chip is a slave, and its power-up state
// machine needs MCLK/BCLK/LRCK to finish starting.  It is re-run on every bus
// open (button, always-on, mic test) because closing I2S stops MCLK, and a
// slave codec whose clocks vanish does not necessarily come back on its own.
static bool es7210_start() {

    // AD1/AD0 strap the address; this board leaves them at 00 (= 0x40), but a
    // wrong address is indistinguishable from a dead codec, so probe first.
    if (!g_micAddr) {
        for (uint8_t a = 0x40; a <= 0x43; a++)
            if (mic_i2c_probe(a)) { g_micAddr = a; break; }
    }
    if (!g_micAddr) {
        USBSerial.println("[mic] no ES7210 on I2C 0x40..0x43 - the mic cannot work");
        return false;
    }

    bool ok = true;
    ok &= es7210_write(0x00, 0xff);              // reset
    ok &= es7210_write(0x00, 0x41);
    ok &= es7210_write(0x01, 0x3f);              // all ADC clocks off for now
    ok &= es7210_write(0x09, 0x30);              // power-on state timing
    ok &= es7210_write(0x0A, 0x30);
    ok &= es7210_write(0x23, 0x2a);              // HPF "quick setup"
    ok &= es7210_write(0x22, 0x0a);
    ok &= es7210_write(0x20, 0x0a);
    ok &= es7210_write(0x21, 0x2a);
    ok &= es7210_update(0x08, 0x01, 0x00);       // slave: clocks come from the S3
    ok &= es7210_write(0x40, 0x43);              // analog power, VDDA 3.3 V
    ok &= es7210_write(0x41, 0x70);              // MIC bias 2.87 V
    ok &= es7210_write(0x42, 0x70);
    ok &= es7210_write(0x07, 0x20);              // OSR
    ok &= es7210_write(0x02, 0xc1);              // ADC clock, clear state

    // MIC1 + MIC2 (two mics => plain I2S, not TDM: TDM only kicks in at 3+).
    for (uint8_t r = 0x43; r <= 0x46; r++) ok &= es7210_update(r, 0x10, 0x00);
    ok &= es7210_write(0x4B, 0xff);
    ok &= es7210_write(0x4C, 0xff);
    ok &= es7210_update(0x01, 0x0b, 0x00);       // enable the MIC1/MIC2 ADC clock
    ok &= es7210_write(0x4B, 0x00);              // MIC bias + PGA for 1/2
    ok &= es7210_update(0x43, 0x10, 0x10);       // MIC1 PGA on
    ok &= es7210_update(0x44, 0x10, 0x10);       // MIC2 PGA on
    ok &= es7210_write(0x12, 0x00);              // SDOUT: 2-slot I2S, not TDM
    ok &= es7210_update(0x43, 0x0f, ES7210_GAIN_CODE);
    ok &= es7210_update(0x44, 0x0f, ES7210_GAIN_CODE);

    // Bring the ADC out of power-down.
    ok &= es7210_write(0x01, 0x34);              // 0x3f with the clock bits cleared
    ok &= es7210_write(0x06, 0x00);
    ok &= es7210_write(0x40, 0x43);
    ok &= es7210_write(0x47, 0x08);
    ok &= es7210_write(0x48, 0x08);
    ok &= es7210_write(0x49, 0x08);
    ok &= es7210_write(0x4A, 0x08);
    ok &= es7210_write(0x40, 0x43);
    ok &= es7210_write(0x00, 0x71);
    ok &= es7210_write(0x00, 0x41);

    // 16-bit samples, Philips I2S framing.
    ok &= es7210_update(0x11, 0xe0, 0x60);
    ok &= es7210_update(0x11, 0x03, 0x00);

    if (!ok) {
        USBSerial.printf("[mic] ES7210 at 0x%02X did not accept its registers\n", g_micAddr);
        return false;
    }
    g_micReady = true;
    USBSerial.printf("[mic] ES7210 at 0x%02X running: 16 kHz, 16-bit, MIC1 left / MIC2 right, gain %d dB\n",
                     g_micAddr, ES7210_GAIN_CODE * 3);
    return true;
}

// Boot-time presence check.  Answers the only question that matters when the
// level reads -99: is the codec there at all, and at which address?
static void mic_probe_report() {
    uint8_t found = 0;
    for (uint8_t a = 0x40; a <= 0x43; a++)
        if (mic_i2c_probe(a)) { found = a; break; }
    const bool es8311 = mic_i2c_probe(0x18) || mic_i2c_probe(0x19);
    USBSerial.printf("[mic] i2c probe: ES7210 %s, ES8311 %s\n",
                     found ? "found" : "** MISSING **",
                     es8311 ? "found" : "** MISSING **");
    if (found) g_micAddr = found;
}
// One press = one window.  The phrase that matched runs automatically when the
// window closes -- there is no second press to remember.
//
// MultiNet closes its own window first: esp32-hal-sr.c creates it with a fixed
// 5760 ms (multinet->create(mn_name, 5760)) and reports SR_EVENT_TIMEOUT, which
// ptt_service() treats as the end of the session.  So the real window is 5.76 s
// and PTT_MAX_SESSION_MS is only the backstop for the case where the engine
// never reaches a verdict at all (dead mic, stalled feed): without it the mic
// would stay open and drain the battery.
#define PTT_MAX_SESSION_MS 6000
#define PTT_RUN_DELAY_MS   700    // show "HEARD: ..." this long, then run it

// PTT_WAKE is the always-on gate.  It lives in the same phrase table as every
// other command, so one ESP_SR engine covers both button push-to-talk and the
// hands-free path -- there is no second sketch and no second model.
enum PttKind : uint8_t { PTT_APP = 0, PTT_INTENT = 1, PTT_WAKE = 2 };

struct PttCmd {
    const char *phrase;   // what MultiNet is trained to hear
    uint8_t     kind;     // PTT_APP | PTT_INTENT
    int16_t     arg;      // XI_* for PTT_INTENT
    const char *bin;      // .bin base name for PTT_APP
};

// Keep the phrases SHORT and DISTINCT: MultiNet spots whole phrases, so
// "astro" and "spotify" overlap far less than "open astro o s" would, and a
// button-gated session has no wake word to strip first.
static const PttCmd kPtt[] = {
    // ---- wake gate for always-on (settings > Always-On Xander) -----------
    { "xander",              PTT_WAKE, 0, nullptr       },
    // ---- the ten OS .bin files -------------------------------------------
    { "astro",               PTT_APP, 0, "AstroOS"    },
    { "spotify",             PTT_APP, 0, "SpotifyOS"  },
    // v2.13g: the two apps that had no spoken name.  "music" is what people
    // actually say for SpotifyOS, and the coffee app is Coffeelings.bin -- the
    // typed console already resolves both spellings, this is the voice door.
    { "music",               PTT_APP, 0, "SpotifyOS"  },
    { "coffee",              PTT_APP, 0, "Coffeelings" },
    { "taekwondo",           PTT_APP, 0, "TKDOS"      },
    { "sport",               PTT_APP, 0, "SportOS"    },
    { "game",                PTT_APP, 0, "GameOS"     },
    { "explore",             PTT_APP, 0, "ExplorerOS" },
    { "tactical",            PTT_APP, 0, "TacticalOS" },
    { "book",                PTT_APP, 0, "BookOS"     },
    { "waypoint",            PTT_APP, 0, "WaypointOS" },
    { "dual link",           PTT_APP, 0, "Dual_Link"  },
    { "find my",             PTT_APP, 0, "Dual_Link"  },
    { "find my phone",       PTT_APP, 0, "Dual_Link"  },
    // ---- v1.7.1 slot swap: writes DualBoot.bin over the RUNNING slot -------
    // MultiNet takes its phrase list at runtime, so these need no new model.
    { "dual boot",           PTT_INTENT, XI_SWAP_SLOT,   nullptr },
    { "hero time",           PTT_INTENT, XI_SWAP_SLOT,   nullptr },
    { "dual",                PTT_INTENT, XI_SWAP_SLOT,   nullptr },
    { "hero",                PTT_INTENT, XI_SWAP_SLOT,   nullptr },
    { "cancel",              PTT_INTENT, XI_SWAP_CANCEL, nullptr },
    { "back to dualboot",    PTT_INTENT, XI_SWAP_BACK,   nullptr },
    { "back to dual",        PTT_INTENT, XI_SWAP_BACK,   nullptr },
    // ---- clock -----------------------------------------------------------
    { "what time is it",     PTT_INTENT, XI_GET_TIME,          nullptr },
    { "what is the date",    PTT_INTENT, XI_GET_DATE,          nullptr },
    { "sync time",           PTT_INTENT, XI_SYNC_TIME,         nullptr },
    { "check time status",   PTT_INTENT, XI_TIME_STATUS,       nullptr },
    // ---- display ---------------------------------------------------------
    { "brightness up",       PTT_INTENT, XI_BRIGHT_UP,         nullptr },
    { "brightness down",     PTT_INTENT, XI_BRIGHT_DOWN,       nullptr },
    { "maximum brightness",  PTT_INTENT, XI_BRIGHT_MAX,        nullptr },
    { "minimum brightness",  PTT_INTENT, XI_BRIGHT_MIN,        nullptr },
    // ---- radios ----------------------------------------------------------
    { "turn wifi on",        PTT_INTENT, XI_WIFI_ON,           nullptr },
    { "turn wifi off",       PTT_INTENT, XI_WIFI_OFF,          nullptr },
    { "turn bluetooth on",   PTT_INTENT, XI_BLE_ON,            nullptr },
    { "turn bluetooth off",  PTT_INTENT, XI_BLE_OFF,           nullptr },
    // ---- power / status / navigation -------------------------------------
    { "battery status",      PTT_INTENT, XI_SHOW_BATTERY,      nullptr },
    { "system status",       PTT_INTENT, XI_GET_SYSTEM_STATUS, nullptr },
    { "restart the watch",   PTT_INTENT, XI_RESTART,           nullptr },
    { "shut down the watch", PTT_INTENT, XI_SHUTDOWN,          nullptr },
    { "open puzzles",        PTT_INTENT, XI_OPEN_PUZZLES,      nullptr },
    { "open knowledge",      PTT_INTENT, XI_OPEN_KNOWLEDGE,    nullptr },
    { "open stories",        PTT_INTENT, XI_OPEN_STORIES,      nullptr },
    { "return home",         PTT_INTENT, XI_HOME_EXIT,         nullptr },
};
#define PTT_N ((int)(sizeof(kPtt) / sizeof(kPtt[0])))

// ---- v1.8: what the Xander console lists -----------------------------------
// The console is defined far above this table, so it reaches the spoken phrases
// through these functions instead of the array itself.  The wake gate and the
// slot swap are deliberately not tappable: the way back to DualBoot is voice
// only, and a console row must not become a second door to it.
static int x_voice_count() { return PTT_N; }
static const char *x_voice_label(int i) {
    return (i >= 0 && i < PTT_N) ? kPtt[i].phrase : "";
}
static bool x_voice_tappable(int i) {
    if (i < 0 || i >= PTT_N) return false;
    const PttCmd &c = kPtt[i];
    if (c.kind == PTT_WAKE) return false;
    return !(c.arg == XI_SWAP_SLOT || c.arg == XI_SWAP_BACK || c.arg == XI_SWAP_CANCEL);
}
static bool x_voice_run(int i) {
    if (!x_voice_tappable(i)) return false;
    const PttCmd &c = kPtt[i];
    if (c.kind == PTT_APP) {
        const int idx = app_resolve(c.bin);
        if (idx < 0) {
            g_x_last = String(c.bin) + " is not on the card."; g_x_orb = 2; x_finish_info();
            return true;
        }
        x_launch_app(idx);            // OTA-copies + reboots; does not return
        return true;
    }
    x_do((int)c.arg);
    return true;
}

static sr_cmd_t   g_pttCmds[PTT_N];      // built in ptt_init()
static I2SClass   g_pttI2S;
static bool       g_pttReady     = false; // phrase table built
static bool       g_pttUsable    = true;  // false after a hard failure
static bool       g_pttArmed     = false; // this press is already handled
static bool       g_pttListening = false;
static volatile int g_pttHit     = 0;     // written by ESP_SR's task
static uint32_t   g_pttStartMs   = 0;
static uint32_t   g_pttHitMs     = 0;     // when loop() first saw the match
static lv_obj_t  *g_pttBox       = nullptr;
static uint32_t   g_pttHideAt    = 0;
static int        g_pttShownHit  = -1;    // hit already painted in the overlay
static uint8_t    g_pttFails     = 0;     // failed starts this boot
static int        g_pttLastId    = 0;
static uint32_t   g_pttLastMs    = 0;

// MultiNet is created with a fixed listening window (esp32-hal-sr.c calls
// create(mn_name, 5760)) and, when it expires, puts the engine in SR_MODE_OFF --
// detection is then OFF, not paused.  For a button press that expiry IS the end
// of the session, so this timestamp is all that is needed to run the match.
// (Always-on keeps one renewal: its gate is 7 s long and a speaker who says
// "xander", pauses, then speaks should not be cut off at 5.76 s.)
static volatile uint32_t g_pttMnExpiredAt = 0;   // set by ESP_SR's task
static volatile uint8_t  g_pttMnRearms    = 0;   // window renewals (always-on only)

// ---- always-on (settings > Always-On Xander) ----------------------------
// One engine, two doors.  The button opens a manual session that runs what it
// heard as soon as it hears it; always-on keeps the same engine open and runs
// a phrase the moment the wake gate has been spoken.  No EI model, no second
// sketch -- MultiNet already has every phrase, "xander" included.
static bool       g_pttAuto      = false; // this session was opened by always-on
static uint32_t   g_aonWakeMs    = 0;     // last time the wake gate was opened
#define AON_GATE_MS        7000   // how long after "xander" a phrase may run

static void ptt_on_event(sr_event_t event, int command_id, int phrase_id) {
    (void)phrase_id;
    if (event == SR_EVENT_COMMAND && command_id > 0 && g_pttHit == 0)
        g_pttHit = command_id;          // first match wins; read in loop()
    else if (event == SR_EVENT_TIMEOUT)
        g_pttMnExpiredAt = millis() ? millis() : 1;   // handled in ptt_service()
}

// ---- LVGL object pool -> PSRAM (see LV_MEM_CUSTOM in lv_conf.h) -----------
// One ~64 KB pool is requested at lv_init() and grown in small steps.  Keeping
// it out of internal DRAM is worth far more to ESP-SR (which needs one large
// contiguous internal block) than the extra latency of touching PSRAM during
// widget layout.  Internal RAM, then the generic heap, are the fallbacks, so a
// device without working PSRAM still boots.
//
// NOTE: defined here, not at the top of the file.  The Arduino prototype pass
// injects generated prototypes ahead of the FIRST function definition in the
// sketch; if that definition sits above `struct LTheme`, every later prototype
// that mentions LTheme fails with "does not name a type".
extern "C" void *lv_pool_alloc(size_t size) {
    if (!size) size = 1;
    void *p = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!p) p = heap_caps_malloc(size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!p) p = heap_caps_malloc(size, MALLOC_CAP_8BIT);
    return p;
}

extern "C" void lv_pool_free(void *ptr) {
    if (ptr) heap_caps_free(ptr);
}

extern "C" void *lv_pool_realloc(void *ptr, size_t size) {
    if (!size) size = 1;
    void *p = heap_caps_realloc(ptr, size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!p) p = heap_caps_realloc(ptr, size, MALLOC_CAP_8BIT);
    return p;
}

// Internal RAM is what the AFE/MultiNet allocations come from, and what LVGL
// draws out of.  Printing it either side of ESP_SR.begin() turns a silent
// "voice unavailable" into a number you can act on.
static void ptt_heap(const char *tag) {
    USBSerial.printf("[ptt] heap %-14s internal free %6u  largest %6u  psram free %7u\n",
                     tag,
                     (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                     (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
                     (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
}

// Same probe as the standalone sketch: the label must exist and not be blank,
// otherwise esp_srmodel_init() would fail deep inside AFE bring-up.
static bool ptt_model_ok() {
    const esp_partition_t *p = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, "model");
    if (!p) {
        USBSerial.println("[ptt] no 'model' partition - voice disabled");
        return false;
    }
    uint8_t head[16] = {0};
    if (esp_partition_read(p, 0, head, sizeof(head)) != ESP_OK) return false;
    for (size_t i = 0; i < sizeof(head); i++) {
        if (head[i] != 0xFF) {
            USBSerial.printf("[ptt] model @ 0x%06X (%u KB) ready\n",
                             (unsigned)p->address, (unsigned)(p->size / 1024));
            return true;
        }
    }
    USBSerial.printf("[ptt] model @ 0x%06X is blank - flash srmodels.bin there\n",
                     (unsigned)p->address);
    return false;
}

// ============================================================================
//  Voice model provisioning from the card
//  ----------------------------------------------------------------------------
//  The MultiNet blob is 3.3 MB and the factory slot is 5 MB with ~3.85 MB of
//  launcher already in it, so the model can never be part of the app image.
//  It has to reach 0x910000 some other way, and there are exactly two:
//
//    (a) the Upload rule in the "DualCore 5MB APP / 3.9MB MODEL" menu entry,
//        which writes srmodels.bin in the same esptool call as the app;
//    (b) this function -- srmodels.bin sits on the SD card and a blank model
//        partition is filled on the next boot.
//
//  (b) is what makes "upload the launcher and nothing else" actually true, and
//  it also repairs a watch whose partition was wiped by an "EraseFlash: Enabled"
//  upload without re-flashing from a PC.  Once written it never runs again:
//  ptt_model_ok() sees a non-blank head and returns early.
// ============================================================================
static const char *kModelSrc[] = {
    "/WATCH/srmodels.bin", "/WATCH/SRMODELS.BIN",
    "/WATCH/bin/srmodels.bin", "/WATCH/bin/SRMODELS.BIN",
    "/srmodels.bin", "/bin/srmodels.bin",
};
static uint8_t g_modelChunk[4096];

static bool sd_provision_model() {
    const esp_partition_t *p = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, "model");
    if (!p) {
        USBSerial.println("[model] no 'model' partition in this table - voice needs the DualCore 5MB one");
        return false;
    }
    if (ptt_model_ok()) return true;        // already there (menu entry or an earlier boot)

    const char *src = nullptr;
    size_t len = 0;
    for (size_t i = 0; i < sizeof(kModelSrc) / sizeof(kModelSrc[0]); i++) {
        if (!SD_MMC.exists(kModelSrc[i])) continue;
        File f = SD_MMC.open(kModelSrc[i], FILE_READ);
        if (!f) continue;
        len = (size_t)f.size();
        f.close();
        if (len == 0 || len > (size_t)p->size) {
            USBSerial.printf("[model] %s is %u B, partition holds %u - skipped\n",
                             kModelSrc[i], (unsigned)len, (unsigned)p->size);
            continue;
        }
        src = kModelSrc[i];
        break;
    }
    if (!src) {
        USBSerial.printf("[model] blank at 0x%06X and no srmodels.bin on the card\n",
                         (unsigned)p->address);
        USBSerial.println("[model] copy srmodels.bin to /WATCH/srmodels.bin once, or Upload the 'DualCore 5MB' scheme");
        return false;
    }

    USBSerial.printf("[model] writing %s (%u KB) -> 0x%06X\n",
                     src, (unsigned)(len / 1024), (unsigned)p->address);
    File f = SD_MMC.open(src, FILE_READ);
    if (!f) return false;

    const size_t span = (len + 4095u) & ~(size_t)4095u;   // erase in 4 KB sectors
    if (esp_partition_erase_range(p, 0, span) != ESP_OK) {
        USBSerial.println("[model] erase failed");
        f.close();
        return false;
    }

    size_t done = 0, painted = 0;
    bool ok = true;
    while (done < len) {
        size_t want = len - done;
        if (want > sizeof(g_modelChunk)) want = sizeof(g_modelChunk);
        int got = f.read(g_modelChunk, want);
        if (got <= 0) { ok = false; break; }
        if (esp_partition_write(p, done, g_modelChunk, (size_t)got) != ESP_OK) { ok = false; break; }
        done += (size_t)got;
        if (done - painted >= 262144u) {          // repaint the splash every 256 KB
            painted = done;
            char msg[32];
            snprintf(msg, sizeof(msg), "voice model %u%%", (unsigned)(done * 100 / len));
            splash_progress(74 + (int)(done * 22 / len), msg);
        }
    }
    f.close();
    if (!ok || done != len) {
        USBSerial.printf("[model] write failed at %u/%u B - partition left blank\n",
                         (unsigned)done, (unsigned)len);
        return false;
    }
    USBSerial.printf("[model] %u KB written, verifying\n", (unsigned)(len / 1024));
    return ptt_model_ok();      // re-reads the head: a silent failure is caught here
}

static void ptt_box_cb(lv_event_t *) {      // tap the overlay: dismiss it now
    if (g_pttBox) lv_obj_add_flag(g_pttBox, LV_OBJ_FLAG_HIDDEN);
    g_pttHideAt = 0;
}

static void ptt_flash(const String &msg, uint32_t holdMs) {
    // Voice feedback is pointless on a dark panel.  Every caller runs in loop
    // context, and this is the single choke point for "Opening mic...",
    // "Yes?", "HEARD: ..." and the command results, so waking the screen here
    // covers a BOOT press, a wake word, and a session that was already open.
    if (screenIsOff) screen_on();
    log_msg(msg);                       // v2.7: survives the pill
    if (!g_pttBox) {
        g_pttBox = lv_label_create(lv_layer_top());
        lv_obj_set_style_bg_color(g_pttBox, lv_color_hex(0x000000), 0);
        lv_obj_set_style_bg_opa(g_pttBox, 220, 0);
        lv_obj_set_style_radius(g_pttBox, 16, 0);
        lv_obj_set_style_pad_hor(g_pttBox, 14, 0);
        lv_obj_set_style_pad_ver(g_pttBox, 8, 0);
        lv_obj_set_style_border_color(g_pttBox, lv_color_hex(theme_primary()), 0);
        lv_obj_set_style_border_width(g_pttBox, 1, 0);
        lv_obj_set_style_text_color(g_pttBox, lv_color_hex(0xFFFFFF), 0);
        lv_obj_set_style_text_font(g_pttBox, &lv_font_montserrat_16, 0);
        lv_obj_clear_flag(g_pttBox, LV_OBJ_FLAG_SCROLLABLE);
        // v1.8: wrap instead of clipping, and let a tap dismiss the pill, so a
        // long answer does not have to be waited out or read in one clipped line.
        lv_label_set_long_mode(g_pttBox, LV_LABEL_LONG_WRAP);
        lv_obj_set_style_max_width(g_pttBox, LCD_WIDTH - 56, 0);
        lv_obj_set_style_text_align(g_pttBox, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_add_flag(g_pttBox, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(g_pttBox, ptt_box_cb, LV_EVENT_CLICKED, NULL);
    }
    // v1.8: a fixed 1.6 s was far too short for the long answers ("system
    // status" is ~170 characters), so the hold now scales with the text.
    if (!holdMs) {
        holdMs = 1100 + (uint32_t)msg.length() * 32;
        if (holdMs > 5200) holdMs = 5200;
    }
    lv_label_set_text(g_pttBox, msg.c_str());
    lv_obj_clear_flag(g_pttBox, LV_OBJ_FLAG_HIDDEN);
    // v1.7: inside Xander the top band is the header, so the overlay drops one
    // band lower there instead of painting over the title.
    lv_obj_align(g_pttBox, LV_ALIGN_TOP_MID, 0, g_xander_active ? 56 : 14);
    lv_obj_move_foreground(g_pttBox);
    lv_timer_handler();                 // loop context: safe to paint now
    g_pttHideAt = millis() + holdMs;
}

// AFE + MultiNet are allocated from internal RAM inside begin().  Below this
// largest-block figure create_from_config() starts failing, so refuse cleanly
// instead of risking an assert deep inside ESP-SR.
#define PTT_MIN_LARGEST_BLOCK 40000

static const char *g_pttErr = nullptr;
// Backing store for the one message that carries a measurement (the RAM
// refusal).  The screen shows "need 40K, have 18K" instead of a bare
// "Not enough RAM", which is the difference between a guess and a diagnosis.
static char        g_pttErrBuf[48];

// AFE + MultiNet want real time; at the 80 MHz idle clock the feed task can
// fall behind the microphone.  Every exit path from the bring-up restores the
// idle clock, so a failed start cannot leave the watch burning 240 MHz.
#if POWER_SAVE
static inline void ptt_cpu(bool fast) {
    // v1.6: the boost is still local to the voice path, but the restore now
    // asks the power manager for the CURRENT state's clock instead of forcing
    // the old fixed idle value -- otherwise leaving a phrase would pin the
    // watch at 80 MHz until the next state change.
    if (fast) { g_pmMicBusy = true;  setCpuFrequencyMhz(CPU_FREQ_BUSY_MHZ); }
    else      { g_pmMicBusy = false; pm_restore_cpu(); }
}
#else
static inline void ptt_cpu(bool) {}
#endif

// ----------------------------------------------------------------------------
//  Bring-up: bus + codec in loop context, sr_start() on a throwaway task.
//  ----------------------------------------------------------------------------
//  sr_start() in esp32-hal-sr.c has an error path that runs
//  `vTaskDelete(g_sr_data->handle_task)`.  With a NULL handle -- one of the
//  three ESP-SR tasks failed to allocate -- that is vTaskDelete(NULL), which
//  deletes THE TASK THAT CALLED IT.  Called from loop() that task is the
//  Arduino loop task, and the symptom is exactly the report: the last painted
//  frame stays on the panel ("Opening mic..." forever), no panic, no log line,
//  nothing answers touch or BOOT afterwards.
//
//  So only sr_start() moves to a throwaway task.  I2S and the ES7210 stay in
//  loop context: they are quick, and Wire must not be shared with a second task
//  while the panel is being serviced.
// ----------------------------------------------------------------------------
static volatile uint8_t  g_pttBring     = 0;   // 0 idle, 1 starting, 2 up, 3 failed
static volatile uint32_t g_pttBringMs   = 0;
static bool              g_pttBringAuto = false;
#define PTT_BRING_TIMEOUT_MS 8000UL

static bool ptt_open_mic() {
    if (!g_pttReady)  { g_pttErr = "Voice not ready";   return false; }
    // Keep a specific reason set at boot ("No voice model") -- it tells the
    // user what to flash, whereas "Voice unavailable" tells them nothing.
    if (!g_pttUsable) { if (!g_pttErr) g_pttErr = "Voice unavailable"; return false; }

    const uint32_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
    if (largest < PTT_MIN_LARGEST_BLOCK) {
        USBSerial.printf("[ptt] refusing to start: largest internal block %u < %u\n",
                         (unsigned)largest, (unsigned)PTT_MIN_LARGEST_BLOCK);
        ptt_heap("short on ram");
        snprintf(g_pttErrBuf, sizeof g_pttErrBuf, "Need %uK RAM, have %uK",
                 (unsigned)(PTT_MIN_LARGEST_BLOCK / 1024), (unsigned)(largest / 1024));
        g_pttErr = g_pttErrBuf;
        // Deliberately NOT latched to g_pttUsable: the shortfall is a snapshot
        // of internal SRAM at this instant (a screen teardown still in flight, an
        // I2S buffer held by the wake-word task).  The next press re-measures and
        // may pass, so keep voice retryable instead of dead until reboot.
        return false;
    }

    ptt_cpu(true);

    g_pttHit         = 0;
    g_pttShownHit    = -1;
    g_pttHitMs       = 0;
    g_pttMnExpiredAt = 0;
    g_pttMnRearms    = 0;
    ptt_heap("before begin");

    g_pttI2S.setPins(PTT_I2S_BCLK_PIN, PTT_I2S_WS_PIN, -1,
                     PTT_I2S_DIN_PIN, PTT_I2S_MCLK_PIN);
    g_pttI2S.setTimeout(2000);
    if (!g_pttI2S.begin(I2S_MODE_STD, 16000, I2S_DATA_BIT_WIDTH_16BIT,
                        I2S_SLOT_MODE_MONO, I2S_STD_SLOT_LEFT)) {
        USBSerial.println("[ptt] I2S start failed - BCLK 41, WS 45, DIN 42, MCLK 16");
        g_pttErr = "Mic did not start";
        ptt_cpu(false);
        return false;
    }
    USBSerial.println("[ptt] I2S open: 16 kHz / 16-bit / mono-left, MCLK 4.096 MHz");

    // The ES7210 stays silent until it is told to start, and it only accepts
    // that once the clocks above are running.  Without this the session would
    // open, listen to a dead bus and time out -- the old "it hears nothing".
    if (!es7210_start()) {
        g_pttI2S.end();
        g_pttErr = "Mic codec failed";
        ptt_cpu(false);
        return false;
    }

    ESP_SR.onEvent(ptt_on_event);        // the callback is set once per boot
    return true;
}

// The one call that can delete its caller.  Two attempts: a failed create can
// leave the heap fragmented enough that the second succeeds, and sr_start()
// cleans up after itself on a clean error return.
static void ptt_sr_task(void *) {
    bool ok = false;
    for (int attempt = 1; attempt <= 2 && !ok; attempt++) {
        ok = ESP_SR.begin(g_pttI2S, g_pttCmds, PTT_N, SR_CHANNELS_MONO,
                          SR_MODE_COMMAND, "M");
        if (!ok) delay(60);
    }
    g_pttBring = ok ? 2 : 3;
    vTaskDelete(NULL);
}

// 0 = still starting, 1 = listening, -1 = failed.  Never blocks past the
// I2S/ES7210 register writes, so loop() keeps painting and stays responsive
// even if sr_start() wedges or dies on the way in.
static int ptt_start_poll(bool autoMode) {
    if (g_pttListening) return 1;

    if (g_pttBring == 1) {
        if (millis() - g_pttBringMs <= PTT_BRING_TIMEOUT_MS) return 0;
        // The task never reported back: it is wedged inside ESP-SR, or the
        // library's error path deleted it.  Latch voice off for this boot --
        // retrying a half-built engine just wedges again -- and leave the bus
        // alone, because a live feed task may still own it.
        g_pttBring  = 4;
        g_pttUsable = false;
        g_pttErr    = "Voice hung - reboot";
        USBSerial.println("[ptt] sr start never returned - voice latched off");
    }

    if (g_pttBring == 4) {
        // Hung, not failed: no I2S.end() and no clock change here.  A feed task
        // that survived inside ESP-SR may still be reading the bus, and closing
        // it underneath would be the freeze this whole split exists to avoid.
        g_pttBring = 0;
        return -1;
    }

    if (g_pttBring == 2) {
        g_pttBring = 0;
        ptt_heap("after begin");
        g_pttAuto      = g_pttBringAuto;
        g_pttListening = true;
        g_pttStartMs   = millis();
        USBSerial.printf("[ptt] listening... (%d phrases, %lus window)\n",
                         PTT_N, (unsigned long)(PTT_MAX_SESSION_MS / 1000));
        return 1;
    }

    if (g_pttBring == 3) {
        g_pttBring = 0;
        // A clean failure means sr_stop() already ran inside ESP-SR, so the
        // bus is ours again: close it and drop back to the idle clock.
        g_pttI2S.end();
        ptt_cpu(false);
        USBSerial.println("[ptt] ESP_SR.begin failed - is srmodels.bin at 0x910000?");
        if (++g_pttFails >= 3) {
            g_pttUsable = false;
            USBSerial.println("[ptt] disabled for this boot after 3 failures");
        }
        g_pttErr = "Voice unavailable";
        return -1;
    }

    // Idle: open the bus, wake the codec, then hand sr_start() to its task.
    if (!g_pttUsable) { if (!g_pttErr) g_pttErr = "Voice unavailable"; return -1; }
    if (!ptt_open_mic()) return -1;

    g_pttErr       = nullptr;
    g_pttBringAuto = autoMode;
    g_pttBringMs   = millis();
    g_pttBring     = 1;
    if (xTaskCreatePinnedToCore(ptt_sr_task, "ptt sr", 4096, nullptr, 4, nullptr, 0) != pdPASS) {
        g_pttBring = 3;
        return ptt_start_poll(autoMode);   // close the bus, report, once
    }
    return 0;
}

// ESP_SR's tasks must stop before the bus does; this is the only teardown path.
// v2.13d: the ES7210 has no shutdown of its own.  es7210_start() leaves the
// analog block (0x40 = 0x43), the 2.87 V MIC bias (0x41/0x42 = 0x70) and both
// PGAs (0x43/0x44) on, and nothing ever wrote them back -- so after the first
// push-to-talk the mic front end stayed biased for the rest of the day.  Start
// does a full reset + re-init, so dropping these here costs nothing on the next
// press.
static void es7210_sleep() {
    if (!g_micAddr) return;
    es7210_write(0x01, 0x3f);            // all ADC clocks off
    es7210_update(0x43, 0x10, 0x00);     // MIC1 PGA off
    es7210_update(0x44, 0x10, 0x00);     // MIC2 PGA off
    es7210_write(0x41, 0x00);            // MIC bias off
    es7210_write(0x42, 0x00);
    es7210_write(0x40, 0x00);            // analog block off
    USBSerial.println("[mic] ES7210 powered down");
}
static int ptt_stop() {
    if (!g_pttListening) return 0;
    ESP_SR.end();
    g_pttI2S.end();
    es7210_sleep();
    g_pttListening   = false;
    g_pttAuto        = false;
    g_pttHitMs       = 0;
    g_pttMnExpiredAt = 0;
    g_aonWakeMs      = 0;   // the gate never outlives the session that opened it
    ptt_cpu(false);
    int id = g_pttHit;
    g_pttHit = 0;
    g_pttLastMs = millis() - g_pttStartMs;
    g_pttLastId = id;
    if (id > 0 && id <= PTT_N)
        USBSerial.printf("[ptt] session ended after %lums, id=%d (\"%s\")\n",
                         (unsigned long)g_pttLastMs, id, kPtt[id - 1].phrase);
    else
        USBSerial.printf("[ptt] session ended after %lums, nothing matched\n",
                         (unsigned long)g_pttLastMs);
    return id;
}

// Settings > BOOT to Talk turned off while a button window is open.  Always-on
// sessions are left alone: they are opened by the wake word, not the button.
static void ptt_boot_off() {
    if (g_pttListening && !g_pttAuto) ptt_stop();
    USBSerial.println("[ptt] BOOT button disabled by settings");
}

// Holds the panel on for as long as the command runs: wakes it if it was dark,
// pauses the idle countdown for the duration, and restarts the countdown when
// the command returns.  Commands that reboot (an SD app launch) never reach the
// destructor, which is exactly right -- the panel should stay lit until the
// restart.  The cap only bounds a wedged command.
#define COMMAND_SCREEN_HOLD_MS 300000UL         // 5 min
struct CommandScreenHold {
    CommandScreenHold() {
        if (screenIsOff) screen_on();
        lastTouchMs = millis();
        screen_hold(COMMAND_SCREEN_HOLD_MS);
    }
    ~CommandScreenHold() { screen_hold_release(); }
};

// v2.13f: the ack is a bank, not one word.  Rotating rather than random is
// deliberate -- it guarantees "Yes?" cannot come up twice running, which a
// fixed phrase already did, while the millis() term keeps the order from being
// predictable.  Same 1.2 s flash, same place, so nothing downstream changed.
static const char *kAck[] = {
    "Yes?", "Listening", "Go ahead", "Your move", "Make it fun",
    "New game?", "Go on then", "I'm here"
};
static void aon_ack() {
    static uint8_t n = 0;
    const uint8_t cnt = (uint8_t)(sizeof(kAck) / sizeof(kAck[0]));
    n = (uint8_t)((n + 1 + (uint8_t)(millis() >> 6)) % cnt);
    ptt_flash(kAck[n], 1200);
    USBSerial.printf("[aon] ack: %s\n", kAck[n]);
}
static void ptt_run(int id) {
    if (id <= 0) { ptt_flash("No command heard"); return; }
    if (id > PTT_N) return;
    const PttCmd &c = kPtt[id - 1];
    CommandScreenHold screenHold;       // lit + paused for the whole command

    // The gate itself is not a command: it only arms the window in which a
    // hands-free phrase is allowed to run.
    if (c.kind == PTT_WAKE) {
        g_aonWakeMs   = millis();
        g_pttMnRearms = 0;
        USBSerial.println("[aon] wake accepted");
        aon_ack();
        // v2.13e: hearing the wake word inside a hands-free session is a request
        // to LISTEN, not a timer refresh.  ESP_SR goes to SR_MODE_OFF the instant
        // it detects, so without this the ack played while nothing was
        // capturing: the window looked open for 7 s and heard nothing.
        if (g_pttAuto && g_pttListening) ESP_SR.setMode(SR_MODE_COMMAND);
        return;
    }

    // Puzzles and Knowledge live INSIDE the Xander layer, so voice has to open
    // that layer first when the normal launcher screens are showing.
    // v2.15: three of the four doors, not two.  Stories could only be opened by
    // tapping it, which is the one thing every other page does not require.
    if (!g_xander_active && (c.arg == XI_OPEN_PUZZLES || c.arg == XI_OPEN_KNOWLEDGE ||
                             c.arg == XI_OPEN_STORIES)) {
        xander_enter();
        if      (c.arg == XI_OPEN_PUZZLES)   { g_x_top = 0; g_x_puz_view = 0; }
        else if (c.arg == XI_OPEN_KNOWLEDGE) { g_x_top = 2; g_x_kn_view = 0; g_x_kn_cat = ""; }
        else                                 { g_x_top = 3; g_x_st_view = 0; }
        ptt_flash(c.arg == XI_OPEN_PUZZLES   ? "Puzzles"
                : c.arg == XI_OPEN_KNOWLEDGE ? "Knowledge" : "Stories", 900);
        return;
    }
    if (!g_xander_active && c.arg == XI_HOME_EXIT) {
        ptt_flash("Already home", 900);
        return;
    }

    if (c.kind == PTT_APP) {
        // Same lookup the app grid uses: /WATCH/bin on the card is the truth,
        // whatever spelling the file arrived with (see app_resolve).
        int idx = app_resolve(c.bin);
        if (idx < 0) {
            USBSerial.printf("[ptt] %s not found under %s\n", c.bin, apps_dir().c_str());
            ptt_flash(String(c.bin) + " is not in /WATCH/bin");
            return;
        }
        USBSerial.printf("[ptt] launch %s <- %s\n", c.bin, g_apps[idx].path.c_str());
        ptt_flash(String("Opening ") + c.bin, 900);
        load_app_from_sd(g_apps[idx]);   // OTA-copies + reboots; does not return
        return;
    }

    USBSerial.printf("[ptt] intent %d (%s)\n", (int)c.arg, c.phrase);
    ptt_flash(String("\"") + c.phrase + "\"", 900);
    x_do((int)c.arg);                   // some of these block (sync) or reboot
    if (g_x_last.length()) ptt_flash(g_x_last);
}

// Called every loop(); owns the whole toggle lifecycle.
static void ptt_service() {
    const uint32_t now  = millis();
    const bool     down = (digitalRead(PTT_PIN) == LOW);

    // Engine bring-up in flight (ptt_start_poll).  Report the outcome the
    // moment the task says so; nothing here blocks, so the panel keeps painting
    // and a failed start is a message on the overlay instead of a frozen frame.
    // Polled before the toggle check: a session must always be finalised, even
    // if the button is switched off while the engine is still coming up.
    if (g_pttBring) {
        const int st = ptt_start_poll(g_pttBringAuto);
        if (st == 1)     ptt_flash("LISTENING - speak now", 60000);
        else if (st < 0) ptt_flash(g_pttErr ? g_pttErr : "Voice unavailable", 2200);
        if (g_pttBring) return;                 // still coming up
    }

    // Settings > BOOT to Talk off: the button is not ours, so no session, no
    // "Opening mic..." overlay, nothing.  The press is swallowed (and re-armed
    // on release) so flipping the toggle back on cannot inherit a stale press.
    if (!g_x_bootmic) {
        if (g_pttListening && !g_pttAuto) ptt_stop();
        g_pttArmed = down;
        if (g_pttHideAt && now >= g_pttHideAt) {
            g_pttHideAt = 0;
            if (g_pttBox) lv_obj_add_flag(g_pttBox, LV_OBJ_FLAG_HIDDEN);
        }
        return;
    }

    // MultiNet's window closed (esp32-hal-sr.c: 5760 ms, then SR_MODE_OFF).
    // For a button press that is the end of the session: whatever was matched
    // inside the window runs now, and a press that heard nothing just ends.
    // Re-arming is gone on purpose -- a second window would only listen to a
    // room the speaker already stopped talking into.
    if (g_pttListening && g_pttMnExpiredAt) {
        if (now - g_pttMnExpiredAt < 30) return;      // let the SR task settle
        g_pttMnExpiredAt = 0;
        if (!g_pttAuto) {
            USBSerial.printf("[ptt] window closed after %lums\n",
                             (unsigned long)(now - g_pttStartMs));
            ptt_run(ptt_stop());
            g_pttArmed = down;
            return;
        }
        // Always-on: MultiNet's own window is 5.76 s, so a 7 s conversation
        // window needs one renewal to stay audible to the end.  The cap of 3 is
        // only there so a stuck setMode loop cannot hold the mic open forever;
        // aon_service() still closes the session when the window expires.
        if ((int32_t)(millis() - g_aonWakeMs) < (int32_t)AON_GATE_MS &&
            ++g_pttMnRearms <= 3) {
            ESP_SR.setMode(SR_MODE_COMMAND);
            USBSerial.printf("[aon] command window renewed (%d)\n", (int)g_pttMnRearms);
        }
    }

    // Backstop: the engine never reached a verdict (dead mic, stalled feed), so
    // nothing would ever close the mic.  End the session and run the match.
    if (g_pttListening && !g_pttAuto && (now - g_pttStartMs > PTT_MAX_SESSION_MS)) {
        ptt_run(ptt_stop());
        g_pttArmed = down;
        return;
    }

    if (g_pttListening) {
        // No screen-off mid-session -- including an always-on window, which can
        // sit open for the 7 s gate after being triggered on a dark screen.
        lastTouchMs = now;
    }
    if (g_pttListening && !g_pttAuto) {
        // A match ends the listening itself: ESP_SR puts the engine in
        // SR_MODE_OFF the instant it detects one (esp32-hal-sr.c,
        // ESP_MN_STATE_DETECTED), so there is nothing left to listen to.  Paint
        // what was heard for a beat -- long enough to read it, and to see a
        // mis-match -- then run it.  No second press, no waiting out the window.
        if (g_pttHit > 0) {
            if (!g_pttHitMs) {
                g_pttHitMs    = now;
                g_pttShownHit = g_pttHit;
                String heard = String("HEARD: \"") + kPtt[g_pttHit - 1].phrase + "\"";
                ptt_flash(heard, 60000);
                USBSerial.printf("[ptt] matched \"%s\"\n", kPtt[g_pttHit - 1].phrase);
            }
            if (now - g_pttHitMs >= PTT_RUN_DELAY_MS) {
                ptt_run(ptt_stop());
                g_pttArmed = down;
                return;
            }
        }
    }

    if (down) {
        if (g_pttArmed) return;                 // this press is already handled
        // BOOT is the voice button, so a press from a dark screen serves both
        // halves of the request: light the panel, then open the window.  This
        // press used to be handed to the wake path alone, which is how a
        // session could run with its overlay invisible on a black screen.
        if (screenIsOff) screen_on();
        // v2.13f: this used to be "delay(PTT_DEBOUNCE_MS); if still HIGH, it was
        // bounce", so a tap that ended before the debounce was thrown away
        // entirely -- a quick press did nothing at all.  What a real contact
        // bounce cannot survive is 12 ms of continuous LOW; a human tap easily
        // does.  Same blocking cost as the old delay.
        {
            const uint32_t lowAt   = millis();
            const uint32_t maxWait = (PTT_DEBOUNCE_MS > 20) ? PTT_DEBOUNCE_MS : 20;
            bool tap = false;
            while (millis() - lowAt < maxWait) {
                if (digitalRead(PTT_PIN) != LOW) break;       // released early
                if (millis() - lowAt >= 12) { tap = true; break; }
                delay(2);
            }
            if (!tap) return;
        }
        g_pttArmed = true;

        // Hands-free session: a press is "listen now", not "stop".  It opens
        // the same gate the wake phrase opens, so BOOT stays useful when the
        // wake word is missed without tearing the engine down.
        if (g_pttListening && g_pttAuto) {
            g_aonWakeMs = millis();
            ptt_flash("Yes?", 1200);
            return;
        }

        if (g_pttListening) {
            // v2.13f: a press during a window that has heard nothing used to END
            // the window and flash "No command heard" -- which is exactly what a
            // second tap looks like when the first one seemed to do nothing, and
            // is why it felt like the button needed three tries.  With a match
            // waiting the press still runs it; with nothing heard it simply
            // restarts the window, because a press always means "listen".
            if (g_pttHit > 0) {
                ptt_run(ptt_stop());
            } else {
                g_pttHitMs   = 0;
                g_pttStartMs = now;             // the MultiNet window starts again
                ESP_SR.setMode(SR_MODE_COMMAND);
                ptt_flash("Listening...", 900);
                USBSerial.println("[ptt] window restarted by a press");
            }
        } else {
            // The detector may be holding the mic; release it before ESP_SR
            // opens its own, or I2S comes up already claimed.
            aon_wake_disarm(true);
            // Paint before the bring-up so the press feels acknowledged
            // immediately; the rest of it lands on later loop passes, which is
            // what keeps the panel alive if sr_start() misbehaves.
            ptt_flash("Opening mic...", 1200);
            const int st = ptt_start_poll(false);
            if (st == 1)     ptt_flash("LISTENING - speak now", 60000);
            else if (st < 0) ptt_flash(g_pttErr ? g_pttErr : "Voice unavailable", 2200);
            // st == 0: the poll at the top of ptt_service finishes the job.
        }
        return;
    }

    // ---- released: re-arm for the next press, retire the overlay ----------
    g_pttArmed = false;
    if (g_pttHideAt && now >= g_pttHideAt) {
        g_pttHideAt = 0;
        if (g_pttBox) lv_obj_add_flag(g_pttBox, LV_OBJ_FLAG_HIDDEN);
    }
}

// Resolved in ptt_init() (before the detector task exists) so a model without a
// "Xander" class can be reported at boot instead of failing silently later.
static int g_wakeLabel = -1;

static void ptt_init() {
    pinMode(PTT_PIN, INPUT_PULLUP);
    for (int i = 0; i < PTT_N; i++) {
        g_pttCmds[i].command_id = i + 1;
        snprintf(g_pttCmds[i].str, sizeof(g_pttCmds[i].str), "%s", kPtt[i].phrase);
        USBSerial.printf("[ptt]  %2d  %s\n", i + 1, kPtt[i].phrase);
    }
    // Resolve the EI class by NAME: a re-export can reorder the 10 classes and
    // index 5 would then be a different word.
    g_wakeLabel = -1;
    for (int i = 0; i < EI_CLASSIFIER_LABEL_COUNT; i++) {
        if (strcmp(ei_classifier_inferencing_categories[i], "Xander") == 0) {
            g_wakeLabel = i;
            break;
        }
    }
    USBSerial.printf("[aon] EI detector: class 'Xander' %s (index %d of %d)\n",
                     g_wakeLabel >= 0 ? "found" : "MISSING", g_wakeLabel,
                     EI_CLASSIFIER_LABEL_COUNT);

    g_pttReady  = true;
    g_pttUsable = ptt_model_ok();
    if (!g_pttUsable) g_pttErr = "No voice model";
    if (g_pttUsable)
        USBSerial.printf("[ptt] press BOOT to talk (%d phrases) - button %s\n",
                         PTT_N, g_x_bootmic ? "enabled" : "DISABLED in settings");
    USBSerial.printf("[aon] always-on %s\n",
                     !g_x_alwayson ? "off" : (g_pttUsable ? "armed" : "requested but unavailable"));
}

// ============================================================================
//  ALWAYS-ON  (settings > Always-On Xander)
// ============================================================================
// Two stages, both inside this sketch:
//
//   STAGE 1  the Edge Impulse "WATCH" model scores 250 ms slices and opens the
//            gate when class "Xander" >= AON_WAKE_THRESHOLD on two consecutive
//            slices.  This is the part MultiNet cannot do: "Xander" is out of
//            vocabulary, so letter-to-sound rules mangle it.
//   STAGE 2  ESP_SR MultiNet hears the command inside AON_GATE_MS and runs it
//            exactly as a button session would, then hands the mic back.
//
// Why a task: one detector slice costs ~250 ms of capture plus inference, and
// spending that inside loop() would stall LVGL for a third of a second at a
// time -- swipes and card taps would stutter.  Stage 1 therefore runs on core 0
// while the UI keeps core 1.  Only one owner has the I2S bus at a time: the
// task closes it and parks before ESP_SR (or a button session) opens its own.
//
// Cost, stated plainly: while armed the mic and the detector stay powered, so
// the battery falls noticeably faster than push-to-talk.  The Battery Limit
// dropdown stops it, and stopping releases I2S completely.

#define AON_WAKE_THRESHOLD 0.70f   // class confidence for "Xander"
#define AON_WAKE_CONFIRM   2       // consecutive slices above it

static I2SClass         g_wakeI2S;
static TaskHandle_t     g_wakeTask    = nullptr;
static volatile bool    g_wakeWanted  = false;  // main asks the task to listen
static volatile bool    g_wakeBusOpen = false;  // the task holds the I2S bus
static volatile bool    g_wakeParked  = true;   // bus released, task idle
static volatile bool    g_wakeHit     = false;  // "Xander" confirmed
static volatile uint8_t g_wakeErr     = 0;      // 1 = I2S, 2 = inference, 3 = mic codec
// v1.3-voice RAM fix: this used to be `static int16_t g_wakeSlice[N]` -- 8 KB
// (EI_CLASSIFIER_SLICE_SIZE samples) of internal DRAM reserved for the whole
// boot whether or not Always-On was switched on.  Voice (AFE + MultiNet) needs
// one big contiguous internal block, so those 8 KB are worth more as heap.
// The task allocates the slice when it opens the mic and frees it when it
// parks, in PSRAM when there is any.
static int16_t         *g_wakeSlice  = nullptr;

// The SDK's signal adapter: offset/length are int16 samples from g_wakeSlice.
static int aon_ei_get_data(size_t offset, size_t length, float *out_ptr) {
    return numpy::int16_to_float(g_wakeSlice + offset, out_ptr, length);
}

// Fill one 250 ms slice.  Runs on the wake task only; the stall timeout keeps a
// dead I2S from wedging the detector forever.
static bool aon_capture_slice(size_t samples) {
    if (!g_wakeSlice) return false;
    size_t got = 0;
    uint32_t last = millis();
    while (got < samples) {
        if (!g_wakeWanted) return false;            // asked to release
        size_t want = samples - got;
        if (want > 512) want = 512;
        const size_t bytes = g_wakeI2S.readBytes((char *)(g_wakeSlice + got),
                                                 want * sizeof(int16_t));
        if (bytes == 0) {
            if (millis() - last > 400) return false;
            vTaskDelay(pdMS_TO_TICKS(2));
            continue;
        }
        got += bytes / sizeof(int16_t);
        last = millis();
    }
    return true;
}

static void aon_wake_task(void *) {
    int  hits   = 0;
    bool inited = false;

    for (;;) {
        if (!g_wakeWanted) {
            if (g_wakeBusOpen) {
                g_wakeI2S.end();                    // hand the bus back
                g_wakeBusOpen = false;
                inited = false;
            }
            if (g_wakeSlice) {                      // parked == slice buffer released
                heap_caps_free(g_wakeSlice);
                g_wakeSlice = nullptr;
            }
            g_wakeParked = true;
            ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
            continue;
        }

        if (!g_wakeSlice) {
            g_wakeSlice = (int16_t *)heap_caps_malloc(
                EI_CLASSIFIER_SLICE_SIZE * sizeof(int16_t),
                MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            if (!g_wakeSlice)
                g_wakeSlice = (int16_t *)heap_caps_malloc(
                    EI_CLASSIFIER_SLICE_SIZE * sizeof(int16_t),
                    MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
            if (!g_wakeSlice) {
                USBSerial.println("[aon] cannot allocate the detector slice "
                                  "- always-on stays off");
                g_wakeErr    = 2;
                g_wakeWanted = false;
                vTaskDelay(pdMS_TO_TICKS(200));
                continue;
            }
        }

        if (!g_wakeBusOpen) {
            g_wakeI2S.setPins(PTT_I2S_BCLK_PIN, PTT_I2S_WS_PIN, -1,
                              PTT_I2S_DIN_PIN, PTT_I2S_MCLK_PIN);
            g_wakeI2S.setTimeout(500);
            if (!g_wakeI2S.begin(I2S_MODE_STD, 16000, I2S_DATA_BIT_WIDTH_16BIT,
                                 I2S_SLOT_MODE_MONO, I2S_STD_SLOT_LEFT)) {
                g_wakeErr = 1;
                vTaskDelay(pdMS_TO_TICKS(1000));
                continue;
            }
            if (!es7210_start()) {          // clocks are up, wake the ADC
                g_wakeErr = 3;
                g_wakeI2S.end();
                vTaskDelay(pdMS_TO_TICKS(1000));
                continue;
            }
            g_wakeBusOpen = true;
            g_wakeParked  = false;
        }
        if (!inited) {
            run_classifier_init();              // fresh window after a handover
            hits   = 0;
            inited = true;
        }

        if (!aon_capture_slice(EI_CLASSIFIER_SLICE_SIZE)) {
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }

        signal_t signal     = {};
        signal.total_length = EI_CLASSIFIER_SLICE_SIZE;
        signal.get_data     = &aon_ei_get_data;

        ei_impulse_result_t result = {};
        if (run_classifier_continuous(&signal, &result, false, false) != EI_IMPULSE_OK) {
            g_wakeErr = 2;
            run_classifier_init();
            hits = 0;
            continue;
        }
        if (g_wakeLabel < 0) continue;

        const float score = result.classification[g_wakeLabel].value;
        hits = (score >= AON_WAKE_THRESHOLD) ? hits + 1 : 0;
        if (hits >= AON_WAKE_CONFIRM) {
            g_wakeHit    = true;
            g_wakeWanted = false;   // next pass closes the bus and parks
        }
    }
}

static void aon_wake_arm() {
    if (!g_x_alwayson || !g_pttReady || !g_pttUsable) return;
    if (g_wakeLabel < 0) return;                    // no "Xander" class in model
    if (!g_wakeTask) {
        if (xTaskCreatePinnedToCore(aon_wake_task, "aon_wake", 8192, nullptr,
                                    1, &g_wakeTask, 0) != pdPASS) {
            g_wakeTask = nullptr;
            USBSerial.println("[aon] cannot create the wake task");
            return;
        }
    }
    if (!g_wakeWanted) {
        g_wakeHit    = false;
        g_wakeWanted = true;
        if (g_wakeParked) xTaskNotifyGive(g_wakeTask);
        USBSerial.println("[aon] EI wake armed - say \"Xander\"");
    }
}

// Release the mic for a button session or the mic test.  wait=true spins
// briefly so the caller can open I2S immediately afterwards without racing
// the detector for the peripheral.
static void aon_wake_disarm(bool wait) {
    if (!g_wakeTask) return;
    g_wakeWanted = false;
    g_wakeHit    = false;
    if (g_wakeParked) return;
    xTaskNotifyGive(g_wakeTask);                    // nudge it out of a wait
    if (!wait) return;
    for (int i = 0; i < 250 && !g_wakeParked; i++) delay(2);
}

static int aon_batt_floor() {
    switch (g_x_battlim) {          // dropdown order: 20%, 30%, 40%, Never
        case 0:  return 20;
        case 1:  return 30;
        case 2:  return 40;
        default: return 0;          // Never
    }
}

static bool aon_battery_low() {
    const int floor_pct = aon_batt_floor();
    return floor_pct > 0 && g_batt_pct >= 0 && g_batt_pct <= floor_pct;
}

static void aon_stop(const char *why) {
    if (!g_pttListening || !g_pttAuto) return;
    ptt_stop();                     // discards whatever was half-heard
    USBSerial.printf("[aon] stopped (%s)\n", why);
}

static void aon_restart() {
    if (!g_x_alwayson) {
        aon_stop("toggle off");
        aon_wake_disarm(true);
        return;
    }
    if (!g_pttReady || !g_pttUsable) return;
    if (g_pttListening || g_pttBring) return;   // a session (or its bring-up) is running
    aon_wake_arm();
}

// Called every loop(), right after ptt_service().
static void aon_service() {
    // A listening wake word cannot survive deep sleep, so the power manager
    // refuses to park the SoC while Always-On Xander is asked for.  Set here,
    // above every early return, so the flag always describes this pass.
    g_pmVoiceArmed = g_x_alwayson;
    if (!g_x_alwayson) {
        aon_stop("off");
        aon_wake_disarm(false);
        return;
    }
    if (!g_pttReady || !g_pttUsable) return;    // no model, or disabled this boot

    if (aon_battery_low()) {
        aon_stop("battery limit");
        aon_wake_disarm(false);
        return;
    }

    // A button session -- or its bring-up -- owns the mic; the detector stays
    // parked until it ends.  A bring-up is polled here as well: ptt_service()
    // cannot be relied on (the button may be disabled in settings), and a
    // wake-word session has to be finalised by somebody.
    if (g_pttBring) {
        aon_wake_disarm(false);
        if (ptt_start_poll(g_pttBringAuto) < 0 && g_x_alwayson && g_pttUsable)
            aon_wake_arm();
        return;
    }
    if (g_pttListening && !g_pttAuto) { aon_wake_disarm(false); return; }

    if (g_wakeErr) {
        USBSerial.printf("[aon] detector problem (%s)\n",
                         g_wakeErr == 1 ? "I2S would not open"
                                        : (g_wakeErr == 3 ? "ES7210 mic codec did not start"
                                                          : "inference failed"));
        g_wakeErr = 0;
    }

    if (!g_pttListening) {
        if (g_wakeHit) {
            if (!g_wakeParked) return;          // bus still closing; next loop
            g_wakeHit     = false;
            g_aonWakeMs   = millis();
            g_pttMnRearms = 0;              // a fresh conversation window
            USBSerial.println("[aon] wake accepted (edge impulse)");
            aon_ack();
            const int st = ptt_start_poll(true);
            if (st < 0) {
                USBSerial.printf("[aon] command window failed (%s)\n",
                                 g_pttErr ? g_pttErr : "unknown");
                aon_wake_arm();                 // straight back to listening
            }
            return;
        }
        aon_wake_arm();
        return;
    }

    if (!g_pttAuto) return;                     // a button session is in charge

    const int hit = g_pttHit;
    if (hit > 0 && hit <= PTT_N) {
        g_pttHit      = 0;
        g_pttShownHit = -1;

        // MultiNet may also hear "xander"; that only refreshes the gate.
        if (kPtt[hit - 1].kind == PTT_WAKE) { ptt_run(hit); return; }

        const int id = ptt_stop();              // closes the window, frees I2S
        ptt_run(id);                            // may reboot (app) or block (sync)
        // v2.13e: no automatic follow-up window.  A window opens when "Xander"
        // is said and at no other time, so the mic is handed straight back to
        // the detector after a command.  Saying the name again is cheap now that
        // a live session re-arms MultiNet (see ptt_run, PTT_WAKE), which is the
        // part that was actually broken.
        if (g_x_alwayson && g_pttUsable) aon_wake_arm();
        return;
    }

    // Nothing heard: close the window and hand the mic back to the detector
    // instead of leaving MultiNet listening to the room.
    if (!g_aonWakeMs || millis() - g_aonWakeMs > AON_GATE_MS) {
        aon_stop("gate expired");
        aon_wake_arm();
    }
}

// One-second level check, reachable from Xander > settings > Test Microphone.
// Opens the exact I2S path ESP_SR uses, so a healthy number here means the
// wiring is right and any remaining problem is the phrase model, not the mic.
//   -20..-12 dBFS  speaking near the watch
//   -60..-40 dBFS  quiet room with a working mic
//   -90 or "no data"  nothing arriving (wiring / power / slot select)
static void ptt_mictest() {
    if (g_pttListening || g_pttBring) { ptt_flash("Already listening", 1200); return; }

    aon_wake_disarm(true);          // the detector must let go of the mic first
    ptt_flash("Mic test: stay quiet", 700);
    g_pttI2S.setPins(PTT_I2S_BCLK_PIN, PTT_I2S_WS_PIN, -1,
                     PTT_I2S_DIN_PIN, PTT_I2S_MCLK_PIN);
    g_pttI2S.setTimeout(500);
    if (!g_pttI2S.begin(I2S_MODE_STD, 16000, I2S_DATA_BIT_WIDTH_16BIT,
                        I2S_SLOT_MODE_MONO, I2S_STD_SLOT_LEFT)) {
        USBSerial.println("[mic] I2S would not open");
        ptt_flash("Mic did not start", 2200);
        return;
    }
    if (!es7210_start()) {
        g_pttI2S.end();
        ptt_flash("No ES7210 mic", 2400);
        return;
    }

    static int16_t buf[512];
    uint32_t n = 0;
    int64_t  sum = 0;
    int32_t  peak = 0;
    double   sq = 0.0;
    const uint32_t t0 = millis();

    while (n < 16000 && (millis() - t0) < 3000) {
        size_t bytes = g_pttI2S.readBytes(reinterpret_cast<char *>(buf), sizeof(buf));
        if (bytes == 0) { delay(2); continue; }
        const size_t got = bytes / sizeof(int16_t);
        for (size_t i = 0; i < got; i++) {
            const int32_t v = buf[i];
            sum += v;
            if (abs(v) > peak) peak = abs(v);
            sq += (double)v * (double)v;
        }
        n += got;
    }
    g_pttI2S.end();

    if (n < 1600) {
        USBSerial.printf("[mic] no data (only %lu samples in %lums)\n",
                         (unsigned long)n, (unsigned long)(millis() - t0));
        ptt_flash("No mic data on DIN 42", 2400);
        return;
    }

    const double dc     = (double)sum / (double)n;
    const double meanSq = sq / (double)n;
    const double acVar  = meanSq - dc * dc;
    const double rms    = sqrt(acVar > 0 ? acVar : 0);
    const double dbfs   = (rms > 0.5) ? 20.0 * log10(rms / 32768.0) : -99.0;

    USBSerial.printf("[mic] %lu samples  dc %.1f  rms %.1f (%.1f dBFS)  peak %ld\n",
                     (unsigned long)n, dc, rms, dbfs, (long)peak);
    if (dbfs <= -90.0)
        USBSerial.println("[mic] silent: ES7210 answered but SDOUT is flat - "
                          "MCLK (16) or DIN (42) not reaching the codec");
    else if (dbfs < -55.0)
        USBSerial.println("[mic] very quiet - usable, but speak close for MultiNet");
    else
        USBSerial.println("[mic] level looks healthy");

    char msg[48];
    snprintf(msg, sizeof(msg), "Mic %.0f dBFS  peak %ld", dbfs, (long)peak);
    ptt_flash(msg, 4000);
}

// ============================================================================
//  v1.7.1  SLOT SWAP  --  "Dual Boot" / "Dual" / "Hero" / "Hero Time"
//  ----------------------------------------------------------------------------
//  Every other launch copies a .bin into the NEXT OTA slot and reboots there.
//  A swap does the opposite: it puts the image into the slot this firmware is
//  executing from.  That cannot be done in place -- erasing the flash you are
//  running out of kills the CPU on the next instruction fetch, and
//  esp_ota_begin() refuses the running partition outright.
//
//  v1.9 does it in ONE stage: DualBoot.bin is written into the slot that is not
//  running, verified, and the bootloader is aimed at it.  Nothing executes from
//  the target slot, so there is no in-place erase problem -- and no second boot
//  for the hand-off to get lost in.  The launcher stays exactly where it is.
//
//  The old two-stage version (clone the launcher into the spare slot, reboot
//  into the clone, only then write DualBoot) is gone.  Its second boot was
//  where installs died silently: the clone came up, its own note looked stale
//  because "lfrom" named the slot it had just left, the note was dropped, and
//  the watch rebooted into a fresh copy of the launcher with DualBoot never
//  written -- exactly the symptom "it copies the launcher but never boots
//  DualBoot".  boot_resolve() is the only thing that finishes or clears a note
//  left by a firmware that still used one.
//
//  Layout assumed (partitions.csv): factory 5 MB + ota_0 4 MB + otadata.  Both
//  directions work -- the target is whichever app slot is not the running one.
// ============================================================================
// v1.7.1: the hand-off note lives in the OTHER firmware's namespace and uses
// its key names ("dualboot" / lswap / lpath / ltry) so both sketches agree and
// either side can finish a swap the other started.  "lfrom" is ours alone: it
// records the slot the note was written from, which the other side ignores.
#define SWAP_NS             "dualboot"
#define SWAP_IMG_PRIMARY    "/WATCH/DualBoot/DualBoot.bin"
#define SWAP_CONFIRM_MS     5000      // cancel window; nothing is erased before it
#define SWAP_MAX_TRIES      2         // stage-2 attempts before giving up
#define SWAP_MIN_PCT        30        // refuse below this on battery alone
#define SWAP_CHIP_ID        0x0009    // ESP32-S3, the chip id at image header +12

static Preferences g_swapNvs;
static bool        g_swapBusy    = false;
static int32_t     g_swapLastSec = -1;

// ---------------------------------------------------------------- the note ---
static void swap_note_set(const char *path, const char *from) {
    g_swapNvs.begin(SWAP_NS, false);
    g_swapNvs.putBool("lswap", true);
    g_swapNvs.putString("lpath", path ? path : "");
    g_swapNvs.putString("lfrom", from ? from : "");
    g_swapNvs.putInt("ltry", 0);
    g_swapNvs.end();
}
static bool swap_note_pending() {
    g_swapNvs.begin(SWAP_NS, true);
    const bool v = g_swapNvs.getBool("lswap", false);
    g_swapNvs.end();
    return v;
}
static String swap_note_path() {
    g_swapNvs.begin(SWAP_NS, true);
    String v = g_swapNvs.getString("lpath", "");
    g_swapNvs.end();
    return v;
}
static String swap_note_from() {
    g_swapNvs.begin(SWAP_NS, true);
    String v = g_swapNvs.getString("lfrom", "");
    g_swapNvs.end();
    return v;
}
static int swap_note_try() {
    g_swapNvs.begin(SWAP_NS, true);
    const int v = g_swapNvs.getInt("ltry", 0);
    g_swapNvs.end();
    return v;
}
static void swap_note_try_set(int n) {
    g_swapNvs.begin(SWAP_NS, false);
    g_swapNvs.putInt("ltry", n);
    g_swapNvs.end();
}
static void swap_note_clear() {
    g_swapNvs.begin(SWAP_NS, false);
    g_swapNvs.remove("lswap");
    g_swapNvs.remove("lpath");
    g_swapNvs.remove("lfrom");
    g_swapNvs.remove("ltry");
    g_swapNvs.end();
}

// -------------------------------------------------------------- the slots ---
// esp_partition_find_first() rather than walking an iterator: the iterator's
// partition field lives inside the iterator and must be released, whereas the
// pointer find_first hands back is the same one esp_ota_* return and keeps.
static const esp_partition_t *swap_other_app(const esp_partition_t *self) {
    const esp_partition_t *f = esp_partition_find_first(
        ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_FACTORY, nullptr);
    if (f && f != self) return f;
    const esp_partition_t *o = esp_partition_find_first(
        ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_0, nullptr);
    if (o && o != self) return o;
    return nullptr;
}

// The documented home is WATCH/DualBoot/; the rest are here because a card
// formatted on a PC loves to upper-case an extension or land the file a folder
// over.
static const char *swap_pick_image() {
    static const char *cand[] = {
        SWAP_IMG_PRIMARY,
        "/WATCH/DualBoot/DualBoot.BIN",
        "/WATCH/bin/DualBoot.bin",
        "/DualBoot.bin",
    };
    for (unsigned i = 0; i < sizeof(cand) / sizeof(cand[0]); i++)
        if (SD_MMC.exists(cand[i])) return cand[i];
    return nullptr;
}

// ---------------------------------------------------------- image checking ---
// A real ESP32 app image starts with 0xE9 and carries an esp_app_desc_t at 0x20.
static bool swap_head_ok(const uint8_t *h, const char **why) {
    if (!h || h[0] != 0xE9) { if (why) *why = "not an ESP image"; return false; }
    const uint16_t chip = (uint16_t)(h[12] | (h[13] << 8));
    if (chip != SWAP_CHIP_ID) { if (why) *why = "built for a different chip"; return false; }
    uint32_t desc = 0;
    memcpy(&desc, h + 0x20, sizeof(desc));
    if (desc != ESP_APP_DESC_MAGIC_WORD) {
        if (why) *why = "merged .bin, use the app-only image";
        return false;
    }
    return true;
}

static bool swap_probe_image(const char *path, size_t *total_out, const char **why) {
    File f = SD_MMC.open(path, FILE_READ);
    if (!f || f.isDirectory()) {
        if (f) f.close();
        if (why) *why = "cannot open file";
        return false;
    }
    const size_t total = f.size();
    uint8_t h[0x24];
    memset(h, 0, sizeof(h));
    if (total < 8192) {
        f.close();
        if (why) *why = "file is too small";
        return false;
    }
    const bool rd = (f.read(h, sizeof(h)) == (int)sizeof(h));
    f.close();
    if (!rd) { if (why) *why = "cannot read file"; return false; }
    if (!swap_head_ok(h, why)) return false;
    if (total_out) *total_out = total;
    return true;
}

// Throttled feedback.  ptt_flash() is the one overlay that exists on every
// screen (and it wakes the panel), so the swap uses it instead of a dialog.
static void swap_flash_progress(size_t done, size_t total, const char *verb) {
    static uint32_t last = 0;
    const uint32_t now = millis();
    if (done < total && (uint32_t)(now - last) < 120) return;
    last = now;
    const int pct = total ? (int)((done * 100) / total) : 0;
    char b[48];
    snprintf(b, sizeof(b), "%s %d%%", verb, pct);
    ptt_flash(String(b), 2500);
    lvgl_pump();
}

// Stream a file into a partition with raw partition calls.  esp_ota_begin() is
// not used on purpose: it refuses the running partition and its bookkeeping
// buys nothing here.
static bool swap_write_image(const char *path, const esp_partition_t *dst, size_t total) {
    File f = SD_MMC.open(path, FILE_READ);
    if (!f || f.isDirectory()) { if (f) f.close(); return false; }
    if (!total || total > dst->size) { f.close(); return false; }
    // Erase granularity is 4 KiB and the span must be rounded UP, or the tail of
    // the image lands in a sector that was never erased.
    const size_t span = (total + 4095u) & ~(size_t)4095u;
    if (span > dst->size) { f.close(); return false; }
    if (esp_partition_erase_range(dst, 0, span) != ESP_OK) { f.close(); return false; }
    uint8_t *buf = (uint8_t *)heap_caps_malloc(8200, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!buf) { f.close(); return false; }
    size_t off = 0;
    bool ok = true;
    int retry = 0;
    while (off < total) {
        (void)esp_task_wdt_reset();            // harmless when not subscribed
        const size_t want = (total - off > 8192) ? 8192 : (size_t)(total - off);
        const int n = f.read(buf, want);
        if (n <= 0) {
            if (retry++ == 0) { f.seek(off); delay(15); continue; }
            ok = false;
            break;
        }
        size_t wlen = (size_t)n;
        while (wlen & 3u) buf[wlen++] = 0xFF;     // partition writes go in 4-byte units
        if (esp_partition_write(dst, off, buf, wlen) != ESP_OK) { ok = false; break; }
        off += (size_t)n;
        retry = 0;
        swap_flash_progress(off, total, "Installing DualBoot");
    }
    free(buf);
    f.close();
    return ok && off == total;
}

// Verify what actually landed, not what we meant to write: the first byte and
// the app descriptor magic are what the bootloader itself will check.
static bool swap_verify_slot(const esp_partition_t *p) {
    if (!p) return false;
    uint8_t h[0x24];
    memset(h, 0, sizeof(h));
    if (esp_partition_read(p, 0, h, sizeof(h)) != ESP_OK) return false;
    return swap_head_ok(h, nullptr);
}

static const char *swap_boot_name(const esp_partition_t *p) {
    if (!p) return "none";
    if (p->subtype == ESP_PARTITION_SUBTYPE_APP_FACTORY) return "factory";
    if (p->subtype == ESP_PARTITION_SUBTYPE_APP_OTA_0)   return "ota_0";
    if (p->subtype == ESP_PARTITION_SUBTYPE_APP_OTA_1)   return "ota_1";
    return "other";
}

// Aim otadata at a slot, then prove it took.  A refused or lost write here is the
// failure that looks from outside like "it switched the partition but never
// booted", so the read-back is not optional.
static bool swap_commit(const esp_partition_t *target) {
    if (!target) return false;
    esp_err_t e = esp_ota_set_boot_partition(target);
    if (e != ESP_OK) {
        USBSerial.printf("[swap] set_boot_partition failed (%d), retrying\n", (int)e);
        delay(20);
        e = esp_ota_set_boot_partition(target);
    }
    const esp_partition_t *sel = esp_ota_get_boot_partition();
    const bool ok = (e == ESP_OK) && sel &&
                    sel->address == target->address &&
                    sel->subtype == target->subtype;
    if (!ok) {
        USBSerial.printf("[swap] boot slot did not take %s (%d)\n",
                         swap_boot_name(target), (int)e);
        return false;
    }
    USBSerial.printf("[swap] boot slot -> %s @ 0x%06X\n",
                     swap_boot_name(target), (unsigned)target->address);
    return true;
}

// One line per boot that answers "did the switch happen" without a debugger.
static void swap_boot_report() {
    USBSerial.printf("[swap] running from %s, boot slot %s\n",
                     swap_boot_name(esp_ota_get_running_partition()),
                     swap_boot_name(esp_ota_get_boot_partition()));
}

// v2.10: list every app slot the bootloader can see, and which one this build
// treats as HOME.  The Arduino IDE builds the partition table from Tools >
// Partition Scheme and ignores the partitions.csv sitting next to this sketch,
// so the only way to know which table is really on the chip is to print it.
// With the factory CSV this reads:
//     factory @ 0x010000 5120 KB <- home
//     ota_0   @ 0x510000 4096 KB        <- transit, the swap target
// If it shows two ota_ slots instead, the factory table was not flashed.
static void bk_table_report() {
    USBSerial.printf("[swap] flash %lu MB, app slots:\n",
                     (unsigned long)(ESP.getFlashChipSize() / (1024u * 1024u)));
    for (esp_partition_iterator_t it = esp_partition_find(
             ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_ANY, nullptr);
         it != nullptr; it = esp_partition_next(it)) {
        const esp_partition_t *pt = esp_partition_get(it);
        USBSerial.printf("[swap]   %-7s @ 0x%06X %5lu KB%s\n",
                         swap_boot_name(pt), (unsigned)pt->address,
                         (unsigned long)(pt->size / 1024u),
                         bk_is_home(pt) ? "  <- home" : "");
    }
    if (!bk_transit())
        USBSerial.println("[swap]   no second app slot - the hand-off cannot run");
}

// -------------------------------------------------- the way back to DualBoot ---
// The hand-off in the other direction: aim the bootloader at the dual-boot image in
// ota_0 and reboot.  Pass a path to replace that image first, or nullptr to boot
// whatever is already there.  Changes nothing if the target is not bootable.
static bool swap_back_to_dualboot(const char *bin_on_card, const char **why) {
    // v2.0: the same transaction as the forward hand-off, not a side door.
    // DualBoot has to end up in factory (HOME).  Written into ota_0 with otadata
    // selecting it, its own resolver sees "record IDLE while running from the
    // spare slot", calls that an orphaned clone, verifies factory and hands the
    // boot slot straight back -- the "it copies across but will not switch"
    // symptom.  Cloning the launcher to ota_0 first and writing DualBoot into
    // factory afterwards keeps the invariant both firmwares now share.
    const char *img = (bin_on_card && bin_on_card[0]) ? bin_on_card : swap_pick_image();
    if (!img) {
        if (why) *why = "DualBoot.bin is not in WATCH/DualBoot on the card";
        return false;
    }
    if (!boot_prepare(img)) {
        if (why) *why = g_x_last.length() ? g_x_last.c_str() : "the swap did not start";
        return false;
    }
    return true;
}

// ============================================================================
//  v2.0 -- BOOT PROTOCOL: the hand-off as a transaction, not a flag
// ----------------------------------------------------------------------------
//  The old hand-off was a boolean ("lswap") plus a path and a try counter, and
//  every failure it ever had came from that shape.  A boolean cannot tell
//  "armed" from "installing" from "gave up", so a boot that landed in the wrong
//  slot could only guess -- and the guesses compounded: otadata left pointing at
//  the spare slot, a half-written factory nobody recorded, a USB reflash the
//  bootloader ignored because otadata still selected the clone.
//
//  This is the transaction from the boot protocol, adopted verbatim so both
//  firmwares speak the same one:
//
//      IDLE -> PREPARED -> ARMED -> INSTALLING -> INSTALLED -> IDLE
//
//  HOME    = factory  (this table: 5 MB @ 0x010000)  -- where the launcher lives
//  TRANSIT = ota_0    (this table: 4 MB @ 0x510000)  -- the copy that keeps HOME
//                                                       safe to erase
//
//  Invariants, in the order they matter:
//   1. never erase the slot we are executing from;
//   2. the record describes reality or it is cleared -- boot_resolve() decides;
//   3. otadata is committed only after the destination verifies, and the record
//      is cleared only after otadata reads back selecting it;
//   4. every phase resumes; none assumes it ran once;
//   5. a failed transaction ends in HOME running a verified launcher;
//   6. one writer of otadata at a time -- the on-boot anchor is gated on IDLE.
//
//  Record: NVS namespace "dualboot", the spec's key names, so DualBoot can
//  finish what we started and we can finish what it started.  rcrc is CRC32 over
//  the canonical text "1|st|tx|ilen|icrc|clen|ccrc|att|why|path"; sv is written
//  0 first and 1 last, so a torn write reads as version 0 and is ignored.
// ============================================================================

static const char *bk_phase_name(uint8_t p) {
    switch (p) {
    case BK_PREPARED:   return "PREPARED";
    case BK_ARMED:      return "ARMED";
    case BK_INSTALLING: return "INSTALLING";
    case BK_INSTALLED:  return "INSTALLED";
    default:            return "IDLE";
    }
}
static const char *bk_why_text(uint8_t w) {
    switch (w) {
    case BK_WHY_NO_CARD:          return "no SD card";
    case BK_WHY_NO_IMAGE:         return "image not found";
    case BK_WHY_BAD_IMAGE:        return "image will not boot";
    case BK_WHY_BATTERY:          return "battery too low";
    case BK_WHY_CLONE_UNBOOTABLE: return "launcher copy would not boot";
    case BK_WHY_ORPHAN_HOME:      return "launcher slot will not verify";
    case BK_WHY_WRITE:            return "write failed";
    case BK_WHY_VERIFY:           return "verify failed";
    case BK_WHY_BOOTSLOT:         return "boot slot did not take";
    case BK_WHY_RB_VERIFY:        return "rollback copy will not verify";
    case BK_WHY_RB_WRITE:         return "rollback write failed";
    case BK_WHY_RESUME:           return "attempt budget spent";
    default:                      return "none";
    }
}

// ROM CRC32 (reflected, init 0, no final xor).  The same call on the card, on
// the write and on the read-back -- a short write shows up as a mismatch, which
// 0xE9 at offset 0 and a 0x24-byte head check can never do.
static uint32_t bk_crc(uint32_t crc, const void *buf, size_t n) {
    // Reflected CRC32, polynomial 0xEDB88320, init 0, no final xor -- the same
    // numbers esp_rom_crc32_le() produces, without depending on a ROM header the
    // Arduino include path does not reliably expose.  The table is built once,
    // 1 KB of .bss, and this runs at ~30 MB/s: 1.5 MB verifies in about 50 ms.
    static uint32_t tab[256];
    static bool built = false;
    if (!built) {
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t c = i;
            for (int k = 0; k < 8; k++) c = (c & 1u) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            tab[i] = c;
        }
        built = true;
    }
    const uint8_t *q = (const uint8_t *)buf;
    while (n--) crc = tab[(crc ^ *q++) & 0xFFu] ^ (crc >> 8);
    return crc;
}
static uint32_t bk_crc_slot(const esp_partition_t *p, uint32_t len) {
    if (!p || !len || len > p->size) return 0;
    uint8_t *buf = (uint8_t *)malloc(4096);
    if (!buf) return 0;
    uint32_t crc = 0, off = 0;
    while (off < len) {
        const size_t n = (len - off > 4096) ? 4096 : (size_t)(len - off);
        if (esp_partition_read(p, off, buf, n) != ESP_OK) { free(buf); return 0; }
        crc = bk_crc(crc, buf, n);
        off += (uint32_t)n;
        (void)esp_task_wdt_reset();
    }
    free(buf);
    return crc;
}
static uint32_t bk_crc_file(const char *path, uint32_t *len_out) {
    File f = SD_MMC.open(path, FILE_READ);
    if (!f || f.isDirectory()) { if (f) f.close(); return 0; }
    uint8_t *buf = (uint8_t *)malloc(4096);
    if (!buf) { f.close(); return 0; }
    uint32_t crc = 0, off = 0;
    for (;;) {
        const int n = f.read(buf, 4096);
        if (n <= 0) break;
        crc = bk_crc(crc, buf, (size_t)n);
        off += (uint32_t)n;
        (void)esp_task_wdt_reset();
    }
    free(buf);
    f.close();
    if (len_out) *len_out = off;
    return crc;
}

// Bytes from the start of a slot to the end of its last non-0xFF byte, rounded
// up to 4 KB.  A 1.5 MB launcher clones in ~3 s instead of copying 5 MB.  The
// tail was 0xFF in the source too and erased flash is 0xFF, so the shortened
// copy is still a byte-exact image.  Any doubt falls back to the whole slot.
static uint32_t used_span(const esp_partition_t *p) {
    if (!p) return 0;
    uint8_t *buf = (uint8_t *)malloc(4096);
    if (!buf) return p->size;
    for (uint32_t off = p->size; off >= 4096; off -= 4096) {
        if (esp_partition_read(p, off - 4096, buf, 4096) != ESP_OK) { free(buf); return p->size; }
        for (int i = 0; i < 4096; i++)
            if (buf[i] != 0xFF) { free(buf); return off; }
    }
    free(buf);
    return 0;
}

// v2.10: HOME and TRANSIT are resolved from the table that is ACTUALLY flashed.
//
// The custom partitions.csv next to this sketch is not used by the Arduino IDE:
// the table comes from Tools > Partition Scheme, so on every stock dual-OTA
// scheme there is no 'factory' partition at all.  bk_home() therefore returned
// nullptr, boot_prepare() refused with "no factory/ota_0 pair", and the whole
// dual-boot hand-off looked broken no matter what was on the card.
//
//   factory present (the custom CSV, or any table with one)
//       HOME = factory      TRANSIT = ota_0
//   no factory (stock dual-OTA schemes, e.g. the IDE's default dual layout)
//       HOME = ota_0        TRANSIT = ota_1
//
// Nothing else changes: HOME is still the slot the launcher must run from, and
// TRANSIT is still the slot that gets overwritten so HOME stays safe.
static const esp_partition_t *bk_home() {
    const esp_partition_t *f = esp_partition_find_first(
        ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_FACTORY, nullptr);
    if (f) return f;
    return esp_partition_find_first(ESP_PARTITION_TYPE_APP,
                                    ESP_PARTITION_SUBTYPE_APP_OTA_0, nullptr);
}
static const esp_partition_t *bk_transit() {
    const esp_partition_t *f = esp_partition_find_first(
        ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_FACTORY, nullptr);
    const esp_partition_t *t = esp_partition_find_first(
        ESP_PARTITION_TYPE_APP,
        f ? ESP_PARTITION_SUBTYPE_APP_OTA_0 : ESP_PARTITION_SUBTYPE_APP_OTA_1, nullptr);
    if (t && f && t->address == f->address) return nullptr;
    return t;
}
static bool bk_is_home(const esp_partition_t *p) {
    const esp_partition_t *h = bk_home();
    return p && h && p->address == h->address;
}
static bool bk_head_ok(const esp_partition_t *p, const char **why) {
    if (!p) { if (why) *why = "no such slot"; return false; }
    uint8_t h[0x24];
    memset(h, 0, sizeof(h));
    if (esp_partition_read(p, 0, h, sizeof(h)) != ESP_OK) {
        if (why) *why = "read failed";
        return false;
    }
    return swap_head_ok(h, why);
}
static bool bk_verified(const esp_partition_t *p, uint32_t len, uint32_t want, const char **why) {
    if (!bk_head_ok(p, why)) return false;
    if (!len || !want) { if (why) *why = "no length/crc in the record"; return false; }
    if (len > p->size) { if (why) *why = "longer than the slot"; return false; }
    if (bk_crc_slot(p, len) != want) { if (why) *why = "crc mismatch"; return false; }
    return true;
}

// Slot -> slot copy of exactly len bytes: the clone (self -> TRANSIT) and the
// rollback (TRANSIT -> HOME).  Neither ever targets the running slot.
static bool bk_copy_slot(const esp_partition_t *src, const esp_partition_t *dst, uint32_t len) {
    if (!src || !dst || src == dst || !len || len > dst->size) return false;
    const size_t span = ((size_t)len + 4095u) & ~(size_t)4095u;
    if (span > dst->size) return false;
    if (esp_partition_erase_range(dst, 0, span) != ESP_OK) return false;
    uint8_t *buf = (uint8_t *)malloc(8192);
    if (!buf) return false;
    uint32_t off = 0;
    bool ok = true;
    while (off < len) {
        const size_t n = (len - off > 8192) ? 8192 : (size_t)(len - off);
        if (esp_partition_read(src, off, buf, n) != ESP_OK) { ok = false; break; }
        if (esp_partition_write(dst, off, buf, n) != ESP_OK) { ok = false; break; }
        off += (uint32_t)n;
        (void)esp_task_wdt_reset();
    }
    free(buf);
    return ok;
}
static bool bk_clone_self(const esp_partition_t *self, const esp_partition_t *dst,
                          uint32_t &clen_out, uint32_t &ccrc_out) {
    if (!self || !dst || self == dst) return false;
    const uint32_t total = used_span(self);
    if (!total || total > dst->size) return false;
    if (!bk_copy_slot(self, dst, total)) return false;
    clen_out = total;
    ccrc_out = bk_crc_slot(dst, total);
    return ccrc_out != 0;
}

static uint32_t bk_rec_crc(const BootRec &r) {
    char line[288];               // 160-char path + 5 x 10-digit fields + markers
    snprintf(line, sizeof(line), "%d|%d|%lu|%lu|%lu|%lu|%lu|%d|%d|%s",
             (int)BK_SV, (int)r.phase,
             (unsigned long)r.tx, (unsigned long)r.ilen, (unsigned long)r.icrc,
             (unsigned long)r.clen, (unsigned long)r.ccrc,
             (int)r.att, (int)r.why, r.path.c_str());
    return bk_crc(0, line, strlen(line));
}
static bool bk_rec_load(BootRec &r) {
    g_swapNvs.begin(BK_NS, true);
    const uint8_t sv = g_swapNvs.getUChar("sv", 0);
    r.phase = g_swapNvs.getUChar("st", 0);
    r.tx    = g_swapNvs.getULong("tx", 0);
    r.ilen  = g_swapNvs.getULong("ilen", 0);
    r.icrc  = g_swapNvs.getULong("icrc", 0);
    r.clen  = g_swapNvs.getULong("clen", 0);
    r.ccrc  = g_swapNvs.getULong("ccrc", 0);
    r.att   = g_swapNvs.getUChar("att", 0);
    r.why   = g_swapNvs.getUChar("why", 0);
    r.path  = g_swapNvs.getString("path", "");
    const uint32_t rcrc = g_swapNvs.getULong("rcrc", 0);
    g_swapNvs.end();
    if (sv != BK_SV) return false;                // torn or foreign record
    if (r.phase > BK_INSTALLED) return false;     // nonsense phase
    if (r.path.length() > 160) return false;
    if (rcrc != bk_rec_crc(r)) return false;      // payload does not match
    return true;
}
static void bk_rec_save(BootRec &r) {
    g_swapNvs.begin(BK_NS, false);
    g_swapNvs.putUChar("sv", 0);                  // invalid while we write
    g_swapNvs.putULong("tx", r.tx);
    g_swapNvs.putString("path", r.path);
    g_swapNvs.putULong("ilen", r.ilen);
    g_swapNvs.putULong("icrc", r.icrc);
    g_swapNvs.putULong("clen", r.clen);
    g_swapNvs.putULong("ccrc", r.ccrc);
    g_swapNvs.putUChar("att", r.att);
    g_swapNvs.putUChar("why", r.why);
    g_swapNvs.putULong("rcrc", bk_rec_crc(r));
    g_swapNvs.putUChar("st", r.phase);            // phase after its payload
    g_swapNvs.putUChar("sv", BK_SV);              // valid marker last of all
    g_swapNvs.end();
}
static void bk_rec_clear() {
    g_swapNvs.begin(BK_NS, false);
    static const char *k[] = { "sv", "st", "tx", "path", "ilen", "icrc",
                               "clen", "ccrc", "att", "why", "rcrc",
                               "lswap", "lpath", "ltry", "lfrom" };  // legacy tail
    for (unsigned i = 0; i < sizeof(k) / sizeof(k[0]); i++) g_swapNvs.remove(k[i]);
    g_swapNvs.end();
}
static uint8_t bk_phase() {
    BootRec r;
    return bk_rec_load(r) ? r.phase : (uint8_t)BK_IDLE;
}
static void bk_report(BootRec &r) {
    USBSerial.printf("[lswap] tx=%lu stopped in %s: %s\n",
                     (unsigned long)r.tx, bk_phase_name(r.phase), bk_why_text(r.why));
    ptt_flash(String("Swap: ") + bk_why_text(r.why), 2600);
}

// ---- rollback: the only terminal state, and it targets HOME ---------------
static void bk_rollback(BootRec &r) {
    const esp_partition_t *home    = bk_home();
    const esp_partition_t *transit = bk_transit();
    USBSerial.printf("[lswap] tx=%lu rollback from %s: %s\n",
                     (unsigned long)r.tx, bk_phase_name(r.phase), bk_why_text(r.why));
    ptt_flash("Putting the launcher back", 1600);
    const char *why = nullptr;
    if (!bk_verified(transit, r.clen, r.ccrc, &why)) {
        r.why = BK_WHY_RB_VERIFY;                  // do not make it worse
        bk_rec_clear();
        bk_report(r);
        return;
    }
    if (!bk_copy_slot(transit, home, r.clen) ||
        !bk_verified(home, r.clen, r.ccrc, &why)) {
        r.why = BK_WHY_RB_WRITE;
        bk_rec_clear();                            // IDLE+TRANSIT self-heals
        bk_report(r);
        return;
    }
    if (!swap_commit(home)) {
        r.why = BK_WHY_BOOTSLOT;
        bk_rec_clear();
        bk_report(r);
        return;
    }
    bk_rec_clear();
    USBSerial.printf("[lswap] tx=%lu otadata -> factory, launcher restored\n",
                     (unsigned long)r.tx);
    ptt_flash("Launcher restored", 900);
    lvgl_pump();
    delay(300);
    esp_restart();
}

// A failure inside a phase either retries (record kept, reboot) or goes to the
// terminal state.  att was already bumped when the resolver entered.
static void bk_phase_failed(BootRec &r) {
    const uint8_t budget = (r.phase == BK_INSTALLED) ? BK_ATT_OTA : BK_ATT_WRITE;
    if (r.att >= budget) { bk_rollback(r); return; }
    USBSerial.printf("[lswap] tx=%lu %s on next boot (%u/%u): %s\n",
                     (unsigned long)r.tx, bk_phase_name(r.phase),
                     (unsigned)r.att, (unsigned)budget, bk_why_text(r.why));
    ptt_flash(String("Swap: ") + bk_why_text(r.why) + ", retrying", 2200);
    lvgl_pump();
    delay(500);
    esp_restart();
}

// ---- INSTALLING -> INSTALLED: HOME is written only while running TRANSIT ---
static void bk_install(BootRec &r) {
    const esp_partition_t *home = bk_home();
    if (!home) { r.why = BK_WHY_WRITE; bk_rec_clear(); bk_report(r); return; }
    if (!g_sd_ok) {                                // keep the record: insert card
        r.why = BK_WHY_NO_CARD;
        bk_rec_save(r);
        bk_report(r);
        return;
    }
    r.phase = BK_INSTALLING;                       // commit point before any erase
    r.why   = BK_WHY_NONE;
    bk_rec_save(r);
    USBSerial.printf("[lswap] tx=%lu phase=ARMED -> INSTALLING  %s  %lu B  crc 0x%08lX\n",
                     (unsigned long)r.tx, r.path.c_str(),
                     (unsigned long)r.ilen, (unsigned long)r.icrc);
    ptt_flash("Installing LauncherOS", 1200);
    if (!swap_write_image(r.path.c_str(), home, (size_t)r.ilen)) {
        r.why = BK_WHY_WRITE;
        bk_rec_save(r);
        bk_phase_failed(r);
        return;
    }
    const char *why = nullptr;
    if (!bk_verified(home, r.ilen, r.icrc, &why)) {
        USBSerial.printf("[lswap] tx=%lu factory verify failed: %s\n",
                         (unsigned long)r.tx, why ? why : "?");
        r.why = BK_WHY_VERIFY;
        bk_rec_save(r);
        bk_phase_failed(r);
        return;
    }
    r.phase = BK_INSTALLED;
    bk_rec_save(r);
    USBSerial.printf("[lswap] tx=%lu write -> factory, verified  crc ok\n",
                     (unsigned long)r.tx);
    if (!swap_commit(home)) {                      // keep the note: retry 4 bytes
        r.why = BK_WHY_BOOTSLOT;
        bk_rec_save(r);
        bk_phase_failed(r);
        return;
    }
    bk_rec_clear();
    USBSerial.printf("[lswap] tx=%lu otadata -> factory, LauncherOS '%s'\n",
                     (unsigned long)r.tx, r.path.c_str());
    ptt_flash("Launching", 900);
    lvgl_pump();
    delay(250);
    esp_restart();
}

// ---- migration: adopt a note left behind by the old build -----------------
static bool bk_migrate_legacy(BootRec &r) {
    if (!swap_note_pending()) return false;
    const String p = swap_note_path();
    if (!p.length()) { bk_rec_clear(); return false; }
    const esp_partition_t *self    = esp_ota_get_running_partition();
    const esp_partition_t *transit = bk_transit();
    r = BootRec();
    r.tx    = (uint32_t)(millis() / 1000u) + 1u;
    r.path  = p;
    r.why   = BK_WHY_RESUME;
    r.phase = bk_is_home(self) ? (uint8_t)BK_PREPARED : (uint8_t)BK_ARMED;
    if (transit) {
        const uint32_t span = used_span(transit);
        if (span) { r.clen = span; r.ccrc = bk_crc_slot(transit, span); }
    }
    size_t len = 0;
    if (g_sd_ok && swap_probe_image(p.c_str(), &len, nullptr) && len) {
        r.ilen = (uint32_t)len;
        r.icrc = bk_crc_file(p.c_str(), nullptr);
    }
    bk_rec_save(r);
    USBSerial.printf("[lswap] adopted legacy note tx=%lu as %s (%s)\n",
                     (unsigned long)r.tx, bk_phase_name(r.phase), p.c_str());
    return true;
}

// ---- the resolver: one entry point, called before the UI exists -----------
static void boot_resolve() {
    BootRec r;
    const bool have = bk_rec_load(r);
    if (!have) {
        if (!bk_migrate_legacy(r)) {
            const esp_partition_t *self = esp_ota_get_running_partition();
            if (bk_is_home(self)) return;          // normal boot; setup anchors
            // IDLE + TRANSIT: the orphaned clone -- record lost, or the old
            // build's "give up" branch.  Hand the boot slot back instead of
            // drifting, which is the wedge users could not get out of.
            USBSerial.println("[lswap] orphaned clone in ota_0, checking factory");
            if (bk_head_ok(bk_home(), nullptr) && swap_commit(bk_home())) {
                USBSerial.println("[lswap] otadata -> factory, restarting");
                ptt_flash("Back to LauncherOS", 800);
                lvgl_pump();
                delay(200);
                esp_restart();
            }
            USBSerial.println("[lswap] factory will not verify, staying on the copy");
            r.why = BK_WHY_ORPHAN_HOME;
            bk_report(r);
            return;
        }
    }

    const esp_partition_t *self = esp_ota_get_running_partition();
    const bool in_home = bk_is_home(self);
    const uint8_t budget = (r.phase == BK_PREPARED || r.phase == BK_INSTALLED)
                         ? BK_ATT_OTA : BK_ATT_WRITE;

    USBSerial.printf("[lswap] tx=%lu phase=%s running=%s att=%u/%u\n",
                     (unsigned long)r.tx, bk_phase_name(r.phase),
                     in_home ? "factory" : "ota_0",
                     (unsigned)r.att, (unsigned)budget);
    if (r.att >= budget) {
        USBSerial.printf("[lswap] tx=%lu budget spent in %s\n",
                         (unsigned long)r.tx, bk_phase_name(r.phase));
        if (r.phase == BK_INSTALLING || r.phase == BK_INSTALLED) { bk_rollback(r); return; }
        if (!r.why) r.why = BK_WHY_RESUME;
        bk_rec_clear();
        bk_report(r);
        return;
    }
    r.att++;
    bk_rec_save(r);

    switch (r.phase) {
    case BK_PREPARED:
        if (in_home) {
            if (swap_commit(bk_transit())) {
                r.phase = BK_ARMED;
                bk_rec_save(r);
                USBSerial.printf("[lswap] tx=%lu otadata -> ota_0, restarting\n",
                                 (unsigned long)r.tx);
                ptt_flash("Restarting to finish", 700);
                lvgl_pump();
                delay(150);
                esp_restart();
            }
            r.why = BK_WHY_BOOTSLOT;               // resolver re-arms next boot
            bk_rec_save(r);
            bk_report(r);
        } else {
            r.phase = BK_ARMED;                    // otadata took, record did not
            bk_rec_save(r);
            boot_resolve();
        }
        return;

    case BK_ARMED:
        if (!in_home) { bk_install(r); return; }
        // the bootloader fell back: the clone would not boot
        r.why = BK_WHY_CLONE_UNBOOTABLE;
        if (r.att < 2 && bk_head_ok(bk_transit(), nullptr) && swap_commit(bk_transit())) {
            USBSerial.printf("[lswap] tx=%lu re-arming the copy once more\n",
                             (unsigned long)r.tx);
            delay(100);
            esp_restart();
        }
        bk_rec_clear();
        bk_report(r);
        return;

    case BK_INSTALLING:
        if (!in_home) { bk_install(r); return; }
        // fell back to HOME and HOME boots: the image may already be there
        if (bk_verified(bk_home(), r.ilen, r.icrc, nullptr)) {
            r.phase = BK_INSTALLED;
            r.why   = BK_WHY_NONE;
            bk_rec_save(r);
            if (swap_commit(bk_home())) { bk_rec_clear(); esp_restart(); }
            bk_rollback(r);
            return;
        }
        r.why = BK_WHY_VERIFY;
        bk_rec_save(r);
        bk_rollback(r);
        return;

    case BK_INSTALLED:
        if (in_home) {
            bk_rec_clear();
            USBSerial.printf("[lswap] tx=%lu launcher is in factory, transaction closed\n",
                             (unsigned long)r.tx);
            return;
        }
        if (swap_commit(bk_home())) {
            bk_rec_clear();
            USBSerial.printf("[lswap] tx=%lu otadata -> factory, restarting\n",
                             (unsigned long)r.tx);
            ptt_flash("Launching", 700);
            lvgl_pump();
            delay(150);
            esp_restart();
        }
        r.why = BK_WHY_BOOTSLOT;
        bk_rec_save(r);
        bk_phase_failed(r);
        return;

    default:
        bk_rec_clear();
        return;
    }
}

// ---- IDLE -> PREPARED: preconditions, clone, record, arm ------------------
// Runs while executing from HOME.  Nothing is written until every check passes.
static bool boot_prepare(const char *path) {
    const esp_partition_t *home    = bk_home();
    const esp_partition_t *transit = bk_transit();
    const esp_partition_t *self    = esp_ota_get_running_partition();
    if (!home || !transit || !self) {
        g_x_last = "This partition table has no second app slot to swap into.";
        g_x_orb = 2; x_finish_info();
        return false;
    }
    if (!bk_is_home(self)) {
        g_x_last = "The launcher must be running from its home slot to move itself.";
        g_x_orb = 2; x_finish_info();
        return false;
    }
    if (bk_phase() != BK_IDLE) {
        g_x_last = "A swap is already in progress. Reboot to let it finish.";
        g_x_orb = 2; x_finish_info();
        return false;
    }
    size_t len = 0;
    const char *why = nullptr;
    if (!swap_probe_image(path, &len, &why)) {
        g_x_last = String("That image will not boot: ") +
                   (why ? why : "not an app image") + ".";
        g_x_orb = 2; x_finish_info();
        return false;
    }
    if (!len || len > home->size) {
        g_x_last = "That image does not fit the launcher slot.";
        g_x_orb = 2; x_finish_info();
        return false;
    }
    const uint32_t icrc = bk_crc_file(path, nullptr);
    if (!icrc) {
        g_x_last = "Could not read the image off the card.";
        g_x_orb = 2; x_finish_info();
        return false;
    }

    BootRec r;
    r.tx    = (uint32_t)(millis() / 1000u) + 1u;
    r.path  = path;
    r.ilen  = (uint32_t)len;
    r.icrc  = icrc;
    r.phase = BK_PREPARED;
    USBSerial.printf("[lswap] tx=%lu phase=IDLE -> PREPARED  %s  %lu B  crc 0x%08lX\n",
                     (unsigned long)r.tx, path, (unsigned long)r.ilen,
                     (unsigned long)r.icrc);
    ptt_flash("Moving the launcher aside", 1200);
    if (!bk_clone_self(home, transit, r.clen, r.ccrc) ||
        !bk_verified(transit, r.clen, r.ccrc, &why)) {
        USBSerial.printf("[lswap] tx=%lu clone failed: %s\n",
                         (unsigned long)r.tx, why ? why : "write");
        r.why = BK_WHY_CLONE_UNBOOTABLE;
        bk_rec_clear();                            // nothing was armed
        g_x_last = "Copying the launcher to the spare slot failed. Nothing was switched.";
        g_x_orb = 2; x_finish_info();
        ptt_flash("Swap failed", 2200);
        return false;
    }
    USBSerial.printf("[lswap] tx=%lu clone -> ota_0 %lu B  crc ok\n",
                     (unsigned long)r.tx, (unsigned long)r.clen);
    bk_rec_save(r);                                // commit point of PREPARED
    if (!swap_commit(transit)) {
        r.why = BK_WHY_BOOTSLOT;                   // resolver re-arms next boot
        bk_rec_save(r);
        g_x_last = "Could not select the spare slot. Reboot will retry.";
        g_x_orb = 2; x_finish_info();
        return false;
    }
    r.phase = BK_ARMED;
    bk_rec_save(r);
    USBSerial.printf("[lswap] tx=%lu otadata -> ota_0, restarting\n",
                     (unsigned long)r.tx);
    ptt_flash("Restarting to finish", 900);
    lvgl_pump();
    delay(250);
    esp_restart();
    return false;
}

// -------------------------------------------------------- voice / UI entry ---
// No dialog means no undo, so arming is separate from installing: everything
// that can be checked without touching flash happens here, then a 5 s window
// runs where any other command (or "cancel") aborts.
static void swap_arm() {
    if (g_swapArmMs) {
        g_x_last = "A slot swap is already waiting.";
        g_x_orb = 0; x_finish_info(); return;
    }
    if (!g_sd_ok) {
        g_x_last = "No SD card, so I cannot install anything.";
        g_x_orb = 2; x_finish_info(); return;
    }
    const char *img = swap_pick_image();
    if (!img) {
        g_x_last = "DualBoot.bin is not in WATCH/DualBoot on the card.";
        g_x_orb = 2; x_finish_info(); return;
    }
    size_t len = 0;
    const char *why = nullptr;
    if (!swap_probe_image(img, &len, &why)) {
        g_x_last = String("That DualBoot.bin will not boot: ") +
                   (why ? why : "not an app image") + ".";
        g_x_orb = 2; x_finish_info(); return;
    }
    const esp_partition_t *self = esp_ota_get_running_partition();
    const esp_partition_t *dst = swap_other_app(self);
    if (!self || !dst || dst == self || len > dst->size) {
        g_x_last = "This layout has no second app slot for that image.";
        g_x_orb = 2; x_finish_info(); return;
    }
    if (g_has_pmu && g_batt_pct >= 0 && g_batt_pct < SWAP_MIN_PCT && !g_charging) {
        char b[96];
        snprintf(b, sizeof(b), "Battery %d%%. Charge to %d%% or plug in first.",
                 g_batt_pct, SWAP_MIN_PCT);
        g_x_last = b; g_x_orb = 2; x_finish_info(); return;
    }
    g_swapArmMs   = millis() | 1;        // never 0: 0 means "not armed"
    g_swapImgPath = img;
    g_swapLastSec = -1;
    pm_activity("slot swap armed");
    USBSerial.printf("[swap] armed: %s (%u B)\n", img, (unsigned)len);
    g_x_last = "Installing Dual Boot in 5 s. Say cancel, or any other command, to stop. "
               "One reboot, about 20 s, do not power off.";
    g_x_orb = 1; x_finish_info();
}

// Returns true when something was actually cancelled.
// The way back is a reboot, not a write, so it is armed the same way: 5 s to
// change your mind, and nothing is touched until the window closes.
static void swap_arm_back() {
    if (g_swapArmMs) {
        g_x_last = "A slot change is already waiting.";
        g_x_orb = 0; x_finish_info(); return;
    }
    const esp_partition_t *self = esp_ota_get_running_partition();
    const esp_partition_t *other = swap_other_app(self);
    if (!other || other == self) {
        g_x_last = "There is no other slot to boot.";
        g_x_orb = 2; x_finish_info(); return;
    }
    if (!swap_verify_slot(other)) {
        g_x_last = "The other slot holds nothing bootable. Say Dual Boot to install one.";
        g_x_orb = 2; x_finish_info(); return;
    }
    g_swapBack    = true;
    g_swapImgPath = nullptr;
    g_swapArmMs   = millis() | 1;
    g_swapLastSec = -1;
    pm_activity("back to dualboot armed");
    g_x_last = "Booting DualBoot in 5 s. Say cancel to stay here.";
    g_x_orb = 1; x_finish_info();
}

static bool swap_abort(const char *why) {
    if (!g_swapArmMs) return false;
    g_swapArmMs = 0;
    g_swapImgPath = nullptr;
    g_swapBack    = false;
    g_swapLastSec = -1;
    USBSerial.printf("[swap] cancelled (%s)\n", why ? why : "cancel");
    g_x_last = String("Slot swap cancelled") +
               (why && *why ? String(" - ") + why : String("")) +
               ". Nothing was written.";
    g_x_orb = 0; x_finish_info();
    ptt_flash("Cancelled", 1200);
    return true;
}

// Runs every loop(), never inside an LVGL event: the installer erases and writes
// flash and pumps the panel while it does.
static void swap_service() {
    if (!g_swapArmMs || g_swapBusy) return;
    const uint32_t now = millis();
    const int32_t  age = (int32_t)(now - g_swapArmMs);   // signed: see the pm_* notes
    if (age < 0) { g_swapArmMs = now | 1; return; }       // clock went backwards
    int32_t left = (SWAP_CONFIRM_MS - age + 999) / 1000;
    if (left < 0) left = 0;
    if (left != g_swapLastSec) {
        g_swapLastSec = left;
        char b[48];
        snprintf(b, sizeof(b), g_swapBack ? "Booting DualBoot in %d"
                                          : "Installing Dual Boot in %d", (int)left);
        ptt_flash(String(b), 1400);
    }
    if (age < SWAP_CONFIRM_MS) return;
    const char *img = g_swapImgPath;
    const bool  back = g_swapBack;
    g_swapBack = false;
    g_swapArmMs = 0;
    g_swapImgPath = nullptr;
    g_swapLastSec = -1;
    if (!img && !back) return;
    g_swapBusy = true;
    if (back) {
        USBSerial.println("[swap] window closed, handing back to DualBoot");
        const char *why = nullptr;
        swap_back_to_dualboot(nullptr, &why);        // reboots; returns only on failure
        g_swapBusy = false;
        g_x_last = String("Could not boot DualBoot") +
                   (why ? String(": ") + why : String("")) + ".";
        g_x_orb = 2; x_finish_info();
        ptt_flash("No DualBoot", 2000);
        return;
    }
    USBSerial.println("[swap] window closed, installing");
    if (!boot_prepare(img)) {
        g_swapBusy = false;
        g_x_last = "Slot swap failed. Nothing was rebooted and the launcher is intact.";
        g_x_orb = 2; x_finish_info();
        ptt_flash("Swap failed", 2000);
        return;
    }
    g_swapBusy = false;
}


// ============================================================================
//  boot
// ============================================================================
// ============================================================================
//  v2.8 SD MOUNT  --  ported from DualBoot's sd_bootstrap()
// ============================================================================
// A card that has just been powered up (or pushed in while the watch was
// already running) does not always answer the first CMD0, and one failed
// begin() used to leave the launcher with no apps, no wallpapers and no
// WATCH/wifi/config.txt -- which also reads as "Wi-Fi is broken", because
// that file is where the network list comes from.
//
// Three attempts, with the pins re-applied between them, exactly the way the
// DualBoot launcher does it.  1-bit mode on D0 only, which is what the board's
// slot is wired for.
static bool sd_mount() {
    SD_MMC.setPins(SDMMC_CLK, SDMMC_CMD, SDMMC_DATA);
    bool ok = false;
    for (uint8_t attempt = 1; attempt <= 3 && !ok; attempt++) {
        ok = SD_MMC.begin("/sdcard", true);
        if (ok) {
            USBSerial.printf("[sd] mounted on attempt %u\n", (unsigned)attempt);
            break;
        }
        USBSerial.printf("[sd] mount attempt %u failed\n", (unsigned)attempt);
        delay(80);
        SD_MMC.setPins(SDMMC_CLK, SDMMC_CMD, SDMMC_DATA);
    }
    if (!ok) {
        USBSerial.println("[sd] mount failed -- card seated?  CLK=2 CMD=1 D0=3  ALDO3=3V3");
        return false;
    }
    if (!SD_MMC.exists("/WATCH")) SD_MMC.mkdir("/WATCH");
    return true;
}

// v2.13d: drop the card when the panel goes black.  The face wallpaper is a
// decoded lv_img_dsc_t in PSRAM and every article / puzzle / app path is in
// flash, so nothing on the dark screen needs the card.  g_sd_ok is left alone:
// it means "this watch has a working card", which is still true while asleep.
static bool g_sd_asleep = false;
static void sd_sleep(const char *why) {
#if SD_SLEEP_WHEN_DARK
    if (!g_sd_ok || g_sd_asleep) return;
    if (g_pttListening) return;          // never pull the bus from a live session
    SD_MMC.end();
    g_sd_asleep = true;
    USBSerial.printf("[sd] unmounted (%s) - card rail idle\n", why ? why : "dark");
#endif
}
static void sd_wake() {
    if (!g_sd_asleep) return;
    g_sd_asleep = false;
    if (sd_mount()) {
        g_sd_ok = true;
        USBSerial.println("[sd] remounted after wake");
    } else {
        g_sd_ok = false;
        USBSerial.println("[sd] remount failed - Settings > Cards > Rescan");
    }
}

void setup() {
    USBSerial.begin(115200);

    // Are we coming back from a deep sleep (Sleep Now)?  If so, the internal
    // RTC still has the wall clock, so skip splash + WiFi for a fast wake.
    bool from_deep_sleep =
        (esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_EXT1);

    USBSerial.printf("\n=== LauncherOS v7.0-ramfix (merged: themes + card settings + styles + carousel loader) ===  %s\n",
                     from_deep_sleep ? "(wake from deep sleep)" : "(cold boot)");
    if (from_deep_sleep) {
        // Deep sleep is a reset, so this is the only record of which button
        // woke us.  A boot loop caused by a stuck LOW wake line shows up here
        // as a repeating "PWR" with no button pressed.
        uint64_t who = esp_sleep_get_ext1_wakeup_status();
        USBSerial.printf("[pm] deep-sleep wake: %s%s\n",
                         (who & WAKE_BOOT_ONLY) ? "BOOT" : "",
                         (who & (1ULL << PWR_WAKE_PIN))
                             ? ((who & WAKE_BOOT_ONLY) ? "+PWR" : "PWR") : "");
    }
    // Build stamp + the two numbers that decide whether voice can start.
    // If this line is missing from the monitor, the image on the watch is not
    // this source -- re-upload before drawing conclusions about RAM.
    USBSerial.printf("[boot] built %s %s\n", __DATE__, __TIME__);
    USBSerial.printf("[boot] psram %s (size %u B, free %u B)  internal free %u B, largest %u B\n",
                     ESP.getPsramSize() ? "OK" : "** MISSING **",
                     (unsigned)ESP.getPsramSize(), (unsigned)ESP.getFreePsram(),
                     (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                     (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
    if (!ESP.getPsramSize())
        USBSerial.println("[boot] ** PSRAM not detected: Tools > PSRAM = OPI PSRAM "
                          "(QSPI on modules with quad PSRAM), Flash Size = 16MB. "
                          "Without it the 49 KB draw buffer and the LVGL pool fall "
                          "back into internal RAM and voice cannot start. **");
    // v7.0: pull persisted preferences BEFORE we build any screen so the
    // very first paint uses the right theme + launcher style.
    theme_load_from_nvs();
    launcher_style_load_from_nvs();
    // v1.6: power settings must exist before build_screen_settings() seeds the
    // profile / timeout / night rows (pm_init() arms the runtime later, once
    // the panel and PMU are up).
    pm_load();
    // v6.0: re-anchor the bootloader on ourselves so any loaded app that
    // reboots (intentionally, via crash, or via power-cycle) comes back
    // to LauncherOS instead of into the last-flashed OTA slot.
    // v2.0 rule 5: this is the other writer of otadata.  It must not run while a
    // transaction is live, or a bootloader fallback would re-point the boot slot
    // at factory before boot_resolve() can see what the record says.
    if (bk_phase() == BK_IDLE) anchor_launcher_as_boot();
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

    // Which audio codecs answer on this bus.  The mic is an ES7210 (capture)
    // and the speaker an ES8311 (output); both must be present for voice, and
    // a missing ES7210 is the one failure that shows up as "-99 dBFS".
    mic_probe_report();

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
    // LVGL 8.4 partial draw buffer: a 60-line RGB565 strip (lv_color_t units).
    // v1.3-voice: this 49,200 B strip comes from PSRAM.  Only the flush callback
    // touches it, and the S3's SPI DMA reads PSRAM directly, so the cost is
    // bandwidth instead of a refused PTT.
    //
    // If there is no PSRAM (wrong Tools > PSRAM setting, or a module without
    // it) the 60-line strip is 49 KB of the internal RAM voice needs.  In that
    // case fall back to a 12-line strip -- 9.8 KB, still one strip per flush,
    // just more flushes -- instead of taking the 49 KB out of the voice budget.
    size_t lines = ESP.getPsramSize() ? 60 : 12;
    size_t bufPx = LCD_WIDTH * lines;
    const char *bufWhere = "PSRAM";
    lvgl_buf = (lv_color_t *)heap_caps_malloc(bufPx*sizeof(lv_color_t),
                                              MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
    if (!lvgl_buf) {
        lines  = 12;                     // PSRAM refused it: shrink before retry
        bufPx  = LCD_WIDTH * lines;
        bufWhere = "internal (no PSRAM!)";
        lvgl_buf = (lv_color_t *)heap_caps_malloc(bufPx*sizeof(lv_color_t),
                                                  MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT);
    }
    if (!lvgl_buf) lvgl_buf = (lv_color_t *)heap_caps_malloc(bufPx*sizeof(lv_color_t), MALLOC_CAP_8BIT);
    USBSerial.printf("[lcd] draw buffer %u lines, %u B in %s\n",
                     (unsigned)lines, (unsigned)(bufPx * sizeof(lv_color_t)), bufWhere);
    ptt_heap("lvgl ready");          // internal headroom voice will start from
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

    // v1.1: restore the persisted panel level + clock format before the first
    // bright frame, so a user who dimmed the watch is not flash-banged on boot.
    {
        g_prefs.begin("dcw", true);
        int  b     = g_prefs.getInt("bright", DEFAULT_BRIGHTNESS);
        g_time_24h = g_prefs.getBool("t24", false);
        g_prefs.end();
        if (b < 20)  b = 20;
        if (b > 255) b = 255;
        g_brightness_target = (uint8_t)b;
    }
    build_splash();
    lv_timer_handler();                      // render the first splash frame
    fade_brightness(g_brightness_target, 260); // soft fade-up out of power-on black
    splash_progress(10, "init display");
    splash_progress(25, "mounting SD");
    bool sd_ok = sd_mount();          // v2.8: retried, ALDO3-powered slot
    g_sd_ok = sd_ok;
    // v2.0: the boot protocol resolves here -- after the mount (the image is on
    // the card) and before Wi-Fi, voice and the app scan, with nothing else
    // competing for flash or heap.  It is unconditional: repairing an orphaned
    // clone needs no card, only otadata.  Every phase is re-entrant and it
    // reboots to finish, so this call is the whole of the hand-off.
    swap_boot_report();          // which slot are we in, and is it the boot slot
    bk_table_report();           // v2.10: which app slots the chip actually has
    boot_resolve();
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
            // v2.10: WiFi stays OFF at boot, by request.  Nothing joins until
            // you ask for it -- Settings > Join a network > Sync time, the
            // Battery screen's WiFi button, or wifi_resync_if_due()'s 12 h
            // top-up.  The hardcoded network is still first in the candidate
            // list, so the sync path tries it before anything else.
            splash_progress(55, "wifi off (on demand)");
            USBSerial.println("[wifi] left off at boot, NTP syncs on demand");
        }
        splash_progress(74, "voice model");
        sd_provision_model();                     // runs only when 0x910000 is blank
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

    // v1.2: build the push-to-talk phrase table and probe the model partition.
    // Safe to do after the UI exists -- ptt_init() only touches GPIO + NVS-free
    // state, and reports a missing model instead of failing the boot.
    ptt_init();

    // v1.6: hand every power decision to the manager.  It applies the
    // profile's awake clock now that boot/WiFi/SD are done (80 MHz on Balanced
    // and Power Saver, 240 only on Performance) and adopts the panel level the
    // splash already faded up to.
    pm_init();
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

    // v1.2: push-to-talk.  Hold BOOT to open the mic, release to run what was
    // heard.  Runs before the swipe/PSU logic so a held button is never treated
    // as a touch or a screen toggle.
    ptt_service();

    // v1.5: always-on.  Runs after ptt_service() so a button press is handled
    // first, and reopens the mic automatically after every executed phrase.
    aon_service();

    // v1.7.1: the slot swap's cancel window and install.  Here, outside every
    // LVGL event, because stage 1 erases and writes flash and pumps the panel.
    swap_service();

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
        } else if (g_ws_open) {
            // v2.13: the WiFi screen is modal.  Without this a swipe loaded the
            // launcher screen underneath it, leaving a detached panel that the
            // wake path then restored to the wrong screen.
        } else if (time_card_is_open()) {
            // The manual-clock card is modal: it owns every gesture.  Swiping
            // down is the second way out of it (Cancel is the first), which
            // keeps it consistent with how Xander is dismissed.
            if (ev == SWIPE_DOWN) time_card_close(true);
        } else if (current_screen == SCREEN_BATTERY && ev == SWIPE_UP) {
            time_card_open();
        } else if (current_screen == SCREEN_FACE && ev == SWIPE_UP) {
            xander_enter();
        } else if (ev == SWIPE_UP) {
            // v2.14.3: UP means "home" everywhere it is not already spoken for
            // (the face opens Xander, the battery screen opens the manual
            // clock).  Before this, a vertical swipe on Apps or Settings did
            // nothing at all, and a gesture that silently does nothing reads as
            // a broken screen.
            goto_screen(SCREEN_FACE);
        } else if (ev == SWIPE_DOWN) {
            // ...and DOWN means the app list, so both axes answer on all four
            // screens instead of only the two the face happens to allow.
            goto_screen(SCREEN_APPS);
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

    // v1.9: the WiFi screen defers its blocking work (scan ~2 s, join ~12 s) to
    // here, never to the tap that asked for it.
    wifi_screen_service();

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
    // 5s/15s tick - refresh battery readings (PMU).  Battery % barely
    // moves in 3 s on a real pack, but it climbs visibly while charging,
    // so we poll fast (5 s) when on USB and slow (15 s) on battery.
    // Each PMU read is an I2C round-trip, so this alone removes ~270
    // CPU wakeups per hour while idle.
    //
    // v1.8: a dark panel keeps sampling, just slowly.  Before this, every
    // battery rule -- the low/critical latch, cell protection, charging
    // detection -- was dead whenever the screen was off, which is the state a
    // watch spends most of its life in: a pack could sit at 5 % all night
    // without the protection ever arming, and plugging in went unnoticed
    // until the next tap.  One I2C read per 5 min is invisible next to the
    // 250 ms light-sleep tick that is already running.
    static uint32_t tB = 0;
    uint32_t batt_period = screenIsOff ? PM_DARK_SAMPLE_MS
                                       : (g_charging ? 5000U : 15000U);
    if (now - tB > batt_period) {
        tB = now;
        pmu_refresh();
        pm_note_battery(now);              // v1.6: freshness for the battery policy
        if (!screenIsOff) {
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

    // v1.6: the power manager owns the screen-off countdown, the wake tap and
    // every brightness / CPU / battery decision.  It reads exactly the same
    // signals this loop used before (touchActive, the post-wake grace, a
    // command hold) so the v5.13 hysteresis behaviour is preserved intact --
    // see pm_service() for the state machine.
    pm_service(now);

#if POWER_SAVE
    // Periodic NTP re-sync (briefly turns WiFi back on, syncs, drops it).
    // pm_wifi_allowed() already covers screen-off and a blocked radio.
    if (pm_wifi_allowed()) wifi_resync_if_due();
#endif

#if POWER_SAVE && LIGHT_SLEEP_WHEN_OFF
    // When the screen is off, sleep the SoC until either:
    //   * BOOT pulls GPIO0 LOW, or the side POWER button makes the AXP2101
    //     pull GPIO10 LOW (ext1 wake -- INSTANT for both; the power button has
    //     to be here or it can only be noticed by the 500 ms poll)
    //   * the LIGHT_SLEEP_POLL_MS timer fires so we can poll TP_INT
    //     (FT3168 IRQ -- not RTC-capable on every S3 variant, so polled)
    if (screenIsOff) {
        esp_sleep_enable_ext1_wakeup(wake_mask(), ESP_EXT1_WAKEUP_ANY_LOW);
        esp_sleep_enable_timer_wakeup((uint64_t)LIGHT_SLEEP_POLL_MS * 1000ULL);
        esp_light_sleep_start();

        esp_sleep_wakeup_cause_t why = esp_sleep_get_wakeup_cause();

        if (why == ESP_SLEEP_WAKEUP_EXT1) {
            // BOOT or POWER -- INSTANT screen-on, no touch required.
            //
            // Consume the PMU latch BEFORE waking: the AXP2101 holds its IRQ
            // line LOW until the status register is read.  Left asserted, the
            // line would pull us straight back out of the next light sleep,
            // and the short-press poll below would then find the same event
            // and toggle the screen back OFF -- the watch would blink on and
            // die again, which looks exactly like "the button does nothing".
            uint64_t who  = esp_sleep_get_ext1_wakeup_status();
            // An empty status is treated as BOOT: never silently drop a wake.
            bool boot = (who == 0) || ((who & WAKE_BOOT_ONLY) != 0);
            bool pwr  = (who & (1ULL << PWR_WAKE_PIN)) != 0;
            bool pkey = false;
            if (g_has_pmu) {
                g_pmu.getIrqStatus();
                pkey = g_pmu.isPekeyShortPressIrq();
                g_pmu.clearIrqStatus();
            }
            if (pwr && !boot && !pkey) {
                // The line went low with no PKEY event behind it, so it is not
                // a power press and it cannot be trusted to sleep on.  Take it
                // out of the mask and go straight back to sleep rather than
                // lighting the panel for a wake nobody asked for.  BOOT stays.
                wake_drop_pwr("EXT1 wake with no PKEY event");
                return;
            }
            USBSerial.printf("[pm] ext1 wake: %s%s\n",
                             boot ? "BOOT" : "",
                             pwr ? (boot ? "+PWR" : "PWR") : "");
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
