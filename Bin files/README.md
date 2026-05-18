# LauncherOS — Game / Media / Sport OS modules

Three single-file `.ino` apps that drop into `/WATCH/bin/` on your
Waveshare ESP32-S3 Touch AMOLED 2.06 watch and are loaded by
[LauncherOS](https://github.com/ithig124-hub/LauncherOS) v6.1+.

| App        | Splash style                       | SD root                  | NVS ns       |
| ---------- | ---------------------------------- | ------------------------ | ------------ |
| GameOS     | CRT scanlines + "GAME//OS"         | `/WATCH/games/`          | `gameos`     |
| MediaOS    | Vinyl ring + "MEDIA::OS"           | `/WATCH/media/*`         | `mediaos`    |
| SportOS    | HUD grid + "SPORT//OS"             | `/WATCH/workouts/`       | `sportos`    |

> **Loading screens?**  Dev OS and Minimal OS **do not** ship a custom
> in-app loading splash — they ride on the LauncherOS letter-by-letter
> "LauncherOS" boot splash. Game / Media / Sport each have their own
> custom in-app splash (CRT / vinyl / HUD) that runs *after* the shared
> boot block and *before* the main UI, so you can visually tell which
> OS just loaded.

## Build & flash (per app)

1. Open the `.ino` in **Arduino IDE 2.x**.
2. Board: **ESP32-S3 Dev Module**
   - USB CDC On Boot: **Enabled**
   - Flash Size: **16 MB** (or match your board)
   - **Partition Scheme: Minimal SPIFFS (large APP)**  ← must match LauncherOS
   - PSRAM: **OPI PSRAM**
   - CPU: **240 MHz (WiFi)**
3. Install libraries (Library Manager):
   - LVGL 8.3 or 8.4
   - Arduino_GFX_Library
   - Arduino_DriveBus_Library
   - XPowersLib
4. **Sketch → Export Compiled Binary**
5. Copy **only** the `*.ino.bin` to your SD card:
   ```
   /WATCH/bin/gameos.bin
   /WATCH/bin/mediaos.bin
   /WATCH/bin/sportos.bin
   ```
6. Boot the watch into LauncherOS → swipe to **Apps** → tap.

## SD card folders (auto-created on first run)

```
/WATCH/
├── bin/                 ← LauncherOS (already there)
├── PHOTOS/              ← LauncherOS wallpapers
├── wifi/config.txt      ← shared WiFi credentials (same file as LauncherOS)
├── games/
│   └── saves/
├── media/
│   ├── music/           ← drop .mp3 / .wav here
│   ├── anime/
│   │   └── watchlist.txt  ← Title|Sn|En|status, one per line
│   └── manga/
│       └── <title>/page_001.raw …
└── workouts/            ← per-session .csv logs (SportOS writes here)
```

## WiFi (matches LauncherOS exactly)

All three apps read `/WATCH/wifi/config.txt`, the **same file** LauncherOS uses:

```
WIFI1_SSID=MyHomeNetwork
WIFI1_PASS=secretpassword
WIFI2_SSID=OfficeWiFi
WIFI2_PASS=officepw
GMT_OFFSET=8
```

Each app does one short NTP sync at boot (during the splash), then **kills the radio**
exactly like LauncherOS to save ~70-100 mA. The status dot top-right of every Home
screen is **green** if synced, **red** if not.

## Back-to-Launcher (single full-width card)

Each app shows a wide **"Back to Launcher"** card at the bottom of its home
screen (cyan-bordered, dark surface, with the subtitle `loads /WATCH/bin/launcheros.bin`).
Tapping it:

1. Opens `/WATCH/bin/launcheros.bin` from the SD card
2. Streams it through `Update.begin / write / end` with a live progress bar overlay
3. `esp_restart()` → boots straight back into LauncherOS

If the file is missing or too small to be a valid firmware, the card falls
back to the OTA-partition-flip path so you can always escape.

Belt-and-braces: **hold BOOT (GPIO 0) for 1.5 s** anywhere in the app for the
hardware safety exit (uses the partition-flip path, no SD needed).

## Watch face wallpapers

Each app's home screen now renders a **raw RGB565 wallpaper** from
`/WATCH/PHOTOS/` as the background — exactly like LauncherOS does. It reads
the same `face / wp_idx` NVS key the launcher uses, so the wallpaper you
picked on the launcher carries over to every app. Files must be
**410 × 502 × 2 bytes = 411 884 bytes** (`.raw` or `.bin`).

## SportOS highlights (the AMAZING one)

- **RUN** mode opens a target-pace picker first (±5 s / km, min 3:00, max 15:00)
- Real-time pace, distance, steps, calories, duration
- Pace band at top of screen is colour-coded: **green = under target**,
  **cyan = on target**, **amber = slightly over**, **red = way over**
- **HIIT** = work/rest interval timer with phase counter
- **Solo-Leveling style XP system** persisted to NVS:
  - 30 XP per km + 5 XP per minute (min 10 / session)
  - Level threshold = 100 + (lv-1)·60
  - Daily streak with calendar-aware reset
- Every session writes a CSV log to `/WATCH/workouts/YYYYMMDD_HHMMSS.csv`

## File checklist

- [x] Single `.ino` per app (no `.h` / `.cpp` companions)
- [x] Matches LauncherOS partition scheme
- [x] Matches LauncherOS pin map & driver versions
- [x] Custom in-app loading splash
- [x] Battery pill + clock + WiFi sync from `/WATCH/wifi/config.txt`
- [x] Back-to-Launcher pill + BOOT long-press
- [x] Auto-creates its SD subdirs

Built for the cyberdeck ecosystem. Have fun.
