# MapOS

An offline tile-map viewer for the Waveshare ESP32-S3 Touch AMOLED 2.06 watch,
designed as a drop-in **LauncherOS** application (lives alongside BookOS,
WiFiOS, etc. under the shared `/WATCH/` folder on the SD card).

* No internet on the device. You prep your tiles on a PC and copy them to the SD.
* No on-screen buttons - every action is on the **BOOT** hardware button.
* Drag to pan with your finger, long-press to drop a breadcrumb.
* Renders 256x256 RGB565 tiles direct from SD via the Arduino_GFX driver
  (no decompression, no LVGL widgets in the hot path).

```
                 +---------------------------+
                 |   MAP // OS   library     |
                 |                           |
                 | <  +-------------------+ >|
                 |    |   Paris           |  |
                 |    |   folder: Paris   |  |
                 |    |   Inside the      |  |
                 |    |   peripherique    |  |
                 |    +-------------------+  |
                 |          1 / 3            |
                 |                           |
                 |  BOOT tap   -> next map   |
                 |  BOOT 2x    -> prev map   |
                 |  BOOT 0.5s  -> open       |
                 |  BOOT 1.5s  -> launcher   |
                 +---------------------------+

                 Once you open a map:

                 +---------------------------+
                 |   Paris z14   83%         |
                 |                           |
                 |          +-+              |
                 |          |o|              |    <-- crosshair + "you are here"
                 |          +-+              |
                 |                           |
                 |        [ tiles ]          |
                 +---------------------------+
                 BOOT tap          = zoom in
                 BOOT double tap   = zoom out
                 BOOT 0.5-1.2 s    = recentre
                 BOOT > 1.5 s      = back to LIBRARY
                 finger drag       = pan
                 finger long-press = breadcrumb
```

---

## Download

Grab the complete bundle (sketch + tile-prep CLI + docs) here:

**[MapOS_v1.1.zip](./MapOS_v1.1.zip)** *(approx 15 KB, plain source)*

Inside the zip:

```
MapOS_v1.1/
 +- MapOS.ino           <- the Arduino sketch (this is what you flash)
 +- README.md           <- this file
 +- TILES.md            <- in-depth tile-prep guide
 +- partitions.csv      <- 16 MB partition table for the watch
 +- tools/
 |   +- prep_tiles.py   <- one-file Python CLI to download/convert tiles
 |   +- requirements.txt
 +- examples/
     +- paris.sh        <- one-line example for Paris (zoom 13-15)
     +- demo_me.txt     <- example me.txt file
```

---

## SD-card layout (the WATCH folder)

Like all LauncherOS apps, **everything for MapOS lives under `/WATCH/MapOS/`**.
Your card ends up looking like this once you've used the launcher a bit:

```
SD:
 +- WATCH/
     +- bin/              <- compiled app binaries (launcher reads these)
     |   +- mapos.bin
     |   +- bookos.bin
     |   +- ...
     +- wifi/             <- shared WiFi creds (WiFiOS / NetOS)
     +- BookOS/           <- BookOS's own folder
     |   +- books/
     |   +- ...
     +- MapOS/            <- *** everything MapOS uses lives here ***
         +- tiles/
         |   +- Paris/                       <- *** each map = its own folder ***
         |   |   +- meta.txt                 (title, desc, centre lat/lon)
         |   |   +- z13/x4149/y2818.raw
         |   |   +- z14/x8299/y5636.raw
         |   |   +- ...
         |   +- London/
         |   |   +- meta.txt
         |   |   +- z13/...
         |   +- Yosemite/
         |       +- meta.txt
         |       +- z11/...
         +- me.txt        <- "lat,lon" of your "you are here" dot (global)
         +- breadcrumbs.csv  <- created automatically on long-press
```

MapOS will **auto-create** `/WATCH/MapOS/` and `/WATCH/MapOS/tiles/` on first
boot if they don't exist. You only need to add tiles + me.txt yourself.

---

## How to flash MapOS

1. Install **Arduino IDE 2.x** and the **ESP32 board package 3.x**.
2. Install these libraries (Library Manager or zip):
   - `LVGL` 8.3.x or 8.4.x (with the `lv_conf.h` from your LauncherOS repo)
   - `Arduino_GFX_Library` (moononournation)
   - `Arduino_DriveBus_Library` (Waveshare fork)
   - `XPowersLib` (lewisxhe)
3. Open `MapOS.ino`. In **Tools** set:
   - Board: **ESP32S3 Dev Module**
   - PSRAM: **OPI PSRAM**
   - Flash size: **16 MB**
   - Partition Scheme: **Minimal SPIFFS (Large APPS with OTA)** *or* the
     `partitions.csv` shipped in the zip (copy it next to the .ino).
4. **Sketch -> Export Compiled Binary**. Find the `.ino.bin` next to the
   sketch and rename it `mapos.bin`.
5. Copy `mapos.bin` to `/WATCH/bin/mapos.bin` on the SD card.
6. Boot the watch into LauncherOS, pick MapOS from the carousel.

---

## How to get tiles - the short version

You have three options. Pick whichever matches your situation.

### Option A: download from OpenStreetMap (small region, easiest)

```bash
# one-time setup
python3 -m pip install pillow requests

cd MapOS_v1.1/tools

# download a city-sized region into its own map folder. The name you give
# --map becomes the folder under /WATCH/MapOS/tiles/ AND appears in the
# library carousel.
python3 prep_tiles.py download \
    --map "Paris" \
    --title "Paris" \
    --desc "Inside the boulevard peripherique" \
    --lat1 48.80 --lon1 2.25 \
    --lat2 48.91 --lon2 2.42 \
    --zmin 13 --zmax 15 \
    --out ./SD

# set "you are here" (one global file, shared by every map)
python3 prep_tiles.py me --lat 48.8566 --lon 2.3522 --out ./SD

# now copy ./SD/WATCH/ to the root of your SD card
# Repeat the download command with a different --map to add more maps to
# the carousel.
```

### Option B: bring your own PNG tiles (Mapbox export, JOSM, QGIS, tilemaker)

```bash
python3 prep_tiles.py convert --map "MyTown" --in ./my_png_tiles --out ./SD
```

`./my_png_tiles` must be laid out as `z<Z>/x<X>/y<Y>.png` (standard slippy
layout). The script converts every PNG to RGB565 raw and writes them under
`./SD/WATCH/MapOS/tiles/`.

### Option C: any other slippy-map server

```bash
python3 prep_tiles.py download \
    --lat1 ... --lon1 ... --lat2 ... --lon2 ... --zmin 13 --zmax 15 \
    --url "https://tiles.stadiamaps.com/tiles/alidade_smooth/{z}/{x}/{y}.png?api_key=XXXX" \
    --out ./SD
```

See **TILES.md** for the full guide (sizing tables, raster providers,
OSM etiquette, self-hosted tile servers).

---

## Controls (the only ones you need to remember)

| Input                          | Action                          |
|--------------------------------|---------------------------------|
| BOOT tap (< 400 ms)            | Zoom in (wraps to min after max)|
| BOOT double-tap (< 500 ms apart)| Zoom out                        |
| BOOT hold 0.5 - 1.2 s          | Recentre on "me"                |
| BOOT hold > 1.5 s              | Back to launcher                |
| Finger drag                    | Pan the map                     |
| Finger long-press (~1 s, no drag)| Drop a breadcrumb (saved to CSV)|

If `me.txt` doesn't exist the map opens at `(0, 0)` - you'll see only the
crosshair and no red dot. Add `/WATCH/MapOS/me.txt` with a `lat,lon` line
to enable the red "you are here" marker.

---

## Troubleshooting

| Symptom                              | Likely cause                                     |
|--------------------------------------|--------------------------------------------------|
| Solid grey screen with bounding boxes| No tiles for that lat/lon at this zoom on SD     |
| Map renders but no red dot           | `me.txt` missing or malformed                    |
| Map is solid blue/scrambled colours  | Tiles aren't RGB565 little-endian. Re-run the CLI|
| Watch reboots when I hold BOOT       | That's the back-to-launcher action - working fine|
| Breadcrumb tap does nothing          | `me.txt` is required - we record the *me* lat/lon|

---

## Building this for yourself / contributing

The project is one self-contained `.ino` plus the tile-prep CLI. The
LauncherOS shared boot block (the chunk between the `===== SHARED BOOT BLOCK`
markers) is copied verbatim from the rest of the LauncherOS apps, so MapOS
participates in the partition-swap return-to-launcher flow without any
custom IPC.

Drop a PR to the LauncherOS repo with a `MapOS/` folder, or just rebuild
locally and use the launcher's "Save to Github" feature.

---

## Licence

MapOS itself: MIT. Tile data is governed by whoever produced the tiles
(OpenStreetMap is **ODbL**; commercial providers have their own terms).
The `prep_tiles.py` script sets a polite User-Agent and rate-limits to 10
requests/second on the public OSM tile servers - **do not** use those
servers for bulk downloads of large regions.
