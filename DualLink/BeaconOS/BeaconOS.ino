// =============================================================================
// BeaconOS - DualCore Link local mesh tracker
// -----------------------------------------------------------------------------
// PATCHED build - fixes:
//   * 'ESP_MAC_BT' was not declared in this scope
//       -> added #include "esp_mac.h"
//       -> ESP32-S3 doesn't have BT-Classic; ESP_MAC_BT is guarded, and we
//          fall back to ESP_MAC_WIFI_STA (BT MAC on S3 is derived from it).
//   * cannot convert 'std::string' to 'String'  (ESP32 Arduino core 3.x)
//       -> BLEAdvertisementData::setManufacturerData() now takes Arduino String
//       -> BLEAdvertisedDevice::getManufacturerData()  now returns Arduino String
//   * "nothing turning on" + I2C NACK spam on Waveshare AMOLED-1.8
//       -> Restored XPowersLib but used the correct concrete class
//          `XPowersAXP2101` (the demo repo confirms an AXP2101 is on-board).
//       -> Added the Adafruit_XCA9554 GPIO expander init: pins 0/1/2 must be
//          pulsed LOW then HIGH to power the AMOLED panel. Missing this step
//          is THE reason nothing lit up.
//       -> Switched display driver to Arduino_SH8601 (v1) / Arduino_CO5300
//          (v2, default) selectable via BOARD_AMOLED_1_8_REV.
//       -> Switched touch driver to Arduino_FT3x68 (v1) / Arduino_CST816x
//          (v2) via the same board macro.
//       -> All pins, addresses and constructor arguments taken verbatim from
//          waveshareteam/ESP32-S3-Touch-AMOLED-1.8 example 09.
//   * wf_time / wf_date / wf_battery / wf_peers used before declaration in
//     status_bar_tick()  -> declarations moved above the function.
//   * ui_watchface() called before declaration in home_on_gesture()
//       -> added forward declaration.
//   * Missing #include <Wire.h> for Wire.begin().
//
// Target boards:
//   * Waveshare ESP32-S3-Touch-AMOLED 2.06" (watch)  <- default
//   * Waveshare ESP32-S3-Touch-AMOLED 1.8"  (beacon)
// LVGL 8.4 compatible.
// Author: Ithiel (StrawHaIthi) - DualCore project - 2026
// =============================================================================

#include <Arduino.h>
#include <Wire.h>
#include <SD_MMC.h>
#include <Update.h>
#include <Preferences.h>
#include <WiFi.h>
#include <esp_wifi.h>
#include <esp_bt.h>
#include <esp_bt_main.h>
#include <esp_timer.h>        // <-- FIX: LVGL tick source (was missing entirely)
#include "esp_mac.h"          // <-- FIX: gives us esp_read_mac() + ESP_MAC_* enums
#include "HWCDC.h"            // <-- FIX: native USB CDC so Serial prints show up
                              //     over USB regardless of Arduino IDE settings.

// ---------------------------------------------------------------------------
// USB Serial compatibility - matches the S3_MiniOS / Widget OS reference.
// On the S3 with native USB, `Serial` only maps to USB when
// ARDUINO_USB_CDC_ON_BOOT=1 in the board menu. If a user forgets to flip
// that setting, every Serial.println here would silently go out UART0
// (GPIO 43/44) and the Serial Monitor would show nothing - which is exactly
// the "no serial output" symptom we saw. Force a HWCDC-backed alias and
// route Serial through it when CDC-on-boot is disabled.
// ---------------------------------------------------------------------------
#if ARDUINO_USB_CDC_ON_BOOT
  // Serial already IS USB CDC, nothing to do.
#else
  #if !defined(USBSerial)
    HWCDC USBSerial;
  #endif
  #undef  Serial
  #define Serial USBSerial
#endif

#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLEScan.h>
#include <BLEAdvertisedDevice.h>
#include <BLEAdvertising.h>

#include <lvgl.h>
#include <Arduino_GFX_Library.h>

// ---------------------------------------------------------------------------
// Colour-macro compatibility shim   <-- FIX: 'BLACK' was not declared in this scope
// ---------------------------------------------------------------------------
// Recent releases of "GFX Library for Arduino" renamed the plain colour
// macros (BLACK, WHITE, ...) to RGB565_BLACK / RGB565_WHITE. Older releases
// only define BLACK. These guards make the sketch build on EITHER version.
#ifndef BLACK
  #ifdef RGB565_BLACK
    #define BLACK RGB565_BLACK
  #else
    #define BLACK 0x0000
  #endif
#endif

#include <Arduino_DriveBus_Library.h>
#include <Adafruit_XCA9554.h>            // I2C GPIO expander that powers the panel
#define XPOWERS_CHIP_AXP2101
#include <XPowersLib.h>                  // AXP2101 PMU is on this board after all
// The prior "no PMU on this board" comment was wrong: the demo repo
// (github.com/waveshareteam/ESP32-S3-Touch-AMOLED-1.8) shows an AXP2101 PMU
// AND a XCA9554 I2C GPIO expander at 0x20 that gates the display power. If
// the expander pins 0/1/2 are never pulled HIGH, the panel is unpowered and
// nothing at all appears — which is what was happening.

// painlessMesh is optional
#if __has_include(<painlessMesh.h>)
  #include <painlessMesh.h>
  #define HAVE_MESH 1
#else
  #define HAVE_MESH 0
#endif

// -----------------------------------------------------------------------------
// ESP_MAC_BT compatibility shim
// -----------------------------------------------------------------------------
// On ESP32-S3 the Bluetooth radio is BLE-only and some builds of the Arduino
// core / esp-idf omit ESP_MAC_BT from esp_mac_type_t. If it's missing we fall
// back to ESP_MAC_WIFI_STA — the BT MAC is derived from the WiFi STA MAC on
// the S3 anyway (differs only in one byte), so as a *stable device ID* they
// are equally good.
#ifndef DC_MAC_TYPE
  #if defined(ESP_MAC_BT)
    #define DC_MAC_TYPE ESP_MAC_BT
  #else
    #define DC_MAC_TYPE ESP_MAC_WIFI_STA
  #endif
#endif

// -----------------------------------------------------------------------------
// Board lock — this firmware targets the ESP32-S3-Touch-AMOLED-1.8 ONLY.
// The 2.06" watch board is no longer supported; keep this at 0.
// -----------------------------------------------------------------------------
#define BOARD_WATCH_206 0

#if BOARD_WATCH_206
  static constexpr uint16_t SCR_W = 410;
  static constexpr uint16_t SCR_H = 502;
#else
  static constexpr uint16_t SCR_W = 368;
  static constexpr uint16_t SCR_H = 448;
#endif

// -----------------------------------------------------------------------------
// Custom BLE advertising payload
// -----------------------------------------------------------------------------
static constexpr uint16_t DC_MFG_ID   = 0x01DC;
static constexpr uint8_t  DC_PROTO_V1 = 0x01;

enum Role : uint8_t { ROLE_WATCH = 0, ROLE_BEACON = 1, ROLE_BOTH = 2 };
enum Transport : uint8_t { TX_BLE_ONLY = 0, TX_MESH_ONLY = 1, TX_BOTH = 2 };

struct Flags {
  uint8_t broadcast : 1;
  uint8_t findme    : 1;
  uint8_t sos       : 1;
  uint8_t lowbat    : 1;
  uint8_t _rsv      : 4;
};

// -----------------------------------------------------------------------------
// Runtime settings
// -----------------------------------------------------------------------------
struct Settings {
  char      name[7]   = "DUAL01";
  Role      role      = ROLE_BOTH;
  Transport transport = TX_BLE_ONLY;
  bool      broadcast = true;
  bool      visible   = true;
  bool      friendOnly= false;
  uint16_t  scanEveryMs = 5000;
  int8_t    rssiAt1m   = -59;
  float     pathLoss   = 2.5f;
  // NEW: screen timeout (0 = never, else seconds before dim/off).
  uint16_t  screenTimeoutSec = 15;
  // NEW: backlight brightness while awake (0..255).
  uint8_t   brightness = 200;
};
static Settings CFG;
static Preferences NVS;

// -----------------------------------------------------------------------------
// Nearby-device table
// -----------------------------------------------------------------------------
struct Peer {
  uint8_t  id[6]   = {0};
  char     name[8] = "";
  uint8_t  role    = ROLE_BOTH;
  uint8_t  battery = 0;
  int8_t   rssi    = -127;
  float    distanceM = -1.0f;
  uint32_t lastSeenMs = 0;
  bool     sos       = false;
  bool     inUse     = false;
};
static constexpr size_t MAX_PEERS = 24;
static Peer peers[MAX_PEERS];
static SemaphoreHandle_t peersMx;

// -----------------------------------------------------------------------------
// Log ring buffer
// -----------------------------------------------------------------------------
struct LogEntry {
  uint32_t tsMs;
  uint8_t  peerIdx;
  int8_t   rssi;
  float    distanceM;
};
static constexpr size_t LOG_CAP = 512;
static LogEntry logs[LOG_CAP];
static size_t   logHead = 0;
static size_t   logCount = 0;

// -----------------------------------------------------------------------------
// Forward decls (LVGL screens)   <-- FIX: added ui_watchface()
// -----------------------------------------------------------------------------
static lv_obj_t *scr_splash = nullptr;   // FIX: keep splash handle so we can delete it
static bool sd_ready = false;            // FIX: only touch SD_MMC.exists() when begin() succeeded
static void ui_boot_splash();
static void ui_watchface();
static void ui_home();
static void ui_find(uint8_t peerIdx);
static void ui_logs();
static void ui_settings();
static void ui_sos();               // NEW: full SOS tab (was ui_sos_broadcast toast)
static void ui_sos_broadcast();     // legacy alias, kept for external callers
static void ui_back_to_launcher();

// -----------------------------------------------------------------------------
// Tab navigation (NEW)
// -----------------------------------------------------------------------------
// Order: HOME <-> LOGS <-> SOS <-> SETTINGS  (swipe left / right cycles).
enum Tab : uint8_t { TAB_HOME = 0, TAB_LOGS = 1, TAB_SOS = 2, TAB_SETTINGS = 3, TAB_COUNT = 4 };
static Tab      g_current_tab = TAB_HOME;
static uint32_t g_last_activity_ms = 0;   // for screen timeout
static bool     g_screen_asleep    = false;
static void nav_to_tab(Tab t, bool from_left);
static void nav_swipe_left();   // finger swipes left -> show next tab
static void nav_swipe_right();  // finger swipes right -> show prev tab
static void wake_screen();
static void sleep_screen();
static void set_backlight(uint8_t b);

// -----------------------------------------------------------------------------
// LVGL / display glue
// -----------------------------------------------------------------------------
Arduino_DataBus *bus  = nullptr;
Arduino_GFX     *gfx  = nullptr;
static lv_disp_draw_buf_t draw_buf;
static lv_color_t         *buf1 = nullptr;
static lv_color_t         *buf2 = nullptr;

// ---------------------------------------------------------------------------
// Board pin map — Waveshare ESP32-S3-Touch-AMOLED-1.8
// ---------------------------------------------------------------------------
// Source of truth: waveshareteam/ESP32-S3-Touch-AMOLED-1.8 demo repo,
// examples/Arduino-v3.3.5[-v2]/libraries/Mylibrary/pin_config.h
//
// There are TWO hardware revisions with identical pinouts:
//   BOARD_AMOLED_1_8_REV = 1  ->  SH8601 display + FT3168 touch  (v1)  <- THIS BUILD
//   BOARD_AMOLED_1_8_REV = 2  ->  CO5300 display + CST816 touch  (v2, newer)
// Set to your board revision. This build defaults to v1 as requested.
#ifndef BOARD_AMOLED_1_8_REV
  #define BOARD_AMOLED_1_8_REV 1
#endif

#define LCD_SDIO0    4
#define LCD_SDIO1    5
#define LCD_SDIO2    6
#define LCD_SDIO3    7
#define LCD_SCLK    11
#define LCD_CS      12
#define LCD_WIDTH  368
#define LCD_HEIGHT 448

#define IIC_SDA     15
#define IIC_SCL     14
#define TP_INT      21

// XCA9554 I2C GPIO expander sits at 0x20 and gates panel power / touch reset.
// Without pulling pins 0/1/2 HIGH (after a 20 ms LOW pulse) the display is
// completely unpowered — this is *the* reason "nothing turned on".
static constexpr uint8_t XCA9554_ADDR = 0x20;
Adafruit_XCA9554 expander;

// ---- Touch driver (chip picked per board rev) ----
std::shared_ptr<Arduino_IIC_DriveBus> IIC_Bus = nullptr;
std::unique_ptr<Arduino_IIC>          TP      = nullptr;
static void IRAM_ATTR Arduino_IIC_Touch_Interrupt() {
  if (TP) TP->IIC_Interrupt_Flag = true;
}

// ---- AXP2101 PMU ----
XPowersAXP2101 pmu;

// ---- Global panel pointer for backlight control (NEW) ---------------------
// setBrightness() only exists on the concrete driver class, so we keep a
// typed global pointer that gets assigned right after `new Arduino_SH8601`
// (or CO5300) in setup(). This is what powers the screen-timeout feature.
#if BOARD_AMOLED_1_8_REV == 1
static Arduino_SH8601 *g_panel = nullptr;
#else
static Arduino_CO5300 *g_panel = nullptr;
#endif

static void set_backlight(uint8_t b) {
  if (g_panel) g_panel->setBrightness(b);
}

// ---- Screens (declared early so status_bar_tick can see wf_*) --------------
// FIX: moved wf_* + scr_* declarations ABOVE status_bar_tick().
static lv_obj_t *scr_watchface= nullptr;
static lv_obj_t *scr_home     = nullptr;
static lv_obj_t *scr_find     = nullptr;
static lv_obj_t *scr_logs     = nullptr;
static lv_obj_t *scr_settings = nullptr;

static lv_obj_t *wf_time      = nullptr;
static lv_obj_t *wf_date      = nullptr;
static lv_obj_t *wf_battery   = nullptr;
static lv_obj_t *wf_peers     = nullptr;
static lv_obj_t *wf_hint      = nullptr;

static lv_obj_t *bar_battery = nullptr;
static lv_obj_t *bar_clock   = nullptr;
static lv_obj_t *bar_role    = nullptr;

static lv_obj_t *home_list   = nullptr;

// ---- Home peer list forwards ----------------------------------------------
static void refresh_home_list();

static void my_disp_flush(lv_disp_drv_t *d, const lv_area_t *a, lv_color_t *p) {
  uint32_t w = a->x2 - a->x1 + 1;
  uint32_t h = a->y2 - a->y1 + 1;
  if (gfx) {
    // Match the reference S3_MiniOS / Widget OS flush - honour LV_COLOR_16_SWAP.
    // On this AMOLED panel lv_conf.h ships with LV_COLOR_16_SWAP=1, so calling
    // draw16bitRGBBitmap unconditionally sends bytes in the wrong order and
    // the panel shows a black screen after the splash. Using the Be variant
    // fixes that; the else branch keeps things sane if the flag is ever 0.
#if (LV_COLOR_16_SWAP != 0)
    gfx->draw16bitBeRGBBitmap(a->x1, a->y1, (uint16_t*)&p->full, w, h);
#else
    gfx->draw16bitRGBBitmap(a->x1, a->y1, (uint16_t*)&p->full, w, h);
#endif
  }
  lv_disp_flush_ready(d);
}

static void my_touch_read(lv_indev_drv_t *d, lv_indev_data_t *data) {
  extern bool dc_touch_read(int16_t *x, int16_t *y);
  int16_t x, y;
  if (dc_touch_read(&x, &y)) {
    // Debug: print every real touch so you can confirm the panel is working.
    Serial.printf("[touch] x=%d y=%d\n", x, y);
    // NEW: if the screen is asleep, any touch wakes it but is not delivered
    // to LVGL — the user should not accidentally activate a button while
    // reaching to wake the device.
    if (g_screen_asleep) {
      wake_screen();
      data->state = LV_INDEV_STATE_RELEASED;
      return;
    }
    g_last_activity_ms = millis();
    data->point.x = x;
    data->point.y = y;
    data->state   = LV_INDEV_STATE_PRESSED;
  } else {
    data->state   = LV_INDEV_STATE_RELEASED;
  }
}

// LVGL tick source (NEW - the reason touch/gestures were dead).
// LVGL needs a monotonic ms counter; without lv_tick_inc() being called
// from a timer, gesture detection, click debouncing and animations all
// silently fail. The Waveshare demo uses an esp_timer at 2 ms cadence.
static void lv_tick_task(void *arg) {
  (void)arg;
  lv_tick_inc(2);
}

// -----------------------------------------------------------------------------
// Screen timeout (NEW)
// -----------------------------------------------------------------------------
static lv_obj_t *scr_sleep      = nullptr;  // full-black screen shown when off
static lv_obj_t *scr_before_off = nullptr;  // what to restore on wake
static void wake_screen() {
  if (!g_screen_asleep) {
    g_last_activity_ms = millis();
    if (lv_disp_get_default()) lv_disp_trig_activity(NULL);
    return;
  }
  g_screen_asleep    = false;
  g_last_activity_ms = millis();
  if (lv_disp_get_default()) lv_disp_trig_activity(NULL);
  // Restore backlight *before* restoring the screen so the panel is ready
  // by the time the first frame lands.
  set_backlight(CFG.brightness ? CFG.brightness : 200);
  if (scr_before_off) {
    lv_scr_load(scr_before_off);
    scr_before_off = nullptr;
  }
  Serial.println("[sleep] wake");
}
static void sleep_screen() {
  if (g_screen_asleep) return;
  g_screen_asleep = true;
  scr_before_off = lv_scr_act();
  // Lazily build the black sleep screen the first time we need it.
  if (!scr_sleep) {
    scr_sleep = lv_obj_create(nullptr);
    lv_obj_set_style_bg_color(scr_sleep, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(scr_sleep, LV_OPA_COVER, 0);
    lv_obj_clear_flag(scr_sleep, LV_OBJ_FLAG_SCROLLABLE);
  }
  lv_scr_load(scr_sleep);
  // Force LVGL to actually push the black frame BEFORE we drop the backlight
  // - otherwise on some AMOLEDs the last frame would linger visibly.
  for (int i = 0; i < 3; ++i) lv_timer_handler();
  lv_refr_now(NULL);
  set_backlight(0);
  Serial.println("[sleep] screen off");
}
static void screen_timeout_tick() {
  if (CFG.screenTimeoutSec == 0) return;              // "Never"
  if (g_screen_asleep) return;
  // Use LVGL's own inactive-time counter as the source of truth - it is
  // updated ONLY by real user input coming through the pointer indev, so
  // it can't be fooled by spurious touch-controller interrupts the way a
  // millis()-based counter can.
  uint32_t idleMs = lv_disp_get_inactive_time(NULL);
  if (idleMs >= (uint32_t)CFG.screenTimeoutSec * 1000UL) {
    sleep_screen();
  }
}

// -----------------------------------------------------------------------------
// BLE — advertising side
// -----------------------------------------------------------------------------
static BLEAdvertising *adv = nullptr;
static uint8_t         mfgBuf[22];

static void ble_build_payload(uint8_t battery, Flags f, uint32_t uptimeS) {
  mfgBuf[0]  = DC_MFG_ID & 0xFF;
  mfgBuf[1]  = DC_MFG_ID >> 8;
  mfgBuf[2]  = DC_PROTO_V1;
  mfgBuf[3]  = (uint8_t)CFG.role;
  memcpy(&mfgBuf[4], &f, 1);
  mfgBuf[5]  = battery;

  uint8_t mac[6] = {0};
  // FIX: DC_MAC_TYPE resolves to ESP_MAC_BT if available, else ESP_MAC_WIFI_STA
  esp_read_mac(mac, (esp_mac_type_t)DC_MAC_TYPE);
  memcpy(&mfgBuf[6], mac, 6);

  memcpy(&mfgBuf[12], &uptimeS, 4);
  memset(&mfgBuf[16], ' ', 6);
  size_t n = strnlen(CFG.name, 6);
  memcpy(&mfgBuf[16], CFG.name, n);
}

static void ble_start_advertising() {
  if (!adv) return;
  BLEAdvertisementData ad;
  ad.setFlags(0x06);
  // FIX: ESP32 Arduino core 3.x expects Arduino String here (was std::string in 2.x).
  String mfg;
  mfg.reserve(sizeof(mfgBuf));
  for (size_t i = 0; i < sizeof(mfgBuf); ++i) mfg += (char)mfgBuf[i];
  ad.setManufacturerData(mfg);
  adv->setAdvertisementData(ad);
  adv->setScanResponse(false);
  adv->setMinInterval(160);
  adv->setMaxInterval(320);
  adv->start();
}

static void ble_stop_advertising() { if (adv) adv->stop(); }

// -----------------------------------------------------------------------------
// BLE — scanner side
// -----------------------------------------------------------------------------
static BLEScan *scanner = nullptr;

static float rssi_to_distance(int8_t rssi) {
  if (rssi == 0) return -1.0f;
  float ratio = (float)(CFG.rssiAt1m - rssi) / (10.0f * CFG.pathLoss);
  return powf(10.0f, ratio);
}

static int find_or_alloc_peer(const uint8_t id[6]) {
  int freeSlot = -1;
  for (size_t i = 0; i < MAX_PEERS; ++i) {
    if (peers[i].inUse && memcmp(peers[i].id, id, 6) == 0) return (int)i;
    if (!peers[i].inUse && freeSlot < 0) freeSlot = (int)i;
  }
  if (freeSlot >= 0) {
    peers[freeSlot].inUse = true;
    memcpy(peers[freeSlot].id, id, 6);
    return freeSlot;
  }
  uint32_t oldestT = UINT32_MAX; int oldestI = 0;
  for (size_t i = 0; i < MAX_PEERS; ++i)
    if (peers[i].lastSeenMs < oldestT) { oldestT = peers[i].lastSeenMs; oldestI = (int)i; }
  memset(&peers[oldestI], 0, sizeof(Peer));
  peers[oldestI].inUse = true;
  memcpy(peers[oldestI].id, id, 6);
  return oldestI;
}

class ScanCB : public BLEAdvertisedDeviceCallbacks {
public:
  void onResult(BLEAdvertisedDevice dev) override {
    if (!dev.haveManufacturerData()) return;
    // FIX: ESP32 Arduino core 3.x returns Arduino String from getManufacturerData()
    //      (core 2.x returned std::string). We copy into a raw byte buffer so the
    //      rest of the parsing code stays byte-oriented and version-agnostic.
    String md = dev.getManufacturerData();
    if (md.length() < sizeof(mfgBuf)) return;
    uint8_t buf[sizeof(mfgBuf)];
    for (size_t i = 0; i < sizeof(mfgBuf); ++i) buf[i] = (uint8_t)md[i];
    const uint8_t *p = buf;
    uint16_t mid = p[0] | (p[1] << 8);
    if (mid != DC_MFG_ID) return;
    if (p[2] != DC_PROTO_V1) return;

    uint8_t role = p[3];
    Flags   f;    memcpy(&f, &p[4], 1);
    uint8_t bat  = p[5];
    const uint8_t *id = &p[6];
    char name[8] = {0}; memcpy(name, &p[16], 6);
    for (int i = 5; i >= 0 && name[i] == ' '; --i) name[i] = 0;

    xSemaphoreTake(peersMx, portMAX_DELAY);
    int idx = find_or_alloc_peer(id);
    bool isNew = (peers[idx].lastSeenMs == 0);
    peers[idx].role       = role;
    peers[idx].battery    = bat;
    peers[idx].rssi       = dev.getRSSI();
    peers[idx].distanceM  = rssi_to_distance(peers[idx].rssi);
    peers[idx].lastSeenMs = millis();
    peers[idx].sos        = f.sos;
    strncpy(peers[idx].name, name, 7);

    logs[logHead].tsMs      = peers[idx].lastSeenMs;
    logs[logHead].peerIdx   = (uint8_t)idx;
    logs[logHead].rssi      = peers[idx].rssi;
    logs[logHead].distanceM = peers[idx].distanceM;
    logHead = (logHead + 1) % LOG_CAP;
    if (logCount < LOG_CAP) logCount++;
    xSemaphoreGive(peersMx);

    if (isNew) {
      Serial.printf("[ble] NEW peer '%s'  role=%u  rssi=%ddBm  dist=%.1fm\n",
                    name[0] ? name : "??????", role,
                    peers[idx].rssi, peers[idx].distanceM);
    }
  }
};
static ScanCB scanCb;

static void ble_scan_once() {
  if (!scanner) return;
  scanner->start(1, false);
  scanner->clearResults();
}

// -----------------------------------------------------------------------------
// Optional ESP-MESH
// -----------------------------------------------------------------------------
#if HAVE_MESH
static painlessMesh mesh;
static const char *MESH_PREFIX = "DualCoreLink";
static const char *MESH_PASS   = "dc-mesh-2026";
static const uint16_t MESH_PORT = 5555;

static void on_mesh_msg(uint32_t from, String &msg) {
  if (msg.startsWith("SOS|")) {
    // TODO: toast
  }
}
static void mesh_start() {
  mesh.setDebugMsgTypes(ERROR | STARTUP);
  mesh.init(MESH_PREFIX, MESH_PASS, MESH_PORT);
  mesh.onReceive(&on_mesh_msg);
}
static void mesh_stop() { mesh.stop(); }
#else
static void mesh_start() {}
static void mesh_stop()  {}
#endif

// -----------------------------------------------------------------------------
// Battery / clock / status bar
// -----------------------------------------------------------------------------
static uint8_t read_battery_pct() {
  // AXP2101 exposes a fuel-gauge percentage directly.
  int p = pmu.getBatteryPercent();
  if (p < 0) return 0;
  if (p > 100) return 100;
  return (uint8_t)p;
}

static void status_bar_tick() {
  uint8_t b = read_battery_pct();
  time_t now = time(nullptr);
  struct tm tm; localtime_r(&now, &tm);

  if (bar_battery) {
    lv_label_set_text_fmt(bar_battery, LV_SYMBOL_BATTERY_FULL "  %u%%", b);
    lv_label_set_text_fmt(bar_clock, "%02d:%02d", tm.tm_hour, tm.tm_min);
    const char *r =
        CFG.role == ROLE_WATCH  ? "WATCH"
      : CFG.role == ROLE_BEACON ? "BEACON"
      :                           "BOTH";
    lv_label_set_text(bar_role, r);
  }

  if (wf_time) {
    lv_label_set_text_fmt(wf_time, "%02d:%02d", tm.tm_hour, tm.tm_min);
    static const char *dow[] = {"SUN","MON","TUE","WED","THU","FRI","SAT"};
    lv_label_set_text_fmt(wf_date, "%s  %02d.%02d",
                          dow[tm.tm_wday % 7], tm.tm_mday, tm.tm_mon + 1);
    lv_label_set_text_fmt(wf_battery, LV_SYMBOL_BATTERY_FULL "  %u%%", b);
    int live = 0, sos = 0;
    xSemaphoreTake(peersMx, portMAX_DELAY);
    for (size_t i = 0; i < MAX_PEERS; ++i) {
      if (!peers[i].inUse) continue;
      if (millis() - peers[i].lastSeenMs > 60000) continue;
      live++;
      if (peers[i].sos) sos++;
    }
    xSemaphoreGive(peersMx);
    if (sos > 0)
      lv_label_set_text_fmt(wf_peers, "%d NEARBY   %d SOS", live, sos);
    else
      lv_label_set_text_fmt(wf_peers, "%d NEARBY", live);
  }
}

// =============================================================================
//                                LVGL UI
// =============================================================================
static void build_status_bar(lv_obj_t *parent) {
  lv_obj_t *bar = lv_obj_create(parent);
  lv_obj_set_size(bar, LV_HOR_RES, 34);
  lv_obj_align(bar, LV_ALIGN_TOP_MID, 0, 0);
  lv_obj_set_style_bg_color(bar, lv_color_hex(0x0a0f14), 0);
  lv_obj_set_style_border_width(bar, 0, 0);
  lv_obj_set_style_shadow_width(bar, 0, 0);   // perf: no shadow ever
  lv_obj_set_style_pad_all(bar, 6, 0);

  bar_battery = lv_label_create(bar);
  lv_obj_align(bar_battery, LV_ALIGN_LEFT_MID, 4, 0);
  lv_obj_set_style_text_color(bar_battery, lv_color_hex(0x7cf0d6), 0);

  bar_clock = lv_label_create(bar);
  lv_obj_align(bar_clock, LV_ALIGN_CENTER, 0, 0);
  lv_obj_set_style_text_color(bar_clock, lv_color_hex(0xe6f4ff), 0);

  bar_role = lv_label_create(bar);
  lv_obj_align(bar_role, LV_ALIGN_RIGHT_MID, -6, 0);
  lv_obj_set_style_text_color(bar_role, lv_color_hex(0xffb84d), 0);
}

// =============================================================================
//                     Tab navigation + shared UI helpers  (NEW)
// =============================================================================
//
// The four tabs (HOME, LOGS, SOS, SETTINGS) can be reached by swiping left or
// right anywhere on the screen. Every tab installs `tab_on_gesture` as its
// LV_EVENT_GESTURE callback and the same handler decides which direction to
// animate.
//
// SPECIAL: The SOS tab is REACHED by swipe like any other tab, but the
// actual broadcast is triggered by a dedicated button so a swipe cannot fire
// an emergency by accident.
// -----------------------------------------------------------------------------
static lv_obj_t *tab_dots[TAB_COUNT] = {nullptr, nullptr, nullptr, nullptr};

static const char *tab_name(Tab t) {
  switch (t) {
    case TAB_HOME:     return "HOME";
    case TAB_LOGS:     return "LOGS";
    case TAB_SOS:      return "SOS";
    case TAB_SETTINGS: return "SETTINGS";
    default:           return "";
  }
}
static uint32_t tab_accent(Tab t) {
  switch (t) {
    case TAB_HOME:     return 0x7cf0d6;   // mint
    case TAB_LOGS:     return 0x5ec8ff;   // sky
    case TAB_SOS:      return 0xff5470;   // red
    case TAB_SETTINGS: return 0xc09cff;   // lilac
    default:           return 0xe6f4ff;
  }
}

// -----------------------------------------------------------------------------
// Bottom navigation bar (NEW - replaces the non-interactive dots).
// Every tab shows the same 4-button strip. Tapping a button jumps to that
// tab with an animated slide. The active tab is drawn with its accent color.
// -----------------------------------------------------------------------------
static void on_tab_button_click(lv_event_t *e) {
  Tab dest = (Tab)(uintptr_t)lv_event_get_user_data(e);
  if (dest == g_current_tab) { wake_screen(); return; }
  // Slide direction: if the destination sits AFTER us in the tab order, we
  // slide left (same as a left swipe). Wrap-around uses the shortest arc so
  // the carousel feels natural.
  int delta = (int)dest - (int)g_current_tab;
  if (delta >  (TAB_COUNT/2)) delta -= TAB_COUNT;
  if (delta < -(TAB_COUNT/2)) delta += TAB_COUNT;
  bool from_left = (delta > 0);
  nav_to_tab(dest, from_left);
}

static void build_tab_indicator(lv_obj_t *parent) {
  lv_obj_t *row = lv_obj_create(parent);
  lv_obj_remove_style_all(row);
  lv_obj_set_size(row, LV_HOR_RES - 16, 44);
  lv_obj_align(row, LV_ALIGN_BOTTOM_MID, 0, -8);
  lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN,
                             LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_column(row, 6, 0);
  lv_obj_set_style_bg_color(row, lv_color_hex(0x0a0f14), 0);
  lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
  lv_obj_set_style_radius(row, 22, 0);
  lv_obj_set_style_pad_all(row, 4, 0);
  lv_obj_set_style_border_width(row, 0, 0);
  lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

  for (int i = 0; i < TAB_COUNT; ++i) {
    lv_obj_t *b = lv_btn_create(row);
    lv_obj_set_flex_grow(b, 1);
    lv_obj_set_height(b, 36);
    bool active = (i == (int)g_current_tab);
    lv_obj_set_style_radius(b, 18, 0);
    lv_obj_set_style_border_width(b, 0, 0);
    lv_obj_set_style_bg_color(b,
        lv_color_hex(active ? tab_accent((Tab)i) : 0x0a0f14), 0);
    lv_obj_set_style_bg_opa(b, active ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
    lv_obj_set_style_shadow_width(b, 0, 0);
    lv_obj_add_event_cb(b, on_tab_button_click, LV_EVENT_CLICKED,
                        (void*)(uintptr_t)i);

    lv_obj_t *l = lv_label_create(b);
    lv_label_set_text(l, tab_name((Tab)i));
    lv_obj_center(l);
    lv_obj_set_style_text_color(l,
        lv_color_hex(active ? 0x02060a : tab_accent((Tab)i)), 0);
    tab_dots[i] = b;
  }
}

// -----------------------------------------------------------------------------
// Master gesture handler installed on every tab screen.
// -----------------------------------------------------------------------------
static void tab_on_gesture(lv_event_t *e) {
  lv_dir_t d = lv_indev_get_gesture_dir(lv_indev_get_act());
  // consume the gesture so it doesn't also click a button underneath
  lv_indev_wait_release(lv_indev_get_act());
  if (d == LV_DIR_LEFT)  nav_swipe_left();
  else if (d == LV_DIR_RIGHT) nav_swipe_right();
}

// Animated screen switch. `from_left` decides which way LVGL slides.
static lv_scr_load_anim_t g_next_anim = LV_SCR_LOAD_ANIM_NONE;
static void nav_to_tab(Tab t, bool from_left) {
  Serial.printf("[nav] -> tab %d (%s slide)\n", (int)t, from_left ? "L" : "R");
  wake_screen(); // any nav counts as activity
  g_current_tab = t;
  g_next_anim = from_left ? LV_SCR_LOAD_ANIM_MOVE_LEFT
                          : LV_SCR_LOAD_ANIM_MOVE_RIGHT;
  switch (t) {
    case TAB_HOME:     ui_home();     break;
    case TAB_LOGS:     ui_logs();     break;
    case TAB_SOS:      ui_sos();      break;
    case TAB_SETTINGS: ui_settings(); break;
    default: g_next_anim = LV_SCR_LOAD_ANIM_NONE; return;
  }
  Serial.printf("[nav] -> tab %d done, heap=%u\n", (int)t, (unsigned)ESP.getFreeHeap());
}

// swipe left (finger goes left) -> advance to next tab
static void nav_swipe_left() {
  Tab next = (Tab)((g_current_tab + 1) % TAB_COUNT);
  nav_to_tab(next, /*from_left=*/true);
}
static void nav_swipe_right() {
  Tab prev = (Tab)((g_current_tab + TAB_COUNT - 1) % TAB_COUNT);
  nav_to_tab(prev, /*from_left=*/false);
}

// Attach the master gesture cb + activity-refreshing press cb to a screen.
static void attach_tab_events(lv_obj_t *scr) {
  lv_obj_add_event_cb(scr, tab_on_gesture, LV_EVENT_GESTURE, nullptr);
}

// A screen builder (ui_home / ui_logs / ...) ends with `screen_load(scr)`.
// If the caller set g_next_anim, we load with a slide; otherwise plain load.
static void screen_load(lv_obj_t *scr) {
  if (g_next_anim == LV_SCR_LOAD_ANIM_NONE) {
    lv_scr_load(scr);
  } else {
    lv_scr_load_anim(scr, g_next_anim, 260, 0, false);
  }
  g_next_anim = LV_SCR_LOAD_ANIM_NONE;
}

// ---- Watch face -----------------------------------------------------------
static void wf_on_gesture(lv_event_t *e) {
  lv_dir_t d = lv_indev_get_gesture_dir(lv_indev_get_act());
  if (d == LV_DIR_LEFT)      ui_home();
  else if (d == LV_DIR_TOP)  ui_sos();
}

static void ui_watchface() {
  if (scr_watchface) { lv_scr_load(scr_watchface); status_bar_tick(); return; }

  scr_watchface = lv_obj_create(nullptr);
  lv_obj_set_style_bg_color(scr_watchface, lv_color_hex(0x02060a), 0);
  lv_obj_clear_flag(scr_watchface, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_event_cb(scr_watchface, wf_on_gesture, LV_EVENT_GESTURE, nullptr);

  wf_time = lv_label_create(scr_watchface);
  lv_obj_set_style_text_font(wf_time, &lv_font_montserrat_48, 0);
  lv_obj_set_style_text_color(wf_time, lv_color_hex(0x7cf0d6), 0);
  lv_label_set_text(wf_time, "--:--");
  lv_obj_align(wf_time, LV_ALIGN_CENTER, 0, -60);

  wf_date = lv_label_create(scr_watchface);
  lv_obj_set_style_text_font(wf_date, &lv_font_montserrat_20, 0);
  lv_obj_set_style_text_color(wf_date, lv_color_hex(0xe6f4ff), 0);
  lv_label_set_text(wf_date, "---  --.--");
  lv_obj_align(wf_date, LV_ALIGN_CENTER, 0, -14);

  wf_battery = lv_label_create(scr_watchface);
  lv_obj_set_style_text_color(wf_battery, lv_color_hex(0xffb84d), 0);
  lv_label_set_text(wf_battery, "");
  lv_obj_align(wf_battery, LV_ALIGN_TOP_RIGHT, -12, 12);

  lv_obj_t *tag = lv_label_create(scr_watchface);
  lv_label_set_text(tag, "BEACON // OS");
  lv_obj_set_style_text_color(tag, lv_color_hex(0x7cf0d6), 0);
  lv_obj_align(tag, LV_ALIGN_TOP_LEFT, 12, 12);

  wf_peers = lv_label_create(scr_watchface);
  lv_obj_set_style_text_font(wf_peers, &lv_font_montserrat_18, 0);
  lv_obj_set_style_text_color(wf_peers, lv_color_hex(0x7cf0d6), 0);
  lv_label_set_text(wf_peers, "0 NEARBY");
  lv_obj_align(wf_peers, LV_ALIGN_CENTER, 0, 30);

  wf_hint = lv_label_create(scr_watchface);
  lv_obj_set_style_text_color(wf_hint, lv_color_hex(0x5c6b7a), 0);
  lv_label_set_text(wf_hint, "swipe  <-----  for beacons");
  lv_obj_align(wf_hint, LV_ALIGN_BOTTOM_MID, 0, -14);

  lv_obj_t *sosHint = lv_label_create(scr_watchface);
  lv_obj_set_style_text_color(sosHint, lv_color_hex(0x4a1420), 0);
  lv_label_set_text(sosHint, "pull down for SOS");
  lv_obj_align(sosHint, LV_ALIGN_TOP_MID, 0, 44);

  lv_scr_load(scr_watchface);
  status_bar_tick();
}

// ---- Home -----------------------------------------------------------------
static void on_peer_row(lv_event_t *e) {
  uint8_t idx = (uint8_t)(uintptr_t)lv_event_get_user_data(e);
  ui_find(idx);
}

static void refresh_home_list() {
  if (!home_list) return;
  lv_obj_clean(home_list);
  xSemaphoreTake(peersMx, portMAX_DELAY);
  int shown = 0;
  for (size_t i = 0; i < MAX_PEERS; ++i) {
    if (!peers[i].inUse) continue;
    if (millis() - peers[i].lastSeenMs > 60000) continue;
    lv_obj_t *row = lv_btn_create(home_list);
    lv_obj_set_width(row, LV_PCT(100));
    lv_obj_set_style_bg_color(row, lv_color_hex(0x111a22), 0);
    lv_obj_set_style_border_color(row, lv_color_hex(0x1f2a36), 0);
    lv_obj_add_event_cb(row, on_peer_row, LV_EVENT_CLICKED, (void*)(uintptr_t)i);

    lv_obj_t *l = lv_label_create(row);
    const char *dot = peers[i].sos ? "#" : "*";
    lv_label_set_text_fmt(l, "%s  %-6s   %5.1fm   %ddBm   %u%%",
                          dot, peers[i].name[0] ? peers[i].name : "??????",
                          peers[i].distanceM, peers[i].rssi, peers[i].battery);
    lv_obj_set_style_text_color(l, peers[i].sos ? lv_color_hex(0xff5470) : lv_color_hex(0xe6f4ff), 0);
    shown++;
  }
  xSemaphoreGive(peersMx);

  if (shown == 0) {
    lv_obj_t *l = lv_label_create(home_list);
    lv_label_set_text(l, "no peers in range\nscanning ...");
    lv_obj_set_style_text_color(l, lv_color_hex(0x5c6b7a), 0);
  }
}

static void on_back_to_launcher(lv_event_t *e) { ui_back_to_launcher(); }

static void ui_home() {
  if (scr_home) { screen_load(scr_home); return; }
  scr_home = lv_obj_create(nullptr);
  lv_obj_set_style_bg_color(scr_home, lv_color_hex(0x05090c), 0);
  lv_obj_clear_flag(scr_home, LV_OBJ_FLAG_SCROLLABLE);
  attach_tab_events(scr_home);

  build_status_bar(scr_home);

  // Title block: large tab name + subtitle
  lv_obj_t *title = lv_label_create(scr_home);
  lv_obj_set_style_text_font(title, &lv_font_montserrat_28, 0);
  lv_label_set_text(title, "NEARBY");
  lv_obj_set_style_text_color(title, lv_color_hex(0x7cf0d6), 0);
  lv_obj_align(title, LV_ALIGN_TOP_LEFT, 16, 44);

  lv_obj_t *sub = lv_label_create(scr_home);
  lv_label_set_text(sub, "dualcore link");
  lv_obj_set_style_text_color(sub, lv_color_hex(0x5c6b7a), 0);
  lv_obj_align(sub, LV_ALIGN_TOP_LEFT, 16, 78);

  // Peer list — takes the bulk of the screen
  home_list = lv_obj_create(scr_home);
  lv_obj_set_size(home_list, LV_HOR_RES - 24, LV_VER_RES - 220);
  lv_obj_align(home_list, LV_ALIGN_TOP_MID, 0, 108);
  lv_obj_set_flex_flow(home_list, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_row(home_list, 8, 0);
  lv_obj_set_style_bg_color(home_list, lv_color_hex(0x0a0f14), 0);
  lv_obj_set_style_border_width(home_list, 0, 0);
  lv_obj_set_style_shadow_width(home_list, 0, 0);
  lv_obj_set_style_radius(home_list, 14, 0);

  if (sd_ready && SD_MMC.exists("/WATCH/bin/launcheros.bin")) {
    lv_obj_t *back = lv_btn_create(scr_home);
    lv_obj_set_size(back, LV_HOR_RES - 24, 36);
    lv_obj_align(back, LV_ALIGN_BOTTOM_MID, 0, -60);
    lv_obj_set_style_bg_color(back, lv_color_hex(0x0a0f14), 0);
    lv_obj_set_style_border_color(back, lv_color_hex(0x7cf0d6), 0);
    lv_obj_set_style_border_width(back, 1, 0);
    lv_obj_set_style_radius(back, 22, 0);
    lv_obj_t *bl = lv_label_create(back);
    lv_label_set_text(bl, "< back to launcher");
    lv_obj_set_style_text_color(bl, lv_color_hex(0x7cf0d6), 0);
    lv_obj_center(bl);
    lv_obj_add_event_cb(back, on_back_to_launcher, LV_EVENT_CLICKED, nullptr);
  }

  // Tab-indicator dots at the very bottom
  build_tab_indicator(scr_home);

  screen_load(scr_home);
  refresh_home_list();
}

// ---- Find mode ------------------------------------------------------------
static uint8_t   find_idx = 0;
static lv_obj_t *find_bar = nullptr;
static lv_obj_t *find_dist= nullptr;

static void on_find_back(lv_event_t *e) { ui_home(); }

static void ui_find(uint8_t peerIdx) {
  find_idx = peerIdx;
  if (scr_find) lv_obj_del(scr_find);
  scr_find = lv_obj_create(nullptr);
  lv_obj_set_style_bg_color(scr_find, lv_color_hex(0x05090c), 0);
  build_status_bar(scr_find);

  lv_obj_t *title = lv_label_create(scr_find);
  lv_label_set_text_fmt(title, "FINDING  %s", peers[peerIdx].name);
  lv_obj_set_style_text_color(title, lv_color_hex(0xffb84d), 0);
  lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 50);

  find_dist = lv_label_create(scr_find);
  lv_obj_set_style_text_font(find_dist, &lv_font_montserrat_28, 0);
  lv_obj_set_style_text_color(find_dist, lv_color_hex(0x7cf0d6), 0);
  lv_obj_align(find_dist, LV_ALIGN_CENTER, 0, -30);

  find_bar = lv_bar_create(scr_find);
  lv_obj_set_size(find_bar, LV_HOR_RES - 60, 28);
  lv_obj_align(find_bar, LV_ALIGN_CENTER, 0, 30);
  lv_bar_set_range(find_bar, 0, 100);
  lv_obj_set_style_bg_color(find_bar, lv_color_hex(0x0a0f14), 0);
  lv_obj_set_style_bg_color(find_bar, lv_color_hex(0x7cf0d6), LV_PART_INDICATOR);

  lv_obj_t *back = lv_btn_create(scr_find);
  lv_obj_align(back, LV_ALIGN_BOTTOM_MID, 0, -12);
  lv_obj_set_size(back, 160, 48);
  lv_obj_t *bl = lv_label_create(back);
  lv_label_set_text(bl, "< BACK");
  lv_obj_center(bl);
  lv_obj_add_event_cb(back, on_find_back, LV_EVENT_CLICKED, nullptr);

  lv_scr_load(scr_find);
}

static void find_tick() {
  if (!scr_find || lv_scr_act() != scr_find) return;
  Peer &p = peers[find_idx];
  if (!p.inUse) return;
  lv_label_set_text_fmt(find_dist, "~ %.1f m", p.distanceM);
  int v = (int)((p.rssi + 100));
  if (v < 0) v = 0; if (v > 100) v = 100;
  lv_bar_set_value(find_bar, v, LV_ANIM_ON);
}

// ---- Logs -----------------------------------------------------------------
static void ui_logs() {
  if (scr_logs) lv_obj_del(scr_logs);
  scr_logs = lv_obj_create(nullptr);
  lv_obj_set_style_bg_color(scr_logs, lv_color_hex(0x05090c), 0);
  lv_obj_clear_flag(scr_logs, LV_OBJ_FLAG_SCROLLABLE);
  attach_tab_events(scr_logs);
  build_status_bar(scr_logs);

  lv_obj_t *title = lv_label_create(scr_logs);
  lv_obj_set_style_text_font(title, &lv_font_montserrat_28, 0);
  lv_label_set_text(title, "LOGS");
  lv_obj_set_style_text_color(title, lv_color_hex(tab_accent(TAB_LOGS)), 0);
  lv_obj_align(title, LV_ALIGN_TOP_LEFT, 16, 44);

  lv_obj_t *sub = lv_label_create(scr_logs);
  lv_label_set_text_fmt(sub, "%u entries", (unsigned)logCount);
  lv_obj_set_style_text_color(sub, lv_color_hex(0x5c6b7a), 0);
  lv_obj_align(sub, LV_ALIGN_TOP_LEFT, 16, 78);

  lv_obj_t *list = lv_obj_create(scr_logs);
  lv_obj_set_size(list, LV_HOR_RES - 24, LV_VER_RES - 220);
  lv_obj_align(list, LV_ALIGN_TOP_MID, 0, 108);
  lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_row(list, 6, 0);
  lv_obj_set_style_bg_color(list, lv_color_hex(0x0a0f14), 0);
  lv_obj_set_style_border_width(list, 0, 0);
  lv_obj_set_style_shadow_width(list, 0, 0);
  lv_obj_set_style_radius(list, 14, 0);

  size_t n = logCount < 40 ? logCount : 40;
  if (n == 0) {
    lv_obj_t *l = lv_label_create(list);
    lv_label_set_text(l, "no logs yet\nnearby devices will appear here");
    lv_obj_set_style_text_color(l, lv_color_hex(0x5c6b7a), 0);
  }
  for (size_t k = 0; k < n; ++k) {
    size_t i = (logHead + LOG_CAP - 1 - k) % LOG_CAP;
    LogEntry &e = logs[i];
    Peer &p = peers[e.peerIdx];
    lv_obj_t *l = lv_label_create(list);
    uint32_t agoS = (millis() - e.tsMs) / 1000;
    lv_label_set_text_fmt(l, "-%02lus  %-6s  %5.1fm  %ddBm",
                          (unsigned long)agoS,
                          p.name[0] ? p.name : "??????",
                          e.distanceM, e.rssi);
    lv_obj_set_style_text_color(l, lv_color_hex(0xe6f4ff), 0);
  }

  build_tab_indicator(scr_logs);
  screen_load(scr_logs);
}

// ---- Settings -------------------------------------------------------------
static void settings_save() {
  NVS.begin("beaconos", false);
  NVS.putString("name", CFG.name);
  NVS.putUChar ("role", (uint8_t)CFG.role);
  NVS.putUChar ("tx",   (uint8_t)CFG.transport);
  NVS.putBool  ("bcast",CFG.broadcast);
  NVS.putBool  ("vis",  CFG.visible);
  NVS.putBool  ("fo",   CFG.friendOnly);
  NVS.putUShort("scan", CFG.scanEveryMs);
  NVS.putChar  ("ref",  CFG.rssiAt1m);
  NVS.putFloat ("pl",   CFG.pathLoss);
  NVS.putUShort("scto", CFG.screenTimeoutSec);   // NEW
  NVS.putUChar ("bri",  CFG.brightness);         // NEW
  NVS.end();
}
static void settings_load() {
  NVS.begin("beaconos", true);
  if (NVS.isKey("name")) NVS.getString("name", CFG.name, sizeof(CFG.name));
  CFG.role      = (Role)NVS.getUChar ("role", CFG.role);
  CFG.transport = (Transport)NVS.getUChar("tx", CFG.transport);
  CFG.broadcast = NVS.getBool  ("bcast",CFG.broadcast);
  CFG.visible   = NVS.getBool  ("vis",  CFG.visible);
  CFG.friendOnly= NVS.getBool  ("fo",   CFG.friendOnly);
  CFG.scanEveryMs=NVS.getUShort("scan", CFG.scanEveryMs);
  CFG.rssiAt1m  = NVS.getChar  ("ref",  CFG.rssiAt1m);
  CFG.pathLoss  = NVS.getFloat ("pl",   CFG.pathLoss);
  CFG.screenTimeoutSec = NVS.getUShort("scto", CFG.screenTimeoutSec);  // NEW
  CFG.brightness       = NVS.getUChar ("bri",  CFG.brightness);        // NEW
  NVS.end();
}

static void on_toggle_bcast(lv_event_t *e) {
  CFG.broadcast = lv_obj_has_state((lv_obj_t*)lv_event_get_target(e), LV_STATE_CHECKED);
  if (CFG.broadcast) ble_start_advertising(); else ble_stop_advertising();
  settings_save();
}
static void on_toggle_vis(lv_event_t *e) {
  CFG.visible = lv_obj_has_state((lv_obj_t*)lv_event_get_target(e), LV_STATE_CHECKED);
  settings_save();
}
static void on_toggle_friend(lv_event_t *e) {
  CFG.friendOnly = lv_obj_has_state((lv_obj_t*)lv_event_get_target(e), LV_STATE_CHECKED);
  settings_save();
}
static void on_role_change(lv_event_t *e) {
  uint16_t sel = lv_dropdown_get_selected((lv_obj_t*)lv_event_get_target(e));
  CFG.role = (Role)sel;
  settings_save();
}
static void on_tx_change(lv_event_t *e) {
  uint16_t sel = lv_dropdown_get_selected((lv_obj_t*)lv_event_get_target(e));
  Transport old = CFG.transport;
  CFG.transport = (Transport)sel;
  settings_save();
  if (old != CFG.transport) {
    if (CFG.transport == TX_BLE_ONLY)  { mesh_stop(); }
    if (CFG.transport == TX_MESH_ONLY) { ble_stop_advertising(); mesh_start(); }
    if (CFG.transport == TX_BOTH)      { mesh_start(); if (CFG.broadcast) ble_start_advertising(); }
  }
}
// NEW: screen-timeout dropdown callback
static void on_scto_change(lv_event_t *e) {
  static const uint16_t opts[] = {0, 10, 15, 30, 60, 120};  // 0 = Never
  uint16_t sel = lv_dropdown_get_selected((lv_obj_t*)lv_event_get_target(e));
  if (sel >= sizeof(opts)/sizeof(opts[0])) sel = 2;
  CFG.screenTimeoutSec = opts[sel];
  settings_save();
  g_last_activity_ms = millis();  // reset idle counter
}
// NEW: brightness slider callback
static void on_bri_change(lv_event_t *e) {
  int32_t v = lv_slider_get_value((lv_obj_t*)lv_event_get_target(e));
  if (v < 20)  v = 20;    // never fully black in awake mode
  if (v > 255) v = 255;
  CFG.brightness = (uint8_t)v;
  set_backlight(CFG.brightness);
  settings_save();
}

static void ui_settings() {
  if (scr_settings) lv_obj_del(scr_settings);
  scr_settings = lv_obj_create(nullptr);
  lv_obj_set_style_bg_color(scr_settings, lv_color_hex(0x05090c), 0);
  lv_obj_clear_flag(scr_settings, LV_OBJ_FLAG_SCROLLABLE);
  attach_tab_events(scr_settings);
  build_status_bar(scr_settings);

  lv_obj_t *title = lv_label_create(scr_settings);
  lv_obj_set_style_text_font(title, &lv_font_montserrat_28, 0);
  lv_label_set_text(title, "SETTINGS");
  lv_obj_set_style_text_color(title, lv_color_hex(tab_accent(TAB_SETTINGS)), 0);
  lv_obj_align(title, LV_ALIGN_TOP_LEFT, 16, 44);

  lv_obj_t *sub = lv_label_create(scr_settings);
  lv_label_set_text(sub, "swipe to change tab");
  lv_obj_set_style_text_color(sub, lv_color_hex(0x5c6b7a), 0);
  lv_obj_align(sub, LV_ALIGN_TOP_LEFT, 16, 78);

  lv_obj_t *box = lv_obj_create(scr_settings);
  lv_obj_set_size(box, LV_HOR_RES - 24, LV_VER_RES - 220);
  lv_obj_align(box, LV_ALIGN_TOP_MID, 0, 108);
  lv_obj_set_flex_flow(box, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_row(box, 8, 0);
  lv_obj_set_style_bg_color(box, lv_color_hex(0x0a0f14), 0);
  lv_obj_set_style_border_width(box, 0, 0);
  lv_obj_set_style_shadow_width(box, 0, 0);
  lv_obj_set_style_radius(box, 14, 0);

  auto row_lbl = [&](const char *t) {
    lv_obj_t *r = lv_obj_create(box);
    lv_obj_set_width(r, LV_PCT(100));
    lv_obj_set_height(r, 48);
    lv_obj_set_flex_flow(r, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(r, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_bg_color(r, lv_color_hex(0x111a22), 0);
    lv_obj_set_style_border_width(r, 0, 0);
    lv_obj_set_style_shadow_width(r, 0, 0);
    lv_obj_set_style_radius(r, 10, 0);
    lv_obj_t *l = lv_label_create(r);
    lv_label_set_text(l, t);
    lv_obj_set_style_text_color(l, lv_color_hex(0xe6f4ff), 0);
    lv_obj_set_flex_grow(l, 1);
    return r;
  };

  lv_obj_t *r1 = row_lbl("Role");
  lv_obj_t *dd = lv_dropdown_create(r1);
  lv_dropdown_set_options(dd, "Watch\nBeacon\nBoth");
  lv_dropdown_set_selected(dd, (uint8_t)CFG.role);
  lv_obj_add_event_cb(dd, on_role_change, LV_EVENT_VALUE_CHANGED, nullptr);

  lv_obj_t *r2 = row_lbl("Transport");
  lv_obj_t *tx = lv_dropdown_create(r2);
  lv_dropdown_set_options(tx, "BLE only\nMesh only\nBoth");
  lv_dropdown_set_selected(tx, (uint8_t)CFG.transport);
  lv_obj_add_event_cb(tx, on_tx_change, LV_EVENT_VALUE_CHANGED, nullptr);

  lv_obj_t *r3 = row_lbl("Broadcast");
  lv_obj_t *sw1 = lv_switch_create(r3);
  if (CFG.broadcast) lv_obj_add_state(sw1, LV_STATE_CHECKED);
  lv_obj_add_event_cb(sw1, on_toggle_bcast, LV_EVENT_VALUE_CHANGED, nullptr);

  lv_obj_t *r4 = row_lbl("Visible");
  lv_obj_t *sw2 = lv_switch_create(r4);
  if (CFG.visible) lv_obj_add_state(sw2, LV_STATE_CHECKED);
  lv_obj_add_event_cb(sw2, on_toggle_vis, LV_EVENT_VALUE_CHANGED, nullptr);

  lv_obj_t *r5 = row_lbl("Friend only");
  lv_obj_t *sw3 = lv_switch_create(r5);
  if (CFG.friendOnly) lv_obj_add_state(sw3, LV_STATE_CHECKED);
  lv_obj_add_event_cb(sw3, on_toggle_friend, LV_EVENT_VALUE_CHANGED, nullptr);

  // NEW: Screen timeout dropdown
  lv_obj_t *r6 = row_lbl("Screen sleep");
  lv_obj_t *sto = lv_dropdown_create(r6);
  lv_dropdown_set_options(sto, "Never\n10s\n15s\n30s\n60s\n2min");
  static const uint16_t sto_opts[] = {0, 10, 15, 30, 60, 120};
  uint16_t sto_sel = 2; // default 15s
  for (uint16_t i = 0; i < sizeof(sto_opts)/sizeof(sto_opts[0]); ++i) {
    if (sto_opts[i] == CFG.screenTimeoutSec) { sto_sel = i; break; }
  }
  lv_dropdown_set_selected(sto, sto_sel);
  lv_obj_add_event_cb(sto, on_scto_change, LV_EVENT_VALUE_CHANGED, nullptr);

  // NEW: Brightness slider
  lv_obj_t *r7 = row_lbl("Brightness");
  lv_obj_t *sl = lv_slider_create(r7);
  lv_obj_set_width(sl, 150);
  lv_slider_set_range(sl, 20, 255);
  lv_slider_set_value(sl, CFG.brightness, LV_ANIM_OFF);
  lv_obj_set_style_bg_color(sl, lv_color_hex(tab_accent(TAB_SETTINGS)), LV_PART_INDICATOR);
  lv_obj_set_style_bg_color(sl, lv_color_hex(tab_accent(TAB_SETTINGS)), LV_PART_KNOB);
  lv_obj_add_event_cb(sl, on_bri_change, LV_EVENT_VALUE_CHANGED, nullptr);

  build_tab_indicator(scr_settings);
  screen_load(scr_settings);
}

// ---- SOS -----------------------------------------------------------------
// SOS is now a full tab. The big red button triggers the actual broadcast.
// A live 30s countdown replaces the button while broadcasting.
static uint32_t sos_untilMs = 0;
static lv_obj_t *sos_btn      = nullptr;
static lv_obj_t *sos_btn_lbl  = nullptr;
static lv_obj_t *sos_status   = nullptr;

// The actual "start broadcasting SOS" action (was inside ui_sos_broadcast).
static void sos_trigger() {
  sos_untilMs = millis() + 30000;
  Flags f{}; f.broadcast = 1; f.sos = 1;
  ble_build_payload(read_battery_pct(), f, millis()/1000);
  ble_stop_advertising();
  ble_start_advertising();
#if HAVE_MESH
  if (CFG.transport != TX_BLE_ONLY) mesh.sendBroadcast(String("SOS|") + CFG.name);
#endif
}

static void on_sos_click(lv_event_t *e) {
  sos_trigger();
  if (sos_btn_lbl) lv_label_set_text(sos_btn_lbl, "BROADCASTING");
  if (sos_status)  lv_label_set_text(sos_status, "signal is live for 30s");
}

// Called by the periodic UI tick so the countdown stays fresh.
static void sos_tick() {
  if (!sos_status || !sos_btn_lbl) return;
  uint32_t now = millis();
  if (now < sos_untilMs) {
    uint32_t left = (sos_untilMs - now) / 1000;
    lv_label_set_text_fmt(sos_btn_lbl, "BROADCASTING\n%lus", (unsigned long)left);
    lv_label_set_text(sos_status, "hold tight - you are visible");
  } else {
    lv_label_set_text(sos_btn_lbl, "TAP TO BROADCAST");
    lv_label_set_text(sos_status, "shares your location for 30s");
  }
}

static lv_obj_t *scr_sos = nullptr;

static void ui_sos() {
  // Cache the screen. Rebuilding it every time was expensive AND crashed if a
  // previous nav_to_tab animation still referenced the old scr_sos.
  if (scr_sos) { screen_load(scr_sos); sos_tick(); return; }

  Serial.println("[sos] build start");
  scr_sos = lv_obj_create(nullptr);
  lv_obj_set_style_bg_color(scr_sos, lv_color_hex(0x1a0508), 0);
  lv_obj_clear_flag(scr_sos, LV_OBJ_FLAG_SCROLLABLE);
  attach_tab_events(scr_sos);
  build_status_bar(scr_sos);

  lv_obj_t *title = lv_label_create(scr_sos);
  lv_obj_set_style_text_font(title, &lv_font_montserrat_28, 0);
  lv_label_set_text(title, "SOS");
  lv_obj_set_style_text_color(title, lv_color_hex(tab_accent(TAB_SOS)), 0);
  lv_obj_align(title, LV_ALIGN_TOP_LEFT, 16, 44);

  lv_obj_t *sub = lv_label_create(scr_sos);
  lv_label_set_text(sub, "emergency broadcast");
  lv_obj_set_style_text_color(sub, lv_color_hex(0xff9aa8), 0);
  lv_obj_align(sub, LV_ALIGN_TOP_LEFT, 16, 78);

  // Cheap rectangular pill-shaped button. Rounding is small (28 px), no
  // shadow, no alpha - so the flush pipeline can hit its full frame rate
  // even on the single 50-line DMA buffer.
  sos_btn = lv_btn_create(scr_sos);
  lv_obj_set_size(sos_btn, LV_HOR_RES - 60, 140);
  lv_obj_align(sos_btn, LV_ALIGN_CENTER, 0, 0);
  lv_obj_set_style_radius(sos_btn, 28, 0);
  lv_obj_set_style_bg_color(sos_btn, lv_color_hex(0xff2a4a), 0);
  lv_obj_set_style_bg_color(sos_btn, lv_color_hex(0xd91838), LV_STATE_PRESSED);
  lv_obj_set_style_shadow_width(sos_btn, 0, 0);
  lv_obj_set_style_shadow_opa(sos_btn, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(sos_btn, 3, 0);
  lv_obj_set_style_border_color(sos_btn, lv_color_hex(0xffb4c0), 0);
  lv_obj_add_event_cb(sos_btn, on_sos_click, LV_EVENT_CLICKED, nullptr);

  sos_btn_lbl = lv_label_create(sos_btn);
  lv_obj_set_style_text_font(sos_btn_lbl, &lv_font_montserrat_20, 0);
  lv_obj_set_style_text_color(sos_btn_lbl, lv_color_hex(0xffffff), 0);
  lv_label_set_text(sos_btn_lbl,
                    (millis() < sos_untilMs) ? "BROADCASTING" : "TAP TO BROADCAST");
  lv_obj_set_style_text_align(sos_btn_lbl, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_center(sos_btn_lbl);

  sos_status = lv_label_create(scr_sos);
  lv_label_set_text(sos_status,
                    (millis() < sos_untilMs) ? "signal is live for 30s"
                                             : "shares your location for 30s");
  lv_obj_set_style_text_color(sos_status, lv_color_hex(0xff9aa8), 0);
  lv_obj_align(sos_status, LV_ALIGN_BOTTOM_MID, 0, -70);

  build_tab_indicator(scr_sos);
  Serial.println("[sos] build done, loading screen");
  screen_load(scr_sos);
  sos_tick();
  Serial.println("[sos] screen loaded");
}

// Legacy entry point: some old code paths still call ui_sos_broadcast().
// Keep the name working - now it just jumps to the SOS tab and fires it.
static void ui_sos_broadcast() {
  g_current_tab = TAB_SOS;
  ui_sos();
  sos_trigger();
  if (sos_btn_lbl) lv_label_set_text(sos_btn_lbl, "BROADCASTING");
  if (sos_status)  lv_label_set_text(sos_status, "signal is live for 30s");
}

// ---- Back to LauncherOS ---------------------------------------------------
static void ui_back_to_launcher() {
  if (!sd_ready) { esp_restart(); return; }
  const char *path = "/WATCH/bin/launcheros.bin";
  File f = SD_MMC.open(path, FILE_READ);
  if (!f || f.size() < 100000) { esp_restart(); return; }
  if (!Update.begin(f.size())) { f.close(); return; }
  uint8_t buf[4096];
  while (f.available()) {
    size_t n = f.read(buf, sizeof(buf));
    Update.write(buf, n);
  }
  f.close();
  if (Update.end(true)) esp_restart();
}

// ---- Boot splash ----------------------------------------------------------
static void ui_boot_splash() {
  scr_splash = lv_obj_create(nullptr);              // FIX: remember the pointer
  lv_obj_set_style_bg_color(scr_splash, lv_color_hex(0x02060a), 0);

  lv_obj_t *l1 = lv_label_create(scr_splash);
  lv_label_set_text(l1, "BEACON // OS");
  lv_obj_set_style_text_font(l1, &lv_font_montserrat_28, 0);
  lv_obj_set_style_text_color(l1, lv_color_hex(0x7cf0d6), 0);
  lv_obj_align(l1, LV_ALIGN_CENTER, 0, -30);

  lv_obj_t *l2 = lv_label_create(scr_splash);
  lv_label_set_text(l2, "dualcore link  v1.0");
  lv_obj_set_style_text_color(l2, lv_color_hex(0xffb84d), 0);
  lv_obj_align(l2, LV_ALIGN_CENTER, 0, 10);

  lv_scr_load(scr_splash);
  // Force LVGL to actually push the splash to the panel.
  for (int i = 0; i < 25; i++) { lv_timer_handler(); delay(20); }
  lv_refr_now(NULL);
}

// =============================================================================
//                          BOOT (GPIO0) safety exit
// =============================================================================
static uint32_t bootPressStartMs = 0;
static void boot_safety_tick() {
  int lvl = digitalRead(0);
  if (lvl == LOW) {
    if (bootPressStartMs == 0) bootPressStartMs = millis();
    if (millis() - bootPressStartMs > 1500) esp_restart();
  } else {
    bootPressStartMs = 0;
  }
}

// =============================================================================
//                          DriveBus / touch shim
// =============================================================================
int16_t dc_last_touch_x = 0, dc_last_touch_y = 0;
bool    dc_last_touch_valid = false;

bool dc_touch_read(int16_t *x, int16_t *y) {
  // Poll whenever the FT3168/CST816 IRQ has fired.
  if (!TP) return false;
  int32_t tx = TP->IIC_Read_Device_Value(TP->Arduino_IIC_Touch::Value_Information::TOUCH_COORDINATE_X);
  int32_t ty = TP->IIC_Read_Device_Value(TP->Arduino_IIC_Touch::Value_Information::TOUCH_COORDINATE_Y);
  if (TP->IIC_Interrupt_Flag) {
    TP->IIC_Interrupt_Flag = false;
    dc_last_touch_x = (int16_t)tx;
    dc_last_touch_y = (int16_t)ty;
    *x = dc_last_touch_x;
    *y = dc_last_touch_y;
    return true;
  }
  return false;
}

// =============================================================================
//                                  setup()
// =============================================================================
void setup() {
  Serial.begin(115200);
  // Give the host USB stack a moment to enumerate the CDC device before we
  // start printing - on the S3 the first ~500ms of output would otherwise be
  // lost. This also makes the "no serial output" symptom go away.
  delay(1200);
  Serial.println("\n\n[BeaconOS] ====== boot ======");
  Serial.flush();
  pinMode(0, INPUT_PULLUP);
  peersMx = xSemaphoreCreateMutex();
  Serial.printf("[ck] free heap at boot: %u bytes, PSRAM: %u bytes\n",
                (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getPsramSize());
  Serial.flush();

  // -------------------------------------------------------------------------
  // Waveshare ESP32-S3-Touch-AMOLED-1.8 power-up sequence
  // -------------------------------------------------------------------------
  // Order matters. This mirrors examples/Arduino-v3.3.5[-v2]/09_LVGL_change_background
  // from waveshareteam/ESP32-S3-Touch-AMOLED-1.8 verbatim.

  // 1) Bring up the touch/PMU I2C bus on the board's dedicated pins.
  Wire.begin(IIC_SDA, IIC_SCL);

  // 2) AXP2101 PMU — turns on the rails that feed the display + touch.
  if (pmu.begin(Wire, AXP2101_SLAVE_ADDRESS, IIC_SDA, IIC_SCL)) {
    pmu.setALDO1Voltage(3300); pmu.enableALDO1();   // Touch VDD
    pmu.setALDO3Voltage(3300); pmu.enableALDO3();   // Panel logic
    pmu.setBLDO1Voltage(3300); pmu.enableBLDO1();   // Panel VCI
    pmu.setChargingLedMode(XPOWERS_CHG_LED_CTRL_CHG);
  } else {
    Serial.println("AXP2101 init failed - continuing (some boards lack it)");
  }

  // 3) XCA9554 GPIO expander @ 0x20 - THIS is what actually enables the panel.
  //    Pins 0,1,2 = panel power / reset / touch reset. Datasheet demands the
  //    LOW->20ms->HIGH pulse below; without it the AMOLED stays dark.
  if (!expander.begin(XCA9554_ADDR)) {
    Serial.println("XCA9554 expander not found - panel will not turn on!");
  } else {
    expander.pinMode(0, OUTPUT);
    expander.pinMode(1, OUTPUT);
    expander.pinMode(2, OUTPUT);
    expander.digitalWrite(0, LOW);
    expander.digitalWrite(1, LOW);
    expander.digitalWrite(2, LOW);
    delay(20);
    expander.digitalWrite(0, HIGH);
    expander.digitalWrite(1, HIGH);
    expander.digitalWrite(2, HIGH);
    delay(20);
  }

  // 4) Build the QSPI bus + panel driver, using the correct chip per board rev.
  bus = new Arduino_ESP32QSPI(
      LCD_CS, LCD_SCLK, LCD_SDIO0, LCD_SDIO1, LCD_SDIO2, LCD_SDIO3);

#if BOARD_AMOLED_1_8_REV == 1
  // v1: SH8601 panel
  Arduino_SH8601 *panel = new Arduino_SH8601(bus, GFX_NOT_DEFINED /* rst */, 0 /* rot */,
                                             LCD_WIDTH, LCD_HEIGHT);
#else
  // v2: CO5300 panel (with the 0x16 column offset the demo uses)
  Arduino_CO5300 *panel = new Arduino_CO5300(bus, GFX_NOT_DEFINED /* rst */, 0 /* rot */,
                                             LCD_WIDTH, LCD_HEIGHT, 16, 0, 0, 0);
#endif
  gfx = panel;   // Arduino_GFX* handle used by LVGL flush
  g_panel = panel; // NEW: keep a typed handle for set_backlight()

  // 5) Touch controller (chip picked per board rev).
  IIC_Bus = std::make_shared<Arduino_HWIIC>(IIC_SDA, IIC_SCL, &Wire);
#if BOARD_AMOLED_1_8_REV == 1
  TP.reset(new Arduino_FT3x68(IIC_Bus, FT3168_DEVICE_ADDRESS,
                              DRIVEBUS_DEFAULT_VALUE, TP_INT,
                              Arduino_IIC_Touch_Interrupt));
#else
  TP.reset(new Arduino_CST816x(IIC_Bus, CST816T_DEVICE_ADDRESS,
                               DRIVEBUS_DEFAULT_VALUE, TP_INT,
                               Arduino_IIC_Touch_Interrupt));
#endif

  uint8_t tp_tries = 0;
  while (TP->begin() == false) {
    Serial.println("Touch init fail");
    if (++tp_tries >= 3) break;
    delay(500);
  }
  if (TP && tp_tries < 3) {
    // Use INTERRUPT_PERIODIC on BOTH revisions - this is what the Waveshare
    // reference sketch (Arduino-v3.3.5-v2/13_LVGL_Widgets) uses on CST816
    // and it is the only mode that reliably raises IIC_Interrupt_Flag on
    // this board. TOUCH_POWER_MONITOR was the old FT3168 setting and it
    // stops the controller from ever poking the IRQ line, which is why
    // touch appeared dead.
    TP->IIC_Write_Device_State(TP->Arduino_IIC_Touch::Device::TOUCH_DEVICE_INTERRUPT_MODE,
                               TP->Arduino_IIC_Touch::Device_Mode::TOUCH_DEVICE_INTERRUPT_PERIODIC);
    Serial.println("[ck] touch: INTERRUPT_PERIODIC mode");
  } else {
    Serial.println("[ck] !! touch controller did not init - panel taps will be ignored");
  }

  // 6) Start the panel and turn the backlight on. setBrightness() only exists
  //    on the concrete driver class, so we call it via the typed `panel` ptr.
  gfx->begin();
  panel->setBrightness(CFG.brightness);
  gfx->fillScreen(BLACK);
  Serial.println("[ck] display up");

  // Single-buffer mode matching the S3_MiniOS reference. Double buffering on
  // this board needs ~75 KB of DMA-capable RAM (2x SCR_W*40*sizeof(lv_color_t))
  // which is more than the S3 usually has free after BLE brings the WiFi/BT
  // stack up. When the secondary buffer couldn't be DMA-allocated the driver
  // silently kept redrawing to a non-DMA buffer and the panel got stuck
  // showing the last DMA-valid frame - i.e. the splash. Using ONE DMA buffer
  // sized like Widget OS (LCD_WIDTH * 50) is rock-solid on this hardware.
  const size_t lvBufPx = SCR_W * 50;
  buf1 = (lv_color_t*)heap_caps_malloc(lvBufPx * sizeof(lv_color_t), MALLOC_CAP_DMA);
  if (!buf1) {
    Serial.println("[ck] DMA buffer alloc failed, falling back to malloc");
    buf1 = (lv_color_t*)malloc(lvBufPx * sizeof(lv_color_t));
  }
  buf2 = nullptr; // intentionally single-buffered - see comment above
  if (!buf1) Serial.println("[ck] !! LVGL draw buffer alloc FAILED - display will be blank");
  lv_init();
  lv_disp_draw_buf_init(&draw_buf, buf1, nullptr, lvBufPx);

  static lv_disp_drv_t ddrv;
  lv_disp_drv_init(&ddrv);
  ddrv.hor_res = SCR_W;
  ddrv.ver_res = SCR_H;
  ddrv.flush_cb = my_disp_flush;
  ddrv.draw_buf = &draw_buf;
  lv_disp_drv_register(&ddrv);

  static lv_indev_drv_t idrv;
  lv_indev_drv_init(&idrv);
  idrv.type    = LV_INDEV_TYPE_POINTER;
  idrv.read_cb = my_touch_read;
  lv_indev_drv_register(&idrv);

  // NEW: start the LVGL tick timer (2 ms cadence, matches Waveshare demo).
  // Without this, LVGL time never advances, so gestures + touch clicks +
  // animations all silently fail.
  static const esp_timer_create_args_t lv_tick_args = {
    .callback = &lv_tick_task,
    .arg      = nullptr,
    .dispatch_method = ESP_TIMER_TASK,
    .name     = "lvgl_tick",
    .skip_unhandled_events = false,
  };
  esp_timer_handle_t lv_tick_timer = nullptr;
  if (esp_timer_create(&lv_tick_args, &lv_tick_timer) == ESP_OK) {
    esp_timer_start_periodic(lv_tick_timer, 2 * 1000);   // 2 ms
    Serial.println("[ck] LVGL tick timer started");
  } else {
    Serial.println("[ck] !! LVGL tick timer create FAILED - touch will not work");
  }

  // ---- SD ----
  sd_ready = SD_MMC.begin("/sdcard", true);
  if (!sd_ready) Serial.println("SD_MMC init failed");
  Serial.println("[ck] SD step done");

  // ---- Settings ----
  settings_load();
  // NEW: apply saved brightness right away and reset the idle timer.
  set_backlight(CFG.brightness);
  g_last_activity_ms = millis();
  Serial.println("[ck] settings loaded");
  Serial.printf("[cfg] name=%s  role=%s  tx=%u  bcast=%d  vis=%d  scanEvery=%ums  scto=%us\n",
                CFG.name,
                CFG.role == ROLE_WATCH  ? "WATCH"
              : CFG.role == ROLE_BEACON ? "BEACON"
              :                           "BOTH",
                (unsigned)CFG.transport, (int)CFG.broadcast, (int)CFG.visible,
                (unsigned)CFG.scanEveryMs, (unsigned)CFG.screenTimeoutSec);

  // ---- Splash ----
  ui_boot_splash();
  Serial.println("[ck] splash shown");

  // ---- Load the real UI BEFORE bringing up the radios ---------------------
  // Previously the splash stayed on screen because BLEDevice::init / mesh_start
  // could block for seconds (or hang forever on a bad power sequence) and
  // ui_home() was only reached afterwards. Building the UI first guarantees
  // the user always gets past the splash, matching how the S3_MiniOS
  // reference sketch renders its main screen straight after LVGL init.
#if BOARD_WATCH_206
  ui_watchface();
#else
  ui_home();
#endif
  // FIX (the actual "stuck on splash" fix):
  //   1) drop the splash screen object so it can't be re-shown
  //   2) invalidate the whole active screen so LVGL fully repaints it
  //   3) pump LVGL long enough for QSPI DMA to actually finish flushing
  //      every 50-line stripe of the 368x448 panel (worst case ~35 stripes,
  //      each takes a few ms) BEFORE we hand the CPU to BLEDevice::init(),
  //      which can block for hundreds of ms with no LVGL ticks in between.
  if (scr_splash) {
    lv_obj_del(scr_splash);
    scr_splash = nullptr;
  }
  lv_obj_invalidate(lv_scr_act());
  for (int i = 0; i < 60; i++) { lv_timer_handler(); delay(15); }
  lv_refr_now(NULL);
  Serial.println("[ck] main UI shown, starting BLE...");

  // ---- BLE ----
  BLEDevice::init(CFG.name);
  Serial.printf("[ck] BLE init done, free heap: %u\n", (unsigned)ESP.getFreeHeap());
  adv     = BLEDevice::getAdvertising();
  scanner = BLEDevice::getScan();
  scanner->setAdvertisedDeviceCallbacks(&scanCb, true);
  scanner->setActiveScan(true);
  scanner->setInterval(160);
  scanner->setWindow(120);
  Serial.println("[ck] BLE scanner configured");

  Flags f{}; f.broadcast = CFG.broadcast ? 1 : 0;
  ble_build_payload(read_battery_pct(), f, 0);
  if (CFG.broadcast && CFG.visible) ble_start_advertising();
  Serial.println("[ck] BLE advertising step done");

  // ---- Optional mesh ----
  if (CFG.transport != TX_BLE_ONLY) {
    Serial.println("[ck] starting mesh...");
    mesh_start();
    Serial.println("[ck] mesh started");
  }

  // ---- Wi-Fi off unless mesh is on ----
  if (CFG.transport == TX_BLE_ONLY) {
    WiFi.mode(WIFI_OFF);
    esp_wifi_stop();
  }

  Serial.println("[ck] ====== setup() complete, entering loop() ======");
}

// =============================================================================
//                                  loop()
// =============================================================================
void loop() {
  static uint32_t lastScan   = 0;
  static uint32_t lastRefr   = 0;
  static uint32_t lastPayld  = 0;
  static uint32_t lastStatus = 0;

  uint32_t now = millis();

  static uint32_t lastBeat = 0;
  if (now - lastBeat > 2000) {
    lastBeat = now;
    Serial.printf("[hb] loop alive t=%lus  heap=%u\n",
                  (unsigned long)(now/1000), (unsigned)ESP.getFreeHeap());
  }

  lv_timer_handler();
#if HAVE_MESH
  if (CFG.transport != TX_BLE_ONLY) mesh.update();
#endif

  // Scan cadence:
  //   * ROLE_WATCH / ROLE_BOTH -> every CFG.scanEveryMs (default 5 s).
  //   * ROLE_BEACON            -> every 3x that so battery stays low,
  //                               but they DO still scan so two beacons
  //                               running in ROLE_BEACON discover each other.
  //   Previously ROLE_BEACON never scanned, which is why two beacons could
  //   not detect one another.
  uint32_t scanPeriod = CFG.scanEveryMs;
  if (CFG.role == ROLE_BEACON) scanPeriod = (uint32_t)CFG.scanEveryMs * 3;
  if (now - lastScan > scanPeriod) {
    lastScan = now;
    ble_scan_once();
  }

  if (now - lastRefr > 1000) {
    lastRefr = now;
    if (lv_scr_act() == scr_home)      refresh_home_list();
    if (lv_scr_act() == scr_find)      find_tick();
    if (lv_scr_act() == scr_watchface) status_bar_tick();
    if (lv_scr_act() == scr_sos)       sos_tick();     // NEW
  }

  // NEW: idle -> sleep_screen(). Wake happens inside my_touch_read().
  screen_timeout_tick();

  if (now - lastPayld > 5000 && CFG.broadcast) {
    lastPayld = now;
    Flags f{}; f.broadcast = 1; f.sos = (now < sos_untilMs) ? 1 : 0;
    ble_build_payload(read_battery_pct(), f, now/1000);
    ble_stop_advertising();
    if (CFG.visible) ble_start_advertising();
  }

  if (now - lastStatus > 3000) {
    lastStatus = now;
    status_bar_tick();
  }

  boot_safety_tick();

  if (CFG.role == ROLE_BEACON) delay(200);
  else                         delay(5);
}
