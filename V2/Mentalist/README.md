# The Mentalist — WATCH/Mentalist

A personal intelligence OS for the DualCore watch. One sketch, one folder, all
state on the SD card. Built for the same board and launcher family as the other
V2 apps.

```
WATCH/Mentalist/
  Mentalist.ino     the whole app (single .ino)
  lv_conf.h         LVGL 8.4 config: keyboard + textarea enabled
  partitions.csv    the DualBootLauncher 16 MB table
  sketch.yaml       board options, PartitionScheme=custom
```

## Contracts with LauncherOS

| Concern | Behaviour |
| --- | --- |
| Boot | Launched from LauncherOS as an SD `.bin` into `ota_0`; the app never claims the boot partition. |
| Back to launcher | Hold **BOOT** (GPIO0) for 1.5 s, or press **RETURN TO LAUNCHEROS** on the System screen. Saves, fades, re-points the boot partition at `factory`, restarts. |
| Screen timeout | 15 s without touch fades the panel to black; a touch wakes it with a 2 s grace window. BOOT while dark only wakes the screen. |
| Clock | Shared `rtcsave` NVS contract: restored on boot, published every 5 min and on exit. |

Swipe right anywhere for the same back action as the on-screen BACK button.

## Flash layout (partitions.csv)

The DualBootLauncher table, 16 MB exactly, no gaps:

```
0x9000     nvs        20 KB
0xe000     otadata     8 KB
0x10000    factory     3 MB    permanent DualBootLauncher — read only here
0x310000   ota_0       6 MB    scratchpad: whichever OS was picked last
0x910000   storage   6.9 MB    SPIFFS for sub-app asset cache
```

This app ships no assets to `storage` and never writes `factory`, so a bad
session can only ever damage its own `ota_0` slot. `return_to_launcher()` sets
the boot partition back to `factory` before restarting.

## Storage

Created on first boot under `/WATCH/Mentalist`, never deleted:

```
CASES/cases.tv          RECORDS/records.tv      CONNECTIONS/links.tv
KNOWLEDGE/cards.tv      DECISIONS/decisions.tv  SETTINGS/settings.json
CACHE/draft.tv          EXPORTS/  BACKUPS/  LOGS/boot.csv
QUESTION/ MEMORY/ SESSIONS/   (reserved)
```

Every store is rewritten atomically: `.tmp` → verify byte count → previous file
kept as `.bak` → rename. A missing store falls back to its `.bak`, stale `.tmp`
files are discarded on boot, and a failed write is reported on screen instead of
being shown as saved.

## V1 scope

- **Observe** — quick capture (12 record types, evidence status, optional event
  date, case), case files, timeline, evidence wall with manual red-thread links
  (7 relations), question board, clue tracker, search.
- **Remember** — knowledge cards with review / blind recall / roulette.
- **Reason** — the pause, three perspectives, decision journal with confidence.
- **Learn** — knowledge vault, reading journal, personal dictionary, ideas.
- **Assist** — focus timer, system, backup / export / restore.

Not in V1 (the screens are reachable and say so): the daily challenge pool,
logic lab, and any assistant that would move data off the watch. No network
code is compiled in.

## Build

Open the folder with Arduino IDE 2 / `arduino-cli`. `sketch.yaml` selects:

```
esp32:esp32:esp32s3 (UploadSpeed=921600, USBMode=hwcdc, CDCOnBoot=cdc,
CPUFreq=240, FlashMode=qio120, FlashSize=16M, PartitionScheme=custom,
PSRAM=opi, LoopCore=1, EventsCore=1, EraseFlash=none)
```

`PartitionScheme=custom` makes the build use `partitions.csv` from this folder,
so the 3 MB / 6 MB table travels with the sketch. If your board package instead
exposes this table under a named menu entry, that name works too — the offsets
are what matter.

Libraries: LVGL **8.4.x**, Arduino_GFX, Arduino_DriveBus, XPowersLib. No IMU or
radio library is required.

Verified here with `arduino-cli` against esp32 core 3.3.12:

- exit 0, `Sketch uses 1,485,119 bytes` — 24% of the 6 MB `ota_0` slot
  (the core prints the custom scheme's maximum as the full 16 MB, so the slot
  budget is checked against `0x600000` by hand)
- `Global variables use 207,004 bytes (63%)` of the 320 KB DRAM, ~120 KB left
  for the LVGL task and stack
- the table's offsets were checked for gaps: `0x9000 → 0x1000000`, exact

Hardware behaviour is still unverified: touch mapping, AMOLED brightness steps,
SD timing on the real card, and the launcher handover all need a run on the
watch.
