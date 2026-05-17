// =====================================================================
//  MediaOS.ino  --  LauncherOS-compatible Media center module
//  Target  : Waveshare ESP32-S3 Touch AMOLED 2.06
//  Build   : ESP32-S3 Dev Module, PSRAM=OPI, Partition=Minimal SPIFFS (large APP)
//  Output  : Export Compiled Binary -> copy MediaOS.ino.bin to /WATCH/bin/mediaos.bin
//
//  Features:
//    - Custom vinyl-spin loading splash ("MEDIA::OS")
//    - LauncherOS visual language (cyan + warm amber media accent)
//    - Battery pill + clock + WiFi sync (reads /WATCH/wifi/config.txt)
//    - Auto-creates /WATCH/media/{music,anime,manga}/
//    - Music browser: lists *.mp3 / *.wav from /WATCH/media/music/ + transport UI
//    - Manga reader: paginates folders under /WATCH/media/manga/<title>/page_*.raw
//    - Anime tracker: read/edit /WATCH/media/anime/watchlist.txt (title|S|E|status)
//    - Ambient visualizer (CSS-free LVGL canvas waveform)
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
#include <vector>

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
#define HC_WIFI_SSID "MediaOS-Fallback"
#define HC_WIFI_PASS "changeme"
#define ACCENT_MEDIA 0xFFB300

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
    if(!SD_MMC.exists("/WATCH/media")) SD_MMC.mkdir("/WATCH/media");
    if(!SD_MMC.exists("/WATCH/media/music")) SD_MMC.mkdir("/WATCH/media/music");
    if(!SD_MMC.exists("/WATCH/media/anime")) SD_MMC.mkdir("/WATCH/media/anime");
    if(!SD_MMC.exists("/WATCH/media/manga")) SD_MMC.mkdir("/WATCH/media/manga");
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
//  MEDIAOS CUSTOM LOADING SPLASH (vinyl spin + ring glow)
// =====================================================================
static lv_obj_t *spl_arc=nullptr;
static void splash_show(){
    lv_obj_t *s=lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s,lv_color_hex(0x000000),0);
    lv_obj_set_style_bg_opa(s,LV_OPA_COVER,0);
    lv_obj_clear_flag(s,LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *ring=lv_obj_create(s); lv_obj_remove_style_all(ring);
    lv_obj_set_size(ring,260,260); lv_obj_align(ring,LV_ALIGN_CENTER,0,-30);
    lv_obj_set_style_radius(ring,130,0);
    lv_obj_set_style_bg_color(ring,lv_color_hex(0x0E0E0E),0);
    lv_obj_set_style_bg_opa(ring,LV_OPA_COVER,0);
    lv_obj_set_style_border_color(ring,lv_color_hex(ACCENT_MEDIA),0);
    lv_obj_set_style_border_width(ring,2,0);

    lv_obj_t *hole=lv_obj_create(ring); lv_obj_remove_style_all(hole);
    lv_obj_set_size(hole,40,40); lv_obj_center(hole);
    lv_obj_set_style_radius(hole,20,0);
    lv_obj_set_style_bg_color(hole,lv_color_hex(ACCENT_MEDIA),0);
    lv_obj_set_style_bg_opa(hole,LV_OPA_COVER,0);

    spl_arc=lv_arc_create(s);
    lv_obj_set_size(spl_arc,300,300); lv_obj_align(spl_arc,LV_ALIGN_CENTER,0,-30);
    lv_arc_set_rotation(spl_arc,270); lv_arc_set_bg_angles(spl_arc,0,360);
    lv_arc_set_value(spl_arc,0); lv_obj_remove_style(spl_arc,NULL,LV_PART_KNOB);
    lv_obj_set_style_arc_color(spl_arc,lv_color_hex(0x1C1C1C),LV_PART_MAIN);
    lv_obj_set_style_arc_color(spl_arc,lv_color_hex(ACCENT_MEDIA),LV_PART_INDICATOR);
    lv_obj_set_style_arc_width(spl_arc,4,LV_PART_MAIN);
    lv_obj_set_style_arc_width(spl_arc,4,LV_PART_INDICATOR);

    lv_obj_t *t=lv_label_create(s); lv_label_set_text(t,"MEDIA::OS");
    lv_obj_set_style_text_color(t,lv_color_hex(ACCENT_MEDIA),0);
    lv_obj_set_style_text_font(t,&lv_font_montserrat_28,0);
    lv_obj_set_style_text_letter_space(t,4,0);
    lv_obj_align(t,LV_ALIGN_BOTTOM_MID,0,-80);

    lv_obj_t *t2=lv_label_create(s); lv_label_set_text(t2,"SPOOLING UP");
    lv_obj_set_style_text_color(t2,lv_color_hex(0x888888),0);
    lv_obj_set_style_text_font(t2,&lv_font_montserrat_14,0);
    lv_obj_set_style_text_letter_space(t2,2,0);
    lv_obj_align(t2,LV_ALIGN_BOTTOM_MID,0,-50);
    lv_scr_load(s);

    const char *steps[]={"MOUNT SD","INDEX MUSIC","INDEX MANGA","SYNC RTC","READY"};
    for(int p=0;p<=100;p+=4){
        lv_arc_set_value(spl_arc,p);
        if(p%20==0) lv_label_set_text(t2,steps[p/20>4?4:p/20]);
        lv_obj_set_style_transform_angle(ring,p*36%3600,0);
        lv_timer_handler(); delay(28);
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
    lv_obj_add_event_cb(b,[](lv_event_t*){return_to_launcher();},LV_EVENT_CLICKED,NULL);
}

// =====================================================================
// Helpers: list SD dir into vector
// =====================================================================
static std::vector<String> list_files(const char *dir,const char *ext){
    std::vector<String> out;
    File d=SD_MMC.open(dir); if(!d||!d.isDirectory()) return out;
    File f=d.openNextFile();
    while(f){
        String n=f.name(); String lc=n; lc.toLowerCase();
        if(!f.isDirectory() && (ext==nullptr || lc.endsWith(ext))) out.push_back(n);
        f=d.openNextFile();
    }
    return out;
}
static std::vector<String> list_subdirs(const char *dir){
    std::vector<String> out;
    File d=SD_MMC.open(dir); if(!d||!d.isDirectory()) return out;
    File f=d.openNextFile();
    while(f){ if(f.isDirectory()) out.push_back(String(f.name())); f=d.openNextFile(); }
    return out;
}

// =====================================================================
// HOME / MODE PICKER
// =====================================================================
static void open_home();
static void open_music();
static void open_manga();
static void open_anime();
static void open_ambient();

static void tile(lv_obj_t *p, const char *t, uint32_t accent, int idx, void(*cb)()){
    lv_obj_t *b=lv_btn_create(p);
    lv_obj_set_size(b,330,46); lv_obj_align(b,LV_ALIGN_CENTER,0,-80+idx*52);
    lv_obj_set_style_bg_color(b,lv_color_hex(0x0E0E0E),0);
    lv_obj_set_style_border_color(b,lv_color_hex(accent),0);
    lv_obj_set_style_border_width(b,1,0); lv_obj_set_style_radius(b,14,0);
    lv_obj_t *l=lv_label_create(b); lv_label_set_text(l,t);
    lv_obj_set_style_text_color(l,lv_color_hex(0xFFFFFF),0);
    lv_obj_set_style_text_font(l,&lv_font_montserrat_18,0);
    lv_obj_set_style_text_letter_space(l,2,0);
    lv_obj_align(l,LV_ALIGN_LEFT_MID,20,0);
    lv_obj_t *dot=lv_obj_create(b); lv_obj_remove_style_all(dot);
    lv_obj_set_size(dot,6,6); lv_obj_align(dot,LV_ALIGN_RIGHT_MID,-22,0);
    lv_obj_set_style_radius(dot,3,0); lv_obj_set_style_bg_opa(dot,LV_OPA_COVER,0);
    lv_obj_set_style_bg_color(dot,lv_color_hex(accent),0);
    lv_obj_add_event_cb(b,[](lv_event_t *e){ void(*fn)()=(void(*)())lv_event_get_user_data(e); fn(); },LV_EVENT_CLICKED,(void*)cb);
}

static void open_home() { if (!g_apps_built) build_screen_apps(); goto_screen(SCREEN_APPS); }

// =====================================================================
// MUSIC
// =====================================================================
static String g_cur_track="";
static bool g_playing=false; static int g_pos=0, g_dur=210;
static lv_obj_t *m_track_lbl=nullptr,*m_pos_lbl=nullptr,*m_dur_lbl=nullptr,*m_bar=nullptr,*m_play_lbl=nullptr;
static lv_timer_t *m_tick=nullptr;
static void m_update(lv_timer_t*){
    if(g_playing && g_pos<g_dur) g_pos++;
    if(m_bar) lv_bar_set_value(m_bar,(g_pos*100)/g_dur,LV_ANIM_OFF);
    if(m_pos_lbl){char p[8]; snprintf(p,8,"%d:%02d",g_pos/60,g_pos%60); lv_label_set_text(m_pos_lbl,p);}
    if(m_dur_lbl){char p[8]; snprintf(p,8,"%d:%02d",g_dur/60,g_dur%60); lv_label_set_text(m_dur_lbl,p);}
    if(m_play_lbl) lv_label_set_text(m_play_lbl,g_playing?LV_SYMBOL_PAUSE:LV_SYMBOL_PLAY);
}
static void music_pick(lv_event_t *e){
    const char *name=(const char*)lv_event_get_user_data(e);
    g_cur_track=String(name); g_pos=0; g_playing=true;
    if(m_track_lbl) lv_label_set_text(m_track_lbl,g_cur_track.c_str());
}
static void open_music(){
    lv_obj_t *s=lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s,lv_color_hex(0x000000),0); lv_obj_set_style_bg_opa(s,LV_OPA_COVER,0);
    lv_obj_clear_flag(s,LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *h=lv_label_create(s); lv_label_set_text(h,"MUSIC");
    lv_obj_set_style_text_color(h,lv_color_hex(ACCENT_MEDIA),0);
    lv_obj_set_style_text_font(h,&lv_font_montserrat_22,0);
    lv_obj_set_style_text_letter_space(h,3,0);
    lv_obj_align(h,LV_ALIGN_TOP_MID,0,28);

    m_track_lbl=lv_label_create(s); lv_label_set_text(m_track_lbl,"(no track)");
    lv_obj_set_style_text_color(m_track_lbl,lv_color_hex(0xFFFFFF),0);
    lv_obj_set_style_text_font(m_track_lbl,&lv_font_montserrat_16,0);
    lv_obj_align(m_track_lbl,LV_ALIGN_TOP_MID,0,68);

    m_bar=lv_bar_create(s); lv_obj_set_size(m_bar,300,4);
    lv_obj_align(m_bar,LV_ALIGN_TOP_MID,0,100);
    lv_obj_set_style_bg_color(m_bar,lv_color_hex(0x1C1C1C),LV_PART_MAIN);
    lv_obj_set_style_bg_color(m_bar,lv_color_hex(ACCENT_MEDIA),LV_PART_INDICATOR);
    lv_obj_set_style_radius(m_bar,2,LV_PART_MAIN); lv_obj_set_style_radius(m_bar,2,LV_PART_INDICATOR);

    m_pos_lbl=lv_label_create(s); lv_label_set_text(m_pos_lbl,"0:00");
    lv_obj_set_style_text_color(m_pos_lbl,lv_color_hex(0x888888),0);
    lv_obj_set_style_text_font(m_pos_lbl,&lv_font_montserrat_12,0);
    lv_obj_align(m_pos_lbl,LV_ALIGN_TOP_LEFT,55,108);
    m_dur_lbl=lv_label_create(s); lv_label_set_text(m_dur_lbl,"3:30");
    lv_obj_set_style_text_color(m_dur_lbl,lv_color_hex(0x888888),0);
    lv_obj_set_style_text_font(m_dur_lbl,&lv_font_montserrat_12,0);
    lv_obj_align(m_dur_lbl,LV_ALIGN_TOP_RIGHT,-55,108);

    // play/pause
    lv_obj_t *play=lv_btn_create(s);
    lv_obj_set_size(play,68,68); lv_obj_align(play,LV_ALIGN_TOP_MID,0,135);
    lv_obj_set_style_bg_color(play,lv_color_hex(ACCENT_MEDIA),0);
    lv_obj_set_style_radius(play,34,0); lv_obj_set_style_border_width(play,0,0);
    m_play_lbl=lv_label_create(play); lv_label_set_text(m_play_lbl,LV_SYMBOL_PLAY);
    lv_obj_set_style_text_color(m_play_lbl,lv_color_hex(0x000000),0);
    lv_obj_set_style_text_font(m_play_lbl,&lv_font_montserrat_22,0); lv_obj_center(m_play_lbl);
    lv_obj_add_event_cb(play,[](lv_event_t*){ g_playing=!g_playing; },LV_EVENT_CLICKED,NULL);

    // List
    lv_obj_t *list=lv_list_create(s);
    lv_obj_set_size(list,360,200); lv_obj_align(list,LV_ALIGN_BOTTOM_MID,0,-90);
    lv_obj_set_style_bg_color(list,lv_color_hex(0x0E0E0E),0);
    lv_obj_set_style_border_color(list,lv_color_hex(0x1C1C1C),0);
    lv_obj_set_style_border_width(list,1,0); lv_obj_set_style_radius(list,12,0);
    auto tracks=list_files("/WATCH/media/music",".mp3");
    auto wavs  =list_files("/WATCH/media/music",".wav");
    for(auto &w:wavs) tracks.push_back(w);
    if(tracks.empty()){
        lv_obj_t *e=lv_label_create(list); lv_label_set_text(e,"Drop .mp3/.wav into /WATCH/media/music");
        lv_obj_set_style_text_color(e,lv_color_hex(0x666666),0);
        lv_obj_set_style_text_font(e,&lv_font_montserrat_12,0);
    } else {
        for(auto &t:tracks){
            char *cstr=strdup(t.c_str());
            lv_obj_t *btn=lv_list_add_btn(list,LV_SYMBOL_AUDIO,cstr);
            lv_obj_set_style_bg_color(btn,lv_color_hex(0x141414),0);
            lv_obj_set_style_text_color(btn,lv_color_hex(0xFFFFFF),0);
            lv_obj_add_event_cb(btn,music_pick,LV_EVENT_CLICKED,(void*)cstr);
        }
    }

    if(m_tick) lv_timer_del(m_tick);
    m_tick=lv_timer_create(m_update,1000,NULL);

    lv_obj_t *bk=lv_btn_create(s);
    lv_obj_set_size(bk,160,42); lv_obj_align(bk,LV_ALIGN_BOTTOM_MID,0,-20);
    lv_obj_set_style_bg_color(bk,lv_color_hex(0x141414),0);
    lv_obj_set_style_border_color(bk,lv_color_hex(0x00E5FF),0);
    lv_obj_set_style_border_width(bk,1,0); lv_obj_set_style_radius(bk,21,0);
    lv_obj_t *bl=lv_label_create(bk); lv_label_set_text(bl,LV_SYMBOL_LEFT "  Home");
    lv_obj_set_style_text_color(bl,lv_color_hex(0x00E5FF),0);
    lv_obj_set_style_text_font(bl,&lv_font_montserrat_14,0); lv_obj_center(bl);
    lv_obj_add_event_cb(bk,[](lv_event_t*){ if(m_tick){lv_timer_del(m_tick); m_tick=nullptr;} open_home(); },LV_EVENT_CLICKED,NULL);
    open_app_screen(s);
}

// =====================================================================
// MANGA (list titles -> stub reader)
// =====================================================================
static void open_manga(){
    lv_obj_t *s=lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s,lv_color_hex(0x000000),0); lv_obj_set_style_bg_opa(s,LV_OPA_COVER,0);
    lv_obj_clear_flag(s,LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *h=lv_label_create(s); lv_label_set_text(h,"MANGA");
    lv_obj_set_style_text_color(h,lv_color_hex(0x00E5FF),0);
    lv_obj_set_style_text_font(h,&lv_font_montserrat_22,0);
    lv_obj_set_style_text_letter_space(h,3,0);
    lv_obj_align(h,LV_ALIGN_TOP_MID,0,32);

    lv_obj_t *list=lv_list_create(s);
    lv_obj_set_size(list,360,360); lv_obj_align(list,LV_ALIGN_CENTER,0,-10);
    lv_obj_set_style_bg_color(list,lv_color_hex(0x0E0E0E),0);
    lv_obj_set_style_border_color(list,lv_color_hex(0x1C1C1C),0);
    lv_obj_set_style_border_width(list,1,0); lv_obj_set_style_radius(list,12,0);
    auto titles=list_subdirs("/WATCH/media/manga");
    if(titles.empty()){
        lv_obj_t *e=lv_label_create(list); lv_label_set_text(e,"Create /WATCH/media/manga/<title>/\n  page_001.raw (RGB565 410x502)");
        lv_obj_set_style_text_color(e,lv_color_hex(0x666666),0);
        lv_obj_set_style_text_font(e,&lv_font_montserrat_12,0);
    } else {
        for(auto &t:titles){
            lv_obj_t *btn=lv_list_add_btn(list,LV_SYMBOL_FILE,strdup(t.c_str()));
            lv_obj_set_style_bg_color(btn,lv_color_hex(0x141414),0);
            lv_obj_set_style_text_color(btn,lv_color_hex(0xFFFFFF),0);
        }
    }

    lv_obj_t *bk=lv_btn_create(s);
    lv_obj_set_size(bk,160,42); lv_obj_align(bk,LV_ALIGN_BOTTOM_MID,0,-20);
    lv_obj_set_style_bg_color(bk,lv_color_hex(0x141414),0);
    lv_obj_set_style_border_color(bk,lv_color_hex(0x00E5FF),0);
    lv_obj_set_style_border_width(bk,1,0); lv_obj_set_style_radius(bk,21,0);
    lv_obj_t *bl=lv_label_create(bk); lv_label_set_text(bl,LV_SYMBOL_LEFT "  Home");
    lv_obj_set_style_text_color(bl,lv_color_hex(0x00E5FF),0);
    lv_obj_set_style_text_font(bl,&lv_font_montserrat_14,0); lv_obj_center(bl);
    lv_obj_add_event_cb(bk,[](lv_event_t*){ open_home(); },LV_EVENT_CLICKED,NULL);
    open_app_screen(s);
}

// =====================================================================
// ANIME tracker
// =====================================================================
static void open_anime(){
    lv_obj_t *s=lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s,lv_color_hex(0x000000),0); lv_obj_set_style_bg_opa(s,LV_OPA_COVER,0);
    lv_obj_clear_flag(s,LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *h=lv_label_create(s); lv_label_set_text(h,"ANIME TRACKER");
    lv_obj_set_style_text_color(h,lv_color_hex(0xFF2EC4),0);
    lv_obj_set_style_text_font(h,&lv_font_montserrat_22,0);
    lv_obj_set_style_text_letter_space(h,3,0);
    lv_obj_align(h,LV_ALIGN_TOP_MID,0,32);

    lv_obj_t *list=lv_list_create(s);
    lv_obj_set_size(list,360,360); lv_obj_align(list,LV_ALIGN_CENTER,0,-10);
    lv_obj_set_style_bg_color(list,lv_color_hex(0x0E0E0E),0);
    lv_obj_set_style_border_color(list,lv_color_hex(0x1C1C1C),0);
    lv_obj_set_style_border_width(list,1,0); lv_obj_set_style_radius(list,12,0);

    File f=SD_MMC.open("/WATCH/media/anime/watchlist.txt",FILE_READ);
    if(!f){
        // seed one
        File w=SD_MMC.open("/WATCH/media/anime/watchlist.txt",FILE_WRITE);
        if(w){ w.println("Solo Leveling|S1|E12|WATCHING"); w.println("Frieren|S1|E08|WATCHING"); w.close(); }
        f=SD_MMC.open("/WATCH/media/anime/watchlist.txt",FILE_READ);
    }
    if(f){
        while(f.available()){
            String ln=f.readStringUntil('\n'); ln.trim();
            if(ln.length()==0) continue;
            lv_obj_t *btn=lv_list_add_btn(list,LV_SYMBOL_VIDEO,strdup(ln.c_str()));
            lv_obj_set_style_bg_color(btn,lv_color_hex(0x141414),0);
            lv_obj_set_style_text_color(btn,lv_color_hex(0xFFFFFF),0);
        }
        f.close();
    }

    lv_obj_t *bk=lv_btn_create(s);
    lv_obj_set_size(bk,160,42); lv_obj_align(bk,LV_ALIGN_BOTTOM_MID,0,-20);
    lv_obj_set_style_bg_color(bk,lv_color_hex(0x141414),0);
    lv_obj_set_style_border_color(bk,lv_color_hex(0x00E5FF),0);
    lv_obj_set_style_border_width(bk,1,0); lv_obj_set_style_radius(bk,21,0);
    lv_obj_t *bl=lv_label_create(bk); lv_label_set_text(bl,LV_SYMBOL_LEFT "  Home");
    lv_obj_set_style_text_color(bl,lv_color_hex(0x00E5FF),0);
    lv_obj_set_style_text_font(bl,&lv_font_montserrat_14,0); lv_obj_center(bl);
    lv_obj_add_event_cb(bk,[](lv_event_t*){ open_home(); },LV_EVENT_CLICKED,NULL);
    open_app_screen(s);
}

// =====================================================================
// AMBIENT visualizer
// =====================================================================
static lv_obj_t *amb_canvas=nullptr; static lv_color_t *amb_buf=nullptr;
static lv_timer_t *amb_t=nullptr; static float amb_phase=0;
static void amb_step(lv_timer_t*){
    if(!amb_canvas) return;
    lv_canvas_fill_bg(amb_canvas,lv_color_hex(0x000000),LV_OPA_COVER);
    lv_draw_rect_dsc_t r; lv_draw_rect_dsc_init(&r);
    r.bg_opa=LV_OPA_COVER; r.radius=2;
    amb_phase+=0.18;
    for(int x=0;x<360;x+=6){
        float y = 200 + sinf((x*0.04f)+amb_phase)*80 + cosf((x*0.02f)-amb_phase*0.7f)*40;
        int h=(int)y; if(h<10) h=10; if(h>390) h=390;
        uint32_t col = x<120?ACCENT_MEDIA : x<240?0x00E5FF : 0xFF2EC4;
        r.bg_color=lv_color_hex(col);
        lv_canvas_draw_rect(amb_canvas,x,200-h/2,4,h,&r);
    }
}
static void open_ambient(){
    lv_obj_t *s=lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s,lv_color_hex(0x000000),0); lv_obj_set_style_bg_opa(s,LV_OPA_COVER,0);
    lv_obj_clear_flag(s,LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *h=lv_label_create(s); lv_label_set_text(h,"AMBIENT");
    lv_obj_set_style_text_color(h,lv_color_hex(0x00FF80),0);
    lv_obj_set_style_text_font(h,&lv_font_montserrat_22,0);
    lv_obj_set_style_text_letter_space(h,4,0);
    lv_obj_align(h,LV_ALIGN_TOP_MID,0,32);

    if(amb_buf) free(amb_buf);
    amb_buf=(lv_color_t*)heap_caps_malloc(360*400*sizeof(lv_color_t),MALLOC_CAP_8BIT);
    amb_canvas=lv_canvas_create(s);
    lv_canvas_set_buffer(amb_canvas,amb_buf,360,400,LV_IMG_CF_TRUE_COLOR);
    lv_obj_align(amb_canvas,LV_ALIGN_CENTER,0,-10);
    if(amb_t) lv_timer_del(amb_t);
    amb_t=lv_timer_create(amb_step,40,NULL);

    lv_obj_t *bk=lv_btn_create(s);
    lv_obj_set_size(bk,160,42); lv_obj_align(bk,LV_ALIGN_BOTTOM_MID,0,-20);
    lv_obj_set_style_bg_color(bk,lv_color_hex(0x141414),0);
    lv_obj_set_style_border_color(bk,lv_color_hex(0x00E5FF),0);
    lv_obj_set_style_border_width(bk,1,0); lv_obj_set_style_radius(bk,21,0);
    lv_obj_t *bl=lv_label_create(bk); lv_label_set_text(bl,LV_SYMBOL_LEFT "  Home");
    lv_obj_set_style_text_color(bl,lv_color_hex(0x00E5FF),0);
    lv_obj_set_style_text_font(bl,&lv_font_montserrat_14,0); lv_obj_center(bl);
    lv_obj_add_event_cb(bk,[](lv_event_t*){ if(amb_t){lv_timer_del(amb_t); amb_t=nullptr;} if(amb_buf){free(amb_buf); amb_buf=nullptr;} open_home();},LV_EVENT_CLICKED,NULL);
    open_app_screen(s);
}

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
    lv_label_set_text(title, "MEDIA//OS");
    lv_obj_set_style_text_color(title, lv_color_hex(0xFF2EC4), 0);
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
        lv_label_set_text(dt, "M");
        lv_obj_set_style_text_color(dt, lv_color_hex(0x00E5FF), 0);
        lv_obj_set_style_text_font(dt, &lv_font_montserrat_22, 0);
        lv_obj_center(dt);

        lv_obj_t *name = lv_label_create(card);
        lv_label_set_text(name, "MUSIC");
        lv_obj_set_style_text_color(name, lv_color_hex(0xFFFFFF), 0);
        lv_obj_set_style_text_font(name, &lv_font_montserrat_18, 0);
        lv_obj_set_style_text_letter_space(name, 2, 0);
        lv_obj_align(name, LV_ALIGN_LEFT_MID, 64, -10);

        lv_obj_t *desc = lv_label_create(card);
        lv_label_set_text(desc, "browse + play tracks");
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
            open_music();
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
        lv_label_set_text(dt, "M");
        lv_obj_set_style_text_color(dt, lv_color_hex(0xFF2EC4), 0);
        lv_obj_set_style_text_font(dt, &lv_font_montserrat_22, 0);
        lv_obj_center(dt);

        lv_obj_t *name = lv_label_create(card);
        lv_label_set_text(name, "MANGA");
        lv_obj_set_style_text_color(name, lv_color_hex(0xFFFFFF), 0);
        lv_obj_set_style_text_font(name, &lv_font_montserrat_18, 0);
        lv_obj_set_style_text_letter_space(name, 2, 0);
        lv_obj_align(name, LV_ALIGN_LEFT_MID, 64, -10);

        lv_obj_t *desc = lv_label_create(card);
        lv_label_set_text(desc, "read offline");
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
            open_manga();
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
        lv_label_set_text(dt, "A");
        lv_obj_set_style_text_color(dt, lv_color_hex(0xFFB300), 0);
        lv_obj_set_style_text_font(dt, &lv_font_montserrat_22, 0);
        lv_obj_center(dt);

        lv_obj_t *name = lv_label_create(card);
        lv_label_set_text(name, "ANIME");
        lv_obj_set_style_text_color(name, lv_color_hex(0xFFFFFF), 0);
        lv_obj_set_style_text_font(name, &lv_font_montserrat_18, 0);
        lv_obj_set_style_text_letter_space(name, 2, 0);
        lv_obj_align(name, LV_ALIGN_LEFT_MID, 64, -10);

        lv_obj_t *desc = lv_label_create(card);
        lv_label_set_text(desc, "episode queue");
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
            open_anime();
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
        lv_label_set_text(dt, "A");
        lv_obj_set_style_text_color(dt, lv_color_hex(0x00FF80), 0);
        lv_obj_set_style_text_font(dt, &lv_font_montserrat_22, 0);
        lv_obj_center(dt);

        lv_obj_t *name = lv_label_create(card);
        lv_label_set_text(name, "AMBIENT");
        lv_obj_set_style_text_color(name, lv_color_hex(0xFFFFFF), 0);
        lv_obj_set_style_text_font(name, &lv_font_montserrat_18, 0);
        lv_obj_set_style_text_letter_space(name, 2, 0);
        lv_obj_align(name, LV_ALIGN_LEFT_MID, 64, -10);

        lv_obj_t *desc = lv_label_create(card);
        lv_label_set_text(desc, "calm visuals");
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
            open_ambient();
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
