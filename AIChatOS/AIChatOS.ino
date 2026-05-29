
// =====================================================================
// AICHAT-OS.ino  --  LauncherOS-compatible mini-OS module
// Target  : Waveshare ESP32-S3 Touch AMOLED 2.06 (410 x 502, FT3168, AXP2101)
// Build   : ESP32-S3 Dev Module, PSRAM=OPI, Partition=Minimal SPIFFS (large APP)
// Output  : AIChatOS.ino.bin -> /WATCH/bin/aichatos.bin
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
#define ACCENT_A  0xEAEAEA
#define ACCENT_B  0x00E5FF

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

// ---- WiFi: hardcoded fallback + /WATCH/wifi/config.txt -------------
// Hardcoded credentials (matches LauncherOS / S3_MiniOS_206 exactly).
// Tried FIRST on every connect attempt, then we fall back to the
// up-to-5 networks defined in /WATCH/wifi/config.txt.
static const char *HC_WIFI_SSID = "Optus_9D2E3D";
static const char *HC_WIFI_PASS = "snucktemptGLeQU";

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
// Try a single SSID/pass with a per-attempt timeout.
static bool wifi_try_one(const char *ssid, const char *pass, uint32_t timeout_ms){
  if(!ssid || !*ssid) return false;
  WiFi.begin(ssid, pass);
  uint32_t t0=millis();
  while(WiFi.status()!=WL_CONNECTED && millis()-t0<timeout_ms){ delay(150); }
  return WiFi.status()==WL_CONNECTED;
}
// Boot-time / button-time auto-connect.
// Order per pass: HARDCODED first, then every WIFIn from config.txt.
// The whole list is attempted TWICE so a router that was momentarily
// unreachable on the first sweep still has a chance.
static bool wifi_quick_connect(uint32_t timeout_ms){
  WiFi.mode(WIFI_STA);
  if(WiFi.status()==WL_CONNECTED){ g_wifi_synced=true; return true; }
  for(int pass_n=0; pass_n<2; pass_n++){
    // 1) hardcoded first
    if(wifi_try_one(HC_WIFI_SSID, HC_WIFI_PASS, timeout_ms)){
      g_wifi_synced=true; return true;
    }
    // 2) then config.txt networks in order
    for(int i=0;i<g_creds_n;i++){
      if(wifi_try_one(g_creds[i].ssid.c_str(), g_creds[i].pass.c_str(), timeout_ms)){
        g_wifi_synced=true; return true;
      }
    }
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
  lv_obj_t *l=lv_label_create(s); lv_label_set_text(l,"AICHAT/OS");
  lv_obj_set_style_text_color(l,lv_color_hex(ACCENT_A),0);
  lv_obj_set_style_text_font(l,&lv_font_montserrat_48,0);
  lv_obj_align(l,LV_ALIGN_CENTER,0,-30);
  lv_obj_t *l2=lv_label_create(s); lv_label_set_text(l2,"WARMING NEURONS");
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
    // NOTE: WiFi is intentionally kept connected after boot (user request).
    // The previous battery-saving wifi_kill() at p==60 has been removed.
    if(digitalRead(BOOT_BTN)==LOW){
      if(!held) held=millis();
      else if(millis()-held>1500) return_to_launcher();
    } else held=0;
  }
}

// ---- Back-to-launcher (BOOT button only — no on-screen pill) -------

// ---- Battery pill (top-right of home) ------------------------------
static lv_obj_t *g_batt_lbl=nullptr;
static lv_obj_t *g_wifi_dot=nullptr;   // top-right sync dot (red/green)
static void update_batt_pill(lv_timer_t*){
  if(!g_has_pmu||!g_batt_lbl) return;
  int pct=g_pmu.getBatteryPercent(); if(pct<0) pct=0; if(pct>100) pct=100;
  char b[24]; snprintf(b,24,"%d%%%s", pct, g_pmu.isCharging()?" "LV_SYMBOL_CHARGE:"");
  lv_label_set_text(g_batt_lbl,b);
}
static void wifi_dot_refresh(){
  if(!g_wifi_dot) return;
  lv_obj_set_style_bg_color(g_wifi_dot,
      lv_color_hex(g_wifi_synced?COL_GREEN:COL_RED), 0);
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
  g_wifi_dot=lv_obj_create(scr); lv_obj_remove_style_all(g_wifi_dot);
  lv_obj_set_size(g_wifi_dot,8,8); lv_obj_align(g_wifi_dot,LV_ALIGN_TOP_RIGHT,-12,32);
  lv_obj_set_style_radius(g_wifi_dot,4,0);
  lv_obj_set_style_bg_opa(g_wifi_dot, LV_OPA_COVER,0);
  wifi_dot_refresh();
}

// ---- "Connect to WiFi" button (runs the boot auto-connect again) ---
// Tapping the pill re-runs wifi_quick_connect() (hardcoded -> config.txt,
// looped twice) and updates the top-right sync dot in place.  Radio is
// killed again afterwards to preserve battery, exactly like at boot.
static lv_obj_t *g_wifi_btn_lbl=nullptr;
static void connect_wifi_btn_cb(lv_event_t*){
  if(g_wifi_btn_lbl) lv_label_set_text(g_wifi_btn_lbl, "CONNECTING...");
  lv_timer_handler();
  g_wifi_synced=false;
  bool ok = wifi_quick_connect(2500);
  // Stay connected — do NOT call wifi_kill() (user request).
  wifi_dot_refresh();
  if(g_wifi_btn_lbl) lv_label_set_text(g_wifi_btn_lbl,
      ok ? LV_SYMBOL_WIFI "  CONNECTED" : LV_SYMBOL_WIFI "  CONNECT WIFI");
}
static void add_wifi_connect_pill(lv_obj_t *scr){
  lv_obj_t *b=lv_btn_create(scr);
  lv_obj_set_size(b, 170, 30);
  lv_obj_align(b, LV_ALIGN_TOP_RIGHT, -24, 52);
  lv_obj_set_style_radius(b, 15, 0);
  lv_obj_set_style_bg_color(b, lv_color_hex(0x0A0A0A), 0);
  lv_obj_set_style_border_color(b, lv_color_hex(ACCENT_B), 0);
  lv_obj_set_style_border_width(b, 1, 0);
  g_wifi_btn_lbl=lv_label_create(b);
  lv_label_set_text(g_wifi_btn_lbl,
      g_wifi_synced ? LV_SYMBOL_WIFI "  CONNECTED" : LV_SYMBOL_WIFI "  CONNECT WIFI");
  lv_obj_set_style_text_color(g_wifi_btn_lbl, lv_color_hex(ACCENT_B), 0);
  lv_obj_set_style_text_font(g_wifi_btn_lbl, &lv_font_montserrat_12, 0);
  lv_obj_set_style_text_letter_space(g_wifi_btn_lbl, 2, 0);
  lv_obj_center(g_wifi_btn_lbl);
  lv_obj_add_event_cb(b, connect_wifi_btn_cb, LV_EVENT_CLICKED, NULL);
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
// AICHAT-OS  --  cloud LLM chat (OpenAI). Boot picks voice OR keyboard.
// Voice mode uses the board's REAL Waveshare audio path:
//   ES8311 codec over I2C (addr ES8311_ADDRRES_0)
//   I2S pins BCLK=41 WS=45 DOUT=40 DIN=42 MCLK=16
//   PA enable on GPIO 46
//   Records 4s of 16 kHz / 16-bit mono PCM into PSRAM, wraps as a WAV,
//   multipart-POSTs to /v1/audio/transcriptions (whisper-1), then feeds
//   the transcript into chat completion (<=2 sentence reply).
// Requires Waveshare ES8311 + ESP_I2S libs (shipped with the board examples).
// ====================================================================
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <ESP_I2S.h>
#include "es8311.h"

static const char *OPENAI_KEY = "sk-abcdef1234567890abcdef1234567890abcdef12";
static const char *OPENAI_MODEL = "gpt-4o-mini";

#define MIC_RATE  16000
#define MIC_SECS  4
#define MIC_BYTES (MIC_SECS * MIC_RATE * 2)  // 16-bit mono
#define I2S_BCLK 41
#define I2S_WS   45
#define I2S_DOUT 40
#define I2S_DIN  42
#define I2S_MCLK 16
#define PA_EN    46

static I2SClass g_i2s;
static bool     g_audio_ready=false;

static int g_input_mode = 0; // 0=keyboard, 1=voice
static String g_compose = "";
static String g_history[6]; static int g_history_n=0;
static lv_obj_t *g_chat_lbl=nullptr, *g_compose_lbl=nullptr, *g_status=nullptr;
static lv_obj_t *g_voice_state=nullptr;
static uint8_t *g_wav_buf=nullptr;

static void history_push(const char *who, const char *txt){
  if(g_history_n>=6){ for(int i=0;i<5;i++) g_history[i]=g_history[i+1]; g_history_n=5; }
  g_history[g_history_n++] = String(who)+": "+txt;
  String all=""; for(int i=0;i<g_history_n;i++) all += g_history[i] + "\n\n";
  if(g_chat_lbl) lv_label_set_text(g_chat_lbl, all.c_str());
}

static String ai_reply(const String &prompt){
  if(WiFi.status()!=WL_CONNECTED){ wifi_quick_connect(3500); }
  if(WiFi.status()!=WL_CONNECTED) return "(no wifi)";
  WiFiClientSecure cli; cli.setInsecure();
  HTTPClient http;
  http.begin(cli, "https://api.openai.com/v1/chat/completions");
  http.addHeader("Content-Type","application/json");
  http.addHeader("Authorization", String("Bearer ")+OPENAI_KEY);
  String body = String("{\"model\":\"")+OPENAI_MODEL+
    "\",\"max_tokens\":120,\"messages\":[{\"role\":\"system\",\"content\":\"You answer in <=2 short sentences for a wristwatch screen.\"},"
    "{\"role\":\"user\",\"content\":\""+prompt+"\"}]}";
  int code = http.POST(body);
  String r = http.getString(); http.end();
  if(code<200||code>=300) return String("(http ")+code+")";
  int idx = r.indexOf("\"content\":\""); if(idx<0) return "(no content)";
  int start = idx+11;
  int end = r.indexOf("\"", start);
  while(end>0 && r.charAt(end-1)=='\\') end = r.indexOf("\"", end+1);
  if(end<0) return "(parse)";
  return r.substring(start, end);
}

// ---- ES8311 + I2S init (mirrors Waveshare 08_ES8311 example) -------
static esp_err_t es8311_setup(){
  es8311_handle_t h = es8311_create(0, ES8311_ADDRRES_0);
  if(!h) return ESP_FAIL;
  const es8311_clock_config_t clk = {
    .mclk_inverted = false, .sclk_inverted = false,
    .mclk_from_mclk_pin = true,
    .mclk_frequency = MIC_RATE * 256,
    .sample_frequency = MIC_RATE
  };
  if(es8311_init(h, &clk, ES8311_RESOLUTION_16, ES8311_RESOLUTION_16) != ESP_OK) return ESP_FAIL;
  es8311_sample_frequency_config(h, clk.mclk_frequency, clk.sample_frequency);
  es8311_microphone_config(h, false);
  es8311_voice_volume_set(h, 85, NULL);
  es8311_microphone_gain_set(h, (es8311_mic_gain_t)3);
  return ESP_OK;
}
static void audio_init_once(){
  if(g_audio_ready) return;
  g_i2s.setPins(I2S_BCLK, I2S_WS, I2S_DOUT, I2S_DIN, I2S_MCLK);
  if(!g_i2s.begin(I2S_MODE_STD, MIC_RATE, I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO, I2S_STD_SLOT_BOTH)){
    Serial.println("i2s begin fail"); return;
  }
  pinMode(PA_EN, OUTPUT); digitalWrite(PA_EN, HIGH);  // power speaker amp (used later)
  if(es8311_setup() != ESP_OK){ Serial.println("ES8311 init fail"); return; }
  g_audio_ready = true;
}

// ---- WAV header (mono, since we keep only one stereo channel) ------
static void wav_write_header(uint8_t *b, uint32_t pcm_bytes){
  uint32_t fsz = pcm_bytes + 36;
  uint32_t br  = MIC_RATE * 2;
  memcpy(b+0,  "RIFF", 4); memcpy(b+4, &fsz, 4);
  memcpy(b+8,  "WAVEfmt ", 8);
  uint32_t sub1=16; memcpy(b+16,&sub1,4);
  uint16_t pcm=1, ch=1, bps=16; uint16_t ba=2;
  memcpy(b+20,&pcm,2); memcpy(b+22,&ch,2);
  uint32_t sr=MIC_RATE; memcpy(b+24,&sr,4); memcpy(b+28,&br,4);
  memcpy(b+32,&ba,2); memcpy(b+34,&bps,2);
  memcpy(b+36,"data",4); memcpy(b+40,&pcm_bytes,4);
}
static String whisper_transcribe(){
  audio_init_once();
  if(!g_audio_ready) return "(audio init failed)";
  if(!g_wav_buf){
    g_wav_buf = (uint8_t*)heap_caps_malloc(MIC_BYTES+44, MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
    if(!g_wav_buf) return "(no PSRAM)";
  }
  if(g_voice_state) lv_label_set_text(g_voice_state, "RECORDING...");
  lv_timer_handler();

  // The codec gives 16-bit stereo frames; we'll keep the left channel only.
  // Read 2x more bytes than mono target, then downmix in-place.
  size_t want_stereo = MIC_BYTES * 2;
  uint8_t *tmp = (uint8_t*)heap_caps_malloc(want_stereo, MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
  if(!tmp) return "(oom rec)";
  size_t got = g_i2s.readBytes((char*)tmp, want_stereo);
  // downmix L+R -> L into wav data area (after 44-byte hdr)
  uint32_t mono_bytes = 0;
  int16_t *src = (int16_t*)tmp;
  int16_t *dst = (int16_t*)(g_wav_buf + 44);
  uint32_t frames = got / 4;  // 4 bytes per stereo frame
  for(uint32_t i=0;i<frames;i++){
    int32_t l = src[i*2], r = src[i*2+1];
    dst[i] = (int16_t)((l + r) / 2);
    mono_bytes += 2;
  }
  free(tmp);
  wav_write_header(g_wav_buf, mono_bytes);

  if(g_voice_state) lv_label_set_text(g_voice_state, "UPLOADING WHISPER...");
  lv_timer_handler();
  if(WiFi.status()!=WL_CONNECTED) wifi_quick_connect(3500);
  if(WiFi.status()!=WL_CONNECTED) return "(no wifi)";
  WiFiClientSecure cli; cli.setInsecure();
  HTTPClient http;
  http.begin(cli, "https://api.openai.com/v1/audio/transcriptions");
  http.addHeader("Authorization", String("Bearer ")+OPENAI_KEY);
  String bnd = "----wristboundary42";
  http.addHeader("Content-Type", "multipart/form-data; boundary=" + bnd);
  String pre =
    "--"+bnd+"\r\nContent-Disposition: form-data; name=\"model\"\r\n\r\nwhisper-1\r\n"+
    "--"+bnd+"\r\nContent-Disposition: form-data; name=\"response_format\"\r\n\r\ntext\r\n"+
    "--"+bnd+"\r\nContent-Disposition: form-data; name=\"file\"; filename=\"v.wav\"\r\nContent-Type: audio/wav\r\n\r\n";
  String post = "\r\n--"+bnd+"--\r\n";
  size_t wlen = 44 + mono_bytes;
  size_t body_len = pre.length() + wlen + post.length();
  uint8_t *mp = (uint8_t*)heap_caps_malloc(body_len, MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
  if(!mp) return "(oom multipart)";
  memcpy(mp, pre.c_str(), pre.length());
  memcpy(mp+pre.length(), g_wav_buf, wlen);
  memcpy(mp+pre.length()+wlen, post.c_str(), post.length());
  int code = http.sendRequest("POST", mp, body_len);
  String r = http.getString(); http.end();
  free(mp);
  if(code<200||code>=300){ char b[40]; snprintf(b,40,"(whisper http %d)",code); return String(b); }
  r.trim();
  return r;
}

static const char *KB_ROWS[3] = { "QWERTYUIOP", "ASDFGHJKL", "ZXCVBNM_." };

static void send_compose(){
  if(g_compose.length()==0) return;
  history_push("you", g_compose.c_str());
  String reply = ai_reply(g_compose);
  history_push("ai ", reply.c_str());
  g_compose=""; if(g_compose_lbl) lv_label_set_text(g_compose_lbl, " ");
}

static void mk_key(lv_obj_t *par, char c, int x,int y,int w,int h){
  lv_obj_t *b=lv_btn_create(par);
  lv_obj_set_size(b,w,h); lv_obj_set_pos(b,x,y);
  lv_obj_set_style_bg_color(b, lv_color_hex(0x101010),0);
  lv_obj_set_style_border_color(b, lv_color_hex(ACCENT_B),0);
  lv_obj_set_style_border_width(b,1,0); lv_obj_set_style_radius(b,8,0);
  lv_obj_t *l=lv_label_create(b);
  char buf[2]={c,0}; lv_label_set_text(l, c=='_'?"SPC":buf);
  lv_obj_set_style_text_color(l, lv_color_hex(ACCENT_A),0);
  lv_obj_set_style_text_font(l,&lv_font_montserrat_22,0); lv_obj_center(l);
  static char store[64]; static int idx=0; char *slot=&store[idx]; *slot=c; idx++;
  lv_obj_add_event_cb(b, [](lv_event_t*e){
    char ch=*(char*)lv_event_get_user_data(e);
    if(ch=='_') ch=' ';
    g_compose += ch;
    if(g_compose_lbl) lv_label_set_text(g_compose_lbl, g_compose.c_str());
  }, LV_EVENT_CLICKED, slot);
}

static void build_chat_home(){
  lv_obj_t *s=lv_obj_create(NULL);
  lv_obj_set_style_bg_color(s,lv_color_hex(COL_BG),0);
  lv_obj_set_style_bg_opa(s,LV_OPA_COVER,0);
  lv_obj_clear_flag(s,LV_OBJ_FLAG_SCROLLABLE);
  add_header_strip(s,"AICHAT",ACCENT_A);
  add_wifi_connect_pill(s);

  g_status=lv_label_create(s);
  lv_label_set_text(g_status, g_input_mode? "VOICE INPUT  ·  WHISPER-1" : "KEYBOARD INPUT");
  lv_obj_set_style_text_color(g_status, lv_color_hex(ACCENT_B),0);
  lv_obj_set_style_text_font(g_status, &lv_font_montserrat_12,0);
  lv_obj_set_style_text_letter_space(g_status,3,0);
  lv_obj_align(g_status,LV_ALIGN_TOP_LEFT,24,54);

  g_chat_lbl=lv_label_create(s);
  lv_label_set_long_mode(g_chat_lbl, LV_LABEL_LONG_WRAP);
  lv_obj_set_width(g_chat_lbl, LCD_W-40);
  lv_obj_align(g_chat_lbl, LV_ALIGN_TOP_LEFT, 20, 80);
  lv_obj_set_style_text_color(g_chat_lbl, lv_color_hex(COL_WHITE),0);
  lv_obj_set_style_text_font(g_chat_lbl, &lv_font_montserrat_14,0);
  lv_label_set_text(g_chat_lbl, "Ask me anything\n(replies kept short)");

  if(g_input_mode==0){
    g_compose_lbl=lv_label_create(s);
    lv_obj_set_width(g_compose_lbl, LCD_W-40);
    lv_label_set_long_mode(g_compose_lbl, LV_LABEL_LONG_WRAP);
    lv_obj_align(g_compose_lbl, LV_ALIGN_TOP_LEFT, 20, 180);
    lv_obj_set_style_text_color(g_compose_lbl, lv_color_hex(ACCENT_B),0);
    lv_obj_set_style_text_font(g_compose_lbl, &lv_font_montserrat_22,0);
    lv_label_set_text(g_compose_lbl, " ");

    lv_obj_t *kbBg=lv_obj_create(s); lv_obj_remove_style_all(kbBg);
    lv_obj_set_size(kbBg, LCD_W-40, 195);
    lv_obj_align(kbBg, LV_ALIGN_BOTTOM_MID, 0, -70);
    lv_obj_set_style_bg_color(kbBg, lv_color_hex(0x050505),0);
    lv_obj_set_style_bg_opa(kbBg, LV_OPA_COVER,0);
    lv_obj_set_style_radius(kbBg,12,0);
    lv_obj_clear_flag(kbBg, LV_OBJ_FLAG_SCROLLABLE);
    int kw=33,kh=52,gap=3;
    for(int r=0;r<3;r++){
      int len=strlen(KB_ROWS[r]);
      int row_w=len*kw+(len-1)*gap;
      int xoff=(LCD_W-40-row_w)/2;
      int yoff=r*(kh+gap)+8;
      for(int i=0;i<len;i++) mk_key(kbBg, KB_ROWS[r][i], xoff+i*(kw+gap), yoff, kw, kh);
    }
    lv_obj_t *send=lv_btn_create(s); lv_obj_set_size(send,160,40);
    lv_obj_align(send, LV_ALIGN_BOTTOM_MID, 0, -28);
    lv_obj_set_style_bg_color(send, lv_color_hex(ACCENT_B),0);
    lv_obj_set_style_radius(send,20,0);
    lv_obj_t *sl=lv_label_create(send); lv_label_set_text(sl, "ASK  " LV_SYMBOL_RIGHT);
    lv_obj_set_style_text_color(sl,lv_color_hex(0x001824),0);
    lv_obj_set_style_text_font(sl,&lv_font_montserrat_18,0); lv_obj_center(sl);
    lv_obj_add_event_cb(send,[](lv_event_t*){ send_compose(); },LV_EVENT_CLICKED,NULL);
  } else {
    audio_init_once();
    lv_obj_t *mic=lv_btn_create(s);
    lv_obj_set_size(mic,200,200);
    lv_obj_align(mic, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_radius(mic,100,0);
    lv_obj_set_style_bg_color(mic, lv_color_hex(0x0A0A0A),0);
    lv_obj_set_style_border_color(mic, lv_color_hex(ACCENT_B),0);
    lv_obj_set_style_border_width(mic,2,0);
    lv_obj_t *ml=lv_label_create(mic); lv_label_set_text(ml, LV_SYMBOL_AUDIO);
    lv_obj_set_style_text_color(ml,lv_color_hex(ACCENT_B),0);
    lv_obj_set_style_text_font(ml,&lv_font_montserrat_48,0); lv_obj_center(ml);

    g_voice_state=lv_label_create(s); lv_label_set_text(g_voice_state,"TAP TO RECORD 4s");
    lv_obj_set_style_text_color(g_voice_state,lv_color_hex(COL_TEXT_DIM),0);
    lv_obj_set_style_text_font(g_voice_state,&lv_font_montserrat_14,0);
    lv_obj_set_style_text_letter_space(g_voice_state,3,0);
    lv_obj_align(g_voice_state, LV_ALIGN_BOTTOM_MID, 0, -60);

    lv_obj_add_event_cb(mic,[](lv_event_t*){
      String t = whisper_transcribe();
      if(t.length()==0) t = "(empty)";
      history_push("you", t.c_str());
      if(g_voice_state) lv_label_set_text(g_voice_state, "ASKING GPT...");
      String reply = ai_reply(t);
      history_push("ai", reply.c_str());
      if(g_voice_state) lv_label_set_text(g_voice_state, "TAP TO RECORD 4s");
    },LV_EVENT_CLICKED,NULL);
  }
  add_boot_hint(s);
  lv_scr_load_anim(s, LV_SCR_LOAD_ANIM_FADE_ON, 260,0,false);
}

static void build_input_select(){
  lv_obj_t *s=lv_obj_create(NULL);
  lv_obj_set_style_bg_color(s,lv_color_hex(COL_BG),0);
  lv_obj_set_style_bg_opa(s,LV_OPA_COVER,0);
  lv_obj_clear_flag(s,LV_OBJ_FLAG_SCROLLABLE);

  // tiny header so the wifi pill has a place to sit
  lv_obj_t *brand=lv_label_create(s); lv_label_set_text(brand,"AICHAT");
  lv_obj_set_style_text_color(brand,lv_color_hex(ACCENT_A),0);
  lv_obj_set_style_text_font(brand,&lv_font_montserrat_22,0);
  lv_obj_set_style_text_letter_space(brand,4,0);
  lv_obj_align(brand,LV_ALIGN_TOP_LEFT,24,22);
  add_wifi_connect_pill(s);

  lv_obj_t *t=lv_label_create(s); lv_label_set_text(t,"CHOOSE INPUT");
  lv_obj_set_style_text_color(t,lv_color_hex(ACCENT_A),0);
  lv_obj_set_style_text_font(t,&lv_font_montserrat_22,0);
  lv_obj_set_style_text_letter_space(t,4,0);
  lv_obj_align(t,LV_ALIGN_TOP_MID,0,110);

  auto mk = [&](const char *name, const char *sym, int yoff, int mode){
    lv_obj_t *c=lv_btn_create(s);
    lv_obj_set_size(c, LCD_W-80, 110);
    lv_obj_align(c, LV_ALIGN_CENTER, 0, yoff);
    lv_obj_set_style_bg_color(c, lv_color_hex(0x0A0A0A),0);
    lv_obj_set_style_border_color(c, lv_color_hex(ACCENT_B),0);
    lv_obj_set_style_border_width(c,2,0);
    lv_obj_set_style_radius(c,18,0);
    lv_obj_t *icn=lv_label_create(c); lv_label_set_text(icn,sym);
    lv_obj_set_style_text_color(icn,lv_color_hex(ACCENT_B),0);
    lv_obj_set_style_text_font(icn,&lv_font_montserrat_48,0);
    lv_obj_align(icn, LV_ALIGN_LEFT_MID, 30, 0);
    lv_obj_t *nm=lv_label_create(c); lv_label_set_text(nm,name);
    lv_obj_set_style_text_color(nm,lv_color_hex(COL_WHITE),0);
    lv_obj_set_style_text_font(nm,&lv_font_montserrat_22,0);
    lv_obj_set_style_text_letter_space(nm,3,0);
    lv_obj_align(nm, LV_ALIGN_RIGHT_MID, -40, 0);
    static int slot[2]; slot[mode]=mode;
    lv_obj_add_event_cb(c, [](lv_event_t*e){
      g_input_mode = *(int*)lv_event_get_user_data(e);
      build_chat_home();
    }, LV_EVENT_CLICKED, &slot[mode]);
  };
  mk("KEYBOARD", LV_SYMBOL_KEYBOARD, -70, 0);
  mk("VOICE",    LV_SYMBOL_AUDIO,     70, 1);

  lv_obj_t *hint=lv_label_create(s);
  lv_label_set_text(hint,"hold BOOT 1.5s to exit anytime");
  lv_obj_set_style_text_color(hint,lv_color_hex(COL_TEXT_DIM),0);
  lv_obj_set_style_text_font(hint,&lv_font_montserrat_12,0);
  lv_obj_align(hint,LV_ALIGN_BOTTOM_MID,0,-25);

  lv_scr_load_anim(s, LV_SCR_LOAD_ANIM_FADE_ON, 260,0,false);
}

// ====================================================================
//  Carousel home  (CHAT / WIFI / BATTERY)
//  LauncherOS-style: three full-screen tiles, swipe horizontally.
//  Implemented with lv_tileview so swipes are first-class & snappy.
// ====================================================================
static lv_obj_t *g_tile_chat=nullptr, *g_tile_wifi=nullptr, *g_tile_batt=nullptr;
static lv_obj_t *g_wifi_list_holder=nullptr;
static lv_obj_t *g_batt_pct_lbl=nullptr, *g_batt_state_lbl=nullptr, *g_batt_volt_lbl=nullptr;
static lv_obj_t *g_wifi_status_lbl=nullptr;

static const char* wifi_status_text(){
  if(WiFi.status()==WL_CONNECTED) return "CONNECTED";
  return "NOT CONNECTED";
}

// (Re)populates the wifi tile's network list from hardcoded + config.txt.
// Active SSID is highlighted with the accent colour + check mark.
static void wifi_refresh_list(){
  if(!g_wifi_list_holder) return;
  lv_obj_clean(g_wifi_list_holder);
  String active = (WiFi.status()==WL_CONNECTED) ? WiFi.SSID() : String("");

  auto add_row = [&](const char *ssid, const char *tag){
    if(!ssid || !*ssid) return;
    lv_obj_t *row = lv_obj_create(g_wifi_list_holder);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, LCD_W-60, 44);
    lv_obj_set_style_bg_color(row, lv_color_hex(0x0A0A0A), 0);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(row, 10, 0);
    lv_obj_set_style_border_color(row,
        lv_color_hex(active == ssid ? ACCENT_B : 0x202020), 0);
    lv_obj_set_style_border_width(row, 1, 0);
    lv_obj_set_style_pad_all(row, 10, 0);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *nm = lv_label_create(row);
    lv_label_set_text(nm, ssid);
    lv_obj_set_style_text_color(nm,
        lv_color_hex(active == ssid ? ACCENT_B : COL_WHITE), 0);
    lv_obj_set_style_text_font(nm, &lv_font_montserrat_14, 0);
    lv_obj_align(nm, LV_ALIGN_LEFT_MID, 4, 0);

    lv_obj_t *tg = lv_label_create(row);
    lv_label_set_text(tg, active == ssid ? LV_SYMBOL_OK "  ACTIVE" : tag);
    lv_obj_set_style_text_color(tg,
        lv_color_hex(active == ssid ? ACCENT_B : COL_TEXT_DIM), 0);
    lv_obj_set_style_text_font(tg, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_letter_space(tg, 2, 0);
    lv_obj_align(tg, LV_ALIGN_RIGHT_MID, -4, 0);
  };

  add_row(HC_WIFI_SSID, "HARDCODED");
  for(int i=0;i<g_creds_n;i++){
    String tag = String("CONFIG ") + (i+1);
    add_row(g_creds[i].ssid.c_str(), tag.c_str());
  }
  if(g_creds_n==0){
    lv_obj_t *empty = lv_label_create(g_wifi_list_holder);
    lv_label_set_text(empty, "no /WATCH/wifi/config.txt entries");
    lv_obj_set_style_text_color(empty, lv_color_hex(COL_TEXT_DIM), 0);
    lv_obj_set_style_text_font(empty, &lv_font_montserrat_12, 0);
  }
  if(g_wifi_status_lbl)
    lv_label_set_text(g_wifi_status_lbl, wifi_status_text());
}

static void carousel_wifi_reconnect_cb(lv_event_t*){
  if(g_wifi_status_lbl) lv_label_set_text(g_wifi_status_lbl, "CONNECTING...");
  lv_timer_handler();
  g_wifi_synced=false;
  wifi_quick_connect(2500);   // hardcoded + config.txt, twice; stays connected
  wifi_dot_refresh();
  wifi_refresh_list();
}

// User-triggered disconnect.  Drops the current association but keeps the
// WiFi driver alive so RECONNECT works instantly afterwards.
static void carousel_wifi_disconnect_cb(lv_event_t*){
  if(g_wifi_status_lbl) lv_label_set_text(g_wifi_status_lbl, "DISCONNECTING...");
  lv_timer_handler();
  WiFi.disconnect(true, false);   // drop AP, erase creds, keep radio on STA
  WiFi.mode(WIFI_STA);
  uint32_t t0=millis();
  while(WiFi.status()==WL_CONNECTED && millis()-t0<1500) delay(50);
  g_wifi_synced=false;
  wifi_dot_refresh();
  wifi_refresh_list();
}

static void carousel_chat_open_cb(lv_event_t*){ build_input_select(); }

// Live battery readout — runs on a 2s timer while the carousel is shown.
static void carousel_batt_tick(lv_timer_t*){
  if(!g_has_pmu || !g_batt_pct_lbl) return;
  int pct = g_pmu.getBatteryPercent(); if(pct<0) pct=0; if(pct>100) pct=100;
  char b[16]; snprintf(b,16,"%d%%", pct);
  lv_label_set_text(g_batt_pct_lbl, b);
  lv_label_set_text(g_batt_state_lbl,
      g_pmu.isCharging() ? LV_SYMBOL_CHARGE "  CHARGING" : "DISCHARGING");
  uint16_t mv = g_pmu.getBattVoltage();   // millivolts
  char v[24]; snprintf(v,24,"%u.%03u V", mv/1000, mv%1000);
  lv_label_set_text(g_batt_volt_lbl, v);
}

static void mk_tile_header(lv_obj_t *tile, const char *title, uint32_t accent){
  lv_obj_t *t=lv_label_create(tile); lv_label_set_text(t,title);
  lv_obj_set_style_text_color(t,lv_color_hex(accent),0);
  lv_obj_set_style_text_font(t,&lv_font_montserrat_22,0);
  lv_obj_set_style_text_letter_space(t,4,0);
  lv_obj_align(t,LV_ALIGN_TOP_LEFT,24,22);
}

static void build_carousel_home(){
  lv_obj_t *s=lv_obj_create(NULL);
  lv_obj_set_style_bg_color(s,lv_color_hex(COL_BG),0);
  lv_obj_set_style_bg_opa(s,LV_OPA_COVER,0);
  lv_obj_clear_flag(s,LV_OBJ_FLAG_SCROLLABLE);

  lv_obj_t *tv = lv_tileview_create(s);
  lv_obj_set_size(tv, LCD_W, LCD_H);
  lv_obj_set_style_bg_color(tv, lv_color_hex(COL_BG), 0);
  lv_obj_set_style_bg_opa(tv, LV_OPA_COVER, 0);
  lv_obj_set_scrollbar_mode(tv, LV_SCROLLBAR_MODE_OFF);

  // --- Tile 0: CHAT ----------------------------------------------------
  g_tile_chat = lv_tileview_add_tile(tv, 0, 0, LV_DIR_RIGHT);
  lv_obj_clear_flag(g_tile_chat, LV_OBJ_FLAG_SCROLLABLE);
  mk_tile_header(g_tile_chat, "AICHAT", ACCENT_A);
  {
    lv_obj_t *card = lv_btn_create(g_tile_chat);
    lv_obj_set_size(card, LCD_W-80, 240);
    lv_obj_align(card, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(card, lv_color_hex(0x0A0A0A),0);
    lv_obj_set_style_border_color(card, lv_color_hex(ACCENT_B),0);
    lv_obj_set_style_border_width(card,2,0);
    lv_obj_set_style_radius(card,22,0);
    lv_obj_t *icn=lv_label_create(card); lv_label_set_text(icn, LV_SYMBOL_KEYBOARD);
    lv_obj_set_style_text_color(icn,lv_color_hex(ACCENT_B),0);
    lv_obj_set_style_text_font(icn,&lv_font_montserrat_48,0);
    lv_obj_align(icn, LV_ALIGN_TOP_MID, 0, 30);
    lv_obj_t *nm=lv_label_create(card); lv_label_set_text(nm,"CHAT");
    lv_obj_set_style_text_color(nm,lv_color_hex(COL_WHITE),0);
    lv_obj_set_style_text_font(nm,&lv_font_montserrat_22,0);
    lv_obj_set_style_text_letter_space(nm,4,0);
    lv_obj_align(nm, LV_ALIGN_CENTER, 0, 10);
    lv_obj_t *sub=lv_label_create(card); lv_label_set_text(sub,"keyboard or voice  >  GPT");
    lv_obj_set_style_text_color(sub,lv_color_hex(COL_TEXT_DIM),0);
    lv_obj_set_style_text_font(sub,&lv_font_montserrat_12,0);
    lv_obj_align(sub, LV_ALIGN_BOTTOM_MID, 0, -24);
    lv_obj_add_event_cb(card, carousel_chat_open_cb, LV_EVENT_CLICKED, NULL);
  }

  // --- Tile 1: WIFI ----------------------------------------------------
  g_tile_wifi = lv_tileview_add_tile(tv, 1, 0, (lv_dir_t)(LV_DIR_LEFT|LV_DIR_RIGHT));
  lv_obj_clear_flag(g_tile_wifi, LV_OBJ_FLAG_SCROLLABLE);
  mk_tile_header(g_tile_wifi, "WIFI", ACCENT_B);

  g_wifi_status_lbl = lv_label_create(g_tile_wifi);
  lv_label_set_text(g_wifi_status_lbl, wifi_status_text());
  lv_obj_set_style_text_color(g_wifi_status_lbl, lv_color_hex(ACCENT_B), 0);
  lv_obj_set_style_text_font(g_wifi_status_lbl, &lv_font_montserrat_12, 0);
  lv_obj_set_style_text_letter_space(g_wifi_status_lbl, 3, 0);
  lv_obj_align(g_wifi_status_lbl, LV_ALIGN_TOP_LEFT, 24, 54);

  g_wifi_list_holder = lv_obj_create(g_tile_wifi);
  lv_obj_remove_style_all(g_wifi_list_holder);
  lv_obj_set_size(g_wifi_list_holder, LCD_W-40, 300);
  lv_obj_align(g_wifi_list_holder, LV_ALIGN_TOP_MID, 0, 80);
  lv_obj_set_style_pad_row(g_wifi_list_holder, 6, 0);
  lv_obj_set_flex_flow(g_wifi_list_holder, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(g_wifi_list_holder,
      LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_scroll_dir(g_wifi_list_holder, LV_DIR_VER);

  // RECONNECT (left) and DISCONNECT (right) pills, side-by-side at the bottom.
  int bw = (LCD_W - 80) / 2 - 6;
  lv_obj_t *rec = lv_btn_create(g_tile_wifi);
  lv_obj_set_size(rec, bw, 44);
  lv_obj_align(rec, LV_ALIGN_BOTTOM_LEFT, 40, -40);
  lv_obj_set_style_radius(rec, 22, 0);
  lv_obj_set_style_bg_color(rec, lv_color_hex(ACCENT_B), 0);
  lv_obj_t *rl = lv_label_create(rec);
  lv_label_set_text(rl, LV_SYMBOL_REFRESH "  CONNECT");
  lv_obj_set_style_text_color(rl, lv_color_hex(0x001824), 0);
  lv_obj_set_style_text_font(rl, &lv_font_montserrat_14, 0);
  lv_obj_set_style_text_letter_space(rl, 2, 0);
  lv_obj_center(rl);
  lv_obj_add_event_cb(rec, carousel_wifi_reconnect_cb, LV_EVENT_CLICKED, NULL);

  lv_obj_t *dcn = lv_btn_create(g_tile_wifi);
  lv_obj_set_size(dcn, bw, 44);
  lv_obj_align(dcn, LV_ALIGN_BOTTOM_RIGHT, -40, -40);
  lv_obj_set_style_radius(dcn, 22, 0);
  lv_obj_set_style_bg_color(dcn, lv_color_hex(0x0A0A0A), 0);
  lv_obj_set_style_border_color(dcn, lv_color_hex(COL_RED), 0);
  lv_obj_set_style_border_width(dcn, 1, 0);
  lv_obj_t *dl = lv_label_create(dcn);
  lv_label_set_text(dl, LV_SYMBOL_CLOSE "  DISCONNECT");
  lv_obj_set_style_text_color(dl, lv_color_hex(COL_RED), 0);
  lv_obj_set_style_text_font(dl, &lv_font_montserrat_14, 0);
  lv_obj_set_style_text_letter_space(dl, 2, 0);
  lv_obj_center(dl);
  lv_obj_add_event_cb(dcn, carousel_wifi_disconnect_cb, LV_EVENT_CLICKED, NULL);

  wifi_refresh_list();

  // --- Tile 2: BATTERY -------------------------------------------------
  g_tile_batt = lv_tileview_add_tile(tv, 2, 0, LV_DIR_LEFT);
  lv_obj_clear_flag(g_tile_batt, LV_OBJ_FLAG_SCROLLABLE);
  mk_tile_header(g_tile_batt, "BATTERY", COL_GREEN);

  g_batt_pct_lbl = lv_label_create(g_tile_batt);
  lv_label_set_text(g_batt_pct_lbl, "--%");
  lv_obj_set_style_text_color(g_batt_pct_lbl, lv_color_hex(COL_WHITE), 0);
  lv_obj_set_style_text_font(g_batt_pct_lbl, &lv_font_montserrat_48, 0);
  lv_obj_align(g_batt_pct_lbl, LV_ALIGN_CENTER, 0, -40);

  g_batt_state_lbl = lv_label_create(g_tile_batt);
  lv_label_set_text(g_batt_state_lbl, "...");
  lv_obj_set_style_text_color(g_batt_state_lbl, lv_color_hex(ACCENT_B), 0);
  lv_obj_set_style_text_font(g_batt_state_lbl, &lv_font_montserrat_14, 0);
  lv_obj_set_style_text_letter_space(g_batt_state_lbl, 3, 0);
  lv_obj_align(g_batt_state_lbl, LV_ALIGN_CENTER, 0, 20);

  g_batt_volt_lbl = lv_label_create(g_tile_batt);
  lv_label_set_text(g_batt_volt_lbl, "-.--- V");
  lv_obj_set_style_text_color(g_batt_volt_lbl, lv_color_hex(COL_TEXT_DIM), 0);
  lv_obj_set_style_text_font(g_batt_volt_lbl, &lv_font_montserrat_14, 0);
  lv_obj_align(g_batt_volt_lbl, LV_ALIGN_CENTER, 0, 60);

  lv_obj_t *bhint = lv_label_create(g_tile_batt);
  lv_label_set_text(bhint, "swipe " LV_SYMBOL_LEFT "  back to WIFI");
  lv_obj_set_style_text_color(bhint, lv_color_hex(COL_TEXT_DIM), 0);
  lv_obj_set_style_text_font(bhint, &lv_font_montserrat_12, 0);
  lv_obj_align(bhint, LV_ALIGN_BOTTOM_MID, 0, -40);

  lv_timer_create(carousel_batt_tick, 2000, NULL);
  carousel_batt_tick(nullptr);

  // Page-indicator dots (3 tiles)
  for(int i=0;i<3;i++){
    lv_obj_t *d=lv_obj_create(s); lv_obj_remove_style_all(d);
    lv_obj_set_size(d, 6, 6); lv_obj_set_style_radius(d, 3, 0);
    lv_obj_set_style_bg_color(d, lv_color_hex(0x444444), 0);
    lv_obj_set_style_bg_opa(d, LV_OPA_COVER, 0);
    lv_obj_align(d, LV_ALIGN_BOTTOM_MID, (i-1)*16, -12);
  }

  // Boot-button exit hint
  add_boot_hint(s);

  // Start on the CHAT tile.
  lv_obj_set_tile(tv, g_tile_chat, LV_ANIM_OFF);

  lv_scr_load_anim(s, LV_SCR_LOAD_ANIM_FADE_ON, 260, 0, false);
}


void setup(){ Serial.begin(115200); hw_boot(); splash_show(); build_carousel_home(); }
void loop(){ lv_timer_handler(); poll_boot_exit(); delay(5); }
