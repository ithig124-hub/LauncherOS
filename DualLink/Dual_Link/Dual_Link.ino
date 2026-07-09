// =============================================================================
// BeaconOS - DualCore Link   (LVGL 8.4, ESP32-S3 AMOLED 2.06)  v2.5
// -----------------------------------------------------------------------------
// v2.5 changes (hardware-parity with LauncherOS - fixes boot crash + input):
//   * SD_MMC.setPins(2,1,3) before begin() -> SD mounts, wallpapers load.
//   * Touch: I2C bus recovery + reset pulse + 400 kHz + reliable swipe reader
//     (no IRQ-flag stomp) -> swipes actually work; fixes the FT3168 NACK.
//   * Single LVGL partial buffer (was dual PSRAM buffer) -> fixes the
//     LoadProhibited boot loop; flush uses &color_p->full, rounder removed.
//   * Infinite left/right swipe loop between watch face and beacon list;
//     swipe up on the face = SOS.  Watch-face hint labels removed.
//   * Wallpaper reads LauncherOS's dcw/wp_idx from /WATCH/PHOTOS (unchanged).
//   * PANEL_INVERT flag: set to 1 only if colours look photo-negative.
// v2.4 changes:
//   * Distance accuracy: 7-sample median + EMA smoothing; auto cm/m; fast-track.
// =============================================================================

#include <Arduino.h>
#include <Wire.h>
#include <SD_MMC.h>
#include <Update.h>
#include <esp_ota_ops.h>       // NEW: partition-switch back to launcher
#include "esp_partition.h"     // NEW: find the factory (launcher) partition
#include <Preferences.h>
#include <WiFi.h>
#include <esp_wifi.h>
#include <sys/time.h>
#include <esp_bt.h>
#include <esp_bt_main.h>
#include "esp_mac.h"

#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLEScan.h>
#include <BLEAdvertisedDevice.h>
#include <BLEAdvertising.h>

#include "pin_config.h"
#include <lvgl.h>
#include "Arduino_GFX_Library.h"
#include "Arduino_DriveBus_Library.h"
#include "XPowersLib.h"

#include "HWCDC.h"
HWCDC USBSerial;

#if __has_include(<painlessMesh.h>)
  #include <painlessMesh.h>
  #define HAVE_MESH 1
#else
  #define HAVE_MESH 0
#endif

#ifndef DC_MAC_TYPE
  #if defined(ESP_MAC_BT)
    #define DC_MAC_TYPE ESP_MAC_BT
  #else
    #define DC_MAC_TYPE ESP_MAC_WIFI_STA
  #endif
#endif

// =============================================================================
// TYPES  (kept above the first function so auto-prototypes can see them)
// =============================================================================
enum Role      : uint8_t { ROLE_WATCH = 0, ROLE_BEACON = 1, ROLE_BOTH = 2 };
enum Transport : uint8_t { TX_BLE_ONLY = 0, TX_MESH_ONLY = 1, TX_BOTH = 2 };

struct Flags { uint8_t broadcast:1, findme:1, sos:1, lowbat:1, _rsv:4; };

struct Settings {
  char      name[7]     = "DUAL01";
  Role      role        = ROLE_BOTH;
  Transport transport   = TX_BLE_ONLY;
  bool      broadcast   = true;
  bool      visible     = true;
  bool      friendOnly  = false;
  uint16_t  scanEveryMs = 5000;
  int8_t    rssiAt1m    = -59;
  float     pathLoss    = 2.5f;
  bool      distanceAuto = true;   // true = auto cm/m, false = always metres
  bool      fastTrack    = true;   // faster scan while the Find screen is open
};

#define RSSI_WIN 7   // median window size for RSSI spike rejection

struct Peer {
  uint8_t  id[6]      = {0};
  char     name[8]    = "";
  uint8_t  role       = ROLE_BOTH;
  uint8_t  battery    = 0;
  int8_t   rssi       = -127;   // filtered RSSI (drives display + signal bars)
  int8_t   rssiRaw    = -127;   // last raw reading (debug / logs)
  float    rssiFilt   = 0.0f;   // median->EMA filtered RSSI (0 = uninitialised)
  int8_t   rssiHist[RSSI_WIN] = {0};
  uint8_t  rssiHistCnt = 0;
  float    distanceM  = -1.0f;  // smoothed distance in metres
  float    distEma    = -1.0f;  // distance EMA accumulator
  uint32_t lastSeenMs = 0;
  bool     sos        = false;
  bool     inUse      = false;
};

struct LogEntry { uint32_t tsMs; uint8_t peerIdx; int8_t rssi; float distanceM; bool sos; };

// -----------------------------------------------------------------------------
// Globals built on those types
// -----------------------------------------------------------------------------
static constexpr uint16_t DC_MFG_ID   = 0x01DC;
static constexpr uint8_t  DC_PROTO_V1 = 0x01;
static Settings    CFG;
static Preferences NVS;
static constexpr size_t MAX_PEERS = 24;
static Peer peers[MAX_PEERS];
static SemaphoreHandle_t peersMx;
static constexpr size_t LOG_CAP = 512;
static LogEntry logs[LOG_CAP];
static size_t   logHead = 0, logCount = 0;

// -----------------------------------------------------------------------------
// Design tokens (theme)
// -----------------------------------------------------------------------------
#define COL_BG        0x02060a
#define COL_CARD      0x0d151c
#define COL_CARD_BRD  0x1b2732
#define COL_ACCENT    0x7cf0d6
#define COL_AMBER     0xffb84d
#define COL_TEXT      0xe6f4ff
#define COL_MUTED     0x62727f
#define COL_DANGER    0xff5470
#define COL_OK        0x4dff9e
#define COL_INACTIVE  0x223040

static lv_style_t st_screen, st_card, st_card_sos, st_title, st_value, st_muted;

static void theme_init() {
  lv_style_init(&st_screen);
  lv_style_set_bg_color(&st_screen, lv_color_hex(COL_BG));
  lv_style_set_bg_opa(&st_screen, LV_OPA_COVER);
  lv_style_set_border_width(&st_screen, 0);
  lv_style_set_pad_all(&st_screen, 0);
  lv_style_set_text_color(&st_screen, lv_color_hex(COL_TEXT));

  lv_style_init(&st_card);
  lv_style_set_bg_color(&st_card, lv_color_hex(COL_CARD));
  lv_style_set_bg_opa(&st_card, LV_OPA_COVER);
  lv_style_set_border_color(&st_card, lv_color_hex(COL_CARD_BRD));
  lv_style_set_border_width(&st_card, 1);
  lv_style_set_radius(&st_card, 14);
  lv_style_set_pad_all(&st_card, 10);

  lv_style_init(&st_card_sos);
  lv_style_set_bg_color(&st_card_sos, lv_color_hex(0x2a0d14));
  lv_style_set_bg_opa(&st_card_sos, LV_OPA_COVER);
  lv_style_set_border_color(&st_card_sos, lv_color_hex(COL_DANGER));
  lv_style_set_border_width(&st_card_sos, 2);
  lv_style_set_radius(&st_card_sos, 14);
  lv_style_set_pad_all(&st_card_sos, 10);

  lv_style_init(&st_title);
  lv_style_set_text_color(&st_title, lv_color_hex(COL_ACCENT));
  lv_style_set_text_font(&st_title, &lv_font_montserrat_20);

  lv_style_init(&st_value);
  lv_style_set_text_color(&st_value, lv_color_hex(COL_ACCENT));
  lv_style_set_text_font(&st_value, &lv_font_montserrat_20);

  lv_style_init(&st_muted);
  lv_style_set_text_color(&st_muted, lv_color_hex(COL_MUTED));
}

// -----------------------------------------------------------------------------
// Forward decls
// -----------------------------------------------------------------------------
static void ui_boot_splash();
static void ui_watchface();
static void ui_home();
static void ui_find(uint8_t peerIdx);
static void ui_logs();
static void ui_settings();
static void ui_sos_broadcast();
static void ui_back_to_launcher();
static void refresh_home_list();

// =============================================================================
// Display + touch
// =============================================================================
// SD-MMC + touch-reset pins (match LauncherOS; guarded so a pin_config.h
// definition still wins if it already provides them).
#ifndef SDMMC_CLK
  #define SDMMC_CLK  2
#endif
#ifndef SDMMC_CMD
  #define SDMMC_CMD  1
#endif
#ifndef SDMMC_DATA
  #define SDMMC_DATA 3
#endif
#ifndef TP_RESET
  #define TP_RESET   9
#endif
// Set to 1 ONLY if the panel shows colours as a photo-negative.
#ifndef PANEL_INVERT
  #define PANEL_INVERT 0
#endif

static Arduino_DataBus *bus = new Arduino_ESP32QSPI(
    LCD_CS, LCD_SCLK, LCD_SDIO0, LCD_SDIO1, LCD_SDIO2, LCD_SDIO3);
static Arduino_GFX *gfx = new Arduino_CO5300(
    bus, LCD_RESET, 0, LCD_WIDTH, LCD_HEIGHT, 22, 0, 0, 0);

static std::shared_ptr<Arduino_IIC_DriveBus> IIC_Bus =
    std::make_shared<Arduino_HWIIC>(IIC_SDA, IIC_SCL, &Wire);
static void Arduino_IIC_Touch_Interrupt(void);
static std::unique_ptr<Arduino_IIC> FT3168(new Arduino_FT3x68(
    IIC_Bus, FT3168_DEVICE_ADDRESS, DRIVEBUS_DEFAULT_VALUE, TP_INT,
    Arduino_IIC_Touch_Interrupt));
static void Arduino_IIC_Touch_Interrupt(void) { FT3168->IIC_Interrupt_Flag = true; }

// -----------------------------------------------------------------------------
// LVGL glue  (single partial buffer - LauncherOS/MiniOS parity)
// -----------------------------------------------------------------------------
#define LVGL_BUF_LINES 60
static uint32_t   screenWidth, screenHeight, bufSize;
static lv_disp_t *disp = nullptr;
static lv_color_t *buf1 = nullptr;

static void my_disp_flush(lv_disp_drv_t *drv, const lv_area_t *area, lv_color_t *color_p) {
  uint32_t w = lv_area_get_width(area);
  uint32_t h = lv_area_get_height(area);
  gfx->draw16bitRGBBitmap(area->x1, area->y1, (uint16_t *)&color_p->full, w, h);
  lv_disp_flush_ready(drv);
}

// -----------------------------------------------------------------------------
// Touch + manual swipe detector (LauncherOS-parity: NO IRQ-flag stomp).
// The old reader cleared IIC_Interrupt_Flag and only reported PRESSED on the
// IRQ edge, so coordinates never updated mid-drag and swipes stalled. This
// reads X / Y / FINGER_NUMBER every poll, applies release hysteresis, and
// classifies a swipe on finger-up into g_pending_swipe (consumed in loop()).
// -----------------------------------------------------------------------------
enum SwipeEv : uint8_t { SW_NONE, SW_LEFT, SW_RIGHT, SW_UP };
static volatile SwipeEv g_pending_swipe = SW_NONE;
static uint32_t g_lastSwipeMs = 0;
#define SWIPE_THRESHOLD_PX     25
#define SWIPE_MAX_DURATION_MS  800
static int32_t ts_x = 0, ts_y = 0; static uint32_t ts_ms = 0;
static int32_t tc_x = 0, tc_y = 0; static bool touchActive = false;

static void my_touchpad_read(lv_indev_drv_t *drv, lv_indev_data_t *data) {
  int32_t x = FT3168->IIC_Read_Device_Value(FT3168->Arduino_IIC_Touch::Value_Information::TOUCH_COORDINATE_X);
  int32_t y = FT3168->IIC_Read_Device_Value(FT3168->Arduino_IIC_Touch::Value_Information::TOUCH_COORDINATE_Y);
  int fingers = FT3168->IIC_Read_Device_Value(FT3168->Arduino_IIC_Touch::Value_Information::TOUCH_FINGER_NUMBER);
  bool pressed = (fingers > 0);

  static int zero_streak = 0; const int RELEASE_HYSTERESIS = 3;
  if (pressed) zero_streak = 0;
  else if (touchActive) { if (++zero_streak < RELEASE_HYSTERESIS) { pressed = true; x = tc_x; y = tc_y; } }

  if (pressed) {
    data->state = LV_INDEV_STATE_PR; data->point.x = x; data->point.y = y;
    if (!touchActive) { touchActive = true; ts_x = x; ts_y = y; ts_ms = millis(); }
    tc_x = x; tc_y = y;
  } else {
    data->state = LV_INDEV_STATE_REL;
    if (touchActive) {
      touchActive = false;
      uint32_t dur = millis() - ts_ms;
      int32_t dx = tc_x - ts_x, dy = tc_y - ts_y;
      if (dur < SWIPE_MAX_DURATION_MS) {
        if (abs(dx) > SWIPE_THRESHOLD_PX && abs(dx) > abs(dy)) g_pending_swipe = (dx < 0) ? SW_LEFT : SW_RIGHT;
        else if (-dy > SWIPE_THRESHOLD_PX && abs(dy) > abs(dx)) g_pending_swipe = SW_UP;
      }
    }
  }
}

// =============================================================================
// PMU
// =============================================================================
static XPowersPMU pmu;
static bool pmu_ok = false;
static void pmu_setup() {
  pmu_ok = pmu.begin(Wire, AXP2101_SLAVE_ADDRESS, IIC_SDA, IIC_SCL);
  if (!pmu_ok) { USBSerial.println("AXP2101 not found"); return; }
  pmu.disableIRQ(XPOWERS_AXP2101_ALL_IRQ);
  pmu.clearIrqStatus();
  pmu.enableIRQ(XPOWERS_AXP2101_PKEY_SHORT_IRQ);
  pmu.enableBattDetection();
  pmu.enableBattVoltageMeasure();
  pmu.enableSystemVoltageMeasure();
  pmu.enableVbusVoltageMeasure();
  pmu.enableTemperatureMeasure();
}
static uint8_t read_battery_pct() {
  if (!pmu_ok || !pmu.isBatteryConnect()) return 0;
  int pct = pmu.getBatteryPercent();
  if (pct < 0) pct = 0; if (pct > 100) pct = 100;
  return (uint8_t)pct;
}

// =============================================================================
// BLE advertising
// =============================================================================
static BLEAdvertising *adv = nullptr;
static uint8_t mfgBuf[22];

static void ble_build_payload(uint8_t battery, Flags f, uint32_t uptimeS) {
  mfgBuf[0] = DC_MFG_ID & 0xFF; mfgBuf[1] = DC_MFG_ID >> 8;
  mfgBuf[2] = DC_PROTO_V1; mfgBuf[3] = (uint8_t)CFG.role;
  memcpy(&mfgBuf[4], &f, 1); mfgBuf[5] = battery;
  uint8_t mac[6] = {0}; esp_read_mac(mac, (esp_mac_type_t)DC_MAC_TYPE);
  memcpy(&mfgBuf[6], mac, 6);
  memcpy(&mfgBuf[12], &uptimeS, 4);
  memset(&mfgBuf[16], ' ', 6);
  memcpy(&mfgBuf[16], CFG.name, strnlen(CFG.name, 6));
}
static void ble_start_advertising() {
  if (!adv) return;
  BLEAdvertisementData ad; ad.setFlags(0x06);
  String mfg; mfg.reserve(sizeof(mfgBuf));
  for (size_t i = 0; i < sizeof(mfgBuf); ++i) mfg += (char)mfgBuf[i];
  ad.setManufacturerData(mfg);
  adv->setAdvertisementData(ad);
  adv->setScanResponse(false);
  adv->setMinInterval(160); adv->setMaxInterval(320);
  adv->start();
}
static void ble_stop_advertising() { if (adv) adv->stop(); }

// =============================================================================
// BLE scanning (own task, core 0)
// =============================================================================
static BLEScan *scanner = nullptr;

// --- Distance / tracking tuning ---------------------------------------------
static constexpr float    RSSI_EMA_ALPHA = 0.35f;  // 0..1, higher = snappier
static constexpr float    DIST_EMA_ALPHA = 0.30f;  // extra smoothing on metres
static constexpr uint32_t FAST_TRACK_MS  = 300;    // scan gap while Find is open
static volatile bool      g_find_active  = false;  // true while on the radar

static float rssi_to_distance(float rssi) {
  if (rssi >= 0.0f) return -1.0f;                 // invalid / no reading
  float d = powf(10.0f, (float)(CFG.rssiAt1m - rssi) / (10.0f * CFG.pathLoss));
  if (d < 0.05f)  d = 0.05f;                       // 5 cm floor
  if (d > 200.0f) d = 200.0f;                      // sane BLE ceiling
  return d;
}
static int rssi_level(int8_t r){ if(r>=-60)return 4; if(r>=-70)return 3; if(r>=-80)return 2; if(r>=-90)return 1; return 0; }

// Median of the RSSI window - kills single-sample spikes before smoothing.
static int8_t median_i8(const int8_t *src, int n) {
  if (n <= 0) return -127;
  int8_t a[RSSI_WIN];
  for (int i = 0; i < n; ++i) a[i] = src[i];
  for (int i = 1; i < n; ++i) { int8_t k = a[i]; int j = i - 1;
    while (j >= 0 && a[j] > k) { a[j + 1] = a[j]; --j; } a[j + 1] = k; }
  return a[n / 2];
}

// Feed a raw RSSI sample into a peer: median filter -> EMA -> smoothed distance.
static void peer_update_rssi(Peer &pr, int8_t raw) {
  pr.rssiRaw = raw;
  if (pr.rssiHistCnt < RSSI_WIN) pr.rssiHist[pr.rssiHistCnt++] = raw;
  else { for (int i = 1; i < RSSI_WIN; ++i) pr.rssiHist[i - 1] = pr.rssiHist[i];
         pr.rssiHist[RSSI_WIN - 1] = raw; }
  int8_t med = median_i8(pr.rssiHist, pr.rssiHistCnt);

  if (pr.rssiFilt == 0.0f) {                        // first sample for this peer
    pr.rssiFilt = med;
    pr.distEma  = rssi_to_distance(pr.rssiFilt);
  } else {
    pr.rssiFilt += RSSI_EMA_ALPHA * (med - pr.rssiFilt);
    float d = rssi_to_distance(pr.rssiFilt);
    if (d < 0)               pr.distEma = d;
    else if (pr.distEma < 0) pr.distEma = d;
    else                     pr.distEma += DIST_EMA_ALPHA * (d - pr.distEma);
  }
  pr.rssi      = (int8_t)lroundf(pr.rssiFilt);
  pr.distanceM = pr.distEma;
}

// Format a distance (metres) as auto cm/m, or always metres, per settings.
static void format_distance(float m, char *out, size_t n) {
  if (m < 0)                             { snprintf(out, n, "--"); return; }
  if (CFG.distanceAuto && m < 1.0f)        snprintf(out, n, "%d cm", (int)lroundf(m * 100.0f));
  else if (m < 10.0f)                      snprintf(out, n, "%.2f m", m);
  else                                     snprintf(out, n, "%.1f m", m);
}

static int find_or_alloc_peer(const uint8_t id[6]) {
  int freeSlot = -1;
  for (size_t i = 0; i < MAX_PEERS; ++i) {
    if (peers[i].inUse && memcmp(peers[i].id, id, 6) == 0) return (int)i;
    if (!peers[i].inUse && freeSlot < 0) freeSlot = (int)i;
  }
  if (freeSlot >= 0) { peers[freeSlot].inUse = true; memcpy(peers[freeSlot].id, id, 6); return freeSlot; }
  uint32_t oldestT = UINT32_MAX; int oldestI = 0;
  for (size_t i = 0; i < MAX_PEERS; ++i)
    if (peers[i].lastSeenMs < oldestT) { oldestT = peers[i].lastSeenMs; oldestI = (int)i; }
  memset(&peers[oldestI], 0, sizeof(Peer));
  peers[oldestI].inUse = true; memcpy(peers[oldestI].id, id, 6);
  return oldestI;
}

class ScanCB : public BLEAdvertisedDeviceCallbacks {
public:
  void onResult(BLEAdvertisedDevice dev) override {
    if (!dev.haveManufacturerData()) return;
    String md = dev.getManufacturerData();
    if (md.length() < sizeof(mfgBuf)) return;
    uint8_t buf[sizeof(mfgBuf)];
    for (size_t i = 0; i < sizeof(mfgBuf); ++i) buf[i] = (uint8_t)md[i];
    const uint8_t *p = buf;
    if ((p[0] | (p[1] << 8)) != DC_MFG_ID || p[2] != DC_PROTO_V1) return;
    uint8_t role = p[3]; Flags f; memcpy(&f, &p[4], 1);
    uint8_t bat = p[5]; const uint8_t *id = &p[6];
    char name[8] = {0}; memcpy(name, &p[16], 6);
    for (int i = 5; i >= 0 && name[i] == ' '; --i) name[i] = 0;

    xSemaphoreTake(peersMx, portMAX_DELAY);
    int idx = find_or_alloc_peer(id);
    peers[idx].role = role; peers[idx].battery = bat;
    peer_update_rssi(peers[idx], (int8_t)dev.getRSSI());
    peers[idx].lastSeenMs = millis();
    peers[idx].sos = f.sos;
    strncpy(peers[idx].name, name, 7);
    logs[logHead] = { peers[idx].lastSeenMs, (uint8_t)idx, peers[idx].rssi, peers[idx].distanceM, (bool)f.sos };
    logHead = (logHead + 1) % LOG_CAP;
    if (logCount < LOG_CAP) logCount++;
    xSemaphoreGive(peersMx);
  }
};
static ScanCB scanCb;

static void ble_scan_task(void *arg) {
  for (;;) {
    // Fix: beacons used to skip scanning entirely, which meant two
    // devices in ROLE_BEACON never discovered each other. Scan on every
    // role - just triple the interval when in beacon-only to save power.
    if (scanner) {
      scanner->start(1, false);
      scanner->clearResults();
    }
    uint32_t period = CFG.scanEveryMs;
    if (CFG.role == ROLE_BEACON) period *= 3;
    // Live tracking: while the Find/radar screen is open, poll much faster so
    // the distance readout updates roughly once a second.
    if (CFG.fastTrack && g_find_active) period = FAST_TRACK_MS;
    vTaskDelay(pdMS_TO_TICKS(period));
  }
}

// =============================================================================
// Optional mesh
// =============================================================================
#if HAVE_MESH
static painlessMesh mesh;
static const char *MESH_PREFIX = "DualCoreLink";
static const char *MESH_PASS   = "dc-mesh-2026";
static const uint16_t MESH_PORT = 5555;
static void on_mesh_msg(uint32_t, String &) {}
static void mesh_start() { mesh.setDebugMsgTypes(ERROR | STARTUP); mesh.init(MESH_PREFIX, MESH_PASS, MESH_PORT); mesh.onReceive(&on_mesh_msg); }
static void mesh_stop() { mesh.stop(); }
#else
static void mesh_start() {}
static void mesh_stop()  {}
#endif

// =============================================================================
// Wallpaper (SHARED WITH LauncherOS — same path, same NVS key)
// -----------------------------------------------------------------------------
// LauncherOS writes the user's chosen wallpaper index into the `dcw`
// preferences namespace under key "wp_idx". Dual_Link reads that same key
// so whichever wallpaper the user picked on the launcher automatically
// carries over. Files live at /WATCH/PHOTOS/*.raw|*.bin as headerless
// RGB565 LE, LCD_WIDTH x LCD_HEIGHT (410 x 502 = 411640 bytes).
// =============================================================================
#define MAX_PHOTOS 24
static String       g_photos[MAX_PHOTOS];
static int          g_photo_count = 0;
static int          g_photo_idx   = 0;
static lv_img_dsc_t g_face_img_dsc = {};
static uint8_t     *g_face_pixels  = nullptr;
static size_t       g_face_alloc   = 0;

static void scan_photos() {
  g_photo_count = 0;
  File dir = SD_MMC.open("/WATCH/PHOTOS");
  if (!dir || !dir.isDirectory()) return;
  File f = dir.openNextFile();
  while (f && g_photo_count < MAX_PHOTOS) {
    if (!f.isDirectory()) {
      String n = f.name();
      int s = n.lastIndexOf('/'); if (s >= 0) n = n.substring(s + 1);
      String lo = n; lo.toLowerCase();
      if (lo.endsWith(".raw") || lo.endsWith(".bin"))
        g_photos[g_photo_count++] = String("/WATCH/PHOTOS/") + n;
    }
    f = dir.openNextFile();
  }
}

static bool load_photo(int idx) {
  if (!g_photo_count) return false;
  if (idx < 0) idx = g_photo_count - 1;
  if (idx >= g_photo_count) idx = 0;
  File f = SD_MMC.open(g_photos[idx].c_str(), FILE_READ);
  if (!f) return false;
  size_t need = (size_t)LCD_WIDTH * LCD_HEIGHT * 2;
  if (!g_face_pixels || g_face_alloc < need) {
    if (g_face_pixels) heap_caps_free(g_face_pixels);
    g_face_pixels = (uint8_t *)heap_caps_malloc(need, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!g_face_pixels) g_face_pixels = (uint8_t *)heap_caps_malloc(need, MALLOC_CAP_8BIT);
    if (!g_face_pixels) { f.close(); return false; }
    g_face_alloc = need;
  }
  memset(g_face_pixels, 0, need);
  size_t fsize = f.size();
  // Skip LVGL 4-byte header if the file is exactly need+4 bytes.
  if (fsize == need + 4) { f.seek(4); fsize -= 4; }
  f.read(g_face_pixels, fsize < need ? fsize : need);
  f.close();
  g_face_img_dsc.header.cf          = LV_IMG_CF_TRUE_COLOR;
  g_face_img_dsc.header.always_zero = 0;
  g_face_img_dsc.header.w           = LCD_WIDTH;
  g_face_img_dsc.header.h           = LCD_HEIGHT;
  g_face_img_dsc.data_size          = need;
  g_face_img_dsc.data               = g_face_pixels;
  g_photo_idx = idx;
  return true;
}

// Read the launcher-shared "wp_idx" and try to load it.
static void wallpaper_init_from_launcher() {
  Preferences dcw;
  dcw.begin("dcw", true);          // read-only, same namespace LauncherOS uses
  int idx = dcw.getInt("wp_idx", 0);
  dcw.end();
  scan_photos();
  if (g_photo_count) load_photo(idx);
}

static void wallpaper_save_idx() {
  Preferences dcw;
  dcw.begin("dcw", false);
  dcw.putInt("wp_idx", g_photo_idx);
  dcw.end();
}

// Live handle to the img widget on the watchface (null if not built yet).
static lv_obj_t *wf_bg_img  = nullptr;
static lv_obj_t *wf_bg_fill = nullptr;   // solid-colour fallback when no photo

static void wallpaper_apply_to_watchface() {
  if (!wf_bg_img) return;
  if (!g_photo_count || !g_face_pixels) {
    lv_obj_add_flag(wf_bg_img, LV_OBJ_FLAG_HIDDEN);
    if (wf_bg_fill) lv_obj_clear_flag(wf_bg_fill, LV_OBJ_FLAG_HIDDEN);
    return;
  }
  if (wf_bg_fill) lv_obj_add_flag(wf_bg_fill, LV_OBJ_FLAG_HIDDEN);
  lv_obj_clear_flag(wf_bg_img, LV_OBJ_FLAG_HIDDEN);
  lv_img_set_src(wf_bg_img, &g_face_img_dsc);
  lv_obj_invalidate(wf_bg_img);
}

// =============================================================================
// Partition switching  ("back to launcher" without needing SD)
// -----------------------------------------------------------------------------
// LauncherOS lives in the `factory` partition (0x10000, 3 MB). Dual_Link
// gets flashed into `ota_0` by the launcher's "load app" flow. To return
// to the launcher we just flip otadata back to factory and reboot -
// instant, no SD flash, no OTA image needed.
// If the factory partition somehow disappeared (rare - only via a full
// erase) we fall back to streaming /WATCH/bin/launcheros.bin through
// Update.h into ota_0.
// =============================================================================
static bool partition_switch_to_launcher() {
  const esp_partition_t *fact = esp_partition_find_first(
      ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_FACTORY, nullptr);
  if (!fact) return false;
  if (esp_ota_set_boot_partition(fact) != ESP_OK) return false;
  USBSerial.println("[part] boot -> factory, restarting");
  delay(80);
  esp_restart();
  return true;   // unreachable
}

static bool sd_flash_launcher_bin() {
  File f = SD_MMC.open("/WATCH/bin/launcheros.bin", FILE_READ);
  if (!f || f.size() < 100000) { if (f) f.close(); return false; }
  if (!Update.begin(f.size())) { f.close(); return false; }
  uint8_t buf[4096];
  while (f.available()) {
    size_t n = f.read(buf, sizeof(buf));
    if (Update.write(buf, n) != n) { f.close(); return false; }
  }
  f.close();
  if (!Update.end(true)) return false;
  delay(80);
  esp_restart();
  return true;
}

// =============================================================================
// WiFi / NTP time sync  (LauncherOS parity)
// Time source order:  NVS snapshot (instant) -> SD /WATCH/wifi/config.txt
// networks -> hardcoded network -> NTP (pool.ntp.org). WiFi is shut down
// after each sync and re-woken every 12 h.  Runs BEFORE BLE init so the two
// radios never fight for the antenna.
// =============================================================================
static const char *HC_WIFI_SSID = "Optus_9D2E3D";
static const char *HC_WIFI_PASS = "snucktemptGLeQU";
#define WIFI_RESYNC_INTERVAL_MS  (12UL * 60UL * 60UL * 1000UL)   // 12 hours

struct WifiNet { String ssid, pass; };
static WifiNet     g_nets[5];
static int         g_net_count = 0;
static int         g_gmt_offset_sec = 0;
static bool        g_wifi_connected = false;
static bool        g_ntp_synced     = false;
static uint32_t    g_last_sync_ms   = 0;
static Preferences g_timeprefs;

static void time_save_now(const char *why) {
  time_t now; time(&now);
  if (now < 1700000000) return;                 // RTC not set yet
  g_timeprefs.begin("rtcsave", false);
  g_timeprefs.putULong("t", (uint32_t)now);
  g_timeprefs.putLong("gmt", g_gmt_offset_sec);
  g_timeprefs.end();
  USBSerial.printf("[rtc] persisted t=%lu (%s)\n", (unsigned long)now, why);
}

// Instant plausible time on the face even after a dead battery.
static void time_restore_from_nvs() {
  time_t rtc_now = 0; time(&rtc_now);
  if (rtc_now >= 1700000000) {                  // RTC already live -> trust it
    g_timeprefs.begin("rtcsave", true);
    g_gmt_offset_sec = g_timeprefs.getLong("gmt", 0);
    g_timeprefs.end();
    g_ntp_synced = true;
    return;
  }
  g_timeprefs.begin("rtcsave", true);
  uint32_t t   = g_timeprefs.getULong("t", 0);
  int32_t  gmt = g_timeprefs.getLong("gmt", 0);
  g_timeprefs.end();
  if (t < 1700000000) { USBSerial.println("[rtc] no persisted time -- waiting for NTP"); return; }
  g_gmt_offset_sec = gmt;
  struct timeval tv = { (time_t)t, 0 };
  settimeofday(&tv, NULL);
  g_ntp_synced = true;
  USBSerial.printf("[rtc] restored t=%lu from NVS\n", (unsigned long)t);
}

static void wifi_shutdown() {
  WiFi.disconnect(true, true);
  WiFi.mode(WIFI_OFF);
  g_wifi_connected = false;
}

// Read up to 5 networks from /WATCH/wifi/config.txt (same file LauncherOS uses).
static void parse_wifi_config() {
  File f = SD_MMC.open("/WATCH/wifi/config.txt", FILE_READ);
  if (!f) return;
  while (f.available()) {
    String line = f.readStringUntil('\n'); line.trim();
    if (!line.length() || line.startsWith("#")) continue;
    int eq = line.indexOf('='); if (eq < 0) continue;
    String key = line.substring(0, eq); key.trim();
    String val = line.substring(eq + 1); val.trim();
    for (int i = 1; i <= 5; i++) {
      if (key == String("WIFI") + i + "_SSID" && val.length() &&
          val != "YourHomeNetwork" && val != "YourWorkNetwork") {
        if (g_net_count < 5) g_nets[g_net_count].ssid = val;
      }
      if (key == String("WIFI") + i + "_PASS" && val.length()) {
        int idx = g_net_count;
        if (idx < 5 && g_nets[idx].ssid.length() && !g_nets[idx].pass.length()) {
          g_nets[idx].pass = val; g_net_count++;
        }
      }
    }
    if (key == "GMT_OFFSET") g_gmt_offset_sec = val.toInt() * 3600;
  }
  f.close();
}

static void wifi_connect_and_sync() {
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(WIFI_PS_MIN_MODEM);
  WiFi.persistent(false);
  for (int i = 0; i < g_net_count && !g_wifi_connected; i++) {
    WiFi.begin(g_nets[i].ssid.c_str(), g_nets[i].pass.c_str());
    uint32_t u = millis() + 6000;
    while (millis() < u && WiFi.status() != WL_CONNECTED) delay(100);
    g_wifi_connected = (WiFi.status() == WL_CONNECTED);
  }
  if (!g_wifi_connected) {
    WiFi.begin(HC_WIFI_SSID, HC_WIFI_PASS);
    uint32_t u = millis() + 6000;
    while (millis() < u && WiFi.status() != WL_CONNECTED) delay(100);
    g_wifi_connected = (WiFi.status() == WL_CONNECTED);
  }
  if (g_wifi_connected) {
    configTime(g_gmt_offset_sec, 0, "pool.ntp.org");
    struct tm tm;
    for (int i = 0; i < 30 && !g_ntp_synced; i++)
      if (getLocalTime(&tm, 200)) g_ntp_synced = true;
    if (g_ntp_synced) time_save_now("ntp ok");
    USBSerial.printf("[wifi] connected %s, ntp=%d\n", WiFi.SSID().c_str(), (int)g_ntp_synced);
  } else {
    USBSerial.println("[wifi] no connection");
  }
  g_last_sync_ms = millis();
  wifi_shutdown();
}

// Called from loop(): tops up the clock every 12 h, then drops WiFi again.
static void wifi_resync_if_due() {
  if (millis() - g_last_sync_ms < WIFI_RESYNC_INTERVAL_MS) return;
  USBSerial.println("[ntp] periodic resync...");
  wifi_connect_and_sync();
}

// =============================================================================
// UI: shared
// =============================================================================
static lv_obj_t *scr_watchface = nullptr, *scr_home = nullptr, *scr_find = nullptr,
                *scr_logs = nullptr, *scr_settings = nullptr;

static lv_obj_t *wf_time=nullptr, *wf_time_shadow=nullptr, *wf_date=nullptr, *wf_battery=nullptr, *wf_peers=nullptr, *wf_sync_dot=nullptr;
static lv_obj_t *bar_battery=nullptr, *bar_clock=nullptr, *bar_role=nullptr;

// LauncherOS-style: screens swap INSTANTLY (no animation). Kept as a
// helper so if you ever want to re-enable slides you only change one line.
static void load_screen(lv_obj_t *s, lv_scr_load_anim_t /*anim*/) {
  lv_scr_load(s);
}
static void adv_dot_anim(void *o, int32_t v) { lv_obj_set_style_bg_opa((lv_obj_t *)o, (lv_opa_t)v, 0); }

static void status_bar_tick();

static void build_status_bar(lv_obj_t *parent) {
  lv_obj_t *bar = lv_obj_create(parent);
  lv_obj_remove_style_all(bar);
  lv_obj_set_size(bar, LV_HOR_RES, 34);
  lv_obj_align(bar, LV_ALIGN_TOP_MID, 0, 0);
  lv_obj_set_style_bg_color(bar, lv_color_hex(0x0a0f14), 0);
  lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
  lv_obj_set_style_pad_all(bar, 6, 0);
  lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);

  bar_battery = lv_label_create(bar);
  lv_obj_align(bar_battery, LV_ALIGN_LEFT_MID, 4, 0);
  lv_obj_set_style_text_color(bar_battery, lv_color_hex(COL_ACCENT), 0);
  bar_clock = lv_label_create(bar);
  lv_obj_align(bar_clock, LV_ALIGN_CENTER, 0, 0);
  lv_obj_set_style_text_color(bar_clock, lv_color_hex(COL_TEXT), 0);
  bar_role = lv_label_create(bar);
  lv_obj_align(bar_role, LV_ALIGN_RIGHT_MID, -6, 0);
  lv_obj_set_style_text_color(bar_role, lv_color_hex(COL_AMBER), 0);
}

static void status_bar_tick() {
  uint8_t b = read_battery_pct();
  time_t now = time(nullptr); struct tm tm; localtime_r(&now, &tm);
  if (bar_battery) {
    lv_label_set_text_fmt(bar_battery, LV_SYMBOL_BATTERY_FULL "  %u%%", b);
    lv_label_set_text_fmt(bar_clock, "%02d:%02d", tm.tm_hour, tm.tm_min);
    lv_label_set_text(bar_role, CFG.role==ROLE_WATCH?"WATCH":CFG.role==ROLE_BEACON?"BEACON":"BOTH");
  }
  if (wf_time) {
    char hm[8]; snprintf(hm, sizeof(hm), "%02d:%02d", tm.tm_hour, tm.tm_min);
    lv_label_set_text(wf_time, hm);
    if (wf_time_shadow) lv_label_set_text(wf_time_shadow, hm);
    char dbuf[24]; strftime(dbuf, sizeof(dbuf), "%a  %d %b", &tm);
    lv_label_set_text(wf_date, dbuf);
    lv_label_set_text_fmt(wf_battery, LV_SYMBOL_BATTERY_FULL " %u%%", b);
    if (wf_sync_dot) {
      uint32_t c = g_ntp_synced ? 0x00FF80 : (g_wifi_connected ? 0xFFB300 : 0x666666);
      lv_obj_set_style_bg_color(wf_sync_dot, lv_color_hex(c), 0);
    }
    int live=0, sos=0;
    xSemaphoreTake(peersMx, portMAX_DELAY);
    for (size_t i=0;i<MAX_PEERS;++i){ if(!peers[i].inUse)continue; if(millis()-peers[i].lastSeenMs>60000)continue; live++; if(peers[i].sos)sos++; }
    xSemaphoreGive(peersMx);
    if (sos>0) lv_label_set_text_fmt(wf_peers, "%d NEARBY   %d SOS", live, sos);
    else       lv_label_set_text_fmt(wf_peers, "%d NEARBY", live);
  }
}

// =============================================================================
// UI: Watchface
// =============================================================================
// Navigation is handled by the manual swipe detector in loop() now (LVGL
// gesture events were unreliable with the old IRQ-edge touch reader).

// Long-press the watchface -> cycle wallpaper forward + persist.
static uint32_t g_wf_press_ms   = 0;
static bool     g_wf_long_fired = false;
static void wf_on_press(lv_event_t *e) {
  lv_event_code_t c = lv_event_get_code(e);
  if (c == LV_EVENT_PRESSED)      { g_wf_press_ms = millis(); g_wf_long_fired = false; }
  else if (c == LV_EVENT_PRESSING) {
    if (!g_wf_long_fired && g_wf_press_ms && millis() - g_wf_press_ms > 700) {
      g_wf_long_fired = true;
      if (g_photo_count) {
        load_photo(g_photo_idx + 1);
        wallpaper_apply_to_watchface();
        wallpaper_save_idx();
      }
    }
  } else if (c == LV_EVENT_RELEASED || c == LV_EVENT_PRESS_LOST) {
    g_wf_press_ms = 0;
  }
}

static void ui_watchface() {
  if (scr_watchface) { load_screen(scr_watchface, LV_SCR_LOAD_ANIM_MOVE_RIGHT); status_bar_tick(); return; }
  scr_watchface = lv_obj_create(nullptr);
  lv_obj_add_style(scr_watchface, &st_screen, 0);
  lv_obj_clear_flag(scr_watchface, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_event_cb(scr_watchface, wf_on_press,  LV_EVENT_PRESSED,     nullptr);
  lv_obj_add_event_cb(scr_watchface, wf_on_press,  LV_EVENT_PRESSING,    nullptr);
  lv_obj_add_event_cb(scr_watchface, wf_on_press,  LV_EVENT_RELEASED,    nullptr);
  lv_obj_add_event_cb(scr_watchface, wf_on_press,  LV_EVENT_PRESS_LOST,  nullptr);

  // Wallpaper background (or solid-colour fallback). MUST be first child so
  // everything else stacks on top.
  wf_bg_fill = lv_obj_create(scr_watchface);
  lv_obj_remove_style_all(wf_bg_fill);
  lv_obj_set_size(wf_bg_fill, LV_HOR_RES, LV_VER_RES);
  lv_obj_set_style_bg_color(wf_bg_fill, lv_color_hex(0x05090c), 0);
  lv_obj_set_style_bg_opa(wf_bg_fill, LV_OPA_COVER, 0);
  lv_obj_clear_flag(wf_bg_fill, LV_OBJ_FLAG_CLICKABLE);

  wf_bg_img = lv_img_create(scr_watchface);
  lv_obj_set_size(wf_bg_img, LV_HOR_RES, LV_VER_RES);
  lv_obj_align(wf_bg_img, LV_ALIGN_TOP_LEFT, 0, 0);
  lv_obj_add_flag(wf_bg_img, LV_OBJ_FLAG_HIDDEN);   // shown once wallpaper loads
  lv_obj_clear_flag(wf_bg_img, LV_OBJ_FLAG_CLICKABLE);
  wallpaper_apply_to_watchface();

  // ---- Battery (top-left) : LauncherOS-style % readout --------------------
  wf_battery = lv_label_create(scr_watchface);
  lv_obj_set_style_text_font(wf_battery, &lv_font_montserrat_18, 0);
  lv_obj_set_style_text_color(wf_battery, lv_color_hex(0xFFFFFF), 0);
  lv_obj_set_style_text_opa(wf_battery, 210, 0);
  lv_label_set_text(wf_battery, "");
  lv_obj_align(wf_battery, LV_ALIGN_TOP_LEFT, 22, 18);

  // ---- Sync status dot (top-right): green=NTP, amber=WiFi, grey=none ------
  wf_sync_dot = lv_obj_create(scr_watchface);
  lv_obj_remove_style_all(wf_sync_dot);
  lv_obj_set_size(wf_sync_dot, 10, 10);
  lv_obj_set_style_radius(wf_sync_dot, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_bg_color(wf_sync_dot, lv_color_hex(0x666666), 0);
  lv_obj_set_style_bg_opa(wf_sync_dot, 220, 0);
  lv_obj_align(wf_sync_dot, LV_ALIGN_TOP_RIGHT, -22, 22);

  // ---- Time (big translucent white with a soft drop-shadow) --------------
  wf_time_shadow = lv_label_create(scr_watchface);
  lv_obj_set_style_text_font(wf_time_shadow, &lv_font_montserrat_48, 0);
  lv_obj_set_style_text_color(wf_time_shadow, lv_color_hex(0x000000), 0);
  lv_obj_set_style_text_opa(wf_time_shadow, 90, 0);
  lv_obj_set_style_text_letter_space(wf_time_shadow, 2, 0);
  lv_label_set_text(wf_time_shadow, "--:--");
  lv_obj_align(wf_time_shadow, LV_ALIGN_TOP_MID, 3, 93);

  wf_time = lv_label_create(scr_watchface);
  lv_obj_set_style_text_font(wf_time, &lv_font_montserrat_48, 0);
  lv_obj_set_style_text_color(wf_time, lv_color_hex(0xFFFFFF), 0);
  lv_obj_set_style_text_opa(wf_time, 235, 0);
  lv_obj_set_style_text_letter_space(wf_time, 2, 0);
  lv_label_set_text(wf_time, "--:--");
  lv_obj_align(wf_time, LV_ALIGN_TOP_MID, 0, 90);

  // ---- Date --------------------------------------------------------------
  wf_date = lv_label_create(scr_watchface);
  lv_obj_set_style_text_font(wf_date, &lv_font_montserrat_20, 0);
  lv_obj_set_style_text_color(wf_date, lv_color_hex(0xFFFFFF), 0);
  lv_obj_set_style_text_opa(wf_date, 175, 0);
  lv_label_set_text(wf_date, "");
  lv_obj_align(wf_date, LV_ALIGN_TOP_MID, 0, 168);

  // ---- Beacon count chip (Dual_Link) -------------------------------------
  lv_obj_t *chip = lv_obj_create(scr_watchface);
  lv_obj_add_style(chip, &st_card, 0);
  lv_obj_set_size(chip, 180, 46);
  lv_obj_align(chip, LV_ALIGN_BOTTOM_MID, 0, -28);
  lv_obj_clear_flag(chip, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_clear_flag(chip, LV_OBJ_FLAG_CLICKABLE);
  wf_peers = lv_label_create(chip);
  lv_obj_center(wf_peers);
  lv_obj_set_style_text_font(wf_peers, &lv_font_montserrat_18, 0);
  lv_obj_set_style_text_color(wf_peers, lv_color_hex(COL_ACCENT), 0);
  lv_label_set_text(wf_peers, "0 NEARBY");

  lv_scr_load(scr_watchface);
  status_bar_tick();
}

// =============================================================================
// UI: Home (pooled cards)
// =============================================================================
#define HOME_MAX_CARDS 16
struct HomeCard {
  lv_obj_t *card, *name, *sub, *dist, *bars[4];
  bool built;
};
static lv_obj_t *home_list  = nullptr;
static lv_obj_t *home_empty = nullptr;
static HomeCard  homeCards[HOME_MAX_CARDS];

static void on_peer_row(lv_event_t *e) {
  if (millis() - g_lastSwipeMs < 300) return;   // ignore taps right after a swipe
  lv_obj_t *card = (lv_obj_t *)lv_event_get_target(e);
  ui_find((uint8_t)(uintptr_t)lv_obj_get_user_data(card));
}

static void build_home_card(int k) {
  HomeCard &hc = homeCards[k];
  lv_obj_t *card = lv_btn_create(home_list);
  hc.card = card;                       // FIX: was never stored -> NULL deref crash
  lv_obj_remove_style_all(card);
  lv_obj_add_style(card, &st_card, 0);
  lv_obj_set_width(card, LV_PCT(100));
  lv_obj_set_height(card, 66);
  lv_obj_add_flag(card, LV_OBJ_FLAG_HIDDEN);
  lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_event_cb(card, on_peer_row, LV_EVENT_CLICKED, nullptr);

  hc.name = lv_label_create(card);
  lv_obj_set_style_text_font(hc.name, &lv_font_montserrat_18, 0);
  lv_obj_align(hc.name, LV_ALIGN_TOP_LEFT, 0, 2);

  hc.sub = lv_label_create(card);
  lv_obj_add_style(hc.sub, &st_muted, 0);
  lv_obj_align(hc.sub, LV_ALIGN_BOTTOM_LEFT, 0, -2);

  hc.dist = lv_label_create(card);
  lv_obj_add_style(hc.dist, &st_value, 0);
  lv_obj_align(hc.dist, LV_ALIGN_TOP_RIGHT, 0, 2);

  static const int hs[4] = {6, 10, 14, 18};
  for (int i = 0; i < 4; i++) {
    lv_obj_t *b = lv_obj_create(card);
    lv_obj_remove_style_all(b);
    lv_obj_clear_flag(b, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(b, 6, hs[i]);
    lv_obj_set_style_radius(b, 1, 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_align(b, LV_ALIGN_BOTTOM_RIGHT, -(6 + (3 - i) * 10), -4);
    hc.bars[i] = b;
  }
  hc.built = true;
}

static void update_home_card(int k, uint8_t slot, const Peer &p) {
  HomeCard &hc = homeCards[k];
  lv_obj_set_user_data(hc.card, (void *)(uintptr_t)slot);
  lv_obj_clear_flag(hc.card, LV_OBJ_FLAG_HIDDEN);

  lv_obj_remove_style(hc.card, &st_card, 0);
  lv_obj_remove_style(hc.card, &st_card_sos, 0);
  lv_obj_add_style(hc.card, p.sos ? &st_card_sos : &st_card, 0);

  lv_label_set_text_fmt(hc.name, "%s%s", p.sos ? LV_SYMBOL_WARNING " " : "",
                        p.name[0] ? p.name : "??????");
  lv_obj_set_style_text_color(hc.name, lv_color_hex(p.sos ? COL_DANGER : COL_TEXT), 0);
  lv_label_set_text_fmt(hc.sub, "%s  " LV_SYMBOL_BATTERY_FULL " %u%%",
                        p.role==ROLE_WATCH?"WATCH":p.role==ROLE_BEACON?"BEACON":"BOTH", p.battery);
  char distBuf[16]; format_distance(p.distanceM, distBuf, sizeof(distBuf));
  lv_label_set_text(hc.dist, distBuf);

  int lvl = rssi_level(p.rssi);
  for (int i = 0; i < 4; i++)
    lv_obj_set_style_bg_color(hc.bars[i], lv_color_hex(i < lvl ? COL_ACCENT : COL_INACTIVE), 0);
}

static void refresh_home_list() {
  if (!home_list) return;
  Peer snap[MAX_PEERS]; bool vis[MAX_PEERS];
  uint32_t now = millis();
  xSemaphoreTake(peersMx, portMAX_DELAY);
  for (size_t i = 0; i < MAX_PEERS; ++i) {
    snap[i] = peers[i];
    vis[i]  = peers[i].inUse && (now - peers[i].lastSeenMs <= 60000);
  }
  xSemaphoreGive(peersMx);

  int used = 0;
  for (size_t i = 0; i < MAX_PEERS && used < HOME_MAX_CARDS; ++i) {
    if (!vis[i]) continue;
    update_home_card(used, (uint8_t)i, snap[i]);
    used++;
  }
  for (int k = used; k < HOME_MAX_CARDS; ++k)
    if (homeCards[k].built) lv_obj_add_flag(homeCards[k].card, LV_OBJ_FLAG_HIDDEN);

  if (home_empty) {
    if (used) lv_obj_add_flag(home_empty, LV_OBJ_FLAG_HIDDEN);
    else      lv_obj_clear_flag(home_empty, LV_OBJ_FLAG_HIDDEN);
  }
}

static void on_nav_logs(lv_event_t *)     { ui_logs(); }
static void on_nav_settings(lv_event_t *) { ui_settings(); }
static void on_nav_sos(lv_event_t *)      { ui_sos_broadcast(); }
static void on_back_to_launcher(lv_event_t *) { if (millis() - g_lastSwipeMs < 350) return; ui_back_to_launcher(); }

static void ui_home() {
  if (scr_home) { load_screen(scr_home, LV_SCR_LOAD_ANIM_FADE_ON); refresh_home_list(); return; }
  scr_home = lv_obj_create(nullptr);
  lv_obj_add_style(scr_home, &st_screen, 0);
  lv_obj_clear_flag(scr_home, LV_OBJ_FLAG_SCROLLABLE);
  build_status_bar(scr_home);

  lv_obj_t *title = lv_label_create(scr_home);
  lv_label_set_text(title, "DUALCORE  LINK");
  lv_obj_add_style(title, &st_title, 0);
  lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 42);

  home_list = lv_obj_create(scr_home);
  lv_obj_remove_style_all(home_list);
  lv_obj_set_size(home_list, LV_HOR_RES - 20, LV_VER_RES - 150);
  lv_obj_align(home_list, LV_ALIGN_TOP_MID, 0, 74);
  lv_obj_set_flex_flow(home_list, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_row(home_list, 8, 0);
  lv_obj_set_style_pad_all(home_list, 4, 0);

  for (int k = 0; k < HOME_MAX_CARDS; ++k) build_home_card(k);

  home_empty = lv_label_create(home_list);
  lv_obj_add_style(home_empty, &st_muted, 0);
  lv_label_set_text(home_empty, "no peers in range\nscanning ...");

  lv_obj_t *nav = lv_obj_create(scr_home);
  lv_obj_remove_style_all(nav);
  lv_obj_set_size(nav, LV_HOR_RES - 20, 56);
  lv_obj_align(nav, LV_ALIGN_BOTTOM_MID, 0, -10);
  lv_obj_set_flex_flow(nav, LV_FLEX_FLOW_ROW);
  lv_obj_set_style_pad_column(nav, 8, 0);
  lv_obj_clear_flag(nav, LV_OBJ_FLAG_SCROLLABLE);

  auto pill = [&](const char *t, lv_event_cb_t cb, uint32_t color) {
    lv_obj_t *b = lv_btn_create(nav);
    lv_obj_set_flex_grow(b, 1);
    lv_obj_set_height(b, LV_PCT(100));
    lv_obj_set_style_radius(b, 12, 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(color), 0);
    lv_obj_t *l = lv_label_create(b); lv_label_set_text(l, t); lv_obj_center(l);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, nullptr);
  };
  pill("LOGS", on_nav_logs, 0x14324a);
  pill("SETTINGS", on_nav_settings, 0x2b1f4a);
  pill(LV_SYMBOL_WARNING " SOS", on_nav_sos, 0x4a1420);

  if (SD_MMC.exists("/WATCH/bin/launcheros.bin")) {
    lv_obj_t *back = lv_btn_create(scr_home);
    lv_obj_set_size(back, 44, 44);
    lv_obj_align(back, LV_ALIGN_TOP_LEFT, 6, 40);
    lv_obj_set_style_radius(back, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(back, lv_color_hex(COL_CARD), 0);
    lv_obj_set_style_border_color(back, lv_color_hex(COL_ACCENT), 0);
    lv_obj_set_style_border_width(back, 1, 0);
    lv_obj_t *bl = lv_label_create(back); lv_label_set_text(bl, LV_SYMBOL_HOME); lv_obj_center(bl);
    lv_obj_add_event_cb(back, on_back_to_launcher, LV_EVENT_CLICKED, nullptr);
  }

  load_screen(scr_home, LV_SCR_LOAD_ANIM_MOVE_LEFT);
  refresh_home_list();
}

// =============================================================================
// UI: Find (radar)
// =============================================================================
static uint8_t find_idx = 0;
static lv_obj_t *find_arc=nullptr, *find_dist=nullptr, *find_hint=nullptr;
static float find_lastDist = -1;

static void on_find_back(lv_event_t *) { ui_home(); }

static void ui_find(uint8_t peerIdx) {
  find_idx = peerIdx; find_lastDist = -1;
  if (scr_find) lv_obj_del(scr_find);
  scr_find = lv_obj_create(nullptr);
  lv_obj_add_style(scr_find, &st_screen, 0);
  build_status_bar(scr_find);

  lv_obj_t *title = lv_label_create(scr_find);
  lv_label_set_text_fmt(title, "FINDING  %s", peers[peerIdx].name);
  lv_obj_set_style_text_color(title, lv_color_hex(COL_AMBER), 0);
  lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 44);

  lv_obj_t *scope = lv_obj_create(scr_find);
  lv_obj_remove_style_all(scope);
  lv_obj_set_size(scope, 250, 250);
  lv_obj_align(scope, LV_ALIGN_CENTER, 0, -4);
  lv_obj_set_style_radius(scope, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_bg_color(scope, lv_color_hex(0x081016), 0);
  lv_obj_set_style_bg_opa(scope, LV_OPA_COVER, 0);
  lv_obj_clear_flag(scope, LV_OBJ_FLAG_SCROLLABLE);

  for (int i = 1; i <= 2; i++) {
    lv_obj_t *ring = lv_obj_create(scope);
    lv_obj_remove_style_all(ring);
    int sz = 250 - i * 75;
    lv_obj_set_size(ring, sz, sz);
    lv_obj_center(ring);
    lv_obj_set_style_radius(ring, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(ring, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_color(ring, lv_color_hex(0x16232f), 0);
    lv_obj_set_style_border_width(ring, 1, 0);
    lv_obj_clear_flag(ring, LV_OBJ_FLAG_SCROLLABLE);
  }

  find_arc = lv_arc_create(scope);
  lv_obj_set_size(find_arc, 246, 246);
  lv_obj_center(find_arc);
  lv_arc_set_rotation(find_arc, 270);
  lv_arc_set_bg_angles(find_arc, 0, 360);
  lv_arc_set_range(find_arc, 0, 100);
  lv_arc_set_value(find_arc, 0);
  lv_obj_remove_style(find_arc, NULL, LV_PART_KNOB);
  lv_obj_clear_flag(find_arc, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_style_arc_color(find_arc, lv_color_hex(0x16232f), LV_PART_MAIN);
  lv_obj_set_style_arc_width(find_arc, 7, LV_PART_MAIN);
  lv_obj_set_style_arc_color(find_arc, lv_color_hex(COL_ACCENT), LV_PART_INDICATOR);
  lv_obj_set_style_arc_width(find_arc, 7, LV_PART_INDICATOR);

  find_dist = lv_label_create(scope);
  lv_obj_set_style_text_font(find_dist, &lv_font_montserrat_28, 0);
  lv_obj_set_style_text_color(find_dist, lv_color_hex(COL_ACCENT), 0);
  lv_label_set_text(find_dist, "-- m");
  lv_obj_align(find_dist, LV_ALIGN_CENTER, 0, -8);
  find_hint = lv_label_create(scope);
  lv_obj_add_style(find_hint, &st_muted, 0);
  lv_label_set_text(find_hint, "searching");
  lv_obj_align(find_hint, LV_ALIGN_CENTER, 0, 22);

  lv_obj_t *back = lv_btn_create(scr_find);
  lv_obj_align(back, LV_ALIGN_BOTTOM_MID, 0, -14);
  lv_obj_set_size(back, 170, 48);
  lv_obj_set_style_radius(back, 12, 0);
  lv_obj_t *bl = lv_label_create(back); lv_label_set_text(bl, LV_SYMBOL_LEFT " BACK"); lv_obj_center(bl);
  lv_obj_add_event_cb(back, on_find_back, LV_EVENT_CLICKED, nullptr);

  load_screen(scr_find, LV_SCR_LOAD_ANIM_FADE_ON);
}

static void find_tick() {
  if (!scr_find || lv_scr_act() != scr_find) return;
  Peer p;
  xSemaphoreTake(peersMx, portMAX_DELAY);
  p = peers[find_idx];
  xSemaphoreGive(peersMx);
  if (!p.inUse) return;
  char distBuf[16]; format_distance(p.distanceM, distBuf, sizeof(distBuf));
  lv_label_set_text(find_dist, distBuf);
  int strength = p.rssi + 100; if (strength < 0) strength = 0; if (strength > 100) strength = 100;
  lv_arc_set_value(find_arc, strength);
  uint32_t col = strength > 55 ? COL_OK : strength > 30 ? COL_ACCENT : COL_AMBER;
  lv_obj_set_style_arc_color(find_arc, lv_color_hex(col), LV_PART_INDICATOR);
  if (find_lastDist >= 0 && p.distanceM >= 0) {
    float thr = p.distanceM * 0.12f + 0.10f;    // ~12% + 10cm dead-band
    if (p.distanceM < find_lastDist - thr)      lv_label_set_text(find_hint, LV_SYMBOL_UP " getting closer");
    else if (p.distanceM > find_lastDist + thr) lv_label_set_text(find_hint, LV_SYMBOL_DOWN " moving away");
    else                                         lv_label_set_text(find_hint, "holding");
  }
  find_lastDist = p.distanceM;
}

// =============================================================================
// UI: Logs
// =============================================================================
static void on_logs_back(lv_event_t *) { ui_home(); }
static void ui_logs() {
  if (scr_logs) lv_obj_del(scr_logs);
  scr_logs = lv_obj_create(nullptr);
  lv_obj_add_style(scr_logs, &st_screen, 0);
  build_status_bar(scr_logs);

  lv_obj_t *title = lv_label_create(scr_logs);
  lv_label_set_text_fmt(title, "LOGS  (%u)", (unsigned)logCount);
  lv_obj_add_style(title, &st_title, 0);
  lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 44);

  lv_obj_t *list = lv_obj_create(scr_logs);
  lv_obj_remove_style_all(list);
  lv_obj_set_size(list, LV_HOR_RES - 20, LV_VER_RES - 150);
  lv_obj_align(list, LV_ALIGN_TOP_MID, 0, 74);
  lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_row(list, 6, 0);
  lv_obj_set_style_pad_all(list, 4, 0);

  size_t n = logCount < 80 ? logCount : 80;
  xSemaphoreTake(peersMx, portMAX_DELAY);
  for (size_t k = 0; k < n; ++k) {
    size_t i = (logHead + LOG_CAP - 1 - k) % LOG_CAP;
    LogEntry &e = logs[i]; Peer &p = peers[e.peerIdx];
    lv_obj_t *row = lv_obj_create(list);
    lv_obj_add_style(row, &st_card, 0);
    lv_obj_set_width(row, LV_PCT(100));
    lv_obj_set_height(row, LV_SIZE_CONTENT);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *l = lv_label_create(row);
    uint32_t agoS = (millis() - e.tsMs) / 1000;
    char distBuf[16]; format_distance(e.distanceM, distBuf, sizeof(distBuf));
    lv_label_set_text_fmt(l, "%s  -%lus  %-6s  %s  %ddBm",
                          e.sos ? LV_SYMBOL_WARNING " SOS" : "SEEN",
                          (unsigned long)agoS, p.name[0] ? p.name : "??????",
                          distBuf, e.rssi);
    lv_obj_set_style_text_color(l, lv_color_hex(e.sos ? COL_DANGER : COL_TEXT), 0);
  }
  xSemaphoreGive(peersMx);

  lv_obj_t *back = lv_btn_create(scr_logs);
  lv_obj_align(back, LV_ALIGN_BOTTOM_MID, 0, -14);
  lv_obj_set_size(back, 170, 48);
  lv_obj_set_style_radius(back, 12, 0);
  lv_obj_t *bl = lv_label_create(back); lv_label_set_text(bl, LV_SYMBOL_LEFT " BACK"); lv_obj_center(bl);
  lv_obj_add_event_cb(back, on_logs_back, LV_EVENT_CLICKED, nullptr);
  load_screen(scr_logs, LV_SCR_LOAD_ANIM_FADE_ON);
}

// =============================================================================
// UI: Settings
// =============================================================================
static lv_obj_t *set_name_ta=nullptr, *set_kb=nullptr;
static lv_obj_t *lbl_scan=nullptr, *lbl_ref=nullptr, *lbl_pl=nullptr;

static void settings_save() {
  NVS.begin("beaconos", false);
  NVS.putString("name", CFG.name); NVS.putUChar("role",(uint8_t)CFG.role);
  NVS.putUChar("tx",(uint8_t)CFG.transport); NVS.putBool("bcast",CFG.broadcast);
  NVS.putBool("vis",CFG.visible); NVS.putBool("fo",CFG.friendOnly);
  NVS.putUShort("scan",CFG.scanEveryMs); NVS.putChar("ref",CFG.rssiAt1m);
  NVS.putFloat("pl",CFG.pathLoss);
  NVS.putBool("dauto",CFG.distanceAuto); NVS.putBool("ftrk",CFG.fastTrack);
  NVS.end();
}
static void settings_load() {
  NVS.begin("beaconos", true);
  if (NVS.isKey("name")) NVS.getString("name", CFG.name, sizeof(CFG.name));
  CFG.role=(Role)NVS.getUChar("role",CFG.role);
  CFG.transport=(Transport)NVS.getUChar("tx",CFG.transport);
  CFG.broadcast=NVS.getBool("bcast",CFG.broadcast);
  CFG.visible=NVS.getBool("vis",CFG.visible);
  CFG.friendOnly=NVS.getBool("fo",CFG.friendOnly);
  CFG.scanEveryMs=NVS.getUShort("scan",CFG.scanEveryMs);
  CFG.rssiAt1m=NVS.getChar("ref",CFG.rssiAt1m);
  CFG.pathLoss=NVS.getFloat("pl",CFG.pathLoss);
  CFG.distanceAuto=NVS.getBool("dauto",CFG.distanceAuto);
  CFG.fastTrack=NVS.getBool("ftrk",CFG.fastTrack);
  NVS.end();
}

static void on_toggle_bcast(lv_event_t *e){ CFG.broadcast=lv_obj_has_state((lv_obj_t*)lv_event_get_target(e),LV_STATE_CHECKED); if(CFG.broadcast)ble_start_advertising();else ble_stop_advertising(); settings_save(); }
static void on_toggle_vis(lv_event_t *e){ CFG.visible=lv_obj_has_state((lv_obj_t*)lv_event_get_target(e),LV_STATE_CHECKED); settings_save(); }
static void on_toggle_friend(lv_event_t *e){ CFG.friendOnly=lv_obj_has_state((lv_obj_t*)lv_event_get_target(e),LV_STATE_CHECKED); settings_save(); }
static void on_role_change(lv_event_t *e){ CFG.role=(Role)lv_dropdown_get_selected((lv_obj_t*)lv_event_get_target(e)); settings_save(); }
static void on_tx_change(lv_event_t *e){
  Transport old=CFG.transport;
  CFG.transport=(Transport)lv_dropdown_get_selected((lv_obj_t*)lv_event_get_target(e));
  settings_save();
  if(old!=CFG.transport){
    if(CFG.transport==TX_BLE_ONLY){mesh_stop();}
    if(CFG.transport==TX_MESH_ONLY){ble_stop_advertising();mesh_start();}
    if(CFG.transport==TX_BOTH){mesh_start(); if(CFG.broadcast)ble_start_advertising();}
  }
}
static void on_scan_change(lv_event_t *e){ int v=lv_slider_get_value((lv_obj_t*)lv_event_get_target(e)); CFG.scanEveryMs=v*1000; lv_label_set_text_fmt(lbl_scan,"%ds",v); settings_save(); }
static void on_ref_change(lv_event_t *e){ CFG.rssiAt1m=(int8_t)lv_slider_get_value((lv_obj_t*)lv_event_get_target(e)); lv_label_set_text_fmt(lbl_ref,"%d dBm",CFG.rssiAt1m); settings_save(); }
static void on_pl_change(lv_event_t *e){ CFG.pathLoss=lv_slider_get_value((lv_obj_t*)lv_event_get_target(e))/10.0f; lv_label_set_text_fmt(lbl_pl,"%.1f",CFG.pathLoss); settings_save(); }
static void on_name_focus(lv_event_t *){ lv_keyboard_set_textarea(set_kb,set_name_ta); lv_obj_clear_flag(set_kb,LV_OBJ_FLAG_HIDDEN); lv_obj_move_foreground(set_kb); }
static void on_kb_done(lv_event_t *){ const char*t=lv_textarea_get_text(set_name_ta); strncpy(CFG.name,t,6); CFG.name[6]=0; settings_save(); lv_obj_add_flag(set_kb,LV_OBJ_FLAG_HIDDEN); }
static void on_settings_back(lv_event_t *){ ui_home(); }
static void on_toggle_dauto(lv_event_t *e){ CFG.distanceAuto=lv_obj_has_state((lv_obj_t*)lv_event_get_target(e),LV_STATE_CHECKED); settings_save(); }
static void on_toggle_ftrk(lv_event_t *e){ CFG.fastTrack=lv_obj_has_state((lv_obj_t*)lv_event_get_target(e),LV_STATE_CHECKED); settings_save(); }

// --- small styling helpers so switches/sliders match the accent theme -------
static void style_switch(lv_obj_t *sw) {
  lv_obj_set_style_bg_color(sw, lv_color_hex(COL_INACTIVE), LV_PART_MAIN);
  lv_obj_set_style_bg_color(sw, lv_color_hex(COL_ACCENT), LV_PART_INDICATOR | LV_STATE_CHECKED);
}
static void style_slider(lv_obj_t *s) {
  lv_obj_set_style_bg_color(s, lv_color_hex(COL_INACTIVE), LV_PART_MAIN);
  lv_obj_set_style_bg_color(s, lv_color_hex(COL_ACCENT), LV_PART_INDICATOR);
  lv_obj_set_style_bg_color(s, lv_color_hex(COL_ACCENT), LV_PART_KNOB);
}
static lv_obj_t* set_section(lv_obj_t *box, const char *t) {
  lv_obj_t *l = lv_label_create(box);
  lv_obj_set_style_text_color(l, lv_color_hex(COL_ACCENT), 0);
  lv_obj_set_style_pad_top(l, 8, 0);
  lv_obj_set_style_pad_bottom(l, 2, 0);
  lv_label_set_text(l, t);
  return l;
}

static lv_obj_t* set_row(lv_obj_t *box, const char *t) {
  lv_obj_t *r = lv_obj_create(box);
  lv_obj_add_style(r, &st_card, 0);
  lv_obj_set_width(r, LV_PCT(100));
  lv_obj_set_height(r, LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(r, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(r, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_clear_flag(r, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_t *l = lv_label_create(r);
  lv_label_set_text(l, t);
  lv_obj_set_style_text_color(l, lv_color_hex(COL_TEXT), 0);
  return r;
}

static void ui_settings() {
  if (scr_settings) lv_obj_del(scr_settings);
  scr_settings = lv_obj_create(nullptr);
  lv_obj_add_style(scr_settings, &st_screen, 0);
  build_status_bar(scr_settings);

  lv_obj_t *title = lv_label_create(scr_settings);
  lv_label_set_text(title, "SETTINGS");
  lv_obj_add_style(title, &st_title, 0);
  lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 44);

  lv_obj_t *box = lv_obj_create(scr_settings);
  lv_obj_remove_style_all(box);
  lv_obj_set_size(box, LV_HOR_RES - 20, LV_VER_RES - 150);
  lv_obj_align(box, LV_ALIGN_TOP_MID, 0, 74);
  lv_obj_set_flex_flow(box, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_row(box, 8, 0);
  lv_obj_set_style_pad_all(box, 4, 0);

  set_section(box, "IDENTITY");
  lv_obj_t *rn = set_row(box, "Name");
  set_name_ta = lv_textarea_create(rn);
  lv_textarea_set_one_line(set_name_ta, true);
  lv_textarea_set_max_length(set_name_ta, 6);
  lv_textarea_set_text(set_name_ta, CFG.name);
  lv_obj_set_width(set_name_ta, 130);
  lv_obj_add_event_cb(set_name_ta, on_name_focus, LV_EVENT_FOCUSED, nullptr);

  set_section(box, "RADIO");
  lv_obj_t *r1 = set_row(box, "Role");
  lv_obj_t *dd = lv_dropdown_create(r1);
  lv_dropdown_set_options(dd, "Watch\nBeacon\nBoth");
  lv_dropdown_set_selected(dd, (uint8_t)CFG.role);
  lv_obj_add_event_cb(dd, on_role_change, LV_EVENT_VALUE_CHANGED, nullptr);

  lv_obj_t *r2 = set_row(box, "Transport");
  lv_obj_t *tx = lv_dropdown_create(r2);
  lv_dropdown_set_options(tx, "BLE only\nMesh only\nBoth");
  lv_dropdown_set_selected(tx, (uint8_t)CFG.transport);
  lv_obj_add_event_cb(tx, on_tx_change, LV_EVENT_VALUE_CHANGED, nullptr);

  lv_obj_t *r3 = set_row(box, "Broadcast");
  lv_obj_t *sw1 = lv_switch_create(r3);
  if (CFG.broadcast) lv_obj_add_state(sw1, LV_STATE_CHECKED);
  style_switch(sw1);
  lv_obj_add_event_cb(sw1, on_toggle_bcast, LV_EVENT_VALUE_CHANGED, nullptr);
  lv_obj_t *r4 = set_row(box, "Visible");
  lv_obj_t *sw2 = lv_switch_create(r4);
  if (CFG.visible) lv_obj_add_state(sw2, LV_STATE_CHECKED);
  style_switch(sw2);
  lv_obj_add_event_cb(sw2, on_toggle_vis, LV_EVENT_VALUE_CHANGED, nullptr);
  lv_obj_t *r5 = set_row(box, "Friend only");
  lv_obj_t *sw3 = lv_switch_create(r5);
  if (CFG.friendOnly) lv_obj_add_state(sw3, LV_STATE_CHECKED);
  style_switch(sw3);
  lv_obj_add_event_cb(sw3, on_toggle_friend, LV_EVENT_VALUE_CHANGED, nullptr);

  set_section(box, "DISTANCE & TRACKING");
  lv_obj_t *ru = set_row(box, "Auto cm / m");
  lv_obj_t *swU = lv_switch_create(ru);
  if (CFG.distanceAuto) lv_obj_add_state(swU, LV_STATE_CHECKED);
  style_switch(swU);
  lv_obj_add_event_cb(swU, on_toggle_dauto, LV_EVENT_VALUE_CHANGED, nullptr);

  lv_obj_t *rf = set_row(box, "Fast track (Find)");
  lv_obj_t *swF = lv_switch_create(rf);
  if (CFG.fastTrack) lv_obj_add_state(swF, LV_STATE_CHECKED);
  style_switch(swF);
  lv_obj_add_event_cb(swF, on_toggle_ftrk, LV_EVENT_VALUE_CHANGED, nullptr);

  lv_obj_t *r6 = set_row(box, "Scan every");
  lbl_scan = lv_label_create(r6);
  lv_obj_add_style(lbl_scan, &st_muted, 0);
  lv_label_set_text_fmt(lbl_scan, "%ds", CFG.scanEveryMs / 1000);
  lv_obj_t *s6 = lv_slider_create(box);
  lv_obj_set_width(s6, LV_PCT(92));
  lv_slider_set_range(s6, 1, 30);
  lv_slider_set_value(s6, CFG.scanEveryMs / 1000, LV_ANIM_OFF);
  style_slider(s6);
  lv_obj_add_event_cb(s6, on_scan_change, LV_EVENT_VALUE_CHANGED, nullptr);

  lv_obj_t *r7 = set_row(box, "RSSI @ 1m");
  lbl_ref = lv_label_create(r7);
  lv_obj_add_style(lbl_ref, &st_muted, 0);
  lv_label_set_text_fmt(lbl_ref, "%d dBm", CFG.rssiAt1m);
  lv_obj_t *s7 = lv_slider_create(box);
  lv_obj_set_width(s7, LV_PCT(92));
  lv_slider_set_range(s7, -90, -40);
  lv_slider_set_value(s7, CFG.rssiAt1m, LV_ANIM_OFF);
  style_slider(s7);
  lv_obj_add_event_cb(s7, on_ref_change, LV_EVENT_VALUE_CHANGED, nullptr);

  lv_obj_t *r8 = set_row(box, "Path loss");
  lbl_pl = lv_label_create(r8);
  lv_obj_add_style(lbl_pl, &st_muted, 0);
  lv_label_set_text_fmt(lbl_pl, "%.1f", CFG.pathLoss);
  lv_obj_t *s8 = lv_slider_create(box);
  lv_obj_set_width(s8, LV_PCT(92));
  lv_slider_set_range(s8, 15, 40);
  lv_slider_set_value(s8, (int)(CFG.pathLoss * 10), LV_ANIM_OFF);
  style_slider(s8);
  lv_obj_add_event_cb(s8, on_pl_change, LV_EVENT_VALUE_CHANGED, nullptr);

  lv_obj_t *hintc = lv_label_create(box);
  lv_obj_add_style(hintc, &st_muted, 0);
  lv_obj_set_width(hintc, LV_PCT(96));
  lv_label_set_long_mode(hintc, LV_LABEL_LONG_WRAP);
  lv_label_set_text(hintc, "Tip: stand 1 m away, then set RSSI @ 1m to the dBm shown in Logs. Lower path loss = open space, higher = walls.");

  lv_obj_t *back = lv_btn_create(box);
  lv_obj_set_width(back, LV_PCT(100));
  lv_obj_set_height(back, 48);
  lv_obj_set_style_radius(back, 12, 0);
  lv_obj_t *bl = lv_label_create(back); lv_label_set_text(bl, LV_SYMBOL_LEFT " BACK"); lv_obj_center(bl);
  lv_obj_add_event_cb(back, on_settings_back, LV_EVENT_CLICKED, nullptr);

  set_kb = lv_keyboard_create(scr_settings);
  lv_obj_add_flag(set_kb, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_event_cb(set_kb, on_kb_done, LV_EVENT_READY, nullptr);
  lv_obj_add_event_cb(set_kb, on_kb_done, LV_EVENT_CANCEL, nullptr);

  load_screen(scr_settings, LV_SCR_LOAD_ANIM_FADE_ON);
}

// =============================================================================
// UI: SOS / launcher / splash
// =============================================================================
static uint32_t sos_untilMs = 0;
static void ui_sos_broadcast() {
  sos_untilMs = millis() + 30000;
  Flags f{}; f.broadcast = 1; f.sos = 1;
  ble_build_payload(read_battery_pct(), f, millis() / 1000);
  ble_stop_advertising(); ble_start_advertising();
#if HAVE_MESH
  if (CFG.transport != TX_BLE_ONLY) mesh.sendBroadcast(String("SOS|") + CFG.name);
#endif
  lv_obj_t *toast = lv_label_create(lv_layer_top());
  lv_label_set_text(toast, LV_SYMBOL_WARNING "  SOS broadcasting 30s");
  lv_obj_set_style_bg_color(toast, lv_color_hex(0x4a1420), 0);
  lv_obj_set_style_bg_opa(toast, LV_OPA_COVER, 0);
  lv_obj_set_style_text_color(toast, lv_color_hex(0xffe6ea), 0);
  lv_obj_set_style_radius(toast, 10, 0);
  lv_obj_set_style_pad_all(toast, 12, 0);
  lv_obj_align(toast, LV_ALIGN_TOP_MID, 0, 60);
  lv_obj_fade_out(toast, 400, 2600);
}

static void ui_back_to_launcher() {
  USBSerial.println("[back] trying partition switch");
  // Fast path - flip otadata back to `factory` and reboot. This is what
  // the LauncherOS ecosystem is designed around and needs zero SD I/O.
  partition_switch_to_launcher();
  // Fallback - re-flash the OTA slot from SD if factory is missing.
  USBSerial.println("[back] partition switch failed, trying SD fallback");
  if (sd_flash_launcher_bin()) return;
  // Last resort: just reboot; user can pick launcher manually.
  USBSerial.println("[back] both paths failed, hard reboot");
  esp_restart();
}

static void ui_boot_splash() {
  lv_obj_t *s = lv_obj_create(nullptr);
  lv_obj_add_style(s, &st_screen, 0);
  lv_obj_t *l1 = lv_label_create(s);
  lv_label_set_text(l1, "BEACON // OS");
  lv_obj_set_style_text_font(l1, &lv_font_montserrat_28, 0);
  lv_obj_set_style_text_color(l1, lv_color_hex(COL_ACCENT), 0);
  lv_obj_align(l1, LV_ALIGN_CENTER, 0, -20);
  lv_obj_t *l2 = lv_label_create(s);
  lv_label_set_text(l2, "dualcore link  v2.5");
  lv_obj_set_style_text_color(l2, lv_color_hex(COL_AMBER), 0);
  lv_obj_align(l2, LV_ALIGN_CENTER, 0, 16);
  lv_scr_load(s);
  for (int i = 0; i < 30; i++) { lv_timer_handler(); delay(20); }
}

// =============================================================================
// Boot-button safety - 1.5 s hold anywhere in the app -> back to launcher
// (partition-flip path, no SD needed).
// =============================================================================
static uint32_t bootPressStartMs = 0;
static void boot_safety_tick() {
  if (digitalRead(0) == LOW) {
    if (bootPressStartMs == 0) bootPressStartMs = millis();
    if (millis() - bootPressStartMs > 1500) {
      USBSerial.println("[boot] long-press -> launcher");
      ui_back_to_launcher();     // partition-switch + fallback SD reflash
    }
  } else bootPressStartMs = 0;
}

// =============================================================================
// setup()
// =============================================================================
void setup() {
  USBSerial.begin(115200);
  USBSerial.println("BeaconOS v2.5 (LVGL 8.4)");
  pinMode(0, INPUT_PULLUP);
  peersMx = xSemaphoreCreateMutex();

  if (!gfx->begin()) USBSerial.println("gfx->begin() failed!");
  gfx->fillScreen(RGB565_BLACK);
#if PANEL_INVERT
  gfx->invertDisplay(true);   // flip if colours look like a photo-negative
#endif

  // --- I2C bus recovery + fast-mode clock (matches LauncherOS) -------------
  pinMode(IIC_SCL, OUTPUT);
  for (int i = 0; i < 9; i++) { digitalWrite(IIC_SCL, LOW);  delayMicroseconds(5);
                                digitalWrite(IIC_SCL, HIGH); delayMicroseconds(5); }
  Wire.begin(IIC_SDA, IIC_SCL);
  Wire.setClock(400000);

  // --- Touch reset pulse then init (fixes the FT3168 I2C NACK / dead touch)-
  pinMode(TP_RESET, OUTPUT);
  digitalWrite(TP_RESET, LOW);  delay(50);
  digitalWrite(TP_RESET, HIGH); delay(200);
  pinMode(TP_INT, INPUT_PULLUP);
  uint32_t t0 = millis();
  while (FT3168->begin() == false) {
    USBSerial.println("FT3168 retry...");
    delay(200);
    if (millis() - t0 > 3000) { USBSerial.println("FT3168 giving up"); break; }
  }
  FT3168->IIC_Write_Device_State(FT3168->Arduino_IIC_Touch::Device::TOUCH_POWER_MODE,
                                 FT3168->Arduino_IIC_Touch::Device_Mode::TOUCH_POWER_MONITOR);
  pmu_setup();

  lv_init();
  theme_init();
  screenWidth = gfx->width(); screenHeight = gfx->height();
  bufSize = screenWidth * LVGL_BUF_LINES;
  // Single partial buffer (LauncherOS parity). Prefer internal RAM, fall back
  // to PSRAM. A single buffer avoids the dual-PSRAM-buffer flush crash.
  buf1 = (lv_color_t *)heap_caps_malloc(bufSize * sizeof(lv_color_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  if (!buf1) buf1 = (lv_color_t *)heap_caps_malloc(bufSize * sizeof(lv_color_t), MALLOC_CAP_8BIT);
  if (!buf1) { USBSerial.println("LVGL buf alloc failed"); return; }

  static lv_disp_draw_buf_t draw_buf;
  lv_disp_draw_buf_init(&draw_buf, buf1, NULL, bufSize);
  static lv_disp_drv_t disp_drv;
  lv_disp_drv_init(&disp_drv);
  disp_drv.hor_res = screenWidth; disp_drv.ver_res = screenHeight;
  disp_drv.flush_cb = my_disp_flush;
  disp_drv.draw_buf = &draw_buf;
  disp = lv_disp_drv_register(&disp_drv);

  static lv_indev_drv_t indev_drv;
  lv_indev_drv_init(&indev_drv);
  indev_drv.type = LV_INDEV_TYPE_POINTER;
  indev_drv.read_cb = my_touchpad_read;
  lv_indev_drv_register(&indev_drv);

  SD_MMC.setPins(SDMMC_CLK, SDMMC_CMD, SDMMC_DATA);
  if (!SD_MMC.begin("/sdcard", true)) USBSerial.println("SD_MMC init failed");
  settings_load();
  wallpaper_init_from_launcher();   // NEW: read /WATCH/PHOTOS + `dcw/wp_idx`
  USBSerial.printf("[wp] %d photos, using idx=%d\n", g_photo_count, g_photo_idx);
  ui_boot_splash();

  // ---- Time: instant from NVS, then WiFi/NTP top-up (LauncherOS parity) ---
  // Done BEFORE BLE init so WiFi and BLE never share the antenna.
  time_restore_from_nvs();
  parse_wifi_config();          // /WATCH/wifi/config.txt from the SD card
  wifi_connect_and_sync();      // SD nets -> hardcoded -> NTP -> save -> WiFi off

  BLEDevice::init(CFG.name);
  adv = BLEDevice::getAdvertising();
  scanner = BLEDevice::getScan();
  scanner->setAdvertisedDeviceCallbacks(&scanCb, true);
  scanner->setActiveScan(true);
  scanner->setInterval(160); scanner->setWindow(120);

  Flags f{}; f.broadcast = CFG.broadcast ? 1 : 0;
  ble_build_payload(read_battery_pct(), f, 0);
  if (CFG.broadcast && CFG.visible) ble_start_advertising();

  if (CFG.transport != TX_BLE_ONLY) mesh_start();
  if (CFG.transport == TX_BLE_ONLY) { WiFi.mode(WIFI_OFF); esp_wifi_stop(); }

  xTaskCreatePinnedToCore(ble_scan_task, "ble_scan", 4096, nullptr, 1, nullptr, 0);

  ui_watchface();
  USBSerial.println("Setup done");
}

// =============================================================================
// loop()
// =============================================================================
void loop() {
  static uint32_t lastRefr = 0, lastPayld = 0, lastStatus = 0;
  uint32_t now = millis();

  lv_timer_handler();

  lv_obj_t *cur = lv_scr_act();
  g_find_active = (cur == scr_find);   // tells the scan task to poll faster

  // Consume a queued swipe (set on finger-up in my_touchpad_read). Infinite
  // loop: any horizontal swipe flips between the watch face and the beacon
  // list; swipe UP on the watch face fires SOS.
  if (g_pending_swipe != SW_NONE) {
    SwipeEv ev = g_pending_swipe; g_pending_swipe = SW_NONE;
    g_lastSwipeMs = now;
    if (cur == scr_watchface) { if (ev == SW_UP) ui_sos_broadcast(); else ui_home(); }
    else if (cur == scr_home) { if (ev != SW_UP) ui_watchface(); }
  }

#if HAVE_MESH
  if (CFG.transport != TX_BLE_ONLY) mesh.update();
#endif

  uint32_t refrGap = (cur == scr_find) ? 400 : 1000;   // snappier while tracking
  if (now - lastRefr > refrGap) {
    lastRefr = now;
    if (cur == scr_home)      refresh_home_list();
    else if (cur == scr_find) find_tick();
    else if (cur == scr_watchface) status_bar_tick();
  }
  if (now - lastPayld > 5000 && CFG.broadcast) {
    lastPayld = now;
    Flags f{}; f.broadcast = 1; f.sos = (now < sos_untilMs) ? 1 : 0;
    ble_build_payload(read_battery_pct(), f, now / 1000);
    ble_stop_advertising();
    if (CFG.visible) ble_start_advertising();
  }
  if (now - lastStatus > 3000) { lastStatus = now; status_bar_tick(); }

  wifi_resync_if_due();   // 12-hourly NTP top-up (LauncherOS parity)
  boot_safety_tick();
  delay(5);
}
