# LinkOS — single-file `.ino` for the LauncherOS cyberdeck

LinkOS is a sibling watch app that joins
[LauncherOS](https://github.com/ithig124-hub/LauncherOS) (GameOS / MediaOS /
SportOS / BookOS / DevOS / …) **without modifying any of them**.  It links
the watch to a Raspberry Pi Zero 2 W running
[ServerOS](https://github.com/ithig124-hub/ServerOS) over WiFi (LAN or the
Pi's own hotspot) and BLE (v1.1+).

| Splash style | SD root | NVS namespace |
| ------------ | ------- | ------------- |
| `LINK//OS` double-ring + packet sweep | `/WATCH/link/` | `linkos` |

## Build & flash

1. Open `linkos.ino` in **Arduino IDE 2.x**.
2. Board: **ESP32-S3 Dev Module**
   * USB CDC On Boot: **Enabled**
   * Flash Size: **16 MB**
   * **Partition Scheme: Minimal SPIFFS (large APP)** ← must match LauncherOS
   * PSRAM: **OPI PSRAM**
   * CPU: **240 MHz (WiFi)**
3. Install libraries from Library Manager (same set as every sibling):
   * LVGL 8.3 or 8.4
   * Arduino_GFX_Library
   * Arduino_DriveBus_Library
   * XPowersLib
   * ArduinoJson
4. **Sketch → Export Compiled Binary**.
5. Copy `linkos.ino.bin` to the SD card as `/WATCH/bin/linkos.bin`.
6. Boot the watch into LauncherOS → swipe to **Apps** → tap **LinkOS**.

## SD card folders (auto-created on first run)

```
/WATCH/link/
├── log/                 ← rolling http log per session
├── cache/               ← last /api/apps response (offline cache)
└── notes/               ← offline-queued notes, flushed on reconnect
```

It also reuses the LauncherOS shared files:

```
/WATCH/wifi/config.txt   ← shared WiFi credentials (read-only)
/WATCH/PHOTOS/           ← raw 410×502 wallpapers (RGB565)
```

## WiFi (matches LauncherOS exactly)

`linkos.ino` reads `/WATCH/wifi/config.txt`:

```
WIFI1_SSID=MyHomeNetwork
WIFI1_PASS=secretpassword
WIFI2_SSID=OfficeWiFi
WIFI2_PASS=officepw
GMT_OFFSET=8
```

Unlike most siblings, LinkOS **keeps the radio alive** (it's the one module
whose job is to talk to the Pi).  When the screen is off, the radio drops
to modem-sleep so the average current is still only ~25 mA over what the
other apps draw.

## Discovery — 3 tiers, automatic

1. **NVS** `last_ip` (instant if you've connected before).
2. **mDNS** `serveros.local`.
3. **UDP broadcast** on port 5050:
   * watch sends `LINKOS?`
   * Pi replies `LINKOS!:<ip>:5000`

If `WiFi.localIP()` starts with `10.42.0.` we know we're on the Pi's
hotspot and skip discovery entirely — the Pi is always at `10.42.0.1`.

## Going back to LauncherOS

LinkOS deliberately has **no on-screen "Back to Launcher" card** — it stays
out of the way like a real network utility.

To return to LauncherOS, hold **BOOT (GPIO 0) for 1.5 s** anywhere in the
app (same as every other sibling).  This OTA-flashes `/WATCH/bin/launcheros.bin`
from the SD card with a live progress bar overlay, then `esp_restart()`s
back into LauncherOS.  If the file is missing or corrupt it falls back to
the OTA partition-flip path so you can always escape.

## Network card (bottom of the home screen)

Two buttons, side by side:

| Button     | Action                                                                 |
| ---------- | ---------------------------------------------------------------------- |
| **Optus**  | Connect to the hardcoded "home" WiFi (`Optus_9D2E3D` — same `HC_WIFI_*` constant LauncherOS uses).  Re-runs Pi discovery on success. |
| **ServerOS** | Connect to the Pi's own hotspot (`ServerOS` / `SeRVEROS`) and probe `10.42.0.1:5000` directly. |

Both buttons persist the chosen SSID/PW into NVS so it sticks across boots.

## Screens

| Screen   | Trigger                            | What it does                               |
| -------- | ---------------------------------- | ------------------------------------------ |
| Splash   | Boot                               | ~1.4 s LINK//OS animation                  |
| Home     | After splash                       | Battery / clock / link dot / vitals / 6-tile grid / Network card |
| Apps     | Tile: Apps                         | List from `/api/apps`, tap to POST `/api/run/<slug>` |
| Mirror   | Tile: Mirror                       | Live JPEG of ServerOS @ 4 fps + tap → click |
| Log      | Tile: Log                          | Last 20 HTTP exchanges                     |
| Launcher | **BOOT (GPIO 0) 1.5 s** only        | OTA flash `/WATCH/bin/launcheros.bin` → LauncherOS |

## Endpoints expected on the Pi

These all live in `apps/linkos/` and `apps/linkos-mirror/` of the
**ServerOS Improvements** pack — drop those folders into the Pi and the
endpoints exist:

```
GET  /api/ping              — link probe
GET  /api/system            — CPU/RAM/temp/uptime/host/mode
GET  /api/apps              — registry of every ServerOS app
POST /api/run/<slug>        — trigger app's linkos_run() handler
POST /api/note              — append a free-form note
POST /api/power/reboot      — reboot the Pi
GET  /mirror/frame          — current dashboard JPEG (410×502)
POST /mirror/click          — { "x": 0..410, "y": 0..502 }
POST /mirror/scroll         — { "dy": pixels }
POST /mirror/goto           — { "url": "/app/terminal" }
```

## File checklist

- [x] Single `.ino` (no `.h` / `.cpp` companions)
- [x] Matches LauncherOS partition scheme
- [x] Matches LauncherOS pin map & driver versions
- [x] Custom in-app loading splash (LINK//OS)
- [x] Battery pill + clock + link dot from `/api/ping`
- [x] Back-to-Launcher pill + BOOT long-press
- [x] Auto-creates its SD subdirs (`/WATCH/link/{log,cache,notes}`)
- [x] WiFi 3-tier connect, NVS cache, mDNS, UDP discovery
- [x] Mirror live-remote with tap injection
- [x] BOOT-button (GPIO 0) safety exit at any time

Built for the cyberdeck ecosystem.  Have fun.
