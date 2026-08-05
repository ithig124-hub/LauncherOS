# WaypointOS v0.1

Offline vector mapping firmware for the Waveshare ESP32-S3 Touch AMOLED 2.06 watch.
Merges MapOS (map viewer) + NavOS (compass / level / sun) into a single coherent
app built around a native `.fmap` vector renderer.

## Hardware

- ESP32-S3 with PSRAM (Waveshare 2.06 AMOLED)
- 410x502 CO5300 display
- FT3168 capacitive touch
- QMI8658 IMU + AK09918 magnetometer
- AXP2101 PMU
- microSD via SD_MMC

## Build

Arduino IDE 2.x, ESP32 core 2.0.14+.

**Board:** ESP32-S3 Dev Module
**PSRAM:** OPI PSRAM
**Flash Size:** 16 MB
**Partition Scheme:** Minimal SPIFFS (large APP)

### Libraries

- `Arduino_GFX_Library` (Moon Light)
- `Arduino_DriveBus_Library`
- `lvgl` 8.3.x  (reuse `lv_conf.h` from MapOS/NavOS)
- `XPowersLib`
- Core: `SD_MMC`, `Preferences`, `Wire`

### Flash

Sketch -> Export Compiled Binary -> copy
`WaypointOS.ino.bin` to `/WATCH/bin/waypointos.bin` on the SD card,
launch from LauncherOS.

## SD Card Layout

```
/WATCH/WayPoint/
├── Byford.fmap
├── Perth.fmap
├── School.fmap
├── saved/
│   └── Byford.csv                # per-map saved locations
├── breadcrumbs.csv
├── bearings.txt
├── loc.txt                       # "lat,lon" for sun calc
└── settings.json                 # theme + last map
```

NVS namespace `waypoint`, keys:
- `last_map`   -> String
- `theme`      -> uint8
- `layers_<mapname>` -> uint32 bitmask
- `mag_cal_*`  -> six floats
- `mark_hdg`   -> float

## Controls

**Library screen**
- BOOT tap = next map, double-tap = prev map, medium hold = open, long hold = launcher

**Map screen**
- Drag = pan
- Pinch = zoom (two-finger)
- Double-tap = zoom in
- Long-press = tools menu (Save location, Layers, Themes, Compass, Level, Sun, Bearings, Search, Dev)
- BOOT tap = zoom in, double-tap = zoom out, medium hold = recentre, long hold = back to library

## Themes

`Fusion Dark` (default), `Fusion Light`, `Serious` (navy + brass), `Chill` (teal + coral).

## `.fmap` Format

See [FMAP_FORMAT.md](./FMAP_FORMAT.md).

### Building sample maps

```
cd tools
python3 make_samples.py     # writes ../samples/{Byford,Perth,School}.fmap
python3 decode_check.py     # round-trip validator (optional)
```

Copy the resulting files to `/WATCH/WayPoint/` on the SD card. The
`fmap_builder.py` module is the reference writer — a PC Map Builder web app
will wrap the same encoder in a later release.

## Not In This Build (Deferred)

- PC Map Builder web app
- Voice commands
- Meshtastic live beacons (layer stub only)
- Phone GPS over BLE
- Explore Mode
