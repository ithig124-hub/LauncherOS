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
//  How the map MOVES is a separate, equally testable thing: the inertia after a
//  flick, the easing of a camera move, the anchoring of a zoom, and the follow
//  of a heading all live in the WP_MOTION_ENGINE region as pure arithmetic
//  (tools/host_test asserts the flick stops, that a split frame travels the
//  same distance as a whole one, and that the point under your fingers does not
//  move). The gesture handlers only decide *what* a gesture means; the motion
//  engine decides how it feels.
//
//  The library screen is built the same way (WP_LIB_ENGINE): where its card and
//  shelf sit, which map a touch landed on, how a long title is folded and how a
//  description is wrapped are all arithmetic, so the screen that decides which
//  map you open is asserted on a PC rather than reviewed by reading it.
//
//    library  swipe < >  browse maps        map  drag           pan, then coast
//             tap card   open the map             double tap     zoom in (here)
//             swipe ^    map details              2-finger tap   zoom out (here)
//             swipe v    hide the details         2-finger drag  pinch to zoom
//             hold       map details              tap            pick a POI
//                                                 hold           Tools menu
//                                                 swipe ^ bottom Tools menu
//                                                 swipe v top    library
//                                                 tap compass    heading-up
//
//    menus    tap a row to act on it, swipe v to go back a level, swipe < > to
//             walk between the screens that belong together (sensors, setup,
//             tools)
//
//  No button for NAVIGATION. Every screen is reachable with one finger: leaving
//  for LauncherOS is the EXIT pill on the library, or a swipe down from the top
//  edge. The side button has exactly one job, and it is the one BookOS gives it:
//  hold it for 1.2 s and the watch goes back to the launcher. That is the exit
//  that still works when a screen has stopped answering, which is the only kind
//  of exit worth having on a device with no keyboard.
//
//  Diagnostics: this build talks on the watch's USB-C port itself (HWCDC, the
//  same way LauncherOS does), not on UART0, so the Arduino monitor shows
//  [boot] / [touch] / [beat] lines. A [beat] once a second says the app is
//  still looping, which screen it is on, whether touch started and how many
//  gestures arrived -- so "it froze" can be told apart from "the panel died".
//
//  Look    : Kiwi -- an ink-black canvas, one vivid green that only ever means
//            "act" or "you are here", and a map of quiet grey-greens drawn from
//            that palette. THEMES holds it alongside the older palettes; the map,
//            the library and the LVGL menus all read the same table.
//
//  SD root : /WATCH/WayPoint/
//  In-app  : maps can also ship INSIDE the binary: tools/bake_bundle.py bakes a
//            map pack into wp_maps_bundle.h, the loader reads tiles straight out
//            of flash, and the library lists them first marked "in the app" --
//            no card, nothing to copy. Without that header the app is card-only.
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

// -----------------------------------------------------------------------------
//  Where the diagnostics go.
//  On this board `Serial` is UART0 unless the build has "USB CDC On Boot"
//  enabled, and the watch's USB-C port is the NATIVE one. A sketch that prints
//  to Serial therefore looks perfectly silent in the Arduino monitor even while
//  it is running happily -- which is exactly how "frozen after the splash" and
//  "nothing in the serial monitor" end up being reported together.
//  LauncherOS opens that port itself (its object is USBSerial); this sketch does
//  the same through the core's own HWCDCSerial, so the monitor is on the port
//  it is listening to.
//
//  Which object that is has to be decided from the CHIP and the board menu, and
//  every branch below is a test the preprocessor can answer by itself. Naming
//  `HWCDC` on a target that has no USB-Serial-JTAG peripheral -- a plain ESP32,
//  or simply the wrong board picked in the IDE -- fails the whole build with
//  "'HWCDC' does not name a type", so the capability is checked first.
// -----------------------------------------------------------------------------
#if defined(SOC_USB_SERIAL_JTAG_SUPPORTED) && SOC_USB_SERIAL_JTAG_SUPPORTED && \
    defined(ARDUINO_USB_MODE) && ARDUINO_USB_MODE
  // USB Mode = "Hardware CDC and JTAG": the peripheral the cable enumerates.
  // The core names that object HWCDCSerial and only creates it when "USB CDC On
  // Boot" is enabled -- otherwise `Serial` is UART0 and nothing else exists, so
  // the object is declared here (HWCDC.cpp's definition is behind the same
  // guard, so there is no duplicate).
  #include "HWCDC.h"
  #if !defined(ARDUINO_USB_CDC_ON_BOOT) || !ARDUINO_USB_CDC_ON_BOOT
HWCDC HWCDCSerial;
  #endif
  #define WP_LOG_SERIAL HWCDCSerial
#else
  // Either the chip has no USB-Serial-JTAG, or the board menu gives the port to
  // TinyUSB. `Serial` is then the only object that exists -- the CDC when "USB
  // CDC On Boot" is enabled, UART0 otherwise -- so logging follows it and this
  // build still compiles and runs.
  #warning "WaypointOS: USB logging needs ESP32S3 + 'USB Mode: Hardware CDC and JTAG' (or 'USB CDC On Boot: Enabled'). Logging to Serial instead."
  #define WP_LOG_SERIAL Serial
#endif
// Logging is gated on a connected host: HWCDC's write() takes a lock and can
// spend up to a tenth of a second per call when the port is open but the reader
// has stalled, and a watch that logs to nobody should pay nothing at all. With
// no monitor attached every line below is a single boolean test.
#define WP_LOG(...) do { if (WP_LOG_SERIAL) WP_LOG_SERIAL.printf(__VA_ARGS__); } while (0)

// The map library that ships inside the app. tools/bake_bundle.py writes this
// header; a fresh checkout has none, and then the library is card-only. The
// #if is what makes "bake the maps in" an opt-in step instead of a build that
// fails on a file nobody generated yet.
#if defined(__has_include)
#  if __has_include("wp_maps_bundle.h")
#    include "wp_maps_bundle.h"
#    define WP_HAS_BUNDLE 1
#  endif
#endif

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
// The library carousel's arithmetic (geometry, hit testing, text folding) is
// spliced out by gen_engine.py as well, and the tools menu takes an array of
// tile descriptions, so those types belong up here too.
struct WpLibLayout;
struct WpLibShelf;
struct ToolTile;
// The built-in map bundle's reader types: used by reference in signatures the
// Arduino IDE hoists to the top, so they have to be named up here too.
struct WpBundle;
struct WpBundleMap;
// ... and the decoder for the baked thumbnails the library card shows.
struct WpImg;

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
  #define MVT_LOG(...)  do { WP_LOG_SERIAL.printf(__VA_ARGS__); } while (0)
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

// ===== BEGIN WP_MOTION_ENGINE
// =============================================================================
//  Motion engine -- the arithmetic behind how the map FEELS.
//
//  Everything the map animates is a pure function of (position, target,
//  elapsed time): a fling that decays to a stop, a zoom that grows under the
//  finger, a heading-up map that follows the wrist without twitching. That is
//  precisely the kind of code a screenshot cannot judge and a watch is the
//  worst place to debug - so the maths lives here, with no LVGL, no Arduino
//  and no I/O, and tools/host_test asserts the properties that actually decide
//  the feel: does the flick stop, does a split frame travel as far as a whole
//  one, does the point under the finger stay put, does north take the short
//  way round.
//
//  Units are screen pixels and milliseconds, matching the touch pipeline.
// =============================================================================

// Inertial panning tuned by what a 410 px panel can take: the flick decays as
// v(t) = v0 * e^(-t/TAU), so the distance still to travel is always v * TAU
// (about half a screen for a hard flick) and the motion is bounded no matter
// how hard the map is thrown.
#define WP_FLING_TAU_MS     260.0f
#define WP_FLING_STOP_PX      1.50f  // remaining travel this small = the last frame
#define WP_FLING_MAX_V       1.80f   // px/ms -- a cap, so the map cannot be thrown into orbit
#define WP_FLING_MIN_START   0.15f   // px/ms -- slower than this and the pan just stops

static inline float wp_clamp01(float t) {
    return t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
}

// Decelerating curve for camera moves. A linear ramp reads as machinery; this
// leaves immediately and arrives softly, which is what "the map settled" feels
// like. (Same shape iOS/Android use for a programmatic pan.)
static inline float wp_ease_out(float t) {
    t = wp_clamp01(t);
    const float u = 1.0f - t;
    return 1.0f - u * u * u;
}

// Cap a flick's speed while keeping its direction: a hard flick should throw
// the map a long way, never squash the motion into an axis.
static void wp_fling_cap(float *vx, float *vy) {
    const float s = sqrtf(*vx * *vx + *vy * *vy);
    if (s <= WP_FLING_MAX_V || s <= 0.0f) return;
    const float k = WP_FLING_MAX_V / s;
    *vx *= k; *vy *= k;
}

// One frame of inertial panning. Writes the pixel pan for this frame into
// *dx/*dy and decays the velocity; returns false once the motion has stopped,
// at which point the velocity is zeroed so it can neither creep nor restart.
//
// The displacement is the exact integral of the decaying velocity over the
// frame, v * TAU * (1 - e^(-dt/TAU)), not v * dt. That is what makes the pan
// frame-rate independent: two 16 ms frames travel exactly as far as one 32 ms
// frame instead of the slow-framed map always panning further.
static bool wp_fling_step(float *vx, float *vy, float dt_ms,
                          float *dx, float *dy) {
    if (dt_ms <= 0.0f) dt_ms = 1.0f;
    if (dt_ms > 100.0f) dt_ms = 100.0f;   // a stalled frame must not teleport
    const float speed = sqrtf(*vx * *vx + *vy * *vy);
    if (speed <= 0.0f) {
        *dx = 0.0f; *dy = 0.0f;
        return false;
    }
    // Still owed: `speed * TAU` of travel. Once that is under a pixel and a
    // half, this frame is the last one and it delivers the remainder instead of
    // dropping it, and the velocity goes to zero with it. Two things follow: the
    // total distance of a flick is exactly v0 * TAU (bounded, whatever the
    // frame rate), and the end of the motion is a settle rather than a visible
    // step followed by nothing.
    const bool last_frame = (speed * WP_FLING_TAU_MS <= WP_FLING_STOP_PX);
    const float f = last_frame ? 0.0f : expf(-dt_ms / WP_FLING_TAU_MS);
    *dx = *vx * WP_FLING_TAU_MS * (1.0f - f);
    *dy = *vy * WP_FLING_TAU_MS * (1.0f - f);
    *vx *= f; *vy *= f;
    return !last_frame;
}

// Shortest signed way from one angle to another, in degrees (-180, 180].
// 350 -> 10 is +20, not -340: without this, crossing north would spin the
// map almost all the way round, which is the single ugliest thing a
// heading-up map can do.
static float wp_angle_delta(float from, float to) {
    float d = fmodf(to - from, 360.0f);
    if (d > 180.0f)   d -= 360.0f;
    if (d <= -180.0f) d += 360.0f;
    return d;
}

// Ease `cur` toward `target` by `alpha` (0..1) along the short way, wrapped to
// [0, 360). The caller passes alpha = 1 - e^(-dt/TAU), which is what makes the
// follow frame-rate independent rather than "faster when the loop is fast".
static float wp_angle_step(float cur, float target, float alpha) {
    alpha = wp_clamp01(alpha);
    float v = cur + wp_angle_delta(cur, target) * alpha;
    v = fmodf(v, 360.0f);
    if (v < 0.0f) v += 360.0f;
    return v;
}

// The world-space offset of one screen pixel from the camera centre: the exact
// inverse of world_to_screen(), rotation included. It exists once, here, and
// both the UI's screen_to_world() and the zoom anchoring below are built on it
// -- an inverse that is written twice is an inverse that drifts.
static void wp_pixel_to_world_offset(int32_t ax, int32_t ay,
                                     int32_t cx, int32_t cy,
                                     float rot_deg, float scale,
                                     double *ux, double *uy) {
    const double rx = (double)(ax - cx);
    const double ry = (double)(cy - ay);        // screen +y is world -y
    const double s  = (scale > 1e-9f) ? (double)scale : 1e-9;
    if (rot_deg != 0.0f) {
        const double r  = -(double)rot_deg * M_PI / 180.0;
        const double ca = cos(r), sa = sin(r);
        *ux = (rx * ca + ry * sa) / s;
        *uy = (-rx * sa + ry * ca) / s;
    } else {
        *ux = rx / s;
        *uy = ry / s;
    }
}

// Where the camera has to be for world point (wx, wy) to land exactly on
// screen pixel (ax, ay). Pinch and double-tap zoom both use this: the map is
// zoomed about the thing under your fingers, so the point you are looking at
// is the point that does not move.
static void wp_cam_anchor(double wx, double wy,
                          int32_t ax, int32_t ay,
                          int32_t cx, int32_t cy,
                          float rot_deg, float scale,
                          double *camx, double *camy) {
    double ux, uy;
    wp_pixel_to_world_offset(ax, ay, cx, cy, rot_deg, scale, &ux, &uy);
    *camx = wx - ux;
    *camy = wy - uy;
}

// What the status strip calls the current scale. "z12.4" is a number only the
// person who wrote the renderer wants; a word is what tells everyone else how
// close they are.
static const char *wp_zoom_band(float z_eff) {
    if (z_eff <  4.0f) return "world";
    if (z_eff <  7.0f) return "country";
    if (z_eff < 10.0f) return "state";
    if (z_eff < 12.0f) return "region";
    if (z_eff < 13.5f) return "district";
    if (z_eff < 15.0f) return "city";
    if (z_eff < 16.5f) return "suburb";
    return "street";
}
// ===== END WP_MOTION_ENGINE

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

    // LauncherOS logs every touch's start and end; the same two lines are what
    // tell a dark screen with a silent monitor apart from a dead panel with a
    // live app.
    const bool was_pressed = g_tch_pressed;
    if (pressed) {
        data->state=LV_INDEV_STATE_PR; data->point.x=x; data->point.y=y;
        g_tch_pressed=true; g_tch_x=x; g_tch_y=y;
        if (!was_pressed) WP_LOG("[touch] start (%d, %d) fingers=%d\n", (int)x, (int)y, fingers);
    } else {
        data->state=LV_INDEV_STATE_REL;
        if (was_pressed) {
            WP_LOG("[touch] end dx=%d dy=%d dur=%lu\n",
                   (int)g_gesture.dx, (int)g_gesture.dy, (unsigned long)g_gesture.dur);
            g_tch_x=x; g_tch_y=y;
        }
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

// ---- shared time block (spec: TIME_CONTRACT.md) -----------------------------
//  Inlined verbatim from LauncherOS V2 / BookOS. The watch's wall clock lives in
//  ONE NVS namespace, so whichever app wrote it last, every app reads the same
//  time. Only the two lines below may differ per app: the serial port it reports
//  on, and who is writing (a module vs the launcher).
#define TIME_SERIAL   WP_LOG_SERIAL
#define TIME_SRC_ID   TIME_SRC_MODULE
// ===== SHARED TIME BLOCK v2 =================================================
//  Namespace "rtcsave" -- the ONE place the watch's wall clock lives:
//      t    uint32  unix epoch seconds, UTC
//      gmt  int32   local offset from UTC, in SECONDS   (+28800 = UTC+8)
//      seq  uint32  write counter, ++ on every publish  (diagnostics)
//      man  uint8   1 = a human set this clock, 0 = machine (NTP/boot)
//      src  uint8   0 = launcher, 1 = module            (diagnostics)
//
//  Two different things have to survive a reboot, and only one of them does:
//    * the EPOCH lives in the chip's RTC timer. It survives every software
//      reset and every deep sleep, so a launcher <-> module reboot inherits it
//      for free -- but power loss wipes it, and then this snapshot is the only
//      thing between the user and "00:00".
//    * the TIMEZONE lives in the C library's TZ string, which is RAM-ONLY. It
//      dies on EVERY reboot and no app can inherit it from the previous one.
//      NVS is therefore the offset's only home.
//  => epoch is inherited through the RTC, backed up here. Timezone is stored
//     here and re-applied by every single boot.
// ============================================================================
#define TIME_VALID_MIN      1700000000UL   // ~2023-11-14: "the RTC has been set"
#define TIME_ADOPT_SLACK_S  2L             // ignore sub-2 s snapshot/rtc skew
#define TIME_SRC_LAUNCHER   0
#define TIME_SRC_MODULE     1

static int32_t  g_tp_gmt   = 0;   // local offset from UTC, in seconds
static uint32_t g_tp_seq   = 0;   // last seq we read or wrote
static bool     g_tp_valid = false;   // a plausible wall clock is available

// Point the C library at the local UTC offset. TZ is RAM-only, so this must run
// on EVERY boot in EVERY app or the watch silently renders UTC.
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
//   nothing saved yet        -> leave everything alone
//   chip has no valid clock  -> cold boot / dead battery: adopt the NVS
//   a human set it (man=1)   -> their correction wins in EITHER direction
//   snapshot runs ahead      -> somebody moved the clock forward: adopt
//   otherwise                -> the RTC is the live clock, keep it
// (uint8_t return keeps the Arduino .ino auto-prototype generator, which runs
//  before this enum is declared, happy.)
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

// WRITE half of the contract: epoch + offset + provenance. Call it after every
// NTP sync, after a manual set, and on the way out of the app (before restart).
static void time_publish(const char *why, bool manual) {
    time_t now; time(&now);
    if (now < (time_t)TIME_VALID_MIN) return;      // RTC not set yet
    Preferences p;
    if (!p.begin("rtcsave", false)) return;
    uint32_t prev     = p.getULong("t", 0);
    uint8_t  prev_man = p.getUChar("man", 0);
    // A human's correction stays in force while the snapshot still describes the
    // same instant. Every boot refreshes `t` by a second or so and must not
    // quietly demote the clock back to "machine". A machine that lands a
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
        g_tp_valid = true;                         // never rewind a live RTC
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

// ---- LauncherOS return (BookOS parity) --------------------------------------
//  Same body BookOS uses: publish the clock FIRST, then point the bootloader at
//  the factory partition (or the other OTA slot) and restart. The publish is
//  what keeps the launcher's face on the right time after the switch; without
//  it the module's last minutes of clock are lost.
static void return_to_launcher() {
    WP_LOG("[exit] back to LauncherOS\n");
    time_publish("exit", false);
    const esp_partition_t *factory = esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_FACTORY, NULL);
    if (factory) esp_ota_set_boot_partition(factory);
    else { const esp_partition_t *o = esp_ota_get_next_update_partition(NULL); if (o && o!=esp_ota_get_running_partition()) esp_ota_set_boot_partition(o); }
    delay(80); esp_restart();
}

static bool g_touch_ok = false;      // did the touch controller answer at boot?

static void launcher_app_boot() {
    Serial.begin(115200);            // UART0, unchanged
    WP_LOG_SERIAL.begin(115200);     // the port the Arduino monitor is actually on
    WP_LOG("\n=== WaypointOS boot ===\n");
    pinMode(BOOT_BTN_PIN, INPUT_PULLUP); pinMode(PWR_BTN_PIN, INPUT_PULLUP); btStop();
    if (!gfx->begin()) { Serial.println("gfx fail"); WP_LOG("[boot] gfx->begin() failed\n"); }
    gfx->fillScreen(0x0000);
    pinMode(IIC_SCL, OUTPUT);
    for (int i=0; i<9; i++) { digitalWrite(IIC_SCL,LOW); delayMicroseconds(5); digitalWrite(IIC_SCL,HIGH); delayMicroseconds(5); }
    Wire.begin(IIC_SDA, IIC_SCL);
    Wire.setClock(400000);           // LauncherOS v6.3: fast-mode I2C
    pinMode(TP_RESET, OUTPUT); digitalWrite(TP_RESET, LOW); delay(50); digitalWrite(TP_RESET, HIGH); delay(200);
    // Touch bring-up, LauncherOS parity. Three things here are load-bearing and
    // were all missing before, any one of which leaves a live app that answers
    // nothing:
    //   * TP_INT as a pulled-up input -- the driver's interrupt flag rides on it;
    //   * retry until the controller ACKs, instead of believing one attempt
    //     (one bad power-up used to leave the panel dead for the whole session);
    //   * TOUCH_POWER_MONITOR -- without it the FT3168 can sit in its idle mode
    //     and never report coordinates.
    pinMode(TP_INT, INPUT_PULLUP);
    int tp_tries = 0;
    while (!FT3168->begin()) {
        tp_tries++;
        WP_LOG("[boot] FT3168 init fail (%d)\n", tp_tries);
        if (tp_tries >= 20) break;   // 20 x 250 ms = 5 s, then carry on and say so
        delay(250);
    }
    g_touch_ok = (tp_tries < 20);
    if (g_touch_ok) {
        FT3168->IIC_Write_Device_State(
            FT3168->Arduino_IIC_Touch::Device::TOUCH_POWER_MODE,
            FT3168->Arduino_IIC_Touch::Device_Mode::TOUCH_POWER_MONITOR);
        WP_LOG("[boot] FT3168 ready\n");
    } else {
        WP_LOG("[boot] NO TOUCH after 5 s -- the UI will not respond\n");
    }
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

// ---- BookOS "back to launcher": a hold on the side button --------------------
//  BookOS can always be left, even when its screen stops answering, because a
//  long press on the side button does the one job the screen cannot: pivot the
//  bootloader back to the launcher. Same here, same 1.2 s, and it publishes the
//  clock on the way out (return_to_launcher does that). Nothing else uses this
//  button, so it cannot collide with a gesture.
#define WP_EXIT_HOLD_MS 1200
static void wp_exit_button_poll() {
    static uint32_t hold = 0;      // millis() when the press started
    static bool     down_before = false;
    const uint32_t now = millis();
    if (digitalRead(BOOT_BTN_PIN) != LOW) { down_before = false; hold = 0; return; }
    if (!down_before) { down_before = true; hold = now; return; }
    if ((uint32_t)(now - hold) >= WP_EXIT_HOLD_MS) {
        down_before = false; hold = 0;
        WP_LOG("[exit] side button held %lums -- back to LauncherOS\n",
               (unsigned long)WP_EXIT_HOLD_MS);
        return_to_launcher();
    }
}
// ===== END SHARED BOOT BLOCK =================================================

// =============================================================================
//  Themes
// =============================================================================
enum ThemeId { THEME_FUSION_DARK=0, THEME_FUSION_LIGHT=1, THEME_SERIOUS=2, THEME_CHILL=3,
               THEME_KIWI=4, THEME_COUNT=5 };

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
    // KIWI -- the one the app ships with. Ink-black canvas, one vivid green that
    // only ever means "act" or "you are here", and a map of quiet grey-greens so
    // the green is the loudest thing on the panel. Every other colour is a shade
    // of those two ideas: dark is ground, light is something you can name. That
    // is what keeps a 410 px AMOLED legible at arm's length with a glove on.
    { "Kiwi", 0x0861, 0x10C2, 0x21A5, 0xF7BE, 0x84B1, 0x3F09, 0xFE27,
      0x29C6, 0x1103, 0x1143, 0x08C4, 0x2185, 0x1944, 0xDB3B, 0x3A47, 0x3F09, 0x3F09, 0xAED6 },
};
static uint8_t g_theme_id = THEME_KIWI;
static inline const ThemePalette &TH() { return THEMES[g_theme_id]; }

// LVGL speaks 0xRRGGBB; this file otherwise speaks RGB565. One conversion means
// an LVGL screen cannot quietly keep a colour from the theme before last -- which
// is how the menus stayed cyan and #141414 through four theme changes while the
// map around them turned green.
static inline uint32_t wp_hex24(uint16_t c) {
    const uint32_t r = (uint32_t)(((c >> 11) & 0x1F) * 255 / 31);
    const uint32_t g = (uint32_t)(((c >> 5)  & 0x3F) * 255 / 63);
    const uint32_t b = (uint32_t)(( c        & 0x1F) * 255 / 31);
    return (r << 16) | (g << 8) | b;
}
#define COL_BG     wp_hex24(TH().bg)
#define COL_TXT    wp_hex24(TH().text)
#define COL_MUTE   wp_hex24(TH().text_dim)
#define COL_ACCENT wp_hex24(TH().accent)
#define COL_GOLD   wp_hex24(TH().warn)
#define COL_CARD   wp_hex24(TH().chrome_bg)
#define COL_LINE   wp_hex24(TH().chrome_border)


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
// Seeded with Kiwi, not with the old cyan: anything drawn before the first
// wp_sync_styles() (splash, the blank screen) should already be in the theme the
// app ships, not flash the previous one for a frame.
static uint16_t   wp_pal_text = 0xF7BE, wp_pal_dim = 0x84B1,
                  wp_pal_accent = 0x3F09, wp_pal_warn = 0xFE27;

static void wp_styles_init() {
    if (wp_styles_ready) return;
    wp_styles_ready = true;

    lv_style_init(&wp_style_menu_button);
    // Kiwi's shape language in one number: a tile is a rounded slab, not a
    // rectangle with softened corners. 20 on a 60 px tile reads as a pill from
    // across a room, which is the whole point of the reference.
    lv_style_set_radius(&wp_style_menu_button, 20);
    lv_style_set_border_width(&wp_style_menu_button, 1);
    lv_style_set_pad_all(&wp_style_menu_button, 6);
    lv_style_set_shadow_width(&wp_style_menu_button, 0);

    lv_style_init(&wp_style_menu_card);
    lv_style_set_radius(&wp_style_menu_card, 22);
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
// =============================================================================
//  Map bundle reader  (WPB1)
//
//  A map can live in the application image instead of on a card: one read-only
//  blob in flash with a sorted index in front of it (tools/bake_bundle.py writes
//  it, and its docstring is the format). This is the reader -- plain pointer
//  arithmetic over a byte span, no filesystem, no allocation, so the same code
//  runs on the watch and in the host tests, and the tests can prove the lookup
//  against a bundle the packer really wrote.
//
//  Every field is read byte by byte rather than through a struct cast. The blob
//  is only 4-byte aligned in the common case, and an unaligned 32-bit load is a
//  fault on the ESP32, not a slow read.
// =============================================================================
#define WPB_HEADER    24
#define WPB_MAP_REC   36
#define WPB_TILE_REC  20

static inline uint16_t wpb_le16(const uint8_t *p) {
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}
static inline uint32_t wpb_le32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static inline float wpb_lef32(const uint8_t *p) {
    const uint32_t v = wpb_le32(p);
    float f;
    memcpy(&f, &v, sizeof(f));
    return f;
}

// Two sizes, because a pack on the card is no longer read whole. `size` is the
// pack's size on the card and bounds every offset in it; `mem` is how much of
// it sits at `data` right now. For a pack baked into the firmware they are the
// same number and nothing below behaves differently. For a pack on the card,
// `mem` covers the header, the two tables and every name/metadata/thumbnail --
// the ~46 KB a 12 MB pack actually needs resident -- while the tile bodies stay
// on the card and are read one at a time. Every place that dereferences `data`
// proves its range against `mem`; every place that only does arithmetic proves
// it against `size`.
struct WpBundle {
    const uint8_t *data;
    uint32_t size;
    uint32_t mem;
    uint16_t maps;
    uint32_t tiles;
    uint32_t map_dir;
    uint32_t tile_dir;
    uint32_t str_off;
};

struct WpBundleMap {
    const char *name;              // NUL-terminated, in the string pool
    const char *meta;              // metadata.json text, not NUL-guaranteed
    uint16_t    meta_len;
    const uint8_t *preview;        // .wpim bytes, or nullptr
    uint16_t    preview_len;
    uint8_t     minzoom, maxzoom;
    float       min_lat, min_lon, max_lat, max_lon;
};

// Validate the header once, at boot. A bundle that is absent, truncated or from
// another format version must fail here rather than produce a library of maps
// that open onto nothing.
static bool wp_bundle_open(WpBundle *b, const uint8_t *data, uint32_t size,
                           uint32_t mem) {
    if (!b) return false;
    memset(b, 0, sizeof(*b));
    if (!data || mem < WPB_HEADER || mem > size) return false;
    if (!(data[0] == 'W' && data[1] == 'P' && data[2] == 'B' && data[3] == '1')) return false;
    if (wpb_le16(data + 4) != 1) return false;
    b->data     = data;
    b->size     = size;
    b->mem      = mem;
    b->maps     = wpb_le16(data + 6);
    b->tiles    = wpb_le32(data + 8);
    b->map_dir  = wpb_le32(data + 12);
    b->tile_dir = wpb_le32(data + 16);
    b->str_off  = wpb_le32(data + 20);
    // The tables themselves have to be RAM: every lookup reads them. Tiles do
    // not, so their records are only proved against the file (`size`).
    // Division, not multiplication: a corrupt `tiles` field times 20 wraps a
    // uint32 and would sail past this check on a 64 KB file.
    if (b->map_dir  > mem || (uint32_t)b->maps  > (mem - b->map_dir)  / WPB_MAP_REC)  return false;
    if (b->tile_dir > mem || b->tiles > (mem - b->tile_dir) / WPB_TILE_REC) return false;
    if (b->str_off  > mem) return false;

    // Every offset is proved once, here. A blob that was truncated in transit or
    // half-rewritten by a bad bake then fails at boot with the library untouched,
    // instead of listing maps that open onto nothing. It is ~400 records for a
    // whole state, which is nothing next to mounting the card.
    for (uint16_t i = 0; i < b->maps; i++) {
        const uint8_t *r = b->data + b->map_dir + (uint32_t)i * WPB_MAP_REC;
        const uint32_t name_off = wpb_le32(r + 0);
        const uint32_t meta_off = wpb_le32(r + 4);
        const uint16_t meta_len = wpb_le16(r + 8);
        const uint16_t pv_len   = wpb_le16(r + 10);
        const uint32_t pv_off   = wpb_le32(r + 12);
        if (name_off >= mem || meta_off >= mem) return false;
        if (!memchr(b->data + name_off, 0, mem - name_off)) return false;
        if (!meta_len || meta_len > mem - meta_off) return false;
        if (pv_len && (pv_off > mem || pv_len > mem - pv_off)) return false;
    }
    for (uint32_t i = 0; i < b->tiles; i++) {
        const uint8_t *r = b->data + b->tile_dir + i * WPB_TILE_REC;
        if ((int)r[0] >= (int)b->maps) return false;
        const uint32_t off = wpb_le32(r + 12);
        const uint32_t len = wpb_le32(r + 16);
        if (off > size || len > size - off) return false;
        // Binary search needs the index sorted by (map, z, x, y); an unsorted one
        // would not fail, it would just stop finding tiles.
        if (i) {
            const uint8_t *p = r - WPB_TILE_REC;
            bool ordered = false;
            if (p[0] != r[0])      ordered = p[0] < r[0];
            else if (p[1] != r[1]) ordered = p[1] < r[1];
            else {
                const uint32_t px = wpb_le32(p + 4), py = wpb_le32(p + 8);
                const uint32_t cx = wpb_le32(r + 4), cy = wpb_le32(r + 8);
                ordered = (px != cx) ? (px < cx) : (py < cy);
            }
            if (!ordered) return false;
        }
    }
    return true;
}

static int wp_bundle_map_count(const WpBundle *b) {
    return (b && b->data) ? (int)b->maps : 0;
}

static bool wp_bundle_map(const WpBundle *b, int i, WpBundleMap *out) {
    if (!b || !b->data || !out || i < 0 || i >= (int)b->maps) return false;
    const uint8_t *r = b->data + b->map_dir + (uint32_t)i * WPB_MAP_REC;
    const uint32_t name_off = wpb_le32(r + 0);
    const uint32_t meta_off = wpb_le32(r + 4);
    const uint16_t meta_len = wpb_le16(r + 8);
    const uint16_t prv_len  = wpb_le16(r + 10);
    const uint32_t prv_off  = wpb_le32(r + 12);
    // The bounds are proved for the *strings* too, not just the numbers: the name
    // is handed out as a C string and the metadata is read by length, so a
    // truncated or hand-edited blob could otherwise point either one past the end
    // of flash and be believed. A name must be terminated; the metadata must lie
    // inside the blob, because its length is what bounds it.
    if (name_off >= b->mem || meta_off >= b->mem) return false;
    if (!memchr(b->data + name_off, 0, b->mem - name_off)) return false;
    if (!meta_len || meta_len > b->mem - meta_off) return false;
    if (prv_len && (prv_off > b->mem || prv_len > b->mem - prv_off)) return false;
    memset(out, 0, sizeof(*out));
    out->name       = (const char *)(b->data + name_off);
    out->meta       = (const char *)(b->data + meta_off);
    out->meta_len   = meta_len;
    out->preview    = prv_len ? (b->data + prv_off) : nullptr;
    out->preview_len= prv_len;
    out->minzoom    = r[16];
    out->maxzoom    = r[17];
    out->min_lat    = wpb_lef32(r + 20);
    out->min_lon    = wpb_lef32(r + 24);
    out->max_lat    = wpb_lef32(r + 28);
    out->max_lon    = wpb_lef32(r + 32);
    return true;
}

// Binary search over the index, which the packer sorts by (map, z, x, y).
// Returns the tile's byte RANGE in the pack -- offset and length, not a pointer.
// A pack on the card only keeps its index resident, so the range is what the
// caller needs: inside `mem` it can be used in place, past `mem` it is a seek on
// the open pack file. Asking "is there a tile here?" never needs the bytes at
// all, and goes through this too.
static bool wp_bundle_tile_span(const WpBundle *b, int map, int z, int32_t x, int32_t y,
                                uint32_t *out_off, uint32_t *out_len) {
    if (out_off) *out_off = 0;
    if (out_len) *out_len = 0;
    if (!b || !b->data || map < 0 || !b->tiles) return false;
    int32_t lo = 0, hi = (int32_t)b->tiles - 1;
    while (lo <= hi) {
        const int32_t mid = lo + (hi - lo) / 2;
        const uint8_t *r = b->data + b->tile_dir + (uint32_t)mid * WPB_TILE_REC;
        const int      m  = r[0];
        const int      zz = r[1];
        const int32_t  rx = (int32_t)wpb_le32(r + 4);
        const int32_t  ry = (int32_t)wpb_le32(r + 8);
        int c;
        if (m != map)       c = (m  < map) ? -1 : 1;
        else if (zz != z)   c = (zz < z)   ? -1 : 1;
        else if (rx != x)   c = (rx < x)   ? -1 : 1;
        else if (ry != y)   c = (ry < y)   ? -1 : 1;
        else                c = 0;
        if (c == 0) {
            const uint32_t off = wpb_le32(r + 12);
            const uint32_t len = wpb_le32(r + 16);
            if (off > b->size || len > b->size - off) return false;
            if (out_off) *out_off = off;
            if (out_len) *out_len = len;
            return true;
        }
        if (c < 0) lo = mid + 1; else hi = mid - 1;
    }
    return false;
}

// How many tiles this map has at one zoom. Linear over the index on purpose:
// it runs once per zoom level when a map is opened, and an index sorted by
// (map, z, ...) would need a second table to beat ~400 pointer-free compares.
static uint32_t wp_bundle_zoom_tiles(const WpBundle *b, int map, int z) {
    if (!b || !b->data) return 0;
    uint32_t n = 0;
    for (uint32_t i = 0; i < b->tiles; i++) {
        const uint8_t *r = b->data + b->tile_dir + i * WPB_TILE_REC;
        if ((int)r[0] == map && (int)r[1] == z) n++;
    }
    return n;
}

// =============================================================================
//  Images  (.wpim)
//
//  A map's thumbnail is a picture, not a map: it is baked once by
//  tools/map_preview.py, palettised to at most 255 colours, and run-length
//  encoded per row. The device only expands it, so there is no PNG decoder, no
//  zlib, no font, and no rasteriser in the firmware -- the whole feature is the
//  eighty lines below.
//
//  Format (little-endian):
//     0  magic "WPIM"     4  version (1)      5  flags
//     6  u16 width        8  u16 height      10  u16 palette_count
//    12  u16 palette[palette_count]      RGB565
//    ...  RLE stream, exactly width*height pixels:
//          0x00..0x7F  literal run of (b+1) indices, then that many bytes
//          0x80..0xFF  repeat of ((b & 0x7F) + 2) pixels of the next byte
// =============================================================================
struct WpImg {
    uint16_t w, h;
    uint16_t palette_count;
    const uint8_t *pal;        // 2 bytes per colour, read with wpb_le16
    const uint8_t *data;
    uint32_t data_len;
};

static bool wp_img_open(WpImg *im, const uint8_t *data, uint32_t len) {
    if (!im) return false;
    memset(im, 0, sizeof(*im));
    if (!data || len < 12) return false;
    if (!(data[0] == 'W' && data[1] == 'P' && data[2] == 'I' && data[3] == 'M')) return false;
    if (data[4] != 1) return false;
    const uint16_t w = wpb_le16(data + 6);
    const uint16_t h = wpb_le16(data + 8);
    const uint16_t n = wpb_le16(data + 10);
    if (!w || !h || !n) return false;
    const uint32_t pal_bytes = (uint32_t)n * 2;
    if (len < 12 + pal_bytes) return false;
    im->w = w; im->h = h; im->palette_count = n;
    im->pal = data + 12;
    im->data = data + 12 + pal_bytes;
    im->data_len = len - 12 - pal_bytes;
    return true;
}

// Expand to an RGB565 buffer of any size, nearest-neighbour. Scaling belongs
// here rather than in the painter because the card's text column changes width
// with the library size, and one bitmap that only fits one of those widths would
// be a layout that can only be drawn one way.
static bool wp_img_decode(const WpImg *im, uint16_t *dst, int dw, int dh) {
    if (!im || !im->data || !dst || dw <= 0 || dh <= 0) return false;
    uint16_t *line = (uint16_t*)malloc((size_t)im->w * sizeof(uint16_t));
    if (!line) return false;
    uint32_t pos = 0;
    bool ok = true;
    int dy = 0;
    for (int y = 0; y < (int)im->h && ok; y++) {
        int x = 0;
        while (x < (int)im->w) {
            if (pos >= im->data_len) { ok = false; break; }
            const uint8_t tok = im->data[pos++];
            // A run never crosses a row: the encoder starts every row fresh. So
            // one that does not fit the rest of this row is a corrupt stream, and
            // saying so is cheaper than carrying a "current row" through the
            // decoder and hoping the next row agrees with it.
            if (tok & 0x80) {
                const int run = (tok & 0x7F) + 2;
                if (run > (int)im->w - x) { ok = false; break; }
                if (pos >= im->data_len) { ok = false; break; }
                const uint8_t idx = im->data[pos++];
                if (idx >= im->palette_count) { ok = false; break; }
                const uint16_t c = wpb_le16(im->pal + (uint32_t)idx * 2);
                for (int i = 0; i < run; i++) line[x++] = c;
            } else {
                const int run = tok + 1;
                if (run > (int)im->w - x) { ok = false; break; }
                if (run > (int)(im->data_len - pos)) { ok = false; break; }
                for (int i = 0; i < run; i++) {
                    const uint8_t idx = im->data[pos + i];
                    if (idx >= im->palette_count) { ok = false; break; }
                    line[x++] = wpb_le16(im->pal + (uint32_t)idx * 2);
                }
                if (!ok) break;
                pos += (uint32_t)run;
            }
        }
        if (!ok) break;
        while (dy < dh && (int)((int64_t)dy * im->h / dh) == y) {
            for (int dx = 0; dx < dw; dx++) {
                const int sx = (int)((int64_t)dx * im->w / dw);
                dst[(size_t)dy * dw + dx] = line[sx];
            }
            dy++;
        }
    }
    free(line);
    return ok && dy == dh;
}

static String     g_map_folder = "/WATCH/WayPoint";
static String     g_map_dir_path;              // full SD path of the loaded map's dir
static bool       g_map_loaded = false;
// -1 = the loaded map came from a folder of tile files. Otherwise this is the
// map's index in a bundle, and every tile lookup goes to that blob instead of
// to the filesystem.
static WpBundle   g_bundle;          // the pack baked into the firmware (if any)
static bool       g_bundle_ok = false;
static int        g_map_bundle = -1;

// -----------------------------------------------------------------------------
//  Map packs on the CARD:  /WATCH/WayPoint/<name>.wpb
//
//  A .wpb is a WPB1 bundle that happens to live on the SD card instead of in
//  the firmware. Only its HEAD is read into RAM: the header, the two index
//  tables, the map names, the metadata.json text and the thumbnails. For a real
//  12 MB pack that is 46 KB -- the tile bodies stay on the card and are read
//  one at a time, when the renderer asks for them. The old reader held the
//  whole pack in one allocation, which is why a 12.6 MB state pack was refused
//  on a board with 8 MB of PSRAM that also owes 3 MB to the tile cache.
//
//  Everything above this is unchanged: same WPB1 validation, same binary
//  search, same library card, same renderer. A card pack and a baked-in pack
//  differ in exactly one place -- whether the offset the index returns is
//  dereferenced or seeked to.
// -----------------------------------------------------------------------------
#define WP_SD_BUNDLE_SLOTS 3
// How much of a pack may live in RAM: index, strings, thumbnails. Reached only
// by a pack that stores its map details behind its tiles instead of in front.
#define WP_SD_BUNDLE_MAX   (6u * 1024u * 1024u)
// The whole pack on the card, tiles included. Only the scan pays for this.
#define WP_SD_BUNDLE_FILE_MAX (64u * 1024u * 1024u)

struct WpSdBundle {
    String  path;                 // where the bytes came from (rescan key too)
    uint8_t *data;                // the resident head, NOT the whole pack
    uint32_t size;                // the file's size on the card
    WpBundle b;
    bool    ok;                   // validated WPB1, listed in the library
    bool    broken;               // a .wpb was here but its bytes made no sense
    const char *why;              // why it is broken, in words, for the card
};
static WpSdBundle g_sd_bundles[WP_SD_BUNDLE_SLOTS];
static int        g_sd_bundle_count = 0;
// Which pack the OPEN map came from: -1 = the baked one, >= 0 = g_sd_bundles[].
static int8_t     g_map_bundle_slot = -1;

// Which blob the open map reads from. One accessor, so the places that ask for
// a tile, a zoom row or the map's header cannot disagree about which bundle
// they mean.
static const WpBundle *wp_bundle_of(int slot) {
    if (slot >= 0) {
        if (slot >= g_sd_bundle_count || !g_sd_bundles[slot].ok) return nullptr;
        return &g_sd_bundles[slot].b;
    }
    return g_bundle_ok ? &g_bundle : nullptr;
}
static const WpBundle *wp_map_bundle() {
    return wp_bundle_of(g_map_bundle_slot);
}

// Drop every card pack (a rescan, or a card pulled out from under us).
static void wp_sd_bundles_reset() {
    for (int i = 0; i < g_sd_bundle_count; i++) {
        free(g_sd_bundles[i].data);
        g_sd_bundles[i].data = nullptr;
        g_sd_bundles[i].size = 0;
        g_sd_bundles[i].ok = false;
        g_sd_bundles[i].broken = false;
        g_sd_bundles[i].why = nullptr;
        g_sd_bundles[i].path = String();
        g_sd_bundles[i].b = WpBundle();
    }
    g_sd_bundle_count = 0;
}

// How many bytes of the pack's head must be resident for every name, metadata
// blob and thumbnail to be addressable. Returns 0 when `have` already covers
// them all. A name has no stored length, so one that runs off the end asks for
// 256 more bytes rather than being called broken -- the caller grows the head
// and asks again.
static uint32_t wp_sd_head_need(const uint8_t *buf, uint32_t have, uint16_t maps,
                                uint32_t map_dir) {
    uint32_t need = 0;
    for (uint16_t i = 0; i < maps; i++) {
        const uint8_t *r = buf + map_dir + (uint32_t)i * WPB_MAP_REC;
        const uint32_t name_off = wpb_le32(r + 0);
        const uint32_t meta_off = wpb_le32(r + 4);
        const uint16_t meta_len = wpb_le16(r + 8);
        const uint16_t pv_len   = wpb_le16(r + 10);
        const uint32_t pv_off   = wpb_le32(r + 12);
        uint32_t end = 0;
        if (meta_len) end = meta_off + (uint32_t)meta_len;
        if (pv_len) {
            const uint32_t pv_end = pv_off + (uint32_t)pv_len;
            if (pv_end > end) end = pv_end;
        }
        if (name_off >= have) {
            // The name starts past what we have. One more byte would not prove
            // it is terminated, so ask for room to read a whole short name.
            const uint32_t want = name_off + 256;
            if (want > need) need = want;
        } else {
            const uint8_t *nul = (const uint8_t*)memchr(buf + name_off, 0, have - name_off);
            if (!nul) {
                const uint32_t want = name_off + 256;
                if (want > need) need = want;
                if (end > need) need = end;
                continue;
            }
            const uint32_t name_end = name_off + (uint32_t)(nul - (buf + name_off)) + 1;
            if (name_end > end) end = name_end;
        }
        if (end > have && end > need) need = end;
    }
    return need;
}

// Read `len` bytes at `off` out of one card pack. The tile bodies live here:
// the index says where they are, this fetches exactly one of them into the
// caller's buffer, and nothing else of the pack is touched.
static bool wp_sd_pack_read(int slot, uint32_t off, uint32_t len, uint8_t *dst) {
    if (slot < 0 || slot >= g_sd_bundle_count || !dst || !len) return false;
    const WpSdBundle &s = g_sd_bundles[slot];
    if (!s.size || off > s.size || len > s.size - off) return false;
    File f = SD_MMC.open(s.path, FILE_READ);
    if (!f) return false;
    bool ok = false;
    if (f.seek(off)) ok = (f.read(dst, len) == (size_t)len);
    f.close();
    return ok;
}

// Read one .wpb's head, prove the header, and claim a slot for it. Returns the
// slot, or -1 only when there is no slot left to claim.
//
// Every other failure still claims the slot and leaves a REASON in it, because
// the library lists what is on the card: a pack the user copied over and cannot
// open has to appear as a card that says why, not vanish. "Out of memory" in
// particular has to read differently from "this is not a map pack" - the first
// one is the app's fault and the second one is the file's.
static int wp_sd_bundle_load(const String &path) {
    if (g_sd_bundle_count >= WP_SD_BUNDLE_SLOTS) {
        WP_LOG("[WPB] '%s' -- only %d pack slots, ignored\n",
                path.c_str(), WP_SD_BUNDLE_SLOTS);
        return -1;
    }
    const int slot = g_sd_bundle_count;
    WpSdBundle &s = g_sd_bundles[slot];
    s.path   = path;
    s.data   = nullptr;
    s.size   = 0;
    s.b      = WpBundle();
    s.ok     = false;
    s.broken = true;
    s.why    = "it could not be read";
    g_sd_bundle_count++;      // from here on the pack is listed either way

    File f = SD_MMC.open(path, FILE_READ);
    if (!f || f.isDirectory()) {
        if (f) f.close();
        s.why = "the file could not be opened";
        WP_LOG("[WPB] '%s' -- could not be opened\n", path.c_str());
        return slot;
    }
    const uint32_t sz = (uint32_t)f.size();
    s.size = sz;              // kept even on failure: the size is the diagnosis
    if (sz < WPB_HEADER) {
        f.close();
        s.why = "the file is smaller than a map pack header";
        WP_LOG("[WPB] '%s' -- %u bytes, under the %u byte header\n",
                path.c_str(), (unsigned)sz, (unsigned)WPB_HEADER);
        return slot;
    }
    if (sz > WP_SD_BUNDLE_FILE_MAX) {
        f.close();
        s.why = "it is larger than a map pack this build will open";
        WP_LOG("[WPB] '%s' -- %u bytes, over the %u byte pack limit\n",
                path.c_str(), (unsigned)sz, (unsigned)WP_SD_BUNDLE_FILE_MAX);
        return slot;
    }
    // The header says where the two tables are, and the tables say where
    // everything else is, so the head is grown in the order the format reveals
    // it. Reading the header alone first also means a file that is not a pack
    // costs 24 bytes to reject instead of the whole file.
    uint8_t hdr[WPB_HEADER];
    if (f.read(hdr, WPB_HEADER) != WPB_HEADER) {
        f.close();
        s.why = "its header could not be read";
        WP_LOG("[WPB] '%s' -- short header read\n", path.c_str());
        return slot;
    }
    if (!(hdr[0] == 'W' && hdr[1] == 'P' && hdr[2] == 'B' && hdr[3] == '1')) {
        f.close();
        s.why = "it does not begin with the letters WPB1, so it is not a map pack";
        WP_LOG("[WPB] '%s' -- not a WPB1 pack\n", path.c_str());
        return slot;
    }
    if (wpb_le16(hdr + 4) != 1) {
        f.close();
        s.why = "the header says WPB1 but in a format version this build does not read";
        WP_LOG("[WPB] '%s' -- format version %u\n", path.c_str(), (unsigned)wpb_le16(hdr + 4));
        return slot;
    }
    const uint16_t maps     = wpb_le16(hdr + 6);
    const uint32_t tiles    = wpb_le32(hdr + 8);
    const uint32_t map_dir  = wpb_le32(hdr + 12);
    const uint32_t tile_dir = wpb_le32(hdr + 16);
    const uint32_t str_off  = wpb_le32(hdr + 20);
    if (map_dir > sz || (uint32_t)maps > (sz - map_dir) / WPB_MAP_REC ||
        tile_dir > sz || tiles > (sz - tile_dir) / WPB_TILE_REC || str_off > sz) {
        f.close();
        s.why = "the header says WPB1 but an index inside points out of range";
        WP_LOG("[WPB] '%s' -- index runs past the end of the file\n", path.c_str());
        return slot;
    }

    uint32_t head = map_dir + (uint32_t)maps * WPB_MAP_REC;
    if (tile_dir + tiles * WPB_TILE_REC > head) head = tile_dir + tiles * WPB_TILE_REC;
    if (str_off + 1 > head) head = str_off + 1;
    if (head > sz) head = sz;

    // PSRAM first, internal heap only as a fallback. `have` is what was really
    // read: the index and every string/thumbnail the map table names.
    uint8_t *buf = nullptr;
    uint32_t have = 0;
    for (int attempt = 0; attempt < 8; attempt++) {
        if (head > WP_SD_BUNDLE_MAX) {
            s.why = "it stores its index and map details behind its tiles, so opening it needs more memory than this build has";
            break;
        }
        buf = (uint8_t*)ps_malloc(head);
        if (!buf) buf = (uint8_t*)malloc(head);
        if (!buf) {
            s.why = "there was not enough free memory to hold its index";
            break;
        }
        if (!f.seek(0) || f.read(buf, head) != (size_t)head) {
            free(buf); buf = nullptr;
            s.why = "the read stopped before the end of the file";
            break;
        }
        have = head;
        const uint32_t need = wp_sd_head_need(buf, have, maps, map_dir);
        if (!need) break;                 // everything the header names is here
        free(buf); buf = nullptr;
        uint32_t next = (need > have) ? need : have + 1;
        if (next > sz) next = sz;
        if (next == have) {
            s.why = "the header says WPB1 but an index inside points out of range";
            break;
        }
        head = next;
    }
    f.close();
    if (!buf) {
        s.b = WpBundle();
        WP_LOG("[WPB] '%s' -- %s\n", path.c_str(), s.why);
        return slot;
    }

    s.data = buf;
    s.ok   = wp_bundle_open(&s.b, buf, sz, have);
    if (!s.ok) {
        const bool looks = (buf[0] == 'W' && buf[1] == 'P' &&
                            buf[2] == 'B' && buf[3] == '1');
        s.why = looks
              ? "the header says WPB1 but an index inside points out of range"
              : "it does not begin with the letters WPB1, so it is not a map pack";
        WP_LOG("[WPB] '%s' -- %s\n", path.c_str(), s.why);
        free(s.data);
        s.data = nullptr;
        s.b    = WpBundle();
        return slot;
    }
    s.broken = false;
    s.why    = nullptr;
    WP_LOG("[WPB] '%s' -- %d maps, %u tiles, %u bytes (%u of them in RAM)\n",
            path.c_str(), (int)s.b.maps, (unsigned)s.b.tiles, (unsigned)sz,
            (unsigned)have);
    return slot;
}

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
// "Is there a tile here?" -- the bundle answers from its index, the card from its
// directories. Both are remembered in the same negative cache, because from the
// renderer's point of view a hole is a hole.
static bool tile_exists_on_disk(int32_t z, int32_t x, int32_t y) {
    if (tile_probe_missing(z, x, y)) return false;
    if (g_map_bundle >= 0) {
        // The index answers this; the tile bytes never leave the card for it.
        const WpBundle *B = wp_map_bundle();
        if (B && wp_bundle_tile_span(B, g_map_bundle, z, x, y, nullptr, nullptr))
            return true;
    } else if (mvt_tile_path(z, x, y).length() > 0) {
        return true;
    }
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

    // Two places a tile can come from, and everything below this point is shared:
    // if the map is built in, the bytes are already in flash and are copied into
    // the same buffer the card path fills, so the inflate/decode/cache code below
    // cannot tell the difference. The copy is a few kilobytes; the inflate it
    // feeds is the expensive part either way.
    String path;
    uint8_t *buf = nullptr;
    size_t sz = 0;

    FMapTile *slot = cache_evict_slot();
    slot->z = z; slot->tx = x; slot->ty = y;
    slot->loaded = false;
    slot->status = TILE_ST_EMPTY;
    slot->obj_count = 0; slot->objs = nullptr;
    for (int i = 0; i <= LAY__COUNT; i++) slot->band[i] = 0;
    slot->last_use_ms = millis();

    if (g_map_bundle >= 0) {
        const WpBundle *B = wp_map_bundle();
        uint32_t off = 0, len = 0;
        if (!B || !wp_bundle_tile_span(B, g_map_bundle, z, x, y, &off, &len)) {
            // Not an error state to cache: a tile the bundle does not hold is
            // simply outside this map, exactly like a missing file on the card.
            MVT_LOG("[TILE] z=%d x=%d y=%d -- not in the bundle\n", (int)z, (int)x, (int)y);
            return nullptr;
        }
        sz = (size_t)len;
        buf = (uint8_t*)ps_malloc(sz);
        if (!buf) buf = (uint8_t*)malloc(sz);
        if (!buf) {
            MVT_LOG("[TILE]   malloc(%u) FAILED -- skipping\n", (unsigned)sz);
            slot->loaded = true; slot->status = TILE_ST_OOM; return slot;
        }
        // Inside the resident head the bytes are already there (that is every
        // tile of a baked-in pack, and none of a real card pack). Past it, the
        // pack's own index gave the offset, so this is one seek + one read of
        // exactly the tile asked for -- not a re-read of the pack.
        if (off <= B->mem && len <= B->mem - off) {
            memcpy(buf, B->data + off, sz);
            MVT_LOG("[TILE] z=%d x=%d y=%d bundle bytes=%u\n",
                    (int)z, (int)x, (int)y, (unsigned)sz);
        } else if (wp_sd_pack_read(g_map_bundle_slot, off, len, buf)) {
            MVT_LOG("[TILE] z=%d x=%d y=%d card pack bytes=%u off=%u\n",
                    (int)z, (int)x, (int)y, (unsigned)sz, (unsigned)off);
        } else {
            MVT_LOG("[TILE] z=%d x=%d y=%d card pack read FAILED (off=%u len=%u)\n",
                    (int)z, (int)x, (int)y, (unsigned)off, (unsigned)len);
            free(buf);
            slot->loaded = true; slot->status = TILE_ST_IOERR;
            return slot;
        }
    } else {
        path = mvt_tile_path(z, x, y);
        if (!path.length()) {
            MVT_LOG("[TILE] z=%d x=%d y=%d -- file not found on SD\n", (int)z, (int)x, (int)y);
            return nullptr;
        }
        File f = SD_MMC.open(path, FILE_READ);
        if (!f) {
            MVT_LOG("[TILE] z=%d x=%d y=%d path='%s' -- SD open FAILED\n",
                    (int)z, (int)x, (int)y, path.c_str());
            slot->loaded = true; slot->status = TILE_ST_IOERR; return slot;
        }
        sz = f.size();
        MVT_LOG("[TILE] z=%d x=%d y=%d path='%s' bytes=%u\n",
                (int)z, (int)x, (int)y, path.c_str(), (unsigned)sz);
        if (!sz || sz > 512 * 1024) {
            MVT_LOG("[TILE]   size out of bounds -- ignored\n");
            f.close(); slot->loaded = true; slot->status = TILE_ST_BADSIZE; return slot;
        }
        buf = (uint8_t*)ps_malloc(sz);
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
// One parser, two sources: a metadata.json on the card and the identical text in
// the bundle's string pool. A bundled map is therefore read by exactly the code
// that has always read a card map -- title, bounds, zoom range, all of it -- and
// cannot quietly acquire a second set of rules.
static void mvt_meta_apply_json(const String &s, const String &dirname, FMapMeta &out) {
    out.title = String();
    out.desc = String();
    out.min_lat = out.min_lon = out.max_lat = out.max_lon = 0;
    out.origin_lat = out.origin_lon = 0;
    out.units_per_metre = 1;
    out.minzoom = 10; out.maxzoom = 17; out.refzoom = 17;
    out.projection = 1; // web mercator
    out.created = 0;

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
    // coordinate is `(1u << refzoom)`-scaled, so a bogus maxzoom (0, or 31 from
    // a hand-edited metadata.json, or a 64-bit value that truncates) used to
    // shift the whole world away and render an empty screen with no error at
    // all. A too-small maxzoom (< minzoom) reversed the zoom loop in
    // mvt_map_open and made the map refuse to open.
    if (out.minzoom > 22) out.minzoom = 22;
    if (out.maxzoom > 22) out.maxzoom = 22;
    if (out.maxzoom < out.minzoom) out.maxzoom = out.minzoom;

    if (!out.title.length()) out.title = dirname;
    if (out.max_lat == 0 && out.min_lat == 0) {
        out.desc = out.desc.length() ? out.desc : String("MVT tiles (bounds unknown)");
    }
    out.refzoom = out.maxzoom;
}

static bool mvt_map_read_header_meta(const String &dir_path, FMapMeta &out) {
    // Try to read <dir>/metadata.json for name/bounds; fall back to dir name.
    int slash = dir_path.lastIndexOf('/');
    String dirname = (slash >= 0) ? dir_path.substring(slash + 1) : dir_path;

    String s;
    File mj = SD_MMC.open((dir_path + "/metadata.json").c_str(), FILE_READ);
    if (mj) { s = mj.readString(); mj.close(); }
    mvt_meta_apply_json(s, dirname, out);
    MVT_LOG("[MAP] meta dir='%s' title='%s' bounds=[%.5f,%.5f .. %.5f,%.5f] "
            "minzoom=%u maxzoom=%u refzoom=%u\n",
            dir_path.c_str(), out.title.c_str(),
            (double)out.min_lon, (double)out.min_lat,
            (double)out.max_lon, (double)out.max_lat,
            (unsigned)out.minzoom, (unsigned)out.maxzoom, (unsigned)out.refzoom);
    return true;
}

// The bundle stores metadata.json's bytes, not a parsed struct: one parser, and
// a bake that drops zoom levels rewrites the JSON rather than adding a field the
// firmware would have to know about.
static bool mvt_meta_from_bundle(const WpBundleMap &bm, FMapMeta &out) {
    if (!bm.meta || !bm.meta_len) return false;
    // Copy out of flash by the *stated* length and terminate it here, so a
    // metadata blob that was never NUL-terminated cannot be read past its end.
    // 512 bytes is several times the JSON this ever holds; a longer one is cut
    // rather than trusted, and the parser is tolerant of a cut tail.
    char buf[512];
    uint32_t n = bm.meta_len;
    if (n > sizeof(buf) - 1) n = sizeof(buf) - 1;
    memcpy(buf, bm.meta, n);
    buf[n] = 0;
    String s(buf);                            // now a copy in RAM, not in flash
    mvt_meta_apply_json(s, String(bm.name), out);
    MVT_LOG("[MAP] bundle '%s' title='%s' minzoom=%u maxzoom=%u\n",
            bm.name, out.title.c_str(), (unsigned)out.minzoom, (unsigned)out.maxzoom);
    return true;
}

// Count tile files under one zoom dir (small helper for library metadata).
static uint32_t count_tiles_at_zoom(const String &dir_path, uint8_t z) {
    if (g_map_bundle >= 0) {
        const WpBundle *B = wp_map_bundle();
        return B ? wp_bundle_zoom_tiles(B, g_map_bundle, z) : 0;
    }
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
    g_map_bundle = -1;          // the next open says where ITS tiles come from
    g_map_bundle_slot = -1;
    poi_reset();
    g_zoom_count = 0;
    g_tile_count = 0;
    for (int i=0; i<LAY__COUNT; i++) g_styles[i].set = false;
    cache_reset();
    // The negative probe cache is keyed on (z,x,y) alone, so an entry recorded
    // against one map is a lie about the next one: open a map, pan where it has
    // no tiles, switch to a map that does, and those tiles stay invisible until
    // the 64-slot ring happens to reuse them. tile_miss_clear() was written for
    // exactly this and never called. mvt_map_open() always closes first, so this
    // is the one place that has to run.
    tile_miss_clear();
    g_last_cached_z = -1;
}

// One rung of the zoom ladder: world-units per pixel where that zoom is
// "preferred", i.e. one MVT tile should render at ~256 screen pixels. Both
// openers (card and bundle) build their ladder through this, so a bundled map
// and a card map holding the same zooms behave identically.
static FMapZoom mvt_zoom_record(uint8_t z, uint32_t tiles) {
    FMapZoom zr;
    zr.zoom_id = z;
    int shift = g_meta.refzoom - z;
    double world_side = (double)MVT_EXTENT_DEFAULT *
                        ((shift >= 0) ? (double)(1u << shift)
                                      : 1.0 / (double)(1u << -shift));
    zr.min_px_scale = (float)(world_side / 256.0);
    zr.object_count = tiles;
    return zr;
}

// The tail both openers share: metres per world unit at the map's own latitude,
// the centre a recentre starts from, and the flag the draw path checks.
static void mvt_map_finish_open() {
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
        g_zooms[g_zoom_count++] = mvt_zoom_record(z, tc);
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

    mvt_map_finish_open();
    return true;
}

// Open a bundled map. sd_slot says WHICH pack (-1 = the one baked into the
// firmware, >= 0 = a .wpb on the card); idx is the map inside it. Everything
// after "where do the tiles come from" is the same as a folder map, so it is
// the same code: the ladder comes from the bundle index instead of from probing
// folders, and the projection tail is shared.
static bool mvt_map_open_bundle(int sd_slot, int idx) {
    mvt_map_close();
    const WpBundle *B = wp_bundle_of(sd_slot);
    WpBundleMap bm;
    if (!B || !wp_bundle_map(B, idx, &bm)) return false;
    if (!mvt_meta_from_bundle(bm, g_meta)) return false;
    g_map_dir_path = String();
    g_map_bundle     = idx;
    g_map_bundle_slot = (int8_t)sd_slot;
    g_zoom_count = 0;
    g_tile_count = 0;
    for (uint8_t z = 0; z <= 22; z++) {
        uint32_t tc = wp_bundle_zoom_tiles(B, idx, z);
        if (!tc) continue;
        if (g_zoom_count >= MAX_ZOOMS) {
            MVT_LOG("[MAP] bundle zoom list full at MAX_ZOOMS=%u - z%u ignored\n",
                    (unsigned)MAX_ZOOMS, (unsigned)z);
            break;
        }
        g_zooms[g_zoom_count++] = mvt_zoom_record(z, tc);
        g_tile_count += tc;
    }
    if (g_zoom_count == 0) {
        MVT_LOG("[MAP] bundle map %d holds no tiles -- aborting open\n", idx);
        g_map_bundle = -1;
        g_map_bundle_slot = -1;
        return false;
    }
    MVT_LOG("[MAP] %s '%s': %u zooms, %u tiles\n",
            sd_slot >= 0 ? "card pack" : "built-in", bm.name,
            (unsigned)g_zoom_count, (unsigned)g_tile_count);
    mvt_map_finish_open();
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

// Where a recentre wants the camera, WITHOUT moving it: the centre of the map's
// declared bounds, at the middle of the zoom ladder, snapped to a level that
// exists on disk. Split out of cam_reset_to_bounds() so the UI can animate a
// move to it - and so the animated destination and the instantaneous one are
// one calculation rather than two that drift apart.
static void cam_bounds_target(double *tx, double *ty, float *tscale,
                              uint8_t *tzoom) {
    // Centre on the map's lat/lon centre (Web Mercator, world +Y = north).
    double lat_c = ((double)g_meta.min_lat + (double)g_meta.max_lat) * 0.5;
    double lon_c = ((double)g_meta.min_lon + (double)g_meta.max_lon) * 0.5;
    if (g_meta.min_lat == 0 && g_meta.max_lat == 0) {
        // Unknown bounds: park camera on the middle discovered tile at midzoom.
        uint8_t z = g_zoom_count ? g_zooms[g_zoom_count/2].zoom_id : g_meta.refzoom;
        int shift = g_meta.refzoom - z;
        double side = (double)MVT_EXTENT_DEFAULT *
                      ((shift >= 0) ? (double)(1u << shift) : 1.0/(double)(1u << -shift));
        *tx     = side * 0.5;
        *ty     = -side * 0.5;
        *tscale = (float)(256.0 / side);
        *tzoom  = z;
        return;
    }
    // Choose the zoom = clamp((minzoom+maxzoom)/2, minzoom, maxzoom), then snap
    // to the closest discovered level. For the user's Perth export (minzoom=10,
    // maxzoom=16) this lands at z13 -- inside the tiny bounds where tiles exist.
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
    *tx     = mercator_lon_to_world_x(lon_c, g_meta.refzoom);
    *ty     = mercator_lat_to_world_y(lat_c, g_meta.refzoom);
    *tscale = (float)(256.0 / side);
    *tzoom  = z;
}

static void cam_reset_to_bounds() {
    double tx, ty; float ts; uint8_t tz;
    cam_bounds_target(&tx, &ty, &ts, &tz);
    g_cam_x = tx; g_cam_y = ty; g_cam_scale = ts; g_cam_zoom = tz;
    MVT_LOG("[CAM] reset to bounds: world=%.1f,%.1f z=%u scale=%.6f "
            "(minzoom=%u maxzoom=%u)\n",
            tx, ty, (unsigned)tz, ts,
            (unsigned)g_meta.minzoom, (unsigned)g_meta.maxzoom);
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
    // Round the joints on every road wide enough for a notch to show. At 3 px
    // a kink used to leave a nick in an otherwise solid line, which is exactly
    // what a map is not supposed to look like.
    if (w >= 3) {
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
    // Which slot of the recent-maps ring this map holds (-1 = not recent), read
    // once during the scan so the shelf and the card can say "recent" without
    // asking NVS again on every redraw.
    int8_t recent_rank = -1;
    // Built-in maps: -1 means the tiles live on the card as loose folders.
    // Anything else is the map's index in a PACK, and it cannot be removed from
    // the library because it is the pack itself.
    int8_t bundle_id = -1;
    bool   builtin   = false;
    // A .wpb on the card: which pack slot, or -1 when the tiles are loose files
    // in a folder (or the pack is the one baked into the firmware).
    int8_t sd_slot   = -1;
    // Listed but unreadable: the file was there and its bytes were not a pack
    // this firmware understands. The card says so rather than hiding it.
    bool   broken    = false;
    // A thumbnail, already painted, sitting in flash. 0 = none: a map on the card
    // has no picture unless its pack ships one, and the card simply loses the
    // band rather than showing an empty box.
    const uint8_t *preview = nullptr;
    uint16_t       preview_len = 0;
};
static MapEntry g_maps[MAX_MAPS];
static int g_map_count_lib = 0;
static int g_map_sel = 0;
static bool g_lib_redraw = true;
static bool g_lib_info   = false;   // details panel: swipe up / long press

// =============================================================================
//  Library engine  (WP_LIB_ENGINE)
//
//  The library is the first screen with a decision on it, and every part of that
//  decision is arithmetic: where the card and the shelf sit, which map a touch
//  landed on, how a long title is shortened, how a description is broken into
//  lines that fit the card, and where a map's zoom range sits on the detail
//  ladder. Keeping it here - plain numbers and char buffers, no LVGL, no panel,
//  no SD - is what lets tools/host_test assert the screen that decides which map
//  you open instead of trusting it. gen_engine.py enforces the boundary: teach
//  this region about a display and the host build stops.
// =============================================================================
// ===== BEGIN WP_LIB_ENGINE
#define WP_LIB_CHAR_W     6      // panel font at size 1: 6 x 8 px per character
#define WP_LIB_CHAR_H     8
#define WP_LIB_CARD_R    22      // same slab radius as every LVGL card
#define WP_LIB_CHIP_PAD  10      // horizontal padding inside a shelf chip
#define WP_LIB_CHIP_MIN  64      // a chip narrower than this cannot be read
#define WP_LIB_SHELF_GAP  6
#define WP_LIB_LADDER_LO  0      // the zoom ladder the detail bar is drawn against
#define WP_LIB_LADDER_HI 22

// Every rect on the screen, derived from the panel size so that the painter and
// the hit test cannot disagree about where anything is.
struct WpLibLayout {
    int pill_x, pill_y, pill_w, pill_h;
    int shelf_x, shelf_y, shelf_w, shelf_h;
    int card_x, card_y, card_w, card_h;
    int row_y, row_h;                  // the "3 / 7" position plate
    int hint_y, hint_line;             // first hint line and the leading under it
    int arrow_w, arrow_h;              // the plates either side of the card
};

// The strip of tabs above the card: which maps it shows, and how wide each chip
// is. A window rather than the whole shelf, because a library of 30 maps has no
// room for 30 tabs.
struct WpLibShelf {
    int first, slots, chip_w;
    int x, w, gap;
};

static void wp_lib_layout(int sw, int sh, WpLibLayout *o) {
    o->pill_x = 4;  o->pill_y = 3;
    o->pill_w = sw - 8;  o->pill_h = 26;
    o->shelf_x = 10;  o->shelf_y = o->pill_y + o->pill_h + 8;
    o->shelf_w = sw - 20;  o->shelf_h = 26;
    o->hint_line = WP_LIB_CHAR_H + 6;
    o->hint_y = sh - 10 - 4 * o->hint_line;
    o->row_h = 22;
    o->card_x = 16;
    o->card_y = o->shelf_y + o->shelf_h + 10;
    o->card_w = sw - 32;
    o->card_h = o->hint_y - 12 - (o->row_h + 6) - o->card_y;
    if (o->card_h < 120) o->card_h = 120;   // never a card too small to read
    o->row_y = o->card_y + o->card_h + 6;
    o->arrow_w = 30;  o->arrow_h = 44;
}

// Step the selection by dir (+1 / -1), wrapping. An empty library stays at 0 and
// a one-map library never moves: there is nothing else to show.
static int wp_lib_step(int sel, int count, int dir) {
    if (count <= 1) return 0;
    int s = sel % count;
    if (s < 0) s += count;
    s += (dir >= 0) ? 1 : -1;
    if (s < 0) s = count - 1;
    if (s >= count) s = 0;
    return s;
}

// How many chips fit across w at a readable width: never more than the library
// holds, never fewer than one.
static int wp_lib_slots(int count, int w, int gap) {
    if (count <= 0) return 0;
    int fit = (w + gap) / (WP_LIB_CHIP_MIN + gap);
    if (fit < 1) fit = 1;
    return (fit < count) ? fit : count;
}

// The leftmost chip shown. The window slides so the selection sits in the middle
// of the strip when it can, and stops at the ends so the shelf is never half
// empty at one side.
static int wp_lib_first(int count, int sel, int slots) {
    if (count <= 0 || slots <= 0) return 0;
    if (count <= slots) return 0;
    int f = sel - slots / 2;
    if (f < 0) f = 0;
    if (f > count - slots) f = count - slots;
    return f;
}

static void wp_lib_shelf(int count, int sel, int x, int w, int gap,
                         WpLibShelf *o) {
    o->x = x;  o->w = w;  o->gap = gap;
    o->slots = wp_lib_slots(count, w, gap);
    o->first = wp_lib_first(count, sel, o->slots);
    o->chip_w = (o->slots > 0) ? (w - gap * (o->slots - 1)) / o->slots : 0;
}

static int wp_lib_chip_x(const WpLibShelf *s, int i) {
    return s->x + i * (s->chip_w + s->gap);
}

// How many characters a chip that wide can hold.
static int wp_lib_chip_chars(const WpLibShelf *s) {
    int room = s->chip_w - 2 * WP_LIB_CHIP_PAD;
    if (room < WP_LIB_CHAR_W) return 0;
    return room / WP_LIB_CHAR_W;
}

// Shorten s to max_chars: the whole string when it fits, otherwise a cut closed
// with '~' so that a shortened title never passes for the real name. Always
// NUL-terminates, and never writes past cap bytes. Returns what it wrote.
static int wp_lib_fold(const char *s, char *out, int cap, int max_chars) {
    if (!out || cap <= 0) return 0;
    out[0] = 0;
    if (!s || max_chars < 1) return 0;
    int n = (int)strlen(s);
    if (n <= max_chars) {
        int k = (n < cap - 1) ? n : cap - 1;
        memcpy(out, s, (size_t)k);  out[k] = 0;
        return k;
    }
    int k = (max_chars < cap - 1) ? max_chars : cap - 1;
    if (k < 2) return 0;
    memcpy(out, s, (size_t)(k - 1));
    out[k - 1] = '~';  out[k] = 0;
    return k;
}

// Pull the next line of a wrapped paragraph out of s, starting at `from`. Breaks
// at a space where there is one, hard-breaks a word longer than the line so the
// loop can never spin, and returns the offset to carry on from (-1 = finished).
// Whatever a break eats is only the space it broke on: the host test reassembles
// the lines and compares them with the source, so a description cannot lose a
// character on its way to a 410 px panel.
static int wp_lib_wrap_line(const char *s, int from, int max_chars,
                            char *out, int cap) {
    if (!out || cap <= 0) return -1;
    out[0] = 0;
    if (!s || max_chars < 1) return -1;
    const int n = (int)strlen(s);
    while (from < n && s[from] == ' ') from++;
    if (from >= n) return -1;
    const int room = n - from;
    if (room <= max_chars) {
        int k = (room < cap - 1) ? room : cap - 1;
        memcpy(out, s + from, (size_t)k);  out[k] = 0;
        return -1;
    }
    int br = max_chars;
    while (br > 0 && s[from + br] != ' ') br--;
    if (br < 1) br = max_chars;          // one word, longer than the whole line
    int k = (br < cap - 1) ? br : cap - 1;
    memcpy(out, s + from, (size_t)k);  out[k] = 0;
    int next = from + br;
    while (next < n && s[next] == ' ') next++;
    return (next >= n) ? -1 : next;
}

// Where a zoom level sits on the ladder, 0..1, clamped.
static float wp_lib_ladder_frac(int z) {
    float f = (float)(z - WP_LIB_LADDER_LO) /
              (float)(WP_LIB_LADDER_HI - WP_LIB_LADDER_LO);
    if (f < 0.0f) f = 0.0f;
    if (f > 1.0f) f = 1.0f;
    return f;
}

// The span a map covers, in pixels across px: the bar that replaces "z12-z16"
// with something you can see the shape of. A one-level map still gets a visible
// bar rather than a zero-width nothing.
static void wp_lib_ladder_span(int zmin, int zmax, int px, int *x0, int *x1) {
    if (px < 2) px = 2;
    float f0 = wp_lib_ladder_frac(zmin);
    float f1 = wp_lib_ladder_frac(zmax);
    if (f1 < f0) { float tf = f0; f0 = f1; f1 = tf; }
    int a = (int)(f0 * (float)px + 0.5f);
    int b = (int)(f1 * (float)px + 0.5f);
    if (a < 0) a = 0;
    if (a > px - 1) a = px - 1;
    if (b > px) b = px;
    if (b <= a) b = a + 1;
    *x0 = a;  *x1 = b;
}

// What the card says in its corner: recency first (it is the reason to open this
// one), then a missing metadata.json, which the card admits rather than hides.
// Returns what it wrote; an empty string means nothing needed saying.
static int wp_lib_badge(int recent_rank, int has_meta, char *out, int cap) {
    if (!out || cap <= 0) return 0;
    const char *s = (recent_rank >= 0) ? "recent"
                  : (!has_meta)         ? "no metadata"
                                        : "";
    int n = (int)strlen(s);
    if (n > cap - 1) n = cap - 1;
    memcpy(out, s, (size_t)n);  out[n] = 0;
    return n;
}

// What a touch landed on. The rects come from the same layout the painter uses,
// so the plate that looks like an arrow is the plate that steps the carousel.
#define WP_LIB_HIT_NONE   0
#define WP_LIB_HIT_PREV   1
#define WP_LIB_HIT_NEXT   2
#define WP_LIB_HIT_OPEN   3
#define WP_LIB_HIT_SHELF  4

static int wp_lib_hit(int x, int y, int count, const WpLibLayout *L,
                      const WpLibShelf *S, int *chip) {
    if (chip) *chip = -1;
    if (S && S->slots > 0 && S->chip_w > 0 &&
        y >= L->shelf_y && y < L->shelf_y + L->shelf_h &&
        x >= S->x && x < S->x + S->w) {
        const int rel  = x - S->x;
        const int step = S->chip_w + S->gap;
        const int i    = rel / step;     // the gap between two chips has no map
        if (i >= 0 && i < S->slots && rel - i * step < S->chip_w) {
            if (chip) *chip = S->first + i;
            return WP_LIB_HIT_SHELF;
        }
        return WP_LIB_HIT_NONE;
    }
    if (x < L->card_x || x >= L->card_x + L->card_w ||
        y < L->card_y || y >= L->card_y + L->card_h) return WP_LIB_HIT_NONE;
    if (count > 1) {                     // with one map there is nowhere to step
        if (x < L->card_x + L->arrow_w) return WP_LIB_HIT_PREV;
        if (x >= L->card_x + L->card_w - L->arrow_w) return WP_LIB_HIT_NEXT;
    }
    return WP_LIB_HIT_OPEN;
}

// The same map can be in the app *and* in a folder on the card: the baked copy
// is there for a watch with no card, the folder is usually the full pack. Both
// would be listed under one name, which reads as a duplicate rather than as a
// choice, so the card's copy replaces the baked one -- but only when it is the
// same map *and* covers at least the zoom range the app has. A narrower folder
// (a card holding just the street levels, say) is not a replacement for the
// baked map, and dropping the baked one for it would lose zooms the watch can
// already draw.
static bool wp_lib_card_covers(bool same_map, int app_zmin, int app_zmax,
                               int card_zmin, int card_zmax) {
    if (!same_map) return false;
    return card_zmin <= app_zmin && card_zmax >= app_zmax;
}
// ===== END WP_LIB_ENGINE

// Detect if a directory looks like an MVT tile-tree (contains at least one// numeric zoom folder holding a tile file). Cheap check: probe a handful of
// zoom levels for existence of {dir}/{z}/.
static bool dir_looks_like_mvt_map(const String &dir_path) {
    for (uint8_t z = 0; z <= 22; z++) {
        char zb[16]; snprintf(zb, sizeof(zb), "/%u", (unsigned)z);
        if (SD_MMC.exists((dir_path + zb).c_str())) return true;
    }
    return false;
}

// One library card per map inside a card pack -- and, when the pack would not
// parse, one card that says so. A file the user put on the card deserves to be
// seen either way: hiding an unreadable WA.wpb is how a broken export looks
// exactly like no file at all.
static bool wp_sd_bundle_entries(int slot) {
    WpSdBundle &s = g_sd_bundles[slot];
    String file = s.path;
    int slash = file.lastIndexOf('/');
    if (slash >= 0) file = file.substring(slash + 1);

    if (!s.ok) {
        if (g_map_count_lib >= MAX_MAPS) return false;
        MapEntry &e = g_maps[g_map_count_lib++];
        e.path   = s.path;
        e.title  = file;
        char why[300];
        const unsigned kb = (unsigned)((s.size + 1023u) / 1024u);
        const char *reason = s.why ? s.why : "unknown reason";
        // The WPB1 advice only helps when the bytes are not a pack. A file that
        // IS a pack but could not be opened -- too large, index damaged, no RAM
        // for its index -- must not be sent off to be re-exported, which is what
        // this message used to do to a perfectly good pack that was simply big.
        const char *hint = (s.why && strstr(s.why, "does not begin"))
            ? "A map pack is one file that starts with the four bytes WPB1 - "
              "re-export it, or copy a folder of {z}/{x}/{y}.pbf tiles instead."
            : "It is a map pack, but this build cannot open it as it is. The "
              "Developer screen lists every pack it scanned, and why it failed.";
        if (s.size)
            snprintf(why, sizeof(why),
                     "%u KB on the card, and it could not be opened: %s.\n\n%s",
                     kb, reason, hint);
        else
            snprintf(why, sizeof(why),
                     "This file on the card could not be opened: %s.\n\n%s",
                     reason, hint);
        e.desc   = why;
        e.has_meta = false;
        e.builtin = false;
        e.sd_slot = -1;
        e.bundle_id = -1;
        e.broken = true;
        e.preview = nullptr;
        e.preview_len = 0;
        e.recent_rank = -1;
        e.zmin = 10; e.zmax = 17;
        e.min_lat = e.min_lon = e.max_lat = e.max_lon = 0;
        return false;
    }

    int added = 0;
    for (int i = 0; i < wp_bundle_map_count(&s.b) && g_map_count_lib < MAX_MAPS; i++) {
        WpBundleMap bm;
        if (!wp_bundle_map(&s.b, i, &bm)) continue;
        FMapMeta m;
        if (!mvt_meta_from_bundle(bm, m)) continue;
        MapEntry &e = g_maps[g_map_count_lib];
        // The '#index' distinguishes two maps inside one pack for the recent
        // ring, and everything that SHOWS the path strips it again.
        e.path   = s.path + "#" + String(i);
        e.title  = m.title.length() ? m.title : String(bm.name);
        e.desc   = m.desc.length()  ? m.desc
                 : String("Map pack on the card (") + file + ")";
        e.zmin = m.minzoom; e.zmax = m.maxzoom;
        e.min_lat = m.min_lat; e.min_lon = m.min_lon;
        e.max_lat = m.max_lat; e.max_lon = m.max_lon;
        e.has_meta    = true;
        e.builtin     = false;
        e.sd_slot     = (int8_t)slot;
        e.bundle_id   = (int8_t)i;
        e.broken      = false;
        e.preview     = bm.preview;
        e.preview_len = bm.preview_len;
        e.recent_rank = -1;
        g_map_count_lib++;
        added++;
    }
    return added > 0;
}

static void library_scan() {
    g_map_count_lib = 0;
    // Packs are read once per scan. Dropping them here first means a second
    // scan cannot keep pointing at bytes that were freed by the first.
    wp_sd_bundles_reset();

    // Built-in maps come first, and they are listed even when the card is absent
    // or empty: they are the firmware's own library, not a copy of one.
    for (int i = 0; i < wp_bundle_map_count(&g_bundle) && g_map_count_lib < MAX_MAPS; i++) {
        WpBundleMap bm;
        if (!wp_bundle_map(&g_bundle, i, &bm)) continue;
        FMapMeta m;
        if (!mvt_meta_from_bundle(bm, m)) continue;
        MapEntry &e = g_maps[g_map_count_lib];
        e.path   = String("@bundle/") + bm.name;
        e.title  = m.title.length() ? m.title : String(bm.name);
        e.desc   = m.desc.length() ? m.desc : String("Built-in vector map");
        e.zmin   = m.minzoom;
        e.zmax   = m.maxzoom;
        e.min_lat = m.min_lat; e.min_lon = m.min_lon;
        e.max_lat = m.max_lat; e.max_lon = m.max_lon;
        e.has_meta    = true;
        e.recent_rank = -1;
        e.bundle_id   = (int8_t)i;
        e.builtin     = true;
        e.preview     = bm.preview;
        e.preview_len = bm.preview_len;
        g_map_count_lib++;
    }

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
                        // A card map is not in the app and has no baked picture,
                        // said explicitly: an entry reused by a later scan must
                        // not keep a pointer into flash from an earlier one.
                        e.bundle_id   = -1;
                        e.builtin     = false;
                        e.sd_slot     = -1;
                        e.broken      = false;
                        e.preview     = nullptr;
                        e.preview_len = 0;
                        e.recent_rank = -1;
                        g_map_count_lib++;
                    }
                }
            }
        } else {
            // A pack on the card: /WATCH/WayPoint/<name>.wpb. The scan used to
            // walk only directories, so a .wpb dropped beside them was never
            // listed -- which is why WA.wpb did not exist as far as the
            // library was concerned.
            String n = ent.name();
            int p = n.lastIndexOf('/'); if (p >= 0) n = n.substring(p + 1);
            String ext;
            int dot = n.lastIndexOf('.');
            if (dot >= 0) { ext = n.substring(dot); ext.toLowerCase(); }
            if (ext == ".wpb" && n.length() && n[0] != '.') {
                const int slot = wp_sd_bundle_load(g_map_folder + "/" + n);
                if (slot >= 0) wp_sd_bundle_entries(slot);
            }
        }
        ent = d.openNextFile();
    }
    // A card that only holds packs still gets an open handle closed behind us.
    d.close();

    // The card can shadow the app. When the full pack is on the card as well as
    // the coarse copy in flash, both entries would carry the same name, and the
    // library would look like it had two of everything. The card copy wins --
    // it is the same map with more of it -- but only when it really does cover
    // the built-in's zoom range (wp_lib_card_covers), so a card holding just the
    // street levels keeps the baked map listed rather than replacing it.
    for (int i = 0; i < g_map_count_lib; i++) {
        if (!g_maps[i].builtin) continue;
        String folded = g_maps[i].path;                     // "@bundle/WA"
        if (folded.startsWith("@bundle/")) folded = folded.substring(8);
        bool shadowed = false;
        for (int j = 0; j < g_map_count_lib && !shadowed; j++) {
            if (g_maps[j].builtin) continue;
            String folder = g_maps[j].path;
            int sl = folder.lastIndexOf('/');
            if (sl >= 0) folder = folder.substring(sl + 1);
            const bool same = folder.equalsIgnoreCase(folded) ||
                              g_maps[j].title.equalsIgnoreCase(g_maps[i].title);
            shadowed = wp_lib_card_covers(same, g_maps[i].zmin, g_maps[i].zmax,
                                          g_maps[j].zmin, g_maps[j].zmax);
        }
        if (!shadowed) continue;
        for (int k = i; k < g_map_count_lib - 1; k++) g_maps[k] = g_maps[k + 1];
        g_map_count_lib--;
        i--;
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

    // Mark the ring, now that the order has settled: the card says "recent" and
    // the shelf keeps a gold dot on the map wherever it has been shuffled to,
    // which is the whole reason to have read the ring at all.
    for (int i = 0; i < g_map_count_lib; i++) g_maps[i].recent_rank = -1;
    for (int i = 0; i < 5; i++) {
        if (!recent[i].length()) continue;
        for (int j = 0; j < g_map_count_lib; j++) {
            if (g_maps[j].path == recent[i] && g_maps[j].recent_rank < 0) {
                g_maps[j].recent_rank = (int8_t)i;
                break;
            }
        }
    }

    // The list can shrink between scans (a baked map shadowed by the card, a
    // folder unplugged), and the selection is an index into it: clamp here, once,
    // rather than trusting every caller to have done it.
    if (g_map_sel < 0) g_map_sel = 0;
    if (g_map_sel >= g_map_count_lib) g_map_sel = g_map_count_lib ? g_map_count_lib - 1 : 0;
}

// =============================================================================
//  Library screen  (LVGL)
//
//  The library is the one screen whose whole job is to answer "which map?", and
//  it used to answer by painting the panel with Arduino_GFX: every rect, every
//  wrapped line and every chip placed by hand. That worked, and it could not
//  follow a theme, a font or a new card shape without all of that arithmetic
//  being redone by hand - which is how it drifted into looking like a different
//  app from the menus next to it.
//
//  It is LVGL now, like every other screen. The arithmetic that DECIDES things -
//  which map is selected, which chips are in the window, how a long title is
//  folded, where a zoom range sits on the ladder - stays in WP_LIB_ENGINE above,
//  because that is the part the host tests pin down. This region only turns
//  those answers into widgets.
//
//  The map itself is still drawn with Arduino_GFX (draw_map_frame). A vector
//  tile layer turned into LVGL objects would be slower and larger for no gain:
//  UI in LVGL, cartography in GFX. That split is deliberate, not a leftover.
// =============================================================================
#define WP_LIB_CHIPS    8       // widgets allocated; the shelf windows them
#define WP_LIB_ART_W   350      // thumbnail band inside the card
#define WP_LIB_ART_H    88

static lv_obj_t *scr_library  = nullptr;
static lv_obj_t *lib_chip[WP_LIB_CHIPS];
static lv_obj_t *lib_chip_lbl[WP_LIB_CHIPS];
static lv_obj_t *lib_card     = nullptr;   // the big slab
static lv_obj_t *lib_art      = nullptr;   // picture, or its placeholder
static lv_obj_t *lib_art_letter = nullptr;
static lv_obj_t *lib_img      = nullptr;
static lv_obj_t *lib_title    = nullptr;
static lv_obj_t *lib_badge    = nullptr;
static lv_obj_t *lib_sub      = nullptr;
static lv_obj_t *lib_ladder   = nullptr;   // real span, not two numbers
static lv_obj_t *lib_ladder_txt = nullptr;
static lv_obj_t *lib_desc     = nullptr;
static lv_obj_t *lib_count    = nullptr;
static lv_obj_t *lib_empty    = nullptr;
static lv_obj_t *lib_prev     = nullptr;
static lv_obj_t *lib_next     = nullptr;
static lv_obj_t *lib_open     = nullptr;
static lv_obj_t *lib_open_lbl = nullptr;
static lv_obj_t *lib_details  = nullptr;
static lv_obj_t *lib_exit     = nullptr;
static lv_obj_t *lib_toast    = nullptr;
static lv_obj_t *lib_hint     = nullptr;

static uint16_t      *lib_art_buf = nullptr;   // decoded thumbnail (PSRAM)
static lv_img_dsc_t   lib_art_dsc;
static uint32_t       lib_toast_until = 0;

static void library_refresh();
static void library_open_selected();

// Dark ink on a bright accent, light ink on a dark one. The screen background is
// one of the theme's two ends, so using it as the text colour keeps the primary
// button legible in every theme instead of assuming the accent is always dark.
static inline uint32_t lib_ink_on(uint16_t bg565) {
    return (wp_luma565(bg565) > 150) ? 0x000000 : 0xFFFFFF;
}

// One small rounded plate. Every secondary action on this screen is cut from
// this: same radius family as the card, same hairline border.
static lv_obj_t *lib_pill(lv_obj_t *parent, int w, int h) {
    lv_obj_t *b = lv_btn_create(parent);
    lv_obj_remove_style_all(b);
    lv_obj_set_size(b, w, h);
    lv_obj_set_style_radius(b, h / 2, 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(COL_CARD), 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(b, 1, 0);
    lv_obj_set_style_border_color(b, lv_color_hex(COL_LINE), 0);
    // Press feedback: the plate dims and tightens rather than moving, which on a
    // 410 px panel reads as a press instead of the layout jumping.
    lv_obj_set_style_bg_color(b, lv_color_hex(COL_ACCENT), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(b, LV_OPA_30, LV_STATE_PRESSED);
    lv_obj_set_style_transform_width(b, -1, LV_STATE_PRESSED);
    lv_obj_set_style_transform_height(b, -1, LV_STATE_PRESSED);
    return b;
}

static lv_obj_t *lib_pill_text(lv_obj_t *b, const char *txt, uint32_t col,
                               const lv_font_t *font) {
    lv_obj_t *l = lv_label_create(b);
    lv_label_set_text(l, txt);
    lv_obj_set_style_text_color(l, lv_color_hex(col), 0);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_center(l);
    return l;
}

// ---- callbacks -------------------------------------------------------------
static void cb_lib_prev(lv_event_t*) {
    g_map_sel = wp_lib_step(g_map_sel, g_map_count_lib, -1);
    g_lib_redraw = true;
}
static void cb_lib_next(lv_event_t*) {
    g_map_sel = wp_lib_step(g_map_sel, g_map_count_lib, +1);
    g_lib_redraw = true;
}
static void cb_lib_open(lv_event_t*)   { library_open_selected(); }
static void cb_lib_details(lv_event_t*) { g_lib_info = !g_lib_info; g_lib_redraw = true; }
static void cb_lib_exit(lv_event_t*)   { return_to_launcher(); }
static void cb_lib_chip(lv_event_t *e) {
    const intptr_t i = (intptr_t)lv_event_get_user_data(e);
    if (i >= 0 && i < g_map_count_lib) g_map_sel = (int)i;
    g_lib_redraw = true;
}

// ---- build -----------------------------------------------------------------
static void build_library() {
    if (scr_library) return;
    wp_sync_styles();

    scr_library = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr_library, lv_color_hex(COL_BG), 0);
    lv_obj_set_style_bg_opa(scr_library, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(scr_library, 0, 0);
    lv_obj_clear_flag(scr_library, LV_OBJ_FLAG_SCROLLABLE);

    // ---- header: what this screen is, how much of it there is, and the way out
    lv_obj_t *h = lv_label_create(scr_library);
    lv_label_set_text(h, "MAP LIBRARY");
    lv_obj_set_style_text_color(h, lv_color_hex(COL_MUTE), 0);
    lv_obj_set_style_text_letter_space(h, 3, 0);
    lv_obj_set_style_text_font(h, &lv_font_montserrat_12, 0);
    lv_obj_set_pos(h, 18, 12);

    lib_count = lv_label_create(scr_library);
    lv_label_set_text(lib_count, "");
    lv_obj_set_style_text_color(lib_count, lv_color_hex(COL_TXT), 0);
    lv_obj_set_style_text_font(lib_count, &lv_font_montserrat_12, 0);
    lv_obj_set_pos(lib_count, 136, 12);

    // Leaving for LauncherOS used to be a 1.5 s hold of the BOOT button. It is a
    // button now, in the corner, because the whole app is supposed to be reachable
    // with a finger.
    lib_exit = lib_pill(scr_library, 78, 30);
    lv_obj_align(lib_exit, LV_ALIGN_TOP_RIGHT, -14, 6);
    lib_pill_text(lib_exit, "EXIT", COL_MUTE, &lv_font_montserrat_12);
    lv_obj_add_event_cb(lib_exit, cb_lib_exit, LV_EVENT_CLICKED, NULL);

    // ---- shelf: the library as a strip of tabs -------------------------------
    // Not an LVGL scroller: the window is chosen by WP_LIB_ENGINE and stepped by
    // a swipe, so the shelf cannot scroll itself out from under the selection.
    for (int i = 0; i < WP_LIB_CHIPS; i++) {
        lv_obj_t *c = lib_pill(scr_library, 70, 32);
        lv_obj_add_flag(c, LV_OBJ_FLAG_HIDDEN);
        lib_chip[i] = c;
        lib_chip_lbl[i] = lib_pill_text(c, "", COL_MUTE, &lv_font_montserrat_12);
        lv_obj_add_event_cb(c, cb_lib_chip, LV_EVENT_CLICKED, (void*)(intptr_t)i);
    }

    // ---- the card ------------------------------------------------------------
    lib_card = lv_obj_create(scr_library);
    lv_obj_remove_style_all(lib_card);
    lv_obj_set_size(lib_card, LCD_WIDTH - 28, 276);
    lv_obj_set_pos(lib_card, 14, 92);
    lv_obj_set_style_radius(lib_card, 24, 0);
    lv_obj_set_style_bg_color(lib_card, lv_color_hex(COL_CARD), 0);
    lv_obj_set_style_bg_opa(lib_card, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(lib_card, 1, 0);
    lv_obj_set_style_border_color(lib_card, lv_color_hex(COL_LINE), 0);
    lv_obj_set_style_pad_all(lib_card, 0, 0);
    lv_obj_clear_flag(lib_card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(lib_card, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(lib_card, cb_lib_open, LV_EVENT_CLICKED, NULL);

    lib_art = lv_obj_create(lib_card);
    lv_obj_remove_style_all(lib_art);
    lv_obj_set_size(lib_art, WP_LIB_ART_W, WP_LIB_ART_H);
    lv_obj_set_pos(lib_art, 16, 16);
    lv_obj_set_style_radius(lib_art, 16, 0);
    lv_obj_set_style_bg_color(lib_art, lv_color_hex(COL_BG), 0);
    lv_obj_set_style_bg_opa(lib_art, LV_OPA_COVER, 0);
    lv_obj_set_style_clip_corner(lib_art, true, 0);
    lv_obj_clear_flag(lib_art, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(lib_art, LV_OBJ_FLAG_CLICKABLE);

    // A map with no baked picture still gets a face: its initial, in the hairline
    // colour. Hiding the band instead would make those maps look broken.
    lib_art_letter = lv_label_create(lib_art);
    lv_label_set_text(lib_art_letter, "");
    lv_obj_set_style_text_color(lib_art_letter, lv_color_hex(COL_LINE), 0);
    lv_obj_set_style_text_font(lib_art_letter, &lv_font_montserrat_48, 0);
    lv_obj_center(lib_art_letter);

    lib_img = lv_img_create(lib_art);
    lv_obj_set_pos(lib_img, 0, 0);
    lv_obj_clear_flag(lib_img, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(lib_img, LV_OBJ_FLAG_HIDDEN);

    lib_title = lv_label_create(lib_card);
    lv_label_set_text(lib_title, "");
    lv_obj_set_style_text_color(lib_title, lv_color_hex(COL_TXT), 0);
    lv_obj_set_style_text_font(lib_title, &lv_font_montserrat_28, 0);
    lv_obj_set_pos(lib_title, 16, 112);
    lv_obj_set_width(lib_title, 240);
    lv_label_set_long_mode(lib_title, LV_LABEL_LONG_DOT);

    // Recency or a missing metadata.json, said in the corner rather than in the
    // description: the reader wants the map first and its caveats second.
    lib_badge = lv_label_create(lib_card);
    lv_label_set_text(lib_badge, "");
    lv_obj_set_style_text_color(lib_badge, lv_color_hex(COL_GOLD), 0);
    lv_obj_set_style_text_font(lib_badge, &lv_font_montserrat_12, 0);
    lv_obj_align(lib_badge, LV_ALIGN_TOP_RIGHT, -16, 120);

    lib_sub = lv_label_create(lib_card);
    lv_label_set_text(lib_sub, "");
    lv_obj_set_style_text_color(lib_sub, lv_color_hex(COL_MUTE), 0);
    lv_obj_set_style_text_font(lib_sub, &lv_font_montserrat_12, 0);
    lv_obj_set_pos(lib_sub, 16, 148);
    lv_obj_set_width(lib_sub, WP_LIB_ART_W);
    lv_label_set_long_mode(lib_sub, LV_LABEL_LONG_DOT);

    // The zoom range as a span you can see the shape of. A one-level map still
    // gets a visible bar, because a zero-width bar is a rendering bug to a reader.
    lib_ladder = lv_bar_create(lib_card);
    lv_obj_set_size(lib_ladder, WP_LIB_ART_W, 8);
    lv_obj_set_pos(lib_ladder, 16, 186);
    lv_bar_set_range(lib_ladder, WP_LIB_LADDER_LO, WP_LIB_LADDER_HI);
    lv_obj_set_style_radius(lib_ladder, 4, LV_PART_MAIN);
    lv_obj_set_style_bg_color(lib_ladder, lv_color_hex(COL_BG), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(lib_ladder, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(lib_ladder, 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(lib_ladder, lv_color_hex(COL_LINE), LV_PART_MAIN);
    lv_obj_set_style_radius(lib_ladder, 4, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(lib_ladder, lv_color_hex(COL_ACCENT), LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(lib_ladder, LV_OPA_COVER, LV_PART_INDICATOR);

    lib_ladder_txt = lv_label_create(lib_card);
    lv_label_set_text(lib_ladder_txt, "");
    lv_obj_set_style_text_color(lib_ladder_txt, lv_color_hex(COL_MUTE), 0);
    lv_obj_set_style_text_font(lib_ladder_txt, &lv_font_montserrat_12, 0);
    lv_obj_align(lib_ladder_txt, LV_ALIGN_TOP_RIGHT, -16, 166);

    lib_desc = lv_label_create(lib_card);
    lv_label_set_text(lib_desc, "");
    lv_obj_set_style_text_color(lib_desc, lv_color_hex(COL_TXT), 0);
    lv_obj_set_style_text_font(lib_desc, &lv_font_montserrat_14, 0);
    lv_obj_set_pos(lib_desc, 16, 204);
    lv_obj_set_size(lib_desc, WP_LIB_ART_W, 60);
    lv_label_set_long_mode(lib_desc, LV_LABEL_LONG_DOT);

    // ---- the row under the card ---------------------------------------------
    lib_prev = lib_pill(scr_library, 52, 52);
    lv_obj_set_pos(lib_prev, 14, 376);
    lv_obj_set_style_radius(lib_prev, LV_RADIUS_CIRCLE, 0);
    lib_pill_text(lib_prev, LV_SYMBOL_LEFT, COL_TXT, &lv_font_montserrat_20);
    lv_obj_add_event_cb(lib_prev, cb_lib_prev, LV_EVENT_CLICKED, NULL);

    lib_next = lib_pill(scr_library, 52, 52);
    lv_obj_set_pos(lib_next, LCD_WIDTH - 14 - 52, 376);
    lv_obj_set_style_radius(lib_next, LV_RADIUS_CIRCLE, 0);
    lib_pill_text(lib_next, LV_SYMBOL_RIGHT, COL_TXT, &lv_font_montserrat_20);
    lv_obj_add_event_cb(lib_next, cb_lib_next, LV_EVENT_CLICKED, NULL);

    lib_open = lv_btn_create(scr_library);
    lv_obj_remove_style_all(lib_open);
    lv_obj_set_size(lib_open, LCD_WIDTH - 76 - 52, 52);
    lv_obj_set_pos(lib_open, 72, 376);
    lv_obj_set_style_radius(lib_open, 26, 0);
    lv_obj_set_style_bg_color(lib_open, lv_color_hex(COL_ACCENT), 0);
    lv_obj_set_style_bg_opa(lib_open, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_opa(lib_open, LV_OPA_80, LV_STATE_PRESSED);
    lib_open_lbl = lib_pill_text(lib_open, "OPEN MAP", lib_ink_on(TH().accent),
                                 &lv_font_montserrat_16);
    lv_obj_add_event_cb(lib_open, cb_lib_open, LV_EVENT_CLICKED, NULL);

    lib_details = lib_pill(scr_library, LCD_WIDTH - 28, 30);
    lv_obj_set_pos(lib_details, 14, 436);
    lib_pill_text(lib_details, "DETAILS", COL_MUTE, &lv_font_montserrat_12);
    lv_obj_add_event_cb(lib_details, cb_lib_details, LV_EVENT_CLICKED, NULL);

    // ---- empty state ---------------------------------------------------------
    lib_empty = lv_label_create(scr_library);
    lv_label_set_text(lib_empty,
        "No maps yet.\n\n"
        "Copy a map pack (.wpb) into /WATCH/WayPoint/, or a folder of\n"
        "{z}/{x}/{y}.pbf tiles, and it appears here.\n\n"
        "The folder is checked every time this screen opens.");
    lv_label_set_long_mode(lib_empty, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_color(lib_empty, lv_color_hex(COL_MUTE), 0);
    lv_obj_set_style_text_font(lib_empty, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_align(lib_empty, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(lib_empty, LCD_WIDTH - 70);
    lv_obj_align(lib_empty, LV_ALIGN_CENTER, 0, 10);
    lv_obj_add_flag(lib_empty, LV_OBJ_FLAG_HIDDEN);

    // ---- toast: a failure that stays on the screen it happened on -----------
    lib_toast = lv_label_create(scr_library);
    lv_label_set_long_mode(lib_toast, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(lib_toast, LCD_WIDTH - 90);
    lv_obj_set_style_text_align(lib_toast, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(lib_toast, lv_color_hex(0xF05040), 0);
    lv_obj_set_style_text_font(lib_toast, &lv_font_montserrat_14, 0);
    lv_obj_set_style_bg_color(lib_toast, lv_color_hex(COL_CARD), 0);
    lv_obj_set_style_bg_opa(lib_toast, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(lib_toast, 1, 0);
    lv_obj_set_style_border_color(lib_toast, lv_color_hex(0xF05040), 0);
    lv_obj_set_style_radius(lib_toast, 14, 0);
    lv_obj_set_style_pad_all(lib_toast, 10, 0);
    lv_obj_align(lib_toast, LV_ALIGN_BOTTOM_MID, 0, -120);
    lv_obj_add_flag(lib_toast, LV_OBJ_FLAG_HIDDEN);

    // ---- the gestures, in one line, because the map teaches them separately --
    lib_hint = lv_label_create(scr_library);
    lv_label_set_text(lib_hint, "swipe " LV_SYMBOL_LEFT LV_SYMBOL_RIGHT
                                " browse   " LV_SYMBOL_UP " details   "
                                LV_SYMBOL_DOWN " exit");
    lv_obj_set_style_text_color(lib_hint, lv_color_hex(COL_MUTE), 0);
    lv_obj_set_style_text_font(lib_hint, &lv_font_montserrat_12, 0);
    lv_obj_align(lib_hint, LV_ALIGN_BOTTOM_MID, 0, -8);
}

// ---- repaint ---------------------------------------------------------------
// Everything below only ever writes values into widgets that already exist: the
// refresh runs on every swipe, and rebuilding the screen there would churn the
// LVGL heap until it fragmented.
static void library_refresh() {
    if (!scr_library) build_library();
    g_lib_redraw = false;

    const ThemePalette &t = TH();
    const bool empty = (g_map_count_lib == 0);

    // Theme can change while a card is on screen, so the colours are re-applied
    // here rather than only at build time.
    lv_obj_set_style_bg_color(scr_library, lv_color_hex(COL_BG), 0);
    lv_obj_set_style_bg_color(lib_card, lv_color_hex(COL_CARD), 0);
    lv_obj_set_style_border_color(lib_card, lv_color_hex(COL_LINE), 0);
    lv_obj_set_style_bg_color(lib_art, lv_color_hex(COL_BG), 0);
    lv_obj_set_style_text_color(lib_art_letter, lv_color_hex(COL_LINE), 0);
    lv_obj_set_style_text_color(lib_title, lv_color_hex(COL_TXT), 0);
    lv_obj_set_style_text_color(lib_sub, lv_color_hex(COL_MUTE), 0);
    lv_obj_set_style_text_color(lib_badge, lv_color_hex(COL_GOLD), 0);
    lv_obj_set_style_text_color(lib_desc, lv_color_hex(COL_TXT), 0);
    lv_obj_set_style_text_color(lib_ladder_txt, lv_color_hex(COL_MUTE), 0);
    lv_obj_set_style_bg_color(lib_ladder, lv_color_hex(COL_BG), LV_PART_MAIN);
    lv_obj_set_style_border_color(lib_ladder, lv_color_hex(COL_LINE), LV_PART_MAIN);
    lv_obj_set_style_bg_color(lib_ladder, lv_color_hex(COL_ACCENT), LV_PART_INDICATOR);
    lv_obj_set_style_text_color(lv_obj_get_child(lib_details, 0),
                                lv_color_hex(COL_MUTE), 0);
    lv_obj_set_style_text_color(lib_count, lv_color_hex(COL_TXT), 0);

    char pos[32];
    snprintf(pos, sizeof(pos), (g_map_count_lib == 1) ? "%d map" : "%d maps",
             g_map_count_lib);
    lv_label_set_text(lib_count, pos);

    // The empty library has nothing to put on a card, so the card goes away and
    // the explanation takes its place - the same screen, not a second one.
    if (empty) {
        lv_obj_add_flag(lib_card, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(lib_open, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(lib_prev, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(lib_next, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(lib_details, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(lib_empty, LV_OBJ_FLAG_HIDDEN);
        for (int i = 0; i < WP_LIB_CHIPS; i++) lv_obj_add_flag(lib_chip[i], LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(lib_hint, "no map packs on the card");
        return;
    }
    lv_obj_clear_flag(lib_card, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(lib_empty, LV_OBJ_FLAG_HIDDEN);

    const MapEntry &e = g_maps[g_map_sel];
    const bool multi = (g_map_count_lib > 1);

    // ---- shelf --------------------------------------------------------------
    WpLibShelf S;
    wp_lib_shelf(g_map_count_lib, g_map_sel, 14, LCD_WIDTH - 28,
                 WP_LIB_SHELF_GAP, &S);
    for (int i = 0; i < WP_LIB_CHIPS; i++) {
        const bool used = (i < S.slots);
        if (!used) { lv_obj_add_flag(lib_chip[i], LV_OBJ_FLAG_HIDDEN); continue; }
        const int idx = S.first + i;
        if (idx >= g_map_count_lib) { lv_obj_add_flag(lib_chip[i], LV_OBJ_FLAG_HIDDEN); continue; }
        const bool on = (idx == g_map_sel);
        lv_obj_clear_flag(lib_chip[i], LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_pos(lib_chip[i], wp_lib_chip_x(&S, i), 50);
        lv_obj_set_size(lib_chip[i], S.chip_w, 32);
        lv_obj_set_style_radius(lib_chip[i], 16, 0);
        lv_obj_set_style_bg_color(lib_chip[i],
            lv_color_hex(on ? COL_ACCENT : COL_CARD), 0);
        lv_obj_set_style_border_color(lib_chip[i],
            lv_color_hex(on ? COL_ACCENT : COL_LINE), 0);
        char lbl[48];
        wp_lib_fold(g_maps[idx].title.c_str(), lbl, sizeof(lbl),
                    wp_lib_chip_chars(&S));
        lv_label_set_text(lib_chip_lbl[i], lbl);
        lv_obj_set_style_text_color(lib_chip_lbl[i],
            lv_color_hex(on ? lib_ink_on(t.accent) : COL_MUTE), 0);
    }

    // ---- the card -----------------------------------------------------------
    if (multi) {
        lv_obj_clear_flag(lib_prev, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(lib_next, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(lib_prev, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(lib_next, LV_OBJ_FLAG_HIDDEN);
    }
    lv_obj_clear_flag(lib_open, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(lib_details, LV_OBJ_FLAG_HIDDEN);

    // A pack this build could not read is not hidden and not silently retried:
    // the card is red, says why, and the OPEN button refuses instead of lying.
    lv_obj_set_style_border_color(lib_card,
        lv_color_hex(e.broken ? 0xF05040 : COL_LINE), 0);
    lv_obj_set_style_border_width(lib_card, e.broken ? 2 : 1, 0);

    lv_label_set_text(lib_title, e.title.c_str());
    char badge[24];
    if (e.broken)                   snprintf(badge, sizeof(badge), "unreadable");
    else if (e.sd_slot >= 0)        snprintf(badge, sizeof(badge), "map pack");
    else wp_lib_badge(e.recent_rank, e.has_meta ? 1 : 0, badge, sizeof(badge));
    lv_label_set_text(lib_badge, badge);
    lv_obj_set_style_text_color(lib_badge,
        lv_color_hex(e.broken ? 0xF05040
                   : e.recent_rank >= 0 ? COL_GOLD : COL_MUTE), 0);

    // Where the map comes from. A pack on the card names its file; a wildcard
    // path or the firmware's own index would be a debug string wearing a label.
    char sub[96];
    String base = e.path;
    int hash = base.indexOf('#');
    if (hash >= 0) base = base.substring(0, hash);
    int slash = base.lastIndexOf('/');
    if (slash >= 0) base = base.substring(slash + 1);
    if (e.broken)          snprintf(sub, sizeof(sub), "%s", base.c_str());
    else if (e.sd_slot >= 0) snprintf(sub, sizeof(sub), "pack  %s", base.c_str());
    else if (e.builtin)    snprintf(sub, sizeof(sub), "in the app  %s", base.c_str());
    else                   snprintf(sub, sizeof(sub), "dir  %s", base.c_str());
    lv_label_set_text(lib_sub, sub);

    // ---- thumbnail ----------------------------------------------------------
    bool art_ok = false;
    if (e.preview && e.preview_len) {
        if (!lib_art_buf) {
            lib_art_buf = (uint16_t*)ps_malloc((size_t)WP_LIB_ART_W * WP_LIB_ART_H *
                                              sizeof(uint16_t));
        }
        WpImg im;
        if (lib_art_buf && wp_img_open(&im, e.preview, e.preview_len) &&
            wp_img_decode(&im, lib_art_buf, WP_LIB_ART_W, WP_LIB_ART_H)) {
            if (!lib_art_dsc.data) {
                lib_art_dsc.header.always_zero = 0;
                lib_art_dsc.header.w = WP_LIB_ART_W;
                lib_art_dsc.header.h = WP_LIB_ART_H;
                lib_art_dsc.header.cf = LV_IMG_CF_TRUE_COLOR;
                lib_art_dsc.data_size = (uint32_t)WP_LIB_ART_W * WP_LIB_ART_H *
                                        sizeof(lv_color_t);
                lib_art_dsc.data = (const uint8_t*)lib_art_buf;
            }
            lv_img_set_src(lib_img, &lib_art_dsc);
            // The descriptor points at a buffer we rewrote in place, so LVGL has
            // no way to know the pixels changed: say so.
            lv_obj_invalidate(lib_img);
            art_ok = true;
        }
    }
    if (art_ok) {
        lv_obj_clear_flag(lib_img, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(lib_art_letter, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(lib_img, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(lib_art_letter, LV_OBJ_FLAG_HIDDEN);
        char initial[2] = { (char)toupper((unsigned char)e.title[0]), 0 };
        lv_label_set_text(lib_art_letter, initial);
    }

    // ---- the ladder ---------------------------------------------------------
    char zr[24];
    if (e.has_meta) snprintf(zr, sizeof(zr), "z%u-%u",
                             (unsigned)e.zmin, (unsigned)e.zmax);
    else            snprintf(zr, sizeof(zr), "range on open");
    lv_label_set_text(lib_ladder_txt, zr);
    if (e.has_meta && !e.broken) {
        lv_obj_clear_flag(lib_ladder, LV_OBJ_FLAG_HIDDEN);
        lv_bar_set_start_value(lib_ladder, (int16_t)e.zmin, LV_ANIM_OFF);
        lv_bar_set_value(lib_ladder, (int16_t)e.zmax, LV_ANIM_OFF);
    } else {
        lv_obj_add_flag(lib_ladder, LV_OBJ_FLAG_HIDDEN);
    }

    // ---- what the card says -------------------------------------------------
    // The primary button wears the refusal too: an accent-coloured "OPEN MAP"
    // over a card that cannot be opened is a button that lies about its state.
    if (e.broken) {
        lv_obj_set_style_bg_color(lib_open, lv_color_hex(COL_CARD), 0);
        lv_obj_set_style_border_width(lib_open, 1, 0);
        lv_obj_set_style_border_color(lib_open, lv_color_hex(0xF05040), 0);
        lv_obj_set_style_text_color(lib_open_lbl, lv_color_hex(0xF05040), 0);
    } else {
        lv_obj_set_style_bg_color(lib_open, lv_color_hex(COL_ACCENT), 0);
        lv_obj_set_style_border_width(lib_open, 0, 0);
        lv_obj_set_style_text_color(lib_open_lbl, lv_color_hex(lib_ink_on(t.accent)), 0);
    }

    if (e.broken) {
        lv_label_set_text(lib_desc, e.desc.c_str());
        lv_label_set_text(lib_open_lbl, "NOT A MAP PACK");
    } else if (g_lib_info) {
        // The details are "where this really is", which for a baked map is the
        // firmware and not a path - and for a card pack is the file plus the map
        // inside it, because one pack can hold several.
        char info[220];
        int n = snprintf(info, sizeof(info), "%s\n", e.path.c_str());
        if (e.has_meta && (e.min_lat != 0 || e.max_lat != 0)) {
            n += snprintf(info + n, sizeof(info) - (size_t)n,
                          "bounds %.4f,%.4f to %.4f,%.4f",
                          (double)e.min_lon, (double)e.min_lat,
                          (double)e.max_lon, (double)e.max_lat);
        } else if (!e.has_meta) {
            snprintf(info + n, sizeof(info) - (size_t)n,
                     "no metadata.json, so the folder name is the title");
        }
        lv_label_set_text(lib_desc, info);
        lv_label_set_text(lib_open_lbl, "OPEN MAP");
    } else {
        lv_label_set_text(lib_desc, e.desc.c_str());
        lv_label_set_text(lib_open_lbl, "OPEN MAP");
    }

    if (multi) {
        snprintf(pos, sizeof(pos), "%d / %d", g_map_sel + 1, g_map_count_lib);
        lv_label_set_text(lib_count, pos);
    }
    lv_label_set_text(lib_hint, g_lib_info
        ? "swipe " LV_SYMBOL_DOWN " hide the details"
        : "swipe " LV_SYMBOL_LEFT LV_SYMBOL_RIGHT " browse   "
          LV_SYMBOL_UP " details   " LV_SYMBOL_DOWN " exit");
}

// A failed open reports itself on the card that failed. Nothing else on the
// screen moves, so the next tap is unambiguous.
static void library_open_failed() {
    if (!scr_library) build_library();
    lv_label_set_text(lib_toast, "Could not open that map.\n"
                                 "Check the pack, or the tile folders inside it.");
    lv_obj_clear_flag(lib_toast, LV_OBJ_FLAG_HIDDEN);
    lib_toast_until = millis() + 3200;
    g_lib_redraw = true;
}

static void library_tick(uint32_t now) {
    if (lib_toast_until && (int32_t)(now - lib_toast_until) >= 0) {
        lib_toast_until = 0;
        if (lib_toast) lv_obj_add_flag(lib_toast, LV_OBJ_FLAG_HIDDEN);
    }
}

// =============================================================================
//  Splash
//
//  The first screen, and the only one that runs before the library exists. It is
//  an LVGL screen like every other so the boot looks like the app it is booting
//  into, and it carries a real progress bar: setup() reports each stage here
//  instead of the panel showing a hand-drawn frame loop and then a hard cut.
// =============================================================================
static lv_obj_t *scr_splash       = nullptr;
static lv_obj_t *lbl_splash_stage = nullptr;
static lv_obj_t *bar_splash       = nullptr;

static void splash_begin() {
    if (scr_splash) return;
    scr_splash = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr_splash, lv_color_hex(COL_BG), 0);
    lv_obj_set_style_bg_opa(scr_splash, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(scr_splash, 0, 0);
    lv_obj_clear_flag(scr_splash, LV_OBJ_FLAG_SCROLLABLE);

    // The mark: a GPS arrow in the accent colour. Drawn from a font glyph so it
    // costs nothing to draw and follows the theme, which a baked bitmap would not.
    lv_obj_t *mark = lv_label_create(scr_splash);
    lv_label_set_text(mark, LV_SYMBOL_GPS);
    lv_obj_set_style_text_color(mark, lv_color_hex(COL_ACCENT), 0);
    lv_obj_set_style_text_font(mark, &lv_font_montserrat_48, 0);
    lv_obj_align(mark, LV_ALIGN_TOP_MID, 0, 96);

    lv_obj_t *word = lv_label_create(scr_splash);
    lv_label_set_text(word, "WAYPOINT");
    lv_obj_set_style_text_color(word, lv_color_hex(COL_TXT), 0);
    lv_obj_set_style_text_font(word, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_letter_space(word, 2, 0);
    lv_obj_align(word, LV_ALIGN_TOP_MID, 0, 190);

    lv_obj_t *os = lv_label_create(scr_splash);
    lv_label_set_text(os, "O S");
    lv_obj_set_style_text_color(os, lv_color_hex(COL_ACCENT), 0);
    lv_obj_set_style_text_font(os, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_letter_space(os, 7, 0);
    lv_obj_align(os, LV_ALIGN_TOP_MID, 0, 226);

    lv_obj_t *tag = lv_label_create(scr_splash);
    lv_label_set_text(tag, "offline vector nav");
    lv_obj_set_style_text_color(tag, lv_color_hex(COL_MUTE), 0);
    lv_obj_set_style_text_font(tag, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_letter_space(tag, 2, 0);
    lv_obj_align(tag, LV_ALIGN_TOP_MID, 0, 258);

    bar_splash = lv_bar_create(scr_splash);
    lv_obj_set_size(bar_splash, 220, 6);
    lv_obj_align(bar_splash, LV_ALIGN_TOP_MID, 0, 330);
    lv_bar_set_range(bar_splash, 0, 100);
    lv_bar_set_value(bar_splash, 0, LV_ANIM_OFF);
    lv_obj_set_style_radius(bar_splash, 3, LV_PART_MAIN);
    lv_obj_set_style_bg_color(bar_splash, lv_color_hex(COL_CARD), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(bar_splash, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(bar_splash, 3, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(bar_splash, lv_color_hex(COL_ACCENT), LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(bar_splash, LV_OPA_COVER, LV_PART_INDICATOR);

    lbl_splash_stage = lv_label_create(scr_splash);
    lv_label_set_text(lbl_splash_stage, "starting");
    lv_obj_set_style_text_color(lbl_splash_stage, lv_color_hex(COL_MUTE), 0);
    lv_obj_set_style_text_font(lbl_splash_stage, &lv_font_montserrat_12, 0);
    lv_obj_align(lbl_splash_stage, LV_ALIGN_TOP_MID, 0, 344);

    lv_obj_t *foot = lv_label_create(scr_splash);
    lv_label_set_text(foot, "swipe  tap  hold");
    lv_obj_set_style_text_color(foot, lv_color_hex(COL_LINE), 0);
    lv_obj_set_style_text_font(foot, &lv_font_montserrat_12, 0);
    lv_obj_align(foot, LV_ALIGN_BOTTOM_MID, 0, -24);

    lv_scr_load(scr_splash);
}

// Advance the bar and pump LVGL while the stage runs. The pump is the point: the
// panel has to keep drawing during a card scan that takes a second, or the boot
// looks like a crash for exactly as long as it is doing the most work.
static void splash_stage(const char *msg, uint8_t pct) {
    if (!scr_splash) return;
    if (msg && lbl_splash_stage) lv_label_set_text(lbl_splash_stage, msg);
    if (bar_splash) lv_bar_set_value(bar_splash, pct, LV_ANIM_ON);
    // Long enough for the bar to visibly move between two stages, short enough
    // that four stages are not most of a second of boot.
    const uint32_t until = millis() + 90;
    while ((int32_t)(millis() - until) < 0) {
        lv_timer_handler();
        delay(4);
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
static void map_recentre();    // fwd: the status pill is the only way to ask now
static bool g_need_map_redraw = true;

// -----------------------------------------------------------------------------
//  The map's per-frame state, declared here with the rest of what the render
//  pass reads.
//
//  These are written much further down the file - by the gesture handlers, the
//  motion tick and the chrome - and read by code that sits between here and
//  there, so the declaration has to be above all of it. That is not cosmetic: a
//  global declared below its first reader does not exist as far as that reader
//  is concerned, the sketch stops compiling on the first `if`, and the host
//  tests cannot see it at all because they compile extracted regions rather
//  than the sketch.
// -----------------------------------------------------------------------------

// The centre reticle's fade (see map_feel_tick).
static uint8_t  g_reticle_step    = 0;    // 3 = full ... 0 = hidden
static uint32_t g_reticle_next_ms = 0;

// What the rotation follow is aiming at: -compass-heading while heading-up, 0
// while the map settles back to north-up, -999 when it is not following.
static float    g_heading_target  = -999.0f;

// Coasting (inertia) after a flick. vx/vy are px/ms in *finger* space, so the
// coast continues the motion the finger made rather than fighting it.
static float    g_fling_vx = 0, g_fling_vy = 0;
static float    g_fling_rem_x = 0, g_fling_rem_y = 0;   // sub-pixel carry
static uint32_t g_fling_last_ms = 0;
static uint32_t g_drag_last_ms = 0;                     // when the last pan sample arrived

// Camera moves that play out over a few frames instead of teleporting.
#define WP_ANIM_NONE    0
#define WP_ANIM_ZOOM    1
#define WP_ANIM_CAMERA  2
#define WP_ZOOM_MS      170      // one zoom step: quick, but not a jump
#define WP_RECENTRE_MS  420      // a long move: long enough to read where it went
#define WP_ROT_TAU_MS   150.0f   // heading-up follow: the wrist, not the magnetometer

static uint8_t  g_anim_kind = WP_ANIM_NONE;
static uint32_t g_anim_t0 = 0, g_anim_dur = 0;
static double   g_anim_from_x = 0, g_anim_from_y = 0, g_anim_to_x = 0, g_anim_to_y = 0;
static float    g_anim_from_scale = 1.0f, g_anim_to_scale = 1.0f;
static int32_t  g_anim_ax = 0, g_anim_ay = 0;      // the pixel a zoom holds still
static uint32_t g_cam_moved_ms = 0;               // drives the reticle fade
static uint32_t g_feel_last_ms = 0;               // frame clock for the follow



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
                const uint8_t base = ROAD_WIDTH_BASE[o.sub < RC__COUNT ? o.sub : RC_OTHER];
                // Width follows the FRACTIONAL camera zoom, not a rounded zoom
                // level. Doubling in one step when the camera crossed a level
                // was the most visible thing a pinch did: a street jumped from
                // 2 px to 4 px halfway through the gesture. Interpolating
                // between the two sizes costs one multiply and makes the whole
                // zoom continuous - while still landing on exactly `base` at the
                // native zoom and exactly 2x one level in, so the widths the
                // tests pin are unchanged.
                const float dz = g_cam_zoom_eff - (float)g_meta.refzoom;
                int w;
                if (dz >= 0.0f) {
                    const float d    = (dz > 1.0f) ? 1.0f : dz;   // never past 2x
                    const int   step = (int)d;
                    const float frac = d - (float)step;
                    const int   lo   = base << step;
                    const int   hi   = base << (step + 1);
                    w = lo + (int)((float)(hi - lo) * frac + 0.5f);
                } else {
                    const float d    = (dz < -6.0f) ? -6.0f : dz; // never past 1/64
                    const int   step = (int)(-d);
                    w = (base + (1 << step) / 2) >> step;
                }
                if (w < 1)   w = 1;
                if (w > 255) w = 255;
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
//
// Two things happen here beyond drawing a circle. Dots are offered in rank
// order and each one claims a box in the same occupancy grid the labels use, so
// a dense strip of shops is thinned to the handful that matter instead of
// becoming a smear - and, because the labels claim their space afterwards, a
// name can no longer land on top of the dot it belongs to. And a dot fades in
// as its zoom band arrives rather than appearing at full strength, which is
// what stops an opening map from shimmering as the detail switches on.
static void render_pois() {
    if (!g_pois) return;
    const ThemePalette &t = TH();
    const uint16_t edge = wp_mix565(t.text, t.bg, 120);
    for (int pass = 0; pass < 2; pass++) {
        const bool wanted_major = (pass == 0);
        for (uint32_t i = 0; i < g_poi_count; i++) {
            const FMapPOI &p = g_pois[i];
            const bool major = (p.rank <= 1);
            if (major != wanted_major) continue;     // cities first, then the rest
            if (!(g_layer_mask & (1u << p.layer))) continue;
            const float span = g_cam_zoom_eff - (float)p.zmin;
            if (span < -0.5f) continue;              // its band has not arrived
            const uint8_t mix = (span >= 0.5f)
                              ? 255 : (uint8_t)((span + 0.5f) * 255.0f);
            if (mix < 40) continue;
            int sx, sy;
            world_to_screen(p.x, p.y, &sx, &sy);
            if (sx < -6 || sy < -6 || sx > LCD_WIDTH + 6 || sy > LCD_HEIGHT + 6)
                continue;
            uint16_t c = t.shops;
            switch (p.type) {
                case 3: c = t.transit; break;
                case 4: c = t.schools; break;
                case 5: c = t.parks;   break;
                default: c = t.shops;  break;
            }
            const int r = major ? 5 : 3;
            if (!lbl_claim(sx - r, sy - r, sx + r + 1, sy + r + 1)) continue;
            gfx->fillCircle(sx, sy, r, wp_mix565(t.bg, c, mix));
            gfx->drawCircle(sx, sy, r + 1, wp_mix565(t.bg, edge, mix));
        }
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
// without needing an opaque plate behind every one of them. Four neighbours
// rather than eight: at one pixel the diagonals add nothing a reader can see,
// and this runs up to 26 times a frame - so halving the glyph runs halves what
// the label pass costs on the QSPI bus.
static void lbl_draw(int x, int y, const char *s, uint8_t size,
                     uint16_t fg, uint16_t halo) {
    static const int8_t ox[4] = { -1,  1,  0,  0 };
    static const int8_t oy[4] = {  0,  0, -1,  1 };
    gfx->setTextSize(size);
    gfx->setTextColor(halo);
    for (int i = 0; i < 4; i++) {
        gfx->setCursor(x + ox[i], y + oy[i]);
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
    // The occupancy grid is cleared once per frame in draw_map_frame(), before
    // the POI dots claim their boxes -- so a name cannot be placed on top of
    // the dot it belongs to.


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
        // A saved place is a filled dot with a dark centre, so it stays readable
        // on top of the bright green route and the POI dots it sits among.
        gfx->fillCircle(sx, sy, 7, TH().saved);
        gfx->fillCircle(sx, sy, 3, TH().bg);
        gfx->drawCircle(sx, sy, 7, wp_mix565(TH().saved, TH().bg, 60));
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
// -----------------------------------------------------------------------------
//  Map chrome
//
//  Three pieces, and each one only says something true. A status pill across the
//  top: the map's name, what the scale is called ("city", "suburb"), and - only
//  when tiles are actually failing - an amber health dot. The 9/9 tile counter
//  that used to sit up here is on the DEV screen now; a shipped map should not
//  show its reader a denominator. A nav banner under it, which exists only while
//  a target is set, with the target's name, distance and direction and an X to
//  clear it. And a centre reticle that is deliberately temporary: it is how you
//  tell where the camera is *while you are moving it*, and it has faded out
//  within a second of you stopping, because a crosshair parked over the middle
//  of a map you are reading is just one more thing in the way.
//
//  The one tappable piece of chrome is the compass rose: tap it to lock the map
//  to heading-up, tap again to settle back to north-up.
// -----------------------------------------------------------------------------
#define WP_TOP_STRIP_H   26
#define WP_COMPASS_W     26
#define WP_NAV_H         24
#define WP_HINT_FULL_MS 5200      // hints stay at full strength this long
#define WP_HINT_FADE_MS  300      // then dim one step per this long ...
#define WP_HINT_STEPS     6       // ... over six steps, so the fade is smooth

static uint8_t  g_map_hint_alpha   = 0;      // WP_HINT_STEPS = full ... 0 = hidden
static uint32_t g_map_hint_next_ms = 0;
static bool     g_hints_seen       = false;  // taught once, remembered in NVS

static void wp_map_hint_show(uint32_t now) {
    g_map_hint_alpha   = WP_HINT_STEPS;
    g_map_hint_next_ms = now + WP_HINT_FULL_MS;
    g_need_map_redraw  = true;
}

// Remember that the gestures have been taught, so someone who knows them is not
// shown the card on every map open (a tap still brings it back on demand). One
// NVS write, on the first fade-out - not one per frame.
static void wp_map_hint_mark_seen() {
    if (g_hints_seen) return;
    g_hints_seen = true;
    Preferences p; p.begin("waypoint", false); p.putBool("hints", true); p.end();
}

static void wp_map_hint_tick(uint32_t now) {
    if (g_map_hint_alpha == 0 || now < g_map_hint_next_ms) return;
    g_map_hint_alpha--;
    g_map_hint_next_ms = now + WP_HINT_FADE_MS;
    if (g_map_hint_alpha == 0) wp_map_hint_mark_seen();
    g_need_map_redraw  = true;
}

static void wp_compass_rect(int *x, int *y, int *w, int *h) {
    *w = WP_COMPASS_W;
    *h = WP_TOP_STRIP_H - 4;
    *x = LCD_WIDTH - WP_COMPASS_W - 7;
    *y = 5;
}

static void wp_map_hint_rect(int *x, int *y, int *w, int *h) {
    // Sits just above the scale bar, and lines up with the bottom edge-swipe
    // zone: the thing that says "swipe up for tools" is the thing you swipe up
    // from.
    *x = 6; *y = LCD_HEIGHT - 92; *w = LCD_WIDTH - 12; *h = 46;
}

// The nav banner: under the status pill, and only there while a target is set.
static void wp_nav_rect(int *x, int *y, int *w, int *h) {
    *x = 6; *y = WP_TOP_STRIP_H + 6; *w = LCD_WIDTH - 12; *h = WP_NAV_H;
}

// Distance to a world point, the way a person reads it.
static void wp_fmt_distance_to(int32_t wx, int32_t wy, char *out, size_t cap) {
    if (g_meta.units_per_metre <= 0) { snprintf(out, cap, "--"); return; }
    const double dx = (double)wx - g_cam_x, dy = (double)wy - g_cam_y;
    const float m = (float)(sqrt(dx * dx + dy * dy) /
                            (double)g_meta.units_per_metre);
    if (m < 1000.0f)       snprintf(out, cap, "%d m", (int)(m + 0.5f));
    else if (m < 10000.0f) snprintf(out, cap, "%.1f km", m / 1000.0f);
    else                   snprintf(out, cap, "%d km", (int)(m / 1000.0f + 0.5f));
}

// Which way to a world point, as a compass word (0 = north, 90 = east). The
// bearing puck tells you which way to turn; this tells you which way to look.
static const char *wp_bearing_to(int32_t wx, int32_t wy) {
    const double dx = (double)wx - g_cam_x, dy = (double)wy - g_cam_y;
    float b = atan2f((float)dx, (float)dy) * 180.0f / (float)M_PI;
    if (b < 0) b += 360.0f;
    return card_for(b);
}

// Returns true when a tap landed on chrome rather than on the map, so the map
// does not also read it as a POI selection.
static bool map_chrome_tap(int32_t x, int32_t y, uint32_t now) {
    int cx, cy, cw, ch;
    wp_compass_rect(&cx, &cy, &cw, &ch);
    if (x >= cx && x < cx + cw && y >= cy && y < cy + ch) {
        g_rotate_with_heading = !g_rotate_with_heading;
        // Heading-up hands the follow a moving target; north-up hands it zero,
        // which the follow then eases back to instead of snapping - a map that
        // slams back to north reads as a fault.
        // Heading-up hands the follow a moving target; north-up hands it zero,
        // which the follow then eases back to instead of snapping - a map that
        // slams back to north reads as a fault. With no compass reading there is
        // nothing to follow, so north is the answer in both cases.
        g_heading_target = (g_rotate_with_heading && g_heading >= 0.0f)
                         ? -g_heading : 0.0f;

        g_need_map_redraw = true;
        return true;
    }
    int hx, hy, hw, hh;
    wp_map_hint_rect(&hx, &hy, &hw, &hh);
    if (x >= hx && x < hx + hw && y >= hy && y < hy + hh) {
        wp_map_hint_show(now);                 // the hint card brings itself back
        return true;
    }
    if (g_has_nav_target) {
        int nx, ny, nw, nh;
        wp_nav_rect(&nx, &ny, &nw, &nh);
        if (x >= nx && x < nx + nw && y >= ny && y < ny + nh) {
            // The X at the far end clears the target; anywhere else on the strip
            // is deliberately inert, so a glance at the distance cannot throw
            // the navigation away.
            if (x >= nx + nw - WP_NAV_H) {
                g_has_nav_target = false;
                g_nav_target_name = "";
            }
            g_need_map_redraw = true;
            return true;
        }
    }
    if (y < WP_TOP_STRIP_H + 4) {
        // Tapping the status pill re-centres the camera. The pill is already the
        // "where am I" plate, so making it the re-centre control costs no new
        // chrome - and it replaces what used to be a medium hold of the BOOT
        // button, which nothing on screen ever mentioned.
        map_recentre();
        wp_map_hint_show(now);
        return true;
    }
    return false;
}

// Scale bar (bottom-centre): a real-world reference distance, on a plate so it
// stays readable over white sand, dark water and everything in between, with a
// solid leading half so the bar reads as a length rather than a hairline.
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
    const int bar_px = (int)target_px;
    const int half   = bar_px / 2;
    const int cx     = LCD_WIDTH / 2;
    const int by     = LCD_HEIGHT - 22;
    char lbl[16];
    if (chosen < 1000) snprintf(lbl, sizeof(lbl), "%d m", (int)chosen);
    else               snprintf(lbl, sizeof(lbl), "%d km", (int)(chosen/1000));
    const int lw = (int)strlen(lbl) * 6;

    const int plate_w = bar_px + 24;
    gfx->fillRoundRect(cx - plate_w/2, by - 16, plate_w, 28, 9, t.chrome_bg);
    gfx->drawRoundRect(cx - plate_w/2, by - 16, plate_w, 28, 9, t.chrome_border);

    gfx->setTextColor(t.text); gfx->setTextSize(1);
    gfx->setCursor(cx - lw/2, by - 12);
    gfx->print(lbl);

    const int bx = cx - bar_px/2;
    gfx->fillRect(bx, by + 3, half, 4, t.text);          // solid half
    gfx->drawRect(bx + half, by + 3, bar_px - half, 4, t.text);   // hollow half
    gfx->drawFastVLine(bx, by + 2, 6, t.text);
    gfx->drawFastVLine(bx + bar_px, by + 2, 6, t.text);
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

// The centre reticle at one of four strengths (see map_feel_tick()).
static void draw_reticle() {
    if (g_reticle_step == 0) return;
    const ThemePalette &t = TH();
    const int cx = LCD_WIDTH / 2, cy = LCD_HEIGHT / 2;
    const uint16_t col = (g_reticle_step >= 3) ? t.accent
                       : (g_reticle_step == 2)  ? wp_mix565(t.accent, t.bg, 80)
                                                : wp_mix565(t.accent, t.bg, 165);
    // A dot, not a crosshair: the map already tells you where the camera is by
    // what is under it, and a crosshair says "aim here" when it means "you are
    // here". Three rings of decaying green make the dot glow without an alpha
    // channel -- the AMOLED has none, so the halo is mixed toward the ground.
    if (g_reticle_step >= 3) {
        for (int i = 12; i >= 7; i -= 5) {
            const uint16_t ring = wp_mix565(t.bg, t.accent, (uint8_t)(26 + (12 - i) * 10));
            gfx->drawCircle(cx, cy, i, ring);
        }
    }
    gfx->fillCircle(cx, cy, 5, col);
    gfx->fillCircle(cx, cy, 2, wp_mix565(col, t.bg, 90));
}

// A target that is off the screen gets a chevron parked on the edge in its
// direction, with the distance beside it: the bearing puck says which way to
// turn, this says which way to look, and neither one replaces the other.
static void draw_offscreen_target() {
    if (!g_has_nav_target) return;
    const int cx = LCD_WIDTH / 2, cy = LCD_HEIGHT / 2;
    int sx, sy;
    world_to_screen(g_nav_target_x, g_nav_target_y, &sx, &sy);
    if (sx >= 20 && sx <= LCD_WIDTH - 20 && sy >= 20 && sy <= LCD_HEIGHT - 20) return;
    const float dx = (float)(sx - cx), dy = (float)(sy - cy);
    const float len = sqrtf(dx * dx + dy * dy);
    if (len < 1.0f) return;
    const float hw = (float)(cx - 18), hh = (float)(cy - 18);
    const float kx = fabsf(dx) > 1.0f ? hw / fabsf(dx) : 1e9f;
    const float ky = fabsf(dy) > 1.0f ? hh / fabsf(dy) : 1e9f;
    float k = (kx < ky) ? kx : ky;
    const int ex = cx + (int)(dx * k), ey = cy + (int)(dy * k);
    const float ux = dx / len, uy = dy / len;         // unit vector, screen space
    const float px = -uy, py = ux;                    // perpendicular
    const uint16_t c = TH().warn;
    // A triangle pointing the way you have to go.
    gfx->fillTriangle(ex + (int)(ux * 9), ey + (int)(uy * 9),
                      ex - (int)(ux * 5) + (int)(px * 7),
                      ey - (int)(uy * 5) + (int)(py * 7),
                      ex - (int)(ux * 5) - (int)(px * 7),
                      ey - (int)(uy * 5) - (int)(py * 7), c);
    char db[16];
    wp_fmt_distance_to(g_nav_target_x, g_nav_target_y, db, sizeof(db));
    int tx = ex - (int)(6 * strlen(db));
    int ty = ey + (int)(uy * 16) - 8;
    if (tx < 2) tx = 2;
    if (ty < WP_TOP_STRIP_H + 4) ty = WP_TOP_STRIP_H + 4;
    if (ty > LCD_HEIGHT - 20) ty = LCD_HEIGHT - 20;
    gfx->setTextColor(c); gfx->setTextSize(1);
    gfx->setCursor(tx, ty);
    gfx->print(db);
}

// The nav banner: target name, distance, direction, and an X that clears it.
static void draw_nav_banner() {
    if (!g_has_nav_target) return;
    const ThemePalette &t = TH();
    int nx, ny, nw, nh;
    wp_nav_rect(&nx, &ny, &nw, &nh);
    gfx->fillRoundRect(nx, ny, nw, nh, nh / 2, t.chrome_bg);
    gfx->drawRoundRect(nx, ny, nw, nh, nh / 2, t.accent);

    char dist[16];
    wp_fmt_distance_to(g_nav_target_x, g_nav_target_y, dist, sizeof(dist));
    const char *dir = wp_bearing_to(g_nav_target_x, g_nav_target_y);
    char right[32];
    snprintf(right, sizeof(right), "%s %s", dir, dist);
    const int rw = (int)strlen(right) * 6;
    const int rx = nx + nw - WP_NAV_H - 8 - rw;

    String nm = g_nav_target_name.length() ? g_nav_target_name : String("target");
    const int max_chars = (rx - (nx + 10)) / 6;
    if (max_chars > 2 && (int)nm.length() > max_chars)
        nm = nm.substring(0, max_chars - 1) + "~";

    const int ty = ny + (nh - 8) / 2;
    gfx->setTextColor(t.warn); gfx->setTextSize(1);
    gfx->setCursor(nx + 10, ty);
    gfx->print(nm);
    gfx->setTextColor(t.text);
    gfx->setCursor(rx, ty);
    gfx->print(right);

    // The X, drawn rather than typeset: the panel font has no glyph for a
    // cross, and two lines are cheaper than one anyway.
    const int kx = nx + nw - WP_NAV_H / 2 - 4, ky = ny + nh / 2;
    gfx->drawLine(kx - 4, ky - 4, kx + 4, ky + 4, t.text_dim);
    gfx->drawLine(kx - 4, ky + 4, kx + 4, ky - 4, t.text_dim);
}

static void draw_overlay_chrome() {
    const ThemePalette &t = TH();
    char b[80];

    // ---- status pill ------------------------------------------------------
    gfx->fillRoundRect(4, 3, LCD_WIDTH - 8, WP_TOP_STRIP_H, WP_TOP_STRIP_H / 2,
                       t.chrome_bg);
    gfx->drawRoundRect(4, 3, LCD_WIDTH - 8, WP_TOP_STRIP_H, WP_TOP_STRIP_H / 2,
                       t.chrome_border);
    const int ty = 3 + (WP_TOP_STRIP_H - 8) / 2;

    String name = g_meta.title.length() ? g_meta.title : String("map");
    if (name.length() > 13) name = name.substring(0, 12) + "~";
    gfx->setTextColor(t.accent); gfx->setTextSize(1);
    gfx->setCursor(12, ty);
    gfx->print(name);

    // The scale, named. "z12.4" is a number for whoever wrote the renderer; the
    // word is what tells a reader how close they are, so the word comes first
    // and the number is there for anyone who wants it.
    snprintf(b, sizeof(b), "%s %.1f", wp_zoom_band(g_cam_zoom_eff),
             (double)g_cam_zoom_eff);
    gfx->setTextColor(t.text_dim);
    gfx->setCursor(12 + (int)name.length() * 6 + 10, ty);
    gfx->print(b);

    // ---- compass rose: tap to lock heading-up ----------------------------
    {
        int cx, cy, cw, ch;
        wp_compass_rect(&cx, &cy, &cw, &ch);
        const int ccx = cx + cw / 2, ccy = cy + ch / 2;
        const int r   = (ch / 2) - 1;
        const uint16_t col = g_rotate_with_heading ? t.warn : t.accent;
        gfx->drawCircle(ccx, ccy, r, t.chrome_border);
        // The needle points at where north IS on the screen, which is
        // g_map_rotation_deg in both modes: up while north-up, and swung round
        // by the follow while heading-up. (It used to be pinned up whenever
        // heading-up was on, which quietly claimed north was straight ahead.)
        const float rr = g_map_rotation_deg * (float)M_PI / 180.0f;
        const int nx2 = ccx - (int)(-sinf(rr) * (float)(r - 3));
        const int ny2 = ccy - (int)( cosf(rr) * (float)(r - 3));
        gfx->fillTriangle(ccx - 3, ccy + 1, ccx + 3, ccy + 1, nx2, ny2, col);
    }

    // Tile health: a dot only when the visible ring is actually broken, which
    // is the earliest sign the tile set and the camera disagree - a missing
    // zoom level, a partial copy, an unknown schema.
    if (g_hud_tiles_empty > 0 || g_hud_tiles_ok == 0) {
        int cx, cy, cw, ch;
        wp_compass_rect(&cx, &cy, &cw, &ch);
        gfx->fillCircle(cx - 10, 3 + WP_TOP_STRIP_H / 2, 3, t.warn);
    }

    draw_nav_banner();
    draw_reticle();
    draw_offscreen_target();
    draw_scale_bar();

    // ---- gesture hints, fading out so the map is not permanently covered --
    if (g_map_hint_alpha) {
        int hx, hy, hw, hh;
        wp_map_hint_rect(&hx, &hy, &hw, &hh);
        // Six steps rather than three: the difference between a card that fades
        // and a card that visibly blinks out.
        const uint16_t hcol = (g_map_hint_alpha >= 5) ? t.text
                            : (g_map_hint_alpha >= 3) ? t.text_dim
                                                      : t.chrome_border;
        gfx->fillRoundRect(hx, hy, hw, hh, 16, t.chrome_bg);
        gfx->drawRoundRect(hx, hy, hw, hh, 16, t.chrome_border);
        gfx->setTextColor(hcol); gfx->setTextSize(1);
        gfx->setCursor(hx + 14, hy + 7);
        gfx->print("drag   pan      pinch  zoom");
        gfx->setCursor(hx + 10, hy + 21);
        gfx->print("tap    poi      hold   tools");
        gfx->setCursor(hx + 10, hy + 35);
        gfx->print("swipe from an edge: tools / library");
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

    // One occupancy grid per frame, cleared here rather than inside the label
    // pass: the POI dots claim their boxes first, so the names drawn after them
    // cannot land on top of the dots they belong to.
    memset(g_lbl_occ, 0, sizeof(g_lbl_occ));
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

static void enter_mode(AppMode m);

static void add_back_pill(lv_obj_t *p, const char *label, lv_event_cb_t cb) {
    lv_obj_t *back = lv_btn_create(p);
    lv_obj_set_size(back, 220, 42);
    lv_obj_align(back, LV_ALIGN_BOTTOM_MID, 0, -16);
    lv_obj_set_style_bg_color(back, lv_color_hex(COL_CARD), 0);
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
    lv_obj_set_style_bg_color(box, lv_color_hex(COL_BG), 0);
    lv_obj_set_style_border_color(box, lv_color_hex(COL_LINE), 0);
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
        lv_obj_set_style_bg_color(sw_row, lv_color_hex(COL_CARD), 0);
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
static lv_obj_t *g_theme_rows[THEME_COUNT] = {nullptr};
static lv_obj_t *g_theme_ticks[THEME_COUNT] = {nullptr};

static void themes_refresh() {
    for (int i=0; i<THEME_COUNT; i++) {
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

    for (int i=0; i<THEME_COUNT; i++) {
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
    lv_obj_set_style_bg_color(save_ta, lv_color_hex(COL_CARD), 0);
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
    lv_obj_set_style_bg_color(search_ta, lv_color_hex(COL_CARD), 0);
    lv_obj_set_style_text_color(search_ta, lv_color_hex(COL_TXT), 0);

    search_results = lv_list_create(scr_search);
    lv_obj_set_size(search_results, 380, 200);
    lv_obj_align(search_results, LV_ALIGN_TOP_MID, 0, 104);
    lv_obj_set_style_bg_color(search_results, lv_color_hex(COL_BG), 0);

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
                lv_obj_set_style_bg_color(it, lv_color_hex(COL_CARD), 0);
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
            lv_obj_set_style_bg_color(it, lv_color_hex(COL_CARD), 0);
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
    lv_obj_set_style_bg_color(back, lv_color_hex(COL_CARD), 0);
    lv_obj_t *bl = lv_label_create(back); lv_label_set_text(bl, LV_SYMBOL_LEFT); lv_obj_center(bl);
    lv_obj_add_event_cb(back, cb_back_to_map, LV_EVENT_CLICKED, NULL);
}

// ----- Dev mode -----
static void build_dev() {
    if (scr_dev) return;
    scr_dev = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr_dev, lv_color_hex(COL_BG), 0);
    lv_obj_set_style_bg_opa(scr_dev, LV_OPA_COVER, 0);
    lv_obj_t *t = lv_label_create(scr_dev);
    lv_label_set_text(t, "DEV / DIAGNOSTICS");
    lv_obj_set_style_text_color(t, lv_color_hex(COL_ACCENT),0);
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
    char buf[1024];
    snprintf(buf, sizeof(buf),
        "Map:   %s\n"
        "Zoom:  %u of %u levels\n"
        "Scale: z%.1f  (%s)\n"
        "Ring:  %u ok, %u empty\n"
        "Cam:   %.0f, %.0f  scl %.4f\n"
        "Rot:   %.1f deg%s\n"
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
        (double)g_cam_zoom_eff, wp_zoom_band(g_cam_zoom_eff),
        (unsigned)g_hud_tiles_ok, (unsigned)g_hud_tiles_empty,
        g_cam_x, g_cam_y, g_cam_scale,
        (double)g_map_rotation_deg,
        g_rotate_with_heading ? "  heading-up" : "",
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
        (double)g_heading, (double)g_marked_heading);

    // Map packs, because "my .wpb does not show up" is the one question this
    // screen can actually answer. A pack is listed here whether or not it
    // parsed, with its size - so a 0-byte copy and a wrong-format export look
    // different instead of both looking like an empty library.
    int used = (int)strlen(buf);
    if (used < (int)sizeof(buf) - 40) {
        used += snprintf(buf + used, sizeof(buf) - (size_t)used,
                         "\nPacks: %d on card, built-in %s\n",
                         g_sd_bundle_count, g_bundle_ok ? "ok" : "none");
        for (int i = 0; i < g_sd_bundle_count && used < (int)sizeof(buf) - 80; i++) {
            const WpSdBundle &pk = g_sd_bundles[i];
            String n = pk.path;
            int sl = n.lastIndexOf('/'); if (sl >= 0) n = n.substring(sl + 1);
            if (pk.ok)
                // The second figure is what the pack cost in RAM: its index,
                // strings and thumbnail. The rest of it stayed on the card.
                used += snprintf(buf + used, sizeof(buf) - (size_t)used,
                                 "  %s  %luK  %d maps  %lu tiles  %luK in RAM\n", n.c_str(),
                                 (unsigned long)(pk.size / 1024), (int)pk.b.maps,
                                 (unsigned long)pk.b.tiles,
                                 (unsigned long)(pk.b.mem / 1024));
            else
                used += snprintf(buf + used, sizeof(buf) - (size_t)used,
                                 "  %s  %luK  NOT a WPB1 pack\n", n.c_str(),
                                 (unsigned long)(pk.size / 1024));
        }
    }

    lv_label_set_text(lbl_dev, buf);
}

// ----- Tools menu -----
static void cb_tools_enter_mode(lv_event_t *e) {
    AppMode *tp = (AppMode*)lv_event_get_user_data(e);
    if (tp) enter_mode(*tp);
}
static void cb_tools_launcher(lv_event_t*) { return_to_launcher(); }
static void cb_tools_breadcrumb(lv_event_t *e) {
    (void)e;
    breadcrumb_add((int32_t)g_cam_x, (int32_t)g_cam_y);
    enter_mode(MODE_MAP);
}

// One tile of the grid. A tile either goes to a mode or runs an action, and both
// are described the same way so the rows below stay a table instead of a pile of
// near-identical loops.
struct ToolTile {
    const char *icon;
    const char *name;
    AppMode target;
    lv_event_cb_t cb;
};

// A captioned row of tiles. Eleven identical squares is a wall; four rows with a
// word in front of each is a menu you can read at a glance.
static void tools_add_row(lv_obj_t *parent, const char *cap,
                          const ToolTile *tiles, size_t count) {
    if (cap) {
        lv_obj_t *c = lv_label_create(parent);
        lv_label_set_text(c, cap);
        lv_obj_add_style(c, &wp_style_label_dim, 0);
        lv_obj_set_width(c, LCD_WIDTH - 32);
        lv_obj_set_style_text_align(c, LV_TEXT_ALIGN_LEFT, 0);
    }
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_set_size(row, LCD_WIDTH - 24, 66);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_pad_all(row, 0, 0);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_EVENLY,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    for (size_t i = 0; i < count; i++) {
        MenuItem mi;
        if (tiles[i].cb) {
            MenuItem act = { tiles[i].icon, tiles[i].name, tiles[i].cb, nullptr };
            mi = act;
        } else {
            AppMode *tgt = (AppMode*)lv_mem_alloc(sizeof(AppMode));
            *tgt = tiles[i].target;
            MenuItem go = { tiles[i].icon, tiles[i].name, cb_tools_enter_mode, tgt };
            mi = go;
        }
        lv_obj_t *b = wp_create_menu_item(row, mi);
        lv_obj_set_size(b, 92, 60);
    }
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
    lv_obj_align(t, LV_ALIGN_TOP_MID, 0, 14);

    // One column of captioned rows, so the groups read top to bottom.
    lv_obj_t *col = lv_obj_create(scr_tools);
    lv_obj_set_size(col, LCD_WIDTH - 8, LCD_HEIGHT - 126);
    lv_obj_align(col, LV_ALIGN_TOP_MID, 0, 40);
    lv_obj_set_style_bg_opa(col, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(col, 0, 0);
    lv_obj_set_style_pad_all(col, 4, 0);
    lv_obj_set_style_pad_row(col, 4, 0);
    lv_obj_clear_flag(col, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(col, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);

    // What the map is: how it is painted, and where you left it.
    static const ToolTile map_tiles[] = {
        { LV_SYMBOL_SAVE,  "Save",   MODE_SAVEDIALOG, nullptr },
        { LV_SYMBOL_LIST,  "Layers", MODE_LAYERS,     nullptr },
        { LV_SYMBOL_IMAGE, "Themes", MODE_THEMES,     nullptr },
    };
    // Where you are going: the instruments for a direction and a distance.
    static const ToolTile go_tiles[] = {
        { LV_SYMBOL_GPS,       "Compass",   MODE_COMPASS,  nullptr },
        { LV_SYMBOL_LOOP,      "Bearings",  MODE_BEARINGS, nullptr },
        { LV_SYMBOL_UP,        "Block Nav", MODE_BLOCKNAV, nullptr },
        { LV_SYMBOL_DIRECTORY, "Search",    MODE_SEARCH,   nullptr },
    };
    // What is out there: the ground under your feet and what is on it.
    static const ToolTile ground_tiles[] = {
        { LV_SYMBOL_CHARGE,   "Sun",        MODE_SUN,     nullptr },
        { LV_SYMBOL_MINUS,    "Level",      MODE_LEVEL,   nullptr },
        { LV_SYMBOL_EYE_OPEN, "Explore",    MODE_EXPLORE, nullptr },
        { LV_SYMBOL_DOWNLOAD, "Breadcrumb", MODE_MAP,     cb_tools_breadcrumb },
    };
    // And the screens that are about the app rather than the map. "Launcher"
    // belongs here rather than behind a button: it is the one exit that has to
    // be findable from anywhere, and Tools is reachable from the map with one
    // hold.
    static const ToolTile system_tiles[] = {
        { LV_SYMBOL_SETTINGS, "Developer", MODE_DEV,  nullptr },
        { LV_SYMBOL_POWER,    "Launcher",  MODE_TOOLS, cb_tools_launcher },
    };

    tools_add_row(col, "THE MAP", map_tiles,
                  sizeof(map_tiles) / sizeof(map_tiles[0]));
    tools_add_row(col, "FIND THE WAY", go_tiles,
                  sizeof(go_tiles) / sizeof(go_tiles[0]));
    tools_add_row(col, "THE GROUND", ground_tiles,
                  sizeof(ground_tiles) / sizeof(ground_tiles[0]));
    tools_add_row(col, "SYSTEM", system_tiles,
                  sizeof(system_tiles) / sizeof(system_tiles[0]));

    // Clearing a target belongs to the target: the nav banner carries its own X,
    // so the tool that duplicated it is gone rather than sitting here confusingly
    // next to the banner that already does it.
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
        lv_obj_set_style_bg_color(b, lv_color_hex(COL_CARD), 0);
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
    lv_obj_set_style_bg_color(sb, lv_color_hex(COL_CARD), 0);
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
    lv_obj_set_style_bg_color(next, lv_color_hex(COL_CARD), 0);
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
    // A long place name wraps onto a second line instead of being clipped at
    // the edge of the card, and the body sits clear of both lines.
    lv_label_set_long_mode(lbl_poi_title, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(lbl_poi_title, 360);
    lv_obj_set_style_text_align(lbl_poi_title, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(lbl_poi_title, LV_ALIGN_TOP_MID, 0, 36);
    lbl_poi_body = lv_label_create(scr_poicard);
    lv_label_set_text(lbl_poi_body, "");
    lv_obj_set_style_text_color(lbl_poi_body, lv_color_hex(COL_TXT), 0);
    lv_obj_set_style_text_font(lbl_poi_body, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_align(lbl_poi_body, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(lbl_poi_body, LV_ALIGN_TOP_MID, 0, 104);

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
    lv_obj_set_style_bg_color(sv, lv_color_hex(COL_CARD), 0);
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

    // Nothing carries across a screen change: no pan coasting into a menu, no
    // camera move still playing behind the tools screen.
    g_fling_vx = g_fling_vy = 0.0f;
    g_fling_rem_x = g_fling_rem_y = 0.0f;
    g_fling_last_ms = 0;
    g_anim_kind = WP_ANIM_NONE;

    switch (m) {
        case MODE_MAP:
            build_blank();
            lv_scr_load(scr_blank);
            g_need_map_redraw = true;
            // Teach the gestures the first time, then let the card fade and
            // stay gone: it is there for someone who has never held this watch,
            // and it is in the way of everyone else (a tap still brings it
            // back on demand).
            if (!g_hints_seen) wp_map_hint_show(millis());
            break;

        case MODE_LIBRARY:
            // LVGL owns this screen now, so it is loaded like every menu rather
            // than painted over a blank one.
            build_library();
            lv_scr_load(scr_library);
            // Re-read the card on the way in. A pack copied on a PC while the
            // watch was running is otherwise invisible until the next reboot --
            // which is indistinguishable, from the outside, from "it does not
            // load this map". Nothing is open at this point (library_close()
            // closes first), so the packs being freed underneath are unreferenced.
            if (lib_hint) {
                lv_label_set_text(lib_hint, "checking the card...");
                lv_refr_now(NULL);
            }
            library_scan();
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
    const MapEntry &ent = g_maps[g_map_sel];
    const String &path = ent.path;
    // Three kinds of map, one loader: a pack baked into the firmware, a pack on
    // the card (a .wpb), and a folder of loose tiles. The map screen cannot
    // tell which it is looking at, and neither can the renderer.
    const bool opened = ent.broken  ? false
                      : ent.sd_slot >= 0 ? mvt_map_open_bundle(ent.sd_slot, ent.bundle_id)
                      : ent.builtin      ? mvt_map_open_bundle(-1, ent.bundle_id)
                                         : mvt_map_open(path);
    if (!opened) {
        // Stay in the library and say so on the card itself: a hard-edged red
        // rectangle around a fixed box was drawn where the LVGL card used to
        // be, which left nothing to actually read.
        library_open_failed();
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
//  Interaction: touch only (pan / pinch / swipe / tap / hold)
//
//  There is no button state machine here any more. The BOOT button used to be a
//  parallel set of controls - step the carousel, open, zoom in and out, back one
//  level, back to the launcher - which meant half the app was invisible unless
//  you knew to press a button on the side of a watch. Every one of those now has
//  a gesture, and the screen says which.
// =============================================================================
// Touch. The recogniser classifies the gesture (see wp_gesture_feed); what lives
// here is only the state the map keeps between samples - the previous pan sample
// and the scale and pixel the current pinch started from. The coasting and
// camera-move state sits up with the rest of the map's per-frame state, by the
// render pipeline, because enter_mode() has to clear it and this is declared
// after that.
static bool     g_dragging = false;
static int32_t  g_drag_last_x = 0, g_drag_last_y = 0;
static bool     g_pinching = false;
static int32_t  g_pinch_start_y = 0;
static float    g_pinch_start_scale = 1;
static int32_t  g_pinch_ax = 0, g_pinch_ay = 0;   // the pixel a pinch is anchored on



// World offset of a screen pixel, from the one inverse implementation in the
// motion engine. (The hand-rolled copy that used to sit in map_pan() did not
// agree with world_to_screen() once the map was rotated, so dragging a
// heading-up map pulled the world off at the wrong angle.)
static void screen_to_world(int32_t sx, int32_t sy, double *wx, double *wy) {
    double ux, uy;
    wp_pixel_to_world_offset(sx, sy, g_screen_cx, g_screen_cy,
                             g_map_rotation_deg, g_cam_scale, &ux, &uy);
    *wx = g_cam_x + ux;
    *wy = g_cam_y + uy;
}

// Pan: whatever was under the pixel the finger came from must now sit under the
// pixel it reached, so the camera moves by the negative of the world offset of
// that screen delta. Expressed through the shared inverse, so this is right on a
// rotated map too - which is the case the old formula got wrong.
static void map_pan(int dx, int dy) {
    double ux, uy;
    wp_pixel_to_world_offset(g_screen_cx + dx, g_screen_cy + dy,
                             g_screen_cx, g_screen_cy,
                             g_map_rotation_deg, g_cam_scale, &ux, &uy);
    g_cam_x -= ux;
    g_cam_y -= uy;
    g_cam_moved_ms = millis();
    g_need_map_redraw = true;
}

// Start a camera move. For a zoom, `ax/ay` is the pixel that must not move: the
// world point under it is the same at both ends of the move, so the map grows
// where you are looking instead of sliding out from under you.
static void anim_start(uint8_t kind, uint32_t dur, int32_t ax, int32_t ay,
                       double tx, double ty, float to_scale, uint32_t now) {
    float lo, hi; cam_scale_bounds(&lo, &hi);
    if (to_scale < lo) to_scale = lo;
    if (to_scale > hi) to_scale = hi;
    g_anim_kind       = kind;
    g_anim_t0         = now;
    g_anim_dur        = dur;
    g_anim_ax         = ax;
    g_anim_ay         = ay;
    g_anim_from_x     = g_cam_x;
    g_anim_from_y     = g_cam_y;
    g_anim_from_scale = g_cam_scale;
    g_anim_to_scale   = to_scale;
    if (kind == WP_ANIM_ZOOM) {
        double wx, wy;
        screen_to_world(ax, ay, &wx, &wy);
        wp_cam_anchor(wx, wy, ax, ay, g_screen_cx, g_screen_cy,
                      g_map_rotation_deg, to_scale, &g_anim_to_x, &g_anim_to_y);
    } else {
        g_anim_to_x = tx;
        g_anim_to_y = ty;
    }
    g_need_map_redraw = true;
}

// One frame of that move. Returns true while it is still running.
static bool anim_tick(uint32_t now) {
    if (g_anim_kind == WP_ANIM_NONE) return false;
    const float t = (g_anim_dur == 0) ? 1.0f
                                      : (float)(now - g_anim_t0) / (float)g_anim_dur;
    const float e = wp_ease_out(t);
    float lo, hi; cam_scale_bounds(&lo, &hi);
    g_cam_scale = g_anim_from_scale + (g_anim_to_scale - g_anim_from_scale) * e;
    if (g_cam_scale < lo) g_cam_scale = lo;
    if (g_cam_scale > hi) g_cam_scale = hi;
    g_cam_x = g_anim_from_x + (g_anim_to_x - g_anim_from_x) * e;
    g_cam_y = g_anim_from_y + (g_anim_to_y - g_anim_from_y) * e;
    g_cam_moved_ms = now;
    if (t >= 1.0f) {
        // Land exactly on the target rather than on the ease's last ulp: a
        // camera that stops a fraction short is a camera that keeps redrawing.
        g_cam_scale = g_anim_to_scale;
        g_cam_x     = g_anim_to_x;
        g_cam_y     = g_anim_to_y;
        g_anim_kind = WP_ANIM_NONE;
    }
    cam_select_zoom();
    g_need_map_redraw = true;
    return g_anim_kind != WP_ANIM_NONE;
}

// Zoom by `factor` about a screen pixel, over a few frames.
static void map_zoom_at(float factor, int32_t ax, int32_t ay, uint32_t now) {
    float lo, hi; cam_scale_bounds(&lo, &hi);
    float to = g_cam_scale * factor;
    if (to < lo) to = lo;
    if (to > hi) to = hi;
    if (fabsf(to - g_cam_scale) < 1e-6f) return;
    anim_start(WP_ANIM_ZOOM, WP_ZOOM_MS, ax, ay, 0, 0, to, now);
}

// Recentre: back to the bounds centre at the middle of the ladder, as a move
// rather than a cut. cam_bounds_target() is the same maths cam_reset_to_bounds()
// uses, so the animated destination and the instantaneous one cannot disagree.
static void map_recentre() {
    double tx, ty; float ts; uint8_t tz;
    cam_bounds_target(&tx, &ty, &ts, &tz);
    anim_start(WP_ANIM_CAMERA, WP_RECENTRE_MS, g_screen_cx, g_screen_cy,
               tx, ty, ts, millis());
    MVT_LOG("[CAM] recentre -> %.5f,%.5f z=%u scale=%.6f\n", tx, ty,
            (unsigned)tz, ts);
}


// What a POI's icon family is called, in words. The card used to print the raw
// enum ("type: 3") and the world coordinates, which is a debugging view of a
// place rather than a description of it.
static const char *poi_type_word(uint8_t type) {
    switch (type) {
        case 3:  return "Transit stop";
        case 4:  return "School";
        case 5:  return "Park";
        case 1:  return "Shop";
        default: return "Place";
    }
}

// POI hit-test on tap. The NEAREST dot wins, not the first one the list happens
// to mention, and at the same distance the more important place wins - tapping
// a cluster should open the town, not the tenancy next door. The radius is
// 24 px because a finger is not a mouse; 14 px meant aiming.
#define WP_POI_TAP_PX 24
static bool tap_hit_poi(int sx, int sy) {
    if (!g_pois) return false;
    uint32_t best_i = 0xFFFFFFFFu;
    int32_t  best_d2 = WP_POI_TAP_PX * WP_POI_TAP_PX;
    uint8_t  best_rank = 255;
    for (uint32_t i = 0; i < g_poi_count; i++) {
        const FMapPOI &p = g_pois[i];
        if (!(g_layer_mask & (1u << p.layer))) continue;
        if (g_cam_zoom_eff < (float)p.zmin) continue;   // not on screen yet
        int px, py;
        world_to_screen(p.x, p.y, &px, &py);
        const int32_t ddx = (int32_t)(px - sx), ddy = (int32_t)(py - sy);
        const int32_t d2 = ddx * ddx + ddy * ddy;
        if (d2 < best_d2 || (d2 == best_d2 && p.rank < best_rank)) {
            best_d2   = d2;
            best_i    = i;
            best_rank = p.rank;
        }
    }
    if (best_i == 0xFFFFFFFFu) return false;

    const FMapPOI &p = g_pois[best_i];
    g_poi_shown_idx = best_i;
    // The card says what this is, how far away it is and which way to look -
    // which is everything a map card is for. The coordinates live on the DEV
    // screen for anyone who wants them.
    String body = poi_type_word(p.type);
    if (g_meta.units_per_metre > 0) {
        char db[16];
        wp_fmt_distance_to(p.x, p.y, db, sizeof(db));
        body += "\n";
        body += db;
        body += "  ";
        body += wp_bearing_to(p.x, p.y);
    }
    build_poicard();
    lv_label_set_text(lbl_poi_title, p.name ? p.name : "(unnamed)");
    lv_label_set_text(lbl_poi_body, body.c_str());
    enter_mode(MODE_POI_CARD);
    return true;
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
    if (ev & WG_SWIPE_LEFT)  { g_map_sel = wp_lib_step(g_map_sel, n, +1); g_lib_redraw = true; }
    if (ev & WG_SWIPE_RIGHT) { g_map_sel = wp_lib_step(g_map_sel, n, -1); g_lib_redraw = true; }
    if (ev & WG_SWIPE_UP)    { if (!g_lib_info) { g_lib_info = true;  g_lib_redraw = true; } }
    if (ev & WG_SWIPE_DOWN)  { if (g_lib_info)  { g_lib_info = false; g_lib_redraw = true; } }
    if (ev & WG_LONG_PRESS)  { g_lib_info = !g_lib_info; g_lib_redraw = true; }

    // Taps are not handled here any more: the card, the arrows and the shelf are
    // real LVGL widgets, so LVGL already knows what was pressed and calls the
    // handler for exactly that object. Doing it twice is how a tap on "open"
    // also stepped the carousel.

    if (ev & WG_SWIPE_DOWN) {
        // Swipe down from the top edge leaves for LauncherOS. The library is the
        // bottom of this app, so "down" is the way out - and it is a flick that
        // starts at the edge, so an ordinary tap can never throw the app away.
        if (g_gesture.edge == WP_EDGE_TOP && g_gesture.dur <= WP_EDGE_SWIPE_MAX_MS)
            return_to_launcher();
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
    // ---- the finger always wins ------------------------------------------
    // A new press stops the coasting and interrupts a camera move where it is.
    // Nothing feels worse than fighting the map for control of itself, and it
    // is the one thing an inertia implementation has to get right.
    if (g_tch_pressed && g_gesture.down) {
        if (g_fling_vx != 0.0f || g_fling_vy != 0.0f) {
            g_fling_vx = g_fling_vy = 0.0f;
            g_fling_rem_x = g_fling_rem_y = 0.0f;
            g_fling_last_ms = 0;
        }
        if (g_anim_kind != WP_ANIM_NONE) g_anim_kind = WP_ANIM_NONE;
    }

    // ---- continuous: one finger pans, two fingers zoom -------------------
    if (g_tch_pressed && g_gesture.down) {
        if (g_gesture.two_finger) {
            if (!g_pinching) {
                g_pinching = true;
                g_dragging = false;
                g_pinch_start_y = g_tch_y;
                g_pinch_start_scale = g_cam_scale;
                g_pinch_ax = g_tch_x;      // the pixel the zoom is anchored on
                g_pinch_ay = g_tch_y;
            }
            const int32_t dy = g_tch_y - g_pinch_start_y;
            float ns = g_pinch_start_scale * expf((float)(-dy) * 0.006f);
            float lo, hi; cam_scale_bounds(&lo, &hi);
            if (ns < lo) ns = lo;
            if (ns > hi) ns = hi;
            const bool fingers_moved = (g_pinch_ax != g_tch_x) ||
                                       (g_pinch_ay != g_tch_y);
            const bool rescaled = fabsf(ns - g_cam_scale) > 1e-4f;
            if (fingers_moved || rescaled) {
                // Anchor on where the fingers are NOW, not where they landed.
                // The world point under them keeps its pixel, which is the whole
                // of what a two-finger pinch means: the map grows about your
                // hand, and moving both fingers pans at the same time instead of
                // the map sliding out from under them. (The scale itself is
                // still measured from where the pinch started, so it cannot
                // drift as the anchor follows.)
                g_pinch_ax = g_tch_x;
                g_pinch_ay = g_tch_y;
                double wx, wy;
                screen_to_world(g_pinch_ax, g_pinch_ay, &wx, &wy);
                g_cam_scale = ns;
                wp_cam_anchor(wx, wy, g_pinch_ax, g_pinch_ay,
                              g_screen_cx, g_screen_cy, g_map_rotation_deg,
                              g_cam_scale, &g_cam_x, &g_cam_y);
                cam_select_zoom();
                g_cam_moved_ms = now;
                g_need_map_redraw = true;
            }

        } else if (g_gesture.dragged) {
            if (!g_dragging) {
                g_dragging = true;
                g_pinching = false;
                g_drag_last_x = g_tch_x;
                g_drag_last_y = g_tch_y;
                g_fling_vx = g_fling_vy = 0.0f;   // a fresh drag, a fresh flick
                g_drag_last_ms = now;
            }
            const int32_t dx = g_tch_x - g_drag_last_x;
            const int32_t dy = g_tch_y - g_drag_last_y;
            const uint32_t dt = now - g_drag_last_ms;
            if (dx || dy) {
                map_pan((int)dx, (int)dy);
                g_drag_last_x = g_tch_x;
                g_drag_last_y = g_tch_y;
            }
            // Flick velocity, weighted toward the newest sample so that a fast
            // drag which SLOWS to a stop before the finger lifts ends up with
            // no fling at all -- which is what a hand expects.
            if (dt > 0 && dt < 120) {
                const float k = 0.55f;
                g_fling_vx = g_fling_vx * (1.0f - k) + k * ((float)dx / (float)dt);
                g_fling_vy = g_fling_vy * (1.0f - k) + k * ((float)dy / (float)dt);
            }
            g_drag_last_ms = now;
        }
    }

    if (ev & WG_RELEASE) {
        const bool was_pan = g_dragging && !g_pinching;
        g_dragging = false;
        g_pinching = false;
        if (was_pan) {
            // Hand the pan over to the motion engine: the map keeps moving the
            // way the finger was moving, and the loop coasts it to a stop.
            wp_fling_cap(&g_fling_vx, &g_fling_vy);
            const float speed = sqrtf(g_fling_vx * g_fling_vx +
                                      g_fling_vy * g_fling_vy);
            if (speed >= WP_FLING_MIN_START) {
                g_fling_last_ms = now;
                g_fling_rem_x = g_fling_rem_y = 0.0f;
            } else {
                g_fling_vx = g_fling_vy = 0.0f;
            }
        } else {
            g_fling_vx = g_fling_vy = 0.0f;
        }
    }

    // ---- discrete --------------------------------------------------------
    if (ev & WG_LONG_PRESS) enter_mode(MODE_TOOLS);
    // Zoom about the point that was tapped, so the place you aimed at is the
    // place that stays put while the map grows.
    if (ev & WG_DOUBLE_TAP) map_zoom_at(1.5f, g_gesture.sx, g_gesture.sy, now);
    if (ev & WG_TWO_TAP)    map_zoom_at(1.0f / 1.5f, g_gesture.sx, g_gesture.sy, now);

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

    // A named gesture is not a coast: swiping away, zooming or opening a menu
    // never leaves the map still drifting underneath it.
    if (ev & (WG_SWIPE_LEFT | WG_SWIPE_RIGHT | WG_SWIPE_UP | WG_SWIPE_DOWN |
              WG_DOUBLE_TAP | WG_TWO_TAP | WG_LONG_PRESS)) {
        g_fling_vx = g_fling_vy = 0.0f;
        g_fling_rem_x = g_fling_rem_y = 0.0f;
        g_fling_last_ms = 0;
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

static uint32_t g_ev_count = 0;      // completed gestures, for the heartbeat

static void handle_map_touch(uint32_t now) {
    const uint16_t ev = g_gesture_events;
    g_gesture_events = WG_NONE;
    if (ev || g_tch_pressed) wp_mark_activity(now);
    if (ev) {
        g_ev_count++;
        WP_LOG("[touch] ev=0x%04x mode=%d\n", (unsigned)ev, (int)g_mode);
    }

    if (g_mode == MODE_LIBRARY) { library_gesture(ev, now); return; }
    if (g_mode == MODE_MAP)     { map_gesture(ev, now);     return; }
    menu_gesture(ev, now);
}

// -----------------------------------------------------------------------------
//  One frame of "feel".
//
//  The only place a coasting pan, a camera move or a turning wrist becomes
//  pixels: a still map does no work at all here, because everything it reads is
//  state some gesture handler set. Order matters - the coast feeds the camera,
//  then the camera move, then the heading follow, and the frame loop paints
//  whatever the three of them agreed on.
// -----------------------------------------------------------------------------
#define WP_RETICLE_HOLD_MS  900      // the centre reticle stays lit this long
#define WP_RETICLE_FADE_MS  280      // then one dimmer step per this long



static void map_feel_tick(uint32_t now) {
    // --- the reticle: lit while you are moving the camera, then out of the way
    // Each step asks for one more frame and no more, so a 410 px panel is not
    // repainting a crosshair thirty times a second while you read the map.
    if ((now - g_cam_moved_ms) < WP_RETICLE_HOLD_MS) {
        if (g_reticle_step != 3) { g_reticle_step = 3; g_need_map_redraw = true; }
        g_reticle_next_ms = now + WP_RETICLE_HOLD_MS;
    } else if (g_reticle_step > 0 && (int32_t)(now - g_reticle_next_ms) >= 0) {
        g_reticle_step--;
        g_reticle_next_ms = now + WP_RETICLE_FADE_MS;
        g_need_map_redraw = true;
    }

    // --- coasting -----------------------------------------------------------
    if (g_fling_vx != 0.0f || g_fling_vy != 0.0f) {
        const uint32_t last = g_fling_last_ms ? g_fling_last_ms : now;
        const float dt = (float)(now - last);
        g_fling_last_ms = now;
        float dx, dy;
        const bool still_moving = wp_fling_step(&g_fling_vx, &g_fling_vy,
                                                dt, &dx, &dy);
        // Sub-pixel carry: a slow flick moves less than a pixel per frame, and
        // rounding that away every frame would stall it long before the curve
        // says it is done.
        g_fling_rem_x += dx;
        g_fling_rem_y += dy;
        const int ix = (int)g_fling_rem_x, iy = (int)g_fling_rem_y;
        if (ix || iy) {
            g_fling_rem_x -= (float)ix;
            g_fling_rem_y -= (float)iy;
            map_pan(ix, iy);
        }
        if (!still_moving) {
            g_fling_last_ms  = 0;
            g_fling_rem_x    = 0.0f;
            g_fling_rem_y    = 0.0f;
        }
    }

    // --- a camera move in flight -------------------------------------------
    anim_tick(now);

    // --- the rotation follow -----------------------------------------------
    //  One target drives this either way: the compass while heading-up is on,
    //  or zero while the map is settling back to north-up after being switched
    //  off. Both are eased, so neither is a jump.
    if (g_heading_target > -900.0f) {
        const float dt = (float)(now - g_feel_last_ms);
        const float alpha = 1.0f - expf(-dt / WP_ROT_TAU_MS);
        const float left = fabsf(wp_angle_delta(g_map_rotation_deg,
                                                g_heading_target));
        // Heading-up chases only while the watch is actually being turned, or
        // when the map is badly out of date. A magnetometer wanders by a degree
        // or two on a stationary wrist, and repainting at 30 fps to follow that
        // is how a watch spends its battery without looking any better. Settling
        // to north has no such excuse: it runs to completion, then stops.
        const bool chase = g_rotate_with_heading
                         ? (left > 6.0f || (wp_imu_motion_detected() && left > 0.05f))
                         : (left > 0.05f);
        if (chase) {
            g_map_rotation_deg = wp_angle_step(g_map_rotation_deg,
                                               g_heading_target, alpha);
            g_cam_moved_ms = now;
            g_need_map_redraw = true;
        } else if (!g_rotate_with_heading) {
            g_map_rotation_deg = 0.0f;      // land exactly north, then stop
            g_heading_target   = -999.0f;
            g_need_map_redraw  = true;
        }
    }
    g_feel_last_ms = now;
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
                "  /WATCH/WayPoint/<MapName>.wpb               (a map PACK: one file)\n"
                "  /WATCH/WayPoint/<MapName>/{z}/{x}/{y}.pbf   (loose tiles, or .mvt)\n"
                "  /WATCH/WayPoint/<MapName>/metadata.json     (optional)\n"
                "  /WATCH/WayPoint/saved/                      (per-map saves)\n"
                "  /WATCH/WayPoint/breadcrumbs.csv\n"
                "  /WATCH/WayPoint/bearings.txt\n"
                "  /WATCH/WayPoint/loc.txt        (single line: LAT,LON for sun calc)\n\n"
                "A .wpb pack is one file holding every tile of a map plus its\n"
                "metadata and thumbnail: faster to copy, and it appears in the\n"
                "library as one card. A pack that this build cannot read is still\n"
                "listed, marked unreadable, rather than silently ignored.\n\n"
                "metadata.json (all fields optional):\n"
                "  { \"name\":\"Byford\", \"description\":\"...\",\n"
                "    \"bounds\":\"minLon,minLat,maxLon,maxLat\",\n"
                "    \"minzoom\":10, \"maxzoom\":17 }\n\n"
                "Controls - gestures first (see WaypointOS.ino header):\n"
                "  library: swipe < > browse, tap the card or OPEN MAP to open,\n"
                "           tap the chips to jump, swipe ^ for details,\n"
                "           swipe v from the TOP EDGE to leave for LauncherOS,\n"
                "           or tap EXIT in the corner\n"
                "  map:     drag pans (it coasts on after a flick), double tap\n"
                "           zooms in where you tapped, 2-finger tap zooms out,\n"
                "           2-finger drag pinches about your fingers, tap picks\n"
                "           a POI, hold opens Tools, a flick from the bottom\n"
                "           strip opens Tools, one from the top goes back,\n"
                "           tap the status pill to re-centre\n"
                "  menus:   tap a row, swipe v back a level, swipe < > between\n"
                "           the screens that belong together\n"
                "  hold the side button for 1.2 s at any time to leave for\n"
                "  LauncherOS (the same exit BookOS has, and the only control\n"
                "  that does not depend on the screen answering)\n"

            );
            rf.close();
        }
    }

    time_restore_from_nvs();
    WP_LOG("[boot] clock %s\n", g_tp_valid ? "live" : "not set yet");
    read_loc();
    imu_init();

    // NVS restore
    Preferences p; p.begin("waypoint", true);
    g_theme_id       = p.getUChar("theme", THEME_KIWI);
    // A stored id from an older build must not index past the table: this used to
    // clamp against the last theme that happened to exist then.
    if (g_theme_id >= THEME_COUNT) g_theme_id = THEME_KIWI;
    wp_sync_styles();   // Track B: prime shared LVGL styles from restored theme
    g_hints_seen = p.getBool("hints", false);   // the gesture card is taught once

    g_mx_off = p.getFloat("mxo", 0); g_my_off = p.getFloat("myo", 0); g_mz_off = p.getFloat("mzo", 0);
    g_mx_scl = p.getFloat("mxs", 1); g_my_scl = p.getFloat("mys", 1); g_mz_scl = p.getFloat("mzs", 1);
    g_marked_heading = p.getFloat("mark_hdg", -1);
    String last_map  = p.getString("last_map", "");
    p.end();

    // The splash is a real screen with a real progress bar, and every stage of
    // the boot reports into it: reading packs off a card is the one slow part of
    // startup, and a bar that actually moves is the difference between "booting"
    // and "hung".
    splash_begin();
    WP_LOG("[boot] splash up\n");
    splash_stage("settings", 15);

    // Open the built-in bundle once. A bundle that is missing, truncated or from
    // another version fails here and leaves the library exactly as it was.
#ifdef WP_HAS_BUNDLE
    g_bundle_ok = wp_bundle_open(&g_bundle, WP_MAPS_BUNDLE, WP_MAPS_BUNDLE_BYTES,
                                 WP_MAPS_BUNDLE_BYTES);
    if (g_bundle_ok)
        MVT_LOG("[BUNDLE] %d built-in maps, %u tiles, %u bytes\n",
                wp_bundle_map_count(&g_bundle), (unsigned)g_bundle.tiles,
                (unsigned)WP_MAPS_BUNDLE_BYTES);
    else
        MVT_LOG("[BUNDLE] wp_maps_bundle.h is present but not a WPB1 bundle\n");
#endif
#ifdef WP_HAS_BUNDLE
    WP_LOG("[boot] built-in bundle %s\n", g_bundle_ok ? "ok" : "failed");
#else
    WP_LOG("[boot] built-in bundle: none in this build\n");
#endif
    splash_stage("reading the card", 55);

    library_scan();
    WP_LOG("[boot] library: %d map(s)\n", (int)g_map_count_lib);
    splash_stage("building the library", 85);

    // pre-select last opened
    if (last_map.length()) {
        for (int i = 0; i < g_map_count_lib; i++) {
            if (g_maps[i].path == last_map) { g_map_sel = i; break; }
        }
    }

    build_blank();
    build_library();
    g_mode = MODE_LIBRARY;
    g_lib_redraw = true;
    library_refresh();          // fill the card before anything is shown
    lv_scr_load(scr_library);
    splash_stage("ready", 100);
    // Paint the library before the splash goes away, so the hand-off is a cut
    // between two finished frames instead of a black flicker between them.
    lv_refr_now(NULL);
    if (scr_splash) { lv_obj_del(scr_splash); scr_splash = nullptr; }

    // If the touch controller never answered, say so ON the screen rather than
    // presenting a library that silently ignores every finger. That was the
    // "frozen after the splash" report: a live app, a dead panel.
    if (!g_touch_ok && lib_toast) {
        lv_label_set_text(lib_toast, "Touch did not start.\n"
                                     "Hold the side button to leave.");
        lv_obj_clear_flag(lib_toast, LV_OBJ_FLAG_HIDDEN);
        lib_toast_until = millis() + 8000;
    }
    WP_LOG("[boot] ready -- touch %s\n", g_touch_ok ? "ok" : "DEAD");

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

    // BookOS parity: the side button is the way out that does not depend on the
    // screen answering at all.
    wp_exit_button_poll();

    // Touch only. The BOOT button used to be a second, invisible set of
    // controls - next map, open, zoom, back - which meant the screen was only
    // half usable without finding it. Everything it did now has a tap, a swipe
    // or a hold on the screen itself.
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
            // Heading-up: hand the follow a target and let map_feel_tick() ease
            // toward it on every frame. The target is -h, not h: a world bearing
            // b lands on screen at (b + g_map_rotation_deg), so facing east
            // (+90) has to turn the map by -90 to put east up. The old code set
            // the rotation to +h, which spun heading-up maps the wrong way.
            // Only ever SET the target here. Clearing it (as this used to) would
            // cancel a map that is still easing back to north-up after
            // heading-up was switched off - leaving it rotated while the rose
            // said north-up.
            if (g_rotate_with_heading) g_heading_target = -h;

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

    // Raw-gfx frames: the map only. Every other screen - the library included -
    // is LVGL and repaints itself, so there is nothing to push here for it.
    // Frame-rate throttle: cap draw_map_frame() at ~30 fps so many small pan
    // events between frames coalesce into a single redraw. The gesture handler
    // keeps g_cam_x/y up to date every touch tick, so the *camera* never lags
    // -- only the render is capped, which is what actually costs time & battery.
    #define WP_MIN_FRAME_MS  33         // ~30 fps
    if (g_mode == MODE_LIBRARY) {
        library_tick(now);
        if (g_lib_redraw) library_refresh();
    }
    else if (g_mode == MODE_MAP) {
        static uint32_t last_frame = 0;
        // Motion first, pixels second: coasting, a camera move in flight and the
        // heading follow all move the camera, then one frame paints the result.
        // This replaces the old "repaint every 250 ms just because heading-up is
        // on" tick - a turning map now asks for frames itself, and a still wrist
        // asks for none at all.
        map_feel_tick(now);
        if (g_need_map_redraw && (now - last_frame) >= WP_MIN_FRAME_MS) {
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

    // One line a second on the USB port. If the watch ever freezes again, the
    // last [beat] says which screen it was on, whether the panel is answering,
    // how many gestures have arrived and how much RAM is left -- instead of a
    // dark screen and a silent monitor.
    static uint32_t beat_t = 0;
    if (now - beat_t >= 1000) {
        beat_t = now;
        WP_LOG("[beat] %lus mode=%d touch=%s ev=%lu heap=%u psram=%u\n",
               (unsigned long)(now / 1000), (int)g_mode,
               g_touch_ok ? "ok" : "DEAD", (unsigned long)g_ev_count,
               (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getFreePsram());
    }

    delay(4);
}
