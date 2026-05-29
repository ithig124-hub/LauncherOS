// =====================================================================
// NOTE-OS.ino  --  LauncherOS-compatible Scribble Notebook (v2 - major rewrite)
// Target  : Waveshare ESP32-S3 Touch AMOLED 2.06 (410 x 502, FT3168, AXP2101)
// Build   : ESP32-S3 Dev Module, PSRAM=OPI, Partition=Minimal SPIFFS (large APP)
// Output  : NoteOS.ino.bin -> /WATCH/bin/noteos.bin
//
// v2 changes:
//   * Crash fix: I2C NACK → FT3168 not present is now handled gracefully
//     (no more LoadProhibited @ EXCVADDR 0x22).
//   * Boot lands on a NOTES LIBRARY (list/open/edit/delete/new) — no more
//     dropping straight into a pad.
//   * Multi-stroke commit timer (600 ms) — letters like t, i, j, x, k, f
//     finally work because the recogniser waits for additional strokes.
//   * Stroke-trail smoothing (midpoint interpolation) + thicker ink (6 px)
//     for far better visibility while drawing.
//   * Lowercase a-z templates + extra uppercase variants + 0-9 + . , ! ? @
//   * Mode toggle:  ABC  →  abc  →  123  →  SYM
//   * Word suggestions / autocomplete:  top 3 tappable chips (~600 words).
//   * Quick gestures inside the pad:
//        swipe LEFT  = backspace
//        swipe RIGHT = space
//        swipe DOWN  = newline
//   * Undo last letter (UNDO pill).
//   * Save to SD card /WATCH/notes/*.txt  + Read mode + Delete.
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
#include <vector>

#include "Arduino_GFX_Library.h"
#include "Arduino_DriveBus_Library.h"
#include "lv_conf.h"
#include "lvgl.h"

#define XPOWERS_CHIP_AXP2101
#include "XPowersLib.h"

// ---- Forward types (must precede Arduino auto-prototypes) ----------
struct Pt { float x, y; int s; };   // s = stroke index (0,1,2..) -- lets $P treat multi-stroke as one cloud
struct Tpl;       // full definition lives further down (built-in glyph templates)
struct UserTpl;   // full definition lives further down (trained user templates)

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

// ---- Palette --------------------------------------------------------
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

// ---- Accents --------------------------------------------------------
#define ACCENT_A  0xFFFFFF
#define ACCENT_B  0x888888
#define ACCENT_HL 0x00E5FF

// ---- Singletons -----------------------------------------------------
Arduino_DataBus *g_bus = new Arduino_ESP32QSPI(LCD_CS,LCD_SCLK,LCD_SDIO0,LCD_SDIO1,LCD_SDIO2,LCD_SDIO3);
Arduino_GFX     *gfx   = new Arduino_CO5300(g_bus,LCD_RESET,0,LCD_W,LCD_H,22,0,0,0);
std::shared_ptr<Arduino_IIC_DriveBus> g_iic = std::make_shared<Arduino_HWIIC>(IIC_SDA,IIC_SCL,&Wire);
void Arduino_IIC_Touch_Interrupt(void);
std::unique_ptr<Arduino_IIC> FT3168(new Arduino_FT3x68(g_iic,FT3168_DEVICE_ADDRESS,DRIVEBUS_DEFAULT_VALUE,TP_INT,Arduino_IIC_Touch_Interrupt));
void Arduino_IIC_Touch_Interrupt(void){FT3168->IIC_Interrupt_Flag=true;}
XPowersPMU g_pmu; bool g_has_pmu=false;
static bool g_has_touch=false;     // <— critical: NoteOS v1 crashed because we called
                                   //   FT3168->IIC_Read_Device_Value() even when the IC
                                   //   was NACKing on the I2C bus. EXCVADDR=0x22 was
                                   //   dereferencing a half-initialised driver field.

// ---- LVGL plumbing --------------------------------------------------
static lv_disp_draw_buf_t g_db;  static lv_color_t *g_lvbuf=nullptr;
static lv_disp_drv_t      g_dd;  static lv_indev_drv_t g_id;
static void lv_flush(lv_disp_drv_t*d,const lv_area_t*a,lv_color_t*p){
  uint32_t w=a->x2-a->x1+1, h=a->y2-a->y1+1;
  gfx->draw16bitRGBBitmap(a->x1,a->y1,(uint16_t*)p,w,h); lv_disp_flush_ready(d);
}
static void lv_touch_read(lv_indev_drv_t*, lv_indev_data_t *d){
  // Hard guard: if the touch controller never came up, never talk to it.
  if(!g_has_touch){ d->state=LV_INDEV_STATE_REL; return; }
  int32_t x = FT3168->IIC_Read_Device_Value(FT3168->Arduino_IIC_Touch::Value_Information::TOUCH_COORDINATE_X);
  int32_t y = FT3168->IIC_Read_Device_Value(FT3168->Arduino_IIC_Touch::Value_Information::TOUCH_COORDINATE_Y);
  int     f = FT3168->IIC_Read_Device_Value(FT3168->Arduino_IIC_Touch::Value_Information::TOUCH_FINGER_NUMBER);
  bool valid = (x>=0 && x<LCD_W && y>=0 && y<LCD_H && f>=0 && f<=5);
  if(!valid){ d->state=LV_INDEV_STATE_REL; return; }
  if(f>0){ d->state=LV_INDEV_STATE_PR; d->point.x=x; d->point.y=y; }
  else d->state=LV_INDEV_STATE_REL;
}

// I2C ping helper - returns true if a slave at addr ACKs.
static bool i2c_ping(uint8_t addr){
  Wire.beginTransmission(addr);
  return (Wire.endTransmission() == 0);
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
  lv_obj_set_style_opa(img, 70, 0);
}

// ---- SD scaffold ----------------------------------------------------
static void sd_ensure_dirs(){
  if(!SD_MMC.exists("/WATCH")) SD_MMC.mkdir("/WATCH");
  if(!SD_MMC.exists("/WATCH/notes")) SD_MMC.mkdir("/WATCH/notes");
}

// ---- HW boot --------------------------------------------------------
static void hw_boot(){
  pinMode(BOOT_BTN, INPUT_PULLUP);
  if(!gfx->begin()) Serial.println("gfx begin fail");
  gfx->fillScreen(0x0000);
  // I2C unstick (9 clocks)
  pinMode(IIC_SCL, OUTPUT);
  for(int i=0;i<9;i++){ digitalWrite(IIC_SCL,LOW); delayMicroseconds(5); digitalWrite(IIC_SCL,HIGH); delayMicroseconds(5); }
  Wire.begin(IIC_SDA, IIC_SCL); Wire.setClock(400000);
  // PMU FIRST (touch rail comes from it)
  if(g_pmu.begin(Wire, AXP2101_SLAVE_ADDRESS, IIC_SDA, IIC_SCL)){
    g_has_pmu=true;
    g_pmu.disableTSPinMeasure(); g_pmu.enableBattVoltageMeasure();
    g_pmu.setALDO1Voltage(3300); g_pmu.enableALDO1();
    g_pmu.setBLDO1Voltage(3300); g_pmu.enableBLDO1();
    delay(60);
  }
  // Touch reset pulse
  pinMode(TP_RESET, OUTPUT);
  digitalWrite(TP_RESET, LOW);  delay(50);
  digitalWrite(TP_RESET, HIGH); delay(220);
  // PING the controller first. Only call ->begin() and only ever read
  // values out of it if we get an ACK. This is THE fix for the
  // LoadProhibited @ EXCVADDR 0x22 crash.
  bool ack = i2c_ping(FT3168_DEVICE_ADDRESS);
  if(!ack){
    // one retry with a longer settle
    delay(120);
    ack = i2c_ping(FT3168_DEVICE_ADDRESS);
  }
  if(ack){
    FT3168->begin();
    g_has_touch = true;
  } else {
    g_has_touch = false;
    Serial.println("[NoteOS] FT3168 NACK -- running headless touch.");
  }
  // LVGL
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
  sd_ensure_dirs();
  load_wifi_config();
}

// ---- Splash ---------------------------------------------------------
static void splash_show(){
  lv_obj_t *s=lv_obj_create(NULL);
  lv_obj_set_style_bg_color(s,lv_color_hex(COL_BG),0);
  lv_obj_set_style_bg_opa(s,LV_OPA_COVER,0);
  lv_obj_clear_flag(s,LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_t *l=lv_label_create(s); lv_label_set_text(l,"NOTE/OS");
  lv_obj_set_style_text_color(l,lv_color_hex(ACCENT_A),0);
  lv_obj_set_style_text_font(l,&lv_font_montserrat_48,0);
  lv_obj_align(l,LV_ALIGN_CENTER,0,-30);
  lv_obj_t *l2=lv_label_create(s); lv_label_set_text(l2,"SCRIBBLE NOTEBOOK");
  lv_obj_set_style_text_color(l2,lv_color_hex(ACCENT_HL),0);
  lv_obj_set_style_text_font(l2,&lv_font_montserrat_14,0);
  lv_obj_set_style_text_letter_space(l2,3,0);
  lv_obj_align(l2,LV_ALIGN_CENTER,0,20);
  lv_obj_t *bar=lv_bar_create(s); lv_obj_set_size(bar,260,4);
  lv_obj_align(bar,LV_ALIGN_CENTER,0,80);
  lv_obj_set_style_bg_color(bar,lv_color_hex(COL_PANEL2),LV_PART_MAIN);
  lv_obj_set_style_bg_color(bar,lv_color_hex(ACCENT_HL),LV_PART_INDICATOR);
  lv_bar_set_range(bar,0,100); lv_bar_set_value(bar,0,LV_ANIM_OFF);
  lv_obj_t *step=lv_label_create(s); lv_label_set_text(step,"BOOTING");
  lv_obj_set_style_text_color(step,lv_color_hex(COL_TEXT_DIM),0);
  lv_obj_set_style_text_font(step,&lv_font_montserrat_12,0);
  lv_obj_set_style_text_letter_space(step,3,0);
  lv_obj_align(step,LV_ALIGN_CENTER,0,110);
  // touch-status pill
  lv_obj_t *tdot=lv_obj_create(s); lv_obj_remove_style_all(tdot);
  lv_obj_set_size(tdot,8,8); lv_obj_align(tdot,LV_ALIGN_TOP_RIGHT,-16,16);
  lv_obj_set_style_radius(tdot,4,0);
  lv_obj_set_style_bg_color(tdot, lv_color_hex(g_has_touch?COL_GREEN:COL_RED),0);
  lv_obj_set_style_bg_opa(tdot, LV_OPA_COVER,0);
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

// ---- Battery pill on every screen ----------------------------------
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

// =====================================================================
//   $P  Point-Cloud Recognizer  (multi-stroke aware)
// =====================================================================
#define MAXP   384
#define N_PTS  48                   // points after resampling (was 32)
static Pt   g_pts[MAXP];
static int  g_pn=0;
static int  g_stroke_id=0;
static bool g_drawing=false;
static lv_obj_t *g_pad=nullptr, *g_pad_frame=nullptr;
static lv_color_t *g_pad_buf=nullptr;
static int g_pad_w=0, g_pad_h=0;

static float pt_dist(Pt a, Pt b){ return sqrtf((a.x-b.x)*(a.x-b.x)+(a.y-b.y)*(a.y-b.y)); }
static float path_len(Pt *p, int n){
  float s=0;
  for(int i=1;i<n;i++) if(p[i].s==p[i-1].s) s+=pt_dist(p[i-1],p[i]);
  return s;
}
static void resample(Pt *out, Pt *in, int n){
  float I = path_len(in,n) / (N_PTS-1); if(I<1e-6f) I=1e-6f;
  float D = 0;
  out[0] = in[0]; int j = 1;
  for(int i=1;i<n;i++){
    if(in[i].s != in[i-1].s){ D = 0; continue; }       // stroke break — no interpolation across
    float d = pt_dist(in[i-1], in[i]);
    if(D + d >= I){
      float t = (I-D)/d;
      Pt np = { in[i-1].x + t*(in[i].x-in[i-1].x),
                in[i-1].y + t*(in[i].y-in[i-1].y),
                in[i].s };
      out[j++] = np;
      in[i-1] = np;
      D = 0; i--;
      if(j>=N_PTS) break;
    } else D += d;
  }
  while(j<N_PTS){ out[j] = out[j-1]; j++; }
}
static void scale_and_translate(Pt *p){
  float xmin=1e9,xmax=-1e9,ymin=1e9,ymax=-1e9;
  for(int i=0;i<N_PTS;i++){
    if(p[i].x<xmin)xmin=p[i].x; if(p[i].x>xmax)xmax=p[i].x;
    if(p[i].y<ymin)ymin=p[i].y; if(p[i].y>ymax)ymax=p[i].y;
  }
  float w=xmax-xmin, h=ymax-ymin, s=w>h?w:h; if(s<1e-3f) s=1;
  float cx=0,cy=0;
  for(int i=0;i<N_PTS;i++){ p[i].x=(p[i].x-xmin)/s; p[i].y=(p[i].y-ymin)/s; cx+=p[i].x; cy+=p[i].y; }
  cx/=N_PTS; cy/=N_PTS;
  for(int i=0;i<N_PTS;i++){ p[i].x-=cx; p[i].y-=cy; }
}
static float greedy_cloud_match(Pt *a, Pt *b){
  float e = 0.50f;
  int step = (int)floorf(powf(N_PTS, 1.0f-e));
  if(step<1) step=1;
  float minD = 1e9;
  for(int i=0;i<N_PTS;i+=step){
    bool used[N_PTS]={0}; float dsum=0; int idx=i;
    for(int k=0;k<N_PTS;k++){
      float dmin=1e9; int jbest=-1;
      for(int j=0;j<N_PTS;j++) if(!used[j]){
        float d = pt_dist(a[idx], b[j]);
        if(d<dmin){ dmin=d; jbest=j; }
      }
      if(jbest<0) break;
      used[jbest]=true;
      float w = 1.0f - ((k - i + N_PTS) % N_PTS) / (float)N_PTS;
      dsum += w * dmin;
      idx = (idx+1) % N_PTS;
    }
    if(dsum < minD) minD = dsum;
  }
  return minD;
}

// ---- Compact template DB.  Each template is stored as a list of
// (x,y,strokeIdx) triplets in [0..100] space.  255 in the X slot ends.
struct Tpl { char ch; uint8_t nstroke; const uint8_t pts[64]; };
// Macro for terseness (note: every triplet must fit in 64 bytes ⇒ max 21 triplets)
#define END 255,255,255
static const Tpl TPLS[] = {
  // ----- UPPERCASE -----
  { 'A', 1, { 10,90,0, 50,5,0,  90,90,0,  25,55,0, 75,55,0, END }},
  { 'B', 2, { 15,5,0, 15,95,0, 15,50,0, 75,28,0, 15,5,0,
              15,50,1, 80,75,1, 15,95,1, END }},
  { 'C', 1, { 90,18,0, 60,5,0, 30,15,0, 12,45,0, 30,80,0, 60,92,0, 90,80,0, END }},
  { 'D', 1, { 15,5,0, 15,95,0, 70,95,0, 90,55,0, 70,5,0, 15,5,0, END }},
  { 'E', 1, { 90,5,0, 15,5,0, 15,95,0, 90,95,0, 15,95,0, 15,50,0, 65,50,0, END }},
  { 'F', 2, { 90,5,0, 15,5,0, 15,95,0,
              15,50,1, 65,50,1, END }},
  { 'G', 1, { 90,18,0, 60,5,0, 30,15,0, 12,45,0, 30,80,0, 70,90,0, 92,70,0, 92,50,0, 60,50,0, END }},
  { 'H', 3, { 15,5,0, 15,95,0,
              90,5,1, 90,95,1,
              15,50,2, 90,50,2, END }},
  { 'I', 1, { 50,5,0, 50,95,0, END }},
  { 'J', 1, { 80,5,0, 80,75,0, 55,92,0, 25,80,0, 18,60,0, END }},
  { 'K', 3, { 15,5,0, 15,95,0,
              15,50,1, 85,5,1,
              15,50,2, 85,95,2, END }},
  { 'L', 1, { 15,5,0, 15,95,0, 90,95,0, END }},
  { 'M', 1, { 5,95,0, 5,5,0, 50,55,0, 95,5,0, 95,95,0, END }},
  { 'N', 1, { 10,95,0, 10,5,0, 90,95,0, 90,5,0, END }},
  { 'O', 1, { 50,5,0, 20,18,0, 8,45,0, 20,75,0, 50,92,0, 80,75,0, 92,45,0, 80,18,0, 50,5,0, END }},
  { 'P', 1, { 15,95,0, 15,5,0, 75,5,0, 92,25,0, 75,48,0, 15,48,0, END }},
  { 'Q', 2, { 50,5,0, 20,18,0, 8,45,0, 20,75,0, 50,92,0, 80,75,0, 92,45,0, 80,18,0, 50,5,0,
              60,65,1, 95,98,1, END }},
  { 'R', 1, { 15,95,0, 15,5,0, 75,5,0, 92,25,0, 75,48,0, 15,48,0, 50,48,0, 92,95,0, END }},
  { 'S', 1, { 90,18,0, 60,5,0, 30,8,0, 15,28,0, 50,48,0, 85,60,0, 88,82,0, 65,92,0, 30,92,0, 12,75,0, END }},
  { 'T', 2, { 5,5,0, 95,5,0,
              50,5,1, 50,95,1, END }},
  { 'U', 1, { 10,5,0, 10,65,0, 30,90,0, 70,90,0, 90,65,0, 90,5,0, END }},
  { 'V', 1, { 10,5,0, 50,95,0, 90,5,0, END }},
  { 'W', 1, { 5,5,0, 25,95,0, 50,30,0, 75,95,0, 95,5,0, END }},
  { 'X', 2, { 10,5,0, 90,95,0,
              90,5,1, 10,95,1, END }},
  { 'Y', 2, { 10,5,0, 50,50,0,  90,5,0,
              50,50,1, 50,95,1, END }},
  { 'Z', 1, { 10,5,0, 90,5,0, 10,95,0, 90,95,0, END }},

  // ----- LOWERCASE -----
  // most lowercase share their uppercase shape; the recognizer outputs
  // the case based on g_mode (see resolve_case()).
  { 'a', 1, { 75,40,0, 50,30,0, 25,40,0, 20,65,0, 40,85,0, 70,80,0, 75,40,0, 75,90,0, END }},
  { 'b', 2, { 15,5,0, 15,95,0,
              15,55,1, 40,40,1, 70,50,1, 75,75,1, 55,92,1, 25,90,1, 15,80,1, END }},
  { 'c', 1, { 80,40,0, 50,30,0, 25,45,0, 25,75,0, 50,90,0, 80,80,0, END }},
  { 'd', 2, { 85,5,0, 85,95,0,
              85,50,1, 60,38,1, 30,48,1, 22,72,1, 40,90,1, 70,88,1, 85,75,1, END }},
  { 'e', 1, { 22,68,0, 78,65,0, 75,45,0, 50,32,0, 25,45,0, 22,72,0, 45,90,0, 78,82,0, END }},
  { 'f', 2, { 75,18,0, 60,8,0, 40,15,0, 35,40,0, 35,95,0,
              15,40,1, 65,40,1, END }},
  { 'g', 2, { 75,40,0, 50,30,0, 25,45,0, 28,65,0, 55,75,0, 75,65,0, 75,40,0,
              75,40,1, 75,95,1, 60,108,1, 30,100,1, END }},
  { 'h', 2, { 15,5,0, 15,95,0,
              15,55,1, 40,38,1, 75,45,1, 80,95,1, END }},
  { 'i', 2, { 50,40,0, 50,95,0,
              50,15,1, 50,22,1, END }},
  { 'j', 2, { 70,40,0, 70,95,0, 50,108,0, 30,98,0,
              70,15,1, 70,22,1, END }},
  { 'k', 3, { 15,5,0, 15,95,0,
              15,65,1, 75,40,1,
              35,55,2, 80,95,2, END }},
  { 'l', 1, { 50,5,0, 50,95,0, END }},
  { 'm', 1, { 10,40,0, 10,95,0, 10,50,0, 38,38,0, 50,55,0, 50,95,0, 50,55,0, 75,38,0, 90,55,0, 90,95,0, END }},
  { 'n', 1, { 15,40,0, 15,95,0, 15,55,0, 45,38,0, 80,50,0, 80,95,0, END }},
  { 'o', 1, { 50,30,0, 25,40,0, 18,60,0, 30,82,0, 60,92,0, 82,75,0, 85,55,0, 70,38,0, 50,30,0, END }},
  { 'p', 2, { 15,40,0, 15,108,0,
              15,55,1, 40,40,1, 75,50,1, 80,72,1, 55,92,1, 25,90,1, 15,80,1, END }},
  { 'q', 2, { 80,40,0, 80,108,0,
              80,55,1, 60,40,1, 30,48,1, 22,72,1, 40,90,1, 70,88,1, 80,75,1, END }},
  { 'r', 1, { 15,40,0, 15,95,0, 15,55,0, 40,38,0, 70,45,0, END }},
  { 's', 1, { 80,42,0, 55,32,0, 30,40,0, 30,55,0, 70,70,0, 80,82,0, 60,92,0, 25,88,0, END }},
  { 't', 2, { 45,15,0, 45,90,0, 65,95,0,
              25,40,1, 70,40,1, END }},
  { 'u', 1, { 15,40,0, 15,80,0, 35,92,0, 70,88,0, 80,70,0, 80,40,0, 80,95,0, END }},
  { 'v', 1, { 15,40,0, 50,95,0, 85,40,0, END }},
  { 'w', 1, { 10,40,0, 28,95,0, 50,55,0, 70,95,0, 90,40,0, END }},
  { 'x', 2, { 20,40,0, 80,95,0,
              80,40,1, 20,95,1, END }},
  { 'y', 1, { 15,40,0, 45,75,0, 80,40,0, 50,108,0, END }},
  { 'z', 1, { 20,40,0, 80,40,0, 20,95,0, 80,95,0, END }},

  // ----- DIGITS -----
  { '0', 1, { 50,5,0, 25,18,0, 12,48,0, 25,78,0, 50,92,0, 75,78,0, 88,48,0, 75,18,0, 50,5,0, END }},
  { '1', 1, { 30,20,0, 55,5,0, 55,95,0, 30,95,0, 80,95,0, END }},
  { '2', 1, { 18,22,0, 50,5,0, 80,18,0, 80,40,0, 18,92,0, 85,92,0, END }},
  { '3', 1, { 20,15,0, 60,5,0, 85,22,0, 60,48,0, 30,50,0, 70,52,0, 90,72,0, 70,92,0, 30,92,0, 12,75,0, END }},
  { '4', 2, { 70,5,0, 12,68,0, 90,68,0,
              70,30,1, 70,95,1, END }},
  { '5', 1, { 85,5,0, 25,5,0, 18,45,0, 60,40,0, 88,60,0, 75,90,0, 30,92,0, 15,78,0, END }},
  { '6', 1, { 80,12,0, 50,5,0, 25,20,0, 12,55,0, 22,82,0, 55,92,0, 85,80,0, 88,60,0, 65,48,0, 25,55,0, END }},
  { '7', 1, { 10,8,0, 90,8,0, 35,95,0, END }},
  { '8', 1, { 50,5,0, 25,15,0, 22,38,0, 50,48,0, 80,38,0, 75,15,0, 50,5,0,
              50,48,0, 22,60,0, 18,82,0, 50,92,0, 82,82,0, 80,60,0, 50,48,0, END }},
  { '9', 1, { 80,40,0, 55,30,0, 25,38,0, 18,58,0, 40,72,0, 70,68,0, 85,52,0, 80,75,0, 55,95,0, 30,95,0, END }},

  // ----- PUNCTUATION (compact, distinctive shapes) -----
  { '.', 1, { 50,90,0, 52,92,0, 50,90,0, END }},
  { ',', 1, { 55,82,0, 52,92,0, 40,105,0, END }},
  { '!', 2, { 50,5,0, 50,70,0,
              50,90,1, 52,92,1, END }},
  { '?', 2, { 20,18,0, 50,5,0, 80,15,0, 80,40,0, 50,55,0, 50,70,0,
              50,90,1, 52,92,1, END }},
  { '@', 1, { 70,40,0, 50,30,0, 30,40,0, 28,65,0, 50,80,0, 70,72,0, 70,45,0, 85,55,0, 85,75,0, 75,90,0, 50,92,0, 25,82,0, 15,55,0, 35,18,0, 70,12,0, 88,30,0, END }},
  { '-', 1, { 20,50,0, 80,50,0, END }},
  { '+', 2, { 50,20,0, 50,80,0,
              20,50,1, 80,50,1, END }},
  { '=', 2, { 20,40,0, 80,40,0,
              20,65,1, 80,65,1, END }},
  { ':', 2, { 50,30,0, 52,32,0,
              50,80,1, 52,82,1, END }},
  { ';', 2, { 50,30,0, 52,32,0,
              55,75,1, 50,85,1, 38,98,1, END }},
  { '\'',1, { 50,8,0, 48,30,0, END }},
};
static const int NTPL = sizeof(TPLS)/sizeof(TPLS[0]);

// =====================================================================
//   USER TEMPLATES  (handwriting training)
//   Saved to /WATCH/notes/_handwriting.dat as a tiny binary file.
//   Format:
//     magic    : "HW02"     (4 bytes)
//     count    : uint16_t   (number of templates)
//     for each:
//       ch    : char        (1)
//       npts  : uint8_t     (1)
//       pts[] : npts * (x,y,s)  (each 1 byte; x,y in 0..120, s in 0..7)
// =====================================================================
#define MAX_USER_PTS_PER_TPL  56
struct UserTpl {
  char    ch;
  uint8_t npts;
  uint8_t pts[MAX_USER_PTS_PER_TPL*3];  // x,y,s triplets
};
static std::vector<UserTpl> g_user_tpls;

// All characters that the training wizard walks through, in order.
static const char *TRAIN_CHARS =
  "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
  "abcdefghijklmnopqrstuvwxyz"
  "0123456789"
  ".,!?@-+=:;'";
static const int TRAIN_TOTAL = 26+26+10+11;   // = 73

static int unpack_user_tpl(const UserTpl *t, Pt *out, int maxN){
  int n = (t->npts > maxN) ? maxN : t->npts;
  for(int i=0;i<n;i++){
    out[i].x = t->pts[i*3+0];
    out[i].y = t->pts[i*3+1];
    out[i].s = t->pts[i*3+2];
  }
  return n;
}

static bool save_user_templates(){
  File f = SD_MMC.open("/WATCH/notes/_handwriting.dat", FILE_WRITE);
  if(!f) return false;
  f.write((const uint8_t*)"HW02", 4);
  uint16_t cnt = (uint16_t)g_user_tpls.size();
  f.write((const uint8_t*)&cnt, 2);
  for(auto &u : g_user_tpls){
    f.write((const uint8_t*)&u.ch, 1);
    f.write(&u.npts, 1);
    f.write(u.pts, u.npts*3);
  }
  f.close();
  return true;
}
static bool load_user_templates(){
  g_user_tpls.clear();
  File f = SD_MMC.open("/WATCH/notes/_handwriting.dat", FILE_READ);
  if(!f) return false;
  char magic[4]; if(f.read((uint8_t*)magic,4)!=4){ f.close(); return false; }
  if(memcmp(magic,"HW02",4)!=0){ f.close(); return false; }
  uint16_t cnt=0; f.read((uint8_t*)&cnt,2);
  for(uint16_t i=0;i<cnt;i++){
    UserTpl u; memset(&u,0,sizeof(u));
    f.read((uint8_t*)&u.ch,1);
    f.read(&u.npts,1);
    if(u.npts > MAX_USER_PTS_PER_TPL) u.npts = MAX_USER_PTS_PER_TPL;
    f.read(u.pts, u.npts*3);
    g_user_tpls.push_back(u);
  }
  f.close();
  return true;
}

static int unpack_tpl(const Tpl *t, Pt *out, int maxN){
  int n=0;
  for(int i=0;i<64 && n<maxN; i+=3){
    uint8_t x = t->pts[i];
    uint8_t y = t->pts[i+1];
    uint8_t s = t->pts[i+2];
    if(x==255 && y==255 && s==255) break;
    out[n].x = x; out[n].y = y; out[n].s = s;
    n++;
  }
  return n;
}

static char classify_letter(Pt *raw, int n, char *out2 /*alt*/){
  if(n<6){ if(out2) *out2='?'; return '?'; }
  Pt re[N_PTS]; resample(re, raw, n);
  scale_and_translate(re);
  float best = 1e9, best2 = 1e9; char bestCh='?', bestCh2='?';
  Pt tp_raw[64], tp[N_PTS];

  auto consider = [&](char ch, float d){
    if(d < best){ best2=best; bestCh2=bestCh; best=d; bestCh=ch; }
    else if(d < best2){ best2=d; bestCh2=ch; }
  };

  // ---- built-in templates ----
  for(int t=0;t<NTPL;t++){
    int tn = unpack_tpl(&TPLS[t], tp_raw, 64);
    if(tn<2) continue;
    resample(tp, tp_raw, tn);
    scale_and_translate(tp);
    float d = greedy_cloud_match(re, tp);
    consider(TPLS[t].ch, d);
  }
  // ---- user templates (your own handwriting) ----
  // We give them a small distance bonus so when YOUR shape and the
  // factory shape are roughly equal, YOUR one wins.
  Pt utp_raw[MAX_USER_PTS_PER_TPL];
  for(size_t u=0; u<g_user_tpls.size(); u++){
    int tn = unpack_user_tpl(&g_user_tpls[u], utp_raw, MAX_USER_PTS_PER_TPL);
    if(tn<2) continue;
    resample(tp, utp_raw, tn);
    scale_and_translate(tp);
    float d = greedy_cloud_match(re, tp) * 0.85f;   // 15% bonus
    consider(g_user_tpls[u].ch, d);
  }
  if(out2) *out2 = bestCh2;
  return bestCh;
}

// =====================================================================
//   MODES   ABC / abc / 123 / SYM
// =====================================================================
enum Mode { M_UP=0, M_LO=1, M_NUM=2, M_SYM=3 };
static Mode g_mode = M_LO;

static bool is_letter(char c){ return (c>='A'&&c<='Z') || (c>='a'&&c<='z'); }
static bool is_digit(char c){ return c>='0'&&c<='9'; }
static bool is_punct(char c){
  switch(c){ case '.': case ',': case '!': case '?': case '@':
             case '-': case '+': case '=': case ':': case ';':
             case '\'': return true; default: return false; }
}

// Pick the best candidate that matches the current mode.
static char resolve_for_mode(char top, char alt){
  auto match = [](char c)->bool{
    switch(g_mode){
      case M_UP:  return c>='A'&&c<='Z';
      case M_LO:  return c>='a'&&c<='z';
      case M_NUM: return c>='0'&&c<='9';
      case M_SYM: return is_punct(c);
    }
    return true;
  };
  if(match(top)) return top;
  if(match(alt)) return alt;
  // map case if possible (recogniser may have returned the wrong case)
  if(g_mode==M_UP){
    if(top>='a'&&top<='z') return top-32;
    if(alt>='a'&&alt<='z') return alt-32;
  }
  if(g_mode==M_LO){
    if(top>='A'&&top<='Z') return top+32;
    if(alt>='A'&&alt<='Z') return alt+32;
  }
  return top;
}

// =====================================================================
//   AUTOCOMPLETE  (compact built-in dictionary, ~360 common words)
// =====================================================================
static const char* DICT[] = {
  "the","be","to","of","and","a","in","that","have","i","it","for","not","on",
  "with","he","as","you","do","at","this","but","his","by","from","they","we",
  "say","her","she","or","an","will","my","one","all","would","there","their",
  "what","so","up","out","if","about","who","get","which","go","me","when",
  "make","can","like","time","no","just","him","know","take","people","into",
  "year","your","good","some","could","them","see","other","than","then","now",
  "look","only","come","its","over","think","also","back","after","use","two",
  "how","our","work","first","well","way","even","new","want","because","any",
  "these","give","day","most","us","is","was","are","were","been","being","has",
  "had","does","did","done","yes","no","ok","okay","note","notes","watch","love",
  "hate","like","help","please","thanks","thank","you","yours","mine","ours",
  "today","tomorrow","yesterday","morning","night","work","play","read","write",
  "code","game","music","food","water","drink","sleep","wake","run","walk","talk",
  "phone","call","text","email","home","house","car","bike","train","plane",
  "fast","slow","big","small","tall","short","hot","cold","warm","cool","light",
  "dark","red","blue","green","yellow","black","white","orange","pink","purple",
  "buy","sell","pay","money","cash","card","bank","save","spend","cost","price",
  "free","open","close","stop","start","begin","end","next","last","year","week",
  "month","date","time","hour","minute","second","yes","no","maybe","sure","fine",
  "great","cool","nice","bad","sad","happy","angry","tired","busy","ready","done",
  "send","receive","share","keep","find","lost","look","watch","listen","speak",
  "learn","teach","study","class","school","book","page","line","word","letter",
  "number","name","city","town","street","river","sky","sun","moon","star","cloud",
  "fire","ice","tree","rock","road","map","place","world","life","mind","body",
  "head","hand","foot","eye","ear","mouth","nose","face","heart","brain","skin",
  "hair","tooth","family","friend","love","peace","truth","light","power","dream",
  "hope","fear","memory","story","song","color","shape","line","point","circle",
  "square","right","left","up","down","north","south","east","west","yes","no",
  "okay","hello","hi","hey","bye","goodbye","please","sorry","welcome","cheers",
  "morning","evening","night","day","week","month","year","second","minute","hour",
  "where","when","why","what","who","how","because","so","then","also","too",
  "only","just","very","really","quite","almost","always","never","sometimes",
  "often","again","once","twice","more","less","many","few","much","little",
  "every","each","both","all","none","any","some","other","another","same",
  "different","such","own","new","old","young","first","second","third","last",
  "next","best","worst","better","worse","good","bad","right","wrong","true",
  "false","real","fake","easy","hard","simple","complex","fun","boring","cool",
  "lame","strange","weird","normal","crazy","quick","slow","loud","quiet","clean",
  "dirty","empty","full","open","closed","safe","danger","ready","later","soon",
};
static const int DICT_N = sizeof(DICT)/sizeof(DICT[0]);

// Find top 3 words that start with the given prefix (case-insensitive).
static void suggest(const String &prefix, String out[3]){
  out[0]=""; out[1]=""; out[2]="";
  if(prefix.length()==0) return;
  String pl = prefix; pl.toLowerCase();
  int nfound=0;
  for(int i=0;i<DICT_N && nfound<3;i++){
    const char *w = DICT[i];
    // case-insensitive startsWith
    bool ok=true;
    for(int k=0;k<(int)pl.length();k++){
      char c = w[k]; if(!c){ ok=false; break; }
      if((c>='A'&&c<='Z')) c+=32;
      if(c != pl[k]){ ok=false; break; }
    }
    if(ok){ out[nfound] = String(w); nfound++; }
  }
}

// =====================================================================
//   EDITOR  (handwriting pad with gestures + smoothing + suggestions)
// =====================================================================
static lv_obj_t *g_ed_screen=nullptr;
static lv_obj_t *g_ed_text_lbl=nullptr;
static lv_obj_t *g_chip[3]={nullptr,nullptr,nullptr};
static lv_obj_t *g_chip_lbl[3]={nullptr,nullptr,nullptr};
static lv_obj_t *g_mode_btn=nullptr, *g_mode_lbl=nullptr;
static String g_text="";
static String g_filename="";    // current open file (empty = unsaved)
static char   g_last_committed_ch = 0;   // for undo
static lv_timer_t *g_commit_timer = nullptr;
static uint32_t g_press_ms=0;
static int g_press_x=0, g_press_y=0;
static int g_release_x=0, g_release_y=0;
static bool g_training_mode = false;   // declared early — used by on_pad() below, real init lives further down too

static const char* mode_name(){
  switch(g_mode){case M_UP:return "ABC"; case M_LO:return "abc"; case M_NUM:return "123"; case M_SYM:return "SYM";}
  return "?";
}

static void clear_pad(){
  if(!g_pad) return;
  lv_canvas_fill_bg(g_pad, lv_color_hex(0x080808), LV_OPA_COVER);
}

// Smoothed thick line: draws midpoint-anchored quadratic segments
static void draw_smooth_seg(int x0,int y0, int x1,int y1, int x2,int y2){
  if(!g_pad) return;
  lv_draw_line_dsc_t d; lv_draw_line_dsc_init(&d);
  d.color = lv_color_hex(COL_WHITE); d.width=6; d.round_start=1; d.round_end=1;
  // approximate quadratic curve with two short segments through the midpoints
  int mx1 = (x0+x1)/2, my1 = (y0+y1)/2;
  int mx2 = (x1+x2)/2, my2 = (y1+y2)/2;
  lv_point_t pp[3] = {{(lv_coord_t)mx1,(lv_coord_t)my1},
                      {(lv_coord_t)x1,(lv_coord_t)y1},
                      {(lv_coord_t)mx2,(lv_coord_t)my2}};
  lv_canvas_draw_line(g_pad, pp, 3, &d);
}
static void draw_straight_seg(int x0,int y0,int x1,int y1){
  if(!g_pad) return;
  lv_draw_line_dsc_t d; lv_draw_line_dsc_init(&d);
  d.color = lv_color_hex(COL_WHITE); d.width=6; d.round_start=1; d.round_end=1;
  lv_point_t pp[2] = {{(lv_coord_t)x0,(lv_coord_t)y0},{(lv_coord_t)x1,(lv_coord_t)y1}};
  lv_canvas_draw_line(g_pad, pp, 2, &d);
}

static void refresh_text_label(){
  if(!g_ed_text_lbl) return;
  String show = g_text;
  if(show.length()==0) show = "_";
  // show last ~3 lines
  lv_label_set_text(g_ed_text_lbl, show.c_str());
}

static void rebuild_chips();

static void editor_clear_strokes(){
  g_pn = 0; g_stroke_id = 0;
  clear_pad();
}

static void commit_letter_now(){
  if(g_pn < 4){ editor_clear_strokes(); return; }
  char alt=0;
  char ch = classify_letter(g_pts, g_pn, &alt);
  char picked = resolve_for_mode(ch, alt);
  if(picked && picked!='?'){
    g_text += picked;
    g_last_committed_ch = picked;
    refresh_text_label();
    rebuild_chips();
  }
  editor_clear_strokes();
}

static void commit_timer_cb(lv_timer_t*){
  if(g_commit_timer){ lv_timer_del(g_commit_timer); g_commit_timer=nullptr; }
  if(!g_drawing) commit_letter_now();
}
static void schedule_commit(){
  if(g_commit_timer){ lv_timer_del(g_commit_timer); }
  g_commit_timer = lv_timer_create(commit_timer_cb, 600, NULL);
  lv_timer_set_repeat_count(g_commit_timer, 1);
}

// Returns true if the release was a gesture (so don't classify).
// To avoid false-firing during letter writing, gestures must be:
//   - the FIRST stroke since the last commit (g_stroke_id == 0)
//   - VERY fast (< 200 ms — letter strokes are usually slower)
//   - LARGE (> 140 px in dominant axis — letters fit in ~120 px box)
//   - HIGHLY directional (dominant axis > 3x the other)
static bool detect_pad_gesture(){
  if(g_stroke_id != 0) return false;     // only first stroke can be a gesture
  uint32_t dur = millis() - g_press_ms;
  if(dur > 200) return false;
  int dx = g_release_x - g_press_x;
  int dy = g_release_y - g_press_y;
  int abx = abs(dx), aby = abs(dy);
  if(abx < 140 && aby < 140) return false;
  if(abx > aby*3){
    if(dx < 0){
      if(g_text.length()>0){ g_text = g_text.substring(0, g_text.length()-1); refresh_text_label(); rebuild_chips(); }
    } else {
      g_text += " "; refresh_text_label(); rebuild_chips();
    }
    return true;
  }
  if(aby > abx*3 && dy > 0){
    g_text += "\n"; refresh_text_label(); rebuild_chips();
    return true;
  }
  return false;
}

static void on_pad(lv_event_t *e){
  if(!g_pad) return;
  lv_event_code_t c = lv_event_get_code(e);
  lv_indev_t *in = lv_indev_get_act();
  if(!in) return;
  lv_point_t p; lv_indev_get_point(in, &p);
  lv_area_t a; lv_obj_get_coords(g_pad, &a);
  int lx = p.x - a.x1, ly = p.y - a.y1;

  if(c==LV_EVENT_PRESSED){
    g_drawing = true;
    g_press_ms = millis();
    g_press_x = lx; g_press_y = ly;
    if(g_commit_timer){ lv_timer_del(g_commit_timer); g_commit_timer=nullptr; }
    if(g_pn < MAXP){
      g_pts[g_pn].x = lx; g_pts[g_pn].y = ly; g_pts[g_pn].s = g_stroke_id;
      g_pn++;
    }
  }
  if(c==LV_EVENT_PRESSING && g_drawing){
    if(g_pn >= 2){
      Pt &prev = g_pts[g_pn-1];
      if(g_pn >= 3){
        Pt &prev2 = g_pts[g_pn-2];
        if(prev2.s == prev.s){
          draw_smooth_seg((int)prev2.x,(int)prev2.y, (int)prev.x,(int)prev.y, lx, ly);
        } else {
          draw_straight_seg((int)prev.x,(int)prev.y, lx, ly);
        }
      } else {
        draw_straight_seg((int)prev.x,(int)prev.y, lx, ly);
      }
    }
    if(g_pn < MAXP){
      g_pts[g_pn].x = lx; g_pts[g_pn].y = ly; g_pts[g_pn].s = g_stroke_id;
      g_pn++;
    }
  }
  if(c==LV_EVENT_RELEASED && g_drawing){
    g_drawing = false;
    g_release_x = lx; g_release_y = ly;
    if(g_training_mode){
      // training: do NOT classify, do NOT detect gestures.
      // Just bump the stroke counter and wait for SAVE & NEXT.
      g_stroke_id++;
      return;
    }
    // Try gesture first (short + linear movement)
    if(detect_pad_gesture()){
      editor_clear_strokes();
      return;
    }
    g_stroke_id++;
    schedule_commit();
  }
}

// ---- Chip handler -------------------------------------------------
static void on_chip(lv_event_t *e){
  int idx = (int)(intptr_t)lv_event_get_user_data(e);
  if(idx<0||idx>2) return;
  const char* w = lv_label_get_text(g_chip_lbl[idx]);
  if(!w || w[0]==0 || w[0]==' ') return;
  // figure out current word prefix and replace
  int n = g_text.length();
  int wstart = n;
  while(wstart>0){
    char c = g_text[wstart-1];
    if(c==' '||c=='\n') break;
    wstart--;
  }
  String head = g_text.substring(0,wstart);
  g_text = head + String(w) + " ";
  refresh_text_label();
  rebuild_chips();
}

static void rebuild_chips(){
  if(!g_chip[0]) return;
  // get current trailing word
  int n = g_text.length();
  int wstart = n;
  while(wstart>0){
    char c = g_text[wstart-1];
    if(c==' '||c=='\n') break;
    wstart--;
  }
  String pref = g_text.substring(wstart);
  String s[3]; suggest(pref, s);
  for(int i=0;i<3;i++){
    if(s[i].length()==0){
      lv_label_set_text(g_chip_lbl[i], " ");
      lv_obj_set_style_opa(g_chip[i], LV_OPA_30, 0);
    } else {
      lv_label_set_text(g_chip_lbl[i], s[i].c_str());
      lv_obj_set_style_opa(g_chip[i], LV_OPA_COVER, 0);
    }
  }
}

// =====================================================================
//   SAVE / OPEN / DELETE
// =====================================================================
static String make_default_name(){
  // pick first available "note_NN.txt"
  for(int i=1;i<999;i++){
    char b[24]; snprintf(b,24,"note_%02d.txt",i);
    String path = String("/WATCH/notes/") + b;
    if(!SD_MMC.exists(path)) return String(b);
  }
  return "note.txt";
}
static bool save_note(const String &name, const String &body){
  String path = String("/WATCH/notes/") + name;
  File f = SD_MMC.open(path.c_str(), FILE_WRITE);
  if(!f) return false;
  f.print(body); f.close();
  return true;
}
static String load_note(const String &name){
  String path = String("/WATCH/notes/") + name;
  File f = SD_MMC.open(path.c_str(), FILE_READ);
  if(!f) return "";
  String out; while(f.available()) out += (char)f.read();
  f.close(); return out;
}
static bool delete_note(const String &name){
  String path = String("/WATCH/notes/") + name;
  return SD_MMC.remove(path.c_str());
}

// =====================================================================
//   LIBRARY SCREEN (boot lands here)
// =====================================================================
static lv_obj_t *g_lib_screen=nullptr;
static void build_library_screen();
static void build_editor_screen(const String &filename, const String &initial);
static void build_reader_screen(const String &filename);
static void build_training_screen();
static int  count_trained();   // forward (used inside build_library_screen below)

static void open_library(){
  if(g_lib_screen){ lv_obj_del_async(g_lib_screen); g_lib_screen=nullptr; }
  if(g_ed_screen){ lv_obj_del_async(g_ed_screen); g_ed_screen=nullptr; }
  if(g_pad_buf){ free(g_pad_buf); g_pad_buf=nullptr; g_pad=nullptr; g_pad_frame=nullptr; }
  build_library_screen();
}

static void make_note_row(lv_obj_t *parent, const String &name){
  lv_obj_t *card = lv_obj_create(parent);
  lv_obj_remove_style_all(card);
  lv_obj_set_size(card, LV_PCT(100), 70);
  lv_obj_set_style_bg_color(card, lv_color_hex(COL_PANEL),0);
  lv_obj_set_style_bg_opa(card, LV_OPA_COVER,0);
  lv_obj_set_style_radius(card, 16,0);
  lv_obj_set_style_border_color(card, lv_color_hex(0x1C1C1C),0);
  lv_obj_set_style_border_width(card,1,0);
  lv_obj_set_style_pad_all(card, 12, 0);
  lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

  lv_obj_t *icn = lv_obj_create(card); lv_obj_remove_style_all(icn);
  lv_obj_set_size(icn, 36, 36); lv_obj_align(icn, LV_ALIGN_LEFT_MID, 4, 0);
  lv_obj_set_style_radius(icn,18,0);
  lv_obj_set_style_bg_color(icn, lv_color_hex(ACCENT_HL),0);
  lv_obj_set_style_bg_opa(icn,40,0);
  lv_obj_set_style_border_color(icn, lv_color_hex(ACCENT_HL),0);
  lv_obj_set_style_border_width(icn,1,0);
  lv_obj_t *icl=lv_label_create(icn); lv_label_set_text(icl, LV_SYMBOL_FILE);
  lv_obj_set_style_text_color(icl, lv_color_hex(ACCENT_HL),0);
  lv_obj_set_style_text_font(icl, &lv_font_montserrat_18,0); lv_obj_center(icl);

  lv_obj_t *nm = lv_label_create(card); lv_label_set_text(nm, name.c_str());
  lv_obj_set_style_text_color(nm, lv_color_hex(COL_WHITE),0);
  lv_obj_set_style_text_font(nm, &lv_font_montserrat_16,0);
  lv_obj_align(nm, LV_ALIGN_LEFT_MID, 50, -8);

  // preview snippet
  String snippet = load_note(name); snippet.replace('\n',' ');
  if(snippet.length()>30) snippet = snippet.substring(0,30) + "...";
  lv_obj_t *pv = lv_label_create(card); lv_label_set_text(pv, snippet.c_str());
  lv_obj_set_style_text_color(pv, lv_color_hex(COL_TEXT_DIM),0);
  lv_obj_set_style_text_font(pv, &lv_font_montserrat_12,0);
  lv_obj_align(pv, LV_ALIGN_LEFT_MID, 50, 14);

  // OPEN button (read)
  static const char *_lbl_open  = "READ";
  static const char *_lbl_edit  = "EDIT";
  static const char *_lbl_del   = LV_SYMBOL_TRASH;
  auto mkbtn = [&](int x, const char* lbl, uint32_t accent, lv_event_cb_t cb){
    lv_obj_t *b=lv_btn_create(card); lv_obj_set_size(b, 50, 32);
    lv_obj_align(b, LV_ALIGN_RIGHT_MID, x, 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(0x101010),0);
    lv_obj_set_style_border_color(b, lv_color_hex(accent),0);
    lv_obj_set_style_border_width(b,1,0);
    lv_obj_set_style_radius(b,16,0);
    lv_obj_t *l=lv_label_create(b); lv_label_set_text(l, lbl);
    lv_obj_set_style_text_color(l, lv_color_hex(accent),0);
    lv_obj_set_style_text_font(l, &lv_font_montserrat_12,0); lv_obj_center(l);
    // user_data = a *new* String copy on heap
    String *sname = new String(name);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, sname);
  };

  mkbtn(-114, _lbl_open, COL_GREEN, [](lv_event_t *e){
    String *nm = (String*)lv_event_get_user_data(e);
    build_reader_screen(*nm);
  });
  mkbtn(-60,  _lbl_edit, COL_CYAN,  [](lv_event_t *e){
    String *nm = (String*)lv_event_get_user_data(e);
    String body = load_note(*nm);
    build_editor_screen(*nm, body);
  });
  mkbtn(-6,   _lbl_del,  COL_RED,   [](lv_event_t *e){
    String *nm = (String*)lv_event_get_user_data(e);
    delete_note(*nm);
    open_library();
  });
}

static void build_library_screen(){
  lv_obj_t *s = lv_obj_create(NULL); g_lib_screen = s;
  lv_obj_set_style_bg_color(s, lv_color_hex(COL_BG),0);
  lv_obj_set_style_bg_opa(s, LV_OPA_COVER,0);
  lv_obj_clear_flag(s, LV_OBJ_FLAG_SCROLLABLE);
  make_wallpaper_bg(s);
  add_header_strip(s, "NOTES", ACCENT_HL);

  // New-note button (top-right under battery)
  lv_obj_t *nb = lv_btn_create(s); lv_obj_set_size(nb, 110, 36);
  lv_obj_align(nb, LV_ALIGN_TOP_RIGHT, -16, 62);
  lv_obj_set_style_radius(nb,18,0);
  lv_obj_set_style_bg_color(nb, lv_color_hex(0x101010),0);
  lv_obj_set_style_border_color(nb, lv_color_hex(COL_GREEN),0);
  lv_obj_set_style_border_width(nb,1,0);
  lv_obj_t *nl = lv_label_create(nb); lv_label_set_text(nl, LV_SYMBOL_PLUS "  NEW");
  lv_obj_set_style_text_color(nl, lv_color_hex(COL_GREEN),0);
  lv_obj_set_style_text_font(nl, &lv_font_montserrat_14,0); lv_obj_center(nl);
  lv_obj_add_event_cb(nb, [](lv_event_t*){
    build_editor_screen(make_default_name(), "");
  }, LV_EVENT_CLICKED, NULL);

  // TRAIN-handwriting button (left of NEW)
  lv_obj_t *tb = lv_btn_create(s); lv_obj_set_size(tb, 110, 36);
  lv_obj_align(tb, LV_ALIGN_TOP_LEFT, 16, 62);
  lv_obj_set_style_radius(tb,18,0);
  lv_obj_set_style_bg_color(tb, lv_color_hex(0x101010),0);
  lv_obj_set_style_border_color(tb, lv_color_hex(COL_AMBER),0);
  lv_obj_set_style_border_width(tb,1,0);
  lv_obj_t *tl = lv_label_create(tb);
  // show "TRAIN" if no user templates yet, otherwise "RETRAIN  ✱"
  char tbuf[28];
  int trained = count_trained();
  if(trained == 0) snprintf(tbuf, 28, LV_SYMBOL_EDIT "  TRAIN");
  else             snprintf(tbuf, 28, LV_SYMBOL_EDIT "  TRAIN (%d)", trained);
  lv_label_set_text(tl, tbuf);
  lv_obj_set_style_text_color(tl, lv_color_hex(COL_AMBER),0);
  lv_obj_set_style_text_font(tl, &lv_font_montserrat_12,0); lv_obj_center(tl);
  lv_obj_add_event_cb(tb, [](lv_event_t*){
    build_training_screen();
  }, LV_EVENT_CLICKED, NULL);

  // list container
  lv_obj_t *list = lv_obj_create(s);
  lv_obj_remove_style_all(list);
  lv_obj_set_size(list, LCD_W - 30, LCD_H - 150);
  lv_obj_align(list, LV_ALIGN_TOP_MID, 0, 110);
  lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_row(list, 10, 0);
  lv_obj_set_style_pad_hor(list, 6, 0);
  lv_obj_set_style_pad_top(list, 4, 0);
  lv_obj_set_style_pad_bottom(list, 60, 0);
  lv_obj_set_scrollbar_mode(list, LV_SCROLLBAR_MODE_AUTO);
  lv_obj_set_scroll_dir(list, LV_DIR_VER);
  lv_obj_set_style_bg_color(list, lv_color_hex(ACCENT_HL), LV_PART_SCROLLBAR);
  lv_obj_set_style_bg_opa(list, 120, LV_PART_SCROLLBAR);
  lv_obj_set_style_width(list, 3, LV_PART_SCROLLBAR);

  // enumerate notes
  bool any=false;
  File dir = SD_MMC.open("/WATCH/notes");
  if(dir){
    File f = dir.openNextFile();
    while(f){
      String nm = f.name();
      // strip leading path
      int sl = nm.lastIndexOf('/'); if(sl>=0) nm = nm.substring(sl+1);
      String lc = nm; lc.toLowerCase();
      if(!f.isDirectory() && lc.endsWith(".txt")){
        make_note_row(list, nm); any = true;
      }
      f = dir.openNextFile();
    }
  }
  if(!any){
    lv_obj_t *empty = lv_label_create(list);
    lv_label_set_text(empty, "no notes yet\n\ntap  + NEW  to start scribbling");
    lv_obj_set_style_text_color(empty, lv_color_hex(COL_TEXT_DIM),0);
    lv_obj_set_style_text_font(empty, &lv_font_montserrat_14,0);
    lv_obj_set_style_text_align(empty, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(empty, LV_PCT(100));
  }

  add_boot_hint(s);
  lv_scr_load_anim(s, LV_SCR_LOAD_ANIM_FADE_ON, 220, 0, true);
}

// =====================================================================
//   READER SCREEN (read-only)
// =====================================================================
static void build_reader_screen(const String &filename){
  String body = load_note(filename);
  lv_obj_t *s = lv_obj_create(NULL);
  lv_obj_set_style_bg_color(s, lv_color_hex(COL_BG),0);
  lv_obj_set_style_bg_opa(s, LV_OPA_COVER,0);
  lv_obj_clear_flag(s, LV_OBJ_FLAG_SCROLLABLE);
  add_header_strip(s, "READ", ACCENT_HL);
  // filename
  lv_obj_t *fn = lv_label_create(s); lv_label_set_text(fn, filename.c_str());
  lv_obj_set_style_text_color(fn, lv_color_hex(COL_TEXT_DIM),0);
  lv_obj_set_style_text_font(fn, &lv_font_montserrat_12,0);
  lv_obj_align(fn, LV_ALIGN_TOP_LEFT, 24, 56);

  lv_obj_t *box = lv_obj_create(s);
  lv_obj_set_size(box, LCD_W-30, LCD_H-180);
  lv_obj_align(box, LV_ALIGN_TOP_MID, 0, 90);
  lv_obj_set_style_bg_color(box, lv_color_hex(0x070707),0);
  lv_obj_set_style_border_color(box, lv_color_hex(0x202020),0);
  lv_obj_set_style_border_width(box,1,0);
  lv_obj_set_style_radius(box,12,0);
  lv_obj_set_style_pad_all(box,12,0);
  lv_obj_set_scroll_dir(box, LV_DIR_VER);

  lv_obj_t *txt = lv_label_create(box);
  lv_label_set_long_mode(txt, LV_LABEL_LONG_WRAP);
  lv_obj_set_width(txt, LCD_W-60);
  lv_label_set_text(txt, body.length()?body.c_str():"(empty)");
  lv_obj_set_style_text_color(txt, lv_color_hex(COL_WHITE),0);
  lv_obj_set_style_text_font(txt, &lv_font_montserrat_18,0);

  // Edit + Back
  auto mkbtn = [&](lv_align_t al, int x, const char* lbl, uint32_t accent, lv_event_cb_t cb, void* ud){
    lv_obj_t *b=lv_btn_create(s); lv_obj_set_size(b, 120, 40);
    lv_obj_align(b, al, x, -22);
    lv_obj_set_style_bg_color(b, lv_color_hex(0x101010),0);
    lv_obj_set_style_border_color(b, lv_color_hex(accent),0);
    lv_obj_set_style_border_width(b,1,0);
    lv_obj_set_style_radius(b,20,0);
    lv_obj_t *l=lv_label_create(b); lv_label_set_text(l, lbl);
    lv_obj_set_style_text_color(l, lv_color_hex(accent),0);
    lv_obj_set_style_text_font(l, &lv_font_montserrat_14,0); lv_obj_center(l);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, ud);
  };
  String *snm = new String(filename);
  mkbtn(LV_ALIGN_BOTTOM_LEFT, 20, LV_SYMBOL_LEFT "  BACK", ACCENT_HL,
        [](lv_event_t *e){ String *n = (String*)lv_event_get_user_data(e); delete n; open_library(); },
        snm);
  String *snm2 = new String(filename);
  mkbtn(LV_ALIGN_BOTTOM_RIGHT, -20, "EDIT", COL_CYAN,
        [](lv_event_t *e){
          String *n = (String*)lv_event_get_user_data(e);
          String body = load_note(*n);
          build_editor_screen(*n, body);
        }, snm2);
  lv_scr_load_anim(s, LV_SCR_LOAD_ANIM_FADE_ON, 220, 0, true);
}

// =====================================================================
//   EDITOR SCREEN
// =====================================================================
static void cycle_mode(){
  g_mode = (Mode)(((int)g_mode + 1) & 3);
  if(g_mode_lbl) lv_label_set_text(g_mode_lbl, mode_name());
}

static void build_editor_screen(const String &filename, const String &initial){
  g_filename = filename;
  g_text = initial;
  g_last_committed_ch = 0;
  g_pn = 0; g_stroke_id = 0; g_drawing = false;
  if(g_commit_timer){ lv_timer_del(g_commit_timer); g_commit_timer=nullptr; }

  lv_obj_t *s = lv_obj_create(NULL); g_ed_screen = s;
  lv_obj_set_style_bg_color(s, lv_color_hex(COL_BG),0);
  lv_obj_set_style_bg_opa(s, LV_OPA_COVER,0);
  lv_obj_clear_flag(s, LV_OBJ_FLAG_SCROLLABLE);

  // header (no large NOTE word, just filename + battery)
  lv_obj_t *fn = lv_label_create(s); lv_label_set_text(fn, filename.c_str());
  lv_obj_set_style_text_color(fn, lv_color_hex(ACCENT_HL),0);
  lv_obj_set_style_text_font(fn, &lv_font_montserrat_18,0);
  lv_obj_set_style_text_letter_space(fn,2,0);
  lv_obj_align(fn, LV_ALIGN_TOP_LEFT, 20, 16);
  g_batt_lbl = lv_label_create(s); lv_label_set_text(g_batt_lbl, "--%");
  lv_obj_set_style_text_color(g_batt_lbl, lv_color_hex(COL_TEXT_DIM),0);
  lv_obj_set_style_text_font(g_batt_lbl, &lv_font_montserrat_12,0);
  lv_obj_align(g_batt_lbl, LV_ALIGN_TOP_RIGHT, -16, 18);
  lv_timer_create(update_batt_pill, 2000, NULL); update_batt_pill(nullptr);

  // back-to-library tiny pill
  lv_obj_t *bk = lv_btn_create(s); lv_obj_set_size(bk, 70, 28);
  lv_obj_align(bk, LV_ALIGN_TOP_RIGHT, -16, 42);
  lv_obj_set_style_radius(bk,14,0);
  lv_obj_set_style_bg_color(bk, lv_color_hex(0x101010),0);
  lv_obj_set_style_border_color(bk, lv_color_hex(ACCENT_B),0);
  lv_obj_set_style_border_width(bk,1,0);
  lv_obj_t *bkl = lv_label_create(bk); lv_label_set_text(bkl, LV_SYMBOL_LEFT "  Lib");
  lv_obj_set_style_text_color(bkl, lv_color_hex(ACCENT_B),0);
  lv_obj_set_style_text_font(bkl, &lv_font_montserrat_12,0); lv_obj_center(bkl);
  lv_obj_add_event_cb(bk, [](lv_event_t*){
    save_note(g_filename, g_text); open_library();
  }, LV_EVENT_CLICKED, NULL);

  // text strip
  lv_obj_t *txt_box = lv_obj_create(s);
  lv_obj_set_size(txt_box, LCD_W-30, 70);
  lv_obj_align(txt_box, LV_ALIGN_TOP_MID, 0, 56);
  lv_obj_set_style_bg_color(txt_box, lv_color_hex(0x070707),0);
  lv_obj_set_style_border_color(txt_box, lv_color_hex(0x202020),0);
  lv_obj_set_style_border_width(txt_box,1,0);
  lv_obj_set_style_radius(txt_box,10,0);
  lv_obj_set_style_pad_all(txt_box, 8, 0);
  lv_obj_set_scroll_dir(txt_box, LV_DIR_VER);
  g_ed_text_lbl = lv_label_create(txt_box);
  lv_label_set_long_mode(g_ed_text_lbl, LV_LABEL_LONG_WRAP);
  lv_obj_set_width(g_ed_text_lbl, LCD_W-50);
  lv_obj_set_style_text_color(g_ed_text_lbl, lv_color_hex(COL_WHITE),0);
  lv_obj_set_style_text_font(g_ed_text_lbl, &lv_font_montserrat_18,0);
  refresh_text_label();

  // suggestion chips row
  lv_obj_t *chrow = lv_obj_create(s); lv_obj_remove_style_all(chrow);
  lv_obj_set_size(chrow, LCD_W-30, 32);
  lv_obj_align(chrow, LV_ALIGN_TOP_MID, 0, 132);
  lv_obj_clear_flag(chrow, LV_OBJ_FLAG_SCROLLABLE);
  for(int i=0;i<3;i++){
    lv_obj_t *c = lv_btn_create(chrow);
    lv_obj_set_size(c, (LCD_W-50)/3 - 6, 30);
    lv_obj_align(c, LV_ALIGN_LEFT_MID, i*((LCD_W-50)/3), 0);
    lv_obj_set_style_radius(c, 15, 0);
    lv_obj_set_style_bg_color(c, lv_color_hex(0x141414),0);
    lv_obj_set_style_border_color(c, lv_color_hex(ACCENT_HL),0);
    lv_obj_set_style_border_width(c,1,0);
    lv_obj_t *cl = lv_label_create(c); lv_label_set_text(cl, " ");
    lv_obj_set_style_text_color(cl, lv_color_hex(COL_WHITE),0);
    lv_obj_set_style_text_font(cl, &lv_font_montserrat_14,0); lv_obj_center(cl);
    lv_obj_add_event_cb(c, on_chip, LV_EVENT_CLICKED, (void*)(intptr_t)i);
    g_chip[i] = c; g_chip_lbl[i] = cl;
  }

  // pad frame + canvas
  lv_obj_t *pframe = lv_obj_create(s); lv_obj_remove_style_all(pframe);
  g_pad_frame = pframe;
  lv_obj_set_size(pframe, LCD_W-30, 200);
  lv_obj_align(pframe, LV_ALIGN_TOP_MID, 0, 174);
  lv_obj_set_style_border_color(pframe, lv_color_hex(ACCENT_HL),0);
  lv_obj_set_style_border_width(pframe, 2, 0);
  lv_obj_set_style_radius(pframe, 16, 0);
  lv_obj_set_style_bg_color(pframe, lv_color_hex(0x050505),0);
  lv_obj_set_style_bg_opa(pframe, LV_OPA_COVER, 0);

  int pw = LCD_W-50, ph = 184;
  g_pad_w = pw; g_pad_h = ph;
  if(g_pad_buf){ free(g_pad_buf); g_pad_buf=nullptr; }
  g_pad_buf = (lv_color_t*)heap_caps_malloc(pw*ph*sizeof(lv_color_t), MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
  if(!g_pad_buf) g_pad_buf = (lv_color_t*)heap_caps_malloc(pw*ph*sizeof(lv_color_t), MALLOC_CAP_8BIT);
  g_pad = lv_canvas_create(pframe);
  lv_canvas_set_buffer(g_pad, g_pad_buf, pw, ph, LV_IMG_CF_TRUE_COLOR);
  lv_obj_center(g_pad);
  clear_pad();
  lv_obj_add_flag(g_pad, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(g_pad, on_pad, LV_EVENT_ALL, NULL);

  // bottom toolbar:  MODE  |  UNDO  |  ↵ NL  |  SAVE
  auto toolbtn = [&](int x, int w, const char* txt, uint32_t accent, lv_event_cb_t cb)->lv_obj_t*{
    lv_obj_t *b=lv_btn_create(s); lv_obj_set_size(b, w, 44);
    lv_obj_align(b, LV_ALIGN_BOTTOM_LEFT, x, -16);
    lv_obj_set_style_radius(b, 22, 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(0x101010),0);
    lv_obj_set_style_border_color(b, lv_color_hex(accent),0);
    lv_obj_set_style_border_width(b,1,0);
    lv_obj_t *l=lv_label_create(b); lv_label_set_text(l, txt);
    lv_obj_set_style_text_color(l, lv_color_hex(accent),0);
    lv_obj_set_style_text_font(l, &lv_font_montserrat_14,0);
    lv_obj_set_style_text_letter_space(l,1,0);
    lv_obj_center(l);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, NULL);
    return l;
  };
  g_mode_lbl = toolbtn(14, 80, mode_name(), COL_AMBER,
                      [](lv_event_t*){ cycle_mode(); });
  toolbtn(102, 80, LV_SYMBOL_CLOSE "  UND", COL_RED, [](lv_event_t*){
    if(g_text.length()>0){
      g_text = g_text.substring(0, g_text.length()-1);
      refresh_text_label(); rebuild_chips();
    }
  });
  toolbtn(190, 80, LV_SYMBOL_NEW_LINE, COL_GREEN, [](lv_event_t*){
    g_text += "\n"; refresh_text_label(); rebuild_chips();
  });
  toolbtn(278, 110, LV_SYMBOL_OK "  SAVE", ACCENT_HL, [](lv_event_t*){
    save_note(g_filename, g_text); open_library();
  });

  rebuild_chips();
  lv_scr_load_anim(s, LV_SCR_LOAD_ANIM_FADE_ON, 220, 0, true);
}

// =====================================================================
//   HANDWRITING TRAINING WIZARD
//   Walks the user through every character in TRAIN_CHARS. They draw
//   the prompted glyph; the captured strokes are resampled/normalised
//   into a UserTpl and pushed into g_user_tpls. At the end we write
//   /WATCH/notes/_handwriting.dat. From then on the classifier prefers
//   the user's shapes (15% distance bonus).
// =====================================================================
// g_training_mode declared earlier (near editor globals) so on_pad() can see it
static lv_obj_t *g_train_screen  = nullptr;
static lv_obj_t *g_train_target_lbl = nullptr;
static lv_obj_t *g_train_progress_lbl = nullptr;
static lv_obj_t *g_train_count_lbl = nullptr;
static int       g_train_idx      = 0;
// While in training mode we DON'T commit/classify on stroke release;
// we wait for the user to hit "SAVE & NEXT".
static lv_timer_t *g_train_silence_timer = nullptr;

static void train_clear_strokes(){
  g_pn = 0; g_stroke_id = 0;
  clear_pad();
}

// Capture the current g_pts buffer as a UserTpl for character `ch`.
// Steps: 1) compute bounding box  2) normalise to 0..100 grid
//        3) resample to MAX_USER_PTS_PER_TPL points
static void capture_user_template(char ch){
  if(g_pn < 4) return;
  // normalise to 0..100
  float xmin=1e9,xmax=-1e9,ymin=1e9,ymax=-1e9;
  for(int i=0;i<g_pn;i++){
    if(g_pts[i].x<xmin)xmin=g_pts[i].x; if(g_pts[i].x>xmax)xmax=g_pts[i].x;
    if(g_pts[i].y<ymin)ymin=g_pts[i].y; if(g_pts[i].y>ymax)ymax=g_pts[i].y;
  }
  float w = xmax-xmin, h = ymax-ymin, sc = (w>h?w:h); if(sc<1) sc=1;
  Pt norm[MAXP];
  for(int i=0;i<g_pn;i++){
    norm[i].x = ((g_pts[i].x-xmin)/sc) * 100.0f;
    norm[i].y = ((g_pts[i].y-ymin)/sc) * 100.0f;
    norm[i].s = g_pts[i].s;
  }
  // resample down to MAX_USER_PTS_PER_TPL
  Pt resd[MAX_USER_PTS_PER_TPL];
  int target = MAX_USER_PTS_PER_TPL;
  // simple uniform stride sample
  if(g_pn <= target){
    for(int i=0;i<g_pn;i++) resd[i] = norm[i];
    target = g_pn;
  } else {
    float stride = (float)g_pn / target;
    for(int i=0;i<target;i++){
      int idx = (int)(i*stride);
      if(idx>=g_pn) idx = g_pn-1;
      resd[i] = norm[idx];
    }
  }
  // remove any previous template for this char so re-training overwrites
  for(auto it=g_user_tpls.begin(); it!=g_user_tpls.end(); ){
    if(it->ch == ch) it = g_user_tpls.erase(it); else ++it;
  }
  UserTpl u; memset(&u,0,sizeof(u));
  u.ch = ch; u.npts = (uint8_t)target;
  for(int i=0;i<target;i++){
    int x = (int)resd[i].x; if(x<0)x=0; if(x>120)x=120;
    int y = (int)resd[i].y; if(y<0)y=0; if(y>120)y=120;
    int sg = resd[i].s; if(sg<0)sg=0; if(sg>7)sg=7;
    u.pts[i*3+0] = (uint8_t)x;
    u.pts[i*3+1] = (uint8_t)y;
    u.pts[i*3+2] = (uint8_t)sg;
  }
  g_user_tpls.push_back(u);
}

static void open_library();   // forward
static void build_training_screen();

static int count_trained(){
  // count unique chars stored
  int seen[128] = {0}; int n=0;
  for(auto &u : g_user_tpls){
    unsigned idx = (unsigned)u.ch & 0x7f;
    if(!seen[idx]){ seen[idx]=1; n++; }
  }
  return n;
}

static void train_update_header(){
  if(!g_train_target_lbl || !g_train_progress_lbl || !g_train_count_lbl) return;
  char glyph[2] = { TRAIN_CHARS[g_train_idx], 0 };
  lv_label_set_text(g_train_target_lbl, glyph);
  char p[24]; snprintf(p, 24, "%d / %d", g_train_idx+1, TRAIN_TOTAL);
  lv_label_set_text(g_train_progress_lbl, p);
  char c[40]; snprintf(c, 40, "%d letters customised", count_trained());
  lv_label_set_text(g_train_count_lbl, c);
}

static void train_next(){
  g_train_idx++;
  if(g_train_idx >= TRAIN_TOTAL){
    // done — persist and exit
    save_user_templates();
    g_training_mode = false;
    open_library();
    return;
  }
  train_update_header();
  train_clear_strokes();
}

static void train_save_and_next(){
  if(g_pn > 0){
    capture_user_template(TRAIN_CHARS[g_train_idx]);
  }
  // persist after every letter so a power loss mid-training doesn't lose work
  save_user_templates();
  train_next();
}

static void build_training_screen(){
  g_training_mode = true;
  g_train_idx = 0;
  if(g_lib_screen){ lv_obj_del_async(g_lib_screen); g_lib_screen=nullptr; }
  if(g_ed_screen){ lv_obj_del_async(g_ed_screen); g_ed_screen=nullptr; }
  if(g_pad_buf){ free(g_pad_buf); g_pad_buf=nullptr; g_pad=nullptr; }

  lv_obj_t *s = lv_obj_create(NULL); g_train_screen = s;
  lv_obj_set_style_bg_color(s, lv_color_hex(COL_BG),0);
  lv_obj_set_style_bg_opa(s, LV_OPA_COVER,0);
  lv_obj_clear_flag(s, LV_OBJ_FLAG_SCROLLABLE);

  // tiny header
  lv_obj_t *ttl = lv_label_create(s); lv_label_set_text(ttl, "TRAIN HANDWRITING");
  lv_obj_set_style_text_color(ttl, lv_color_hex(COL_AMBER),0);
  lv_obj_set_style_text_font(ttl, &lv_font_montserrat_14,0);
  lv_obj_set_style_text_letter_space(ttl, 3, 0);
  lv_obj_align(ttl, LV_ALIGN_TOP_LEFT, 20, 14);

  // back-out
  lv_obj_t *bk = lv_btn_create(s); lv_obj_set_size(bk, 70, 28);
  lv_obj_align(bk, LV_ALIGN_TOP_RIGHT, -16, 12);
  lv_obj_set_style_radius(bk,14,0);
  lv_obj_set_style_bg_color(bk, lv_color_hex(0x101010),0);
  lv_obj_set_style_border_color(bk, lv_color_hex(ACCENT_B),0);
  lv_obj_set_style_border_width(bk,1,0);
  lv_obj_t *bkl=lv_label_create(bk); lv_label_set_text(bkl, LV_SYMBOL_LEFT "  Lib");
  lv_obj_set_style_text_color(bkl, lv_color_hex(ACCENT_B),0);
  lv_obj_set_style_text_font(bkl, &lv_font_montserrat_12,0); lv_obj_center(bkl);
  lv_obj_add_event_cb(bk, [](lv_event_t*){
    save_user_templates();   // keep whatever they've done so far
    g_training_mode = false;
    open_library();
  }, LV_EVENT_CLICKED, NULL);

  // progress + counter
  g_train_progress_lbl = lv_label_create(s);
  lv_label_set_text(g_train_progress_lbl, "1 / 73");
  lv_obj_set_style_text_color(g_train_progress_lbl, lv_color_hex(COL_WHITE),0);
  lv_obj_set_style_text_font(g_train_progress_lbl, &lv_font_montserrat_18,0);
  lv_obj_align(g_train_progress_lbl, LV_ALIGN_TOP_LEFT, 20, 44);

  g_train_count_lbl = lv_label_create(s);
  lv_label_set_text(g_train_count_lbl, "");
  lv_obj_set_style_text_color(g_train_count_lbl, lv_color_hex(COL_TEXT_DIM),0);
  lv_obj_set_style_text_font(g_train_count_lbl, &lv_font_montserrat_12,0);
  lv_obj_align(g_train_count_lbl, LV_ALIGN_TOP_LEFT, 20, 70);

  // BIG target glyph (the prompt)
  lv_obj_t *tgt_box = lv_obj_create(s); lv_obj_remove_style_all(tgt_box);
  lv_obj_set_size(tgt_box, 110, 110);
  lv_obj_align(tgt_box, LV_ALIGN_TOP_RIGHT, -24, 50);
  lv_obj_set_style_radius(tgt_box, 18, 0);
  lv_obj_set_style_bg_color(tgt_box, lv_color_hex(0x141414),0);
  lv_obj_set_style_bg_opa(tgt_box, LV_OPA_COVER,0);
  lv_obj_set_style_border_color(tgt_box, lv_color_hex(COL_AMBER),0);
  lv_obj_set_style_border_width(tgt_box, 2, 0);
  g_train_target_lbl = lv_label_create(tgt_box);
  lv_label_set_text(g_train_target_lbl, "A");
  lv_obj_set_style_text_color(g_train_target_lbl, lv_color_hex(COL_AMBER),0);
  lv_obj_set_style_text_font(g_train_target_lbl, &lv_font_montserrat_48,0);
  lv_obj_center(g_train_target_lbl);

  // pad frame + canvas
  lv_obj_t *pframe = lv_obj_create(s); lv_obj_remove_style_all(pframe);
  g_pad_frame = pframe;
  lv_obj_set_size(pframe, LCD_W-30, 220);
  lv_obj_align(pframe, LV_ALIGN_TOP_MID, 0, 170);
  lv_obj_set_style_border_color(pframe, lv_color_hex(COL_AMBER),0);
  lv_obj_set_style_border_width(pframe, 2, 0);
  lv_obj_set_style_radius(pframe, 16, 0);
  lv_obj_set_style_bg_color(pframe, lv_color_hex(0x050505),0);
  lv_obj_set_style_bg_opa(pframe, LV_OPA_COVER, 0);

  int pw = LCD_W-50, ph = 204;
  g_pad_w = pw; g_pad_h = ph;
  g_pad_buf = (lv_color_t*)heap_caps_malloc(pw*ph*sizeof(lv_color_t), MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
  if(!g_pad_buf) g_pad_buf = (lv_color_t*)heap_caps_malloc(pw*ph*sizeof(lv_color_t), MALLOC_CAP_8BIT);
  g_pad = lv_canvas_create(pframe);
  lv_canvas_set_buffer(g_pad, g_pad_buf, pw, ph, LV_IMG_CF_TRUE_COLOR);
  lv_obj_center(g_pad);
  clear_pad();
  lv_obj_add_flag(g_pad, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(g_pad, on_pad, LV_EVENT_ALL, NULL);

  // hint
  lv_obj_t *hint = lv_label_create(s);
  lv_label_set_text(hint, "draw the highlighted letter the way YOU naturally write it");
  lv_obj_set_style_text_color(hint, lv_color_hex(COL_TEXT_DIM),0);
  lv_obj_set_style_text_font(hint, &lv_font_montserrat_12,0);
  lv_obj_set_style_text_align(hint, LV_TEXT_ALIGN_CENTER,0);
  lv_obj_set_width(hint, LCD_W-40);
  lv_obj_align(hint, LV_ALIGN_TOP_MID, 0, 398);

  // bottom toolbar: RETRY | SKIP | SAVE & NEXT
  auto btn = [&](int x, int w, const char* txt, uint32_t accent, lv_event_cb_t cb){
    lv_obj_t *b = lv_btn_create(s); lv_obj_set_size(b, w, 44);
    lv_obj_align(b, LV_ALIGN_BOTTOM_LEFT, x, -16);
    lv_obj_set_style_radius(b, 22, 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(0x101010),0);
    lv_obj_set_style_border_color(b, lv_color_hex(accent),0);
    lv_obj_set_style_border_width(b,1,0);
    lv_obj_t *l=lv_label_create(b); lv_label_set_text(l, txt);
    lv_obj_set_style_text_color(l, lv_color_hex(accent),0);
    lv_obj_set_style_text_font(l, &lv_font_montserrat_14,0); lv_obj_center(l);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, NULL);
  };
  btn( 14,  90, LV_SYMBOL_REFRESH "  RETRY", COL_RED, [](lv_event_t*){
    train_clear_strokes();
  });
  btn(110,  90, LV_SYMBOL_RIGHT "  SKIP", ACCENT_B, [](lv_event_t*){
    train_next();
  });
  btn(206, 178, LV_SYMBOL_OK "  SAVE & NEXT", COL_GREEN, [](lv_event_t*){
    train_save_and_next();
  });

  train_update_header();
  lv_scr_load_anim(s, LV_SCR_LOAD_ANIM_FADE_ON, 220, 0, true);
}

// =====================================================================
//   SETUP / LOOP
// =====================================================================
void setup(){
  Serial.begin(115200);
  hw_boot();
  splash_show();
  load_user_templates();   // load /WATCH/notes/_handwriting.dat if present
  // Boot lands on the NOTES LIBRARY
  open_library();
}
void loop(){
  lv_timer_handler();
  poll_boot_exit();
  delay(5);
}
