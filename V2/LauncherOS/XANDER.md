# Xander — command reference

Everything Xander can be told to do, in one firmware.

| Piece | File | Runs when |
|---|---|---|
| **Launcher: push-to-talk + always-on** — BOOT to talk, or `xander` hands-free; typed commands too | `LauncherOS.ino` v1.5 | LauncherOS is running |
| **Headless test sketch** (optional) — Edge Impulse `Xander` detector, no UI | `Xander/Xander.ino` | that image is flashed instead |

**Voice lives in the launcher now.** There is one sketch and one mic stack:
ESP-SR MultiNet already holds every phrase, `xander` included, so the hands-free
path is a wake *gate* on the same engine rather than a second model in a second
sketch. `Xander/Xander.ino` still builds and still runs standalone; it is no
longer needed to get a wake word.

---

## Wake word / always-on (v1.5)

Default: **off**. Enable it in **Xander → settings → Always-On Xander**;
the toggle applies immediately (no reboot) and persists in NVS.

| Property | Value |
|---|---|
| Phrase | `xander` — entry 1 of `kPtt`, kind `PTT_WAKE` |
| Engine | the same ESP-SR MultiNet session push-to-talk uses |
| Gate window | `AON_GATE_MS` = 7 000 ms after a wake |
| Retry | `AON_RETRY_MS` = 8 s after a failed bring-up (never more than 3× that) |
| CPU | held at 240 MHz while the mic is open, back to 80 MHz when released |
| Battery stop | the **Battery Limit** dropdown (20 / 30 / 40 %); `Never` disables the stop |
| Trigger | say `xander`, then the command — no press |

What it does with a phrase it hears:

| Heard | Result |
|---|---|
| `xander` | overlay `Yes?`, gate open for 7 s |
| a command, gate open | stops the mic and runs it, then reopens the mic |
| a command, no wake | ignored: `[aon] ignored without wake: "spotify"` on serial, nothing on screen |
| `xander` never heard | stays listening silently — no overlay, no LED, screen still sleeps |

**BOOT still works in both modes, and this is the part worth knowing** — unless
`settings > BOOT to Talk` is switched off, in which case the button does nothing
for voice at all (it still wakes the screen from light sleep):

| Mode | Effect of pressing BOOT |
|---|---|
| Always-on **off** (default) | one press opens the mic and the match runs on its own |
| Always-on **on**, screen on | the mic is already open, so the press opens the gate (`Yes?`) instead of restarting the engine — say the command and it runs at once |
| Always-on **on**, screen off | the press wakes the screen and opens the gate in the same press |
| Voice unavailable (`No voice model`) | no auto-start at all; BOOT shows the same error it always did |

Cost, stated plainly: the mic, AFE and MultiNet stay powered and the core stays
at 240 MHz, so the battery falls noticeably faster than in pure push-to-talk.
The Battery Limit setting stops it, and stopping releases I2S completely.

Serial at boot and on every state change:

```
[aon] always-on armed
[aon] listening - say "xander" then a command
[aon] wake accepted
[aon] ignored without wake: "game"
[aon] stopped (battery limit)
```

---

## Push-to-talk (LauncherOS v1.3)

| Step | What happens |
|---|---|
| **Press BOOT** (any screen, dark panel included) | the panel is woken first if it was asleep, then overlay `Opening mic...`, I2S + ESP-SR come up (~1–2 s), then `LISTENING - speak now` |
| **Say one phrase** | MultiNet spots it; the overlay changes to `HEARD: "<phrase>"` |
| ~0.7 s later | the mic closes and the latched command executes — no second press |
| 6 s with nothing heard | the window closes, `No command heard`, mic released |
| **Press BOOT again** during the window | optional: ends the wait early and runs the match |

The `HEARD:` flash is the important bit: you see which phrase won, so a
near-miss (`sport` instead of `spotify`) is visible rather than a silent mystery.
It is not a confirmation step — the phrase runs either way.

It is a timed window, not a hold: you can lower your arm and keep talking, and
a slipped thumb does not fire a half-finished command. The mic is closed as
soon as there is nothing left to listen to (after a match, or at the 6 s cap).

Voice never runs on a black panel: opening a window calls `screen_on()` if the
panel was asleep, every `ptt_flash()` wakes it too (so an always-on `Yes?`, a
mic-test result and a command's feedback are all visible), and `lastTouchMs` is
refreshed for the whole session so the 15 s sleep timer cannot fire mid-listen.

That hold continues through **execution**, not just listening. `ptt_run()` takes
a `CommandScreenHold` before it dispatches: the panel is lit if needed, the 15 s
countdown is paused while the command runs (the loop keeps refreshing
`lastTouchMs`), and the countdown restarts from zero when it returns. So a
phrase that starts the SD → OTA app copy shows the update screen and the reboot
for however long the copy takes, and a blocking command like a WiFi sync cannot
finish on a dark panel. Commands that reboot never reach the release, which is
the intended state — the panel stays lit until the restart.

All of this is optional: `Xander > settings > BOOT to Talk` switches the button
off for voice, after which a press does nothing at all (it still wakes the
screen from light sleep).

The bring-up is also not allowed to take the watch with it. `sr_start()` in
`esp32-hal-sr.c` deletes its **caller** when one of its three tasks fails to
allocate (`vTaskDelete(g_sr_data->handle_task)` with a NULL handle), which froze
the launcher on the last painted frame — `Opening mic...` forever, no panic, no
log line. So only `ESP_SR.begin()` runs on a throwaway task; I2S and the ES7210
stay in loop context (they are quick, and `Wire` is not shared with a second
task). If that task does not report back within 8 s the overlay says
`Voice hung - reboot` and voice stays off for the rest of the boot: the panel
keeps painting and touch keeps working the whole time.

No “Xander” is needed — the button is the wake word. Say the phrase on its own.

| Phrase | Action |
|---|---|
| `astro` | launch AstroOS |
| `spotify` | launch SpotifyOS |
| `taekwondo` | launch TKDOS |
| `sport` | launch SportOS |
| `game` | launch GameOS |
| `explore` | launch ExplorerOS |
| `tactical` | launch TacticalOS |
| `book` | launch BookOS |
| `waypoint` | launch WaypointOS |
| `dual link` | launch Dual_Link |
| `what time is it` | read the clock |
| `what is the date` | read the date |
| `sync time` | WiFi + NTP |
| `check time status` | sync state, 12/24 h, offset |
| `brightness up` / `brightness down` | ±30 |
| `maximum brightness` / `minimum brightness` | 255 / 20 |
| `turn wifi on` / `turn wifi off` | radio |
| `turn bluetooth on` / `turn bluetooth off` | radio |
| `battery status` | % + charging |
| `system status` | battery, radios, SD, heap, uptime |
| `restart the watch` | reboot |
| `shut down the watch` | power off |
| `open puzzles` / `open knowledge` | enter the Xander layer on that screen |
| `return home` | leave the Xander layer |

Feedback: the overlay shows `LISTENING…`, then `Opening SpotifyOS` / the
spoken intent, then the command's answer (e.g. `Battery at 84%.`). Failures are
explicit: `No command heard`, `Voice unavailable` (ESP-SR would not start),
`Voice hung - reboot` (the engine never came back — see above), `No voice model`
(blank/missing `model` partition), `Mic did not start` (wiring),
`Not enough RAM`, `<Name> is not in /WATCH/bin`.

Serial — the whole diagnostic story, at boot and per press:

```
[ptt] 1  astro                       <- 29 lines: id -> phrase, printed once
...
[ptt] model @ 0x910000 (3968 KB) ready
[ptt] press BOOT to talk (29 phrases)
[ptt] heap before begin    internal free 214336  largest 110592  psram free ...
[ptt] I2S open: 16 kHz / 16-bit / mono-left
[ptt] heap after begin     internal free 121904  largest  61440  psram free ...
[ptt] listening... (29 phrases, auto-stop 12s)
[ptt] released after 2408ms, id=2 ("spotify")
[ptt] launch SpotifyOS
```

`heap after begin` is the number to watch: if it lands near 20–30 KB the
launcher will still run, but LVGL is close to starving — that is the case for
cutting phrases or dropping `Always-On`. The start is refused outright when the
largest internal block is below 40 KB, and after three failed starts the
feature disables itself for that boot instead of failing on every press.

### Test Microphone (new)

`Xander > settings > Test Microphone` records one second through the same I2S
path ESP-SR uses and reports level:

```
[mic] 16000 samples  dc -12.4  rms 812.3 (-32.1 dBFS)  peak 4021
[mic] level looks healthy
```

| Reading | Meaning |
|---|---|
| −20…−12 dBFS | speaking near the watch — ideal for MultiNet |
| −60…−40 dBFS | quiet room, mic working |
| below −55 dBFS | very quiet; speak closer |
| −99 dBFS / `no data` | codec silent — MCLK/DIN not reaching the ES7210, or the ADC still powered down |

### BOOT to Talk (settings toggle)

`Xander > settings > BOOT to Talk` (default **on**) decides whether the physical
button opens the mic at all. Switch it off and BOOT stops being a voice control
entirely: no `Opening mic...`, no window, no overlay — useful when the button is
bumped by a wrist or when always-on is the only way in you want. It is saved in
NVS (`xander/bootmic`) and applies immediately; flipping it off mid-window closes
the mic rather than leaving a session no press can end. Always-on sessions are
unaffected, and BOOT still wakes the screen from light sleep either way.

This is the fastest way to separate a wiring fault from a phrase-model fault:
healthy levels but wrong matches means the phrases, silence means the mic.

Requirements: the `model` partition (in `partitions.csv`) and `srmodels.bin`
flashed at **0x910000**. That write is not done by the stock `boards.txt` — it comes
from `boards.local.txt`, which adds the **DualCore 5MB APP / 3.9MB MODEL (16MB)**
entry with `upload.extra_flags=0x910000 {build.path}/srmodels.bin`. Upload on *that*
entry and the model is written for you; Upload on plain **"custom"** builds the same
table from `partitions.csv` and never writes it, and `merged.bin` never contains it
(`0x910000` reads `FF` in the 16 MB merge — verified). The one-command fallback:

```bat
esptool.exe --chip esp32s3 -p COM7 -b 921600 write_flash 0x910000 build\srmodels.bin
```

Every Upload after that leaves `0x910000` alone; only *Erase All Flash Before
Sketch Upload* (or `erase_flash`) takes it away. Without it the feature logs one
line and disables itself while every other launcher path keeps working.

### Tuning recognition

MultiNet compares whole phrases, so length and distinctness matter more than
volume. If two entries are being confused, change one in `kPtt` — `spotify`
becoming `spotify music`, or `sport` becoming `sport mode`, is usually enough,
and there is no reflash of the model partition involved: only the launch image
changes. Note that `spotify` and `taekwondo` are not dictionary words, so they
are pronounced by letter-to-sound rules; if either never matches, a plain-word
substitute (`music`, `martial arts`) is the reliable fix.

## LauncherOS commands

Enter with **swipe up** from the clock face → **COMMAND**, then type or tap a
chip. Exit with **swipe down**. Left/right still move between Puzzles, Home and
Knowledge.

### Clock

Phrases are shown the way you would say them. Punctuation is stripped before
matching, so `what's` and `whats`, or `24-hour` and `24 hour`, are the same
command.

| Phrase | Does |
|---|---|
| `sync time`, `sync clock`, `update the time`, `update time` | WiFi + NTP round-trip, publishes to NVS |
| `sync date` | same round-trip (one sync sets both) |
| `check time status`, `time status` | synced?, 12/24-hour, UTC offset |
| `what time is it`, `whats the time`, `what is the time`, `what time`, `time` | reads the RTC, honours 12/24-hour |
| `whats today's date`, `what is today's date`, `what is the date`, `whats the date`, `date` | `Today is Sunday 19 October 2026.` |
| `what day is it`, `whats the day`, `day` | weekday only |
| `open clock settings`, `clock settings` | leaves Xander on the Settings screen |
| `use twelve hour time`, `twelve hour time`, `12 hour time` | persists, repaints the face |
| `use twenty four hour time`, `twenty four hour time`, `24 hour time` | persists, repaints the face |

`sync` needs WiFi enabled and blocks ~6–10 s; the “Syncing…” label is painted
first because the round-trip runs from `loop()`, never inside an LVGL callback.

### Display

| Phrase | Does |
|---|---|
| `brightness up`, `brighter` | +30 of 255 |
| `brightness down`, `dimmer` | −30 of 255 |
| `maximum brightness`, `max brightness` | 255 |
| `minimum brightness`, `min brightness` | 20 |
| `set brightness to fifty percent`, `brightness fifty percent` | 128 |

Persisted as `dcw`/`bright`; the level is restored at boot and by
`screen_on()`, so waking never returns to a hard-coded 140.

### Radios

| Phrase | Does |
|---|---|
| `turn wifi on`, `wifi on`, `enable wifi` | re-enables the WiFi manager |
| `turn wifi off`, `wifi off`, `disable wifi` | `WiFi.disconnect` + radio off |
| `turn bluetooth on`, `bluetooth on`, `enable bluetooth` | `btStart()` |
| `turn bluetooth off`, `bluetooth off`, `disable bluetooth` | `btStop()` |

### Power

| Phrase | Does |
|---|---|
| `restart the watch`, `restart watch`, `reboot the watch` | saves the clock, `esp_restart()` |
| `shut down the watch`, `shutdown the watch`, `power off the watch` | AXP2101 hard power-off; press PWR to boot |

### Battery, storage, system

| Phrase | Answer |
|---|---|
| `battery status`, `how much battery is left`, `battery left` | % and charging |
| `show memory usage`, `memory usage` | heap and PSRAM free/total |
| `how much ram is free`, `free ram` | heap + PSRAM free |
| `show flash storage`, `flash storage` | chip size, firmware size, app-slot size |
| `show sd card status`, `sd card status` | card type + used MB |
| `how much storage is left`, `free storage` | free/total MB on the card |
| `system status` | battery, WiFi, BLE, SD, heap, uptime |

### Legacy commands (unchanged from Xander v1.0)

`give me a puzzle`, `teach me something`, `open puzzles`, `open knowledge`,
`help`, `return home`, `show battery`, `show status`, `wifi on/off`,
`bluetooth on/off`, `gps on/off`, `beacon on/off`, plus any app by name.

---

## Launching an OS

The name is resolved against the `/WATCH/bin` scan taken at boot, then the
`.bin` is OTA-copied into `ota_0` and the watch reboots into it. If the file is
not on the card, Xander says `<Name> is not on the card`.

The list comes from the **watch's** SD card (`/WATCH/bin`), not from
`V2/bin` on the PC — that folder is only where the images are staged for
copying.

### What "in `/WATCH/bin`" accepts

The file name is matched loosely, because the folder is filled from a PC:

| On the card | Launches |
|---|---|
| `AstroOS.bin` | `astro`, `AstroOS`, `open astro` |
| `dual link.bin`, `DualLink.bin`, `DUAL_LINK.BIN` | `dual link`, `Dual_Link` |
| `/WATCH/bin/AstroOS/AstroOS.bin` (one subfolder deep) | `astro` |

The folder itself may be spelled `bin`, `BIN`, `Bin`, `bins` — whatever the
card actually contains is detected at boot, not guessed.

Lookup order for a spoken name: the boot scan → a forced rescan (a `.bin`
copied on after the watch started) → a direct walk of `/WATCH/bin` on the SD.
A file found late is announced on serial as
`[scan] AstroOS picked up from /WATCH/bin after boot`. AppleDouble metadata
(`._AstroOS.bin`) and zero-byte files are ignored, so a Mac-formatted card
cannot offer a half-image for flashing.

### By name

| `.bin` | Name forms | Mood / intent handles |
|---|---|---|
| `AstroOS.bin` | astroos, astro | takemetothestars, stargazing, spacestuff, astronomytime, lookatthesky, stars |
| `SpotifyOS.bin` | spotifyos, spotify | bootsometunes, bootuptunes, playmusic, listentomusic, spotifytime, tunein, tunes, tune, songs, song, music |
| `TKDOS.bin` | tkdos, tkd | dosometkd, taekwondo, tkdtraining, practicemartialarts, martialarts |
| `SportOS.bin` | sportos, sport | itsgotime, gymtime, workouttime, letstrain, letssweat, workout, exercise |
| `GameOS.bin` | gameos, game | imbored, iwanttoplay, playagame, gametime, havesomefun, killtime, bored |
| `ExplorerOS.bin` | exploreros, explorer | letsexplore, goexploring, adventuretime, takeawalk, getoutside, explore, hike |
| `TacticalOS.bin` | tacticalos, tactical | fieldmode, tacticalmode, godark, missiontime, whatstheplan, lockitin |
| `BookOS.bin` | bookos, book | iwanttoread, readabook, readingtime, booktime, readmeastory, read |
| `WaypointOS.bin` | waypointos, waypoint | whereami, markthisspot, setawaypoint, ineeddirections, howdoigetthere, navigate |
| `Dual_Link.bin` | duallink, dual | linkup, pairwithmyphone, connecttomyphone, syncwithmyphone, connectmyphone |

### Matching rules

1. Lower-case, alphanumeric words only; `wi fi` → `wifi`, `blue tooth` →
   `bluetooth`, `dual link` → `duallink`.
2. Apostrophes fold for `I'm` / `It's` / `let's` (not for `what's`, so the
   clock phrases keep working).
3. Words are joined and searched: `its go time` → `itsgotime`.
4. A key of **6+ characters** stands alone (`imbored`, `godark`).
5. A **short** key (`music`, `read`, `stars`) needs a launch verb
   (`open/launch/start/run/boot/go/play/put`) or to be the whole sentence —
   that is why `music status` and `read the time` do **not** launch anything.
6. Names beat moods; moods beat the system phrase table.

### What does not trigger a launch

| Input | Why |
|---|---|
| `music status` | short key + extra word, no verb |
| `read the time` | short key + extra word, no verb |
| `stars in my eyes` | short key + extra words |
| `bored of this menu` | short key + extra words |

Apostrophe/typo tolerance is word-level, not sound-level: `boot of tune for
spotify` still works because `spotify` is in it; a mangled word with no
recognisable key falls through to “unknown”.

---

## Headless sketch commands (optional firmware)

Only for `Xander/Xander.ino`. The launcher does not use these — in LauncherOS
the spoken commands are the 29 phrases in the [push-to-talk table](#push-to-talk-launcheros-v13),
and they actually launch apps and change settings instead of printing.

Recognised after “Xander”, inside a 4 s window:

| ID | Phrase | `executeBootCommand()` today |
|---|---|---|
| 1 | `boot game o s` | prints `boot routine: GAME OS` |
| 2 | `boot music o s` | prints `boot routine: MUSIC OS` |
| 3 | `boot notes o s` | prints `boot routine: NOTES OS` |
| 4 | `boot diagnostics` | prints `boot routine: DIAGNOSTICS` |
| 5 | `power off` | prints `boot routine: POWER OFF` |

These are **placeholders**: the sketch only logs them. `executeBootCommand()`
is the single integration point — replace the prints with a partition switch,
e.g.

```cpp
const esp_partition_t *p = esp_partition_find_first(
    ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_0, nullptr);
esp_ota_set_boot_partition(p);
esp_restart();
```

Phrases are written without “OS” because ESP-SR converts plain English to
phonemes; `boot game o s` is what “boot GameOS” sounds like to it. Keep every
entry under `SR_CMD_STR_LEN_MAX` and give it a case in `executeBootCommand()`.

---

## Editing the launcher tables

All in `LauncherOS.ino`, Xander module:

| Table | Add to it | Effect |
|---|---|---|
| `kPhrase` | `{ "words", XI_INTENT }` | exact system command |
| `kOs` | `{ "joinedkey", "AppName" }` | launch handle, name or mood |
| `kChips` | `{ "Label", XI_INTENT }` | tappable console chip |
| `x_do()` | `case XI_...:` | what the intent actually does |
| `x_is_filler()` | word | ignored around/inside phrases |

Rules of thumb: keys are lower-case with no spaces; multi-word handles run
together (`takeawalk`); keep new phrases disjoint from clock/system words;
add a `case` or the command silently falls to “unknown”.

---

## Matching pipeline

```
input
  │  normalise (case, punctuation, wi fi, i'm/it's/let's)
  ├─▶ 1. OS name / mood aliases        (kOs)
  ├─▶ 2. system phrase table           (kPhrase, longest wins)
  ├─▶ 3. knowledge triggers            ("what is …", "explain …")
  ├─▶ 4. legacy keyword templates + app-name match
  └─▶ 5. "I do not know that command yet."
```

Order is deliberate: it is what stops `what is the time` from being answered by
the knowledge lookup with “I do not have that in my library yet”.

This pipeline applies to **typed** input. Push-to-talk does not go through it:
ESP-SR matches its own 29-phrase list and hands back an id, which is why a PTT
phrase must be one of the entries in the Push-to-talk table rather than any of
the 86 typed aliases.

---

## Persisted settings (NVS)

| Namespace | Key | Meaning |
|---|---|---|
| `dcw` | `bright` | panel level 20–255 (v1.1) |
| `dcw` | `t24` | 1 = 24-hour clock (v1.1) |
| `dcw` | `favs`, `wp_idx` | favourites, wallpaper |
| `xander` | `solved/correct/hints/streak/best/hard` | puzzle stats |
| `xander` | `voice`, `aon`, `blim` | voice replies, always-on, battery limit |
| `xander` | `bootmic` | 1 = BOOT opens the mic (default), 0 = button ignored by voice |
| `rtcsave` | `t/gmt/seq/man/src` | shared wall clock |
| `appcache` | `sig/cnt/n*/s*` | SD app-list cache |

Clearing `dcw` resets brightness to 140 and the clock to 24-hour.

---

## Build & flash

| Item | Value |
|---|---|
| Core | esp32 **3.3.12** |
| Arduino_GFX | **1.6.9** — installed and already patched for core 3.3.6+ in `<sketchbook>/libraries/GFX_Library_for_Arduino` (1.6.4 does not compile against core 3.3.x) |
| Partition scheme | **DualCore 5MB APP / 3.9MB MODEL (16MB)** — added by `boards.local.txt`. "custom" builds the *same* table (the sketch ships `partitions.csv`), but it does not flash `srmodels.bin` and reports usage against 16 MB instead of the 5 MB slot, so it cannot warn you when the app outgrows `factory` |
| Flash layout | factory 5 MB @ 0x10000, ota_0 4 MB @ 0x510000, model 3.9 MB @ 0x910000, storage 3.1 MB |
| Launcher size | 3 776 179 B compiled / 3 776 432 B `.bin` — 72 % of the 5 MB slot (md5 `91dc4126…`) |
| Launcher RAM | 149 024 B — 45 % |
| Model | `srmodels.bin` @ **0x910000** — separate `write_flash`, not carried by Upload or `merged.bin` |
| Prebuilt launcher | `build/LauncherOS-v1.3-voice.bin` @ `0x10000` |
| Voice | in `LauncherOS.ino` — push-to-talk *and* always-on; the model at 0x910000 serves both |

### Does this table affect the other OS `.bin` files? No.

The new table lives in the **launcher image only**. The ten apps in
`/WATCH/bin` were not rebuilt, re-partitioned, or touched:

| Concern | Reality |
|---|---|
| Do the apps need a new partition table? | No. An app `.bin` does not carry a table; LauncherOS copies it into `ota_0` (now 4 MB) at boot and it runs there |
| Do they fit? | Yes — largest is SpotifyOS at 2 423 072 B (2.31 MB) against a 4 MB slot |
| Do they use flash partitions? | No. They use SD + NVS only. Every app's sole `esp_partition_*` call is `find_first(factory)` to “return home”, which still exists at 0x10000 — and each one falls back safely if it didn't. None uses SPIFFS/LittleFS or a hard-coded offset |
| Does NVS survive? | Yes — `nvs` (0x9000, 20 KB) and `otadata` (0xE000) keep their stock offsets, so brightness, clock, favourites and every app's saved state are untouched |
| Does flashing the launcher wipe `ota_0`? | The table flash does invalidate the app currently sitting in `ota_0`; the bootloader falls back to `factory`, and `setup()` re-anchors the launcher. The SD copies are unaffected — pick any app again and it is copied back in |
| Do the apps' own builds change? | No. Compile them exactly as before; only the offset they are *copied to* moved from 0x310000 to 0x510000, and LauncherOS does that copy itself |

## Troubleshooting

| Symptom | Cause / fix |
|---|---|
| `<Name> is not on the card` | `.bin` really absent from the folder `apps_dir()` settled on — the lookup already rescanned and walked the card. The search now covers `/WATCH/bin`, `/WATCH/apps`, `/bin`, `/apps` (any case), plus one level down from `/WATCH` and from the card root, so a card holding a plain `bin/` at its root works. Check `[ptt] <Name> not found under <path>` and the `[scan] card root holds:` line |
| `Dual_Link.bin` not found | it must be in `/WATCH/bin` when the launcher boots; reboot after copying (the alias already exists) |
| Command answers “I do not know that” | phrase not in `kPhrase`, or the intent has no `case` in `x_do()` |
| Sync fails | WiFi toggled off, or no AP reachable |
| `spiFrequencyToClockDiv` errors in `Arduino_ESP32SPI.cpp` / `Arduino_ESP32SPIDMA.cpp` | Arduino_GFX 1.6.4 was built for core 3.2.x; the 3.3.x core changed that call to `spiFrequencyToClockDiv(spi_t*, uint32_t)`. The build log line `Using library GFX Library for Arduino at version 1.6.4` is the giveaway. Upgrade to 1.6.9 and restart the IDE: `arduino-cli lib install "GFX Library for Arduino@1.6.9"` (or Library Manager → Update) |
| `Sketch too big` | pick **DualCore 5MB APP** — every stock 16 MB scheme caps the app at 3 MB |
| `Voice unavailable` | ESP-SR refused to start: wrong/old scheme, or `srmodels.bin` not written to 0x910000 — check the `[ptt] begin fail` heap lines |
| `No voice model` | `model` partition missing or reads back `0xFF`. Serial says `[ptt] model @ 0x910000 is blank - flash srmodels.bin there`, and BOOT then answers `No voice model` instead of opening the mic. Cause is the Upload, not the sketch, and there were two settings doing it: **Partition Scheme: custom** (builds the right table but carries no model write) and **Erase All Flash Before Sketch Upload: Enabled** (`EraseFlash=all` adds `-e` to esptool, wiping the whole chip including 0x910000, so any model written outside the IDE is lost on the next upload — and on a `custom` scheme the model is never written in the first place). Fix: Tools → Partition Scheme = **DualCore 5MB APP / 3.9MB MODEL (16MB)** and Erase = **Disabled**, then Upload once. Belt and braces: drop `srmodels.bin` on the card as `/WATCH/srmodels.bin` and `sd_provision_model()` fills a blank partition at boot (`voice model nn%` on the splash) — see `build/FLASH.md`. If serial says `no 'model' partition` instead, the table is wrong: re-flash the merged image |
| `[pm] CPU @ 80 MHz` | normal — `POWER_SAVE` drops the core from 240 to 80 MHz once the UI is built; it returns to 240 only while OTA-copying an app |
| `Not enough RAM` | largest internal block under 40 KB; close the Xander layer / reboot and retry |
| `Mic did not start` | I2S mic wiring (BCLK 17 / WS 18 / DIN 16), 3V3, L/R tied to GND |
| Wrong OS launches | the `HEARD:` overlay names the winner — say the phrase more slowly, or rename it in `kPtt` (`spotify` → `spotify music`) |
| `No command heard` | phrase not in the 29-entry list, or said after the ~6 s window closed |
| Voice stage says model partition empty | copy `srmodels.bin` to `/WATCH/srmodels.bin` and reboot (auto-fills 0x910000), or Upload on the DualCore 5MB entry; don't use `merged.bin` alone — it never contains the model |
| Always-On is on but nothing happens | it cannot start without the model partition: the serial line is `[aon] start failed (No voice model) - retry in 8000ms`, and BOOT reports the same. Upload with the **DualCore 5MB** entry and Erase disabled, or put `srmodels.bin` on the card |
| Always-On ignores a phrase you clearly said | the 7 s gate had expired. Say `xander` first, or press BOOT to reopen it; `[aon] ignored without wake: "…"` names what it heard |
| Battery drains overnight with a fresh flash | expected if Always-On is on (mic + AFE + 240 MHz). Set Battery Limit, or turn the toggle off |
| “Xander” ignored in the launcher | expected — that build has no model; use the voice firmware |
