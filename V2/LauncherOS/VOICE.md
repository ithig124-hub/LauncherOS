# LauncherOS V2 + voice — what was added and how to flash it

This folder is `V2/LauncherOS` from
`github.com/ithig124-hub/LauncherOS/tree/main/V2/LauncherOS`
with the voice mod ported into it. The launcher is the same one you already
had; nothing from the template was removed.

## 1. What the port adds

| Area | Change |
|---|---|
| Audio | `ESP_SR` (MultiNet, COMMAND mode) driven by an `I2SClass` mic on BCLK 41 / LRCK 45 / MCLK 16 / DIN 42. The mic is an **ES7210 ADC** (MIC1 left, MIC2 right) that must be woken over I2C 0x40 with the vendor register script before its SDOUT carries anything — see `es7210_start()`. No wake word: **press BOOT once — the match runs by itself** (after a `HEARD:` flash, or when the ~6 s window closes). |
| Partitioning | `partitions.csv` grows `factory` to 5 MB and adds a `model` partition at `0x910000` (3.9 MB). `ota_0` drops to 4 MB so the table still totals 16 MB. |
| Model provisioning | `sd_provision_model()` copies `srmodels.bin` from the card into the blank `model` partition on boot (splash shows `voice model nn%`), then never runs again. |
| Commands | 30 MultiNet phrases (29 commands + the `xander` always-on gate): the ten OS `.bin`s (`astro`, `spotify`, `taekwondo`, `sport`, `game`, `explore`, `tactical`, `book`, `waypoint`, `dual link`) plus clock, brightness, radios, battery/status, restart, shutdown, puzzles/knowledge, `return home`. |
| UI feedback | Overlay pill on `lv_layer_top()`: `Opening mic…`, `LISTENING - speak now`, `HEARD: "…"`, or the exact failure reason (`No voice model`, `Not enough RAM`, `Mic did not start`, `Voice hung - reboot`). The panel is woken first if it was asleep, kept lit for the whole session, and held on through command execution (the idle countdown is paused while a phrase runs and restarted after it), so voice feedback — including the SD → OTA update screen — is never painted onto a dark panel. |
| Card handling | App folder is discovered instead of assumed: `/WATCH/bin`, `/WATCH/BIN`, `/WATCH/apps`, `/bin`, `/apps`, plus one level down from `/WATCH` and from the card root. A folder that actually contains `.bin` files beats an empty decoy. AppleDouble (`._name.bin`) files are skipped. |
| Recovery | Opening the Apps screen with an empty list rescans, so a card inserted after boot appears without a reboot. |
| Safety | If `model` is missing/blank the feature disables itself with one log line and everything else behaves as before. Sessions close after 6 s (MultiNet's own window is 5.76 s) and run whatever was heard. A BOOT press on a sleeping screen wakes the panel and opens the window in the same press. The BOOT button itself can be switched off in `Xander > settings > BOOT to Talk` (NVS `xander/bootmic`). A failed bring-up never leaves the CPU at 240 MHz. |
| Freeze fix | `sr_start()` in `esp32-hal-sr.c` calls `vTaskDelete(NULL)` on its error path when an ESP-SR task fails to allocate — from `loop()` that deleted the launcher's own task, freezing the watch on the last frame (`Opening mic...`). Only `ESP_SR.begin()` now runs on a throwaway task, with an 8 s deadline; a bring-up that never reports back shows `Voice hung - reboot` and latches voice off for the boot instead of freezing. |

## 2. Flash it — one Upload, nothing else

The partition table in this folder **is** the "DualCore 5MB APP / 3.9MB MODEL
(16MB)" layout, so the menu entry in the core (`boards.local.txt`) matches it
and its `upload.extra_flags=0x910000 {build.path}/srmodels.bin` appends the
model write to the launcher write. One Upload = one `write-flash` call:
`0x0` bootloader, `0x8000` partitions, `0xe000` boot_app0, `0x10000` launcher,
`0x910000` model. Nothing goes to `ota_0` (`0x510000`) — that is only used at
runtime when the launcher runs an app from the card, and the model lives in a
data partition, not an app slot.

Arduino IDE → Tools:

* Board: **ESP32S3 Dev Module** (16 MB flash, OPI PSRAM, USB CDC on boot)
* Partition Scheme: **DualCore 5MB APP / 3.9MB MODEL (16MB)**
* Erase All Flash Before Sketch Upload: **Disabled**

Then Upload. One upload, nothing else.

From a terminal (the exact options this build was verified with):

```
arduino-cli compile --fqbn "esp32:esp32:esp32s3:UploadSpeed=921600,USBMode=hwcdc,CDCOnBoot=cdc,FlashSize=16M,PartitionScheme=dualfive,DebugLevel=none,PSRAM=opi,CPUFreq=240,FlashMode=qio120,EraseFlash=none" .
```

If the model partition is blank (for example after an erase-all upload or a
flash from `merged.bin`), you do **not** need to reflash from the PC: copy
`srmodels.bin` from this folder to `/WATCH/srmodels.bin` on the card once and
reboot. The launcher writes it into `0x910000` itself.

## 3. Card layout

```
/WATCH/bin/            (or /bin, /apps, /WATCH/apps — any case)
    AstroOS.bin
    SpotifyOS.bin
    ...
    categories.txt     (optional)
/WATCH/srmodels.bin    (only needed to heal a blank model partition)
```

The boot log names what it found:

```
[scan] app folder is /bin         -> the folder actually used
[scan] found 12 apps              -> how many passed the filter
[scan] card root holds: (...)     -> only printed when nothing was found
```

Voice:

```
[mic]  i2c probe: ES7210 found, ES8311 found     <- once, at boot
[ptt]  model @ 0x910000 (3968 KB) ready
[ptt]  press BOOT to talk (30 phrases) - button enabled
[mic]  ES7210 at 0x40 running: 16 kHz, 16-bit, MIC1 left / MIC2 right, gain 30 dB
[ptt]  I2S open: 16 kHz / 16-bit / mono-left, MCLK 4.096 MHz
[ptt]  listening... (30 phrases, 6s window)
[ptt]  matched "spotify"
[ptt]  session ended after 2380ms, id=3 ("spotify")
```

### "-99 dBFS, peak 0" from Test Microphone

That exact pair means *digital silence*: the bus is clocking, but every sample is
zero.  On this board it is never a level or gain problem — it is one of:

* **`ES7210 ** MISSING **` in the boot probe.**  No codec, no mic.  The mic is
  ES7210 at I2C 0x40..0x43 on SDA 15 / SCL 14; the ES8311 (0x18) is output only.
* **The codec was never started.**  Reset defaults leave the ADC powered down, so
  `es7210_start()` must run *after* the I2S clocks are up (MCLK 16 = 256 × 16 kHz
  = 4.096 MHz).  Its log line is the proof it ran.
* **Wrong pins.**  There is no raw I2S mic on this board.  17 is the SD card CS,
  18 is unused, and an INMP441-style read on 17/18/16 returns zeros forever —
  which is exactly what v7.0 did before the ES7210 fix.

## 4. If voice still says "No voice model"

Check the `model` partition, not the sketch:

1. `model` must exist at `0x910000` — that only happens with the partition
   scheme above, not with `custom`/`default`.
2. `srmodels.bin` must be 3 340 296 bytes. Anything smaller is a partial copy.
3. `[model] blank at 0x910000 and no srmodels.bin on the card` means the card
   is mounted but the file is not at any of the six probed paths.

## 5. Build (verified in this folder)

Built with arduino-cli 1.5.1, core 3.x, `PartitionScheme=dualfive`,
`EraseFlash=none`:

| | |
|---|---|
| sketch | 3 857 939 B of 5 242 880 B `factory` (73 %) |
| globals | 164 728 B (50 %) |
| `partitions.bin` | `factory 0x10000/5M · ota_0 0x510000/4M · model 0x910000/3.9M · storage 0xCF0000/3.1M` |

Artifacts in `build/`:

| file | md5 |
|---|---|
| `LauncherOS-v2.0-voice.bin` (app only) | `9497ab0bfc754e502b14e243d7121480` |
| `LauncherOS-v2.0-voice.merged.bin` (16 MB, **no model** — also needs the model write) | `7534aa1a1b3a6765311253d75f7512db` |
| `partitions.bin` | `2f81343771ef352337522de2e7377dce` |
| `srmodels.bin` (3 340 296 B) | `8de3e9c912ab2a8486cf1de1a3d469d4` |

Voice and card strings confirmed inside the shipped app image: `LISTENING -
No command heard`, `No voice model`, `/WATCH/srmodels.bin`, `card root holds:`,
`app folder is`, `Test Microphone`.
