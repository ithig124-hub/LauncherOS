# Writing apps for LauncherOS  (`/WATCH/bin/*.bin`)

This is the **developer guide** for apps that the LauncherOS app screen
loads from `/WATCH/bin/` on the SD card. Drop a compiled `.bin` in that
folder and it shows up automatically.

A LauncherOS "app" is **not** a sandboxed program — it is a complete
ESP32-S3 firmware image that the launcher flashes into the spare OTA
slot and then boots into. So your app has full hardware access (display,
touch, PMU, SD, WiFi, RTC) and must look + feel like the launcher to
keep the experience consistent.

This guide covers:

1. The partition scheme your app **must** match
2. How a `.bin` is produced from your sketch
3. The shared visual language (clock + battery card + raw wallpapers)
4. The mandatory **Back to Launcher** flow
5. A complete starter template you can paste in and compile

---

## 1. Partition scheme  ← **read this first**

Both the launcher **and** every app you load through it have to live in
firmware partitions of the same size, because the launcher copies your
`.bin` byte-for-byte into the *other* OTA slot before rebooting. If your
app was compiled with a different partition scheme, the launcher will
either refuse to flash it (`esp_ota_begin` fails) or boot it into a slot
that's too small and brick the boot until you re-flash over USB.

**Use this exact Arduino IDE selection for both LauncherOS and every
app:**

| Setting              | Value                                |
| -------------------- | ------------------------------------ |
| Board                | ESP32-S3 Dev Module                  |
| USB CDC On Boot      | Enabled                              |
| Flash Size           | 16 MB *(or 8 MB — match your board)* |
| Partition Scheme     | **Minimal SPIFFS (large APP)**       |
| PSRAM                | OPI PSRAM                            |
| CPU Frequency        | 240 MHz (WiFi)                       |

`Minimal SPIFFS (large APP)` gives you two ~6.5 MB OTA slots, which is
what the launcher's `esp_ota_get_next_update_partition()` call relies
on. If you pick "No OTA (2 MB APP)" your `.bin` won't work as an app.

> **About your "Im not sure weather" question** — yes, **every app you
> drop into `/WATCH/bin/` has to be built with the same partition
> scheme as LauncherOS**. The launcher itself runs from one OTA slot
> and writes your app into the other; mismatched sizes break that
> contract.

### One sketch = one `.ino` file  *(this is a hard rule)*

Every app **must be a single `.ino` sketch** — no extra `.h` / `.cpp`
files, no library folders shipped alongside it. The only file that's
allowed (and required) outside the `.ino` is the **partition CSV** if
you're overriding the default scheme (and even then, the recommendation
above — `Minimal SPIFFS (large APP)` — uses the stock CSV that ships
with the ESP32 Arduino core, so you don't need a custom one at all).

Why this rule:

- The launcher only ever copies **one file** (`.ino.bin`) per app, so
  any logic split across `.cpp` / `.h` files **must** already be
  inlined into the single compiled image at build time anyway.
- Keeping everything in one `.ino` keeps the source self-contained:
  a contributor can drop a single file into `/WATCH/bin/src/` on the
  repo and anyone can compile it without chasing missing headers.
- Forward-declare your helpers at the top, then define them inline —
  see how `LauncherOS.ino` does it for `fade_brightness`,
  `anchor_launcher_as_boot`, `time_save_now`, etc.

So the **only** files that ever leave your build directory are:

```
MyApp/
  └── MyApp.ino            ← all your code lives here
```

And the **only** files that ever land on the SD card are:

```
/WATCH/bin/
  └── myapp.bin            ← the compiled MyApp.ino.bin, renamed
```

No headers, no `data/` folders, no companion configs. If your app
needs runtime assets, read them from the SD card at runtime (the same
way the launcher reads wallpapers from `/WATCH/PHOTOS/`).

---

## 2. What "a `.bin`" actually is

When you click **Sketch → Export Compiled Binary** in the Arduino IDE
you'll find two files in your sketch folder:

```
MyApp.ino.bin           ← THIS is the file LauncherOS expects
MyApp.ino.bootloader.bin
MyApp.ino.partitions.bin
```

Copy **only** `MyApp.ino.bin` to the SD card and rename it to whatever
should appear in the launcher list:

```
/WATCH/bin/snake.bin
/WATCH/bin/stopwatch.bin
/WATCH/bin/heart_rate.bin
```

The filename (minus `.bin`) is what the launcher shows on the tile.

> Don't copy the bootloader or partitions `.bin` — those are flashed
> once over USB when you first set up LauncherOS, and the launcher's
> OTA loader doesn't touch them. Loading a wrong-sized image won't
> brick the watch (the launcher will reject it or stay anchored as
> bootable), but you'll get an `esp_ota_begin failed` message.

---

## 3. The shared visual language

To make your app feel like part of LauncherOS instead of a random
ESP32 sketch, reuse the same building blocks. The launcher's source
file `LauncherOS.ino` is the canonical reference, but here are the
exact tokens you should match:

### Palette
| Role             | Hex        |
| ---------------- | ---------- |
| Background       | `#000000`  |
| Card surface     | `#0E0E0E`  |
| Card border      | `#1C1C1C`  |
| Primary accent   | `#00E5FF`  |
| Success / OK     | `#00FF80`  |
| Warning          | `#FFB300`  |
| Danger / error   | `#FF4040`  |
| Body text        | `#FFFFFF`  |
| Muted text       | `#666666`  |
| Section eyebrow  | `#888888`  |

### Typography  *(LVGL Montserrat)*
- Title:      `lv_font_montserrat_28`
- Heading:    `lv_font_montserrat_22`
- Body:       `lv_font_montserrat_16` / `_18`
- Eyebrow / caption: `lv_font_montserrat_12` / `_14`  (with `letter_space=2-3`)
- Big numeric (battery %, copy %): `lv_font_montserrat_48`

### Layout
- Round AMOLED screen, **410 × 502** logical pixels
- Cards: 14 px corner radius, 12 px inside padding, 1 px border
- Section headers as **6 px coloured dot + UPPERCASE label**
- Pill buttons: 22-25 px corner radius, gradient fill on primary
- 10 px gap between cards in vertical lists (`pad_row`)

### Raw wallpapers  (`/WATCH/PHOTOS/*.raw|*.bin`)
The launcher stores wallpapers as **RGB565 little-endian, 410 × 502 = 411 884 bytes** raw frame buffers and blits them with `Arduino_GFX`. To use the exact same files in your app:

```cpp
#define LCD_W 410
#define LCD_H 502

static void draw_wallpaper(Arduino_GFX *gfx, const char *path) {
    File f = SD_MMC.open(path, FILE_READ);
    if (!f) { gfx->fillScreen(0x0000); return; }

    const size_t row_bytes = LCD_W * 2;
    uint8_t *line = (uint8_t *)heap_caps_malloc(row_bytes, MALLOC_CAP_8BIT);
    for (int y = 0; y < LCD_H; y++) {
        if (f.read(line, row_bytes) != (int)row_bytes) break;
        gfx->draw16bitRGBBitmap(0, y, (uint16_t *)line, LCD_W, 1);
    }
    free(line);
    f.close();
}
```

To overlay LVGL widgets on top, use the same trick the launcher uses
for its watch face: create an `lv_img` whose source is the raw bytes
loaded into PSRAM, then draw the time/battery widgets as children of
the active LVGL screen.

---

## 4. The mandatory **Back to Launcher** button

Every app should end its UI with one obvious way to return home, so
the user is never stuck. The launcher (since v6.0) **re-anchors itself
as the boot partition on every cold boot**, which means as soon as
your app reboots — for any reason — the next boot lands back in
LauncherOS. So "Back to Launcher" is literally:

```cpp
#include <esp_ota_ops.h>
#include <esp_system.h>

// Drop-in helper: switch the bootloader pointer to the OTHER OTA slot
// (= the launcher) and reboot.  Instant return, no flashing required.
static void return_to_launcher() {
    const esp_partition_t *other = esp_ota_get_next_update_partition(NULL);
    if (other) esp_ota_set_boot_partition(other);   // = launcher slot
    delay(80);
    esp_restart();
}
```

Why `esp_ota_get_next_update_partition(NULL)`? On a 2-slot OTA setup
(the partition scheme above), that call always returns the slot you're
**not** currently running from. Since your app is in the "new" slot
the launcher flashed, "the other one" *is* the launcher. No need to
hardcode partition labels.

Reboot is fast (~600 ms cold-start because RTC is retained), and the
launcher's v6.1 RTC-persist code means the time is back on screen
before the splash even completes.

### UI: the Back button itself

Put it bottom-centre, pill-shaped, cyan border, exactly like the
launcher's other return buttons:

```cpp
static void back_btn_cb(lv_event_t *) { return_to_launcher(); }

lv_obj_t *back = lv_btn_create(scr);
lv_obj_set_size(back, 200, 50);
lv_obj_align(back, LV_ALIGN_BOTTOM_MID, 0, -20);
lv_obj_set_style_bg_color(back, lv_color_hex(0x141414), 0);
lv_obj_set_style_border_color(back, lv_color_hex(0x00E5FF), 0);
lv_obj_set_style_border_width(back, 1, 0);
lv_obj_set_style_radius(back, 25, 0);

lv_obj_t *bl = lv_label_create(back);
lv_label_set_text(bl, LV_SYMBOL_LEFT "  Back to Launcher");
lv_obj_set_style_text_color(bl, lv_color_hex(0x00E5FF), 0);
lv_obj_set_style_text_font(bl, &lv_font_montserrat_16, 0);
lv_obj_center(bl);

lv_obj_add_event_cb(back, back_btn_cb, LV_EVENT_CLICKED, NULL);
```

> **Belt + braces:** also wire it to a long-press of the BOOT button
> (GPIO 0). That way even a misbehaving app can be exited without
> ripping the battery. Sample code is in the starter template below.

---

## 5. The shared "battery card"

The battery card from LauncherOS's home face is reusable verbatim.
Pull from the AXP2101 via `XPowersLib`, then render the same pill:

```cpp
#define XPOWERS_CHIP_AXP2101
#include "XPowersLib.h"

static XPowersPMU pmu;
static lv_obj_t *batt_card, *batt_fill, *batt_pct, *batt_bolt;

static void build_battery_card(lv_obj_t *parent) {
    batt_card = lv_obj_create(parent);
    lv_obj_remove_style_all(batt_card);
    lv_obj_set_size(batt_card, 110, 36);
    lv_obj_align(batt_card, LV_ALIGN_TOP_LEFT, 20, 18);
    lv_obj_set_style_bg_color(batt_card, lv_color_hex(0x0E0E0E), 0);
    lv_obj_set_style_bg_opa(batt_card, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(batt_card, 18, 0);
    lv_obj_set_style_border_color(batt_card, lv_color_hex(0x1C1C1C), 0);
    lv_obj_set_style_border_width(batt_card, 1, 0);

    // fill bar (left → right, coloured by % in update fn)
    batt_fill = lv_obj_create(batt_card);
    lv_obj_remove_style_all(batt_fill);
    lv_obj_set_size(batt_fill, 0, 18);
    lv_obj_align(batt_fill, LV_ALIGN_LEFT_MID, 8, 0);
    lv_obj_set_style_radius(batt_fill, 9, 0);
    lv_obj_set_style_bg_opa(batt_fill, LV_OPA_COVER, 0);

    batt_bolt = lv_label_create(batt_card);
    lv_label_set_text(batt_bolt, LV_SYMBOL_CHARGE);
    lv_obj_set_style_text_color(batt_bolt, lv_color_hex(0x00FF80), 0);
    lv_obj_align(batt_bolt, LV_ALIGN_LEFT_MID, 6, 0);
    lv_obj_add_flag(batt_bolt, LV_OBJ_FLAG_HIDDEN);

    batt_pct = lv_label_create(batt_card);
    lv_label_set_text(batt_pct, "--%");
    lv_obj_set_style_text_color(batt_pct, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(batt_pct, &lv_font_montserrat_14, 0);
    lv_obj_align(batt_pct, LV_ALIGN_RIGHT_MID, -10, 0);
}

static void update_battery_card() {
    int pct = pmu.getBatteryPercent();
    bool charging = pmu.isCharging();
    uint32_t col = pct < 15 ? 0xFF4040
                 : pct < 30 ? 0xFFB300
                            : 0x00E5FF;
    lv_obj_set_size(batt_fill, (94 * pct) / 100, 18);
    lv_obj_set_style_bg_color(batt_fill, lv_color_hex(col), 0);
    char b[8]; snprintf(b, 8, "%d%%", pct);
    lv_label_set_text(batt_pct, b);
    if (charging) lv_obj_clear_flag(batt_bolt, LV_OBJ_FLAG_HIDDEN);
    else          lv_obj_add_flag(batt_bolt, LV_OBJ_FLAG_HIDDEN);
}
```

Initialise the PMU once in `setup()`:

```cpp
Wire.begin(15, 14);          // SDA, SCL — same as LauncherOS
pmu.begin(Wire, AXP2101_SLAVE_ADDRESS, 15, 14);
```

Call `update_battery_card()` from a 3-second LVGL timer.

---

## 6. The shared "clock" widget on a raw wallpaper

This is the launcher's watch-face look — a wallpaper underneath, big
translucent time on top, date underneath. The launcher persists the
"current wallpaper index" to NVS (`Preferences` namespace `face`,
key `wp_idx`), so to keep continuity between launcher and app you can
read the same key.

```cpp
#include <Preferences.h>
#include <time.h>

static lv_obj_t *clock_lbl, *date_lbl;
static Preferences wp_prefs;

static String pick_wallpaper() {
    wp_prefs.begin("face", true);
    int idx = wp_prefs.getInt("wp_idx", 0);
    wp_prefs.end();
    // Walk /WATCH/PHOTOS/, take the idx-th .raw/.bin
    File dir = SD_MMC.open("/WATCH/PHOTOS");
    File f = dir.openNextFile();
    int i = 0;
    while (f) {
        String n = f.name();
        n.toLowerCase();
        if (n.endsWith(".raw") || n.endsWith(".bin")) {
            if (i == idx) return String("/WATCH/PHOTOS/") + f.name();
            i++;
        }
        f = dir.openNextFile();
    }
    return "";   // no wallpaper -> caller falls back to black
}

static void build_clock_screen(lv_obj_t *scr) {
    // 1) wallpaper as an lv_img with a raw-RGB565 source (or paint
    //    directly with gfx->draw16bitRGBBitmap as shown earlier).

    // 2) Big time
    clock_lbl = lv_label_create(scr);
    lv_obj_set_style_text_color(clock_lbl, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(clock_lbl, &lv_font_montserrat_48, 0);
    lv_obj_set_style_text_opa(clock_lbl, 230, 0);
    lv_obj_align(clock_lbl, LV_ALIGN_CENTER, 0, -40);

    // 3) Date underneath
    date_lbl = lv_label_create(scr);
    lv_obj_set_style_text_color(date_lbl, lv_color_hex(0xBBBBBB), 0);
    lv_obj_set_style_text_font(date_lbl, &lv_font_montserrat_16, 0);
    lv_obj_align(date_lbl, LV_ALIGN_CENTER, 0, 10);
}

static void update_clock() {
    struct tm tm;
    if (!getLocalTime(&tm, 5)) {
        lv_label_set_text(clock_lbl, "--:--");
        lv_label_set_text(date_lbl, "no sync");
        return;
    }
    char t[8], d[24];
    strftime(t, sizeof(t), "%H:%M", &tm);
    strftime(d, sizeof(d), "%a %d %b", &tm);
    lv_label_set_text(clock_lbl, t);
    lv_label_set_text(date_lbl, d);
}
```

The launcher already syncs NTP and **persists the wall-clock to NVS
every 5 minutes** (v6.1+), so your app gets a correct `getLocalTime()`
result the instant it boots — no extra work.

---

## 7. Complete starter template  (`MyApp.ino`)

Paste this into a new sketch, set the partition scheme as above,
**Sketch → Export Compiled Binary**, and copy `MyApp.ino.bin` to
`/WATCH/bin/myapp.bin`. It boots into LauncherOS-styled UI with:

- Wallpaper background (same source as launcher)
- Time + date in the centre
- Battery card top-left
- "Back to Launcher" button at the bottom (instant return via OTA pointer)
- BOOT-button long-press as a hardware safety exit

```cpp
// ---------------------------------------------------------------
//  MyApp.ino   --   minimal LauncherOS-compatible app template
// ---------------------------------------------------------------
#include <Arduino.h>
#include <Wire.h>
#include <SD_MMC.h>
#include <Preferences.h>
#include <time.h>
#include <esp_ota_ops.h>
#include <esp_system.h>

#include "lvgl.h"
#include "Arduino_GFX_Library.h"
#define XPOWERS_CHIP_AXP2101
#include "XPowersLib.h"

// ---- pins (must match LauncherOS) ----------------------------
#define LCD_W 410
#define LCD_H 502
#define IIC_SDA 15
#define IIC_SCL 14
#define BOOT_BTN 0
// (display + touch init code identical to LauncherOS -- copy
//  the gfx/bus/touch setup block straight from LauncherOS.ino)

static XPowersPMU pmu;
static lv_obj_t  *scr_root;
extern Arduino_GFX *gfx;   // wired up in your display init

// ---- Back-to-launcher ----------------------------------------
static void return_to_launcher() {
    const esp_partition_t *other = esp_ota_get_next_update_partition(NULL);
    if (other) esp_ota_set_boot_partition(other);
    delay(80);
    esp_restart();
}
static void back_btn_cb(lv_event_t *) { return_to_launcher(); }

// ---- UI build ------------------------------------------------
static void build_ui() {
    scr_root = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr_root, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(scr_root, LV_OPA_COVER, 0);

    // (optional: draw_wallpaper(gfx, pick_wallpaper().c_str()) here)

    // Battery card  -- code from section 5
    build_battery_card(scr_root);

    // Clock        -- code from section 6
    build_clock_screen(scr_root);

    // App-specific content (your feature goes here) ------------
    lv_obj_t *hello = lv_label_create(scr_root);
    lv_label_set_text(hello, "Hello from MyApp");
    lv_obj_set_style_text_color(hello, lv_color_hex(0x00E5FF), 0);
    lv_obj_set_style_text_font(hello, &lv_font_montserrat_22, 0);
    lv_obj_align(hello, LV_ALIGN_CENTER, 0, 90);

    // Back to Launcher pill -- code from section 4
    lv_obj_t *back = lv_btn_create(scr_root);
    lv_obj_set_size(back, 220, 50);
    lv_obj_align(back, LV_ALIGN_BOTTOM_MID, 0, -22);
    lv_obj_set_style_bg_color(back, lv_color_hex(0x141414), 0);
    lv_obj_set_style_border_color(back, lv_color_hex(0x00E5FF), 0);
    lv_obj_set_style_border_width(back, 1, 0);
    lv_obj_set_style_radius(back, 25, 0);
    lv_obj_t *bl = lv_label_create(back);
    lv_label_set_text(bl, LV_SYMBOL_LEFT "  Back to Launcher");
    lv_obj_set_style_text_color(bl, lv_color_hex(0x00E5FF), 0);
    lv_obj_set_style_text_font(bl, &lv_font_montserrat_16, 0);
    lv_obj_center(bl);
    lv_obj_add_event_cb(back, back_btn_cb, LV_EVENT_CLICKED, NULL);

    lv_scr_load(scr_root);
}

void setup() {
    Serial.begin(115200);
    // (copy gfx->begin(), bus, touch, LVGL init from LauncherOS.ino)
    Wire.begin(IIC_SDA, IIC_SCL);
    pmu.begin(Wire, AXP2101_SLAVE_ADDRESS, IIC_SDA, IIC_SCL);
    pinMode(BOOT_BTN, INPUT_PULLUP);

    SD_MMC.begin("/sdcard", true);
    build_ui();
}

void loop() {
    static uint32_t t1 = 0, tbtn = 0;
    uint32_t now = millis();

    lv_timer_handler();

    if (now - t1 > 1000) { t1 = now; update_clock(); }
    if (now - t1 > 3000) { update_battery_card(); }

    // Hardware safety exit: hold BOOT for 1.5 s -> launcher.
    if (digitalRead(BOOT_BTN) == LOW) {
        if (!tbtn) tbtn = now;
        else if (now - tbtn > 1500) return_to_launcher();
    } else tbtn = 0;
}
```

---

## 8. Conventions checklist

Before you copy your `.bin` to `/WATCH/bin/`, double-check:

- [ ] **All app code lives in a single `.ino` file** (no `.h` / `.cpp` companions; partitions CSV exempt — and the recommended scheme doesn't need a custom one)
- [ ] Partition scheme = **Minimal SPIFFS (large APP)**
- [ ] PSRAM = **OPI PSRAM** (matches LauncherOS)
- [ ] You're shipping **only** `MyApp.ino.bin` (not bootloader or partitions)
- [ ] Filename uses lowercase, no spaces (e.g. `stopwatch.bin`)
- [ ] App calls `return_to_launcher()` from an obvious on-screen button
- [ ] Hardware BOOT-button long-press also calls `return_to_launcher()`
- [ ] No `nvs_flash_erase()` calls (would wipe favourites + persisted RTC)
- [ ] Doesn't touch OTA partitions itself (let the launcher own that flow)
- [ ] Doesn't disable RTC retention (`esp_sleep_pd_config(ESP_PD_DOMAIN_RTC_PERIPH, ESP_PD_OPTION_ON)`)
- [ ] Uses the shared palette + Montserrat font sizes from §3

---

## 9. NVS namespaces owned by the launcher  *(don't trample)*

| Namespace   | Key(s)          | Owner                  |
| ----------- | --------------- | ---------------------- |
| `favs`      | `csv`           | App-list favourites    |
| `face`      | `wp_idx`        | Current wallpaper      |
| `appcache`  | `sig`, `cnt`, `nN`, `sN` | App-list NVS cache |
| `rtcsave`   | `t`, `gmt`      | Persistent wall clock  |

Pick your own namespace (e.g. `myapp`) for your app's own preferences.

---

## 10. Troubleshooting

| Symptom                                          | Likely cause                                                       |
| ------------------------------------------------ | ------------------------------------------------------------------ |
| Launcher shows `esp_ota_begin failed`            | Wrong partition scheme — recompile with Minimal SPIFFS (large APP) |
| App boots but reboots into itself instead of launcher | You forgot `esp_ota_set_boot_partition(other)` before `esp_restart()` |
| App shows `--:--` for the clock                  | Add `time_restore_from_nvs()` analogue (see §6) — read the launcher's `rtcsave` namespace |
| App can't find the wallpaper                     | SD path is case-sensitive; confirm `/WATCH/PHOTOS/<name>.raw` exists |
| `pmu.getBatteryPercent()` returns 0              | Wire.begin pins wrong; must be `(15, 14)` matching LauncherOS      |
| Touch dead                                       | Reuse LauncherOS's TP_RESET pulse + FT3168 init verbatim           |

Built for [LauncherOS](https://github.com/ithig124-hub/LauncherOS) v6.1+.

---

## 11. Shared boot block  *(paste verbatim into every app)*

Every app needs ~80 lines of identical hardware init — pins, QSPI bus,
CO5300 display, FT3168 touch, AXP2101 PMU, LVGL draw buffer. Twelve apps
each subtly reimplementing this is how you end up with twelve different
"my touch is flaky" bugs.

So: **copy the entire block below into the top of your single `.ino`
file, verbatim**. It's lifted from `LauncherOS.ino` and uses the same
pin map / driver versions, so any app built on top will behave exactly
like the launcher does.

```cpp
// ===== BEGIN SHARED BOOT BLOCK (LauncherOS v6.1+ compatible) =====
//   Don't edit values here -- if a pin changes on a hardware revision,
//   it changes for the launcher too, and we ship both updates together.

#include <Arduino.h>
#include <FS.h>
#include <SD_MMC.h>
#include <Wire.h>
#include <WiFi.h>
#include <Preferences.h>
#include <time.h>
#include <sys/time.h>
#include <memory>

#include "Arduino_GFX_Library.h"
#include "Arduino_DriveBus_Library.h"
#include "lv_conf.h"
#include "lvgl.h"

#define XPOWERS_CHIP_AXP2101
#include "XPowersLib.h"

#include <esp_ota_ops.h>
#include <esp_system.h>
#include <esp_sleep.h>
#include <esp_bt.h>
#include <esp32-hal-cpu.h>

// ---- Pin map (must match LauncherOS) ---------------------------------
#define LCD_SDIO0   4
#define LCD_SDIO1   5
#define LCD_SDIO2   6
#define LCD_SDIO3   7
#define LCD_SCLK   11
#define LCD_CS     12
#define LCD_RESET   8
#define LCD_WIDTH  410
#define LCD_HEIGHT 502

#define IIC_SDA    15
#define IIC_SCL    14
#define TP_INT     38
#define TP_RESET    9

static const int SDMMC_CLK  = 2;
static const int SDMMC_CMD  = 1;
static const int SDMMC_DATA = 3;

#define PWR_BTN_PIN 10     // side power button (idles HIGH)
#define BOOT_BTN_PIN 0     // BOOT button -- also our hardware safety exit

// ---- Display + touch -------------------------------------------------
Arduino_DataBus *bus = new Arduino_ESP32QSPI(
    LCD_CS, LCD_SCLK, LCD_SDIO0, LCD_SDIO1, LCD_SDIO2, LCD_SDIO3);
Arduino_GFX *gfx = new Arduino_CO5300(
    bus, LCD_RESET, 0, LCD_WIDTH, LCD_HEIGHT, 22, 0, 0, 0);

std::shared_ptr<Arduino_IIC_DriveBus> IIC_Bus =
    std::make_shared<Arduino_HWIIC>(IIC_SDA, IIC_SCL, &Wire);

void Arduino_IIC_Touch_Interrupt(void);
std::unique_ptr<Arduino_IIC> FT3168(new Arduino_FT3x68(
    IIC_Bus, FT3168_DEVICE_ADDRESS, DRIVEBUS_DEFAULT_VALUE,
    TP_INT, Arduino_IIC_Touch_Interrupt));
void Arduino_IIC_Touch_Interrupt(void) { FT3168->IIC_Interrupt_Flag = true; }

// ---- LVGL plumbing ---------------------------------------------------
static lv_disp_draw_buf_t draw_buf;
static lv_color_t        *lvgl_buf = nullptr;
static lv_disp_drv_t      disp_drv;
static lv_indev_drv_t     indev_drv;

static void my_disp_flush(lv_disp_drv_t *d, const lv_area_t *a, lv_color_t *px) {
    uint32_t w = a->x2 - a->x1 + 1, h = a->y2 - a->y1 + 1;
    gfx->draw16bitRGBBitmap(a->x1, a->y1, (uint16_t *)px, w, h);
    lv_disp_flush_ready(d);
}
static void my_touchpad_read(lv_indev_drv_t *, lv_indev_data_t *data) {
    // Minimal version: full v5.13 hysteresis + swipe detection is in
    // LauncherOS.ino if you need it.  For most apps this is enough.
    int32_t x = FT3168->IIC_Read_Device_Value(Arduino_IIC_Touch::Arduino_IIC_Touch_X);
    int32_t y = FT3168->IIC_Read_Device_Value(Arduino_IIC_Touch::Arduino_IIC_Touch_Y);
    int fingers = FT3168->IIC_Read_Device_Value(
        Arduino_IIC_Touch::Arduino_IIC_Touch_FingerNumber);
    if (fingers > 0) { data->state = LV_INDEV_STATE_PR; data->point.x = x; data->point.y = y; }
    else             { data->state = LV_INDEV_STATE_REL; }
}

// ---- AXP2101 PMU -----------------------------------------------------
XPowersPMU g_pmu;
static bool g_has_pmu = false;
static void pmu_init() {
    if (g_pmu.begin(Wire, AXP2101_SLAVE_ADDRESS, IIC_SDA, IIC_SCL)) {
        g_has_pmu = true;
        g_pmu.disableTSPinMeasure();
        g_pmu.enableBattVoltageMeasure();
    }
}

// ---- Back-to-launcher (mandatory; see §4) ----------------------------
static void return_to_launcher() {
    const esp_partition_t *other = esp_ota_get_next_update_partition(NULL);
    if (other) esp_ota_set_boot_partition(other);
    delay(80);
    esp_restart();
}

// ---- One call to bring everything online -----------------------------
// Call this FIRST in your setup().  After it returns you have:
//   * gfx-> ready (panel at full brightness)
//   * I2C bus up, touch chip reset, FT3168 ready
//   * LVGL display + indev registered (use lv_scr_load_anim / lv_obj_create freely)
//   * pmu_init() called (battery readings via g_pmu.*)
//   * SD_MMC mounted at /sdcard (so /WATCH/... is readable)
//   * BOOT button pin pulled up
static void launcher_app_boot() {
    Serial.begin(115200);
    pinMode(BOOT_BTN_PIN, INPUT_PULLUP);
    pinMode(PWR_BTN_PIN,  INPUT_PULLUP);
    btStop();

    if (!gfx->begin()) Serial.println("gfx->begin() failed");
    gfx->fillScreen(0x0000);

    // I2C bus recovery + init
    pinMode(IIC_SCL, OUTPUT);
    for (int i = 0; i < 9; i++) {
        digitalWrite(IIC_SCL, LOW); delayMicroseconds(5);
        digitalWrite(IIC_SCL, HIGH); delayMicroseconds(5);
    }
    Wire.begin(IIC_SDA, IIC_SCL);

    // Touch reset pulse
    pinMode(TP_RESET, OUTPUT);
    digitalWrite(TP_RESET, LOW);  delay(50);
    digitalWrite(TP_RESET, HIGH); delay(200);
    FT3168->begin();

    pmu_init();

    // LVGL
    lv_init();
    size_t bufPx = LCD_WIDTH * 60;     // ~50 KB -- safe on PSRAM-equipped boards
    lvgl_buf = (lv_color_t *)heap_caps_malloc(bufPx * sizeof(lv_color_t),
                                              MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!lvgl_buf) lvgl_buf = (lv_color_t *)heap_caps_malloc(
                                  bufPx * sizeof(lv_color_t), MALLOC_CAP_8BIT);
    lv_disp_draw_buf_init(&draw_buf, lvgl_buf, NULL, bufPx);
    lv_disp_drv_init(&disp_drv);
    disp_drv.flush_cb = my_disp_flush;
    disp_drv.draw_buf = &draw_buf;
    disp_drv.hor_res  = LCD_WIDTH;
    disp_drv.ver_res  = LCD_HEIGHT;
    lv_disp_drv_register(&disp_drv);
    lv_indev_drv_init(&indev_drv);
    indev_drv.type    = LV_INDEV_TYPE_POINTER;
    indev_drv.read_cb = my_touchpad_read;
    lv_indev_drv_register(&indev_drv);

    // SD
    SD_MMC.setPins(SDMMC_CLK, SDMMC_CMD, SDMMC_DATA);
    SD_MMC.begin("/sdcard", true);
}

// Call this from loop() every iteration -- pumps LVGL and gives you
// the hardware BOOT-button safety exit (hold 1.5 s -> back to launcher).
static void launcher_app_loop_tick() {
    lv_timer_handler();
    static uint32_t btn_t = 0;
    if (digitalRead(BOOT_BTN_PIN) == LOW) {
        if (!btn_t) btn_t = millis();
        else if (millis() - btn_t > 1500) return_to_launcher();
    } else btn_t = 0;
}
// ===== END SHARED BOOT BLOCK =====
```

With that pasted at the top, your entire `MyApp.ino` collapses to:

```cpp
// (shared boot block above)

void setup() {
    launcher_app_boot();   // hardware + LVGL + SD all up

    // Your UI -- battery card, clock, back button, app content...
    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x000000), 0);
    lv_obj_t *hello = lv_label_create(scr);
    lv_label_set_text(hello, "Hello from MyApp");
    lv_obj_set_style_text_color(hello, lv_color_hex(0x00E5FF), 0);
    lv_obj_set_style_text_font(hello, &lv_font_montserrat_28, 0);
    lv_obj_center(hello);
    lv_scr_load(scr);
}

void loop() {
    launcher_app_loop_tick();   // LVGL + BOOT-button safety
    // Your per-frame logic here
}
```

That's a complete, compilable, single-`.ino` app that:

- Boots the panel correctly (no flaky touch from drifted init order)
- Mounts the SD card so wallpapers / settings are reachable
- Reads the battery via the same PMU instance the launcher uses
- Has a hardware safety exit (hold BOOT) baked in for free

If a future hardware revision changes a pin, just replace the SHARED
BOOT BLOCK with the new one from this README — no per-app archaeology.

