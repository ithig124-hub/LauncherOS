// =============================================================================
//  MapOS.ino v1.2 -- LauncherOS-compatible offline RGB565 tile map viewer
//  Target  : Waveshare ESP32-S3 Touch AMOLED 2.06 (410 x 502, CO5300, FT3168)
//  Compile : ESP32-S3 Dev Module / PSRAM=OPI / Flash=16MB / Minimal SPIFFS
//  Export  : Sketch -> Export Compiled Binary -> copy MapOS.ino.bin to
//            /WATCH/bin/mapos.bin on the SD card.
// -----------------------------------------------------------------------------
//  v1.2 changes:
//    * BookOS-style LIBRARY carousel. Each map is its own folder under:
//        /WATCH/MapOS/tiles/<MapName>/z<Z>/x<X>/y<Y>.raw
//      MapOS scans /WATCH/MapOS/tiles/ on boot and lets you cycle through
//      every map you have on the card, then "opens" the selected one.
//    * Per-map metadata via /WATCH/MapOS/tiles/<MapName>/meta.txt:
//        TITLE=Paris
//        DESC=Inside the boulevard peripherique
//        CENTER_LAT=48.8566
//        CENTER_LON=2.3522
//        DEFAULT_ZOOM=14
//      meta.txt is optional; if absent MapOS opens at (0,0) and you pan.
//    * Last-opened map remembered in NVS namespace "mapos".
//
//  ALL CONTROL through hardware BOOT button (GPIO0):
//    LIBRARY mode:
//      tap (< 400 ms)      -> next map (carousel right)
//      double-tap (<500 ms)-> previous map (carousel left)
//      hold 0.5 - 1.2 s    -> OPEN this map
//      hold > 1.5 s        -> back to launcher
//    MAP mode:
//      tap                 -> zoom in (wraps)
//      double-tap          -> zoom out
//      hold 0.5 - 1.2 s    -> recentre on "me" (or map centre if no me)
//      hold > 1.5 s        -> back to LIBRARY
//
//  Touch is unchanged: drag to pan, ~1 s long-press (no drag) = breadcrumb.
//
//  SD layout (this OS only ever uses paths under /WATCH/MapOS/):
//    /WATCH/MapOS/tiles/<MapName>/z<Z>/x<X>/y<Y>.raw   <- 256x256 RGB565
//    /WATCH/MapOS/tiles/<MapName>/meta.txt              <- optional per-map info
//    /WATCH/MapOS/me.txt                                <- global "you are here"
//    /WATCH/MapOS/breadcrumbs.csv                       <- appended on long-press
// =============================================================================

#include <Arduino.h>
#include <FS.h>
#include <SD_MMC.h>
#include <Wire.h>
#include <Preferences.h>
#include <math.h>
#include <time.h>
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
#include <esp_bt.h>

// ===== SHARED BOOT BLOCK =====================================================
#define LCD_SDIO0 4
#define LCD_SDIO1 5
#define LCD_SDIO2 6
#define LCD_SDIO3 7
#define LCD_SCLK 11
#define LCD_CS 12
#define LCD_RESET 8
#define LCD_WIDTH 410
#define LCD_HEIGHT 502
#define IIC_SDA 15
#define IIC_SCL 14
#define TP_INT 38
#define TP_RESET 9
static const int SDMMC_CLK=2, SDMMC_CMD=1, SDMMC_DATA=3;
#define BOOT_BTN_PIN 0
#define PWR_BTN_PIN 10

Arduino_DataBus *bus = new Arduino_ESP32QSPI(LCD_CS, LCD_SCLK, LCD_SDIO0, LCD_SDIO1, LCD_SDIO2, LCD_SDIO3);
Arduino_GFX *gfx = new Arduino_CO5300(bus, LCD_RESET, 0, LCD_WIDTH, LCD_HEIGHT, 22, 0, 0, 0);
std::shared_ptr<Arduino_IIC_DriveBus> IIC_Bus = std::make_shared<Arduino_HWIIC>(IIC_SDA, IIC_SCL, &Wire);
void Arduino_IIC_Touch_Interrupt(void);
std::unique_ptr<Arduino_IIC> FT3168(new Arduino_FT3x68(IIC_Bus, FT3168_DEVICE_ADDRESS, DRIVEBUS_DEFAULT_VALUE, TP_INT, Arduino_IIC_Touch_Interrupt));
void Arduino_IIC_Touch_Interrupt(void) { FT3168->IIC_Interrupt_Flag = true; }

static lv_disp_draw_buf_t draw_buf;
static lv_color_t *lvgl_buf=nullptr;
static lv_disp_drv_t disp_drv;
static lv_indev_drv_t indev_drv;

static volatile bool g_tch_pressed=false;
static volatile int32_t g_tch_x=0, g_tch_y=0;

static void my_disp_flush(lv_disp_drv_t *d, const lv_area_t *a, lv_color_t *px) {
    uint32_t w=a->x2-a->x1+1, h=a->y2-a->y1+1;
    gfx->draw16bitRGBBitmap(a->x1, a->y1, (uint16_t*)px, w, h);
    lv_disp_flush_ready(d);
}
static void my_touchpad_read(lv_indev_drv_t *, lv_indev_data_t *data) {
    int32_t x=FT3168->IIC_Read_Device_Value(FT3168->Arduino_IIC_Touch::Value_Information::TOUCH_COORDINATE_X);
    int32_t y=FT3168->IIC_Read_Device_Value(FT3168->Arduino_IIC_Touch::Value_Information::TOUCH_COORDINATE_Y);
    int fingers=FT3168->IIC_Read_Device_Value(FT3168->Arduino_IIC_Touch::Value_Information::TOUCH_FINGER_NUMBER);
    if (fingers > 0) {
        data->state=LV_INDEV_STATE_PR; data->point.x=x; data->point.y=y;
        g_tch_pressed=true; g_tch_x=x; g_tch_y=y;
    } else { data->state=LV_INDEV_STATE_REL; g_tch_pressed=false; }
}

XPowersPMU g_pmu; static bool g_has_pmu=false;
static void pmu_init() { if (g_pmu.begin(Wire, AXP2101_SLAVE_ADDRESS, IIC_SDA, IIC_SCL)) { g_has_pmu=true; g_pmu.disableTSPinMeasure(); g_pmu.enableBattVoltageMeasure(); } }

static void return_to_launcher() {
    const esp_partition_t *factory = esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_FACTORY, NULL);
    if (factory) esp_ota_set_boot_partition(factory);
    else { const esp_partition_t *o = esp_ota_get_next_update_partition(NULL); if (o && o!=esp_ota_get_running_partition()) esp_ota_set_boot_partition(o); }
    delay(80); esp_restart();
}

static void launcher_app_boot() {
    Serial.begin(115200);
    pinMode(BOOT_BTN_PIN, INPUT_PULLUP); pinMode(PWR_BTN_PIN, INPUT_PULLUP); btStop();
    if (!gfx->begin()) Serial.println("gfx fail"); gfx->fillScreen(0x0000);
    pinMode(IIC_SCL, OUTPUT);
    for (int i=0; i<9; i++) { digitalWrite(IIC_SCL,LOW); delayMicroseconds(5); digitalWrite(IIC_SCL,HIGH); delayMicroseconds(5); }
    Wire.begin(IIC_SDA, IIC_SCL);
    pinMode(TP_RESET, OUTPUT); digitalWrite(TP_RESET, LOW); delay(50); digitalWrite(TP_RESET, HIGH); delay(200);
    FT3168->begin(); pmu_init();
    lv_init();
    size_t bufPx = LCD_WIDTH * 20;
    lvgl_buf = (lv_color_t*)heap_caps_malloc(bufPx*sizeof(lv_color_t), MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT);
    if (!lvgl_buf) lvgl_buf = (lv_color_t*)heap_caps_malloc(bufPx*sizeof(lv_color_t), MALLOC_CAP_8BIT);
    lv_disp_draw_buf_init(&draw_buf, lvgl_buf, NULL, bufPx);
    lv_disp_drv_init(&disp_drv); disp_drv.flush_cb=my_disp_flush; disp_drv.draw_buf=&draw_buf;
    disp_drv.hor_res=LCD_WIDTH; disp_drv.ver_res=LCD_HEIGHT;
    lv_disp_drv_register(&disp_drv);
    lv_indev_drv_init(&indev_drv); indev_drv.type=LV_INDEV_TYPE_POINTER; indev_drv.read_cb=my_touchpad_read;
    lv_indev_drv_register(&indev_drv);
    SD_MMC.setPins(SDMMC_CLK, SDMMC_CMD, SDMMC_DATA); SD_MMC.begin("/sdcard", true);
}
// ===== END SHARED BOOT BLOCK =================================================

static void splash_show() {
    for (int f=0; f<10; f++) {
        gfx->fillScreen(0x0000);
        for (int x=0; x<LCD_WIDTH; x+=20) gfx->drawLine(x, 0, x, LCD_HEIGHT, 0x10A2);
        for (int y=0; y<LCD_HEIGHT; y+=20) gfx->drawLine(0, y, LCD_WIDTH, y, 0x10A2);
        int cx=LCD_WIDTH/2, cy=LCD_HEIGHT/2;
        gfx->drawLine(cx-30+f, cy, cx+30+f, cy, 0x07FF);
        gfx->drawLine(cx, cy-30, cx, cy+30, 0x07FF);
        gfx->drawCircle(cx, cy, 14, 0x07FF);
        gfx->setTextColor(0xFFFF); gfx->setTextSize(3);
        gfx->setCursor(cx - 70, LCD_HEIGHT - 80);
        gfx->print("MAP//OS");
        delay(50);
    }
}

// =============================================================================
//  Library state
// =============================================================================
#define MAX_MAPS 32
static String g_maps[MAX_MAPS];
static int    g_map_count = 0;
static int    g_map_sel   = 0;       // index into g_maps in library view

static String g_active_map;          // empty in library, else folder name

// =============================================================================
//  Map (active) state
// =============================================================================
#define TILE_SZ 256
static int    g_zoom = 13;
static int    g_min_zoom = 1, g_max_zoom = 19;
static float  g_my_lat = 0, g_my_lon = 0;
static bool   g_have_me = false;
static double g_centre_lat = 0, g_centre_lon = 0;
static int    g_pan_dx = 0, g_pan_dy = 0;
static bool   g_need_redraw = true;

// Per-map metadata (parsed from meta.txt)
static String g_meta_title, g_meta_desc;
static bool   g_meta_has_centre = false;
static double g_meta_centre_lat = 0, g_meta_centre_lon = 0;
static int    g_meta_default_zoom = -1;

// =============================================================================
//  Modes
// =============================================================================
enum Mode { MODE_LIBRARY, MODE_MAP };
static Mode g_mode = MODE_LIBRARY;
static bool g_lib_redraw = true;

Preferences g_prefs;

// =============================================================================
//  Helpers
// =============================================================================
static void lonlat_to_tile_xy(double lat, double lon, int z, double *tx, double *ty) {
    double n = (double)(1 << z);
    *tx = (lon + 180.0) / 360.0 * n;
    double lat_r = lat * M_PI / 180.0;
    *ty = (1.0 - log(tan(lat_r) + 1.0/cos(lat_r)) / M_PI) / 2.0 * n;
}

static void scan_maps() {
    g_map_count = 0;
    SD_MMC.mkdir("/WATCH");
    SD_MMC.mkdir("/WATCH/MapOS");
    SD_MMC.mkdir("/WATCH/MapOS/tiles");
    File d = SD_MMC.open("/WATCH/MapOS/tiles");
    if (!d) return;
    File ent = d.openNextFile();
    while (ent && g_map_count < MAX_MAPS) {
        if (ent.isDirectory()) {
            String n = ent.name();
            int p = n.lastIndexOf('/');
            if (p >= 0) n = n.substring(p+1);
            if (n.length() > 0 && !n.startsWith(".")) {
                g_maps[g_map_count++] = n;
            }
        }
        ent = d.openNextFile();
    }
}

static void scan_zoom_range_for_active() {
    g_min_zoom = 999; g_max_zoom = -1;
    if (g_active_map.length() == 0) return;
    String base = "/WATCH/MapOS/tiles/" + g_active_map;
    File d = SD_MMC.open(base);
    if (!d) return;
    File ent = d.openNextFile();
    while (ent) {
        String n = ent.name();
        int p = n.lastIndexOf('/'); if (p >= 0) n = n.substring(p+1);
        if (n.startsWith("z")) {
            int z = n.substring(1).toInt();
            if (z < g_min_zoom) g_min_zoom = z;
            if (z > g_max_zoom) g_max_zoom = z;
        }
        ent = d.openNextFile();
    }
    if (g_min_zoom == 999) { g_min_zoom = g_max_zoom = 13; }
    if (g_zoom < g_min_zoom) g_zoom = g_min_zoom;
    if (g_zoom > g_max_zoom) g_zoom = g_max_zoom;
}

static String trim_str(const String &s) {
    int a = 0, b = s.length();
    while (a < b && (s[a] == ' ' || s[a] == '\t' || s[a] == '\r' || s[a] == '\n')) a++;
    while (b > a && (s[b-1] == ' ' || s[b-1] == '\t' || s[b-1] == '\r' || s[b-1] == '\n')) b--;
    return s.substring(a, b);
}

static void load_active_meta() {
    g_meta_title = g_active_map;
    g_meta_desc = "";
    g_meta_has_centre = false;
    g_meta_default_zoom = -1;
    String path = "/WATCH/MapOS/tiles/" + g_active_map + "/meta.txt";
    File f = SD_MMC.open(path, FILE_READ);
    if (!f) return;
    while (f.available()) {
        String line = f.readStringUntil('\n');
        int eq = line.indexOf('=');
        if (eq < 1) continue;
        String k = trim_str(line.substring(0, eq));
        String v = trim_str(line.substring(eq + 1));
        if      (k == "TITLE")        g_meta_title = v;
        else if (k == "DESC")         g_meta_desc = v;
        else if (k == "CENTER_LAT")   { g_meta_centre_lat = v.toFloat(); g_meta_has_centre = true; }
        else if (k == "CENTER_LON")   { g_meta_centre_lon = v.toFloat(); g_meta_has_centre = true; }
        else if (k == "DEFAULT_ZOOM") { g_meta_default_zoom = v.toInt(); }
    }
    f.close();
}

static void load_me() {
    File f = SD_MMC.open("/WATCH/MapOS/me.txt", FILE_READ);
    g_have_me = false;
    if (!f) return;
    String s = f.readString(); f.close();
    int c = s.indexOf(','); if (c < 0) return;
    g_my_lat = s.substring(0, c).toFloat();
    g_my_lon = s.substring(c+1).toFloat();
    g_have_me = true;
}

static bool draw_tile(int z, int tx, int ty, int sx, int sy) {
    char path[120];
    snprintf(path, sizeof(path), "/WATCH/MapOS/tiles/%s/z%d/x%d/y%d.raw",
             g_active_map.c_str(), z, tx, ty);
    File f = SD_MMC.open(path, FILE_READ);
    if (!f) {
        gfx->fillRect(sx, sy, TILE_SZ, TILE_SZ, 0x18C3);
        gfx->drawRect(sx, sy, TILE_SZ, TILE_SZ, 0x2945);
        return false;
    }
    static uint8_t row[TILE_SZ*2];
    int y_clip_start = (sy < 0) ? -sy : 0;
    int y_clip_end   = (sy + TILE_SZ > LCD_HEIGHT) ? (LCD_HEIGHT - sy) : TILE_SZ;
    if (y_clip_start > 0) f.seek(y_clip_start * TILE_SZ * 2);
    for (int row_y = y_clip_start; row_y < y_clip_end; row_y++) {
        if (f.read(row, TILE_SZ*2) != TILE_SZ*2) break;
        gfx->draw16bitRGBBitmap(sx, sy + row_y, (uint16_t*)row, TILE_SZ, 1);
    }
    f.close();
    return true;
}

static void draw_overlay_text() {
    char b[40];
    int bat = -1;
    if (g_has_pmu) bat = g_pmu.getBatteryPercent();
    String title = g_meta_title.length() ? g_meta_title : g_active_map;
    if (bat >= 0) snprintf(b, sizeof(b), "%s  z%d  %d%%", title.c_str(), g_zoom, bat);
    else          snprintf(b, sizeof(b), "%s  z%d",       title.c_str(), g_zoom);
    int w = (int)strlen(b) * 6 + 14;
    if (w > LCD_WIDTH - 16) w = LCD_WIDTH - 16;
    gfx->fillRect(8, 8, w, 22, 0x0000);
    gfx->drawRect(8, 8, w, 22, 0x2945);
    gfx->setTextColor(0x07FF); gfx->setTextSize(1);
    gfx->setCursor(16, 16); gfx->print(b);
}

static void redraw_map() {
    if (!g_need_redraw) return;
    g_need_redraw = false;
    double tx_f, ty_f;
    lonlat_to_tile_xy(g_centre_lat, g_centre_lon, g_zoom, &tx_f, &ty_f);
    int ct_x = (int)floor(tx_f);
    int ct_y = (int)floor(ty_f);
    int off_x = (int)((tx_f - ct_x) * TILE_SZ);
    int off_y = (int)((ty_f - ct_y) * TILE_SZ);
    int cx = LCD_WIDTH/2, cy = LCD_HEIGHT/2;
    for (int j = -1; j <= 1; j++) {
        for (int i = -1; i <= 1; i++) {
            int sx = cx + i*TILE_SZ - off_x + g_pan_dx;
            int sy = cy + j*TILE_SZ - off_y + g_pan_dy;
            if (sx + TILE_SZ < 0 || sy + TILE_SZ < 0 || sx > LCD_WIDTH || sy > LCD_HEIGHT) continue;
            draw_tile(g_zoom, ct_x + i, ct_y + j, sx, sy);
        }
    }
    if (g_have_me) {
        double mx_f, my_f;
        lonlat_to_tile_xy(g_my_lat, g_my_lon, g_zoom, &mx_f, &my_f);
        int dx = (int)((mx_f - tx_f) * TILE_SZ) + cx + g_pan_dx;
        int dy = (int)((my_f - ty_f) * TILE_SZ) + cy + g_pan_dy;
        gfx->fillCircle(dx, dy, 8, 0xF800);
        gfx->drawCircle(dx, dy, 9, 0xFFFF);
        gfx->drawCircle(dx, dy, 12, 0xF800);
    }
    gfx->drawLine(cx-12, cy, cx+12, cy, 0x07FF);
    gfx->drawLine(cx, cy-12, cx, cy+12, 0x07FF);
    draw_overlay_text();
}

// =============================================================================
//  Library rendering
// =============================================================================
static void redraw_library() {
    if (!g_lib_redraw) return;
    g_lib_redraw = false;
    gfx->fillScreen(0x0000);
    for (int x=0; x<LCD_WIDTH; x+=40) gfx->drawLine(x, 0, x, LCD_HEIGHT, 0x0841);
    for (int y=0; y<LCD_HEIGHT; y+=40) gfx->drawLine(0, y, LCD_WIDTH, y, 0x0841);

    int cx = LCD_WIDTH/2;
    gfx->setTextColor(0x07FF); gfx->setTextSize(2);
    gfx->setCursor(cx - 60, 28); gfx->print("MAP // OS");

    gfx->setTextColor(0x52AA); gfx->setTextSize(1);
    gfx->setCursor(cx - 50, 56); gfx->print("library");

    if (g_map_count == 0) {
        gfx->setTextColor(0xFFFF); gfx->setTextSize(1);
        const char *lines[] = {
            "No maps found.",
            "",
            "Put a folder of tiles on the card:",
            "/WATCH/MapOS/tiles/<MapName>/",
            "  z<Z>/x<X>/y<Y>.raw   (RGB565)",
            "  meta.txt             (optional)",
            "",
            "Use tools/prep_tiles.py from the",
            "MapOS bundle to generate them.",
            "",
            "BOOT long-press  -> back to launcher",
        };
        int y = 110;
        for (auto &ln : lines) { gfx->setCursor(28, y); gfx->print(ln); y += 16; }
    } else {
        // Big centre card: current map
        String &name = g_maps[g_map_sel];

        // Try to load meta for the card display (without setting active state)
        String title = name;
        String desc  = "";
        String path = "/WATCH/MapOS/tiles/" + name + "/meta.txt";
        File f = SD_MMC.open(path, FILE_READ);
        if (f) {
            while (f.available()) {
                String line = f.readStringUntil('\n');
                int eq = line.indexOf('=');
                if (eq < 1) continue;
                String k = trim_str(line.substring(0, eq));
                String v = trim_str(line.substring(eq + 1));
                if      (k == "TITLE") title = v;
                else if (k == "DESC")  desc  = v;
            }
            f.close();
        }

        int card_x = 20, card_y = 110, card_w = LCD_WIDTH - 40, card_h = 220;
        gfx->fillRoundRect(card_x, card_y, card_w, card_h, 16, 0x10A2);
        gfx->drawRoundRect(card_x, card_y, card_w, card_h, 16, 0x07FF);

        gfx->setTextColor(0xFFFF); gfx->setTextSize(2);
        gfx->setCursor(card_x + 18, card_y + 20);
        // Truncate to ~16 chars at size 2
        String shown = title.length() > 18 ? title.substring(0, 17) + "~" : title;
        gfx->print(shown);

        gfx->setTextColor(0x52AA); gfx->setTextSize(1);
        gfx->setCursor(card_x + 18, card_y + 50); gfx->print("folder: "); gfx->print(name);

        gfx->setTextColor(0xCEDB); gfx->setTextSize(1);
        // simple word-wrap of desc
        int max_chars = (card_w - 36) / 6;
        int dy = card_y + 80;
        String d = desc;
        while (d.length() > 0 && dy < card_y + card_h - 14) {
            String chunk;
            if ((int)d.length() <= max_chars) { chunk = d; d = ""; }
            else {
                int br = max_chars;
                while (br > 0 && d[br] != ' ') br--;
                if (br == 0) br = max_chars;
                chunk = d.substring(0, br);
                d = trim_str(d.substring(br));
            }
            gfx->setCursor(card_x + 18, dy); gfx->print(chunk);
            dy += 14;
        }

        // Carousel arrows + counter
        gfx->setTextColor(0x07FF); gfx->setTextSize(3);
        gfx->setCursor(card_x - 14, card_y + card_h/2 - 12);
        if (g_map_count > 1) gfx->print("<");
        gfx->setCursor(card_x + card_w - 4, card_y + card_h/2 - 12);
        if (g_map_count > 1) gfx->print(">");

        gfx->setTextColor(0x07FF); gfx->setTextSize(1);
        char counter[32];
        snprintf(counter, sizeof(counter), "%d / %d", g_map_sel + 1, g_map_count);
        gfx->setCursor(cx - 16, card_y + card_h + 14);
        gfx->print(counter);
    }

    // Bottom hint area
    int hy = LCD_HEIGHT - 110;
    gfx->setTextColor(0x52AA); gfx->setTextSize(1);
    gfx->setCursor(20, hy);      gfx->print("BOOT tap         -> next map");
    gfx->setCursor(20, hy + 16); gfx->print("BOOT 2x tap      -> prev map");
    gfx->setCursor(20, hy + 32); gfx->print("BOOT hold 0.5 s  -> open map");
    gfx->setCursor(20, hy + 48); gfx->print("BOOT hold 1.5 s  -> launcher");
}

// =============================================================================
//  Open / close map
// =============================================================================
static void open_map(int idx) {
    if (g_map_count == 0) return;
    if (idx < 0) idx = 0;
    if (idx >= g_map_count) idx = g_map_count - 1;
    g_active_map = g_maps[idx];
    load_active_meta();
    scan_zoom_range_for_active();
    load_me();

    // Choose starting centre + zoom
    if (g_meta_has_centre) {
        g_centre_lat = g_meta_centre_lat;
        g_centre_lon = g_meta_centre_lon;
    } else if (g_have_me) {
        g_centre_lat = g_my_lat;
        g_centre_lon = g_my_lon;
    } else {
        g_centre_lat = 0; g_centre_lon = 0;
    }
    int z = g_meta_default_zoom;
    if (z < g_min_zoom || z > g_max_zoom) z = (g_min_zoom + g_max_zoom) / 2;
    g_zoom = z;
    g_pan_dx = g_pan_dy = 0;

    // Persist last-opened
    g_prefs.begin("mapos", false);
    g_prefs.putString("last", g_active_map);
    g_prefs.end();

    g_mode = MODE_MAP;
    g_need_redraw = true;
    gfx->fillScreen(0x0000);
}

static void close_map() {
    g_active_map = "";
    g_mode = MODE_LIBRARY;
    g_lib_redraw = true;
    gfx->fillScreen(0x0000);
}

// =============================================================================
//  Actions
// =============================================================================
static void action_zoom_in() {
    if (g_mode != MODE_MAP) return;
    g_zoom++; if (g_zoom > g_max_zoom) g_zoom = g_min_zoom;
    g_pan_dx = g_pan_dy = 0; g_need_redraw = true;
}
static void action_zoom_out() {
    if (g_mode != MODE_MAP) return;
    g_zoom--; if (g_zoom < g_min_zoom) g_zoom = g_max_zoom;
    g_pan_dx = g_pan_dy = 0; g_need_redraw = true;
}
static void action_recentre() {
    if (g_mode != MODE_MAP) return;
    if (g_have_me) { g_centre_lat = g_my_lat; g_centre_lon = g_my_lon; }
    else if (g_meta_has_centre) { g_centre_lat = g_meta_centre_lat; g_centre_lon = g_meta_centre_lon; }
    g_pan_dx = g_pan_dy = 0; g_need_redraw = true;
}
static void action_breadcrumb() {
    if (g_mode != MODE_MAP || !g_have_me) return;
    File f = SD_MMC.open("/WATCH/MapOS/breadcrumbs.csv", FILE_APPEND);
    if (f) {
        time_t t; time(&t);
        char b[80]; snprintf(b,80,"%lu,%s,%.6f,%.6f\n",
                              (unsigned long)t, g_active_map.c_str(),
                              g_my_lat, g_my_lon);
        f.print(b); f.close();
    }
    gfx->fillRect(LCD_WIDTH-120, 8, 110, 22, 0x0000);
    gfx->drawRect(LCD_WIDTH-120, 8, 110, 22, 0xFFB300);
    gfx->setTextColor(0xFFB300); gfx->setTextSize(1);
    gfx->setCursor(LCD_WIDTH-112, 16); gfx->print("breadcrumb");
}

static void lib_next() { if (g_map_count) { g_map_sel = (g_map_sel + 1) % g_map_count; g_lib_redraw = true; } }
static void lib_prev() { if (g_map_count) { g_map_sel = (g_map_sel - 1 + g_map_count) % g_map_count; g_lib_redraw = true; } }

// =============================================================================
//  setup / loop
// =============================================================================
void setup() {
    launcher_app_boot();
    splash_show();
    SD_MMC.mkdir("/WATCH");
    SD_MMC.mkdir("/WATCH/bin");
    SD_MMC.mkdir("/WATCH/wifi");
    SD_MMC.mkdir("/WATCH/MapOS");
    SD_MMC.mkdir("/WATCH/MapOS/tiles");
    scan_maps();

    // Preselect last-opened map if present
    g_prefs.begin("mapos", true);
    String last = g_prefs.getString("last", "");
    g_prefs.end();
    if (last.length()) {
        for (int i = 0; i < g_map_count; i++) {
            if (g_maps[i] == last) { g_map_sel = i; break; }
        }
    }
    g_mode = MODE_LIBRARY;
    g_lib_redraw = true;
}

// BOOT button state machine
static uint32_t g_btn_down_t = 0;
static uint32_t g_last_release_t = 0;
static bool     g_btn_long_fired = false;

// Touch drag tracking (map mode only)
static bool     g_dragging = false;
static int32_t  g_drag_last_x = 0, g_drag_last_y = 0;
static uint32_t g_touch_press_t = 0;
static bool     g_breadcrumb_fired = false;

void loop() {
    lv_timer_handler();
    uint32_t now = millis();

    // ----- BOOT BUTTON -----
    bool btn = (digitalRead(BOOT_BTN_PIN) == LOW);
    if (btn) {
        if (!g_btn_down_t) {
            g_btn_down_t = now;
            g_btn_long_fired = false;
        } else {
            uint32_t held = now - g_btn_down_t;
            if (!g_btn_long_fired && held > 1500) {
                g_btn_long_fired = true;
                if (g_mode == MODE_MAP) close_map();
                else return_to_launcher();
            }
        }
    } else if (g_btn_down_t) {
        uint32_t held = now - g_btn_down_t;
        g_btn_down_t = 0;
        if (g_btn_long_fired) {
            // already acted
        } else if (held >= 400 && held <= 1200) {
            if (g_mode == MODE_LIBRARY) {
                // medium press in library = OPEN
                open_map(g_map_sel);
            } else {
                action_recentre();
            }
            g_last_release_t = 0;
        } else if (held < 400) {
            if (g_last_release_t && (now - g_last_release_t) < 500) {
                // double tap
                if (g_mode == MODE_LIBRARY) lib_prev();
                else                        action_zoom_out();
                g_last_release_t = 0;
            } else {
                g_last_release_t = now;
            }
        }
    }
    if (g_last_release_t && (now - g_last_release_t) > 500) {
        // single tap fired (no second tap arrived)
        if (g_mode == MODE_LIBRARY) lib_next();
        else                        action_zoom_in();
        g_last_release_t = 0;
    }

    // ----- TOUCH (map mode only -- library is BOOT-only) -----
    if (g_mode == MODE_MAP) {
        if (g_tch_pressed) {
            if (!g_dragging) {
                g_dragging = true;
                g_drag_last_x = g_tch_x; g_drag_last_y = g_tch_y;
                g_touch_press_t = now;
                g_breadcrumb_fired = false;
            } else {
                int dx = g_tch_x - g_drag_last_x;
                int dy = g_tch_y - g_drag_last_y;
                int moved = abs(dx) + abs(dy);
                if (moved > 2) {
                    g_pan_dx += dx; g_pan_dy += dy;
                    g_drag_last_x = g_tch_x; g_drag_last_y = g_tch_y;
                    g_need_redraw = true;
                    g_breadcrumb_fired = true;
                } else if (!g_breadcrumb_fired && (now - g_touch_press_t) > 900) {
                    action_breadcrumb();
                    g_breadcrumb_fired = true;
                }
            }
        } else {
            g_dragging = false;
        }

        // Recentre when pan exceeds tile bounds
        if (abs(g_pan_dx) > TILE_SZ/2 || abs(g_pan_dy) > TILE_SZ/2) {
            double tx_f, ty_f;
            lonlat_to_tile_xy(g_centre_lat, g_centre_lon, g_zoom, &tx_f, &ty_f);
            double n = (double)(1 << g_zoom);
            tx_f -= (double)g_pan_dx / TILE_SZ;
            ty_f -= (double)g_pan_dy / TILE_SZ;
            g_centre_lon = tx_f / n * 360.0 - 180.0;
            double yr = M_PI * (1 - 2 * ty_f / n);
            g_centre_lat = 180.0 / M_PI * atan(sinh(yr));
            g_pan_dx = g_pan_dy = 0;
            g_need_redraw = true;
        }

        redraw_map();
    } else {
        redraw_library();
    }
    delay(5);
}
