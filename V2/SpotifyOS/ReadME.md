# SpotifyOS

Offline, Spotify-inspired music player for the **Waveshare ESP32-S3-Touch-AMOLED-2.06**
watch. One sketch: `SpotifyOS.ino`.

## Files

| File | Purpose |
|---|---|
| `SpotifyOS.ino` | the whole application (single .ino) |
| `lv_conf.h` | LVGL 8.4 config, must sit next to the .ino (same file LauncherOS/AstroOS use) |
| `../dist/SpotifyOS.bin` | prebuilt app image (2.43 MB) for LauncherOS |

## Build

```
arduino-cli compile \
  --fqbn "esp32:esp32:esp32s3:FlashSize=16M,PartitionScheme=app3M_fat9M_16MB,PSRAM=opi,CPUFreq=240,FlashMode=qio,USBMode=hwcdc,CDCOnBoot=default,UploadSpeed=921600,DebugLevel=none" \
  SpotifyOS
```
Board: **ESP32S3 Dev Module**, PSRAM **OPI**, Flash **16 MB**.
Libraries: `lvgl 8.4.x`, `Arduino_GFX_Library`, `Arduino_DriveBus`, `XPowersLib`,
`ESP32-audioI2S` (4.0.0).

Verified result on this toolchain (arduino-esp32 3.3.12 / ESP32-audioI2S 4.0.0):
`Sketch uses 2,434,887 bytes (77%)`, `Global variables 167,156 bytes (51%)`.

## Install on the watch

1. Put `SpotifyOS.bin` in `/WATCH/bin/` on the SD card (LauncherOS lists `/WATCH/bin/*.bin`).
2. LauncherOS copies it into `ota_0` and reboots into it.
3. Maps are created on first launch:
```
/WATCH/SpotifyOS/
├── Music/<Playlist>/*.mp3 + playlist.json
├── Data/{library,liked,history,queue,stats}.json
├── Config/{settings,theme}.json
├── Wallpapers/          raw RGB565 410x502 .raw / .bin
├── Cache/artwork/       optional square RGB565 <folder>__<file>.raw
└── Logs/boot.log
```
Optional playlist order:
`{"schema":1,"name":"Chill","order":["002.mp3","001.mp3"]}`

## Controls

- Tap the back arrow / swipe **right** = back. Swipe **left** = Now Playing.
- **Hold BOOT 1.5 s** = save and reboot into LauncherOS (also in Settings).
- Screen timeout 10–60 s (Settings → Screen timeout); BOOT wakes it.

## Honest limitations

- **Bluetooth Audio is not available on this watch.** The ESP32-S3 has BLE only: no
  Bluetooth Classic A2DP, no LE-Audio playback profile. Selecting it reports
  `HARDWARE NOT AVAILABLE`, playback is refused, and there is **no automatic fallback**
  to the speaker. It becomes real only with an added A2DP transmitter / audio companion
  (`SPOTIFY_HAS_BT_AUDIO_HW` in the sketch).
- Wallpapers and album art are **raw RGB565 only** (no JPEG/PNG decoder is linked in).
- ID3: v2.3/v2.4 text tags in Latin-1 or UTF-8; UTF-16 tags are skipped and the filename
  is used instead. Duration is a CBR estimate, corrected once the track plays (VBR files
  may read slightly wrong until then).
- Clock is the ESP32 system clock persisted in NVS; it is UTC (no timezone UI yet).
- Seeking is `ESP32-audioI2S`'s `setAudioPlayTime()`; accuracy follows the decoder.
- Compile-verified only — no watch was available, so audio output, power draw and touch
  calibration were not measured on hardware.