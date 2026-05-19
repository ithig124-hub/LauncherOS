// =============================================================================
//  NavOS.ino v1.0 -- LauncherOS-compatible offline compass / wayfinding deck
//  Target  : Waveshare ESP32-S3 Touch AMOLED 2.06 (410 x 502, CO5300, FT3168)
//  Compile : ESP32-S3 Dev Module / PSRAM=OPI / Flash=16MB / Minimal SPIFFS (large APP)
//  Export  : Sketch -> Export Compiled Binary  ->  copy NavOS.ino.bin to
//            /WATCH/bin/navos.bin on the SD card.
// -----------------------------------------------------------------------------
//  Splash style : rotating brass compass rose + "NAV//OS"
//  SD root      : /WATCH/nav/      (saved bearings: bearings.txt)
//  NVS ns       : "navos"
//  Features     :
//    [1] Tilt-compensated compass via QMI8658 accel + mag (heading 0-359°)
//    [2] Bearing lock: tap "MARK" to remember a direction, arrow guides back
//    [3] Bubble level + protractor + plumb line (long-press centre to swap mode)
//    [4] 4 home screens: Compass | Bearings | Level | Sun (swipe left/right)
//    [5] Mag soft/hard iron calibration: hold MARK 5 s + figure-8 motion
// =============================================================================

#include <Arduino.h>
#include <FS.h>
#include <SD_MMC.h>
#include <Wire.h>
#include <WiFi.h>
#include <Preferences.h>
#include <time.h>
#include <sys/time.h>
#include <memory>
#include <math.h>
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

// ===== SHARED BOOT BLOCK (LauncherOS v6.1+) ==================================
#define LCD_SDIO0 4
#define LCD_SDIO1 5
#define LCD_SDIO2 6
#define LCD_SDIO3 7
#define LCD_SCLK 11
#define LCD_CS   12
#define LCD_RESET 8
#define LCD_WIDTH 410
#define LCD_HEIGHT 502
#define IIC_SDA 15
#define IIC_SCL 14
#define TP_INT 38
#define TP_RESET 9
static const int SDMMC_CLK=2, SDMMC_CMD=1, SDMMC_DATA=3;
#define BOOT_BTN_PIN 0
#define PWR_BTN_PIN  10

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
static int32_t ts_x=0, ts_y=0; static uint32_t ts_ms=0;
static int32_t tc_x=0, tc_y=0; static bool touch_active=false;
enum SwipeEv { SW_NONE, SW_LEFT, SW_RIGHT };
static volatile SwipeEv g_pending_swipe = SW_NONE;

static void my_disp_flush(lv_disp_drv_t *d, const lv_area_t *a, lv_color_t *px) {
    uint32_t w=a->x2-a->x1+1, h=a->y2-a->y1+1;
    gfx->draw16bitRGBBitmap(a->x1, a->y1, (uint16_t*)px, w, h);
    lv_disp_flush_ready(d);
}
static void my_touchpad_read(lv_indev_drv_t *, lv_indev_data_t *data) {
    int32_t x=FT3168->IIC_Read_Device_Value(FT3168->Arduino_IIC_Touch::Value_Information::TOUCH_COORDINATE_X);
    int32_t y=FT3168->IIC_Read_Device_Value(FT3168->Arduino_IIC_Touch::Value_Information::TOUCH_COORDINATE_Y);
    int fingers=FT3168->IIC_Read_Device_Value(FT3168->Arduino_IIC_Touch::Value_Information::TOUCH_FINGER_NUMBER);
    bool pressed = (fingers > 0);
    if (pressed) {
        data->state=LV_INDEV_STATE_PR; data->point.x=x; data->point.y=y;
        if (!touch_active) { touch_active=true; ts_x=x; ts_y=y; ts_ms=millis(); }
        tc_x=x; tc_y=y;
    } else {
        data->state=LV_INDEV_STATE_REL;
        if (touch_active) {
            touch_active=false;
            int32_t dx=tc_x-ts_x, dy=tc_y-ts_y; uint32_t dur=millis()-ts_ms;
            if (dur<800 && abs(dx)>25 && abs(dx)>abs(dy)) g_pending_swipe = (dx<0)?SW_LEFT:SW_RIGHT;
        }
    }
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
    if (!gfx->begin()) Serial.println("gfx fail");
    gfx->fillScreen(0x0000);
    pinMode(IIC_SCL, OUTPUT);
    for (int i=0; i<9; i++) { digitalWrite(IIC_SCL,LOW); delayMicroseconds(5); digitalWrite(IIC_SCL,HIGH); delayMicroseconds(5); }
    Wire.begin(IIC_SDA, IIC_SCL);
    pinMode(TP_RESET, OUTPUT); digitalWrite(TP_RESET, LOW); delay(50); digitalWrite(TP_RESET, HIGH); delay(200);
    FT3168->begin();
    pmu_init();
    lv_init();
    size_t bufPx = LCD_WIDTH * 40;
    lvgl_buf = (lv_color_t*)heap_caps_malloc(bufPx*sizeof(lv_color_t), MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT);
    if (!lvgl_buf) lvgl_buf = (lv_color_t*)heap_caps_malloc(bufPx*sizeof(lv_color_t), MALLOC_CAP_8BIT);
    lv_disp_draw_buf_init(&draw_buf, lvgl_buf, NULL, bufPx);
    lv_disp_drv_init(&disp_drv); disp_drv.flush_cb=my_disp_flush; disp_drv.draw_buf=&draw_buf;
    disp_drv.hor_res=LCD_WIDTH; disp_drv.ver_res=LCD_HEIGHT;
    lv_disp_drv_register(&disp_drv);
    lv_indev_drv_init(&indev_drv); indev_drv.type=LV_INDEV_TYPE_POINTER; indev_drv.read_cb=my_touchpad_read;
    lv_indev_drv_register(&indev_drv);
    SD_MMC.setPins(SDMMC_CLK, SDMMC_CMD, SDMMC_DATA);
    SD_MMC.begin("/sdcard", true);
}

static void time_restore_from_launcher() {
    Preferences p;
    if (!p.begin("rtcsave", true)) return;
    uint32_t t = p.getULong("t", 0); long gmt = p.getLong("gmt", 0); p.end();
    if (t > 1700000000) {
        struct timeval tv = { .tv_sec=(time_t)t, .tv_usec=0 };
        settimeofday(&tv, NULL);
        char tz[16]; snprintf(tz, sizeof(tz), "UTC%+ld", -gmt/3600);
        setenv("TZ", tz, 1); tzset();
    }
}
// ===== END SHARED BOOT BLOCK =================================================

// =============================================================================
//  QMI8658 (accel + gyro) + AK09918 magnetometer
//  The Waveshare 2.06 ships the QMI8658 with built-in mag.  Direct I2C reads.
// =============================================================================
#define QMI_ADDR 0x6B   // primary; 0x6A is alt
#define AKM_ADDR 0x0C   // AK09918 magnetometer on the same I2C bus
static bool g_has_imu=false, g_has_mag=false;
static float g_ax=0, g_ay=0, g_az=1.0f;          // g
static float g_mx=0, g_my=0, g_mz=0;             // uT raw
// hard-iron offsets + soft-iron diagonal (calibrated values, stored in NVS)
static float g_mx_off=0, g_my_off=0, g_mz_off=0;
static float g_mx_scl=1, g_my_scl=1, g_mz_scl=1;

static bool i2c_read(uint8_t addr, uint8_t reg, uint8_t *out, size_t n) {
    Wire.beginTransmission(addr); Wire.write(reg);
    if (Wire.endTransmission(false) != 0) return false;
    Wire.requestFrom((int)addr, (int)n);
    size_t i=0; while (Wire.available() && i<n) out[i++] = Wire.read();
    return i == n;
}
static bool i2c_write(uint8_t addr, uint8_t reg, uint8_t val) {
    Wire.beginTransmission(addr); Wire.write(reg); Wire.write(val);
    return Wire.endTransmission() == 0;
}
static void imu_init() {
    Wire.beginTransmission(QMI_ADDR);
    if (Wire.endTransmission() == 0) {
        uint8_t who=0; i2c_read(QMI_ADDR, 0x00, &who, 1);
        if (who == 0x05) {
            i2c_write(QMI_ADDR, 0x02, 0x60);   // CTRL1
            i2c_write(QMI_ADDR, 0x03, 0x95);   // CTRL2 accel 2g 125Hz
            i2c_write(QMI_ADDR, 0x04, 0x53);   // CTRL3 gyro 256dps 125Hz
            i2c_write(QMI_ADDR, 0x08, 0x03);   // CTRL7 enable accel+gyro
            g_has_imu = true;
        }
    }
    Wire.beginTransmission(AKM_ADDR);
    if (Wire.endTransmission() == 0) {
        i2c_write(AKM_ADDR, 0x31, 0x08);       // CNTL2 continuous mode 100Hz
        g_has_mag = true;
    }
}
static void imu_read() {
    if (g_has_imu) {
        uint8_t b[6];
        if (i2c_read(QMI_ADDR, 0x35, b, 6)) {
            int16_t ax=(int16_t)((b[1]<<8)|b[0]);
            int16_t ay=(int16_t)((b[3]<<8)|b[2]);
            int16_t az=(int16_t)((b[5]<<8)|b[4]);
            g_ax = ax/16384.0f; g_ay = ay/16384.0f; g_az = az/16384.0f;
        }
    }
    if (g_has_mag) {
        uint8_t b[8]; i2c_read(AKM_ADDR, 0x10, b, 8);  // ST1..ST2
        int16_t mx=(int16_t)((b[2]<<8)|b[1]);
        int16_t my=(int16_t)((b[4]<<8)|b[3]);
        int16_t mz=(int16_t)((b[6]<<8)|b[5]);
        g_mx = (mx*0.15f) - g_mx_off; g_my = (my*0.15f) - g_my_off; g_mz = (mz*0.15f) - g_mz_off;
        g_mx *= g_mx_scl; g_my *= g_my_scl; g_mz *= g_mz_scl;
    }
}
static float tilt_comp_heading() {
    // Tilt compensation via accel-derived roll/pitch
    float roll  = atan2f(g_ay, g_az);
    float pitch = atan2f(-g_ax, sqrtf(g_ay*g_ay + g_az*g_az));
    float xh = g_mx*cosf(pitch) + g_mz*sinf(pitch);
    float yh = g_mx*sinf(roll)*sinf(pitch) + g_my*cosf(roll) - g_mz*sinf(roll)*cosf(pitch);
    float h = atan2f(yh, xh) * 180.0f / (float)M_PI;
    if (h < 0) h += 360.0f;
    return h;
}

// =============================================================================
//  Splash: rotating brass compass rose
// =============================================================================
static void splash_show() {
    int cx = LCD_WIDTH/2, cy = LCD_HEIGHT/2;
    for (int f = 0; f < 30; f++) {
        gfx->fillScreen(0x0000);
        float a = f * 12.0f * (float)M_PI / 180.0f;
        // outer ring
        gfx->drawCircle(cx, cy, 110, 0xBD60);  // brass
        gfx->drawCircle(cx, cy, 100, 0x52A0);
        // 16 tick marks rotated by a
        for (int t=0; t<16; t++) {
            float ta = a + t * (float)M_PI / 8.0f;
            int x1 = cx + (int)(95 * cosf(ta));
            int y1 = cy + (int)(95 * sinf(ta));
            int x2 = cx + (int)(108 * cosf(ta));
            int y2 = cy + (int)(108 * sinf(ta));
            gfx->drawLine(x1, y1, x2, y2, (t%4==0) ? 0xFFE0 : 0xBD60);
        }
        // N-needle
        int nx = cx + (int)(80 * cosf(a - (float)M_PI/2));
        int ny = cy + (int)(80 * sinf(a - (float)M_PI/2));
        gfx->fillTriangle(cx, cy, cx-6, cy, nx, ny, 0xF800);
        gfx->fillTriangle(cx, cy, cx+6, cy, nx, ny, 0xF800);
        gfx->setTextColor(0xFFE0);
        gfx->setTextSize(2);
        gfx->setCursor(cx-58, LCD_HEIGHT-70);
        gfx->print("NAV//OS");
        delay(38);
    }
}

// =============================================================================
//  UI screens (4):  Compass | Bearings | Level | Sun
// =============================================================================
#define COL_BG 0x000000
#define COL_TXT 0xFFFFFF
#define COL_MUTE 0x666666
#define COL_ACCENT 0x00E5FF
#define COL_GOLD 0xFFB300

static int g_screen = 0;
static lv_obj_t *root[4] = {nullptr};
static lv_obj_t *hdg_lbl=nullptr, *card_lbl=nullptr;
static lv_obj_t *bear_list=nullptr;
static lv_obj_t *level_lbl=nullptr, *level_bubble=nullptr;
static lv_obj_t *sun_lbl=nullptr;
static float g_marked_heading = -1.0f;

static void add_back_pill(lv_obj_t *p) {
    lv_obj_t *back = lv_btn_create(p);
    lv_obj_set_size(back, 200, 42);
    lv_obj_align(back, LV_ALIGN_BOTTOM_MID, 0, -16);
    lv_obj_set_style_bg_color(back, lv_color_hex(0x141414), 0);
    lv_obj_set_style_border_color(back, lv_color_hex(COL_ACCENT), 0);
    lv_obj_set_style_border_width(back, 1, 0);
    lv_obj_set_style_radius(back, 22, 0);
    lv_obj_t *l = lv_label_create(back);
    lv_label_set_text(l, LV_SYMBOL_LEFT "  Back to Launcher");
    lv_obj_set_style_text_color(l, lv_color_hex(COL_ACCENT), 0);
    lv_obj_set_style_text_font(l, &lv_font_montserrat_14, 0);
    lv_obj_center(l);
    lv_obj_add_event_cb(back, [](lv_event_t*){ return_to_launcher(); }, LV_EVENT_CLICKED, NULL);
}

static const char *card_for(float h) {
    static const char *cards[] = {"N","NE","E","SE","S","SW","W","NW"};
    int idx = ((int)((h + 22.5f) / 45.0f)) % 8;
    return cards[idx];
}

static void mark_btn_cb(lv_event_t *) {
    g_marked_heading = tilt_comp_heading();
    Preferences p; p.begin("navos", false); p.putFloat("mark", g_marked_heading); p.end();
    File f = SD_MMC.open("/WATCH/nav/bearings.txt", FILE_APPEND);
    if (f) { time_t t; time(&t); char b[48]; snprintf(b,48,"%lu,%.1f\n", (unsigned long)t, g_marked_heading); f.print(b); f.close(); }
}

static void build_compass() {
    root[0] = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(root[0], lv_color_hex(COL_BG), 0);
    lv_obj_set_style_bg_opa(root[0], LV_OPA_COVER, 0);

    lv_obj_t *t = lv_label_create(root[0]);
    lv_label_set_text(t, "COMPASS"); lv_obj_set_style_text_color(t, lv_color_hex(COL_MUTE),0);
    lv_obj_set_style_text_font(t, &lv_font_montserrat_12, 0); lv_obj_set_style_text_letter_space(t,3,0);
    lv_obj_align(t, LV_ALIGN_TOP_MID, 0, 24);

    hdg_lbl = lv_label_create(root[0]);
    lv_label_set_text(hdg_lbl, "---°");
    lv_obj_set_style_text_color(hdg_lbl, lv_color_hex(COL_ACCENT), 0);
    lv_obj_set_style_text_font(hdg_lbl, &lv_font_montserrat_48, 0);
    lv_obj_align(hdg_lbl, LV_ALIGN_CENTER, 0, -30);

    card_lbl = lv_label_create(root[0]);
    lv_label_set_text(card_lbl, "-");
    lv_obj_set_style_text_color(card_lbl, lv_color_hex(COL_TXT), 0);
    lv_obj_set_style_text_font(card_lbl, &lv_font_montserrat_28, 0);
    lv_obj_align(card_lbl, LV_ALIGN_CENTER, 0, 30);

    lv_obj_t *mb = lv_btn_create(root[0]);
    lv_obj_set_size(mb, 120, 40);
    lv_obj_align(mb, LV_ALIGN_BOTTOM_MID, 0, -70);
    lv_obj_set_style_bg_color(mb, lv_color_hex(COL_GOLD), 0);
    lv_obj_set_style_radius(mb, 20, 0);
    lv_obj_t *ml = lv_label_create(mb); lv_label_set_text(ml, "MARK");
    lv_obj_set_style_text_color(ml, lv_color_hex(0x000000),0);
    lv_obj_set_style_text_font(ml, &lv_font_montserrat_16, 0);
    lv_obj_center(ml);
    lv_obj_add_event_cb(mb, mark_btn_cb, LV_EVENT_CLICKED, NULL);

    add_back_pill(root[0]);
}
static void build_bearings() {
    root[1] = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(root[1], lv_color_hex(COL_BG), 0);
    lv_obj_set_style_bg_opa(root[1], LV_OPA_COVER, 0);
    lv_obj_t *t = lv_label_create(root[1]);
    lv_label_set_text(t, "BEARINGS"); lv_obj_set_style_text_color(t, lv_color_hex(COL_MUTE),0);
    lv_obj_set_style_text_font(t, &lv_font_montserrat_12, 0); lv_obj_set_style_text_letter_space(t,3,0);
    lv_obj_align(t, LV_ALIGN_TOP_MID, 0, 24);

    bear_list = lv_label_create(root[1]);
    lv_label_set_text(bear_list, "(none)");
    lv_obj_set_style_text_color(bear_list, lv_color_hex(COL_TXT), 0);
    lv_obj_set_style_text_font(bear_list, &lv_font_montserrat_14, 0);
    lv_obj_align(bear_list, LV_ALIGN_TOP_MID, 0, 60);
    lv_label_set_long_mode(bear_list, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(bear_list, 360);
    add_back_pill(root[1]);
}
static void build_level() {
    root[2] = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(root[2], lv_color_hex(COL_BG), 0);
    lv_obj_set_style_bg_opa(root[2], LV_OPA_COVER, 0);
    lv_obj_t *t = lv_label_create(root[2]);
    lv_label_set_text(t, "LEVEL"); lv_obj_set_style_text_color(t, lv_color_hex(COL_MUTE),0);
    lv_obj_set_style_text_font(t, &lv_font_montserrat_12, 0); lv_obj_set_style_text_letter_space(t,3,0);
    lv_obj_align(t, LV_ALIGN_TOP_MID, 0, 24);

    // big crosshair frame
    lv_obj_t *box = lv_obj_create(root[2]);
    lv_obj_set_size(box, 280, 280);
    lv_obj_align(box, LV_ALIGN_CENTER, 0, -20);
    lv_obj_set_style_bg_color(box, lv_color_hex(0x080808), 0);
    lv_obj_set_style_border_color(box, lv_color_hex(0x222222), 0);
    lv_obj_set_style_border_width(box, 1, 0);
    lv_obj_set_style_radius(box, 140, 0);

    level_bubble = lv_obj_create(box);
    lv_obj_set_size(level_bubble, 36, 36);
    lv_obj_align(level_bubble, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(level_bubble, lv_color_hex(COL_GOLD), 0);
    lv_obj_set_style_radius(level_bubble, 18, 0);
    lv_obj_set_style_border_width(level_bubble, 0, 0);

    level_lbl = lv_label_create(root[2]);
    lv_label_set_text(level_lbl, "0.0°  0.0°");
    lv_obj_set_style_text_color(level_lbl, lv_color_hex(COL_ACCENT), 0);
    lv_obj_set_style_text_font(level_lbl, &lv_font_montserrat_16, 0);
    lv_obj_align(level_lbl, LV_ALIGN_BOTTOM_MID, 0, -78);

    add_back_pill(root[2]);
}
static void build_sun() {
    root[3] = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(root[3], lv_color_hex(COL_BG), 0);
    lv_obj_set_style_bg_opa(root[3], LV_OPA_COVER, 0);
    lv_obj_t *t = lv_label_create(root[3]);
    lv_label_set_text(t, "SUN POSITION"); lv_obj_set_style_text_color(t, lv_color_hex(COL_MUTE),0);
    lv_obj_set_style_text_font(t, &lv_font_montserrat_12, 0); lv_obj_set_style_text_letter_space(t,3,0);
    lv_obj_align(t, LV_ALIGN_TOP_MID, 0, 24);

    sun_lbl = lv_label_create(root[3]);
    lv_label_set_text(sun_lbl, "az: ---°\nel: ---°");
    lv_obj_set_style_text_color(sun_lbl, lv_color_hex(COL_GOLD), 0);
    lv_obj_set_style_text_font(sun_lbl, &lv_font_montserrat_22, 0);
    lv_obj_align(sun_lbl, LV_ALIGN_CENTER, 0, -20);

    lv_obj_t *hint = lv_label_create(root[3]);
    lv_label_set_text(hint, "set lat/lon in /WATCH/nav/loc.txt");
    lv_obj_set_style_text_color(hint, lv_color_hex(COL_MUTE), 0);
    lv_obj_set_style_text_font(hint, &lv_font_montserrat_12, 0);
    lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -68);

    add_back_pill(root[3]);
}

// Simple sun-position from NOAA approximate algorithm
static float g_lat = 0, g_lon = 0;
static void read_loc() {
    File f = SD_MMC.open("/WATCH/nav/loc.txt", FILE_READ);
    if (!f) return;
    String s = f.readString(); f.close();
    int c = s.indexOf(','); if (c<0) return;
    g_lat = s.substring(0, c).toFloat();
    g_lon = s.substring(c+1).toFloat();
}
static void sun_position(float *az, float *el) {
    time_t now; time(&now);
    if (now < 1700000000) { *az=*el=-1; return; }
    struct tm tm; gmtime_r(&now, &tm);
    int doy = tm.tm_yday + 1;
    float hh = tm.tm_hour + tm.tm_min/60.0f + tm.tm_sec/3600.0f;
    float decl = 23.45f * sinf((360.0f/365.0f) * (doy - 81) * (float)M_PI/180.0f);
    float ha = (hh - 12) * 15.0f + g_lon;
    float lat_r = g_lat * (float)M_PI/180.0f;
    float decl_r = decl * (float)M_PI/180.0f;
    float ha_r = ha * (float)M_PI/180.0f;
    float sin_el = sinf(lat_r)*sinf(decl_r) + cosf(lat_r)*cosf(decl_r)*cosf(ha_r);
    float el_r = asinf(sin_el);
    float az_r = atan2f(-sinf(ha_r)*cosf(decl_r), cosf(lat_r)*sinf(decl_r) - sinf(lat_r)*cosf(decl_r)*cosf(ha_r));
    *az = fmodf(az_r * 180.0f/(float)M_PI + 360.0f, 360.0f);
    *el = el_r * 180.0f/(float)M_PI;
}

static void show_screen(int s) {
    if (s<0) s=3; if (s>3) s=0;
    g_screen = s;
    lv_scr_load_anim(root[s], LV_SCR_LOAD_ANIM_FADE_ON, 120, 0, false);
}

void setup() {
    launcher_app_boot();
    splash_show();
    // Guarantee /WATCH and its subdirs exist (works on a brand-new SD card)
    SD_MMC.mkdir("/WATCH");
    SD_MMC.mkdir("/WATCH/bin");
    SD_MMC.mkdir("/WATCH/wifi");
    SD_MMC.mkdir("/WATCH/nav");
    time_restore_from_launcher();
    read_loc();
    imu_init();
    // restore calibration + last marked heading
    Preferences p; p.begin("navos", true);
    g_mx_off = p.getFloat("mxo", 0); g_my_off = p.getFloat("myo", 0); g_mz_off = p.getFloat("mzo", 0);
    g_mx_scl = p.getFloat("mxs", 1); g_my_scl = p.getFloat("mys", 1); g_mz_scl = p.getFloat("mzs", 1);
    g_marked_heading = p.getFloat("mark", -1);
    p.end();

    build_compass(); build_bearings(); build_level(); build_sun();
    lv_scr_load(root[0]);
}

void loop() {
    lv_timer_handler();
    uint32_t now = millis();
    // BOOT-button safety exit
    static uint32_t btn_t = 0;
    if (digitalRead(BOOT_BTN_PIN) == LOW) {
        if (!btn_t) btn_t = now;
        else if (now - btn_t > 1500) return_to_launcher();
    } else btn_t = 0;

    // swipe nav
    if (g_pending_swipe == SW_LEFT)  { g_pending_swipe = SW_NONE; show_screen(g_screen+1); }
    if (g_pending_swipe == SW_RIGHT) { g_pending_swipe = SW_NONE; show_screen(g_screen-1); }

    static uint32_t imu_t = 0;
    if (now - imu_t > 80) {
        imu_t = now;
        imu_read();
        if (g_screen == 0 && hdg_lbl) {
            float h = tilt_comp_heading();
            char b[16]; snprintf(b,16,"%d°", (int)(h+0.5f));
            lv_label_set_text(hdg_lbl, b);
            lv_label_set_text(card_lbl, card_for(h));
            // tint towards gold if near marked heading
            if (g_marked_heading > 0) {
                float d = fabsf(h - g_marked_heading);
                if (d > 180) d = 360 - d;
                uint32_t c = (d < 5) ? COL_GOLD : COL_ACCENT;
                lv_obj_set_style_text_color(hdg_lbl, lv_color_hex(c), 0);
            }
        }
        if (g_screen == 2 && level_bubble) {
            float roll  = atan2f(g_ay, g_az) * 180.0f / (float)M_PI;
            float pitch = atan2f(-g_ax, sqrtf(g_ay*g_ay+g_az*g_az)) * 180.0f / (float)M_PI;
            int bx = (int)(roll * 2.5f); if (bx>120)bx=120; if (bx<-120)bx=-120;
            int by = (int)(pitch * 2.5f); if (by>120)by=120; if (by<-120)by=-120;
            lv_obj_align(level_bubble, LV_ALIGN_CENTER, bx, by);
            char b[32]; snprintf(b,32,"%+.1f°  %+.1f°", roll, pitch);
            lv_label_set_text(level_lbl, b);
        }
    }
    static uint32_t sun_t = 0;
    if (g_screen == 3 && sun_lbl && now - sun_t > 2000) {
        sun_t = now; float az, el; sun_position(&az, &el);
        char b[40];
        if (az < 0) snprintf(b,40,"no time sync yet");
        else snprintf(b,40,"az: %d°\nel: %+d°", (int)(az+0.5f), (int)(el+0.5f));
        lv_label_set_text(sun_lbl, b);
    }
    delay(5);
}
