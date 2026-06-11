// =============================================================================
//  UltraOS.ino v1.0 -- Apple Watch Ultra-class OS for LauncherOS
//  Target  : Waveshare ESP32-S3 Touch AMOLED 2.06  (410 x 502, CO5300, FT3168)
//  Compile : ESP32-S3 Dev Module / PSRAM=OPI / Flash=16MB / Minimal SPIFFS (large APP)
//  Export  : Sketch -> Export Compiled Binary  ->  copy UltraOS.ino.bin to
//            /WATCH/bin/ultraos.bin on the SD card.
// -----------------------------------------------------------------------------
//  Features:
//   - Multiple Ultra-style watch faces (Wayfinder, Modular, Solar)
//   - Activity rings (Move, Exercise, Stand) with IMU step counter
//   - Stopwatch / Timer / Alarm
//   - Weather widget (Wi-Fi NTP + temp simulation)
//   - Battery stats (voltage, %, charging state)
//   - Sunrise/Sunset calculator
//   - Moon phase display
//   - Flashlight mode (600 nit white)
//   - Calculator
//   - Notes app
//   - Silent / DnD / Theater modes
//   - Always-On Display with burn-in protection
//   - Battery stats screen
//   - Back to Launcher (BOOT button hold ~1.2s + UI button)
// =============================================================================

// ===== BEGIN SHARED BOOT BLOCK (LauncherOS v6.1+ compatible) =================
#include <Arduino.h>
#include <FS.h>
#include <SD_MMC.h>
#include <Wire.h>
#include <WiFi.h>
#include <HTTPClient.h>
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

#define HC_WIFI_SSID "UltraWatch"
#define HC_WIFI_PASS "ultra1234"

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
        // Enable hardware power-button IRQs so we can react to PEKEY events
        g_pmu.disableIRQ(XPOWERS_AXP2101_ALL_IRQ);
        g_pmu.enableIRQ(XPOWERS_AXP2101_PKEY_SHORT_IRQ |
                        XPOWERS_AXP2101_PKEY_LONG_IRQ);
        g_pmu.clearIrqStatus();
    }
}

// ---- Back-to-launcher (mandatory) -------------------------------------------
// Flip the boot partition to the `factory` app (where LauncherOS lives) and
// restart. This is the always-works escape hatch — no SD card required.
static void return_to_launcher() {
    Serial.println("[ULTRA] return_to_launcher() -> factory partition");
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
// ===== END SHARED BOOT BLOCK =================================================

// =============================================================================
//  POWER & DISPLAY MANAGEMENT
//   - PWR_BTN_PIN (GPIO 10) short press  -> toggle display on/off
//   - PWR_BTN_PIN (GPIO 10) hold >= 3s   -> deep sleep (true power off)
//   - AXP2101 PEKEY hardware events also handled (more reliable than GPIO poll)
//   - Any touch when off / wrist-raise / shake -> wake display
//   - Configurable inactivity auto-sleep (15/30/60/120/300 s or never)
//   - Always-On Display (AOD) shows dimmed time at 1 Hz instead of full off
//   - Battery-low overlay (<= 15 %) auto-shown over any screen
//   - Brightness + AOD + timeout persisted to NVS namespace "ultraos"
// =============================================================================
static Preferences g_uosprefs;
static uint8_t  g_brightness        = 200;     // 50..255
static bool     g_display_on        = true;
static bool     g_aod_active        = false;   // currently showing AOD
static bool     g_aod_enabled       = true;    // user preference (NVS)
static uint32_t g_last_activity_ms  = 0;
static uint32_t g_inactivity_ms     = 60000;   // user-configurable (NVS)
static uint8_t  g_timeout_idx       = 2;       // index into TIMEOUTS[]
static const uint32_t TIMEOUTS[] = {15000, 30000, 60000, 120000, 300000, 0xFFFFFFFF};
static const char *TIMEOUT_LABELS = "15 s\n30 s\n1 min\n2 min\n5 min\nNever";
static const uint32_t PWR_SHORT_MAX = 800;     // < 800 ms = short press
static const uint32_t PWR_LONG_MS   = 3000;    // >= 3 s   = deep sleep
static lv_obj_t *g_aod_screen = nullptr;       // tiny low-power face
static lv_obj_t *g_aod_time_lbl = nullptr;
static lv_obj_t *g_aod_date_lbl = nullptr;
static lv_obj_t *g_prev_screen = nullptr;      // screen to restore on wake
static lv_obj_t *g_batt_warn_overlay = nullptr;
static bool     g_batt_warned       = false;

static void display_apply_brightness(uint8_t v) {
    static_cast<Arduino_CO5300*>(gfx)->setBrightness(v);
}

static void prefs_load() {
    g_uosprefs.begin("ultraos", true);
    g_brightness  = g_uosprefs.getUChar("bright",  200);
    g_aod_enabled = g_uosprefs.getBool ("aod",     true);
    g_timeout_idx = g_uosprefs.getUChar("tmo_idx", 2);
    g_uosprefs.end();
    if (g_brightness < 50) g_brightness = 50;
    if (g_timeout_idx >= sizeof(TIMEOUTS)/sizeof(TIMEOUTS[0])) g_timeout_idx = 2;
    g_inactivity_ms = TIMEOUTS[g_timeout_idx];
    display_apply_brightness(g_brightness);
}

static void prefs_save_bright(uint8_t v) {
    g_brightness = v;
    g_uosprefs.begin("ultraos", false);
    g_uosprefs.putUChar("bright", v);
    g_uosprefs.end();
    display_apply_brightness(v);
}

static void prefs_save_aod(bool en) {
    g_aod_enabled = en;
    g_uosprefs.begin("ultraos", false);
    g_uosprefs.putBool("aod", en);
    g_uosprefs.end();
}

static void prefs_save_timeout(uint8_t idx) {
    if (idx >= sizeof(TIMEOUTS)/sizeof(TIMEOUTS[0])) return;
    g_timeout_idx = idx;
    g_inactivity_ms = TIMEOUTS[idx];
    g_uosprefs.begin("ultraos", false);
    g_uosprefs.putUChar("tmo_idx", idx);
    g_uosprefs.end();
}

// Forward decls removed — aod_update() now uses localtime() directly.

static void aod_build() {
    if (g_aod_screen) return;
    g_aod_screen = lv_obj_create(NULL);
    lv_obj_remove_style_all(g_aod_screen);
    lv_obj_set_size(g_aod_screen, LCD_WIDTH, LCD_HEIGHT);
    lv_obj_set_style_bg_color(g_aod_screen, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(g_aod_screen, LV_OPA_COVER, 0);

    g_aod_time_lbl = lv_label_create(g_aod_screen);
    lv_label_set_text(g_aod_time_lbl, "00:00");
    lv_obj_set_style_text_color(g_aod_time_lbl, lv_color_hex(0x444444), 0);
    lv_obj_set_style_text_font(g_aod_time_lbl, &lv_font_montserrat_48, 0);
    lv_obj_align(g_aod_time_lbl, LV_ALIGN_CENTER, 0, -10);

    g_aod_date_lbl = lv_label_create(g_aod_screen);
    lv_label_set_text(g_aod_date_lbl, "--- --");
    lv_obj_set_style_text_color(g_aod_date_lbl, lv_color_hex(0x2A2A2A), 0);
    lv_obj_set_style_text_font(g_aod_date_lbl, &lv_font_montserrat_16, 0);
    lv_obj_align(g_aod_date_lbl, LV_ALIGN_CENTER, 0, 40);
}

static void aod_update() {
    if (!g_aod_time_lbl) return;
    time_t now = time(nullptr);
    struct tm tm;
    localtime_r(&now, &tm);
    static const char *MONS[] = {"Jan","Feb","Mar","Apr","May","Jun","Jul","Aug","Sep","Oct","Nov","Dec"};
    char t[8];  snprintf(t, 8, "%02d:%02d", tm.tm_hour, tm.tm_min);
    lv_label_set_text(g_aod_time_lbl, t);
    char d[16]; snprintf(d, 16, "%s %d", MONS[tm.tm_mon % 12], tm.tm_mday);
    lv_label_set_text(g_aod_date_lbl, d);
}

static void display_sleep() {
    if (!g_display_on) return;
    Serial.println("[ULTRA] Display -> SLEEP");
    // Soft fade-out for a nicer feel
    for (int v = g_brightness; v >= 0; v -= 20) {
        display_apply_brightness((uint8_t)max(0, v));
        delay(15);
    }
    if (g_aod_enabled) {
        aod_build();
        g_prev_screen = lv_scr_act();
        lv_scr_load(g_aod_screen);
        aod_update();
        display_apply_brightness(8);  // ultra-dim AOD glow
        g_aod_active = true;
        g_display_on = false;
    } else {
        display_apply_brightness(0);
        g_display_on = false;
    }
    setCpuFrequencyMhz(80);  // throttle to save power
}

static void display_wake() {
    if (g_display_on) { g_last_activity_ms = millis(); return; }
    Serial.println("[ULTRA] Display -> WAKE");
    setCpuFrequencyMhz(160);
    if (g_aod_active && g_prev_screen) {
        lv_scr_load(g_prev_screen);
        g_aod_active = false;
    }
    // Fade-in
    for (int v = 0; v <= g_brightness; v += 20) {
        display_apply_brightness((uint8_t)min(255, v));
        delay(15);
    }
    display_apply_brightness(g_brightness);
    g_display_on = true;
    g_last_activity_ms = millis();
}

static void enter_deep_sleep() {
    Serial.println("[ULTRA] Entering DEEP SLEEP (PWR button to wake)");
    display_apply_brightness(0);
    delay(50);
    // Wake on PWR button press (active LOW with internal pull-up)
    esp_sleep_enable_ext0_wakeup((gpio_num_t)PWR_BTN_PIN, 0);
    esp_deep_sleep_start();
}

// AXP2101 PEKEY (hardware power button) — supplements the GPIO 10 poll.
// Returns: 0 = no event, 1 = short press, 2 = long press
static int pmu_pekey_event() {
    if (!g_has_pmu) return 0;
    int ev = 0;
    g_pmu.getIrqStatus();
    if (g_pmu.isPekeyShortPressIrq()) ev = 1;
    else if (g_pmu.isPekeyLongPressIrq()) ev = 2;
    g_pmu.clearIrqStatus();
    return ev;
}

// Shake / wrist-raise detection is inlined inside loop() where the IMU
// symbols (g_has_imu, qmi_read_accel) are already in scope.

static void battery_check_warning() {
    if (!g_has_pmu) return;
    int pct = g_pmu.getBatteryPercent();
    if (pct < 0) return;
    if (pct <= 15 && !g_batt_warned && g_display_on) {
        g_batt_warned = true;
        g_batt_warn_overlay = lv_obj_create(lv_scr_act());
        lv_obj_remove_style_all(g_batt_warn_overlay);
        lv_obj_set_size(g_batt_warn_overlay, 280, 110);
        lv_obj_align(g_batt_warn_overlay, LV_ALIGN_TOP_MID, 0, 70);
        lv_obj_set_style_bg_color(g_batt_warn_overlay, lv_color_hex(0x1C0000), 0);
        lv_obj_set_style_bg_opa(g_batt_warn_overlay, 240, 0);
        lv_obj_set_style_radius(g_batt_warn_overlay, 14, 0);
        lv_obj_set_style_border_color(g_batt_warn_overlay, lv_color_hex(0xFF453A), 0);
        lv_obj_set_style_border_width(g_batt_warn_overlay, 2, 0);
        lv_obj_t *lbl = lv_label_create(g_batt_warn_overlay);
        char b[48]; snprintf(b, 48, LV_SYMBOL_BATTERY_EMPTY "  Low Battery\n%d%% remaining", pct);
        lv_label_set_text(lbl, b);
        lv_obj_set_style_text_color(lbl, lv_color_hex(0xFF9F0A), 0);
        lv_obj_set_style_text_font(lbl, &lv_font_montserrat_22, 0);
        lv_obj_set_style_text_align(lbl, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_center(lbl);
        // Auto-dismiss after 4 s
        lv_timer_create([](lv_timer_t *t) {
            if (g_batt_warn_overlay) { lv_obj_del(g_batt_warn_overlay); g_batt_warn_overlay = nullptr; }
            lv_timer_del(t);
        }, 4000, NULL);
    }
    if (pct > 25) g_batt_warned = false;  // re-arm after recovery
}

// =============================================================================
//  QUICK SETTINGS PANEL — swipe down from top of watch face to open
//   Contents: brightness slider · AOD toggle · Lock · Flashlight ·
//             Back to Launcher · live battery %
// =============================================================================
static lv_obj_t *g_qs_panel = nullptr;
static void open_flashlight();  // fwd decl

static void quick_settings_close() {
    if (!g_qs_panel) return;
    lv_obj_del(g_qs_panel);
    g_qs_panel = nullptr;
}

static void quick_settings_open() {
    if (g_qs_panel) return;
    g_qs_panel = lv_obj_create(lv_scr_act());
    lv_obj_remove_style_all(g_qs_panel);
    lv_obj_set_size(g_qs_panel, LCD_WIDTH, 380);
    lv_obj_align(g_qs_panel, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_bg_color(g_qs_panel, lv_color_hex(0x0A0A0A), 0);
    lv_obj_set_style_bg_opa(g_qs_panel, 240, 0);
    lv_obj_set_style_radius(g_qs_panel, 24, 0);
    lv_obj_set_style_border_color(g_qs_panel, lv_color_hex(COL_ORANGE), 0);
    lv_obj_set_style_border_width(g_qs_panel, 1, 0);

    // Title row
    lv_obj_t *title = lv_label_create(g_qs_panel);
    lv_label_set_text(title, "CONTROL");
    lv_obj_set_style_text_color(title, lv_color_hex(COL_ORANGE), 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_22, 0);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 30, 25);

    // Battery indicator
    lv_obj_t *batt = lv_label_create(g_qs_panel);
    char b[16];
    int pct = g_has_pmu ? g_pmu.getBatteryPercent() : 0;
    snprintf(b, 16, LV_SYMBOL_BATTERY_FULL " %d%%", pct < 0 ? 0 : pct);
    lv_label_set_text(batt, b);
    lv_obj_set_style_text_color(batt, lv_color_hex(COL_GREEN), 0);
    lv_obj_set_style_text_font(batt, &lv_font_montserrat_18, 0);
    lv_obj_align(batt, LV_ALIGN_TOP_RIGHT, -30, 30);

    // Brightness slider
    lv_obj_t *bl = lv_label_create(g_qs_panel);
    lv_label_set_text(bl, LV_SYMBOL_EYE_OPEN "  Brightness");
    lv_obj_set_style_text_color(bl, lv_color_hex(COL_WHITE), 0);
    lv_obj_set_style_text_font(bl, &lv_font_montserrat_14, 0);
    lv_obj_align(bl, LV_ALIGN_TOP_LEFT, 30, 75);

    lv_obj_t *sl = lv_slider_create(g_qs_panel);
    lv_obj_set_size(sl, 320, 12);
    lv_obj_align(sl, LV_ALIGN_TOP_MID, 0, 105);
    lv_slider_set_range(sl, 50, 255);
    lv_slider_set_value(sl, g_brightness, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(sl, lv_color_hex(COL_PANEL),  LV_PART_MAIN);
    lv_obj_set_style_bg_color(sl, lv_color_hex(COL_ORANGE), LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(sl, lv_color_hex(COL_ORANGE), LV_PART_KNOB);
    lv_obj_add_event_cb(sl, [](lv_event_t *e) {
        prefs_save_bright((uint8_t)lv_slider_get_value(lv_event_get_target(e)));
    }, LV_EVENT_VALUE_CHANGED, NULL);

    // 4-button grid: AOD · Lock · Flashlight · Launcher
    struct QBtn { const char *lbl; const char *icon; uint32_t col; lv_event_cb_t cb; };
    auto cb_aod = [](lv_event_t*) { prefs_save_aod(!g_aod_enabled); };
    auto cb_lock = [](lv_event_t*) { quick_settings_close(); display_sleep(); };
    auto cb_torch = [](lv_event_t*) { quick_settings_close(); open_flashlight(); };
    auto cb_back  = [](lv_event_t*) { return_to_launcher(); };
    static QBtn buttons[4] = {
        {"AOD",        LV_SYMBOL_REFRESH, COL_BLUE,   cb_aod},
        {"Lock",       LV_SYMBOL_POWER,   COL_GRAY,   cb_lock},
        {"Flashlight", LV_SYMBOL_EYE_OPEN,COL_YELLOW, cb_torch},
        {"Launcher",   LV_SYMBOL_HOME,    COL_ORANGE, cb_back},
    };
    int bx[4] = {30, 130, 230, 30};
    int by[4] = {160, 160, 160, 270};
    int bw[4] = {90, 90, 150, 350};
    for (int i = 0; i < 4; i++) {
        lv_obj_t *btn = lv_btn_create(g_qs_panel);
        lv_obj_set_size(btn, bw[i], 80);
        lv_obj_set_pos(btn, bx[i], by[i]);
        lv_obj_set_style_bg_color(btn, lv_color_hex(0x141414), 0);
        lv_obj_set_style_border_color(btn, lv_color_hex(buttons[i].col), 0);
        lv_obj_set_style_border_width(btn, 1, 0);
        lv_obj_set_style_radius(btn, 16, 0);
        lv_obj_add_event_cb(btn, buttons[i].cb, LV_EVENT_CLICKED, NULL);

        lv_obj_t *ic = lv_label_create(btn);
        lv_label_set_text(ic, buttons[i].icon);
        lv_obj_set_style_text_color(ic, lv_color_hex(buttons[i].col), 0);
        lv_obj_set_style_text_font(ic, &lv_font_montserrat_22, 0);
        lv_obj_align(ic, i == 3 ? LV_ALIGN_LEFT_MID : LV_ALIGN_TOP_MID,
                     i == 3 ? 20 : 0, i == 3 ? 0 : 8);

        lv_obj_t *lb = lv_label_create(btn);
        lv_label_set_text(lb, buttons[i].lbl);
        lv_obj_set_style_text_color(lb, lv_color_hex(COL_WHITE), 0);
        lv_obj_set_style_text_font(lb, &lv_font_montserrat_14, 0);
        lv_obj_align(lb, i == 3 ? LV_ALIGN_CENTER : LV_ALIGN_BOTTOM_MID,
                     i == 3 ? 20 : 0, -6);
    }

    // Dismiss area below the panel (tap anywhere to close)
    lv_obj_t *dismiss = lv_btn_create(g_qs_panel);
    lv_obj_set_size(dismiss, LCD_WIDTH, 30);
    lv_obj_align(dismiss, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_opa(dismiss, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(dismiss, 0, 0);
    lv_obj_t *chev = lv_label_create(dismiss);
    lv_label_set_text(chev, LV_SYMBOL_UP);
    lv_obj_set_style_text_color(chev, lv_color_hex(COL_GRAY), 0);
    lv_obj_center(chev);
    lv_obj_add_event_cb(dismiss, [](lv_event_t*) { quick_settings_close(); },
                        LV_EVENT_CLICKED, NULL);
}

// =============================================================================
//  COLOR PALETTE (Apple Watch Ultra inspired)
// =============================================================================
#define COL_BG           0x000000
#define COL_PANEL        0x1C1C1C
#define COL_PANEL_LIGHT  0x2C2C2C
#define COL_ORANGE       0xFF9F0A    // Ultra signature orange
#define COL_BLUE         0x0A84FF
#define COL_GREEN        0x30D158
#define COL_RED          0xFF453A
#define COL_CYAN         0x64D2FF
#define COL_YELLOW       0xFFD60A
#define COL_PINK         0xFF375F
#define COL_PURPLE       0xBF5AF2
#define COL_WHITE        0xFFFFFF
#define COL_GRAY         0x8E8E93
#define COL_TEXT_DIM     0x636366

// =============================================================================
//  QMI8658 IMU (Step Counter)
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
            if (who == 0x05) {
                qmi_write_reg(0x02, 0x60);  // CTRL1
                qmi_write_reg(0x03, 0x95);  // CTRL2: accel 2g, ODR 125Hz
                qmi_write_reg(0x05, 0x55);  // CTRL4: gyro ±512 dps, ODR 125Hz
                qmi_write_reg(0x08, 0x03);  // CTRL7: enable accel + gyro
                g_has_imu = true;
                Serial.printf("[IMU] QMI8658 @ 0x%02X\n", a);
                return;
            }
        }
    }
}

static void qmi_read_accel(float &ax, float &ay, float &az) {
    if (!g_has_imu) { ax = ay = az = 0; return; }
    uint8_t b[6];
    if (!qmi_read(0x35, b, 6)) { ax = ay = az = 0; return; }
    int16_t raw_x = (int16_t)((b[1] << 8) | b[0]);
    int16_t raw_y = (int16_t)((b[3] << 8) | b[2]);
    int16_t raw_z = (int16_t)((b[5] << 8) | b[4]);
    ax = raw_x / 16384.0f;
    ay = raw_y / 16384.0f;
    az = raw_z / 16384.0f;
}

static void qmi_read_gyro(float &gx, float &gy, float &gz) {
    if (!g_has_imu) { gx = gy = gz = 0; return; }
    uint8_t b[6];
    if (!qmi_read(0x3B, b, 6)) { gx = gy = gz = 0; return; }
    int16_t raw_x = (int16_t)((b[1] << 8) | b[0]);
    int16_t raw_y = (int16_t)((b[3] << 8) | b[2]);
    int16_t raw_z = (int16_t)((b[5] << 8) | b[4]);
    gx = raw_x / 64.0f;  // ±512 dps scale
    gy = raw_y / 64.0f;
    gz = raw_z / 64.0f;
}

// =============================================================================
//  ACTIVITY TRACKING (Step Counter)
// =============================================================================
static int     g_steps = 0;
static int     g_move_calories = 0;
static int     g_exercise_minutes = 0;
static int     g_stand_hours = 0;
static float   g_last_ax = 0, g_last_ay = 0, g_last_az = 0;
static uint32_t g_last_step_time = 0;

static void step_counter_update() {
    if (!g_has_imu) return;
    float ax, ay, az;
    qmi_read_accel(ax, ay, az);
    
    // Simple step detection: magnitude delta threshold
    float mag = sqrtf(ax*ax + ay*ay + az*az);
    float last_mag = sqrtf(g_last_ax*g_last_ax + g_last_ay*g_last_ay + g_last_az*g_last_az);
    float delta = fabsf(mag - last_mag);
    
    uint32_t now = millis();
    if (delta > 0.3f && (now - g_last_step_time) > 300) {  // 300ms debounce
        g_steps++;
        g_last_step_time = now;
        // Rough calories: 0.04 per step
        g_move_calories = (int)(g_steps * 0.04f);
    }
    
    g_last_ax = ax; g_last_ay = ay; g_last_az = az;
}

// =============================================================================
//  TIME & DATE
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
}

static void wifi_load_and_sync() {
    WiFi.mode(WIFI_STA);
    WiFi.begin(HC_WIFI_SSID, HC_WIFI_PASS);
    uint32_t t0 = millis();
    while (millis() - t0 < 4000) {
        if (WiFi.status() == WL_CONNECTED) {
            configTime(g_gmt_offset_sec, 0, "pool.ntp.org");
            delay(800);
            struct tm tm;
            if (getLocalTime(&tm, 200)) {
                g_time_valid = true;
            }
            WiFi.disconnect(true, true);
            WiFi.mode(WIFI_OFF);
            return;
        }
        delay(120);
    }
    WiFi.disconnect(true, true);
    WiFi.mode(WIFI_OFF);
}

// =============================================================================
//  SUNRISE / SUNSET CALCULATOR
// =============================================================================
static float g_latitude = 37.7749f;   // San Francisco default
static float g_longitude = -122.4194f;

static float sun_julian_day(int y, int m, int d) {
    if (m <= 2) { y--; m += 12; }
    int A = y / 100;
    int B = 2 - A + (A / 4);
    return (int)(365.25 * (y + 4716)) + (int)(30.6001 * (m + 1)) + d + B - 1524.5;
}

static void calc_sunrise_sunset(int &rise_h, int &rise_m, int &set_h, int &set_m) {
    struct tm tm;
    if (!getLocalTime(&tm, 5)) { rise_h = 6; rise_m = 0; set_h = 18; set_m = 0; return; }
    
    float jd = sun_julian_day(tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday);
    float n = jd - 2451545.0 + 0.0008;
    float J_star = n - g_longitude / 360.0;
    float M = fmod(357.5291 + 0.98560028 * J_star, 360.0);
    float C = 1.9148 * sin(M * PI / 180.0) + 0.02 * sin(2 * M * PI / 180.0);
    float lambda = fmod(M + C + 180.0 + 102.9372, 360.0);
    float J_transit = 2451545.0 + J_star + 0.0053 * sin(M * PI / 180.0) - 0.0069 * sin(2 * lambda * PI / 180.0);
    float delta = asin(sin(lambda * PI / 180.0) * sin(23.44 * PI / 180.0));
    float cos_omega = (sin(-0.833 * PI / 180.0) - sin(g_latitude * PI / 180.0) * sin(delta)) / (cos(g_latitude * PI / 180.0) * cos(delta));
    if (cos_omega < -1 || cos_omega > 1) { rise_h = 6; rise_m = 0; set_h = 18; set_m = 0; return; }
    float omega = acos(cos_omega) * 180.0 / PI;
    
    float J_rise = J_transit - omega / 360.0;
    float J_set  = J_transit + omega / 360.0;
    
    float rise_frac = (J_rise - (int)J_rise - 0.5) * 24.0 + g_gmt_offset_sec / 3600.0;
    float set_frac  = (J_set  - (int)J_set  - 0.5) * 24.0 + g_gmt_offset_sec / 3600.0;
    
    if (rise_frac < 0) rise_frac += 24;
    if (set_frac < 0) set_frac += 24;
    
    rise_h = (int)rise_frac; rise_m = (int)((rise_frac - rise_h) * 60);
    set_h  = (int)set_frac;  set_m  = (int)((set_frac  - set_h)  * 60);
}

// =============================================================================
//  MOON PHASE
// =============================================================================
static int moon_phase_day() {
    struct tm tm;
    if (!getLocalTime(&tm, 5)) return 0;
    int y = tm.tm_year + 1900, m = tm.tm_mon + 1, d = tm.tm_mday;
    if (m < 3) { y--; m += 12; }
    int a = y / 100;
    int b = a / 4;
    int c = 2 - a + b;
    int e = (int)(365.25 * (y + 4716));
    int f = (int)(30.6001 * (m + 1));
    float jd = c + d + e + f - 1524.5;
    float days_since_new = jd - 2451549.5;
    float phase = fmod(days_since_new, 29.53);
    return (int)phase;
}

// =============================================================================
//  SPLASH SCREEN (Ultra-style)
// =============================================================================
static void splash_show() {
    lv_obj_t *s = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s, lv_color_hex(COL_BG), 0);
    lv_obj_set_style_bg_opa(s, LV_OPA_COVER, 0);
    
    // Orange accent circle
    lv_obj_t *circle = lv_obj_create(s);
    lv_obj_remove_style_all(circle);
    lv_obj_set_size(circle, 160, 160);
    lv_obj_align(circle, LV_ALIGN_CENTER, 0, -40);
    lv_obj_set_style_radius(circle, 80, 0);
    lv_obj_set_style_bg_color(circle, lv_color_hex(COL_ORANGE), 0);
    lv_obj_set_style_bg_opa(circle, 60, 0);
    lv_obj_set_style_border_color(circle, lv_color_hex(COL_ORANGE), 0);
    lv_obj_set_style_border_width(circle, 3, 0);
    
    // Title
    lv_obj_t *title = lv_label_create(s);
    lv_label_set_text(title, "ULTRA");
    lv_obj_set_style_text_color(title, lv_color_hex(COL_ORANGE), 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_48, 0);
    lv_obj_set_style_text_letter_space(title, 8, 0);
    lv_obj_align(title, LV_ALIGN_CENTER, 0, -45);
    
    // Subtitle
    lv_obj_t *sub = lv_label_create(s);
    lv_label_set_text(sub, "WATCH OS");
    lv_obj_set_style_text_color(sub, lv_color_hex(COL_WHITE), 0);
    lv_obj_set_style_text_font(sub, &lv_font_montserrat_18, 0);
    lv_obj_set_style_text_letter_space(sub, 4, 0);
    lv_obj_align(sub, LV_ALIGN_CENTER, 0, 10);
    
    // Progress bar
    lv_obj_t *bar = lv_bar_create(s);
    lv_obj_set_size(bar, 280, 4);
    lv_obj_align(bar, LV_ALIGN_CENTER, 0, 80);
    lv_obj_set_style_bg_color(bar, lv_color_hex(COL_PANEL), LV_PART_MAIN);
    lv_obj_set_style_bg_color(bar, lv_color_hex(COL_ORANGE), LV_PART_INDICATOR);
    lv_bar_set_range(bar, 0, 100);
    
    lv_scr_load(s);
    
    // Animate loading
    for (int p = 0; p <= 100; p += 5) {
        lv_bar_set_value(bar, p, LV_ANIM_OFF);
        lv_timer_handler();
        delay(35);
    }
    
    delay(300);
    lv_obj_del(s);
}

// =============================================================================
//  WATCH FACES
// =============================================================================
static lv_obj_t *g_face_screen = nullptr;
static lv_obj_t *g_time_label = nullptr;
static lv_obj_t *g_date_label = nullptr;
static lv_obj_t *g_activity_arc[3] = {nullptr, nullptr, nullptr};
static lv_obj_t *g_face_complication[4] = {nullptr, nullptr, nullptr, nullptr};
static int g_current_face = 0;  // 0=Modular, 1=Wayfinder, 2=Solar Dial, 3=Digital Bold
#define FACE_COUNT 4
static const char *FACE_NAMES[FACE_COUNT] = {"Modular", "Wayfinder", "Solar Dial", "Digital Bold"};

static void face_clear_widgets() {
    if (g_time_label) { lv_obj_del(g_time_label); g_time_label = nullptr; }
    if (g_date_label) { lv_obj_del(g_date_label); g_date_label = nullptr; }
    for (int i = 0; i < 3; i++) {
        if (g_activity_arc[i]) { lv_obj_del(g_activity_arc[i]); g_activity_arc[i] = nullptr; }
    }
    for (int i = 0; i < 4; i++) {
        if (g_face_complication[i]) { lv_obj_del(g_face_complication[i]); g_face_complication[i] = nullptr; }
    }
    // Wipe any extra children from previous face
    lv_obj_clean(g_face_screen);
}

static void build_modular_face() {
    face_clear_widgets();
    // Central time
    g_time_label = lv_label_create(g_face_screen);
    lv_label_set_text(g_time_label, "12:00");
    lv_obj_set_style_text_color(g_time_label, lv_color_hex(COL_WHITE), 0);
    lv_obj_set_style_text_font(g_time_label, &lv_font_montserrat_48, 0);
    lv_obj_align(g_time_label, LV_ALIGN_CENTER, 0, -80);
    
    // Date
    g_date_label = lv_label_create(g_face_screen);
    lv_label_set_text(g_date_label, "MON 1 JAN");
    lv_obj_set_style_text_color(g_date_label, lv_color_hex(COL_ORANGE), 0);
    lv_obj_set_style_text_font(g_date_label, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_letter_space(g_date_label, 2, 0);
    lv_obj_align(g_date_label, LV_ALIGN_CENTER, 0, -25);
    
    // Activity rings
    for (int i = 0; i < 3; i++) {
        g_activity_arc[i] = lv_arc_create(g_face_screen);
        int radius = 140 - (i * 25);
        lv_obj_set_size(g_activity_arc[i], radius, radius);
        lv_obj_align(g_activity_arc[i], LV_ALIGN_CENTER, 0, 80);
        lv_arc_set_rotation(g_activity_arc[i], 270);
        lv_arc_set_bg_angles(g_activity_arc[i], 0, 360);
        lv_arc_set_range(g_activity_arc[i], 0, 100);
        lv_arc_set_value(g_activity_arc[i], 0);
        lv_obj_remove_style(g_activity_arc[i], NULL, LV_PART_KNOB);
        lv_obj_clear_flag(g_activity_arc[i], LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_style_arc_width(g_activity_arc[i], 12, LV_PART_MAIN);
        lv_obj_set_style_arc_width(g_activity_arc[i], 12, LV_PART_INDICATOR);
        lv_obj_set_style_arc_color(g_activity_arc[i], lv_color_hex(0x1C1C1C), LV_PART_MAIN);
        
        uint32_t col = (i == 0) ? COL_PINK : (i == 1) ? COL_GREEN : COL_CYAN;
        lv_obj_set_style_arc_color(g_activity_arc[i], lv_color_hex(col), LV_PART_INDICATOR);
    }
    
    // Complications (corners)
    const char *comp_labels[] = {"STEPS", "BATTERY", "SUNRISE", "MOON"};
    int comp_pos[][2] = {{-120, -180}, {120, -180}, {-120, 200}, {120, 200}};
    
    for (int i = 0; i < 4; i++) {
        g_face_complication[i] = lv_label_create(g_face_screen);
        lv_label_set_text(g_face_complication[i], comp_labels[i]);
        lv_obj_set_style_text_color(g_face_complication[i], lv_color_hex(COL_GRAY), 0);
        lv_obj_set_style_text_font(g_face_complication[i], &lv_font_montserrat_12, 0);
        lv_obj_align(g_face_complication[i], LV_ALIGN_CENTER, comp_pos[i][0], comp_pos[i][1]);
    }
}

// =============================================================================
//  WAYFINDER FACE (Ultra Signature!)
// =============================================================================
static void build_wayfinder_face() {
    face_clear_widgets();
    
    // Outer ring (compass-style)
    lv_obj_t *ring = lv_obj_create(g_face_screen);
    lv_obj_set_size(ring, 380, 380);
    lv_obj_align(ring, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_radius(ring, 190, 0);
    lv_obj_set_style_border_width(ring, 4, 0);
    lv_obj_set_style_border_color(ring, lv_color_hex(COL_ORANGE), 0);
    lv_obj_set_style_bg_opa(ring, LV_OPA_TRANSP, 0);
    
    // Cardinal markers
    const char *cardinals[] = {"N", "E", "S", "W"};
    int card_x[] = {0, 160, 0, -160};
    int card_y[] = {-180, 0, 180, 0};
    for (int i = 0; i < 4; i++) {
        lv_obj_t *mark = lv_label_create(g_face_screen);
        lv_label_set_text(mark, cardinals[i]);
        lv_obj_set_style_text_color(mark, lv_color_hex(i == 0 ? COL_ORANGE : COL_GRAY), 0);
        lv_obj_set_style_text_font(mark, &lv_font_montserrat_22, 0);
        lv_obj_align(mark, LV_ALIGN_CENTER, card_x[i], card_y[i]);
    }
    
    // Central time (large)
    g_time_label = lv_label_create(g_face_screen);
    lv_label_set_text(g_time_label, "12:00");
    lv_obj_set_style_text_color(g_time_label, lv_color_hex(COL_WHITE), 0);
    lv_obj_set_style_text_font(g_time_label, &lv_font_montserrat_48, 0);
    lv_obj_align(g_time_label, LV_ALIGN_CENTER, 0, -20);
    
    // Date below
    g_date_label = lv_label_create(g_face_screen);
    lv_label_set_text(g_date_label, "MON 1");
    lv_obj_set_style_text_color(g_date_label, lv_color_hex(COL_ORANGE), 0);
    lv_obj_set_style_text_font(g_date_label, &lv_font_montserrat_18, 0);
    lv_obj_align(g_date_label, LV_ALIGN_CENTER, 0, 30);
    
    // Degree markers
    for (int i = 0; i < 12; i++) {
        lv_obj_t *tick = lv_obj_create(g_face_screen);
        lv_obj_set_size(tick, 3, 15);
        lv_obj_set_style_bg_color(tick, lv_color_hex(COL_ORANGE), 0);
        lv_obj_set_style_border_width(tick, 0, 0);
        float angle = (i * 30) * PI / 180.0f;
        int x = (int)(sin(angle) * 180);
        int y = (int)(-cos(angle) * 180);
        lv_obj_set_pos(tick, 205 + x - 1, 251 + y - 7);
    }
    
    // Bottom complications
    g_face_complication[0] = lv_label_create(g_face_screen);
    lv_label_set_text(g_face_complication[0], "0 m");
    lv_obj_set_style_text_color(g_face_complication[0], lv_color_hex(COL_BLUE), 0);
    lv_obj_set_style_text_font(g_face_complication[0], &lv_font_montserrat_14, 0);
    lv_obj_align(g_face_complication[0], LV_ALIGN_BOTTOM_LEFT, 30, -20);
    
    g_face_complication[1] = lv_label_create(g_face_screen);
    lv_label_set_text(g_face_complication[1], "100%");
    lv_obj_set_style_text_color(g_face_complication[1], lv_color_hex(COL_GREEN), 0);
    lv_obj_set_style_text_font(g_face_complication[1], &lv_font_montserrat_14, 0);
    lv_obj_align(g_face_complication[1], LV_ALIGN_BOTTOM_RIGHT, -30, -20);
}

// =============================================================================
//  SOLAR DIAL FACE (sunrise/sunset arc + time)
// =============================================================================
static void build_solar_dial_face() {
    face_clear_widgets();

    // Outer arc — golden sun ring
    lv_obj_t *sun_arc = lv_arc_create(g_face_screen);
    lv_obj_set_size(sun_arc, 380, 380);
    lv_obj_align(sun_arc, LV_ALIGN_CENTER, 0, 0);
    lv_arc_set_rotation(sun_arc, 90);
    lv_arc_set_bg_angles(sun_arc, 0, 180);
    lv_arc_set_angles(sun_arc, 0, 180);
    lv_obj_remove_style(sun_arc, NULL, LV_PART_KNOB);
    lv_obj_clear_flag(sun_arc, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_arc_width(sun_arc, 6, LV_PART_MAIN);
    lv_obj_set_style_arc_width(sun_arc, 6, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(sun_arc, lv_color_hex(0x332200), LV_PART_MAIN);
    lv_obj_set_style_arc_color(sun_arc, lv_color_hex(0xFFB300), LV_PART_INDICATOR);

    // Night arc bottom — deep blue
    lv_obj_t *night_arc = lv_arc_create(g_face_screen);
    lv_obj_set_size(night_arc, 380, 380);
    lv_obj_align(night_arc, LV_ALIGN_CENTER, 0, 0);
    lv_arc_set_rotation(night_arc, 270);
    lv_arc_set_bg_angles(night_arc, 0, 180);
    lv_arc_set_angles(night_arc, 0, 180);
    lv_obj_remove_style(night_arc, NULL, LV_PART_KNOB);
    lv_obj_clear_flag(night_arc, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_arc_width(night_arc, 6, LV_PART_MAIN);
    lv_obj_set_style_arc_width(night_arc, 6, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(night_arc, lv_color_hex(0x000033), LV_PART_MAIN);
    lv_obj_set_style_arc_color(night_arc, lv_color_hex(0x3355CC), LV_PART_INDICATOR);

    // Horizon line
    lv_obj_t *horizon = lv_obj_create(g_face_screen);
    lv_obj_set_size(horizon, 300, 2);
    lv_obj_align(horizon, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(horizon, lv_color_hex(0x666666), 0);
    lv_obj_set_style_border_width(horizon, 0, 0);

    // Central time
    g_time_label = lv_label_create(g_face_screen);
    lv_label_set_text(g_time_label, "12:00");
    lv_obj_set_style_text_color(g_time_label, lv_color_hex(0xFFD27F), 0);
    lv_obj_set_style_text_font(g_time_label, &lv_font_montserrat_48, 0);
    lv_obj_align(g_time_label, LV_ALIGN_CENTER, 0, -60);

    g_date_label = lv_label_create(g_face_screen);
    lv_label_set_text(g_date_label, "MON 1 JAN");
    lv_obj_set_style_text_color(g_date_label, lv_color_hex(COL_ORANGE), 0);
    lv_obj_set_style_text_font(g_date_label, &lv_font_montserrat_16, 0);
    lv_obj_align(g_date_label, LV_ALIGN_CENTER, 0, -10);

    // Sunrise / Sunset labels
    g_face_complication[2] = lv_label_create(g_face_screen);
    lv_label_set_text(g_face_complication[2], "06:00");
    lv_obj_set_style_text_color(g_face_complication[2], lv_color_hex(0xFFB300), 0);
    lv_obj_set_style_text_font(g_face_complication[2], &lv_font_montserrat_16, 0);
    lv_obj_align(g_face_complication[2], LV_ALIGN_CENTER, -140, 70);

    g_face_complication[0] = lv_label_create(g_face_screen);
    lv_label_set_text(g_face_complication[0], "STEPS 0");
    lv_obj_set_style_text_color(g_face_complication[0], lv_color_hex(COL_WHITE), 0);
    lv_obj_set_style_text_font(g_face_complication[0], &lv_font_montserrat_14, 0);
    lv_obj_align(g_face_complication[0], LV_ALIGN_CENTER, 0, 90);

    g_face_complication[1] = lv_label_create(g_face_screen);
    lv_label_set_text(g_face_complication[1], "100%");
    lv_obj_set_style_text_color(g_face_complication[1], lv_color_hex(COL_GREEN), 0);
    lv_obj_set_style_text_font(g_face_complication[1], &lv_font_montserrat_16, 0);
    lv_obj_align(g_face_complication[1], LV_ALIGN_CENTER, 140, 70);

    // Sun marker on arc (decorative)
    lv_obj_t *sun = lv_obj_create(g_face_screen);
    lv_obj_set_size(sun, 24, 24);
    lv_obj_set_style_radius(sun, 12, 0);
    lv_obj_set_style_bg_color(sun, lv_color_hex(0xFFD700), 0);
    lv_obj_set_style_border_width(sun, 0, 0);
    lv_obj_set_style_shadow_width(sun, 20, 0);
    lv_obj_set_style_shadow_color(sun, lv_color_hex(0xFFB300), 0);
    lv_obj_align(sun, LV_ALIGN_CENTER, 130, -130);
}

// =============================================================================
//  DIGITAL BOLD FACE (huge digital time, AMOLED-friendly)
// =============================================================================
static void build_digital_bold_face() {
    face_clear_widgets();

    // Huge HH
    g_time_label = lv_label_create(g_face_screen);
    lv_label_set_text(g_time_label, "12:00");
    lv_obj_set_style_text_color(g_time_label, lv_color_hex(COL_WHITE), 0);
    lv_obj_set_style_text_font(g_time_label, &lv_font_montserrat_48, 0);
    lv_obj_set_style_text_letter_space(g_time_label, 4, 0);
    lv_obj_align(g_time_label, LV_ALIGN_CENTER, 0, -40);

    // Big underline accent
    lv_obj_t *line = lv_obj_create(g_face_screen);
    lv_obj_set_size(line, 260, 4);
    lv_obj_align(line, LV_ALIGN_CENTER, 0, 20);
    lv_obj_set_style_bg_color(line, lv_color_hex(COL_ORANGE), 0);
    lv_obj_set_style_border_width(line, 0, 0);
    lv_obj_set_style_radius(line, 2, 0);

    // Date
    g_date_label = lv_label_create(g_face_screen);
    lv_label_set_text(g_date_label, "MON 1 JAN");
    lv_obj_set_style_text_color(g_date_label, lv_color_hex(COL_GRAY), 0);
    lv_obj_set_style_text_font(g_date_label, &lv_font_montserrat_22, 0);
    lv_obj_set_style_text_letter_space(g_date_label, 3, 0);
    lv_obj_align(g_date_label, LV_ALIGN_CENTER, 0, 50);

    // Battery complication
    g_face_complication[1] = lv_label_create(g_face_screen);
    lv_label_set_text(g_face_complication[1], "100%");
    lv_obj_set_style_text_color(g_face_complication[1], lv_color_hex(COL_GREEN), 0);
    lv_obj_set_style_text_font(g_face_complication[1], &lv_font_montserrat_18, 0);
    lv_obj_align(g_face_complication[1], LV_ALIGN_BOTTOM_MID, 0, -60);

    // Bottom hint
    lv_obj_t *hint = lv_label_create(g_face_screen);
    lv_label_set_text(hint, "ULTRA");
    lv_obj_set_style_text_color(hint, lv_color_hex(0x333333), 0);
    lv_obj_set_style_text_font(hint, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_letter_space(hint, 6, 0);
    lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -25);
}

static void apply_watch_face(int face_idx) {
    g_current_face = face_idx % FACE_COUNT;
    switch (g_current_face) {
        case 0: build_modular_face(); break;
        case 1: build_wayfinder_face(); break;
        case 2: build_solar_dial_face(); break;
        case 3: build_digital_bold_face(); break;
    }
}

static void update_watch_face() {
    struct tm tm;
    if (getLocalTime(&tm, 5)) {
        char t[8], d[16];
        strftime(t, 8, "%H:%M", &tm);
        strftime(d, 16, "%a %d %b", &tm);
        if (g_time_label) lv_label_set_text(g_time_label, t);
        if (g_date_label) {
            for (int i = 0; d[i]; i++) d[i] = toupper(d[i]);
            lv_label_set_text(g_date_label, d);
        }
    }
    
    // Update activity rings
    if (g_activity_arc[0]) lv_arc_set_value(g_activity_arc[0], min((g_move_calories * 100) / 500, 100));
    if (g_activity_arc[1]) lv_arc_set_value(g_activity_arc[1], min((g_exercise_minutes * 100) / 30, 100));
    if (g_activity_arc[2]) lv_arc_set_value(g_activity_arc[2], min((g_stand_hours * 100) / 12, 100));
    
    // Update complications
    if (g_face_complication[0]) {
        char buf[16];
        snprintf(buf, 16, "%d STEPS", g_steps);
        lv_label_set_text(g_face_complication[0], buf);
    }
    if (g_face_complication[1] && g_has_pmu) {
        char buf[16];
        snprintf(buf, 16, "%d%%", g_pmu.getBatteryPercent());
        lv_label_set_text(g_face_complication[1], buf);
    }
    if (g_face_complication[2]) {
        int rh, rm, sh, sm;
        calc_sunrise_sunset(rh, rm, sh, sm);
        char buf[16];
        snprintf(buf, 16, "%02d:%02d", rh, rm);
        lv_label_set_text(g_face_complication[2], buf);
    }
    if (g_face_complication[3]) {
        int phase = moon_phase_day();
        const char *moon_icons[] = {"🌑", "🌒", "🌓", "🌔", "🌕", "🌖", "🌗", "🌘"};
        int idx = (phase * 8) / 30;
        lv_label_set_text(g_face_complication[3], moon_icons[idx % 8]);
    }
}

// =============================================================================
//  APP LAUNCHER
// =============================================================================
static lv_obj_t *g_launcher_screen = nullptr;
static int g_launcher_style = 0;  // 0 = Grid (scrollable), 1 = Honeycomb (top 12 only)

static void open_stopwatch();
static void open_timer();
static void open_alarm();
static void open_weather();
static void open_battery_stats();
static void open_flashlight();
static void open_calculator();
static void open_settings();
static void open_siren();
static void open_workout();
static void open_depth();
static void open_waypoint();
static void open_compass();
static void open_breathe();
static void open_heart_rate();
static void open_notes();
static void open_pomodoro();
static void open_world_clock();
static void open_snake();
static void open_dice();
static void open_reaction();
static void open_find_phone();

struct AppEntry {
    const char *name;
    const char *icon;
    uint32_t color;
    void (*open)();
};

static const AppEntry APPS[] = {
    {"Stopwatch",  LV_SYMBOL_PLAY,     COL_ORANGE, open_stopwatch},
    {"Timer",      LV_SYMBOL_BELL,     COL_ORANGE, open_timer},
    {"Alarm",      LV_SYMBOL_WARNING,  COL_RED,    open_alarm},
    {"Weather",    LV_SYMBOL_IMAGE,    COL_BLUE,   open_weather},
    {"Battery",    LV_SYMBOL_CHARGE,   COL_GREEN,  open_battery_stats},
    {"Workout",    LV_SYMBOL_CHARGE,   COL_GREEN,  open_workout},
    {"Depth",      LV_SYMBOL_DOWNLOAD, COL_BLUE,   open_depth},
    {"Siren",      LV_SYMBOL_WARNING,  COL_RED,    open_siren},
    {"Waypoint",   LV_SYMBOL_CALL,     COL_ORANGE, open_waypoint},
    {"Flashlight", LV_SYMBOL_EYE_OPEN, COL_WHITE,  open_flashlight},
    {"Calculator", LV_SYMBOL_EDIT,     COL_ORANGE, open_calculator},
    {"Settings",   LV_SYMBOL_SETTINGS, COL_GRAY,   open_settings},
    {"Compass",    LV_SYMBOL_GPS,      COL_RED,    open_compass},
    {"Breathe",    LV_SYMBOL_REFRESH,  COL_CYAN,   open_breathe},
    {"Heart",      LV_SYMBOL_WARNING,  COL_PINK,   open_heart_rate},
    {"Notes",      LV_SYMBOL_FILE,     COL_ORANGE, open_notes},
    {"Pomodoro",   LV_SYMBOL_BELL,     COL_RED,    open_pomodoro},
    {"World",      LV_SYMBOL_REFRESH,  COL_BLUE,   open_world_clock},
    {"Snake",      LV_SYMBOL_PLAY,     COL_GREEN,  open_snake},
    {"Dice",       LV_SYMBOL_LIST,     COL_ORANGE, open_dice},
    {"Reaction",   LV_SYMBOL_OK,       COL_GREEN,  open_reaction},
    {"FindPhone",  LV_SYMBOL_CALL,     COL_BLUE,   open_find_phone},
};
static const int APPS_COUNT = sizeof(APPS) / sizeof(APPS[0]);


// =============================================================================
//  LAUNCHER STYLE BUILDERS
// =============================================================================
static void build_launcher_honeycomb() {
    if (g_launcher_screen) lv_obj_del(g_launcher_screen);
    g_launcher_screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(g_launcher_screen, lv_color_hex(COL_BG), 0);
    lv_obj_set_style_bg_opa(g_launcher_screen, LV_OPA_COVER, 0);
    lv_obj_clear_flag(g_launcher_screen, LV_OBJ_FLAG_SCROLLABLE);
    
    int hex_x[] = {205, 135, 275, 95, 205, 315, 135, 275, 95, 205, 315, 205};
    int hex_y[] = {100, 160, 160, 220, 220, 220, 280, 280, 340, 340, 340, 400};
    
    for (int i = 0; i < APPS_COUNT && i < 12; i++) {
        lv_obj_t *btn = lv_btn_create(g_launcher_screen);
        lv_obj_set_size(btn, 80, 80);
        lv_obj_set_pos(btn, hex_x[i] - 40, hex_y[i] - 40);
        lv_obj_set_style_radius(btn, 40, 0);
        lv_obj_set_style_bg_color(btn, lv_color_hex(APPS[i].color), 0);
        lv_obj_set_style_border_width(btn, 2, 0);
        lv_obj_set_style_border_color(btn, lv_color_hex(0x333333), 0);
        lv_obj_set_style_shadow_width(btn, 8, 0);
        lv_obj_set_style_shadow_color(btn, lv_color_hex(0x000000), 0);
        lv_obj_set_style_shadow_opa(btn, 100, 0);
        
        lv_obj_t *lbl = lv_label_create(btn);
        lv_label_set_text(lbl, APPS[i].icon);
        lv_obj_set_style_text_color(lbl, lv_color_hex(COL_WHITE), 0);
        lv_obj_set_style_text_font(lbl, &lv_font_montserrat_28, 0);
        lv_obj_center(lbl);
        
        lv_obj_add_event_cb(btn, [](lv_event_t *e) {
            int idx = (int)(intptr_t)lv_event_get_user_data(e);
            if (APPS[idx].open) APPS[idx].open();
        }, LV_EVENT_CLICKED, (void*)(intptr_t)i);
    }
    
    lv_obj_t *ultra_lbl = lv_label_create(g_launcher_screen);
    lv_label_set_text(ultra_lbl, "ULTRA");
    lv_obj_set_style_text_color(ultra_lbl, lv_color_hex(COL_ORANGE), 0);
    lv_obj_set_style_text_font(ultra_lbl, &lv_font_montserrat_22, 0);
    lv_obj_set_style_text_letter_space(ultra_lbl, 5, 0);
    lv_obj_align(ultra_lbl, LV_ALIGN_TOP_MID, 0, 20);
    
    lv_obj_t *back = lv_btn_create(g_launcher_screen);
    lv_obj_set_size(back, 200, 46);
    lv_obj_align(back, LV_ALIGN_BOTTOM_MID, 0, -10);
    lv_obj_set_style_bg_color(back, lv_color_hex(0x141414), 0);
    lv_obj_set_style_border_color(back, lv_color_hex(COL_ORANGE), 0);
    lv_obj_set_style_border_width(back, 1, 0);
    lv_obj_set_style_radius(back, 23, 0);
    lv_obj_t *bl = lv_label_create(back);
    lv_label_set_text(bl, LV_SYMBOL_LEFT "  LauncherOS");
    lv_obj_set_style_text_color(bl, lv_color_hex(COL_ORANGE), 0);
    lv_obj_set_style_text_font(bl, &lv_font_montserrat_16, 0);
    lv_obj_center(bl);
    lv_obj_add_event_cb(back, [](lv_event_t*) { return_to_launcher(); }, LV_EVENT_CLICKED, NULL);
}

static void build_launcher_grid() {
    if (g_launcher_screen) lv_obj_del(g_launcher_screen);
    g_launcher_screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(g_launcher_screen, lv_color_hex(COL_BG), 0);
    lv_obj_set_style_bg_opa(g_launcher_screen, LV_OPA_COVER, 0);
    lv_obj_clear_flag(g_launcher_screen, LV_OBJ_FLAG_SCROLLABLE);
    
    lv_obj_t *ultra_lbl = lv_label_create(g_launcher_screen);
    lv_label_set_text(ultra_lbl, "ULTRA");
    lv_obj_set_style_text_color(ultra_lbl, lv_color_hex(COL_ORANGE), 0);
    lv_obj_set_style_text_font(ultra_lbl, &lv_font_montserrat_22, 0);
    lv_obj_set_style_text_letter_space(ultra_lbl, 5, 0);
    lv_obj_align(ultra_lbl, LV_ALIGN_TOP_MID, 0, 15);

    // Scrollable grid container — accommodates all apps
    lv_obj_t *cont = lv_obj_create(g_launcher_screen);
    lv_obj_set_size(cont, LCD_WIDTH, LCD_HEIGHT - 110);
    lv_obj_align(cont, LV_ALIGN_TOP_MID, 0, 50);
    lv_obj_set_style_bg_opa(cont, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(cont, 0, 0);
    lv_obj_set_style_pad_all(cont, 0, 0);
    lv_obj_set_scroll_dir(cont, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(cont, LV_SCROLLBAR_MODE_AUTO);

    for (int i = 0; i < APPS_COUNT; i++) {
        int row = i / 3;
        int col = i % 3;

        lv_obj_t *btn = lv_btn_create(cont);
        lv_obj_set_size(btn, 100, 100);
        lv_obj_set_pos(btn, 30 + col * 120, row * 130);
        lv_obj_set_style_radius(btn, 22, 0);
        lv_obj_set_style_bg_color(btn, lv_color_hex(APPS[i].color), 0);
        lv_obj_set_style_border_width(btn, 0, 0);
        lv_obj_set_style_shadow_width(btn, 6, 0);
        lv_obj_set_style_shadow_color(btn, lv_color_hex(0x000000), 0);
        lv_obj_set_style_shadow_opa(btn, 80, 0);

        lv_obj_t *lbl = lv_label_create(btn);
        lv_label_set_text(lbl, APPS[i].icon);
        lv_obj_set_style_text_color(lbl, lv_color_hex(COL_WHITE), 0);
        lv_obj_set_style_text_font(lbl, &lv_font_montserrat_28, 0);
        lv_obj_center(lbl);

        lv_obj_add_event_cb(btn, [](lv_event_t *e) {
            int idx = (int)(intptr_t)lv_event_get_user_data(e);
            if (APPS[idx].open) APPS[idx].open();
        }, LV_EVENT_CLICKED, (void*)(intptr_t)i);

        lv_obj_t *name = lv_label_create(cont);
        lv_label_set_text(name, APPS[i].name);
        lv_obj_set_style_text_color(name, lv_color_hex(COL_WHITE), 0);
        lv_obj_set_style_text_font(name, &lv_font_montserrat_12, 0);
        lv_obj_set_style_text_align(name, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_width(name, 100);
        lv_obj_set_pos(name, 30 + col * 120, row * 130 + 102);
    }

    lv_obj_t *back = lv_btn_create(g_launcher_screen);
    lv_obj_set_size(back, 200, 46);
    lv_obj_align(back, LV_ALIGN_BOTTOM_MID, 0, -10);
    lv_obj_set_style_bg_color(back, lv_color_hex(0x141414), 0);
    lv_obj_set_style_border_color(back, lv_color_hex(COL_ORANGE), 0);
    lv_obj_set_style_border_width(back, 1, 0);
    lv_obj_set_style_radius(back, 23, 0);
    lv_obj_t *bl = lv_label_create(back);
    lv_label_set_text(bl, LV_SYMBOL_LEFT "  LauncherOS");
    lv_obj_set_style_text_color(bl, lv_color_hex(COL_ORANGE), 0);
    lv_obj_set_style_text_font(bl, &lv_font_montserrat_16, 0);
    lv_obj_center(bl);
    lv_obj_add_event_cb(back, [](lv_event_t*) { return_to_launcher(); }, LV_EVENT_CLICKED, NULL);
}

static void build_launcher() {
    if (g_launcher_style == 0) build_launcher_grid();
    else build_launcher_honeycomb();
}

// =============================================================================
//  STOPWATCH APP
// =============================================================================
static lv_obj_t *g_stopwatch_screen = nullptr;
static lv_obj_t *g_stopwatch_time_label = nullptr;
static uint32_t g_stopwatch_start = 0;
static uint32_t g_stopwatch_elapsed = 0;
static bool g_stopwatch_running = false;

static void open_stopwatch() {
    g_stopwatch_screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(g_stopwatch_screen, lv_color_hex(COL_BG), 0);
    lv_obj_set_style_bg_opa(g_stopwatch_screen, LV_OPA_COVER, 0);
    
    lv_obj_t *title = lv_label_create(g_stopwatch_screen);
    lv_label_set_text(title, "STOPWATCH");
    lv_obj_set_style_text_color(title, lv_color_hex(COL_ORANGE), 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_22, 0);
    lv_obj_set_style_text_letter_space(title, 3, 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 30);
    
    g_stopwatch_time_label = lv_label_create(g_stopwatch_screen);
    lv_label_set_text(g_stopwatch_time_label, "00:00.00");
    lv_obj_set_style_text_color(g_stopwatch_time_label, lv_color_hex(COL_WHITE), 0);
    lv_obj_set_style_text_font(g_stopwatch_time_label, &lv_font_montserrat_48, 0);
    lv_obj_align(g_stopwatch_time_label, LV_ALIGN_CENTER, 0, -20);
    
    // Start/Stop button
    lv_obj_t *btn_start = lv_btn_create(g_stopwatch_screen);
    lv_obj_set_size(btn_start, 120, 120);
    lv_obj_align(btn_start, LV_ALIGN_CENTER, -70, 120);
    lv_obj_set_style_radius(btn_start, 60, 0);
    lv_obj_set_style_bg_color(btn_start, lv_color_hex(COL_GREEN), 0);
    lv_obj_t *lbl_start = lv_label_create(btn_start);
    lv_label_set_text(lbl_start, LV_SYMBOL_PLAY);
    lv_obj_set_style_text_font(lbl_start, &lv_font_montserrat_48, 0);
    lv_obj_center(lbl_start);
    lv_obj_add_event_cb(btn_start, [](lv_event_t*) {
        if (!g_stopwatch_running) {
            g_stopwatch_start = millis();
            g_stopwatch_running = true;
        } else {
            g_stopwatch_elapsed += (millis() - g_stopwatch_start);
            g_stopwatch_running = false;
        }
    }, LV_EVENT_CLICKED, NULL);
    
    // Reset button
    lv_obj_t *btn_reset = lv_btn_create(g_stopwatch_screen);
    lv_obj_set_size(btn_reset, 120, 120);
    lv_obj_align(btn_reset, LV_ALIGN_CENTER, 70, 120);
    lv_obj_set_style_radius(btn_reset, 60, 0);
    lv_obj_set_style_bg_color(btn_reset, lv_color_hex(COL_GRAY), 0);
    lv_obj_t *lbl_reset = lv_label_create(btn_reset);
    lv_label_set_text(lbl_reset, LV_SYMBOL_REFRESH);
    lv_obj_set_style_text_font(lbl_reset, &lv_font_montserrat_48, 0);
    lv_obj_center(lbl_reset);
    lv_obj_add_event_cb(btn_reset, [](lv_event_t*) {
        g_stopwatch_elapsed = 0;
        g_stopwatch_running = false;
    }, LV_EVENT_CLICKED, NULL);
    
    // Back button
    lv_obj_t *back = lv_btn_create(g_stopwatch_screen);
    lv_obj_set_size(back, 140, 40);
    lv_obj_align(back, LV_ALIGN_TOP_LEFT, 20, 70);
    lv_obj_set_style_bg_color(back, lv_color_hex(0x141414), 0);
    lv_obj_set_style_border_color(back, lv_color_hex(COL_ORANGE), 0);
    lv_obj_set_style_border_width(back, 1, 0);
    lv_obj_set_style_radius(back, 20, 0);
    lv_obj_t *bl = lv_label_create(back);
    lv_label_set_text(bl, LV_SYMBOL_LEFT " Back");
    lv_obj_set_style_text_color(bl, lv_color_hex(COL_ORANGE), 0);
    lv_obj_center(bl);
    lv_obj_add_event_cb(back, [](lv_event_t*) {
        lv_scr_load(g_launcher_screen);
    }, LV_EVENT_CLICKED, NULL);
    
    lv_scr_load(g_stopwatch_screen);
}

// =============================================================================
//  TIMER APP - Complete Implementation
// =============================================================================
static lv_obj_t *g_timer_screen = nullptr;
static lv_obj_t *g_timer_label = nullptr;
static lv_obj_t *g_timer_arc = nullptr;
static uint32_t g_timer_duration = 0;
static uint32_t g_timer_remaining = 0;
static uint32_t g_timer_start_time = 0;
static bool g_timer_running = false;

static void open_timer() {
    g_timer_screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(g_timer_screen, lv_color_hex(COL_BG), 0);
    lv_obj_set_style_bg_opa(g_timer_screen, LV_OPA_COVER, 0);
    
    lv_obj_t *title = lv_label_create(g_timer_screen);
    lv_label_set_text(title, "TIMER");
    lv_obj_set_style_text_color(title, lv_color_hex(COL_ORANGE), 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_22, 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 30);
    
    g_timer_label = lv_label_create(g_timer_screen);
    lv_label_set_text(g_timer_label, "05:00");
    lv_obj_set_style_text_color(g_timer_label, lv_color_hex(COL_WHITE), 0);
    lv_obj_set_style_text_font(g_timer_label, &lv_font_montserrat_48, 0);
    lv_obj_align(g_timer_label, LV_ALIGN_CENTER, 0, -40);
    
    g_timer_arc = lv_arc_create(g_timer_screen);
    lv_obj_set_size(g_timer_arc, 200, 200);
    lv_obj_align(g_timer_arc, LV_ALIGN_CENTER, 0, -30);
    lv_arc_set_rotation(g_timer_arc, 270);
    lv_arc_set_bg_angles(g_timer_arc, 0, 360);
    lv_arc_set_range(g_timer_arc, 0, 100);
    lv_arc_set_value(g_timer_arc, 100);
    lv_obj_remove_style(g_timer_arc, NULL, LV_PART_KNOB);
    lv_obj_clear_flag(g_timer_arc, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_arc_width(g_timer_arc, 15, LV_PART_MAIN);
    lv_obj_set_style_arc_width(g_timer_arc, 15, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(g_timer_arc, lv_color_hex(0x1C1C1C), LV_PART_MAIN);
    lv_obj_set_style_arc_color(g_timer_arc, lv_color_hex(COL_ORANGE), LV_PART_INDICATOR);
    
    const char *presets[] = {"1m", "3m", "5m", "10m", "15m", "30m"};
    int preset_secs[] = {60, 180, 300, 600, 900, 1800};
    int x_pos[] = {-120, -40, 40, -120, -40, 40};
    int y_pos[] = {100, 100, 100, 160, 160, 160};
    
    for (int i = 0; i < 6; i++) {
        lv_obj_t *btn = lv_btn_create(g_timer_screen);
        lv_obj_set_size(btn, 70, 50);
        lv_obj_align(btn, LV_ALIGN_CENTER, x_pos[i], y_pos[i]);
        lv_obj_set_style_bg_color(btn, lv_color_hex(COL_PANEL), 0);
        lv_obj_set_style_radius(btn, 10, 0);
        lv_obj_t *lbl = lv_label_create(btn);
        lv_label_set_text(lbl, presets[i]);
        lv_obj_set_style_text_color(lbl, lv_color_hex(COL_WHITE), 0);
        lv_obj_center(lbl);
        int duration = preset_secs[i];
        lv_obj_add_event_cb(btn, [](lv_event_t *e) {
            int dur = (int)(intptr_t)lv_event_get_user_data(e);
            g_timer_duration = dur;
            g_timer_remaining = dur;
            g_timer_running = false;
            char buf[16];
            snprintf(buf, 16, "%02d:%02d", dur/60, dur%60);
            lv_label_set_text(g_timer_label, buf);
            lv_arc_set_value(g_timer_arc, 100);
        }, LV_EVENT_CLICKED, (void*)(intptr_t)duration);
    }
    
    lv_obj_t *btn_start = lv_btn_create(g_timer_screen);
    lv_obj_set_size(btn_start, 100, 100);
    lv_obj_align(btn_start, LV_ALIGN_BOTTOM_MID, 0, -60);
    lv_obj_set_style_radius(btn_start, 50, 0);
    lv_obj_set_style_bg_color(btn_start, lv_color_hex(COL_GREEN), 0);
    lv_obj_t *lbl_start = lv_label_create(btn_start);
    lv_label_set_text(lbl_start, LV_SYMBOL_PLAY);
    lv_obj_set_style_text_font(lbl_start, &lv_font_montserrat_48, 0);
    lv_obj_center(lbl_start);
    lv_obj_add_event_cb(btn_start, [](lv_event_t*) {
        if (!g_timer_running && g_timer_remaining > 0) {
            g_timer_start_time = millis();
            g_timer_running = true;
        } else {
            g_timer_running = false;
        }
    }, LV_EVENT_CLICKED, NULL);
    
    lv_obj_t *back = lv_btn_create(g_timer_screen);
    lv_obj_set_size(back, 140, 40);
    lv_obj_align(back, LV_ALIGN_TOP_LEFT, 20, 70);
    lv_obj_set_style_bg_color(back, lv_color_hex(0x141414), 0);
    lv_obj_set_style_border_color(back, lv_color_hex(COL_ORANGE), 0);
    lv_obj_set_style_border_width(back, 1, 0);
    lv_obj_t *bl = lv_label_create(back);
    lv_label_set_text(bl, LV_SYMBOL_LEFT " Back");
    lv_obj_set_style_text_color(bl, lv_color_hex(COL_ORANGE), 0);
    lv_obj_center(bl);
    lv_obj_add_event_cb(back, [](lv_event_t*) { lv_scr_load(g_launcher_screen); }, LV_EVENT_CLICKED, NULL);
    
    lv_scr_load(g_timer_screen);
}

// =============================================================================
//  ALARM APP - Complete Implementation
// =============================================================================
static lv_obj_t *g_alarm_screen = nullptr;
struct Alarm { int hour; int minute; bool enabled; };
static Alarm g_alarms[3] = {{7, 0, false}, {8, 30, false}, {12, 0, false}};

static void open_alarm() {
    g_alarm_screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(g_alarm_screen, lv_color_hex(COL_BG), 0);
    
    lv_obj_t *title = lv_label_create(g_alarm_screen);
    lv_label_set_text(title, "ALARMS");
    lv_obj_set_style_text_color(title, lv_color_hex(COL_RED), 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_22, 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 30);
    
    for (int i = 0; i < 3; i++) {
        lv_obj_t *panel = lv_obj_create(g_alarm_screen);
        lv_obj_set_size(panel, 350, 80);
        lv_obj_set_pos(panel, 30, 80 + i * 95);
        lv_obj_set_style_bg_color(panel, lv_color_hex(COL_PANEL), 0);
        lv_obj_set_style_radius(panel, 15, 0);
        
        char time_str[8];
        snprintf(time_str, 8, "%02d:%02d", g_alarms[i].hour, g_alarms[i].minute);
        lv_obj_t *time_lbl = lv_label_create(panel);
        lv_label_set_text(time_lbl, time_str);
        lv_obj_set_style_text_font(time_lbl, &lv_font_montserrat_28, 0);
        lv_obj_set_style_text_color(time_lbl, lv_color_hex(COL_WHITE), 0);
        lv_obj_set_pos(time_lbl, 20, 20);
        
        lv_obj_t *sw = lv_switch_create(panel);
        lv_obj_set_size(sw, 60, 35);
        lv_obj_align(sw, LV_ALIGN_RIGHT_MID, -15, 0);
        if (g_alarms[i].enabled) lv_obj_add_state(sw, LV_STATE_CHECKED);
        
        int idx = i;
        lv_obj_add_event_cb(sw, [](lv_event_t *e) {
            int i = (int)(intptr_t)lv_event_get_user_data(e);
            g_alarms[i].enabled = !g_alarms[i].enabled;
        }, LV_EVENT_VALUE_CHANGED, (void*)(intptr_t)idx);
    }
    
    lv_obj_t *back = lv_btn_create(g_alarm_screen);
    lv_obj_set_size(back, 140, 40);
    lv_obj_align(back, LV_ALIGN_BOTTOM_MID, 0, -20);
    lv_obj_set_style_bg_color(back, lv_color_hex(0x141414), 0);
    lv_obj_set_style_border_color(back, lv_color_hex(COL_RED), 0);
    lv_obj_set_style_border_width(back, 1, 0);
    lv_obj_t *bl = lv_label_create(back);
    lv_label_set_text(bl, LV_SYMBOL_LEFT " Back");
    lv_obj_center(bl);
    lv_obj_add_event_cb(back, [](lv_event_t*) { lv_scr_load(g_launcher_screen); }, LV_EVENT_CLICKED, NULL);
    
    lv_scr_load(g_alarm_screen);
}

// =============================================================================
//  WEATHER APP - OpenWeatherMap API
// =============================================================================
static lv_obj_t *g_weather_screen = nullptr;
static lv_obj_t *g_weather_temp = nullptr;
static lv_obj_t *g_weather_desc = nullptr;
static String g_weather_api_key = "51e5cba611055b1fb8145d36eb4b4f45";
static String g_weather_city = "Perth";  // Default: Perth, Australia
static float g_temp = 0.0;
static String g_weather_condition = "Loading...";

static void fetch_weather() {
    if (WiFi.status() != WL_CONNECTED) {
        WiFi.begin("UltraWatch", "ultra1234");
        int tries = 0;
        while (WiFi.status() != WL_CONNECTED && tries++ < 20) delay(200);
    }
    if (WiFi.status() == WL_CONNECTED) {
        HTTPClient http;
        String url = "http://api.openweathermap.org/data/2.5/weather?q=" + 
                     g_weather_city + "&appid=" + g_weather_api_key + "&units=metric";
        http.begin(url);
        int code = http.GET();
        if (code == 200) {
            String payload = http.getString();
            int temp_idx = payload.indexOf("\"temp\":");
            if (temp_idx > 0) {
                String temp_str = payload.substring(temp_idx + 7, temp_idx + 12);
                g_temp = temp_str.toFloat();
            }
            int desc_idx = payload.indexOf("\"description\":\"");
            if (desc_idx > 0) {
                int end_idx = payload.indexOf("\"", desc_idx + 15);
                g_weather_condition = payload.substring(desc_idx + 15, end_idx);
            }
        }
        http.end();
        WiFi.disconnect(true);
        WiFi.mode(WIFI_OFF);
    }
}

static void open_weather() {
    g_weather_screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(g_weather_screen, lv_color_hex(COL_BG), 0);
    
    lv_obj_t *title = lv_label_create(g_weather_screen);
    lv_label_set_text(title, "WEATHER");
    lv_obj_set_style_text_color(title, lv_color_hex(COL_BLUE), 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_22, 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 30);
    
    g_weather_temp = lv_label_create(g_weather_screen);
    lv_label_set_text(g_weather_temp, "...");
    lv_obj_set_style_text_color(g_weather_temp, lv_color_hex(COL_WHITE), 0);
    lv_obj_set_style_text_font(g_weather_temp, &lv_font_montserrat_48, 0);
    lv_obj_align(g_weather_temp, LV_ALIGN_CENTER, 0, -40);
    
    g_weather_desc = lv_label_create(g_weather_screen);
    lv_label_set_text(g_weather_desc, "...");
    lv_obj_set_style_text_color(g_weather_desc, lv_color_hex(COL_GRAY), 0);
    lv_obj_set_style_text_font(g_weather_desc, &lv_font_montserrat_18, 0);
    lv_obj_align(g_weather_desc, LV_ALIGN_CENTER, 0, 20);
    
    lv_obj_t *city_lbl = lv_label_create(g_weather_screen);
    lv_label_set_text(city_lbl, g_weather_city.c_str());
    lv_obj_set_style_text_color(city_lbl, lv_color_hex(COL_BLUE), 0);
    lv_obj_align(city_lbl, LV_ALIGN_CENTER, 0, 60);
    
    lv_obj_t *refresh = lv_btn_create(g_weather_screen);
    lv_obj_set_size(refresh, 160, 50);
    lv_obj_align(refresh, LV_ALIGN_CENTER, 0, 120);
    lv_obj_set_style_bg_color(refresh, lv_color_hex(COL_BLUE), 0);
    lv_obj_t *rlbl = lv_label_create(refresh);
    lv_label_set_text(rlbl, LV_SYMBOL_REFRESH " Refresh");
    lv_obj_center(rlbl);
    lv_obj_add_event_cb(refresh, [](lv_event_t*) {
        lv_label_set_text(g_weather_temp, "Loading...");
        fetch_weather();
        char buf[16];
        snprintf(buf, 16, "%.1f°C", g_temp);
        lv_label_set_text(g_weather_temp, buf);
        lv_label_set_text(g_weather_desc, g_weather_condition.c_str());
    }, LV_EVENT_CLICKED, NULL);
    
    lv_obj_t *back = lv_btn_create(g_weather_screen);
    lv_obj_set_size(back, 140, 40);
    lv_obj_align(back, LV_ALIGN_TOP_LEFT, 20, 70);
    lv_obj_set_style_bg_color(back, lv_color_hex(0x141414), 0);
    lv_obj_set_style_border_color(back, lv_color_hex(COL_BLUE), 0);
    lv_obj_set_style_border_width(back, 1, 0);
    lv_obj_t *bl = lv_label_create(back);
    lv_label_set_text(bl, LV_SYMBOL_LEFT " Back");
    lv_obj_center(bl);
    lv_obj_add_event_cb(back, [](lv_event_t*) { lv_scr_load(g_launcher_screen); }, LV_EVENT_CLICKED, NULL);
    
    lv_scr_load(g_weather_screen);
    fetch_weather();
}

// =============================================================================
//  BATTERY STATS APP — voltage / %, charging state, runtime estimate
// =============================================================================
static lv_obj_t *g_batt_screen = nullptr;
static lv_obj_t *g_batt_arc    = nullptr;
static lv_obj_t *g_batt_pct    = nullptr;
static lv_obj_t *g_batt_volts  = nullptr;
static lv_obj_t *g_batt_state  = nullptr;

static void open_battery_stats() {
    g_batt_screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(g_batt_screen, lv_color_hex(COL_BG), 0);

    lv_obj_t *title = lv_label_create(g_batt_screen);
    lv_label_set_text(title, "BATTERY");
    lv_obj_set_style_text_color(title, lv_color_hex(COL_GREEN), 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_22, 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 30);

    // Big arc visualising %
    g_batt_arc = lv_arc_create(g_batt_screen);
    lv_obj_set_size(g_batt_arc, 280, 280);
    lv_obj_align(g_batt_arc, LV_ALIGN_CENTER, 0, -20);
    lv_arc_set_rotation(g_batt_arc, 270);
    lv_arc_set_bg_angles(g_batt_arc, 0, 360);
    lv_arc_set_range(g_batt_arc, 0, 100);
    lv_obj_remove_style(g_batt_arc, NULL, LV_PART_KNOB);
    lv_obj_clear_flag(g_batt_arc, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_arc_color(g_batt_arc, lv_color_hex(COL_PANEL), LV_PART_MAIN);
    lv_obj_set_style_arc_color(g_batt_arc, lv_color_hex(COL_GREEN), LV_PART_INDICATOR);
    lv_obj_set_style_arc_width(g_batt_arc, 14, LV_PART_MAIN);
    lv_obj_set_style_arc_width(g_batt_arc, 14, LV_PART_INDICATOR);

    g_batt_pct = lv_label_create(g_batt_screen);
    lv_label_set_text(g_batt_pct, "--%");
    lv_obj_set_style_text_color(g_batt_pct, lv_color_hex(COL_WHITE), 0);
    lv_obj_set_style_text_font(g_batt_pct, &lv_font_montserrat_48, 0);
    lv_obj_align(g_batt_pct, LV_ALIGN_CENTER, 0, -50);

    g_batt_volts = lv_label_create(g_batt_screen);
    lv_label_set_text(g_batt_volts, "-.-- V");
    lv_obj_set_style_text_color(g_batt_volts, lv_color_hex(COL_GRAY), 0);
    lv_obj_set_style_text_font(g_batt_volts, &lv_font_montserrat_18, 0);
    lv_obj_align(g_batt_volts, LV_ALIGN_CENTER, 0, 0);

    g_batt_state = lv_label_create(g_batt_screen);
    lv_label_set_text(g_batt_state, "");
    lv_obj_set_style_text_color(g_batt_state, lv_color_hex(COL_ORANGE), 0);
    lv_obj_set_style_text_font(g_batt_state, &lv_font_montserrat_16, 0);
    lv_obj_align(g_batt_state, LV_ALIGN_CENTER, 0, 35);

    lv_obj_t *back = lv_btn_create(g_batt_screen);
    lv_obj_set_size(back, 140, 40);
    lv_obj_align(back, LV_ALIGN_BOTTOM_MID, 0, -20);
    lv_obj_set_style_bg_color(back, lv_color_hex(0x141414), 0);
    lv_obj_set_style_border_color(back, lv_color_hex(COL_GREEN), 0);
    lv_obj_set_style_border_width(back, 1, 0);
    lv_obj_t *bl = lv_label_create(back);
    lv_label_set_text(bl, LV_SYMBOL_LEFT " Back");
    lv_obj_center(bl);
    lv_obj_add_event_cb(back, [](lv_event_t*) {
        g_batt_arc = nullptr; g_batt_pct = nullptr; g_batt_volts = nullptr; g_batt_state = nullptr;
        lv_scr_load(g_launcher_screen);
    }, LV_EVENT_CLICKED, NULL);

    lv_scr_load(g_batt_screen);
}

static void open_flashlight() {
    gfx->fillScreen(0xFFFF);
    static_cast<Arduino_CO5300*>(gfx)->setBrightness(255);
    delay(100);
}

// =============================================================================
//  CALCULATOR APP - Complete Implementation
// =============================================================================
static lv_obj_t *g_calc_screen = nullptr;
static lv_obj_t *g_calc_display = nullptr;
static String g_calc_current = "0";
static String g_calc_operator = "";
static float g_calc_mem = 0;
static bool g_calc_new_number = true;

static void calc_button_click(const char *btn) {
    if (strcmp(btn, "C") == 0) {
        g_calc_current = "0";
        g_calc_operator = "";
        g_calc_mem = 0;
        g_calc_new_number = true;
    } else if (strcmp(btn, "=") == 0) {
        float current_val = g_calc_current.toFloat();
        if (g_calc_operator == "+") g_calc_mem += current_val;
        else if (g_calc_operator == "-") g_calc_mem -= current_val;
        else if (g_calc_operator == "×") g_calc_mem *= current_val;
        else if (g_calc_operator == "÷" && current_val != 0) g_calc_mem /= current_val;
        g_calc_current = String(g_calc_mem);
        g_calc_operator = "";
        g_calc_new_number = true;
    } else if (strchr("+-×÷", btn[0]) != NULL) {
        g_calc_mem = g_calc_current.toFloat();
        g_calc_operator = btn;
        g_calc_new_number = true;
    } else {
        if (g_calc_new_number) {
            g_calc_current = btn;
            g_calc_new_number = false;
        } else {
            if (g_calc_current == "0") g_calc_current = btn;
            else g_calc_current += btn;
        }
    }
    lv_label_set_text(g_calc_display, g_calc_current.c_str());
}

static void open_calculator() {
    g_calc_screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(g_calc_screen, lv_color_hex(COL_BG), 0);
    
    lv_obj_t *title = lv_label_create(g_calc_screen);
    lv_label_set_text(title, "CALCULATOR");
    lv_obj_set_style_text_color(title, lv_color_hex(COL_ORANGE), 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_18, 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 20);
    
    g_calc_display = lv_label_create(g_calc_screen);
    lv_label_set_text(g_calc_display, "0");
    lv_obj_set_style_text_color(g_calc_display, lv_color_hex(COL_WHITE), 0);
    lv_obj_set_style_text_font(g_calc_display, &lv_font_montserrat_48, 0);
    lv_obj_align(g_calc_display, LV_ALIGN_TOP_MID, 0, 60);
    
    const char *buttons[] = {
        "7", "8", "9", "÷",
        "4", "5", "6", "×",
        "1", "2", "3", "-",
        "C", "0", "=", "+"
    };
    
    for (int i = 0; i < 16; i++) {
        int row = i / 4;
        int col = i % 4;
        lv_obj_t *btn = lv_btn_create(g_calc_screen);
        lv_obj_set_size(btn, 85, 70);
        lv_obj_set_pos(btn, 20 + col * 92, 140 + row * 80);
        uint32_t color = (col == 3) ? COL_ORANGE : (strcmp(buttons[i], "C") == 0 || strcmp(buttons[i], "=") == 0) ? COL_GRAY : COL_PANEL;
        lv_obj_set_style_bg_color(btn, lv_color_hex(color), 0);
        lv_obj_set_style_radius(btn, 15, 0);
        lv_obj_t *lbl = lv_label_create(btn);
        lv_label_set_text(lbl, buttons[i]);
        lv_obj_set_style_text_font(lbl, &lv_font_montserrat_28, 0);
        lv_obj_center(lbl);
        const char *btn_val = buttons[i];
        lv_obj_add_event_cb(btn, [](lv_event_t *e) {
            const char *val = (const char*)lv_event_get_user_data(e);
            calc_button_click(val);
        }, LV_EVENT_CLICKED, (void*)btn_val);
    }
    
    lv_obj_t *back = lv_btn_create(g_calc_screen);
    lv_obj_set_size(back, 120, 35);
    lv_obj_align(back, LV_ALIGN_TOP_LEFT, 15, 120);
    lv_obj_set_style_bg_color(back, lv_color_hex(0x141414), 0);
    lv_obj_set_style_border_color(back, lv_color_hex(COL_ORANGE), 0);
    lv_obj_set_style_border_width(back, 1, 0);
    lv_obj_t *bl = lv_label_create(back);
    lv_label_set_text(bl, LV_SYMBOL_LEFT " Back");
    lv_obj_set_style_text_font(bl, &lv_font_montserrat_12, 0);
    lv_obj_center(bl);
    lv_obj_add_event_cb(back, [](lv_event_t*) { lv_scr_load(g_launcher_screen); }, LV_EVENT_CLICKED, NULL);
    
    lv_scr_load(g_calc_screen);
}

// =============================================================================
//  SETTINGS APP - City Selection + Brightness + About
// =============================================================================
static lv_obj_t *g_settings_screen = nullptr;
static const char *g_cities[] = {
    "Perth", "Sydney", "Melbourne", "Brisbane", "Adelaide",
    "London", "New York", "Tokyo", "Paris", "Dubai"
};

static void open_settings() {
    g_settings_screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(g_settings_screen, lv_color_hex(COL_BG), 0);
    
    lv_obj_t *title = lv_label_create(g_settings_screen);
    lv_label_set_text(title, "SETTINGS");
    lv_obj_set_style_text_color(title, lv_color_hex(COL_GRAY), 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_22, 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 30);
    
    // Brightness section
    lv_obj_t *bright_lbl = lv_label_create(g_settings_screen);
    lv_label_set_text(bright_lbl, "Brightness");
    lv_obj_set_style_text_color(bright_lbl, lv_color_hex(COL_WHITE), 0);
    lv_obj_set_style_text_font(bright_lbl, &lv_font_montserrat_16, 0);
    lv_obj_set_pos(bright_lbl, 30, 80);
    
    lv_obj_t *slider = lv_slider_create(g_settings_screen);
    lv_obj_set_size(slider, 300, 10);
    lv_obj_set_pos(slider, 55, 115);
    lv_slider_set_range(slider, 50, 255);
    lv_slider_set_value(slider, g_brightness, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(slider, lv_color_hex(COL_PANEL), LV_PART_MAIN);
    lv_obj_set_style_bg_color(slider, lv_color_hex(COL_ORANGE), LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(slider, lv_color_hex(COL_ORANGE), LV_PART_KNOB);
    lv_obj_add_event_cb(slider, [](lv_event_t *e) {
        lv_obj_t *s = lv_event_get_target(e);
        int val = lv_slider_get_value(s);
        prefs_save_bright((uint8_t)val);
    }, LV_EVENT_VALUE_CHANGED, NULL);
    
    // Launcher Style section
    lv_obj_t *style_lbl = lv_label_create(g_settings_screen);
    lv_label_set_text(style_lbl, "Launcher Style");
    lv_obj_set_style_text_color(style_lbl, lv_color_hex(COL_WHITE), 0);
    lv_obj_set_style_text_font(style_lbl, &lv_font_montserrat_16, 0);
    lv_obj_set_pos(style_lbl, 30, 150);
    
    lv_obj_t *style_dropdown = lv_dropdown_create(g_settings_screen);
    lv_dropdown_set_options(style_dropdown, "Grid\nHoneycomb");
    lv_obj_set_size(style_dropdown, 300, 45);
    lv_obj_set_pos(style_dropdown, 55, 180);
    lv_obj_set_style_bg_color(style_dropdown, lv_color_hex(COL_PANEL), 0);
    lv_obj_set_style_text_color(style_dropdown, lv_color_hex(COL_WHITE), 0);
    lv_dropdown_set_selected(style_dropdown, g_launcher_style);
    lv_obj_add_event_cb(style_dropdown, [](lv_event_t *e) {
        lv_obj_t *dd = lv_event_get_target(e);
        uint16_t idx = lv_dropdown_get_selected(dd);
        g_launcher_style = idx;
        build_launcher();
        Serial.printf("[SETTINGS] Launcher style: %s\n", idx == 0 ? "Grid" : "Honeycomb");
    }, LV_EVENT_VALUE_CHANGED, NULL);
    
    // Weather City section
    lv_obj_t *city_lbl = lv_label_create(g_settings_screen);
    lv_label_set_text(city_lbl, "Weather City");
    lv_obj_set_style_text_color(city_lbl, lv_color_hex(COL_WHITE), 0);
    lv_obj_set_style_text_font(city_lbl, &lv_font_montserrat_16, 0);
    lv_obj_set_pos(city_lbl, 30, 240);
    
    lv_obj_t *city_dropdown = lv_dropdown_create(g_settings_screen);
    lv_dropdown_set_options(city_dropdown, 
        "Perth\nSydney\nMelbourne\nBrisbane\nAdelaide\n"
        "London\nNew York\nTokyo\nParis\nDubai");
    lv_obj_set_size(city_dropdown, 300, 45);
    lv_obj_set_pos(city_dropdown, 55, 270);
    lv_obj_set_style_bg_color(city_dropdown, lv_color_hex(COL_PANEL), 0);
    lv_obj_set_style_text_color(city_dropdown, lv_color_hex(COL_WHITE), 0);
    lv_dropdown_set_selected(city_dropdown, 0);  // Perth default
    lv_obj_add_event_cb(city_dropdown, [](lv_event_t *e) {
        lv_obj_t *dd = lv_event_get_target(e);
        uint16_t idx = lv_dropdown_get_selected(dd);
        g_weather_city = String(g_cities[idx]);
        Serial.printf("[SETTINGS] City changed to: %s\n", g_weather_city.c_str());
    }, LV_EVENT_VALUE_CHANGED, NULL);
    
    // Always-On Display toggle
    lv_obj_t *aod_lbl = lv_label_create(g_settings_screen);
    lv_label_set_text(aod_lbl, "Always-On Display");
    lv_obj_set_style_text_color(aod_lbl, lv_color_hex(COL_WHITE), 0);
    lv_obj_set_style_text_font(aod_lbl, &lv_font_montserrat_16, 0);
    lv_obj_set_pos(aod_lbl, 30, 330);

    lv_obj_t *aod_sw = lv_switch_create(g_settings_screen);
    lv_obj_set_pos(aod_sw, 310, 325);
    if (g_aod_enabled) lv_obj_add_state(aod_sw, LV_STATE_CHECKED);
    lv_obj_set_style_bg_color(aod_sw, lv_color_hex(COL_PANEL), LV_PART_MAIN);
    lv_obj_set_style_bg_color(aod_sw, lv_color_hex(COL_ORANGE), LV_PART_INDICATOR | LV_STATE_CHECKED);
    lv_obj_add_event_cb(aod_sw, [](lv_event_t *e) {
        lv_obj_t *s = lv_event_get_target(e);
        prefs_save_aod(lv_obj_has_state(s, LV_STATE_CHECKED));
    }, LV_EVENT_VALUE_CHANGED, NULL);

    // Auto-sleep timeout
    lv_obj_t *tmo_lbl = lv_label_create(g_settings_screen);
    lv_label_set_text(tmo_lbl, "Sleep After");
    lv_obj_set_style_text_color(tmo_lbl, lv_color_hex(COL_WHITE), 0);
    lv_obj_set_style_text_font(tmo_lbl, &lv_font_montserrat_16, 0);
    lv_obj_set_pos(tmo_lbl, 30, 380);

    lv_obj_t *tmo_dd = lv_dropdown_create(g_settings_screen);
    lv_dropdown_set_options(tmo_dd, TIMEOUT_LABELS);
    lv_obj_set_size(tmo_dd, 300, 45);
    lv_obj_set_pos(tmo_dd, 55, 410);
    lv_obj_set_style_bg_color(tmo_dd, lv_color_hex(COL_PANEL), 0);
    lv_obj_set_style_text_color(tmo_dd, lv_color_hex(COL_WHITE), 0);
    lv_dropdown_set_selected(tmo_dd, g_timeout_idx);
    lv_obj_add_event_cb(tmo_dd, [](lv_event_t *e) {
        lv_obj_t *dd = lv_event_get_target(e);
        prefs_save_timeout((uint8_t)lv_dropdown_get_selected(dd));
    }, LV_EVENT_VALUE_CHANGED, NULL);

    // About section
    lv_obj_t *about_panel = lv_obj_create(g_settings_screen);
    lv_obj_set_size(about_panel, 350, 110);
    lv_obj_set_pos(about_panel, 30, 480);
    lv_obj_set_style_bg_color(about_panel, lv_color_hex(COL_PANEL), 0);
    lv_obj_set_style_radius(about_panel, 15, 0);
    
    lv_obj_t *about_title = lv_label_create(about_panel);
    lv_label_set_text(about_title, "UltraOS v2.1");
    lv_obj_set_style_text_color(about_title, lv_color_hex(COL_ORANGE), 0);
    lv_obj_set_style_text_font(about_title, &lv_font_montserrat_18, 0);
    lv_obj_set_pos(about_title, 15, 10);
    
    lv_obj_t *about_text = lv_label_create(about_panel);
    lv_label_set_text(about_text, "Apple Watch Ultra Clone\nESP32-S3 AMOLED\n11 Apps · 2 Faces");
    lv_obj_set_style_text_color(about_text, lv_color_hex(COL_GRAY), 0);
    lv_obj_set_style_text_font(about_text, &lv_font_montserrat_12, 0);
    lv_obj_set_pos(about_text, 15, 40);
    
    if (g_has_pmu) {
        char bat_info[32];
        snprintf(bat_info, 32, "Battery: %d%% · %.2fV", 
                 g_pmu.getBatteryPercent(), g_pmu.getBattVoltage() / 1000.0f);
        lv_obj_t *bat_lbl = lv_label_create(about_panel);
        lv_label_set_text(bat_lbl, bat_info);
        lv_obj_set_style_text_color(bat_lbl, lv_color_hex(COL_GREEN), 0);
        lv_obj_set_style_text_font(bat_lbl, &lv_font_montserrat_12, 0);
        lv_obj_align(bat_lbl, LV_ALIGN_BOTTOM_LEFT, 15, -10);
    }
    
    lv_obj_t *back = lv_btn_create(g_settings_screen);
    lv_obj_set_size(back, 140, 40);
    lv_obj_set_pos(back, 130, 610);
    lv_obj_set_style_bg_color(back, lv_color_hex(0x141414), 0);
    lv_obj_set_style_border_color(back, lv_color_hex(COL_GRAY), 0);
    lv_obj_set_style_border_width(back, 1, 0);
    lv_obj_t *bl = lv_label_create(back);
    lv_label_set_text(bl, LV_SYMBOL_LEFT " Back");
    lv_obj_center(bl);
    lv_obj_add_event_cb(back, [](lv_event_t*) { lv_scr_load(g_launcher_screen); }, LV_EVENT_CLICKED, NULL);
    
    lv_scr_load(g_settings_screen);
}

// =============================================================================
//  SIREN APP (Ultra Emergency Feature!)
// =============================================================================
static lv_obj_t *g_siren_screen = nullptr;
static bool g_siren_active = false;

static void open_siren() {
    g_siren_screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(g_siren_screen, lv_color_hex(COL_BG), 0);
    
    lv_obj_t *title = lv_label_create(g_siren_screen);
    lv_label_set_text(title, "⚠️ EMERGENCY SIREN");
    lv_obj_set_style_text_color(title, lv_color_hex(COL_RED), 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_22, 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 30);
    
    lv_obj_t *warning = lv_label_create(g_siren_screen);
    lv_label_set_text(warning, "86dB Alert Sound\nPress to activate");
    lv_obj_set_style_text_color(warning, lv_color_hex(COL_GRAY), 0);
    lv_obj_set_style_text_font(warning, &lv_font_montserrat_18, 0);
    lv_obj_set_style_text_align(warning, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(warning, LV_ALIGN_CENTER, 0, -80);
    
    // Giant red siren button
    lv_obj_t *siren_btn = lv_btn_create(g_siren_screen);
    lv_obj_set_size(siren_btn, 200, 200);
    lv_obj_align(siren_btn, LV_ALIGN_CENTER, 0, 20);
    lv_obj_set_style_radius(siren_btn, 100, 0);
    lv_obj_set_style_bg_color(siren_btn, lv_color_hex(COL_RED), 0);
    lv_obj_set_style_border_width(siren_btn, 5, 0);
    lv_obj_set_style_border_color(siren_btn, lv_color_hex(COL_ORANGE), 0);
    
    lv_obj_t *siren_icon = lv_label_create(siren_btn);
    lv_label_set_text(siren_icon, "SOS");
    lv_obj_set_style_text_font(siren_icon, &lv_font_montserrat_48, 0);
    lv_obj_set_style_text_color(siren_icon, lv_color_hex(COL_WHITE), 0);
    lv_obj_center(siren_icon);
    
    lv_obj_add_event_cb(siren_btn, [](lv_event_t*) {
        g_siren_active = !g_siren_active;
        if (g_siren_active) {
            Serial.println("[SIREN] ACTIVATED! 86dB Alert");
            // TODO: Play loud alert tone via speaker
        } else {
            Serial.println("[SIREN] Stopped");
        }
    }, LV_EVENT_CLICKED, NULL);
    
    lv_obj_t *back = lv_btn_create(g_siren_screen);
    lv_obj_set_size(back, 140, 40);
    lv_obj_align(back, LV_ALIGN_BOTTOM_MID, 0, -20);
    lv_obj_set_style_bg_color(back, lv_color_hex(0x141414), 0);
    lv_obj_set_style_border_color(back, lv_color_hex(COL_RED), 0);
    lv_obj_set_style_border_width(back, 1, 0);
    lv_obj_t *bl = lv_label_create(back);
    lv_label_set_text(bl, LV_SYMBOL_LEFT " Back");
    lv_obj_center(bl);
    lv_obj_add_event_cb(back, [](lv_event_t*) { 
        g_siren_active = false;
        lv_scr_load(g_launcher_screen); 
    }, LV_EVENT_CLICKED, NULL);
    
    lv_scr_load(g_siren_screen);
}

// =============================================================================
//  WORKOUT APP (Ultra Fitness!)
// =============================================================================
static lv_obj_t *g_workout_screen = nullptr;
static const char *g_workout_types[] = {"Run", "Cycle", "Hike", "Swim", "Yoga", "HIIT"};
static int g_workout_selected = 0;
static uint32_t g_workout_start = 0;
static bool g_workout_active = false;

static void open_workout() {
    g_workout_screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(g_workout_screen, lv_color_hex(COL_BG), 0);
    
    lv_obj_t *title = lv_label_create(g_workout_screen);
    lv_label_set_text(title, "WORKOUT");
    lv_obj_set_style_text_color(title, lv_color_hex(COL_GREEN), 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_22, 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 30);
    
    // Workout type selector
    lv_obj_t *type_label = lv_label_create(g_workout_screen);
    lv_label_set_text(type_label, g_workout_types[0]);
    lv_obj_set_style_text_color(type_label, lv_color_hex(COL_WHITE), 0);
    lv_obj_set_style_text_font(type_label, &lv_font_montserrat_28, 0);
    lv_obj_align(type_label, LV_ALIGN_TOP_MID, 0, 80);
    
    // Workout type buttons
    for (int i = 0; i < 6; i++) {
        lv_obj_t *btn = lv_btn_create(g_workout_screen);
        lv_obj_set_size(btn, 110, 60);
        int row = i / 3;
        int col = i % 3;
        lv_obj_set_pos(btn, 20 + col * 123, 140 + row * 75);
        lv_obj_set_style_bg_color(btn, lv_color_hex(COL_PANEL), 0);
        
        lv_obj_t *lbl = lv_label_create(btn);
        lv_label_set_text(lbl, g_workout_types[i]);
        lv_obj_set_style_text_font(lbl, &lv_font_montserrat_14, 0);
        lv_obj_center(lbl);
        
        int idx = i;
        lv_obj_add_event_cb(btn, [](lv_event_t *e) {
            int i = (int)(intptr_t)lv_event_get_user_data(e);
            g_workout_selected = i;
            Serial.printf("[WORKOUT] Selected: %s\n", g_workout_types[i]);
        }, LV_EVENT_CLICKED, (void*)(intptr_t)idx);
    }
    
    // Start button
    lv_obj_t *start_btn = lv_btn_create(g_workout_screen);
    lv_obj_set_size(start_btn, 180, 70);
    lv_obj_align(start_btn, LV_ALIGN_BOTTOM_MID, 0, -80);
    lv_obj_set_style_bg_color(start_btn, lv_color_hex(COL_GREEN), 0);
    lv_obj_set_style_radius(start_btn, 15, 0);
    
    lv_obj_t *start_lbl = lv_label_create(start_btn);
    lv_label_set_text(start_lbl, "START");
    lv_obj_set_style_text_font(start_lbl, &lv_font_montserrat_28, 0);
    lv_obj_center(start_lbl);
    
    lv_obj_add_event_cb(start_btn, [](lv_event_t*) {
        g_workout_active = true;
        g_workout_start = millis();
        Serial.printf("[WORKOUT] Started: %s\n", g_workout_types[g_workout_selected]);
    }, LV_EVENT_CLICKED, NULL);
    
    lv_obj_t *back = lv_btn_create(g_workout_screen);
    lv_obj_set_size(back, 140, 40);
    lv_obj_align(back, LV_ALIGN_BOTTOM_MID, 0, -20);
    lv_obj_set_style_bg_color(back, lv_color_hex(0x141414), 0);
    lv_obj_set_style_border_color(back, lv_color_hex(COL_GREEN), 0);
    lv_obj_set_style_border_width(back, 1, 0);
    lv_obj_t *bl = lv_label_create(back);
    lv_label_set_text(bl, LV_SYMBOL_LEFT " Back");
    lv_obj_center(bl);
    lv_obj_add_event_cb(back, [](lv_event_t*) { lv_scr_load(g_launcher_screen); }, LV_EVENT_CLICKED, NULL);
    
    lv_scr_load(g_workout_screen);
}

// =============================================================================
//  DEPTH GAUGE (Ultra Diving Feature!)
// =============================================================================
static lv_obj_t *g_depth_screen = nullptr;
static lv_obj_t *g_depth_label = nullptr;
static float g_depth_meters = 0.0f;

static void open_depth() {
    g_depth_screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(g_depth_screen, lv_color_hex(0x001a33), 0);  // Deep ocean blue
    
    lv_obj_t *title = lv_label_create(g_depth_screen);
    lv_label_set_text(title, "DEPTH GAUGE");
    lv_obj_set_style_text_color(title, lv_color_hex(COL_CYAN), 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_22, 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 30);
    
    // Large depth display
    g_depth_label = lv_label_create(g_depth_screen);
    lv_label_set_text(g_depth_label, "0.0");
    lv_obj_set_style_text_color(g_depth_label, lv_color_hex(COL_CYAN), 0);
    lv_obj_set_style_text_font(g_depth_label, &lv_font_montserrat_48, 0);
    lv_obj_align(g_depth_label, LV_ALIGN_CENTER, 0, -40);
    
    lv_obj_t *units = lv_label_create(g_depth_screen);
    lv_label_set_text(units, "METERS");
    lv_obj_set_style_text_color(units, lv_color_hex(COL_GRAY), 0);
    lv_obj_set_style_text_font(units, &lv_font_montserrat_18, 0);
    lv_obj_align(units, LV_ALIGN_CENTER, 0, 10);
    
    // Max depth
    lv_obj_t *max_lbl = lv_label_create(g_depth_screen);
    lv_label_set_text(max_lbl, "Max: 0.0m");
    lv_obj_set_style_text_color(max_lbl, lv_color_hex(COL_BLUE), 0);
    lv_obj_set_style_text_font(max_lbl, &lv_font_montserrat_16, 0);
    lv_obj_align(max_lbl, LV_ALIGN_CENTER, 0, 60);
    
    // Dive time
    lv_obj_t *time_lbl = lv_label_create(g_depth_screen);
    lv_label_set_text(time_lbl, "Dive Time: 00:00");
    lv_obj_set_style_text_color(time_lbl, lv_color_hex(COL_CYAN), 0);
    lv_obj_set_style_text_font(time_lbl, &lv_font_montserrat_16, 0);
    lv_obj_align(time_lbl, LV_ALIGN_BOTTOM_MID, 0, -80);
    
    // Simulate depth button
    lv_obj_t *sim_btn = lv_btn_create(g_depth_screen);
    lv_obj_set_size(sim_btn, 160, 50);
    lv_obj_align(sim_btn, LV_ALIGN_CENTER, 0, 120);
    lv_obj_set_style_bg_color(sim_btn, lv_color_hex(COL_BLUE), 0);
    lv_obj_t *sim_lbl = lv_label_create(sim_btn);
    lv_label_set_text(sim_lbl, "Simulate Dive");
    lv_obj_center(sim_lbl);
    lv_obj_add_event_cb(sim_btn, [](lv_event_t*) {
        g_depth_meters += 5.0f;
        if (g_depth_meters > 40.0f) g_depth_meters = 0;
        char buf[16];
        snprintf(buf, 16, "%.1f", g_depth_meters);
        lv_label_set_text(g_depth_label, buf);
    }, LV_EVENT_CLICKED, NULL);
    
    lv_obj_t *back = lv_btn_create(g_depth_screen);
    lv_obj_set_size(back, 140, 40);
    lv_obj_align(back, LV_ALIGN_BOTTOM_MID, 0, -20);
    lv_obj_set_style_bg_color(back, lv_color_hex(0x141414), 0);
    lv_obj_set_style_border_color(back, lv_color_hex(COL_CYAN), 0);
    lv_obj_set_style_border_width(back, 1, 0);
    lv_obj_t *bl = lv_label_create(back);
    lv_label_set_text(bl, LV_SYMBOL_LEFT " Back");
    lv_obj_center(bl);
    lv_obj_add_event_cb(back, [](lv_event_t*) { lv_scr_load(g_launcher_screen); }, LV_EVENT_CLICKED, NULL);
    
    lv_scr_load(g_depth_screen);
}

// =============================================================================
//  WAYPOINT APP (Ultra Navigation!)
// =============================================================================
static lv_obj_t *g_waypoint_screen = nullptr;
struct Waypoint { const char *name; int bearing; float distance; };
static Waypoint g_waypoints[] = {
    {"Camp", 45, 2.5f},
    {"Summit", 315, 5.2f},
    {"Trailhead", 180, 1.8f}
};

static void open_waypoint() {
    g_waypoint_screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(g_waypoint_screen, lv_color_hex(COL_BG), 0);
    
    lv_obj_t *title = lv_label_create(g_waypoint_screen);
    lv_label_set_text(title, "WAYPOINTS");
    lv_obj_set_style_text_color(title, lv_color_hex(COL_ORANGE), 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_22, 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 30);
    
    // Waypoint list
    for (int i = 0; i < 3; i++) {
        lv_obj_t *panel = lv_obj_create(g_waypoint_screen);
        lv_obj_set_size(panel, 350, 90);
        lv_obj_set_pos(panel, 30, 80 + i * 100);
        lv_obj_set_style_bg_color(panel, lv_color_hex(COL_PANEL), 0);
        lv_obj_set_style_radius(panel, 15, 0);
        
        lv_obj_t *name_lbl = lv_label_create(panel);
        lv_label_set_text(name_lbl, g_waypoints[i].name);
        lv_obj_set_style_text_font(name_lbl, &lv_font_montserrat_22, 0);
        lv_obj_set_style_text_color(name_lbl, lv_color_hex(COL_WHITE), 0);
        lv_obj_set_pos(name_lbl, 20, 15);
        
        char info[32];
        snprintf(info, 32, "%d° · %.1f km", g_waypoints[i].bearing, g_waypoints[i].distance);
        lv_obj_t *info_lbl = lv_label_create(panel);
        lv_label_set_text(info_lbl, info);
        lv_obj_set_style_text_color(info_lbl, lv_color_hex(COL_ORANGE), 0);
        lv_obj_set_style_text_font(info_lbl, &lv_font_montserrat_14, 0);
        lv_obj_set_pos(info_lbl, 20, 50);
    }
    
    lv_obj_t *back = lv_btn_create(g_waypoint_screen);
    lv_obj_set_size(back, 140, 40);
    lv_obj_align(back, LV_ALIGN_BOTTOM_MID, 0, -20);
    lv_obj_set_style_bg_color(back, lv_color_hex(0x141414), 0);
    lv_obj_set_style_border_color(back, lv_color_hex(COL_ORANGE), 0);
    lv_obj_set_style_border_width(back, 1, 0);
    lv_obj_t *bl = lv_label_create(back);
    lv_label_set_text(bl, LV_SYMBOL_LEFT " Back");
    lv_obj_center(bl);
    lv_obj_add_event_cb(back, [](lv_event_t*) { lv_scr_load(g_launcher_screen); }, LV_EVENT_CLICKED, NULL);
    
    lv_scr_load(g_waypoint_screen);
}

// =============================================================================
//  COMPASS APP — gyro-based heading + cardinal direction
// =============================================================================
static lv_obj_t *g_compass_screen = nullptr;
static lv_obj_t *g_compass_deg_lbl = nullptr;
static lv_obj_t *g_compass_card_lbl = nullptr;
static lv_obj_t *g_compass_needle = nullptr;
static float g_compass_heading = 0.0f;
static uint32_t g_compass_last_ms = 0;

static const char* compass_cardinal(float deg) {
    static const char *DIRS[] = {"N","NE","E","SE","S","SW","W","NW"};
    int idx = (int)((deg + 22.5f) / 45.0f) & 7;
    return DIRS[idx];
}

static void open_compass() {
    g_compass_screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(g_compass_screen, lv_color_hex(COL_BG), 0);
    lv_obj_clear_flag(g_compass_screen, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title = lv_label_create(g_compass_screen);
    lv_label_set_text(title, "COMPASS");
    lv_obj_set_style_text_color(title, lv_color_hex(COL_RED), 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_22, 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 20);

    // Outer dial
    lv_obj_t *dial = lv_obj_create(g_compass_screen);
    lv_obj_set_size(dial, 280, 280);
    lv_obj_align(dial, LV_ALIGN_CENTER, 0, -10);
    lv_obj_set_style_radius(dial, 140, 0);
    lv_obj_set_style_bg_color(dial, lv_color_hex(0x0A0A0A), 0);
    lv_obj_set_style_border_color(dial, lv_color_hex(COL_RED), 0);
    lv_obj_set_style_border_width(dial, 3, 0);
    lv_obj_clear_flag(dial, LV_OBJ_FLAG_SCROLLABLE);

    // Cardinal marks
    const char *cards[] = {"N","E","S","W"};
    uint32_t cardc[] = {COL_RED, COL_WHITE, COL_WHITE, COL_WHITE};
    int cx[] = {0, 110, 0, -110};
    int cy[] = {-110, 0, 110, 0};
    for (int i = 0; i < 4; i++) {
        lv_obj_t *m = lv_label_create(g_compass_screen);
        lv_label_set_text(m, cards[i]);
        lv_obj_set_style_text_color(m, lv_color_hex(cardc[i]), 0);
        lv_obj_set_style_text_font(m, &lv_font_montserrat_28, 0);
        lv_obj_align(m, LV_ALIGN_CENTER, cx[i], cy[i] - 10);
    }

    // Needle (simple line)
    g_compass_needle = lv_obj_create(g_compass_screen);
    lv_obj_set_size(g_compass_needle, 6, 160);
    lv_obj_align(g_compass_needle, LV_ALIGN_CENTER, 0, -10);
    lv_obj_set_style_bg_color(g_compass_needle, lv_color_hex(COL_RED), 0);
    lv_obj_set_style_border_width(g_compass_needle, 0, 0);
    lv_obj_set_style_radius(g_compass_needle, 3, 0);
    lv_obj_set_style_transform_pivot_x(g_compass_needle, 3, 0);
    lv_obj_set_style_transform_pivot_y(g_compass_needle, 80, 0);

    g_compass_deg_lbl = lv_label_create(g_compass_screen);
    lv_label_set_text(g_compass_deg_lbl, "0°");
    lv_obj_set_style_text_color(g_compass_deg_lbl, lv_color_hex(COL_WHITE), 0);
    lv_obj_set_style_text_font(g_compass_deg_lbl, &lv_font_montserrat_48, 0);
    lv_obj_align(g_compass_deg_lbl, LV_ALIGN_BOTTOM_MID, 0, -110);

    g_compass_card_lbl = lv_label_create(g_compass_screen);
    lv_label_set_text(g_compass_card_lbl, "N");
    lv_obj_set_style_text_color(g_compass_card_lbl, lv_color_hex(COL_RED), 0);
    lv_obj_set_style_text_font(g_compass_card_lbl, &lv_font_montserrat_22, 0);
    lv_obj_align(g_compass_card_lbl, LV_ALIGN_BOTTOM_MID, 0, -70);

    lv_obj_t *back = lv_btn_create(g_compass_screen);
    lv_obj_set_size(back, 140, 40);
    lv_obj_align(back, LV_ALIGN_BOTTOM_MID, 0, -15);
    lv_obj_set_style_bg_color(back, lv_color_hex(0x141414), 0);
    lv_obj_set_style_border_color(back, lv_color_hex(COL_RED), 0);
    lv_obj_set_style_border_width(back, 1, 0);
    lv_obj_t *bl = lv_label_create(back);
    lv_label_set_text(bl, LV_SYMBOL_LEFT " Back");
    lv_obj_center(bl);
    lv_obj_add_event_cb(back, [](lv_event_t*) {
        g_compass_needle = nullptr; g_compass_deg_lbl = nullptr; g_compass_card_lbl = nullptr;
        lv_scr_load(g_launcher_screen);
    }, LV_EVENT_CLICKED, NULL);

    g_compass_heading = 0;
    g_compass_last_ms = millis();
    lv_scr_load(g_compass_screen);
}

// =============================================================================
//  BREATHE APP — guided breathing exercise (4-7-8 / box breathing)
// =============================================================================
static lv_obj_t *g_breathe_screen = nullptr;
static lv_obj_t *g_breathe_circle = nullptr;
static lv_obj_t *g_breathe_label = nullptr;
static lv_obj_t *g_breathe_count = nullptr;
static bool g_breathe_running = false;
static uint32_t g_breathe_start = 0;
static int g_breathe_phase = 0; // 0=inhale, 1=hold, 2=exhale
static int g_breathe_cycle = 0;

static void open_breathe() {
    g_breathe_screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(g_breathe_screen, lv_color_hex(0x001020), 0);
    lv_obj_clear_flag(g_breathe_screen, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title = lv_label_create(g_breathe_screen);
    lv_label_set_text(title, "BREATHE");
    lv_obj_set_style_text_color(title, lv_color_hex(COL_CYAN), 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_22, 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 25);

    g_breathe_circle = lv_obj_create(g_breathe_screen);
    lv_obj_set_size(g_breathe_circle, 120, 120);
    lv_obj_align(g_breathe_circle, LV_ALIGN_CENTER, 0, -20);
    lv_obj_set_style_radius(g_breathe_circle, 60, 0);
    lv_obj_set_style_bg_color(g_breathe_circle, lv_color_hex(COL_CYAN), 0);
    lv_obj_set_style_bg_opa(g_breathe_circle, 150, 0);
    lv_obj_set_style_border_color(g_breathe_circle, lv_color_hex(COL_CYAN), 0);
    lv_obj_set_style_border_width(g_breathe_circle, 3, 0);

    g_breathe_label = lv_label_create(g_breathe_screen);
    lv_label_set_text(g_breathe_label, "Tap Start");
    lv_obj_set_style_text_color(g_breathe_label, lv_color_hex(COL_WHITE), 0);
    lv_obj_set_style_text_font(g_breathe_label, &lv_font_montserrat_28, 0);
    lv_obj_align(g_breathe_label, LV_ALIGN_CENTER, 0, 90);

    g_breathe_count = lv_label_create(g_breathe_screen);
    lv_label_set_text(g_breathe_count, "Cycle 0/5");
    lv_obj_set_style_text_color(g_breathe_count, lv_color_hex(COL_GRAY), 0);
    lv_obj_set_style_text_font(g_breathe_count, &lv_font_montserrat_14, 0);
    lv_obj_align(g_breathe_count, LV_ALIGN_CENTER, 0, 130);

    lv_obj_t *start = lv_btn_create(g_breathe_screen);
    lv_obj_set_size(start, 160, 50);
    lv_obj_align(start, LV_ALIGN_BOTTOM_MID, 0, -75);
    lv_obj_set_style_bg_color(start, lv_color_hex(COL_CYAN), 0);
    lv_obj_set_style_radius(start, 25, 0);
    lv_obj_t *sl = lv_label_create(start);
    lv_label_set_text(sl, "Start");
    lv_obj_set_style_text_color(sl, lv_color_hex(0x000000), 0);
    lv_obj_set_style_text_font(sl, &lv_font_montserrat_22, 0);
    lv_obj_center(sl);
    lv_obj_add_event_cb(start, [](lv_event_t*) {
        g_breathe_running = !g_breathe_running;
        if (g_breathe_running) {
            g_breathe_start = millis();
            g_breathe_phase = 0; g_breathe_cycle = 0;
        }
    }, LV_EVENT_CLICKED, NULL);

    lv_obj_t *back = lv_btn_create(g_breathe_screen);
    lv_obj_set_size(back, 140, 40);
    lv_obj_align(back, LV_ALIGN_BOTTOM_MID, 0, -15);
    lv_obj_set_style_bg_color(back, lv_color_hex(0x141414), 0);
    lv_obj_set_style_border_color(back, lv_color_hex(COL_CYAN), 0);
    lv_obj_set_style_border_width(back, 1, 0);
    lv_obj_t *bl = lv_label_create(back);
    lv_label_set_text(bl, LV_SYMBOL_LEFT " Back");
    lv_obj_center(bl);
    lv_obj_add_event_cb(back, [](lv_event_t*) {
        g_breathe_running = false;
        g_breathe_circle = nullptr; g_breathe_label = nullptr; g_breathe_count = nullptr;
        lv_scr_load(g_launcher_screen);
    }, LV_EVENT_CLICKED, NULL);

    lv_scr_load(g_breathe_screen);
}

// =============================================================================
//  HEART RATE APP — simulated BPM with pulse animation
// =============================================================================
static lv_obj_t *g_hr_screen = nullptr;
static lv_obj_t *g_hr_bpm_lbl = nullptr;
static lv_obj_t *g_hr_zone_lbl = nullptr;
static lv_obj_t *g_hr_heart = nullptr;
static int g_hr_bpm = 72;
static uint32_t g_hr_last_ms = 0;
static bool g_hr_pulse_state = false;

static const char* hr_zone(int bpm) {
    if (bpm < 60) return "RESTING";
    if (bpm < 100) return "NORMAL";
    if (bpm < 130) return "FAT BURN";
    if (bpm < 160) return "CARDIO";
    return "PEAK";
}
static uint32_t hr_zone_color(int bpm) {
    if (bpm < 60)  return COL_BLUE;
    if (bpm < 100) return COL_GREEN;
    if (bpm < 130) return COL_ORANGE;
    if (bpm < 160) return COL_PINK;
    return COL_RED;
}

static void open_heart_rate() {
    g_hr_screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(g_hr_screen, lv_color_hex(COL_BG), 0);
    lv_obj_clear_flag(g_hr_screen, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title = lv_label_create(g_hr_screen);
    lv_label_set_text(title, "HEART RATE");
    lv_obj_set_style_text_color(title, lv_color_hex(COL_PINK), 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_22, 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 25);

    g_hr_heart = lv_obj_create(g_hr_screen);
    lv_obj_set_size(g_hr_heart, 140, 140);
    lv_obj_align(g_hr_heart, LV_ALIGN_CENTER, 0, -30);
    lv_obj_set_style_radius(g_hr_heart, 70, 0);
    lv_obj_set_style_bg_color(g_hr_heart, lv_color_hex(COL_PINK), 0);
    lv_obj_set_style_bg_opa(g_hr_heart, 120, 0);
    lv_obj_set_style_border_color(g_hr_heart, lv_color_hex(COL_PINK), 0);
    lv_obj_set_style_border_width(g_hr_heart, 4, 0);

    lv_obj_t *hl = lv_label_create(g_hr_heart);
    lv_label_set_text(hl, LV_SYMBOL_PLUS);
    lv_obj_set_style_text_color(hl, lv_color_hex(COL_WHITE), 0);
    lv_obj_set_style_text_font(hl, &lv_font_montserrat_48, 0);
    lv_obj_center(hl);

    g_hr_bpm_lbl = lv_label_create(g_hr_screen);
    lv_label_set_text(g_hr_bpm_lbl, "72 BPM");
    lv_obj_set_style_text_color(g_hr_bpm_lbl, lv_color_hex(COL_WHITE), 0);
    lv_obj_set_style_text_font(g_hr_bpm_lbl, &lv_font_montserrat_48, 0);
    lv_obj_align(g_hr_bpm_lbl, LV_ALIGN_CENTER, 0, 80);

    g_hr_zone_lbl = lv_label_create(g_hr_screen);
    lv_label_set_text(g_hr_zone_lbl, "NORMAL");
    lv_obj_set_style_text_color(g_hr_zone_lbl, lv_color_hex(COL_GREEN), 0);
    lv_obj_set_style_text_font(g_hr_zone_lbl, &lv_font_montserrat_18, 0);
    lv_obj_align(g_hr_zone_lbl, LV_ALIGN_CENTER, 0, 130);

    lv_obj_t *back = lv_btn_create(g_hr_screen);
    lv_obj_set_size(back, 140, 40);
    lv_obj_align(back, LV_ALIGN_BOTTOM_MID, 0, -20);
    lv_obj_set_style_bg_color(back, lv_color_hex(0x141414), 0);
    lv_obj_set_style_border_color(back, lv_color_hex(COL_PINK), 0);
    lv_obj_set_style_border_width(back, 1, 0);
    lv_obj_t *bl = lv_label_create(back);
    lv_label_set_text(bl, LV_SYMBOL_LEFT " Back");
    lv_obj_center(bl);
    lv_obj_add_event_cb(back, [](lv_event_t*) {
        g_hr_heart = nullptr; g_hr_bpm_lbl = nullptr; g_hr_zone_lbl = nullptr;
        lv_scr_load(g_launcher_screen);
    }, LV_EVENT_CLICKED, NULL);

    g_hr_bpm = 65 + (esp_random() % 25);
    g_hr_last_ms = millis();
    lv_scr_load(g_hr_screen);
}

// =============================================================================
//  NOTES APP — read-only viewer for /notes.txt on SD card
// =============================================================================
static lv_obj_t *g_notes_screen = nullptr;

static void open_notes() {
    g_notes_screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(g_notes_screen, lv_color_hex(COL_BG), 0);

    lv_obj_t *title = lv_label_create(g_notes_screen);
    lv_label_set_text(title, "NOTES");
    lv_obj_set_style_text_color(title, lv_color_hex(COL_ORANGE), 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_22, 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 25);

    lv_obj_t *panel = lv_obj_create(g_notes_screen);
    lv_obj_set_size(panel, 350, 320);
    lv_obj_align(panel, LV_ALIGN_CENTER, 0, -10);
    lv_obj_set_style_bg_color(panel, lv_color_hex(COL_PANEL), 0);
    lv_obj_set_style_radius(panel, 14, 0);
    lv_obj_set_style_border_width(panel, 1, 0);
    lv_obj_set_style_border_color(panel, lv_color_hex(COL_ORANGE), 0);

    lv_obj_t *body = lv_label_create(panel);
    lv_label_set_long_mode(body, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(body, 320);
    lv_obj_align(body, LV_ALIGN_TOP_LEFT, 5, 5);
    lv_obj_set_style_text_color(body, lv_color_hex(COL_WHITE), 0);
    lv_obj_set_style_text_font(body, &lv_font_montserrat_14, 0);

    // Try reading /notes.txt from SD
    String text = "";
    File f = SD_MMC.open("/notes.txt");
    if (f) {
        while (f.available() && text.length() < 800) text += (char)f.read();
        f.close();
    }
    if (text.length() == 0) {
        text = "No notes found.\n\nCreate /notes.txt on the SD card to display your notes here.\n\nSupports up to 800 characters of plain text.";
    }
    lv_label_set_text(body, text.c_str());

    lv_obj_t *back = lv_btn_create(g_notes_screen);
    lv_obj_set_size(back, 140, 40);
    lv_obj_align(back, LV_ALIGN_BOTTOM_MID, 0, -20);
    lv_obj_set_style_bg_color(back, lv_color_hex(0x141414), 0);
    lv_obj_set_style_border_color(back, lv_color_hex(COL_ORANGE), 0);
    lv_obj_set_style_border_width(back, 1, 0);
    lv_obj_t *bl = lv_label_create(back);
    lv_label_set_text(bl, LV_SYMBOL_LEFT " Back");
    lv_obj_center(bl);
    lv_obj_add_event_cb(back, [](lv_event_t*) { lv_scr_load(g_launcher_screen); }, LV_EVENT_CLICKED, NULL);

    lv_scr_load(g_notes_screen);
}

// =============================================================================
//  POMODORO APP — 25min focus / 5min break cycles
// =============================================================================
static lv_obj_t *g_pomo_screen = nullptr;
static lv_obj_t *g_pomo_label = nullptr;
static lv_obj_t *g_pomo_phase_lbl = nullptr;
static lv_obj_t *g_pomo_cycle_lbl = nullptr;
static lv_obj_t *g_pomo_arc = nullptr;
static bool g_pomo_running = false;
static int g_pomo_phase = 0;     // 0=focus, 1=short break, 2=long break
static int g_pomo_cycle = 0;
static uint32_t g_pomo_remaining = 25 * 60;
static uint32_t g_pomo_duration  = 25 * 60;
static uint32_t g_pomo_start = 0;

static void pomo_phase_setup(int phase) {
    g_pomo_phase = phase;
    if (phase == 0) { g_pomo_duration = 25 * 60; }      // focus
    else if (phase == 1) { g_pomo_duration = 5 * 60; }  // short break
    else { g_pomo_duration = 15 * 60; }                  // long break
    g_pomo_remaining = g_pomo_duration;
    if (g_pomo_phase_lbl) {
        lv_label_set_text(g_pomo_phase_lbl, phase == 0 ? "FOCUS" : (phase == 1 ? "BREAK" : "LONG BREAK"));
        lv_obj_set_style_text_color(g_pomo_phase_lbl,
            lv_color_hex(phase == 0 ? COL_RED : COL_GREEN), 0);
    }
}

static void open_pomodoro() {
    g_pomo_screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(g_pomo_screen, lv_color_hex(COL_BG), 0);
    lv_obj_clear_flag(g_pomo_screen, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title = lv_label_create(g_pomo_screen);
    lv_label_set_text(title, "POMODORO");
    lv_obj_set_style_text_color(title, lv_color_hex(COL_RED), 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_22, 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 25);

    g_pomo_arc = lv_arc_create(g_pomo_screen);
    lv_obj_set_size(g_pomo_arc, 280, 280);
    lv_obj_align(g_pomo_arc, LV_ALIGN_CENTER, 0, -20);
    lv_arc_set_rotation(g_pomo_arc, 270);
    lv_arc_set_bg_angles(g_pomo_arc, 0, 360);
    lv_arc_set_range(g_pomo_arc, 0, 100);
    lv_arc_set_value(g_pomo_arc, 100);
    lv_obj_remove_style(g_pomo_arc, NULL, LV_PART_KNOB);
    lv_obj_clear_flag(g_pomo_arc, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_arc_width(g_pomo_arc, 14, LV_PART_MAIN);
    lv_obj_set_style_arc_width(g_pomo_arc, 14, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(g_pomo_arc, lv_color_hex(0x1C1C1C), LV_PART_MAIN);
    lv_obj_set_style_arc_color(g_pomo_arc, lv_color_hex(COL_RED), LV_PART_INDICATOR);

    g_pomo_label = lv_label_create(g_pomo_screen);
    lv_label_set_text(g_pomo_label, "25:00");
    lv_obj_set_style_text_color(g_pomo_label, lv_color_hex(COL_WHITE), 0);
    lv_obj_set_style_text_font(g_pomo_label, &lv_font_montserrat_48, 0);
    lv_obj_align(g_pomo_label, LV_ALIGN_CENTER, 0, -30);

    g_pomo_phase_lbl = lv_label_create(g_pomo_screen);
    lv_label_set_text(g_pomo_phase_lbl, "FOCUS");
    lv_obj_set_style_text_color(g_pomo_phase_lbl, lv_color_hex(COL_RED), 0);
    lv_obj_set_style_text_font(g_pomo_phase_lbl, &lv_font_montserrat_22, 0);
    lv_obj_align(g_pomo_phase_lbl, LV_ALIGN_CENTER, 0, 20);

    g_pomo_cycle_lbl = lv_label_create(g_pomo_screen);
    lv_label_set_text(g_pomo_cycle_lbl, "Cycle 1");
    lv_obj_set_style_text_color(g_pomo_cycle_lbl, lv_color_hex(COL_GRAY), 0);
    lv_obj_set_style_text_font(g_pomo_cycle_lbl, &lv_font_montserrat_14, 0);
    lv_obj_align(g_pomo_cycle_lbl, LV_ALIGN_CENTER, 0, 60);

    lv_obj_t *start = lv_btn_create(g_pomo_screen);
    lv_obj_set_size(start, 70, 70);
    lv_obj_align(start, LV_ALIGN_BOTTOM_MID, -50, -90);
    lv_obj_set_style_radius(start, 35, 0);
    lv_obj_set_style_bg_color(start, lv_color_hex(COL_GREEN), 0);
    lv_obj_t *sl = lv_label_create(start);
    lv_label_set_text(sl, LV_SYMBOL_PLAY);
    lv_obj_set_style_text_font(sl, &lv_font_montserrat_28, 0);
    lv_obj_center(sl);
    lv_obj_add_event_cb(start, [](lv_event_t*) {
        g_pomo_running = !g_pomo_running;
        if (g_pomo_running) g_pomo_start = millis();
        else g_pomo_remaining = g_pomo_remaining; // freeze
    }, LV_EVENT_CLICKED, NULL);

    lv_obj_t *reset = lv_btn_create(g_pomo_screen);
    lv_obj_set_size(reset, 70, 70);
    lv_obj_align(reset, LV_ALIGN_BOTTOM_MID, 50, -90);
    lv_obj_set_style_radius(reset, 35, 0);
    lv_obj_set_style_bg_color(reset, lv_color_hex(COL_RED), 0);
    lv_obj_t *rl = lv_label_create(reset);
    lv_label_set_text(rl, LV_SYMBOL_REFRESH);
    lv_obj_set_style_text_font(rl, &lv_font_montserrat_28, 0);
    lv_obj_center(rl);
    lv_obj_add_event_cb(reset, [](lv_event_t*) {
        g_pomo_running = false; g_pomo_cycle = 0;
        pomo_phase_setup(0);
        if (g_pomo_label) lv_label_set_text(g_pomo_label, "25:00");
        if (g_pomo_arc)   lv_arc_set_value(g_pomo_arc, 100);
        if (g_pomo_cycle_lbl) lv_label_set_text(g_pomo_cycle_lbl, "Cycle 1");
    }, LV_EVENT_CLICKED, NULL);

    lv_obj_t *back = lv_btn_create(g_pomo_screen);
    lv_obj_set_size(back, 140, 40);
    lv_obj_align(back, LV_ALIGN_BOTTOM_MID, 0, -20);
    lv_obj_set_style_bg_color(back, lv_color_hex(0x141414), 0);
    lv_obj_set_style_border_color(back, lv_color_hex(COL_RED), 0);
    lv_obj_set_style_border_width(back, 1, 0);
    lv_obj_t *bl = lv_label_create(back);
    lv_label_set_text(bl, LV_SYMBOL_LEFT " Back");
    lv_obj_center(bl);
    lv_obj_add_event_cb(back, [](lv_event_t*) {
        g_pomo_running = false;
        g_pomo_label = nullptr; g_pomo_phase_lbl = nullptr; g_pomo_cycle_lbl = nullptr; g_pomo_arc = nullptr;
        lv_scr_load(g_launcher_screen);
    }, LV_EVENT_CLICKED, NULL);

    pomo_phase_setup(0);
    g_pomo_cycle = 0;
    lv_scr_load(g_pomo_screen);
}

// =============================================================================
//  WORLD CLOCK APP — 6 major timezones
// =============================================================================
struct TzCity { const char *name; int offset_h; };
static const TzCity TZ_CITIES[] = {
    {"NEW YORK",   -5},
    {"LONDON",      0},
    {"PARIS",       1},
    {"DUBAI",       4},
    {"TOKYO",       9},
    {"SYDNEY",     11},
};
#define TZ_COUNT (sizeof(TZ_CITIES)/sizeof(TZ_CITIES[0]))
static lv_obj_t *g_world_screen = nullptr;
static lv_obj_t *g_world_labels[TZ_COUNT] = {nullptr};

static void open_world_clock() {
    g_world_screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(g_world_screen, lv_color_hex(COL_BG), 0);
    lv_obj_clear_flag(g_world_screen, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title = lv_label_create(g_world_screen);
    lv_label_set_text(title, "WORLD CLOCK");
    lv_obj_set_style_text_color(title, lv_color_hex(COL_BLUE), 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_22, 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 20);

    for (int i = 0; i < (int)TZ_COUNT; i++) {
        lv_obj_t *p = lv_obj_create(g_world_screen);
        lv_obj_set_size(p, 360, 56);
        lv_obj_align(p, LV_ALIGN_TOP_MID, 0, 60 + i * 62);
        lv_obj_set_style_bg_color(p, lv_color_hex(COL_PANEL), 0);
        lv_obj_set_style_radius(p, 12, 0);
        lv_obj_set_style_border_width(p, 0, 0);
        lv_obj_clear_flag(p, LV_OBJ_FLAG_SCROLLABLE);

        lv_obj_t *name = lv_label_create(p);
        lv_label_set_text(name, TZ_CITIES[i].name);
        lv_obj_set_style_text_color(name, lv_color_hex(COL_WHITE), 0);
        lv_obj_set_style_text_font(name, &lv_font_montserrat_18, 0);
        lv_obj_align(name, LV_ALIGN_LEFT_MID, 15, 0);

        g_world_labels[i] = lv_label_create(p);
        lv_label_set_text(g_world_labels[i], "--:--");
        lv_obj_set_style_text_color(g_world_labels[i], lv_color_hex(COL_BLUE), 0);
        lv_obj_set_style_text_font(g_world_labels[i], &lv_font_montserrat_28, 0);
        lv_obj_align(g_world_labels[i], LV_ALIGN_RIGHT_MID, -15, 0);
    }

    lv_obj_t *back = lv_btn_create(g_world_screen);
    lv_obj_set_size(back, 140, 40);
    lv_obj_align(back, LV_ALIGN_BOTTOM_MID, 0, -10);
    lv_obj_set_style_bg_color(back, lv_color_hex(0x141414), 0);
    lv_obj_set_style_border_color(back, lv_color_hex(COL_BLUE), 0);
    lv_obj_set_style_border_width(back, 1, 0);
    lv_obj_t *bl = lv_label_create(back);
    lv_label_set_text(bl, LV_SYMBOL_LEFT " Back");
    lv_obj_center(bl);
    lv_obj_add_event_cb(back, [](lv_event_t*) {
        for (int i = 0; i < (int)TZ_COUNT; i++) g_world_labels[i] = nullptr;
        lv_scr_load(g_launcher_screen);
    }, LV_EVENT_CLICKED, NULL);

    lv_scr_load(g_world_screen);
}

// =============================================================================
//  SNAKE GAME — classic snake with on-screen direction pad
// =============================================================================
#define SNAKE_COLS  16
#define SNAKE_ROWS  16
#define SNAKE_CELL  20
#define SNAKE_MAX  128
static lv_obj_t *g_snake_screen = nullptr;
static lv_obj_t *g_snake_canvas = nullptr;
static lv_obj_t *g_snake_score_lbl = nullptr;
static uint8_t  g_snake_x[SNAKE_MAX], g_snake_y[SNAKE_MAX];
static int      g_snake_len = 0;
static int      g_snake_dx = 1, g_snake_dy = 0;
static uint8_t  g_food_x = 0, g_food_y = 0;
static uint32_t g_snake_score = 0;
static uint32_t g_snake_last_tick = 0;
static bool     g_snake_running = false;

static void snake_spawn_food() {
    g_food_x = esp_random() % SNAKE_COLS;
    g_food_y = esp_random() % SNAKE_ROWS;
}

static void snake_reset() {
    g_snake_len = 4;
    for (int i = 0; i < g_snake_len; i++) { g_snake_x[i] = 8 - i; g_snake_y[i] = 8; }
    g_snake_dx = 1; g_snake_dy = 0;
    g_snake_score = 0;
    snake_spawn_food();
}

static void snake_redraw() {
    if (!g_snake_canvas) return;
    lv_obj_clean(g_snake_canvas);
    // food
    lv_obj_t *f = lv_obj_create(g_snake_canvas);
    lv_obj_set_size(f, SNAKE_CELL - 2, SNAKE_CELL - 2);
    lv_obj_set_pos(f, g_food_x * SNAKE_CELL, g_food_y * SNAKE_CELL);
    lv_obj_set_style_bg_color(f, lv_color_hex(COL_RED), 0);
    lv_obj_set_style_radius(f, 4, 0);
    lv_obj_set_style_border_width(f, 0, 0);
    // snake
    for (int i = 0; i < g_snake_len; i++) {
        lv_obj_t *s = lv_obj_create(g_snake_canvas);
        lv_obj_set_size(s, SNAKE_CELL - 2, SNAKE_CELL - 2);
        lv_obj_set_pos(s, g_snake_x[i] * SNAKE_CELL, g_snake_y[i] * SNAKE_CELL);
        lv_obj_set_style_bg_color(s, lv_color_hex(i == 0 ? COL_GREEN : 0x008833), 0);
        lv_obj_set_style_radius(s, 3, 0);
        lv_obj_set_style_border_width(s, 0, 0);
    }
}

static void snake_dpad_cb(lv_event_t *e) {
    int dir = (int)(intptr_t)lv_event_get_user_data(e);
    // 0=up,1=right,2=down,3=left
    int ndx[] = {0, 1, 0, -1};
    int ndy[] = {-1, 0, 1, 0};
    // ignore reverse
    if (g_snake_dx == -ndx[dir] && g_snake_dy == -ndy[dir]) return;
    g_snake_dx = ndx[dir]; g_snake_dy = ndy[dir];
}

static void open_snake() {
    g_snake_screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(g_snake_screen, lv_color_hex(COL_BG), 0);
    lv_obj_clear_flag(g_snake_screen, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title = lv_label_create(g_snake_screen);
    lv_label_set_text(title, "SNAKE");
    lv_obj_set_style_text_color(title, lv_color_hex(COL_GREEN), 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_22, 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 10);

    g_snake_score_lbl = lv_label_create(g_snake_screen);
    lv_label_set_text(g_snake_score_lbl, "Score 0");
    lv_obj_set_style_text_color(g_snake_score_lbl, lv_color_hex(COL_WHITE), 0);
    lv_obj_set_style_text_font(g_snake_score_lbl, &lv_font_montserrat_16, 0);
    lv_obj_align(g_snake_score_lbl, LV_ALIGN_TOP_MID, 0, 40);

    // Play area
    g_snake_canvas = lv_obj_create(g_snake_screen);
    lv_obj_set_size(g_snake_canvas, SNAKE_COLS * SNAKE_CELL, SNAKE_ROWS * SNAKE_CELL);
    lv_obj_align(g_snake_canvas, LV_ALIGN_TOP_MID, 0, 70);
    lv_obj_set_style_bg_color(g_snake_canvas, lv_color_hex(0x0A0A0A), 0);
    lv_obj_set_style_border_color(g_snake_canvas, lv_color_hex(COL_GREEN), 0);
    lv_obj_set_style_border_width(g_snake_canvas, 2, 0);
    lv_obj_set_style_pad_all(g_snake_canvas, 0, 0);
    lv_obj_clear_flag(g_snake_canvas, LV_OBJ_FLAG_SCROLLABLE);

    // D-pad
    const char *icons[] = {LV_SYMBOL_UP, LV_SYMBOL_RIGHT, LV_SYMBOL_DOWN, LV_SYMBOL_LEFT};
    int px[] = {0, 80, 0, -80};
    int py[] = {-50, 0, 50, 0};
    for (int i = 0; i < 4; i++) {
        lv_obj_t *b = lv_btn_create(g_snake_screen);
        lv_obj_set_size(b, 60, 60);
        lv_obj_align(b, LV_ALIGN_BOTTOM_MID, px[i], py[i] - 35);
        lv_obj_set_style_radius(b, 30, 0);
        lv_obj_set_style_bg_color(b, lv_color_hex(COL_GREEN), 0);
        lv_obj_t *l = lv_label_create(b);
        lv_label_set_text(l, icons[i]);
        lv_obj_set_style_text_color(l, lv_color_hex(0x000000), 0);
        lv_obj_set_style_text_font(l, &lv_font_montserrat_22, 0);
        lv_obj_center(l);
        lv_obj_add_event_cb(b, snake_dpad_cb, LV_EVENT_CLICKED, (void*)(intptr_t)i);
    }

    lv_obj_t *back = lv_btn_create(g_snake_screen);
    lv_obj_set_size(back, 110, 36);
    lv_obj_align(back, LV_ALIGN_BOTTOM_LEFT, 8, -8);
    lv_obj_set_style_bg_color(back, lv_color_hex(0x141414), 0);
    lv_obj_set_style_border_color(back, lv_color_hex(COL_GREEN), 0);
    lv_obj_set_style_border_width(back, 1, 0);
    lv_obj_t *bl = lv_label_create(back);
    lv_label_set_text(bl, LV_SYMBOL_LEFT " Back");
    lv_obj_center(bl);
    lv_obj_add_event_cb(back, [](lv_event_t*) {
        g_snake_running = false;
        g_snake_canvas = nullptr; g_snake_score_lbl = nullptr;
        lv_scr_load(g_launcher_screen);
    }, LV_EVENT_CLICKED, NULL);

    snake_reset();
    snake_redraw();
    g_snake_running = true;
    g_snake_last_tick = millis();
    lv_scr_load(g_snake_screen);
}

// =============================================================================
//  DICE ROLLER — D6, D20, D100
// =============================================================================
static lv_obj_t *g_dice_screen = nullptr;
static lv_obj_t *g_dice_result = nullptr;
static lv_obj_t *g_dice_type_lbl = nullptr;
static int g_dice_sides = 6;

static void open_dice() {
    g_dice_screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(g_dice_screen, lv_color_hex(COL_BG), 0);

    lv_obj_t *title = lv_label_create(g_dice_screen);
    lv_label_set_text(title, "DICE ROLLER");
    lv_obj_set_style_text_color(title, lv_color_hex(COL_ORANGE), 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_22, 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 25);

    g_dice_result = lv_label_create(g_dice_screen);
    lv_label_set_text(g_dice_result, "?");
    lv_obj_set_style_text_color(g_dice_result, lv_color_hex(COL_WHITE), 0);
    lv_obj_set_style_text_font(g_dice_result, &lv_font_montserrat_48, 0);
    lv_obj_align(g_dice_result, LV_ALIGN_CENTER, 0, -30);

    g_dice_type_lbl = lv_label_create(g_dice_screen);
    lv_label_set_text(g_dice_type_lbl, "D6");
    lv_obj_set_style_text_color(g_dice_type_lbl, lv_color_hex(COL_ORANGE), 0);
    lv_obj_set_style_text_font(g_dice_type_lbl, &lv_font_montserrat_22, 0);
    lv_obj_align(g_dice_type_lbl, LV_ALIGN_CENTER, 0, 30);

    int sides[] = {6, 20, 100};
    const char *labels[] = {"D6", "D20", "D100"};
    for (int i = 0; i < 3; i++) {
        lv_obj_t *b = lv_btn_create(g_dice_screen);
        lv_obj_set_size(b, 90, 50);
        lv_obj_align(b, LV_ALIGN_CENTER, (i - 1) * 110, 100);
        lv_obj_set_style_bg_color(b, lv_color_hex(COL_PANEL), 0);
        lv_obj_t *l = lv_label_create(b);
        lv_label_set_text(l, labels[i]);
        lv_obj_set_style_text_color(l, lv_color_hex(COL_WHITE), 0);
        lv_obj_set_style_text_font(l, &lv_font_montserrat_18, 0);
        lv_obj_center(l);
        int s = sides[i];
        lv_obj_add_event_cb(b, [](lv_event_t *e) {
            g_dice_sides = (int)(intptr_t)lv_event_get_user_data(e);
            const char *t = g_dice_sides == 6 ? "D6" : (g_dice_sides == 20 ? "D20" : "D100");
            if (g_dice_type_lbl) lv_label_set_text(g_dice_type_lbl, t);
        }, LV_EVENT_CLICKED, (void*)(intptr_t)s);
    }

    lv_obj_t *roll = lv_btn_create(g_dice_screen);
    lv_obj_set_size(roll, 200, 60);
    lv_obj_align(roll, LV_ALIGN_BOTTOM_MID, 0, -80);
    lv_obj_set_style_radius(roll, 30, 0);
    lv_obj_set_style_bg_color(roll, lv_color_hex(COL_ORANGE), 0);
    lv_obj_t *rl = lv_label_create(roll);
    lv_label_set_text(rl, "ROLL");
    lv_obj_set_style_text_color(rl, lv_color_hex(0x000000), 0);
    lv_obj_set_style_text_font(rl, &lv_font_montserrat_28, 0);
    lv_obj_center(rl);
    lv_obj_add_event_cb(roll, [](lv_event_t*) {
        int v = (esp_random() % g_dice_sides) + 1;
        char buf[8]; snprintf(buf, 8, "%d", v);
        if (g_dice_result) lv_label_set_text(g_dice_result, buf);
    }, LV_EVENT_CLICKED, NULL);

    lv_obj_t *back = lv_btn_create(g_dice_screen);
    lv_obj_set_size(back, 140, 40);
    lv_obj_align(back, LV_ALIGN_BOTTOM_MID, 0, -15);
    lv_obj_set_style_bg_color(back, lv_color_hex(0x141414), 0);
    lv_obj_set_style_border_color(back, lv_color_hex(COL_ORANGE), 0);
    lv_obj_set_style_border_width(back, 1, 0);
    lv_obj_t *bl = lv_label_create(back);
    lv_label_set_text(bl, LV_SYMBOL_LEFT " Back");
    lv_obj_center(bl);
    lv_obj_add_event_cb(back, [](lv_event_t*) {
        g_dice_result = nullptr; g_dice_type_lbl = nullptr;
        lv_scr_load(g_launcher_screen);
    }, LV_EVENT_CLICKED, NULL);

    lv_scr_load(g_dice_screen);
}

// =============================================================================
//  REACTION TEST — measure reaction time
// =============================================================================
static lv_obj_t *g_rxn_screen = nullptr;
static lv_obj_t *g_rxn_panel = nullptr;
static lv_obj_t *g_rxn_label = nullptr;
static lv_obj_t *g_rxn_best_lbl = nullptr;
static int g_rxn_phase = 0;  // 0=idle, 1=waiting, 2=ready, 3=result
static uint32_t g_rxn_trigger_ms = 0;
static uint32_t g_rxn_best = 9999;

static void rxn_set_phase(int phase) {
    g_rxn_phase = phase;
    if (!g_rxn_panel) return;
    if (phase == 0) {
        lv_obj_set_style_bg_color(g_rxn_panel, lv_color_hex(COL_BLUE), 0);
        lv_label_set_text(g_rxn_label, "TAP TO START");
    } else if (phase == 1) {
        lv_obj_set_style_bg_color(g_rxn_panel, lv_color_hex(COL_RED), 0);
        lv_label_set_text(g_rxn_label, "WAIT FOR GREEN...");
        g_rxn_trigger_ms = millis() + 1500 + (esp_random() % 3000);
    } else if (phase == 2) {
        lv_obj_set_style_bg_color(g_rxn_panel, lv_color_hex(COL_GREEN), 0);
        lv_label_set_text(g_rxn_label, "TAP NOW!");
    } else if (phase == 3) {
        lv_obj_set_style_bg_color(g_rxn_panel, lv_color_hex(COL_ORANGE), 0);
    }
}

static void open_reaction() {
    g_rxn_screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(g_rxn_screen, lv_color_hex(COL_BG), 0);

    lv_obj_t *title = lv_label_create(g_rxn_screen);
    lv_label_set_text(title, "REACTION");
    lv_obj_set_style_text_color(title, lv_color_hex(COL_GREEN), 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_22, 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 20);

    g_rxn_panel = lv_btn_create(g_rxn_screen);
    lv_obj_set_size(g_rxn_panel, 340, 280);
    lv_obj_align(g_rxn_panel, LV_ALIGN_CENTER, 0, -10);
    lv_obj_set_style_radius(g_rxn_panel, 20, 0);
    lv_obj_set_style_bg_color(g_rxn_panel, lv_color_hex(COL_BLUE), 0);

    g_rxn_label = lv_label_create(g_rxn_panel);
    lv_label_set_text(g_rxn_label, "TAP TO START");
    lv_obj_set_style_text_color(g_rxn_label, lv_color_hex(COL_WHITE), 0);
    lv_obj_set_style_text_font(g_rxn_label, &lv_font_montserrat_28, 0);
    lv_obj_center(g_rxn_label);

    g_rxn_best_lbl = lv_label_create(g_rxn_screen);
    char buf[32];
    if (g_rxn_best < 9999) snprintf(buf, 32, "Best: %u ms", g_rxn_best);
    else snprintf(buf, 32, "Best: --");
    lv_label_set_text(g_rxn_best_lbl, buf);
    lv_obj_set_style_text_color(g_rxn_best_lbl, lv_color_hex(COL_GRAY), 0);
    lv_obj_set_style_text_font(g_rxn_best_lbl, &lv_font_montserrat_16, 0);
    lv_obj_align(g_rxn_best_lbl, LV_ALIGN_BOTTOM_MID, 0, -70);

    lv_obj_add_event_cb(g_rxn_panel, [](lv_event_t*) {
        if (g_rxn_phase == 0 || g_rxn_phase == 3) {
            rxn_set_phase(1);
        } else if (g_rxn_phase == 1) {
            // Too early!
            lv_obj_set_style_bg_color(g_rxn_panel, lv_color_hex(COL_RED), 0);
            lv_label_set_text(g_rxn_label, "TOO EARLY!");
            g_rxn_phase = 3;
        } else if (g_rxn_phase == 2) {
            uint32_t rt = millis() - g_rxn_trigger_ms;
            char b[32]; snprintf(b, 32, "%u ms", rt);
            lv_label_set_text(g_rxn_label, b);
            if (rt < g_rxn_best) {
                g_rxn_best = rt;
                char b2[32]; snprintf(b2, 32, "Best: %u ms", g_rxn_best);
                if (g_rxn_best_lbl) lv_label_set_text(g_rxn_best_lbl, b2);
            }
            g_rxn_phase = 3;
        }
    }, LV_EVENT_CLICKED, NULL);

    lv_obj_t *back = lv_btn_create(g_rxn_screen);
    lv_obj_set_size(back, 140, 40);
    lv_obj_align(back, LV_ALIGN_BOTTOM_MID, 0, -20);
    lv_obj_set_style_bg_color(back, lv_color_hex(0x141414), 0);
    lv_obj_set_style_border_color(back, lv_color_hex(COL_GREEN), 0);
    lv_obj_set_style_border_width(back, 1, 0);
    lv_obj_t *bl = lv_label_create(back);
    lv_label_set_text(bl, LV_SYMBOL_LEFT " Back");
    lv_obj_center(bl);
    lv_obj_add_event_cb(back, [](lv_event_t*) {
        g_rxn_panel = nullptr; g_rxn_label = nullptr; g_rxn_best_lbl = nullptr;
        g_rxn_phase = 0;
        lv_scr_load(g_launcher_screen);
    }, LV_EVENT_CLICKED, NULL);

    g_rxn_phase = 0;
    lv_scr_load(g_rxn_screen);
}

// =============================================================================
//  FIND MY PHONE — placeholder BLE ping screen
// =============================================================================
static lv_obj_t *g_find_screen = nullptr;
static lv_obj_t *g_find_status = nullptr;
static bool g_find_pinging = false;
static uint32_t g_find_started = 0;

static void open_find_phone() {
    g_find_screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(g_find_screen, lv_color_hex(COL_BG), 0);

    lv_obj_t *title = lv_label_create(g_find_screen);
    lv_label_set_text(title, "FIND MY PHONE");
    lv_obj_set_style_text_color(title, lv_color_hex(COL_BLUE), 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_22, 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 25);

    lv_obj_t *icon_panel = lv_obj_create(g_find_screen);
    lv_obj_set_size(icon_panel, 180, 180);
    lv_obj_align(icon_panel, LV_ALIGN_CENTER, 0, -40);
    lv_obj_set_style_radius(icon_panel, 90, 0);
    lv_obj_set_style_bg_color(icon_panel, lv_color_hex(COL_BLUE), 0);
    lv_obj_set_style_bg_opa(icon_panel, 80, 0);
    lv_obj_set_style_border_color(icon_panel, lv_color_hex(COL_BLUE), 0);
    lv_obj_set_style_border_width(icon_panel, 3, 0);

    lv_obj_t *icn = lv_label_create(icon_panel);
    lv_label_set_text(icn, LV_SYMBOL_CALL);
    lv_obj_set_style_text_color(icn, lv_color_hex(COL_WHITE), 0);
    lv_obj_set_style_text_font(icn, &lv_font_montserrat_48, 0);
    lv_obj_center(icn);

    g_find_status = lv_label_create(g_find_screen);
    lv_label_set_text(g_find_status, "Ready to ping");
    lv_obj_set_style_text_color(g_find_status, lv_color_hex(COL_GRAY), 0);
    lv_obj_set_style_text_font(g_find_status, &lv_font_montserrat_18, 0);
    lv_obj_align(g_find_status, LV_ALIGN_CENTER, 0, 80);

    lv_obj_t *ping = lv_btn_create(g_find_screen);
    lv_obj_set_size(ping, 200, 60);
    lv_obj_align(ping, LV_ALIGN_BOTTOM_MID, 0, -80);
    lv_obj_set_style_radius(ping, 30, 0);
    lv_obj_set_style_bg_color(ping, lv_color_hex(COL_BLUE), 0);
    lv_obj_t *pl = lv_label_create(ping);
    lv_label_set_text(pl, "PING PHONE");
    lv_obj_set_style_text_color(pl, lv_color_hex(COL_WHITE), 0);
    lv_obj_set_style_text_font(pl, &lv_font_montserrat_22, 0);
    lv_obj_center(pl);
    lv_obj_add_event_cb(ping, [](lv_event_t*) {
        g_find_pinging = !g_find_pinging;
        g_find_started = millis();
        if (g_find_status) lv_label_set_text(g_find_status,
            g_find_pinging ? "Pinging..." : "Ping stopped");
    }, LV_EVENT_CLICKED, NULL);

    lv_obj_t *back = lv_btn_create(g_find_screen);
    lv_obj_set_size(back, 140, 40);
    lv_obj_align(back, LV_ALIGN_BOTTOM_MID, 0, -15);
    lv_obj_set_style_bg_color(back, lv_color_hex(0x141414), 0);
    lv_obj_set_style_border_color(back, lv_color_hex(COL_BLUE), 0);
    lv_obj_set_style_border_width(back, 1, 0);
    lv_obj_t *bl = lv_label_create(back);
    lv_label_set_text(bl, LV_SYMBOL_LEFT " Back");
    lv_obj_center(bl);
    lv_obj_add_event_cb(back, [](lv_event_t*) {
        g_find_pinging = false;
        g_find_status = nullptr;
        lv_scr_load(g_launcher_screen);
    }, LV_EVENT_CLICKED, NULL);

    lv_scr_load(g_find_screen);
}

// =============================================================================
//  MAIN SCREEN NAVIGATION
// =============================================================================
static int g_current_screen = 0;  // 0 = face, 1 = launcher

static void goto_screen(int screen) {
    g_current_screen = screen;
    if (screen == 0) lv_scr_load_anim(g_face_screen, LV_SCR_LOAD_ANIM_FADE_ON, 200, 0, false);
    else if (screen == 1) lv_scr_load_anim(g_launcher_screen, LV_SCR_LOAD_ANIM_FADE_ON, 200, 0, false);
}

// =============================================================================
//  SETUP & LOOP
// =============================================================================
void setup() {
    launcher_app_boot();
    qmi_init();
    time_restore_from_nvs();
    prefs_load();
    g_last_activity_ms = millis();
    
    splash_show();
    
    wifi_load_and_sync();
    
    // Build watch face
    g_face_screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(g_face_screen, lv_color_hex(COL_BG), 0);
    lv_obj_set_style_bg_opa(g_face_screen, LV_OPA_COVER, 0);
    lv_obj_clear_flag(g_face_screen, LV_OBJ_FLAG_SCROLLABLE);
    apply_watch_face(g_current_face);
    
    build_launcher();
    
    lv_scr_load(g_face_screen);
    update_watch_face();
    
    setCpuFrequencyMhz(160);
}

void loop() {
    lv_timer_handler();
    
    // Update stopwatch display
    if (g_stopwatch_running && g_stopwatch_time_label) {
        uint32_t total_ms = g_stopwatch_elapsed + (millis() - g_stopwatch_start);
        int mins = (total_ms / 60000) % 60;
        int secs = (total_ms / 1000) % 60;
        int cs = (total_ms / 10) % 100;
        char buf[16];
        snprintf(buf, 16, "%02d:%02d.%02d", mins, secs, cs);
        lv_label_set_text(g_stopwatch_time_label, buf);
    }
    
    // Update timer display
    if (g_timer_running && g_timer_label) {
        uint32_t elapsed = (millis() - g_timer_start_time) / 1000;
        if (elapsed >= g_timer_remaining) {
            g_timer_running = false;
            g_timer_remaining = 0;
            lv_label_set_text(g_timer_label, "00:00");
            lv_arc_set_value(g_timer_arc, 0);
        } else {
            uint32_t left = g_timer_remaining - elapsed;
            char buf[16];
            snprintf(buf, 16, "%02d:%02d", (int)(left/60), (int)(left%60));
            lv_label_set_text(g_timer_label, buf);
            int percent = (left * 100) / g_timer_duration;
            lv_arc_set_value(g_timer_arc, percent);
        }
    }

    // =========================================================================
    //  v2 APP TICKS
    // =========================================================================
    // --- Compass: integrate gyro yaw rate, update needle + label (10 Hz) ---
    if (g_compass_needle && g_has_imu) {
        uint32_t now = millis();
        if (now - g_compass_last_ms > 100) {
            float dt = (now - g_compass_last_ms) / 1000.0f;
            g_compass_last_ms = now;
            float gx, gy, gz;
            qmi_read_gyro(gx, gy, gz);
            g_compass_heading += gz * dt;
            while (g_compass_heading < 0)    g_compass_heading += 360.0f;
            while (g_compass_heading >= 360) g_compass_heading -= 360.0f;
            lv_obj_set_style_transform_angle(g_compass_needle, (int16_t)(g_compass_heading * 10), 0);
            if (g_compass_deg_lbl) {
                char b[8]; snprintf(b, 8, "%d°", (int)g_compass_heading);
                lv_label_set_text(g_compass_deg_lbl, b);
            }
            if (g_compass_card_lbl) lv_label_set_text(g_compass_card_lbl, compass_cardinal(g_compass_heading));
        }
    }

    // --- Breathe: 4-7-8 cycle animation (inhale 4s, hold 7s, exhale 8s) ---
    if (g_breathe_running && g_breathe_circle && g_breathe_label) {
        uint32_t elapsed = (millis() - g_breathe_start) / 1000;
        uint32_t cycle_t = elapsed % 19;  // 4+7+8
        int new_phase;
        const char *txt;
        int size;
        if (cycle_t < 4)       { new_phase = 0; txt = "Inhale";  size = 60 + (int)((cycle_t      ) * 30); }
        else if (cycle_t < 11) { new_phase = 1; txt = "Hold";    size = 180; }
        else                   { new_phase = 2; txt = "Exhale";  size = 180 - (int)((cycle_t - 11) * 15); }
        if (size < 60) size = 60; if (size > 200) size = 200;
        lv_obj_set_size(g_breathe_circle, size, size);
        lv_obj_set_style_radius(g_breathe_circle, size/2, 0);
        lv_obj_align(g_breathe_circle, LV_ALIGN_CENTER, 0, -20);
        if (new_phase != g_breathe_phase) {
            g_breathe_phase = new_phase;
            lv_label_set_text(g_breathe_label, txt);
        }
        int cycle_n = elapsed / 19;
        if (cycle_n != g_breathe_cycle) {
            g_breathe_cycle = cycle_n;
            if (g_breathe_count) {
                char b[24]; snprintf(b, 24, "Cycle %d/5", g_breathe_cycle);
                lv_label_set_text(g_breathe_count, b);
            }
            if (g_breathe_cycle >= 5) g_breathe_running = false;
        }
    }

    // --- Heart Rate: simulated drift + pulse animation ---
    if (g_hr_bpm_lbl && g_hr_heart) {
        uint32_t now = millis();
        // BPM jitter every ~1.2s
        static uint32_t last_jitter = 0;
        if (now - last_jitter > 1200) {
            last_jitter = now;
            int delta = (int)(esp_random() % 5) - 2;
            g_hr_bpm += delta;
            if (g_hr_bpm < 55) g_hr_bpm = 55;
            if (g_hr_bpm > 140) g_hr_bpm = 140;
            char b[16]; snprintf(b, 16, "%d BPM", g_hr_bpm);
            lv_label_set_text(g_hr_bpm_lbl, b);
            if (g_hr_zone_lbl) {
                lv_label_set_text(g_hr_zone_lbl, hr_zone(g_hr_bpm));
                lv_obj_set_style_text_color(g_hr_zone_lbl, lv_color_hex(hr_zone_color(g_hr_bpm)), 0);
            }
        }
        // Heart pulse — toggle opacity at BPM frequency
        uint32_t period_ms = 60000UL / (uint32_t)g_hr_bpm;
        bool state = ((now / (period_ms/2)) & 1);
        if (state != g_hr_pulse_state) {
            g_hr_pulse_state = state;
            lv_obj_set_size(g_hr_heart, state ? 160 : 140, state ? 160 : 140);
            lv_obj_set_style_radius(g_hr_heart, state ? 80 : 70, 0);
            lv_obj_align(g_hr_heart, LV_ALIGN_CENTER, 0, -30);
            lv_obj_set_style_bg_opa(g_hr_heart, state ? 200 : 120, 0);
        }
    }

    // --- Pomodoro: countdown + auto-phase advance ---
    if (g_pomo_running && g_pomo_label && g_pomo_arc) {
        uint32_t e = (millis() - g_pomo_start) / 1000;
        if (e >= g_pomo_remaining) {
            // phase complete
            if (g_pomo_phase == 0) {
                g_pomo_cycle++;
                if (g_pomo_cycle_lbl) {
                    char b[24]; snprintf(b, 24, "Cycle %d", g_pomo_cycle + 1);
                    lv_label_set_text(g_pomo_cycle_lbl, b);
                }
                pomo_phase_setup((g_pomo_cycle % 4 == 0) ? 2 : 1);
            } else {
                pomo_phase_setup(0);
            }
            g_pomo_start = millis();
            // update color
            lv_obj_set_style_arc_color(g_pomo_arc,
                lv_color_hex(g_pomo_phase == 0 ? COL_RED : COL_GREEN), LV_PART_INDICATOR);
        } else {
            uint32_t left = g_pomo_remaining - e;
            char b[16]; snprintf(b, 16, "%02u:%02u", (unsigned)(left/60), (unsigned)(left%60));
            lv_label_set_text(g_pomo_label, b);
            int pct = (int)((left * 100) / g_pomo_duration);
            lv_arc_set_value(g_pomo_arc, pct);
        }
    }

    // --- World Clock: refresh every second ---
    if (g_world_labels[0]) {
        static uint32_t last_wc = 0;
        if (millis() - last_wc > 1000) {
            last_wc = millis();
            time_t now = time(nullptr);
            for (int i = 0; i < (int)TZ_COUNT; i++) {
                if (!g_world_labels[i]) continue;
                time_t adj = now + TZ_CITIES[i].offset_h * 3600;
                struct tm tm;
                gmtime_r(&adj, &tm);
                char b[8]; snprintf(b, 8, "%02d:%02d", tm.tm_hour, tm.tm_min);
                lv_label_set_text(g_world_labels[i], b);
            }
        }
    }

    // --- Snake: game tick at ~5 Hz ---
    if (g_snake_running && g_snake_canvas) {
        if (millis() - g_snake_last_tick > 200) {
            g_snake_last_tick = millis();
            int nx = g_snake_x[0] + g_snake_dx;
            int ny = g_snake_y[0] + g_snake_dy;
            // wall collision -> reset
            if (nx < 0 || nx >= SNAKE_COLS || ny < 0 || ny >= SNAKE_ROWS) { snake_reset(); snake_redraw(); }
            else {
                // self collision -> reset
                bool hit = false;
                for (int i = 0; i < g_snake_len; i++) if (g_snake_x[i] == nx && g_snake_y[i] == ny) { hit = true; break; }
                if (hit) { snake_reset(); snake_redraw(); }
                else {
                    // shift body
                    bool grew = (nx == g_food_x && ny == g_food_y);
                    int new_len = grew ? g_snake_len + 1 : g_snake_len;
                    if (new_len > SNAKE_MAX) new_len = SNAKE_MAX;
                    for (int i = new_len - 1; i > 0; i--) {
                        g_snake_x[i] = g_snake_x[i-1];
                        g_snake_y[i] = g_snake_y[i-1];
                    }
                    g_snake_x[0] = nx; g_snake_y[0] = ny;
                    g_snake_len = new_len;
                    if (grew) {
                        g_snake_score += 10;
                        snake_spawn_food();
                        if (g_snake_score_lbl) {
                            char b[24]; snprintf(b, 24, "Score %u", (unsigned)g_snake_score);
                            lv_label_set_text(g_snake_score_lbl, b);
                        }
                    }
                    snake_redraw();
                }
            }
        }
    }

    // --- Reaction: check for trigger -> GO ---
    if (g_rxn_phase == 1 && g_rxn_panel && millis() >= g_rxn_trigger_ms) {
        rxn_set_phase(2);
    }

    // --- Find My Phone: blink status every 500ms while pinging ---
    if (g_find_pinging && g_find_status) {
        static uint32_t last_blink = 0;
        if (millis() - last_blink > 500) {
            last_blink = millis();
            uint32_t secs = (millis() - g_find_started) / 1000;
            char b[32]; snprintf(b, 32, "Pinging... %us", (unsigned)secs);
            lv_label_set_text(g_find_status, b);
            if (secs > 30) { g_find_pinging = false; lv_label_set_text(g_find_status, "Ping timeout"); }
        }
    }
    
    // Battery stats live refresh (1 Hz while screen is open)
    if (g_batt_arc && g_has_pmu) {
        static uint32_t last_batt_ui = 0;
        if (millis() - last_batt_ui > 1000) {
            last_batt_ui = millis();
            int pct = g_pmu.getBatteryPercent();
            float v = g_pmu.getBattVoltage() / 1000.0f;
            bool charging = g_pmu.isCharging();
            lv_arc_set_value(g_batt_arc, pct < 0 ? 0 : pct);
            lv_obj_set_style_arc_color(g_batt_arc,
                lv_color_hex(pct <= 15 ? COL_RED : (pct <= 40 ? COL_ORANGE : COL_GREEN)),
                LV_PART_INDICATOR);
            char b[16]; snprintf(b, 16, "%d%%", pct < 0 ? 0 : pct);
            lv_label_set_text(g_batt_pct, b);
            char vb[16]; snprintf(vb, 16, "%.2f V", v);
            lv_label_set_text(g_batt_volts, vb);
            lv_label_set_text(g_batt_state, charging ? LV_SYMBOL_CHARGE " Charging" : "On Battery");
            lv_obj_set_style_text_color(g_batt_state,
                lv_color_hex(charging ? COL_GREEN : COL_GRAY), 0);
        }
    }
    
    // Step counter update (10 Hz sampling)
    static uint32_t last_step_update = 0;
    if (millis() - last_step_update > 100) {
        last_step_update = millis();
        step_counter_update();
    }
    
    // Watch face update (1 Hz)
    static uint32_t last_face_update = 0;
    if (millis() - last_face_update > 1000) {
        last_face_update = millis();
        if (g_current_screen == 0) update_watch_face();
    }
    
    // Swipe detection for screen switching
    static int32_t swipe_start_x = 0, swipe_start_y = 0;
    static bool swipe_in_progress = false;
    
    if (g_touch_pressed_now) {
        if (!swipe_in_progress) {
            lv_indev_t *indev = lv_indev_get_act();
            lv_point_t p;
            lv_indev_get_point(indev, &p);
            swipe_start_x = p.x;
            swipe_start_y = p.y;
            swipe_in_progress = true;
        }
    } else {
        if (swipe_in_progress) {
            lv_indev_t *indev = lv_indev_get_act();
            lv_point_t p;
            lv_indev_get_point(indev, &p);
            int32_t dx = p.x - swipe_start_x;
            int32_t dy = p.y - swipe_start_y;
            
            if (abs(dx) > 100 && abs(dx) > abs(dy)) {
                if (dx < 0 && g_current_screen == 0) goto_screen(1);  // Left swipe: face -> launcher
                else if (dx > 0 && g_current_screen == 1) goto_screen(0);  // Right swipe: launcher -> face
            } else if (dy > 100 && abs(dy) > abs(dx) && g_current_screen == 0) {
                // Down swipe on face: top edge → Quick Settings, else → flashlight
                if (swipe_start_y < 80) quick_settings_open();
                else                    open_flashlight();
            } else if (dy < -100 && abs(dy) > abs(dx) && g_current_screen == 0) {
                // UP SWIPE on face: CYCLE through 4 watch faces (Ultra feature)
                apply_watch_face(g_current_face + 1);
                Serial.printf("[ULTRA] Switched to face %d: %s\n", g_current_face, FACE_NAMES[g_current_face]);
            }
            
            swipe_in_progress = false;
        }
    }
    
    // =========================================================================
    //  AOD tick (when display is "sleeping" but AOD is enabled, update 1 Hz)
    // =========================================================================
    if (g_aod_active) {
        static uint32_t last_aod = 0;
        if (millis() - last_aod > 1000) {
            last_aod = millis();
            aod_update();
        }
    }

    // =========================================================================
    //  AXP2101 hardware power-button events (PEKEY) — runs alongside GPIO poll
    // =========================================================================
    {
        static uint32_t last_pmu_poll = 0;
        if (millis() - last_pmu_poll > 50) {
            last_pmu_poll = millis();
            int ev = pmu_pekey_event();
            if (ev == 1) {
                if (g_display_on) display_sleep();
                else              display_wake();
            } else if (ev == 2) {
                enter_deep_sleep();
            }
        }
    }

    // =========================================================================
    //  Shake / wrist-raise to wake (only when display is off)
    // =========================================================================
    if (!g_display_on && g_has_imu) {
        static float last_mag = 1.0f;
        static uint32_t last_motion = 0;
        if (millis() - last_motion > 80) {
            last_motion = millis();
            float ax, ay, az;
            qmi_read_accel(ax, ay, az);
            float mag = sqrtf(ax*ax + ay*ay + az*az);
            if (fabsf(mag - last_mag) > 0.45f) display_wake();
            last_mag = mag;
        }
    }

    // =========================================================================
    //  Battery low warning (poll every 30 s)
    // =========================================================================
    {
        static uint32_t last_batt = 0;
        if (millis() - last_batt > 30000) {
            last_batt = millis();
            battery_check_warning();
        }
    }

    // =========================================================================
    //  POWER BUTTON (GPIO 10): short = display off/on, long-hold (3s) = deep sleep
    // =========================================================================
    static uint32_t pwr_down_t  = 0;
    static bool     pwr_was_low = false;
    static bool     pwr_long_fired = false;
    bool pwr_low = (digitalRead(PWR_BTN_PIN) == LOW);

    if (pwr_low && !pwr_was_low) {
        // Falling edge — debounce 20 ms then start timer
        delay(20);
        if (digitalRead(PWR_BTN_PIN) == LOW) {
            pwr_down_t = millis();
            pwr_was_low = true;
            pwr_long_fired = false;
        }
    } else if (pwr_low && pwr_was_low) {
        // Held — check for long-press deep sleep
        if (!pwr_long_fired && (millis() - pwr_down_t) >= PWR_LONG_MS) {
            pwr_long_fired = true;
            enter_deep_sleep();  // does not return
        }
    } else if (!pwr_low && pwr_was_low) {
        // Released
        uint32_t held = millis() - pwr_down_t;
        pwr_was_low = false;
        if (!pwr_long_fired && held < PWR_SHORT_MAX) {
            // Short press: toggle display
            if (g_display_on) display_sleep();
            else              display_wake();
        }
    }

    // =========================================================================
    //  TOUCH-TO-WAKE & AUTO-SLEEP
    // =========================================================================
    if (g_touch_pressed_now) {
        g_last_activity_ms = millis();
        if (!g_display_on) {
            display_wake();
            // Swallow this touch so it doesn't trigger a swipe immediately
            swipe_in_progress = false;
        }
    }
    if (g_display_on && (millis() - g_last_activity_ms) > g_inactivity_ms) {
        display_sleep();
    }

    // =========================================================================
    //  BOOT BUTTON (GPIO 0) -> back to LauncherOS (hold 1.2 s, mandatory)
    //  Adds a 3-sample debounce so EMI/glitches can't trigger a reboot.
    // =========================================================================
    static uint32_t btn_t        = 0;
    static lv_obj_t *btn_overlay = nullptr;
    static lv_obj_t *btn_bar     = nullptr;
    static uint8_t  btn_low_streak = 0;

    const uint32_t HOLD_MS = 1200;

    // Debounced "is BOOT held LOW?" — require 3 consecutive LOW samples.
    if (digitalRead(BOOT_BTN_PIN) == LOW) {
        if (btn_low_streak < 255) btn_low_streak++;
    } else {
        btn_low_streak = 0;
    }
    bool boot_held = (btn_low_streak >= 3);

    if (boot_held) {
        // Pressing BOOT is itself activity — keep display awake.
        g_last_activity_ms = millis();
        if (!g_display_on) display_wake();

        if (!btn_t) {
            btn_t = millis();
        } else {
            uint32_t held = millis() - btn_t;

            // After 200 ms of contiguous hold, spawn the overlay.
            if (held > 200 && !btn_overlay) {
                btn_overlay = lv_obj_create(lv_scr_act());
                lv_obj_remove_style_all(btn_overlay);
                lv_obj_set_size(btn_overlay, LCD_WIDTH, LCD_HEIGHT);
                lv_obj_align(btn_overlay, LV_ALIGN_CENTER, 0, 0);
                lv_obj_set_style_bg_color(btn_overlay, lv_color_hex(0x000000), 0);
                lv_obj_set_style_bg_opa(btn_overlay, 220, 0);

                lv_obj_t *icon = lv_label_create(btn_overlay);
                lv_label_set_text(icon, LV_SYMBOL_HOME);
                lv_obj_set_style_text_color(icon, lv_color_hex(COL_ORANGE), 0);
                lv_obj_set_style_text_font(icon, &lv_font_montserrat_48, 0);
                lv_obj_align(icon, LV_ALIGN_CENTER, 0, -60);

                lv_obj_t *msg = lv_label_create(btn_overlay);
                lv_label_set_text(msg, "Returning to\nLauncher...");
                lv_obj_set_style_text_color(msg, lv_color_hex(COL_WHITE), 0);
                lv_obj_set_style_text_font(msg, &lv_font_montserrat_22, 0);
                lv_obj_set_style_text_align(msg, LV_TEXT_ALIGN_CENTER, 0);
                lv_obj_align(msg, LV_ALIGN_CENTER, 0, 10);

                btn_bar = lv_bar_create(btn_overlay);
                lv_obj_set_size(btn_bar, 240, 6);
                lv_obj_align(btn_bar, LV_ALIGN_CENTER, 0, 80);
                lv_bar_set_range(btn_bar, 0, 100);
                lv_obj_set_style_bg_color(btn_bar, lv_color_hex(COL_PANEL), LV_PART_MAIN);
                lv_obj_set_style_bg_color(btn_bar, lv_color_hex(COL_ORANGE), LV_PART_INDICATOR);
            }

            // Animate progress while user keeps holding.
            if (btn_bar) {
                int pct = (int)((held * 100) / HOLD_MS);
                if (pct > 100) pct = 100;
                lv_bar_set_value(btn_bar, pct, LV_ANIM_OFF);
            }

            if (held > HOLD_MS) {
                lv_timer_handler();   // make sure overlay flushes once
                delay(120);
                return_to_launcher(); // SD .bin first, then factory partition + esp_restart()
            }
        }
    } else {
        // Released early: tear down overlay if present.
        if (btn_overlay) {
            lv_obj_del(btn_overlay);
            btn_overlay = nullptr;
            btn_bar     = nullptr;
        }
        btn_t = 0;
    }

    delay(5);
}
