# DualCore Special Editions — mini module

Three experiences, one firmware module, one `.ino`:

| File | What it is |
| --- | --- |
| [DualCoreSE.ino](DualCoreSE.ino) | The module: NoirOS, CrownOS, NightOS, shared services, chooser, alarms, diagnostics. |
| [LauncherOS_Demo.ino](LauncherOS_Demo.ino) | A minimal reference launcher: lists `/WATCH/bin/*.bin`, flashes one to the OTA slot, restarts. Separate sketch. |
| [ui-mockup.html](ui-mockup.html) | Interactive 410 × 502 UI mockup of every screen. Open it in a browser; no build step. |
| [lv_conf.h](lv_conf.h) | The LVGL 8.4 configuration. Lives **beside the sketch**; see [Build](#build). |

Target: Waveshare ESP32-S3 Touch AMOLED 2.06 (WOS 206A), 410 × 502, LVGL 8.4.
Module version **1.5.0**, manifest API 1.

---

## The contract it follows

**Boot pattern (LauncherOS / GameOS).** QSPI CO5300 panel, FT3168 touch with the
v5.13 release hysteresis, AXP2101 PMU, `SD_MMC`, a 60-line partial LVGL draw
buffer (internal RAM → PSRAM → 8-bit fallback), brightness fades, 15 s
screen-off, side-button screen toggle.

**Sleep, exactly like LauncherOS — plus a timer.** Panel off means the SoC
goes into **light sleep**: `ext1` on GPIO0 wakes it instantly from BOOT, and a
250 ms timer polls TP_INT for a touch (the FT3168 IRQ is not RTC-capable on
every S3 board). Light sleep keeps `millis()`, the RTC and the wake sources
alive, so countdowns and the schedule keep running with the screen dark. After
the configured idle time (`AUTO SLEEP 0/1/5/15/30 min`, default 5) the module
takes the LauncherOS **deep sleep** step instead: it saves everything, stores
the active experience in RTC memory, arms `ext1` on BOOT **and** a timer for the
next alarm, schedule edge or running countdown, then sleeps until one of them
fires. Deep sleep is a reboot, so `setup()` reopens the recorded experience
when the wake source is BOOT or the timer; a cold boot still lands on the
chooser. Deep sleep is refused while charging, while an alarm is ringing, a
session overlay or the flashlight is open, or while the countdown is running.

**Shared time block v2.** Copied verbatim from LauncherOS v7.1. The only two
lines that differ are the per-app knobs, set to:

```c
#define TIME_SERIAL Serial
#define TIME_SRC_ID TIME_SRC_MODULE
```

The block re-applies the timezone from NVS `rtcsave` on every boot, adopts a
better epoch when one exists, and publishes on exit. It is byte-identical to
the launcher's copy (verified with `diff`).

**Back to LauncherOS (GameOS protocol).** Hold BOOT for 1.5 s anywhere, or use
`TO LAUNCHER` on the chooser:

```c
time_publish("exit", false);
dcs_save_all();
dcs_boot_guard_success();
esp_ota_set_boot_partition(FACTORY);   // or the next update slot
esp_restart();
```

No SD access is needed to get home. There is also a three-strike boot guard
(NVS `dcseboot`): a module that dies before showing its chooser three launches
in a row anchors the factory partition and reboots instead of looping. The
guard is not a watchdog replacement: it counts launches that never reached a
usable UI, it does not detect a hung loop.

---

## Switching without rebooting

The module boots to the chooser, not straight into an experience. Choosing an
experience destroys the previous LVGL tree and builds the next one; the shared
services (inbox, clock, timers, alarms, link, power) live outside them, so
switching keeps your notifications, timers and alarms. `EXIT` in any experience
returns to the chooser; only `TO LAUNCHER` / BOOT-hold reboots.

`active_os` in the shared settings remembers the **last** experience and shows
a `LAST` marker on its card. It is not auto-launched at boot — a broken
experience must never be able to close the chooser on you.

### Each experience gets its own boot sequence

Choosing a card plays that OS's own sequence before its screen tree exists
(1.4.0). They are three different films, not one template with a new colour:

| | NoirOS | CrownOS | NightOS |
| --- | --- | --- | --- |
| palette | obsidian / ice blue | black lacquer / champagne | near-black / crimson |
| mark | `OS 01` | `OS 02` | `OS 03` |
| flourish | growing rule under the wordmark | spinning arc around a DualCore crown | scanline sweeping a bounded frame |
| stages | mount, settings + tiles, shared inbox, build | mount, face library, occasion, case + dial | mount, shadow profile, quiet policy, night surface |

Each stage names work that has already happened by the time it appears, so the
bar is a report rather than a decoration. The sequence is driven by an
`lv_timer` at 40 ms, never a blocking delay: touch stays live throughout, a tap
skips to the end, and a slow SD card cannot wedge the UI. Full sequence ≈ 2.0 s,
quick ≈ 0.4 s.

`SYSTEM → BOOT SEQUENCE` cycles **FULL / QUICK / OFF** (persisted as
`bootanim` in the shared settings). Waking from deep sleep always takes the
quick form whatever the setting says, because a wake should feel instant, and
`OFF` builds the target tree directly with no intermediate screen at all.

---

## Storage map

Everything the module writes is under `/WATCH/`:

```
/WATCH/SHARED/
  settings.json      brightness, power profile, 12/24h, privacy, auto-power,
                     last OS, sleep idle, theme accent, wallpaper, auto-NTP
  wifi.txt           optional; same keys as LauncherOS's /WATCH/wifi/config.txt
  inbox.jsonl        shared notification inbox (one record per line)
  timers.json        countdown anchor + session count + stopwatch base
  alarms.json        three daily alarms (hour, minute, enabled)
/WATCH/NoirOS/
  manifest.json      id "noir", features, min_firmware_api
  config/settings.json
  data/world_clocks.jsonl
  data/focus_sessions.jsonl
  logs/diag.log, logs/power.log
/WATCH/CrownOS/
  manifest.json      id "crown"
  config/settings.json   face, profile, accent, bg, complications, anim, seconds hand
  data/saved_faces.jsonl
  data/events.jsonl
  logs/…
/WATCH/NightOS/
  manifest.json      id "night"
  config/settings.json   shadow, brightness, quiet, schedule, palette
  logs/…
```

Configs are flat JSON (read by a small in-house reader; no ArduinoJson
dependency). Line-oriented runtime records use `.jsonl` so they can be appended
and bounded without rewriting. Writes go through a temp file + rename, so a
yanked card cannot leave a half-written config. Missing files are recreated;
corrupt files fail back to defaults for that file only — the module never wipes
an OS directory.

NVS holds only two things: `rtcsave` (the shared time contract) and `dcseboot`
(the boot guard counter).

---

## What each experience does

**NoirOS** — chooser → `HOME` / `INBOX` / `TOOLS` / `DAY`.
Large clock or analogue dial (dials only redraw when the second changes), date,
battery and link state, a **three-event agenda**, four shortcut tiles. Inbox
with unread markers, privacy-aware presentation, detail modal with `DISMISS`,
per-row `X`, `READ ALL`, `CLEAR READ`, overflow badge. Tools: phone media
control, focus countdown with ±5 min **and 5 / 15 / 25 / 45-minute presets**,
monotonic stopwatch, world clock (six cities, fixed offsets), shared quick
settings and a `SIMULATE NOTIFICATION` button so the inbox can be exercised
with no phone. **DAY** (1.4.0) is the numbers tab: a twelve-dot water counter
(`-1` / `+1 GLASS` / `RESET`), screen-on time, focus minutes, notifications
received and battery-against-boot, then `DIAGNOSTICS` and `FOCUS TOOLS`
shortcuts. Counters are measured from the shared services, not guessed from
the UI, and every one of them rolls over at local midnight.

**CrownOS** — `FACE` / `STUDIO` / `EVENTS` / `ALMANAC`.
**`FACE` is the whole watch.** No header, no tab bar, no card: the dial is drawn
edge to edge at 410 × 502 and there is nothing on screen but the watch. **Tap**
the dial and the chrome slides in over it — battery, link, `HIDE`, exit and the
four tabs — without the face being rebuilt or cropped; tap again (or swipe down)
to dismiss it. **Swipe left/right** on the face pages straight to Studio, Events,
Almanac and back. The gestures are hand-rolled from press/release deltas, so they
do not depend on `LV_INDEV_DEF_GESTURE_LIMIT` or the gesture module being on.

Eleven face styles, all drawn from primitives, so no asset can break a face:
`CLASSIC`, `MINIMAL`, `DUAL TIME`, `ROMAN`, `MOON`, `SOVEREIGN`,
**`SERPENT`** (black lacquer, guilloché, a raised gold coil), **`CHRONO`**
(ivory dial, tachymeter, three registers, black hands), **`SKELETON`**
(exposed movement — bridges, four-gear train, jewels), **`DIVE`** (chunky
bezel, lume pip, red sweep), **`PHOTO`** (your own dial image).

The **dial glitch** is the one piece of motion on the face: every so often the
dial tears for five frames — seven offset colour slices plus a two-pixel
tracking shift — then snaps back. `SYSTEM → STUDIO` sets `OFF` / `RARE`
(~12 s) / `OFTEN` (~5 s); it never fires in Eco or Survival, with animations
off, or while the panel is asleep.

`PHOTO` reads `/WATCH/CrownOS/assets/faces/dial.raw` — RGB565 little endian,
exactly 410 × 502 — into PSRAM (≈402 KB) only while a photo face is on screen,
and frees it the moment you leave. No file, or a short file, falls back to a
drawn classic face and says `NO DIAL IMAGE` rather than showing a black screen.

Face definitions are data: style, accent, background, complications. Hands are
LVGL lines and only move when the second changes; the moon face has no hands at
all. The **seconds hand can be hidden**, which removes the only per-second
redraw. Long-press the face (or tap the STUDIO tab) to customise: five accents,
four backgrounds, complications, signature animation, seconds hand, dial
glitch, save-as-new (bounded to 16 faces) and overwrite for user faces only. Four occasion profiles
(Formal, Celebration, Everyday, Travel) set face, palette and power profile;
Formal also forces sender-only notification previews through the shared
presentation layer, not the UI. Preset events (+2 h … +7 d, bounded to 8) with
live countdowns, each deletable. The **moon-phase complication** is computed
from a known new moon with a circular terminator — approximate on purpose, and
no asset or network involved. **ALMANAC** (1.4.0) is the horology tab: a large
moon with phase name, illumination and days to full/new; a perpetual calendar
reading weekday, `dd MONTH yyyy`, ISO week, day-of-year out of 365/366, days
left and leap-year state; a **power reserve** subdial (ticked scale, colour
shift below 20 %, measured runtime estimate, 8-hour min/max from the battery
history); and the next engagement with its live countdown. With no valid clock
it says `Clock not set` and explains how to fix it instead of printing 1970.

**NightOS** — `HOME` / `SHADOW` / `TOOLS` / `OPS`.
Shadow Mode reduces the NightOS brightness target, suppresses decoration,
forces at least sender-only previews, and can be driven by a weekday schedule
(start/end hour, day mask) that uses the shared clock. A **crimson palette**
retints the whole OS — it swaps the theme colours rather than layering a
translucent filter, so touch routing and compositing cost stay unchanged.
Utilities: display-based flashlight (max brightness white overlay — this board
has no flashlight LED), **wake alarm** (shared alarm service), an interruptible
**4-7-8 breathing pacer** that holds the panel awake only while open, power
profile, stopwatch, countdown, phone media, and an IMU panel. No compass: the
board has no magnetometer, and the UI says so.

**OPS** (1.4.0) is the field tab: a **tripwire** that arms the IMU at 10 Hz
(`LOW` / `MED` / `HIGH`) and raises a full-screen alert on movement, with a
live alert count; `GO DARK` (panel off, BOOT wakes); and a **night log** of
minutes in Shadow, screen-on time, notifications and Shadow switches this
session, closing with the measured runtime estimate. The tripwire is
deliberately RAM-only — arming a motion alarm and then letting deep sleep
disable it silently would be worse than not having it — and with
`DCS_USE_SENSORLIB 0` the tab reports `NO IMU` instead of pretending.

**Shared overlays** — a long-press on any experience's top bar opens the
**quick sheet**: brightness, power profile, privacy, alarms and screen-off,
without leaving what you were doing. Alarms (three, daily with weekday masks)
ring over the chooser *and* every experience with `SNOOZE 5` / `DISMISS`;
edits save immediately. The alarm overlay wakes the display and stays on top.

---

## System (the LauncherOS conveniences, shared)

The quick sheet's last row opens **SYSTEM**, which owns the things that do not
belong to one experience:

- **WI-FI / CLOCK** — NTP with the same credentials file LauncherOS reads
  (`/WATCH/wifi/config.txt`) or a module-local `/WATCH/SHARED/wifi.txt`:

  ```
  WIFI1_SSID=MyNetwork
  WIFI1_PASS=secret
  GMT_OFFSET=1          # hours, negative or fractional allowed
  ```

  `SYNC NOW` queues a sync; `net_service()` runs it from `loop()`, never from an
  LVGL event, pumping the UI while the radio connects. It re-pins the TZ string,
  calls `configTime`, publishes through the shared time block, then drops the
  radio. Auto-NTP tops the clock up when it is unset (retry back-off 15 min) and
  every six hours while the panel is on, and never interrupts a live BLE phone
  link. UTC offset is adjustable in 30-minute steps without a `wifi.txt`.
- **THEME ACCENT** — one accent across all three experiences: champagne, ice
  blue, mint, violet, crimson, platinum, or `OS DEFAULT` (each OS's own palette).
  It recolours the accent and warning roles only; NightOS's crimson palette and
  CrownOS's per-face accents still work on top.
- **NOIR WALLPAPER** — `410×502` RGB565 little-endian `.raw`/`.bin` frames from
  `/WATCH/PHOTOS`, the same export LauncherOS uses. The frame lives in PSRAM and
  is released when the wallpaper returns to OFF; it is recoloured to black at
  47 % opacity so the clock and tiles stay readable.
- **DEVELOPER INFO** — version, build flags, heap/PSRAM free and minimum, SD
  card, reset reason, wake reason, uptime, boot strikes, inbox/alarm/city/event
  counts, power, accent, wallpaper and clock source.
- **BOOT SEQUENCE** — `FULL`, `QUICK` or `OFF` for the per-experience launch
  films described above. Persisted in the shared settings, so it applies to all
  three; deep-sleep wake always uses the quick form regardless.
- **FACTORY RESET** — per experience, or all three plus `/WATCH/SHARED`. Always
  confirmed, never automatic, and it recreates manifests with defaults instead
  of leaving a folder the module cannot launch. The chooser stays reachable
  throughout, and the firmware partition is never touched.

---

## Power

One manager owns brightness and profiles (`Performance`, `Balanced`, `Eco`,
`Survival`). Experiences request; the manager applies. Low-battery auto-switch
uses hysteresis at 30 % / 15 % with release at 35 % / 20 %, and only runs when
`auto_power` is on. Battery is polled once every 5 s by the module, never per
screen. Diagnostics (chooser → `DIAGNOSTICS`) shows heap, PSRAM, battery, reset
reason, link and SD state, and `WRITE SNAPSHOT` appends one metrics line to
`logs/power.log`. Notification content is never logged.

Power-relevant additions are all opt-in: hiding the seconds hand removes a 1 Hz
redraw, the moon face never animates, the crimson palette costs nothing per
frame, and the breathing pacer (the only continuous animation) stops the moment
it is closed or the experience is left. The wallpaper is a single static frame,
not a redraw, and NTP is a short radio window rather than a permanent Wi-Fi
connection.

Sleep is where the current actually goes: panel-off **light sleep** is the
LauncherOS behaviour (BOOT wake is instant, the timer poll costs one wake per
250 ms), and idle **deep sleep** drops the radio, the panel and the CPU, waking
on BOOT or the next alarm/schedule/countdown. Deep sleep is deliberately
refused while charging, while an alarm rings, while the flashlight or a session
overlay is open, or while a countdown is running, so a feature the user asked
for is never silently cancelled. **These current figures still need the Power
Lab pass on hardware**; the module reports state, not calibrated microamps.

---

## Build

1. Copy `DualCoreSE.ino` into its own sketch folder (name it `DualCoreSE`).
2. Keep [lv_conf.h](lv_conf.h) **beside `DualCoreSE.ino`**. The sketch does
   `#include "lv_conf.h"` before `<lvgl.h>`, and LVGL resolves that same file
   for its own compilation — verified, `lv_mem.c.o` in the build links
   `ps_malloc`/`psramFound` and carries no 48 KiB `.bss` array. One file, one
   set of struct layouts. The same file is needed in the `LauncherOS` sketch
   folder too; if you keep a shared copy in `Documents/Arduino/libraries/` it is
   only reached by sketches that have none of their own, so it must stay
   byte-identical — a stale copy configures a different ABI and fails as heap
   corruption, not as a compile error.

   What it changes from the stock 8.4 template:

   | Setting | Value | Why |
   | --- | --- | --- |
   | `LV_MEM_SIZE` | **128 KiB in PSRAM** (`LV_MEM_POOL_ALLOC`) | The pool is one `ps_malloc()` block, not a static array, so internal RAM is freed rather than spent. The stock 48 KiB template costs 48 KiB of `.bss` — the same RAM Wi-Fi and BLE need. Raise the size freely; only PSRAM pays. |
   | `LV_DISP_DEF_REFR_PERIOD` / `LV_INDEV_DEF_READ_PERIOD` | 20 / 20 ms | 50 fps ceiling. 100 fps (template) burns current redrawing nothing; 33 fps made the crown second-file step. Touch is INT-driven, so 20 ms adds no latency. |
   | Fonts | Montserrat 10–48, twelve sizes | Every size the two sketches reference and nothing else. No flash win — the linker already discards unreferenced fonts — but the list stops lying about what is used. |
   | Demos, examples | off | Unreferenced either way; off so nothing suggests they ship, and a demo build cannot creep in. |
   | `LV_USE_LOG` | `-DDCS_LV_LOG=1` per build | WARN+ through `printf`, including allocation failures. |
   | `LV_DPI_DEF` | 130 (unchanged) | The UI is laid out in exact pixels; the real ~315 dpi would inflate every default padding by 2.4×. |

   Widgets stay enabled on purpose: only `obj`, `label`, `line`, `btn`, `img`
   and `timer` are used here, but LauncherOS, GameOS and SpotifyOS build against
   the same install, and flash is at 32 %.
3. Libraries: Arduino_GFX, Arduino_DriveBus, XPowersLib, LVGL 8.4.
4. Build for the WOS 206A (ESP32-S3, 16 MB flash, 8 MB PSRAM), then place the
   resulting `DualCoreSE.ino.bin` in `/WATCH/bin/`.

Optional flags at the top of the file:

| Flag | Default | Effect |
| --- | --- | --- |
| `DCS_USE_BLE` | `0` | Compiles the NimBLE NUS-style phone transport. **Unverified on hardware.** |
| `DCS_USE_SENSORLIB` | `0` | Compiles the QMI8658 read path via `SensorLib`. |

### Phone link

With `DCS_USE_BLE 0` the module uses a demo source: the link comes up shortly
after boot, media metadata is local, and notifications are injected by the
`SIMULATE NOTIFICATION` buttons. This is deliberate — the UI must be provable
with no phone.

With `DCS_USE_BLE 1` the adapter exposes a NUS-like service
(`6E400001-…`, RX `…0002`, TX `…0003`, name `DualCore SE`) carrying
newline-delimited JSON:

```
phone -> watch
  {"t":"notif","pid":"a1","app":"Messages","sender":"Alex","title":"Hi","body":"…"}
  {"t":"notifrm","pid":"a1"}
  {"t":"media","playing":true,"title":"Song","artist":"Artist"}
  {"t":"time","epoch":1700000000,"gmt":28800}

watch -> phone
  {"t":"media","cmd":"toggle"|"next"|"prev"}
```

BLE callbacks only enqueue complete lines; parsing, inbox mutation and SD writes
run on the loop task. That shape is the seam for a real Gadgetbridge-compatible
transport. Proving it against an actual Android phone is still milestone 1 of
the spec; treat the BLE path as a skeleton until that test exists.

---

## Compile check

Both sketches build for `esp32:esp32:esp32s3` with
`FlashSize=16M,PSRAM=opi,PartitionScheme=dualfive` (the DualCore 5 MB / model
scheme already installed on this machine):

| Sketch | Flash | Static RAM |
| --- | --- | --- |
| `DualCoreSE` v1.5.0, pool in PSRAM (**this config**) | **1,745,611 B (33 %)** | **81,372 B (24 %)** |
| `DualCoreSE` v1.4.0, pool in PSRAM | 1,737,771 B (33 %) | 80,172 B (24 %) |
| `DualCoreSE` v1.3.0, pool in PSRAM | 1,722,791 B (32 %) | 79,964 B (24 %) |
| `DualCoreSE` v1.3.0, stock 48 KiB static pool | 1,722,731 B (32 %) | 129,116 B (39 %) |
| `LauncherOS_Demo` | ~809 KB (15 %) | ~198 KB, minus the same 48 KiB pool (not re-measured) |
| `DualCoreSE` + `DCS_USE_BLE=1` + `DCS_USE_SENSORLIB=1` (v1.0.0 baseline) | ~1002 KB (19 %) | ~220 KB minus the same 48 KiB |

1.4.0 costs 14,980 B of flash and 208 B of static RAM: three boot skins with
their strings, the tripwire, the battery history ring and the three new tabs.

1.5.0 costs a further 7,840 B of flash and 1,200 B of static RAM: five more
face styles with their drawing code (the serpent coil, the gear train, the
tachymeter and dive bezel), the swipe/tap gesture handling, the glitch engine
and the photo-dial loader. The 1,200 B of RAM is the static point arrays the
LVGL lines point into — `fc_minutes` alone is 60 × 2 points, and those cannot
live on the stack because `lv_line` keeps the pointer. The pool is unchanged.

The 48 KiB difference is the entire effect of [lv_conf.h](lv_conf.h): the pool
moved from `.bss` into PSRAM and grew from 48 KiB to 128 KiB at the same time.
`lv_mem.c.o` in the build references `ps_malloc`/`psramFound` and has no
`work_mem` array, and the linked image carries no `work_mem` symbol either — so
the runtime heap on DEVELOPER INFO is the honest number to watch, not the static
one. Flash did not move: the 60 B delta is alignment noise, because the linker
was already dropping the unreferenced fonts and demos.

Wi-Fi is the expensive addition: the station driver plus `configTime` is about
570 KB of flash (v1.2.0 was 1,151,476 B). It fits with room to spare in the
5 MB `dualfive` slot, and it buys a real wall clock with no phone and no
launcher. The BLE + SensorLib pair still needs a hardware re-measure.

Toolchain: ESP32 core 3.3.12, LVGL 8.4.0, XPowersLib 0.2.6, GFX 1.6.9,
NimBLE-Arduino 1.4.0, SensorLib 0.3.3.

## UI mockup

[ui-mockup.html](ui-mockup.html) is a self-contained, interactive mockup at the
real 410 × 502 pixel size: chooser, all three OSes, quick sheet, system sheet,
Wi-Fi/clock, developer info, factory reset, alarm editor, alarm ring, breathing
pacer, diagnostics and notification detail. It mirrors the firmware's layout
grid (58 px top bar, 378 px body, 66 px tab bar) and every 1.5.0 feature is
clickable — the accent row really retints all three OSes, the wallpaper row
redraws NoirOS, `SYNC NOW` runs a (simulated) NTP window, and dismissing a
notification really removes it. The three boot sequences play at the firmware's
real 40 ms cadence and a tap skips them, the water dots on `DAY` are live, and
the `ALMANAC`, `OPS` and boot-sequence rows behave like the device.

The CrownOS face is drawn in the mockup as SVG at the same geometry the
firmware uses (centre 205/250, case 190, aperture 174). All eleven styles are
there, tap really toggles the chrome, swipe really pages, and the dial glitch
teases on the same schedule — `RARE` every 12 s, `OFF` never. Unlike the
device, the mockup can show the glitch on demand, which is why a screenshot is
not a substitute for looking at it.

Type rendering and anti-aliasing differ from LVGL on the watch; geometry and
interaction model do not. It is a design/interaction reference, not a build
artifact and not shipped to the device.

## Known limits

- Alarms carry a weekday mask, but they need the module running: powered off,
  nothing can ring (deep sleep can, because it arms a timer first). Snooze is
  RAM-only, so a reset drops the snooze but never the alarm.
- Deep sleep loses RAM: timers are restored from disk, the stopwatch does not
  survive it, and the experience is rebuilt on wake rather than resumed.
- Wi-Fi is 2.4 GHz only (the S3 has no 5 GHz radio). A failed NTP attempt backs
  off 15 minutes; there is no captive portal or on-watch keyboard, so
  credentials are edited on the card.
- Wallpapers and the `PHOTO` dial are raw RGB565 only (no JPEG/PNG decode) and
  cost one 402 KB PSRAM buffer while selected. Convert with
  `ffmpeg -i in.jpg -vf scale=410:502 -f rawvideo -pix_fmt rgb565le dial.raw`
  and copy it to `/WATCH/CrownOS/assets/faces/dial.raw`.
- The face swipes only on the FACE page. Studio, Events and Almanac are ordinary
  tabbed pages, so getting back to the watch is one tap on `FACE` (or a
  long-press on the dial anywhere).
- The breathing pacer holds the screen awake on purpose; it stops the 15 s
  screen-off timer while it is open.
- Moon phase uses a mean synodic month and a circular terminator: good enough
  for a complication, not an ephemeris.
- The tripwire is RAM-only by design (`HIGH` = ~0.12 g of change at 10 Hz): it
  reports *is it moving*, not *what moved it*, and arming it costs battery at a
  rate Power Lab has not measured yet. It is also silently disarmed by a reboot
  or deep sleep — the tab says so rather than claiming persistence.
- The boot sequences are a UI event, not a second firmware: they cannot detect
  a broken experience before it loads, so recovery still relies on the boot
  guard and the chooser rather than on the film.
- Daily counters are wall-clock dated: with no valid clock they read `----` and
  the water/focus/on-time totals wait for a real date instead of guessing.
- The crimson palette is a theme change, not a colour filter; it cannot make
  photos or notification icons red.
- No vibration motor is exposed on this board (LauncherOS's haptics toggle is a
  placeholder too), so timer and alarm cues are visual/log only.
- No on-screen keyboard: events and custom faces use preset names.
- World clock uses fixed UTC offsets (no tz database in flash), and the UI says
  `fixed UTC offset`.
- Stopwatch is monotonic and resets with the chip; the countdown is wall-clock
  anchored and survives a reset. Both are stated on screen where it matters.
- The night schedule cannot run a watch that is powered off; it does arm a
  deep-sleep timer for its own start/end edge, and the Shadow tab says so.
- Boot always lands on the chooser.
- The BLE transport and the SensorLib IMU path are compile-checked by
  construction but **not** hardware-verified here.

---

## Quick test pass

1. Boot → chooser appears, fades up, battery and date correct; the status line
   shows unread count, next alarm and link state.
2. Open each experience, change a setting, `EXIT`, reopen: the setting stuck.
3. In NoirOS, `SIMULATE NOTIFICATION`, switch to CrownOS, come back: unread
   count and content survive; Formal profile shows sender only.
4. Inbox: open an alert (marks read), `DISMISS`, then `X` another, then
   `CLEAR READ` — counts and the empty state follow.
5. Long-press the top bar in any experience → quick sheet; change brightness
   and profile, close, reopen: values persisted (they are the shared settings).
6. Alarm editor: enable 07:00, wait for 07:00 (or set the time near it) → the
   overlay rings over whatever was open; `SNOOZE 5` re-rings in five minutes,
   `DISMISS` clears it; reboot and the alarm is still enabled.
7. CrownOS: apply a face, hide the seconds hand, save a copy, overwrite it,
   reboot: selections persist. `LUNE` shows the current moon phase.
8. CrownOS events: add a preset, delete one with `X`, reboot: the list matches.
9. NightOS: Shadow on/off changes brightness; palette switches to crimson;
   schedule edits persist; flashlight restores brightness on tap; breathe opens
   and `STOP` returns to the utility panel.
10. Hold BOOT 1.5 s from inside any experience → LauncherOS demo comes back with
    the clock intact (no `00:00`).
11. Pull the SD card while running: the UI keeps working from RAM and logs stop.
12. Fill the inbox past 24 records: oldest read entries drop, `OVERFLOW` shows.
13. Let the panel time out: the watch answers a tap in well under a second
    (light sleep), and BOOT wakes it too. Set `AUTO SLEEP 1 MIN`, leave it idle:
    the module deep-sleeps; BOOT brings back the same experience, with the clock
    and inbox intact. Set an alarm two minutes out and repeat — the timer wakes
    the watch and the alarm rings without a button.
14. SYSTEM → WI-FI / CLOCK with a real `wifi.txt`: `SYNC NOW` reports synced,
    UTC offset changes the displayed time, the radio is off afterwards, and the
    clock survives a reboot without NTP.
15. SYSTEM → THEME ACCENT: every experience's accent follows; `OS DEFAULT`
    restores NoirOS blue, CrownOS gold and NightOS crimson.
16. SYSTEM → NOIR WALLPAPER with a raw frame in `/WATCH/PHOTOS`: NoirOS home
    shows it dimmed behind the tiles; set OFF and PSRAM is released (watch the
    PSRAM figure on DEVELOPER INFO).
17. SYSTEM → FACTORY RESET → CROWNOS: confirmation, then CrownOS is back to its
    defaults while NoirOS and NightOS keep their settings and the inbox survives.
18. Launch each experience from the chooser: three different sequences play (ice
    rule / spinning crown arc / crimson scanline), the stage text advances with
    the bar, and a tap mid-way lands on the target at once without a reboot.
    Set `BOOT SEQUENCE` to `OFF` and repeat: no intermediate screen appears.
19. NoirOS → `DAY`: tap `+ 1 GLASS` three times, reboot, reopen: three dots.
    `RESET` clears them. Screen-on time and notification count move as you use
    the watch.
20. CrownOS → `ALMANAC`: the moon phase matches the date, the week / day-of-year
    figures change at midnight, and the power reserve shows the 8-hour min/max
    after the watch has run for an hour or two.
21. NightOS → `OPS`: with `DCS_USE_SENSORLIB 0` the tripwire reports `NO IMU`
    and offers nothing fake; with it enabled, arm the tripwire and move the
    watch — the alert covers the screen, and `GO DARK` sleeps the panel until
    BOOT.
