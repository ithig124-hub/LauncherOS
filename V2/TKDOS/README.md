# TKD OS — Taekwondo Watch OS 🥋

Single-sketch Arduino firmware for the **Waveshare ESP32-S3-Touch-AMOLED-2.06** (410×502 CO5300 QSPI AMOLED + FT3168 touch + SD_MMC).
Built on **LVGL 8.4** with a belt-based aesthetic that evolves as you rank up: White → Yellow → Green → Blue → Red → Black.

---

## ✨ Features

- **Belt theme system** — pick your belt from an **Omnitrix-style circular dial**, saved to NVS (defaults to White on first boot). Colours, accents, and fonts change for each belt.
- **6 bundled belt-specific fonts** — Plus Jakarta Sans (White), Barlow Condensed Black (Yellow), Syne Bold (Green), Space Grotesk Bold (Blue), Anton (Red), Cormorant Garamond (Black). All pre-converted from your `Fonts.zip` to LVGL 8.4 `.c` sources — no runtime font loading needed.
- **Transformation sequence** — every belt change fires a full-screen power-up: expanding belt-coloured shockwave, pulsing core, hangul flash, then boots into the new theme. Boboiboy / Omnitrix vibes.
- **Watchface with Omnitrix emblem** — 300 px circular dial with the belt hangul, big clock, and tagline. **Hold the emblem** to open the transform dial.
- **Infinite app carousel** — swipe left/right forever through: `Watchface ↔ Stretches ↔ Moves ↔ Settings ↔ (loop)`
- **Sub-apps** — swipe **up** to open a sub-app, **down** to return.
  - Stretches → Workouts (randomised reps/hold times)
  - Settings → Transform dial (belt selector)
- **Stretches** — 8 stretches with `.raw` image thumbnails loaded from SD + captions
- **Moves** — 29 Taekwondo techniques (Kicks / Strikes / Blocks / Stances) with Korean names, English translations and depth descriptions
- **Wallpaper picker** — belt-locked (you only see wallpapers from your current belt's folder)
- **Auto SD bootstrap** — the whole `/WATCH/TKDOS/...` tree is created for you on first boot
- **Back to launcher** — flips otadata back to the factory partition (LauncherOS parity) with SD-image fallback. Also triggered by holding the BOOT button for 1.5 s.

---

## 📦 What's in this bundle

```
TKDOS/
├── TKDOS/
│   ├── TKDOS.ino          # the single .ino you flash
│   ├── pin_config.h       # Waveshare 2.06" pin map
│   ├── partitions.csv     # 16 MB layout (factory + ota_0), LauncherOS-compatible
│   ├── tkd_white_34.c     # Plus Jakarta Sans ExtraBold @34px (White belt)
│   ├── tkd_yellow_34.c    # Barlow Condensed Black @34px  (Yellow belt)
│   ├── tkd_green_34.c     # Syne Bold @34px               (Green belt)
│   ├── tkd_blue_34.c      # Space Grotesk Bold @34px      (Blue belt)
│   ├── tkd_red_34.c       # Anton @34px                   (Red belt)
│   └── tkd_black_34.c     # Cormorant Garamond SemiBold @34px (Black belt)
├── SD_TEMPLATE/           # copy this to your microSD root
│   └── WATCH/
│       ├── TKDOS/
│       │   ├── Theme/{WhiteBelt,YellowBelt,GreenBelt,BlueBelt,RedBelt,BlackBelt}/
│       │   │   └── *.raw          <-- drop wallpapers here (belt-locked)
│       │   ├── Stretches/          <-- drop 320×320 RGB565 .raw here
│       │   └── Moves/              <-- reserved
│       └── bin/                    <-- optional launcheros.bin (fallback)
└── README.md
```

Every folder is also auto-created by the firmware on first boot if you skip the template.

---

## 🔧 Arduino IDE setup

1. **Board**: install `esp32` package by Espressif (v3.x). Select **ESP32S3 Dev Module**.
2. **Board options**:
   - Flash Size: **16MB (128Mb)**
   - Partition Scheme: **Custom** — copy `partitions.csv` next to `TKDOS.ino` and it will be picked up automatically.
   - PSRAM: **OPI PSRAM**
   - USB CDC On Boot: **Enabled**
3. **Libraries** (drop into `~/Arduino/libraries/`):
   - `lvgl` — v8.4.x (matches the Waveshare demo's `lv_conf.h`; you can copy [this one](https://github.com/waveshareteam/ESP32-S3-Touch-AMOLED-2.06/blob/main/examples/arduino/libraries/lv_conf.h) next to your `lvgl` folder)
   - `Arduino_GFX_Library` (Waveshare fork — bundled at `examples/arduino/libraries/Arduino_GFX` in that repo)
   - `Arduino_DriveBus_Library` (Waveshare fork — same repo, `libraries/Arduino_DriveBus`)
4. Open `TKDOS.ino`, hit **Upload**.
5. Copy the `SD_TEMPLATE/WATCH/` folder to the root of your microSD (FAT32).

---

## 🖼 Asset formats

### Stretch images

- **Format**: headerless RGB565 (little-endian), **320×320 px**
- **Naming**: exact case-sensitive filenames the firmware expects:
  - `Butterfly.raw`
  - `Forward Splits.raw`
  - `Hip Rotations.raw`
  - `Pigeon.raw`
  - `Side Bend.raw`
  - `Side Splits.raw`
  - `Single leg crouch.raw`
  - `Touch touches.raw`
- **Convert with ImageMagick**: `magick input.png -resize 320x320^ -gravity center -extent 320x320 -depth 8 rgb565:Butterfly.raw`
- Missing image = the app shows a placeholder card telling you exactly which file it wants.

### Wallpapers

- Same 320×320 RGB565 `.raw` format
- Drop into `/WATCH/TKDOS/Theme/<Belt>/Wallpapers/` — you'll only see them when the corresponding belt is active.

### Fonts

- LVGL 8.4 needs fonts converted to `.c` files via the [LVGL Font Converter](https://lvgl.io/tools/fontconverter). The 6 belt-specific title fonts (Plus Jakarta Sans, Barlow Condensed Black, Syne Bold, Space Grotesk Bold, Anton, Cormorant Garamond SemiBold) are already converted and shipped in this bundle as `tkd_<belt>_34.c` next to the sketch — no SD assets needed.

---

## 🎨 Belt aesthetic (built in)

| Belt   | Vibe                        | Primary   | Accent    |
|--------|-----------------------------|-----------|-----------|
| WHITE  | The Beginning — warm white  | `#171717` | `#B8D8E8` |
| YELLOW | The Spark — dark + gold     | `#FFC400` | `#FFDA44` |
| GREEN  | The Growth — deep forest    | `#00A86B` | `#5CFFB0` |
| BLUE   | The Sky — infinite futurist | `#0066FF` | `#00C2FF` |
| RED    | The Power — restraint       | `#D00000` | `#FF2020` |
| BLACK  | The Master — mastery + gold | `#B89B5E` | `#D00000` |

Each belt uses its Korean characters (백帯 / 黄帯 / 綠帯 / 青帯 / 紅帯 / 黒帯) on the watchface + boot splash.

---

## 🧭 Navigation cheat sheet

| Gesture               | Where                | What it does                    |
|-----------------------|----------------------|---------------------------------|
| Swipe LEFT / RIGHT    | Any top-level app    | Cycle through apps (looped)     |
| Swipe LEFT / RIGHT    | Stretch / Move detail| Prev / next item                |
| Swipe UP              | Stretches            | Open Workouts sub-app           |
| Swipe UP              | Settings             | Open Belt picker                |
| Swipe DOWN            | Any sub-app          | Back to parent                  |
| Tap SHUFFLE           | Workouts             | Re-roll reps + hold times       |
| Hold BOOT 1.5 s       | Anywhere             | Back to launcher (partition flip)|
| **Hold watchface emblem** | Watchface        | **Open transform dial**          |
| **Tap belt orb**      | Transform dial       | **Trigger full transformation**  |

---

## ⚙️ Back-to-launcher

TKD OS installs into the `ota_0` partition. `partitions.csv` matches LauncherOS byte-for-byte, so:

1. **Preferred path** — `esp_ota_set_boot_partition(factory)` + `esp_restart()`. Instant, zero SD I/O.
2. **Fallback** — if the factory slot has been wiped, TKD OS streams `/WATCH/bin/launcheros.bin` through `Update.h` into `ota_0`, then reboots. Put your launcher's `.bin` there once.

---

## 📄 Licence

MIT — do whatever you want, credit appreciated.
 whatever you want, credit appreciated.
