# NoteOS + GameOS — drop-in files

## Download links

| File | URL |
|---|---|
| **NoteOS.ino** (full rewrite, 1648 lines) | https://amoled-notepad.preview.emergentagent.com/api/download/NoteOS.ino |
| **GameOS.ino** (crash patch only) | https://amoled-notepad.preview.emergentagent.com/api/download/GameOS.ino |
| **NoteOS_and_GameOS_fixed.zip** (everything in one zip) | https://amoled-notepad.preview.emergentagent.com/api/download/NoteOS_and_GameOS_fixed.zip |
| **CHANGES.md** (this file) | https://amoled-notepad.preview.emergentagent.com/api/download/CHANGES.md |
| Index page | https://amoled-notepad.preview.emergentagent.com/api/downloads |

---

## 1. The crash (`Guru Meditation - LoadProhibited` @ `EXCVADDR 0x22`)

The boot loop on **both** apps was caused by the same thing:

```
E (1005) i2c.master: I2C transaction unexpected nack detected
E (1005) i2c.master: s_i2c_synchronous_transaction(924): I2C transaction failed
E (1007) i2c.master: i2c_master_multi_buffer_transmit(1186): I2C transaction failed
Guru Meditation Error: Core 1 panic'ed (LoadProhibited).
PC      : 0x42017890
EXCVADDR: 0x00000022
```

The FT3168 touch IC NACKed on the I2C bus (loose flex, cold-boot rail
glitch, etc.). `FT3168->begin()` doesn't propagate that — it leaves the
driver half-initialised. The next `lv_touch_read()` then calls
`FT3168->IIC_Read_Device_Value(...)`, which dereferences an uninitialised
member at struct offset **0x22** → crash → boot loop.

### Fix (applied to both files)

* New `i2c_ping(addr)` helper. Called *before* `FT3168->begin()`.
* New `static bool g_has_touch` flag.
* If the IC doesn't ACK after one retry, we **skip** `FT3168->begin()`
  entirely and set `g_has_touch=false`.
* The LVGL touch-read callbacks early-return with `LV_INDEV_STATE_REL`
  whenever `g_has_touch` is false.

Result: the watch boots into the UI even with a dead touch panel
(BOOT button still works for return-to-launcher).

---

## 2. NoteOS — complete rewrite

### Multi-stroke letters finally work (`t`, `i`, `j`, `x`, `k`, `f`, `+`, `=`, `;`, `:`, `?`)

The old recognizer classified on every finger-lift, so it fired the
moment you finished the vertical of a `t`. Now there's a **600 ms
commit timer** that resets on every new stroke. Multi-stroke letters
get fully collected before classification.

The `Pt` struct has a new `int s` field (stroke index). `$P` resampling
respects stroke breaks, so the point cloud carries multi-stroke
topology through to `greedy_cloud_match`.

### Templates added

* `A-Z` (richer shapes)
* `a-z` lowercase (full set)
* `0-9`
* `. , ! ? @ - + = : ; '`

73 built-in templates in one compact `Tpl[]` array.

### Boot lands on the NOTES LIBRARY, not the pad

The library:

* Lists all `*.txt` files in `/WATCH/notes/`
* Shows a 30-char snippet preview per note
* Each row has **READ** (green) · **EDIT** (cyan) · **🗑** (red)
* Top-right **+ NEW** button creates `/WATCH/notes/note_NN.txt`
* Top-left **TRAIN** button opens the handwriting trainer (next section)

Reader screen is a full-text scrollable viewer with `BACK` and `EDIT`.

### ⭐ NEW: Handwriting Training Wizard

Tap **TRAIN** on the library screen. The watch walks you through
**73 characters** in order — `A-Z`, `a-z`, `0-9`, `. , ! ? @ - + = : ; '`.

For each character:

* A big amber glyph in the top-right shows the *target* letter
* You write it on the pad below, **the way YOU naturally write it**
* `SAVE & NEXT` (green) captures the stroke(s), normalises them into a
  `UserTpl`, persists `/WATCH/notes/_handwriting.dat`, and advances
* `RETRY` (red) clears the pad if you slip up
* `SKIP` (grey) moves on without saving for that letter
* `← Lib` (top-right) bails out — anything you've already saved is kept

The trainer **persists after every letter**, so a battery yank
mid-training won't lose your work. The library's TRAIN button shows
`TRAIN (47)` once you've customised 47 letters so you know your
progress at a glance.

At boot, `_handwriting.dat` is loaded into `g_user_tpls` and the
classifier checks *your* templates **alongside** the built-ins, with a
15 % distance bonus — so when your shape and the factory shape score
roughly equal, your one wins.

Re-running the trainer for any character **overwrites** the previous
user template for it.

### Mode toggle: `ABC → abc → 123 → SYM`

Tap the amber `ABC` pill at bottom-left to cycle. The recognizer's
top-2 candidates are filtered to the active mode, with automatic
case-flip fallback (so a sloppy `A` while in `abc` mode still becomes
`a`).

### Quick gestures on the pad

| Gesture | Action |
|---|---|
| swipe LEFT  | backspace |
| swipe RIGHT | space |
| swipe DOWN  | newline |

Triggers ONLY when **all** of these are true so they never fire
during letter writing:

* It's the first stroke since the last commit
* Faster than 200 ms
* Longer than 140 px in dominant axis
* Dominant axis > 3 × the other axis

### Word suggestions / autocomplete

3 tappable pill chips above the pad, backed by a ~360-word built-in
dictionary. Tap a chip → replaces the trailing word + adds a space.

### Other editor features

| Feature | How |
|---|---|
| **Undo last letter** | `UND` button |
| **Newline button** | `↵` (separate from swipe-down gesture) |
| **Save** | `✓ SAVE` writes to SD and returns to library. `← Lib` auto-saves too. |
| **Smoother + thicker ink** | Stroke width 6 px (was 4). Quadratic-midpoint smoothing through every triple of points. |

### Memory / safety

* Pad buffer + LVGL buffer go to **SPIRAM first**, fall back to internal heap.
* `g_pad`, `lv_indev_get_act()` are null-checked in `on_pad`.
* Every `lv_touch_read` value range-checked.
* `g_has_touch` guard on every touch register read.

---

## 3. GameOS — minimum-touch crash fix

The full feature set is unchanged. The only edits are the three I2C
safety patches needed to stop the boot loop:

1. `static bool g_has_touch = false;` + `i2c_ping()`.
2. `hw_boot()` pings `FT3168_DEVICE_ADDRESS` (with one retry) **before**
   calling `FT3168->begin()`. Skips driver init if NACK.
3. `lv_touch_gesture()` early-returns with `LV_INDEV_STATE_REL` if
   `g_has_touch` is false.

All 30 games still work.

---

## How to flash

Same workflow as before:

1. Open `NoteOS.ino` (and `GameOS.ino`) in Arduino IDE 2.x
2. Board: **ESP32-S3 Dev Module**
3. **PSRAM: OPI PSRAM**, **Partition: Minimal SPIFFS (large APP)**
4. `Sketch → Export Compiled Binary`
5. Copy the `.ino.bin` to your SD card:
   * `/WATCH/bin/noteos.bin`
   * `/WATCH/bin/gameos.bin`
6. From LauncherOS, swipe to Apps and tap.

### After first boot

1. Tap **TRAIN** on the NoteOS library screen.
2. Spend ~5 minutes walking through all 73 characters.
3. From then on, recognition is calibrated to *your* handwriting.

### Quick "did it crash again?" check

If you still see `LoadProhibited @ EXCVADDR 0x22`:

* Open serial monitor (115200). You should see one of:
  * `[NoteOS] FT3168 NACK -- running headless touch.`
  * `[GameOS] FT3168 NACK -- skipping touch driver init.`
* If yes: the crash is gone, the touch panel is just genuinely unreachable
  on the I2C bus (flex cable, dead rail). The UI will still come up; you
  can navigate by `BOOT` (return to launcher).
* If neither line prints and you still crash: your I2C is hanging
  *before* the ping, i.e. the bus is held LOW. Power-cycle, check the
  TP_RESET wiring.
