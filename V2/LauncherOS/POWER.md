# LauncherOS power management (v2.1)

Everything here lives inside `LauncherOS.ino`. No new files, tasks or libraries
were added, and every new hardware call goes through objects the launcher
already owned (`gfx`, `g_pmu`, `g_prefs`, `FT3168`).

## Where it is

| Section | Lines (approx) |
| --- | --- |
| State enum + profile types, before the first function | 389 |
| Configuration constants, profile table, runtime state, settings | 680 |
| Power manager implementation (state machine, brightness, CPU, battery, night, notices, diagnostics) | 3719 |
| Settings rows | 4424 |
| `setup()` / `loop()` integration | 8065 / 8282 |

The Arduino builder injects generated prototypes before the first function
definition, so any type used in a `static` function signature (`PmState`,
`PmProfileDef`) has to be declared above that point. That is why the enum and the
profile struct sit in the tunables block rather than beside their code.

## States and priority

One state is active at a time. Highest priority wins:

| State | Enters when | Brightness | Wallpaper | CPU | WiFi |
| --- | --- | --- | --- | --- | --- |
| `SCREEN_OFF` | no activity for the timeout | 0 | — | `cpu_off` | blocked |
| `CRITICAL` | on battery, ≤ 7 % | 20 | black | active | blocked |
| `LOW_BATT` | on battery, ≤ 20 % | profile `low_bright` | black | active | blocked |
| `NIGHT` | manual toggle, or 22:00–07:00 | profile `night_bright` | black | active | allowed |
| `CHARGING` | AXP2101 reports charging | user level | shown | active | allowed |
| `ACTIVE` | anything else | user level | shown | `cpu_active` | allowed |

`ACTIVE` covers the entire screen-on window: there is no auto-dim (v2.1, below).
`PM_IDLE` still exists in the enum and the table still carries `idle_bright` /
`cpu_idle` columns, but nothing enters it — the only brightness changes while
the panel is lit come from `night`, `low`, `critical` or the user's slider.

Order in `pm_desired_state()`: a dark screen outranks everything, the battery
latches outrank night, and night outranks charging (so plugging in overnight
does not jump to full brightness).


`screen_held()` (a voice command, app copy or update screen in flight) forces
`ACTIVE` even at night or on a drained pack, and pauses the idle countdown.

Deep sleep is deliberately **not** one of these states: it is an exit from
`SCREEN_OFF` (see Auto deep sleep below), so the state machine never has to
reason about a mode it cannot return from without a reboot.

## Profiles

| Profile | idle | night | low | active | idle | off | WiFi |
| --- | --- | --- | --- | --- | --- | --- | --- |
| Power Saver | 45 | 25 | 30 | 240 MHz | 80 | 80 | no |
| Balanced *(default)* | 70 | 35 | 40 | 240 MHz | 80 | 80 | yes |
| Performance | 100 | 50 | 50 | 240 MHz | 160 | 80 | yes |

Brightness caps are applied as `min(user_target, cap)`, so a watch dimmed to 60
wakes at 60 in every state — the user's level is always the ceiling.

### No auto-dim (v2.1)

The profile table has no `idle_after_ms` column any more, and `pm_desired_state()`
no longer has a branch that returns `IDLE`. A lit panel keeps the user's
brightness for the entire screen-on window and goes straight to black at the
timeout, which is what the pre-v1.6 firmware did.

The dim was removed rather than retuned because it read as the watch falling
asleep mid-use: the dim state also hides the wallpaper, so the panel dropped to
a few glyphs on black and every glance ended there. Power Saver's 4 s value was
the worst case, but a dim step in the middle of the window is wrong at any value.

**Every profile now runs the SoC at 240 MHz while the panel is lit.** The 80 MHz
active clock was the other half of the same complaint: LVGL and the QSPI flush
both ride this core, so a 3× downclock is a 3× slower UI, and the panel (~145 mA)
dwarfs the 10–15 mA the downclock saved. The clock still drops to 80 MHz once the
panel is dark, and `screen_on()` restores 240 MHz *before* its repaint and fade,
so a wake is never rendered at the sleep clock.

### Quiet face (v1.8)

In `NIGHT`, `LOW_BATT` and `CRITICAL` the wallpaper is not drawn: the
clock sits on black. AMOLED emission is per pixel, so at an identical brightness
level a full-screen photo draws several times the current of a few glyphs, and
black pixels draw none. This is why the panel is 68 % of the bill — brightness
sets the gain, but lit area sets what there is to amplify.

Nothing about the user's wallpaper changes: `refresh_wallpaper()` is the single
place that decides visibility and it now consults `g_pmQuietFace`, so the image
is still selected, still persisted, and comes straight back in `ACTIVE` or
`CHARGING` without an SD re-read (`pm_face_quiet()` reuses the image already
loaded). If the wallpaper is changed while the face is quiet — a voice command
at night, say — the change is deferred and painted on the next bright frame
instead of being lost.

Set `PM_QUIET_FACE 0` in the tunables block to keep the wallpaper in every
state; nothing else depends on the flag.

### Unchanged pixels are not repainted (v1.8)

The face shows `HH:MM` and has no seconds, so 59 of every 60 one-second ticks
were setting the same three labels and invalidating the same region — a partial
panel refresh every second, in every screen-on state, to draw identical pixels.
`update_face_time()` now compares the string, date and status-dot state against
what it last painted and returns early when nothing moved. On an AMOLED the win
is not the CPU cycles, it is the emission and the flush that the repaint would
have cost.

### The dark phase (v2.0)

The time after the panel goes black is split into two stages, and the split is
what makes the watch usable rather than thrifty:

| Stage | When | Draw | Wake cost |
| --- | --- | --- | --- |
| Light sleep | first `deep_after_ms` dark | ~3 mA | **instant** — one tap puts the same interface back, no boot |
| Deep sleep | after that, still dark | ~0.2 mA | full reboot, only BOOT presses |

Ten hours of continuous darkness costs ~30 mAh in stage 1 and ~2 mAh in stage 2,
which is the entire argument for crossing over.

**The clock is `g_pmDarkSince`, not the touch timer.** It is stamped in
`screen_off()` and zeroed in `screen_on()`, so the interval means exactly what the
Settings row says: *continuous* darkness. Any wake restarts it — a tap, the PWR
button, a BOOT press, a voice command that holds the screen, or a battery alert
that lights the panel on its own. Measuring from `lastTouchMs` instead (what v1.9
did) let a non-touch wake shorten the window, because the touch clock and the
dark clock are not the same clock.

`pm_auto_deep()` refuses to cross over when it would be wrong: charging, or on
USB with a full pack (VBUS is read from the PMIC directly, because a full battery
reports "not charging" — a dock must not reboot itself every ten minutes), an app
copy or command holding the screen, a WiFi scan, the ESP-SR bring-up, and
**Always-On Xander**, because a wake word cannot hear anything from deep sleep.

Intervals live in the profile table (`deep_after_ms`): Power Saver 10 min,
Balanced 10 min, Performance 0 (light sleep for ever). One cell changes it, and
the **Auto Sleep** switch in Settings disables the crossover without touching the
profiles.

The clock keeps running across the crossover. `deep_sleep_now()` pins
`ESP_PD_DOMAIN_RTC_PERIPH` on, so the internal RTC keeps time, `time_save_now()`
snapshots it to NVS first, and `setup()` reconciles the two on the way back up —
the watch wakes with the right time, offline, and the `from_deep_sleep` path skips
the splash and the WiFi bring-up.

**The board limit that shapes all of this:** deep sleep can only be woken by
BOOT. `TP_INT` is GPIO38, which is not an RTC pad, so ext1 cannot watch the touch
controller and the FT3168 cannot wake the chip. That is why the first stage
exists — it is the only way to keep tap-to-wake.

### Timeout under load (v1.8)

The Settings timeout is a ceiling, not a floor. On a low pack the panel holds
for at most 8 s (`PM_LOW_HOLD_MS`) and at critical for 5 s (`PM_CRIT_HOLD_MS`),
whatever the slider says — a 60 s hold at 7 % is how a watch dies mid-evening.
`pm_hold_ms()` is the only place that number is computed. Charging clears the
latch, so a plugged-in watch keeps the user's setting.

Below 20 % the panel sits at the low/critical brightness cap from the first frame
and only the *timeout* (8 s / 5 s) controls when it goes black. A
`g_pmBattLatch` of 1 or 2 is therefore a pure hold-time reduction, not a
brightness one — which is what the table above means by "ceiling".

## Settings (Xander → Settings)

* **Power Profile** — Power Saver / Balanced / Performance.
* **Screen Timeout** — 8 / 15 / 30 / 60 s.
* **Night Mode** — manual dim, stays until switched off.
* **Night Schedule** — automatic 22:00–07:00 dim (default on).
* **Auto Sleep** — deep sleep after the profile's dark interval (default on;
  never fires in Performance).

Persisted in NVS namespace `pwr`, keys `prof`, `toff`, `night`, `nsched`,
`deep`. Missing keys fall back to Balanced / 15 s / night off / schedule on /
auto sleep on.

The old `SCREEN_OFF_TIMEOUT_MS` constant is still in the file for reference,
but the countdown now uses the persisted setting (`g_pmTimeoutMs`), whose
default is the same 15 s.

## Activity

`lastTouchMs` is the only activity clock. It is stamped by:

* touch press and release (`my_touchpad_read`),
* a BOOT press that opens the mic,
* the side PWR button,
* a recognised Xander command (`x_do`),
* every `screen_on()`.

Redraws, sensor samples and battery polls never stamp it.

## Battery policy

* Readings come from the existing `pmu_refresh()`; `pm_note_battery()` only
  records that the sample is fresh, so no extra I²C traffic is created while the
  screen is on.
* **The dark panel still samples (v1.8).** One `pmu_refresh()` every five minutes
  (`PM_DARK_SAMPLE_MS`) while `SCREEN_OFF`. Before that, every battery rule was
  dead in the state a watch spends most of its life in: the latch could not
  move, the cell protection could not arm, and plugging in went unnoticed until
  the next tap. One I²C read per 5 min is nothing next to the 250 ms light-sleep
  tick already running.
* A policy is only applied from a sample younger than 60 s. No PMU, a failed
  read or a stale sample simply leaves the last latch in place.
* Thresholds latch with 3 % hysteresis (enter low at 20 %, leave at 23 %;
  enter critical at 7 %, leave at 10 %), so a sag under AMOLED load cannot
  flap the state.
* Crossing a threshold shows one notice (`Battery 18% - power saver`).
  Charging clears every battery limit.
* The 20 % notice does **not** light a dark screen; the 7 % one does, once.
  There is no point waking you to say the battery is fine for another hour.
* Cell protection: two separate readings at ≤ 2 % at least 60 s apart, with no
  command holding the screen, saves the clock and calls the existing
  `deep_sleep_now()`. It never fires twice.

## CPU

While the panel is lit every profile asks for `cpu_active` = 240 MHz; the only
downclock left is `cpu_off` (80 MHz) for `SCREEN_OFF`. `setCpuFrequencyMhz()` is
called only on a state transition (and at boot), never on every loop pass, and
never while:

* `screen_held()` — an app copy or command is running,
* `g_wifi_scan_busy` — a scan is in flight,
* `g_pmMicBusy` — the ESP-SR bring-up is using its 240 MHz boost.

The voice path still boosts itself to 240 MHz and now returns to the *current*
state's clock via `pm_restore_cpu()` instead of forcing 80 MHz.

## Diagnostics

* `[pm] ACTIVE -> SCREEN_OFF  cpu=80 MHz  bright=0  batt=64%` on each transition,
  rate-limited to one line per second.
* `[pm] <state> batt=.. cpu=.. bright=.. awake=..s off=..s xitions=.. heap=..`
  every 10 minutes, plus the %/hour since boot.
* The measured per-mode table every 30 minutes, and again whenever Settings →
  Developer Info is opened (see below).
* Settings → Developer Info shows state, profile, minutes awake/idle/dark and
  the battery line.

Nothing is written to the SD card, and no *instantaneous* current is shown: the
AXP2101 path in this firmware exposes percentage, voltage and charging status
only, with no current register. What the watch can do is integrate the drain it
observes over time and attribute it to the mode that caused it — that is the
ledger below. `BATT_DRAW_ACTIVE_MA` / `BATT_DRAW_SCREENOFF_MA` remain estimates
used by the existing "time left" readout; they are not measurements.

## Measured per-mode energy (v1.7)

Every mA figure elsewhere in this document is a model. This is the instrument
that replaces it, using nothing but the PMIC the board already has.

### Why it samples on transitions

The AXP2101 percentage is coarse: 1 % of a 400 mAh pack is 4 mAh, about 25 s of
`ACTIVE`. So a sample has to cover a window long enough to move that digit, and
for the result to mean anything the window must contain exactly one mode.
Reading on a fixed timer fails both ways — a 15 s poll during use straddles
`ACTIVE` and `SCREEN_OFF` and averages 145 mA with 3 mA, which is worse than no
number at all.

So the ledger closes its window on the state change instead: `pm_enter()` reads
the PMIC *before* adopting the new state, which makes every interval
single-mode by construction. A dark stretch is sampled only every five minutes
(v1.8), which is both the cell-protection cadence and enough to close
`SCREEN_OFF` windows: at ~3 mA a five-minute window moves 0.06 %, so the 1 %
step lands on a window of up to ~85 minutes, still entirely `SCREEN_OFF`.

### Output

Thirty minutes after boot, and on demand:

```
[pm] energy: 41 windows, 88.0 mAh measured, capacity 400 mAh
[pm]   ACTIVE       0.48h     69.6mAh   145mA   79%
[pm]   NIGHT        0.05h      2.9mAh    58mA    3%
[pm]   LOW_BATT     0.00h      0.0mAh      -    0%
[pm]   SCREEN_OFF   5.03h     15.2mAh     3mA   18%
```

Columns: exact dwell in that state, mAh attributed to it, the resulting mean
current, and its share of everything measured. `-` means no window has landed
there yet (under 1 s of attributed time).

### Producing the table

The table below is what the watch reported; it is not hand-filled. Capture the
serial monitor to a file, then convert it:

```sh
awk '
BEGIN { print "| Mode | Dwell (h) | mAh | mA (measured) | Share |"; print "| --- | --- | --- | --- | --- |" }
/^\[pm\]   [A-Z_]+ / {
  mode=$2; d=$3; m=$4; a=$5; s=$6;
  gsub(/h$/, "", d); gsub(/mAh$/, "", m); gsub(/mA$/, "", a); gsub(/%$/, "", s);
  printf "| %s | %s | %s | %s | %s%% |\n", mode, d, m, (a == "-" ? "n/a" : a), s
}' serial.log
```

Take the last block in the file (each block is cumulative since boot, not
delta), so more runtime means a better table. A run that has seen at least one
cycle of each mode you care about is the minimum.

### The table

| Mode | Dwell (h) | mAh | mA (measured) | Share |
| --- | --- | --- | --- | --- |
| `ACTIVE` | — | — | — | — |
| `IDLE` | — | — | — | — |
| `NIGHT` | — | — | — | — |
| `LOW_BATT` | — | — | — | — |
| `CRITICAL` | — | — | — | — |
| `CHARGING` | — | — | — | — |
| `SCREEN_OFF` | — | — | — | — |

Fill it from the converter output above. Until then the rows stay blank on
purpose — a table of guessed numbers is exactly what this feature exists to
replace.

### Sanity check on the first run

A healthy first block lands near `ACTIVE` 145 mA, `NIGHT` 58 mA and `SCREEN_OFF`
3 mA, which is the model in the earlier tables (`IDLE` stays at 0.00 h — nothing
enters it since v2.1). Two failures worth recognising:

* `SCREEN_OFF` in the tens of mA — light sleep is not engaging, so the dark
  screen is still running at full clock. The measurement is right, the sleep
  is not happening.
* `ACTIVE` far below 100 mA on a screen you can read outdoors — the brightness
  target is lower than you think, or the battery percentage is stale (check
  `batt=` in the ten-minute line against the battery screen).

### Accuracy, honestly

* **The mAh column scales with `BATT_CAPACITY_MAH`.** Set that to your pack's
  real nameplate first; the mA column is then the measured shape and the mAh
  column is that shape expressed in your pack's units. A wrong capacity makes
  every mAh wrong by the same factor, not the ratios.
* **1 % resolution.** Short windows carry no charge, so their energy appears in
  the following window (the deltas telescope — nothing is invented or lost). A
  mode needs roughly 30 s of attributed dwell before its mA figure stops
  jumping; expect ±10 % after ten minutes of real use and ±3 % after an hour.
* **Charging is excluded.** While the pack is filling, a delta is not drain, so
  the ledger drops its baseline and restarts on battery.
* **A charge unrelated to a transition is dropped**, not spread: a reading that
  moves more than 25 % in one window is treated as a bad read and counted in
  the `discarded as invalid` line.
* **It cannot separate two modes that alternate faster than 1 % moves.** If you
  tap the screen awake every few seconds, `ACTIVE` and `SCREEN_OFF` share one
  window and the ledger attributes it to the state that was running when the
  window closed. Leave the watch alone for the run if you want clean per-mode numbers.

## Not implemented (and why)

* **Touch wake from deep sleep.** Impossible on this board: wake-from-deep
  needs an RTC pad, `TP_INT` is GPIO38. `esp_deep_sleep_enable_gpio_wakeup()`
  has the same restriction — its mask only accepts RTC pads — so no API can
  route the FT3168 into a deep-sleep wake source. BOOT (GPIO0) is the only
  wake, which is why auto deep sleep is opt-in and off in Performance.
* **QMI8658 motion detection.** The launcher never opens the IMU today; adding
  a driver plus an interrupt path would be new hardware integration, so an
  accelerometer activity source is left as a hook rather than guessed at.
* **Per-state display power-off.** `displayOff()` was deliberately avoided by
  v5.8 because the CO5300 can come back flaky; brightness 0 is used instead and
  the touch controller stays armed, which is what the wake paths rely on.
* **Battery calibration / coulomb counting.** Only the PMIC's own percentage is
  used, as before.

## Test checklist on hardware

1. Boot, wait untouched → the panel holds the user's brightness for the whole
   timeout with **no dim step and no wallpaper swap**, then goes black: the log
   goes `ACTIVE -> SCREEN_OFF`, never `-> IDLE`.
2. After the configured timeout → `SCREEN_OFF`, panel black, tap wakes it, the
   brightness fades up **smoothly at 240 MHz** (no stutter in the fade).
3. Press BOOT with the panel dark → screen lights, `Opening mic...` visible.
4. Settings → screen timeout 8 s / 60 s behaves accordingly; profile change is
   persisted across a reboot.
5. Night Mode on → panel drops to 35; off → back to the user level.
6. On charger: state reports `CHARGING`, no low-battery notice even under 20 %.
7. Drag the battery to 20 % → one warning, brightness capped, no WiFi resync.
8. Set a wallpaper, leave the watch until the panel is dark, then wake it: the
   picture returns without a visible SD re-read (no white flash) and without any
   intermediate dim frame.
9. Set the timeout to 60 s and cross 20 % on battery: the panel now holds ~8 s,
   not 60 s. Plug in → back to 60 s.
10. Leave the watch dark at 21 % overnight: the log shows
    `[pm] notice (screen off): Battery 20% - power saver` and **no** screen wake.
    Wake it in the morning → it comes up dimmed at the low-battery cap.
11. While dark, move the USB cable in: within 5 min `g_charging` flips and the
    next log line shows `9% chg` — the watch notices a charger without being
    touched.
12. At 2 % (or a bench supply set just under it) with the panel dark, the cell
    protection still arms after two samples and deep-sleeps; it must not do so
    while an app copy holds the screen.
13. Dark at the 10 min mark on Balanced → log shows
    `[pm] dark 10 min on Balanced -> deep sleep (BOOT wakes)`, then BOOT wakes it
    and the face comes up with the correct time (no NTP needed).
13a. **The tap window.** On Power Saver, wake it, let it go dark, then tap it at
    2 min, 6 min and 9 min: each tap restores the same screen instantly with no
    boot splash. At 11 min the same tap does nothing and BOOT is required — that
    boundary is the whole point of the two-stage dark phase.
13b. Wake it at minute 9 with a voice command (not a tap), let it go dark again:
    the ten minutes start over, so it must still answer a tap at minute 18. This
    is what `g_pmDarkSince` buys over the old `lastTouchMs` clock.
13c. Wake, then leave it alone: the panel keeps your brightness unchanged for
    the full timeout and then goes straight to black. Nothing dims before that.
14. Same test on Performance: no sleep at 10 min, log stays quiet, screen-off
    draw stays ~3 mA.
15. Auto Sleep off in Settings → 13 does not happen, even after an hour dark.
16. Enable Always-On Xander, go dark for 15 min → no deep sleep (the wake word
    stays armed), and that is deliberate rather than a missed timer.
17. Leave it dark on USB at 100 % for 15 min → no deep sleep (VBUS, not just
    "charging"), so a dock never reboots the watch.
