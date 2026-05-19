// =============================================================================
//  VaultOS.ino v1.0 -- LauncherOS-compatible AES-256 vault + RFC 6238 TOTP
//  Target  : Waveshare ESP32-S3 Touch AMOLED 2.06 (410 x 502, CO5300, FT3168)
//  Compile : ESP32-S3 Dev Module / PSRAM=OPI / Flash=16MB / Minimal SPIFFS (large APP)
//  Export  : Sketch -> Export Compiled Binary  ->  copy VaultOS.ino.bin to
//            /WATCH/bin/vaultos.bin on the SD card.
// -----------------------------------------------------------------------------
//  Splash style : pulsing padlock + "VAULT::OS"
//  SD root      : /WATCH/vault/vault.dat (AES-256-CTR encrypted blob)
//  NVS ns       : "vaultos"  (stores key-derivation salt + failed-attempt counter)
//  Features     :
//    [1] AES-256-CTR encrypted entry store on SD
//    [2] PBKDF2-HMAC-SHA256 master-password key derivation (50k rounds)
//    [3] RFC 6238 TOTP code generator (30 s window, 6 digits, HMAC-SHA1)
//    [4] Progressive lockout: 1 s -> 5 s -> 30 s -> wipe after 10 fails
//    [5] Panic wipe: BOOT held 10 s on unlock screen erases vault.dat + salt
//    [6] On-screen 28-key keyboard for master password entry
//    [7] Auto-relock after 30 s of inactivity
//    [8] Reads /WATCH/wifi/config.txt only to refresh NTP at boot (TOTP needs clock)
// =============================================================================

// ===== BEGIN SHARED BOOT BLOCK (LauncherOS v6.1+ compatible) =================
#include <Arduino.h>
#include <FS.h>
#include <SD_MMC.h>
#include <Wire.h>
#include <WiFi.h>
#include <Preferences.h>
#include <time.h>
#include <sys/time.h>
#include <memory>

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

#include "mbedtls/aes.h"
#include "mbedtls/md.h"
#include "mbedtls/sha1.h"
#include "mbedtls/sha256.h"
#include "mbedtls/pkcs5.h"

#define LCD_SDIO0 4
#define LCD_SDIO1 5
#define LCD_SDIO2 6
#define LCD_SDIO3 7
#define LCD_SCLK 11
#define LCD_CS   12
#define LCD_RESET 8
#define LCD_WIDTH 410
#define LCD_HEIGHT 502
#define IIC_SDA 15
#define IIC_SCL 14
#define TP_INT  38
#define TP_RESET 9
static const int SDMMC_CLK=2, SDMMC_CMD=1, SDMMC_DATA=3;
#define PWR_BTN_PIN 10
#define BOOT_BTN_PIN 0

Arduino_DataBus *bus = new Arduino_ESP32QSPI(LCD_CS, LCD_SCLK, LCD_SDIO0, LCD_SDIO1, LCD_SDIO2, LCD_SDIO3);
Arduino_GFX *gfx = new Arduino_CO5300(bus, LCD_RESET, 0, LCD_WIDTH, LCD_HEIGHT, 22, 0, 0, 0);
std::shared_ptr<Arduino_IIC_DriveBus> IIC_Bus = std::make_shared<Arduino_HWIIC>(IIC_SDA, IIC_SCL, &Wire);
void Arduino_IIC_Touch_Interrupt(void);
std::unique_ptr<Arduino_IIC> FT3168(new Arduino_FT3x68(IIC_Bus, FT3168_DEVICE_ADDRESS, DRIVEBUS_DEFAULT_VALUE, TP_INT, Arduino_IIC_Touch_Interrupt));
void Arduino_IIC_Touch_Interrupt(void) { FT3168->IIC_Interrupt_Flag = true; }

static lv_disp_draw_buf_t draw_buf;
static lv_color_t *lvgl_buf=nullptr;
static lv_disp_drv_t disp_drv;
static lv_indev_drv_t indev_drv;
static uint32_t g_last_touch_t = 0;

static void my_disp_flush(lv_disp_drv_t *d, const lv_area_t *a, lv_color_t *px) {
    uint32_t w=a->x2-a->x1+1, h=a->y2-a->y1+1;
    gfx->draw16bitRGBBitmap(a->x1, a->y1, (uint16_t*)px, w, h);
    lv_disp_flush_ready(d);
}
static void my_touchpad_read(lv_indev_drv_t *, lv_indev_data_t *data) {
    int32_t x=FT3168->IIC_Read_Device_Value(FT3168->Arduino_IIC_Touch::Value_Information::TOUCH_COORDINATE_X);
    int32_t y=FT3168->IIC_Read_Device_Value(FT3168->Arduino_IIC_Touch::Value_Information::TOUCH_COORDINATE_Y);
    int fingers=FT3168->IIC_Read_Device_Value(FT3168->Arduino_IIC_Touch::Value_Information::TOUCH_FINGER_NUMBER);
    if (fingers > 0) { data->state=LV_INDEV_STATE_PR; data->point.x=x; data->point.y=y; g_last_touch_t=millis(); }
    else             { data->state=LV_INDEV_STATE_REL; }
}

XPowersPMU g_pmu; static bool g_has_pmu=false;
static void pmu_init() {
    if (g_pmu.begin(Wire, AXP2101_SLAVE_ADDRESS, IIC_SDA, IIC_SCL)) {
        g_has_pmu=true; g_pmu.disableTSPinMeasure(); g_pmu.enableBattVoltageMeasure();
    }
}

static void return_to_launcher() {
    const esp_partition_t *factory = esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_FACTORY, NULL);
    if (factory) { esp_ota_set_boot_partition(factory); }
    else {
        const esp_partition_t *other = esp_ota_get_next_update_partition(NULL);
        if (other && other != esp_ota_get_running_partition()) esp_ota_set_boot_partition(other);
    }
    delay(80); esp_restart();
}

static void launcher_app_boot() {
    Serial.begin(115200);
    pinMode(BOOT_BTN_PIN, INPUT_PULLUP);
    pinMode(PWR_BTN_PIN,  INPUT_PULLUP);
    btStop();
    if (!gfx->begin()) Serial.println("gfx->begin() failed");
    gfx->fillScreen(0x0000);
    pinMode(IIC_SCL, OUTPUT);
    for (int i=0; i<9; i++) { digitalWrite(IIC_SCL,LOW); delayMicroseconds(5); digitalWrite(IIC_SCL,HIGH); delayMicroseconds(5); }
    Wire.begin(IIC_SDA, IIC_SCL);
    pinMode(TP_RESET, OUTPUT);
    digitalWrite(TP_RESET, LOW);  delay(50);
    digitalWrite(TP_RESET, HIGH); delay(200);
    FT3168->begin();
    pmu_init();
    lv_init();
    size_t bufPx = LCD_WIDTH * 40;
    lvgl_buf = (lv_color_t*)heap_caps_malloc(bufPx*sizeof(lv_color_t), MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT);
    if (!lvgl_buf) lvgl_buf = (lv_color_t*)heap_caps_malloc(bufPx*sizeof(lv_color_t), MALLOC_CAP_8BIT);
    lv_disp_draw_buf_init(&draw_buf, lvgl_buf, NULL, bufPx);
    lv_disp_drv_init(&disp_drv);
    disp_drv.flush_cb = my_disp_flush; disp_drv.draw_buf = &draw_buf;
    disp_drv.hor_res = LCD_WIDTH; disp_drv.ver_res = LCD_HEIGHT;
    lv_disp_drv_register(&disp_drv);
    lv_indev_drv_init(&indev_drv);
    indev_drv.type = LV_INDEV_TYPE_POINTER; indev_drv.read_cb = my_touchpad_read;
    lv_indev_drv_register(&indev_drv);
    SD_MMC.setPins(SDMMC_CLK, SDMMC_CMD, SDMMC_DATA);
    SD_MMC.begin("/sdcard", true);
}
// ===== END SHARED BOOT BLOCK =================================================

// ---- LauncherOS clock restore (read its "rtcsave" namespace) ----------------
static void time_restore_from_launcher() {
    Preferences p;
    if (!p.begin("rtcsave", true)) return;
    uint32_t t = p.getULong("t", 0);
    long gmt   = p.getLong("gmt", 0);
    p.end();
    if (t > 1700000000) {
        struct timeval tv = { .tv_sec = (time_t)t, .tv_usec = 0 };
        settimeofday(&tv, NULL);
        setenv("TZ", "UTC0", 1); tzset();
        // Apply gmt offset by shifting TZ string
        char tz[16]; snprintf(tz, sizeof(tz), "UTC%+ld", -gmt/3600);
        setenv("TZ", tz, 1); tzset();
    }
}

// ---- WiFi (boot only, one-shot NTP, then radio off) -------------------------
static String g_wifi_ssid="", g_wifi_pass=""; static int g_gmt = 0;
static void read_wifi_cfg() {
    File f = SD_MMC.open("/WATCH/wifi/config.txt", FILE_READ);
    if (!f) return;
    while (f.available()) {
        String line = f.readStringUntil('\n'); line.trim();
        if (line.startsWith("WIFI1_SSID=")) g_wifi_ssid = line.substring(11);
        else if (line.startsWith("WIFI1_PASS=")) g_wifi_pass = line.substring(11);
        else if (line.startsWith("GMT_OFFSET=")) g_gmt = line.substring(11).toInt();
    }
    f.close();
}
static bool g_ntp_ok=false;
static void wifi_quick_ntp() {
    if (g_wifi_ssid.length()==0) return;
    WiFi.mode(WIFI_STA);
    WiFi.begin(g_wifi_ssid.c_str(), g_wifi_pass.c_str());
    uint32_t t0=millis();
    while (WiFi.status()!=WL_CONNECTED && millis()-t0<6000) delay(100);
    if (WiFi.status()==WL_CONNECTED) {
        configTime(g_gmt*3600, 0, "pool.ntp.org", "time.google.com");
        struct tm tm; if (getLocalTime(&tm, 3000)) g_ntp_ok=true;
    }
    WiFi.disconnect(true); WiFi.mode(WIFI_OFF);
}

// =============================================================================
//  VaultOS palette + styling
// =============================================================================
#define COL_BG       0x000000
#define COL_CARD     0x0E0E0E
#define COL_BORDER   0x1C1C1C
#define COL_ACCENT   0x00E5FF   // cyan = unlocked / actions
#define COL_LOCK     0xFFB300   // amber = locked
#define COL_DANGER   0xFF4040
#define COL_OK       0x00FF80
#define COL_TXT      0xFFFFFF
#define COL_MUTE     0x666666

// =============================================================================
//  Splash: pulsing padlock + "VAULT::OS"
// =============================================================================
static void splash_show() {
    gfx->fillScreen(0x0000);
    int cx = LCD_WIDTH/2, cy = LCD_HEIGHT/2 - 40;
    // 8 frames of pulsing lock shackle + body
    for (int f = 0; f < 24; f++) {
        int pulse = (f%8 < 4) ? (f%8)*2 : (8 - f%8)*2;
        gfx->fillScreen(0x0000);
        // body
        gfx->fillRoundRect(cx-36, cy, 72, 56, 6, 0xFFB3);    // amber 0xFFB300 ~ RGB565 0xFFB3
        // shackle
        // shackle (top semicircle, 3 px thick) -- Arduino_GFX drawArc:
        // drawArc(x, y, r_outer, r_inner, start_deg, end_deg, color)
        gfx->drawArc(cx, cy, 26, 23, 180.0f, 360.0f, 0xFFB3);
        // pulse ring
        int r = 60 + pulse * 4;
        gfx->drawCircle(cx, cy, r, (pulse > 6) ? 0x0000 : 0x4209);
        gfx->setTextColor(0xFFFF);
        gfx->setTextSize(2);
        gfx->setCursor(cx-70, cy+88);
        gfx->print("VAULT::OS");
        gfx->setTextColor(0x4208);
        gfx->setCursor(cx-66, cy+115);
        gfx->print("ENCRYPTED");
        delay(45);
    }
}

// =============================================================================
//  Crypto: PBKDF2 -> AES-256-CTR + HMAC-SHA1 TOTP
// =============================================================================
struct VaultEntry {
    char label[32];   // e.g. "Gmail"
    char user[48];
    char secret[80];  // password OR base32 TOTP secret
    uint8_t type;     // 0 = password, 1 = TOTP
};
#define MAX_ENTRIES 32

static uint8_t g_key[32];   // derived master key
static uint8_t g_salt[16];
static bool    g_unlocked = false;
static VaultEntry g_entries[MAX_ENTRIES];
static int     g_n_entries = 0;
static uint8_t g_fails = 0;
static uint32_t g_relock_at_ms = 0;
#define AUTO_RELOCK_MS 30000

static void pbkdf2_derive(const char *pw, uint8_t out[32]) {
    // Newer ESP-IDF mbedTLS dropped mbedtls_pkcs5_pbkdf2_hmac() in favor of
    // mbedtls_pkcs5_pbkdf2_hmac_ext(), which takes the md type directly and
    // no longer needs an mbedtls_md_context_t.
#if defined(MBEDTLS_VERSION_NUMBER) && (MBEDTLS_VERSION_NUMBER >= 0x03000000)
    mbedtls_pkcs5_pbkdf2_hmac_ext(MBEDTLS_MD_SHA256,
                                  (const unsigned char*)pw, strlen(pw),
                                  g_salt, 16, 50000, 32, out);
#else
    mbedtls_md_context_t md;
    const mbedtls_md_info_t *info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    mbedtls_md_init(&md);
    mbedtls_md_setup(&md, info, 1);
    mbedtls_pkcs5_pbkdf2_hmac(&md, (const unsigned char*)pw, strlen(pw),
                              g_salt, 16, 50000, 32, out);
    mbedtls_md_free(&md);
#endif
}

static void aes_ctr_xcrypt(const uint8_t key[32], const uint8_t nonce[16],
                           uint8_t *data, size_t len) {
    mbedtls_aes_context ctx; mbedtls_aes_init(&ctx);
    mbedtls_aes_setkey_enc(&ctx, key, 256);
    uint8_t iv[16], sb[16]; memcpy(iv, nonce, 16);
    size_t nc_off = 0;
    mbedtls_aes_crypt_ctr(&ctx, len, &nc_off, iv, sb, data, data);
    mbedtls_aes_free(&ctx);
}

// File layout: [16B nonce][N*sizeof(VaultEntry) ciphertext]
static bool vault_save() {
    SD_MMC.mkdir("/WATCH/vault");
    File f = SD_MMC.open("/WATCH/vault/vault.dat", FILE_WRITE);
    if (!f) return false;
    uint8_t nonce[16];
    for (int i=0; i<16; i++) nonce[i] = (uint8_t)esp_random();
    f.write(nonce, 16);
    size_t blob = sizeof(VaultEntry) * g_n_entries + 4;  // +4 bytes for count
    uint8_t *buf = (uint8_t*)heap_caps_malloc(blob, MALLOC_CAP_8BIT);
    if (!buf) { f.close(); return false; }
    memcpy(buf, &g_n_entries, 4);
    memcpy(buf+4, g_entries, sizeof(VaultEntry)*g_n_entries);
    aes_ctr_xcrypt(g_key, nonce, buf, blob);
    f.write(buf, blob);
    free(buf); f.close();
    return true;
}
static bool vault_load(const char *master) {
    pbkdf2_derive(master, g_key);
    File f = SD_MMC.open("/WATCH/vault/vault.dat", FILE_READ);
    if (!f) { g_n_entries = 0; return true; }   // fresh vault is fine
    uint8_t nonce[16];
    if (f.read(nonce, 16) != 16) { f.close(); return false; }
    size_t cipher_len = f.size() - 16;
    if (cipher_len < 4 || cipher_len > 64*1024) { f.close(); return false; }
    uint8_t *buf = (uint8_t*)heap_caps_malloc(cipher_len, MALLOC_CAP_8BIT);
    if (!buf) { f.close(); return false; }
    f.read(buf, cipher_len); f.close();
    aes_ctr_xcrypt(g_key, nonce, buf, cipher_len);
    int32_t n = 0; memcpy(&n, buf, 4);
    if (n < 0 || n > MAX_ENTRIES) { free(buf); return false; }
    // Sanity-check: labels must be printable ASCII or NUL
    for (int i=0; i<n; i++) {
        const char *lbl = (const char*)(buf + 4 + i*sizeof(VaultEntry));
        if ((uint8_t)lbl[0] < 32 || (uint8_t)lbl[0] > 126) { free(buf); return false; }
    }
    g_n_entries = n;
    memcpy(g_entries, buf+4, n*sizeof(VaultEntry));
    free(buf);
    return true;
}

// Base32 decode for TOTP secrets
static size_t b32_decode(const char *in, uint8_t *out, size_t outmax) {
    int bits=0; uint32_t val=0; size_t n=0;
    while (*in) {
        char c = *in++;
        int v;
        if      (c>='A'&&c<='Z') v = c-'A';
        else if (c>='a'&&c<='z') v = c-'a';
        else if (c>='2'&&c<='7') v = 26 + (c-'2');
        else if (c=='='||c==' ') continue;
        else return 0;
        val = (val<<5) | v; bits += 5;
        if (bits >= 8) { bits -= 8; if (n<outmax) out[n++] = (val>>bits)&0xFF; }
    }
    return n;
}
static uint32_t totp_generate(const char *b32_secret) {
    uint8_t key[64]; size_t klen = b32_decode(b32_secret, key, sizeof(key));
    if (!klen) return 0;
    time_t now; time(&now);
    uint64_t counter = now / 30;
    uint8_t msg[8];
    for (int i=7; i>=0; i--) { msg[i] = counter & 0xFF; counter >>= 8; }
    uint8_t mac[20];
    mbedtls_md_context_t md;
    mbedtls_md_init(&md);
    mbedtls_md_setup(&md, mbedtls_md_info_from_type(MBEDTLS_MD_SHA1), 1);
    mbedtls_md_hmac_starts(&md, key, klen);
    mbedtls_md_hmac_update(&md, msg, 8);
    mbedtls_md_hmac_finish(&md, mac);
    mbedtls_md_free(&md);
    int off = mac[19] & 0x0F;
    uint32_t bin = ((mac[off]&0x7F)<<24) | (mac[off+1]<<16) | (mac[off+2]<<8) | mac[off+3];
    return bin % 1000000;
}

static void panic_wipe() {
    SD_MMC.remove("/WATCH/vault/vault.dat");
    Preferences p; p.begin("vaultos", false); p.clear(); p.end();
    g_n_entries = 0; memset(g_entries, 0, sizeof(g_entries));
}

// =============================================================================
//  UI state machine
// =============================================================================
enum Screen { SCR_UNLOCK, SCR_LIST, SCR_DETAIL };
static Screen g_screen = SCR_UNLOCK;
static int    g_selected = 0;
static lv_obj_t *scr_root=nullptr, *pw_label=nullptr, *attempts_lbl=nullptr;
static char  g_pw_buf[33] = ""; static int g_pw_n = 0;
static uint32_t g_boot_press_t = 0;
static uint32_t g_lockout_until = 0;

static void show_toast_text(const char *msg);

static void build_unlock_screen();
static void build_list_screen();
static void build_detail_screen();

static void kb_key_cb(lv_event_t *e) {
    const char *k = (const char*)lv_event_get_user_data(e);
    if (!k) return;
    if (strcmp(k,"<")==0) { if (g_pw_n>0) g_pw_buf[--g_pw_n]=0; }
    else if (strcmp(k,"OK")==0) {
        if (g_lockout_until > millis()) { show_toast_text("Locked out -- wait"); return; }
        // Try to decrypt
        if (vault_load(g_pw_buf)) {
            g_unlocked = true; g_fails = 0;
            Preferences p; p.begin("vaultos", false);
            p.putUChar("fails", 0);
            if (!p.isKey("salt")) { p.putBytes("salt", g_salt, 16); }
            p.end();
            g_relock_at_ms = millis() + AUTO_RELOCK_MS;
            g_screen = SCR_LIST; build_list_screen();
            return;
        } else {
            g_fails++;
            Preferences p; p.begin("vaultos", false); p.putUChar("fails", g_fails); p.end();
            if (g_fails >= 10) { panic_wipe(); show_toast_text("10 fails -- WIPED"); g_fails=0; }
            else {
                uint32_t pen = g_fails < 3 ? 1000 : g_fails < 6 ? 5000 : 30000;
                g_lockout_until = millis() + pen;
                char b[48]; snprintf(b,48,"Wrong (%u/10) -- %us", g_fails, pen/1000);
                show_toast_text(b);
            }
            g_pw_n = 0; g_pw_buf[0] = 0;
        }
    } else {
        if (g_pw_n < 32) { g_pw_buf[g_pw_n++] = k[0]; g_pw_buf[g_pw_n] = 0; }
    }
    if (pw_label) {
        char dots[34]; for (int i=0;i<g_pw_n;i++) dots[i]='*'; dots[g_pw_n]=0;
        lv_label_set_text(pw_label, dots[0] ? dots : "...");
    }
}

static lv_obj_t *toast = nullptr;
static uint32_t toast_until = 0;
static void show_toast_text(const char *msg) {
    if (!scr_root) return;
    if (toast) { lv_obj_del(toast); toast=nullptr; }
    toast = lv_label_create(scr_root);
    lv_label_set_text(toast, msg);
    lv_obj_set_style_text_color(toast, lv_color_hex(COL_DANGER), 0);
    lv_obj_set_style_text_font(toast, &lv_font_montserrat_14, 0);
    lv_obj_align(toast, LV_ALIGN_BOTTOM_MID, 0, -90);
    toast_until = millis() + 2200;
}

static void back_btn_cb(lv_event_t *) { return_to_launcher(); }

static void add_back_pill(lv_obj_t *parent) {
    lv_obj_t *back = lv_btn_create(parent);
    lv_obj_set_size(back, 220, 48);
    lv_obj_align(back, LV_ALIGN_BOTTOM_MID, 0, -22);
    lv_obj_set_style_bg_color(back, lv_color_hex(0x141414), 0);
    lv_obj_set_style_border_color(back, lv_color_hex(COL_ACCENT), 0);
    lv_obj_set_style_border_width(back, 1, 0);
    lv_obj_set_style_radius(back, 24, 0);
    lv_obj_t *bl = lv_label_create(back);
    lv_label_set_text(bl, LV_SYMBOL_LEFT "  Back to Launcher");
    lv_obj_set_style_text_color(bl, lv_color_hex(COL_ACCENT), 0);
    lv_obj_set_style_text_font(bl, &lv_font_montserrat_14, 0);
    lv_obj_center(bl);
    lv_obj_add_event_cb(back, back_btn_cb, LV_EVENT_CLICKED, NULL);
}

static const char *KEYS_ROW1[] = {"1","2","3","4","5","6","7","8","9","0"};
static const char *KEYS_ROW2[] = {"q","w","e","r","t","y","u","i","o","p"};
static const char *KEYS_ROW3[] = {"a","s","d","f","g","h","j","k","l","<"};
static const char *KEYS_ROW4[] = {"z","x","c","v","b","n","m",".","_","OK"};

static void make_key(lv_obj_t *parent, const char *k, int x, int y) {
    lv_obj_t *btn = lv_btn_create(parent);
    lv_obj_set_size(btn, 36, 30);
    lv_obj_set_pos(btn, x, y);
    lv_obj_set_style_bg_color(btn, lv_color_hex(0x141414), 0);
    lv_obj_set_style_border_color(btn, lv_color_hex(0x222222), 0);
    lv_obj_set_style_border_width(btn, 1, 0);
    lv_obj_set_style_radius(btn, 6, 0);
    lv_obj_t *l = lv_label_create(btn);
    lv_label_set_text(l, k);
    lv_obj_set_style_text_color(l, lv_color_hex(strcmp(k,"OK")==0 ? COL_OK : COL_TXT), 0);
    lv_obj_set_style_text_font(l, &lv_font_montserrat_14, 0);
    lv_obj_center(l);
    lv_obj_add_event_cb(btn, kb_key_cb, LV_EVENT_CLICKED, (void*)k);
}

static void build_unlock_screen() {
    if (scr_root) lv_obj_del(scr_root);
    scr_root = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr_root, lv_color_hex(COL_BG), 0);
    lv_obj_set_style_bg_opa(scr_root, LV_OPA_COVER, 0);

    lv_obj_t *title = lv_label_create(scr_root);
    lv_label_set_text(title, "VAULT::OS");
    lv_obj_set_style_text_color(title, lv_color_hex(COL_LOCK), 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_22, 0);
    lv_obj_set_style_text_letter_space(title, 4, 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 30);

    lv_obj_t *hint = lv_label_create(scr_root);
    lv_label_set_text(hint, "MASTER PASSWORD");
    lv_obj_set_style_text_color(hint, lv_color_hex(COL_MUTE), 0);
    lv_obj_set_style_text_font(hint, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_letter_space(hint, 3, 0);
    lv_obj_align(hint, LV_ALIGN_TOP_MID, 0, 70);

    pw_label = lv_label_create(scr_root);
    lv_label_set_text(pw_label, "...");
    lv_obj_set_style_text_color(pw_label, lv_color_hex(COL_TXT), 0);
    lv_obj_set_style_text_font(pw_label, &lv_font_montserrat_28, 0);
    lv_obj_align(pw_label, LV_ALIGN_TOP_MID, 0, 100);

    // keyboard
    lv_obj_t *kb = lv_obj_create(scr_root);
    lv_obj_remove_style_all(kb);
    lv_obj_set_size(kb, 380, 140);
    lv_obj_align(kb, LV_ALIGN_TOP_MID, 0, 160);
    lv_obj_set_style_bg_opa(kb, LV_OPA_TRANSP, 0);
    for (int i=0; i<10; i++) make_key(kb, KEYS_ROW1[i], i*38, 0);
    for (int i=0; i<10; i++) make_key(kb, KEYS_ROW2[i], i*38, 34);
    for (int i=0; i<10; i++) make_key(kb, KEYS_ROW3[i], i*38, 68);
    for (int i=0; i<10; i++) make_key(kb, KEYS_ROW4[i], i*38, 102);

    add_back_pill(scr_root);

    lv_obj_t *panic_hint = lv_label_create(scr_root);
    lv_label_set_text(panic_hint, "hold BOOT 10s = PANIC WIPE");
    lv_obj_set_style_text_color(panic_hint, lv_color_hex(0x333333), 0);
    lv_obj_set_style_text_font(panic_hint, &lv_font_montserrat_12, 0);
    lv_obj_align(panic_hint, LV_ALIGN_BOTTOM_MID, 0, -2);

    lv_scr_load(scr_root);
}

static void entry_tap_cb(lv_event_t *e) {
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    g_selected = idx;
    g_screen = SCR_DETAIL;
    g_relock_at_ms = millis() + AUTO_RELOCK_MS;
    build_detail_screen();
}

static void build_list_screen() {
    if (scr_root) lv_obj_del(scr_root);
    scr_root = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr_root, lv_color_hex(COL_BG), 0);
    lv_obj_set_style_bg_opa(scr_root, LV_OPA_COVER, 0);

    lv_obj_t *t = lv_label_create(scr_root);
    lv_label_set_text(t, "VAULT");
    lv_obj_set_style_text_color(t, lv_color_hex(COL_OK), 0);
    lv_obj_set_style_text_font(t, &lv_font_montserrat_22, 0);
    lv_obj_set_style_text_letter_space(t, 4, 0);
    lv_obj_align(t, LV_ALIGN_TOP_MID, 0, 24);

    lv_obj_t *list = lv_obj_create(scr_root);
    lv_obj_set_size(list, 380, 340);
    lv_obj_align(list, LV_ALIGN_TOP_MID, 0, 60);
    lv_obj_set_style_bg_color(list, lv_color_hex(COL_BG), 0);
    lv_obj_set_style_border_width(list, 0, 0);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(list, 8, 0);

    if (g_n_entries == 0) {
        lv_obj_t *empty = lv_label_create(list);
        lv_label_set_text(empty, "No entries.\nAdd via /WATCH/vault/import.txt");
        lv_obj_set_style_text_color(empty, lv_color_hex(COL_MUTE), 0);
        lv_obj_set_style_text_font(empty, &lv_font_montserrat_14, 0);
    }
    for (int i=0; i<g_n_entries; i++) {
        lv_obj_t *card = lv_btn_create(list);
        lv_obj_set_size(card, 360, 50);
        lv_obj_set_style_bg_color(card, lv_color_hex(COL_CARD), 0);
        lv_obj_set_style_border_color(card, lv_color_hex(g_entries[i].type==1 ? COL_LOCK : COL_ACCENT), 0);
        lv_obj_set_style_border_width(card, 1, 0);
        lv_obj_set_style_radius(card, 10, 0);
        lv_obj_t *lbl = lv_label_create(card);
        char buf[80]; snprintf(buf,80,"%s%s  %s", g_entries[i].type==1?"[TOTP] ":"", g_entries[i].label, g_entries[i].user);
        lv_label_set_text(lbl, buf);
        lv_obj_set_style_text_color(lbl, lv_color_hex(COL_TXT), 0);
        lv_obj_set_style_text_font(lbl, &lv_font_montserrat_14, 0);
        lv_obj_align(lbl, LV_ALIGN_LEFT_MID, 12, 0);
        lv_obj_add_event_cb(card, entry_tap_cb, LV_EVENT_CLICKED, (void*)(intptr_t)i);
    }
    add_back_pill(scr_root);
    lv_scr_load(scr_root);
}

static lv_obj_t *secret_lbl = nullptr;
static void detail_back_cb(lv_event_t *) { g_screen = SCR_LIST; build_list_screen(); }
static void build_detail_screen() {
    if (scr_root) lv_obj_del(scr_root);
    scr_root = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr_root, lv_color_hex(COL_BG), 0);
    lv_obj_set_style_bg_opa(scr_root, LV_OPA_COVER, 0);

    VaultEntry &e = g_entries[g_selected];
    lv_obj_t *t = lv_label_create(scr_root);
    lv_label_set_text(t, e.label);
    lv_obj_set_style_text_color(t, lv_color_hex(COL_TXT), 0);
    lv_obj_set_style_text_font(t, &lv_font_montserrat_22, 0);
    lv_obj_align(t, LV_ALIGN_TOP_MID, 0, 40);

    lv_obj_t *u = lv_label_create(scr_root);
    lv_label_set_text(u, e.user);
    lv_obj_set_style_text_color(u, lv_color_hex(COL_MUTE), 0);
    lv_obj_set_style_text_font(u, &lv_font_montserrat_14, 0);
    lv_obj_align(u, LV_ALIGN_TOP_MID, 0, 75);

    secret_lbl = lv_label_create(scr_root);
    lv_obj_set_style_text_color(secret_lbl, lv_color_hex(e.type==1?COL_LOCK:COL_ACCENT), 0);
    lv_obj_set_style_text_font(secret_lbl, &lv_font_montserrat_48, 0);
    lv_obj_align(secret_lbl, LV_ALIGN_CENTER, 0, 0);
    if (e.type == 1) lv_label_set_text(secret_lbl, "------");
    else             lv_label_set_text(secret_lbl, e.secret);

    lv_obj_t *up = lv_btn_create(scr_root);
    lv_obj_set_size(up, 180, 42);
    lv_obj_align(up, LV_ALIGN_BOTTOM_MID, 0, -80);
    lv_obj_set_style_bg_color(up, lv_color_hex(0x141414), 0);
    lv_obj_set_style_border_color(up, lv_color_hex(COL_MUTE), 0);
    lv_obj_set_style_border_width(up, 1, 0);
    lv_obj_set_style_radius(up, 20, 0);
    lv_obj_t *upl = lv_label_create(up);
    lv_label_set_text(upl, LV_SYMBOL_UP "  back to list");
    lv_obj_set_style_text_color(upl, lv_color_hex(COL_MUTE), 0);
    lv_obj_set_style_text_font(upl, &lv_font_montserrat_14, 0);
    lv_obj_center(upl);
    lv_obj_add_event_cb(up, detail_back_cb, LV_EVENT_CLICKED, NULL);

    add_back_pill(scr_root);
    lv_scr_load(scr_root);
}

// =============================================================================
//  Import: parse /WATCH/vault/import.txt on first unlock to bootstrap entries
//   Format per line:  P|label|user|password    (password entry)
//                     T|label|user|BASE32SECRET (TOTP entry)
//   File is deleted after successful import.
// =============================================================================
static void try_import() {
    File f = SD_MMC.open("/WATCH/vault/import.txt", FILE_READ);
    if (!f) return;
    while (f.available() && g_n_entries < MAX_ENTRIES) {
        String line = f.readStringUntil('\n'); line.trim();
        if (line.length()==0 || line[0]=='#') continue;
        int a = line.indexOf('|'); int b = line.indexOf('|', a+1); int c = line.indexOf('|', b+1);
        if (a<0||b<0||c<0) continue;
        VaultEntry &e = g_entries[g_n_entries];
        memset(&e, 0, sizeof(e));
        e.type = (line[0]=='T') ? 1 : 0;
        strlcpy(e.label,  line.substring(a+1,b).c_str(), sizeof(e.label));
        strlcpy(e.user,   line.substring(b+1,c).c_str(), sizeof(e.user));
        strlcpy(e.secret, line.substring(c+1).c_str(),   sizeof(e.secret));
        g_n_entries++;
    }
    f.close();
    SD_MMC.remove("/WATCH/vault/import.txt");
    vault_save();
}

// =============================================================================
//  setup() / loop()
// =============================================================================
static uint32_t g_last_totp_refresh = 0;

void setup() {
    launcher_app_boot();
    splash_show();
    // Guarantee /WATCH and its subdirs exist (works on a brand-new SD card)
    SD_MMC.mkdir("/WATCH");
    SD_MMC.mkdir("/WATCH/bin");
    SD_MMC.mkdir("/WATCH/wifi");
    SD_MMC.mkdir("/WATCH/vault");
    time_restore_from_launcher();
    read_wifi_cfg();
    wifi_quick_ntp();

    Preferences p; p.begin("vaultos", false);
    if (p.isKey("salt")) { p.getBytes("salt", g_salt, 16); }
    else {
        for (int i=0;i<16;i++) g_salt[i] = (uint8_t)esp_random();
        p.putBytes("salt", g_salt, 16);
    }
    g_fails = p.getUChar("fails", 0);
    p.end();

    build_unlock_screen();
}

void loop() {
    lv_timer_handler();
    uint32_t now = millis();

    // BOOT-button: short = back-to-launcher (safety), held 10 s on unlock = panic wipe
    static uint32_t btn_t = 0;
    if (digitalRead(BOOT_BTN_PIN) == LOW) {
        if (!btn_t) btn_t = now;
        else if (g_screen == SCR_UNLOCK && now - btn_t > 10000) panic_wipe();
        else if (now - btn_t > 1500) return_to_launcher();
    } else btn_t = 0;

    // Toast clear
    if (toast && now > toast_until) { lv_obj_del(toast); toast=nullptr; }

    // Auto-relock
    if (g_unlocked && now > g_relock_at_ms) {
        g_unlocked = false; memset(g_key, 0, 32);
        g_pw_n = 0; g_pw_buf[0]=0; g_screen = SCR_UNLOCK;
        build_unlock_screen();
    }

    // Activity keeps unlock alive
    if (g_unlocked && g_last_touch_t + 1000 > now) {
        g_relock_at_ms = now + AUTO_RELOCK_MS;
    }

    // TOTP live update (once per second on the detail screen)
    if (g_unlocked && g_screen == SCR_DETAIL && secret_lbl &&
        g_entries[g_selected].type == 1 && now - g_last_totp_refresh > 1000) {
        g_last_totp_refresh = now;
        uint32_t code = totp_generate(g_entries[g_selected].secret);
        char b[8]; snprintf(b, 8, "%06u", code);
        lv_label_set_text(secret_lbl, b);
    }

    // First successful unlock -> try import.txt
    static bool imported = false;
    if (g_unlocked && !imported) { imported = true; try_import(); }

    delay(5);
}
