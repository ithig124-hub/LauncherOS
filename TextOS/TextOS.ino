
// =====================================================================
// ESPNOW-OS.ino  --  LauncherOS-compatible mini-OS module
// Target  : Waveshare ESP32-S3 Touch AMOLED 2.06 (410 x 502, FT3168, AXP2101)
// Build   : ESP32-S3 Dev Module, PSRAM=OPI, Partition=Minimal SPIFFS (large APP)
// Output  : ESPNowOS.ino.bin -> /WATCH/bin/espnowos.bin
// =====================================================================
#include <Arduino.h>
#include <WiFi.h>
#include <SD_MMC.h>
#include <Preferences.h>
#include <Update.h>
#include <esp_ota_ops.h>
#include <esp_partition.h>
#include <esp_sleep.h>
#include <esp_wifi.h>
#include <esp_now.h>
#include <math.h>

// ---- ESP-NOW callback signature compatibility shim -----------------
// Core 3.x (ESP-IDF 5.x): on_recv(const esp_now_recv_info_t*, const uint8_t*, int)
// Core 2.x (ESP-IDF 4.x): on_recv(const uint8_t* mac,          const uint8_t*, int)
#ifndef ESP_ARDUINO_VERSION_MAJOR
  #include "esp_arduino_version.h"
#endif
#if ESP_ARDUINO_VERSION_MAJOR >= 3
  #define ESPNOW_RECV_SIG  const esp_now_recv_info_t *info, const uint8_t *data, int len
  #define ESPNOW_RECV_MAC  (info->src_addr)
#else
  #define ESPNOW_RECV_SIG  const uint8_t *mac, const uint8_t *data, int len
  #define ESPNOW_RECV_MAC  (mac)
#endif

#include "Arduino_GFX_Library.h"
#include "Arduino_DriveBus_Library.h"
#include "lv_conf.h"
#include "lvgl.h"

#define XPOWERS_CHIP_AXP2101
#include "XPowersLib.h"

// ---- Pin map (matches LauncherOS) ----------------------------------
#define LCD_SDIO0 4
#define LCD_SDIO1 5
#define LCD_SDIO2 6
#define LCD_SDIO3 7
#define LCD_SCLK 11
#define LCD_CS   12
#define LCD_RESET 8
#define LCD_W 410
#define LCD_H 502
#define IIC_SDA  15
#define IIC_SCL  14
#define TP_INT   38
#define TP_RESET 9
static const int SDMMC_CLK=2, SDMMC_CMD=1, SDMMC_DATA=3;
#define BOOT_BTN 0

// ---- Palette (per-OS accent overridden below) ----------------------
#define COL_BG       0x000000
#define COL_PANEL    0x0E0E0E
#define COL_PANEL2   0x1C1C1C
#define COL_WHITE    0xFFFFFF
#define COL_TEXT_DIM 0x666666
#define COL_RED      0xFF4040
#define COL_GREEN    0x00FF80
#define COL_AMBER    0xFFB300
#define COL_CYAN     0x00E5FF
#define COL_MAGENTA  0xFF2EC4

// ---- App-specific accents ------------------------------------------
#define ACCENT_A  0x00FFD0
#define ACCENT_B  0x00B5FF

// ---- Singletons ----------------------------------------------------
Arduino_DataBus *g_bus = new Arduino_ESP32QSPI(LCD_CS,LCD_SCLK,LCD_SDIO0,LCD_SDIO1,LCD_SDIO2,LCD_SDIO3);
Arduino_GFX     *gfx   = new Arduino_CO5300(g_bus,LCD_RESET,0,LCD_W,LCD_H,22,0,0,0);
std::shared_ptr<Arduino_IIC_DriveBus> g_iic = std::make_shared<Arduino_HWIIC>(IIC_SDA,IIC_SCL,&Wire);
void Arduino_IIC_Touch_Interrupt(void);
std::unique_ptr<Arduino_IIC> FT3168(new Arduino_FT3x68(g_iic,FT3168_DEVICE_ADDRESS,DRIVEBUS_DEFAULT_VALUE,TP_INT,Arduino_IIC_Touch_Interrupt));
void Arduino_IIC_Touch_Interrupt(void){FT3168->IIC_Interrupt_Flag=true;}
XPowersPMU g_pmu; bool g_has_pmu=false;

// ---- LVGL plumbing -------------------------------------------------
static lv_disp_draw_buf_t g_db;  static lv_color_t *g_lvbuf=nullptr;
static lv_disp_drv_t      g_dd;  static lv_indev_drv_t g_id;
static void lv_flush(lv_disp_drv_t*d,const lv_area_t*a,lv_color_t*p){
  uint32_t w=a->x2-a->x1+1, h=a->y2-a->y1+1;
  gfx->draw16bitRGBBitmap(a->x1,a->y1,(uint16_t*)p,w,h); lv_disp_flush_ready(d);
}
static void lv_touch_read(lv_indev_drv_t*, lv_indev_data_t *d){
  int32_t x = FT3168->IIC_Read_Device_Value(FT3168->Arduino_IIC_Touch::Value_Information::TOUCH_COORDINATE_X);
  int32_t y = FT3168->IIC_Read_Device_Value(FT3168->Arduino_IIC_Touch::Value_Information::TOUCH_COORDINATE_Y);
  int     f = FT3168->IIC_Read_Device_Value(FT3168->Arduino_IIC_Touch::Value_Information::TOUCH_FINGER_NUMBER);
  bool valid = (x>=0 && x<LCD_W && y>=0 && y<LCD_H && f>=0 && f<=5);
  if(!valid){ d->state=LV_INDEV_STATE_REL; return; }
  if(f>0){ d->state=LV_INDEV_STATE_PR; d->point.x=x; d->point.y=y; }
  else d->state=LV_INDEV_STATE_REL;
}

// ---- Back-to-launcher ----------------------------------------------
static void return_to_launcher(){
  const esp_partition_t *factory = esp_partition_find_first(
      ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_FACTORY, NULL);
  if(factory) esp_ota_set_boot_partition(factory);
  else {
    const esp_partition_t *other = esp_ota_get_next_update_partition(NULL);
    if(other && other != esp_ota_get_running_partition())
      esp_ota_set_boot_partition(other);
  }
  delay(80); esp_restart();
}

// ---- WiFi from /WATCH/wifi/config.txt ------------------------------
struct WifiCred{ String ssid,pass; };
static WifiCred g_creds[6]; static int g_creds_n=0; static int g_gmt=0;
static bool g_wifi_synced=false;
static void load_wifi_config(){
  g_creds_n=0;
  File f=SD_MMC.open("/WATCH/wifi/config.txt",FILE_READ);
  if(!f) return;
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
}
static bool wifi_quick_connect(uint32_t timeout_ms){
  if(g_creds_n==0) return false;
  WiFi.mode(WIFI_STA);
  for(int i=0;i<g_creds_n;i++){
    WiFi.begin(g_creds[i].ssid.c_str(), g_creds[i].pass.c_str());
    uint32_t t0=millis();
    while(WiFi.status()!=WL_CONNECTED && millis()-t0<timeout_ms){ delay(150); }
    if(WiFi.status()==WL_CONNECTED){ g_wifi_synced=true; return true; }
  }
  return false;
}
static void wifi_kill(){ WiFi.disconnect(true,true); WiFi.mode(WIFI_OFF); }

// ---- Wallpaper -----------------------------------------------------
static uint8_t *g_wp_buf=nullptr; static lv_img_dsc_t g_wp_dsc;
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
  lv_obj_align(img,LV_ALIGN_CENTER,0,0); lv_obj_move_background(img);
  lv_obj_set_style_opa(img, 90, 0);
}

// ---- HW boot --------------------------------------------------------
static void hw_boot(){
  pinMode(BOOT_BTN, INPUT_PULLUP);
  if(!gfx->begin()) Serial.println("gfx begin fail");
  gfx->fillScreen(0x0000);
  pinMode(IIC_SCL, OUTPUT);
  for(int i=0;i<9;i++){ digitalWrite(IIC_SCL,LOW); delayMicroseconds(5); digitalWrite(IIC_SCL,HIGH); delayMicroseconds(5); }
  Wire.begin(IIC_SDA, IIC_SCL); Wire.setClock(400000);
  if(g_pmu.begin(Wire, AXP2101_SLAVE_ADDRESS, IIC_SDA, IIC_SCL)){
    g_has_pmu=true;
    g_pmu.disableTSPinMeasure(); g_pmu.enableBattVoltageMeasure();
    g_pmu.setALDO1Voltage(3300); g_pmu.enableALDO1();
    g_pmu.setBLDO1Voltage(3300); g_pmu.enableBLDO1();
    delay(40);
  }
  pinMode(TP_RESET, OUTPUT);
  digitalWrite(TP_RESET, LOW);  delay(50);
  digitalWrite(TP_RESET, HIGH); delay(200);
  FT3168->begin();
  lv_init();
  size_t px = LCD_W * 60;
  g_lvbuf = (lv_color_t*)heap_caps_malloc(px*sizeof(lv_color_t), MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT);
  if(!g_lvbuf) g_lvbuf = (lv_color_t*)heap_caps_malloc(px*sizeof(lv_color_t), MALLOC_CAP_8BIT);
  lv_disp_draw_buf_init(&g_db, g_lvbuf, NULL, px);
  lv_disp_drv_init(&g_dd);
  g_dd.flush_cb=lv_flush; g_dd.draw_buf=&g_db; g_dd.hor_res=LCD_W; g_dd.ver_res=LCD_H;
  lv_disp_drv_register(&g_dd);
  lv_indev_drv_init(&g_id);
  g_id.type=LV_INDEV_TYPE_POINTER; g_id.read_cb=lv_touch_read;
  lv_indev_drv_register(&g_id);
  SD_MMC.setPins(SDMMC_CLK, SDMMC_CMD, SDMMC_DATA);
  SD_MMC.begin("/sdcard", true);
  load_wifi_config();
}

// ---- Themed splash --------------------------------------------------
static void splash_show(){
  lv_obj_t *s=lv_obj_create(NULL);
  lv_obj_set_style_bg_color(s,lv_color_hex(COL_BG),0);
  lv_obj_set_style_bg_opa(s,LV_OPA_COVER,0);
  lv_obj_clear_flag(s,LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_t *l=lv_label_create(s); lv_label_set_text(l,"ESPNOW//OS");
  lv_obj_set_style_text_color(l,lv_color_hex(ACCENT_A),0);
  lv_obj_set_style_text_font(l,&lv_font_montserrat_48,0);
  lv_obj_align(l,LV_ALIGN_CENTER,0,-30);
  lv_obj_t *l2=lv_label_create(s); lv_label_set_text(l2,"DISCOVERING PEERS");
  lv_obj_set_style_text_color(l2,lv_color_hex(ACCENT_B),0);
  lv_obj_set_style_text_font(l2,&lv_font_montserrat_14,0);
  lv_obj_set_style_text_letter_space(l2,3,0);
  lv_obj_align(l2,LV_ALIGN_CENTER,0,20);

  lv_obj_t *bar=lv_bar_create(s); lv_obj_set_size(bar,260,4);
  lv_obj_align(bar,LV_ALIGN_CENTER,0,80);
  lv_obj_set_style_bg_color(bar,lv_color_hex(COL_PANEL2),LV_PART_MAIN);
  lv_obj_set_style_bg_color(bar,lv_color_hex(ACCENT_A),LV_PART_INDICATOR);
  lv_bar_set_range(bar,0,100); lv_bar_set_value(bar,0,LV_ANIM_OFF);
  lv_obj_t *step=lv_label_create(s); lv_label_set_text(step,"BOOTING");
  lv_obj_set_style_text_color(step,lv_color_hex(COL_TEXT_DIM),0);
  lv_obj_set_style_text_font(step,&lv_font_montserrat_12,0);
  lv_obj_set_style_text_letter_space(step,3,0);
  lv_obj_align(step,LV_ALIGN_CENTER,0,110);
  lv_scr_load(s);
  const char *steps[]={"INIT HW","MOUNT SD","LOAD CFG","WIFI SYNC","READY"};
  uint32_t held=0;
  for(int p=0;p<=100;p+=4){
    lv_bar_set_value(bar,p,LV_ANIM_OFF);
    lv_label_set_text(step, steps[p/25>4?4:p/25]);
    lv_timer_handler(); delay(28);
    if(p==40){ wifi_quick_connect(2500); }
    if(p==60){ wifi_kill(); }
    if(digitalRead(BOOT_BTN)==LOW){
      if(!held) held=millis();
      else if(millis()-held>1500) return_to_launcher();
    } else held=0;
  }
}

// ---- Back-to-launcher (BOOT button only — no on-screen pill) -------

// ---- Battery pill (top-right of home) ------------------------------
static lv_obj_t *g_batt_lbl=nullptr;
static void update_batt_pill(lv_timer_t*){
  if(!g_has_pmu||!g_batt_lbl) return;
  int pct=g_pmu.getBatteryPercent(); if(pct<0) pct=0; if(pct>100) pct=100;
  char b[24]; snprintf(b,24,"%d%%%s", pct, g_pmu.isCharging()?" "LV_SYMBOL_CHARGE:"");
  lv_label_set_text(g_batt_lbl,b);
}
static void add_header_strip(lv_obj_t *scr, const char *title, uint32_t accent){
  lv_obj_t *t=lv_label_create(scr); lv_label_set_text(t,title);
  lv_obj_set_style_text_color(t,lv_color_hex(accent),0);
  lv_obj_set_style_text_font(t,&lv_font_montserrat_22,0);
  lv_obj_set_style_text_letter_space(t,4,0);
  lv_obj_align(t,LV_ALIGN_TOP_LEFT,24,22);
  g_batt_lbl=lv_label_create(scr); lv_label_set_text(g_batt_lbl,"--%");
  lv_obj_set_style_text_color(g_batt_lbl,lv_color_hex(COL_TEXT_DIM),0);
  lv_obj_set_style_text_font(g_batt_lbl,&lv_font_montserrat_14,0);
  lv_obj_align(g_batt_lbl,LV_ALIGN_TOP_RIGHT,-24,28);
  lv_timer_create(update_batt_pill,2000,NULL); update_batt_pill(nullptr);
  lv_obj_t *dot=lv_obj_create(scr); lv_obj_remove_style_all(dot);
  lv_obj_set_size(dot,8,8); lv_obj_align(dot,LV_ALIGN_TOP_RIGHT,-12,32);
  lv_obj_set_style_radius(dot,4,0);
  lv_obj_set_style_bg_color(dot, lv_color_hex(g_wifi_synced?COL_GREEN:COL_RED),0);
  lv_obj_set_style_bg_opa(dot, LV_OPA_COVER,0);
}

// ---- Boot button safety exit (call from loop) — ONLY way to exit --
// Hold BOOT (GPIO 0) for 1.5 s anywhere in the app to return to LauncherOS.
// A subtle persistent hint is rendered top-center on every home screen.
static void add_boot_hint(lv_obj_t *scr){
  lv_obj_t *h=lv_label_create(scr);
  lv_label_set_text(h, "HOLD BOOT 1.5s  " LV_SYMBOL_LEFT "  LAUNCHEROS");
  lv_obj_set_style_text_color(h, lv_color_hex(0x444444),0);
  lv_obj_set_style_text_font(h, &lv_font_montserrat_12,0);
  lv_obj_set_style_text_letter_space(h, 2, 0);
  lv_obj_align(h, LV_ALIGN_BOTTOM_MID, 0, -16);
}
static void poll_boot_exit(){
  static uint32_t held=0;
  if(digitalRead(BOOT_BTN)==LOW){
    if(!held) held=millis();
    else if(millis()-held>1500) return_to_launcher();
  } else held=0;
}


// ====================================================================
// ESPNOW-OS  --  peer-to-peer chat between two watches via ESP-NOW
// Big-key keyboard + ability to send a "play raw" command to the peer.
// ====================================================================
// (esp_now.h / esp_wifi.h are already included at the top of the file)

static String   g_compose = "";
static String   g_log[8];
static int      g_log_n = 0;
static uint8_t  g_peer[6] = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF}; // broadcast
static lv_obj_t *g_log_lbl = nullptr;
static lv_obj_t *g_compose_lbl = nullptr;

static void log_push(const char *prefix, const char *txt){
  if(g_log_n>=8){ for(int i=0;i<7;i++) g_log[i]=g_log[i+1]; g_log_n=7; }
  g_log[g_log_n++] = String(prefix) + txt;
  String all=""; for(int i=0;i<g_log_n;i++) all += g_log[i] + "\n";
  if(g_log_lbl) lv_label_set_text(g_log_lbl, all.c_str());
}
static void on_recv(ESPNOW_RECV_SIG){
  (void)ESPNOW_RECV_MAC;  // mac available if you need it
  char buf[200]; int n=len>199?199:len; memcpy(buf,data,n); buf[n]=0;
  log_push("< ", buf);
}
static void send_compose(){
  if(g_compose.length()==0) return;
  esp_now_send(g_peer, (uint8_t*)g_compose.c_str(), g_compose.length());
  log_push("> ", g_compose.c_str());
  g_compose=""; if(g_compose_lbl) lv_label_set_text(g_compose_lbl, " ");
}
static void send_raw_play(){
  const char *cmd="!PLAY:beep.raw";
  esp_now_send(g_peer, (uint8_t*)cmd, strlen(cmd));
  log_push("> ", cmd);
}

static const char *KB_ROWS[3] = {
  "QWERTYUIOP",
  "ASDFGHJKL",
  "ZXCVBNM_."
};

static void mk_key(lv_obj_t *par, const char c, int x, int y, int w, int h){
  lv_obj_t *b=lv_btn_create(par);
  lv_obj_set_size(b,w,h);
  lv_obj_set_pos(b,x,y);
  lv_obj_set_style_bg_color(b, lv_color_hex(0x101820),0);
  lv_obj_set_style_border_color(b, lv_color_hex(ACCENT_A),0);
  lv_obj_set_style_border_width(b,1,0);
  lv_obj_set_style_radius(b,8,0);
  lv_obj_set_style_bg_color(b, lv_color_hex(0x062028), LV_STATE_PRESSED);
  lv_obj_t *l=lv_label_create(b);
  char buf[2]={c,0}; lv_label_set_text(l, c==' '?"SPC": (c=='_'?"SPC":buf));
  lv_obj_set_style_text_color(l, lv_color_hex(ACCENT_A),0);
  lv_obj_set_style_text_font(l, &lv_font_montserrat_22,0);
  lv_obj_center(l);
  static char store[64]; static int idx=0; if(idx<63){ store[idx]=c; }
  char *slot = &store[idx]; idx++;
  lv_obj_add_event_cb(b, [](lv_event_t *e){
    char ch = *(char*)lv_event_get_user_data(e);
    if(ch=='_') ch=' ';
    g_compose += ch;
    if(g_compose_lbl) lv_label_set_text(g_compose_lbl, g_compose.c_str());
  }, LV_EVENT_CLICKED, slot);
}

static void build_home(){
  lv_obj_t *s=lv_obj_create(NULL);
  lv_obj_set_style_bg_color(s,lv_color_hex(COL_BG),0);
  lv_obj_set_style_bg_opa(s,LV_OPA_COVER,0);
  lv_obj_clear_flag(s,LV_OBJ_FLAG_SCROLLABLE);
  add_header_strip(s, "ESPNOW", ACCENT_A);
  g_log_lbl = lv_label_create(s);
  lv_label_set_long_mode(g_log_lbl, LV_LABEL_LONG_WRAP);
  lv_obj_set_width(g_log_lbl, LCD_W-40);
  lv_obj_align(g_log_lbl, LV_ALIGN_TOP_LEFT, 20, 60);
  lv_obj_set_style_text_color(g_log_lbl, lv_color_hex(ACCENT_B),0);
  lv_obj_set_style_text_font(g_log_lbl, &lv_font_montserrat_14,0);
  lv_label_set_text(g_log_lbl, "(no messages yet)\nbroadcast peer FF:FF:FF:FF:FF:FF");

  g_compose_lbl = lv_label_create(s);
  lv_obj_set_width(g_compose_lbl, LCD_W-60);
  lv_label_set_long_mode(g_compose_lbl, LV_LABEL_LONG_WRAP);
  lv_obj_align(g_compose_lbl, LV_ALIGN_TOP_LEFT, 20, 175);
  lv_obj_set_style_text_color(g_compose_lbl, lv_color_hex(COL_WHITE),0);
  lv_obj_set_style_text_font(g_compose_lbl, &lv_font_montserrat_22,0);
  lv_label_set_text(g_compose_lbl, " ");

  lv_obj_t *kbBg=lv_obj_create(s); lv_obj_remove_style_all(kbBg);
  lv_obj_set_size(kbBg, LCD_W-40, 200);
  lv_obj_align(kbBg, LV_ALIGN_BOTTOM_MID, 0, -100);
  lv_obj_set_style_bg_color(kbBg, lv_color_hex(0x050505),0);
  lv_obj_set_style_bg_opa(kbBg, LV_OPA_COVER,0);
  lv_obj_set_style_radius(kbBg,12,0);
  lv_obj_clear_flag(kbBg, LV_OBJ_FLAG_SCROLLABLE);

  int kw=33, kh=54, gap=3;
  for(int r=0;r<3;r++){
    int len=strlen(KB_ROWS[r]);
    int row_w = len*kw + (len-1)*gap;
    int xoff = (LCD_W-40 - row_w)/2;
    int yoff = r*(kh+gap)+8;
    for(int i=0;i<len;i++) mk_key(kbBg, KB_ROWS[r][i], xoff+i*(kw+gap), yoff, kw, kh);
  }

  lv_obj_t *send=lv_btn_create(s); lv_obj_set_size(send,140,46);
  lv_obj_align(send, LV_ALIGN_BOTTOM_LEFT, 20, -38);
  lv_obj_set_style_bg_color(send, lv_color_hex(ACCENT_A),0);
  lv_obj_set_style_radius(send,23,0);
  lv_obj_t *sl=lv_label_create(send); lv_label_set_text(sl, "SEND " LV_SYMBOL_RIGHT);
  lv_obj_set_style_text_color(sl, lv_color_hex(0x001018),0);
  lv_obj_set_style_text_font(sl, &lv_font_montserrat_18,0); lv_obj_center(sl);
  lv_obj_add_event_cb(send, [](lv_event_t*){ send_compose(); }, LV_EVENT_CLICKED, NULL);

  lv_obj_t *play=lv_btn_create(s); lv_obj_set_size(play,140,46);
  lv_obj_align(play, LV_ALIGN_BOTTOM_RIGHT, -20, -38);
  lv_obj_set_style_bg_color(play, lv_color_hex(0x101820),0);
  lv_obj_set_style_border_color(play, lv_color_hex(ACCENT_B),0);
  lv_obj_set_style_border_width(play,1,0);
  lv_obj_set_style_radius(play,23,0);
  lv_obj_t *pl=lv_label_create(play); lv_label_set_text(pl, LV_SYMBOL_PLAY " RAW");
  lv_obj_set_style_text_color(pl, lv_color_hex(ACCENT_B),0);
  lv_obj_set_style_text_font(pl, &lv_font_montserrat_18,0); lv_obj_center(pl);
  lv_obj_add_event_cb(play, [](lv_event_t*){ send_raw_play(); }, LV_EVENT_CLICKED, NULL);

  add_boot_hint(s);
  lv_scr_load_anim(s, LV_SCR_LOAD_ANIM_FADE_ON, 260,0,false);
}

static void start_espnow(){
  WiFi.mode(WIFI_STA); WiFi.disconnect();
  esp_wifi_set_channel(1, WIFI_SECOND_CHAN_NONE);
  if(esp_now_init()!=ESP_OK) return;
  esp_now_register_recv_cb(on_recv);
  esp_now_peer_info_t pi={}; memcpy(pi.peer_addr, g_peer, 6); pi.channel=1; pi.encrypt=false;
  esp_now_add_peer(&pi);
}

void setup(){ Serial.begin(115200); hw_boot(); splash_show(); start_espnow(); build_home(); }
void loop(){ lv_timer_handler(); poll_boot_exit(); delay(5); }
