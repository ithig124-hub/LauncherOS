// =============================================================================
// TKD OS  -  Taekwondo Watch OS   (LVGL 8.4, ESP32-S3 AMOLED 2.06)   v1.0
// -----------------------------------------------------------------------------
// Hardware : Waveshare ESP32-S3-Touch-AMOLED-2.06 (CO5300 QSPI 410x502, FT3168)
// Framework: Arduino-ESP32, LVGL 8.4, Arduino_GFX, Arduino_DriveBus, SD_MMC
// Concept  : belt-based OS - the whole aesthetic evolves as your Taekwondo
//            rank grows (White -> Yellow -> Green -> Blue -> Red -> Black).
//
// Navigation
//   * Horizontal swipe = infinite app carousel :
//         Watchface  <->  Stretches  <->  Moves  <->  Settings  <->  (loop)
//   * Swipe UP on an app that has a sub-app  ->  sub-app opens
//         (Stretches has sub-app "Workouts")
//   * Swipe DOWN on a sub-app                ->  back to the parent app
//   * Hold BOOT button 1.5 s                 ->  back to launcher (OTA flip)
//
// SD layout (auto-created on first boot if missing) :
//   /WATCH/TKDOS/Theme/WhiteBelt/Fonts/White.ttf         (optional)
//                     /Wallpapers/*.raw                  (belt-locked)
//   /WATCH/TKDOS/Theme/YellowBelt/... (same pattern for each belt)
//   /WATCH/TKDOS/Stretches/Butterfly.raw   (RGB565, 320x320, headerless)
//   /WATCH/TKDOS/Stretches/Forward Splits.raw   ... etc.
//   /WATCH/TKDOS/Moves/           (reserved - moves are text for now)
//   /WATCH/bin/launcheros.bin     (fallback launcher image, optional)
// =============================================================================

#include <Arduino.h>
#include <Wire.h>
#include <SD_MMC.h>
#include <FS.h>
#include <Update.h>
#include <Preferences.h>
#include <esp_ota_ops.h>
#include "esp_partition.h"

#include "pin_config.h"
#include <lvgl.h>
#include "Arduino_GFX_Library.h"
#include "Arduino_DriveBus_Library.h"
#include "XPowersLib.h"
#include <WiFi.h>
#include <time.h>

#include "HWCDC.h"
HWCDC USBSerial;

// =============================================================================
// LauncherOS-port additions: I2C recovery, health flags, watchdog, hand-off
// =============================================================================
#include "esp_task_wdt.h"
#include "esp_system.h"
#include <esp_sleep.h>   // deep-sleep fast-wake detection (skip splash on wake)

// ---- Health flags (guard every I2C-touching call) ---------------------------
static volatile bool g_pmu_ok    = false;   // AXP2101 responding?
static volatile bool g_touch_ok  = false;   // FT3168 responding?
static volatile bool g_switching = false;   // launcher hand-off in flight?

// ---- Rate-limited I2C error logging (log on state change only) --------------
static bool     g_i2c_was_bad     = false;
static uint32_t g_i2c_last_log_ms = 0;
static uint32_t g_i2c_fail_count  = 0;

static void i2c_log_state(bool ok, const char *who) {
  bool bad = !ok;
  if (bad != g_i2c_was_bad) {
    g_i2c_was_bad = bad;
    if (bad) {
      USBSerial.printf("[i2c] %s FAIL (edge)\n", who);
    } else {
      USBSerial.printf("[i2c] %s RECOVERED after %u fails\n",
                       who, (unsigned)g_i2c_fail_count);
      g_i2c_fail_count = 0;
    }
    g_i2c_last_log_ms = millis();
  } else if (bad && millis() - g_i2c_last_log_ms > 10000) {
    // Heartbeat every 10s while stuck bad, so we know it's still stuck.
    USBSerial.printf("[i2c] %s still down (%u fails)\n",
                     who, (unsigned)g_i2c_fail_count);
    g_i2c_last_log_ms = millis();
  }
}

// ---- I2C bus recovery: 9-toggle SCL + STOP + Wire re-init -------------------
// Ported from LauncherOS setup() pattern, made callable on any failure.
static void i2c_bus_recover() {
  USBSerial.println("[i2c] bus recover: 9-toggle SCL + STOP + reinit");
  Wire.end();
  delay(2);

  pinMode(IIC_SCL, OUTPUT_OPEN_DRAIN);
  pinMode(IIC_SDA, INPUT_PULLUP);
  // Clock 9 pulses to release any slave holding SDA low.
  for (int i = 0; i < 9; i++) {
    digitalWrite(IIC_SCL, LOW);  delayMicroseconds(6);
    digitalWrite(IIC_SCL, HIGH); delayMicroseconds(6);
  }
  // Manufacture a STOP condition (SDA low->high while SCL high).
  pinMode(IIC_SDA, OUTPUT_OPEN_DRAIN);
  digitalWrite(IIC_SDA, LOW);  delayMicroseconds(6);
  digitalWrite(IIC_SCL, HIGH); delayMicroseconds(6);
  digitalWrite(IIC_SDA, HIGH); delayMicroseconds(6);

  Wire.begin(IIC_SDA, IIC_SCL);
  Wire.setClock(400000);
  delay(2);
}

// ---- Ping a slave with a bare zero-length write (fast, no side effects) -----
static bool i2c_ping(uint8_t addr) {
  Wire.beginTransmission(addr);
  return (Wire.endTransmission(true) == 0);
}

// ---- Forward decls ----------------------------------------------------------
static void pmu_try_reinit();
static void ui_back_to_launcher_action();

// -- Per-belt display fonts (LVGL 8.4, converted from your Fonts.zip) ----
// (Arduino auto-compiles the tkd_*_34.c files in this sketch folder.)
extern "C" const lv_font_t tkd_white_34;
extern "C" const lv_font_t tkd_yellow_34;
extern "C" const lv_font_t tkd_green_34;
extern "C" const lv_font_t tkd_blue_34;
extern "C" const lv_font_t tkd_red_34;
extern "C" const lv_font_t tkd_black_34;
static const lv_font_t *BELT_FONT[] = {
  &tkd_white_34, &tkd_yellow_34, &tkd_green_34,
  &tkd_blue_34,  &tkd_red_34,    &tkd_black_34
};

// =============================================================================
// BELT / THEME SYSTEM
// =============================================================================
enum Belt : uint8_t {
  BELT_WHITE = 0, BELT_YELLOW = 1, BELT_GREEN = 2,
  BELT_BLUE  = 3, BELT_RED    = 4, BELT_BLACK = 5, BELT_COUNT = 6
};

struct BeltTheme {
  const char *name;         // english
  const char *hangul;       // korean
  const char *tagline;      // "THE BEGINNING", "THE SPARK", ...
  const char *dir;          // SD folder name
  uint32_t   bg;
  uint32_t   card;
  uint32_t   border;
  uint32_t   primary;
  uint32_t   accent;
  uint32_t   text;
  uint32_t   muted;
  bool       dark;          // dark theme?
};

static const BeltTheme BELTS[BELT_COUNT] = {
  // WHITE - "The Beginning" - warm white, minimal, subtle blue accent
  { "WHITE",  "\xE7\x99\xBD\xE5\xB8\xAF", "THE BEGINNING", "WhiteBelt",
    0xF7F7F5, 0xFFFFFF, 0xE5E7EB, 0x171717, 0xB8D8E8, 0x171717, 0x8A8A8A, false },
  // YELLOW - "The Spark" - dark mode w/ golden yellow
  { "YELLOW", "\xE9\xBB\x83\xE5\xB8\xAF", "THE SPARK",     "YellowBelt",
    0x101010, 0x1B1B1B, 0x2A2A2A, 0xFFC400, 0xFFDA44, 0xFFFFFF, 0x9A9A9A, true },
  // GREEN - "The Growth" - deep forest, growth network
  { "GREEN",  "\xE7\xB6\xA0\xE5\xB8\xAF", "THE GROWTH",    "GreenBelt",
    0x06110D, 0x0B1F16, 0x123B27, 0x00A86B, 0x5CFFB0, 0xE6FFF3, 0x5C7F6C, true },
  // BLUE - "The Sky" - infinite futuristic
  { "BLUE",   "\xE9\x9D\x92\xE5\xB8\xAF", "THE SKY",       "BlueBelt",
    0x030712, 0x0A1226, 0x142445, 0x0066FF, 0x00C2FF, 0xE6F3FF, 0x5D6E8C, true },
  // RED - "The Power" - danger, restraint, heartbeat
  { "RED",    "\xE7\xB4\x85\xE5\xB8\xAF", "THE POWER",     "RedBelt",
    0x050505, 0x101010, 0x2A0000, 0xD00000, 0xFF2020, 0xF5F5F5, 0x6E4A4A, true },
  // BLACK - "The Master" - mastery, gold hint, crimson
  { "BLACK",  "\xE9\xBB\x91\xE5\xB8\xAF", "THE MASTER",    "BlackBelt",
    0x030303, 0x0A0A0A, 0x1A0A0A, 0xB89B5E, 0xD00000, 0xE8E8E8, 0x5A5A5A, true }
};

static Preferences NVS;
static Belt        g_belt = BELT_WHITE;

// =============================================================================
// DATA - Stretches (8 hardcoded, .raw filenames match SD card exactly)
// =============================================================================
struct Stretch { const char *name; const char *file; const char *desc; };
static const Stretch STRETCHES[] = {
  { "Butterfly",         "Butterfly.raw",
    "Sit tall, soles of the feet together. Draw heels toward you and let knees drop. Hinge from the hips - inner thighs, groin, lower back. Hold 45-60 s." },
  { "Forward Splits",    "Forward Splits.raw",
    "From a low lunge slide the front heel forward and the back knee back until both legs are straight. Square the hips. Prop under the front thigh if needed. Hold 60 s each side." },
  { "Hip Rotations",     "Hip Rotations.raw",
    "Stand tall, hands on hips. Draw large circles with the hips - 10 clockwise, 10 counter-clockwise. Warms and mobilises the hip capsule before kicks." },
  { "Pigeon",            "Pigeon.raw",
    "From all fours, bring one shin forward at 90 degrees, extend the other leg straight behind. Fold over the shin. Deep glute and hip release. 60 s each side." },
  { "Side Bend",         "Side Bend.raw",
    "Stand tall, reach one arm overhead and lean sideways from the waist. Keep hips square. Opens the obliques and QL - critical for round-house extension." },
  { "Side Splits",       "Side Splits.raw",
    "Feet wider than shoulder width, toes forward. Slide feet apart, dropping the hips between them. Support with hands or blocks. Hold 60-90 s. Never bounce." },
  { "Single Leg Crouch", "Single leg crouch.raw",
    "Squat on one leg, other leg extended straight out in front. Heel stays down. Builds pistol-squat control - the foundation of jump-kick landings." },
  { "Toe Touches",       "Touch touches.raw",
    "Feet together, hinge at the hips and reach for the toes. Soft knees. Do not force - let the hamstrings release breath by breath. 45 s hold." }
};
static const int STRETCH_COUNT = sizeof(STRETCHES) / sizeof(STRETCHES[0]);

// =============================================================================
// DATA - Moves (text only, Korean + English + description)
// =============================================================================
struct Move { const char *category; const char *kor; const char *eng; const char *desc; };
static const Move MOVES[] = {
  // KICKS
  { "KICKS",  "Ap Chagi",           "Front Kick",
    "Chamber the knee high, snap the ball of the foot forward. Return the foot to chamber before setting it down. Speed > power - your first weapon." },
  { "KICKS",  "Dollyeo Chagi",      "Roundhouse Kick",
    "Pivot the support foot 180 degrees, drive the hip through, snap the instep or shin across the target in a horizontal arc. Signature TKD kick." },
  { "KICKS",  "Yeop Chagi",         "Side Kick",
    "Chamber knee tight, rotate hip over, thrust the blade of the foot in a straight line. Strike with the heel - a linear, penetrating kick." },
  { "KICKS",  "Dwit Chagi",         "Back Kick",
    "Look over the shoulder, drive the heel straight backward through the target. Fast, mule-kick style. Devastating counter to a forward rush." },
  { "KICKS",  "Naeryeo Chagi",      "Axe Kick",
    "Raise the leg as high as possible (inside or outside crescent), then drive the heel straight down through the target. Head-height finisher." },
  { "KICKS",  "Huryeo Chagi",       "Hook Kick",
    "Extend the leg past the target, then whip the heel back in a hook motion. Deceptive - looks like a side kick until it curves around the guard." },
  { "KICKS",  "Mireo Chagi",        "Push Kick",
    "Flat-footed shove with the sole. Not a strike - a distance-manager to stop an advance and set up your own attack." },
  { "KICKS",  "An/Bakkat Chagi",    "Inward/Outward Crescent Kick",
    "Sweeping arc kick. Inward crescent clears blocks; outward crescent hooks around a defensive frame. Trains hip mobility." },
  { "KICKS",  "Twi-myo Chagi",      "Flying/Jumping Kick",
    "Any standard kick performed airborne. Fly with the whole body - kick lands as the peak of a jump. Flying side kick is the classic." },
  { "KICKS",  "Kawi Chagi",         "Scissor Kick",
    "Two kicks in mid-air simultaneously - one forward, one backward, opening like scissors. Advanced demonstration kick." },
  // PUNCHES / STRIKES
  { "STRIKES","Ap Jireugi",         "Front Punch",
    "Standard straight punch. Fist rotates palm-down at impact. Push from the hip, retract sharply - the retraction is what makes it snap." },
  { "STRIKES","Momtong Jireugi",    "Body Punch",
    "Same mechanics as Ap Jireugi, targeted to the solar plexus. Middle-section is a scoring zone in sparring - practice depth control." },
  { "STRIKES","Olgul Jireugi",      "High Punch",
    "Upward-angled straight punch aimed at the face. Chin tucked, non-punching hand covering. Do not telegraph with the shoulder." },
  { "STRIKES","Baro Jireugi",       "Reverse / Straight Punch",
    "Baro (straight) = same-side arm & leg forward. Bahndae (reverse) = opposite arm & leg. Baro is faster; bahndae is heavier." },
  { "STRIKES","Doo Bun Chirugi",    "Double Punch",
    "Two rapid consecutive punches, usually low-then-high or high-then-low. Trains recovery and combination flow." },
  { "STRIKES","Sonnal",             "Knifehand",
    "Open-hand chop with the edge of the palm. Tension the fingers, strike with the meaty edge. Excellent for the neck and collarbone." },
  { "STRIKES","Deung Joomuk",       "Backfist",
    "Whipping strike with the back of the knuckles. Loose shoulder, tight fist at impact. Fastest way from guard to the temple." },
  { "STRIKES","Palkup Chigi",       "Elbow Strike",
    "Close-range strike with the point of the elbow. Horizontal, upward, or downward. The most powerful strike per inch of travel." },
  { "STRIKES","Me Joomuk",          "Hammer Fist",
    "Downward strike with the pinky-side of a closed fist. Great for driving through a lowered guard onto the collarbone or bridge of the nose." },
  // BLOCKS
  { "BLOCKS", "Arae Makgi",         "Low Block",
    "Sweep the forearm diagonally downward across the body. Deflects front kicks and low punches. The first block every student learns." },
  { "BLOCKS", "Momtong Makgi",      "Middle Block",
    "Forearm block from the outside inward to guard the mid-section. Rotate the wrist at contact to peel the incoming strike." },
  { "BLOCKS", "Olgul Makgi",        "High Block",
    "Upward forearm block at forehead height, forearm angled 45 deg. Protects against downward and axe strikes." },
  { "BLOCKS", "Sonnal Makgi",       "Knifehand Block",
    "Open-hand version of the middle block - deflects with the edge of the palm, freeing the hand for an immediate grab or counter-strike." },
  { "BLOCKS", "Eotgeoro Makgi",     "X-Block",
    "Both forearms crossed in an X, wrists locked. Absorbs heavy vertical strikes (downward blade / weapon defence). Sink into the stance." },
  // STANCES
  { "STANCE", "Charyot Sogi",       "Attention Stance",
    "Feet together, heels touching, body straight. Stance of respect - used to bow in and out of every training session." },
  { "STANCE", "Chumbi Sogi",        "Ready Stance",
    "Feet shoulder-width apart, fists at belt height. Balanced, relaxed, one breath from action. The default guard between techniques." },
  { "STANCE", "Gunnun Sogi",        "Walking Stance",
    "Long forward stance, 70% weight on the front leg, back leg straight. Platform for punching combinations and forward pressure." },
  { "STANCE", "Dwi Gubi",           "Back Stance",
    "70% weight on the back leg, feet at right angles (L-shape). Defensive - front leg is light and free to fire kicks instantly." },
  { "STANCE", "Juchum Sogi",        "Horse Riding Stance",
    "Wide, low, both knees bent as if riding a horse. Builds leg endurance and hip strength. Home of the classic side-punch drills." }
};
static const int MOVE_COUNT = sizeof(MOVES) / sizeof(MOVES[0]);

// =============================================================================
// DISPLAY + TOUCH  (Waveshare CO5300 + FT3168, single partial LVGL buffer)
// =============================================================================
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

// Manual swipe detector (LauncherOS-parity: no IRQ-flag stomp).
enum SwipeEv : uint8_t { SW_NONE, SW_LEFT, SW_RIGHT, SW_UP, SW_DOWN };
static volatile SwipeEv g_pending_swipe = SW_NONE;
static uint32_t g_lastSwipeMs = 0;
#define SWIPE_THRESHOLD_PX     30
#define SWIPE_MAX_DURATION_MS  800
static int32_t ts_x = 0, ts_y = 0; static uint32_t ts_ms = 0;
static int32_t tc_x = 0, tc_y = 0; static bool touchActive = false;

// Forward decls needed by the touch read callback (real definitions live
// in the SCREEN TIMEOUT section further down the file).
static uint32_t g_last_touch_ms = 0;
static uint32_t g_last_wake_ms  = 0;
static bool     g_screen_off    = false;
static void     screen_on_now();
static void     screen_off_now();

static uint32_t g_lastSwipeEndMs = 0;
#define CLICK_AFTER_SWIPE_MS 200   // LauncherOS post-swipe click guard

static void my_touchpad_read(lv_indev_drv_t *drv, lv_indev_data_t *data) {
  data->state = LV_INDEV_STATE_REL;
  if (g_switching || !g_touch_ok) return;

  int32_t x = 0, y = 0; int fingers = 0;
  x       = FT3168->IIC_Read_Device_Value(FT3168->Arduino_IIC_Touch::Value_Information::TOUCH_COORDINATE_X);
  y       = FT3168->IIC_Read_Device_Value(FT3168->Arduino_IIC_Touch::Value_Information::TOUCH_COORDINATE_Y);
  fingers = FT3168->IIC_Read_Device_Value(FT3168->Arduino_IIC_Touch::Value_Information::TOUCH_FINGER_NUMBER);

  if (fingers < 0 || fingers > 10 || x < 0 || y < 0) {
    g_touch_ok = false;
    i2c_log_state(false, "FT3168");
    return;
  }

  bool pressed = (fingers > 0);
  if (pressed) g_last_touch_ms = millis();
  if (pressed && g_screen_off) { screen_on_now(); return; }

  // Post-swipe click guard - swallow the release-tap after a gesture.
  if (pressed && millis() - g_lastSwipeEndMs < CLICK_AFTER_SWIPE_MS) return;

  static int zero_streak = 0; const int RELEASE_HYSTERESIS = 3;
  if (pressed) zero_streak = 0;
  else if (touchActive) { if (++zero_streak < RELEASE_HYSTERESIS) { pressed = true; x = tc_x; y = tc_y; } }

  if (pressed) {
    data->state = LV_INDEV_STATE_PR; data->point.x = x; data->point.y = y;
    if (!touchActive) { touchActive = true; ts_x = x; ts_y = y; ts_ms = millis(); }
    tc_x = x; tc_y = y;
  } else if (touchActive) {
    touchActive = false;
    uint32_t dur = millis() - ts_ms;
    int32_t dx = tc_x - ts_x, dy = tc_y - ts_y;
    if (dur < SWIPE_MAX_DURATION_MS) {
      if (abs(dx) > SWIPE_THRESHOLD_PX && abs(dx) > abs(dy)) {
        g_pending_swipe  = (dx < 0) ? SW_LEFT : SW_RIGHT;
        g_lastSwipeEndMs = millis();
      } else if (abs(dy) > SWIPE_THRESHOLD_PX && abs(dy) > abs(dx)) {
        g_pending_swipe  = (dy < 0) ? SW_UP : SW_DOWN;
        g_lastSwipeEndMs = millis();
      }
    }
  }
}

// =============================================================================
// THEME  (rebuilt every time the belt changes)
// =============================================================================
static lv_style_t st_screen, st_card, st_title, st_body, st_muted, st_accent, st_pill;

static lv_color_t hex(uint32_t c) { return lv_color_hex(c); }

static const lv_font_t *belt_title_font() {
  // Use the belt-specific TTF-derived font we bundled at build time.
  return BELT_FONT[g_belt];
}
static const lv_font_t *belt_body_font()  { return &lv_font_montserrat_16; }
static const lv_font_t *belt_small_font() { return &lv_font_montserrat_14; }

static void theme_apply() {
  const BeltTheme &t = BELTS[g_belt];

  lv_style_reset(&st_screen);
  lv_style_init(&st_screen);
  lv_style_set_bg_color(&st_screen, hex(t.bg));
  lv_style_set_bg_opa(&st_screen, LV_OPA_COVER);
  lv_style_set_border_width(&st_screen, 0);
  lv_style_set_pad_all(&st_screen, 0);
  lv_style_set_text_color(&st_screen, hex(t.text));
  lv_style_set_text_font(&st_screen, belt_body_font());

  lv_style_reset(&st_card);
  lv_style_init(&st_card);
  lv_style_set_bg_color(&st_card, hex(t.card));
  lv_style_set_bg_opa(&st_card, LV_OPA_COVER);
  lv_style_set_border_color(&st_card, hex(t.border));
  lv_style_set_border_width(&st_card, 1);
  lv_style_set_radius(&st_card, 18);
  lv_style_set_pad_all(&st_card, 14);
  lv_style_set_text_color(&st_card, hex(t.text));

  lv_style_reset(&st_title);
  lv_style_init(&st_title);
  lv_style_set_text_color(&st_title, hex(t.primary));
  lv_style_set_text_font(&st_title, belt_title_font());

  lv_style_reset(&st_body);
  lv_style_init(&st_body);
  lv_style_set_text_color(&st_body, hex(t.text));
  lv_style_set_text_font(&st_body, belt_body_font());

  lv_style_reset(&st_muted);
  lv_style_init(&st_muted);
  lv_style_set_text_color(&st_muted, hex(t.muted));
  lv_style_set_text_font(&st_muted, belt_small_font());

  lv_style_reset(&st_accent);
  lv_style_init(&st_accent);
  lv_style_set_text_color(&st_accent, hex(t.accent));
  lv_style_set_text_font(&st_accent, belt_body_font());

  lv_style_reset(&st_pill);
  lv_style_init(&st_pill);
  lv_style_set_bg_color(&st_pill, hex(t.primary));
  lv_style_set_bg_opa(&st_pill, LV_OPA_COVER);
  lv_style_set_text_color(&st_pill, hex(t.dark ? 0x000000 : 0xFFFFFF));
  lv_style_set_radius(&st_pill, LV_RADIUS_CIRCLE);
  lv_style_set_pad_hor(&st_pill, 18);
  lv_style_set_pad_ver(&st_pill, 10);
  lv_style_set_border_width(&st_pill, 0);
}

// =============================================================================
// SD helpers
// =============================================================================
static bool g_sd_ok = false;

static void ensure_dir(const char *path) {
  if (!g_sd_ok) return;
  if (SD_MMC.exists(path)) return;
  SD_MMC.mkdir(path);
}

static void sd_bootstrap_folders() {
  ensure_dir("/WATCH");
  ensure_dir("/WATCH/TKDOS");
  ensure_dir("/WATCH/TKDOS/Theme");
  for (int i = 0; i < BELT_COUNT; i++) {
    char p[96];
    snprintf(p, sizeof(p), "/WATCH/TKDOS/Theme/%s", BELTS[i].dir); ensure_dir(p);
  }
  ensure_dir("/WATCH/TKDOS/Stretches");
  ensure_dir("/WATCH/TKDOS/Moves");
  ensure_dir("/WATCH/bin");
  ensure_dir("/WATCH/wifi");
}

// =============================================================================
// STRETCH IMAGE LOADER  (headerless RGB565, expected 320x320)
// =============================================================================
#define STRETCH_IMG_W 320
#define STRETCH_IMG_H 320
static lv_color_t   *g_stretch_buf = nullptr;      // PSRAM
static lv_img_dsc_t  g_stretch_dsc;
static int           g_stretch_loaded_idx = -1;

static bool stretch_load_raw(int idx) {
  if (!g_sd_ok || !g_stretch_buf) return false;
  if (idx == g_stretch_loaded_idx) return true;
  char path[128];
  snprintf(path, sizeof(path), "/WATCH/TKDOS/Stretches/%s", STRETCHES[idx].file);
  File f = SD_MMC.open(path, FILE_READ);
  if (!f) { USBSerial.printf("[stretch] missing: %s\n", path); return false; }
  size_t want = (size_t)STRETCH_IMG_W * STRETCH_IMG_H * 2;
  size_t sz   = f.size();
  size_t rd   = (sz < want) ? sz : want;
  f.read((uint8_t *)g_stretch_buf, rd);
  f.close();
  if (rd < want) memset(((uint8_t *)g_stretch_buf) + rd, 0, want - rd);
  g_stretch_dsc.header.always_zero = 0;
  g_stretch_dsc.header.w = STRETCH_IMG_W;
  g_stretch_dsc.header.h = STRETCH_IMG_H;
  g_stretch_dsc.header.cf = LV_IMG_CF_TRUE_COLOR;
  g_stretch_dsc.data_size = want;
  g_stretch_dsc.data = (const uint8_t *)g_stretch_buf;
  g_stretch_loaded_idx = idx;
  return true;
}

// =============================================================================
// PMU  (AXP2101 via XPowersLib - LauncherOS-parity, IRQ-driven with poll fallback)
// =============================================================================
static XPowersPMU pmu;
static uint8_t g_bat_pct = 0;
static bool    g_charging = false;
static uint32_t g_bat_last_ms = 0;

static volatile bool g_pmu_irq_pending = false;
static uint32_t      g_pmu_last_reinit_ms = 0;

static void IRAM_ATTR pmu_irq_isr() { g_pmu_irq_pending = true; }

static void pmu_setup() {
  g_pmu_ok = pmu.begin(Wire, AXP2101_SLAVE_ADDRESS, IIC_SDA, IIC_SCL);
  i2c_log_state(g_pmu_ok, "AXP2101");
  if (!g_pmu_ok) return;

  pmu.disableTSPinMeasure();
  pmu.enableBattDetection();
  pmu.enableBattVoltageMeasure();
  pmu.enableSystemVoltageMeasure();
  pmu.enableVbusVoltageMeasure();

  // IRQ-driven power-key (LauncherOS pattern)
  pmu.disableIRQ(XPOWERS_AXP2101_ALL_IRQ);
  pmu.clearIrqStatus();
  pmu.enableIRQ(XPOWERS_AXP2101_PKEY_SHORT_IRQ |
                XPOWERS_AXP2101_PKEY_LONG_IRQ);

#ifdef PMU_IRQ_PIN
  pinMode(PMU_IRQ_PIN, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(PMU_IRQ_PIN), pmu_irq_isr, FALLING);
#endif

  USBSerial.println("[pmu] AXP2101 ready");
}

static void pmu_try_reinit() {
  // Rate-limit reinit attempts to once every 5s, otherwise we spam I2C.
  if (millis() - g_pmu_last_reinit_ms < 5000) return;
  g_pmu_last_reinit_ms = millis();

  i2c_bus_recover();

  if (!i2c_ping(AXP2101_SLAVE_ADDRESS)) {
    g_pmu_ok = false;
    i2c_log_state(false, "AXP2101");
    return;
  }
  // Bus is back - reinit the driver.
  bool ok = pmu.begin(Wire, AXP2101_SLAVE_ADDRESS, IIC_SDA, IIC_SCL);
  if (ok) {
    pmu.disableTSPinMeasure();
    pmu.enableBattDetection();
    pmu.enableBattVoltageMeasure();
    pmu.enableSystemVoltageMeasure();
    pmu.enableVbusVoltageMeasure();
    pmu.disableIRQ(XPOWERS_AXP2101_ALL_IRQ);
    pmu.clearIrqStatus();
    pmu.enableIRQ(XPOWERS_AXP2101_PKEY_SHORT_IRQ |
                  XPOWERS_AXP2101_PKEY_LONG_IRQ);
  }
  g_pmu_ok = ok;
  i2c_log_state(g_pmu_ok, "AXP2101");
}

// Called from loop() every ~3s AND on IRQ.
static void pmu_poll() {
  if (g_switching) return;   // don't touch I2C once hand-off started

  // IRQ-driven side path (short-press = wake/sleep, long-press = launcher)
  if (g_pmu_irq_pending && g_pmu_ok) {
    g_pmu_irq_pending = false;
    pmu.getIrqStatus();
    if (pmu.isPekeyShortPressIrq()) {
      USBSerial.println("[pmu] PKEY short");
      if (g_screen_off) { screen_on_now(); g_last_touch_ms = millis(); }
      else              { screen_off_now(); }
    }
    if (pmu.isPekeyLongPressIrq()) {
      USBSerial.println("[pmu] PKEY long -> launcher");
      pmu.clearIrqStatus();
      ui_back_to_launcher_action();
      return;
    }
    pmu.clearIrqStatus();
  }

  // 3s battery poll (throttled)
  if (millis() - g_bat_last_ms < 3000) return;
  g_bat_last_ms = millis();

  if (!g_pmu_ok) { pmu_try_reinit(); return; }

  // Guard every hardware call. XPowersLib returns int on I2C fail;
  // a wildly-out-of-range read is our tripwire.
  int pct = pmu.getBatteryPercent();
  if (pct < 0 || pct > 100) {
    g_i2c_fail_count++;
    i2c_log_state(false, "AXP2101");
    g_pmu_ok = false;
    pmu_try_reinit();
    return;
  }
  g_bat_pct  = (uint8_t)pct;
  g_charging = pmu.isVbusIn();
  i2c_log_state(true, "AXP2101");
}

// =============================================================================
// SCREEN TIMEOUT + BRIGHTNESS FADE  (LauncherOS-parity)
// =============================================================================
#define DEFAULT_BRIGHTNESS       140
#define SCREEN_OFF_TIMEOUT_MS  15000     // 15 s of no touch -> screen off
#define WAKE_GRACE_MS           2000

static uint8_t  g_brightness      = DEFAULT_BRIGHTNESS;
// g_screen_off / g_last_touch_ms / g_last_wake_ms are forward-declared above
// the touch handler so it can bump/read them.

static void fade_brightness(uint8_t target, uint32_t dur_ms) {
  int from = g_brightness, to = target;
  if (from == to) return;
  const int steps = 12;
  int step_ms = (int)dur_ms / steps; if (step_ms < 1) step_ms = 1;
  for (int i = 1; i <= steps; i++) {
    int b = from + ((to - from) * i) / steps;
    if (b < 0) b = 0; if (b > 255) b = 255;
    static_cast<Arduino_CO5300 *>(gfx)->setBrightness((uint8_t)b);
    g_brightness = (uint8_t)b;
    lv_timer_handler();
    delay(step_ms);
  }
  static_cast<Arduino_CO5300 *>(gfx)->setBrightness(target);
  g_brightness = target;
}
static void screen_off_now() {
  if (g_screen_off) return;
  g_screen_off = true;
  fade_brightness(0, 220);
  FT3168->IIC_Interrupt_Flag = false;
}
static void screen_on_now() {
  if (!g_screen_off) return;
  g_screen_off = false;
  lv_obj_invalidate(lv_scr_act());
  lv_timer_handler();
  fade_brightness(DEFAULT_BRIGHTNESS, 200);
  g_last_touch_ms = millis();
  g_last_wake_ms  = millis();
}

// =============================================================================
// WIFI / NTP / persistent wall-clock  (LauncherOS parity)
// =============================================================================
struct WifiNet { String ssid, pass; };
static WifiNet    g_nets[5];
static int        g_net_count      = 0;
static int        g_gmt_offset_sec = 0;
static bool       g_wifi_connected = false;
static bool       g_ntp_synced     = false;
static uint32_t   g_last_sync_ms   = 0;

// >>> Hardcoded WiFi fallback if /WATCH/wifi/config.txt is absent <<<
static const char *HC_WIFI_SSID = "Optus_9D2E3D";
static const char *HC_WIFI_PASS = "snucktemptGLeQU";
#define WIFI_RESYNC_INTERVAL_MS  (12UL * 60UL * 60UL * 1000UL)   // 12 h

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
// The block owns the clock now; keep TKDOS's own call site working.
static void time_save_now(const char *why) { time_publish(why, false); }
static void parse_wifi_config() {
  if (!g_sd_ok) return;
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
          g_nets[idx].pass = val;
          g_net_count++;
        }
      }
    }
    if (key == "GMT_OFFSET") g_gmt_offset_sec = val.toInt() * 3600;
  }
  f.close();
}
static void wifi_shutdown() {
  WiFi.disconnect(true, true); WiFi.mode(WIFI_OFF); g_wifi_connected = false;
}
static void wifi_connect_and_sync() {
  WiFi.mode(WIFI_STA); WiFi.setSleep(WIFI_PS_MIN_MODEM);
  WiFi.setTxPower(WIFI_POWER_8_5dBm); WiFi.persistent(false);
  for (int i = 0; i < g_net_count && !g_wifi_connected; i++) {
    USBSerial.printf("[wifi] try %s\n", g_nets[i].ssid.c_str());
    WiFi.begin(g_nets[i].ssid.c_str(), g_nets[i].pass.c_str());
    uint32_t u = millis() + 6000;
    while (millis() < u && WiFi.status() != WL_CONNECTED) delay(100);
    g_wifi_connected = (WiFi.status() == WL_CONNECTED);
  }
  if (!g_wifi_connected) {
    USBSerial.printf("[wifi] fallback %s\n", HC_WIFI_SSID);
    WiFi.begin(HC_WIFI_SSID, HC_WIFI_PASS);
    uint32_t u = millis() + 6000;
    while (millis() < u && WiFi.status() != WL_CONNECTED) delay(100);
    g_wifi_connected = (WiFi.status() == WL_CONNECTED);
  }
  if (g_wifi_connected) {
    time_apply_tz(g_gmt_offset_sec);   // pin the TZ string: the timezone is RAM-only and dies every reboot
    configTime(g_gmt_offset_sec, 0, "pool.ntp.org");
    struct tm tm;
    for (int i = 0; i < 30 && !g_ntp_synced; i++)
      if (getLocalTime(&tm, 200)) g_ntp_synced = true;
    if (g_ntp_synced) time_save_now("ntp ok");
  }
  g_last_sync_ms = millis();
  wifi_shutdown();
}
static void wifi_resync_if_due() {
  if (millis() - g_last_sync_ms < WIFI_RESYNC_INTERVAL_MS) return;
  g_wifi_connected = false;
  // Detach from the 8s task WDT while the (potentially 18s) blocking sync runs.
  esp_task_wdt_delete(NULL);
  wifi_connect_and_sync();
  esp_task_wdt_add(NULL);
}

// =============================================================================
// BELT WALLPAPER LOADER  (RGB565 410x502, headerless OR 4-byte LVGL header)
// Path: /WATCH/TKDOS/Theme/<Belt>/*.raw   (NVS override key: wp_<beltIndex>)
// =============================================================================
#define WP_W  410
#define WP_H  502

static uint8_t      *g_wp_buf       = nullptr;
static lv_img_dsc_t  g_wp_dsc;
static Belt          g_wp_loaded_for = BELT_COUNT;   // invalid sentinel
static bool          g_wp_present    = false;

static bool wallpaper_load_for_belt(int belt_int) {
  Belt b = (Belt)belt_int;
  if (b == g_wp_loaded_for) return g_wp_present;
  g_wp_loaded_for = b;
  g_wp_present    = false;

  if (!g_sd_ok || !g_wp_buf) {
    USBSerial.printf("[wp] abort: sd_ok=%d buf=%p\n", g_sd_ok, g_wp_buf);
    return false;
  }

  // Prefer NVS-remembered pick, else first .raw in the belt's folder.
  String chosen;
  NVS.begin("tkdos", true);
  char key[16]; snprintf(key, sizeof(key), "wp_%d", (int)b);
  chosen = NVS.getString(key, "");
  NVS.end();

  if (!chosen.length() || !SD_MMC.exists(chosen.c_str())) {
    char dir[96];
    snprintf(dir, sizeof(dir), "/WATCH/TKDOS/Theme/%s", BELTS[b].dir);
    USBSerial.printf("[wp] scan %s\n", dir);
    File d = SD_MMC.open(dir);
    if (d && d.isDirectory()) {
      File f = d.openNextFile();
      while (f) {
        if (!f.isDirectory()) {
          String n = f.name();
          if (n.endsWith(".raw") || n.endsWith(".RAW")) {
            String base = n.substring(n.lastIndexOf('/') + 1);
            chosen = String(dir) + "/" + base;
            USBSerial.printf("[wp] picked %s (%u bytes)\n",
                             chosen.c_str(), (unsigned)f.size());
            break;
          }
        }
        f = d.openNextFile();
      }
      if (d) d.close();
    } else {
      USBSerial.println("[wp] dir open FAILED");
    }
  }
  if (!chosen.length()) { USBSerial.println("[wp] no .raw found"); return false; }

  File f = SD_MMC.open(chosen.c_str(), FILE_READ);
  if (!f) { USBSerial.printf("[wp] open FAILED: %s\n", chosen.c_str()); return false; }

  size_t need  = (size_t)WP_W * WP_H * 2;   // 411,640 bytes
  size_t fsize = f.size();

  // Auto-detect and skip the 4-byte LVGL image header
  // (the LVGL converter's "Binary RGB565" output prepends one)
  if (fsize == need + 4) {
    f.seek(4);
    fsize -= 4;
    USBSerial.println("[wp] stripped 4-byte LVGL header");
  }

  size_t rd = fsize < need ? fsize : need;
  f.read(g_wp_buf, rd);
  f.close();
  if (rd < need) memset(g_wp_buf + rd, 0, need - rd);

  g_wp_dsc.header.always_zero = 0;
  g_wp_dsc.header.w  = WP_W;
  g_wp_dsc.header.h  = WP_H;
  g_wp_dsc.header.cf = LV_IMG_CF_TRUE_COLOR;
  g_wp_dsc.data_size = need;
  g_wp_dsc.data      = g_wp_buf;

  g_wp_present = true;
  USBSerial.printf("[wp] loaded OK (%u bytes)\n", (unsigned)rd);
  return true;
}

// =============================================================================
// APP CAROUSEL  (infinite horizontal loop)
// =============================================================================
enum App : uint8_t { APP_WATCHFACE = 0, APP_STRETCHES, APP_MOVES, APP_SETTINGS, APP_COUNT };
static App g_app = APP_WATCHFACE;

// Sub-app tracking (swipe UP opens, swipe DOWN closes).
enum Sub : uint8_t { SUB_NONE = 0, SUB_STRETCH_DETAIL, SUB_WORKOUT, SUB_MOVE_DETAIL,
                     SUB_THEME_PICKER, SUB_WALLPAPER_PICKER };
static Sub g_sub = SUB_NONE;
static int g_stretch_idx = 0;
static int g_move_idx    = 0;

// Forward decls of UI builders
static void ui_boot_splash();
static void ui_watchface();
static void ui_stretches_list();
static void ui_stretch_detail();
static void ui_workout();
static void ui_moves_list();
static void ui_move_detail();
static void ui_settings();
static void ui_theme_picker();
static void ui_wallpaper_picker();
static void ui_load_current();

// =============================================================================
// SHARED UI PIECES
// =============================================================================
static lv_obj_t *make_screen() {
  lv_obj_t *s = lv_obj_create(nullptr);
  lv_obj_add_style(s, &st_screen, 0);
  lv_obj_clear_flag(s, LV_OBJ_FLAG_SCROLLABLE);
  return s;
}

static void add_status_bar(lv_obj_t *parent, const char *title) {
  const BeltTheme &t = BELTS[g_belt];
  lv_obj_t *bar = lv_obj_create(parent);
  lv_obj_remove_style_all(bar);
  lv_obj_set_size(bar, LCD_WIDTH, 46);
  lv_obj_align(bar, LV_ALIGN_TOP_MID, 0, 0);
  lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);

  lv_obj_t *belt_pill = lv_obj_create(bar);
  lv_obj_remove_style_all(belt_pill);
  lv_obj_set_size(belt_pill, 76, 26);
  lv_obj_align(belt_pill, LV_ALIGN_LEFT_MID, 18, 0);
  lv_obj_set_style_bg_color(belt_pill, hex(t.primary), 0);
  lv_obj_set_style_bg_opa(belt_pill, LV_OPA_COVER, 0);
  lv_obj_set_style_radius(belt_pill, LV_RADIUS_CIRCLE, 0);

  lv_obj_t *bl = lv_label_create(belt_pill);
  lv_label_set_text(bl, BELTS[g_belt].name);
  lv_obj_set_style_text_color(bl, hex(t.dark ? 0x000000 : 0xFFFFFF), 0);
  lv_obj_set_style_text_font(bl, &lv_font_montserrat_14, 0);
  lv_obj_center(bl);

  // Battery pill top-right of status bar
  lv_obj_t *bat = lv_obj_create(bar);
  lv_obj_remove_style_all(bat);
  lv_obj_set_size(bat, 62, 24);
  lv_obj_align(bat, LV_ALIGN_RIGHT_MID, -110, 0);
  lv_obj_set_style_bg_opa(bat, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_color(bat, hex(t.muted), 0);
  lv_obj_set_style_border_width(bat, 1, 0);
  lv_obj_set_style_radius(bat, 8, 0);
  lv_obj_t *batlab = lv_label_create(bat);
  char bb[16];
  if (g_charging) snprintf(bb, sizeof(bb), "%d%% +", g_bat_pct);
  else            snprintf(bb, sizeof(bb), "%d%%",   g_bat_pct);
  lv_label_set_text(batlab, bb);
  lv_obj_set_style_text_color(batlab, g_bat_pct < 20 ? hex(0xFF5470) : hex(t.text), 0);
  lv_obj_set_style_text_font(batlab, &lv_font_montserrat_14, 0);
  lv_obj_center(batlab);

  lv_obj_t *tl = lv_label_create(bar);
  lv_label_set_text(tl, title);
  lv_obj_set_style_text_color(tl, hex(t.text), 0);
  lv_obj_set_style_text_font(tl, &lv_font_montserrat_16, 0);
  lv_obj_align(tl, LV_ALIGN_RIGHT_MID, -22, 0);
}

// =============================================================================
// UI - BOOT SPLASH  (per-belt signature reveal)
// =============================================================================
static void ui_boot_splash() {
  const BeltTheme &t = BELTS[g_belt];
  lv_obj_t *s = make_screen();

  // Belt name (big) - uses the belt-specific bundled TTF font
  lv_obj_t *big = lv_label_create(s);
  lv_label_set_text(big, t.name);
  lv_obj_set_style_text_font(big, belt_title_font(), 0);
  lv_obj_set_style_text_color(big, hex(t.primary), 0);
  lv_obj_align(big, LV_ALIGN_CENTER, 0, -30);

  // Hangul
  lv_obj_t *k = lv_label_create(s);
  lv_label_set_text(k, t.hangul);
  lv_obj_set_style_text_font(k, belt_title_font(), 0);
  lv_obj_set_style_text_color(k, hex(t.accent), 0);
  lv_obj_align(k, LV_ALIGN_CENTER, 0, 20);

  // tagline
  lv_obj_t *tag = lv_label_create(s);
  lv_label_set_text(tag, t.tagline);
  lv_obj_set_style_text_font(tag, &lv_font_montserrat_14, 0);
  lv_obj_set_style_text_color(tag, hex(t.muted), 0);
  lv_obj_align(tag, LV_ALIGN_CENTER, 0, 60);

  // signature circle-expand animation (works for every belt)
  lv_obj_t *ring = lv_obj_create(s);
  lv_obj_remove_style_all(ring);
  lv_obj_set_size(ring, 8, 8);
  lv_obj_align(ring, LV_ALIGN_CENTER, 0, 130);
  lv_obj_set_style_bg_color(ring, hex(t.primary), 0);
  lv_obj_set_style_bg_opa(ring, LV_OPA_COVER, 0);
  lv_obj_set_style_radius(ring, LV_RADIUS_CIRCLE, 0);

  lv_anim_t a; lv_anim_init(&a);
  lv_anim_set_var(&a, ring);
  lv_anim_set_values(&a, 8, 260);
  lv_anim_set_time(&a, 900);
  lv_anim_set_exec_cb(&a, [](void *o, int32_t v){ lv_obj_set_size((lv_obj_t*)o, v, v); });
  lv_anim_start(&a);

  lv_scr_load(s);
  uint32_t t0 = millis();
  while (millis() - t0 < 1400) { lv_timer_handler(); delay(10); }
}

// =============================================================================
// UI - WATCHFACE
// =============================================================================
static lv_obj_t *g_wf_time = nullptr;
static lv_obj_t *g_wf_date = nullptr;

static void ui_watchface() {
  const BeltTheme &t = BELTS[g_belt];
  lv_obj_t *s = make_screen();

  // Belt wallpaper as background (loaded from /WATCH/TKDOS/Theme/<Belt>/*.raw).
  // BUGFIX: the previous version applied lv_img_set_zoom(wp, 640) -- a 2.5x
  // transform on a 320x320 source. LVGL 8.4 renders zoomed images through
  // the partial buffer line-by-line and on a 60-line buffer at that scale
  // it can produce a garbled / all-black frame on the CO5300 (LauncherOS
  // never zooms its wallpapers -- see refresh_wallpaper()). Draw the
  // wallpaper 1:1 centered on the theme background instead. For a full
  // bleed wallpaper, pre-scale the .raw to LCD_WIDTH x LCD_HEIGHT.
  if (wallpaper_load_for_belt(g_belt)) {
    lv_obj_t *wp = lv_img_create(s);
    lv_img_set_src(wp, &g_wp_dsc);
    lv_obj_center(wp);
    lv_obj_move_background(wp);
    // Slight dim overlay so the clock stays legible on busy wallpapers.
    lv_obj_t *dim = lv_obj_create(s);
    lv_obj_remove_style_all(dim);
    lv_obj_set_size(dim, LCD_WIDTH, LCD_HEIGHT);
    lv_obj_center(dim);
    lv_obj_set_style_bg_color(dim, hex(0x000000), 0);
    lv_obj_set_style_bg_opa(dim, LV_OPA_20, 0);
    lv_obj_clear_flag(dim, LV_OBJ_FLAG_CLICKABLE);
  }

  // Big clock (real RTC when NTP-synced, session counter otherwise).
  // NOTE: belt_title_font() is the bundled tkd_*_34 font which only contains
  // the belt-name glyphs and Korean hangul -- it has NO digits or colon, so
  // using it for the time renders blank. Use lv_font_montserrat_48 which
  // ships with LVGL and has the full ASCII range.
 g_wf_time = lv_label_create(s);
lv_label_set_text(g_wf_time, "12:34");
lv_obj_set_style_text_font(g_wf_time, &lv_font_montserrat_48, 0);
lv_obj_set_style_text_color(g_wf_time, hex(t.primary), 0);
lv_obj_set_style_text_opa(g_wf_time, LV_OPA_COVER, 0);
lv_obj_center(g_wf_time);
lv_obj_move_foreground(g_wf_time);

USBSerial.printf("[wf] time label created (montserrat_48)\n");
  // stash a hidden date label so watchface_tick() can safely write to it
  // (we no longer paint it on-screen per user request).
  g_wf_date = lv_label_create(s);
  lv_obj_add_flag(g_wf_date, LV_OBJ_FLAG_HIDDEN);

  // Battery pill top-right (kept - it's a status element, not chrome).
  lv_obj_t *bpill = lv_obj_create(s);
  lv_obj_remove_style_all(bpill);
  lv_obj_set_size(bpill, 72, 26);
  lv_obj_align(bpill, LV_ALIGN_TOP_RIGHT, -20, 20);
  lv_obj_set_style_bg_opa(bpill, LV_OPA_60, 0);
  lv_obj_set_style_bg_color(bpill, hex(t.card), 0);
  lv_obj_set_style_border_color(bpill, hex(t.primary), 0);
  lv_obj_set_style_border_width(bpill, 1, 0);
  lv_obj_set_style_radius(bpill, LV_RADIUS_CIRCLE, 0);
  lv_obj_t *bl = lv_label_create(bpill);
  char bb[16];
  if (g_charging) snprintf(bb, sizeof(bb), "%d%% +", g_bat_pct);
  else            snprintf(bb, sizeof(bb), "%d%%",   g_bat_pct);
  lv_label_set_text(bl, bb);
  lv_obj_set_style_text_font(bl, &lv_font_montserrat_14, 0);
  lv_obj_set_style_text_color(bl, g_bat_pct < 20 ? hex(0xFF5470) : hex(t.text), 0);
  lv_obj_center(bl);

  // Tiny NTP-sync dot top-left (green = synced).
  lv_obj_t *dot = lv_obj_create(s);
  lv_obj_remove_style_all(dot);
  lv_obj_set_size(dot, 8, 8);
  lv_obj_align(dot, LV_ALIGN_TOP_LEFT, 22, 26);
  lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_bg_color(dot, hex(g_ntp_synced ? 0x00FF80 : t.muted), 0);
  lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);

  lv_scr_load(s);
}

static void watchface_tick() {
  if (!g_wf_time) return;
  struct tm tm;
  if (g_ntp_synced && getLocalTime(&tm, 5)) {
    char buf[16]; snprintf(buf, sizeof(buf), "%02d:%02d", tm.tm_hour, tm.tm_min);
    lv_label_set_text(g_wf_time, buf);
    static const char *DOW[] = {"SUN","MON","TUE","WED","THU","FRI","SAT"};
    static const char *MON[] = {"JAN","FEB","MAR","APR","MAY","JUN",
                                "JUL","AUG","SEP","OCT","NOV","DEC"};
    char db[32];
    snprintf(db, sizeof(db), "%s  %d %s", DOW[tm.tm_wday], tm.tm_mday, MON[tm.tm_mon]);
    lv_label_set_text(g_wf_date, db);
  } else {
    uint32_t s = millis() / 1000;
    uint32_t hh = (s / 3600) % 24, mm = (s / 60) % 60, ss = s % 60;
    char buf[16]; snprintf(buf, sizeof(buf), "%02lu:%02lu", (unsigned long)hh, (unsigned long)mm);
    lv_label_set_text(g_wf_time, buf);
    char db[24]; snprintf(db, sizeof(db), "syncing... %02lus", (unsigned long)ss);
    lv_label_set_text(g_wf_date, db);
  }
}

// =============================================================================
// UI - STRETCHES LIST
// =============================================================================
static void on_stretch_row(lv_event_t *e) {
  int idx = (int)(intptr_t)lv_event_get_user_data(e);
  g_stretch_idx = idx;
  g_sub = SUB_STRETCH_DETAIL;
  ui_stretch_detail();
}

static void ui_stretches_list() {
  const BeltTheme &t = BELTS[g_belt];
  lv_obj_t *s = make_screen();
  add_status_bar(s, "STRETCHES");

  lv_obj_t *list = lv_obj_create(s);
  lv_obj_remove_style_all(list);
  lv_obj_set_size(list, LCD_WIDTH - 40, LCD_HEIGHT - 110);
  lv_obj_align(list, LV_ALIGN_TOP_MID, 0, 60);
  lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_row(list, 10, 0);
  lv_obj_set_style_pad_all(list, 6, 0);

  for (int i = 0; i < STRETCH_COUNT; i++) {
    lv_obj_t *row = lv_obj_create(list);
    lv_obj_add_style(row, &st_card, 0);
    lv_obj_set_size(row, LV_PCT(100), 60);
    lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(row, on_stretch_row, LV_EVENT_CLICKED, (void*)(intptr_t)i);

    lv_obj_t *num = lv_label_create(row);
    char nb[8]; snprintf(nb, sizeof(nb), "%02d", i + 1);
    lv_label_set_text(num, nb);
    lv_obj_set_style_text_color(num, hex(t.primary), 0);
    lv_obj_set_style_text_font(num, &lv_font_montserrat_20, 0);
    lv_obj_align(num, LV_ALIGN_LEFT_MID, 0, 0);

    lv_obj_t *name = lv_label_create(row);
    lv_label_set_text(name, STRETCHES[i].name);
    lv_obj_set_style_text_color(name, hex(t.text), 0);
    lv_obj_set_style_text_font(name, &lv_font_montserrat_16, 0);
    lv_obj_align(name, LV_ALIGN_LEFT_MID, 50, 0);
  }

  // hint
  lv_obj_t *hint = lv_label_create(s);
  lv_label_set_text(hint, "Swipe UP for Workouts");
  lv_obj_set_style_text_color(hint, hex(t.muted), 0);
  lv_obj_set_style_text_font(hint, &lv_font_montserrat_14, 0);
  lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -18);

  lv_scr_load(s);
}

// =============================================================================
// UI - STRETCH DETAIL  (loads .raw image + caption)
// =============================================================================
static void on_stretch_prev(lv_event_t*) { g_stretch_idx = (g_stretch_idx - 1 + STRETCH_COUNT) % STRETCH_COUNT; ui_stretch_detail(); }
static void on_stretch_next(lv_event_t*) { g_stretch_idx = (g_stretch_idx + 1) % STRETCH_COUNT; ui_stretch_detail(); }

static void ui_stretch_detail() {
  const BeltTheme &t = BELTS[g_belt];
  const Stretch &st = STRETCHES[g_stretch_idx];
  lv_obj_t *s = make_screen();
  add_status_bar(s, st.name);

  // image (or placeholder)
  lv_obj_t *img_card = lv_obj_create(s);
  lv_obj_add_style(img_card, &st_card, 0);
  lv_obj_set_size(img_card, LCD_WIDTH - 40, 260);
  lv_obj_align(img_card, LV_ALIGN_TOP_MID, 0, 60);
  lv_obj_clear_flag(img_card, LV_OBJ_FLAG_SCROLLABLE);

  if (stretch_load_raw(g_stretch_idx)) {
    lv_obj_t *img = lv_img_create(img_card);
    lv_img_set_src(img, &g_stretch_dsc);
    lv_obj_center(img);
  } else {
    lv_obj_t *ph = lv_label_create(img_card);
    char buf[128];
    snprintf(buf, sizeof(buf), "%s\n\n(drop %s\ninto /WATCH/TKDOS/Stretches)", st.name, st.file);
    lv_label_set_text(ph, buf);
    lv_obj_set_style_text_align(ph, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(ph, hex(t.muted), 0);
    lv_obj_center(ph);
  }

  // caption card
  lv_obj_t *cap = lv_obj_create(s);
  lv_obj_add_style(cap, &st_card, 0);
  lv_obj_set_size(cap, LCD_WIDTH - 40, 130);
  lv_obj_align(cap, LV_ALIGN_TOP_MID, 0, 330);
  lv_obj_t *dsc = lv_label_create(cap);
  lv_label_set_text(dsc, st.desc);
  lv_label_set_long_mode(dsc, LV_LABEL_LONG_WRAP);
  lv_obj_set_width(dsc, LCD_WIDTH - 80);
  lv_obj_set_style_text_color(dsc, hex(t.text), 0);
  lv_obj_set_style_text_font(dsc, &lv_font_montserrat_14, 0);
  lv_obj_align(dsc, LV_ALIGN_TOP_LEFT, 0, 0);

  // prev/next tap zones
  lv_obj_t *lz = lv_obj_create(s);
  lv_obj_remove_style_all(lz); lv_obj_set_size(lz, 40, 260);
  lv_obj_align(lz, LV_ALIGN_TOP_LEFT, 0, 60);
  lv_obj_add_flag(lz, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(lz, on_stretch_prev, LV_EVENT_CLICKED, nullptr);

  lv_obj_t *rz = lv_obj_create(s);
  lv_obj_remove_style_all(rz); lv_obj_set_size(rz, 40, 260);
  lv_obj_align(rz, LV_ALIGN_TOP_RIGHT, 0, 60);
  lv_obj_add_flag(rz, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(rz, on_stretch_next, LV_EVENT_CLICKED, nullptr);

  lv_scr_load(s);
}

// =============================================================================
// UI - WORKOUT  (randomised reps of 4 stretches)
// =============================================================================
static void on_workout_reroll(lv_event_t*);

static void ui_workout() {
  const BeltTheme &t = BELTS[g_belt];
  lv_obj_t *s = make_screen();
  add_status_bar(s, "WORKOUT");

  lv_obj_t *title = lv_label_create(s);
  lv_label_set_text(title, "TODAY'S SET");
  lv_obj_set_style_text_color(title, hex(t.primary), 0);
  lv_obj_set_style_text_font(title, &lv_font_montserrat_20, 0);
  lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 60);

  lv_obj_t *card = lv_obj_create(s);
  lv_obj_add_style(card, &st_card, 0);
  lv_obj_set_size(card, LCD_WIDTH - 40, 330);
  lv_obj_align(card, LV_ALIGN_TOP_MID, 0, 100);
  lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_row(card, 10, 0);

  // shuffle indices
  int order[STRETCH_COUNT]; for (int i = 0; i < STRETCH_COUNT; i++) order[i] = i;
  for (int i = STRETCH_COUNT - 1; i > 0; i--) {
    int j = esp_random() % (i + 1);
    int tmp = order[i]; order[i] = order[j]; order[j] = tmp;
  }
  const int SETS = 4;
  for (int i = 0; i < SETS; i++) {
    int reps = 6 + (int)(esp_random() % 9); // 6..14
    int hold = 20 + (int)(esp_random() % 41); // 20..60 seconds
    lv_obj_t *row = lv_obj_create(card);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, LV_PCT(100), 60);
    lv_obj_set_style_bg_color(row, hex(t.bg), 0);
    lv_obj_set_style_bg_opa(row, LV_OPA_40, 0);
    lv_obj_set_style_radius(row, 10, 0);
    lv_obj_set_style_pad_all(row, 8, 0);

    lv_obj_t *nm = lv_label_create(row);
    lv_label_set_text(nm, STRETCHES[order[i]].name);
    lv_obj_set_style_text_color(nm, hex(t.text), 0);
    lv_obj_set_style_text_font(nm, &lv_font_montserrat_16, 0);
    lv_obj_align(nm, LV_ALIGN_LEFT_MID, 0, -8);

    lv_obj_t *sp = lv_label_create(row);
    char sb[48]; snprintf(sb, sizeof(sb), "%d reps  -  %d s hold", reps, hold);
    lv_label_set_text(sp, sb);
    lv_obj_set_style_text_color(sp, hex(t.accent), 0);
    lv_obj_set_style_text_font(sp, &lv_font_montserrat_14, 0);
    lv_obj_align(sp, LV_ALIGN_LEFT_MID, 0, 12);
  }

  lv_obj_t *btn = lv_btn_create(s);
  lv_obj_add_style(btn, &st_pill, 0);
  lv_obj_align(btn, LV_ALIGN_BOTTOM_MID, 0, -20);
  lv_obj_t *bl = lv_label_create(btn);
  lv_label_set_text(bl, "SHUFFLE");
  lv_obj_center(bl);
  lv_obj_add_event_cb(btn, on_workout_reroll, LV_EVENT_CLICKED, nullptr);

  lv_scr_load(s);
}
static void on_workout_reroll(lv_event_t*) { ui_workout(); }

// =============================================================================
// UI - MOVES LIST
// =============================================================================
static void on_move_row(lv_event_t *e) {
  g_move_idx = (int)(intptr_t)lv_event_get_user_data(e);
  g_sub = SUB_MOVE_DETAIL;
  ui_move_detail();
}

static void ui_moves_list() {
  const BeltTheme &t = BELTS[g_belt];
  lv_obj_t *s = make_screen();
  add_status_bar(s, "MOVES");

  lv_obj_t *list = lv_obj_create(s);
  lv_obj_remove_style_all(list);
  lv_obj_set_size(list, LCD_WIDTH - 40, LCD_HEIGHT - 90);
  lv_obj_align(list, LV_ALIGN_TOP_MID, 0, 60);
  lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_row(list, 8, 0);
  lv_obj_set_style_pad_all(list, 4, 0);

  const char *cur_cat = "";
  for (int i = 0; i < MOVE_COUNT; i++) {
    if (strcmp(MOVES[i].category, cur_cat) != 0) {
      cur_cat = MOVES[i].category;
      lv_obj_t *h = lv_label_create(list);
      lv_label_set_text(h, cur_cat);
      lv_obj_set_style_text_color(h, hex(t.primary), 0);
      lv_obj_set_style_text_font(h, &lv_font_montserrat_16, 0);
    }
    lv_obj_t *row = lv_obj_create(list);
    lv_obj_add_style(row, &st_card, 0);
    lv_obj_set_size(row, LV_PCT(100), 58);
    lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(row, on_move_row, LV_EVENT_CLICKED, (void*)(intptr_t)i);

    lv_obj_t *k = lv_label_create(row);
    lv_label_set_text(k, MOVES[i].kor);
    lv_obj_set_style_text_color(k, hex(t.accent), 0);
    lv_obj_set_style_text_font(k, &lv_font_montserrat_16, 0);
    lv_obj_align(k, LV_ALIGN_TOP_LEFT, 0, 0);

    lv_obj_t *e = lv_label_create(row);
    lv_label_set_text(e, MOVES[i].eng);
    lv_obj_set_style_text_color(e, hex(t.muted), 0);
    lv_obj_set_style_text_font(e, &lv_font_montserrat_14, 0);
    lv_obj_align(e, LV_ALIGN_BOTTOM_LEFT, 0, -2);
  }

  lv_scr_load(s);
}

static void ui_move_detail() {
  const BeltTheme &t = BELTS[g_belt];
  const Move &m = MOVES[g_move_idx];
  lv_obj_t *s = make_screen();
  add_status_bar(s, m.category);

  lv_obj_t *kor = lv_label_create(s);
  lv_label_set_text(kor, m.kor);
  lv_obj_set_style_text_font(kor, &lv_font_montserrat_28, 0);
  lv_obj_set_style_text_color(kor, hex(t.primary), 0);
  lv_obj_align(kor, LV_ALIGN_TOP_MID, 0, 70);

  lv_obj_t *eng = lv_label_create(s);
  lv_label_set_text(eng, m.eng);
  lv_obj_set_style_text_font(eng, &lv_font_montserrat_20, 0);
  lv_obj_set_style_text_color(eng, hex(t.accent), 0);
  lv_obj_align(eng, LV_ALIGN_TOP_MID, 0, 112);

  lv_obj_t *card = lv_obj_create(s);
  lv_obj_add_style(card, &st_card, 0);
  lv_obj_set_size(card, LCD_WIDTH - 40, 260);
  lv_obj_align(card, LV_ALIGN_TOP_MID, 0, 160);
  lv_obj_t *dsc = lv_label_create(card);
  lv_label_set_text(dsc, m.desc);
  lv_label_set_long_mode(dsc, LV_LABEL_LONG_WRAP);
  lv_obj_set_width(dsc, LCD_WIDTH - 80);
  lv_obj_set_style_text_color(dsc, hex(t.text), 0);
  lv_obj_set_style_text_font(dsc, &lv_font_montserrat_16, 0);

  lv_scr_load(s);
}

// =============================================================================
// UI - SETTINGS  (opens Theme picker or Wallpaper picker via swipe-up cycle)
// =============================================================================
static void on_open_theme(lv_event_t*)     { g_sub = SUB_THEME_PICKER;     ui_theme_picker(); }
static void on_open_wallpaper(lv_event_t*) { g_sub = SUB_WALLPAPER_PICKER; ui_wallpaper_picker(); }
static void on_back_to_launcher_click(lv_event_t*);

// --- Time-sync settings row (LauncherOS-parity manual trigger) ---------------
// Text for the "TIME SYNC" row's right-hand value. Filled in by
// settings_time_status_str() from the current NTP / NVS state.
static void settings_time_status_str(char *out, size_t n) {
  if (g_ntp_synced) {
    struct tm tm;
    if (getLocalTime(&tm, 5)) {
      snprintf(out, n, "%02d:%02d", tm.tm_hour, tm.tm_min);
      return;
    }
  }
  if (g_wifi_connected) { snprintf(out, n, "Syncing..."); return; }
  snprintf(out, n, "Tap to sync");
}

static void on_sync_time_click(lv_event_t *e) {
  if (g_switching) return;

  // Toast so the user gets feedback while we block on WiFi.
  lv_obj_t *toast = lv_label_create(lv_scr_act());
  lv_label_set_text(toast, "Syncing time via WiFi...");
  lv_obj_set_style_text_color(toast, lv_color_hex(0xFFFFFF), 0);
  lv_obj_set_style_text_font(toast, &lv_font_montserrat_16, 0);
  lv_obj_set_style_bg_color(toast, lv_color_hex(0x000000), 0);
  lv_obj_set_style_bg_opa(toast, LV_OPA_80, 0);
  lv_obj_set_style_pad_all(toast, 12, 0);
  lv_obj_set_style_radius(toast, 10, 0);
  lv_obj_center(toast);
  lv_obj_move_foreground(toast);
  uint32_t t0 = millis();
  while (millis() - t0 < 250) { lv_timer_handler(); delay(10); }

  // Force a resync even if the 12h interval isn't up yet.
  // wifi_connect_and_sync() can block for up to ~18s (2 networks + NTP), so
  // detach from the 8s task WDT for the duration, then re-attach.
  esp_task_wdt_delete(NULL);

  g_ntp_synced    = false;
  g_wifi_connected = false;
  parse_wifi_config();          // re-read /WATCH/wifi/config.txt if present
  wifi_connect_and_sync();      // connects, runs NTP, persists to NVS, drops WiFi

  esp_task_wdt_add(NULL);

  // Refresh the settings screen so the "TIME SYNC" value updates.
  g_last_touch_ms = millis();   // don't let the sync count as idle time
  ui_settings();
}
// -----------------------------------------------------------------------------

static void ui_settings() {
  const BeltTheme &t = BELTS[g_belt];
  lv_obj_t *s = make_screen();
  add_status_bar(s, "SETTINGS");

  auto make_row = [&](const char *label, const char *value, lv_event_cb_t cb, int y){
    lv_obj_t *row = lv_obj_create(s);
    lv_obj_add_style(row, &st_card, 0);
    lv_obj_set_size(row, LCD_WIDTH - 40, 60);
    lv_obj_align(row, LV_ALIGN_TOP_MID, 0, y);
    lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(row, cb, LV_EVENT_CLICKED, nullptr);
    lv_obj_t *l = lv_label_create(row);
    lv_label_set_text(l, label);
    lv_obj_set_style_text_font(l, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(l, hex(t.text), 0);
    lv_obj_align(l, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_t *v = lv_label_create(row);
    lv_label_set_text(v, value);
    lv_obj_set_style_text_font(v, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(v, hex(t.primary), 0);
    lv_obj_align(v, LV_ALIGN_RIGHT_MID, 0, 0);
  };

  char time_val[24];
  settings_time_status_str(time_val, sizeof(time_val));

  make_row("BELT THEME",  BELTS[g_belt].name, on_open_theme,     70);
  make_row("WALLPAPER",   "Pick",             on_open_wallpaper, 140);
  make_row("TIME SYNC",   time_val,           on_sync_time_click, 210);

  lv_obj_t *btn = lv_btn_create(s);
  lv_obj_add_style(btn, &st_pill, 0);
  lv_obj_align(btn, LV_ALIGN_BOTTOM_MID, 0, -30);
  lv_obj_t *bl = lv_label_create(btn);
  lv_label_set_text(bl, "BACK TO LAUNCHER");
  lv_obj_center(bl);
  lv_obj_add_event_cb(btn, on_back_to_launcher_click, LV_EVENT_CLICKED, nullptr);

  lv_obj_t *hint = lv_label_create(s);
  lv_label_set_text(hint, "hold BOOT 1.5s  ->  launcher");
  lv_obj_set_style_text_color(hint, hex(t.muted), 0);
  lv_obj_set_style_text_font(hint, &lv_font_montserrat_14, 0);
  lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -80);

  lv_scr_load(s);
}

// =============================================================================
// TRANSFORMATION SEQUENCE  (Omnitrix / BoBoiBoy-style power-up)
// Full-screen belt-colored shockwave, hangul flash, then load new theme.
// =============================================================================
static void ui_transformation(int from_int, int to_int) {
  Belt from = (Belt)from_int; (void)from;
  Belt to   = (Belt)to_int;
  const BeltTheme &t = BELTS[to];
  lv_obj_t *s = lv_obj_create(nullptr);
  lv_obj_remove_style_all(s);
  lv_obj_set_size(s, LCD_WIDTH, LCD_HEIGHT);
  lv_obj_set_style_bg_color(s, hex(0x000000), 0);
  lv_obj_set_style_bg_opa(s, LV_OPA_COVER, 0);

  // Outer expanding ring (shockwave)
  lv_obj_t *ring = lv_obj_create(s);
  lv_obj_remove_style_all(ring);
  lv_obj_set_size(ring, 20, 20);
  lv_obj_center(ring);
  lv_obj_set_style_radius(ring, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_bg_opa(ring, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_color(ring, hex(t.primary), 0);
  lv_obj_set_style_border_width(ring, 6, 0);

  // Inner solid pulse
  lv_obj_t *core = lv_obj_create(s);
  lv_obj_remove_style_all(core);
  lv_obj_set_size(core, 10, 10);
  lv_obj_center(core);
  lv_obj_set_style_radius(core, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_bg_color(core, hex(t.primary), 0);
  lv_obj_set_style_bg_opa(core, LV_OPA_COVER, 0);

  // Hangul (fades in mid-animation)
  lv_obj_t *k = lv_label_create(s);
  lv_label_set_text(k, t.hangul);
  lv_obj_set_style_text_font(k, belt_title_font(), 0);
  lv_obj_set_style_text_color(k, hex(t.primary), 0);
  lv_obj_center(k);
  lv_obj_set_style_opa(k, LV_OPA_TRANSP, 0);

  lv_scr_load(s);

  // Phase 1: ring expands to fill screen (~700 ms)
  uint32_t t0 = millis();
  const uint32_t P1 = 700;
  while (millis() - t0 < P1) {
    float p = (float)(millis() - t0) / (float)P1;
    int32_t sz = 20 + (int32_t)(p * 640);
    lv_obj_set_size(ring, sz, sz);
    lv_obj_center(ring);
    // core pulses
    int32_t csz = 10 + (int32_t)(sinf(p * 6.28f * 3) * 20 + 20);
    lv_obj_set_size(core, csz, csz);
    lv_obj_center(core);
    lv_timer_handler();
    delay(8);
  }

  // Phase 2: whole screen flashes belt color, hangul fades in (~350 ms)
  lv_obj_set_style_bg_color(s, hex(t.primary), 0);
  lv_obj_add_flag(ring, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_flag(core, LV_OBJ_FLAG_HIDDEN);
  lv_obj_set_style_text_color(k, hex(t.dark ? 0x000000 : 0xFFFFFF), 0);
  t0 = millis();
  const uint32_t P2 = 350;
  while (millis() - t0 < P2) {
    float p = (float)(millis() - t0) / (float)P2;
    lv_obj_set_style_opa(k, (lv_opa_t)(p * 255), 0);
    lv_timer_handler(); delay(8);
  }

  // Phase 3: fade to black hold (~400 ms) with the belt name + tagline
  lv_obj_set_style_bg_color(s, hex(0x000000), 0);
  lv_obj_set_style_text_color(k, hex(t.primary), 0);
  lv_obj_t *tag = lv_label_create(s);
  lv_label_set_text(tag, t.tagline);
  lv_obj_set_style_text_font(tag, &lv_font_montserrat_16, 0);
  lv_obj_set_style_text_color(tag, hex(t.accent), 0);
  lv_obj_align(tag, LV_ALIGN_CENTER, 0, 60);

  lv_obj_t *nm = lv_label_create(s);
  char nb[24]; snprintf(nb, sizeof(nb), "%s BELT", t.name);
  lv_label_set_text(nm, nb);
  lv_obj_set_style_text_font(nm, belt_title_font(), 0);
  lv_obj_set_style_text_color(nm, hex(t.primary), 0);
  lv_obj_align(nm, LV_ALIGN_CENTER, 0, -70);

  t0 = millis();
  while (millis() - t0 < 900) { lv_timer_handler(); delay(10); }
}

// =============================================================================
// UI - THEME PICKER  (Omnitrix-style circular dial)
// Belts arranged in a ring, selector at the top, tap belt to transform.
// =============================================================================
static void on_pick_belt(lv_event_t *e) {
  int b = (int)(intptr_t)lv_event_get_user_data(e);
  if (b == g_belt) return;
  Belt from = g_belt;
  g_belt = (Belt)b;
  NVS.begin("tkdos", false);
  NVS.putUChar("belt", g_belt);
  NVS.end();
  theme_apply();
  g_stretch_loaded_idx = -1;
  g_wp_loaded_for = BELT_COUNT;   // force wallpaper reload for new belt
  ui_transformation(from, g_belt);
  g_sub = SUB_NONE;
  g_app = APP_WATCHFACE;
  ui_watchface();
}

static void ui_theme_picker() {
  const BeltTheme &t = BELTS[g_belt];
  lv_obj_t *s = make_screen();
  add_status_bar(s, "TRANSFORM");

  // Center hangul + name for current belt
  lv_obj_t *core = lv_obj_create(s);
  lv_obj_remove_style_all(core);
  lv_obj_set_size(core, 150, 150);
  lv_obj_align(core, LV_ALIGN_CENTER, 0, 10);
  lv_obj_set_style_radius(core, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_bg_color(core, hex(t.primary), 0);
  lv_obj_set_style_bg_opa(core, LV_OPA_COVER, 0);
  lv_obj_set_style_border_color(core, hex(t.accent), 0);
  lv_obj_set_style_border_width(core, 3, 0);

  lv_obj_t *khan = lv_label_create(core);
  lv_label_set_text(khan, t.hangul);
  lv_obj_set_style_text_font(khan, belt_title_font(), 0);
  lv_obj_set_style_text_color(khan, hex(t.dark ? 0x000000 : 0xFFFFFF), 0);
  lv_obj_center(khan);

  // 6 belt orbs arranged in a ring around the core
  const int R = 175;
  const int CX = LCD_WIDTH / 2;
  const int CY = 60 + (LCD_HEIGHT - 60) / 2 + 10;
  for (int i = 0; i < BELT_COUNT; i++) {
    float ang = (-90.0f + i * (360.0f / BELT_COUNT)) * 3.14159265f / 180.0f;
    int x = CX + (int)(cosf(ang) * R);
    int y = CY + (int)(sinf(ang) * R);
    int sz = (i == g_belt) ? 56 : 46;

    lv_obj_t *orb = lv_obj_create(s);
    lv_obj_remove_style_all(orb);
    lv_obj_set_size(orb, sz, sz);
    lv_obj_set_pos(orb, x - sz / 2, y - sz / 2);
    lv_obj_set_style_radius(orb, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(orb, hex(BELTS[i].primary), 0);
    lv_obj_set_style_bg_opa(orb, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(orb, hex(BELTS[i].accent), 0);
    lv_obj_set_style_border_width(orb, i == g_belt ? 4 : 2, 0);
    lv_obj_add_flag(orb, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(orb, on_pick_belt, LV_EVENT_CLICKED, (void*)(intptr_t)i);

    lv_obj_t *lbl = lv_label_create(orb);
    char c[2] = { BELTS[i].name[0], 0 };
    lv_label_set_text(lbl, c);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(lbl, hex(BELTS[i].dark ? 0x000000 : 0xFFFFFF), 0);
    lv_obj_center(lbl);
  }

  lv_obj_t *hint = lv_label_create(s);
  lv_label_set_text(hint, "TAP AN ORB TO TRANSFORM");
  lv_obj_set_style_text_color(hint, hex(t.muted), 0);
  lv_obj_set_style_text_font(hint, &lv_font_montserrat_14, 0);
  lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -18);

  lv_scr_load(s);
}

// =============================================================================
// UI - WALLPAPER PICKER  (belt-locked - only shows current belt's folder)
// =============================================================================
#define MAX_WP 24
static String g_wp_paths[MAX_WP];
static int    g_wp_count = 0;

static void wp_scan() {
  g_wp_count = 0;
  if (!g_sd_ok) return;
  char dir[96];
  snprintf(dir, sizeof(dir), "/WATCH/TKDOS/Theme/%s", BELTS[g_belt].dir);
  File d = SD_MMC.open(dir);
  if (!d || !d.isDirectory()) return;
  File f = d.openNextFile();
  while (f && g_wp_count < MAX_WP) {
    if (!f.isDirectory()) {
      String n = f.name();
      if (n.endsWith(".raw") || n.endsWith(".RAW")) {
        g_wp_paths[g_wp_count++] = String(dir) + "/" + n.substring(n.lastIndexOf('/') + 1);
      }
    }
    f = d.openNextFile();
  }
  d.close();
}

static void on_pick_wallpaper(lv_event_t *e) {
  int idx = (int)(intptr_t)lv_event_get_user_data(e);
  if (idx < 0 || idx >= g_wp_count) return;
  NVS.begin("tkdos", false);
  char key[16]; snprintf(key, sizeof(key), "wp_%d", g_belt);
  NVS.putString(key, g_wp_paths[idx]);
  NVS.end();
  g_wp_loaded_for = BELT_COUNT;   // force wallpaper reload next watchface paint
  ui_wallpaper_picker();  // refresh with checkmark
}

static void ui_wallpaper_picker() {
  const BeltTheme &t = BELTS[g_belt];
  wp_scan();
  lv_obj_t *s = make_screen();
  add_status_bar(s, "WALLPAPER");

  if (g_wp_count == 0) {
    lv_obj_t *l = lv_label_create(s);
    char buf[160];
    snprintf(buf, sizeof(buf), "No wallpapers found.\n\nDrop .raw files (320x320 RGB565) into\n/WATCH/TKDOS/Theme/%s", BELTS[g_belt].dir);
    lv_label_set_text(l, buf);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(l, hex(t.muted), 0);
    lv_obj_center(l);
    lv_scr_load(s);
    return;
  }

  lv_obj_t *list = lv_obj_create(s);
  lv_obj_remove_style_all(list);
  lv_obj_set_size(list, LCD_WIDTH - 40, LCD_HEIGHT - 90);
  lv_obj_align(list, LV_ALIGN_TOP_MID, 0, 60);
  lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_row(list, 8, 0);

  for (int i = 0; i < g_wp_count; i++) {
    lv_obj_t *row = lv_obj_create(list);
    lv_obj_add_style(row, &st_card, 0);
    lv_obj_set_size(row, LV_PCT(100), 50);
    lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(row, on_pick_wallpaper, LV_EVENT_CLICKED, (void*)(intptr_t)i);
    lv_obj_t *n = lv_label_create(row);
    String base = g_wp_paths[i];
    int slash = base.lastIndexOf('/');
    lv_label_set_text(n, base.substring(slash + 1).c_str());
    lv_obj_set_style_text_color(n, hex(t.text), 0);
    lv_obj_align(n, LV_ALIGN_LEFT_MID, 0, 0);
  }

  lv_obj_t *lock = lv_label_create(s);
  char lb[80];
  snprintf(lb, sizeof(lb), "%s belt - locked to this belt's set", BELTS[g_belt].name);
  lv_label_set_text(lock, lb);
  lv_obj_set_style_text_color(lock, hex(t.muted), 0);
  lv_obj_set_style_text_font(lock, &lv_font_montserrat_14, 0);
  lv_obj_align(lock, LV_ALIGN_BOTTOM_MID, 0, -14);

  lv_scr_load(s);
}

// =============================================================================
// PARTITION SWITCH - clean hand-off: LVGL flush -> PMU IRQ off -> Wire.end
//                    -> esp_ota_set_boot_partition(factory) -> esp_restart
// =============================================================================
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

static bool partition_switch_to_launcher() {
  const esp_partition_t *fact = esp_partition_find_first(
      ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_FACTORY, nullptr);
  if (!fact) { USBSerial.println("[part] no factory partition"); return false; }
  esp_err_t e = esp_ota_set_boot_partition(fact);
  if (e != ESP_OK) { USBSerial.printf("[part] set_boot err=%d\n", (int)e); return false; }
  USBSerial.println("[part] boot -> factory, restarting");
  delay(80);
  esp_restart();
  return true;   // unreachable
}

// Anchor bootloader on the currently-running partition. Safe no-op when we
// ARE the factory image (running == factory -> writes same value it read).
static void anchor_launcher_as_boot() {
  const esp_partition_t *self = esp_ota_get_running_partition();
  if (!self) { USBSerial.println("[boot] no running partition?"); return; }
  esp_err_t e = esp_ota_set_boot_partition(self);
  USBSerial.printf("[boot] anchored to %s (err=%d)\n", self->label, (int)e);
}

static void ui_back_to_launcher_action() {
  time_publish("exit", false);   // hand the clock back to the launcher
  if (g_switching) return;                 // re-entry guard
  g_switching = true;
  USBSerial.println("[back] hand-off starting");

  // 1) Toast so the user sees SOMETHING before the panel goes dark.
  lv_obj_t *toast = lv_label_create(lv_scr_act());
  lv_label_set_text(toast, "Returning to launcher...");
  lv_obj_set_style_text_color(toast, lv_color_hex(0xFFFFFF), 0);
  lv_obj_set_style_text_font(toast, &lv_font_montserrat_16, 0);
  lv_obj_set_style_bg_color(toast, lv_color_hex(0x000000), 0);
  lv_obj_set_style_bg_opa(toast, LV_OPA_80, 0);
  lv_obj_set_style_pad_all(toast, 12, 0);
  lv_obj_set_style_radius(toast, 10, 0);
  lv_obj_center(toast);
  lv_obj_move_foreground(toast);

  // 2) Flush LVGL so the toast is actually on the panel.
  uint32_t t0 = millis();
  while (millis() - t0 < 350) { lv_timer_handler(); delay(10); }

  // 3) Shut down anything that touches I2C so we can't crash mid-flip.
  if (g_pmu_ok) {
    pmu.disableIRQ(XPOWERS_AXP2101_ALL_IRQ);
    pmu.clearIrqStatus();
  }
#ifdef PMU_IRQ_PIN
  detachInterrupt(digitalPinToInterrupt(PMU_IRQ_PIN));
#endif
  FT3168->IIC_Interrupt_Flag = false;
  detachInterrupt(digitalPinToInterrupt(TP_INT));
  Wire.end();

  // 4) Fade the panel down so the reset doesn't flash.
  static_cast<Arduino_CO5300 *>(gfx)->setBrightness(0);

  // 5) Flip the boot partition and reboot.
  USBSerial.println("[back] partition switch");
  if (partition_switch_to_launcher()) return;   // esp_restart above

  // 6) Fallbacks (matches original TKDOS behaviour).
  USBSerial.println("[back] fallback SD flash");
  if (sd_flash_launcher_bin()) return;

  USBSerial.println("[back] hard reboot");
  esp_restart();
}
static void on_back_to_launcher_click(lv_event_t*) { ui_back_to_launcher_action(); }

// =============================================================================
// NAVIGATION  (loop + swipe handling)
// =============================================================================
static void ui_load_current() {
  if (g_sub == SUB_NONE) {
    switch (g_app) {
      case APP_WATCHFACE:  ui_watchface();      break;
      case APP_STRETCHES:  ui_stretches_list(); break;
      case APP_MOVES:      ui_moves_list();     break;
      case APP_SETTINGS:   ui_settings();       break;
      default: break;
    }
  } else {
    switch (g_sub) {
      case SUB_STRETCH_DETAIL:    ui_stretch_detail();    break;
      case SUB_WORKOUT:           ui_workout();           break;
      case SUB_MOVE_DETAIL:       ui_move_detail();       break;
      case SUB_THEME_PICKER:      ui_theme_picker();      break;
      case SUB_WALLPAPER_PICKER:  ui_wallpaper_picker();  break;
      default: break;
    }
  }
}

// NOTE: handle_swipe takes a plain `int` (not SwipeEv) so that Arduino IDE's
// auto-prototype generator - which places the prototype ABOVE the enum
// definition - compiles cleanly. We cast at the call site.
static void handle_swipe(int ev) {
  if (g_switching) return;
  if (millis() - g_lastSwipeMs < 250) return;
  g_lastSwipeMs = millis();

  if (g_sub != SUB_NONE) {
    // Sub-app: swipe DOWN returns to parent (UNCHANGED behaviour).
    if (ev == SW_DOWN) { g_sub = SUB_NONE; ui_load_current(); return; }
    if (g_sub == SUB_STRETCH_DETAIL) {
      if (ev == SW_LEFT)  { g_stretch_idx = (g_stretch_idx + 1) % STRETCH_COUNT; ui_stretch_detail(); return; }
      if (ev == SW_RIGHT) { g_stretch_idx = (g_stretch_idx - 1 + STRETCH_COUNT) % STRETCH_COUNT; ui_stretch_detail(); return; }
    }
    if (g_sub == SUB_MOVE_DETAIL) {
      if (ev == SW_LEFT)  { g_move_idx = (g_move_idx + 1) % MOVE_COUNT; ui_move_detail(); return; }
      if (ev == SW_RIGHT) { g_move_idx = (g_move_idx - 1 + MOVE_COUNT) % MOVE_COUNT; ui_move_detail(); return; }
    }
    return;
  }

  // Top-level carousel
  if (ev == SW_LEFT)  { g_app = (App)((g_app + 1) % APP_COUNT); ui_load_current(); return; }
  if (ev == SW_RIGHT) { g_app = (App)((g_app + APP_COUNT - 1) % APP_COUNT); ui_load_current(); return; }

  // *** ROOT-ONLY swipe-down = back-to-launcher (spec: WATCHFACE + SUB_NONE only) ***
  if (ev == SW_DOWN && g_app == APP_WATCHFACE && g_sub == SUB_NONE) {
    USBSerial.println("[nav] root swipe-down -> launcher");
    ui_back_to_launcher_action();
    return;
  }

  if (ev == SW_UP) {
    switch (g_app) {
      case APP_STRETCHES: g_sub = SUB_WORKOUT;      ui_workout();      return;
      case APP_SETTINGS:  g_sub = SUB_THEME_PICKER; ui_theme_picker(); return;
      case APP_WATCHFACE: return;
      case APP_MOVES:     return;
      default:            return;
    }
  }
}

// =============================================================================
// BOOT-BUTTON SAFETY  (hold GPIO0 1.5 s -> launcher)
// =============================================================================
static uint32_t bootPressStartMs = 0;
static void boot_safety_tick() {
  if (g_switching) return;
  if (digitalRead(0) == LOW) {
    if (bootPressStartMs == 0) bootPressStartMs = millis();
    if (millis() - bootPressStartMs > 1500) {
      USBSerial.println("[boot] long-press -> launcher");
      ui_back_to_launcher_action();
    }
  } else {
    bootPressStartMs = 0;
  }
}

// -----------------------------------------------------------------------------
// Touch reinit helper (called from loop() when g_touch_ok is false)
// -----------------------------------------------------------------------------
static uint32_t g_touch_last_reinit_ms = 0;
static void touch_try_reinit() {
  if (millis() - g_touch_last_reinit_ms < 5000) return;
  g_touch_last_reinit_ms = millis();

  i2c_bus_recover();
  if (!i2c_ping(FT3168_DEVICE_ADDRESS)) {
    g_touch_ok = false;
    i2c_log_state(false, "FT3168");
    return;
  }
  digitalWrite(TP_RESET, LOW);  delay(30);
  digitalWrite(TP_RESET, HIGH); delay(150);
  g_touch_ok = FT3168->begin();
  if (g_touch_ok) {
    FT3168->IIC_Write_Device_State(
        FT3168->Arduino_IIC_Touch::Device::TOUCH_POWER_MODE,
        FT3168->Arduino_IIC_Touch::Device_Mode::TOUCH_POWER_MONITOR);
  }
  i2c_log_state(g_touch_ok, "FT3168");
}

// =============================================================================
// setup() + loop()
// =============================================================================
void setup() {
  USBSerial.begin(115200);
  USBSerial.println("TKD OS v1.0 (LVGL 8.4)");

  // LauncherOS-style fast-wake detect: if the wake cause is anything other
  // than UNDEFINED, the chip resumed from deep sleep (e.g. EXT1 button/touch)
  // rather than a cold boot / reset. On such a wake we skip the boot splash
  // and jump straight to the watchface. NOTE: TKDOS itself does not enter
  // deep sleep today (screen-off just fades the panel and keeps the loop
  // running, RAM preserved), so this only fires if a deep-sleep resume ever
  // happens -- it is harmless on a normal cold boot.
  bool from_deep_sleep =
      (esp_sleep_get_wakeup_cause() != ESP_SLEEP_WAKEUP_UNDEFINED);
  USBSerial.printf("[boot] %s\n",
                   from_deep_sleep ? "wake from deep sleep" : "cold boot");

  pinMode(0, INPUT_PULLUP);

  if (!gfx->begin()) USBSerial.println("gfx->begin() failed!");
  gfx->fillScreen(RGB565_BLACK);
  // LauncherOS-parity: start the panel completely dark BEFORE anything
  // else so the whole boot happens invisibly, then fade up once the
  // watchface has rendered its first frame. Prevents the "flash of
  // splash + then black" and the CO5300 half-woken-sleep-state artifact.
  static_cast<Arduino_CO5300 *>(gfx)->setBrightness(0);
  g_brightness = 0;

  // I2C bus recovery + fast mode
  i2c_bus_recover();   // was: inline 9-toggle + Wire.begin

  // Touch reset then init
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
  pmu_poll();

  // Touch health flag mirrors g_pmu_ok
  g_touch_ok = (FT3168 != nullptr);   // begin() already ran above; if it timed out we'll reinit lazily
  i2c_log_state(g_touch_ok, "FT3168");

  // Anchor bootloader on whatever we're running from now.
  // If TKDOS is flashed to factory, this is a no-op. If it's in ota_0, this
  // makes TKDOS survive a spontaneous reboot (won't accidentally jump to
  // ota_1 launcher). LauncherOS parity.
  anchor_launcher_as_boot();

  lv_init();
  screenWidth  = gfx->width();
  screenHeight = gfx->height();
  bufSize = screenWidth * LVGL_BUF_LINES;
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

  // Allocate stretch image buffer in PSRAM (falls back to any 8-bit).
  size_t need = (size_t)STRETCH_IMG_W * STRETCH_IMG_H * 2;
  g_stretch_buf = (lv_color_t *)heap_caps_malloc(need, MALLOC_CAP_SPIRAM);
  if (!g_stretch_buf) g_stretch_buf = (lv_color_t *)heap_caps_malloc(need, MALLOC_CAP_8BIT);
  if (!g_stretch_buf) USBSerial.println("[stretch] no room for image buffer");
// Wallpaper buffer — 410 * 502 * 2 = 411,640 bytes, prefer PSRAM
{
  size_t wpn = (size_t)WP_W * WP_H * 2;
  g_wp_buf = (uint8_t *)heap_caps_malloc(wpn, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!g_wp_buf) g_wp_buf = (uint8_t *)heap_caps_malloc(wpn, MALLOC_CAP_8BIT);
  USBSerial.printf("[wp] buf=%p size=%u  freePSRAM=%u  largestPSRAM=%u\n",
                   g_wp_buf, (unsigned)wpn,
                   (unsigned)ESP.getFreePsram(),
                   (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM));
  if (!g_wp_buf) USBSerial.println("[wp] no room for wallpaper buffer");
}
  // SD_MMC bring-up
  SD_MMC.setPins(SDMMC_CLK, SDMMC_CMD, SDMMC_DATA);
  g_sd_ok = SD_MMC.begin("/sdcard", true);
  if (!g_sd_ok) USBSerial.println("SD_MMC init failed - continuing without SD");
  else sd_bootstrap_folders();

  // Load saved belt (defaults to WHITE on first boot per spec)
  NVS.begin("tkdos", true);
  uint8_t b = NVS.getUChar("belt", BELT_WHITE);
  NVS.end();
  if (b >= BELT_COUNT) b = BELT_WHITE;
  g_belt = (Belt)b;

  // Bring up the wall clock (from NVS + WiFi/NTP in the background)
  time_restore_from_nvs();
  g_gmt_offset_sec = g_tp_gmt;    // mirror the shared clock into TKDOS
  g_ntp_synced     = g_tp_valid;
  parse_wifi_config();
  wifi_connect_and_sync();

  theme_apply();

  // LauncherOS-style: show the boot splash only on a cold boot. On a
  // deep-sleep wake, skip it so the watchface appears almost instantly.
  if (!from_deep_sleep) {
    ui_boot_splash();
  } else {
    USBSerial.println("[boot] fast wake -> skipping splash");
  }

  g_app = APP_WATCHFACE;
  g_sub = SUB_NONE;
  ui_load_current();

  // Panel was dropped to 0 at the top of setup(). Render the first
  // watchface frame while dark, then smoothly fade the AMOLED up to
  // DEFAULT_BRIGHTNESS -- LauncherOS pattern (setup line ~2615).
  lv_timer_handler();
  fade_brightness(DEFAULT_BRIGHTNESS, 260);
  g_last_touch_ms = g_last_wake_ms = millis();

  // Task watchdog on the Arduino loop core (Core 1). 8s timeout.
  // IDF 5.x (Arduino-ESP32 3.x) changed the signature to take a struct;
  // older cores still accept (timeout_s, panic). Handle both.
#if defined(ESP_IDF_VERSION_MAJOR) && ESP_IDF_VERSION_MAJOR >= 5
  esp_task_wdt_config_t twdt_cfg = {
    .timeout_ms     = 8000,
    .idle_core_mask = 0,
    .trigger_panic  = true,
  };
  // If the TWDT is already initialised by the Arduino core, reconfigure it.
  if (esp_task_wdt_init(&twdt_cfg) == ESP_ERR_INVALID_STATE) {
    esp_task_wdt_reconfigure(&twdt_cfg);
  }
#else
  esp_task_wdt_init(8, true);      // 8s, panic on trip (IDF 4.x)
#endif
  // Only add the current task if it isn't already being watched.
  if (esp_task_wdt_status(NULL) == ESP_ERR_NOT_FOUND) {
    esp_task_wdt_add(NULL);        // watch the loopTask
  }
  USBSerial.println("[wdt] enabled (8s)");

  USBSerial.println("Setup done");
}

void loop() {
  static uint32_t lastTick = 0;
  static uint32_t lvLastMs = 0;

#if LV_TICK_CUSTOM == 0
  uint32_t nowMs = millis();
  lv_tick_inc(nowMs - lvLastMs);
  lvLastMs = nowMs;
#else
  (void)lvLastMs;
#endif

  esp_task_wdt_reset();          // <-- feed the dog every iteration
  lv_timer_handler();

  if (g_pending_swipe != SW_NONE) {
    int ev = (int)g_pending_swipe; g_pending_swipe = SW_NONE;
    if (g_screen_off) { screen_on_now(); }
    else              { handle_swipe(ev); }
  }

  uint32_t now = millis();
  if (now - lastTick > 1000) {
    lastTick = now;
    pmu_poll();
    if (!g_touch_ok) touch_try_reinit();
    wifi_resync_if_due();
    if (g_app == APP_WATCHFACE && g_sub == SUB_NONE) watchface_tick();
  }

  // Also poll PMU IRQ hot-path more often than 1Hz
  if (g_pmu_irq_pending) pmu_poll();

  if (!g_screen_off && (now - g_last_touch_ms > SCREEN_OFF_TIMEOUT_MS)
      && (now - g_last_wake_ms > WAKE_GRACE_MS)) {
    USBSerial.println("[timeout] screen off");
    screen_off_now();
  }

  boot_safety_tick();
  delay(5);
}
