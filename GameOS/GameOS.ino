// =====================================================================
//  GameOS.ino   --   LauncherOS-compatible Game console module
//  Target  : Waveshare ESP32-S3 Touch AMOLED 2.06  (410 x 502, FT3168, AXP2101)
//  Build   : ESP32-S3 Dev Module, PSRAM=OPI, Partition=Minimal SPIFFS (large APP)
//  Output  : Export Compiled Binary -> copy GameOS.ino.bin to /WATCH/bin/gameos.bin
//
//  Features:
//    - Custom CRT-scanline boot splash ("GAME//OS")
//    - LauncherOS visual language (cyan accent + neon magenta game accent)
//    - Battery pill + clock with WiFi sync (reads /WATCH/wifi/config.txt)
//    - Auto-creates /WATCH/games/, /WATCH/games/saves/
//    - Game library: scans /WATCH/games/*.gb /*.nes /*.txt manifests
//    - Built-in instant-play games: SNAKE, PONG (LVGL canvas, full touch control)
//    - Suspend/resume snapshot to NVS namespace "gameos"
//    - Back-to-Launcher pill + BOOT long-press (1.5s) safety exit
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

// ---- LauncherOS-matched pin map -------------------------------------
#define LCD_SDIO0 4
#define LCD_SDIO1 5
#define LCD_SDIO2 6
#define LCD_SDIO3 7
#define LCD_SCLK 11
#define LCD_CS   12
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

// ---- Hardcoded WiFi fallback (matches LauncherOS) -------------------
#define HC_WIFI_SSID  "GameOS-Fallback"
#define HC_WIFI_PASS  "changeme"

// ---- Display / Touch / PMU singletons --------------------------------
Arduino_DataBus *g_bus = new Arduino_ESP32QSPI(LCD_CS,LCD_SCLK,LCD_SDIO0,LCD_SDIO1,LCD_SDIO2,LCD_SDIO3);
Arduino_GFX     *gfx   = new Arduino_CO5300(g_bus,LCD_RESET,0,LCD_W,LCD_H,22,0,0,0);
std::shared_ptr<Arduino_IIC_DriveBus> g_iic = std::make_shared<Arduino_HWIIC>(IIC_SDA,IIC_SCL,&Wire);
void Arduino_IIC_Touch_Interrupt(void);
std::unique_ptr<Arduino_IIC> FT3168(new Arduino_FT3x68(g_iic,FT3168_DEVICE_ADDRESS,DRIVEBUS_DEFAULT_VALUE,TP_INT,Arduino_IIC_Touch_Interrupt));
void Arduino_IIC_Touch_Interrupt(void){FT3168->IIC_Interrupt_Flag=true;}
XPowersPMU g_pmu; bool g_has_pmu=false;

// ---- LVGL plumbing ---------------------------------------------------
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

// ---- Back-to-launcher (MANDATORY) ------------------------------------
// Boot the FACTORY partition (where LauncherOS lives).  Using
// esp_ota_get_next_update_partition() alone skips the factory slot, so
// with a single ota_0 layout it returned the running app and the watch
// just rebooted back into this app.  Explicit factory lookup fixes it.

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

// ---- WiFi config loader (same parser as LauncherOS) ------------------
struct WifiCred{ String ssid,pass; };
static WifiCred g_creds[6]; static int g_creds_n=0; static int g_gmt=0;
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
            if(k=="WIFI"+String(i)+"_SSID"){ if(g_creds_n<5){g_creds[g_creds_n].ssid=v;} }
            if(k=="WIFI"+String(i)+"_PASS"){ if(g_creds_n<5){g_creds[g_creds_n].pass=v; g_creds_n++;} }
        }
        if(k=="GMT_OFFSET") g_gmt=v.toInt();
    }
    f.close();
    g_creds[g_creds_n++]={HC_WIFI_SSID,HC_WIFI_PASS};
}
static bool g_wifi_synced=false;
static void wifi_quick_sync(){
    if(g_creds_n==0) return;
    WiFi.mode(WIFI_STA);
    for(int i=0;i<g_creds_n;i++){
        WiFi.begin(g_creds[i].ssid.c_str(),g_creds[i].pass.c_str());
        uint32_t t=millis();
        while(WiFi.status()!=WL_CONNECTED && millis()-t<4000) delay(100);
        if(WiFi.status()==WL_CONNECTED){
            configTime(g_gmt*3600,0,"pool.ntp.org","time.google.com");
            struct tm tm; getLocalTime(&tm,3000);
            g_wifi_synced=true; break;
        }
    }
    WiFi.disconnect(true); WiFi.mode(WIFI_OFF);
}

// ---- SD bootstrap ----------------------------------------------------
static void sd_ensure_dirs(){
    if(!SD_MMC.exists("/WATCH")) SD_MMC.mkdir("/WATCH");
    if(!SD_MMC.exists("/WATCH/games")) SD_MMC.mkdir("/WATCH/games");
    if(!SD_MMC.exists("/WATCH/games/saves")) SD_MMC.mkdir("/WATCH/games/saves");
}

// ---- Shared boot ------------------------------------------------------
static void hw_boot(){
    Serial.begin(115200);
    pinMode(BOOT_BTN,INPUT_PULLUP); pinMode(PWR_BTN,INPUT_PULLUP); btStop();
    if(!gfx->begin()) Serial.println("gfx begin fail");
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
    SD_MMC.setPins(SDMMC_CLK,SDMMC_CMD,SDMMC_DATA);
    SD_MMC.begin("/sdcard",true);
    sd_ensure_dirs();
    load_wifi_config();
}

// =====================================================================
//   GAMEOS CUSTOM LOADING SPLASH (CRT scanline + glitch)
// =====================================================================
static void splash_show(){
    lv_obj_t *s=lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s,lv_color_hex(0x000000),0);
    lv_obj_set_style_bg_opa(s,LV_OPA_COVER,0);
    lv_obj_clear_flag(s,LV_OBJ_FLAG_SCROLLABLE);

    // scanlines
    for(int y=0;y<LCD_H;y+=4){
        lv_obj_t *ln=lv_obj_create(s);
        lv_obj_remove_style_all(ln);
        lv_obj_set_size(ln,LCD_W,1);
        lv_obj_set_pos(ln,0,y);
        lv_obj_set_style_bg_color(ln,lv_color_hex(0x111111),0);
        lv_obj_set_style_bg_opa(ln,80,0);
    }
    lv_obj_t *t=lv_label_create(s);
    lv_label_set_text(t,"GAME//OS");
    lv_obj_set_style_text_color(t,lv_color_hex(0xFF2EC4),0);
    lv_obj_set_style_text_font(t,&lv_font_montserrat_48,0);
    lv_obj_align(t,LV_ALIGN_CENTER,0,-30);

    lv_obj_t *t2=lv_label_create(s);
    lv_label_set_text(t2,"INITIALIZING ARCADE CORE");
    lv_obj_set_style_text_color(t2,lv_color_hex(0x00E5FF),0);
    lv_obj_set_style_text_font(t2,&lv_font_montserrat_14,0);
    lv_obj_set_style_text_letter_space(t2,3,0);
    lv_obj_align(t2,LV_ALIGN_CENTER,0,20);

    lv_obj_t *bar=lv_bar_create(s);
    lv_obj_set_size(bar,260,4);
    lv_obj_align(bar,LV_ALIGN_CENTER,0,60);
    lv_obj_set_style_bg_color(bar,lv_color_hex(0x1C1C1C),LV_PART_MAIN);
    lv_obj_set_style_bg_color(bar,lv_color_hex(0xFF2EC4),LV_PART_INDICATOR);
    lv_obj_set_style_radius(bar,2,LV_PART_MAIN);
    lv_obj_set_style_radius(bar,2,LV_PART_INDICATOR);
    lv_bar_set_range(bar,0,100); lv_bar_set_value(bar,0,LV_ANIM_OFF);
    lv_scr_load(s);

    const char *steps[]={"MOUNT SD","SCAN /games","LOAD SAVES","SYNC RTC","READY"};
    for(int p=0;p<=100;p+=4){
        lv_bar_set_value(bar,p,LV_ANIM_OFF);
        if(p%20==0) lv_label_set_text(t2,steps[p/20>4?4:p/20]);
        lv_timer_handler(); delay(28);
        if(p==40) wifi_quick_sync();
    }
}

// =====================================================================
//   SHARED CARDS (battery + clock) - LauncherOS look
// =====================================================================
static lv_obj_t *g_batt_fill,*g_batt_pct,*g_batt_bolt,*g_clock_lbl,*g_date_lbl,*g_wifi_dot;

static void make_top_strip(lv_obj_t *p){
    // Battery pill
    lv_obj_t *card=lv_obj_create(p); lv_obj_remove_style_all(card);
    lv_obj_set_size(card,110,36); lv_obj_align(card,LV_ALIGN_TOP_LEFT,20,18);
    lv_obj_set_style_bg_color(card,lv_color_hex(0x0E0E0E),0); lv_obj_set_style_bg_opa(card,LV_OPA_COVER,0);
    lv_obj_set_style_radius(card,18,0);
    lv_obj_set_style_border_color(card,lv_color_hex(0x1C1C1C),0); lv_obj_set_style_border_width(card,1,0);
    g_batt_fill=lv_obj_create(card); lv_obj_remove_style_all(g_batt_fill);
    lv_obj_set_size(g_batt_fill,0,18); lv_obj_align(g_batt_fill,LV_ALIGN_LEFT_MID,8,0);
    lv_obj_set_style_radius(g_batt_fill,9,0); lv_obj_set_style_bg_opa(g_batt_fill,LV_OPA_COVER,0);
    g_batt_bolt=lv_label_create(card); lv_label_set_text(g_batt_bolt,LV_SYMBOL_CHARGE);
    lv_obj_set_style_text_color(g_batt_bolt,lv_color_hex(0x00FF80),0);
    lv_obj_align(g_batt_bolt,LV_ALIGN_LEFT_MID,6,0); lv_obj_add_flag(g_batt_bolt,LV_OBJ_FLAG_HIDDEN);
    g_batt_pct=lv_label_create(card); lv_label_set_text(g_batt_pct,"--%");
    lv_obj_set_style_text_color(g_batt_pct,lv_color_hex(0xFFFFFF),0);
    lv_obj_set_style_text_font(g_batt_pct,&lv_font_montserrat_14,0);
    lv_obj_align(g_batt_pct,LV_ALIGN_RIGHT_MID,-10,0);

    // WiFi dot
    g_wifi_dot=lv_obj_create(p); lv_obj_remove_style_all(g_wifi_dot);
    lv_obj_set_size(g_wifi_dot,10,10); lv_obj_align(g_wifi_dot,LV_ALIGN_TOP_RIGHT,-22,30);
    lv_obj_set_style_radius(g_wifi_dot,5,0); lv_obj_set_style_bg_opa(g_wifi_dot,LV_OPA_COVER,0);
    lv_obj_set_style_bg_color(g_wifi_dot,lv_color_hex(g_wifi_synced?0x00FF80:0xFF4040),0);

    // Clock
    g_clock_lbl=lv_label_create(p); lv_label_set_text(g_clock_lbl,"--:--");
    lv_obj_set_style_text_color(g_clock_lbl,lv_color_hex(0xFFFFFF),0);
    lv_obj_set_style_text_font(g_clock_lbl,&lv_font_montserrat_28,0);
    lv_obj_align(g_clock_lbl,LV_ALIGN_TOP_MID,0,70);
    g_date_lbl=lv_label_create(p); lv_label_set_text(g_date_lbl,"no sync");
    lv_obj_set_style_text_color(g_date_lbl,lv_color_hex(0x888888),0);
    lv_obj_set_style_text_font(g_date_lbl,&lv_font_montserrat_14,0);
    lv_obj_align(g_date_lbl,LV_ALIGN_TOP_MID,0,108);
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
//   GAME LAUNCHER UI
// =====================================================================
static lv_obj_t *g_root;
static void make_back_pill(lv_obj_t *p){
    lv_obj_t *b=lv_btn_create(p);
    lv_obj_set_size(b,220,50); lv_obj_align(b,LV_ALIGN_BOTTOM_MID,0,-22);
    lv_obj_set_style_bg_color(b,lv_color_hex(0x141414),0);
    lv_obj_set_style_border_color(b,lv_color_hex(0x00E5FF),0);
    lv_obj_set_style_border_width(b,1,0); lv_obj_set_style_radius(b,25,0);
    lv_obj_t *l=lv_label_create(b);
    lv_label_set_text(l,LV_SYMBOL_LEFT "  Back to Launcher");
    lv_obj_set_style_text_color(l,lv_color_hex(0x00E5FF),0);
    lv_obj_set_style_text_font(l,&lv_font_montserrat_16,0); lv_obj_center(l);
    lv_obj_add_event_cb(b,[](lv_event_t*){ return_to_launcher(); },LV_EVENT_CLICKED,NULL);
}

// ---- Mini-game: SNAKE ------------------------------------------------
#define GRID 20
static int snake_x[64],snake_y[64],snake_len=4,snake_dx=1,snake_dy=0,food_x=10,food_y=10,score=0;
static lv_obj_t *snake_canvas=nullptr; static lv_color_t *snake_buf=nullptr;
static bool snake_running=false; static lv_timer_t *snake_timer=nullptr;
static void snake_draw(){
    if(!snake_canvas) return;
    lv_canvas_fill_bg(snake_canvas,lv_color_hex(0x050505),LV_OPA_COVER);
    lv_draw_rect_dsc_t r; lv_draw_rect_dsc_init(&r);
    r.bg_color=lv_color_hex(0xFF2EC4); r.bg_opa=LV_OPA_COVER; r.radius=2;
    lv_canvas_draw_rect(snake_canvas,food_x*GRID,food_y*GRID,GRID-2,GRID-2,&r);
    r.bg_color=lv_color_hex(0x00E5FF);
    for(int i=0;i<snake_len;i++) lv_canvas_draw_rect(snake_canvas,snake_x[i]*GRID,snake_y[i]*GRID,GRID-2,GRID-2,&r);
}
static lv_obj_t *snake_score_lbl=nullptr;
static void snake_step(lv_timer_t*){
    for(int i=snake_len-1;i>0;i--){snake_x[i]=snake_x[i-1];snake_y[i]=snake_y[i-1];}
    snake_x[0]+=snake_dx; snake_y[0]+=snake_dy;
    if(snake_x[0]<0||snake_x[0]>=18||snake_y[0]<0||snake_y[0]>=20){snake_len=4;snake_dx=1;snake_dy=0;snake_x[0]=2;snake_y[0]=2;score=0;}
    if(snake_x[0]==food_x&&snake_y[0]==food_y){snake_len=min(snake_len+1,63);food_x=random(18);food_y=random(20);score+=10;}
    snake_draw();
    if(snake_score_lbl){char s[16]; snprintf(s,16,"SCORE  %d",score); lv_label_set_text(snake_score_lbl,s);}
}
static void snake_swipe(lv_event_t *e){
    lv_indev_t *in=lv_indev_get_act();
    lv_dir_t d=lv_indev_get_gesture_dir(in);
    if(d==LV_DIR_LEFT && snake_dx==0){snake_dx=-1;snake_dy=0;}
    if(d==LV_DIR_RIGHT && snake_dx==0){snake_dx=1;snake_dy=0;}
    if(d==LV_DIR_TOP && snake_dy==0){snake_dx=0;snake_dy=-1;}
    if(d==LV_DIR_BOTTOM && snake_dy==0){snake_dx=0;snake_dy=1;}
}
static void open_snake();
static void open_pong();
static void open_launcher_screen();

static void open_snake(){
    lv_obj_t *s=lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s,lv_color_hex(0x000000),0); lv_obj_set_style_bg_opa(s,LV_OPA_COVER,0);
    lv_obj_clear_flag(s,LV_OBJ_FLAG_SCROLLABLE);
    snake_score_lbl=lv_label_create(s); lv_label_set_text(snake_score_lbl,"SCORE  0");
    lv_obj_set_style_text_color(snake_score_lbl,lv_color_hex(0x00E5FF),0);
    lv_obj_set_style_text_font(snake_score_lbl,&lv_font_montserrat_22,0);
    lv_obj_align(snake_score_lbl,LV_ALIGN_TOP_MID,0,30);

    snake_buf=(lv_color_t*)heap_caps_malloc(360*400*sizeof(lv_color_t),MALLOC_CAP_8BIT);
    snake_canvas=lv_canvas_create(s);
    lv_canvas_set_buffer(snake_canvas,snake_buf,360,400,LV_IMG_CF_TRUE_COLOR);
    lv_obj_align(snake_canvas,LV_ALIGN_CENTER,0,-10);
    snake_len=4; snake_dx=1; snake_dy=0; score=0;
    for(int i=0;i<4;i++){snake_x[i]=4-i; snake_y[i]=10;}
    lv_obj_add_event_cb(s,snake_swipe,LV_EVENT_GESTURE,NULL);
    if(snake_timer) lv_timer_del(snake_timer);
    snake_timer=lv_timer_create(snake_step,150,NULL);

    lv_obj_t *b=lv_btn_create(s);
    lv_obj_set_size(b,180,42); lv_obj_align(b,LV_ALIGN_BOTTOM_MID,0,-20);
    lv_obj_set_style_bg_color(b,lv_color_hex(0x141414),0);
    lv_obj_set_style_border_color(b,lv_color_hex(0xFF2EC4),0);
    lv_obj_set_style_border_width(b,1,0); lv_obj_set_style_radius(b,21,0);
    lv_obj_t *l=lv_label_create(b); lv_label_set_text(l,LV_SYMBOL_LEFT "  Exit Game");
    lv_obj_set_style_text_color(l,lv_color_hex(0xFF2EC4),0);
    lv_obj_set_style_text_font(l,&lv_font_montserrat_14,0); lv_obj_center(l);
    lv_obj_add_event_cb(b,[](lv_event_t*){ if(snake_timer){lv_timer_del(snake_timer);snake_timer=nullptr;} if(snake_buf){free(snake_buf);snake_buf=nullptr;} open_launcher_screen();},LV_EVENT_CLICKED,NULL);
    open_app_screen(s);
}

// ---- Mini-game: PONG (single-player vs AI) --------------------------
static float pong_bx,pong_by,pong_vx,pong_vy; static int pong_py,pong_aiy,pong_s_p=0,pong_s_a=0;
static lv_obj_t *pong_canvas=nullptr; static lv_color_t *pong_buf=nullptr;
static lv_timer_t *pong_timer=nullptr; static lv_obj_t *pong_score=nullptr;
static void pong_step(lv_timer_t*){
    pong_bx+=pong_vx; pong_by+=pong_vy;
    if(pong_by<0||pong_by>380) pong_vy=-pong_vy;
    pong_aiy += (pong_by-pong_aiy-40 > 0) ? 3 : -3;
    if(pong_bx<20 && pong_by>pong_py && pong_by<pong_py+80) pong_vx=-pong_vx;
    if(pong_bx>340 && pong_by>pong_aiy && pong_by<pong_aiy+80) pong_vx=-pong_vx;
    if(pong_bx<0){pong_s_a++; pong_bx=180; pong_by=200; pong_vx=2.5; pong_vy=1.8;}
    if(pong_bx>360){pong_s_p++; pong_bx=180; pong_by=200; pong_vx=-2.5; pong_vy=1.8;}
    lv_canvas_fill_bg(pong_canvas,lv_color_hex(0x050505),LV_OPA_COVER);
    lv_draw_rect_dsc_t r; lv_draw_rect_dsc_init(&r);
    r.bg_color=lv_color_hex(0x00E5FF); r.bg_opa=LV_OPA_COVER; r.radius=2;
    lv_canvas_draw_rect(pong_canvas,10,pong_py,8,80,&r);
    r.bg_color=lv_color_hex(0xFF2EC4);
    lv_canvas_draw_rect(pong_canvas,342,pong_aiy,8,80,&r);
    r.bg_color=lv_color_hex(0xFFFFFF); r.radius=6;
    lv_canvas_draw_rect(pong_canvas,(int)pong_bx,(int)pong_by,12,12,&r);
    if(pong_score){char s[24]; snprintf(s,24,"%d   :   %d",pong_s_p,pong_s_a); lv_label_set_text(pong_score,s);}
}
static void pong_drag(lv_event_t *e){
    lv_indev_t *in=lv_indev_get_act();
    lv_point_t p; lv_indev_get_point(in,&p);
    int local=p.y - (LCD_H/2 - 200);
    pong_py=constrain(local-40,0,320);
}
static void open_pong(){
    lv_obj_t *s=lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s,lv_color_hex(0x000000),0); lv_obj_set_style_bg_opa(s,LV_OPA_COVER,0);
    lv_obj_clear_flag(s,LV_OBJ_FLAG_SCROLLABLE);
    pong_score=lv_label_create(s); lv_label_set_text(pong_score,"0   :   0");
    lv_obj_set_style_text_color(pong_score,lv_color_hex(0xFFFFFF),0);
    lv_obj_set_style_text_font(pong_score,&lv_font_montserrat_22,0);
    lv_obj_align(pong_score,LV_ALIGN_TOP_MID,0,30);
    pong_buf=(lv_color_t*)heap_caps_malloc(360*400*sizeof(lv_color_t),MALLOC_CAP_8BIT);
    pong_canvas=lv_canvas_create(s); lv_canvas_set_buffer(pong_canvas,pong_buf,360,400,LV_IMG_CF_TRUE_COLOR);
    lv_obj_align(pong_canvas,LV_ALIGN_CENTER,0,-10);
    pong_bx=180; pong_by=200; pong_vx=2.5; pong_vy=1.8; pong_py=160; pong_aiy=160; pong_s_p=0; pong_s_a=0;
    lv_obj_add_event_cb(pong_canvas,pong_drag,LV_EVENT_PRESSING,NULL);
    if(pong_timer) lv_timer_del(pong_timer);
    pong_timer=lv_timer_create(pong_step,33,NULL);
    lv_obj_t *b=lv_btn_create(s);
    lv_obj_set_size(b,180,42); lv_obj_align(b,LV_ALIGN_BOTTOM_MID,0,-20);
    lv_obj_set_style_bg_color(b,lv_color_hex(0x141414),0);
    lv_obj_set_style_border_color(b,lv_color_hex(0xFF2EC4),0);
    lv_obj_set_style_border_width(b,1,0); lv_obj_set_style_radius(b,21,0);
    lv_obj_t *l=lv_label_create(b); lv_label_set_text(l,LV_SYMBOL_LEFT "  Exit Game");
    lv_obj_set_style_text_color(l,lv_color_hex(0xFF2EC4),0);
    lv_obj_set_style_text_font(l,&lv_font_montserrat_14,0); lv_obj_center(l);
    lv_obj_add_event_cb(b,[](lv_event_t*){if(pong_timer){lv_timer_del(pong_timer);pong_timer=nullptr;} if(pong_buf){free(pong_buf);pong_buf=nullptr;} open_launcher_screen();},LV_EVENT_CLICKED,NULL);
    open_app_screen(s);
}

// ---- Launcher screen (game tiles) -----------------------------------
static void tile_cb(lv_event_t *e){
    int id=(int)(intptr_t)lv_event_get_user_data(e);
    if(id==0) open_snake();
    else if(id==1) open_pong();
}
static void open_launcher_screen() { if (!g_apps_built) build_screen_apps(); goto_screen(SCREEN_APPS); }

// resolves open_watchface()'s home call to this OS's home screen
static void open_home_wrap(){ open_launcher_screen(); }

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
    lv_label_set_text(title, "GAME//OS");
    lv_obj_set_style_text_color(title, lv_color_hex(0x00E5FF), 0);
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
        lv_label_set_text(dt, "S");
        lv_obj_set_style_text_color(dt, lv_color_hex(0x00FF80), 0);
        lv_obj_set_style_text_font(dt, &lv_font_montserrat_22, 0);
        lv_obj_center(dt);

        lv_obj_t *name = lv_label_create(card);
        lv_label_set_text(name, "SNAKE");
        lv_obj_set_style_text_color(name, lv_color_hex(0xFFFFFF), 0);
        lv_obj_set_style_text_font(name, &lv_font_montserrat_18, 0);
        lv_obj_set_style_text_letter_space(name, 2, 0);
        lv_obj_align(name, LV_ALIGN_LEFT_MID, 64, -10);

        lv_obj_t *desc = lv_label_create(card);
        lv_label_set_text(desc, "swipe to steer");
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
            open_snake();
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
        lv_label_set_text(dt, "P");
        lv_obj_set_style_text_color(dt, lv_color_hex(0x00E5FF), 0);
        lv_obj_set_style_text_font(dt, &lv_font_montserrat_22, 0);
        lv_obj_center(dt);

        lv_obj_t *name = lv_label_create(card);
        lv_label_set_text(name, "PONG");
        lv_obj_set_style_text_color(name, lv_color_hex(0xFFFFFF), 0);
        lv_obj_set_style_text_font(name, &lv_font_montserrat_18, 0);
        lv_obj_set_style_text_letter_space(name, 2, 0);
        lv_obj_align(name, LV_ALIGN_LEFT_MID, 64, -10);

        lv_obj_t *desc = lv_label_create(card);
        lv_label_set_text(desc, "drag to defend");
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
            open_pong();
        }, LV_EVENT_CLICKED, NULL);
    }

    g_apps_built = true;
}

void setup() {
    hw_boot();
    splash_show();

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
