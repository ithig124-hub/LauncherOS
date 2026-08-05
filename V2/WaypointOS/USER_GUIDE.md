# WaypointOS — User Guide (v0.1)

Offline vector-map navigator for the Waveshare ESP32-S3 Touch AMOLED 2.06 watch.

---

## Contents

1. [What is WaypointOS?](#1-what-is-waypointos)
2. [Hardware & prerequisites](#2-hardware--prerequisites)
3. [Installing the firmware](#3-installing-the-firmware)
4. [Preparing the SD card](#4-preparing-the-sd-card)
5. [First boot](#5-first-boot)
6. [The Library screen](#6-the-library-screen)
7. [The Map screen](#7-the-map-screen)
8. [The Tools menu](#8-the-tools-menu)
9. [Compass, Level, Sun, Bearings](#9-compass-level-sun-bearings)
10. [Layers, Themes, Saved locations, Breadcrumbs](#10-layers-themes-saved-locations-breadcrumbs)
11. [POIs & Search](#11-pois--search)
12. [Developer mode](#12-developer-mode)
13. [Building your own `.fmap` maps](#13-building-your-own-fmap-maps)
14. [Full control cheat-sheet](#14-full-control-cheat-sheet)
15. [Troubleshooting](#15-troubleshooting)
16. [File & storage reference](#16-file--storage-reference)
17. [What's not in this build](#17-whats-not-in-this-build)

---

## 1. What is WaypointOS?

WaypointOS merges the previous **MapOS** (map viewer) and **NavOS** (compass /
level / sun / bearings) into a single coherent app built around a **native
vector `.fmap` renderer**. Everything runs **offline** — no phone, no network,
no cloud. Maps live as compact `.fmap` binary files on the SD card and are
drawn as real vectors (lines, polygons, points), not raster tiles.

Key differences from MapOS/NavOS:

| Feature | MapOS | NavOS | **WaypointOS** |
|---|---|---|---|
| Map data | 256×256 raster tiles | — | `.fmap` vector tiles (much smaller, sharper) |
| Compass overlay | — | full screen only | overlay on map + full screen |
| Themes | — | — | 4 built-in (Fusion Dark, Fusion Light, Serious, Chill) |
| Layers | — | — | 11 toggleable layers, persisted per-map |
| POIs | — | — | tap to inspect card; text search |
| Saved locations | — | — | per-map CSV |
| Breadcrumbs | on long-press touch | — | dropped from Tools menu |
| Sun icon on map | — | separate screen | overlay + full screen |
| Bearing lock | — | full screen only | on-map floating arrow + full screen |

---

## 2. Hardware & prerequisites

- **Watch**: Waveshare ESP32-S3 Touch AMOLED 2.06 (410 × 502, CO5300 driver,
  FT3168 touch, QMI8658 IMU + AK09918 magnetometer, AXP2101 PMU, microSD via
  SD_MMC, BOOT button on GPIO0).
- **microSD card**: FAT32-formatted. Any size — even a 1 GB card is plenty.
- **LauncherOS**: WaypointOS is a LauncherOS-launched app. If you don't have
  LauncherOS already, install it first — the shared boot pattern that
  MapOS/NavOS/WaypointOS all use lives inside LauncherOS.
- **PC toolchain (for building firmware, only if you want to compile
  yourself)**: Arduino IDE 2.x with ESP32 core 2.0.14+.

---

## 3. Installing the firmware

### Option A — flash the pre-built binary

1. Copy the file `WaypointOS.ino.bin` to your SD card at:
   ```
   /WATCH/bin/waypointos.bin
   ```
2. Insert the SD card, boot into LauncherOS, and launch WaypointOS from the
   launcher grid.

### Option B — build from source

1. Open `WaypointOS.ino` in Arduino IDE.
2. Board settings:
   - **Board**: ESP32-S3 Dev Module
   - **PSRAM**: OPI PSRAM
   - **Flash size**: 16 MB
   - **Partition scheme**: Minimal SPIFFS (large APP)
3. Required libraries:
   - `Arduino_GFX_Library` (by Moon Light)
   - `Arduino_DriveBus_Library`
   - `lvgl` 8.3.x — reuse the `lv_conf.h` that NavOS/MapOS already ship with
   - `XPowersLib`
4. `Sketch → Export Compiled Binary`.
5. Copy the resulting `.bin` to `/WATCH/bin/waypointos.bin` on the SD card.

---

## 4. Preparing the SD card

On first boot WaypointOS auto-creates the folder tree:

```
/WATCH/
├── bin/
│   └── waypointos.bin           ← the firmware
└── WayPoint/
    ├── README.txt               ← auto-dropped on first run
    ├── saved/                   ← per-map saved locations (CSV)
    ├── loc.txt                  ← your lat,lon for sun calc (optional)
    ├── bearings.txt             ← MARKed bearings log
    ├── breadcrumbs.csv          ← dropped breadcrumbs
    └── *.fmap                   ← your vector maps go here
```

You just need to add `.fmap` files. Three sample maps ship with the project:

| File | Size | Contents |
|---|---|---|
| `School.fmap` | 627 B | Byford Secondary campus — buildings, oval, paths |
| `Byford.fmap` | 2.6 KB | Small town — highway, residential grid, parks, river, 15 POIs |
| `Perth.fmap` | 5.5 KB | Perth CBD — grid, Swan River, Kings Park, 25 POIs incl. Bell Tower |

Copy them from `/app/WaypointOS/samples/` to `/WATCH/WayPoint/` on the card.

Optional: create `/WATCH/WayPoint/loc.txt` with a single line like:
```
-32.2175,116.074
```
That's your latitude,longitude — used only by the Sun position calculator.

---

## 5. First boot

You'll see, in order:

1. **Splash** — animated pin drop + "WAYPOINT / OS" text (~1 sec).
2. **Library** — a card carousel of every `.fmap` in `/WATCH/WayPoint/`.

If the library is empty:

```
No .fmap files found.

Place vector maps in:
/WATCH/WayPoint/*.fmap
```

Just add `.fmap` files to the card and reboot (or leave the card in and
LauncherOS→WaypointOS again).

---

## 6. The Library screen

The library is a **carousel** — one map card at a time, with the map title,
filename, and description read from the `.fmap` metadata.

```
╭─────────────────────────────╮
│         Byford              │
│         file: Byford.fmap   │
│                             │
│  Small town south of Perth  │
│  — arterial roads, parks    │
│  and shops.                 │
│                             │
│  <             >            │
╰─────────────────────────────╯
       2 / 3
```

### Controls

| Input | Action |
|---|---|
| BOOT tap | **Next** map |
| BOOT double-tap | **Previous** map |
| BOOT hold 0.5–1.2 s | **Open** the current map |
| BOOT hold 1.5 s | **Return to LauncherOS** |
| Touch swipe left/right | Next / Previous map |
| Touch tap on card | Open the current map |

Last-opened map is remembered across reboots (NVS key `last_map`).

---

## 7. The Map screen

This is the main view — a vector map with a small overlay chip in the top-left
showing the map title and current zoom (e.g. `Byford z2`). A crosshair marks
the current camera centre.

### Overlays

- **Top-right** — North indicator; the red needle points to true magnetic
  north relative to the map's rotation. (Only appears once the compass has a
  reading.)
- **Top-left corner** — floating **bearing arrow** (only after you've MARKed a
  bearing on the Compass screen). Colour goes gold when your heading is within
  5° of the marked bearing.
- **Top-centre arc** — **sun** icon, placed at the sun's azimuth relative to
  your heading. Only visible if `loc.txt` is set + the RTC is synced.

### Controls

| Input | Action |
|---|---|
| Touch drag | Pan the map |
| Touch double-tap | Zoom in |
| Touch long-press (>700 ms, no drag) | Open the **Tools** menu |
| Touch tap on a POI dot | Open the POI card |
| BOOT tap | Zoom in |
| BOOT double-tap | Zoom out |
| BOOT hold 0.5–1.2 s | **Recentre** the map |
| BOOT hold 1.5 s | Back to Library |
| BOOT hold 4 s | Direct return to LauncherOS (works from anywhere) |

### Zoom levels

`.fmap` files declare their own zoom bands (typically 4: overview → detailed).
Individual objects can be tagged with a `zmin`/`zmax` range so, e.g., a small
alley only appears at zoom 3 and above. The camera picks the best-matching
zoom based on the current pixel scale — you'll see the zoom number update in
the overlay chip.

---

## 8. The Tools menu

Long-press on the map to open the Tools menu. It's a 3×3 grid of buttons plus
a bottom row with two quick-toggles.

```
┌──────────────┬──────────────┬──────────────┐
│ Save         │ Layers       │ Themes       │
│ Location     │              │              │
├──────────────┼──────────────┼──────────────┤
│ Compass      │ Level        │ Sun          │
├──────────────┼──────────────┼──────────────┤
│ Bearings     │ Search       │ Developer    │
├──────────────┴──────────────┼──────────────┤
│ Drop Breadcrumb             │ Rotate: OFF  │
└─────────────────────────────┴──────────────┘
```

| Button | What it does |
|---|---|
| **Save Location** | Save the current camera-centre as a named waypoint (opens on-screen keyboard). |
| **Layers** | Toggle the 11 render layers on/off, persisted per map. |
| **Themes** | Switch between Fusion Dark, Fusion Light, Serious, Chill. |
| **Compass** | Full-screen tilt-compensated heading readout + MARK button. |
| **Level** | Bubble level + pitch/roll degrees. |
| **Sun** | Sun azimuth + elevation readout. |
| **Bearings** | List of your MARKed bearings from `bearings.txt`. |
| **Search** | Full-text search over the loaded POI list. |
| **Developer** | Diagnostic screen (heap, tile cache, frame time…). |
| **Drop Breadcrumb** | Append the current camera position to `breadcrumbs.csv`. |
| **Rotate: ON/OFF** | Toggle "map rotates with heading" mode (off = north-up). |

Every sub-screen has a **Back to Map** pill at the bottom, or press BOOT hold
1.5 s. Press BOOT hold 4 s at any time to jump straight back to LauncherOS.

---

## 9. Compass, Level, Sun, Bearings

### Compass

- Large heading readout in degrees, plus cardinal (N / NE / E / …).
- **MARK button** — tap to remember the current heading. From then on, a
  floating gold-when-aligned arrow on the map points back to that direction.
  MARK is persisted in NVS across reboots.
- **Long-press MARK (≥ 5 s)** — starts **magnetometer calibration**. Slowly
  rotate the watch through a "figure-8" pattern for 20 seconds. Hard-iron
  offsets + soft-iron scales are computed and stored in NVS. Do this once
  after installing the watch on a new strap or after moving to a new
  magnetic environment.

### Level

Uses the QMI8658 accelerometer to show pitch/roll. The bubble travels around
a large circular gauge; the numbers below show `roll  pitch` in degrees.
Useful for mounting shelves, checking table flatness, or sighting a slope.

### Sun

Approximate NOAA-style sun position:

- **az** = azimuth (° from true north)
- **el** = elevation above the horizon (negative = below horizon, i.e. night)

Requires the RTC to be synced (LauncherOS handles this via WiFi time) and
`/WATCH/WayPoint/loc.txt` to contain your `lat,lon`. Without either, it
displays `no time sync yet`.

### Bearings

Every MARK on the Compass screen appends a line to
`/WATCH/WayPoint/bearings.txt`:
```
1706700123,127.5
1706700455,244.0
```
(Unix seconds, heading°.) The Bearings screen renders the last 20 entries
with their cardinal.

---

## 10. Layers, Themes, Saved locations, Breadcrumbs

### Layers

11 toggles, persisted per-map (NVS key `lm_<title>`):

`Roads · Buildings · Parks · Water · Paths · Schools · Shops · Transit ·
Beacons · Saved · Breadcrumbs`

Turning a layer off skips its objects entirely — a perf win on dense city
maps.

### Themes

4 palettes:

| Theme | Vibe |
|---|---|
| **Fusion Dark** (default) | Deep navy + amber accents. Best in low light. |
| **Fusion Light** | Bright cream + navy on paper white. Best in sunlight. |
| **Serious** | Navy + brass. Nautical / cartographic. |
| **Chill** | Teal + coral. Playful, casual. |

Theme is persisted in NVS (`theme`). Layer colours re-derive from the theme,
unless the `.fmap` bundles its own style override — in which case the map's
style wins.

### Saved locations

Tools → Save Location opens a keyboard. Type a name, press Save. The current
camera position is appended to
`/WATCH/WayPoint/saved/<MapTitle>.csv`:
```
150,180,0,IGA
-30,220,0,Cafe stop
```
(x, y, icon_id, name.) Saved locations render as their own layer on the map
(orange dot with a small pin stem).

### Breadcrumbs

Tools → Drop Breadcrumb appends:
```
1706700888,Byford,150,180
```
(unix seconds, map title, x, y.) All maps share the same breadcrumbs file.

---

## 11. POIs & Search

POIs come from the `.fmap` POI section. Each POI has a name, type, icon id,
and world coordinates. They render as small coloured dots on the map (colour
= POI category via the theme's shops/parks/transit/etc. palette).

- **Tap a POI dot** — opens a full-screen card with the POI name and a small
  info block. Press **Back to Map** to return.
- **Tools → Search** — full-screen keyboard. As you type, matching POIs
  appear in a list below. Tap any result to jump the camera to that POI.
  Match is case-insensitive substring on the POI name.

---

## 12. Developer mode

Tools → Developer shows a live diagnostic block:

```
Map:   Byford
Zoom:  2 (4 zooms)
Cam:   250, -180  scl 0.0430
Tiles: 12 indexed, 9 cached
POIs:  15
Objs:  47 last frame
Draw:  8231 us
Heap:  184 KB free
PSRAM: 6291 KB free
Bat:   87 %
IMU:   ok   Mag: ok
Hdg:   142.3   Mark: -1.0
```

Refresh happens twice per second. Useful for verifying tile-cache eviction,
render performance, and IMU/magnetometer availability.

---

## 13. Building your own `.fmap` maps

Ship with the project (in `tools/`):

- **`fmap_builder.py`** — reference Python encoder. Writes byte-for-byte
  compatible `.fmap` files.
- **`make_samples.py`** — builds the three sample maps.
- **`decode_check.py`** — round-trip validator.

### Minimal example

```python
from fmap_builder import (
    FMap, LAY_ROADS, LAY_BUILDINGS, LAY_PARKS, POI_LANDMARK,
)

m = FMap(
    title="MyPark",
    desc="Community park with paths and a rotunda.",
    bounds=(-32.20, 116.07, -32.19, 116.08),
    origin=(-32.195, 116.075),
)
m.tile_size(500)                     # world units per tile
m.add_zoom(1, 5.0)                   # overview
m.add_zoom(2, 1.0)                   # detailed

# A park polygon
m.add_poly(layer=LAY_PARKS,
           pts=[(-200,-200),(200,-200),(200,200),(-200,200)])
# A path
m.add_line(layer=LAY_ROADS, pts=[(-200,0),(200,0)])
# A landmark
m.add_poi(kind=POI_LANDMARK, x=0, y=0, name="Rotunda")

m.set_style(LAY_ROADS, colour=0xC618, width=3)
m.write("MyPark.fmap")
```

### Coordinate system

All object coordinates are `int32` **world units**. It's a local metric grid;
the `.fmap` metadata carries an `origin_lat/lon` + `units_per_metre` so a
future GPS layer can align. For hand-authored maps, treat the numbers as
metres from a chosen origin (0,0 at the map centre is a good default).

### Zoom bands

Each object has `zmin`/`zmax` — the zoom range where it's visible. Use
this to hide clutter at overview zoom (`zmin=3`) or show only major roads
zoomed out (`zmax=2`).

### See also

- `FMAP_FORMAT.md` — the full binary spec, section by section.
- `tools/fmap_builder.py` — heavily commented; read it to understand the
  encoder.

---

## 14. Full control cheat-sheet

### BOOT button

| Held for… | Library | Map | Any sub-screen |
|---|---|---|---|
| tap (< 400 ms) | next map | zoom in | (LVGL button events) |
| double-tap | prev map | zoom out | — |
| 400–1200 ms | **open** map | recentre | — |
| ≥ 1500 ms | **→ LauncherOS** | → Library | → Map |
| ≥ 4000 ms | **→ LauncherOS** | **→ LauncherOS** | **→ LauncherOS** |

### Touch

| Gesture | Library | Map |
|---|---|---|
| Swipe left/right | next/prev map | pan |
| Drag | — | pan |
| Tap | open selected | tap POI → card |
| Double-tap | open selected | zoom in |
| Long-press (700 ms, still) | — | **Tools menu** |

---

## 15. Troubleshooting

**"No .fmap files found."** — Confirm files are directly under
`/WATCH/WayPoint/` (not in a `maps/` subfolder) and end in `.fmap`. Case
matters on some cards; `.FMAP` also works.

**Nothing draws even though the library shows a card.** — The file's header
parsed OK but the tile-index or vec section is corrupt. Regenerate the file
with `fmap_builder.py`. Check `tools/decode_check.py` output for that file.

**Compass reads always the same direction.** — Do a magnetometer calibration:
Compass screen → **long-press MARK for 5 s**, then slowly move the watch
through a figure-8 for 20 s. Values are saved in NVS.

**Sun screen says "no time sync yet".** — LauncherOS sets the RTC over WiFi.
Boot into LauncherOS first, let it sync, then launch WaypointOS.

**Layer changes don't persist per map.** — NVS keys are truncated to 15 chars
(`lm_` + 12 chars of the map title). Two maps whose titles match on the first
12 characters will share a layer preference. Rename them slightly.

**Screen goes garbled after opening the Tools menu, then Back.** — Force a
redraw by panning one pixel. If it happens consistently, capture a serial log
(115200 baud) and file an issue.

**"I'm stuck in a sub-screen and the back pill isn't responsive."** — Hold
BOOT for 4 seconds. That always returns to LauncherOS, regardless of state.

---

## 16. File & storage reference

### On the SD card

```
/WATCH/
├── bin/
│   └── waypointos.bin
└── WayPoint/
    ├── README.txt                 (auto-generated, first boot)
    ├── <Anything>.fmap
    ├── saved/
    │   └── <MapTitle>.csv         "x,y,icon,name" per line
    ├── breadcrumbs.csv            "unix,mapTitle,x,y" per line
    ├── bearings.txt               "unix,heading°" per line
    └── loc.txt                    single line "lat,lon"
```

### In NVS (namespace `waypoint`)

| Key | Type | Meaning |
|---|---|---|
| `last_map` | String | full path of the last-opened `.fmap` |
| `theme` | uint8 | 0=Fusion Dark, 1=Light, 2=Serious, 3=Chill |
| `lm_<title>` | uint32 | layer bitmask, one per map (title truncated to 12 chars) |
| `mxo`, `myo`, `mzo` | float | magnetometer hard-iron offsets |
| `mxs`, `mys`, `mzs` | float | magnetometer soft-iron scales |
| `mark_hdg` | float | last MARKed heading (0–359), or -1 if none |

---

## 17. What's not in this build

Deferred per the v0.1 spec:

- **PC Map Builder web app** — full OSM ingestion + Leaflet region picker +
  `.fmap` export. Right now, `.fmap` files are hand-authored via the Python
  `fmap_builder.py`. The web app will wrap the same encoder.
- **Meshtastic live beacon integration** (v0.7) — the Beacons layer is a
  data-model stub for now; nothing is drawn until the live protocol lands.
- **Voice commands** (v0.6).
- **Phone GPS over BLE** — you still need to hand-set `loc.txt` for the Sun
  calc, and there's no live "you are here" dot yet.
- **Explore Mode** — random POI suggestion.
- **Pinch-to-zoom** — the shared FT3168 touch driver in the boot block only
  returns single-finger coords. Use double-tap on the screen or BOOT
  double-tap for zoom-out.
- **Block-nav (↑←↓→ tap-to-pan)** — planned but not enabled in v0.1.

---

## Colophon

WaypointOS v0.1 is a merged descendant of MapOS v1.2 and NavOS v1.0. The
shared LauncherOS boot block is reused verbatim; the vector renderer, `.fmap`
loader, layer/theme system, POI card, search, and Tools menu are new. Total
firmware ~2300 lines in a single `.ino`, no external services, no cloud.
