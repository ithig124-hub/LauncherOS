/* ============================================================================
 *  LinkOS - v0.2.0  (single-file Arduino sketch)
 *
 *  v0.2.0 - Boots straight to a WATCH FACE.  All apps are arranged on an
 *           INFINITE HORIZONTAL CAROUSEL (swipe left / right to cycle,
 *           wraps around at both ends).  Instant + snappy - no slide
 *           animation, no fade.  Inspired by LauncherOS.
 *
 *           Carousel order (wraps both ways):
 *             [0] WatchFace   (big clock + batt + link dot)
 *             [1] Home        (network + vitals + action tiles)
 *             [2] Apps        (ServerOS app launcher)
 *             [3] Mirror      (Pi screen mirror)
 *             [4] Log         (link log)
 *
 *  Waveshare ESP32-S3 Touch AMOLED 2.06  (410 x 502, capacitive touch)
 *
 *  Sibling module for the LauncherOS cyberdeck ecosystem.  Bridges the
 *  watch to a Raspberry Pi Zero 2 W running ServerOS over WiFi.
 *
 *  Drop the compiled `.bin` into:
 *      /WATCH/bin/linkos.bin
 *  on the SD card, then launch from LauncherOS -> Apps -> LinkOS.
 *
 *  Hardware-safety exit: hold BOOT (GPIO 0) for 1.5 s anywhere.
 *
 *  Board settings  (must match the rest of LauncherOS)
 *      ESP32-S3 Dev Module
 *        USB CDC On Boot : Enabled
 *        Flash Size      : 16 MB
 *        Partition Scheme: Minimal SPIFFS (large APP)
 *        PSRAM           : OPI PSRAM
 *        CPU             : 240 MHz (WiFi)
 *
 *  Libraries (Library Manager)
 *      LVGL 8.3 or 8.4
 *      Arduino_GFX_Library
 *      Arduino_DriveBus_Library
 *      XPowersLib
 *      ArduinoJson
 * ============================================================================ */

#include <Arduino.h>
#include <WiFi.h>
#include <ESPmDNS.h>
#include <WiFiUdp.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include <SD_MMC.h>
#include <FS.h>
#include <Update.h>
#include <ArduinoJson.h>
#include <lvgl.h>

// ---- Watch hardware shims --------------------------------------------------
// These are STUBS so LinkOS links and boots immediately.  Replace each body
// with the real Arduino_GFX_Library / Arduino_DriveBus_Library / XPowersLib
// init code from any sibling that already runs on your watch -- the simplest
// source is LauncherOS.ino itself:
//   * watch_display_init()   ->  `gfx->begin()`, `lv_init()`, the disp_drv
//                                setup, and DEFAULT_BRIGHTNESS=140 lines
//                                (see LauncherOS.ino around lines 308-360).
//   * watch_touch_init()     ->  the `bsp_drive_bus` + `lv_indev_drv`
//                                touch driver block.
//   * watch_set_brightness() ->  call `gfx->Display_Brightness(...)`
//                                or `fade_brightness(pct*255/100, 280)`.
//   * watch_battery_pct()    ->  `g_pmu.getBatteryPercent()`.
//   * watch_is_charging()    ->  `g_pmu.isCharging()`.
//
// Until you paste those in, the panel stays dark but every other LinkOS
// feature (WiFi, mDNS, /api/system, /api/apps, Mirror, BOOT-exit) works
// over serial so you can verify the link.
static inline void watch_display_init()             { /* TODO: paste gfx init from LauncherOS.ino */ }
static inline void watch_touch_init()               { /* TODO: paste touch init from LauncherOS.ino */ }
static inline void watch_set_brightness(uint8_t pct){ (void)pct; /* TODO: gfx->Display_Brightness(pct*255/100); */ }
static inline int  watch_battery_pct()              { return 100; /* TODO: return g_pmu.getBatteryPercent(); */ }
static inline bool watch_is_charging()              { return false; /* TODO: return g_pmu.isCharging(); */ }

// ----- Constants -----------------------------------------------------------
static const uint16_t SCREEN_W = 410;
static const uint16_t SCREEN_H = 502;
static const uint16_t UDP_PORT = 5050;
static const uint16_t PI_PORT  = 5000;
static const char*    HOTSPOT_SSID = "ServerOS";
static const char*    HOTSPOT_PASS = "SeRVEROS";
static const char*    HOTSPOT_IP   = "10.42.0.1";

// Hardcoded "home" WiFi - matches LauncherOS.ino HC_WIFI_* exactly so the
// watch can always fall back to the same network the launcher uses.
static const char*    HC_WIFI_SSID = "Optus_9D2E3D";
static const char*    HC_WIFI_PASS = "snucktemptGLeQU";
static const uint8_t  BTN_BOOT       = 0;   // GPIO 0 (the BOOT button on the
                                            // S3 Touch AMOLED 2.06 dev board).
                                            // Don't call this BOOT_PIN - the
                                            // ESP32 core 3.x already defines
                                            // that symbol in esp32-hal.h.
static const uint32_t SAFETY_HOLD_MS = 1500;

// Brand palette (matches LinkOS Master Design)
#define COL_BG       lv_color_hex(0x04050a)
#define COL_CARD     lv_color_hex(0x0d1322)
#define COL_BORDER   lv_color_hex(0x1f2640)
#define COL_ACCENT   lv_color_hex(0x22d3ee)
#define COL_ACC2     lv_color_hex(0xa855f7)
#define COL_ACC3     lv_color_hex(0x6366f1)
#define COL_TXT      lv_color_hex(0xffffff)
#define COL_DIM      lv_color_hex(0x8b95b1)
#define COL_OK       lv_color_hex(0x22c55e)
#define COL_WARN     lv_color_hex(0xf59e0b)
#define COL_BAD      lv_color_hex(0xef4444)

// ----- Globals -------------------------------------------------------------
Preferences nvs;
WiFiUDP udp;
String   pi_ip = "";
String   pi_url_base = "";
uint16_t rtt_ms = 0;
String   pi_mode = "-";   // "lan" | "hotspot"
String   net_label = "OFFLINE";
bool     link_ok = false;

struct Vitals {
    float cpu = 0, ram = 0, temp = 0;
    uint32_t uptime = 0;
    String host = "-";
} vit;

// LVGL screen + key widgets
static lv_obj_t* scr_splash;
static lv_obj_t* scr_watchface;
static lv_obj_t* scr_home;
static lv_obj_t* scr_mirror;
static lv_obj_t* scr_apps;
static lv_obj_t* scr_log;

// ---- Carousel state (infinite horizontal loop) ----
//   index 0 = watch face (boot lands here), then Home, Apps, Mirror, Log.
//   Swipe LEFT  -> next  (wraps to 0)
//   Swipe RIGHT -> prev  (wraps to N-1)
static lv_obj_t* g_carousel[5];           // filled in setup() once all screens built
static const uint8_t CAROUSEL_N = 5;
static int8_t g_idx = 0;                  // currently visible screen index

static lv_obj_t* lbl_wf_clock;            // big watch-face clock
static lv_obj_t* lbl_wf_date;             // watch-face date row
static lv_obj_t* lbl_wf_batt;             // watch-face battery pill
static lv_obj_t* lbl_wf_dot;              // watch-face link dot
static lv_obj_t* lbl_wf_hint;             // "swipe ->" hint at bottom
static lv_obj_t* lbl_wf_pageind;          // page-indicator dots row container

static lv_obj_t* lbl_link_dot;
static lv_obj_t* lbl_clock;
static lv_obj_t* lbl_batt;
static lv_obj_t* lbl_mode_chip;
static lv_obj_t* lbl_ip;
static lv_obj_t* lbl_rtt;
static lv_obj_t* lbl_cpu, *lbl_ram, *lbl_tmp, *lbl_up;
static lv_obj_t* mirror_img;
static lv_img_dsc_t mirror_dsc;
static uint8_t*  mirror_buf = nullptr;
static size_t    mirror_buf_cap = 0;

static char log_lines[20][96];
static uint8_t log_idx = 0;

static void log_push(const char* fmt, ...) {
    va_list a; va_start(a, fmt);
    vsnprintf(log_lines[log_idx % 20], sizeof(log_lines[0]), fmt, a);
    va_end(a);
    log_idx++;
}

// ===== Forward declarations ==================================================
static void build_splash();
static void build_watchface();
static void build_home();
static void build_apps_screen();
static void build_mirror_screen();
static void build_log_screen();
static void carousel_init();
static void carousel_go(int delta);
static void carousel_attach(lv_obj_t* scr);
static void carousel_set(int idx);
static void refresh_apps_list();
static void on_carousel_gesture(lv_event_t* e);
static void connect_wifi_chain();
static bool discover_pi();
static bool probe_pi(const String& host);
static void poll_vitals_task(void*);
static void mirror_task(void*);
static void return_to_launcher();
static void check_boot_button();

// ===== Setup =================================================================
void setup() {
    Serial.begin(115200);
    pinMode(BTN_BOOT, INPUT_PULLUP);

    nvs.begin("linkos", false);

    // Display + LVGL + touch are brought up by the shared LauncherOS shims:
    watch_display_init();
    watch_touch_init();
    watch_set_brightness(80);

    // SD card (matches every other sibling module)
    if (!SD_MMC.begin("/sdcard", true)) {
        Serial.println("[linkos] SD mount failed - continuing without SD");
    } else {
        if (!SD_MMC.exists("/WATCH/link"))         SD_MMC.mkdir("/WATCH/link");
        if (!SD_MMC.exists("/WATCH/link/log"))     SD_MMC.mkdir("/WATCH/link/log");
        if (!SD_MMC.exists("/WATCH/link/cache"))   SD_MMC.mkdir("/WATCH/link/cache");
        if (!SD_MMC.exists("/WATCH/link/notes"))   SD_MMC.mkdir("/WATCH/link/notes");
    }

    build_splash();
    lv_scr_load(scr_splash);

    // Drive the splash for ~1.4 s, then connect + go home
    uint32_t t0 = millis();
    while (millis() - t0 < 1400) {
        lv_timer_handler();
        delay(8);
    }

    connect_wifi_chain();
    if (WiFi.status() == WL_CONNECTED) discover_pi();

    build_home();
    build_apps_screen();
    build_mirror_screen();
    build_log_screen();
    build_watchface();
    carousel_init();
    // Boot lands on the watch face (carousel index 0)
    g_idx = 0;
    lv_scr_load(scr_watchface);

    // Background workers
    xTaskCreatePinnedToCore(poll_vitals_task, "vitals", 4096, nullptr, 1, nullptr, 0);
}

// ===== Loop =================================================================
void loop() {
    lv_timer_handler();
    check_boot_button();
    delay(8);
}

// ===== Splash - "LINK//OS" packet sweep =====================================
static void build_splash() {
    scr_splash = lv_obj_create(nullptr);
    lv_obj_set_style_bg_color(scr_splash, COL_BG, 0);

    // Animated double ring
    lv_obj_t* ring_outer = lv_arc_create(scr_splash);
    lv_obj_set_size(ring_outer, 160, 160);
    lv_obj_center(ring_outer);
    lv_arc_set_bg_angles(ring_outer, 0, 360);
    lv_arc_set_angles(ring_outer, 0, 90);
    lv_obj_set_style_arc_color(ring_outer, COL_BORDER, LV_PART_MAIN);
    lv_obj_set_style_arc_color(ring_outer, COL_ACCENT, LV_PART_INDICATOR);
    lv_obj_set_style_arc_width(ring_outer, 4, LV_PART_MAIN);
    lv_obj_set_style_arc_width(ring_outer, 4, LV_PART_INDICATOR);
    lv_obj_remove_style(ring_outer, nullptr, LV_PART_KNOB);
    lv_obj_clear_flag(ring_outer, LV_OBJ_FLAG_CLICKABLE);

    lv_anim_t a; lv_anim_init(&a);
    lv_anim_set_var(&a, ring_outer);
    lv_anim_set_exec_cb(&a, [](void* obj, int32_t v){ lv_arc_set_angles((lv_obj_t*)obj, v, v + 90); });
    lv_anim_set_time(&a, 1400);
    lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_values(&a, 0, 360);
    lv_anim_start(&a);

    lv_obj_t* core = lv_obj_create(scr_splash);
    lv_obj_set_size(core, 96, 96);
    lv_obj_center(core);
    lv_obj_set_style_radius(core, 20, 0);
    lv_obj_set_style_bg_color(core, COL_ACC3, 0);
    lv_obj_set_style_bg_grad_color(core, COL_ACC2, 0);
    lv_obj_set_style_bg_grad_dir(core, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_border_width(core, 0, 0);
    lv_obj_set_style_shadow_color(core, COL_ACC3, 0);
    lv_obj_set_style_shadow_width(core, 30, 0);
    lv_obj_set_style_shadow_opa(core, LV_OPA_60, 0);
    lv_obj_clear_flag(core, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* glyph = lv_label_create(core);
    lv_label_set_text(glyph, LV_SYMBOL_WIFI);
    lv_obj_set_style_text_color(glyph, COL_TXT, 0);
    lv_obj_set_style_text_font(glyph, &lv_font_montserrat_28, 0);
    lv_obj_center(glyph);

    lv_obj_t* tag = lv_label_create(scr_splash);
    lv_label_set_text(tag, "L I N K  / /  O S");
    lv_obj_set_style_text_color(tag, COL_TXT, 0);
    lv_obj_set_style_text_letter_space(tag, 6, 0);
    lv_obj_align(tag, LV_ALIGN_BOTTOM_MID, 0, -100);

    lv_obj_t* sub = lv_label_create(scr_splash);
    lv_label_set_text(sub, "linking the deck...");
    lv_obj_set_style_text_color(sub, COL_ACCENT, 0);
    lv_obj_set_style_text_font(sub, &lv_font_montserrat_12, 0);
    lv_obj_align(sub, LV_ALIGN_BOTTOM_MID, 0, -72);
}

// ===== WiFi + discovery =====================================================
struct WifiCred { String ssid, pass; };

static std::vector<WifiCred> read_wifi_config() {
    std::vector<WifiCred> out;
    File f = SD_MMC.open("/WATCH/wifi/config.txt", "r");
    if (!f) return out;
    String key, val;
    WifiCred curr;
    while (f.available()) {
        String line = f.readStringUntil('\n');
        line.trim();
        if (line.length() == 0 || line.startsWith("#")) continue;
        int eq = line.indexOf('=');
        if (eq < 0) continue;
        key = line.substring(0, eq); val = line.substring(eq+1);
        key.trim(); val.trim();
        if (key.startsWith("WIFI") && key.endsWith("_SSID")) {
            if (curr.ssid.length()) { out.push_back(curr); curr = WifiCred(); }
            curr.ssid = val;
        } else if (key.startsWith("WIFI") && key.endsWith("_PASS")) {
            curr.pass = val;
        }
    }
    if (curr.ssid.length()) out.push_back(curr);
    f.close();
    return out;
}

static bool try_connect(const String& ssid, const String& pass, uint32_t timeout_ms = 8000) {
    WiFi.mode(WIFI_STA);
    WiFi.begin(ssid.c_str(), pass.c_str());
    uint32_t t0 = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - t0 < timeout_ms) {
        delay(120);
    }
    return WiFi.status() == WL_CONNECTED;
}

static void connect_wifi_chain() {
    // Tier 1 - last known SSID from NVS
    String last = nvs.getString("last_ssid", "");
    String last_pw = nvs.getString("last_pw", "");
    if (last.length() && try_connect(last, last_pw, 5000)) {
        log_push("wifi: NVS %s ok", last.c_str());
        return;
    }
    // Tier 2 - scan /WATCH/wifi/config.txt
    for (auto& c : read_wifi_config()) {
        if (try_connect(c.ssid, c.pass, 7000)) {
            nvs.putString("last_ssid", c.ssid);
            nvs.putString("last_pw",   c.pass);
            log_push("wifi: cfg %s ok", c.ssid.c_str());
            return;
        }
    }
    // Tier 3 - hardcoded "home" WiFi (mirrors LauncherOS HC_WIFI_*)
    if (try_connect(HC_WIFI_SSID, HC_WIFI_PASS, 8000)) {
        nvs.putString("last_ssid", HC_WIFI_SSID);
        nvs.putString("last_pw",   HC_WIFI_PASS);
        log_push("wifi: HC %s ok", HC_WIFI_SSID);
        return;
    }
    // Tier 4 - fallback hotspot
    if (try_connect(HOTSPOT_SSID, HOTSPOT_PASS, 9000)) {
        nvs.putString("last_ssid", HOTSPOT_SSID);
        nvs.putString("last_pw",   HOTSPOT_PASS);
        log_push("wifi: hotspot ok");
        return;
    }
    log_push("wifi: ALL FAILED");
}

static bool probe_pi(const String& host) {
    HTTPClient h;
    String url = "http://" + host + ":" + String(PI_PORT) + "/api/ping";
    h.setConnectTimeout(800);
    h.setTimeout(1200);
    uint32_t t0 = millis();
    if (!h.begin(url)) return false;
    int code = h.GET();
    rtt_ms = millis() - t0;
    h.end();
    if (code == 200) {
        pi_ip = host;
        pi_url_base = "http://" + host + ":" + String(PI_PORT);
        link_ok = true;
        nvs.putString("last_ip", host);
        log_push("pi: %s (%u ms)", host.c_str(), (unsigned)rtt_ms);
        return true;
    }
    return false;
}

static bool discover_pi() {
    // Hotspot shortcut
    String ip = WiFi.localIP().toString();
    if (ip.startsWith("10.42.0.")) {
        net_label = "HOTSPOT"; pi_mode = "hotspot";
        return probe_pi(HOTSPOT_IP);
    }
    net_label = "LAN"; pi_mode = "lan";

    // 1. mDNS
    if (MDNS.begin("linkos")) {
        IPAddress addr = MDNS.queryHost("serveros");
        if (addr) {
            String s = addr.toString();
            if (probe_pi(s)) return true;
        }
    }
    // 2. cached IP
    String cached = nvs.getString("last_ip", "");
    if (cached.length() && probe_pi(cached)) return true;

    // 3. UDP broadcast
    udp.begin(0);
    IPAddress bcast(255,255,255,255);
    udp.beginPacket(bcast, UDP_PORT);
    udp.write((const uint8_t*)"LINKOS?", 7);
    udp.endPacket();
    uint32_t t0 = millis();
    while (millis() - t0 < 1500) {
        int sz = udp.parsePacket();
        if (sz > 0) {
            char buf[64] = {0};
            int n = udp.read((uint8_t*)buf, sizeof(buf)-1);
            (void)n;
            String r(buf);
            if (r.startsWith("LINKOS!:")) {
                int c1 = r.indexOf(':', 8);
                String hip = r.substring(8, c1 > 0 ? c1 : r.length());
                if (probe_pi(hip)) { udp.stop(); return true; }
            }
        }
        delay(20);
    }
    udp.stop();
    link_ok = false;
    return false;
}

// ===== Home screen ===========================================================
static lv_obj_t* card(lv_obj_t* parent, lv_coord_t w, lv_coord_t h) {
    lv_obj_t* c = lv_obj_create(parent);
    lv_obj_set_size(c, w, h);
    lv_obj_set_style_bg_color(c, COL_CARD, 0);
    lv_obj_set_style_border_color(c, COL_BORDER, 0);
    lv_obj_set_style_border_width(c, 1, 0);
    lv_obj_set_style_radius(c, 18, 0);
    lv_obj_set_style_pad_all(c, 10, 0);
    lv_obj_clear_flag(c, LV_OBJ_FLAG_SCROLLABLE);
    return c;
}

static lv_obj_t* tile(lv_obj_t* parent, const char* sym, const char* label, lv_event_cb_t cb) {
    lv_obj_t* t = card(parent, 110, 80);
    lv_obj_t* s = lv_label_create(t);
    lv_label_set_text(s, sym);
    lv_obj_set_style_text_color(s, COL_ACCENT, 0);
    lv_obj_set_style_text_font(s, &lv_font_montserrat_22, 0);
    lv_obj_align(s, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_t* l = lv_label_create(t);
    lv_label_set_text(l, label);
    lv_obj_set_style_text_color(l, COL_TXT, 0);
    lv_obj_set_style_text_font(l, &lv_font_montserrat_12, 0);
    lv_obj_align(l, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_add_event_cb(t, cb, LV_EVENT_CLICKED, nullptr);
    return t;
}

static void on_tile_refresh(lv_event_t*) {
    discover_pi();
}
static void on_tile_mirror (lv_event_t*) { carousel_set(3); }
static void on_tile_apps   (lv_event_t*) { carousel_set(2); refresh_apps_list(); }
static void on_tile_note   (lv_event_t*) {
    if (!link_ok) return;
    HTTPClient h;
    h.begin(pi_url_base + "/api/note");
    h.addHeader("Content-Type", "application/json");
    h.POST("{\"text\":\"ping from watch\"}");
    h.end();
}
static void on_tile_reboot (lv_event_t*) {
    if (!link_ok) return;
    HTTPClient h;
    h.begin(pi_url_base + "/api/power/reboot");
    h.POST("");
    h.end();
}

// --- Network shortcuts (used by the Network card on the home screen) ---
static void on_btn_connect_optus(lv_event_t*) {
    log_push("net: connecting %s", HC_WIFI_SSID);
    WiFi.disconnect(true, true);
    delay(150);
    if (try_connect(HC_WIFI_SSID, HC_WIFI_PASS, 9000)) {
        nvs.putString("last_ssid", HC_WIFI_SSID);
        nvs.putString("last_pw",   HC_WIFI_PASS);
        log_push("net: %s ok", HC_WIFI_SSID);
        discover_pi();
    } else {
        log_push("net: %s FAILED", HC_WIFI_SSID);
    }
}

static void on_btn_connect_hotspot(lv_event_t*) {
    log_push("net: connecting %s", HOTSPOT_SSID);
    WiFi.disconnect(true, true);
    delay(150);
    if (try_connect(HOTSPOT_SSID, HOTSPOT_PASS, 9000)) {
        nvs.putString("last_ssid", HOTSPOT_SSID);
        nvs.putString("last_pw",   HOTSPOT_PASS);
        log_push("net: hotspot ok");
        pi_ip = HOTSPOT_IP;
        probe_pi(HOTSPOT_IP);
    } else {
        log_push("net: hotspot FAILED");
    }
}

static void build_home() {
    scr_home = lv_obj_create(nullptr);
    lv_obj_set_style_bg_color(scr_home, COL_BG, 0);
    lv_obj_set_style_pad_all(scr_home, 12, 0);

    // Top bar
    lv_obj_t* top = lv_obj_create(scr_home);
    lv_obj_set_size(top, SCREEN_W - 24, 36);
    lv_obj_align(top, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_bg_opa(top, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(top, 0, 0);
    lv_obj_clear_flag(top, LV_OBJ_FLAG_SCROLLABLE);

    lbl_batt = lv_label_create(top); lv_label_set_text(lbl_batt, "100%");
    lv_obj_set_style_text_color(lbl_batt, COL_TXT, 0);
    lv_obj_align(lbl_batt, LV_ALIGN_LEFT_MID, 0, 0);

    lbl_clock = lv_label_create(top); lv_label_set_text(lbl_clock, "--:--");
    lv_obj_set_style_text_color(lbl_clock, COL_TXT, 0);
    lv_obj_align(lbl_clock, LV_ALIGN_CENTER, 0, 0);

    lbl_link_dot = lv_label_create(top); lv_label_set_text(lbl_link_dot, LV_SYMBOL_BULLET);
    lv_obj_set_style_text_color(lbl_link_dot, link_ok ? COL_OK : COL_BAD, 0);
    lv_obj_set_style_text_font(lbl_link_dot, &lv_font_montserrat_22, 0);
    lv_obj_align(lbl_link_dot, LV_ALIGN_RIGHT_MID, 0, 0);

    // Mode + IP + RTT card
    lv_obj_t* head = card(scr_home, SCREEN_W - 24, 64);
    lv_obj_align(head, LV_ALIGN_TOP_MID, 0, 44);
    lbl_mode_chip = lv_label_create(head);
    lv_label_set_text_fmt(lbl_mode_chip, "%s", net_label.c_str());
    lv_obj_set_style_text_color(lbl_mode_chip, COL_ACCENT, 0);
    lv_obj_align(lbl_mode_chip, LV_ALIGN_LEFT_MID, 0, 0);
    lbl_ip = lv_label_create(head);
    lv_label_set_text(lbl_ip, pi_ip.length() ? pi_ip.c_str() : "-");
    lv_obj_set_style_text_color(lbl_ip, COL_TXT, 0);
    lv_obj_align(lbl_ip, LV_ALIGN_CENTER, 0, 0);
    lbl_rtt = lv_label_create(head);
    lv_label_set_text_fmt(lbl_rtt, "%u ms", (unsigned)rtt_ms);
    lv_obj_set_style_text_color(lbl_rtt, COL_DIM, 0);
    lv_obj_align(lbl_rtt, LV_ALIGN_RIGHT_MID, 0, 0);

    // Vitals card
    lv_obj_t* v = card(scr_home, SCREEN_W - 24, 72);
    lv_obj_align(v, LV_ALIGN_TOP_MID, 0, 116);
    lbl_cpu = lv_label_create(v); lv_label_set_text(lbl_cpu, "CPU  --%");
    lv_obj_set_style_text_color(lbl_cpu, COL_TXT, 0);
    lv_obj_align(lbl_cpu, LV_ALIGN_TOP_LEFT, 0, 0);
    lbl_ram = lv_label_create(v); lv_label_set_text(lbl_ram, "RAM  --%");
    lv_obj_set_style_text_color(lbl_ram, COL_TXT, 0);
    lv_obj_align(lbl_ram, LV_ALIGN_TOP_RIGHT, 0, 0);
    lbl_tmp = lv_label_create(v); lv_label_set_text(lbl_tmp, "T  -- C");
    lv_obj_set_style_text_color(lbl_tmp, COL_WARN, 0);
    lv_obj_align(lbl_tmp, LV_ALIGN_BOTTOM_LEFT, 0, 0);
    lbl_up  = lv_label_create(v); lv_label_set_text(lbl_up,  "up --");
    lv_obj_set_style_text_color(lbl_up, COL_DIM, 0);
    lv_obj_align(lbl_up, LV_ALIGN_BOTTOM_RIGHT, 0, 0);

    // 2x3 action grid
    lv_obj_t* grid = lv_obj_create(scr_home);
    lv_obj_set_size(grid, SCREEN_W - 24, 184);
    lv_obj_align(grid, LV_ALIGN_TOP_MID, 0, 200);
    lv_obj_set_style_bg_opa(grid, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(grid, 0, 0);
    lv_obj_set_style_pad_all(grid, 0, 0);
    lv_obj_set_layout(grid, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(grid, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_gap(grid, 8, 0);

    tile(grid, LV_SYMBOL_REFRESH, "Refresh", on_tile_refresh);
    tile(grid, LV_SYMBOL_IMAGE,   "Mirror",  on_tile_mirror);
    tile(grid, LV_SYMBOL_LIST,    "Apps",    on_tile_apps);
    tile(grid, LV_SYMBOL_EDIT,    "Note",    on_tile_note);
    tile(grid, LV_SYMBOL_POWER,   "Reboot",  on_tile_reboot);
    tile(grid, LV_SYMBOL_HOME,    "Log",     [](lv_event_t*){ carousel_set(4); });

    // ---- Network card (full-width, two side-by-side connect buttons) -----
    // Replaces the old Back-to-Launcher card.  To go back to LauncherOS,
    // hold the BOOT button (GPIO 0) for 1.5 s - same as every sibling.
    lv_obj_t* netc = lv_obj_create(scr_home);
    lv_obj_set_size(netc, SCREEN_W - 24, 80);
    lv_obj_align(netc, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_color(netc, COL_CARD, 0);
    lv_obj_set_style_border_color(netc, COL_BORDER, 0);
    lv_obj_set_style_border_width(netc, 1, 0);
    lv_obj_set_style_radius(netc, 14, 0);
    lv_obj_set_style_pad_all(netc, 8, 0);
    lv_obj_clear_flag(netc, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* nl = lv_label_create(netc);
    lv_label_set_text(nl, "NETWORK");
    lv_obj_set_style_text_color(nl, COL_DIM, 0);
    lv_obj_set_style_text_font(nl, &lv_font_montserrat_10, 0);
    lv_obj_set_style_text_letter_space(nl, 3, 0);
    lv_obj_align(nl, LV_ALIGN_TOP_LEFT, 4, 0);

    // Left button - connect to hardcoded "Optus" home WiFi
    lv_obj_t* b1 = lv_btn_create(netc);
    lv_obj_set_size(b1, (SCREEN_W - 24 - 24) / 2, 44);
    lv_obj_align(b1, LV_ALIGN_BOTTOM_LEFT, 0, 0);
    lv_obj_set_style_bg_color(b1, COL_ACC3, 0);
    lv_obj_set_style_bg_grad_color(b1, COL_ACC2, 0);
    lv_obj_set_style_bg_grad_dir(b1, LV_GRAD_DIR_HOR, 0);
    lv_obj_set_style_border_width(b1, 0, 0);
    lv_obj_set_style_radius(b1, 10, 0);
    lv_obj_add_event_cb(b1, on_btn_connect_optus, LV_EVENT_CLICKED, nullptr);
    lv_obj_t* b1l = lv_label_create(b1);
    lv_label_set_text(b1l, LV_SYMBOL_WIFI "  Optus");
    lv_obj_set_style_text_color(b1l, COL_TXT, 0);
    lv_obj_set_style_text_font(b1l, &lv_font_montserrat_14, 0);
    lv_obj_center(b1l);

    // Right button - connect to ServerOS hotspot
    lv_obj_t* b2 = lv_btn_create(netc);
    lv_obj_set_size(b2, (SCREEN_W - 24 - 24) / 2, 44);
    lv_obj_align(b2, LV_ALIGN_BOTTOM_RIGHT, 0, 0);
    lv_obj_set_style_bg_color(b2, COL_CARD, 0);
    lv_obj_set_style_border_color(b2, COL_ACCENT, 0);
    lv_obj_set_style_border_width(b2, 2, 0);
    lv_obj_set_style_radius(b2, 10, 0);
    lv_obj_add_event_cb(b2, on_btn_connect_hotspot, LV_EVENT_CLICKED, nullptr);
    lv_obj_t* b2l = lv_label_create(b2);
    lv_label_set_text(b2l, LV_SYMBOL_WIFI "  ServerOS");
    lv_obj_set_style_text_color(b2l, COL_ACCENT, 0);
    lv_obj_set_style_text_font(b2l, &lv_font_montserrat_14, 0);
    lv_obj_center(b2l);
}

// ===== Watch Face (boot screen, carousel index 0) ============================
//
// Big centered clock, small date row, battery pill top-left, link dot
// top-right, and a "swipe ->" hint plus 5-dot page indicator at the bottom.
// Tap anywhere advances the carousel (handy if touch swipe is finicky).
static void build_watchface() {
    scr_watchface = lv_obj_create(nullptr);
    lv_obj_set_style_bg_color(scr_watchface, COL_BG, 0);
    lv_obj_set_style_pad_all(scr_watchface, 0, 0);
    lv_obj_clear_flag(scr_watchface, LV_OBJ_FLAG_SCROLLABLE);

    // Subtle radial accent in the center (sense of depth without animation)
    lv_obj_t* halo = lv_obj_create(scr_watchface);
    lv_obj_set_size(halo, 280, 280);
    lv_obj_center(halo);
    lv_obj_set_style_radius(halo, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(halo, COL_ACC3, 0);
    lv_obj_set_style_bg_opa(halo, LV_OPA_10, 0);
    lv_obj_set_style_border_width(halo, 1, 0);
    lv_obj_set_style_border_color(halo, COL_BORDER, 0);
    lv_obj_clear_flag(halo, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(halo, LV_OBJ_FLAG_SCROLLABLE);

    // Top bar
    lbl_wf_batt = lv_label_create(scr_watchface);
    lv_label_set_text(lbl_wf_batt, "100%");
    lv_obj_set_style_text_color(lbl_wf_batt, COL_TXT, 0);
    lv_obj_set_style_text_font(lbl_wf_batt, &lv_font_montserrat_14, 0);
    lv_obj_align(lbl_wf_batt, LV_ALIGN_TOP_LEFT, 18, 18);

    lbl_wf_dot = lv_label_create(scr_watchface);
    lv_label_set_text(lbl_wf_dot, LV_SYMBOL_BULLET);
    lv_obj_set_style_text_color(lbl_wf_dot, link_ok ? COL_OK : COL_BAD, 0);
    lv_obj_set_style_text_font(lbl_wf_dot, &lv_font_montserrat_28, 0);
    lv_obj_align(lbl_wf_dot, LV_ALIGN_TOP_RIGHT, -18, 6);

    // Big clock
    lbl_wf_clock = lv_label_create(scr_watchface);
    lv_label_set_text(lbl_wf_clock, "--:--");
    lv_obj_set_style_text_color(lbl_wf_clock, COL_TXT, 0);
    lv_obj_set_style_text_font(lbl_wf_clock, &lv_font_montserrat_48, 0);
    lv_obj_set_style_text_letter_space(lbl_wf_clock, 2, 0);
    lv_obj_align(lbl_wf_clock, LV_ALIGN_CENTER, 0, -10);

    lbl_wf_date = lv_label_create(scr_watchface);
    lv_label_set_text(lbl_wf_date, "L I N K  / /  O S");
    lv_obj_set_style_text_color(lbl_wf_date, COL_ACCENT, 0);
    lv_obj_set_style_text_font(lbl_wf_date, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_letter_space(lbl_wf_date, 5, 0);
    lv_obj_align(lbl_wf_date, LV_ALIGN_CENTER, 0, 38);

    // Bottom hint + page indicator
    lbl_wf_hint = lv_label_create(scr_watchface);
    lv_label_set_text(lbl_wf_hint, "swipe  " LV_SYMBOL_RIGHT "  apps");
    lv_obj_set_style_text_color(lbl_wf_hint, COL_DIM, 0);
    lv_obj_set_style_text_font(lbl_wf_hint, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_letter_space(lbl_wf_hint, 3, 0);
    lv_obj_align(lbl_wf_hint, LV_ALIGN_BOTTOM_MID, 0, -22);

    // 5 small dots = page indicator (the active dot is repainted by
    // carousel_go).  The container is a tiny flex row.
    lbl_wf_pageind = lv_obj_create(scr_watchface);
    lv_obj_set_size(lbl_wf_pageind, 100, 14);
    lv_obj_align(lbl_wf_pageind, LV_ALIGN_BOTTOM_MID, 0, -6);
    lv_obj_set_style_bg_opa(lbl_wf_pageind, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(lbl_wf_pageind, 0, 0);
    lv_obj_set_style_pad_all(lbl_wf_pageind, 0, 0);
    lv_obj_set_layout(lbl_wf_pageind, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(lbl_wf_pageind, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(lbl_wf_pageind, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_gap(lbl_wf_pageind, 6, 0);
    lv_obj_clear_flag(lbl_wf_pageind, LV_OBJ_FLAG_SCROLLABLE);
    for (uint8_t i = 0; i < CAROUSEL_N; i++) {
        lv_obj_t* d = lv_obj_create(lbl_wf_pageind);
        lv_obj_set_size(d, 6, 6);
        lv_obj_set_style_radius(d, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_border_width(d, 0, 0);
        lv_obj_set_style_bg_color(d, i == 0 ? COL_ACCENT : COL_BORDER, 0);
        lv_obj_clear_flag(d, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_clear_flag(d, LV_OBJ_FLAG_SCROLLABLE);
    }
}

// ===== Carousel (infinite loop, instant + snappy) ============================
static void update_page_indicator() {
    if (!lbl_wf_pageind) return;
    uint32_t n = lv_obj_get_child_cnt(lbl_wf_pageind);
    for (uint32_t i = 0; i < n; i++) {
        lv_obj_t* d = lv_obj_get_child(lbl_wf_pageind, i);
        lv_obj_set_style_bg_color(d, (int)i == g_idx ? COL_ACCENT : COL_BORDER, 0);
    }
}

static void carousel_go(int delta) {
    // Modulo with proper wrap for negative numbers = infinite loop
    int next = ((int)g_idx + delta) % (int)CAROUSEL_N;
    if (next < 0) next += CAROUSEL_N;
    g_idx = (int8_t)next;
    update_page_indicator();
    // Refresh the apps list when the user lands on it via swipe
    if (g_idx == 2) refresh_apps_list();
    // LV_SCR_LOAD_ANIM_NONE + 0 ms = instant + snappy (no slide)
    lv_scr_load_anim(g_carousel[g_idx], LV_SCR_LOAD_ANIM_NONE, 0, 0, false);
}

// Jump directly to a specific carousel slot (used by Home tap-tiles and the
// "Back to Home" buttons on the inner screens, so swipe stays in sync).
static void carousel_set(int idx) {
    if (idx < 0 || idx >= (int)CAROUSEL_N) return;
    g_idx = (int8_t)idx;
    update_page_indicator();
    lv_scr_load_anim(g_carousel[g_idx], LV_SCR_LOAD_ANIM_NONE, 0, 0, false);
}

static void on_carousel_gesture(lv_event_t* e) {
    lv_indev_t* indev = lv_indev_get_act();
    if (!indev) return;
    lv_dir_t d = lv_indev_get_gesture_dir(indev);
    if (d == LV_DIR_LEFT) {
        // Block LVGL from auto-firing the gesture again on the new screen
        lv_indev_wait_release(indev);
        carousel_go(+1);
    } else if (d == LV_DIR_RIGHT) {
        lv_indev_wait_release(indev);
        carousel_go(-1);
    }
    (void)e;
}

static void carousel_attach(lv_obj_t* scr) {
    // Listen for horizontal swipes on every carousel screen.
    lv_obj_add_event_cb(scr, on_carousel_gesture, LV_EVENT_GESTURE, nullptr);
}

static void carousel_init() {
    g_carousel[0] = scr_watchface;
    g_carousel[1] = scr_home;
    g_carousel[2] = scr_apps;
    g_carousel[3] = scr_mirror;
    g_carousel[4] = scr_log;
    for (uint8_t i = 0; i < CAROUSEL_N; i++) carousel_attach(g_carousel[i]);
    update_page_indicator();
}

// ===== Apps list screen ======================================================
static lv_obj_t* apps_list_obj;
static void build_apps_screen() {
    scr_apps = lv_obj_create(nullptr);
    lv_obj_set_style_bg_color(scr_apps, COL_BG, 0);

    lv_obj_t* hdr = lv_label_create(scr_apps);
    lv_label_set_text(hdr, "ServerOS Apps");
    lv_obj_set_style_text_color(hdr, COL_TXT, 0);
    lv_obj_set_style_text_font(hdr, &lv_font_montserrat_22, 0);
    lv_obj_align(hdr, LV_ALIGN_TOP_MID, 0, 16);

    apps_list_obj = lv_list_create(scr_apps);
    lv_obj_set_size(apps_list_obj, SCREEN_W - 24, SCREEN_H - 130);
    lv_obj_align(apps_list_obj, LV_ALIGN_TOP_MID, 0, 56);
    lv_obj_set_style_bg_color(apps_list_obj, COL_CARD, 0);
    lv_obj_set_style_border_color(apps_list_obj, COL_BORDER, 0);
    lv_obj_set_style_radius(apps_list_obj, 16, 0);

    lv_obj_t* back = lv_btn_create(scr_apps);
    lv_obj_set_size(back, SCREEN_W - 24, 48);
    lv_obj_align(back, LV_ALIGN_BOTTOM_MID, 0, -12);
    lv_obj_set_style_bg_color(back, COL_CARD, 0);
    lv_obj_set_style_border_color(back, COL_ACCENT, 0);
    lv_obj_set_style_border_width(back, 2, 0);
    lv_obj_t* bl = lv_label_create(back);
    lv_label_set_text(bl, LV_SYMBOL_LEFT "  Home");
    lv_obj_center(bl);
    lv_obj_add_event_cb(back, [](lv_event_t*){ carousel_set(1); }, LV_EVENT_CLICKED, nullptr);
}

static void refresh_apps_list() {
    lv_obj_clean(apps_list_obj);
    if (!link_ok) {
        lv_list_add_text(apps_list_obj, "offline - no link");
        return;
    }
    HTTPClient h;
    h.begin(pi_url_base + "/api/apps");
    int code = h.GET();
    if (code == 200) {
        String body = h.getString();
        JsonDocument doc;
        if (!deserializeJson(doc, body)) {
            for (JsonObject a : doc.as<JsonArray>()) {
                const char* name = a["name"] | "?";
                const char* slug = a["slug"] | a["id"] | "";
                char* slug_dup = strdup(slug);
                lv_obj_t* it = lv_list_add_btn(apps_list_obj, LV_SYMBOL_PLAY, name);
                lv_obj_add_event_cb(it, [](lv_event_t* e){
                    const char* s = (const char*) lv_event_get_user_data(e);
                    HTTPClient hh;
                    hh.begin(pi_url_base + "/api/run/" + String(s));
                    hh.addHeader("Content-Type", "application/json");
                    hh.POST("{}");
                    hh.end();
                }, LV_EVENT_CLICKED, slug_dup);
            }
        }
    }
    h.end();
}

// ===== Mirror screen =========================================================
static void on_mirror_tap(lv_event_t* e) {
    if (!link_ok) return;
    lv_indev_t* indev = lv_indev_get_act();
    lv_point_t p; lv_indev_get_point(indev, &p);
    char body[64];
    snprintf(body, sizeof(body), "{\"x\":%d,\"y\":%d}", p.x, p.y);
    HTTPClient h;
    h.begin(pi_url_base + "/mirror/click");
    h.addHeader("Content-Type", "application/json");
    h.POST(body);
    h.end();
}

static void build_mirror_screen() {
    scr_mirror = lv_obj_create(nullptr);
    lv_obj_set_style_bg_color(scr_mirror, COL_BG, 0);
    lv_obj_set_style_pad_all(scr_mirror, 0, 0);

    // Pre-allocate a 60 KB JPEG buffer (LVGL will decode in place)
    mirror_buf_cap = 64 * 1024;
    mirror_buf = (uint8_t*) heap_caps_malloc(mirror_buf_cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);

    mirror_img = lv_img_create(scr_mirror);
    lv_obj_set_size(mirror_img, SCREEN_W, SCREEN_H - 50);
    lv_obj_align(mirror_img, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_add_flag(mirror_img, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(mirror_img, on_mirror_tap, LV_EVENT_CLICKED, nullptr);

    lv_obj_t* back = lv_btn_create(scr_mirror);
    lv_obj_set_size(back, SCREEN_W, 50);
    lv_obj_align(back, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_color(back, COL_CARD, 0);
    lv_obj_set_style_border_color(back, COL_ACCENT, 0);
    lv_obj_set_style_border_width(back, 2, 0);
    lv_obj_t* bl = lv_label_create(back);
    lv_label_set_text(bl, LV_SYMBOL_LEFT "  Home   |   tap to click");
    lv_obj_center(bl);
    lv_obj_add_event_cb(back, [](lv_event_t*){ carousel_set(1); }, LV_EVENT_CLICKED, nullptr);

    xTaskCreatePinnedToCore(mirror_task, "mirror", 8192, nullptr, 1, nullptr, 0);
}

static void mirror_task(void*) {
    while (true) {
        if (!link_ok || lv_scr_act() != scr_mirror) { vTaskDelay(pdMS_TO_TICKS(400)); continue; }
        int fps = (watch_battery_pct() >= 50) ? 4 : (watch_battery_pct() >= 20 ? 2 : 1);
        HTTPClient h;
        h.begin(pi_url_base + "/mirror/frame?q=55");
        h.setTimeout(2500);
        int code = h.GET();
        if (code == 200) {
            int len = h.getSize();
            if (len > 0 && (size_t)len <= mirror_buf_cap) {
                WiFiClient* stream = h.getStreamPtr();
                int got = 0;
                while (h.connected() && got < len) {
                    int n = stream->readBytes(mirror_buf + got, len - got);
                    if (n <= 0) break; got += n;
                }
                if (got == len) {
                    mirror_dsc.data = mirror_buf;
                    mirror_dsc.data_size = got;
                    mirror_dsc.header.always_zero = 0;
                    mirror_dsc.header.w = SCREEN_W;
                    mirror_dsc.header.h = SCREEN_H - 50;
                    mirror_dsc.header.cf = LV_IMG_CF_RAW;
                    lv_img_set_src(mirror_img, &mirror_dsc);
                    lv_obj_invalidate(mirror_img);
                }
            }
        }
        h.end();
        vTaskDelay(pdMS_TO_TICKS(1000 / fps));
    }
}

// ===== Log screen ============================================================
static lv_obj_t* log_label;
static void build_log_screen() {
    scr_log = lv_obj_create(nullptr);
    lv_obj_set_style_bg_color(scr_log, COL_BG, 0);

    lv_obj_t* hdr = lv_label_create(scr_log);
    lv_label_set_text(hdr, "Link Log");
    lv_obj_set_style_text_color(hdr, COL_TXT, 0);
    lv_obj_set_style_text_font(hdr, &lv_font_montserrat_22, 0);
    lv_obj_align(hdr, LV_ALIGN_TOP_MID, 0, 14);

    lv_obj_t* box = card(scr_log, SCREEN_W - 24, SCREEN_H - 140);
    lv_obj_align(box, LV_ALIGN_TOP_MID, 0, 50);
    log_label = lv_label_create(box);
    lv_label_set_long_mode(log_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(log_label, SCREEN_W - 50);
    lv_obj_set_style_text_color(log_label, COL_TXT, 0);
    lv_obj_set_style_text_font(log_label, &lv_font_montserrat_12, 0);
    lv_label_set_text(log_label, "");

    lv_obj_t* back = lv_btn_create(scr_log);
    lv_obj_set_size(back, SCREEN_W - 24, 48);
    lv_obj_align(back, LV_ALIGN_BOTTOM_MID, 0, -12);
    lv_obj_set_style_bg_color(back, COL_CARD, 0);
    lv_obj_set_style_border_color(back, COL_ACCENT, 0);
    lv_obj_set_style_border_width(back, 2, 0);
    lv_obj_t* bl = lv_label_create(back);
    lv_label_set_text(bl, LV_SYMBOL_LEFT "  Home");
    lv_obj_center(bl);
    lv_obj_add_event_cb(back, [](lv_event_t*){ carousel_set(1); }, LV_EVENT_CLICKED, nullptr);
}

static void render_log() {
    if (!log_label) return;
    String s;
    for (int i = 0; i < 20; i++) {
        int idx = (log_idx - 1 - i + 200) % 20;
        if (log_lines[idx][0]) { s += log_lines[idx]; s += "\n"; }
    }
    lv_label_set_text(log_label, s.c_str());
}

// ===== Vitals task ===========================================================
static void poll_vitals_task(void*) {
    while (true) {
        if (link_ok && WiFi.status() == WL_CONNECTED) {
            HTTPClient h;
            h.begin(pi_url_base + "/api/system");
            h.setTimeout(1500);
            int code = h.GET();
            if (code == 200) {
                JsonDocument doc;
                if (!deserializeJson(doc, h.getString())) {
                    vit.cpu    = doc["cpu"]    | 0.0f;
                    vit.ram    = doc["ram"]    | 0.0f;
                    vit.temp   = doc["temp"]   | 0.0f;
                    vit.uptime = doc["uptime"] | 0;
                    pi_mode    = (const char*)(doc["mode"] | "lan");
                    pi_ip      = (const char*)(doc["ip"]   | pi_ip.c_str());
                    net_label  = pi_mode == "hotspot" ? "HOTSPOT" : "LAN";
                }
            } else {
                link_ok = false;
            }
            h.end();
        } else if (WiFi.status() == WL_CONNECTED) {
            discover_pi();
        }

        // Refresh UI labels
        if (lbl_link_dot) lv_obj_set_style_text_color(lbl_link_dot, link_ok ? COL_OK : COL_BAD, 0);
        if (lbl_wf_dot)   lv_obj_set_style_text_color(lbl_wf_dot,   link_ok ? COL_OK : COL_BAD, 0);
        if (lbl_batt)     lv_label_set_text_fmt(lbl_batt, "%d%%%s", watch_battery_pct(), watch_is_charging() ? "+" : "");
        if (lbl_wf_batt)  lv_label_set_text_fmt(lbl_wf_batt, "%d%%%s", watch_battery_pct(), watch_is_charging() ? "+" : "");
        if (lbl_mode_chip) lv_label_set_text(lbl_mode_chip, net_label.c_str());
        if (lbl_ip)       lv_label_set_text(lbl_ip, pi_ip.length() ? pi_ip.c_str() : "-");
        if (lbl_rtt)      lv_label_set_text_fmt(lbl_rtt, "%u ms", (unsigned)rtt_ms);
        if (lbl_cpu)      lv_label_set_text_fmt(lbl_cpu, "CPU  %d%%", (int)vit.cpu);
        if (lbl_ram)      lv_label_set_text_fmt(lbl_ram, "RAM  %d%%", (int)vit.ram);
        if (lbl_tmp)      lv_label_set_text_fmt(lbl_tmp, "T  %d C",   (int)vit.temp);
        if (lbl_up) {
            uint32_t u = vit.uptime;
            lv_label_set_text_fmt(lbl_up, "up %luh%02lum", (unsigned long)(u/3600), (unsigned long)((u%3600)/60));
        }
        if (lbl_clock) {
            time_t now = time(nullptr);
            struct tm tm; localtime_r(&now, &tm);
            lv_label_set_text_fmt(lbl_clock, "%02d:%02d", tm.tm_hour, tm.tm_min);
        }
        if (lbl_wf_clock) {
            time_t now = time(nullptr);
            struct tm tm; localtime_r(&now, &tm);
            lv_label_set_text_fmt(lbl_wf_clock, "%02d:%02d", tm.tm_hour, tm.tm_min);
        }
        if (lbl_wf_date) {
            time_t now = time(nullptr);
            struct tm tm; localtime_r(&now, &tm);
            // Only overwrite the LINK//OS branding once the clock has a real
            // time (year >= 2024 after NTP sync) - otherwise keep the tag.
            if (tm.tm_year + 1900 >= 2024) {
                static const char* dow[] = {"SUN","MON","TUE","WED","THU","FRI","SAT"};
                static const char* mon[] = {"JAN","FEB","MAR","APR","MAY","JUN",
                                            "JUL","AUG","SEP","OCT","NOV","DEC"};
                lv_label_set_text_fmt(lbl_wf_date, "%s  %02d %s",
                                      dow[tm.tm_wday % 7], tm.tm_mday,
                                      mon[tm.tm_mon % 12]);
            }
        }
        render_log();
        vTaskDelay(pdMS_TO_TICKS(2000));
    }
}

// ===== Back-to-Launcher ======================================================
static void show_progress(lv_obj_t* parent, lv_obj_t** bar_out) {
    lv_obj_t* ov = lv_obj_create(parent);
    lv_obj_set_size(ov, SCREEN_W, SCREEN_H);
    lv_obj_align(ov, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(ov, COL_BG, 0);
    lv_obj_set_style_bg_opa(ov, LV_OPA_90, 0);
    lv_obj_t* lbl = lv_label_create(ov);
    lv_label_set_text(lbl, "Returning to LauncherOS...");
    lv_obj_set_style_text_color(lbl, COL_TXT, 0);
    lv_obj_align(lbl, LV_ALIGN_CENTER, 0, -30);
    lv_obj_t* bar = lv_bar_create(ov);
    lv_obj_set_size(bar, 280, 8);
    lv_obj_align(bar, LV_ALIGN_CENTER, 0, 10);
    lv_obj_set_style_bg_color(bar, COL_BORDER, 0);
    lv_obj_set_style_bg_color(bar, COL_ACCENT, LV_PART_INDICATOR);
    lv_bar_set_range(bar, 0, 100);
    *bar_out = bar;
}

static void return_to_launcher() {
    if (!SD_MMC.exists("/WATCH/bin/launcheros.bin")) {
        // OTA-partition flip fallback
        esp_restart();
        return;
    }
    File f = SD_MMC.open("/WATCH/bin/launcheros.bin", "r");
    if (!f || f.size() < 0x10000) { esp_restart(); return; }
    size_t total = f.size();

    lv_obj_t* bar = nullptr;
    show_progress(lv_scr_act(), &bar);
    lv_timer_handler();

    if (!Update.begin(total)) { f.close(); esp_restart(); return; }

    uint8_t buf[4096];
    size_t  written = 0;
    while (f.available()) {
        int n = f.read(buf, sizeof(buf));
        if (n <= 0) break;
        Update.write(buf, n);
        written += n;
        if (bar) lv_bar_set_value(bar, (int)(written * 100 / total), LV_ANIM_OFF);
        lv_timer_handler();
    }
    f.close();
    if (Update.end(true)) {
        delay(250);
        esp_restart();
    } else {
        esp_restart();
    }
}

// ===== BOOT-button safety exit ==============================================
static void check_boot_button() {
    static uint32_t pressed_since = 0;
    if (digitalRead(BTN_BOOT) == LOW) {
        if (pressed_since == 0) pressed_since = millis();
        if (millis() - pressed_since >= SAFETY_HOLD_MS) {
            return_to_launcher();
        }
    } else {
        pressed_since = 0;
    }
}
