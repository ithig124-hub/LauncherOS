// =============================================================================
//  DevOS.ino  --  LauncherOS-compatible engineering console
//  Target  : Waveshare ESP32-S3 Touch AMOLED 2.06  (410 x 502, CO5300, FT3168)
//  Compile : ESP32-S3 Dev Module / PSRAM=OPI / Flash=16MB / Minimal SPIFFS (large APP)
//  Export  : Sketch -> Export Compiled Binary  ->  copy DevOS.ino.bin to
//            /WATCH/bin/devos.bin on the SD card.
// =============================================================================
//  Dev OS = the engineering layer of the LauncherOS ecosystem:
//    - Live performance dashboard (FPS, heap, PSRAM, CPU, WiFi RSSI, battery)
//    - Sensor inspector (touch coords, PMU voltage/current, temperature)
//    - Terminal / serial-log mirror (in-RAM ring buffer, on-screen)
//    - Partition & firmware info (running slot, OTA slot, sketch size, MAC)
//    - WiFi panel (hardcoded fallback + /WATCH/wifi/config.txt loader, same
//      flow as LauncherOS, then radio shut off when idle)
//  All five panels are swipeable (left/right) and share the launcher palette.
// =============================================================================

// ===== BEGIN SHARED BOOT BLOCK (LauncherOS v6.1+ compatible) =================
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
#include <esp_partition.h>
#include <esp_chip_info.h>
#include <rom/rtc.h>

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

// ---- Hardcoded WiFi fallback (mirrors LauncherOS HC_WIFI_*) -----------------
// Same fallback the launcher uses if no /WATCH/wifi/config.txt is present.
#define HC_WIFI_SSID "WatchNet"
#define HC_WIFI_PASS "watch1234"

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

// ---- LVGL plumbing ----------------------------------------------------------
static lv_disp_draw_buf_t draw_buf;
static lv_color_t        *lvgl_buf = nullptr;
static lv_disp_drv_t      disp_drv;
static lv_indev_drv_t     indev_drv;
static int32_t            g_last_touch_x = -1, g_last_touch_y = -1;
static bool               g_touch_pressed = false;

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
        data->state   = LV_INDEV_STATE_PR;
        data->point.x = x; data->point.y = y;
        g_last_touch_x = x; g_last_touch_y = y; g_touch_pressed = true;
    } else {
        data->state = LV_INDEV_STATE_REL;
        g_touch_pressed = false;
    }
}

// ---- AXP2101 PMU ------------------------------------------------------------
XPowersPMU g_pmu;
static bool g_has_pmu = false;
static void pmu_init() {
    if (g_pmu.begin(Wire, AXP2101_SLAVE_ADDRESS, IIC_SDA, IIC_SCL)) {
        g_has_pmu = true;
        g_pmu.disableTSPinMeasure();
        g_pmu.enableBattVoltageMeasure();
    }
}

// ---- Back-to-launcher (mandatory) -------------------------------------------
static void return_to_launcher() {
    const esp_partition_t *other = esp_ota_get_next_update_partition(NULL);
    if (other) esp_ota_set_boot_partition(other);
    delay(80);
    esp_restart();
}

// ---- Boot bring-up ----------------------------------------------------------
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
    size_t bufPx = LCD_WIDTH * 60;
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

static void launcher_app_loop_tick() {
    lv_timer_handler();
    static uint32_t btn_t = 0;
    if (digitalRead(BOOT_BTN_PIN) == LOW) {
        if (!btn_t) btn_t = millis();
        else if (millis() - btn_t > 1500) return_to_launcher();
    } else btn_t = 0;
}
// ===== END SHARED BOOT BLOCK =================================================

// =============================================================================
//  WiFi loader -- mirrors LauncherOS' /WATCH/wifi/config.txt + HC fallback flow
// =============================================================================
struct WifiCred { String ssid; String pass; };
static WifiCred  g_wifi_list[6];
static int       g_wifi_count = 0;
static int       g_gmt_offset = 0;

static void wifi_load_from_sd() {
    g_wifi_count = 0;
    File f = SD_MMC.open("/WATCH/wifi/config.txt", FILE_READ);
    if (!f) { Serial.println("[wifi] no /WATCH/wifi/config.txt"); return; }
    while (f.available() && g_wifi_count < 5) {
        String line = f.readStringUntil('\n');
        line.trim();
        if (line.length() == 0 || line.startsWith("#")) continue;
        int eq = line.indexOf('=');
        if (eq < 0) continue;
        String k = line.substring(0, eq); k.trim();
        String v = line.substring(eq + 1); v.trim();
        for (int i = 1; i <= 5; i++) {
            String si = "WIFI" + String(i) + "_SSID";
            String pi = "WIFI" + String(i) + "_PASS";
            if (k == si) { while (g_wifi_count < i) g_wifi_count++;
                           g_wifi_list[i-1].ssid = v; }
            else if (k == pi) { g_wifi_list[i-1].pass = v; }
        }
        if (k == "GMT_OFFSET") g_gmt_offset = v.toInt();
    }
    f.close();
    // Always append the hardcoded fallback at the end
    g_wifi_list[g_wifi_count].ssid = HC_WIFI_SSID;
    g_wifi_list[g_wifi_count].pass = HC_WIFI_PASS;
    g_wifi_count++;
    Serial.printf("[wifi] loaded %d networks (incl. fallback)\n", g_wifi_count);
}

static bool g_wifi_connected = false;
static String g_wifi_ssid_now = "";
static int    g_wifi_rssi     = 0;

static bool wifi_try_connect(uint32_t per_net_ms = 4000) {
    for (int i = 0; i < g_wifi_count; i++) {
        if (g_wifi_list[i].ssid.length() == 0) continue;
        Serial.printf("[wifi] trying '%s'\n", g_wifi_list[i].ssid.c_str());
        WiFi.mode(WIFI_STA);
        WiFi.begin(g_wifi_list[i].ssid.c_str(), g_wifi_list[i].pass.c_str());
        uint32_t t0 = millis();
        while (millis() - t0 < per_net_ms) {
            if (WiFi.status() == WL_CONNECTED) {
                g_wifi_connected = true;
                g_wifi_ssid_now = g_wifi_list[i].ssid;
                g_wifi_rssi     = WiFi.RSSI();
                Serial.printf("[wifi] connected, RSSI %d\n", g_wifi_rssi);
                return true;
            }
            delay(120);
        }
    }
    g_wifi_connected = false;
    WiFi.disconnect(true, true);
    WiFi.mode(WIFI_OFF);
    return false;
}

// =============================================================================
//  Telemetry ring buffer  (used by terminal screen) + SD log file [v1.1 #1]
// =============================================================================
#define LOG_LINES 80
static String  g_log[LOG_LINES];
static int     g_log_head = 0;
static bool    g_sd_log_ok = false;

static void sd_log_open() {
    SD_MMC.mkdir("/WATCH/logs");
    File f = SD_MMC.open("/WATCH/logs/devos.log", FILE_APPEND);
    if (f) { f.printf("\n=== devos boot %lu ===\n", (unsigned long)millis()); f.close(); g_sd_log_ok = true; }
}
static void log_line(const String &s) {
    g_log[g_log_head] = s;
    g_log_head = (g_log_head + 1) % LOG_LINES;
    Serial.println(s);
    if (g_sd_log_ok) {
        File f = SD_MMC.open("/WATCH/logs/devos.log", FILE_APPEND);
        if (f) { f.print(millis()); f.print(" "); f.println(s); f.close(); }
    }
}

// ---- Reset / crash info  [v1.1 #2] ------------------------------------------
static const char *reset_reason_str(int r) {
    switch (r) {
        case 1:  return "POWERON";
        case 3:  return "SW_RESTART";
        case 4:  return "TASK_WDT";
        case 5:  return "OTHER_WDT";
        case 6:  return "DEEP_SLEEP";
        case 7:  return "BROWNOUT";
        case 8:  return "SDIO";
        case 12: return "PANIC";
        default: return "UNKNOWN";
    }
}

// ---- QMI8658 IMU (for shake-to-reboot) [v1.1 #14] ---------------------------
static uint8_t g_qmi_addr = 0;
static bool    g_has_imu  = false;
static bool qmi_write_reg(uint8_t r, uint8_t v) {
    Wire.beginTransmission(g_qmi_addr); Wire.write(r); Wire.write(v);
    return Wire.endTransmission() == 0;
}
static bool qmi_read(uint8_t r, uint8_t *out, size_t n) {
    Wire.beginTransmission(g_qmi_addr); Wire.write(r);
    if (Wire.endTransmission(false) != 0) return false;
    Wire.requestFrom((int)g_qmi_addr, (int)n);
    for (size_t i = 0; i < n && Wire.available(); i++) out[i] = Wire.read();
    return true;
}
static void qmi_init() {
    for (uint8_t a : { (uint8_t)0x6B, (uint8_t)0x6A }) {
        Wire.beginTransmission(a);
        if (Wire.endTransmission() == 0) {
            g_qmi_addr = a;
            uint8_t who = 0; qmi_read(0x00, &who, 1);
            if (who == 0x05) {
                qmi_write_reg(0x02, 0x60);
                qmi_write_reg(0x03, 0x95);
                qmi_write_reg(0x08, 0x01);
                g_has_imu = true;
                return;
            }
        }
    }
}
static void qmi_xyz_g(float *x, float *y, float *z) {
    *x = *y = *z = 0;
    if (!g_has_imu) return;
    uint8_t b[6];
    if (!qmi_read(0x35, b, 6)) return;
    int16_t ax = (int16_t)((b[1] << 8) | b[0]);
    int16_t ay = (int16_t)((b[3] << 8) | b[2]);
    int16_t az = (int16_t)((b[5] << 8) | b[4]);
    *x = ax / 16384.0f; *y = ay / 16384.0f; *z = az / 16384.0f;
}
// triple-shake: |a|>2g three times within 800 ms
static int      g_shake_hits = 0;
static uint32_t g_shake_first = 0;
static void check_shake_reboot() {
    if (!g_has_imu) return;
    float x, y, z; qmi_xyz_g(&x, &y, &z);
    float mag = sqrtf(x*x + y*y + z*z);
    uint32_t now = millis();
    if (mag > 2.0f) {
        if (!g_shake_hits || now - g_shake_first > 800) {
            g_shake_hits = 1; g_shake_first = now;
        } else if (now - g_shake_first > 80) {
            g_shake_hits++;
            if (g_shake_hits >= 3) {
                log_line("[shake] triple-shake -> reboot");
                delay(100);
                esp_restart();
            }
            g_shake_first = now;
        }
    }
}

// =============================================================================
//  UI -- 5 swipeable screens (carousel) sharing launcher palette
// =============================================================================
#define COL_BG       0x000000
#define COL_CARD     0x0E0E0E
#define COL_BORDER   0x1C1C1C
#define COL_ACCENT   0x00E5FF   // cyber cyan
#define COL_OK       0x00FF80
#define COL_WARN     0xFFB300
#define COL_DANGER   0xFF4040
#define COL_TXT      0xFFFFFF
#define COL_MUTE     0x666666
#define COL_EYE      0x888888

enum { SCR_PERF = 0, SCR_SENSOR, SCR_PARTS, SCR_WIFI, SCR_TERM, SCR_COUNT };
static lv_obj_t *g_scr[SCR_COUNT] = {0};
static int       g_cur = 0;
static bool      g_nav_locked = false;
static uint32_t  g_last_nav = 0;
#define NAV_COOLDOWN_MS 200

// ---- shared header (eyebrow dot + label + page indicator + back pill) -------
static void build_header(lv_obj_t *parent, const char *label, uint32_t dot_col) {
    lv_obj_t *dot = lv_obj_create(parent);
    lv_obj_remove_style_all(dot);
    lv_obj_set_size(dot, 8, 8);
    lv_obj_set_style_radius(dot, 4, 0);
    lv_obj_set_style_bg_color(dot, lv_color_hex(dot_col), 0);
    lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
    lv_obj_align(dot, LV_ALIGN_TOP_LEFT, 24, 26);

    lv_obj_t *lbl = lv_label_create(parent);
    lv_label_set_text(lbl, label);
    lv_obj_set_style_text_color(lbl, lv_color_hex(COL_EYE), 0);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_letter_space(lbl, 3, 0);
    lv_obj_align(lbl, LV_ALIGN_TOP_LEFT, 38, 22);
}

static void back_btn_cb(lv_event_t *) { return_to_launcher(); }

static void build_back_pill(lv_obj_t *parent) {
    lv_obj_t *back = lv_btn_create(parent);
    lv_obj_set_size(back, 220, 46);
    lv_obj_align(back, LV_ALIGN_BOTTOM_MID, 0, -22);
    lv_obj_set_style_bg_color(back, lv_color_hex(0x141414), 0);
    lv_obj_set_style_border_color(back, lv_color_hex(COL_ACCENT), 0);
    lv_obj_set_style_border_width(back, 1, 0);
    lv_obj_set_style_radius(back, 23, 0);
    lv_obj_t *bl = lv_label_create(back);
    lv_label_set_text(bl, LV_SYMBOL_LEFT "  Back to Launcher");
    lv_obj_set_style_text_color(bl, lv_color_hex(COL_ACCENT), 0);
    lv_obj_set_style_text_font(bl, &lv_font_montserrat_16, 0);
    lv_obj_center(bl);
    lv_obj_add_event_cb(back, back_btn_cb, LV_EVENT_CLICKED, NULL);
}

// ---- 1) Performance dashboard ----------------------------------------------
static lv_obj_t *lbl_fps, *lbl_heap, *lbl_psram, *lbl_cpu, *lbl_rssi, *lbl_batt, *lbl_reset;
static lv_obj_t *bar_heap_fill, *bar_psram_fill;
#define GRAPH_W 360
#define GRAPH_H 60
static lv_obj_t *graph_canvas;
static lv_color_t graph_buf[GRAPH_W * GRAPH_H];
static uint16_t   fps_hist[GRAPH_W] = {0};
static int        fps_idx = 0;

static lv_obj_t *make_metric_card(lv_obj_t *parent, const char *eyebrow,
                                   int x, int y, int w, int h, lv_obj_t **out_val) {
    lv_obj_t *c = lv_obj_create(parent);
    lv_obj_remove_style_all(c);
    lv_obj_set_size(c, w, h);
    lv_obj_set_pos(c, x, y);
    lv_obj_set_style_bg_color(c, lv_color_hex(COL_CARD), 0);
    lv_obj_set_style_bg_opa(c, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(c, 14, 0);
    lv_obj_set_style_border_color(c, lv_color_hex(COL_BORDER), 0);
    lv_obj_set_style_border_width(c, 1, 0);
    lv_obj_set_style_pad_all(c, 10, 0);

    lv_obj_t *eb = lv_label_create(c);
    lv_label_set_text(eb, eyebrow);
    lv_obj_set_style_text_color(eb, lv_color_hex(COL_EYE), 0);
    lv_obj_set_style_text_font(eb, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_letter_space(eb, 2, 0);
    lv_obj_align(eb, LV_ALIGN_TOP_LEFT, 4, 2);

    lv_obj_t *v = lv_label_create(c);
    lv_label_set_text(v, "--");
    lv_obj_set_style_text_color(v, lv_color_hex(COL_TXT), 0);
    lv_obj_set_style_text_font(v, &lv_font_montserrat_22, 0);
    lv_obj_align(v, LV_ALIGN_BOTTOM_LEFT, 4, -2);
    *out_val = v;
    return c;
}

static void build_perf_screen() {
    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr, lv_color_hex(COL_BG), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    g_scr[SCR_PERF] = scr;

    build_header(scr, "DEV OS  /  PERFORMANCE", COL_ACCENT);

    // 2x3 grid of metric cards
    make_metric_card(scr, "FPS",      24,  60, 170, 70, &lbl_fps);
    make_metric_card(scr, "RSSI",    216,  60, 170, 70, &lbl_rssi);
    make_metric_card(scr, "CPU MHZ",  24, 140, 170, 70, &lbl_cpu);
    make_metric_card(scr, "BATT",    216, 140, 170, 70, &lbl_batt);

    // Heap bar
    lv_obj_t *heap_card = lv_obj_create(scr);
    lv_obj_remove_style_all(heap_card);
    lv_obj_set_size(heap_card, 362, 56);
    lv_obj_set_pos(heap_card, 24, 220);
    lv_obj_set_style_bg_color(heap_card, lv_color_hex(COL_CARD), 0);
    lv_obj_set_style_bg_opa(heap_card, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(heap_card, 14, 0);
    lv_obj_set_style_border_color(heap_card, lv_color_hex(COL_BORDER), 0);
    lv_obj_set_style_border_width(heap_card, 1, 0);
    lv_obj_set_style_pad_all(heap_card, 10, 0);

    lv_obj_t *heap_eb = lv_label_create(heap_card);
    lv_label_set_text(heap_eb, "INTERNAL HEAP");
    lv_obj_set_style_text_color(heap_eb, lv_color_hex(COL_EYE), 0);
    lv_obj_set_style_text_font(heap_eb, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_letter_space(heap_eb, 2, 0);
    lv_obj_align(heap_eb, LV_ALIGN_TOP_LEFT, 4, 2);

    lv_obj_t *heap_track = lv_obj_create(heap_card);
    lv_obj_remove_style_all(heap_track);
    lv_obj_set_size(heap_track, 340, 10);
    lv_obj_align(heap_track, LV_ALIGN_BOTTOM_LEFT, 0, -4);
    lv_obj_set_style_bg_color(heap_track, lv_color_hex(0x111111), 0);
    lv_obj_set_style_bg_opa(heap_track, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(heap_track, 5, 0);

    bar_heap_fill = lv_obj_create(heap_track);
    lv_obj_remove_style_all(bar_heap_fill);
    lv_obj_set_size(bar_heap_fill, 0, 10);
    lv_obj_align(bar_heap_fill, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_set_style_radius(bar_heap_fill, 5, 0);
    lv_obj_set_style_bg_color(bar_heap_fill, lv_color_hex(COL_ACCENT), 0);
    lv_obj_set_style_bg_opa(bar_heap_fill, LV_OPA_COVER, 0);

    lbl_heap = lv_label_create(heap_card);
    lv_label_set_text(lbl_heap, "--/-- kB");
    lv_obj_set_style_text_color(lbl_heap, lv_color_hex(COL_TXT), 0);
    lv_obj_set_style_text_font(lbl_heap, &lv_font_montserrat_14, 0);
    lv_obj_align(lbl_heap, LV_ALIGN_TOP_RIGHT, -4, 2);

    // PSRAM bar (same construction)
    lv_obj_t *ps_card = lv_obj_create(scr);
    lv_obj_remove_style_all(ps_card);
    lv_obj_set_size(ps_card, 362, 56);
    lv_obj_set_pos(ps_card, 24, 286);
    lv_obj_set_style_bg_color(ps_card, lv_color_hex(COL_CARD), 0);
    lv_obj_set_style_bg_opa(ps_card, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(ps_card, 14, 0);
    lv_obj_set_style_border_color(ps_card, lv_color_hex(COL_BORDER), 0);
    lv_obj_set_style_border_width(ps_card, 1, 0);
    lv_obj_set_style_pad_all(ps_card, 10, 0);

    lv_obj_t *ps_eb = lv_label_create(ps_card);
    lv_label_set_text(ps_eb, "PSRAM");
    lv_obj_set_style_text_color(ps_eb, lv_color_hex(COL_EYE), 0);
    lv_obj_set_style_text_font(ps_eb, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_letter_space(ps_eb, 2, 0);
    lv_obj_align(ps_eb, LV_ALIGN_TOP_LEFT, 4, 2);

    lv_obj_t *ps_track = lv_obj_create(ps_card);
    lv_obj_remove_style_all(ps_track);
    lv_obj_set_size(ps_track, 340, 10);
    lv_obj_align(ps_track, LV_ALIGN_BOTTOM_LEFT, 0, -4);
    lv_obj_set_style_bg_color(ps_track, lv_color_hex(0x111111), 0);
    lv_obj_set_style_bg_opa(ps_track, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(ps_track, 5, 0);

    bar_psram_fill = lv_obj_create(ps_track);
    lv_obj_remove_style_all(bar_psram_fill);
    lv_obj_set_size(bar_psram_fill, 0, 10);
    lv_obj_align(bar_psram_fill, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_set_style_radius(bar_psram_fill, 5, 0);
    lv_obj_set_style_bg_color(bar_psram_fill, lv_color_hex(COL_OK), 0);
    lv_obj_set_style_bg_opa(bar_psram_fill, LV_OPA_COVER, 0);

    lbl_psram = lv_label_create(ps_card);
    lv_label_set_text(lbl_psram, "--/-- kB");
    lv_obj_set_style_text_color(lbl_psram, lv_color_hex(COL_TXT), 0);
    lv_obj_set_style_text_font(lbl_psram, &lv_font_montserrat_14, 0);
    lv_obj_align(lbl_psram, LV_ALIGN_TOP_RIGHT, -4, 2);

    // Reset-reason card  [v1.1 #2]
    make_metric_card(scr, "LAST RESET", 24, 352, 362, 50, &lbl_reset);
    {
        int r0 = (int)rtc_get_reset_reason(0);
        int r1 = (int)rtc_get_reset_reason(1);
        char buf[64];
        snprintf(buf, sizeof(buf), "c0:%s  c1:%s",
                 reset_reason_str(r0), reset_reason_str(r1));
        lv_label_set_text(lbl_reset, buf);
        lv_obj_set_style_text_font(lbl_reset, &lv_font_montserrat_14, 0);
    }

    // FPS wireframe graph (canvas) -- moved up to fit reset card
    graph_canvas = lv_canvas_create(scr);
    lv_canvas_set_buffer(graph_canvas, graph_buf, GRAPH_W, GRAPH_H, LV_IMG_CF_TRUE_COLOR);
    lv_obj_set_pos(graph_canvas, 25, 410);
    lv_canvas_fill_bg(graph_canvas, lv_color_hex(0x050505), LV_OPA_COVER);

    build_back_pill(scr);
}

static uint32_t g_frame_count = 0, g_last_fps_t = 0;
static uint16_t g_fps = 0;

static void redraw_fps_graph() {
    lv_canvas_fill_bg(graph_canvas, lv_color_hex(0x050505), LV_OPA_COVER);
    // grid
    lv_draw_rect_dsc_t grid; lv_draw_rect_dsc_init(&grid);
    grid.bg_color = lv_color_hex(0x111111); grid.bg_opa = LV_OPA_COVER;
    for (int gy = 15; gy < GRAPH_H; gy += 15) {
        lv_canvas_draw_rect(graph_canvas, 0, gy, GRAPH_W, 1, &grid);
    }
    // trace
    lv_draw_rect_dsc_t pt; lv_draw_rect_dsc_init(&pt);
    pt.bg_color = lv_color_hex(COL_ACCENT); pt.bg_opa = LV_OPA_COVER;
    for (int x = 0; x < GRAPH_W; x++) {
        int v = fps_hist[(fps_idx + x) % GRAPH_W];
        if (v > 60) v = 60;
        int h = (v * GRAPH_H) / 60;
        if (h < 1) h = 1;
        lv_canvas_draw_rect(graph_canvas, x, GRAPH_H - h, 1, h, &pt);
    }
}

static void update_perf() {
    uint32_t now = millis();
    g_frame_count++;
    if (now - g_last_fps_t >= 500) {
        g_fps = (g_frame_count * 1000) / (now - g_last_fps_t);
        g_frame_count = 0;
        g_last_fps_t = now;
        fps_hist[fps_idx] = g_fps;
        fps_idx = (fps_idx + 1) % GRAPH_W;
        if (g_scr[SCR_PERF] == lv_scr_act()) redraw_fps_graph();
    }
    if (g_scr[SCR_PERF] != lv_scr_act()) return;

    char buf[40];
    snprintf(buf, sizeof(buf), "%u", g_fps);
    lv_label_set_text(lbl_fps, buf);
    snprintf(buf, sizeof(buf), "%u", (unsigned)getCpuFrequencyMhz());
    lv_label_set_text(lbl_cpu, buf);

    if (g_has_pmu) {
        int pct = g_pmu.getBatteryPercent();
        snprintf(buf, sizeof(buf), "%d%%", pct);
    } else snprintf(buf, sizeof(buf), "n/a");
    lv_label_set_text(lbl_batt, buf);

    if (g_wifi_connected) snprintf(buf, sizeof(buf), "%d dBm", (int)WiFi.RSSI());
    else                  snprintf(buf, sizeof(buf), "off");
    lv_label_set_text(lbl_rssi, buf);

    size_t hf = ESP.getFreeHeap(), ht = ESP.getHeapSize();
    snprintf(buf, sizeof(buf), "%u / %u kB", (unsigned)((ht - hf)/1024), (unsigned)(ht/1024));
    lv_label_set_text(lbl_heap, buf);
    int pct = ht ? (int)(((ht - hf) * 340) / ht) : 0;
    lv_obj_set_size(bar_heap_fill, pct, 10);

    size_t pf = ESP.getFreePsram(), pt2 = ESP.getPsramSize();
    if (pt2) {
        snprintf(buf, sizeof(buf), "%u / %u kB", (unsigned)((pt2 - pf)/1024), (unsigned)(pt2/1024));
        lv_label_set_text(lbl_psram, buf);
        int p = (int)(((pt2 - pf) * 340) / pt2);
        lv_obj_set_size(bar_psram_fill, p, 10);
    } else {
        lv_label_set_text(lbl_psram, "absent");
    }
}

// ---- 2) Sensor inspector ----------------------------------------------------
static lv_obj_t *lbl_touch_xy, *lbl_touch_state, *lbl_vbat, *lbl_vbus, *lbl_chg, *lbl_temp;
static lv_obj_t *touch_dot;
static lv_obj_t *btn_i2c_scan;

// I2C bus scanner  [v1.1 #10] -- dumps to terminal log
static void i2c_scan_cb(lv_event_t *) {
    log_line("[i2c] scanning bus...");
    int found = 0;
    for (uint8_t a = 0x08; a < 0x78; a++) {
        Wire.beginTransmission(a);
        if (Wire.endTransmission() == 0) {
            const char *who = "?";
            if (a == 0x34) who = "AXP2101";
            else if (a == 0x38) who = "FT3168";
            else if (a == 0x6A || a == 0x6B) who = "QMI8658";
            else if (a == 0x51) who = "PCF85063 RTC";
            char b[40]; snprintf(b, sizeof(b), "[i2c] 0x%02X  %s", a, who);
            log_line(b);
            found++;
        }
    }
    char done[32]; snprintf(done, sizeof(done), "[i2c] %d device(s)", found);
    log_line(done);
}

static void build_sensor_screen() {
    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr, lv_color_hex(COL_BG), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    g_scr[SCR_SENSOR] = scr;

    build_header(scr, "DEV OS  /  SENSORS", COL_OK);

    // Touch pad visualization
    lv_obj_t *pad = lv_obj_create(scr);
    lv_obj_remove_style_all(pad);
    lv_obj_set_size(pad, 200, 200);
    lv_obj_align(pad, LV_ALIGN_TOP_MID, 0, 60);
    lv_obj_set_style_bg_color(pad, lv_color_hex(COL_CARD), 0);
    lv_obj_set_style_bg_opa(pad, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(pad, 14, 0);
    lv_obj_set_style_border_color(pad, lv_color_hex(COL_ACCENT), 0);
    lv_obj_set_style_border_width(pad, 1, 0);

    touch_dot = lv_obj_create(pad);
    lv_obj_remove_style_all(touch_dot);
    lv_obj_set_size(touch_dot, 14, 14);
    lv_obj_set_style_radius(touch_dot, 7, 0);
    lv_obj_set_style_bg_color(touch_dot, lv_color_hex(COL_ACCENT), 0);
    lv_obj_set_style_bg_opa(touch_dot, LV_OPA_COVER, 0);
    lv_obj_set_pos(touch_dot, 93, 93);

    lv_obj_t *xy_card; make_metric_card(scr, "TOUCH X,Y", 24, 268, 170, 60, &lbl_touch_xy);
    lv_obj_t *st_card; make_metric_card(scr, "STATE",    216, 268, 170, 60, &lbl_touch_state);
    lv_obj_t *vb_card; make_metric_card(scr, "VBAT mV",   24, 332, 170, 60, &lbl_vbat);
    lv_obj_t *vs_card; make_metric_card(scr, "VBUS mV",  216, 332, 170, 60, &lbl_vbus);
    (void)xy_card; (void)st_card; (void)vb_card; (void)vs_card;

    // I2C scan button  [v1.1 #10]
    btn_i2c_scan = lv_btn_create(scr);
    lv_obj_set_size(btn_i2c_scan, 362, 44);
    lv_obj_align(btn_i2c_scan, LV_ALIGN_TOP_MID, 0, 398);
    lv_obj_set_style_bg_color(btn_i2c_scan, lv_color_hex(0x141414), 0);
    lv_obj_set_style_border_color(btn_i2c_scan, lv_color_hex(COL_OK), 0);
    lv_obj_set_style_border_width(btn_i2c_scan, 1, 0);
    lv_obj_set_style_radius(btn_i2c_scan, 22, 0);
    lv_obj_t *bil = lv_label_create(btn_i2c_scan);
    lv_label_set_text(bil, LV_SYMBOL_REFRESH "  Scan I2C bus");
    lv_obj_set_style_text_color(bil, lv_color_hex(COL_OK), 0);
    lv_obj_set_style_text_font(bil, &lv_font_montserrat_16, 0);
    lv_obj_center(bil);
    lv_obj_add_event_cb(btn_i2c_scan, i2c_scan_cb, LV_EVENT_CLICKED, NULL);

    build_back_pill(scr);
}

static void update_sensor() {
    if (g_scr[SCR_SENSOR] != lv_scr_act()) return;
    char buf[40];
    if (g_touch_pressed) {
        snprintf(buf, sizeof(buf), "%ld , %ld", (long)g_last_touch_x, (long)g_last_touch_y);
        lv_label_set_text(lbl_touch_state, "PRESSED");
        lv_obj_set_style_text_color(lbl_touch_state, lv_color_hex(COL_ACCENT), 0);
        int px = (g_last_touch_x * 186) / LCD_WIDTH;
        int py = (g_last_touch_y * 186) / LCD_HEIGHT;
        lv_obj_set_pos(touch_dot, px, py);
    } else {
        snprintf(buf, sizeof(buf), "-- , --");
        lv_label_set_text(lbl_touch_state, "REL");
        lv_obj_set_style_text_color(lbl_touch_state, lv_color_hex(COL_MUTE), 0);
    }
    lv_label_set_text(lbl_touch_xy, buf);

    if (g_has_pmu) {
        snprintf(buf, sizeof(buf), "%u", (unsigned)g_pmu.getBattVoltage());
        lv_label_set_text(lbl_vbat, buf);
        snprintf(buf, sizeof(buf), "%u", (unsigned)g_pmu.getVbusVoltage());
        lv_label_set_text(lbl_vbus, buf);
    } else {
        lv_label_set_text(lbl_vbat, "n/a");
        lv_label_set_text(lbl_vbus, "n/a");
    }
}

// ---- 3) Partition / firmware info ------------------------------------------
static lv_obj_t *lbl_running, *lbl_other, *lbl_size, *lbl_mac, *lbl_chip, *lbl_sketch;

static void build_parts_screen() {
    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr, lv_color_hex(COL_BG), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    g_scr[SCR_PARTS] = scr;

    build_header(scr, "DEV OS  /  PARTITIONS", COL_WARN);

    make_metric_card(scr, "RUNNING SLOT", 24,  60, 362, 56, &lbl_running);
    make_metric_card(scr, "OTHER SLOT",   24, 124, 362, 56, &lbl_other);
    make_metric_card(scr, "SKETCH SIZE",  24, 188, 362, 56, &lbl_sketch);
    make_metric_card(scr, "CHIP",         24, 252, 170, 56, &lbl_chip);
    make_metric_card(scr, "FLASH MB",    216, 252, 170, 56, &lbl_size);
    make_metric_card(scr, "MAC",          24, 316, 362, 56, &lbl_mac);

    build_back_pill(scr);
}

static void update_parts_once() {
    if (!lbl_running) return;
    const esp_partition_t *r = esp_ota_get_running_partition();
    const esp_partition_t *o = esp_ota_get_next_update_partition(NULL);
    char b[64];
    if (r) { snprintf(b, sizeof(b), "%s @ 0x%lx", r->label, (long)r->address);
             lv_label_set_text(lbl_running, b); }
    if (o) { snprintf(b, sizeof(b), "%s @ 0x%lx", o->label, (long)o->address);
             lv_label_set_text(lbl_other, b); }
    snprintf(b, sizeof(b), "%u kB", (unsigned)(ESP.getSketchSize() / 1024));
    lv_label_set_text(lbl_sketch, b);
    esp_chip_info_t ci; esp_chip_info(&ci);
    snprintf(b, sizeof(b), "ESP32-S3 r%d", ci.revision);
    lv_label_set_text(lbl_chip, b);
    snprintf(b, sizeof(b), "%u", (unsigned)(ESP.getFlashChipSize() / (1024 * 1024)));
    lv_label_set_text(lbl_size, b);
    uint64_t mac = ESP.getEfuseMac();
    snprintf(b, sizeof(b), "%02X:%02X:%02X:%02X:%02X:%02X",
        (uint8_t)(mac >> 40), (uint8_t)(mac >> 32), (uint8_t)(mac >> 24),
        (uint8_t)(mac >> 16), (uint8_t)(mac >> 8),  (uint8_t) mac);
    lv_label_set_text(lbl_mac, b);
}

// ---- 4) WiFi screen ---------------------------------------------------------
static lv_obj_t *lbl_wifi_state, *lbl_wifi_ssid, *lbl_wifi_rssi, *lbl_wifi_count, *btn_wifi_scan;
static bool g_wifi_busy = false;

static void wifi_action_cb(lv_event_t *) {
    if (g_wifi_busy) return;
    g_wifi_busy = true;
    log_line("[wifi] scan/connect requested");
    lv_label_set_text(lbl_wifi_state, "CONNECTING...");
    lv_obj_set_style_text_color(lbl_wifi_state, lv_color_hex(COL_WARN), 0);
    lv_refr_now(NULL);
    wifi_load_from_sd();
    bool ok = wifi_try_connect();
    if (ok) {
        lv_label_set_text(lbl_wifi_state, "CONNECTED");
        lv_obj_set_style_text_color(lbl_wifi_state, lv_color_hex(COL_OK), 0);
        lv_label_set_text(lbl_wifi_ssid, g_wifi_ssid_now.c_str());
        char b[24]; snprintf(b, sizeof(b), "%d dBm", g_wifi_rssi);
        lv_label_set_text(lbl_wifi_rssi, b);
        // Sync NTP then shut radio off like LauncherOS does
        configTime(g_gmt_offset * 3600, 0, "pool.ntp.org");
        delay(800);
        WiFi.disconnect(true, true);
        WiFi.mode(WIFI_OFF);
        log_line("[wifi] sync done, radio off");
    } else {
        lv_label_set_text(lbl_wifi_state, "FAILED");
        lv_obj_set_style_text_color(lbl_wifi_state, lv_color_hex(COL_DANGER), 0);
        log_line("[wifi] all networks failed");
    }
    char b[16]; snprintf(b, sizeof(b), "%d", g_wifi_count);
    lv_label_set_text(lbl_wifi_count, b);
    g_wifi_busy = false;
}

static void build_wifi_screen() {
    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr, lv_color_hex(COL_BG), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    g_scr[SCR_WIFI] = scr;

    build_header(scr, "DEV OS  /  WIFI", COL_ACCENT);

    make_metric_card(scr, "STATE",       24,  60, 362, 60, &lbl_wifi_state);
    make_metric_card(scr, "SSID",        24, 128, 362, 60, &lbl_wifi_ssid);
    make_metric_card(scr, "RSSI",        24, 196, 170, 60, &lbl_wifi_rssi);
    make_metric_card(scr, "NET COUNT",  216, 196, 170, 60, &lbl_wifi_count);
    lv_label_set_text(lbl_wifi_state, "IDLE");
    lv_label_set_text(lbl_wifi_ssid,  "--");
    lv_label_set_text(lbl_wifi_rssi,  "--");
    lv_label_set_text(lbl_wifi_count, "--");

    btn_wifi_scan = lv_btn_create(scr);
    lv_obj_set_size(btn_wifi_scan, 280, 54);
    lv_obj_align(btn_wifi_scan, LV_ALIGN_TOP_MID, 0, 280);
    lv_obj_set_style_bg_color(btn_wifi_scan, lv_color_hex(0x141414), 0);
    lv_obj_set_style_border_color(btn_wifi_scan, lv_color_hex(COL_ACCENT), 0);
    lv_obj_set_style_border_width(btn_wifi_scan, 1, 0);
    lv_obj_set_style_radius(btn_wifi_scan, 26, 0);
    lv_obj_t *bt = lv_label_create(btn_wifi_scan);
    lv_label_set_text(bt, LV_SYMBOL_WIFI "  Load config & connect");
    lv_obj_set_style_text_color(bt, lv_color_hex(COL_ACCENT), 0);
    lv_obj_set_style_text_font(bt, &lv_font_montserrat_16, 0);
    lv_obj_center(bt);
    lv_obj_add_event_cb(btn_wifi_scan, wifi_action_cb, LV_EVENT_CLICKED, NULL);

    build_back_pill(scr);
}

// ---- 5) Terminal screen -----------------------------------------------------
static lv_obj_t *term_ta;

static void build_term_screen() {
    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr, lv_color_hex(COL_BG), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    g_scr[SCR_TERM] = scr;

    build_header(scr, "DEV OS  /  TERMINAL", COL_DANGER);

    term_ta = lv_textarea_create(scr);
    lv_obj_set_size(term_ta, 362, 320);
    lv_obj_align(term_ta, LV_ALIGN_TOP_MID, 0, 60);
    lv_obj_set_style_bg_color(term_ta, lv_color_hex(COL_CARD), 0);
    lv_obj_set_style_bg_opa(term_ta, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(term_ta, lv_color_hex(COL_BORDER), 0);
    lv_obj_set_style_border_width(term_ta, 1, 0);
    lv_obj_set_style_radius(term_ta, 12, 0);
    lv_obj_set_style_text_color(term_ta, lv_color_hex(COL_OK), 0);
    lv_obj_set_style_text_font(term_ta, &lv_font_montserrat_12, 0);
    lv_textarea_set_text(term_ta, "[devos] terminal ready\n");

    build_back_pill(scr);
}

static int g_log_last_drawn = 0;
static uint32_t g_last_task_dump = 0;
static void update_term() {
    if (g_scr[SCR_TERM] != lv_scr_act()) return;
    while (g_log_last_drawn != g_log_head) {
        const String &s = g_log[g_log_last_drawn];
        if (s.length()) {
            lv_textarea_add_text(term_ta, s.c_str());
            lv_textarea_add_char(term_ta, '\n');
        }
        g_log_last_drawn = (g_log_last_drawn + 1) % LOG_LINES;
    }
    // Task list dump every 3 s  [v1.1 #8]
    uint32_t now = millis();
    if (now - g_last_task_dump > 3000) {
        g_last_task_dump = now;
        UBaseType_t n = uxTaskGetNumberOfTasks();
        TaskStatus_t *arr = (TaskStatus_t *)pvPortMalloc(n * sizeof(TaskStatus_t));
        if (arr) {
            n = uxTaskGetSystemState(arr, n, NULL);
            char hdr[64];
            snprintf(hdr, sizeof(hdr), "--- %u tasks ---", (unsigned)n);
            log_line(hdr);
            for (UBaseType_t i = 0; i < n; i++) {
                char ln[80];
                snprintf(ln, sizeof(ln), "  %-14s pri=%2u stk=%5u",
                         arr[i].pcTaskName, (unsigned)arr[i].uxCurrentPriority,
                         (unsigned)arr[i].usStackHighWaterMark);
                log_line(ln);
            }
            vPortFree(arr);
        }
    }
}

// =============================================================================
//  Carousel navigation (swipe left/right with cooldown + lock)
// =============================================================================
static int wrap_screen(int i) {
    while (i < 0)         i += SCR_COUNT;
    while (i >= SCR_COUNT) i -= SCR_COUNT;
    return i;
}

static void go_to(int target, bool from_right) {
    target = wrap_screen(target);
    if (target == g_cur || g_nav_locked) return;
    g_nav_locked = true;
    g_last_nav = millis();
    g_cur = target;
    lv_scr_load_anim(g_scr[target],
        from_right ? LV_SCR_LOAD_ANIM_MOVE_LEFT : LV_SCR_LOAD_ANIM_MOVE_RIGHT,
        220, 0, false);
}

// Manual swipe detection layered on top of LVGL touch readings
static int32_t  swipe_start_x = -1, swipe_start_y = -1;
static uint32_t swipe_start_t = 0;
static bool     swipe_active = false;

static void poll_swipes() {
    if (g_nav_locked && millis() - g_last_nav > 240) g_nav_locked = false;
    if (g_nav_locked) return;
    if (g_touch_pressed) {
        if (!swipe_active) {
            swipe_active  = true;
            swipe_start_x = g_last_touch_x;
            swipe_start_y = g_last_touch_y;
            swipe_start_t = millis();
        }
    } else if (swipe_active) {
        swipe_active = false;
        int32_t dx = g_last_touch_x - swipe_start_x;
        int32_t dy = g_last_touch_y - swipe_start_y;
        uint32_t dt = millis() - swipe_start_t;
        if (dt < 600 && abs(dx) > 60 && abs(dx) > abs(dy) * 2) {
            if (dx < 0) go_to(g_cur + 1, true);
            else        go_to(g_cur - 1, false);
        }
    }
}

// =============================================================================
//  setup / loop
// =============================================================================
void setup() {
    launcher_app_boot();
    sd_log_open();              // [v1.1 #1] SD logfile
    qmi_init();                 // [v1.1 #14] IMU for shake-to-reboot
    log_line("[devos] boot v1.1");
    log_line(String("[devos] running on core ") + xPortGetCoreID());
    {
        char b[64];
        snprintf(b, sizeof(b), "[reset] c0=%s c1=%s",
            reset_reason_str((int)rtc_get_reset_reason(0)),
            reset_reason_str((int)rtc_get_reset_reason(1)));
        log_line(b);
    }
    log_line(g_has_imu ? "[imu] QMI8658 OK  (triple-shake to reboot)"
                       : "[imu] not detected");

    build_perf_screen();
    build_sensor_screen();
    build_parts_screen();
    build_wifi_screen();
    build_term_screen();
    update_parts_once();

    lv_scr_load(g_scr[SCR_PERF]);
    log_line("[devos] ui ready - swipe L/R between 5 panels");
}

void loop() {
    launcher_app_loop_tick();

    static uint32_t t_perf = 0, t_sens = 0, t_term = 0;
    uint32_t now = millis();
    if (now - t_perf >= 250) { t_perf = now; update_perf();   }
    if (now - t_sens >=  80) { t_sens = now; update_sensor(); }
    if (now - t_term >= 150) { t_term = now; update_term();   }

    poll_swipes();
    check_shake_reboot();          // [v1.1 #14]
    g_frame_count++;   // counted by update_perf() for the FPS metric
    delay(2);
}
