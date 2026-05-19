// =============================================================================
//  PomoOS.ino v1.0 -- LauncherOS-compatible Pomodoro / Deep-work timer
//  Target  : Waveshare ESP32-S3 Touch AMOLED 2.06 (410 x 502, CO5300, FT3168)
//  Compile : ESP32-S3 Dev Module / PSRAM=OPI / Flash=16MB / Minimal SPIFFS (large APP)
//  Export  : Sketch -> Export Compiled Binary  ->  copy PomoOS.ino.bin to
//            /WATCH/bin/pomoos.bin on the SD card.
// -----------------------------------------------------------------------------
//  Splash  : single static ring + "POMO::OS"
//  SD root : /WATCH/focus/      (per-month CSV totals)
//  NVS ns  : "pomoos"
//  Features:
//    [1] 25 / 5 / 15 Pomodoro phases with auto-advance
//    [2] Big circular progress ring on AMOLED-black background
//    [3] Daily streak + total-focus-hours counter
//    [4] CSV log /WATCH/focus/YYYY-MM.csv (one row per completed pomodoro)
//    [5] "Cathedral mode" -- brightness 8, only the ring visible
//    [6] Custom durations via /WATCH/focus/config.txt (focus,short,long mins)
// =============================================================================

#include <Arduino.h>
#include <FS.h>
#include <SD_MMC.h>
#include <Wire.h>
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

static void my_disp_flush(lv_disp_drv_t *d, const lv_area_t *a, lv_color_t *px) {
    uint32_t w=a->x2-a->x1+1, h=a->y2-a->y1+1;
    gfx->draw16bitRGBBitmap(a->x1, a->y1, (uint16_t*)px, w, h);
    lv_disp_flush_ready(d);
}
static void my_touchpad_read(lv_indev_drv_t *, lv_indev_data_t *data) {
    int32_t x=FT3168->IIC_Read_Device_Value(FT3168->Arduino_IIC_Touch::Value_Information::TOUCH_COORDINATE_X);
    int32_t y=FT3168->IIC_Read_Device_Value(FT3168->Arduino_IIC_Touch::Value_Information::TOUCH_COORDINATE_Y);
    int fingers=FT3168->IIC_Read_Device_Value(FT3168->Arduino_IIC_Touch::Value_Information::TOUCH_FINGER_NUMBER);
    if (fingers > 0) { data->state=LV_INDEV_STATE_PR; data->point.x=x; data->point.y=y; }
    else             { data->state=LV_INDEV_STATE_REL; }
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
    size_t bufPx = LCD_WIDTH * 40;
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

static void time_restore_from_launcher() {
    Preferences p; if (!p.begin("rtcsave", true)) return;
    uint32_t t = p.getULong("t", 0); long gmt = p.getLong("gmt", 0); p.end();
    if (t > 1700000000) { struct timeval tv={.tv_sec=(time_t)t,.tv_usec=0}; settimeofday(&tv, NULL);
        char tz[16]; snprintf(tz, sizeof(tz), "UTC%+ld", -gmt/3600); setenv("TZ", tz, 1); tzset(); }
}

static void set_brightness(uint8_t v) {
    bus->beginWrite(); bus->writeCommand(0x51); bus->write(v); bus->endWrite();
}
// ===== END SHARED BOOT BLOCK =================================================

// Splash
static void splash_show() {
    int cx = LCD_WIDTH/2, cy = LCD_HEIGHT/2;
    for (int f=0; f<14; f++) {
        gfx->fillScreen(0x0000);
        int r = 120 - f*2;
        for (int rr = r-2; rr <= r+2; rr++) gfx->drawCircle(cx, cy, rr, 0xFB60);   // amber
        gfx->setTextSize(3);
        gfx->setTextColor(0xFFFF);
        gfx->setCursor(cx - 80, cy - 12);
        gfx->print("POMO::OS");
        delay(45);
    }
}

#define COL_BG 0x000000
#define COL_RING_BG 0x111111
#define COL_FOCUS 0xFF4040
#define COL_SHORT 0x00E5FF
#define COL_LONG  0x00FF80
#define COL_MUTE 0x666666
#define COL_TXT 0xFFFFFF

static int g_min_focus = 25;
static int g_min_short = 5;
static int g_min_long  = 15;
static int g_phase = 0;       // 0=focus, 1=short, 2=long
static int g_pomos_today = 0; // resets at local midnight
static uint32_t g_phase_end_ms = 0;
static bool g_running = false;
static int g_streak_days = 0;
static int g_total_pomos = 0;
static char g_today_key[16] = "";

static lv_obj_t *scr_root, *arc, *time_lbl, *phase_lbl, *streak_lbl, *cathedral_btn;
static bool g_cathedral = false;

static void csv_log_pomodoro() {
    time_t now; time(&now);
    struct tm tm; localtime_r(&now, &tm);
    char path[40]; snprintf(path, 40, "/WATCH/focus/%04d-%02d.csv", tm.tm_year+1900, tm.tm_mon+1);
    File f = SD_MMC.open(path, FILE_APPEND);
    if (!f) { SD_MMC.mkdir("/WATCH/focus"); f = SD_MMC.open(path, FILE_APPEND); }
    if (f) {
        char b[64]; snprintf(b,64,"%04d-%02d-%02d,%02d:%02d,%d\n",
            tm.tm_year+1900, tm.tm_mon+1, tm.tm_mday, tm.tm_hour, tm.tm_min, g_min_focus);
        f.print(b); f.close();
    }
}
static void check_streak_rollover() {
    time_t now; time(&now);
    if (now < 1700000000) return;
    struct tm tm; localtime_r(&now, &tm);
    char tk[16]; snprintf(tk,16,"%04d%02d%02d", tm.tm_year+1900, tm.tm_mon+1, tm.tm_mday);
    if (strcmp(tk, g_today_key) != 0) {
        // new day -- if yesterday we had >=1 pomo, streak++, else reset
        Preferences p; p.begin("pomoos", false);
        char last[16]; strlcpy(last, p.getString("today","").c_str(), 16);
        int last_n = p.getInt("today_n", 0);
        if (strcmp(tk, last) != 0) {
            // Was last day yesterday?
            struct tm yt = tm; yt.tm_mday -= 1; mktime(&yt);
            char yk[16]; snprintf(yk,16,"%04d%02d%02d", yt.tm_year+1900, yt.tm_mon+1, yt.tm_mday);
            if (strcmp(last, yk) == 0 && last_n > 0) { g_streak_days++; }
            else if (last[0]) g_streak_days = (last_n > 0) ? 1 : 0;
            p.putInt("streak", g_streak_days);
        }
        g_pomos_today = 0;
        strlcpy(g_today_key, tk, 16);
        p.putString("today", g_today_key);
        p.putInt("today_n", 0);
        p.end();
    }
}

static void persist_session() {
    Preferences p; p.begin("pomoos", false);
    p.putInt("total", g_total_pomos);
    p.putInt("today_n", g_pomos_today);
    p.putInt("streak", g_streak_days);
    p.end();
}

static void update_ui() {
    int total = g_phase==0 ? g_min_focus*60 : g_phase==1 ? g_min_short*60 : g_min_long*60;
    int left  = g_running ? (int)((g_phase_end_ms - millis()) / 1000) : total;
    if (left < 0) left = 0;
    int pct = total > 0 ? 100 - (left*100)/total : 0;
    if (arc) lv_arc_set_value(arc, pct);
    char b[16]; snprintf(b, 16, "%02d:%02d", left/60, left%60);
    if (time_lbl) lv_label_set_text(time_lbl, b);
    const char *names[] = {"FOCUS","SHORT BREAK","LONG BREAK"};
    if (phase_lbl) lv_label_set_text(phase_lbl, names[g_phase]);
    char sb[40]; snprintf(sb,40,"streak %d  -  today %d", g_streak_days, g_pomos_today);
    if (streak_lbl) lv_label_set_text(streak_lbl, sb);
    uint32_t col = g_phase==0 ? COL_FOCUS : g_phase==1 ? COL_SHORT : COL_LONG;
    if (arc) {
        lv_obj_set_style_arc_color(arc, lv_color_hex(col), LV_PART_INDICATOR);
        lv_obj_set_style_text_color(time_lbl, lv_color_hex(col), 0);
    }
}

static void next_phase() {
    if (g_phase == 0) {
        g_total_pomos++;
        g_pomos_today++;
        csv_log_pomodoro();
        persist_session();
        // every 4 focus blocks -> long break
        g_phase = (g_pomos_today % 4 == 0) ? 2 : 1;
    } else {
        g_phase = 0;
    }
    int mins = g_phase==0 ? g_min_focus : g_phase==1 ? g_min_short : g_min_long;
    g_phase_end_ms = millis() + (uint32_t)mins * 60UL * 1000UL;
}

static void start_stop_cb(lv_event_t *) {
    g_running = !g_running;
    if (g_running) {
        int mins = g_phase==0 ? g_min_focus : g_phase==1 ? g_min_short : g_min_long;
        g_phase_end_ms = millis() + (uint32_t)mins * 60UL * 1000UL;
    }
}
static void reset_cb(lv_event_t *) {
    g_running = false; g_phase = 0;
    update_ui();
}
static void cathedral_cb(lv_event_t *) {
    g_cathedral = !g_cathedral;
    set_brightness(g_cathedral ? 8 : 200);
    lv_obj_t *lbl = lv_obj_get_child(cathedral_btn, 0);
    if (lbl) lv_label_set_text(lbl, g_cathedral ? "EXIT CATHEDRAL" : "CATHEDRAL");
}

static void read_config() {
    File f = SD_MMC.open("/WATCH/focus/config.txt", FILE_READ);
    if (!f) return;
    String s = f.readString(); f.close();
    int a = s.indexOf(','); int b = s.indexOf(',', a+1);
    if (a > 0 && b > 0) {
        g_min_focus = s.substring(0, a).toInt();
        g_min_short = s.substring(a+1, b).toInt();
        g_min_long  = s.substring(b+1).toInt();
    }
}

static void build_ui() {
    scr_root = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr_root, lv_color_hex(COL_BG), 0);
    lv_obj_set_style_bg_opa(scr_root, LV_OPA_COVER, 0);

    arc = lv_arc_create(scr_root);
    lv_obj_set_size(arc, 360, 360);
    lv_obj_align(arc, LV_ALIGN_TOP_MID, 0, 30);
    lv_arc_set_range(arc, 0, 100);
    lv_arc_set_value(arc, 0);
    lv_obj_remove_style(arc, NULL, LV_PART_KNOB);
    lv_obj_clear_flag(arc, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_arc_color(arc, lv_color_hex(COL_RING_BG), LV_PART_MAIN);
    lv_obj_set_style_arc_width(arc, 18, LV_PART_MAIN);
    lv_obj_set_style_arc_width(arc, 18, LV_PART_INDICATOR);
    lv_arc_set_bg_angles(arc, 135, 45);  // big sweep

    time_lbl = lv_label_create(scr_root);
    lv_label_set_text(time_lbl, "25:00");
    lv_obj_set_style_text_color(time_lbl, lv_color_hex(COL_FOCUS), 0);
    lv_obj_set_style_text_font(time_lbl, &lv_font_montserrat_48, 0);
    lv_obj_align(time_lbl, LV_ALIGN_TOP_MID, 0, 180);

    phase_lbl = lv_label_create(scr_root);
    lv_label_set_text(phase_lbl, "FOCUS");
    lv_obj_set_style_text_color(phase_lbl, lv_color_hex(COL_TXT), 0);
    lv_obj_set_style_text_font(phase_lbl, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_letter_space(phase_lbl, 4, 0);
    lv_obj_align(phase_lbl, LV_ALIGN_TOP_MID, 0, 140);

    streak_lbl = lv_label_create(scr_root);
    lv_label_set_text(streak_lbl, "streak 0  -  today 0");
    lv_obj_set_style_text_color(streak_lbl, lv_color_hex(COL_MUTE), 0);
    lv_obj_set_style_text_font(streak_lbl, &lv_font_montserrat_12, 0);
    lv_obj_align(streak_lbl, LV_ALIGN_TOP_MID, 0, 240);

    // start / stop
    lv_obj_t *ss = lv_btn_create(scr_root);
    lv_obj_set_size(ss, 130, 44);
    lv_obj_align(ss, LV_ALIGN_CENTER, -75, 100);
    lv_obj_set_style_bg_color(ss, lv_color_hex(COL_FOCUS), 0);
    lv_obj_set_style_radius(ss, 22, 0);
    lv_obj_t *ssl = lv_label_create(ss); lv_label_set_text(ssl, "START / STOP");
    lv_obj_set_style_text_color(ssl, lv_color_hex(0x000000),0);
    lv_obj_set_style_text_font(ssl, &lv_font_montserrat_14, 0);
    lv_obj_center(ssl);
    lv_obj_add_event_cb(ss, start_stop_cb, LV_EVENT_CLICKED, NULL);

    // reset
    lv_obj_t *rs = lv_btn_create(scr_root);
    lv_obj_set_size(rs, 100, 44);
    lv_obj_align(rs, LV_ALIGN_CENTER, 75, 100);
    lv_obj_set_style_bg_color(rs, lv_color_hex(0x141414), 0);
    lv_obj_set_style_border_color(rs, lv_color_hex(COL_MUTE), 0);
    lv_obj_set_style_border_width(rs, 1, 0);
    lv_obj_set_style_radius(rs, 22, 0);
    lv_obj_t *rsl = lv_label_create(rs); lv_label_set_text(rsl, "RESET");
    lv_obj_set_style_text_color(rsl, lv_color_hex(COL_MUTE),0);
    lv_obj_set_style_text_font(rsl, &lv_font_montserrat_14, 0);
    lv_obj_center(rsl);
    lv_obj_add_event_cb(rs, reset_cb, LV_EVENT_CLICKED, NULL);

    cathedral_btn = lv_btn_create(scr_root);
    lv_obj_set_size(cathedral_btn, 200, 38);
    lv_obj_align(cathedral_btn, LV_ALIGN_BOTTOM_MID, 0, -64);
    lv_obj_set_style_bg_color(cathedral_btn, lv_color_hex(0x141414), 0);
    lv_obj_set_style_border_color(cathedral_btn, lv_color_hex(COL_MUTE), 0);
    lv_obj_set_style_border_width(cathedral_btn, 1, 0);
    lv_obj_set_style_radius(cathedral_btn, 19, 0);
    lv_obj_t *cl = lv_label_create(cathedral_btn);
    lv_label_set_text(cl, "CATHEDRAL");
    lv_obj_set_style_text_color(cl, lv_color_hex(COL_MUTE),0);
    lv_obj_set_style_text_font(cl, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_letter_space(cl, 3, 0);
    lv_obj_center(cl);
    lv_obj_add_event_cb(cathedral_btn, cathedral_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *back = lv_btn_create(scr_root);
    lv_obj_set_size(back, 200, 42);
    lv_obj_align(back, LV_ALIGN_BOTTOM_MID, 0, -16);
    lv_obj_set_style_bg_color(back, lv_color_hex(0x141414), 0);
    lv_obj_set_style_border_color(back, lv_color_hex(0x00E5FF), 0);
    lv_obj_set_style_border_width(back, 1, 0);
    lv_obj_set_style_radius(back, 22, 0);
    lv_obj_t *bl = lv_label_create(back);
    lv_label_set_text(bl, LV_SYMBOL_LEFT "  Back to Launcher");
    lv_obj_set_style_text_color(bl, lv_color_hex(0x00E5FF), 0);
    lv_obj_set_style_text_font(bl, &lv_font_montserrat_14, 0);
    lv_obj_center(bl);
    lv_obj_add_event_cb(back, [](lv_event_t*){ return_to_launcher(); }, LV_EVENT_CLICKED, NULL);

    lv_scr_load(scr_root);
}

void setup() {
    launcher_app_boot();
    splash_show();
    // Guarantee /WATCH and its subdirs exist (works on a brand-new SD card)
    SD_MMC.mkdir("/WATCH");
    SD_MMC.mkdir("/WATCH/bin");
    SD_MMC.mkdir("/WATCH/wifi");
    SD_MMC.mkdir("/WATCH/focus");
    time_restore_from_launcher();
    read_config();
    Preferences p; p.begin("pomoos", true);
    g_total_pomos  = p.getInt("total", 0);
    g_pomos_today  = p.getInt("today_n", 0);
    g_streak_days  = p.getInt("streak", 0);
    strlcpy(g_today_key, p.getString("today","").c_str(), 16);
    p.end();
    check_streak_rollover();
    build_ui();
    update_ui();
}

void loop() {
    lv_timer_handler();
    uint32_t now = millis();
    static uint32_t btn_t = 0;
    if (digitalRead(BOOT_BTN_PIN) == LOW) {
        if (!btn_t) btn_t = now;
        else if (now - btn_t > 1500) return_to_launcher();
    } else btn_t = 0;

    static uint32_t t_ui = 0;
    if (now - t_ui > 250) { t_ui = now; update_ui(); }

    if (g_running && (int32_t)(g_phase_end_ms - now) <= 0) {
        next_phase();
        // tiny brightness pulse to mark phase change
        set_brightness(255); delay(80); set_brightness(g_cathedral ? 8 : 200);
    }

    static uint32_t t_day = 0;
    if (now - t_day > 60000) { t_day = now; check_streak_rollover(); }
    delay(5);
}
