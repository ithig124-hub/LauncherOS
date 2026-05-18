# Nocturne OS v2.0 — for LauncherOS (Waveshare ESP32-S3 Touch AMOLED 2.06)

A **timeless atmosphere operating system** built as a single-file `.ino`
LauncherOS module. Drop the compiled `.bin` into `/WATCH/bin/nocturneos.bin`
on the SD card and Nocturne shows up as an app on your launcher.

## v2.0 — what's new
- **Six canvas-rendered clock faces** (`luxury / piano / moon / vinyl / rings / cathedral`) — each theme picks its own analog face with sweeping hands
- **Real audio** — uses the on-board **ES8311 codec** (mic + speaker, I²S pins 16/41/45/40/42, PA on GPIO 46) for live 3-band energy + soft piano-tap UI sounds. Falls back to synthesized energy if codec is absent.
- **Entrance choreography** — every card swipe staggers a fade + drift on its children
- **Journey-mode horizon overlay** — drifting particles strip at the bottom
- **QMI8658 wrist double-tap** — flick twice to cycle ambience modes without touching the screen
- **Aura learning** — per-hour theme histogram persists to NVS; over time `AUTO` starts to match your habits
- **Wallpaper passthrough** — reads `/WATCH/PHOTOS/wp_<idx>.raw` (RGB565, 410 × 502) using the same `face/wp_idx` NVS key LauncherOS uses
- **Concert Mode** — plug it into a charger and the visualizer goes fullscreen automatically; tap to exit

## What it is

Not a productivity OS. Not a smartwatch clone. Nocturne is an **emotional
atmosphere engine** built around:

- piano + orchestral aesthetics
- cinematic slow motion
- AMOLED black + intentional empty space
- a horizontal infinite card carousel (clock → aura → visualizer → metronome → themes → modes → settings)

## The 9 themes (emotional environments, not just color packs)

| Theme                  | Mood                |
| ---------------------- | ------------------- |
| Nocturne               | quiet midnight      |
| Golden Brown           | warm nostalgia      |
| Love Story             | cinematic romance   |
| Chasing Kou            | dreamlike midnight  |
| Cordelia               | elegant melancholy  |
| Merry-Go-Round of Life | timeless wonder     |
| Velvet                 | jazz luxury         |
| Winter                 | reflective calm     |
| Lancelot               | noble resolve       |

Each theme changes palette, motion speed, glow, BPM, clock face style and
visualizer style (strings / vinyl / waves / rings / ribbons).

## The Aura Engine

Derives ambience from time-of-day and the active mode:

- **AUTO** — follows time-of-day
- **NIGHT WALK** — dim amber, minimal
- **RAIN** — foggy blur, cool reflections
- **SILENT** — meditative, muted
- **RESOLVE** — disciplined focus
- **JOURNEY** — drifting horizons, cinematic

It dynamically adjusts brightness, particle glow strength and motion scale.

## Build & flash

1. Open `NocturneOS.ino` in **Arduino IDE 2.x**
2. Board: **ESP32-S3 Dev Module**
   - USB CDC On Boot: **Enabled**
   - Flash Size: **16 MB**
   - **Partition Scheme: Minimal SPIFFS (large APP)**  ← must match LauncherOS
   - PSRAM: **OPI PSRAM**
   - CPU: **240 MHz (WiFi)**
3. Install libraries (Library Manager):
   - LVGL 8.3 or 8.4
   - Arduino_GFX_Library
   - Arduino_DriveBus_Library
   - XPowersLib
4. Make sure `lv_conf.h` (included here) sits in your `Documents/Arduino/libraries/` next to the `lvgl/` folder, OR alongside this `.ino`.
5. **Sketch → Export Compiled Binary**
6. Copy **only** `NocturneOS.ino.bin` to your SD card and rename it:
   ```
   /WATCH/bin/nocturneos.bin
   ```
7. Boot the watch into LauncherOS → swipe to **Apps** → tap **nocturneos**.

## SD layout

```
/WATCH/
├── bin/
│   ├── launcheros.bin
│   └── nocturneos.bin       ← this app
├── wifi/config.txt          ← shared WiFi credentials (same as LauncherOS)
└── nocturne/
    └── sessions/            ← reserved for future journey/walk logs
```

## Exit to launcher

- Long-press the wide **Back to Launcher** card on the Settings tile (last card)
- OR hold **BOOT (GPIO 0)** for 1.5 s anywhere in the app

Both routes save state, return to the factory partition (LauncherOS) and reboot.

## Files

```
NocturneOS/
├── NocturneOS.ino   ← single-file sketch (everything lives here)
├── lv_conf.h        ← canonical LauncherOS LVGL config (DO NOT alter)
├── partitions.csv   ← canonical 16 MB layout (matches LauncherOS)
└── README.md        ← this file
```
