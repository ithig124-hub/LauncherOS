// =============================================================================
// GPSModule.ino  --  Xander GPS node  (Seeed Studio XIAO ESP32-S3 + L76K GNSS)
// Role   : BLE PERIPHERAL / SERVER  (the watch is the central/client)
//
//        XIAO ESP32-S3   --UART-->   L76K GNSS
//             |  BLE (XANDER_GPS_SERVICE)
//             v
//           WATCH (GPSOS.ino)
//
// The XIAO owns the L76K. The watch never sees the L76K -- it only speaks the
// stable Xander GPS BLE protocol. This makes the whole Xander ecosystem
// (Xavier / Fusion Watch / phone / computer / future GPS nodes) expandable.
//
// SINGLE-FILE Arduino sketch. Reliability + modularity over UI.
//
// LIBRARY DEPENDENCIES:
//   - TinyGPSPlus                 (GNSS/NMEA parser)
//   - NimBLE-Arduino  >= 1.4.x    (BLE peripheral)
//   - ArduinoJson     >= 6.x      (compact JSON packets)
//   - SD / FS  (ESP32 core)       (microSD, if present)
//   - esp_sleep / esp_system (ESP32 core)  (deep sleep, RTC/timer wake)
// =============================================================================

#include <Arduino.h>
#include <TinyGPSPlus.h>
#include <NimBLEDevice.h>
#include <ArduinoJson.h>
#include <FS.h>
#include <SD.h>
#include <SPI.h>
#include <Preferences.h>
#include <math.h>
#include <esp_sleep.h>
#include <esp_system.h>
#include <driver/rtc_io.h>

// =============================================================================
// SECTION 1 -- CONFIGURATION  (>>> CHANGE HARDWARE VALUES HERE <<<)
// =============================================================================
// ---- UART to the L76K -------------------------------------------------------
// >>> HARDWARE: set these to the XIAO pads you wired the L76K to.
// >>> XIAO ESP32-S3 pads: D0..D10 map to specific GPIOs -- confirm YOUR wiring.
// >>> Common choice: GPS_RX <- L76K TX, GPS_TX -> L76K RX.
#define GPS_UART_RX_PIN   44   // <<< CHANGE: XIAO RX  (from L76K TX)
#define GPS_UART_TX_PIN   43   // <<< CHANGE: XIAO TX  (to   L76K RX)
#define GPS_UART_BAUD     9600 // <<< CHANGE if your L76K default differs (often 9600)
#define GPS_UART_NUM      1    // use Serial1
HardwareSerial GpsSerial(GPS_UART_NUM);

// ---- Optional L76K standby/wake control pin ---------------------------------
// >>> HARDWARE: L76K has a low-power standby pin on some breakouts. -1 = unused.
#define L76K_STANDBY_PIN  -1   // <<< CHANGE or leave -1

// ---- microSD (XIAO ESP32-S3 Sense onboard slot uses SPI) --------------------
// >>> HARDWARE: set to your board. Set SD_ENABLED 0 if you have no card slot.
#define SD_ENABLED        1
#define SD_CS_PIN         21   // <<< CHANGE: XIAO Sense onboard SD CS (verify!)
#define SD_SCK_PIN        7    // <<< CHANGE
#define SD_MISO_PIN       8    // <<< CHANGE
#define SD_MOSI_PIN       9    // <<< CHANGE

// ---- Battery ADC ------------------------------------------------------------
// >>> HARDWARE: XIAO ESP32-S3 has NO simple onboard battery divider by default.
// >>> If you added a divider, set the pin + ratio. -1 = report -1 (unknown).
#define BAT_ADC_PIN       -1   // <<< CHANGE if you wired a divider (e.g. A0/GPIO1)
#define BAT_ADC_RATIO     2.0f // divider ratio (Vbat = Vadc * ratio)
#define BAT_FULL_MV       4200
#define BAT_EMPTY_MV      3300

// ---- USB power detect (to exit deep sleep / stay awake) ---------------------
// >>> HARDWARE: optional VBUS-sense GPIO. -1 = disabled.
#define USB_SENSE_PIN     -1   // <<< CHANGE or leave -1

// ---- RTC / deep-sleep wake --------------------------------------------------
// A GPIO the watch can pulse to wake the module (ext0). -1 = timer wake only.
#define WAKE_GPIO_PIN     GPIO_NUM_2   // <<< CHANGE or set timer-only

// =============================================================================
//                    XANDER GPS BLE PROTOCOL  (SHARED CONTRACT)
//   >>> IDENTICAL to GPSOS.ino. Keep in perfect sync. protocol_version = 1 <<<
// =============================================================================
#define XG_PROTOCOL_VERSION  1
#define XANDER_GPS_SERVICE_UUID "7a3e5b00-6f2d-4c8a-9b1e-000000000001"
#define XG_CHAR_LOCATION_UUID   "7a3e5b00-6f2d-4c8a-9b1e-000000000002"
#define XG_CHAR_STATUS_UUID     "7a3e5b00-6f2d-4c8a-9b1e-000000000003"
#define XG_CHAR_BATTERY_UUID    "7a3e5b00-6f2d-4c8a-9b1e-000000000004"
#define XG_CHAR_TRACKING_UUID   "7a3e5b00-6f2d-4c8a-9b1e-000000000005"
#define XG_CHAR_COMMAND_UUID    "7a3e5b00-6f2d-4c8a-9b1e-000000000006"  // watch writes
#define XG_CHAR_CONFIG_UUID     "7a3e5b00-6f2d-4c8a-9b1e-000000000007"  // watch writes

enum XgCmd : uint8_t {
    XG_GET_LOCATION = 0x01, XG_GET_STATUS = 0x02,
    XG_START_TRACK  = 0x03, XG_STOP_TRACK = 0x04, XG_PAUSE_TRACK = 0x05,
    XG_SET_INTERVAL = 0x06, XG_SET_DURATION = 0x07,
    XG_GET_HISTORY  = 0x08, XG_SYNC = 0x09, XG_SLEEP = 0x0A, XG_WAKE = 0x0B,
};

// ---- Xander device identity -------------------------------------------------
#define XG_DEVICE_TYPE     "gps"
#define XG_DEVICE_NAME     "Xander GPS"
#define XG_FW_VERSION      "1.0.0"

// =============================================================================
// SECTION 2 -- GPS SERVICE (background state; runs regardless of the UI)
// =============================================================================
TinyGPSPlus gps;

struct GpsState {
    double  lat = 0, lon = 0;
    float   alt = 0, speed_mps = 0, accuracy = 99.9f;
    int     heading = 0, satellites = 0;
    bool    fix = false;
    char    fix_kind[8] = "NONE";   // NONE / 2D / 3D
    char    iso_time[24] = "";      // UTC
    uint32_t last_update_ms = 0;
};
static GpsState g_st;

static void gps_update_state() {
    if (gps.location.isValid()) {
        g_st.lat = gps.location.lat();
        g_st.lon = gps.location.lng();
        g_st.fix = true;
    }
    if (gps.altitude.isValid()) g_st.alt = gps.altitude.meters();
    if (gps.speed.isValid())    g_st.speed_mps = gps.speed.mps();
    if (gps.course.isValid())   g_st.heading = (int)gps.course.deg();
    if (gps.satellites.isValid()) g_st.satellites = gps.satellites.value();
    // HDOP -> rough horizontal accuracy estimate (metres). ~ hdop * 2.5m UERE.
    if (gps.hdop.isValid()) {
        float hdop = gps.hdop.hdop();
        if (hdop > 0) g_st.accuracy = hdop * 2.5f;
    }
    g_st.fix = gps.location.isValid() && (gps.satellites.value() >= 3);
    strncpy(g_st.fix_kind, g_st.fix ? (g_st.alt != 0 ? "3D" : "2D") : "NONE", sizeof(g_st.fix_kind));
    if (gps.date.isValid() && gps.time.isValid()) {
        snprintf(g_st.iso_time, sizeof(g_st.iso_time), "%04d-%02d-%02dT%02d:%02d:%02dZ",
                 gps.date.year(), gps.date.month(), gps.date.day(),
                 gps.time.hour(), gps.time.minute(), gps.time.second());
    }
    g_st.last_update_ms = millis();
}

// ---- public GPS service API (mirrors the watch-side Xander API) -------------
static GpsState getLocation()     { return g_st; }
static bool     getStatusFix()    { return g_st.fix; }
static float    getSpeed()        { return g_st.speed_mps; }
static int      getHeading()      { return g_st.heading; }
static float    getAccuracy()     { return g_st.accuracy; }
static int      getSatelliteCount(){ return g_st.satellites; }

// ---- battery ----------------------------------------------------------------
static int getBattery() {
    if (BAT_ADC_PIN < 0) return -1;
    int raw = analogReadMilliVolts(BAT_ADC_PIN);
    int mv = (int)(raw * BAT_ADC_RATIO);
    int pct = (int)(100.0f * (mv - BAT_EMPTY_MV) / (BAT_FULL_MV - BAT_EMPTY_MV));
    if (pct < 0) pct = 0; if (pct > 100) pct = 100;
    return pct;
}

// =============================================================================
// SECTION 3 -- STORAGE  ( /GPS/current.json, /GPS/tracks/*.gpx, /GPS/history )
// =============================================================================
static bool g_sd_ok = false;
static SPIClass g_sd_spi(FSPI);

static void storage_begin() {
#if SD_ENABLED
    g_sd_spi.begin(SD_SCK_PIN, SD_MISO_PIN, SD_MOSI_PIN, SD_CS_PIN);
    if (SD.begin(SD_CS_PIN, g_sd_spi)) {
        g_sd_ok = true;
        if (!SD.exists("/GPS"))         SD.mkdir("/GPS");
        if (!SD.exists("/GPS/tracks"))  SD.mkdir("/GPS/tracks");
        if (!SD.exists("/GPS/history")) SD.mkdir("/GPS/history");
        Serial.println("[sd] mounted, /GPS ready");
    } else {
        Serial.println("[sd] mount FAILED -- running SD-less");
    }
#else
    Serial.println("[sd] disabled at compile time");
#endif
}
static const char* storage_status() { return g_sd_ok ? "SD OK" : "no SD"; }

static void storage_write_current() {
    if (!g_sd_ok) return;
    File f = SD.open("/GPS/current.json", FILE_WRITE);
    if (!f) return;
    StaticJsonDocument<256> d;
    d["lat"]=g_st.lat; d["lon"]=g_st.lon; d["alt"]=g_st.alt;
    d["speed"]=g_st.speed_mps; d["heading"]=g_st.heading; d["accuracy"]=g_st.accuracy;
    d["satellites"]=g_st.satellites; d["fix"]=g_st.fix; d["time"]=g_st.iso_time;
    serializeJson(d, f); f.close();
}

// ---- GPX track logging ------------------------------------------------------
static File     g_gpx;
static char     g_gpx_path[48] = "";
static bool     g_gpx_open = false;
static void gpx_open_new() {
    if (!g_sd_ok) return;
    // filename: YYYY-MM-DD_track_NNN.gpx (increment NNN)
    char date[12];
    if (gps.date.isValid()) snprintf(date, sizeof(date), "%04d-%02d-%02d", gps.date.year(), gps.date.month(), gps.date.day());
    else snprintf(date, sizeof(date), "0000-00-00");
    int n = 1;
    do { snprintf(g_gpx_path, sizeof(g_gpx_path), "/GPS/tracks/%s_track_%03d.gpx", date, n++); }
    while (SD.exists(g_gpx_path) && n < 1000);
    g_gpx = SD.open(g_gpx_path, FILE_WRITE);
    if (!g_gpx) { Serial.println("[gpx] open failed"); return; }
    g_gpx.print("<?xml version=\"1.0\"?>\n<gpx version=\"1.1\" creator=\"Xander GPS\">\n<trk><name>");
    g_gpx.print(g_gpx_path); g_gpx.print("</name><trkseg>\n");
    g_gpx.flush();
    g_gpx_open = true;
    Serial.printf("[gpx] logging -> %s\n", g_gpx_path);
}
static void gpx_write_point() {
    if (!g_gpx_open) return;
    g_gpx.printf("<trkpt lat=\"%.6f\" lon=\"%.6f\"><ele>%.1f</ele><time>%s</time>"
                 "<speed>%.2f</speed><course>%d</course></trkpt>\n",
                 g_st.lat, g_st.lon, g_st.alt, g_st.iso_time, g_st.speed_mps, g_st.heading);
    g_gpx.flush();
}
static void gpx_close() {
    if (!g_gpx_open) return;
    g_gpx.print("</trkseg></trk></gpx>\n"); g_gpx.flush(); g_gpx.close();
    g_gpx_open = false;
    Serial.println("[gpx] closed");
}

// =============================================================================
// SECTION 4 -- TRACKING  (intervals, unlimited/timed, distance/speed stats)
// =============================================================================
struct Tracking {
    bool     active = false;
    bool     paused = false;
    int      interval_s = 5;      // 1,5,10,30,60,300
    int      duration_s = 0;      // 0 = unlimited
    uint32_t start_ms = 0;
    uint32_t last_point_ms = 0;
    uint32_t elapsed_s = 0;
    double   last_lat = 0, last_lon = 0;
    bool     have_last = false;
    float    distance_m = 0;
    float    max_speed = 0;
    double   speed_sum = 0;
    int      points = 0;
};
static Tracking g_tk;

// ALWAYS-ON mode: GNSS + tracking run continuously until explicitly turned off.
// Overrides duration (forced unlimited) and blocks all sleep/standby.
static bool g_always_on = false;
static bool g_always_auto_usb = false;   // set when always-on was auto-enabled by USB

static double hav_m(double la1,double lo1,double la2,double lo2){
    const double R=6371000.0, r=M_PI/180.0;
    double dla=(la2-la1)*r, dlo=(lo2-lo1)*r;
    double a=sin(dla/2)*sin(dla/2)+cos(la1*r)*cos(la2*r)*sin(dlo/2)*sin(dlo/2);
    return R*2*atan2(sqrt(a),sqrt(1-a));
}
static void track_start() {
    if (g_tk.active) return;
    g_tk = Tracking();          // reset stats but keep configured interval/duration
    Preferences p; p.begin("xgps", true);
    g_tk.interval_s = p.getInt("interval", 5);
    g_tk.duration_s = p.getInt("duration", 0);
    p.end();
    if (g_always_on) g_tk.duration_s = 0;   // always-on forces unlimited
    g_tk.active = true; g_tk.start_ms = millis();
    gpx_open_new();
    Serial.printf("[track] start interval=%ds duration=%ds always=%d\n",
                  g_tk.interval_s, g_tk.duration_s, g_always_on);
}
static void track_stop() {
    if (!g_tk.active) return;
    g_tk.active = false; g_tk.paused = false;
    gpx_close();
    // append summary to history.json
    if (g_sd_ok) {
        File f = SD.open("/GPS/history/history.json", FILE_APPEND);
        if (f) {
            StaticJsonDocument<256> d;
            char date[12];
            if (gps.date.isValid()) snprintf(date,sizeof(date),"%04d-%02d-%02d",gps.date.year(),gps.date.month(),gps.date.day());
            else strcpy(date,"0000-00-00");
            d["date"]=date; d["duration"]=g_tk.elapsed_s; d["distance"]=g_tk.distance_m;
            d["max"]=g_tk.max_speed; d["avg"]=(g_tk.points?g_tk.speed_sum/g_tk.points:0);
            d["points"]=g_tk.points; d["file"]=g_gpx_path;
            serializeJson(d,f); f.print("\n"); f.close();
        }
    }
    Serial.printf("[track] stop pts=%d dist=%.1fm\n", g_tk.points, g_tk.distance_m);
}
static void track_pause() { if (g_tk.active) { g_tk.paused = !g_tk.paused; Serial.printf("[track] paused=%d\n", g_tk.paused);} }

static void set_always_on(bool on);   // fwd; defined after power helpers (uses g_power/l76k_standby)


static void track_tick() {
    if (!g_tk.active || g_tk.paused) return;
    uint32_t now = millis();
    g_tk.elapsed_s = (now - g_tk.start_ms)/1000;
    if (g_tk.duration_s > 0 && g_tk.elapsed_s >= (uint32_t)g_tk.duration_s) { track_stop(); return; }
    if (now - g_tk.last_point_ms < (uint32_t)g_tk.interval_s*1000) return;
    if (!g_st.fix) return;
    g_tk.last_point_ms = now;
    if (g_tk.have_last) g_tk.distance_m += hav_m(g_tk.last_lat, g_tk.last_lon, g_st.lat, g_st.lon);
    g_tk.last_lat = g_st.lat; g_tk.last_lon = g_st.lon; g_tk.have_last = true;
    if (g_st.speed_mps > g_tk.max_speed) g_tk.max_speed = g_st.speed_mps;
    g_tk.speed_sum += g_st.speed_mps; g_tk.points++;
    gpx_write_point();
    storage_write_current();
}

// =============================================================================
// SECTION 5 -- POWER MANAGEMENT (ACTIVE/TRACKING/STANDBY/SCHEDULED/DEEP_SLEEP)
// =============================================================================
enum PowerMode { PM_ACTIVE, PM_TRACKING, PM_STANDBY, PM_SCHEDULED, PM_DEEP_SLEEP };
static PowerMode g_power = PM_ACTIVE;
static uint32_t  g_sched_wake_s = 0;   // scheduled wake interval (seconds)

static bool usb_powered() {
    if (USB_SENSE_PIN < 0) return false;
    return digitalRead(USB_SENSE_PIN) == HIGH;
}
static void l76k_standby(bool on) {
    if (L76K_STANDBY_PIN < 0) return;
    pinMode(L76K_STANDBY_PIN, OUTPUT);
    digitalWrite(L76K_STANDBY_PIN, on ? LOW : HIGH);  // >>> polarity depends on your breakout
}

// ALWAYS-ON: start continuous tracking that runs until turned off. When turned
// off, tracking stops and normal power management resumes.
static void set_always_on(bool on) {
    g_always_on = on;
    Preferences p; p.begin("xgps", false); p.putBool("always", on); p.end();
    if (on) {
        g_tk.paused = false;
        if (!g_tk.active) track_start();
        else g_tk.duration_s = 0;   // upgrade a running timed track to unlimited
        g_power = PM_TRACKING;
        l76k_standby(false);
        Serial.println("[always] ON -- continuous tracking until turned off");
    } else {
        if (g_tk.active) track_stop();
        g_power = PM_ACTIVE;
        Serial.println("[always] OFF");
    }
}
static void enter_deep_sleep(uint32_t seconds) {
    // IMPORTANT: BLE is NOT maintained in deep sleep. The watch must have
    // configured the wake schedule BEFORE we get here.
    Serial.printf("[power] deep sleep for %us\n", seconds);
    gpx_close();
    l76k_standby(true);
    NimBLEDevice::deinit(true);
    if (seconds > 0) esp_sleep_enable_timer_wakeup((uint64_t)seconds * 1000000ULL);
#if defined(WAKE_GPIO_PIN)
    rtc_gpio_pullup_en(WAKE_GPIO_PIN);
    esp_sleep_enable_ext0_wakeup(WAKE_GPIO_PIN, 1);   // watch pulses this HIGH to wake
#endif
    delay(50);
    esp_deep_sleep_start();
}

// WAKE-AT-TIME: schedule a deep-sleep wake at an exact clock time (HH:MM UTC,
// from the GPS clock). The watch picks the time; we compute seconds until the
// next occurrence and arm the SCHEDULED sleep path. Needs a valid GPS time.
static int g_wake_hh = -1, g_wake_mm = -1;
static void schedule_wake_at(int hh, int mm) {
    if (hh < 0 || hh > 23 || mm < 0 || mm > 59) return;
    if (!gps.time.isValid()) { Serial.println("[wake] no GPS time yet -- cannot schedule"); return; }
    int now_sod = gps.time.hour()*3600 + gps.time.minute()*60 + gps.time.second();
    int tgt_sod = hh*3600 + mm*60;
    int delta = tgt_sod - now_sod;
    if (delta <= 0) delta += 86400;            // next occurrence
    g_wake_hh = hh; g_wake_mm = mm;
    g_sched_wake_s = (uint32_t)delta;
    g_power = PM_SCHEDULED;
    Serial.printf("[wake] scheduled %02d:%02d UTC -> sleep in %ds (once idle)\n", hh, mm, delta);
}

// =============================================================================
// SECTION 6 -- FLIGHT MODE  (GNSS+SD on, all TX radios off)
// =============================================================================
static bool g_flight_mode = false;
static void set_flight_mode(bool on) {
    g_flight_mode = on;
    Preferences p; p.begin("xgps", false); p.putBool("flight", on); p.end();
    if (on) {
        Serial.println("[flight] ON: WiFi/BT/LoRa OFF, GNSS+SD logging ON");
        NimBLEDevice::deinit(true);     // Bluetooth OFF
        // WiFi is never started in this sketch; LoRa not present. Both stay OFF.
        if (!g_tk.active) track_start(); // keep recording locally
    } else {
        Serial.println("[flight] OFF");
        // caller should re-init BLE (done in loop guard)
    }
}

// =============================================================================
// SECTION 7 -- XANDER GPS BLE  (peripheral/server)
// =============================================================================
static NimBLEServer         *g_server = nullptr;
static NimBLECharacteristic *g_c_loc=nullptr,*g_c_stat=nullptr,*g_c_batt=nullptr,
                            *g_c_trk=nullptr,*g_c_cmd=nullptr,*g_c_cfg=nullptr;
static volatile bool g_ble_connected = false;

static void notify_location() {
    if (!g_c_loc) return;
    StaticJsonDocument<256> d;
    d["lat"]=g_st.lat; d["lon"]=g_st.lon; d["alt"]=g_st.alt;
    d["speed"]=g_st.speed_mps; d["heading"]=g_st.heading; d["accuracy"]=g_st.accuracy;
    d["satellites"]=g_st.satellites; d["fix"]=g_st.fix; d["battery"]=getBattery();
    d["time"]=g_st.iso_time;
    char buf[256]; size_t n=serializeJson(d,buf,sizeof(buf));
    g_c_loc->setValue((uint8_t*)buf,n); if (g_ble_connected) g_c_loc->notify();
}
static void notify_status() {
    if (!g_c_stat) return;
    StaticJsonDocument<320> d;
    d["fix"]=g_st.fix; d["fixkind"]=g_st.fix_kind; d["satellites"]=g_st.satellites;
    d["accuracy"]=g_st.accuracy; d["battery"]=getBattery();
    d["tracking"]=g_tk.active; d["paused"]=g_tk.paused;
    d["always"]=g_always_on;
    d["interval"]=g_tk.interval_s; d["duration"]=g_tk.duration_s;
    d["fw"]=XG_FW_VERSION; d["storage"]=storage_status();
    d["type"]=XG_DEVICE_TYPE; d["proto"]=XG_PROTOCOL_VERSION;
    if (g_wake_hh >= 0) { char w[8]; snprintf(w,sizeof(w),"%02d:%02d",g_wake_hh,g_wake_mm); d["wake"]=w; }
    char buf[320]; size_t n=serializeJson(d,buf,sizeof(buf));
    g_c_stat->setValue((uint8_t*)buf,n); if (g_ble_connected) g_c_stat->notify();
}
static void notify_battery() {
    if (!g_c_batt) return;
    StaticJsonDocument<64> d; d["battery"]=getBattery();
    char buf[64]; size_t n=serializeJson(d,buf,sizeof(buf));
    g_c_batt->setValue((uint8_t*)buf,n); if (g_ble_connected) g_c_batt->notify();
}
static void notify_tracking() {
    if (!g_c_trk) return;
    StaticJsonDocument<256> d;
    d["active"]=g_tk.active; d["elapsed"]=g_tk.elapsed_s; d["distance"]=g_tk.distance_m;
    d["speed"]=g_st.speed_mps; d["avg"]=(g_tk.points?g_tk.speed_sum/g_tk.points:0);
    d["max"]=g_tk.max_speed; d["points"]=g_tk.points; d["accuracy"]=g_st.accuracy;
    char buf[256]; size_t n=serializeJson(d,buf,sizeof(buf));
    g_c_trk->setValue((uint8_t*)buf,n); if (g_ble_connected) g_c_trk->notify();
}
static void notify_history() {
    if (!g_c_stat || !g_sd_ok) return;
    // Stream a compact summary of the last few tracks on the STATUS channel.
    File f = SD.open("/GPS/history/history.json", FILE_READ);
    StaticJsonDocument<2048> out; JsonArray arr = out.createNestedArray("tracks");
    if (f) {
        while (f.available() && arr.size() < 12) {
            String line = f.readStringUntil('\n'); line.trim();
            if (!line.length()) continue;
            StaticJsonDocument<256> row;
            if (deserializeJson(row, line)) continue;
            JsonObject o = arr.createNestedObject();
            o["date"]=row["date"]; o["duration"]=row["duration"]; o["distance"]=row["distance"];
            o["max"]=row["max"]; o["avg"]=row["avg"]; o["points"]=row["points"];
        }
        f.close();
    }
    char buf[2048]; size_t n=serializeJson(out,buf,sizeof(buf));
    g_c_stat->setValue((uint8_t*)buf,n); if (g_ble_connected) g_c_stat->notify();
}

class ServerCallbacks : public NimBLEServerCallbacks {
    void onConnect(NimBLEServer *s, ble_gap_conn_desc *) override {
        g_ble_connected = true; Serial.println("[ble] central connected");
        g_power = PM_ACTIVE;
    }
    void onDisconnect(NimBLEServer *s) override {
        g_ble_connected = false; Serial.println("[ble] central disconnected -- readvertise");
        NimBLEDevice::startAdvertising();   // keep logging even if watch drops
    }
};
static ServerCallbacks g_srv_cbs;

// COMMAND characteristic: first byte = opcode.
class CommandCallbacks : public NimBLECharacteristicCallbacks {
    void onWrite(NimBLECharacteristic *c) override {
        std::string v = c->getValue();
        if (v.empty()) return;
        uint8_t op = (uint8_t)v[0];
        Serial.printf("[ble] <- cmd 0x%02X\n", op);
        switch (op) {
            case XG_GET_LOCATION: l76k_standby(false); notify_location(); break;
            case XG_GET_STATUS:   notify_status();   break;
            case XG_START_TRACK:  track_start();  g_power=PM_TRACKING; notify_tracking(); notify_status(); break;
            case XG_STOP_TRACK:   if (g_always_on) set_always_on(false); else track_stop();
                                  g_power=PM_ACTIVE;   notify_tracking(); notify_status(); break;
            case XG_PAUSE_TRACK:  track_pause();  notify_tracking();   break;
            case XG_GET_HISTORY:  notify_history(); break;
            case XG_SYNC:         notify_status(); notify_tracking(); notify_location(); break;
            case XG_SLEEP:        g_power=PM_STANDBY; l76k_standby(true); notify_status(); break;
            case XG_WAKE:         g_power=PM_ACTIVE;  l76k_standby(false); notify_status(); break;
            default: break;
        }
    }
};
static CommandCallbacks g_cmd_cbs;

// CONFIG characteristic: JSON {"interval":N} / {"duration":N} / {"schedule":N} / {"flight":true}
class ConfigCallbacks : public NimBLECharacteristicCallbacks {
    void onWrite(NimBLECharacteristic *c) override {
        std::string v = c->getValue();
        StaticJsonDocument<128> d;
        if (deserializeJson(d, v)) return;
        Preferences p; p.begin("xgps", false);
        if (d.containsKey("interval")) { g_tk.interval_s = d["interval"]; p.putInt("interval", g_tk.interval_s); }
        if (d.containsKey("duration")) { g_tk.duration_s = d["duration"]; p.putInt("duration", g_tk.duration_s); }
        if (d.containsKey("schedule")) { g_sched_wake_s = d["schedule"]; p.putUInt("sched", g_sched_wake_s); g_power = PM_SCHEDULED; }
        if (d.containsKey("flight"))   { set_flight_mode((bool)d["flight"]); }
        if (d.containsKey("always"))   { p.end(); set_always_on((bool)d["always"]); p.begin("xgps", false); }
        if (d.containsKey("wake_hhmm")) {
            const char *s = d["wake_hhmm"]; int hh=-1, mm=-1;
            if (s && sscanf(s, "%d:%d", &hh, &mm) == 2) schedule_wake_at(hh, mm);
        }
        p.end();
        Serial.printf("[cfg] interval=%d duration=%d sched=%u always=%d\n",
                      g_tk.interval_s, g_tk.duration_s, g_sched_wake_s, g_always_on);
        notify_status();
    }
};
static ConfigCallbacks g_cfg_cbs;

static void ble_begin() {
    NimBLEDevice::init(XG_DEVICE_NAME);
    NimBLEDevice::setPower(ESP_PWR_LVL_P6);
    g_server = NimBLEDevice::createServer();
    g_server->setCallbacks(&g_srv_cbs);
    NimBLEService *svc = g_server->createService(XANDER_GPS_SERVICE_UUID);
    g_c_loc  = svc->createCharacteristic(XG_CHAR_LOCATION_UUID, NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY);
    g_c_stat = svc->createCharacteristic(XG_CHAR_STATUS_UUID,   NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY);
    g_c_batt = svc->createCharacteristic(XG_CHAR_BATTERY_UUID,  NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY);
    g_c_trk  = svc->createCharacteristic(XG_CHAR_TRACKING_UUID, NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY);
    g_c_cmd  = svc->createCharacteristic(XG_CHAR_COMMAND_UUID,  NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR);
    g_c_cfg  = svc->createCharacteristic(XG_CHAR_CONFIG_UUID,   NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR);
    g_c_cmd->setCallbacks(&g_cmd_cbs);
    g_c_cfg->setCallbacks(&g_cfg_cbs);
    svc->start();
    NimBLEAdvertising *adv = NimBLEDevice::getAdvertising();
    adv->addServiceUUID(XANDER_GPS_SERVICE_UUID);
    adv->setName(XG_DEVICE_NAME);
    adv->setScanResponse(true);
    NimBLEDevice::startAdvertising();
    Serial.println("[ble] advertising XANDER_GPS_SERVICE");
}

// =============================================================================
// SECTION 8 -- GNSS FEED PUMP (read UART, feed TinyGPSPlus)
// =============================================================================
static void gps_pump() {
    while (GpsSerial.available()) {
        char ch = GpsSerial.read();
        if (gps.encode(ch)) gps_update_state();
    }
}

// =============================================================================
// SECTION 9 -- SERIAL DEBUG (fix, sats, coords, speed, heading, batt, BLE, ...)
// =============================================================================
static void debug_dump() {
    Serial.printf("[gps] fix=%s kind=%s sat=%d lat=%.6f lon=%.6f alt=%.1f "
                  "spd=%.2fm/s hdg=%d acc=%.1fm batt=%d BLE=%s track=%s always=%s SD=%s flight=%s\n",
        g_st.fix?"Y":"N", g_st.fix_kind, g_st.satellites, g_st.lat, g_st.lon, g_st.alt,
        g_st.speed_mps, g_st.heading, g_st.accuracy, getBattery(),
        g_ble_connected?"conn":"adv", g_tk.active?(g_tk.paused?"pause":"REC"):"off",
        g_always_on?"ON":"off", g_sd_ok?"ok":"no", g_flight_mode?"ON":"off");
}

// =============================================================================
// SECTION 10 -- MAIN
// =============================================================================
static void print_wake_reason() {
    esp_sleep_wakeup_cause_t r = esp_sleep_get_wakeup_cause();
    switch (r) {
        case ESP_SLEEP_WAKEUP_TIMER: Serial.println("[wake] timer"); break;
        case ESP_SLEEP_WAKEUP_EXT0:  Serial.println("[wake] ext0 (watch pulse)"); break;
        default:                     Serial.println("[wake] power-on/reset"); break;
    }
}

void setup() {
    Serial.begin(115200); delay(80);
    Serial.printf("\n[GPSModule] %s fw %s proto v%d\n", XG_DEVICE_NAME, XG_FW_VERSION, XG_PROTOCOL_VERSION);
    print_wake_reason();

    if (USB_SENSE_PIN >= 0) pinMode(USB_SENSE_PIN, INPUT);
    if (BAT_ADC_PIN   >= 0) analogReadResolution(12);

    GpsSerial.begin(GPS_UART_BAUD, SERIAL_8N1, GPS_UART_RX_PIN, GPS_UART_TX_PIN);
    l76k_standby(false);
    storage_begin();

    // restore persisted config
    { Preferences p; p.begin("xgps", true);
      g_tk.interval_s = p.getInt("interval", 5);
      g_tk.duration_s = p.getInt("duration", 0);
      g_sched_wake_s  = p.getUInt("sched", 0);
      g_flight_mode   = p.getBool("flight", false);
      g_always_on     = p.getBool("always", false);
      p.end(); }

    if (g_flight_mode) {
        set_flight_mode(true);   // radios stay off, GNSS+SD logging on
    } else {
        ble_begin();
    }
    if (g_always_on) { g_power = PM_TRACKING; track_start(); }  // resume continuous tracking
    Serial.println("[GPSModule] ready");
}

void loop() {
    gps_pump();
    track_tick();

    // periodic notifications while connected
    static uint32_t last_loc=0, last_stat=0, last_dbg=0;
    uint32_t now = millis();
    if (g_ble_connected && now-last_loc  > 1000) { last_loc  = now; notify_location(); if (g_tk.active) notify_tracking(); }
    if (g_ble_connected && now-last_stat > 3000) { last_stat = now; notify_status(); notify_battery(); }
    if (now-last_dbg > 2000) { last_dbg = now; debug_dump(); }

    // ---- power management ----
    // USB power auto-enables ALWAYS-ON (continuous tracking while plugged in).
    // >>> Requires USB_SENSE_PIN configured; otherwise usb_powered() is always false.
    static bool usb_prev = false;
    bool usb = usb_powered();
    if (usb && !usb_prev) {
        if (!g_always_on) { set_always_on(true); g_always_auto_usb = true; Serial.println("[usb] plugged -> ALWAYS-ON"); }
    } else if (!usb && usb_prev) {
        if (g_always_auto_usb) { set_always_on(false); g_always_auto_usb = false; Serial.println("[usb] unplugged -> ALWAYS-ON off"); }
    }
    usb_prev = usb;

    // ALWAYS-ON: never sleep, keep GNSS + tracking alive until turned off.
    if (g_always_on) {
        l76k_standby(false);
        if (!g_tk.active) track_start();        // auto-restart if something stopped it
    } else if (g_power == PM_SCHEDULED && !g_ble_connected && !g_tk.active && g_sched_wake_s > 0) {
        // no client and nothing to do -> sleep until next scheduled wake
        enter_deep_sleep(g_sched_wake_s);
    }
    // (STANDBY keeps BLE up but stops GNSS via l76k_standby; DEEP_SLEEP entered
    //  only via SCHEDULED here so the watch always configures the schedule first.)

    delay(2);
}
