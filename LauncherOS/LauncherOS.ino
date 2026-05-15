// ============================================================================
//  LauncherOS.ino   --   DualCore Watch launcher  (LVGL 8.3 / 8.4)
//  ----------------------------------------------------------------------------
//  v5 changes:
//      - tileview removed; three independent screens swapped instantly
//        (no slide animation) -- swipe LEFT = next, swipe RIGHT = previous,
//        looping 0 -> 1 -> 2 -> 0.
//      - Screens: 0 = WATCH FACE, 1 = APPS, 2 = BATTERY.
//      - Manual touch-delta swipe detection inside the indev callback,
//        mirroring the S3_MiniOS_206 pattern.
//      - Battery screen with big percentage arc + "X h Y min" estimate.
//      - Boot splash upgraded: letter-by-letter title slide, scanline
//        sweep, hardware-ID footer.
//      - All previous features kept (favourites, wallpapers, NTP, BOOT
//        return helper, OTA loader, 3-second screen-off, etc.)
// ============================================================================

#include <Arduino.h>
#include <FS.h>
#include <SD_MMC.h>
#include <Wire.h>
#include <WiFi.h>
#include <Preferences.h>
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
#include <esp_image_format.h>
#include <esp_system.h>
#include <esp_sleep.h>
#include <esp_bt.h>
#include <esp32-hal-cpu.h>

#include "HWCDC.h"
HWCDC USBSerial;

// ============================================================================
//  POWER-SAVING TUNABLES   (set POWER_SAVE 0 to disable everything below)
// ============================================================================
#define POWER_SAVE              0

// Drop CPU from 240 MHz to this when sitting on a static screen.
// 80 MHz still runs the AMOLED + LVGL smoothly and roughly halves CPU draw.
#define CPU_FREQ_IDLE_MHZ      80
#define CPU_FREQ_BUSY_MHZ     240

// Default panel brightness (0-255).  Anything below ~70% is barely noticeable
// outdoors but saves real current on an AMOLED.
#define DEFAULT_BRIGHTNESS    180

// Once NTP has synced, WiFi is turned OFF entirely.  We turn it back on
// every WIFI_RESYNC_INTERVAL_MS to refresh the clock then shut it again.
// 6 h is a good balance: ~6 s of WiFi every 6 hours = <0.05 % radio uptime.
#define WIFI_RESYNC_INTERVAL_MS  (6UL * 60UL * 60UL * 1000UL)   // 6 hours

// When the AMOLED is off we just stop rendering and call displayOff().
// CPU stays awake but the loop is mostly idle so draw is modest (~25 mA).
// For a real "shut me down until I touch the BOOT button" mode, use the
// "Sleep Now" button on the Battery card -- it enters esp_deep_sleep with
// the internal RTC retained.  Set this to 0 to disable the auto-flickery
// light sleep entirely (recommended, it was annoying).
#define LIGHT_SLEEP_WHEN_OFF    0
// ============================================================================

// ============================================================================
//  TUNABLES
// ============================================================================
#define USE_CUSTOM_FONT          0
#if USE_CUSTOM_FONT
    extern const lv_font_t oswald_72;
    #define FACE_TIME_FONT  oswald_72
#else
    #define FACE_TIME_FONT  lv_font_montserrat_48
#endif

#define SCREEN_OFF_TIMEOUT_MS   10000     // 10 s of no touch -> screen off
#define WAKE_TOUCH_SUPPRESS_MS    400     // ignore touches for 400ms after wake
#define SWIPE_THRESHOLD_PX         30     // pixels of horizontal travel to count
#define SWIPE_MAX_DURATION_MS     800     // anything longer is ignored
#define TAP_MAX_TRAVEL_PX          15
#define NAV_COOLDOWN_MS           250     // ignore rapid-fire swipes

// Battery runtime estimate
#define BATT_CAPACITY_MAH         400     // tune to your pack
#define BATT_DRAW_ACTIVE_MA       120     // typical with AMOLED + WiFi
#define BATT_DRAW_SCREENOFF_MA     35

// ---- hardcoded WiFi fallback (matches S3_MiniOS_206) ----------------------
static const char *HC_WIFI_SSID = "Optus_9D2E3D";
static const char *HC_WIFI_PASS = "snucktemptGLeQU";

// ============================================================================
//  PIN MAP
// ============================================================================
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

// ============================================================================
//  Display + touch + LVGL
// ============================================================================
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

static uint32_t lastTouchMs = 0;
static bool     screenIsOff = false;
static uint32_t g_suppress_touch_until = 0;   // wake-tap grace period

// ---- swipe queue ----------------------------------------------------------
enum SwipeEvent { SWIPE_NONE, SWIPE_LEFT, SWIPE_RIGHT };
static volatile SwipeEvent g_pending_swipe = SWIPE_NONE;

static int32_t  ts_x = 0, ts_y = 0;
static uint32_t ts_ms = 0;
static int32_t  tc_x = 0, tc_y = 0;
static bool     touchActive = false;

static void my_disp_flush(lv_disp_drv_t *drv, const lv_area_t *area,
                          lv_color_t *color_p) {
    uint32_t w = lv_area_get_width(area);
    uint32_t h = lv_area_get_height(area);
    gfx->draw16bitRGBBitmap(area->x1, area->y1, (uint16_t *)&color_p->full, w, h);
    lv_disp_flush_ready(drv);
}

static void my_touchpad_read(lv_indev_drv_t *drv, lv_indev_data_t *data) {
    if (millis() < g_suppress_touch_until) {
        data->state = LV_INDEV_STATE_REL;
        FT3168->IIC_Interrupt_Flag = false;
        return;
    }
    
    int fingers = FT3168->IIC_Read_Device_Value(FT3168->Arduino_IIC_Touch::Value_Information::TOUCH_FINGER_NUMBER);
    int32_t x = FT3168->IIC_Read_Device_Value(FT3168->Arduino_IIC_Touch::Value_Information::TOUCH_COORDINATE_X);
    int32_t y = FT3168->IIC_Read_Device_Value(FT3168->Arduino_IIC_Touch::Value_Information::TOUCH_COORDINATE_Y);

    if (fingers > 0) {
        USBSerial.printf("[TOUCH] Fingers: %d at (%d, %d)\n", fingers, x, y);
    }

    bool pressed = (fingers > 0);

    if (pressed) {
        FT3168->IIC_Interrupt_Flag = false;
        data->state   = LV_INDEV_STATE_PR;
        data->point.x = x;
        data->point.y = y;
        lastTouchMs = millis();
        if (!touchActive) {
            touchActive = true;
            ts_x = x; ts_y = y; ts_ms = millis();
        }
        tc_x = x; tc_y = y;
    } else {
        data->state = LV_INDEV_STATE_REL;
        if (touchActive) {
            touchActive = false;
            uint32_t dur = millis() - ts_ms;
            int32_t dx = tc_x - ts_x;
            int32_t dy = tc_y - ts_y;
            if (dur < SWIPE_MAX_DURATION_MS) {
                if (abs(dx) > SWIPE_THRESHOLD_PX && abs(dx) > abs(dy)) {
                    g_pending_swipe = (dx < 0) ? SWIPE_LEFT : SWIPE_RIGHT;
                }
            }
        }
    }
}

static void rounder_event_cb(lv_disp_drv_t *drv, lv_area_t *area) {
    area->x1 = (area->x1 >> 1) << 1;
    area->y1 = (area->y1 >> 1) << 1;
    area->x2 = ((area->x2 >> 1) << 1) + 1;
    area->y2 = ((area->y2 >> 1) << 1) + 1;
}

static void lvgl_pump() { lv_timer_handler(); }

// ============================================================================
//  AXP2101 PMU
// ============================================================================
XPowersPMU g_pmu;
static bool g_has_pmu = false;
static int  g_batt_pct = -1;
static bool g_charging = false;
static int  g_batt_mv  = 0;

static void pmu_init() {
    if (g_pmu.begin(Wire, AXP2101_SLAVE_ADDRESS, IIC_SDA, IIC_SCL)) {
        g_has_pmu = true;
        g_pmu.disableTSPinMeasure();
        g_pmu.enableBattVoltageMeasure();
    }
}
static void pmu_refresh() {
    if (!g_has_pmu) return;
    g_batt_pct = g_pmu.getBatteryPercent();
    g_charging = g_pmu.isCharging();
    g_batt_mv  = g_pmu.getBattVoltage();
}

// Returns minutes remaining, or -1 when charging / unknown.
static int batt_minutes_left() {
    if (!g_has_pmu || g_batt_pct < 0 || g_charging) return -1;
    float draw = screenIsOff ? BATT_DRAW_SCREENOFF_MA : BATT_DRAW_ACTIVE_MA;
    float mah_left = BATT_CAPACITY_MAH * (g_batt_pct / 100.0f);
    return (int)((mah_left / draw) * 60.0f);
}

// ============================================================================
//  WiFi / NTP
// ============================================================================
struct WifiNet { String ssid, pass; };
static WifiNet g_nets[5];
static int     g_net_count = 0;
static int     g_gmt_offset_sec = 0;
static bool    g_wifi_connected = false;
static bool    g_ntp_synced     = false;
static uint32_t g_last_sync_ms  = 0;

static void wifi_shutdown() {
    // Disconnect, drop the radio, stop the WiFi driver.
    // Saves ~70-100 mA continuous when we don't need it.
    WiFi.disconnect(true, true);
    WiFi.mode(WIFI_OFF);
    g_wifi_connected = false;
}

static void parse_wifi_config() {
    File f = SD_MMC.open("/WATCH/wifi/config.txt", FILE_READ);
    if (!f) return;
    while (f.available()) {
        String line = f.readStringUntil('\n');
        line.trim();
        if (!line.length() || line.startsWith("#")) continue;
        int eq = line.indexOf('=');
        if (eq < 0) continue;
        String key = line.substring(0, eq); key.trim();
        String val = line.substring(eq + 1); val.trim();
        for (int i = 1; i <= 5; i++) {
            if (key == String("WIFI") + i + "_SSID" && val.length() &&
                val != "YourHomeNetwork" && val != "YourWorkNetwork") {
                if (g_net_count < 5) g_nets[g_net_count].ssid = val;
            }
            if (key == String("WIFI") + i + "_PASS" && val.length()) {
                int idx = g_net_count;
                if (idx < 5 && g_nets[idx].ssid.length() &&
                    !g_nets[idx].pass.length()) {
                    g_nets[idx].pass = val;
                    g_net_count++;
                }
            }
        }
        if (key == "GMT_OFFSET") g_gmt_offset_sec = val.toInt() * 3600;
    }
    f.close();
}

static void wifi_connect_and_sync() {
    WiFi.mode(WIFI_STA);
    for (int i = 0; i < g_net_count && !g_wifi_connected; i++) {
        WiFi.begin(g_nets[i].ssid.c_str(), g_nets[i].pass.c_str());
        uint32_t u = millis() + 6000;
        while (millis() < u && WiFi.status() != WL_CONNECTED) delay(100);
        g_wifi_connected = (WiFi.status() == WL_CONNECTED);
    }
    if (!g_wifi_connected) {
        WiFi.begin(HC_WIFI_SSID, HC_WIFI_PASS);
        uint32_t u = millis() + 6000;
        while (millis() < u && WiFi.status() != WL_CONNECTED) delay(100);
        g_wifi_connected = (WiFi.status() == WL_CONNECTED);
    }
    if (g_wifi_connected) {
        configTime(g_gmt_offset_sec, 0, "pool.ntp.org");
        struct tm tm;
        for (int i = 0; i < 30 && !g_ntp_synced; i++)
            if (getLocalTime(&tm, 200)) g_ntp_synced = true;
    }

#if POWER_SAVE
    // Once the clock is set (or even if it failed), drop WiFi entirely.
    // It'll come back periodically via wifi_resync_if_due().
    g_last_sync_ms = millis();
    wifi_shutdown();
#endif
}

#if POWER_SAVE
// Re-wake WiFi every few hours, top up the RTC, drop it again.  Runs from
// loop() so it never blocks user input for more than a fraction of a second
// at a time.  Returns immediately if the interval hasn't elapsed.
static void wifi_resync_if_due() {
    if (millis() - g_last_sync_ms < WIFI_RESYNC_INTERVAL_MS) return;
    USBSerial.println("[ntp] periodic resync...");
    g_wifi_connected = false;
    wifi_connect_and_sync();          // re-connects, syncs, and shuts down
    g_last_sync_ms = millis();        // even on failure, back-off the next try
}
#endif

// ============================================================================
//  Favourites (NVS-backed)
// ============================================================================
Preferences g_prefs;
#define MAX_FAVS 16
static String g_favs[MAX_FAVS];
static int    g_fav_count = 0;

static void favs_load() {
    g_fav_count = 0;
    g_prefs.begin("dcw", true);
    String csv = g_prefs.getString("favs", "");
    g_prefs.end();
    int from = 0;
    while (from < (int)csv.length() && g_fav_count < MAX_FAVS) {
        int c = csv.indexOf(',', from);
        if (c < 0) c = csv.length();
        String it = csv.substring(from, c); it.trim();
        if (it.length()) g_favs[g_fav_count++] = it;
        from = c + 1;
    }
}
static void favs_save() {
    String csv;
    for (int i = 0; i < g_fav_count; i++) { if (i) csv += ","; csv += g_favs[i]; }
    g_prefs.begin("dcw", false); g_prefs.putString("favs", csv); g_prefs.end();
}
static bool favs_contains(const String &n){for(int i=0;i<g_fav_count;i++)if(g_favs[i]==n)return true;return false;}
static void favs_toggle(const String &n) {
    for (int i = 0; i < g_fav_count; i++) if (g_favs[i] == n) {
        for (int j=i;j<g_fav_count-1;j++) g_favs[j]=g_favs[j+1];
        g_fav_count--; favs_save(); return;
    }
    if (g_fav_count < MAX_FAVS) { g_favs[g_fav_count++] = n; favs_save(); }
}

// ============================================================================
//  ============     EVEN-MORE-FIRE  "DualCore Watch"  splash       ============
//  - 3 concentric pulsing/rotating arcs (kept from v4)
//  - Letter-by-letter title slide-in
//  - Bright scanline that sweeps top to bottom while booting
//  - Hardware-ID line "ESP32-S3 · 16MB · 8MB PSRAM" fades in at the end
// ============================================================================
static lv_obj_t *splash_screen = nullptr;
static lv_obj_t *splash_status = nullptr;
static lv_obj_t *splash_bar    = nullptr;
static lv_obj_t *splash_scanline = nullptr;

static void anim_arc(void *v, int32_t x){lv_arc_set_value((lv_obj_t*)v,x);}
static void anim_rot(void *v, int32_t x){lv_arc_set_rotation((lv_obj_t*)v,(uint16_t)(x%360));}
static void anim_opa(void *v, int32_t x){lv_obj_set_style_text_opa((lv_obj_t*)v,x,0);}
static void anim_y  (void *v, int32_t x){lv_obj_set_y((lv_obj_t*)v, x);}
static void anim_bg_opa(void *v, int32_t x){lv_obj_set_style_bg_opa((lv_obj_t*)v, x, 0);}

static lv_obj_t *make_arc(lv_obj_t *par, int size, lv_color_t col, int w) {
    lv_obj_t *a = lv_arc_create(par);
    lv_obj_set_size(a, size, size);
    lv_obj_align(a, LV_ALIGN_CENTER, 0, -40);
    lv_arc_set_bg_angles(a, 0, 360);
    lv_obj_remove_style(a, NULL, LV_PART_KNOB);
    lv_obj_set_style_arc_color(a, lv_color_hex(0x101010), LV_PART_MAIN);
    lv_obj_set_style_arc_width(a, w, LV_PART_MAIN);
    lv_obj_set_style_arc_color(a, col, LV_PART_INDICATOR);
    lv_obj_set_style_arc_width(a, w, LV_PART_INDICATOR);
    lv_arc_set_value(a, 0);
    return a;
}
static void pulse_arc(lv_obj_t *a, uint32_t period, int dir) {
    lv_anim_t an; lv_anim_init(&an); lv_anim_set_var(&an, a);
    lv_anim_set_exec_cb(&an, anim_arc);
    lv_anim_set_values(&an, 0, 100);
    lv_anim_set_time(&an, period);
    lv_anim_set_playback_time(&an, period);
    lv_anim_set_repeat_count(&an, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_path_cb(&an, lv_anim_path_ease_in_out);
    lv_anim_start(&an);

    lv_anim_t r; lv_anim_init(&r); lv_anim_set_var(&r, a);
    lv_anim_set_exec_cb(&r, anim_rot);
    lv_anim_set_values(&r, 0, 360 * dir);
    lv_anim_set_time(&r, period * 4);
    lv_anim_set_repeat_count(&r, LV_ANIM_REPEAT_INFINITE);
    lv_anim_start(&r);
}

static void build_splash() {
    splash_screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(splash_screen, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(splash_screen, LV_OPA_COVER, 0);
    lv_scr_load(splash_screen);

    pulse_arc(make_arc(splash_screen, 280, lv_color_hex(0x00E5FF), 6), 1400,  1);
    pulse_arc(make_arc(splash_screen, 220, lv_color_hex(0xFF2BD6), 5), 1100, -1);
    pulse_arc(make_arc(splash_screen, 160, lv_color_hex(0xFFB300), 4),  850,  1);

    // -- letter-by-letter title slide-in -----------------------------------
    const char *letters[] = { "D","u","a","l","C","o","r","e" };
    const int   N = 8;
    int totalW   = N * 22;                       // ~22 px per glyph at 28pt
    int startX   = (LCD_WIDTH - totalW) / 2;
    for (int i = 0; i < N; i++) {
        lv_obj_t *L = lv_label_create(splash_screen);
        lv_label_set_text(L, letters[i]);
        lv_obj_set_style_text_color(L, lv_color_hex(0xFFFFFF), 0);
        lv_obj_set_style_text_font(L, &lv_font_montserrat_28, 0);

        lv_obj_set_pos(L, startX + i * 22, -40);  // start above-screen
        lv_obj_set_style_text_opa(L, 0, 0);

        lv_anim_t a; lv_anim_init(&a); lv_anim_set_var(&a, L);
        lv_anim_set_exec_cb(&a, anim_y);
        lv_anim_set_values(&a, -40, LCD_HEIGHT / 2 + 60);
        lv_anim_set_time(&a, 600);
        lv_anim_set_delay(&a, 80 + i * 70);
        lv_anim_set_path_cb(&a, lv_anim_path_overshoot);
        lv_anim_start(&a);

        lv_anim_t b; lv_anim_init(&b); lv_anim_set_var(&b, L);
        lv_anim_set_exec_cb(&b, anim_opa);
        lv_anim_set_values(&b, 0, 255);
        lv_anim_set_time(&b, 500);
        lv_anim_set_delay(&b, 80 + i * 70);
        lv_anim_start(&b);
    }

    // WATCH subtitle (fades in after the letters land)
    lv_obj_t *sub = lv_label_create(splash_screen);
    lv_label_set_text(sub, "W A T C H");
    lv_obj_set_style_text_color(sub, lv_color_hex(0x00E5FF), 0);
    lv_obj_set_style_text_font(sub, &lv_font_montserrat_18, 0);
    lv_obj_set_style_text_letter_space(sub, 6, 0);
    lv_obj_align(sub, LV_ALIGN_CENTER, 0, 100);
    lv_obj_set_style_text_opa(sub, 0, 0);
    { lv_anim_t a; lv_anim_init(&a); lv_anim_set_var(&a, sub);
      lv_anim_set_exec_cb(&a, anim_opa); lv_anim_set_values(&a, 0, 230);
      lv_anim_set_time(&a, 700); lv_anim_set_delay(&a, 800); lv_anim_start(&a); }

    // -- scanline sweeping top-to-bottom infinitely ------------------------
    splash_scanline = lv_obj_create(splash_screen);
    lv_obj_set_size(splash_scanline, LCD_WIDTH, 2);
    lv_obj_align(splash_scanline, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_set_style_radius(splash_scanline, 0, 0);
    lv_obj_set_style_border_width(splash_scanline, 0, 0);
    lv_obj_set_style_bg_color(splash_scanline, lv_color_hex(0x00E5FF), 0);
    lv_obj_set_style_bg_opa(splash_scanline, 90, 0);
    { lv_anim_t a; lv_anim_init(&a); lv_anim_set_var(&a, splash_scanline);
      lv_anim_set_exec_cb(&a, anim_y);
      lv_anim_set_values(&a, 0, LCD_HEIGHT);
      lv_anim_set_time(&a, 1600);
      lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
      lv_anim_start(&a); }

    // -- progress bar + status text ---------------------------------------
    splash_bar = lv_bar_create(splash_screen);
    lv_obj_set_size(splash_bar, 240, 4);
    lv_obj_align(splash_bar, LV_ALIGN_BOTTOM_MID, 0, -70);
    lv_obj_set_style_bg_color(splash_bar, lv_color_hex(0x111111), LV_PART_MAIN);
    lv_obj_set_style_bg_color(splash_bar, lv_color_hex(0x00E5FF), LV_PART_INDICATOR);
    lv_bar_set_range(splash_bar, 0, 100);

    splash_status = lv_label_create(splash_screen);
    lv_label_set_text(splash_status, "booting...");
    lv_obj_set_style_text_color(splash_status, lv_color_hex(0x7BEF7B), 0);
    lv_obj_set_style_text_font(splash_status, &lv_font_montserrat_14, 0);
    lv_obj_align(splash_status, LV_ALIGN_BOTTOM_MID, 0, -40);

    // -- hardware-ID footer (fades in slowly at the very end) -------------
    lv_obj_t *hwid = lv_label_create(splash_screen);
    lv_label_set_text(hwid, "ESP32-S3  ·  16 MB  ·  8 MB PSRAM");
    lv_obj_set_style_text_color(hwid, lv_color_hex(0x666666), 0);
    lv_obj_set_style_text_font(hwid, &lv_font_montserrat_12, 0);
    lv_obj_align(hwid, LV_ALIGN_BOTTOM_MID, 0, -16);
    lv_obj_set_style_text_opa(hwid, 0, 0);
    { lv_anim_t a; lv_anim_init(&a); lv_anim_set_var(&a, hwid);
      lv_anim_set_exec_cb(&a, anim_opa); lv_anim_set_values(&a, 0, 200);
      lv_anim_set_time(&a, 900); lv_anim_set_delay(&a, 1500); lv_anim_start(&a); }
}
static void splash_progress(int pct, const char *msg) {
    if (!splash_bar) return;
    lv_bar_set_value(splash_bar, pct, LV_ANIM_ON);
    lv_label_set_text(splash_status, msg);
    uint32_t until = millis() + 250;
    while (millis() < until) { lvgl_pump(); delay(5); }
}

// ============================================================================
//  Wallpaper loader  -- /WATCH/PHOTOS/*.raw or *.bin (RGB565 LE, 410x502)
// ============================================================================
#define MAX_PHOTOS 24
static String g_photos[MAX_PHOTOS];
static int    g_photo_count = 0;
static int    g_photo_idx   = 0;
static lv_img_dsc_t g_face_img_dsc = { };
static uint8_t     *g_face_pixels  = nullptr;
static size_t       g_face_alloc   = 0;

static void scan_photos() {
    g_photo_count = 0;
    File dir = SD_MMC.open("/WATCH/PHOTOS");
    if (!dir || !dir.isDirectory()) return;
    File f = dir.openNextFile();
    while (f && g_photo_count < MAX_PHOTOS) {
        if (!f.isDirectory()) {
            String n = f.name();
            int s = n.lastIndexOf('/'); if (s >= 0) n = n.substring(s+1);
            String lo = n; lo.toLowerCase();
            if (lo.endsWith(".raw") || lo.endsWith(".bin"))
                g_photos[g_photo_count++] = String("/WATCH/PHOTOS/") + n;
        }
        f = dir.openNextFile();
    }
}
static bool load_photo(int idx) {
    if (!g_photo_count) return false;
    if (idx < 0) idx = g_photo_count - 1;
    if (idx >= g_photo_count) idx = 0;
    File f = SD_MMC.open(g_photos[idx].c_str(), FILE_READ);
    if (!f) return false;
    size_t need = (size_t)LCD_WIDTH * LCD_HEIGHT * 2;
    if (!g_face_pixels || g_face_alloc < need) {
        if (g_face_pixels) heap_caps_free(g_face_pixels);
        g_face_pixels = (uint8_t *)heap_caps_malloc(need, MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
        if (!g_face_pixels) g_face_pixels = (uint8_t *)heap_caps_malloc(need, MALLOC_CAP_8BIT);
        if (!g_face_pixels) { f.close(); return false; }
        g_face_alloc = need;
    }
    memset(g_face_pixels, 0, need);
    size_t fsize = f.size();
    f.read(g_face_pixels, fsize < need ? fsize : need);
    f.close();
    g_face_img_dsc.header.cf = LV_IMG_CF_TRUE_COLOR;
    g_face_img_dsc.header.always_zero = 0;
    g_face_img_dsc.header.w = LCD_WIDTH;
    g_face_img_dsc.header.h = LCD_HEIGHT;
    g_face_img_dsc.data_size = need;
    g_face_img_dsc.data = g_face_pixels;
    g_photo_idx = idx;
    return true;
}

// ============================================================================
//  Three independent screens swapped instantly (no animation)
// ============================================================================
#define SCREEN_FACE     0
#define SCREEN_APPS     1
#define SCREEN_BATTERY  2
#define NUM_SCREENS     3

static lv_obj_t *screens[NUM_SCREENS] = { nullptr };
static int        current_screen = SCREEN_FACE;
static uint32_t   last_nav_ms = 0;

static void goto_screen(int s) {
    if (s == current_screen) return;
    if (millis() - last_nav_ms < NAV_COOLDOWN_MS) return;
    if (s < 0) s = NUM_SCREENS - 1;
    if (s >= NUM_SCREENS) s = 0;
    current_screen = s;
    last_nav_ms = millis();
    lv_scr_load_anim(screens[s], LV_SCR_LOAD_ANIM_NONE, 0, 0, false);
}

// ---- watch-face widgets ---------------------------------------------------
static lv_obj_t *face_bg_img=nullptr, *face_bg_fill=nullptr;
static lv_obj_t *face_time=nullptr, *face_time_shadow=nullptr, *face_date=nullptr;
static lv_obj_t *face_batt_icon=nullptr, *face_batt_fill=nullptr;
static lv_obj_t *face_batt_bolt=nullptr, *face_batt_lbl=nullptr;
static lv_obj_t *face_status=nullptr;

static void refresh_wallpaper() {
    if (!g_photo_count) {
        if (face_bg_img)  lv_obj_add_flag(face_bg_img, LV_OBJ_FLAG_HIDDEN);
        if (face_bg_fill) lv_obj_clear_flag(face_bg_fill, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    if (!load_photo(g_photo_idx)) return;
    if (face_bg_fill) lv_obj_add_flag(face_bg_fill, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(face_bg_img, LV_OBJ_FLAG_HIDDEN);
    lv_img_set_src(face_bg_img, &g_face_img_dsc);
    lv_obj_invalidate(face_bg_img);
}

static void face_tap_cb(lv_event_t *e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    if (!g_photo_count) return;
    g_photo_idx = (g_photo_idx + 1) % g_photo_count;
    refresh_wallpaper();
}

static void update_face_time() {
    if (!face_time) return;
    char hm[8] = "--:--", dt[32] = "no wifi";
    struct tm tm;
    if (g_ntp_synced && getLocalTime(&tm, 5)) {
        strftime(hm, sizeof(hm), "%H:%M", &tm);
        strftime(dt, sizeof(dt), "%a  %d %b", &tm);
    } else if (g_wifi_connected) {
        strcpy(dt, "syncing...");
    }
    lv_label_set_text(face_time, hm);
    lv_label_set_text(face_time_shadow, hm);
    lv_label_set_text(face_date, dt);
    lv_color_t dot = g_ntp_synced     ? lv_color_hex(0x00FF80)
                   : g_wifi_connected ? lv_color_hex(0xFFB300)
                                      : lv_color_hex(0xFF4040);
    lv_obj_set_style_bg_color(face_status, dot, 0);
}

static void update_face_battery() {
    if (!face_batt_icon) return;
    if (!g_has_pmu || g_batt_pct < 0) {
        lv_obj_add_flag(face_batt_icon, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(face_batt_fill, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(face_batt_bolt, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(face_batt_lbl, "");
        return;
    }
    lv_obj_clear_flag(face_batt_icon, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(face_batt_fill, LV_OBJ_FLAG_HIDDEN);
    int pct = g_batt_pct; if (pct<0)pct=0; if (pct>100)pct=100;
    int fw = (28*pct)/100; if (fw<1 && pct>0) fw=1;
    lv_obj_set_width(face_batt_fill, fw);
    lv_color_t col = pct<=15 ? lv_color_hex(0xFF4040)
                   : pct<=30 ? lv_color_hex(0xFFB300)
                             : lv_color_hex(0xFFFFFF);
    lv_obj_set_style_bg_color(face_batt_fill, col, 0);
    if (g_charging) lv_obj_clear_flag(face_batt_bolt, LV_OBJ_FLAG_HIDDEN);
    else            lv_obj_add_flag(face_batt_bolt, LV_OBJ_FLAG_HIDDEN);
    char b[8]; snprintf(b, 8, "%d%%", pct); lv_label_set_text(face_batt_lbl, b);
}

static void build_screen_face() {
    lv_obj_t *s = lv_obj_create(NULL);
    screens[SCREEN_FACE] = s;
    lv_obj_set_style_bg_color(s, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(s, LV_OPA_COVER, 0);
    lv_obj_clear_flag(s, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_pad_all(s, 0, 0);

    // Fallback gradient
    face_bg_fill = lv_obj_create(s);
    lv_obj_set_size(face_bg_fill, LCD_WIDTH, LCD_HEIGHT);
    lv_obj_align(face_bg_fill, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_set_style_radius(face_bg_fill, 0, 0);
    lv_obj_set_style_border_width(face_bg_fill, 0, 0);
    lv_obj_set_style_bg_color(face_bg_fill,      lv_color_hex(0x05060A), 0);
    lv_obj_set_style_bg_grad_color(face_bg_fill, lv_color_hex(0x0A1A2A), 0);
    lv_obj_set_style_bg_grad_dir(face_bg_fill,   LV_GRAD_DIR_VER, 0);
    lv_obj_clear_flag(face_bg_fill, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(face_bg_fill, LV_OBJ_FLAG_CLICKABLE);

    // Photo wallpaper
    face_bg_img = lv_img_create(s);
    lv_obj_align(face_bg_img, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_add_flag(face_bg_img, LV_OBJ_FLAG_HIDDEN);

    // Tap zone (lower 3/4 -- cycles wallpaper)
    lv_obj_t *tap = lv_obj_create(s);
    lv_obj_set_size(tap, LCD_WIDTH, LCD_HEIGHT - 70);
    lv_obj_align(tap, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_opa(tap, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(tap, 0, 0);
    lv_obj_add_flag(tap, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(tap, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(tap, face_tap_cb, LV_EVENT_CLICKED, NULL);

    // Translucent time
    face_time = lv_label_create(s);
    lv_label_set_text(face_time, "18:15");
    lv_obj_set_style_text_color(face_time, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(face_time, &FACE_TIME_FONT, 0);
    lv_obj_set_style_text_opa(face_time, 220, 0);
    lv_obj_set_style_text_letter_space(face_time, 2, 0);
    lv_obj_align(face_time, LV_ALIGN_TOP_MID, 0, 60);

    // Shadow behind time (for legibility on bright photos)
    face_time_shadow = lv_label_create(s);
    lv_label_set_text(face_time_shadow, "18:15");
    lv_obj_set_style_text_color(face_time_shadow, lv_color_hex(0x000000), 0);
    lv_obj_set_style_text_font(face_time_shadow, &FACE_TIME_FONT, 0);
    lv_obj_set_style_text_opa(face_time_shadow, 90, 0);
    lv_obj_set_style_text_letter_space(face_time_shadow, 2, 0);
    lv_obj_align(face_time_shadow, LV_ALIGN_TOP_MID, 2, 62);
    lv_obj_move_background(face_time_shadow);

    face_date = lv_label_create(s);
    lv_label_set_text(face_date, "");
    lv_obj_set_style_text_color(face_date, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(face_date, &lv_font_montserrat_18, 0);
    lv_obj_set_style_text_opa(face_date, 160, 0);
    lv_obj_align(face_date, LV_ALIGN_TOP_MID, 0, 145);

    // Battery indicator (top-left)
    face_batt_icon = lv_obj_create(s);
    lv_obj_set_size(face_batt_icon, 32, 14);
    lv_obj_align(face_batt_icon, LV_ALIGN_TOP_LEFT, 18, 18);
    lv_obj_set_style_radius(face_batt_icon, 3, 0);
    lv_obj_set_style_border_color(face_batt_icon, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_border_width(face_batt_icon, 1, 0);
    lv_obj_set_style_border_opa(face_batt_icon, 180, 0);
    lv_obj_set_style_bg_opa(face_batt_icon, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(face_batt_icon, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(face_batt_icon, LV_OBJ_FLAG_CLICKABLE);
    face_batt_fill = lv_obj_create(face_batt_icon);
    lv_obj_set_size(face_batt_fill, 0, 10);
    lv_obj_align(face_batt_fill, LV_ALIGN_LEFT_MID, 1, 0);
    lv_obj_set_style_radius(face_batt_fill, 2, 0);
    lv_obj_set_style_border_width(face_batt_fill, 0, 0);
    lv_obj_set_style_bg_color(face_batt_fill, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_bg_opa(face_batt_fill, 200, 0);
    face_batt_bolt = lv_label_create(s);
    lv_label_set_text(face_batt_bolt, LV_SYMBOL_CHARGE);
    lv_obj_set_style_text_color(face_batt_bolt, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(face_batt_bolt, &lv_font_montserrat_14, 0);
    lv_obj_align_to(face_batt_bolt, face_batt_icon, LV_ALIGN_CENTER, 0, 0);
    lv_obj_add_flag(face_batt_bolt, LV_OBJ_FLAG_HIDDEN);
    face_batt_lbl = lv_label_create(s);
    lv_label_set_text(face_batt_lbl, "");
    lv_obj_set_style_text_color(face_batt_lbl, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(face_batt_lbl, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_opa(face_batt_lbl, 180, 0);
    lv_obj_align_to(face_batt_lbl, face_batt_icon, LV_ALIGN_OUT_RIGHT_MID, 8, 0);

    face_status = lv_obj_create(s);
    lv_obj_set_size(face_status, 10, 10);
    lv_obj_align(face_status, LV_ALIGN_TOP_RIGHT, -20, 22);
    lv_obj_set_style_radius(face_status, 5, 0);
    lv_obj_set_style_border_width(face_status, 0, 0);
    lv_obj_set_style_bg_color(face_status, lv_color_hex(0xFF4040), 0);
    lv_obj_set_style_bg_opa(face_status, 200, 0);

    refresh_wallpaper();
    update_face_time();
    update_face_battery();
}

// ============================================================================
//  App selector screen
// ============================================================================
struct AppEntry { String name, path; size_t size; };
#define MAX_APPS 32
static AppEntry  g_apps[MAX_APPS];
static int       g_app_count = 0;
static lv_obj_t *g_list_widget = nullptr;
static bool      load_app_from_sd(const AppEntry &app);
static void      populate_app_list();

// OTA progress overlay (loaded as its own screen during the copy)
static lv_obj_t *copy_screen=nullptr,*copy_bar=nullptr,*copy_title=nullptr,*copy_pct=nullptr;

static void build_copy_screen(const String &n) {
    copy_screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(copy_screen, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(copy_screen, LV_OPA_COVER, 0);
    copy_title = lv_label_create(copy_screen);
    lv_label_set_text_fmt(copy_title, "Loading\n%s", n.c_str());
    lv_obj_set_style_text_align(copy_title, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(copy_title, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(copy_title, &lv_font_montserrat_22, 0);
    lv_obj_align(copy_title, LV_ALIGN_CENTER, 0, -60);
    copy_bar = lv_bar_create(copy_screen);
    lv_obj_set_size(copy_bar, 280, 10);
    lv_obj_align(copy_bar, LV_ALIGN_CENTER, 0, 20);
    lv_obj_set_style_bg_color(copy_bar, lv_color_hex(0x141414), LV_PART_MAIN);
    lv_obj_set_style_bg_color(copy_bar, lv_color_hex(0x00E5FF), LV_PART_INDICATOR);
    lv_bar_set_range(copy_bar, 0, 100);
    copy_pct = lv_label_create(copy_screen);
    lv_label_set_text(copy_pct, "0%");
    lv_obj_set_style_text_color(copy_pct, lv_color_hex(0x00E5FF), 0);
    lv_obj_set_style_text_font(copy_pct, &lv_font_montserrat_18, 0);
    lv_obj_align(copy_pct, LV_ALIGN_CENTER, 0, 60);
    lv_scr_load(copy_screen);
    lvgl_pump();
}
static void copy_progress(int p){lv_bar_set_value(copy_bar,p,LV_ANIM_OFF);char b[8];snprintf(b,8,"%d%%",p);lv_label_set_text(copy_pct,b);lvgl_pump();}
static void copy_error(const String&m){lv_label_set_text_fmt(copy_title,"ERROR\n%s",m.c_str());lv_obj_set_style_text_color(copy_title,lv_color_hex(0xFF4040),0);lvgl_pump();}

static bool load_app_from_sd(const AppEntry &app) {
    build_copy_screen(app.name);
#if POWER_SAVE
    setCpuFrequencyMhz(CPU_FREQ_BUSY_MHZ);   // OTA flash writes need speed
#endif
    const esp_partition_t *ota = esp_ota_get_next_update_partition(NULL);
    if (!ota) { copy_error("no ota slot"); return false; }
    File f = SD_MMC.open(app.path.c_str(), FILE_READ);
    if (!f) { copy_error("open failed"); return false; }
    esp_ota_handle_t h;
    if (esp_ota_begin(ota, app.size, &h) != ESP_OK) { copy_error("ota_begin"); f.close(); return false; }
    uint8_t *buf = (uint8_t *)heap_caps_malloc(4096, MALLOC_CAP_8BIT);
    size_t written = 0; int last = -1;
    while (true) {
        int n = f.read(buf, 4096);
        if (n <= 0) break;
        if (esp_ota_write(h, buf, n) != ESP_OK) { free(buf); f.close(); copy_error("ota_write"); return false; }
        written += n;
        int pct = app.size ? (int)((written * 100) / app.size) : 0;
        if (pct != last) { copy_progress(pct); last = pct; }
    }
    free(buf); f.close();
    if (esp_ota_end(h) != ESP_OK) { copy_error("ota_end"); return false; }
    if (esp_ota_set_boot_partition(ota) != ESP_OK) { copy_error("set_boot"); return false; }
    copy_progress(100);
    lv_label_set_text(copy_title, "Launching...");
    lv_obj_set_style_text_color(copy_title, lv_color_hex(0x00FF80), 0);
    lvgl_pump();
    delay(600);
    esp_restart();
    return true;
}

static void scan_apps() {
    g_app_count = 0;
    File dir = SD_MMC.open("/WATCH/bin");
    if (!dir || !dir.isDirectory()) return;
    File f = dir.openNextFile();
    while (f && g_app_count < MAX_APPS) {
        if (!f.isDirectory()) {
            String n = f.name();
            int s = n.lastIndexOf('/'); if (s >= 0) n = n.substring(s+1);
            String lo = n; lo.toLowerCase();
            if (lo.endsWith(".bin")) {
                g_apps[g_app_count].path = String("/WATCH/bin/") + n;
                g_apps[g_app_count].name = n.substring(0, n.length() - 4);
                g_apps[g_app_count].size = f.size();
                g_app_count++;
            }
        }
        f = dir.openNextFile();
    }
}
static void list_event_cb(lv_event_t *e) {
    lv_event_code_t code = lv_event_get_code(e);
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    if (idx < 0 || idx >= g_app_count) return;
    if (code == LV_EVENT_CLICKED) load_app_from_sd(g_apps[idx]);
    else if (code == LV_EVENT_LONG_PRESSED) { favs_toggle(g_apps[idx].name); populate_app_list(); }
}
static void compute_order(int *out) {
    int n = 0;
    for (int i = 0; i < g_fav_count; i++)
        for (int j = 0; j < g_app_count; j++)
            if (g_apps[j].name == g_favs[i]) { out[n++] = j; break; }
    for (int j = 0; j < g_app_count; j++)
        if (!favs_contains(g_apps[j].name)) out[n++] = j;
}
static void populate_app_list() {
    if (!g_list_widget) return;
    lv_obj_clean(g_list_widget);
    if (!g_app_count) {
        lv_obj_t *e = lv_label_create(g_list_widget);
        lv_label_set_text(e, "No /WATCH/bin/*.bin found.\n\n"
                             "Drop compiled .bin files in\n/WATCH/bin/.");
        lv_obj_set_style_text_align(e, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_text_color(e, lv_color_hex(0xAAAAAA), 0);
        lv_obj_center(e);
        return;
    }
    int order[MAX_APPS]; compute_order(order);
    if (g_fav_count > 0) {
        lv_obj_t *h = lv_label_create(g_list_widget);
        lv_label_set_text(h, LV_SYMBOL_OK "  Favourites");
        lv_obj_set_style_text_color(h, lv_color_hex(0xFFB300), 0);
        lv_obj_set_style_text_font(h, &lv_font_montserrat_14, 0);
        lv_obj_set_style_pad_left(h, 12, 0);
    }
    int sf = 0;
    for (int k = 0; k < g_app_count; k++) {
        int i = order[k];
        bool isFav = favs_contains(g_apps[i].name);
        if (g_fav_count > 0 && sf == g_fav_count && !isFav) {
            lv_obj_t *sep = lv_label_create(g_list_widget);
            lv_label_set_text(sep, "All apps");
            lv_obj_set_style_text_color(sep, lv_color_hex(0x666666), 0);
            lv_obj_set_style_text_font(sep, &lv_font_montserrat_14, 0);
            lv_obj_set_style_pad_left(sep, 12, 0);
            lv_obj_set_style_pad_top(sep, 8, 0);
        }
        if (isFav) sf++;
        char line[96];
        snprintf(line, sizeof(line), "%s    (%.1f KB)",
                 g_apps[i].name.c_str(), g_apps[i].size/1024.0f);
        lv_obj_t *b = lv_list_add_btn(g_list_widget,
                                      isFav ? LV_SYMBOL_OK : LV_SYMBOL_PLAY, line);
        lv_obj_set_style_bg_color(b, lv_color_hex(isFav?0x1A1408:0x111111), 0);
        lv_obj_set_style_text_color(b, lv_color_hex(isFav?0xFFD060:0xFFFFFF), 0);
        lv_obj_set_style_text_font(b, &lv_font_montserrat_16, 0);
        lv_obj_add_event_cb(b, list_event_cb, LV_EVENT_CLICKED,      (void*)(intptr_t)i);
        lv_obj_add_event_cb(b, list_event_cb, LV_EVENT_LONG_PRESSED, (void*)(intptr_t)i);
    }
}

static void build_screen_apps() {
    lv_obj_t *s = lv_obj_create(NULL);
    screens[SCREEN_APPS] = s;
    lv_obj_set_style_bg_color(s, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(s, LV_OPA_COVER, 0);
    lv_obj_clear_flag(s, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *h1 = lv_label_create(s);
    lv_label_set_text(h1, "Apps");
    lv_obj_set_style_text_color(h1, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(h1, &lv_font_montserrat_22, 0);
    lv_obj_align(h1, LV_ALIGN_TOP_LEFT, 24, 22);

    lv_obj_t *h2 = lv_label_create(s);
    lv_label_set_text(h2, "/WATCH/bin  ·  long-press = star");
    lv_obj_set_style_text_color(h2, lv_color_hex(0x00E5FF), 0);
    lv_obj_set_style_text_font(h2, &lv_font_montserrat_14, 0);
    lv_obj_align(h2, LV_ALIGN_TOP_LEFT, 24, 50);

    g_list_widget = lv_list_create(s);
    lv_obj_set_size(g_list_widget, LCD_WIDTH - 30, LCD_HEIGHT - 130);
    lv_obj_align(g_list_widget, LV_ALIGN_TOP_MID, 0, 80);
    lv_obj_set_style_bg_color(g_list_widget, lv_color_hex(0x080808), 0);
    lv_obj_set_style_border_width(g_list_widget, 0, 0);
    populate_app_list();

    lv_obj_t *foot = lv_label_create(s);
    lv_label_set_text(foot, LV_SYMBOL_LEFT "  face   ·   battery  " LV_SYMBOL_RIGHT);
    lv_obj_set_style_text_color(foot, lv_color_hex(0x555555), 0);
    lv_obj_set_style_text_font(foot, &lv_font_montserrat_12, 0);
    lv_obj_align(foot, LV_ALIGN_BOTTOM_MID, 0, -10);
}

// ============================================================================
//  BATTERY SCREEN  --  big % arc + remaining time + voltage
// ============================================================================
static lv_obj_t *batt_arc=nullptr, *batt_pct_lbl=nullptr;
static lv_obj_t *batt_state=nullptr, *batt_remain=nullptr;
static lv_obj_t *batt_volt=nullptr;

// forward decl -- defined further down (after screen_off/screen_on)
static void deep_sleep_now();

static void update_battery_screen() {
    if (!batt_arc) return;
    if (!g_has_pmu || g_batt_pct < 0) {
        lv_label_set_text(batt_pct_lbl, "--%");
        lv_label_set_text(batt_state, "no PMU");
        lv_label_set_text(batt_remain, "");
        lv_label_set_text(batt_volt, "");
        return;
    }
    int pct = g_batt_pct; if (pct<0) pct=0; if (pct>100) pct=100;
    lv_arc_set_value(batt_arc, pct);
    char b[16]; snprintf(b, 16, "%d%%", pct);
    lv_label_set_text(batt_pct_lbl, b);

    lv_color_t col = pct<=15 ? lv_color_hex(0xFF4040)
                   : pct<=30 ? lv_color_hex(0xFFB300)
                             : lv_color_hex(0x00FF80);
    lv_obj_set_style_arc_color(batt_arc, col, LV_PART_INDICATOR);
    lv_obj_set_style_text_color(batt_pct_lbl, col, 0);

    if (g_charging) {
        lv_label_set_text(batt_state, LV_SYMBOL_CHARGE "  charging");
        lv_obj_set_style_text_color(batt_state, lv_color_hex(0x00E5FF), 0);
        lv_label_set_text(batt_remain, "");
    } else {
        lv_label_set_text(batt_state, "on battery");
        lv_obj_set_style_text_color(batt_state, lv_color_hex(0xBBBBBB), 0);
        int mins = batt_minutes_left();
        if (mins < 0) lv_label_set_text(batt_remain, "");
        else {
            char r[48];
            int h = mins / 60, m = mins % 60;
            if (h > 0) snprintf(r, 48, "~ %d h %02d min remaining", h, m);
            else       snprintf(r, 48, "~ %d min remaining", m);
            lv_label_set_text(batt_remain, r);
        }
    }
    char v[32]; snprintf(v, 32, "%.2f V", g_batt_mv / 1000.0f);
    lv_label_set_text(batt_volt, v);
}

static void build_screen_battery() {
    lv_obj_t *s = lv_obj_create(NULL);
    screens[SCREEN_BATTERY] = s;
    lv_obj_set_style_bg_color(s, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(s, LV_OPA_COVER, 0);
    lv_obj_clear_flag(s, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title = lv_label_create(s);
    lv_label_set_text(title, "Battery");
    lv_obj_set_style_text_color(title, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_22, 0);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 24, 22);

    lv_obj_t *sub = lv_label_create(s);
    lv_label_set_text(sub, "AXP2101");
    lv_obj_set_style_text_color(sub, lv_color_hex(0x00E5FF), 0);
    lv_obj_set_style_text_font(sub, &lv_font_montserrat_14, 0);
    lv_obj_align(sub, LV_ALIGN_TOP_LEFT, 24, 50);

    // The big percent arc
    batt_arc = lv_arc_create(s);
    lv_obj_set_size(batt_arc, 280, 280);
    lv_obj_align(batt_arc, LV_ALIGN_CENTER, 0, -10);
    lv_arc_set_rotation(batt_arc, 135);
    lv_arc_set_bg_angles(batt_arc, 0, 270);
    lv_arc_set_range(batt_arc, 0, 100);
    lv_arc_set_value(batt_arc, 0);
    lv_obj_remove_style(batt_arc, NULL, LV_PART_KNOB);
    lv_obj_clear_flag(batt_arc, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_arc_color(batt_arc, lv_color_hex(0x141414), LV_PART_MAIN);
    lv_obj_set_style_arc_width(batt_arc, 14, LV_PART_MAIN);
    lv_obj_set_style_arc_color(batt_arc, lv_color_hex(0x00FF80), LV_PART_INDICATOR);
    lv_obj_set_style_arc_width(batt_arc, 14, LV_PART_INDICATOR);

    batt_pct_lbl = lv_label_create(s);
    lv_label_set_text(batt_pct_lbl, "--%");
    lv_obj_set_style_text_color(batt_pct_lbl, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(batt_pct_lbl, &FACE_TIME_FONT, 0);
    lv_obj_align(batt_pct_lbl, LV_ALIGN_CENTER, 0, -10);

    batt_state = lv_label_create(s);
    lv_label_set_text(batt_state, "");
    lv_obj_set_style_text_color(batt_state, lv_color_hex(0xBBBBBB), 0);
    lv_obj_set_style_text_font(batt_state, &lv_font_montserrat_18, 0);
    lv_obj_align(batt_state, LV_ALIGN_CENTER, 0, 90);

    batt_remain = lv_label_create(s);
    lv_label_set_text(batt_remain, "");
    lv_obj_set_style_text_color(batt_remain, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(batt_remain, &lv_font_montserrat_16, 0);
    lv_obj_align(batt_remain, LV_ALIGN_CENTER, 0, 125);

    batt_volt = lv_label_create(s);
    lv_label_set_text(batt_volt, "");
    lv_obj_set_style_text_color(batt_volt, lv_color_hex(0x666666), 0);
    lv_obj_set_style_text_font(batt_volt, &lv_font_montserrat_14, 0);
    lv_obj_align(batt_volt, LV_ALIGN_CENTER, 0, 160);

    // ---- "Sleep Now" button -- enters deep sleep (RTC kept alive) -------
    lv_obj_t *btn = lv_btn_create(s);
    lv_obj_set_size(btn, 180, 44);
    lv_obj_align(btn, LV_ALIGN_BOTTOM_MID, 0, -40);
    lv_obj_set_style_radius(btn, 22, 0);
    lv_obj_set_style_bg_color(btn, lv_color_hex(0x141414), 0);
    lv_obj_set_style_border_color(btn, lv_color_hex(0x00E5FF), 0);
    lv_obj_set_style_border_width(btn, 1, 0);
    lv_obj_add_event_cb(btn,
        [](lv_event_t *){ deep_sleep_now(); },
        LV_EVENT_CLICKED, NULL);
    lv_obj_t *bl = lv_label_create(btn);
    lv_label_set_text(bl, LV_SYMBOL_POWER "  Sleep Now");
    lv_obj_set_style_text_color(bl, lv_color_hex(0x00E5FF), 0);
    lv_obj_set_style_text_font(bl, &lv_font_montserrat_16, 0);
    lv_obj_center(bl);

    lv_obj_t *foot = lv_label_create(s);
    lv_label_set_text(foot, "press BOOT to wake from deep sleep");
    lv_obj_set_style_text_color(foot, lv_color_hex(0x444444), 0);
    lv_obj_set_style_text_font(foot, &lv_font_montserrat_12, 0);
    lv_obj_align(foot, LV_ALIGN_BOTTOM_MID, 0, -10);
}

// ============================================================================
//  Screen-off  --  AMOLED brightness to 0 (MiniOS pattern, reliable)
//  Using setBrightness instead of displayOff/displayOn because the CO5300
//  sometimes comes back in a flaky state after a DISPOFF/DISPON cycle.
// ============================================================================
static void screen_off() {
    if (screenIsOff) return;
    screenIsOff = true;
    static_cast<Arduino_CO5300 *>(gfx)->setBrightness(0);
}
static void screen_on() {
    if (!screenIsOff) return;
    screenIsOff = false;
    static_cast<Arduino_CO5300 *>(gfx)->setBrightness(DEFAULT_BRIGHTNESS);
    // Drop the touch tracker + swipe queue so the wake-up tap doesn't
    // get interpreted as a stale gesture or click.
    touchActive = false;
    g_pending_swipe = SWIPE_NONE;
    FT3168->IIC_Interrupt_Flag = false;
    g_suppress_touch_until = millis() + WAKE_TOUCH_SUPPRESS_MS;
    lv_obj_invalidate(lv_scr_act());
}

// ============================================================================
//  DEEP SLEEP  --  manual "Sleep Now" from the Battery card.
//  Powers down everything except the internal RTC (which keeps the wall
//  clock ticking) and the GPIO0 / BOOT wake input.  Press BOOT to wake.
//  Draw drops to ~150-300 uA (about 50 days on a 400 mAh pack).
// ============================================================================
static void deep_sleep_now() {
    USBSerial.println("[pm] entering deep sleep -- press BOOT to wake");
    // Quick visual confirmation before we go dark.
    gfx->fillScreen(RGB565_BLACK);
    gfx->setCursor(LCD_WIDTH/2 - 60, LCD_HEIGHT/2);
    gfx->setTextColor(0xFFFF);
    gfx->setTextSize(2);
    gfx->print("sleeping...");
    delay(400);
    static_cast<Arduino_CO5300 *>(gfx)->setBrightness(0);
    // Make sure WiFi + BT are fully off so wake-from-deep-sleep is clean.
    WiFi.disconnect(true, true);
    WiFi.mode(WIFI_OFF);
    btStop();
    // Only the BOOT button (GPIO0) is RTC-capable, so only it can wake us.
    esp_sleep_enable_ext1_wakeup((1ULL << 0), ESP_EXT1_WAKEUP_ANY_LOW);
    esp_deep_sleep_start();   // never returns
}

// ============================================================================
//  SETUP
// ============================================================================
void setup() {
    USBSerial.begin(115200);

    // Are we coming back from a deep sleep (Sleep Now)?  If so, the internal
    // RTC still has the wall clock, so skip splash + WiFi for a fast wake.
    bool from_deep_sleep =
        (esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_EXT1);

    USBSerial.printf("\n=== LauncherOS v5 ===  %s\n",
                     from_deep_sleep ? "(wake from deep sleep)" : "(cold boot)");

#if POWER_SAVE
    // Kill Bluetooth radio -- we never use it.  Saves ~5-10 mA.
    btStop();
    // Make sure GPIO0 (BOOT / power button) has its internal pull-up
    // enabled so it reads HIGH at rest -- light-sleep wake on LOW depends
    // on a clean idle level.
    pinMode(0, INPUT_PULLUP);
#endif

    if (!gfx->begin()) USBSerial.println("gfx->begin() failed");
    gfx->fillScreen(RGB565_BLACK);
#if POWER_SAVE
    // setBrightness() is on the derived Arduino_CO5300 class, not the
    // base Arduino_GFX -- cast so the lookup hits the right method.
    static_cast<Arduino_CO5300 *>(gfx)->setBrightness(DEFAULT_BRIGHTNESS);
#endif

    // I2C bus recovery: clock out any stuck transaction before Wire.begin()
    pinMode(IIC_SCL, OUTPUT);
    for (int i = 0; i < 9; i++) {
        digitalWrite(IIC_SCL, LOW); delayMicroseconds(5);
        digitalWrite(IIC_SCL, HIGH); delayMicroseconds(5);
    }

    Wire.begin(IIC_SDA, IIC_SCL);

    // Touch controller reset pulse
    pinMode(TP_RESET, OUTPUT);
    digitalWrite(TP_RESET, LOW);
    delay(50);
    digitalWrite(TP_RESET, HIGH);
    delay(200);

    pinMode(TP_INT, INPUT_PULLUP);
    while (FT3168->begin() == false) { USBSerial.println("FT3168 init fail"); delay(500); }
    FT3168->IIC_Write_Device_State(
        FT3168->Arduino_IIC_Touch::Device::TOUCH_POWER_MODE,
        FT3168->Arduino_IIC_Touch::Device_Mode::TOUCH_POWER_MONITOR);

    pmu_init();
    pmu_refresh();
    favs_load();

    lv_init();
    size_t bufPx = LCD_WIDTH * 60;
    lvgl_buf = (lv_color_t *)heap_caps_malloc(bufPx*sizeof(lv_color_t),
                                              MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT);
    if (!lvgl_buf) lvgl_buf = (lv_color_t *)heap_caps_malloc(bufPx*sizeof(lv_color_t), MALLOC_CAP_8BIT);
    lv_disp_draw_buf_init(&draw_buf, lvgl_buf, NULL, bufPx);

    lv_disp_drv_init(&disp_drv);
    disp_drv.flush_cb   = my_disp_flush;
    disp_drv.rounder_cb = rounder_event_cb;
    disp_drv.draw_buf   = &draw_buf;
    disp_drv.hor_res    = LCD_WIDTH;
    disp_drv.ver_res    = LCD_HEIGHT;
    lv_disp_drv_register(&disp_drv);

    lv_indev_drv_init(&indev_drv);
    indev_drv.type    = LV_INDEV_TYPE_POINTER;
    indev_drv.read_cb = my_touchpad_read;
    lv_indev_drv_register(&indev_drv);

    build_splash();
    splash_progress(10, "init display");
    splash_progress(25, "mounting SD");
    SD_MMC.setPins(SDMMC_CLK, SDMMC_CMD, SDMMC_DATA);
    bool sd_ok = SD_MMC.begin("/sdcard", true);
    if (sd_ok) {
        splash_progress(40, "wifi config");      parse_wifi_config();
        if (from_deep_sleep) {
            // RTC kept time -- skip WiFi entirely on this wake.
            struct tm tm;
            if (getLocalTime(&tm, 5)) g_ntp_synced = true;
            splash_progress(70, "skipping wifi (RTC retained)");
        } else {
            splash_progress(55, "connecting wifi");
            wifi_connect_and_sync();
        }
        splash_progress(82, "scanning apps");    scan_apps();
        splash_progress(92, "scanning photos");  scan_photos();
    } else {
        splash_progress(50, "SD mount failed");  delay(800);
    }
    splash_progress(100, "ready");
    delay(from_deep_sleep ? 100 : 400);   // quick wake when coming back

    // Build all three screens once, then start on the watch face.
    build_screen_face();
    build_screen_apps();
    build_screen_battery();
    current_screen = SCREEN_FACE;
    lv_scr_load_anim(screens[SCREEN_FACE], LV_SCR_LOAD_ANIM_NONE, 0, 0, false);
    lastTouchMs = millis();

#if POWER_SAVE
    // Drop the CPU to 80 MHz now that boot/WiFi/SD are done.  Plenty fast
    // for LVGL on this panel.  Bumped back up to 240 only during OTA copy.
    setCpuFrequencyMhz(CPU_FREQ_IDLE_MHZ);
    USBSerial.printf("[pm] CPU @ %d MHz\n", CPU_FREQ_IDLE_MHZ);
#endif
}

// ============================================================================
//  LOOP
// ============================================================================
void loop() {
    // LVGL tick -- skipped when LV_TICK_CUSTOM == 1 (our lv_conf.h sets it).
    static uint32_t lastMs = 0;
    uint32_t now = millis();
#if !defined(LV_TICK_CUSTOM) || (LV_TICK_CUSTOM == 0)
    lv_tick_inc(now - lastMs);
#endif
    lastMs = now;

    // While the panel is off, skip LVGL entirely -- no point rendering into
    // a powered-down AMOLED, and it keeps the CPU cool.  We still poll the
    // touch interrupt below so we can wake on the next tap.
    if (!screenIsOff) {
        lvgl_pump();
    }

    // Consume any queued swipe (set by my_touchpad_read on release).
    if (!screenIsOff && g_pending_swipe != SWIPE_NONE) {
        SwipeEvent ev = g_pending_swipe;
        g_pending_swipe = SWIPE_NONE;
        if (ev == SWIPE_LEFT)  goto_screen(current_screen + 1);
        if (ev == SWIPE_RIGHT) goto_screen(current_screen - 1);
    }

    // 1s tick - update only the visible screen.
    static uint32_t tT = 0;
    if (now - tT > 1000) {
        tT = now;
        if (!screenIsOff) {
            if (current_screen == SCREEN_FACE)    update_face_time();
            if (current_screen == SCREEN_BATTERY) update_battery_screen();
        }
    }
    // 3s tick - refresh battery readings (PMU) regardless of screen.
    static uint32_t tB = 0;
    if (now - tB > 3000) {
        tB = now;
        if (!screenIsOff) {
            pmu_refresh();
            if (current_screen == SCREEN_FACE)    update_face_battery();
            if (current_screen == SCREEN_BATTERY) update_battery_screen();
        }
    }

    // Screen-off timeout.
    if (!screenIsOff && (now - lastTouchMs > SCREEN_OFF_TIMEOUT_MS)) {
        screen_off();
    } else if (screenIsOff && FT3168->IIC_Interrupt_Flag) {
        lastTouchMs = now;
        screen_on();
    }

#if POWER_SAVE
    // Periodic NTP re-sync (briefly turns WiFi back on, syncs, drops it).
    if (!screenIsOff) wifi_resync_if_due();
#endif

#if POWER_SAVE && LIGHT_SLEEP_WHEN_OFF
    // When the screen is off, sleep the SoC until either:
    //   * the BOOT/power button pulls GPIO0 LOW   (ext1 wake -- instant)
    //   * the 80 ms timer wakes us so we can poll the touch line
    //     (TP_INT / GPIO38 is NOT an RTC IO on the ESP32-S3, so it can't
    //      directly wake the chip -- we poll it with digitalRead instead)
    if (screenIsOff) {
        const uint64_t WAKE_MASK = (1ULL << 0);             // BOOT button only
        esp_sleep_enable_ext1_wakeup(WAKE_MASK, ESP_EXT1_WAKEUP_ANY_LOW);
        esp_sleep_enable_timer_wakeup(80ULL * 1000ULL);     // 80 ms touch poll
        esp_light_sleep_start();

        esp_sleep_wakeup_cause_t why = esp_sleep_get_wakeup_cause();
        bool wake = false;

        if (why == ESP_SLEEP_WAKEUP_EXT1) {
            // BOOT button pressed
            wake = true;
        } else if (why == ESP_SLEEP_WAKEUP_TIMER) {
            // Timer tick -- quickly check if the touch IRQ line is low
            // (FT3168 pulls TP_INT LOW while a finger is on the screen).
            if (digitalRead(TP_INT) == LOW) wake = true;
        }

        if (wake) {
            FT3168->IIC_Interrupt_Flag = true;
            lastTouchMs = millis();
        }
        return;   // skip the delay() below -- we already slept
    }
#endif

    delay(5);
}
