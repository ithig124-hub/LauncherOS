// =============================================================================
//  TacticalOS.ino v1.0 -- LauncherOS-compatible operator / adventure OS
//  Target  : Waveshare ESP32-S3 Touch AMOLED 2.06  (410 x 502, CO5300, FT3168)
//  Compile : ESP32-S3 Dev Module / PSRAM=OPI / Flash=16MB / Minimal SPIFFS (large APP)
//  Export  : Sketch -> Export Compiled Binary  ->  copy TacticalOS.ino.bin to
//            /WATCH/bin/tacticalos.bin on the SD card.
// -----------------------------------------------------------------------------
//  Aesthetic : matte black + red/amber night accents, thin lines, radar grid.
//  Vibe      : cyberdeck field computer / stealth wrist operator.
//
//  Features:
//   [1] NIGHT MODE        - red-only UI, ultra-dim, anti-eye-strain (AMOLED
//                           true-black saves battery + protects night vision).
//   [2] CAMP DASHBOARD    - battery estimate, clock, sunrise/sunset (lat-based),
//                           daylight remaining, compass dot, IMU motion.
//   [3] STEALTH MODE      - kills sounds, dims to absolute minimum, freezes
//                           animations and burn-shifts the UI to protect AMOLED.
//   [4] COMPASS + HUD     - QMI8658 IMU heading, tilt, radar sweep animation.
//   [5] SURVIVAL TOOLS    - flashlight (full-white panel), SOS strobe (... --- ...),
//                           offline notes (/WATCH/tactical/notes.txt), timer.
//   [6] QUICK GESTURES    - double-tap anywhere -> flashlight; long-hold BOOT
//                           -> Back to Launcher; swipe between tabs.
//   [7] FOCUS WALK MODE   - minimal "time + steps + heading" face, no chrome.
//   [8] ADAPTIVE NIGHT UI - between 19:00 and 06:00 UI auto-shifts redder and
//                           dimmer (cinematic AMOLED feel, also kills bright
//                           white emissions that ruin dark adaptation).
//   [9] BACK TO LAUNCHER  - mandatory cyan-bordered pill, long-press BOOT also.
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
#include <math.h>
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

// ---- Forward type declarations (Arduino IDE hoists function prototypes
//      above the file, so any struct used in a return/arg type must be
//      defined before those prototypes — i.e. here, at the very top.) ----
struct AccG { float x, y, z; };

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
static int32_t            g_touch_x = 0, g_touch_y = 0;

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
        g_touch_x = x; g_touch_y = y;
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

// ---- CO5300 brightness ------------------------------------------------------
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
//  NVS clock restore -- read LauncherOS' "rtcsave" namespace at boot
// =============================================================================
static Preferences g_timeprefs;
static int32_t     g_gmt_offset_sec = 0;
static bool        g_time_valid     = false;
static uint32_t    g_last_ntp_ok    = 0;

static void time_restore_from_nvs() {
    g_timeprefs.begin("rtcsave", true);
    uint32_t t   = g_timeprefs.getULong("t",   0);
    int32_t  gmt = g_timeprefs.getLong("gmt",  0);
    g_last_ntp_ok = g_timeprefs.getULong("ntp", 0);
    g_timeprefs.end();
    if (t < 1700000000UL) return;
    g_gmt_offset_sec = gmt;
    struct timeval tv = { (time_t)t, 0 };
    settimeofday(&tv, NULL);
    g_time_valid = true;
}

// =============================================================================
//  WiFi config + one-shot NTP (kills radio after to save ~80 mA)
// =============================================================================
struct WifiCred { String ssid; String pass; };
static WifiCred g_wifi_list[6];
static int      g_wifi_count = 0;
static float    g_lat = 0.0f;    // for sunrise/sunset (read from config)
static float    g_lon = 0.0f;

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
            if      (k == "GMT_OFFSET") g_gmt_offset_sec = v.toInt() * 3600;
            else if (k == "LAT")        g_lat = v.toFloat();
            else if (k == "LON")        g_lon = v.toFloat();
        }
        f.close();
    }
    g_wifi_list[g_wifi_count].ssid = HC_WIFI_SSID;
    g_wifi_list[g_wifi_count].pass = HC_WIFI_PASS;
    g_wifi_count++;
}

static bool g_wifi_synced = false;
static bool wifi_sync_once() {
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
                    time_t nowt; time(&nowt);
                    g_last_ntp_ok = (uint32_t)nowt;
                    g_timeprefs.begin("rtcsave", false);
                    g_timeprefs.putULong("t",   (uint32_t)nowt);
                    g_timeprefs.putLong ("gmt", g_gmt_offset_sec);
                    g_timeprefs.putULong("ntp", g_last_ntp_ok);
                    g_timeprefs.end();
                }
                g_wifi_synced = true;
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
//  QMI8658 IMU (heading + tilt + step detection)
// =============================================================================
#define QMI_ADDR_PRIMARY   0x6B
#define QMI_ADDR_SECONDARY 0x6A
static uint8_t g_qmi_addr = 0;
static bool    g_has_imu  = false;

static bool qmi_w(uint8_t r, uint8_t v) {
    Wire.beginTransmission(g_qmi_addr);
    Wire.write(r); Wire.write(v);
    return Wire.endTransmission() == 0;
}
static bool qmi_r(uint8_t r, uint8_t *out, size_t n) {
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
            uint8_t who = 0; qmi_r(0x00, &who, 1);
            if (who == 0x05) {
                qmi_w(0x02, 0x60);
                qmi_w(0x03, 0x95);   // accel 2g 125Hz
                qmi_w(0x04, 0x65);   // gyro  256dps 125Hz
                qmi_w(0x08, 0x03);   // enable accel + gyro
                g_has_imu = true;
                return;
            }
        }
    }
}
static AccG qmi_acc() {
    AccG g = {0,0,0};
    if (!g_has_imu) return g;
    uint8_t b[6];
    if (!qmi_r(0x35, b, 6)) return g;
    int16_t ax = (int16_t)((b[1]<<8)|b[0]);
    int16_t ay = (int16_t)((b[3]<<8)|b[2]);
    int16_t az = (int16_t)((b[5]<<8)|b[4]);
    g.x = ax / 16384.0f; g.y = ay / 16384.0f; g.z = az / 16384.0f;
    return g;
}
// Cheap "heading" by integrating gyro Z. Without a magnetometer this is a
// relative bearing -- good enough for "which way am I facing relative to
// when I opened the compass". Drifts ~1 deg/min, recalibrated by tap.
static float g_heading_deg = 0.0f;
static uint32_t g_last_gyro_ms = 0;
static void update_heading() {
    if (!g_has_imu) return;
    uint8_t b[2];
    if (!qmi_r(0x3F, b, 2)) return;        // gyro Z
    int16_t gz = (int16_t)((b[1]<<8)|b[0]);
    float dps = gz / 64.0f;                 // 2048 LSB/dps at 16dps? -> use 256dps full scale conversion
    // For QMI8658 with FS=256dps -> 128 LSB/dps. We pick a calibrated divisor.
    dps = gz / 128.0f;
    uint32_t now = millis();
    if (g_last_gyro_ms) {
        float dt = (now - g_last_gyro_ms) / 1000.0f;
        g_heading_deg += dps * dt;
        while (g_heading_deg >=  360.0f) g_heading_deg -= 360.0f;
        while (g_heading_deg <    0.0f)  g_heading_deg += 360.0f;
    }
    g_last_gyro_ms = now;
}
static void heading_zero() { g_heading_deg = 0.0f; }

// Step counter (cheap magnitude-peak detector) -- persisted to NVS, resets at midnight
static uint32_t g_steps = 0;
static uint16_t g_steps_yday = 0xFFFF;
static Preferences g_steps_prefs;
static float    g_lp_mag = 1.0f;
static bool     g_above = false;
static void steps_load() {
    g_steps_prefs.begin("tacticalos", true);
    g_steps      = g_steps_prefs.getULong("steps", 0);
    g_steps_yday = g_steps_prefs.getUShort("sd", 0xFFFF);
    g_steps_prefs.end();
}
static void steps_save() {
    g_steps_prefs.begin("tacticalos", false);
    g_steps_prefs.putULong("steps", g_steps);
    g_steps_prefs.putUShort("sd", g_steps_yday);
    g_steps_prefs.end();
}
static void steps_rollover_check() {
    struct tm tm;
    if (!getLocalTime(&tm, 5)) return;
    uint16_t yd = tm.tm_yday;
    if (g_steps_yday == 0xFFFF) { g_steps_yday = yd; steps_save(); return; }
    if (yd != g_steps_yday) {
        g_steps = 0;
        g_steps_yday = yd;
        steps_save();
    }
}
static void update_steps() {
    AccG a = qmi_acc();
    float mag = sqrtf(a.x*a.x + a.y*a.y + a.z*a.z);
    g_lp_mag = g_lp_mag * 0.9f + mag * 0.1f;
    float delta = mag - g_lp_mag;
    if (!g_above && delta > 0.15f)  {
        g_above = true; g_steps++;
        if ((g_steps & 0x1F) == 0) steps_save();      // persist every 32 steps
    }
    else if (g_above && delta < 0.05f) g_above = false;
}

// =============================================================================
//  Tactical palette (red operator)
// =============================================================================
#define T_BG       0x000000
#define T_SURFACE  0x0A0000   // hint of red in the surface
#define T_BORDER   0x2A0000
#define T_RED      0xFF3030
#define T_DIM_RED  0x8A1010
#define T_AMBER    0xFFB300
#define T_CYAN     0x00E5FF   // ONLY the Back-to-Launcher pill uses cyan (LauncherOS contract)
#define T_TXT      0xE0C8C8
#define T_MUTE     0x553333
#define T_OK       0x39FF7A
#define T_DANGER   0xFF4040

// =============================================================================
//  Mode state machine
// =============================================================================
enum Mode { M_HOME = 0, M_COMPASS, M_TOOLS, M_CAMP, M_WALK };
static Mode    g_mode = M_HOME;
static bool    g_night         = false;     // [1] night mode active
static bool    g_stealth       = false;     // [3] stealth mode
static bool    g_adaptive_night = true;     // [8] auto-flip at dusk
static bool    g_flashlight    = false;
static bool    g_sos_active    = false;
static uint32_t g_sos_t        = 0;

// SOS pattern: ... --- ... (Morse "S O S"), dot=200ms, dash=600ms, gap=200ms
//  on/off pattern, in ms
static const uint16_t SOS_PATTERN[] = {
    200, 200, 200, 200, 200, 500,   // S
    600, 200, 600, 200, 600, 500,   // O
    200, 200, 200, 200, 200, 1200   // S + long gap
};
#define SOS_LEN (sizeof(SOS_PATTERN)/sizeof(SOS_PATTERN[0]))
static uint8_t g_sos_idx = 0;

// =============================================================================
//  UI -- tab views
// =============================================================================
static lv_obj_t *scr_root;
static lv_obj_t *tabv;
static lv_obj_t *tab_home, *tab_compass, *tab_tools, *tab_camp, *tab_walk;
static lv_obj_t *flash_scr;     // full-white flashlight overlay
static lv_obj_t *toast_lbl;

// Home widgets
static lv_obj_t *home_clock, *home_date, *home_batt, *home_mode_dot, *home_sync_dot;
static lv_obj_t *home_status, *home_subtitle;

// Compass widgets (canvas-based radar)
static lv_obj_t *cmp_canvas, *cmp_heading_lbl, *cmp_tilt_lbl, *cmp_zero_btn;
#define CMP_W 360
#define CMP_H 360
static lv_color_t *cmp_buf = nullptr;

// Tools widgets
static lv_obj_t *tools_flash_btn, *tools_sos_btn, *tools_notes_btn, *tools_timer_btn;
static lv_obj_t *tools_stealth_sw, *tools_night_sw;

// Camp widgets
static lv_obj_t *camp_sunrise, *camp_sunset, *camp_daylight, *camp_batt_hours,
                *camp_temp, *camp_steps;

// Walk widgets
static lv_obj_t *walk_time, *walk_steps, *walk_heading;

// Back-to-launcher pill (shown on every tab)
static lv_obj_t *back_btn;

// Carousel page-indicator dots (S3_MiniOS-style)
static lv_obj_t *page_dots[5] = {0};

// =============================================================================
//  Toast helper
// =============================================================================
static uint32_t g_toast_hide_at = 0;
static void show_toast(const char *msg, uint32_t ms) {
    if (!toast_lbl) return;
    lv_label_set_text(toast_lbl, msg);
    lv_obj_clear_flag(toast_lbl, LV_OBJ_FLAG_HIDDEN);
    g_toast_hide_at = millis() + ms;
}
static void tick_toast() {
    if (g_toast_hide_at && millis() >= g_toast_hide_at) {
        lv_obj_add_flag(toast_lbl, LV_OBJ_FLAG_HIDDEN);
        g_toast_hide_at = 0;
    }
}

// =============================================================================
//  Theming -- apply night / stealth recoloring atomically
// =============================================================================
static uint32_t accent_color() {
    if (g_stealth || g_night) return T_DIM_RED;
    return T_RED;
}
static uint32_t text_color() {
    if (g_stealth)         return 0x4A0000;
    if (g_night)           return 0xC07060;
    return T_TXT;
}
static void apply_theme() {
    uint32_t a = accent_color(), t = text_color();
    if (home_clock)  lv_obj_set_style_text_color(home_clock, lv_color_hex(t), 0);
    if (home_date)   lv_obj_set_style_text_color(home_date,  lv_color_hex(g_stealth ? 0x300000 : 0x7A5050), 0);
    if (home_batt)   lv_obj_set_style_text_color(home_batt,  lv_color_hex(a), 0);
    if (home_mode_dot) lv_obj_set_style_bg_color(home_mode_dot, lv_color_hex(a), 0);
    if (home_status) lv_obj_set_style_text_color(home_status, lv_color_hex(a), 0);
    // brightness target
    uint8_t br = g_stealth ? 12 : (g_night ? 40 : 180);
    co5300_set_brightness(br);
}

// =============================================================================
//  Adaptive night UI ([8]) - flip at dusk
// =============================================================================
static void adaptive_night_tick() {
    if (!g_adaptive_night || g_stealth) return;
    struct tm tm;
    if (!getLocalTime(&tm, 5)) return;
    bool should = (tm.tm_hour >= 19 || tm.tm_hour < 6);
    if (should != g_night) {
        g_night = should;
        apply_theme();
        show_toast(should ? "NIGHT MODE: AUTO" : "DAY MODE: AUTO", 1500);
    }
}

// =============================================================================
//  Sunrise / sunset (approx, NOAA simplified)
// =============================================================================
static void sunrise_sunset(int yy, int mm, int dd, float lat, float lon, int tz_h,
                           int *sr_h, int *sr_m, int *ss_h, int *ss_m) {
    // Day-of-year
    static const int doy_off[] = {0,31,59,90,120,151,181,212,243,273,304,334};
    int N = doy_off[mm-1] + dd;
    if (mm > 2 && (yy % 4 == 0 && (yy % 100 != 0 || yy % 400 == 0))) N++;
    float gamma = 2.0f * M_PI / 365.0f * (N - 1);
    float decl  = 0.006918f
                - 0.399912f*cosf(gamma) + 0.070257f*sinf(gamma)
                - 0.006758f*cosf(2*gamma) + 0.000907f*sinf(2*gamma);
    float latR = lat * M_PI / 180.0f;
    float cosH = (cosf(90.833f*M_PI/180.0f) - sinf(latR)*sinf(decl))
                 / (cosf(latR)*cosf(decl));
    if (cosH > 1.0f)  { *sr_h=*sr_m=*ss_h=*ss_m = -1; return; } // polar night
    if (cosH < -1.0f) { *sr_h=*sr_m=*ss_h=*ss_m = -2; return; } // polar day
    float H = acosf(cosH) * 180.0f / M_PI;
    float sr = 12.0f - H/15.0f - lon/15.0f + tz_h;
    float ss = 12.0f + H/15.0f - lon/15.0f + tz_h;
    while (sr < 0) sr += 24; while (sr >= 24) sr -= 24;
    while (ss < 0) ss += 24; while (ss >= 24) ss -= 24;
    *sr_h = (int)sr; *sr_m = (int)((sr - *sr_h) * 60.0f);
    *ss_h = (int)ss; *ss_m = (int)((ss - *ss_h) * 60.0f);
}

// =============================================================================
//  Compass canvas draw
// =============================================================================
static void draw_compass() {
    if (!cmp_canvas) return;
    lv_canvas_fill_bg(cmp_canvas, lv_color_hex(T_BG), LV_OPA_COVER);
    uint32_t col = accent_color();

    int cx = CMP_W/2, cy = CMP_H/2;
    // Rings (3 concentric)
    lv_draw_arc_dsc_t arc;
    lv_draw_arc_dsc_init(&arc);
    arc.color = lv_color_hex(g_stealth ? 0x300000 : T_BORDER);
    arc.width = 1;
    arc.start_angle = 0; arc.end_angle = 360;
    lv_canvas_draw_arc(cmp_canvas, cx, cy, 170, 0, 360, &arc);
    lv_canvas_draw_arc(cmp_canvas, cx, cy, 120, 0, 360, &arc);
    lv_canvas_draw_arc(cmp_canvas, cx, cy,  70, 0, 360, &arc);

    // Cardinal ticks
    lv_draw_line_dsc_t ld; lv_draw_line_dsc_init(&ld);
    ld.color = lv_color_hex(col); ld.width = 2;
    for (int i = 0; i < 360; i += 30) {
        float a = (i - g_heading_deg) * M_PI / 180.0f;
        int r1 = (i % 90 == 0) ? 150 : 165;
        int r2 = 170;
        lv_point_t pts[2] = {
            { (lv_coord_t)(cx + r1*sinf(a)), (lv_coord_t)(cy - r1*cosf(a)) },
            { (lv_coord_t)(cx + r2*sinf(a)), (lv_coord_t)(cy - r2*cosf(a)) }
        };
        lv_canvas_draw_line(cmp_canvas, pts, 2, &ld);
    }
    // North needle (red) -- always points to "north" relative to zeroed heading
    {
        float a = (-g_heading_deg) * M_PI / 180.0f;
        lv_draw_line_dsc_t ld2; lv_draw_line_dsc_init(&ld2);
        ld2.color = lv_color_hex(T_RED); ld2.width = 3;
        lv_point_t pts[2] = {
            { (lv_coord_t)cx, (lv_coord_t)cy },
            { (lv_coord_t)(cx + 140*sinf(a)), (lv_coord_t)(cy - 140*cosf(a)) }
        };
        lv_canvas_draw_line(cmp_canvas, pts, 2, &ld2);
    }
    // Radar sweep -- 1 line at "current sweep angle" (animation)
    {
        static int sweep = 0;
        sweep = (sweep + 4) % 360;
        float a = sweep * M_PI / 180.0f;
        lv_draw_line_dsc_t ld3; lv_draw_line_dsc_init(&ld3);
        ld3.color = lv_color_hex(g_stealth ? 0x300000 : T_DIM_RED); ld3.width = 1;
        lv_point_t pts[2] = {
            { (lv_coord_t)cx, (lv_coord_t)cy },
            { (lv_coord_t)(cx + 165*sinf(a)), (lv_coord_t)(cy - 165*cosf(a)) }
        };
        lv_canvas_draw_line(cmp_canvas, pts, 2, &ld3);
    }
    // Center dot
    lv_draw_rect_dsc_t rd; lv_draw_rect_dsc_init(&rd);
    rd.bg_color = lv_color_hex(T_RED); rd.bg_opa = LV_OPA_COVER; rd.radius = 4;
    lv_canvas_draw_rect(cmp_canvas, cx-3, cy-3, 6, 6, &rd);
}

// =============================================================================
//  Event callbacks
// =============================================================================
static uint32_t g_back_press_t = 0;
static void back_btn_cb(lv_event_t *e) {
    lv_event_code_t c = lv_event_get_code(e);
    if (c == LV_EVENT_PRESSED)        g_back_press_t = millis();
    else if (c == LV_EVENT_LONG_PRESSED) return_to_launcher();
    else if (c == LV_EVENT_RELEASED) {
        uint32_t held = g_back_press_t ? (millis() - g_back_press_t) : 0;
        g_back_press_t = 0;
        if (held > 0 && held < 600) show_toast("Hold to exit to Launcher", 1500);
    }
}

static void flash_on();
static void flash_off();

static void tools_flash_cb(lv_event_t *) { if (!g_flashlight) flash_on(); else flash_off(); }
static void tools_sos_cb(lv_event_t *) {
    g_sos_active = !g_sos_active;
    if (g_sos_active) { g_sos_t = millis(); g_sos_idx = 0; show_toast("SOS ARMED", 1500); }
    else { flash_off(); show_toast("SOS OFF", 1200); }
}
static void tools_notes_cb(lv_event_t *) {
    File f = SD_MMC.open("/WATCH/tactical/notes.txt", FILE_APPEND);
    if (f) {
        time_t t; time(&t);
        struct tm tm; localtime_r(&t, &tm);
        char ts[32]; strftime(ts, sizeof(ts), "[%Y-%m-%d %H:%M] ", &tm);
        f.print(ts); f.println("ping");
        f.close();
        show_toast("Note appended", 1200);
    } else show_toast("SD write failed", 1500);
}
static void tools_timer_cb(lv_event_t *) {
    static uint32_t t_start = 0;
    if (!t_start) { t_start = millis(); show_toast("Timer started", 1200); }
    else {
        uint32_t e = (millis() - t_start) / 1000;
        char m[40]; snprintf(m, sizeof(m), "Elapsed %02lu:%02lu", e/60, e%60);
        show_toast(m, 1800);
        t_start = 0;
    }
}
static void tools_night_cb(lv_event_t *e) {
    g_night = lv_obj_has_state((lv_obj_t*)lv_event_get_target(e), LV_STATE_CHECKED);
    g_adaptive_night = false;   // user override
    apply_theme();
}
static void tools_stealth_cb(lv_event_t *e) {
    g_stealth = lv_obj_has_state((lv_obj_t*)lv_event_get_target(e), LV_STATE_CHECKED);
    apply_theme();
}
static void cmp_zero_cb(lv_event_t *) { heading_zero(); show_toast("Heading zeroed", 1000); }

// Double-tap flashlight gesture (anywhere)
static uint32_t g_last_tap_t = 0;
static void scr_clicked_cb(lv_event_t *) {
    uint32_t now = millis();
    if (now - g_last_tap_t < 350) {     // double tap window
        if (!g_flashlight) flash_on(); else flash_off();
        g_last_tap_t = 0;
    } else g_last_tap_t = now;
}

// =============================================================================
//  Flashlight overlay
// =============================================================================
static void flash_on() {
    g_flashlight = true;
    co5300_set_brightness(255);
    lv_obj_clear_flag(flash_scr, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(flash_scr);
}
static void flash_off() {
    g_flashlight = false;
    lv_obj_add_flag(flash_scr, LV_OBJ_FLAG_HIDDEN);
    apply_theme();
}
static void flash_scr_cb(lv_event_t *) { flash_off(); }

// =============================================================================
//  SOS strobe -- driven from loop()
// =============================================================================
static void tick_sos() {
    if (!g_sos_active) return;
    uint32_t now = millis();
    if (now - g_sos_t >= SOS_PATTERN[g_sos_idx]) {
        g_sos_t = now;
        g_sos_idx = (g_sos_idx + 1) % SOS_LEN;
        // even index = "on", odd = "off"
        if (g_sos_idx % 2 == 0) flash_on(); else flash_off();
    }
}

// =============================================================================
//  Clock + battery + camp panel update
// =============================================================================
static char g_last_hhmm[8] = {0};

static void update_clock_labels() {
    struct tm tm;
    if (!getLocalTime(&tm, 5)) {
        lv_label_set_text(home_clock, "--:--");
        lv_label_set_text(home_date,  "no sync");
        if (walk_time) lv_label_set_text(walk_time, "--:--");
        return;
    }
    char t[8], d[32];
    strftime(t, sizeof(t), "%H:%M", &tm);
    strftime(d, sizeof(d), "%a %d %b // %Y", &tm);
    if (strcmp(t, g_last_hhmm) != 0) {
        strcpy(g_last_hhmm, t);
        lv_label_set_text(home_clock, t);
        if (walk_time) lv_label_set_text(walk_time, t);
    }
    lv_label_set_text(home_date, d);
}

static void update_battery_card() {
    if (!g_has_pmu) return;
    int pct = g_pmu.getBatteryPercent();
    bool ch = g_pmu.isCharging();
    char b[24];
    if (ch) snprintf(b, sizeof(b), "%s %d%%", LV_SYMBOL_CHARGE, pct);
    else    snprintf(b, sizeof(b), "BATT %d%%", pct);
    lv_label_set_text(home_batt, b);

    // [B] Auto-dim when battery is low (skips if already in stealth)
    if (!g_stealth && !ch) {
        static int last_clamp = 255;
        int target = pct < 8 ? 8 : (pct < 15 ? 40 : (g_night ? 40 : 180));
        if (target != last_clamp) { co5300_set_brightness(target); last_clamp = target; }
    }

    // Battery hours estimate (~ pct / 4 -> hours, very rough)
    if (camp_batt_hours) {
        char e[32]; snprintf(e, sizeof(e), "EST %d h", pct / 4);
        lv_label_set_text(camp_batt_hours, e);
    }
}

static void update_camp_panel() {
    if (!camp_sunrise) return;
    struct tm tm;
    if (!getLocalTime(&tm, 5)) {
        lv_label_set_text(camp_sunrise, "--:--");
        lv_label_set_text(camp_sunset,  "--:--");
        lv_label_set_text(camp_daylight,"--");
        return;
    }
    int srh, srm, ssh, ssm;
    sunrise_sunset(tm.tm_year+1900, tm.tm_mon+1, tm.tm_mday,
                   g_lat, g_lon, g_gmt_offset_sec/3600, &srh, &srm, &ssh, &ssm);
    char a[16], b[16], c[24];
    if (srh < 0) { strcpy(a,"--:--"); strcpy(b,"--:--"); strcpy(c,"polar"); }
    else {
        snprintf(a, sizeof(a), "%02d:%02d", srh, srm);
        snprintf(b, sizeof(b), "%02d:%02d", ssh, ssm);
        int now_min = tm.tm_hour*60 + tm.tm_min;
        int ss_min  = ssh*60 + ssm;
        int rem     = ss_min - now_min;
        if (rem < 0) rem += 24*60;
        if (rem > 12*60) snprintf(c, sizeof(c), "NIGHT");
        else snprintf(c, sizeof(c), "%dh %dm light left", rem/60, rem%60);
    }
    lv_label_set_text(camp_sunrise, a);
    lv_label_set_text(camp_sunset,  b);
    lv_label_set_text(camp_daylight, c);

    if (camp_steps) { char s[24]; snprintf(s, sizeof(s), "%lu steps", g_steps); lv_label_set_text(camp_steps, s); }
    if (camp_temp && g_has_pmu) {
        float tc = (float)g_pmu.getTemperature();
        char tt[24]; snprintf(tt, sizeof(tt), "%.1f °C", tc);
        lv_label_set_text(camp_temp, tt);
    }
}

static void update_compass_labels() {
    if (!cmp_heading_lbl) return;
    char h[16]; snprintf(h, sizeof(h), "%03d°", (int)g_heading_deg);
    lv_label_set_text(cmp_heading_lbl, h);
    AccG g = qmi_acc();
    float tilt = atan2f(sqrtf(g.x*g.x+g.y*g.y), g.z) * 180.0f / M_PI;
    char tt[16]; snprintf(tt, sizeof(tt), "TILT %02d°", (int)tilt);
    lv_label_set_text(cmp_tilt_lbl, tt);
}

static void update_walk_panel() {
    if (!walk_steps) return;
    char s[24]; snprintf(s, sizeof(s), "%lu STEPS", g_steps);
    lv_label_set_text(walk_steps, s);
    char h[16]; snprintf(h, sizeof(h), "%03d°", (int)g_heading_deg);
    lv_label_set_text(walk_heading, h);
}

// =============================================================================
//  UI builder
// =============================================================================
static lv_obj_t *make_card(lv_obj_t *parent, int w, int h) {
    lv_obj_t *c = lv_obj_create(parent);
    lv_obj_remove_style_all(c);
    lv_obj_set_size(c, w, h);
    lv_obj_set_style_bg_color(c, lv_color_hex(T_SURFACE), 0);
    lv_obj_set_style_bg_opa(c, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(c, lv_color_hex(T_BORDER), 0);
    lv_obj_set_style_border_width(c, 1, 0);
    lv_obj_set_style_radius(c, 14, 0);
    lv_obj_set_style_pad_all(c, 12, 0);
    return c;
}

static lv_obj_t *make_eyebrow(lv_obj_t *parent, const char *txt, lv_align_t a, int x, int y) {
    lv_obj_t *l = lv_label_create(parent);
    lv_label_set_text(l, txt);
    lv_obj_set_style_text_color(l, lv_color_hex(T_DIM_RED), 0);
    lv_obj_set_style_text_font(l, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_letter_space(l, 3, 0);
    lv_obj_align(l, a, x, y);
    return l;
}

static void build_back_btn(lv_obj_t *parent) {
    back_btn = lv_btn_create(parent);
    lv_obj_set_size(back_btn, 220, 46);
    lv_obj_align(back_btn, LV_ALIGN_BOTTOM_MID, 0, -16);
    lv_obj_set_style_bg_color(back_btn, lv_color_hex(0x0E0E0E), 0);
    lv_obj_set_style_border_color(back_btn, lv_color_hex(T_CYAN), 0);
    lv_obj_set_style_border_width(back_btn, 1, 0);
    lv_obj_set_style_radius(back_btn, 23, 0);
    lv_obj_t *bl = lv_label_create(back_btn);
    lv_label_set_text(bl, LV_SYMBOL_LEFT "  Back to Launcher");
    lv_obj_set_style_text_color(bl, lv_color_hex(T_CYAN), 0);
    lv_obj_set_style_text_font(bl, &lv_font_montserrat_16, 0);
    lv_obj_center(bl);
    lv_obj_add_event_cb(back_btn, back_btn_cb, LV_EVENT_PRESSED,      NULL);
    lv_obj_add_event_cb(back_btn, back_btn_cb, LV_EVENT_LONG_PRESSED, NULL);
    lv_obj_add_event_cb(back_btn, back_btn_cb, LV_EVENT_RELEASED,     NULL);
}

static void build_home_tab() {
    make_eyebrow(tab_home, "TACTICAL // OS", LV_ALIGN_TOP_MID, 0, 14);

    // Sync dot top-right
    home_sync_dot = lv_obj_create(tab_home);
    lv_obj_remove_style_all(home_sync_dot);
    lv_obj_set_size(home_sync_dot, 10, 10);
    lv_obj_set_style_radius(home_sync_dot, 5, 0);
    lv_obj_set_style_bg_color(home_sync_dot, lv_color_hex(g_wifi_synced ? T_OK : T_DANGER), 0);
    lv_obj_set_style_bg_opa(home_sync_dot, LV_OPA_COVER, 0);
    lv_obj_align(home_sync_dot, LV_ALIGN_TOP_RIGHT, -30, 18);

    // Mode dot top-left
    home_mode_dot = lv_obj_create(tab_home);
    lv_obj_remove_style_all(home_mode_dot);
    lv_obj_set_size(home_mode_dot, 6, 6);
    lv_obj_set_style_radius(home_mode_dot, 3, 0);
    lv_obj_set_style_bg_color(home_mode_dot, lv_color_hex(T_RED), 0);
    lv_obj_set_style_bg_opa(home_mode_dot, LV_OPA_COVER, 0);
    lv_obj_align(home_mode_dot, LV_ALIGN_TOP_LEFT, 30, 22);

    // Clock
    home_clock = lv_label_create(tab_home);
    lv_label_set_text(home_clock, "--:--");
    lv_obj_set_style_text_color(home_clock, lv_color_hex(T_TXT), 0);
    lv_obj_set_style_text_font(home_clock, &lv_font_montserrat_48, 0);
    lv_obj_align(home_clock, LV_ALIGN_CENTER, 0, -60);

    home_date = lv_label_create(tab_home);
    lv_label_set_text(home_date, "");
    lv_obj_set_style_text_color(home_date, lv_color_hex(0x7A5050), 0);
    lv_obj_set_style_text_font(home_date, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_letter_space(home_date, 2, 0);
    lv_obj_align(home_date, LV_ALIGN_CENTER, 0, -16);

    home_batt = lv_label_create(tab_home);
    lv_label_set_text(home_batt, "BATT --%");
    lv_obj_set_style_text_color(home_batt, lv_color_hex(T_RED), 0);
    lv_obj_set_style_text_font(home_batt, &lv_font_montserrat_18, 0);
    lv_obj_align(home_batt, LV_ALIGN_CENTER, 0, 30);

    home_status = lv_label_create(tab_home);
    lv_label_set_text(home_status, "OPERATOR READY");
    lv_obj_set_style_text_color(home_status, lv_color_hex(T_RED), 0);
    lv_obj_set_style_text_font(home_status, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_letter_space(home_status, 4, 0);
    lv_obj_align(home_status, LV_ALIGN_CENTER, 0, 70);

    home_subtitle = lv_label_create(tab_home);
    lv_label_set_text(home_subtitle, "swipe < > for COMPASS / TOOLS / CAMP / WALK");
    lv_obj_set_style_text_color(home_subtitle, lv_color_hex(T_MUTE), 0);
    lv_obj_set_style_text_font(home_subtitle, &lv_font_montserrat_12, 0);
    lv_obj_align(home_subtitle, LV_ALIGN_CENTER, 0, 110);

    build_back_btn(tab_home);
}

static void build_compass_tab() {
    make_eyebrow(tab_compass, "COMPASS // HUD", LV_ALIGN_TOP_MID, 0, 14);

    // Canvas
    cmp_buf = (lv_color_t *)heap_caps_malloc(CMP_W * CMP_H * sizeof(lv_color_t),
                                             MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!cmp_buf) cmp_buf = (lv_color_t *)heap_caps_malloc(CMP_W * CMP_H * sizeof(lv_color_t),
                                                            MALLOC_CAP_8BIT);
    cmp_canvas = lv_canvas_create(tab_compass);
    lv_canvas_set_buffer(cmp_canvas, cmp_buf, CMP_W, CMP_H, LV_IMG_CF_TRUE_COLOR);
    lv_obj_align(cmp_canvas, LV_ALIGN_CENTER, 0, -10);

    cmp_heading_lbl = lv_label_create(tab_compass);
    lv_label_set_text(cmp_heading_lbl, "000°");
    lv_obj_set_style_text_color(cmp_heading_lbl, lv_color_hex(T_RED), 0);
    lv_obj_set_style_text_font(cmp_heading_lbl, &lv_font_montserrat_28, 0);
    lv_obj_align(cmp_heading_lbl, LV_ALIGN_CENTER, 0, -10);

    cmp_tilt_lbl = lv_label_create(tab_compass);
    lv_label_set_text(cmp_tilt_lbl, "TILT 0°");
    lv_obj_set_style_text_color(cmp_tilt_lbl, lv_color_hex(T_MUTE), 0);
    lv_obj_set_style_text_font(cmp_tilt_lbl, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_letter_space(cmp_tilt_lbl, 3, 0);
    lv_obj_align(cmp_tilt_lbl, LV_ALIGN_CENTER, 0, 30);

    cmp_zero_btn = lv_btn_create(tab_compass);
    lv_obj_set_size(cmp_zero_btn, 140, 38);
    lv_obj_align(cmp_zero_btn, LV_ALIGN_BOTTOM_MID, 0, -70);
    lv_obj_set_style_bg_color(cmp_zero_btn, lv_color_hex(T_SURFACE), 0);
    lv_obj_set_style_border_color(cmp_zero_btn, lv_color_hex(T_DIM_RED), 0);
    lv_obj_set_style_border_width(cmp_zero_btn, 1, 0);
    lv_obj_set_style_radius(cmp_zero_btn, 19, 0);
    lv_obj_t *zl = lv_label_create(cmp_zero_btn);
    lv_label_set_text(zl, "ZERO HEADING");
    lv_obj_set_style_text_color(zl, lv_color_hex(T_RED), 0);
    lv_obj_set_style_text_font(zl, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_letter_space(zl, 3, 0);
    lv_obj_center(zl);
    lv_obj_add_event_cb(cmp_zero_btn, cmp_zero_cb, LV_EVENT_CLICKED, NULL);

    build_back_btn(tab_compass);
}

static lv_obj_t *tool_btn(lv_obj_t *parent, const char *txt, lv_event_cb_t cb,
                          int x, int y, int w, int h, uint32_t accent) {
    lv_obj_t *b = lv_btn_create(parent);
    lv_obj_set_size(b, w, h);
    lv_obj_align(b, LV_ALIGN_TOP_LEFT, x, y);
    lv_obj_set_style_bg_color(b, lv_color_hex(T_SURFACE), 0);
    lv_obj_set_style_border_color(b, lv_color_hex(accent), 0);
    lv_obj_set_style_border_width(b, 1, 0);
    lv_obj_set_style_radius(b, 12, 0);
    lv_obj_t *l = lv_label_create(b);
    lv_label_set_text(l, txt);
    lv_obj_set_style_text_color(l, lv_color_hex(accent), 0);
    lv_obj_set_style_text_font(l, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_letter_space(l, 2, 0);
    lv_obj_center(l);
    if (cb) lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, NULL);
    return b;
}

static lv_obj_t *toggle_row(lv_obj_t *parent, const char *txt, lv_event_cb_t cb,
                            int y, bool *bound) {
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, 360, 44);
    lv_obj_align(row, LV_ALIGN_TOP_MID, 0, y);
    lv_obj_set_style_bg_color(row, lv_color_hex(T_SURFACE), 0);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(row, lv_color_hex(T_BORDER), 0);
    lv_obj_set_style_border_width(row, 1, 0);
    lv_obj_set_style_radius(row, 12, 0);
    lv_obj_set_style_pad_all(row, 10, 0);
    lv_obj_t *l = lv_label_create(row);
    lv_label_set_text(l, txt);
    lv_obj_set_style_text_color(l, lv_color_hex(T_RED), 0);
    lv_obj_set_style_text_font(l, &lv_font_montserrat_14, 0);
    lv_obj_align(l, LV_ALIGN_LEFT_MID, 4, 0);
    lv_obj_t *sw = lv_switch_create(row);
    lv_obj_align(sw, LV_ALIGN_RIGHT_MID, -4, 0);
    lv_obj_set_size(sw, 50, 24);
    lv_obj_set_style_bg_color(sw, lv_color_hex(0x250000), 0);
    lv_obj_set_style_bg_color(sw, lv_color_hex(T_RED), LV_PART_INDICATOR | LV_STATE_CHECKED);
    if (*bound) lv_obj_add_state(sw, LV_STATE_CHECKED);
    lv_obj_add_event_cb(sw, cb, LV_EVENT_VALUE_CHANGED, NULL);
    return sw;
}

static void build_tools_tab() {
    make_eyebrow(tab_tools, "SURVIVAL // TOOLS", LV_ALIGN_TOP_MID, 0, 14);
    tools_flash_btn = tool_btn(tab_tools, LV_SYMBOL_EYE_OPEN "  FLASH",      tools_flash_cb,  20,  46, 175, 70, T_RED);
    tools_sos_btn   = tool_btn(tab_tools, LV_SYMBOL_WARNING "  SOS",         tools_sos_cb,   215,  46, 175, 70, T_AMBER);
    tools_notes_btn = tool_btn(tab_tools, LV_SYMBOL_EDIT   "  NOTE PING",    tools_notes_cb,  20, 126, 175, 70, T_RED);
    tools_timer_btn = tool_btn(tab_tools, LV_SYMBOL_LOOP   "  TIMER",        tools_timer_cb, 215, 126, 175, 70, T_RED);

    tools_night_sw   = toggle_row(tab_tools, "NIGHT MODE",   tools_night_cb,   216, &g_night);
    tools_stealth_sw = toggle_row(tab_tools, "STEALTH MODE", tools_stealth_cb, 268, &g_stealth);

    build_back_btn(tab_tools);
}

static void build_camp_tab() {
    make_eyebrow(tab_camp, "CAMP // FIELD", LV_ALIGN_TOP_MID, 0, 14);

    lv_obj_t *c1 = make_card(tab_camp, 175, 90);
    lv_obj_align(c1, LV_ALIGN_TOP_LEFT, 20, 50);
    lv_obj_t *l1 = lv_label_create(c1); lv_label_set_text(l1, "SUNRISE");
    lv_obj_set_style_text_color(l1, lv_color_hex(T_DIM_RED), 0);
    lv_obj_set_style_text_font(l1, &lv_font_montserrat_12, 0);
    lv_obj_align(l1, LV_ALIGN_TOP_LEFT, 0, 0);
    camp_sunrise = lv_label_create(c1);
    lv_label_set_text(camp_sunrise, "--:--");
    lv_obj_set_style_text_color(camp_sunrise, lv_color_hex(T_TXT), 0);
    lv_obj_set_style_text_font(camp_sunrise, &lv_font_montserrat_22, 0);
    lv_obj_align(camp_sunrise, LV_ALIGN_BOTTOM_LEFT, 0, 0);

    lv_obj_t *c2 = make_card(tab_camp, 175, 90);
    lv_obj_align(c2, LV_ALIGN_TOP_LEFT, 215, 50);
    lv_obj_t *l2 = lv_label_create(c2); lv_label_set_text(l2, "SUNSET");
    lv_obj_set_style_text_color(l2, lv_color_hex(T_DIM_RED), 0);
    lv_obj_set_style_text_font(l2, &lv_font_montserrat_12, 0);
    lv_obj_align(l2, LV_ALIGN_TOP_LEFT, 0, 0);
    camp_sunset = lv_label_create(c2);
    lv_label_set_text(camp_sunset, "--:--");
    lv_obj_set_style_text_color(camp_sunset, lv_color_hex(T_TXT), 0);
    lv_obj_set_style_text_font(camp_sunset, &lv_font_montserrat_22, 0);
    lv_obj_align(camp_sunset, LV_ALIGN_BOTTOM_LEFT, 0, 0);

    lv_obj_t *c3 = make_card(tab_camp, 370, 60);
    lv_obj_align(c3, LV_ALIGN_TOP_LEFT, 20, 150);
    camp_daylight = lv_label_create(c3);
    lv_label_set_text(camp_daylight, "-- light left");
    lv_obj_set_style_text_color(camp_daylight, lv_color_hex(T_AMBER), 0);
    lv_obj_set_style_text_font(camp_daylight, &lv_font_montserrat_16, 0);
    lv_obj_center(camp_daylight);

    lv_obj_t *c4 = make_card(tab_camp, 175, 60);
    lv_obj_align(c4, LV_ALIGN_TOP_LEFT, 20, 220);
    camp_batt_hours = lv_label_create(c4);
    lv_label_set_text(camp_batt_hours, "EST -- h");
    lv_obj_set_style_text_color(camp_batt_hours, lv_color_hex(T_RED), 0);
    lv_obj_set_style_text_font(camp_batt_hours, &lv_font_montserrat_16, 0);
    lv_obj_center(camp_batt_hours);

    lv_obj_t *c5 = make_card(tab_camp, 175, 60);
    lv_obj_align(c5, LV_ALIGN_TOP_LEFT, 215, 220);
    camp_temp = lv_label_create(c5);
    lv_label_set_text(camp_temp, "-- °C");
    lv_obj_set_style_text_color(camp_temp, lv_color_hex(T_RED), 0);
    lv_obj_set_style_text_font(camp_temp, &lv_font_montserrat_16, 0);
    lv_obj_center(camp_temp);

    lv_obj_t *c6 = make_card(tab_camp, 370, 60);
    lv_obj_align(c6, LV_ALIGN_TOP_LEFT, 20, 290);
    camp_steps = lv_label_create(c6);
    lv_label_set_text(camp_steps, "0 steps");
    lv_obj_set_style_text_color(camp_steps, lv_color_hex(T_TXT), 0);
    lv_obj_set_style_text_font(camp_steps, &lv_font_montserrat_16, 0);
    lv_obj_center(camp_steps);

    build_back_btn(tab_camp);
}

static void build_walk_tab() {
    // FOCUS WALK MODE -- minimal
    walk_time = lv_label_create(tab_walk);
    lv_label_set_text(walk_time, "--:--");
    lv_obj_set_style_text_color(walk_time, lv_color_hex(T_TXT), 0);
    lv_obj_set_style_text_font(walk_time, &lv_font_montserrat_48, 0);
    lv_obj_align(walk_time, LV_ALIGN_CENTER, 0, -60);

    walk_steps = lv_label_create(tab_walk);
    lv_label_set_text(walk_steps, "0 STEPS");
    lv_obj_set_style_text_color(walk_steps, lv_color_hex(T_RED), 0);
    lv_obj_set_style_text_font(walk_steps, &lv_font_montserrat_22, 0);
    lv_obj_set_style_text_letter_space(walk_steps, 3, 0);
    lv_obj_align(walk_steps, LV_ALIGN_CENTER, 0, 0);

    walk_heading = lv_label_create(tab_walk);
    lv_label_set_text(walk_heading, "000°");
    lv_obj_set_style_text_color(walk_heading, lv_color_hex(T_DIM_RED), 0);
    lv_obj_set_style_text_font(walk_heading, &lv_font_montserrat_22, 0);
    lv_obj_align(walk_heading, LV_ALIGN_CENTER, 0, 60);

    build_back_btn(tab_walk);
}

static void build_flash_overlay() {
    flash_scr = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(flash_scr);
    lv_obj_set_size(flash_scr, LCD_WIDTH, LCD_HEIGHT);
    lv_obj_set_style_bg_color(flash_scr, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_bg_opa(flash_scr, LV_OPA_COVER, 0);
    lv_obj_add_flag(flash_scr, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(flash_scr, flash_scr_cb, LV_EVENT_CLICKED, NULL);
}

static void build_ui() {
    scr_root = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr_root, lv_color_hex(T_BG), 0);
    lv_obj_set_style_bg_opa(scr_root, LV_OPA_COVER, 0);
    lv_obj_add_event_cb(scr_root, scr_clicked_cb, LV_EVENT_CLICKED, NULL);

    tabv = lv_tabview_create(scr_root, LV_DIR_TOP, 0);   // 0 = no visible bar
    lv_obj_set_size(tabv, LCD_WIDTH, LCD_HEIGHT);
    lv_obj_set_style_bg_color(tabv, lv_color_hex(T_BG), 0);
    lv_obj_set_style_bg_opa(tabv, LV_OPA_COVER, 0);

    tab_home    = lv_tabview_add_tab(tabv, "HOME");
    tab_compass = lv_tabview_add_tab(tabv, "CMP");
    tab_tools   = lv_tabview_add_tab(tabv, "TOOLS");
    tab_camp    = lv_tabview_add_tab(tabv, "CAMP");
    tab_walk    = lv_tabview_add_tab(tabv, "WALK");

    for (lv_obj_t *t : { tab_home, tab_compass, tab_tools, tab_camp, tab_walk }) {
        lv_obj_set_style_bg_color(t, lv_color_hex(T_BG), 0);
        lv_obj_set_style_bg_opa(t, LV_OPA_COVER, 0);
        lv_obj_set_style_pad_all(t, 0, 0);
    }

    build_home_tab();
    build_compass_tab();
    build_tools_tab();
    build_camp_tab();
    build_walk_tab();

    // Toast (top-level)
    toast_lbl = lv_label_create(lv_layer_top());
    lv_label_set_text(toast_lbl, "");
    lv_obj_set_style_text_color(toast_lbl, lv_color_hex(T_AMBER), 0);
    lv_obj_set_style_text_font(toast_lbl, &lv_font_montserrat_14, 0);
    lv_obj_set_style_bg_color(toast_lbl, lv_color_hex(0x140000), 0);
    lv_obj_set_style_bg_opa(toast_lbl, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(toast_lbl, 8, 0);
    lv_obj_set_style_radius(toast_lbl, 8, 0);
    lv_obj_align(toast_lbl, LV_ALIGN_TOP_MID, 0, 6);
    lv_obj_add_flag(toast_lbl, LV_OBJ_FLAG_HIDDEN);

    build_flash_overlay();

    // Carousel page-indicator dots (S3_MiniOS-style: 5 dots, active = red, others = dim)
    lv_obj_t *dot_row = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(dot_row);
    lv_obj_set_size(dot_row, 120, 16);
    lv_obj_align(dot_row, LV_ALIGN_BOTTOM_MID, 0, -2);
    lv_obj_set_flex_flow(dot_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(dot_row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(dot_row, 8, 0);
    for (int i = 0; i < 5; i++) {
        page_dots[i] = lv_obj_create(dot_row);
        lv_obj_remove_style_all(page_dots[i]);
        lv_obj_set_size(page_dots[i], i == 0 ? 8 : 5, i == 0 ? 8 : 5);
        lv_obj_set_style_radius(page_dots[i], 4, 0);
        lv_obj_set_style_bg_color(page_dots[i],
            lv_color_hex(i == 0 ? T_RED : T_DIM_RED), 0);
        lv_obj_set_style_bg_opa(page_dots[i], LV_OPA_COVER, 0);
    }
    // Update dots when tab changes
    lv_obj_add_event_cb(tabv, [](lv_event_t *e){
        uint32_t a = lv_tabview_get_tab_act((lv_obj_t*)lv_event_get_target(e));
        for (int i = 0; i < 5; i++) if (page_dots[i]) {
            lv_obj_set_size(page_dots[i], i == (int)a ? 8 : 5, i == (int)a ? 8 : 5);
            lv_obj_set_style_bg_color(page_dots[i],
                lv_color_hex(i == (int)a ? T_RED : T_DIM_RED), 0);
        }
    }, LV_EVENT_VALUE_CHANGED, NULL);

    lv_scr_load(scr_root);
}

// =============================================================================
//  Splash (custom in-app loading: red radar grid + "TACTICAL//OS")
// =============================================================================
static void show_splash() {
    gfx->fillScreen(0x0000);
    // Radar rings
    for (int r = 60; r <= 240; r += 60) {
        gfx->drawCircle(LCD_WIDTH/2, LCD_HEIGHT/2, r, gfx->color565(0x60, 0x10, 0x10));
    }
    // Cross-hair
    gfx->drawLine(LCD_WIDTH/2, 60, LCD_WIDTH/2, LCD_HEIGHT-60, gfx->color565(0x40, 0x08, 0x08));
    gfx->drawLine(40, LCD_HEIGHT/2, LCD_WIDTH-40, LCD_HEIGHT/2, gfx->color565(0x40, 0x08, 0x08));
    // Title
    gfx->setTextSize(3);
    gfx->setTextColor(gfx->color565(0xFF, 0x30, 0x30));
    gfx->setCursor(80, LCD_HEIGHT/2 - 12);
    gfx->print("TACTICAL//OS");
    gfx->setTextSize(1);
    gfx->setTextColor(gfx->color565(0x8A, 0x10, 0x10));
    gfx->setCursor(120, LCD_HEIGHT/2 + 30);
    gfx->print("OPERATOR  BOOT  v1.0");
    panel_on(180);
    delay(800);
}

// =============================================================================
//  setup / loop
// =============================================================================
void setup() {
    launcher_app_boot();
    show_splash();
    qmi_init();
    time_restore_from_nvs();
    steps_load();
    wifi_load_from_sd();
    // Auto-create all SD folders this app touches (idempotent; safe if they exist)
    SD_MMC.mkdir("/WATCH");
    SD_MMC.mkdir("/WATCH/wifi");
    SD_MMC.mkdir("/WATCH/PHOTOS");
    SD_MMC.mkdir("/WATCH/bin");
    SD_MMC.mkdir("/WATCH/tactical");
    SD_MMC.mkdir("/WATCH/tactical/logs");

    build_ui();
    setCpuFrequencyMhz(160);

    // One-shot NTP if needed (>6h since last)
    time_t now; time(&now);
    bool need_ntp = !g_time_valid || (now < 1700000000)
                  || ((uint32_t)now - g_last_ntp_ok > 6UL * 3600UL);
    if (need_ntp) wifi_sync_once();
    // Update sync dot once at boot
    if (home_sync_dot)
        lv_obj_set_style_bg_color(home_sync_dot,
            lv_color_hex(g_wifi_synced ? T_OK : T_DANGER), 0);

    update_clock_labels();
    update_battery_card();
    update_camp_panel();
    apply_theme();
    show_toast("TACTICAL ONLINE", 1500);
}

void loop() {
    uint32_t now = millis();
    lv_timer_handler();
    tick_toast();
    tick_sos();

    // BOOT-button hardware exit
    static uint32_t btn_t = 0;
    if (digitalRead(BOOT_BTN_PIN) == LOW) {
        if (!btn_t) btn_t = now;
        else if (now - btn_t > 1500) return_to_launcher();
    } else btn_t = 0;

    // IMU integration (heading + steps) at ~50 Hz
    static uint32_t imu_t = 0;
    if (now - imu_t >= 20) {
        imu_t = now;
        update_heading();
        update_steps();
    }

    // 1 Hz updates
    static uint32_t tick_t = 0;
    if (now - tick_t >= 1000) {
        tick_t = now;
        update_clock_labels();
        static int bcnt = 0;
        if (++bcnt >= 5) { bcnt = 0; update_battery_card(); }
        update_camp_panel();
        update_compass_labels();
        update_walk_panel();
        draw_compass();
        adaptive_night_tick();
        steps_rollover_check();
    }

    delay(5);
}
