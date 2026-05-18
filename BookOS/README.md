# BookOS  —  a library trapped inside a watch

A LauncherOS-compatible offline TXT reader for the Waveshare ESP32-S3
Touch AMOLED 2.06 cyberdeck watch (410 × 502, CO5300 + FT3168).

> No internet. No accounts. No DRM. Just folders and text.
> Drag-and-drop a book onto the SD card and it shows up in the library.

---

## Build & flash

1. Open `BookOS.ino` in **Arduino IDE 2.x**.
2. Board: **ESP32-S3 Dev Module**
   - USB CDC On Boot: **Enabled**
   - Flash Size: **16 MB** (or match your board)
   - **Partition Scheme: Minimal SPIFFS (large APP)** ← must match LauncherOS
   - PSRAM: **OPI PSRAM**
   - CPU: **240 MHz (WiFi)**
3. Install libraries (Library Manager):
   - LVGL 8.3 or 8.4
   - Arduino_GFX_Library
   - Arduino_DriveBus_Library
   - XPowersLib
4. **Sketch → Export Compiled Binary**
5. Copy **only** `BookOS.ino.bin` to your SD card as:

   ```
   /WATCH/bin/bookos.bin
   ```

6. Boot the watch into LauncherOS → swipe to **Apps** → tap **BookOS**.

The partition layout in `partitions.csv` matches every other LauncherOS
module exactly so the OTA flip back to the launcher works.

---

## Auto-created SD folders

On first boot BookOS will create the entire archive layout:

```
/WATCH/BookOS/
├── README.txt               (mini in-place reminder)
├── Books/                   <-- drop one folder per book here
│   └── Welcome_To_BookOS/   (auto-seeded demo book, safe to delete)
│       ├── meta.txt
│       ├── 0001.txt
│       ├── 0002.txt
│       └── ...
├── Saves/
│   ├── last_session.txt     (BOOK= / PAGE= — auto session restore)
│   └── bookmarks/
│       └── <book>.txt       (one page number per line)
├── Themes/                  (placeholder for future custom themes)
├── Quotes/                  (placeholder for saved quotes)
└── System/
    └── version.txt
```

A handful of pre-formatted sample books ship in this repo under
`SampleBooks/`. Copy any of them into `/WATCH/BookOS/Books/` on your SD
card and they'll appear in the carousel.

---

## Adding your own book (drag-and-drop)

1. Create a folder under `/WATCH/BookOS/Books/` — e.g. `Last_Olympian/`.
2. Inside it, drop a `meta.txt`:

   ```
   TITLE=The Last Olympian
   AUTHOR=Rick Riordan
   GENRE=Fantasy
   ```

3. Add the pages as **numbered TXT files**, one TXT = one page:

   ```
   0001.txt   0002.txt   0003.txt   ...
   ```

4. **Page formatting tips** (tuned for the 410 × 502 panel):
   - ~80–100 words per page (sweet spot: ~90).
   - 7–11 lines per page, 1–3 paragraphs.
   - 28–38 characters per line (~32 ideal).
   - Insert manual line breaks; don't rely on auto-wrap.
   - Leave **one blank line** between paragraphs.
   - End pages on impactful lines / scene breaks for cinematic flow.

Example page (`0001.txt`):

```
The hallway trembled beneath
Percy's feet. Dust rained from
the ceiling as another explosion
echoed through Olympus.

Annabeth grabbed his arm.

"We don't have much time."

Somewhere far below them,
the city was already burning.
```

That's it — the next boot will pick the book up automatically.

---

## Controls

| Where           | Gesture                | Action                         |
|-----------------|------------------------|--------------------------------|
| Library         | Swipe ←/→              | Cycle to previous / next book  |
| Library         | Tap center book        | Open book (resumes last page)  |
| Library         | Tap **BACK TO LAUNCHER** pill | Returns to LauncherOS  |
| Reader          | Tap LEFT third         | Previous page                  |
| Reader          | Tap RIGHT third        | Next page                      |
| Reader          | Long-press anywhere    | Quick menu (theme / bookmark / exit) |
| Reader          | Double-tap center      | Focus mode (hides chrome)      |
| Anywhere        | Hold **BOOT** ≥ 1.5 s  | Hardware safety exit to launcher |

The library remembers the centered book across boots, and every page
turn is auto-saved to `Saves/last_session.txt` + NVS so the exact page
is restored next time you open BookOS.

---

## Themes

Hold center in the reader → **THEME** button cycles between:

- WARM PAPER
- OLED BLACK (default)
- FOREST TERMINAL
- DEEP NAVY
- CRIMSON ARCHIVE

The chosen theme persists to NVS.

---

## Back-to-Launcher (matches every other OS module)

The library shows a **cyan-bordered "BACK TO LAUNCHER"** pill at the
bottom. Tapping it streams `/WATCH/bin/launcheros.bin` through
`Update.begin / write / end` and `esp_restart()`s straight into
LauncherOS. If the file is missing or invalid it falls back to the
OTA-partition-flip path so you can always escape.

**Belt-and-braces:** hold the **BOOT** button (GPIO 0) for 1.5 s
anywhere in BookOS for the hardware safety exit.

---

## File checklist

- [x] Single `BookOS.ino` (no `.h` / `.cpp` companions)
- [x] Matches LauncherOS partition scheme (`partitions.csv`)
- [x] Matches LauncherOS pin map & driver versions
- [x] Custom in-app **ARCHIVE RECOVERY** loading splash
- [x] Battery pill + clock in reader bottom bar
- [x] Back-to-Launcher pill + BOOT long-press exit
- [x] Auto-creates `/WATCH/BookOS/` and all subfolders
- [x] Seeds a `Welcome_To_BookOS` demo book on first run

Built for the cyberdeck ecosystem. Read in peace.
