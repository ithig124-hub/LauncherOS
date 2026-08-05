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
//  MVT rendering diagnostics.
//  Set to 1 to have the tile loader / layer decoder / feature decoder print
//  their progress on Serial. Turn OFF once the map is confirmed rendering.
// =============================================================================
#ifndef MVT_DEBUG
#define MVT_DEBUG 1
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
struct HudRect;
enum HudBtn : int;   // forward-decl needs a fixed underlying type (C++11)

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
static void my_touchpad_read(lv_indev_drv_t *, lv_indev_data_t *data) {
    int32_t x=FT3168->IIC_Read_Device_Value(FT3168->Arduino_IIC_Touch::Value_Information::TOUCH_COORDINATE_X);
    int32_t y=FT3168->IIC_Read_Device_Value(FT3168->Arduino_IIC_Touch::Value_Information::TOUCH_COORDINATE_Y);
    int fingers=FT3168->IIC_Read_Device_Value(FT3168->Arduino_IIC_Touch::Value_Information::TOUCH_FINGER_NUMBER);
    g_tch_fingers = fingers;
    if (fingers > 0) {
        data->state=LV_INDEV_STATE_PR; data->point.x=x; data->point.y=y;
        g_tch_pressed=true; g_tch_x=x; g_tch_y=y;
    } else {
        data->state=LV_INDEV_STATE_REL;
        g_tch_pressed=false;
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

// =============================================================================
//  Layers
// =============================================================================
enum LayerId {
    LAY_ROADS=0, LAY_BUILDINGS, LAY_PARKS, LAY_WATER, LAY_PATHS,
    LAY_SCHOOLS, LAY_SHOPS, LAY_TRANSIT, LAY_BEACONS, LAY_SAVED, LAY_BREADCRUMBS,
    LAY_BOUNDARY, LAY_POI, LAY_UNKNOWN,
    LAY__COUNT
};
static const char *LAYER_NAMES[LAY__COUNT] = {
    "Roads","Buildings","Parks","Water","Paths",
    "Schools","Shops","Transit","Beacons","Saved","Breadcrumbs",
    "Boundary","POI","Unknown"
};
static uint32_t g_layer_mask = 0xFFFFFFFFu; // all on

static uint16_t layer_colour(uint8_t l) {
    const ThemePalette &t = TH();
    switch (l) {
        case LAY_ROADS: return t.roads;
        case LAY_BUILDINGS: return t.buildings;
        case LAY_PARKS: return t.parks;
        case LAY_WATER: return t.water;
        case LAY_PATHS: return t.paths;
        case LAY_SCHOOLS: return t.schools;
        case LAY_SHOPS: return t.shops;
        case LAY_TRANSIT: return t.transit;
        case LAY_BEACONS: return t.beacons;
        case LAY_SAVED: return t.saved;
        case LAY_BREADCRUMBS: return t.breadcrumb;
        case LAY_BOUNDARY: return t.text_dim;
        case LAY_POI: return t.shops;
        case LAY_UNKNOWN: return t.text_dim;
    }    return t.text;
}

// =============================================================================
//  MVT (Mapbox Vector Tile) format constants + WaypointOS layer mapping
//  Spec: https://github.com/mapbox/vector-tile-spec/tree/master/2.1
//  Tiles live at   /WATCH/WayPoint/<map>/{z}/{x}/{y}.pbf   (also accepts .mvt)
// =============================================================================
// Some tilemaker profiles use OpenMapTiles-style names, others use custom
// short names. Both are mapped to the WaypointOS LayerId enum here.
//
// Matching is case-insensitive and tries a "contains" test so hybrid names
// like "osm_roads", "roads_line", "custom_building_poly" still land in the
// right bucket. Anything not recognised falls into LAY_UNKNOWN (drawn as a
// thin grey outline when MVT_DRAW_UNKNOWN=1) instead of being silently
// dropped -- that way tiles from non-standard converters (e.g. this user's
// custom Perth export) are still visible on-screen while we discover which
// names to bake in permanently.
static inline char mvt_tolower(char c) { return (c >= 'A' && c <= 'Z') ? c + 32 : c; }
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

static uint8_t mvt_layer_id(const char *name, uint16_t nlen) {
    if (!name || nlen == 0) return LAY_UNKNOWN;

    // Label / name-only layers -- drop entirely (no geometry we care about).
    if (mvt_name_contains(name, nlen, "housenumber"))          return 0xFF;
    if (mvt_name_contains(name, nlen, "transportation_name"))  return 0xFF;
    if (mvt_name_contains(name, nlen, "water_name"))           return 0xFF;

    // Roads / transportation
    if (mvt_name_contains(name, nlen, "road"))          return LAY_ROADS;
    if (mvt_name_contains(name, nlen, "highway"))       return LAY_ROADS;
    if (mvt_name_contains(name, nlen, "street"))        return LAY_ROADS;
    if (mvt_name_contains(name, nlen, "motorway"))      return LAY_ROADS;
    if (mvt_name_contains(name, nlen, "transportation"))return LAY_ROADS;
    if (mvt_name_contains(name, nlen, "transport"))     return LAY_ROADS;

    // Paths / trails
    if (mvt_name_contains(name, nlen, "path"))          return LAY_PATHS;
    if (mvt_name_contains(name, nlen, "footway"))       return LAY_PATHS;
    if (mvt_name_contains(name, nlen, "cycleway"))      return LAY_PATHS;
    if (mvt_name_contains(name, nlen, "trail"))         return LAY_PATHS;
    if (mvt_name_contains(name, nlen, "track"))         return LAY_PATHS;

    // Rail / aeroway = transit
    if (mvt_name_contains(name, nlen, "rail"))          return LAY_TRANSIT;
    if (mvt_name_contains(name, nlen, "transit"))       return LAY_TRANSIT;
    if (mvt_name_contains(name, nlen, "aeroway"))       return LAY_TRANSIT;
    if (mvt_name_contains(name, nlen, "aero"))          return LAY_TRANSIT;

    // Buildings
    if (mvt_name_contains(name, nlen, "build"))         return LAY_BUILDINGS;

    // Water
    if (mvt_name_contains(name, nlen, "water"))         return LAY_WATER;
    if (mvt_name_contains(name, nlen, "ocean"))         return LAY_WATER;
    if (mvt_name_contains(name, nlen, "river"))         return LAY_WATER;
    if (mvt_name_contains(name, nlen, "lake"))          return LAY_WATER;
    if (mvt_name_contains(name, nlen, "sea"))           return LAY_WATER;

    // Parks / greenery / landuse / landcover
    if (mvt_name_contains(name, nlen, "park"))          return LAY_PARKS;
    if (mvt_name_contains(name, nlen, "green"))         return LAY_PARKS;
    if (mvt_name_contains(name, nlen, "landuse"))       return LAY_PARKS;
    if (mvt_name_contains(name, nlen, "landcover"))     return LAY_PARKS;
    if (mvt_name_contains(name, nlen, "forest"))        return LAY_PARKS;
    if (mvt_name_contains(name, nlen, "wood"))          return LAY_PARKS;
    if (mvt_name_contains(name, nlen, "grass"))         return LAY_PARKS;

    // Schools / education
    if (mvt_name_contains(name, nlen, "school"))        return LAY_SCHOOLS;
    if (mvt_name_contains(name, nlen, "education"))     return LAY_SCHOOLS;

    // Shops / amenities
    if (mvt_name_contains(name, nlen, "shop"))          return LAY_SHOPS;
    if (mvt_name_contains(name, nlen, "amenity"))       return LAY_SHOPS;

    // POIs / labels / places
    if (mvt_name_contains(name, nlen, "poi"))           return LAY_POI;
    if (mvt_name_contains(name, nlen, "place"))         return LAY_POI;
    if (mvt_name_contains(name, nlen, "label"))         return LAY_POI;

    // Boundaries / admin
    if (mvt_name_contains(name, nlen, "boundary"))      return LAY_BOUNDARY;
    if (mvt_name_contains(name, nlen, "admin"))         return LAY_BOUNDARY;
    if (mvt_name_contains(name, nlen, "border"))        return LAY_BOUNDARY;

    // Unknown -- render as thin grey outline (see MVT_DRAW_UNKNOWN) so the
    // user still sees SOMETHING and the Serial log tells us the name so we
    // can bake in a hard mapping next revision.
    return LAY_UNKNOWN;
}

// =============================================================================
//  In-memory structures (kept small; heap tuned for ESP32-S3 PSRAM)
// =============================================================================
struct FVec2 { int32_t x, y; };

struct FMapObj {
    uint8_t type;        // OBJ_LINE / OBJ_POLY / OBJ_POINT
    uint8_t layer;       // LayerId
    uint8_t zmin, zmax;  // slippy zoom visibility range
    uint8_t flags;       // bit0 = filled polygon
    uint16_t point_count;
    FVec2 *points;       // heap alloc
};

struct FMapPOI {
    uint8_t type;
    int32_t x, y;
    uint8_t icon_id;
    char *name;
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

// Loaded tile (decoded objects held in RAM/PSRAM). Keyed by (z, x, y).
struct FMapTile {
    int8_t   z;
    int16_t  tx, ty;
    bool     loaded;
    uint16_t obj_count;
    FMapObj *objs;
    uint32_t last_use_ms;
};

#define MAX_ZOOMS   8
#define TILE_CACHE_SLOTS 16   // 3x3 ring + spares at boundary between zoom levels
#define MAX_POIS 2048

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
static int8_t     g_last_cached_z = -1;        // used to flush cache on zoom-change

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

// Skip a protobuf field of the given wire type. Used to skip fields we don't
// care about (keys, values, feature.id, etc.).
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
// =============================================================================
static void tile_free(FMapTile &t) {
    if (t.objs) {
        for (uint16_t i=0; i<t.obj_count; i++) {
            if (t.objs[i].points) free(t.objs[i].points);
        }
        free(t.objs);
        t.objs = nullptr;
    }
    t.obj_count = 0;
    t.loaded = false;
}
static void cache_reset() {
    for (int i=0; i<TILE_CACHE_SLOTS; i++) tile_free(g_cache[i]);
}
static FMapTile *cache_find(int8_t z, int16_t tx, int16_t ty) {
    for (int i=0; i<TILE_CACHE_SLOTS; i++)
        if (g_cache[i].loaded && g_cache[i].z==z &&
            g_cache[i].tx==tx && g_cache[i].ty==ty) {
            g_cache[i].last_use_ms = millis();
            return &g_cache[i];
        }
    return nullptr;
}
static FMapTile *cache_evict_slot() {
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
//  Parses a raw MVT byte buffer and decodes geometry into FMapObj entries.
//  extent handling: features quantise into [0..extent] per tile. Scaled to
//  world units by (world_units_per_tile / extent).
// =============================================================================
struct MVTDecodeCtx {
    // World transform to apply to feature-local (lx, ly) coords:
    //   world_x = ox + lx * sx
    //   world_y = oy - ly * sy      (mercator y flip)
    double ox, oy;
    double sx, sy;
    int8_t z;                 // slippy zoom of this tile (for zmin/zmax on decoded objects)
    // POI collection buffer for this tile
};

static bool poi_append(uint8_t type, int32_t wx, int32_t wy, const String &name);

// Decode one Feature { geometry:[u32], type:GeomType }. Returns objects created (0..N).
static uint16_t mvt_decode_feature(const uint8_t *pfeat, const uint8_t *fend,
                                   uint32_t extent, const MVTDecodeCtx &c,
                                   const char *layer_name, uint8_t layer_id,
                                   FMapObj *out_arr, uint16_t out_cap,
                                   String *inout_poi_name) {
    uint8_t geom_type = MVT_UNKNOWN;
    const uint8_t *geom_p = nullptr;
    uint32_t geom_len = 0;
    // Feature.tags = 2 (packed varint), Feature.type = 3, Feature.geometry = 4, Feature.id = 1
    while (pfeat < fend) {
        uint32_t key = pb_varint(pfeat, fend);
        uint8_t wire = key & 7;
        uint32_t field = key >> 3;
        if (field == 3 && wire == 0) {
            geom_type = (uint8_t)pb_varint(pfeat, fend);
        } else if (field == 4 && wire == 2) {
            geom_len = pb_varint(pfeat, fend);
            geom_p = pfeat;
            if (pfeat + geom_len > fend) { pfeat = fend; }
            else pfeat += geom_len;
        } else {
            pb_skip(pfeat, fend, wire);
        }
    }
    if (!geom_p || geom_len == 0 || geom_type == MVT_UNKNOWN) return 0;

    // Geometry command decoding
    const uint8_t *gp = geom_p;
    const uint8_t *gend = geom_p + geom_len;

    // scale: local -> world
    double sx = c.sx / (double)extent;
    double sy = c.sy / (double)extent;

    int32_t cur_x = 0, cur_y = 0;       // MVT command cursor (local units)

    // Ring / linestring buffer
    // Use a resizable heap buffer; typical rings ~10..200 points, occasional larger.
    struct RingBuf {
        FVec2 *pts;
        uint16_t n, cap;
    };
    auto ring_reset = [](RingBuf &r) { r.n = 0; };
    auto ring_push = [](RingBuf &r, int32_t wx, int32_t wy) -> bool {
        if (r.n >= r.cap) {
            uint16_t nc = r.cap ? r.cap * 2 : 32;
            if (nc > 2048) nc = 2048;
            if (nc == r.cap) return false;
            FVec2 *np = (FVec2*)realloc(r.pts, sizeof(FVec2) * nc);
            if (!np) return false;
            r.pts = np; r.cap = nc;
        }
        r.pts[r.n].x = wx; r.pts[r.n].y = wy;
        r.n++;
        return true;
    };

    RingBuf ring = {0};
    uint16_t made = 0;

    auto emit_obj = [&](uint8_t objtype, uint8_t flags) {
        if (ring.n < 2 && objtype != OBJ_POINT) return;
        if (made >= out_cap) return;
        FMapObj &o = out_arr[made];
        o.type = objtype;
        o.layer = layer_id;
        o.zmin = 0;
        o.zmax = 22;
        o.flags = flags;
        o.point_count = ring.n;
        o.points = (FVec2*)malloc(sizeof(FVec2) * ring.n);
        if (!o.points) { o.point_count = 0; return; }
        memcpy(o.points, ring.pts, sizeof(FVec2) * ring.n);
        made++;
    };

    while (gp < gend) {
        uint32_t cmd = pb_varint(gp, gend);
        uint8_t  id  = cmd & 7;
        uint32_t count = cmd >> 3;
        if (id == 1) {   // MoveTo
            // MoveTo starts a new sub-geometry
            if (geom_type == MVT_POLYGON) {
                // In polygons, MoveTo may start a new ring; emit the previous
                // ring as its own polygon so the scanline filler sees it.
                if (ring.n >= 3) emit_obj(OBJ_POLY, 0x01);
                ring_reset(ring);
            } else if (geom_type == MVT_LINESTRING) {
                if (ring.n >= 2) emit_obj(OBJ_LINE, 0);
                ring_reset(ring);
            }
            for (uint32_t k = 0; k < count && gp < gend; k++) {
                int32_t dx = zz_decode(pb_varint(gp, gend));
                int32_t dy = zz_decode(pb_varint(gp, gend));
                cur_x += dx; cur_y += dy;
                int32_t wx = (int32_t)(c.ox + (double)cur_x * sx);
                int32_t wy = (int32_t)(c.oy - (double)cur_y * sy);
                if (geom_type == MVT_POINT) {
                    // Each MoveTo point is its own point-object.
                    if (made < out_cap) {
                        FMapObj &o = out_arr[made];
                        o.type = OBJ_POINT;
                        o.layer = layer_id;
                        o.zmin = 0; o.zmax = 22;
                        o.flags = 0;
                        o.point_count = 1;
                        o.points = (FVec2*)malloc(sizeof(FVec2));
                        if (o.points) {
                            o.points[0].x = wx;
                            o.points[0].y = wy;
                            made++;
                            // Also register as POI when in a POI-ish layer
                            if (layer_id == LAY_SHOPS || layer_id == LAY_SCHOOLS ||
                                layer_id == LAY_TRANSIT) {
                                uint8_t pt = (layer_id == LAY_SCHOOLS) ? 4 :
                                             (layer_id == LAY_TRANSIT) ? 3 : 1;
                                poi_append(pt, wx, wy, inout_poi_name ? *inout_poi_name : String(""));
                            }
                        } else {
                            o.point_count = 0;
                        }
                    }
                } else {
                    ring_push(ring, wx, wy);
                }
            }
        } else if (id == 2) {  // LineTo
            for (uint32_t k = 0; k < count && gp < gend; k++) {
                int32_t dx = zz_decode(pb_varint(gp, gend));
                int32_t dy = zz_decode(pb_varint(gp, gend));
                cur_x += dx; cur_y += dy;
                int32_t wx = (int32_t)(c.ox + (double)cur_x * sx);
                int32_t wy = (int32_t)(c.oy - (double)cur_y * sy);
                ring_push(ring, wx, wy);
            }
        } else if (id == 7) {  // ClosePath (polygons only)
            if (geom_type == MVT_POLYGON && ring.n >= 3) {
                // Close by pushing first point again if not already closed
                if (ring.pts[0].x != ring.pts[ring.n-1].x ||
                    ring.pts[0].y != ring.pts[ring.n-1].y) {
                    ring_push(ring, ring.pts[0].x, ring.pts[0].y);
                }
                emit_obj(OBJ_POLY, 0x01);
                ring_reset(ring);
            }
        } else {
            // Unknown command; abort to keep decoder safe
            break;
        }
    }
    // Flush trailing ring / linestring
    if (geom_type == MVT_POLYGON && ring.n >= 3) emit_obj(OBJ_POLY, 0x01);
    else if (geom_type == MVT_LINESTRING && ring.n >= 2) emit_obj(OBJ_LINE, 0);

    if (ring.pts) free(ring.pts);
    (void)layer_name;
    return made;
}

// Decode one Layer { name, features:[Feature], extent }. Appends FMapObj to *out_objs.
// Returns number of objects added.
static uint32_t mvt_decode_layer(const uint8_t *plyr, const uint8_t *lend,
                                 const MVTDecodeCtx &c,
                                 FMapObj **out_objs, uint32_t *cap, uint32_t used) {
    // First pass: extract layer name + extent, remember features spans.
    const uint8_t *p = plyr;
    char lname[48] = {0}; uint16_t lname_len = 0;
    uint32_t extent = MVT_EXTENT_DEFAULT;
    // We collect feature slices (start, len) up to a cap to avoid re-scanning.
    struct FSlice { const uint8_t *p; uint32_t n; };
    static FSlice slices[512];
    int nslices = 0;
    while (p < lend) {
        uint32_t key = pb_varint(p, lend);
        uint8_t wire = key & 7;
        uint32_t field = key >> 3;
        if (field == 1 && wire == 2) {                // name
            uint32_t n = pb_varint(p, lend);
            uint16_t k = (n < sizeof(lname) - 1) ? (uint16_t)n : (uint16_t)(sizeof(lname) - 1);
            memcpy(lname, p, k); lname[k] = 0; lname_len = k;
            if (p + n <= lend) p += n; else p = lend;
        } else if (field == 5 && wire == 0) {         // extent
            extent = pb_varint(p, lend);
        } else if (field == 2 && wire == 2) {         // Feature
            uint32_t n = pb_varint(p, lend);
            if (p + n <= lend) {
                if (nslices < (int)(sizeof(slices)/sizeof(slices[0]))) {
                    slices[nslices].p = p;
                    slices[nslices].n = n;
                    nslices++;
                }
                p += n;
            } else { p = lend; }
        } else {
            pb_skip(p, lend, wire);
        }
    }
     if (nslices == 0 || lname_len == 0 || extent == 0) {
        MVT_LOG("[LAYER] name='%.*s' extent=%u features=%u -> skipped (empty/malformed)\n",
                (int)lname_len, lname, (unsigned)extent, (unsigned)nslices);
        return 0;
    }

    uint8_t layer_id = mvt_layer_id(lname, lname_len);
#if !MVT_DRAW_UNKNOWN
    if (layer_id == LAY_UNKNOWN) {
        MVT_LOG("[LAYER] name='%.*s' extent=%u features=%u mapped_id=UNKNOWN -> dropped\n",
                (int)lname_len, lname, (unsigned)extent, (unsigned)nslices);
        return 0;
    }
#endif
    if (layer_id == 0xFF) {
        MVT_LOG("[LAYER] name='%.*s' extent=%u features=%u -> dropped (label-only)\n",
                (int)lname_len, lname, (unsigned)extent, (unsigned)nslices);
        return 0;
    }
    MVT_LOG("[LAYER] name='%.*s' extent=%u features=%u mapped_id=%u\n",
            (int)lname_len, lname, (unsigned)extent, (unsigned)nslices, (unsigned)layer_id);

    uint32_t added = 0;
    for (int i = 0; i < nslices; i++) {
        // Grow output buffer as needed (up to a safe cap).
        if (used + added + 16 > *cap) {
            uint32_t nc = *cap ? (*cap * 2) : 128;
            if (nc > 6000) nc = 6000;
            if (nc == *cap) break;
            FMapObj *nb = (FMapObj*)realloc(*out_objs, sizeof(FMapObj) * nc);
            if (!nb) break;
            *out_objs = nb;
            *cap = nc;
        }
       String noname;
        added += mvt_decode_feature(slices[i].p, slices[i].p + slices[i].n,
                                    extent, c, lname, layer_id,
                                    (*out_objs) + used + added, *cap - used - added,
                                    &noname);
    }
    MVT_LOG("[LAYER] name='%.*s' -> added %u objects\n",
            (int)lname_len, lname, (unsigned)added);
    return added;
}

// =============================================================================
//  MVT tile loader
// =============================================================================
static String mvt_tile_path(int8_t z, int16_t x, int16_t y) {
    // tilemaker default output: {z}/{x}/{y}.pbf ; some tools write .mvt.
    // SD_MMC on ESP32 (FATFS) filenames come back in the ORIGINAL case they
    // were written -- some converters (and Windows) upper-case the extension,
    // e.g. "608.PBF". We therefore probe all four permutations before giving up.
    char b[80];
    snprintf(b, sizeof(b), "/%u/%d/%d", (unsigned)z, (int)x, (int)y);
    String base = g_map_dir_path + String(b);
    static const char *EXTS[] = { ".pbf", ".PBF", ".mvt", ".MVT" };
    for (unsigned i = 0; i < sizeof(EXTS)/sizeof(EXTS[0]); i++) {
        String candidate = base + EXTS[i];
        if (SD_MMC.exists(candidate.c_str())) return candidate;
    }
    return String();
}


// Returns true if a tile file exists on disk.
static bool tile_exists_on_disk(int8_t z, int16_t x, int16_t y) {
    return mvt_tile_path(z, x, y).length() > 0;
}

// -----------------------------------------------------------------------------
// Tile-render diagnostics counters (visible on the map HUD as "tiles: N/M").
//   g_hud_tiles_ok    = tiles the ring drew that had >=1 decoded object
//   g_hud_tiles_empty = tiles the ring drew that had 0 decoded objects
// Reset each frame in draw_map_frame() before iterating the 3x3 ring.
// -----------------------------------------------------------------------------
static uint16_t g_hud_tiles_ok    = 0;
static uint16_t g_hud_tiles_empty = 0;

// Load one MVT tile into a cache slot.
static FMapTile *tile_load(int8_t z, int16_t x, int16_t y) {
    String path = mvt_tile_path(z, x, y);
    if (!path.length()) {
        MVT_LOG("[TILE] z=%d x=%d y=%d -- file not found on SD\n", (int)z, (int)x, (int)y);
        return nullptr;
    }

    FMapTile *slot = cache_evict_slot();
    slot->z = z; slot->tx = x; slot->ty = y;
    slot->loaded = false;
    slot->obj_count = 0; slot->objs = nullptr;
    slot->last_use_ms = millis();

    File f = SD_MMC.open(path, FILE_READ);
    if (!f) {
        MVT_LOG("[TILE] z=%d x=%d y=%d path='%s' -- SD open FAILED\n",
                (int)z, (int)x, (int)y, path.c_str());
        return slot;
    }
    size_t sz = f.size();
    MVT_LOG("[TILE] z=%d x=%d y=%d path='%s' bytes=%u\n",
            (int)z, (int)x, (int)y, path.c_str(), (unsigned)sz);
    if (!sz || sz > 512 * 1024) {
        MVT_LOG("[TILE]   size out of bounds -- ignored\n");
        f.close(); slot->loaded = true; return slot;
    }

    uint8_t *buf = (uint8_t*)ps_malloc(sz);
    if (!buf) buf = (uint8_t*)malloc(sz);
    if (!buf) {
        MVT_LOG("[TILE]   malloc(%u) FAILED -- skipping\n", (unsigned)sz);
        f.close(); slot->loaded = true; return slot;
    }
    size_t got = f.read(buf, sz);
    f.close();
    if (got != sz) {
        MVT_LOG("[TILE]   short read got=%u expected=%u -- skipping\n",
                (unsigned)got, (unsigned)sz);
        free(buf); slot->loaded = true; return slot;
    }

    // Defensive gzip guard: MVT tiles on the wire are usually gzipped, but on
    // disk the WaypointOS pipeline expects them UNCOMPRESSED.
    if (sz >= 2 && buf[0] == 0x1F && buf[1] == 0x8B) {
        MVT_LOG("[TILE]   GZIP detected (1F 8B) -- not supported. "
                "Re-export tiles uncompressed (tilemaker --compress none).\n");
        free(buf); slot->loaded = true; return slot;
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
        return slot;
    }
    FMapObj *trimmed = (FMapObj*)realloc(objs, sizeof(FMapObj) * used);
    slot->objs = trimmed ? trimmed : objs;
    slot->obj_count = (uint16_t)(used > 65535 ? 65535 : used);
    slot->loaded = true;
    return slot;
}
// =============================================================================
//  POI accumulation (populated as MVT tiles are decoded)
// =============================================================================
static bool poi_append(uint8_t type, int32_t wx, int32_t wy, const String &name) {
    // De-dup by proximity (< 4 world units) & same type
    for (uint32_t i = 0; i < g_poi_count; i++) {
        if (g_pois[i].type == type &&
            abs((long)g_pois[i].x - (long)wx) < 4 &&
            abs((long)g_pois[i].y - (long)wy) < 4) return false;
    }
    if (g_poi_count >= MAX_POIS) return false;
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
    g_pois[g_poi_count].type    = type;
    g_pois[g_poi_count].x       = wx;
    g_pois[g_poi_count].y       = wy;
    g_pois[g_poi_count].icon_id = 0;
    g_pois[g_poi_count].name    = name.length() ? strdup(name.c_str()) : nullptr;
    g_poi_count++;
    return true;
}
static void poi_reset() {
    if (g_pois) {
        for (uint32_t i=0; i<g_poi_count; i++) if (g_pois[i].name) free(g_pois[i].name);
        free(g_pois); g_pois = nullptr;
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
    for (uint8_t z = g_meta.minzoom; z <= g_meta.maxzoom && g_zoom_count < MAX_ZOOMS; z++) {
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

// =============================================================================
//  Camera + projection : world (int32) -> screen (int)
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
    if (best != g_cam_zoom) {
        // Zoom level changed -- old-zoom tiles are no longer needed.
        cache_reset();
    }
    g_cam_zoom = best;
    g_last_cached_z = (int8_t)best;
}

// =============================================================================
//  Vector primitive rendering (Arduino_GFX)
// =============================================================================
static inline bool onscreen(int x, int y) { return x>=0 && y>=0 && x<LCD_WIDTH && y<LCD_HEIGHT; }

static void draw_thick_line(int x0, int y0, int x1, int y1, int w, uint16_t c) {
    if (w <= 1) { gfx->drawLine(x0,y0,x1,y1,c); return; }
    // Cheap thick line: perpendicular offsets
    float dx = x1-x0, dy = y1-y0;
    float len = sqrtf(dx*dx + dy*dy);
    if (len < 0.5f) { gfx->fillCircle(x0,y0, w/2, c); return; }
    float px = -dy/len, py = dx/len;
    int half = w/2;
    for (int off = -half; off <= half; off++) {
        int ox0 = x0 + (int)(px*off), oy0 = y0 + (int)(py*off);
        int ox1 = x1 + (int)(px*off), oy1 = y1 + (int)(py*off);
        gfx->drawLine(ox0,oy0,ox1,oy1,c);
    }
}

// Draw polyline in world coords, clipping by screen bounds
static void draw_polyline_world(const FVec2 *pts, int n, uint16_t col, uint8_t width, uint8_t dash) {
    if (n < 2) return;
    int sx_prev=0, sy_prev=0;
    world_to_screen(pts[0].x, pts[0].y, &sx_prev, &sy_prev);
    int dash_on = dash ? dash : 0;
    int step = 0;
    for (int i=1; i<n; i++) {
        int sx, sy;
        world_to_screen(pts[i].x, pts[i].y, &sx, &sy);
        // trivial reject if both endpoints far offscreen
        bool skip = (sx < -50 && sx_prev < -50) ||
                    (sy < -50 && sy_prev < -50) ||
                    (sx > LCD_WIDTH+50 && sx_prev > LCD_WIDTH+50) ||
                    (sy > LCD_HEIGHT+50 && sy_prev > LCD_HEIGHT+50);
        if (!skip) {
            if (dash_on == 0 || (step % (dash_on*2)) < dash_on) {
                draw_thick_line(sx_prev, sy_prev, sx, sy, width, col);
            }
        }
        sx_prev = sx; sy_prev = sy;
        step++;
    }
}

// Scanline polygon fill (in screen space, with rotation applied via world_to_screen)
static void draw_polygon_filled(const FVec2 *pts, int n, uint16_t col) {
    if (n < 3) return;
    // Project to screen once
    static int16_t sx[256], sy[256];
    int m = n > 256 ? 256 : n;
    int ymin=LCD_HEIGHT, ymax=-1;
    for (int i=0; i<m; i++) {
        int ix, iy;
        world_to_screen(pts[i].x, pts[i].y, &ix, &iy);
        sx[i] = (int16_t)ix; sy[i] = (int16_t)iy;
        if (iy < ymin) ymin = iy;
        if (iy > ymax) ymax = iy;
    }
    if (ymin < 0) ymin = 0;
    if (ymax >= LCD_HEIGHT) ymax = LCD_HEIGHT-1;
    if (ymax < 0 || ymin >= LCD_HEIGHT) return;

    int x_int[64];
    for (int y = ymin; y <= ymax; y++) {
        int cnt = 0;
        for (int i=0, j=m-1; i<m; j=i++) {
            int yi = sy[i], yj = sy[j];
            if ((yi <= y && yj > y) || (yj <= y && yi > y)) {
                int xi = sx[i] + (y - yi) * (sx[j] - sx[i]) / (yj - yi + (yj==yi?1:0));
                if (cnt < 64) x_int[cnt++] = xi;
            }
        }
        // sort
        for (int a=1; a<cnt; a++) {
            int v = x_int[a], b = a-1;
            while (b>=0 && x_int[b] > v) { x_int[b+1] = x_int[b]; b--; }
            x_int[b+1] = v;
        }
        for (int a=0; a+1<cnt; a+=2) {
            int xa = x_int[a], xb = x_int[a+1];
            if (xa < 0) xa = 0;
            if (xb >= LCD_WIDTH) xb = LCD_WIDTH-1;
            if (xb >= xa) gfx->drawFastHLine(xa, y, xb-xa+1, col);
        }
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
struct MapEntry { String path; String title; String desc; };
static MapEntry g_maps[MAX_MAPS];
static int g_map_count_lib = 0;
static int g_map_sel = 0;
static bool g_lib_redraw = true;

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
                        g_maps[g_map_count_lib].path  = path;
                        g_maps[g_map_count_lib].title = m.title.length() ? m.title : n;
                        g_maps[g_map_count_lib].desc  = m.desc.length()  ? m.desc
                                                       : String("Vector map (MVT)");
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
    gfx->setCursor(cx - 92, 28); gfx->print("WAYPOINT / OS");
    gfx->setTextColor(t.text_dim); gfx->setTextSize(1);
    gfx->setCursor(cx - 48, 56); gfx->print("map library");

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
            "",
            "BOOT long-press -> launcher",
        };
        int y = 120;
        for (auto &ln : lines) { gfx->setCursor(28, y); gfx->print(ln); y += 16; }
        return;
    }

    MapEntry &e = g_maps[g_map_sel];
    int card_x = 20, card_y = 110, card_w = LCD_WIDTH - 40, card_h = 240;
    gfx->fillRoundRect(card_x, card_y, card_w, card_h, 16, t.chrome_bg);
    gfx->drawRoundRect(card_x, card_y, card_w, card_h, 16, t.accent);

    gfx->setTextColor(t.text); gfx->setTextSize(2);
    gfx->setCursor(card_x + 18, card_y + 20);
    String shown = e.title.length() > 18 ? e.title.substring(0,17) + "~" : e.title;
    gfx->print(shown);

    gfx->setTextColor(t.text_dim); gfx->setTextSize(1);
    gfx->setCursor(card_x + 18, card_y + 50);
    String fn = e.path; int p = fn.lastIndexOf('/'); if (p>=0) fn = fn.substring(p+1);
    gfx->print("dir: "); gfx->print(fn);

    // description with wrap
    gfx->setTextColor(t.text); gfx->setTextSize(1);
    int max_chars = (card_w - 36) / 6;
    int dy = card_y + 80;
    String d = e.desc;
    while (d.length() > 0 && dy < card_y + card_h - 14) {
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

    gfx->setTextColor(t.accent); gfx->setTextSize(3);
    gfx->setCursor(card_x - 14, card_y + card_h/2 - 12);
    if (g_map_count_lib > 1) gfx->print("<");
    gfx->setCursor(card_x + card_w - 4, card_y + card_h/2 - 12);
    if (g_map_count_lib > 1) gfx->print(">");

    gfx->setTextColor(t.accent); gfx->setTextSize(1);
    char counter[32];
    snprintf(counter, sizeof(counter), "%d / %d", g_map_sel + 1, g_map_count_lib);
    gfx->setCursor(cx - 16, card_y + card_h + 14);
    gfx->print(counter);

    // hints
    int hy = LCD_HEIGHT - 96;
    gfx->setTextColor(t.text_dim);
    gfx->setCursor(20, hy);      gfx->print("BOOT tap        -> next");
    gfx->setCursor(20, hy + 14); gfx->print("BOOT 2x tap     -> prev");
    gfx->setCursor(20, hy + 28); gfx->print("BOOT hold 0.5s  -> open");
    gfx->setCursor(20, hy + 42); gfx->print("BOOT hold 1.5s  -> launcher");
    gfx->setCursor(20, hy + 56); gfx->print("Touch: swipe to browse");
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

// Ensure current + neighbouring tiles are loaded at the active slippy zoom.
static void ensure_ring_loaded(int8_t z, int16_t ctx, int16_t cty) {
    int32_t nmax = (1L << z) - 1;
    for (int j=-1; j<=1; j++) {
        for (int i=-1; i<=1; i++) {
            int16_t tx = ctx + i, ty = cty + j;
            if (tx < 0 || ty < 0 || tx > nmax || ty > nmax) continue;
            if (cache_find(z, tx, ty)) continue;
            if (!tile_exists_on_disk(z, tx, ty)) continue;
            tile_load(z, tx, ty);
        }
    }
}

static void draw_map_frame();  // fwd
static bool g_need_map_redraw = true;

static void render_tile_objects(FMapTile *t) {
    if (!t || !t->loaded) return;
    for (uint16_t i=0; i<t->obj_count; i++) {
        FMapObj &o = t->objs[i];
        if (o.layer >= LAY__COUNT) continue;
        if (!(g_layer_mask & (1u << o.layer))) continue;
        if (g_cam_zoom < o.zmin || g_cam_zoom > o.zmax) continue;

        // Style lookup: prefer per-layer style overrides; else theme default.
        uint16_t col = layer_colour(o.layer);
        uint8_t width = 1;
        uint8_t dash = 0;
        if (g_styles[o.layer].set) {
            col = g_styles[o.layer].colour;
            width = g_styles[o.layer].width_px;
            dash = g_styles[o.layer].dash_len;
          } else {
            width = (o.layer == LAY_ROADS) ? 3 : (o.layer == LAY_PATHS ? 1 : 2);
            if (o.layer == LAY_PATHS) dash = 4;
            // Unknown-layer bucket: thin grey outline only, no fill, no dash.
            if (o.layer == LAY_UNKNOWN) { width = 1; dash = 0; }
        }

        switch (o.type) {
            case OBJ_LINE:
                draw_polyline_world(o.points, o.point_count, col, width, dash);
                g_render_obj_count++;
                break;
           case OBJ_POLY:
                if ((o.flags & 0x01) && o.layer != LAY_UNKNOWN) {
                    draw_polygon_filled(o.points, o.point_count, col);
                }
                draw_polyline_world(o.points, o.point_count, col, 1, 0);
                g_render_obj_count++;
                break;
            case OBJ_POINT:
                if (o.point_count > 0) {
                    int sx, sy;
                    world_to_screen(o.points[0].x, o.points[0].y, &sx, &sy);
                    if (sx > -8 && sy > -8 && sx < LCD_WIDTH+8 && sy < LCD_HEIGHT+8) {
                        gfx->fillCircle(sx, sy, 3, col);
                        gfx->drawCircle(sx, sy, 4, TH().text);
                        g_render_obj_count++;
                    }
                }
                break;
        }
    }
}

// Draw POIs (points, not tile-bound)
static void render_pois() {
    if (!g_pois) return;
    const ThemePalette &t = TH();
    for (uint32_t i=0; i<g_poi_count; i++) {
        int sx, sy;
        world_to_screen(g_pois[i].x, g_pois[i].y, &sx, &sy);
        if (sx < -6 || sy < -6 || sx > LCD_WIDTH+6 || sy > LCD_HEIGHT+6) continue;
        // small dot; icon-specific later
        uint16_t c = t.warn;
        switch (g_pois[i].type) {
            case 1: c = t.shops;   break;
            case 2: c = t.shops;   break;
            case 3: c = t.transit; break;
            case 4: c = t.schools; break;
            case 5: c = t.parks;   break;
            case 6: c = t.warn;    break;
        }
        gfx->fillCircle(sx, sy, 4, c);
        gfx->drawCircle(sx, sy, 5, t.text);
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
//  On-screen map HUD buttons (touch-driven controls). Kept small enough to
//  keep the vector map dominant but always visible so the watch is usable
//  without knowing the BOOT-button chords.
// -----------------------------------------------------------------------------
enum HudBtn : int { HUD_ZOOM_IN=0, HUD_ZOOM_OUT, HUD_TOOLS, HUD_RECENTRE, HUD_ROTATE, HUD__COUNT };
struct HudRect { int x, y, w, h; };
static HudRect hud_rect(HudBtn b) {
    // Right-hand column of buttons, then left-hand tools+recentre stack.
    const int BW = 44, BH = 44, GAP = 8;
    int right_x = LCD_WIDTH - BW - 10;
    int left_x  = 10;
    int bot_y   = LCD_HEIGHT - BH - 12;
    switch (b) {
        case HUD_ZOOM_IN:  return { right_x, bot_y - (BH + GAP), BW, BH };
        case HUD_ZOOM_OUT: return { right_x, bot_y,               BW, BH };
        case HUD_TOOLS:    return { left_x,  bot_y,               BW, BH };
        case HUD_RECENTRE: return { left_x,  bot_y - (BH + GAP), BW, BH };
        case HUD_ROTATE:   return { left_x + BW + GAP, bot_y,     BW, BH };
        default: return { 0,0,0,0 };
    }
}
static void hud_draw_one(HudBtn id, const char *glyph, uint16_t colour) {
    const ThemePalette &t = TH();
    HudRect r = hud_rect(id);
    gfx->fillRoundRect(r.x, r.y, r.w, r.h, 10, t.chrome_bg);
    gfx->drawRoundRect(r.x, r.y, r.w, r.h, 10, t.chrome_border);
    gfx->setTextColor(colour);
    gfx->setTextSize(2);
    // rough centring: 2-char glyphs offset differently than 1-char ones
    int gw = (int)strlen(glyph) * 12;
    gfx->setCursor(r.x + (r.w - gw)/2, r.y + (r.h - 14)/2);
    gfx->print(glyph);
}
static void draw_hud_buttons() {
    const ThemePalette &t = TH();
    hud_draw_one(HUD_ZOOM_IN,  "+",   t.accent);
    hud_draw_one(HUD_ZOOM_OUT, "-",   t.accent);
    hud_draw_one(HUD_TOOLS,    "::",  t.accent);
    hud_draw_one(HUD_RECENTRE, "o",   t.accent);
    hud_draw_one(HUD_ROTATE,   g_rotate_with_heading ? "N*" : "N",
                 g_rotate_with_heading ? t.warn : t.accent);
    (void)t;
}
static int hud_hit_test(int sx, int sy) {
    for (int i=0; i<HUD__COUNT; i++) {
        HudRect r = hud_rect((HudBtn)i);
        if (sx >= r.x && sx < r.x + r.w && sy >= r.y && sy < r.y + r.h) return i;
    }
    return -1;
}

// Scale bar (bottom-centre): shows a real-world reference distance
static void draw_scale_bar() {
    if (g_meta.units_per_metre <= 0 || g_cam_scale <= 0) return;
    const ThemePalette &t = TH();
    // metres per pixel = 1 / (cam_scale * units_per_metre)
    float m_per_px = 1.0f / (g_cam_scale * g_meta.units_per_metre);
    if (!isfinite(m_per_px) || m_per_px <= 0) return;
    // Choose a "nice" length: 1,2,5,10,20,50,100,200,500,1000,...
    static const float nice[] = { 1,2,5,10,20,50,100,200,500,1000,2000,5000,10000 };
    float target_px = 90.0f;
    float chosen = nice[0];
    for (unsigned i=0; i<sizeof(nice)/sizeof(nice[0]); i++) {
        float px = nice[i] / m_per_px;
        if (px >= 40 && px <= 160) { chosen = nice[i]; target_px = px; break; }
        if (px < 160) { chosen = nice[i]; target_px = px; }
    }
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
    char b[64];
    const char *title = g_meta.title.length() ? g_meta.title.c_str() : "map";
    snprintf(b, sizeof(b), "%s  z%u", title, (unsigned)g_cam_zoom);
    int w = (int)strlen(b) * 6 + 14;
    if (w > LCD_WIDTH - 100) w = LCD_WIDTH - 100;
    gfx->fillRoundRect(8, 8, w, 22, 4, t.chrome_bg);
    gfx->drawRoundRect(8, 8, w, 22, 4, t.chrome_border);
  gfx->setTextColor(t.accent); gfx->setTextSize(1);
    gfx->setCursor(16, 16); gfx->print(b);

    // Tile-status badge -- shows how many of the currently visible 3x3 ring
    // decoded to at least one object vs how many parsed to 0 objects. If the
    // whole ring is "empty" the camera is off-map; if it's a mix, the tile
    // parser is dropping features (usually unknown layer names).
    {
        char tb[32];
        snprintf(tb, sizeof(tb), "tiles %u/%u",
                 (unsigned)g_hud_tiles_ok,
                 (unsigned)(g_hud_tiles_ok + g_hud_tiles_empty));
        int tw = (int)strlen(tb) * 6 + 12;
        int tx = 8, ty = 34;
        uint16_t bcol = (g_hud_tiles_empty == 0 && g_hud_tiles_ok > 0) ? t.accent : t.warn;
        gfx->fillRoundRect(tx, ty, tw, 18, 4, t.chrome_bg);
        gfx->drawRoundRect(tx, ty, tw, 18, 4, bcol);
        gfx->setTextColor(bcol); gfx->setTextSize(1);
        gfx->setCursor(tx + 6, ty + 5);
        gfx->print(tb);
    }

    // crosshair
    int cx = LCD_WIDTH/2, cy = LCD_HEIGHT/2;
    gfx->drawLine(cx - 10, cy, cx + 10, cy, t.accent);
    gfx->drawLine(cx, cy - 10, cx, cy + 10, t.accent);
    gfx->drawCircle(cx, cy, 4, t.accent);

    draw_scale_bar();
    draw_hud_buttons();
}

static void draw_map_frame() {
    if (!g_need_map_redraw) return;
    g_need_map_redraw = false;
    uint32_t t0 = micros();

    const ThemePalette &t = TH();
    gfx->fillScreen(t.bg);
    g_render_obj_count = 0;

    // Determine current camera tile at the currently selected slippy zoom.
    int8_t   z   = (int8_t)g_cam_zoom;
    int shift = g_meta.refzoom - z;
    double side = (double)MVT_EXTENT_DEFAULT *
                  ((shift >= 0) ? (double)(1u << shift) : 1.0/(double)(1u << -shift));
    // world_x -> tile x  (world x grows east, tile x grows east)
    int16_t ctx = (int16_t)floor(g_cam_x / side);
    // world_y is +Y = north; tile y grows south -> negate before dividing
    int16_t cty = (int16_t)floor(-g_cam_y / side);
    ensure_ring_loaded(z, ctx, cty);

    // Draw ring
    g_hud_tiles_ok = 0;
    g_hud_tiles_empty = 0;
    for (int j=-1; j<=1; j++) {
        for (int i=-1; i<=1; i++) {
            FMapTile *tl = cache_find(z, ctx + i, cty + j);
            if (tl) {
                if (tl->obj_count > 0) g_hud_tiles_ok++;
                else                    g_hud_tiles_empty++;
                render_tile_objects(tl);
            }
        }
    }
    render_saved();
    render_beacons();
    render_pois();
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
    if (g_layer_mask & (1u << LAY_BEACONS)) {
        // handled in render_beacons
    }
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
static void layers_toggle(uint8_t layer) {
    g_layer_mask ^= (1u << layer);
    Preferences p; p.begin("waypoint", false);
    String key = String("lm_") + g_meta.title;
    if (key.length() > 15) key = key.substring(0, 15);
    p.putUInt(key.c_str(), g_layer_mask);
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
static void build_themes() {
    if (scr_themes) return;
    scr_themes = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr_themes, lv_color_hex(COL_BG), 0);
    lv_obj_set_style_bg_opa(scr_themes, LV_OPA_COVER, 0);
    lv_obj_t *t = lv_label_create(scr_themes);
    lv_label_set_text(t, "THEMES");
    lv_obj_set_style_text_color(t, lv_color_hex(COL_MUTE),0);
    lv_obj_set_style_text_font(t, &lv_font_montserrat_14, 0);
    lv_obj_align(t, LV_ALIGN_TOP_MID, 0, 24);
    // Fixed border colors per theme index (LVGL RGB888)
    static const uint32_t THEME_BORDER_RGB[4] = { 0x00E5FF, 0x001FCC, 0xBD8000, 0x67FFCC };
    for (int i=0; i<4; i++) {
        lv_obj_t *b = lv_btn_create(scr_themes);
        lv_obj_set_size(b, 260, 48);
        lv_obj_align(b, LV_ALIGN_TOP_MID, 0, 70 + i*58);
        lv_obj_set_style_bg_color(b, lv_color_hex(0x141414), 0);
        lv_obj_set_style_border_color(b, lv_color_hex(THEME_BORDER_RGB[i]), 0);
        lv_obj_set_style_border_width(b, 1, 0);
        lv_obj_set_style_radius(b, 12, 0);
        lv_obj_t *l = lv_label_create(b);
        lv_label_set_text(l, THEMES[i].name);
        lv_obj_center(l);
        lv_obj_set_style_text_color(l, lv_color_hex(COL_TXT), 0);
        int *id = (int*)lv_mem_alloc(sizeof(int)); *id = i;
        lv_obj_add_event_cb(b, [](lv_event_t *e){
            int *idp = (int*)lv_event_get_user_data(e);
            set_theme((uint8_t)*idp);
            enter_mode(MODE_MAP);
        }, LV_EVENT_CLICKED, id);
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
    int cached = 0;
    for (int i=0; i<TILE_CACHE_SLOTS; i++) if (g_cache[i].loaded) cached++;
    uint32_t heap = ESP.getFreeHeap();
    uint32_t psram = ESP.getFreePsram();
    int bat = g_has_pmu ? g_pmu.getBatteryPercent() : -1;
    char buf[512];
    snprintf(buf, sizeof(buf),
        "Map:   %s\n"
        "Zoom:  %u (%u zooms)\n"
        "Cam:   %.0f, %.0f  scl %.4f\n"
        "Tiles: %u indexed, %d cached\n"
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
static void build_tools() {
    if (scr_tools) return;
    scr_tools = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr_tools, lv_color_hex(COL_BG), 0);
    lv_obj_set_style_bg_opa(scr_tools, LV_OPA_COVER, 0);

    lv_obj_t *t = lv_label_create(scr_tools);
    lv_label_set_text(t, "TOOLS");
    lv_obj_set_style_text_color(t, lv_color_hex(COL_MUTE),0);
    lv_obj_set_style_text_font(t, &lv_font_montserrat_14, 0);
    lv_obj_align(t, LV_ALIGN_TOP_MID, 0, 20);

    struct Item { const char *name; AppMode target; };
    static const Item items[] = {
        {"Save Location", MODE_SAVEDIALOG},
        {"Layers",        MODE_LAYERS},
        {"Themes",        MODE_THEMES},
        {"Compass",       MODE_COMPASS},
        {"Level",         MODE_LEVEL},
        {"Sun",           MODE_SUN},
        {"Bearings",      MODE_BEARINGS},
        {"Search",        MODE_SEARCH},
        {"Block Nav",     MODE_BLOCKNAV},
        {"Explore",       MODE_EXPLORE},
        {"Developer",     MODE_DEV},
    };
    const int cols = 3;
    for (size_t i=0; i<sizeof(items)/sizeof(items[0]); i++) {
        int col = i % cols, row = i / cols;
        lv_obj_t *b = lv_btn_create(scr_tools);
        lv_obj_set_size(b, 116, 74);
        lv_obj_set_pos(b, 26 + col * 128, 60 + row * 84);
        lv_obj_set_style_bg_color(b, lv_color_hex(0x141414), 0);
        lv_obj_set_style_border_color(b, lv_color_hex(COL_ACCENT), 0);
        lv_obj_set_style_border_width(b, 1, 0);
        lv_obj_set_style_radius(b, 14, 0);
        lv_obj_t *l = lv_label_create(b);
        lv_label_set_text(l, items[i].name);
        lv_obj_set_style_text_color(l, lv_color_hex(COL_TXT), 0);
        lv_obj_center(l);
        AppMode *tgt = (AppMode*)lv_mem_alloc(sizeof(AppMode));
        *tgt = items[i].target;
        lv_obj_add_event_cb(b, [](lv_event_t *e){
            AppMode *tp = (AppMode*)lv_event_get_user_data(e);
            enter_mode(*tp);
        }, LV_EVENT_CLICKED, tgt);
    }

    // Extra row: quick toggles (breadcrumb drop, clear nav target)
    lv_obj_t *bc = lv_btn_create(scr_tools);
    lv_obj_set_size(bc, 176, 46);
    lv_obj_set_pos(bc, 26, 60 + 4 * 84);
    lv_obj_set_style_bg_color(bc, lv_color_hex(0x141414), 0);
    lv_obj_set_style_border_color(bc, lv_color_hex(COL_GOLD), 0);
    lv_obj_set_style_border_width(bc, 1, 0);
    lv_obj_set_style_radius(bc, 12, 0);
    lv_obj_t *bcl = lv_label_create(bc);
    lv_label_set_text(bcl, "Drop Breadcrumb");
    lv_obj_set_style_text_color(bcl, lv_color_hex(COL_GOLD), 0);
    lv_obj_center(bcl);
    lv_obj_add_event_cb(bc, [](lv_event_t *e){
        (void)e;
        breadcrumb_add((int32_t)g_cam_x, (int32_t)g_cam_y);
        enter_mode(MODE_MAP);
    }, LV_EVENT_CLICKED, NULL);

    lv_obj_t *cn = lv_btn_create(scr_tools);
    lv_obj_set_size(cn, 176, 46);
    lv_obj_set_pos(cn, 210, 60 + 4 * 84);
    lv_obj_set_style_bg_color(cn, lv_color_hex(0x141414), 0);
    lv_obj_set_style_border_color(cn, lv_color_hex(COL_ACCENT), 0);
    lv_obj_set_style_border_width(cn, 1, 0);
    lv_obj_set_style_radius(cn, 12, 0);
    lv_obj_t *cnl = lv_label_create(cn);
    lv_label_set_text(cnl, "Clear Nav Target");
    lv_obj_set_style_text_color(cnl, lv_color_hex(COL_ACCENT), 0);
    lv_obj_center(cnl);
    lv_obj_add_event_cb(cn, [](lv_event_t *e){
        (void)e;
        g_has_nav_target = false;
        g_need_map_redraw = true;
        enter_mode(MODE_MAP);
    }, LV_EVENT_CLICKED, NULL);
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
    String key = String("lm_") + g_meta.title;
    if (key.length() > 15) key = key.substring(0, 15);
    g_layer_mask = p.getUInt(key.c_str(), 0xFFFFFFFFu);
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

// Touch
static bool     g_dragging = false;
static int32_t  g_drag_last_x = 0, g_drag_last_y = 0;
static uint32_t g_touch_press_t = 0;
static bool     g_longpress_fired = false;
static bool     g_pinching = false;
static float    g_pinch_start_dist = 0;
static float    g_pinch_start_scale = 1;
static uint32_t g_double_tap_last_t = 0;
static int32_t  g_press_start_x=0, g_press_start_y=0;

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
    if (g_cam_scale < 1e-6f) g_cam_scale = 1e-6f;
    if (g_cam_scale > 100.0f) g_cam_scale = 100.0f;
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

static void handle_map_touch(uint32_t now) {
    // Any active touch = user activity (used by idle-dim to wake the display).
    if (g_tch_pressed) wp_mark_activity(now);

    // Library swipe support (screen coord swipe) + arrow-zone tap
    static bool lib_touch_active = false;
    static int32_t lib_sx=0, lib_sy=0;
    if (g_mode == MODE_LIBRARY) {
        if (g_tch_pressed) {
            if (!lib_touch_active) { lib_touch_active = true; lib_sx = g_tch_x; lib_sy = g_tch_y; }
        } else if (lib_touch_active) {
            lib_touch_active = false;
            int32_t dx = g_tch_x - lib_sx;
            int32_t dy = g_tch_y - lib_sy;
            if (abs(dx) > 60 && abs(dx) > abs(dy)) {
                if (dx < 0) g_map_sel = (g_map_sel + 1) % (g_map_count_lib?g_map_count_lib:1);
                else        g_map_sel = (g_map_sel - 1 + g_map_count_lib) % (g_map_count_lib?g_map_count_lib:1);
                g_lib_redraw = true;
            } else if (abs(dx) < 20 && abs(dy) < 20) {
                // Small tap: check arrow-zones (left third / right third of the card)
                int card_x = 20, card_y = 110, card_w = LCD_WIDTH - 40, card_h = 240;
                bool in_card = (lib_sx >= card_x && lib_sx <= card_x + card_w &&
                                lib_sy >= card_y && lib_sy <= card_y + card_h);
                if (in_card && g_map_count_lib > 1 && lib_sx < card_x + 60) {
                    g_map_sel = (g_map_sel - 1 + g_map_count_lib) % g_map_count_lib;
                    g_lib_redraw = true;
                } else if (in_card && g_map_count_lib > 1 && lib_sx > card_x + card_w - 60) {
                    g_map_sel = (g_map_sel + 1) % g_map_count_lib;
                    g_lib_redraw = true;
                } else {
                    // tap centre opens
                    library_open_selected();
                }
            }
        }
        return;
    }
    if (g_mode != MODE_MAP) return;

    // Two-finger pinch surrogate: driver only exposes 1 point, but reports
    // finger count. Interpret vertical drag while two fingers are down as zoom.
    static bool  two_finger = false;
    static int32_t two_finger_start_y = 0;
    static float  two_finger_start_scale = 1.0f;
    if (g_tch_pressed && g_tch_fingers >= 2) {
        if (!two_finger) {
            two_finger = true;
            two_finger_start_y = g_tch_y;
            two_finger_start_scale = g_cam_scale;
        } else {
            int32_t dy = g_tch_y - two_finger_start_y;
            // moving up (dy < 0) zooms in
            float factor = expf(-dy * 0.006f);
            float ns = two_finger_start_scale * factor;
            if (ns < 1e-6f) ns = 1e-6f;
            if (ns > 100.0f) ns = 100.0f;
            if (fabsf(ns - g_cam_scale) > 1e-4f) {
                g_cam_scale = ns;
                cam_select_zoom();
                g_need_map_redraw = true;
            }
        }
        g_dragging = false;
        g_longpress_fired = true; // block tap-actions after pinch
        return;
    } else {
        two_finger = false;
    }

    if (g_tch_pressed) {
        if (!g_dragging) {
            // First-touch: check HUD button hit first (only on touch-down).
            int hit = hud_hit_test(g_tch_x, g_tch_y);
            if (hit >= 0) {
                // Consume this touch as a HUD button press
                g_dragging = true;
                g_drag_last_x = g_tch_x; g_drag_last_y = g_tch_y;
                g_press_start_x = g_tch_x; g_press_start_y = g_tch_y;
                g_touch_press_t = now;
                g_longpress_fired = true; // don't fire long-press or POI tap
                // Defer the action to release so a user can slide off to cancel
                return;
            }
            g_dragging = true;
            g_drag_last_x = g_tch_x; g_drag_last_y = g_tch_y;
            g_press_start_x = g_tch_x; g_press_start_y = g_tch_y;
            g_touch_press_t = now;
            g_longpress_fired = false;
            g_pinching = false;
        } else {
            int dx = g_tch_x - g_drag_last_x;
            int dy = g_tch_y - g_drag_last_y;
            int moved = abs(g_tch_x - g_press_start_x) + abs(g_tch_y - g_press_start_y);
            if (abs(dx) + abs(dy) > 2) {
                // Only pan if the initial press wasn't on a HUD button
                if (hud_hit_test(g_press_start_x, g_press_start_y) < 0) {
                    map_pan(dx, dy);
                }
                g_drag_last_x = g_tch_x; g_drag_last_y = g_tch_y;
                g_longpress_fired = true; // moved -> not a long press
            } else if (!g_longpress_fired && moved < 12 && (now - g_touch_press_t) > 700) {
                // long press: open Tools menu
                g_longpress_fired = true;
                enter_mode(MODE_TOOLS);
            }
        }
    } else {
        if (g_dragging) {
            uint32_t dur = now - g_touch_press_t;
            int moved = abs(g_tch_x - g_press_start_x) + abs(g_tch_y - g_press_start_y);
            if (dur < 500 && moved < 15) {
                // Release inside a HUD button that was pressed on touch-down?
                int hit_now = hud_hit_test(g_press_start_x, g_press_start_y);
                if (hit_now >= 0) {
                    switch (hit_now) {
                        case HUD_ZOOM_IN:  map_zoom(1.5f); break;
                        case HUD_ZOOM_OUT: map_zoom(1.0f/1.5f); break;
                        case HUD_TOOLS:    enter_mode(MODE_TOOLS); break;
                        case HUD_RECENTRE: map_recentre(); break;
                        case HUD_ROTATE:
                            g_rotate_with_heading = !g_rotate_with_heading;
                            if (!g_rotate_with_heading) g_map_rotation_deg = 0.0f;
                            g_need_map_redraw = true;
                            break;
                    }
                    g_dragging = false; g_pinching = false;
                    return;
                }
            }
            if (!g_longpress_fired && dur < 350 && moved < 15) {
                // Tap: check POI first, else zoom-in on double-tap detection
                bool hit = tap_hit_poi(g_press_start_x, g_press_start_y);
                if (!hit) {
                    if (g_double_tap_last_t && (now - g_double_tap_last_t) < 400) {
                        // double tap -> zoom in
                        map_zoom(1.5f);
                        g_double_tap_last_t = 0;
                    } else {
                        g_double_tap_last_t = now;
                    }
                }
            }
        }
        g_dragging = false;
        g_pinching = false;
    }
    (void)g_pinch_start_scale;
    (void)g_pinch_start_dist;
    (void)g_double_tap_last_t;
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
                "Controls (see WaypointOS.ino header):\n"
                "  BOOT tap        - library: next map / map: zoom in\n"
                "  BOOT 2x tap     - library: prev / map: zoom out\n"
                "  BOOT hold 0.5s  - library: open / map: recentre\n"
                "  BOOT hold 1.5s  - back one level (sub -> map -> lib -> launcher)\n"
                "  BOOT hold 4s    - direct return to LauncherOS from anywhere\n"
                "  Long press map  - Tools menu (Layers, Themes, Compass, ...)\n"
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

    // Idle screen dim: after WP_IDLE_DIM_MS with no touch/button/motion,
    // fade the AMOLED down to ~15% brightness. Wake on any activity.
    wp_idle_dim_tick(now);
    wp_brightness_tick(now);

    delay(4);
}
