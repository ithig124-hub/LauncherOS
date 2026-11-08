// ============================================================================
//  Mentalist.ino -- "The Mentalist" personal intelligence OS.
//  Target: DualCore ESP32-S3 watch (Waveshare ESP32-S3-Touch-AMOLED-2.06,
//  CO5300 AMOLED 410x502, FT3168 touch, AXP2101 PMU, SD_MMC).
//
//  Single-file app.  Launched from LauncherOS as an SD card .bin, it keeps the
//  same contracts as the other V2 apps:
//
//   * BOOT PATTERN  -- an app never owns the boot partition.  LauncherOS anchors
//                      `factory` on every launcher boot (anchor_launcher_as_boot)
//                      and streams this .bin into ota_0 (load_app_from_sd).
//                      We never set ourselves as the boot partition.
//   * BACK TO LAUNCHER -- hold BOOT (GPIO0) 1.5 s, or press LAUNCHEROS on the
//                      System screen.  Both run return_to_launcher(): save,
//                      cover overlay, esp_ota_set_boot_partition(factory),
//                      fade, esp_restart().  Same protocol as V2/ExplorerOS.
//   * SCREEN TIMEOUT -- LauncherOS policy: 15 s without touch -> panel dark,
//                      2 s wake grace, TP_INT wakes, 12-step CO5300 brightness
//                      fade (never DISPOFF/DISPON).
//   * CLOCK -- the shared "rtcsave" NVS contract (TIME_CONTRACT.md): restore on
//                      boot, publish every 5 min, and on exit.
//
//  Storage lives on the SD card, never on the app partition:
//
//   /WATCH/Mentalist/
//     CASES/cases.tv           id|title|summary|status|created|updated
//     RECORDS/records.tv       id|case|type|status|title|body|tags|source|
//                              created|event_at|updated
//     CONNECTIONS/links.tv     id|case|from|to|relation|note|created
//     KNOWLEDGE/cards.tv       id|front|back|tags|due|reviews
//     DECISIONS/decisions.tv   id|title|options|rationale|confidence|expected|
//                              outcome|lesson|created|review_at
//     QUESTION/ MEMORY/ SESSIONS/            reserved, created empty
//     EXPORTS/ BACKUPS/ LOGS/ CACHE/ SETTINGS/
//
//  Every store is rewritten atomically: write <file>.tmp, verify the byte
//  count, keep the previous committed file as <file>.bak, rename .tmp into
//  place.  On boot a missing store falls back to its .bak and stale .tmp files
//  are discarded, so an interrupted write can never look like a save.
//
//  Everything is local and offline.  No network code is linked in.
// ============================================================================

#include <Arduino.h>
#include <FS.h>
#include <SD_MMC.h>
#include <Wire.h>
#include <WiFi.h>
#include <Preferences.h>
#include <time.h>
#include <sys/time.h>
#include <memory>
#include <ctype.h>
#include <math.h>
#include <stdlib.h>

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

#if LVGL_VERSION_MAJOR != 8 || LVGL_VERSION_MINOR != 4
#error "The Mentalist is built against LVGL 8.4.x (see lv_conf.h in this folder)."
#endif
#if !LV_USE_KEYBOARD || !LV_USE_TEXTAREA
#error "lv_conf.h must enable LV_USE_KEYBOARD and LV_USE_TEXTAREA for the capture editor."
#endif

// ---------------------------------------------------------------------------
//  Identity
// ---------------------------------------------------------------------------
#define OS_NAME          "Mentalist"
#define OS_MARK          "MENTALIST//OS"
#define OS_FOLDER        "/WATCH/Mentalist"
#define OS_VERSION       "1.14"

// External processing stays off in V1: there is no radio code path here at all.
#define MENTALIST_ENABLE_COMPANION 0

// ---------------------------------------------------------------------------
//  Hardware (identical to the rest of the V2 apps on this board)
// ---------------------------------------------------------------------------
#define LCD_SDIO0 4
#define LCD_SDIO1 5
#define LCD_SDIO2 6
#define LCD_SDIO3 7
#define LCD_SCLK  11
#define LCD_CS    12
#define LCD_RST   8
#define LCD_W     410
#define LCD_H     502

#define IIC_SDA   15
#define IIC_SCL   14
#define TP_INT    38
#define TP_RESET  9
#define BOOT_BTN  0

#define SDMMC_CLK  2
#define SDMMC_CMD  1
#define SDMMC_DATA 3

#define BACKLIGHT_MAX 255

Arduino_DataBus *bus = new Arduino_ESP32QSPI(LCD_CS, LCD_SCLK, LCD_SDIO0, LCD_SDIO1,
                                             LCD_SDIO2, LCD_SDIO3);
Arduino_GFX *gfx = new Arduino_CO5300(bus, LCD_RST, 0, LCD_W, LCD_H, 22, 0, 0, 0);

std::shared_ptr<Arduino_IIC_DriveBus> IIC_Bus =
    std::make_shared<Arduino_HWIIC>(IIC_SDA, IIC_SCL, &Wire);
void Arduino_IIC_Touch_Interrupt(void);
std::unique_ptr<Arduino_IIC> FT3168(new Arduino_FT3x68(
    IIC_Bus, FT3168_DEVICE_ADDRESS, DRIVEBUS_DEFAULT_VALUE, TP_INT,
    Arduino_IIC_Touch_Interrupt));
XPowersPMU PMU;

// ---------------------------------------------------------------------------
//  Timing -- LauncherOS policy
// ---------------------------------------------------------------------------
#define DEFAULT_BRIGHTNESS    180
#define SCREEN_OFF_TIMEOUT_MS 15000UL
#define WAKE_GRACE_MS         2000UL

// Screen timeout is a setting, not a constant: cycled in SYSTEM and kept in NVS.
static const uint32_t SCREEN_TIMEOUT_OPTS[4] = {10000UL, 20000UL, 30000UL,
                                                60000UL};
static uint32_t g_screen_off_ms = 10000UL;

// A dark watch is not an off watch. After the screen timeout the face goes
// away and the CPU drops into light sleep with PSRAM and every store still
// resident; two minutes later the session is saved and the watch powers down.
#define DEEP_SLEEP_MS       120000UL
#define LIGHT_SLEEP_POLL_MS 250UL
#define ENABLE_LIGHT_SLEEP  1
#define RETURN_HOLD_MS        1500UL
#define CLICK_AFTER_SWIPE_MS  350UL
#define SWIPE_THRESHOLD_PX    25
#define SWIPE_MAX_DURATION_MS 800UL

#define STATUS_TICK_MS        1000UL
#define AUTOSAVE_MIN_MS       60000UL

// ---------------------------------------------------------------------------
//  Theme -- charcoal, surface, amber, cream, grey, red thread
// ---------------------------------------------------------------------------
#define COL_BG      0x171717
#define COL_SURF    0x30291F
#define COL_SURF_HI 0x3C3327
#define COL_AMBER   0xB18A50
#define COL_CREAM   0xE8DCC3
#define COL_MUTED   0x77766F
#define COL_THREAD  0xC34E4E
#define COL_LINE    0x413A2E
#define COL_SURF_LO  0x241F18
#define COL_AMBER_DK 0x6E5530

// ---------------------------------------------------------------------------
//  Data model
// ---------------------------------------------------------------------------
#define MAX_RECORDS   120
#define MAX_CASES     40
#define MAX_LINKS     120
#define MAX_CARDS     80
#define MAX_DECISIONS 30

#define TITLE_LEN  44
#define BODY_LEN   200
#define TAG_LEN    40
#define SRC_LEN    36
#define SUM_LEN    100
#define FRONT_LEN  60
#define BACK_LEN   120
#define OPT_LEN    120
#define RAT_LEN    120
#define DEC_FIELD  80

enum RecType {
  TYPE_OBSERVATION = 0,
  TYPE_QUESTION,
  TYPE_HYPOTHESIS,
  TYPE_SOURCE,
  TYPE_EVENT,
  TYPE_CONCEPT,
  TYPE_FACT,
  TYPE_READING,
  TYPE_WORD,
  TYPE_IDEA,
  TYPE_PAUSE,
  TYPE_PERSPECTIVE,
  TYPE_COUNT
};

enum RecStatus {
  ST_UNVERIFIED = 0,
  ST_CORROBORATED,
  ST_CONTRADICTED,
  ST_STALE,
  ST_ARCHIVED,
  ST_OPEN,
  ST_INVESTIGATING,
  ST_ANSWERED,
  ST_PARKED
};

enum Relation {
  REL_SUPPORTS = 0,
  REL_CONTRADICTS,
  REL_CAUSES,
  REL_PRECEDES,
  REL_EXPLAINS,
  REL_SIMILAR,
  REL_UNRELATED,
  REL_COUNT
};

enum CaseStatus { CASE_ACTIVE = 0, CASE_COLD, CASE_CLOSED };

struct Record {
  uint32_t id;
  uint32_t case_id;
  uint8_t  type;
  uint8_t  status;
  char     title[TITLE_LEN];
  char     body[BODY_LEN];
  char     tags[TAG_LEN];
  char     source[SRC_LEN];
  uint32_t created;
  uint32_t event_at;
  uint32_t updated;
};

struct CaseRec {
  uint32_t id;
  char     title[TITLE_LEN];
  char     summary[SUM_LEN];
  uint8_t  status;
  uint32_t created;
  uint32_t updated;
};

struct Thread {
  uint32_t id;
  uint32_t case_id;
  uint32_t from_id;
  uint32_t to_id;
  uint8_t  relation;
  char     note[TAG_LEN];
  uint32_t created;
};

struct Card {
  uint32_t id;
  char     front[FRONT_LEN];
  char     back[BACK_LEN];
  char     tags[TAG_LEN];
  uint32_t due;
  uint16_t reviews;
};

struct Decision {
  uint32_t id;
  char     title[TITLE_LEN];
  char     options[OPT_LEN];
  char     rationale[RAT_LEN];
  uint8_t  confidence;   // 1..5
  char     expected[DEC_FIELD];
  char     outcome[DEC_FIELD];
  char     lesson[DEC_FIELD];
  uint32_t created;
  uint32_t review_at;
};

struct Draft {
  uint8_t  type;
  uint8_t  status;
  uint32_t case_id;
  bool     dirty;
  char     title[TITLE_LEN];
  char     body[BODY_LEN];
  char     tags[TAG_LEN];
  char     source[SRC_LEN];
};

// Declared before every function: the .ino prototype pass runs above the code
// and must never meet an undeclared sketch type in a signature.
struct AreaItem {
  const char *title;
  const char *meta;
  int action;
};

// ---------------------------------------------------------------------------
//  Names
// ---------------------------------------------------------------------------
static const char *TYPE_NAMES[TYPE_COUNT] = {
    "Observation", "Question", "Hypothesis", "Source", "Event", "Concept",
    "Fact",        "Reading",  "Word",       "Idea",   "Pause", "Perspective"};

static const char *TYPE_SHORT[TYPE_COUNT] = {
    "OBS", "QST", "HYP", "SRC", "EVT", "CON", "FACT", "READ", "WORD", "IDEA",
    "PAUSE", "PERSP"};

static const char *EVIDENCE_STATUS[6] = {"Unverified", "Corroborated",
                                        "Contradicted", "Stale", "Archived",
                                        "(unknown)"};

static const char *QUESTION_STATUS[4] = {"Open", "Investigating", "Answered",
                                         "Parked"};

static const char *RELATION_NAMES[REL_COUNT] = {
    "supports", "contradicts", "causes", "precedes",
    "explains", "similar to", "not related"};

static const char *CASE_STATUS_NAMES[3] = {"Active", "Cold", "Closed"};

// ---------------------------------------------------------------------------
//  RAM stores
// ---------------------------------------------------------------------------
Record  *RECORDS   = nullptr;
int      g_record_count = 0;
uint32_t g_record_next  = 1;

CaseRec *CASES     = nullptr;
int      g_case_count = 0;
uint32_t g_case_next  = 1;

Thread  *LINKS     = nullptr;
int      g_link_count = 0;
uint32_t g_link_next  = 1;

Card    *CARDS     = nullptr;
int      g_card_count = 0;
uint32_t g_card_next  = 1;

Decision *DECISIONS = nullptr;
int      g_decision_count = 0;
uint32_t g_decision_next  = 1;

bool     g_stores_ok = false;

// At their caps the five stores need ~90 KB, most of what the S3 has left after
// LVGL's heap and the draw buffer. Allocate once from PSRAM when present,
// internal RAM otherwise. Only the base pointers are static, so every existing
// RECORDS[i] call site keeps working.
static void *store_alloc(size_t bytes) {
  void *p = heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!p) p = heap_caps_malloc(bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  return p;
}

static bool alloc_stores() {
  RECORDS   = (Record *)store_alloc(sizeof(Record) * MAX_RECORDS);
  CASES     = (CaseRec *)store_alloc(sizeof(CaseRec) * MAX_CASES);
  LINKS     = (Thread *)store_alloc(sizeof(Thread) * MAX_LINKS);
  CARDS     = (Card *)store_alloc(sizeof(Card) * MAX_CARDS);
  DECISIONS = (Decision *)store_alloc(sizeof(Decision) * MAX_DECISIONS);
  g_stores_ok = RECORDS && CASES && LINKS && CARDS && DECISIONS;
  if (!g_stores_ok) return false;
  memset(RECORDS, 0, sizeof(Record) * MAX_RECORDS);
  memset(CASES, 0, sizeof(CaseRec) * MAX_CASES);
  memset(LINKS, 0, sizeof(Thread) * MAX_LINKS);
  memset(CARDS, 0, sizeof(Card) * MAX_CARDS);
  memset(DECISIONS, 0, sizeof(Decision) * MAX_DECISIONS);
  return true;
}

Draft    g_draft;
int      g_capture_type = TYPE_OBSERVATION;

// ---------------------------------------------------------------------------
//  Storage state
// ---------------------------------------------------------------------------
static bool g_sd_ok = false;
static char g_sd_error[48] = "not mounted";
static char g_store_error[48] = "";
static uint32_t g_store_error_ms = 0;
static int g_damaged_lines = 0;   // lines rejected by the checksum on load

static const char *PATH_CASES    = OS_FOLDER "/CASES/cases.tv";
static const char *PATH_RECORDS  = OS_FOLDER "/RECORDS/records.tv";
static const char *PATH_LINKS    = OS_FOLDER "/CONNECTIONS/links.tv";
static const char *PATH_CARDS    = OS_FOLDER "/KNOWLEDGE/cards.tv";
static const char *PATH_DECISION = OS_FOLDER "/DECISIONS/decisions.tv";
static const char *PATH_SETTINGS = OS_FOLDER "/SETTINGS/settings.json";
static const char *PATH_LOG      = OS_FOLDER "/LOGS/boot.csv";
static const char *PATH_VERSION  = OS_FOLDER "/.folder_version";

// Declared up here because the System screen reads it before the power
// section defines its own state.
static bool g_has_pmu = false;

static const char *STORE_PATHS[5] = {PATH_CASES, PATH_RECORDS, PATH_LINKS,
                                     PATH_CARDS, PATH_DECISION};
static const char *STORE_HEADS[5] = {
    "# Mentalist cases schema=1", "# Mentalist records schema=1",
    "# Mentalist links schema=1", "# Mentalist cards schema=1",
    "# Mentalist decisions schema=1"};

// ---------------------------------------------------------------------------
//  Shared time block (spec: TIME_CONTRACT.md) -- spliced verbatim
// ---------------------------------------------------------------------------
// ---- shared time block (spec: TIME_CONTRACT.md) -----------------------------
#define TIME_SERIAL Serial
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
static void restore_clock(){ time_restore_from_nvs(); }

// ---------------------------------------------------------------------------
//  Small helpers
// ---------------------------------------------------------------------------
static void copy_field(char *dst, size_t n, const char *src) {
  if (!dst || n == 0) return;
  if (!src) src = "";
  size_t out = 0;
  for (size_t i = 0; src[i] && out + 1 < n; i++) {
    char c = src[i];
    if (c == '|' || c == '\n' || c == '\r' || c == '\t') c = ' ';
    dst[out++] = c;
  }
  while (out > 0 && dst[out - 1] == ' ') out--;
  dst[out] = 0;
}

static void sanitize_input(char *text) {
  if (!text) return;
  char *w = text;
  for (char *r = text; *r; r++) {
    if (*r == '|' || *r == '\n' || *r == '\r' || *r == '\t') {
      if (w > text && w[-1] != ' ') *w++ = ' ';
    } else {
      *w++ = *r;
    }
  }
  *w = 0;
}

static int split_fields(char *line, char **out, int maxFields) {
  int n = 0;
  char *p = line;
  while (n < maxFields) {
    out[n++] = p;
    char *bar = strchr(p, '|');
    if (!bar) break;
    *bar = 0;
    p = bar + 1;
  }
  return n;
}

// Every store line ends in "|~XXXX": a hash of the payload before it. Lines
// from older builds carry no marker and still load, so the format stays
// backward compatible. A line whose payload no longer matches its hash is
// counted as damaged instead of being accepted as if it were intact.
static uint16_t line_hash_n(const char *s, size_t n) {
  uint32_t h = 2166136261u;
  for (size_t i = 0; i < n; i++) {
    h ^= (uint8_t)s[i];
    h *= 16777619u;
  }
  return (uint16_t)((h >> 16) ^ (h & 0xFFFF));
}

static bool hex4(const char *s) {
  for (int i = 0; i < 4; i++) {
    char c = s[i];
    if (!((c >= '0' && c <= '9') || (c >= 'A' && c <= 'F') ||
          (c >= 'a' && c <= 'f')))
      return false;
  }
  return s[4] == 0;
}

static void append_line(String &out, const char *payload) {
  char tail[10];
  snprintf(tail, sizeof(tail), "|~%04X\n", line_hash_n(payload, strlen(payload)));
  out += payload;
  out += tail;
}

static bool line_intact(const char *line) {
  const char *bar = strrchr(line, '|');
  if (!bar || bar[1] != '~' || !hex4(bar + 2)) return true;  // legacy line
  unsigned want = (unsigned)strtoul(bar + 2, nullptr, 16);
  return line_hash_n(line, (size_t)(bar - line)) == (uint16_t)want;
}

static uint32_t clamp_ts(uint32_t ts) {
  return (ts < (uint32_t)TIME_VALID_MIN) ? 0 : ts;
}

static void fmt_datetime(char *out, size_t n, uint32_t ts) {
  if (!out || n == 0) return;
  if (ts < (uint32_t)TIME_VALID_MIN) {
    snprintf(out, n, "--");
    return;
  }
  time_t t = (time_t)ts;
  struct tm tmv;
  localtime_r(&t, &tmv);
  strftime(out, n, "%d %b %H:%M", &tmv);
}

static void fmt_dateonly(char *out, size_t n, uint32_t ts) {
  if (!out || n == 0) return;
  if (ts < (uint32_t)TIME_VALID_MIN) {
    snprintf(out, n, "--");
    return;
  }
  time_t t = (time_t)ts;
  struct tm tmv;
  localtime_r(&t, &tmv);
  strftime(out, n, "%Y-%m-%d %H:%M", &tmv);
}

static uint32_t parse_datetime(const char *text) {
  if (!text) return 0;
  int y = 0, mo = 0, d = 0, h = 0, mi = 0;
  int got = sscanf(text, "%d-%d-%d %d:%d", &y, &mo, &d, &h, &mi);
  if (got < 3) return 0;
  if (y < 2000 || y > 2100 || mo < 1 || mo > 12 || d < 1 || d > 31) return 0;
  if (h < 0 || h > 23 || mi < 0 || mi > 59) return 0;
  struct tm tmv;
  memset(&tmv, 0, sizeof(tmv));
  tmv.tm_year = y - 1900;
  tmv.tm_mon  = mo - 1;
  tmv.tm_mday = d;
  tmv.tm_hour = h;
  tmv.tm_min  = mi;
  tmv.tm_isdst = -1;
  time_t t = mktime(&tmv);
  if (t < (time_t)TIME_VALID_MIN) return 0;
  return (uint32_t)t;
}

static const char *status_name(int type, int status) {
  if (type == TYPE_QUESTION) {
    if (status > 3) status = 0;
    return QUESTION_STATUS[status];
  }
  if (status > 4) status = 0;
  return EVIDENCE_STATUS[status];
}

static int status_slots(int type) { return (type == TYPE_QUESTION) ? 4 : 5; }

static const char *relation_name(int rel) {
  if (rel < 0 || rel >= REL_COUNT) return RELATION_NAMES[REL_UNRELATED];
  return RELATION_NAMES[rel];
}

static const char *case_title(uint32_t id) {
  if (id == 0) return "no case";
  for (int i = 0; i < g_case_count; i++)
    if (CASES[i].id == id) return CASES[i].title;
  return "deleted case";
}

static int find_case_index(uint32_t id) {
  for (int i = 0; i < g_case_count; i++)
    if (CASES[i].id == id) return i;
  return -1;
}

static int find_record_index(uint32_t id) {
  for (int i = 0; i < g_record_count; i++)
    if (RECORDS[i].id == id) return i;
  return -1;
}

static int find_card_index(uint32_t id) {
  for (int i = 0; i < g_card_count; i++)
    if (CARDS[i].id == id) return i;
  return -1;
}

static int find_decision_index(uint32_t id) {
  for (int i = 0; i < g_decision_count; i++)
    if (DECISIONS[i].id == id) return i;
  return -1;
}

static void set_store_error(const char *msg) {
  snprintf(g_store_error, sizeof(g_store_error), "%s", msg ? msg : "SAVE FAILED");
  g_store_error_ms = millis();
}

// ---------------------------------------------------------------------------
//  Atomic file writes
// ---------------------------------------------------------------------------
static uint32_t payload_hash(const char *s, size_t n) {
  uint32_t h = 2166136261u;
  for (size_t i = 0; i < n; i++) {
    h ^= (uint8_t)s[i];
    h *= 16777619u;
  }
  return h;
}

static int store_slot(const char *path) {
  if (!path) return -1;
  for (int i = 0; i < 5; i++)
    if (strcmp(path, STORE_PATHS[i]) == 0) return i;
  return -1;
}

// Last payload written per store, this session. Screens save on exit, so an
// unchanged store was being re-serialised and rewritten for nothing.
static uint32_t g_written_hash[5] = {0, 0, 0, 0, 0};
static bool     g_written_seen[5] = {false, false, false, false, false};

static bool atomic_write(const char *path, const String &content) {
  if (!g_sd_ok) {
    set_store_error("SD UNAVAILABLE");
    return false;
  }
  int slot = store_slot(path);
  if (slot >= 0) {
    uint32_t h = payload_hash(content.c_str(), content.length());
    if (g_written_seen[slot] && g_written_hash[slot] == h) return true;
  }
  String tmp = String(path) + ".tmp";
  String bak = String(path) + ".bak";

  File f = SD_MMC.open(tmp.c_str(), FILE_WRITE);
  if (!f) {
    set_store_error("SD WRITE FAILED");
    return false;
  }
  size_t put = f.print(content);
  f.flush();
  f.close();
  if (put != content.length()) {
    SD_MMC.remove(tmp.c_str());
    set_store_error("SD WRITE FAILED");
    return false;
  }
  if (SD_MMC.exists(path)) {
    SD_MMC.remove(bak.c_str());
    SD_MMC.rename(path, bak.c_str());
  }
  if (!SD_MMC.rename(tmp.c_str(), path)) {
    set_store_error("SD RENAME FAILED");
    return false;
  }
  if (slot >= 0) {
    g_written_hash[slot] = payload_hash(content.c_str(), content.length());
    g_written_seen[slot] = true;
  }
  return true;
}

static File open_store(const char *path, bool *recovered) {
  File f = SD_MMC.open(path, FILE_READ);
  if (f) return f;
  String bak = String(path) + ".bak";
  f = SD_MMC.open(bak.c_str(), FILE_READ);
  if (f && recovered) *recovered = true;
  return f;
}

static bool read_all(const char *path, String &out) {
  File f = SD_MMC.open(path, FILE_READ);
  if (!f) return false;
  out = "";
  while (f.available()) out += (char)f.read();
  f.close();
  return out.length() < 48000;
}

static void ensure_store_file(const char *path, const char *head) {
  String bak = String(path) + ".bak";
  if (SD_MMC.exists(path) || SD_MMC.exists(bak.c_str())) return;
  String content = String(head) + "\n";
  atomic_write(path, content);
}

// ---------------------------------------------------------------------------
//  Loading
// ---------------------------------------------------------------------------
static void load_cases() {
  g_case_count = 0;
  g_case_next  = 1;
  bool recovered = false;
  File f = open_store(PATH_CASES, &recovered);
  if (!f) return;
  char line[220];
  while (f.available() && g_case_count < MAX_CASES) {
    size_t n = f.readBytesUntil('\n', line, sizeof(line) - 1);
    line[n] = 0;
    if (line[0] == 0 || line[0] == '#') continue;
    if (!line_intact(line)) {
      g_damaged_lines++;
      continue;
    }
    char *fl[8];
    if (split_fields(line, fl, 8) < 6) {
      g_damaged_lines++;
      continue;
    }
    CaseRec &c = CASES[g_case_count];
    memset(&c, 0, sizeof(c));
    c.id = strtoul(fl[0], nullptr, 10);
    if (c.id == 0) continue;
    copy_field(c.title, sizeof(c.title), fl[1]);
    copy_field(c.summary, sizeof(c.summary), fl[2]);
    c.status  = (uint8_t)atoi(fl[3]);
    if (c.status > 2) c.status = 0;
    c.created = strtoul(fl[4], nullptr, 10);
    c.updated = strtoul(fl[5], nullptr, 10);
    if (c.id >= g_case_next) g_case_next = c.id + 1;
    g_case_count++;
  }
  f.close();
  if (recovered) set_store_error("CASES RESTORED FROM BACKUP");
}

static void voice_load();

static void load_records() {
  g_record_count = 0;
  g_record_next  = 1;
  bool recovered = false;
  File f = open_store(PATH_RECORDS, &recovered);
  if (!f) return;
  char line[480];
  while (f.available() && g_record_count < MAX_RECORDS) {
    size_t n = f.readBytesUntil('\n', line, sizeof(line) - 1);
    line[n] = 0;
    if (line[0] == 0 || line[0] == '#') continue;
    if (!line_intact(line)) {
      g_damaged_lines++;
      continue;
    }
    char *fl[16];
    if (split_fields(line, fl, 16) < 11) {
      g_damaged_lines++;
      continue;
    }
    Record &r = RECORDS[g_record_count];
    memset(&r, 0, sizeof(r));
    r.id = strtoul(fl[0], nullptr, 10);
    if (r.id == 0) continue;
    r.case_id = strtoul(fl[1], nullptr, 10);
    r.type    = (uint8_t)atoi(fl[2]);
    if (r.type >= TYPE_COUNT) r.type = TYPE_OBSERVATION;
    r.status = (uint8_t)atoi(fl[3]);
    if (r.status > 4) r.status = 0;
    copy_field(r.title, sizeof(r.title), fl[4]);
    copy_field(r.body, sizeof(r.body), fl[5]);
    copy_field(r.tags, sizeof(r.tags), fl[6]);
    copy_field(r.source, sizeof(r.source), fl[7]);
    r.created  = strtoul(fl[8], nullptr, 10);
    r.event_at = strtoul(fl[9], nullptr, 10);
    r.updated  = strtoul(fl[10], nullptr, 10);
    if (r.id >= g_record_next) g_record_next = r.id + 1;
    g_record_count++;
  }
  f.close();
  if (recovered) set_store_error("RECORDS RESTORED FROM BACKUP");
}

static void load_links() {
  g_link_count = 0;
  g_link_next  = 1;
  File f = open_store(PATH_LINKS, nullptr);
  if (!f) return;
  char line[200];
  while (f.available() && g_link_count < MAX_LINKS) {
    size_t n = f.readBytesUntil('\n', line, sizeof(line) - 1);
    line[n] = 0;
    if (line[0] == 0 || line[0] == '#') continue;
    if (!line_intact(line)) {
      g_damaged_lines++;
      continue;
    }
    char *fl[10];
    if (split_fields(line, fl, 10) < 7) {
      g_damaged_lines++;
      continue;
    }
    Thread &t = LINKS[g_link_count];
    memset(&t, 0, sizeof(t));
    t.id = strtoul(fl[0], nullptr, 10);
    if (t.id == 0) continue;
    t.case_id = strtoul(fl[1], nullptr, 10);
    t.from_id = strtoul(fl[2], nullptr, 10);
    t.to_id   = strtoul(fl[3], nullptr, 10);
    t.relation = (uint8_t)atoi(fl[4]);
    if (t.relation >= REL_COUNT) t.relation = REL_UNRELATED;
    copy_field(t.note, sizeof(t.note), fl[5]);
    t.created = strtoul(fl[6], nullptr, 10);
    if (t.id >= g_link_next) g_link_next = t.id + 1;
    g_link_count++;
  }
  f.close();
}

static void load_cards() {
  g_card_count = 0;
  g_card_next  = 1;
  File f = open_store(PATH_CARDS, nullptr);
  if (!f) return;
  char line[300];
  while (f.available() && g_card_count < MAX_CARDS) {
    size_t n = f.readBytesUntil('\n', line, sizeof(line) - 1);
    line[n] = 0;
    if (line[0] == 0 || line[0] == '#') continue;
    if (!line_intact(line)) {
      g_damaged_lines++;
      continue;
    }
    char *fl[10];
    if (split_fields(line, fl, 10) < 6) {
      g_damaged_lines++;
      continue;
    }
    Card &c = CARDS[g_card_count];
    memset(&c, 0, sizeof(c));
    c.id = strtoul(fl[0], nullptr, 10);
    if (c.id == 0) continue;
    copy_field(c.front, sizeof(c.front), fl[1]);
    copy_field(c.back, sizeof(c.back), fl[2]);
    copy_field(c.tags, sizeof(c.tags), fl[3]);
    c.due     = strtoul(fl[4], nullptr, 10);
    c.reviews = (uint16_t)atoi(fl[5]);
    if (c.id >= g_card_next) g_card_next = c.id + 1;
    g_card_count++;
  }
  f.close();
}

static void load_decisions() {
  g_decision_count = 0;
  g_decision_next  = 1;
  File f = open_store(PATH_DECISION, nullptr);
  if (!f) return;
  char line[560];
  while (f.available() && g_decision_count < MAX_DECISIONS) {
    size_t n = f.readBytesUntil('\n', line, sizeof(line) - 1);
    line[n] = 0;
    if (line[0] == 0 || line[0] == '#') continue;
    if (!line_intact(line)) {
      g_damaged_lines++;
      continue;
    }
    char *fl[14];
    if (split_fields(line, fl, 14) < 10) {
      g_damaged_lines++;
      continue;
    }
    Decision &d = DECISIONS[g_decision_count];
    memset(&d, 0, sizeof(d));
    d.id = strtoul(fl[0], nullptr, 10);
    if (d.id == 0) continue;
    copy_field(d.title, sizeof(d.title), fl[1]);
    copy_field(d.options, sizeof(d.options), fl[2]);
    copy_field(d.rationale, sizeof(d.rationale), fl[3]);
    d.confidence = (uint8_t)atoi(fl[4]);
    if (d.confidence < 1 || d.confidence > 5) d.confidence = 3;
    copy_field(d.expected, sizeof(d.expected), fl[5]);
    copy_field(d.outcome, sizeof(d.outcome), fl[6]);
    copy_field(d.lesson, sizeof(d.lesson), fl[7]);
    d.created   = strtoul(fl[8], nullptr, 10);
    d.review_at = strtoul(fl[9], nullptr, 10);
    if (d.id >= g_decision_next) g_decision_next = d.id + 1;
    g_decision_count++;
  }
  f.close();
}

static void load_all_stores() {
  g_damaged_lines = 0;
  load_cases();
  load_records();
  voice_load();
  load_links();
  load_cards();
  load_decisions();
}

// ---------------------------------------------------------------------------
//  Saving
// ---------------------------------------------------------------------------
static bool save_cases() {
  if (!g_sd_ok) return false;
  String out = String(STORE_HEADS[0]) + "\n";
  char line[240];
  for (int i = 0; i < g_case_count; i++) {
    CaseRec &c = CASES[i];
    snprintf(line, sizeof(line), "%lu|%s|%s|%u|%lu|%lu", (unsigned long)c.id,
             c.title, c.summary, (unsigned)c.status, (unsigned long)c.created,
             (unsigned long)c.updated);
    append_line(out, line);
  }
  bool ok = atomic_write(PATH_CASES, out);
  if (!ok) set_store_error("CASE SAVE FAILED");
  return ok;
}

static bool save_records() {
  if (!g_sd_ok) return false;
  String out = String(STORE_HEADS[1]) + "\n";
  char line[520];
  for (int i = 0; i < g_record_count; i++) {
    Record &r = RECORDS[i];
    snprintf(line, sizeof(line), "%lu|%lu|%u|%u|%s|%s|%s|%s|%lu|%lu|%lu",
             (unsigned long)r.id, (unsigned long)r.case_id, (unsigned)r.type,
             (unsigned)r.status, r.title, r.body, r.tags, r.source,
             (unsigned long)r.created, (unsigned long)r.event_at,
             (unsigned long)r.updated);
    append_line(out, line);
  }
  bool ok = atomic_write(PATH_RECORDS, out);
  if (!ok) set_store_error("RECORD SAVE FAILED");
  return ok;
}

static bool save_links() {
  if (!g_sd_ok) return false;
  String out = String(STORE_HEADS[2]) + "\n";
  char line[240];
  for (int i = 0; i < g_link_count; i++) {
    Thread &t = LINKS[i];
    snprintf(line, sizeof(line), "%lu|%lu|%lu|%lu|%u|%s|%lu",
             (unsigned long)t.id, (unsigned long)t.case_id,
             (unsigned long)t.from_id, (unsigned long)t.to_id,
             (unsigned)t.relation, t.note, (unsigned long)t.created);
    append_line(out, line);
  }
  bool ok = atomic_write(PATH_LINKS, out);
  if (!ok) set_store_error("LINK SAVE FAILED");
  return ok;
}

static bool save_cards() {
  if (!g_sd_ok) return false;
  String out = String(STORE_HEADS[3]) + "\n";
  char line[320];
  for (int i = 0; i < g_card_count; i++) {
    Card &c = CARDS[i];
    snprintf(line, sizeof(line), "%lu|%s|%s|%s|%lu|%u", (unsigned long)c.id,
             c.front, c.back, c.tags, (unsigned long)c.due,
             (unsigned)c.reviews);
    append_line(out, line);
  }
  bool ok = atomic_write(PATH_CARDS, out);
  if (!ok) set_store_error("CARD SAVE FAILED");
  return ok;
}

static bool save_decisions() {
  if (!g_sd_ok) return false;
  String out = String(STORE_HEADS[4]) + "\n";
  char line[620];
  for (int i = 0; i < g_decision_count; i++) {
    Decision &d = DECISIONS[i];
    snprintf(line, sizeof(line), "%lu|%s|%s|%s|%u|%s|%s|%s|%lu|%lu",
             (unsigned long)d.id, d.title, d.options, d.rationale,
             (unsigned)d.confidence, d.expected, d.outcome, d.lesson,
             (unsigned long)d.created, (unsigned long)d.review_at);
    append_line(out, line);
  }
  bool ok = atomic_write(PATH_DECISION, out);
  if (!ok) set_store_error("DECISION SAVE FAILED");
  return ok;
}

static void save_all_stores() {
  save_cases();
  save_records();
  save_links();
  save_cards();
  save_decisions();
}

// ---------------------------------------------------------------------------
//  Case / record / link mutation
// ---------------------------------------------------------------------------
static int create_case(const char *title, const char *summary) {
  if (g_case_count >= MAX_CASES) {
    set_store_error("CASE STORE FULL");
    return -1;
  }
  CaseRec &c = CASES[g_case_count];
  memset(&c, 0, sizeof(c));
  c.id      = g_case_next++;
  c.status  = CASE_ACTIVE;
  c.created = (uint32_t)time(nullptr);
  c.updated = c.created;
  copy_field(c.title, sizeof(c.title), title);
  copy_field(c.summary, sizeof(c.summary), summary);
  g_case_count++;
  save_cases();
  return g_case_count - 1;
}

static int create_record(uint8_t type, const char *title, const char *body,
                         const char *tags, const char *source,
                         uint32_t case_id) {
  if (g_record_count >= MAX_RECORDS) {
    set_store_error("RECORD STORE FULL");
    return -1;
  }
  Record &r = RECORDS[g_record_count];
  memset(&r, 0, sizeof(r));
  r.id      = g_record_next++;
  r.type    = type;
  r.status  = 0;
  r.case_id = case_id;
  r.created = (uint32_t)time(nullptr);
  r.updated = r.created;
  copy_field(r.title, sizeof(r.title), title);
  copy_field(r.body, sizeof(r.body), body);
  copy_field(r.tags, sizeof(r.tags), tags);
  copy_field(r.source, sizeof(r.source), source);
  g_record_count++;
  if (!save_records()) return -1;
  return g_record_count - 1;
}

static void delete_record_at(int idx) {
  if (idx < 0 || idx >= g_record_count) return;
  uint32_t id = RECORDS[idx].id;
  for (int i = idx; i < g_record_count - 1; i++) RECORDS[i] = RECORDS[i + 1];
  g_record_count--;
  int w = 0;
  for (int i = 0; i < g_link_count; i++) {
    if (LINKS[i].from_id == id || LINKS[i].to_id == id) continue;
    LINKS[w++] = LINKS[i];
  }
  g_link_count = w;
  save_records();
  save_links();
}

static void delete_case_at(int idx) {
  if (idx < 0 || idx >= g_case_count) return;
  uint32_t id = CASES[idx].id;
  for (int i = 0; i < g_record_count; i++)
    if (RECORDS[i].case_id == id) RECORDS[i].case_id = 0;
  int w = 0;
  for (int i = 0; i < g_link_count; i++) {
    if (LINKS[i].case_id == id) continue;
    LINKS[w++] = LINKS[i];
  }
  g_link_count = w;
  for (int i = idx; i < g_case_count - 1; i++) CASES[i] = CASES[i + 1];
  g_case_count--;
  save_cases();
  save_records();
  save_links();
}

static int link_count_for(uint32_t record_id) {
  int n = 0;
  for (int i = 0; i < g_link_count; i++)
    if (LINKS[i].from_id == record_id || LINKS[i].to_id == record_id) n++;
  return n;
}

static int case_link_count(uint32_t case_id, int relation) {
  int n = 0;
  for (int i = 0; i < g_link_count; i++) {
    Thread &t = LINKS[i];
    int a = find_record_index(t.from_id);
    int b = find_record_index(t.to_id);
    bool in_case = (a >= 0 && RECORDS[a].case_id == case_id) ||
                   (b >= 0 && RECORDS[b].case_id == case_id);
    if (!in_case) continue;
    if (relation >= 0 && t.relation != relation) continue;
    n++;
  }
  return n;
}

static int record_count_in_case(uint32_t case_id) {
  int n = 0;
  for (int i = 0; i < g_record_count; i++)
    if (RECORDS[i].case_id == case_id) n++;
  return n;
}

static bool link_exists(uint32_t a, uint32_t b) {
  for (int i = 0; i < g_link_count; i++) {
    if ((LINKS[i].from_id == a && LINKS[i].to_id == b) ||
        (LINKS[i].from_id == b && LINKS[i].to_id == a))
      return true;
  }
  return false;
}

static int create_link(uint32_t from_id, uint32_t to_id, uint8_t relation) {
  if (from_id == to_id) {
    set_store_error("CANNOT LINK TO ITSELF");
    return -1;
  }
  if (link_exists(from_id, to_id)) {
    set_store_error("THREAD ALREADY EXISTS");
    return -1;
  }
  if (g_link_count >= MAX_LINKS) {
    set_store_error("THREAD STORE FULL");
    return -1;
  }
  Thread &t = LINKS[g_link_count];
  memset(&t, 0, sizeof(t));
  t.id      = g_link_next++;
  t.from_id = from_id;
  t.to_id   = to_id;
  t.relation = relation;
  t.created = (uint32_t)time(nullptr);
  int from_idx = find_record_index(from_id);
  if (from_idx >= 0) t.case_id = RECORDS[from_idx].case_id;
  g_link_count++;
  if (!save_links()) return -1;
  return g_link_count - 1;
}

static void delete_link_at(int idx) {
  if (idx < 0 || idx >= g_link_count) return;
  for (int i = idx; i < g_link_count - 1; i++) LINKS[i] = LINKS[i + 1];
  g_link_count--;
  save_links();
}

// ---------------------------------------------------------------------------
//  Boot log, folders, migration
// ---------------------------------------------------------------------------
static void log_boot() {
  if (!g_sd_ok) return;
  bool fresh = !SD_MMC.exists(PATH_LOG);
  File f = SD_MMC.open(PATH_LOG, FILE_APPEND);
  if (!f) return;
  if (fresh) f.print("boot_epoch,heap_free,records,cases,damaged\n");
  f.printf("%lu,%lu,%d,%d,%d\n", (unsigned long)time(nullptr),
           (unsigned long)ESP.getFreeHeap(), g_record_count, g_case_count,
           g_damaged_lines);
  f.close();
}

static void ensure_folder() {
  if (!g_sd_ok) return;
  static const char *dirs[] = {"CASES",   "RECORDS", "CONNECTIONS", "QUESTION",
                               "MEMORY",  "KNOWLEDGE", "DECISIONS", "SESSIONS",
                               "EXPORTS", "BACKUPS", "LOGS",       "CACHE",
                               "SETTINGS"};
  if (!SD_MMC.exists(OS_FOLDER)) SD_MMC.mkdir(OS_FOLDER);
  for (size_t i = 0; i < sizeof(dirs) / sizeof(dirs[0]); i++) {
    String p = String(OS_FOLDER) + "/" + dirs[i];
    if (!SD_MMC.exists(p.c_str())) SD_MMC.mkdir(p.c_str());
  }
  if (!SD_MMC.exists(PATH_VERSION)) atomic_write(PATH_VERSION, "1\n");
  if (!SD_MMC.exists(PATH_SETTINGS)) {
    String s = "{\n  \"theme\": \"mentalist\",\n  \"screen_timeout_ms\": 15000,\n"
               "  \"default_view\": \"home\",\n  \"schema\": 1\n}\n";
    atomic_write(PATH_SETTINGS, s);
  }
}

static void recover_tmp_files() {
  if (!g_sd_ok) return;
  static const char *dirs[] = {"CASES", "RECORDS", "CONNECTIONS",
                               "KNOWLEDGE", "DECISIONS"};
  for (size_t i = 0; i < sizeof(dirs) / sizeof(dirs[0]); i++) {
    File d = SD_MMC.open((String(OS_FOLDER) + "/" + dirs[i]).c_str());
    if (!d || !d.isDirectory()) continue;
    File e = d.openNextFile();
    while (e) {
      const char *name = e.name();
      const char *slash = strrchr(name, '/');
      const char *base = slash ? slash + 1 : name;
      size_t len = strlen(base);
      if (e.isDirectory()) {
        e = d.openNextFile();
        continue;
      }
      if (len > 4 && strcmp(base + len - 4, ".tmp") == 0) {
        String full = String(OS_FOLDER) + "/" + dirs[i] + "/" + base;
        e.close();
        SD_MMC.remove(full.c_str());
        e = d.openNextFile();
        continue;
      }
      e = d.openNextFile();
    }
  }
}

// ---------------------------------------------------------------------------
//  Backup / export / restore
// ---------------------------------------------------------------------------
static uint32_t latest_backup_id() {
  if (!g_sd_ok) return 0;
  File dir = SD_MMC.open(OS_FOLDER "/BACKUPS");
  if (!dir || !dir.isDirectory()) return 0;
  uint32_t best = 0;
  File e = dir.openNextFile();
  while (e) {
    if (e.isDirectory()) {
      const char *name = e.name();
      const char *slash = strrchr(name, '/');
      const char *base = slash ? slash + 1 : name;
      uint32_t v = strtoul(base, nullptr, 10);
      if (v > best) best = v;
    }
    e = dir.openNextFile();
  }
  return best;
}

// ---------------------------------------------------------------------------
//  Voice notes
//  A voice note is an audio file plus the record that carries it. The index is
//  a separate store so the records format stays untouched and older cards keep
//  loading; an entry whose record is gone is pruned at boot.
// ---------------------------------------------------------------------------
#define MAX_VOICE      40
#define VOICE_NAME_LEN 28

struct VoiceRef {
  uint32_t rec;
  char     file[VOICE_NAME_LEN];
  uint16_t sec;
};

static VoiceRef g_voice_refs[MAX_VOICE];
static int g_voice_count = 0;
static char g_wav_names[16][VOICE_NAME_LEN];
static int g_wav_count = 0;
static const char *DIR_VOICE = OS_FOLDER "/VOICE";
static const char *PATH_VOICE = OS_FOLDER "/VOICE/index.txt";

static char lower_v(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c; }

static int voice_for_record(uint32_t id) {
  for (int i = 0; i < g_voice_count; i++)
    if (g_voice_refs[i].rec == id) return i;
  return -1;
}

static bool voice_file_in_use(const char *file) {
  for (int i = 0; i < g_voice_count; i++)
    if (strcmp(g_voice_refs[i].file, file) == 0) return true;
  return false;
}

static void vocab_settings_load();

static void voice_load() {
  g_voice_count = 0;
  if (!g_sd_ok) return;
  File f = SD_MMC.open(PATH_VOICE);
  if (!f) return;
  char line[160];
  while (f.available() && g_voice_count < MAX_VOICE) {
    size_t n = f.readBytesUntil('\n', line, sizeof(line) - 1);
    line[n] = 0;
    if (!line[0] || line[0] == '#') continue;
    if (!line_intact(line)) {
      g_damaged_lines++;
      continue;
    }
    char *fl[8];
    if (split_fields(line, fl, 8) < 3) {
      g_damaged_lines++;
      continue;
    }
    VoiceRef &v = g_voice_refs[g_voice_count];
    memset(&v, 0, sizeof(v));
    v.rec = strtoul(fl[0], nullptr, 10);
    if (v.rec == 0) continue;
    copy_field(v.file, sizeof(v.file), fl[1]);
    int sec = atoi(fl[2]);
    v.sec = (uint16_t)(sec > 0 ? sec : 1);
    g_voice_count++;
  }
  f.close();

  int keep = 0;
  for (int i = 0; i < g_voice_count; i++) {
    if (find_record_index(g_voice_refs[i].rec) < 0) continue;
    if (keep != i) g_voice_refs[keep] = g_voice_refs[i];
    keep++;
  }
  g_voice_count = keep;
  vocab_settings_load();
}

static bool voice_save() {
  if (!g_sd_ok) return false;
  String out = String("# ") + OS_MARK + " voice index\n";
  char line[160];
  for (int i = 0; i < g_voice_count; i++) {
    snprintf(line, sizeof(line), "%lu|%s|%u", (unsigned long)g_voice_refs[i].rec,
             g_voice_refs[i].file, (unsigned)g_voice_refs[i].sec);
    append_line(out, line);
  }
  bool ok = atomic_write(PATH_VOICE, out);
  if (!ok) set_store_error("VOICE INDEX SAVE FAILED");
  return ok;
}

static bool voice_attach(uint32_t rec, const char *file, uint16_t sec) {
  if (!file || !file[0] || rec == 0) return false;
  int existing = voice_for_record(rec);
  if (existing < 0) {
    if (g_voice_count >= MAX_VOICE) return false;
    existing = g_voice_count++;
    memset(&g_voice_refs[existing], 0, sizeof(g_voice_refs[existing]));
    g_voice_refs[existing].rec = rec;
  }
  copy_field(g_voice_refs[existing].file, sizeof(g_voice_refs[existing].file),
             file);
  g_voice_refs[existing].sec = (uint16_t)(sec > 0 ? sec : 1);
  return voice_save();
}

static int wav_seconds(const char *path) {
  File f = SD_MMC.open(path);
  if (!f) return -1;
  uint8_t h[44];
  int got = (int)f.read(h, 44);
  f.close();
  if (got < 44) return -1;
  if (memcmp(h, "RIFF", 4) != 0 || memcmp(h + 8, "WAVE", 4) != 0) return -1;
  uint32_t rate = (uint32_t)h[24] | ((uint32_t)h[25] << 8) |
                  ((uint32_t)h[26] << 16) | ((uint32_t)h[27] << 24);
  uint16_t chans = (uint16_t)(h[22] | (h[23] << 8));
  uint16_t bits = (uint16_t)(h[34] | (h[35] << 8));
  uint32_t data = (uint32_t)h[40] | ((uint32_t)h[41] << 8) |
                  ((uint32_t)h[42] << 16) | ((uint32_t)h[43] << 24);
  if (!rate || !chans || !bits) return -1;
  if (data == 0 || data > 0x7FFFFF) {
    File g = SD_MMC.open(path);
    if (!g) return -1;
    uint32_t size = (uint32_t)g.size();
    g.close();
    data = size > 44 ? size - 44 : 0;
  }
  uint32_t per_sec = rate * (uint32_t)chans * (uint32_t)(bits / 8);
  if (!per_sec) return -1;
  int sec = (int)(data / per_sec);
  if (sec <= 0) sec = 1;
  if (sec > 600) sec = 600;
  return sec;
}

static void wav_transcript_path(char *out, int len, const char *wav_name) {
  snprintf(out, len, OS_FOLDER "/VOICE/%s", wav_name);
  for (int i = (int)strlen(out) - 1; i >= 0 && i > (int)strlen(out) - 5; i--) {
    if (out[i] == '.') {
      out[i] = 0;
      break;
    }
  }
  int used = (int)strlen(out);
  snprintf(out + used, (size_t)(len - used), ".txt");
}

static bool wav_has_transcript(const char *wav_name) {
  char path[96];
  wav_transcript_path(path, sizeof(path), wav_name);
  return SD_MMC.exists(path);
}

// Transcripts arrive as plain text with line breaks; the record body is a
// single field, so newlines become single spaces rather than control bytes.
static void flatten_text(const char *in, char *out, int max_len) {
  int o = 0;
  bool prev_space = false;
  while (in && *in && o < max_len - 1) {
    char c = *in++;
    if (c == '\n' || c == '\r' || c == '\t') c = ' ';
    if (c == ' ') {
      if (prev_space || o == 0) continue;
      prev_space = true;
    } else {
      prev_space = false;
    }
    out[o++] = c;
  }
  while (o > 0 && out[o - 1] == ' ') o--;
  out[o] = 0;
}

// ---------------------------------------------------------------------------
//  Basic vocabulary
//  A controlled word list for transcripts: everything outside it is reported,
//  and optionally replaced with ****, so a note never pretends to have heard a
//  word it does not cover.
// ---------------------------------------------------------------------------
static const char VOCAB_DEFAULT[] =
    "a about above after again against ago all almost alone along already also "
    "always am among an and another answer any anyone anything are area around "
    "as ask asked at away back bad be because been before began begin behind "
    "being believe below best better between big both bring brought but buy by "
    "audio book business car cold doctor email game meeting photo police rain "
    "recording sick speech sun text transcript voice warm "
    "call called came can cannot care carry case certain change check child "
    "choose city clear close clue come coming could country course cut day dead "
    "decide did die do does dog done door down draw during each early earth easy "
    "eat end enough even ever every everyone everything evidence eye face fact "
    "fall family far fast father feel few find fine fire first five follow food "
    "for form found four free friend from front full get give go going gone good "
    "got great group grow had half hand happen happy hard has hate have he head "
    "hear heard heart help her here herself high him himself his hit hold home "
    "hope horse hot hour house how however hundred i idea if in inside into is "
    "issue it its itself job just keep kind know known land large last late "
    "later laugh law lead learn least leave left less let letter life light like "
    "line list little live long look lost lot love low made make man many may "
    "maybe me mean met might mind minute miss money month more morning most "
    "mother move much must my myself name near need never new next night no none "
    "nor not note nothing now number of off often oh on once one only open or "
    "other our out over own page paper part pass past people perhaps person "
    "phone pick place plan play point put question quick quiet quite raise read "
    "ready real really reason record red remember rest right road room run said "
    "same saw say school see seem seen self send sense set seven she short "
    "should show side since six small so some someone something sometimes son "
    "soon sound speak stand start stay step still stop story street strong such "
    "take talk tell ten than that the their them then there these they thing "
    "think third this those though thought three through time to today together "
    "told too took top toward town true try turn two under until up upon us use "
    "very wait walk want war was watch water way we week well went were what "
    "when where which while white who whole why will with woman word work world "
    "would write year yes yet you young your";

static const char VOCAB_EXTRA[] =
    "abandon ability absence absent absorb abstract absurd abuse accelerate access "
    "accident accompany account accountant accuse ache achieve acorn acquire acre "
    "across act action active activity actor actress actual actually adapt "
    "address adequate adjust admiral admire admit adopt advanced advantage adverse "
    "advice advise affair afford age aged agent aggressive agreement ahead "
    "aid aim aircraft airline airplane airport alarm alert alfalfa alien "
    "alike alley alligator alpaca alphabet although ambassador ambitious ambulance amount "
    "ample amuse analyst analyze anchor ancient anger angle ankle announce "
    "annoy annual antelope anxiety anxious apartment ape apologize apparent appeal "
    "appearance appetite applaud applause apply appoint appreciate apprentice approve april "
    "apron apt architect arena argue argument aromatic arrange arrest arrival "
    "art artery article artificial artist ash ashamed aspect assemble assess "
    "assignment assist assistant athlete athletic atomic attach attack attempt attend "
    "attention attic attitude attorney attract attractive audience august author authority "
    "automatic available avalanche avenue average avoid awake awareness awful awkward "
    "axle background backpack backward badge badger baggage baker bakery balance "
    "balcony balloon ban bandana bang banker banknote bar barber bare "
    "bargain barge bark barracks barrel basement basin basis bat bath "
    "bathe bathroom bathtub battle bay bazaar bead beam bear beauty "
    "bedroom beetle beginning behave behavior belief bell bench bend beneath "
    "benefit beside betray beyond bicycle bikini billion bin bind birthday "
    "bishop bison bitter blacksmith bladder blade blame blank blanket blast "
    "bleak bleed blend bless blister block blossom blot blouse blunt "
    "blush boar board boast bob bodyguard bold bolt bomb bond "
    "bonus bony bookkeeper booklet boost border bore bored boring boss "
    "botany boulevard bound boundary bow bowel boxer bracelet brag braid "
    "brake branch brand breath breed breeze brew bribe brief briefcase "
    "brilliant broad broaden bronze brook broom brown bruise bubble bucket "
    "buckle bud budget buffalo builder bulb bull bulletin bump bumper "
    "bumpy bundle burden burnt burst bush bust butcher butler butterfly "
    "button byte cabin cabinet cactus calculator calendar calf calm calorie "
    "camel camp campaign campus canal cancel candle cane canoe canvas "
    "canyon capable capacity capital captain capture cardigan career careful carefully "
    "careless cargo carnival carp carpenter carpet carriage cart carve cashier "
    "cast casual category caterpillar cathedral cattle caution cave cease cedar "
    "ceiling cellar cemetery cent centimeter centipede central ceremony certificate chalk "
    "chance chapel chapter character charge chariot charity charm charming chart "
    "chassis chat cheat cheek cheetah chef chemist chest chestnut chick "
    "chief childish chilly chimney chimpanzee chin choice choke chop chosen "
    "cinema circle circus civic clam clap clarify clash clasp classic "
    "classify classroom clay clean cleanse clearly clerk click cliff climate "
    "cling clinic clip closely closet clothe cloudless clover club clumsy "
    "cluster clutch coach coal coast coastal coax cobbler cockroach code "
    "coil coin collapse collar collarbone college colonel colony colorful colt "
    "column comb combine comedian comet comfort comic command commander comment "
    "commissioner commit commitment committee common communicate community company comparison compass "
    "compete competition compile complaint completely complex compose composer compromise conceal "
    "concept concern conclude condemn condition conductor cone confess confide confident "
    "confine conflict confront confuse confused confusion congratulate connection conquer conscience "
    "consent consequence conserve consist console constant constantly construct consult consultant "
    "consume contact contain contend content contest continent contract contractor contrast "
    "contribute convention conversation convert convey convince convoy cooperate coordinate cop "
    "cope copper coral corn correct correctly correspondent cost costly cottage "
    "cotton couch cough councilor counsel counselor counter couple courage courier "
    "court courtesy courtyard cover coyote cozy crab crack craft cramp "
    "crane crater creamy create creative credit creek creep cricket crime "
    "crisis crisp critic critical criticism criticize crocodile crooked crop crossing "
    "crouch crow crowd cruel cruiser crush crystal cub cubic cuff "
    "cultural culture cupboard curator cure curiosity curl curly currency current "
    "currently curse curtain curve curved cushion custodian custom cycle cyclone "
    "cypress daffodil daisy dam damage dampen dancer dandelion danger dangerous "
    "dare daring dash data date daybreak deadly deal dealer dean "
    "death debate debit debt december decent decimal decision deck declare "
    "decline decorate decorator decrease dedicate deeply deer defeat defend defense "
    "define definite definitely definition deflate deflect degree delay delete deliberately "
    "delicate delicious delight demand democracy demonstrate denim dense dentist deny "
    "depart departure dependent deposit depot deprive deputy derive descend deserve "
    "designer desire despair desperate detach detail detailed detect detective detergent "
    "determination determine devote devoted dew diagnose diagram dial diaper diary "
    "dictate dictionary diesel difference difficulty dig digest digital dignity dim "
    "dimple dine dinghy dinosaur dip diploma direct direction director disappear "
    "disappoint disarm discard discharge discipline disclose disconnect discourage discovery discussion "
    "disease dislike dismiss display dispose dispute dissolve distant distinct distinguish "
    "distribute district disturb dive diver dizzy dock dockworker document dodge "
    "dollar dolphin dome domestic dominate donate donkey dormitory dose double "
    "doubtful dove download downtown downward dozen draft dragonfly drain dramatic "
    "drawer dread dreadful dreamy dried drift drip drive driver drizzle "
    "drought drown drum dry dryer dual dump dune dusk dust "
    "dustpan dusty duty dwell dye eager eagle eardrum earthquake ease "
    "easily east eastern echo eclipse economic economy edge edible edit "
    "editor eel effect effective efficient effort eighth elastic elderly electric "
    "electrician elegant element elephant elevate elevator eleven eliminate elk elm "
    "embassy embrace emerge emergency emotion emotional emphasis emphasize employee empty "
    "emu enable enclose encounter endless endure energy enforce engage engagement "
    "engine engineer engraver enhance enjoyment enlarge enlist enormous enrich enroll "
    "ensure enterprise entertain enthusiasm entire entitle entrance entry envy episode "
    "epoch equal equally equator equip era erase erosion error erupt "
    "escort essay essential establish estimate estuary eternal evaluate evaporate event "
    "everyday everywhere evident examine examiner example exceed excellent except excess "
    "exchange excite exclude excuse execute exhaust exhibit exist exotic expand "
    "experience experiment expert explode explorer export expose express expression extend "
    "extra extract extreme extremely eyebrow eyelash fabric factor factory fade "
    "failure faint faith faithful falcon false fame familiar fantastic fare "
    "fashion fasten fault favor fawn fear fearless feature february federal "
    "fee feeling fellow female feminine fence fern ferret ferry fertile "
    "festival fetch fever fewer fiction fiery fifth fifty figure file "
    "film filter finalize finance finch fir firefighter firehouse firm fiscal "
    "fish fisher fishing fist fixed flamingo flap flash flashy flaw "
    "flax flea flee fleece fleet flesh flexible flip flock flood "
    "florist flow flu fluent flush foal foam focus folder foliage "
    "fond forbid force forecast forehead foreman forensic forest forge forget "
    "former formula fortune forward fossil founder fountain fraction frame frank "
    "freckle freedom freight frequent friday fridge friendship frighten frost frown "
    "frugal frustration fuel fulfill fun function fund fungus fur furious "
    "furnace furniture future fuzzy gain galaxy gallery gallon gallop gamble "
    "gap garbage gardener garment gas gateway gaze gazelle gear gecko "
    "generate generation geologist gerbil germ geyser gibbon gift gifted giraffe "
    "glacier glance gland glare glassy glide glider glimpse global globe "
    "gloomy glorious glossary glossy glow glue gnat goal goldfish golf "
    "goose gopher gorge gorgeous gorilla govern governor gown grab grace "
    "grain gram grammar grand grant graph graphic grasp grasshopper grave "
    "gravel graze grease greenhouse grim grin grind grip groan gross "
    "growl growth gruff grumble guarantee guard guilt guinea gulf gull "
    "gum gut gym habit hail hairdresser hamster handbag handkerchief handle "
    "handy handyman harbor hare harsh hasty hatch haul hawk hay "
    "haze hazel headache headquarters heal health hearty heater heather hectare "
    "hedge hedgehog heel height helicopter helmet hemp hero heron hesitate "
    "hidden highlight highly highway hike hint hip hippo historian history "
    "holiday hollow holy homeroom honesty honor hood hoodie hook hop "
    "hopeful horizon horn hornet horrible horror hose host hostile hound "
    "hover hovercraft howl hub hum humble humid humidity hummingbird hunter "
    "hurl hurricane hut hyena iceberg icy ideal identity idle iguana "
    "illness illustrate illustrator image imaginary imitate immediate immense immerse immune "
    "immunity impact imperial impolite impress impression imprison improvement improvise inactive "
    "inch incident incline income incorporate incredible indeed index indicate indoor "
    "induce indulge industrial industry infect infection infinite inflate influence informal "
    "information inherit initiate inject injection injury injustice ink inlet inn "
    "inner innovate inquire inquiry insert insight inspect inspector inspire install "
    "instance instant instinct instruct instruction instructor insult insurance insure intend "
    "intense intention interact interest interfere interior intern internal interpret interrupt "
    "intervene interview interviewer intimate invade inventor invest investigate investigator invisible "
    "involve irritate isthmus itch itchy item ivy jackal jaguar jail "
    "jam janitor january jar jaw jellyfish jersey jet jewel jeweler "
    "jewelry jog joint jolly journal journalist journey joy joyful judgment "
    "juggle juicy july jumper junction june jungle junior juniper juror "
    "justice kangaroo kayak keen keeper kennel kernel kettle keyboard kidnap "
    "kidney kill kilogram kilometer kilt kiosk kitten kneel knit knot "
    "knowledge knuckle koala lab label labor laboratory lace lack lagoon "
    "lamb landscape lane language laptop lash latch latest latter laughter "
    "launch lava lavender lawn lawyer lay layer leader leading leafy "
    "league leak lean leap lease leather lecture lecturer lemur lend "
    "length lengthy leopard lesser level liberty librarian license lichen lick "
    "lid lie lieutenant lifeboat lifeguard lifestyle lighten lighthouse lightning likewise "
    "lilac lily limb limit limited limousine limp linen link lion "
    "liquid liter literary lively liver lizard llama load loan lobby "
    "lobster local locate locksmith locomotive locust lodge lofty log logger "
    "logical loop loose loosen lorry loss lotion lousy lower loyal "
    "luck lump lunar lung lush luxury lynx machinist mackerel magazine "
    "maggot magic magician magnify magpie maid mail main major majority "
    "male mall mammal manager manner manor mansion manual map maple "
    "march mare margin marigold marina marine married marsh marshal mash "
    "mason massage massive mast master mathematics matter mattress mature maximum "
    "mayor meadow meager meaning meanwhile mechanic medic medical medicine medium "
    "meek meet mellow melodic memo memorize memory mental merchant mercy "
    "mere merge merit merry message messenger meteor meter method midday "
    "middle midnight midwife mild mile military mill million mine miner "
    "mingle minister minivan mink minnow minor minus miracle mist mistake "
    "mistreat mite mitten mixture moan mock model modest modify moist "
    "mole moment monday mongoose monitor monk monsoon monthly monument mood "
    "moose mop moped moral moreover mortar mosque mosquito moss motel "
    "moth motivate motor motorcycle mound mount mourn mow muddy mug "
    "mule multiple murky murmur muscle museum musical musician mussel mutter "
    "mutual mysterious mystery nail naked nanny nap napkin narrow native "
    "naval navel navigate nebula necessary necklace nectar negative neglect negotiate "
    "negotiator nerve neutral nevertheless newly news newt nickel nightmare ninth "
    "noble nod nominal nominate north northern notebook notion nourish novel "
    "novelist november nowhere nozzle numb numerous nursery oak oar oasis "
    "object obligation oblige observer obstacle occupy ocean october octopus officer "
    "official oily okay olive onto operate operator opinion opportunity oppose "
    "opposite optimal option oral orbit orchard orchid ordinary organic original "
    "ostrich otter ounce outcome outer outfit outgoing outlet outline outright "
    "outside oval oven overall overcoat overhear overlook overseas overtake owe "
    "owl owner ox oyster ozone pace paddle pain painful painter "
    "pair pajamas palace pale palm pancreas panda panic pansy panther "
    "pantry parachute paragraph paralegal parallel pardon park parking parlor parrot "
    "partial participate particular partner party passage passenger passion passive passport "
    "paste pastor pat patch path patience patient patio pattern pause "
    "pavement pavilion peace peaceful peacock peak pearl peculiar pedal peek "
    "peep pelican penalty penguin peninsula penny perceive percent perch perfect "
    "performance period permanent persist personal petal pharmacist pheasant philosopher photographer "
    "phrase physical physician physics pianist piece pier pierce pigeon piglet "
    "pike pile pill pillar pillow pilot pin pinch pine pinecone "
    "pink pint pipe piston pitch planet plantation plaster plastic plateau "
    "platform plaza plead pleasant please pleased pleasure pledge plenty plot "
    "plow plug plumber plunge plus pocket poem poet poetic pointed "
    "poke policy political poncho pond ponder pontoon pony poodle pool "
    "pop poppy porch porcupine port portable porter portfolio pose position "
    "positive possess possibility possible possum post postcard poster postman postpone "
    "pound poverty power powerful practical prairie praise prawn prayer preach "
    "preacher precede precious precise pregnant prepared prescribe presence preserve president "
    "pressure presume prevail previous price prick pride priest prime principal "
    "principle printer prior priority private privilege probable problem proceed process "
    "proclaim produce producer productive professional professor profit profitable profound program "
    "programmer progress prohibit project promote prompt pronounce proof propeller proper "
    "property proposal propose prospect prosper protest provoke pub public publish "
    "publisher pug pull pulse puma pump punch puncture puppy purchase "
    "pure purpose purse pursue puzzle pyramid python quail quake qualify "
    "quality quantity quarter quay quilt quiz quote raccoon rack radiator "
    "radical rail railway rainbow raincoat rainy rake rally ram ramp "
    "ranch rancher random range ranger rank rapid rash rat rate "
    "ratio rational rattlesnake raven ravine react reaction reader reality realize "
    "reasonable rebuild recall receipt receive recent receptionist recite recognize recommend "
    "recorder recover recycle reduce reduction reed reef refer reference reflect "
    "refrigerator refund refuse regard register regret regular reindeer relation relax "
    "release relevant reliable relief religion religious reluctant rely remain remark "
    "remedy remind remote remove renew renewable rent repay reporter represent "
    "representative reptile request require researcher resemble reserve resident resist resolve "
    "resort respond response responsibility restore restrict result resume retailer retain "
    "retire retired retreat reveal revenge reverse revise revive reward rhinoceros "
    "rhythm rib ribbon rickshaw rider ridge rigid rinse rip ripe "
    "ripple risk risky rival roam roar roast rob robe robin "
    "robotic rock rocket role romance romantic rooster root rose rosemary "
    "rot route routine royal rubber rudder rug ruin ruler rumor "
    "runway rupee rural rusty sacred sacrifice saddle safety sailboat sailor "
    "salary sale salesperson salmon salty sample sanctuary sandal sandbar sandy "
    "sapling sardine satin satisfaction satisfied satisfy savanna scale scan scar "
    "scarce scare scarf scary scatter scene schedule scheme scholar science "
    "scientific scientist scold scoop scooter score scorpion scrapbook scrape scream "
    "screen scribble script scrub sculptor seagull seal seam seasonal seat "
    "seatbelt second secondary secret secretary section secure security sedan seedling "
    "seek seize seldom select sell senator senior sentence separate september "
    "sequin sequoia sergeant series serpent servant service session seventh severe "
    "shallow shape shark shawl shed sheer sheet shelter sheriff shift "
    "shilling shiver shock shoot shopper shore shortage shorten shorts shot "
    "shove shower shrimp shrink shrub shrug shuffle shutter shuttle sidewalk "
    "sigh sight sign signal silence silent silk silver similar singer "
    "single sink sip site situation sixth size sketch ski skill "
    "skilled skip skull skunk slam slap slave sled sleet sleeve "
    "sleigh slide slight slightly slip slipper slit slope sloth slow "
    "slug slush smear smith smoke smoky snail snap snatch sneak "
    "sneaker sniff snore snowfall snowy snuggle soak sob social society "
    "soften solar soldier sole solicitor solid solitary solstice solution somehow "
    "somewhere song sonic soothe sorrow sorry soul source southern space "
    "spacecraft spare spark sparkle sparrow sparse special specialist specialize specific "
    "specify speck speculate speed speedboat spelling spider spine spirit spiritual "
    "spit splash spoil sponsor spot sprinkle sprint sprout spy squall "
    "square squid squirrel stab stable stack stadium staff stain stair "
    "stall stallion standard staple stare starfish starve state statement static "
    "stationery statue status steam steamer steel steer stem stereo stern "
    "steward sting stink stitch stockbroker stocking stool store stork stormy "
    "stove straight straighten strain strap strategist strategy stream strength stress "
    "stride strike stripe stroke stroll structure struggle stubborn studio stumble "
    "stump stunning style stylish subject submit subtle suburb subway success "
    "sudden suffer suffering sufficient suggestion suitable suitcase sum summarize summary "
    "summit sunday sunflower sunlight sunny sunrise sunset superb superior supervise "
    "support suppose supreme sure surgeon surgery surplus surprise surround survey "
    "surveyor suspect suspend swamp swan swap sway sweat sweater sweet "
    "swell swimsuit syllabus symbol symbolic sympathize sympathy symptom system tackle "
    "tadpole tailor talent tall tame tangible tank tanker tap target "
    "task tasty tavern tax taxi tease technical technician technique technology "
    "teller temper temporary tempt temptation tenant tend tendon tension tenth "
    "term terminate termite terrace terror thaw theater theoretical theory therapist "
    "therefore thesis thief thigh thistle thorn thorough thousand threat threaten "
    "thrill thrive throat throne throughout thumb thunder thursday ticket tide "
    "tidy tight tighten tights tile tiller tilt timber tip title "
    "toad toe toilet tolerate toll ton tone tongue tonne tool "
    "topic torch tornado tortoise total tote toucan tough tour tow "
    "toxic toy track tractor trader tradition traditional traffic tragedy tragic "
    "trail trailer train trainer training tram tranquil transfer transform transit "
    "translate translator transmit transparent trap traveler tray treasurer treatment tremble "
    "trench trend trial tribe trick tricky tricycle trim trolley trooper "
    "tropical tropics trot trouble trousers trout truly trunk truth tub "
    "tuck tuesday tug tulip tumble tumor tuna tundra tune tunnel "
    "turkey turtle tutor tuxedo twelfth twig typical typist umpire unable "
    "unaware undergo underline undershirt understanding undertake undo uneasy uneven unfair "
    "unfold unhappy uniform unify union unit united unity universal universe "
    "university unknown unlawful unless unlock unpack unusual update upgrade uphold "
    "upper upset urban urgent usher usual usually utter vacant valid "
    "valuable van vanish vapor variety various vary vase vault vehicle "
    "veil vein vent venue veranda verbal verse version versus vertical "
    "vessel vest veteran via vicar vicious victim victory view villa "
    "vine vineyard vinyl violence violet viper virtue virus vision vivid "
    "vocabulary vocal volcano volume voluntary vomit vulgar vulture wage wagon "
    "waist waiter wallet wallpaper walrus wander warden wardrobe warehouse warmth "
    "warrior wasp wasteful watchman waterfall weaken weakness wealth weapon wear "
    "weasel weather weave weaver wednesday weekend weep weight welder west "
    "western wetland whale wharf whether whip wholesaler widen widow width "
    "willing willow wind windshield windy wing wink wire wisdom wish "
    "withdraw within without witness wizard wolf wolverine wombat wonder wooden "
    "woodland woodpecker wool woolen workbook worker works workshop worse worst "
    "worth worthy wound wreath wreck wrestle wrestler wring wrist writer "
    "writing yacht yak yard yield youth youthful zebra zip zipper "
    "zone zoo zoom apple appetizer apricot artichoke asparagus avocado bacon "
    "bagel bake banana barbecue basil batter bean beef beet berry "
    "beverage biscuit boil bowl bread breakfast broccoli broth brownie bun "
    "burger butter cabbage cafe cake candy caramel carrot casserole cauliflower "
    "celery cereal cheddar cheese cherry chicken chili chip chocolate cider "
    "cinnamon cocoa coconut coffee cookie cracker cranberry cream crepe crumb "
    "crust cucumber cuisine cupcake curry custard dessert diet dish dough "
    "doughnut drink dumpling eggplant egg feast fig flavor flour fork "
    "fries fruit garlic garnish ginger glass grape grapefruit gravy grill "
    "grocery ham hamburger herb honey hungry icing ingredient jelly juice "
    "kale ketchup kitchen kiwi knife ladle lasagna leek lemon lemonade "
    "lentil lettuce lime loaf lunch macaroni mango margarine marmalade mayonnaise "
    "meat melon menu milk milkshake mint muffin mushroom mustard noodle "
    "nut nutmeg oatmeal oil omelet onion orange pancake papaya paprika "
    "parsley pasta pastry peach peanut pear peas pepper peppermint pickle "
    "pie pineapple pita pizza plate plum popcorn pork porridge potato "
    "pot poultry pretzel prune pudding pumpkin quiche radish raisin ramen "
    "raspberry recipe rice roll salad salami salsa salt sandwich sauce "
    "sausage savor scallop seasoning serve sherbet simmer skillet slice snack "
    "soda soup sour spaghetti spatula spice spicy spinach spoon steak "
    "stew stir strawberry ";

static const char *PATH_VSET = OS_FOLDER "/VOICE/settings.txt";
static bool g_strict_vocab = false;
static bool g_vocab_extended = false;
// The board's microphone path is not usable in software, so "voice input" means
// the note is spoken into a phone, transcribed there, and the pair is copied to
// the SD card. This switch makes that the only way a note gets written here.
static bool g_voice_input = false;
static char g_pending_wav[VOICE_NAME_LEN];
static uint16_t g_pending_sec = 0;
static bool g_voice_capture = false;
static char g_last_unknown[8][20];
static int g_last_unknown_hits[8];
static int g_last_unknown_n = 0;

static void vocab_settings_load() {
  if (!g_sd_ok) return;
  File f = SD_MMC.open(PATH_VSET);
  if (!f) return;
  char line[64];
  while (f.available()) {
    size_t n = f.readBytesUntil('\n', line, sizeof(line) - 1);
    line[n] = 0;
    if (!line[0] || line[0] == '#') continue;
    if (strncmp(line, "strict=", 7) == 0) g_strict_vocab = atoi(line + 7) != 0;
    if (strncmp(line, "words=", 6) == 0) g_vocab_extended = atoi(line + 6) != 0;
    if (strncmp(line, "input=", 6) == 0) g_voice_input = atoi(line + 6) != 0;
  }
  f.close();
}

static void vocab_settings_save() {
  if (!g_sd_ok) return;
  char text[64];
  snprintf(text, sizeof(text), "strict=%d\nwords=%d\ninput=%d\n",
           g_strict_vocab ? 1 : 0, g_vocab_extended ? 1 : 0,
           g_voice_input ? 1 : 0);
  atomic_write(PATH_VSET, String(text));
}

static int vocab_count_in(const char *list) {
  int n = 0;
  bool in_word = false;
  for (const char *p = list; *p; p++) {
    if (*p == ' ') {
      in_word = false;
    } else if (!in_word) {
      in_word = true;
      n++;
    }
  }
  return n;
}

static bool vocab_list_has(const char *list, const char *w) {
  size_t n = strlen(w);
  const char *p = list;
  while (*p) {
    while (*p == ' ') p++;
    const char *s = p;
    while (*p && *p != ' ') p++;
    if ((size_t)(p - s) == n && strncmp(s, w, n) == 0) return true;
  }
  return false;
}

static int vocab_size() {
  return vocab_count_in(VOCAB_DEFAULT) +
         (g_vocab_extended ? vocab_count_in(VOCAB_EXTRA) : 0);
}

static bool vocab_has(const char *w) {
  if (!w || !*w) return false;
  if (vocab_list_has(VOCAB_DEFAULT, w)) return true;
  return g_vocab_extended && vocab_list_has(VOCAB_EXTRA, w);
}

// A base word list is useless for speech without its inflections: "walked",
// "books" and "quickly" have to count as known, or half of ordinary speech is
// flagged. Stems are only used for the lookup, never for the emitted text.
static bool vocab_stem_has(const char *w) {
  int n = (int)strlen(w);
  const char *ends[6] = {"s", "es", "ed", "d", "ing", "ly"};
  for (int i = 0; i < 6; i++) {
    int sl = (int)strlen(ends[i]);
    int bl = n - sl;
    if (bl < 3) continue;
    if (strcmp(w + bl, ends[i]) != 0) continue;
    char base[24];
    if (bl > (int)sizeof(base) - 1) bl = sizeof(base) - 1;
    memcpy(base, w, bl);
    base[bl] = 0;
    if (vocab_has(base)) return true;
    if (bl > 4 && base[bl - 1] == base[bl - 2]) {  // stopped -> stopp -> stop
      base[bl - 1] = 0;
      if (vocab_has(base)) return true;
    }
  }
  return false;
}

static int strip_word(const char *in, char *out, int max_len) {
  int n = 0;
  while (*in && n < max_len - 1) {
    char c = lower_v(*in++);
    if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) {
      out[n++] = c;
    } else if (n > 0) {
      break;
    }
  }
  out[n] = 0;
  return n;
}

static void note_unknown(const char *w) {
  for (int i = 0; i < g_last_unknown_n; i++)
    if (strcmp(g_last_unknown[i], w) == 0) {
      g_last_unknown_hits[i]++;
      return;
    }
  if (g_last_unknown_n >= 8) return;
  snprintf(g_last_unknown[g_last_unknown_n], sizeof(g_last_unknown[0]), "%s", w);
  g_last_unknown_hits[g_last_unknown_n] = 1;
  g_last_unknown_n++;
}

// Words outside the list are counted always and replaced with **** only when
// strict mode is on, so a transcript is never silently rewritten.
static void vocab_apply(const char *in, char *out, int max_len, int *unknown) {
  int o = 0;
  *unknown = 0;
  g_last_unknown_n = 0;
  bool first = true;
  const char *p = in;
  char token[24], word[24];
  while (*p) {
    while (*p == ' ') p++;
    if (!*p) break;
    int n = 0;
    while (*p && *p != ' ' && n < (int)sizeof(token) - 1) token[n++] = *p++;
    while (*p && *p != ' ') p++;  // drop the rest of an overlong token
    token[n] = 0;
    if (strip_word(token, word, sizeof(word)) <= 0) continue;
    bool known = vocab_has(word) || vocab_stem_has(word);
    if (!known) {
      (*unknown)++;
      note_unknown(word);
    }
    const char *emit = (!g_strict_vocab) ? token : (known ? word : "****");
    int el = (int)strlen(emit);
    if (!first) {
      if (o >= max_len - 1) break;
      out[o++] = ' ';
    }
    if (o + el >= max_len - 1) el = max_len - 1 - o;
    if (el <= 0) break;
    memcpy(out + o, emit, el);
    o += el;
    first = false;
  }
  out[o] = 0;
}

static bool has_wav_ext(const char *name) {
  int len = (int)strlen(name);
  if (len < 5) return false;
  const char *e = name + len - 4;
  return e[0] == '.' && lower_v(e[1]) == 'w' && lower_v(e[2]) == 'a' &&
         lower_v(e[3]) == 'v';
}

static void scan_wavs() {
  g_wav_count = 0;
  if (!g_sd_ok) return;
  if (!SD_MMC.exists(DIR_VOICE)) SD_MMC.mkdir(DIR_VOICE);
  File dir = SD_MMC.open(DIR_VOICE);
  if (!dir || !dir.isDirectory()) return;
  File e = dir.openNextFile();
  while (e) {
    if (!e.isDirectory()) {
      const char *n = e.name();
      const char *slash = strrchr(n, '/');
      const char *base = slash ? slash + 1 : n;
      if (has_wav_ext(base) && strlen(base) < VOICE_NAME_LEN &&
          !voice_file_in_use(base) && g_wav_count < 16)
        snprintf(g_wav_names[g_wav_count++], sizeof(g_wav_names[0]), "%s", base);
    }
    e = dir.openNextFile();
  }
}

static bool i2c_present(uint8_t addr) {
  Wire.beginTransmission(addr);
  return Wire.endTransmission() == 0;
}

static int backup_now() {
  if (!g_sd_ok) return 0;
  uint32_t stamp = (uint32_t)time(nullptr);
  char folder[40];
  if (stamp < (uint32_t)TIME_VALID_MIN)
    snprintf(folder, sizeof(folder), "t%lu", (unsigned long)millis());
  else
    snprintf(folder, sizeof(folder), "%lu", (unsigned long)stamp);
  String dir = String(OS_FOLDER) + "/BACKUPS/" + folder;
  if (!SD_MMC.exists(dir.c_str())) SD_MMC.mkdir(dir.c_str());
  int copied = 0;
  for (int i = 0; i < 5; i++) {
    if (!SD_MMC.exists(STORE_PATHS[i])) continue;
    String content;
    if (!read_all(STORE_PATHS[i], content)) continue;
    const char *slash = strrchr(STORE_PATHS[i], '/');
    String dest = dir + "/" + (slash ? slash + 1 : STORE_PATHS[i]);
    if (atomic_write(dest.c_str(), content)) copied++;
  }
  return copied;
}

static int export_now() {
  if (!g_sd_ok) return 0;
  uint32_t stamp = (uint32_t)time(nullptr);
  char name[48], when[24];
  if (stamp < (uint32_t)TIME_VALID_MIN)
    snprintf(name, sizeof(name), "t%lu", (unsigned long)millis());
  else
    snprintf(name, sizeof(name), "%lu", (unsigned long)stamp);
  fmt_dateonly(when, sizeof(when), stamp);

  String out = "# " OS_MARK " export\n";
  out += "# generated ";
  out += when;
  out += "\n\n## CASES\n";
  for (int i = 0; i < g_case_count; i++) {
    CaseRec &c = CASES[i];
    out += "- [" + String(c.id) + "] " + c.title + "  (" +
           CASE_STATUS_NAMES[c.status] + ", " +
           String(record_count_in_case(c.id)) + " records)\n";
    if (c.summary[0]) out += "    " + String(c.summary) + "\n";
  }
  out += "\n## RECORDS\n";
  for (int i = 0; i < g_record_count; i++) {
    Record &r = RECORDS[i];
    char when2[24];
    fmt_dateonly(when2, sizeof(when2), r.created);
    out += "- [" + String(r.id) + "] " + TYPE_NAMES[r.type] + " / " +
           status_name(r.type, r.status) + " / " + case_title(r.case_id) +
           "\n    " + r.title + "\n";
    if (r.body[0]) out += "    " + String(r.body) + "\n";
    if (r.tags[0]) out += "    tags: " + String(r.tags) + "\n";
    if (r.source[0]) out += "    source: " + String(r.source) + "\n";
    int vr = voice_for_record(r.id);
    if (vr >= 0)
      out += "    voice: " + String(g_voice_refs[vr].file) + " (" +
             String(g_voice_refs[vr].sec) + "s)\n";
    out += "    captured " + String(when2) + "\n";
  }
  out += "\n## THREADS\n";
  for (int i = 0; i < g_link_count; i++) {
    Thread &t = LINKS[i];
    int a = find_record_index(t.from_id), b = find_record_index(t.to_id);
    out += "- " + String(a >= 0 ? RECORDS[a].title : "?") + " " +
           relation_name(t.relation) + " " +
           String(b >= 0 ? RECORDS[b].title : "?") + "\n";
  }
  out += "\n## CARDS\n";
  for (int i = 0; i < g_card_count; i++)
    out += "- " + String(CARDS[i].front) + " => " + CARDS[i].back + "\n";
  out += "\n## DECISIONS\n";
  for (int i = 0; i < g_decision_count; i++) {
    Decision &d = DECISIONS[i];
    out += "- " + String(d.title) + " (confidence " + String(d.confidence) +
           "/5)\n";
    if (d.rationale[0]) out += "    why: " + String(d.rationale) + "\n";
    if (d.outcome[0]) out += "    outcome: " + String(d.outcome) + "\n";
  }
  String path = String(OS_FOLDER) + "/EXPORTS/export_" + name + ".txt";
  if (!atomic_write(path.c_str(), out)) return 0;
  return (int)out.length();
}

static int restore_latest() {
  if (!g_sd_ok) return 0;
  uint32_t id = latest_backup_id();
  if (id == 0) return 0;
  String dir = String(OS_FOLDER) + "/BACKUPS/" + String(id);
  int restored = 0;
  for (int i = 0; i < 5; i++) {
    const char *slash = strrchr(STORE_PATHS[i], '/');
    String src = dir + "/" + (slash ? slash + 1 : STORE_PATHS[i]);
    String content;
    if (!read_all(src.c_str(), content)) continue;
    if (content.indexOf("# Mentalist") != 0) continue;
    if (atomic_write(STORE_PATHS[i], content)) restored++;
  }
  if (restored) load_all_stores();
  return restored;
}

// ---------------------------------------------------------------------------
//  Filters (shared by the record lists)
// ---------------------------------------------------------------------------
static int g_filter_type   = -1;
static int g_filter_status = -1;
static uint32_t g_filter_case = 0;
static char g_filter_title[32] = "All notes";

static void set_filter(int type, int status, uint32_t case_id, const char *title) {
  g_filter_type   = type;
  g_filter_status = status;
  g_filter_case   = case_id;
  snprintf(g_filter_title, sizeof(g_filter_title), "%s", title ? title : "Notes");
}

// ---------------------------------------------------------------------------
//  Navigation state
// ---------------------------------------------------------------------------
enum View {
  V_HOME = 0,
  V_AREA,
  V_CAPTURE,
  V_RECORDS,
  V_DETAIL,
  V_CASES,
  V_CASE,
  V_TIMELINE,
  V_WALL,
  V_NODE,
  V_LINKPICK,
  V_LINKREL,
  V_QUESTIONS,
  V_QDETAIL,
  V_SEARCH,
  V_CARDS,
  V_CARDEDIT,
  V_CARDSTUDY,
  V_DECISIONS,
  V_DDETAIL,
  V_FOCUS,
  V_SYSTEM,
  V_INSIGHTS,
  V_TAGS,
  V_RESURFACE,
  V_VOICE,
  V_VIMPORT,
  V_VOCAB,
  V_PLANNED,
  V_CLOCK
};

enum Action {
  ACT_CAPTURE_OBS = 0,
  ACT_CASES,
  ACT_TIMELINE,
  ACT_WALL,
  ACT_QUESTIONS,
  ACT_CLUES,
  ACT_SEARCH,
  ACT_CARDS,
  ACT_BLIND,
  ACT_ROULETTE,
  ACT_VAULT,
  ACT_READING,
  ACT_WORDS,
  ACT_IDEAS,
  ACT_PAUSE,
  ACT_PERSPECTIVE,
  ACT_DECISIONS,
  ACT_FOCUS,
  ACT_SYSTEM,
  ACT_INSIGHTS,
  ACT_TAGS,
  ACT_RESURFACE,
  ACT_VOICE,
  ACT_DAILY,
  ACT_LOGIC,
  ACT_JANE
};

enum ConfirmAction {
  CA_NONE = 0,
  CA_DEL_RECORD,
  CA_DEL_CASE,
  CA_DEL_LINK,
  CA_DEL_CARD,
  CA_DEL_DECISION,
  CA_ARCHIVE_CASE,
  CA_RESTORE
};

enum EditField {
  EF_NONE = 0,
  EF_CAP_TITLE,
  EF_CAP_BODY,
  EF_CAP_TAGS,
  EF_CAP_SOURCE,
  EF_REC_TITLE,
  EF_REC_BODY,
  EF_REC_TAGS,
  EF_REC_SOURCE,
  EF_REC_DATE,
  EF_CASE_TITLE,
  EF_CASE_SUMMARY,
  EF_CASE_NEW_TITLE,
  EF_SEARCH,
  EF_CARD_FRONT,
  EF_CARD_BACK,
  EF_CARD_TAGS,
  EF_DEC_TITLE,
  EF_DEC_OPTIONS,
  EF_DEC_RATIONALE,
  EF_DEC_EXPECTED,
  EF_DEC_OUTCOME,
  EF_DEC_LESSON
};

static int g_view = V_HOME;
static int g_param = 0;
static int g_view_stack[12];
static int g_view_depth = 0;

static lv_obj_t *g_cur_screen = nullptr;
static lv_obj_t *g_clock_label = nullptr;
static lv_obj_t *g_power_label = nullptr;
static lv_obj_t *g_focus_label = nullptr;
static lv_obj_t *g_face_time = nullptr;
static lv_obj_t *g_face_date = nullptr;
static lv_obj_t *g_face_bat = nullptr;
// What each clock label last painted. On an AMOLED a label that is set to the
// same text still invalidates and still costs a panel refresh, so the launcher's
// rule is followed: a tick that changes nothing draws nothing.
static char g_face_last_hm[10] = "";
static char g_face_last_dt[32] = "";
static char g_face_last_bat[16] = "";
static char g_clock_last[10] = "";
static bool g_face_24h = true;      // follows the launcher's NVS "dcw"/"t24"
static uint32_t g_dark_since = 0;   // 0 = the panel is lit
static lv_obj_t *g_toast = nullptr;

static int g_area = 0;
static int g_sel = -1;        // record index
static int g_sel_case = -1;   // case index
static int g_card_sel = -1;   // card index
static int g_dec_sel = -1;    // decision index
static int g_card_mode = 0;   // 0 review, 1 blind, 2 roulette
static int g_link_from = -1;  // record index of the thread's first node
static int g_link_pick = -1;  // record index of the second node
static int g_link_rel = REL_SUPPORTS;
static uint32_t g_wall_case = 0;
static int g_timeline_desc = 1;
static char g_search_query[36] = "";

static int g_results[MAX_RECORDS];
static int g_result_count = 0;

// Forward declarations for the pieces that live further down the file.
static lv_obj_t *build_home();
static lv_obj_t *build_area();
static lv_obj_t *build_capture();
static lv_obj_t *build_records();
static lv_obj_t *build_detail();
static lv_obj_t *build_cases();
static lv_obj_t *build_case();
static lv_obj_t *build_timeline();
static lv_obj_t *build_wall();
static lv_obj_t *build_node();
static lv_obj_t *build_linkpick();
static lv_obj_t *build_linkrel();
static lv_obj_t *build_questions();
static lv_obj_t *build_qdetail();
static lv_obj_t *build_search();
static lv_obj_t *build_cards();
static lv_obj_t *build_cardedit();
static lv_obj_t *build_cardstudy();
static lv_obj_t *build_decisions();
static lv_obj_t *build_ddecision();
static lv_obj_t *build_focus();
static lv_obj_t *build_system();
static lv_obj_t *build_insights();
static lv_obj_t *build_tags();
static lv_obj_t *build_resurface();
static lv_obj_t *build_voice();
static lv_obj_t *build_vimport();
static lv_obj_t *build_vocab();
static void open_tag_view(const char *tag);
static lv_obj_t *build_planned();

static void render();
static void open_view(int v, int param);
static void go_back();
static void run_action(int action);
static void run_confirm();
static void refresh_runtime_labels();
static void capture_save();
static void capture_clear();
static void capture_load_draft();
static void capture_store_draft();
static void open_editor(const char *title, const char *initial, int maxLen,
                        int fid, bool numeric);
static void on_field_committed(int fid);
static void toast(const char *msg);
static void confirm_ask(const char *msg, int action, uint32_t ref);
static void set_filter(int type, int status, uint32_t case_id, const char *title);
static void set_filter_mask(uint16_t mask, const char *title);
static void return_to_launcher();
static void deep_sleep_now();

// ---------------------------------------------------------------------------
//  LVGL plumbing
// ---------------------------------------------------------------------------
static lv_disp_draw_buf_t g_draw_buf;
static lv_color_t *g_disp_buf = nullptr;
static lv_disp_drv_t g_disp_drv;
static lv_indev_drv_t g_indev_drv;

static void ui_reset_refs() {
  g_clock_label = nullptr;
  g_power_label = nullptr;
  g_focus_label = nullptr;
  g_face_time = nullptr;
  g_face_date = nullptr;
  g_face_bat = nullptr;
}

static lv_obj_t *scr_new() {
  ui_reset_refs();
  lv_obj_t *s = lv_obj_create(nullptr);
  lv_obj_set_style_bg_color(s, lv_color_hex(COL_BG), 0);
  lv_obj_set_style_bg_opa(s, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(s, 0, 0);
  lv_obj_set_style_pad_all(s, 0, 0);
  lv_obj_clear_flag(s, LV_OBJ_FLAG_SCROLLABLE);
  return s;
}

static lv_obj_t *label(lv_obj_t *parent, const char *text,
                       const lv_font_t *font, uint32_t color,
                       lv_align_t align, int dx, int dy) {
  lv_obj_t *l = lv_label_create(parent);
  lv_label_set_text(l, text ? text : "");
  lv_obj_set_style_text_font(l, font, 0);
  lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
  lv_obj_align(l, align, dx, dy);
  return l;
}

static void style_panel(lv_obj_t *o) {
  lv_obj_set_style_bg_color(o, lv_color_hex(COL_SURF), 0);
  lv_obj_set_style_bg_grad_color(o, lv_color_hex(COL_SURF_LO), 0);
  lv_obj_set_style_bg_grad_dir(o, LV_GRAD_DIR_VER, 0);
  lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
  lv_obj_set_style_radius(o, 18, 0);
  lv_obj_set_style_border_width(o, 1, 0);
  lv_obj_set_style_border_color(o, lv_color_hex(COL_LINE), 0);
  lv_obj_set_style_border_opa(o, LV_OPA_60, 0);
  lv_obj_set_style_shadow_width(o, 0, 0);
  lv_obj_set_style_pad_all(o, 0, 0);
}

static void style_btn(lv_obj_t *b, uint32_t border) {
  style_panel(b);
  lv_obj_set_style_border_color(b, lv_color_hex(border), 0);
  lv_obj_set_style_border_opa(b, LV_OPA_COVER, 0);
  lv_obj_set_style_bg_grad_color(b, lv_color_hex(COL_SURF), 0);
  lv_obj_set_style_bg_color(b, lv_color_hex(COL_SURF_HI), LV_STATE_PRESSED);
  lv_obj_set_style_bg_color(b, lv_color_hex(COL_SURF_HI), LV_STATE_FOCUSED);
}

static lv_obj_t *header(lv_obj_t *screen, const char *title, bool withBack) {
  lv_obj_t *bar = lv_obj_create(screen);
  lv_obj_set_pos(bar, 0, 0);
  lv_obj_set_size(bar, LCD_W, 64);
  lv_obj_set_style_bg_opa(bar, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(bar, 0, 0);
  lv_obj_set_style_pad_all(bar, 0, 0);
  lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);

  if (withBack) {
    lv_obj_t *b = lv_btn_create(bar);
    lv_obj_set_pos(b, 12, 15);
    lv_obj_set_size(b, 44, 34);
    style_btn(b, COL_LINE);
    label(b, LV_SYMBOL_LEFT, &lv_font_montserrat_16, COL_CREAM, LV_ALIGN_CENTER,
          0, 0);
    lv_obj_add_event_cb(b, [](lv_event_t *) { go_back(); }, LV_EVENT_CLICKED,
                        nullptr);
    label(bar, title, &lv_font_montserrat_18, COL_CREAM, LV_ALIGN_TOP_LEFT, 68,
          18);
  } else {
    label(bar, title, &lv_font_montserrat_18, COL_CREAM, LV_ALIGN_TOP_LEFT, 18,
          18);
  }

  g_clock_label = label(bar, "--:--", &lv_font_montserrat_18, COL_CREAM,
                        LV_ALIGN_TOP_RIGHT, -14, 14);
  lv_obj_t *rule = lv_obj_create(bar);
  lv_obj_set_pos(rule, 16, 60);
  lv_obj_set_size(rule, LCD_W - 32, 1);
  lv_obj_set_style_bg_color(rule, lv_color_hex(COL_LINE), 0);
  lv_obj_set_style_bg_opa(rule, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(rule, 0, 0);
  lv_obj_set_style_radius(rule, 0, 0);
  lv_obj_t *tip = lv_obj_create(bar);
  lv_obj_set_pos(tip, 16, 59);
  lv_obj_set_size(tip, 44, 3);
  lv_obj_set_style_bg_color(tip, lv_color_hex(COL_AMBER), 0);
  lv_obj_set_style_bg_opa(tip, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(tip, 0, 0);
  lv_obj_set_style_radius(tip, 0, 0);
  refresh_runtime_labels();
  return bar;
}

static lv_obj_t *body(lv_obj_t *screen, int y, int h) {
  lv_obj_t *o = lv_obj_create(screen);
  lv_obj_set_pos(o, 12, y);
  lv_obj_set_size(o, LCD_W - 24, h);
  lv_obj_set_style_bg_opa(o, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(o, 0, 0);
  lv_obj_set_style_pad_all(o, 4, 0);
  lv_obj_set_style_pad_row(o, 10, 0);
  lv_obj_set_flex_flow(o, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_scroll_dir(o, LV_DIR_VER);
  lv_obj_set_scrollbar_mode(o, LV_SCROLLBAR_MODE_AUTO);
  return o;
}

static lv_obj_t *flex_row(lv_obj_t *parent, int h) {
  lv_obj_t *r = lv_obj_create(parent);
  lv_obj_set_width(r, LCD_W - 36);
  lv_obj_set_height(r, h);
  lv_obj_set_style_bg_opa(r, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(r, 0, 0);
  lv_obj_set_style_pad_all(r, 0, 0);
  lv_obj_set_style_pad_column(r, 8, 0);
  lv_obj_set_flex_flow(r, LV_FLEX_FLOW_ROW);
  lv_obj_clear_flag(r, LV_OBJ_FLAG_SCROLLABLE);
  return r;
}

static lv_obj_t *field(lv_obj_t *parent, const char *caption, const char *value,
                       int w, int h, lv_event_cb_t cb, void *arg) {
  lv_obj_t *b = lv_btn_create(parent);
  lv_obj_set_width(b, w);
  lv_obj_set_height(b, h);
  style_btn(b, COL_LINE);
  label(b, caption, &lv_font_montserrat_12, COL_AMBER, LV_ALIGN_TOP_LEFT, 12, 7);
  lv_obj_t *v = label(b, (value && value[0]) ? value : "tap to type",
                      &lv_font_montserrat_14,
                      (value && value[0]) ? COL_CREAM : COL_MUTED,
                      LV_ALIGN_TOP_LEFT, 12, 25);
  lv_obj_set_width(v, w - 24);
  lv_label_set_long_mode(v, LV_LABEL_LONG_DOT);
  lv_obj_set_height(v, 34);
  if (cb) lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, arg);
  return b;
}

static lv_obj_t *action_button(lv_obj_t *parent, const char *text, int w, int h,
                               uint32_t border, lv_event_cb_t cb, void *arg) {
  lv_obj_t *b = lv_btn_create(parent);
  lv_obj_set_width(b, w);
  lv_obj_set_height(b, h);
  style_btn(b, border);
  label(b, text, &lv_font_montserrat_14, COL_CREAM, LV_ALIGN_CENTER, 0, 0);
  if (cb) lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, arg);
  return b;
}

static lv_obj_t *list_row(lv_obj_t *parent, const char *title,
                          const char *meta, uint32_t accent,
                          lv_event_cb_t cb, void *arg) {
  lv_obj_t *b = lv_btn_create(parent);
  lv_obj_set_width(b, LCD_W - 36);
  lv_obj_set_height(b, 74);
  style_btn(b, COL_LINE);
  lv_obj_t *edge = lv_obj_create(b);
  lv_obj_set_pos(edge, 0, 17);
  lv_obj_set_size(edge, 3, 40);
  lv_obj_set_style_bg_color(edge, lv_color_hex(accent), 0);
  lv_obj_set_style_bg_opa(edge, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(edge, 0, 0);
  lv_obj_set_style_radius(edge, 2, 0);
  lv_obj_t *t = label(b, title, &lv_font_montserrat_16, COL_CREAM,
                      LV_ALIGN_TOP_LEFT, 16, 12);
  lv_obj_set_width(t, LCD_W - 96);
  lv_label_set_long_mode(t, LV_LABEL_LONG_DOT);
  label(b, meta, &lv_font_montserrat_12, COL_MUTED, LV_ALIGN_TOP_LEFT, 16, 38);
  label(b, LV_SYMBOL_RIGHT, &lv_font_montserrat_14, COL_AMBER_DK,
        LV_ALIGN_RIGHT_MID, -14, 0);
  if (cb) lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, arg);
  return b;
}

static lv_obj_t *empty_note(lv_obj_t *parent, const char *text) {
  lv_obj_t *box = lv_obj_create(parent);
  lv_obj_set_width(box, LCD_W - 36);
  lv_obj_set_height(box, LV_SIZE_CONTENT);
  lv_obj_set_style_bg_opa(box, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(box, 1, 0);
  lv_obj_set_style_border_color(box, lv_color_hex(COL_LINE), 0);
  lv_obj_set_style_border_opa(box, LV_OPA_50, 0);
  lv_obj_set_style_radius(box, 14, 0);
  lv_obj_set_style_pad_all(box, 14, 0);
  lv_obj_clear_flag(box, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_t *l = label(box, text, &lv_font_montserrat_12, COL_MUTED,
                      LV_ALIGN_TOP_LEFT, 0, 0);
  lv_obj_set_width(l, LCD_W - 66);
  lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
  return box;
}

static void section_label(lv_obj_t *parent, const char *text, int y) {
  label(parent, text, &lv_font_montserrat_12, COL_AMBER, LV_ALIGN_TOP_LEFT, 17,
        y);
}

// ---------------------------------------------------------------------------
//  Toast
// ---------------------------------------------------------------------------
static void toast_kill(lv_timer_t *t) {
  if (g_toast) {
    lv_obj_del_async(g_toast);
    g_toast = nullptr;
  }
  lv_timer_del(t);
}

static void toast(const char *msg) {
  if (g_toast) {
    lv_obj_del_async(g_toast);
    g_toast = nullptr;
  }
  g_toast = lv_obj_create(lv_layer_top());
  lv_obj_set_size(g_toast, 330, 52);
  lv_obj_align(g_toast, LV_ALIGN_BOTTOM_MID, 0, -22);
  lv_obj_set_style_bg_color(g_toast, lv_color_hex(0x000000), 0);
  lv_obj_set_style_bg_opa(g_toast, LV_OPA_80, 0);
  lv_obj_set_style_radius(g_toast, 14, 0);
  lv_obj_set_style_border_width(g_toast, 1, 0);
  lv_obj_set_style_border_color(g_toast, lv_color_hex(COL_AMBER), 0);
  lv_obj_clear_flag(g_toast, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_t *l = lv_label_create(g_toast);
  lv_label_set_text(l, msg);
  lv_obj_set_style_text_font(l, &lv_font_montserrat_14, 0);
  lv_obj_set_style_text_color(l, lv_color_hex(COL_CREAM), 0);
  lv_obj_set_width(l, 300);
  lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
  lv_obj_center(l);
  lv_timer_create(toast_kill, 1900, nullptr);
}

// ---------------------------------------------------------------------------
//  Confirm dialog
// ---------------------------------------------------------------------------
static lv_obj_t *g_confirm = nullptr;
static int g_confirm_action = CA_NONE;
static uint32_t g_confirm_ref = 0;

static void confirm_close() {
  if (g_confirm) {
    lv_obj_del_async(g_confirm);
    g_confirm = nullptr;
  }
  g_confirm_action = CA_NONE;
}

static void confirm_yes_cb(lv_event_t *) {
  int action = g_confirm_action;
  uint32_t ref = g_confirm_ref;
  g_confirm_action = action;
  confirm_close();
  g_confirm_action = action;
  g_confirm_ref = ref;
  run_confirm();
}

static void confirm_ask(const char *msg, int action, uint32_t ref) {
  if (g_confirm) confirm_close();
  g_confirm_action = action;
  g_confirm_ref = ref;

  g_confirm = lv_obj_create(lv_layer_top());
  lv_obj_set_size(g_confirm, LCD_W, LCD_H);
  lv_obj_set_pos(g_confirm, 0, 0);
  lv_obj_set_style_bg_color(g_confirm, lv_color_hex(0x000000), 0);
  lv_obj_set_style_bg_opa(g_confirm, LV_OPA_70, 0);
  lv_obj_set_style_border_width(g_confirm, 0, 0);
  lv_obj_set_style_radius(g_confirm, 0, 0);
  lv_obj_clear_flag(g_confirm, LV_OBJ_FLAG_SCROLLABLE);

  lv_obj_t *panel = lv_obj_create(g_confirm);
  lv_obj_set_size(panel, 340, 226);
  lv_obj_align(panel, LV_ALIGN_CENTER, 0, 0);
  style_panel(panel);

  lv_obj_t *l = label(panel, msg, &lv_font_montserrat_14, COL_CREAM,
                      LV_ALIGN_TOP_LEFT, 16, 16);
  lv_obj_set_width(l, 308);
  lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);

  lv_obj_t *no = lv_btn_create(panel);
  lv_obj_set_pos(no, 16, 162);
  lv_obj_set_size(no, 142, 46);
  style_btn(no, COL_LINE);
  label(no, "CANCEL", &lv_font_montserrat_14, COL_CREAM, LV_ALIGN_CENTER, 0, 0);
  lv_obj_add_event_cb(no, [](lv_event_t *) { confirm_close(); },
                      LV_EVENT_CLICKED, nullptr);

  lv_obj_t *yes = lv_btn_create(panel);
  lv_obj_set_pos(yes, 182, 162);
  lv_obj_set_size(yes, 142, 46);
  style_btn(yes, COL_THREAD);
  label(yes, "CONFIRM", &lv_font_montserrat_14, COL_CREAM, LV_ALIGN_CENTER, 0, 0);
  lv_obj_add_event_cb(yes, confirm_yes_cb, LV_EVENT_CLICKED, nullptr);
}

// ---------------------------------------------------------------------------
//  Keyboard editor (one overlay, reused)
// ---------------------------------------------------------------------------
static lv_obj_t *g_editor = nullptr;
static lv_obj_t *g_editor_ta = nullptr;
static lv_obj_t *g_editor_title = nullptr;
static char g_edit_scratch[220];
static int g_edit_field = EF_NONE;
static bool g_edit_numeric = false;

static void editor_close() {
  if (g_editor) {
    lv_obj_del_async(g_editor);
    g_editor = nullptr;
  }
  g_editor_ta = nullptr;
  g_editor_title = nullptr;
  g_edit_field = EF_NONE;
}

static void editor_kb_cb(lv_event_t *e) {
  lv_event_code_t code = lv_event_get_code(e);
  if (code == LV_EVENT_READY) {
    const char *txt = lv_textarea_get_text(g_editor_ta);
    snprintf(g_edit_scratch, sizeof(g_edit_scratch), "%s", txt ? txt : "");
    if (!g_edit_numeric) sanitize_input(g_edit_scratch);
    int fid = g_edit_field;
    editor_close();
    on_field_committed(fid);
  } else if (code == LV_EVENT_CANCEL) {
    editor_close();
  }
}

static void open_editor(const char *title, const char *initial, int maxLen,
                        int fid, bool numeric) {
  if (g_editor) editor_close();
  g_edit_field = fid;
  g_edit_numeric = numeric;

  g_editor = lv_obj_create(lv_layer_top());
  lv_obj_set_size(g_editor, LCD_W, LCD_H);
  lv_obj_set_pos(g_editor, 0, 0);
  lv_obj_set_style_bg_color(g_editor, lv_color_hex(COL_BG), 0);
  lv_obj_set_style_bg_opa(g_editor, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(g_editor, 0, 0);
  lv_obj_set_style_radius(g_editor, 0, 0);
  lv_obj_clear_flag(g_editor, LV_OBJ_FLAG_SCROLLABLE);

  g_editor_title = label(g_editor, title, &lv_font_montserrat_16, COL_CREAM,
                         LV_ALIGN_TOP_LEFT, 16, 14);

  lv_obj_t *cancel = lv_btn_create(g_editor);
  lv_obj_set_pos(cancel, LCD_W - 96, 10);
  lv_obj_set_size(cancel, 84, 34);
  style_btn(cancel, COL_LINE);
  label(cancel, "CANCEL", &lv_font_montserrat_12, COL_CREAM, LV_ALIGN_CENTER, 0,
        0);
  lv_obj_add_event_cb(cancel, [](lv_event_t *) { editor_close(); },
                      LV_EVENT_CLICKED, nullptr);

  g_editor_ta = lv_textarea_create(g_editor);
  lv_obj_set_pos(g_editor_ta, 14, 56);
  lv_obj_set_size(g_editor_ta, LCD_W - 28, 84);
  lv_textarea_set_one_line(g_editor_ta, true);
  lv_textarea_set_max_length(g_editor_ta, maxLen > 0 ? maxLen : 1);
  lv_textarea_set_placeholder_text(g_editor_ta, "type here");
  if (initial && initial[0]) lv_textarea_set_text(g_editor_ta, initial);
  lv_obj_set_style_bg_color(g_editor_ta, lv_color_hex(COL_SURF), 0);
  lv_obj_set_style_text_color(g_editor_ta, lv_color_hex(COL_CREAM), 0);
  lv_obj_set_style_text_font(g_editor_ta, &lv_font_montserrat_16, 0);
  lv_obj_set_style_border_color(g_editor_ta, lv_color_hex(COL_AMBER), 0);
  lv_obj_set_style_border_width(g_editor_ta, 1, 0);
  lv_obj_set_style_radius(g_editor_ta, 12, 0);

  lv_obj_t *hint = label(g_editor,
                         numeric ? "YYYY-MM-DD HH:MM" : "SAVE = keyboard check",
                         &lv_font_montserrat_12, COL_MUTED, LV_ALIGN_TOP_LEFT,
                         16, 145);
  (void)hint;

  lv_obj_t *kb = lv_keyboard_create(g_editor);
  lv_obj_set_size(kb, LCD_W, LCD_H - 168);
  lv_obj_align(kb, LV_ALIGN_BOTTOM_MID, 0, 0);
  lv_keyboard_set_textarea(kb, g_editor_ta);
  lv_keyboard_set_mode(kb, LV_KEYBOARD_MODE_TEXT_LOWER);
  lv_obj_set_style_bg_color(kb, lv_color_hex(COL_SURF), 0);
  lv_obj_set_style_border_width(kb, 0, 0);
  lv_obj_set_style_bg_color(kb, lv_color_hex(COL_AMBER), LV_PART_ITEMS);
  lv_obj_set_style_text_color(kb, lv_color_hex(COL_BG), LV_PART_ITEMS);
  lv_obj_set_style_bg_opa(kb, LV_OPA_COVER, LV_PART_ITEMS);
  lv_obj_set_style_radius(kb, 8, LV_PART_ITEMS);
  lv_obj_set_style_bg_color(kb, lv_color_hex(COL_BG), LV_PART_MAIN);
  lv_obj_set_style_text_color(kb, lv_color_hex(COL_CREAM), LV_PART_MAIN);
  lv_obj_add_event_cb(kb, editor_kb_cb, LV_EVENT_ALL, nullptr);
}

// ---------------------------------------------------------------------------
//  Field commit routing
// ---------------------------------------------------------------------------
static void on_field_committed(int fid) {
  uint32_t now = (uint32_t)time(nullptr);

  switch (fid) {
    case EF_CAP_TITLE:
      copy_field(g_draft.title, sizeof(g_draft.title), g_edit_scratch);
      g_draft.dirty = true;
      capture_store_draft();
      break;
    case EF_CAP_BODY:
      copy_field(g_draft.body, sizeof(g_draft.body), g_edit_scratch);
      g_draft.dirty = true;
      capture_store_draft();
      break;
    case EF_CAP_TAGS:
      copy_field(g_draft.tags, sizeof(g_draft.tags), g_edit_scratch);
      g_draft.dirty = true;
      capture_store_draft();
      break;
    case EF_CAP_SOURCE:
      copy_field(g_draft.source, sizeof(g_draft.source), g_edit_scratch);
      g_draft.dirty = true;
      capture_store_draft();
      break;

    case EF_REC_TITLE:
      if (g_sel >= 0 && g_sel < g_record_count) {
        copy_field(RECORDS[g_sel].title, sizeof(RECORDS[g_sel].title),
                   g_edit_scratch);
        RECORDS[g_sel].updated = now;
        save_records();
      }
      break;
    case EF_REC_BODY:
      if (g_sel >= 0 && g_sel < g_record_count) {
        copy_field(RECORDS[g_sel].body, sizeof(RECORDS[g_sel].body),
                   g_edit_scratch);
        RECORDS[g_sel].updated = now;
        save_records();
      }
      break;
    case EF_REC_TAGS:
      if (g_sel >= 0 && g_sel < g_record_count) {
        copy_field(RECORDS[g_sel].tags, sizeof(RECORDS[g_sel].tags),
                   g_edit_scratch);
        RECORDS[g_sel].updated = now;
        save_records();
      }
      break;
    case EF_REC_SOURCE:
      if (g_sel >= 0 && g_sel < g_record_count) {
        copy_field(RECORDS[g_sel].source, sizeof(RECORDS[g_sel].source),
                   g_edit_scratch);
        RECORDS[g_sel].updated = now;
        save_records();
      }
      break;
    case EF_REC_DATE:
      if (g_sel >= 0 && g_sel < g_record_count) {
        uint32_t ts = parse_datetime(g_edit_scratch);
        if (ts == 0) {
          toast("USE YYYY-MM-DD HH:MM");
        } else {
          RECORDS[g_sel].event_at = ts;
          RECORDS[g_sel].updated = now;
          save_records();
        }
      }
      break;

    case EF_CASE_TITLE:
      if (g_sel_case >= 0 && g_sel_case < g_case_count) {
        copy_field(CASES[g_sel_case].title, sizeof(CASES[g_sel_case].title),
                   g_edit_scratch);
        CASES[g_sel_case].updated = now;
        save_cases();
      }
      break;
    case EF_CASE_SUMMARY:
      if (g_sel_case >= 0 && g_sel_case < g_case_count) {
        copy_field(CASES[g_sel_case].summary, sizeof(CASES[g_sel_case].summary),
                   g_edit_scratch);
        CASES[g_sel_case].updated = now;
        save_cases();
      }
      break;
    case EF_CASE_NEW_TITLE: {
      int idx = create_case(g_edit_scratch, "");
      if (idx < 0) {
        toast("CASE SAVE FAILED");
      } else {
        g_sel_case = idx;
        open_view(V_CASE, 0);
        return;
      }
      break;
    }

    case EF_SEARCH:
      copy_field(g_search_query, sizeof(g_search_query), g_edit_scratch);
      break;

    case EF_CARD_FRONT:
      if (g_card_sel >= 0 && g_card_sel < g_card_count) {
        copy_field(CARDS[g_card_sel].front, sizeof(CARDS[g_card_sel].front),
                   g_edit_scratch);
        save_cards();
      }
      break;
    case EF_CARD_BACK:
      if (g_card_sel >= 0 && g_card_sel < g_card_count) {
        copy_field(CARDS[g_card_sel].back, sizeof(CARDS[g_card_sel].back),
                   g_edit_scratch);
        save_cards();
      }
      break;
    case EF_CARD_TAGS:
      if (g_card_sel >= 0 && g_card_sel < g_card_count) {
        copy_field(CARDS[g_card_sel].tags, sizeof(CARDS[g_card_sel].tags),
                   g_edit_scratch);
        save_cards();
      }
      break;

    case EF_DEC_TITLE:
      if (g_dec_sel >= 0 && g_dec_sel < g_decision_count) {
        copy_field(DECISIONS[g_dec_sel].title,
                   sizeof(DECISIONS[g_dec_sel].title), g_edit_scratch);
        save_decisions();
      }
      break;
    case EF_DEC_OPTIONS:
      if (g_dec_sel >= 0 && g_dec_sel < g_decision_count) {
        copy_field(DECISIONS[g_dec_sel].options,
                   sizeof(DECISIONS[g_dec_sel].options), g_edit_scratch);
        save_decisions();
      }
      break;
    case EF_DEC_RATIONALE:
      if (g_dec_sel >= 0 && g_dec_sel < g_decision_count) {
        copy_field(DECISIONS[g_dec_sel].rationale,
                   sizeof(DECISIONS[g_dec_sel].rationale), g_edit_scratch);
        save_decisions();
      }
      break;
    case EF_DEC_EXPECTED:
      if (g_dec_sel >= 0 && g_dec_sel < g_decision_count) {
        copy_field(DECISIONS[g_dec_sel].expected,
                   sizeof(DECISIONS[g_dec_sel].expected), g_edit_scratch);
        save_decisions();
      }
      break;
    case EF_DEC_OUTCOME:
      if (g_dec_sel >= 0 && g_dec_sel < g_decision_count) {
        copy_field(DECISIONS[g_dec_sel].outcome,
                   sizeof(DECISIONS[g_dec_sel].outcome), g_edit_scratch);
        save_decisions();
      }
      break;
    case EF_DEC_LESSON:
      if (g_dec_sel >= 0 && g_dec_sel < g_decision_count) {
        copy_field(DECISIONS[g_dec_sel].lesson,
                   sizeof(DECISIONS[g_dec_sel].lesson), g_edit_scratch);
        save_decisions();
      }
      break;

    default:
      break;
  }
  render();
}

// ---------------------------------------------------------------------------
//  Draft persistence
// ---------------------------------------------------------------------------
static void capture_store_draft() {
  if (!g_sd_ok) return;
  if (!g_draft.dirty) {
    SD_MMC.remove(OS_FOLDER "/CACHE/draft.tv");
    return;
  }
  char line[420];
  snprintf(line, sizeof(line), "%u|%u|%lu|%s|%s|%s|%s|%s|%u\n",
           (unsigned)g_draft.type, (unsigned)g_draft.status,
           (unsigned long)g_draft.case_id, g_draft.title, g_draft.body,
           g_draft.tags, g_draft.source, g_pending_wav,
           (unsigned)g_pending_sec);
  atomic_write(OS_FOLDER "/CACHE/draft.tv", line);
}

static void capture_load_draft() {
  memset(&g_draft, 0, sizeof(g_draft));
  g_draft.type = TYPE_OBSERVATION;
  g_draft.status = 0;
  if (!g_sd_ok) return;
  File f = SD_MMC.open(OS_FOLDER "/CACHE/draft.tv", FILE_READ);
  if (!f) return;
  char line[440];
  size_t n = f.readBytesUntil('\n', line, sizeof(line) - 1);
  line[n] = 0;
  f.close();
  if (!line[0]) return;
  char *fl[10];
  int nf = split_fields(line, fl, 10);
  if (nf < 7) return;
  g_draft.type = (uint8_t)atoi(fl[0]);
  if (g_draft.type >= TYPE_COUNT) g_draft.type = TYPE_OBSERVATION;
  g_draft.status = (uint8_t)atoi(fl[1]);
  g_draft.case_id = strtoul(fl[2], nullptr, 10);
  copy_field(g_draft.title, sizeof(g_draft.title), fl[3]);
  copy_field(g_draft.body, sizeof(g_draft.body), fl[4]);
  copy_field(g_draft.tags, sizeof(g_draft.tags), fl[5]);
  copy_field(g_draft.source, sizeof(g_draft.source), fl[6]);
  if (nf >= 9) {  // a voice draft keeps its unclaimed audio across a reboot
    copy_field(g_pending_wav, sizeof(g_pending_wav), fl[7]);
    g_pending_sec = (uint16_t)atoi(fl[8]);
    if (!g_pending_wav[0]) g_pending_sec = 0;
  }
  g_draft.dirty = (g_draft.title[0] || g_draft.body[0] || g_draft.tags[0] ||
                   g_draft.source[0]);
}

static void capture_clear() {
  memset(&g_draft, 0, sizeof(g_draft));
  g_draft.type = (uint8_t)g_capture_type;
  g_draft.status = 0;
  g_draft.dirty = false;
  g_pending_wav[0] = 0;
  g_pending_sec = 0;
  capture_store_draft();
}

static void capture_begin(int type) {
  g_capture_type = type;
  if (!g_draft.dirty) {
    memset(&g_draft, 0, sizeof(g_draft));
    g_draft.type = (uint8_t)type;
    g_draft.status = 0;
  }
  g_draft.type = (uint8_t)type;
}

// ---------------------------------------------------------------------------
//  Capture screen
// ---------------------------------------------------------------------------
static const char *capture_hint(int type) {
  switch (type) {
    case TYPE_PAUSE:
      return "What do I know? What am I assuming? What else explains it? "
             "What would change my mind?";
    case TYPE_PERSPECTIVE:
      return "Name the other side. What would they say is true, and what "
             "evidence would they accept?";
    case TYPE_QUESTION:
      return "A question is a gap, not a claim. Record it before you fill it.";
    case TYPE_SOURCE:
      return "Where did this come from, and how close is that to the event?";
    case TYPE_READING:
      return "Page or chapter in SOURCE. Keep the idea, not the whole book.";
    case TYPE_WORD:
      return "WORD in the title, meaning in the note, example in TAGS.";
    default:
      return "Capture the observation before the interpretation.";
  }
}

static void cap_cycle_type(lv_event_t *) {
  int t = g_draft.type + 1;
  if (t >= TYPE_COUNT) t = 0;
  g_draft.type = (uint8_t)t;
  g_capture_type = t;
  g_draft.dirty = true;
  capture_store_draft();
  render();
}

static void cap_cycle_status(lv_event_t *) {
  int slots = status_slots(g_draft.type);
  int s = (int)g_draft.status + 1;
  if (s >= slots) s = 0;
  g_draft.status = (uint8_t)s;
  g_draft.dirty = true;
  capture_store_draft();
  render();
}

static void cap_cycle_case(lv_event_t *) {
  int idx = find_case_index(g_draft.case_id);
  idx++;
  if (idx >= g_case_count) idx = -1;
  g_draft.case_id = (idx < 0) ? 0 : CASES[idx].id;
  g_draft.dirty = true;
  capture_store_draft();
  render();
}

static void cap_edit_title(lv_event_t *) {
  open_editor("TITLE", g_draft.title, TITLE_LEN - 1, EF_CAP_TITLE, false);
}
static void cap_edit_body(lv_event_t *) {
  open_editor("NOTE", g_draft.body, BODY_LEN - 1, EF_CAP_BODY, false);
}
static void cap_edit_tags(lv_event_t *) {
  open_editor("TAGS", g_draft.tags, TAG_LEN - 1, EF_CAP_TAGS, false);
}
static void cap_edit_source(lv_event_t *) {
  open_editor("SOURCE", g_draft.source, SRC_LEN - 1, EF_CAP_SOURCE, false);
}
static void cap_save_cb(lv_event_t *) { capture_save(); }
static void cap_discard_cb(lv_event_t *) {
  capture_clear();
  toast("DRAFT CLEARED");
  render();
}

static void capture_save() {
  if (!g_draft.title[0] && !g_draft.body[0]) {
    toast("NOTHING TO SAVE");
    return;
  }
  char title[TITLE_LEN];
  if (g_draft.title[0]) {
    snprintf(title, sizeof(title), "%s", g_draft.title);
  } else {
    snprintf(title, sizeof(title), "%.40s", g_draft.body);
  }
  int idx = create_record(g_draft.type, title, g_draft.body, g_draft.tags,
                          g_draft.source, g_draft.case_id);
  if (idx < 0) {
    toast(g_store_error[0] ? g_store_error : "SAVE FAILED");
    return;
  }
  RECORDS[idx].status = g_draft.status;
  if (g_pending_wav[0]) {
    voice_attach(RECORDS[idx].id, g_pending_wav, g_pending_sec);
    g_pending_wav[0] = 0;
    g_pending_sec = 0;
  }
  save_records();
  g_draft.dirty = false;
  capture_store_draft();
  memset(&g_draft, 0, sizeof(g_draft));
  g_draft.type = (uint8_t)g_capture_type;
  toast(g_sd_ok ? "SAVED" : "NOT SAVED - SD UNAVAILABLE");
  go_back();
}

static void capture_voice_cb(lv_event_t *) {
  g_voice_capture = true;
  open_view(V_VIMPORT, 0);
}

static void capture_input_cb(lv_event_t *) {
  g_voice_input = !g_voice_input;
  vocab_settings_save();
  toast(g_voice_input ? "VOICE INPUT ONLY" : "KEYBOARD INPUT");
  render();
}

// Voice mode drops the typed fields entirely rather than merely hiding them, so
// there is no half-keyboard state to fall into: the transcript is the note.
static void capture_voice_block(lv_obj_t *b) {
  char head[52];
  if (g_pending_wav[0])
    snprintf(head, sizeof(head), "TRANSCRIPT READY · %us OF AUDIO",
             (unsigned)g_pending_sec);
  else
    snprintf(head, sizeof(head), "NO TRANSCRIPT YET");
  label(b, head, &lv_font_montserrat_12, COL_AMBER, LV_ALIGN_TOP_LEFT, 2, 0);

  if (g_pending_wav[0]) {
    lv_obj_t *t = label(b, g_draft.body, &lv_font_montserrat_14, COL_CREAM,
                        LV_ALIGN_TOP_LEFT, 2, 0);
    lv_obj_set_width(t, LCD_W - 44);
    lv_label_set_long_mode(t, LV_LABEL_LONG_DOT);
    lv_obj_set_height(t, 90);
    char snd[56];
    snprintf(snd, sizeof(snd), "audio %.30s · attaches on save", g_pending_wav);
    label(b, snd, &lv_font_montserrat_12, COL_MUTED, LV_ALIGN_TOP_LEFT, 2, 0);
  } else {
    lv_obj_t *e = label(b,
                        "Speak into a phone, let it transcribe, then copy the WAV "
                        "and its .txt (same name) into /WATCH/Mentalist/VOICE.",
                        &lv_font_montserrat_12, COL_MUTED, LV_ALIGN_TOP_LEFT, 2, 0);
    lv_obj_set_width(e, LCD_W - 44);
    lv_label_set_long_mode(e, LV_LABEL_LONG_WRAP);
  }
  action_button(b,
                g_pending_wav[0] ? "REPLACE FROM VOICE"
                                 : "IMPORT VOICE TRANSCRIPT",
                LCD_W - 36, 50, COL_AMBER, capture_voice_cb, nullptr);
  action_button(b, "INPUT: VOICE ONLY", LCD_W - 36, 40, COL_LINE,
                capture_input_cb, nullptr);
}

static lv_obj_t *build_capture() {
  lv_obj_t *s = scr_new();
  header(s, "CAPTURE", true);
  lv_obj_t *b = body(s, 64, 434);

  lv_obj_t *typerow = flex_row(b, 30);
  char typebuf[64];
  snprintf(typebuf, sizeof(typebuf), "%s  ·  tap to change",
           TYPE_NAMES[g_draft.type]);
  lv_obj_t *l = label(typerow, typebuf, &lv_font_montserrat_12, COL_AMBER,
                      LV_ALIGN_LEFT_MID, 2, 0);
  lv_obj_add_flag(l, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(l, cap_cycle_type, LV_EVENT_CLICKED, nullptr);

  if (g_voice_input) {
    capture_voice_block(b);
  } else {
    field(b, "TITLE", g_draft.title, LCD_W - 36, 56, cap_edit_title, nullptr);
    field(b, "NOTE", g_draft.body, LCD_W - 36, 74, cap_edit_body, nullptr);
    field(b, "TAGS", g_draft.tags, LCD_W - 36, 60, cap_edit_tags, nullptr);
    field(b, "SOURCE", g_draft.source, LCD_W - 36, 60, cap_edit_source, nullptr);
  }

  lv_obj_t *hint = label(b, capture_hint(g_draft.type), &lv_font_montserrat_12,
                         COL_MUTED, LV_ALIGN_TOP_LEFT, 2, 0);
  lv_obj_set_width(hint, LCD_W - 40);
  lv_label_set_long_mode(hint, LV_LABEL_LONG_WRAP);

  lv_obj_t *meta = flex_row(b, 52);
  char statbuf[40];
  snprintf(statbuf, sizeof(statbuf), "%s",
           status_name(g_draft.type, g_draft.status));

  lv_obj_t *sb = lv_btn_create(meta);
  lv_obj_set_size(sb, 170, 52);
  style_btn(sb, COL_AMBER);
  label(sb, "STATUS", &lv_font_montserrat_12, COL_AMBER, LV_ALIGN_TOP_LEFT, 10,
        6);
  label(sb, statbuf, &lv_font_montserrat_14, COL_CREAM, LV_ALIGN_TOP_LEFT, 10,
        24);
  lv_obj_add_event_cb(sb, cap_cycle_status, LV_EVENT_CLICKED, nullptr);

  lv_obj_t *cb = lv_btn_create(meta);
  lv_obj_set_size(cb, 170, 52);
  style_btn(cb, COL_LINE);
  label(cb, "CASE", &lv_font_montserrat_12, COL_AMBER, LV_ALIGN_TOP_LEFT, 10, 6);
  lv_obj_t *cl = label(cb, case_title(g_draft.case_id), &lv_font_montserrat_14,
                       COL_CREAM, LV_ALIGN_TOP_LEFT, 10, 24);
  lv_obj_set_width(cl, 150);
  lv_label_set_long_mode(cl, LV_LABEL_LONG_DOT);
  lv_obj_add_event_cb(cb, cap_cycle_case, LV_EVENT_CLICKED, nullptr);

  action_button(b, "SAVE TO CASE FILE", LCD_W - 36, 54, COL_AMBER, cap_save_cb,
                nullptr);
  action_button(b, "DISCARD", LCD_W - 36, 40, COL_LINE, cap_discard_cb, nullptr);

  if (!g_sd_ok) {
    lv_obj_t *w = label(b, "SD card is not mounted. This note stays in RAM "
                           "only and will be lost.",
                        &lv_font_montserrat_12, COL_THREAD,
                        LV_ALIGN_TOP_LEFT, 2, 0);
    lv_obj_set_width(w, LCD_W - 40);
    lv_label_set_long_mode(w, LV_LABEL_LONG_WRAP);
  }
  return s;
}

// ---------------------------------------------------------------------------
//  Home
// ---------------------------------------------------------------------------
static void area_tile_cb(lv_event_t *e) {
  g_area = (int)(intptr_t)lv_event_get_user_data(e);
  open_view(V_AREA, 0);
}

static lv_obj_t *bottom_button(lv_obj_t *parent, const char *text,
                               uint32_t border, lv_event_cb_t cb) {
  lv_obj_t *b = lv_btn_create(parent);
  lv_obj_set_pos(b, 14, 442);
  lv_obj_set_size(b, LCD_W - 28, 50);
  style_btn(b, border);
  label(b, text, &lv_font_montserrat_14, COL_CREAM, LV_ALIGN_CENTER, 0, 0);
  if (cb) lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, nullptr);
  return b;
}

static lv_obj_t *tile_button(lv_obj_t *parent, const char *title,
                             const char *meta, int x, int y, int w, int h,
                             lv_event_cb_t cb, void *arg, uint32_t border) {
  lv_obj_t *b = lv_btn_create(parent);
  lv_obj_set_pos(b, x, y);
  lv_obj_set_size(b, w, h);
  style_btn(b, border);
  lv_obj_t *t = label(b, title, &lv_font_montserrat_16, COL_CREAM,
                      LV_ALIGN_TOP_LEFT, 12, 12);
  lv_obj_set_width(t, w - 20);
  lv_label_set_long_mode(t, LV_LABEL_LONG_DOT);
  lv_obj_t *m = label(b, meta, &lv_font_montserrat_12, COL_MUTED,
                      LV_ALIGN_TOP_LEFT, 12, 34);
  lv_obj_set_width(m, w - 20);
  lv_label_set_long_mode(m, LV_LABEL_LONG_DOT);
  if (cb) lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, arg);
  return b;
}

static void action_tile_cb(lv_event_t *e) {
  run_action((int)(intptr_t)lv_event_get_user_data(e));
}

static lv_obj_t *home_tile(lv_obj_t *parent, const char *title, const char *meta,
                           int x, int y, int w, int h, int action,
                           uint32_t border) {
  return tile_button(parent, title, meta, x, y, w, h, action_tile_cb,
                     (void *)(intptr_t)action, border);
}

static lv_obj_t *capture_tile(lv_obj_t *parent, const char *title,
                              const char *meta, int x, int y, int w, int h,
                              int action) {
  lv_obj_t *b = tile_button(parent, title, meta, x, y, w, h, action_tile_cb,
                            (void *)(intptr_t)action, COL_AMBER);
  lv_obj_set_style_border_width(b, 2, 0);
  lv_obj_set_style_border_color(b, lv_color_hex(COL_AMBER), 0);
  lv_obj_set_style_border_opa(b, LV_OPA_COVER, 0);
  label(b, LV_SYMBOL_PLUS, &lv_font_montserrat_22, COL_AMBER, LV_ALIGN_RIGHT_MID,
        -18, 0);
  return b;
}

static lv_obj_t *build_home() {
  lv_obj_t *s = scr_new();

  label(s, "THE MENTALIST", &lv_font_montserrat_22, COL_CREAM,
        LV_ALIGN_TOP_LEFT, 16, 12);
  label(s, "OBSERVE  ·  REMEMBER  ·  REASON  ·  LEARN  ·  ASSIST",
        &lv_font_montserrat_12, COL_AMBER, LV_ALIGN_TOP_LEFT, 17, 44);
  g_clock_label = label(s, "--:--", &lv_font_montserrat_28, COL_CREAM,
                        LV_ALIGN_TOP_RIGHT, -16, 10);
  lv_obj_add_flag(g_clock_label, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(g_clock_label, clock_to_face_cb, LV_EVENT_CLICKED,
                      nullptr);
  g_power_label = label(s, "BAT --", &lv_font_montserrat_12, COL_MUTED,
                        LV_ALIGN_TOP_RIGHT, -16, 44);

  lv_obj_t *hr = lv_obj_create(s);
  lv_obj_set_pos(hr, 16, 66);
  lv_obj_set_size(hr, 36, 2);
  lv_obj_set_style_bg_color(hr, lv_color_hex(COL_AMBER), 0);
  lv_obj_set_style_bg_opa(hr, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(hr, 0, 0);
  lv_obj_set_style_radius(hr, 1, 0);

  capture_tile(s, "QUICK CAPTURE", "note · timestamp · case", 14, 78,
               LCD_W - 28, 62, ACT_CAPTURE_OBS);
  section_label(s, "WORKSPACES", 156);

  int areas[5] = {0, 1, 2, 3, 4};
  const char *area_meta[5] = {"cases · timeline · wall", "cards · blind recall",
                             "pause · perspectives", "vault · reading · words",
                             "focus · decisions · system"};
  const int area_x[5] = {14, 210, 14, 210, 14};
  const int area_y[5] = {172, 172, 250, 250, 328};
  for (int i = 0; i < 5; i++) {
    tile_button(s, area_name(areas[i]), area_meta[i], area_x[i], area_y[i],
                186, 74, area_tile_cb, (void *)(intptr_t)areas[i], COL_LINE);
  }
  home_tile(s, "DECISIONS", "journal · confidence", 210, 328, 186, 74,
            ACT_DECISIONS, COL_LINE);

  lv_obj_t *strip = lv_obj_create(s);
  lv_obj_set_pos(strip, 14, 404);
  lv_obj_set_size(strip, LCD_W - 28, 82);
  style_panel(strip);
  lv_obj_clear_flag(strip, LV_OBJ_FLAG_SCROLLABLE);

  int open_q = 0;
  for (int i = 0; i < g_record_count; i++) {
    if (RECORDS[i].type != TYPE_QUESTION) continue;
    const char *sn = status_name(RECORDS[i].type, RECORDS[i].status);
    if (strcmp(sn, "Answered") != 0 && strcmp(sn, "Parked") != 0) open_q++;
  }
  char stats[96];
  snprintf(stats, sizeof(stats), "%d notes · %d due · %d open Q",
           g_record_count, card_due_count(), open_q);
  label(strip, g_sd_ok ? "SD  ready" : "SD  unavailable", &lv_font_montserrat_12,
        g_sd_ok ? COL_AMBER : COL_THREAD, LV_ALIGN_TOP_LEFT, 14, 10);
  label(strip, stats, &lv_font_montserrat_16, COL_CREAM, LV_ALIGN_TOP_LEFT, 14,
        28);
  label(strip, "hold BOOT 1.5s to return to LauncherOS",
        &lv_font_montserrat_12, COL_MUTED, LV_ALIGN_TOP_LEFT, 14, 58);

  {
    int due = card_due_count();
    lv_obj_t *qb = lv_btn_create(strip);
    lv_obj_set_size(qb, 104, 38);
    lv_obj_align(qb, LV_ALIGN_TOP_RIGHT, -12, 22);
    style_btn(qb, due > 0 ? COL_AMBER : COL_LINE);
    char qt[24];
    snprintf(qt, sizeof(qt), due > 0 ? "REVIEW %d" : "REVIEW", due);
    label(qb, qt, &lv_font_montserrat_12, COL_CREAM, LV_ALIGN_CENTER, 0, 0);
    lv_obj_add_event_cb(qb, card_review_due_cb, LV_EVENT_CLICKED, nullptr);
  }

  if (g_draft.dirty) {
    lv_obj_t *d = label(s, "unsaved draft", &lv_font_montserrat_12, COL_THREAD,
                        LV_ALIGN_TOP_LEFT, 250, 44);
    lv_obj_add_flag(d, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(d, [](lv_event_t *) { open_view(V_CAPTURE, g_draft.type); },
                        LV_EVENT_CLICKED, nullptr);
  }
  return s;
}

// ---------------------------------------------------------------------------
//  Areas
// ---------------------------------------------------------------------------
static const AreaItem AREA_OBSERVE[] = {
    {"Quick capture", "note, timestamp, case", ACT_CAPTURE_OBS},
    {"Case files", "group evidence into cases", ACT_CASES},
    {"Timeline", "everything in time order", ACT_TIMELINE},
    {"Evidence wall", "nodes and red threads", ACT_WALL},
    {"Question board", "open, investigating, answered", ACT_QUESTIONS},
    {"Tags", "every label in use", ACT_TAGS},
    {"Voice notes", "audio as evidence", ACT_VOICE},
    {"Clue tracker", "status across all evidence", ACT_CLUES},
    {"Search everything", "titles, notes, tags, cases", ACT_SEARCH}};

static const AreaItem AREA_REMEMBER[] = {
    {"Knowledge cards", "front and back, stored on SD", ACT_CARDS},
    {"Blind recall", "hide the answer, then compare", ACT_BLIND},
    {"Recall roulette", "random card from the deck", ACT_ROULETTE},
    {"Knowledge vault", "facts, concepts, sources", ACT_VAULT},
    {"Daily challenge", "built-in offline pool — phase 4", ACT_DAILY}};

static const AreaItem AREA_REASON[] = {
    {"The pause", "four questions before judging", ACT_PAUSE},
    {"Three perspectives", "consider other explanations", ACT_PERSPECTIVE},
    {"Decision journal", "rationale, confidence, outcome", ACT_DECISIONS},
    {"Logic lab", "offline puzzles — phase 5", ACT_LOGIC}};

static const AreaItem AREA_LEARN[] = {
    {"Knowledge vault", "facts, concepts, sources", ACT_VAULT},
    {"Reading journal", "books, sections, key ideas", ACT_READING},
    {"Personal dictionary", "word, definition, example", ACT_WORDS},
    {"Idea collector", "one-tap ideas and hunches", ACT_IDEAS}};

static const AreaItem AREA_ASSIST[] = {
    {"Focus timer", "quiet countdown for deep work", ACT_FOCUS},
    {"Decision journal", "decisions with expected outcomes", ACT_DECISIONS},
    {"Search everything", "one query across the store", ACT_SEARCH},
    {"Insights", "balance, counts, activity", ACT_INSIGHTS},
    {"System and backup", "storage, export, launcher", ACT_SYSTEM},
    {"Jane assistant", "external processing — not in V1", ACT_JANE}};

#define AREA_LEN(t) ((int)(sizeof(t) / sizeof(t[0])))

static const char *area_name(int a) {
  switch (a) {
    case 0: return "OBSERVE";
    case 1: return "REMEMBER";
    case 2: return "REASON";
    case 3: return "LEARN";
    default: return "ASSIST";
  }
}

// Accessors, not a pointer-to-struct return: .ino auto-prototyping must never
// meet a sketch-defined type in a function signature.
static int area_item_count(int a) {
  switch (a) {
    case 0: return AREA_LEN(AREA_OBSERVE);
    case 1: return AREA_LEN(AREA_REMEMBER);
    case 2: return AREA_LEN(AREA_REASON);
    case 3: return AREA_LEN(AREA_LEARN);
    default: return AREA_LEN(AREA_ASSIST);
  }
}

static const AreaItem *area_table(int a) {
  switch (a) {
    case 0: return AREA_OBSERVE;
    case 1: return AREA_REMEMBER;
    case 2: return AREA_REASON;
    case 3: return AREA_LEARN;
    default: return AREA_ASSIST;
  }
}

static const char *area_item_title(int a, int i) {
  if (i < 0 || i >= area_item_count(a)) return "";
  return area_table(a)[i].title;
}

static const char *area_item_meta(int a, int i) {
  if (i < 0 || i >= area_item_count(a)) return "";
  return area_table(a)[i].meta;
}

static int area_item_action(int a, int i) {
  if (i < 0 || i >= area_item_count(a)) return ACT_CAPTURE_OBS;
  return area_table(a)[i].action;
}

static lv_obj_t *build_area() {
  if (g_area < 0 || g_area > 4) g_area = 0;
  lv_obj_t *s = scr_new();
  header(s, area_name(g_area), true);
  lv_obj_t *b = body(s, 64, 428);

  int count = area_item_count(g_area);
  for (int i = 0; i < count; i++) {
    list_row(b, area_item_title(g_area, i), area_item_meta(g_area, i),
             COL_AMBER, action_tile_cb,
             (void *)(intptr_t)area_item_action(g_area, i));
  }
  return s;
}

// ---------------------------------------------------------------------------
//  Record lists
// ---------------------------------------------------------------------------
static void sort_results_by_updated() {
  for (int i = 1; i < g_result_count; i++) {
    int key = g_results[i];
    uint32_t key_ts = RECORDS[key].updated;
    int j = i - 1;
    while (j >= 0 && RECORDS[g_results[j]].updated < key_ts) {
      g_results[j + 1] = g_results[j];
      j--;
    }
    g_results[j + 1] = key;
  }
}

static uint16_t g_filter_mask = 0;
// A tag filter is a second, independent axis: it survives navigation the way a
// type filter does, but any ordinary view change drops it again.
static char g_tag_filter[28] = "";
static bool g_tag_keep = false;

static char lower_a(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c; }

static bool same_tag(const char *a, const char *b) {
  while (*a && *b) {
    if (lower_a(*a) != lower_a(*b)) return false;
    a++;
    b++;
  }
  return *a == 0 && *b == 0;
}

static bool tag_list_has(const char *list, const char *want) {
  if (!list || !*list || !want || !*want) return false;
  while (*list) {
    while (*list == ' ' || *list == ',' || *list == ';') list++;
    const char *start = list;
    while (*list && *list != ',' && *list != ';') list++;
    const char *end = list;
    while (end > start && end[-1] == ' ') end--;
    int len = (int)(end - start);
    if (len <= 0) continue;
    char token[28];
    if (len > 27) len = 27;
    memcpy(token, start, len);
    token[len] = 0;
    if (same_tag(token, want)) return true;
  }
  return false;
}

static void set_filter_mask(uint16_t mask, const char *title) {
  g_filter_mask = mask;
  g_filter_type = -1;
  g_filter_status = -1;
  g_filter_case = 0;
  snprintf(g_filter_title, sizeof(g_filter_title), "%s", title ? title : "Notes");
}

static bool record_matches_filters(int i) {
  if (g_filter_mask && !(g_filter_mask & (1u << RECORDS[i].type))) return false;
  if (g_filter_type >= 0 && RECORDS[i].type != g_filter_type) return false;
  if (g_filter_status >= 0 && RECORDS[i].status != g_filter_status) return false;
  if (g_filter_case != 0 && RECORDS[i].case_id != g_filter_case) return false;
  return true;
}

static void collect_filtered() {
  g_result_count = 0;
  for (int i = 0; i < g_record_count; i++) {
    if (!record_matches_filters(i)) continue;
    if (g_tag_filter[0] && !tag_list_has(RECORDS[i].tags, g_tag_filter)) continue;
    g_results[g_result_count++] = i;
  }
  sort_results_by_updated();
}

static void record_row_cb(lv_event_t *e) {
  g_sel = (int)(intptr_t)lv_event_get_user_data(e);
  open_view(V_DETAIL, 0);
}

// Evidence that has sat unverified for a month is the thing most likely to be
// quietly wrong, so lists say so instead of looking as fresh as today's notes.
static bool record_is_stale(int idx, uint32_t now) {
  if (idx < 0 || idx >= g_record_count) return false;
  if (RECORDS[idx].status != ST_UNVERIFIED) return false;
  uint32_t t = RECORDS[idx].updated ? RECORDS[idx].updated : RECORDS[idx].created;
  if (t < (uint32_t)TIME_VALID_MIN) return false;
  return (int32_t)(now - t) >= 30L * 86400L;
}

static lv_obj_t *build_records() {
  lv_obj_t *s = scr_new();
  header(s, g_filter_title, true);
  lv_obj_t *b = body(s, 64, 376);
  collect_filtered();

  char meta[112];
  uint32_t now_ts = (uint32_t)time(nullptr);
  for (int i = 0; i < g_result_count; i++) {
    int idx = g_results[i];
    char when[24];
    bool stale = record_is_stale(idx, now_ts);
    fmt_datetime(when, sizeof(when),
                 RECORDS[idx].event_at ? RECORDS[idx].event_at
                                       : RECORDS[idx].created);
    int vref = voice_for_record(RECORDS[idx].id);
    char vbuf[24];
    if (vref >= 0)
      snprintf(vbuf, sizeof(vbuf), " · voice %us", g_voice_refs[vref].sec);
    else
      vbuf[0] = 0;
    snprintf(meta, sizeof(meta), "%s · %s · %s%s%s",
             TYPE_SHORT[RECORDS[idx].type],
             status_name(RECORDS[idx].type, RECORDS[idx].status), when,
             stale ? " · stale" : "", vbuf);
    uint32_t accent = (RECORDS[idx].status == ST_CONTRADICTED)
                          ? COL_THREAD
                          : (RECORDS[idx].status == ST_CORROBORATED ? COL_AMBER
                                                                    : COL_LINE);
    list_row(b, RECORDS[idx].title, meta, accent, record_row_cb,
             (void *)(intptr_t)idx);
  }
  if (g_result_count == 0)
    empty_note(b, "No records here yet. Use Quick capture to add one.");
  return s;
}

// ---------------------------------------------------------------------------
//  Record detail
// ---------------------------------------------------------------------------
static void detail_cycle_status(lv_event_t *) {
  if (g_sel < 0 || g_sel >= g_record_count) return;
  int slots = status_slots(RECORDS[g_sel].type);
  int s = (int)RECORDS[g_sel].status + 1;
  if (s >= slots) s = 0;
  RECORDS[g_sel].status = (uint8_t)s;
  RECORDS[g_sel].updated = (uint32_t)time(nullptr);
  save_records();
  render();
}

static void detail_edit_title(lv_event_t *) {
  if (g_sel < 0 || g_sel >= g_record_count) return;
  open_editor("TITLE", RECORDS[g_sel].title, TITLE_LEN - 1, EF_REC_TITLE, false);
}
static void detail_edit_body(lv_event_t *) {
  if (g_sel < 0 || g_sel >= g_record_count) return;
  open_editor("NOTE", RECORDS[g_sel].body, BODY_LEN - 1, EF_REC_BODY, false);
}
static void detail_edit_tags(lv_event_t *) {
  if (g_sel < 0 || g_sel >= g_record_count) return;
  open_editor("TAGS", RECORDS[g_sel].tags, TAG_LEN - 1, EF_REC_TAGS, false);
}
static void detail_edit_source(lv_event_t *) {
  if (g_sel < 0 || g_sel >= g_record_count) return;
  open_editor("SOURCE", RECORDS[g_sel].source, SRC_LEN - 1, EF_REC_SOURCE,
              false);
}
static void detail_edit_date(lv_event_t *) {
  if (g_sel < 0 || g_sel >= g_record_count) return;
  char buf[24];
  fmt_dateonly(buf, sizeof(buf), RECORDS[g_sel].event_at);
  if (buf[0] == '-' && buf[1] == '-') snprintf(buf, sizeof(buf), "");
  open_editor("EVENT DATE", buf, 19, EF_REC_DATE, true);
}
static void detail_delete_cb(lv_event_t *) {
  if (g_sel < 0 || g_sel >= g_record_count) return;
  confirm_ask("Delete this record and every red thread touching it?",
              CA_DEL_RECORD, RECORDS[g_sel].id);
}
static void detail_link_cb(lv_event_t *) {
  if (g_sel < 0 || g_sel >= g_record_count) return;
  g_link_from = g_sel;
  open_view(V_LINKPICK, 0);
}

static void node_open_cb(lv_event_t *e) {
  int idx = (int)(intptr_t)lv_event_get_user_data(e);
  if (idx < 0 || idx >= g_record_count) return;
  g_sel = idx;
  open_view(V_NODE, 0);
}

static void thread_delete_cb(lv_event_t *e) {
  int idx = (int)(intptr_t)lv_event_get_user_data(e);
  if (idx < 0 || idx >= g_link_count) return;
  confirm_ask("Remove this red thread? The records stay.", CA_DEL_LINK,
              LINKS[idx].id);
}

static lv_obj_t *build_detail() {
  if (g_sel < 0 || g_sel >= g_record_count) return build_records();
  Record &r = RECORDS[g_sel];
  lv_obj_t *s = scr_new();
  header(s, TYPE_NAMES[r.type], true);
  lv_obj_t *b = body(s, 64, 428);

  char meta[96];
  char when[24];
  fmt_datetime(when, sizeof(when), r.created);
  snprintf(meta, sizeof(meta), "%s · %s · %s", status_name(r.type, r.status),
           case_title(r.case_id), when);
  lv_obj_t *m = label(b, meta, &lv_font_montserrat_12, COL_AMBER,
                      LV_ALIGN_TOP_LEFT, 2, 0);
  lv_obj_set_width(m, LCD_W - 44);
  lv_label_set_long_mode(m, LV_LABEL_LONG_WRAP);

  field(b, "TITLE", r.title, LCD_W - 36, 56, detail_edit_title, nullptr);
  field(b, "NOTE", r.body, LCD_W - 36, 74, detail_edit_body, nullptr);
  field(b, "TAGS", r.tags, LCD_W - 36, 56, detail_edit_tags, nullptr);
  field(b, "SOURCE", r.source, LCD_W - 36, 56, detail_edit_source, nullptr);

  int vref = voice_for_record(r.id);
  if (vref >= 0) {
    lv_obj_t *vp = lv_obj_create(b);
    lv_obj_set_width(vp, LCD_W - 36);
    lv_obj_set_height(vp, 46);
    style_panel(vp);
    lv_obj_clear_flag(vp, LV_OBJ_FLAG_SCROLLABLE);
    char vt[80];
    snprintf(vt, sizeof(vt), "VOICE  %us  ·  %.20s", g_voice_refs[vref].sec,
             g_voice_refs[vref].file);
    label(vp, vt, &lv_font_montserrat_12, COL_AMBER, LV_ALIGN_LEFT_MID, 14, 0);
  }

  lv_obj_t *row = flex_row(b, 52);
  lv_obj_t *st = lv_btn_create(row);
  lv_obj_set_size(st, 170, 52);
  style_btn(st, COL_AMBER);
  label(st, "STATUS", &lv_font_montserrat_12, COL_AMBER, LV_ALIGN_TOP_LEFT, 10, 6);
  label(st, status_name(r.type, r.status), &lv_font_montserrat_14, COL_CREAM,
        LV_ALIGN_TOP_LEFT, 10, 24);
  lv_obj_add_event_cb(st, detail_cycle_status, LV_EVENT_CLICKED, nullptr);

  lv_obj_t *dt = lv_btn_create(row);
  lv_obj_set_size(dt, 170, 52);
  style_btn(dt, COL_LINE);
  label(dt, "EVENT DATE", &lv_font_montserrat_12, COL_AMBER, LV_ALIGN_TOP_LEFT,
        10, 6);
  char evt[24];
  fmt_datetime(evt, sizeof(evt), r.event_at ? r.event_at : r.created);
  label(dt, evt, &lv_font_montserrat_14, COL_CREAM, LV_ALIGN_TOP_LEFT, 10, 24);
  lv_obj_add_event_cb(dt, detail_edit_date, LV_EVENT_CLICKED, nullptr);

  action_button(b, "CONNECT WITH A RED THREAD", LCD_W - 36, 50, COL_THREAD,
                detail_link_cb, nullptr);

  lv_obj_t *lh = label(b, "RED THREADS", &lv_font_montserrat_12, COL_AMBER,
                       LV_ALIGN_TOP_LEFT, 2, 0);
  (void)lh;

  int shown = 0;
  for (int i = 0; i < g_link_count; i++) {
    if (LINKS[i].from_id != r.id && LINKS[i].to_id != r.id) continue;
    uint32_t other_id =
        (LINKS[i].from_id == r.id) ? LINKS[i].to_id : LINKS[i].from_id;
    int other = find_record_index(other_id);
    const char *other_title = (other >= 0) ? RECORDS[other].title : "missing";
    char line[120];
    snprintf(line, sizeof(line), "%s: %s", relation_name(LINKS[i].relation),
             other_title);
    lv_obj_t *lr = lv_obj_create(b);
    lv_obj_set_width(lr, LCD_W - 36);
    lv_obj_set_height(lr, 44);
    style_panel(lr);
    lv_obj_clear_flag(lr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(lr, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(lr, node_open_cb, LV_EVENT_CLICKED,
                        (void *)(intptr_t)other);
    lv_obj_t *l = label(lr, line, &lv_font_montserrat_12, COL_CREAM,
                        LV_ALIGN_LEFT_MID, 12, 0);
    lv_obj_set_width(l, LCD_W - 130);
    lv_label_set_long_mode(l, LV_LABEL_LONG_DOT);
    lv_obj_t *del = lv_btn_create(lr);
    lv_obj_set_size(del, 44, 34);
    lv_obj_align(del, LV_ALIGN_RIGHT_MID, -6, 0);
    style_btn(del, COL_THREAD);
    label(del, "X", &lv_font_montserrat_14, COL_THREAD, LV_ALIGN_CENTER, 0, 0);
    lv_obj_add_event_cb(del, thread_delete_cb, LV_EVENT_CLICKED,
                        (void *)(intptr_t)i);
    shown++;
  }
  if (shown == 0)
    empty_note(b, "No threads yet. A thread is a claim about how two records "
                  "relate.");

  action_button(b, "DELETE RECORD", LCD_W - 36, 46, COL_THREAD,
                detail_delete_cb, nullptr);
  return s;
}

// ---------------------------------------------------------------------------
//  Cases
// ---------------------------------------------------------------------------
static void case_row_cb(lv_event_t *e) {
  int idx = (int)(intptr_t)lv_event_get_user_data(e);
  if (idx < 0 || idx >= g_case_count) return;
  g_sel_case = idx;
  open_view(V_CASE, 0);
}

static void new_case_cb(lv_event_t *) {
  open_editor("NEW CASE TITLE", "", TITLE_LEN - 1, EF_CASE_NEW_TITLE, false);
}

static lv_obj_t *build_cases() {
  lv_obj_t *s = scr_new();
  header(s, "CASE FILES", true);
  lv_obj_t *b = body(s, 64, 372);

  char meta[64];
  for (int i = 0; i < g_case_count; i++) {
    snprintf(meta, sizeof(meta), "%s · %d records", CASE_STATUS_NAMES[CASES[i].status],
             record_count_in_case(CASES[i].id));
    list_row(b, CASES[i].title, meta,
             CASES[i].status == CASE_ACTIVE ? COL_AMBER : COL_MUTED, case_row_cb,
             (void *)(intptr_t)i);
  }
  if (g_case_count == 0)
    empty_note(b, "No cases yet. A case is a container for evidence about one "
                  "question, event or claim.");

  bottom_button(s, "NEW CASE", COL_AMBER, new_case_cb);
  return s;
}

static void case_edit_title(lv_event_t *) {
  if (g_sel_case < 0 || g_sel_case >= g_case_count) return;
  open_editor("CASE TITLE", CASES[g_sel_case].title, TITLE_LEN - 1,
              EF_CASE_TITLE, false);
}

static void case_edit_summary(lv_event_t *) {
  if (g_sel_case < 0 || g_sel_case >= g_case_count) return;
  open_editor("CASE SUMMARY", CASES[g_sel_case].summary, SUM_LEN - 1,
              EF_CASE_SUMMARY, false);
}

static void case_capture_cb(lv_event_t *) {
  if (g_sel_case < 0 || g_sel_case >= g_case_count) return;
  capture_begin(TYPE_OBSERVATION);
  g_draft.case_id = CASES[g_sel_case].id;
  g_draft.dirty = true;
  capture_store_draft();
  open_view(V_CAPTURE, 0);
}

static void case_wall_cb(lv_event_t *) {
  if (g_sel_case < 0 || g_sel_case >= g_case_count) return;
  g_wall_case = CASES[g_sel_case].id;
  open_view(V_WALL, 0);
}

static void case_archive_cb(lv_event_t *) {
  if (g_sel_case < 0 || g_sel_case >= g_case_count) return;
  confirm_ask("Mark this case closed? The records and threads are kept.",
              CA_ARCHIVE_CASE, CASES[g_sel_case].id);
}

static void case_delete_cb(lv_event_t *) {
  if (g_sel_case < 0 || g_sel_case >= g_case_count) return;
  confirm_ask("Delete this case? Its records become unfiled and its threads "
              "are removed.",
              CA_DEL_CASE, CASES[g_sel_case].id);
}

static lv_obj_t *build_case() {
  if (g_sel_case < 0 || g_sel_case >= g_case_count) return build_cases();
  CaseRec &c = CASES[g_sel_case];
  lv_obj_t *s = scr_new();
  header(s, "CASE", true);
  lv_obj_t *b = body(s, 64, 428);

  char meta[80];
  snprintf(meta, sizeof(meta), "%s · %d records · %d threads",
           CASE_STATUS_NAMES[c.status], record_count_in_case(c.id),
           g_link_count);
  lv_obj_t *m = label(b, meta, &lv_font_montserrat_12, COL_AMBER,
                      LV_ALIGN_TOP_LEFT, 2, 0);
  lv_obj_set_width(m, LCD_W - 44);

  {
    int all = case_link_count(c.id, -1);
    int sup = case_link_count(c.id, REL_SUPPORTS);
    int con = case_link_count(c.id, REL_CONTRADICTS);
    char ev[80];
    snprintf(ev, sizeof(ev), "%d threads · %d support · %d contradict · %d other",
             all, sup, con, all - sup - con);
    lv_obj_t *el = label(b, ev, &lv_font_montserrat_12, COL_MUTED,
                         LV_ALIGN_TOP_LEFT, 2, 0);
    lv_obj_set_width(el, LCD_W - 44);
  }

  if (c.summary[0]) {
    lv_obj_t *sm = label(b, c.summary, &lv_font_montserrat_14, COL_CREAM,
                         LV_ALIGN_TOP_LEFT, 2, 0);
    lv_obj_set_width(sm, LCD_W - 44);
    lv_label_set_long_mode(sm, LV_LABEL_LONG_WRAP);
  }

  field(b, "TITLE", c.title, LCD_W - 36, 56, case_edit_title, nullptr);
  field(b, "SUMMARY", c.summary, LCD_W - 36, 66, case_edit_summary, nullptr);

  lv_obj_t *row = flex_row(b, 50);
  lv_obj_t *cap = lv_btn_create(row);
  lv_obj_set_size(cap, 170, 50);
  style_btn(cap, COL_AMBER);
  label(cap, "CAPTURE INTO CASE", &lv_font_montserrat_12, COL_AMBER,
        LV_ALIGN_TOP_LEFT, 8, 8);
  label(cap, "opens quick capture", &lv_font_montserrat_12, COL_MUTED,
        LV_ALIGN_TOP_LEFT, 8, 26);
  lv_obj_add_event_cb(cap, case_capture_cb, LV_EVENT_CLICKED, nullptr);

  lv_obj_t *wall = lv_btn_create(row);
  lv_obj_set_size(wall, 170, 50);
  style_btn(wall, COL_THREAD);
  label(wall, "EVIDENCE WALL", &lv_font_montserrat_12, COL_THREAD,
        LV_ALIGN_TOP_LEFT, 8, 8);
  label(wall, "this case only", &lv_font_montserrat_12, COL_MUTED,
        LV_ALIGN_TOP_LEFT, 8, 26);
  lv_obj_add_event_cb(wall, case_wall_cb, LV_EVENT_CLICKED, nullptr);

  lv_obj_t *ch = label(b, "RECORDS IN THIS CASE", &lv_font_montserrat_12,
                       COL_AMBER, LV_ALIGN_TOP_LEFT, 2, 0);
  (void)ch;

  int shown = 0;
  for (int i = 0; i < g_record_count; i++) {
    if (RECORDS[i].case_id != c.id) continue;
    char rm[64];
    char when[24];
    fmt_datetime(when, sizeof(when), RECORDS[i].created);
    snprintf(rm, sizeof(rm), "%s · %s · %s", TYPE_SHORT[RECORDS[i].type],
             status_name(RECORDS[i].type, RECORDS[i].status), when);
    list_row(b, RECORDS[i].title, rm, COL_LINE, record_row_cb,
             (void *)(intptr_t)i);
    shown++;
  }
  if (shown == 0)
    empty_note(b, "Nothing filed here yet. Capture into this case to start the "
                  "stack.");

  action_button(b, "CLOSE / ARCHIVE CASE", LCD_W - 36, 46, COL_LINE,
                case_archive_cb, nullptr);
  action_button(b, "DELETE CASE", LCD_W - 36, 46, COL_THREAD, case_delete_cb,
                nullptr);
  return s;
}

// ---------------------------------------------------------------------------
//  Timeline
// ---------------------------------------------------------------------------
static uint32_t record_ts(int i) {
  return RECORDS[i].event_at ? RECORDS[i].event_at : RECORDS[i].created;
}

static void sort_results_by_time() {
  for (int i = 1; i < g_result_count; i++) {
    int key = g_results[i];
    uint32_t kt = record_ts(key);
    int j = i - 1;
    while (j >= 0) {
      uint32_t jt = record_ts(g_results[j]);
      bool move = g_timeline_desc ? (jt < kt) : (jt > kt);
      if (!move) break;
      g_results[j + 1] = g_results[j];
      j--;
    }
    g_results[j + 1] = key;
  }
}

static void timeline_sort_cb(lv_event_t *) {
  g_timeline_desc = g_timeline_desc ? 0 : 1;
  render();
}

static lv_obj_t *build_timeline() {
  lv_obj_t *s = scr_new();
  header(s, "TIMELINE", true);
  lv_obj_t *b = body(s, 64, 428);

  action_button(b, g_timeline_desc ? "NEWEST FIRST" : "OLDEST FIRST",
                LCD_W - 36, 42, COL_LINE, timeline_sort_cb, nullptr);

  g_result_count = 0;
  for (int i = 0; i < g_record_count; i++) {
    if (RECORDS[i].status == ST_ARCHIVED) continue;
    g_results[g_result_count++] = i;
  }
  sort_results_by_time();

  char meta[80];
  for (int i = 0; i < g_result_count; i++) {
    int idx = g_results[i];
    char when[24];
    fmt_datetime(when, sizeof(when), record_ts(idx));
    snprintf(meta, sizeof(meta), "%s · %s · %s", when,
             TYPE_SHORT[RECORDS[idx].type], case_title(RECORDS[idx].case_id));
    list_row(b, RECORDS[idx].title, meta,
             RECORDS[idx].event_at ? COL_AMBER : COL_LINE, record_row_cb,
             (void *)(intptr_t)idx);
  }
  if (g_result_count == 0)
    empty_note(b, "The timeline is empty. Captured records appear here in the "
                  "order things happened.");
  return s;
}

// ---------------------------------------------------------------------------
//  Evidence wall and nodes
// ---------------------------------------------------------------------------
static void wall_filter_cb(lv_event_t *) {
  if (g_wall_case == 0) {
    g_wall_case = (g_case_count > 0) ? CASES[0].id : 0;
  } else {
    int idx = find_case_index(g_wall_case);
    idx++;
    g_wall_case = (idx >= 0 && idx < g_case_count) ? CASES[idx].id : 0;
  }
  render();
}

static void wall_node_cb(lv_event_t *e) {
  int idx = (int)(intptr_t)lv_event_get_user_data(e);
  if (idx < 0 || idx >= g_record_count) return;
  g_sel = idx;
  open_view(V_NODE, 0);
}

static lv_obj_t *build_wall() {
  lv_obj_t *s = scr_new();
  header(s, "EVIDENCE WALL", true);
  lv_obj_t *b = body(s, 64, 428);

  char fbuf[64];
  snprintf(fbuf, sizeof(fbuf), "CASE: %s",
           g_wall_case ? case_title(g_wall_case) : "all cases");
  action_button(b, fbuf, LCD_W - 36, 42, COL_LINE, wall_filter_cb, nullptr);

  int nodes = 0;
  int shown = 0;
  for (int i = 0; i < g_record_count; i++) {
    if (g_wall_case != 0 && RECORDS[i].case_id != g_wall_case) continue;
    nodes++;
    int threads = link_count_for(RECORDS[i].id);
    char meta[64];
    snprintf(meta, sizeof(meta), "%s · %d thread%s", TYPE_SHORT[RECORDS[i].type],
             threads, threads == 1 ? "" : "s");
    list_row(b, RECORDS[i].title, meta, threads ? COL_THREAD : COL_LINE,
             wall_node_cb, (void *)(intptr_t)i);
    shown++;
  }
  if (shown == 0)
    empty_note(b, "No nodes in this view. Capture evidence, then connect two "
                  "records with a red thread.");

  if (nodes > 0 && g_link_count == 0) {
    lv_obj_t *h = label(b, "A wall is only useful once you claim how two "
                           "records relate. Open a node and add a thread.",
                        &lv_font_montserrat_12, COL_MUTED, LV_ALIGN_TOP_LEFT, 2,
                        0);
    lv_obj_set_width(h, LCD_W - 44);
    lv_label_set_long_mode(h, LV_LABEL_LONG_WRAP);
  }
  return s;
}

static void node_add_thread_cb(lv_event_t *) {
  if (g_sel < 0 || g_sel >= g_record_count) return;
  g_link_from = g_sel;
  open_view(V_LINKPICK, 0);
}

static void node_open_detail_cb(lv_event_t *) { open_view(V_DETAIL, 0); }

static lv_obj_t *build_node() {
  if (g_sel < 0 || g_sel >= g_record_count) return build_wall();
  Record &r = RECORDS[g_sel];
  lv_obj_t *s = scr_new();
  header(s, "NODE", true);
  lv_obj_t *b = body(s, 64, 428);

  char meta[96];
  char when[24];
  fmt_datetime(when, sizeof(when), record_ts(g_sel));
  snprintf(meta, sizeof(meta), "%s · %s · %s · %s", TYPE_SHORT[r.type],
           status_name(r.type, r.status), case_title(r.case_id), when);
  lv_obj_t *m = label(b, meta, &lv_font_montserrat_12, COL_AMBER,
                      LV_ALIGN_TOP_LEFT, 2, 0);
  lv_obj_set_width(m, LCD_W - 44);

  lv_obj_t *t = label(b, r.title, &lv_font_montserrat_16, COL_CREAM,
                      LV_ALIGN_TOP_LEFT, 2, 0);
  lv_obj_set_width(t, LCD_W - 44);
  lv_label_set_long_mode(t, LV_LABEL_LONG_WRAP);

  if (r.body[0]) {
    lv_obj_t *bd = label(b, r.body, &lv_font_montserrat_12, COL_MUTED,
                         LV_ALIGN_TOP_LEFT, 2, 0);
    lv_obj_set_width(bd, LCD_W - 44);
    lv_label_set_long_mode(bd, LV_LABEL_LONG_WRAP);
  }

  lv_obj_t *h = label(b, "THREADS", &lv_font_montserrat_12, COL_THREAD,
                      LV_ALIGN_TOP_LEFT, 2, 0);
  (void)h;

  int shown = 0;
  for (int i = 0; i < g_link_count; i++) {
    if (LINKS[i].from_id != r.id && LINKS[i].to_id != r.id) continue;
    uint32_t other_id =
        (LINKS[i].from_id == r.id) ? LINKS[i].to_id : LINKS[i].from_id;
    int other = find_record_index(other_id);
    char line[120];
    snprintf(line, sizeof(line), "%s %s",
             (LINKS[i].from_id == r.id) ? "→" : "←",
             relation_name(LINKS[i].relation));
    lv_obj_t *row = lv_obj_create(b);
    lv_obj_set_width(row, LCD_W - 36);
    lv_obj_set_height(row, 58);
    style_panel(row);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(row, node_open_cb, LV_EVENT_CLICKED,
                        (void *)(intptr_t)other);
    label(row, line, &lv_font_montserrat_12, COL_THREAD, LV_ALIGN_TOP_LEFT, 12,
          6);
    lv_obj_t *ol = label(row,
                         other >= 0 ? RECORDS[other].title : "missing record",
                         &lv_font_montserrat_14, COL_CREAM, LV_ALIGN_TOP_LEFT,
                         12, 26);
    lv_obj_set_width(ol, LCD_W - 120);
    lv_label_set_long_mode(ol, LV_LABEL_LONG_DOT);
    lv_obj_t *del = lv_btn_create(row);
    lv_obj_set_size(del, 44, 36);
    lv_obj_align(del, LV_ALIGN_RIGHT_MID, -6, 0);
    style_btn(del, COL_THREAD);
    label(del, "X", &lv_font_montserrat_14, COL_THREAD, LV_ALIGN_CENTER, 0, 0);
    lv_obj_add_event_cb(del, thread_delete_cb, LV_EVENT_CLICKED,
                        (void *)(intptr_t)i);
    shown++;
  }
  if (shown == 0)
    empty_note(b, "This node has no threads yet.");

  action_button(b, "ADD RED THREAD", LCD_W - 36, 48, COL_THREAD,
                node_add_thread_cb, nullptr);
  action_button(b, "OPEN FULL RECORD", LCD_W - 36, 44, COL_LINE,
                node_open_detail_cb, nullptr);
  return s;
}

// ---------------------------------------------------------------------------
//  Thread creation
// ---------------------------------------------------------------------------
static void linkpick_cb(lv_event_t *e) {
  int idx = (int)(intptr_t)lv_event_get_user_data(e);
  if (idx < 0 || idx >= g_record_count) return;
  g_link_pick = idx;
  open_view(V_LINKREL, 0);
}

static lv_obj_t *build_linkpick() {
  if (g_link_from < 0 || g_link_from >= g_record_count) return build_wall();
  lv_obj_t *s = scr_new();
  header(s, "PICK A NODE", true);
  lv_obj_t *b = body(s, 64, 428);

  char head[80];
  snprintf(head, sizeof(head), "Connecting from: %s", RECORDS[g_link_from].title);
  lv_obj_t *h = label(b, head, &lv_font_montserrat_12, COL_AMBER,
                      LV_ALIGN_TOP_LEFT, 2, 0);
  lv_obj_set_width(h, LCD_W - 44);
  lv_label_set_long_mode(h, LV_LABEL_LONG_WRAP);

  int shown = 0;
  for (int i = 0; i < g_record_count; i++) {
    if (i == g_link_from) continue;
    if (link_exists(RECORDS[g_link_from].id, RECORDS[i].id)) continue;
    char meta[64];
    snprintf(meta, sizeof(meta), "%s · %s", TYPE_SHORT[RECORDS[i].type],
             case_title(RECORDS[i].case_id));
    list_row(b, RECORDS[i].title, meta, COL_LINE, linkpick_cb,
             (void *)(intptr_t)i);
    shown++;
  }
  if (shown == 0)
    empty_note(b, "Every other record is already connected, or there is only "
                  "one record.");
  return s;
}

static void linkrel_cb(lv_event_t *e) {
  int rel = (int)(intptr_t)lv_event_get_user_data(e);
  if (g_link_from < 0 || g_link_from >= g_record_count) return;
  if (g_link_pick < 0 || g_link_pick >= g_record_count) return;
  int made = create_link(RECORDS[g_link_from].id, RECORDS[g_link_pick].id,
                         (uint8_t)rel);
  if (made < 0) {
    toast(g_store_error[0] ? g_store_error : "THREAD FAILED");
    return;
  }
  g_sel = g_link_from;
  g_view_depth -= 2;
  if (g_view_depth < 0) g_view_depth = 0;
  g_view = V_NODE;
  toast("THREAD ADDED");
  render();
}

static lv_obj_t *build_linkrel() {
  if (g_link_from < 0 || g_link_from >= g_record_count) return build_wall();
  lv_obj_t *s = scr_new();
  header(s, "RELATION", true);
  lv_obj_t *b = body(s, 64, 428);

  char a[TITLE_LEN + 8], c[TITLE_LEN + 8];
  snprintf(a, sizeof(a), "%s", RECORDS[g_link_from].title);
  snprintf(c, sizeof(c), "%s",
           (g_link_pick >= 0 && g_link_pick < g_record_count)
               ? RECORDS[g_link_pick].title
               : "?");

  lv_obj_t *l1 = label(b, a, &lv_font_montserrat_14, COL_CREAM,
                       LV_ALIGN_TOP_LEFT, 2, 0);
  lv_obj_set_width(l1, LCD_W - 44);
  lv_label_set_long_mode(l1, LV_LABEL_LONG_WRAP);
  label(b, "how does this relate to", &lv_font_montserrat_12, COL_MUTED,
        LV_ALIGN_TOP_LEFT, 2, 0);
  lv_obj_t *l2 = label(b, c, &lv_font_montserrat_14, COL_CREAM,
                       LV_ALIGN_TOP_LEFT, 2, 0);
  lv_obj_set_width(l2, LCD_W - 44);
  lv_label_set_long_mode(l2, LV_LABEL_LONG_WRAP);

  for (int i = 0; i < REL_COUNT; i++) {
    lv_obj_t *btn = action_button(b, relation_name(i), LCD_W - 36, 46,
                                  i == REL_CONTRADICTS ? COL_THREAD : COL_LINE,
                                  linkrel_cb, (void *)(intptr_t)i);
    (void)btn;
  }
  lv_obj_t *note = label(b, "A thread is a visible claim, not a fact. Label it "
                            "honestly and revisit it.",
                         &lv_font_montserrat_12, COL_MUTED, LV_ALIGN_TOP_LEFT,
                         2, 0);
  lv_obj_set_width(note, LCD_W - 44);
  lv_label_set_long_mode(note, LV_LABEL_LONG_WRAP);
  return s;
}

// ---------------------------------------------------------------------------
//  Question board
// ---------------------------------------------------------------------------
static void question_row_cb(lv_event_t *e) {
  int idx = (int)(intptr_t)lv_event_get_user_data(e);
  if (idx < 0 || idx >= g_record_count) return;
  g_sel = idx;
  open_view(V_QDETAIL, 0);
}

static void question_new_cb(lv_event_t *) {
  capture_begin(TYPE_QUESTION);
  open_view(V_CAPTURE, 0);
}

static lv_obj_t *build_questions() {
  lv_obj_t *s = scr_new();
  header(s, "QUESTION BOARD", true);
  lv_obj_t *b = body(s, 64, 372);

  int shown = 0;
  for (int pass = 0; pass < 4; pass++) {
    for (int i = 0; i < g_record_count; i++) {
      if (RECORDS[i].type != TYPE_QUESTION) continue;
      if (RECORDS[i].status != pass) continue;
      char meta[64];
      snprintf(meta, sizeof(meta), "%s · %s", status_name(TYPE_QUESTION, pass),
               case_title(RECORDS[i].case_id));
      list_row(b, RECORDS[i].title, meta,
               pass == ST_OPEN ? COL_AMBER
                               : (pass == ST_ANSWERED ? COL_MUTED : COL_LINE),
               question_row_cb, (void *)(intptr_t)i);
      shown++;
    }
  }
  if (shown == 0)
    empty_note(b, "No questions yet. The board works best when questions are "
                  "written as gaps, not as claims.");

  bottom_button(s, "NEW QUESTION", COL_AMBER, question_new_cb);
  return s;
}

static void q_set_status_cb(lv_event_t *e) {
  int st = (int)(intptr_t)lv_event_get_user_data(e);
  if (g_sel < 0 || g_sel >= g_record_count) return;
  RECORDS[g_sel].status = (uint8_t)st;
  RECORDS[g_sel].updated = (uint32_t)time(nullptr);
  save_records();
  render();
}

static void q_open_full_cb(lv_event_t *) { open_view(V_DETAIL, 0); }

static lv_obj_t *build_qdetail() {
  if (g_sel < 0 || g_sel >= g_record_count) return build_questions();
  Record &r = RECORDS[g_sel];
  lv_obj_t *s = scr_new();
  header(s, "QUESTION", true);
  lv_obj_t *b = body(s, 64, 428);

  char meta[80];
  char when[24];
  fmt_datetime(when, sizeof(when), r.created);
  snprintf(meta, sizeof(meta), "%s · %s · %s", status_name(TYPE_QUESTION, r.status),
           case_title(r.case_id), when);
  lv_obj_t *m = label(b, meta, &lv_font_montserrat_12, COL_AMBER,
                      LV_ALIGN_TOP_LEFT, 2, 0);
  lv_obj_set_width(m, LCD_W - 44);

  lv_obj_t *t = label(b, r.title, &lv_font_montserrat_16, COL_CREAM,
                      LV_ALIGN_TOP_LEFT, 2, 0);
  lv_obj_set_width(t, LCD_W - 44);
  lv_label_set_long_mode(t, LV_LABEL_LONG_WRAP);

  if (r.body[0]) {
    lv_obj_t *bd = label(b, r.body, &lv_font_montserrat_12, COL_MUTED,
                         LV_ALIGN_TOP_LEFT, 2, 0);
    lv_obj_set_width(bd, LCD_W - 44);
    lv_label_set_long_mode(bd, LV_LABEL_LONG_WRAP);
  }

  label(b, "SET STATE", &lv_font_montserrat_12, COL_AMBER, LV_ALIGN_TOP_LEFT, 2,
        0);
  for (int i = 0; i < 4; i++) {
    char text[40];
    snprintf(text, sizeof(text), "%s%s", (r.status == i) ? "• " : "",
             QUESTION_STATUS[i]);
    action_button(b, text, LCD_W - 36, 44,
                  (r.status == i) ? COL_AMBER : COL_LINE, q_set_status_cb,
                  (void *)(intptr_t)i);
  }

  action_button(b, "OPEN FULL RECORD", LCD_W - 36, 46, COL_LINE,
                q_open_full_cb, nullptr);
  return s;
}

// ---------------------------------------------------------------------------
//  Search
// ---------------------------------------------------------------------------
static void search_edit_cb(lv_event_t *) {
  open_editor("SEARCH", g_search_query, sizeof(g_search_query) - 1, EF_SEARCH,
              false);
}

static bool text_has(const char *hay, const char *needle) {
  if (!hay || !needle || !needle[0]) return false;
  return strcasestr(hay, needle) != nullptr;
}

static lv_obj_t *build_search() {
  lv_obj_t *s = scr_new();
  header(s, "SEARCH", true);
  lv_obj_t *b = body(s, 64, 428);

  lv_obj_t *q = lv_btn_create(b);
  lv_obj_set_width(q, LCD_W - 36);
  lv_obj_set_height(q, 54);
  style_btn(q, COL_AMBER);
  label(q, "QUERY", &lv_font_montserrat_12, COL_AMBER, LV_ALIGN_TOP_LEFT, 12, 7);
  lv_obj_t *qv = label(q, g_search_query[0] ? g_search_query : "tap to search",
                       &lv_font_montserrat_14,
                       g_search_query[0] ? COL_CREAM : COL_MUTED,
                       LV_ALIGN_TOP_LEFT, 12, 25);
  lv_obj_set_width(qv, LCD_W - 60);
  lv_label_set_long_mode(qv, LV_LABEL_LONG_DOT);
  lv_obj_add_event_cb(q, search_edit_cb, LV_EVENT_CLICKED, nullptr);

  if (!g_search_query[0]) {
    empty_note(b, "Search titles, notes, tags, sources and case names. "
                  "Everything stays on this card.");
    return s;
  }

  int hits = 0;
  char meta[80];
  for (int i = 0; i < g_record_count && hits < 60; i++) {
    Record &r = RECORDS[i];
    bool match = text_has(r.title, g_search_query) ||
                 text_has(r.body, g_search_query) ||
                 text_has(r.tags, g_search_query) ||
                 text_has(r.source, g_search_query);
    if (!match && r.case_id) match = text_has(case_title(r.case_id), g_search_query);
    if (!match) continue;
    snprintf(meta, sizeof(meta), "%s · %s · %s", TYPE_SHORT[r.type],
             status_name(r.type, r.status), case_title(r.case_id));
    list_row(b, r.title[0] ? r.title : "(untitled)", meta, COL_AMBER,
             record_row_cb, (void *)(intptr_t)i);
    hits++;
  }
  char tail[48];
  snprintf(tail, sizeof(tail), "%d match%s", hits, hits == 1 ? "" : "es");
  label(b, tail, &lv_font_montserrat_12, COL_MUTED, LV_ALIGN_TOP_LEFT, 2, 0);
  if (hits == 0) empty_note(b, "Nothing matches that query.");
  return s;
}

// ---------------------------------------------------------------------------
//  Knowledge cards
// ---------------------------------------------------------------------------
static void card_row_cb(lv_event_t *e) {
  int idx = (int)(intptr_t)lv_event_get_user_data(e);
  if (idx < 0 || idx >= g_card_count) return;
  g_card_sel = idx;
  open_view(V_CARDEDIT, 0);
}

static void card_new_cb(lv_event_t *) {
  if (g_card_count >= MAX_CARDS) {
    toast("CARD STORE FULL");
    return;
  }
  Card &c = CARDS[g_card_count];
  memset(&c, 0, sizeof(c));
  c.id = g_card_next++;
  copy_field(c.front, sizeof(c.front), "New card");
  g_card_count++;
  save_cards();
  g_card_sel = g_card_count - 1;
  open_view(V_CARDEDIT, 0);
}

static void card_study_cb(lv_event_t *e) {
  int mode = (int)(intptr_t)lv_event_get_user_data(e);
  if (g_card_count == 0) {
    toast("NO CARDS YET");
    return;
  }
  g_card_mode = mode;
  g_card_sel = (int)(esp_random() % (uint32_t)g_card_count);
  open_view(V_CARDSTUDY, 0);
}

// ---------------------------------------------------------------------------
//  Card scheduling
// ---------------------------------------------------------------------------
// A card's interval follows how many times it has been recalled, so a card
// known ten times is not still offered tomorrow. "Again" drops it back to the
// first rung; "Easy" jumps one rung further.
static uint32_t card_step_days(uint16_t reviews) {
  static const uint16_t ladder[8] = {1, 3, 7, 16, 35, 90, 180, 365};
  return ladder[reviews < 8 ? reviews : 7];
}

static bool card_is_due(const Card &c, uint32_t now) {
  if (c.due == 0) return true;  // never reviewed
  return (int32_t)(now - c.due) >= 0;
}

static int card_due_count() {
  uint32_t now = (uint32_t)time(nullptr);
  if (now < (uint32_t)TIME_VALID_MIN) return 0;
  int n = 0;
  for (int i = 0; i < g_card_count; i++)
    if (card_is_due(CARDS[i], now)) n++;
  return n;
}

static int first_due_index() {
  uint32_t now = (uint32_t)time(nullptr);
  if (g_card_count == 0 || now < (uint32_t)TIME_VALID_MIN) return -1;
  for (int i = 0; i < g_card_count; i++)
    if (card_is_due(CARDS[i], now)) return i;
  return -1;
}

static int next_due_index(int from) {
  uint32_t now = (uint32_t)time(nullptr);
  if (g_card_count == 0 || now < (uint32_t)TIME_VALID_MIN) return -1;
  for (int k = 1; k <= g_card_count; k++) {
    int i = ((from % g_card_count) + k) % g_card_count;
    if (card_is_due(CARDS[i], now)) return i;
  }
  return -1;
}

static void card_review_due_cb(lv_event_t *) {
  int idx = first_due_index();
  if (idx < 0) {
    toast("NOTHING DUE");
    return;
  }
  g_card_sel = idx;
  g_card_mode = 0;
  open_view(V_CARDSTUDY, 0);
}

static lv_obj_t *build_cards() {
  lv_obj_t *s = scr_new();
  header(s, "KNOWLEDGE CARDS", true);
  lv_obj_t *b = body(s, 64, 322);

  char meta[72];
  uint32_t now = (uint32_t)time(nullptr);
  for (int i = 0; i < g_card_count; i++) {
    char when[24];
    bool due = card_is_due(CARDS[i], now);
    if (CARDS[i].due == 0) snprintf(when, sizeof(when), "new");
    else if (due) snprintf(when, sizeof(when), "due now");
    else snprintf(when, sizeof(when), "in %lud",
                  (unsigned long)((CARDS[i].due - now + 86399UL) / 86400UL));
    snprintf(meta, sizeof(meta), "%s · %u review%s", when,
             (unsigned)CARDS[i].reviews, CARDS[i].reviews == 1 ? "" : "s");
    list_row(b, CARDS[i].front, meta, due ? COL_AMBER : COL_LINE, card_row_cb,
             (void *)(intptr_t)i);
  }
  if (g_card_count == 0)
    empty_note(b, "No cards yet. A card is one question on the front and the "
                  "answer on the back.");

  lv_obj_t *row = flex_row(s, 52);
  lv_obj_set_pos(row, 14, 396);
  lv_obj_t *b1 = lv_btn_create(row);
  lv_obj_set_size(b1, 118, 46);
  style_btn(b1, COL_AMBER);
  label(b1, "NEW", &lv_font_montserrat_14, COL_CREAM, LV_ALIGN_CENTER, 0, 0);
  lv_obj_add_event_cb(b1, card_new_cb, LV_EVENT_CLICKED, nullptr);

  lv_obj_t *b2 = lv_btn_create(row);
  lv_obj_set_size(b2, 118, 46);
  style_btn(b2, COL_AMBER);
  label(b2, "REVIEW DUE", &lv_font_montserrat_12, COL_CREAM, LV_ALIGN_CENTER, 0,
        0);
  lv_obj_add_event_cb(b2, card_review_due_cb, LV_EVENT_CLICKED, nullptr);

  lv_obj_t *b3 = lv_btn_create(row);
  lv_obj_set_size(b3, 118, 46);
  style_btn(b3, COL_LINE);
  label(b3, "BLIND", &lv_font_montserrat_14, COL_CREAM, LV_ALIGN_CENTER, 0, 0);
  lv_obj_add_event_cb(b3, card_study_cb, LV_EVENT_CLICKED, (void *)(intptr_t)1);

  lv_obj_t *b4 = lv_btn_create(row);
  lv_obj_set_size(b4, 118, 46);
  style_btn(b4, COL_LINE);
  label(b4, "RANDOM", &lv_font_montserrat_14, COL_CREAM, LV_ALIGN_CENTER, 0, 0);
  lv_obj_add_event_cb(b4, card_study_cb, LV_EVENT_CLICKED, (void *)(intptr_t)2);
  return s;
}

static void card_edit_front(lv_event_t *) {
  if (g_card_sel < 0 || g_card_sel >= g_card_count) return;
  open_editor("FRONT", CARDS[g_card_sel].front, FRONT_LEN - 1, EF_CARD_FRONT,
              false);
}
static void card_edit_back(lv_event_t *) {
  if (g_card_sel < 0 || g_card_sel >= g_card_count) return;
  open_editor("BACK", CARDS[g_card_sel].back, BACK_LEN - 1, EF_CARD_BACK, false);
}
static void card_edit_tags(lv_event_t *) {
  if (g_card_sel < 0 || g_card_sel >= g_card_count) return;
  open_editor("TAGS", CARDS[g_card_sel].tags, TAG_LEN - 1, EF_CARD_TAGS, false);
}

static void card_delete_cb(lv_event_t *) {
  if (g_card_sel < 0 || g_card_sel >= g_card_count) return;
  confirm_ask("Delete this card?", CA_DEL_CARD, CARDS[g_card_sel].id);
}

static void card_study_this_cb(lv_event_t *) {
  if (g_card_sel < 0 || g_card_sel >= g_card_count) return;
  g_card_mode = 0;
  open_view(V_CARDSTUDY, 0);
}

static lv_obj_t *build_cardedit() {
  if (g_card_sel < 0 || g_card_sel >= g_card_count) return build_cards();
  Card &c = CARDS[g_card_sel];
  lv_obj_t *s = scr_new();
  header(s, "CARD", true);
  lv_obj_t *b = body(s, 64, 428);

  field(b, "FRONT", c.front, LCD_W - 36, 66, card_edit_front, nullptr);
  field(b, "BACK", c.back, LCD_W - 36, 86, card_edit_back, nullptr);
  field(b, "TAGS", c.tags, LCD_W - 36, 56, card_edit_tags, nullptr);

  char meta[64];
  snprintf(meta, sizeof(meta), "reviewed %u time%s", (unsigned)c.reviews,
           c.reviews == 1 ? "" : "s");
  lv_obj_t *m = label(b, meta, &lv_font_montserrat_12, COL_MUTED,
                      LV_ALIGN_TOP_LEFT, 2, 0);
  (void)m;

  action_button(b, "STUDY THIS CARD", LCD_W - 36, 48, COL_AMBER,
                card_study_this_cb, nullptr);
  action_button(b, "DELETE CARD", LCD_W - 36, 44, COL_THREAD, card_delete_cb,
                nullptr);
  return s;
}

static bool g_card_revealed = false;

static void card_reveal_cb(lv_event_t *) {
  g_card_revealed = true;
  render();
}

static void card_grade_cb(lv_event_t *e) {
  int grade = (int)(intptr_t)lv_event_get_user_data(e);
  if (g_card_sel < 0 || g_card_sel >= g_card_count) return;
  Card &c = CARDS[g_card_sel];
  uint32_t now = (uint32_t)time(nullptr);
  if (now < (uint32_t)TIME_VALID_MIN) {
    toast("SET THE CLOCK FIRST");
    return;
  }
  char msg[40];
  if (grade == 0) {
    c.reviews = 0;  // back to the first rung
    c.due     = now + 600UL;
    snprintf(msg, sizeof(msg), "AGAIN IN 10 MIN");
  } else {
    uint32_t days = card_step_days(c.reviews);
    if (grade == 2 && days < 365) days *= 2;
    c.due = now + days * 86400UL;
    if (c.reviews < 65000) c.reviews++;
    snprintf(msg, sizeof(msg), "IN %lu DAY%s", (unsigned long)days,
             days == 1 ? "" : "S");
  }
  save_cards();
  g_card_revealed = false;

  int next = next_due_index(g_card_sel);
  if (next >= 0) {
    g_card_sel = next;
    toast(msg);
    render();
  } else {
    toast("DECK CLEAR");
    open_view(V_CARDS, 0);
  }
}

static void card_flip_cb(lv_event_t *) {
  g_card_revealed = !g_card_revealed;
  render();
}

static lv_obj_t *build_cardstudy() {
  if (g_card_sel < 0 || g_card_sel >= g_card_count) return build_cards();
  Card &c = CARDS[g_card_sel];
  lv_obj_t *s = scr_new();
  header(s, g_card_mode == 2 ? "ROULETTE" : (g_card_mode == 1 ? "BLIND RECALL" : "REVIEW"),
         true);
  lv_obj_t *b = body(s, 64, 428);

  lv_obj_t *q = label(b, c.front, &lv_font_montserrat_18, COL_CREAM,
                      LV_ALIGN_TOP_LEFT, 2, 0);
  lv_obj_set_width(q, LCD_W - 44);
  lv_label_set_long_mode(q, LV_LABEL_LONG_WRAP);

  bool show_back = (g_card_mode == 0) || g_card_revealed;
  if (show_back) {
    lv_obj_t *lbl = label(b, "ANSWER", &lv_font_montserrat_12, COL_AMBER,
                          LV_ALIGN_TOP_LEFT, 2, 6);
    (void)lbl;
    lv_obj_t *a = label(b, c.back[0] ? c.back : "(no answer yet)",
                        &lv_font_montserrat_16, COL_CREAM, LV_ALIGN_TOP_LEFT, 2,
                        0);
    lv_obj_set_width(a, LCD_W - 44);
    lv_label_set_long_mode(a, LV_LABEL_LONG_WRAP);
  } else {
    empty_note(b, "Answer it in your head first. Then reveal.");
    action_button(b, "REVEAL ANSWER", LCD_W - 36, 50, COL_AMBER, card_reveal_cb,
                  nullptr);
  }

  if (c.tags[0]) {
    lv_obj_t *tg = label(b, c.tags, &lv_font_montserrat_12, COL_MUTED,
                         LV_ALIGN_TOP_LEFT, 2, 0);
    (void)tg;
  }

  lv_obj_t *row = flex_row(b, 50);
  const char *grades[3] = {"AGAIN", "GOOD", "EASY"};
  for (int i = 0; i < 3; i++) {
    lv_obj_t *btn = lv_btn_create(row);
    lv_obj_set_size(btn, 110, 46);
    style_btn(btn, i == 0 ? COL_THREAD : COL_LINE);
    label(btn, grades[i], &lv_font_montserrat_12, COL_CREAM, LV_ALIGN_CENTER, 0,
          0);
    lv_obj_add_event_cb(btn, card_grade_cb, LV_EVENT_CLICKED,
                        (void *)(intptr_t)i);
  }

  if (show_back && g_card_mode != 0) {
    action_button(b, "HIDE ANSWER", LCD_W - 36, 40, COL_LINE, card_flip_cb,
                  nullptr);
  }
  return s;
}

// ---------------------------------------------------------------------------
//  Decision journal
// ---------------------------------------------------------------------------
static void decision_row_cb(lv_event_t *e) {
  int idx = (int)(intptr_t)lv_event_get_user_data(e);
  if (idx < 0 || idx >= g_decision_count) return;
  g_dec_sel = idx;
  open_view(V_DDETAIL, 0);
}

static void decision_new_cb(lv_event_t *) {
  if (g_decision_count >= MAX_DECISIONS) {
    toast("DECISION STORE FULL");
    return;
  }
  Decision &d = DECISIONS[g_decision_count];
  memset(&d, 0, sizeof(d));
  d.id = g_decision_next++;
  d.confidence = 3;
  d.created = (uint32_t)time(nullptr);
  d.review_at = d.created + 30UL * 86400UL;
  copy_field(d.title, sizeof(d.title), "Untitled decision");
  g_decision_count++;
  save_decisions();
  g_dec_sel = g_decision_count - 1;
  open_view(V_DDETAIL, 0);
}

static lv_obj_t *build_decisions() {
  lv_obj_t *s = scr_new();
  header(s, "DECISION JOURNAL", true);
  lv_obj_t *b = body(s, 64, 372);

  char meta[64];
  for (int i = g_decision_count - 1; i >= 0; i--) {
    char when[24];
    fmt_datetime(when, sizeof(when), DECISIONS[i].created);
    bool review_due = !DECISIONS[i].outcome[0] && DECISIONS[i].review_at &&
                      (int32_t)((uint32_t)time(nullptr) - DECISIONS[i].review_at) >= 0;
    snprintf(meta, sizeof(meta), "confidence %u/5 · %s%s",
             (unsigned)DECISIONS[i].confidence, when,
             review_due ? " · review due" : "");
    list_row(b, DECISIONS[i].title, meta,
             DECISIONS[i].outcome[0] ? COL_MUTED : COL_AMBER, decision_row_cb,
             (void *)(intptr_t)i);
  }
  if (g_decision_count == 0)
    empty_note(b, "No decisions logged. Write the reasoning now, while the "
                  "outcome is still unknown.");

  bottom_button(s, "NEW DECISION", COL_AMBER, decision_new_cb);
  return s;
}

static void dec_edit_title(lv_event_t *) {
  if (g_dec_sel < 0 || g_dec_sel >= g_decision_count) return;
  open_editor("DECISION", DECISIONS[g_dec_sel].title, TITLE_LEN - 1,
              EF_DEC_TITLE, false);
}
static void dec_edit_options(lv_event_t *) {
  if (g_dec_sel < 0 || g_dec_sel >= g_decision_count) return;
  open_editor("OPTIONS", DECISIONS[g_dec_sel].options, OPT_LEN - 1,
              EF_DEC_OPTIONS, false);
}
static void dec_edit_rationale(lv_event_t *) {
  if (g_dec_sel < 0 || g_dec_sel >= g_decision_count) return;
  open_editor("WHY", DECISIONS[g_dec_sel].rationale, RAT_LEN - 1,
              EF_DEC_RATIONALE, false);
}
static void dec_edit_expected(lv_event_t *) {
  if (g_dec_sel < 0 || g_dec_sel >= g_decision_count) return;
  open_editor("EXPECTED", DECISIONS[g_dec_sel].expected, DEC_FIELD - 1,
              EF_DEC_EXPECTED, false);
}
static void dec_edit_outcome(lv_event_t *) {
  if (g_dec_sel < 0 || g_dec_sel >= g_decision_count) return;
  open_editor("OUTCOME", DECISIONS[g_dec_sel].outcome, DEC_FIELD - 1,
              EF_DEC_OUTCOME, false);
}
static void dec_edit_lesson(lv_event_t *) {
  if (g_dec_sel < 0 || g_dec_sel >= g_decision_count) return;
  open_editor("LESSON", DECISIONS[g_dec_sel].lesson, DEC_FIELD - 1,
              EF_DEC_LESSON, false);
}

static void dec_cycle_confidence(lv_event_t *) {
  if (g_dec_sel < 0 || g_dec_sel >= g_decision_count) return;
  Decision &d = DECISIONS[g_dec_sel];
  d.confidence = (uint8_t)(d.confidence >= 5 ? 1 : d.confidence + 1);
  save_decisions();
  render();
}

static void dec_delete_cb(lv_event_t *) {
  if (g_dec_sel < 0 || g_dec_sel >= g_decision_count) return;
  confirm_ask("Delete this decision and its lesson?", CA_DEL_DECISION,
              DECISIONS[g_dec_sel].id);
}

static lv_obj_t *build_ddecision() {
  if (g_dec_sel < 0 || g_dec_sel >= g_decision_count) return build_decisions();
  Decision &d = DECISIONS[g_dec_sel];
  lv_obj_t *s = scr_new();
  header(s, "DECISION", true);
  lv_obj_t *b = body(s, 64, 428);

  field(b, "DECISION", d.title, LCD_W - 36, 56, dec_edit_title, nullptr);
  field(b, "OPTIONS", d.options, LCD_W - 36, 66, dec_edit_options, nullptr);
  field(b, "WHY", d.rationale, LCD_W - 36, 66, dec_edit_rationale, nullptr);

  lv_obj_t *cf = lv_btn_create(b);
  lv_obj_set_width(cf, LCD_W - 36);
  lv_obj_set_height(cf, 50);
  style_btn(cf, COL_AMBER);
  label(cf, "CONFIDENCE", &lv_font_montserrat_12, COL_AMBER, LV_ALIGN_TOP_LEFT,
        12, 6);
  char cbuf[16];
  snprintf(cbuf, sizeof(cbuf), "%u / 5", (unsigned)d.confidence);
  label(cf, cbuf, &lv_font_montserrat_16, COL_CREAM, LV_ALIGN_TOP_LEFT, 12, 24);
  lv_obj_add_event_cb(cf, dec_cycle_confidence, LV_EVENT_CLICKED, nullptr);

  field(b, "EXPECTED", d.expected, LCD_W - 36, 62, dec_edit_expected, nullptr);
  field(b, "OUTCOME", d.outcome, LCD_W - 36, 62, dec_edit_outcome, nullptr);
  field(b, "LESSON", d.lesson, LCD_W - 36, 62, dec_edit_lesson, nullptr);

  action_button(b, "DELETE DECISION", LCD_W - 36, 44, COL_THREAD,
                dec_delete_cb, nullptr);
  return s;
}

// ---------------------------------------------------------------------------
//  Focus timer
// ---------------------------------------------------------------------------
static bool g_focus_run = false;
static uint32_t g_focus_end = 0;
static uint32_t g_focus_total = 0;
static int g_focus_minutes = 25;

static void focus_start(int minutes) {
  g_focus_minutes = minutes;
  g_focus_total = (uint32_t)minutes * 60000UL;
  g_focus_end = millis() + g_focus_total;
  g_focus_run = true;
  toast("FOCUS STARTED");
  render();
}

static void focus_stop() {
  g_focus_run = false;
  g_focus_total = 0;
  toast("FOCUS STOPPED");
  render();
}

static void focus_pick_cb(lv_event_t *e) {
  focus_start((int)(intptr_t)lv_event_get_user_data(e));
}

static void focus_toggle_cb(lv_event_t *) {
  if (g_focus_run) {
    focus_stop();
  } else {
    focus_start(g_focus_minutes);
  }
}

static lv_obj_t *build_focus() {
  lv_obj_t *s = scr_new();
  header(s, "FOCUS", true);
  lv_obj_t *b = body(s, 64, 428);

  g_focus_label = label(b, "25:00", &lv_font_montserrat_48, COL_CREAM,
                        LV_ALIGN_TOP_LEFT, 2, 10);
  (void)g_focus_label;
  lv_obj_t *sub = label(b, "one thing, no notifications", &lv_font_montserrat_12,
                        COL_MUTED, LV_ALIGN_TOP_LEFT, 2, 0);
  (void)sub;

  lv_obj_t *row = flex_row(b, 50);
  int mins[3] = {25, 45, 60};
  for (int i = 0; i < 3; i++) {
    char t[12];
    snprintf(t, sizeof(t), "%d MIN", mins[i]);
    lv_obj_t *btn = lv_btn_create(row);
    lv_obj_set_size(btn, 110, 46);
    style_btn(btn, g_focus_minutes == mins[i] ? COL_AMBER : COL_LINE);
    label(btn, t, &lv_font_montserrat_12, COL_CREAM, LV_ALIGN_CENTER, 0, 0);
    lv_obj_add_event_cb(btn, focus_pick_cb, LV_EVENT_CLICKED,
                        (void *)(intptr_t)mins[i]);
  }

  action_button(b, g_focus_run ? "STOP" : "START", LCD_W - 36, 52,
                g_focus_run ? COL_THREAD : COL_AMBER, focus_toggle_cb, nullptr);
  empty_note(b, "The timer does not block the watch. The screen still sleeps "
                "after 15 seconds of no touch.");
  return s;
}

// ---------------------------------------------------------------------------
//  System
// ---------------------------------------------------------------------------
static void sys_backup_cb(lv_event_t *) {
  int n = backup_now();
  toast(n ? "BACKUP WRITTEN" : "BACKUP FAILED");
  render();
}

static void sys_export_cb(lv_event_t *) {
  int bytes = export_now();
  toast(bytes ? "EXPORTED TO /EXPORTS" : "EXPORT FAILED");
  render();
}

static void sys_restore_cb(lv_event_t *) {
  if (latest_backup_id() == 0) {
    toast("NO BACKUP FOUND");
    return;
  }
  confirm_ask("Restore the newest backup? Current stores are replaced.",
              CA_RESTORE, 0);
}

static void sys_reload_cb(lv_event_t *) {
  load_all_stores();
  toast("RELOADED FROM SD");
  render();
}

static void voice_row_cb(lv_event_t *e) {
  g_sel = (int)(intptr_t)lv_event_get_user_data(e);
  open_view(V_DETAIL, 0);
}

static lv_obj_t *build_voice() {
  lv_obj_t *s = scr_new();
  header(s, "VOICE NOTES", true);
  lv_obj_t *b = body(s, 64, 376);

  bool es7210 = g_sd_ok && i2c_present(0x40);
  bool es8311 = g_sd_ok && i2c_present(0x18);
  char cap[96];
  snprintf(cap, sizeof(cap), "mic ES7210 %s · speaker ES8311 %s%s",
           es7210 ? "found" : "absent", es8311 ? "found" : "absent",
           es7210 ? "" : " — on-device recording unsupported");
  lv_obj_t *cl = label(b, cap, &lv_font_montserrat_12,
                       es7210 ? COL_MUTED : COL_THREAD, LV_ALIGN_TOP_LEFT, 2, 0);
  lv_obj_set_width(cl, LCD_W - 44);
  lv_label_set_long_mode(cl, LV_LABEL_LONG_WRAP);

  int shown = 0;
  for (int i = 0; i < g_record_count; i++) {
    int v = voice_for_record(RECORDS[i].id);
    if (v < 0) continue;
    char meta[96], when[24];
    fmt_dateonly(when, sizeof(when), RECORDS[i].created);
    snprintf(meta, sizeof(meta), "%us · %s · %s", g_voice_refs[v].sec, when,
             strcmp(RECORDS[i].body, "Voice note") == 0 ? "audio only"
                                                        : "transcript");
    list_row(b, RECORDS[i].title, meta, COL_AMBER, voice_row_cb,
             (void *)(intptr_t)i);
    shown++;
  }
  if (!shown)
    empty_note(b, "No voice notes yet. A WAV in /WATCH/Mentalist/VOICE becomes "
                  "evidence once you import it.");
  action_button(b, "IMPORT WAV FROM SD", LCD_W - 36, 50, COL_AMBER,
                voice_import_cb, nullptr);
  action_button(b, "BASIC VOCABULARY", LCD_W - 36, 50, COL_LINE, vocab_open_cb,
                nullptr);
  return s;
}

static void import_wav_cb(lv_event_t *e) {
  int i = (int)(intptr_t)lv_event_get_user_data(e);
  if (i < 0 || i >= g_wav_count) return;
  char path[96];
  snprintf(path, sizeof(path), OS_FOLDER "/VOICE/%s", g_wav_names[i]);
  int sec = wav_seconds(path);
  if (sec <= 0) {
    toast("NOT A READABLE WAV");
    return;
  }
  char tpath[96], bodyB[BODY_LEN];
  bodyB[0] = 0;
  wav_transcript_path(tpath, sizeof(tpath), g_wav_names[i]);
  if (SD_MMC.exists(tpath)) {
    File tf = SD_MMC.open(tpath);
    if (tf) {
      size_t tsize = tf.size();
      tf.close();
      String tr;
      if (tsize > 0 && tsize < 8192 && read_all(tpath, tr))
        flatten_text(tr.c_str(), bodyB, sizeof(bodyB));
    }
  }
  int unknown = 0;
  if (bodyB[0]) {
    char vb[BODY_LEN];
    vocab_apply(bodyB, vb, sizeof(vb), &unknown);
    snprintf(bodyB, sizeof(bodyB), "%s", vb);
  }
  bool has_tr = bodyB[0] != 0;
  if (g_voice_capture) {
    g_voice_capture = false;
    if (!has_tr) {  // never invent a note from audio nobody transcribed
      toast("NO TRANSCRIPT FOR THAT AUDIO");
      open_view(V_VIMPORT, 0);
      return;
    }
    copy_field(g_draft.body, sizeof(g_draft.body), bodyB);
    snprintf(g_draft.title, sizeof(g_draft.title), "%.*s", TITLE_LEN - 1, bodyB);
    copy_field(g_draft.source, sizeof(g_draft.source), "voice transcript");
    copy_field(g_pending_wav, sizeof(g_pending_wav), g_wav_names[i]);
    g_pending_sec = (uint16_t)sec;
    g_draft.dirty = true;
    capture_store_draft();
    toast("TRANSCRIPT LOADED");
    open_view(V_CAPTURE, 0);
    return;
  }
  char title[TITLE_LEN];
  if (has_tr)
    snprintf(title, sizeof(title), "%.43s", bodyB);
  else
    snprintf(title, sizeof(title), "VOICE %.24s", g_wav_names[i]);
  int idx = create_record(TYPE_OBSERVATION, title,
                          has_tr ? bodyB : "Voice note", "",
                          has_tr ? "voice transcript" : "sd audio", 0);
  if (idx < 0) {
    toast(g_store_error[0] ? g_store_error : "SAVE FAILED");
    return;
  }
  if (!voice_attach(RECORDS[idx].id, g_wav_names[i], (uint16_t)sec)) {
    toast("VOICE INDEX FULL");
    return;
  }
  save_records();
  char msg[48];
  if (has_tr)
    snprintf(msg, sizeof(msg), "VOICE NOTE · %d UNKNOWN", unknown);
  else
    snprintf(msg, sizeof(msg), "VOICE NOTE ADDED");
  toast(msg);
  g_sel = idx;
  open_view(V_DETAIL, 0);
}

static lv_obj_t *build_vimport() {
  lv_obj_t *s = scr_new();
  header(s, "IMPORT AUDIO", true);
  lv_obj_t *b = body(s, 64, 376);
  scan_wavs();
  if (!g_sd_ok) {
    empty_note(b, "SD card unavailable, so there is nothing to read.");
    return s;
  }
  if (g_wav_count == 0) {
    empty_note(b, "No unclaimed WAV files in /WATCH/Mentalist/VOICE. Copy one "
                  "there over USB, then come back to this screen.");
    return s;
  }
  char meta[80];
  for (int i = 0; i < g_wav_count; i++) {
    snprintf(meta, sizeof(meta), "%s · tap to add",
             wav_has_transcript(g_wav_names[i]) ? "transcript found"
                                                : "audio only");
    list_row(b, g_wav_names[i], meta, COL_AMBER, import_wav_cb,
             (void *)(intptr_t)i);
  }
  return s;
}

static void voice_import_cb(lv_event_t *) {
  g_voice_capture = false;
  open_view(V_VIMPORT, 0);
}
static void vocab_open_cb(lv_event_t *) { open_view(V_VOCAB, 0); }

static void vocab_tier_cb(lv_event_t *) {
  g_vocab_extended = !g_vocab_extended;
  vocab_settings_save();
  toast(g_vocab_extended ? "EXTENDED WORD LIST" : "BASIC WORD LIST");
  render();
}

static void vocab_toggle_cb(lv_event_t *) {
  g_strict_vocab = !g_strict_vocab;
  vocab_settings_save();
  toast(g_strict_vocab ? "BASIC VOCAB ONLY" : "FULL TRANSCRIPT");
  render();
}

static lv_obj_t *build_vocab() {
  lv_obj_t *s = scr_new();
  header(s, "VOCABULARY", true);
  lv_obj_t *b = body(s, 64, 376);

  char line[80];
  snprintf(line, sizeof(line), "%d words in the %s list", vocab_size(),
           g_vocab_extended ? "extended" : "basic");
  label(b, line, &lv_font_montserrat_14, COL_CREAM, LV_ALIGN_TOP_LEFT, 2, 0);

  action_button(b,
                g_vocab_extended ? "WORD LIST: EXTENDED" : "WORD LIST: BASIC",
                LCD_W - 36, 50, g_vocab_extended ? COL_AMBER : COL_LINE,
                vocab_tier_cb, nullptr);

  action_button(b, g_strict_vocab ? "ONLY LISTED WORDS: ON"
                                  : "ONLY LISTED WORDS: OFF",
                LCD_W - 36, 50, g_strict_vocab ? COL_AMBER : COL_LINE,
                vocab_toggle_cb, nullptr);

  lv_obj_t *hint = label(b,
                         g_strict_vocab
                             ? "Words outside the list become **** so a note never "
                               "claims to have heard what it cannot spell."
                             : "Transcripts are kept as written. Turn this on to "
                               "replace words outside the list with ****.",
                         
                         &lv_font_montserrat_12, COL_MUTED, LV_ALIGN_TOP_LEFT, 2, 0);
  lv_obj_set_width(hint, LCD_W - 44);
  lv_label_set_long_mode(hint, LV_LABEL_LONG_WRAP);

  label(b, "LAST IMPORT", &lv_font_montserrat_12, COL_AMBER, LV_ALIGN_TOP_LEFT,
        2, 0);
  if (g_last_unknown_n == 0) {
    empty_note(b, "No words outside the list in the last import, or nothing "
                  "imported yet this session.");
  } else {
    char meta[48];
    for (int i = 0; i < g_last_unknown_n; i++) {
      snprintf(meta, sizeof(meta), "%d in last transcript",
               g_last_unknown_hits[i]);
      list_row(b, g_last_unknown[i], meta, COL_THREAD, nullptr, nullptr);
    }
  }
  return s;
}

static char g_tag_names[32][24];
static int g_tag_counts[32];
static int g_tag_total = 0;

static void open_tag_view(const char *tag) {
  snprintf(g_tag_filter, sizeof(g_tag_filter), "%s", tag ? tag : "");
  snprintf(g_filter_title, sizeof(g_filter_title), "TAG  %s", tag ? tag : "");
  g_filter_mask = 0;
  g_filter_type = -1;
  g_filter_status = -1;
  g_filter_case = 0;
  g_tag_keep = true;
  open_view(V_RECORDS, 0);
}

static void collect_tags() {
  g_tag_total = 0;
  for (int i = 0; i < g_record_count; i++) {
    const char *t = RECORDS[i].tags;
    while (t && *t) {
      while (*t == ' ' || *t == ',' || *t == ';') t++;
      const char *start = t;
      while (*t && *t != ',' && *t != ';') t++;
      const char *end = t;
      while (end > start && end[-1] == ' ') end--;
      int len = (int)(end - start);
      if (len <= 0) continue;
      if (len > 22) len = 22;
      char name[24];
      memcpy(name, start, len);
      name[len] = 0;
      int found = -1;
      for (int k = 0; k < g_tag_total; k++)
        if (same_tag(g_tag_names[k], name)) {
          found = k;
          break;
        }
      if (found >= 0) {
        g_tag_counts[found]++;
        continue;
      }
      if (g_tag_total >= 32) continue;
      snprintf(g_tag_names[g_tag_total], sizeof(g_tag_names[0]), "%s", name);
      g_tag_counts[g_tag_total] = 1;
      g_tag_total++;
    }
  }
  for (int i = 1; i < g_tag_total; i++) {
    int k = i;
    while (k > 0 && g_tag_counts[k - 1] < g_tag_counts[k]) {
      int c = g_tag_counts[k];
      g_tag_counts[k] = g_tag_counts[k - 1];
      g_tag_counts[k - 1] = c;
      char tmp[24];
      memcpy(tmp, g_tag_names[k], 24);
      memcpy(g_tag_names[k], g_tag_names[k - 1], 24);
      memcpy(g_tag_names[k - 1], tmp, 24);
      k--;
    }
  }
}

static void tag_row_cb(lv_event_t *e) {
  int i = (int)(intptr_t)lv_event_get_user_data(e);
  if (i < 0 || i >= g_tag_total) return;
  open_tag_view(g_tag_names[i]);
}

static lv_obj_t *build_tags() {
  lv_obj_t *s = scr_new();
  header(s, "TAGS", true);
  lv_obj_t *b = body(s, 64, 376);
  collect_tags();
  char meta[48];
  for (int i = 0; i < g_tag_total; i++) {
    snprintf(meta, sizeof(meta), "%d note%s carrying this tag",
             g_tag_counts[i], g_tag_counts[i] == 1 ? "" : "s");
    list_row(b, g_tag_names[i], meta, COL_AMBER, tag_row_cb,
             (void *)(intptr_t)i);
  }
  if (g_tag_total == 0)
    empty_note(b, "No tags yet. Tags typed while capturing gather here, "
                  "heaviest first.");
  return s;
}

// ---------------------------------------------------------------------------
//  Resurface - old evidence comes back and asks for a verdict
// ---------------------------------------------------------------------------
static int g_resurface_idx = -1;

static int pick_resurface(int avoid) {
  uint32_t now = (uint32_t)time(nullptr);
  bool clock_ok = now >= (uint32_t)TIME_VALID_MIN;
  int chosen = -1, pool = 0;
  for (int i = 0; i < g_record_count; i++) {
    if (i == avoid) continue;
    if (RECORDS[i].status != ST_UNVERIFIED) continue;
    if (status_slots(RECORDS[i].type) <= (int)ST_CONTRADICTED) continue;
    uint32_t t = RECORDS[i].updated ? RECORDS[i].updated : RECORDS[i].created;
    if (clock_ok && t >= (uint32_t)TIME_VALID_MIN &&
        (int32_t)(now - t) < 7L * 86400L)
      continue;  // too fresh to be worth a second look
    pool++;
    if ((esp_random() % (uint32_t)pool) == 0) chosen = i;
  }
  if (chosen >= 0) return chosen;
  pool = 0;
  for (int i = 0; i < g_record_count; i++) {
    if (i == avoid) continue;
    if (status_slots(RECORDS[i].type) <= (int)ST_CONTRADICTED) continue;
    pool++;
    if ((esp_random() % (uint32_t)pool) == 0) chosen = i;
  }
  return chosen;
}

static lv_obj_t *resurface_btn(lv_obj_t *parent, const char *text,
                               uint32_t color, lv_event_cb_t cb) {
  lv_obj_t *bt = lv_btn_create(parent);
  lv_obj_set_width(bt, LCD_W - 36);
  lv_obj_set_height(bt, 46);
  style_btn(bt, color);
  label(bt, text, &lv_font_montserrat_14, COL_CREAM, LV_ALIGN_CENTER, 0, 0);
  lv_obj_add_event_cb(bt, cb, LV_EVENT_CLICKED, nullptr);
  return bt;
}

static void resurface_mark(uint8_t status) {
  int idx = g_resurface_idx;
  if (idx < 0 || idx >= g_record_count) return;
  int slots = status_slots(RECORDS[idx].type);
  int v = (int)status;
  if (v >= slots) v = slots - 1;
  RECORDS[idx].status = (uint8_t)v;
  RECORDS[idx].updated = (uint32_t)time(nullptr);
  save_records();
  toast(v == (int)ST_CORROBORATED ? "STILL TRUE" : "MARKED DOUBTFUL");
  g_resurface_idx = pick_resurface(idx);
  render();
}

static void resurface_true_cb(lv_event_t *) { resurface_mark(ST_CORROBORATED); }
static void resurface_doubt_cb(lv_event_t *) { resurface_mark(ST_CONTRADICTED); }
static void resurface_next_cb(lv_event_t *) {
  g_resurface_idx = pick_resurface(g_resurface_idx);
  render();
}
static void resurface_open_cb(lv_event_t *) {
  g_resurface_idx = -1;
  open_view(V_RESURFACE, 0);
}

static lv_obj_t *build_resurface() {
  lv_obj_t *s = scr_new();
  header(s, "RESURFACE", true);
  lv_obj_t *b = body(s, 64, 376);
  if (g_resurface_idx < 0 || g_resurface_idx >= g_record_count)
    g_resurface_idx = pick_resurface(-1);
  if (g_resurface_idx < 0) {
    empty_note(b, "Nothing to resurface yet. Capture a little evidence and the "
                  "older notes come back here for a verdict.");
    return s;
  }
  Record &r = RECORDS[g_resurface_idx];
  lv_obj_t *card = lv_obj_create(b);
  lv_obj_set_width(card, LCD_W - 36);
  lv_obj_set_height(card, 252);
  style_panel(card);
  lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

  char meta[96], when[24];
  fmt_dateonly(when, sizeof(when), r.created);
  snprintf(meta, sizeof(meta), "%s · %s · %s", TYPE_NAMES[r.type],
           status_name(r.type, r.status), when);
  label(card, meta, &lv_font_montserrat_12, COL_AMBER, LV_ALIGN_TOP_LEFT, 14,
        12);
  lv_obj_t *tl = label(card, r.title, &lv_font_montserrat_16, COL_CREAM,
                       LV_ALIGN_TOP_LEFT, 14, 36);
  lv_obj_set_width(tl, LCD_W - 72);
  lv_label_set_long_mode(tl, LV_LABEL_LONG_WRAP);
  lv_obj_t *bd = label(card, r.body[0] ? r.body : "(no detail recorded)",
                       &lv_font_montserrat_12, COL_CREAM, LV_ALIGN_TOP_LEFT,
                       14, 104);
  lv_obj_set_width(bd, LCD_W - 72);
  lv_label_set_long_mode(bd, LV_LABEL_LONG_WRAP);
  if (r.source[0] || r.tags[0]) {
    snprintf(meta, sizeof(meta), "%s%s%s", r.source[0] ? "source " : "",
             r.source, r.tags[0] ? "" : "");
    if (r.tags[0]) {
      char both[96];
      snprintf(both, sizeof(both), "%s%s%s%s", r.source[0] ? "source " : "",
               r.source, r.source[0] ? "  ·  " : "#", r.tags);
      label(card, both, &lv_font_montserrat_12, COL_MUTED, LV_ALIGN_TOP_LEFT,
            14, 210);
      lv_obj_set_width(lv_obj_get_child(card, lv_obj_get_child_cnt(card) - 1),
                       LCD_W - 72);
    } else {
      label(card, meta, &lv_font_montserrat_12, COL_MUTED, LV_ALIGN_TOP_LEFT,
            14, 210);
    }
  }

  resurface_btn(b, "STILL TRUE", COL_AMBER, resurface_true_cb);
  resurface_btn(b, "MARK DOUBTFUL", COL_THREAD, resurface_doubt_cb);
  resurface_btn(b, "ANOTHER NOTE", COL_LINE, resurface_next_cb);
  return s;
}

static void insight_bar(lv_obj_t *parent, const char *caption, int value,
                        int max_value, uint32_t color) {
  lv_obj_t *b = lv_obj_create(parent);
  lv_obj_set_width(b, LCD_W - 36);
  lv_obj_set_height(b, 34);
  style_panel(b);
  lv_obj_clear_flag(b, LV_OBJ_FLAG_SCROLLABLE);
  int span = LCD_W - 40;
  int fillw = (max_value > 0) ? (span * value) / max_value : 0;
  if (value > 0 && fillw < 4) fillw = 4;
  lv_obj_t *fill = lv_obj_create(b);
  lv_obj_set_pos(fill, 0, 0);
  lv_obj_set_size(fill, fillw, 34);
  lv_obj_set_style_bg_color(fill, lv_color_hex(color), 0);
  lv_obj_set_style_bg_opa(fill, LV_OPA_30, 0);
  lv_obj_set_style_border_width(fill, 0, 0);
  lv_obj_set_style_radius(fill, 16, 0);
  lv_obj_clear_flag(fill, LV_OBJ_FLAG_SCROLLABLE);
  label(b, caption, &lv_font_montserrat_12, COL_CREAM, LV_ALIGN_LEFT_MID, 12, 0);
  char num[16];
  snprintf(num, sizeof(num), "%d", value);
  label(b, num, &lv_font_montserrat_14, COL_AMBER, LV_ALIGN_RIGHT_MID, -12, 0);
}

static lv_obj_t *build_insights() {
  lv_obj_t *s = scr_new();
  header(s, "INSIGHTS", true);
  lv_obj_t *b = body(s, 64, 428);

  uint32_t now = (uint32_t)time(nullptr);
  bool clock_ok = now >= (uint32_t)TIME_VALID_MIN;

  int corroborated = 0, contradicted = 0, unverified = 0, stale = 0;
  int week = 0;
  for (int i = 0; i < g_record_count; i++) {
    if (RECORDS[i].type != TYPE_QUESTION) {
      if (RECORDS[i].status == ST_CORROBORATED) corroborated++;
      else if (RECORDS[i].status == ST_CONTRADICTED) contradicted++;
      else unverified++;
    }
    if (record_is_stale(i, now)) stale++;
    if (clock_ok) {
      uint32_t t = RECORDS[i].updated ? RECORDS[i].updated : RECORDS[i].created;
      if ((int32_t)(now - t) <= 7L * 86400L) week++;
    }
  }

  char head[96];
  snprintf(head, sizeof(head), "%d notes · %d cases · %d threads",
           g_record_count, g_case_count, g_link_count);
  lv_obj_t *hl = label(b, head, &lv_font_montserrat_14, COL_CREAM,
                       LV_ALIGN_TOP_LEFT, 2, 0);
  (void)hl;
  resurface_btn(b, "RESURFACE AN OLD NOTE", COL_LINE, resurface_open_cb);

  label(b, "EVIDENCE BALANCE", &lv_font_montserrat_12, COL_AMBER,
        LV_ALIGN_TOP_LEFT, 2, 0);
  int maxv = corroborated;
  if (contradicted > maxv) maxv = contradicted;
  if (unverified > maxv) maxv = unverified;
  if (maxv < 1) maxv = 1;
  insight_bar(b, "corroborated", corroborated, maxv, COL_AMBER);
  insight_bar(b, "contradicted", contradicted, maxv, COL_THREAD);
  insight_bar(b, "unverified", unverified, maxv, COL_MUTED);

  label(b, "MOST USED TYPES", &lv_font_montserrat_12, COL_AMBER,
        LV_ALIGN_TOP_LEFT, 2, 0);
  int counts[TYPE_COUNT];
  for (int t = 0; t < TYPE_COUNT; t++) counts[t] = 0;
  for (int i = 0; i < g_record_count; i++) {
    if (RECORDS[i].type < TYPE_COUNT) counts[RECORDS[i].type]++;
  }
  int tmax = 1;
  for (int t = 0; t < TYPE_COUNT; t++)
    if (counts[t] > tmax) tmax = counts[t];
  for (int k = 0; k < 4; k++) {
    int best = -1;
    for (int t = 0; t < TYPE_COUNT; t++) {
      if (counts[t] <= 0) continue;
      if (best < 0 || counts[t] > counts[best]) best = t;
    }
    if (best < 0) break;
    insight_bar(b, TYPE_NAMES[best], counts[best], tmax, COL_LINE);
    counts[best] = 0;
  }
  if (g_record_count == 0)
    empty_note(b, "Nothing counted yet. Capture a few notes and the balance "
                  "starts to mean something.");

  label(b, "THIS WEEK", &lv_font_montserrat_12, COL_AMBER, LV_ALIGN_TOP_LEFT, 2,
        0);
  char wk[80];
  snprintf(wk, sizeof(wk), "%d touched%s · %d due · %d stale",
           week, clock_ok ? " in 7 days" : "", card_due_count(), stale);
  lv_obj_t *wl = label(b, wk, &lv_font_montserrat_12, COL_CREAM,
                       LV_ALIGN_TOP_LEFT, 2, 0);
  lv_obj_set_width(wl, LCD_W - 44);
  lv_label_set_long_mode(wl, LV_LABEL_LONG_WRAP);
  return s;
}

static void maybe_auto_backup() {
  if (!g_sd_ok) return;
  uint32_t now = (uint32_t)time(nullptr);
  if (now < (uint32_t)TIME_VALID_MIN) return;  // no clock, no schedule
  uint32_t last = latest_backup_id();
  if (last && (int32_t)(now - last) < 7L * 86400L) return;
  int n = backup_now();
  if (n > 0) toast("WEEKLY BACKUP SAVED");
}

static void sys_timeout_cb(lv_event_t *) {
  int idx = 1;
  for (int i = 0; i < 4; i++)
    if (g_screen_off_ms == SCREEN_TIMEOUT_OPTS[i]) idx = i;
  g_screen_off_ms = SCREEN_TIMEOUT_OPTS[(idx + 1) % 4];
  Preferences p;
  p.begin("mentalist", false);
  p.putULong("soff", g_screen_off_ms);
  p.end();
  char m[24];
  snprintf(m, sizeof(m), "SCREEN %lus", (unsigned long)(g_screen_off_ms / 1000));
  toast(m);
  render();
}

static void sys_input_cb(lv_event_t *) {
  g_voice_input = !g_voice_input;
  vocab_settings_save();
  toast(g_voice_input ? "VOICE INPUT ONLY" : "KEYBOARD INPUT");
  render();
}

static lv_obj_t *build_system() {
  lv_obj_t *s = scr_new();
  header(s, "SYSTEM", true);
  lv_obj_t *b = body(s, 64, 428);

  char buf[80];
  snprintf(buf, sizeof(buf), "%s %s · %s", OS_MARK, OS_VERSION,
           g_sd_ok ? "SD mounted" : "SD unavailable");
  lv_obj_t *l = label(b, buf, &lv_font_montserrat_14, COL_CREAM,
                      LV_ALIGN_TOP_LEFT, 2, 0);
  lv_obj_set_width(l, LCD_W - 44);

  snprintf(buf, sizeof(buf), "%d records · %d cases · %d threads", g_record_count,
           g_case_count, g_link_count);
  lv_obj_t *l2 = label(b, buf, &lv_font_montserrat_12, COL_MUTED,
                       LV_ALIGN_TOP_LEFT, 2, 0);
  (void)l2;

  snprintf(buf, sizeof(buf),
           "%d cards · %d decisions\n"
           "heap %lu KB free · min %lu KB\n"
           "psram %lu KB free · stores %s",
           g_card_count, g_decision_count,
           (unsigned long)(ESP.getFreeHeap() / 1024),
           (unsigned long)(heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL) / 1024),
           (unsigned long)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024),
           g_stores_ok ? "external" : "internal");
  lv_obj_t *l3 = label(b, buf, &lv_font_montserrat_12, COL_MUTED,
                       LV_ALIGN_TOP_LEFT, 2, 0);
  (void)l3;

  char when[24];
  fmt_datetime(when, sizeof(when), (uint32_t)time(nullptr));
  snprintf(buf, sizeof(buf), "clock %s · %s", when,
           time(nullptr) >= (time_t)TIME_VALID_MIN ? "set" : "unset");
  lv_obj_t *l4 = label(b, buf, &lv_font_montserrat_12, COL_AMBER,
                       LV_ALIGN_TOP_LEFT, 2, 0);
  (void)l4;

  snprintf(buf, sizeof(buf), "boot partition: %s",
           esp_ota_get_running_partition() ? esp_ota_get_running_partition()->label
                                           : "?");
  lv_obj_t *l5 = label(b, buf, &lv_font_montserrat_12, COL_MUTED,
                       LV_ALIGN_TOP_LEFT, 2, 0);
  (void)l5;

  if (g_store_error[0]) {
    snprintf(buf, sizeof(buf), "last error: %s", g_store_error);
    lv_obj_t *e = label(b, buf, &lv_font_montserrat_12, COL_THREAD,
                        LV_ALIGN_TOP_LEFT, 2, 0);
    lv_obj_set_width(e, LCD_W - 44);
    lv_label_set_long_mode(e, LV_LABEL_LONG_WRAP);
  }

  if (g_damaged_lines > 0) {
    snprintf(buf, sizeof(buf), "%d unreadable lines skipped on load",
             g_damaged_lines);
    lv_obj_t *dm = label(b, buf, &lv_font_montserrat_12, COL_THREAD,
                         LV_ALIGN_TOP_LEFT, 2, 0);
    lv_obj_set_width(dm, LCD_W - 44);
  }

  lv_obj_t *row = flex_row(b, 50);
  lv_obj_t *bk = lv_btn_create(row);
  lv_obj_set_size(bk, 170, 46);
  style_btn(bk, COL_AMBER);
  label(bk, "BACKUP NOW", &lv_font_montserrat_12, COL_CREAM, LV_ALIGN_CENTER, 0,
        0);
  lv_obj_add_event_cb(bk, sys_backup_cb, LV_EVENT_CLICKED, nullptr);
  lv_obj_t *ex = lv_btn_create(row);
  lv_obj_set_size(ex, 170, 46);
  style_btn(ex, COL_AMBER);
  label(ex, "EXPORT TEXT", &lv_font_montserrat_12, COL_CREAM, LV_ALIGN_CENTER,
        0, 0);
  lv_obj_add_event_cb(ex, sys_export_cb, LV_EVENT_CLICKED, nullptr);

  action_button(b, "RESTORE NEWEST BACKUP", LCD_W - 36, 44, COL_LINE,
                sys_restore_cb, nullptr);
  action_button(b, "RELOAD FROM SD", LCD_W - 36, 44, COL_LINE, sys_reload_cb,
                nullptr);
  {
    char to[44];
    snprintf(to, sizeof(to), "SLEEP AFTER   %lus · DEEP +2m",
             (unsigned long)(g_screen_off_ms / 1000));
    action_button(b, to, LCD_W - 36, 44, COL_AMBER, sys_timeout_cb, nullptr);
  }
  action_button(b, g_voice_input ? "INPUT: VOICE ONLY" : "INPUT: KEYBOARD",
                LCD_W - 36, 44, g_voice_input ? COL_AMBER : COL_LINE,
                sys_input_cb, nullptr);
  action_button(b, "CLOCK FACE", LCD_W - 36, 44, COL_LINE, clock_to_face_cb,
                nullptr);
  action_button(b, "RETURN TO LAUNCHEROS", LCD_W - 36, 50, COL_AMBER,
                [](lv_event_t *) { return_to_launcher(); }, nullptr);
  action_button(b, "DEEP SLEEP", LCD_W - 36, 44, COL_LINE,
                [](lv_event_t *) { deep_sleep_now(); }, nullptr);
  return s;
}

// ---------------------------------------------------------------------------
//  Planned (phase) screens
// ---------------------------------------------------------------------------
static const char *PLANNED_TITLE[3] = {"DAILY CHALLENGE", "LOGIC LAB",
                                       "JANE ASSISTANT"};

static const char *PLANNED_BODY[3] = {
    "Not in V1.\n\nThe daily challenge pool — logic, sequences, memory, "
    "arithmetic — follows once the record store and the review flow are "
    "stable on hardware.",
    "Not in V1.\n\nOffline puzzle levels with hints live in phase 5, kept "
    "separate from the journal so the capture path stays fast.",
    "Not in V1.\n\nThis watch never sends a record anywhere. A Jane assistant "
    "would be an explicit, opt-in external connection with a visible "
    "data-transfer indicator and the ability to stay off."};

// ---------------------------------------------------------------------------
//  Clock face - the watch when it is only being a watch
// ---------------------------------------------------------------------------
// LauncherOS prints the time as 24-hour HH:MM and the date as
// WEEKDAY DAY MONTH, uppercase and unpunctuated. The face follows that pattern
// so the two screens read as the same device.
// LauncherOS' pattern, taken from its own face code:
//   time  strftime(g_time_24h ? "%H:%M" : "%I:%M")
//   date  strftime("%a  %d %b")     -- mixed case, two spaces after the weekday
// The 12/24-hour choice is the launcher's, read from NVS "dcw"/"t24" at boot, so
// the two screens never disagree about how the watch tells time.
static void fmt_face_time(char *out, size_t len, const struct tm *t) {
  strftime(out, len, g_face_24h ? "%H:%M" : "%I:%M", t);
}

static void fmt_face_date(char *out, size_t len, const struct tm *t) {
  strftime(out, len, "%a  %d %b", t);
}

static void label_set_if_changed(lv_obj_t *l, char *cache, size_t len,
                                 const char *text) {
  if (strncmp(cache, text, len - 1) == 0) return;
  snprintf(cache, len, "%s", text);
  lv_label_set_text(l, text);
}

static int open_questions() {
  int n = 0;
  for (int i = 0; i < g_record_count; i++) {
    if (RECORDS[i].type != TYPE_QUESTION) continue;
    const char *sn = status_name(RECORDS[i].type, RECORDS[i].status);
    if (strcmp(sn, "Answered") != 0 && strcmp(sn, "Parked") != 0) n++;
  }
  return n;
}

static void clock_open_cb(lv_event_t *) {
  g_view_depth = 0;
  open_view(V_HOME, 0);
}

static void clock_to_face_cb(lv_event_t *) {
  g_view_depth = 0;
  open_view(V_CLOCK, 0);
}

static lv_obj_t *build_clock_face() {
  lv_obj_t *s = scr_new();
  lv_obj_add_flag(s, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(s, clock_open_cb, LV_EVENT_CLICKED, nullptr);
  g_face_last_hm[0] = 0;   // fresh widgets: force the next tick to paint
  g_face_last_dt[0] = 0;
  g_face_last_bat[0] = 0;

  time_t now = time(nullptr);
  char tb[10], db[32];
  if (now >= (time_t)TIME_VALID_MIN) {
    struct tm tmv;
    localtime_r(&now, &tmv);
    fmt_face_time(tb, sizeof(tb), &tmv);
    fmt_face_date(db, sizeof(db), &tmv);
  } else {
    snprintf(tb, sizeof(tb), "--:--");
    snprintf(db, sizeof(db), "no clock set");
  }

  // LauncherOS puts the time high and the date directly under it, with nothing
  // else in the upper third. The app's own counts sit lower, so the screen still
  // reads as a watch first and a workspace second.
  g_face_time = label(s, tb, &lv_font_montserrat_48, COL_CREAM,
                      LV_ALIGN_TOP_MID, 0, 56);
  lv_obj_set_style_text_letter_space(g_face_time, 2, 0);
  lv_obj_set_style_text_opa(g_face_time, 220, 0);
  g_face_date = label(s, db, &lv_font_montserrat_16, COL_AMBER,
                      LV_ALIGN_TOP_MID, 0, 132);
  lv_obj_set_style_text_opa(g_face_date, 200, 0);

  lv_obj_t *rule = lv_obj_create(s);
  lv_obj_set_size(rule, 64, 2);
  lv_obj_align(rule, LV_ALIGN_TOP_MID, 0, 178);
  lv_obj_set_style_bg_color(rule, lv_color_hex(COL_AMBER), 0);
  lv_obj_set_style_bg_opa(rule, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(rule, 0, 0);
  lv_obj_set_style_radius(rule, 1, 0);

  g_face_bat = label(s, "BAT --", &lv_font_montserrat_14, COL_MUTED,
                     LV_ALIGN_TOP_MID, 0, 200);
  {
    char meta[64];
    snprintf(meta, sizeof(meta), "%d notes · %d due · %d open Q", g_record_count,
             card_due_count(), open_questions());
    label(s, meta, &lv_font_montserrat_12, COL_MUTED, LV_ALIGN_TOP_MID, 0, 232);
  }
  label(s, g_sd_ok ? "SD ready" : "SD unavailable", &lv_font_montserrat_12,
        g_sd_ok ? COL_AMBER_DK : COL_THREAD, LV_ALIGN_TOP_MID, 0, 258);

  lv_obj_t *tap = lv_btn_create(s);
  lv_obj_set_size(tap, 230, 58);
  lv_obj_align(tap, LV_ALIGN_TOP_MID, 0, 342);
  style_btn(tap, COL_AMBER);
  label(tap, "TAP TO OPEN", &lv_font_montserrat_16, COL_CREAM, LV_ALIGN_CENTER,
        0, 0);
  lv_obj_add_event_cb(tap, clock_open_cb, LV_EVENT_CLICKED, nullptr);

  label(s, OS_MARK " " OS_VERSION, &lv_font_montserrat_12, COL_MUTED,
        LV_ALIGN_BOTTOM_MID, 0, -44);
  label(s, "hold BOOT 1.5s for LauncherOS", &lv_font_montserrat_12, COL_MUTED,
        LV_ALIGN_BOTTOM_MID, 0, -18);
  return s;
}

static lv_obj_t *build_planned() {
  int idx = g_param;
  if (idx < 0 || idx > 2) idx = 0;
  lv_obj_t *s = scr_new();
  header(s, PLANNED_TITLE[idx], true);
  lv_obj_t *b = body(s, 64, 428);
  lv_obj_t *bodylbl = label(b, PLANNED_BODY[idx], &lv_font_montserrat_14,
                            COL_CREAM, LV_ALIGN_TOP_LEFT, 2, 4);
  lv_obj_set_width(bodylbl, LCD_W - 44);
  lv_label_set_long_mode(bodylbl, LV_LABEL_LONG_WRAP);
  return s;
}

// ---------------------------------------------------------------------------
//  Runtime labels (clock, battery, focus)
// ---------------------------------------------------------------------------
static void focus_tick() {
  char buf[12];
  if (g_focus_run) {
    uint32_t now = millis();
    if ((int32_t)(now - g_focus_end) >= 0) {
      g_focus_run = false;
      g_focus_total = 0;
      if (g_focus_label) lv_label_set_text(g_focus_label, "00:00");
      toast("FOCUS COMPLETE");
      return;
    }
    int32_t left = (int32_t)(g_focus_end - now);
    snprintf(buf, sizeof(buf), "%02d:%02d", (int)(left / 60000),
             (int)((left / 1000) % 60));
  } else {
    snprintf(buf, sizeof(buf), "%02d:00", g_focus_minutes);
  }
  if (g_focus_label) lv_label_set_text(g_focus_label, buf);
}

static void refresh_runtime_labels() {
  char buf[24];
  time_t now = time(nullptr);
  if (now >= (time_t)TIME_VALID_MIN) {
    struct tm tmv;
    localtime_r(&now, &tmv);
    snprintf(buf, sizeof(buf), "%02d:%02d", tmv.tm_hour, tmv.tm_min);
  } else {
    snprintf(buf, sizeof(buf), "--:--");
  }
  if (g_clock_label)
    label_set_if_changed(g_clock_label, g_clock_last, sizeof(g_clock_last),
                         buf);

  if (g_face_time) {
    char hb[10];
    if (now >= (time_t)TIME_VALID_MIN) {
      struct tm tmv;
      localtime_r(&now, &tmv);
      fmt_face_time(hb, sizeof(hb), &tmv);
    } else {
      snprintf(hb, sizeof(hb), "--:--");
    }
    label_set_if_changed(g_face_time, g_face_last_hm, sizeof(g_face_last_hm),
                         hb);
  }
  if (g_face_date) {
    char db[32];
    if (now >= (time_t)TIME_VALID_MIN) {
      struct tm tmv;
      localtime_r(&now, &tmv);
      fmt_face_date(db, sizeof(db), &tmv);
    } else {
      snprintf(db, sizeof(db), "no clock set");
    }
    label_set_if_changed(g_face_date, g_face_last_dt, sizeof(g_face_last_dt),
                         db);
  }
  if (g_face_bat) {
    int fpct = g_has_pmu ? PMU.getBatteryPercent() : -1;
    char fb[16];
    if (fpct < 0) {
      snprintf(fb, sizeof(fb), "BAT --");
    } else {
      snprintf(fb, sizeof(fb), "BAT %d%%", fpct);
    }
    label_set_if_changed(g_face_bat, g_face_last_bat, sizeof(g_face_last_bat),
                         fb);
    lv_obj_set_style_text_color(
        g_face_bat,
        lv_color_hex((fpct >= 0 && fpct <= 10) ? COL_THREAD : COL_MUTED), 0);
  }

  if (g_power_label) {
    int pct = g_has_pmu ? PMU.getBatteryPercent() : -1;
    if (pct < 0) {
      snprintf(buf, sizeof(buf), "BAT --");
    } else {
      snprintf(buf, sizeof(buf), "BAT %d%%", pct);
    }
    lv_label_set_text(g_power_label, buf);
    lv_obj_set_style_text_color(
        g_power_label,
        lv_color_hex((pct >= 0 && pct <= 10) ? COL_THREAD : COL_MUTED), 0);
    if (pct >= 0 && pct <= 10) {
      static uint32_t last_warn = 0;
      if (millis() - last_warn > 300000UL) {
        last_warn = millis();
        toast("BATTERY LOW");
      }
    }
  }

  if (g_focus_label) focus_tick();
}

// ---------------------------------------------------------------------------
//  Navigation
// ---------------------------------------------------------------------------
static void render() {
  lv_obj_t *s = nullptr;
  switch (g_view) {
    case V_AREA: s = build_area(); break;
    case V_CAPTURE: s = build_capture(); break;
    case V_RECORDS: s = build_records(); break;
    case V_DETAIL: s = build_detail(); break;
    case V_CASES: s = build_cases(); break;
    case V_CASE: s = build_case(); break;
    case V_TIMELINE: s = build_timeline(); break;
    case V_WALL: s = build_wall(); break;
    case V_NODE: s = build_node(); break;
    case V_LINKPICK: s = build_linkpick(); break;
    case V_LINKREL: s = build_linkrel(); break;
    case V_QUESTIONS: s = build_questions(); break;
    case V_QDETAIL: s = build_qdetail(); break;
    case V_SEARCH: s = build_search(); break;
    case V_CARDS: s = build_cards(); break;
    case V_CARDEDIT: s = build_cardedit(); break;
    case V_CARDSTUDY: s = build_cardstudy(); break;
    case V_DECISIONS: s = build_decisions(); break;
    case V_DDETAIL: s = build_ddecision(); break;
    case V_FOCUS: s = build_focus(); break;
    case V_SYSTEM: s = build_system(); break;
    case V_INSIGHTS: s = build_insights(); break;
    case V_TAGS: s = build_tags(); break;
    case V_RESURFACE: s = build_resurface(); break;
    case V_VOICE: s = build_voice(); break;
    case V_VIMPORT: s = build_vimport(); break;
    case V_VOCAB: s = build_vocab(); break;
    case V_CLOCK: s = build_clock_face(); break;
    case V_PLANNED: s = build_planned(); break;
    case V_HOME:
    default: s = build_home(); break;
  }
  if (!s) s = build_home();

  lv_obj_t *old = g_cur_screen;
  g_cur_screen = s;
  lv_scr_load(s);
  if (old && old != s) lv_obj_del_async(old);
  refresh_runtime_labels();
}

static void open_view(int v, int param) {
  if (v != V_VIMPORT) g_voice_capture = false;
  g_param = param;
  if (!g_tag_keep) g_tag_filter[0] = 0;
  g_tag_keep = false;
  if (v == V_AREA) g_area = param;
  if (v == V_CAPTURE && param > 0 && param < TYPE_COUNT) capture_begin(param);

  if (v == V_HOME) {
    g_view_depth = 0;
  } else if (g_view_depth < (int)(sizeof(g_view_stack) / sizeof(g_view_stack[0]))) {
    g_view_stack[g_view_depth++] = g_view;
  }
  g_view = v;
  render();
}

static void go_back() {
  if (g_view_depth <= 0) {
    g_view = V_HOME;
    g_param = 0;
    g_view_depth = 0;
    render();
    return;
  }
  g_view = g_view_stack[--g_view_depth];
  g_param = 0;
  render();
}

// ---------------------------------------------------------------------------
//  Confirm execution
// ---------------------------------------------------------------------------
static void run_confirm() {
  int action = g_confirm_action;
  uint32_t ref = g_confirm_ref;
  g_confirm_action = CA_NONE;

  switch (action) {
    case CA_DEL_RECORD: {
      int idx = find_record_index(ref);
      if (idx >= 0) {
        delete_record_at(idx);
        g_sel = -1;
        g_view = V_RECORDS;
        g_view_depth = 0;
        toast("RECORD DELETED");
        render();
        return;
      }
      break;
    }
    case CA_DEL_CASE: {
      int idx = find_case_index(ref);
      if (idx >= 0) {
        delete_case_at(idx);
        g_sel_case = -1;
        g_view = V_CASES;
        g_view_depth = 0;
        toast("CASE DELETED");
        render();
        return;
      }
      break;
    }
    case CA_DEL_LINK: {
      for (int i = 0; i < g_link_count; i++) {
        if (LINKS[i].id != ref) continue;
        delete_link_at(i);
        toast("THREAD REMOVED");
        render();
        return;
      }
      break;
    }
    case CA_DEL_CARD: {
      for (int i = 0; i < g_card_count; i++) {
        if (CARDS[i].id != ref) continue;
        for (int j = i; j < g_card_count - 1; j++) CARDS[j] = CARDS[j + 1];
        g_card_count--;
        save_cards();
        g_card_sel = -1;
        g_view = V_CARDS;
        g_view_depth = 0;
        toast("CARD DELETED");
        render();
        return;
      }
      break;
    }
    case CA_DEL_DECISION: {
      for (int i = 0; i < g_decision_count; i++) {
        if (DECISIONS[i].id != ref) continue;
        for (int j = i; j < g_decision_count - 1; j++)
          DECISIONS[j] = DECISIONS[j + 1];
        g_decision_count--;
        save_decisions();
        g_dec_sel = -1;
        g_view = V_DECISIONS;
        g_view_depth = 0;
        toast("DECISION DELETED");
        render();
        return;
      }
      break;
    }
    case CA_ARCHIVE_CASE: {
      int idx = find_case_index(ref);
      if (idx >= 0) {
        CASES[idx].status = CASE_CLOSED;
        CASES[idx].updated = (uint32_t)time(nullptr);
        save_cases();
        toast("CASE CLOSED");
      }
      break;
    }
    case CA_RESTORE: {
      int n = restore_latest();
      toast(n ? "RESTORED FROM BACKUP" : "RESTORE FAILED");
      break;
    }
    default:
      break;
  }
  render();
}

// ---------------------------------------------------------------------------
//  Actions from the area menus and the home screen
// ---------------------------------------------------------------------------
static void run_action(int a) {
  switch (a) {
    case ACT_CAPTURE_OBS:
      capture_begin(TYPE_OBSERVATION);
      open_view(V_CAPTURE, 0);
      break;
    case ACT_CASES: open_view(V_CASES, 0); break;
    case ACT_TIMELINE: open_view(V_TIMELINE, 0); break;
    case ACT_WALL:
      g_wall_case = 0;
      open_view(V_WALL, 0);
      break;
    case ACT_QUESTIONS: open_view(V_QUESTIONS, 0); break;
    case ACT_CLUES:
      set_filter_mask(0, "CLUE TRACKER");
      open_view(V_RECORDS, 0);
      break;
    case ACT_SEARCH: open_view(V_SEARCH, 0); break;
    case ACT_CARDS: open_view(V_CARDS, 0); break;
    case ACT_BLIND:
    case ACT_ROULETTE:
      if (g_card_count == 0) {
        toast("NO CARDS YET");
        open_view(V_CARDS, 0);
        break;
      }
      g_card_mode = (a == ACT_BLIND) ? 1 : 2;
      g_card_revealed = false;
      g_card_sel = (int)(esp_random() % (uint32_t)g_card_count);
      open_view(V_CARDSTUDY, 0);
      break;
    case ACT_VAULT:
      set_filter_mask((uint16_t)((1u << TYPE_FACT) | (1u << TYPE_CONCEPT) |
                                 (1u << TYPE_SOURCE)),
                      "KNOWLEDGE VAULT");
      open_view(V_RECORDS, 0);
      break;
    case ACT_READING:
      set_filter_mask((uint16_t)(1u << TYPE_READING), "READING JOURNAL");
      open_view(V_RECORDS, 0);
      break;
    case ACT_WORDS:
      set_filter_mask((uint16_t)(1u << TYPE_WORD), "PERSONAL DICTIONARY");
      open_view(V_RECORDS, 0);
      break;
    case ACT_IDEAS:
      set_filter_mask((uint16_t)((1u << TYPE_IDEA) | (1u << TYPE_HYPOTHESIS)),
                      "IDEA COLLECTOR");
      open_view(V_RECORDS, 0);
      break;
    case ACT_PAUSE:
      capture_begin(TYPE_PAUSE);
      open_view(V_CAPTURE, 0);
      break;
    case ACT_PERSPECTIVE:
      capture_begin(TYPE_PERSPECTIVE);
      open_view(V_CAPTURE, 0);
      break;
    case ACT_DECISIONS: open_view(V_DECISIONS, 0); break;
    case ACT_FOCUS: open_view(V_FOCUS, 0); break;
    case ACT_SYSTEM: open_view(V_SYSTEM, 0); break;
    case ACT_INSIGHTS: open_view(V_INSIGHTS, 0); break;
    case ACT_TAGS: open_view(V_TAGS, 0); break;
    case ACT_VOICE: open_view(V_VOICE, 0); break;
    case ACT_RESURFACE:
      g_resurface_idx = -1;
      open_view(V_RESURFACE, 0);
      break;
    case ACT_DAILY: open_view(V_PLANNED, 0); break;
    case ACT_LOGIC: open_view(V_PLANNED, 1); break;
    case ACT_JANE: open_view(V_PLANNED, 2); break;
    default: break;
  }
}

// ---------------------------------------------------------------------------
//  Power, launcher handover, sleep
// ---------------------------------------------------------------------------
static uint8_t g_brightness = 0;
static bool g_screen_off = false;
static bool g_returning = false;
static bool g_touch_active = false;
static uint32_t g_last_touch_ms = 0;
static uint32_t g_last_wake_ms = 0;
static uint32_t g_last_swipe_ms = 0;
static uint32_t g_last_clock_save_ms = 0;
static int g_pending_swipe = 0;
static int32_t g_start_x = 0, g_start_y = 0, g_current_x = 0, g_current_y = 0;
static uint32_t g_start_ms = 0;

static void pump() { lv_timer_handler(); }

static void fade_brightness(uint8_t target, uint32_t duration) {
  int from = g_brightness;
  if (from == target) return;
  for (int i = 1; i <= 12; i++) {
    int value = from + ((int(target) - from) * i) / 12;
    static_cast<Arduino_CO5300 *>(gfx)->setBrightness((uint8_t)value);
    g_brightness = (uint8_t)value;
    pump();
    delay(max(1, (int)duration / 12));
  }
}

static void save_clock() {
  time_publish("mentalist", false);
  g_last_clock_save_ms = millis();
}

static void stop_and_save() {
  if (g_sd_ok) {
    save_all_stores();
    capture_store_draft();
  }
  save_clock();
  WiFi.disconnect(true, true);
  WiFi.mode(WIFI_OFF);
  btStop();
  Preferences p;
  p.begin("mentalist", false);
  p.putUChar("area", (uint8_t)g_area);
  p.putBool("draft", g_draft.dirty);
  p.end();
}

static void return_to_launcher() {
  time_publish("exit", false);
  if (g_returning) return;
  g_returning = true;
  stop_and_save();

  lv_obj_t *cover = lv_obj_create(lv_layer_top());
  lv_obj_set_size(cover, LCD_W, LCD_H);
  lv_obj_align(cover, LV_ALIGN_CENTER, 0, 0);
  lv_obj_set_style_bg_color(cover, lv_color_hex(COL_BG), 0);
  lv_obj_set_style_bg_opa(cover, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(cover, 0, 0);
  lv_obj_set_style_radius(cover, 0, 0);
  label(cover, "SAVING SESSION...", &lv_font_montserrat_22, COL_AMBER,
        LV_ALIGN_CENTER, 0, -20);
  label(cover, "RETURNING TO LAUNCHEROS", &lv_font_montserrat_14, COL_MUTED,
        LV_ALIGN_CENTER, 0, 20);
  pump();

  const esp_partition_t *factory = esp_partition_find_first(
      ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_FACTORY, nullptr);
  if (!factory || esp_ota_set_boot_partition(factory) != ESP_OK) {
    lv_obj_del(cover);
    g_returning = false;
    lv_obj_t *m = lv_msgbox_create(
        nullptr, "RETURN FAILED",
        "Factory LauncherOS partition was not found.\nNo alternate OTA slot "
        "was selected.",
        nullptr, true);
    lv_obj_center(m);
    pump();
    return;
  }
  fade_brightness(0, 280);
  delay(80);
  esp_restart();
}

static void deep_sleep_now() {
  stop_and_save();
  fade_brightness(0, 260);
  esp_sleep_pd_config(ESP_PD_DOMAIN_RTC_PERIPH, ESP_PD_OPTION_ON);
  esp_sleep_enable_ext1_wakeup(1ULL << BOOT_BTN, ESP_EXT1_WAKEUP_ANY_LOW);
  esp_deep_sleep_start();
}

// LauncherOS' screen_held(): a command that owns the screen keeps it lit.
// Here the equivalents are the editor, a confirm dialog and a focus session.
static bool screen_held(uint32_t now) {
  (void)now;
  return g_editor || g_confirm || g_focus_run;
}

// pm_auto_deep() bails while the pack is charging or VBUS is present: "a docked
// watch must not reboot itself every ten minutes". The explicit DEEP SLEEP
// button still works on a charger, because that one is the user's decision.
static bool on_external_power() {
  if (!g_has_pmu) return false;
  return PMU.isCharging() || PMU.isVbusIn();
}

#if ENABLE_LIGHT_SLEEP
// LauncherOS' dark-phase pattern on this board: a 250 ms timer tick sleeps the
// SoC and the touch controller is polled on each tick, because the FT3168's
// TP_INT (GPIO38) is not an RTC pad and cannot be a wake source. ext1 on BOOT
// still wakes instantly, which is the wake the user expects anyway.
static void light_sleep_tick() {
  esp_sleep_enable_ext1_wakeup(1ULL << BOOT_BTN, ESP_EXT1_WAKEUP_ANY_LOW);
  esp_sleep_enable_timer_wakeup((uint64_t)LIGHT_SLEEP_POLL_MS * 1000ULL);
  esp_light_sleep_start();
  esp_sleep_wakeup_cause_t why = esp_sleep_get_wakeup_cause();

  if (why == ESP_SLEEP_WAKEUP_EXT1) {
    FT3168->IIC_Interrupt_Flag = true;
    g_last_touch_ms = millis();
    screen_on();
  } else if (why == ESP_SLEEP_WAKEUP_TIMER && digitalRead(TP_INT) == LOW) {
    FT3168->IIC_Interrupt_Flag = true;
    g_last_touch_ms = millis();
    screen_on();
  }
  // Continuous darkness, stamped in screen_off(): any wake restarts the two
  // minutes, so a watch you just looked at does not deep-sleep seconds later.
  if (g_dark_since && (uint32_t)(millis() - g_dark_since) >= DEEP_SLEEP_MS &&
      !on_external_power())
    deep_sleep_now();  // saves the session first; never returns
}
#endif

// Going dark switches to the face first, then blanks the panel: a watch should
// be showing the time at the moment the screen goes away.
static void enter_dark() {
  if (g_view != V_CLOCK) {
    g_view = V_CLOCK;
    g_view_depth = 0;
    render();
  }
  pump();
  screen_off();
}

static void screen_on() {
  if (!g_screen_off) return;
  g_screen_off = false;
  lv_obj_invalidate(lv_scr_act());
  pump();
  g_dark_since = 0;   // lit again: the dark phase is over
  fade_brightness(DEFAULT_BRIGHTNESS, 200);
  g_last_touch_ms = millis();
  g_last_wake_ms = millis();
}

static void screen_off() {
  if (g_screen_off) return;
  g_screen_off = true;
  g_dark_since = millis();
  fade_brightness(0, 220);
  FT3168->IIC_Interrupt_Flag = false;
}

// ---------------------------------------------------------------------------
//  Display and touch drivers
// ---------------------------------------------------------------------------
static void display_flush(lv_disp_drv_t *drv, const lv_area_t *area,
                          lv_color_t *pixels) {
  uint32_t w = area->x2 - area->x1 + 1;
  uint32_t h = area->y2 - area->y1 + 1;
  gfx->draw16bitRGBBitmap(area->x1, area->y1, (uint16_t *)pixels, w, h);
  lv_disp_flush_ready(drv);
}

void Arduino_IIC_Touch_Interrupt(void) { FT3168->IIC_Interrupt_Flag = true; }

static void touch_read(lv_indev_drv_t *, lv_indev_data_t *data) {
  int32_t x = FT3168->IIC_Read_Device_Value(
      FT3168->Arduino_IIC_Touch::Value_Information::TOUCH_COORDINATE_X);
  int32_t y = FT3168->IIC_Read_Device_Value(
      FT3168->Arduino_IIC_Touch::Value_Information::TOUCH_COORDINATE_Y);
  int fingers = FT3168->IIC_Read_Device_Value(
      FT3168->Arduino_IIC_Touch::Value_Information::TOUCH_FINGER_NUMBER);
  bool pressed = fingers > 0;
  static uint8_t zero_streak = 0;
  if (pressed) {
    zero_streak = 0;
  } else if (g_touch_active && ++zero_streak < 3) {
    pressed = true;
    x = g_current_x;
    y = g_current_y;
  }

  if (pressed) {
    data->state = LV_INDEV_STATE_PR;
    data->point.x = x;
    data->point.y = y;
    g_last_touch_ms = millis();
    if (!g_touch_active) {
      g_touch_active = true;
      g_start_x = x;
      g_start_y = y;
      g_start_ms = millis();
    }
    g_current_x = x;
    g_current_y = y;
  } else {
    data->state = LV_INDEV_STATE_REL;
    if (g_touch_active) {
      g_touch_active = false;
      int32_t dx = g_current_x - g_start_x;
      int32_t dy = g_current_y - g_start_y;
      if (millis() - g_start_ms < SWIPE_MAX_DURATION_MS &&
          abs(dx) > SWIPE_THRESHOLD_PX && abs(dx) > abs(dy)) {
        g_last_swipe_ms = millis();
        g_pending_swipe = (dx > 0) ? -1 : 1;  // right swipe = back
      }
      g_last_touch_ms = millis();
    }
  }
}

// ---------------------------------------------------------------------------
//  Hardware bring-up
// ---------------------------------------------------------------------------
static void hardware_init() {
  Serial.begin(115200);
  pinMode(BOOT_BTN, INPUT_PULLUP);
  pinMode(TP_INT, INPUT_PULLUP);
  btStop();

  gfx->begin();
  gfx->fillScreen(0);
  static_cast<Arduino_CO5300 *>(gfx)->setBrightness(0);
  g_brightness = 0;

  pinMode(IIC_SCL, OUTPUT);
  for (int i = 0; i < 9; i++) {
    digitalWrite(IIC_SCL, LOW);
    delayMicroseconds(5);
    digitalWrite(IIC_SCL, HIGH);
    delayMicroseconds(5);
  }
  Wire.begin(IIC_SDA, IIC_SCL);
  Wire.setClock(400000);

  pinMode(TP_RESET, OUTPUT);
  digitalWrite(TP_RESET, LOW);
  delay(50);
  digitalWrite(TP_RESET, HIGH);
  delay(200);
  FT3168->begin();

  if (PMU.begin(Wire, AXP2101_SLAVE_ADDRESS, IIC_SDA, IIC_SCL)) {
    g_has_pmu = true;
    PMU.disableTSPinMeasure();
    PMU.enableBattVoltageMeasure();
  }

  lv_init();
  size_t pixels = LCD_W * 60;
  g_disp_buf = (lv_color_t *)heap_caps_malloc(pixels * sizeof(lv_color_t),
                                              MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  if (!g_disp_buf)
    g_disp_buf = (lv_color_t *)heap_caps_malloc(pixels * sizeof(lv_color_t),
                                                MALLOC_CAP_8BIT);
  lv_disp_draw_buf_init(&g_draw_buf, g_disp_buf, nullptr, pixels);
  lv_disp_drv_init(&g_disp_drv);
  g_disp_drv.flush_cb = display_flush;
  g_disp_drv.draw_buf = &g_draw_buf;
  g_disp_drv.hor_res = LCD_W;
  g_disp_drv.ver_res = LCD_H;
  lv_disp_drv_register(&g_disp_drv);

  lv_indev_drv_init(&g_indev_drv);
  g_indev_drv.type = LV_INDEV_TYPE_POINTER;
  g_indev_drv.read_cb = touch_read;
  lv_indev_drv_register(&g_indev_drv);
}

static void build_splash() {
  lv_obj_t *s = scr_new();
  lv_scr_load(s);
  g_cur_screen = s;
  lv_obj_t *dot = lv_obj_create(s);
  lv_obj_set_size(dot, 26, 26);
  lv_obj_align(dot, LV_ALIGN_CENTER, 0, -78);
  lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_bg_color(dot, lv_color_hex(COL_AMBER), 0);
  lv_obj_set_style_border_width(dot, 0, 0);
  label(s, OS_MARK, &lv_font_montserrat_28, COL_CREAM, LV_ALIGN_CENTER, 0, -10);
  label(s, "OBSERVE  ·  REMEMBER  ·  REASON", &lv_font_montserrat_12, COL_AMBER,
        LV_ALIGN_CENTER, 0, 34);
  label(s, "local-first  ·  offline  ·  SD storage", &lv_font_montserrat_12,
        COL_MUTED, LV_ALIGN_BOTTOM_MID, 0, -54);
  pump();
}

// ---------------------------------------------------------------------------
//  Setup / loop
// ---------------------------------------------------------------------------
void setup() {
  hardware_init();
  if (!alloc_stores()) {
    lv_obj_t *m = lv_msgbox_create(nullptr, "STORE MEMORY FAILED",
                                   "Not enough RAM for the record stores.\n"
                                   "Reboot with PSRAM enabled.",
                                   nullptr, true);
    lv_obj_center(m);
    for (;;) {
      pump();
      delay(10);
    }
  }
  restore_clock();
  {
    // The launcher owns the 12/24-hour choice in NVS "dcw"/"t24"; the face
    // follows it so both screens agree. It defaults to 24-hour here because
    // every other clock in this app has always been 24-hour.
    Preferences lp;
    if (lp.begin("dcw", true)) {
      g_face_24h = lp.getBool("t24", true);
      lp.end();
    }
  }
  // Waking from deep sleep is a reboot, but it is not a cold boot: the launcher
  // skips its splash here and so does this, so BOOT returns straight to the face.
  if (esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_UNDEFINED) {
    build_splash();
    delay(250);
  }

  SD_MMC.setPins(SDMMC_CLK, SDMMC_CMD, SDMMC_DATA);
  g_sd_ok = SD_MMC.begin("/sdcard", true);
  if (!g_sd_ok) {
    snprintf(g_sd_error, sizeof(g_sd_error), "mount failed");
  } else {
    ensure_folder();
    recover_tmp_files();
    for (int i = 0; i < 5; i++) ensure_store_file(STORE_PATHS[i], STORE_HEADS[i]);
  }

  load_all_stores();
  capture_load_draft();

  {
    Preferences p;
    p.begin("mentalist", true);
    g_area = constrain((int)p.getUChar("area", 0), 0, 4);
    g_screen_off_ms = p.getULong("soff", 10000UL);
    p.end();
  }
  {
    bool valid = false;
    for (int i = 0; i < 4; i++)
      if (g_screen_off_ms == SCREEN_TIMEOUT_OPTS[i]) valid = true;
    if (!valid) g_screen_off_ms = 10000UL;
  }

  g_view = V_CLOCK;
  g_view_depth = 0;
  render();
  log_boot();
  maybe_auto_backup();

  g_last_touch_ms = millis();
  g_last_wake_ms = millis();
  refresh_runtime_labels();
  fade_brightness(DEFAULT_BRIGHTNESS, 240);
}

void loop() {
  static uint32_t last_tick = millis();
  static uint32_t last_live = 0;
  static uint32_t boot_press = 0;
  uint32_t now = millis();

#if !defined(LV_TICK_CUSTOM) || (LV_TICK_CUSTOM == 0)
  lv_tick_inc(now - last_tick);
#endif
  last_tick = now;

  pump();
  focus_tick();

  if (g_pending_swipe) {
    int event = g_pending_swipe;
    g_pending_swipe = 0;
    if (!g_editor && !g_confirm && event < 0) {
      if (g_view != V_HOME) go_back();
    }
  }

  if (now - last_live >= 500) {
    last_live = now;
    refresh_runtime_labels();
  }

  if (now - g_last_clock_save_ms >= 300000UL) save_clock();

  // LauncherOS' pm_service() keeps exactly three things alive, and nothing
  // else: a finger on the panel, the post-wake grace window, and a screen held
  // by a command. Reading is not touching, so the countdown still runs -- but
  // nothing else is allowed to reset it either.
  //
  // v1.14: the half-timeout dim is gone. The launcher deleted auto-dim in v2.1
  // ("a lit panel keeps the user's brightness until the timeout takes it to
  // black") and after 5 s of reading this panel at 55/255 it looked like the
  // watch was switching itself off at random.
  bool grace = (uint32_t)(now - g_last_wake_ms) < WAKE_GRACE_MS;
  bool held = screen_held(now);
  if (g_touch_active || grace || held) g_last_touch_ms = now;

  if (g_screen_off) {
    if (digitalRead(TP_INT) == LOW || g_touch_active) screen_on();
  } else if (!g_touch_active && !held) {
    // v5.9's "pause while touching" rule: the countdown cannot tick down at
    // all under a finger, however still it is.
    if ((uint32_t)(now - g_last_touch_ms) >= g_screen_off_ms) enter_dark();
  }

  if (digitalRead(BOOT_BTN) == LOW) {
    if (!boot_press) boot_press = now;
    else if (!g_screen_off && now - boot_press > RETURN_HOLD_MS) return_to_launcher();
    else if (g_screen_off) {
      screen_on();
      boot_press = now;
    }
  } else {
    boot_press = 0;
  }

#if ENABLE_LIGHT_SLEEP
  if (g_screen_off) {
    light_sleep_tick();   // sleeps ~250 ms; may wake the panel or deep-sleep
    return;               // already slept: skip the 3 ms delay
  }
#endif
  delay(3);
}

