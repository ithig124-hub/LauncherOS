
// =====================================================================
// AMBIENT-OS.ino  --  LauncherOS-compatible mini-OS module
// Target  : Waveshare ESP32-S3 Touch AMOLED 2.06 (410 x 502, FT3168, AXP2101)
// Build   : ESP32-S3 Dev Module, PSRAM=OPI, Partition=Minimal SPIFFS (large APP)
// Output  : AmbientOS.ino.bin -> /WATCH/bin/ambientos.bin
// =====================================================================
#include <Arduino.h>
#include <WiFi.h>
#include <SD_MMC.h>
#include <Preferences.h>
#include <Update.h>
#include <esp_ota_ops.h>
#include <esp_partition.h>
#include <esp_sleep.h>
#include <math.h>

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
#define ACCENT_A  0xFFB300
#define ACCENT_B  0xFF4040

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
  lv_obj_t *l=lv_label_create(s); lv_label_set_text(l,"AMBIENT/OS");
  lv_obj_set_style_text_color(l,lv_color_hex(ACCENT_A),0);
  lv_obj_set_style_text_font(l,&lv_font_montserrat_48,0);
  lv_obj_align(l,LV_ALIGN_CENTER,0,-30);
  lv_obj_t *l2=lv_label_create(s); lv_label_set_text(l2,"CALIBRATING MIC");
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
// AMBIENT-OS  --  ambient sound-pressure-level meter
// Uses the Waveshare ES8311 codec on I2S (BCLK=41 WS=45 DIN=42 MCLK=16).
// ====================================================================
#include <ESP_I2S.h>
#include "es8311.h"

#define MIC_RATE 16000
#define SAMPLES  1024
#define I2S_BCLK 41
#define I2S_WS   45
#define I2S_DOUT 40
#define I2S_DIN  42
#define I2S_MCLK 16
#define PA_EN    46

static I2SClass g_i2s;
static int16_t g_buf[SAMPLES*2];   // stereo frames
static lv_obj_t *g_db_big=nullptr, *g_bar=nullptr, *g_peak=nullptr, *g_meter=nullptr;
static float g_db_peak=0;
static bool  g_audio_ready=false;

static esp_err_t es8311_setup(){
  es8311_handle_t h = es8311_create(0, ES8311_ADDRRES_0);
  if(!h) return ESP_FAIL;
  const es8311_clock_config_t clk = {
    .mclk_inverted=false, .sclk_inverted=false, .mclk_from_mclk_pin=true,
    .mclk_frequency = MIC_RATE*256, .sample_frequency = MIC_RATE
  };
  if(es8311_init(h,&clk,ES8311_RESOLUTION_16,ES8311_RESOLUTION_16)!=ESP_OK) return ESP_FAIL;
  es8311_sample_frequency_config(h, clk.mclk_frequency, clk.sample_frequency);
  es8311_microphone_config(h, false);
  es8311_microphone_gain_set(h, (es8311_mic_gain_t)4);
  return ESP_OK;
}
static void mic_setup(){
  g_i2s.setPins(I2S_BCLK, I2S_WS, I2S_DOUT, I2S_DIN, I2S_MCLK);
  if(!g_i2s.begin(I2S_MODE_STD, MIC_RATE, I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO, I2S_STD_SLOT_BOTH)) return;
  pinMode(PA_EN, OUTPUT); digitalWrite(PA_EN, HIGH);
  if(es8311_setup()!=ESP_OK) return;
  g_audio_ready=true;
}

static void sample_tick(lv_timer_t*){
  if(!g_audio_ready) return;
  size_t br = g_i2s.readBytes((char*)g_buf, sizeof(g_buf));
  int frames = br / 4; // stereo 16-bit
  if(frames<=0) return;
  double sum=0;
  for(int i=0;i<frames;i++){
    double l = g_buf[i*2]   / 32768.0;
    double r = g_buf[i*2+1] / 32768.0;
    double s = (l + r) * 0.5;
    sum += s*s;
  }
  double rms = sqrt(sum/frames);
  if(rms<1e-6) rms=1e-6;
  float db = 20.0f * log10(rms) + 94.0f;
  if(db<0) db=0; if(db>120) db=120;
  if(db>g_db_peak) g_db_peak = db;
  else g_db_peak = g_db_peak*0.96f + db*0.04f;

  char b[12]; snprintf(b,12,"%.1f", db); lv_label_set_text(g_db_big, b);
  uint32_t col = db<55?COL_GREEN : db<75?ACCENT_A : db<90?ACCENT_B : COL_RED;
  lv_obj_set_style_text_color(g_db_big, lv_color_hex(col),0);
  lv_bar_set_value(g_bar, (int)db, LV_ANIM_OFF);
  lv_obj_set_style_bg_color(g_bar, lv_color_hex(col), LV_PART_INDICATOR);
  char p[24]; snprintf(p,24,"peak %.1f dB", g_db_peak); lv_label_set_text(g_peak, p);

  if(g_meter){
    int segs=20, lit=(int)((db/120.0f)*segs);
    while(lv_obj_get_child_cnt(g_meter)<(uint32_t)segs){
      lv_obj_t *seg=lv_obj_create(g_meter); lv_obj_remove_style_all(seg);
      lv_obj_set_size(seg, 14, 26); lv_obj_set_style_radius(seg,3,0);
    }
    for(int i=0;i<segs;i++){
      lv_obj_t *seg = lv_obj_get_child(g_meter, i);
      uint32_t c = (i<8)?COL_GREEN:(i<14?ACCENT_A:(i<17?ACCENT_B:COL_RED));
      lv_obj_set_style_bg_color(seg, lv_color_hex(i<lit?c:0x222222),0);
      lv_obj_set_style_bg_opa(seg, LV_OPA_COVER,0);
    }
  }
}

static void build_home(){
  lv_obj_t *s=lv_obj_create(NULL);
  lv_obj_set_style_bg_color(s,lv_color_hex(COL_BG),0);
  lv_obj_set_style_bg_opa(s,LV_OPA_COVER,0);
  lv_obj_clear_flag(s,LV_OBJ_FLAG_SCROLLABLE);
  add_header_strip(s,"AMBIENT",ACCENT_A);
  lv_obj_t *sub=lv_label_create(s); lv_label_set_text(sub,"ES8311  ·  SPL dB approx");
  lv_obj_set_style_text_color(sub,lv_color_hex(ACCENT_B),0);
  lv_obj_set_style_text_font(sub,&lv_font_montserrat_12,0);
  lv_obj_set_style_text_letter_space(sub,3,0);
  lv_obj_align(sub,LV_ALIGN_TOP_LEFT,24,54);

  g_db_big=lv_label_create(s); lv_label_set_text(g_db_big,"--");
  lv_obj_set_style_text_color(g_db_big,lv_color_hex(ACCENT_A),0);
  lv_obj_set_style_text_font(g_db_big,&lv_font_montserrat_48,0);
  lv_obj_align(g_db_big, LV_ALIGN_TOP_MID, 0, 100);
  lv_obj_t *db=lv_label_create(s); lv_label_set_text(db,"dB SPL");
  lv_obj_set_style_text_color(db,lv_color_hex(COL_TEXT_DIM),0);
  lv_obj_set_style_text_font(db,&lv_font_montserrat_18,0);
  lv_obj_align(db, LV_ALIGN_TOP_MID, 0, 160);

  g_bar=lv_bar_create(s); lv_obj_set_size(g_bar, LCD_W-80, 14);
  lv_obj_align(g_bar, LV_ALIGN_TOP_MID, 0, 210);
  lv_bar_set_range(g_bar, 0, 120);
  lv_obj_set_style_bg_color(g_bar, lv_color_hex(0x101010), LV_PART_MAIN);
  lv_obj_set_style_radius(g_bar, 7, LV_PART_MAIN);
  lv_obj_set_style_radius(g_bar, 7, LV_PART_INDICATOR);

  g_meter=lv_obj_create(s); lv_obj_remove_style_all(g_meter);
  lv_obj_set_size(g_meter, LCD_W-60, 30);
  lv_obj_align(g_meter, LV_ALIGN_TOP_MID, 0, 240);
  lv_obj_set_flex_flow(g_meter, LV_FLEX_FLOW_ROW);
  lv_obj_set_style_pad_column(g_meter, 3, 0);
  lv_obj_clear_flag(g_meter, LV_OBJ_FLAG_SCROLLABLE);

  g_peak=lv_label_create(s); lv_label_set_text(g_peak,"peak --");
  lv_obj_set_style_text_color(g_peak,lv_color_hex(ACCENT_B),0);
  lv_obj_set_style_text_font(g_peak,&lv_font_montserrat_14,0);
  lv_obj_align(g_peak, LV_ALIGN_TOP_MID, 0, 285);

  add_boot_hint(s);
  lv_scr_load_anim(s, LV_SCR_LOAD_ANIM_FADE_ON, 260,0,false);
  mic_setup();
  lv_timer_create(sample_tick, 150, NULL);
}

void setup(){ Serial.begin(115200); hw_boot(); splash_show(); build_home(); }
void loop(){ lv_timer_handler(); poll_boot_exit(); delay(5); }
