# LauncherOS

**A DualCore watch launcher for ESP32-S3 AMOLED smartwatches, built on LVGL 8.3/8.4.**

LauncherOS turns your ESP32-S3 + AMOLED wearable into a fully functional
smartwatch with a watch face, app launcher, battery monitor, and OTA firmware
loader â€” all driven by swipe gestures on a 410 Ã— 502 round display.

---

## âœ¨ Features

### ðŸ–¥ï¸ Three-Screen Carousel

Navigate between screens with horizontal swipes. The carousel loops seamlessly
(`Watch Face â†’ Apps â†’ Battery â†’ Watch Face â€¦`) with directional slide
animations and a navigation cooldown to prevent double-fires.

| Screen | Index | Description |
|--------|-------|-------------|
| **Watch Face** | 0 | Large clock, date, battery indicator, wallpaper photos |
| **Apps** | 1 | Scrollable list of `.bin` apps from SD card with favourites |
| **Battery** | 2 | Big arc gauge, % / voltage, time-remaining estimate, deep-sleep button |

### âŒš Watch Face

- **Big translucent time** (`HH:MM`) with a drop-shadow for legibility on any
  wallpaper.
- **Date line** (`Thu 15 May`) beneath the clock.
- **Status dot** (top-right): green = NTP-synced, amber = syncing, red = no
  WiFi.
- **Battery pill** (top-left): fill bar + bolt icon when charging + percentage
  label.
- **Photo wallpapers** â€” tap the lower Â¾ of the screen to cycle through
  RGB565 `.raw` / `.bin` images stored in `/WATCH/PHOTOS/` on the SD card.
  Falls back to a subtle dark gradient when no photos are present.

### ðŸ“± App Launcher (OTA Loader)

- Scans `/WATCH/bin/` on the SD card for compiled `.bin` firmware images.
- **Tap** an app â†’ full OTA copy with a progress bar + percentage overlay.
  On completion the watch reboots into the new firmware.
- **Long-press** an app â†’ toggle â­ favourite. Favourites are persisted to NVS
  (`Preferences`) and float to the top of the list with a gold highlight.
- Footer hint shows swipe directions for adjacent screens.

### ðŸ”‹ Battery Screen

- **270Â° arc gauge** coloured by charge level (green â†’ amber â†’ red).
- **Large percentage** label centred inside the arc.
- **Charging state** with âš¡ icon, or runtime estimate
  (`~ 3 h 22 min remaining`) based on configurable capacity / draw constants.
- **Voltage readout** from the AXP2101 PMU.
- **"Sleep Now" button** â€” enters ESP32 deep sleep. Only the BOOT button
  (GPIO 0) can wake the device. Draw drops to ~150â€“300 ÂµA.

### ðŸš€ Boot Splash

- Letter-by-letter "LauncherOS" title with a scanline sweep effect.
- Progress bar with live status messages (`mounting SD`, `connecting wifi`,
  `scanning apps`, â€¦).
- Hardware-ID footer derived from the ESP32's eFuse MAC.
- Fast-wake path: when waking from deep sleep the splash completes in ~100 ms
  because WiFi is skipped (internal RTC retains the time).

### ðŸ“¡ WiFi & NTP

- Reads up to 5 SSIDs + passwords from `/WATCH/wifi/config.txt` on the SD
  card, plus a hardcoded fallback network.
- Connects once at boot, syncs the clock via `pool.ntp.org`, then **shuts WiFi
  off entirely** to save ~70â€“100 mA.
- Re-syncs every 6 hours (configurable `WIFI_RESYNC_INTERVAL_MS`) â€” WiFi is on
  for ~6 seconds each cycle, giving < 0.05 % radio uptime.
- `GMT_OFFSET` read from the config file.

### âš¡ Power Saving

| Feature | Detail |
|---------|--------|
| CPU frequency scaling | 240 MHz during boot/OTA, 80 MHz otherwise |
| WiFi off after sync | Radio disabled until the next 6-hour resync |
| Bluetooth disabled | `btStop()` at boot â€” never used |
| Screen-off timeout | 10 s of no touch â†’ AMOLED brightness 0 |
| LVGL skip when off | `lv_timer_handler()` not called while panel is dark |
| Deep sleep mode | Manual "Sleep Now" â†’ ext1 wake on GPIO 0 (~150 ÂµA) |
| Optional light sleep | Compile-time flag `LIGHT_SLEEP_WHEN_OFF` (disabled by default) |

### ðŸ”„ Navigation System (v5.1)

The horizontal swipe navigation mirrors the proven pattern from
[S3_MiniOS_206](https://github.com/ithig124-hub/ESP32-MINI-Ino/tree/main/S3_MiniOS_206):

- **`navigationLocked` flag** â€” blocks new swipes while a slide animation is
  in progress, preventing the "double-swipe" / skipped-screen bug.
- **Directional slide animations** â€” swiping left slides the next screen in
  from the right (`LV_SCR_LOAD_ANIM_MOVE_LEFT`); swiping right does the
  opposite.
- **Proper index wrapping** â€” `wrap_screen()` normalises the target index
  *before* the same-screen guard, so edge transitions (screen 2 â†’ 0, screen
  0 â†’ 2) always resolve correctly.
- **Pending-swipe clear** â€” any stale queued gesture is discarded on every
  successful navigation so old swipes never "replay" after a screen change.
- **200 ms cooldown** (was 250 ms in v5) for snappier response while still
  filtering touch noise.

### ðŸ—‚ï¸ SD Card Layout

```
/WATCH/
â”œâ”€â”€ bin/            # Compiled .bin firmwares (app launcher)
â”œâ”€â”€ PHOTOS/         # RGB565 .raw / .bin wallpapers (410Ã—502)
â””â”€â”€ wifi/
    â””â”€â”€ config.txt  # WiFi credentials + GMT offset
```

#### `config.txt` format

```ini
# Up to 5 networks
WIFI1_SSID=MyHomeNetwork
WIFI1_PASS=secretpassword
WIFI2_SSID=OfficeWiFi
WIFI2_PASS=officepw

# Hours east of UTC (e.g. 8 for AWST, -5 for EST)
GMT_OFFSET=8
```

---

## ðŸ› ï¸ Hardware

| Component | Part |
|-----------|------|
| MCU | ESP32-S3 (dual-core, PSRAM) |
| Display | 410 Ã— 502 AMOLED (CO5300 driver, QSPI) |
| Touch | FT3168 capacitive controller (IÂ²C) |
| PMU | AXP2101 (battery %, voltage, charge detect) |
| Storage | SD card (1-bit SDMMC) |

### Pin Map

| Function | GPIO |
|----------|------|
| QSPI SDIO 0â€“3 | 4, 5, 6, 7 |
| QSPI SCLK | 11 |
| QSPI CS | 12 |
| LCD RESET | 8 |
| IÂ²C SDA / SCL | 15 / 14 |
| Touch INT | 38 |
| Touch RESET | 9 |
| SDMMC CLK / CMD / DATA | 2 / 1 / 3 |
| BOOT button (wake) | 0 |

---

## ðŸ“¦ Dependencies

Install via the Arduino Library Manager or PlatformIO:

| Library | Purpose |
|---------|---------|
| [LVGL](https://github.com/lvgl/lvgl) (8.3 / 8.4) | UI framework |
| [Arduino_GFX_Library](https://github.com/moononournation/Arduino_GFX) | Display driver (CO5300) |
| [Arduino_DriveBus_Library](https://github.com/Xinyuan-LilyGO/Arduino_DriveBus_Library) | IÂ²C touch + bus |
| [XPowersLib](https://github.com/lewisxhe/XPowersLib) | AXP2101 PMU |
| ESP32 Arduino core â‰¥ 2.0 | WiFi, OTA, NVS, SD_MMC, deep sleep |

> **LVGL config**: make sure `lv_conf.h` enables `LV_FONT_MONTSERRAT_12`,
> `_14`, `_16`, `_18`, `_22`, and `_48` (used by the watch face and UI).
> If `LV_TICK_CUSTOM` is set to `1`, the sketch skips manual `lv_tick_inc()`
> calls.

---

## ðŸ”§ Building & Flashing

1. Open `LauncherOS.ino` in the Arduino IDE (or import into PlatformIO).
2. Select board **ESP32-S3 Dev Module** with:
   - USB CDC On Boot: **Enabled**
   - PSRAM: **OPI PSRAM** (if your board has it)
   - Flash Size: **16 MB** (or match your board)
   - Partition Scheme: **Minimal SPIFFS (large APP)** â€” you need two OTA
     slots for the app launcher.
3. Adjust `HC_WIFI_SSID` / `HC_WIFI_PASS` if you want a hardcoded fallback
   (or rely on `config.txt` on the SD card).
4. Build & upload.

---

## ðŸŽ® Usage

| Gesture | Action |
|---------|--------|
| **Swipe left** | Next screen (Face â†’ Apps â†’ Battery â†’ Face) |
| **Swipe right** | Previous screen (Face â†’ Battery â†’ Apps â†’ Face) |
| **Tap** (watch face) | Cycle wallpaper photo |
| **Tap** (app list) | OTA-load the selected `.bin` |
| **Long-press** (app list) | Toggle â­ favourite |
| **Tap "Sleep Now"** | Enter deep sleep (~150 ÂµA) |
| **Press BOOT button** | Wake from deep sleep |

The screen turns off after 10 seconds of inactivity. Any touch wakes it (with a
400 ms grace period so the wake tap isn't forwarded to the UI).

---

## ðŸ“‹ Changelog

### v5.1

- **Fixed**: Navigation loop rewritten to match S3_MiniOS_206 pattern.
  - Added `navigationLocked` flag to prevent double-swipe during slide
    animations.
  - Index wrapping now happens *before* the same-screen guard so edge
    transitions (2 â†’ 0, 0 â†’ 2) always work.
  - Directional slide animations (`MOVE_LEFT` / `MOVE_RIGHT`) give visual
    feedback.
  - Pending-swipe queue cleared on every successful navigation.
  - `NAV_COOLDOWN_MS` reduced from 250 â†’ 200 ms.
  - Navigation lock auto-releases on wake (`screen_on()`).

### v5.0

- Removed tileview; replaced with three independent LVGL screens.
- Manual touch-delta swipe detection inside the LVGL indev callback.
- Battery screen with arc gauge + time-remaining estimate.
- Boot splash with letter-by-letter title, scanline sweep, hardware-ID footer.
- Deep sleep with BOOT-button wake (GPIO 0 ext1).
- Power-saving: CPU scaling, WiFi off after sync, 6-hour periodic resync.
- Favourites persisted to NVS; wallpapers loaded from SD card.

---

## ðŸ“„ License

This project is provided as-is for personal / educational use. See the
repository root for license details.

---

## ðŸ™ Acknowledgements

- Navigation and swipe-detection patterns adapted from
  [S3_MiniOS_206](https://github.com/ithig124-hub/ESP32-MINI-Ino/tree/main/S3_MiniOS_206)
  by the same author.
- Built on [LVGL](https://lvgl.io/) and the
  [Arduino_GFX](https://github.com/moononournation/Arduino_GFX) ecosystem.
