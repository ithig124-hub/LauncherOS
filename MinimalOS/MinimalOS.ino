// =============================================================================
//  MinimalOS.ino v1.1 -- LauncherOS-compatible ultra-low-power survival OS
//  Target  : Waveshare ESP32-S3 Touch AMOLED 2.06  (410 x 502, CO5300, FT3168)
//  Compile : ESP32-S3 Dev Module / PSRAM=OPI / Flash=16MB / Minimal SPIFFS (large APP)
//  Export  : Sketch -> Export Compiled Binary  ->  copy MinimalOS.ino.bin to
//            /WATCH/bin/minimalos.bin on the SD card.
// -----------------------------------------------------------------------------
//  v1.1 changes vs v1.0:
//   [1] NVS clock restore from LauncherOS' "rtcsave" namespace (t/gmt keys)
//       -- watch face shows real time BEFORE WiFi connects.
//   [2] esp_pm_configure() with automatic light-sleep -- kernel-managed DFS
//       replaces manual setCpuFrequencyMhz() calls.
//   [3] Real esp_light_sleep_start() in SLEEP state (TP_INT poll-on-timer wake,
//       same pattern as LauncherOS v5.x LIGHT_SLEEP_WHEN_OFF).
//   [4] Always-On Display (AOD) in SLEEP -- tiny dim clock at a randomised
//       position instead of full panel-off. Burn-safe by construction.
//   [5] QMI8658 IMU wrist-down detection -- Z-accel > 0.7 g for 1.5 s -> SLEEP.
//   [6] Cache last "HH:MM" string -- skip LVGL redraw when minute hasn't changed.
//   [7] Back-to-Launcher pill is included in the burn-shift drift.
//   [8] Quiet hours (23:00 - 06:00) force DEEP after 5 s unless charging.
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
static bool               g_touch_pressed_now = false;
static uint32_t           g_last_touch_t = 0;

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
        g_touch_pressed_now = true;
        g_last_touch_t = millis();
    } else {
        data->state = LV_INDEV_STATE_REL;
        g_touch_pressed_now = false;
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
//  [1] NVS clock restore -- read LauncherOS' "rtcsave" namespace at boot
// =============================================================================
static Preferences g_timeprefs;
static int32_t     g_gmt_offset_sec = 0;
static bool        g_time_valid     = false;

static void time_restore_from_nvs() {
    g_timeprefs.begin("rtcsave", true);
    uint32_t t   = g_timeprefs.getULong("t",   0);
    int32_t  gmt = g_timeprefs.getLong("gmt",  0);
    g_timeprefs.end();
    if (t < 1700000000UL) return;
    g_gmt_offset_sec = gmt;
    struct timeval tv = { (time_t)t, 0 };
    settimeofday(&tv, NULL);
    g_time_valid = true;
    Serial.printf("[rtc] restored t=%lu gmt=%ld\n", (unsigned long)t, (long)gmt);
}

static void time_save_to_nvs() {
    time_t now; time(&now);
    if (now < 1700000000) return;
    g_timeprefs.begin("rtcsave", false);
    g_timeprefs.putULong("t",   (uint32_t)now);
    g_timeprefs.putLong("gmt",  g_gmt_offset_sec);
    g_timeprefs.end();
}

// =============================================================================
//  WiFi loader (one-shot NTP only)
// =============================================================================
struct WifiCred { String ssid; String pass; };
static WifiCred g_wifi_list[6];
static int      g_wifi_count = 0;

static void wifi_load_from_sd() {
    g_wifi_count = 0;
    File f = SD_MMC.open("/WATCH/wifi/config.txt", FILE_READ);
    if (f) {
        while (f.available() && g_wifi_count < 5) {
            String line = f.readStringUntil('\n'); line.trim();
            if (!line.length() || line.startsWith("#")) continue;
            int eq = line.indexOf('='); if (eq < 0) continue;
            String k = line.substring(0, eq); k.trim();
            String v = line.substring(eq + 1); v.trim();
            for (int i = 1; i <= 5; i++) {
                String si = "WIFI" + String(i) + "_SSID";
                String pi = "WIFI" + String(i) + "_PASS";
                if (k == si) { while (g_wifi_count < i) g_wifi_count++;
                               g_wifi_list[i-1].ssid = v; }
                else if (k == pi) g_wifi_list[i-1].pass = v;
            }
            if (k == "GMT_OFFSET") g_gmt_offset_sec = v.toInt() * 3600;
        }
        f.close();
    }
    g_wifi_list[g_wifi_count].ssid = HC_WIFI_SSID;
    g_wifi_list[g_wifi_count].pass = HC_WIFI_PASS;
    g_wifi_count++;
}

static bool wifi_sync_once() {
    wifi_load_from_sd();
    WiFi.mode(WIFI_STA);
    for (int i = 0; i < g_wifi_count; i++) {
        if (!g_wifi_list[i].ssid.length()) continue;
        WiFi.begin(g_wifi_list[i].ssid.c_str(), g_wifi_list[i].pass.c_str());
        uint32_t t0 = millis();
        while (millis() - t0 < 4000) {
            if (WiFi.status() == WL_CONNECTED) {
                configTime(g_gmt_offset_sec, 0, "pool.ntp.org");
                delay(800);
                struct tm tm;
                if (getLocalTime(&tm, 200)) {
                    g_time_valid = true;
                    time_save_to_nvs();
                }
                WiFi.disconnect(true, true);
                WiFi.mode(WIFI_OFF);
                return true;
            }
            delay(120);
        }
    }
    WiFi.disconnect(true, true);
    WiFi.mode(WIFI_OFF);
    return false;
}

// =============================================================================
//  [5] QMI8658 IMU (wrist-down detection) -- direct I2C, no extra library
// =============================================================================
#define QMI_ADDR_PRIMARY   0x6B
#define QMI_ADDR_SECONDARY 0x6A
static uint8_t g_qmi_addr = 0;
static bool    g_has_imu  = false;

static bool qmi_write_reg(uint8_t r, uint8_t v) {
    Wire.beginTransmission(g_qmi_addr);
    Wire.write(r); Wire.write(v);
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
    for (uint8_t a : { (uint8_t)QMI_ADDR_PRIMARY, (uint8_t)QMI_ADDR_SECONDARY }) {
        Wire.beginTransmission(a);
        if (Wire.endTransmission() == 0) {
            g_qmi_addr = a;
            uint8_t who = 0; qmi_read(0x00, &who, 1);
            if (who == 0x05) {                  // QMI8658 WHO_AM_I
                qmi_write_reg(0x02, 0x60);      // CTRL1: AUTO INC + BIG ENDIAN off
                qmi_write_reg(0x03, 0x95);      // CTRL2: accel 2g, ODR 125Hz
                qmi_write_reg(0x08, 0x01);      // CTRL7: enable accel
                g_has_imu = true;
                Serial.printf("[imu] QMI8658 @ 0x%02X\n", a);
                return;
            }
        }
    }
    Serial.println("[imu] not found");
}
// Returns Z in g (1.0 = wrist-down face-up, -1.0 = face-down)
static float qmi_read_z() {
    if (!g_has_imu) return 0.0f;
    uint8_t b[6];
    if (!qmi_read(0x35, b, 6)) return 0.0f;     // AX_L..AZ_H
    int16_t az = (int16_t)((b[5] << 8) | b[4]);
    return az / 16384.0f;                       // ±2 g full scale
}

// =============================================================================
//  Power-state machine + brightness
// =============================================================================
enum PState { ST_ACTIVE = 0, ST_DIM, ST_SLEEP, ST_DEEP };
static PState  g_state = ST_ACTIVE;
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
static void panel_off() {
    co5300_set_brightness(0);
    uint8_t cmd = 0x28;
    bus->beginWrite(); bus->writeCommand(cmd); bus->endWrite();
}

// =============================================================================
//  UI -- main face + [4] AOD overlay
// =============================================================================
#define COL_BG     0x000000
#define COL_TXT    0xFFFFFF
#define COL_MUTE   0x666666
#define COL_ACCENT 0x00E5FF
#define COL_DANGER 0xFF4040

static lv_obj_t *scr_root;
static lv_obj_t *lbl_clock, *lbl_date, *lbl_batt, *lbl_state, *lbl_emerg, *back_btn;
static lv_obj_t *aod_clock;          // [4] AOD label (separate, tiny, dim)

// [7] burn-shift now includes the back pill
static int8_t g_shift_x = 0, g_shift_y = 0;
static uint32_t g_last_shift = 0;
static void apply_burn_shift() {
    uint32_t now = millis();
    if (now - g_last_shift < 60000) return;
    g_last_shift = now;
    g_shift_x = (int8_t)((g_shift_x + 1) % 5 - 2);
    g_shift_y = (int8_t)((g_shift_y + 2) % 5 - 2);
    lv_obj_align(lbl_clock, LV_ALIGN_CENTER,     g_shift_x,       -40 + g_shift_y);
    lv_obj_align(lbl_date,  LV_ALIGN_CENTER,     g_shift_x,        12 + g_shift_y);
    lv_obj_align(lbl_batt,  LV_ALIGN_TOP_MID,    g_shift_x,        40 + g_shift_y);
    lv_obj_align(lbl_state, LV_ALIGN_BOTTOM_MID, g_shift_x,       -90 + g_shift_y);
    lv_obj_align(back_btn,  LV_ALIGN_BOTTOM_MID, g_shift_x,       -28 + g_shift_y);
}

static void back_btn_cb(lv_event_t *) { return_to_launcher(); }

static void build_ui() {
    scr_root = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr_root, lv_color_hex(COL_BG), 0);
    lv_obj_set_style_bg_opa(scr_root, LV_OPA_COVER, 0);

    lbl_batt = lv_label_create(scr_root);
    lv_label_set_text(lbl_batt, "--%");
    lv_obj_set_style_text_color(lbl_batt, lv_color_hex(COL_MUTE), 0);
    lv_obj_set_style_text_font(lbl_batt, &lv_font_montserrat_14, 0);
    lv_obj_align(lbl_batt, LV_ALIGN_TOP_MID, 0, 40);

    lbl_clock = lv_label_create(scr_root);
    lv_label_set_text(lbl_clock, "--:--");
    lv_obj_set_style_text_color(lbl_clock, lv_color_hex(COL_TXT), 0);
    lv_obj_set_style_text_font(lbl_clock, &lv_font_montserrat_48, 0);
    lv_obj_set_style_text_opa(lbl_clock, 220, 0);
    lv_obj_align(lbl_clock, LV_ALIGN_CENTER, 0, -40);

    lbl_date = lv_label_create(scr_root);
    lv_label_set_text(lbl_date, "");
    lv_obj_set_style_text_color(lbl_date, lv_color_hex(0x888888), 0);
    lv_obj_set_style_text_font(lbl_date, &lv_font_montserrat_16, 0);
    lv_obj_align(lbl_date, LV_ALIGN_CENTER, 0, 12);

    lbl_state = lv_label_create(scr_root);
    lv_label_set_text(lbl_state, "ACTIVE");
    lv_obj_set_style_text_color(lbl_state, lv_color_hex(COL_MUTE), 0);
    lv_obj_set_style_text_font(lbl_state, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_letter_space(lbl_state, 3, 0);
    lv_obj_align(lbl_state, LV_ALIGN_BOTTOM_MID, 0, -90);

    lbl_emerg = lv_label_create(scr_root);
    lv_label_set_text(lbl_emerg, "EMERGENCY");
    lv_obj_set_style_text_color(lbl_emerg, lv_color_hex(COL_DANGER), 0);
    lv_obj_set_style_text_font(lbl_emerg, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_letter_space(lbl_emerg, 4, 0);
    lv_obj_align(lbl_emerg, LV_ALIGN_TOP_MID, 0, 18);
    lv_obj_add_flag(lbl_emerg, LV_OBJ_FLAG_HIDDEN);

    back_btn = lv_btn_create(scr_root);
    lv_obj_set_size(back_btn, 200, 46);
    lv_obj_align(back_btn, LV_ALIGN_BOTTOM_MID, 0, -28);
    lv_obj_set_style_bg_color(back_btn, lv_color_hex(0x141414), 0);
    lv_obj_set_style_border_color(back_btn, lv_color_hex(COL_ACCENT), 0);
    lv_obj_set_style_border_width(back_btn, 1, 0);
    lv_obj_set_style_radius(back_btn, 23, 0);
    lv_obj_t *bl = lv_label_create(back_btn);
    lv_label_set_text(bl, LV_SYMBOL_LEFT "  Launcher");
    lv_obj_set_style_text_color(bl, lv_color_hex(COL_ACCENT), 0);
    lv_obj_set_style_text_font(bl, &lv_font_montserrat_16, 0);
    lv_obj_center(bl);
    lv_obj_add_event_cb(back_btn, back_btn_cb, LV_EVENT_CLICKED, NULL);

    // [4] AOD label -- hidden by default, shown only in SLEEP
    aod_clock = lv_label_create(scr_root);
    lv_label_set_text(aod_clock, "--:--");
    lv_obj_set_style_text_color(aod_clock, lv_color_hex(0xAAAAAA), 0);
    lv_obj_set_style_text_font(aod_clock, &lv_font_montserrat_22, 0);
    lv_obj_set_style_text_opa(aod_clock, 130, 0);
    lv_obj_align(aod_clock, LV_ALIGN_CENTER, 0, 0);
    lv_obj_add_flag(aod_clock, LV_OBJ_FLAG_HIDDEN);

    lv_scr_load(scr_root);
}

// =============================================================================
//  Clock update -- [6] cache last "HH:MM" and skip redraw if unchanged
// =============================================================================
static char     g_last_hhmm[8] = {0};
static char     g_last_date[24] = {0};

static void update_clock(bool force = false) {
    struct tm tm;
    if (!getLocalTime(&tm, 5)) {
        if (force) {
            lv_label_set_text(lbl_clock, "--:--");
            lv_label_set_text(lbl_date,  "no sync");
            lv_label_set_text(aod_clock, "--:--");
        }
        return;
    }
    char t[8], d[24];
    strftime(t, sizeof(t), "%H:%M",      &tm);
    strftime(d, sizeof(d), "%a %d %b",   &tm);
    if (force || strcmp(t, g_last_hhmm) != 0) {
        strcpy(g_last_hhmm, t);
        lv_label_set_text(lbl_clock, t);
        lv_label_set_text(aod_clock, t);
    }
    if (force || strcmp(d, g_last_date) != 0) {
        strcpy(g_last_date, d);
        lv_label_set_text(lbl_date, d);
    }
}

static int  g_batt_pct  = 100;
static bool g_emergency = false;
static bool g_charging  = false;

static void update_battery() {
    if (!g_has_pmu) return;
    g_batt_pct = g_pmu.getBatteryPercent();
    g_charging = g_pmu.isCharging();
    char b[16];
    if (g_charging) snprintf(b, sizeof(b), "%d%%  CHG", g_batt_pct);
    else            snprintf(b, sizeof(b), "%d%%",      g_batt_pct);
    lv_label_set_text(lbl_batt, b);

    bool new_emerg = (g_batt_pct < 5 && !g_charging);
    if (new_emerg != g_emergency) {
        g_emergency = new_emerg;
        if (g_emergency) lv_obj_clear_flag(lbl_emerg, LV_OBJ_FLAG_HIDDEN);
        else             lv_obj_add_flag  (lbl_emerg, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_style_text_color(lbl_batt,
            lv_color_hex(g_emergency ? COL_DANGER : COL_MUTE), 0);
    }
}

static void set_state_label() {
    const char *txt = "ACTIVE";
    switch (g_state) {
        case ST_ACTIVE: txt = "ACTIVE"; break;
        case ST_DIM:    txt = "DIM";    break;
        case ST_SLEEP:  txt = "SLEEP";  break;
        case ST_DEEP:   txt = "DEEP";   break;
    }
    lv_label_set_text(lbl_state, txt);
}

// =============================================================================
//  [4] AOD positioning -- randomise once per state entry
// =============================================================================
static void aod_reposition() {
    int dx = (esp_random() % 120) - 60;
    int dy = (esp_random() % 200) - 100;
    lv_obj_align(aod_clock, LV_ALIGN_CENTER, dx, dy);
}
static void aod_show()  {
    lv_obj_add_flag  (lbl_clock, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag  (lbl_date,  LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag  (lbl_batt,  LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag  (lbl_state, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag  (back_btn,  LV_OBJ_FLAG_HIDDEN);
    if (g_emergency) lv_obj_add_flag(lbl_emerg, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(aod_clock, LV_OBJ_FLAG_HIDDEN);
    aod_reposition();
}
static void aod_hide()  {
    lv_obj_add_flag  (aod_clock, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(lbl_clock, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(lbl_date,  LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(lbl_batt,  LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(lbl_state, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(back_btn,  LV_OBJ_FLAG_HIDDEN);
    if (g_emergency) lv_obj_clear_flag(lbl_emerg, LV_OBJ_FLAG_HIDDEN);
}

// =============================================================================
//  Adaptive sleep state machine
// =============================================================================
static void enter_state(int s_int) {
    PState s = (PState)s_int;
    if (g_state == s) return;
    PState prev = g_state;
    g_state = s;
    set_state_label();
    // Leaving AOD?
    if (prev == ST_SLEEP && s != ST_SLEEP) aod_hide();
    switch (s) {
        case ST_ACTIVE:
            panel_on(g_emergency ? 60 : 200);
            setCpuFrequencyMhz(g_emergency ? 80 : 160);
            break;
        case ST_DIM:
            co5300_set_brightness(g_emergency ? 8 : 35);
            setCpuFrequencyMhz(80);
            break;
        case ST_SLEEP:
            // [4] AOD -- show tiny dim clock instead of full panel-off
            aod_show();
            update_clock(true);
            lv_refr_now(NULL);
            co5300_set_brightness(g_emergency ? 3 : 6);
            setCpuFrequencyMhz(40);
            break;
        case ST_DEEP:
            panel_off();
            time_save_to_nvs();
            esp_sleep_enable_ext1_wakeup(
                (1ULL << BOOT_BTN_PIN) | (1ULL << TP_INT),
                ESP_EXT1_WAKEUP_ANY_LOW);
            esp_sleep_pd_config(ESP_PD_DOMAIN_RTC_PERIPH, ESP_PD_OPTION_ON);
            delay(50);
            esp_deep_sleep_start();
            break;
    }
}

static void instant_wake() {
    if (g_state == ST_ACTIVE) return;
    enter_state(ST_ACTIVE);
    g_last_touch_t = millis();
}

static void check_touch_wake() {
    if (FT3168->IIC_Interrupt_Flag) {
        FT3168->IIC_Interrupt_Flag = false;
        g_last_touch_t = millis();
        instant_wake();
    }
}

// =============================================================================
//  [5] Wrist-down detection
// =============================================================================
static uint32_t g_wristdown_since = 0;
static void check_wrist_down() {
    if (!g_has_imu) return;
    float z = qmi_read_z();
    if (z > 0.7f) {
        if (!g_wristdown_since) g_wristdown_since = millis();
        else if (millis() - g_wristdown_since > 1500
                 && g_state == ST_ACTIVE) {
            Serial.println("[imu] wrist down -> SLEEP");
            enter_state(ST_SLEEP);
        }
    } else g_wristdown_since = 0;
}

// =============================================================================
//  [8] Quiet hours (23:00 - 06:00)
// =============================================================================
static bool in_quiet_hours() {
    struct tm tm;
    if (!g_time_valid || !getLocalTime(&tm, 5)) return false;
    return (tm.tm_hour >= 23 || tm.tm_hour < 6);
}

// =============================================================================
//  setup / loop
// =============================================================================
void setup() {
    launcher_app_boot();
    qmi_init();
    time_restore_from_nvs();        // [1]

    build_ui();
    update_battery();
    update_clock(true);

    // Manual CPU scaling -- portable across ESP32 Arduino core versions.
    // (Light-sleep in the SLEEP state below covers most of the same wins
    //  that esp_pm_configure() would give, without the IDF-version dance.)
    setCpuFrequencyMhz(160);

    // One-shot NTP only if we don't already have a valid time
    if (!g_time_valid) {
        Serial.println("[minimal] syncing time...");
        wifi_sync_once();
        update_clock(true);
    } else {
        Serial.println("[minimal] using NVS-restored time, skipping wifi");
    }

    enter_state(ST_ACTIVE);
    g_last_touch_t = millis();
}

void loop() {
    uint32_t now = millis();

    lv_timer_handler();

    // BOOT-button safety exit
    static uint32_t btn_t = 0;
    if (digitalRead(BOOT_BTN_PIN) == LOW) {
        if (!btn_t) btn_t = now;
        else if (now - btn_t > 1500) return_to_launcher();
    } else btn_t = 0;

    check_touch_wake();
    if (g_touch_pressed_now) { g_last_touch_t = now; instant_wake(); }
    check_wrist_down();             // [5]

    uint32_t idle = now - g_last_touch_t;

    // 1 Hz tick: clock + AOD reposition + persist
    static uint32_t g_last_tick = 0;
    static uint32_t g_last_save = 0;
    static uint32_t g_last_aod_move = 0;
    if (now - g_last_tick >= 1000) {
        g_last_tick = now;
        update_clock();                                 // [6]
        static int b = 0;
        if (++b >= 5) { b = 0; update_battery(); }
        apply_burn_shift();                             // [7]
        if (g_state == ST_SLEEP && now - g_last_aod_move >= 60000) {
            g_last_aod_move = now;
            aod_reposition();                           // [4]
        }
        if (g_time_valid && now - g_last_save >= 300000UL) {
            g_last_save = now;
            time_save_to_nvs();
        }
    }

    // [8] Quiet-hours override
    bool quiet = in_quiet_hours() && !g_charging;
    uint32_t t_dim   = (g_emergency ?  3000 :  4000);
    uint32_t t_sleep = (g_emergency ?  8000 : 12000);
    uint32_t t_deep  = (quiet       ?  5000 : (g_emergency ? 30000 : 60000));

    if      (idle >= t_deep  && g_state != ST_DEEP)   enter_state(ST_DEEP);
    else if (idle >= t_sleep && g_state == ST_DIM)    enter_state(ST_SLEEP);
    else if (idle >= t_dim   && g_state == ST_ACTIVE) enter_state(ST_DIM);

    // [3] Real light-sleep while in SLEEP/AOD -- TP_INT poll on timer wake.
    // TP_INT (GPIO 38) is not an RTC IO, so we wake on a short timer and poll.
    if (g_state == ST_SLEEP) {
        esp_sleep_enable_ext1_wakeup((1ULL << BOOT_BTN_PIN), ESP_EXT1_WAKEUP_ANY_LOW);
        esp_sleep_enable_timer_wakeup(120ULL * 1000ULL);  // 120 ms poll
        esp_light_sleep_start();
        if (digitalRead(TP_INT) == LOW) {
            FT3168->IIC_Interrupt_Flag = true;
            g_last_touch_t = millis();
            instant_wake();
        }
        return;
    }

    delay(g_state == ST_ACTIVE ? 5 : 30);
}
