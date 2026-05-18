// =============================================================================
//  StudyOS.ino v1.0 -- LauncherOS-compatible deep-focus / discipline OS
//  Target  : Waveshare ESP32-S3 Touch AMOLED 2.06  (410 x 502, CO5300, FT3168)
//  Compile : ESP32-S3 Dev Module / PSRAM=OPI / Flash=16MB / Minimal SPIFFS (large APP)
//  Export  : Sketch -> Export Compiled Binary  ->  copy StudyOS.ino.bin to
//            /WATCH/bin/studyos.bin on the SD card.
// -----------------------------------------------------------------------------
//  Aesthetic : matte black + soft white + muted blue accent. Monochrome calm.
//  Vibe      : premium productivity device.  Anti-distraction by design.
//
//  Features:
//   [1] FOCUS MODE        - clean home, blocks toasts/notifications, shows only
//                           today's tasks + active timer.
//   [2] POMODORO ENGINE   - presets: DEEP 50/10, FOCUS 25/5, QUICK 15/3, EXAM 90/0.
//   [3] TASK SYSTEM       - loads /WATCH/study/tasks.txt (one task per line),
//                           tap to toggle done. Done count persisted to NVS.
//   [4] FLASHCARD SYSTEM  - /WATCH/study/cards/*.txt   format: front|back
//                           swipe right = remembered, left = need-review.
//   [5] QUICK NOTES       - tap NOTE button -> appends timestamped ping to
//                           /WATCH/study/notes.txt (full transcription requires
//                           the LauncherOS Voice service -- placeholder here).
//   [6] FOCUS ANALYTICS   - today's minutes, current streak, total sessions.
//   [7] EXAM MODE         - kills tabs, shows only big countdown + subject.
//   [8] FLOW MODE         - after 20 min uninterrupted focus, UI simplifies +
//                           darkens to encourage deep work.
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
#include <driver/i2s.h>

// ---- Forward type declarations (Arduino IDE hoists function prototypes
//      above the file, so any enum/struct used in a return/arg type must be
//      defined before those prototypes — i.e. here, at the very top.) ----
enum Tab { TAB_HOME = 0, TAB_POMO, TAB_TASK, TAB_CARDS, TAB_ANALYTICS };
enum PomoPhase { PH_IDLE = 0, PH_WORK, PH_BREAK, PH_DONE };
struct PomoPreset { const char *name; int work_min; int break_min; };
struct Task { char text[64]; bool done; };
struct Card { char front[80]; char pinyin[80]; char back[80]; bool has_pinyin; };

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

static lv_disp_draw_buf_t draw_buf;
static lv_color_t        *lvgl_buf = nullptr;
static lv_disp_drv_t      disp_drv;
static lv_indev_drv_t     indev_drv;
static bool               g_touch_pressed_now = false;
static uint32_t           g_last_touch_t = 0;
static int32_t            g_touch_x = 0, g_touch_y = 0;
static int32_t            g_touch_dx = 0;

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
//  NVS clock restore + WiFi/NTP
// =============================================================================
static Preferences g_timeprefs;
static int32_t     g_gmt_offset_sec = 0;
static bool        g_time_valid     = false;
static uint32_t    g_last_ntp_ok    = 0;
static bool        g_wifi_synced    = false;

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
//  Study palette
// =============================================================================
#define S_BG       0x000000
#define S_SURFACE  0x0E0E0E
#define S_BORDER   0x1C1C1C
#define S_TXT      0xF2F2F2
#define S_MUTE     0x808080
#define S_DIM      0x4A4A4A
#define S_ACCENT   0x4FB3FF   // muted blue
#define S_OK       0x4FFFA0   // streak / done
#define S_WARN     0xFFB300
#define S_CYAN     0x00E5FF   // back-to-launcher only
#define S_FLOW_BG  0x000000   // flow mode bg
#define S_FLOW_TXT 0xA0A0A0   // dimmed text in flow

// =============================================================================
//  State
// =============================================================================
static const PomoPreset PRESETS[] = {
    { "DEEP",  50, 10 },
    { "FOCUS", 25,  5 },
    { "QUICK", 15,  3 },
    { "EXAM",  90,  0 }
};
#define NUM_PRESETS 4

static int        g_preset_idx = 1;        // default FOCUS
static PomoPhase  g_phase      = PH_IDLE;
static uint32_t   g_phase_t0   = 0;
static uint32_t   g_phase_len_ms = 0;
static bool       g_exam_mode  = false;

// Subject tagging
static const char *SUBJECTS[] = { "GENERAL", "MATH", "SCIENCE", "LANG", "HISTORY", "OTHER" };
#define NUM_SUBJECTS 6
static int g_subject_idx = 0;
// Per-subject seconds today (parallel array to SUBJECTS)
static uint32_t g_subject_seconds[NUM_SUBJECTS] = {0};

// Analytics (persisted to NVS namespace "studyos")
static Preferences g_prefs;
static uint32_t g_today_seconds  = 0;
static uint32_t g_total_sessions = 0;
static uint32_t g_streak_days    = 0;
static uint16_t g_streak_yday    = 0xFFFF;  // last counted day-of-year
static uint16_t g_today_yday     = 0xFFFF;

// Flow mode trigger
#define FLOW_THRESHOLD_S (20 * 60)
static bool g_flow = false;

// Tasks & cards
#define MAX_TASKS 16
static Task g_tasks[MAX_TASKS];
static int  g_task_count = 0;
static uint32_t g_task_done_bits = 0;        // bitmask of done tasks (persisted)

#define MAX_CARDS 200
static Card g_cards[MAX_CARDS];
static int  g_card_count = 0;
static int  g_card_idx   = 0;
static bool g_card_flipped = false;
static int  g_card_remembered = 0, g_card_review = 0;

// =============================================================================
//  Persistence
// =============================================================================
static void prefs_load() {
    g_prefs.begin("studyos", true);
    g_today_seconds  = g_prefs.getULong("today_s", 0);
    g_total_sessions = g_prefs.getULong("sessions", 0);
    g_streak_days    = g_prefs.getULong("streak", 0);
    g_streak_yday    = g_prefs.getUShort("syday", 0xFFFF);
    g_today_yday     = g_prefs.getUShort("tyday", 0xFFFF);
    g_task_done_bits = g_prefs.getULong("tasks_done", 0);
    g_preset_idx     = g_prefs.getUInt("preset", 1);
    g_subject_idx    = g_prefs.getUInt("subj", 0);
    for (int i = 0; i < NUM_SUBJECTS; i++) {
        char k[8]; snprintf(k, sizeof(k), "ss%d", i);
        g_subject_seconds[i] = g_prefs.getULong(k, 0);
    }
    g_prefs.end();
}
static void prefs_save() {
    g_prefs.begin("studyos", false);
    g_prefs.putULong ("today_s", g_today_seconds);
    g_prefs.putULong ("sessions", g_total_sessions);
    g_prefs.putULong ("streak", g_streak_days);
    g_prefs.putUShort("syday", g_streak_yday);
    g_prefs.putUShort("tyday", g_today_yday);
    g_prefs.putULong ("tasks_done", g_task_done_bits);
    g_prefs.putUInt  ("preset", g_preset_idx);
    g_prefs.putUInt  ("subj", g_subject_idx);
    for (int i = 0; i < NUM_SUBJECTS; i++) {
        char k[8]; snprintf(k, sizeof(k), "ss%d", i);
        g_prefs.putULong(k, g_subject_seconds[i]);
    }
    g_prefs.end();
}

static void rollover_day_if_needed() {
    struct tm tm;
    if (!getLocalTime(&tm, 5)) return;
    uint16_t yd = tm.tm_yday;
    if (g_today_yday == 0xFFFF) { g_today_yday = yd; return; }
    if (yd != g_today_yday) {
        // [D] Export yesterday's summary as CSV (SportOS-style)
        char path[64];
        snprintf(path, sizeof(path), "/WATCH/study/exports/%04d%02d%02d.csv",
                 tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday);
        if (!SD_MMC.exists(path)) {
            File f = SD_MMC.open(path, FILE_WRITE);
            if (f) {
                f.println("metric,value");
                f.printf("focus_minutes,%lu\n", g_today_seconds / 60);
                f.printf("sessions,%lu\n",      g_total_sessions);
                f.printf("streak_days,%lu\n",   g_streak_days);
                int done = 0;
                for (int i = 0; i < g_task_count; i++) if (g_tasks[i].done) done++;
                f.printf("tasks_done,%d\n",     done);
                f.printf("tasks_total,%d\n",    g_task_count);
                f.printf("cards_remembered,%d\n", g_card_remembered);
                f.printf("cards_review,%d\n",     g_card_review);
                f.close();
            }
        }
        // [D] Reset day-scoped counters
        g_today_seconds   = 0;
        g_task_done_bits  = 0;
        g_card_remembered = 0;
        g_card_review     = 0;
        for (int i = 0; i < g_task_count; i++) g_tasks[i].done = false;
        for (int i = 0; i < NUM_SUBJECTS; i++) g_subject_seconds[i] = 0;
        g_today_yday      = yd;
        prefs_save();
    }
}

// =============================================================================
//  Task + card loading
// =============================================================================
static void load_tasks() {
    g_task_count = 0;
    File f = SD_MMC.open("/WATCH/study/tasks.txt", FILE_READ);
    if (!f) return;
    while (f.available() && g_task_count < MAX_TASKS) {
        String line = f.readStringUntil('\n'); line.trim();
        if (!line.length() || line.startsWith("#")) continue;
        strncpy(g_tasks[g_task_count].text, line.c_str(),
                sizeof(g_tasks[g_task_count].text) - 1);
        g_tasks[g_task_count].text[sizeof(g_tasks[g_task_count].text) - 1] = 0;
        g_tasks[g_task_count].done = (g_task_done_bits >> g_task_count) & 1;
        g_task_count++;
    }
    f.close();
}

static void load_cards() {
    g_card_count = 0;
    File dir = SD_MMC.open("/WATCH/study/cards");
    if (!dir || !dir.isDirectory()) return;
    File f = dir.openNextFile();
    while (f && g_card_count < MAX_CARDS) {
        String n = f.name(); n.toLowerCase();
        if (n.endsWith(".txt")) {
            while (f.available() && g_card_count < MAX_CARDS) {
                String line = f.readStringUntil('\n'); line.trim();
                if (!line.length() || line.startsWith("#")) continue;
                int p1 = line.indexOf('|');
                if (p1 < 0) continue;
                int p2 = line.indexOf('|', p1 + 1);
                Card &c = g_cards[g_card_count];
                memset(&c, 0, sizeof(c));
                if (p2 < 0) {
                    // 2-field: front|back
                    strncpy(c.front, line.substring(0, p1).c_str(), sizeof(c.front)-1);
                    strncpy(c.back,  line.substring(p1 + 1).c_str(),  sizeof(c.back)-1);
                    c.has_pinyin = false;
                } else {
                    // 3-field: front|pinyin|back  (Chinese cheat-sheet mode)
                    strncpy(c.front,  line.substring(0, p1).c_str(),         sizeof(c.front)-1);
                    strncpy(c.pinyin, line.substring(p1 + 1, p2).c_str(),    sizeof(c.pinyin)-1);
                    strncpy(c.back,   line.substring(p2 + 1).c_str(),        sizeof(c.back)-1);
                    c.has_pinyin = true;
                }
                g_card_count++;
            }
        }
        f = dir.openNextFile();
    }
}

// =============================================================================
//  UI -- widgets
// =============================================================================
static lv_obj_t *scr_root;
static lv_obj_t *tabv;
static lv_obj_t *tab_home, *tab_pomo, *tab_task, *tab_cards, *tab_analytics;

// Home
static lv_obj_t *home_clock, *home_date, *home_today_min, *home_streak,
                *home_status, *home_batt, *home_sync_dot, *home_goal_bar;

// Pomodoro
static lv_obj_t *pomo_arc, *pomo_time_lbl, *pomo_phase_lbl, *pomo_preset_lbl,
                *pomo_start_btn, *pomo_preset_btn, *pomo_exam_sw,
                *pomo_subject_btn, *pomo_subject_lbl;
static lv_obj_t *pomo_start_label;

// Tasks
static lv_obj_t *task_list_obj, *task_summary;

// Cards
static lv_obj_t *card_face, *card_text, *card_counter, *card_yes_btn,
                *card_no_btn, *card_flip_btn;

// Analytics
static lv_obj_t *an_today, *an_sessions, *an_streak, *an_minutes_arc;

// Toast + Back
static lv_obj_t *toast_lbl;
static lv_obj_t *back_btn;

// Flow overlay (covers all tabs when flow active)
static lv_obj_t *flow_overlay = nullptr;
static lv_obj_t *flow_time_lbl = nullptr, *flow_remain_lbl = nullptr;

// Carousel page-indicator dots (S3_MiniOS-style)
static lv_obj_t *page_dots[5] = {0};

// =============================================================================
//  Toast
// =============================================================================
static uint32_t g_toast_hide_at = 0;
static void show_toast(const char *msg, uint32_t ms) {
    if (!toast_lbl) return;
    if (g_flow) return;     // [8] Flow Mode suppresses toasts
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
//  Pomodoro engine
// =============================================================================
static void pomo_start_phase(PomoPhase ph) {
    g_phase = ph;
    g_phase_t0 = millis();
    int mins = 0;
    if (ph == PH_WORK)  mins = PRESETS[g_preset_idx].work_min;
    if (ph == PH_BREAK) mins = PRESETS[g_preset_idx].break_min;
    g_phase_len_ms = (uint32_t)mins * 60UL * 1000UL;
    if (ph == PH_WORK)  show_toast("Focus started", 1200);
    if (ph == PH_BREAK) show_toast("Break", 1200);
}
static void pomo_stop() {
    g_phase = PH_IDLE;
    g_phase_len_ms = 0;
    g_flow = false;
}
static void pomo_complete_work() {
    g_total_sessions++;
    prefs_save();
    if (PRESETS[g_preset_idx].break_min > 0) pomo_start_phase(PH_BREAK);
    else { g_phase = PH_DONE; show_toast("Session complete", 1500); }
}
static void pomo_complete_break() {
    g_phase = PH_DONE;
    show_toast("Break done", 1200);
}

static void update_pomo_ui() {
    // Arc + time
    uint32_t remain_ms = 0;
    int pct = 0;
    const char *phase_txt = "IDLE";
    uint32_t arc_col = S_DIM;
    if (g_phase == PH_WORK || g_phase == PH_BREAK) {
        uint32_t elapsed = millis() - g_phase_t0;
        if (elapsed > g_phase_len_ms) elapsed = g_phase_len_ms;
        remain_ms = g_phase_len_ms - elapsed;
        pct = g_phase_len_ms ? (int)((elapsed * 100UL) / g_phase_len_ms) : 0;
        phase_txt = (g_phase == PH_WORK) ? "FOCUS" : "BREAK";
        arc_col   = (g_phase == PH_WORK) ? S_ACCENT : S_OK;
    } else if (g_phase == PH_DONE) {
        phase_txt = "DONE";
        arc_col = S_OK;
        pct = 100;
    }
    lv_arc_set_value(pomo_arc, pct);
    lv_obj_set_style_arc_color(pomo_arc, lv_color_hex(arc_col), LV_PART_INDICATOR);
    uint32_t sec = remain_ms / 1000;
    char t[16]; snprintf(t, sizeof(t), "%02lu:%02lu", sec / 60, sec % 60);
    if (g_phase == PH_IDLE) {
        snprintf(t, sizeof(t), "%02d:00", PRESETS[g_preset_idx].work_min);
    }
    lv_label_set_text(pomo_time_lbl, t);
    lv_label_set_text(pomo_phase_lbl, phase_txt);
    char p[40];
    snprintf(p, sizeof(p), "%s · %d / %d", PRESETS[g_preset_idx].name,
             PRESETS[g_preset_idx].work_min, PRESETS[g_preset_idx].break_min);
    lv_label_set_text(pomo_preset_lbl, p);
    lv_label_set_text(pomo_start_label,
        (g_phase == PH_WORK || g_phase == PH_BREAK) ? "STOP" : "START");
}

static void pomo_tick() {
    if (g_phase != PH_WORK && g_phase != PH_BREAK) return;
    uint32_t elapsed = millis() - g_phase_t0;
    // accumulate today_seconds during WORK phase only
    static uint32_t last_accum = 0;
    if (g_phase == PH_WORK) {
        uint32_t now = millis();
        if (last_accum == 0) last_accum = now;
        if (now - last_accum >= 1000) {
            g_today_seconds += (now - last_accum) / 1000;
            g_subject_seconds[g_subject_idx] += (now - last_accum) / 1000;
            last_accum = now;
            // streak: if first session today, bump streak
            struct tm tm;
            if (getLocalTime(&tm, 5)) {
                uint16_t yd = tm.tm_yday;
                if (g_streak_yday != yd) {
                    if (g_streak_yday == 0xFFFF || (uint16_t)((g_streak_yday + 1) % 366) == yd) {
                        g_streak_days++;
                    } else {
                        g_streak_days = 1;
                    }
                    g_streak_yday = yd;
                    prefs_save();
                }
            }
            if (g_today_seconds % 30 == 0) prefs_save();
        }
    } else last_accum = 0;

    if (elapsed >= g_phase_len_ms) {
        if (g_phase == PH_WORK)  pomo_complete_work();
        else                     pomo_complete_break();
    }

    // [8] Flow Mode trigger
    if (g_phase == PH_WORK && (elapsed / 1000) >= FLOW_THRESHOLD_S && !g_flow) {
        g_flow = true;
        if (flow_overlay) lv_obj_clear_flag(flow_overlay, LV_OBJ_FLAG_HIDDEN);
        co5300_set_brightness(60);
    }
    if (g_phase != PH_WORK && g_flow) {
        g_flow = false;
        if (flow_overlay) lv_obj_add_flag(flow_overlay, LV_OBJ_FLAG_HIDDEN);
        co5300_set_brightness(180);
    }
}

// =============================================================================
//  Event callbacks
// =============================================================================
static uint32_t g_back_press_t = 0;
static void back_btn_cb(lv_event_t *e) {
    lv_event_code_t c = lv_event_get_code(e);
    if (c == LV_EVENT_PRESSED)         g_back_press_t = millis();
    else if (c == LV_EVENT_LONG_PRESSED) return_to_launcher();
    else if (c == LV_EVENT_RELEASED) {
        uint32_t held = g_back_press_t ? (millis() - g_back_press_t) : 0;
        g_back_press_t = 0;
        if (held > 0 && held < 600) show_toast("Hold to exit to Launcher", 1500);
    }
}

static void pomo_start_cb(lv_event_t *) {
    if (g_phase == PH_WORK || g_phase == PH_BREAK) pomo_stop();
    else pomo_start_phase(PH_WORK);
    update_pomo_ui();
}
static void pomo_preset_cb(lv_event_t *) {
    g_preset_idx = (g_preset_idx + 1) % NUM_PRESETS;
    prefs_save();
    update_pomo_ui();
    char m[40]; snprintf(m, sizeof(m), "Preset: %s", PRESETS[g_preset_idx].name);
    show_toast(m, 1200);
}
static void pomo_subject_cb(lv_event_t *) {
    g_subject_idx = (g_subject_idx + 1) % NUM_SUBJECTS;
    prefs_save();
    lv_label_set_text(pomo_subject_lbl, SUBJECTS[g_subject_idx]);
    char m[40]; snprintf(m, sizeof(m), "Subject: %s", SUBJECTS[g_subject_idx]);
    show_toast(m, 1200);
}
static void pomo_exam_cb(lv_event_t *e) {
    g_exam_mode = lv_obj_has_state((lv_obj_t*)lv_event_get_target(e), LV_STATE_CHECKED);
    if (g_exam_mode) {
        // Lock to EXAM preset, hide tab bar (use lv_tabview no bar already)
        for (int i = 0; i < NUM_PRESETS; i++) if (strcmp(PRESETS[i].name, "EXAM") == 0) g_preset_idx = i;
        update_pomo_ui();
        show_toast("EXAM MODE LOCKED", 1500);
    } else {
        show_toast("Exam mode off", 1200);
    }
}

static void task_row_cb(lv_event_t *e) {
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    if (idx < 0 || idx >= g_task_count) return;
    g_tasks[idx].done = !g_tasks[idx].done;
    if (g_tasks[idx].done) g_task_done_bits |= (1UL << idx);
    else                   g_task_done_bits &= ~(1UL << idx);
    prefs_save();
    // Refresh label color
    lv_obj_t *row = (lv_obj_t *)lv_event_get_target(e);
    lv_obj_t *lbl = lv_obj_get_child(row, 1);
    lv_obj_set_style_text_color(lbl,
        lv_color_hex(g_tasks[idx].done ? S_DIM : S_TXT), 0);
    lv_obj_t *dot = lv_obj_get_child(row, 0);
    lv_obj_set_style_bg_color(dot,
        lv_color_hex(g_tasks[idx].done ? S_OK : S_BORDER), 0);
    // Summary
    if (task_summary) {
        int done = 0; for (int i = 0; i < g_task_count; i++) if (g_tasks[i].done) done++;
        char s[48]; snprintf(s, sizeof(s), "%d / %d complete", done, g_task_count);
        lv_label_set_text(task_summary, s);
    }
}

static void card_yes_cb(lv_event_t *) {
    if (!g_card_count) return;
    g_card_remembered++;
    g_card_idx = (g_card_idx + 1) % g_card_count;
    g_card_flipped = false;
}
static void card_no_cb(lv_event_t *) {
    if (!g_card_count) return;
    g_card_review++;
    g_card_idx = (g_card_idx + 1) % g_card_count;
    g_card_flipped = false;
}
static void card_flip_cb(lv_event_t *) { g_card_flipped = !g_card_flipped; }

static void note_btn_cb(lv_event_t *) {
    SD_MMC.mkdir("/WATCH/study");
    File f = SD_MMC.open("/WATCH/study/notes.txt", FILE_APPEND);
    if (f) {
        time_t t; time(&t);
        struct tm tm; localtime_r(&t, &tm);
        char ts[32]; strftime(ts, sizeof(ts), "[%Y-%m-%d %H:%M] ", &tm);
        f.print(ts); f.println("(quick note)");
        f.close();
        show_toast("Note appended", 1200);
    } else show_toast("SD write failed", 1500);
}

// =============================================================================
//  VOICE MEMO -- ES8311 codec + I2S RX -> WAV on SD
//  (tap-and-hold "Voice Memo" button on Home tab)
//  Pin map matches S3_MiniOS_206 ES8311 / I2S MCLK/BCLK/LRCK/DOUT wiring.
// =============================================================================
#define I2S_MCLK 16
#define I2S_BCLK 9
#define I2S_LRCK 45
#define I2S_DIN  17     // mic data (ES8311 ADC -> ESP32)
#define I2S_DOUT 8      // unused for record
#define ES8311_ADDR 0x18

static bool g_es8311_inited = false;
static bool g_recording     = false;
static File g_rec_file;
static uint32_t g_rec_bytes = 0;

static bool es8311_w(uint8_t r, uint8_t v) {
    Wire.beginTransmission(ES8311_ADDR);
    Wire.write(r); Wire.write(v);
    return Wire.endTransmission() == 0;
}

static bool es8311_init() {
    if (g_es8311_inited) return true;
    // Minimal ES8311 ADC-only init @ 16 kHz / 16-bit mono.
    // Sequence cribbed from Espressif esp-adf and S3_MiniOS_206/es8311.c.
    bool ok = true;
    ok &= es8311_w(0x00, 0x1F); // reset
    delay(20);
    ok &= es8311_w(0x00, 0x00);
    ok &= es8311_w(0x01, 0x30); // power up clock manager
    ok &= es8311_w(0x02, 0x10); // MCLK divider
    ok &= es8311_w(0x03, 0x10); // ADC FS = 16 kHz from 12.288 MHz MCLK
    ok &= es8311_w(0x16, 0x24); // ADC ramp / power up
    ok &= es8311_w(0x04, 0x10);
    ok &= es8311_w(0x05, 0x00);
    ok &= es8311_w(0x06, 0x03);
    ok &= es8311_w(0x07, 0x00);
    ok &= es8311_w(0x08, 0xFF);
    ok &= es8311_w(0x09, 0x0C); // I2S 16-bit
    ok &= es8311_w(0x0A, 0x0C);
    ok &= es8311_w(0x0D, 0x10); // power on
    ok &= es8311_w(0x0E, 0x02);
    ok &= es8311_w(0x12, 0x00); // enable ADC
    ok &= es8311_w(0x13, 0x10);
    ok &= es8311_w(0x14, 0x1A); // analog input -> ADC mic boost
    ok &= es8311_w(0x15, 0x40);
    ok &= es8311_w(0x17, 0xC8); // ADC volume = 0 dB
    ok &= es8311_w(0x1C, 0x6A); // ADC ALC enable + target
    g_es8311_inited = ok;
    return ok;
}

static bool i2s_rx_init() {
    i2s_config_t cfg = {};
    cfg.mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX);
    cfg.sample_rate = 16000;
    cfg.bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT;
    cfg.channel_format = I2S_CHANNEL_FMT_ONLY_LEFT;
    cfg.communication_format = I2S_COMM_FORMAT_STAND_I2S;
    cfg.intr_alloc_flags = ESP_INTR_FLAG_LEVEL1;
    cfg.dma_buf_count = 4;
    cfg.dma_buf_len = 512;
    cfg.use_apll = true;
    cfg.tx_desc_auto_clear = false;
    cfg.fixed_mclk = 12288000;
    cfg.mclk_multiple = I2S_MCLK_MULTIPLE_384;
    if (i2s_driver_install(I2S_NUM_0, &cfg, 0, NULL) != ESP_OK) return false;
    i2s_pin_config_t pin = {};
    pin.mck_io_num   = I2S_MCLK;
    pin.bck_io_num   = I2S_BCLK;
    pin.ws_io_num    = I2S_LRCK;
    pin.data_in_num  = I2S_DIN;
    pin.data_out_num = I2S_PIN_NO_CHANGE;
    return i2s_set_pin(I2S_NUM_0, &pin) == ESP_OK;
}

static void wav_write_header(File &f, uint32_t data_bytes) {
    uint32_t sr = 16000, br = sr * 2;
    uint8_t h[44] = {
        'R','I','F','F', 0,0,0,0, 'W','A','V','E',
        'f','m','t',' ', 16,0,0,0, 1,0, 1,0,
        (uint8_t)(sr),(uint8_t)(sr>>8),(uint8_t)(sr>>16),(uint8_t)(sr>>24),
        (uint8_t)(br),(uint8_t)(br>>8),(uint8_t)(br>>16),(uint8_t)(br>>24),
        2,0, 16,0,
        'd','a','t','a', 0,0,0,0
    };
    uint32_t riff = 36 + data_bytes;
    h[4]=riff; h[5]=riff>>8; h[6]=riff>>16; h[7]=riff>>24;
    h[40]=data_bytes; h[41]=data_bytes>>8; h[42]=data_bytes>>16; h[43]=data_bytes>>24;
    f.seek(0); f.write(h, 44);
}

static void voice_start() {
    if (g_recording) return;
    SD_MMC.mkdir("/WATCH/study/voice");
    if (!es8311_init()) { show_toast("Codec init failed", 1500); return; }
    if (!i2s_rx_init()) { show_toast("I2S init failed", 1500); return; }
    time_t t; time(&t);
    struct tm tm; localtime_r(&t, &tm);
    char path[64];
    strftime(path, sizeof(path), "/WATCH/study/voice/%Y%m%d_%H%M%S.wav", &tm);
    g_rec_file = SD_MMC.open(path, FILE_WRITE);
    if (!g_rec_file) { show_toast("SD open failed", 1500); return; }
    uint8_t blank[44] = {0}; g_rec_file.write(blank, 44);   // placeholder header
    g_rec_bytes = 0;
    g_recording = true;
    show_toast("● REC voice memo", 1200);
}
static void voice_stop() {
    if (!g_recording) return;
    g_recording = false;
    i2s_driver_uninstall(I2S_NUM_0);
    wav_write_header(g_rec_file, g_rec_bytes);
    g_rec_file.close();
    char m[40]; snprintf(m, sizeof(m), "Saved %lu KB voice memo", g_rec_bytes / 1024);
    show_toast(m, 1800);
}
static void voice_tick() {
    if (!g_recording) return;
    uint8_t buf[1024]; size_t n = 0;
    if (i2s_read(I2S_NUM_0, buf, sizeof(buf), &n, 0) == ESP_OK && n > 0) {
        g_rec_file.write(buf, n);
        g_rec_bytes += n;
        if (g_rec_bytes > 16000UL * 2 * 60) voice_stop();   // hard cap 60 s
    }
}
static void voice_btn_cb(lv_event_t *e) {
    lv_event_code_t c = lv_event_get_code(e);
    if (c == LV_EVENT_PRESSED)  voice_start();
    if (c == LV_EVENT_RELEASED) voice_stop();
}

// =============================================================================
//  UI builders
// =============================================================================
static lv_obj_t *make_card(lv_obj_t *parent, int w, int h) {
    lv_obj_t *c = lv_obj_create(parent);
    lv_obj_remove_style_all(c);
    lv_obj_set_size(c, w, h);
    lv_obj_set_style_bg_color(c, lv_color_hex(S_SURFACE), 0);
    lv_obj_set_style_bg_opa(c, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(c, lv_color_hex(S_BORDER), 0);
    lv_obj_set_style_border_width(c, 1, 0);
    lv_obj_set_style_radius(c, 14, 0);
    lv_obj_set_style_pad_all(c, 12, 0);
    return c;
}

static void make_eyebrow(lv_obj_t *parent, const char *txt) {
    lv_obj_t *l = lv_label_create(parent);
    lv_label_set_text(l, txt);
    lv_obj_set_style_text_color(l, lv_color_hex(S_MUTE), 0);
    lv_obj_set_style_text_font(l, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_letter_space(l, 3, 0);
    lv_obj_align(l, LV_ALIGN_TOP_MID, 0, 14);
}

static void build_back_btn(lv_obj_t *parent) {
    back_btn = lv_btn_create(parent);
    lv_obj_set_size(back_btn, 220, 46);
    lv_obj_align(back_btn, LV_ALIGN_BOTTOM_MID, 0, -16);
    lv_obj_set_style_bg_color(back_btn, lv_color_hex(0x141414), 0);
    lv_obj_set_style_border_color(back_btn, lv_color_hex(S_CYAN), 0);
    lv_obj_set_style_border_width(back_btn, 1, 0);
    lv_obj_set_style_radius(back_btn, 23, 0);
    lv_obj_t *bl = lv_label_create(back_btn);
    lv_label_set_text(bl, LV_SYMBOL_LEFT "  Back to Launcher");
    lv_obj_set_style_text_color(bl, lv_color_hex(S_CYAN), 0);
    lv_obj_set_style_text_font(bl, &lv_font_montserrat_16, 0);
    lv_obj_center(bl);
    lv_obj_add_event_cb(back_btn, back_btn_cb, LV_EVENT_PRESSED,      NULL);
    lv_obj_add_event_cb(back_btn, back_btn_cb, LV_EVENT_LONG_PRESSED, NULL);
    lv_obj_add_event_cb(back_btn, back_btn_cb, LV_EVENT_RELEASED,     NULL);
}

static void build_home_tab() {
    make_eyebrow(tab_home, "STUDY // OS");

    // Sync dot top-right (green if synced, red if not)
    home_sync_dot = lv_obj_create(tab_home);
    lv_obj_remove_style_all(home_sync_dot);
    lv_obj_set_size(home_sync_dot, 10, 10);
    lv_obj_set_style_radius(home_sync_dot, 5, 0);
    lv_obj_set_style_bg_opa(home_sync_dot, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(home_sync_dot, lv_color_hex(g_wifi_synced ? S_OK : 0xFF4040), 0);
    lv_obj_align(home_sync_dot, LV_ALIGN_TOP_RIGHT, -30, 18);

    home_batt = lv_label_create(tab_home);
    lv_label_set_text(home_batt, "--%");
    lv_obj_set_style_text_color(home_batt, lv_color_hex(S_MUTE), 0);
    lv_obj_set_style_text_font(home_batt, &lv_font_montserrat_14, 0);
    lv_obj_align(home_batt, LV_ALIGN_TOP_LEFT, 30, 18);

    home_clock = lv_label_create(tab_home);
    lv_label_set_text(home_clock, "--:--");
    lv_obj_set_style_text_color(home_clock, lv_color_hex(S_TXT), 0);
    lv_obj_set_style_text_font(home_clock, &lv_font_montserrat_48, 0);
    lv_obj_align(home_clock, LV_ALIGN_TOP_MID, 0, 50);

    home_date = lv_label_create(tab_home);
    lv_label_set_text(home_date, "");
    lv_obj_set_style_text_color(home_date, lv_color_hex(S_MUTE), 0);
    lv_obj_set_style_text_font(home_date, &lv_font_montserrat_14, 0);
    lv_obj_align(home_date, LV_ALIGN_TOP_MID, 0, 108);

    // Today minutes card
    lv_obj_t *c1 = make_card(tab_home, 175, 100);
    lv_obj_align(c1, LV_ALIGN_TOP_LEFT, 20, 150);
    lv_obj_t *l1 = lv_label_create(c1);
    lv_label_set_text(l1, "TODAY");
    lv_obj_set_style_text_color(l1, lv_color_hex(S_MUTE), 0);
    lv_obj_set_style_text_font(l1, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_letter_space(l1, 3, 0);
    lv_obj_align(l1, LV_ALIGN_TOP_LEFT, 0, 0);
    home_today_min = lv_label_create(c1);
    lv_label_set_text(home_today_min, "0m");
    lv_obj_set_style_text_color(home_today_min, lv_color_hex(S_ACCENT), 0);
    lv_obj_set_style_text_font(home_today_min, &lv_font_montserrat_28, 0);
    lv_obj_align(home_today_min, LV_ALIGN_BOTTOM_LEFT, 0, 0);

    // Streak card
    lv_obj_t *c2 = make_card(tab_home, 175, 100);
    lv_obj_align(c2, LV_ALIGN_TOP_LEFT, 215, 150);
    lv_obj_t *l2 = lv_label_create(c2);
    lv_label_set_text(l2, "STREAK");
    lv_obj_set_style_text_color(l2, lv_color_hex(S_MUTE), 0);
    lv_obj_set_style_text_font(l2, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_letter_space(l2, 3, 0);
    lv_obj_align(l2, LV_ALIGN_TOP_LEFT, 0, 0);
    home_streak = lv_label_create(c2);
    lv_label_set_text(home_streak, "0 d");
    lv_obj_set_style_text_color(home_streak, lv_color_hex(S_OK), 0);
    lv_obj_set_style_text_font(home_streak, &lv_font_montserrat_28, 0);
    lv_obj_align(home_streak, LV_ALIGN_BOTTOM_LEFT, 0, 0);

    home_status = lv_label_create(tab_home);
    lv_label_set_text(home_status, "swipe < > for POMODORO / TASKS / CARDS / STATS");
    lv_obj_set_style_text_color(home_status, lv_color_hex(S_DIM), 0);
    lv_obj_set_style_text_font(home_status, &lv_font_montserrat_12, 0);
    lv_obj_align(home_status, LV_ALIGN_TOP_MID, 0, 270);

    // Daily-goal progress bar (today's minutes / 240 min daily goal)
    home_goal_bar = lv_bar_create(tab_home);
    lv_obj_set_size(home_goal_bar, 370, 6);
    lv_obj_align(home_goal_bar, LV_ALIGN_TOP_MID, 0, 296);
    lv_bar_set_range(home_goal_bar, 0, 240);
    lv_bar_set_value(home_goal_bar, 0, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(home_goal_bar, lv_color_hex(S_BORDER), LV_PART_MAIN);
    lv_obj_set_style_bg_color(home_goal_bar, lv_color_hex(S_ACCENT), LV_PART_INDICATOR);
    lv_obj_set_style_radius(home_goal_bar, 3, LV_PART_MAIN);
    lv_obj_set_style_radius(home_goal_bar, 3, LV_PART_INDICATOR);

    // Quick note button
    lv_obj_t *note = lv_btn_create(tab_home);
    lv_obj_set_size(note, 220, 42);
    lv_obj_align(note, LV_ALIGN_TOP_MID, 0, 310);
    lv_obj_set_style_bg_color(note, lv_color_hex(S_SURFACE), 0);
    lv_obj_set_style_border_color(note, lv_color_hex(S_ACCENT), 0);
    lv_obj_set_style_border_width(note, 1, 0);
    lv_obj_set_style_radius(note, 21, 0);
    lv_obj_t *nl = lv_label_create(note);
    lv_label_set_text(nl, LV_SYMBOL_EDIT "  Quick Note");
    lv_obj_set_style_text_color(nl, lv_color_hex(S_ACCENT), 0);
    lv_obj_set_style_text_font(nl, &lv_font_montserrat_14, 0);
    lv_obj_center(nl);
    lv_obj_add_event_cb(note, note_btn_cb, LV_EVENT_CLICKED, NULL);

    // Voice Memo button (tap-and-hold to record to WAV on SD)
    lv_obj_t *voice = lv_btn_create(tab_home);
    lv_obj_set_size(voice, 220, 42);
    lv_obj_align(voice, LV_ALIGN_TOP_MID, 0, 358);
    lv_obj_set_style_bg_color(voice, lv_color_hex(S_SURFACE), 0);
    lv_obj_set_style_border_color(voice, lv_color_hex(S_WARN), 0);
    lv_obj_set_style_border_width(voice, 1, 0);
    lv_obj_set_style_radius(voice, 21, 0);
    lv_obj_t *vl = lv_label_create(voice);
    lv_label_set_text(vl, LV_SYMBOL_AUDIO "  Hold to record");
    lv_obj_set_style_text_color(vl, lv_color_hex(S_WARN), 0);
    lv_obj_set_style_text_font(vl, &lv_font_montserrat_14, 0);
    lv_obj_center(vl);
    lv_obj_add_event_cb(voice, voice_btn_cb, LV_EVENT_PRESSED,  NULL);
    lv_obj_add_event_cb(voice, voice_btn_cb, LV_EVENT_RELEASED, NULL);

    build_back_btn(tab_home);
}

static void build_pomo_tab() {
    make_eyebrow(tab_pomo, "POMODORO");

    pomo_arc = lv_arc_create(tab_pomo);
    lv_obj_set_size(pomo_arc, 300, 300);
    lv_obj_align(pomo_arc, LV_ALIGN_TOP_MID, 0, 50);
    lv_arc_set_range(pomo_arc, 0, 100);
    lv_arc_set_value(pomo_arc, 0);
    lv_arc_set_bg_angles(pomo_arc, 135, 45);
    lv_obj_remove_style(pomo_arc, NULL, LV_PART_KNOB);
    lv_obj_clear_flag(pomo_arc, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_arc_color(pomo_arc, lv_color_hex(S_BORDER), LV_PART_MAIN);
    lv_obj_set_style_arc_color(pomo_arc, lv_color_hex(S_ACCENT), LV_PART_INDICATOR);
    lv_obj_set_style_arc_width(pomo_arc, 6, LV_PART_MAIN);
    lv_obj_set_style_arc_width(pomo_arc, 6, LV_PART_INDICATOR);

    pomo_phase_lbl = lv_label_create(tab_pomo);
    lv_label_set_text(pomo_phase_lbl, "IDLE");
    lv_obj_set_style_text_color(pomo_phase_lbl, lv_color_hex(S_MUTE), 0);
    lv_obj_set_style_text_font(pomo_phase_lbl, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_letter_space(pomo_phase_lbl, 4, 0);
    lv_obj_align(pomo_phase_lbl, LV_ALIGN_TOP_MID, 0, 130);

    pomo_time_lbl = lv_label_create(tab_pomo);
    lv_label_set_text(pomo_time_lbl, "25:00");
    lv_obj_set_style_text_color(pomo_time_lbl, lv_color_hex(S_TXT), 0);
    lv_obj_set_style_text_font(pomo_time_lbl, &lv_font_montserrat_48, 0);
    lv_obj_align(pomo_time_lbl, LV_ALIGN_TOP_MID, 0, 160);

    pomo_preset_lbl = lv_label_create(tab_pomo);
    lv_label_set_text(pomo_preset_lbl, "FOCUS · 25 / 5");
    lv_obj_set_style_text_color(pomo_preset_lbl, lv_color_hex(S_DIM), 0);
    lv_obj_set_style_text_font(pomo_preset_lbl, &lv_font_montserrat_14, 0);
    lv_obj_align(pomo_preset_lbl, LV_ALIGN_TOP_MID, 0, 222);

    // Start button
    pomo_start_btn = lv_btn_create(tab_pomo);
    lv_obj_set_size(pomo_start_btn, 175, 48);
    lv_obj_align(pomo_start_btn, LV_ALIGN_TOP_LEFT, 20, 256);
    lv_obj_set_style_bg_color(pomo_start_btn, lv_color_hex(S_ACCENT), 0);
    lv_obj_set_style_radius(pomo_start_btn, 24, 0);
    pomo_start_label = lv_label_create(pomo_start_btn);
    lv_label_set_text(pomo_start_label, "START");
    lv_obj_set_style_text_color(pomo_start_label, lv_color_hex(0x000000), 0);
    lv_obj_set_style_text_font(pomo_start_label, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_letter_space(pomo_start_label, 3, 0);
    lv_obj_center(pomo_start_label);
    lv_obj_add_event_cb(pomo_start_btn, pomo_start_cb, LV_EVENT_CLICKED, NULL);

    // Preset cycle button
    pomo_preset_btn = lv_btn_create(tab_pomo);
    lv_obj_set_size(pomo_preset_btn, 175, 48);
    lv_obj_align(pomo_preset_btn, LV_ALIGN_TOP_LEFT, 215, 256);
    lv_obj_set_style_bg_color(pomo_preset_btn, lv_color_hex(S_SURFACE), 0);
    lv_obj_set_style_border_color(pomo_preset_btn, lv_color_hex(S_ACCENT), 0);
    lv_obj_set_style_border_width(pomo_preset_btn, 1, 0);
    lv_obj_set_style_radius(pomo_preset_btn, 24, 0);
    lv_obj_t *pl = lv_label_create(pomo_preset_btn);
    lv_label_set_text(pl, "PRESET");
    lv_obj_set_style_text_color(pl, lv_color_hex(S_ACCENT), 0);
    lv_obj_set_style_text_font(pl, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_letter_space(pl, 3, 0);
    lv_obj_center(pl);
    lv_obj_add_event_cb(pomo_preset_btn, pomo_preset_cb, LV_EVENT_CLICKED, NULL);

    // Exam mode switch
    lv_obj_t *exam_row = lv_obj_create(tab_pomo);
    lv_obj_remove_style_all(exam_row);
    lv_obj_set_size(exam_row, 370, 42);
    lv_obj_align(exam_row, LV_ALIGN_TOP_MID, 0, 316);
    lv_obj_set_style_bg_color(exam_row, lv_color_hex(S_SURFACE), 0);
    lv_obj_set_style_bg_opa(exam_row, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(exam_row, lv_color_hex(S_BORDER), 0);
    lv_obj_set_style_border_width(exam_row, 1, 0);
    lv_obj_set_style_radius(exam_row, 12, 0);
    lv_obj_set_style_pad_all(exam_row, 10, 0);
    lv_obj_t *exam_lbl = lv_label_create(exam_row);
    lv_label_set_text(exam_lbl, "EXAM MODE (90 min, no break)");
    lv_obj_set_style_text_color(exam_lbl, lv_color_hex(S_TXT), 0);
    lv_obj_set_style_text_font(exam_lbl, &lv_font_montserrat_14, 0);
    lv_obj_align(exam_lbl, LV_ALIGN_LEFT_MID, 4, 0);
    pomo_exam_sw = lv_switch_create(exam_row);
    lv_obj_align(pomo_exam_sw, LV_ALIGN_RIGHT_MID, -4, 0);
    lv_obj_set_style_bg_color(pomo_exam_sw, lv_color_hex(S_ACCENT), LV_PART_INDICATOR | LV_STATE_CHECKED);
    lv_obj_add_event_cb(pomo_exam_sw, pomo_exam_cb, LV_EVENT_VALUE_CHANGED, NULL);

    // Subject tag button (cycles through SUBJECTS array)
    pomo_subject_btn = lv_btn_create(tab_pomo);
    lv_obj_set_size(pomo_subject_btn, 370, 38);
    lv_obj_align(pomo_subject_btn, LV_ALIGN_TOP_MID, 0, 364);
    lv_obj_set_style_bg_color(pomo_subject_btn, lv_color_hex(S_SURFACE), 0);
    lv_obj_set_style_border_color(pomo_subject_btn, lv_color_hex(S_WARN), 0);
    lv_obj_set_style_border_width(pomo_subject_btn, 1, 0);
    lv_obj_set_style_radius(pomo_subject_btn, 12, 0);
    pomo_subject_lbl = lv_label_create(pomo_subject_btn);
    lv_label_set_text(pomo_subject_lbl, SUBJECTS[g_subject_idx]);
    lv_obj_set_style_text_color(pomo_subject_lbl, lv_color_hex(S_WARN), 0);
    lv_obj_set_style_text_font(pomo_subject_lbl, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_letter_space(pomo_subject_lbl, 4, 0);
    lv_obj_center(pomo_subject_lbl);
    lv_obj_add_event_cb(pomo_subject_btn, pomo_subject_cb, LV_EVENT_CLICKED, NULL);

    build_back_btn(tab_pomo);
}

static void build_task_tab() {
    make_eyebrow(tab_task, "TASKS // TODAY");

    task_summary = lv_label_create(tab_task);
    lv_label_set_text(task_summary, "0 / 0 complete");
    lv_obj_set_style_text_color(task_summary, lv_color_hex(S_MUTE), 0);
    lv_obj_set_style_text_font(task_summary, &lv_font_montserrat_14, 0);
    lv_obj_align(task_summary, LV_ALIGN_TOP_MID, 0, 40);

    task_list_obj = lv_obj_create(tab_task);
    lv_obj_set_size(task_list_obj, 370, 320);
    lv_obj_align(task_list_obj, LV_ALIGN_TOP_MID, 0, 70);
    lv_obj_set_style_bg_color(task_list_obj, lv_color_hex(S_BG), 0);
    lv_obj_set_style_border_width(task_list_obj, 0, 0);
    lv_obj_set_style_pad_all(task_list_obj, 0, 0);
    lv_obj_set_flex_flow(task_list_obj, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(task_list_obj, 8, 0);

    if (g_task_count == 0) {
        lv_obj_t *empty = lv_label_create(task_list_obj);
        lv_label_set_text(empty,
            "No tasks loaded.\nCreate /WATCH/study/tasks.txt\nOne task per line.");
        lv_obj_set_style_text_color(empty, lv_color_hex(S_DIM), 0);
        lv_obj_set_style_text_font(empty, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_align(empty, LV_TEXT_ALIGN_CENTER, 0);
    } else {
        int done = 0;
        for (int i = 0; i < g_task_count; i++) {
            lv_obj_t *row = lv_obj_create(task_list_obj);
            lv_obj_remove_style_all(row);
            lv_obj_set_size(row, 360, 44);
            lv_obj_set_style_bg_color(row, lv_color_hex(S_SURFACE), 0);
            lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
            lv_obj_set_style_border_color(row, lv_color_hex(S_BORDER), 0);
            lv_obj_set_style_border_width(row, 1, 0);
            lv_obj_set_style_radius(row, 10, 0);
            lv_obj_set_style_pad_all(row, 10, 0);

            lv_obj_t *dot = lv_obj_create(row);
            lv_obj_remove_style_all(dot);
            lv_obj_set_size(dot, 12, 12);
            lv_obj_set_style_radius(dot, 6, 0);
            lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
            lv_obj_set_style_bg_color(dot,
                lv_color_hex(g_tasks[i].done ? S_OK : S_BORDER), 0);
            lv_obj_align(dot, LV_ALIGN_LEFT_MID, 0, 0);

            lv_obj_t *lbl = lv_label_create(row);
            lv_label_set_text(lbl, g_tasks[i].text);
            lv_obj_set_style_text_color(lbl,
                lv_color_hex(g_tasks[i].done ? S_DIM : S_TXT), 0);
            lv_obj_set_style_text_font(lbl, &lv_font_montserrat_14, 0);
            lv_obj_align(lbl, LV_ALIGN_LEFT_MID, 22, 0);

            lv_obj_add_event_cb(row, task_row_cb, LV_EVENT_CLICKED,
                                (void *)(intptr_t)i);
            if (g_tasks[i].done) done++;
        }
        char s[48]; snprintf(s, sizeof(s), "%d / %d complete", done, g_task_count);
        lv_label_set_text(task_summary, s);
    }

    build_back_btn(tab_task);
}

static void build_cards_tab() {
    make_eyebrow(tab_cards, "FLASHCARDS");

    card_face = make_card(tab_cards, 370, 280);
    lv_obj_align(card_face, LV_ALIGN_TOP_MID, 0, 50);
    lv_obj_set_style_pad_all(card_face, 24, 0);

    card_text = lv_label_create(card_face);
    lv_label_set_text(card_text,
        g_card_count == 0 ? "No cards loaded.\n/WATCH/study/cards/*.txt\nformat: front|back\nor: hanzi|pinyin|english"
                          : g_cards[0].front);
    lv_obj_set_style_text_color(card_text, lv_color_hex(S_TXT), 0);
    // SimSun CJK font handles Chinese hanzi AND Latin/pinyin in one glyph set.
    lv_obj_set_style_text_font(card_text, &lv_font_simsun_16_cjk, 0);
    lv_obj_set_style_text_align(card_text, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(card_text, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(card_text, 320);
    lv_obj_center(card_text);

    card_counter = lv_label_create(tab_cards);
    char cc[24];
    snprintf(cc, sizeof(cc), "%d / %d",
             g_card_count ? 1 : 0, g_card_count);
    lv_label_set_text(card_counter, cc);
    lv_obj_set_style_text_color(card_counter, lv_color_hex(S_DIM), 0);
    lv_obj_set_style_text_font(card_counter, &lv_font_montserrat_14, 0);
    lv_obj_align(card_counter, LV_ALIGN_TOP_MID, 0, 340);

    // No / Flip / Yes
    card_no_btn = lv_btn_create(tab_cards);
    lv_obj_set_size(card_no_btn, 110, 44);
    lv_obj_align(card_no_btn, LV_ALIGN_TOP_LEFT, 20, 370);
    lv_obj_set_style_bg_color(card_no_btn, lv_color_hex(S_SURFACE), 0);
    lv_obj_set_style_border_color(card_no_btn, lv_color_hex(S_WARN), 0);
    lv_obj_set_style_border_width(card_no_btn, 1, 0);
    lv_obj_set_style_radius(card_no_btn, 22, 0);
    lv_obj_t *nl = lv_label_create(card_no_btn);
    lv_label_set_text(nl, "REVIEW");
    lv_obj_set_style_text_color(nl, lv_color_hex(S_WARN), 0);
    lv_obj_set_style_text_font(nl, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_letter_space(nl, 2, 0);
    lv_obj_center(nl);
    lv_obj_add_event_cb(card_no_btn, card_no_cb, LV_EVENT_CLICKED, NULL);

    card_flip_btn = lv_btn_create(tab_cards);
    lv_obj_set_size(card_flip_btn, 130, 44);
    lv_obj_align(card_flip_btn, LV_ALIGN_TOP_MID, 0, 370);
    lv_obj_set_style_bg_color(card_flip_btn, lv_color_hex(S_ACCENT), 0);
    lv_obj_set_style_radius(card_flip_btn, 22, 0);
    lv_obj_t *fl = lv_label_create(card_flip_btn);
    lv_label_set_text(fl, "FLIP");
    lv_obj_set_style_text_color(fl, lv_color_hex(0x000000), 0);
    lv_obj_set_style_text_font(fl, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_letter_space(fl, 3, 0);
    lv_obj_center(fl);
    lv_obj_add_event_cb(card_flip_btn, card_flip_cb, LV_EVENT_CLICKED, NULL);

    card_yes_btn = lv_btn_create(tab_cards);
    lv_obj_set_size(card_yes_btn, 110, 44);
    lv_obj_align(card_yes_btn, LV_ALIGN_TOP_LEFT, 260, 370);
    lv_obj_set_style_bg_color(card_yes_btn, lv_color_hex(S_SURFACE), 0);
    lv_obj_set_style_border_color(card_yes_btn, lv_color_hex(S_OK), 0);
    lv_obj_set_style_border_width(card_yes_btn, 1, 0);
    lv_obj_set_style_radius(card_yes_btn, 22, 0);
    lv_obj_t *yl = lv_label_create(card_yes_btn);
    lv_label_set_text(yl, "GOT IT");
    lv_obj_set_style_text_color(yl, lv_color_hex(S_OK), 0);
    lv_obj_set_style_text_font(yl, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_letter_space(yl, 2, 0);
    lv_obj_center(yl);
    lv_obj_add_event_cb(card_yes_btn, card_yes_cb, LV_EVENT_CLICKED, NULL);

    build_back_btn(tab_cards);
}

static void build_analytics_tab() {
    make_eyebrow(tab_analytics, "FOCUS // ANALYTICS");

    // Today minutes (big)
    an_today = lv_label_create(tab_analytics);
    lv_label_set_text(an_today, "0 min");
    lv_obj_set_style_text_color(an_today, lv_color_hex(S_ACCENT), 0);
    lv_obj_set_style_text_font(an_today, &lv_font_montserrat_48, 0);
    lv_obj_align(an_today, LV_ALIGN_TOP_MID, 0, 60);

    lv_obj_t *l = lv_label_create(tab_analytics);
    lv_label_set_text(l, "TODAY");
    lv_obj_set_style_text_color(l, lv_color_hex(S_MUTE), 0);
    lv_obj_set_style_text_font(l, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_letter_space(l, 3, 0);
    lv_obj_align(l, LV_ALIGN_TOP_MID, 0, 130);

    // arc showing progress to daily goal (4 hours = 240 min)
    an_minutes_arc = lv_arc_create(tab_analytics);
    lv_obj_set_size(an_minutes_arc, 220, 220);
    lv_obj_align(an_minutes_arc, LV_ALIGN_TOP_MID, 0, 0);
    lv_arc_set_range(an_minutes_arc, 0, 240);
    lv_arc_set_value(an_minutes_arc, 0);
    lv_arc_set_bg_angles(an_minutes_arc, 135, 45);
    lv_obj_remove_style(an_minutes_arc, NULL, LV_PART_KNOB);
    lv_obj_clear_flag(an_minutes_arc, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_arc_color(an_minutes_arc, lv_color_hex(S_BORDER), LV_PART_MAIN);
    lv_obj_set_style_arc_color(an_minutes_arc, lv_color_hex(S_ACCENT), LV_PART_INDICATOR);
    lv_obj_set_style_arc_width(an_minutes_arc, 4, LV_PART_MAIN);
    lv_obj_set_style_arc_width(an_minutes_arc, 4, LV_PART_INDICATOR);

    // Streak + sessions
    lv_obj_t *c1 = make_card(tab_analytics, 175, 80);
    lv_obj_align(c1, LV_ALIGN_TOP_LEFT, 20, 250);
    lv_obj_t *cl1 = lv_label_create(c1);
    lv_label_set_text(cl1, "STREAK");
    lv_obj_set_style_text_color(cl1, lv_color_hex(S_MUTE), 0);
    lv_obj_set_style_text_font(cl1, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_letter_space(cl1, 3, 0);
    lv_obj_align(cl1, LV_ALIGN_TOP_LEFT, 0, 0);
    an_streak = lv_label_create(c1);
    lv_label_set_text(an_streak, "0 d");
    lv_obj_set_style_text_color(an_streak, lv_color_hex(S_OK), 0);
    lv_obj_set_style_text_font(an_streak, &lv_font_montserrat_22, 0);
    lv_obj_align(an_streak, LV_ALIGN_BOTTOM_LEFT, 0, 0);

    lv_obj_t *c2 = make_card(tab_analytics, 175, 80);
    lv_obj_align(c2, LV_ALIGN_TOP_LEFT, 215, 250);
    lv_obj_t *cl2 = lv_label_create(c2);
    lv_label_set_text(cl2, "SESSIONS");
    lv_obj_set_style_text_color(cl2, lv_color_hex(S_MUTE), 0);
    lv_obj_set_style_text_font(cl2, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_letter_space(cl2, 3, 0);
    lv_obj_align(cl2, LV_ALIGN_TOP_LEFT, 0, 0);
    an_sessions = lv_label_create(c2);
    lv_label_set_text(an_sessions, "0");
    lv_obj_set_style_text_color(an_sessions, lv_color_hex(S_TXT), 0);
    lv_obj_set_style_text_font(an_sessions, &lv_font_montserrat_22, 0);
    lv_obj_align(an_sessions, LV_ALIGN_BOTTOM_LEFT, 0, 0);

    build_back_btn(tab_analytics);
}

static void build_flow_overlay() {
    flow_overlay = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(flow_overlay);
    lv_obj_set_size(flow_overlay, LCD_WIDTH, LCD_HEIGHT);
    lv_obj_set_style_bg_color(flow_overlay, lv_color_hex(S_FLOW_BG), 0);
    lv_obj_set_style_bg_opa(flow_overlay, LV_OPA_COVER, 0);
    lv_obj_add_flag(flow_overlay, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t *eye = lv_label_create(flow_overlay);
    lv_label_set_text(eye, "FLOW");
    lv_obj_set_style_text_color(eye, lv_color_hex(S_DIM), 0);
    lv_obj_set_style_text_font(eye, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_letter_space(eye, 6, 0);
    lv_obj_align(eye, LV_ALIGN_TOP_MID, 0, 40);

    flow_time_lbl = lv_label_create(flow_overlay);
    lv_label_set_text(flow_time_lbl, "00:00");
    lv_obj_set_style_text_color(flow_time_lbl, lv_color_hex(S_FLOW_TXT), 0);
    lv_obj_set_style_text_font(flow_time_lbl, &lv_font_montserrat_48, 0);
    lv_obj_set_style_text_opa(flow_time_lbl, 170, 0);
    lv_obj_align(flow_time_lbl, LV_ALIGN_CENTER, 0, -10);

    flow_remain_lbl = lv_label_create(flow_overlay);
    lv_label_set_text(flow_remain_lbl, "");
    lv_obj_set_style_text_color(flow_remain_lbl, lv_color_hex(S_DIM), 0);
    lv_obj_set_style_text_font(flow_remain_lbl, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_letter_space(flow_remain_lbl, 3, 0);
    lv_obj_align(flow_remain_lbl, LV_ALIGN_CENTER, 0, 50);
}

static void build_ui() {
    scr_root = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr_root, lv_color_hex(S_BG), 0);
    lv_obj_set_style_bg_opa(scr_root, LV_OPA_COVER, 0);

    tabv = lv_tabview_create(scr_root, LV_DIR_TOP, 0);
    lv_obj_set_size(tabv, LCD_WIDTH, LCD_HEIGHT);
    lv_obj_set_style_bg_color(tabv, lv_color_hex(S_BG), 0);
    lv_obj_set_style_bg_opa(tabv, LV_OPA_COVER, 0);

    tab_home      = lv_tabview_add_tab(tabv, "HOME");
    tab_pomo      = lv_tabview_add_tab(tabv, "POMO");
    tab_task      = lv_tabview_add_tab(tabv, "TASKS");
    tab_cards     = lv_tabview_add_tab(tabv, "CARDS");
    tab_analytics = lv_tabview_add_tab(tabv, "STATS");

    for (lv_obj_t *t : { tab_home, tab_pomo, tab_task, tab_cards, tab_analytics }) {
        lv_obj_set_style_bg_color(t, lv_color_hex(S_BG), 0);
        lv_obj_set_style_bg_opa(t, LV_OPA_COVER, 0);
        lv_obj_set_style_pad_all(t, 0, 0);
    }

    build_home_tab();
    build_pomo_tab();
    build_task_tab();
    build_cards_tab();
    build_analytics_tab();

    toast_lbl = lv_label_create(lv_layer_top());
    lv_label_set_text(toast_lbl, "");
    lv_obj_set_style_text_color(toast_lbl, lv_color_hex(S_TXT), 0);
    lv_obj_set_style_text_font(toast_lbl, &lv_font_montserrat_14, 0);
    lv_obj_set_style_bg_color(toast_lbl, lv_color_hex(0x141414), 0);
    lv_obj_set_style_bg_opa(toast_lbl, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(toast_lbl, 8, 0);
    lv_obj_set_style_radius(toast_lbl, 8, 0);
    lv_obj_align(toast_lbl, LV_ALIGN_TOP_MID, 0, 6);
    lv_obj_add_flag(toast_lbl, LV_OBJ_FLAG_HIDDEN);

    build_flow_overlay();

    // Carousel page-indicator dots (S3_MiniOS-style: 5 dots, active = accent, others = dim)
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
            lv_color_hex(i == 0 ? S_ACCENT : S_DIM), 0);
        lv_obj_set_style_bg_opa(page_dots[i], LV_OPA_COVER, 0);
    }
    lv_obj_add_event_cb(tabv, [](lv_event_t *e){
        uint32_t a = lv_tabview_get_tab_act((lv_obj_t*)lv_event_get_target(e));
        for (int i = 0; i < 5; i++) if (page_dots[i]) {
            lv_obj_set_size(page_dots[i], i == (int)a ? 8 : 5, i == (int)a ? 8 : 5);
            lv_obj_set_style_bg_color(page_dots[i],
                lv_color_hex(i == (int)a ? S_ACCENT : S_DIM), 0);
        }
    }, LV_EVENT_VALUE_CHANGED, NULL);

    lv_scr_load(scr_root);
}

// =============================================================================
//  Splash -- monochrome calm boot
// =============================================================================
static void show_splash() {
    gfx->fillScreen(0x0000);
    gfx->setTextSize(3);
    gfx->setTextColor(gfx->color565(0xF2, 0xF2, 0xF2));
    gfx->setCursor(120, LCD_HEIGHT/2 - 16);
    gfx->print("STUDY");
    gfx->setTextColor(gfx->color565(0x4F, 0xB3, 0xFF));
    gfx->setCursor(120, LCD_HEIGHT/2 + 20);
    gfx->print(" / / OS");
    gfx->setTextSize(1);
    gfx->setTextColor(gfx->color565(0x80, 0x80, 0x80));
    gfx->setCursor(110, LCD_HEIGHT - 100);
    gfx->print("calm. disciplined. structured.");
    panel_on(180);
    delay(900);
}

// =============================================================================
//  Per-tab updaters
// =============================================================================
static char g_last_hhmm[8] = {0};
static void update_home_panel() {
    struct tm tm;
    if (getLocalTime(&tm, 5)) {
        char t[8], d[32];
        strftime(t, sizeof(t), "%H:%M", &tm);
        strftime(d, sizeof(d), "%A %d %b", &tm);
        if (strcmp(t, g_last_hhmm) != 0) {
            strcpy(g_last_hhmm, t);
            lv_label_set_text(home_clock, t);
        }
        lv_label_set_text(home_date, d);
    }
    char m[16]; snprintf(m, sizeof(m), "%lum", g_today_seconds / 60);
    lv_label_set_text(home_today_min, m);
    char s[16]; snprintf(s, sizeof(s), "%lu d", g_streak_days);
    lv_label_set_text(home_streak, s);
    if (home_goal_bar) {
        int mins = g_today_seconds / 60;
        if (mins > 240) mins = 240;
        lv_bar_set_value(home_goal_bar, mins, LV_ANIM_ON);
    }
    if (g_has_pmu) {
        int p = g_pmu.getBatteryPercent();
        bool ch = g_pmu.isCharging();
        char b[16]; snprintf(b, sizeof(b), "%d%%", p);
        lv_label_set_text(home_batt, b);
        // [B] Auto-dim under 15% (skip when charging or in flow which already dims)
        if (!g_flow && !ch) {
            static int last_clamp = 255;
            int target = p < 8 ? 8 : (p < 15 ? 50 : 180);
            if (target != last_clamp) { co5300_set_brightness(target); last_clamp = target; }
        }
    }
}

static void update_card_face() {
    if (!card_text) return;
    if (g_card_count == 0) return;
    Card &c = g_cards[g_card_idx];
    char buf[280];
    if (c.has_pinyin) {
        // Chinese-style card: characters / pinyin always shown; English revealed on flip
        if (g_card_flipped) {
            snprintf(buf, sizeof(buf), "%s\n\n%s\n\n%s", c.front, c.pinyin, c.back);
        } else {
            snprintf(buf, sizeof(buf), "%s\n\n%s\n\n( tap FLIP for English )",
                     c.front, c.pinyin);
        }
        lv_label_set_text(card_text, buf);
    } else {
        lv_label_set_text(card_text, g_card_flipped ? c.back : c.front);
    }
    lv_obj_set_style_text_color(card_text,
        lv_color_hex(g_card_flipped ? S_ACCENT : S_TXT), 0);
    char cc[40];
    snprintf(cc, sizeof(cc), "%d / %d  ·  %d ✓  %d ↻",
             g_card_idx + 1, g_card_count,
             g_card_remembered, g_card_review);
    lv_label_set_text(card_counter, cc);
}

static void update_analytics() {
    char m[16]; snprintf(m, sizeof(m), "%lu min", g_today_seconds / 60);
    lv_label_set_text(an_today, m);
    char s[16]; snprintf(s, sizeof(s), "%lu d", g_streak_days);
    lv_label_set_text(an_streak, s);
    char ss[16]; snprintf(ss, sizeof(ss), "%lu", g_total_sessions);
    lv_label_set_text(an_sessions, ss);
    int mins = g_today_seconds / 60;
    if (mins > 240) mins = 240;
    lv_arc_set_value(an_minutes_arc, mins);
}

static void update_flow_overlay() {
    if (!g_flow || !flow_overlay) return;
    uint32_t elapsed = millis() - g_phase_t0;
    if (elapsed > g_phase_len_ms) elapsed = g_phase_len_ms;
    uint32_t remain = (g_phase_len_ms - elapsed) / 1000;
    char t[16]; snprintf(t, sizeof(t), "%02lu:%02lu", remain / 60, remain % 60);
    lv_label_set_text(flow_time_lbl, t);
    lv_label_set_text(flow_remain_lbl, "STAY WITH IT");
}

// =============================================================================
//  setup / loop
// =============================================================================
void setup() {
    launcher_app_boot();
    show_splash();
    time_restore_from_nvs();
    wifi_load_from_sd();

    // Auto-create all SD folders this app touches (idempotent; safe if they exist)
    SD_MMC.mkdir("/WATCH");
    SD_MMC.mkdir("/WATCH/wifi");
    SD_MMC.mkdir("/WATCH/PHOTOS");
    SD_MMC.mkdir("/WATCH/bin");
    SD_MMC.mkdir("/WATCH/study");
    SD_MMC.mkdir("/WATCH/study/cards");
    SD_MMC.mkdir("/WATCH/study/voice");
    SD_MMC.mkdir("/WATCH/study/exports");

    // First-run scaffolding: seed example tasks + flashcards so the app
    // doesn't boot into empty screens. Skipped if user files already exist.
    if (!SD_MMC.exists("/WATCH/study/tasks.txt")) {
        File f = SD_MMC.open("/WATCH/study/tasks.txt", FILE_WRITE);
        if (f) {
            f.println("# StudyOS daily tasks -- one per line, # for comments");
            f.println("Review lecture notes");
            f.println("30 min reading");
            f.println("Practice flashcards");
            f.println("Deep work session (Pomodoro DEEP)");
            f.close();
        }
    }
    if (!SD_MMC.exists("/WATCH/study/cards/example.txt")) {
        File f = SD_MMC.open("/WATCH/study/cards/example.txt", FILE_WRITE);
        if (f) {
            f.println("# format: front|back");
            f.println("mitochondria|powerhouse of the cell");
            f.println("ribosome|protein synthesis");
            f.println("golgi apparatus|packaging + distribution");
            f.close();
        }
    }
    // Chinese conversation cheat-sheet (3-field: hanzi|pinyin|english)
    if (!SD_MMC.exists("/WATCH/study/cards/chinese_basics.txt")) {
        File f = SD_MMC.open("/WATCH/study/cards/chinese_basics.txt", FILE_WRITE);
        if (f) {
            f.println("# Mandarin conversation cheat sheet");
            f.println("# format: hanzi|pinyin|english");
            // Greetings
            f.println("你好|ni3 hao3|hello");
            f.println("你好吗|ni3 hao3 ma|how are you?");
            f.println("我很好|wo3 hen3 hao3|I'm good");
            f.println("早上好|zao3 shang4 hao3|good morning");
            f.println("晚安|wan3 an1|good night");
            f.println("再见|zai4 jian4|goodbye");
            f.println("谢谢|xie4 xie4|thank you");
            f.println("不客气|bu2 ke4 qi4|you're welcome");
            f.println("对不起|dui4 bu4 qi3|sorry");
            f.println("没关系|mei2 guan1 xi4|it's ok / no problem");
            // Identity
            f.println("我叫...|wo3 jiao4 ...|my name is ...");
            f.println("你叫什么名字|ni3 jiao4 shen2 me ming2 zi|what's your name?");
            f.println("我是美国人|wo3 shi4 mei3 guo2 ren2|I'm American");
            f.println("你是哪国人|ni3 shi4 na3 guo2 ren2|where are you from?");
            // Survival
            f.println("我不懂|wo3 bu4 dong3|I don't understand");
            f.println("请再说一遍|qing3 zai4 shuo1 yi2 bian4|please say it again");
            f.println("你会说英语吗|ni3 hui4 shuo1 ying1 yu3 ma|do you speak English?");
            f.println("洗手间在哪里|xi3 shou3 jian1 zai4 na3 li3|where is the bathroom?");
            f.println("多少钱|duo1 shao qian2|how much money?");
            f.println("太贵了|tai4 gui4 le|too expensive");
            f.println("便宜一点|pian2 yi yi4 dian3|cheaper please");
            // Food
            f.println("我饿了|wo3 e4 le|I'm hungry");
            f.println("我要这个|wo3 yao4 zhe4 ge4|I want this one");
            f.println("好吃|hao3 chi1|delicious");
            f.println("不要辣|bu2 yao4 la4|not spicy please");
            f.println("买单|mai3 dan1|the bill please");
            f.println("水|shui3|water");
            f.println("茶|cha2|tea");
            // Numbers
            f.println("一 二 三|yi1 er4 san1|1 2 3");
            f.println("四 五 六|si4 wu3 liu4|4 5 6");
            f.println("七 八 九 十|qi1 ba1 jiu3 shi2|7 8 9 10");
            f.println("一百|yi4 bai3|one hundred");
            // Directions
            f.println("左|zuo3|left");
            f.println("右|you4|right");
            f.println("直走|zhi2 zou3|straight ahead");
            f.println("这里|zhe4 li3|here");
            f.println("那里|na4 li3|there");
            // Useful
            f.println("是|shi4|yes / is");
            f.println("不是|bu2 shi4|no / is not");
            f.println("有|you3|to have");
            f.println("没有|mei2 you3|don't have");
            f.println("可以|ke3 yi3|can / may");
            f.println("好的|hao3 de|ok / good");
            f.println("帮我|bang1 wo3|help me");
            f.close();
        }
    }
    // Slang / gossip / jokes / sus pack -- real internet Mandarin
    if (!SD_MMC.exists("/WATCH/study/cards/chinese_slang.txt")) {
        File f = SD_MMC.open("/WATCH/study/cards/chinese_slang.txt", FILE_WRITE);
        if (f) {
            f.println("# Chinese slang, gossip, jokes & sus phrases");
            f.println("# format: hanzi|pinyin|english");
            // Internet slang
            f.println("yyds|yong3 yuan3 de shen2|eternal GOAT / best ever");
            f.println("666|liu4 liu4 liu4|sick / awesome / damn that's good");
            f.println("绝绝子|jue2 jue2 zi3|absolutely killer / iconic");
            f.println("牛|niu2|badass / cow (slang for impressive)");
            f.println("太牛了|tai4 niu2 le|that's insane / so OP");
            f.println("emo了|emo le|I'm emo / I'm in my feelings");
            f.println("躺平|tang3 ping2|lying flat / I give up trying");
            f.println("内卷|nei4 juan3|toxic grind culture");
            f.println("摆烂|bai3 lan4|let it rot / stop caring");
            f.println("好家伙|hao3 jia1 huo|whoa buddy / wtf");
            f.println("我裂开了|wo3 lie4 kai1 le|I'm losing it / broken");
            f.println("绷不住了|beng1 bu zhu4 le|can't hold it in / dying");
            f.println("社死|she4 si3|social death / so embarrassing");
            f.println("尴尬|gan1 ga4|awkward / cringe");
            f.println("无语|wu2 yu3|speechless / I can't even");
            f.println("笑死我了|xiao4 si3 wo3 le|I'm dying of laughter");
            f.println("哈哈哈|ha ha ha|lol");
            f.println("草|cao3|damn (mild, like crap)");
            // Gossip
            f.println("听说|ting1 shuo1|I heard that...");
            f.println("八卦|ba1 gua4|gossip / tea");
            f.println("吃瓜|chi1 gua1|eating melon / spectating drama");
            f.println("瓜很大|gua1 hen3 da4|the tea is HOT");
            f.println("塌房了|ta1 fang2 le|the house collapsed / idol got cancelled");
            f.println("绿茶|lv4 cha2|fake/manipulative person");
            f.println("渣男|zha1 nan2|fuckboy");
            f.println("渣女|zha1 nv3|female counterpart");
            f.println("舔狗|tian3 gou3|simp");
            f.println("备胎|bei4 tai1|backup option / spare tire");
            f.println("脚踏两只船|jiao3 ta4 liang3 zhi1 chuan2|two-timing");
            f.println("劈腿|pi1 tui3|cheating");
            f.println("分手|fen1 shou3|breakup");
            f.println("前任|qian2 ren4|ex");
            // Compliments / vibes
            f.println("好看|hao3 kan4|good looking");
            f.println("帅|shuai4|handsome / cool");
            f.println("漂亮|piao4 liang|pretty");
            f.println("身材好|shen1 cai2 hao3|nice body");
            f.println("有点意思|you3 dian3 yi4 si|kinda interesting / I'm into it");
            // Sus / flirty (PG-13 only)
            f.println("可疑|ke3 yi2|sus");
            f.println("懂得都懂|dong3 de dou1 dong3|iykyk");
            f.println("我想你了|wo3 xiang3 ni3 le|I miss you");
            f.println("心动|xin1 dong4|heart skipped / crushing");
            f.println("脸红|lian3 hong2|blushing");
            f.println("撩|liao1|to flirt");
            f.println("暧昧|ai4 mei4|situationship vibes");
            // Gaming / school slang
            f.println("上分|shang4 fen1|ranking up");
            f.println("掉分|diao4 fen1|losing rank");
            f.println("挂机|gua4 ji1|afk");
            f.println("送人头|song4 ren2 tou2|feeding (gaming)");
            f.println("队友是坑|dui4 you3 shi4 keng1|my teammates are trash");
            f.println("躺赢|tang3 ying2|carried to victory");
            f.println("学霸|xue2 ba4|tryhard student / nerd (positive)");
            f.println("学渣|xue2 zha1|trash at school (self-roast)");
            f.println("作业写完了吗|zuo4 ye4 xie3 wan2 le ma|did you finish homework");
            f.println("抄作业|chao1 zuo4 ye4|copying homework");
            f.println("考试要凉了|kao3 shi4 yao4 liang2 le|gonna bomb this test");
            // Roasts / banter
            f.println("废话|fei4 hua4|nonsense / no shit");
            f.println("闭嘴|bi4 zui3|shut up");
            f.println("滚|gun3|get lost");
            f.println("你脑子有问题|ni3 nao3 zi you3 wen4 ti2|you got a screw loose");
            f.println("傻子|sha3 zi3|dummy");
            f.println("笨蛋|ben4 dan4|idiot (cute / playful)");
            f.println("菜|cai4|trash at it (gaming/skill)");
            f.println("太菜了|tai4 cai4 le|absolute bot / so bad");
            // Jokes / wordplay
            f.println("冷笑话|leng3 xiao4 hua4|cold joke / dad joke");
            f.println("为什么鸡过马路|wei4 shen2 me ji1 guo4 ma3 lu4|why did the chicken cross the road?");
            f.println("因为它想到对面|yin1 wei4 ta1 xiang3 dao4 dui4 mian4|because it wanted to get to the other side");
            f.println("摸鱼|mo1 yu2|slacking / touching fish");
            f.println("打工人|da3 gong1 ren2|wage slave / grunt");
            f.println("加班|jia1 ban1|overtime (cry)");
            f.println("救命|jiu4 ming4|HELP / save me (overreaction)");
            f.println("芭比Q了|ba1 bi3 Q le|BBQ'd / I'm cooked");
            f.println("我谢谢你啊|wo3 xie4 xie ni3 a|gee thanks (sarcastic)");
            f.println("你礼貌吗|ni3 li3 mao4 ma|excuse me, manners?");
            f.println("不会吧不会吧|bu2 hui4 ba bu2 hui4 ba|no way, no way (disbelief)");
            f.close();
        }
    }

    prefs_load();
    load_tasks();
    load_cards();
    build_ui();
    setCpuFrequencyMhz(160);

    time_t now; time(&now);
    bool need_ntp = !g_time_valid || (now < 1700000000)
                  || ((uint32_t)now - g_last_ntp_ok > 6UL * 3600UL);
    if (need_ntp) wifi_sync_once();
    if (home_sync_dot)
        lv_obj_set_style_bg_color(home_sync_dot,
            lv_color_hex(g_wifi_synced ? S_OK : 0xFF4040), 0);

    rollover_day_if_needed();
    update_home_panel();
    update_pomo_ui();
    update_card_face();
    update_analytics();
    show_toast("STUDY ONLINE", 1500);
}

void loop() {
    uint32_t now = millis();
    lv_timer_handler();
    tick_toast();
    pomo_tick();
    voice_tick();

    static uint32_t btn_t = 0;
    if (digitalRead(BOOT_BTN_PIN) == LOW) {
        if (!btn_t) btn_t = now;
        else if (now - btn_t > 1500) return_to_launcher();
    } else btn_t = 0;

    static uint32_t tick_t = 0;
    if (now - tick_t >= 1000) {
        tick_t = now;
        rollover_day_if_needed();
        update_home_panel();
        update_pomo_ui();
        update_card_face();
        update_analytics();
        update_flow_overlay();
    }

    delay(5);
}
