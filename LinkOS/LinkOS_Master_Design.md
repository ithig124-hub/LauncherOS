# LinkOS — Master Design Document

> Single-file `.ino` submodule for the
> [LauncherOS](https://github.com/ithig124-hub/LauncherOS) cyberdeck ecosystem.
> Links a Waveshare ESP32-S3 Touch AMOLED 2.06 watch to a Raspberry Pi Zero 2 W
> running [ServerOS](https://github.com/ithig124-hub/ServerOS) over WiFi (LAN +
> Pi hotspot) and BLE, with **zero edits required** to any existing OS module.

---

## Table of contents

1. Concept & constraints
2. Repos analysed (LauncherOS + ServerOS)
3. Architecture
4. Connection / discovery flow
5. File layout in the LauncherOS repo
6. UI (LVGL 8.x, 410 × 502)
7. ServerOS-side blueprint (Tier-1, additive only)
8. Mirror feature (view + use Pi's actual screen on the watch)
9. Mirror adaptation to the watch
10. Book / library transfer (WiFi + BLE + sneakernet)
11. Reports from every OS module
12. Full feature catalogue — 120 ideas
13. Build order — shortlist
14. Power, libraries, board settings
15. Roadmap & open questions

---

## 1. Concept & constraints

- **Goal**: a sibling app (`LinkOS`) that joins the existing fleet of
  `GameOS / MediaOS / SportOS / BookOS / PomoOS / NavOS / MapOS / VaultOS /
  HackOS / DevOS / NocturneOS / StudyOS / TacticalOS / MinimalOS / MediaOS`
  on the watch, **without touching any of them**.
- **Hardware**: Waveshare ESP32-S3 Touch AMOLED 2.06, single `.ino`, same
  toolchain (Arduino IDE 2.x, LVGL 8.3/8.4, Arduino_GFX_Library,
  Arduino_DriveBus_Library, XPowersLib).
- **Network**: home WiFi (mDNS `serveros.local`) or the Pi's own hotspot
  (`ServerOS` / `SeRVEROS` → `10.42.0.1`). Plus optional BLE for direct
  phone-to-watch transfers.
- **Protocol**: HTTP REST over port 5000 (ServerOS's existing Flask port),
  augmented with new sibling Flask blueprints in `apps/linkos-*/` on the Pi.

---

## 2. Repos analysed

### LauncherOS (Waveshare watch firmware)

- Multiple "OS" modules, each a single `.ino` compiled to `*.bin` and dropped
  into `/WATCH/bin/` on the SD card.
- Shared WiFi creds in `/WATCH/wifi/config.txt`:
  ```
  WIFI1_SSID=MyHomeNetwork
  WIFI1_PASS=secretpassword
  WIFI2_SSID=OfficeWiFi
  WIFI2_PASS=officepw
  GMT_OFFSET=8
  ```
- Each module: custom splash → home with battery / clock / sync dot →
  Back-to-Launcher card + BOOT (GPIO 0) long-press 1.5 s safety exit.
- One-shot NTP sync at boot, radio killed afterwards (~70-100 mA saving).
- Wallpaper system: `/WATCH/PHOTOS/*.raw` (410 × 502 × 2 bytes = 411,884 B
  RGB565), face index in shared NVS key `face / wp_idx`.
- Per-app SD subdirs: `/WATCH/games/`, `/WATCH/media/{music,anime,manga}/`,
  `/WATCH/workouts/`, etc., auto-created on first run.

### ServerOS (Pi Zero 2 W dashboard / native UI)

- Mobile-first, glassmorphic, modular: `apps/<slug>/manifest.json` +
  optional `routes.py` auto-discovered.
- Three access modes:
  | Mode      | Address                       | Trigger                                              |
  |-----------|-------------------------------|------------------------------------------------------|
  | Local WiFi| `http://serveros.local:5000`  | watch & Pi on same SSID (via Avahi mDNS)             |
  | Hotspot   | `http://10.42.0.1:5000`       | `nmcli device wifi hotspot ssid ServerOS password SeRVEROS` |
  | Remote    | `http://<tailscale-ip>:5000`  | Tailscale, out of scope for v1                        |
- Built-in apps: Chem Suite, DropShare, Web Terminal, Phone Beacon, Speed
  Checker, MediaGrab, Focus Glass, VaultOS, Backup Manager, Power Control,
  Resource Guard, App Smith, Theme Engine (Midnight / Glassic / Cyberpunk).
- Service: `serveros.service` / `severos.service` (both spellings in repo),
  managed by `systemctl`.

---

## 3. Architecture

```
┌────────────────── ESP32-S3 (watch) ──────────────────┐
│  linkos.ino  (single file)                           │
│   ┌──────────────┐ ┌──────────────┐ ┌──────────────┐ │
│   │ wifi_manager │→│ pi_discovery │→│  pi_client   │ │
│   │ cfg + STA    │ │ mDNS/IP/UDP  │ │ HTTP + JSON  │ │
│   └──────┬───────┘ └──────┬───────┘ └──────┬───────┘ │
│          ▼                ▼                ▼         │
│   ┌──────────────────────────────────────────────┐   │
│   │  LVGL UI  splash → home → apps / mirror /    │   │
│   │  library / log                               │   │
│   └──────────────────────────────────────────────┘   │
│   ┌──────────────┐ ┌──────────────┐ ┌──────────────┐ │
│   │ NVS state    │ │  SD logger   │ │ BLE GATT     │ │
│   │ last_ip/ssid │ │ /WATCH/link/ │ │ book ingress │ │
│   └──────────────┘ └──────────────┘ └──────────────┘ │
└──────────────────────────────────────────────────────┘
                    ▲  HTTP :5000   ▲  ESP-NOW (peers)
                    │               │
                    ▼               ▼
┌────────────── Pi Zero 2 W (ServerOS) ────────────────┐
│  Flask app, existing apps untouched                  │
│                                                      │
│  NEW sibling apps (additive only):                   │
│   apps/linkos/        ← /api/system /apps /run /...  │
│   apps/linkos-mirror/ ← /mirror/frame /click /...    │
│   apps/linkos-library/← /library/list /<id>/<n>      │
│   apps/linkos-reports/← /reports/upload /<wid>       │
│   apps/linkos-rules/  ← automation rules engine      │
└──────────────────────────────────────────────────────┘
```

---

## 4. Connection / discovery flow

```
            ┌──────────────────────────────┐
            │ read /WATCH/wifi/config.txt  │
            └──────────────┬───────────────┘
                           ▼
            ┌──────────────────────────────┐
            │ try NVS last_ssid (instant)  │
            └──────────────┬───────────────┘
                           │ ok?
                  ┌────────┴─────────┐
                 yes                no
                  │                  │
                  ▼                  ▼
   ┌──────────────────────┐  ┌──────────────────────────┐
   │ scan WIFI1, WIFI2…   │  │ scan SSID `ServerOS`     │
   │                      │  │ pass `SeRVEROS`          │
   └──────────┬───────────┘  └────────────┬─────────────┘
              ▼ connected                 ▼ connected
   ┌──────────────────────┐  ┌──────────────────────────┐
   │ Discovery #1: mDNS   │  │ Use fixed 10.42.0.1       │
   │ `serveros.local`     │  │ (Pi is the gateway)       │
   ├──────────────────────┤  └────────────┬─────────────┘
   │ Discovery #2: cached │                ▼
   │ NVS last_ip          │   ┌──────────────────────────┐
   ├──────────────────────┤   │ probe http://<ip>:5000/  │
   │ Discovery #3: UDP    │   │ → save NVS, link OK      │
   │ broadcast :5050      │   └──────────────────────────┘
   │ "LINKOS?" / reply    │
   │ "LINKOS!:<ip>:5000"  │
   └──────────┬───────────┘
              ▼
       same probe + save
```

Mode detect: if `WiFi.localIP()` matches `10.42.0.x` we're on the hotspot and
skip mDNS / UDP entirely.

---

## 5. File layout in the LauncherOS repo

```
LauncherOS/
├── LinkOS/
│   ├── linkos.ino                ← single sketch
│   ├── README.md                 ← per-module readme
│   └── LinkOS_Master_Design.md   ← this document
└── Bin files/
    └── linkos.bin                ← after Sketch → Export Compiled Binary
```

On the SD card:

```
/WATCH/bin/linkos.bin
/WATCH/link/
├── log/                  ← rolling http log per session
├── cache/serveros.json   ← last /api/apps response (offline cache)
├── notes/queue.txt       ← offline-queued notes, flushed on reconnect
├── clips/                ← screen recordings from /mirror/recording
├── gestures.json         ← user-customised gesture macros
├── pins.json             ← quick-launch grid pinned URLs
├── rules.json            ← watch-side automation rules
└── config.txt            ← module-local prefs (auto_return, fps caps, etc.)
```

---

## 6. UI (LVGL 8.x, 410 × 502)

### Screens

1. **Splash (~1.4 s)** — packet-sweep animation; "LINK//OS" types letter by
   letter; same boot-block aesthetic as siblings.
2. **Home**
   - Top bar: battery pill • clock • link dot (🔴 offline → 🟠 wifi only → 🟢 API ok).
   - Row 1: mode chip (LAN / HOTSPOT / OFFLINE) • Pi IP • RTT ms.
   - Row 2: vitals (CPU % / RAM % / temp °C / uptime).
   - 6-tile action grid: Refresh • Mirror • Library • Reports • Note • Reboot.
   - Full-width **Back to Launcher** card (cyan border, dark surface).
3. **Apps** — list from `/api/apps`. Tap → POST `/api/run/<slug>` + toast.
4. **Mirror** — live view + remote control of the Pi (see section 8).
5. **Library** — browse + download books from the Pi (see section 10).
6. **Reports** — local view of last sync; full data lives on the Pi.
7. **Log** (long-press top bar) — last 20 HTTP exchanges, scrollable.

Wallpaper reuses the launcher's selected `face / wp_idx` from shared NVS.

---

## 7. ServerOS-side blueprint (Tier 1)

A new sibling app — never edits existing code.

```python
# apps/linkos/routes.py
from flask import Blueprint, jsonify, request
import psutil, socket, subprocess, time, os, json

bp = Blueprint("linkos", __name__, url_prefix="/api")

@bp.get("/system")
def system():
    with open("/sys/class/thermal/thermal_zone0/temp") as f:
        temp_c = int(f.read()) / 1000.0
    return jsonify({
        "cpu":    psutil.cpu_percent(interval=0.2),
        "ram":    psutil.virtual_memory().percent,
        "temp":   round(temp_c, 1),
        "uptime": int(time.time() - psutil.boot_time()),
        "host":   socket.gethostname(),
        "mode":   "hotspot" if os.path.exists("/tmp/serveros.hotspot") else "lan",
    })

@bp.get("/apps")
def apps():
    out = []
    for slug in os.listdir("apps"):
        m = os.path.join("apps", slug, "manifest.json")
        if os.path.isfile(m):
            out.append({"slug": slug, **json.load(open(m))})
    return jsonify(out)

@bp.post("/run/<slug>")
def run(slug):
    mod = __import__(f"apps.{slug}.routes", fromlist=["linkos_run"])
    fn  = getattr(mod, "linkos_run", None)
    if not fn: return ("not supported", 405)
    return jsonify(fn(request.get_json(silent=True) or {}))

@bp.post("/note")
def note():
    txt = (request.get_json(silent=True) or {}).get("text", "").strip()
    if not txt: return ("empty", 400)
    with open("data/notes.log", "a") as f:
        f.write(f"{int(time.time())}\t{txt}\n")
    return jsonify(ok=True)

@bp.post("/power/reboot")
def reboot():
    subprocess.Popen(["sudo", "reboot"])
    return jsonify(ok=True)
```

Optional UDP beacon (when Avahi/Bonjour is off):

```python
# apps/linkos-beacon/routes.py
import socket, threading
def _beacon():
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
    s.bind(("", 5050))
    while True:
        data, addr = s.recvfrom(64)
        if data.startswith(b"LINKOS?"):
            ip = socket.gethostbyname(socket.gethostname())
            s.sendto(f"LINKOS!:{ip}:5000".encode(), addr)
threading.Thread(target=_beacon, daemon=True).start()
```

---

## 8. Mirror feature — view + use the Pi's screen on the watch

### What it does

Watch becomes a VNC-style live remote for ServerOS's **actual** UI (the one
running natively on the Pi). Tap on the watch → Pi sees a click at the
corresponding coordinates. Drag → scroll. Long-press → keyboard pops up,
keystrokes get injected on the Pi.

### Architecture (framebuffer + uinput — NOT a browser)

```
ServerOS (running natively on the Pi)
                ▼
       Pi's real display (/dev/fb0 or DRM)
                │ read pixels
                ▼
┌─────────────────────────────────────────────┐
│  apps/linkos-mirror/  (new sibling service) │
│   capture loop ── /dev/fb0 → JPEG           │
│   input  loop ── /dev/uinput ← {x,y,click}  │
└──────────┬──────────────────────────────────┘
           │ HTTP :5000
           ▼
   Watch (linkos.ino) — JPEG poll @ 4 fps + tap forwarding
```

### Capture strategies (auto-detected at boot)

- **Framebuffer** (`/dev/fb0`) — fastest, no compositor needed; reads pixels
  directly. Works on a Pi running ServerOS in kiosk / direct-draw mode.
- **X11** — `import Xlib.display` or shell `scrot -o /tmp/f.png`.
- **Wayland** — `grim -t jpeg -q 60 -`.
- **DRM/KMS** — `python-drm`, same speed as `/dev/fb0`.

### Input injection (uinput)

```python
from evdev import UInput, AbsInfo, ecodes as e
W, H = 1920, 1080   # actual Pi screen
ui = UInput({
    e.EV_ABS: [(e.ABS_X, AbsInfo(0,0,W,0,0,0)),
               (e.ABS_Y, AbsInfo(0,0,H,0,0,0))],
    e.EV_KEY: [e.BTN_TOUCH, e.BTN_LEFT],
}, name="linkos-virtual-touch")

def click(wx, wy):
    px = int(wx * W / 410); py = int(wy * H / 502)
    ui.write(e.EV_ABS, e.ABS_X, px); ui.write(e.EV_ABS, e.ABS_Y, py)
    ui.write(e.EV_KEY, e.BTN_TOUCH, 1); ui.syn()
    ui.write(e.EV_KEY, e.BTN_TOUCH, 0); ui.syn()
```

### Capture (framebuffer)

```python
import os
from PIL import Image
def grab_fb0():
    w = int(open("/sys/class/graphics/fb0/virtual_size").read().split(",")[0])
    h = int(open("/sys/class/graphics/fb0/virtual_size").read().split(",")[1])
    bpp = int(open("/sys/class/graphics/fb0/bits_per_pixel").read())
    fd = os.open("/dev/fb0", os.O_RDONLY)
    buf = os.read(fd, w*h*(bpp//8)); os.close(fd)
    mode = "RGB" if bpp == 24 else ("RGBA" if bpp == 32 else "BGR;16")
    img = Image.frombytes(mode, (w, h), buf)
    img.thumbnail((410, 502))
    return img
```

### HTTP surface

```
GET  /mirror/frame?mode=fit|pan|crop&x=&y=&w=&h=
POST /mirror/click   {x,y}
POST /mirror/scroll  {dy}
POST /mirror/text    {str}
POST /mirror/key     {key}        e.g. "Enter", "Escape", "Alt+Left"
GET  /mirror/info                  → {width, height, mode}
```

### Watch-side resource cost

- JPEG q55 @ 410 × 502 ≈ 15-25 KB/frame.
- At 4 fps that's ~80 KB/s on the LAN — trivial on local WiFi.
- No Chromium memory cost (was 150 MB in the early design, now 0).

---

## 9. Mirror adaptation to the watch

Three modes selectable via the `mode=` query parameter on `/mirror/frame`:

| Mode  | Behaviour                                  | Best for             |
|-------|--------------------------------------------|----------------------|
| Fit   | Whole screen scaled to 410 × 502 letterbox | Overview / nav       |
| Pan   | Native pixels, drag to pan                 | Reading, terminals   |
| Crop  | 200 × 200 zoom at `(x,y)` — pinch to enter | Fine-tap toggles     |

Watch-side gestures the Pi never sees:

| Gesture               | Action                                          |
|-----------------------|-------------------------------------------------|
| Pinch-zoom            | Switches to `mode=crop&x=…&y=…&w=200&h=200`     |
| Two-finger drag       | `/mirror/scroll` (never registers as a click)   |
| Edge-swipe right      | Sends key `Alt+ArrowLeft` (browser-back style)  |
| Long-press top bar    | Pop URL bar → `/mirror/goto` (if applicable)    |
| Triple-tap            | Toggle watch-skin CSS on/off live               |

---

## 10. Book / library transfer (3 transports)

Books land in `/WATCH/books/<title>/` (text/epub) or
`/WATCH/media/manga/<title>/page_NNN.raw` (RGB565 pages). BookOS just finds
them on next scan — **no edits**.

### Path 1 — WiFi pull (Pi → watch)

Pi runs an auto-converter (Pillow / `wkhtmltoimage`): drop any
`.pdf / .epub / .cbz / .jpg` into `data/inbox/` → renders 410 × 502 RGB565
pages into `data/library/<title>/page_NNN.raw` + `meta.json`.

```
GET  /library/list                → JSON tree
GET  /library/<title>/meta        → {pages, type, size}
GET  /library/<title>/<n>         → raw RGB565 page n
GET  /library/<title>/text        → utf8 .txt
```

Watch streams pages with resumable progress (sync.idx per book) into a
`.tmp/` folder, then atomically renames.

### Path 2 — BLE push (phone → watch, no Pi)

ESP32-S3 advertises a GATT service while on the "Receive Book" screen:

```
Service UUID  : 7a4e0001-link-books-watch (128-bit, custom)
  Char 0x01 WRITE       — manifest  {name, type, pages, total_bytes, sha1}
  Char 0x02 WRITE+NR    — chunk     20-244 B body (MTU-aware)
  Char 0x03 READ+NOTIFY — progress  {received, total, ok|err}
  Char 0x04 WRITE       — control   {cmd: "start|abort|commit"}
```

Phone side options:
- nRF Connect / LightBlue with saved macro (zero phone code).
- Web Bluetooth PWA hosted on the Pi at `/send-book` (no app store, no signing).
- Native Flutter / Expo app `link-pocket` (optional v2).

Realistic throughput: ~80 KB/s on BLE 5.0 with DLE + 247-byte MTU.

### Path 3 — Sneakernet helper

For monster downloads: watch shows a QR + 6-digit PIN, parks itself in AP
mode for 60 s, browser drag-and-drop at `http://<watch-ip>:80/upload`. No
phone app, no Pi.

---

## 11. Reports from every OS

PiLinkOS only runs when its `.bin` is loaded — but each existing OS already
writes its data to SD or NVS. On boot, LinkOS scrapes, diffs, ships.

| OS         | Already writes to…                                  | Report contents                                        |
|------------|-----------------------------------------------------|--------------------------------------------------------|
| SportOS    | `/WATCH/workouts/*.csv` + NVS `sportos`             | pace, km, calories, XP, level, streak, daily series    |
| GameOS     | `/WATCH/games/saves/` + NVS `gameos`                | high scores, play time per game                        |
| MediaOS    | `/WATCH/media/{music,anime,manga}/`, NVS `mediaos`  | watchlist, play counts, manga read counter             |
| BookOS     | NVS `bookos` (last-page) + `/WATCH/books/.read/`    | reading streak, pages/day                              |
| PomoOS     | NVS `pomoos`                                        | daily focus minutes, distractions                      |
| StudyOS    | session logs + NVS `studyos`                        | minutes per subject                                    |
| NavOS/MapOS| GPS waypoints (if logged) + tile usage              | places visited, distance                               |
| HackOS/DevOS| scratch files, scripts                             | last edited / last command                             |
| VaultOS    | encrypted blob                                      | ONLY `{last_access_ts, item_count}` — no contents      |
| NocturneOS | sleep logs (if any)                                 | duration, wake consistency                             |
| TacticalOS | session logs                                        | last used + session length                             |
| MinimalOS  | minimal NVS                                         | last booted timestamp                                  |
| LauncherOS | NVS `launcher`                                      | most-used apps, launch counts                          |

Flow:

```
on LinkOS boot
   ├─ scan /WATCH/ (all known data folders)
   ├─ open NVS read-only for each known namespace
   ├─ build JSON: { ts, watch_id, reports: {sportos:{…}, …} }
   ├─ diff against /WATCH/link/cache/last.hash
   ├─ POST /reports/upload  with only the delta
   └─ on success → bump hash; on fail → keep delta for next boot
```

Pi side (`apps/linkos-reports/routes.py`):

```
POST /reports/upload          ← delta uploads
GET  /reports/<watch_id>      ← rendered HTML dashboard
GET  /reports/<watch_id>.json ← raw export
GET  /reports/leaderboard     ← multi-watch comparison
```

---

## 12. Full feature catalogue — 120 ideas

Legend: `[Z]` zero-edit, `[N]` needs a new sibling Flask blueprint, `[H]`
needs a small hardware add-on.

### Tier 1 — Works out of the box with `apps/linkos/`

1. Home pulls `/api/system` → CPU%, RAM%, temp, uptime, hostname, mode (1.5 s poll).
2. "Open Apps" pulls `/api/apps` → renders every ServerOS app as a tappable list.
3. Tap an app row → `POST /api/run/<slug>` → app's `linkos_run()` handler decides.
4. Tap "Note" → `POST /api/note` → appended to `data/notes.log`; queues offline.
5. Tap "Reboot" → `POST /api/power/reboot`.
6. Long-press top bar → Log screen with last 20 HTTP exchanges.
7. Connection state dot: red → amber → green.

### Tier 2 — Each existing ServerOS app gets a 5-line `linkos_run()` (additive)

8.  Chem Suite — push today's quiz score; trigger 5-question quiz.
9.  DropShare — toggle upload mode; report USB mount + free space.
10. Web Terminal — send preset commands; show last stdout line.
11. Phone Beacon — watch can be the beacon; confirm map cursor.
12. Speed Checker — kick off speedtest; live Mbps result.
13. MediaGrab — pause/resume queue; show queue length.
14. Focus Glass — start/stop Pomodoro on the Pi.
15. VaultOS — physical-presence-style lock from the wrist.
16. Backup Manager — trigger backup; poll % complete.
17. Theme Engine — cycle Midnight / Glassic / Cyberpunk.
18. Power Control — reboot / shutdown / one-click update with swipe confirm.
19. Resource Guard — receive high-temp / low-storage alerts.

### Tier 3 — Cross-OS integrations (zero edits)

20. SportOS → Pi — auto-upload workout CSVs to `/data/workouts/`.
21. MediaOS → Pi — sync `watchlist.txt`, pull MediaGrab queue.
22. BookOS → Pi — push books from `/data/books/` to `/WATCH/manga/`.
23. VaultOS ⇄ Pi VaultOS — two-way encrypted sync, hotspot-only.
24. GameOS → Pi — global leaderboard via on-game-over POST.
25. NavOS / MapOS → Pi — fetch cached OSM tiles on demand.
26. PomoOS ⇄ Focus Glass — bidirectional Pomodoro mirroring.
27. MinimalOS — "📡 ping pi" tile for instant peace of mind.

### Tier 4 — Bidirectional / push (v2, after WebSocket upgrade)

28. Doorbell / motion alerts → vibrate + toast on the watch.
29. CI/CD pings (e.g., "build #471 passed").
30. Calendar reminders from Pi cron.
31. Tailscale presence — peer-online notifications.
32. Music now-playing — `mpd` / Plex tile with play/pause control.

### Tier 5 — Identity / security

33. Proximity unlock — dashboard auto-unlocks when watch heartbeats present.
34. Wake-on-presence — dashboard wakes on watch arrival (RSSI threshold).
35. Auto-lock on walk-away — VaultOS locks after 30 s without heartbeats.
36. Signed actions — `PI_TOKEN=` HMAC in `/WATCH/wifi/config.txt` → `X-LinkOS-Token`.

### Tier 6 — Polish & delights

37. Wallpaper sync — dashboard "Send to watch" button reformats JPG → 410×502 RGB565.
38. OTA firmware push — dashboard hosts latest `.bin`; one-tap flash from watch.
39. Time fix — sync RTC from `/api/system.now` when NTP is blocked.
40. Field hotspot UX — auto-hide tiles needing internet when on Pi hotspot.

### Mirror & dashboard (the headline feature)

41. Full live remote control of ServerOS via Mirror (section 8).
42. Mirror Fit / Pan / Crop modes.
43. Pinch-to-zoom.
44. Two-finger scroll.
45. Edge-swipe back.
46. Long-press URL bar.
47. Triple-tap to toggle watch-skin CSS.

### Pure-watch enhancements (no Pi changes)

48. Auto-reconnect on wake (250 ms cached IP probe).
49. Latency heat-strip sparkline at top of mirror screen.
50. Offline draft notes in `/WATCH/link/notes/queue.txt`.
51. Per-SSID profile — different favourite path per network.
52. Boot-direct-to-Mirror (hold crown during splash).
53. Battery-aware fps (>50% = 4 fps, 20-50% = 2 fps, <20% = 0.5 fps + dim).
54. Tap-to-zoom region.
55. Gesture macros in `/WATCH/link/gestures.json`.
56. Quick-launch grid (8 pinned dashboard URLs in `pins.json`).
57. PIN lock on watch.
58. Always-On status face (LauncherOS watchface variant).
59. Wallpaper hotswap — save current mirror frame as wallpaper.

### Pi-side additions inside `apps/linkos-mirror/` only

60. `GET /mirror/recording?seconds=10` — return MJPEG/GIF of last N seconds.
61. `GET /mirror/text-snapshot` — Playwright/`pyautogui` text scrape; search-and-jump.
62. `GET /mirror/speed` — server-side speedtest-cli.
63. `GET /mirror/stats` — per-core CPU, rx/tx, disk I/O, top 5 RAM procs.
64. `GET /mirror/hotspot/on` — flip the Pi into hotspot mode from the watch.
65. `GET /mirror/tailscale` — `tailscale status --json` parsed.
66. `POST /mirror/eject` — unmount USB safely.
67. `POST /mirror/shell {cmd:"presetA"}` — whitelisted preset only.
68. `GET /mirror/gpio/<pin>` — toggle Pi GPIO.
69. `GET /mirror/cam` — Pi-cam JPEG.
70. `GET /mirror/np` — `mpd` / `playerctl` "now playing".
71. `GET/POST /mirror/clip` — Pi clipboard via `xclip` / `wl-copy`.

### Cyberdeck-only wow features

72. Walkie-Talkie — watch records → POST `/mirror/voicemail` → plays on Pi speaker.
73. Geofence triggers — RSSI rules in `/WATCH/link/geofence.json`.
74. Cross-cyberdeck handoff — two watches share Mirror state via stateful session.
75. Phone-as-second-display — same Mirror endpoints work from a phone browser.
76. Pi-as-watch-printer — `POST /mirror/print {text}` → USB thermal printer.
77. Replay mode — minutely dashboard snapshots; scrub back 24 h.

### Books & library

78. Auto-convert PDF/EPUB/CBZ → 410×502 RGB565 pages in `data/inbox/`.
79. OCR existing manga folder via `POST /library/<title>/ocr`.
80. Cross-OS reports HTML dashboard at `serveros.local:5000/reports`.
81. Weekly digest webhook (Discord / email) Sunday night.
82. Public read-only share token for reports.
83. Auto-build "best of" books from saved highlights.
84. Speech-to-book via Whisper from `.mp3` audiobooks.

### Cross-device & sharing

85. Phone tells watch what to fetch — Web BLE PWA picker → watch downloads via WiFi.
86. Field librarian — Tailscale-published library from anywhere.
87. NFC tap-to-add — sticker → PWA → book on watch.
88. Borrow mode — time-limited signed URLs, TTL sidecar auto-delete.
89. Reading sprint — two watches race; Pi dashboard livestream.

### ESP-NOW peer-to-peer (no router needed)

90. Watch-to-watch chat — pure ESP-NOW, no Pi, no WiFi.
91. Pi-as-ESP-NOW-gateway — $3 dongle plugged into Pi forwards to ServerOS.
92. Coop book sharing — push pages between watches via ESP-NOW.
93. Find-my-cyberdeck — every watch broadcasts heartbeat with RSSI distance.
94. Cross-watch leaderboards — GameOS scores swap silently when near.
95. Range alarm — friend's watch drops out of ESP-NOW range → buzz.

### Sensors / physical world

96.  Shake-to-lock vault via IMU.
97.  Morse-tap commands on the chassis → preset Pi actions.
98.  Wrist gestures = browser keys (flick left/right/twist).
99.  IR blaster via Pi GPIO; watch sends TV codes.
100. Ambient-light → dashboard brightness sync.
101. Pi humidity/temp tile from DHT22.
102. Watch as USB HID over BLE — keyboard for ANY computer.
103. Tap-the-bezel scroll on the mirror.

### Voice / audio

104. Pi-side TTS pager via Piper.
105. Watch buzzer as Morse pager for sensitive messages.
106. RTTTL ringtone pack — different tone per app.
107. Voice memo via Pi mic → Whisper transcribe → toast back.

### Automation engine

108. `rules.json` on the watch — `if pi.cpu > 80 for 60s then buzz`.
109. Pi-side `apps/linkos-rules/` IFTTT-style engine.
110. Sunrise/sunset triggers + day/night wallpaper auto-switch.
111. Calendar pull — next-event countdown tile.
112. Geofence-driven dashboard — RSSI rules → auto-open most relevant app.
113. Habit nudges — "you haven't opened BookOS in 3 days" at 8 PM.

### Privacy & secrets

114. Dead-man's-switch — no check-in for N hours → Pi runs cleanup script.
115. Panic gesture — quintuple-tap link dot → lock-down sequence.
116. Honeypot mode — fake dashboard for unknown watch MACs.
117. Watch-only AES — VaultOS key lives only on the watch.
118. Ephemeral notes — auto-delete after N minutes unless pinned.

### Developer / hacker flow

119. Wireless OTA from Arduino IDE — watcher on Pi pushes new builds.
120. Serial-over-WiFi — `Serial.println` mirrored to a TCP socket.

(Plus 50+ more in the conversation: Tamagotchi mode, ASCII rain idle,
procedural MAC-derived splash glyph, charging-aware sync, two-way clipboard,
SD card pie chart, hot-seat games over the mirror, MAC roaming whitelist,
decoy boot, geo-ratcheted secrets, magic-shake ECDH pairing, local-AI tile
with llama.cpp, question-of-the-day, flashcard buzz, parental dashboard,
replay-yesterday, weekly digest.)

---

## 13. Build order — recommended shortlist

If implementing in priority order:

1. **`apps/linkos/routes.py`** (80 lines) — gets you Tier 1 immediately.
2. **`apps/linkos-mirror/routes.py`** (framebuffer + uinput, 120 lines) — the
   headline value. Watch becomes a real wrist remote.
3. **`scr_mirror` LVGL screen** in `linkos.ino` — full-screen JPEG poll + tap
   injection + drag-scroll.
4. **Quick-launch grid + battery-aware fps + latency sparkline** — polish that
   signals "this is engineered, not a toy."
5. **`apps/linkos-library/routes.py`** + library screen on watch — books in.
6. **`apps/linkos-reports/routes.py`** + report scraper on watch — stats out.
7. **BLE GATT block** in `linkos.ino` — phone-to-watch book transfers.
8. **ESP-NOW block** — peer-to-peer features once you have multiple watches.
9. **Automation rules engine** — turns the watch from "remote" into "trigger."
10. **Personality features** (procedural splash, Tamagotchi, ASCII rain) —
    the hook that makes people show their friends.

---

## 14. Power, libraries, board settings

- **Board**: ESP32-S3 Dev Module
  - USB CDC On Boot: **Enabled**
  - Flash Size: **16 MB**
  - **Partition Scheme: Minimal SPIFFS (large APP)** — must match LauncherOS
  - PSRAM: **OPI PSRAM**
  - CPU: **240 MHz (WiFi)**
- **Libraries** (all already pulled in by sibling OSes):
  - LVGL 8.3 or 8.4
  - Arduino_GFX_Library
  - Arduino_DriveBus_Library
  - XPowersLib
  - ArduinoJson (already a transitive dep)
- **Radio policy**: LinkOS is the **one** module that does **not** kill the
  radio after NTP — it keeps STA alive in foreground, parks WiFi to
  modem-sleep when the screen is off (saves ~50 mA, keeps reconnect <300 ms).
- **Back-to-Launcher**: identical card + identical OTA-stream of
  `/WATCH/bin/launcheros.bin` + BOOT (GPIO 0) long-press 1.5 s safety exit.

---

## 15. Roadmap & open questions

### v1.0 ship list

- WiFi connect (3-tier) + discovery (mDNS / cached / UDP) + hotspot fallback.
- `apps/linkos/` blueprint on the Pi.
- Home + Apps + Mirror + Library + Reports screens.
- Back-to-Launcher + BOOT safety exit.

### v1.1

- BLE GATT book ingress.
- ESP-NOW peer chat.
- Watch-skin CSS layer for Mirror.

### v2

- WebSocket live tickers (drop polling).
- Pi → watch push notifications via temporary `WiFiServer` on port 80.
- Tailscale-aware remote mode (`TS_IP=` line in `wifi/config.txt`).
- `PI_TOKEN=` HMAC auth.

### Open questions

- Exact resolution of the Pi's native display (1920×1080? 1280×720? Pi Zero
  2 W with a small DSI panel?) — drives Mirror's W/H constants in the
  uinput translation.
- Does ServerOS use a compositor (X/Wayland) or draw direct to fb?
  Decides which capture path is default.
- BLE coexistence: watch radio policy when both BLE GATT and WiFi STA are
  active — needs bench testing.

---

*Built for the LauncherOS cyberdeck ecosystem. — LinkOS v0.1.0 design draft.*
