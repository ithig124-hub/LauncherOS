// =============================================================================
//  WaypointOS.ino v0.2 -- LauncherOS-compatible offline vector map + navigator
//  Target  : Waveshare ESP32-S3 Touch AMOLED 2.06 (410 x 502, CO5300, FT3168)
//  Compile : ESP32-S3 Dev Module / PSRAM=OPI / Flash=16MB / Minimal SPIFFS
//  Export  : Sketch -> Export Compiled Binary -> copy WaypointOS.ino.bin to
//            /WATCH/bin/waypointos.bin on the SD card.
// -----------------------------------------------------------------------------
//  Merges MapOS + NavOS around a native Mapbox Vector Tile (MVT) renderer.
//  Follows the WaypointOS design doc: OSM -> tilemaker -> MVT -> watch.
//
//  Screens : Splash -> Library -> Map -> (Compass | Level | Sun) via Tools menu
//
//  Controls are gestures first: there are no navigation buttons on the map any
//  more (a 410 px panel does not have room for buttons that also stays usable
//  with a glove). Every screen answers the same four verbs - tap what you want,
//  swipe to move between screens, hold for the contextual menu, drag what you
//  are looking at - and the recogniser that classifies them (wp_gesture_feed)
//  is plain arithmetic with no LVGL in it, so the host test suite compiles and
//  tests it. Thresholds come from LauncherOS V2's touch handling.
//
//    library  swipe < >  browse maps        map  drag           pan
//             tap card   open the map             double tap     zoom in
//             swipe ^    map details              2-finger tap   zoom out
//             swipe v    hide the details         2-finger drag  zoom
//             hold       map details              tap            pick a POI
//                                                 hold           Tools menu
//                                                 swipe ^ bottom Tools menu
//                                                 swipe v top    library
//                                                 tap compass    heading-up
//
//    menus    tap a row to act on it, swipe v to go back a level, swipe < > to
//             walk between the screens that belong together (sensors, setup,
//             tools)
//    BOOT     tap next / 2x tap prev / 0.5 s open or recentre / 1.5 s back a
//             level / 4 s straight back to LauncherOS
//
//  SD root : /WATCH/WayPoint/
//  Maps    : /WATCH/WayPoint/<map>/{z}/{x}/{y}.pbf   (tilemaker default output)
//            optional /WATCH/WayPoint/<map>/metadata.json with
//              { "name":"Byford", "description":"...",
//                "bounds":"minLon,minLat,maxLon,maxLat",
//                "minzoom":10, "maxzoom":17 }
//  NVS ns  : "waypoint"
// =============================================================================

#include <Arduino.h>
#include <FS.h>
#include <SD_MMC.h>
#include <Wire.h>
#include <Preferences.h>
#include <math.h>
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
#include <esp_heap_caps.h>
#include <esp_bt.h>

// =============================================================================
//  Forward-declare EVERY user struct/enum here, before any inlined helper code.
//  Arduino auto-generates function prototypes and inserts them just before the
//  FIRST function definition in the sketch. Because puff.h (inlined just below)
//  defines functions, that insertion point is now near the top -- so these
//  forward declarations MUST precede it, or prototypes that mention these types
//  (TH(), tile_free(FMapTile&), wp_create_menu_item(const MenuItem&), ...) fail
//  to compile with "does not name a type".
// =============================================================================
struct ThemePalette;
struct FVec2;
struct FMapObj;
struct FMapPOI;
struct FMapMeta;
struct FMapZoom;
struct FMapStyle;
struct FMapTile;
struct MVTDecodeCtx;
struct MenuItem;

// The gesture recogniser, the MVT helper types and the styler are in the same
// boat: they name their types in function signatures. Their structs are defined
// further down, next to the code that uses them -- deliberately, because the
// host tests splice those blocks (WP_GESTURE_ENGINE, WAYPOINT_MAP_ENGINE, the
// styler slices) straight out of this file and compile them with g++ -- so
// without a forward declaration up here, the prototypes Arduino inserts just
// below this block would not compile.
enum WpGesture : uint16_t;
enum WpEdge : uint8_t;
struct WpTouchDebounce;
struct WpGestureState;
struct MvtStr;
struct MvtVal;
struct MvtKeyTable;
struct MvtTags;
struct WpStyle;

// ===== BEGIN inlined puff.h (gzip/DEFLATE inflater) =====
// =============================================================================
//  puff.h -- self-contained DEFLATE/gzip inflater for WaypointOS
//
//  Based on puff.c by Mark Adler (public domain, part of zlib/contrib).
//  Trimmed to a single header and wrapped with wp_gunzip() so the firmware can
//  inflate gzip-compressed MVT tiles from SD with NO external zlib/miniz
//  dependency. Slow-but-correct reference inflate; fine for one 5-50 KB tile.
//
//  Usage:
//     long out_len = dest_cap;
//     unsigned long in_len = src_len;
//     int rc = wp_gunzip(dest, &out_len, src, &in_len);  // rc==0 on success
//
//  puff() itself decodes a raw DEFLATE stream; wp_gunzip() strips the gzip
//  (RFC 1952) header/trailer first.
// =============================================================================

#include <setjmp.h>
#include <string.h>

#define PUFF_MAXBITS   15
#define PUFF_MAXLCODES 286
#define PUFF_MAXDCODES 30
#define PUFF_MAXCODES  (PUFF_MAXLCODES + PUFF_MAXDCODES)
#define PUFF_FIXLCODES 288

struct puff_state {
    unsigned char *out;
    unsigned long outlen;
    unsigned long outcnt;
    const unsigned char *in;
    unsigned long inlen;
    unsigned long incnt;
    int bitbuf;
    int bitcnt;
    jmp_buf env;
};

struct puff_huffman { short *count; short *symbol; };

static int puff_bits(struct puff_state *s, int need) {
    long val = s->bitbuf;
    while (s->bitcnt < need) {
        if (s->incnt == s->inlen) longjmp(s->env, 1);
        val |= (long)(s->in[s->incnt++]) << s->bitcnt;
        s->bitcnt += 8;
    }
    s->bitbuf = (int)(val >> need);
    s->bitcnt -= need;
    return (int)(val & ((1L << need) - 1));
}

static int puff_stored(struct puff_state *s) {
    unsigned len;
    s->bitbuf = 0; s->bitcnt = 0;
    if (s->incnt + 4 > s->inlen) return 2;
    len = s->in[s->incnt++];
    len |= s->in[s->incnt++] << 8;
    if (s->in[s->incnt++] != (~len & 0xff) ||
        s->in[s->incnt++] != ((~len >> 8) & 0xff)) return -2;
    if (s->incnt + len > s->inlen) return 2;
    while (len--) {
        if (s->outcnt < s->outlen) s->out[s->outcnt] = s->in[s->incnt];
        else if (s->out != 0) return 1;
        s->outcnt++; s->incnt++;
    }
    return 0;
}

static int puff_decode(struct puff_state *s, const struct puff_huffman *h) {
    int len, code = 0, first = 0, index = 0;
    short *next = h->count + 1;
    while (1) {
        for (len = 1; len <= PUFF_MAXBITS; len++) {
            code |= puff_bits(s, 1);
            int count = *next++;
            if (code - count < first) return h->symbol[index + (code - first)];
            index += count; first += count; first <<= 1; code <<= 1;
        }
        return -10;
    }
}

static int puff_construct(struct puff_huffman *h, const short *length, int n) {
    int symbol, len, left;
    short offs[PUFF_MAXBITS + 1];
    for (len = 0; len <= PUFF_MAXBITS; len++) h->count[len] = 0;
    for (symbol = 0; symbol < n; symbol++) h->count[length[symbol]]++;
    if (h->count[0] == n) return 0;
    left = 1;
    for (len = 1; len <= PUFF_MAXBITS; len++) {
        left <<= 1;
        left -= h->count[len];
        if (left < 0) return left;
    }
    offs[1] = 0;
    for (len = 1; len < PUFF_MAXBITS; len++)
        offs[len + 1] = offs[len] + h->count[len];
    for (symbol = 0; symbol < n; symbol++)
        if (length[symbol] != 0) h->symbol[offs[length[symbol]]++] = symbol;
    return left;
}

static int puff_codes(struct puff_state *s,
                      const struct puff_huffman *lencode,
                      const struct puff_huffman *distcode) {
    static const short lens[29] = {
        3,4,5,6,7,8,9,10,11,13,15,17,19,23,27,31,35,43,51,59,67,83,99,115,131,163,195,227,258};
    static const short lext[29] = {
        0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2,3,3,3,3,4,4,4,4,5,5,5,5,0};
    static const short dists[30] = {
        1,2,3,4,5,7,9,13,17,25,33,49,65,97,129,193,257,385,513,769,
        1025,1537,2049,3073,4097,6145,8193,12289,16385,24577};
    static const short dext[30] = {
        0,0,0,0,1,1,2,2,3,3,4,4,5,5,6,6,7,7,8,8,9,9,10,10,11,11,12,12,13,13};
    int symbol, len;
    unsigned long dist;
    do {
        symbol = puff_decode(s, lencode);
        if (symbol < 0) return symbol;
        if (symbol < 256) {
            if (s->outcnt < s->outlen) s->out[s->outcnt] = (unsigned char)symbol;
            else if (s->out != 0) return 1;
            s->outcnt++;
        } else if (symbol > 256) {
            symbol -= 257;
            if (symbol >= 29) return -10;
            len = lens[symbol] + puff_bits(s, lext[symbol]);
            symbol = puff_decode(s, distcode);
            if (symbol < 0) return symbol;
            dist = dists[symbol] + puff_bits(s, dext[symbol]);
            if (s->out != 0 && dist > s->outcnt) return -11;
            while (len--) {
                if (s->outcnt < s->outlen) {
                    if (s->out != 0)
                        s->out[s->outcnt] = s->out[s->outcnt - dist];
                } else if (s->out != 0) return 1;
                s->outcnt++;
            }
        }
    } while (symbol != 256);
    return 0;
}

static int puff_fixed(struct puff_state *s) {
    static int virgin = 1;
    static short lencnt[PUFF_MAXBITS + 1], lensym[PUFF_FIXLCODES];
    static short distcnt[PUFF_MAXBITS + 1], distsym[PUFF_MAXDCODES];
    static struct puff_huffman lencode, distcode;
    if (virgin) {
        int symbol; short lengths[PUFF_FIXLCODES];
        lencode.count = lencnt; lencode.symbol = lensym;
        distcode.count = distcnt; distcode.symbol = distsym;
        for (symbol = 0; symbol < 144; symbol++) lengths[symbol] = 8;
        for (; symbol < 256; symbol++) lengths[symbol] = 9;
        for (; symbol < 280; symbol++) lengths[symbol] = 7;
        for (; symbol < PUFF_FIXLCODES; symbol++) lengths[symbol] = 8;
        puff_construct(&lencode, lengths, PUFF_FIXLCODES);
        for (symbol = 0; symbol < PUFF_MAXDCODES; symbol++) lengths[symbol] = 5;
        puff_construct(&distcode, lengths, PUFF_MAXDCODES);
        virgin = 0;
    }
    return puff_codes(s, &lencode, &distcode);
}

static int puff_dynamic(struct puff_state *s) {
    int nlen, ndist, ncode, index, err;
    short lengths[PUFF_MAXCODES];
    short lencnt[PUFF_MAXBITS + 1], lensym[PUFF_MAXLCODES];
    short distcnt[PUFF_MAXBITS + 1], distsym[PUFF_MAXDCODES];
    struct puff_huffman lencode, distcode;
    static const short order[19] =
        {16,17,18,0,8,7,9,6,10,5,11,4,12,3,13,2,14,1,15};
    lencode.count = lencnt; lencode.symbol = lensym;
    distcode.count = distcnt; distcode.symbol = distsym;
    nlen = puff_bits(s, 5) + 257;
    ndist = puff_bits(s, 5) + 1;
    ncode = puff_bits(s, 4) + 4;
    if (nlen > PUFF_MAXLCODES || ndist > PUFF_MAXDCODES) return -3;
    for (index = 0; index < ncode; index++) lengths[order[index]] = puff_bits(s, 3);
    for (; index < 19; index++) lengths[order[index]] = 0;
    err = puff_construct(&lencode, lengths, 19);
    if (err != 0) return -4;
    index = 0;
    while (index < nlen + ndist) {
        int symbol = puff_decode(s, &lencode);
        if (symbol < 0) return symbol;
        if (symbol < 16) lengths[index++] = symbol;
        else {
            int len = 0;
            if (symbol == 16) {
                if (index == 0) return -5;
                len = lengths[index - 1];
                symbol = 3 + puff_bits(s, 2);
            } else if (symbol == 17) symbol = 3 + puff_bits(s, 3);
            else symbol = 11 + puff_bits(s, 7);
            if (index + symbol > nlen + ndist) return -6;
            while (symbol--) lengths[index++] = len;
        }
    }
    if (lengths[256] == 0) return -9;
    err = puff_construct(&lencode, lengths, nlen);
    if (err && (err < 0 || nlen != lencode.count[0] + lencode.count[1])) return -7;
    err = puff_construct(&distcode, lengths + nlen, ndist);
    if (err && (err < 0 || ndist != distcode.count[0] + distcode.count[1])) return -8;
    return puff_codes(s, &lencode, &distcode);
}

// Inflate a raw DEFLATE stream. *destlen in=capacity, out=bytes written.
static int puff(unsigned char *dest, unsigned long *destlen,
                const unsigned char *source, unsigned long *sourcelen) {
    struct puff_state s;
    int last, type, err;
    s.out = dest; s.outlen = *destlen; s.outcnt = 0;
    s.in = source; s.inlen = *sourcelen; s.incnt = 0;
    s.bitbuf = 0; s.bitcnt = 0;
    if (setjmp(s.env) != 0) err = 2;
    else {
        do {
            last = puff_bits(&s, 1);
            type = puff_bits(&s, 2);
            err = type == 0 ? puff_stored(&s) :
                  type == 1 ? puff_fixed(&s) :
                  type == 2 ? puff_dynamic(&s) : -1;
            if (err != 0) break;
        } while (!last);
    }
    if (err <= 0) { *destlen = s.outcnt; *sourcelen = s.incnt; }
    return err;
}

// -----------------------------------------------------------------------------
// wp_gunzip: strip the gzip (RFC 1952) header/trailer and inflate the payload.
// Returns 0 on success. *destlen in=capacity, out=decompressed size.
// -----------------------------------------------------------------------------
static int wp_gunzip(unsigned char *dest, unsigned long *destlen,
                     const unsigned char *src, unsigned long *srclen) {
    unsigned long n = *srclen;
    if (n < 18) return -100;                 // too small to be gzip
    if (src[0] != 0x1F || src[1] != 0x8B) return -101;
    if (src[2] != 8) return -102;            // not DEFLATE
    unsigned char flg = src[3];
    unsigned long pos = 10;                  // fixed header
    if (flg & 0x04) {                        // FEXTRA
        if (pos + 2 > n) return -103;
        unsigned xlen = src[pos] | (src[pos + 1] << 8);
        pos += 2 + xlen;
    }
    if (flg & 0x08) { while (pos < n && src[pos]) pos++; pos++; }  // FNAME
    if (flg & 0x10) { while (pos < n && src[pos]) pos++; pos++; }  // FCOMMENT
    if (flg & 0x02) pos += 2;                // FHCRC
    if (pos + 8 > n) return -104;            // must leave room for CRC+ISIZE
    unsigned long deflate_len = n - pos - 8;
    unsigned long dl = deflate_len;
    int rc = puff(dest, destlen, src + pos, &dl);
    return rc;                               // 0 = ok, 1 = out buffer too small
}
// ===== END inlined puff.h =====



// =============================================================================
//  MVT rendering diagnostics.
//  Set to 1 to have the tile loader / layer decoder / feature decoder print
//  their progress on Serial. Turn OFF once the map is confirmed rendering.
// =============================================================================
#ifndef MVT_DEBUG
#define MVT_DEBUG 0
#endif
#if MVT_DEBUG
  #define MVT_LOG(...)  do { Serial.printf(__VA_ARGS__); } while (0)
#else
  #define MVT_LOG(...)  do { } while (0)
#endif

// Set to 1 (default) so that layers whose names WaypointOS doesn't recognise
// still get rendered as thin grey outlines (LAY_UNKNOWN bucket). This makes
// tiles from non-OpenMapTiles converters visible so the user can tell whether
// parsing succeeded, and the MVT_DEBUG log tells us which names to bake in.
#ifndef MVT_DRAW_UNKNOWN
#define MVT_DRAW_UNKNOWN 1
#endif

// ===== SHARED BOOT BLOCK (verbatim from MapOS/NavOS) =========================
// Forward-declare every user struct BEFORE any function uses them. Arduino IDE
// auto-prepends function prototypes to the top of the transformed .cpp; those
// prototypes fail to compile if the struct types aren't visible yet.
struct ThemePalette;
struct FVec2;
struct FMapObj;
struct FMapPOI;
struct FMapMeta;
struct FMapZoom;
struct FMapStyle;
struct FMapTile;
struct MVTDecodeCtx;

#define LCD_SDIO0 4
#define LCD_SDIO1 5
#define LCD_SDIO2 6
#define LCD_SDIO3 7
#define LCD_SCLK 11
#define LCD_CS   12
#define LCD_RESET 8
#define LCD_WIDTH  410
#define LCD_HEIGHT 502
#define IIC_SDA 15
#define IIC_SCL 14
#define TP_INT 38
#define TP_RESET 9
static const int SDMMC_CLK=2, SDMMC_CMD=1, SDMMC_DATA=3;
#define BOOT_BTN_PIN 0
#define PWR_BTN_PIN  10

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

static volatile bool     g_tch_pressed=false;
static volatile int32_t  g_tch_x=0, g_tch_y=0;
static volatile int      g_tch_fingers=0;
static volatile int32_t  g_tch_x2=0, g_tch_y2=0;

static void my_disp_flush(lv_disp_drv_t *d, const lv_area_t *a, lv_color_t *px) {
    uint32_t w=a->x2-a->x1+1, h=a->y2-a->y1+1;
    gfx->draw16bitRGBBitmap(a->x1, a->y1, (uint16_t*)px, w, h);
    lv_disp_flush_ready(d);
}
// ===== BEGIN WP_GESTURE_ENGINE
// =============================================================================
//  Gesture recogniser
//
//  One recogniser for the whole UI, so every screen answers the same four
//  verbs: tap what you want, swipe to move between screens, hold for the
//  contextual menu, drag the thing you are looking at. It replaces the
//  on-screen arrow / zoom / tools buttons the map screen used to need.
//
//  The thresholds and the rules are the ones this watch's touch feel was tuned
//  with in LauncherOS V2: TAP 20 px, SWIPE 25 px, a swipe only inside 800 ms,
//  every gesture classified on finger release - plus the two things that repo
//  learned the hard way. Release hysteresis, because the FT3168 reports
//  fingers=0 for a single sample mid-touch and believing that sample ends the
//  gesture early. And the post-swipe click guard, so the finger that just
//  swiped cannot also click whatever it landed on.
//
//  Everything below is plain integer arithmetic: no LVGL, no Arduino, no I/O.
//  That is deliberate - tools/host_test compiles this exact code with g++ and
//  asserts the classification, so "a tap stays a tap" is a tested claim rather
//  than a hope about a panel that cannot be seen from a build machine.
//
//  Coordinates are SCREEN coordinates (origin top-left, +y down): the same ones
//  the display driver and the hit tests already use.
// =============================================================================
#define WP_TAP_PX               20    // < this on BOTH axes = a tap
#define WP_SWIPE_PX             25    // >= this on the dominant axis = a swipe
#define WP_SWIPE_MAX_MS        800    // slower than this is a drag, never a swipe
#define WP_EDGE_SWIPE_MAX_MS   500    // ... and an edge swipe has to be a flick
#define WP_LONG_PRESS_MS       550    // hold still this long -> long press
#define WP_DOUBLE_TAP_MS       320    // second tap inside this -> double tap
#define WP_DOUBLE_TAP_PX        40    // ... and within this many px of the first
#define WP_DRAG_SLOP_PX          6    // movement that starts a pan
#define WP_EDGE_PX              56    // edge zone width for edge swipes
#define WP_CLICK_AFTER_SWIPE_MS 350   // swallow pointer input after a swipe
#define WP_RELEASE_HYSTERESIS    3    // empty samples before a release is real
#define WP_TWO_FINGER_SAMPLES    2    // samples before two fingers are believed

enum WpGesture : uint16_t {
    WG_NONE        = 0u,
    WG_TAP         = 1u << 0,
    WG_DOUBLE_TAP  = 1u << 1,
    WG_LONG_PRESS  = 1u << 2,
    WG_SWIPE_LEFT  = 1u << 3,
    WG_SWIPE_RIGHT = 1u << 4,
    WG_SWIPE_UP    = 1u << 5,
    WG_SWIPE_DOWN  = 1u << 6,
    WG_DRAG_START  = 1u << 7,
    WG_DRAG        = 1u << 8,
    WG_RELEASE     = 1u << 9,
    WG_PINCH       = 1u << 10,
    WG_TWO_TAP     = 1u << 11,
};

enum WpEdge : uint8_t {
    WP_EDGE_NONE = 0, WP_EDGE_LEFT, WP_EDGE_RIGHT, WP_EDGE_TOP, WP_EDGE_BOTTOM
};

struct WpTouchDebounce { uint8_t zero_streak; bool active; };

// Returns the debounced pressed state. A single empty sample in the middle of a
// touch is reported as still-pressed, so it cannot fire a false release, which
// would end the gesture early and start the screen-dim clock under a finger.
static bool wp_touch_debounce(WpTouchDebounce *d, bool raw_pressed, uint8_t hysteresis) {
    if (raw_pressed) { d->zero_streak = 0; d->active = true; return true; }
    if (!d->active) return false;
    d->zero_streak++;
    if (d->zero_streak < hysteresis) return true;
    d->zero_streak = 0;
    d->active = false;
    return false;
}

// The post-swipe click guard is active for a short window after a swipe.
static bool wp_swipe_guard_active(uint32_t now_ms, uint32_t last_swipe_ms) {
    if (!last_swipe_ms) return false;
    return (uint32_t)(now_ms - last_swipe_ms) < WP_CLICK_AFTER_SWIPE_MS;
}

static inline int32_t wp_abs32(int32_t v) { return v < 0 ? -v : v; }

// Which screen edge a press started nearest to (within WP_EDGE_PX). A screen
// uses this to tell an edge swipe from a pan that happens to point the same
// way, so panning and going back never fight over the same flick.
static uint8_t wp_edge_zone(int32_t x, int32_t y, int32_t w, int32_t h) {
    int32_t dl = x, dr = w - 1 - x, dt = y, db = h - 1 - y;
    int32_t best = dl;
    uint8_t zone = WP_EDGE_LEFT;
    if (dr < best) { best = dr; zone = WP_EDGE_RIGHT; }
    if (dt < best) { best = dt; zone = WP_EDGE_TOP; }
    if (db < best) { best = db; zone = WP_EDGE_BOTTOM; }
    return (best <= WP_EDGE_PX) ? zone : WP_EDGE_NONE;
}

struct WpGestureState {
    bool     down;            // a touch is being tracked
    bool     second_tap;      // this press is the second half of a double tap
    bool     dragged;         // WG_DRAG_START has been emitted
    bool     long_fired;
    bool     two_finger;      // two fingers, for more than one sample
    uint8_t  two_streak;
    uint8_t  fingers;
    uint8_t  edge;            // edge zone the press started in
    int32_t  sx, sy;          // press start
    int32_t  lx, ly;          // last sample
    int32_t  dx, dy;          // last - start, i.e. total travel
    uint32_t t0;
    uint32_t dur;             // duration of the last completed touch
    bool     tap_pending;     // a tap the double-tap window may still claim
    uint32_t tap_pending_t;
    int32_t  tap_x, tap_y;
    uint32_t last_swipe_t;
};

static void wp_gesture_reset(WpGestureState *g) {
    g->down = false; g->second_tap = false; g->dragged = false;
    g->long_fired = false; g->two_finger = false; g->two_streak = 0;
    g->fingers = 0; g->edge = WP_EDGE_NONE;
    g->dx = g->dy = 0; g->dur = 0;
    g->tap_pending = false; g->tap_pending_t = 0; g->last_swipe_t = 0;
}

// Feed one touch sample; returns the gestures that completed on this sample.
//
// Call it every poll, pressed or not: a tap is only final once the double-tap
// window has closed, and those idle samples are what resolve it. That delay is
// what lets one finger do both jobs - a tap on a POI selects it, while a second
// tap inside 320 ms becomes a zoom instead of running both actions.
static uint16_t wp_gesture_feed(WpGestureState *g, bool pressed, int32_t x, int32_t y,
                                int fingers, uint32_t now_ms,
                                int32_t screen_w, int32_t screen_h) {
    uint16_t ev = WG_NONE;

    if (pressed) {
        if (!g->down) {
            // ---- press start ---------------------------------------------
            g->down       = true;
            g->sx = x; g->sy = y; g->lx = x; g->ly = y;
            g->dx = 0; g->dy = 0;
            g->t0         = now_ms;
            g->dragged    = false;
            g->long_fired = false;
            g->fingers    = (uint8_t)(fingers > 0 ? fingers : 1);
            // A touch that ARRIVES with two fingers is a pinch or a two-finger
            // tap straight away; the streak below is for a finger count that
            // flickers mid-touch, which is the noise that must not turn a pan
            // into a pinch.
            g->two_finger = (fingers >= 2);
            g->two_streak = (fingers >= 2) ? WP_TWO_FINGER_SAMPLES : 0;
            g->edge       = wp_edge_zone(x, y, screen_w, screen_h);
            if (g->tap_pending &&
                (uint32_t)(now_ms - g->tap_pending_t) <= WP_DOUBLE_TAP_MS &&
                wp_abs32(x - g->tap_x) <= WP_DOUBLE_TAP_PX &&
                wp_abs32(y - g->tap_y) <= WP_DOUBLE_TAP_PX) {
                g->second_tap  = true;      // resolved on release
                g->tap_pending = false;     // the single tap never happened
            } else {
                g->second_tap = false;
            }
            return ev;
        }
        // ---- while pressed ------------------------------------------------
        if (fingers > (int)g->fingers) g->fingers = (uint8_t)fingers;
        if (fingers >= 2) {
            if (g->two_streak < 255) g->two_streak++;
            // One noisy sample must not turn a pan into a pinch.
            if (g->two_streak >= WP_TWO_FINGER_SAMPLES) g->two_finger = true;
        } else {
            g->two_streak = 0;
        }
        g->lx = x; g->ly = y;
        g->dx = x - g->sx;
        g->dy = y - g->sy;
        const int32_t ax = wp_abs32(g->dx), ay = wp_abs32(g->dy);

        if (!g->dragged && (ax + ay) > WP_DRAG_SLOP_PX) {
            g->dragged = true;
            ev |= WG_DRAG_START;
        }
        if (g->dragged) ev |= (g->two_finger ? WG_PINCH : WG_DRAG);
        if (!g->long_fired && !g->dragged &&
            (uint32_t)(now_ms - g->t0) >= WP_LONG_PRESS_MS) {
            g->long_fired = true;
            ev |= WG_LONG_PRESS;
        }
        return ev;
    }

    if (g->down) {
        // ---- release: classify the whole touch -----------------------------
        g->down = false;
        g->dur  = (uint32_t)(now_ms - g->t0);
        const int32_t ax = wp_abs32(g->dx), ay = wp_abs32(g->dy);
        const bool tap_like   = (ax < WP_TAP_PX && ay < WP_TAP_PX && !g->long_fired);
        const bool swipe_like = (g->dur <= WP_SWIPE_MAX_MS) &&
                                ((ax >= WP_SWIPE_PX && ax > ay) ||
                                 (ay >= WP_SWIPE_PX && ay > ax));
        ev |= WG_RELEASE;
        if (swipe_like) {
            if (ax >= WP_SWIPE_PX && ax > ay)
                ev |= (g->dx < 0) ? WG_SWIPE_LEFT : WG_SWIPE_RIGHT;
            else
                ev |= (g->dy < 0) ? WG_SWIPE_UP : WG_SWIPE_DOWN;
            g->last_swipe_t = now_ms;
            g->tap_pending  = false;            // a swipe is never also a tap
        } else if (tap_like && g->second_tap) {
            ev |= WG_DOUBLE_TAP;
            g->second_tap = false;
        } else if (tap_like && g->two_finger) {
            ev |= WG_TWO_TAP;
        } else if (tap_like) {
            g->tap_pending   = true;
            g->tap_pending_t = now_ms;
            g->tap_x = g->sx; g->tap_y = g->sy;
        } else {
            g->tap_pending = false;
        }
        return ev;
    }

    // ---- idle: resolve a tap the double-tap window never claimed -----------
    if (g->tap_pending &&
        (uint32_t)(now_ms - g->tap_pending_t) >= WP_DOUBLE_TAP_MS) {
        g->tap_pending = false;
        ev |= WG_TAP;
    }
    return ev;
}

// ===== END WP_GESTURE_ENGINE

// =============================================================================
//  Touch driver
// =============================================================================
static WpGestureState   g_gesture;
static WpTouchDebounce  g_touch_deb = { 0, false };
static uint16_t         g_gesture_events = WG_NONE;   // drained by the UI

static void my_touchpad_read(lv_indev_drv_t *, lv_indev_data_t *data) {
    int32_t x=FT3168->IIC_Read_Device_Value(FT3168->Arduino_IIC_Touch::Value_Information::TOUCH_COORDINATE_X);
    int32_t y=FT3168->IIC_Read_Device_Value(FT3168->Arduino_IIC_Touch::Value_Information::TOUCH_COORDINATE_Y);
    int fingers=FT3168->IIC_Read_Device_Value(FT3168->Arduino_IIC_Touch::Value_Information::TOUCH_FINGER_NUMBER);
    static int32_t last_x = 0, last_y = 0;
    const uint32_t now = millis();

    // Hysteresis: while the FT3168 is glitching, keep reporting the last known
    // position rather than a release, so the gesture survives the blip.
    const bool raw     = (fingers > 0);
    const bool pressed = wp_touch_debounce(&g_touch_deb, raw, WP_RELEASE_HYSTERESIS);
    if (pressed) {
        if (raw) { last_x = x; last_y = y; }
        else     { x = last_x; y = last_y; }
    }
    g_tch_fingers = pressed ? fingers : 0;

    g_gesture_events |= wp_gesture_feed(&g_gesture, pressed, x, y, fingers, now,
                                        LCD_WIDTH, LCD_HEIGHT);

    if (pressed) {
        data->state=LV_INDEV_STATE_PR; data->point.x=x; data->point.y=y;
        g_tch_pressed=true; g_tch_x=x; g_tch_y=y;
    } else {
        data->state=LV_INDEV_STATE_REL;
        if (g_tch_pressed) { g_tch_x=x; g_tch_y=y; }
        g_tch_pressed=false;
    }

    // Post-swipe click guard (LauncherOS parity): while it is open, LVGL is
    // told nothing is touching the panel, so the screen a swipe just moved to
    // cannot also register a click on whatever ended up under the finger.
    if (wp_swipe_guard_active(now, g_gesture.last_swipe_t)) {
        data->state = LV_INDEV_STATE_REL;
    }
}

XPowersPMU g_pmu; static bool g_has_pmu=false;
static void pmu_init() {
    if (g_pmu.begin(Wire, AXP2101_SLAVE_ADDRESS, IIC_SDA, IIC_SCL)) {
        g_has_pmu=true;
        g_pmu.disableTSPinMeasure();
        g_pmu.enableBattVoltageMeasure();
    }
}

// =============================================================================
//  Display brightness + idle-dim management
//  AMOLED brightness is controlled via CO5300 command 0x51 (Write Display
//  Brightness). Arduino_GFX's CO5300 driver exposes this as Display_Brightness().
//  We ramp softly so the transition doesn't flash.
// =============================================================================
static uint8_t  g_brightness_cur    = 0;     // current applied brightness (start 0 so first apply writes)
static uint8_t  g_brightness_target = 255;   // where we're heading (0..255)
#define WP_BRIGHTNESS_FULL     255
#define WP_BRIGHTNESS_DIM       38          // ~15% of 255
#define WP_IDLE_DIM_MS       30000UL        // dim after 30 s of no input
#define WP_BRIGHTNESS_STEP       6          // per-tick ramp step (~40 ms/step)
#define WP_BRIGHTNESS_TICK_MS   35

static void wp_brightness_apply(uint8_t v) {
    // No-op if unchanged; also cache so tick doesn't spam the bus.
    if (v == g_brightness_cur) return;
    g_brightness_cur = v;
    // CO5300 Write Display Brightness = command 0x51 + 1 data byte (0..255).
    // Arduino_GFX 1.6.x doesn't expose gfx->Display_Brightness(); we send it
    // directly through the QSPI data bus, which is what Waveshare's own
    // ESP32-S3-Touch-AMOLED-2.06 examples do for runtime brightness.
    bus->beginWrite();
    bus->writeCommand(0x51);
    bus->write(v);
    bus->endWrite();
}

static void wp_brightness_tick(uint32_t now) {
    static uint32_t last_step = 0;
    if (now - last_step < WP_BRIGHTNESS_TICK_MS) return;
    last_step = now;
    if (g_brightness_cur == g_brightness_target) return;
    int delta = (int)g_brightness_target - (int)g_brightness_cur;
    int step  = delta >  WP_BRIGHTNESS_STEP ?  WP_BRIGHTNESS_STEP :
                delta < -WP_BRIGHTNESS_STEP ? -WP_BRIGHTNESS_STEP : delta;
    wp_brightness_apply((uint8_t)((int)g_brightness_cur + step));
}

// -----------------------------------------------------------------------------
//  Activity tracking (touch, button, IMU motion) drives idle-dim.
// -----------------------------------------------------------------------------
static uint32_t g_last_activity_ms = 0;
static bool     g_idle_dimmed      = false;

static inline void wp_mark_activity(uint32_t now) {
    g_last_activity_ms = now;
    if (g_idle_dimmed) {
        g_idle_dimmed = false;
        g_brightness_target = WP_BRIGHTNESS_FULL;
    }
}

static void wp_idle_dim_tick(uint32_t now) {
    if (g_idle_dimmed) return;
    if (now - g_last_activity_ms > WP_IDLE_DIM_MS) {
        g_idle_dimmed = true;
        g_brightness_target = WP_BRIGHTNESS_DIM;
    }
}

static void return_to_launcher() {
    const esp_partition_t *factory = esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_FACTORY, NULL);
    if (factory) esp_ota_set_boot_partition(factory);
    else { const esp_partition_t *o = esp_ota_get_next_update_partition(NULL); if (o && o!=esp_ota_get_running_partition()) esp_ota_set_boot_partition(o); }
    delay(80); esp_restart();
}

static void launcher_app_boot() {
    Serial.begin(115200);
    pinMode(BOOT_BTN_PIN, INPUT_PULLUP); pinMode(PWR_BTN_PIN, INPUT_PULLUP); btStop();
    if (!gfx->begin()) Serial.println("gfx fail");
    gfx->fillScreen(0x0000);
    pinMode(IIC_SCL, OUTPUT);
    for (int i=0; i<9; i++) { digitalWrite(IIC_SCL,LOW); delayMicroseconds(5); digitalWrite(IIC_SCL,HIGH); delayMicroseconds(5); }
    Wire.begin(IIC_SDA, IIC_SCL);
    pinMode(TP_RESET, OUTPUT); digitalWrite(TP_RESET, LOW); delay(50); digitalWrite(TP_RESET, HIGH); delay(200);
    FT3168->begin();
    // The gesture state is a static (so already zero), but reset it explicitly:
    // it documents that a boot starts with no half-finished gesture in flight.
    wp_gesture_reset(&g_gesture);
    pmu_init();
    lv_init();
    size_t bufPx = LCD_WIDTH * 40;
    lvgl_buf = (lv_color_t*)heap_caps_malloc(bufPx*sizeof(lv_color_t), MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT);
    if (!lvgl_buf) lvgl_buf = (lv_color_t*)heap_caps_malloc(bufPx*sizeof(lv_color_t), MALLOC_CAP_8BIT);
    lv_disp_draw_buf_init(&draw_buf, lvgl_buf, NULL, bufPx);
    lv_disp_drv_init(&disp_drv); disp_drv.flush_cb=my_disp_flush; disp_drv.draw_buf=&draw_buf;
    disp_drv.hor_res=LCD_WIDTH; disp_drv.ver_res=LCD_HEIGHT;
    lv_disp_drv_register(&disp_drv);
    lv_indev_drv_init(&indev_drv); indev_drv.type=LV_INDEV_TYPE_POINTER; indev_drv.read_cb=my_touchpad_read;
    lv_indev_drv_register(&indev_drv);
    SD_MMC.setPins(SDMMC_CLK, SDMMC_CMD, SDMMC_DATA);
    SD_MMC.begin("/sdcard", true);
}

static void time_restore_from_launcher() {
    Preferences p;
    if (!p.begin("rtcsave", true)) return;
    uint32_t t = p.getULong("t", 0); long gmt = p.getLong("gmt", 0); p.end();
    if (t > 1700000000) {
        struct timeval tv = { .tv_sec=(time_t)t, .tv_usec=0 };
        settimeofday(&tv, NULL);
        char tz[16]; snprintf(tz, sizeof(tz), "UTC%+ld", -gmt/3600);
        setenv("TZ", tz, 1); tzset();
    }
}
// ===== END SHARED BOOT BLOCK =================================================

// =============================================================================
//  Themes
// =============================================================================
enum ThemeId { THEME_FUSION_DARK=0, THEME_FUSION_LIGHT=1, THEME_SERIOUS=2, THEME_CHILL=3 };

struct ThemePalette {
    const char *name;
    uint16_t bg;            // map background
    uint16_t chrome_bg;     // overlay chip bg
    uint16_t chrome_border; // overlay chip stroke
    uint16_t text;
    uint16_t text_dim;
    uint16_t accent;        // primary UI accent
    uint16_t warn;          // gold / align tint
    uint16_t roads;
    uint16_t buildings;
    uint16_t parks;
    uint16_t water;
    uint16_t paths;
    uint16_t schools;
    uint16_t shops;
    uint16_t transit;
    uint16_t beacons;
    uint16_t saved;
    uint16_t breadcrumb;
};

static const ThemePalette THEMES[] = {
    // FUSION DARK: deep navy + amber accents
    { "Fusion Dark", 0x0841, 0x1082, 0x2945, 0xFFFF, 0x8410, 0x07FF, 0xFD20,
      0xC618, 0x39C7, 0x0500, 0x0293, 0xBDF7, 0xFA20, 0xF813, 0x03FF, 0x07E0, 0xFB48, 0xFFE0 },
    // FUSION LIGHT
    { "Fusion Light", 0xE71C, 0xD69A, 0xA514, 0x1082, 0x8410, 0x001F, 0xFA00,
      0x4208, 0xB596, 0x2D68, 0x459F, 0x8410, 0x0448, 0xC0C4, 0x2196, 0x001F, 0xC28C, 0x8A00 },
    // SERIOUS: navy + brass
    { "Serious", 0x0021, 0x18A5, 0x4188, 0xFFFF, 0x8410, 0xBD60, 0xFFE0,
      0xC618, 0x39C7, 0x0500, 0x0293, 0xBDF7, 0xFA20, 0x8945, 0xBD60, 0x07E0, 0xFB48, 0xBD60 },
    // CHILL: teal + coral
    { "Chill", 0x0329, 0x1CB1, 0x3E9C, 0xFFFF, 0x8534, 0x67FF, 0xFB0D,
      0xEE7C, 0x4A69, 0x0620, 0x057D, 0xCEFB, 0xFD79, 0xFACA, 0x67FF, 0xF800, 0xFB0D, 0xFFDA },
};
static uint8_t g_theme_id = THEME_FUSION_DARK;
static inline const ThemePalette &TH() { return THEMES[g_theme_id]; }

// Track B -- shared LVGL style system. Push the active theme's colours into the
// central styles used by every menu/card. Call after any theme change.
// ===== BEGIN inlined styles.h (shared LVGL style system) =====
// =============================================================================
//  styles.h -- WaypointOS shared LVGL style system (Track B)
//
//  One place for the look of every menu/card/button so screens stop hand-rolling
//  their own colours, radii and borders. Screens build UI from these styles and
//  from wp_create_menu_item(), giving a consistent look + press feedback.
//
//  Self-contained: depends only on lvgl.h. Themes are RGB565 (Arduino_GFX), so
//  wp_565() converts to LVGL's lv_color_t. Call wp_styles_init() once, then
//  wp_styles_set_palette(...) whenever the active theme changes.
//
//  Shared styles:
//     wp_style_menu_button   - tappable tile (tools grid)
//     wp_style_menu_card     - larger content card (library, POI)
//     wp_style_pressed       - shared pressed/active feedback (add to any btn)
//     wp_style_label_title / _body / _dim
// =============================================================================


// RGB565 (Arduino_GFX theme colour) -> LVGL lv_color_t.
static inline lv_color_t wp_565(uint16_t c) {
    uint8_t r = (uint8_t)(((c >> 11) & 0x1F) * 255 / 31);
    uint8_t g = (uint8_t)(((c >> 5)  & 0x3F) * 255 / 63);
    uint8_t b = (uint8_t)(( c        & 0x1F) * 255 / 31);
    return lv_color_make(r, g, b);
}

// Shared style objects (single instance; colours updated on theme change).
static lv_style_t wp_style_menu_button;
static lv_style_t wp_style_menu_card;
static lv_style_t wp_style_pressed;
static lv_style_t wp_style_label_title;
static lv_style_t wp_style_label_body;
static lv_style_t wp_style_label_dim;
static bool       wp_styles_ready = false;

// Cached palette (RGB565) so labels/icons can match without a global dependency.
static uint16_t   wp_pal_text = 0xFFFF, wp_pal_dim = 0x8410,
                  wp_pal_accent = 0x07FF, wp_pal_warn = 0xFD20;

static void wp_styles_init() {
    if (wp_styles_ready) return;
    wp_styles_ready = true;

    lv_style_init(&wp_style_menu_button);
    lv_style_set_radius(&wp_style_menu_button, 14);
    lv_style_set_border_width(&wp_style_menu_button, 1);
    lv_style_set_pad_all(&wp_style_menu_button, 6);
    lv_style_set_shadow_width(&wp_style_menu_button, 0);

    lv_style_init(&wp_style_menu_card);
    lv_style_set_radius(&wp_style_menu_card, 16);
    lv_style_set_border_width(&wp_style_menu_card, 1);
    lv_style_set_pad_all(&wp_style_menu_card, 14);

    // Shared pressed/active feedback: subtle scale + brighter border.
    lv_style_init(&wp_style_pressed);
    lv_style_set_transform_width(&wp_style_pressed, -2);
    lv_style_set_transform_height(&wp_style_pressed, -2);

    lv_style_init(&wp_style_label_title);
    lv_style_set_text_font(&wp_style_label_title, &lv_font_montserrat_14);

    lv_style_init(&wp_style_label_body);
    lv_style_set_text_font(&wp_style_label_body, &lv_font_montserrat_14);

    lv_style_init(&wp_style_label_dim);
    lv_style_set_text_font(&wp_style_label_dim, &lv_font_montserrat_12);
    lv_style_set_text_letter_space(&wp_style_label_dim, 2);
}

// Push the active theme's colours into the shared styles (RGB565 in).
static void wp_styles_set_palette(uint16_t card_bg, uint16_t border,
                                  uint16_t text, uint16_t dim,
                                  uint16_t accent, uint16_t warn) {
    wp_styles_init();
    wp_pal_text = text; wp_pal_dim = dim; wp_pal_accent = accent; wp_pal_warn = warn;

    lv_color_t c_card = wp_565(card_bg);
    lv_color_t c_bord = wp_565(border);
    lv_color_t c_acc  = wp_565(accent);

    lv_style_set_bg_color(&wp_style_menu_button, c_card);
    lv_style_set_bg_opa(&wp_style_menu_button, LV_OPA_COVER);
    lv_style_set_border_color(&wp_style_menu_button, c_bord);

    lv_style_set_bg_color(&wp_style_menu_card, c_card);
    lv_style_set_bg_opa(&wp_style_menu_card, LV_OPA_COVER);
    lv_style_set_border_color(&wp_style_menu_card, c_acc);

    lv_style_set_border_color(&wp_style_pressed, c_acc);
    lv_style_set_border_width(&wp_style_pressed, 2);

    lv_style_set_text_color(&wp_style_label_title, wp_565(text));
    lv_style_set_text_color(&wp_style_label_body,  wp_565(text));
    lv_style_set_text_color(&wp_style_label_dim,   wp_565(dim));
}

// -----------------------------------------------------------------------------
//  MenuItem abstraction -- declarative {icon, label, callback} -> one builder.
//  icon is an LVGL symbol glyph string (LV_SYMBOL_*) or nullptr.
// -----------------------------------------------------------------------------
struct MenuItem {
    const char   *icon;    // LV_SYMBOL_* glyph or nullptr
    const char   *label;
    lv_event_cb_t cb;      // CLICKED handler
    void         *user;    // user_data passed to cb (freed by caller if heap)
};

// Build one consistent menu tile: icon on top, label below, shared style +
// pressed feedback. Returns the button so callers can tweak width in a flex row.
static lv_obj_t *wp_create_menu_item(lv_obj_t *parent, const MenuItem &it) {
    wp_styles_init();
    lv_obj_t *b = lv_btn_create(parent);
    lv_obj_add_style(b, &wp_style_menu_button, 0);
    lv_obj_add_style(b, &wp_style_pressed, LV_STATE_PRESSED);
    lv_obj_set_size(b, 108, 76);
    lv_obj_set_flex_flow(b, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(b, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    if (it.icon) {
        lv_obj_t *ic = lv_label_create(b);
        lv_label_set_text(ic, it.icon);
        lv_obj_set_style_text_color(ic, wp_565(wp_pal_accent), 0);
        lv_obj_set_style_text_font(ic, &lv_font_montserrat_14, 0);
    }
    lv_obj_t *l = lv_label_create(b);
    lv_label_set_text(l, it.label);
    lv_obj_add_style(l, &wp_style_label_body, 0);
    lv_label_set_long_mode(l, LV_LABEL_LONG_DOT);
    lv_obj_set_width(l, lv_pct(100));
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);

    if (it.cb) lv_obj_add_event_cb(b, it.cb, LV_EVENT_CLICKED, it.user);
    return b;
}
// ===== END inlined styles.h =====

static void wp_sync_styles() {
    const ThemePalette &t = TH();
    wp_styles_set_palette(t.chrome_bg, t.chrome_border, t.text, t.text_dim,
                          t.accent, t.warn);
}

// =============================================================================
//  Layers
// =============================================================================
// -----------------------------------------------------------------------------
//  RGB565 colour helpers used by the map styler (blending, dimming, tints).
//  These replace the old habit of hardcoding a second, slightly-off colour for
//  every shade the map needed.
// -----------------------------------------------------------------------------
static inline uint16_t wp_rgb565(uint8_t r, uint8_t g, uint8_t b) {
    return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}
// Blend a -> b; t = 0 returns a, t = 255 returns b.
static inline uint16_t wp_mix565(uint16_t a, uint16_t b, uint8_t t) {
    int ar = (a >> 11) & 0x1F, ag = (a >> 5) & 0x3F, ab = a & 0x1F;
    int br = (b >> 11) & 0x1F, bg = (b >> 5) & 0x3F, bb = b & 0x1F;
    int r  = ar + ((br - ar) * (int)t) / 255;
    int g  = ag + ((bg - ag) * (int)t) / 255;
    int bl = ab + ((bb - ab) * (int)t) / 255;
    return (uint16_t)((r << 11) | (g << 5) | bl);
}
// Perceived brightness 0..255; lets the styler pick light-on-dark vs
// dark-on-light without knowing which theme is active.
static inline uint8_t wp_luma565(uint16_t c) {
    return (uint8_t)((((c >> 11) & 0x1F) * 77 + ((c >> 5) & 0x3F) * 151 +
                      (c & 0x1F) * 28) >> 8);
}

// -----------------------------------------------------------------------------
//  LayerId doubles as PAINT ORDER: lower index = drawn first (furthest back).
//  draw_layers() walks this enum front-to-back, so the stack is
//  earth -> land cover -> land use -> water -> parks -> schools -> buildings ->
//  transit -> paths -> roads -> boundary, then the CSV/user overlays on top.
//
//  NOTE: inserting a layer moves every later bit position, so the per-map layer
//  mask is keyed on a hash of the map's *title* rather than on its bit layout
//  (see wp_map_key / layers_toggle). Adding a layer therefore costs every map
//  its toggles once, instead of silently reassigning them to the wrong layers,
//  and two maps whose titles share a long prefix can no longer collide.
// -----------------------------------------------------------------------------
enum LayerId {
    LAY_EARTH = 0,          // base land plate (Protomaps earth): the ground
    LAY_LANDCOVER,          // natural cover: scrub, sand, rock, farm, wetland
    LAY_LANDUSE,            // residential / commercial / industrial ground tint
    LAY_WATER,              // sea, lakes, rivers, waterway lines
    LAY_PARKS,              // parks, woods, grass, sand, pitches
    LAY_SCHOOLS,            // education grounds / campuses
    LAY_BUILDINGS,          // building footprints
    LAY_TRANSIT,            // rail, tram, ferry, aerialway
    LAY_PATHS,              // footway / cycleway / track
    LAY_ROADS,              // streets (casing pass, then fill pass)
    LAY_BOUNDARY,           // admin boundaries
    LAY_SHOPS,              // amenity / shop points + labels
    LAY_POI,                // generic POI + place points + labels
    LAY_BEACONS,            // Meshtastic-style beacons (CSV)
    LAY_SAVED,              // user-saved waypoints
    LAY_BREADCRUMBS,        // dropped breadcrumbs
    LAY_UNKNOWN,            // unmapped layer names (thin grey outline)
    LAY__COUNT
};
static const char *LAYER_NAMES[LAY__COUNT] = {
    "Ground","Land cover","Land use","Water","Parks","Schools","Buildings",
    "Transit","Paths","Roads","Boundary",
    "Shops","POI","Beacons","Saved","Breadcrumbs","Unknown"
};
static uint32_t g_layer_mask = 0xFFFFFFFFu; // all on

static uint16_t layer_colour(uint8_t l) {
    const ThemePalette &t = TH();
    switch (l) {
        // The ground the whole map sits on, then the natural cover over it.
        // Both derive from the theme instead of being hardcoded, so a light
        // theme gets pale ground and a dark theme keeps dark ground without a
        // second palette that can drift out of sync. The per-kind tints
        // (scrub vs sand vs forest) are applied in mvt_style_object().
        case LAY_EARTH:       return wp_mix565(t.bg, t.text_dim, 26);
        case LAY_LANDCOVER:   return wp_mix565(t.bg, t.text_dim, 62);
        case LAY_LANDUSE:     return wp_mix565(t.bg, t.text_dim, 46);
        case LAY_WATER:       return t.water;
        case LAY_PARKS:       return t.parks;
        case LAY_SCHOOLS:     return t.schools;
        case LAY_BUILDINGS:   return t.buildings;
        case LAY_TRANSIT:     return t.transit;
        case LAY_PATHS:       return t.paths;
        case LAY_ROADS:       return t.roads;
        case LAY_BOUNDARY:    return t.text_dim;
        case LAY_SHOPS:       return t.shops;
        case LAY_POI:         return t.shops;
        case LAY_BEACONS:     return t.beacons;
        case LAY_SAVED:       return t.saved;
        case LAY_BREADCRUMBS: return t.breadcrumb;
        case LAY_UNKNOWN:     return t.text_dim;
    }
    return t.text;
}

// =============================================================================
// ===== BEGIN WAYPOINT_MAP_ENGINE =============================================
//  Everything between this marker and END WAYPOINT_MAP_ENGINE is deliberately
//  free of LVGL / Arduino_GFX / panel dependencies, so the whole tile pipeline
//  (gzip inflate -> protobuf/MVT decode -> layer classify -> tile cache) can be
//  compiled and unit-tested on a PC. See tools/host_test/ and run_tests.sh.
// ===== do not use lv_* or gfx-> inside this region =====
//  MVT (Mapbox Vector Tile) format constants + WaypointOS layer mapping
//  Spec: https://github.com/mapbox/vector-tile-spec/tree/master/2.1
//  Tiles live at   /WATCH/WayPoint/<map>/{z}/{x}/{y}.pbf   (also accepts .mvt)
// =============================================================================
// Layer names are matched CASE-INSENSITIVELY and EXACTLY. That distinction is
// the whole point of this function: the previous "contains" scan tested
// "transportation" before anything else, so every rail line, footway, cycleway
// and ferry route in the OpenMapTiles transportation layer was painted as a
// road, and "water" matched "water_name"/"waterway_name".
//
// Exact matching maps the *layer*; the feature's `class`/`subclass` tags then
// decide rail-vs-street and park-vs-car-park (see mvt_refine_layer). A short
// list of substring aliases is kept at the end so hand-rolled converters
// ("osm_roads", "custom_building_poly") still land somewhere sane.
static inline char mvt_tolower(char c) { return (c >= 'A' && c <= 'Z') ? c + 32 : c; }

// Legacy fallback: case-insensitive substring test.
static bool mvt_name_contains(const char *name, uint16_t nlen, const char *needle) {
    size_t nn = strlen(needle);
    if (nn == 0 || nlen < nn) return false;
    for (size_t i = 0; i + nn <= (size_t)nlen; i++) {
        bool ok = true;
        for (size_t k = 0; k < nn; k++) {
            if (mvt_tolower(name[i + k]) != mvt_tolower(needle[k])) { ok = false; break; }
        }
        if (ok) return true;
    }
    return false;
}

// Case-insensitive EXACT match of a length-delimited MVT string.
static inline bool mvt_eq_ci(const char *a, uint16_t n, const char *lit) {
    size_t m = strlen(lit);
    if (!a || n != m) return false;
    for (size_t i = 0; i < m; i++)
        if (mvt_tolower(a[i]) != mvt_tolower(lit[i])) return false;
    return true;
}

static uint8_t mvt_layer_id(const char *name, uint16_t nlen) {
    if (!name || nlen == 0) return LAY_UNKNOWN;

    // Label-only layers: we rasterise our own text from the feature tags, so
    // their geometry is dropped rather than drawn as anonymous grey lines.
    if (mvt_eq_ci(name, nlen, "housenumber"))          return 0xFF;
    if (mvt_eq_ci(name, nlen, "transportation_name"))  return 0xFF;
    if (mvt_eq_ci(name, nlen, "water_name"))           return 0xFF;
    if (mvt_eq_ci(name, nlen, "waterway_name"))        return 0xFF;
    if (mvt_eq_ci(name, nlen, "mountain_peak"))        return LAY_POI;
    if (mvt_eq_ci(name, nlen, "aerodrome_label"))      return LAY_POI;

    // Canonical OpenMapTiles / tilemaker default-profile layers.
    if (mvt_eq_ci(name, nlen, "transportation"))       return LAY_ROADS;   // refined by class
    if (mvt_eq_ci(name, nlen, "building"))             return LAY_BUILDINGS;
    if (mvt_eq_ci(name, nlen, "water"))                return LAY_WATER;
    if (mvt_eq_ci(name, nlen, "waterway"))             return LAY_WATER;
    if (mvt_eq_ci(name, nlen, "landcover"))            return LAY_LANDCOVER; // refined by class
    if (mvt_eq_ci(name, nlen, "landuse"))              return LAY_LANDUSE; // refined by class
    if (mvt_eq_ci(name, nlen, "park"))                 return LAY_PARKS;
    if (mvt_eq_ci(name, nlen, "boundary"))             return LAY_BOUNDARY;
    if (mvt_eq_ci(name, nlen, "aerialway"))            return LAY_TRANSIT;
    if (mvt_eq_ci(name, nlen, "poi"))                  return LAY_POI;
    if (mvt_eq_ci(name, nlen, "place"))                return LAY_POI;

    // Protomaps basemaps v4 -- the schema behind the whole-of-WA map built by
    // tools/build_wa_map.sh. The layer names differ from OpenMapTiles, and the
    // feature's `kind`/`kind_detail` pair carries what OpenMapTiles puts in
    // `class`/`subclass` (mvt_read_tags normalises the two).
    if (mvt_eq_ci(name, nlen, "roads"))                return LAY_ROADS;     // refined by kind
    if (mvt_eq_ci(name, nlen, "buildings"))            return LAY_BUILDINGS;
    if (mvt_eq_ci(name, nlen, "places"))               return LAY_POI;       // city/town/suburb
    if (mvt_eq_ci(name, nlen, "pois"))                 return LAY_POI;       // refined by kind
    if (mvt_eq_ci(name, nlen, "boundaries"))           return LAY_BOUNDARY;
    if (mvt_eq_ci(name, nlen, "earth"))                return LAY_EARTH;     // base land plate
    if (mvt_eq_ci(name, nlen, "transit"))              return LAY_TRANSIT;
    if (mvt_eq_ci(name, nlen, "physical_line"))        return LAY_WATER;
    if (mvt_eq_ci(name, nlen, "physical_point"))       return LAY_POI;

    // Legacy / hand-rolled converter aliases, best effort.
    if (mvt_name_contains(name, nlen, "rail"))         return LAY_TRANSIT;
    if (mvt_name_contains(name, nlen, "transit"))      return LAY_TRANSIT;
    if (mvt_name_contains(name, nlen, "ferry"))        return LAY_TRANSIT;
    if (mvt_name_contains(name, nlen, "aero"))         return LAY_TRANSIT;
    if (mvt_name_contains(name, nlen, "path"))         return LAY_PATHS;
    if (mvt_name_contains(name, nlen, "footway"))      return LAY_PATHS;
    if (mvt_name_contains(name, nlen, "cycleway"))     return LAY_PATHS;
    if (mvt_name_contains(name, nlen, "trail"))        return LAY_PATHS;
    if (mvt_name_contains(name, nlen, "road"))         return LAY_ROADS;
    if (mvt_name_contains(name, nlen, "highway"))      return LAY_ROADS;
    if (mvt_name_contains(name, nlen, "street"))       return LAY_ROADS;
    if (mvt_name_contains(name, nlen, "build"))        return LAY_BUILDINGS;
    if (mvt_name_contains(name, nlen, "ocean"))        return LAY_WATER;
    if (mvt_name_contains(name, nlen, "river"))        return LAY_WATER;
    if (mvt_name_contains(name, nlen, "lake"))         return LAY_WATER;
    if (mvt_name_contains(name, nlen, "forest"))       return LAY_PARKS;
    if (mvt_name_contains(name, nlen, "wood"))         return LAY_PARKS;
    if (mvt_name_contains(name, nlen, "grass"))        return LAY_PARKS;
    if (mvt_name_contains(name, nlen, "school"))       return LAY_SCHOOLS;
    if (mvt_name_contains(name, nlen, "shop"))         return LAY_SHOPS;
    if (mvt_name_contains(name, nlen, "amenity"))      return LAY_SHOPS;
    if (mvt_name_contains(name, nlen, "boundary"))     return LAY_BOUNDARY;
    if (mvt_name_contains(name, nlen, "admin"))        return LAY_BOUNDARY;
    if (mvt_name_contains(name, nlen, "border"))       return LAY_BOUNDARY;

    // Unknown: drawn as a thin grey outline when MVT_DRAW_UNKNOWN=1 so the map
    // is never blank, and named in the MVT_DEBUG log so it can be baked in.
    return LAY_UNKNOWN;
}

// -----------------------------------------------------------------------------
//  Road classes, ordered MINOR -> MAJOR. The renderer paints roads in ascending
//  sort_key, so motorways naturally end up on top of the residential streets
//  they cross, exactly like a published map.
// -----------------------------------------------------------------------------
enum MvtRoadClass {
    RC_SERVICE = 0, RC_MINOR, RC_TERTIARY, RC_SECONDARY, RC_PRIMARY,
    RC_TRUNK, RC_MOTORWAY, RC_RAIL, RC_PATH, RC_TRACK, RC_OTHER,
    RC__COUNT
};

// Base drawn width in pixels per class at the map's reference (native) zoom.
static const uint8_t ROAD_WIDTH_BASE[RC__COUNT] = {
    /*SERVICE  */ 1, /*MINOR  */ 2, /*TERTIARY */ 3, /*SECONDARY*/ 4,
    /*PRIMARY  */ 5, /*TRUNK  */ 6, /*MOTORWAY */ 7, /*RAIL     */ 1,
    /*PATH     */ 1, /*TRACK  */ 1, /*OTHER    */ 2
};

// -----------------------------------------------------------------------------
//  Surface kind for the ground layers. Both schemas describe what the ground
//  *is* in a `class`/`kind` tag (scrub, sand, forest, farmland, built-up...).
//  On a map of a 2.5 million km2 state that tag is the difference between
//  "desert", "wheatbelt" and "lawn", so one byte per polygon carries it from
//  the decoder to the styler in FMapObj::sub.
// -----------------------------------------------------------------------------
enum MvtCover {
    COV_NONE = 0, COV_EARTH, COV_BUILT, COV_FOREST, COV_GRASS, COV_SCRUB,
    COV_SAND, COV_ROCK, COV_FARM, COV_WETLAND, COV__COUNT
};

// =============================================================================
//  In-memory structures (kept small; heap tuned for ESP32-S3 PSRAM)
// =============================================================================
struct FVec2 { int32_t x, y; };

// A filled polygon is one exterior ring plus zero or more holes. `ring_end`
// describes the ring boundaries inside `points` so the scanline filler can
// apply even-odd winding and leave courtyards, lakes and islands unpainted.
struct FMapObj {
    uint8_t   type;        // OBJ_LINE / OBJ_POLY / OBJ_POINT
    uint8_t   layer;       // LayerId -- also the paint order index
    uint8_t   zmin, zmax;  // slippy zoom visibility range
    uint8_t   flags;       // OF_FILLED / OF_DASHED
    uint8_t   sort_key;    // within-layer paint order (road class weight)
    uint8_t   sub;         // style sub-key (road class) for width lookup
    uint8_t   rings;       // number of closed rings in `points` (>=1 for polys)
    uint16_t  point_count; // vertices across all rings
    uint16_t *ring_end;    // nullptr => single ring; else last-index+1 per ring
    FVec2    *points;      // heap alloc, ring 0 first
};

struct FMapPOI {
    int32_t  x, y;
    uint8_t  layer;        // which LayerId produced it (also the mask bit)
    uint8_t  type;         // icon family: 1 shop, 3 transit, 4 school, 5 park
    uint8_t  rank;         // 0 = most important, drives label priority
    uint8_t  zmin;         // hide below this effective zoom
    char    *name;         // NUL-terminated, never null
};

struct FMapMeta {
    String title, desc;
    uint32_t created;
    float min_lat, min_lon, max_lat, max_lon;
    float origin_lat, origin_lon;
    float units_per_metre;
    uint8_t projection;
    uint8_t minzoom, maxzoom, refzoom;
};

struct FMapZoom {
    uint8_t zoom_id;
    float min_px_scale;    // world-units per pixel where this zoom is preferred
    uint32_t object_count; // legacy field (kept for dev screen); tile count actually
};

struct FMapStyle {
    bool set;
    uint16_t colour;
    uint8_t width_px;
    uint8_t dash_len;
    uint8_t z_min;
};

// Per-tile load status (Track A: per-tile status instead of silent blanks).
enum TileStatus : uint8_t {
    TILE_ST_EMPTY = 0,   // slot never used
    TILE_ST_OK,          // decoded, >=1 object
    TILE_ST_NODATA,      // decoded ok but 0 objects (genuinely empty tile)
    TILE_ST_MISSING,     // no file on disk
    TILE_ST_GZIP,        // gzip stream that failed to inflate
    TILE_ST_OOM,         // allocation failed
    TILE_ST_BADSIZE,     // file empty / too large
    TILE_ST_IOERR        // SD open / short read
};

// Loaded tile (decoded objects held in RAM/PSRAM). Keyed by (z, x, y).
//
// tx/ty are int32_t, NOT int16_t. A z17 tile index for Perth is about 107 000;
// the old int16_t fields wrapped it to -23 302, so every lookup at high zoom
// resolved to a tile that does not exist and the map silently went blank.
struct FMapTile {
    int32_t  tx, ty;
    int8_t   z;
    bool     loaded;
    uint8_t  status;      // TileStatus
    uint16_t obj_count;
    FMapObj *objs;        // sorted by (layer, sort_key)
    uint16_t band[LAY__COUNT + 1];  // band[L]..band[L+1]-1 = objects in layer L
    uint32_t last_use_ms;
};

// Every zoom level the map actually has is kept, up to z22 (metadata maxzoom is
// clamped there when it is read). This used to be 8, and the loop in
// mvt_map_open() fills the list from minzoom UPWARD, so any map spanning more
// levels silently lost its highest ones: the whole-of-WA build (z0-z15)
// recorded only z0..z7, which left cam_select_zoom() unable to ever choose a
// street-level zoom. The z12-15 metro and town tiers were on the SD card and
// unreachable, and the state tiles were merely overzoomed harder. 24 entries x
// 12 bytes is nothing.
#define MAX_ZOOMS   24
#define TILE_CACHE_SLOTS 28   // 3x3 ring + overzoom parents + radius-2 prefetch
#define MAX_POIS 2048
// Overzoom: how many parent zoom levels to walk up when a tile is missing.
#define TILE_OVERZOOM_MAX 5
// Soft PSRAM budget for decoded tile geometry (bytes). Cache evicts past this.
#define TILE_CACHE_PSRAM_BUDGET (3u * 1024u * 1024u)

// =============================================================================
// MVT geometry type constants (per Mapbox Vector Tile spec 2.1)
//   https://github.com/mapbox/vector-tile-spec/tree/master/2.1
// The 'type' field on a Feature is one of these four values.
// =============================================================================
#define MVT_UNKNOWN         0
#define MVT_POINT           1
#define MVT_LINESTRING      2
#define MVT_POLYGON         3

// Default tile extent (MVT spec default is 4096 units per tile side).
#define MVT_EXTENT_DEFAULT  4096

// =============================================================================
// Internal FMapObj type tags. Any distinct uint8_t values are fine; these
// map 1:1 to the MVT geometry types after decoding.
// =============================================================================
#define OBJ_LINE            0
#define OBJ_POLY            1
#define OBJ_POINT           2

// FMapObj::flags
#define OF_FILLED           0x01
#define OF_DASHED           0x02

// Limits for one layer of one tile. A feature's packed `tags` reference the
// layer's keys/values BY INDEX, so both tables must stay resident while the
// layer is decoded. They are file-scope rather than stack to keep the decode
// frame small; decoding is never re-entrant.
#define MVT_MAX_KEYS   96
#define MVT_MAX_VALS   192
#define MVT_MAX_RINGS  48
#define MVT_MAX_RING_PTS 8192

// =============================================================================
//  Active map state (directory-of-MVT-tiles)
// =============================================================================
static String     g_map_folder = "/WATCH/WayPoint";
static String     g_map_dir_path;              // full SD path of the loaded map's dir
static bool       g_map_loaded = false;

static FMapMeta   g_meta;
static FMapZoom   g_zooms[MAX_ZOOMS];
static uint8_t    g_zoom_count = 0;
static uint32_t   g_tile_count = 0;            // total tile files discovered on SD

// Static POI list is accumulated as tiles are decoded (used by search).
static FMapPOI   *g_pois = nullptr;
static uint32_t   g_poi_count = 0;
static uint32_t   g_poi_cap = 0;

static FMapStyle  g_styles[LAY__COUNT];

// per-tile decoded cache
static FMapTile   g_cache[TILE_CACHE_SLOTS];
static int8_t     g_last_cached_z = -1;        // last zoom the cache was populated at

// =============================================================================
//  Small buffer IO helpers + protobuf/MVT varint decoders
// =============================================================================
static inline int32_t zz_decode(uint32_t v) {
    return (int32_t)((v >> 1) ^ -((int32_t)(v & 1)));
}

// Read varint from a byte buffer. Advances *pp. Returns 0 on truncation.
static uint32_t pb_varint(const uint8_t *&p, const uint8_t *end) {
    uint32_t r = 0; int shift = 0;
    while (p < end) {
        uint8_t b = *p++;
        r |= (uint32_t)(b & 0x7F) << shift;
        if (!(b & 0x80)) return r;
        shift += 7;
        if (shift > 32) break;
    }
    return r;
}
static uint64_t pb_varint64(const uint8_t *&p, const uint8_t *end) {
    uint64_t r = 0; int shift = 0;
    while (p < end) {
        uint8_t b = *p++;
        r |= (uint64_t)(b & 0x7F) << shift;
        if (!(b & 0x80)) return r;
        shift += 7;
        if (shift > 64) break;
    }
    return r;
}

// Skip a protobuf field of the given wire type.
static void pb_skip(const uint8_t *&p, const uint8_t *end, uint8_t wire) {
    switch (wire) {
        case 0: pb_varint64(p, end); break;               // varint
        case 1: if (p + 8 <= end) p += 8; else p = end; break;    // 64-bit
        case 2: {                                                 // length-delimited
            uint32_t n = pb_varint(p, end);
            if (p + n <= end) p += n; else p = end;
            break;
        }
        case 5: if (p + 4 <= end) p += 4; else p = end; break;    // 32-bit
        default: p = end;
    }
}

// =============================================================================
//  Web Mercator projection helpers
//  World units: MVT-native at the map's reference zoom (=maxzoom, EXTENT=4096).
//  world_x grows east, world_y grows NORTH (we flip mercator so N is up on screen).
// =============================================================================
static inline double mercator_lon_to_world_x(double lon, uint8_t refzoom) {
    double n = (double)(1u << refzoom);
    return ((lon + 180.0) / 360.0) * n * (double)MVT_EXTENT_DEFAULT;
}
static inline double mercator_lat_to_world_y(double lat, uint8_t refzoom) {
    double lat_r = lat * M_PI / 180.0;
    double n = (double)(1u << refzoom);
    // Standard slippy y (grows south); we return NEGATIVE to make +Y = north.
    double tile_y = (1.0 - log(tan(lat_r) + 1.0/cos(lat_r)) / M_PI) / 2.0 * n;
    return -(tile_y * (double)MVT_EXTENT_DEFAULT);
}
static double g_metres_per_world_unit = 1.0; // recomputed at map open

// Compute world coords of the top-left corner of MVT tile (z, x, y).
// (world y is flipped so north = +Y on screen)
static inline void mvt_tile_topleft_world(int8_t z, int32_t x, int32_t y,
                                          uint8_t refzoom,
                                          double *ox, double *oy) {
    int shift = refzoom - z;   // may be negative if z > refzoom; capped elsewhere
    double scale = (shift >= 0) ? (double)(1u << shift) : 1.0 / (double)(1u << -shift);
    *ox = (double)x * (double)MVT_EXTENT_DEFAULT * scale;
    // tile y grows south, so top-left in flipped coords = -(y * ext)
    *oy = -(double)y * (double)MVT_EXTENT_DEFAULT * scale;
}

// =============================================================================
//  Tile cache management (keyed by (z, x, y))
//  LRU by last_use_ms, with a soft PSRAM byte budget so a big region can't
//  balloon decoded geometry past what the ESP32-S3 can hold.
// =============================================================================
static uint32_t g_cache_bytes = 0;   // approx bytes of decoded geometry held

static uint32_t tile_bytes(const FMapTile &t) {
    uint32_t b = (uint32_t)t.obj_count * sizeof(FMapObj);
    for (uint16_t i = 0; i < t.obj_count; i++) {
        b += (uint32_t)t.objs[i].point_count * sizeof(FVec2);
        if (t.objs[i].ring_end) b += sizeof(uint16_t);  // holes are rare: count 1
    }
    return b;
}
static void tile_free(FMapTile &t) {
    if (t.objs) {
        g_cache_bytes -= tile_bytes(t);
        for (uint16_t i=0; i<t.obj_count; i++) {
            if (t.objs[i].points)   free(t.objs[i].points);
            if (t.objs[i].ring_end) free(t.objs[i].ring_end);
        }
        free(t.objs);
        t.objs = nullptr;
    }
    t.obj_count = 0;
    t.loaded = false;
    t.status = TILE_ST_EMPTY;
}
static void cache_reset() {
    for (int i=0; i<TILE_CACHE_SLOTS; i++) tile_free(g_cache[i]);
    g_cache_bytes = 0;
}
static FMapTile *cache_find(int8_t z, int32_t tx, int32_t ty) {
    for (int i=0; i<TILE_CACHE_SLOTS; i++)
        if (g_cache[i].loaded && g_cache[i].z==z &&
            g_cache[i].tx==tx && g_cache[i].ty==ty) {
            g_cache[i].last_use_ms = millis();
            return &g_cache[i];
        }
    return nullptr;
}
// Evict the least-recently-used loaded slot; also trims to the PSRAM budget.
static FMapTile *cache_evict_slot() {
    // Enforce byte budget first: drop LRU tiles until under budget (keep >=1 slot).
    while (g_cache_bytes > TILE_CACHE_PSRAM_BUDGET) {
        int lru = -1; uint32_t oldest = UINT32_MAX; int loaded = 0;
        for (int i=0; i<TILE_CACHE_SLOTS; i++) {
            if (!g_cache[i].loaded) continue;
            loaded++;
            if (g_cache[i].last_use_ms < oldest) { oldest = g_cache[i].last_use_ms; lru = i; }
        }
        if (loaded <= 1 || lru < 0) break;
        tile_free(g_cache[lru]);
    }
    int slot=0; uint32_t oldest=UINT32_MAX;
    for (int i=0; i<TILE_CACHE_SLOTS; i++) {
        if (!g_cache[i].loaded) return &g_cache[i];
        if (g_cache[i].last_use_ms < oldest) { oldest = g_cache[i].last_use_ms; slot=i; }
    }
    tile_free(g_cache[slot]);
    return &g_cache[slot];
}

// =============================================================================
//  MVT decoder
//
//  A Layer is {name, Features, keys, values, extent}. Feature tags are packed
//  varints of (key_index, value_index) pairs into the layer's own key/value
//  tables -- so names, road classes and label ranks are only reachable if both
//  tables are decoded. The previous decoder skipped them entirely, which is why
//  every POI came out unnamed and the Tools->Search screen could never match
//  anything.
// =============================================================================
struct MVTDecodeCtx {
    // World transform to apply to feature-local (lx, ly) coords:
    //   world_x = ox + lx * sx
    //   world_y = oy - ly * sy      (mercator y flip)
    double ox, oy;
    double sx, sy;
    int8_t z;                 // slippy zoom of this tile
};

// A length-delimited string view into the tile buffer (never copied).
struct MvtStr { const char *p; uint16_t n; };

// One entry of Layer.values.
struct MvtVal {
    uint8_t kind;   // 0 = string, 1 = number, 2 = bool
    uint8_t b;
    double  num;
    MvtStr  s;
};

struct MvtKeyTable {
    uint16_t key_count;
    uint16_t val_count;
    MvtStr   key[MVT_MAX_KEYS];
    MvtVal   val[MVT_MAX_VALS];
    // Key indexes we care about, resolved once per layer (-1 = absent).
    int16_t  i_name, i_class, i_subclass, i_rank, i_layer,
             i_brunnel, i_ramp, i_ref, i_type, i_place, i_aeroway,
             i_kind, i_kind_detail, i_min_zoom, i_is_tunnel, i_is_bridge,
             i_oneway, i_shield, i_sort_rank, i_pop_rank;
};

// Tags extracted for one feature.
struct MvtTags {
    MvtStr  name, klass, subclass, brunnel, ref;
    int32_t rank;
    int32_t layer_tag;
    int32_t sort_rank, pop_rank;   // Protomaps place importance, -1 = absent
    int16_t min_zoom;              // producer's own "first show here" hint
    int8_t  oneway;                // -1 backwards, 0 no hint, 1 forwards
};

static inline bool mvt_val_eq_ci(const MvtStr &s, const char *lit) {
    return s.p && mvt_eq_ci(s.p, s.n, lit);
}

static inline void mvt_tags_clear(MvtTags &t) {
    t.name.p = t.klass.p = t.subclass.p = t.brunnel.p = t.ref.p = nullptr;
    t.name.n = t.klass.n = t.subclass.n = t.brunnel.n = t.ref.n = 0;
    t.rank = -1;
    t.layer_tag = -1;
    t.sort_rank = -1;
    t.pop_rank = -1;
    t.min_zoom = -1;
    t.oneway = 0;
}

// Protomaps describes a place with `kind` instead of OpenMapTiles' numeric
// `rank`, so this turns one into the other. Without it every city on the state
// map would carry rank -1, which the label pass treats as "least important"
// and hides until z15 - i.e. no town names at all.
static int32_t mvt_place_rank_one(const MvtStr &c) {
    if (!c.p) return -1;
    if (mvt_val_eq_ci(c, "continent") || mvt_val_eq_ci(c, "country")) return 0;
    if (mvt_val_eq_ci(c, "state") || mvt_val_eq_ci(c, "region") ||
        mvt_val_eq_ci(c, "province") || mvt_val_eq_ci(c, "city"))     return 1;
    if (mvt_val_eq_ci(c, "town"))                                         return 2;
    if (mvt_val_eq_ci(c, "village"))                                      return 3;
    if (mvt_val_eq_ci(c, "suburb") || mvt_val_eq_ci(c, "quarter"))        return 4;
    if (mvt_val_eq_ci(c, "neighbourhood") || mvt_val_eq_ci(c, "macrohood")) return 5;
    if (mvt_val_eq_ci(c, "hamlet") || mvt_val_eq_ci(c, "locality"))       return 6;
    if (mvt_val_eq_ci(c, "isolated_dwelling") || mvt_val_eq_ci(c, "farm")) return 7;
    return -1;
}

static int32_t mvt_place_rank(const MvtTags &t) {
    // Both slots have to be consulted, and the specific one first: Protomaps
    // gives a place a near-useless `kind` ("locality") and puts the real value
    // in `kind_detail` ("city", "town", "suburb"), so testing `kind` first
    // would make Perth a rank-6 label.
    int32_t r = mvt_place_rank_one(t.subclass);
    if (r >= 0) return r;
    return mvt_place_rank_one(t.klass);
}

// Parse a Layer.values entry (protobuf Value message).
//   1:string  2:float  3:double  4:int64  5:uint64  6:sint64  7:bool
static void mvt_parse_value(const uint8_t *p, const uint8_t *end, MvtVal &out) {
    out.kind = 1; out.b = 0; out.num = 0;
    out.s.p = nullptr; out.s.n = 0;
    while (p < end) {
        uint32_t key   = pb_varint(p, end);
        uint8_t  wire  = key & 7;
        uint32_t field = key >> 3;
        if (field == 1 && wire == 2) {
            uint32_t n = pb_varint(p, end);
            if (p + n > end) { p = end; break; }
            out.kind = 0;
            out.s.p = (const char*)p;
            out.s.n = (uint16_t)n;
            p += n;
        } else if (field == 2 && wire == 5) {             // float
            if (p + 4 > end) { p = end; break; }
            float f; memcpy(&f, p, 4); out.num = (double)f; out.kind = 1; p += 4;
        } else if (field == 3 && wire == 1) {             // double
            if (p + 8 > end) { p = end; break; }
            double d; memcpy(&d, p, 8); out.num = d; out.kind = 1; p += 8;
        } else if ((field == 4 || field == 5) && wire == 0) {
            out.num = (double)pb_varint64(p, end); out.kind = 1;
        } else if (field == 6 && wire == 0) {             // sint64 (zigzag)
            uint64_t z = pb_varint64(p, end);
            out.num = (double)(int64_t)((z >> 1) ^ (uint64_t)(-(int64_t)(z & 1)));
            out.kind = 1;
        } else if (field == 7 && wire == 0) {
            out.b = (uint8_t)pb_varint(p, end); out.kind = 2;
        } else {
            pb_skip(p, end, wire);
        }
    }
}

static void mvt_resolve_keys(MvtKeyTable &kt) {
    kt.i_name = kt.i_class = kt.i_subclass = kt.i_rank = kt.i_layer =
    kt.i_brunnel = kt.i_ramp = kt.i_ref = kt.i_type = kt.i_place =
    kt.i_aeroway = kt.i_kind = kt.i_kind_detail = kt.i_min_zoom =
    kt.i_is_tunnel = kt.i_is_bridge = kt.i_oneway = kt.i_shield =
    kt.i_sort_rank = kt.i_pop_rank = -1;
    for (uint16_t i = 0; i < kt.key_count; i++) {
        const MvtStr &k = kt.key[i];
        if      (mvt_eq_ci(k.p, k.n, "name"))        kt.i_name        = (int16_t)i;
        else if (mvt_eq_ci(k.p, k.n, "class"))       kt.i_class       = (int16_t)i;
        else if (mvt_eq_ci(k.p, k.n, "subclass"))    kt.i_subclass    = (int16_t)i;
        else if (mvt_eq_ci(k.p, k.n, "rank"))        kt.i_rank        = (int16_t)i;
        else if (mvt_eq_ci(k.p, k.n, "layer"))       kt.i_layer       = (int16_t)i;
        else if (mvt_eq_ci(k.p, k.n, "brunnel"))     kt.i_brunnel     = (int16_t)i;
        else if (mvt_eq_ci(k.p, k.n, "ramp"))        kt.i_ramp        = (int16_t)i;
        else if (mvt_eq_ci(k.p, k.n, "ref"))         kt.i_ref         = (int16_t)i;
        else if (mvt_eq_ci(k.p, k.n, "type"))        kt.i_type        = (int16_t)i;
        else if (mvt_eq_ci(k.p, k.n, "place"))       kt.i_place       = (int16_t)i;
        else if (mvt_eq_ci(k.p, k.n, "aeroway"))     kt.i_aeroway     = (int16_t)i;
        // Protomaps basemaps v4 spellings.
        else if (mvt_eq_ci(k.p, k.n, "kind"))        kt.i_kind        = (int16_t)i;
        else if (mvt_eq_ci(k.p, k.n, "kind_detail")) kt.i_kind_detail = (int16_t)i;
        else if (mvt_eq_ci(k.p, k.n, "min_zoom"))    kt.i_min_zoom    = (int16_t)i;
        else if (mvt_eq_ci(k.p, k.n, "is_tunnel"))   kt.i_is_tunnel   = (int16_t)i;
        else if (mvt_eq_ci(k.p, k.n, "is_bridge"))   kt.i_is_bridge   = (int16_t)i;
        else if (mvt_eq_ci(k.p, k.n, "oneway"))      kt.i_oneway      = (int16_t)i;
        else if (mvt_eq_ci(k.p, k.n, "shield_text")) kt.i_shield      = (int16_t)i;
        else if (mvt_eq_ci(k.p, k.n, "sort_rank"))   kt.i_sort_rank   = (int16_t)i;
        else if (mvt_eq_ci(k.p, k.n, "population_rank")) kt.i_pop_rank = (int16_t)i;
    }
}

// Unpack a feature's packed (key,value) varint pairs into MvtTags.
//
// This is where the two schemas are reconciled: Protomaps' `kind` fills the
// class slot and `kind_detail` the subclass slot when OpenMapTiles' `class` /
// `subclass` are absent, so every classifier downstream sees one vocabulary.
// Its `is_tunnel`/`is_bridge` booleans become the `brunnel` string the styler
// already understands, and `min_zoom` is kept as the producer's own
// "show this from here" hint.
static void mvt_read_tags(const uint8_t *tp, const uint8_t *tend,
                          const MvtKeyTable &kt, MvtTags &t) {
    mvt_tags_clear(t);
    while (tp < tend) {
        uint32_t ki = pb_varint(tp, tend);
        uint32_t vi = pb_varint(tp, tend);
        if (vi >= kt.val_count) continue;
        uint8_t want =
            (ki == (uint32_t)kt.i_name)        ? 1  :
            (ki == (uint32_t)kt.i_class)       ? 2  :
            (ki == (uint32_t)kt.i_subclass)    ? 3  :
            (ki == (uint32_t)kt.i_rank)        ? 4  :
            (ki == (uint32_t)kt.i_layer)       ? 5  :
            (ki == (uint32_t)kt.i_brunnel)     ? 6  :
            (ki == (uint32_t)kt.i_ref)         ? 7  :
            (ki == (uint32_t)kt.i_kind)        ? 8  :
            (ki == (uint32_t)kt.i_kind_detail) ? 9  :
            (ki == (uint32_t)kt.i_min_zoom)    ? 10 :
            (ki == (uint32_t)kt.i_is_tunnel)   ? 11 :
            (ki == (uint32_t)kt.i_is_bridge)   ? 12 :
            (ki == (uint32_t)kt.i_oneway)      ? 13 :
            (ki == (uint32_t)kt.i_shield)      ? 14 :
            (ki == (uint32_t)kt.i_sort_rank)   ? 15 :
            (ki == (uint32_t)kt.i_pop_rank)    ? 16 : 0;
        if (!want) continue;
        const MvtVal &v = kt.val[vi];
        switch (want) {
            case 1: if (v.kind == 0) t.name     = v.s; break;
            case 2: if (v.kind == 0) t.klass    = v.s; break;
            case 3: if (v.kind == 0) t.subclass = v.s; break;
            case 4: t.rank      = (int32_t)v.num; break;
            case 5: t.layer_tag = (int32_t)v.num; break;
            case 6: if (v.kind == 0) t.brunnel  = v.s; break;
            case 7: if (v.kind == 0) t.ref      = v.s; break;
            case 8:                                    // kind -> class
                if (v.kind == 0 && !t.klass.p) t.klass = v.s;
                break;
            case 9:                                    // kind_detail -> subclass
                if (v.kind == 0 && !t.subclass.p) t.subclass = v.s;
                break;
            case 10:
                if (v.kind == 1) t.min_zoom = (int16_t)v.num;
                break;
            case 11:                                   // is_tunnel
                if (v.kind == 2 ? (v.b != 0)
                                : (v.kind == 0 && (mvt_val_eq_ci(v.s, "true") ||
                                                   mvt_val_eq_ci(v.s, "yes")))) {
                    t.brunnel.p = "tunnel"; t.brunnel.n = 6;
                }
                break;
            case 12:                                   // is_bridge
                if (v.kind == 2 ? (v.b != 0)
                                : (v.kind == 0 && (mvt_val_eq_ci(v.s, "true") ||
                                                   mvt_val_eq_ci(v.s, "yes")))) {
                    t.brunnel.p = "bridge"; t.brunnel.n = 6;
                }
                break;
            case 13:                                   // oneway
                if (v.kind == 0) {
                    if (mvt_val_eq_ci(v.s, "-1") || mvt_val_eq_ci(v.s, "reverse") ||
                        mvt_val_eq_ci(v.s, "backward")) t.oneway = -1;
                    else if (mvt_val_eq_ci(v.s, "yes") || mvt_val_eq_ci(v.s, "true") ||
                             mvt_val_eq_ci(v.s, "1")   || mvt_val_eq_ci(v.s, "forward"))
                        t.oneway = 1;
                } else if (v.kind == 2 && v.b) {
                    t.oneway = 1;
                }
                break;
            case 14:                                   // shield_text -> ref
                if (v.kind == 0 && !t.ref.p) t.ref = v.s;
                break;
            case 15:                                   // sort_rank
                if (v.kind == 1) t.sort_rank = (int32_t)v.num;
                break;
            case 16:                                   // population_rank
                if (v.kind == 1) t.pop_rank = (int32_t)v.num;
                break;
        }
    }
    // Rank drives label priority, so resolve it properly: an explicit
    // OpenMapTiles `rank` wins, then Protomaps' own display ordering, then
    // population, and finally the place kind itself - without that last step a
    // city would default to "rank 7" and no town name would ever appear.
    if (t.rank < 0) t.rank = mvt_place_rank(t);
    if (t.rank < 0 && t.pop_rank >= 0) t.rank = t.pop_rank;
    if (t.rank > 7) t.rank = 7;
    // (`sort_rank` is deliberately NOT used here: in Protomaps it is a
    //  draw-order value in the hundreds, not a 0-7 importance rank, so
    //  trusting it would push every label out to z15. It is parsed above
    //  only so the key cannot be mistaken for one of the others.)
}

// Map a feature's road / rail / path class onto the road class ladder.
//
// ONE table serves every schema, because the same words arrive in different
// slots: OpenMapTiles puts streets in `class` and rail/soft routes in
// `subclass`, while Protomaps v4 puts the raw OSM value in `kind_detail` and a
// coarse family in `kind` ("major_road", "minor_road", "path", "rail"...).
// mvt_read_tags normalises the two into class/subclass, and this walks the
// class slot, then the subclass slot, and the first one with an opinion wins.
static uint8_t mvt_road_class_str(const MvtStr &s) {
    if (!s.p) return RC__COUNT;                       // no opinion
    if (mvt_val_eq_ci(s, "motorway") || mvt_val_eq_ci(s, "motorway_link") ||
        mvt_val_eq_ci(s, "highway")  || mvt_val_eq_ci(s, "expressway"))
        return RC_MOTORWAY;
    if (mvt_val_eq_ci(s, "trunk") || mvt_val_eq_ci(s, "trunk_link"))
        return RC_TRUNK;
    if (mvt_val_eq_ci(s, "primary") || mvt_val_eq_ci(s, "primary_link") ||
        mvt_val_eq_ci(s, "major_road"))
        return RC_PRIMARY;
    if (mvt_val_eq_ci(s, "secondary") || mvt_val_eq_ci(s, "secondary_link"))
        return RC_SECONDARY;
    if (mvt_val_eq_ci(s, "tertiary") || mvt_val_eq_ci(s, "tertiary_link"))
        return RC_TERTIARY;
    // Rail, ferries, aerialways and everything else that is not a street.
    if (mvt_val_eq_ci(s, "rail") || mvt_val_eq_ci(s, "railway") ||
        mvt_val_eq_ci(s, "subway") || mvt_val_eq_ci(s, "light_rail") ||
        mvt_val_eq_ci(s, "monorail") || mvt_val_eq_ci(s, "tram") ||
        mvt_val_eq_ci(s, "narrow_gauge") || mvt_val_eq_ci(s, "preserved") ||
        mvt_val_eq_ci(s, "miniature") || mvt_val_eq_ci(s, "funicular") ||
        mvt_val_eq_ci(s, "transit") || mvt_val_eq_ci(s, "ferry") ||
        mvt_val_eq_ci(s, "ship") || mvt_val_eq_ci(s, "aerialway") ||
        mvt_val_eq_ci(s, "cable_car") || mvt_val_eq_ci(s, "gondola") ||
        mvt_val_eq_ci(s, "pier") || mvt_val_eq_ci(s, "aeroway") ||
        mvt_val_eq_ci(s, "runway") || mvt_val_eq_ci(s, "taxiway") ||
        mvt_val_eq_ci(s, "apron"))
        return RC_RAIL;
    // Soft routes.
    if (mvt_val_eq_ci(s, "path") || mvt_val_eq_ci(s, "footway") ||
        mvt_val_eq_ci(s, "cycleway") || mvt_val_eq_ci(s, "steps") ||
        mvt_val_eq_ci(s, "bridleway") || mvt_val_eq_ci(s, "pedestrian") ||
        mvt_val_eq_ci(s, "corridor") || mvt_val_eq_ci(s, "sidewalk") ||
        mvt_val_eq_ci(s, "crossing") || mvt_val_eq_ci(s, "platform"))
        return RC_PATH;
    if (mvt_val_eq_ci(s, "track") || mvt_val_eq_ci(s, "piste") ||
        mvt_val_eq_ci(s, "ski") || mvt_val_eq_ci(s, "raceway"))
        return RC_TRACK;
    if (mvt_val_eq_ci(s, "service") || mvt_val_eq_ci(s, "alley") ||
        mvt_val_eq_ci(s, "driveway") || mvt_val_eq_ci(s, "parking_aisle") ||
        mvt_val_eq_ci(s, "drive-through") || mvt_val_eq_ci(s, "emergency_access") ||
        mvt_val_eq_ci(s, "utility"))
        return RC_SERVICE;
    if (mvt_val_eq_ci(s, "minor") || mvt_val_eq_ci(s, "minor_road") ||
        mvt_val_eq_ci(s, "residential") || mvt_val_eq_ci(s, "unclassified") ||
        mvt_val_eq_ci(s, "living_street") || mvt_val_eq_ci(s, "road") ||
        mvt_val_eq_ci(s, "street"))
        return RC_MINOR;
    return RC__COUNT;
}

static uint8_t mvt_road_class(const MvtTags &t) {
    // The SUBCLASS is checked first because it is the more specific slot in
    // both schemas: Protomaps puts a coarse family in `kind` ("major_road")
    // and the real OSM value in `kind_detail` ("trunk", "secondary"), while
    // OpenMapTiles leaves `subclass` empty for ordinary streets but uses it
    // for rail and soft routes. Subclass, then class, first opinion wins -
    // otherwise every trunk road on the map is drawn as a primary road.
    uint8_t rc = mvt_road_class_str(t.subclass);
    if (rc != RC__COUNT) return rc;
    rc = mvt_road_class_str(t.klass);
    return (rc == RC__COUNT) ? RC_OTHER : rc;
}

// Subclass-only view, kept because the host tests call it by name.
static uint8_t mvt_class_from_subclass(const MvtTags &t) {
    uint8_t rc = mvt_road_class_str(t.subclass);
    return (rc == RC__COUNT) ? RC_OTHER : rc;
}

// What the ground *is*, for the layers that paint a surface. Reads whichever
// tag the schema provides, so OpenMapTiles' `class` and Protomaps' `kind` both
// land here.
static uint8_t mvt_cover_kind(uint8_t layer, const MvtTags &t) {
    if (layer == LAY_EARTH) return COV_EARTH;
    const MvtStr &c = t.klass.p ? t.klass : t.subclass;
    if (!c.p) return (layer == LAY_LANDUSE) ? COV_BUILT : COV_NONE;
    struct CoverWord { const char *word; uint8_t kind; };
    static const CoverWord WORDS[] = {
        { "scrub", COV_SCRUB },   { "heath", COV_SCRUB },    { "shrub", COV_SCRUB },
        { "sand", COV_SAND },     { "beach", COV_SAND },     { "dune", COV_SAND },
        { "shingle", COV_SAND },
        { "rock", COV_ROCK },     { "bare_rock", COV_ROCK }, { "glacier", COV_ROCK },
        { "ice", COV_ROCK },      { "barren", COV_ROCK },
        { "forest", COV_FOREST }, { "wood", COV_FOREST },    { "woodland", COV_FOREST },
        { "orchard", COV_FOREST },
        { "grass", COV_GRASS },   { "grassland", COV_GRASS },{ "meadow", COV_GRASS },
        { "park", COV_GRASS },    { "garden", COV_GRASS },   { "village_green", COV_GRASS },
        { "farm", COV_FARM },     { "farmland", COV_FARM },  { "farmyard", COV_FARM },
        { "allotments", COV_FARM },
        { "wetland", COV_WETLAND }, { "marsh", COV_WETLAND }, { "swamp", COV_WETLAND },
        { "residential", COV_BUILT }, { "commercial", COV_BUILT },
        { "industrial", COV_BUILT },  { "retail", COV_BUILT },
        { "urban", COV_BUILT },       { "urban_area", COV_BUILT },
        { "neighbourhood", COV_BUILT },
    };
    for (unsigned i = 0; i < sizeof(WORDS) / sizeof(WORDS[0]); i++)
        if (mvt_val_eq_ci(c, WORDS[i].word)) return WORDS[i].kind;
    return (layer == LAY_LANDUSE) ? COV_BUILT : COV_NONE;
}

// Route a feature from a coarse layer bucket into its final LayerId using the
// `class`/`subclass` tags. This is what stops rail lines being drawn as roads
// and car parks being painted bright green.
static uint8_t mvt_refine_layer(uint8_t base, const MvtTags &t) {
    if (base == LAY_ROADS || base == LAY_PATHS || base == LAY_TRANSIT) {
        uint8_t rc = mvt_road_class(t);
        if (rc == RC_RAIL) return LAY_TRANSIT;
        if (rc == RC_PATH || rc == RC_TRACK) return LAY_PATHS;
        return LAY_ROADS;
    }
    // The base land plate is never anything else: it IS the ground.
    if (base == LAY_EARTH) return LAY_EARTH;

    if (base == LAY_LANDUSE || base == LAY_PARKS || base == LAY_LANDCOVER) {
        const MvtStr &c = t.klass.p ? t.klass : t.subclass;
        if (c.p) {
            if (mvt_val_eq_ci(c, "park")     || mvt_val_eq_ci(c, "forest") ||
                mvt_val_eq_ci(c, "wood")     || mvt_val_eq_ci(c, "woodland") ||
                mvt_val_eq_ci(c, "grass")    || mvt_val_eq_ci(c, "grassland") ||
                mvt_val_eq_ci(c, "meadow")   || mvt_val_eq_ci(c, "nature_reserve") ||
                mvt_val_eq_ci(c, "national_park") || mvt_val_eq_ci(c, "protected_area") ||
                mvt_val_eq_ci(c, "golf_course") || mvt_val_eq_ci(c, "pitch") ||
                mvt_val_eq_ci(c, "garden")   || mvt_val_eq_ci(c, "village_green") ||
                mvt_val_eq_ci(c, "playground") || mvt_val_eq_ci(c, "cemetery") ||
                mvt_val_eq_ci(c, "recreation_ground") || mvt_val_eq_ci(c, "orchard") ||
                mvt_val_eq_ci(c, "zoo")      || mvt_val_eq_ci(c, "stadium"))
                return LAY_PARKS;
            if (mvt_val_eq_ci(c, "school")   || mvt_val_eq_ci(c, "college") ||
                mvt_val_eq_ci(c, "university") || mvt_val_eq_ci(c, "kindergarten"))
                return LAY_SCHOOLS;
            // Natural cover. Sand, rock, scrub and farmland are NOT parks:
            // painting the outback green would misrepresent the whole state,
            // and these are exactly the tags that say what the ground is.
            if (mvt_val_eq_ci(c, "sand")     || mvt_val_eq_ci(c, "beach") ||
                mvt_val_eq_ci(c, "dune")     || mvt_val_eq_ci(c, "shingle") ||
                mvt_val_eq_ci(c, "scrub")    || mvt_val_eq_ci(c, "heath") ||
                mvt_val_eq_ci(c, "shrub")    || mvt_val_eq_ci(c, "barren") ||
                mvt_val_eq_ci(c, "rock")     || mvt_val_eq_ci(c, "bare_rock") ||
                mvt_val_eq_ci(c, "glacier")  || mvt_val_eq_ci(c, "ice") ||
                mvt_val_eq_ci(c, "wetland")  || mvt_val_eq_ci(c, "marsh") ||
                mvt_val_eq_ci(c, "mud")      || mvt_val_eq_ci(c, "playa") ||
                mvt_val_eq_ci(c, "farm")     || mvt_val_eq_ci(c, "farmland") ||
                mvt_val_eq_ci(c, "farmyard") || mvt_val_eq_ci(c, "allotments"))
                return LAY_LANDCOVER;
        }
        return LAY_LANDUSE;
    }
    // Point-of-interest routing: a school is not a shop and a bus stop is not
    // a shop either, whichever schema supplied the word.
    if (base == LAY_POI || base == LAY_SHOPS) {
        const MvtStr &c = t.klass.p ? t.klass : t.subclass;
        const MvtStr &s = t.subclass.p ? t.subclass : t.klass;
        if (c.p || s.p) {
            if (mvt_val_eq_ci(c, "school")   || mvt_val_eq_ci(c, "college") ||
                mvt_val_eq_ci(c, "university") || mvt_val_eq_ci(c, "kindergarten") ||
                mvt_val_eq_ci(c, "library")  || mvt_val_eq_ci(s, "school") ||
                mvt_val_eq_ci(s, "college")  || mvt_val_eq_ci(s, "university"))
                return LAY_SCHOOLS;
            if (mvt_val_eq_ci(c, "station")  || mvt_val_eq_ci(c, "railway") ||
                mvt_val_eq_ci(c, "halt")     || mvt_val_eq_ci(c, "bus_stop") ||
                mvt_val_eq_ci(c, "tram_stop") || mvt_val_eq_ci(c, "airport") ||
                mvt_val_eq_ci(c, "aerodrome") || mvt_val_eq_ci(c, "ferry_terminal"))
                return LAY_TRANSIT;
            if (mvt_val_eq_ci(c, "park")     || mvt_val_eq_ci(c, "national_park") ||
                mvt_val_eq_ci(c, "nature_reserve") || mvt_val_eq_ci(c, "playground") ||
                mvt_val_eq_ci(c, "golf_course") || mvt_val_eq_ci(c, "pitch") ||
                mvt_val_eq_ci(c, "cemetery"))
                return LAY_PARKS;
        }
        return base;
    }
    return base;
}

// -----------------------------------------------------------------------------
//  Per-object zoom visibility.
//  Previously EVERY object carried zmin=0 / zmax=22, so a city tile painted its
//  full contents at every zoom and the map was unreadable zoomed out. These
//  bands follow the usual cartographic ladder: motorways early, buildings and
//  shops last.
// -----------------------------------------------------------------------------
static void mvt_zoom_range(uint8_t layer, const MvtTags &t, uint8_t rc,
                           uint8_t *zmin, uint8_t *zmax) {
    uint8_t lo = 12;
    switch (layer) {
        case LAY_EARTH:     lo = 0;  break;   // the ground is never hidden
        case LAY_LANDCOVER: lo = 6;  break;
        case LAY_LANDUSE:   lo = 6;  break;
        case LAY_WATER:     lo = 4;  break;
        case LAY_PARKS:     lo = 7;  break;
        case LAY_BOUNDARY:  lo = 4;  break;
        case LAY_UNKNOWN:   lo = 6;  break;
        case LAY_SCHOOLS:   lo = 12; break;
        case LAY_BUILDINGS: lo = 14; break;
        case LAY_TRANSIT:
        case LAY_PATHS:
        case LAY_ROADS:
            switch (rc) {
                case RC_MOTORWAY:  lo = 7;  break;
                case RC_TRUNK:     lo = 8;  break;
                case RC_PRIMARY:   lo = 9;  break;
                case RC_RAIL:      lo = 10; break;
                case RC_SECONDARY: lo = 10; break;
                case RC_TERTIARY:  lo = 11; break;
                case RC_MINOR:     lo = 12; break;
                case RC_PATH:      lo = 13; break;
                case RC_TRACK:     lo = 13; break;
                case RC_SERVICE:   lo = 14; break;
                default:           lo = 13; break;
            }
            break;
        case LAY_SHOPS:
        case LAY_POI:
            if (t.rank >= 0) {
                if      (t.rank <= 2) lo = 8;    // city / town
                else if (t.rank <= 6) lo = 12;   // village / suburb
                else                  lo = 15;
            } else {
                lo = 15;
            }
            break;
        default: break;
    }
    // A producer-supplied hint beats our ladder. Protomaps computes min_zoom
    // per feature from the OSM tags, which is far finer-grained than anything
    // we can infer from the class alone: a rural highway shows up at z7 while
    // a city laneway waits for z14, both with the same kind. For the ground
    // layers the hint may only pull a feature EARLIER, never hide it, so a
    // featureless stretch of the Nullarbor still gets its base plate.
    if (t.min_zoom >= 0) {
        int hint = t.min_zoom;
        if (hint < 4)  hint = 4;
        if (hint > 20) hint = 20;
        const bool ground = (layer == LAY_EARTH || layer == LAY_LANDCOVER ||
                             layer == LAY_LANDUSE || layer == LAY_WATER ||
                             layer == LAY_PARKS || layer == LAY_BOUNDARY);
        if (ground) { if ((uint8_t)hint < lo) lo = (uint8_t)hint; }
        else        { lo = (uint8_t)hint; }
    }
    if (zmin) *zmin = lo;
    if (zmax) *zmax = 22;
}

// The zoom at which a NAME becomes worth drawing.
//
// Ground layers deliberately appear far earlier than their labels: the shape
// must never vanish, but a river name at z4 is noise. A producer hint wins
// (Protomaps hints the Swan at z14 and the Canning at z10, exactly the ladder a
// paper map would use); otherwise area names start at z12 and schools at z13.
// Both the point labels and the polygon labels go through here so the two can
// never disagree about when a name is allowed.
static uint8_t mvt_label_zoom(uint8_t layer, const MvtTags &t, uint8_t obj_zmin) {
    if (t.min_zoom >= 0) {
        int h = t.min_zoom;
        if (h < 4)  h = 4;
        if (h > 20) h = 20;
        return (uint8_t)h;
    }
    switch (layer) {
        case LAY_WATER:
        case LAY_LANDCOVER:
        case LAY_EARTH:
        case LAY_PARKS:   return 12;
        case LAY_SCHOOLS: return 13;
        default:          return obj_zmin;
    }
}

// Twice the signed area of a closed ring, computed about the ring's first
// vertex so int64 never overflows even at high reference zooms.
//
// MVT tile space has y growing DOWN, where an exterior ring is clockwise and
// has POSITIVE area. This renderer flips y when projecting to world space,
// which negates the area -- so here:
//      area2 < 0  =>  exterior ring (paint it)
//      area2 > 0  =>  interior ring (a hole -- leave it unpainted)
static int64_t mvt_ring_area2(const FVec2 *p, uint16_t n) {
    if (n < 3) return 0;
    const int64_t x0 = p[0].x, y0 = p[0].y;
    int64_t a = 0;
    for (uint16_t i = 0, j = (uint16_t)(n - 1); i < n; j = i++)
        a += ((int64_t)p[j].x - x0) * ((int64_t)p[i].y - y0)
           - ((int64_t)p[i].x - x0) * ((int64_t)p[j].y - y0);
    return a;
}

// Grow the per-tile object array. Returns false only on real OOM / hard cap.
static bool mvt_grow_objs(FMapObj **arr, uint32_t *cap, uint32_t need) {
    if (*cap >= need) return true;
    uint32_t nc = *cap ? *cap : 128;
    while (nc < need) nc *= 2;
    if (nc > 60000) nc = 60000;
    if (nc < need) return false;
    FMapObj *nb = (FMapObj*)realloc(*arr, sizeof(FMapObj) * nc);
    if (!nb) return false;
    *arr = nb;
    *cap = nc;
    return true;
}

static bool poi_append(uint8_t layer, uint8_t type, int32_t wx, int32_t wy,
                       const char *name, uint16_t nlen, uint8_t rank, uint8_t zmin);

// Decode one Feature. Returns the number of FMapObj entries written to out_arr.
// Named point features are routed into the POI list instead of becoming
// objects, because a point carries no geometry worth a heap allocation.
static uint16_t mvt_decode_feature(const uint8_t *pfeat, const uint8_t *fend,
                                   uint32_t extent, const MVTDecodeCtx &c,
                                   uint8_t layer_base, const MvtKeyTable &kt,
                                   FMapObj *out_arr, uint16_t out_cap) {
    uint8_t  geom_type = MVT_UNKNOWN;
    const uint8_t *geom_p = nullptr;
    uint32_t geom_len = 0;
    MvtTags  tg;
    mvt_tags_clear(tg);

    // Feature: 1 = id, 2 = tags (packed), 3 = type, 4 = geometry (packed).
    while (pfeat < fend) {
        uint32_t key   = pb_varint(pfeat, fend);
        uint8_t  wire  = key & 7;
        uint32_t field = key >> 3;
        if (field == 2 && wire == 2) {
            uint32_t n = pb_varint(pfeat, fend);
            if (pfeat + n > fend) { pfeat = fend; break; }
            mvt_read_tags(pfeat, pfeat + n, kt, tg);
            pfeat += n;
        } else if (field == 3 && wire == 0) {
            geom_type = (uint8_t)pb_varint(pfeat, fend);
        } else if (field == 4 && wire == 2) {
            geom_len = pb_varint(pfeat, fend);
            geom_p = pfeat;
            if (pfeat + geom_len > fend) { pfeat = fend; } else { pfeat += geom_len; }
        } else {
            pb_skip(pfeat, fend, wire);
        }
    }
    if (!geom_p || geom_len == 0 || geom_type == MVT_UNKNOWN) return 0;

    const double sx = c.sx / (double)extent;
    const double sy = c.sy / (double)extent;
    int32_t cur_x = 0, cur_y = 0;

    const uint8_t rc       = mvt_road_class(tg);
    const uint8_t layer_id = mvt_refine_layer(layer_base, tg);
    uint8_t zmin = 0, zmax = 22;
    mvt_zoom_range(layer_id, tg, rc, &zmin, &zmax);

    // One vertex buffer holds every ring of the feature back to back; ring
    // boundaries are recorded separately so holes line up with their exterior.
    FVec2   *pts = nullptr;
    uint16_t pn = 0, pcap = 0, ring0 = 0, nrings = 0;
    uint16_t ring_start[MVT_MAX_RINGS];
    uint16_t ring_end[MVT_MAX_RINGS];
    uint8_t  ring_hole[MVT_MAX_RINGS];
    uint16_t made = 0;

    auto push_pt = [&](int32_t x, int32_t y) -> bool {
        if (pn >= pcap) {
            uint16_t nc = pcap ? (uint16_t)(pcap * 2) : 64;
            if (nc > MVT_MAX_RING_PTS) nc = MVT_MAX_RING_PTS;
            if (nc == pcap) return false;
            FVec2 *np = (FVec2*)realloc(pts, sizeof(FVec2) * nc);
            if (!np) return false;
            pts = np; pcap = nc;
        }
        pts[pn].x = x; pts[pn].y = y; pn++;
        return true;
    };

    auto close_ring = [&]() {
        uint16_t n = (uint16_t)(pn - ring0);
        if (n >= 3 && nrings < MVT_MAX_RINGS) {
            ring_start[nrings] = ring0;
            ring_end[nrings]   = pn;
            ring_hole[nrings]  = (mvt_ring_area2(pts + ring0, n) > 0) ? 1 : 0;
            nrings++;
        }
        ring0 = pn;   // degenerate rings are dropped, not merged into the next
    };

    auto emit_line = [&]() {
        if (pn >= 2 && made < out_cap) {
            FMapObj &o = out_arr[made];
            o.points = (FVec2*)malloc(sizeof(FVec2) * pn);
            if (o.points) {
                memcpy(o.points, pts, sizeof(FVec2) * pn);
                o.type       = OBJ_LINE;
                o.layer      = layer_id;
                o.zmin       = zmin;
                o.zmax       = zmax;
                o.flags      = mvt_val_eq_ci(tg.brunnel, "tunnel") ? OF_DASHED : 0;
                o.sort_key   = rc;
                o.sub        = rc;
                o.rings      = 1;
                o.point_count= pn;
                o.ring_end   = nullptr;
                made++;
            }
        }
        pn = 0; ring0 = 0; nrings = 0;
    };

    // Emit one FMapObj per (exterior + its holes) group. MVT guarantees each
    // polygon is written exterior-first with its holes immediately after, so a
    // group is always a contiguous slice of the vertex buffer.
    auto emit_polygons = [&]() {
        uint16_t r = 0;
        while (r < nrings) {
            uint16_t g0 = r;
            uint16_t g1 = (uint16_t)(r + 1);
            // An exterior ring is one that is NOT a hole; holes extend the group.
            while (g1 < nrings && ring_hole[g1]) g1++;
            uint16_t v0 = ring_start[g0];
            uint16_t v1 = ring_end[g1 - 1];
            uint16_t nv = (uint16_t)(v1 - v0);
            uint16_t nr = (uint16_t)(g1 - g0);
            if (nv >= 3 && made < out_cap) {
                FMapObj &o = out_arr[made];
                o.points = (FVec2*)malloc(sizeof(FVec2) * nv);
                if (o.points) {
                    memcpy(o.points, pts + v0, sizeof(FVec2) * nv);
                    o.type        = OBJ_POLY;
                    o.layer       = layer_id;
                    o.zmin        = zmin;
                    o.zmax        = zmax;
                    o.flags       = OF_FILLED;
                    o.sort_key    = 0;
                    // For a surface layer this byte carries the cover kind
                    // (scrub / sand / forest / ...), not a road class.
                    o.sub         = mvt_cover_kind(layer_id, tg);
                    o.rings       = 1;
                    o.point_count = nv;
                    o.ring_end    = nullptr;
                    if (nr > 1) {
                        // Holes present: describe the ring boundaries. If this
                        // allocation fails we fall back to an outline so the
                        // polygon never paints over its own courtyards.
                        o.ring_end = (uint16_t*)malloc(sizeof(uint16_t) * nr);
                        if (o.ring_end) {
                            for (uint16_t k = 0; k < nr; k++)
                                o.ring_end[k] = (uint16_t)(ring_end[g0 + k] - v0);
                            o.rings = (uint8_t)nr;
                        } else {
                            o.flags &= (uint8_t)~OF_FILLED;
                        }
                    }
                    made++;
                }
            }
            r = g1;
        }

        // Named polygons get a label too. A river, a lake, a national park or
        // a suburb is exactly the thing you look at a map to identify, and
        // until now only *points* could be labelled - which is why a state map
        // full of named water features showed none of those names. The rank
        // comes from the producer's own min_zoom hint, so a big feature
        // outranks a pond, and a feature has to be either hinted as
        // significant or genuinely large before it costs a label.
        if (tg.name.p && tg.name.n && nrings > 0 && made < out_cap) {
            const bool labelable = (layer_id == LAY_WATER || layer_id == LAY_PARKS ||
                                    layer_id == LAY_SCHOOLS ||
                                    layer_id == LAY_LANDCOVER);
            const uint16_t n0 = (uint16_t)(ring_end[0] - ring_start[0]);
            const bool worthwhile = (n0 >= 12) ||
                                    (tg.min_zoom >= 0 && tg.min_zoom <= 12);
            if (labelable && worthwhile && n0 > 0) {
                int64_t cx = 0, cy = 0;
                for (uint16_t k = 0; k < n0; k++) {
                    cx += pts[ring_start[0] + k].x;
                    cy += pts[ring_start[0] + k].y;
                }
                int32_t rank = (tg.min_zoom >= 6) ? (tg.min_zoom - 6) : 0;
                if (rank > 7) rank = 7;
                // The label follows the producer's own hint rather than the
                // object's zmin. A ground layer is allowed to appear early so
                // the shape never vanishes, but a river's *name* at z4 would be
                // noise: the Swan is hinted at z14 and the Canning at z10,
                // which is exactly the ladder a paper map would use.
                // With no hint, a label still needs its own floor: the
                // geometry's ladder is deliberately generous for ground layers
                // (a coastline must not vanish), but a name at z4 is noise.
                // z12 is where area names start being useful.
                const uint8_t lz = mvt_label_zoom(layer_id, tg, zmin);
                const uint8_t ptype = (layer_id == LAY_PARKS)   ? 5 :
                                      (layer_id == LAY_SCHOOLS) ? 4 : 1;
                poi_append(layer_id, ptype, (int32_t)(cx / (int64_t)n0),
                           (int32_t)(cy / (int64_t)n0), tg.name.p, tg.name.n,
                           (uint8_t)rank, lz);
            }
        }
    };

    auto emit_point = [&](int32_t wx, int32_t wy) {
        // Points are only interesting when they are named: an unnamed dot has
        // nothing to show on the card, in Search or in Explore.
        if (!tg.name.p || tg.name.n == 0) return;
        uint8_t ptype = 1;
        if      (layer_id == LAY_SCHOOLS) ptype = 4;
        else if (layer_id == LAY_TRANSIT) ptype = 3;
        else if (layer_id == LAY_PARKS)   ptype = 5;
        uint8_t rank = (tg.rank < 0) ? 7 : (uint8_t)(tg.rank > 7 ? 7 : tg.rank);
        poi_append(layer_id, ptype, wx, wy, tg.name.p, tg.name.n, rank,
                   mvt_label_zoom(layer_id, tg, zmin));
    };

    const uint8_t *gp   = geom_p;
    const uint8_t *gend = geom_p + geom_len;
    while (gp < gend) {
        uint32_t cmd   = pb_varint(gp, gend);
        uint8_t  id    = cmd & 7;
        uint32_t count = cmd >> 3;
        if (id == 1) {                                      // ---- MoveTo ----
            if (geom_type == MVT_LINESTRING)             emit_line();
            else if (geom_type == MVT_POLYGON && pn > 0) close_ring();
            for (uint32_t k = 0; k < count && gp < gend; k++) {
                int32_t dx = zz_decode(pb_varint(gp, gend));
                int32_t dy = zz_decode(pb_varint(gp, gend));
                cur_x += dx; cur_y += dy;
                int32_t wx = (int32_t)(c.ox + (double)cur_x * sx);
                int32_t wy = (int32_t)(c.oy - (double)cur_y * sy);
                if (geom_type == MVT_POINT) emit_point(wx, wy);
                else push_pt(wx, wy);
            }
        } else if (id == 2) {                               // ---- LineTo ----
            for (uint32_t k = 0; k < count && gp < gend; k++) {
                int32_t dx = zz_decode(pb_varint(gp, gend));
                int32_t dy = zz_decode(pb_varint(gp, gend));
                cur_x += dx; cur_y += dy;
                int32_t wx = (int32_t)(c.ox + (double)cur_x * sx);
                int32_t wy = (int32_t)(c.oy - (double)cur_y * sy);
                if (geom_type == MVT_POINT) emit_point(wx, wy);
                else push_pt(wx, wy);
            }
        } else if (id == 7) {                               // ---- ClosePath ----
            // MVT rings are implicitly closed: the first vertex is NOT
            // repeated, so the filler must wrap (j == n-1) to close them.
            if (geom_type == MVT_POLYGON && pn > 0) close_ring();
        } else {
            gp = gend;                                      // unknown command
        }
    }

    if (geom_type == MVT_POLYGON) {
        close_ring();
        emit_polygons();
    } else if (geom_type == MVT_LINESTRING) {
        emit_line();
    }

    if (pts) free(pts);
    return made;
}

// Decode one Layer { name, features, keys, values, extent }.
//
// Two passes on purpose. The first collects the name, extent, keys and values;
// the second decodes features with those tables available. Tags reference keys
// by index, so a single pass would only work if the tile happened to serialise
// keys before features -- which the spec does not guarantee.
//
// This also removes the old `static FSlice slices[512]` cap, which silently
// dropped every feature past the 512th in a layer (routine for a city-centre
// building or transportation layer at z16+) and parked 4 KB of internal RAM in
// .bss forever.
static uint32_t mvt_decode_layer(const uint8_t *plyr, const uint8_t *lend,
                                 const MVTDecodeCtx &c,
                                 FMapObj **out_objs, uint32_t *cap, uint32_t used) {
    static MvtKeyTable kt;              // ~5 KB, reused per layer; not re-entrant
    char     lname[48] = {0};
    uint16_t lname_len = 0;
    uint32_t extent    = MVT_EXTENT_DEFAULT;
    kt.key_count = 0;
    kt.val_count = 0;

    // ---- pass 1: name, extent, keys, values ----
    const uint8_t *p = plyr;
    while (p < lend) {
        uint32_t key   = pb_varint(p, lend);
        uint8_t  wire  = key & 7;
        uint32_t field = key >> 3;
        if (field == 1 && wire == 2) {                       // name
            uint32_t n = pb_varint(p, lend);
            if (p + n > lend) { p = lend; break; }
            uint16_t k = (n < sizeof(lname) - 1) ? (uint16_t)n : (uint16_t)(sizeof(lname) - 1);
            memcpy(lname, p, k); lname[k] = 0; lname_len = k;
            p += n;
        } else if (field == 5 && wire == 0) {                // extent
            extent = pb_varint(p, lend);
        } else if (field == 3 && wire == 2) {                // keys  (repeated string)
            uint32_t n = pb_varint(p, lend);
            if (p + n > lend) { p = lend; break; }
            if (kt.key_count < MVT_MAX_KEYS) {
                kt.key[kt.key_count].p = (const char*)p;
                kt.key[kt.key_count].n = (uint16_t)n;
                kt.key_count++;
            }
            p += n;
        } else if (field == 4 && wire == 2) {                // values (repeated Value)
            uint32_t n = pb_varint(p, lend);
            if (p + n > lend) { p = lend; break; }
            if (kt.val_count < MVT_MAX_VALS)
                mvt_parse_value(p, p + n, kt.val[kt.val_count++]);
            p += n;
        } else {
            pb_skip(p, lend, wire);
        }
    }
    if (lname_len == 0 || extent == 0) return 0;

    const uint8_t layer_base = mvt_layer_id(lname, lname_len);
    if (layer_base == 0xFF) return 0;       // label-only layer: we draw our own text

    mvt_resolve_keys(kt);

    // ---- pass 2: decode features ----
    const uint8_t *q = plyr;
    uint32_t added = 0;
    while (q < lend) {
        uint32_t key   = pb_varint(q, lend);
        uint8_t  wire  = key & 7;
        uint32_t field = key >> 3;
        if (field == 2 && wire == 2) {                       // Feature
            uint32_t n = pb_varint(q, lend);
            if (q + n > lend) break;
            if (!mvt_grow_objs(out_objs, cap, used + added + 8)) break;
            uint32_t room = *cap - used - added;
            if (room > 60000) room = 60000;
            added += mvt_decode_feature(q, q + n, extent, c, layer_base, kt,
                                        (*out_objs) + used + added, (uint16_t)room);
            q += n;
        } else {
            pb_skip(q, lend, wire);
        }
    }
    MVT_LOG("[LAYER] name='%.*s' extent=%u -> base=%u added=%u\n",
            (int)lname_len, lname, (unsigned)extent,
            (unsigned)layer_base, (unsigned)added);
    return added;
}

// =============================================================================
//  MVT tile loader
//
//  Path resolution probes once and remembers the winner. The old code ran up to
//  twelve SD_MMC.exists() calls per lookup, and tile_resolve() asks up to six
//  times per cell per frame -- which is what made panning stutter even when
//  every tile was already decoded and cached.
// =============================================================================
static int8_t g_tile_ext    = -1;   // index into TILE_EXTS, -1 = not yet known
static int8_t g_tile_layout = -1;   // 0 = /z/x/y, 1 = /z/x-y, 2 = flat /z_x_y

static void tile_path_fmt(char *b, size_t cap, int32_t z, int32_t x, int32_t y, int layout) {
    switch (layout) {
        case 0:  snprintf(b, cap, "/%d/%d/%d", (int)z, (int)x, (int)y); break;
        case 1:  snprintf(b, cap, "/%d/%d-%d", (int)z, (int)x, (int)y); break;
        default: snprintf(b, cap, "/%d_%d_%d", (int)z, (int)x, (int)y); break;
    }
}

static String mvt_tile_path(int32_t z, int32_t x, int32_t y) {
    static const char *EXTS[] = { ".pbf", ".PBF", ".mvt", ".MVT" };
    char b[96];
    // Fast path: the layout and extension we settled on last time. One stat.
    if (g_tile_ext >= 0) {
        tile_path_fmt(b, sizeof(b), z, x, y, g_tile_layout);
        String c = g_map_dir_path + String(b) + EXTS[g_tile_ext];
        if (SD_MMC.exists(c.c_str())) return c;
    }
    // Slow path: probe the permutations this map's converter might have used.
    for (int L = 0; L < 3; L++) {
        tile_path_fmt(b, sizeof(b), z, x, y, L);
        String base = g_map_dir_path + String(b);
        for (int i = 0; i < 4; i++) {
            String c = base + EXTS[i];
            if (SD_MMC.exists(c.c_str())) {
                g_tile_layout = (int8_t)L;
                g_tile_ext    = (int8_t)i;
                return c;
            }
        }
    }
    return String();
}

// -----------------------------------------------------------------------------
//  Negative probe cache.
//  Tiles outside a map's bounds do not exist, and every frame used to re-stat
//  the SD card for all nine ring cells x up to six overzoom levels to find that
//  out again. A compact ring of the last 64 confirmed-missing keys removes
//  essentially all of that traffic. Cleared whenever a different map is opened,
//  because the same (z,x,y) may well exist in the next map.
// -----------------------------------------------------------------------------
#define TILE_MISS_SLOTS 64
static int32_t g_miss_key[TILE_MISS_SLOTS][3];
static uint8_t g_miss_next = 0;

static void tile_miss_clear() {
    for (int i = 0; i < TILE_MISS_SLOTS; i++)
        g_miss_key[i][0] = g_miss_key[i][1] = g_miss_key[i][2] = INT32_MIN;
    g_miss_next = 0;
}
static bool tile_probe_missing(int32_t z, int32_t x, int32_t y) {
    for (int i = 0; i < TILE_MISS_SLOTS; i++)
        if (g_miss_key[i][0] == z && g_miss_key[i][1] == x && g_miss_key[i][2] == y)
            return true;
    return false;
}
static void tile_probe_remember_missing(int32_t z, int32_t x, int32_t y) {
    g_miss_key[g_miss_next][0] = z;
    g_miss_key[g_miss_next][1] = x;
    g_miss_key[g_miss_next][2] = y;
    g_miss_next = (uint8_t)((g_miss_next + 1) % TILE_MISS_SLOTS);
}

// Returns true if a tile file exists on disk.
static bool tile_exists_on_disk(int32_t z, int32_t x, int32_t y) {
    if (tile_probe_missing(z, x, y)) return false;
    if (mvt_tile_path(z, x, y).length() > 0) return true;
    tile_probe_remember_missing(z, x, y);
    return false;
}

// -----------------------------------------------------------------------------
// Tile-render diagnostics counters (visible on the map HUD as "tiles: N/M").
// Reset each frame in draw_map_frame() before iterating the 3x3 ring.
// -----------------------------------------------------------------------------
static uint16_t g_hud_tiles_ok    = 0;
static uint16_t g_hud_tiles_empty = 0;

// -----------------------------------------------------------------------------
//  Paint-order indexing. Objects arrive grouped by the *source* layer, which is
//  not the same as paint order (one `transportation` layer yields roads, paths
//  AND rail). Sorting once per tile and recording each layer's run lets the
//  renderer walk layers across all nine tiles without rescanning every object
//  fifteen times per frame.
// -----------------------------------------------------------------------------
static int mvt_obj_cmp(const void *a, const void *b) {
    const FMapObj *x = (const FMapObj*)a;
    const FMapObj *y = (const FMapObj*)b;
    if (x->layer    != y->layer)    return (int)x->layer    - (int)y->layer;
    if (x->sort_key != y->sort_key) return (int)x->sort_key - (int)y->sort_key;
    return 0;
}

static void tile_index_layers(FMapTile &t) {
    for (int i = 0; i <= LAY__COUNT; i++) t.band[i] = 0;
    if (!t.objs || t.obj_count == 0) return;
    qsort(t.objs, t.obj_count, sizeof(FMapObj), mvt_obj_cmp);
    uint16_t i = 0;
    for (uint8_t L = 0; L < LAY__COUNT; L++) {
        t.band[L] = i;
        while (i < t.obj_count && t.objs[i].layer == L) i++;
    }
    t.band[LAY__COUNT] = i;
}

// Load one MVT tile into a cache slot.
static FMapTile *tile_load(int8_t z, int32_t x, int32_t y) {
    // Already decoded? Hand back the cached slot instead of decoding the same
    // tile into a second cache entry. Duplicates cost PSRAM twice over AND
    // repeat the two most expensive operations in the app (SD read + inflate),
    // and every current caller would have had to remember to check first.
    if (FMapTile *hit = cache_find(z, x, y)) return hit;

    String path = mvt_tile_path(z, x, y);
    if (!path.length()) {
        MVT_LOG("[TILE] z=%d x=%d y=%d -- file not found on SD\n", (int)z, (int)x, (int)y);
        return nullptr;
    }

    FMapTile *slot = cache_evict_slot();
    slot->z = z; slot->tx = x; slot->ty = y;
    slot->loaded = false;
    slot->status = TILE_ST_EMPTY;
    slot->obj_count = 0; slot->objs = nullptr;
    for (int i = 0; i <= LAY__COUNT; i++) slot->band[i] = 0;
    slot->last_use_ms = millis();

    File f = SD_MMC.open(path, FILE_READ);
    if (!f) {
        MVT_LOG("[TILE] z=%d x=%d y=%d path='%s' -- SD open FAILED\n",
                (int)z, (int)x, (int)y, path.c_str());
        slot->loaded = true; slot->status = TILE_ST_IOERR; return slot;
    }
    size_t sz = f.size();
    MVT_LOG("[TILE] z=%d x=%d y=%d path='%s' bytes=%u\n",
            (int)z, (int)x, (int)y, path.c_str(), (unsigned)sz);
    if (!sz || sz > 512 * 1024) {
        MVT_LOG("[TILE]   size out of bounds -- ignored\n");
        f.close(); slot->loaded = true; slot->status = TILE_ST_BADSIZE; return slot;
    }

    uint8_t *buf = (uint8_t*)ps_malloc(sz);
    if (!buf) buf = (uint8_t*)malloc(sz);
    if (!buf) {
        MVT_LOG("[TILE]   malloc(%u) FAILED -- skipping\n", (unsigned)sz);
        f.close(); slot->loaded = true; slot->status = TILE_ST_OOM; return slot;
    }
    size_t got = f.read(buf, sz);
    f.close();
    if (got != sz) {
        MVT_LOG("[TILE]   short read got=%u expected=%u -- skipping\n",
                (unsigned)got, (unsigned)sz);
        free(buf); slot->loaded = true; slot->status = TILE_ST_IOERR; return slot;
    }

    // Track A -- gzip handling. MBTiles/Planetiler vector tiles are gzip'd
    // (magic 1F 8B). Inflate in place with the bundled puff() so gzip'd tiles
    // render instead of being rejected. Uncompressed tiles fall straight through.
    if (sz >= 2 && buf[0] == 0x1F && buf[1] == 0x8B) {
        // gzip trailer ISIZE (little-endian) = uncompressed size mod 2^32.
        unsigned long isize = (unsigned long)buf[sz-4] | ((unsigned long)buf[sz-3] << 8) |
                              ((unsigned long)buf[sz-2] << 16) | ((unsigned long)buf[sz-1] << 24);
        unsigned long outcap = isize ? isize : (unsigned long)sz * 8;
        if (outcap < (unsigned long)sz) outcap = (unsigned long)sz * 8;  // guard bad ISIZE
        if (outcap > 1024UL * 1024UL) outcap = 1024UL * 1024UL;          // 1 MB ceiling
        uint8_t *ubuf = (uint8_t*)ps_malloc(outcap);
        if (!ubuf) ubuf = (uint8_t*)malloc(outcap);
        if (!ubuf) {
            MVT_LOG("[TILE]   gunzip malloc(%lu) FAILED\n", outcap);
            free(buf); slot->loaded = true; slot->status = TILE_ST_OOM; return slot;
        }
        unsigned long outlen = outcap, srclen = (unsigned long)sz;
        int rc = wp_gunzip(ubuf, &outlen, buf, &srclen);
        free(buf);
        if (rc != 0) {
            MVT_LOG("[TILE]   gunzip FAILED rc=%d (outcap=%lu)\n", rc, outcap);
            free(ubuf); slot->loaded = true; slot->status = TILE_ST_GZIP; return slot;
        }
        MVT_LOG("[TILE]   gunzip ok %u -> %lu bytes\n", (unsigned)sz, outlen);
        buf = ubuf; sz = (size_t)outlen;
    }

    MVTDecodeCtx c;
    mvt_tile_topleft_world(z, x, y, g_meta.refzoom, &c.ox, &c.oy);
    int shift = g_meta.refzoom - z;
    double tile_world_side = (shift >= 0) ? (double)MVT_EXTENT_DEFAULT * (double)(1u << shift)
                                          : (double)MVT_EXTENT_DEFAULT / (double)(1u << -shift);
    c.sx =  tile_world_side;
    c.sy =  tile_world_side;
    c.z  = z;

    FMapObj *objs = nullptr;
    uint32_t cap = 0, used = 0;
    uint32_t layer_seen = 0;
    const uint8_t *p = buf;
    const uint8_t *end = buf + sz;
    while (p < end) {
        uint32_t key = pb_varint(p, end);
        uint8_t wire = key & 7;
        uint32_t field = key >> 3;
        if (field == 3 && wire == 2) {
            uint32_t n = pb_varint(p, end);
            if (p + n > end) break;
            layer_seen++;
            uint32_t added = mvt_decode_layer(p, p + n, c, &objs, &cap, used);
            used += added;
            p += n;
        } else {
            pb_skip(p, end, wire);
        }
    }
    free(buf);

    MVT_LOG("[TILE] z=%d x=%d y=%d -> layers=%u objects=%u\n",
            (int)z, (int)x, (int)y, (unsigned)layer_seen, (unsigned)used);

    if (used == 0) {
        if (objs) free(objs);
        slot->loaded = true;
        slot->status = TILE_ST_NODATA;
        return slot;
    }
    FMapObj *trimmed = (FMapObj*)realloc(objs, sizeof(FMapObj) * used);
    slot->objs = trimmed ? trimmed : objs;
    slot->obj_count = (uint16_t)(used > 65535 ? 65535 : used);
    tile_index_layers(*slot);
    slot->loaded = true;
    slot->status = TILE_ST_OK;
    g_cache_bytes += tile_bytes(*slot);
    return slot;
}
// =============================================================================
//  POI accumulation (populated as MVT tiles are decoded)
// =============================================================================
// -----------------------------------------------------------------------------
//  POI accumulation.
//
//  POIs are added as tiles decode, and tiles are re-decoded every time the
//  cache evicts and reloads them, so de-duplication has to be cheap. The old
//  version linearly scanned every POI collected so far for each candidate,
//  which is quadratic: tens of millions of comparisons to build a city's list.
//  A small open-addressed grid keyed by world cell makes it O(1).
// -----------------------------------------------------------------------------
#define POI_HASH_BITS   12
#define POI_HASH_SIZE   (1 << POI_HASH_BITS)
#define POI_DEDUP_UNITS 4          // world units (~60 m at z17 / lat -32)

static int32_t *g_poi_hash = nullptr;   // cell -> poi index, -1 = empty (PSRAM)

static inline uint32_t poi_hash_cell(int32_t x, int32_t y) {
    uint32_t ux = (uint32_t)(x / POI_DEDUP_UNITS);
    uint32_t uy = (uint32_t)(y / POI_DEDUP_UNITS);
    return (ux * 73856093u ^ uy * 19349663u) & (POI_HASH_SIZE - 1);
}

static bool poi_is_duplicate(int32_t x, int32_t y) {
    if (!g_poi_hash) return false;
    uint32_t h = poi_hash_cell(x, y);
    for (int k = 0; k < 8; k++) {
        int32_t idx = g_poi_hash[(h + k) & (POI_HASH_SIZE - 1)];
        if (idx < 0) return false;
        if (idx < (int32_t)g_poi_count &&
            abs((long)g_pois[idx].x - (long)x) < POI_DEDUP_UNITS &&
            abs((long)g_pois[idx].y - (long)y) < POI_DEDUP_UNITS) return true;
    }
    return false;
}

static void poi_hash_put(int32_t x, int32_t y, int32_t idx) {
    uint32_t h = poi_hash_cell(x, y);
    for (int k = 0; k < 8; k++) {
        uint32_t b = (h + k) & (POI_HASH_SIZE - 1);
        if (g_poi_hash[b] < 0) { g_poi_hash[b] = idx; return; }
    }
}

static bool poi_append(uint8_t layer, uint8_t type, int32_t wx, int32_t wy,
                       const char *name, uint16_t nlen, uint8_t rank, uint8_t zmin) {
    if (!name || nlen == 0) return false;
    if (g_poi_count >= MAX_POIS) return false;
    if (!g_poi_hash) {
        g_poi_hash = (int32_t*)ps_malloc(sizeof(int32_t) * POI_HASH_SIZE);
        if (!g_poi_hash) g_poi_hash = (int32_t*)malloc(sizeof(int32_t) * POI_HASH_SIZE);
        if (!g_poi_hash) return false;
        for (int i = 0; i < POI_HASH_SIZE; i++) g_poi_hash[i] = -1;
    }
    if (poi_is_duplicate(wx, wy)) return false;

    if (g_poi_count >= g_poi_cap) {
        uint32_t nc = g_poi_cap ? g_poi_cap * 2 : 64;
        if (nc > MAX_POIS) nc = MAX_POIS;
        FMapPOI *nb = (FMapPOI*)ps_malloc(sizeof(FMapPOI) * nc);
        if (!nb) nb = (FMapPOI*)malloc(sizeof(FMapPOI) * nc);
        if (!nb) return false;
        if (g_pois) { memcpy(nb, g_pois, sizeof(FMapPOI) * g_poi_count); free(g_pois); }
        memset(nb + g_poi_count, 0, sizeof(FMapPOI) * (nc - g_poi_count));
        g_pois = nb; g_poi_cap = nc;
    }

    char *nm = (char*)malloc((size_t)nlen + 1);
    if (!nm) return false;
    memcpy(nm, name, nlen);
    nm[nlen] = 0;

    FMapPOI &p = g_pois[g_poi_count];
    p.x    = wx;
    p.y    = wy;
    p.layer= layer;
    p.type = type;
    p.rank = rank;
    p.zmin = zmin;
    p.name = nm;
    poi_hash_put(wx, wy, (int32_t)g_poi_count);
    g_poi_count++;
    return true;
}

static void poi_reset() {
    if (g_pois) {
        for (uint32_t i=0; i<g_poi_count; i++) if (g_pois[i].name) free(g_pois[i].name);
        free(g_pois); g_pois = nullptr;
    }
    if (g_poi_hash) {
        for (int i = 0; i < POI_HASH_SIZE; i++) g_poi_hash[i] = -1;
    }
    g_poi_count = 0; g_poi_cap = 0;
}

// =============================================================================
//  metadata.json parser (deliberately tiny -- looks up bare "key":value pairs)
// =============================================================================
static String json_find_str(const String &src, const char *key) {
    String needle = String("\"") + key + "\"";
    int k = src.indexOf(needle);
    if (k < 0) return String();
    int colon = src.indexOf(':', k);
    if (colon < 0) return String();
    int q1 = src.indexOf('"', colon);
    if (q1 < 0) return String();
    int q2 = src.indexOf('"', q1 + 1);
    if (q2 < 0) return String();
    return src.substring(q1 + 1, q2);
}
static int json_find_int(const String &src, const char *key, int fallback) {
    String needle = String("\"") + key + "\"";
    int k = src.indexOf(needle);
    if (k < 0) return fallback;
    int colon = src.indexOf(':', k);
    if (colon < 0) return fallback;
    int i = colon + 1;
    while (i < (int)src.length() && (src[i] == ' ' || src[i] == '"')) i++;
    int j = i;
    while (j < (int)src.length() && (src[j] == '-' || (src[j] >= '0' && src[j] <= '9'))) j++;
    if (j == i) return fallback;
    return src.substring(i, j).toInt();
}

// =============================================================================
//  Map open / close  (a "map" is a directory containing MVT tiles)
// =============================================================================
static bool mvt_map_read_header_meta(const String &dir_path, FMapMeta &out) {
    // Try to read <dir>/metadata.json for name/bounds; fall back to dir name.
    out.title = String();
    out.desc = String();
    out.min_lat = out.min_lon = out.max_lat = out.max_lon = 0;
    out.origin_lat = out.origin_lon = 0;
    out.units_per_metre = 1;
    out.minzoom = 10; out.maxzoom = 17; out.refzoom = 17;
    out.projection = 1; // web mercator
    out.created = 0;

    // dir name as fallback title
    int slash = dir_path.lastIndexOf('/');
    String dirname = (slash >= 0) ? dir_path.substring(slash + 1) : dir_path;

    File mj = SD_MMC.open((dir_path + "/metadata.json").c_str(), FILE_READ);
    if (mj) {
        String s = mj.readString();
        mj.close();
        out.title = json_find_str(s, "name");
        out.desc  = json_find_str(s, "description");
        // bounds "minLon,minLat,maxLon,maxLat"
        String b = json_find_str(s, "bounds");
        if (b.length()) {
            int p1 = b.indexOf(','); int p2 = (p1>=0) ? b.indexOf(',', p1+1) : -1;
            int p3 = (p2>=0) ? b.indexOf(',', p2+1) : -1;
            if (p1>0 && p2>p1 && p3>p2) {
                out.min_lon = b.substring(0, p1).toFloat();
                out.min_lat = b.substring(p1+1, p2).toFloat();
                out.max_lon = b.substring(p2+1, p3).toFloat();
                out.max_lat = b.substring(p3+1).toFloat();
            }
        }
        out.minzoom = (uint8_t)json_find_int(s, "minzoom", 10);
        out.maxzoom = (uint8_t)json_find_int(s, "maxzoom", 17);
        // Clamp to the slippy-zoom range we can actually represent. Every world
        // coordinate is `(1u << refzoom)`-scaled, so a bogus maxzoom (0, or 31
        // from a hand-edited metadata.json, or a 64-bit value that truncates)
        // used to shift the whole world away and render an empty screen with no
        // error at all. A too-small maxzoom (< minzoom) reversed the zoom loop
        // in mvt_map_open and made the map refuse to open.
        if (out.minzoom > 22) out.minzoom = 22;
        if (out.maxzoom > 22) out.maxzoom = 22;
        if (out.maxzoom < out.minzoom) out.maxzoom = out.minzoom;
    }
 if (!out.title.length()) out.title = dirname;
    if (out.max_lat == 0 && out.min_lat == 0) {
        out.desc = out.desc.length() ? out.desc : String("MVT tiles (bounds unknown)");
    }
    out.refzoom = out.maxzoom;
    MVT_LOG("[MAP] meta dir='%s' title='%s' bounds=[%.5f,%.5f .. %.5f,%.5f] "
            "minzoom=%u maxzoom=%u refzoom=%u\n",
            dir_path.c_str(), out.title.c_str(),
            (double)out.min_lon, (double)out.min_lat,
            (double)out.max_lon, (double)out.max_lat,
            (unsigned)out.minzoom, (unsigned)out.maxzoom, (unsigned)out.refzoom);
    return true;
}

// Count tile files under one zoom dir (small helper for library metadata).
static uint32_t count_tiles_at_zoom(const String &dir_path, uint8_t z) {
    char zb[16]; snprintf(zb, sizeof(zb), "/%u", (unsigned)z);
    File zd = SD_MMC.open((dir_path + zb).c_str());
    if (!zd || !zd.isDirectory()) { if (zd) zd.close(); return 0; }
    uint32_t total = 0;
    File xd = zd.openNextFile();
    while (xd) {
        if (xd.isDirectory()) {
            File yd = xd.openNextFile();
            while (yd) {
              
   if (!yd.isDirectory()) {
                    String n = yd.name();
                    // Case-insensitive check for .pbf / .mvt (some converters
                    // and Windows tooling upper-case the extension).
                    String ext;
                    int dot = n.lastIndexOf('.');
                    if (dot >= 0) { ext = n.substring(dot); ext.toLowerCase(); }
                    if (ext == ".pbf" || ext == ".mvt") total++;
                }
                yd = xd.openNextFile();
            }
        }
        xd = zd.openNextFile();
    }
    return total;
}

static void mvt_map_close() {
    g_map_loaded = false;
    g_map_dir_path = String();
    poi_reset();
    g_zoom_count = 0;
    g_tile_count = 0;
    for (int i=0; i<LAY__COUNT; i++) g_styles[i].set = false;
    cache_reset();
    g_last_cached_z = -1;
}

static bool mvt_map_open(const String &dir_path) {
    mvt_map_close();

    if (!mvt_map_read_header_meta(dir_path, g_meta)) return false;
    g_map_dir_path = dir_path;

    // Populate g_zooms list by probing which {z} folders exist on the SD card.
    g_zoom_count = 0;
    g_tile_count = 0;
    for (uint8_t z = g_meta.minzoom; z <= g_meta.maxzoom; z++) {
        if (g_zoom_count >= MAX_ZOOMS) {
            // Loud rather than silent: a truncated list means the finer levels
            // are unreachable and the map paints its coarsest tiles at every
            // zoom, which reads as a rendering bug rather than a data one.
            MVT_LOG("[MAP] zoom list full at MAX_ZOOMS=%u - z%u and above ignored\n",
                    (unsigned)MAX_ZOOMS, (unsigned)z);
            break;
        }
        char zb[16]; snprintf(zb, sizeof(zb), "/%u", (unsigned)z);
        if (!SD_MMC.exists((dir_path + zb).c_str())) continue;
        uint32_t tc = count_tiles_at_zoom(dir_path, z);
        if (!tc) continue;
        FMapZoom zr;
        zr.zoom_id = z;
        // world-units per pixel where this zoom is "preferred": one MVT tile
        // (world span at ref zoom) should render at ~256 screen pixels.
        int shift = g_meta.refzoom - z;
        double world_side = (double)MVT_EXTENT_DEFAULT *
                            ((shift >= 0) ? (double)(1u << shift)
                                          : 1.0 / (double)(1u << -shift));
        zr.min_px_scale = (float)(world_side / 256.0);
        zr.object_count = tc;
        g_zooms[g_zoom_count++] = zr;
        g_tile_count += tc;
    }
    if (g_zoom_count == 0) {
        MVT_LOG("[MAP] no tile zoom folders found under '%s' -- aborting open\n",
                dir_path.c_str());
        return false;
    }
    MVT_LOG("[MAP] discovered %u zoom levels, %u total tiles under '%s'\n",
            (unsigned)g_zoom_count, (unsigned)g_tile_count, dir_path.c_str());
    for (uint8_t i=0; i<g_zoom_count; i++) {
        MVT_LOG("[MAP]   z=%u tiles=%u\n",
                (unsigned)g_zooms[i].zoom_id, (unsigned)g_zooms[i].object_count);
    }

    // Compute metres-per-world-unit at map's origin latitude for scale bar / nav.
    double lat_c = ((double)g_meta.min_lat + (double)g_meta.max_lat) * 0.5;
    if (lat_c == 0) lat_c = -32.0;   // fallback (Byford-ish southern hemisphere)
    double cosL = cos(lat_c * M_PI / 180.0);
    if (cosL < 0.05) cosL = 0.05;
    // 156543.03392 m/px at equator zoom 0, slippy 256px tiles.
    // world_units per pixel at refzoom = MVT_EXTENT / 256.
    // => metres per world unit = 156543.03392 * cos(L) / (2^refzoom * (EXTENT/256))
    g_metres_per_world_unit = 156543.03392 * cosL /
                              ((double)(1u << g_meta.refzoom) *
                               ((double)MVT_EXTENT_DEFAULT / 256.0));
    g_meta.units_per_metre = (float)(1.0 / g_metres_per_world_unit);
    g_meta.origin_lat = (float)lat_c;
    g_meta.origin_lon = (float)(((double)g_meta.min_lon + (double)g_meta.max_lon) * 0.5);

    g_map_loaded = true;
    return true;
}
// -----------------------------------------------------------------------------
//  Overzoom. Resolve the best available tile for (z,x,y): try the requested
//  zoom, then walk up to parent tiles (z-1, z-2, ...) until one exists on disk
//  or is already cached. Parent geometry is stored in world coords at the map's
//  reference zoom, so it renders at the correct place and scale with no
//  per-point rescaling -- the camera scale does the "zooming in" for us.
//
//  This is the single most important reason a z10-z14 tile set is enough for a
//  watch: build once at z14 (tilemaker's ceiling) and let overzoom carry the
//  display up to z19.
//
//  Returns the resolved cached tile (may be a parent), or nullptr if nothing
//  covers the area within TILE_OVERZOOM_MAX levels.
// -----------------------------------------------------------------------------
static FMapTile *tile_resolve(int8_t z, int32_t x, int32_t y) {
    for (int up = 0; up <= TILE_OVERZOOM_MAX; up++) {
        int8_t pz = (int8_t)(z - up);
        if (pz < (int8_t)g_meta.minzoom) break;
        // int32_t throughout: at z17 a Perth tile column is ~107 000, so the old
        // int16_t shifted straight into negative tile space and nothing loaded.
        int32_t px = x >> up, py = y >> up;
        FMapTile *t = cache_find(pz, px, py);
        if (t) {
            if (t->status == TILE_ST_OK) return t;   // usable data
            if (up == 0) return t;                   // native zoom: report as-is
            continue;                                // empty parent: try grandparent
        }
        if (tile_exists_on_disk(pz, px, py)) {
            FMapTile *nt = tile_load(pz, px, py);
            if (nt && nt->status == TILE_ST_OK) return nt;
            if (up == 0) return nt;                  // present but empty/failed
            // parent present but empty -> keep walking up
        }
    }
    return nullptr;
}
// =============================================================================
//  Camera + projection : world (int32) -> screen (int)
//
//  Inside the testable region on purpose. This is where "which zoom level does
//  the camera ask for" is decided, and that decision is exactly what made a
//  whole-of-WA map unusable while the zoom list was being truncated (see
//  MAX_ZOOMS): the camera kept asking for a level the list no longer held. The
//  host tests drive cam_select_zoom() and cam_scale_bounds() directly.
// =============================================================================
static double g_cam_x = 0, g_cam_y = 0;      // world centre
static float  g_cam_scale = 1.0f;            // pixels per world unit
static uint8_t g_cam_zoom = 1;               // 1..N -> index into g_zooms
static int    g_screen_cx = LCD_WIDTH/2;
static int    g_screen_cy = LCD_HEIGHT/2;
static float  g_map_rotation_deg = 0.0f;     // 0 = north up; set to heading if rotate-with-heading
static bool   g_rotate_with_heading = false;
static float  g_last_rot_drawn_deg = -999.0f;// throttle: only redraw when heading moves > N degrees

// Nav target (POI or map centre set as "Navigate here")
static bool   g_has_nav_target = false;
static int32_t g_nav_target_x = 0, g_nav_target_y = 0;
static String  g_nav_target_name;

// Beacons (loaded from /WATCH/WayPoint/beacons.csv)
struct Beacon { int32_t x, y; uint8_t online; String name; };
#define MAX_BEACONS 32
static Beacon g_beacons[MAX_BEACONS];
static int    g_beacon_count = 0;

static inline void world_to_screen(int32_t wx, int32_t wy, int *sx, int *sy) {
    double dx = (wx - g_cam_x) * g_cam_scale;
    double dy = (wy - g_cam_y) * g_cam_scale;
    if (g_map_rotation_deg != 0) {
        float r = -g_map_rotation_deg * (float)M_PI / 180.0f;
        float ca = cosf(r), sa = sinf(r);
        double rx = dx * ca - dy * sa;
        double ry = dx * sa + dy * ca;
        dx = rx; dy = ry;
    }
    *sx = g_screen_cx + (int)dx;
    *sy = g_screen_cy - (int)dy;   // world +Y is up
}

static void cam_reset_to_bounds() {
    // Centre on the map's lat/lon centre (Web Mercator, world +Y = north).
    double lat_c = ((double)g_meta.min_lat + (double)g_meta.max_lat) * 0.5;
    double lon_c = ((double)g_meta.min_lon + (double)g_meta.max_lon) * 0.5;
    if (g_meta.min_lat == 0 && g_meta.max_lat == 0) {
        // Unknown bounds: park camera on the middle discovered tile at midzoom.
        uint8_t z = g_zoom_count ? g_zooms[g_zoom_count/2].zoom_id : g_meta.refzoom;
        // Look for first tile file at this zoom to derive x/y.
        int shift = g_meta.refzoom - z;
        double side = (double)MVT_EXTENT_DEFAULT *
                      ((shift >= 0) ? (double)(1u << shift) : 1.0/(double)(1u << -shift));
        g_cam_x = side * 0.5;
        g_cam_y = -side * 0.5;
        g_cam_scale = (float)(256.0 / side);
        g_cam_zoom  = z;
        return;
    }
 g_cam_x = mercator_lon_to_world_x(lon_c, g_meta.refzoom);
    g_cam_y = mercator_lat_to_world_y(lat_c, g_meta.refzoom);
    // Choose initial zoom = clamp((minzoom+maxzoom)/2, minzoom, maxzoom), then
    // snap to the closest discovered zoom level on disk. For the user's Perth
    // export (minzoom=10, maxzoom=16) this lands at z13 -- inside the tiny
    // bounds where tiles actually exist.
    uint8_t z_want = (uint8_t)(((int)g_meta.minzoom + (int)g_meta.maxzoom) / 2);
    if (z_want < g_meta.minzoom) z_want = g_meta.minzoom;
    if (z_want > g_meta.maxzoom) z_want = g_meta.maxzoom;
    uint8_t z = g_zoom_count ? g_zooms[0].zoom_id : z_want;
    int best_dz = 999;
    for (uint8_t i=0; i<g_zoom_count; i++) {
        int dz = (int)g_zooms[i].zoom_id - (int)z_want;
        if (dz < 0) dz = -dz;
        if (dz < best_dz) { best_dz = dz; z = g_zooms[i].zoom_id; }
    }
    int shift = g_meta.refzoom - z;
    double side = (double)MVT_EXTENT_DEFAULT *
                  ((shift >= 0) ? (double)(1u << shift) : 1.0/(double)(1u << -shift));
    g_cam_scale = (float)(256.0 / side);
    g_cam_zoom  = z;
    MVT_LOG("[CAM] reset to bounds: lon=%.5f lat=%.5f z=%u scale=%.6f "
            "(minzoom=%u maxzoom=%u want=%u)\n",
            lon_c, lat_c, (unsigned)z, g_cam_scale,
            (unsigned)g_meta.minzoom, (unsigned)g_meta.maxzoom, (unsigned)z_want);
}

static void cam_select_zoom() {
    // Pick the slippy zoom whose "preferred" world-units-per-pixel best matches
    // the current camera scale. min_px_scale is world_units_per_screen_pixel at
    // that zoom's canonical rendering size.
    if (g_zoom_count == 0) return;
    float upp = 1.0f / g_cam_scale;    // world units per screen pixel right now
    uint8_t best = g_zooms[0].zoom_id;
    float best_ratio = 1e9f;
    for (uint8_t i=0; i<g_zoom_count; i++) {
        float r = g_zooms[i].min_px_scale / upp;
        if (r < 1.0f) r = 1.0f / r;   // closest to 1.0 wins
        if (r < best_ratio) { best_ratio = r; best = g_zooms[i].zoom_id; }
    }
    // NOTE: deliberately do NOT wipe the tile cache when the slippy zoom
    // changes. With overzoom, the parent tile we loaded for z-1 is exactly the
    // geometry the new zoom still wants, and the cache is keyed by exact
    // (z,x,y) so an entry from another zoom can never be mistaken for this one.
    // Clearing here forced every zoom in/out to re-read and re-inflate 9+ tiles
    // off the SD card -- the single most expensive thing the app does.
    g_cam_zoom = best;
    g_last_cached_z = (int8_t)best;
}

// Track A -- metadata-driven camera scale clamp. Replaces the old hardcoded
// 1e-6..100 limits. Derives the min/max pixels-per-world-unit from the map's
// own minzoom/maxzoom, allowing TILE_OVERZOOM_MAX extra levels of zoom-in
// (served by overzoomed parent tiles) and one extra level of zoom-out.
static void cam_scale_bounds(float *lo, float *hi) {
    double base = 256.0 / (double)MVT_EXTENT_DEFAULT;   // scale at z = refzoom
    int out_levels = (int)g_meta.refzoom - (int)g_meta.minzoom + 1;
    if (out_levels < 0) out_levels = 0;
    int in_levels  = ((int)g_meta.maxzoom - (int)g_meta.refzoom) + TILE_OVERZOOM_MAX;
    if (in_levels < 0) in_levels = 0;
    *lo = (float)(base / (double)(1u << out_levels));
    *hi = (float)(base * (double)(1u << in_levels));
}
// ===== END WAYPOINT_MAP_ENGINE ===============================================

// =============================================================================
//  Vector primitive rendering (Arduino_GFX)
// =============================================================================
static inline bool onscreen(int x, int y) { return x>=0 && y>=0 && x<LCD_WIDTH && y<LCD_HEIGHT; }

// -----------------------------------------------------------------------------
//  Projected-geometry scratch. One shared pair of buffers (rendering is
//  single-threaded) instead of the old 256-vertex stack arrays, which quietly
//  truncated any polygon longer than 256 vertices -- most admin boundaries and
//  large water bodies -- into a wrong shape.
// -----------------------------------------------------------------------------
#define WP_MAX_PTS 1536

static int16_t g_px[WP_MAX_PTS];
static int16_t g_py[WP_MAX_PTS];
static int     g_xs[512];       // scanline crossing scratch

// Project `n` world vertices into g_px/g_py. Far-off-screen points are clamped
// rather than allowed to overflow the integer edge maths.
static void project_pts(const FVec2 *pts, int n) {
    for (int i = 0; i < n; i++) {
        int ix, iy;
        world_to_screen(pts[i].x, pts[i].y, &ix, &iy);
        if (ix < -32000) ix = -32000; else if (ix > 32000) ix = 32000;
        if (iy < -32000) iy = -32000; else if (iy > 32000) iy = 32000;
        g_px[i] = (int16_t)ix;
        g_py[i] = (int16_t)iy;
    }
}

// Even-odd scanline fill of one or more closed rings already projected.
// `ring_end[k]` is one past the last vertex of ring k; rings must therefore be
// contiguous in g_px/g_py, which is how the decoder lays them out.
//
// Holes need no special case: even-odd toggles the fill between each crossing
// pair, so an interior ring punches straight through. That is why this is
// correct where the old "paint every ring as its own filled polygon" was not
// -- and unlike painting a hole in the background colour, whatever is beneath
// (water, land use) still shows through.
static void fill_rings_screen(int npts, const uint16_t *ring_end, uint16_t nr,
                              uint16_t col) {
    if (npts < 3 || nr == 0) return;
    int ymin = LCD_HEIGHT, ymax = -1;
    for (int i = 0; i < npts; i++) {
        if (g_py[i] < ymin) ymin = g_py[i];
        if (g_py[i] > ymax) ymax = g_py[i];
    }
    if (ymin < 0) ymin = 0;
    if (ymax >= LCD_HEIGHT) ymax = LCD_HEIGHT - 1;
    if (ymax < ymin) return;

    const int maxx = (int)(sizeof(g_xs) / sizeof(g_xs[0]));
    for (int y = ymin; y <= ymax; y++) {
        int cnt = 0;
        int a = 0;
        for (uint16_t r = 0; r < nr; r++) {
            int b = (int)ring_end[r];
            if (b > npts) b = npts;
            if (b - a < 3) { a = b; continue; }        // degenerate ring
            for (int i = a, j = b - 1; i < b; j = i++) {
                int yi = g_py[i], yj = g_py[j];
                // half-open test: counts each vertex exactly once
                if ((yi <= y && yj > y) || (yj <= y && yi > y)) {
                    if (cnt < maxx)
                        g_xs[cnt++] = g_px[i] +
                            (int)(((int32_t)(y - yi) * (g_px[j] - g_px[i])) / (yj - yi));
                }
            }
            a = b;
        }
        if (cnt < 2) continue;
        for (int i = 1; i < cnt; i++) {               // insertion sort
            int v = g_xs[i], j = i - 1;
            while (j >= 0 && g_xs[j] > v) { g_xs[j + 1] = g_xs[j]; j--; }
            g_xs[j + 1] = v;
        }
        for (int i = 0; i + 1 < cnt; i += 2) {
            int xa = g_xs[i], xb = g_xs[i + 1];
            if (xb < 0 || xa >= LCD_WIDTH) continue;
            if (xa < 0) xa = 0;
            if (xb >= LCD_WIDTH) xb = LCD_WIDTH - 1;
            if (xb >= xa) gfx->drawFastHLine(xa, y, xb - xa + 1, col);
        }
    }
}

// Fill + outline one decoded polygon (holes included).
static void draw_polygon_world(const FMapObj &o, uint16_t fill_col,
                               uint16_t line_col, bool do_fill) {
    const int n = o.point_count;
    if (n < 3) return;
    if (n > WP_MAX_PTS) return;      // pathological ring: skip rather than truncate
    project_pts(o.points, n);

    if (do_fill) {
        if (o.ring_end && o.rings > 1) {
            fill_rings_screen(n, o.ring_end, o.rings, fill_col);
        } else {
            uint16_t one = (uint16_t)n;
            fill_rings_screen(n, &one, 1, fill_col);
        }
    }
    // Outline short rings only: buildings and small parcels read much better
    // with an edge, but stroking a whole coastline costs more than it adds.
    if (n <= 128 && line_col != fill_col) {
        int a = 0;
        const uint16_t nr = (o.ring_end && o.rings > 1) ? o.rings : (uint16_t)1;
        for (uint16_t r = 0; r < nr; r++) {
            const int b = o.ring_end ? (int)o.ring_end[r] : n;
            for (int i = a, j = b - 1; i < b; j = i++)
                gfx->drawLine(g_px[j], g_py[j], g_px[i], g_py[i], line_col);
            a = b;
        }
    }
}

// One thick segment, drawn as a filled quad instead of `w` parallel
// drawLine() calls -- fewer bus transactions and no seams between the offsets.
static void draw_thick_seg(int x0, int y0, int x1, int y1, int w, uint16_t c) {
    if (w <= 1) { gfx->drawLine(x0, y0, x1, y1, c); return; }
    int dx = x1 - x0, dy = y1 - y0;
    float len = sqrtf((float)(dx * dx + dy * dy));
    if (len < 0.5f) { gfx->fillCircle(x0, y0, (w + 1) / 2, c); return; }
    float half = (float)w * 0.5f;
    float px = (float)(-dy) / len * half;
    float py = (float)( dx) / len * half;
    g_px[0] = (int16_t)(x0 + (int)px); g_py[0] = (int16_t)(y0 + (int)py);
    g_px[1] = (int16_t)(x1 + (int)px); g_py[1] = (int16_t)(y1 + (int)py);
    g_px[2] = (int16_t)(x1 - (int)px); g_py[2] = (int16_t)(y1 - (int)py);
    g_px[3] = (int16_t)(x0 - (int)px); g_py[3] = (int16_t)(y0 - (int)py);
    uint16_t quad_end = 4;
    fill_rings_screen(4, &quad_end, 1, c);
    // Round the joints on the fatter roads so corners don't show notches.
    if (w >= 4) {
        gfx->fillCircle(x0, y0, w / 2, c);
        gfx->fillCircle(x1, y1, w / 2, c);
    }
}

// Draw polyline in world coords, clipped by screen bounds.
static void draw_polyline_world(const FVec2 *pts, int n, uint16_t col,
                                uint8_t width, uint8_t dash) {
    if (n < 2 || width == 0) return;
    int sx_prev = 0, sy_prev = 0;
    world_to_screen(pts[0].x, pts[0].y, &sx_prev, &sy_prev);
    const int dash_on = dash ? dash : 0;
    int step = 0;
    for (int i = 1; i < n; i++) {
        int sx, sy;
        world_to_screen(pts[i].x, pts[i].y, &sx, &sy);
        // trivial reject if both endpoints are far off the same side
        bool skip = (sx < -60 && sx_prev < -60) ||
                    (sy < -60 && sy_prev < -60) ||
                    (sx > LCD_WIDTH + 60 && sx_prev > LCD_WIDTH + 60) ||
                    (sy > LCD_HEIGHT + 60 && sy_prev > LCD_HEIGHT + 60);
        if (!skip) {
            if (dash_on == 0 || (step % (dash_on * 2)) < dash_on)
                draw_thick_seg(sx_prev, sy_prev, sx, sy, width, col);
        }
        sx_prev = sx; sy_prev = sy;
        step++;
    }
}

// =============================================================================
//  Sensors (QMI8658 + AK09918) — ported from NavOS
// =============================================================================
#define QMI_ADDR 0x6B
#define AKM_ADDR 0x0C
static bool g_has_imu=false, g_has_mag=false;
static float g_ax=0, g_ay=0, g_az=1.0f;
static float g_mx=0, g_my=0, g_mz=0;
static float g_mx_off=0, g_my_off=0, g_mz_off=0;
static float g_mx_scl=1, g_my_scl=1, g_mz_scl=1;
static float g_heading = -1.0f;
static float g_marked_heading = -1.0f;

static bool i2c_read(uint8_t addr, uint8_t reg, uint8_t *out, size_t n) {
    Wire.beginTransmission(addr); Wire.write(reg);
    if (Wire.endTransmission(false) != 0) return false;
    Wire.requestFrom((int)addr, (int)n);
    size_t i=0; while (Wire.available() && i<n) out[i++] = Wire.read();
    return i == n;
}
static bool i2c_write(uint8_t addr, uint8_t reg, uint8_t val) {
    Wire.beginTransmission(addr); Wire.write(reg); Wire.write(val);
    return Wire.endTransmission() == 0;
}
static void imu_init() {
    Wire.beginTransmission(QMI_ADDR);
    if (Wire.endTransmission() == 0) {
        uint8_t who=0; i2c_read(QMI_ADDR, 0x00, &who, 1);
        if (who == 0x05) {
            i2c_write(QMI_ADDR, 0x02, 0x60);
            i2c_write(QMI_ADDR, 0x03, 0x95);
            i2c_write(QMI_ADDR, 0x04, 0x53);
            i2c_write(QMI_ADDR, 0x08, 0x03);
            g_has_imu = true;
        }
    }
    Wire.beginTransmission(AKM_ADDR);
    if (Wire.endTransmission() == 0) { i2c_write(AKM_ADDR, 0x31, 0x08); g_has_mag = true; }
}
static void imu_read() {
    if (g_has_imu) {
        uint8_t b[6];
        if (i2c_read(QMI_ADDR, 0x35, b, 6)) {
            int16_t ax=(int16_t)((b[1]<<8)|b[0]);
            int16_t ay=(int16_t)((b[3]<<8)|b[2]);
            int16_t az=(int16_t)((b[5]<<8)|b[4]);
            g_ax = ax/16384.0f; g_ay = ay/16384.0f; g_az = az/16384.0f;
        }
    }
    if (g_has_mag) {
        uint8_t b[8]; i2c_read(AKM_ADDR, 0x10, b, 8);
        int16_t mx=(int16_t)((b[2]<<8)|b[1]);
        int16_t my=(int16_t)((b[4]<<8)|b[3]);
        int16_t mz=(int16_t)((b[6]<<8)|b[5]);
        g_mx = (mx*0.15f) - g_mx_off; g_my = (my*0.15f) - g_my_off; g_mz = (mz*0.15f) - g_mz_off;
        g_mx *= g_mx_scl; g_my *= g_my_scl; g_mz *= g_mz_scl;
    }
}
// Motion-based wake: if the IMU accelerometer vector jumps more than a small
// threshold between consecutive reads, count that as user activity so the
// screen brightens back up without needing a touch.
static bool wp_imu_motion_detected() {
    static float last_ax = 0, last_ay = 0, last_az = 0;
    static bool  primed = false;
    if (!g_has_imu) return false;
    if (!primed) { last_ax = g_ax; last_ay = g_ay; last_az = g_az; primed = true; return false; }
    float d = fabsf(g_ax - last_ax) + fabsf(g_ay - last_ay) + fabsf(g_az - last_az);
    last_ax = g_ax; last_ay = g_ay; last_az = g_az;
    // ~0.12 g worth of change across the 3 axes = "user moved it deliberately".
    return d > 0.12f;
}

static float tilt_comp_heading() {
    float roll  = atan2f(g_ay, g_az);
    float pitch = atan2f(-g_ax, sqrtf(g_ay*g_ay + g_az*g_az));
    float xh = g_mx*cosf(pitch) + g_mz*sinf(pitch);
    float yh = g_mx*sinf(roll)*sinf(pitch) + g_my*cosf(roll) - g_mz*sinf(roll)*cosf(pitch);
    float h = atan2f(yh, xh) * 180.0f / (float)M_PI;
    if (h < 0) h += 360.0f;
    return h;
}
static const char *card_for(float h) {
    static const char *cards[] = {"N","NE","E","SE","S","SW","W","NW"};
    int idx = ((int)((h + 22.5f) / 45.0f)) % 8;
    return cards[idx];
}

// Mag calibration figure-8 sample buffer
static bool  g_cal_active = false;
static uint32_t g_cal_start_ms = 0;
static float g_cal_minx=1e9,g_cal_maxx=-1e9,g_cal_miny=1e9,g_cal_maxy=-1e9,g_cal_minz=1e9,g_cal_maxz=-1e9;

static void mag_cal_start() {
    g_cal_active = true; g_cal_start_ms = millis();
    g_cal_minx=1e9;g_cal_maxx=-1e9;g_cal_miny=1e9;g_cal_maxy=-1e9;g_cal_minz=1e9;g_cal_maxz=-1e9;
}
static void mag_cal_tick() {
    if (!g_cal_active) return;
    // note: g_mx/y/z here are already offset+scaled by imu_read; we want raw-ish; refresh from raw
    uint8_t b[8]; if (i2c_read(AKM_ADDR, 0x10, b, 8)) {
        int16_t mx=(int16_t)((b[2]<<8)|b[1]);
        int16_t my=(int16_t)((b[4]<<8)|b[3]);
        int16_t mz=(int16_t)((b[6]<<8)|b[5]);
        float rx=mx*0.15f, ry=my*0.15f, rz=mz*0.15f;
        if (rx<g_cal_minx)g_cal_minx=rx; if (rx>g_cal_maxx)g_cal_maxx=rx;
        if (ry<g_cal_miny)g_cal_miny=ry; if (ry>g_cal_maxy)g_cal_maxy=ry;
        if (rz<g_cal_minz)g_cal_minz=rz; if (rz>g_cal_maxz)g_cal_maxz=rz;
    }
    if (millis() - g_cal_start_ms > 20000) {
        g_mx_off = (g_cal_maxx + g_cal_minx) * 0.5f;
        g_my_off = (g_cal_maxy + g_cal_miny) * 0.5f;
        g_mz_off = (g_cal_maxz + g_cal_minz) * 0.5f;
        float rx = (g_cal_maxx - g_cal_minx) * 0.5f;
        float ry = (g_cal_maxy - g_cal_miny) * 0.5f;
        float rz = (g_cal_maxz - g_cal_minz) * 0.5f;
        float ravg = (rx + ry + rz) / 3.0f;
        g_mx_scl = ravg / (rx > 0 ? rx : 1);
        g_my_scl = ravg / (ry > 0 ? ry : 1);
        g_mz_scl = ravg / (rz > 0 ? rz : 1);
        Preferences p; p.begin("waypoint", false);
        p.putFloat("mxo", g_mx_off); p.putFloat("myo", g_my_off); p.putFloat("mzo", g_mz_off);
        p.putFloat("mxs", g_mx_scl); p.putFloat("mys", g_my_scl); p.putFloat("mzs", g_mz_scl);
        p.end();
        g_cal_active = false;
    }
}

// Sun position (NOAA approx, from NavOS)
static float g_lat = 0, g_lon = 0;
static void read_loc() {
    File f = SD_MMC.open("/WATCH/WayPoint/loc.txt", FILE_READ);
    if (!f) return;
    String s = f.readString(); f.close();
    int c = s.indexOf(','); if (c<0) return;
    g_lat = s.substring(0, c).toFloat();
    g_lon = s.substring(c+1).toFloat();
}
static void sun_position(float *az, float *el) {
    time_t now; time(&now);
    if (now < 1700000000) { *az=*el=-1; return; }
    struct tm tm; gmtime_r(&now, &tm);
    int doy = tm.tm_yday + 1;
    float hh = tm.tm_hour + tm.tm_min/60.0f + tm.tm_sec/3600.0f;
    float decl = 23.45f * sinf((360.0f/365.0f) * (doy - 81) * (float)M_PI/180.0f);
    float ha = (hh - 12) * 15.0f + g_lon;
    float lat_r = g_lat * (float)M_PI/180.0f;
    float decl_r = decl * (float)M_PI/180.0f;
    float ha_r = ha * (float)M_PI/180.0f;
    float sin_el = sinf(lat_r)*sinf(decl_r) + cosf(lat_r)*cosf(decl_r)*cosf(ha_r);
    float el_r = asinf(sin_el);
    float az_r = atan2f(-sinf(ha_r)*cosf(decl_r), cosf(lat_r)*sinf(decl_r) - sinf(lat_r)*cosf(decl_r)*cosf(ha_r));
    *az = fmodf(az_r * 180.0f/(float)M_PI + 360.0f, 360.0f);
    *el = el_r * 180.0f/(float)M_PI;
}

// =============================================================================
//  Saved locations + breadcrumbs
// =============================================================================
struct SavedLoc { int32_t x, y; uint8_t icon; String name; };
#define MAX_SAVED 64
static SavedLoc g_saved[MAX_SAVED];
static int g_saved_count = 0;

static String saved_csv_path() { return "/WATCH/WayPoint/saved/" + g_meta.title + ".csv"; }

static void saved_load() {
    g_saved_count = 0;
    SD_MMC.mkdir("/WATCH/WayPoint/saved");
    File f = SD_MMC.open(saved_csv_path(), FILE_READ);
    if (!f) return;
    while (f.available() && g_saved_count < MAX_SAVED) {
        String ln = f.readStringUntil('\n');
        if (ln.length() < 3) continue;
        int p1 = ln.indexOf(','); if (p1<0) continue;
        int p2 = ln.indexOf(',', p1+1); if (p2<0) continue;
        int p3 = ln.indexOf(',', p2+1); if (p3<0) continue;
        SavedLoc s;
        s.x = ln.substring(0,p1).toInt();
        s.y = ln.substring(p1+1,p2).toInt();
        s.icon = (uint8_t)ln.substring(p2+1,p3).toInt();
        s.name = ln.substring(p3+1); s.name.trim();
        g_saved[g_saved_count++] = s;
    }
    f.close();
}
static void saved_add(int32_t wx, int32_t wy, const String &name, uint8_t icon) {
    if (g_saved_count >= MAX_SAVED) return;
    SavedLoc s = { wx, wy, icon, name };
    g_saved[g_saved_count++] = s;
    File f = SD_MMC.open(saved_csv_path(), FILE_APPEND);
    if (f) { char b[128]; snprintf(b, sizeof(b), "%ld,%ld,%u,%s\n", (long)wx,(long)wy, icon, name.c_str()); f.print(b); f.close(); }
}

static void breadcrumb_add(int32_t wx, int32_t wy) {
    File f = SD_MMC.open("/WATCH/WayPoint/breadcrumbs.csv", FILE_APPEND);
    if (!f) return;
    time_t t; time(&t);
    char b[128]; snprintf(b,128,"%lu,%s,%ld,%ld\n", (unsigned long)t, g_meta.title.c_str(), (long)wx, (long)wy);
    f.print(b); f.close();
}

// Beacon loader: reads a CSV of name,x,y,online per line from
// /WATCH/WayPoint/beacons.csv — provides a Meshtastic-style layer without
// requiring the actual radio to be wired up yet.
static void beacons_load() {
    g_beacon_count = 0;
    File f = SD_MMC.open("/WATCH/WayPoint/beacons.csv", FILE_READ);
    if (!f) return;
    while (f.available() && g_beacon_count < MAX_BEACONS) {
        String ln = f.readStringUntil('\n');
        ln.trim();
        if (ln.length() < 3 || ln.startsWith("#")) continue;
        int p1 = ln.indexOf(','); if (p1 < 0) continue;
        int p2 = ln.indexOf(',', p1+1); if (p2 < 0) continue;
        int p3 = ln.indexOf(',', p2+1);
        Beacon b;
        b.name   = ln.substring(0, p1);
        b.x      = (int32_t)ln.substring(p1+1, p2).toInt();
        b.y      = (int32_t)(p3 > 0 ? ln.substring(p2+1, p3) : ln.substring(p2+1)).toInt();
        b.online = (uint8_t)(p3 > 0 ? ln.substring(p3+1).toInt() : 1);
        g_beacons[g_beacon_count++] = b;
    }
    f.close();
}

// =============================================================================
//  Library carousel  (each "map" is a directory of MVT tiles)
// =============================================================================
#define MAX_MAPS 32
// What the library knows about a map before it is opened. The zoom range and
// bounds are read out of metadata.json during the scan, so a card can describe
// a map - and its detail ladder - without loading a single tile.
struct MapEntry {
    String path; String title; String desc;
    uint8_t zmin = 10, zmax = 17;
    float min_lat = 0, min_lon = 0, max_lat = 0, max_lon = 0;
    bool has_meta = false;
};
static MapEntry g_maps[MAX_MAPS];
static int g_map_count_lib = 0;
static int g_map_sel = 0;
static bool g_lib_redraw = true;
static bool g_lib_info   = false;   // details panel: swipe up / long press

// Detect if a directory looks like an MVT tile-tree (contains at least one
// numeric zoom folder holding a tile file). Cheap check: probe a handful of
// zoom levels for existence of {dir}/{z}/.
static bool dir_looks_like_mvt_map(const String &dir_path) {
    for (uint8_t z = 0; z <= 22; z++) {
        char zb[16]; snprintf(zb, sizeof(zb), "/%u", (unsigned)z);
        if (SD_MMC.exists((dir_path + zb).c_str())) return true;
    }
    return false;
}

static void library_scan() {
    g_map_count_lib = 0;
    SD_MMC.mkdir("/WATCH");
    SD_MMC.mkdir("/WATCH/WayPoint");
    SD_MMC.mkdir("/WATCH/WayPoint/saved");
    File d = SD_MMC.open(g_map_folder);
    if (!d) return;
    File ent = d.openNextFile();
    while (ent && g_map_count_lib < MAX_MAPS) {
        if (ent.isDirectory()) {
            String n = ent.name();
            int p = n.lastIndexOf('/'); if (p>=0) n = n.substring(p+1);
            // Skip the 'saved' bookkeeping directory
            if (n != "saved" && n.length() && n[0] != '.') {
                String path = g_map_folder + "/" + n;
                if (dir_looks_like_mvt_map(path)) {
                    FMapMeta m;
                    if (mvt_map_read_header_meta(path, m)) {
                        MapEntry &e = g_maps[g_map_count_lib];
                        e.path  = path;
                        e.title = m.title.length() ? m.title : n;
                        e.desc  = m.desc.length()  ? m.desc
                                 : String("Vector map (MVT)");
                        e.zmin = m.minzoom; e.zmax = m.maxzoom;
                        e.min_lat = m.min_lat; e.min_lon = m.min_lon;
                        e.max_lat = m.max_lat; e.max_lon = m.max_lon;
                        e.has_meta = SD_MMC.exists((path + "/metadata.json").c_str());
                        g_map_count_lib++;
                    }
                }
            }
        }
        ent = d.openNextFile();
    }

    // Reorder: put "recent" maps first, in their MRU order.
    Preferences p; p.begin("waypoint", true);
    String recent[5];
    for (int i=0; i<5; i++) { char k[6]; snprintf(k,6,"rm%d",i); recent[i] = p.getString(k, ""); }
    p.end();
    // Stable-ish selection sort: for each recent slot, promote its match to that index.
    int slot = 0;
    for (int i=0; i<5 && slot<g_map_count_lib; i++) {
        if (!recent[i].length()) continue;
        int found = -1;
        for (int j=slot; j<g_map_count_lib; j++) if (g_maps[j].path == recent[i]) { found = j; break; }
        if (found < 0 || found == slot) { if (found == slot) slot++; continue; }
        MapEntry tmp = g_maps[found];
        for (int k=found; k>slot; k--) g_maps[k] = g_maps[k-1];
        g_maps[slot] = tmp;
        slot++;
    }
}

static void library_draw() {
    if (!g_lib_redraw) return;
    g_lib_redraw = false;
    const ThemePalette &t = TH();
    gfx->fillScreen(t.bg);
    // grid backdrop
    for (int x=0; x<LCD_WIDTH; x+=40) gfx->drawLine(x, 0, x, LCD_HEIGHT, t.chrome_bg);
    for (int y=0; y<LCD_HEIGHT; y+=40) gfx->drawLine(0, y, LCD_WIDTH, y, t.chrome_bg);

    int cx = LCD_WIDTH/2;
    gfx->setTextColor(t.accent); gfx->setTextSize(2);
    gfx->setCursor(cx - 92, 22); gfx->print("WAYPOINT / OS");
    gfx->setTextColor(t.text_dim); gfx->setTextSize(1);
    gfx->setCursor(cx - 40, 48); gfx->print("map library");

    if (g_map_count_lib == 0) {
        gfx->setTextColor(t.text); gfx->setTextSize(1);
        const char *lines[] = {
            "No map folders found.",
            "",
            "Place MVT tiles under:",
            "/WATCH/WayPoint/<name>/",
            "     {z}/{x}/{y}.pbf",
            "",
            "Optional metadata.json for",
            "name, bounds and minzoom/",
            "maxzoom.",
        };
        int y = 110;
        for (auto &ln : lines) { gfx->setCursor(28, y); gfx->print(ln); y += 16; }
        gfx->setTextColor(t.text_dim);
        gfx->setCursor(28, LCD_HEIGHT - 60); gfx->print("BOOT hold 1.5s -> LauncherOS");
        return;
    }

    MapEntry &e = g_maps[g_map_sel];
    int card_x = 20, card_y = 104, card_w = LCD_WIDTH - 40, card_h = 246;
    gfx->fillRoundRect(card_x, card_y, card_w, card_h, 16, t.chrome_bg);
    gfx->drawRoundRect(card_x, card_y, card_w, card_h, 16, t.accent);

    gfx->setTextColor(t.text); gfx->setTextSize(2);
    gfx->setCursor(card_x + 18, card_y + 16);
    String shown = e.title.length() > 18 ? e.title.substring(0,17) + "~" : e.title;
    gfx->print(shown);

    // Zoom range chip: the detail ladder is worth knowing before opening a map,
    // and metadata.json already told us during the scan.
    if (e.has_meta) {
        char zb[24];
        snprintf(zb, sizeof(zb), "z%u-%u", (unsigned)e.zmin, (unsigned)e.zmax);
        int zw = (int)strlen(zb) * 6 + 12;
        int zx = card_x + card_w - zw - 14;
        gfx->fillRoundRect(zx, card_y + 13, zw, 18, 5, t.bg);
        gfx->drawRoundRect(zx, card_y + 13, zw, 18, 5, t.chrome_border);
        gfx->setTextColor(t.text_dim); gfx->setTextSize(1);
        gfx->setCursor(zx + 6, card_y + 19);
        gfx->print(zb);
    }

    gfx->setTextColor(t.text_dim); gfx->setTextSize(1);
    gfx->setCursor(card_x + 18, card_y + 44);
    String fn = e.path; int p = fn.lastIndexOf('/'); if (p>=0) fn = fn.substring(p+1);
    gfx->print("dir: "); gfx->print(fn);

    // Body: the map's description, or - when the details panel is open - the
    // metadata the description leaves out.
    gfx->setTextColor(t.text); gfx->setTextSize(1);
    const int max_chars = (card_w - 36) / 6;
    int dy = card_y + 66;
    const int dy_max = card_y + card_h - 12;
    if (g_lib_info) {
        String lines[6];
        int n = 0;
        lines[n++] = e.path;
        if (e.has_meta) {
            char zb[48];
            snprintf(zb, sizeof(zb), "zooms: z%u to z%u", (unsigned)e.zmin, (unsigned)e.zmax);
            lines[n++] = zb;
            if (e.min_lat != 0 || e.max_lat != 0) {
                char bb[80];
                snprintf(bb, sizeof(bb), "bounds: %.4f,%.4f .. %.4f,%.4f",
                         (double)e.min_lon, (double)e.min_lat,
                         (double)e.max_lon, (double)e.max_lat);
                lines[n++] = bb;
            } else {
                lines[n++] = "bounds: not stated";
            }
        } else {
            lines[n++] = "no metadata.json: the folder name";
            lines[n++] = "is the title, and the zoom levels";
            lines[n++] = "are discovered when it opens";
        }
        for (int i = 0; i < n && dy < dy_max; i++) {
            String d = lines[i];
            while (d.length() > 0 && dy < dy_max) {
                String chunk;
                if ((int)d.length() <= max_chars) { chunk = d; d = ""; }
                else {
                    int br = max_chars;
                    while (br > 0 && d[br] != ' ') br--;
                    if (br == 0) br = max_chars;
                    chunk = d.substring(0, br);
                    d = d.substring(br); d.trim();
                }
                gfx->setCursor(card_x + 18, dy); gfx->print(chunk);
                dy += 14;
            }
        }
    } else {
        String d = e.desc;
        while (d.length() > 0 && dy < dy_max) {
            String chunk;
            if ((int)d.length() <= max_chars) { chunk = d; d = ""; }
            else {
                int br = max_chars;
                while (br > 0 && d[br] != ' ') br--;
                if (br == 0) br = max_chars;
                chunk = d.substring(0, br);
                d = d.substring(br); d.trim();
            }
            gfx->setCursor(card_x + 18, dy); gfx->print(chunk);
            dy += 14;
        }
    }

    gfx->setTextColor(t.accent); gfx->setTextSize(3);
    if (g_map_count_lib > 1) {
        gfx->setCursor(card_x - 14, card_y + card_h/2 - 12); gfx->print("<");
        gfx->setCursor(card_x + card_w - 4, card_y + card_h/2 - 12); gfx->print(">");
    }

    // Position: a counter, plus dots when the library is small enough for dots
    // to mean anything (a library of 30 maps needs the number, not 30 dots).
    if (g_map_count_lib > 1) {
        const int dots = (g_map_count_lib > 9) ? 9 : g_map_count_lib;
        const int sel  = (g_map_count_lib > 1)
                       ? (int)((long)g_map_sel * (dots - 1) / (g_map_count_lib - 1)) : 0;
        const int dx0  = cx - (dots * 12) / 2;
        for (int i = 0; i < dots; i++) {
            int px = dx0 + i * 12 + 5, py = card_y + card_h + 22;
            if (i == sel) gfx->fillCircle(px, py, 3, t.accent);
            else          gfx->drawCircle(px, py, 3, t.chrome_border);
        }
    }
    char counter[32];
    snprintf(counter, sizeof(counter), "%d / %d", g_map_sel + 1, g_map_count_lib);
    gfx->setTextColor(t.text_dim); gfx->setTextSize(1);
    gfx->setCursor(cx - 16, card_y + card_h + 8);
    gfx->print(counter);

    // Gestures, spelled out once. This is the whole navigation model now, so it
    // earns the space the BOOT-button chords used to take.
    int hy = LCD_HEIGHT - 78;
    gfx->setTextColor(t.text_dim);
    gfx->setCursor(20, hy);      gfx->print("swipe < >    browse the library");
    gfx->setCursor(20, hy + 14); gfx->print("tap card     open the map");
    gfx->setCursor(20, hy + 28); gfx->print(g_lib_info ? "swipe v      hide details"
                                                        : "swipe ^      map details");
    gfx->setCursor(20, hy + 42); gfx->print("hold 1.5s    back to LauncherOS");
}

// =============================================================================
//  Splash
// =============================================================================
static void splash_show() {
    int cx = LCD_WIDTH/2, cy = LCD_HEIGHT/2;
    const ThemePalette &t = TH();
    for (int f = 0; f < 22; f++) {
        gfx->fillScreen(t.bg);
        // isometric grid pulse
        int step = 20 + (f % 6) * 2;
        for (int x = -LCD_HEIGHT; x < LCD_WIDTH; x += step) {
            gfx->drawLine(x, 0, x + LCD_HEIGHT, LCD_HEIGHT, t.chrome_bg);
        }
        // pin
        gfx->fillCircle(cx, cy - 20, 22, t.accent);
        gfx->fillCircle(cx, cy - 20, 10, t.bg);
        gfx->fillTriangle(cx - 22, cy - 8, cx + 22, cy - 8, cx, cy + 30, t.accent);
        // ring
        gfx->drawCircle(cx, cy - 20, 40 + (f % 8), t.warn);
        gfx->setTextColor(t.text); gfx->setTextSize(2);
        gfx->setCursor(cx - 100, LCD_HEIGHT - 90);
        gfx->print("WAYPOINT / OS");
        gfx->setTextColor(t.text_dim); gfx->setTextSize(1);
        gfx->setCursor(cx - 62, LCD_HEIGHT - 66);
        gfx->print("offline vector nav");
        delay(35);
    }
}

// =============================================================================
//  Map render pipeline
// =============================================================================
static uint32_t g_render_last_ms = 0;
static uint16_t g_render_obj_count = 0;
static uint32_t g_render_time_us = 0;

// Effective (fractional) camera zoom, recomputed from the camera scale at the
// top of draw_map_frame(). It is the scale signal every styler decision uses
// (road widths, whether a feature is in its zoom band), so it lives here rather
// than being re-derived per call. Declared above the styler that reads it; the
// host build supplies its own copy in shim_style.h, which is why forgetting it
// here is invisible to the tests.
static float    g_cam_zoom_eff = 14.0f;

// tile_resolve() deliberately has no second copy here. It is defined once, in
// the WAYPOINT_MAP_ENGINE region above, because that is the definition the host
// tests compile -- a copy left in the render pipeline is both a redefinition
// error and a pair of implementations free to drift apart.

// Ensure current + neighbouring tiles are resolved (with overzoom fallback) at
// the active slippy zoom, so the 3x3 render ring has data to draw.
static void ensure_ring_loaded(int8_t z, int32_t ctx, int32_t cty) {
    const int32_t nmax = (int32_t)((1u << z) - 1);
    for (int j=-1; j<=1; j++) {
        for (int i=-1; i<=1; i++) {
            int32_t tx = ctx + i, ty = cty + j;
            if (tx < 0 || ty < 0 || tx > nmax || ty > nmax) continue;
            tile_resolve(z, tx, ty);
        }
    }
}

// Track A -- neighbour prefetch. Warm the cache one ring further out (radius 2)
// so panning doesn't stutter. Budget-limited: at most a few loads per frame,
// and only when the byte budget has headroom, so we never block the UI.
static void prefetch_ring(int8_t z, int32_t ctx, int32_t cty) {
    if (g_cache_bytes > (TILE_CACHE_PSRAM_BUDGET * 3) / 4) return;
    const int32_t nmax = (int32_t)((1u << z) - 1);
    int budget = 3;   // max tile loads per frame for prefetch
    for (int j=-2; j<=2 && budget > 0; j++) {
        for (int i=-2; i<=2 && budget > 0; i++) {
            if (i >= -1 && i <= 1 && j >= -1 && j <= 1) continue;  // inner ring already done
            int32_t tx = ctx + i, ty = cty + j;
            if (tx < 0 || ty < 0 || tx > nmax || ty > nmax) continue;
            if (cache_find(z, tx, ty)) continue;
            if (!tile_exists_on_disk(z, tx, ty)) continue;
            tile_load(z, tx, ty);
            budget--;
        }
    }
}

static void draw_map_frame();  // fwd
static bool g_need_map_redraw = true;

// -----------------------------------------------------------------------------
//  Per-object styling: colour, width, dash and whether this layer fills.
//
//  Widths now come from the feature's road class and from how far the camera
//  sits from the map's native zoom, so a motorway stays legible zoomed out and
//  an unclassified street only materialises once you are close to it.
// -----------------------------------------------------------------------------
struct WpStyle {
    uint16_t colour;
    uint8_t  width;
    uint8_t  dash;
    bool     fill;
    bool     outline;
};

static void mvt_style_object(const FMapObj &o, bool casing, WpStyle &s) {
    const ThemePalette &t = TH();
    s.colour  = layer_colour(o.layer);
    s.width   = 1;
    s.dash    = 0;
    s.fill    = false;
    s.outline = false;

    // Per-map style override: honoured above every automatic decision below.
    // Nothing in the v0.2 UI sets one yet (Tools -> Layers only toggles layers
    // on and off) - it is the hook a future per-map style editor would drive,
    // and the one path a theme genuinely cannot override.
    if (g_styles[o.layer].set) {
        s.colour = g_styles[o.layer].colour;
        s.width  = g_styles[o.layer].width_px ? g_styles[o.layer].width_px : 1;
        s.dash   = g_styles[o.layer].dash_len;
    } else if (o.layer == LAY_LANDCOVER) {
        // Ground tints: the base colour says "this is land", the cover kind
        // says what kind of land, mixed against the theme's own colours so
        // every theme stays coherent. Scrub and sand dominate Western
        // Australia, so they get the strongest treatment.
        const uint16_t base = s.colour;
        switch (o.sub) {
            case COV_FOREST:  s.colour = wp_mix565(base, t.parks,    190); break;
            case COV_GRASS:   s.colour = wp_mix565(base, t.parks,    120); break;
            case COV_SCRUB:   s.colour = wp_mix565(base, t.warn,      70); break;
            case COV_SAND:    s.colour = wp_mix565(base, t.text,      70); break;
            case COV_ROCK:    s.colour = wp_mix565(base, t.text_dim,  80); break;
            case COV_FARM:    s.colour = wp_mix565(base, t.warn,     120); break;
            case COV_WETLAND: s.colour = wp_mix565(base, t.water,     90); break;
            case COV_BUILT:   s.colour = wp_mix565(base, t.buildings, 60); break;
            default: break;
        }
    } else if (o.layer == LAY_LANDUSE) {
        s.colour = wp_mix565(s.colour, t.buildings, 40);
    }

    switch (o.layer) {
        case LAY_EARTH:
        case LAY_LANDCOVER:
        case LAY_LANDUSE:
        case LAY_WATER:
        case LAY_PARKS:
        case LAY_SCHOOLS:
            s.fill = (o.flags & OF_FILLED) != 0;
            break;
        case LAY_BUILDINGS:
            s.fill    = true;
            s.outline = true;
            break;
        case LAY_BOUNDARY:
            s.colour = wp_mix565(t.text_dim, t.bg, 110);
            break;
        case LAY_TRANSIT:
            // Tunnels are checked FIRST: the old order let every rail tunnel
            // fall into the plain-rail dash, so underground and surface lines
            // looked identical on the map.
            if (o.flags & OF_DASHED)       s.dash = 5;    // brunnel=tunnel
            else if (o.sub == RC_RAIL)     s.dash = 6;
            break;
        case LAY_PATHS:
            s.dash   = 4;
            s.colour = wp_mix565(t.paths, t.bg, 40);
            break;
        case LAY_ROADS: {
            // An explicit per-map width/colour (if one is ever set) replaces the
            // automatic ones, but the casing pass still stays 2 px wider.
            const bool over = g_styles[o.layer].set;
            if (!over) {
                int dz = (int)lroundf(g_cam_zoom_eff) - (int)g_meta.refzoom;
                if (dz >  1) dz =  1;      // never grow past 2x
                if (dz < -6) dz = -6;      // never shrink past 1/64
                const uint8_t base = ROAD_WIDTH_BASE[o.sub < RC__COUNT ? o.sub : RC_OTHER];
                int w = (dz >= 0) ? (base << dz)
                                  : ((base + (1 << (-dz)) / 2) >> (-dz));
                if (w < 1) w = 1;
                s.width = (uint8_t)w;
                if (o.sub == RC_MOTORWAY || o.sub == RC_TRUNK) {
                    s.colour = wp_mix565(t.roads, t.warn, 55);
                }
                // A tunnel (brunnel=tunnel) recedes toward the background
                // instead of being dashed: dashed motorways read as roadworks.
                if (o.flags & OF_DASHED) s.colour = wp_mix565(s.colour, t.bg, 90);
            }
            if (casing) {
                // Derive the casing from whatever the fill ended up being, so a
                // tinted motorway keeps a matching outline instead of a casing
                // colour borrowed from the plain road colour.
                s.width  = (uint8_t)(s.width + 2);
                s.colour = wp_mix565(s.colour, t.bg, 165);
            }
            break;
        }
        case LAY_UNKNOWN:
        default:
            break;
    }
}

// Draw every object of one layer of one tile. Objects are pre-sorted by
// (layer, sort_key) and banded at load time, so this touches only the run it
// needs instead of rescanning the whole tile fifteen times per frame.
static void draw_layer_band(FMapTile *tl, uint8_t L, bool casing) {
    if (!tl || !tl->loaded || tl->obj_count == 0) return;
    const uint16_t b0 = tl->band[L], b1 = tl->band[L + 1];
    for (uint16_t i = b0; i < b1; i++) {
        FMapObj &o = tl->objs[i];
        if (g_cam_zoom_eff < (float)o.zmin || g_cam_zoom_eff > (float)o.zmax) continue;
        WpStyle s;
        mvt_style_object(o, casing, s);
        switch (o.type) {
            case OBJ_LINE:
                draw_polyline_world(o.points, o.point_count, s.colour, s.width, s.dash);
                g_render_obj_count++;
                break;
            case OBJ_POLY:
                if (s.fill && (o.flags & OF_FILLED)) {
                    const uint16_t edge = wp_mix565(s.colour, TH().bg, 90);
                    draw_polygon_world(o, s.colour, edge, true);
                } else {
                    draw_polyline_world(o.points, o.point_count, s.colour, s.width, s.dash);
                }
                g_render_obj_count++;
                break;
            case OBJ_POINT:
                if (o.point_count > 0) {
                    int sx, sy;
                    world_to_screen(o.points[0].x, o.points[0].y, &sx, &sy);
                    if (sx > -8 && sy > -8 && sx < LCD_WIDTH + 8 && sy < LCD_HEIGHT + 8) {
                        gfx->fillCircle(sx, sy, 3, s.colour);
                        g_render_obj_count++;
                    }
                }
                break;
        }
    }
}

// -----------------------------------------------------------------------------
//  Paint the visible ring LAYER BY LAYER instead of TILE BY TILE.
//
//  This is the fix for the biggest visual artefact in the old renderer: drawing
//  tile-by-tile meant a building decoded from the neighbouring tile painted
//  straight over the road decoded from this one, so every street appeared
//  chopped wherever a tile boundary happened to fall.
//
//  Roads additionally get two passes -- all casings, then all fills -- which is
//  what lets a side street join a trunk road instead of slicing through it.
// -----------------------------------------------------------------------------
static void draw_layers(FMapTile **ring, int nring) {
    for (uint8_t L = 0; L < LAY__COUNT; L++) {
        if (!(g_layer_mask & (1u << L))) continue;
        if (L == LAY_ROADS) {
            for (int k = 0; k < nring; k++) draw_layer_band(ring[k], L, true);
            for (int k = 0; k < nring; k++) draw_layer_band(ring[k], L, false);
        } else if (L == LAY_BOUNDARY) {
            for (int k = 0; k < nring; k++) draw_layer_band(ring[k], L, false);
        } else {
            for (int k = 0; k < nring; k++) draw_layer_band(ring[k], L, false);
        }
    }
}

// POI dots, gated by zoom, by rank and by the layer toggle.
static void render_pois() {
    if (!g_pois) return;
    const ThemePalette &t = TH();
    const uint16_t edge = wp_mix565(t.text, t.bg, 120);
    for (uint32_t i = 0; i < g_poi_count; i++) {
        const FMapPOI &p = g_pois[i];
        if (!(g_layer_mask & (1u << p.layer))) continue;
        if (g_cam_zoom_eff < (float)p.zmin) continue;
        int sx, sy;
        world_to_screen(p.x, p.y, &sx, &sy);
        if (sx < -6 || sy < -6 || sx > LCD_WIDTH + 6 || sy > LCD_HEIGHT + 6) continue;
        uint16_t c = t.shops;
        switch (p.type) {
            case 3: c = t.transit; break;
            case 4: c = t.schools; break;
            case 5: c = t.parks;   break;
            default: c = t.shops;  break;
        }
        const int r = (p.rank <= 1) ? 5 : 3;
        gfx->fillCircle(sx, sy, r, c);
        gfx->drawCircle(sx, sy, r + 1, edge);
    }
}

// -----------------------------------------------------------------------------
//  Label pass. Runs after the vector layers and after the dots, written
//  straight to the panel. Candidates are visited most-important-first and each
//  label claims an occupancy box; anything that would collide is skipped, so a
//  town name always wins the space it needs from a corner shop, and 300 shop
//  names cannot turn the map into mush.
// -----------------------------------------------------------------------------
#define LBL_CELL 8
#define LBL_COLS ((LCD_WIDTH  + LBL_CELL - 1) / LBL_CELL)
#define LBL_ROWS ((LCD_HEIGHT + LBL_CELL - 1) / LBL_CELL)
#define LBL_MAX  26

static uint8_t g_lbl_occ[(LBL_COLS * LBL_ROWS + 7) / 8];

static bool lbl_claim(int x0, int y0, int x1, int y1) {
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > LCD_WIDTH)  x1 = LCD_WIDTH;
    if (y1 > LCD_HEIGHT) y1 = LCD_HEIGHT;
    if (x1 <= x0 || y1 <= y0) return false;
    const int c0 = x0 / LBL_CELL, c1 = (x1 - 1) / LBL_CELL;
    const int r0 = y0 / LBL_CELL, r1 = (y1 - 1) / LBL_CELL;
    for (int r = r0; r <= r1; r++)
        for (int c = c0; c <= c1; c++) {
            const int b = r * LBL_COLS + c;
            if (g_lbl_occ[b >> 3] & (1u << (b & 7))) return false;
        }
    for (int r = r0; r <= r1; r++)
        for (int c = c0; c <= c1; c++) {
            const int b = r * LBL_COLS + c;
            g_lbl_occ[b >> 3] |= (uint8_t)(1u << (b & 7));
        }
    return true;
}

// Text with a 1px halo, so names stay readable over whatever they land on
// without needing an opaque plate behind every one of them.
static void lbl_draw(int x, int y, const char *s, uint8_t size,
                     uint16_t fg, uint16_t halo) {
    gfx->setTextSize(size);
    gfx->setTextColor(halo);
    for (int dy = -1; dy <= 1; dy++)
        for (int dx = -1; dx <= 1; dx++) {
            if (!dx && !dy) continue;
            gfx->setCursor(x + dx, y + dy);
            gfx->print(s);
        }
    gfx->setTextColor(fg);
    gfx->setCursor(x, y);
    gfx->print(s);
}

static void render_labels() {
    if (!g_pois) return;
    const ThemePalette &t = TH();
    const uint16_t halo = (wp_luma565(t.text) > 128) ? 0x0000 : 0xFFFF;
    memset(g_lbl_occ, 0, sizeof(g_lbl_occ));

    int placed = 0;
    // Rank 0 first (cities/towns) down to rank 4: everything below that is a dot
    // only, because a shopping centre's individual tenancies are pure noise.
    for (int want = 0; want <= 4 && placed < LBL_MAX; want++) {
        for (uint32_t i = 0; i < g_poi_count && placed < LBL_MAX; i++) {
            const FMapPOI &p = g_pois[i];
            if (p.rank != (uint8_t)want) continue;
            if (!(g_layer_mask & (1u << p.layer))) continue;
            if (g_cam_zoom_eff < (float)p.zmin) continue;
            const int len = (int)strlen(p.name);
            if (len == 0) continue;
            int sx, sy;
            world_to_screen(p.x, p.y, &sx, &sy);
            if (sx < -40 || sy < -12 || sx > LCD_WIDTH + 40 || sy > LCD_HEIGHT + 12) continue;

            const uint8_t size = (p.rank <= 1) ? 2 : 1;
            const int wpx = len * 6 * size;
            const int hpx = 8 * size;
            int tx = sx + 8;
            if (tx + wpx > LCD_WIDTH - 2) tx = sx - 8 - wpx;   // flip to the left
            const int ty = sy - hpx / 2;
            if (tx < 2 || ty < 2) continue;
            if (!lbl_claim(tx - 1, ty - 1, tx + wpx + 1, ty + hpx + 1)) continue;
            lbl_draw(tx, ty, p.name, size, t.text, halo);
            placed++;
        }
    }
}

static void render_saved() {
    if (!(g_layer_mask & (1u << LAY_SAVED))) return;
    for (int i=0; i<g_saved_count; i++) {
        int sx, sy;
        world_to_screen(g_saved[i].x, g_saved[i].y, &sx, &sy);
        if (sx < 0 || sy < 0 || sx >= LCD_WIDTH || sy >= LCD_HEIGHT) continue;
        gfx->fillCircle(sx, sy, 5, TH().saved);
        gfx->drawCircle(sx, sy, 7, TH().text);
        // pin stem
        gfx->drawLine(sx, sy, sx, sy + 8, TH().saved);
    }
}

static void render_compass_overlay() {
    if (g_heading < 0) return;
    const ThemePalette &t = TH();
    int cx = LCD_WIDTH - 40, cy = 44;
    gfx->fillCircle(cx, cy, 22, t.chrome_bg);
    gfx->drawCircle(cx, cy, 22, t.chrome_border);
    // North arrow, rotated inverse to heading (map is north-up if not rotate_with_heading)
    float a = -g_heading * (float)M_PI / 180.0f;
    if (g_rotate_with_heading) a = 0; // needle points up
    int nx = cx + (int)(16 * sinf(a));
    int ny = cy - (int)(16 * cosf(a));
    int sx1= cx - (int)(6  * sinf(a));
    int sy1= cy + (int)(6  * cosf(a));
    gfx->fillTriangle(cx - 4, cy + 2, cx + 4, cy + 2, nx, ny, 0xF800);
    gfx->fillTriangle(cx - 4, cy + 2, cx + 4, cy + 2, sx1, sy1, t.text_dim);
    gfx->setTextColor(t.text); gfx->setTextSize(1);
    gfx->setCursor(cx - 3, cy + 24); gfx->print("N");
}

static void render_bearing_arrow() {
    const ThemePalette &t = TH();
    // Priority: nav target > marked heading
    float rel;
    bool has_target_dist = false;
    float target_dist_m = 0;

    if (g_has_nav_target && g_meta.units_per_metre > 0) {
        double dx = (double)g_nav_target_x - g_cam_x;
        double dy = (double)g_nav_target_y - g_cam_y;
        // bearing: 0 = north (+Y), 90 = east (+X); world +Y is up
        float target_bearing = atan2f((float)dx, (float)dy) * 180.0f / (float)M_PI;
        if (target_bearing < 0) target_bearing += 360.0f;
        rel = target_bearing - (g_heading >= 0 ? g_heading : 0);
        target_dist_m = sqrtf((float)(dx*dx + dy*dy)) / g_meta.units_per_metre;
        has_target_dist = true;
    } else if (g_marked_heading >= 0 && g_heading >= 0) {
        rel = g_marked_heading - g_heading;
    } else {
        return;
    }
    while (rel > 180) rel -= 360;
    while (rel < -180) rel += 360;
    float r = rel * (float)M_PI / 180.0f;
    int cx = 40, cy = 44;
    gfx->fillCircle(cx, cy, 22, t.chrome_bg);
    gfx->drawCircle(cx, cy, 22, t.chrome_border);
    int nx = cx + (int)(16 * sinf(r));
    int ny = cy - (int)(16 * cosf(r));
    uint16_t col = (fabsf(rel) < 5) ? t.warn : t.accent;
    gfx->fillTriangle(cx - 4, cy + 2, cx + 4, cy + 2, nx, ny, col);
    gfx->setTextColor(col); gfx->setTextSize(1);
    gfx->setCursor(cx - 12, cy + 24);
    gfx->print(g_has_nav_target ? "NAV" : "MARK");

    // Distance chip to the right of the bearing puck
    if (has_target_dist) {
        char db[16];
        if (target_dist_m < 1000) snprintf(db, sizeof(db), "%d m", (int)(target_dist_m + 0.5f));
        else snprintf(db, sizeof(db), "%.1f km", target_dist_m / 1000.0f);
        int cw = (int)strlen(db) * 6 + 12;
        int cx2 = cx + 30, cy2 = cy - 10;
        gfx->fillRoundRect(cx2, cy2, cw, 20, 4, t.chrome_bg);
        gfx->drawRoundRect(cx2, cy2, cw, 20, 4, t.chrome_border);
        gfx->setTextColor(t.warn); gfx->setTextSize(1);
        gfx->setCursor(cx2 + 6, cy2 + 6);
        gfx->print(db);
    }
}

static void render_sun_overlay() {
    float az, el;
    sun_position(&az, &el);
    if (az < 0 || g_heading < 0) return;
    // Position relative to heading: place around a small ring at top-centre
    float rel = az - g_heading;
    while (rel > 180) rel -= 360;
    while (rel < -180) rel += 360;
    float r = rel * (float)M_PI / 180.0f;
    int cx = LCD_WIDTH/2, cy = 40;
    int radius = 80;
    int sx = cx + (int)(radius * sinf(r));
    int sy = cy - (int)(radius * cosf(r) * 0.5f); // squish vertically
    gfx->fillCircle(sx, sy, 6, TH().warn);
    gfx->drawCircle(sx, sy, 8, TH().text);
    // rays
    for (int i=0; i<8; i++) {
        float ra = i * (float)M_PI / 4.0f;
        int rx1 = sx + (int)(9  * cosf(ra));
        int ry1 = sy + (int)(9  * sinf(ra));
        int rx2 = sx + (int)(13 * cosf(ra));
        int ry2 = sy + (int)(13 * sinf(ra));
        gfx->drawLine(rx1, ry1, rx2, ry2, TH().warn);
    }
}

// -----------------------------------------------------------------------------
//  Map chrome: a status strip along the top, a hint strip along the bottom.
//
//  The five corner buttons that used to live here are gone. On a 410 px panel
//  they covered the map and every one of them had to be hit exactly; their five
//  actions are gestures now (see map_gesture()), and the corners carry what you
//  actually glance at instead: where you are, and - for the first few seconds -
//  which way to swipe.
//
//  The one tappable piece of chrome is the compass rose: tap it to lock the map
//  to heading-up, tap again for north-up.
// -----------------------------------------------------------------------------
#define WP_TOP_STRIP_H   24
#define WP_COMPASS_W     26
#define WP_HINT_FULL_MS 5000      // hints stay at full strength this long
#define WP_HINT_FADE_MS  900      // then fade out in three steps

static uint8_t  g_map_hint_alpha   = 0;      // 3 = full ... 0 = hidden
static uint32_t g_map_hint_next_ms = 0;

static void wp_map_hint_show(uint32_t now) {
    g_map_hint_alpha   = 3;
    g_map_hint_next_ms = now + WP_HINT_FULL_MS;
    g_need_map_redraw  = true;
}

static void wp_map_hint_tick(uint32_t now) {
    if (g_map_hint_alpha == 0 || now < g_map_hint_next_ms) return;
    g_map_hint_alpha--;
    g_map_hint_next_ms = now + (WP_HINT_FADE_MS / 3);
    g_need_map_redraw  = true;
}

static void wp_compass_rect(int *x, int *y, int *w, int *h) {
    *w = WP_COMPASS_W;
    *h = WP_TOP_STRIP_H - 2;
    *x = LCD_WIDTH - WP_COMPASS_W - 8;
    *y = 3;
}

static void wp_map_hint_rect(int *x, int *y, int *w, int *h) {
    // Sits just above the scale bar, and lines up with the bottom edge-swipe
    // zone: the thing that says "swipe up for tools" is the thing you swipe up
    // from.
    *x = 6; *y = LCD_HEIGHT - 74; *w = LCD_WIDTH - 12; *h = 36;
}

// Returns true when a tap landed on chrome rather than on the map, so the map
// does not also read it as a POI selection.
static bool map_chrome_tap(int32_t x, int32_t y, uint32_t now) {
    int cx, cy, cw, ch;
    wp_compass_rect(&cx, &cy, &cw, &ch);
    if (x >= cx && x < cx + cw && y >= cy && y < cy + ch) {
        g_rotate_with_heading = !g_rotate_with_heading;
        if (!g_rotate_with_heading) g_map_rotation_deg = 0.0f;
        g_need_map_redraw = true;
        return true;
    }
    int hx, hy, hw, hh;
    wp_map_hint_rect(&hx, &hy, &hw, &hh);
    if (x >= hx && x < hx + hw && y >= hy && y < hy + hh) {
        wp_map_hint_show(now);                 // the hint strip brings itself back
        return true;
    }
    return (y < WP_TOP_STRIP_H);               // the status strip swallows taps
}

// Scale bar (bottom-centre): shows a real-world reference distance
static void draw_scale_bar() {
    if (g_meta.units_per_metre <= 0 || g_cam_scale <= 0) return;
    const ThemePalette &t = TH();
    // metres per pixel = 1 / (cam_scale * units_per_metre)
    float m_per_px = 1.0f / (g_cam_scale * g_meta.units_per_metre);
    if (!isfinite(m_per_px) || m_per_px <= 0) return;
    // Choose a "nice" length: 1,2,5 x 10^n, big enough to read. The list has
    // to reach 500 km, because now that a map can cover a whole state the old
    // 10 km ceiling made the bar a two-pixel stub whenever you zoomed out.
    static const float nice[] = { 1,2,5,10,20,50,100,200,500,
                                  1000,2000,5000,10000,20000,50000,
                                  100000,200000,500000 };
    float chosen = nice[0];
    float target_px = nice[0] / m_per_px;
    for (unsigned i = 0; i < sizeof(nice)/sizeof(nice[0]); i++) {
        float px = nice[i] / m_per_px;
        if (px < 40.0f) continue;                  // too short to read
        chosen = nice[i];
        target_px = px;
        break;
    }
    if (target_px > 200.0f) target_px = 200.0f;    // never wider than the screen
    int bar_px = (int)target_px;
    int cx = LCD_WIDTH/2;
    int by = LCD_HEIGHT - 20;
    gfx->drawLine(cx - bar_px/2, by, cx + bar_px/2, by, t.text);
    gfx->drawLine(cx - bar_px/2, by-4, cx - bar_px/2, by+4, t.text);
    gfx->drawLine(cx + bar_px/2, by-4, cx + bar_px/2, by+4, t.text);
    char lbl[16];
    if (chosen < 1000) snprintf(lbl, sizeof(lbl), "%d m", (int)chosen);
    else               snprintf(lbl, sizeof(lbl), "%d km", (int)(chosen/1000));
    int lw = (int)strlen(lbl) * 6;
    gfx->setTextColor(t.text); gfx->setTextSize(1);
    gfx->setCursor(cx - lw/2, by - 14);
    gfx->print(lbl);
}

// Render beacons layer (from CSV) if enabled.
static void render_beacons() {
    if (!(g_layer_mask & (1u << LAY_BEACONS))) return;
    const ThemePalette &t = TH();
    for (int i=0; i<g_beacon_count; i++) {
        int sx, sy;
        world_to_screen(g_beacons[i].x, g_beacons[i].y, &sx, &sy);
        if (sx < -8 || sy < -8 || sx > LCD_WIDTH+8 || sy > LCD_HEIGHT+8) continue;
        uint16_t c = g_beacons[i].online ? t.beacons : t.text_dim;
        // Draw a diamond marker
        gfx->fillTriangle(sx, sy-6, sx-6, sy, sx+6, sy, c);
        gfx->fillTriangle(sx, sy+6, sx-6, sy, sx+6, sy, c);
        gfx->drawCircle(sx, sy, 8, t.text);
    }
}

static void draw_overlay_chrome() {
    const ThemePalette &t = TH();
    char b[80];

    // ---- status strip ----------------------------------------------------
    gfx->fillRoundRect(4, 2, LCD_WIDTH - 8, WP_TOP_STRIP_H, 6, t.chrome_bg);
    gfx->drawRoundRect(4, 2, LCD_WIDTH - 8, WP_TOP_STRIP_H, 6, t.chrome_border);
    const int ty = 2 + (WP_TOP_STRIP_H - 8) / 2;

    String name = g_meta.title.length() ? g_meta.title : String("map");
    if (name.length() > 14) name = name.substring(0, 13) + "~";
    gfx->setTextColor(t.accent); gfx->setTextSize(1);
    gfx->setCursor(12, ty);
    gfx->print(name);

    // Effective (fractional) zoom: the real scale signal, continuous and still
    // correct when the tile on screen is an overzoomed parent.
    snprintf(b, sizeof(b), "z%.1f", (double)g_cam_zoom_eff);
    int zx = 12 + (int)name.length() * 6 + 12;
    gfx->setTextColor(t.text);
    gfx->setCursor(zx, ty);
    gfx->print(b);

    // Tile health: amber the moment any tile in the visible ring fails to
    // decode, which is the earliest sign that the tile set and the camera
    // disagree - a missing zoom level, a partial copy, an unknown schema.
    const bool tiles_ok = (g_hud_tiles_empty == 0 && g_hud_tiles_ok > 0);
    snprintf(b, sizeof(b), "%u/%u", (unsigned)g_hud_tiles_ok,
             (unsigned)(g_hud_tiles_ok + g_hud_tiles_empty));
    gfx->setTextColor(tiles_ok ? t.text_dim : t.warn);
    gfx->setCursor(zx + 46, ty);
    gfx->print(b);

    // ---- compass rose: tap to lock heading-up ----------------------------
    {
        int cx, cy, cw, ch;
        wp_compass_rect(&cx, &cy, &cw, &ch);
        const int ccx = cx + cw / 2, ccy = cy + ch / 2;
        const int r   = (ch / 2) - 1;
        const uint16_t col = g_rotate_with_heading ? t.warn : t.accent;
        gfx->drawCircle(ccx, ccy, r, t.chrome_border);
        const float rr = g_rotate_with_heading
                       ? (g_map_rotation_deg * (float)M_PI / 180.0f) : 0.0f;
        const int nx = ccx + (int)(-sinf(rr) * (float)(r - 3));
        const int ny = ccy - (int)( cosf(rr) * (float)(r - 3));
        gfx->fillTriangle(ccx - 3, ccy + 1, ccx + 3, ccy + 1, nx, ny, col);
    }

    // ---- crosshair: the point the camera is centred on -------------------
    {
        const int ccx = LCD_WIDTH / 2, ccy = LCD_HEIGHT / 2;
        gfx->drawLine(ccx - 10, ccy, ccx + 10, ccy, t.accent);
        gfx->drawLine(ccx, ccy - 10, ccx, ccy + 10, t.accent);
        gfx->drawCircle(ccx, ccy, 4, t.accent);
    }

    draw_scale_bar();

    // ---- gesture hints, fading out so the map is not permanently covered --
    if (g_map_hint_alpha) {
        int hx, hy, hw, hh;
        wp_map_hint_rect(&hx, &hy, &hw, &hh);
        const uint16_t hcol = (g_map_hint_alpha >= 3) ? t.text
                            : (g_map_hint_alpha == 2)  ? t.text_dim
                                                       : t.chrome_border;
        gfx->fillRoundRect(hx, hy, hw, hh, 6, t.chrome_bg);
        gfx->drawRoundRect(hx, hy, hw, hh, 6, t.chrome_border);
        gfx->setTextColor(hcol); gfx->setTextSize(1);
        gfx->setCursor(hx + 8, hy + 5);
        gfx->print("swipe ^ tools       swipe v library");
        gfx->setCursor(hx + 8, hy + 19);
        gfx->print("tap poi   hold tools   2f tap: zoom out");
    }
}

static void draw_map_frame() {
    if (!g_need_map_redraw) return;
    g_need_map_redraw = false;
    uint32_t t0 = micros();

    const ThemePalette &t = TH();

    // Effective (fractional) zoom. This is the real scale signal: continuous,
    // independent of which tile zoom we happen to be sampling, and therefore
    // still correct for overzoomed parent tiles.
    if (g_cam_scale > 0) {
        const double upp = 1.0 / (double)g_cam_scale;    // world units per pixel
        g_cam_zoom_eff = (float)((double)g_meta.refzoom -
                         log2(upp * 256.0 / (double)MVT_EXTENT_DEFAULT));
    }

    // Tile column/row are int32_t. At z17 the Perth column is ~107 000; the old
    // int16_t wrapped it negative, so the entire ring resolved to nothing.
    const int8_t  z     = (int8_t)g_cam_zoom;
    const int     shift = g_meta.refzoom - z;
    const double  side  = (double)MVT_EXTENT_DEFAULT *
                          ((shift >= 0) ? (double)(1u << shift)
                                        : 1.0 / (double)(1u << -shift));
    // world_x -> tile x  (world x grows east, tile x grows east)
    const int32_t ctx = (int32_t)floor(g_cam_x / side);
    // world_y is +Y = north; tile y grows south -> negate before dividing
    const int32_t cty = (int32_t)floor(-g_cam_y / side);
    ensure_ring_loaded(z, ctx, cty);

    // Collect the visible ring (overzoom fallback + parent dedup so a parent
    // tile that covers several cells is only rendered once).
    g_hud_tiles_ok = 0;
    g_hud_tiles_empty = 0;
    FMapTile *ring[9];
    int nring = 0;
    for (int j=-1; j<=1; j++) {
        for (int i=-1; i<=1; i++) {
            FMapTile *tl = tile_resolve(z, ctx + i, cty + j);
            if (!tl) { g_hud_tiles_empty++; continue; }
            bool seen = false;
            for (int k=0; k<nring; k++) if (ring[k] == tl) { seen = true; break; }
            if (seen) continue;
            if (nring < 9) ring[nring++] = tl;
            if (tl->obj_count > 0) g_hud_tiles_ok++;
            else                   g_hud_tiles_empty++;
        }
    }

    // Paint the whole ring layer by layer.
    gfx->fillScreen(t.bg);
    g_render_obj_count = 0;
    draw_layers(ring, nring);

    // Warm the next ring out so panning stays smooth.
    prefetch_ring(z, ctx, cty);

    render_saved();
    render_beacons();
    render_pois();
    render_labels();

    // Nav target crosshair (bright, larger than a normal saved pin)
    if (g_has_nav_target) {
        int sx, sy;
        world_to_screen(g_nav_target_x, g_nav_target_y, &sx, &sy);
        if (sx > -12 && sy > -12 && sx < LCD_WIDTH+12 && sy < LCD_HEIGHT+12) {
            gfx->drawCircle(sx, sy, 12, TH().warn);
            gfx->drawCircle(sx, sy, 8,  TH().warn);
            gfx->drawLine(sx-14, sy, sx+14, sy, TH().warn);
            gfx->drawLine(sx, sy-14, sx, sy+14, TH().warn);
        }
    }

    draw_overlay_chrome();
    render_compass_overlay();
    render_bearing_arrow();
    render_sun_overlay();

    g_render_time_us = micros() - t0;
    g_render_last_ms = millis();
    (void)t;
}

// =============================================================================
//  App state machine + LVGL screens (Compass / Level / Sun / Menus)
// =============================================================================
enum AppMode {
    MODE_SPLASH, MODE_LIBRARY, MODE_MAP, MODE_TOOLS, MODE_COMPASS,
    MODE_LEVEL, MODE_SUN, MODE_BEARINGS, MODE_LAYERS, MODE_THEMES,
    MODE_SAVEDIALOG, MODE_SEARCH, MODE_DEV, MODE_POI_CARD,
    MODE_BLOCKNAV, MODE_EXPLORE
};
static AppMode g_mode = MODE_SPLASH;
static AppMode g_prev_mode = MODE_LIBRARY;

// LVGL roots (built lazily)
static lv_obj_t *scr_tools=nullptr, *scr_compass=nullptr, *scr_level=nullptr,
                *scr_sun=nullptr, *scr_bearings=nullptr, *scr_layers=nullptr,
                *scr_themes=nullptr, *scr_savedlg=nullptr, *scr_search=nullptr,
                *scr_dev=nullptr, *scr_poicard=nullptr;
static lv_obj_t *lbl_hdg=nullptr, *lbl_card=nullptr;
static lv_obj_t *level_bubble=nullptr, *lbl_level=nullptr;
static lv_obj_t *lbl_sun=nullptr, *lbl_bearlist=nullptr, *lbl_dev=nullptr;
static lv_obj_t *lbl_poi_title=nullptr, *lbl_poi_body=nullptr;

// A blank LVGL screen used to "hide" LVGL when we are in raw-gfx modes
static lv_obj_t *scr_blank = nullptr;

#define COL_BG 0x000000
#define COL_TXT 0xFFFFFF
#define COL_MUTE 0x666666
#define COL_ACCENT 0x00E5FF
#define COL_GOLD 0xFFB300

static void enter_mode(AppMode m);

static void add_back_pill(lv_obj_t *p, const char *label, lv_event_cb_t cb) {
    lv_obj_t *back = lv_btn_create(p);
    lv_obj_set_size(back, 220, 42);
    lv_obj_align(back, LV_ALIGN_BOTTOM_MID, 0, -16);
    lv_obj_set_style_bg_color(back, lv_color_hex(0x141414), 0);
    lv_obj_set_style_border_color(back, lv_color_hex(COL_ACCENT), 0);
    lv_obj_set_style_border_width(back, 1, 0);
    lv_obj_set_style_radius(back, 22, 0);
    lv_obj_t *l = lv_label_create(back);
    lv_label_set_text_fmt(l, LV_SYMBOL_LEFT "  %s", label);
    lv_obj_set_style_text_color(l, lv_color_hex(COL_ACCENT), 0);
    lv_obj_set_style_text_font(l, &lv_font_montserrat_14, 0);
    lv_obj_center(l);
    lv_obj_add_event_cb(back, cb, LV_EVENT_CLICKED, NULL);
}
static void cb_back_to_map(lv_event_t*) { enter_mode(MODE_MAP); }
static void cb_back_to_tools(lv_event_t*) { enter_mode(MODE_TOOLS); }

static void build_blank() {
    if (scr_blank) return;
    scr_blank = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr_blank, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(scr_blank, LV_OPA_COVER, 0);
    lv_obj_clear_flag(scr_blank, LV_OBJ_FLAG_SCROLLABLE);
}

static void build_compass() {
    if (scr_compass) return;
    scr_compass = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr_compass, lv_color_hex(COL_BG), 0);
    lv_obj_set_style_bg_opa(scr_compass, LV_OPA_COVER, 0);

    lv_obj_t *t = lv_label_create(scr_compass);
    lv_label_set_text(t, "COMPASS");
    lv_obj_set_style_text_color(t, lv_color_hex(COL_MUTE),0);
    lv_obj_set_style_text_font(t, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_letter_space(t,3,0);
    lv_obj_align(t, LV_ALIGN_TOP_MID, 0, 24);

    lbl_hdg = lv_label_create(scr_compass);
    lv_label_set_text(lbl_hdg, "---°");
    lv_obj_set_style_text_color(lbl_hdg, lv_color_hex(COL_ACCENT), 0);
    lv_obj_set_style_text_font(lbl_hdg, &lv_font_montserrat_48, 0);
    lv_obj_align(lbl_hdg, LV_ALIGN_CENTER, 0, -30);

    lbl_card = lv_label_create(scr_compass);
    lv_label_set_text(lbl_card, "-");
    lv_obj_set_style_text_color(lbl_card, lv_color_hex(COL_TXT), 0);
    lv_obj_set_style_text_font(lbl_card, &lv_font_montserrat_28, 0);
    lv_obj_align(lbl_card, LV_ALIGN_CENTER, 0, 30);

    lv_obj_t *mb = lv_btn_create(scr_compass);
    lv_obj_set_size(mb, 120, 40);
    lv_obj_align(mb, LV_ALIGN_BOTTOM_MID, 0, -70);
    lv_obj_set_style_bg_color(mb, lv_color_hex(COL_GOLD), 0);
    lv_obj_set_style_radius(mb, 20, 0);
    lv_obj_t *ml = lv_label_create(mb); lv_label_set_text(ml, "MARK");
    lv_obj_set_style_text_color(ml, lv_color_hex(0x000000),0);
    lv_obj_set_style_text_font(ml, &lv_font_montserrat_16, 0);
    lv_obj_center(ml);
    lv_obj_add_event_cb(mb, [](lv_event_t *e){
        (void)e;
        g_marked_heading = tilt_comp_heading();
        Preferences p; p.begin("waypoint", false); p.putFloat("mark_hdg", g_marked_heading); p.end();
        File f = SD_MMC.open("/WATCH/WayPoint/bearings.txt", FILE_APPEND);
        if (f) { time_t t; time(&t); char b[48]; snprintf(b,48,"%lu,%.1f\n", (unsigned long)t, g_marked_heading); f.print(b); f.close(); }
    }, LV_EVENT_CLICKED, NULL);
    // Long-press to start calibration
    lv_obj_add_event_cb(mb, [](lv_event_t *e){ (void)e; mag_cal_start(); }, LV_EVENT_LONG_PRESSED, NULL);

    add_back_pill(scr_compass, "Back to Map", cb_back_to_map);
}
static void build_level() {
    if (scr_level) return;
    scr_level = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr_level, lv_color_hex(COL_BG), 0);
    lv_obj_set_style_bg_opa(scr_level, LV_OPA_COVER, 0);

    lv_obj_t *t = lv_label_create(scr_level);
    lv_label_set_text(t, "LEVEL");
    lv_obj_set_style_text_color(t, lv_color_hex(COL_MUTE),0);
    lv_obj_set_style_text_font(t, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_letter_space(t,3,0);
    lv_obj_align(t, LV_ALIGN_TOP_MID, 0, 24);

    lv_obj_t *box = lv_obj_create(scr_level);
    lv_obj_set_size(box, 280, 280);
    lv_obj_align(box, LV_ALIGN_CENTER, 0, -20);
    lv_obj_set_style_bg_color(box, lv_color_hex(0x080808), 0);
    lv_obj_set_style_border_color(box, lv_color_hex(0x222222), 0);
    lv_obj_set_style_border_width(box, 1, 0);
    lv_obj_set_style_radius(box, 140, 0);

    level_bubble = lv_obj_create(box);
    lv_obj_set_size(level_bubble, 36, 36);
    lv_obj_align(level_bubble, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(level_bubble, lv_color_hex(COL_GOLD), 0);
    lv_obj_set_style_radius(level_bubble, 18, 0);
    lv_obj_set_style_border_width(level_bubble, 0, 0);

    lbl_level = lv_label_create(scr_level);
    lv_label_set_text(lbl_level, "0.0°  0.0°");
    lv_obj_set_style_text_color(lbl_level, lv_color_hex(COL_ACCENT), 0);
    lv_obj_set_style_text_font(lbl_level, &lv_font_montserrat_16, 0);
    lv_obj_align(lbl_level, LV_ALIGN_BOTTOM_MID, 0, -78);

    add_back_pill(scr_level, "Back to Map", cb_back_to_map);
}
static void build_sun() {
    if (scr_sun) return;
    scr_sun = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr_sun, lv_color_hex(COL_BG), 0);
    lv_obj_set_style_bg_opa(scr_sun, LV_OPA_COVER, 0);
    lv_obj_t *t = lv_label_create(scr_sun);
    lv_label_set_text(t, "SUN POSITION");
    lv_obj_set_style_text_color(t, lv_color_hex(COL_MUTE),0);
    lv_obj_set_style_text_font(t, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_letter_space(t,3,0);
    lv_obj_align(t, LV_ALIGN_TOP_MID, 0, 24);
    lbl_sun = lv_label_create(scr_sun);
    lv_label_set_text(lbl_sun, "az: ---°\nel: ---°");
    lv_obj_set_style_text_color(lbl_sun, lv_color_hex(COL_GOLD), 0);
    lv_obj_set_style_text_font(lbl_sun, &lv_font_montserrat_22, 0);
    lv_obj_align(lbl_sun, LV_ALIGN_CENTER, 0, -20);
    lv_obj_t *hint = lv_label_create(scr_sun);
    lv_label_set_text(hint, "set lat/lon in /WATCH/WayPoint/loc.txt");
    lv_obj_set_style_text_color(hint, lv_color_hex(COL_MUTE), 0);
    lv_obj_set_style_text_font(hint, &lv_font_montserrat_12, 0);
    lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -68);
    add_back_pill(scr_sun, "Back to Map", cb_back_to_map);
}
static void build_bearings() {
    if (scr_bearings) return;
    scr_bearings = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr_bearings, lv_color_hex(COL_BG), 0);
    lv_obj_set_style_bg_opa(scr_bearings, LV_OPA_COVER, 0);
    lv_obj_t *t = lv_label_create(scr_bearings);
    lv_label_set_text(t, "BEARINGS");
    lv_obj_set_style_text_color(t, lv_color_hex(COL_MUTE),0);
    lv_obj_set_style_text_font(t, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_letter_space(t,3,0);
    lv_obj_align(t, LV_ALIGN_TOP_MID, 0, 24);
    lbl_bearlist = lv_label_create(scr_bearings);
    lv_label_set_text(lbl_bearlist, "(none)");
    lv_obj_set_style_text_color(lbl_bearlist, lv_color_hex(COL_TXT), 0);
    lv_obj_set_style_text_font(lbl_bearlist, &lv_font_montserrat_14, 0);
    lv_obj_align(lbl_bearlist, LV_ALIGN_TOP_MID, 0, 60);
    lv_label_set_long_mode(lbl_bearlist, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(lbl_bearlist, 360);
    add_back_pill(scr_bearings, "Back to Map", cb_back_to_map);
}

static void bearings_refresh() {
    if (!lbl_bearlist) return;
    File f = SD_MMC.open("/WATCH/WayPoint/bearings.txt", FILE_READ);
    if (!f) { lv_label_set_text(lbl_bearlist, "(none)"); return; }
    String s; int count = 0;
    while (f.available() && count < 20) {
        String ln = f.readStringUntil('\n');
        if (ln.length() < 3) continue;
        int c = ln.indexOf(',');
        if (c < 0) continue;
        float h = ln.substring(c+1).toFloat();
        char buf[48]; snprintf(buf, 48, "#%d  %d°  %s\n", count+1, (int)(h+0.5f), card_for(h));
        s += buf; count++;
    }
    f.close();
    if (!count) s = "(none)";
    lv_label_set_text(lbl_bearlist, s.c_str());
}

// ----- Layers menu -----
// Stable NVS key for one map's layer mask. The old code truncated the map
// title to 15 characters, so two per-map entries sharing a long prefix
// ("Byford" and "Byford North", or two exports from the same city) collided on
// the SAME key and silently inherited each other's layer toggles. Hashing the
// whole title keeps the key short, deterministic and collision-free in
// practice for the handful of maps a watch holds.
//
// The prefix is versioned. Adding LAY_EARTH and LAY_LANDCOVER moved every
// later bit position, so a mask saved under the old prefix would light up the
// wrong layers: bumping to "ln_" makes every map start with everything on
// instead, which is the honest default when the layout changed.
static void wp_map_key(const String &title, char *out, size_t cap) {
    uint32_t h = 2166136261u;                 // FNV-1a
    for (size_t i = 0; i < title.length(); i++) {
        h ^= (uint8_t)title[i];
        h *= 16777619u;
    }
    snprintf(out, cap, "ln_%08lx", (unsigned long)h);
}

static void layers_toggle(uint8_t layer) {
    g_layer_mask ^= (1u << layer);
    Preferences p; p.begin("waypoint", false);
    char key[16]; wp_map_key(g_meta.title, key, sizeof(key));
    p.putUInt(key, g_layer_mask);
    p.end();
    g_need_map_redraw = true;
}
static void build_layers() {
    if (scr_layers) return;
    scr_layers = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr_layers, lv_color_hex(COL_BG), 0);
    lv_obj_set_style_bg_opa(scr_layers, LV_OPA_COVER, 0);

    lv_obj_t *t = lv_label_create(scr_layers);
    lv_label_set_text(t, "LAYERS");
    lv_obj_set_style_text_color(t, lv_color_hex(COL_MUTE),0);
    lv_obj_set_style_text_font(t, &lv_font_montserrat_14, 0);
    lv_obj_align(t, LV_ALIGN_TOP_MID, 0, 24);

    lv_obj_t *list = lv_obj_create(scr_layers);
    lv_obj_set_size(list, LCD_WIDTH - 40, LCD_HEIGHT - 150);
    lv_obj_align(list, LV_ALIGN_TOP_MID, 0, 60);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_bg_opa(list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(list, 0, 0);

    for (int i=0; i<LAY__COUNT; i++) {
        lv_obj_t *sw_row = lv_obj_create(list);
        lv_obj_set_size(sw_row, lv_pct(100), 44);
        lv_obj_set_style_bg_color(sw_row, lv_color_hex(0x141414), 0);
        lv_obj_set_style_border_width(sw_row, 0, 0);
        lv_obj_set_style_radius(sw_row, 8, 0);
        lv_obj_set_flex_flow(sw_row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(sw_row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

        lv_obj_t *lb = lv_label_create(sw_row);
        lv_label_set_text(lb, LAYER_NAMES[i]);
        lv_obj_set_style_text_color(lb, lv_color_hex(COL_TXT), 0);

        lv_obj_t *sw = lv_switch_create(sw_row);
        if (g_layer_mask & (1u << i)) lv_obj_add_state(sw, LV_STATE_CHECKED);
        int *layer_id = (int*)lv_mem_alloc(sizeof(int));
        *layer_id = i;
        lv_obj_add_event_cb(sw, [](lv_event_t *e){
            int *idp = (int*)lv_event_get_user_data(e);
            layers_toggle((uint8_t)*idp);
        }, LV_EVENT_VALUE_CHANGED, layer_id);
    }
    add_back_pill(scr_layers, "Back to Tools", cb_back_to_tools);
}

// ----- Themes menu -----
static void set_theme(uint8_t id) {
    g_theme_id = id;
    Preferences p; p.begin("waypoint", false); p.putUChar("theme", id); p.end();
    g_lib_redraw = true; g_need_map_redraw = true;
}
static lv_obj_t *g_theme_rows[4] = {nullptr,nullptr,nullptr,nullptr};
static lv_obj_t *g_theme_ticks[4] = {nullptr,nullptr,nullptr,nullptr};

static void themes_refresh() {
    for (int i=0; i<4; i++) {
        if (g_theme_ticks[i])
            lv_label_set_text(g_theme_ticks[i], (i == g_theme_id) ? LV_SYMBOL_OK : "");
        if (g_theme_rows[i]) {
            if (i == g_theme_id) lv_obj_add_state(g_theme_rows[i], LV_STATE_CHECKED);
            else                 lv_obj_clear_state(g_theme_rows[i], LV_STATE_CHECKED);
        }
    }
}
static void cb_theme_pick(lv_event_t *e) {
    int *idp = (int*)lv_event_get_user_data(e);
    set_theme((uint8_t)*idp);
    wp_sync_styles();
    themes_refresh();
    enter_mode(MODE_MAP);
}
static void build_themes() {
    if (scr_themes) { themes_refresh(); return; }
    wp_sync_styles();
    scr_themes = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr_themes, lv_color_hex(COL_BG), 0);
    lv_obj_set_style_bg_opa(scr_themes, LV_OPA_COVER, 0);
    lv_obj_clear_flag(scr_themes, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *t = lv_label_create(scr_themes);
    lv_label_set_text(t, "THEMES");
    lv_obj_add_style(t, &wp_style_label_dim, 0);
    lv_obj_align(t, LV_ALIGN_TOP_MID, 0, 22);

    // Flex column of theme cards (Track B: shared card style + selected tick).
    lv_obj_t *list = lv_obj_create(scr_themes);
    lv_obj_set_size(list, LCD_WIDTH - 40, LCD_HEIGHT - 130);
    lv_obj_align(list, LV_ALIGN_TOP_MID, 0, 56);
    lv_obj_set_style_bg_opa(list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(list, 0, 0);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(list, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(list, 12, 0);

    for (int i=0; i<4; i++) {
        lv_obj_t *b = lv_btn_create(list);
        lv_obj_add_style(b, &wp_style_menu_card, 0);
        lv_obj_add_style(b, &wp_style_pressed, LV_STATE_PRESSED);
        lv_obj_set_size(b, lv_pct(100), 54);
        lv_obj_set_flex_flow(b, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(b, LV_FLEX_ALIGN_SPACE_BETWEEN,
                              LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        // swatch dot in this theme's accent colour
        lv_obj_t *dot = lv_obj_create(b);
        lv_obj_set_size(dot, 18, 18);
        lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_border_width(dot, 0, 0);
        lv_obj_set_style_bg_color(dot, wp_565(THEMES[i].accent), 0);
        lv_obj_clear_flag(dot, LV_OBJ_FLAG_SCROLLABLE);

        lv_obj_t *l = lv_label_create(b);
        lv_label_set_text(l, THEMES[i].name);
        lv_obj_add_style(l, &wp_style_label_title, 0);
        lv_obj_set_flex_grow(l, 1);
        lv_obj_set_style_pad_left(l, 12, 0);

        lv_obj_t *tick = lv_label_create(b);
        lv_label_set_text(tick, (i == g_theme_id) ? LV_SYMBOL_OK : "");
        lv_obj_set_style_text_color(tick, wp_565(THEMES[i].accent), 0);
        g_theme_ticks[i] = tick;
        g_theme_rows[i] = b;

        int *id = (int*)lv_mem_alloc(sizeof(int)); *id = i;
        lv_obj_add_event_cb(b, cb_theme_pick, LV_EVENT_CLICKED, id);
    }
    add_back_pill(scr_themes, "Back to Tools", cb_back_to_tools);
}

// ----- Save location dialog -----
static lv_obj_t *save_kb=nullptr, *save_ta=nullptr;
static void build_savedlg() {
    if (scr_savedlg) return;
    scr_savedlg = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr_savedlg, lv_color_hex(COL_BG), 0);
    lv_obj_set_style_bg_opa(scr_savedlg, LV_OPA_COVER, 0);
    lv_obj_t *t = lv_label_create(scr_savedlg);
    lv_label_set_text(t, "SAVE LOCATION");
    lv_obj_set_style_text_color(t, lv_color_hex(COL_MUTE),0);
    lv_obj_set_style_text_font(t, &lv_font_montserrat_14, 0);
    lv_obj_align(t, LV_ALIGN_TOP_MID, 0, 20);

    save_ta = lv_textarea_create(scr_savedlg);
    lv_obj_set_size(save_ta, 360, 40);
    lv_obj_align(save_ta, LV_ALIGN_TOP_MID, 0, 56);
    lv_textarea_set_one_line(save_ta, true);
    lv_textarea_set_placeholder_text(save_ta, "Location name");
    lv_obj_set_style_bg_color(save_ta, lv_color_hex(0x141414), 0);
    lv_obj_set_style_text_color(save_ta, lv_color_hex(COL_TXT), 0);

    lv_obj_t *ok = lv_btn_create(scr_savedlg);
    lv_obj_set_size(ok, 140, 42);
    lv_obj_align(ok, LV_ALIGN_TOP_LEFT, 30, 108);
    lv_obj_set_style_bg_color(ok, lv_color_hex(COL_GOLD), 0);
    lv_obj_set_style_radius(ok, 20, 0);
    lv_obj_t *okl = lv_label_create(ok); lv_label_set_text(okl, "Save");
    lv_obj_set_style_text_color(okl, lv_color_hex(0x000000), 0);
    lv_obj_center(okl);
    lv_obj_add_event_cb(ok, [](lv_event_t *e){
        (void)e;
        String nm;
#if LV_USE_KEYBOARD
        const char *n = save_ta ? lv_textarea_get_text(save_ta) : "";
        nm = String(n);
#endif
        if (nm.length() == 0) {
            time_t tt; time(&tt);
            char buf[32]; snprintf(buf, 32, "waypoint-%lu", (unsigned long)tt);
            nm = buf;
        }
        saved_add((int32_t)g_cam_x, (int32_t)g_cam_y, nm, 0);
        enter_mode(MODE_MAP);
    }, LV_EVENT_CLICKED, NULL);

#if LV_USE_KEYBOARD
    save_kb = lv_keyboard_create(scr_savedlg);
    lv_obj_set_size(save_kb, LCD_WIDTH, 210);
    lv_obj_align(save_kb, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_keyboard_set_textarea(save_kb, save_ta);
#else
    // No keyboard in this LVGL build — user just taps Save (auto-names).
    lv_obj_t *nk = lv_label_create(scr_savedlg);
    lv_label_set_text(nk,
        "On-screen keyboard disabled in lv_conf.h.\n"
        "Press Save to store as a timestamped waypoint.");
    lv_obj_set_style_text_color(nk, lv_color_hex(COL_MUTE), 0);
    lv_obj_set_style_text_font(nk, &lv_font_montserrat_14, 0);
    lv_obj_align(nk, LV_ALIGN_TOP_MID, 0, 170);
    lv_label_set_long_mode(nk, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(nk, 340);
    (void)save_kb;
#endif
}

// ----- Search -----
static lv_obj_t *search_ta=nullptr, *search_kb=nullptr, *search_results=nullptr;
static void build_search() {
    if (scr_search) return;
    scr_search = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr_search, lv_color_hex(COL_BG), 0);
    lv_obj_set_style_bg_opa(scr_search, LV_OPA_COVER, 0);
    lv_obj_t *t = lv_label_create(scr_search);
    lv_label_set_text(t, "SEARCH POIs");
    lv_obj_set_style_text_color(t, lv_color_hex(COL_MUTE),0);
    lv_obj_set_style_text_font(t, &lv_font_montserrat_14, 0);
    lv_obj_align(t, LV_ALIGN_TOP_MID, 0, 20);

    search_ta = lv_textarea_create(scr_search);
    lv_obj_set_size(search_ta, 360, 40);
    lv_obj_align(search_ta, LV_ALIGN_TOP_MID, 0, 52);
    lv_textarea_set_one_line(search_ta, true);
    lv_textarea_set_placeholder_text(search_ta, "Type to search");
    lv_obj_set_style_bg_color(search_ta, lv_color_hex(0x141414), 0);
    lv_obj_set_style_text_color(search_ta, lv_color_hex(COL_TXT), 0);

    search_results = lv_list_create(scr_search);
    lv_obj_set_size(search_results, 380, 200);
    lv_obj_align(search_results, LV_ALIGN_TOP_MID, 0, 104);
    lv_obj_set_style_bg_color(search_results, lv_color_hex(0x0a0a0a), 0);

    lv_obj_add_event_cb(search_ta, [](lv_event_t *e){
        (void)e;
        if (!search_results) return;
        lv_obj_clean(search_results);
        String q(lv_textarea_get_text(search_ta));
        q.toLowerCase();
        if (q.length() == 0) return;
        int shown = 0;
        for (uint32_t i=0; i<g_poi_count && shown < 20; i++) {
            if (!g_pois[i].name) continue;
            String nm(g_pois[i].name);
            String nmlow = nm; nmlow.toLowerCase();
            if (nmlow.indexOf(q) >= 0) {
                lv_obj_t *it = lv_list_add_btn(search_results, NULL, nm.c_str());
                lv_obj_set_style_text_color(it, lv_color_hex(COL_TXT), 0);
                lv_obj_set_style_bg_color(it, lv_color_hex(0x141414), 0);
                uint32_t *pi = (uint32_t*)lv_mem_alloc(sizeof(uint32_t));
                *pi = i;
                lv_obj_add_event_cb(it, [](lv_event_t *ev){
                    uint32_t *pip = (uint32_t*)lv_event_get_user_data(ev);
                    if (*pip < g_poi_count) {
                        g_cam_x = g_pois[*pip].x;
                        g_cam_y = g_pois[*pip].y;
                        g_need_map_redraw = true;
                        enter_mode(MODE_MAP);
                    }
                }, LV_EVENT_CLICKED, pi);
                shown++;
            }
        }
    }, LV_EVENT_VALUE_CHANGED, NULL);

#if LV_USE_KEYBOARD
    search_kb = lv_keyboard_create(scr_search);
    lv_obj_set_size(search_kb, LCD_WIDTH, 180);
    lv_obj_align(search_kb, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_keyboard_set_textarea(search_kb, search_ta);
#else
    // No keyboard — populate list with all POIs so user can browse instead.
    (void)search_kb;
    if (search_results) {
        for (uint32_t i=0; i<g_poi_count && i<40; i++) {
            if (!g_pois[i].name) continue;
            lv_obj_t *it = lv_list_add_btn(search_results, NULL, g_pois[i].name);
            lv_obj_set_style_text_color(it, lv_color_hex(COL_TXT), 0);
            lv_obj_set_style_bg_color(it, lv_color_hex(0x141414), 0);
            uint32_t *pi = (uint32_t*)lv_mem_alloc(sizeof(uint32_t));
            *pi = i;
            lv_obj_add_event_cb(it, [](lv_event_t *ev){
                uint32_t *pip = (uint32_t*)lv_event_get_user_data(ev);
                if (*pip < g_poi_count) {
                    g_cam_x = g_pois[*pip].x;
                    g_cam_y = g_pois[*pip].y;
                    g_need_map_redraw = true;
                    enter_mode(MODE_MAP);
                }
            }, LV_EVENT_CLICKED, pi);
        }
    }
#endif

    // Back button in top-right
    lv_obj_t *back = lv_btn_create(scr_search);
    lv_obj_set_size(back, 60, 32);
    lv_obj_align(back, LV_ALIGN_TOP_RIGHT, -10, 10);
    lv_obj_set_style_bg_color(back, lv_color_hex(0x141414), 0);
    lv_obj_t *bl = lv_label_create(back); lv_label_set_text(bl, LV_SYMBOL_LEFT); lv_obj_center(bl);
    lv_obj_add_event_cb(back, cb_back_to_map, LV_EVENT_CLICKED, NULL);
}

// ----- Dev mode -----
static void build_dev() {
    if (scr_dev) return;
    scr_dev = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr_dev, lv_color_hex(0x0a0a0a), 0);
    lv_obj_set_style_bg_opa(scr_dev, LV_OPA_COVER, 0);
    lv_obj_t *t = lv_label_create(scr_dev);
    lv_label_set_text(t, "DEV / DIAGNOSTICS");
    lv_obj_set_style_text_color(t, lv_color_hex(0x00E5FF),0);
    lv_obj_set_style_text_font(t, &lv_font_montserrat_14, 0);
    lv_obj_align(t, LV_ALIGN_TOP_MID, 0, 20);
    lbl_dev = lv_label_create(scr_dev);
    lv_label_set_text(lbl_dev, "");
    lv_obj_set_style_text_color(lbl_dev, lv_color_hex(COL_TXT), 0);
    lv_obj_set_style_text_font(lbl_dev, &lv_font_montserrat_14, 0);
    lv_obj_align(lbl_dev, LV_ALIGN_TOP_LEFT, 20, 60);
    add_back_pill(scr_dev, "Back to Tools", cb_back_to_tools);
}
static void dev_refresh() {
    if (!lbl_dev) return;
    int cached = 0, ok = 0, empty = 0, miss = 0, gz = 0, err = 0;
    for (int i=0; i<TILE_CACHE_SLOTS; i++) {
        if (!g_cache[i].loaded) continue;
        cached++;
        switch (g_cache[i].status) {
            case TILE_ST_OK:      ok++; break;
            case TILE_ST_NODATA:  empty++; break;
            case TILE_ST_MISSING: miss++; break;
            case TILE_ST_GZIP:    gz++; break;
            default:              err++; break;
        }
    }
    uint32_t heap = ESP.getFreeHeap();
    uint32_t psram = ESP.getFreePsram();
    int bat = g_has_pmu ? g_pmu.getBatteryPercent() : -1;
    char buf[640];
    snprintf(buf, sizeof(buf),
        "Map:   %s\n"
        "Zoom:  %u (%u zooms)\n"
        "Cam:   %.0f, %.0f  scl %.4f\n"
        "Tiles: %u indexed, %d cached\n"
        "Cache: ok%d empty%d gz%d err%d\n"
        "TileMem: %lu KB\n"
        "POIs:  %lu\n"
        "Objs:  %u last frame\n"
        "Draw:  %lu us\n"
        "Heap:  %lu KB free\n"
        "PSRAM: %lu KB free\n"
        "Bat:   %d %%\n"
        "IMU:   %s   Mag: %s\n"
        "Hdg:   %.1f   Mark: %.1f",
        g_meta.title.c_str(),
        (unsigned)g_cam_zoom, (unsigned)g_zoom_count,
        g_cam_x, g_cam_y, g_cam_scale,
        (unsigned)g_tile_count, cached,
        ok, empty, gz, err,
        (unsigned long)(g_cache_bytes/1024),
        (unsigned long)g_poi_count,
        (unsigned)g_render_obj_count,
        (unsigned long)g_render_time_us,
        (unsigned long)(heap/1024),
        (unsigned long)(psram/1024),
        bat,
        g_has_imu?"ok":"--", g_has_mag?"ok":"--",
        g_heading, g_marked_heading);
    lv_label_set_text(lbl_dev, buf);
}

// ----- Tools menu -----
static void cb_tools_enter_mode(lv_event_t *e) {
    AppMode *tp = (AppMode*)lv_event_get_user_data(e);
    if (tp) enter_mode(*tp);
}
static void cb_tools_breadcrumb(lv_event_t *e) {
    (void)e;
    breadcrumb_add((int32_t)g_cam_x, (int32_t)g_cam_y);
    enter_mode(MODE_MAP);
}
static void cb_tools_clear_nav(lv_event_t *e) {
    (void)e;
    g_has_nav_target = false;
    g_need_map_redraw = true;
    enter_mode(MODE_MAP);
}

static void build_tools() {
    if (scr_tools) return;
    wp_sync_styles();
    scr_tools = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr_tools, lv_color_hex(COL_BG), 0);
    lv_obj_set_style_bg_opa(scr_tools, LV_OPA_COVER, 0);
    lv_obj_clear_flag(scr_tools, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *t = lv_label_create(scr_tools);
    lv_label_set_text(t, "TOOLS");
    lv_obj_add_style(t, &wp_style_label_dim, 0);
    lv_obj_align(t, LV_ALIGN_TOP_MID, 0, 18);

    // Flex-wrap grid container (Track B: no more hand-placed x/y positions).
    lv_obj_t *grid = lv_obj_create(scr_tools);
    lv_obj_set_size(grid, LCD_WIDTH - 24, LCD_HEIGHT - 120);
    lv_obj_align(grid, LV_ALIGN_TOP_MID, 0, 46);
    lv_obj_set_style_bg_opa(grid, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(grid, 0, 0);
    lv_obj_set_style_pad_all(grid, 4, 0);
    lv_obj_set_flex_flow(grid, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_align(grid, LV_FLEX_ALIGN_SPACE_EVENLY,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    struct ToolDef { const char *icon; const char *name; AppMode target; };
    static const ToolDef defs[] = {
        { LV_SYMBOL_SAVE,      "Save",      MODE_SAVEDIALOG },
        { LV_SYMBOL_LIST,      "Layers",    MODE_LAYERS },
        { LV_SYMBOL_IMAGE,     "Themes",    MODE_THEMES },
        { LV_SYMBOL_GPS,       "Compass",   MODE_COMPASS },
        { LV_SYMBOL_MINUS,     "Level",     MODE_LEVEL },
        { LV_SYMBOL_CHARGE,    "Sun",       MODE_SUN },
        { LV_SYMBOL_LOOP,      "Bearings",  MODE_BEARINGS },
        { LV_SYMBOL_DIRECTORY, "Search",    MODE_SEARCH },
        { LV_SYMBOL_UP,        "Block Nav", MODE_BLOCKNAV },
        { LV_SYMBOL_EYE_OPEN,  "Explore",   MODE_EXPLORE },
        { LV_SYMBOL_SETTINGS,  "Developer", MODE_DEV },
    };
    for (size_t i = 0; i < sizeof(defs)/sizeof(defs[0]); i++) {
        AppMode *tgt = (AppMode*)lv_mem_alloc(sizeof(AppMode));
        *tgt = defs[i].target;
        MenuItem mi = { defs[i].icon, defs[i].name, cb_tools_enter_mode, tgt };
        wp_create_menu_item(grid, mi);
    }

    // Quick actions -- same tile builder, gold-tinted via warn accent.
    MenuItem drop  = { LV_SYMBOL_GPS,  "Breadcrumb", cb_tools_breadcrumb, nullptr };
    MenuItem clear = { LV_SYMBOL_CLOSE, "Clear Nav", cb_tools_clear_nav, nullptr };
    wp_create_menu_item(grid, drop);
    wp_create_menu_item(grid, clear);

    add_back_pill(scr_tools, "Back to Map", cb_back_to_map);
}

// ----- Block navigation (D-pad for small-screen users) -----
static lv_obj_t *scr_blocknav = nullptr;
static lv_obj_t *lbl_block_info = nullptr;
static float g_block_step_m = 100.0f;
static void block_nav_step(int dx_blocks, int dy_blocks) {
    // step = g_block_step_m metres worth of world units
    float step = g_block_step_m * (g_meta.units_per_metre > 0 ? g_meta.units_per_metre : 1.0f);
    g_cam_x += dx_blocks * step;
    g_cam_y += dy_blocks * step;
    g_need_map_redraw = true;
    if (lbl_block_info) {
        char b[64];
        snprintf(b, 64, "step: %d m\ncam: %ld, %ld", (int)g_block_step_m,
                 (long)g_cam_x, (long)g_cam_y);
        lv_label_set_text(lbl_block_info, b);
    }
}
static void build_blocknav() {
    if (scr_blocknav) return;
    scr_blocknav = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr_blocknav, lv_color_hex(COL_BG), 0);
    lv_obj_set_style_bg_opa(scr_blocknav, LV_OPA_COVER, 0);

    lv_obj_t *t = lv_label_create(scr_blocknav);
    lv_label_set_text(t, "BLOCK NAV");
    lv_obj_set_style_text_color(t, lv_color_hex(COL_MUTE), 0);
    lv_obj_set_style_text_font(t, &lv_font_montserrat_14, 0);
    lv_obj_align(t, LV_ALIGN_TOP_MID, 0, 20);

    // 4-way D-pad
    struct P { int dx, dy; const char *lbl; };
    static const P pads[] = {
        { 0,+1, LV_SYMBOL_UP },
        {-1, 0, LV_SYMBOL_LEFT },
        {+1, 0, LV_SYMBOL_RIGHT },
        { 0,-1, LV_SYMBOL_DOWN },
    };
    int cx = LCD_WIDTH/2, cy = LCD_HEIGHT/2 - 20;
    int sz = 72;
    for (unsigned i=0; i<sizeof(pads)/sizeof(pads[0]); i++) {
        lv_obj_t *b = lv_btn_create(scr_blocknav);
        lv_obj_set_size(b, sz, sz);
        int bx = cx + pads[i].dx * (sz + 10) - sz/2;
        int by = cy - pads[i].dy * (sz + 10) - sz/2;
        lv_obj_set_pos(b, bx, by);
        lv_obj_set_style_bg_color(b, lv_color_hex(0x141414), 0);
        lv_obj_set_style_border_color(b, lv_color_hex(COL_ACCENT), 0);
        lv_obj_set_style_border_width(b, 1, 0);
        lv_obj_set_style_radius(b, 14, 0);
        lv_obj_t *l = lv_label_create(b);
        lv_label_set_text(l, pads[i].lbl);
        lv_obj_set_style_text_color(l, lv_color_hex(COL_ACCENT), 0);
        lv_obj_set_style_text_font(l, &lv_font_montserrat_22, 0);
        lv_obj_center(l);
        int *dxy = (int*)lv_mem_alloc(sizeof(int) * 2);
        dxy[0] = pads[i].dx; dxy[1] = pads[i].dy;
        lv_obj_add_event_cb(b, [](lv_event_t *e){
            int *p = (int*)lv_event_get_user_data(e);
            block_nav_step(p[0], p[1]);
        }, LV_EVENT_CLICKED, dxy);
    }
    // Step size cycle button
    lv_obj_t *sb = lv_btn_create(scr_blocknav);
    lv_obj_set_size(sb, 120, 40);
    lv_obj_align(sb, LV_ALIGN_TOP_MID, 0, 60);
    lv_obj_set_style_bg_color(sb, lv_color_hex(0x141414), 0);
    lv_obj_set_style_border_color(sb, lv_color_hex(COL_GOLD), 0);
    lv_obj_set_style_border_width(sb, 1, 0);
    lv_obj_set_style_radius(sb, 20, 0);
    lv_obj_t *sbl = lv_label_create(sb);
    lv_label_set_text(sbl, "Step: 100 m");
    lv_obj_set_style_text_color(sbl, lv_color_hex(COL_GOLD), 0);
    lv_obj_center(sbl);
    lv_obj_add_event_cb(sb, [](lv_event_t *e){
        static const float steps[] = { 20.0f, 50.0f, 100.0f, 250.0f, 500.0f, 1000.0f };
        int cur = 2;
        for (unsigned i=0; i<sizeof(steps)/sizeof(steps[0]); i++)
            if (fabsf(steps[i] - g_block_step_m) < 0.5f) { cur = (int)i; break; }
        cur = (cur + 1) % (int)(sizeof(steps)/sizeof(steps[0]));
        g_block_step_m = steps[cur];
        lv_obj_t *lbl = lv_obj_get_child(lv_event_get_target(e), 0);
        if (lbl) {
            char b[24];
            if (g_block_step_m < 1000) snprintf(b, 24, "Step: %d m", (int)g_block_step_m);
            else                        snprintf(b, 24, "Step: %.1f km", g_block_step_m/1000);
            lv_label_set_text(lbl, b);
        }
    }, LV_EVENT_CLICKED, NULL);

    lbl_block_info = lv_label_create(scr_blocknav);
    lv_label_set_text(lbl_block_info, "step: 100 m");
    lv_obj_set_style_text_color(lbl_block_info, lv_color_hex(COL_MUTE), 0);
    lv_obj_set_style_text_font(lbl_block_info, &lv_font_montserrat_12, 0);
    lv_obj_align(lbl_block_info, LV_ALIGN_BOTTOM_MID, 0, -70);

    add_back_pill(scr_blocknav, "Back to Map", cb_back_to_map);
}

// ----- Explore (pick a random POI and preview it) -----
static lv_obj_t *scr_explore = nullptr;
static lv_obj_t *lbl_expl_title = nullptr;
static lv_obj_t *lbl_expl_body = nullptr;
static uint32_t g_expl_idx = 0;
static void explore_pick() {
    if (!g_pois || g_poi_count == 0) {
        if (lbl_expl_title) lv_label_set_text(lbl_expl_title, "No POIs");
        if (lbl_expl_body)  lv_label_set_text(lbl_expl_body,
            "This map has no POI database.\nOpen a map built with POIs.");
        return;
    }
    g_expl_idx = (uint32_t)random(0, (long)g_poi_count);
    FMapPOI &p = g_pois[g_expl_idx];
    if (lbl_expl_title) lv_label_set_text(lbl_expl_title, p.name ? p.name : "(unnamed)");
    if (lbl_expl_body) {
        double dx = (double)p.x - g_cam_x, dy = (double)p.y - g_cam_y;
        float m = (g_meta.units_per_metre > 0)
            ? sqrtf((float)(dx*dx+dy*dy)) / g_meta.units_per_metre : 0;
        char b[128];
        if (g_meta.units_per_metre > 0) {
            if (m < 1000) snprintf(b, 128, "type: %u\ndistance: %d m\ntap Go to jump", p.type, (int)(m+0.5f));
            else          snprintf(b, 128, "type: %u\ndistance: %.1f km\ntap Go to jump", p.type, m/1000.0f);
        } else {
            snprintf(b, 128, "type: %u\ntap Go to jump", p.type);
        }
        lv_label_set_text(lbl_expl_body, b);
    }
}
static void build_explore() {
    if (scr_explore) return;
    scr_explore = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr_explore, lv_color_hex(COL_BG), 0);
    lv_obj_set_style_bg_opa(scr_explore, LV_OPA_COVER, 0);
    lv_obj_t *t = lv_label_create(scr_explore);
    lv_label_set_text(t, "EXPLORE");
    lv_obj_set_style_text_color(t, lv_color_hex(COL_MUTE), 0);
    lv_obj_set_style_text_font(t, &lv_font_montserrat_14, 0);
    lv_obj_align(t, LV_ALIGN_TOP_MID, 0, 20);

    lbl_expl_title = lv_label_create(scr_explore);
    lv_label_set_text(lbl_expl_title, "—");
    lv_obj_set_style_text_color(lbl_expl_title, lv_color_hex(COL_ACCENT), 0);
    lv_obj_set_style_text_font(lbl_expl_title, &lv_font_montserrat_22, 0);
    lv_obj_align(lbl_expl_title, LV_ALIGN_TOP_MID, 0, 70);

    lbl_expl_body = lv_label_create(scr_explore);
    lv_label_set_text(lbl_expl_body, "");
    lv_obj_set_style_text_color(lbl_expl_body, lv_color_hex(COL_TXT), 0);
    lv_obj_set_style_text_font(lbl_expl_body, &lv_font_montserrat_14, 0);
    lv_label_set_long_mode(lbl_expl_body, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(lbl_expl_body, 360);
    lv_obj_align(lbl_expl_body, LV_ALIGN_TOP_MID, 0, 120);

    lv_obj_t *next = lv_btn_create(scr_explore);
    lv_obj_set_size(next, 140, 42);
    lv_obj_align(next, LV_ALIGN_BOTTOM_MID, -80, -74);
    lv_obj_set_style_bg_color(next, lv_color_hex(0x141414), 0);
    lv_obj_set_style_border_color(next, lv_color_hex(COL_ACCENT), 0);
    lv_obj_set_style_border_width(next, 1, 0);
    lv_obj_set_style_radius(next, 20, 0);
    lv_obj_t *nl = lv_label_create(next); lv_label_set_text(nl, "Next"); lv_obj_center(nl);
    lv_obj_set_style_text_color(nl, lv_color_hex(COL_ACCENT), 0);
    lv_obj_add_event_cb(next, [](lv_event_t *e){ (void)e; explore_pick(); }, LV_EVENT_CLICKED, NULL);

    lv_obj_t *go = lv_btn_create(scr_explore);
    lv_obj_set_size(go, 140, 42);
    lv_obj_align(go, LV_ALIGN_BOTTOM_MID, 80, -74);
    lv_obj_set_style_bg_color(go, lv_color_hex(COL_GOLD), 0);
    lv_obj_set_style_radius(go, 20, 0);
    lv_obj_t *gl = lv_label_create(go); lv_label_set_text(gl, "Go"); lv_obj_center(gl);
    lv_obj_set_style_text_color(gl, lv_color_hex(0x000000), 0);
    lv_obj_add_event_cb(go, [](lv_event_t *e){
        (void)e;
        if (g_pois && g_expl_idx < g_poi_count) {
            g_cam_x = g_pois[g_expl_idx].x;
            g_cam_y = g_pois[g_expl_idx].y;
            g_has_nav_target = true;
            g_nav_target_x = g_pois[g_expl_idx].x;
            g_nav_target_y = g_pois[g_expl_idx].y;
            g_need_map_redraw = true;
        }
        enter_mode(MODE_MAP);
    }, LV_EVENT_CLICKED, NULL);

    add_back_pill(scr_explore, "Back to Tools", cb_back_to_tools);
}

// ----- POI card -----
static uint32_t g_poi_shown_idx = 0xFFFFFFFFu; // last POI index shown on card
static void build_poicard() {
    if (scr_poicard) return;
    scr_poicard = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr_poicard, lv_color_hex(COL_BG), 0);
    lv_obj_set_style_bg_opa(scr_poicard, LV_OPA_COVER, 0);
    lbl_poi_title = lv_label_create(scr_poicard);
    lv_label_set_text(lbl_poi_title, "");
    lv_obj_set_style_text_color(lbl_poi_title, lv_color_hex(COL_ACCENT),0);
    lv_obj_set_style_text_font(lbl_poi_title, &lv_font_montserrat_22, 0);
    lv_obj_align(lbl_poi_title, LV_ALIGN_TOP_MID, 0, 40);
    lbl_poi_body = lv_label_create(scr_poicard);
    lv_label_set_text(lbl_poi_body, "");
    lv_obj_set_style_text_color(lbl_poi_body, lv_color_hex(COL_TXT), 0);
    lv_obj_set_style_text_font(lbl_poi_body, &lv_font_montserrat_14, 0);
    lv_obj_align(lbl_poi_body, LV_ALIGN_TOP_MID, 0, 90);
    lv_label_set_long_mode(lbl_poi_body, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(lbl_poi_body, 360);

    // Navigate here — sets the nav target so the bearing arrow tracks the POI.
    lv_obj_t *nav = lv_btn_create(scr_poicard);
    lv_obj_set_size(nav, 160, 44);
    lv_obj_align(nav, LV_ALIGN_BOTTOM_MID, -90, -74);
    lv_obj_set_style_bg_color(nav, lv_color_hex(COL_GOLD), 0);
    lv_obj_set_style_radius(nav, 22, 0);
    lv_obj_t *nl = lv_label_create(nav);
    lv_label_set_text(nl, LV_SYMBOL_RIGHT "  Navigate");
    lv_obj_set_style_text_color(nl, lv_color_hex(0x000000), 0);
    lv_obj_center(nl);
    lv_obj_add_event_cb(nav, [](lv_event_t *e){
        (void)e;
        if (g_poi_shown_idx < g_poi_count && g_pois) {
            g_has_nav_target = true;
            g_nav_target_x = g_pois[g_poi_shown_idx].x;
            g_nav_target_y = g_pois[g_poi_shown_idx].y;
            g_nav_target_name = g_pois[g_poi_shown_idx].name ? String(g_pois[g_poi_shown_idx].name) : String("target");
            enter_mode(MODE_MAP);
        }
    }, LV_EVENT_CLICKED, NULL);

    // Save location — same as the map's Save dialog but auto-named to the POI.
    lv_obj_t *sv = lv_btn_create(scr_poicard);
    lv_obj_set_size(sv, 160, 44);
    lv_obj_align(sv, LV_ALIGN_BOTTOM_MID, 90, -74);
    lv_obj_set_style_bg_color(sv, lv_color_hex(0x141414), 0);
    lv_obj_set_style_border_color(sv, lv_color_hex(COL_ACCENT), 0);
    lv_obj_set_style_border_width(sv, 1, 0);
    lv_obj_set_style_radius(sv, 22, 0);
    lv_obj_t *sl = lv_label_create(sv);
    lv_label_set_text(sl, LV_SYMBOL_SAVE "  Save");
    lv_obj_set_style_text_color(sl, lv_color_hex(COL_ACCENT), 0);
    lv_obj_center(sl);
    lv_obj_add_event_cb(sv, [](lv_event_t *e){
        (void)e;
        if (g_poi_shown_idx < g_poi_count && g_pois) {
            String nm = g_pois[g_poi_shown_idx].name ? String(g_pois[g_poi_shown_idx].name) : String("waypoint");
            saved_add(g_pois[g_poi_shown_idx].x, g_pois[g_poi_shown_idx].y, nm, 0);
        }
        enter_mode(MODE_MAP);
    }, LV_EVENT_CLICKED, NULL);

    add_back_pill(scr_poicard, "Back to Map", cb_back_to_map);
}

// =============================================================================
//  Mode transitions
// =============================================================================
static void enter_mode(AppMode m) {
    g_prev_mode = g_mode;
    g_mode = m;
    switch (m) {
        case MODE_MAP:
            build_blank();
            lv_scr_load(scr_blank);
            g_need_map_redraw = true;
            wp_map_hint_show(millis());   // teach the gestures, then get out of the way
            break;
        case MODE_LIBRARY:
            build_blank();
            lv_scr_load(scr_blank);
            g_lib_redraw = true;
            break;
        case MODE_TOOLS:      build_tools();     lv_scr_load(scr_tools); break;
        case MODE_COMPASS:    build_compass();   lv_scr_load(scr_compass); break;
        case MODE_LEVEL:      build_level();     lv_scr_load(scr_level); break;
        case MODE_SUN:        build_sun();       lv_scr_load(scr_sun); break;
        case MODE_BEARINGS:   build_bearings();  bearings_refresh(); lv_scr_load(scr_bearings); break;
        case MODE_LAYERS:     build_layers();    lv_scr_load(scr_layers); break;
        case MODE_THEMES:     build_themes();    lv_scr_load(scr_themes); break;
        case MODE_SAVEDIALOG: build_savedlg();   lv_scr_load(scr_savedlg); break;
        case MODE_SEARCH:     build_search();    lv_scr_load(scr_search); break;
        case MODE_DEV:        build_dev();       dev_refresh(); lv_scr_load(scr_dev); break;
        case MODE_POI_CARD:   build_poicard();   lv_scr_load(scr_poicard); break;
        case MODE_BLOCKNAV:   build_blocknav();  lv_scr_load(scr_blocknav); break;
        case MODE_EXPLORE:    build_explore();   explore_pick(); lv_scr_load(scr_explore); break;
        default: break;
    }
}

// =============================================================================
//  Open / close a map (directory of MVT tiles) from the library
// =============================================================================
static void library_open_selected() {
    if (g_map_count_lib == 0) return;
    if (g_map_sel < 0 || g_map_sel >= g_map_count_lib) return;
    const String &path = g_maps[g_map_sel].path;
    if (!mvt_map_open(path)) {
        // stay in library, flash red border
        gfx->drawRoundRect(18, 108, LCD_WIDTH - 36, 244, 16, 0xF800);
        delay(500);
        g_lib_redraw = true;
        return;
    }
    saved_load();
    beacons_load();

    // restore per-map layers
    Preferences p; p.begin("waypoint", true);
    char key[16]; wp_map_key(g_meta.title, key, sizeof(key));
    g_layer_mask = p.getUInt(key, 0xFFFFFFFFu);
    p.end();

    // persist last opened + recent-maps ring (up to 5)
    Preferences pw; pw.begin("waypoint", false);
    pw.putString("last_map", path);
    String recent[5];
    for (int i=0; i<5; i++) { char k[6]; snprintf(k, 6, "rm%d", i); recent[i] = pw.getString(k, ""); }
    String stack[5]; int sp = 0;
    stack[sp++] = path;
    for (int i=0; i<5 && sp<5; i++) if (recent[i].length() && recent[i] != path) stack[sp++] = recent[i];
    for (int i=0; i<5; i++) { char k[6]; snprintf(k, 6, "rm%d", i); pw.putString(k, i < sp ? stack[i].c_str() : ""); }
    pw.end();

    cam_reset_to_bounds();
    cam_select_zoom();
    enter_mode(MODE_MAP);
}

static void library_close() {
    mvt_map_close();
    enter_mode(MODE_LIBRARY);
}

// =============================================================================
//  Interaction: BOOT button state machine + touch (pan / pinch / long-press)
// =============================================================================
static uint32_t g_btn_down_t = 0;
static uint32_t g_last_release_t = 0;
static bool     g_btn_long_fired = false;

// Touch. The recogniser classifies the gesture (see wp_gesture_feed); what lives
// here is only the state the map keeps between samples - the previous pan sample
// and the scale the current pinch started from.
static bool     g_dragging = false;
static int32_t  g_drag_last_x = 0, g_drag_last_y = 0;
static bool     g_pinching = false;
static int32_t  g_pinch_start_y = 0;
static float    g_pinch_start_scale = 1;

static void map_pan(int dx, int dy) {
    // Convert screen delta into world delta (inverse of world_to_screen scaling / rotation)
    float rr = g_map_rotation_deg * (float)M_PI / 180.0f;
    float ca = cosf(rr), sa = sinf(rr);
    double wdx = ( dx * ca + -dy * sa) / (double)g_cam_scale;
    double wdy = (-dx * sa + -dy * ca) / (double)g_cam_scale;
    // note: screen +y = world -y; sign flipped via -dy above
    g_cam_x -= wdx;
    g_cam_y -= wdy;
    g_need_map_redraw = true;
}
static void map_zoom(float factor) {
    g_cam_scale *= factor;
    float lo, hi; cam_scale_bounds(&lo, &hi);
    if (g_cam_scale < lo) g_cam_scale = lo;
    if (g_cam_scale > hi) g_cam_scale = hi;
    cam_select_zoom();
    g_need_map_redraw = true;
}
static void map_recentre() {
    cam_reset_to_bounds();
    cam_select_zoom();
    g_need_map_redraw = true;
}

static void handle_boot_button(uint32_t now) {
    bool btn = (digitalRead(BOOT_BTN_PIN) == LOW);
    if (btn) wp_mark_activity(now);
    if (btn) {
        if (!g_btn_down_t) { g_btn_down_t = now; g_btn_long_fired = false; }
        else {
            uint32_t held = now - g_btn_down_t;
            // Escape hatch: >= 4 s hold = direct return to LauncherOS from ANY mode.
            if (held >= 4000) {
                return_to_launcher();
            }
            // Regular 1.5 s hold = back one level (matches MapOS behaviour).
            if (!g_btn_long_fired && held > 1500) {
                g_btn_long_fired = true;
                if (g_mode == MODE_MAP)              library_close();
                else if (g_mode == MODE_LIBRARY)     return_to_launcher();
                else                                 enter_mode(MODE_MAP);
            }
        }
    } else if (g_btn_down_t) {
        uint32_t held = now - g_btn_down_t;
        g_btn_down_t = 0;
        if (g_btn_long_fired) { /* handled */ }
        else if (held >= 400 && held <= 1200) {
            if (g_mode == MODE_LIBRARY) library_open_selected();
            else if (g_mode == MODE_MAP) map_recentre();
            g_last_release_t = 0;
        } else if (held < 400) {
            if (g_last_release_t && (now - g_last_release_t) < 500) {
                // double tap
                if (g_mode == MODE_LIBRARY) {
                    g_map_sel = (g_map_sel - 1 + g_map_count_lib) % (g_map_count_lib?g_map_count_lib:1);
                    g_lib_redraw = true;
                } else if (g_mode == MODE_MAP) {
                    map_zoom(1.0f / 1.5f);
                }
                g_last_release_t = 0;
            } else {
                g_last_release_t = now;
            }
        }
    }
    if (g_last_release_t && (now - g_last_release_t) > 500) {
        if (g_mode == MODE_LIBRARY) {
            g_map_sel = (g_map_sel + 1) % (g_map_count_lib?g_map_count_lib:1);
            g_lib_redraw = true;
        } else if (g_mode == MODE_MAP) {
            map_zoom(1.5f);
        }
        g_last_release_t = 0;
    }
}

// POI hit-test on tap (within ~14 px screen)
static bool tap_hit_poi(int sx, int sy) {
    if (!g_pois) return false;
    for (uint32_t i=0; i<g_poi_count; i++) {
        int px, py;
        world_to_screen(g_pois[i].x, g_pois[i].y, &px, &py);
        int d2 = (px-sx)*(px-sx) + (py-sy)*(py-sy);
        if (d2 < 14*14) {
            g_poi_shown_idx = i;
            String body;
            body += "type: "; body += g_pois[i].type; body += "\n";
            body += "x: "; body += g_pois[i].x; body += "\n";
            body += "y: "; body += g_pois[i].y; body += "\n";
            // Distance from cam
            if (g_meta.units_per_metre > 0) {
                double dx = (double)g_pois[i].x - g_cam_x;
                double dy = (double)g_pois[i].y - g_cam_y;
                float m = sqrtf((float)(dx*dx+dy*dy)) / g_meta.units_per_metre;
                char db[24];
                if (m < 1000) snprintf(db, 24, "distance: %d m", (int)(m+0.5f));
                else          snprintf(db, 24, "distance: %.1f km", m/1000.0f);
                body += db;
            }
            build_poicard();
            lv_label_set_text(lbl_poi_title, g_pois[i].name ? g_pois[i].name : "(unnamed)");
            lv_label_set_text(lbl_poi_body, body.c_str());
            enter_mode(MODE_POI_CARD);
            return true;
        }
    }
    return false;
}

// -----------------------------------------------------------------------------
//  Gesture dispatch
//
//  Every screen is driven by the same four verbs: tap what you want, swipe to
//  move between screens, hold for the contextual menu, drag what you are
//  looking at. The recogniser has already done the hard part - these functions
//  only decide what a gesture means where the user is standing.
// -----------------------------------------------------------------------------

// -----------------------------------------------------------------------------
//  Library: swipe left/right to browse, tap the card to open, swipe up (or
//  hold) for the details of the selected map, swipe down to put them away.
// -----------------------------------------------------------------------------
static void library_gesture(uint16_t ev, uint32_t now) {
    (void)now;
    const int n = g_map_count_lib ? g_map_count_lib : 1;
    if (ev & WG_SWIPE_LEFT)  { g_map_sel = (g_map_sel + 1) % n;     g_lib_redraw = true; }
    if (ev & WG_SWIPE_RIGHT) { g_map_sel = (g_map_sel - 1 + n) % n; g_lib_redraw = true; }
    if (ev & WG_SWIPE_UP)    { if (!g_lib_info) { g_lib_info = true;  g_lib_redraw = true; } }
    if (ev & WG_SWIPE_DOWN)  { if (g_lib_info)  { g_lib_info = false; g_lib_redraw = true; } }
    if (ev & WG_LONG_PRESS)  { g_lib_info = !g_lib_info; g_lib_redraw = true; }

    if (ev & WG_TAP) {
        const int32_t x = g_gesture.sx, y = g_gesture.sy;
        const int card_x = 20, card_y = 110, card_w = LCD_WIDTH - 40, card_h = 240;
        const bool in_card = (x >= card_x && x < card_x + card_w &&
                              y >= card_y && y < card_y + card_h);
        if (!in_card) {
            if (g_lib_info) { g_lib_info = false; g_lib_redraw = true; }
            return;
        }
        // The arrows either side of the card step through the library; a tap
        // anywhere else on the card opens the map.
        if (n > 1 && x < card_x + 70)               { g_map_sel = (g_map_sel - 1 + n) % n; g_lib_redraw = true; }
        else if (n > 1 && x > card_x + card_w - 70) { g_map_sel = (g_map_sel + 1) % n;     g_lib_redraw = true; }
        else                                        { library_open_selected(); }
    }
}

// -----------------------------------------------------------------------------
//  Map: drag to pan, two fingers to zoom, tap a POI, double-tap in, two-finger
//  tap out, hold (or swipe up from the bottom strip) for Tools, swipe down from
//  the top strip to go back to the library.
//
//  Going back is deliberately a *flick* - it has to start at the top edge and
//  finish inside WP_EDGE_SWIPE_MAX_MS - because an ordinary pan that happened to
//  end low on the screen must never throw the map away. Two fingers anywhere
//  does the same thing without that caveat.
// -----------------------------------------------------------------------------
static void map_gesture(uint16_t ev, uint32_t now) {
    // ---- continuous: one finger pans, two fingers zoom -------------------
    if (g_tch_pressed && g_gesture.down) {
        if (g_gesture.two_finger) {
            if (!g_pinching) {
                g_pinching = true;
                g_dragging = false;
                g_pinch_start_y = g_tch_y;
                g_pinch_start_scale = g_cam_scale;
            }
            const int32_t dy = g_tch_y - g_pinch_start_y;
            float ns = g_pinch_start_scale * expf((float)(-dy) * 0.006f);
            float lo, hi; cam_scale_bounds(&lo, &hi);
            if (ns < lo) ns = lo;
            if (ns > hi) ns = hi;
            if (fabsf(ns - g_cam_scale) > 1e-4f) {
                g_cam_scale = ns;
                cam_select_zoom();
                g_need_map_redraw = true;
            }
        } else if (g_gesture.dragged) {
            if (!g_dragging) {
                g_dragging = true;
                g_pinching = false;
                g_drag_last_x = g_tch_x;
                g_drag_last_y = g_tch_y;
            }
            const int32_t dx = g_tch_x - g_drag_last_x;
            const int32_t dy = g_tch_y - g_drag_last_y;
            if (dx || dy) {
                map_pan((int)dx, (int)dy);
                g_drag_last_x = g_tch_x;
                g_drag_last_y = g_tch_y;
            }
        }
    }
    if (ev & WG_RELEASE) { g_dragging = false; g_pinching = false; }

    // ---- discrete --------------------------------------------------------
    if (ev & WG_LONG_PRESS) enter_mode(MODE_TOOLS);
    if (ev & WG_DOUBLE_TAP) map_zoom(1.5f);
    if (ev & WG_TWO_TAP)    map_zoom(1.0f / 1.5f);

    if (ev & WG_SWIPE_DOWN) {
        const bool edge = (g_gesture.edge == WP_EDGE_TOP) &&
                          (g_gesture.dur <= WP_EDGE_SWIPE_MAX_MS);
        if (g_gesture.two_finger || edge) { library_close(); return; }
    }
    if (ev & WG_SWIPE_UP) {
        const bool edge = (g_gesture.edge == WP_EDGE_BOTTOM) &&
                          (g_gesture.dur <= WP_EDGE_SWIPE_MAX_MS);
        if (g_gesture.two_finger || edge) { enter_mode(MODE_TOOLS); return; }
    }

    if (ev & WG_TAP) {
        if (!map_chrome_tap(g_gesture.sx, g_gesture.sy, now) &&
            !tap_hit_poi((int)g_gesture.sx, (int)g_gesture.sy)) {
            wp_map_hint_show(now);      // any tap brings the gesture hints back
        }
    }
}

// -----------------------------------------------------------------------------
//  Menu screens: tap a row to act on it, swipe down to go back a level, swipe
//  left/right to walk between the screens that belong together (the sensors,
//  the two setup screens, the tools). Nothing to press, and the footer of each
//  screen says which way to swipe.
// -----------------------------------------------------------------------------
static const AppMode WP_SENSOR_CYCLE[] = { MODE_COMPASS, MODE_LEVEL, MODE_SUN, MODE_BEARINGS };
static const AppMode WP_SETUP_CYCLE[]  = { MODE_LAYERS, MODE_THEMES };
static const AppMode WP_TOOL_CYCLE[]   = { MODE_SEARCH, MODE_SAVEDIALOG, MODE_BLOCKNAV,
                                           MODE_EXPLORE, MODE_DEV };

static void menu_back() {
    if (g_mode == MODE_POI_CARD || g_mode == MODE_TOOLS) enter_mode(MODE_MAP);
    else                                                 enter_mode(MODE_TOOLS);
}

static void menu_cycle(int dir) {
    const AppMode *sets[3]  = { WP_SENSOR_CYCLE, WP_SETUP_CYCLE, WP_TOOL_CYCLE };
    const int      sizes[3] = { 4, 2, 5 };
    for (int s = 0; s < 3; s++) {
        for (int i = 0; i < sizes[s]; i++) {
            if (sets[s][i] == g_mode) {
                enter_mode(sets[s][(i + dir + sizes[s]) % sizes[s]]);
                return;
            }
        }
    }
}

static void menu_gesture(uint16_t ev, uint32_t now) {
    (void)now;
    if (ev & WG_SWIPE_DOWN) { menu_back(); return; }
    if (ev & (WG_SWIPE_LEFT | WG_SWIPE_RIGHT))
        menu_cycle((ev & WG_SWIPE_LEFT) ? 1 : -1);
}

static void handle_map_touch(uint32_t now) {
    const uint16_t ev = g_gesture_events;
    g_gesture_events = WG_NONE;
    if (ev || g_tch_pressed) wp_mark_activity(now);

    if (g_mode == MODE_LIBRARY) { library_gesture(ev, now); return; }
    if (g_mode == MODE_MAP)     { map_gesture(ev, now);     return; }
    menu_gesture(ev, now);
}

// =============================================================================
//  setup / loop
// =============================================================================
void setup() {
    launcher_app_boot();
    // Auto-create the WayPoint tree on the SD card. mkdir is idempotent — safe
    // if the folder already exists.
    SD_MMC.mkdir("/WATCH");
    SD_MMC.mkdir("/WATCH/bin");
    SD_MMC.mkdir("/WATCH/WayPoint");
    SD_MMC.mkdir("/WATCH/WayPoint/saved");

    // First-run helper: drop a README into /WATCH/WayPoint/ if it doesn't exist,
    // so anyone browsing the SD card on a PC sees how to add maps.
    if (!SD_MMC.exists("/WATCH/WayPoint/README.txt")) {
        File rf = SD_MMC.open("/WATCH/WayPoint/README.txt", FILE_WRITE);
        if (rf) {
            rf.print(
                "WaypointOS map folder\n"
                "---------------------\n"
                "Place map directories in this folder to have them appear in\n"
                "the watch's map library carousel.\n\n"
                "Each map is a directory of Mapbox Vector Tiles (MVT), produced\n"
                "by tilemaker from OSM data:\n\n"
                "  OpenStreetMap -> .osm.pbf -> tilemaker -> MVT -> WaypointOS\n\n"
                "Structure:\n"
                "  /WATCH/WayPoint/<MapName>/{z}/{x}/{y}.pbf   (or .mvt)\n"
                "  /WATCH/WayPoint/<MapName>/metadata.json     (optional)\n"
                "  /WATCH/WayPoint/saved/                      (per-map saves)\n"
                "  /WATCH/WayPoint/breadcrumbs.csv\n"
                "  /WATCH/WayPoint/bearings.txt\n"
                "  /WATCH/WayPoint/loc.txt        (single line: LAT,LON for sun calc)\n\n"
                "metadata.json (all fields optional):\n"
                "  { \"name\":\"Byford\", \"description\":\"...\",\n"
                "    \"bounds\":\"minLon,minLat,maxLon,maxLat\",\n"
                "    \"minzoom\":10, \"maxzoom\":17 }\n\n"
                "Controls - gestures first (see WaypointOS.ino header):\n"
                "  library: swipe < > browse, tap the card to open,\n"
                "           swipe ^ for details, swipe v to put them away\n"
                "  map:     drag pan, double tap zoom in, 2-finger tap zoom\n"
                "           out, 2-finger drag to zoom, tap a POI,\n"
                "           hold for Tools, swipe ^ from the bottom strip for\n"
                "           Tools, swipe v from the top strip back to the library\n"
                "  menus:   tap a row, swipe v back a level, swipe < > between\n"
                "           the screens that belong together\n"
                "  BOOT tap        - library: next map / map: zoom in\n"
                "  BOOT 2x tap     - library: prev / map: zoom out\n"
                "  BOOT hold 0.5s  - library: open / map: recentre\n"
                "  BOOT hold 1.5s  - back one level (sub -> map -> lib -> launcher)\n"
                "  BOOT hold 4s    - direct return to LauncherOS from anywhere\n"
            );
            rf.close();
        }
    }

    time_restore_from_launcher();
    read_loc();
    imu_init();

    // NVS restore
    Preferences p; p.begin("waypoint", true);
    g_theme_id       = p.getUChar("theme", THEME_FUSION_DARK);
    if (g_theme_id > THEME_CHILL) g_theme_id = THEME_FUSION_DARK;
    wp_sync_styles();   // Track B: prime shared LVGL styles from restored theme
    g_mx_off = p.getFloat("mxo", 0); g_my_off = p.getFloat("myo", 0); g_mz_off = p.getFloat("mzo", 0);
    g_mx_scl = p.getFloat("mxs", 1); g_my_scl = p.getFloat("mys", 1); g_mz_scl = p.getFloat("mzs", 1);
    g_marked_heading = p.getFloat("mark_hdg", -1);
    String last_map  = p.getString("last_map", "");
    p.end();

    splash_show();

    library_scan();

    // pre-select last opened
    if (last_map.length()) {
        for (int i = 0; i < g_map_count_lib; i++) {
            if (g_maps[i].path == last_map) { g_map_sel = i; break; }
        }
    }

    build_blank();
    lv_scr_load(scr_blank);
    g_mode = MODE_LIBRARY;
    g_lib_redraw = true;

    // Kick display brightness to full and seed the idle timer so we don't
    // instantly dim while the user is still looking at the splash/library.
    wp_brightness_apply(WP_BRIGHTNESS_FULL);
    g_brightness_target = WP_BRIGHTNESS_FULL;
    g_last_activity_ms  = millis();
    g_idle_dimmed       = false;
}

void loop() {
    lv_timer_handler();
    uint32_t now = millis();

    handle_boot_button(now);
    handle_map_touch(now);

    // Sensors: 12 Hz
    static uint32_t imu_t = 0;
    if (now - imu_t > 80) {
        imu_t = now;
        imu_read();
        mag_cal_tick();
        // IMU-based wake: pick the watch up and the screen brightens back.
        if (wp_imu_motion_detected()) wp_mark_activity(now);
        if (g_has_mag) {
            float h = tilt_comp_heading();
            g_heading = h;
            if (g_rotate_with_heading && g_mode == MODE_MAP) {
                // Throttle: only redraw when heading moved more than 3° since last drawn frame
                float dh = fabsf(h - g_last_rot_drawn_deg);
                if (dh > 180) dh = 360 - dh;
                if (g_last_rot_drawn_deg < -900 || dh > 3.0f) {
                    g_map_rotation_deg = h;
                    g_last_rot_drawn_deg = h;
                    g_need_map_redraw = true;
                }
            }
        }
        // Update LVGL labels only on their screens
        if (g_mode == MODE_COMPASS && lbl_hdg && g_heading >= 0) {
            char b[16]; snprintf(b,16,"%d°", (int)(g_heading+0.5f));
            lv_label_set_text(lbl_hdg, b);
            lv_label_set_text(lbl_card, card_for(g_heading));
            if (g_marked_heading > 0) {
                float d = fabsf(g_heading - g_marked_heading);
                if (d > 180) d = 360 - d;
                uint32_t c = (d < 5) ? COL_GOLD : COL_ACCENT;
                lv_obj_set_style_text_color(lbl_hdg, lv_color_hex(c), 0);
            }
            if (g_cal_active) {
                lv_label_set_text_fmt(lbl_card, "CAL... %us",
                    (unsigned)((millis() - g_cal_start_ms)/1000));
            }
        }
        if (g_mode == MODE_LEVEL && level_bubble) {
            float roll  = atan2f(g_ay, g_az) * 180.0f / (float)M_PI;
            float pitch = atan2f(-g_ax, sqrtf(g_ay*g_ay+g_az*g_az)) * 180.0f / (float)M_PI;
            int bx = (int)(roll * 2.5f); if (bx>120)bx=120; if (bx<-120)bx=-120;
            int by = (int)(pitch * 2.5f); if (by>120)by=120; if (by<-120)by=-120;
            lv_obj_align(level_bubble, LV_ALIGN_CENTER, bx, by);
            char b[32]; snprintf(b,32,"%+.1f°  %+.1f°", roll, pitch);
            lv_label_set_text(lbl_level, b);
        }
    }
    static uint32_t sun_t = 0;
    if (g_mode == MODE_SUN && lbl_sun && now - sun_t > 2000) {
        sun_t = now; float az, el; sun_position(&az, &el);
        char b[40];
        if (az < 0) snprintf(b,40,"no time sync yet");
        else snprintf(b,40,"az: %d°\nel: %+d°", (int)(az+0.5f), (int)(el+0.5f));
        lv_label_set_text(lbl_sun, b);
    }
    static uint32_t dev_t = 0;
    if (g_mode == MODE_DEV && now - dev_t > 500) { dev_t = now; dev_refresh(); }

    // Raw-gfx frames (map & library)
    // Frame-rate throttle: cap draw_map_frame() at ~30 fps so many small pan
    // events between frames coalesce into a single redraw. map_pan() keeps
    // g_cam_x/y up to date every touch tick, so the *camera* never lags --
    // only the render is capped, which is what actually costs time & battery.
    #define WP_MIN_FRAME_MS  33         // ~30 fps
    if (g_mode == MODE_LIBRARY) library_draw();
    else if (g_mode == MODE_MAP) {
        static uint32_t last_frame = 0;
        bool time_ok      = (now - last_frame) >= WP_MIN_FRAME_MS;
        bool heading_tick = g_rotate_with_heading && (now - last_frame > 250);
        if ((g_need_map_redraw && time_ok) || heading_tick) {
            draw_map_frame();
            last_frame = now;
        }
    }

    // Map chrome: fade the gesture hints out a few steps after the last gesture,
    // so the strip stops covering the map without needing a button to dismiss it.
    if (g_mode == MODE_MAP) wp_map_hint_tick(now);

    // Idle screen dim: after WP_IDLE_DIM_MS with no touch/button/motion,
    // fade the AMOLED down to ~15% brightness. Wake on any activity.
    wp_idle_dim_tick(now);
    wp_brightness_tick(now);

    delay(4);
}
