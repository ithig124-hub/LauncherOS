// =====================================================================
//  SportOS.ino  --  v2  Athlete Edition
//  LauncherOS-compatible Workout / Sport module
//  Target  : Waveshare ESP32-S3 Touch AMOLED 2.06
//  Build   : ESP32-S3 Dev Module, PSRAM=OPI, Partition=Minimal SPIFFS (large APP)
//  Output  : Export Compiled Binary -> copy SportOS.ino.bin to /WATCH/bin/sportos.bin
//
//  What's new vs v1 (improvements):
//    + REAL step counter via QMI8658 IMU (peak detection on accel magnitude)
//    + Auto-pause when motion stops (RUN / WALK)
//    + Shake-to-lap gesture
//    + Sport profiles (Run / Walk / Cycle / HIIT) with per-sport MET, stride, colour
//    + Workout MODES: Free Session, Training (Easy/Tempo/Long/Intervals/Fartlek),
//                     Race Day (target distance + target finish + virtual partner),
//                     Cross Country (laps + bumpiness score + hill flag)
//    + Auto-laps every 1 km (or 1 mi) + manual lap button
//    + Splits screen during workout (last 6 laps)
//    + Weekly km ring on watch face
//    + Settings: km / mi toggle (default km), weight, weekly goal, audio cues
//    + Sport-specific XP curve (XC > Race > Run > Training > Walk)
//    + Reusable LVGL helpers (mk_card / mk_btn / mk_label) → much less code
//
//  Required libraries (in addition to v1):
//    - SensorLib (SensorQMI8658.hpp)   bundled in Waveshare board kit
//
//  All v1 behaviour preserved: 3-card swipe loop, Launcher return,
//  AXP2101 battery, SD wallpaper, OTA flash of launcheros.bin, BOOT
//  long-press safety, watch face fonts/colours, splash visual.
// =====================================================================

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

#include "SensorQMI8658.hpp"

#include <esp_ota_ops.h>
#include <esp_partition.h>
#include <esp_system.h>
#include <esp_sleep.h>
#include <esp_bt.h>
#include <esp32-hal-cpu.h>
#include <Update.h>

// ---------------------------------------------------------------------
// Forward decls / sport + mode enums
// ---------------------------------------------------------------------
enum Sport  { SP_RUN=0, SP_WALK=1, SP_CYCLE=2, SP_HIIT=3, SP_XC=4 };
enum Mode   { M_FREE=0, M_TRAINING=1, M_RACE=2, M_CROSS=3 };
enum Plan   { PL_EASY=0, PL_TEMPO=1, PL_LONG=2, PL_INTERVAL=3, PL_FARTLEK=4 };

// v3: forward decls for new helpers that finalize_session calls
static void evaluate_achievements_after_session(int sec);
static void unlock_ach(int id);
static void daily_rollover_tick();
static void show_ach_toast(int id);

#define LCD_SDIO0 4
#define LCD_SDIO1 5
#define LCD_SDIO2 6
#define LCD_SDIO3 7
#define LCD_SCLK 11
#define LCD_CS 12
#define LCD_RESET 8
#define LCD_W 410
#define LCD_H 502
#define IIC_SDA 15
#define IIC_SCL 14
#define TP_INT 38
#define TP_RESET 9
static const int SDMMC_CLK=2,SDMMC_CMD=1,SDMMC_DATA=3;
#define BOOT_BTN 0
#define PWR_BTN 10
#define HC_WIFI_SSID "SportOS-Fallback"
#define HC_WIFI_PASS "changeme"

// Palette
#define ACCENT_SPORT  0x00FF80   // neon green   (RUN)
#define ACCENT_CYAN   0x00E5FF   // (WALK / UI primary)
#define ACCENT_AMBER  0xFFB300   // (CYCLE)
#define ACCENT_DANGER 0xFF4040   // (HIIT)
#define ACCENT_XC     0xB48BFF   // muted violet (CROSS COUNTRY)
#define ACCENT_RACE   0xFFD400   // gold         (RACE DAY)
#define ACCENT_TRAIN  0x6EE7FF   // ice cyan     (TRAINING)
#define COL_BG        0x000000
#define COL_CARD      0x0E0E0E
#define COL_BORDER    0x1C1C1C
#define COL_MUTED     0x666666
#define COL_MUTED2    0x888888
#define COL_MUTED3    0xBBBBBB

#define WEIGHT_KG_DEFAULT      70
#define DEFAULT_WEEKLY_GOAL_KM 20
#define DEFAULT_AUTO_LAP_M     1000
#define MAX_LAPS               64

Arduino_DataBus *g_bus = new Arduino_ESP32QSPI(LCD_CS,LCD_SCLK,LCD_SDIO0,LCD_SDIO1,LCD_SDIO2,LCD_SDIO3);
Arduino_GFX *gfx = new Arduino_CO5300(g_bus,LCD_RESET,0,LCD_W,LCD_H,22,0,0,0);
std::shared_ptr<Arduino_IIC_DriveBus> g_iic = std::make_shared<Arduino_HWIIC>(IIC_SDA,IIC_SCL,&Wire);
void Arduino_IIC_Touch_Interrupt(void);
std::unique_ptr<Arduino_IIC> FT3168(new Arduino_FT3x68(g_iic,FT3168_DEVICE_ADDRESS,DRIVEBUS_DEFAULT_VALUE,TP_INT,Arduino_IIC_Touch_Interrupt));
void Arduino_IIC_Touch_Interrupt(void){ FT3168->IIC_Interrupt_Flag = true; }
XPowersPMU g_pmu; bool g_has_pmu=false;
SensorQMI8658 g_imu; bool g_has_imu=false;

static lv_disp_draw_buf_t g_db; static lv_color_t *g_lvbuf=nullptr;
static lv_disp_drv_t g_dd; static lv_indev_drv_t g_id;
static void lv_flush(lv_disp_drv_t*d,const lv_area_t*a,lv_color_t*p){
    uint32_t w=a->x2-a->x1+1, h=a->y2-a->y1+1;
    gfx->draw16bitRGBBitmap(a->x1,a->y1,(uint16_t*)p,w,h);
    lv_disp_flush_ready(d);
}

// =====================================================================
//  3-CARD SWIPE LOOP (Face <-> Apps <-> Battery)
// =====================================================================
#define SCREEN_FACE             0
#define SCREEN_APPS             1
#define SCREEN_BATTERY          2
#define NUM_SCREENS             3
#define SWIPE_THRESHOLD_PX      25
#define SWIPE_MAX_DURATION_MS   800
#define TAP_THRESHOLD_PX        20
#define CLICK_AFTER_SWIPE_MS    350
#define SCREEN_SWITCH_ANIM_MS   140

static lv_obj_t *screens[NUM_SCREENS] = { nullptr, nullptr, nullptr };
static int       current_screen   = SCREEN_FACE;
static int32_t   ts_x=0, ts_y=0, tc_x=0, tc_y=0;
static uint32_t  ts_ms=0;
static bool      touchActive=false;
static uint32_t  lastSwipeMs=0;
static volatile int g_pending_swipe = 0;
static lv_obj_t *g_transient_screen = nullptr;
static bool      g_apps_built = false;

static void lv_touch_gesture(lv_indev_drv_t *, lv_indev_data_t *d);
static void goto_screen(int s);
static void build_screen_face();
static void build_screen_apps();
static void build_screen_battery();
static void update_battery_screen();
static void open_app_screen(lv_obj_t *s);
static void open_home();

// =====================================================================
//  Return to LauncherOS (factory partition)
// =====================================================================
static void return_to_launcher(){
    const esp_partition_t *factory = esp_partition_find_first(
        ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_FACTORY, NULL);
    if(factory){
        esp_ota_set_boot_partition(factory);
    } else {
        const esp_partition_t *other = esp_ota_get_next_update_partition(NULL);
        if(other && other != esp_ota_get_running_partition())
            esp_ota_set_boot_partition(other);
    }
    delay(80); esp_restart();
}

// =====================================================================
//  Wallpaper (raw RGB565 from /WATCH/PHOTOS)
// =====================================================================
static uint8_t *g_wp_buf=nullptr;
static lv_img_dsc_t g_wp_dsc;
static String pick_wallpaper(){
    Preferences p; p.begin("face",true);
    int idx=p.getInt("wp_idx",0); p.end();
    File dir=SD_MMC.open("/WATCH/PHOTOS"); if(!dir) return "";
    File f=dir.openNextFile(); int i=0; String found="";
    while(f){
        String n=f.name(); String lc=n; lc.toLowerCase();
        if(!f.isDirectory() && (lc.endsWith(".raw")||lc.endsWith(".bin"))){
            if(i==idx){ found=String("/WATCH/PHOTOS/")+f.name(); break; }
            i++;
        }
        f=dir.openNextFile();
    }
    return found;
}
static void make_wallpaper_bg(lv_obj_t *scr){
    String p=pick_wallpaper(); if(p.length()==0) return;
    File f=SD_MMC.open(p.c_str(),FILE_READ); if(!f) return;
    size_t sz=LCD_W*LCD_H*2;
    if(!g_wp_buf) g_wp_buf=(uint8_t*)heap_caps_malloc(sz,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
    if(!g_wp_buf) g_wp_buf=(uint8_t*)heap_caps_malloc(sz,MALLOC_CAP_8BIT);
    if(!g_wp_buf){ f.close(); return; }
    f.read(g_wp_buf,sz); f.close();
    g_wp_dsc.header.cf=LV_IMG_CF_TRUE_COLOR;
    g_wp_dsc.header.always_zero=0; g_wp_dsc.header.reserved=0;
    g_wp_dsc.header.w=LCD_W; g_wp_dsc.header.h=LCD_H;
    g_wp_dsc.data_size=sz; g_wp_dsc.data=g_wp_buf;
    lv_obj_t *img=lv_img_create(scr);
    lv_img_set_src(img,&g_wp_dsc);
    lv_obj_align(img,LV_ALIGN_CENTER,0,0);
    lv_obj_move_background(img);
}

// =====================================================================
//  SD-based OTA: flash /WATCH/bin/launcheros.bin
// =====================================================================
static void flash_launcher_bin(){
    File f=SD_MMC.open("/WATCH/bin/launcheros.bin",FILE_READ);
    if(!f || f.size()<10000){ if(f) f.close(); return_to_launcher(); return; }
    lv_obj_t *ov=lv_obj_create(lv_scr_act()); lv_obj_remove_style_all(ov);
    lv_obj_set_size(ov,LCD_W,LCD_H); lv_obj_align(ov,LV_ALIGN_CENTER,0,0);
    lv_obj_set_style_bg_color(ov,lv_color_hex(COL_BG),0);
    lv_obj_set_style_bg_opa(ov,LV_OPA_COVER,0);
    lv_obj_t *t=lv_label_create(ov); lv_label_set_text(t,"LOADING LAUNCHEROS");
    lv_obj_set_style_text_color(t,lv_color_hex(ACCENT_CYAN),0);
    lv_obj_set_style_text_font(t,&lv_font_montserrat_18,0);
    lv_obj_set_style_text_letter_space(t,3,0);
    lv_obj_align(t,LV_ALIGN_CENTER,0,-40);
    lv_obj_t *bar=lv_bar_create(ov); lv_obj_set_size(bar,280,4);
    lv_obj_align(bar,LV_ALIGN_CENTER,0,20);
    lv_obj_set_style_bg_color(bar,lv_color_hex(COL_BORDER),LV_PART_MAIN);
    lv_obj_set_style_bg_color(bar,lv_color_hex(ACCENT_CYAN),LV_PART_INDICATOR);
    lv_obj_t *pct=lv_label_create(ov); lv_label_set_text(pct,"0 %");
    lv_obj_set_style_text_color(pct,lv_color_hex(0xFFFFFF),0);
    lv_obj_set_style_text_font(pct,&lv_font_montserrat_22,0);
    lv_obj_align(pct,LV_ALIGN_CENTER,0,55);
    lv_timer_handler();
    size_t sz=f.size();
    if(!Update.begin(sz)){ f.close(); return_to_launcher(); return; }
    uint8_t buf[2048]; size_t total=0;
    while(f.available()){
        int n=f.read(buf,sizeof(buf)); if(n<=0) break;
        Update.write(buf,n); total+=n;
        int pp=(int)((total*100)/sz);
        lv_bar_set_value(bar,pp,LV_ANIM_OFF);
        char b[8]; snprintf(b,8,"%d %%",pp); lv_label_set_text(pct,b);
        if((total & 0x3FFF)==0) lv_timer_handler();
    }
    f.close();
    if(Update.end(true)){ delay(300); esp_restart(); } else { return_to_launcher(); }
}

// =====================================================================
//  WiFi / NTP / SD
// =====================================================================
struct WifiCred{ String ssid, pass; };
static WifiCred g_creds[6]; static int g_creds_n=0; static int g_gmt=0; static bool g_wifi_synced=false;
static void load_wifi_config(){
    g_creds_n=0;
    File f=SD_MMC.open("/WATCH/wifi/config.txt",FILE_READ);
    if(!f){ g_creds[g_creds_n++]={HC_WIFI_SSID,HC_WIFI_PASS}; return; }
    while(f.available()){
        String ln=f.readStringUntil('\n'); ln.trim();
        if(ln.length()==0 || ln.startsWith("#")) continue;
        int eq=ln.indexOf('='); if(eq<0) continue;
        String k=ln.substring(0,eq); k.trim();
        String v=ln.substring(eq+1); v.trim();
        for(int i=1;i<=5;i++){
            if(k=="WIFI"+String(i)+"_SSID"){ if(g_creds_n<5) g_creds[g_creds_n].ssid=v; }
            if(k=="WIFI"+String(i)+"_PASS"){ if(g_creds_n<5){ g_creds[g_creds_n].pass=v; g_creds_n++; } }
        }
        if(k=="GMT_OFFSET") g_gmt=v.toInt();
    }
    f.close();
    g_creds[g_creds_n++]={HC_WIFI_SSID,HC_WIFI_PASS};
}
static void wifi_quick_sync(){
    if(!g_creds_n) return;
    WiFi.mode(WIFI_STA);
    for(int i=0;i<g_creds_n;i++){
        WiFi.begin(g_creds[i].ssid.c_str(),g_creds[i].pass.c_str());
        uint32_t t=millis();
        while(WiFi.status()!=WL_CONNECTED && millis()-t<4000) delay(100);
        if(WiFi.status()==WL_CONNECTED){
            configTime(g_gmt*3600,0,"pool.ntp.org","time.google.com");
            struct tm tm; getLocalTime(&tm,3000); g_wifi_synced=true; break;
        }
    }
    WiFi.disconnect(true); WiFi.mode(WIFI_OFF);
}
static void sd_ensure_dirs(){
    if(!SD_MMC.exists("/WATCH"))          SD_MMC.mkdir("/WATCH");
    if(!SD_MMC.exists("/WATCH/workouts")) SD_MMC.mkdir("/WATCH/workouts");
}

// =====================================================================
//  Hardware boot (display, touch, PMIC, IMU, SD)
// =====================================================================
static void hw_boot(){
    Serial.begin(115200);
    pinMode(BOOT_BTN,INPUT_PULLUP); pinMode(PWR_BTN,INPUT_PULLUP); btStop();
    if(!gfx->begin()) Serial.println("gfx fail");
    gfx->fillScreen(0x0000);

    // Force-clear stuck I2C
    pinMode(IIC_SCL,OUTPUT);
    for(int i=0;i<9;i++){ digitalWrite(IIC_SCL,LOW); delayMicroseconds(5);
                          digitalWrite(IIC_SCL,HIGH);delayMicroseconds(5); }
    Wire.begin(IIC_SDA,IIC_SCL);

    pinMode(TP_RESET,OUTPUT); digitalWrite(TP_RESET,LOW); delay(50);
    digitalWrite(TP_RESET,HIGH); delay(200);
    FT3168->begin();

    if(g_pmu.begin(Wire,AXP2101_SLAVE_ADDRESS,IIC_SDA,IIC_SCL)){
        g_has_pmu=true;
        g_pmu.disableTSPinMeasure();
        g_pmu.enableBattVoltageMeasure();
    }

    // QMI8658 IMU (shared I2C bus). 250 Hz is plenty for step detection.
    if(g_imu.begin(Wire, QMI8658_L_SLAVE_ADDRESS, IIC_SDA, IIC_SCL)){
        g_has_imu = true;
        g_imu.configAccelerometer(SensorQMI8658::ACC_RANGE_4G,
                                  SensorQMI8658::ACC_ODR_250Hz,
                                  SensorQMI8658::LPF_MODE_0);
        g_imu.enableAccelerometer();
    }

    lv_init();
    size_t px = LCD_W * 60;
    g_lvbuf = (lv_color_t*)heap_caps_malloc(px*sizeof(lv_color_t),MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT);
    if(!g_lvbuf) g_lvbuf = (lv_color_t*)heap_caps_malloc(px*sizeof(lv_color_t),MALLOC_CAP_8BIT);
    lv_disp_draw_buf_init(&g_db,g_lvbuf,NULL,px);
    lv_disp_drv_init(&g_dd); g_dd.flush_cb=lv_flush; g_dd.draw_buf=&g_db;
    g_dd.hor_res=LCD_W; g_dd.ver_res=LCD_H;
    lv_disp_drv_register(&g_dd);
    lv_indev_drv_init(&g_id); g_id.type=LV_INDEV_TYPE_POINTER; g_id.read_cb=lv_touch_gesture;
    lv_indev_drv_register(&g_id);

    SD_MMC.setPins(SDMMC_CLK,SDMMC_CMD,SDMMC_DATA);
    SD_MMC.begin("/sdcard",true);
    sd_ensure_dirs();
    load_wifi_config();
}

// =====================================================================
//  Boot splash (LauncherOS-style grid + arc)
// =====================================================================
static void splash_show(){
    lv_obj_t *s=lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s,lv_color_hex(COL_BG),0);
    lv_obj_set_style_bg_opa(s,LV_OPA_COVER,0);
    lv_obj_clear_flag(s,LV_OBJ_FLAG_SCROLLABLE);
    for(int x=0;x<LCD_W;x+=40){
        lv_obj_t *v=lv_obj_create(s); lv_obj_remove_style_all(v);
        lv_obj_set_size(v,1,LCD_H); lv_obj_set_pos(v,x,0);
        lv_obj_set_style_bg_color(v,lv_color_hex(0x0A1A12),0);
        lv_obj_set_style_bg_opa(v,LV_OPA_COVER,0);
    }
    for(int y=0;y<LCD_H;y+=40){
        lv_obj_t *h=lv_obj_create(s); lv_obj_remove_style_all(h);
        lv_obj_set_size(h,LCD_W,1); lv_obj_set_pos(h,0,y);
        lv_obj_set_style_bg_color(h,lv_color_hex(0x0A1A12),0);
        lv_obj_set_style_bg_opa(h,LV_OPA_COVER,0);
    }
    lv_obj_t *arc=lv_arc_create(s);
    lv_obj_set_size(arc,280,280); lv_obj_align(arc,LV_ALIGN_CENTER,0,-20);
    lv_arc_set_rotation(arc,270); lv_arc_set_bg_angles(arc,0,360);
    lv_arc_set_value(arc,0); lv_obj_remove_style(arc,NULL,LV_PART_KNOB);
    lv_obj_set_style_arc_color(arc,lv_color_hex(0x0E2218),LV_PART_MAIN);
    lv_obj_set_style_arc_color(arc,lv_color_hex(ACCENT_SPORT),LV_PART_INDICATOR);
    lv_obj_set_style_arc_width(arc,6,LV_PART_MAIN);
    lv_obj_set_style_arc_width(arc,6,LV_PART_INDICATOR);

    lv_obj_t *t=lv_label_create(s); lv_label_set_text(t,"SPORT//OS");
    lv_obj_set_style_text_color(t,lv_color_hex(ACCENT_SPORT),0);
    lv_obj_set_style_text_font(t,&lv_font_montserrat_28,0);
    lv_obj_set_style_text_letter_space(t,4,0);
    lv_obj_align(t,LV_ALIGN_CENTER,0,-40);
    lv_obj_t *sub=lv_label_create(s); lv_label_set_text(sub,"00 %");
    lv_obj_set_style_text_color(sub,lv_color_hex(0xFFFFFF),0);
    lv_obj_set_style_text_font(sub,&lv_font_montserrat_22,0);
    lv_obj_align(sub,LV_ALIGN_CENTER,0,10);
    lv_obj_t *st=lv_label_create(s); lv_label_set_text(st,"BOOT IMU");
    lv_obj_set_style_text_color(st,lv_color_hex(COL_MUTED2),0);
    lv_obj_set_style_text_font(st,&lv_font_montserrat_14,0);
    lv_obj_set_style_text_letter_space(st,3,0);
    lv_obj_align(st,LV_ALIGN_BOTTOM_MID,0,-80);
    lv_scr_load(s);

    const char *steps[]={
        g_has_imu?"IMU OK":"IMU MISSING",
        "CAL GYRO","LOAD STATS","SYNC RTC","READY"
    };
    for(int p=0; p<=100; p+=4){
        lv_arc_set_value(arc,p);
        char buf[8]; snprintf(buf,8,"%02d %%",p); lv_label_set_text(sub,buf);
        if(p%20==0) lv_label_set_text(st,steps[p/20>4?4:p/20]);
        lv_timer_handler(); delay(30);
        if(p==40) wifi_quick_sync();
    }
}

// =====================================================================
//  Shared top strip (transient screens)
// =====================================================================
static lv_obj_t *g_batt_fill,*g_batt_pct,*g_batt_bolt,*g_clock_lbl,*g_date_lbl,*g_wifi_dot;
static void make_top_strip(lv_obj_t *p){
    lv_obj_t *c=lv_obj_create(p); lv_obj_remove_style_all(c);
    lv_obj_set_size(c,110,36); lv_obj_align(c,LV_ALIGN_TOP_LEFT,20,18);
    lv_obj_set_style_bg_color(c,lv_color_hex(COL_CARD),0);
    lv_obj_set_style_bg_opa(c,LV_OPA_COVER,0);
    lv_obj_set_style_radius(c,18,0);
    lv_obj_set_style_border_color(c,lv_color_hex(COL_BORDER),0);
    lv_obj_set_style_border_width(c,1,0);
    g_batt_fill=lv_obj_create(c); lv_obj_remove_style_all(g_batt_fill);
    lv_obj_set_size(g_batt_fill,0,18); lv_obj_align(g_batt_fill,LV_ALIGN_LEFT_MID,8,0);
    lv_obj_set_style_radius(g_batt_fill,9,0);
    lv_obj_set_style_bg_opa(g_batt_fill,LV_OPA_COVER,0);
    g_batt_bolt=lv_label_create(c); lv_label_set_text(g_batt_bolt,LV_SYMBOL_CHARGE);
    lv_obj_set_style_text_color(g_batt_bolt,lv_color_hex(ACCENT_SPORT),0);
    lv_obj_align(g_batt_bolt,LV_ALIGN_LEFT_MID,6,0); lv_obj_add_flag(g_batt_bolt,LV_OBJ_FLAG_HIDDEN);
    g_batt_pct=lv_label_create(c); lv_label_set_text(g_batt_pct,"--%");
    lv_obj_set_style_text_color(g_batt_pct,lv_color_hex(0xFFFFFF),0);
    lv_obj_set_style_text_font(g_batt_pct,&lv_font_montserrat_14,0);
    lv_obj_align(g_batt_pct,LV_ALIGN_RIGHT_MID,-10,0);
    g_wifi_dot=lv_obj_create(p); lv_obj_remove_style_all(g_wifi_dot);
    lv_obj_set_size(g_wifi_dot,10,10); lv_obj_align(g_wifi_dot,LV_ALIGN_TOP_RIGHT,-22,30);
    lv_obj_set_style_radius(g_wifi_dot,5,0); lv_obj_set_style_bg_opa(g_wifi_dot,LV_OPA_COVER,0);
    lv_obj_set_style_bg_color(g_wifi_dot,lv_color_hex(g_wifi_synced?ACCENT_SPORT:ACCENT_DANGER),0);
    g_clock_lbl=lv_label_create(p); lv_label_set_text(g_clock_lbl,"--:--");
    lv_obj_set_style_text_color(g_clock_lbl,lv_color_hex(0xFFFFFF),0);
    lv_obj_set_style_text_font(g_clock_lbl,&lv_font_montserrat_22,0);
    lv_obj_align(g_clock_lbl,LV_ALIGN_TOP_MID,0,68);
    g_date_lbl=lv_label_create(p); lv_label_set_text(g_date_lbl,"no sync");
    lv_obj_set_style_text_color(g_date_lbl,lv_color_hex(COL_MUTED2),0);
    lv_obj_set_style_text_font(g_date_lbl,&lv_font_montserrat_12,0);
    lv_obj_align(g_date_lbl,LV_ALIGN_TOP_MID,0,98);
}
void tick_top(){
    if (!g_batt_fill || !lv_obj_is_valid(g_batt_fill)) return;
    if(g_has_pmu){
        int pct=g_pmu.getBatteryPercent(); bool ch=g_pmu.isCharging();
        uint32_t c = pct<15?ACCENT_DANGER : pct<30?ACCENT_AMBER : ACCENT_CYAN;
        lv_obj_set_size(g_batt_fill,(94*pct)/100,18);
        lv_obj_set_style_bg_color(g_batt_fill,lv_color_hex(c),0);
        char b[8]; snprintf(b,8,"%d%%",pct); lv_label_set_text(g_batt_pct,b);
        if(ch) lv_obj_clear_flag(g_batt_bolt,LV_OBJ_FLAG_HIDDEN);
        else   lv_obj_add_flag(g_batt_bolt,LV_OBJ_FLAG_HIDDEN);
    }
    struct tm tm;
    if(getLocalTime(&tm,5)){
        char t[8],d[24]; strftime(t,8,"%H:%M",&tm); strftime(d,24,"%a %d %b",&tm);
        lv_label_set_text(g_clock_lbl,t); lv_label_set_text(g_date_lbl,d);
    }
}

// =====================================================================
//  Settings (NVS)
// =====================================================================
struct Settings {
    bool  use_miles      = false;
    int   weight_kg      = WEIGHT_KG_DEFAULT;
    int   weekly_goal_km = DEFAULT_WEEKLY_GOAL_KM;
    int   auto_lap_m     = DEFAULT_AUTO_LAP_M;
    bool  audio_cues     = true;
    int   week_iso       = -1;          // ISO week of weekly_done
    float weekly_done_km = 0.0f;
    // v3: daily step tracker (24h pedometer, always-on while watch is awake)
    int   daily_step_goal = 8000;
    int   today_steps     = 0;          // resets at local midnight
    String today_ymd      = "";         // YYYY-MM-DD of `today_steps`
    int   step_hist[7]    = {0,0,0,0,0,0,0}; // last 7 finished days, hist[0]=oldest
    // v3: achievements bitmap (bit per badge, see ACH_* enum below)
    uint32_t ach_unlocked = 0;
};
static Settings S;
static Preferences nvs;
static void load_settings(){
    nvs.begin("sportos",true);
    S.use_miles      = nvs.getBool ("mi",     false);
    S.weight_kg      = nvs.getInt  ("wkg",    WEIGHT_KG_DEFAULT);
    S.weekly_goal_km = nvs.getInt  ("wgoal",  DEFAULT_WEEKLY_GOAL_KM);
    S.auto_lap_m     = nvs.getInt  ("alap",   DEFAULT_AUTO_LAP_M);
    S.audio_cues     = nvs.getBool ("aud",    true);
    S.week_iso       = nvs.getInt  ("wkiso",  -1);
    S.weekly_done_km = nvs.getFloat("wkkm",   0.0f);
    // v3
    S.daily_step_goal= nvs.getInt  ("dgoal",  8000);
    S.today_steps    = nvs.getInt  ("dstep",  0);
    S.today_ymd      = nvs.getString("dymd",  "");
    for(int i=0;i<7;i++){
        char k[8]; snprintf(k,8,"sh%d",i);
        S.step_hist[i] = nvs.getInt(k, 0);
    }
    S.ach_unlocked   = nvs.getUInt ("ach",    0);
    nvs.end();
}
static void save_settings(){
    nvs.begin("sportos",false);
    nvs.putBool ("mi",    S.use_miles);
    nvs.putInt  ("wkg",   S.weight_kg);
    nvs.putInt  ("wgoal", S.weekly_goal_km);
    nvs.putInt  ("alap",  S.auto_lap_m);
    nvs.putBool ("aud",   S.audio_cues);
    nvs.putInt  ("wkiso", S.week_iso);
    nvs.putFloat("wkkm",  S.weekly_done_km);
    nvs.putInt  ("dgoal", S.daily_step_goal);
    nvs.putInt  ("dstep", S.today_steps);
    nvs.putString("dymd", S.today_ymd);
    for(int i=0;i<7;i++){
        char k[8]; snprintf(k,8,"sh%d",i);
        nvs.putInt(k, S.step_hist[i]);
    }
    nvs.putUInt ("ach",   S.ach_unlocked);
    nvs.end();
}
static int iso_week_now(){
    struct tm tm; if(!getLocalTime(&tm,5)) return -1;
    char b[4]; strftime(b,4,"%V",&tm); return atoi(b);
}
static void register_weekly_km(float add_km){
    int w = iso_week_now();
    if(w<0) return;
    if(w != S.week_iso){ S.week_iso = w; S.weekly_done_km = 0; }
    S.weekly_done_km += add_km;
}

// Unit conversions
static inline float to_user_dist(float km){ return S.use_miles ? km*0.621371f : km; }
static inline const char* unit_dist(){ return S.use_miles ? "mi" : "km"; }
static inline float user_to_km(float v){  return S.use_miles ? v/0.621371f : v; }
static inline int   pace_sec_to_user(int sec_per_km){
    if(sec_per_km<=0) return 0;
    return S.use_miles ? (int)(sec_per_km*1.609344f) : sec_per_km;
}
static inline const char* unit_pace(){ return S.use_miles ? "/ mi" : "/ km"; }

// =====================================================================
//  Player stats / XP / streak (NVS)
// =====================================================================
struct PlayerStats {
    int xp=0, level=1, streak=0;
    float total_km=0; int total_sec=0;
    String last_day="";
};
static PlayerStats P;
static int xp_for_level(int lv){ return 100 + (lv-1)*60; }
static void load_player(){
    nvs.begin("sportos",true);
    P.xp=nvs.getInt("xp",0);
    P.level=nvs.getInt("lv",1);
    P.streak=nvs.getInt("st",0);
    P.total_km=nvs.getFloat("km",0);
    P.total_sec=nvs.getInt("sec",0);
    P.last_day=nvs.getString("day","");
    nvs.end();
}
static void save_player(){
    nvs.begin("sportos",false);
    nvs.putInt("xp",P.xp);
    nvs.putInt("lv",P.level);
    nvs.putInt("st",P.streak);
    nvs.putFloat("km",P.total_km);
    nvs.putInt("sec",P.total_sec);
    nvs.putString("day",P.last_day);
    nvs.end();
}
static void add_xp(int gain){
    P.xp += gain;
    while(P.xp >= xp_for_level(P.level)){ P.xp -= xp_for_level(P.level); P.level++; }
}
static void register_session_day(){
    struct tm tm; if(!getLocalTime(&tm,5)) return;
    char day[16]; strftime(day,16,"%Y-%m-%d",&tm);
    if(P.last_day==day) return;
    if(P.last_day.length()==0) P.streak=1;
    else {
        struct tm pt={}; int y,mo,d; sscanf(P.last_day.c_str(),"%d-%d-%d",&y,&mo,&d);
        pt.tm_year=y-1900; pt.tm_mon=mo-1; pt.tm_mday=d;
        time_t pp=mktime(&pt), nn=mktime(&tm);
        long diff=(nn-pp)/86400;
        if(diff==1) P.streak++; else if(diff>1) P.streak=1;
    }
    P.last_day=day;
}

// =====================================================================
//  Sport profiles (per-sport tuning)
// =====================================================================
struct SportProfile {
    const char *name;
    const char *letter;
    uint32_t    color;
    float       met;        // metabolic equivalent at moderate effort
    float       stride_m;   // base stride (0 for non-stepping sports)
    float       spm_default;// cadence (used as fallback when IMU unavailable)
    int         default_pace_sec; // sec / km for sports where pace matters
    float       xp_per_km;  // sport-specific XP weight
};
static const SportProfile SPORTS[] = {
    /* RUN   */ {"RUN",  "R", ACCENT_SPORT, 9.8f, 1.05f, 165, 360, 30.0f},
    /* WALK  */ {"WALK", "W", ACCENT_CYAN,  3.8f, 0.72f, 105, 720, 18.0f},
    /* CYCLE */ {"CYCLE","C", ACCENT_AMBER, 7.5f, 0.00f,   0, 180, 14.0f},
    /* HIIT  */ {"HIIT", "H", ACCENT_DANGER,10.0f,0.00f,   0,   0, 28.0f},
    /* XC    */ {"XC",   "X", ACCENT_XC,    11.5f,1.00f, 160, 420, 40.0f}
};

// =====================================================================
//  Workout state (extended)
// =====================================================================
struct Lap { float km; uint32_t sec; int pace_sec_per_km; };
struct Workout {
    // identity
    Sport sport      = SP_RUN;
    Mode  mode       = M_FREE;
    Plan  plan       = PL_EASY;
    // timing
    uint32_t t_start = 0;
    uint32_t t_pause = 0;
    uint32_t paused_total = 0;
    bool     paused  = false;
    bool     auto_paused = false;
    // counters
    int   steps          = 0;
    float km             = 0;
    int   target_pace_sec= 360;
    // race-day specifics
    float race_target_km     = 5.0f;     // 5K, 10K, 21.0975, 42.195, or custom
    int   race_target_sec    = 30*60;    // 30:00
    // HIIT
    int hiit_work=30, hiit_rest=15, hiit_rounds=8, hiit_phase=0;
    uint32_t hiit_phase_start=0;
    // intervals (TRAINING / PL_INTERVAL)  e.g. 6 x 400 m, 90 s recovery
    int intv_reps=6, intv_dist_m=400, intv_recov_s=90;
    int intv_idx=0; bool intv_in_recov=false; uint32_t intv_phase_start=0; float intv_phase_km=0;
    // laps
    Lap   laps[MAX_LAPS];
    int   lap_count   = 0;
    float lap_start_km= 0;
    uint32_t lap_start_sec=0;
    // XC
    float bumpiness = 0;        // smoothed IMU jerk magnitude
    bool  on_hill   = false;    // gravity-z tilt heuristic
};
static Workout WK;

// =====================================================================
//  IMU step detection + auto-pause + shake
// =====================================================================
//   - Read accel ~50 Hz from `loop()`
//   - Filter: high-pass-ish via running mean of magnitude, look for peaks
//     above 1.15 g with min 280 ms gap between steps
//   - last_step_ms drives auto-pause timeout
// ---------------------------------------------------------------------
struct ImuState {
    float mean = 1.0f;
    float prev = 1.0f;
    uint32_t last_step_ms = 0;
    uint32_t last_motion_ms = 0;
    float gz_lp = 1.0f;
    uint32_t last_shake_ms = 0;
};
static ImuState IMU;
static volatile int g_imu_pending_lap = 0;

static bool workout_active(); // fwd

static void imu_tick(){
    static uint32_t t_last = 0;
    uint32_t now = millis();
    if(now - t_last < 18) return;          // ~55 Hz
    t_last = now;
    if(!g_has_imu) return;
    if(!g_imu.getDataReady()) return;
    float ax=0, ay=0, az=0;
    if(!g_imu.getAccelerometer(ax,ay,az)) return;
    float mag = sqrtf(ax*ax + ay*ay + az*az);
    // smooth gravity-z for hill detection
    IMU.gz_lp = 0.97f*IMU.gz_lp + 0.03f*az;

    // running mean to track baseline (acts as DC removal)
    IMU.mean = 0.95f*IMU.mean + 0.05f*mag;
    float ac = mag - IMU.mean;             // ac-coupled magnitude

    // peak detector (positive going)
    bool peak = (IMU.prev < ac) && (ac > 0.18f);   // ~0.18 g threshold
    IMU.prev = ac;

    // motion timestamp
    if(fabsf(ac) > 0.06f) IMU.last_motion_ms = now;

    // step?
    if(peak && (now - IMU.last_step_ms) > 280){
        IMU.last_step_ms = now;
        // v3: count toward the 24h pedometer even outside workouts
        S.today_steps++;
        if(workout_active() && !WK.paused && !WK.auto_paused){
            const SportProfile &sp = SPORTS[WK.sport];
            if(sp.stride_m > 0.0f){
                WK.steps++;
                WK.km += sp.stride_m / 1000.0f;
            }
        }
    }

    // shake (high jerk in any axis) -> lap
    float ajerk = fabsf(ac);
    if(ajerk > 0.9f && (now - IMU.last_shake_ms) > 1200){
        IMU.last_shake_ms = now;
        if(workout_active()) g_imu_pending_lap = 1;
    }

    // hill flag (Cross Country only): if smoothed gz < 0.85 -> uphill,
    // > 1.10 -> downhill.  We just set a generic "on_hill" flag.
    if(WK.sport == SP_XC) WK.on_hill = (IMU.gz_lp < 0.88f) || (IMU.gz_lp > 1.12f);

    // bumpiness EMA (for XC) — variance proxy
    if(WK.sport == SP_XC) WK.bumpiness = 0.98f*WK.bumpiness + 0.02f*fabsf(ac);
}

// =====================================================================
//  Workout engine helpers
// =====================================================================
static lv_timer_t *aw_t = nullptr;
static lv_obj_t *aw_time,*aw_dist,*aw_pace,*aw_steps,*aw_cal,*aw_paceband;
static lv_obj_t *aw_hud,*aw_phase,*aw_lap,*aw_extra,*aw_partner;

static bool workout_active(){ return aw_t != nullptr; }

static uint32_t elapsed_sec(){
    if(WK.t_start==0) return 0;
    uint32_t base = (WK.paused ? WK.t_pause : millis()) - WK.t_start - WK.paused_total;
    return base/1000;
}
static int current_pace_sec_per_km(){
    if(WK.km < 0.005f) return 0;
    return (int)(elapsed_sec()/WK.km);
}
static float estimate_calories(){
    float met = SPORTS[WK.sport].met;
    if(WK.sport == SP_XC && WK.on_hill) met *= 1.15f;     // hill bonus
    if(WK.mode == M_RACE) met *= 1.05f;                   // race-effort bias
    float hours = elapsed_sec() / 3600.0f;
    return met * S.weight_kg * hours;
}

static void log_lap(){
    if(WK.lap_count >= MAX_LAPS) return;
    float lkm  = WK.km - WK.lap_start_km;
    uint32_t lsec = elapsed_sec() - WK.lap_start_sec;
    int      pkm  = (lkm > 0.005f) ? (int)(lsec/lkm) : 0;
    WK.laps[WK.lap_count++] = { lkm, lsec, pkm };
    WK.lap_start_km  = WK.km;
    WK.lap_start_sec = elapsed_sec();
}

// =====================================================================
//  LVGL helpers — small primitives to cut UI duplication
// =====================================================================
static lv_obj_t* mk_label(lv_obj_t *p, const char *txt, const lv_font_t *font,
                          uint32_t col, lv_align_t align, int x, int y, int lspace=0){
    lv_obj_t *l = lv_label_create(p);
    lv_label_set_text(l, txt);
    lv_obj_set_style_text_color(l, lv_color_hex(col), 0);
    lv_obj_set_style_text_font(l, font, 0);
    if(lspace) lv_obj_set_style_text_letter_space(l, lspace, 0);
    lv_obj_align(l, align, x, y);
    return l;
}
static lv_obj_t* mk_pill(lv_obj_t *p, int w, int h, uint32_t bg, uint32_t border,
                         const char *txt, uint32_t txt_col, const lv_font_t *font,
                         lv_align_t align, int x, int y, lv_event_cb_t cb){
    lv_obj_t *b = lv_btn_create(p);
    lv_obj_set_size(b,w,h); lv_obj_align(b,align,x,y);
    lv_obj_set_style_bg_color(b,lv_color_hex(bg),0);
    lv_obj_set_style_border_color(b,lv_color_hex(border),0);
    lv_obj_set_style_border_width(b,1,0);
    lv_obj_set_style_radius(b,h/2,0);
    lv_obj_t *l = lv_label_create(b);
    lv_label_set_text(l,txt);
    lv_obj_set_style_text_color(l,lv_color_hex(txt_col),0);
    lv_obj_set_style_text_font(l,font,0);
    lv_obj_set_style_text_letter_space(l,2,0);
    lv_obj_center(l);
    if(cb) lv_obj_add_event_cb(b,cb,LV_EVENT_CLICKED,NULL);
    return b;
}
static lv_obj_t* mk_solid_btn(lv_obj_t *p, int w, int h, uint32_t bg,
                              const char *txt, uint32_t txt_col,
                              lv_align_t align, int x, int y, lv_event_cb_t cb){
    lv_obj_t *b = lv_btn_create(p);
    lv_obj_set_size(b,w,h); lv_obj_align(b,align,x,y);
    lv_obj_set_style_bg_color(b,lv_color_hex(bg),0);
    lv_obj_set_style_border_width(b,0,0);
    lv_obj_set_style_radius(b,h/2,0);
    lv_obj_t *l = lv_label_create(b);
    lv_label_set_text(l,txt);
    lv_obj_set_style_text_color(l,lv_color_hex(txt_col),0);
    lv_obj_set_style_text_font(l,&lv_font_montserrat_16,0);
    lv_obj_set_style_text_letter_space(l,3,0);
    lv_obj_center(l);
    if(cb) lv_obj_add_event_cb(b,cb,LV_EVENT_CLICKED,NULL);
    return b;
}
// Single reusable card with coloured accent bar + circular letter chip
static lv_obj_t* mk_card(lv_obj_t *list, uint32_t color, const char *letter,
                         const char *name, const char *desc,
                         lv_event_cb_t cb){
    lv_obj_t *card = lv_obj_create(list); lv_obj_remove_style_all(card);
    lv_obj_set_size(card, LV_PCT(100), 72);
    lv_obj_set_style_bg_color(card,lv_color_hex(COL_CARD),0);
    lv_obj_set_style_bg_opa(card,LV_OPA_COVER,0);
    lv_obj_set_style_radius(card,18,0);
    lv_obj_set_style_border_color(card,lv_color_hex(COL_BORDER),0);
    lv_obj_set_style_border_width(card,1,0);
    lv_obj_set_style_pad_all(card,14,0);
    lv_obj_set_style_bg_color(card,lv_color_hex(0x1A1A1A),LV_STATE_PRESSED);
    lv_obj_set_style_transform_zoom(card,250,LV_STATE_PRESSED);
    lv_obj_clear_flag(card,LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *bar=lv_obj_create(card); lv_obj_remove_style_all(bar);
    lv_obj_set_size(bar,4,38); lv_obj_align(bar,LV_ALIGN_LEFT_MID,0,0);
    lv_obj_set_style_radius(bar,2,0);
    lv_obj_set_style_bg_color(bar,lv_color_hex(color),0);
    lv_obj_set_style_bg_opa(bar,LV_OPA_COVER,0);

    lv_obj_t *dot=lv_obj_create(card); lv_obj_remove_style_all(dot);
    lv_obj_set_size(dot,36,36); lv_obj_align(dot,LV_ALIGN_LEFT_MID,18,0);
    lv_obj_set_style_radius(dot,18,0);
    lv_obj_set_style_bg_color(dot,lv_color_hex(color),0);
    lv_obj_set_style_bg_opa(dot,60,0);
    lv_obj_set_style_border_color(dot,lv_color_hex(color),0);
    lv_obj_set_style_border_width(dot,1,0);
    mk_label(dot, letter, &lv_font_montserrat_22, color, LV_ALIGN_CENTER, 0, 0);

    mk_label(card, name, &lv_font_montserrat_18, 0xFFFFFF, LV_ALIGN_LEFT_MID, 64, -10, 2);
    mk_label(card, desc, &lv_font_montserrat_12, COL_MUTED, LV_ALIGN_LEFT_MID, 64, 12, 1);
    mk_label(card, LV_SYMBOL_RIGHT, &lv_font_montserrat_22, 0x333333,
             LV_ALIGN_RIGHT_MID, -8, 0);

    lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);
    if(cb) lv_obj_add_event_cb(card, cb, LV_EVENT_CLICKED, NULL);
    return card;
}

// =====================================================================
//  Back-to-Launcher card
// =====================================================================
static void make_back_card(lv_obj_t *p){
    lv_obj_t *card=lv_btn_create(p);
    lv_obj_set_size(card,LCD_W-60,72);
    lv_obj_align(card,LV_ALIGN_BOTTOM_MID,0,-26);
    lv_obj_set_style_bg_color(card,lv_color_hex(COL_CARD),0);
    lv_obj_set_style_bg_opa(card,LV_OPA_COVER,0);
    lv_obj_set_style_border_color(card,lv_color_hex(ACCENT_CYAN),0);
    lv_obj_set_style_border_width(card,1,0);
    lv_obj_set_style_radius(card,16,0);
    mk_label(card, LV_SYMBOL_LEFT, &lv_font_montserrat_22, ACCENT_CYAN, LV_ALIGN_LEFT_MID, 20, 0);
    mk_label(card, "Back to Launcher",
             &lv_font_montserrat_18, 0xFFFFFF, LV_ALIGN_LEFT_MID, 56, -10);
    mk_label(card, "loads /WATCH/bin/launcheros.bin",
             &lv_font_montserrat_12, COL_MUTED, LV_ALIGN_LEFT_MID, 56, 12, 1);
    lv_obj_t *dot=lv_obj_create(card); lv_obj_remove_style_all(dot);
    lv_obj_set_size(dot,8,8); lv_obj_align(dot,LV_ALIGN_RIGHT_MID,-22,0);
    lv_obj_set_style_radius(dot,4,0); lv_obj_set_style_bg_opa(dot,LV_OPA_COVER,0);
    lv_obj_set_style_bg_color(dot,lv_color_hex(ACCENT_CYAN),0);
    lv_obj_add_event_cb(card,[](lv_event_t*){ flash_launcher_bin(); },LV_EVENT_CLICKED,NULL);
}

// =====================================================================
//  Watch face (clock + weekly km ring)
// =====================================================================
static lv_obj_t *wf_clk=nullptr,*wf_dt=nullptr,*wf_goal=nullptr,*wf_arc=nullptr;
static lv_obj_t *wf_step_arc=nullptr, *wf_step_lbl=nullptr;
static lv_timer_t *wf_tk=nullptr;
static void wf_upd(lv_timer_t*){
    struct tm tm; if(getLocalTime(&tm,5)){
        char t[8],d[24]; strftime(t,8,"%H:%M",&tm); strftime(d,24,"%a %d %b",&tm);
        if(wf_clk) lv_label_set_text(wf_clk,t);
        if(wf_dt)  lv_label_set_text(wf_dt,d);
    }
    // refresh weekly ring (in case ISO week rolled)
    int w = iso_week_now();
    if(w >= 0 && w != S.week_iso){ S.week_iso = w; S.weekly_done_km = 0; save_settings(); }
    if(wf_arc && S.weekly_goal_km > 0){
        int pct = (int)((S.weekly_done_km / (float)S.weekly_goal_km) * 100.0f);
        if(pct>100) pct=100;
        lv_arc_set_value(wf_arc, pct);
    }
    if(wf_goal){
        char b[32];
        snprintf(b, 32, "%.1f / %d %s",
                 to_user_dist(S.weekly_done_km),
                 (int)to_user_dist(S.weekly_goal_km),
                 unit_dist());
        lv_label_set_text(wf_goal,b);
    }
    // v3: daily-step ring + label, plus midnight rollover
    daily_rollover_tick();
    if(wf_step_arc && S.daily_step_goal>0){
        int pct = (int)((S.today_steps * 100.0f) / S.daily_step_goal);
        if(pct>100) pct=100;
        lv_arc_set_value(wf_step_arc, pct);
    }
    if(wf_step_lbl){
        char b[40];
        snprintf(b,40,"%d / %d steps", S.today_steps, S.daily_step_goal);
        lv_label_set_text(wf_step_lbl,b);
    }
}

// =====================================================================
//  Workout ACTIVE screen
// =====================================================================
static lv_obj_t *sm_xp_lbl=nullptr;
static void finish_workout();
static void open_summary(int xp_gain);
static void open_splits();

static void aw_tick(lv_timer_t*){
    // -- auto-pause logic (RUN/WALK/XC only; cycle/hiit not triggered) --
    if((WK.sport==SP_RUN||WK.sport==SP_WALK||WK.sport==SP_XC) && !WK.paused){
        uint32_t since = millis() - IMU.last_motion_ms;
        if(g_has_imu && since > 6000 && !WK.auto_paused){
            WK.auto_paused = true; WK.t_pause = millis();
        } else if(g_has_imu && since < 1500 && WK.auto_paused){
            WK.paused_total += millis() - WK.t_pause;
            WK.auto_paused = false;
        }
    }

    // -- pending shake-to-lap from IMU --
    if(g_imu_pending_lap){
        g_imu_pending_lap = 0;
        log_lap();
    }

    // -- auto-lap by distance --
    if(S.auto_lap_m > 0 && WK.sport != SP_HIIT){
        float target_km = (WK.lap_count+1) * (S.auto_lap_m/1000.0f);
        if(WK.km >= target_km) log_lap();
    }

    // -- HIIT engine --
    if(WK.sport == SP_HIIT && aw_phase){
        uint32_t pe = (millis() - WK.hiit_phase_start) / 1000;
        bool work  = (WK.hiit_phase % 2) == 0;
        int  dur   = work ? WK.hiit_work : WK.hiit_rest;
        if((int)pe >= dur){
            WK.hiit_phase++;
            WK.hiit_phase_start = millis();
            if(WK.hiit_phase >= WK.hiit_rounds*2){ /* session ends on FINISH press */ }
        }
        int round = WK.hiit_phase/2 + 1;
        int rem   = dur - (int)pe; if(rem<0) rem=0;
        char b[24];
        snprintf(b,24,"%s  %d/%d   %02d",
                 work?"WORK":"REST", round, WK.hiit_rounds, rem);
        lv_label_set_text(aw_phase,b);
        lv_obj_set_style_bg_color(aw_paceband,
            lv_color_hex(work?ACCENT_DANGER:ACCENT_SPORT), 0);
    }

    // -- TRAINING / INTERVALS engine --
    if(WK.mode == M_TRAINING && WK.plan == PL_INTERVAL && aw_phase){
        float target_phase_km = WK.intv_dist_m / 1000.0f;
        uint32_t phase_sec = (millis() - WK.intv_phase_start) / 1000;
        if(!WK.intv_in_recov){
            float phase_km_done = WK.km - WK.intv_phase_km;
            if(phase_km_done >= target_phase_km){
                WK.intv_in_recov   = true;
                WK.intv_phase_start= millis();
                WK.intv_phase_km   = WK.km;
                log_lap();
            }
        } else {
            if((int)phase_sec >= WK.intv_recov_s){
                WK.intv_in_recov   = false;
                WK.intv_phase_start= millis();
                WK.intv_phase_km   = WK.km;
                WK.intv_idx++;
                log_lap();
            }
        }
        char b[28];
        if(WK.intv_in_recov)
            snprintf(b,28,"RECOV  %d/%d   %02ds",
                     WK.intv_idx+1, WK.intv_reps, WK.intv_recov_s-(int)phase_sec);
        else
            snprintf(b,28,"REP %d/%d   %.0f m",
                     WK.intv_idx+1, WK.intv_reps,
                     (WK.km-WK.intv_phase_km)*1000.0f);
        lv_label_set_text(aw_phase,b);
        lv_obj_set_style_bg_color(aw_paceband,
            lv_color_hex(WK.intv_in_recov?ACCENT_TRAIN:ACCENT_RACE),0);
    }

    // -- main numbers --
    uint32_t s = elapsed_sec();
    char b[36];
    snprintf(b,36,"%02d:%02d:%02d",s/3600,(s/60)%60,s%60);
    lv_label_set_text(aw_time,b);
    snprintf(b,36,"%.2f %s", to_user_dist(WK.km), unit_dist());
    lv_label_set_text(aw_dist,b);
    snprintf(b,36,"%d steps", WK.steps);
    lv_label_set_text(aw_steps,b);
    snprintf(b,36,"%.0f kcal", estimate_calories());
    lv_label_set_text(aw_cal,b);

    int pkm = current_pace_sec_per_km();
    int pu  = pace_sec_to_user(pkm);
    if(pu>0) snprintf(b,36,"%d:%02d %s", pu/60, pu%60, unit_pace());
    else     snprintf(b,36,"-- %s", unit_pace());
    lv_label_set_text(aw_pace,b);

    // -- pace band colour (RUN-style sports) --
    if((WK.sport==SP_RUN || WK.sport==SP_XC) && pkm>0 && WK.sport!=SP_HIIT){
        int diff = pkm - WK.target_pace_sec;
        uint32_t col = diff<-15 ? ACCENT_SPORT :
                       diff< 15 ? ACCENT_CYAN  :
                       diff< 60 ? ACCENT_AMBER : ACCENT_DANGER;
        lv_obj_set_style_bg_color(aw_paceband,lv_color_hex(col),0);
    }

    // -- lap label --
    if(aw_lap){
        snprintf(b,36,"LAP %d", WK.lap_count+1);
        lv_label_set_text(aw_lap,b);
    }

    // -- auto-pause indicator --
    if(WK.auto_paused){
        lv_obj_set_style_bg_color(aw_paceband,lv_color_hex(ACCENT_AMBER),0);
    }

    // -- mode-specific sub-line --
    if(aw_extra){
        if(WK.mode == M_RACE){
            float left_km = WK.race_target_km - WK.km;
            if(left_km < 0) left_km = 0;
            int   target_sec_left = WK.race_target_sec - (int)elapsed_sec();
            if(target_sec_left < 0) target_sec_left = 0;
            snprintf(b,36,"GOAL %.2f %s  T-%02d:%02d",
                     to_user_dist(WK.race_target_km), unit_dist(),
                     target_sec_left/60, target_sec_left%60);
            lv_label_set_text(aw_extra,b);
            // virtual partner: pos_partner_km = target_km * (elapsed / target_sec)
            if(aw_partner && WK.race_target_sec > 0){
                float frac = (float)elapsed_sec() / (float)WK.race_target_sec;
                if(frac > 1.0f) frac = 1.0f;
                float ppartner = WK.race_target_km * frac;
                float delta = WK.km - ppartner;          // +: ahead, -: behind
                char p[24];
                if(delta >= 0)
                    snprintf(p,24,"+%.2f %s ahead",
                             to_user_dist(delta), unit_dist());
                else
                    snprintf(p,24,"%.2f %s behind",
                             to_user_dist(delta), unit_dist());
                lv_label_set_text(aw_partner,p);
                lv_obj_set_style_text_color(aw_partner,
                    lv_color_hex(delta>=0?ACCENT_SPORT:ACCENT_DANGER),0);
            }
        } else if(WK.mode == M_CROSS){
            const char *terrain = WK.bumpiness < 0.04f ? "smooth" :
                                  WK.bumpiness < 0.10f ? "uneven" : "technical";
            snprintf(b,36,"%s  %s",
                     WK.on_hill ? "HILL " : "FLAT ", terrain);
            lv_label_set_text(aw_extra,b);
        } else if(WK.mode == M_TRAINING){
            const char *plans[]={"EASY","TEMPO","LONG","INTERVAL","FARTLEK"};
            lv_label_set_text(aw_extra, plans[WK.plan]);
        } else {
            lv_label_set_text(aw_extra,"FREE SESSION");
        }
    }
}

static void open_workout_active(){
    lv_obj_t *s=lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s,lv_color_hex(COL_BG),0);
    lv_obj_set_style_bg_opa(s,LV_OPA_COVER,0);
    lv_obj_clear_flag(s,LV_OBJ_FLAG_SCROLLABLE);

    const SportProfile &sp = SPORTS[WK.sport];

    aw_paceband=lv_obj_create(s); lv_obj_remove_style_all(aw_paceband);
    lv_obj_set_size(aw_paceband,LCD_W,6); lv_obj_align(aw_paceband,LV_ALIGN_TOP_MID,0,0);
    lv_obj_set_style_bg_color(aw_paceband,lv_color_hex(sp.color),0);
    lv_obj_set_style_bg_opa(aw_paceband,LV_OPA_COVER,0);

    aw_hud  = mk_label(s, sp.name, &lv_font_montserrat_18, sp.color,
                       LV_ALIGN_TOP_MID, 0, 22, 4);

    aw_extra = mk_label(s, "...", &lv_font_montserrat_14, ACCENT_CYAN,
                        LV_ALIGN_TOP_MID, 0, 46, 3);

    aw_time = mk_label(s, "00:00:00", &lv_font_montserrat_48, 0xFFFFFF,
                       LV_ALIGN_TOP_MID, 0, 70);

    aw_pace = mk_label(s, "-- / km", &lv_font_montserrat_28, ACCENT_CYAN,
                       LV_ALIGN_TOP_MID, 0, 138);

    aw_dist = mk_label(s, "0.00 km", &lv_font_montserrat_22, 0xFFFFFF,
                       LV_ALIGN_TOP_MID, 0, 195);

    aw_lap   = mk_label(s, "LAP 1",   &lv_font_montserrat_14, sp.color,
                        LV_ALIGN_TOP_MID, 0, 226, 3);

    aw_steps = mk_label(s, "0 steps", &lv_font_montserrat_14, COL_MUTED2,
                        LV_ALIGN_TOP_LEFT, 40, 250);
    aw_cal   = mk_label(s, "0 kcal",  &lv_font_montserrat_14, COL_MUTED2,
                        LV_ALIGN_TOP_RIGHT, -40, 250);

    if(WK.sport == SP_HIIT){
        aw_phase = mk_label(s, "WORK  1/8   30", &lv_font_montserrat_18,
                            ACCENT_DANGER, LV_ALIGN_TOP_MID, 0, 280);
    } else if(WK.mode == M_TRAINING && WK.plan == PL_INTERVAL){
        aw_phase = mk_label(s, "REP 1/6", &lv_font_montserrat_18,
                            ACCENT_RACE, LV_ALIGN_TOP_MID, 0, 280);
    } else aw_phase = nullptr;

    // virtual partner (Race Day only)
    if(WK.mode == M_RACE){
        aw_partner = mk_label(s, "+0.00 km ahead", &lv_font_montserrat_14,
                              ACCENT_SPORT, LV_ALIGN_TOP_MID, 0, 280);
    } else aw_partner = nullptr;

    // PAUSE pill
    mk_pill(s, 120, 48, COL_CARD, ACCENT_CYAN, "PAUSE", ACCENT_CYAN,
            &lv_font_montserrat_16, LV_ALIGN_BOTTOM_LEFT, 20, -90,
            [](lv_event_t *e){
        lv_obj_t *b=(lv_obj_t*)lv_event_get_target(e);
        lv_obj_t *lbl=lv_obj_get_child(b,0);
        if(!WK.paused){
            WK.paused=true; WK.t_pause=millis();
            lv_label_set_text(lbl,"RESUME");
        } else {
            WK.paused_total += millis()-WK.t_pause; WK.paused=false;
            lv_label_set_text(lbl,"PAUSE");
        }
    });

    // LAP pill
    mk_pill(s, 100, 48, COL_CARD, sp.color, "LAP", sp.color,
            &lv_font_montserrat_16, LV_ALIGN_BOTTOM_MID, 0, -90,
            [](lv_event_t*){ log_lap(); });

    // SPLITS pill
    mk_pill(s, 100, 48, COL_CARD, ACCENT_TRAIN, "SPLITS", ACCENT_TRAIN,
            &lv_font_montserrat_14, LV_ALIGN_BOTTOM_RIGHT, -20, -90,
            [](lv_event_t*){ open_splits(); });

    // FINISH solid green button
    mk_solid_btn(s, LCD_W-60, 50, ACCENT_SPORT, "FINISH SESSION",
                 COL_BG, LV_ALIGN_BOTTOM_MID, 0, -30,
                 [](lv_event_t*){ finish_workout(); });

    if(aw_t) lv_timer_del(aw_t);
    aw_t = lv_timer_create(aw_tick, 250, NULL);
    open_app_screen(s);
}

// =====================================================================
//  Splits screen (live during workout)
// =====================================================================
static void open_splits(){
    lv_obj_t *s=lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s,lv_color_hex(COL_BG),0);
    lv_obj_set_style_bg_opa(s,LV_OPA_COVER,0);
    lv_obj_clear_flag(s,LV_OBJ_FLAG_SCROLLABLE);

    mk_label(s,"SPLITS",&lv_font_montserrat_22, ACCENT_TRAIN,
             LV_ALIGN_TOP_MID,0,40,3);

    int n = WK.lap_count; if(n>6) n=6;
    int start = WK.lap_count - n;
    char b[64];
    for(int i=0;i<n;i++){
        const Lap &L = WK.laps[start+i];
        int pu = pace_sec_to_user(L.pace_sec_per_km);
        snprintf(b,64,"%2d   %.2f %s   %d:%02d   %d:%02d %s",
                 start+i+1,
                 to_user_dist(L.km), unit_dist(),
                 (int)L.sec/60, (int)L.sec%60,
                 pu/60, pu%60, unit_pace());
        mk_label(s,b,&lv_font_montserrat_14, 0xFFFFFF,
                 LV_ALIGN_TOP_LEFT, 36, 90 + i*32, 1);
    }
    if(n==0){
        mk_label(s,"no laps yet",&lv_font_montserrat_16, COL_MUTED,
                 LV_ALIGN_CENTER,0,0);
    }

    mk_pill(s, 200, 48, COL_CARD, ACCENT_CYAN, "BACK", ACCENT_CYAN,
            &lv_font_montserrat_16, LV_ALIGN_BOTTOM_MID, 0, -30,
            [](lv_event_t*){ open_workout_active(); });
    open_app_screen(s);
}

// =====================================================================
//  Session log + summary
// =====================================================================
static void log_session_csv(){
    struct tm tm; if(!getLocalTime(&tm,5)) return;
    char path[64]; strftime(path,64,"/WATCH/workouts/%Y%m%d_%H%M%S.csv",&tm);
    File f=SD_MMC.open(path,FILE_WRITE);
    if(!f) return;
    const char *modes[] = {"FREE","TRAINING","RACE","XC"};
    f.println("# SportOS v2 session");
    f.printf ("sport,%s\n",  SPORTS[WK.sport].name);
    f.printf ("mode,%s\n",   modes[WK.mode]);
    f.printf ("duration_sec,%u\n",elapsed_sec());
    f.printf ("distance_km,%.3f\n",WK.km);
    f.printf ("steps,%d\n",WK.steps);
    f.printf ("calories,%.1f\n",estimate_calories());
    f.printf ("avg_pace_sec_per_km,%d\n", current_pace_sec_per_km());
    f.printf ("target_pace_sec,%d\n", WK.target_pace_sec);
    if(WK.mode==M_RACE){
        f.printf("race_target_km,%.3f\n",WK.race_target_km);
        f.printf("race_target_sec,%d\n",WK.race_target_sec);
    }
    if(WK.mode==M_CROSS){
        f.printf("bumpiness,%.3f\n",WK.bumpiness);
    }
    f.println("# laps");
    f.println("idx,km,sec,pace_sec_per_km");
    for(int i=0;i<WK.lap_count;i++){
        f.printf("%d,%.3f,%u,%d\n",
                 i+1, WK.laps[i].km, WK.laps[i].sec,
                 WK.laps[i].pace_sec_per_km);
    }
    f.close();
}

static void open_summary(int xp_gain){
    lv_obj_t *s=lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s,lv_color_hex(COL_BG),0);
    lv_obj_set_style_bg_opa(s,LV_OPA_COVER,0);
    lv_obj_clear_flag(s,LV_OBJ_FLAG_SCROLLABLE);

    mk_label(s,"SESSION COMPLETE",&lv_font_montserrat_22, ACCENT_SPORT,
             LV_ALIGN_TOP_MID,0,40,3);

    char b[64];
    uint32_t sec=elapsed_sec();
    snprintf(b,64,"%02d:%02d:%02d",sec/3600,(sec/60)%60,sec%60);
    mk_label(s,b,&lv_font_montserrat_48, 0xFFFFFF, LV_ALIGN_TOP_MID,0,85);

    snprintf(b,64,"%.2f %s    %d steps",
             to_user_dist(WK.km), unit_dist(), WK.steps);
    mk_label(s,b,&lv_font_montserrat_16, COL_MUTED3, LV_ALIGN_TOP_MID,0,160);

    snprintf(b,64,"+%d XP   LV %d   STREAK %d",xp_gain,P.level,P.streak);
    sm_xp_lbl=mk_label(s,b,&lv_font_montserrat_18, ACCENT_SPORT,
                       LV_ALIGN_TOP_MID,0,200,2);

    lv_obj_t *xpb=lv_bar_create(s);
    lv_obj_set_size(xpb,300,6); lv_obj_align(xpb,LV_ALIGN_TOP_MID,0,240);
    lv_obj_set_style_bg_color(xpb,lv_color_hex(COL_BORDER),LV_PART_MAIN);
    lv_obj_set_style_bg_color(xpb,lv_color_hex(ACCENT_SPORT),LV_PART_INDICATOR);
    lv_obj_set_style_radius(xpb,3,LV_PART_MAIN);
    lv_obj_set_style_radius(xpb,3,LV_PART_INDICATOR);
    lv_bar_set_range(xpb,0,xp_for_level(P.level));
    lv_bar_set_value(xpb,P.xp,LV_ANIM_OFF);

    snprintf(b,64,"%.1f / %d %s this week",
             to_user_dist(S.weekly_done_km),
             (int)to_user_dist(S.weekly_goal_km), unit_dist());
    mk_label(s,b,&lv_font_montserrat_14, COL_MUTED2, LV_ALIGN_TOP_MID,0,275);

    mk_pill(s, 220, 50, COL_CARD, ACCENT_SPORT, LV_SYMBOL_HOME "  HOME",
            ACCENT_SPORT, &lv_font_montserrat_16,
            LV_ALIGN_BOTTOM_MID, 0, -22,
            [](lv_event_t*){ open_home(); });
    open_app_screen(s);
}

static void finish_workout(){
    if(aw_t){ lv_timer_del(aw_t); aw_t=nullptr; }
    if(WK.lap_count==0 || WK.km > WK.lap_start_km + 0.01f) log_lap();

    uint32_t sec=elapsed_sec();
    int xp_gain = (int)(WK.km * SPORTS[WK.sport].xp_per_km) + (int)(sec/60)*5;
    if(WK.mode==M_RACE  && WK.km >= WK.race_target_km) xp_gain += 200;
    if(WK.mode==M_CROSS && WK.on_hill)                 xp_gain += 50;
    if(xp_gain<10) xp_gain=10;

    P.total_km += WK.km; P.total_sec += sec;
    register_session_day();
    register_weekly_km(WK.km);
    add_xp(xp_gain);
    evaluate_achievements_after_session(sec);
    save_player(); save_settings();
    log_session_csv();
    open_summary(xp_gain);
}

// =====================================================================
//  Generic "configure target pace" picker  (used by RUN/XC free session)
// =====================================================================
static lv_obj_t *pp_lbl=nullptr;
static void update_pp(){
    int pu = pace_sec_to_user(WK.target_pace_sec);
    char b[24]; snprintf(b,24,"%d:%02d %s", pu/60, pu%60, unit_pace());
    lv_label_set_text(pp_lbl,b);
}
static void start_active_now(){
    WK.t_start=millis(); WK.t_pause=0; WK.paused_total=0;
    WK.paused=false; WK.auto_paused=false;
    WK.steps=0; WK.km=0; WK.lap_count=0;
    WK.lap_start_km=0; WK.lap_start_sec=0; WK.bumpiness=0;
    WK.intv_idx=0; WK.intv_in_recov=false; WK.intv_phase_km=0;
    WK.intv_phase_start=millis();
    WK.hiit_phase=0; WK.hiit_phase_start=millis();
    IMU.last_motion_ms = millis();
    open_workout_active();
}
static void open_pace_picker(const char *title, uint32_t accent,
                             const char *start_lbl){
    lv_obj_t *s=lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s,lv_color_hex(COL_BG),0);
    lv_obj_set_style_bg_opa(s,LV_OPA_COVER,0);
    lv_obj_clear_flag(s,LV_OBJ_FLAG_SCROLLABLE);

    mk_label(s, title, &lv_font_montserrat_22, accent,
             LV_ALIGN_TOP_MID, 0, 60, 3);

    pp_lbl = mk_label(s,"", &lv_font_montserrat_48, 0xFFFFFF,
                      LV_ALIGN_CENTER, 0, -30);
    update_pp();

    mk_pill(s, 90,90, COL_CARD, accent, "-", accent,
            &lv_font_montserrat_28, LV_ALIGN_CENTER, -90, 80,
            [](lv_event_t*){
        WK.target_pace_sec = max(180, WK.target_pace_sec - 5); update_pp();
    });
    mk_pill(s, 90,90, COL_CARD, accent, "+", accent,
            &lv_font_montserrat_28, LV_ALIGN_CENTER, 90, 80,
            [](lv_event_t*){
        WK.target_pace_sec = min(900, WK.target_pace_sec + 5); update_pp();
    });

    mk_solid_btn(s, 220, 52, accent, start_lbl, COL_BG,
                 LV_ALIGN_BOTTOM_MID, 0, -22,
                 [](lv_event_t*){ start_active_now(); });
}

// =====================================================================
//  RACE DAY picker (distance + target time)
// =====================================================================
static lv_obj_t *rd_dist_lbl=nullptr, *rd_time_lbl=nullptr, *rd_pace_lbl=nullptr;
static const float RACE_DIST_KM[] = { 5.0f, 10.0f, 21.0975f, 42.195f };
static const char *RACE_DIST_NAME[] = {"5 K","10 K","HALF","FULL"};
static int rd_dist_idx = 0;

static void rd_refresh(){
    char b[24];
    snprintf(b,24,"%s  (%.2f %s)",
             RACE_DIST_NAME[rd_dist_idx],
             to_user_dist(WK.race_target_km), unit_dist());
    lv_label_set_text(rd_dist_lbl,b);

    int hh = WK.race_target_sec/3600;
    int mm = (WK.race_target_sec/60)%60;
    int ss = WK.race_target_sec%60;
    if(hh>0) snprintf(b,24,"%d:%02d:%02d", hh, mm, ss);
    else     snprintf(b,24,"%02d:%02d", mm, ss);
    lv_label_set_text(rd_time_lbl,b);

    int pkm = (WK.race_target_km>0) ? (int)(WK.race_target_sec/WK.race_target_km) : 0;
    int pu  = pace_sec_to_user(pkm);
    snprintf(b,24,"target  %d:%02d %s", pu/60, pu%60, unit_pace());
    lv_label_set_text(rd_pace_lbl,b);
}
static void open_race_day(){
    WK.sport = SP_RUN; WK.mode = M_RACE;
    WK.race_target_km  = RACE_DIST_KM[rd_dist_idx];
    if(WK.race_target_sec<300) WK.race_target_sec = 30*60;

    lv_obj_t *s=lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s,lv_color_hex(COL_BG),0);
    lv_obj_set_style_bg_opa(s,LV_OPA_COVER,0);
    lv_obj_clear_flag(s,LV_OBJ_FLAG_SCROLLABLE);

    mk_label(s,"RACE DAY",&lv_font_montserrat_22, ACCENT_RACE,
             LV_ALIGN_TOP_MID,0,40,4);

    rd_dist_lbl = mk_label(s,"",&lv_font_montserrat_18, 0xFFFFFF,
                           LV_ALIGN_TOP_MID,0,90);
    mk_pill(s,80,40,COL_CARD,ACCENT_RACE,"<",ACCENT_RACE,
            &lv_font_montserrat_22, LV_ALIGN_TOP_MID,-130,135,
            [](lv_event_t*){
        rd_dist_idx = (rd_dist_idx+3)%4;
        WK.race_target_km = RACE_DIST_KM[rd_dist_idx];
        rd_refresh();
    });
    mk_pill(s,80,40,COL_CARD,ACCENT_RACE,">",ACCENT_RACE,
            &lv_font_montserrat_22, LV_ALIGN_TOP_MID,130,135,
            [](lv_event_t*){
        rd_dist_idx = (rd_dist_idx+1)%4;
        WK.race_target_km = RACE_DIST_KM[rd_dist_idx];
        rd_refresh();
    });

    rd_time_lbl = mk_label(s,"--:--",&lv_font_montserrat_48, 0xFFFFFF,
                           LV_ALIGN_TOP_MID,0,190);
    mk_pill(s,70,70,COL_CARD,ACCENT_RACE,"-1m",ACCENT_RACE,
            &lv_font_montserrat_18, LV_ALIGN_CENTER,-100,40,
            [](lv_event_t*){
        WK.race_target_sec = max(300, WK.race_target_sec-60); rd_refresh();
    });
    mk_pill(s,70,70,COL_CARD,ACCENT_RACE,"+1m",ACCENT_RACE,
            &lv_font_montserrat_18, LV_ALIGN_CENTER,100,40,
            [](lv_event_t*){
        WK.race_target_sec = min(6*3600, WK.race_target_sec+60); rd_refresh();
    });

    rd_pace_lbl = mk_label(s,"",&lv_font_montserrat_14, ACCENT_AMBER,
                           LV_ALIGN_CENTER,0,110,2);
    rd_refresh();

    mk_solid_btn(s, 220, 52, ACCENT_RACE, "START RACE", COL_BG,
                 LV_ALIGN_BOTTOM_MID, 0, -22,
                 [](lv_event_t*){
        WK.target_pace_sec = (WK.race_target_km>0)
            ? (int)(WK.race_target_sec / WK.race_target_km)
            : 360;
        start_active_now();
    });
    open_app_screen(s);
}

// =====================================================================
//  TRAINING picker (Plan list)
// =====================================================================
static void start_training(Plan plan){
    WK.sport = SP_RUN; WK.mode = M_TRAINING; WK.plan = plan;
    switch(plan){
        case PL_EASY:    WK.target_pace_sec = 360; break;   // 6:00
        case PL_TEMPO:   WK.target_pace_sec = 300; break;   // 5:00
        case PL_LONG:    WK.target_pace_sec = 390; break;   // 6:30
        case PL_INTERVAL:
            WK.target_pace_sec = 270;                       // 4:30
            WK.intv_reps      = 6;
            WK.intv_dist_m    = 400;
            WK.intv_recov_s   = 90;
            break;
        case PL_FARTLEK: WK.target_pace_sec = 330; break;
    }
    open_pace_picker(plan==PL_INTERVAL?"INTERVAL  6 x 400 m":
                     plan==PL_TEMPO?   "TEMPO  RUN":
                     plan==PL_LONG?    "LONG  RUN":
                     plan==PL_EASY?    "EASY  RUN":
                                       "FARTLEK",
                     ACCENT_TRAIN, "START TRAINING");
}
static void open_training_picker(){
    lv_obj_t *s=lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s,lv_color_hex(COL_BG),0);
    lv_obj_set_style_bg_opa(s,LV_OPA_COVER,0);
    lv_obj_clear_flag(s,LV_OBJ_FLAG_SCROLLABLE);

    mk_label(s,"TRAINING",&lv_font_montserrat_22, ACCENT_TRAIN,
             LV_ALIGN_TOP_MID,0,26,4);
    mk_label(s,"structured plans",&lv_font_montserrat_12, COL_MUTED,
             LV_ALIGN_TOP_MID,0,60,1);

    lv_obj_t *list=lv_obj_create(s); lv_obj_remove_style_all(list);
    lv_obj_set_size(list, LCD_W-50, LCD_H-160);
    lv_obj_align(list,LV_ALIGN_CENTER,0,30);
    lv_obj_set_flex_flow(list,LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(list,12,0);
    lv_obj_set_style_pad_all(list,8,0);
    lv_obj_set_scrollbar_mode(list,LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_scroll_dir(list,LV_DIR_VER);

    mk_card(list, ACCENT_TRAIN, "E", "EASY",     "conversational pace",
            [](lv_event_t*){ if(millis()-lastSwipeMs<CLICK_AFTER_SWIPE_MS)return; start_training(PL_EASY); });
    mk_card(list, ACCENT_AMBER, "T", "TEMPO",    "comfortably hard",
            [](lv_event_t*){ if(millis()-lastSwipeMs<CLICK_AFTER_SWIPE_MS)return; start_training(PL_TEMPO); });
    mk_card(list, ACCENT_CYAN,  "L", "LONG RUN", "endurance builder",
            [](lv_event_t*){ if(millis()-lastSwipeMs<CLICK_AFTER_SWIPE_MS)return; start_training(PL_LONG); });
    mk_card(list, ACCENT_RACE,  "I", "INTERVALS","6 x 400 m / 90 s rec",
            [](lv_event_t*){ if(millis()-lastSwipeMs<CLICK_AFTER_SWIPE_MS)return; start_training(PL_INTERVAL); });
    mk_card(list, ACCENT_XC,    "F", "FARTLEK",  "play with pace",
            [](lv_event_t*){ if(millis()-lastSwipeMs<CLICK_AFTER_SWIPE_MS)return; start_training(PL_FARTLEK); });

    open_app_screen(s);
}

// =====================================================================
//  v3:  ACHIEVEMENTS  (8 badges, stored in S.ach_unlocked bitmap)
// =====================================================================
enum AchId {
    ACH_FIRST_KM    = 0,
    ACH_FIRST_5K    = 1,
    ACH_FIRST_10K   = 2,
    ACH_100KM_LIFE  = 3,
    ACH_SUB25_5K    = 4,
    ACH_STREAK_7    = 5,
    ACH_STEPS_10K   = 6,
    ACH_XC_FINISH   = 7,
    ACH__N          = 8
};
struct AchInfo { const char* name; const char* desc; uint32_t color; const char* icon; };
static const AchInfo ACH[ACH__N] = {
    {"FIRST KM",     "complete 1 km",          ACCENT_SPORT, "1"},
    {"FIRST 5K",     "finish a 5 km session",  ACCENT_CYAN,  "5"},
    {"FIRST 10K",    "finish a 10 km session", ACCENT_AMBER, "X"},
    {"CENTURY",      "100 km lifetime",        ACCENT_RACE,  "C"},
    {"SPEED DEMON",  "sub-25:00 5K",           ACCENT_DANGER,"!"},
    {"WEEK STREAK",  "7-day session streak",   0xFF2EC4,     "7"},
    {"DAILY 10K",    "10 000 steps in a day",  ACCENT_TRAIN, "S"},
    {"XC FINISHER",  "complete a XC distance", ACCENT_XC,    "X"}
};
static volatile int g_ach_toast = -1;
static void unlock_ach(int id){
    if(id<0||id>=ACH__N) return;
    if(S.ach_unlocked & (1u<<id)) return;
    S.ach_unlocked |= (1u<<id);
    g_ach_toast = id;
    save_settings();
}

// =====================================================================
//  v3:  daily rollover  (call from wf_upd)
// =====================================================================
static void daily_rollover_tick(){
    struct tm tm; if(!getLocalTime(&tm,5)) return;
    char ymd[16]; strftime(ymd,16,"%Y-%m-%d",&tm);
    if(S.today_ymd.length()==0){ S.today_ymd = ymd; return; }
    if(S.today_ymd == ymd) return;
    // new day: shift history left, drop oldest, append yesterday's total
    for(int i=0;i<6;i++) S.step_hist[i] = S.step_hist[i+1];
    S.step_hist[6]   = S.today_steps;
    S.today_steps    = 0;
    S.today_ymd      = ymd;
    save_settings();
}

// =====================================================================
//  v3:  achievement evaluation hook (called from finalize_session)
// =====================================================================
static void evaluate_achievements_after_session(int sec){
    if(WK.km >= 1.0f)              unlock_ach(ACH_FIRST_KM);
    if(WK.km >= 5.0f)              unlock_ach(ACH_FIRST_5K);
    if(WK.km >= 10.0f)             unlock_ach(ACH_FIRST_10K);
    if(P.total_km >= 100.0f)       unlock_ach(ACH_100KM_LIFE);
    if(WK.km >= 5.0f && sec <= 25*60) unlock_ach(ACH_SUB25_5K);
    if(P.streak >= 7)              unlock_ach(ACH_STREAK_7);
    if(S.today_steps >= 10000)     unlock_ach(ACH_STEPS_10K);
    if(WK.mode == M_CROSS)         unlock_ach(ACH_XC_FINISH);
}

// =====================================================================
//  v3:  Cross-Country distance picker  (3/4/5/6/8/10 km + LAPS-FREE)
//        --> 4 km is included, per athlete request
// =====================================================================
static const float XC_DIST_KM[]   = { 3.0f, 4.0f, 5.0f, 6.0f, 8.0f, 10.0f, 0.0f };
static const char* XC_DIST_NAME[] = {"3 KM","4 KM","5 KM","6 KM","8 KM","10 KM","FREE"};
static const int   XC_DIST_N      = (int)(sizeof(XC_DIST_KM)/sizeof(XC_DIST_KM[0]));
static int  xc_dist_idx = 1;   // default 4 km
static lv_obj_t *xc_dist_lbl = nullptr;

static void xc_refresh(){
    char b[32];
    if(XC_DIST_KM[xc_dist_idx] > 0)
        snprintf(b,32,"%s  (%.1f %s)",
                 XC_DIST_NAME[xc_dist_idx],
                 to_user_dist(XC_DIST_KM[xc_dist_idx]), unit_dist());
    else
        snprintf(b,32,"%s  (no target)", XC_DIST_NAME[xc_dist_idx]);
    if(xc_dist_lbl) lv_label_set_text(xc_dist_lbl,b);
}

// fwd
static void open_pace_picker(const char *title, uint32_t accent, const char *start_lbl);
static void start_xc(){
    WK.sport = SP_XC; WK.mode = M_CROSS;
    // open custom distance picker
    lv_obj_t *s=lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s,lv_color_hex(COL_BG),0);
    lv_obj_set_style_bg_opa(s,LV_OPA_COVER,0);
    lv_obj_clear_flag(s,LV_OBJ_FLAG_SCROLLABLE);

    mk_label(s,"CROSS COUNTRY",&lv_font_montserrat_22, ACCENT_XC,
             LV_ALIGN_TOP_MID,0,32,4);
    mk_label(s,"choose race distance",&lv_font_montserrat_12, COL_MUTED,
             LV_ALIGN_TOP_MID,0,66,2);

    xc_dist_lbl = mk_label(s,"",&lv_font_montserrat_28, 0xFFFFFF,
                           LV_ALIGN_TOP_MID,0,120);
    xc_refresh();

    mk_pill(s,90,90,COL_CARD,ACCENT_XC,"<",ACCENT_XC,
            &lv_font_montserrat_28, LV_ALIGN_CENTER,-110,0,
            [](lv_event_t*){
        xc_dist_idx = (xc_dist_idx + XC_DIST_N - 1) % XC_DIST_N;
        xc_refresh();
    });
    mk_pill(s,90,90,COL_CARD,ACCENT_XC,">",ACCENT_XC,
            &lv_font_montserrat_28, LV_ALIGN_CENTER,110,0,
            [](lv_event_t*){
        xc_dist_idx = (xc_dist_idx + 1) % XC_DIST_N;
        xc_refresh();
    });

    mk_label(s, "hills · bumpiness · shake to lap",
             &lv_font_montserrat_12, COL_MUTED2,
             LV_ALIGN_CENTER,0,80,2);

    mk_solid_btn(s, 240, 54, ACCENT_XC, "START XC", COL_BG,
                 LV_ALIGN_BOTTOM_MID, 0, -22,
                 [](lv_event_t*){
        // commit XC distance choice and jump straight in
        WK.race_target_km  = XC_DIST_KM[xc_dist_idx];     // 0 means free
        WK.target_pace_sec = 420;
        start_active_now();
    });
    open_app_screen(s);
}

// =====================================================================
//  Free Session picker (RUN/WALK/CYCLE/HIIT — preserves v1 behaviour)
// =====================================================================
static void start_free(Sport sp){
    WK.sport = sp; WK.mode = M_FREE;
    if(sp == SP_RUN){
        open_pace_picker("FREE RUN", ACCENT_SPORT, "START RUN");
    } else {
        start_active_now();
    }
}
static void open_free_picker(){
    lv_obj_t *s=lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s,lv_color_hex(COL_BG),0);
    lv_obj_set_style_bg_opa(s,LV_OPA_COVER,0);
    lv_obj_clear_flag(s,LV_OBJ_FLAG_SCROLLABLE);

    mk_label(s,"FREE SESSION",&lv_font_montserrat_22, ACCENT_SPORT,
             LV_ALIGN_TOP_MID,0,26,4);
    mk_label(s,"untimed, no targets",&lv_font_montserrat_12, COL_MUTED,
             LV_ALIGN_TOP_MID,0,60,1);

    lv_obj_t *list=lv_obj_create(s); lv_obj_remove_style_all(list);
    lv_obj_set_size(list, LCD_W-50, LCD_H-160);
    lv_obj_align(list,LV_ALIGN_CENTER,0,30);
    lv_obj_set_flex_flow(list,LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(list,12,0);
    lv_obj_set_style_pad_all(list,8,0);
    lv_obj_set_scrollbar_mode(list,LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_scroll_dir(list,LV_DIR_VER);

    mk_card(list, SPORTS[SP_RUN].color,  SPORTS[SP_RUN].letter,
            "RUN",   "real-time pace + zones",
            [](lv_event_t*){ if(millis()-lastSwipeMs<CLICK_AFTER_SWIPE_MS)return; start_free(SP_RUN); });
    mk_card(list, SPORTS[SP_WALK].color, SPORTS[SP_WALK].letter,
            "WALK",  "casual cadence",
            [](lv_event_t*){ if(millis()-lastSwipeMs<CLICK_AFTER_SWIPE_MS)return; start_free(SP_WALK); });
    mk_card(list, SPORTS[SP_CYCLE].color,SPORTS[SP_CYCLE].letter,
            "CYCLE", "distance + calories",
            [](lv_event_t*){ if(millis()-lastSwipeMs<CLICK_AFTER_SWIPE_MS)return; start_free(SP_CYCLE); });
    mk_card(list, SPORTS[SP_HIIT].color, SPORTS[SP_HIIT].letter,
            "HIIT",  "interval timer",
            [](lv_event_t*){ if(millis()-lastSwipeMs<CLICK_AFTER_SWIPE_MS)return; start_free(SP_HIIT); });

    open_app_screen(s);
}

// =====================================================================
//  Stats screen
// =====================================================================
static void open_stats(){
    lv_obj_t *s=lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s,lv_color_hex(COL_BG),0);
    lv_obj_set_style_bg_opa(s,LV_OPA_COVER,0);
    lv_obj_clear_flag(s,LV_OBJ_FLAG_SCROLLABLE);

    mk_label(s,"HUNTER STATS",&lv_font_montserrat_22, ACCENT_SPORT,
             LV_ALIGN_TOP_MID,0,40,3);

    char b[64];
    snprintf(b,64,"LEVEL %d", P.level);
    mk_label(s,b,&lv_font_montserrat_48, 0xFFFFFF, LV_ALIGN_TOP_MID,0,90);

    snprintf(b,64,"%d / %d XP",P.xp,xp_for_level(P.level));
    mk_label(s,b,&lv_font_montserrat_16, ACCENT_CYAN, LV_ALIGN_TOP_MID,0,160);

    lv_obj_t *xb=lv_bar_create(s);
    lv_obj_set_size(xb,300,8); lv_obj_align(xb,LV_ALIGN_TOP_MID,0,190);
    lv_obj_set_style_bg_color(xb,lv_color_hex(COL_BORDER),LV_PART_MAIN);
    lv_obj_set_style_bg_color(xb,lv_color_hex(ACCENT_SPORT),LV_PART_INDICATOR);
    lv_obj_set_style_radius(xb,4,LV_PART_MAIN);
    lv_obj_set_style_radius(xb,4,LV_PART_INDICATOR);
    lv_bar_set_range(xb,0,xp_for_level(P.level));
    lv_bar_set_value(xb,P.xp,LV_ANIM_OFF);

    snprintf(b,64,"%.2f %s total", to_user_dist(P.total_km), unit_dist());
    mk_label(s,b,&lv_font_montserrat_18, COL_MUTED3, LV_ALIGN_TOP_MID,0,215);

    snprintf(b,64,"%d:%02d:%02d total time",
             P.total_sec/3600,(P.total_sec/60)%60,P.total_sec%60);
    mk_label(s,b,&lv_font_montserrat_14, COL_MUTED2, LV_ALIGN_TOP_MID,0,250);

    snprintf(b,64,"STREAK %d days",P.streak);
    mk_label(s,b,&lv_font_montserrat_18, ACCENT_AMBER, LV_ALIGN_TOP_MID,0,280,2);

    snprintf(b,64,"WEEK %.1f / %d %s",
             to_user_dist(S.weekly_done_km),
             (int)to_user_dist(S.weekly_goal_km), unit_dist());
    mk_label(s,b,&lv_font_montserrat_14, ACCENT_SPORT, LV_ALIGN_TOP_MID,0,315,2);

    // v3: today's steps + 7-day mini bar chart
    snprintf(b,64,"TODAY  %d / %d steps", S.today_steps, S.daily_step_goal);
    mk_label(s,b,&lv_font_montserrat_14, ACCENT_CYAN, LV_ALIGN_TOP_MID,0,340,2);
    // 7-day bars (today highlighted on the right, cyan)
    int bx0 = (LCD_W-180)/2;
    int by  = 360;
    int peak = 1;
    for(int i=0;i<7;i++) if(S.step_hist[i]>peak) peak=S.step_hist[i];
    if(S.today_steps>peak) peak=S.today_steps;
    for(int i=0;i<8;i++){
        int val = (i<7) ? S.step_hist[i] : S.today_steps;
        int h = peak>0 ? (val*22)/peak : 0;
        if(h<2 && val>0) h=2;
        lv_obj_t *bar = lv_obj_create(s); lv_obj_remove_style_all(bar);
        lv_obj_set_size(bar, 14, h>0?h:1);
        lv_obj_set_pos(bar, bx0 + i*22, by + (24-h));
        lv_obj_set_style_bg_color(bar,
            lv_color_hex(i==7?ACCENT_CYAN:COL_MUTED2), 0);
        lv_obj_set_style_bg_opa(bar, i==7?LV_OPA_COVER:160, 0);
        lv_obj_set_style_radius(bar, 2, 0);
    }

    // v3: achievements row (8 dots, lit if unlocked) - placed above HOME pill
    int abx0 = (LCD_W-8*32)/2;
    int aby  = 395;
    for(int i=0;i<ACH__N;i++){
        bool got = (S.ach_unlocked & (1u<<i))!=0;
        lv_obj_t *dot = lv_obj_create(s); lv_obj_remove_style_all(dot);
        lv_obj_set_size(dot, 26,26);
        lv_obj_set_pos(dot, abx0+i*32, aby);
        lv_obj_set_style_radius(dot,13,0);
        lv_obj_set_style_bg_color(dot, lv_color_hex(got?ACH[i].color:0x111111),0);
        lv_obj_set_style_bg_opa(dot, LV_OPA_COVER,0);
        lv_obj_set_style_border_color(dot, lv_color_hex(got?ACH[i].color:COL_BORDER),0);
        lv_obj_set_style_border_width(dot,1,0);
        lv_obj_t *l = lv_label_create(dot); lv_label_set_text(l, ACH[i].icon);
        lv_obj_set_style_text_color(l, lv_color_hex(got?COL_BG:COL_MUTED),0);
        lv_obj_set_style_text_font(l, &lv_font_montserrat_12,0); lv_obj_center(l);
    }

    mk_pill(s, 220, 50, COL_CARD, ACCENT_CYAN, LV_SYMBOL_LEFT "  HOME",
            ACCENT_CYAN, &lv_font_montserrat_16,
            LV_ALIGN_BOTTOM_MID, 0, -22,
            [](lv_event_t*){ open_home(); });
    open_app_screen(s);
}

// =====================================================================
//  Settings screen
// =====================================================================
static lv_obj_t *set_unit_lbl=nullptr,*set_wt_lbl=nullptr,*set_goal_lbl=nullptr,*set_lap_lbl=nullptr;
static void set_refresh(){
    char b[24];
    lv_label_set_text(set_unit_lbl, S.use_miles ? "MILES" : "KILOMETRES");
    snprintf(b,24,"%d kg", S.weight_kg);              lv_label_set_text(set_wt_lbl,  b);
    snprintf(b,24,"%d %s/wk",
             (int)to_user_dist(S.weekly_goal_km), unit_dist());
    lv_label_set_text(set_goal_lbl,b);
    snprintf(b,24,"%d m", S.auto_lap_m);              lv_label_set_text(set_lap_lbl, b);
}
static void open_settings(){
    lv_obj_t *s=lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s,lv_color_hex(COL_BG),0);
    lv_obj_set_style_bg_opa(s,LV_OPA_COVER,0);
    lv_obj_clear_flag(s,LV_OBJ_FLAG_SCROLLABLE);

    mk_label(s,"SETTINGS",&lv_font_montserrat_22, ACCENT_CYAN,
             LV_ALIGN_TOP_MID,0,30,4);

    auto row = [&](int y, const char *name, uint32_t accent,
                   lv_event_cb_t cb_minus, lv_event_cb_t cb_plus,
                   lv_obj_t **lbl_out){
        mk_label(s,name,&lv_font_montserrat_14, COL_MUTED,
                 LV_ALIGN_TOP_LEFT,30,y,2);
        *lbl_out = mk_label(s,"-",&lv_font_montserrat_18, 0xFFFFFF,
                            LV_ALIGN_TOP_MID,0,y);
        mk_pill(s,52,38,COL_CARD,accent,"-",accent,
                &lv_font_montserrat_18, LV_ALIGN_TOP_RIGHT,-90,y-8, cb_minus);
        mk_pill(s,52,38,COL_CARD,accent,"+",accent,
                &lv_font_montserrat_18, LV_ALIGN_TOP_RIGHT,-30,y-8, cb_plus);
    };

    row(82,  "UNITS",     ACCENT_CYAN,
        [](lv_event_t*){ S.use_miles=!S.use_miles; save_settings(); set_refresh(); },
        [](lv_event_t*){ S.use_miles=!S.use_miles; save_settings(); set_refresh(); },
        &set_unit_lbl);
    row(140, "WEIGHT",    ACCENT_SPORT,
        [](lv_event_t*){ S.weight_kg=max(30, S.weight_kg-1); save_settings(); set_refresh(); },
        [](lv_event_t*){ S.weight_kg=min(200,S.weight_kg+1); save_settings(); set_refresh(); },
        &set_wt_lbl);
    row(198, "WEEKLY GOAL", ACCENT_TRAIN,
        [](lv_event_t*){ S.weekly_goal_km=max(1, S.weekly_goal_km-1); save_settings(); set_refresh(); },
        [](lv_event_t*){ S.weekly_goal_km=min(500,S.weekly_goal_km+1); save_settings(); set_refresh(); },
        &set_goal_lbl);
    row(256, "AUTO LAP",  ACCENT_AMBER,
        [](lv_event_t*){ S.auto_lap_m=max(0,   S.auto_lap_m-100); save_settings(); set_refresh(); },
        [](lv_event_t*){ S.auto_lap_m=min(5000,S.auto_lap_m+100); save_settings(); set_refresh(); },
        &set_lap_lbl);
    set_refresh();

    mk_pill(s, 220, 50, COL_CARD, ACCENT_CYAN, LV_SYMBOL_LEFT "  HOME",
            ACCENT_CYAN, &lv_font_montserrat_16,
            LV_ALIGN_BOTTOM_MID, 0, -22,
            [](lv_event_t*){ open_home(); });
    open_app_screen(s);
}

// =====================================================================
//  Home (apps grid) — grouped: Modes + Tools
// =====================================================================
static void open_home(){
    if(!g_apps_built) build_screen_apps();
    goto_screen(SCREEN_APPS);
}

static void build_screen_apps(){
    lv_obj_t *s=lv_obj_create(NULL);
    screens[SCREEN_APPS] = s;
    lv_obj_set_style_bg_color(s,lv_color_hex(COL_BG),0);
    lv_obj_set_style_bg_opa(s,LV_OPA_COVER,0);
    lv_obj_clear_flag(s,LV_OBJ_FLAG_SCROLLABLE);

    mk_label(s,"SPORT//OS",&lv_font_montserrat_22, ACCENT_SPORT,
             LV_ALIGN_TOP_MID,0,26,4);
    mk_label(s, LV_SYMBOL_LEFT "  watch face   ·   battery  " LV_SYMBOL_RIGHT,
             &lv_font_montserrat_12, 0x444444, LV_ALIGN_TOP_MID,0,60,1);

    lv_obj_t *list=lv_obj_create(s); lv_obj_remove_style_all(list);
    lv_obj_set_size(list, LCD_W-50, LCD_H-160);
    lv_obj_align(list,LV_ALIGN_CENTER,0,30);
    lv_obj_set_flex_flow(list,LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(list,12,0);
    lv_obj_set_style_pad_all(list,8,0);
    lv_obj_set_scrollbar_mode(list,LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_scroll_dir(list,LV_DIR_VER);

    // MODES
    mk_card(list, ACCENT_RACE,  "R", "RACE DAY",
            "target distance + virtual partner",
            [](lv_event_t*){ if(millis()-lastSwipeMs<CLICK_AFTER_SWIPE_MS)return; open_race_day(); });
    mk_card(list, ACCENT_XC,    "X", "CROSS COUNTRY",
            "3/4/5/6/8/10 km · hills · bumpiness",
            [](lv_event_t*){ if(millis()-lastSwipeMs<CLICK_AFTER_SWIPE_MS)return; start_xc(); });
    mk_card(list, ACCENT_TRAIN, "T", "TRAINING",
            "easy / tempo / long / intervals / fartlek",
            [](lv_event_t*){ if(millis()-lastSwipeMs<CLICK_AFTER_SWIPE_MS)return; open_training_picker(); });
    mk_card(list, ACCENT_SPORT, "F", "FREE SESSION",
            "run · walk · cycle · hiit",
            [](lv_event_t*){ if(millis()-lastSwipeMs<CLICK_AFTER_SWIPE_MS)return; open_free_picker(); });

    // TOOLS
    mk_card(list, 0xFF2EC4,    "S", "STATS",
            "XP · streak · sessions",
            [](lv_event_t*){ if(millis()-lastSwipeMs<CLICK_AFTER_SWIPE_MS)return; open_stats(); });
    mk_card(list, ACCENT_CYAN,  "G", "SETTINGS",
            "units · weight · goal · auto-lap",
            [](lv_event_t*){ if(millis()-lastSwipeMs<CLICK_AFTER_SWIPE_MS)return; open_settings(); });

    g_apps_built = true;
}

// =====================================================================
//  Watch face (with weekly km ring)
// =====================================================================
static void build_screen_face(){
    lv_obj_t *s=lv_obj_create(NULL);
    screens[SCREEN_FACE] = s;
    lv_obj_set_style_bg_color(s,lv_color_hex(COL_BG),0);
    lv_obj_set_style_bg_opa(s,LV_OPA_COVER,0);
    lv_obj_clear_flag(s,LV_OBJ_FLAG_SCROLLABLE);
    make_wallpaper_bg(s);

    // Weekly goal ring
    wf_arc = lv_arc_create(s);
    lv_obj_set_size(wf_arc, 360, 360);
    lv_obj_align(wf_arc, LV_ALIGN_CENTER, 0, 0);
    lv_arc_set_rotation(wf_arc, 135);
    lv_arc_set_bg_angles(wf_arc, 0, 270);
    lv_arc_set_range(wf_arc, 0, 100);
    lv_arc_set_value(wf_arc, 0);
    lv_obj_remove_style(wf_arc, NULL, LV_PART_KNOB);
    lv_obj_clear_flag(wf_arc, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_arc_color(wf_arc, lv_color_hex(0x101010), LV_PART_MAIN);
    lv_obj_set_style_arc_width(wf_arc, 5, LV_PART_MAIN);
    lv_obj_set_style_arc_color(wf_arc, lv_color_hex(ACCENT_SPORT), LV_PART_INDICATOR);
    lv_obj_set_style_arc_width(wf_arc, 5, LV_PART_INDICATOR);
    lv_obj_set_style_arc_opa(wf_arc, 200, LV_PART_INDICATOR);

    wf_clk = mk_label(s, "--:--",   &lv_font_montserrat_48, 0xFFFFFF,
                      LV_ALIGN_CENTER, 0, -30);
    lv_obj_set_style_text_opa(wf_clk, 240, 0);
    wf_dt  = mk_label(s, "no sync", &lv_font_montserrat_18, COL_MUTED3,
                      LV_ALIGN_CENTER, 0, 20);
    wf_goal= mk_label(s, "0 / 0 km",&lv_font_montserrat_14, ACCENT_SPORT,
                      LV_ALIGN_CENTER, 0, 60, 2);

    // v3: inner daily-step ring + label
    wf_step_arc = lv_arc_create(s);
    lv_obj_set_size(wf_step_arc, 290, 290);
    lv_obj_align(wf_step_arc, LV_ALIGN_CENTER, 0, 0);
    lv_arc_set_rotation(wf_step_arc, 135);
    lv_arc_set_bg_angles(wf_step_arc, 0, 270);
    lv_arc_set_range(wf_step_arc, 0, 100);
    lv_arc_set_value(wf_step_arc, 0);
    lv_obj_remove_style(wf_step_arc, NULL, LV_PART_KNOB);
    lv_obj_clear_flag(wf_step_arc, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_arc_color(wf_step_arc, lv_color_hex(0x0A0A0A), LV_PART_MAIN);
    lv_obj_set_style_arc_width(wf_step_arc, 3, LV_PART_MAIN);
    lv_obj_set_style_arc_color(wf_step_arc, lv_color_hex(ACCENT_CYAN), LV_PART_INDICATOR);
    lv_obj_set_style_arc_width(wf_step_arc, 3, LV_PART_INDICATOR);
    lv_obj_set_style_arc_opa(wf_step_arc, 200, LV_PART_INDICATOR);

    wf_step_lbl = mk_label(s, "0 / 0 steps", &lv_font_montserrat_12,
                           ACCENT_CYAN, LV_ALIGN_CENTER, 0, 85, 2);

    mk_label(s, "swipe " LV_SYMBOL_RIGHT " for apps",
             &lv_font_montserrat_12, COL_MUTED, LV_ALIGN_BOTTOM_MID, 0, -40, 2);

    if(wf_tk) lv_timer_del(wf_tk);
    wf_tk = lv_timer_create(wf_upd, 1000, NULL);
    wf_upd(nullptr);
}

// =====================================================================
//  Battery screen (unchanged behaviour)
// =====================================================================
static lv_obj_t *batt_arc=nullptr,*batt_pct_lbl=nullptr;
static lv_obj_t *batt_state=nullptr,*batt_remain=nullptr,*batt_volt=nullptr;

static int batt_remain_min_estimate(int pct){ if(pct<=0) return 0; return (pct*360)/100; }
static void update_battery_screen(){
    if(!batt_arc || !g_has_pmu) return;
    int pct=g_pmu.getBatteryPercent();
    if(pct<0) pct=0; if(pct>100) pct=100;
    lv_arc_set_value(batt_arc,pct);
    char b[16]; snprintf(b,16,"%d%%",pct); lv_label_set_text(batt_pct_lbl,b);
    uint32_t col = pct<=15 ? ACCENT_DANGER : pct<=30 ? ACCENT_AMBER : ACCENT_SPORT;
    lv_obj_set_style_arc_color(batt_arc,lv_color_hex(col),LV_PART_INDICATOR);
    lv_obj_set_style_text_color(batt_pct_lbl,lv_color_hex(col),0);
    bool charging=g_pmu.isCharging();
    if(charging){
        lv_label_set_text(batt_state,LV_SYMBOL_CHARGE "  charging");
        lv_obj_set_style_text_color(batt_state,lv_color_hex(ACCENT_CYAN),0);
        lv_label_set_text(batt_remain,"");
    } else {
        lv_label_set_text(batt_state,"on battery");
        lv_obj_set_style_text_color(batt_state,lv_color_hex(COL_MUTED3),0);
        int mins=batt_remain_min_estimate(pct);
        char r[48]; int h=mins/60, m=mins%60;
        if(h>0) snprintf(r,48,"~ %d h %02d min remaining",h,m);
        else    snprintf(r,48,"~ %d min remaining",m);
        lv_label_set_text(batt_remain,r);
    }
    float mv=g_pmu.getBattVoltage();
    char v[32]; snprintf(v,32,"%.2f V",mv/1000.0f);
    lv_label_set_text(batt_volt,v);
}
static void build_screen_battery(){
    lv_obj_t *s=lv_obj_create(NULL); screens[SCREEN_BATTERY]=s;
    lv_obj_set_style_bg_color(s,lv_color_hex(COL_BG),0);
    lv_obj_set_style_bg_opa(s,LV_OPA_COVER,0);
    lv_obj_clear_flag(s,LV_OBJ_FLAG_SCROLLABLE);

    mk_label(s,"Battery",&lv_font_montserrat_22, 0xFFFFFF,
             LV_ALIGN_TOP_LEFT,24,22);
    mk_label(s,"AXP2101",&lv_font_montserrat_14, ACCENT_CYAN,
             LV_ALIGN_TOP_LEFT,24,50);

    batt_arc=lv_arc_create(s);
    lv_obj_set_size(batt_arc,280,280); lv_obj_align(batt_arc,LV_ALIGN_CENTER,0,-10);
    lv_arc_set_rotation(batt_arc,135); lv_arc_set_bg_angles(batt_arc,0,270);
    lv_arc_set_range(batt_arc,0,100); lv_arc_set_value(batt_arc,0);
    lv_obj_remove_style(batt_arc,NULL,LV_PART_KNOB);
    lv_obj_clear_flag(batt_arc,LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_arc_color(batt_arc,lv_color_hex(COL_CARD),LV_PART_MAIN);
    lv_obj_set_style_arc_width(batt_arc,14,LV_PART_MAIN);
    lv_obj_set_style_arc_color(batt_arc,lv_color_hex(ACCENT_SPORT),LV_PART_INDICATOR);
    lv_obj_set_style_arc_width(batt_arc,14,LV_PART_INDICATOR);

    batt_pct_lbl = mk_label(s,"--%",&lv_font_montserrat_48, 0xFFFFFF,
                            LV_ALIGN_CENTER,0,-10);
    batt_state   = mk_label(s,"on battery",&lv_font_montserrat_18, COL_MUTED3,
                            LV_ALIGN_CENTER,0,90);
    batt_remain  = mk_label(s,"",&lv_font_montserrat_16, 0xFFFFFF,
                            LV_ALIGN_CENTER,0,125);
    batt_volt    = mk_label(s,"",&lv_font_montserrat_14, COL_MUTED,
                            LV_ALIGN_CENTER,0,160);

    lv_obj_t *sleepBtn=lv_btn_create(s);
    lv_obj_set_size(sleepBtn,175,44); lv_obj_align(sleepBtn,LV_ALIGN_BOTTOM_RIGHT,-15,-40);
    lv_obj_set_style_radius(sleepBtn,22,0);
    lv_obj_set_style_bg_color(sleepBtn,lv_color_hex(COL_CARD),0);
    lv_obj_set_style_border_color(sleepBtn,lv_color_hex(ACCENT_CYAN),0);
    lv_obj_set_style_border_width(sleepBtn,1,0);
    lv_obj_add_event_cb(sleepBtn,[](lv_event_t*){
        if(millis()-lastSwipeMs<CLICK_AFTER_SWIPE_MS) return;
        static_cast<Arduino_CO5300*>(gfx)->setBrightness(0);
        esp_sleep_enable_ext1_wakeup((1ULL<<BOOT_BTN), ESP_EXT1_WAKEUP_ANY_LOW);
        esp_deep_sleep_start();
    },LV_EVENT_CLICKED,NULL);
    mk_label(sleepBtn, LV_SYMBOL_POWER "  Sleep Now",
             &lv_font_montserrat_16, ACCENT_CYAN, LV_ALIGN_CENTER,0,0);

    lv_obj_t *launchBtn=lv_btn_create(s);
    lv_obj_set_size(launchBtn,175,44); lv_obj_align(launchBtn,LV_ALIGN_BOTTOM_LEFT,15,-40);
    lv_obj_set_style_radius(launchBtn,22,0);
    lv_obj_set_style_bg_color(launchBtn,lv_color_hex(COL_CARD),0);
    lv_obj_set_style_border_color(launchBtn,lv_color_hex(ACCENT_SPORT),0);
    lv_obj_set_style_border_width(launchBtn,1,0);
    lv_obj_add_event_cb(launchBtn,[](lv_event_t*){
        if(millis()-lastSwipeMs<CLICK_AFTER_SWIPE_MS) return;
        return_to_launcher();
    },LV_EVENT_CLICKED,NULL);
    mk_label(launchBtn, LV_SYMBOL_HOME "  Launcher",
             &lv_font_montserrat_16, ACCENT_SPORT, LV_ALIGN_CENTER,0,0);

    mk_label(s, LV_SYMBOL_LEFT "  apps   ·   watch face  " LV_SYMBOL_RIGHT,
             &lv_font_montserrat_12, 0x444444, LV_ALIGN_BOTTOM_MID,0,-10);
}

// =====================================================================
//  Swipe / screen routing
// =====================================================================
static void lv_touch_gesture(lv_indev_drv_t *, lv_indev_data_t *d){
    int32_t x=FT3168->IIC_Read_Device_Value(FT3168->Arduino_IIC_Touch::Value_Information::TOUCH_COORDINATE_X);
    int32_t y=FT3168->IIC_Read_Device_Value(FT3168->Arduino_IIC_Touch::Value_Information::TOUCH_COORDINATE_Y);
    int     f=FT3168->IIC_Read_Device_Value(FT3168->Arduino_IIC_Touch::Value_Information::TOUCH_FINGER_NUMBER);
    bool pressed = (f>0);
    static int zero_streak = 0;
    if(pressed) zero_streak = 0;
    else if(touchActive){
        zero_streak++;
        if(zero_streak < 3){ pressed=true; x=tc_x; y=tc_y; }
    }
    if(pressed){
        d->state = LV_INDEV_STATE_PR;
        d->point.x = x; d->point.y = y;
        if(!touchActive){ touchActive=true; ts_x=x; ts_y=y; ts_ms=millis(); }
        tc_x = x; tc_y = y;
    } else {
        d->state = LV_INDEV_STATE_REL;
        if(touchActive){
            touchActive = false;
            uint32_t dur = millis() - ts_ms;
            int32_t dx = tc_x - ts_x;
            int32_t dy = tc_y - ts_y;
            if(dur < SWIPE_MAX_DURATION_MS &&
               abs(dx) > SWIPE_THRESHOLD_PX && abs(dx) > abs(dy)){
                g_pending_swipe = (dx < 0) ? +1 : -1;
            }
        }
    }
}
static void goto_screen(int s){
    while(s < 0)            s += NUM_SCREENS;
    while(s >= NUM_SCREENS) s -= NUM_SCREENS;
    if(s == current_screen && g_transient_screen == nullptr) return;
    current_screen  = s;
    g_pending_swipe = 0;
    // If we are leaving a transient (custom) screen, let lv_scr_load_anim
    // delete it AFTER the fade completes (auto_del=true).  Deleting it
    // ourselves via lv_obj_del_async while the fade still references the
    // "from" screen causes a use-after-free and reboots the watch.
    bool had_transient = (g_transient_screen != nullptr);
    g_transient_screen = nullptr;
    if(screens[s])
        lv_scr_load_anim(screens[s], LV_SCR_LOAD_ANIM_FADE_ON,
                         SCREEN_SWITCH_ANIM_MS, 0, had_transient);
}
static void open_app_screen(lv_obj_t *s){
    // CRITICAL: pass auto_del=true when there is a previous transient screen
    // so LVGL deletes it AFTER the fade animation completes.  Doing
    // lv_obj_del_async(prev) ourselves runs at the start of the next
    // lv_timer_handler() pass — while the fade still holds a pointer to the
    // "from" screen — and crashes/reboots the watch on every workout start.
    lv_obj_t *prev = g_transient_screen;
    g_transient_screen = s;
    lv_scr_load_anim(s, LV_SCR_LOAD_ANIM_FADE_ON, 250, 0, prev != nullptr);
}

// =====================================================================
//  v3:  achievement toast  (pops a small badge top-of-screen for ~2.5 s)
// =====================================================================
static void show_ach_toast(int id){
    if(id<0||id>=ACH__N) return;
    const AchInfo &a = ACH[id];
    lv_obj_t *box = lv_obj_create(lv_scr_act());
    lv_obj_remove_style_all(box);
    lv_obj_set_size(box, 320, 64);
    lv_obj_align(box, LV_ALIGN_TOP_MID, 0, 12);
    lv_obj_set_style_bg_color(box, lv_color_hex(0x111111),0);
    lv_obj_set_style_bg_opa(box, 240,0);
    lv_obj_set_style_radius(box, 18,0);
    lv_obj_set_style_border_color(box, lv_color_hex(a.color),0);
    lv_obj_set_style_border_width(box, 2,0);
    lv_obj_clear_flag(box, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *dot = lv_obj_create(box); lv_obj_remove_style_all(dot);
    lv_obj_set_size(dot, 36,36); lv_obj_align(dot, LV_ALIGN_LEFT_MID, 12, 0);
    lv_obj_set_style_radius(dot,18,0);
    lv_obj_set_style_bg_color(dot, lv_color_hex(a.color),0);
    lv_obj_set_style_bg_opa(dot, 60,0);
    lv_obj_set_style_border_color(dot, lv_color_hex(a.color),0);
    lv_obj_set_style_border_width(dot,1,0);
    lv_obj_t *dl = lv_label_create(dot); lv_label_set_text(dl, a.icon);
    lv_obj_set_style_text_color(dl, lv_color_hex(a.color),0);
    lv_obj_set_style_text_font(dl, &lv_font_montserrat_18,0); lv_obj_center(dl);

    lv_obj_t *t1=lv_label_create(box); lv_label_set_text(t1,"ACHIEVEMENT UNLOCKED");
    lv_obj_set_style_text_color(t1, lv_color_hex(COL_MUTED2),0);
    lv_obj_set_style_text_font(t1, &lv_font_montserrat_12,0);
    lv_obj_set_style_text_letter_space(t1, 3, 0);
    lv_obj_align(t1, LV_ALIGN_TOP_LEFT, 60, 10);

    lv_obj_t *t2=lv_label_create(box); lv_label_set_text(t2, a.name);
    lv_obj_set_style_text_color(t2, lv_color_hex(a.color),0);
    lv_obj_set_style_text_font(t2, &lv_font_montserrat_18,0);
    lv_obj_align(t2, LV_ALIGN_BOTTOM_LEFT, 60, -8);

    lv_timer_t *tt = lv_timer_create([](lv_timer_t *t){
        lv_obj_t *o = (lv_obj_t*)t->user_data;
        if(o && lv_obj_is_valid(o)) lv_obj_del_async(o);
        lv_timer_del(t);
    }, 2500, NULL);
    tt->user_data = box;
}


// =====================================================================
//  Boot / loop
// =====================================================================
void setup(){
    hw_boot();
    splash_show();
    load_player();
    load_settings();
    build_screen_face();
    build_screen_apps();
    build_screen_battery();
    current_screen = SCREEN_FACE;
    lv_scr_load_anim(screens[SCREEN_FACE], LV_SCR_LOAD_ANIM_FADE_ON, 260, 0, false);
}
void loop(){
    lv_timer_handler();
    imu_tick();
    uint32_t now = millis();

    if(g_pending_swipe != 0 && g_transient_screen == nullptr){
        int ev = g_pending_swipe; g_pending_swipe = 0;
        lastSwipeMs = now;
        goto_screen(current_screen + ev);
    }

    static uint32_t tB=0;
    if(now-tB > 1000){
        tB = now;
        if(current_screen == SCREEN_BATTERY) update_battery_screen();
    }

    static uint32_t t1=0;
    if(now-t1 > 2000){
        t1 = now;
        if(g_transient_screen) tick_top();
    }

    // v3: drain pending achievement toast (set from save context, rendered here)
    if(g_ach_toast >= 0){
        int id = g_ach_toast; g_ach_toast = -1;
        show_ach_toast(id);
    }

    // v3: periodic save of daily step counter (every 60 s while awake)
    static uint32_t t_save = 0;
    if(now - t_save > 60000){
        t_save = now;
        save_settings();
        // also unlock daily-10k badge live, not just post-session
        if(S.today_steps >= 10000) unlock_ach(ACH_STEPS_10K);
    }

    // BOOT long-press -> Launcher
    static uint32_t btn_t = 0;
    if(digitalRead(BOOT_BTN) == LOW){
        if(!btn_t) btn_t = now;
        else if(now - btn_t > 1500) return_to_launcher();
    } else btn_t = 0;

    delay(3);
}
