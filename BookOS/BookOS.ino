// =============================================================================
//  BookOS.ino  v1.0  --  LauncherOS-compatible offline TXT reader / archive
//  Target  : Waveshare ESP32-S3 Touch AMOLED 2.06  (410 x 502, CO5300, FT3168)
//  Compile : ESP32-S3 Dev Module / PSRAM=OPI / Flash=16MB / Minimal SPIFFS (large APP)
//  Export  : Sketch -> Export Compiled Binary  ->  copy BookOS.ino.bin to
//            /WATCH/bin/bookos.bin on the SD card.
// -----------------------------------------------------------------------------
//  Aesthetic : "a library trapped inside a watch" -- archive / terminal / paper
//  Vibe      : silent, offline, weightless. No internet. No accounts. Just TXT.
//
//  Folder model (auto-created on first boot, drag-and-drop friendly):
//    /WATCH/BookOS/
//      Books/                       <-- each subfolder = one book
//        <book name>/
//          meta.txt                 (TITLE=, AUTHOR=, GENRE=, THEME=)
//          0001.txt   0002.txt ...  (one TXT file = one page, ~80-100 words)
//      Saves/
//        last_session.txt           (BOOK= / PAGE= -- auto session restore)
//        bookmarks/<book>.txt       (one page number per line)
//      Themes/                      (placeholder for future custom themes)
//      Quotes/                      (placeholder for saved quotes)
//      System/
//        version.txt
//
//  Features in v1:
//    [1] ARCHIVE RECOVERY boot splash with library metadata flash
//    [2] Library carousel - swipe left/right to cycle, tap center to open
//    [3] Last-read book pre-selected with "Continue - Page N" hint
//    [4] Reader - tap LEFT zone = prev page, RIGHT zone = next page
//    [5] Hold center 600ms -> quick menu (theme / bookmark / exit)
//    [6] Double tap reader -> focus mode (text only, no chrome)
//    [7] 5 themes: Warm Paper / OLED Black / Forest Terminal / Deep Navy / Crimson
//    [8] Auto-save session every page turn
//    [9] BACK TO LAUNCHER cyan pill on library + BOOT long-press exit
//   [10] Auto-creates all folders, seeds one demo book on first boot
// =============================================================================

// ===== BEGIN SHARED BOOT BLOCK (LauncherOS v6.1+ compatible) =================
#include <Arduino.h>
#include <FS.h>
#include <SD_MMC.h>
#include <Wire.h>
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
#include <esp_partition.h>
#include <esp_system.h>
#include <esp_sleep.h>
#include <esp_bt.h>
#include <esp32-hal-cpu.h>
#include <Update.h>

// ---- Forward type declarations ---------------------------------------------
//  Arduino IDE hoists generated function prototypes to the very top of the
//  .ino, BEFORE any user-defined types. Any struct used in a return / arg
//  type of a non-static helper must be declared here so the hoisted
//  prototypes compile. ------------------------------------------------------
struct Theme;
struct Book;

// ---- Pin map (must match LauncherOS) ----------------------------------------
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

#define PWR_BTN_PIN  10
#define BOOT_BTN_PIN  0

// ---- Display + touch --------------------------------------------------------
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
    int32_t x = FT3168->IIC_Read_Device_Value(FT3168->Arduino_IIC_Touch::Value_Information::TOUCH_COORDINATE_X);
    int32_t y = FT3168->IIC_Read_Device_Value(FT3168->Arduino_IIC_Touch::Value_Information::TOUCH_COORDINATE_Y);
    int fingers = FT3168->IIC_Read_Device_Value(FT3168->Arduino_IIC_Touch::Value_Information::TOUCH_FINGER_NUMBER);
    if (fingers > 0) {
        data->state = LV_INDEV_STATE_PR;
        data->point.x = x; data->point.y = y;
    } else {
        data->state = LV_INDEV_STATE_REL;
    }
}

XPowersPMU g_pmu;
static bool g_has_pmu = false;
static void pmu_init() {
    if (g_pmu.begin(Wire, AXP2101_SLAVE_ADDRESS, IIC_SDA, IIC_SCL)) {
        g_has_pmu = true;
        g_pmu.disableTSPinMeasure();
        g_pmu.enableBattVoltageMeasure();
    }
}

static void return_to_launcher() {
    // Try SD-loaded launcher first (matches LauncherOS pattern)
    File f = SD_MMC.open("/WATCH/bin/launcheros.bin", FILE_READ);
    if (f && f.size() > 0x20000) {
        size_t sz = f.size();
        if (Update.begin(sz)) {
            uint8_t buf[2048];
            while (f.available()) {
                size_t r = f.read(buf, sizeof(buf));
                if (r) Update.write(buf, r);
            }
            f.close();
            if (Update.end(true)) {
                delay(80);
                esp_restart();
            }
        } else {
            f.close();
        }
    }
    // Fallback: OTA partition flip
    const esp_partition_t *factory = esp_partition_find_first(
        ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_FACTORY, NULL);
    if (factory) {
        esp_ota_set_boot_partition(factory);
    } else {
        const esp_partition_t *other = esp_ota_get_next_update_partition(NULL);
        if (other && other != esp_ota_get_running_partition()) {
            esp_ota_set_boot_partition(other);
        }
    }
    delay(80);
    esp_restart();
}

static uint8_t g_brightness = 200;
static void co5300_set_brightness(uint8_t v) {
    uint8_t cmd = 0x51;
    bus->beginWrite(); bus->writeCommand(cmd); bus->write(v); bus->endWrite();
    g_brightness = v;
}
static void panel_on(uint8_t b) {
    uint8_t cmd = 0x29;
    bus->beginWrite(); bus->writeCommand(cmd); bus->endWrite();
    co5300_set_brightness(b);
}

static void launcher_app_boot() {
    Serial.begin(115200);
    pinMode(BOOT_BTN_PIN, INPUT_PULLUP);
    pinMode(PWR_BTN_PIN,  INPUT_PULLUP);
    btStop();

    if (!gfx->begin()) Serial.println("gfx->begin() failed");
    gfx->fillScreen(0x0000);

    pinMode(IIC_SCL, OUTPUT);
    for (int i = 0; i < 9; i++) {
        digitalWrite(IIC_SCL, LOW);  delayMicroseconds(5);
        digitalWrite(IIC_SCL, HIGH); delayMicroseconds(5);
    }
    Wire.begin(IIC_SDA, IIC_SCL);

    pinMode(TP_RESET, OUTPUT);
    digitalWrite(TP_RESET, LOW);  delay(50);
    digitalWrite(TP_RESET, HIGH); delay(200);
    FT3168->begin();

    pmu_init();

    lv_init();
    size_t bufPx = LCD_WIDTH * 40;
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

    SD_MMC.setPins(SDMMC_CLK, SDMMC_CMD, SDMMC_DATA);
    SD_MMC.begin("/sdcard", true);
}
// ===== END SHARED BOOT BLOCK =================================================

// =============================================================================
//  BookOS palette / themes
// =============================================================================
struct Theme {
    const char *name;
    uint32_t bg;
    uint32_t surface;
    uint32_t border;
    uint32_t txt;
    uint32_t mute;
    uint32_t accent;
};
static const Theme THEMES[] = {
    // Warm Paper
    { "WARM PAPER",      0x1C1812, 0x2A241C, 0x3A3328, 0xF0E6D2, 0x9E927C, 0xD9B26B },
    // OLED Black
    { "OLED BLACK",      0x000000, 0x0A0A0A, 0x1C1C1C, 0xEEEEEE, 0x707070, 0xFFFFFF },
    // Forest Terminal
    { "FOREST TERMINAL", 0x05140A, 0x0B2014, 0x163020, 0xB6FFC8, 0x4F8A66, 0x4FFFA0 },
    // Deep Navy
    { "DEEP NAVY",       0x06101F, 0x0C1A30, 0x182944, 0xE0E8F4, 0x6E7E9A, 0x4FB3FF },
    // Crimson Archive
    { "CRIMSON ARCHIVE", 0x140606, 0x200A0A, 0x301414, 0xF4E0E0, 0xA07070, 0xFF4F6E },
};
#define NUM_THEMES 5
static int g_theme_idx = 1;   // default OLED BLACK

#define CYAN   0x00E5FF       // back-to-launcher only

static inline lv_color_t TC(uint32_t hex) { return lv_color_hex(hex); }
static inline const Theme &TH() { return THEMES[g_theme_idx]; }

// =============================================================================
//  Books model
// =============================================================================
#define MAX_BOOKS      32
#define MAX_NAME_LEN   48
#define MAX_TITLE_LEN  64
#define MAX_AUTHOR_LEN 48
#define MAX_PAGE_BYTES 2200    // ~100 words very comfortably

struct Book {
    char folder[MAX_NAME_LEN];    // exact folder name on SD
    char title[MAX_TITLE_LEN];
    char author[MAX_AUTHOR_LEN];
    int  total_pages;
    int  current_page;            // 1-based, restored from session
    uint32_t last_opened;         // unix seconds, 0 if never
};

static Book g_books[MAX_BOOKS];
static int  g_book_count = 0;
static int  g_lib_idx    = 0;     // currently centered book in carousel
static int  g_reading_book = -1;  // -1 = in library, otherwise index of open book
static char g_page_buf[MAX_PAGE_BYTES + 1];

// =============================================================================
//  Persistence (NVS namespace "bookos")
// =============================================================================
static Preferences g_prefs;
static void prefs_load() {
    g_prefs.begin("bookos", true);
    g_theme_idx = g_prefs.getUInt("theme", 1);
    if (g_theme_idx < 0 || g_theme_idx >= NUM_THEMES) g_theme_idx = 1;
    g_prefs.end();
}
static void prefs_save_theme() {
    g_prefs.begin("bookos", false);
    g_prefs.putUInt("theme", g_theme_idx);
    g_prefs.end();
}

// Save per-book current page (NVS keyed by short hash of folder name)
static uint16_t book_key_hash(const char *s) {
    uint32_t h = 2166136261u;
    while (*s) { h ^= (uint8_t)*s++; h *= 16777619u; }
    return (uint16_t)(h & 0xFFFF);
}
static void save_book_page(const Book &b) {
    g_prefs.begin("bookos", false);
    char k[12]; snprintf(k, sizeof(k), "p%04X", book_key_hash(b.folder));
    g_prefs.putUInt(k, (uint32_t)b.current_page);
    g_prefs.end();
}
static int load_book_page(const Book &b) {
    g_prefs.begin("bookos", true);
    char k[12]; snprintf(k, sizeof(k), "p%04X", book_key_hash(b.folder));
    uint32_t p = g_prefs.getUInt(k, 1);
    g_prefs.end();
    if ((int)p < 1) p = 1;
    if ((int)p > b.total_pages && b.total_pages > 0) p = b.total_pages;
    return (int)p;
}

// =============================================================================
//  Folder bootstrap + demo book
// =============================================================================
static void ensure_dirs() {
    SD_MMC.mkdir("/WATCH");
    SD_MMC.mkdir("/WATCH/BookOS");
    SD_MMC.mkdir("/WATCH/BookOS/Books");
    SD_MMC.mkdir("/WATCH/BookOS/Saves");
    SD_MMC.mkdir("/WATCH/BookOS/Saves/bookmarks");
    SD_MMC.mkdir("/WATCH/BookOS/Themes");
    SD_MMC.mkdir("/WATCH/BookOS/Quotes");
    SD_MMC.mkdir("/WATCH/BookOS/System");

    // Write version + README hint
    if (!SD_MMC.exists("/WATCH/BookOS/System/version.txt")) {
        File f = SD_MMC.open("/WATCH/BookOS/System/version.txt", FILE_WRITE);
        if (f) { f.println("BookOS v1.0"); f.close(); }
    }
    if (!SD_MMC.exists("/WATCH/BookOS/README.txt")) {
        File f = SD_MMC.open("/WATCH/BookOS/README.txt", FILE_WRITE);
        if (f) {
            f.println("BookOS - drop a folder per book inside Books/");
            f.println("Each folder needs:");
            f.println("  meta.txt   (TITLE=, AUTHOR=, GENRE=)");
            f.println("  0001.txt, 0002.txt, ...  (one TXT = one page)");
            f.println("Recommended: 80-100 words per page, manual line breaks.");
            f.close();
        }
    }
}

// Write a small demo book so first boot is never empty
static void seed_demo_book() {
    const char *root = "/WATCH/BookOS/Books/Welcome_To_BookOS";
    if (SD_MMC.exists(root)) return;
    SD_MMC.mkdir(root);

    {
        File f = SD_MMC.open("/WATCH/BookOS/Books/Welcome_To_BookOS/meta.txt", FILE_WRITE);
        if (f) {
            f.println("TITLE=Welcome to BookOS");
            f.println("AUTHOR=The Archive");
            f.println("GENRE=Manual");
            f.println("THEME=Navy");
            f.close();
        }
    }
    const char *pages[] = {
        // 0001
        "BOOKOS\n\nA library trapped\ninside a watch.\n\nNo internet.\nNo accounts.\nNo syncing.\n\nJust folders and text.\n\nTap RIGHT to continue.",
        // 0002
        "Each book lives as a folder\nunder /WATCH/BookOS/Books/.\n\nEvery TXT file in that folder\nis one page of the book.\n\n0001.txt   first page\n0002.txt   second page\n0003.txt   ...\n\nDrag and drop. Done.",
        // 0003
        "Controls\n\nTap LEFT  -- previous page\nTap RIGHT -- next page\nHold CENTER -- quick menu\nDouble tap -- focus mode\n\nHold BOOT button for 1.5s\nto exit back to LauncherOS\nfrom anywhere.",
        // 0004
        "Each book remembers\nwhere you stopped.\n\nWhen BookOS boots\nyour last book is already\nselected in the carousel,\nwith a Continue hint\nshowing the exact page.\n\nOne tap and you're back\ninside the story.",
        // 0005
        "Themes\n\nWarm Paper\nOLED Black\nForest Terminal\nDeep Navy\nCrimson Archive\n\nHold center, choose theme.\nThe whole archive shifts.\n\nThat's BookOS.\nWelcome home.",
    };
    char path[80];
    for (int i = 0; i < 5; i++) {
        snprintf(path, sizeof(path),
                 "/WATCH/BookOS/Books/Welcome_To_BookOS/%04d.txt", i + 1);
        File f = SD_MMC.open(path, FILE_WRITE);
        if (f) { f.print(pages[i]); f.close(); }
    }
}

// =============================================================================
//  Library scan
// =============================================================================
static void parse_meta(Book &b) {
    char path[120];
    snprintf(path, sizeof(path), "/WATCH/BookOS/Books/%s/meta.txt", b.folder);
    File f = SD_MMC.open(path, FILE_READ);
    if (!f) {
        // Fallback: use folder name as title
        strncpy(b.title, b.folder, sizeof(b.title) - 1);
        b.title[sizeof(b.title) - 1] = 0;
        strcpy(b.author, "Unknown");
        return;
    }
    b.title[0] = 0; b.author[0] = 0;
    while (f.available()) {
        String line = f.readStringUntil('\n'); line.trim();
        if (!line.length() || line.startsWith("#")) continue;
        int eq = line.indexOf('='); if (eq < 0) continue;
        String k = line.substring(0, eq); k.trim();
        String v = line.substring(eq + 1); v.trim();
        if (k.equalsIgnoreCase("TITLE")) {
            strncpy(b.title, v.c_str(), sizeof(b.title) - 1);
            b.title[sizeof(b.title) - 1] = 0;
        } else if (k.equalsIgnoreCase("AUTHOR")) {
            strncpy(b.author, v.c_str(), sizeof(b.author) - 1);
            b.author[sizeof(b.author) - 1] = 0;
        }
    }
    f.close();
    if (!b.title[0]) {
        strncpy(b.title, b.folder, sizeof(b.title) - 1);
        b.title[sizeof(b.title) - 1] = 0;
    }
    if (!b.author[0]) strcpy(b.author, "Unknown");
}

static int count_pages(const char *folder) {
    char path[120];
    int count = 0;
    File dir = SD_MMC.open("/WATCH/BookOS/Books/");
    if (!dir) return 0;
    dir.close();
    // Walk the book folder
    snprintf(path, sizeof(path), "/WATCH/BookOS/Books/%s", folder);
    File bd = SD_MMC.open(path);
    if (!bd || !bd.isDirectory()) return 0;
    File f = bd.openNextFile();
    while (f) {
        String n = f.name();
        // SD_MMC returns full path on ESP32 -- take basename
        int slash = n.lastIndexOf('/');
        if (slash >= 0) n = n.substring(slash + 1);
        n.toLowerCase();
        if (!f.isDirectory() && n.endsWith(".txt") && n.length() >= 8) {
            // Expect NNNN.txt
            bool digits = true;
            for (int i = 0; i < 4; i++) if (!isdigit(n[i])) { digits = false; break; }
            if (digits) count++;
        }
        f = bd.openNextFile();
    }
    return count;
}

static void scan_books() {
    g_book_count = 0;
    File dir = SD_MMC.open("/WATCH/BookOS/Books");
    if (!dir || !dir.isDirectory()) return;
    File f = dir.openNextFile();
    while (f && g_book_count < MAX_BOOKS) {
        if (f.isDirectory()) {
            String n = f.name();
            int slash = n.lastIndexOf('/');
            if (slash >= 0) n = n.substring(slash + 1);
            if (n.length() && !n.startsWith(".")) {
                Book &b = g_books[g_book_count];
                memset(&b, 0, sizeof(b));
                strncpy(b.folder, n.c_str(), sizeof(b.folder) - 1);
                parse_meta(b);
                b.total_pages = count_pages(b.folder);
                if (b.total_pages < 1) {
                    // skip empty folders
                    f = dir.openNextFile();
                    continue;
                }
                b.current_page = load_book_page(b);
                g_book_count++;
            }
        }
        f = dir.openNextFile();
    }
}

// =============================================================================
//  Session save / restore
// =============================================================================
static int g_bookmarks_restored = 0;

static void save_session() {
    File f = SD_MMC.open("/WATCH/BookOS/Saves/last_session.txt", FILE_WRITE);
    if (!f) return;
    if (g_reading_book >= 0 && g_reading_book < g_book_count) {
        f.printf("BOOK=%s\n", g_books[g_reading_book].folder);
        f.printf("PAGE=%d\n", g_books[g_reading_book].current_page);
    } else if (g_lib_idx >= 0 && g_lib_idx < g_book_count) {
        f.printf("BOOK=%s\n", g_books[g_lib_idx].folder);
        f.printf("PAGE=%d\n", g_books[g_lib_idx].current_page);
    }
    f.close();
}

static void restore_session_select() {
    File f = SD_MMC.open("/WATCH/BookOS/Saves/last_session.txt", FILE_READ);
    if (!f) return;
    String wantFolder; int wantPage = 1;
    while (f.available()) {
        String line = f.readStringUntil('\n'); line.trim();
        int eq = line.indexOf('='); if (eq < 0) continue;
        String k = line.substring(0, eq);
        String v = line.substring(eq + 1);
        if (k == "BOOK") wantFolder = v;
        if (k == "PAGE") wantPage = v.toInt();
    }
    f.close();
    if (!wantFolder.length()) return;
    for (int i = 0; i < g_book_count; i++) {
        if (wantFolder == g_books[i].folder) {
            g_lib_idx = i;
            if (wantPage >= 1 && wantPage <= g_books[i].total_pages)
                g_books[i].current_page = wantPage;
            break;
        }
    }
    // Count bookmark files for splash metadata flair
    File d = SD_MMC.open("/WATCH/BookOS/Saves/bookmarks");
    if (d && d.isDirectory()) {
        File ff = d.openNextFile();
        while (ff) {
            String nn = ff.name(); nn.toLowerCase();
            if (nn.endsWith(".txt")) g_bookmarks_restored++;
            ff = d.openNextFile();
        }
    }
}

// =============================================================================
//  Bookmarks (lightweight - one page number per line)
// =============================================================================
static void toggle_bookmark(const Book &b) {
    char path[120];
    snprintf(path, sizeof(path), "/WATCH/BookOS/Saves/bookmarks/%s.txt", b.folder);
    // Read existing
    int marks[64]; int n = 0;
    File f = SD_MMC.open(path, FILE_READ);
    if (f) {
        while (f.available() && n < 64) {
            String line = f.readStringUntil('\n'); line.trim();
            if (!line.length()) continue;
            int v = line.toInt();
            if (v > 0) marks[n++] = v;
        }
        f.close();
    }
    // Toggle
    bool found = false; int idx = -1;
    for (int i = 0; i < n; i++) if (marks[i] == b.current_page) { found = true; idx = i; break; }
    if (found) {
        for (int i = idx; i < n - 1; i++) marks[i] = marks[i + 1];
        n--;
    } else if (n < 64) {
        marks[n++] = b.current_page;
    }
    // Write back
    File g = SD_MMC.open(path, FILE_WRITE);
    if (g) {
        for (int i = 0; i < n; i++) g.printf("%d\n", marks[i]);
        g.close();
    }
}

// =============================================================================
//  Page loading
// =============================================================================
static bool load_page(const Book &b, int page) {
    if (page < 1 || page > b.total_pages) return false;
    char path[140];
    snprintf(path, sizeof(path), "/WATCH/BookOS/Books/%s/%04d.txt", b.folder, page);
    File f = SD_MMC.open(path, FILE_READ);
    if (!f) {
        snprintf(g_page_buf, sizeof(g_page_buf), "[ page %d missing ]", page);
        return false;
    }
    size_t sz = f.size();
    if (sz > MAX_PAGE_BYTES) sz = MAX_PAGE_BYTES;
    size_t r = f.read((uint8_t *)g_page_buf, sz);
    g_page_buf[r] = 0;
    f.close();
    return true;
}

// =============================================================================
//  Time / battery helpers
// =============================================================================
static int g_battery_pct = -1;
static void update_battery() {
    if (!g_has_pmu) { g_battery_pct = -1; return; }
    int p = g_pmu.getBatteryPercent();
    if (p < 0) p = 0; if (p > 100) p = 100;
    g_battery_pct = p;
}
static void fmt_time(char *out, size_t n) {
    struct tm tm;
    if (getLocalTime(&tm, 5)) {
        snprintf(out, n, "%02d:%02d", tm.tm_hour, tm.tm_min);
    } else {
        snprintf(out, n, "--:--");
    }
}

// =============================================================================
//  UI - LVGL objects
// =============================================================================
static lv_obj_t *scr_root;

// Boot splash overlay (kept until faded out)
static lv_obj_t *splash_layer = nullptr;
static lv_obj_t *splash_title = nullptr;
static lv_obj_t *splash_sub   = nullptr;
static lv_obj_t *splash_meta1 = nullptr;
static lv_obj_t *splash_meta2 = nullptr;

// Library
static lv_obj_t *lib_layer       = nullptr;
static lv_obj_t *lib_card_left   = nullptr;
static lv_obj_t *lib_card_center = nullptr;
static lv_obj_t *lib_card_right  = nullptr;
static lv_obj_t *lib_title_lbl   = nullptr;
static lv_obj_t *lib_author_lbl  = nullptr;
static lv_obj_t *lib_progress_lbl= nullptr;
static lv_obj_t *lib_continue_lbl= nullptr;
static lv_obj_t *lib_dots_row    = nullptr;
static lv_obj_t *lib_back_btn    = nullptr;
static lv_obj_t *lib_theme_lbl   = nullptr;
static lv_obj_t *lib_empty_lbl   = nullptr;

// Reader
static lv_obj_t *read_layer    = nullptr;
static lv_obj_t *read_top_bar  = nullptr;
static lv_obj_t *read_title    = nullptr;
static lv_obj_t *read_pageinfo = nullptr;
static lv_obj_t *read_text     = nullptr;
static lv_obj_t *read_bot_bar  = nullptr;
static lv_obj_t *read_clock    = nullptr;
static lv_obj_t *read_battery  = nullptr;

// Quick menu (modal)
static lv_obj_t *menu_layer    = nullptr;

// Focus mode flag
static bool g_focus_mode = false;

// =============================================================================
//  Apply theme to all existing widgets
// =============================================================================
static void apply_theme() {
    const Theme &t = TH();
    if (scr_root) {
        lv_obj_set_style_bg_color(scr_root, TC(t.bg), 0);
        lv_obj_set_style_bg_opa(scr_root, LV_OPA_COVER, 0);
    }
    // Library
    if (lib_layer)        lv_obj_set_style_bg_color(lib_layer, TC(t.bg), 0);
    auto card_style = [&](lv_obj_t *c, bool center) {
        if (!c) return;
        lv_obj_set_style_bg_color(c, TC(t.surface), 0);
        lv_obj_set_style_bg_opa(c, LV_OPA_COVER, 0);
        lv_obj_set_style_border_color(c, TC(center ? t.accent : t.border), 0);
        lv_obj_set_style_border_width(c, center ? 2 : 1, 0);
        lv_obj_set_style_radius(c, 14, 0);
    };
    card_style(lib_card_left, false);
    card_style(lib_card_right, false);
    card_style(lib_card_center, true);

    if (lib_title_lbl)    lv_obj_set_style_text_color(lib_title_lbl, TC(t.txt), 0);
    if (lib_author_lbl)   lv_obj_set_style_text_color(lib_author_lbl, TC(t.mute), 0);
    if (lib_progress_lbl) lv_obj_set_style_text_color(lib_progress_lbl, TC(t.accent), 0);
    if (lib_continue_lbl) lv_obj_set_style_text_color(lib_continue_lbl, TC(t.mute), 0);
    if (lib_theme_lbl)    lv_obj_set_style_text_color(lib_theme_lbl, TC(t.mute), 0);
    if (lib_empty_lbl)    lv_obj_set_style_text_color(lib_empty_lbl, TC(t.mute), 0);

    // Reader
    if (read_layer)    lv_obj_set_style_bg_color(read_layer, TC(t.bg), 0);
    if (read_top_bar)  lv_obj_set_style_bg_color(read_top_bar, TC(t.bg), 0);
    if (read_bot_bar)  lv_obj_set_style_bg_color(read_bot_bar, TC(t.bg), 0);
    if (read_title)    lv_obj_set_style_text_color(read_title, TC(t.mute), 0);
    if (read_pageinfo) lv_obj_set_style_text_color(read_pageinfo, TC(t.mute), 0);
    if (read_text)     lv_obj_set_style_text_color(read_text, TC(t.txt), 0);
    if (read_clock)    lv_obj_set_style_text_color(read_clock, TC(t.mute), 0);
    if (read_battery)  lv_obj_set_style_text_color(read_battery, TC(t.mute), 0);
}

// =============================================================================
//  Library UI builders
// =============================================================================
static void update_library_view();   // fwd

static void on_back_btn(lv_event_t *e) {
    (void)e;
    return_to_launcher();
}

static void open_book_at(int idx) {
    if (idx < 0 || idx >= g_book_count) return;
    g_reading_book = idx;
    g_books[idx].current_page = load_book_page(g_books[idx]);
    if (g_books[idx].current_page < 1) g_books[idx].current_page = 1;
    save_session();
    lv_obj_add_flag(lib_layer, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(read_layer, LV_OBJ_FLAG_HIDDEN);
    extern void update_reader_view();
    update_reader_view();
}

static lv_point_t g_press_start = {0, 0};
static uint32_t   g_press_t0    = 0;
static bool       g_swiped      = false;
static uint32_t   g_last_tap_t  = 0;

// ---- Library swipe / tap handler --------------------------------------------
static void lib_input_cb(lv_event_t *e) {
    lv_event_code_t code = lv_event_get_code(e);
    lv_indev_t *indev = lv_indev_get_act();
    if (!indev) return;
    lv_point_t p; lv_indev_get_point(indev, &p);

    if (code == LV_EVENT_PRESSED) {
        g_press_start = p; g_press_t0 = millis(); g_swiped = false;
    } else if (code == LV_EVENT_PRESSING) {
        int dx = p.x - g_press_start.x;
        if (!g_swiped && abs(dx) > 50) {
            g_swiped = true;
            if (dx < 0) {
                if (g_lib_idx < g_book_count - 1) g_lib_idx++;
            } else {
                if (g_lib_idx > 0) g_lib_idx--;
            }
            update_library_view();
            save_session();
        }
    } else if (code == LV_EVENT_RELEASED) {
        uint32_t dt = millis() - g_press_t0;
        int dx = p.x - g_press_start.x, dy = p.y - g_press_start.y;
        if (!g_swiped && abs(dx) < 30 && abs(dy) < 30 && dt < 600) {
            // tap on center area => open book
            if (p.x > 120 && p.x < LCD_WIDTH - 120 && p.y > 120 && p.y < 380) {
                open_book_at(g_lib_idx);
            }
        }
    }
}

static void build_library_ui() {
    lib_layer = lv_obj_create(scr_root);
    lv_obj_remove_style_all(lib_layer);
    lv_obj_set_size(lib_layer, LCD_WIDTH, LCD_HEIGHT);
    lv_obj_set_pos(lib_layer, 0, 0);
    lv_obj_set_style_bg_opa(lib_layer, LV_OPA_COVER, 0);
    lv_obj_clear_flag(lib_layer, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(lib_layer, lib_input_cb, LV_EVENT_PRESSED,   NULL);
    lv_obj_add_event_cb(lib_layer, lib_input_cb, LV_EVENT_PRESSING,  NULL);
    lv_obj_add_event_cb(lib_layer, lib_input_cb, LV_EVENT_RELEASED,  NULL);

    // Header label (theme name, top-left)
    lib_theme_lbl = lv_label_create(lib_layer);
    lv_label_set_text(lib_theme_lbl, "BOOKOS");
    lv_obj_set_style_text_font(lib_theme_lbl, &lv_font_montserrat_14, 0);
    lv_obj_align(lib_theme_lbl, LV_ALIGN_TOP_LEFT, 22, 18);

    // Empty-state label (hidden if books present)
    lib_empty_lbl = lv_label_create(lib_layer);
    lv_label_set_text(lib_empty_lbl,
        "Library is empty.\n\nAdd a folder under\n/WATCH/BookOS/Books/\nwith numbered TXT pages.");
    lv_obj_set_style_text_align(lib_empty_lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(lib_empty_lbl, &lv_font_montserrat_16, 0);
    lv_obj_align(lib_empty_lbl, LV_ALIGN_CENTER, 0, -20);
    lv_obj_add_flag(lib_empty_lbl, LV_OBJ_FLAG_HIDDEN);

    // 3 cards horizontally arranged
    int cardW = 180, cardH = 240;
    int cy = (LCD_HEIGHT - cardH) / 2 - 20;
    lib_card_left = lv_obj_create(lib_layer);
    lv_obj_remove_style_all(lib_card_left);
    lv_obj_set_size(lib_card_left, 120, 200);
    lv_obj_set_pos(lib_card_left, -40, cy + 20);
    lv_obj_set_style_bg_opa(lib_card_left, LV_OPA_50, 0);
    lv_obj_clear_flag(lib_card_left, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);

    lib_card_right = lv_obj_create(lib_layer);
    lv_obj_remove_style_all(lib_card_right);
    lv_obj_set_size(lib_card_right, 120, 200);
    lv_obj_set_pos(lib_card_right, LCD_WIDTH - 80, cy + 20);
    lv_obj_set_style_bg_opa(lib_card_right, LV_OPA_50, 0);
    lv_obj_clear_flag(lib_card_right, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);

    lib_card_center = lv_obj_create(lib_layer);
    lv_obj_remove_style_all(lib_card_center);
    lv_obj_set_size(lib_card_center, cardW, cardH);
    lv_obj_set_pos(lib_card_center, (LCD_WIDTH - cardW) / 2, cy);
    lv_obj_set_style_bg_opa(lib_card_center, LV_OPA_COVER, 0);
    lv_obj_clear_flag(lib_card_center, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);

    // Title / author / progress / continue (under center card)
    lib_title_lbl = lv_label_create(lib_layer);
    lv_label_set_long_mode(lib_title_lbl, LV_LABEL_LONG_DOT);
    lv_obj_set_width(lib_title_lbl, LCD_WIDTH - 40);
    lv_obj_set_style_text_align(lib_title_lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(lib_title_lbl, &lv_font_montserrat_22, 0);
    lv_obj_align(lib_title_lbl, LV_ALIGN_TOP_MID, 0, cy + cardH + 12);

    lib_author_lbl = lv_label_create(lib_layer);
    lv_label_set_long_mode(lib_author_lbl, LV_LABEL_LONG_DOT);
    lv_obj_set_width(lib_author_lbl, LCD_WIDTH - 40);
    lv_obj_set_style_text_align(lib_author_lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(lib_author_lbl, &lv_font_montserrat_14, 0);
    lv_obj_align(lib_author_lbl, LV_ALIGN_TOP_MID, 0, cy + cardH + 42);

    lib_progress_lbl = lv_label_create(lib_layer);
    lv_obj_set_style_text_font(lib_progress_lbl, &lv_font_montserrat_14, 0);
    lv_obj_align(lib_progress_lbl, LV_ALIGN_TOP_MID, 0, cy + cardH + 62);

    lib_continue_lbl = lv_label_create(lib_layer);
    lv_obj_set_style_text_font(lib_continue_lbl, &lv_font_montserrat_14, 0);
    lv_obj_align(lib_continue_lbl, LV_ALIGN_TOP_MID, 0, cy + cardH + 82);

    // Back to Launcher (cyan pill, bottom)
    lib_back_btn = lv_btn_create(lib_layer);
    lv_obj_set_size(lib_back_btn, LCD_WIDTH - 60, 46);
    lv_obj_align(lib_back_btn, LV_ALIGN_BOTTOM_MID, 0, -14);
    lv_obj_set_style_bg_color(lib_back_btn, lv_color_hex(0x05161B), 0);
    lv_obj_set_style_bg_opa(lib_back_btn, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(lib_back_btn, lv_color_hex(CYAN), 0);
    lv_obj_set_style_border_width(lib_back_btn, 2, 0);
    lv_obj_set_style_radius(lib_back_btn, 23, 0);
    lv_obj_t *bl = lv_label_create(lib_back_btn);
    lv_label_set_text(bl, "BACK TO LAUNCHER");
    lv_obj_set_style_text_color(bl, lv_color_hex(CYAN), 0);
    lv_obj_set_style_text_font(bl, &lv_font_montserrat_14, 0);
    lv_obj_center(bl);
    lv_obj_add_event_cb(lib_back_btn, on_back_btn, LV_EVENT_CLICKED, NULL);
}

static void update_library_view() {
    if (!lib_layer) return;
    if (g_book_count == 0) {
        lv_obj_clear_flag(lib_empty_lbl, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(lib_card_left,   LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(lib_card_center, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(lib_card_right,  LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(lib_title_lbl, "");
        lv_label_set_text(lib_author_lbl, "");
        lv_label_set_text(lib_progress_lbl, "");
        lv_label_set_text(lib_continue_lbl, "");
        return;
    }
    lv_obj_add_flag(lib_empty_lbl, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(lib_card_center, LV_OBJ_FLAG_HIDDEN);

    if (g_lib_idx > 0) lv_obj_clear_flag(lib_card_left, LV_OBJ_FLAG_HIDDEN);
    else                lv_obj_add_flag(lib_card_left, LV_OBJ_FLAG_HIDDEN);
    if (g_lib_idx < g_book_count - 1) lv_obj_clear_flag(lib_card_right, LV_OBJ_FLAG_HIDDEN);
    else                              lv_obj_add_flag(lib_card_right, LV_OBJ_FLAG_HIDDEN);

    const Book &b = g_books[g_lib_idx];
    lv_label_set_text(lib_title_lbl, b.title);
    lv_label_set_text(lib_author_lbl, b.author);

    int pct = b.total_pages > 0 ? (b.current_page * 100) / b.total_pages : 0;
    char buf[64];
    snprintf(buf, sizeof(buf), "%d%%  -  page %d / %d", pct, b.current_page, b.total_pages);
    lv_label_set_text(lib_progress_lbl, buf);

    if (b.current_page > 1)
        snprintf(buf, sizeof(buf), "Continue - page %d", b.current_page);
    else
        snprintf(buf, sizeof(buf), "Tap to begin");
    lv_label_set_text(lib_continue_lbl, buf);

    char tag[64];
    snprintf(tag, sizeof(tag), "BOOKOS  -  %d of %d  -  %s",
             g_lib_idx + 1, g_book_count, TH().name);
    lv_label_set_text(lib_theme_lbl, tag);
}

// =============================================================================
//  Reader UI
// =============================================================================
static void reader_advance(int delta);  // fwd
static void open_quick_menu();          // fwd
static void close_quick_menu();         // fwd

static void reader_input_cb(lv_event_t *e) {
    lv_event_code_t code = lv_event_get_code(e);
    lv_indev_t *indev = lv_indev_get_act();
    if (!indev) return;
    lv_point_t p; lv_indev_get_point(indev, &p);

    if (code == LV_EVENT_PRESSED) {
        g_press_start = p; g_press_t0 = millis(); g_swiped = false;
    } else if (code == LV_EVENT_RELEASED) {
        uint32_t dt = millis() - g_press_t0;
        int dx = p.x - g_press_start.x;
        int dy = p.y - g_press_start.y;
        if (g_swiped) return;
        if (abs(dx) < 25 && abs(dy) < 25) {
            // Tap
            if (dt > 600) {
                // Long press -> quick menu
                open_quick_menu();
                return;
            }
            uint32_t now = millis();
            if (now - g_last_tap_t < 320 && p.x > LCD_WIDTH / 3 && p.x < 2 * LCD_WIDTH / 3) {
                // Double-tap center -> toggle focus mode
                g_focus_mode = !g_focus_mode;
                if (g_focus_mode) {
                    lv_obj_add_flag(read_top_bar, LV_OBJ_FLAG_HIDDEN);
                    lv_obj_add_flag(read_bot_bar, LV_OBJ_FLAG_HIDDEN);
                } else {
                    lv_obj_clear_flag(read_top_bar, LV_OBJ_FLAG_HIDDEN);
                    lv_obj_clear_flag(read_bot_bar, LV_OBJ_FLAG_HIDDEN);
                }
                g_last_tap_t = 0;
                return;
            }
            g_last_tap_t = now;
            // Single tap zones
            if (p.x < LCD_WIDTH / 3)            reader_advance(-1);
            else if (p.x > 2 * LCD_WIDTH / 3)   reader_advance(+1);
            // center single tap = no-op (wait for double)
        }
    }
}

static void build_reader_ui() {
    read_layer = lv_obj_create(scr_root);
    lv_obj_remove_style_all(read_layer);
    lv_obj_set_size(read_layer, LCD_WIDTH, LCD_HEIGHT);
    lv_obj_set_pos(read_layer, 0, 0);
    lv_obj_set_style_bg_opa(read_layer, LV_OPA_COVER, 0);
    lv_obj_clear_flag(read_layer, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(read_layer, reader_input_cb, LV_EVENT_PRESSED,  NULL);
    lv_obj_add_event_cb(read_layer, reader_input_cb, LV_EVENT_RELEASED, NULL);

    // top bar
    read_top_bar = lv_obj_create(read_layer);
    lv_obj_remove_style_all(read_top_bar);
    lv_obj_set_size(read_top_bar, LCD_WIDTH, 38);
    lv_obj_set_pos(read_top_bar, 0, 0);
    lv_obj_set_style_bg_opa(read_top_bar, LV_OPA_COVER, 0);
    lv_obj_clear_flag(read_top_bar, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);

    read_title = lv_label_create(read_top_bar);
    lv_label_set_long_mode(read_title, LV_LABEL_LONG_DOT);
    lv_obj_set_width(read_title, LCD_WIDTH - 140);
    lv_obj_set_style_text_font(read_title, &lv_font_montserrat_14, 0);
    lv_obj_align(read_title, LV_ALIGN_LEFT_MID, 18, 0);

    read_pageinfo = lv_label_create(read_top_bar);
    lv_obj_set_style_text_font(read_pageinfo, &lv_font_montserrat_14, 0);
    lv_obj_align(read_pageinfo, LV_ALIGN_RIGHT_MID, -18, 0);

    // body text
    read_text = lv_label_create(read_layer);
    lv_label_set_long_mode(read_text, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(read_text, 340);
    lv_obj_set_pos(read_text, (LCD_WIDTH - 340) / 2, 64);
    lv_obj_set_height(read_text, 380);
    lv_obj_set_style_text_font(read_text, &lv_font_montserrat_22, 0);
    lv_obj_set_style_text_line_space(read_text, 6, 0);

    // bottom bar
    read_bot_bar = lv_obj_create(read_layer);
    lv_obj_remove_style_all(read_bot_bar);
    lv_obj_set_size(read_bot_bar, LCD_WIDTH, 36);
    lv_obj_set_pos(read_bot_bar, 0, LCD_HEIGHT - 36);
    lv_obj_set_style_bg_opa(read_bot_bar, LV_OPA_COVER, 0);
    lv_obj_clear_flag(read_bot_bar, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);

    read_clock = lv_label_create(read_bot_bar);
    lv_obj_set_style_text_font(read_clock, &lv_font_montserrat_14, 0);
    lv_obj_align(read_clock, LV_ALIGN_LEFT_MID, 18, 0);

    read_battery = lv_label_create(read_bot_bar);
    lv_obj_set_style_text_font(read_battery, &lv_font_montserrat_14, 0);
    lv_obj_align(read_battery, LV_ALIGN_RIGHT_MID, -18, 0);

    lv_obj_add_flag(read_layer, LV_OBJ_FLAG_HIDDEN);
}

void update_reader_view() {
    if (g_reading_book < 0) return;
    Book &b = g_books[g_reading_book];
    load_page(b, b.current_page);
    lv_label_set_text(read_text, g_page_buf);
    lv_label_set_text(read_title, b.title);

    char info[40];
    snprintf(info, sizeof(info), "%d / %d", b.current_page, b.total_pages);
    lv_label_set_text(read_pageinfo, info);

    char tt[16]; fmt_time(tt, sizeof(tt));
    lv_label_set_text(read_clock, tt);
    update_battery();
    char bb[16];
    if (g_battery_pct >= 0) snprintf(bb, sizeof(bb), "BAT %d%%", g_battery_pct);
    else                    snprintf(bb, sizeof(bb), "BAT --");
    lv_label_set_text(read_battery, bb);
}

static void reader_advance(int delta) {
    if (g_reading_book < 0) return;
    Book &b = g_books[g_reading_book];
    int p = b.current_page + delta;
    if (p < 1 || p > b.total_pages) return;
    b.current_page = p;
    save_book_page(b);
    save_session();
    update_reader_view();
}

// =============================================================================
//  Quick menu (modal): Theme cycle, Bookmark toggle, Exit to library, Close
// =============================================================================
static void qm_close_cb(lv_event_t *) { close_quick_menu(); }

static void qm_theme_cb(lv_event_t *) {
    g_theme_idx = (g_theme_idx + 1) % NUM_THEMES;
    prefs_save_theme();
    apply_theme();
}

static void qm_bookmark_cb(lv_event_t *) {
    if (g_reading_book >= 0) toggle_bookmark(g_books[g_reading_book]);
    close_quick_menu();
}

static void qm_exit_cb(lv_event_t *) {
    // Return to library
    save_session();
    g_reading_book = -1;
    g_focus_mode = false;
    lv_obj_clear_flag(read_top_bar, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(read_bot_bar, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(read_layer, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(lib_layer, LV_OBJ_FLAG_HIDDEN);
    update_library_view();
    close_quick_menu();
}

static void qm_launcher_cb(lv_event_t *) {
    return_to_launcher();
}

static void open_quick_menu() {
    if (menu_layer) return;
    const Theme &t = TH();
    menu_layer = lv_obj_create(scr_root);
    lv_obj_remove_style_all(menu_layer);
    lv_obj_set_size(menu_layer, LCD_WIDTH, LCD_HEIGHT);
    lv_obj_set_pos(menu_layer, 0, 0);
    lv_obj_set_style_bg_color(menu_layer, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(menu_layer, LV_OPA_70, 0);
    lv_obj_clear_flag(menu_layer, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(menu_layer, qm_close_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *panel = lv_obj_create(menu_layer);
    lv_obj_remove_style_all(panel);
    lv_obj_set_size(panel, 320, 380);
    lv_obj_align(panel, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(panel, TC(t.surface), 0);
    lv_obj_set_style_bg_opa(panel, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(panel, TC(t.border), 0);
    lv_obj_set_style_border_width(panel, 1, 0);
    lv_obj_set_style_radius(panel, 16, 0);
    lv_obj_set_style_pad_all(panel, 18, 0);
    lv_obj_clear_flag(panel, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title = lv_label_create(panel);
    lv_label_set_text(title, "QUICK MENU");
    lv_obj_set_style_text_color(title, TC(t.mute), 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_14, 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 0);

    auto make_btn = [&](const char *label, int y, lv_event_cb_t cb, uint32_t border) {
        lv_obj_t *b = lv_btn_create(panel);
        lv_obj_set_size(b, 270, 54);
        lv_obj_align(b, LV_ALIGN_TOP_MID, 0, y);
        lv_obj_set_style_bg_color(b, TC(t.bg), 0);
        lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
        lv_obj_set_style_border_color(b, lv_color_hex(border), 0);
        lv_obj_set_style_border_width(b, 1, 0);
        lv_obj_set_style_radius(b, 12, 0);
        lv_obj_t *l = lv_label_create(b);
        lv_label_set_text(l, label);
        lv_obj_set_style_text_color(l, TC(t.txt), 0);
        lv_obj_set_style_text_font(l, &lv_font_montserrat_16, 0);
        lv_obj_center(l);
        lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, NULL);
        return b;
    };

    char tbuf[48]; snprintf(tbuf, sizeof(tbuf), "THEME: %s", t.name);
    make_btn(tbuf,            38,  qm_theme_cb,    t.accent);
    make_btn("TOGGLE BOOKMARK",100, qm_bookmark_cb, t.border);
    make_btn("BACK TO LIBRARY",162, qm_exit_cb,     t.border);
    make_btn("BACK TO LAUNCHER",224, qm_launcher_cb, CYAN);

    lv_obj_t *hint = lv_label_create(panel);
    lv_label_set_text(hint, "tap outside to close");
    lv_obj_set_style_text_color(hint, TC(t.mute), 0);
    lv_obj_set_style_text_font(hint, &lv_font_montserrat_14, 0);
    lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, 0);
}
static void close_quick_menu() {
    if (!menu_layer) return;
    lv_obj_del(menu_layer);
    menu_layer = nullptr;
}

// =============================================================================
//  Boot splash -- "ARCHIVE RECOVERY"
// =============================================================================
static void build_splash_ui() {
    splash_layer = lv_obj_create(scr_root);
    lv_obj_remove_style_all(splash_layer);
    lv_obj_set_size(splash_layer, LCD_WIDTH, LCD_HEIGHT);
    lv_obj_set_pos(splash_layer, 0, 0);
    lv_obj_set_style_bg_color(splash_layer, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(splash_layer, LV_OPA_COVER, 0);
    lv_obj_clear_flag(splash_layer, LV_OBJ_FLAG_SCROLLABLE);

    splash_title = lv_label_create(splash_layer);
    lv_label_set_text(splash_title, "BOOKOS");
    lv_obj_set_style_text_color(splash_title, lv_color_hex(0xEEEEEE), 0);
    lv_obj_set_style_text_font(splash_title, &lv_font_montserrat_32, 0);
    lv_obj_set_style_text_letter_space(splash_title, 6, 0);
    lv_obj_align(splash_title, LV_ALIGN_CENTER, 0, -50);

    splash_sub = lv_label_create(splash_layer);
    lv_label_set_text(splash_sub, "");
    lv_obj_set_style_text_color(splash_sub, lv_color_hex(0x707070), 0);
    lv_obj_set_style_text_font(splash_sub, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_letter_space(splash_sub, 2, 0);
    lv_obj_align(splash_sub, LV_ALIGN_CENTER, 0, -10);

    splash_meta1 = lv_label_create(splash_layer);
    lv_label_set_text(splash_meta1, "");
    lv_obj_set_style_text_color(splash_meta1, lv_color_hex(0x4FB3FF), 0);
    lv_obj_set_style_text_font(splash_meta1, &lv_font_montserrat_14, 0);
    lv_obj_align(splash_meta1, LV_ALIGN_CENTER, 0, 40);

    splash_meta2 = lv_label_create(splash_layer);
    lv_label_set_text(splash_meta2, "");
    lv_obj_set_style_text_color(splash_meta2, lv_color_hex(0x4FB3FF), 0);
    lv_obj_set_style_text_font(splash_meta2, &lv_font_montserrat_14, 0);
    lv_obj_align(splash_meta2, LV_ALIGN_CENTER, 0, 70);

    // Version footer
    lv_obj_t *ver = lv_label_create(splash_layer);
    lv_label_set_text(ver, "v1.0  -  offline archive");
    lv_obj_set_style_text_color(ver, lv_color_hex(0x404040), 0);
    lv_obj_set_style_text_font(ver, &lv_font_montserrat_14, 0);
    lv_obj_align(ver, LV_ALIGN_BOTTOM_MID, 0, -22);
}

static void splash_set(const char *sub, const char *m1, const char *m2) {
    if (splash_sub)   lv_label_set_text(splash_sub,   sub ? sub : "");
    if (splash_meta1) lv_label_set_text(splash_meta1, m1 ? m1 : "");
    if (splash_meta2) lv_label_set_text(splash_meta2, m2 ? m2 : "");
    lv_timer_handler();
}

static void splash_run_and_dismiss() {
    splash_set("Recovering archive...", "", "");
    delay(420);
    lv_timer_handler();

    char m1[40], m2[40];
    snprintf(m1, sizeof(m1), "%d books found", g_book_count);
    snprintf(m2, sizeof(m2), "%d bookmarks restored", g_bookmarks_restored);
    splash_set("Scanning library...", m1, m2);
    delay(620);
    lv_timer_handler();

    splash_set("Restoring last session...", m1, m2);
    delay(380);
    lv_timer_handler();

    // Unhide library beneath so the fade reveals it
    lv_obj_clear_flag(lib_layer, LV_OBJ_FLAG_HIDDEN);
    // Make sure splash is the topmost layer during the fade
    lv_obj_move_foreground(splash_layer);

    // Fade out splash via opacity animation
    for (int o = LV_OPA_COVER; o >= 0; o -= 24) {
        lv_obj_set_style_bg_opa(splash_layer, o < 0 ? 0 : o, 0);
        lv_obj_set_style_text_opa(splash_title, o < 0 ? 0 : o, 0);
        lv_obj_set_style_text_opa(splash_sub,   o < 0 ? 0 : o, 0);
        lv_obj_set_style_text_opa(splash_meta1, o < 0 ? 0 : o, 0);
        lv_obj_set_style_text_opa(splash_meta2, o < 0 ? 0 : o, 0);
        lv_timer_handler();
        delay(24);
    }
    lv_obj_add_flag(splash_layer, LV_OBJ_FLAG_HIDDEN);
}

// =============================================================================
//  setup() / loop()
// =============================================================================
static void build_ui_root() {
    scr_root = lv_scr_act();
    lv_obj_set_style_bg_color(scr_root, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(scr_root, LV_OPA_COVER, 0);
    lv_obj_clear_flag(scr_root, LV_OBJ_FLAG_SCROLLABLE);
}

void setup() {
    launcher_app_boot();
    panel_on(220);

    build_ui_root();
    build_splash_ui();

    // First-tick render so splash is on screen instantly
    lv_timer_handler();

    // Bootstrap SD folders + seed demo book before scan
    ensure_dirs();
    seed_demo_book();

    prefs_load();

    scan_books();
    restore_session_select();

    // Build the rest of the UI offscreen, hidden
    build_library_ui();
    build_reader_ui();
    lv_obj_add_flag(lib_layer, LV_OBJ_FLAG_HIDDEN);
    apply_theme();
    update_library_view();
    lv_timer_handler();

    // Now run the loading sequence + fade out
    splash_run_and_dismiss();

    lv_obj_clear_flag(lib_layer, LV_OBJ_FLAG_HIDDEN);

    setCpuFrequencyMhz(160);
}

void loop() {
    lv_timer_handler();

    // BOOT-button long press = always-on safety exit
    static uint32_t btn_t = 0;
    if (digitalRead(BOOT_BTN_PIN) == LOW) {
        if (!btn_t) btn_t = millis();
        else if (millis() - btn_t > 1500) return_to_launcher();
    } else {
        btn_t = 0;
    }

    // Periodic clock/battery refresh in reader bottom bar
    static uint32_t tick_t = 0;
    if (millis() - tick_t > 5000) {
        tick_t = millis();
        if (g_reading_book >= 0 && !g_focus_mode) {
            char tt[16]; fmt_time(tt, sizeof(tt));
            lv_label_set_text(read_clock, tt);
            update_battery();
            char bb[16];
            if (g_battery_pct >= 0) snprintf(bb, sizeof(bb), "BAT %d%%", g_battery_pct);
            else                    snprintf(bb, sizeof(bb), "BAT --");
            lv_label_set_text(read_battery, bb);
        }
    }

    delay(5);
}
