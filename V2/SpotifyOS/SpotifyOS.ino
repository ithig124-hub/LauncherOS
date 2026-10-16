/* ============================================================================
   SpotifyOS.ino  --  offline Spotify-inspired music player
   ----------------------------------------------------------------------------
   Target board : Waveshare ESP32-S3-Touch-AMOLED-2.06
                  ESP32-S3, 8 MB PSRAM, 410 x 502 AMOLED (CO5300, QSPI),
                  FT3168 touch, AXP2101 PMIC, ES8311 codec, NS4150B amp,
                  SD/TF card.
   Framework    : Arduino-ESP32 core 3.x, LVGL 8.4.x
   Launcher     : LauncherOS.  This sketch is built to a .bin that the user
                  selects from LauncherOS; LauncherOS copies it into the ota_0
                  partition and reboots.  "Back to Launcher" below re-anchors
                  the factory (launcher) partition and restarts, exactly like
                  AstroOS.ino does.

   Themes 4..6 ("Garden Light", "Golden King", "Castle Dawn") are painted for
   the three knight wallpapers.  Generate them with:
       python tools/make_wallpapers.py garden.jpg  --theme garden-light
       python tools/make_wallpapers.py king.jpg    --theme golden-king
       python tools/make_wallpapers.py castle.jpg  --theme castle-dawn

   This file is a SINGLE .ino.  Everything (codec driver, output abstraction,
   library indexer, playback engine, UI) lives in this one file.  It needs one
   companion header in the same sketch folder:
       lv_conf.h   (the LVGL 8.4 config used by the LauncherOS/AstroOS apps;
                    it must have LV_USE_MSGBOX, LV_USE_SLIDER, LV_USE_BAR,
                    LV_USE_SWITCH, LV_USE_KEYBOARD and LV_USE_LIST enabled)

   Required libraries (Arduino Library Manager):
       lvgl                    8.4.x
       Arduino_GFX_Library
       Arduino_DriveBus
       XPowersLib
       ESP32-audioI2S         (schreibfaul1)
   Board options: ESP32S3 Dev Module, PSRAM "OPI PSRAM" (or QSPI PSRAM),
                  Flash 16MB, Partition Scheme "16M Flash (3MB APP/9.9MB ...)"
                  or the LauncherOS 16 MB dual-slot table, PSRAM enabled.

   Storage layout (created automatically on first launch):
       /WATCH/SpotifyOS/
           Music/<Playlist>/NNN.mp3        (folder = playlist, auto-shuffled,
                                            no playlist.json)
           Data/{library,liked,history,queue,stats}.json
           Config/{settings,theme}.json
           Wallpapers/            (raw RGB565 410x502 .raw/.bin;
                                   garden-light / golden-king / castle-dawn)
           Cache/artwork/         (optional per-track square RGB565 .raw)

   HONESTY NOTES (important):
     * Watch Speaker is the only real audio output on this hardware.
       ES8311 -> NS4150B -> onboard speaker.
     * "Bluetooth Audio" is a future-ready mode.  The ESP32-S3 has no
       Bluetooth Classic A2DP and no LE-Audio playback profile, so with the
       stock hardware there is no audio path.  Bluetooth Audio reports
       UNAVAILABLE and never silently falls back to the speaker.  See
       SPOTIFY_HAS_BT_AUDIO_HW below.
     * The volume slider drives the real ES8311 DAC volume register (0x32)
       and, at 0 %, mutes the codec.  It is not a cosmetic number.
   TIME -- SHARED CLOCK CONTRACT (spec: TIME_CONTRACT.md):
     * the wall clock is not this app's private business.  It lives in the NVS
       namespace "rtcsave" (t = epoch, gmt = offset in SECONDS, seq, man, src)
       and the SHARED TIME BLOCK below is inlined verbatim into every app
       under V2/.  SpotifyOS adopts it at boot, publishes it on the way out
       and every 5 min, and re-applies the timezone the moment it is read --
       TZ is RAM-only and dies on EVERY reboot, so a module that skips that
       renders UTC until something else happens to set it.
     * this file used to pin TZ to UTC0 and format the clock with gmtime_r(),
       which ignores TZ altogether: the glance and the header could not have
       shown local time even with the right offset stored.
   ============================================================================ */

#include <Arduino.h>
#include <FS.h>
#include <SD_MMC.h>
#include <Wire.h>
#include <WiFi.h>
#include <Preferences.h>
#include <time.h>
#include <sys/time.h>
#include <memory>
#include <math.h>
#include <ctype.h>
#include <vector>
#include "Arduino_GFX_Library.h"
#include "Arduino_DriveBus_Library.h"

#ifndef LV_CONF_INCLUDE_SIMPLE
#define LV_CONF_INCLUDE_SIMPLE
#endif
#include "lv_conf.h"
#include "lvgl.h"

#define XPOWERS_CHIP_AXP2101
#include "XPowersLib.h"

#include <esp_ota_ops.h>
#include <esp_partition.h>
#include <esp_system.h>
#include <esp_sleep.h>
#include <esp_heap_caps.h>
#include <esp_bt.h>
#include <esp_log.h>
#include <freertos/task.h>
#include "HWCDC.h"

/* LauncherOS -- the known-good app for this board -- declares its own HWCDC
 * object and logs on it (`HWCDC USBSerial;` + `USBSerial.begin(115200)`), and
 * that is the port the USB-C cable enumerates, so it is the one you are
 * actually watching.  Arduino's `Serial` only reaches that same port when the
 * board is built with "USB CDC On Boot: Enabled"; with it disabled Serial is
 * UART0, which this board does not bring out -- and every diagnostic below
 * would be invisible.  All boot logging goes through USBSerial. */
HWCDC USBSerial;

#include "Audio.h"

#if LVGL_VERSION_MAJOR != 8 || LVGL_VERSION_MINOR != 4
#error "SpotifyOS requires LVGL 8.4.x (same as LauncherOS/AstroOS)"
#endif

/* ---------------------------------------------------------------------------
 * 0.  LOOP TASK STACK
 *     The core runs setup() and loop() in ONE task, created with
 *     ARDUINO_LOOP_STACK_SIZE -- 8192 bytes.  That single task does everything:
 *     the LVGL render, the QSPI panel flush, the I2C touch and PMIC reads, the
 *     JSON/SD work and the MP3 decoding (ESP32-audioI2S decodes inside whoever
 *     calls its loop()).  8 KB is not enough for that combination: the end-of-
 *     stack watchpoint trips and the core panics with
 *         Debug exception reason: Stack canary watchpoint triggered (loopTask)
 *     and reboots, forever.  The core's definition is weak
 *     (cores/esp32/main.cpp, "__attribute__((weak)) size_t
 *     getArduinoLoopTaskStackSize(void)"), so redefining it here is the
 *     supported way to ask for more without touching the core.  The boot log
 *     prints the value that was actually used, so the log always says which
 *     build is running.
 * ------------------------------------------------------------------------ */
#define SPOTIFY_LOOP_STACK   32768
#define SPOTIFY_BUILD_TAG    "v1.3.1-shared-clock"
/* NOTE: the *definition* of getArduinoLoopTaskStackSize() is deliberately NOT
 * here.  Arduino generates a prototype for every function in the sketch and
 * inserts that block at the FIRST function definition in the file -- so a
 * function defined up here drags the insertion point above the type
 * declarations below, and the generated prototype for readId3v2(...Track&...)
 * then fails with "'Track' has not been declared".  It is defined next to its
 * declaration in section 16 instead.  tools/tests/arduino_prototypes_test.py runs
 * the builder's own generator and fails if a definition ever moves back here. */
/* For the record: LauncherOS fits inside the 8 KB default, because it never
 * decodes audio on this task -- this sketch does (ESP32-audioI2S decodes in
 * whoever calls its loop()), so it asks for 32 KB.  The stack overflow we
 * chased was NOT fixed by this size: it happened in the failing-I2C path while
 * the touch controller was dead, which the retry loop in hardwareInit() and
 * the reference app's own `while (FT3168->begin() == false)` address. */

/* ---------------------------------------------------------------------------
 * 1.  PIN MAP  --  verified against pin_config.h and the Waveshare wiki
 *     (docs.waveshare.com/ESP32-S3-Touch-AMOLED-2.06)
 * ------------------------------------------------------------------------ */
#define LCD_SDIO0   4
#define LCD_SDIO1   5
#define LCD_SDIO2   6
#define LCD_SDIO3   7
#define LCD_SCLK    11
#define LCD_CS      12
#define LCD_RESET   8
#define LCD_W       410
#define LCD_H       502

#define IIC_SDA     15
#define IIC_SCL     14
#define TP_INT      38
#define TP_RESET    9
#define BOOT_BTN    0

#define I2S_MCLK    16   /* ES8311 MCLK                       */
#define I2S_BCLK    41   /* ES8311 SCLK/BCLK                  */
#define I2S_LRC     45   /* ES8311 LRCK/WS                    */
#define I2S_DOUT    40   /* ESP32 -> ES8311 I2S_DSDIN (play)  */
#define I2S_DIN     42   /* ES8311/ES7210 -> ESP32 (mic, not used) */
#define AMP_EN      46   /* NS4150B enable, active HIGH       */

static const int SDMMC_CLK = 2, SDMMC_CMD = 1, SDMMC_DATA = 3;

/* ---------------------------------------------------------------------------
 * 2.  FEATURE SWITCHES
 * ------------------------------------------------------------------------ */
/* Bluetooth audio requires an added A2DP-capable transmitter / companion
 * audio processor.  The ESP32-S3 alone cannot do it.  Flip this to 1 only
 * after such hardware is wired in and driven by BluetoothAudioOutput. */
#define SPOTIFY_HAS_BT_AUDIO_HW 0

#define SPOTIFY_APP_DIR      "/WATCH/SpotifyOS"
#define SPOTIFY_MUSIC_DIR    SPOTIFY_APP_DIR "/Music"
#define SPOTIFY_DATA_DIR     SPOTIFY_APP_DIR "/Data"
#define SPOTIFY_CONFIG_DIR   SPOTIFY_APP_DIR "/Config"
#define SPOTIFY_WALL_DIR     SPOTIFY_APP_DIR "/Wallpapers"
#define SPOTIFY_ART_DIR      SPOTIFY_APP_DIR "/Cache/artwork"

#define MAX_TRACKS           500
#define MAX_PLAYLISTS        40
#define MAX_QUEUE            400
#define MAX_HISTORY          60
#define MAX_ART_PX           400          /* max artwork edge in px        */
#define DEFAULT_VOLUME       40
#define SAFE_MAX_VOLUME_PCT  85           /* keep the 3 W amp out of clipping */
#define SCREEN_OFF_TIMEOUT_MS 15000UL
#define WAKE_GRACE_MS        2000UL
#define RETURN_HOLD_MS       1500UL
#define CLICK_AFTER_SWIPE_MS 350UL
#define SWIPE_THRESHOLD_PX   25
#define SWIPE_MAX_DURATION_MS 800UL
#define DEFAULT_BRIGHTNESS   180
#define SCHEMA_VERSION       1

/* ---------------------------------------------------------------------------
 * 3.  DESIGN TOKENS / THEMES
 *     Nothing below hardcodes the Spotify Classic palette except the theme
 *     table itself; every screen reads g_theme.
 * ------------------------------------------------------------------------ */
struct ThemeTokens {
  uint32_t bg;        /* primary background      */
  uint32_t card;      /* card background         */
  uint32_t elev;      /* elevated surface        */
  uint32_t text;      /* primary text            */
  uint32_t text2;     /* secondary/muted text    */
  uint32_t accent;    /* accent / progress fill  */
  uint32_t error;     /* error colour            */
  uint32_t warn;      /* warning colour          */
  uint32_t divider;   /* subtle divider          */
};

/* 4..6 are painted to match the three knight paintings that ship with the
 * wallpaper pack (tools/make_wallpapers.py).  Order matters: the Theme &
 * Wallpaper grid, THEME_ACCENTS and THEME_WALLPAPERS all follow this order. */
#define THEME_COUNT   7
#define THEME_CUSTOM  THEME_COUNT   /* hand-picked accent, tokens from theme.json */

static const ThemeTokens THEME_PRESETS[THEME_COUNT] = {
  /* 0 Spotify Classic */ { 0x121212, 0x202020, 0x2A2A2A, 0xFFFFFF, 0xB3B3B3, 0x1ED760, 0xF15E6C, 0xF0B429, 0x2A2A2A },
  /* 1 Freedom         */ { 0x0B0F14, 0x141C26, 0x1E2938, 0xEAF2FA, 0x93A4B8, 0x3B9DFF, 0xE0645F, 0xE9B949, 0x1E2938 },
  /* 2 Shadow          */ { 0x08070D, 0x14121F, 0x1E1B2E, 0xF2EFFA, 0x9A93B8, 0x9B6BFF, 0xD95C6A, 0xE0A54A, 0x1E1B2E },
  /* 3 Nature          */ { 0x0A100C, 0x13201A, 0x1B2C23, 0xEFF7F0, 0x9BB3A4, 0x5BE49B, 0xD4635F, 0xE3B24B, 0x1B2C23 },
  /* 4 Garden Light    */ { 0x12140B, 0x1D2013, 0x2A2E1B, 0xFFF7E6, 0xC9BEA2, 0xE9B23C, 0xE0604F, 0xF0B429, 0x2A2E1B },
  /* 5 Golden King     */ { 0x140D04, 0x22180A, 0x342511, 0xFFF1D8, 0xD9BA8B, 0xF3A63A, 0xD65A45, 0xFFC857, 0x342511 },
  /* 6 Castle Dawn     */ { 0x150A13, 0x241328, 0x341E36, 0xFFEFF7, 0xD9B6CE, 0xF0799E, 0xE0526A, 0xF5B96A, 0x341E36 },
};
static const char *THEME_NAMES[THEME_COUNT] = {
  "Spotify Classic", "Freedom", "Shadow", "Nature",
  "Garden Light", "Golden King", "Castle Dawn"
};
/* Default accent of each preset; also the swatch colours on the Theme screen. */
static const uint32_t THEME_ACCENTS[THEME_COUNT] = {
  0x1ED760, 0x3B9DFF, 0x9B6BFF, 0x5BE49B, 0xE9B23C, 0xF3A63A, 0xF0799E
};
/* Wallpaper that belongs to each preset.  "" = none.  Files live in
 * /WATCH/SpotifyOS/Wallpapers/ as raw little-endian RGB565 410x502.
 * These three names are what tools/make_wallpapers.py --theme writes. */
static const char *THEME_WALLPAPERS[THEME_COUNT] = {
  "", "", "", "", "garden-light.raw", "golden-king.raw", "castle-dawn.raw"
};

static ThemeTokens g_theme;         /* active tokens (preset or custom) */

/* ---------------------------------------------------------------------------
 * 3b. LAYOUT + TYPE SCALE  (the look-and-feel numbers, in one place)
 *     Screens read these instead of inventing their own values.  They hold the
 *     numbers the existing screens already use, so adopting them is visually a
 *     no-op; the new Quick Glance screen inherits the same rhythm, and the
 *     older screens can converge on them as they are touched.
 * ------------------------------------------------------------------------ */
#define UI_PAD          14     /* screen-edge inset for full-width content  */
#define UI_GAP           8     /* gap between related controls              */
#define UI_RADIUS_CARD  18     /* card / list-row corner radius             */
#define UI_RADIUS_PILL  26     /* bar / sheet corner radius                 */
#define UI_TOUCH_MIN    46     /* smallest comfortable touch target         */
#define UI_HEADER_H     54     /* header band height                        */

/* Quick Glance geometry, so the layout can be checked as arithmetic rather
 * than by eye (tools/tests/glance_layout_test.py). */
#define UI_GLANCE_BAR_H 112    /* mini-player sheet height                  */
#define UI_GLANCE_ART    60    /* album art in the sheet                    */
#define UI_GLANCE_BTN    56    /* play/pause and next in the sheet          */
#define UI_GLANCE_COL_W 176    /* title / artist / progress column width    */

/* The clock face wants the biggest Montserrat the config provides.  The
 * lv_conf.h this sketch ships with (the LauncherOS/AstroOS one) enables 48;
 * any other config falls back to 28 instead of failing to link. */
#if LV_FONT_MONTSERRAT_48
  #define UI_FONT_CLOCK  &lv_font_montserrat_48
#else
  #define UI_FONT_CLOCK  &lv_font_montserrat_28
#endif
#define UI_FONT_TITLE   &lv_font_montserrat_18
#define UI_FONT_BODY    &lv_font_montserrat_14
#define UI_FONT_META    &lv_font_montserrat_12

/* ---------------------------------------------------------------------------
 * 4.  GLOBAL STATE
 * ------------------------------------------------------------------------ */
static bool g_has_pmu = false, g_sd_ok = false;
static bool g_screen_off = false, g_returning = false;
static uint8_t g_brightness = 0;

static lv_disp_draw_buf_t g_draw_buf;
static lv_color_t *g_lvbuf = nullptr;
static lv_disp_drv_t g_disp_drv;
static lv_indev_drv_t g_indev_drv;

static Arduino_DataBus *g_bus = nullptr;
static Arduino_GFX *gfx = nullptr;
static XPowersPMU g_pmu;

static uint32_t g_last_touch_ms = 0, g_last_wake_ms = 0, g_last_swipe_ms = 0;
static bool g_touch_active = false;
static int32_t g_start_x = 0, g_start_y = 0, g_current_x = 0, g_current_y = 0;
static uint32_t g_start_ms = 0;
static volatile int g_pending_swipe = 0;   /* +1 = swipe left, -1 = swipe right */

/* ---------------------------------------------------------------------------
 * 4b. TRACK RECORD (declared before the first function so Arduino's generated
 *     prototypes are always valid, matching the LauncherOS/AstroOS style)
 * ------------------------------------------------------------------------ */
struct Track {
  String path;       /* relative to Music/, e.g. "Chill/001.mp3" */
  String title;      /* falls back to the filename                  */
  String artist;
  String album;
  uint32_t duration; /* seconds, 0 = not known yet                   */
};

static Track g_tracks[MAX_TRACKS];
static int g_trackCount = 0;

/* The only two functions whose signature mentions a type declared in this file.
 * Arduino generates prototypes for functions that do not already have one, and
 * inserts them at the first function definition in the sketch; declaring these
 * here -- below `struct Track`, above every function -- leaves nothing for it to
 * generate that could land above the type.  Same style as LauncherOS, which
 * keeps a forward-declaration block next to its own local types. */
static bool readId3v2(File &f, Track &t, uint32_t &audioStart);
static bool trackFileExists(const Track &t);

/* The Arduino builder normally inserts its own prototype block before the
 * first function definition, which is why the rest of this file does not
 * declare what it defines.  That generated block is not guaranteed to
 * cover everything -- a build of this sketch failed with "'uiAction' was
 * not declared in this scope" -- so the functions that are *used* before
 * they are defined are declared here instead, above every use.  A
 * declaration here is free; a missing one is a compile error. */
static void playerAccumulateSession();
static String fmtTime(uint32_t sec);
static void uiAction(lv_event_t *e);
static void runSearch();
static void buildSearch();
static void pump();
static void fadeBrightness(uint8_t target, uint32_t duration);
static void returnToLauncher();
static void logBoot();

/* Forward declarations for types that appear in the prototypes Arduino
 * generates for this .ino (they must be visible before the first function). */
class AudioOutput;

/* ---------------------------------------------------------------------------
 * 5.  ES8311 CODEC DRIVER  (I2C register level, Wire based)
 *     Register logic follows the Espressif ES8311 driver / the ESP32-audioI2S
 *     Arduino example driver.  Only the playback path is implemented; the
 *     microphone is left powered down because SpotifyOS never records.
 * ------------------------------------------------------------------------ */
#define ES8311_ADDR 0x18

struct EsCoeff {
  uint32_t mclk, rate;
  uint8_t pre_div, pre_multi, adc_div, dac_div, fs_mode, lrck_h, lrck_l, bclk_div, adc_osr, dac_osr;
};
/* One row per MP3 sample-rate, MCLK = 256 x fs (what ESP32-audioI2S drives). */
static const EsCoeff ES_COEFF[] = {
  { 2048000,  8000, 0x01, 0x00, 0x01, 0x01, 0x00, 0x00, 0xff, 0x04, 0x10, 0x10 },
  { 2822400, 11025, 0x01, 0x00, 0x01, 0x01, 0x00, 0x00, 0xff, 0x04, 0x10, 0x10 },
  { 3072000, 12000, 0x01, 0x00, 0x01, 0x01, 0x00, 0x00, 0xff, 0x04, 0x10, 0x10 },
  { 4096000, 16000, 0x01, 0x00, 0x01, 0x01, 0x00, 0x00, 0xff, 0x04, 0x10, 0x10 },
  { 5644800, 22050, 0x01, 0x00, 0x01, 0x01, 0x00, 0x00, 0xff, 0x04, 0x10, 0x10 },
  { 6144000, 24000, 0x01, 0x00, 0x01, 0x01, 0x00, 0x00, 0xff, 0x04, 0x10, 0x10 },
  { 8192000, 32000, 0x01, 0x00, 0x01, 0x01, 0x00, 0x00, 0xff, 0x04, 0x10, 0x10 },
  { 11289600, 44100, 0x01, 0x00, 0x01, 0x01, 0x00, 0x00, 0xff, 0x04, 0x10, 0x10 },
  { 12288000, 48000, 0x01, 0x00, 0x01, 0x01, 0x00, 0x00, 0xff, 0x04, 0x10, 0x10 },
};

static uint32_t g_codecRate = 0;       /* sample rate the codec is set to */

static bool esWrite(uint8_t reg, uint8_t val) {
  Wire.beginTransmission(ES8311_ADDR);
  Wire.write(reg);
  Wire.write(val);
  return Wire.endTransmission() == 0;
}
static uint8_t esRead(uint8_t reg) {
  Wire.beginTransmission(ES8311_ADDR);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) return 0;
  if (Wire.requestFrom((uint8_t)ES8311_ADDR, (uint8_t)1) != 1) return 0;
  return Wire.read();
}

static bool esSetSampleRate(uint32_t rate) {
  const EsCoeff *c = nullptr;
  for (size_t i = 0; i < sizeof(ES_COEFF) / sizeof(ES_COEFF[0]); i++)
    if (ES_COEFF[i].rate == rate) { c = &ES_COEFF[i]; break; }
  if (!c) return false;                       /* unsupported rate */

  uint8_t reg = esRead(0x02);
  reg |= (uint8_t)((c->pre_div - 1) << 5);
  reg |= (uint8_t)(c->pre_multi << 3);
  bool ok = esWrite(0x02, reg);
  ok &= esWrite(0x03, (uint8_t)((c->fs_mode << 6) | c->adc_osr));
  ok &= esWrite(0x04, c->dac_osr);
  ok &= esWrite(0x05, (uint8_t)(((c->adc_div - 1) << 4) | (c->dac_div - 1)));
  reg = esRead(0x06);
  reg &= 0xE0;
  reg |= (uint8_t)((c->bclk_div < 19 ? (c->bclk_div - 1) : c->bclk_div) & 0x1F);
  ok &= esWrite(0x06, reg);
  reg = esRead(0x07);
  reg &= 0xC0;
  reg |= c->lrck_h;
  ok &= esWrite(0x07, reg);
  ok &= esWrite(0x08, c->lrck_l);
  if (ok) g_codecRate = rate;
  return ok;
}

static bool esSetBitsPerSample(uint8_t bps) {
  uint8_t r9 = esRead(0x09), rA = esRead(0x0A);
  uint8_t sel;
  switch (bps) {
    case 16: sel = 3; break;
    case 18: sel = 2; break;
    case 20: sel = 1; break;
    case 24: sel = 0; break;
    case 32: sel = 4; break;
    default: return false;
  }
  r9 = (uint8_t)((r9 & ~(0x07 << 2)) | (sel << 2));
  rA = (uint8_t)((rA & ~(0x07 << 2)) | (sel << 2));
  bool ok = esWrite(0x09, r9);
  ok &= esWrite(0x0A, rA);
  return ok;
}

/* volume 0..100 -> ES8311 DAC volume register 0x32.  0 mutes. */
static bool esSetVolumePct(uint8_t pct) {
  if (pct > 100) pct = 100;
  uint8_t reg32 = (pct == 0) ? 0 : (uint8_t)(((uint16_t)pct * 256 / 100) - 1);
  return esWrite(0x32, reg32);
}

static bool esBegin() {
  Wire.beginTransmission(ES8311_ADDR);
  if (Wire.endTransmission() != 0) return false;      /* codec not on the bus */

  bool ok = true;
  ok &= esWrite(0x00, 0x1F);  delay(20);   /* reset          */
  ok &= esWrite(0x00, 0x00);               /* release reset  */
  ok &= esWrite(0x00, 0x80);               /* power on       */
  ok &= esWrite(0x01, 0x3F);               /* all clocks on  */

  uint8_t r6 = esRead(0x06);
  r6 &= (uint8_t)~0x20;                    /* SCLK not inverted */
  ok &= esWrite(0x06, r6);

  ok &= esSetSampleRate(44100);            /* default, re-set per track */
  ok &= esSetBitsPerSample(16);

  ok &= esWrite(0x0D, 0x01);   /* power up analog circuitry        */
  ok &= esWrite(0x0E, 0x02);   /* analog PGA + ADC modulator (off) */
  ok &= esWrite(0x12, 0x00);   /* power up DAC                     */
  ok &= esWrite(0x13, 0x10);   /* enable output to HP drive        */
  ok &= esWrite(0x1C, 0x6A);   /* ADC EQ bypass / DC offset        */
  ok &= esWrite(0x37, 0x08);   /* bypass DAC equaliser             */
  ok &= esWrite(0x14, 0x00);   /* analog mic off (we never record) */
  return ok;
}

/* ---------------------------------------------------------------------------
 * 6.  TINY JSON HELPERS
 *     The sketch only ever reads files it wrote itself, so a compact
 *     key/value scanner is enough and avoids another library dependency.
 *     Every reader is tolerant: a malformed document yields defaults.
 * ------------------------------------------------------------------------ */
static String jsonEsc(const String &in) {
  String out;
  out.reserve(in.length() + 8);
  for (size_t i = 0; i < in.length(); i++) {
    char c = in[i];
    if (c == '"' || c == '\\') { out += '\\'; out += c; }
    else if (c == '\n') out += "\\n";
    else if (c == '\r') out += "\\r";
    else if (c == '\t') out += "\\t";
    else out += c;
  }
  return out;
}

/* index just past the ':' of "key": , or -1 */
static int jsonKeyPos(const String &doc, const char *key) {
  String pat = String("\"") + key + "\"";
  int p = doc.indexOf(pat);
  if (p < 0) return -1;
  int c = doc.indexOf(':', p + pat.length());
  if (c < 0) return -1;
  return c + 1;
}

static String jsonGetStr(const String &doc, const char *key, const String &dflt = "") {
  int p = jsonKeyPos(doc, key);
  if (p < 0) return dflt;
  while (p < (int)doc.length() && isspace((unsigned char)doc[p])) p++;
  if (p >= (int)doc.length() || doc[p] != '"') return dflt;
  p++;
  String out;
  while (p < (int)doc.length()) {
    char c = doc[p++];
    if (c == '\\' && p < (int)doc.length()) {
      char n = doc[p++];
      if (n == 'n') out += '\n';
      else if (n == 't') out += '\t';
      else if (n == 'r') out += '\r';
      else out += n;
    } else if (c == '"') break;
    else out += c;
  }
  return out;
}

static long jsonGetLong(const String &doc, const char *key, long dflt = 0) {
  int p = jsonKeyPos(doc, key);
  if (p < 0) return dflt;
  while (p < (int)doc.length() && isspace((unsigned char)doc[p])) p++;
  String num;
  while (p < (int)doc.length()) {
    char c = doc[p];
    if (isDigit((unsigned char)c) || c == '-' || c == '+' || c == '.' || c == 'e' || c == 'E') num += c;
    else break;
    p++;
  }
  if (!num.length()) return dflt;
  return (long)num.toFloat();
}

static bool jsonGetBool(const String &doc, const char *key, bool dflt = false) {
  int p = jsonKeyPos(doc, key);
  if (p < 0) return dflt;
  while (p < (int)doc.length() && isspace((unsigned char)doc[p])) p++;
  if (doc.startsWith("true", p)) return true;
  if (doc.startsWith("false", p)) return false;
  return dflt;
}

/* Reads a flat JSON array of strings under "key". */
static int jsonGetStrArray(const String &doc, const char *key, String *out, int maxOut) {
  int p = jsonKeyPos(doc, key);
  if (p < 0) return 0;
  while (p < (int)doc.length() && doc[p] != '[') p++;
  if (p >= (int)doc.length()) return 0;
  p++;
  int n = 0;
  while (p < (int)doc.length() && n < maxOut) {
    while (p < (int)doc.length() && (isspace((unsigned char)doc[p]) || doc[p] == ',')) p++;
    if (p >= (int)doc.length() || doc[p] == ']') break;
    if (doc[p] != '"') break;
    p++;
    String s;
    while (p < (int)doc.length()) {
      char c = doc[p++];
      if (c == '\\' && p < (int)doc.length()) {
        char e = doc[p++];
        if (e == 'n') s += '\n';
        else if (e == 't') s += '\t';
        else s += e;
      } else if (c == '"') break;
      else s += c;
    }
    out[n++] = s;
  }
  return n;
}

/* Reads a flat JSON array of numbers under "key". */
static int jsonGetLongArray(const String &doc, const char *key, long *out, int maxOut) {
  int p = jsonKeyPos(doc, key);
  if (p < 0) return 0;
  while (p < (int)doc.length() && doc[p] != '[') p++;
  if (p >= (int)doc.length()) return 0;
  p++;
  int n = 0;
  while (p < (int)doc.length() && n < maxOut) {
    while (p < (int)doc.length() && (isspace((unsigned char)doc[p]) || doc[p] == ',')) p++;
    if (p >= (int)doc.length() || doc[p] == ']') break;
    String num;
    while (p < (int)doc.length() && (isDigit((unsigned char)doc[p]) || doc[p] == '-' || doc[p] == '+' || doc[p] == '.')) {
      num += doc[p++];
    }
    if (!num.length()) { p++; continue; }
    out[n++] = (long)num.toFloat();
  }
  return n;
}

static String jsonStrArray(const char *key, const String *items, int n) {
  String out = String("\"") + key + "\":[";
  for (int i = 0; i < n; i++) {
    if (i) out += ",";
    out += "\"" + jsonEsc(items[i]) + "\"";
  }
  out += "]";
  return out;
}
static String jsonLongArray(const char *key, const long *items, int n) {
  String out = String("\"") + key + "\":[";
  for (int i = 0; i < n; i++) {
    if (i) out += ",";
    out += String(items[i]);
  }
  out += "]";
  return out;
}

/* ---------------------------------------------------------------------------
 * 7.  STORAGE: safe atomic writes + directory bootstrap
 * ------------------------------------------------------------------------ */

/* Long SD/JSON work in setup() has to yield now and then.  The core's task
 * watchdog panics and reboots if the idle task cannot run for 5 s
 * (CONFIG_ESP_TASK_WDT_PANIC=y in the prebuilt 3.x libraries), which would
 * turn a slow first-time scan into a boot loop.  Yields at most once every
 * 50 ms, so it is cheap enough to call from inner loops. */
static void bootYield() {
  static uint32_t last = 0;
  uint32_t now = millis();
  if (!last || now - last >= 50) { last = now; delay(1); }
}

static bool storageBegin() {
  SD_MMC.setPins(SDMMC_CLK, SDMMC_CMD, SDMMC_DATA);
  g_sd_ok = SD_MMC.begin("/sdcard", true);
  return g_sd_ok;
}

/* Write to <path>.tmp, verify the byte count, keep the previous file as
 * <path>.bak, then rename.  A failed save never destroys the last good file. */
static bool atomicWrite(const String &path, const String &content) {
  if (!g_sd_ok) return false;
  String tmp = path + ".tmp";
  if (SD_MMC.exists(tmp.c_str())) SD_MMC.remove(tmp.c_str());
  File f = SD_MMC.open(tmp.c_str(), FILE_WRITE);
  if (!f) return false;
  size_t wrote = f.print(content);
  f.flush();
  f.close();
  if (wrote != content.length()) { SD_MMC.remove(tmp.c_str()); return false; }

  String bak = path + ".bak";
  if (SD_MMC.exists(bak.c_str())) SD_MMC.remove(bak.c_str());
  if (SD_MMC.exists(path.c_str())) SD_MMC.rename(path.c_str(), bak.c_str());
  if (!SD_MMC.rename(tmp.c_str(), path.c_str())) {
    /* rename failed: try to put the backup back */
    if (SD_MMC.exists(bak.c_str()) && !SD_MMC.exists(path.c_str()))
      SD_MMC.rename(bak.c_str(), path.c_str());
    return false;
  }
  return true;
}

/* Reads a config file; if the primary copy is missing or looks corrupt it
 * falls back to the .bak that atomicWrite keeps. */
static bool readConfigFile(const String &path, String &out) {
  const String candidates[2] = { path, path + ".bak" };
  for (const String &p : candidates) {
    if (!g_sd_ok || !SD_MMC.exists(p.c_str())) continue;
    File f = SD_MMC.open(p.c_str(), FILE_READ);
    if (!f) continue;
    String doc;
    doc.reserve(f.size() + 1);
    while (f.available()) { doc += (char)f.read(); bootYield(); }
    f.close();
    doc.trim();
    if (doc.length() >= 2 && doc[0] == '{' && doc[doc.length() - 1] == '}') { out = doc; return true; }
  }
  return false;
}

static void mkdirIfMissing(const char *p) {
  if (g_sd_ok && !SD_MMC.exists(p)) SD_MMC.mkdir(p);
}

static const char *BOOT_SUBDIRS[] = {
  SPOTIFY_MUSIC_DIR "/Chill", SPOTIFY_MUSIC_DIR "/Focus",
  SPOTIFY_MUSIC_DIR "/Lock In", SPOTIFY_MUSIC_DIR "/Beautiful Mix",
  SPOTIFY_DATA_DIR, SPOTIFY_CONFIG_DIR, SPOTIFY_WALL_DIR,
  SPOTIFY_APP_DIR "/Cache", SPOTIFY_ART_DIR, SPOTIFY_APP_DIR "/Logs"
};

/* Creates the whole tree + default config on first launch.  Existing user
 * files are never overwritten. */
static void ensureStorage() {
  if (!g_sd_ok) return;
  mkdirIfMissing("/WATCH");
  mkdirIfMissing(SPOTIFY_APP_DIR);
  for (size_t i = 0; i < sizeof(BOOT_SUBDIRS) / sizeof(BOOT_SUBDIRS[0]); i++) mkdirIfMissing(BOOT_SUBDIRS[i]);

  struct { const char *path; const char *body; } defaults[] = {
    { SPOTIFY_CONFIG_DIR "/settings.json",
      "{\"schema\":1,\"volume\":40,\"muted\":false,\"output\":\"speaker\","
      "\"shuffle\":true,\"repeat\":1,\"resumeLast\":true,\"rememberQueue\":true,"
      "\"autoNext\":true,\"brightness\":180,\"timeoutMs\":15000,\"useWallpaper\":false,"
      "\"wallpaperFile\":\"\",\"wallpaperDim\":50,\"artworkBg\":true}\n" },
    { SPOTIFY_CONFIG_DIR "/theme.json",
      "{\"schema\":1,\"theme\":0,\"accent\":\"1ED760\",\"bg\":\"121212\","
      "\"card\":\"202020\",\"elev\":\"2A2A2A\",\"text\":\"FFFFFF\",\"text2\":\"B3B3B3\"}\n" },
    { SPOTIFY_DATA_DIR "/library.json",
      "{\"schema\":1,\"paths\":[],\"titles\":[],\"artists\":[],\"albums\":[],\"durations\":[]}\n" },
    { SPOTIFY_DATA_DIR "/liked.json",   "{\"schema\":1,\"paths\":[]}\n" },
    { SPOTIFY_DATA_DIR "/history.json", "{\"schema\":1,\"paths\":[]}\n" },
    { SPOTIFY_DATA_DIR "/queue.json",   "{\"schema\":1,\"paths\":[],\"index\":0}\n" },
    { SPOTIFY_DATA_DIR "/stats.json",
      "{\"schema\":1,\"totalSeconds\":0,\"started\":0,\"completed\":0,\"sessions\":0,"
      "\"trackPaths\":[],\"trackCounts\":[],\"trackSeconds\":[]}\n" },
  };
  for (size_t i = 0; i < sizeof(defaults) / sizeof(defaults[0]); i++) {
    if (!SD_MMC.exists(defaults[i].path)) atomicWrite(defaults[i].path, defaults[i].body);
  }
  if (!SD_MMC.exists(SPOTIFY_APP_DIR "/Music/README.txt")) {
    atomicWrite(SPOTIFY_APP_DIR "/Music/README.txt",
      "Drop folders of MP3 files here. Each folder is one playlist.\n"
      "Ex: Music/Chill/001.mp3, Music/Chill/002.mp3\n"
      "No playlist.json is needed. Files are indexed alphabetically and the\n"
      "playlist is played in a fresh random order every time (shuffle).\n"
      "Loose MP3 files in Music/ itself become the \"Singles\" playlist.\n");
  }
  if (!SD_MMC.exists(SPOTIFY_ART_DIR "/README.txt")) {
    atomicWrite(SPOTIFY_ART_DIR "/README.txt",
      "Optional square album art, raw 16-bit little-endian RGB565.\n"
      "Name it <folder>__<file>.raw, for example Chill__001.raw\n"
      "Recommended 300x300. Missing art falls back to a themed placeholder.\n");
  }
}

/* ---------------------------------------------------------------------------
 * 8.  MUSIC LIBRARY  --  index, ID3 metadata, duration estimate
 * ------------------------------------------------------------------------ */
struct Playlist {
  String name;             /* display name (the folder name)                */
  String folder;           /* folder under Music/                            */
  std::vector<int> order;  /* indices into g_tracks, in playlist order       */
};
static std::vector<Playlist> g_playlists;

static std::vector<String> g_liked;       /* paths */
static std::vector<String> g_history;     /* paths, newest first */
static std::vector<String> g_queue;       /* paths */
static int g_queueIndex = 0;

static std::vector<String> g_statPaths;
static std::vector<uint32_t> g_statCounts;
static std::vector<uint32_t> g_statSeconds;

static const int PL_ALL = -1, PL_LIKED = -2, PL_HISTORY = -3;

static String baseName(const String &p) {
  int s = p.lastIndexOf('/');
  String n = (s >= 0) ? p.substring(s + 1) : p;
  int d = n.lastIndexOf('.');
  if (d > 0) n = n.substring(0, d);
  return n;
}
static String folderOf(const String &p) {
  int s = p.lastIndexOf('/');
  return (s >= 0) ? p.substring(0, s) : String("");
}
static String stripExt(const String &n) {
  int d = n.lastIndexOf('.');
  return (d > 0) ? n.substring(0, d) : n;
}

/* mp3 duration estimate from the first frame header.  A CBR MP3 can be timed
 * from (file size - audio start) and the bitrate; the real value is corrected
 * from the decoder once the track actually plays. */
static uint32_t estimateMp3Duration(File &f, uint32_t audioStart) {
  static const uint16_t brV1L3[16]  = { 0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 0 };
  static const uint16_t brV2L3[16]  = { 0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160, 0 };
  static const uint32_t srV1[3]     = { 44100, 48000, 32000 };
  static const uint32_t srV2[3]     = { 22050, 24000, 16000 };
  static const uint32_t srV25[3]    = { 11025, 12000, 8000 };

  uint32_t size = f.size();
  if (size < 1024 || audioStart >= size) return 0;
  if (!f.seek(audioStart)) return 0;

  uint8_t buf[4];
  for (int guard = 0; guard < 65536 && f.available() >= 4; guard++) {
    if (f.read(buf, 1) != 1) break;
    if (buf[0] != 0xFF) continue;
    if (f.read(buf + 1, 1) != 1) break;
    if ((buf[1] & 0xE0) != 0xE0) continue;              /* 11-bit sync */
    if (f.read(buf + 2, 1) != 1 || f.read(buf + 3, 1) != 1) break;
    uint8_t version = (buf[1] >> 3) & 0x03;             /* 0=2.5 2=2 3=1 */
    uint8_t layer   = (buf[1] >> 1) & 0x03;             /* 1 = layer III */
    if (version == 1 || layer != 1) continue;
    uint8_t bri = (buf[2] >> 4) & 0x0F;
    uint8_t sri = (buf[2] >> 2) & 0x03;
    if (bri == 0 || bri == 15 || sri == 3) continue;
    uint32_t rate = (version == 3) ? srV1[sri] : (version == 2 ? srV2[sri] : srV25[sri]);
    uint32_t kbps = (version == 3) ? brV1L3[bri] : brV2L3[bri];
    if (!rate || !kbps) continue;
    uint32_t audioBytes = size - audioStart;
    /* (bytes * 8) / (kbps * 1000) seconds */
    return (uint32_t)((uint64_t)audioBytes * 8ULL / ((uint64_t)kbps * 1000ULL));
  }
  return 0;
}

/* Reads a text ID3v2 frame body into out (ASCII/Latin-1 and UTF-8 only).
 * UTF-16 frames are skipped rather than mis-decoded. */
static void cleanTag(String &s) {
  while (s.length() && (uint8_t)s[s.length() - 1] < 0x20) s.remove(s.length() - 1);
  for (size_t i = 0; i < s.length(); i++) if ((uint8_t)s[i] < 0x20) s[i] = ' ';
  s.trim();
}

static bool readId3v2(File &f, Track &t, uint32_t &audioStart) {
  audioStart = 0;
  if (f.size() < 10) return false;
  if (!f.seek(0)) return false;
  uint8_t h[10];
  if (f.read(h, 10) != 10) return false;
  if (!(h[0] == 'I' && h[1] == 'D' && h[2] == '3')) return false;

  uint8_t major = h[3];
  uint32_t tagSize = ((uint32_t)(h[6] & 0x7F) << 21) | ((uint32_t)(h[7] & 0x7F) << 14) |
                     ((uint32_t)(h[8] & 0x7F) << 7) | (uint32_t)(h[9] & 0x7F);
  audioStart = 10 + tagSize;
  if (major < 3) { audioStart = 10 + tagSize; return true; }  /* v2.2: header only */
  if (tagSize < 10 || tagSize > 4 * 1024 * 1024) { audioStart = 0; return false; }

  uint32_t pos = 10;
  uint8_t fh[10];
  int frames = 0;
  while (pos + 10 <= 10 + tagSize && frames++ < 64) {
    if (!f.seek(pos)) break;
    if (f.read(fh, 10) != 10) break;
    if (fh[0] == 0) break;                                   /* padding */
    char id[5] = { (char)fh[0], (char)fh[1], (char)fh[2], (char)fh[3], 0 };
    uint32_t fsz;
    if (major >= 4) fsz = ((uint32_t)(fh[4] & 0x7F) << 21) | ((uint32_t)(fh[5] & 0x7F) << 14) |
                          ((uint32_t)(fh[6] & 0x7F) << 7) | (uint32_t)(fh[7] & 0x7F);
    else            fsz = ((uint32_t)fh[4] << 24) | ((uint32_t)fh[5] << 16) |
                          ((uint32_t)fh[6] << 8) | (uint32_t)fh[7];
    uint16_t flags = ((uint16_t)fh[8] << 8) | fh[9];
    if (fsz == 0 || fsz > 1024 * 1024) break;
    if (flags & 0x00C0) { pos += 10 + fsz; continue; }        /* compressed / encrypted */

    bool want = (!strcmp(id, "TIT2") || !strcmp(id, "TPE1") || !strcmp(id, "TALB"));
    if (want && fsz < 600) {
      uint8_t body[600];
      if (f.seek(pos + 10) && f.read(body, fsz) == (int)fsz) {
        uint8_t enc = body[0];
        String val;
        if (enc == 0 || enc == 3) {
          for (uint32_t i = 1; i < fsz; i++) {
            if (body[i] == 0) break;
            val += (char)body[i];
          }
          cleanTag(val);
          if (val.length()) {
            if (!strcmp(id, "TIT2")) t.title = val;
            else if (!strcmp(id, "TPE1")) t.artist = val;
            else t.album = val;
          }
        }
      }
    }
    pos += 10 + fsz;
  }
  return true;
}

static void indexMp3(const String &relPath) {
  if (g_trackCount >= MAX_TRACKS) return;
  String full = String(SPOTIFY_MUSIC_DIR) + "/" + relPath;
  File f = SD_MMC.open(full.c_str(), FILE_READ);
  if (!f) return;
  Track &t = g_tracks[g_trackCount];
  t.path = relPath;
  t.title = baseName(relPath);       /* filename fallback */
  t.artist = "";
  t.album = folderOf(relPath);
  t.duration = 0;
  uint32_t audioStart = 0;
  readId3v2(f, t, audioStart);
  t.duration = estimateMp3Duration(f, audioStart);
  f.close();
  g_trackCount++;
}

/* Case-insensitive ordering.  Arduino-ESP32 core 3.x dropped
 * String::compareToIgnoreCase, so fold both sides to lower case and use
 * compareTo(), which core 2.x and 3.x both have. */
static int ciCompare(const String &a, const String &b) {
  String la = a, lb = b;
  la.toLowerCase();
  lb.toLowerCase();
  return la.compareTo(lb);
}

/* Case-insensitive name sort for a small list (avoids pulling in <algorithm>). */
static void nameSort(std::vector<String> &v) {
  for (size_t i = 1; i < v.size(); i++) {
    String key = v[i];
    size_t j = i;
    while (j > 0 && ciCompare(v[j - 1], key) > 0) { v[j] = v[j - 1]; j--; }
    v[j] = key;
  }
}

/* Walks Music/ : every sub-folder is ONE playlist, loose mp3s form "Singles".
 * There is no playlist.json -- files are indexed alphabetically and the order
 * the user actually hears comes from shuffleQueue(). */
static void scanMusic() {
  g_trackCount = 0;
  g_playlists.clear();
  if (!g_sd_ok) return;
  File dir = SD_MMC.open(SPOTIFY_MUSIC_DIR);
  if (!dir || !dir.isDirectory()) return;

  std::vector<String> loose;
  File e = dir.openNextFile();
  while (e) {
    String nm = e.name();
    int s = nm.lastIndexOf('/');
    if (s >= 0) nm = nm.substring(s + 1);
    if (e.isDirectory()) {
      Playlist pl;
      pl.name = nm;
      pl.folder = nm;
      std::vector<String> files;
      File sub = SD_MMC.open((String(SPOTIFY_MUSIC_DIR) + "/" + nm).c_str());
      if (sub && sub.isDirectory()) {
        File f2 = sub.openNextFile();
        while (f2) {
          String fn = f2.name();
          int s2 = fn.lastIndexOf('/');
          if (s2 >= 0) fn = fn.substring(s2 + 1);
          String low = fn; low.toLowerCase();
          if (!f2.isDirectory() && low.endsWith(".mp3")) files.push_back(fn);
          bootYield();
          f2 = sub.openNextFile();
        }
        sub.close();
      }
      nameSort(files);
      for (size_t fi = 0; fi < files.size(); fi++) {
        int before = g_trackCount;
        indexMp3(nm + "/" + files[fi]);
        if (g_trackCount > before) pl.order.push_back(before);
        bootYield();
      }
      if (pl.order.size()) g_playlists.push_back(pl);
    } else {
      String low = nm; low.toLowerCase();
      if (low.endsWith(".mp3")) loose.push_back(nm);
    }
    bootYield();
    e = dir.openNextFile();
  }
  dir.close();

  nameSort(loose);
  if (loose.size()) {
    Playlist pl;
    pl.name = "Singles";
    pl.folder = "";
    for (size_t i = 0; i < loose.size(); i++) {
      int before = g_trackCount;
      indexMp3(loose[i]);
      if (g_trackCount > before) pl.order.push_back(before);
      bootYield();
    }
    g_playlists.insert(g_playlists.begin(), pl);
  }
}

/* ---- library index cache ------------------------------------------------ */
static void saveLibrary() {
  /* capacity check first: refuse to write a truncated index */
  if (g_trackCount > MAX_TRACKS) return;
  std::vector<String> paths(MAX_TRACKS), titles(MAX_TRACKS), artists(MAX_TRACKS), albums(MAX_TRACKS);
  std::vector<long> durations(MAX_TRACKS);
  for (int i = 0; i < g_trackCount; i++) {
    paths[i] = g_tracks[i].path;
    titles[i] = g_tracks[i].title;
    artists[i] = g_tracks[i].artist;
    albums[i] = g_tracks[i].album;
    durations[i] = (long)g_tracks[i].duration;
  }
  String doc = "{\"schema\":1,";
  doc += jsonStrArray("paths", paths.data(), g_trackCount) + ",";
  doc += jsonStrArray("titles", titles.data(), g_trackCount) + ",";
  doc += jsonStrArray("artists", artists.data(), g_trackCount) + ",";
  doc += jsonStrArray("albums", albums.data(), g_trackCount) + ",";
  doc += jsonLongArray("durations", durations.data(), g_trackCount) + "}\n";
  atomicWrite(String(SPOTIFY_DATA_DIR) + "/library.json", doc);
}

static void loadLibraryIndex() {
  String doc;
  if (!readConfigFile(String(SPOTIFY_DATA_DIR) + "/library.json", doc)) return;
  std::vector<String> paths(MAX_TRACKS), titles(MAX_TRACKS), artists(MAX_TRACKS), albums(MAX_TRACKS);
  std::vector<long> durations(MAX_TRACKS);
  int n = jsonGetStrArray(doc, "paths", paths.data(), MAX_TRACKS);
  int nt = jsonGetStrArray(doc, "titles", titles.data(), MAX_TRACKS);
  int na = jsonGetStrArray(doc, "artists", artists.data(), MAX_TRACKS);
  int nb = jsonGetStrArray(doc, "albums", albums.data(), MAX_TRACKS);
  int nd = jsonGetLongArray(doc, "durations", durations.data(), MAX_TRACKS);
  g_trackCount = 0;
  for (int i = 0; i < n && g_trackCount < MAX_TRACKS; i++) {
    if (!paths[i].length()) continue;
    Track &t = g_tracks[g_trackCount];
    t.path = paths[i];
    t.title = (i < nt && titles[i].length()) ? titles[i] : baseName(paths[i]);
    t.artist = (i < na) ? artists[i] : "";
    t.album = (i < nb) ? albums[i] : "";
    t.duration = (i < nd && durations[i] > 0) ? (uint32_t)durations[i] : 0;
    g_trackCount++;
  }
}

/* Fills playlist membership from the tracks that are actually indexed.  Run
 * after loadLibraryIndex() so cached data can be reused without a full scan. */
static void buildPlaylistsFromIndex() {
  g_playlists.clear();
  std::vector<String> folders;
  for (int i = 0; i < g_trackCount; i++) {
    String fd = folderOf(g_tracks[i].path);
    if (!fd.length()) continue;
    bool seen = false;
    for (size_t k = 0; k < folders.size(); k++) if (folders[k] == fd) { seen = true; break; }
    if (!seen) folders.push_back(fd);
  }
  for (size_t k = 0; k < folders.size() && g_playlists.size() < MAX_PLAYLISTS; k++) {
    Playlist pl;
    pl.name = folders[k];
    pl.folder = folders[k];
    for (int i = 0; i < g_trackCount; i++)
      if (folderOf(g_tracks[i].path) == folders[k]) pl.order.push_back(i);
    if (pl.order.size()) g_playlists.push_back(pl);
  }
  /* loose files -> Singles, always first */
  Playlist singles;
  singles.name = "Singles";
  singles.folder = "";
  for (int i = 0; i < g_trackCount; i++)
    if (!folderOf(g_tracks[i].path).length()) singles.order.push_back(i);
  if (singles.order.size()) g_playlists.insert(g_playlists.begin(), singles);
}

static int findTrack(const String &path) {
  for (int i = 0; i < g_trackCount; i++) if (g_tracks[i].path == path) return i;
  return -1;
}
static bool likeIndex(const String &path, int *idxOut = nullptr) {
  for (size_t i = 0; i < g_liked.size(); i++)
    if (g_liked[i] == path) { if (idxOut) *idxOut = (int)i; return true; }
  if (idxOut) *idxOut = -1;
  return false;
}
static bool isLiked(const String &path) { return likeIndex(path); }
static bool trackFileExists(const Track &t) {
  if (!g_sd_ok) return false;
  return SD_MMC.exists((String(SPOTIFY_MUSIC_DIR) + "/" + t.path).c_str());
}

/* ---------------------------------------------------------------------------
 * 9.  PERSISTENCE: likes, history, queue, statistics
 * ------------------------------------------------------------------------ */
/* The buffers below are on the heap, not on the stack.  All of this runs in
 * the core's loopTask, whose stack is ARDUINO_LOOP_STACK_SIZE -- 8192 bytes by
 * default -- so a local `String arr[512]` (512 x 16 B) is the entire stack in
 * one frame, and the core aborts with "A stack overflow in task loopTask has
 * been detected" and reboots.  On the boot path that is a boot loop nothing on
 * the card can fix.  The same rule applies to saveQueue/loadQueue,
 * saveStats/loadStats and saveLibrary/loadLibraryIndex;
 * tools/tests/stack_budget_test.py checks this whole file for it. */
static void saveLiked() {
  std::vector<String> arr(512);
  int n = 0;
  for (size_t i = 0; i < g_liked.size() && n < 512; i++) arr[n++] = g_liked[i];
  atomicWrite(String(SPOTIFY_DATA_DIR) + "/liked.json",
              "{\"schema\":1," + jsonStrArray("paths", arr.data(), n) + "}\n");
}
static void loadLiked() {
  String doc;
  g_liked.clear();
  if (!readConfigFile(String(SPOTIFY_DATA_DIR) + "/liked.json", doc)) return;
  std::vector<String> arr(512);
  int n = jsonGetStrArray(doc, "paths", arr.data(), 512);
  for (int i = 0; i < n; i++) if (arr[i].length()) g_liked.push_back(arr[i]);
}
static void toggleLike(const String &path) {
  int idx;
  if (likeIndex(path, &idx)) g_liked.erase(g_liked.begin() + idx);
  else g_liked.push_back(path);
  saveLiked();
}

static void saveHistory() {
  String arr[MAX_HISTORY];
  int n = 0;
  for (size_t i = 0; i < g_history.size() && n < MAX_HISTORY; i++) arr[n++] = g_history[i];
  atomicWrite(String(SPOTIFY_DATA_DIR) + "/history.json",
              "{\"schema\":1," + jsonStrArray("paths", arr, n) + "}\n");
}
static void loadHistory() {
  String doc;
  g_history.clear();
  if (!readConfigFile(String(SPOTIFY_DATA_DIR) + "/history.json", doc)) return;
  String arr[MAX_HISTORY];
  int n = jsonGetStrArray(doc, "paths", arr, MAX_HISTORY);
  for (int i = 0; i < n; i++) if (arr[i].length()) g_history.push_back(arr[i]);
}
static void pushHistory(const String &path) {
  for (size_t i = 0; i < g_history.size(); i++)
    if (g_history[i] == path) { g_history.erase(g_history.begin() + i); break; }
  g_history.insert(g_history.begin(), path);
  while (g_history.size() > MAX_HISTORY) g_history.pop_back();
  saveHistory();
}

static void saveQueue() {
  std::vector<String> arr(MAX_QUEUE);
  int n = 0;
  for (size_t i = 0; i < g_queue.size() && n < MAX_QUEUE; i++) arr[n++] = g_queue[i];
  String doc = "{\"schema\":1," + jsonStrArray("paths", arr.data(), n) + ",\"index\":" + String(g_queueIndex) + "}\n";
  atomicWrite(String(SPOTIFY_DATA_DIR) + "/queue.json", doc);
}
static void loadQueue() {
  String doc;
  g_queue.clear();
  g_queueIndex = 0;
  if (!readConfigFile(String(SPOTIFY_DATA_DIR) + "/queue.json", doc)) return;
  std::vector<String> arr(MAX_QUEUE);
  int n = jsonGetStrArray(doc, "paths", arr.data(), MAX_QUEUE);
  for (int i = 0; i < n; i++) if (arr[i].length()) g_queue.push_back(arr[i]);
  g_queueIndex = (int)jsonGetLong(doc, "index", 0);
  if (g_queueIndex < 0 || g_queueIndex >= (int)g_queue.size()) g_queueIndex = 0;
}
static void setQueueFromPlaylist(int plIndex, int startTrack) {
  g_queue.clear();
  if (plIndex == PL_LIKED) {
    for (size_t i = 0; i < g_liked.size(); i++) g_queue.push_back(g_liked[i]);
  } else if (plIndex == PL_HISTORY) {
    for (size_t i = 0; i < g_history.size(); i++) g_queue.push_back(g_history[i]);
  } else if (plIndex == PL_ALL) {
    for (int i = 0; i < g_trackCount; i++) g_queue.push_back(g_tracks[i].path);
  } else if (plIndex >= 0 && plIndex < (int)g_playlists.size()) {
    for (size_t i = 0; i < g_playlists[plIndex].order.size(); i++)
      g_queue.push_back(g_tracks[g_playlists[plIndex].order[i]].path);
  }
  g_queueIndex = 0;
  if (startTrack >= 0) {
    String want = g_tracks[startTrack].path;
    for (size_t i = 0; i < g_queue.size(); i++) if (g_queue[i] == want) { g_queueIndex = (int)i; break; }
  }
  saveQueue();
}

/* ---------------------------------------------------------------------------
 * The auto-shuffle.  Playlists are stored alphabetically, but every time a
 * playlist starts it gets its own random order (Fisher-Yates over the queue).
 * Sequential playback then walks that order, so nothing repeats until the
 * whole list has been played -- unlike the old "pick a random index each time"
 * which could play the same track twice in a row and never play others at all.
 * keepPath, if given, is moved to the front so "tap this song" still works.
 * ------------------------------------------------------------------------ */
static void shuffleQueue(const String &keepPath = "") {
  if (g_queue.size() < 2) return;
  for (size_t i = g_queue.size(); i > 1; i--) {
    size_t j = (size_t)(esp_random() % i);
    String tmp = g_queue[i - 1];
    g_queue[i - 1] = g_queue[j];
    g_queue[j] = tmp;
  }
  if (keepPath.length()) {
    for (size_t i = 0; i < g_queue.size(); i++) {
      if (g_queue[i] == keepPath) {
        g_queue.insert(g_queue.begin(), g_queue[i]);
        g_queue.erase(g_queue.begin() + (int)i + 1);
        break;
      }
    }
  }
  g_queueIndex = 0;
  saveQueue();
}

/* ---- statistics -------------------------------------------------------- */
static uint32_t g_statTotalSeconds = 0, g_statStarted = 0, g_statCompleted = 0, g_statSessions = 0;
static uint32_t g_sessionStartMs = 0, g_sessionSeconds = 0;
static bool g_statDirty = false;
static uint32_t g_lastStatSaveMs = 0;

static void statNotePath(const String &path, uint32_t addSeconds, bool addPlay) {
  for (size_t i = 0; i < g_statPaths.size(); i++) {
    if (g_statPaths[i] == path) {
      if (addPlay) g_statCounts[i]++;
      g_statSeconds[i] += addSeconds;
      g_statDirty = true;
      return;
    }
  }
  g_statPaths.push_back(path);
  g_statCounts.push_back(addPlay ? 1 : 0);
  g_statSeconds.push_back(addSeconds);
  g_statDirty = true;
}

static void saveStats() {
  std::vector<String> arr(256);
  std::vector<long> counts(256), secs(256);
  int n = 0;
  for (size_t i = 0; i < g_statPaths.size() && n < 256; i++) {
    arr[n] = g_statPaths[i];
    counts[n] = (long)g_statCounts[i];
    secs[n] = (long)g_statSeconds[i];
    n++;
  }
  String doc = "{\"schema\":1,\"totalSeconds\":" + String(g_statTotalSeconds) +
               ",\"started\":" + String(g_statStarted) +
               ",\"completed\":" + String(g_statCompleted) +
               ",\"sessions\":" + String(g_statSessions) + ",";
  doc += jsonStrArray("trackPaths", arr.data(), n) + ",";
  doc += jsonLongArray("trackCounts", counts.data(), n) + ",";
  doc += jsonLongArray("trackSeconds", secs.data(), n) + "}\n";
  if (atomicWrite(String(SPOTIFY_DATA_DIR) + "/stats.json", doc)) g_statDirty = false;
}
static void loadStats() {
  String doc;
  g_statPaths.clear(); g_statCounts.clear(); g_statSeconds.clear();
  if (!readConfigFile(String(SPOTIFY_DATA_DIR) + "/stats.json", doc)) return;
  g_statTotalSeconds = (uint32_t)jsonGetLong(doc, "totalSeconds", 0);
  g_statStarted = (uint32_t)jsonGetLong(doc, "started", 0);
  g_statCompleted = (uint32_t)jsonGetLong(doc, "completed", 0);
  g_statSessions = (uint32_t)jsonGetLong(doc, "sessions", 0);
  std::vector<String> paths(256);
  std::vector<long> counts(256), secs(256);
  int n = jsonGetStrArray(doc, "trackPaths", paths.data(), 256);
  int nc = jsonGetLongArray(doc, "trackCounts", counts.data(), 256);
  int ns = jsonGetLongArray(doc, "trackSeconds", secs.data(), 256);
  for (int i = 0; i < n && i < 256; i++) {
    g_statPaths.push_back(paths[i]);
    g_statCounts.push_back(i < nc ? (uint32_t)counts[i] : 0);
    g_statSeconds.push_back(i < ns ? (uint32_t)secs[i] : 0);
  }
}
static void resetStats() {
  g_statPaths.clear(); g_statCounts.clear(); g_statSeconds.clear();
  g_statTotalSeconds = g_statStarted = g_statCompleted = g_statSessions = 0;
  g_sessionSeconds = 0;
  saveStats();
}

/* ---------------------------------------------------------------------------
 * 10. SETTINGS + THEME / WALLPAPER
 * ------------------------------------------------------------------------ */
struct Settings {
  uint8_t  volume      = DEFAULT_VOLUME;
  bool     muted       = false;
  int      output      = 0;        /* 0 speaker, 1 bluetooth            */
  bool     shuffle     = true;     /* playlists are auto-shuffled by default */
  int      repeat      = 1;        /* 0 off, 1 all, 2 one               */
  bool     resumeLast  = true;
  bool     rememberQueue = true;
  bool     autoNext    = true;
  uint8_t  brightness  = DEFAULT_BRIGHTNESS;
  uint32_t timeoutMs   = SCREEN_OFF_TIMEOUT_MS;
  bool     useWallpaper = false;
  String   wallpaperFile;
  bool     artworkBg    = true;
  uint8_t  wallpaperDim = 50;      /* 0..80 % black laid over the wallpaper  */
  /* Quick Glance */
  bool     startInGlance = true;   /* open on the watch face, not the list   */
  bool     clock24h      = true;   /* 24-hour clock (off = 12-hour)          */
  bool     clockSeconds  = false;  /* seconds on the glance clock            */
  bool     glanceStatus  = true;   /* battery + output row on the glance     */
};
static Settings g_settings;
static uint8_t g_lastNonZeroVolume = DEFAULT_VOLUME;

struct ThemeConfig {
  int      theme  = 0;
  uint32_t accent = 0x1ED760;
  uint32_t bg     = 0x121212;
  uint32_t card   = 0x202020;
  uint32_t elev   = 0x2A2A2A;
  uint32_t text   = 0xFFFFFF;
  uint32_t text2  = 0xB3B3B3;
};
static ThemeConfig g_themeCfg;

static void saveSettings() {
  char buf[64];
  String doc = "{\"schema\":1";
  doc += ",\"volume\":" + String(g_settings.volume);
  doc += ",\"muted\":" + String(g_settings.muted ? "true" : "false");
  doc += ",\"output\":\"" + String(g_settings.output == 1 ? "bluetooth" : "speaker") + "\"";
  doc += ",\"shuffle\":" + String(g_settings.shuffle ? "true" : "false");
  doc += ",\"repeat\":" + String(g_settings.repeat);
  doc += ",\"resumeLast\":" + String(g_settings.resumeLast ? "true" : "false");
  doc += ",\"rememberQueue\":" + String(g_settings.rememberQueue ? "true" : "false");
  doc += ",\"autoNext\":" + String(g_settings.autoNext ? "true" : "false");
  doc += ",\"brightness\":" + String(g_settings.brightness);
  doc += ",\"timeoutMs\":" + String(g_settings.timeoutMs);
  doc += ",\"useWallpaper\":" + String(g_settings.useWallpaper ? "true" : "false");
  snprintf(buf, sizeof(buf), ",\"wallpaperFile\":\"%s\"", jsonEsc(g_settings.wallpaperFile).c_str());
  doc += buf;
  doc += ",\"wallpaperDim\":" + String(g_settings.wallpaperDim);
  doc += ",\"artworkBg\":" + String(g_settings.artworkBg ? "true" : "false");
  doc += ",\"startInGlance\":" + String(g_settings.startInGlance ? "true" : "false");
  doc += ",\"clock24h\":" + String(g_settings.clock24h ? "true" : "false");
  doc += ",\"clockSeconds\":" + String(g_settings.clockSeconds ? "true" : "false");
  doc += ",\"glanceStatus\":" + String(g_settings.glanceStatus ? "true" : "false") + "}\n";
  atomicWrite(String(SPOTIFY_CONFIG_DIR) + "/settings.json", doc);
}

static void loadSettings() {
  String doc;
  if (!readConfigFile(String(SPOTIFY_CONFIG_DIR) + "/settings.json", doc)) { saveSettings(); return; }
  long v = jsonGetLong(doc, "volume", DEFAULT_VOLUME);
  g_settings.volume = (uint8_t)constrain((int)v, 0, 100);
  if (g_settings.volume) g_lastNonZeroVolume = g_settings.volume;
  g_settings.muted = jsonGetBool(doc, "muted", false);
  g_settings.output = jsonGetStr(doc, "output", "speaker") == "bluetooth" ? 1 : 0;
  g_settings.shuffle = jsonGetBool(doc, "shuffle", true);
  g_settings.repeat = (int)constrain((int)jsonGetLong(doc, "repeat", 1), 0, 2);
  g_settings.resumeLast = jsonGetBool(doc, "resumeLast", true);
  g_settings.rememberQueue = jsonGetBool(doc, "rememberQueue", true);
  g_settings.autoNext = jsonGetBool(doc, "autoNext", true);
  g_settings.brightness = (uint8_t)constrain((int)jsonGetLong(doc, "brightness", DEFAULT_BRIGHTNESS), 20, 255);
  g_settings.timeoutMs = (uint32_t)constrain((long)jsonGetLong(doc, "timeoutMs", SCREEN_OFF_TIMEOUT_MS), 5000, 120000);
  g_settings.useWallpaper = jsonGetBool(doc, "useWallpaper", false);
  g_settings.wallpaperFile = jsonGetStr(doc, "wallpaperFile", "");
  g_settings.artworkBg = jsonGetBool(doc, "artworkBg", true);
  g_settings.wallpaperDim = (uint8_t)constrain((int)jsonGetLong(doc, "wallpaperDim", 50), 0, 80);
  g_settings.startInGlance = jsonGetBool(doc, "startInGlance", true);
  g_settings.clock24h = jsonGetBool(doc, "clock24h", true);
  g_settings.clockSeconds = jsonGetBool(doc, "clockSeconds", false);
  g_settings.glanceStatus = jsonGetBool(doc, "glanceStatus", true);
}

static uint32_t hexToColor(const String &s, uint32_t dflt) {
  if (s.length() != 6) return dflt;
  uint32_t v = 0;
  for (int i = 0; i < 6; i++) {
    char c = s[i];
    int d;
    if (c >= '0' && c <= '9') d = c - '0';
    else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
    else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
    else return dflt;
    v = (v << 4) | (uint32_t)d;
  }
  return v;
}
static String colorToHex(uint32_t c) {
  char b[8];
  snprintf(b, sizeof(b), "%06X", (unsigned)(c & 0xFFFFFF));
  return String(b);
}

static void applyTheme() {
  if (g_themeCfg.theme >= 0 && g_themeCfg.theme < THEME_COUNT) {
    g_theme = THEME_PRESETS[g_themeCfg.theme];
    /* keep the editable tokens in sync with the preset so a later custom
     * accent starts from this palette instead of a stale one */
    g_themeCfg.accent = g_theme.accent;
    g_themeCfg.bg     = g_theme.bg;
    g_themeCfg.card   = g_theme.card;
    g_themeCfg.elev   = g_theme.elev;
    g_themeCfg.text   = g_theme.text;
    g_themeCfg.text2  = g_theme.text2;
  } else {
    g_theme = THEME_PRESETS[0];
    g_theme.bg = g_themeCfg.bg;
    g_theme.card = g_themeCfg.card;
    g_theme.elev = g_themeCfg.elev;
    g_theme.text = g_themeCfg.text;
    g_theme.text2 = g_themeCfg.text2;
    g_theme.accent = g_themeCfg.accent;
  }
  g_theme.divider = g_theme.elev;
}

static void saveThemeConfig() {
  String doc = "{\"schema\":1,";
  doc += "\"theme\":" + String(g_themeCfg.theme) + ",";
  doc += "\"accent\":\"" + colorToHex(g_themeCfg.accent) + "\",";
  doc += "\"bg\":\"" + colorToHex(g_themeCfg.bg) + "\",";
  doc += "\"card\":\"" + colorToHex(g_themeCfg.card) + "\",";
  doc += "\"elev\":\"" + colorToHex(g_themeCfg.elev) + "\",";
  doc += "\"text\":\"" + colorToHex(g_themeCfg.text) + "\",";
  doc += "\"text2\":\"" + colorToHex(g_themeCfg.text2) + "\"}\n";
  atomicWrite(String(SPOTIFY_CONFIG_DIR) + "/theme.json", doc);
}
static void loadThemeConfig() {
  String doc;
  if (!readConfigFile(String(SPOTIFY_CONFIG_DIR) + "/theme.json", doc)) { applyTheme(); return; }
  g_themeCfg.theme = (int)constrain((int)jsonGetLong(doc, "theme", 0), 0, THEME_CUSTOM);
  g_themeCfg.accent = hexToColor(jsonGetStr(doc, "accent", "1ED760"), 0x1ED760);
  g_themeCfg.bg = hexToColor(jsonGetStr(doc, "bg", "121212"), 0x121212);
  g_themeCfg.card = hexToColor(jsonGetStr(doc, "card", "202020"), 0x202020);
  g_themeCfg.elev = hexToColor(jsonGetStr(doc, "elev", "2A2A2A"), 0x2A2A2A);
  g_themeCfg.text = hexToColor(jsonGetStr(doc, "text", "FFFFFF"), 0xFFFFFF);
  g_themeCfg.text2 = hexToColor(jsonGetStr(doc, "text2", "B3B3B3"), 0xB3B3B3);
  applyTheme();
}

/* ---- wallpaper: one raw RGB565 410x502 image, loaded on demand --------- */
static lv_color_t *g_wallBuf = nullptr;
static lv_img_dsc_t g_wallDsc;
static String g_wallLoaded;
static bool g_wallReady = false;

static void freeWallpaper() {
  if (g_wallBuf) { heap_caps_free(g_wallBuf); g_wallBuf = nullptr; }
  g_wallLoaded = "";
  g_wallReady = false;
}

static void loadWallpaperFile(const String &fileName) {
  if (!g_sd_ok || !fileName.length()) return;
  if (g_wallLoaded == fileName && g_wallReady) return;
  freeWallpaper();
  String path = String(SPOTIFY_WALL_DIR) + "/" + fileName;
  File f = SD_MMC.open(path.c_str(), FILE_READ);
  if (!f) { return; }                      /* missing -> keep the flat background */
  size_t need = (size_t)LCD_W * LCD_H * 2;
  if (f.size() != (int)need) { f.close(); return; }   /* reject wrong dimensions */
  lv_color_t *buf = (lv_color_t *)heap_caps_malloc(need, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!buf) buf = (lv_color_t *)heap_caps_malloc(need, MALLOC_CAP_8BIT);
  if (!buf) { f.close(); return; }
  size_t got = f.read((uint8_t *)buf, need);
  f.close();
  if (got != need) { heap_caps_free(buf); return; }
  g_wallBuf = buf;
  g_wallDsc.header.cf = LV_IMG_CF_TRUE_COLOR;
  g_wallDsc.header.always_zero = 0;
  g_wallDsc.header.w = LCD_W;
  g_wallDsc.header.h = LCD_H;
  g_wallDsc.data_size = need;
  g_wallDsc.data = (const uint8_t *)g_wallBuf;
  g_wallLoaded = fileName;
  g_wallReady = true;
}

/* ---- theme <-> wallpaper pairing -------------------------------------- */
/* Selecting a theme that owns a painting also makes that painting the
 * wallpaper, but only if the .raw file is really on the card. */
static void autoApplyThemeWallpaper(int theme) {
  if (theme < 0 || theme >= THEME_COUNT) return;
  const char *wf = THEME_WALLPAPERS[theme];
  if (!wf || !*wf || !g_sd_ok) return;
  if (!SD_MMC.exists((String(SPOTIFY_WALL_DIR) + "/" + wf).c_str())) return;
  g_settings.wallpaperFile = wf;
  g_settings.useWallpaper = true;
  g_settings.wallpaperDim = 50;
  freeWallpaper();
  saveSettings();
}

/* ...and the other way round: choosing one of the paintings as wallpaper
 * switches to the theme it was painted for. */
static void themeForWallpaper(const String &file) {
  for (int i = 0; i < THEME_COUNT; i++) {
    if (!*THEME_WALLPAPERS[i] || file != THEME_WALLPAPERS[i]) continue;
    if (g_themeCfg.theme != i) {
      g_themeCfg.theme = i;
      applyTheme();
      saveThemeConfig();
    }
    return;
  }
}

/* ---------------------------------------------------------------------------
 * 11. AUDIO OUTPUT ABSTRACTION
 *     The player never talks to the codec or the amplifier directly; it asks
 *     AudioOutputManager for the active output and checks isReady() first.
 * ------------------------------------------------------------------------ */
static Audio g_audio;                     /* the MP3 decoder + I2S engine */

class AudioOutput {
public:
  virtual ~AudioOutput() {}
  virtual const char *name()        = 0;
  virtual bool isAvailable()        = 0;   /* is the hardware present?      */
  virtual bool isReady()            = 0;   /* initialised and usable now?   */
  virtual bool initialize()         = 0;
  virtual bool setVolumePercent(uint8_t pct) = 0;
  virtual size_t writeAudio(const int16_t *frames, size_t frameCount) = 0;
  virtual void stop()               = 0;
  virtual void shutdown()           = 0;
  virtual const char *status()      = 0;
};

/* ---- Watch Speaker: ES8311 -> NS4150B -> onboard speaker ---------------- */
class WatchSpeakerOutput : public AudioOutput {
  bool m_ready = false;
  bool m_codecOk = false;
  uint8_t m_applied = 0;
  uint32_t m_failures = 0;
public:
  const char *name() override { return "Watch Speaker"; }
  bool isAvailable() override { return true; }

  bool initialize() override {
    if (m_ready) return true;
    pinMode(AMP_EN, OUTPUT);
    digitalWrite(AMP_EN, LOW);                  /* amp off while we configure */

    if (!Wire.begin(IIC_SDA, IIC_SCL, 400000)) { Serial.println("[audio] I2C begin failed"); }
    m_codecOk = esBegin();
    if (!m_codecOk) { Serial.println("[audio] ES8311 init failed"); return false; }

    g_audio.setPinout(I2S_BCLK, I2S_LRC, I2S_DOUT, I2S_MCLK);
    g_audio.setVolume(21);                      /* digital stage, no attenuation */

    applyVolume(g_settings.muted ? 0 : g_settings.volume);
    digitalWrite(AMP_EN, HIGH);                 /* enable NS4150B */
    m_ready = true;
    return true;
  }

  bool isReady() override { return m_ready && m_codecOk; }

  /* percent 0..100 -> ES8311 DAC volume register write + decoder digital gate.
   * Hard-capped at SAFE_MAX_VOLUME_PCT so the amplifier stays clean. */
  bool setVolumePercent(uint8_t pct) override {
    if (!m_ready) return false;
    if (pct > 100) pct = 100;
    applyVolume(pct);
    return true;
  }
  uint8_t appliedVolume() const { return m_applied; }

  /* The speaker backend is pull-based: ESP32-audioI2S decodes and writes the
   * I2S DMA itself, so there is no PCM hand-off here.  This method exists for
   * push-based backends (the future Bluetooth path) and reports honestly. */
  size_t writeAudio(const int16_t *, size_t) override { return 0; }

  void stop() override {
    if (!m_ready) return;
    g_audio.stopSong();
    esSetVolumePct(0);
  }
  void shutdown() override {
    g_audio.stopSong();
    esSetVolumePct(0);
    digitalWrite(AMP_EN, LOW);                  /* release the amplifier */
    m_ready = false;
  }
  const char *status() override {
    if (!m_ready) return "not initialised";
    if (m_failures) return "running (recent write errors)";
    return "ready";
  }
  uint32_t failures() const { return m_failures; }

private:
  void applyVolume(uint8_t pct) {
    uint16_t safe = (uint16_t)pct * SAFE_MAX_VOLUME_PCT / 100;
    esSetVolumePct((uint8_t)safe);              /* real codec gain */
    g_audio.setVolume(pct == 0 ? 0 : 21);       /* hard silence at 0 % */
    m_applied = pct;
  }
};

/* ---- Bluetooth Audio: real only when compatible hardware is fitted ------ */
class BluetoothAudioOutput : public AudioOutput {
  const char *m_status = "Bluetooth audio hardware is not available on this watch configuration.";
public:
  const char *name() override { return "Bluetooth Audio"; }
  bool isAvailable() override { return SPOTIFY_HAS_BT_AUDIO_HW ? true : false; }
  bool isReady() override { return false; }
  bool initialize() override { return false; }
  bool setVolumePercent(uint8_t) override { return false; }
  size_t writeAudio(const int16_t *, size_t) override { return 0; }
  void stop() override {}
  void shutdown() override {}
  const char *status() override { return m_status; }
};

/* ---- Output manager ---------------------------------------------------- */
static WatchSpeakerOutput  g_outSpeaker;
static BluetoothAudioOutput g_outBluetooth;
static AudioOutput *g_outputs[2] = { &g_outSpeaker, &g_outBluetooth };
static AudioOutput *g_activeOutput = nullptr;

static AudioOutput *outputAt(int i) { return (i == 1) ? (AudioOutput *)&g_outBluetooth : (AudioOutput *)&g_outSpeaker; }
static int outputIndexOf(AudioOutput *o) { return (o == (AudioOutput *)&g_outBluetooth) ? 1 : 0; }

/* Activates the selected output.  Returns false when it cannot be made ready
 * (for example Bluetooth without the add-on transmitter). */
static bool outputActivate(int index) {
  AudioOutput *want = outputAt(index);
  if (!want->isAvailable()) {
    if (g_activeOutput && g_activeOutput != want) g_activeOutput->shutdown();
    g_activeOutput = nullptr;
    return false;
  }
  if (g_activeOutput == want && want->isReady()) return true;
  if (g_activeOutput && g_activeOutput != want) g_activeOutput->shutdown();
  if (!want->initialize()) { g_activeOutput = nullptr; return false; }
  g_activeOutput = want;
  return true;
}

static void outputShutdownAll() {
  if (g_activeOutput) { g_activeOutput->shutdown(); g_activeOutput = nullptr; }
  g_outSpeaker.shutdown();
}

/* ---------------------------------------------------------------------------
 * 12. PLAYBACK ENGINE  (states, queue navigation, shuffle, repeat, seek)
 * ------------------------------------------------------------------------ */
enum PlaybackState { PB_STOPPED = 0, PB_PLAYING, PB_PAUSED, PB_ERROR };
static PlaybackState g_pbState = PB_STOPPED;
static int      g_currentTrack = -1;        /* index into g_tracks           */
static String   g_errorText;
static volatile bool g_eofFlag = false;
static uint32_t g_playStartMs = 0;          /* wall clock when playback began */
static uint32_t g_playedSecAtStart = 0;
static uint32_t g_lastLivePushMs = 0;
static bool     g_audioInitialised = false;

/* seek API differs between ESP32-audioI2S 3.x and 4.x */
#define SPOTIFY_AUDIO_LIB_V4 1
static bool audioSeekSeconds(uint16_t sec) {
#if SPOTIFY_AUDIO_LIB_V4
  return g_audio.setAudioPlayTime(sec);
#else
  return g_audio.setAudioPlayPosition(sec);
#endif
}

static void playerOnInfo(Audio::msg_t m) {
  if (m.e == Audio::evt_eof) g_eofFlag = true;
}

static bool playerOpenPath(const String &relPath) {
  int idx = findTrack(relPath);
  if (idx < 0) { g_errorText = "Track is not in the library index"; return false; }
  if (!trackFileExists(g_tracks[idx])) { g_errorText = "File missing on SD card"; g_pbState = PB_ERROR; return false; }
  if (!g_activeOutput || !g_activeOutput->isReady()) {
    g_errorText = "Audio output is not ready";
    g_pbState = PB_ERROR;
    return false;
  }
  String full = String(SPOTIFY_MUSIC_DIR) + "/" + relPath;
  g_eofFlag = false;
  if (!g_audio.connecttoFS(SD_MMC, full.c_str())) {
    g_errorText = "Decoder could not open this file";
    g_pbState = PB_ERROR;
    return false;
  }
  /* The decoder may pick a different sample rate than the last track; keep the
   * ES8311 clock in step or the pitch would be wrong. */
  uint32_t sr = g_audio.getSampleRate();
  if (sr && sr != g_codecRate) {
    if (!esSetSampleRate(sr)) Serial.printf("[audio] codec rejected %u Hz\n", (unsigned)sr);
  }
  g_currentTrack = idx;
  g_pbState = PB_PLAYING;
  g_playStartMs = millis();
  g_playedSecAtStart = 0;
  g_errorText = "";
  statNotePath(relPath, 0, true);
  g_statStarted++;
  if (g_tracks[idx].duration == 0) {
    uint32_t d = g_audio.getAudioFileDuration();
    if (d) g_tracks[idx].duration = d;
  }
  pushHistory(relPath);
  g_statDirty = true;
  return true;
}

static void playerPlayIndex(int trackIdx) {
  if (trackIdx < 0 || trackIdx >= g_trackCount) return;
  playerOpenPath(g_tracks[trackIdx].path);
}
static void playerPlayQueuePos(int pos) {
  if (g_queue.empty()) return;
  if (pos < 0) pos = 0;
  if (pos >= (int)g_queue.size()) pos = (int)g_queue.size() - 1;
  g_queueIndex = pos;
  playerOpenPath(g_queue[pos]);
}

static int playerPickNext(int direction) {
  if (g_queue.empty()) {
    if (g_currentTrack < 0) return -1;
    return constrain(g_currentTrack + direction, 0, g_trackCount - 1);
  }
  if (g_settings.shuffle && g_queue.size() > 1) {
    int pos = g_queueIndex + direction;
    if (pos >= (int)g_queue.size()) {
      if (direction > 0 && g_settings.repeat == 0) return -1;   /* end of the shuffle */
      shuffleQueue();                                            /* new random order   */
      pos = 0;
    }
    if (pos < 0) pos = 0;
    g_queueIndex = pos;
    return findTrack(g_queue[pos]);
  }
  int pos = g_queueIndex + direction;
  if (pos < 0) pos = 0;
  if (pos >= (int)g_queue.size()) {
    if (g_settings.repeat == 1) pos = 0;            /* repeat all */
    else pos = (int)g_queue.size() - 1;
  }
  g_queueIndex = pos;
  return findTrack(g_queue[pos]);
}

static void playerNext(bool userAction) {
  if (g_queue.empty() && g_currentTrack < 0) return;
  int dir = userAction ? 1 : 1;
  int idx = playerPickNext(dir);
  if (idx >= 0) playerPlayIndex(idx);
}

static void playerPrev() {
  if (g_audio.getAudioCurrentTime() > 3 && g_pbState == PB_PLAYING) { audioSeekSeconds(0); return; }
  int idx = playerPickNext(-1);
  if (idx >= 0) playerPlayIndex(idx);
}

static void playerToggle() {
  if (g_pbState == PB_PLAYING) {
    g_audio.pauseResume();
    g_pbState = PB_PAUSED;
    playerAccumulateSession();
  } else if (g_pbState == PB_PAUSED) {
    g_audio.pauseResume();
    g_pbState = PB_PLAYING;
    g_playStartMs = millis();
  } else if (g_pbState == PB_STOPPED || g_pbState == PB_ERROR) {
    if (g_currentTrack >= 0) playerPlayIndex(g_currentTrack);
    else if (!g_queue.empty()) playerPlayQueuePos(g_queueIndex);
  }
}

static void playerStop(bool save) {
  playerAccumulateSession();
  if (g_audioInitialised) g_audio.stopSong();
  g_pbState = PB_STOPPED;
  if (save) { saveStats(); saveSettings(); saveQueue(); }
}

/* how long the current track has actually been sounding */
static uint32_t playerElapsedSeconds() {
  if (g_pbState != PB_PLAYING) return g_playedSecAtStart;
  return g_playedSecAtStart + (millis() - g_playStartMs) / 1000;
}

static void playerAccumulateSession() {
  uint32_t elapsed = playerElapsedSeconds();
  uint32_t delta = (elapsed > g_playedSecAtStart) ? (elapsed - g_playedSecAtStart) : 0;
  if (delta) {
    g_statTotalSeconds += delta;
    g_sessionSeconds += delta;
    if (g_currentTrack >= 0) statNotePath(g_tracks[g_currentTrack].path, delta, false);
    g_statDirty = true;
  }
  g_playedSecAtStart = playerElapsedSeconds();
}

static void playerSeekFraction(float f) {
  if (g_pbState == PB_STOPPED || g_pbState == PB_ERROR) return;
  uint32_t dur = g_audio.getAudioFileDuration();
  if (!dur) return;
  if (f < 0) f = 0;
  if (f > 1) f = 1;
  audioSeekSeconds((uint16_t)(dur * f));
}

/* called every loop(): drives the decoder and reacts to state changes */
static void playerTick() {
  if (!g_audioInitialised) return;
  /* never drive I2S before the codec/amp are up (and never for an output that
   * is not actually ready, e.g. Bluetooth without the add-on transmitter) */
  if (!g_activeOutput || !g_activeOutput->isReady()) return;
  g_audio.loop();

  if (g_pbState == PB_PLAYING) {
    uint32_t elapsed = (millis() - g_playStartMs) / 1000;
    if (elapsed >= 1 && elapsed % 5 == 0 && millis() - g_lastLivePushMs > 800) {
      g_lastLivePushMs = millis();
      playerAccumulateSession();
    }
    uint32_t dur = g_audio.getAudioFileDuration();
    if (dur && g_tracks[g_currentTrack].duration != dur) g_tracks[g_currentTrack].duration = dur;

    if (g_eofFlag || !g_audio.isRunning()) {
      g_eofFlag = false;
      playerAccumulateSession();
      g_statCompleted++;
      g_statDirty = true;
      if (g_settings.repeat == 2) {                 /* repeat one */
        playerPlayIndex(g_currentTrack);
      } else if (g_settings.autoNext) {
        int before = g_queueIndex;
        playerNext(false);
        if (g_settings.repeat == 0 && g_queueIndex <= before && g_currentTrack >= 0) {
          /* end of queue with repeat off */
          playerStop(true);
        }
      } else {
        playerStop(true);
      }
    }
  }
  if (g_statDirty && millis() - g_lastStatSaveMs > 30000UL) {
    g_lastStatSaveMs = millis();
    saveStats();
  }
}

/* ---------------------------------------------------------------------------
 * 13. UI  --  helpers, navigation, screens
 *     Every screen is rebuilt from data, which is what makes an instant theme
 *     switch possible without touching the decoder or the audio output.
 * ------------------------------------------------------------------------ */
enum ViewId {
  V_HOME = 0, V_LIBRARY, V_PLAYLIST, V_NOWPLAYING, V_QUEUE,
  V_OUTPUT, V_SETTINGS, V_STATS, V_THEME, V_STORAGE, V_SEARCH,
  V_GLANCE                                  /* appended: existing ids keep their values */
};

/* user-data encoding for the single action callback */
#define UD(code, arg) ((void *)(intptr_t)((long)(code) * 1000 + (long)(arg)))
#define UD_CODE(v)    ((int)((long)(intptr_t)(v) / 1000))
#define UD_ARG(v)     ((int)((long)(intptr_t)(v) % 1000))

static lv_obj_t *g_root = nullptr;
static int   g_view = V_HOME;
static int   g_viewArg = 0;
static int   g_navView[16];
static int   g_navArg[16];
static int   g_navTop = -1;

static lv_obj_t *g_clockLbl = nullptr;
static lv_obj_t *g_battLbl = nullptr;
static lv_obj_t *g_glanceClock = nullptr;   /* Quick Glance widgets          */
static lv_obj_t *g_glanceDate = nullptr;
static lv_obj_t *g_glanceBar = nullptr;     /* progress in the mini bar      */
static lv_obj_t *g_glanceBatt = nullptr;
static lv_obj_t *g_glanceOut = nullptr;
static uint32_t  g_glanceStatusMs = 0;      /* battery is an I2C read        */
static String    g_glanceDateText;
static lv_obj_t *g_liveLbl = nullptr;        /* "now playing" body text */
static lv_obj_t *g_seekSlider = nullptr;
static lv_obj_t *g_volSlider = nullptr;
static lv_obj_t *g_volLbl = nullptr;
static lv_obj_t *g_playBtnLbl = nullptr;
static bool      g_seekDragging = false;
static lv_obj_t *g_toast = nullptr;
static uint32_t  g_toastUntil = 0;

static void uiBuild();                       /* rebuild the current view  */

/* ---- primitives -------------------------------------------------------- */
static lv_obj_t *uiLabel(lv_obj_t *parent, const char *text, const lv_font_t *font,
                         uint32_t color, lv_align_t align, int x, int y) {
  lv_obj_t *o = lv_label_create(parent);
  lv_label_set_text(o, text);
  lv_obj_set_style_text_font(o, font, 0);
  lv_obj_set_style_text_color(o, lv_color_hex(color), 0);
  if (align != (lv_align_t)0xFFFF) lv_obj_align(o, align, x, y);
  else lv_obj_set_pos(o, x, y);
  return o;
}

static lv_obj_t *uiPanel(lv_obj_t *parent, int x, int y, int w, int h, uint32_t color, int radius) {
  lv_obj_t *o = lv_obj_create(parent);
  lv_obj_set_pos(o, x, y);
  lv_obj_set_size(o, w, h);
  lv_obj_set_style_bg_color(o, lv_color_hex(color), 0);
  lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(o, 0, 0);
  lv_obj_set_style_radius(o, radius, 0);
  lv_obj_set_style_pad_all(o, 0, 0);
  lv_obj_clear_flag(o, LV_OBJ_FLAG_SCROLLABLE);
  return o;
}

static lv_obj_t *uiBtnRaw(lv_obj_t *parent, const char *text, int x, int y, int w, int h,
                          uint32_t bg, uint32_t fg, const lv_font_t *font, void *ud) {
  lv_obj_t *b = lv_btn_create(parent);
  lv_obj_set_pos(b, x, y);
  lv_obj_set_size(b, w, h);
  lv_obj_set_style_bg_color(b, lv_color_hex(bg), 0);
  lv_obj_set_style_bg_color(b, lv_color_hex(g_theme.accent), LV_STATE_PRESSED);
  lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(b, 0, 0);
  lv_obj_set_style_radius(b, h / 2 > 26 ? 20 : h / 2, 0);
  lv_obj_set_style_shadow_width(b, 0, 0);
  if (ud) lv_obj_add_event_cb(b, uiAction, LV_EVENT_CLICKED, ud);
  if (text && *text) {
    lv_obj_t *l = lv_label_create(b);
    lv_label_set_text(l, text);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(fg), 0);
    lv_obj_center(l);
  }
  return b;
}

static lv_obj_t *uiCard(lv_obj_t *parent, int x, int y, int w, int h) {
  return uiPanel(parent, x, y, w, h, g_theme.card, 18);
}

static lv_obj_t *uiHeader(const char *title) {
  lv_obj_t *hdr = uiPanel(g_root, 0, 0, LCD_W, 54, g_theme.bg, 0);
  if (g_navTop >= 0) uiBtnRaw(hdr, LV_SYMBOL_LEFT " Back", 8, 7, 92, 40, g_theme.card, g_theme.text, &lv_font_montserrat_14, UD(1, 0));
  else uiBtnRaw(hdr, LV_SYMBOL_HOME, 8, 7, 44, 40, g_theme.card, g_theme.text, &lv_font_montserrat_16, UD(2, 0));
  uiLabel(hdr, title, &lv_font_montserrat_18, g_theme.text, LV_ALIGN_TOP_MID, 0, 18);
  g_clockLbl = uiLabel(hdr, "--:--", &lv_font_montserrat_14, g_theme.text2, LV_ALIGN_RIGHT_MID, -12, 0);
  return hdr;
}

static lv_obj_t *uiScrollArea(int yTop) {
  lv_obj_t *o = lv_obj_create(g_root);
  lv_obj_set_pos(o, 0, yTop);
  lv_obj_set_size(o, LCD_W, LCD_H - yTop);
  lv_obj_set_style_bg_opa(o, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(o, 0, 0);
  lv_obj_set_style_pad_all(o, 0, 0);
  lv_obj_set_style_pad_row(o, 0, 0);
  lv_obj_set_scroll_dir(o, LV_DIR_VER);
  lv_obj_add_flag(o, LV_OBJ_FLAG_SCROLL_MOMENTUM);
  return o;
}

static void uiToast(const String &msg, uint32_t ms = 2500) {
  if (g_toast) { lv_obj_del(g_toast); g_toast = nullptr; }
  g_toast = uiPanel(g_root, 20, LCD_H - 96, LCD_W - 40, 56, g_theme.elev, 16);
  lv_obj_set_style_border_width(g_toast, 1, 0);
  lv_obj_set_style_border_color(g_toast, lv_color_hex(g_theme.accent), 0);
  lv_obj_t *l = uiLabel(g_toast, msg.c_str(), &lv_font_montserrat_14, g_theme.text, LV_ALIGN_CENTER, 0, 0);
  lv_label_set_long_mode(l, LV_LABEL_LONG_DOT);
  lv_obj_set_width(l, LCD_W - 72);
  g_toastUntil = millis() + ms;
}

/* ---- transport summary used by several screens ------------------------- */
static String currentTitle() {
  if (g_currentTrack < 0 || g_currentTrack >= g_trackCount) return String("Nothing playing");
  return g_tracks[g_currentTrack].title;
}
static String currentArtist() {
  if (g_currentTrack < 0 || g_currentTrack >= g_trackCount) return String("Pick a track from your library");
  const Track &t = g_tracks[g_currentTrack];
  return t.artist.length() ? t.artist : t.album;
}
static const char *playPauseSymbol() { return g_pbState == PB_PLAYING ? LV_SYMBOL_PAUSE : LV_SYMBOL_PLAY; }

/* ---- artwork ---------------------------------------------------------- */
static lv_color_t *g_artBuf = nullptr;
static lv_img_dsc_t g_artDsc;
static int g_artSide = 0;
static String g_artLoaded;

static void freeArtwork() {
  if (g_artBuf) { heap_caps_free(g_artBuf); g_artBuf = nullptr; }
  g_artSide = 0;
  g_artLoaded = "";
}
/* Optional raw RGB565 square art: Cache/artwork/<folder>__<file>.raw */
static bool artworkCandidate(String &out, int trackIdx) {
  if (trackIdx < 0 || trackIdx >= g_trackCount) return false;
  String folder = folderOf(g_tracks[trackIdx].path);
  if (folder.length()) folder += "__"; else folder = "";
  out = String(SPOTIFY_ART_DIR) + "/" + folder + baseName(g_tracks[trackIdx].path) + ".raw";
  return g_sd_ok && SD_MMC.exists(out.c_str());
}
static void loadArtwork(int trackIdx) {
  String path;
  if (!g_settings.artworkBg || !artworkCandidate(path, trackIdx)) { freeArtwork(); return; }
  if (g_artLoaded == path) return;
  freeArtwork();
  File f = SD_MMC.open(path.c_str(), FILE_READ);
  if (!f) return;
  size_t sz = f.size();
  size_t pixels = sz / 2;
  int side = (int)sqrtf((float)pixels);
  if (side < 32 || side > MAX_ART_PX || (size_t)side * side != pixels) { f.close(); return; }
  lv_color_t *buf = (lv_color_t *)heap_caps_malloc(sz, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!buf) { f.close(); return; }
  size_t got = f.read((uint8_t *)buf, sz);
  f.close();
  if (got != sz) { heap_caps_free(buf); return; }
  g_artBuf = buf;
  g_artSide = side;
  g_artDsc.header.cf = LV_IMG_CF_TRUE_COLOR;
  g_artDsc.header.always_zero = 0;
  g_artDsc.header.w = side;
  g_artDsc.header.h = side;
  g_artDsc.data_size = sz;
  g_artDsc.data = (const uint8_t *)g_artBuf;
  g_artLoaded = path;
}

/* draws a square artwork widget with a themed placeholder fallback */
static void drawArtwork(lv_obj_t *parent, int x, int y, int size, int trackIdx) {
  loadArtwork(trackIdx);
  if (g_artBuf && g_artSide) {
    lv_obj_t *img = lv_img_create(parent);
    lv_img_set_src(img, &g_artDsc);
    lv_img_set_zoom(img, (uint16_t)(256 * size / g_artSide));
    lv_img_set_antialias(img, true);
    lv_obj_set_pos(img, x, y);
    lv_obj_set_size(img, size, size);
    lv_obj_clear_flag(img, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_radius(img, 16, 0);
    lv_obj_set_style_clip_corner(img, true, 0);
    return;
  }
  lv_obj_t *ph = uiPanel(parent, x, y, size, size, g_theme.elev, size / 6);
  lv_obj_set_style_bg_grad_color(ph, lv_color_hex(g_theme.card), 0);
  lv_obj_set_style_bg_grad_dir(ph, LV_GRAD_DIR_VER, 0);
  uiLabel(ph, LV_SYMBOL_AUDIO, size > 120 ? &lv_font_montserrat_28 : &lv_font_montserrat_18,
          g_theme.accent, LV_ALIGN_CENTER, 0, 0);
}

/* ---- navigation ------------------------------------------------------- */
static void uiPush(int view, int arg) {
  if (g_navTop < 15) {
    g_navTop++;
    g_navView[g_navTop] = g_view;
    g_navArg[g_navTop] = g_viewArg;
  }
  g_view = view;
  g_viewArg = arg;
  uiBuild();
}
static void uiBack() {
  if (g_navTop >= 0) {
    g_view = g_navView[g_navTop];
    g_viewArg = g_navArg[g_navTop];
    g_navTop--;
  } else {
    g_view = V_HOME;
    g_viewArg = 0;
  }
  uiBuild();
}
static void uiHome() {
  g_navTop = -1;
  g_view = V_HOME;
  g_viewArg = 0;
  uiBuild();
}

/* ---------------------------------------------------------------------------
 * 14. SCREENS
 * ------------------------------------------------------------------------ */
static void buildMiniPlayer(lv_obj_t *parent, int y) {
  lv_obj_t *card = uiCard(parent, 14, y, LCD_W - 28, 92);
  lv_obj_set_style_border_width(card, 1, 0);
  lv_obj_set_style_border_color(card, lv_color_hex(g_theme.divider), 0);
  drawArtwork(card, 12, 12, 68, g_currentTrack);
  lv_obj_t *ttl = uiLabel(card, currentTitle().c_str(), &lv_font_montserrat_16, g_theme.text, LV_ALIGN_TOP_LEFT, 92, 16);
  lv_label_set_long_mode(ttl, LV_LABEL_LONG_DOT);
  lv_obj_set_width(ttl, 158);
  lv_obj_t *art = uiLabel(card, currentArtist().c_str(), &lv_font_montserrat_12, g_theme.text2, LV_ALIGN_TOP_LEFT, 92, 42);
  lv_label_set_long_mode(art, LV_LABEL_LONG_DOT);
  lv_obj_set_width(art, 158);
  uiBtnRaw(card, playPauseSymbol(), 254, 22, 48, 48, g_theme.accent, 0x101010, &lv_font_montserrat_16, UD(20, 0));
  uiBtnRaw(card, LV_SYMBOL_NEXT, 306, 22, 48, 48, g_theme.elev, g_theme.text, &lv_font_montserrat_16, UD(21, 0));
  lv_obj_t *open = uiBtnRaw(card, LV_SYMBOL_UP, 268, 70, 86, 16, g_theme.card, g_theme.text2, &lv_font_montserrat_12, UD(11, 0));
  lv_obj_set_style_bg_opa(open, LV_OPA_TRANSP, 0);
  return;
}

static void buildHome() {
  uiHeader("Spotify OS");
  lv_obj_t *s = uiScrollArea(60);

  lv_obj_t *greet = uiCard(s, UI_PAD, 0, LCD_W - 2 * UI_PAD, 62);
  char gbuf[64];
  struct tm tmv;
  String hello = "Good evening";
  if (getLocalTime(&tmv, 0)) {
    if (tmv.tm_hour < 12) hello = "Good morning";
    else if (tmv.tm_hour < 18) hello = "Good afternoon";
  }
  uiLabel(greet, hello.c_str(), &lv_font_montserrat_18, g_theme.text, LV_ALIGN_LEFT_MID, 16, -8);
  snprintf(gbuf, sizeof(gbuf), "%u track%s  -  %u playlist%s",
           (unsigned)g_trackCount, g_trackCount == 1 ? "" : "s",
           (unsigned)g_playlists.size(), g_playlists.size() == 1 ? "" : "s");
  uiLabel(greet, gbuf, &lv_font_montserrat_12, g_theme.text2, LV_ALIGN_LEFT_MID, 16, 12);
  snprintf(gbuf, sizeof(gbuf), "%s: %s", g_activeOutput ? g_activeOutput->name() : outputAt(g_settings.output)->name(),
           g_activeOutput ? g_activeOutput->status() : "unavailable");
  uiLabel(greet, gbuf, &lv_font_montserrat_12, g_activeOutput ? g_theme.accent : g_theme.warn, LV_ALIGN_RIGHT_MID, -16, 12);

  buildMiniPlayer(s, 70);

  /* Shortcuts: three across, then one full-width row.  The numbers come from
   * the shared layout tokens, and each block advances by its own height plus
   * UI_GAP, so two rows can never overlap -- checked as arithmetic by
   * tools/tests/screens_layout_test.py rather than by eye. */
  const int tileW = (LCD_W - 2 * UI_PAD - 2 * UI_GAP) / 3;      /* 122 px */
  const int rowStep = 56 + UI_GAP;
  int y = 174;
  uiLabel(s, "LIBRARY", UI_FONT_META, g_theme.accent, LV_ALIGN_TOP_LEFT, UI_PAD + 6, y);
  y += 22;
  uiBtnRaw(s, "Liked Songs", UI_PAD, y, tileW, UI_TOUCH_MIN, g_theme.elev, g_theme.text, UI_FONT_BODY, UD(41, 0));
  uiBtnRaw(s, "All Songs", UI_PAD + tileW + UI_GAP, y, tileW, UI_TOUCH_MIN, g_theme.elev, g_theme.text, UI_FONT_BODY, UD(40, 0));
  uiBtnRaw(s, LV_SYMBOL_LIST " Browse", UI_PAD + 2 * (tileW + UI_GAP), y, tileW, UI_TOUCH_MIN,
           g_theme.elev, g_theme.text, UI_FONT_BODY, UD(10, 0));
  y += UI_TOUCH_MIN + UI_GAP + 8;
  uiBtnRaw(s, LV_SYMBOL_HOME "  Quick Glance", UI_PAD, y, LCD_W - 2 * UI_PAD, UI_TOUCH_MIN,
           g_theme.elev, g_theme.text, UI_FONT_BODY, UD(124, 0));
  y += UI_TOUCH_MIN + UI_GAP + 8;
  uiBtnRaw(s, LV_SYMBOL_LIST " Queue", UI_PAD, y, tileW, UI_TOUCH_MIN, g_theme.elev, g_theme.text, UI_FONT_BODY, UD(26, 0));
  uiBtnRaw(s, LV_SYMBOL_AUDIO " Output", UI_PAD + tileW + UI_GAP, y, tileW, UI_TOUCH_MIN,
           g_theme.elev, g_theme.text, UI_FONT_BODY, UD(27, 0));
  uiBtnRaw(s, LV_SYMBOL_SETTINGS " Setup", UI_PAD + 2 * (tileW + UI_GAP), y, tileW, UI_TOUCH_MIN,
           g_theme.elev, g_theme.text, UI_FONT_BODY, UD(12, 0));
  y += UI_TOUCH_MIN + 20;

  /* recently played */
  uiLabel(s, "RECENTLY PLAYED", UI_FONT_META, g_theme.accent, LV_ALIGN_TOP_LEFT, UI_PAD + 6, y);
  y += 26;
  int shown = 0;
  for (size_t i = 0; i < g_history.size() && shown < 4; i++) {
    int ti = findTrack(g_history[i]);
    if (ti < 0) continue;
    lv_obj_t *row = uiPanel(s, UI_PAD, y, LCD_W - 2 * UI_PAD, 56, g_theme.card, UI_RADIUS_CARD);
    lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(row, uiAction, LV_EVENT_CLICKED, UD(50, ti));
    uiLabel(row, g_tracks[ti].title.c_str(), UI_FONT_BODY, g_theme.text, LV_ALIGN_LEFT_MID, UI_PAD, -11);
    uiLabel(row, g_tracks[ti].artist.length() ? g_tracks[ti].artist.c_str() : g_tracks[ti].album.c_str(),
            UI_FONT_META, g_theme.text2, LV_ALIGN_LEFT_MID, UI_PAD, 10);
    uiLabel(row, fmtTime(g_tracks[ti].duration).c_str(), UI_FONT_META, g_theme.text2, LV_ALIGN_RIGHT_MID, -UI_PAD, 0);
    y += rowStep;
    shown++;
  }
  if (!shown) {
    lv_obj_t *row = uiPanel(s, UI_PAD, y, LCD_W - 2 * UI_PAD, 56, g_theme.card, UI_RADIUS_CARD);
    uiLabel(row, "Nothing played yet - pick a track", UI_FONT_BODY, g_theme.text2, LV_ALIGN_CENTER, 0, 0);
    y += rowStep;
    shown = 1;
  }
  y += 14;

  /* playlists: the folder name, how many tracks are in it, how long it runs,
   * and a play button that starts it */
  uiLabel(s, "YOUR PLAYLISTS", UI_FONT_META, g_theme.accent, LV_ALIGN_TOP_LEFT, UI_PAD + 6, y);
  y += 26;
  if (!g_playlists.size()) {
    uiLabel(s, "Drop folders of MP3s into Music/ on the card", UI_FONT_BODY, g_theme.text2,
            LV_ALIGN_TOP_LEFT, UI_PAD + 6, y + 4);
    y += rowStep;
  }
  int pShown = 0;
  for (size_t i = 0; i < g_playlists.size() && pShown < 4; i++) {
    lv_obj_t *row = uiPanel(s, UI_PAD, y, LCD_W - 2 * UI_PAD, 56, g_theme.card, UI_RADIUS_CARD);
    lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(row, uiAction, LV_EVENT_CLICKED, UD(30, (int)i));
    lv_obj_t *nm = uiLabel(row, g_playlists[i].name.c_str(), UI_FONT_BODY, g_theme.text, LV_ALIGN_LEFT_MID, UI_PAD, -11);
    lv_label_set_long_mode(nm, LV_LABEL_LONG_DOT);
    lv_obj_set_width(nm, LCD_W - 2 * UI_PAD - 90);
    uint32_t plSecs = 0;
    for (size_t k = 0; k < g_playlists[i].order.size(); k++) {
      int ti = g_playlists[i].order[k];
      if (ti >= 0 && ti < g_trackCount) plSecs += g_tracks[ti].duration;
    }
    char pb[64];
    snprintf(pb, sizeof(pb), "%u track%s  -  %s", (unsigned)g_playlists[i].order.size(),
             g_playlists[i].order.size() == 1 ? "" : "s", fmtTime(plSecs).c_str());
    uiLabel(row, pb, UI_FONT_META, g_theme.text2, LV_ALIGN_LEFT_MID, UI_PAD, 10);
    uiBtnRaw(row, LV_SYMBOL_PLAY, LCD_W - 2 * UI_PAD - 74, 10, 60, 36,
             g_theme.elev, g_theme.accent, UI_FONT_META, UD(45, (int)i));
    y += rowStep;
    pShown++;
  }
  (void)pShown;
}

static String fmtTime(uint32_t sec) {
  char b[16];
  snprintf(b, sizeof(b), "%u:%02u", (unsigned)(sec / 60), (unsigned)(sec % 60));
  return String(b);
}

/* the currently displayed track list (playlist / all / liked / search) */
static int g_listIdx[MAX_TRACKS];
static int g_listCount = 0;
static int g_sortMode = 0;              /* 0 as-is, 1 title, 2 artist, 3 duration */

static void buildTrackList(int playlistId) {
  g_listCount = 0;
  if (playlistId == PL_ALL) {
    for (int i = 0; i < g_trackCount; i++) g_listIdx[g_listCount++] = i;
    if (g_sortMode == 1) { /* title */ }
  } else if (playlistId == PL_LIKED) {
    for (size_t i = 0; i < g_liked.size(); i++) {
      int ti = findTrack(g_liked[i]);
      if (ti >= 0) g_listIdx[g_listCount++] = ti;
    }
  } else if (playlistId == PL_HISTORY) {
    for (size_t i = 0; i < g_history.size(); i++) {
      int ti = findTrack(g_history[i]);
      if (ti >= 0) g_listIdx[g_listCount++] = ti;
    }
  } else if (playlistId >= 0 && playlistId < (int)g_playlists.size()) {
    for (size_t i = 0; i < g_playlists[playlistId].order.size(); i++)
      g_listIdx[g_listCount++] = g_playlists[playlistId].order[i];
  }
  if (g_sortMode == 0) return;
  for (int i = 1; i < g_listCount; i++) {           /* insertion sort, small N */
    int key = g_listIdx[i];
    int j = i - 1;
    while (j >= 0) {
      const Track &a = g_tracks[g_listIdx[j]];
      const Track &b = g_tracks[key];
      bool after;
      if (g_sortMode == 1) after = a.title.compareTo(b.title) > 0;
      else if (g_sortMode == 2) after = a.artist.compareTo(b.artist) > 0;
      else after = a.duration > b.duration;
      if (!after) break;
      g_listIdx[j + 1] = g_listIdx[j];
      j--;
    }
    g_listIdx[j + 1] = key;
  }
}

static void trackRow(lv_obj_t *parent, int y, int trackIdx, int tapCode) {
  const Track &t = g_tracks[trackIdx];
  bool missing = !trackFileExists(t);
  lv_obj_t *row = uiPanel(parent, 14, y, LCD_W - 28, 54, g_theme.card, 14);
  lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(row, uiAction, LV_EVENT_CLICKED, UD(tapCode, trackIdx));
  lv_obj_t *ttl = uiLabel(row, t.title.c_str(), &lv_font_montserrat_14,
                          missing ? g_theme.text2 : g_theme.text, LV_ALIGN_LEFT_MID, 14, -9);
  lv_label_set_long_mode(ttl, LV_LABEL_LONG_DOT);
  lv_obj_set_width(ttl, 240);
  String sub = t.artist.length() ? t.artist : t.album;
  if (missing) sub += "  (file missing)";
  uiLabel(row, sub.c_str(), &lv_font_montserrat_12, missing ? g_theme.error : g_theme.text2, LV_ALIGN_LEFT_MID, 14, 12);
  uiLabel(row, fmtTime(t.duration).c_str(), &lv_font_montserrat_12, g_theme.text2, LV_ALIGN_RIGHT_MID, -54, 0);
  if (isLiked(t.path)) uiLabel(row, LV_SYMBOL_OK, &lv_font_montserrat_14, g_theme.accent, LV_ALIGN_RIGHT_MID, -16, 0);
}

static void buildLibrary() {
  uiHeader("Library");
  lv_obj_t *s = uiScrollArea(60);
  int y = 2;
  /* Every row says how much is really there; the counts come from the index,
   * never from a guess.  Rows step by height + UI_GAP, all on the tokens. */
  const char *names[5] = { "All Songs", "Liked Songs", "Recently Played", "Search", "Rescan library" };
  const int codes[5] = { 40, 41, 42, 43, 92 };
  char cnt[64];
  for (int i = 0; i < 5; i++) {
    lv_obj_t *row = uiPanel(s, UI_PAD, y, LCD_W - 2 * UI_PAD, 56, g_theme.card, UI_RADIUS_CARD);
    lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(row, uiAction, LV_EVENT_CLICKED, UD(codes[i], 0));
    uiLabel(row, names[i], UI_FONT_TITLE, g_theme.text, LV_ALIGN_LEFT_MID, UI_PAD, -11);
    if (i == 0)      snprintf(cnt, sizeof(cnt), "%d song%s", g_trackCount, g_trackCount == 1 ? "" : "s");
    else if (i == 1) snprintf(cnt, sizeof(cnt), "%u liked", (unsigned)g_liked.size());
    else if (i == 2) snprintf(cnt, sizeof(cnt), "%u in history", (unsigned)g_history.size());
    else if (i == 3) snprintf(cnt, sizeof(cnt), "find a title or artist");
    else             snprintf(cnt, sizeof(cnt), "%u playlist%s on the card", (unsigned)g_playlists.size(),
                              g_playlists.size() == 1 ? "" : "s");
    uiLabel(row, cnt, UI_FONT_META, g_theme.text2, LV_ALIGN_LEFT_MID, UI_PAD, 10);
    y += 64;
  }
  snprintf(cnt, sizeof(cnt), "%d song%s indexed", g_trackCount, g_trackCount == 1 ? "" : "s");
  uiLabel(s, cnt, UI_FONT_META, g_theme.text2, LV_ALIGN_TOP_LEFT, UI_PAD + 6, y + 4);
  y += 30;
  uiLabel(s, "YOUR PLAYLISTS", UI_FONT_META, g_theme.accent, LV_ALIGN_TOP_LEFT, UI_PAD + 6, y);
  y += 26;
  if (!g_playlists.size()) {
    uiLabel(s, "No folders under Music/ yet.", UI_FONT_BODY, g_theme.text2, LV_ALIGN_TOP_LEFT, UI_PAD + 6, y);
    return;
  }
  for (size_t i = 0; i < g_playlists.size(); i++) {
    lv_obj_t *row = uiPanel(s, UI_PAD, y, LCD_W - 2 * UI_PAD, 56, g_theme.card, UI_RADIUS_CARD);
    lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(row, uiAction, LV_EVENT_CLICKED, UD(44, (int)i));
    lv_obj_t *nm = uiLabel(row, g_playlists[i].name.c_str(), UI_FONT_TITLE, g_theme.text, LV_ALIGN_LEFT_MID, UI_PAD, -11);
    lv_label_set_long_mode(nm, LV_LABEL_LONG_DOT);
    lv_obj_set_width(nm, LCD_W - 2 * UI_PAD - 90);
    snprintf(cnt, sizeof(cnt), "%u track%s", (unsigned)g_playlists[i].order.size(),
             g_playlists[i].order.size() == 1 ? "" : "s");
    uiLabel(row, cnt, UI_FONT_META, g_theme.text2, LV_ALIGN_LEFT_MID, UI_PAD, 10);
    uiBtnRaw(row, LV_SYMBOL_PLAY, LCD_W - 2 * UI_PAD - 74, 10, 60, 36,
             g_theme.elev, g_theme.accent, UI_FONT_META, UD(45, (int)i));
    y += 64;
  }
}

static void buildPlaylist() {
  int plId = g_viewArg;
  const char *title = (plId == PL_ALL) ? "All Songs" : (plId == PL_LIKED) ? "Liked Songs"
                      : (plId == PL_HISTORY) ? "Recently Played"
                      : (plId >= 0 && plId < (int)g_playlists.size()) ? g_playlists[plId].name.c_str() : "Playlist";
  uiHeader(title);
  buildTrackList(plId);
  lv_obj_t *s = uiScrollArea(60);

  int y = 4;
  /* Header card: what this list is, how big it is, how long it runs, and the
   * source it will actually play from. */
  lv_obj_t *head = uiCard(s, UI_PAD, y, LCD_W - 2 * UI_PAD, 92);
  lv_obj_t *art = uiPanel(head, 12, 12, 68, 68, g_theme.elev, 14);
  drawArtwork(art, 0, 0, 68, g_listCount ? g_listIdx[0] : -1);
  lv_obj_t *hTitle = uiLabel(head, title, UI_FONT_TITLE, g_theme.text, LV_ALIGN_TOP_LEFT, 92, 14);
  lv_label_set_long_mode(hTitle, LV_LABEL_LONG_DOT);
  lv_obj_set_width(hTitle, LCD_W - 2 * UI_PAD - 104);
  uint32_t listSecs = 0;
  for (int i = 0; i < g_listCount; i++) listSecs += g_tracks[g_listIdx[i]].duration;
  char hbuf[64];
  snprintf(hbuf, sizeof(hbuf), "%d track%s  -  %s", g_listCount, g_listCount == 1 ? "" : "s",
           fmtTime(listSecs).c_str());
  uiLabel(head, hbuf, UI_FONT_META, g_theme.text2, LV_ALIGN_TOP_LEFT, 92, 40);
  lv_obj_t *srcChip = uiPanel(head, 92, 58, 78, 22, g_theme.elev, 11);
  uiLabel(srcChip, "WATCH", UI_FONT_META, g_theme.accent, LV_ALIGN_CENTER, 0, 0);
  y += 102;

  uiBtnRaw(s, LV_SYMBOL_PLAY "  Play", UI_PAD, y, 118, UI_TOUCH_MIN, g_theme.accent, 0x101010, UI_FONT_BODY, UD(46, plId));
  uiBtnRaw(s, LV_SYMBOL_SHUFFLE "  Shuffle", 140, y, 130, UI_TOUCH_MIN,
           g_settings.shuffle ? g_theme.accent : g_theme.elev,
           g_settings.shuffle ? 0x101010 : g_theme.text, UI_FONT_BODY, UD(47, plId));
  const char *sortName = g_sortMode == 0 ? "Order" : g_sortMode == 1 ? "Title" : g_sortMode == 2 ? "Artist" : "Length";
  uiBtnRaw(s, sortName, 278, y, 118, UI_TOUCH_MIN, g_theme.elev, g_theme.text, UI_FONT_BODY, UD(48, 0));
  y += UI_TOUCH_MIN + 12;

  if (!g_listCount) {
    uiLabel(s, "This list is empty. Add MP3 files to Music/ and rescan.", &lv_font_montserrat_14,
            g_theme.text2, LV_ALIGN_TOP_LEFT, 20, y + 10);
    return;
  }
  for (int i = 0; i < g_listCount; i++) {
    trackRow(s, y, g_listIdx[i], 50);
    y += 62;
  }
}

static void buildNowPlaying() {
  uiHeader("Now Playing");
  lv_obj_t *s = uiScrollArea(60);
  int artSize = 260;
  lv_obj_t *artCard = uiPanel(s, (LCD_W - artSize) / 2, 4, artSize, artSize, g_theme.card, 20);
  drawArtwork(artCard, 0, 0, artSize, g_currentTrack);

  lv_obj_t *ttl = uiLabel(s, currentTitle().c_str(), &lv_font_montserrat_18, g_theme.text, LV_ALIGN_TOP_LEFT, 22, artSize + 16);
  lv_label_set_long_mode(ttl, LV_LABEL_LONG_DOT);
  lv_obj_set_width(ttl, LCD_W - 44);
  String sub = currentArtist();
  if (g_currentTrack >= 0 && g_currentTrack < g_trackCount && g_tracks[g_currentTrack].album.length())
    sub += "  -  " + g_tracks[g_currentTrack].album;
  lv_obj_t *subL = uiLabel(s, sub.c_str(), &lv_font_montserrat_14, g_theme.text2, LV_ALIGN_TOP_LEFT, 22, artSize + 44);
  lv_label_set_long_mode(subL, LV_LABEL_LONG_DOT);
  lv_obj_set_width(subL, LCD_W - 44);

  int y = artSize + 74;
  g_seekSlider = lv_slider_create(s);
  lv_slider_set_range(g_seekSlider, 0, 1000);
  lv_obj_set_pos(g_seekSlider, 22, y);
  lv_obj_set_size(g_seekSlider, LCD_W - 44, 14);
  lv_obj_set_style_bg_color(g_seekSlider, lv_color_hex(g_theme.elev), LV_PART_MAIN);
  lv_obj_set_style_bg_color(g_seekSlider, lv_color_hex(g_theme.accent), LV_PART_INDICATOR);
  lv_obj_set_style_bg_color(g_seekSlider, lv_color_hex(g_theme.text), LV_PART_KNOB);
  lv_obj_add_event_cb(g_seekSlider, uiAction, LV_EVENT_VALUE_CHANGED, UD(70, 0));
  lv_obj_add_event_cb(g_seekSlider, uiAction, LV_EVENT_RELEASED, UD(71, 0));
  g_liveLbl = uiLabel(s, "0:00 / 0:00", &lv_font_montserrat_12, g_theme.text2, LV_ALIGN_TOP_LEFT, 22, y + 18);
  {
    uint32_t totalDur = (g_currentTrack >= 0 && g_currentTrack < g_trackCount) ? g_tracks[g_currentTrack].duration : 0;
    uiLabel(s, fmtTime(totalDur).c_str(), &lv_font_montserrat_12, g_theme.text2, LV_ALIGN_TOP_RIGHT, -22, y + 18);
  }

  /* transport */
  y += 44;
  uiBtnRaw(s, LV_SYMBOL_PREV, 46, y, 70, 70, g_theme.elev, g_theme.text, &lv_font_montserrat_22, UD(22, 0));
  uiBtnRaw(s, playPauseSymbol(), 170, y, 70, 70, g_theme.accent, 0x101010, &lv_font_montserrat_28, UD(20, 0));
  uiBtnRaw(s, LV_SYMBOL_NEXT, 294, y, 70, 70, g_theme.elev, g_theme.text, &lv_font_montserrat_22, UD(21, 0));

  /* secondary row */
  y += 82;
  uiBtnRaw(s, LV_SYMBOL_SHUFFLE, 14, y, 60, 46, g_settings.shuffle ? g_theme.accent : g_theme.elev,
           g_settings.shuffle ? 0x101010 : g_theme.text, &lv_font_montserrat_14, UD(23, 0));
  uiBtnRaw(s, LV_SYMBOL_LOOP, 82, y, 60, 46, g_settings.repeat ? g_theme.accent : g_theme.elev,
           g_settings.repeat ? 0x101010 : g_theme.text, &lv_font_montserrat_14, UD(24, 0));
  const char *lbl = (g_currentTrack >= 0 && g_currentTrack < g_trackCount && isLiked(g_tracks[g_currentTrack].path))
                    ? LV_SYMBOL_OK : LV_SYMBOL_PLUS;
  uiBtnRaw(s, lbl, 150, y, 60, 46, g_theme.elev, g_theme.accent, &lv_font_montserrat_14, UD(25, 0));
  uiBtnRaw(s, LV_SYMBOL_LIST, 218, y, 60, 46, g_theme.elev, g_theme.text, &lv_font_montserrat_14, UD(26, 0));
  uiBtnRaw(s, LV_SYMBOL_AUDIO, 286, y, 60, 46, g_theme.elev, g_theme.text, &lv_font_montserrat_14, UD(27, 0));
  uiBtnRaw(s, LV_SYMBOL_SETTINGS, 354, y, 42, 46, g_theme.elev, g_theme.text, &lv_font_montserrat_14, UD(12, 0));

  /* volume */
  y += 62;
  uiLabel(s, LV_SYMBOL_VOLUME_MAX, &lv_font_montserrat_14, g_theme.text2, LV_ALIGN_TOP_LEFT, 22, y + 4);
  g_volLbl = uiLabel(s, "", &lv_font_montserrat_14, g_theme.text, LV_ALIGN_TOP_RIGHT, -22, y);
  g_volSlider = lv_slider_create(s);
  lv_slider_set_range(g_volSlider, 0, 100);
  lv_slider_set_value(g_volSlider, g_settings.muted ? 0 : g_settings.volume, LV_ANIM_OFF);
  lv_obj_set_pos(g_volSlider, 22, y + 26);
  lv_obj_set_size(g_volSlider, LCD_W - 100, 14);
  lv_obj_set_style_bg_color(g_volSlider, lv_color_hex(g_theme.elev), LV_PART_MAIN);
  lv_obj_set_style_bg_color(g_volSlider, lv_color_hex(g_theme.accent), LV_PART_INDICATOR);
  lv_obj_set_style_bg_color(g_volSlider, lv_color_hex(g_theme.text), LV_PART_KNOB);
  lv_obj_add_event_cb(g_volSlider, uiAction, LV_EVENT_VALUE_CHANGED, UD(72, 0));
  lv_obj_add_event_cb(g_volSlider, uiAction, LV_EVENT_RELEASED, UD(73, 0));
  uiBtnRaw(s, g_settings.muted ? LV_SYMBOL_MUTE : LV_SYMBOL_VOLUME_MAX, LCD_W - 72, y + 14, 46, 38,
           g_settings.muted ? g_theme.accent : g_theme.elev, g_settings.muted ? 0x101010 : g_theme.text,
           &lv_font_montserrat_14, UD(74, 0));

  y += 54;
  /* Source indicator.  SD-card playback is the only source that exists today,
   * so this reads WATCH; when the BLE phone remote lands, PHONE will flip this
   * chip and the transport above will drive the remote instead of the decoder.
   * It never claims a source the watch is not actually playing from. */
  lv_obj_t *srcChip = uiPanel(s, 22, y, 78, 24, g_theme.elev, 12);
  uiLabel(srcChip, "WATCH", &lv_font_montserrat_12, g_theme.accent, LV_ALIGN_CENTER, 0, 0);
  char ob[80];
  snprintf(ob, sizeof(ob), "Output: %s  -  %s",
           g_activeOutput ? g_activeOutput->name() : outputAt(g_settings.output)->name(),
           g_activeOutput ? g_activeOutput->status() : outputAt(g_settings.output)->status());
  uiLabel(s, ob, &lv_font_montserrat_12, g_activeOutput ? g_theme.accent : g_theme.warn, LV_ALIGN_TOP_LEFT, 108, y + 5);
  if (g_pbState == PB_ERROR) uiLabel(s, g_errorText.c_str(), &lv_font_montserrat_12, g_theme.error, LV_ALIGN_TOP_LEFT, 22, y + 32);
}

/* ---- Quick Glance ------------------------------------------------------ *
 * The watch face the app opens on.  Every value on it is live state: the clock
 * is the system time the launcher persists in NVS (namespace "rtcsave", key
 * "t" -- the same convention LauncherOS writes and reads), the bar follows the
 * decoder's own position, the battery comes from the PMU.  With no time set it
 * reads "--:--" and with nothing playing the bar is simply absent rather than
 * an empty card.  Nothing here is mocked.
 *
 * Touch regions: the area above the sheet is one "open Spotify OS" surface (the
 * clock, date and status labels carry the same handler, so whichever object
 * takes the hit the result is the same), the sheet body opens Now Playing and
 * its two buttons take their own taps.  No two regions overlap and LVGL click
 * events do not bubble, so one tap can never fire two actions. */
static void glanceScrim(int y, int h, int fromOpa, int toOpa, int bands) {
  /* LVGL gradients carry no alpha, so a scrim is a stack of thin bands whose
   * opacity ramps.  Built once per view change, never animated. */
  if (bands < 2) bands = 2;
  int bh = (h + bands - 1) / bands;
  for (int i = 0; i < bands; i++) {
    lv_obj_t *b = uiPanel(g_root, 0, y + i * bh, LCD_W, bh, 0x000000, 0);
    int opa = fromOpa + (toOpa - fromOpa) * i / (bands - 1);
    if (opa < 0) opa = 0;
    if (opa > 100) opa = 100;
    lv_obj_set_style_bg_opa(b, (lv_opa_t)(opa * 255 / 100), 0);
  }
}

static void buildGlance() {
  const bool showBar = (g_currentTrack >= 0 && g_currentTrack < g_trackCount) || g_pbState != PB_STOPPED;
  const int tapH = showBar ? LCD_H - UI_GLANCE_BAR_H : LCD_H;

  /* scrims first, so the tap surface below covers them */
  glanceScrim(0, 178, 80, 0, 9);
  if (showBar) glanceScrim(LCD_H - UI_GLANCE_BAR_H - 36, 36, 0, 60, 5);

  lv_obj_t *tap = uiPanel(g_root, 0, 0, LCD_W, tapH, 0x000000, 0);
  lv_obj_set_style_bg_opa(tap, LV_OPA_TRANSP, 0);
  lv_obj_add_event_cb(tap, uiAction, LV_EVENT_CLICKED, UD(125, 0));

  if (g_settings.glanceStatus) {
    if (g_has_pmu) {
      int pct = g_pmu.getBatteryPercent();
      if (pct >= 0) {
        char b[28];
        snprintf(b, sizeof(b), "%d%%%s", pct, g_pmu.isCharging() ? "  charging" : "");
        g_glanceBatt = uiLabel(g_root, b, UI_FONT_META, g_theme.text2, LV_ALIGN_TOP_LEFT, UI_PAD + 4, 14);
        lv_obj_add_flag(g_glanceBatt, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(g_glanceBatt, uiAction, LV_EVENT_CLICKED, UD(125, 0));
      }
    }
    g_glanceOut = uiLabel(g_root, g_activeOutput ? g_activeOutput->name() : outputAt(g_settings.output)->name(),
                          UI_FONT_META, g_theme.text2, LV_ALIGN_TOP_RIGHT, -(UI_PAD + 4), 14);
    lv_obj_add_flag(g_glanceOut, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(g_glanceOut, uiAction, LV_EVENT_CLICKED, UD(125, 0));
  }

  g_glanceDate = uiLabel(g_root, g_glanceDateText.length() ? g_glanceDateText.c_str() : "",
                         UI_FONT_META, g_theme.text2, LV_ALIGN_TOP_MID, 0, 52);
  lv_obj_add_flag(g_glanceDate, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(g_glanceDate, uiAction, LV_EVENT_CLICKED, UD(125, 0));

  g_glanceClock = uiLabel(g_root, "--:--", UI_FONT_CLOCK, g_theme.text, LV_ALIGN_TOP_MID, 0, 72);
  lv_obj_add_flag(g_glanceClock, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(g_glanceClock, uiAction, LV_EVENT_CLICKED, UD(125, 0));

  if (!showBar) {
    lv_obj_t *hint = uiLabel(g_root, "Tap to open Spotify OS", UI_FONT_META, g_theme.text2,
                             LV_ALIGN_BOTTOM_MID, 0, -44);
    lv_obj_add_flag(hint, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(hint, uiAction, LV_EVENT_CLICKED, UD(125, 0));
    return;
  }

  lv_obj_t *bar = uiPanel(g_root, 0, LCD_H - UI_GLANCE_BAR_H, LCD_W, UI_GLANCE_BAR_H, g_theme.card, 0);
  lv_obj_set_style_bg_opa(bar, LV_OPA_90, 0);
  lv_obj_set_style_border_width(bar, 1, 0);
  lv_obj_set_style_border_side(bar, LV_BORDER_SIDE_TOP, 0);
  lv_obj_set_style_border_color(bar, lv_color_hex(g_theme.divider), 0);
  lv_obj_add_event_cb(bar, uiAction, LV_EVENT_CLICKED, UD(11, 0));      /* body -> Now Playing */

  drawArtwork(bar, UI_PAD + 2, (UI_GLANCE_BAR_H - UI_GLANCE_ART) / 2, UI_GLANCE_ART, g_currentTrack);

  const int colX = UI_PAD + 2 + UI_GLANCE_ART + 12;
  lv_obj_t *ttl = uiLabel(bar, currentTitle().c_str(), &lv_font_montserrat_16, g_theme.text,
                          LV_ALIGN_TOP_LEFT, colX, 24);
  lv_label_set_long_mode(ttl, LV_LABEL_LONG_DOT);
  lv_obj_set_width(ttl, UI_GLANCE_COL_W);
  lv_obj_t *art = uiLabel(bar, currentArtist().c_str(), UI_FONT_META, g_theme.text2, LV_ALIGN_TOP_LEFT, colX, 48);
  lv_label_set_long_mode(art, LV_LABEL_LONG_DOT);
  lv_obj_set_width(art, UI_GLANCE_COL_W);

  g_glanceBar = lv_bar_create(bar);
  lv_bar_set_range(g_glanceBar, 0, 1000);
  lv_bar_set_value(g_glanceBar, 0, LV_ANIM_OFF);
  lv_obj_set_pos(g_glanceBar, colX, 74);
  lv_obj_set_size(g_glanceBar, UI_GLANCE_COL_W, 4);
  lv_obj_set_style_radius(g_glanceBar, 2, LV_PART_MAIN);
  lv_obj_set_style_radius(g_glanceBar, 2, LV_PART_INDICATOR);
  lv_obj_set_style_bg_color(g_glanceBar, lv_color_hex(g_theme.elev), LV_PART_MAIN);
  lv_obj_set_style_bg_color(g_glanceBar, lv_color_hex(g_theme.accent), LV_PART_INDICATOR);

  const int btnY = (UI_GLANCE_BAR_H - UI_GLANCE_BTN) / 2;
  uiBtnRaw(bar, playPauseSymbol(), LCD_W - 2 * UI_GLANCE_BTN - UI_GAP - UI_PAD, btnY,
           UI_GLANCE_BTN, UI_GLANCE_BTN, g_theme.accent, 0x101010, &lv_font_montserrat_18, UD(20, 0));
  uiBtnRaw(bar, LV_SYMBOL_NEXT, LCD_W - UI_GLANCE_BTN - UI_PAD, btnY,
           UI_GLANCE_BTN, UI_GLANCE_BTN, g_theme.elev, g_theme.text, &lv_font_montserrat_18, UD(21, 0));
}

static void buildQueue() {
  uiHeader("Queue");
  lv_obj_t *s = uiScrollArea(60);
  int y = 4;
  uiBtnRaw(s, LV_SYMBOL_TRASH "  Clear", 14, y, 120, 42, g_theme.elev, g_theme.text, &lv_font_montserrat_14, UD(52, 0));
  char b[48];
  snprintf(b, sizeof(b), "%u track%s", (unsigned)g_queue.size(), g_queue.size() == 1 ? "" : "s");
  uiLabel(s, b, &lv_font_montserrat_12, g_theme.text2, LV_ALIGN_TOP_RIGHT, -20, y + 12);
  y += 52;
  if (g_queue.empty()) {
    uiLabel(s, "The queue is empty. Play a playlist to fill it.", &lv_font_montserrat_14, g_theme.text2, LV_ALIGN_TOP_LEFT, 20, y + 8);
    return;
  }
  for (size_t i = 0; i < g_queue.size(); i++) {
    int ti = findTrack(g_queue[i]);
    if (ti < 0) continue;
    bool cur = (ti == g_currentTrack);
    lv_obj_t *row = uiPanel(s, 14, y, LCD_W - 28, 52, cur ? g_theme.elev : g_theme.card, 14);
    lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(row, uiAction, LV_EVENT_CLICKED, UD(53, (int)i));
    uiLabel(row, g_tracks[ti].title.c_str(), &lv_font_montserrat_14, cur ? g_theme.accent : g_theme.text, LV_ALIGN_LEFT_MID, 14, -8);
    uiLabel(row, g_tracks[ti].artist.length() ? g_tracks[ti].artist.c_str() : g_tracks[ti].album.c_str(),
            &lv_font_montserrat_12, g_theme.text2, LV_ALIGN_LEFT_MID, 14, 11);
    uiLabel(row, fmtTime(g_tracks[ti].duration).c_str(), &lv_font_montserrat_12, g_theme.text2, LV_ALIGN_RIGHT_MID, -16, 0);
    y += 60;
  }
}

/* ---- modal dialog (own widget: no dependency on lv_msgbox version) ------ */
static lv_obj_t *g_modal = nullptr;
static void uiCloseModal() {
  if (g_modal) { lv_obj_del(g_modal); g_modal = nullptr; }
}
static void uiModal(const char *title, const char *body, const char *okText, void *okUd,
                    const char *cancelText, void *cancelUd) {
  uiCloseModal();
  g_modal = uiPanel(lv_layer_top(), 0, 0, LCD_W, LCD_H, 0x000000, 0);
  lv_obj_set_style_bg_opa(g_modal, LV_OPA_70, 0);
  lv_obj_t *box = uiPanel(g_modal, 22, 130, LCD_W - 44, 226, g_theme.elev, 20);
  lv_obj_set_style_border_width(box, 1, 0);
  lv_obj_set_style_border_color(box, lv_color_hex(g_theme.accent), 0);
  lv_obj_t *t = uiLabel(box, title, &lv_font_montserrat_18, g_theme.text, LV_ALIGN_TOP_MID, 0, 18);
  lv_obj_t *b = uiLabel(box, body, &lv_font_montserrat_14, g_theme.text2, LV_ALIGN_TOP_LEFT, 18, 56);
  lv_obj_set_width(b, LCD_W - 84);
  lv_label_set_long_mode(b, LV_LABEL_LONG_WRAP);
  if (cancelText && *cancelText) uiBtnRaw(box, cancelText, 18, 168, 160, 46, g_theme.card, g_theme.text, &lv_font_montserrat_14, cancelUd);
  uiBtnRaw(box, okText, cancelText && *cancelText ? 194 : 60, 168, 160, 46,
           g_theme.accent, 0x101010, &lv_font_montserrat_14, okUd);
  (void)t;
}

/* ---- audio output screen + switching ----------------------------------- */
static uint32_t g_resumeAtSec = 0;
static bool     g_hadPlayback = false;

static void switchOutput(int index) {
  if (index == g_settings.output && g_activeOutput && outputIndexOf(g_activeOutput) == index) {
    uiToast("Already using this output");
    return;
  }
  g_hadPlayback = (g_pbState == PB_PLAYING || g_pbState == PB_PAUSED);
  g_resumeAtSec = playerElapsedSeconds();

  /* safe order: pause -> stop writing -> release -> init new -> verify */
  if (g_pbState == PB_PLAYING) { playerAccumulateSession(); g_audio.stopSong(); g_pbState = PB_PAUSED; }

  g_settings.output = index;
  saveSettings();

  if (!outputActivate(index)) {
    g_pbState = PB_STOPPED;
    uiBuild();
    AudioOutput *o = outputAt(index);
    String body = String(o->status());
    if (index == 1) body += "\nPlayback is disabled until an output is available.";
    else body += "\nCheck the ES8311 wiring / SD card.";
    uiModal("OUTPUT UNAVAILABLE", body.c_str(), "Use Watch Speaker", UD(60, 0), "Close", UD(104, 0));
    return;
  }
  uiBuild();
  uiToast(String("Output: ") + g_activeOutput->name());
  if (g_hadPlayback) {
    uiModal("OUTPUT SWITCHED", "Playback was paused. Resume from where it stopped?",
            "Resume", UD(103, 0), "Stay paused", UD(104, 0));
  }
}

static void buildOutput() {
  uiHeader("Audio Output");
  lv_obj_t *s = uiScrollArea(60);
  int y = 6;
  for (int i = 0; i < 2; i++) {
    AudioOutput *o = outputAt(i);
    bool sel = (i == g_settings.output);
    bool avail = o->isAvailable();
    lv_obj_t *card = uiPanel(s, 14, y, LCD_W - 28, 96, sel ? g_theme.elev : g_theme.card, 18);
    lv_obj_set_style_border_width(card, sel ? 2 : 1, 0);
    lv_obj_set_style_border_color(card, lv_color_hex(sel ? g_theme.accent : g_theme.divider), 0);
    uiLabel(card, o->name(), &lv_font_montserrat_16, g_theme.text, LV_ALIGN_TOP_LEFT, 18, 14);
    const char *state = !avail ? "HARDWARE NOT AVAILABLE" : (o->isReady() ? "READY" : "AVAILABLE - NOT ACTIVE");
    uiLabel(card, state, &lv_font_montserrat_12, !avail ? g_theme.error : (o->isReady() ? g_theme.accent : g_theme.warn),
            LV_ALIGN_TOP_LEFT, 18, 40);
    lv_obj_t *st = uiLabel(card, o->status(), &lv_font_montserrat_12, g_theme.text2, LV_ALIGN_TOP_LEFT, 18, 60);
    lv_obj_set_width(st, LCD_W - 150);
    lv_label_set_long_mode(st, LV_LABEL_LONG_WRAP);
    uiBtnRaw(card, sel ? "SELECTED" : "SELECT", LCD_W - 136, 30, 108, 40,
             sel ? g_theme.accent : g_theme.card, sel ? 0x101010 : g_theme.text, &lv_font_montserrat_12, UD(60, i));
    y += 106;
  }
  uiLabel(s, "Changing output pauses playback; the track and position are kept.", &lv_font_montserrat_12,
          g_theme.text2, LV_ALIGN_TOP_LEFT, 20, y + 2);
  y += 34;
  uiLabel(s, "VOLUME", &lv_font_montserrat_12, g_theme.accent, LV_ALIGN_TOP_LEFT, 20, y);
  g_volLbl = uiLabel(s, "", &lv_font_montserrat_14, g_theme.text, LV_ALIGN_TOP_RIGHT, -22, y - 2);
  g_volSlider = lv_slider_create(s);
  lv_slider_set_range(g_volSlider, 0, 100);
  lv_slider_set_value(g_volSlider, g_settings.muted ? 0 : g_settings.volume, LV_ANIM_OFF);
  lv_obj_set_pos(g_volSlider, 22, y + 26);
  lv_obj_set_size(g_volSlider, LCD_W - 100, 16);
  lv_obj_set_style_bg_color(g_volSlider, lv_color_hex(g_theme.elev), LV_PART_MAIN);
  lv_obj_set_style_bg_color(g_volSlider, lv_color_hex(g_theme.accent), LV_PART_INDICATOR);
  lv_obj_set_style_bg_color(g_volSlider, lv_color_hex(g_theme.text), LV_PART_KNOB);
  lv_obj_add_event_cb(g_volSlider, uiAction, LV_EVENT_VALUE_CHANGED, UD(72, 0));
  lv_obj_add_event_cb(g_volSlider, uiAction, LV_EVENT_RELEASED, UD(73, 0));
  uiBtnRaw(s, g_settings.muted ? LV_SYMBOL_MUTE : LV_SYMBOL_VOLUME_MAX, LCD_W - 72, y + 12, 46, 44,
           g_settings.muted ? g_theme.accent : g_theme.elev, g_settings.muted ? 0x101010 : g_theme.text,
           &lv_font_montserrat_14, UD(74, 0));
  uiBtnRaw(s, "Re-initialise output", 14, y + 58, LCD_W - 28, 44, g_theme.card, g_theme.text, &lv_font_montserrat_14, UD(61, 0));
}

/* ---- settings ---------------------------------------------------------- */
static void settingsRow(lv_obj_t *parent, int y, const char *name, const String &value, void *ud) {
  lv_obj_t *row = uiPanel(parent, 14, y, LCD_W - 28, 52, g_theme.card, 14);
  lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
  if (ud) lv_obj_add_event_cb(row, uiAction, LV_EVENT_CLICKED, ud);
  uiLabel(row, name, &lv_font_montserrat_14, g_theme.text, LV_ALIGN_LEFT_MID, 16, 0);
  uiLabel(row, value.c_str(), &lv_font_montserrat_12, g_theme.accent, LV_ALIGN_RIGHT_MID, -16, 0);
}
static const char *repeatName(int r) { return r == 0 ? "Off" : r == 1 ? "All" : "One"; }
static String timeoutName(uint32_t ms) { return String(ms / 1000) + " s"; }
static const char *themeName() {
  if (g_themeCfg.theme >= 0 && g_themeCfg.theme < THEME_COUNT) return THEME_NAMES[g_themeCfg.theme];
  return "Custom";
}

static void buildSettings() {
  uiHeader("Settings");
  lv_obj_t *s = uiScrollArea(60);
  int y = 2;
  uiLabel(s, "APPEARANCE", &lv_font_montserrat_12, g_theme.accent, LV_ALIGN_TOP_LEFT, 20, y);
  y += 22;
  settingsRow(s, y, "Theme", themeName(), UD(80, 0)); y += 58;
  settingsRow(s, y, "Wallpaper", g_settings.useWallpaper ? "On" : "Off", UD(81, 0)); y += 58;
  settingsRow(s, y, "Wallpaper dim", String(g_settings.wallpaperDim) + " %", UD(107, 0)); y += 58;
  settingsRow(s, y, "Artwork background", g_settings.artworkBg ? "On" : "Off", UD(82, 0)); y += 58;
  settingsRow(s, y, "Brightness +", String(g_settings.brightness), UD(84, 0)); y += 58;
  settingsRow(s, y, "Brightness -", String(g_settings.brightness), UD(83, 0)); y += 58;
  settingsRow(s, y, "Screen timeout", timeoutName(g_settings.timeoutMs), UD(85, 0)); y += 58;

  uiLabel(s, "CLOCK & GLANCE", &lv_font_montserrat_12, g_theme.accent, LV_ALIGN_TOP_LEFT, 20, y);
  y += 22;
  settingsRow(s, y, "Quick Glance", "open", UD(124, 0)); y += 58;
  settingsRow(s, y, "Start in Quick Glance", g_settings.startInGlance ? "On" : "Off", UD(123, 0)); y += 58;
  settingsRow(s, y, "Clock format", g_settings.clock24h ? "24-hour" : "12-hour", UD(120, 0)); y += 58;
  settingsRow(s, y, "Clock seconds", g_settings.clockSeconds ? "On" : "Off", UD(121, 0)); y += 58;
  settingsRow(s, y, "Glance status row", g_settings.glanceStatus ? "On" : "Off", UD(122, 0)); y += 58;

  uiLabel(s, "PLAYBACK", &lv_font_montserrat_12, g_theme.accent, LV_ALIGN_TOP_LEFT, 20, y);
  y += 22;
  settingsRow(s, y, "Shuffle by default", g_settings.shuffle ? "On" : "Off", UD(86, 0)); y += 58;
  settingsRow(s, y, "Repeat by default", repeatName(g_settings.repeat), UD(87, 0)); y += 58;
  settingsRow(s, y, "Resume last track", g_settings.resumeLast ? "On" : "Off", UD(88, 0)); y += 58;
  settingsRow(s, y, "Remember queue", g_settings.rememberQueue ? "On" : "Off", UD(89, 0)); y += 58;
  settingsRow(s, y, "Automatic next track", g_settings.autoNext ? "On" : "Off", UD(90, 0)); y += 58;

  uiLabel(s, "AUDIO", &lv_font_montserrat_12, g_theme.accent, LV_ALIGN_TOP_LEFT, 20, y);
  y += 22;
  settingsRow(s, y, "Output", outputAt(g_settings.output)->name(), UD(91, 0)); y += 58;
  settingsRow(s, y, "Volume", String(g_settings.muted ? 0 : g_settings.volume) + " %", UD(91, 0)); y += 58;
  settingsRow(s, y, "Mute", g_settings.muted ? "On" : "Off", UD(74, 0)); y += 58;

  uiLabel(s, "LIBRARY", &lv_font_montserrat_12, g_theme.accent, LV_ALIGN_TOP_LEFT, 20, y);
  y += 22;
  settingsRow(s, y, "Rescan library", String(g_trackCount) + " tracks", UD(92, 0)); y += 58;
  settingsRow(s, y, "Refresh metadata", "rebuild index", UD(93, 0)); y += 58;

  uiLabel(s, "STATISTICS", &lv_font_montserrat_12, g_theme.accent, LV_ALIGN_TOP_LEFT, 20, y);
  y += 22;
  settingsRow(s, y, "Listening statistics", fmtTime(g_statTotalSeconds), UD(94, 0)); y += 58;
  settingsRow(s, y, "Reset statistics", "clear", UD(95, 0)); y += 58;

  uiLabel(s, "MUSIC SOURCE", &lv_font_montserrat_12, g_theme.accent, LV_ALIGN_TOP_LEFT, 20, y);
  y += 22;
  settingsRow(s, y, "Music source", "WATCH - SD card", UD(126, 0)); y += 58;

  uiLabel(s, "SYSTEM", &lv_font_montserrat_12, g_theme.accent, LV_ALIGN_TOP_LEFT, 20, y);
  y += 22;
  settingsRow(s, y, "Storage diagnostics", g_sd_ok ? "SD ready" : "SD missing", UD(96, 0)); y += 58;
  settingsRow(s, y, "Audio diagnostics", g_activeOutput ? g_activeOutput->status() : "no output", UD(97, 0)); y += 58;
  settingsRow(s, y, "SpotifyOS", SPOTIFY_BUILD_TAG, nullptr); y += 58;
  uiBtnRaw(s, "Return to LauncherOS", 14, y, LCD_W - 28, 56, g_theme.error, 0xFFFFFF, &lv_font_montserrat_16, UD(98, 0));
  y += 64;
  lv_obj_t *pad = uiPanel(s, 0, y, 1, 40, g_theme.bg, 0);
  lv_obj_set_style_bg_opa(pad, LV_OPA_TRANSP, 0);
}

/* ---- statistics -------------------------------------------------------- */
static void buildStats() {
  uiHeader("Statistics");
  lv_obj_t *s = uiScrollArea(60);
  int y = 4;
  lv_obj_t *card = uiCard(s, 14, y, LCD_W - 28, 116);
  uiLabel(card, "TOTAL LISTENING", &lv_font_montserrat_12, g_theme.accent, LV_ALIGN_TOP_LEFT, 16, 12);
  uiLabel(card, (fmtTime(g_statTotalSeconds) + " h:mm").c_str(), &lv_font_montserrat_28, g_theme.text, LV_ALIGN_TOP_LEFT, 16, 34);
  char b[96];
  snprintf(b, sizeof(b), "%u started   %u completed", (unsigned)g_statStarted, (unsigned)g_statCompleted);
  uiLabel(card, b, &lv_font_montserrat_12, g_theme.text2, LV_ALIGN_BOTTOM_LEFT, 16, -34);
  snprintf(b, sizeof(b), "%u session%s   current %s", (unsigned)g_statSessions,
           g_statSessions == 1 ? "" : "s", fmtTime(g_sessionSeconds).c_str());
  uiLabel(card, b, &lv_font_montserrat_12, g_theme.text2, LV_ALIGN_BOTTOM_LEFT, 16, -14);
  y += 126;

  uiLabel(s, "MOST PLAYED", &lv_font_montserrat_12, g_theme.accent, LV_ALIGN_TOP_LEFT, 20, y);
  y += 24;
  /* top 5 by play count */
  int top[5] = { -1, -1, -1, -1, -1 };
  for (size_t i = 0; i < g_statPaths.size(); i++) {
    for (int k = 0; k < 5; k++) {
      if (top[k] < 0) { top[k] = (int)i; break; }
      if (g_statCounts[i] > g_statCounts[top[k]]) {
        for (int m = 4; m > k; m--) top[m] = top[m - 1];
        top[k] = (int)i;
        break;
      }
    }
  }
  int shown = 0;
  for (int k = 0; k < 5; k++) {
    int si = top[k];
    if (si < 0) continue;
    int ti = findTrack(g_statPaths[si]);
    const char *title = ti >= 0 ? g_tracks[ti].title.c_str() : baseName(g_statPaths[si]).c_str();
    lv_obj_t *row = uiPanel(s, 14, y, LCD_W - 28, 48, g_theme.card, 14);
    uiLabel(row, title, &lv_font_montserrat_14, g_theme.text, LV_ALIGN_LEFT_MID, 14, -8);
    snprintf(b, sizeof(b), "%u plays  -  %s", (unsigned)g_statCounts[si], fmtTime(g_statSeconds[si]).c_str());
    uiLabel(row, b, &lv_font_montserrat_12, g_theme.text2, LV_ALIGN_LEFT_MID, 14, 10);
    y += 56;
    shown++;
  }
  if (!shown) uiLabel(s, "No plays recorded yet.", &lv_font_montserrat_14, g_theme.text2, LV_ALIGN_TOP_LEFT, 20, y);
  y += (shown ? 8 : 30);
  uiBtnRaw(s, "Reset statistics", 14, y, LCD_W - 28, 50, g_theme.card, g_theme.error, &lv_font_montserrat_14, UD(95, 0));
}

/* ---- theme + wallpaper ------------------------------------------------- */
static String g_wallFiles[24];
static int g_wallCount = 0;
static void listWallpapers() {
  g_wallCount = 0;
  if (!g_sd_ok) return;
  File dir = SD_MMC.open(SPOTIFY_WALL_DIR);
  if (!dir || !dir.isDirectory()) return;
  File f = dir.openNextFile();
  while (f && g_wallCount < 24) {
    String nm = f.name();
    int s = nm.lastIndexOf('/');
    if (s >= 0) nm = nm.substring(s + 1);
    String low = nm; low.toLowerCase();
    if (!f.isDirectory() && (low.endsWith(".raw") || low.endsWith(".bin"))) g_wallFiles[g_wallCount++] = nm;
    f = dir.openNextFile();
  }
  dir.close();
}

static void buildTheme() {
  uiHeader("Theme & Wallpaper");
  lv_obj_t *s = uiScrollArea(60);
  int y = 4;
  uiLabel(s, "THEME", &lv_font_montserrat_12, g_theme.accent, LV_ALIGN_TOP_LEFT, 20, y);
  y += 22;
  const int cols = 2, bw = 186, bh = 48, gap = 8;
  for (int i = 0; i < THEME_COUNT; i++) {
    uiBtnRaw(s, THEME_NAMES[i], 14 + (i % cols) * (bw + gap), y + (i / cols) * (bh + gap), bw, bh,
             g_themeCfg.theme == i ? g_theme.accent : g_theme.card,
             g_themeCfg.theme == i ? 0x101010 : g_theme.text, &lv_font_montserrat_14, UD(100, i));
  }
  y += ((THEME_COUNT + cols - 1) / cols) * (bh + gap) + 6;
  uiLabel(s, "ACCENT COLOUR", &lv_font_montserrat_12, g_theme.accent, LV_ALIGN_TOP_LEFT, 20, y);
  y += 24;
  const int swSize = 46;
  const int swStep = (LCD_W - 28 - swSize) / (THEME_COUNT - 1);   /* 56 px, all 7 fit */
  for (int i = 0; i < THEME_COUNT; i++) {
    lv_obj_t *sw = uiPanel(s, 14 + i * swStep, y, swSize, swSize, THEME_ACCENTS[i], swSize / 2);
    lv_obj_add_flag(sw, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(sw, uiAction, LV_EVENT_CLICKED, UD(101, i));
    if (g_themeCfg.accent == THEME_ACCENTS[i]) {
      lv_obj_set_style_border_width(sw, 3, 0);
      lv_obj_set_style_border_color(sw, lv_color_hex(g_theme.text), 0);
    }
  }
  y += swSize + 16;
  uiLabel(s, "WALLPAPER", &lv_font_montserrat_12, g_theme.accent, LV_ALIGN_TOP_LEFT, 20, y);
  y += 24;
  settingsRow(s, y, "Use wallpaper", g_settings.useWallpaper ? "On" : "Off", UD(81, 0)); y += 58;
  settingsRow(s, y, "Wallpaper dim", String(g_settings.wallpaperDim) + " %", UD(107, 0)); y += 58;
  listWallpapers();
  if (!g_wallCount) {
    uiLabel(s, "No wallpapers found. Add raw RGB565 410x502 .raw files to Wallpapers/.",
            &lv_font_montserrat_12, g_theme.text2, LV_ALIGN_TOP_LEFT, 20, y);
    y += 40;
  } else {
    for (int i = 0; i < g_wallCount; i++) {
      String label = g_wallFiles[i];
      for (int t = 0; t < THEME_COUNT; t++)
        if (*THEME_WALLPAPERS[t] && g_wallFiles[i] == THEME_WALLPAPERS[t])
          label = String(THEME_NAMES[t]) + "  -  " + label;
      lv_obj_t *b = uiBtnRaw(s, label.c_str(), 14, y, LCD_W - 28, 46,
                             g_settings.wallpaperFile == g_wallFiles[i] ? g_theme.accent : g_theme.card,
                             g_settings.wallpaperFile == g_wallFiles[i] ? 0x101010 : g_theme.text,
                             &lv_font_montserrat_12, UD(102, i));
      lv_label_set_long_mode(lv_obj_get_child(b, 0), LV_LABEL_LONG_DOT);
      lv_obj_set_width(lv_obj_get_child(b, 0), LCD_W - 80);
      y += 54;
    }
  }
  settingsRow(s, y, "Artwork background", g_settings.artworkBg ? "On" : "Off", UD(82, 0));
}

/* ---- storage / diagnostics -------------------------------------------- */
static void buildStorage() {
  uiHeader("Diagnostics");
  lv_obj_t *s = uiScrollArea(60);
  int y = 4;
  char b[200];
  const esp_partition_t *run = esp_ota_get_running_partition();
  const esp_partition_t *fact = esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_FACTORY, nullptr);

  lv_obj_t *card = uiCard(s, 14, y, LCD_W - 28, 168);
  uiLabel(card, "STORAGE", &lv_font_montserrat_12, g_theme.accent, LV_ALIGN_TOP_LEFT, 16, 12);
  if (g_sd_ok) {
    uint64_t total = SD_MMC.totalBytes(), used = SD_MMC.usedBytes();
    snprintf(b, sizeof(b), "SD card          READY\nCapacity         %.2f GB\nUsed             %.2f GB\nFree             %.2f GB",
             total / 1073741824.0, used / 1073741824.0, (total - used) / 1073741824.0);
  } else {
    snprintf(b, sizeof(b), "SD card          NOT MOUNTED\nSpotifyOS needs the TF card mounted.\nReseat the card and restart.");
  }
  lv_obj_t *st = uiLabel(card, b, &lv_font_montserrat_12, g_theme.text, LV_ALIGN_TOP_LEFT, 16, 36);
  lv_obj_set_width(st, LCD_W - 64);
  lv_label_set_long_mode(st, LV_LABEL_LONG_WRAP);
  y += 178;

  lv_obj_t *lib = uiCard(s, 14, y, LCD_W - 28, 132);
  uiLabel(lib, "LIBRARY", &lv_font_montserrat_12, g_theme.accent, LV_ALIGN_TOP_LEFT, 16, 12);
  snprintf(b, sizeof(b), "Tracks indexed   %d\nPlaylists        %u\nLiked songs      %u\nHistory entries  %u\nQueue length     %u",
           g_trackCount, (unsigned)g_playlists.size(), (unsigned)g_liked.size(), (unsigned)g_history.size(), (unsigned)g_queue.size());
  lv_obj_t *l2 = uiLabel(lib, b, &lv_font_montserrat_12, g_theme.text, LV_ALIGN_TOP_LEFT, 16, 36);
  lv_obj_set_width(l2, LCD_W - 64);
  lv_label_set_long_mode(l2, LV_LABEL_LONG_WRAP);
  y += 142;

  lv_obj_t *au = uiCard(s, 14, y, LCD_W - 28, 176);
  uiLabel(au, "AUDIO", &lv_font_montserrat_12, g_theme.accent, LV_ALIGN_TOP_LEFT, 16, 12);
  snprintf(b, sizeof(b),
           "Selected output  %s\nOutput status    %s\nCodec            ES8311 @ 0x18\nCodec rate       %u Hz\nAmplifier        NS4150B (GPIO46)\nESP32-S3 BT      BLE only, no A2DP",
           outputAt(g_settings.output)->name(),
           g_activeOutput ? g_activeOutput->status() : "inactive",
           (unsigned)g_codecRate);
  lv_obj_t *a2 = uiLabel(au, b, &lv_font_montserrat_12, g_theme.text, LV_ALIGN_TOP_LEFT, 16, 36);
  lv_obj_set_width(a2, LCD_W - 64);
  lv_label_set_long_mode(a2, LV_LABEL_LONG_WRAP);
  y += 186;

  lv_obj_t *sys = uiCard(s, 14, y, LCD_W - 28, 150);
  uiLabel(sys, "SYSTEM", &lv_font_montserrat_12, g_theme.accent, LV_ALIGN_TOP_LEFT, 16, 12);
  snprintf(b, sizeof(b),
           "Firmware         SpotifyOS v1.0.0\nLVGL             %d.%d.%d\nFree heap        %u KB\nFree PSRAM       %u KB\nRunning slot     %s\nLauncher slot    %s",
           LVGL_VERSION_MAJOR, LVGL_VERSION_MINOR, LVGL_VERSION_PATCH,
           (unsigned)(ESP.getFreeHeap() / 1024),
           (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024),
           run ? run->label : "?",
           fact ? fact->label : "missing");
  lv_obj_t *s2 = uiLabel(sys, b, &lv_font_montserrat_12, g_theme.text, LV_ALIGN_TOP_LEFT, 16, 36);
  lv_obj_set_width(s2, LCD_W - 64);
  lv_label_set_long_mode(s2, LV_LABEL_LONG_WRAP);
  y += 158;
  uiBtnRaw(s, "Return to LauncherOS", 14, y, LCD_W - 28, 54, g_theme.error, 0xFFFFFF, &lv_font_montserrat_16, UD(98, 0));
}

/* ---- search ------------------------------------------------------------ */
static lv_obj_t *g_searchArea = nullptr;
static lv_obj_t *g_searchResults = nullptr;
static void runSearch() {
  if (!g_searchResults || !g_searchArea) return;
  lv_obj_clean(g_searchResults);
  String q = String(lv_textarea_get_text(g_searchArea));
  q.toLowerCase();
  int shown = 0;
  for (int i = 0; i < g_trackCount && shown < 12; i++) {
    String hay = g_tracks[i].title + " " + g_tracks[i].artist + " " + g_tracks[i].album;
    hay.toLowerCase();
    if (q.length() && hay.indexOf(q) < 0) continue;
    trackRow(g_searchResults, shown * 62, i, 50);
    shown++;
  }
  if (!shown) uiLabel(g_searchResults, "No matches.", &lv_font_montserrat_14, g_theme.text2, LV_ALIGN_TOP_LEFT, 20, 8);
}
static void buildSearch() {
  uiHeader("Search");
  g_searchArea = lv_textarea_create(g_root);
  lv_textarea_set_one_line(g_searchArea, true);
  lv_textarea_set_max_length(g_searchArea, 40);
  lv_textarea_set_placeholder_text(g_searchArea, "Title, artist or album");
  lv_obj_set_pos(g_searchArea, 14, 62);
  lv_obj_set_size(g_searchArea, LCD_W - 28, 48);
  lv_obj_set_style_text_font(g_searchArea, &lv_font_montserrat_14, 0);
  lv_obj_set_style_bg_color(g_searchArea, lv_color_hex(g_theme.card), 0);
  lv_obj_set_style_text_color(g_searchArea, lv_color_hex(g_theme.text), 0);
  lv_obj_set_style_border_color(g_searchArea, lv_color_hex(g_theme.divider), 0);
  lv_obj_add_event_cb(g_searchArea, uiAction, LV_EVENT_VALUE_CHANGED, UD(105, 0));

  g_searchResults = lv_obj_create(g_root);
  lv_obj_set_pos(g_searchResults, 0, 118);
  lv_obj_set_size(g_searchResults, LCD_W, 150);
  lv_obj_set_style_bg_opa(g_searchResults, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(g_searchResults, 0, 0);
  lv_obj_set_style_pad_all(g_searchResults, 0, 0);
  lv_obj_set_scroll_dir(g_searchResults, LV_DIR_VER);

  lv_obj_t *kb = lv_keyboard_create(g_root);
  lv_obj_set_pos(kb, 0, 274);
  lv_obj_set_size(kb, LCD_W, 228);
  lv_obj_set_style_bg_color(kb, lv_color_hex(g_theme.card), 0);
  lv_keyboard_set_textarea(kb, g_searchArea);
  lv_obj_add_event_cb(kb, uiAction, LV_EVENT_VALUE_CHANGED, UD(106, 0));
  runSearch();
}

/* ---- the single action dispatcher -------------------------------------- */
static void uiAction(lv_event_t *e) {
  int code = UD_CODE(lv_event_get_user_data(e));
  int arg  = UD_ARG(lv_event_get_user_data(e));
  lv_event_code_t ec = lv_event_get_code(e);

  if (ec == LV_EVENT_VALUE_CHANGED && code == 70) { g_seekDragging = true; return; }
  if (ec == LV_EVENT_RELEASED && code == 71) {
    g_seekDragging = false;
    lv_obj_t *sl = lv_event_get_target(e);
    playerSeekFraction(lv_slider_get_value(sl) / 1000.0f);
    return;
  }
  if (ec == LV_EVENT_VALUE_CHANGED && code == 72) {
    lv_obj_t *sl = lv_event_get_target(e);
    int v = lv_slider_get_value(sl);
    g_settings.volume = (uint8_t)v;
    if (v) g_lastNonZeroVolume = (uint8_t)v;
    g_settings.muted = (v == 0);
    if (g_activeOutput) g_activeOutput->setVolumePercent((uint8_t)v);
    if (g_volLbl) lv_label_set_text(g_volLbl, (String(v) + " %").c_str());
    return;
  }
  if (ec == LV_EVENT_RELEASED && code == 73) { saveSettings(); return; }
  if (ec == LV_EVENT_VALUE_CHANGED && code == 105) { runSearch(); return; }
  if (ec == LV_EVENT_VALUE_CHANGED && code == 106) { runSearch(); return; }
  if (ec != LV_EVENT_CLICKED) return;

  if (code >= 100 && code < 130) uiCloseModal();

  switch (code) {
    case 1: uiBack(); break;
    case 2: uiHome(); break;
    case 10: uiPush(V_LIBRARY, 0); break;
    case 11: uiPush(V_NOWPLAYING, 0); break;
    case 12: uiPush(V_SETTINGS, 0); break;
    case 20: playerToggle(); uiBuild(); break;
    case 21: playerNext(true); uiBuild(); break;
    case 22: playerPrev(); uiBuild(); break;
    case 23: g_settings.shuffle = !g_settings.shuffle; saveSettings(); uiBuild(); break;
    case 24: g_settings.repeat = (g_settings.repeat + 1) % 3; saveSettings(); uiBuild(); break;
    case 25:
      if (g_currentTrack >= 0 && g_currentTrack < g_trackCount) {
        toggleLike(g_tracks[g_currentTrack].path);
        uiToast(isLiked(g_tracks[g_currentTrack].path) ? "Added to Liked Songs" : "Removed from Liked Songs");
      } else uiToast("Nothing playing");
      uiBuild();
      break;
    case 26: uiPush(V_QUEUE, 0); break;
    case 27: uiPush(V_OUTPUT, 0); break;
    case 30: uiPush(V_PLAYLIST, arg); break;
    case 40: uiPush(V_PLAYLIST, PL_ALL); break;
    case 41: uiPush(V_PLAYLIST, PL_LIKED); break;
    case 42: uiPush(V_PLAYLIST, PL_HISTORY); break;
    case 43: uiPush(V_SEARCH, 0); break;
    case 44: uiPush(V_PLAYLIST, arg); break;
    case 45:
      setQueueFromPlaylist(arg, -1);
      if (g_settings.shuffle) shuffleQueue();      /* fresh random order */
      playerPlayQueuePos(0);
      uiPush(V_NOWPLAYING, 0);
      break;
    case 46:
      setQueueFromPlaylist(g_viewArg, -1);
      if (g_settings.shuffle) shuffleQueue();
      playerPlayQueuePos(0);
      uiBuild();
      break;
    case 47:
      g_settings.shuffle = true;
      saveSettings();
      setQueueFromPlaylist(arg, -1);
      shuffleQueue();                              /* always shuffle here */
      playerPlayQueuePos(0);
      uiPush(V_NOWPLAYING, 0);
      break;
    case 48: g_sortMode = (g_sortMode + 1) % 4; uiBuild(); break;
    case 50:
      if (g_view == V_PLAYLIST) {
        setQueueFromPlaylist(g_viewArg, arg);
        if (g_settings.shuffle) shuffleQueue(g_tracks[arg].path);   /* tapped one plays first */
        playerPlayQueuePos(g_queueIndex);
      } else playerPlayIndex(arg);
      uiPush(V_NOWPLAYING, 0);
      break;
    case 51: playerPlayQueuePos(arg); uiBuild(); break;
    case 52: g_queue.clear(); g_queueIndex = 0; saveQueue(); uiBuild(); break;
    case 53: playerPlayQueuePos(arg); uiBuild(); break;
    case 60: switchOutput(arg); break;
    case 61: {
      if (g_activeOutput) {
        g_activeOutput->shutdown();
        g_activeOutput = nullptr;
      }
      bool ok = outputActivate(g_settings.output);
      uiToast(ok ? "Output re-initialised" : "Output could not be initialised");
      uiBuild();
      break;
    }
    case 70: case 71: case 72: case 73: case 80: case 103: case 104:
      if (code == 103) {
        if (g_currentTrack >= 0) {
          playerPlayIndex(g_currentTrack);
          if (g_resumeAtSec) audioSeekSeconds((uint16_t)g_resumeAtSec);
        }
      }
      if (code == 80) uiPush(V_THEME, 0);
      break;
    case 74:
      if (g_settings.muted || g_settings.volume == 0) {
        g_settings.muted = false;
        if (g_settings.volume == 0) g_settings.volume = g_lastNonZeroVolume;
        if (g_settings.volume == 0) g_settings.volume = DEFAULT_VOLUME;
      } else {
        g_settings.muted = true;
      }
      if (g_activeOutput) g_activeOutput->setVolumePercent(g_settings.muted ? 0 : g_settings.volume);
      saveSettings();
      uiBuild();
      break;
    case 81: g_settings.useWallpaper = !g_settings.useWallpaper; saveSettings(); uiBuild(); break;
    case 82: g_settings.artworkBg = !g_settings.artworkBg; saveSettings(); uiBuild(); break;
    case 83: g_settings.brightness = (uint8_t)max(20, (int)g_settings.brightness - 20); fadeBrightness(g_settings.brightness, 120); saveSettings(); uiBuild(); break;
    case 84: g_settings.brightness = (uint8_t)min(255, (int)g_settings.brightness + 20); fadeBrightness(g_settings.brightness, 120); saveSettings(); uiBuild(); break;
    case 85:
      g_settings.timeoutMs = (g_settings.timeoutMs >= 60000) ? 10000 : g_settings.timeoutMs + 10000;
      saveSettings();
      uiBuild();
      break;
    case 86: g_settings.shuffle = !g_settings.shuffle; saveSettings(); uiBuild(); break;
    case 87: g_settings.repeat = (g_settings.repeat + 1) % 3; saveSettings(); uiBuild(); break;
    case 88: g_settings.resumeLast = !g_settings.resumeLast; saveSettings(); uiBuild(); break;
    case 89: g_settings.rememberQueue = !g_settings.rememberQueue; saveSettings(); uiBuild(); break;
    case 90: g_settings.autoNext = !g_settings.autoNext; saveSettings(); uiBuild(); break;
    case 91: uiPush(V_OUTPUT, 0); break;
    case 92: case 93:
      uiToast("Scanning Music/ ...", 1200);
      pump();
      scanMusic();
      saveLibrary();
      uiToast(String(g_trackCount) + " tracks indexed");
      uiBuild();
      break;
    case 94: uiPush(V_STATS, 0); break;
    case 95:
      uiModal("RESET STATISTICS", "Delete all listening totals, play counts and history?",
              "Reset", UD(110, 0), "Cancel", UD(104, 0));
      break;
    case 120: g_settings.clock24h = !g_settings.clock24h; saveSettings(); uiBuild(); break;
    case 121: g_settings.clockSeconds = !g_settings.clockSeconds; saveSettings(); uiBuild(); break;
    case 122: g_settings.glanceStatus = !g_settings.glanceStatus; saveSettings(); uiBuild(); break;
    case 123:
      g_settings.startInGlance = !g_settings.startInGlance;
      saveSettings();
      uiToast(g_settings.startInGlance ? "Opens on Quick Glance" : "Opens on the home screen");
      uiBuild();
      break;
    case 124: uiPush(V_GLANCE, 0); break;       /* Home / Settings -> watch face */
    case 125: uiPush(V_HOME, 0); break;         /* tap the glance -> the full app */
    case 126:
      /* The brief asks for phone control; the truth is that it is not built.
       * Say so here rather than showing a source that cannot play. */
      uiModal("MUSIC SOURCE",
              "WATCH only: SpotifyOS plays the MP3s on the card.\n\n"
              "Phone Music is not implemented.  Controlling a phone needs the BLE GATT "
              "protocol and an Android companion app, and the ESP32-S3 has no Bluetooth "
              "Classic A2DP, so the watch cannot stream a phone's audio either.",
              "Got it", UD(104, 0), "", nullptr);
      break;
    case 110: resetStats(); uiToast("Statistics cleared"); uiBuild(); break;
    case 96: case 97: uiPush(V_STORAGE, 0); break;
    case 98:
      uiModal("RETURN TO LAUNCHEROS", "Save the session and reboot into the launcher?",
              "Return", UD(111, 0), "Cancel", UD(104, 0));
      break;
    case 111: returnToLauncher(); break;
    case 100:
      if (arg >= 0 && arg < THEME_COUNT) {
        g_themeCfg.theme = arg;
        applyTheme();
        saveThemeConfig();
        autoApplyThemeWallpaper(arg);       /* painting follows its theme */
        uiToast(String(THEME_NAMES[arg]) + "  -  wallpaper: " +
                (g_settings.useWallpaper ? "on" : "off"));
        uiBuild();
      }
      break;
    case 101:
      if (arg >= 0 && arg < THEME_COUNT) {
        g_themeCfg.accent = THEME_ACCENTS[arg];
        g_themeCfg.theme = (arg == 0) ? 0 : THEME_CUSTOM;   /* custom accent = custom tokens */
        applyTheme();
        saveThemeConfig();
        uiBuild();
      }
      break;
    case 102:
      if (arg >= 0 && arg < g_wallCount) {
        g_settings.wallpaperFile = g_wallFiles[arg];
        g_settings.useWallpaper = true;
        themeForWallpaper(g_wallFiles[arg]);   /* painting -> its theme */
        freeWallpaper();
        saveSettings();
        uiToast("Wallpaper set");
        uiBuild();
      }
      break;
    case 107: {
      static const uint8_t DIMS[4] = { 0, 25, 50, 75 };
      int cur = 2;
      for (int i = 0; i < 4; i++) if (g_settings.wallpaperDim == DIMS[i]) cur = i;
      g_settings.wallpaperDim = DIMS[(cur + 1) % 4];
      saveSettings();
      uiBuild();
      break;
    }
    default: break;
  }
}
/* ---------------------------------------------------------------------------
 * 15. VIEW DISPATCH + WALLPAPER
 * ------------------------------------------------------------------------ */
static void applyWallpaperToRoot() {
  if (!g_settings.useWallpaper || !g_settings.wallpaperFile.length()) { return; }
  loadWallpaperFile(g_settings.wallpaperFile);
  if (!g_wallReady) return;
  lv_obj_t *img = lv_img_create(g_root);
  lv_img_set_src(img, &g_wallDsc);
  lv_obj_set_pos(img, 0, 0);
  lv_obj_set_size(img, LCD_W, LCD_H);
  lv_obj_clear_flag(img, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_t *dim = uiPanel(g_root, 0, 0, LCD_W, LCD_H, 0x000000, 0);
  lv_obj_set_style_bg_opa(dim, (lv_opa_t)(g_settings.wallpaperDim * 255 / 100), 0);
}

static void uiBuild() {
  if (!g_root) return;
  g_seekDragging = false;
  if (g_seekSlider) { g_seekSlider = nullptr; g_liveLbl = nullptr; }
  g_volSlider = nullptr;
  g_volLbl = nullptr;
  g_playBtnLbl = nullptr;
  g_clockLbl = nullptr;
  g_glanceClock = nullptr;
  g_glanceDate = nullptr;
  g_glanceBar = nullptr;
  g_glanceBatt = nullptr;
  g_glanceOut = nullptr;
  g_searchArea = nullptr;      /* the objects below are about to be deleted */
  g_searchResults = nullptr;
  uiCloseModal();
  lv_obj_clean(g_root);
  lv_obj_set_style_bg_color(g_root, lv_color_hex(g_theme.bg), 0);
  lv_obj_set_style_bg_opa(g_root, LV_OPA_COVER, 0);
  lv_obj_clear_flag(g_root, LV_OBJ_FLAG_SCROLLABLE);
  applyWallpaperToRoot();

  switch (g_view) {
    case V_HOME:       buildHome(); break;
    case V_LIBRARY:    buildLibrary(); break;
    case V_PLAYLIST:   buildPlaylist(); break;
    case V_NOWPLAYING: buildNowPlaying(); break;
    case V_QUEUE:      buildQueue(); break;
    case V_OUTPUT:     buildOutput(); break;
    case V_SETTINGS:   buildSettings(); break;
    case V_STATS:      buildStats(); break;
    case V_THEME:      buildTheme(); break;
    case V_STORAGE:    buildStorage(); break;
    case V_SEARCH:     buildSearch(); break;
    case V_GLANCE:     buildGlance(); break;
    default:           buildHome(); break;
  }
  /* volume label refresh */
  if (g_volLbl) {
    char vb[16];
    snprintf(vb, sizeof(vb), "%u %%", (unsigned)(g_settings.muted ? 0 : g_settings.volume));
    lv_label_set_text(g_volLbl, vb);
  }
}

/* ---------------------------------------------------------------------------
 * 16. HARDWARE BOOT  --  same bring-up order as LauncherOS / AstroOS
 * ------------------------------------------------------------------------ */
static std::shared_ptr<Arduino_IIC_DriveBus> g_iic;
static std::unique_ptr<Arduino_IIC> g_touch;
static void Arduino_IIC_Touch_Interrupt(void);

static void pump() { lv_timer_handler(); }

static void fadeBrightness(uint8_t target, uint32_t duration) {
  int from = g_brightness;
  if (from == target) return;
  for (int i = 1; i <= 10; i++) {
    int value = from + ((int(target) - from) * i) / 10;
    if (gfx) static_cast<Arduino_CO5300 *>(gfx)->setBrightness((uint8_t)value);
    g_brightness = (uint8_t)value;
    pump();
    delay(max(1, (int)(duration / 10)));
  }
}

/* The loop task's remaining-stack low-water mark, printed only when it
 * reaches a NEW low, so the log stays readable but still names the callback
 * that eats the stack (this is how you tell the flush from the touch read). */
static uint32_t g_stackLowSeen = 0xFFFFFFFFu;
static void stackProbe(const char *where) {
  uint32_t left = (uint32_t)(uxTaskGetStackHighWaterMark(NULL) * 4);
  if (left < g_stackLowSeen) {
    g_stackLowSeen = left;
    USBSerial.printf("[stack] new low in %s: %u B left\n", where, left);
  }
}

static void displayFlush(lv_disp_drv_t *drv, const lv_area_t *area, lv_color_t *pixels) {
  stackProbe("display flush");
  uint32_t w = area->x2 - area->x1 + 1, h = area->y2 - area->y1 + 1;
  if (gfx) gfx->draw16bitRGBBitmap(area->x1, area->y1, (uint16_t *)pixels, w, h);
  lv_disp_flush_ready(drv);
}

static void touchRead(lv_indev_drv_t *, lv_indev_data_t *data) {
  if (!g_touch) { data->state = LV_INDEV_STATE_REL; return; }
  stackProbe("touch read");
  int32_t x = g_touch->IIC_Read_Device_Value(g_touch->Arduino_IIC_Touch::Value_Information::TOUCH_COORDINATE_X);
  int32_t y = g_touch->IIC_Read_Device_Value(g_touch->Arduino_IIC_Touch::Value_Information::TOUCH_COORDINATE_Y);
  int fingers = g_touch->IIC_Read_Device_Value(g_touch->Arduino_IIC_Touch::Value_Information::TOUCH_FINGER_NUMBER);
  bool pressed = fingers > 0;
  static uint8_t zeroStreak = 0;
  if (pressed) zeroStreak = 0;
  else if (g_touch_active && ++zeroStreak < 3) { pressed = true; x = g_current_x; y = g_current_y; }
  if (pressed) {
    data->state = LV_INDEV_STATE_PR;
    data->point.x = x;
    data->point.y = y;
    g_last_touch_ms = millis();
    if (!g_touch_active) { g_touch_active = true; g_start_x = x; g_start_y = y; g_start_ms = millis(); }
    g_current_x = x;
    g_current_y = y;
  } else {
    data->state = LV_INDEV_STATE_REL;
    if (g_touch_active) {
      g_touch_active = false;
      int32_t dx = g_current_x - g_start_x, dy = g_current_y - g_start_y;
      if (millis() - g_start_ms < SWIPE_MAX_DURATION_MS && abs(dx) > SWIPE_THRESHOLD_PX && abs(dx) > abs(dy)) {
        g_last_swipe_ms = millis();
        g_pending_swipe = (dx < 0) ? 1 : -1;   /* left = forward, right = back */
      }
      g_last_touch_ms = millis();
    }
  }
}

static void Arduino_IIC_Touch_Interrupt(void) {
  if (g_touch) g_touch->IIC_Interrupt_Flag = true;
}
/* The core defines this weakly in main.cpp (cores/esp32/main.cpp) and creates
 * loopTask with whatever it returns, so defining it here overrides it.
 * It lives down here, and not at the top of the file, on purpose -- see the
 * note in section 0: a function definition above the type declarations moves
 * Arduino's generated prototypes above them too. */
size_t getArduinoLoopTaskStackSize(void) { return SPOTIFY_LOOP_STACK; }

/* One line that says why the watch restarted.  On a boot loop this is the
 * first thing the Serial log tells you: "panic/exception" is a crash (an
 * aborted stack overflow reports as a panic), "task watchdog" is setup()
 * starving the idle task for more than five seconds. */
static const char *resetReasonName(esp_reset_reason_t r) {
  switch (r) {
    case ESP_RST_POWERON:   return "power-on";
    case ESP_RST_SW:        return "software restart";
    case ESP_RST_PANIC:     return "panic/exception";
    case ESP_RST_INT_WDT:   return "interrupt watchdog";
    case ESP_RST_TASK_WDT:  return "task watchdog";
    case ESP_RST_WDT:       return "watchdog";
    case ESP_RST_BROWNOUT:  return "brownout";
    case ESP_RST_DEEPSLEEP: return "deep sleep wake";
    default:                return "other";
  }
}

/* Does anything actually answer on the I2C bus?  The driver's own error log
 * only says "NACK" without saying from which device, and one failing device
 * turns into a flood. */
static bool i2cPresent(uint8_t addr) {
  Wire.beginTransmission(addr);
  return Wire.endTransmission() == 0;
}

static void hardwareInit() {
  Serial.begin(115200);
  USBSerial.begin(115200);
  delay(60);
  USBSerial.printf("\n[SpotifyOS] boot %s - reset: %s, heap %u KB, PSRAM %u KB, loop stack %u B\n",
                SPOTIFY_BUILD_TAG,
                resetReasonName(esp_reset_reason()),
                (unsigned)(ESP.getFreeHeap() / 1024),
                (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024),
                (unsigned)getArduinoLoopTaskStackSize());
  /* the i2c driver logs every NACK at error level; with one device not
   * answering, the 3 reads per touchRead() would bury the rest of the boot
   * log.  The one probe line below says the same thing, once. */
  esp_log_level_set("i2c.master", ESP_LOG_NONE);
  pinMode(BOOT_BTN, INPUT_PULLUP);
  pinMode(TP_INT, INPUT_PULLUP);
  btStop();                        /* this build never uses the radio */
  WiFi.mode(WIFI_OFF);

  /* QSPI display first (same order as the reference apps) */
  g_bus = new Arduino_ESP32QSPI(LCD_CS, LCD_SCLK, LCD_SDIO0, LCD_SDIO1, LCD_SDIO2, LCD_SDIO3);
  gfx = new Arduino_CO5300(g_bus, LCD_RESET, 0, LCD_W, LCD_H, 22, 0, 0, 0);
  gfx->begin();
  gfx->fillScreen(0);
  static_cast<Arduino_CO5300 *>(gfx)->setBrightness(0);

  /* I2C rail wake pulse, exactly like LauncherOS does */
  pinMode(IIC_SCL, OUTPUT);
  for (int i = 0; i < 9; i++) {
    digitalWrite(IIC_SCL, LOW); delayMicroseconds(5);
    digitalWrite(IIC_SCL, HIGH); delayMicroseconds(5);
  }
  Wire.begin(IIC_SDA, IIC_SCL);
  Wire.setClock(400000);
  USBSerial.printf("[i2c] 0x18 ES8311 %s, 0x34 AXP2101 %s, 0x38 FT3168 %s\n",
                i2cPresent(0x18) ? "ack" : "NO ACK",
                i2cPresent(0x34) ? "ack" : "NO ACK",
                i2cPresent(0x38) ? "ack" : "NO ACK");

  pinMode(TP_RESET, OUTPUT);
  digitalWrite(TP_RESET, LOW); delay(50);
  digitalWrite(TP_RESET, HIGH); delay(200);
  g_iic = std::make_shared<Arduino_HWIIC>(IIC_SDA, IIC_SCL, &Wire);
  g_touch.reset(new Arduino_FT3x68(g_iic, FT3168_DEVICE_ADDRESS, DRIVEBUS_DEFAULT_VALUE, TP_INT,
                                   Arduino_IIC_Touch_Interrupt));
  /* LauncherOS waits here -- `while (FT3168->begin() == false) { ...; delay(500); }`
   * -- because on this board the touch controller does not answer its init
   * registers straight away.  Calling begin() once and ignoring the result
   * (what this sketch used to do) leaves the chip dead for the whole session,
   * and every touchRead() then NACKs three times, over and over, forever.
   * Bounded rather than infinite: if the chip never answers, drop the driver so
   * touchRead() stops hammering a bus nothing is listening on. */
  bool touchOk = false;
  const int TOUCH_TRIES = 20;                 /* up to 10 s, then give up */
  for (int attempt = 1; attempt <= TOUCH_TRIES && !touchOk; attempt++) {
    touchOk = g_touch->begin();
    if (!touchOk) {
      USBSerial.printf("[i2c] FT3168 init attempt %d/%d failed, retrying\n", attempt, TOUCH_TRIES);
      delay(500);
    }
  }
  if (touchOk) {
    /* the reference app sets the same mode right after a successful init */
    g_touch->IIC_Write_Device_State(g_touch->Arduino_IIC_Touch::Device::TOUCH_POWER_MODE,
                                    g_touch->Arduino_IIC_Touch::Device_Mode::TOUCH_POWER_MONITOR);
    USBSerial.println("[i2c] FT3168 ready (monitor mode)");
  } else {
    USBSerial.println("[i2c] FT3168 never answered - touch disabled, UI is read-only");
    g_touch.reset();
  }

  if (g_pmu.begin(Wire, AXP2101_SLAVE_ADDRESS, IIC_SDA, IIC_SCL)) {
    g_has_pmu = true;
    g_pmu.disableTSPinMeasure();
    g_pmu.enableBattVoltageMeasure();
  }

  lv_init();
  size_t pixels = LCD_W * 60;
  g_lvbuf = (lv_color_t *)heap_caps_malloc(pixels * sizeof(lv_color_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  if (!g_lvbuf) g_lvbuf = (lv_color_t *)heap_caps_malloc(pixels * sizeof(lv_color_t), MALLOC_CAP_8BIT);
  lv_disp_draw_buf_init(&g_draw_buf, g_lvbuf, nullptr, pixels);
  lv_disp_drv_init(&g_disp_drv);
  g_disp_drv.flush_cb = displayFlush;
  g_disp_drv.draw_buf = &g_draw_buf;
  g_disp_drv.hor_res = LCD_W;
  g_disp_drv.ver_res = LCD_H;
  lv_disp_drv_register(&g_disp_drv);
  lv_indev_drv_init(&g_indev_drv);
  g_indev_drv.type = LV_INDEV_TYPE_POINTER;
  g_indev_drv.read_cb = touchRead;
  lv_indev_drv_register(&g_indev_drv);
}
/* ---------------------------------------------------------------------------
 * 17. TIME SERVICE (system clock persisted in NVS, like the reference apps)
 * ------------------------------------------------------------------------ */
// ---- shared time block (spec: TIME_CONTRACT.md) -----------------------------
#define TIME_SERIAL USBSerial
#define TIME_SRC_ID TIME_SRC_MODULE
// ===== SHARED TIME BLOCK v2 =================================================
//  Inlined verbatim into every app in this repo.  Spec + rationale:
//  TIME_CONTRACT.md.  Only the two lines above the block may differ per app:
//      #define TIME_SERIAL  Serial          // or USBSerial
//      #define TIME_SRC_ID  TIME_SRC_MODULE // or TIME_SRC_LAUNCHER
//
//  Namespace "rtcsave" -- the ONE place the watch's wall clock lives:
//      t    uint32  unix epoch seconds, UTC
//      gmt  int32   local offset from UTC, in SECONDS   (+28800 = UTC+8)
//      seq  uint32  write counter, ++ on every publish  (diagnostics)
//      man  uint8   1 = a human set this clock, 0 = machine (NTP/boot).  A boot
//                       refresh keeps it; a machine that lands a NEW epoch clears it.
//      src  uint8   0 = launcher, 1 = module            (diagnostics)
//
//  Two different things have to survive a reboot, and only one of them does:
//    * the EPOCH lives in the chip's RTC timer.  It survives every software
//      reset and every deep sleep, so a launcher <-> module reboot inherits
//      it for free -- but power loss wipes it, and then this snapshot is the
//      only thing between the user and "00:00".
//    * the TIMEZONE lives in the C library's TZ string, which is RAM-ONLY.
//      It dies on EVERY reboot and no app can inherit it from the previous
//      one.  NVS is therefore the offset's only home.
//  => frame: epoch is inherited through the RTC, backed up here.
//     timezone is stored here and re-applied by every single boot.
// ============================================================================
#define TIME_VALID_MIN      1700000000UL   // ~2023-11-14: "the RTC has been set"
#define TIME_ADOPT_SLACK_S  2L             // ignore sub-2 s snapshot/rtc skew
#define TIME_SRC_LAUNCHER   0
#define TIME_SRC_MODULE     1

static int32_t  g_tp_gmt   = 0;   // local offset from UTC, in seconds
static uint32_t g_tp_seq   = 0;   // last seq we read or wrote
static bool     g_tp_valid = false;   // a plausible wall clock is available

// Point the C library at the local UTC offset.  TZ is RAM-only, so this must
// run on EVERY boot in EVERY app or the watch silently renders UTC.
// POSIX counts the offset backwards from local to UTC, hence the minus.
static void time_apply_tz(int32_t gmt_sec) {
    long mins = -(gmt_sec / 60);
    long a    = labs(mins);
    char tz[24];
    snprintf(tz, sizeof(tz), "UTC%c%ld:%02ld", mins < 0 ? '-' : '+', a / 60, a % 60);
    setenv("TZ", tz, 1);
    tzset();
    g_tp_gmt = gmt_sec;
}

// Is the shared NVS snapshot a better clock than the one the chip is holding?
//   nothing saved yet            -> leave everything alone
//   chip has no valid clock      -> cold boot / dead battery: adopt the NVS
//   a human set it (man=1)       -> their correction wins in EITHER direction
//   snapshot runs ahead of the   -> somebody moved the clock forward: adopt
//   chip by more than the slack
//   otherwise                    -> the RTC is the live clock, keep it
//   Returns TIME_DEC_* below (uint8_t return keeps the Arduino .ino auto
//   prototype generator, which runs before this enum is declared, happy).
enum TimeDecision { TIME_DEC_NOTHING = 0, TIME_DEC_KEEP_RTC = 1, TIME_DEC_ADOPT = 2 };
static uint8_t time_decide(time_t rtc_now, uint32_t snap_t, uint8_t snap_man) {
    if (snap_t < TIME_VALID_MIN)                       return TIME_DEC_NOTHING;
    if (rtc_now < (time_t)TIME_VALID_MIN)              return TIME_DEC_ADOPT;
    if (snap_man && labs((long)snap_t - (long)rtc_now) > TIME_ADOPT_SLACK_S)
                                                       return TIME_DEC_ADOPT;
    if ((int32_t)(snap_t - (uint32_t)rtc_now) > TIME_ADOPT_SLACK_S)
                                                       return TIME_DEC_ADOPT;
    return TIME_DEC_KEEP_RTC;
}

// WRITE half of the contract: epoch + offset + provenance.  Call it after
// every NTP sync, after a manual set, before deep sleep, and on the way out of
// the app (before esp_restart()).
static void time_publish(const char *why, bool manual) {
    time_t now; time(&now);
    if (now < (time_t)TIME_VALID_MIN) return;      // RTC not set yet
    Preferences p;
    if (!p.begin("rtcsave", false)) return;
    uint32_t prev     = p.getULong("t", 0);
    uint8_t  prev_man = p.getUChar("man", 0);
    // A human's correction stays in force while the snapshot still describes
    // the same instant.  Every boot refreshes `t` by a second or so and must
    // not quietly demote the clock back to "machine" -- otherwise the man=1
    // rule above only survives a single reboot.  A machine that lands a
    // genuinely DIFFERENT epoch (an NTP sync) does take the clock over.
    bool machine_moved_it = !prev ||
                            labs((long)now - (long)prev) > TIME_ADOPT_SLACK_S;
    uint8_t man = (manual || (prev_man && !machine_moved_it)) ? 1 : 0;
    p.putULong("t",   (uint32_t)now);
    p.putLong ("gmt", g_tp_gmt);
    p.putUChar("man", man);
    p.putUChar("src", TIME_SRC_ID);
    g_tp_seq = p.getULong("seq", 0) + 1;           // read-modify-write, session open
    p.putULong("seq", g_tp_seq);
    p.end();
    TIME_SERIAL.printf("[time] publish t=%lu gmt=%ld seq=%lu man=%d (%s)\n",
                       (unsigned long)now, (long)g_tp_gmt,
                       (unsigned long)g_tp_seq, man, why);
}

// READ half: reconcile the EPOCH, then ALWAYS re-apply the TIMEZONE, then
// republish so the snapshot never lags the running clock by more than a boot.
// Call this at the very top of setup(), before anything that shows a time.
static void time_restore_from_nvs() {
    time_t rtc_now = 0; time(&rtc_now);

    uint32_t snap_t = 0, snap_seq = 0;
    int32_t  snap_gmt = 0;
    uint8_t  snap_man = 0;
    Preferences p;
    if (p.begin("rtcsave", true)) {
        snap_t   = p.getULong("t",   0);
        snap_gmt = p.getLong ("gmt", 0);
        snap_seq = p.getULong("seq", 0);
        snap_man = p.getUChar("man", 0);
        p.end();
    }
    if (snap_seq == 0 && snap_t) snap_seq = 1;     // pre-v2 snapshot

    uint8_t d = time_decide(rtc_now, snap_t, snap_man);
    if (d == TIME_DEC_ADOPT) {
        struct timeval tv = { (time_t)snap_t, 0 };
        settimeofday(&tv, NULL);                   // writes the chip RTC as well
        g_tp_valid = true;
        TIME_SERIAL.printf("[time] adopted NVS t=%lu (rtc was %ld man=%d)\n",
                           (unsigned long)snap_t, (long)rtc_now, snap_man);
    } else if (d == TIME_DEC_KEEP_RTC) {
        g_tp_valid = true;                         // v6.2 rule: never rewind a live RTC
        TIME_SERIAL.printf("[time] RTC live t=%ld -- NVS kept as fallback\n",
                           (long)rtc_now);
    } else {
        TIME_SERIAL.println("[time] no persisted clock -- waiting for NTP");
    }

    // TZ is the part that never survives a reboot: take it from here, always.
    if (snap_t) time_apply_tz(snap_gmt);
    g_tp_seq = snap_seq;
    time_publish("boot", false);                   // keep the snapshot current
}
// ===== END SHARED TIME BLOCK v2 =============================================
// The shared block owns the clock now; keep SpotifyOS's two call sites
// working.  restoreClock() is the ADOPT half (epoch + timezone +
// publish), saveClock() is the WRITE half.
static uint32_t g_lastClockSaveMs = 0;
static void restoreClock() { time_restore_from_nvs(); }
static void saveClock() {
  time_publish("spotify", false);   // epoch + offset, one writer for "rtcsave"
  g_lastClockSaveMs = millis();
}
/* ---------------------------------------------------------------------------
 * 18. SPLASH + POWER MANAGEMENT + LAUNCHER RETURN
 * ------------------------------------------------------------------------ */
static void buildSplash(const char *status) {
  /* Stage trail: the last line before a reboot is where it died.  The number
   * is how much of the loop task's stack is left (lowest since boot), so you
   * can watch the margin shrink and see which stage eats it. */
  USBSerial.printf("[boot] %-24s free stack %u B\n", status,
                   (unsigned)(uxTaskGetStackHighWaterMark(NULL) * 4));
  lv_obj_t *s = lv_obj_create(nullptr);
  lv_obj_clear_flag(s, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_style_bg_color(s, lv_color_hex(THEME_PRESETS[0].bg), 0);
  lv_obj_set_style_bg_opa(s, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(s, 0, 0);
  lv_scr_load(s);
  lv_obj_t *dot = lv_obj_create(s);
  lv_obj_set_size(dot, 68, 68);
  lv_obj_align(dot, LV_ALIGN_CENTER, 0, -78);
  lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_bg_color(dot, lv_color_hex(THEME_PRESETS[0].accent), 0);
  lv_obj_set_style_border_width(dot, 0, 0);
  lv_obj_t *note = lv_label_create(dot);
  lv_label_set_text(note, LV_SYMBOL_AUDIO);
  lv_obj_set_style_text_font(note, &lv_font_montserrat_28, 0);
  lv_obj_set_style_text_color(note, lv_color_hex(0x101010), 0);
  lv_obj_center(note);
  lv_obj_t *t = lv_label_create(s);
  lv_label_set_text(t, "Spotify OS");
  lv_obj_set_style_text_font(t, &lv_font_montserrat_28, 0);
  lv_obj_set_style_text_color(t, lv_color_hex(THEME_PRESETS[0].text), 0);
  lv_obj_align(t, LV_ALIGN_CENTER, 0, 0);
  lv_obj_t *sub = lv_label_create(s);
  lv_label_set_text(sub, "OFFLINE MUSIC");
  lv_obj_set_style_text_font(sub, &lv_font_montserrat_14, 0);
  lv_obj_set_style_text_color(sub, lv_color_hex(THEME_PRESETS[0].accent), 0);
  lv_obj_align(sub, LV_ALIGN_CENTER, 0, 40);
  lv_obj_t *st = lv_label_create(s);
  lv_label_set_text(st, status);
  lv_obj_set_style_text_font(st, &lv_font_montserrat_12, 0);
  lv_obj_set_style_text_color(st, lv_color_hex(THEME_PRESETS[0].text2), 0);
  lv_obj_align(st, LV_ALIGN_BOTTOM_MID, 0, -40);
  pump();
  fadeBrightness(g_settings.brightness ? g_settings.brightness : DEFAULT_BRIGHTNESS, 220);
  delay(300);
}

static void screenOn() {
  if (!g_screen_off) return;
  g_screen_off = false;
  lv_obj_invalidate(lv_scr_act());
  pump();
  fadeBrightness(g_settings.brightness, 200);
  g_last_touch_ms = g_last_wake_ms = millis();
}
static void screenOff() {
  if (g_screen_off) return;
  g_screen_off = true;
  fadeBrightness(0, 220);
  if (g_touch) g_touch->IIC_Interrupt_Flag = false;
}

/* Saves everything, releases audio, then re-anchors the launcher partition. */
static void returnToLauncher() {
  if (g_returning) return;
  g_returning = true;
  playerAccumulateSession();
  playerStop(true);
  saveSettings();
  saveThemeConfig();
  saveStats();
  saveQueue();
  saveClock();

  lv_obj_t *cover = uiPanel(lv_layer_top(), 0, 0, LCD_W, LCD_H, g_theme.bg, 0);
  lv_obj_set_style_bg_opa(cover, LV_OPA_COVER, 0);
  uiLabel(cover, "SAVING SESSION...", &lv_font_montserrat_22, g_theme.accent, LV_ALIGN_CENTER, 0, -20);
  uiLabel(cover, "RETURNING TO LAUNCHEROS", &lv_font_montserrat_14, g_theme.text2, LV_ALIGN_CENTER, 0, 20);
  pump();

  outputShutdownAll();
  freeArtwork();
  freeWallpaper();
  if (g_sd_ok) SD_MMC.end();

  const esp_partition_t *factory = esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_FACTORY, nullptr);
  if (!factory || esp_ota_set_boot_partition(factory) != ESP_OK) {
    /* fall back to whichever partition we are actually running from so the
     * watch can never end up with no bootable partition */
    const esp_partition_t *self = esp_ota_get_running_partition();
    bool anchorOk = (self && esp_ota_set_boot_partition(self) == ESP_OK);
    lv_obj_del(cover);
    g_returning = false;
    uiModal("RETURN FAILED", anchorOk ? "LauncherOS partition was not found. Staying in SpotifyOS."
                                      : "No bootable partition could be selected. Nothing was changed.",
            "OK", UD(104, 0), "", nullptr);
    return;
  }
  time_publish("exit", false);   // hand the clock back to the launcher
  fadeBrightness(0, 280);
  delay(80);
  esp_restart();
}
/* ---------------------------------------------------------------------------
 * 19. LIVE UI REFRESH
 * ------------------------------------------------------------------------ */
static int g_lastRenderedTrack = -2;
static int g_lastRenderedState = -1;

static void uiLiveUpdate() {
  char cb[24] = "--:--";      /* header clock: no seconds */
  char cb2[24] = "--:--";     /* glance clock: may show seconds */
  char db[32] = "";
  time_t now = time(nullptr);
  if (now >= 1700000000) {
    struct tm t;
    localtime_r(&now, &t);   // LOCAL time: gmtime_r() ignores TZ outright
    if (g_settings.clock24h) {
      snprintf(cb, sizeof(cb), "%02d:%02d", t.tm_hour, t.tm_min);
      snprintf(cb2, sizeof(cb2), "%02d:%02d:%02d", t.tm_hour, t.tm_min, t.tm_sec);
    } else {
      int h12 = t.tm_hour % 12;
      if (!h12) h12 = 12;
      snprintf(cb, sizeof(cb), "%d:%02d %s", h12, t.tm_min, t.tm_hour < 12 ? "AM" : "PM");
      snprintf(cb2, sizeof(cb2), "%d:%02d:%02d %s", h12, t.tm_min, t.tm_sec, t.tm_hour < 12 ? "AM" : "PM");
    }
    strftime(db, sizeof(db), "%a %d %b", &t);
  }
  if (g_clockLbl && lv_obj_is_valid(g_clockLbl)) lv_label_set_text(g_clockLbl, cb);
  if (g_glanceClock && lv_obj_is_valid(g_glanceClock))
    lv_label_set_text(g_glanceClock, g_settings.clockSeconds ? cb2 : cb);
  if (g_glanceDate && lv_obj_is_valid(g_glanceDate)) {
    String d = db;
    d.toUpperCase();
    if (d != g_glanceDateText) {          /* only redraw when the day changes */
      g_glanceDateText = d;
      lv_label_set_text(g_glanceDate, d.c_str());
    }
  }
  if (g_glanceBar && lv_obj_is_valid(g_glanceBar) && g_pbState != PB_STOPPED) {
    uint32_t dur = g_audio.getAudioFileDuration();
    if (dur) {
      uint32_t frac = (uint32_t)((uint64_t)g_audio.getAudioCurrentTime() * 1000ULL / dur);
      if (frac > 1000) frac = 1000;
      lv_bar_set_value(g_glanceBar, (int)frac, LV_ANIM_OFF);
    }
  }
  /* the battery is an I2C read, so it is refreshed on a timer, not every second */
  uint32_t nowMs = millis();
  if (g_glanceBatt && lv_obj_is_valid(g_glanceBatt) && g_has_pmu &&
      (g_glanceStatusMs == 0 || nowMs - g_glanceStatusMs >= 30000UL)) {
    g_glanceStatusMs = nowMs;
    int pct = g_pmu.getBatteryPercent();
    if (pct >= 0) {
      char b[28];
      snprintf(b, sizeof(b), "%d%%%s", pct, g_pmu.isCharging() ? "  charging" : "");
      lv_label_set_text(g_glanceBatt, b);
    }
  }
  if (g_glanceOut && lv_obj_is_valid(g_glanceOut))
    lv_label_set_text(g_glanceOut, g_activeOutput ? g_activeOutput->name() : outputAt(g_settings.output)->name());

  if (g_seekSlider && lv_obj_is_valid(g_seekSlider) && !g_seekDragging && g_pbState != PB_STOPPED) {
    uint32_t dur = g_audio.getAudioFileDuration();
    uint32_t pos = g_audio.getAudioCurrentTime();
    if (dur) {
      uint32_t frac = (uint32_t)((uint64_t)pos * 1000ULL / dur);
      if (frac > 1000) frac = 1000;
      lv_slider_set_value(g_seekSlider, (int)frac, LV_ANIM_OFF);
      if (g_liveLbl && lv_obj_is_valid(g_liveLbl))
        lv_label_set_text(g_liveLbl, (fmtTime(pos) + " / " + fmtTime(dur)).c_str());
    }
  }
}
/* ---------------------------------------------------------------------------
 * 20. SETUP / LOOP
 * ------------------------------------------------------------------------ */
static lv_obj_t *g_splash = nullptr;

void setup() {
  hardwareInit();                        /* display, touch, PMIC, LVGL */
  restoreClock();
  buildSplash("Starting up");
  g_splash = lv_scr_act();

  buildSplash("Mounting SD card");
  if (!storageBegin()) {
    buildSplash("SD CARD NOT FOUND - checking again");
    delay(600);
    storageBegin();
  }
  ensureStorage();

  loadSettings();
  loadThemeConfig();
  loadLiked();
  loadHistory();
  loadStats();
  if (g_settings.rememberQueue) loadQueue();

  buildSplash("Indexing music");
  loadLibraryIndex();
  buildPlaylistsFromIndex();
  if (g_trackCount == 0 || g_playlists.empty()) {
    scanMusic();
    saveLibrary();
  }

  if (g_settings.resumeLast && !g_queue.empty()) {
    int ti = findTrack(g_queue[g_queueIndex]);
    if (ti >= 0) { g_currentTrack = ti; g_pbState = PB_STOPPED; }
  }

  buildSplash("Starting audio output");
  Audio::audio_info_callback = playerOnInfo;
  g_audioInitialised = true;
  if (!outputActivate(g_settings.output)) {
    Serial.printf("[audio] output '%s' unavailable\n", outputAt(g_settings.output)->name());
  }

  /* leave the splash and build the real screen */
  if (g_splash) { lv_obj_del(g_splash); g_splash = nullptr; }
  g_root = lv_obj_create(nullptr);
  lv_obj_clear_flag(g_root, LV_OBJ_FLAG_SCROLLABLE);
  lv_scr_load(g_root);

  g_navTop = -1;
  g_view = g_settings.startInGlance ? V_GLANCE : V_HOME;
  g_viewArg = 0;
  g_statSessions++;
  logBoot();
  uiBuild();
  g_lastRenderedTrack = g_currentTrack;
  g_lastRenderedState = (int)g_pbState;
  fadeBrightness(g_settings.brightness, 200);
  g_last_touch_ms = g_last_wake_ms = millis();
}

static void logBoot() {
  if (!g_sd_ok) return;
  File f = SD_MMC.open(SPOTIFY_APP_DIR "/Logs/boot.log", FILE_APPEND);
  if (!f) return;
  f.printf("%lu,spotifyos,boot,tracks=%d,playlists=%u,sd=%d,pmu=%d,output=%s,heap=%u\n",
           (unsigned long)time(nullptr), g_trackCount, (unsigned)g_playlists.size(), g_sd_ok ? 1 : 0,
           g_has_pmu ? 1 : 0, outputAt(g_settings.output)->name(), (unsigned)ESP.getFreeHeap());
  f.close();
}

void loop() {
  static uint32_t lastTick = 0, lastLive = 0, lastHousekeep = 0, bootPress = 0;
  uint32_t now = millis();
  if (!lastTick) lastTick = now;

#if !defined(LV_TICK_CUSTOM) || (LV_TICK_CUSTOM == 0)
  lv_tick_inc(now - lastTick);
#endif
  lastTick = now;

  playerTick();
  pump();

  if (g_pending_swipe) {
    int ev = g_pending_swipe;
    g_pending_swipe = 0;
    if (ev > 0) { if (g_view != V_NOWPLAYING) uiPush(V_NOWPLAYING, 0); }
    else uiBack();
  }

  if (g_toast && (int32_t)(now - g_toastUntil) >= 0) { lv_obj_del(g_toast); g_toast = nullptr; }

  if (now - lastLive >= 1000UL) {
    lastLive = now;
    uiLiveUpdate();
    if (g_view == V_NOWPLAYING || g_view == V_HOME || g_view == V_QUEUE || g_view == V_GLANCE) {
      if (g_lastRenderedTrack != g_currentTrack || g_lastRenderedState != (int)g_pbState) {
        g_lastRenderedTrack = g_currentTrack;
        g_lastRenderedState = (int)g_pbState;
        uiBuild();
      }
    }
  }

  if (now - lastHousekeep >= 300000UL) {       /* every 5 minutes */
    lastHousekeep = now;
    saveClock();
    saveStats();
    if (g_settings.rememberQueue) saveQueue();
  }

  bool grace = (now - g_last_wake_ms) < WAKE_GRACE_MS;
  if (g_touch_active || grace) g_last_touch_ms = now;
  if (!g_screen_off && (now - g_last_touch_ms) > g_settings.timeoutMs) screenOff();
  else if (g_screen_off && digitalRead(TP_INT) == LOW) screenOn();

  if (digitalRead(BOOT_BTN) == LOW) {
    if (!bootPress) bootPress = now;
    else if (!g_screen_off && now - bootPress > RETURN_HOLD_MS) returnToLauncher();
    else if (g_screen_off) { screenOn(); bootPress = now; }
  } else {
    bootPress = 0;
  }

  delay(2);
}
