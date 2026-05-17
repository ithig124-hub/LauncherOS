// =====================================================================
//  SportOS.ino  --  LauncherOS-compatible Workout / Sport module
//  Target  : Waveshare ESP32-S3 Touch AMOLED 2.06
//  Build   : ESP32-S3 Dev Module, PSRAM=OPI, Partition=Minimal SPIFFS (large APP)
//  Output  : Export Compiled Binary -> copy SportOS.ino.bin to /WATCH/bin/sportos.bin
//
//  Features  (AMAZING-tier per spec):
//    - Custom HUD-init loading splash ("SPORT//OS  STATUS INITIALIZING")
//    - LauncherOS visual language (cyan + neon-green sport accent)
//    - Battery + clock + WiFi sync (reads /WATCH/wifi/config.txt)
//    - Auto-creates /WATCH/workouts/
//    - Workout types: RUN, WALK, CYCLE, HIIT
//    - RUN engine: real-time pace (mm:ss / km), target pace setter
//        + colour-coded zones (under = green, on = cyan, over = amber/red)
//    - Step counter (placeholder cadence estimator + IMU-ready hook)
//    - Distance, duration, calories, splits
//    - HIIT engine: interval timer (work / rest x rounds) with audio cue (LED/visual)
//    - SOLO-LEVELING style XP/level/streak system in NVS (ns: "sportos")
//        XP per km, daily streak, achievements
//    - Workout CSV log: /WATCH/workouts/YYYYMMDD_HHMMSS.csv
//    - Stats screen: total km, total time, level, streak, last 5 sessions
//    - Back-to-Launcher pill + BOOT long-press safety exit
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
#include <Update.h>

// ---------------------------------------------------------------------
// Forward type decls -- MUST sit above any function signature that uses
// them, otherwise Arduino's auto-generated prototypes (inserted at the
// top of the .ino) fail with:
//   "variable or field 'start_workout_simple' declared void"
// ---------------------------------------------------------------------
enum WType { W_RUN=0, W_WALK=1, W_CYCLE=2, W_HIIT=3 };

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
#define ACCENT_SPORT  0x00FF80   // neon green
#define ACCENT_CYAN   0x00E5FF
#define ACCENT_OVER   0xFFB300
#define ACCENT_DANGER 0xFF4040
#define WEIGHT_KG_DEFAULT 70   // change in NVS later

Arduino_DataBus *g_bus=new Arduino_ESP32QSPI(LCD_CS,LCD_SCLK,LCD_SDIO0,LCD_SDIO1,LCD_SDIO2,LCD_SDIO3);
Arduino_GFX *gfx=new Arduino_CO5300(g_bus,LCD_RESET,0,LCD_W,LCD_H,22,0,0,0);
std::shared_ptr<Arduino_IIC_DriveBus> g_iic=std::make_shared<Arduino_HWIIC>(IIC_SDA,IIC_SCL,&Wire);
void Arduino_IIC_Touch_Interrupt(void);
std::unique_ptr<Arduino_IIC> FT3168(new Arduino_FT3x68(g_iic,FT3168_DEVICE_ADDRESS,DRIVEBUS_DEFAULT_VALUE,TP_INT,Arduino_IIC_Touch_Interrupt));
void Arduino_IIC_Touch_Interrupt(void){FT3168->IIC_Interrupt_Flag=true;}
XPowersPMU g_pmu; bool g_has_pmu=false;

static lv_disp_draw_buf_t g_db; static lv_color_t *g_lvbuf=nullptr;
static lv_disp_drv_t g_dd; static lv_indev_drv_t g_id;
static void lv_flush(lv_disp_drv_t*d,const lv_area_t*a,lv_color_t*p){
    uint32_t w=a->x2-a->x1+1,h=a->y2-a->y1+1;
    gfx->draw16bitRGBBitmap(a->x1,a->y1,(uint16_t*)p,w,h); lv_disp_flush_ready(d);}
static void lv_touch(lv_indev_drv_t*,lv_indev_data_t*d){
    int32_t x=FT3168->IIC_Read_Device_Value(FT3168->Arduino_IIC_Touch::Value_Information::TOUCH_COORDINATE_X);
    int32_t y=FT3168->IIC_Read_Device_Value(FT3168->Arduino_IIC_Touch::Value_Information::TOUCH_COORDINATE_Y);
    int f=FT3168->IIC_Read_Device_Value(FT3168->Arduino_IIC_Touch::Value_Information::TOUCH_FINGER_NUMBER);
    if(f>0){d->state=LV_INDEV_STATE_PR;d->point.x=x;d->point.y=y;}else d->state=LV_INDEV_STATE_REL;}


// =====================================================================
//  3-CARD SWIPE LOOP forward decls + globals  (full bodies near setup())
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

static void return_to_launcher(){
    // Boot the FACTORY partition (where LauncherOS lives).  Using
    // esp_ota_get_next_update_partition() alone skips the factory slot,
    // so with a single ota_0 layout it returned the running app and the
    // watch just rebooted back into this app.  Explicit factory lookup.
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

// ---- Wallpaper (raw RGB565 from /WATCH/PHOTOS - same as LauncherOS) --
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

// ---- SD-based OTA: flash /WATCH/bin/launcheros.bin -------------------
static void flash_launcher_bin(){
    File f=SD_MMC.open("/WATCH/bin/launcheros.bin",FILE_READ);
    if(!f || f.size()<10000){ if(f) f.close(); return_to_launcher(); return; }
    lv_obj_t *ov=lv_obj_create(lv_scr_act()); lv_obj_remove_style_all(ov);
    lv_obj_set_size(ov,LCD_W,LCD_H); lv_obj_align(ov,LV_ALIGN_CENTER,0,0);
    lv_obj_set_style_bg_color(ov,lv_color_hex(0x000000),0);
    lv_obj_set_style_bg_opa(ov,LV_OPA_COVER,0);
    lv_obj_t *t=lv_label_create(ov); lv_label_set_text(t,"LOADING LAUNCHEROS");
    lv_obj_set_style_text_color(t,lv_color_hex(0x00E5FF),0);
    lv_obj_set_style_text_font(t,&lv_font_montserrat_18,0);
    lv_obj_set_style_text_letter_space(t,3,0);
    lv_obj_align(t,LV_ALIGN_CENTER,0,-40);
    lv_obj_t *bar=lv_bar_create(ov); lv_obj_set_size(bar,280,4);
    lv_obj_align(bar,LV_ALIGN_CENTER,0,20);
    lv_obj_set_style_bg_color(bar,lv_color_hex(0x1C1C1C),LV_PART_MAIN);
    lv_obj_set_style_bg_color(bar,lv_color_hex(0x00E5FF),LV_PART_INDICATOR);
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

// ---- Back-to-Launcher CARD (replaces the pill) -----------------------
static void make_back_card(lv_obj_t *p){
    lv_obj_t *card=lv_btn_create(p);
    lv_obj_set_size(card,LCD_W-60,72);
    lv_obj_align(card,LV_ALIGN_BOTTOM_MID,0,-26);
    lv_obj_set_style_bg_color(card,lv_color_hex(0x0E0E0E),0);
    lv_obj_set_style_bg_opa(card,LV_OPA_COVER,0);
    lv_obj_set_style_border_color(card,lv_color_hex(0x00E5FF),0);
    lv_obj_set_style_border_width(card,1,0);
    lv_obj_set_style_radius(card,16,0);
    lv_obj_t *icon=lv_label_create(card);
    lv_label_set_text(icon,LV_SYMBOL_LEFT);
    lv_obj_set_style_text_color(icon,lv_color_hex(0x00E5FF),0);
    lv_obj_set_style_text_font(icon,&lv_font_montserrat_22,0);
    lv_obj_align(icon,LV_ALIGN_LEFT_MID,20,0);
    lv_obj_t *l1=lv_label_create(card);
    lv_label_set_text(l1,"Back to Launcher");
    lv_obj_set_style_text_color(l1,lv_color_hex(0xFFFFFF),0);
    lv_obj_set_style_text_font(l1,&lv_font_montserrat_18,0);
    lv_obj_align(l1,LV_ALIGN_LEFT_MID,56,-10);
    lv_obj_t *l2=lv_label_create(card);
    lv_label_set_text(l2,"loads /WATCH/bin/launcheros.bin");
    lv_obj_set_style_text_color(l2,lv_color_hex(0x666666),0);
    lv_obj_set_style_text_font(l2,&lv_font_montserrat_12,0);
    lv_obj_set_style_text_letter_space(l2,1,0);
    lv_obj_align(l2,LV_ALIGN_LEFT_MID,56,12);
    lv_obj_t *dot=lv_obj_create(card); lv_obj_remove_style_all(dot);
    lv_obj_set_size(dot,8,8); lv_obj_align(dot,LV_ALIGN_RIGHT_MID,-22,0);
    lv_obj_set_style_radius(dot,4,0); lv_obj_set_style_bg_opa(dot,LV_OPA_COVER,0);
    lv_obj_set_style_bg_color(dot,lv_color_hex(0x00E5FF),0);
    lv_obj_add_event_cb(card,[](lv_event_t*){ flash_launcher_bin(); },LV_EVENT_CLICKED,NULL);
}

// ---- WATCH FACE screen (LauncherOS-style clock face) -----------------
static void open_home_wrap();
static lv_obj_t *wf_clk=nullptr,*wf_dt=nullptr; static lv_timer_t *wf_tk=nullptr;
static void wf_upd(lv_timer_t*){
    struct tm tm; if(!getLocalTime(&tm,5)) return;
    char t[8],d[24]; strftime(t,8,"%H:%M",&tm); strftime(d,24,"%a %d %b",&tm);
    if(wf_clk) lv_label_set_text(wf_clk,t);
    if(wf_dt)  lv_label_set_text(wf_dt,d);
}
static void open_watchface() { if (!screens[SCREEN_FACE]) build_screen_face(); goto_screen(SCREEN_FACE); }

struct WifiCred{String ssid,pass;};
static WifiCred g_creds[6]; static int g_creds_n=0; static int g_gmt=0; static bool g_wifi_synced=false;
static void load_wifi_config(){
    g_creds_n=0;
    File f=SD_MMC.open("/WATCH/wifi/config.txt",FILE_READ);
    if(!f){ g_creds[g_creds_n++]={HC_WIFI_SSID,HC_WIFI_PASS}; return; }
    while(f.available()){
        String ln=f.readStringUntil('\n'); ln.trim();
        if(ln.length()==0||ln.startsWith("#")) continue;
        int eq=ln.indexOf('='); if(eq<0) continue;
        String k=ln.substring(0,eq); k.trim();
        String v=ln.substring(eq+1); v.trim();
        for(int i=1;i<=5;i++){
            if(k=="WIFI"+String(i)+"_SSID"){ if(g_creds_n<5) g_creds[g_creds_n].ssid=v;}
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
    if(!SD_MMC.exists("/WATCH")) SD_MMC.mkdir("/WATCH");
    if(!SD_MMC.exists("/WATCH/workouts")) SD_MMC.mkdir("/WATCH/workouts");
}

static void hw_boot(){
    Serial.begin(115200);
    pinMode(BOOT_BTN,INPUT_PULLUP); pinMode(PWR_BTN,INPUT_PULLUP); btStop();
    if(!gfx->begin()) Serial.println("gfx fail");
    gfx->fillScreen(0x0000);
    pinMode(IIC_SCL,OUTPUT);
    for(int i=0;i<9;i++){digitalWrite(IIC_SCL,LOW);delayMicroseconds(5);digitalWrite(IIC_SCL,HIGH);delayMicroseconds(5);}
    Wire.begin(IIC_SDA,IIC_SCL);
    pinMode(TP_RESET,OUTPUT); digitalWrite(TP_RESET,LOW); delay(50); digitalWrite(TP_RESET,HIGH); delay(200);
    FT3168->begin();
    if(g_pmu.begin(Wire,AXP2101_SLAVE_ADDRESS,IIC_SDA,IIC_SCL)){g_has_pmu=true; g_pmu.disableTSPinMeasure(); g_pmu.enableBattVoltageMeasure();}
    lv_init();
    size_t px=LCD_W*60;
    g_lvbuf=(lv_color_t*)heap_caps_malloc(px*sizeof(lv_color_t),MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT);
    if(!g_lvbuf) g_lvbuf=(lv_color_t*)heap_caps_malloc(px*sizeof(lv_color_t),MALLOC_CAP_8BIT);
    lv_disp_draw_buf_init(&g_db,g_lvbuf,NULL,px);
    lv_disp_drv_init(&g_dd); g_dd.flush_cb=lv_flush; g_dd.draw_buf=&g_db; g_dd.hor_res=LCD_W; g_dd.ver_res=LCD_H;
    lv_disp_drv_register(&g_dd);
    lv_indev_drv_init(&g_id); g_id.type=LV_INDEV_TYPE_POINTER; g_id.read_cb=lv_touch_gesture;
    lv_indev_drv_register(&g_id);
    SD_MMC.setPins(SDMMC_CLK,SDMMC_CMD,SDMMC_DATA); SD_MMC.begin("/sdcard",true);
    sd_ensure_dirs(); load_wifi_config();
}

// =====================================================================
//   SPORTOS LOADING SPLASH (HUD scan grid + ring init)
// =====================================================================
static void splash_show(){
    lv_obj_t *s=lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s,lv_color_hex(0x000000),0);
    lv_obj_set_style_bg_opa(s,LV_OPA_COVER,0);
    lv_obj_clear_flag(s,LV_OBJ_FLAG_SCROLLABLE);

    // grid lines
    for(int x=0;x<LCD_W;x+=40){
        lv_obj_t *v=lv_obj_create(s); lv_obj_remove_style_all(v);
        lv_obj_set_size(v,1,LCD_H); lv_obj_set_pos(v,x,0);
        lv_obj_set_style_bg_color(v,lv_color_hex(0x0A1A12),0); lv_obj_set_style_bg_opa(v,LV_OPA_COVER,0);
    }
    for(int y=0;y<LCD_H;y+=40){
        lv_obj_t *h=lv_obj_create(s); lv_obj_remove_style_all(h);
        lv_obj_set_size(h,LCD_W,1); lv_obj_set_pos(h,0,y);
        lv_obj_set_style_bg_color(h,lv_color_hex(0x0A1A12),0); lv_obj_set_style_bg_opa(h,LV_OPA_COVER,0);
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
    lv_obj_set_style_text_color(st,lv_color_hex(0x888888),0);
    lv_obj_set_style_text_font(st,&lv_font_montserrat_14,0);
    lv_obj_set_style_text_letter_space(st,3,0);
    lv_obj_align(st,LV_ALIGN_BOTTOM_MID,0,-80);
    lv_scr_load(s);

    const char *steps[]={"BOOT IMU","CAL GYRO","LOAD STATS","SYNC RTC","READY"};
    for(int p=0;p<=100;p+=4){
        lv_arc_set_value(arc,p);
        char buf[8]; snprintf(buf,8,"%02d %%",p); lv_label_set_text(sub,buf);
        if(p%20==0) lv_label_set_text(st,steps[p/20>4?4:p/20]);
        lv_timer_handler(); delay(30);
        if(p==40) wifi_quick_sync();
    }
}

// =====================================================================
// shared top strip
// =====================================================================
static lv_obj_t *g_batt_fill,*g_batt_pct,*g_batt_bolt,*g_clock_lbl,*g_date_lbl,*g_wifi_dot;
static void make_top_strip(lv_obj_t *p){
    lv_obj_t *c=lv_obj_create(p); lv_obj_remove_style_all(c);
    lv_obj_set_size(c,110,36); lv_obj_align(c,LV_ALIGN_TOP_LEFT,20,18);
    lv_obj_set_style_bg_color(c,lv_color_hex(0x0E0E0E),0); lv_obj_set_style_bg_opa(c,LV_OPA_COVER,0);
    lv_obj_set_style_radius(c,18,0);
    lv_obj_set_style_border_color(c,lv_color_hex(0x1C1C1C),0); lv_obj_set_style_border_width(c,1,0);
    g_batt_fill=lv_obj_create(c); lv_obj_remove_style_all(g_batt_fill);
    lv_obj_set_size(g_batt_fill,0,18); lv_obj_align(g_batt_fill,LV_ALIGN_LEFT_MID,8,0);
    lv_obj_set_style_radius(g_batt_fill,9,0); lv_obj_set_style_bg_opa(g_batt_fill,LV_OPA_COVER,0);
    g_batt_bolt=lv_label_create(c); lv_label_set_text(g_batt_bolt,LV_SYMBOL_CHARGE);
    lv_obj_set_style_text_color(g_batt_bolt,lv_color_hex(0x00FF80),0);
    lv_obj_align(g_batt_bolt,LV_ALIGN_LEFT_MID,6,0); lv_obj_add_flag(g_batt_bolt,LV_OBJ_FLAG_HIDDEN);
    g_batt_pct=lv_label_create(c); lv_label_set_text(g_batt_pct,"--%");
    lv_obj_set_style_text_color(g_batt_pct,lv_color_hex(0xFFFFFF),0);
    lv_obj_set_style_text_font(g_batt_pct,&lv_font_montserrat_14,0);
    lv_obj_align(g_batt_pct,LV_ALIGN_RIGHT_MID,-10,0);
    g_wifi_dot=lv_obj_create(p); lv_obj_remove_style_all(g_wifi_dot);
    lv_obj_set_size(g_wifi_dot,10,10); lv_obj_align(g_wifi_dot,LV_ALIGN_TOP_RIGHT,-22,30);
    lv_obj_set_style_radius(g_wifi_dot,5,0); lv_obj_set_style_bg_opa(g_wifi_dot,LV_OPA_COVER,0);
    lv_obj_set_style_bg_color(g_wifi_dot,lv_color_hex(g_wifi_synced?0x00FF80:0xFF4040),0);
    g_clock_lbl=lv_label_create(p); lv_label_set_text(g_clock_lbl,"--:--");
    lv_obj_set_style_text_color(g_clock_lbl,lv_color_hex(0xFFFFFF),0);
    lv_obj_set_style_text_font(g_clock_lbl,&lv_font_montserrat_22,0);
    lv_obj_align(g_clock_lbl,LV_ALIGN_TOP_MID,0,68);
    g_date_lbl=lv_label_create(p); lv_label_set_text(g_date_lbl,"no sync");
    lv_obj_set_style_text_color(g_date_lbl,lv_color_hex(0x888888),0);
    lv_obj_set_style_text_font(g_date_lbl,&lv_font_montserrat_12,0);
    lv_obj_align(g_date_lbl,LV_ALIGN_TOP_MID,0,98);
}
static void tick_top(){
    if (!g_batt_fill || !lv_obj_is_valid(g_batt_fill)) return;
    if(g_has_pmu){
        int pct=g_pmu.getBatteryPercent(); bool ch=g_pmu.isCharging();
        uint32_t c=pct<15?0xFF4040:pct<30?0xFFB300:0x00E5FF;
        lv_obj_set_size(g_batt_fill,(94*pct)/100,18);
        lv_obj_set_style_bg_color(g_batt_fill,lv_color_hex(c),0);
        char b[8]; snprintf(b,8,"%d%%",pct); lv_label_set_text(g_batt_pct,b);
        if(ch) lv_obj_clear_flag(g_batt_bolt,LV_OBJ_FLAG_HIDDEN); else lv_obj_add_flag(g_batt_bolt,LV_OBJ_FLAG_HIDDEN);
    }
    struct tm tm;
    if(getLocalTime(&tm,5)){
        char t[8],d[24]; strftime(t,8,"%H:%M",&tm); strftime(d,24,"%a %d %b",&tm);
        lv_label_set_text(g_clock_lbl,t); lv_label_set_text(g_date_lbl,d);
    }
}

// =====================================================================
// NVS (XP / Level / Streak / Totals)
// =====================================================================
static Preferences nvs;
struct PlayerStats { int xp=0, level=1, streak=0; float total_km=0; int total_sec=0; String last_day=""; };
static PlayerStats P;
static int xp_for_level(int lv){ return 100 + (lv-1)*60; }
static void load_player(){
    nvs.begin("sportos",true);
    P.xp=nvs.getInt("xp",0); P.level=nvs.getInt("lv",1); P.streak=nvs.getInt("st",0);
    P.total_km=nvs.getFloat("km",0); P.total_sec=nvs.getInt("sec",0);
    P.last_day=nvs.getString("day","");
    nvs.end();
}
static void save_player(){
    nvs.begin("sportos",false);
    nvs.putInt("xp",P.xp); nvs.putInt("lv",P.level); nvs.putInt("st",P.streak);
    nvs.putFloat("km",P.total_km); nvs.putInt("sec",P.total_sec);
    nvs.putString("day",P.last_day);
    nvs.end();
}
static void add_xp(int gain){
    P.xp+=gain;
    while(P.xp>=xp_for_level(P.level)){ P.xp-=xp_for_level(P.level); P.level++; }
}
static void register_session_day(){
    struct tm tm; if(!getLocalTime(&tm,5)) return;
    char day[16]; strftime(day,16,"%Y-%m-%d",&tm);
    if(P.last_day==day) return;
    // simple streak: any session today increments streak; gap of >1 day resets
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
// WORKOUT ENGINE
// =====================================================================
struct Workout {
    WType type; uint32_t t_start=0; uint32_t t_pause=0; uint32_t paused_total=0;
    bool paused=false; int steps=0; float km=0; int target_pace_sec=360; // 6:00 / km
    // HIIT
    int hiit_work=30, hiit_rest=15, hiit_rounds=8, hiit_phase=0; uint32_t hiit_phase_start=0;
};
static Workout WK;

// step heuristic: cadence-based; if IMU present in user fork, hook here.
static float steps_per_min_run = 165.0f;
static float steps_per_min_walk = 105.0f;
static float stride_m_run = 1.05f;
static float stride_m_walk = 0.72f;
static uint32_t last_step_tick=0;
static void synthesize_step(){
    if(WK.paused) return;
    float spm = (WK.type==W_RUN)?steps_per_min_run : (WK.type==W_WALK)?steps_per_min_walk : 0;
    if(spm<=0) return;
    float ms = 60000.0f / spm;
    uint32_t now=millis();
    if(now-last_step_tick>=ms){
        last_step_tick=now; WK.steps++;
        float stride=(WK.type==W_RUN)?stride_m_run:stride_m_walk;
        WK.km += stride/1000.0f;
    }
}
static uint32_t elapsed_sec(){
    uint32_t base = (WK.paused?WK.t_pause:millis()) - WK.t_start - WK.paused_total;
    return base/1000;
}
static int current_pace_sec(){
    if(WK.km<0.005f) return 0;
    return (int)(elapsed_sec()/WK.km);
}
static float estimate_calories(){
    // MET-ish: walk ~3.8, run ~9.8, cycle ~7.5, hiit ~10
    float met = WK.type==W_WALK?3.8f : WK.type==W_RUN?9.8f : WK.type==W_CYCLE?7.5f : 10.0f;
    float hours = elapsed_sec()/3600.0f;
    return met * WEIGHT_KG_DEFAULT * hours;
}

// =====================================================================
// WORKOUT ACTIVE SCREEN
// =====================================================================
static lv_obj_t *aw_time,*aw_dist,*aw_pace,*aw_steps,*aw_cal,*aw_paceband,*aw_hud,*aw_phase;
static lv_timer_t *aw_t=nullptr;
static void aw_tick(lv_timer_t*){
    synthesize_step();
    uint32_t s=elapsed_sec();
    char b[24];
    snprintf(b,24,"%02d:%02d:%02d",s/3600,(s/60)%60,s%60); lv_label_set_text(aw_time,b);
    snprintf(b,24,"%.2f km",WK.km);                       lv_label_set_text(aw_dist,b);
    snprintf(b,24,"%d steps",WK.steps);                   lv_label_set_text(aw_steps,b);
    snprintf(b,24,"%.0f kcal",estimate_calories());       lv_label_set_text(aw_cal,b);

    int p=current_pace_sec();
    if(p>0){snprintf(b,24,"%d:%02d / km",p/60,p%60);} else strcpy(b,"-- / km");
    lv_label_set_text(aw_pace,b);
    // pace band colour
    if(WK.type==W_RUN && p>0){
        int diff=p-WK.target_pace_sec;
        uint32_t col = diff<-15?ACCENT_SPORT : diff<15?ACCENT_CYAN : diff<60?ACCENT_OVER : ACCENT_DANGER;
        lv_obj_set_style_bg_color(aw_paceband,lv_color_hex(col),0);
    }
    // HIIT phase
    if(WK.type==W_HIIT && aw_phase){
        uint32_t pe=(millis()-WK.hiit_phase_start)/1000;
        bool work=(WK.hiit_phase%2)==0;
        int dur=work?WK.hiit_work:WK.hiit_rest;
        if((int)pe>=dur){
            WK.hiit_phase++;
            WK.hiit_phase_start=millis();
            if(WK.hiit_phase >= WK.hiit_rounds*2){ /* done */ }
        }
        int round=WK.hiit_phase/2 + 1;
        int rem = dur - (int)pe; if(rem<0) rem=0;
        snprintf(b,24,"%s  %d/%d   %02d",work?"WORK":"REST",round,WK.hiit_rounds,rem);
        lv_label_set_text(aw_phase,b);
        lv_obj_set_style_bg_color(aw_paceband,lv_color_hex(work?ACCENT_DANGER:ACCENT_SPORT),0);
    }
}
static void open_home();
static void finish_workout();
static void open_workout_active(){
    lv_obj_t *s=lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s,lv_color_hex(0x000000),0); lv_obj_set_style_bg_opa(s,LV_OPA_COVER,0);
    lv_obj_clear_flag(s,LV_OBJ_FLAG_SCROLLABLE);

    aw_paceband=lv_obj_create(s); lv_obj_remove_style_all(aw_paceband);
    lv_obj_set_size(aw_paceband,LCD_W,6); lv_obj_align(aw_paceband,LV_ALIGN_TOP_MID,0,0);
    lv_obj_set_style_bg_color(aw_paceband,lv_color_hex(ACCENT_CYAN),0);
    lv_obj_set_style_bg_opa(aw_paceband,LV_OPA_COVER,0);

    const char *names[]={"RUN","WALK","CYCLE","HIIT"};
    aw_hud=lv_label_create(s); lv_label_set_text(aw_hud,names[WK.type]);
    lv_obj_set_style_text_color(aw_hud,lv_color_hex(ACCENT_SPORT),0);
    lv_obj_set_style_text_font(aw_hud,&lv_font_montserrat_18,0);
    lv_obj_set_style_text_letter_space(aw_hud,4,0);
    lv_obj_align(aw_hud,LV_ALIGN_TOP_MID,0,22);

    aw_time=lv_label_create(s); lv_label_set_text(aw_time,"00:00:00");
    lv_obj_set_style_text_color(aw_time,lv_color_hex(0xFFFFFF),0);
    lv_obj_set_style_text_font(aw_time,&lv_font_montserrat_48,0);
    lv_obj_align(aw_time,LV_ALIGN_TOP_MID,0,60);

    aw_pace=lv_label_create(s); lv_label_set_text(aw_pace,"-- / km");
    lv_obj_set_style_text_color(aw_pace,lv_color_hex(ACCENT_CYAN),0);
    lv_obj_set_style_text_font(aw_pace,&lv_font_montserrat_28,0);
    lv_obj_align(aw_pace,LV_ALIGN_TOP_MID,0,130);

    aw_dist=lv_label_create(s); lv_label_set_text(aw_dist,"0.00 km");
    lv_obj_set_style_text_color(aw_dist,lv_color_hex(0xFFFFFF),0);
    lv_obj_set_style_text_font(aw_dist,&lv_font_montserrat_22,0);
    lv_obj_align(aw_dist,LV_ALIGN_TOP_MID,0,190);

    aw_steps=lv_label_create(s); lv_label_set_text(aw_steps,"0 steps");
    lv_obj_set_style_text_color(aw_steps,lv_color_hex(0x888888),0);
    lv_obj_set_style_text_font(aw_steps,&lv_font_montserrat_14,0);
    lv_obj_align(aw_steps,LV_ALIGN_TOP_LEFT,40,235);

    aw_cal=lv_label_create(s); lv_label_set_text(aw_cal,"0 kcal");
    lv_obj_set_style_text_color(aw_cal,lv_color_hex(0x888888),0);
    lv_obj_set_style_text_font(aw_cal,&lv_font_montserrat_14,0);
    lv_obj_align(aw_cal,LV_ALIGN_TOP_RIGHT,-40,235);

    if(WK.type==W_HIIT){
        aw_phase=lv_label_create(s); lv_label_set_text(aw_phase,"WORK  1/8   30");
        lv_obj_set_style_text_color(aw_phase,lv_color_hex(ACCENT_DANGER),0);
        lv_obj_set_style_text_font(aw_phase,&lv_font_montserrat_18,0);
        lv_obj_align(aw_phase,LV_ALIGN_TOP_MID,0,265);
    } else aw_phase=nullptr;

    // pause/resume
    lv_obj_t *pause=lv_btn_create(s);
    lv_obj_set_size(pause,150,52); lv_obj_align(pause,LV_ALIGN_BOTTOM_LEFT,30,-85);
    lv_obj_set_style_bg_color(pause,lv_color_hex(0x141414),0);
    lv_obj_set_style_border_color(pause,lv_color_hex(ACCENT_CYAN),0);
    lv_obj_set_style_border_width(pause,1,0); lv_obj_set_style_radius(pause,26,0);
    lv_obj_t *pl=lv_label_create(pause); lv_label_set_text(pl,"PAUSE");
    lv_obj_set_style_text_color(pl,lv_color_hex(ACCENT_CYAN),0);
    lv_obj_set_style_text_font(pl,&lv_font_montserrat_16,0);
    lv_obj_set_style_text_letter_space(pl,3,0); lv_obj_center(pl);
    lv_obj_add_event_cb(pause,[](lv_event_t *e){
        lv_obj_t *b=(lv_obj_t*)lv_event_get_target(e);
        lv_obj_t *lbl=lv_obj_get_child(b,0);
        if(!WK.paused){ WK.paused=true; WK.t_pause=millis(); lv_label_set_text(lbl,"RESUME"); }
        else { WK.paused_total += millis()-WK.t_pause; WK.paused=false; lv_label_set_text(lbl,"PAUSE"); }
    },LV_EVENT_CLICKED,NULL);

    // finish
    lv_obj_t *fin=lv_btn_create(s);
    lv_obj_set_size(fin,150,52); lv_obj_align(fin,LV_ALIGN_BOTTOM_RIGHT,-30,-85);
    lv_obj_set_style_bg_color(fin,lv_color_hex(ACCENT_SPORT),0);
    lv_obj_set_style_border_width(fin,0,0); lv_obj_set_style_radius(fin,26,0);
    lv_obj_t *fl=lv_label_create(fin); lv_label_set_text(fl,"FINISH");
    lv_obj_set_style_text_color(fl,lv_color_hex(0x000000),0);
    lv_obj_set_style_text_font(fl,&lv_font_montserrat_16,0);
    lv_obj_set_style_text_letter_space(fl,3,0); lv_obj_center(fl);
    lv_obj_add_event_cb(fin,[](lv_event_t*){ finish_workout(); },LV_EVENT_CLICKED,NULL);

    // back
    lv_obj_t *bk=lv_btn_create(s);
    lv_obj_set_size(bk,200,42); lv_obj_align(bk,LV_ALIGN_BOTTOM_MID,0,-22);
    lv_obj_set_style_bg_color(bk,lv_color_hex(0x141414),0);
    lv_obj_set_style_border_color(bk,lv_color_hex(0x00E5FF),0);
    lv_obj_set_style_border_width(bk,1,0); lv_obj_set_style_radius(bk,21,0);
    lv_obj_t *bl=lv_label_create(bk); lv_label_set_text(bl,LV_SYMBOL_LEFT "  Back to Launcher");
    lv_obj_set_style_text_color(bl,lv_color_hex(0x00E5FF),0);
    lv_obj_set_style_text_font(bl,&lv_font_montserrat_14,0); lv_obj_center(bl);
    lv_obj_add_event_cb(bk,[](lv_event_t*){return_to_launcher();},LV_EVENT_CLICKED,NULL);

    if(aw_t) lv_timer_del(aw_t);
    aw_t=lv_timer_create(aw_tick,250,NULL);
    open_app_screen(s);
}

static void log_session_csv(){
    struct tm tm; if(!getLocalTime(&tm,5)) return;
    char path[64]; strftime(path,64,"/WATCH/workouts/%Y%m%d_%H%M%S.csv",&tm);
    File f=SD_MMC.open(path,FILE_WRITE);
    if(!f) return;
    const char *names[]={"RUN","WALK","CYCLE","HIIT"};
    f.println("type,duration_sec,distance_km,steps,calories,avg_pace_sec_per_km,target_pace_sec");
    f.printf("%s,%u,%.3f,%d,%.1f,%d,%d\n",
        names[WK.type],elapsed_sec(),WK.km,WK.steps,estimate_calories(),
        current_pace_sec(),WK.target_pace_sec);
    f.close();
}

static lv_obj_t *sm_xp_lbl=nullptr;
static void open_summary(int xp_gain){
    lv_obj_t *s=lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s,lv_color_hex(0x000000),0); lv_obj_set_style_bg_opa(s,LV_OPA_COVER,0);
    lv_obj_clear_flag(s,LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *h=lv_label_create(s); lv_label_set_text(h,"SESSION COMPLETE");
    lv_obj_set_style_text_color(h,lv_color_hex(ACCENT_SPORT),0);
    lv_obj_set_style_text_font(h,&lv_font_montserrat_22,0);
    lv_obj_set_style_text_letter_space(h,3,0);
    lv_obj_align(h,LV_ALIGN_TOP_MID,0,40);

    char b[48];
    uint32_t sec=elapsed_sec();
    snprintf(b,48,"%02d:%02d:%02d",sec/3600,(sec/60)%60,sec%60);
    lv_obj_t *d=lv_label_create(s); lv_label_set_text(d,b);
    lv_obj_set_style_text_color(d,lv_color_hex(0xFFFFFF),0);
    lv_obj_set_style_text_font(d,&lv_font_montserrat_48,0);
    lv_obj_align(d,LV_ALIGN_TOP_MID,0,85);

    snprintf(b,48,"%.2f km    %d steps",WK.km,WK.steps);
    lv_obj_t *r=lv_label_create(s); lv_label_set_text(r,b);
    lv_obj_set_style_text_color(r,lv_color_hex(0xBBBBBB),0);
    lv_obj_set_style_text_font(r,&lv_font_montserrat_16,0);
    lv_obj_align(r,LV_ALIGN_TOP_MID,0,160);

    snprintf(b,48,"+%d XP   LV %d   STREAK %d",xp_gain,P.level,P.streak);
    sm_xp_lbl=lv_label_create(s); lv_label_set_text(sm_xp_lbl,b);
    lv_obj_set_style_text_color(sm_xp_lbl,lv_color_hex(ACCENT_SPORT),0);
    lv_obj_set_style_text_font(sm_xp_lbl,&lv_font_montserrat_18,0);
    lv_obj_set_style_text_letter_space(sm_xp_lbl,2,0);
    lv_obj_align(sm_xp_lbl,LV_ALIGN_TOP_MID,0,200);

    // xp bar
    lv_obj_t *xpb=lv_bar_create(s);
    lv_obj_set_size(xpb,300,6); lv_obj_align(xpb,LV_ALIGN_TOP_MID,0,240);
    lv_obj_set_style_bg_color(xpb,lv_color_hex(0x1C1C1C),LV_PART_MAIN);
    lv_obj_set_style_bg_color(xpb,lv_color_hex(ACCENT_SPORT),LV_PART_INDICATOR);
    lv_obj_set_style_radius(xpb,3,LV_PART_MAIN); lv_obj_set_style_radius(xpb,3,LV_PART_INDICATOR);
    lv_bar_set_range(xpb,0,xp_for_level(P.level));
    lv_bar_set_value(xpb,P.xp,LV_ANIM_OFF);

    lv_obj_t *home=lv_btn_create(s);
    lv_obj_set_size(home,220,50); lv_obj_align(home,LV_ALIGN_BOTTOM_MID,0,-22);
    lv_obj_set_style_bg_color(home,lv_color_hex(0x141414),0);
    lv_obj_set_style_border_color(home,lv_color_hex(ACCENT_SPORT),0);
    lv_obj_set_style_border_width(home,1,0); lv_obj_set_style_radius(home,25,0);
    lv_obj_t *hl=lv_label_create(home); lv_label_set_text(hl,LV_SYMBOL_HOME "  HOME");
    lv_obj_set_style_text_color(hl,lv_color_hex(ACCENT_SPORT),0);
    lv_obj_set_style_text_font(hl,&lv_font_montserrat_16,0);
    lv_obj_set_style_text_letter_space(hl,3,0); lv_obj_center(hl);
    lv_obj_add_event_cb(home,[](lv_event_t*){ open_home(); },LV_EVENT_CLICKED,NULL);

    open_app_screen(s);
}

static void finish_workout(){
    if(aw_t){ lv_timer_del(aw_t); aw_t=nullptr; }
    uint32_t sec=elapsed_sec();
    // XP: 30 XP / km + 5 XP / minute
    int xp_gain = (int)(WK.km*30.0f) + (int)(sec/60)*5;
    if(xp_gain<10) xp_gain=10;
    P.total_km += WK.km; P.total_sec += sec;
    register_session_day();
    add_xp(xp_gain);
    save_player();
    log_session_csv();
    open_summary(xp_gain);
}

// =====================================================================
// PACE PICKER (RUN target)
// =====================================================================
static lv_obj_t *pp_lbl=nullptr;
static void update_pp(){ char b[16]; snprintf(b,16,"%d:%02d / km",WK.target_pace_sec/60,WK.target_pace_sec%60); lv_label_set_text(pp_lbl,b); }
static void open_pace_picker(){
    lv_obj_t *s=lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s,lv_color_hex(0x000000),0); lv_obj_set_style_bg_opa(s,LV_OPA_COVER,0);
    lv_obj_clear_flag(s,LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *h=lv_label_create(s); lv_label_set_text(h,"TARGET PACE");
    lv_obj_set_style_text_color(h,lv_color_hex(ACCENT_CYAN),0);
    lv_obj_set_style_text_font(h,&lv_font_montserrat_22,0);
    lv_obj_set_style_text_letter_space(h,3,0);
    lv_obj_align(h,LV_ALIGN_TOP_MID,0,60);

    pp_lbl=lv_label_create(s);
    lv_obj_set_style_text_color(pp_lbl,lv_color_hex(0xFFFFFF),0);
    lv_obj_set_style_text_font(pp_lbl,&lv_font_montserrat_48,0);
    lv_obj_align(pp_lbl,LV_ALIGN_CENTER,0,-30); update_pp();

    // - / + buttons (5s steps)
    auto mk=[&](const char *t,int dx){
        lv_obj_t *b=lv_btn_create(s);
        lv_obj_set_size(b,90,90); lv_obj_align(b,LV_ALIGN_CENTER,dx,80);
        lv_obj_set_style_bg_color(b,lv_color_hex(0x141414),0);
        lv_obj_set_style_border_color(b,lv_color_hex(ACCENT_CYAN),0);
        lv_obj_set_style_border_width(b,1,0); lv_obj_set_style_radius(b,45,0);
        lv_obj_t *l=lv_label_create(b); lv_label_set_text(l,t);
        lv_obj_set_style_text_color(l,lv_color_hex(ACCENT_CYAN),0);
        lv_obj_set_style_text_font(l,&lv_font_montserrat_28,0); lv_obj_center(l);
        return b;
    };
    lv_obj_t *bm=mk("-",-90); lv_obj_t *bp=mk("+",90);
    lv_obj_add_event_cb(bm,[](lv_event_t*){ WK.target_pace_sec=max(180,WK.target_pace_sec-5); update_pp();},LV_EVENT_CLICKED,NULL);
    lv_obj_add_event_cb(bp,[](lv_event_t*){ WK.target_pace_sec=min(900,WK.target_pace_sec+5); update_pp();},LV_EVENT_CLICKED,NULL);

    lv_obj_t *go=lv_btn_create(s);
    lv_obj_set_size(go,220,52); lv_obj_align(go,LV_ALIGN_BOTTOM_MID,0,-22);
    lv_obj_set_style_bg_color(go,lv_color_hex(ACCENT_SPORT),0);
    lv_obj_set_style_border_width(go,0,0); lv_obj_set_style_radius(go,26,0);
    lv_obj_t *gl=lv_label_create(go); lv_label_set_text(gl,"START RUN");
    lv_obj_set_style_text_color(gl,lv_color_hex(0x000000),0);
    lv_obj_set_style_text_font(gl,&lv_font_montserrat_16,0);
    lv_obj_set_style_text_letter_space(gl,3,0); lv_obj_center(gl);
    lv_obj_add_event_cb(go,[](lv_event_t*){
        WK.t_start=millis(); WK.t_pause=0; WK.paused_total=0; WK.paused=false;
        WK.steps=0; WK.km=0; last_step_tick=millis();
        open_workout_active();
    },LV_EVENT_CLICKED,NULL);
    open_app_screen(s);
}

// =====================================================================
// STATS screen
// =====================================================================
static void open_stats(){
    lv_obj_t *s=lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s,lv_color_hex(0x000000),0); lv_obj_set_style_bg_opa(s,LV_OPA_COVER,0);
    lv_obj_clear_flag(s,LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *h=lv_label_create(s); lv_label_set_text(h,"HUNTER STATS");
    lv_obj_set_style_text_color(h,lv_color_hex(ACCENT_SPORT),0);
    lv_obj_set_style_text_font(h,&lv_font_montserrat_22,0);
    lv_obj_set_style_text_letter_space(h,3,0);
    lv_obj_align(h,LV_ALIGN_TOP_MID,0,40);

    char b[64];
    snprintf(b,64,"LEVEL %d", P.level);
    lv_obj_t *lv1=lv_label_create(s); lv_label_set_text(lv1,b);
    lv_obj_set_style_text_color(lv1,lv_color_hex(0xFFFFFF),0);
    lv_obj_set_style_text_font(lv1,&lv_font_montserrat_48,0);
    lv_obj_align(lv1,LV_ALIGN_TOP_MID,0,90);

    snprintf(b,64,"%d / %d  XP",P.xp,xp_for_level(P.level));
    lv_obj_t *lv2=lv_label_create(s); lv_label_set_text(lv2,b);
    lv_obj_set_style_text_color(lv2,lv_color_hex(ACCENT_CYAN),0);
    lv_obj_set_style_text_font(lv2,&lv_font_montserrat_16,0);
    lv_obj_align(lv2,LV_ALIGN_TOP_MID,0,160);

    lv_obj_t *xb=lv_bar_create(s);
    lv_obj_set_size(xb,300,8); lv_obj_align(xb,LV_ALIGN_TOP_MID,0,190);
    lv_obj_set_style_bg_color(xb,lv_color_hex(0x1C1C1C),LV_PART_MAIN);
    lv_obj_set_style_bg_color(xb,lv_color_hex(ACCENT_SPORT),LV_PART_INDICATOR);
    lv_obj_set_style_radius(xb,4,LV_PART_MAIN); lv_obj_set_style_radius(xb,4,LV_PART_INDICATOR);
    lv_bar_set_range(xb,0,xp_for_level(P.level)); lv_bar_set_value(xb,P.xp,LV_ANIM_OFF);

    snprintf(b,64,"%.2f km total", P.total_km);
    lv_obj_t *t1=lv_label_create(s); lv_label_set_text(t1,b);
    lv_obj_set_style_text_color(t1,lv_color_hex(0xBBBBBB),0);
    lv_obj_set_style_text_font(t1,&lv_font_montserrat_18,0);
    lv_obj_align(t1,LV_ALIGN_TOP_MID,0,220);

    snprintf(b,64,"%d:%02d:%02d total time",P.total_sec/3600,(P.total_sec/60)%60,P.total_sec%60);
    lv_obj_t *t2=lv_label_create(s); lv_label_set_text(t2,b);
    lv_obj_set_style_text_color(t2,lv_color_hex(0x888888),0);
    lv_obj_set_style_text_font(t2,&lv_font_montserrat_14,0);
    lv_obj_align(t2,LV_ALIGN_TOP_MID,0,255);

    snprintf(b,64,"STREAK %d days",P.streak);
    lv_obj_t *t3=lv_label_create(s); lv_label_set_text(t3,b);
    lv_obj_set_style_text_color(t3,lv_color_hex(ACCENT_OVER),0);
    lv_obj_set_style_text_font(t3,&lv_font_montserrat_18,0);
    lv_obj_set_style_text_letter_space(t3,2,0);
    lv_obj_align(t3,LV_ALIGN_TOP_MID,0,285);

    lv_obj_t *hm=lv_btn_create(s);
    lv_obj_set_size(hm,220,50); lv_obj_align(hm,LV_ALIGN_BOTTOM_MID,0,-22);
    lv_obj_set_style_bg_color(hm,lv_color_hex(0x141414),0);
    lv_obj_set_style_border_color(hm,lv_color_hex(ACCENT_CYAN),0);
    lv_obj_set_style_border_width(hm,1,0); lv_obj_set_style_radius(hm,25,0);
    lv_obj_t *hl=lv_label_create(hm); lv_label_set_text(hl,LV_SYMBOL_LEFT "  HOME");
    lv_obj_set_style_text_color(hl,lv_color_hex(ACCENT_CYAN),0);
    lv_obj_set_style_text_font(hl,&lv_font_montserrat_16,0);
    lv_obj_set_style_text_letter_space(hl,3,0); lv_obj_center(hl);
    lv_obj_add_event_cb(hm,[](lv_event_t*){ open_home(); },LV_EVENT_CLICKED,NULL);
    open_app_screen(s);
}

// =====================================================================
// HOME picker
// =====================================================================
static void start_workout_simple(WType t){
    WK.type=t; WK.t_start=millis(); WK.t_pause=0; WK.paused_total=0;
    WK.paused=false; WK.steps=0; WK.km=0; last_step_tick=millis();
    WK.hiit_phase=0; WK.hiit_phase_start=millis();
    open_workout_active();
}
static void open_home() { if (!g_apps_built) build_screen_apps(); goto_screen(SCREEN_APPS); }

// resolves open_watchface()'s home call to this OS's home screen
static void open_home_wrap(){ open_home(); }

// =====================================================================

// =====================================================================
//  3-CARD SWIPE LOOP full bodies (Face <-> Apps <-> Battery)
// =====================================================================
static void lv_touch_gesture(lv_indev_drv_t *, lv_indev_data_t *d) {
    int32_t x = FT3168->IIC_Read_Device_Value(FT3168->Arduino_IIC_Touch::Value_Information::TOUCH_COORDINATE_X);
    int32_t y = FT3168->IIC_Read_Device_Value(FT3168->Arduino_IIC_Touch::Value_Information::TOUCH_COORDINATE_Y);
    int     f = FT3168->IIC_Read_Device_Value(FT3168->Arduino_IIC_Touch::Value_Information::TOUCH_FINGER_NUMBER);
    bool pressed = (f > 0);
    static int zero_streak = 0;
    if (pressed) zero_streak = 0;
    else if (touchActive) {
        zero_streak++;
        if (zero_streak < 3) { pressed = true; x = tc_x; y = tc_y; }
    }
    if (pressed) {
        d->state = LV_INDEV_STATE_PR;
        d->point.x = x; d->point.y = y;
        if (!touchActive) { touchActive = true; ts_x=x; ts_y=y; ts_ms=millis(); }
        tc_x = x; tc_y = y;
    } else {
        d->state = LV_INDEV_STATE_REL;
        if (touchActive) {
            touchActive = false;
            uint32_t dur = millis() - ts_ms;
            int32_t dx = tc_x - ts_x;
            int32_t dy = tc_y - ts_y;
            if (dur < SWIPE_MAX_DURATION_MS &&
                abs(dx) > SWIPE_THRESHOLD_PX && abs(dx) > abs(dy)) {
                g_pending_swipe = (dx < 0) ? +1 : -1;
            }
        }
    }
}

static void goto_screen(int s) {
    while (s < 0)             s += NUM_SCREENS;
    while (s >= NUM_SCREENS)  s -= NUM_SCREENS;
    if (s == current_screen)  return;
    current_screen  = s;
    g_pending_swipe = 0;
    if (g_transient_screen) {
        lv_obj_del_async(g_transient_screen);
        g_transient_screen = nullptr;
    }
    if (screens[s])
        lv_scr_load_anim(screens[s], LV_SCR_LOAD_ANIM_FADE_ON,
                         SCREEN_SWITCH_ANIM_MS, 0, false);
}

static void open_app_screen(lv_obj_t *s) {
    lv_obj_t *prev = g_transient_screen;
    g_transient_screen = s;
    lv_scr_load_anim(s, LV_SCR_LOAD_ANIM_FADE_ON, 250, 0, false);
    if (prev) lv_obj_del_async(prev);
}

static lv_obj_t *batt_arc=nullptr, *batt_pct_lbl=nullptr;
static lv_obj_t *batt_state=nullptr, *batt_remain=nullptr;
static lv_obj_t *batt_volt=nullptr;

static int batt_remain_min_estimate(int pct) {
    if (pct <= 0) return 0;
    return (pct * 360) / 100;
}

static void update_battery_screen() {
    if (!batt_arc || !g_has_pmu) return;
    int pct = g_pmu.getBatteryPercent();
    if (pct < 0) pct = 0; if (pct > 100) pct = 100;
    lv_arc_set_value(batt_arc, pct);
    char b[16]; snprintf(b, 16, "%d%%", pct);
    lv_label_set_text(batt_pct_lbl, b);
    uint32_t col = pct<=15 ? 0xFF4040 : pct<=30 ? 0xFFB300 : 0x00FF80;
    lv_obj_set_style_arc_color(batt_arc, lv_color_hex(col), LV_PART_INDICATOR);
    lv_obj_set_style_text_color(batt_pct_lbl, lv_color_hex(col), 0);
    bool charging = g_pmu.isCharging();
    if (charging) {
        lv_label_set_text(batt_state, LV_SYMBOL_CHARGE "  charging");
        lv_obj_set_style_text_color(batt_state, lv_color_hex(0x00E5FF), 0);
        lv_label_set_text(batt_remain, "");
    } else {
        lv_label_set_text(batt_state, "on battery");
        lv_obj_set_style_text_color(batt_state, lv_color_hex(0xBBBBBB), 0);
        int mins = batt_remain_min_estimate(pct);
        char r[48];
        int h = mins/60, m = mins%60;
        if (h>0) snprintf(r,48,"~ %d h %02d min remaining", h, m);
        else     snprintf(r,48,"~ %d min remaining", m);
        lv_label_set_text(batt_remain, r);
    }
    float mv = g_pmu.getBattVoltage();
    char v[32]; snprintf(v, 32, "%.2f V", mv/1000.0f);
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
    lv_obj_set_style_text_font(batt_pct_lbl, &lv_font_montserrat_48, 0);
    lv_obj_align(batt_pct_lbl, LV_ALIGN_CENTER, 0, -10);

    batt_state = lv_label_create(s);
    lv_label_set_text(batt_state, "on battery");
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

    lv_obj_t *sleepBtn = lv_btn_create(s);
    lv_obj_set_size(sleepBtn, 175, 44);
    lv_obj_align(sleepBtn, LV_ALIGN_BOTTOM_RIGHT, -15, -40);
    lv_obj_set_style_radius(sleepBtn, 22, 0);
    lv_obj_set_style_bg_color(sleepBtn, lv_color_hex(0x141414), 0);
    lv_obj_set_style_border_color(sleepBtn, lv_color_hex(0x00E5FF), 0);
    lv_obj_set_style_border_width(sleepBtn, 1, 0);
    lv_obj_add_event_cb(sleepBtn, [](lv_event_t*){
        if (millis() - lastSwipeMs < CLICK_AFTER_SWIPE_MS) return;
        static_cast<Arduino_CO5300*>(gfx)->setBrightness(0);
        esp_sleep_enable_ext1_wakeup((1ULL<<BOOT_BTN), ESP_EXT1_WAKEUP_ANY_LOW);
        esp_deep_sleep_start();
    }, LV_EVENT_CLICKED, NULL);
    lv_obj_t *sl = lv_label_create(sleepBtn);
    lv_label_set_text(sl, LV_SYMBOL_POWER "  Sleep Now");
    lv_obj_set_style_text_color(sl, lv_color_hex(0x00E5FF), 0);
    lv_obj_set_style_text_font(sl, &lv_font_montserrat_16, 0);
    lv_obj_center(sl);

    lv_obj_t *launchBtn = lv_btn_create(s);
    lv_obj_set_size(launchBtn, 175, 44);
    lv_obj_align(launchBtn, LV_ALIGN_BOTTOM_LEFT, 15, -40);
    lv_obj_set_style_radius(launchBtn, 22, 0);
    lv_obj_set_style_bg_color(launchBtn, lv_color_hex(0x141414), 0);
    lv_obj_set_style_border_color(launchBtn, lv_color_hex(0x00FF80), 0);
    lv_obj_set_style_border_width(launchBtn, 1, 0);
    lv_obj_add_event_cb(launchBtn, [](lv_event_t*){
        if (millis() - lastSwipeMs < CLICK_AFTER_SWIPE_MS) return;
        return_to_launcher();
    }, LV_EVENT_CLICKED, NULL);
    lv_obj_t *ll = lv_label_create(launchBtn);
    lv_label_set_text(ll, LV_SYMBOL_HOME "  Launcher");
    lv_obj_set_style_text_color(ll, lv_color_hex(0x00FF80), 0);
    lv_obj_set_style_text_font(ll, &lv_font_montserrat_16, 0);
    lv_obj_center(ll);

    lv_obj_t *foot = lv_label_create(s);
    lv_label_set_text(foot, LV_SYMBOL_LEFT "  apps   \u00b7   watch face  " LV_SYMBOL_RIGHT);
    lv_obj_set_style_text_color(foot, lv_color_hex(0x444444), 0);
    lv_obj_set_style_text_font(foot, &lv_font_montserrat_12, 0);
    lv_obj_align(foot, LV_ALIGN_BOTTOM_MID, 0, -10);
}

static void build_screen_face() {
    lv_obj_t *s = lv_obj_create(NULL);
    screens[SCREEN_FACE] = s;
    lv_obj_set_style_bg_color(s, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(s, LV_OPA_COVER, 0);
    lv_obj_clear_flag(s, LV_OBJ_FLAG_SCROLLABLE);
    make_wallpaper_bg(s);

    wf_clk = lv_label_create(s);
    lv_label_set_text(wf_clk, "--:--");
    lv_obj_set_style_text_color(wf_clk, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(wf_clk, &lv_font_montserrat_48, 0);
    lv_obj_set_style_text_opa(wf_clk, 240, 0);
    lv_obj_align(wf_clk, LV_ALIGN_CENTER, 0, -30);

    wf_dt = lv_label_create(s);
    lv_label_set_text(wf_dt, "no sync");
    lv_obj_set_style_text_color(wf_dt, lv_color_hex(0xBBBBBB), 0);
    lv_obj_set_style_text_font(wf_dt, &lv_font_montserrat_18, 0);
    lv_obj_align(wf_dt, LV_ALIGN_CENTER, 0, 20);

    lv_obj_t *hint = lv_label_create(s);
    lv_label_set_text(hint, "swipe " LV_SYMBOL_RIGHT " for apps");
    lv_obj_set_style_text_color(hint, lv_color_hex(0x666666), 0);
    lv_obj_set_style_text_font(hint, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_letter_space(hint, 2, 0);
    lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -40);

    // 1 Hz time tick
    if (wf_tk) lv_timer_del(wf_tk);
    wf_tk = lv_timer_create(wf_upd, 1000, NULL);
    wf_upd(nullptr);
}

static void build_screen_apps() {
    lv_obj_t *s = lv_obj_create(NULL);
    screens[SCREEN_APPS] = s;
    lv_obj_set_style_bg_color(s, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(s, LV_OPA_COVER, 0);
    lv_obj_clear_flag(s, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title = lv_label_create(s);
    lv_label_set_text(title, "SPORT//OS");
    lv_obj_set_style_text_color(title, lv_color_hex(0x00FF80), 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_22, 0);
    lv_obj_set_style_text_letter_space(title, 4, 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 26);

    lv_obj_t *hint = lv_label_create(s);
    lv_label_set_text(hint, LV_SYMBOL_LEFT "  watch face   ·   battery  " LV_SYMBOL_RIGHT);
    lv_obj_set_style_text_color(hint, lv_color_hex(0x444444), 0);
    lv_obj_set_style_text_font(hint, &lv_font_montserrat_12, 0);
    lv_obj_align(hint, LV_ALIGN_TOP_MID, 0, 60);

    // Card list
    lv_obj_t *list = lv_obj_create(s);
    lv_obj_remove_style_all(list);
    lv_obj_set_size(list, LCD_W - 50, LCD_H - 160);
    lv_obj_align(list, LV_ALIGN_CENTER, 0, 30);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(list, 12, 0);
    lv_obj_set_style_pad_all(list, 8, 0);
    lv_obj_set_scrollbar_mode(list, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_scroll_dir(list, LV_DIR_VER);

    {
        lv_obj_t *card = lv_obj_create(list);
        lv_obj_remove_style_all(card);
        lv_obj_set_size(card, LV_PCT(100), 72);
        lv_obj_set_style_bg_color(card, lv_color_hex(0x0E0E0E), 0);
        lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(card, 18, 0);
        lv_obj_set_style_border_color(card, lv_color_hex(0x1C1C1C), 0);
        lv_obj_set_style_border_width(card, 1, 0);
        lv_obj_set_style_pad_all(card, 14, 0);
        lv_obj_set_style_bg_color(card, lv_color_hex(0x1A1A1A), LV_STATE_PRESSED);
        lv_obj_set_style_transform_zoom(card, 250, LV_STATE_PRESSED);
        lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

        lv_obj_t *bar = lv_obj_create(card); lv_obj_remove_style_all(bar);
        lv_obj_set_size(bar, 4, 38); lv_obj_align(bar, LV_ALIGN_LEFT_MID, 0, 0);
        lv_obj_set_style_radius(bar, 2, 0);
        lv_obj_set_style_bg_color(bar, lv_color_hex(0x00FF80), 0);
        lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);

        lv_obj_t *dot = lv_obj_create(card); lv_obj_remove_style_all(dot);
        lv_obj_set_size(dot, 36, 36); lv_obj_align(dot, LV_ALIGN_LEFT_MID, 18, 0);
        lv_obj_set_style_radius(dot, 18, 0);
        lv_obj_set_style_bg_color(dot, lv_color_hex(0x00FF80), 0);
        lv_obj_set_style_bg_opa(dot, 60, 0);
        lv_obj_set_style_border_color(dot, lv_color_hex(0x00FF80), 0);
        lv_obj_set_style_border_width(dot, 1, 0);
        lv_obj_t *dt = lv_label_create(dot);
        lv_label_set_text(dt, "R");
        lv_obj_set_style_text_color(dt, lv_color_hex(0x00FF80), 0);
        lv_obj_set_style_text_font(dt, &lv_font_montserrat_22, 0);
        lv_obj_center(dt);

        lv_obj_t *name = lv_label_create(card);
        lv_label_set_text(name, "RUN");
        lv_obj_set_style_text_color(name, lv_color_hex(0xFFFFFF), 0);
        lv_obj_set_style_text_font(name, &lv_font_montserrat_18, 0);
        lv_obj_set_style_text_letter_space(name, 2, 0);
        lv_obj_align(name, LV_ALIGN_LEFT_MID, 64, -10);

        lv_obj_t *desc = lv_label_create(card);
        lv_label_set_text(desc, "real-time pace + zones");
        lv_obj_set_style_text_color(desc, lv_color_hex(0x666666), 0);
        lv_obj_set_style_text_font(desc, &lv_font_montserrat_12, 0);
        lv_obj_set_style_text_letter_space(desc, 1, 0);
        lv_obj_align(desc, LV_ALIGN_LEFT_MID, 64, 12);

        lv_obj_t *chev = lv_label_create(card);
        lv_label_set_text(chev, LV_SYMBOL_RIGHT);
        lv_obj_set_style_text_color(chev, lv_color_hex(0x333333), 0);
        lv_obj_set_style_text_font(chev, &lv_font_montserrat_22, 0);
        lv_obj_align(chev, LV_ALIGN_RIGHT_MID, -8, 0);

        lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(card, [](lv_event_t*){
            if (millis() - lastSwipeMs < CLICK_AFTER_SWIPE_MS) return;
            open_pace_picker();
        }, LV_EVENT_CLICKED, NULL);
    }
    {
        lv_obj_t *card = lv_obj_create(list);
        lv_obj_remove_style_all(card);
        lv_obj_set_size(card, LV_PCT(100), 72);
        lv_obj_set_style_bg_color(card, lv_color_hex(0x0E0E0E), 0);
        lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(card, 18, 0);
        lv_obj_set_style_border_color(card, lv_color_hex(0x1C1C1C), 0);
        lv_obj_set_style_border_width(card, 1, 0);
        lv_obj_set_style_pad_all(card, 14, 0);
        lv_obj_set_style_bg_color(card, lv_color_hex(0x1A1A1A), LV_STATE_PRESSED);
        lv_obj_set_style_transform_zoom(card, 250, LV_STATE_PRESSED);
        lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

        lv_obj_t *bar = lv_obj_create(card); lv_obj_remove_style_all(bar);
        lv_obj_set_size(bar, 4, 38); lv_obj_align(bar, LV_ALIGN_LEFT_MID, 0, 0);
        lv_obj_set_style_radius(bar, 2, 0);
        lv_obj_set_style_bg_color(bar, lv_color_hex(0x00E5FF), 0);
        lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);

        lv_obj_t *dot = lv_obj_create(card); lv_obj_remove_style_all(dot);
        lv_obj_set_size(dot, 36, 36); lv_obj_align(dot, LV_ALIGN_LEFT_MID, 18, 0);
        lv_obj_set_style_radius(dot, 18, 0);
        lv_obj_set_style_bg_color(dot, lv_color_hex(0x00E5FF), 0);
        lv_obj_set_style_bg_opa(dot, 60, 0);
        lv_obj_set_style_border_color(dot, lv_color_hex(0x00E5FF), 0);
        lv_obj_set_style_border_width(dot, 1, 0);
        lv_obj_t *dt = lv_label_create(dot);
        lv_label_set_text(dt, "W");
        lv_obj_set_style_text_color(dt, lv_color_hex(0x00E5FF), 0);
        lv_obj_set_style_text_font(dt, &lv_font_montserrat_22, 0);
        lv_obj_center(dt);

        lv_obj_t *name = lv_label_create(card);
        lv_label_set_text(name, "WALK");
        lv_obj_set_style_text_color(name, lv_color_hex(0xFFFFFF), 0);
        lv_obj_set_style_text_font(name, &lv_font_montserrat_18, 0);
        lv_obj_set_style_text_letter_space(name, 2, 0);
        lv_obj_align(name, LV_ALIGN_LEFT_MID, 64, -10);

        lv_obj_t *desc = lv_label_create(card);
        lv_label_set_text(desc, "casual cadence");
        lv_obj_set_style_text_color(desc, lv_color_hex(0x666666), 0);
        lv_obj_set_style_text_font(desc, &lv_font_montserrat_12, 0);
        lv_obj_set_style_text_letter_space(desc, 1, 0);
        lv_obj_align(desc, LV_ALIGN_LEFT_MID, 64, 12);

        lv_obj_t *chev = lv_label_create(card);
        lv_label_set_text(chev, LV_SYMBOL_RIGHT);
        lv_obj_set_style_text_color(chev, lv_color_hex(0x333333), 0);
        lv_obj_set_style_text_font(chev, &lv_font_montserrat_22, 0);
        lv_obj_align(chev, LV_ALIGN_RIGHT_MID, -8, 0);

        lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(card, [](lv_event_t*){
            if (millis() - lastSwipeMs < CLICK_AFTER_SWIPE_MS) return;
            start_workout_simple(W_WALK);
        }, LV_EVENT_CLICKED, NULL);
    }
    {
        lv_obj_t *card = lv_obj_create(list);
        lv_obj_remove_style_all(card);
        lv_obj_set_size(card, LV_PCT(100), 72);
        lv_obj_set_style_bg_color(card, lv_color_hex(0x0E0E0E), 0);
        lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(card, 18, 0);
        lv_obj_set_style_border_color(card, lv_color_hex(0x1C1C1C), 0);
        lv_obj_set_style_border_width(card, 1, 0);
        lv_obj_set_style_pad_all(card, 14, 0);
        lv_obj_set_style_bg_color(card, lv_color_hex(0x1A1A1A), LV_STATE_PRESSED);
        lv_obj_set_style_transform_zoom(card, 250, LV_STATE_PRESSED);
        lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

        lv_obj_t *bar = lv_obj_create(card); lv_obj_remove_style_all(bar);
        lv_obj_set_size(bar, 4, 38); lv_obj_align(bar, LV_ALIGN_LEFT_MID, 0, 0);
        lv_obj_set_style_radius(bar, 2, 0);
        lv_obj_set_style_bg_color(bar, lv_color_hex(0xFFB300), 0);
        lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);

        lv_obj_t *dot = lv_obj_create(card); lv_obj_remove_style_all(dot);
        lv_obj_set_size(dot, 36, 36); lv_obj_align(dot, LV_ALIGN_LEFT_MID, 18, 0);
        lv_obj_set_style_radius(dot, 18, 0);
        lv_obj_set_style_bg_color(dot, lv_color_hex(0xFFB300), 0);
        lv_obj_set_style_bg_opa(dot, 60, 0);
        lv_obj_set_style_border_color(dot, lv_color_hex(0xFFB300), 0);
        lv_obj_set_style_border_width(dot, 1, 0);
        lv_obj_t *dt = lv_label_create(dot);
        lv_label_set_text(dt, "C");
        lv_obj_set_style_text_color(dt, lv_color_hex(0xFFB300), 0);
        lv_obj_set_style_text_font(dt, &lv_font_montserrat_22, 0);
        lv_obj_center(dt);

        lv_obj_t *name = lv_label_create(card);
        lv_label_set_text(name, "CYCLE");
        lv_obj_set_style_text_color(name, lv_color_hex(0xFFFFFF), 0);
        lv_obj_set_style_text_font(name, &lv_font_montserrat_18, 0);
        lv_obj_set_style_text_letter_space(name, 2, 0);
        lv_obj_align(name, LV_ALIGN_LEFT_MID, 64, -10);

        lv_obj_t *desc = lv_label_create(card);
        lv_label_set_text(desc, "distance + calories");
        lv_obj_set_style_text_color(desc, lv_color_hex(0x666666), 0);
        lv_obj_set_style_text_font(desc, &lv_font_montserrat_12, 0);
        lv_obj_set_style_text_letter_space(desc, 1, 0);
        lv_obj_align(desc, LV_ALIGN_LEFT_MID, 64, 12);

        lv_obj_t *chev = lv_label_create(card);
        lv_label_set_text(chev, LV_SYMBOL_RIGHT);
        lv_obj_set_style_text_color(chev, lv_color_hex(0x333333), 0);
        lv_obj_set_style_text_font(chev, &lv_font_montserrat_22, 0);
        lv_obj_align(chev, LV_ALIGN_RIGHT_MID, -8, 0);

        lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(card, [](lv_event_t*){
            if (millis() - lastSwipeMs < CLICK_AFTER_SWIPE_MS) return;
            start_workout_simple(W_CYCLE);
        }, LV_EVENT_CLICKED, NULL);
    }
    {
        lv_obj_t *card = lv_obj_create(list);
        lv_obj_remove_style_all(card);
        lv_obj_set_size(card, LV_PCT(100), 72);
        lv_obj_set_style_bg_color(card, lv_color_hex(0x0E0E0E), 0);
        lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(card, 18, 0);
        lv_obj_set_style_border_color(card, lv_color_hex(0x1C1C1C), 0);
        lv_obj_set_style_border_width(card, 1, 0);
        lv_obj_set_style_pad_all(card, 14, 0);
        lv_obj_set_style_bg_color(card, lv_color_hex(0x1A1A1A), LV_STATE_PRESSED);
        lv_obj_set_style_transform_zoom(card, 250, LV_STATE_PRESSED);
        lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

        lv_obj_t *bar = lv_obj_create(card); lv_obj_remove_style_all(bar);
        lv_obj_set_size(bar, 4, 38); lv_obj_align(bar, LV_ALIGN_LEFT_MID, 0, 0);
        lv_obj_set_style_radius(bar, 2, 0);
        lv_obj_set_style_bg_color(bar, lv_color_hex(0xFF4040), 0);
        lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);

        lv_obj_t *dot = lv_obj_create(card); lv_obj_remove_style_all(dot);
        lv_obj_set_size(dot, 36, 36); lv_obj_align(dot, LV_ALIGN_LEFT_MID, 18, 0);
        lv_obj_set_style_radius(dot, 18, 0);
        lv_obj_set_style_bg_color(dot, lv_color_hex(0xFF4040), 0);
        lv_obj_set_style_bg_opa(dot, 60, 0);
        lv_obj_set_style_border_color(dot, lv_color_hex(0xFF4040), 0);
        lv_obj_set_style_border_width(dot, 1, 0);
        lv_obj_t *dt = lv_label_create(dot);
        lv_label_set_text(dt, "H");
        lv_obj_set_style_text_color(dt, lv_color_hex(0xFF4040), 0);
        lv_obj_set_style_text_font(dt, &lv_font_montserrat_22, 0);
        lv_obj_center(dt);

        lv_obj_t *name = lv_label_create(card);
        lv_label_set_text(name, "HIIT");
        lv_obj_set_style_text_color(name, lv_color_hex(0xFFFFFF), 0);
        lv_obj_set_style_text_font(name, &lv_font_montserrat_18, 0);
        lv_obj_set_style_text_letter_space(name, 2, 0);
        lv_obj_align(name, LV_ALIGN_LEFT_MID, 64, -10);

        lv_obj_t *desc = lv_label_create(card);
        lv_label_set_text(desc, "interval timer");
        lv_obj_set_style_text_color(desc, lv_color_hex(0x666666), 0);
        lv_obj_set_style_text_font(desc, &lv_font_montserrat_12, 0);
        lv_obj_set_style_text_letter_space(desc, 1, 0);
        lv_obj_align(desc, LV_ALIGN_LEFT_MID, 64, 12);

        lv_obj_t *chev = lv_label_create(card);
        lv_label_set_text(chev, LV_SYMBOL_RIGHT);
        lv_obj_set_style_text_color(chev, lv_color_hex(0x333333), 0);
        lv_obj_set_style_text_font(chev, &lv_font_montserrat_22, 0);
        lv_obj_align(chev, LV_ALIGN_RIGHT_MID, -8, 0);

        lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(card, [](lv_event_t*){
            if (millis() - lastSwipeMs < CLICK_AFTER_SWIPE_MS) return;
            start_workout_simple(W_HIIT);
        }, LV_EVENT_CLICKED, NULL);
    }
    {
        lv_obj_t *card = lv_obj_create(list);
        lv_obj_remove_style_all(card);
        lv_obj_set_size(card, LV_PCT(100), 72);
        lv_obj_set_style_bg_color(card, lv_color_hex(0x0E0E0E), 0);
        lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(card, 18, 0);
        lv_obj_set_style_border_color(card, lv_color_hex(0x1C1C1C), 0);
        lv_obj_set_style_border_width(card, 1, 0);
        lv_obj_set_style_pad_all(card, 14, 0);
        lv_obj_set_style_bg_color(card, lv_color_hex(0x1A1A1A), LV_STATE_PRESSED);
        lv_obj_set_style_transform_zoom(card, 250, LV_STATE_PRESSED);
        lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

        lv_obj_t *bar = lv_obj_create(card); lv_obj_remove_style_all(bar);
        lv_obj_set_size(bar, 4, 38); lv_obj_align(bar, LV_ALIGN_LEFT_MID, 0, 0);
        lv_obj_set_style_radius(bar, 2, 0);
        lv_obj_set_style_bg_color(bar, lv_color_hex(0xFF2EC4), 0);
        lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);

        lv_obj_t *dot = lv_obj_create(card); lv_obj_remove_style_all(dot);
        lv_obj_set_size(dot, 36, 36); lv_obj_align(dot, LV_ALIGN_LEFT_MID, 18, 0);
        lv_obj_set_style_radius(dot, 18, 0);
        lv_obj_set_style_bg_color(dot, lv_color_hex(0xFF2EC4), 0);
        lv_obj_set_style_bg_opa(dot, 60, 0);
        lv_obj_set_style_border_color(dot, lv_color_hex(0xFF2EC4), 0);
        lv_obj_set_style_border_width(dot, 1, 0);
        lv_obj_t *dt = lv_label_create(dot);
        lv_label_set_text(dt, "S");
        lv_obj_set_style_text_color(dt, lv_color_hex(0xFF2EC4), 0);
        lv_obj_set_style_text_font(dt, &lv_font_montserrat_22, 0);
        lv_obj_center(dt);

        lv_obj_t *name = lv_label_create(card);
        lv_label_set_text(name, "STATS");
        lv_obj_set_style_text_color(name, lv_color_hex(0xFFFFFF), 0);
        lv_obj_set_style_text_font(name, &lv_font_montserrat_18, 0);
        lv_obj_set_style_text_letter_space(name, 2, 0);
        lv_obj_align(name, LV_ALIGN_LEFT_MID, 64, -10);

        lv_obj_t *desc = lv_label_create(card);
        lv_label_set_text(desc, "XP / streak / sessions");
        lv_obj_set_style_text_color(desc, lv_color_hex(0x666666), 0);
        lv_obj_set_style_text_font(desc, &lv_font_montserrat_12, 0);
        lv_obj_set_style_text_letter_space(desc, 1, 0);
        lv_obj_align(desc, LV_ALIGN_LEFT_MID, 64, 12);

        lv_obj_t *chev = lv_label_create(card);
        lv_label_set_text(chev, LV_SYMBOL_RIGHT);
        lv_obj_set_style_text_color(chev, lv_color_hex(0x333333), 0);
        lv_obj_set_style_text_font(chev, &lv_font_montserrat_22, 0);
        lv_obj_align(chev, LV_ALIGN_RIGHT_MID, -8, 0);

        lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(card, [](lv_event_t*){
            if (millis() - lastSwipeMs < CLICK_AFTER_SWIPE_MS) return;
            open_stats();
        }, LV_EVENT_CLICKED, NULL);
    }

    g_apps_built = true;
}

void setup() {
    hw_boot();
    splash_show();
load_player();
    build_screen_face();
    build_screen_apps();
    build_screen_battery();
    current_screen = SCREEN_FACE;
    lv_scr_load_anim(screens[SCREEN_FACE], LV_SCR_LOAD_ANIM_FADE_ON, 260, 0, false);
}
void loop() {
    lv_timer_handler();
    uint32_t now = millis();

    // Consume queued swipe
    if (g_pending_swipe != 0 && g_transient_screen == nullptr) {
        int ev = g_pending_swipe; g_pending_swipe = 0;
        lastSwipeMs = now;
        goto_screen(current_screen + ev);
    }

    // 1 s tick (battery screen)
    static uint32_t tB = 0;
    if (now - tB > 1000) {
        tB = now;
        if (current_screen == SCREEN_BATTERY) update_battery_screen();
    }

    // 2 s tick (legacy top-strip on transient screens)
    static uint32_t t1 = 0;
    if (now - t1 > 2000) {
        t1 = now;
        // tick_top() is only safe when its globals are alive.  They are
        // built by transient screens that use the legacy top-strip; on the
        // persistent 3-card screens they're not used.  Guard accordingly.
        extern void tick_top();
        if (g_transient_screen) tick_top();
    }

    // BOOT button long-press -> back to LauncherOS
    static uint32_t btn_t = 0;
    if (digitalRead(BOOT_BTN) == LOW) {
        if (!btn_t) btn_t = now;
        else if (now - btn_t > 1500) return_to_launcher();
    } else btn_t = 0;

    delay(5);
}
