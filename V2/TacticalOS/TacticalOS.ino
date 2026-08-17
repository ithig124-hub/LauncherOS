// =============================================================================
// TacticalOS.ino v2.4 -- LauncherOS-compatible operator / adventure OS
// Target  : Waveshare ESP32-S3 Touch AMOLED 2.06 (410 x 502, CO5300, FT3168)
// Compile : ESP32-S3 Dev Module / PSRAM=OPI / Flash=16MB / Minimal SPIFFS
// -----------------------------------------------------------------------------
// v2.4 CHANGES (user requested):
//   * TOOLS tab SPLIT into TWO SUB-CARDS, mirroring the S3 MiniOS pattern
//     (left/right swipe = switch category, up/down swipe = switch sub-card
//     within the current category).
//       Page 1 - "SURVIVAL // TOOLS"  : Flash, Beacon, Mission Timer,
//                                       Countdown, Morse Keyer.
//       Page 2 - "SETTINGS // PREFS"  : Night/Stealth toggles, Pace pill,
//                                       Theme cycle, Stride Cal, Watch Face.
//     Swipe UP   -> reveal Settings (page 2).
//     Swipe DOWN -> back to Tools  (page 1).
//     Transition is an INSTANT SNAP -- no animation, no lag.  Children of
//     both sub-pages get LV_OBJ_FLAG_EVENT_BUBBLE so the gesture reaches
//     the page container no matter which control was pressed first.
//     Horizontal swipes still bubble to the tabview (we only consume
//     LV_DIR_TOP / LV_DIR_BOTTOM).
// v2.3 CHANGES (user requested):
//   * PIN LOCK fully REMOVED.  No boot keypad, no overlay, no Settings toggle.
//     Watch boots straight into the home tab -- same first-touch feel as
//     LauncherOS.  Old `tac-pin` NVS namespace is left untouched on disk.
//   * WATCH FACE is no longer cycled by tapping the home centre.  The face
//     can ONLY be changed from the new "WATCH FACE" card in the TOOLS tab
//     (settings).  Choice persists across reboots via NVS (tac-face/idx).
//   * SWIPE NAVIGATION re-enabled across every tab.  The tabview's content
//     wrapper is scrollable again so left/right swipe moves between
//     HOME / CMP / NAV / TOOLS / CAMP / WALK / DATA the way LVGL intends.
//     Tap-edge nav is kept as a secondary input for gloved use.
// v2.2 ADDS:
//   * Back-to-launcher pill REMOVED on every tab.  Long-press BOOT (1.5 s)
//     is now the only exit path -- same as StudyOS.
//   * TAP-EDGE NAV: tap the leftmost ~20% of the screen -> previous tab,
//     rightmost ~20% -> next tab.  Center band keeps the existing
//     double-tap flashlight gesture.  No animation, instant snap to tab.
// v2.1: TRAIL BREADCRUMBS, PACE COUNTER, 3 CAMO THEMES, STRIDE CALIBRATE
// v2.0 features (PIN, watchfaces, NAV waypoints, Morse keyer, beacon, etc.)
// remain intact -- see /app/memory/PRD.md.
// =============================================================================

// ===== BEGIN SHARED BOOT BLOCK (LauncherOS v6.1+ compatible) =================
#include <Arduino.h>
#include <Wire.h>
#include <SD_MMC.h>
#include <WiFi.h>
#include <Preferences.h>
#include <time.h>
#include <sys/time.h>
#include <esp_partition.h>
#include <esp_ota_ops.h>
#include <esp_system.h>
#include <esp_sleep.h>
#include <memory>
#include <math.h>

#include "Arduino_GFX_Library.h"
#include "Arduino_DriveBus_Library.h"
#include "lv_conf.h"
#include "lvgl.h"

#define XPOWERS_CHIP_AXP2101
#include "XPowersLib.h"

// ---- Forward type declarations ----------------------------------------------
struct AccG { float x, y, z; };

// ---- Pin map (must match LauncherOS) ----------------------------------------
#define LCD_SDIO0   4
#define LCD_SDIO1   5
#define LCD_SDIO2   6
#define LCD_SDIO3   7
#define LCD_SCLK    11
#define LCD_CS      12
#define LCD_RESET   8
#define LCD_WIDTH   410
#define LCD_HEIGHT  502

#define IIC_SDA     15
#define IIC_SCL     14
#define TP_INT      38
#define TP_RESET    9

static const int SDMMC_CLK  = 2;
static const int SDMMC_CMD  = 1;
static const int SDMMC_DATA = 3;

#define PWR_BTN_PIN   10
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

// ---- LVGL plumbing ----------------------------------------------------------
static lv_disp_draw_buf_t draw_buf;
static lv_color_t        *lvgl_buf = nullptr;
static lv_disp_drv_t      disp_drv;
static lv_indev_drv_t     indev_drv;
static bool     g_touch_pressed_now = false;
static uint32_t g_last_touch_t = 0;
static int32_t  g_touch_x = 0, g_touch_y = 0;

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

// ---- AXP2101 PMU ------------------------------------------------------------
XPowersPMU g_pmu;
static bool g_has_pmu = false;
static void pmu_init() {
  if (g_pmu.begin(Wire, AXP2101_SLAVE_ADDRESS, IIC_SDA, IIC_SCL)) {
    g_has_pmu = true;
    g_pmu.disableTSPinMeasure();
    g_pmu.enableBattVoltageMeasure();
  }
}

// ---- Back-to-launcher (mandatory) -------------------------------------------
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

// ---- CO5300 brightness ------------------------------------------------------
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

// ---- Boot bring-up ----------------------------------------------------------
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
// NVS clock restore -- read LauncherOS' "rtcsave" namespace at boot
// =============================================================================
static Preferences g_timeprefs;
static int32_t  g_gmt_offset_sec = 0;
static bool     g_time_valid = false;
static uint32_t g_last_ntp_ok = 0;

static void time_restore_from_nvs() {
  g_timeprefs.begin("rtcsave", true);
  uint32_t t   = g_timeprefs.getULong("t", 0);
  int32_t  gmt = g_timeprefs.getLong("gmt", 0);
  g_last_ntp_ok = g_timeprefs.getULong("ntp", 0);
  g_timeprefs.end();
  if (t < 1700000000UL) return;
  g_gmt_offset_sec = gmt;
  struct timeval tv = { (time_t)t, 0 };
  settimeofday(&tv, NULL);
  g_time_valid = true;
}

// =============================================================================
// WiFi + NTP (one-shot, kills radio after)
// =============================================================================
struct WifiCred { String ssid; String pass; };
static WifiCred g_wifi_list[6];
static int      g_wifi_count = 0;
static float    g_lat = 0.0f;
static float    g_lon = 0.0f;

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
      if      (k == "GMT_OFFSET") g_gmt_offset_sec = v.toInt() * 3600;
      else if (k == "LAT")        g_lat = v.toFloat();
      else if (k == "LON")        g_lon = v.toFloat();
    }
    f.close();
  }
  g_wifi_list[g_wifi_count].ssid = HC_WIFI_SSID;
  g_wifi_list[g_wifi_count].pass = HC_WIFI_PASS;
  g_wifi_count++;
}

static bool g_wifi_synced = false;
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
// QMI8658 IMU
// =============================================================================
#define QMI_ADDR_PRIMARY   0x6B
#define QMI_ADDR_SECONDARY 0x6A
static uint8_t g_qmi_addr = 0;
static bool    g_has_imu  = false;

static bool qmi_w(uint8_t r, uint8_t v) {
  Wire.beginTransmission(g_qmi_addr);
  Wire.write(r); Wire.write(v);
  return Wire.endTransmission() == 0;
}
static bool qmi_r(uint8_t r, uint8_t *out, size_t n) {
  Wire.beginTransmission(g_qmi_addr); Wire.write(r);
  if (Wire.endTransmission(false) != 0) return false;
  Wire.requestFrom((int)g_qmi_addr, (int)n);
  for (size_t i = 0; i < n && Wire.available(); i++) out[i] = Wire.read();
  return true;
}
static void qmi_init() {
  for (uint8_t a : { (uint8_t)QMI_ADDR_PRIMARY, (uint8_t)QMI_ADDR_SECONDARY }) {
    Wire.beginTransmission(a);
    if (Wire.endTransmission() == 0) {
      g_qmi_addr = a;
      uint8_t who = 0; qmi_r(0x00, &who, 1);
      if (who == 0x05) {
        qmi_w(0x02, 0x60);
        qmi_w(0x03, 0x95);
        qmi_w(0x04, 0x65);
        qmi_w(0x08, 0x03);
        g_has_imu = true;
        return;
      }
    }
  }
}
static AccG qmi_acc() {
  AccG g = {0,0,0};
  if (!g_has_imu) return g;
  uint8_t b[6];
  if (!qmi_r(0x35, b, 6)) return g;
  int16_t ax = (int16_t)((b[1]<<8)|b[0]);
  int16_t ay = (int16_t)((b[3]<<8)|b[2]);
  int16_t az = (int16_t)((b[5]<<8)|b[4]);
  g.x = ax / 16384.0f; g.y = ay / 16384.0f; g.z = az / 16384.0f;
  return g;
}
static float    g_heading_deg = 0.0f;
static uint32_t g_last_gyro_ms = 0;
static void update_heading() {
  if (!g_has_imu) return;
  uint8_t b[2];
  if (!qmi_r(0x3F, b, 2)) return;
  int16_t gz = (int16_t)((b[1]<<8)|b[0]);
  float dps = gz / 128.0f;
  uint32_t now = millis();
  if (g_last_gyro_ms) {
    float dt = (now - g_last_gyro_ms) / 1000.0f;
    g_heading_deg += dps * dt;
    while (g_heading_deg >= 360.0f) g_heading_deg -= 360.0f;
    while (g_heading_deg <    0.0f) g_heading_deg += 360.0f;
  }
  g_last_gyro_ms = now;
}
static void heading_zero() { g_heading_deg = 0.0f; }

// =============================================================================
// Step counter + dead-reckoning position + 7-day history
// v2.3: detector rewritten -- AC-coupled peak detector + minimum-interval
//   refractory window, ported from SportOS.  The previous single-threshold
//   detector double-counted on bumps and triggered on hand-jitter; this one
//   ignores DC gravity via a running mean and rejects bursts faster than
//   ~3.5 Hz which is well above human cadence.
// =============================================================================
static uint32_t g_steps        = 0;
static uint16_t g_steps_yday   = 0xFFFF;
static Preferences g_steps_prefs;
static float    g_acc_mean     = 1.0f;   // EMA of |a| -- tracks gravity DC
static float    g_acc_prev_ac  = 0.0f;   // previous AC-coupled sample (for slope)
static uint32_t g_last_step_ms = 0;      // refractory timestamp
static float    g_stride_m     = 0.75f;  // meters per step (configurable)
static float    g_pos_x_m      = 0.0f;   // dead-reckoning X (east+)
static float    g_pos_y_m      = 0.0f;   // dead-reckoning Y (north+)
static float    g_trip_m       = 0.0f;   // total distance this session
static uint32_t g_steps_hist[7] = {0,0,0,0,0,0,0};  // [0]=today...[6]=6 days ago

// Panic-tap detection
static uint32_t g_tap_times[3] = {0,0,0};
static uint8_t  g_tap_idx = 0;
static bool     g_panic_armed = true;

// Breadcrumbs (ring buffer of last 64 positions, recorded every 5 steps)
#define CRUMB_N 64
static float    g_crumb_x[CRUMB_N];
static float    g_crumb_y[CRUMB_N];
static uint8_t  g_crumb_count = 0;
static uint8_t  g_crumb_head  = 0;
static uint32_t g_crumb_step_marker = 0;

// Pace counter (Ranger 100m)
static uint32_t g_pace_count = 0;
static uint32_t g_pace_hundreds = 0;

// Stride calibration
static bool     g_stride_cal_active = false;
static uint32_t g_stride_cal_start_steps = 0;

// Camo theme (0=RED, 1=GREEN, 2=AMBER)
static uint8_t  g_theme = 0;

static void hist_load() {
  g_steps_prefs.begin("tacticalos", true);
  g_steps      = g_steps_prefs.getULong ("steps", 0);
  g_steps_yday = g_steps_prefs.getUShort("sd",    0xFFFF);
  g_stride_m   = g_steps_prefs.getFloat ("stride", 0.75f);
  for (int i = 0; i < 7; i++) {
    char k[6]; snprintf(k, sizeof(k), "h%d", i);
    g_steps_hist[i] = g_steps_prefs.getULong(k, 0);
  }
  g_steps_prefs.end();
}
static void hist_save() {
  g_steps_prefs.begin("tacticalos", false);
  g_steps_prefs.putULong ("steps", g_steps);
  g_steps_prefs.putUShort("sd",    g_steps_yday);
  g_steps_prefs.putFloat ("stride",g_stride_m);
  for (int i = 0; i < 7; i++) {
    char k[6]; snprintf(k, sizeof(k), "h%d", i);
    g_steps_prefs.putULong(k, g_steps_hist[i]);
  }
  g_steps_prefs.end();
}
static void steps_rollover_check() {
  struct tm tm;
  if (!getLocalTime(&tm, 5)) return;
  uint16_t yd = tm.tm_yday;
  if (g_steps_yday == 0xFFFF) { g_steps_yday = yd; hist_save(); return; }
  if (yd != g_steps_yday) {
    // shift history
    for (int i = 6; i > 0; i--) g_steps_hist[i] = g_steps_hist[i-1];
    g_steps_hist[0] = g_steps;
    g_steps      = 0;
    g_steps_yday = yd;
    hist_save();
  }
}
static void update_steps() {
  AccG a = qmi_acc();
  float mag = sqrtf(a.x*a.x + a.y*a.y + a.z*a.z);

  // Running mean tracks gravity DC ~0.95s window @50Hz -> AC = mag - mean.
  // Slow enough that walking peaks survive, fast enough to absorb tilt.
  g_acc_mean = 0.95f * g_acc_mean + 0.05f * mag;
  float ac = mag - g_acc_mean;

  // Panic-tap detection (sharp spikes > ~0.6 g on either side, x3 quick).
  if (g_panic_armed && fabsf(ac) > 0.6f) {
    uint32_t now = millis();
    if (g_tap_idx == 0 || (now - g_tap_times[g_tap_idx-1]) > 80) {
      g_tap_times[g_tap_idx++] = now;
      if (g_tap_idx >= 3) {
        if ((g_tap_times[2] - g_tap_times[0]) < 1200) {
          extern void panic_trigger();
          panic_trigger();
        }
        g_tap_idx = 0;
      }
    }
  }
  if (g_tap_idx > 0 && (millis() - g_tap_times[g_tap_idx-1]) > 1500) g_tap_idx = 0;

  // Peak detector (positive-going) with refractory.
  //   * peak when previous AC sample was lower AND current is above threshold
  //   * 0.18 g threshold rejects idle hand wobble (~0.03-0.08 g)
  //   * 280 ms gap = max ~3.5 steps/sec (fastest sustainable cadence)
  uint32_t now = millis();
  bool peak = (g_acc_prev_ac < ac) && (ac > 0.18f);
  g_acc_prev_ac = ac;

  if (peak && (now - g_last_step_ms) > 280) {
    g_last_step_ms = now;
    g_steps++;
    g_steps_hist[0] = g_steps;
    // dead-reckoning
    float rad = g_heading_deg * M_PI / 180.0f;
    g_pos_x_m += g_stride_m * sinf(rad);
    g_pos_y_m += g_stride_m * cosf(rad);
    g_trip_m  += g_stride_m;
    // breadcrumbs every 5 steps
    if (g_steps - g_crumb_step_marker >= 5) {
      g_crumb_step_marker = g_steps;
      g_crumb_x[g_crumb_head] = g_pos_x_m;
      g_crumb_y[g_crumb_head] = g_pos_y_m;
      g_crumb_head = (g_crumb_head + 1) % CRUMB_N;
      if (g_crumb_count < CRUMB_N) g_crumb_count++;
    }
    if ((g_steps & 0x1F) == 0) hist_save();
  }
}

// =============================================================================
// Waypoints (8 slots in NVS) -- dead-reckoning bearing-back
// =============================================================================
struct Waypoint { bool used; float x; float y; char name[12]; };
static Waypoint g_wp[8];
static Preferences g_wp_prefs;

static void wp_load() {
  g_wp_prefs.begin("tac-wp", true);
  for (int i = 0; i < 8; i++) {
    char k[8];
    snprintf(k, sizeof(k), "u%d", i); g_wp[i].used = g_wp_prefs.getBool(k, false);
    snprintf(k, sizeof(k), "x%d", i); g_wp[i].x    = g_wp_prefs.getFloat(k, 0);
    snprintf(k, sizeof(k), "y%d", i); g_wp[i].y    = g_wp_prefs.getFloat(k, 0);
    snprintf(k, sizeof(k), "n%d", i);
    String n = g_wp_prefs.getString(k, "");
    strncpy(g_wp[i].name, n.c_str(), 11); g_wp[i].name[11] = 0;
  }
  g_wp_prefs.end();
}
static void wp_save(int i) {
  g_wp_prefs.begin("tac-wp", false);
  char k[8];
  snprintf(k, sizeof(k), "u%d", i); g_wp_prefs.putBool  (k, g_wp[i].used);
  snprintf(k, sizeof(k), "x%d", i); g_wp_prefs.putFloat (k, g_wp[i].x);
  snprintf(k, sizeof(k), "y%d", i); g_wp_prefs.putFloat (k, g_wp[i].y);
  snprintf(k, sizeof(k), "n%d", i); g_wp_prefs.putString(k, g_wp[i].name);
  g_wp_prefs.end();
}
static int wp_first_free() {
  for (int i = 0; i < 8; i++) if (!g_wp[i].used) return i;
  return -1;
}

// =============================================================================
// Tactical palette
// =============================================================================
#define T_BG       0x000000
#define T_SURFACE  0x0A0000
#define T_BORDER   0x2A0000
#define T_RED      0xFF3030
#define T_DIM_RED  0x8A1010
#define T_AMBER    0xFFB300
#define T_CYAN     0x00E5FF
#define T_TXT      0xE0C8C8
#define T_MUTE     0x553333
#define T_OK       0x39FF7A
#define T_DANGER   0xFF4040

// =============================================================================
// Mode state
// =============================================================================
enum Mode { M_HOME = 0, M_COMPASS, M_NAV, M_TOOLS, M_CAMP, M_WALK, M_DATA, M_BATT, M_NTABS };
static Mode g_mode = M_HOME;
static bool g_night          = false;
static bool g_stealth        = false;
static bool g_adaptive_night = true;
static bool g_flashlight     = false;
static bool g_sos_active     = false;
static uint8_t g_beacon_mode = 0;     // 0=SOS, 1=MAYDAY, 2=STROBE
static uint32_t g_sos_t      = 0;
static uint8_t  g_watchface  = 0;     // 0=DIGITAL 1=RADAR 2=MINIMAL

// SOS Morse (... --- ...)
static const uint16_t SOS_PATTERN[] = {
  200, 200, 200, 200, 200, 500,
  600, 200, 600, 200, 600, 500,
  200, 200, 200, 200, 200, 1200
};
#define SOS_LEN (sizeof(SOS_PATTERN)/sizeof(SOS_PATTERN[0]))

// MAYDAY: M(-- ) A(.-) Y(-.--) D(-..) A(.-) Y(-.--)
static const uint16_t MAYDAY_PATTERN[] = {
  600,150, 600,400,                                    // M
  200,150, 600,400,                                    // A
  600,150, 200,150, 600,150, 600,400,                  // Y
  600,150, 200,150, 200,400,                           // D
  200,150, 600,400,                                    // A
  600,150, 200,150, 600,150, 600,1200                  // Y
};
#define MAYDAY_LEN (sizeof(MAYDAY_PATTERN)/sizeof(MAYDAY_PATTERN[0]))

// Disco strobe
static const uint16_t STROBE_PATTERN[] = { 80,80,80,80,80,80,80,500 };
#define STROBE_LEN (sizeof(STROBE_PATTERN)/sizeof(STROBE_PATTERN[0]))

static uint8_t  g_sos_idx = 0;

// Mission timer / countdown
static uint32_t g_timer_start    = 0;
static uint32_t g_timer_lap_t    = 0;
static uint32_t g_lap_times[8]   = {0};
static uint8_t  g_lap_count      = 0;
static uint32_t g_countdown_end  = 0;   // 0 = inactive
// v2.5 -- Full-screen Stopwatch (independent state; own lap list, centiseconds)
static uint32_t g_sw_start    = 0;      // millis at last (re)start
static uint32_t g_sw_accum    = 0;      // ms accumulated while paused
static bool     g_sw_run      = false;
static uint32_t g_sw_lap_mark = 0;      // elapsed at last lap
static uint32_t g_sw_laps[16] = {0};
static uint8_t  g_sw_lapn     = 0;
static uint32_t g_countdown_set  = 0;

// Morse keyer state
static String   g_morse_buf      = "";
static String   g_morse_text     = "";
static bool     g_morse_playing  = false;
static uint32_t g_morse_play_t   = 0;
static uint16_t g_morse_play_pat[256];
static uint16_t g_morse_play_len = 0;
static uint16_t g_morse_play_idx = 0;
static uint32_t g_keyer_press_t  = 0;
static uint32_t g_keyer_release_t= 0;

// Morse lookup table
static const char* MORSE_TBL[36] = {
  ".-","-...","-.-.","-..",".","..-.","--.","....","..",".---","-.-",".-..",
  "--","-.","---",".--.","--.-",".-.","...","-","..-","...-",".--","-..-",
  "-.--","--..",
  "-----",".----","..---","...--","....-",".....","-....","--...","---..","----."
};

// =============================================================================
// UI globals
// =============================================================================
static lv_obj_t *scr_root;
// v2.5: lightweight pager (no lv_tabview -- some lv_conf builds disable it).
static lv_obj_t *tab_home, *tab_compass, *tab_nav, *tab_tools, *tab_camp, *tab_walk, *tab_data, *tab_batt;
static lv_obj_t *g_tabs[M_NTABS] = {0};
static uint8_t   g_active_tab = 0;
static void update_page_dots(int active);   // fwd-decl (defined near build_ui)
static void set_active_tab(int idx);        // fwd-decl (defined near build_ui)
static lv_obj_t *flash_scr;
// v2.5 stopwatch overlay widgets
static lv_obj_t *g_sw_overlay = nullptr;
static lv_obj_t *g_sw_time_lbl = nullptr;
static lv_obj_t *g_sw_lap_box = nullptr;
static lv_obj_t *g_sw_ss_lbl = nullptr;
static lv_timer_t *g_sw_tick = nullptr;
static lv_obj_t *toast_lbl;
static lv_obj_t *page_dots[M_NTABS] = {0};
// v2.5 accessibility: always-visible current-tab name + swipe affordance arrows.
static lv_obj_t *tab_name_lbl = nullptr;
static const char *kTabNames[M_NTABS] = {
  "HOME", "COMPASS", "NAV", "TOOLS", "CAMP", "WALK", "DATA", "POWER"
};

// PIN lock overlay
static lv_obj_t *pin_overlay = nullptr;
static lv_obj_t *pin_dots[4] = {0};
static lv_obj_t *pin_msg = nullptr;
static char      g_pin_buf[5] = "";
static uint8_t   g_pin_len    = 0;
static bool      g_locked     = false;   // v2.3: PIN lock removed -- always unlocked

// Home widgets (multiple watchfaces)
static lv_obj_t *home_face_digital, *home_face_radar, *home_face_minimal;
static lv_obj_t *home_clock_d, *home_date_d, *home_batt_d, *home_status_d, *home_sync_d;
static lv_obj_t *home_radar_canvas, *home_radar_time;
static lv_obj_t *home_min_time, *home_min_label;
#define HFACE_W 360
#define HFACE_H 360
static lv_color_t *home_radar_buf = nullptr;

// Compass
static lv_obj_t *cmp_canvas, *cmp_heading_lbl, *cmp_tilt_lbl, *cmp_slope_lbl, *cmp_zero_btn;
static lv_obj_t *cmp_bubble; // bubble level dot overlay
#define CMP_W 360
#define CMP_H 360
static lv_color_t *cmp_buf = nullptr;

// NAV widgets
static lv_obj_t *nav_canvas, *nav_list, *nav_bearing_lbl, *nav_dist_lbl, *nav_name_lbl;
static lv_obj_t *nav_mark_btn, *nav_clear_btn, *nav_trip_lbl;
static int       g_nav_selected = -1;
#define NAV_W 360
#define NAV_H 220
static lv_color_t *nav_buf = nullptr;

// Tools
static lv_obj_t *tools_stealth_sw, *tools_night_sw;
static lv_obj_t *tools_timer_lbl, *tools_lap_list, *tools_countdown_lbl;
static lv_obj_t *tools_cd_min_lbl;
static int       g_cd_set_min = 5;
static lv_obj_t *tools_morse_input, *tools_morse_preview;

// Camp
static lv_obj_t *camp_sunrise, *camp_sunset, *camp_daylight, *camp_batt_hours,
                *camp_temp, *camp_steps;

// Walk
static lv_obj_t *walk_time, *walk_steps, *walk_heading, *walk_dist;

// Data (journal + stats)
static lv_obj_t *data_graph_canvas, *data_graph_lbl;
static lv_obj_t *data_journal_lbl, *data_journal_log;
#define GRAPH_W 360
#define GRAPH_H 160
static lv_color_t *graph_buf = nullptr;

// Back-to-launcher pill
static lv_obj_t *back_btn;

// v2.2 Settings card (PIN toggle) -- file-scope so the toggle callback
// can refresh the ARMED/DISARMED label without LVGL class-introspection.
static lv_obj_t *s_pin_state_lbl = nullptr;

// Glitch transition overlay
static lv_obj_t *glitch_overlay = nullptr;
static uint32_t  g_glitch_until  = 0;

// =============================================================================
// Toast helper
// =============================================================================
static uint32_t g_toast_hide_at = 0;
static void show_toast(const char *msg, uint32_t ms) {
  if (!toast_lbl) return;
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
// Theme application
// =============================================================================
static uint32_t accent_color() {
  if (g_stealth) return T_DIM_RED;
  // theme palettes: bright accent for normal, dim for night
  static const uint32_t accents_bright[3] = { 0xFF3030, 0x39FF7A, 0xFFB300 };
  static const uint32_t accents_dim[3]    = { 0x8A1010, 0x108A30, 0x8A6010 };
  return g_night ? accents_dim[g_theme] : accents_bright[g_theme];
}
static uint32_t text_color() {
  if (g_stealth) return 0x4A0000;
  static const uint32_t texts_night[3] = { 0xC07060, 0x60C080, 0xC09060 };
  if (g_night) return texts_night[g_theme];
  return T_TXT;
}
static void apply_theme() {
  uint32_t a = accent_color(), t = text_color();
  if (home_clock_d)  lv_obj_set_style_text_color(home_clock_d,  lv_color_hex(t), 0);
  if (home_date_d)   lv_obj_set_style_text_color(home_date_d,   lv_color_hex(g_stealth ? 0x300000 : 0x7A5050), 0);
  if (home_batt_d)   lv_obj_set_style_text_color(home_batt_d,   lv_color_hex(a), 0);
  if (home_status_d) lv_obj_set_style_text_color(home_status_d, lv_color_hex(a), 0);
  if (home_min_time) lv_obj_set_style_text_color(home_min_time, lv_color_hex(t), 0);
  uint8_t br = g_stealth ? 12 : (g_night ? 40 : 180);
  co5300_set_brightness(br);
}

// Adaptive night
static void adaptive_night_tick() {
  if (!g_adaptive_night || g_stealth) return;
  struct tm tm;
  if (!getLocalTime(&tm, 5)) return;
  bool should = (tm.tm_hour >= 19 || tm.tm_hour < 6);
  if (should != g_night) {
    g_night = should;
    apply_theme();
    show_toast(should ? "NIGHT MODE: AUTO" : "DAY MODE: AUTO", 1500);
  }
}

// =============================================================================
// Sunrise / sunset
// =============================================================================
static void sunrise_sunset(int yy, int mm, int dd, float lat, float lon, int tz_h,
                           int *sr_h, int *sr_m, int *ss_h, int *ss_m) {
  static const int doy_off[] = {0,31,59,90,120,151,181,212,243,273,304,334};
  int N = doy_off[mm-1] + dd;
  if (mm > 2 && (yy % 4 == 0 && (yy % 100 != 0 || yy % 400 == 0))) N++;
  float gamma = 2.0f * M_PI / 365.0f * (N - 1);
  float decl  = 0.006918f
              - 0.399912f*cosf(gamma) + 0.070257f*sinf(gamma)
              - 0.006758f*cosf(2*gamma) + 0.000907f*sinf(2*gamma);
  float latR = lat * M_PI / 180.0f;
  float cosH = (cosf(90.833f*M_PI/180.0f) - sinf(latR)*sinf(decl))
             / (cosf(latR)*cosf(decl));
  if (cosH >  1.0f) { *sr_h=*sr_m=*ss_h=*ss_m = -1; return; }
  if (cosH < -1.0f) { *sr_h=*sr_m=*ss_h=*ss_m = -2; return; }
  float H = acosf(cosH) * 180.0f / M_PI;
  float sr = 12.0f - H/15.0f - lon/15.0f + tz_h;
  float ss = 12.0f + H/15.0f - lon/15.0f + tz_h;
  while (sr < 0) sr += 24; while (sr >= 24) sr -= 24;
  while (ss < 0) ss += 24; while (ss >= 24) ss -= 24;
  *sr_h = (int)sr; *sr_m = (int)((sr - *sr_h) * 60.0f);
  *ss_h = (int)ss; *ss_m = (int)((ss - *ss_h) * 60.0f);
}

// =============================================================================
// SD logging helpers
// =============================================================================
static void log_event(const char *evt) {
  struct tm tm; if (!getLocalTime(&tm, 5)) return;
  char path[64];
  snprintf(path, sizeof(path), "/WATCH/tactical/logs/%04d-%02d-%02d.txt",
           tm.tm_year+1900, tm.tm_mon+1, tm.tm_mday);
  File f = SD_MMC.open(path, FILE_APPEND);
  if (!f) return;
  char ts[16]; strftime(ts, sizeof(ts), "[%H:%M:%S] ", &tm);
  f.print(ts); f.println(evt);
  f.close();
}

static void journal_append(const char *tag, const char *body) {
  struct tm tm; if (!getLocalTime(&tm, 5)) {
    File f = SD_MMC.open("/WATCH/tactical/journal.txt", FILE_APPEND);
    if (f) { f.printf("[--] %s :: %s\n", tag, body); f.close(); }
    return;
  }
  File f = SD_MMC.open("/WATCH/tactical/journal.txt", FILE_APPEND);
  if (!f) return;
  char ts[32]; strftime(ts, sizeof(ts), "[%Y-%m-%d %H:%M]", &tm);
  f.printf("%s [%s] %s\n", ts, tag, body);
  f.close();
}

// Panic trigger (called from update_steps detector)
void panic_trigger() {
  static uint32_t last = 0;
  if (millis() - last < 3000) return;
  last = millis();
  char buf[64];
  snprintf(buf, sizeof(buf), "PANIC heading=%03d steps=%lu",
           (int)g_heading_deg, g_steps);
  log_event(buf);
  journal_append("PANIC", buf);
  show_toast("[!] PANIC LOGGED", 2000);
}

// =============================================================================
// Compass canvas + bubble level
// =============================================================================
static void draw_compass() {
  if (!cmp_canvas) return;
  lv_canvas_fill_bg(cmp_canvas, lv_color_hex(T_BG), LV_OPA_COVER);
  uint32_t col = accent_color();
  int cx = CMP_W/2, cy = CMP_H/2;

  lv_draw_arc_dsc_t arc; lv_draw_arc_dsc_init(&arc);
  arc.color = lv_color_hex(g_stealth ? 0x300000 : T_BORDER);
  arc.width = 1;
  lv_canvas_draw_arc(cmp_canvas, cx, cy, 170, 0, 360, &arc);
  lv_canvas_draw_arc(cmp_canvas, cx, cy, 120, 0, 360, &arc);
  lv_canvas_draw_arc(cmp_canvas, cx, cy, 70,  0, 360, &arc);

  lv_draw_line_dsc_t ld; lv_draw_line_dsc_init(&ld);
  ld.color = lv_color_hex(col); ld.width = 2;
  for (int i = 0; i < 360; i += 30) {
    float a = (i - g_heading_deg) * M_PI / 180.0f;
    int r1 = (i % 90 == 0) ? 150 : 165;
    int r2 = 170;
    lv_point_t pts[2] = {
      { (lv_coord_t)(cx + r1*sinf(a)), (lv_coord_t)(cy - r1*cosf(a)) },
      { (lv_coord_t)(cx + r2*sinf(a)), (lv_coord_t)(cy - r2*cosf(a)) }
    };
    lv_canvas_draw_line(cmp_canvas, pts, 2, &ld);
  }
  // North needle
  {
    float a = (-g_heading_deg) * M_PI / 180.0f;
    lv_draw_line_dsc_t ld2; lv_draw_line_dsc_init(&ld2);
    ld2.color = lv_color_hex(T_RED); ld2.width = 3;
    lv_point_t pts[2] = {
      { (lv_coord_t)cx, (lv_coord_t)cy },
      { (lv_coord_t)(cx + 140*sinf(a)), (lv_coord_t)(cy - 140*cosf(a)) }
    };
    lv_canvas_draw_line(cmp_canvas, pts, 2, &ld2);
  }
  // Radar sweep
  {
    static int sweep = 0;
    sweep = (sweep + 4) % 360;
    float a = sweep * M_PI / 180.0f;
    lv_draw_line_dsc_t ld3; lv_draw_line_dsc_init(&ld3);
    ld3.color = lv_color_hex(g_stealth ? 0x300000 : T_DIM_RED); ld3.width = 1;
    lv_point_t pts[2] = {
      { (lv_coord_t)cx, (lv_coord_t)cy },
      { (lv_coord_t)(cx + 165*sinf(a)), (lv_coord_t)(cy - 165*cosf(a)) }
    };
    lv_canvas_draw_line(cmp_canvas, pts, 2, &ld3);
    // 3 fake blips on the sweep line (decorative target indicators)
    lv_draw_rect_dsc_t bd; lv_draw_rect_dsc_init(&bd);
    bd.bg_color = lv_color_hex(T_AMBER); bd.bg_opa = LV_OPA_COVER; bd.radius = 2;
    int blip_r[3] = {60, 100, 140};
    int blip_off[3] = {0, 90, 200};
    for (int i = 0; i < 3; i++) {
      int bs = (sweep + blip_off[i]) % 360;
      float ba = bs * M_PI / 180.0f;
      int bx = cx + blip_r[i]*sinf(ba) - 2;
      int by = cy - blip_r[i]*cosf(ba) - 2;
      lv_canvas_draw_rect(cmp_canvas, bx, by, 4, 4, &bd);
    }
  }
  // Bubble level (accel-driven)
  AccG g = qmi_acc();
  int bx = cx + (int)(g.x * 150.0f);
  int by = cy + (int)(g.y * 150.0f);
  if (bx < cx-150) bx = cx-150; if (bx > cx+150) bx = cx+150;
  if (by < cy-150) by = cy-150; if (by > cy+150) by = cy+150;
  lv_draw_rect_dsc_t lvl; lv_draw_rect_dsc_init(&lvl);
  bool level = (fabsf(g.x) < 0.05f && fabsf(g.y) < 0.05f);
  lvl.bg_color = lv_color_hex(level ? T_OK : T_AMBER);
  lvl.bg_opa = LV_OPA_COVER; lvl.radius = 7;
  lv_canvas_draw_rect(cmp_canvas, bx-7, by-7, 14, 14, &lvl);

  // Center dot
  lv_draw_rect_dsc_t rd; lv_draw_rect_dsc_init(&rd);
  rd.bg_color = lv_color_hex(T_RED); rd.bg_opa = LV_OPA_COVER; rd.radius = 4;
  lv_canvas_draw_rect(cmp_canvas, cx-3, cy-3, 6, 6, &rd);
}

// =============================================================================
// NAV canvas (mini radar with waypoint blips)
// =============================================================================
static void draw_nav() {
  if (!nav_canvas) return;
  lv_canvas_fill_bg(nav_canvas, lv_color_hex(T_BG), LV_OPA_COVER);
  int cx = NAV_W/2, cy = NAV_H/2;

  lv_draw_arc_dsc_t arc; lv_draw_arc_dsc_init(&arc);
  arc.color = lv_color_hex(T_BORDER); arc.width = 1;
  lv_canvas_draw_arc(nav_canvas, cx, cy, 90, 0, 360, &arc);
  lv_canvas_draw_arc(nav_canvas, cx, cy, 60, 0, 360, &arc);
  lv_canvas_draw_arc(nav_canvas, cx, cy, 30, 0, 360, &arc);

  // Find max distance for scaling
  float maxd = 10.0f;
  for (int i = 0; i < 8; i++) if (g_wp[i].used) {
    float dx = g_wp[i].x - g_pos_x_m;
    float dy = g_wp[i].y - g_pos_y_m;
    float d  = sqrtf(dx*dx + dy*dy);
    if (d > maxd) maxd = d;
  }
  float scale = 85.0f / maxd;

  // Heading arrow (where I'm facing -- up direction of HUD)
  lv_draw_line_dsc_t hd; lv_draw_line_dsc_init(&hd);
  hd.color = lv_color_hex(T_DIM_RED); hd.width = 2;
  lv_point_t hp[2] = { {(lv_coord_t)cx, (lv_coord_t)cy},
                       {(lv_coord_t)cx, (lv_coord_t)(cy - 95)} };
  lv_canvas_draw_line(nav_canvas, hp, 2, &hd);

  // Waypoint blips (rotated by heading so up = forward)
  lv_draw_rect_dsc_t bd; lv_draw_rect_dsc_init(&bd);
  bd.bg_color = lv_color_hex(T_AMBER); bd.bg_opa = LV_OPA_COVER; bd.radius = 4;
  lv_draw_rect_dsc_t sd; lv_draw_rect_dsc_init(&sd);
  sd.bg_color = lv_color_hex(T_RED); sd.bg_opa = LV_OPA_COVER; sd.radius = 5;
  float hrad = -g_heading_deg * M_PI / 180.0f;
  float cs = cosf(hrad), sn = sinf(hrad);

  // Breadcrumbs — fade from dim to bright (oldest -> newest)
  lv_draw_rect_dsc_t crd; lv_draw_rect_dsc_init(&crd);
  crd.bg_opa = LV_OPA_COVER; crd.radius = 1;
  for (uint8_t i = 0; i < g_crumb_count; i++) {
    uint8_t idx = (g_crumb_head + CRUMB_N - g_crumb_count + i) % CRUMB_N;
    float dx = (g_crumb_x[idx] - g_pos_x_m) * scale;
    float dy = (g_crumb_y[idx] - g_pos_y_m) * scale;
    float rx = dx * cs - dy * sn;
    float ry = dx * sn + dy * cs;
    int px = cx + (int)rx;
    int py = cy - (int)ry;
    if (px < 0 || px >= NAV_W-2 || py < 0 || py >= NAV_H-2) continue;
    uint8_t age = (g_crumb_count - i);
    uint32_t col = (age < 10) ? T_AMBER : T_DIM_RED;
    crd.bg_color = lv_color_hex(col);
    lv_canvas_draw_rect(nav_canvas, px-1, py-1, 2, 2, &crd);
  }

  for (int i = 0; i < 8; i++) if (g_wp[i].used) {
    float dx = (g_wp[i].x - g_pos_x_m) * scale;
    float dy = (g_wp[i].y - g_pos_y_m) * scale;
    // rotate -heading so "up" is forward
    float rx = dx * cs - dy * sn;
    float ry = dx * sn + dy * cs;
    int px = cx + (int)rx;
    int py = cy - (int)ry;
    if (i == g_nav_selected) lv_canvas_draw_rect(nav_canvas, px-5, py-5, 10, 10, &sd);
    else                     lv_canvas_draw_rect(nav_canvas, px-3, py-3, 6, 6,  &bd);
  }
  // self dot
  lv_draw_rect_dsc_t cd; lv_draw_rect_dsc_init(&cd);
  cd.bg_color = lv_color_hex(T_CYAN); cd.bg_opa = LV_OPA_COVER; cd.radius = 3;
  lv_canvas_draw_rect(nav_canvas, cx-3, cy-3, 6, 6, &cd);
}

static void update_nav_labels() {
  if (!nav_bearing_lbl) return;
  if (g_nav_selected < 0 || !g_wp[g_nav_selected].used) {
    lv_label_set_text(nav_bearing_lbl, "---°");
    lv_label_set_text(nav_dist_lbl, "---- m");
    lv_label_set_text(nav_name_lbl, "no target");
  } else {
    float dx = g_wp[g_nav_selected].x - g_pos_x_m;
    float dy = g_wp[g_nav_selected].y - g_pos_y_m;
    float d  = sqrtf(dx*dx + dy*dy);
    float b  = atan2f(dx, dy) * 180.0f / M_PI;
    if (b < 0) b += 360.0f;
    // relative bearing = absolute bearing - heading
    float rel = b - g_heading_deg;
    while (rel < 0) rel += 360.0f; while (rel >= 360.0f) rel -= 360.0f;
    char hb[24]; snprintf(hb, sizeof(hb), "%03d°", (int)rel);
    lv_label_set_text(nav_bearing_lbl, hb);
    char db[24]; snprintf(db, sizeof(db), "%d m", (int)d);
    lv_label_set_text(nav_dist_lbl, db);
    lv_label_set_text(nav_name_lbl, g_wp[g_nav_selected].name);
  }
  if (nav_trip_lbl) {
    char tb[32]; snprintf(tb, sizeof(tb), "TRIP %.1f km", g_trip_m / 1000.0f);
    lv_label_set_text(nav_trip_lbl, tb);
  }
}

// =============================================================================
// 7-day step graph canvas
// =============================================================================
static void draw_graph() {
  if (!data_graph_canvas) return;
  lv_canvas_fill_bg(data_graph_canvas, lv_color_hex(T_BG), LV_OPA_COVER);
  uint32_t mx = 1;
  for (int i = 0; i < 7; i++) if (g_steps_hist[i] > mx) mx = g_steps_hist[i];
  int bar_w = 36;
  int gap   = 12;
  int x0    = 20;
  int y_bot = GRAPH_H - 30;
  int y_top = 10;
  lv_draw_rect_dsc_t bd; lv_draw_rect_dsc_init(&bd);
  bd.bg_opa = LV_OPA_COVER; bd.radius = 3;
  for (int i = 0; i < 7; i++) {
    int idx = i;  // 0 = today (leftmost on chart? let's make rightmost = today)
    int day = 6 - i; // i=0 -> leftmost = 6 days ago, i=6 -> rightmost = today
    int bx = x0 + i * (bar_w + gap);
    int h  = (int)((y_bot - y_top) * (g_steps_hist[day] / (float)mx));
    int by = y_bot - h;
    bd.bg_color = lv_color_hex(day == 0 ? T_RED : T_DIM_RED);
    lv_canvas_draw_rect(data_graph_canvas, bx, by, bar_w, h, &bd);
  }
  // baseline
  lv_draw_line_dsc_t ld; lv_draw_line_dsc_init(&ld);
  ld.color = lv_color_hex(T_BORDER); ld.width = 1;
  lv_point_t lp[2] = {{(lv_coord_t)10,(lv_coord_t)(y_bot+1)},
                      {(lv_coord_t)(GRAPH_W-10),(lv_coord_t)(y_bot+1)}};
  lv_canvas_draw_line(data_graph_canvas, lp, 2, &ld);
}

// =============================================================================
// Event callbacks
// =============================================================================
static uint32_t g_back_press_t = 0;
static void back_btn_cb(lv_event_t *e) {
  lv_event_code_t c = lv_event_get_code(e);
  if (c == LV_EVENT_PRESSED) g_back_press_t = millis();
  else if (c == LV_EVENT_LONG_PRESSED) return_to_launcher();
  else if (c == LV_EVENT_RELEASED) {
    uint32_t held = g_back_press_t ? (millis() - g_back_press_t) : 0;
    g_back_press_t = 0;
    if (held > 0 && held < 600) show_toast("Hold to exit to Launcher", 1500);
  }
}

static void flash_on();
static void flash_off();

static void tools_flash_cb(lv_event_t *) { if (!g_flashlight) flash_on(); else flash_off(); }
static void tools_sos_cb(lv_event_t *)   {
  g_sos_active = !g_sos_active;
  if (g_sos_active) { g_sos_t = millis(); g_sos_idx = 0;
    const char* names[] = {"SOS","MAYDAY","STROBE"};
    char m[24]; snprintf(m, sizeof(m), "%s ARMED", names[g_beacon_mode]);
    show_toast(m, 1500);
    log_event(m);
  } else { flash_off(); show_toast("BEACON OFF", 1200); }
}
static void tools_beacon_mode_cb(lv_event_t *) {
  g_beacon_mode = (g_beacon_mode + 1) % 3;
  const char* names[] = {"SOS","MAYDAY","STROBE"};
  char m[40]; snprintf(m, sizeof(m), "BEACON: %s", names[g_beacon_mode]);
  show_toast(m, 1200);
}
static void tools_night_cb(lv_event_t *e) {
  g_night = lv_obj_has_state((lv_obj_t*)lv_event_get_target(e), LV_STATE_CHECKED);
  g_adaptive_night = false;
  apply_theme();
}
static void tools_stealth_cb(lv_event_t *e) {
  g_stealth = lv_obj_has_state((lv_obj_t*)lv_event_get_target(e), LV_STATE_CHECKED);
  apply_theme();
}
static void cmp_zero_cb(lv_event_t *) { heading_zero(); show_toast("Heading zeroed", 1000); }

// Timer callbacks
static void timer_start_cb(lv_event_t *) {
  if (!g_timer_start) {
    g_timer_start = millis();
    g_timer_lap_t = g_timer_start;
    g_lap_count = 0;
    show_toast("Timer started", 1000);
  } else {
    char m[40]; snprintf(m, sizeof(m), "Stopped @ %lus", (millis()-g_timer_start)/1000);
    log_event(m); show_toast(m, 1500);
    g_timer_start = 0;
  }
}
static void timer_lap_cb(lv_event_t *) {
  if (!g_timer_start || g_lap_count >= 8) return;
  uint32_t now = millis();
  g_lap_times[g_lap_count++] = now - g_timer_lap_t;
  g_timer_lap_t = now;
  char m[40]; snprintf(m, sizeof(m), "LAP %d  +%lus", g_lap_count, g_lap_times[g_lap_count-1]/1000);
  show_toast(m, 1500);
}
static void cd_inc_cb(lv_event_t *) { g_cd_set_min += 1; if (g_cd_set_min > 99) g_cd_set_min = 1; }
static void cd_dec_cb(lv_event_t *) { g_cd_set_min -= 1; if (g_cd_set_min < 1)  g_cd_set_min = 99; }
static void cd_start_cb(lv_event_t *) {
  if (!g_countdown_end) {
    g_countdown_end = millis() + (uint32_t)g_cd_set_min * 60 * 1000;
    g_countdown_set = g_countdown_end;
    char m[32]; snprintf(m, sizeof(m), "Countdown %d min", g_cd_set_min);
    show_toast(m, 1200);
  } else {
    g_countdown_end = 0;
    show_toast("Countdown cleared", 1000);
  }
}

// Morse keyer
static void play_morse(const char *s);

static void keyer_press_cb(lv_event_t *) { g_keyer_press_t = millis(); }
static void keyer_release_cb(lv_event_t *) {
  if (!g_keyer_press_t) return;
  uint32_t held = millis() - g_keyer_press_t;
  g_keyer_release_t = millis();
  if (held < 250)      g_morse_buf += ".";
  else                 g_morse_buf += "-";
  if (g_morse_buf.length() > 8) g_morse_buf = "";
  // try decode
  for (int i = 0; i < 36; i++) if (g_morse_buf == MORSE_TBL[i]) {
    char c = (i < 26) ? ('A'+i) : ('0' + (i-26));
    g_morse_text += c;
    g_morse_buf = "";
    break;
  }
  if (tools_morse_preview)
    lv_label_set_text_fmt(tools_morse_preview, "%s | %s_",
                          g_morse_text.c_str(), g_morse_buf.c_str());
  g_keyer_press_t = 0;
}
static void keyer_space_cb(lv_event_t *) { g_morse_buf = ""; g_morse_text += " ";
  if (tools_morse_preview) lv_label_set_text(tools_morse_preview, g_morse_text.c_str()); }
static void keyer_clear_cb(lv_event_t *) { g_morse_buf = ""; g_morse_text = "";
  if (tools_morse_preview) lv_label_set_text(tools_morse_preview, "_"); }
static void keyer_send_cb(lv_event_t *) {
  if (g_morse_text.length() == 0) { show_toast("Empty", 800); return; }
  play_morse(g_morse_text.c_str());
  char m[40]; snprintf(m, sizeof(m), "Sending: %s", g_morse_text.c_str());
  show_toast(m, 1500);
  log_event(m);
}

// Build morse pattern (on/off ms) for a string
static void play_morse(const char *s) {
  g_morse_play_len = 0;
  for (const char *p = s; *p && g_morse_play_len < 250; p++) {
    char c = *p; if (c >= 'a' && c <= 'z') c -= 32;
    const char *code = NULL;
    if      (c >= 'A' && c <= 'Z') code = MORSE_TBL[c - 'A'];
    else if (c >= '0' && c <= '9') code = MORSE_TBL[26 + (c - '0')];
    else if (c == ' ') { g_morse_play_pat[g_morse_play_len++] = 0;     // off
                         g_morse_play_pat[g_morse_play_len++] = 700; continue; }
    else continue;
    for (const char *q = code; *q; q++) {
      g_morse_play_pat[g_morse_play_len++] = 1;                                     // on
      g_morse_play_pat[g_morse_play_len++] = (*q == '.') ? 200 : 600;               // dur
      g_morse_play_pat[g_morse_play_len++] = 0;                                     // off
      g_morse_play_pat[g_morse_play_len++] = 200;
    }
    g_morse_play_pat[g_morse_play_len++] = 0;
    g_morse_play_pat[g_morse_play_len++] = 400;
  }
  g_morse_play_idx = 0;
  g_morse_play_t   = millis();
  g_morse_playing  = true;
}
static void tick_morse() {
  if (!g_morse_playing) return;
  if (g_morse_play_idx >= g_morse_play_len) { g_morse_playing = false; flash_off(); return; }
  uint16_t on  = g_morse_play_pat[g_morse_play_idx];
  uint16_t dur = g_morse_play_pat[g_morse_play_idx+1];
  if (millis() - g_morse_play_t >= dur) {
    g_morse_play_t = millis();
    g_morse_play_idx += 2;
    if (g_morse_play_idx >= g_morse_play_len) { g_morse_playing = false; flash_off(); return; }
    if (g_morse_play_pat[g_morse_play_idx] == 1) flash_on();
    else flash_off();
  }
}

// NAV callbacks
static void nav_mark_cb(lv_event_t *) {
  int i = wp_first_free();
  if (i < 0) { show_toast("All slots full", 1200); return; }
  g_wp[i].used = true;
  g_wp[i].x    = g_pos_x_m;
  g_wp[i].y    = g_pos_y_m;
  snprintf(g_wp[i].name, sizeof(g_wp[i].name), "WP-%d", i+1);
  wp_save(i);
  g_nav_selected = i;
  char m[32]; snprintf(m, sizeof(m), "Marked %s", g_wp[i].name);
  show_toast(m, 1200);
  log_event(m);
  if (nav_list) {
    String s = "";
    for (int k = 0; k < 8; k++) if (g_wp[k].used)
      s += String(g_wp[k].name) + "\n"; else s += "[empty]\n";
    lv_dropdown_set_options(nav_list, s.c_str());
    lv_dropdown_set_selected(nav_list, i);
  }
}
static void nav_clear_cb(lv_event_t *) {
  if (g_nav_selected < 0) return;
  g_wp[g_nav_selected].used = false;
  wp_save(g_nav_selected);
  show_toast("Waypoint cleared", 1000);
  g_nav_selected = -1;
}
static void nav_list_cb(lv_event_t *e) {
  int i = lv_dropdown_get_selected((lv_obj_t*)lv_event_get_target(e));
  if (g_wp[i].used) g_nav_selected = i;
}
static void nav_reset_pos_cb(lv_event_t *) {
  g_pos_x_m = 0; g_pos_y_m = 0; g_trip_m = 0;
  g_crumb_count = 0; g_crumb_head = 0; g_crumb_step_marker = 0;
  show_toast("Position reset to origin", 1200);
}

// Pace counter
static lv_obj_t *tools_pace_lbl = nullptr;
static lv_obj_t *tools_pace_hundreds_lbl = nullptr;
static void pace_bump_cb(lv_event_t *) {
  g_pace_count++;
  if (g_pace_count >= 64) {       // ranger pace = ~64 paces per 100m at avg stride
    g_pace_count = 0;
    g_pace_hundreds++;
    show_toast("+100m", 800);
  }
}
static void pace_reset_cb(lv_event_t *) {
  g_pace_count = 0; g_pace_hundreds = 0;
  show_toast("Pace reset", 800);
}

// Theme cycle
static void theme_cycle_cb(lv_event_t *) {
  g_theme = (g_theme + 1) % 3;
  const char *nm[] = {"RED OP","GREEN OP","AMBER CRT"};
  char m[24]; snprintf(m, sizeof(m), "THEME: %s", nm[g_theme]);
  show_toast(m, 1200);
  apply_theme();
}

// Stride auto-calibrate
static void stride_cal_cb(lv_event_t *) {
  if (!g_stride_cal_active) {
    g_stride_cal_active = true;
    g_stride_cal_start_steps = g_steps;
    show_toast("Walk 100m then tap again", 2500);
  } else {
    uint32_t walked = g_steps - g_stride_cal_start_steps;
    if (walked < 30) { show_toast("Need >=30 steps", 1500); g_stride_cal_active = false; return; }
    g_stride_m = 100.0f / (float)walked;
    if (g_stride_m < 0.4f) g_stride_m = 0.4f;
    if (g_stride_m > 1.2f) g_stride_m = 1.2f;
    hist_save();
    char m[40]; snprintf(m, sizeof(m), "Stride=%.2fm (%lu steps)", g_stride_m, walked);
    show_toast(m, 2500);
    log_event(m);
    g_stride_cal_active = false;
  }
}

// Home watchface selection (v2.3)
//   * `home_face_apply(idx)` shows the requested face (0..2), updates the
//     global `g_watchface`, and persists the choice to NVS so the next
//     boot opens to the same face.  Called from the WATCH FACE card in
//     the TOOLS tab only -- there is no more centre-tap shortcut.
//   * `home_face_load_idx()` reads the saved index at boot (default 0).
static uint8_t home_face_load_idx() {
  Preferences p; p.begin("tac-face", true);
  uint8_t idx = p.getUChar("idx", 0);
  p.end();
  if (idx > 2) idx = 0;
  return idx;
}
static void home_face_save_idx(uint8_t idx) {
  Preferences p; p.begin("tac-face", false);
  p.putUChar("idx", idx);
  p.end();
}
static void home_face_apply(uint8_t idx, bool toast = true) {
  if (idx > 2) idx = 0;
  g_watchface = idx;
  lv_obj_add_flag(home_face_digital, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_flag(home_face_radar,   LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_flag(home_face_minimal, LV_OBJ_FLAG_HIDDEN);
  switch (g_watchface) {
    case 0: lv_obj_clear_flag(home_face_digital, LV_OBJ_FLAG_HIDDEN); break;
    case 1: lv_obj_clear_flag(home_face_radar,   LV_OBJ_FLAG_HIDDEN); break;
    case 2: lv_obj_clear_flag(home_face_minimal, LV_OBJ_FLAG_HIDDEN); break;
  }
  home_face_save_idx(g_watchface);
  if (toast) {
    const char* nm[] = {"DIGITAL","RADAR","MINIMAL"};
    char m[32]; snprintf(m, sizeof(m), "FACE: %s", nm[g_watchface]);
    show_toast(m, 1000);
  }
}

// v2.2 -- Tap-edge nav.
//   * Tap on left ~20 % of the screen  -> previous tab (snappy, no anim)
//   * Tap on right ~20 % of the screen -> next tab     (snappy, no anim)
//   * Center band remains the double-tap flashlight zone.
// We read the last-known touch X recorded by my_touchpad_read (g_touch_x).
// Tab change uses LV_ANIM_OFF -> instant switch, same feel as LauncherOS.
#define EDGE_NAV_PX  (LCD_WIDTH / 5)        // ~82 px on a 410 px panel

static void nav_relative(int delta) {
  if (g_locked) return;                     // don't nav while PIN screen up
  int32_t nxt = (int32_t)g_active_tab + delta;
  if (nxt < 0) nxt = M_NTABS - 1;           // wrap edges, feels nicer than clamp
  if (nxt >= M_NTABS) nxt = 0;
  set_active_tab((int)nxt);                  // instant show/hide, no animation
}

static uint32_t g_last_tap_t = 0;
static void scr_clicked_cb(lv_event_t *) {
  uint32_t now = millis();
  int32_t  x   = g_touch_x;

  // Edge tap = nav, never trigger the flashlight double-tap there.
  if (x >= 0 && x < EDGE_NAV_PX)             { nav_relative(-1); g_last_tap_t = 0; return; }
  if (x > LCD_WIDTH - EDGE_NAV_PX && x <= LCD_WIDTH) { nav_relative(+1); g_last_tap_t = 0; return; }

  // Center band: keep the existing double-tap flashlight behaviour.
  if (now - g_last_tap_t < 350) {
    if (!g_flashlight) flash_on(); else flash_off();
    g_last_tap_t = 0;
  } else {
    g_last_tap_t = now;
  }
}

// =============================================================================
// Flashlight
// =============================================================================
static void flash_on() {
  g_flashlight = true;
  co5300_set_brightness(255);
  lv_obj_clear_flag(flash_scr, LV_OBJ_FLAG_HIDDEN);
  lv_obj_move_foreground(flash_scr);
}
static void flash_off() {
  g_flashlight = false;
  lv_obj_add_flag(flash_scr, LV_OBJ_FLAG_HIDDEN);
  apply_theme();
}
static void flash_scr_cb(lv_event_t *) { flash_off(); }

// =============================================================================
// Beacon (SOS / MAYDAY / STROBE)
// =============================================================================
static void tick_beacon() {
  if (!g_sos_active) return;
  const uint16_t *pat;
  uint16_t len;
  switch (g_beacon_mode) {
    case 1: pat = MAYDAY_PATTERN; len = MAYDAY_LEN; break;
    case 2: pat = STROBE_PATTERN; len = STROBE_LEN; break;
    default: pat = SOS_PATTERN;   len = SOS_LEN;    break;
  }
  uint32_t now = millis();
  if (now - g_sos_t >= pat[g_sos_idx]) {
    g_sos_t = now;
    g_sos_idx = (g_sos_idx + 1) % len;
    if (g_sos_idx % 2 == 0) flash_on(); else flash_off();
  }
}

// =============================================================================
// PIN LOCK
// =============================================================================
static String g_stored_pin     = "6030";
static bool   g_pin_enabled    = true;   // v2.2: user-toggleable from Settings
static void pin_load() {
  Preferences p; p.begin("tac-pin", true);
  g_stored_pin  = p.getString("pin", "6030");
  g_pin_enabled = p.getBool  ("enabled", true);
  p.end();
}
static void pin_save_enabled(bool en) {
  g_pin_enabled = en;
  Preferences p; p.begin("tac-pin", false);
  p.putBool("enabled", en);
  p.end();
}

// v2.2 -- Settings card switch callback.  Persists the new state and
// updates the ARMED/DISARMED label on the same card.  The change takes
// effect on the next boot; the current session is unaffected so a user
// who just typed their PIN doesn't get kicked out mid-use.
static void pin_toggle_cb(lv_event_t *e) {
  lv_obj_t *sw = (lv_obj_t *)lv_event_get_target(e);
  bool on = lv_obj_has_state(sw, LV_STATE_CHECKED);
  pin_save_enabled(on);
  if (s_pin_state_lbl) {
    lv_label_set_text(s_pin_state_lbl, on ? "ARMED" : "DISARMED");
    lv_obj_set_style_text_color(s_pin_state_lbl,
        lv_color_hex(on ? T_RED : T_MUTE), 0);
  }
  show_toast(on ? "PIN LOCK ARMED (next boot)" : "PIN LOCK DISARMED", 1600);
  log_event(on ? "PIN ENABLED" : "PIN DISABLED");
}
static void pin_update_dots() {
  for (int i = 0; i < 4; i++) {
    lv_obj_set_style_bg_color(pin_dots[i],
      lv_color_hex(i < g_pin_len ? T_RED : T_DIM_RED), 0);
  }
}
static void pin_btn_cb(lv_event_t *e) {
  const char *txt = (const char *)lv_event_get_user_data(e);
  if (!txt) return;
  if (txt[0] == 'X') {
    if (g_pin_len > 0) { g_pin_len--; g_pin_buf[g_pin_len] = 0; }
  } else if (txt[0] == 'V') {
    if (g_pin_len == 4) {
      if (g_stored_pin == String(g_pin_buf)) {
        g_locked = false;
        lv_obj_add_flag(pin_overlay, LV_OBJ_FLAG_HIDDEN);
        show_toast("ACCESS GRANTED", 1500);
        log_event("PIN OK -> session start");
      } else {
        if (pin_msg) lv_label_set_text(pin_msg, "ACCESS DENIED");
        g_pin_len = 0; g_pin_buf[0] = 0;
        log_event("PIN FAIL");
      }
    }
  } else if (g_pin_len < 4) {
    g_pin_buf[g_pin_len++] = txt[0];
    g_pin_buf[g_pin_len]   = 0;
    if (pin_msg) lv_label_set_text(pin_msg, "ENTER PIN");
  }
  pin_update_dots();
}

static const char* PIN_KEYS[12] = {
  "1","2","3","4","5","6","7","8","9","X","0","V"
};
static void build_pin_overlay() {
  pin_overlay = lv_obj_create(lv_layer_top());
  lv_obj_remove_style_all(pin_overlay);
  lv_obj_set_size(pin_overlay, LCD_WIDTH, LCD_HEIGHT);
  lv_obj_set_style_bg_color(pin_overlay, lv_color_hex(0x000000), 0);
  lv_obj_set_style_bg_opa  (pin_overlay, LV_OPA_COVER, 0);

  lv_obj_t *title = lv_label_create(pin_overlay);
  lv_label_set_text(title, "TACTICAL // LOCK");
  lv_obj_set_style_text_color(title, lv_color_hex(T_RED), 0);
  lv_obj_set_style_text_font (title, &lv_font_montserrat_22, 0);
  lv_obj_set_style_text_letter_space(title, 3, 0);
  lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 36);

  pin_msg = lv_label_create(pin_overlay);
  lv_label_set_text(pin_msg, "ENTER PIN");
  lv_obj_set_style_text_color(pin_msg, lv_color_hex(T_DIM_RED), 0);
  lv_obj_set_style_text_font (pin_msg, &lv_font_montserrat_14, 0);
  lv_obj_align(pin_msg, LV_ALIGN_TOP_MID, 0, 70);

  // 4 dots
  for (int i = 0; i < 4; i++) {
    pin_dots[i] = lv_obj_create(pin_overlay);
    lv_obj_remove_style_all(pin_dots[i]);
    lv_obj_set_size(pin_dots[i], 22, 22);
    lv_obj_set_style_radius(pin_dots[i], 11, 0);
    lv_obj_set_style_bg_color(pin_dots[i], lv_color_hex(T_DIM_RED), 0);
    lv_obj_set_style_bg_opa  (pin_dots[i], LV_OPA_COVER, 0);
    lv_obj_align(pin_dots[i], LV_ALIGN_TOP_MID, -54 + i*36, 100);
  }

  // 3x4 keypad
  int kx = 55, ky = 150, kw = 90, kh = 70, gap = 10;
  for (int i = 0; i < 12; i++) {
    int row = i / 3, col = i % 3;
    lv_obj_t *b = lv_btn_create(pin_overlay);
    lv_obj_set_size(b, kw, kh);
    lv_obj_align(b, LV_ALIGN_TOP_LEFT, kx + col*(kw+gap), ky + row*(kh+gap));
    lv_obj_set_style_bg_color(b, lv_color_hex(T_SURFACE), 0);
    lv_obj_set_style_border_color(b, lv_color_hex(T_DIM_RED), 0);
    lv_obj_set_style_border_width(b, 1, 0);
    lv_obj_set_style_radius(b, 12, 0);
    lv_obj_t *l = lv_label_create(b);
    const char *t = PIN_KEYS[i];
    lv_label_set_text(l, t[0] == 'X' ? LV_SYMBOL_BACKSPACE :
                        t[0] == 'V' ? LV_SYMBOL_OK : t);
    lv_obj_set_style_text_color(l, lv_color_hex(t[0]=='V' ? T_OK :
                                                t[0]=='X' ? T_AMBER : T_RED), 0);
    lv_obj_set_style_text_font(l, &lv_font_montserrat_22, 0);
    lv_obj_center(l);
    lv_obj_add_event_cb(b, pin_btn_cb, LV_EVENT_CLICKED, (void *)PIN_KEYS[i]);
  }

  lv_obj_t *hint = lv_label_create(pin_overlay);
  lv_label_set_text(hint, "default 6030");
  lv_obj_set_style_text_color(hint, lv_color_hex(T_MUTE), 0);
  lv_obj_set_style_text_font (hint, &lv_font_montserrat_12, 0);
  lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -16);
}

// =============================================================================
// Clock / battery / camp / walk updates
// =============================================================================
static char g_last_hhmm[8] = {0};

static void update_clock_labels() {
  struct tm tm;
  if (!getLocalTime(&tm, 5)) {
    if (home_clock_d) lv_label_set_text(home_clock_d, "--:--");
    if (home_date_d)  lv_label_set_text(home_date_d, "no sync");
    if (walk_time)    lv_label_set_text(walk_time, "--:--");
    if (home_radar_time) lv_label_set_text(home_radar_time, "--:--");
    if (home_min_time)   lv_label_set_text(home_min_time, "--:--");
    return;
  }
  char t[8], d[32];
  strftime(t, sizeof(t), "%H:%M", &tm);
  strftime(d, sizeof(d), "%a %d %b // %Y", &tm);
  if (strcmp(t, g_last_hhmm) != 0) {
    strcpy(g_last_hhmm, t);
    if (home_clock_d)    lv_label_set_text(home_clock_d, t);
    if (walk_time)       lv_label_set_text(walk_time, t);
    if (home_radar_time) lv_label_set_text(home_radar_time, t);
    if (home_min_time)   lv_label_set_text(home_min_time, t);
  }
  if (home_date_d) lv_label_set_text(home_date_d, d);
}

static void update_battery_card() {
  if (!g_has_pmu) return;
  int pct = g_pmu.getBatteryPercent();
  bool ch = g_pmu.isCharging();
  char b[24];
  if (ch) snprintf(b, sizeof(b), "%s %d%%", LV_SYMBOL_CHARGE, pct);
  else    snprintf(b, sizeof(b), "BATT %d%%", pct);
  if (home_batt_d) lv_label_set_text(home_batt_d, b);

  if (!g_stealth && !ch) {
    static int last_clamp = 255;
    int target = pct < 8 ? 8 : (pct < 15 ? 40 : (g_night ? 40 : 180));
    if (target != last_clamp) { co5300_set_brightness(target); last_clamp = target; }
  }
  if (camp_batt_hours) {
    char e[32]; snprintf(e, sizeof(e), "EST %d h", pct / 4);
    lv_label_set_text(camp_batt_hours, e);
  }
}

static void update_camp_panel() {
  if (!camp_sunrise) return;
  struct tm tm;
  if (!getLocalTime(&tm, 5)) {
    lv_label_set_text(camp_sunrise, "--:--");
    lv_label_set_text(camp_sunset,  "--:--");
    lv_label_set_text(camp_daylight,"--");
    return;
  }
  int srh, srm, ssh, ssm;
  sunrise_sunset(tm.tm_year+1900, tm.tm_mon+1, tm.tm_mday,
                 g_lat, g_lon, g_gmt_offset_sec/3600, &srh, &srm, &ssh, &ssm);
  char a[16], b[16], c[32];
  if (srh < 0) { strcpy(a,"--:--"); strcpy(b,"--:--"); strcpy(c,"polar"); }
  else {
    snprintf(a, sizeof(a), "%02d:%02d", srh, srm);
    snprintf(b, sizeof(b), "%02d:%02d", ssh, ssm);
    int now_min = tm.tm_hour*60 + tm.tm_min;
    int ss_min  = ssh*60 + ssm;
    int rem     = ss_min - now_min;
    if (rem < 0) rem += 24*60;
    if (rem > 12*60) snprintf(c, sizeof(c), "NIGHT");
    else snprintf(c, sizeof(c), "%dh %dm light left", rem/60, rem%60);
  }
  lv_label_set_text(camp_sunrise, a);
  lv_label_set_text(camp_sunset,  b);
  lv_label_set_text(camp_daylight, c);

  if (camp_steps) { char s[24]; snprintf(s, sizeof(s), "%lu steps", g_steps); lv_label_set_text(camp_steps, s); }
  if (camp_temp && g_has_pmu) {
    float tc = (float)g_pmu.getTemperature();
    char tt[24]; snprintf(tt, sizeof(tt), "%.1f °C", tc);
    lv_label_set_text(camp_temp, tt);
  }
}

static void update_compass_labels() {
  if (!cmp_heading_lbl) return;
  char h[16]; snprintf(h, sizeof(h), "%03d°", (int)g_heading_deg);
  lv_label_set_text(cmp_heading_lbl, h);
  AccG g = qmi_acc();
  float tilt  = atan2f(sqrtf(g.x*g.x+g.y*g.y), g.z) * 180.0f / M_PI;
  float slope = tanf(tilt * M_PI / 180.0f) * 100.0f;
  if (slope > 999) slope = 999;
  char tt[16]; snprintf(tt, sizeof(tt), "TILT %02d°", (int)tilt);
  lv_label_set_text(cmp_tilt_lbl, tt);
  if (cmp_slope_lbl) {
    char ss[16]; snprintf(ss, sizeof(ss), "%d%% SLOPE", (int)slope);
    lv_label_set_text(cmp_slope_lbl, ss);
  }
}

static void update_walk_panel() {
  if (!walk_steps) return;
  char s[24]; snprintf(s, sizeof(s), "%lu STEPS", g_steps);
  lv_label_set_text(walk_steps, s);
  char h[16]; snprintf(h, sizeof(h), "%03d°", (int)g_heading_deg);
  lv_label_set_text(walk_heading, h);
  if (walk_dist) {
    char d[24]; snprintf(d, sizeof(d), "%.2f KM", g_trip_m / 1000.0f);
    lv_label_set_text(walk_dist, d);
  }
}

static void update_tools_panel() {
  if (tools_timer_lbl) {
    if (g_timer_start) {
      uint32_t e = (millis() - g_timer_start) / 1000;
      char m[24]; snprintf(m, sizeof(m), "%02lu:%02lu", e/60, e%60);
      lv_label_set_text(tools_timer_lbl, m);
    } else lv_label_set_text(tools_timer_lbl, "00:00");
  }
  if (tools_countdown_lbl) {
    if (g_countdown_end) {
      int32_t left = (int32_t)(g_countdown_end - millis());
      if (left <= 0) {
        g_countdown_end = 0;
        flash_on();
        show_toast("[!] COUNTDOWN DONE", 3000);
        log_event("COUNTDOWN done");
        static uint32_t cd_done_t = 0;
        cd_done_t = millis();
      }
      uint32_t s = left > 0 ? (uint32_t)(left/1000) : 0;
      char m[24]; snprintf(m, sizeof(m), "%02lu:%02lu", s/60, s%60);
      lv_label_set_text(tools_countdown_lbl, m);
    } else lv_label_set_text(tools_countdown_lbl, "--:--");
  }
  if (tools_cd_min_lbl) {
    char m[16]; snprintf(m, sizeof(m), "%d MIN", g_cd_set_min);
    lv_label_set_text(tools_cd_min_lbl, m);
  }
  if (tools_lap_list && g_lap_count) {
    String s = "";
    for (int i = 0; i < g_lap_count; i++) {
      char b[24]; snprintf(b, sizeof(b), "L%d  %lus\n", i+1, g_lap_times[i]/1000);
      s += b;
    }
    lv_label_set_text(tools_lap_list, s.c_str());
  } else if (tools_lap_list) lv_label_set_text(tools_lap_list, "");
  if (tools_pace_lbl) {
    char m[24];
    if (g_pace_hundreds) snprintf(m, sizeof(m), "%lu00m+%lu", g_pace_hundreds, g_pace_count);
    else                 snprintf(m, sizeof(m), "PACE %lu", g_pace_count);
    lv_label_set_text(tools_pace_lbl, m);
  }
}

// =============================================================================
// UI builders
// =============================================================================
static lv_obj_t *make_card(lv_obj_t *parent, int w, int h) {
  lv_obj_t *c = lv_obj_create(parent);
  lv_obj_remove_style_all(c);
  lv_obj_set_size(c, w, h);
  lv_obj_set_style_bg_color(c, lv_color_hex(T_SURFACE), 0);
  lv_obj_set_style_bg_opa  (c, LV_OPA_COVER, 0);
  lv_obj_set_style_border_color(c, lv_color_hex(T_BORDER), 0);
  lv_obj_set_style_border_width(c, 1, 0);
  lv_obj_set_style_radius(c, 14, 0);
  lv_obj_set_style_pad_all(c, 12, 0);
  return c;
}
static lv_obj_t *make_eyebrow(lv_obj_t *parent, const char *txt, lv_align_t a, int x, int y) {
  lv_obj_t *l = lv_label_create(parent);
  lv_label_set_text(l, txt);
  lv_obj_set_style_text_color(l, lv_color_hex(T_DIM_RED), 0);
  lv_obj_set_style_text_font (l, &lv_font_montserrat_12, 0);
  lv_obj_set_style_text_letter_space(l, 3, 0);
  lv_obj_align(l, a, x, y);
  return l;
}
static void build_back_btn(lv_obj_t *parent) {
  // v2.2: pill removed.  BOOT button is the only exit path now -- held
  // for >1.5 s in loop() calls return_to_launcher().  Keeping the function
  // around as a no-op so the 7 build_back_btn(...) call sites still compile.
  (void)parent;
  back_btn = nullptr;
}

// ---- HOME with 3 watchfaces -------------------------------------------------
static void build_home_face_digital() {
  home_face_digital = lv_obj_create(tab_home);
  lv_obj_remove_style_all(home_face_digital);
  lv_obj_set_size(home_face_digital, LCD_WIDTH, LCD_HEIGHT);
  lv_obj_set_style_bg_opa(home_face_digital, LV_OPA_TRANSP, 0);

  make_eyebrow(home_face_digital, "TACTICAL // OS", LV_ALIGN_TOP_MID, 0, 14);

  home_sync_d = lv_obj_create(home_face_digital);
  lv_obj_remove_style_all(home_sync_d);
  lv_obj_set_size(home_sync_d, 10, 10);
  lv_obj_set_style_radius(home_sync_d, 5, 0);
  lv_obj_set_style_bg_color(home_sync_d, lv_color_hex(g_wifi_synced ? T_OK : T_DANGER), 0);
  lv_obj_set_style_bg_opa  (home_sync_d, LV_OPA_COVER, 0);
  lv_obj_align(home_sync_d, LV_ALIGN_TOP_RIGHT, -30, 18);

  home_clock_d = lv_label_create(home_face_digital);
  lv_label_set_text(home_clock_d, "--:--");
  lv_obj_set_style_text_color(home_clock_d, lv_color_hex(T_TXT), 0);
  lv_obj_set_style_text_font (home_clock_d, &lv_font_montserrat_48, 0);
  lv_obj_align(home_clock_d, LV_ALIGN_CENTER, 0, -70);

  home_date_d = lv_label_create(home_face_digital);
  lv_label_set_text(home_date_d, "");
  lv_obj_set_style_text_color(home_date_d, lv_color_hex(0x7A5050), 0);
  lv_obj_set_style_text_font (home_date_d, &lv_font_montserrat_14, 0);
  lv_obj_set_style_text_letter_space(home_date_d, 2, 0);
  lv_obj_align(home_date_d, LV_ALIGN_CENTER, 0, -26);

  home_batt_d = lv_label_create(home_face_digital);
  lv_label_set_text(home_batt_d, "BATT --%");
  lv_obj_set_style_text_color(home_batt_d, lv_color_hex(T_RED), 0);
  lv_obj_set_style_text_font (home_batt_d, &lv_font_montserrat_18, 0);
  lv_obj_align(home_batt_d, LV_ALIGN_CENTER, 0, 20);

  home_status_d = lv_label_create(home_face_digital);
  lv_label_set_text(home_status_d, "OPERATOR READY");
  lv_obj_set_style_text_color(home_status_d, lv_color_hex(T_RED), 0);
  lv_obj_set_style_text_font (home_status_d, &lv_font_montserrat_14, 0);
  lv_obj_set_style_text_letter_space(home_status_d, 4, 0);
  lv_obj_align(home_status_d, LV_ALIGN_CENTER, 0, 60);

  lv_obj_t *sub = lv_label_create(home_face_digital);
  lv_label_set_text(sub, "swipe left/right to change tabs");
  lv_obj_set_style_text_color(sub, lv_color_hex(T_MUTE), 0);
  lv_obj_set_style_text_font (sub, &lv_font_montserrat_12, 0);
  lv_obj_align(sub, LV_ALIGN_CENTER, 0, 100);
}

static void draw_home_radar();
static void build_home_face_radar() {
  home_face_radar = lv_obj_create(tab_home);
  lv_obj_remove_style_all(home_face_radar);
  lv_obj_set_size(home_face_radar, LCD_WIDTH, LCD_HEIGHT);
  lv_obj_set_style_bg_opa(home_face_radar, LV_OPA_TRANSP, 0);
  lv_obj_add_flag(home_face_radar, LV_OBJ_FLAG_HIDDEN);

  home_radar_buf = (lv_color_t *)heap_caps_malloc(HFACE_W * HFACE_H * sizeof(lv_color_t),
                    MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!home_radar_buf) home_radar_buf = (lv_color_t *)heap_caps_malloc(
                    HFACE_W * HFACE_H * sizeof(lv_color_t), MALLOC_CAP_8BIT);
  home_radar_canvas = lv_canvas_create(home_face_radar);
  lv_canvas_set_buffer(home_radar_canvas, home_radar_buf, HFACE_W, HFACE_H, LV_IMG_CF_TRUE_COLOR);
  lv_obj_align(home_radar_canvas, LV_ALIGN_CENTER, 0, 0);

  home_radar_time = lv_label_create(home_face_radar);
  lv_label_set_text(home_radar_time, "--:--");
  lv_obj_set_style_text_color(home_radar_time, lv_color_hex(T_RED), 0);
  lv_obj_set_style_text_font (home_radar_time, &lv_font_montserrat_42, 0);
  lv_obj_align(home_radar_time, LV_ALIGN_CENTER, 0, 0);
}
static void draw_home_radar() {
  if (!home_radar_canvas) return;
  if (lv_obj_has_flag(home_face_radar, LV_OBJ_FLAG_HIDDEN)) return;
  lv_canvas_fill_bg(home_radar_canvas, lv_color_hex(T_BG), LV_OPA_COVER);
  int cx = HFACE_W/2, cy = HFACE_H/2;
  lv_draw_arc_dsc_t arc; lv_draw_arc_dsc_init(&arc);
  arc.color = lv_color_hex(T_BORDER); arc.width = 1;
  for (int r = 50; r <= 170; r += 40) lv_canvas_draw_arc(home_radar_canvas, cx, cy, r, 0, 360, &arc);
  // tick marks every 30deg
  lv_draw_line_dsc_t ld; lv_draw_line_dsc_init(&ld);
  ld.color = lv_color_hex(T_DIM_RED); ld.width = 1;
  for (int i = 0; i < 360; i += 30) {
    float a = i * M_PI / 180.0f;
    lv_point_t pts[2] = {
      { (lv_coord_t)(cx + 160*sinf(a)), (lv_coord_t)(cy - 160*cosf(a)) },
      { (lv_coord_t)(cx + 170*sinf(a)), (lv_coord_t)(cy - 170*cosf(a)) }
    };
    lv_canvas_draw_line(home_radar_canvas, pts, 2, &ld);
  }
  // animated sweep
  static int sw = 0;
  sw = (sw + 6) % 360;
  float a = sw * M_PI / 180.0f;
  lv_draw_line_dsc_t sd; lv_draw_line_dsc_init(&sd);
  sd.color = lv_color_hex(T_RED); sd.width = 1;
  lv_point_t pts[2] = {
    { (lv_coord_t)cx, (lv_coord_t)cy },
    { (lv_coord_t)(cx + 165*sinf(a)), (lv_coord_t)(cy - 165*cosf(a)) }
  };
  lv_canvas_draw_line(home_radar_canvas, pts, 2, &sd);
}

static void build_home_face_minimal() {
  home_face_minimal = lv_obj_create(tab_home);
  lv_obj_remove_style_all(home_face_minimal);
  lv_obj_set_size(home_face_minimal, LCD_WIDTH, LCD_HEIGHT);
  lv_obj_set_style_bg_opa(home_face_minimal, LV_OPA_TRANSP, 0);
  lv_obj_add_flag(home_face_minimal, LV_OBJ_FLAG_HIDDEN);

  home_min_time = lv_label_create(home_face_minimal);
  lv_label_set_text(home_min_time, "--:--");
  lv_obj_set_style_text_color(home_min_time, lv_color_hex(T_TXT), 0);
  lv_obj_set_style_text_font (home_min_time, &lv_font_montserrat_48, 0);
  lv_obj_align(home_min_time, LV_ALIGN_CENTER, 0, -10);

  home_min_label = lv_label_create(home_face_minimal);
  lv_label_set_text(home_min_label, ".  .  .");
  lv_obj_set_style_text_color(home_min_label, lv_color_hex(T_DIM_RED), 0);
  lv_obj_set_style_text_font (home_min_label, &lv_font_montserrat_22, 0);
  lv_obj_set_style_text_letter_space(home_min_label, 8, 0);
  lv_obj_align(home_min_label, LV_ALIGN_CENTER, 0, 50);
}

// v2.3: home_tap_cb removed -- watchface is picked from TOOLS card now.

static void build_home_tab() {
  build_home_face_digital();
  build_home_face_radar();
  build_home_face_minimal();
  // v2.3: invisible centre tap target REMOVED.  It used to cycle the
  // watchface, but the user now picks the face from the WATCH FACE card
  // in the TOOLS tab only.  Removing it also frees the home centre for
  // the tabview's horizontal swipe nav -- a clickable child object was
  // eating the gesture before LVGL could route it.
  build_back_btn(tab_home);
}

// ---- COMPASS ----------------------------------------------------------------
static void build_compass_tab() {
  make_eyebrow(tab_compass, "COMPASS // HUD", LV_ALIGN_TOP_MID, 0, 14);

  cmp_buf = (lv_color_t *)heap_caps_malloc(CMP_W * CMP_H * sizeof(lv_color_t),
              MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!cmp_buf) cmp_buf = (lv_color_t *)heap_caps_malloc(CMP_W * CMP_H * sizeof(lv_color_t),
              MALLOC_CAP_8BIT);
  cmp_canvas = lv_canvas_create(tab_compass);
  lv_canvas_set_buffer(cmp_canvas, cmp_buf, CMP_W, CMP_H, LV_IMG_CF_TRUE_COLOR);
  lv_obj_align(cmp_canvas, LV_ALIGN_CENTER, 0, -10);

  cmp_heading_lbl = lv_label_create(tab_compass);
  lv_label_set_text(cmp_heading_lbl, "000°");
  lv_obj_set_style_text_color(cmp_heading_lbl, lv_color_hex(T_RED), 0);
  lv_obj_set_style_text_font (cmp_heading_lbl, &lv_font_montserrat_28, 0);
  lv_obj_align(cmp_heading_lbl, LV_ALIGN_CENTER, 0, -10);

  cmp_tilt_lbl = lv_label_create(tab_compass);
  lv_label_set_text(cmp_tilt_lbl, "TILT 0°");
  lv_obj_set_style_text_color(cmp_tilt_lbl, lv_color_hex(T_MUTE), 0);
  lv_obj_set_style_text_font (cmp_tilt_lbl, &lv_font_montserrat_14, 0);
  lv_obj_set_style_text_letter_space(cmp_tilt_lbl, 3, 0);
  lv_obj_align(cmp_tilt_lbl, LV_ALIGN_CENTER, -70, 28);

  cmp_slope_lbl = lv_label_create(tab_compass);
  lv_label_set_text(cmp_slope_lbl, "0% SLOPE");
  lv_obj_set_style_text_color(cmp_slope_lbl, lv_color_hex(T_AMBER), 0);
  lv_obj_set_style_text_font (cmp_slope_lbl, &lv_font_montserrat_14, 0);
  lv_obj_set_style_text_letter_space(cmp_slope_lbl, 3, 0);
  lv_obj_align(cmp_slope_lbl, LV_ALIGN_CENTER, 70, 28);

  cmp_zero_btn = lv_btn_create(tab_compass);
  lv_obj_set_size(cmp_zero_btn, 140, 38);
  lv_obj_align(cmp_zero_btn, LV_ALIGN_BOTTOM_MID, 0, -70);
  lv_obj_set_style_bg_color(cmp_zero_btn, lv_color_hex(T_SURFACE), 0);
  lv_obj_set_style_border_color(cmp_zero_btn, lv_color_hex(T_DIM_RED), 0);
  lv_obj_set_style_border_width(cmp_zero_btn, 1, 0);
  lv_obj_set_style_radius(cmp_zero_btn, 19, 0);
  lv_obj_t *zl = lv_label_create(cmp_zero_btn);
  lv_label_set_text(zl, "ZERO HEADING");
  lv_obj_set_style_text_color(zl, lv_color_hex(T_RED), 0);
  lv_obj_set_style_text_font (zl, &lv_font_montserrat_12, 0);
  lv_obj_set_style_text_letter_space(zl, 3, 0);
  lv_obj_center(zl);
  lv_obj_add_event_cb(cmp_zero_btn, cmp_zero_cb, LV_EVENT_CLICKED, NULL);

  build_back_btn(tab_compass);
}

// ---- NAV --------------------------------------------------------------------
static void build_nav_tab() {
  make_eyebrow(tab_nav, "NAV // WAYPOINTS", LV_ALIGN_TOP_MID, 0, 14);

  nav_buf = (lv_color_t *)heap_caps_malloc(NAV_W * NAV_H * sizeof(lv_color_t),
              MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!nav_buf) nav_buf = (lv_color_t *)heap_caps_malloc(NAV_W * NAV_H * sizeof(lv_color_t),
              MALLOC_CAP_8BIT);
  nav_canvas = lv_canvas_create(tab_nav);
  lv_canvas_set_buffer(nav_canvas, nav_buf, NAV_W, NAV_H, LV_IMG_CF_TRUE_COLOR);
  lv_obj_align(nav_canvas, LV_ALIGN_TOP_MID, 0, 36);

  // bearing big
  nav_bearing_lbl = lv_label_create(tab_nav);
  lv_label_set_text(nav_bearing_lbl, "---°");
  lv_obj_set_style_text_color(nav_bearing_lbl, lv_color_hex(T_RED), 0);
  lv_obj_set_style_text_font (nav_bearing_lbl, &lv_font_montserrat_28, 0);
  lv_obj_align(nav_bearing_lbl, LV_ALIGN_TOP_LEFT, 20, 270);

  nav_dist_lbl = lv_label_create(tab_nav);
  lv_label_set_text(nav_dist_lbl, "---- m");
  lv_obj_set_style_text_color(nav_dist_lbl, lv_color_hex(T_AMBER), 0);
  lv_obj_set_style_text_font (nav_dist_lbl, &lv_font_montserrat_22, 0);
  lv_obj_align(nav_dist_lbl, LV_ALIGN_TOP_RIGHT, -20, 274);

  nav_name_lbl = lv_label_create(tab_nav);
  lv_label_set_text(nav_name_lbl, "no target");
  lv_obj_set_style_text_color(nav_name_lbl, lv_color_hex(T_DIM_RED), 0);
  lv_obj_set_style_text_font (nav_name_lbl, &lv_font_montserrat_14, 0);
  lv_obj_set_style_text_letter_space(nav_name_lbl, 3, 0);
  lv_obj_align(nav_name_lbl, LV_ALIGN_TOP_MID, 0, 305);

  nav_trip_lbl = lv_label_create(tab_nav);
  lv_label_set_text(nav_trip_lbl, "TRIP 0.0 km");
  lv_obj_set_style_text_color(nav_trip_lbl, lv_color_hex(T_MUTE), 0);
  lv_obj_set_style_text_font (nav_trip_lbl, &lv_font_montserrat_12, 0);
  lv_obj_set_style_text_letter_space(nav_trip_lbl, 3, 0);
  lv_obj_align(nav_trip_lbl, LV_ALIGN_TOP_MID, 0, 330);

  // controls
  nav_mark_btn = lv_btn_create(tab_nav);
  lv_obj_set_size(nav_mark_btn, 110, 40);
  lv_obj_align(nav_mark_btn, LV_ALIGN_BOTTOM_LEFT, 18, -70);
  lv_obj_set_style_bg_color(nav_mark_btn, lv_color_hex(T_SURFACE), 0);
  lv_obj_set_style_border_color(nav_mark_btn, lv_color_hex(T_AMBER), 0);
  lv_obj_set_style_border_width(nav_mark_btn, 1, 0);
  lv_obj_set_style_radius(nav_mark_btn, 12, 0);
  lv_obj_t *ml = lv_label_create(nav_mark_btn);
  lv_label_set_text(ml, "MARK HERE");
  lv_obj_set_style_text_color(ml, lv_color_hex(T_AMBER), 0);
  lv_obj_set_style_text_font (ml, &lv_font_montserrat_12, 0);
  lv_obj_set_style_text_letter_space(ml, 2, 0);
  lv_obj_center(ml);
  lv_obj_add_event_cb(nav_mark_btn, nav_mark_cb, LV_EVENT_CLICKED, NULL);

  nav_clear_btn = lv_btn_create(tab_nav);
  lv_obj_set_size(nav_clear_btn, 110, 40);
  lv_obj_align(nav_clear_btn, LV_ALIGN_BOTTOM_RIGHT, -18, -70);
  lv_obj_set_style_bg_color(nav_clear_btn, lv_color_hex(T_SURFACE), 0);
  lv_obj_set_style_border_color(nav_clear_btn, lv_color_hex(T_DIM_RED), 0);
  lv_obj_set_style_border_width(nav_clear_btn, 1, 0);
  lv_obj_set_style_radius(nav_clear_btn, 12, 0);
  lv_obj_t *cl = lv_label_create(nav_clear_btn);
  lv_label_set_text(cl, "CLEAR");
  lv_obj_set_style_text_color(cl, lv_color_hex(T_RED), 0);
  lv_obj_set_style_text_font (cl, &lv_font_montserrat_12, 0);
  lv_obj_set_style_text_letter_space(cl, 2, 0);
  lv_obj_center(cl);
  lv_obj_add_event_cb(nav_clear_btn, nav_clear_cb, LV_EVENT_CLICKED, NULL);

  // dropdown to select wp
  nav_list = lv_dropdown_create(tab_nav);
  lv_obj_set_size(nav_list, 140, 36);
  lv_obj_align(nav_list, LV_ALIGN_BOTTOM_MID, 0, -70);
  lv_obj_set_style_bg_color(nav_list, lv_color_hex(T_SURFACE), 0);
  lv_obj_set_style_border_color(nav_list, lv_color_hex(T_DIM_RED), 0);
  lv_obj_set_style_border_width(nav_list, 1, 0);
  lv_obj_set_style_radius(nav_list, 8, 0);
  lv_obj_set_style_text_color(nav_list, lv_color_hex(T_TXT), 0);
  lv_obj_set_style_text_font (nav_list, &lv_font_montserrat_12, 0);
  String s = "";
  for (int k = 0; k < 8; k++) {
    if (g_wp[k].used) s += String(g_wp[k].name);
    else              s += "[empty]";
    if (k < 7) s += "\n";
  }
  lv_dropdown_set_options(nav_list, s.c_str());
  lv_obj_add_event_cb(nav_list, nav_list_cb, LV_EVENT_VALUE_CHANGED, NULL);

  build_back_btn(tab_nav);
}

// ---- TOOLS ------------------------------------------------------------------
static lv_obj_t *tool_btn(lv_obj_t *parent, const char *txt, lv_event_cb_t cb,
                          int x, int y, int w, int h, uint32_t accent) {
  lv_obj_t *b = lv_btn_create(parent);
  lv_obj_set_size(b, w, h);
  lv_obj_align(b, LV_ALIGN_TOP_LEFT, x, y);
  lv_obj_set_style_bg_color(b, lv_color_hex(T_SURFACE), 0);
  lv_obj_set_style_border_color(b, lv_color_hex(accent), 0);
  lv_obj_set_style_border_width(b, 1, 0);
  lv_obj_set_style_radius(b, 12, 0);
  lv_obj_t *l = lv_label_create(b);
  lv_label_set_text(l, txt);
  lv_obj_set_style_text_color(l, lv_color_hex(accent), 0);
  lv_obj_set_style_text_font (l, &lv_font_montserrat_12, 0);
  lv_obj_set_style_text_letter_space(l, 2, 0);
  lv_obj_center(l);
  if (cb) lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, NULL);
  return b;
}
static lv_obj_t *toggle_row(lv_obj_t *parent, const char *txt, lv_event_cb_t cb,
                            int y, bool *bound) {
  lv_obj_t *row = lv_obj_create(parent);
  lv_obj_remove_style_all(row);
  lv_obj_set_size(row, 360, 38);
  lv_obj_align(row, LV_ALIGN_TOP_MID, 0, y);
  lv_obj_set_style_bg_color(row, lv_color_hex(T_SURFACE), 0);
  lv_obj_set_style_bg_opa  (row, LV_OPA_COVER, 0);
  lv_obj_set_style_border_color(row, lv_color_hex(T_BORDER), 0);
  lv_obj_set_style_border_width(row, 1, 0);
  lv_obj_set_style_radius(row, 10, 0);
  lv_obj_set_style_pad_all(row, 8, 0);
  lv_obj_t *l = lv_label_create(row);
  lv_label_set_text(l, txt);
  lv_obj_set_style_text_color(l, lv_color_hex(T_RED), 0);
  lv_obj_set_style_text_font (l, &lv_font_montserrat_12, 0);
  lv_obj_align(l, LV_ALIGN_LEFT_MID, 4, 0);
  lv_obj_t *sw = lv_switch_create(row);
  lv_obj_align(sw, LV_ALIGN_RIGHT_MID, -4, 0);
  lv_obj_set_size(sw, 44, 22);
  lv_obj_set_style_bg_color(sw, lv_color_hex(0x250000), 0);
  lv_obj_set_style_bg_color(sw, lv_color_hex(T_RED), LV_PART_INDICATOR | LV_STATE_CHECKED);
  if (*bound) lv_obj_add_state(sw, LV_STATE_CHECKED);
  lv_obj_add_event_cb(sw, cb, LV_EVENT_VALUE_CHANGED, NULL);
  return sw;
}

// -----------------------------------------------------------------------------
// v2.4 -- TOOLS tab sub-paging (Tools <-> Settings, swipe up/down, instant)
// -----------------------------------------------------------------------------
// The TOOLS tab now hosts two full-size sub-pages.  Page 0 = Survival/Tools
// (timers, beacon, morse).  Page 1 = Settings/Prefs (night/stealth, theme,
// stride, watchface).  A vertical swipe flips between them with NO animation.
static lv_obj_t *tools_page_1 = nullptr;   // SURVIVAL // TOOLS
static lv_obj_t *tools_page_2 = nullptr;   // SETTINGS // PREFS
static uint8_t   tools_page   = 0;         // 0 -> page1 visible, 1 -> page2

static void tools_show_page(uint8_t p) {
  if (p > 1) p = 1;
  tools_page = p;
  if (tools_page_1) {
    if (p == 0) lv_obj_clear_flag(tools_page_1, LV_OBJ_FLAG_HIDDEN);
    else        lv_obj_add_flag  (tools_page_1, LV_OBJ_FLAG_HIDDEN);
  }
  if (tools_page_2) {
    if (p == 1) lv_obj_clear_flag(tools_page_2, LV_OBJ_FLAG_HIDDEN);
    else        lv_obj_add_flag  (tools_page_2, LV_OBJ_FLAG_HIDDEN);
  }
}

// Recursively enable event bubbling so a swipe that starts on any child
// (button, switch, label, card) is delivered to the sub-page container.
static void tools_enable_bubble(lv_obj_t *obj) {
  if (!obj) return;
  lv_obj_add_flag(obj, LV_OBJ_FLAG_EVENT_BUBBLE);
  uint32_t n = lv_obj_get_child_cnt(obj);
  for (uint32_t i = 0; i < n; i++) {
    tools_enable_bubble(lv_obj_get_child(obj, i));
  }
}

// Vertical-swipe gesture handler.  Only consumes LV_DIR_TOP / LV_DIR_BOTTOM
// so the tabview's horizontal scroll-snap keeps working unchanged.
static void tools_gesture_cb(lv_event_t *e) {
  (void)e;
  lv_indev_t *idv = lv_indev_get_act();
  if (!idv) return;
  lv_dir_t d = lv_indev_get_gesture_dir(idv);
  if (d == LV_DIR_TOP) {                 // swipe UP  -> show settings
    if (tools_page == 0) {
      lv_indev_wait_release(idv);        // one flip per swipe
      tools_show_page(1);
    }
  } else if (d == LV_DIR_BOTTOM) {       // swipe DOWN -> back to tools
    if (tools_page == 1) {
      lv_indev_wait_release(idv);
      tools_show_page(0);
    }
  }
  // LV_DIR_LEFT / LV_DIR_RIGHT: ignored, tabview handles them.
}

static lv_obj_t *make_tools_subpage(lv_obj_t *parent) {
  lv_obj_t *p = lv_obj_create(parent);
  lv_obj_remove_style_all(p);
  lv_obj_set_size(p, LCD_WIDTH, LCD_HEIGHT);
  lv_obj_set_pos (p, 0, 0);
  lv_obj_set_style_bg_color(p, lv_color_hex(T_BG), 0);
  lv_obj_set_style_bg_opa  (p, LV_OPA_COVER, 0);
  lv_obj_set_style_pad_all (p, 0, 0);
  lv_obj_clear_flag(p, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_event_cb(p, tools_gesture_cb, LV_EVENT_GESTURE, NULL);
  return p;
}

// =============================================================================
// v2.5 -- Full-screen STOPWATCH + LAP LIST (independent from Mission Timer)
// =============================================================================
static uint32_t sw_elapsed_ms() {
  return g_sw_accum + (g_sw_run ? (millis() - g_sw_start) : 0);
}
static void sw_fmt(uint32_t ms, char *b, size_t n) {
  uint32_t cs = (ms % 1000) / 10;
  uint32_t s  = (ms / 1000) % 60;
  uint32_t m  = ms / 60000;
  snprintf(b, n, "%02u:%02u.%02u", (unsigned)m, (unsigned)s, (unsigned)cs);
}
static void sw_refresh_time() {
  if (!g_sw_time_lbl) return;
  char b[16]; sw_fmt(sw_elapsed_ms(), b, sizeof(b));
  lv_label_set_text(g_sw_time_lbl, b);
}
static void sw_refresh_laps() {
  if (!g_sw_lap_box) return;
  lv_obj_clean(g_sw_lap_box);
  for (int i = (int)g_sw_lapn - 1; i >= 0; i--) {          // newest lap on top
    char t[16]; sw_fmt(g_sw_laps[i], t, sizeof(t));
    char b[32]; snprintf(b, sizeof(b), "LAP %d     %s", i + 1, t);
    lv_obj_t *row = lv_label_create(g_sw_lap_box);
    lv_label_set_text(row, b);
    lv_obj_set_style_text_color(row, lv_color_hex(i == (int)g_sw_lapn - 1 ? T_AMBER : T_TXT), 0);
    lv_obj_set_style_text_font (row, &lv_font_montserrat_16, 0);
  }
}
static void sw_tick_cb(lv_timer_t *) { sw_refresh_time(); }
static void sw_startstop_cb(lv_event_t *) {
  if (g_sw_run) { g_sw_accum += millis() - g_sw_start; g_sw_run = false; }
  else          { g_sw_start = millis();               g_sw_run = true;  }
  if (g_sw_ss_lbl) lv_label_set_text(g_sw_ss_lbl, g_sw_run ? "STOP" : "START");
}
static void sw_lap_cb(lv_event_t *) {
  if (!g_sw_run || g_sw_lapn >= 16) return;
  uint32_t e = sw_elapsed_ms();
  g_sw_laps[g_sw_lapn++] = e - g_sw_lap_mark;
  g_sw_lap_mark = e;
  sw_refresh_laps();
}
static void sw_reset_cb(lv_event_t *) {
  g_sw_run = false; g_sw_start = 0; g_sw_accum = 0;
  g_sw_lap_mark = 0; g_sw_lapn = 0;
  if (g_sw_ss_lbl) lv_label_set_text(g_sw_ss_lbl, "START");
  sw_refresh_time(); sw_refresh_laps();
}
static void sw_open(lv_event_t *) {
  if (!g_sw_overlay) return;
  if (g_sw_ss_lbl) lv_label_set_text(g_sw_ss_lbl, g_sw_run ? "STOP" : "START");
  lv_obj_clear_flag(g_sw_overlay, LV_OBJ_FLAG_HIDDEN);
  lv_obj_move_foreground(g_sw_overlay);
  sw_refresh_time(); sw_refresh_laps();
  if (g_sw_tick) lv_timer_resume(g_sw_tick);
}
static void sw_close(lv_event_t *) {
  if (g_sw_tick) lv_timer_pause(g_sw_tick);
  if (g_sw_overlay) lv_obj_add_flag(g_sw_overlay, LV_OBJ_FLAG_HIDDEN);
}
static lv_obj_t *sw_make_btn(lv_obj_t *par, const char *txt, uint32_t col,
                             int w, int h, int dx, int dy, lv_event_cb_t cb,
                             lv_obj_t **out_lbl) {
  lv_obj_t *b = lv_btn_create(par);
  lv_obj_set_size(b, w, h);
  lv_obj_align(b, LV_ALIGN_CENTER, dx, dy);
  lv_obj_set_style_bg_color(b, lv_color_hex(0x150000), 0);
  lv_obj_set_style_border_color(b, lv_color_hex(col), 0);
  lv_obj_set_style_border_width(b, 1, 0);
  lv_obj_set_style_radius(b, 8, 0);
  lv_obj_t *l = lv_label_create(b);
  lv_label_set_text(l, txt);
  lv_obj_set_style_text_color(l, lv_color_hex(col), 0);
  lv_obj_set_style_text_font (l, &lv_font_montserrat_16, 0);
  lv_obj_center(l);
  lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, NULL);
  if (out_lbl) *out_lbl = l;
  return b;
}
static void sw_build_overlay() {
  g_sw_overlay = lv_obj_create(lv_layer_top());
  lv_obj_remove_style_all(g_sw_overlay);
  lv_obj_set_size(g_sw_overlay, LCD_WIDTH, LCD_HEIGHT);
  lv_obj_set_pos (g_sw_overlay, 0, 0);
  lv_obj_set_style_bg_color(g_sw_overlay, lv_color_hex(T_BG), 0);
  lv_obj_set_style_bg_opa  (g_sw_overlay, LV_OPA_COVER, 0);
  lv_obj_clear_flag(g_sw_overlay, LV_OBJ_FLAG_SCROLLABLE);

  lv_obj_t *h = lv_label_create(g_sw_overlay);
  lv_label_set_text(h, "STOPWATCH // LAP");
  lv_obj_set_style_text_color(h, lv_color_hex(T_DIM_RED), 0);
  lv_obj_set_style_text_font (h, &lv_font_montserrat_16, 0);
  lv_obj_set_style_text_letter_space(h, 3, 0);
  lv_obj_align(h, LV_ALIGN_TOP_LEFT, 20, 18);

  lv_obj_t *x = lv_btn_create(g_sw_overlay);
  lv_obj_set_size(x, 44, 44);
  lv_obj_align(x, LV_ALIGN_TOP_RIGHT, -14, 10);
  lv_obj_set_style_bg_color(x, lv_color_hex(0x150000), 0);
  lv_obj_set_style_border_color(x, lv_color_hex(T_RED), 0);
  lv_obj_set_style_border_width(x, 1, 0);
  lv_obj_set_style_radius(x, 22, 0);
  lv_obj_t *xl = lv_label_create(x); lv_label_set_text(xl, LV_SYMBOL_CLOSE);
  lv_obj_set_style_text_color(xl, lv_color_hex(T_RED), 0); lv_obj_center(xl);
  lv_obj_add_event_cb(x, sw_close, LV_EVENT_CLICKED, NULL);

  g_sw_time_lbl = lv_label_create(g_sw_overlay);
  lv_label_set_text(g_sw_time_lbl, "00:00.00");
  lv_obj_set_style_text_color(g_sw_time_lbl, lv_color_hex(T_RED), 0);
  lv_obj_set_style_text_font (g_sw_time_lbl, &lv_font_montserrat_42, 0);
  lv_obj_align(g_sw_time_lbl, LV_ALIGN_TOP_MID, 0, 74);

  sw_make_btn(g_sw_overlay, "START", T_OK,    175, 56, -92, -46, sw_startstop_cb, &g_sw_ss_lbl);
  sw_make_btn(g_sw_overlay, "LAP",   T_AMBER, 175, 56,  92, -46, sw_lap_cb,       nullptr);
  sw_make_btn(g_sw_overlay, "RESET", T_DIM_RED,360, 46,  0,  16, sw_reset_cb,     nullptr);

  g_sw_lap_box = lv_obj_create(g_sw_overlay);
  lv_obj_set_size(g_sw_lap_box, 360, 150);
  lv_obj_align(g_sw_lap_box, LV_ALIGN_BOTTOM_MID, 0, -16);
  lv_obj_set_style_bg_color(g_sw_lap_box, lv_color_hex(T_SURFACE), 0);
  lv_obj_set_style_bg_opa  (g_sw_lap_box, LV_OPA_COVER, 0);
  lv_obj_set_style_border_color(g_sw_lap_box, lv_color_hex(T_BORDER), 0);
  lv_obj_set_style_border_width(g_sw_lap_box, 1, 0);
  lv_obj_set_style_radius(g_sw_lap_box, 8, 0);
  lv_obj_set_style_pad_all(g_sw_lap_box, 10, 0);
  lv_obj_set_flex_flow(g_sw_lap_box, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_row(g_sw_lap_box, 4, 0);

  g_sw_tick = lv_timer_create(sw_tick_cb, 47, NULL);
  lv_timer_pause(g_sw_tick);
  lv_obj_add_flag(g_sw_overlay, LV_OBJ_FLAG_HIDDEN);
}


static void build_tools_tab() {
  // v2.4: two full-size sub-pages, swipe up/down between them.
  // Make sure tab_tools itself doesn't intercept vertical scroll, and that
  // its own children fill the screen edge-to-edge.
  lv_obj_clear_flag(tab_tools, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_style_pad_all(tab_tools, 0, 0);

  tools_page_1 = make_tools_subpage(tab_tools);
  tools_page_2 = make_tools_subpage(tab_tools);
  lv_obj_add_flag(tools_page_2, LV_OBJ_FLAG_HIDDEN);   // boot on page 1
  tools_page = 0;

  // ===========================================================================
  // PAGE 1 -- SURVIVAL // TOOLS  (Flash, Beacon, Timer, Countdown, Morse)
  // The little "  UP" hint in the eyebrow tells the user a swipe-up reveals
  // the Settings sub-card.  No dot indicator (per user spec).
  // ===========================================================================
  make_eyebrow(tools_page_1, "SURVIVAL // TOOLS    UP", LV_ALIGN_TOP_MID, 0, 14);

  // Row 1: Flash + Beacon
  tool_btn(tools_page_1, LV_SYMBOL_EYE_OPEN " FLASH", tools_flash_cb,        20, 38, 175, 60, T_RED);
  tool_btn(tools_page_1, LV_SYMBOL_WARNING  " BEACON", tools_sos_cb,         215, 38, 105, 60, T_AMBER);
  tool_btn(tools_page_1, "MODE",                        tools_beacon_mode_cb, 325, 38, 65,  60, T_AMBER);

  // TIMER card
  lv_obj_t *tc = make_card(tools_page_1, 175, 130);
  lv_obj_align(tc, LV_ALIGN_TOP_LEFT, 20, 108);
  lv_obj_t *tl = lv_label_create(tc);
  lv_label_set_text(tl, "MISSION TIMER");
  lv_obj_set_style_text_color(tl, lv_color_hex(T_DIM_RED), 0);
  lv_obj_set_style_text_font (tl, &lv_font_montserrat_12, 0);
  lv_obj_set_style_text_letter_space(tl, 2, 0);
  lv_obj_align(tl, LV_ALIGN_TOP_LEFT, 0, 0);
  tools_timer_lbl = lv_label_create(tc);
  lv_label_set_text(tools_timer_lbl, "00:00");
  lv_obj_set_style_text_color(tools_timer_lbl, lv_color_hex(T_TXT), 0);
  lv_obj_set_style_text_font (tools_timer_lbl, &lv_font_montserrat_22, 0);
  lv_obj_align(tools_timer_lbl, LV_ALIGN_CENTER, 0, -8);
  lv_obj_t *btn_s = lv_btn_create(tc);
  lv_obj_set_size(btn_s, 70, 28);
  lv_obj_align(btn_s, LV_ALIGN_BOTTOM_LEFT, 0, 0);
  lv_obj_set_style_bg_color(btn_s, lv_color_hex(0x150000), 0);
  lv_obj_set_style_border_color(btn_s, lv_color_hex(T_RED), 0);
  lv_obj_set_style_border_width(btn_s, 1, 0);
  lv_obj_set_style_radius(btn_s, 6, 0);
  lv_obj_t *bsl = lv_label_create(btn_s); lv_label_set_text(bsl, "START/STOP");
  lv_obj_set_style_text_color(bsl, lv_color_hex(T_RED), 0);
  lv_obj_set_style_text_font (bsl, &lv_font_montserrat_12, 0); lv_obj_center(bsl);
  lv_obj_add_event_cb(btn_s, timer_start_cb, LV_EVENT_CLICKED, NULL);
  lv_obj_t *btn_l = lv_btn_create(tc);
  lv_obj_set_size(btn_l, 60, 28);
  lv_obj_align(btn_l, LV_ALIGN_BOTTOM_RIGHT, 0, 0);
  lv_obj_set_style_bg_color(btn_l, lv_color_hex(0x150000), 0);
  lv_obj_set_style_border_color(btn_l, lv_color_hex(T_AMBER), 0);
  lv_obj_set_style_border_width(btn_l, 1, 0);
  lv_obj_set_style_radius(btn_l, 6, 0);
  lv_obj_t *bll = lv_label_create(btn_l); lv_label_set_text(bll, "LAP");
  lv_obj_set_style_text_color(bll, lv_color_hex(T_AMBER), 0);
  lv_obj_set_style_text_font (bll, &lv_font_montserrat_12, 0); lv_obj_center(bll);
  lv_obj_add_event_cb(btn_l, timer_lap_cb, LV_EVENT_CLICKED, NULL);

  // COUNTDOWN card
  lv_obj_t *cc = make_card(tools_page_1, 175, 130);
  lv_obj_align(cc, LV_ALIGN_TOP_LEFT, 215, 108);
  lv_obj_t *cl = lv_label_create(cc);
  lv_label_set_text(cl, "COUNTDOWN");
  lv_obj_set_style_text_color(cl, lv_color_hex(T_DIM_RED), 0);
  lv_obj_set_style_text_font (cl, &lv_font_montserrat_12, 0);
  lv_obj_set_style_text_letter_space(cl, 2, 0);
  lv_obj_align(cl, LV_ALIGN_TOP_LEFT, 0, 0);
  tools_countdown_lbl = lv_label_create(cc);
  lv_label_set_text(tools_countdown_lbl, "--:--");
  lv_obj_set_style_text_color(tools_countdown_lbl, lv_color_hex(T_AMBER), 0);
  lv_obj_set_style_text_font (tools_countdown_lbl, &lv_font_montserrat_22, 0);
  lv_obj_align(tools_countdown_lbl, LV_ALIGN_CENTER, 0, -16);
  lv_obj_t *minus = lv_btn_create(cc);
  lv_obj_set_size(minus, 28, 26); lv_obj_align(minus, LV_ALIGN_CENTER, -50, 16);
  lv_obj_set_style_bg_color(minus, lv_color_hex(0x150000), 0);
  lv_obj_set_style_border_color(minus, lv_color_hex(T_DIM_RED), 0);
  lv_obj_set_style_border_width(minus, 1, 0); lv_obj_set_style_radius(minus, 6, 0);
  lv_obj_t *ml2 = lv_label_create(minus); lv_label_set_text(ml2, "-");
  lv_obj_set_style_text_color(ml2, lv_color_hex(T_RED), 0); lv_obj_center(ml2);
  lv_obj_add_event_cb(minus, cd_dec_cb, LV_EVENT_CLICKED, NULL);
  tools_cd_min_lbl = lv_label_create(cc);
  lv_label_set_text(tools_cd_min_lbl, "5 MIN");
  lv_obj_set_style_text_color(tools_cd_min_lbl, lv_color_hex(T_TXT), 0);
  lv_obj_set_style_text_font (tools_cd_min_lbl, &lv_font_montserrat_14, 0);
  lv_obj_align(tools_cd_min_lbl, LV_ALIGN_CENTER, 0, 18);
  lv_obj_t *plus = lv_btn_create(cc);
  lv_obj_set_size(plus, 28, 26); lv_obj_align(plus, LV_ALIGN_CENTER, 50, 16);
  lv_obj_set_style_bg_color(plus, lv_color_hex(0x150000), 0);
  lv_obj_set_style_border_color(plus, lv_color_hex(T_DIM_RED), 0);
  lv_obj_set_style_border_width(plus, 1, 0); lv_obj_set_style_radius(plus, 6, 0);
  lv_obj_t *pl2 = lv_label_create(plus); lv_label_set_text(pl2, "+");
  lv_obj_set_style_text_color(pl2, lv_color_hex(T_RED), 0); lv_obj_center(pl2);
  lv_obj_add_event_cb(plus, cd_inc_cb, LV_EVENT_CLICKED, NULL);
  lv_obj_t *go = lv_btn_create(cc);
  lv_obj_set_size(go, 60, 26); lv_obj_align(go, LV_ALIGN_BOTTOM_MID, 0, 0);
  lv_obj_set_style_bg_color(go, lv_color_hex(0x150000), 0);
  lv_obj_set_style_border_color(go, lv_color_hex(T_AMBER), 0);
  lv_obj_set_style_border_width(go, 1, 0); lv_obj_set_style_radius(go, 6, 0);
  lv_obj_t *gol = lv_label_create(go); lv_label_set_text(gol, "GO/STOP");
  lv_obj_set_style_text_color(gol, lv_color_hex(T_AMBER), 0);
  lv_obj_set_style_text_font (gol, &lv_font_montserrat_12, 0); lv_obj_center(gol);
  lv_obj_add_event_cb(go, cd_start_cb, LV_EVENT_CLICKED, NULL);

  // MORSE keyer card
  lv_obj_t *mc = make_card(tools_page_1, 370, 90);
  lv_obj_align(mc, LV_ALIGN_TOP_LEFT, 20, 248);
  lv_obj_t *mt = lv_label_create(mc);
  lv_label_set_text(mt, "MORSE KEYER");
  lv_obj_set_style_text_color(mt, lv_color_hex(T_DIM_RED), 0);
  lv_obj_set_style_text_font (mt, &lv_font_montserrat_12, 0);
  lv_obj_set_style_text_letter_space(mt, 2, 0);
  lv_obj_align(mt, LV_ALIGN_TOP_LEFT, 0, 0);
  tools_morse_preview = lv_label_create(mc);
  lv_label_set_text(tools_morse_preview, "_");
  lv_obj_set_style_text_color(tools_morse_preview, lv_color_hex(T_TXT), 0);
  lv_obj_set_style_text_font (tools_morse_preview, &lv_font_montserrat_14, 0);
  lv_obj_align(tools_morse_preview, LV_ALIGN_TOP_LEFT, 0, 20);
  // Keyer button (press hold to tap dit/dah)
  lv_obj_t *kb = lv_btn_create(mc);
  lv_obj_set_size(kb, 90, 36);
  lv_obj_align(kb, LV_ALIGN_BOTTOM_LEFT, 0, 0);
  lv_obj_set_style_bg_color(kb, lv_color_hex(0x250000), 0);
  lv_obj_set_style_border_color(kb, lv_color_hex(T_RED), 0);
  lv_obj_set_style_border_width(kb, 1, 0);
  lv_obj_set_style_radius(kb, 8, 0);
  lv_obj_t *kbl = lv_label_create(kb); lv_label_set_text(kbl, "TAP-KEY");
  lv_obj_set_style_text_color(kbl, lv_color_hex(T_RED), 0);
  lv_obj_set_style_text_font (kbl, &lv_font_montserrat_12, 0); lv_obj_center(kbl);
  lv_obj_add_event_cb(kb, keyer_press_cb, LV_EVENT_PRESSED, NULL);
  lv_obj_add_event_cb(kb, keyer_release_cb, LV_EVENT_RELEASED, NULL);
  // Space + Clear + Send
  lv_obj_t *sp = lv_btn_create(mc); lv_obj_set_size(sp, 60, 36);
  lv_obj_align(sp, LV_ALIGN_BOTTOM_LEFT, 100, 0);
  lv_obj_set_style_bg_color(sp, lv_color_hex(0x150000), 0);
  lv_obj_set_style_border_color(sp, lv_color_hex(T_DIM_RED), 0);
  lv_obj_set_style_border_width(sp, 1, 0); lv_obj_set_style_radius(sp, 8, 0);
  lv_obj_t *spl = lv_label_create(sp); lv_label_set_text(spl, "SP");
  lv_obj_set_style_text_color(spl, lv_color_hex(T_RED), 0); lv_obj_center(spl);
  lv_obj_add_event_cb(sp, keyer_space_cb, LV_EVENT_CLICKED, NULL);
  lv_obj_t *clr = lv_btn_create(mc); lv_obj_set_size(clr, 60, 36);
  lv_obj_align(clr, LV_ALIGN_BOTTOM_LEFT, 170, 0);
  lv_obj_set_style_bg_color(clr, lv_color_hex(0x150000), 0);
  lv_obj_set_style_border_color(clr, lv_color_hex(T_DIM_RED), 0);
  lv_obj_set_style_border_width(clr, 1, 0); lv_obj_set_style_radius(clr, 8, 0);
  lv_obj_t *clrl = lv_label_create(clr); lv_label_set_text(clrl, "CLR");
  lv_obj_set_style_text_color(clrl, lv_color_hex(T_RED), 0); lv_obj_center(clrl);
  lv_obj_add_event_cb(clr, keyer_clear_cb, LV_EVENT_CLICKED, NULL);
  lv_obj_t *snd = lv_btn_create(mc); lv_obj_set_size(snd, 60, 36);
  lv_obj_align(snd, LV_ALIGN_BOTTOM_RIGHT, 0, 0);
  lv_obj_set_style_bg_color(snd, lv_color_hex(0x150000), 0);
  lv_obj_set_style_border_color(snd, lv_color_hex(T_AMBER), 0);
  lv_obj_set_style_border_width(snd, 1, 0); lv_obj_set_style_radius(snd, 8, 0);
  lv_obj_t *sndl = lv_label_create(snd); lv_label_set_text(sndl, "SEND");
  lv_obj_set_style_text_color(sndl, lv_color_hex(T_AMBER), 0); lv_obj_center(sndl);
  lv_obj_add_event_cb(snd, keyer_send_cb, LV_EVENT_CLICKED, NULL);

  // v2.5: full Stopwatch + Lap-list launcher (opens a full-screen overlay)
  lv_obj_t *swb = lv_btn_create(tools_page_1);
  lv_obj_set_size(swb, 370, 46);
  lv_obj_align(swb, LV_ALIGN_TOP_LEFT, 20, 350);
  lv_obj_set_style_bg_color(swb, lv_color_hex(0x150000), 0);
  lv_obj_set_style_border_color(swb, lv_color_hex(T_CYAN), 0);
  lv_obj_set_style_border_width(swb, 1, 0);
  lv_obj_set_style_radius(swb, 8, 0);
  lv_obj_t *swl = lv_label_create(swb);
  lv_label_set_text(swl, LV_SYMBOL_LOOP "  STOPWATCH  +  LAP LIST");
  lv_obj_set_style_text_color(swl, lv_color_hex(T_CYAN), 0);
  lv_obj_set_style_text_font (swl, &lv_font_montserrat_16, 0);
  lv_obj_center(swl);
  lv_obj_add_event_cb(swb, sw_open, LV_EVENT_CLICKED, NULL);

  // "SWIPE UP FOR SETTINGS" footer hint (subtle, single line)
  lv_obj_t *hint1 = lv_label_create(tools_page_1);
  lv_label_set_text(hint1, "SWIPE UP  -  SETTINGS");
  lv_obj_set_style_text_color(hint1, lv_color_hex(T_DIM_RED), 0);
  lv_obj_set_style_text_font (hint1, &lv_font_montserrat_12, 0);
  lv_obj_set_style_text_letter_space(hint1, 3, 0);
  lv_obj_align(hint1, LV_ALIGN_BOTTOM_MID, 0, -16);

  // ===========================================================================
  // PAGE 2 -- SETTINGS // PREFS  (Night, Stealth, Pace, Theme, Stride, Face)
  // ===========================================================================
  make_eyebrow(tools_page_2, "DOWN    SETTINGS // PREFS", LV_ALIGN_TOP_MID, 0, 14);

  // Night / Stealth toggles -- moved to top of the settings page
  tools_night_sw   = toggle_row(tools_page_2, "NIGHT MODE",   tools_night_cb,   48, &g_night);
  tools_stealth_sw = toggle_row(tools_page_2, "STEALTH MODE", tools_stealth_cb, 92, &g_stealth);

  // Pace / Theme / Stride Cal row
  lv_obj_t *pace_btn = lv_btn_create(tools_page_2);
  lv_obj_set_size(pace_btn, 110, 36);
  lv_obj_align(pace_btn, LV_ALIGN_TOP_LEFT, 18, 150);
  lv_obj_set_style_bg_color(pace_btn, lv_color_hex(T_SURFACE), 0);
  lv_obj_set_style_border_color(pace_btn, lv_color_hex(T_AMBER), 0);
  lv_obj_set_style_border_width(pace_btn, 1, 0);
  lv_obj_set_style_radius(pace_btn, 8, 0);
  tools_pace_lbl = lv_label_create(pace_btn);
  lv_label_set_text(tools_pace_lbl, "PACE 0");
  lv_obj_set_style_text_color(tools_pace_lbl, lv_color_hex(T_AMBER), 0);
  lv_obj_set_style_text_font (tools_pace_lbl, &lv_font_montserrat_14, 0);
  lv_obj_center(tools_pace_lbl);
  lv_obj_add_event_cb(pace_btn, pace_bump_cb, LV_EVENT_CLICKED, NULL);
  lv_obj_add_event_cb(pace_btn, pace_reset_cb, LV_EVENT_LONG_PRESSED, NULL);

  lv_obj_t *theme_btn = lv_btn_create(tools_page_2);
  lv_obj_set_size(theme_btn, 110, 36);
  lv_obj_align(theme_btn, LV_ALIGN_TOP_MID, 0, 150);
  lv_obj_set_style_bg_color(theme_btn, lv_color_hex(T_SURFACE), 0);
  lv_obj_set_style_border_color(theme_btn, lv_color_hex(T_DIM_RED), 0);
  lv_obj_set_style_border_width(theme_btn, 1, 0);
  lv_obj_set_style_radius(theme_btn, 8, 0);
  lv_obj_t *thl = lv_label_create(theme_btn);
  lv_label_set_text(thl, "THEME");
  lv_obj_set_style_text_color(thl, lv_color_hex(T_RED), 0);
  lv_obj_set_style_text_font (thl, &lv_font_montserrat_14, 0);
  lv_obj_set_style_text_letter_space(thl, 2, 0);
  lv_obj_center(thl);
  lv_obj_add_event_cb(theme_btn, theme_cycle_cb, LV_EVENT_CLICKED, NULL);

  lv_obj_t *cal_btn = lv_btn_create(tools_page_2);
  lv_obj_set_size(cal_btn, 110, 36);
  lv_obj_align(cal_btn, LV_ALIGN_TOP_RIGHT, -18, 150);
  lv_obj_set_style_bg_color(cal_btn, lv_color_hex(T_SURFACE), 0);
  lv_obj_set_style_border_color(cal_btn, lv_color_hex(T_DIM_RED), 0);
  lv_obj_set_style_border_width(cal_btn, 1, 0);
  lv_obj_set_style_radius(cal_btn, 8, 0);
  lv_obj_t *cll2 = lv_label_create(cal_btn);
  lv_label_set_text(cll2, "STRIDE CAL");
  lv_obj_set_style_text_color(cll2, lv_color_hex(T_RED), 0);
  lv_obj_set_style_text_font (cll2, &lv_font_montserrat_12, 0);
  lv_obj_set_style_text_letter_space(cll2, 2, 0);
  lv_obj_center(cll2);
  lv_obj_add_event_cb(cal_btn, stride_cal_cb, LV_EVENT_CLICKED, NULL);

  // WATCH FACE selector card (DIGITAL / RADAR / MINIMAL pills)
  lv_obj_t *sc = make_card(tools_page_2, LCD_WIDTH - 40, 100);
  lv_obj_align(sc, LV_ALIGN_TOP_MID, 0, 210);

  lv_obj_t *sl = lv_label_create(sc);
  lv_label_set_text(sl, "WATCH FACE");
  lv_obj_set_style_text_color(sl, lv_color_hex(T_DIM_RED), 0);
  lv_obj_set_style_text_font (sl, &lv_font_montserrat_12, 0);
  lv_obj_set_style_text_letter_space(sl, 2, 0);
  lv_obj_align(sl, LV_ALIGN_TOP_LEFT, 0, 0);

  const char *face_names[3] = { "DIGITAL", "RADAR", "MINIMAL" };
  static lv_obj_t *face_pills[3] = { nullptr, nullptr, nullptr };
  for (int i = 0; i < 3; i++) {
    lv_obj_t *b = lv_btn_create(sc);
    lv_obj_set_size(b, 108, 34);
    lv_obj_align(b, LV_ALIGN_BOTTOM_LEFT, i * 116, 0);
    lv_obj_set_style_radius(b, 17, 0);
    lv_obj_set_style_border_width(b, 1, 0);
    bool active = (g_watchface == i);
    lv_obj_set_style_bg_color(b, lv_color_hex(active ? T_RED : T_SURFACE), 0);
    lv_obj_set_style_border_color(b, lv_color_hex(active ? T_RED : T_DIM_RED), 0);
    lv_obj_t *bl = lv_label_create(b);
    lv_label_set_text(bl, face_names[i]);
    lv_obj_set_style_text_color(bl, lv_color_hex(active ? 0x000000 : T_RED), 0);
    lv_obj_set_style_text_font (bl, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_letter_space(bl, 2, 0);
    lv_obj_center(bl);
    face_pills[i] = b;
    lv_obj_add_event_cb(b, [](lv_event_t *ev) {
      int idx = (int)(intptr_t)lv_event_get_user_data(ev);
      home_face_apply((uint8_t)idx);
      for (int j = 0; j < 3; j++) {
        if (!face_pills[j]) continue;
        bool act = (g_watchface == j);
        lv_obj_set_style_bg_color(face_pills[j],
          lv_color_hex(act ? T_RED : T_SURFACE), 0);
        lv_obj_set_style_border_color(face_pills[j],
          lv_color_hex(act ? T_RED : T_DIM_RED), 0);
        lv_obj_t *lbl = lv_obj_get_child(face_pills[j], 0);
        if (lbl) lv_obj_set_style_text_color(lbl,
          lv_color_hex(act ? 0x000000 : T_RED), 0);
      }
    }, LV_EVENT_CLICKED, (void *)(intptr_t)i);
  }

  // "SWIPE DOWN FOR TOOLS" footer hint
  lv_obj_t *hint2 = lv_label_create(tools_page_2);
  lv_label_set_text(hint2, "SWIPE DOWN  -  TOOLS");
  lv_obj_set_style_text_color(hint2, lv_color_hex(T_DIM_RED), 0);
  lv_obj_set_style_text_font (hint2, &lv_font_montserrat_12, 0);
  lv_obj_set_style_text_letter_space(hint2, 3, 0);
  lv_obj_align(hint2, LV_ALIGN_BOTTOM_MID, 0, -16);

  // v2.4: enable gesture-event bubbling on every descendant so a swipe
  // that started on a button or switch still flips the sub-page.
  tools_enable_bubble(tools_page_1);
  tools_enable_bubble(tools_page_2);

  build_back_btn(tab_tools);
}

// ---- CAMP -------------------------------------------------------------------
static void build_camp_tab() {
  make_eyebrow(tab_camp, "CAMP // FIELD", LV_ALIGN_TOP_MID, 0, 14);

  lv_obj_t *c1 = make_card(tab_camp, 175, 90);
  lv_obj_align(c1, LV_ALIGN_TOP_LEFT, 20, 50);
  lv_obj_t *l1 = lv_label_create(c1); lv_label_set_text(l1, "SUNRISE");
  lv_obj_set_style_text_color(l1, lv_color_hex(T_DIM_RED), 0);
  lv_obj_set_style_text_font (l1, &lv_font_montserrat_12, 0);
  lv_obj_align(l1, LV_ALIGN_TOP_LEFT, 0, 0);
  camp_sunrise = lv_label_create(c1);
  lv_label_set_text(camp_sunrise, "--:--");
  lv_obj_set_style_text_color(camp_sunrise, lv_color_hex(T_TXT), 0);
  lv_obj_set_style_text_font (camp_sunrise, &lv_font_montserrat_22, 0);
  lv_obj_align(camp_sunrise, LV_ALIGN_BOTTOM_LEFT, 0, 0);

  lv_obj_t *c2 = make_card(tab_camp, 175, 90);
  lv_obj_align(c2, LV_ALIGN_TOP_LEFT, 215, 50);
  lv_obj_t *l2 = lv_label_create(c2); lv_label_set_text(l2, "SUNSET");
  lv_obj_set_style_text_color(l2, lv_color_hex(T_DIM_RED), 0);
  lv_obj_set_style_text_font (l2, &lv_font_montserrat_12, 0);
  lv_obj_align(l2, LV_ALIGN_TOP_LEFT, 0, 0);
  camp_sunset = lv_label_create(c2);
  lv_label_set_text(camp_sunset, "--:--");
  lv_obj_set_style_text_color(camp_sunset, lv_color_hex(T_TXT), 0);
  lv_obj_set_style_text_font (camp_sunset, &lv_font_montserrat_22, 0);
  lv_obj_align(camp_sunset, LV_ALIGN_BOTTOM_LEFT, 0, 0);

  lv_obj_t *c3 = make_card(tab_camp, 370, 60);
  lv_obj_align(c3, LV_ALIGN_TOP_LEFT, 20, 150);
  camp_daylight = lv_label_create(c3);
  lv_label_set_text(camp_daylight, "-- light left");
  lv_obj_set_style_text_color(camp_daylight, lv_color_hex(T_AMBER), 0);
  lv_obj_set_style_text_font (camp_daylight, &lv_font_montserrat_16, 0);
  lv_obj_center(camp_daylight);

  lv_obj_t *c4 = make_card(tab_camp, 175, 60);
  lv_obj_align(c4, LV_ALIGN_TOP_LEFT, 20, 220);
  camp_batt_hours = lv_label_create(c4);
  lv_label_set_text(camp_batt_hours, "EST -- h");
  lv_obj_set_style_text_color(camp_batt_hours, lv_color_hex(T_RED), 0);
  lv_obj_set_style_text_font (camp_batt_hours, &lv_font_montserrat_16, 0);
  lv_obj_center(camp_batt_hours);

  lv_obj_t *c5 = make_card(tab_camp, 175, 60);
  lv_obj_align(c5, LV_ALIGN_TOP_LEFT, 215, 220);
  camp_temp = lv_label_create(c5);
  lv_label_set_text(camp_temp, "-- °C");
  lv_obj_set_style_text_color(camp_temp, lv_color_hex(T_RED), 0);
  lv_obj_set_style_text_font (camp_temp, &lv_font_montserrat_16, 0);
  lv_obj_center(camp_temp);

  lv_obj_t *c6 = make_card(tab_camp, 370, 60);
  lv_obj_align(c6, LV_ALIGN_TOP_LEFT, 20, 290);
  camp_steps = lv_label_create(c6);
  lv_label_set_text(camp_steps, "0 steps");
  lv_obj_set_style_text_color(camp_steps, lv_color_hex(T_TXT), 0);
  lv_obj_set_style_text_font (camp_steps, &lv_font_montserrat_16, 0);
  lv_obj_center(camp_steps);

  build_back_btn(tab_camp);
}

// ---- WALK -------------------------------------------------------------------
static void build_walk_tab() {
  walk_time = lv_label_create(tab_walk);
  lv_label_set_text(walk_time, "--:--");
  lv_obj_set_style_text_color(walk_time, lv_color_hex(T_TXT), 0);
  lv_obj_set_style_text_font (walk_time, &lv_font_montserrat_48, 0);
  lv_obj_align(walk_time, LV_ALIGN_CENTER, 0, -80);

  walk_steps = lv_label_create(tab_walk);
  lv_label_set_text(walk_steps, "0 STEPS");
  lv_obj_set_style_text_color(walk_steps, lv_color_hex(T_RED), 0);
  lv_obj_set_style_text_font (walk_steps, &lv_font_montserrat_22, 0);
  lv_obj_set_style_text_letter_space(walk_steps, 3, 0);
  lv_obj_align(walk_steps, LV_ALIGN_CENTER, 0, -20);

  walk_dist = lv_label_create(tab_walk);
  lv_label_set_text(walk_dist, "0.00 KM");
  lv_obj_set_style_text_color(walk_dist, lv_color_hex(T_AMBER), 0);
  lv_obj_set_style_text_font (walk_dist, &lv_font_montserrat_22, 0);
  lv_obj_set_style_text_letter_space(walk_dist, 3, 0);
  lv_obj_align(walk_dist, LV_ALIGN_CENTER, 0, 30);

  walk_heading = lv_label_create(tab_walk);
  lv_label_set_text(walk_heading, "000°");
  lv_obj_set_style_text_color(walk_heading, lv_color_hex(T_DIM_RED), 0);
  lv_obj_set_style_text_font (walk_heading, &lv_font_montserrat_22, 0);
  lv_obj_align(walk_heading, LV_ALIGN_CENTER, 0, 80);

  build_back_btn(tab_walk);
}

// ---- DATA (Journal + Stats) -------------------------------------------------
static void journal_ping_cb(lv_event_t *e) {
  const char *tag = (const char*)lv_event_get_user_data(e);
  journal_append(tag, "field marker");
  char m[40]; snprintf(m, sizeof(m), "JOURNAL: [%s]", tag);
  show_toast(m, 1200);
  if (data_journal_log) {
    File f = SD_MMC.open("/WATCH/tactical/journal.txt", FILE_READ);
    if (f) {
      // tail last ~500 bytes
      size_t sz = f.size();
      if (sz > 500) f.seek(sz - 500);
      String s = "";
      while (f.available()) s += (char)f.read();
      f.close();
      lv_label_set_text(data_journal_log, s.c_str());
    }
  }
}

static void build_data_tab() {
  make_eyebrow(tab_data, "DATA // STATS + LOG", LV_ALIGN_TOP_MID, 0, 14);

  // 7-day step graph card
  lv_obj_t *gc = make_card(tab_data, 374, 200);
  lv_obj_align(gc, LV_ALIGN_TOP_MID, 0, 36);
  lv_obj_t *gl = lv_label_create(gc); lv_label_set_text(gl, "STEPS // 7 DAY");
  lv_obj_set_style_text_color(gl, lv_color_hex(T_DIM_RED), 0);
  lv_obj_set_style_text_font (gl, &lv_font_montserrat_12, 0);
  lv_obj_set_style_text_letter_space(gl, 2, 0);
  lv_obj_align(gl, LV_ALIGN_TOP_LEFT, 0, 0);
  graph_buf = (lv_color_t *)heap_caps_malloc(GRAPH_W * GRAPH_H * sizeof(lv_color_t),
              MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!graph_buf) graph_buf = (lv_color_t *)heap_caps_malloc(
              GRAPH_W * GRAPH_H * sizeof(lv_color_t), MALLOC_CAP_8BIT);
  data_graph_canvas = lv_canvas_create(gc);
  lv_canvas_set_buffer(data_graph_canvas, graph_buf, GRAPH_W, GRAPH_H, LV_IMG_CF_TRUE_COLOR);
  lv_obj_align(data_graph_canvas, LV_ALIGN_BOTTOM_MID, 0, 0);

  // Journal tag buttons
  const char *tags[5] = { "OK", "RIDGE", "WATER", "CAMP", "SPOT" };
  for (int i = 0; i < 5; i++) {
    lv_obj_t *b = lv_btn_create(tab_data);
    lv_obj_set_size(b, 68, 38);
    lv_obj_align(b, LV_ALIGN_TOP_LEFT, 18 + i*74, 250);
    lv_obj_set_style_bg_color(b, lv_color_hex(T_SURFACE), 0);
    lv_obj_set_style_border_color(b, lv_color_hex(T_DIM_RED), 0);
    lv_obj_set_style_border_width(b, 1, 0);
    lv_obj_set_style_radius(b, 8, 0);
    lv_obj_t *l = lv_label_create(b); lv_label_set_text(l, tags[i]);
    lv_obj_set_style_text_color(l, lv_color_hex(T_RED), 0);
    lv_obj_set_style_text_font (l, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_letter_space(l, 2, 0);
    lv_obj_center(l);
    lv_obj_add_event_cb(b, journal_ping_cb, LV_EVENT_CLICKED, (void*)tags[i]);
  }

  // Recent journal tail
  lv_obj_t *jc = make_card(tab_data, 374, 80);
  lv_obj_align(jc, LV_ALIGN_TOP_MID, 0, 300);
  data_journal_log = lv_label_create(jc);
  lv_label_set_text(data_journal_log, "no entries yet");
  lv_obj_set_style_text_color(data_journal_log, lv_color_hex(T_MUTE), 0);
  lv_obj_set_style_text_font (data_journal_log, &lv_font_montserrat_12, 0);
  lv_obj_set_width(data_journal_log, 350);
  lv_label_set_long_mode(data_journal_log, LV_LABEL_LONG_WRAP);
  lv_obj_center(data_journal_log);

  build_back_btn(tab_data);
}

// =============================================================================
// BATTERY tab (v2.3)
//   * Inspired by LauncherOS' battery screen: big % arc, charging state,
//     voltage, runtime estimate, and a manual deep-sleep button.
//   * Runtime estimate is a 6-sample linear-fit on (pct, millis) pairs --
//     no static "%/hour" lookup table, it adapts to the user's actual drain
//     rate.  Returns -1 when there isn't enough data yet or charging.
// =============================================================================
static lv_obj_t *batt_arc      = nullptr;
static lv_obj_t *batt_pct_lbl  = nullptr;
static lv_obj_t *batt_state_lbl= nullptr;
static lv_obj_t *batt_remain_lbl=nullptr;
static lv_obj_t *batt_volt_lbl = nullptr;
static lv_obj_t *batt_steps_lbl= nullptr;

#define BATT_SAMP_N 6
static int      g_batt_pct_s[BATT_SAMP_N] = { -1,-1,-1,-1,-1,-1 };
static uint32_t g_batt_ms_s [BATT_SAMP_N] = {  0, 0, 0, 0, 0, 0 };
static uint8_t  g_batt_si   = 0;

static void batt_sample_push(int pct, uint32_t ms) {
  g_batt_pct_s[g_batt_si] = pct;
  g_batt_ms_s [g_batt_si] = ms;
  g_batt_si = (g_batt_si + 1) % BATT_SAMP_N;
}

static int batt_minutes_left(int pct, bool charging) {
  if (charging || pct < 0) return -1;
  // collect samples in chronological order
  int   px[BATT_SAMP_N]; uint32_t mx[BATT_SAMP_N]; int n = 0;
  for (int k = 0; k < BATT_SAMP_N; k++) {
    int i = (g_batt_si + k) % BATT_SAMP_N;
    if (g_batt_pct_s[i] < 0) continue;
    px[n] = g_batt_pct_s[i]; mx[n] = g_batt_ms_s[i]; n++;
  }
  if (n < 3) return -1;
  // need a real downward drop, otherwise the slope is just ADC jitter
  int drop = px[0] - px[n-1];
  uint32_t dt = mx[n-1] - mx[0];
  if (drop < 1 || dt < 60UL*1000UL) return -1;
  // minutes-per-percent at the observed rate
  float mins_per_pct = (dt / 60000.0f) / drop;
  float left = mins_per_pct * pct;
  if (left < 0)        return 0;
  if (left > 60*72)    return 60*72;   // clamp at 72 h
  return (int)left;
}

static lv_color_t batt_color_for(int pct) {
  if (pct <= 15) return lv_color_hex(0xFF4040);
  if (pct <= 30) return lv_color_hex(0xFFB300);
  return lv_color_hex(T_RED);   // tactical theme stays red on healthy state
}

static void update_battery_panel() {
  if (!batt_arc) return;
  if (!g_has_pmu) {
    lv_label_set_text(batt_pct_lbl,   "--%");
    lv_label_set_text(batt_state_lbl, "NO PMU");
    lv_label_set_text(batt_remain_lbl,"");
    lv_label_set_text(batt_volt_lbl,  "");
    return;
  }
  int  pct = g_pmu.getBatteryPercent();
  bool ch  = g_pmu.isCharging();
  int  mv  = g_pmu.getBattVoltage();
  if (pct < 0) pct = 0; if (pct > 100) pct = 100;

  // sample for the runtime estimator (once per call, ~5 s cadence from loop)
  batt_sample_push(pct, millis());

  lv_arc_set_value(batt_arc, pct);
  char b[16]; snprintf(b, sizeof(b), "%d%%", pct);
  lv_label_set_text(batt_pct_lbl, b);
  lv_color_t col = batt_color_for(pct);
  lv_obj_set_style_arc_color (batt_arc,     col, LV_PART_INDICATOR);
  lv_obj_set_style_text_color(batt_pct_lbl, col, 0);

  if (ch) {
    lv_label_set_text(batt_state_lbl, "CHARGING");
    lv_obj_set_style_text_color(batt_state_lbl, lv_color_hex(T_OK), 0);
    lv_label_set_text(batt_remain_lbl, "");
  } else {
    lv_label_set_text(batt_state_lbl, "ON BATTERY");
    lv_obj_set_style_text_color(batt_state_lbl, lv_color_hex(T_DIM_RED), 0);
    int mins = batt_minutes_left(pct, ch);
    if (mins < 0) lv_label_set_text(batt_remain_lbl, "EST ~ --");
    else {
      char r[48];
      int h = mins / 60, m = mins % 60;
      if (h > 0) snprintf(r, sizeof(r), "EST ~ %d h %02d m", h, m);
      else       snprintf(r, sizeof(r), "EST ~ %d m",       m);
      lv_label_set_text(batt_remain_lbl, r);
    }
  }
  char v[32]; snprintf(v, sizeof(v), "%.2f V", mv / 1000.0f);
  lv_label_set_text(batt_volt_lbl, v);

  if (batt_steps_lbl) {
    char s[32]; snprintf(s, sizeof(s), "%lu STEPS TODAY", (unsigned long)g_steps);
    lv_label_set_text(batt_steps_lbl, s);
  }
}

static void batt_sleep_btn_cb(lv_event_t *) {
  log_event("SLEEP via BATT tab");
  show_toast("ENTERING DEEP SLEEP", 800);
  lv_timer_handler();
  delay(900);
  // BOOT button (GPIO 0) wakes the watch.
  esp_sleep_enable_ext0_wakeup((gpio_num_t)BOOT_BTN_PIN, 0);
  esp_deep_sleep_start();
}

static void build_batt_tab() {
  make_eyebrow(tab_batt, "POWER // CORE", LV_ALIGN_TOP_MID, 0, 14);

  // Big tactical % arc (270° sweep, opens at the bottom like a HUD gauge).
  batt_arc = lv_arc_create(tab_batt);
  lv_obj_set_size(batt_arc, 280, 280);
  lv_obj_align(batt_arc, LV_ALIGN_CENTER, 0, -20);
  lv_arc_set_rotation(batt_arc, 135);
  lv_arc_set_bg_angles(batt_arc, 0, 270);
  lv_arc_set_range(batt_arc, 0, 100);
  lv_arc_set_value(batt_arc, 0);
  lv_obj_remove_style(batt_arc, NULL, LV_PART_KNOB);
  lv_obj_clear_flag(batt_arc, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_style_arc_color(batt_arc, lv_color_hex(0x200000), LV_PART_MAIN);
  lv_obj_set_style_arc_width(batt_arc, 12, LV_PART_MAIN);
  lv_obj_set_style_arc_color(batt_arc, lv_color_hex(T_RED),    LV_PART_INDICATOR);
  lv_obj_set_style_arc_width(batt_arc, 12, LV_PART_INDICATOR);

  batt_pct_lbl = lv_label_create(tab_batt);
  lv_label_set_text(batt_pct_lbl, "--%");
  lv_obj_set_style_text_color(batt_pct_lbl, lv_color_hex(T_RED), 0);
  lv_obj_set_style_text_font (batt_pct_lbl, &lv_font_montserrat_48, 0);
  lv_obj_align(batt_pct_lbl, LV_ALIGN_CENTER, 0, -40);

  batt_state_lbl = lv_label_create(tab_batt);
  lv_label_set_text(batt_state_lbl, "");
  lv_obj_set_style_text_color(batt_state_lbl, lv_color_hex(T_DIM_RED), 0);
  lv_obj_set_style_text_font (batt_state_lbl, &lv_font_montserrat_14, 0);
  lv_obj_set_style_text_letter_space(batt_state_lbl, 3, 0);
  lv_obj_align(batt_state_lbl, LV_ALIGN_CENTER, 0, 5);

  batt_remain_lbl = lv_label_create(tab_batt);
  lv_label_set_text(batt_remain_lbl, "EST ~ --");
  lv_obj_set_style_text_color(batt_remain_lbl, lv_color_hex(T_TXT), 0);
  lv_obj_set_style_text_font (batt_remain_lbl, &lv_font_montserrat_16, 0);
  lv_obj_align(batt_remain_lbl, LV_ALIGN_CENTER, 0, 35);

  batt_volt_lbl = lv_label_create(tab_batt);
  lv_label_set_text(batt_volt_lbl, "");
  lv_obj_set_style_text_color(batt_volt_lbl, lv_color_hex(T_MUTE), 0);
  lv_obj_set_style_text_font (batt_volt_lbl, &lv_font_montserrat_14, 0);
  lv_obj_align(batt_volt_lbl, LV_ALIGN_CENTER, 0, 65);

  batt_steps_lbl = lv_label_create(tab_batt);
  lv_label_set_text(batt_steps_lbl, "0 STEPS TODAY");
  lv_obj_set_style_text_color(batt_steps_lbl, lv_color_hex(T_AMBER), 0);
  lv_obj_set_style_text_font (batt_steps_lbl, &lv_font_montserrat_12, 0);
  lv_obj_set_style_text_letter_space(batt_steps_lbl, 2, 0);
  lv_obj_align(batt_steps_lbl, LV_ALIGN_CENTER, 0, 95);

  // SLEEP NOW pill -- enters deep sleep, BOOT button wakes.
  lv_obj_t *sb = lv_btn_create(tab_batt);
  lv_obj_set_size(sb, 200, 44);
  lv_obj_align(sb, LV_ALIGN_BOTTOM_MID, 0, -38);
  lv_obj_set_style_radius(sb, 22, 0);
  lv_obj_set_style_bg_color(sb, lv_color_hex(T_SURFACE), 0);
  lv_obj_set_style_border_color(sb, lv_color_hex(T_AMBER), 0);
  lv_obj_set_style_border_width(sb, 1, 0);
  lv_obj_t *sl = lv_label_create(sb);
  lv_label_set_text(sl, "SLEEP NOW");
  lv_obj_set_style_text_color(sl, lv_color_hex(T_AMBER), 0);
  lv_obj_set_style_text_font (sl, &lv_font_montserrat_14, 0);
  lv_obj_set_style_text_letter_space(sl, 4, 0);
  lv_obj_center(sl);
  lv_obj_add_event_cb(sb, batt_sleep_btn_cb, LV_EVENT_CLICKED, NULL);

  build_back_btn(tab_batt);
}

static void build_flash_overlay() {
  flash_scr = lv_obj_create(lv_layer_top());
  lv_obj_remove_style_all(flash_scr);
  lv_obj_set_size(flash_scr, LCD_WIDTH, LCD_HEIGHT);
  lv_obj_set_style_bg_color(flash_scr, lv_color_hex(0xFFFFFF), 0);
  lv_obj_set_style_bg_opa  (flash_scr, LV_OPA_COVER, 0);
  lv_obj_add_flag(flash_scr, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_event_cb(flash_scr, flash_scr_cb, LV_EVENT_CLICKED, NULL);
}

// Glitch transition (scanline sweep)
static void build_glitch_overlay() {
  glitch_overlay = lv_obj_create(lv_layer_top());
  lv_obj_remove_style_all(glitch_overlay);
  lv_obj_set_size(glitch_overlay, LCD_WIDTH, 8);
  lv_obj_set_style_bg_color(glitch_overlay, lv_color_hex(T_RED), 0);
  lv_obj_set_style_bg_opa  (glitch_overlay, LV_OPA_50, 0);
  lv_obj_add_flag(glitch_overlay, LV_OBJ_FLAG_HIDDEN);
  lv_obj_align(glitch_overlay, LV_ALIGN_TOP_MID, 0, 0);
}
static void trigger_glitch() {
  if (!glitch_overlay) return;
  lv_obj_clear_flag(glitch_overlay, LV_OBJ_FLAG_HIDDEN);
  lv_obj_move_foreground(glitch_overlay);
  g_glitch_until = millis() + 280;
}
static void tick_glitch() {
  if (!glitch_overlay) return;
  if (lv_obj_has_flag(glitch_overlay, LV_OBJ_FLAG_HIDDEN)) return;
  uint32_t now = millis();
  if (now >= g_glitch_until) {
    lv_obj_add_flag(glitch_overlay, LV_OBJ_FLAG_HIDDEN);
    return;
  }
  // animate y position
  uint32_t left = g_glitch_until - now;
  int y = ((280 - left) * LCD_HEIGHT) / 280;
  lv_obj_align(glitch_overlay, LV_ALIGN_TOP_MID, 0, y);
}

// =============================================================================
// BUILD UI
// =============================================================================
static void update_page_dots(int active) {
  for (int i = 0; i < M_NTABS; i++) if (page_dots[i]) {
    lv_obj_set_size(page_dots[i], i == active ? 8 : 5, i == active ? 8 : 5);
    lv_obj_set_style_bg_color(page_dots[i],
       lv_color_hex(i == active ? T_RED : T_DIM_RED), 0);
  }
  // v2.5 accessibility: keep the current tab name readable at all times, with
  // swipe affordance arrows so navigation is discoverable without a tab bar.
  if (tab_name_lbl && active >= 0 && active < M_NTABS) {
    lv_label_set_text_fmt(tab_name_lbl, "%s  %s  %s",
      active > 0 ? "<" : " ", kTabNames[active], active < M_NTABS - 1 ? ">" : " ");
  }
}
static void set_active_tab(int idx) {
  if (idx < 0 || idx >= M_NTABS) return;
  for (int i = 0; i < M_NTABS; i++) if (g_tabs[i]) {
    if (i == idx) lv_obj_clear_flag(g_tabs[i], LV_OBJ_FLAG_HIDDEN);
    else          lv_obj_add_flag  (g_tabs[i], LV_OBJ_FLAG_HIDDEN);
  }
  g_active_tab = (uint8_t)idx;
  if (g_tabs[idx]) lv_obj_move_foreground(g_tabs[idx]);
  update_page_dots(idx);
}

// v2.5 -- Horizontal swipe -> INSTANT tab change (no slide animation).
//   Swipe LEFT  -> next tab, swipe RIGHT -> previous tab, both LV_ANIM_OFF.
//   Vertical swipes (TOP/BOTTOM) are ignored here so the TOOLS sub-page
//   up/down flip keeps working via tools_gesture_cb.
static void tab_swipe_gesture_cb(lv_event_t *e) {
  (void)e;
  lv_indev_t *idv = lv_indev_get_act();
  if (!idv) return;
  lv_dir_t d = lv_indev_get_gesture_dir(idv);
  if (d == LV_DIR_LEFT) {
    lv_indev_wait_release(idv);   // one flip per swipe
    nav_relative(+1);
  } else if (d == LV_DIR_RIGHT) {
    lv_indev_wait_release(idv);
    nav_relative(-1);
  }
}

static void build_ui() {
  scr_root = lv_obj_create(NULL);
  lv_obj_set_style_bg_color(scr_root, lv_color_hex(T_BG), 0);
  lv_obj_set_style_bg_opa  (scr_root, LV_OPA_COVER, 0);
  lv_obj_add_event_cb(scr_root, scr_clicked_cb, LV_EVENT_CLICKED, NULL);

  // v2.5: lightweight pager (no lv_tabview dependency -- some lv_conf builds
  // ship with LV_USE_TABVIEW disabled). Each tab is a full-screen container on
  // scr_root; nav shows one and hides the rest for an instant, no-anim snap.
  tab_home    = lv_obj_create(scr_root);
  tab_compass = lv_obj_create(scr_root);
  tab_nav     = lv_obj_create(scr_root);
  tab_tools   = lv_obj_create(scr_root);
  tab_camp    = lv_obj_create(scr_root);
  tab_walk    = lv_obj_create(scr_root);
  tab_data    = lv_obj_create(scr_root);
  tab_batt    = lv_obj_create(scr_root);
  g_tabs[0] = tab_home;  g_tabs[1] = tab_compass; g_tabs[2] = tab_nav;  g_tabs[3] = tab_tools;
  g_tabs[4] = tab_camp;  g_tabs[5] = tab_walk;    g_tabs[6] = tab_data; g_tabs[7] = tab_batt;
  for (int i = 0; i < M_NTABS; i++) {
    lv_obj_t *t = g_tabs[i];
    lv_obj_remove_style_all(t);
    lv_obj_set_size(t, LCD_WIDTH, LCD_HEIGHT);
    lv_obj_set_pos (t, 0, 0);
    lv_obj_set_style_bg_color(t, lv_color_hex(T_BG), 0);
    lv_obj_set_style_bg_opa  (t, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all (t, 0, 0);
    lv_obj_clear_flag(t, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag  (t, LV_OBJ_FLAG_HIDDEN);   // hidden until selected
  }

  build_home_tab();
  build_compass_tab();
  build_nav_tab();
  build_tools_tab();
  build_camp_tab();
  build_walk_tab();
  build_data_tab();
  build_batt_tab();

  toast_lbl = lv_label_create(lv_layer_top());
  lv_label_set_text(toast_lbl, "");
  lv_obj_set_style_text_color(toast_lbl, lv_color_hex(T_AMBER), 0);
  lv_obj_set_style_text_font (toast_lbl, &lv_font_montserrat_14, 0);
  lv_obj_set_style_bg_color(toast_lbl, lv_color_hex(0x140000), 0);
  lv_obj_set_style_bg_opa  (toast_lbl, LV_OPA_COVER, 0);
  lv_obj_set_style_pad_all (toast_lbl, 8, 0);
  lv_obj_set_style_radius  (toast_lbl, 8, 0);
  lv_obj_align(toast_lbl, LV_ALIGN_TOP_MID, 0, 6);
  lv_obj_add_flag(toast_lbl, LV_OBJ_FLAG_HIDDEN);

  build_flash_overlay();
  build_glitch_overlay();
  sw_build_overlay();   // v2.5: full-screen stopwatch + lap list

  // v2.5 accessibility: current-tab name shown above the dots so the user
  // always knows where they are and can see swipe affordances.
  tab_name_lbl = lv_label_create(lv_layer_top());
  lv_label_set_text(tab_name_lbl, "HOME");
  lv_obj_set_style_text_color(tab_name_lbl, lv_color_hex(T_RED), 0);
  lv_obj_set_style_text_font (tab_name_lbl, &lv_font_montserrat_14, 0);
  lv_obj_set_style_text_letter_space(tab_name_lbl, 2, 0);
  lv_obj_align(tab_name_lbl, LV_ALIGN_BOTTOM_MID, 0, -20);

  // Page dot row
  lv_obj_t *dot_row = lv_obj_create(lv_layer_top());
  lv_obj_remove_style_all(dot_row);
  lv_obj_set_size(dot_row, 200, 16);
  lv_obj_align(dot_row, LV_ALIGN_BOTTOM_MID, 0, -2);
  lv_obj_set_flex_flow(dot_row, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(dot_row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_column(dot_row, 8, 0);
  for (int i = 0; i < M_NTABS; i++) {
    page_dots[i] = lv_obj_create(dot_row);
    lv_obj_remove_style_all(page_dots[i]);
    lv_obj_set_size(page_dots[i], i == 0 ? 8 : 5, i == 0 ? 8 : 5);
    lv_obj_set_style_radius(page_dots[i], 4, 0);
    lv_obj_set_style_bg_color(page_dots[i],
       lv_color_hex(i == 0 ? T_RED : T_DIM_RED), 0);
    lv_obj_set_style_bg_opa  (page_dots[i], LV_OPA_COVER, 0);
  }
  set_active_tab(0);   // v2.5: show HOME, seed dots + tab-name label

  // v2.5: route horizontal swipes to an instant (no-anim) tab flip. Enabling
  // gesture bubbling on the whole tree means a swipe that starts on any card
  // or button still reaches scr_root's handler.
  tools_enable_bubble(scr_root);
  lv_obj_add_event_cb(scr_root, tab_swipe_gesture_cb, LV_EVENT_GESTURE, NULL);

  // v2.3: PIN overlay removed -- no build_pin_overlay() call here.
  // Apply the saved watchface index so we boot into the user's choice
  // (silent -- the splash/boot toast handles user feedback).
  home_face_apply(home_face_load_idx(), false);
  lv_scr_load(scr_root);
}

// =============================================================================
// Animated splash (typing + radar warm-up)
// =============================================================================
static void show_splash() {
  gfx->fillScreen(0x0000);
  // Radar rings appearing in
  for (int r = 60; r <= 240; r += 60) {
    gfx->drawCircle(LCD_WIDTH/2, LCD_HEIGHT/2, r, gfx->color565(0x60, 0x10, 0x10));
    delay(80);
  }
  gfx->drawLine(LCD_WIDTH/2, 60, LCD_WIDTH/2, LCD_HEIGHT-60, gfx->color565(0x40, 0x08, 0x08));
  gfx->drawLine(40, LCD_HEIGHT/2, LCD_WIDTH-40, LCD_HEIGHT/2, gfx->color565(0x40, 0x08, 0x08));

  // Typing title
  const char *title = "TACTICAL//OS";
  gfx->setTextSize(3);
  gfx->setTextColor(gfx->color565(0xFF, 0x30, 0x30));
  panel_on(180);
  for (int i = 0; title[i]; i++) {
    gfx->setCursor(80 + i*18, LCD_HEIGHT/2 - 12);
    gfx->print(title[i]);
    delay(45);
  }
  gfx->setTextSize(1);
  gfx->setTextColor(gfx->color565(0x8A, 0x10, 0x10));
  const char *line2 = "INITIALIZING OPERATOR LINK...";
  for (int i = 0; line2[i]; i++) {
    gfx->setCursor(95 + i*6, LCD_HEIGHT/2 + 30);
    gfx->print(line2[i]);
    delay(18);
  }
  // Radar sweep
  for (int s = 0; s < 360; s += 12) {
    float a = s * M_PI / 180.0f;
    int x = LCD_WIDTH/2 + (int)(220*sinf(a));
    int y = LCD_HEIGHT/2 - (int)(220*cosf(a));
    gfx->drawLine(LCD_WIDTH/2, LCD_HEIGHT/2, x, y, gfx->color565(0x60,0x10,0x10));
    delay(8);
  }
}

// =============================================================================
// setup / loop
// =============================================================================
void setup() {
  launcher_app_boot();
  show_splash();
  qmi_init();
  time_restore_from_nvs();
  hist_load();
  wp_load();
  // v2.3: pin_load() removed -- PIN lock no longer exists.
  wifi_load_from_sd();

  SD_MMC.mkdir("/WATCH");
  SD_MMC.mkdir("/WATCH/wifi");
  SD_MMC.mkdir("/WATCH/PHOTOS");
  SD_MMC.mkdir("/WATCH/bin");
  SD_MMC.mkdir("/WATCH/tactical");
  SD_MMC.mkdir("/WATCH/tactical/logs");

  build_ui();
  setCpuFrequencyMhz(160);

  // v2.3: PIN lock removed -- watch boots straight to the home tab.

  time_t now; time(&now);
  bool need_ntp = !g_time_valid || (now < 1700000000)
                  || ((uint32_t)now - g_last_ntp_ok > 6UL * 3600UL);
  if (need_ntp) wifi_sync_once();
  if (home_sync_d)
    lv_obj_set_style_bg_color(home_sync_d,
       lv_color_hex(g_wifi_synced ? T_OK : T_DANGER), 0);

  update_clock_labels();
  update_battery_card();
  update_battery_panel();
  update_camp_panel();
  apply_theme();
  log_event("BOOT");
  show_toast("SWIPE < > OR TAP EDGES TO NAVIGATE", 2600);
}

void loop() {
  uint32_t now = millis();
  lv_timer_handler();
  tick_toast();
  tick_glitch();
  tick_beacon();
  tick_morse();

  // BOOT-button hardware exit
  static uint32_t btn_t = 0;
  if (digitalRead(BOOT_BTN_PIN) == LOW) {
    if (!btn_t) btn_t = now;
    else if (now - btn_t > 1500) { log_event("EXIT to launcher"); return_to_launcher(); }
  } else btn_t = 0;

  // IMU @ ~50 Hz
  static uint32_t imu_t = 0;
  if (now - imu_t >= 20) {
    imu_t = now;
    update_heading();
    update_steps();
  }

  // 250ms updates (radar redraw)
  static uint32_t qt = 0;
  if (now - qt >= 250) {
    qt = now;
    draw_compass();
    draw_nav();
    draw_home_radar();
  }

  // 1s updates
  static uint32_t tick_t = 0;
  if (now - tick_t >= 1000) {
    tick_t = now;
    update_clock_labels();
    static int bcnt = 0;
    if (++bcnt >= 5) { bcnt = 0; update_battery_card(); update_battery_panel(); }
    update_camp_panel();
    update_compass_labels();
    update_walk_panel();
    update_nav_labels();
    update_tools_panel();
    draw_graph();
    adaptive_night_tick();
    steps_rollover_check();
    // periodic stride save
    static int sc = 0;
    if (++sc >= 60) { sc = 0; hist_save(); }
  }

  delay(5);
}
