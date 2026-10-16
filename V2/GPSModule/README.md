# Xander GPS — two-firmware system

Two Arduino sketches that talk over one **stable BLE protocol** from day one.

```
        LauncherOS
            │            (app-partition switch, GameOS style)
            ▼
        Xander GPS API   →  GPS.getLocation(), GPS.getStatus(), ...
            │
            ▼
        Xander GPS BLE   →  XANDER_GPS_SERVICE
            │  BLE
            ▼
        XIAO ESP32-S3    ──UART──►  L76K GNSS
```

| File            | Board                                   | BLE role            |
|-----------------|-----------------------------------------|---------------------|
| `GPSOS.ino`     | Waveshare ESP32-S3 Touch AMOLED 2.06    | Central / Client    |
| `GPSModule.ino` | Seeed XIAO ESP32-S3 + L76K GNSS         | Peripheral / Server |

The watch **never** talks to the L76K. It only speaks the Xander GPS protocol to
the XIAO. Swap the GNSS chip later and `GPSOS.ino` never changes.

---

## GPSOS.ino (watch)
- GPS OS is an **application inside** the existing watch ecosystem — it does **not**
  replace LauncherOS. Hold **BOOT** (1.5 s) to return to LauncherOS via
  `esp_ota_set_boot_partition()` (identical mechanism to GameOS).
- 7 screens: **Home, Location, Tracking, Around Me, History, Module, Settings**
  (menu → tap to open, swipe-right or BACK to return). Tapping a **History** row
  opens a **Track Detail** screen with route stats + a **per-point speed graph**
  (lv_chart).
- **Reconnect banner**: a persistent top strip shows `SEARCHING MODULE…` /
  `RECONNECTING…` on every screen while the BLE link is down, so always-on
  tracking status is never ambiguous.
- **Always-on dot**: a small persistent indicator (top-right) on every screen
  whenever the module is in always-on mode.
- **Wake At Time**: Module screen → **WAKE AT** opens an HH:MM picker; the watch
  tells the module to deep-sleep and wake at that exact clock time (UTC/GPS).
- **Screen-off after X seconds** (ported from LauncherOS `SCREEN_OFF_TIMEOUT_MS`):
  the AMOLED fades to black after inactivity and a tap (TP_INT) wakes it. The
  timeout (15 s / 30 s / 1 min / 2 min / 5 min / Never) is chosen in **Settings →
  Screen off after** and persisted in NVS. `WAKE_GRACE_MS` prevents wake→re-sleep.
- **ALWAYS ON** control on the Tracking screen: one tap starts continuous tracking
  on the module that runs until turned off (STATUS reflects `always`).
- "Around Me" uses a **local POI database** (haversine distance + bearing). **No
  vector maps.**
- Caches the latest fix/status/tracking over BLE and exposes the **Xander GPS API**
  (`GPS.getLocation()`, `GPS.isConnected()`, `GPS.startTracking()`, …) so LauncherOS
  can show `GPS connected` in its status bar without opening the app.
- SD cache: creates `WATCH/GPS/` on the card if missing.

**Libraries:** Arduino_GFX_Library, Arduino_DriveBus_Library, lvgl 8.4 (+ `lv_conf.h`),
XPowersLib, **NimBLE-Arduino ≥ 1.4.x**, **ArduinoJson ≥ 6**.

## GPSModule.ino (XIAO + L76K)
- Background GPS service (TinyGPSPlus) always running — keeps logging even if the
  watch disconnects.
- BLE server exposes `XANDER_GPS_SERVICE`; handles all commands.
- Tracking with intervals 1/5/10/30/60/300 s, unlimited/timed/scheduled.
- **ALWAYS-ON mode**: continuous GNSS + tracking that runs **until turned off** —
  forces unlimited duration, blocks all sleep/standby, **persists across reboot**,
  and auto-restarts if anything stops it. Toggle via the watch (Tracking → ALWAYS
  ON) or `{"always":true/false}` on the CONFIGURATION characteristic. **Auto-enabled
  when USB power is detected** (requires `USB_SENSE_PIN` configured).
- **Wake-at-time**: `{"wake_hhmm":"HH:MM"}` schedules a deep-sleep wake at an exact
  clock time (UTC from the GPS clock); reported back in STATUS as `"wake"`.
- Storage: `/GPS/current.json`, `/GPS/tracks/YYYY-MM-DD_track_NNN.gpx`,
  `/GPS/history/history.json`.
- Power modes: ACTIVE / TRACKING / STANDBY / SCHEDULED / DEEP_SLEEP (RTC timer +
  ext0 wake). **The watch configures the wake schedule BEFORE deep sleep** (BLE is
  not kept during deep sleep). ALWAYS-ON overrides all sleep.
- **FLIGHT_MODE**: GNSS + SD logging ON; WiFi/BT/LoRa OFF.
- Identifies as `device_type="gps"`, `device_name="Xander GPS"`, `protocol_version=1`.
- Rich serial debug at 115200.

**Libraries:** TinyGPSPlus, **NimBLE-Arduino ≥ 1.4.x**, **ArduinoJson ≥ 6**,
SD/FS (ESP32 core), esp_sleep (ESP32 core).

---

## ⚠️ Hardware values you MUST set
Every board-specific value is marked with `>>> CHANGE` / `>>> HARDWARE` in the code.

**GPSModule.ino:** `GPS_UART_RX_PIN`, `GPS_UART_TX_PIN`, `GPS_UART_BAUD`,
`L76K_STANDBY_PIN`, `SD_*_PIN` (or `SD_ENABLED 0`), `BAT_ADC_PIN`/`BAT_ADC_RATIO`,
`USB_SENSE_PIN`, `WAKE_GPIO_PIN`.

**GPSOS.ino:** pins already match the Waveshare 2.06 demo repo — change only if your
wiring differs.

## Shared BLE contract — keep both files in sync
See `PROTOCOL.md`. UUIDs + command bytes are duplicated verbatim in both sketches.

## Build (Arduino IDE / arduino-cli)
1. Boards Manager → **esp32 by Espressif** core.
2. Install the libraries listed above.
3. `GPSOS.ino`: select the Waveshare ESP32-S3 board, enable **PSRAM**, use a
   partition scheme with a **factory/app partition** for the LauncherOS return.
4. `GPSModule.ino`: select **XIAO_ESP32S3**.

> These are ESP32 firmware sketches — they compile/flash from the Arduino toolchain
> with the vendor libraries; they are not runnable inside a plain Linux container.
