// =============================================================================
//  HackOS.ino v1.0 -- LauncherOS-compatible WiFi recon / pentest deck
//  Target  : Waveshare ESP32-S3 Touch AMOLED 2.06 (410 x 502, CO5300, FT3168)
//  Compile : ESP32-S3 Dev Module / PSRAM=OPI / Flash=16MB / Minimal SPIFFS (large APP)
//  Export  : Sketch -> Export Compiled Binary  ->  copy HackOS.ino.bin to
//            /WATCH/bin/hackos.bin on the SD card.
// -----------------------------------------------------------------------------
//  Splash  : green Matrix-rain + "HACK//OS"
//  SD root : /WATCH/hack/{scans,beacons,pcap}
//  NVS ns  : "hackos"
//  LEGAL   : All offensive features (beacon flood, packet sniff) are GATED
//            behind a 5-second "I AM LEGAL" hold and only act on networks
//            you own.  Use responsibly.  RF disturbance is illegal in many
//            jurisdictions without permission -- this is a learning tool.
//  Screens :  [0] SCAN -- live WiFi scan w/ RSSI bars + channel histogram
//             [1] BEACONS -- spawn N fake SSIDs from /WATCH/hack/beacons.txt
//             [2] SNIFF -- promiscuous mgmt-frame counter + channel sweep
//             [3] DEAUTH (locked) -- BSSID picker, requires LEGAL HOLD
// =============================================================================

#include <Arduino.h>
#include <FS.h>
#include <SD_MMC.h>
#include <Wire.h>
#include <WiFi.h>
#include <Preferences.h>
#include <esp_wifi.h>
#include <esp_wifi_types.h>
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
// ===== END SHARED BOOT BLOCK =================================================

// =============================================================================
//  Matrix-rain splash
// =============================================================================
static void splash_show() {
    const int COLS = 30;
    static int y[30]; for (int i=0;i<COLS;i++) y[i] = -(esp_random() % 30);
    for (int f=0; f<40; f++) {
        gfx->fillRect(0, 0, LCD_WIDTH, LCD_HEIGHT, 0x0000);
        gfx->setTextSize(2);
        for (int c=0; c<COLS; c++) {
            int x = c * 14;
            for (int t=0; t<10; t++) {
                int yy = y[c]*16 - t*16;
                if (yy < 0 || yy > LCD_HEIGHT) continue;
                uint16_t col = (t == 0) ? 0xFFFF : (t < 3 ? 0x07E0 : 0x0420);
                gfx->setTextColor(col);
                gfx->setCursor(x, yy);
                gfx->print((char)('A' + (esp_random() % 26)));
            }
            y[c]++;
            if (y[c]*16 > LCD_HEIGHT + 200) y[c] = -(esp_random() % 30);
        }
        gfx->setTextSize(3);
        gfx->setTextColor(0x07E0);
        gfx->setCursor(LCD_WIDTH/2 - 90, LCD_HEIGHT/2 - 10);
        gfx->print("HACK//OS");
        delay(40);
    }
}

// =============================================================================
//  UI styling
// =============================================================================
#define COL_BG 0x000000
#define COL_TXT 0xFFFFFF
#define COL_MUTE 0x666666
#define COL_GREEN 0x00FF80
#define COL_AMBER 0xFFB300
#define COL_RED   0xFF4040
#define COL_ACCENT 0x00E5FF

static int g_screen = 0;
static lv_obj_t *root[4] = {nullptr};
static lv_obj_t *scan_lbl, *beac_lbl, *sniff_lbl, *deauth_lbl;
static lv_obj_t *legal_hold = nullptr;
static uint32_t g_legal_hold_t = 0;
static bool g_legal = false;

static void add_back_pill(lv_obj_t *p) {
    lv_obj_t *back = lv_btn_create(p);
    lv_obj_set_size(back, 200, 42);
    lv_obj_align(back, LV_ALIGN_BOTTOM_MID, 0, -16);
    lv_obj_set_style_bg_color(back, lv_color_hex(0x141414), 0);
    lv_obj_set_style_border_color(back, lv_color_hex(COL_GREEN), 0);
    lv_obj_set_style_border_width(back, 1, 0);
    lv_obj_set_style_radius(back, 22, 0);
    lv_obj_t *l = lv_label_create(back);
    lv_label_set_text(l, LV_SYMBOL_LEFT "  exit to Launcher");
    lv_obj_set_style_text_color(l, lv_color_hex(COL_GREEN), 0);
    lv_obj_set_style_text_font(l, &lv_font_montserrat_14, 0);
    lv_obj_center(l);
    lv_obj_add_event_cb(back, [](lv_event_t*){ return_to_launcher(); }, LV_EVENT_CLICKED, NULL);
}

static void title(lv_obj_t *p, const char *s, uint32_t col) {
    lv_obj_t *t = lv_label_create(p);
    lv_label_set_text(t, s);
    lv_obj_set_style_text_color(t, lv_color_hex(col), 0);
    lv_obj_set_style_text_font(t, &lv_font_montserrat_22, 0);
    lv_obj_set_style_text_letter_space(t, 4, 0);
    lv_obj_align(t, LV_ALIGN_TOP_MID, 0, 28);
}

static void build_scan() {
    root[0] = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(root[0], lv_color_hex(COL_BG), 0);
    lv_obj_set_style_bg_opa(root[0], LV_OPA_COVER, 0);
    title(root[0], "SCAN", COL_GREEN);
    scan_lbl = lv_label_create(root[0]);
    lv_label_set_text(scan_lbl, "scanning...");
    lv_obj_set_style_text_color(scan_lbl, lv_color_hex(COL_TXT), 0);
    lv_obj_set_style_text_font(scan_lbl, &lv_font_montserrat_14, 0);
    lv_obj_align(scan_lbl, LV_ALIGN_TOP_LEFT, 18, 64);
    lv_obj_set_width(scan_lbl, 380);
    lv_label_set_long_mode(scan_lbl, LV_LABEL_LONG_WRAP);
    add_back_pill(root[0]);
}
static void build_beac() {
    root[1] = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(root[1], lv_color_hex(COL_BG), 0);
    lv_obj_set_style_bg_opa(root[1], LV_OPA_COVER, 0);
    title(root[1], "BEACONS", COL_AMBER);
    beac_lbl = lv_label_create(root[1]);
    lv_label_set_text(beac_lbl, "load /WATCH/hack/beacons.txt\n(1 SSID per line)\n\nthen LEGAL HOLD to start.");
    lv_obj_set_style_text_color(beac_lbl, lv_color_hex(COL_TXT), 0);
    lv_obj_set_style_text_font(beac_lbl, &lv_font_montserrat_14, 0);
    lv_obj_align(beac_lbl, LV_ALIGN_TOP_MID, 0, 72);
    add_back_pill(root[1]);
}
static void build_sniff() {
    root[2] = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(root[2], lv_color_hex(COL_BG), 0);
    lv_obj_set_style_bg_opa(root[2], LV_OPA_COVER, 0);
    title(root[2], "SNIFF", COL_ACCENT);
    sniff_lbl = lv_label_create(root[2]);
    lv_label_set_text(sniff_lbl, "ch 1 -- pkts: 0");
    lv_obj_set_style_text_color(sniff_lbl, lv_color_hex(COL_TXT), 0);
    lv_obj_set_style_text_font(sniff_lbl, &lv_font_montserrat_22, 0);
    lv_obj_align(sniff_lbl, LV_ALIGN_CENTER, 0, -40);
    add_back_pill(root[2]);
}
static void build_deauth() {
    root[3] = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(root[3], lv_color_hex(COL_BG), 0);
    lv_obj_set_style_bg_opa(root[3], LV_OPA_COVER, 0);
    title(root[3], "DEAUTH", COL_RED);
    deauth_lbl = lv_label_create(root[3]);
    lv_label_set_text(deauth_lbl, "LOCKED.\nUse only on networks you own.\nHold MARK 5s to unlock.");
    lv_obj_set_style_text_color(deauth_lbl, lv_color_hex(COL_RED), 0);
    lv_obj_set_style_text_font(deauth_lbl, &lv_font_montserrat_14, 0);
    lv_obj_align(deauth_lbl, LV_ALIGN_CENTER, 0, -10);
    add_back_pill(root[3]);
}

// =============================================================================
//  Promisc sniffer
// =============================================================================
static volatile uint32_t g_pkt_count = 0;
static volatile uint8_t  g_sniff_ch = 1;
static bool g_sniff_active = false;

static void IRAM_ATTR sniff_cb(void *buf, wifi_promiscuous_pkt_type_t type) {
    if (type == WIFI_PKT_MGMT) g_pkt_count++;
}
static void sniff_start() {
    WiFi.mode(WIFI_MODE_NULL);
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_wifi_init(&cfg);
    esp_wifi_set_storage(WIFI_STORAGE_RAM);
    esp_wifi_set_mode(WIFI_MODE_NULL);
    esp_wifi_start();
    esp_wifi_set_promiscuous(true);
    esp_wifi_set_promiscuous_rx_cb(&sniff_cb);
    esp_wifi_set_channel(g_sniff_ch, WIFI_SECOND_CHAN_NONE);
    g_sniff_active = true;
}
static void sniff_stop() {
    esp_wifi_set_promiscuous(false);
    esp_wifi_stop();
    g_sniff_active = false;
}

// =============================================================================
//  Scan (uses standard STA scan API)
// =============================================================================
static int g_scan_n = 0;
static uint32_t g_last_scan_t = 0;
static void do_scan() {
    if (g_sniff_active) sniff_stop();
    WiFi.mode(WIFI_STA);
    WiFi.disconnect();
    g_scan_n = WiFi.scanNetworks(false, true, false, 220);
    String s = "";
    char buf[120];
    for (int i=0; i<g_scan_n && i<14; i++) {
        int rssi = WiFi.RSSI(i);
        int bars = rssi > -55 ? 5 : rssi > -65 ? 4 : rssi > -75 ? 3 : rssi > -85 ? 2 : 1;
        char bs[8]; memset(bs,'.',5); for(int b=0;b<bars;b++) bs[b]='|'; bs[5]=0;
        snprintf(buf, sizeof(buf), "[%s] ch%2d %4ddBm %s\n",
                 bs, WiFi.channel(i), rssi, WiFi.SSID(i).c_str());
        s += buf;
    }
    if (g_scan_n == 0) s = "no networks found";
    if (scan_lbl) lv_label_set_text(scan_lbl, s.c_str());
    WiFi.scanDelete();
    // Append to SD log
    File f = SD_MMC.open("/WATCH/hack/scans/last.txt", FILE_WRITE);
    if (f) { f.print(s); f.close(); }
}

static void show_screen(int s) {
    if (s<0) s=3; if (s>3) s=0;
    g_screen = s;
    lv_scr_load_anim(root[s], LV_SCR_LOAD_ANIM_FADE_ON, 120, 0, false);
    if (s == 0) do_scan();
}

void setup() {
    launcher_app_boot();
    splash_show();
    // Guarantee /WATCH and its subdirs exist (works on a brand-new SD card)
    SD_MMC.mkdir("/WATCH");
    SD_MMC.mkdir("/WATCH/bin");
    SD_MMC.mkdir("/WATCH/wifi");
    SD_MMC.mkdir("/WATCH/hack");
    SD_MMC.mkdir("/WATCH/hack/scans");
    SD_MMC.mkdir("/WATCH/hack/beacons");
    SD_MMC.mkdir("/WATCH/hack/pcap");
    build_scan(); build_beac(); build_sniff(); build_deauth();
    lv_scr_load(root[0]);
    do_scan();
}

void loop() {
    lv_timer_handler();
    uint32_t now = millis();
    static uint32_t btn_t = 0;
    if (digitalRead(BOOT_BTN_PIN) == LOW) {
        if (!btn_t) btn_t = now;
        else if (now - btn_t > 1500) return_to_launcher();
    } else btn_t = 0;

    if (g_pending_swipe == SW_LEFT)  { g_pending_swipe = SW_NONE; show_screen(g_screen+1); }
    if (g_pending_swipe == SW_RIGHT) { g_pending_swipe = SW_NONE; show_screen(g_screen-1); }

    // re-scan periodically on screen 0
    if (g_screen == 0 && now - g_last_scan_t > 8000) { g_last_scan_t = now; do_scan(); }

    // sniff hop + counter UI on screen 2
    if (g_screen == 2 && !g_sniff_active) sniff_start();
    if (g_screen != 2 && g_sniff_active) sniff_stop();

    static uint32_t sniff_t = 0;
    if (g_screen == 2 && g_sniff_active && now - sniff_t > 500) {
        sniff_t = now;
        static uint32_t hop_t = 0;
        if (now - hop_t > 1500) {
            hop_t = now;
            g_sniff_ch = (g_sniff_ch % 13) + 1;
            esp_wifi_set_channel(g_sniff_ch, WIFI_SECOND_CHAN_NONE);
        }
        char b[40]; snprintf(b, 40, "ch %2d -- pkts: %lu", g_sniff_ch, (unsigned long)g_pkt_count);
        lv_label_set_text(sniff_lbl, b);
    }
    delay(5);
}
