// =====================================================================
//  GameOS.ino  --  LauncherOS-compatible Game console module (v2 - 30 games)
//  Target  : Waveshare ESP32-S3 Touch AMOLED 2.06  (410 x 502, FT3168, AXP2101)
//  Build   : ESP32-S3 Dev Module, PSRAM=OPI, Partition=Minimal SPIFFS
//  Output  : GameOS.ino.bin -> /WATCH/bin/gameos.bin
//
//  v2 changes:
//    - Registry-driven launcher (GAMES[]), categoried + momentum-scrolling list
//    - Rebuilt PONG: ball spin from paddle contact, speed-up volleys, trail,
//      smarter AI with reaction lag, screen flash on score, deeper neon palette
//    - 28 NEW games (see games_pack1/2/3.h):
//      TETRIS, BREAKOUT, 2048, FLAPPY, TIC-TAC-TOE, WHACK-A-MOLE,
//      SIMON, DINO, PONG-2P, REACTION,
//      MINESWEEPER, MEMORY, LIGHTS-OUT, CONNECT4, 15-PUZZLE,
//      INVADERS, ASTEROIDS, DOODLE-DODGE, FROGGER, MAZE,
//      TRON, MATCH-3, PINBALL, WORDLE, SUDOKU, CHECKERS,
//      AIR-HOCKEY, PLATFORMER, TOWER-STACK, CROSSY
// =====================================================================

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
#include <Update.h>

// =====================================================================
//   GameEntry struct  (moved to TOP of file so Arduino's auto-prototype
//   generator sees it before it emits prototypes for make_game_card()
//   and the click-handler lambda that reference 'const GameEntry*'.
//   This fixes the 'GameEntry does not name a type' compile error.)
// =====================================================================
struct GameEntry {
    const char* name;
    const char* desc;
    const char  icon;        // initial / glyph
    uint32_t    accent;
    void      (*open)();
    uint8_t     cat;         // 0 = built-in, 1 = easy, 2 = medium, 3 = showpiece
};


// ---- Pin map ---------------------------------------------------------
#define LCD_SDIO0 4
#define LCD_SDIO1 5
#define LCD_SDIO2 6
#define LCD_SDIO3 7
#define LCD_SCLK 11
#define LCD_CS   12
#define LCD_RESET 8
#define LCD_W 410
#define LCD_H 502
#define IIC_SDA 15
#define IIC_SCL 14
#define TP_INT 38
#define TP_RESET 9
static const int SDMMC_CLK=2,SDMMC_CMD=1,SDMMC_DATA=3;
#define BOOT_BTN 0
#define PWR_BTN 10

#define HC_WIFI_SSID  "GameOS-Fallback"
#define HC_WIFI_PASS  "changeme"

// ---- Color palette (shared across all games) -------------------------
#define COL_BG       0x000000
#define COL_PANEL    0x0E0E0E
#define COL_PANEL2   0x1C1C1C
#define COL_CYAN     0x00E5FF
#define COL_MAGENTA  0xFF2EC4
#define COL_GREEN    0x00FF80
#define COL_YELLOW   0xFFD93D
#define COL_ORANGE   0xFFB300
#define COL_RED      0xFF4040
#define COL_PURPLE   0x9B5DE5
#define COL_WHITE    0xFFFFFF
#define COL_TEXT_DIM 0x666666

// ---- Singletons ------------------------------------------------------
Arduino_DataBus *g_bus = new Arduino_ESP32QSPI(LCD_CS,LCD_SCLK,LCD_SDIO0,LCD_SDIO1,LCD_SDIO2,LCD_SDIO3);
Arduino_GFX     *gfx   = new Arduino_CO5300(g_bus,LCD_RESET,0,LCD_W,LCD_H,22,0,0,0);
std::shared_ptr<Arduino_IIC_DriveBus> g_iic = std::make_shared<Arduino_HWIIC>(IIC_SDA,IIC_SCL,&Wire);
void Arduino_IIC_Touch_Interrupt(void);
std::unique_ptr<Arduino_IIC> FT3168(new Arduino_FT3x68(g_iic,FT3168_DEVICE_ADDRESS,DRIVEBUS_DEFAULT_VALUE,TP_INT,Arduino_IIC_Touch_Interrupt));
void Arduino_IIC_Touch_Interrupt(void){FT3168->IIC_Interrupt_Flag=true;}
XPowersPMU g_pmu; bool g_has_pmu=false;
// CRASH FIX: track whether the FT3168 touch IC actually ACKed on I2C.
// If it didn't, reading from FT3168->IIC_Read_Device_Value() dereferences
// a half-initialised driver field and faults with LoadProhibited @
// EXCVADDR 0x22 (the boot-loop crash we were chasing).
static bool g_has_touch=false;
static bool i2c_ping(uint8_t addr){
    Wire.beginTransmission(addr);
    return (Wire.endTransmission() == 0);
}

// ---- LVGL plumbing ---------------------------------------------------
static lv_disp_draw_buf_t g_db; static lv_color_t *g_lvbuf=nullptr;
static lv_disp_drv_t g_dd; static lv_indev_drv_t g_id;
static void lv_flush(lv_disp_drv_t*d,const lv_area_t*a,lv_color_t*p){
    uint32_t w=a->x2-a->x1+1,h=a->y2-a->y1+1;
    gfx->draw16bitRGBBitmap(a->x1,a->y1,(uint16_t*)p,w,h); lv_disp_flush_ready(d);}

// ---- Screen / swipe globals ------------------------------------------
#define SCREEN_FACE             0
#define SCREEN_APPS             1
#define SCREEN_BATTERY          2
#define NUM_SCREENS             3
#define SWIPE_THRESHOLD_PX      25
#define SWIPE_MAX_DURATION_MS   800
#define CLICK_AFTER_SWIPE_MS    350
#define SCREEN_SWITCH_ANIM_MS   140

static lv_obj_t *screens[NUM_SCREENS] = { nullptr, nullptr, nullptr };
static int       current_screen   = SCREEN_FACE;
static int32_t   ts_x=0, ts_y=0, tc_x=0, tc_y=0;
static uint32_t  ts_ms=0;
static bool      touchActive=false;
static uint32_t  lastSwipeMs=0;
static volatile int g_pending_swipe = 0;
static lv_obj_t *g_transient_screen = nullptr;
static bool      g_apps_built = false;

static void lv_touch_gesture(lv_indev_drv_t *, lv_indev_data_t *d);
static void goto_screen(int s);
static void build_screen_face();
static void build_screen_apps();
static void build_screen_battery();
static void update_battery_screen();
static void open_app_screen(lv_obj_t *s);
static void open_launcher_screen();

// ---- Back-to-launcher ------------------------------------------------
static void return_to_launcher(){
    const esp_partition_t *factory = esp_partition_find_first(
        ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_FACTORY, NULL);
    if(factory) esp_ota_set_boot_partition(factory);
    else {
        const esp_partition_t *other = esp_ota_get_next_update_partition(NULL);
        if(other && other != esp_ota_get_running_partition())
            esp_ota_set_boot_partition(other);
    }
    delay(80); esp_restart();
}

// =====================================================================
//   GAME HELPERS  (shared by all 30 games)
// =====================================================================
// Standard game playfield size (fits 410x502 with header + exit btn)
#define GW 370
#define GH 380

// Shared canvas/timer pointer slots that ANY game can use, so we can
// free reliably from a single "exit any game" path.
static lv_obj_t* g_game_canvas  = nullptr;
static lv_color_t* g_game_buf   = nullptr;
static lv_timer_t* g_game_timer = nullptr;
static lv_obj_t* g_game_hdr_lbl = nullptr;

static void game_cleanup(){
    if(g_game_timer){ lv_timer_del(g_game_timer); g_game_timer=nullptr; }
    if(g_game_buf){ free(g_game_buf); g_game_buf=nullptr; }
    g_game_canvas = nullptr;
    g_game_hdr_lbl = nullptr;
}

static lv_obj_t* game_screen_base(const char* title, uint32_t accent){
    lv_obj_t *s = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s, lv_color_hex(COL_BG),0);
    lv_obj_set_style_bg_opa(s, LV_OPA_COVER,0);
    lv_obj_clear_flag(s, LV_OBJ_FLAG_SCROLLABLE);

    // title pill
    lv_obj_t *t = lv_label_create(s);
    lv_label_set_text(t, title);
    lv_obj_set_style_text_color(t, lv_color_hex(accent),0);
    lv_obj_set_style_text_font(t, &lv_font_montserrat_22,0);
    lv_obj_set_style_text_letter_space(t, 3,0);
    lv_obj_align(t, LV_ALIGN_TOP_MID, 0, 22);

    // header label (score area). game can update via g_game_hdr_lbl
    g_game_hdr_lbl = lv_label_create(s);
    lv_label_set_text(g_game_hdr_lbl, "");
    lv_obj_set_style_text_color(g_game_hdr_lbl, lv_color_hex(COL_WHITE),0);
    lv_obj_set_style_text_font(g_game_hdr_lbl, &lv_font_montserrat_18,0);
    lv_obj_align(g_game_hdr_lbl, LV_ALIGN_TOP_MID, 0, 56);
    return s;
}

static lv_obj_t* game_add_canvas(lv_obj_t* s, int w, int h){
    g_game_buf = (lv_color_t*)heap_caps_malloc(w*h*sizeof(lv_color_t),
                    MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
    if(!g_game_buf)
        g_game_buf = (lv_color_t*)heap_caps_malloc(w*h*sizeof(lv_color_t), MALLOC_CAP_8BIT);
    g_game_canvas = lv_canvas_create(s);
    lv_canvas_set_buffer(g_game_canvas, g_game_buf, w, h, LV_IMG_CF_TRUE_COLOR);
    lv_obj_align(g_game_canvas, LV_ALIGN_CENTER, 0, 0);
    lv_canvas_fill_bg(g_game_canvas, lv_color_hex(0x050505), LV_OPA_COVER);
    return g_game_canvas;
}

static void game_add_exit(lv_obj_t* s, uint32_t accent){
    lv_obj_t *b = lv_btn_create(s);
    lv_obj_set_size(b, 180, 42);
    lv_obj_align(b, LV_ALIGN_BOTTOM_MID, 0, -16);
    lv_obj_set_style_bg_color(b, lv_color_hex(0x141414),0);
    lv_obj_set_style_border_color(b, lv_color_hex(accent),0);
    lv_obj_set_style_border_width(b,1,0);
    lv_obj_set_style_radius(b,21,0);
    lv_obj_t *l = lv_label_create(b);
    lv_label_set_text(l, LV_SYMBOL_LEFT "  Exit Game");
    lv_obj_set_style_text_color(l, lv_color_hex(accent),0);
    lv_obj_set_style_text_font(l, &lv_font_montserrat_14,0);
    lv_obj_center(l);
    lv_obj_add_event_cb(b, [](lv_event_t*){
        game_cleanup();
        open_launcher_screen();
    }, LV_EVENT_CLICKED, NULL);
}

static void game_canvas_clear(lv_obj_t* cv, uint32_t bg){
    if(cv) lv_canvas_fill_bg(cv, lv_color_hex(bg), LV_OPA_COVER);
}

static void game_canvas_rect(lv_obj_t* cv, int x, int y, int w, int h, uint32_t col, int radius=0){
    if(!cv) return;
    lv_draw_rect_dsc_t r; lv_draw_rect_dsc_init(&r);
    r.bg_color = lv_color_hex(col); r.bg_opa = LV_OPA_COVER; r.radius = radius;
    lv_canvas_draw_rect(cv, x, y, w, h, &r);
}

static void game_canvas_circle(lv_obj_t* cv, int cx, int cy, int rr, uint32_t col){
    game_canvas_rect(cv, cx-rr, cy-rr, rr*2, rr*2, col, rr);
}

static void game_set_score(const char* fmt, ...){
    if(!g_game_hdr_lbl) return;
    char buf[64]; va_list ap; va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap); va_end(ap);
    lv_label_set_text(g_game_hdr_lbl, buf);
}

// =====================================================================
//   PONG  (REBUILT  -  spin, speed-up, trail, smart AI, flash)
// =====================================================================
struct PongState {
    float bx, by, vx, vy;
    int   py, aiy;
    int   sp, sa;
    float speed;
    int   flash;           // frames to flash bg
    uint32_t flashCol;
    // trail
    float tx[8], ty[8];
    int   ti;
};
static PongState pg;

static void pong_reset_ball(int direction){
    pg.bx = GW/2; pg.by = GH/2;
    pg.speed = 4.0f;
    pg.vx = direction * pg.speed * 0.7f;
    pg.vy = ((random(2)*2)-1) * pg.speed * 0.5f;
    pg.ti = 0;
    for(int i=0;i<8;i++){ pg.tx[i]=pg.bx; pg.ty[i]=pg.by; }
}

static void pong_step(lv_timer_t*){
    // record trail
    pg.tx[pg.ti] = pg.bx; pg.ty[pg.ti] = pg.by;
    pg.ti = (pg.ti+1) & 7;

    pg.bx += pg.vx; pg.by += pg.vy;

    // walls
    if(pg.by < 6){ pg.by=6; pg.vy = fabsf(pg.vy); }
    if(pg.by > GH-18){ pg.by = GH-18; pg.vy = -fabsf(pg.vy); }

    // smarter AI: predict + add reaction lag + small jitter
    static int aiLag = 0;
    aiLag++;
    if(aiLag > 3){
        aiLag = 0;
        int target = (int)pg.by - 40 + (int)(((int)random(20))-10);
        int diff = target - pg.aiy;
        int spd = (int)(pg.speed * 0.8f);
        if(spd < 2) spd = 2;
        if(diff >  spd) pg.aiy += spd;
        else if(diff < -spd) pg.aiy -= spd;
        else pg.aiy = target;
        if(pg.aiy < 0) pg.aiy = 0;
        if(pg.aiy > GH-90) pg.aiy = GH-90;
    }

    // paddle collisions w/ spin (contact point bends vy, increases speed)
    if(pg.bx < 26 && pg.bx > 10 && pg.by > pg.py-2 && pg.by < pg.py+92){
        float rel = ((pg.by + 6) - (pg.py + 45)) / 45.0f; // -1..1
        pg.vx = fabsf(pg.vx);
        pg.vy = rel * pg.speed * 0.9f;
        pg.speed = fminf(pg.speed + 0.35f, 9.0f);
        float mag = sqrtf(pg.vx*pg.vx + pg.vy*pg.vy);
        pg.vx = pg.vx/mag * pg.speed;
        pg.vy = pg.vy/mag * pg.speed;
        pg.flash = 6; pg.flashCol = COL_CYAN;
    }
    if(pg.bx > GW-30 && pg.bx < GW-12 && pg.by > pg.aiy-2 && pg.by < pg.aiy+92){
        float rel = ((pg.by + 6) - (pg.aiy + 45)) / 45.0f;
        pg.vx = -fabsf(pg.vx);
        pg.vy = rel * pg.speed * 0.9f;
        pg.speed = fminf(pg.speed + 0.35f, 9.0f);
        float mag = sqrtf(pg.vx*pg.vx + pg.vy*pg.vy);
        pg.vx = pg.vx/mag * pg.speed;
        pg.vy = pg.vy/mag * pg.speed;
        pg.flash = 6; pg.flashCol = COL_MAGENTA;
    }

    // scoring
    if(pg.bx < -8){ pg.sa++; pg.flash=12; pg.flashCol=COL_MAGENTA; pong_reset_ball(+1); }
    if(pg.bx > GW+8){ pg.sp++; pg.flash=12; pg.flashCol=COL_CYAN;    pong_reset_ball(-1); }

    // render
    uint32_t bg = 0x050505;
    if(pg.flash > 0){
        int a = pg.flash * 3;
        bg = ((pg.flashCol>>16)&0xFF) * a/100;
        bg = 0x050505; // keep bg dark; we instead draw flash overlay below
        pg.flash--;
    }
    lv_canvas_fill_bg(g_game_canvas, lv_color_hex(0x050505), LV_OPA_COVER);

    // center dotted line
    for(int y=8; y<GH-8; y+=18){
        game_canvas_rect(g_game_canvas, GW/2-1, y, 2, 8, 0x1A1A1A, 1);
    }

    // ball trail (fades from old to new)
    for(int k=0;k<7;k++){
        int idx = (pg.ti + k) & 7;
        uint8_t alpha_pct = 8 + k*10;
        uint32_t c = (alpha_pct < 35) ? 0x222222 :
                     (alpha_pct < 55) ? 0x555555 :
                     (alpha_pct < 75) ? 0xAAAAAA : COL_WHITE;
        game_canvas_rect(g_game_canvas, (int)pg.tx[idx]-3, (int)pg.ty[idx]-3, 6,6, c, 3);
    }

    // paddles
    game_canvas_rect(g_game_canvas, 10, pg.py,    10, 90, COL_CYAN, 4);
    game_canvas_rect(g_game_canvas, GW-22, pg.aiy, 10, 90, COL_MAGENTA, 4);

    // ball
    game_canvas_rect(g_game_canvas, (int)pg.bx-6, (int)pg.by-6, 12, 12, COL_WHITE, 6);

    // score flash overlay
    if(pg.flash > 0){
        // simple bottom-strip glow band
        game_canvas_rect(g_game_canvas, 0, 0, GW, 4, pg.flashCol, 0);
        game_canvas_rect(g_game_canvas, 0, GH-4, GW, 4, pg.flashCol, 0);
    }

    game_set_score("%d   :   %d", pg.sp, pg.sa);
}

static void pong_drag(lv_event_t*){
    lv_indev_t *in = lv_indev_get_act();
    lv_point_t p; lv_indev_get_point(in, &p);
    // canvas is centered, GH=380 so top y = (LCD_H-GH)/2 + (header offset)
    lv_area_t a; lv_obj_get_coords(g_game_canvas, &a);
    int local = p.y - a.y1 - 45;
    if(local < 0) local = 0;
    if(local > GH-90) local = GH-90;
    pg.py = local;
}

static void open_pong(){
    lv_obj_t *s = game_screen_base("PONG", COL_CYAN);
    lv_obj_t *cv = game_add_canvas(s, GW, GH);
    pg.py = GH/2 - 45; pg.aiy = GH/2 - 45;
    pg.sp = 0; pg.sa = 0; pg.flash = 0;
    pong_reset_ball(1);
    lv_obj_add_event_cb(cv, pong_drag, LV_EVENT_PRESSING, NULL);
    g_game_timer = lv_timer_create(pong_step, 28, NULL);
    game_add_exit(s, COL_CYAN);
    open_app_screen(s);
}

// =====================================================================
//   SNAKE  (kept from v1, lightly modernised)
// =====================================================================
#define GRID 20
static int snake_x[80], snake_y[80], snake_len=4, snake_dx=1, snake_dy=0;
static int food_x=10, food_y=10, snake_score=0;

static void snake_step(lv_timer_t*){
    int nx = snake_x[0]+snake_dx, ny = snake_y[0]+snake_dy;
    for(int i=snake_len-1;i>0;i--){snake_x[i]=snake_x[i-1];snake_y[i]=snake_y[i-1];}
    snake_x[0]=nx; snake_y[0]=ny;
    if(nx<0||nx>=18||ny<0||ny>=18){
        snake_len=4; snake_dx=1; snake_dy=0; snake_score=0;
        for(int i=0;i<4;i++){snake_x[i]=4-i; snake_y[i]=9;}
    }
    if(nx==food_x && ny==food_y){
        snake_len = min(snake_len+1, 79);
        food_x = random(18); food_y = random(18); snake_score += 10;
    }
    lv_canvas_fill_bg(g_game_canvas, lv_color_hex(0x050505), LV_OPA_COVER);
    // grid hint
    for(int i=0;i<=18;i+=6) game_canvas_rect(g_game_canvas, i*GRID, 0, 1, 18*GRID, 0x0F0F0F);
    game_canvas_rect(g_game_canvas, food_x*GRID, food_y*GRID, GRID-2, GRID-2, COL_MAGENTA, 3);
    for(int i=0;i<snake_len;i++){
        uint32_t col = (i==0)?COL_CYAN:0x009BB8;
        game_canvas_rect(g_game_canvas, snake_x[i]*GRID, snake_y[i]*GRID, GRID-2, GRID-2, col, 3);
    }
    game_set_score("SCORE  %d", snake_score);
}
static void snake_swipe(lv_event_t*){
    lv_dir_t d = lv_indev_get_gesture_dir(lv_indev_get_act());
    if(d==LV_DIR_LEFT  && snake_dx==0){snake_dx=-1;snake_dy=0;}
    if(d==LV_DIR_RIGHT && snake_dx==0){snake_dx= 1;snake_dy=0;}
    if(d==LV_DIR_TOP   && snake_dy==0){snake_dx=0;snake_dy=-1;}
    if(d==LV_DIR_BOTTOM&& snake_dy==0){snake_dx=0;snake_dy= 1;}
}
static void open_snake(){
    lv_obj_t *s = game_screen_base("SNAKE", COL_GREEN);
    game_add_canvas(s, 18*GRID, 18*GRID);
    snake_len=4; snake_dx=1; snake_dy=0; snake_score=0;
    for(int i=0;i<4;i++){snake_x[i]=4-i; snake_y[i]=9;}
    lv_obj_add_event_cb(s, snake_swipe, LV_EVENT_GESTURE, NULL);
    g_game_timer = lv_timer_create(snake_step, 140, NULL);
    game_add_exit(s, COL_GREEN);
    open_app_screen(s);
}

// =====================================================================
//   30 GAMES  (split into 3 packs for readability)
// =====================================================================
// ==================== INLINED FROM games_pack1.h ====================
// =====================================================================
//  games_pack1.h  -  EASY-PACK (10 games)
//    TETRIS, BREAKOUT, 2048, FLAPPY, TIC-TAC-TOE,
//    WHACK-A-MOLE, SIMON, DINO, PONG-2P, REACTION
//
//  All games use the helpers from GameOS.ino:
//    game_screen_base, game_add_canvas, game_add_exit,
//    game_canvas_clear/rect/circle, game_set_score, game_cleanup,
//    g_game_canvas, g_game_timer, g_game_buf
// =====================================================================


static void open_launcher_screen();
static void open_app_screen(lv_obj_t *s);

// =====================================================================
//   1.  TETRIS  (10x18 grid, swipe L/R/down, tap rotate)
// =====================================================================
#define TT_W 10
#define TT_H 18
#define TT_CELL 22
static uint8_t  tt_grid[TT_H][TT_W];
static int      tt_px, tt_py, tt_rot, tt_kind, tt_score;
static const int8_t TT_PIECES[7][4][4][2] = {
    // I
    {{{0,1},{1,1},{2,1},{3,1}}, {{2,0},{2,1},{2,2},{2,3}}, {{0,2},{1,2},{2,2},{3,2}}, {{1,0},{1,1},{1,2},{1,3}}},
    // O
    {{{1,0},{2,0},{1,1},{2,1}}, {{1,0},{2,0},{1,1},{2,1}}, {{1,0},{2,0},{1,1},{2,1}}, {{1,0},{2,0},{1,1},{2,1}}},
    // T
    {{{1,0},{0,1},{1,1},{2,1}}, {{1,0},{1,1},{2,1},{1,2}}, {{0,1},{1,1},{2,1},{1,2}}, {{1,0},{0,1},{1,1},{1,2}}},
    // S
    {{{1,0},{2,0},{0,1},{1,1}}, {{1,0},{1,1},{2,1},{2,2}}, {{1,1},{2,1},{0,2},{1,2}}, {{0,0},{0,1},{1,1},{1,2}}},
    // Z
    {{{0,0},{1,0},{1,1},{2,1}}, {{2,0},{1,1},{2,1},{1,2}}, {{0,1},{1,1},{1,2},{2,2}}, {{1,0},{0,1},{1,1},{0,2}}},
    // J
    {{{0,0},{0,1},{1,1},{2,1}}, {{1,0},{2,0},{1,1},{1,2}}, {{0,1},{1,1},{2,1},{2,2}}, {{1,0},{1,1},{0,2},{1,2}}},
    // L
    {{{2,0},{0,1},{1,1},{2,1}}, {{1,0},{1,1},{1,2},{2,2}}, {{0,1},{1,1},{2,1},{0,2}}, {{0,0},{1,0},{1,1},{1,2}}},
};
static const uint32_t TT_COL[7] = {COL_CYAN, COL_YELLOW, COL_PURPLE, COL_GREEN, COL_RED, 0x3D8BFF, COL_ORANGE};

static bool tt_fits(int px,int py,int rot,int kind){
    for(int i=0;i<4;i++){
        int x = px + TT_PIECES[kind][rot][i][0];
        int y = py + TT_PIECES[kind][rot][i][1];
        if(x<0||x>=TT_W||y>=TT_H) return false;
        if(y>=0 && tt_grid[y][x]) return false;
    }
    return true;
}
static void tt_lock(){
    for(int i=0;i<4;i++){
        int x = tt_px + TT_PIECES[tt_kind][tt_rot][i][0];
        int y = tt_py + TT_PIECES[tt_kind][tt_rot][i][1];
        if(y>=0&&y<TT_H&&x>=0&&x<TT_W) tt_grid[y][x] = tt_kind+1;
    }
    // clear lines
    for(int y=TT_H-1;y>=0;y--){
        bool full=true;
        for(int x=0;x<TT_W;x++) if(!tt_grid[y][x]){full=false; break;}
        if(full){
            for(int yy=y;yy>0;yy--) for(int x=0;x<TT_W;x++) tt_grid[yy][x]=tt_grid[yy-1][x];
            for(int x=0;x<TT_W;x++) tt_grid[0][x]=0;
            tt_score += 100; y++;
        }
    }
    tt_kind = random(7); tt_rot = 0; tt_px = 3; tt_py = -1;
    if(!tt_fits(tt_px,tt_py,tt_rot,tt_kind)){
        // game over - clear
        memset(tt_grid,0,sizeof(tt_grid)); tt_score=0;
    }
}
static void tt_draw(){
    int OX = (GW - TT_W*TT_CELL)/2, OY = 10;
    lv_canvas_fill_bg(g_game_canvas, lv_color_hex(0x050505), LV_OPA_COVER);
    // grid bg
    game_canvas_rect(g_game_canvas, OX-2, OY-2, TT_W*TT_CELL+4, TT_H*TT_CELL+4, 0x101010, 2);
    for(int y=0;y<TT_H;y++) for(int x=0;x<TT_W;x++){
        if(tt_grid[y][x])
            game_canvas_rect(g_game_canvas, OX+x*TT_CELL+1, OY+y*TT_CELL+1, TT_CELL-2, TT_CELL-2, TT_COL[tt_grid[y][x]-1], 3);
    }
    // current piece
    for(int i=0;i<4;i++){
        int x = tt_px + TT_PIECES[tt_kind][tt_rot][i][0];
        int y = tt_py + TT_PIECES[tt_kind][tt_rot][i][1];
        if(y>=0)
            game_canvas_rect(g_game_canvas, OX+x*TT_CELL+1, OY+y*TT_CELL+1, TT_CELL-2, TT_CELL-2, TT_COL[tt_kind], 3);
    }
    game_set_score("LINES  %d", tt_score/100);
}
static void tt_step(lv_timer_t*){
    if(tt_fits(tt_px, tt_py+1, tt_rot, tt_kind)) tt_py++;
    else tt_lock();
    tt_draw();
}
static void tt_gesture(lv_event_t*){
    lv_dir_t d = lv_indev_get_gesture_dir(lv_indev_get_act());
    if(d==LV_DIR_LEFT  && tt_fits(tt_px-1,tt_py,tt_rot,tt_kind)) tt_px--;
    if(d==LV_DIR_RIGHT && tt_fits(tt_px+1,tt_py,tt_rot,tt_kind)) tt_px++;
    if(d==LV_DIR_BOTTOM){ while(tt_fits(tt_px,tt_py+1,tt_rot,tt_kind)) tt_py++; tt_lock(); }
    tt_draw();
}
static void tt_click(lv_event_t*){
    int nr = (tt_rot+1)&3;
    if(tt_fits(tt_px,tt_py,nr,tt_kind)) tt_rot = nr;
    tt_draw();
}
static void open_tetris(){
    lv_obj_t *s = game_screen_base("TETRIS", COL_CYAN);
    game_add_canvas(s, GW, GH);
    memset(tt_grid,0,sizeof(tt_grid));
    tt_kind=random(7); tt_rot=0; tt_px=3; tt_py=-1; tt_score=0;
    lv_obj_add_event_cb(s, tt_gesture, LV_EVENT_GESTURE, NULL);
    lv_obj_add_event_cb(g_game_canvas, tt_click, LV_EVENT_CLICKED, NULL);
    g_game_timer = lv_timer_create(tt_step, 500, NULL);
    game_add_exit(s, COL_CYAN);
    open_app_screen(s);
}

// =====================================================================
//   2.  BREAKOUT
// =====================================================================
#define BR_ROWS 6
#define BR_COLS 8
static uint8_t br_bricks[BR_ROWS][BR_COLS];
static float br_bx, br_by, br_vx, br_vy;
static int   br_px, br_score, br_lives;
static void br_reset_ball(){ br_bx=GW/2; br_by=GH/2+40; br_vx=3; br_vy=3.2f; }
static void br_step(lv_timer_t*){
    br_bx+=br_vx; br_by+=br_vy;
    if(br_bx<5) br_vx= fabsf(br_vx);
    if(br_bx>GW-13) br_vx=-fabsf(br_vx);
    if(br_by<5) br_vy= fabsf(br_vy);
    int paddle_y = GH - 30;
    if(br_by > paddle_y-10 && br_by < paddle_y && br_bx > br_px && br_bx < br_px+70){
        br_vy = -fabsf(br_vy);
        br_vx += ((br_bx - (br_px+35))/35.0f) * 2.0f;
    }
    if(br_by > GH){
        br_lives--;
        if(br_lives<=0){ br_lives=3; br_score=0; for(int r=0;r<BR_ROWS;r++)for(int c=0;c<BR_COLS;c++) br_bricks[r][c]=1; }
        br_reset_ball();
    }
    int bw = (GW-20)/BR_COLS;
    for(int r=0;r<BR_ROWS;r++) for(int c=0;c<BR_COLS;c++){
        if(!br_bricks[r][c]) continue;
        int bx = 10+c*bw, by = 20+r*16;
        if(br_bx>bx && br_bx<bx+bw-2 && br_by>by && br_by<by+14){
            br_bricks[r][c]=0; br_vy=-br_vy; br_score+=10; break;
        }
    }
    lv_canvas_fill_bg(g_game_canvas, lv_color_hex(0x050505), LV_OPA_COVER);
    static const uint32_t COLS[BR_ROWS]={COL_RED,COL_ORANGE,COL_YELLOW,COL_GREEN,COL_CYAN,COL_MAGENTA};
    for(int r=0;r<BR_ROWS;r++) for(int c=0;c<BR_COLS;c++){
        if(!br_bricks[r][c]) continue;
        game_canvas_rect(g_game_canvas, 10+c*bw+2, 20+r*16, bw-4, 12, COLS[r], 2);
    }
    game_canvas_rect(g_game_canvas, br_px, paddle_y, 70, 8, COL_CYAN, 4);
    game_canvas_rect(g_game_canvas, (int)br_bx-4, (int)br_by-4, 8,8, COL_WHITE, 4);
    game_set_score("SCORE %d  LIVES %d", br_score, br_lives);
}
static void br_drag(lv_event_t*){
    lv_point_t p; lv_indev_get_point(lv_indev_get_act(),&p);
    lv_area_t a; lv_obj_get_coords(g_game_canvas,&a);
    int local = p.x - a.x1 - 35;
    if(local<0) local=0; if(local>GW-70) local=GW-70;
    br_px = local;
}
static void open_breakout(){
    lv_obj_t *s = game_screen_base("BREAKOUT", COL_MAGENTA);
    lv_obj_t *cv = game_add_canvas(s, GW, GH);
    for(int r=0;r<BR_ROWS;r++) for(int c=0;c<BR_COLS;c++) br_bricks[r][c]=1;
    br_px=GW/2-35; br_score=0; br_lives=3; br_reset_ball();
    lv_obj_add_event_cb(cv, br_drag, LV_EVENT_PRESSING, NULL);
    g_game_timer = lv_timer_create(br_step, 24, NULL);
    game_add_exit(s, COL_MAGENTA);
    open_app_screen(s);
}

// =====================================================================
//   3.  2048
// =====================================================================
static int g2[4][4]; static int g2_score=0;
static lv_obj_t* g2_lbls[4][4];
static lv_obj_t* g2_score_lbl;
static uint32_t g2_tile_color(int v){
    switch(v){
        case 2: return 0x1a3a4a; case 4: return 0x1f5466; case 8: return 0x2f7e91;
        case 16: return COL_CYAN; case 32: return 0xFF7A5C; case 64: return COL_ORANGE;
        case 128: return COL_YELLOW; case 256: return COL_GREEN;
        case 512: return COL_MAGENTA; case 1024: return COL_PURPLE; default: return COL_WHITE;
    }
}
static void g2_spawn(){
    int empty[16][2], n=0;
    for(int y=0;y<4;y++) for(int x=0;x<4;x++) if(!g2[y][x]){empty[n][0]=x;empty[n][1]=y;n++;}
    if(!n) return;
    int k=random(n); g2[empty[k][1]][empty[k][0]] = (random(10)<9)?2:4;
}
static void g2_render(){
    for(int y=0;y<4;y++) for(int x=0;x<4;x++){
        int v = g2[y][x];
        lv_obj_set_style_bg_color(lv_obj_get_parent(g2_lbls[y][x]), lv_color_hex(v?g2_tile_color(v):0x1A1A1A),0);
        if(v){ char b[6]; snprintf(b,6,"%d",v); lv_label_set_text(g2_lbls[y][x], b); }
        else lv_label_set_text(g2_lbls[y][x], "");
    }
    char sb[24]; snprintf(sb,24,"SCORE  %d", g2_score);
    lv_label_set_text(g2_score_lbl, sb);
}
static bool g2_shift_left(){
    bool moved=false;
    for(int y=0;y<4;y++){
        int t[4]={0,0,0,0}, k=0;
        for(int x=0;x<4;x++) if(g2[y][x]) t[k++]=g2[y][x];
        for(int i=0;i<3;i++) if(t[i] && t[i]==t[i+1]){ t[i]*=2; g2_score+=t[i]; t[i+1]=0; }
        int nt[4]={0,0,0,0}, kk=0;
        for(int i=0;i<4;i++) if(t[i]) nt[kk++]=t[i];
        for(int x=0;x<4;x++){ if(g2[y][x]!=nt[x]) moved=true; g2[y][x]=nt[x]; }
    }
    return moved;
}
static void g2_rotateCW(){
    int t[4][4];
    for(int y=0;y<4;y++) for(int x=0;x<4;x++) t[x][3-y]=g2[y][x];
    memcpy(g2,t,sizeof(g2));
}
static void g2_gesture(lv_event_t*){
    lv_dir_t d=lv_indev_get_gesture_dir(lv_indev_get_act());
    bool m=false;
    if(d==LV_DIR_LEFT){ m=g2_shift_left(); }
    else if(d==LV_DIR_RIGHT){ g2_rotateCW(); g2_rotateCW(); m=g2_shift_left(); g2_rotateCW(); g2_rotateCW(); }
    else if(d==LV_DIR_TOP){ g2_rotateCW(); g2_rotateCW(); g2_rotateCW(); m=g2_shift_left(); g2_rotateCW(); }
    else if(d==LV_DIR_BOTTOM){ g2_rotateCW(); m=g2_shift_left(); g2_rotateCW(); g2_rotateCW(); g2_rotateCW(); }
    if(m){ g2_spawn(); g2_render(); }
}
static void open_2048(){
    lv_obj_t *s = game_screen_base("2048", COL_YELLOW);
    g2_score_lbl = g_game_hdr_lbl;
    memset(g2,0,sizeof(g2)); g2_score=0; g2_spawn(); g2_spawn();
    lv_obj_t *grid = lv_obj_create(s); lv_obj_remove_style_all(grid);
    lv_obj_set_size(grid, 320, 320); lv_obj_align(grid, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(grid, lv_color_hex(0x0A0A0A),0);
    lv_obj_set_style_bg_opa(grid, LV_OPA_COVER,0);
    lv_obj_set_style_radius(grid,10,0);
    lv_obj_set_style_pad_all(grid,6,0);
    lv_obj_clear_flag(grid, LV_OBJ_FLAG_SCROLLABLE);
    for(int y=0;y<4;y++) for(int x=0;x<4;x++){
        lv_obj_t *c=lv_obj_create(grid); lv_obj_remove_style_all(c);
        lv_obj_set_size(c,72,72); lv_obj_set_pos(c, 8+x*76, 8+y*76);
        lv_obj_set_style_radius(c,8,0);
        lv_obj_set_style_bg_color(c,lv_color_hex(0x1A1A1A),0);
        lv_obj_set_style_bg_opa(c,LV_OPA_COVER,0);
        lv_obj_t *l=lv_label_create(c); g2_lbls[y][x]=l;
        lv_obj_set_style_text_color(l,lv_color_hex(COL_WHITE),0);
        lv_obj_set_style_text_font(l,&lv_font_montserrat_22,0);
        lv_obj_center(l);
    }
    g2_render();
    lv_obj_add_event_cb(s, g2_gesture, LV_EVENT_GESTURE, NULL);
    game_add_exit(s, COL_YELLOW);
    open_app_screen(s);
}

// =====================================================================
//   4.  FLAPPY
// =====================================================================
static float fl_y, fl_v; static int fl_px, fl_score, fl_pipe_x, fl_pipe_gap, fl_dead;
static void fl_step(lv_timer_t*){
    if(!fl_dead){ fl_v += 0.55f; fl_y += fl_v; }
    fl_pipe_x -= 3;
    if(fl_pipe_x < -40){ fl_pipe_x = GW+20; fl_pipe_gap = 60 + random(GH-180); fl_score++; }
    int bird_x = 80; int bird_y = (int)fl_y;
    bool hit = false;
    if(bird_y<0 || bird_y>GH-12) hit=true;
    if(bird_x+12 > fl_pipe_x && bird_x < fl_pipe_x+40){
        if(bird_y < fl_pipe_gap || bird_y+12 > fl_pipe_gap+100) hit=true;
    }
    if(hit){ fl_dead=1; }
    lv_canvas_fill_bg(g_game_canvas, lv_color_hex(0x080816), LV_OPA_COVER);
    // pipe
    game_canvas_rect(g_game_canvas, fl_pipe_x, 0, 40, fl_pipe_gap, COL_GREEN, 4);
    game_canvas_rect(g_game_canvas, fl_pipe_x, fl_pipe_gap+100, 40, GH-fl_pipe_gap-100, COL_GREEN, 4);
    // bird
    game_canvas_rect(g_game_canvas, bird_x, bird_y, 14, 14, fl_dead?COL_RED:COL_YELLOW, 4);
    game_set_score("SCORE  %d%s", fl_score, fl_dead?"  TAP TO RESTART":"");
}
static void fl_tap(lv_event_t*){
    if(fl_dead){ fl_dead=0; fl_y=GH/2; fl_v=0; fl_score=0; fl_pipe_x=GW; return; }
    fl_v = -5.5f;
}
static void open_flappy(){
    lv_obj_t *s = game_screen_base("FLAPPY", COL_CYAN);
    lv_obj_t *cv = game_add_canvas(s, GW, GH);
    fl_y=GH/2; fl_v=0; fl_score=0; fl_pipe_x=GW; fl_pipe_gap=120; fl_dead=0;
    lv_obj_add_event_cb(cv, fl_tap, LV_EVENT_CLICKED, NULL);
    g_game_timer = lv_timer_create(fl_step, 30, NULL);
    game_add_exit(s, COL_CYAN);
    open_app_screen(s);
}

// =====================================================================
//   5.  TIC-TAC-TOE (vs minimax)
// =====================================================================
static int ttt_b[9]; static int ttt_winner=0; static lv_obj_t* ttt_cells[9];
static int ttt_check(int *b){
    static const int W[8][3]={{0,1,2},{3,4,5},{6,7,8},{0,3,6},{1,4,7},{2,5,8},{0,4,8},{2,4,6}};
    for(int i=0;i<8;i++){
        if(b[W[i][0]] && b[W[i][0]]==b[W[i][1]] && b[W[i][1]]==b[W[i][2]]) return b[W[i][0]];
    }
    for(int i=0;i<9;i++) if(!b[i]) return 0;
    return 3; // draw
}
static int ttt_mm(int *b, int turn){
    int w = ttt_check(b);
    if(w==2) return 1; if(w==1) return -1; if(w==3) return 0;
    int best = turn==2?-2:2;
    for(int i=0;i<9;i++) if(!b[i]){
        b[i]=turn; int v=ttt_mm(b, turn==1?2:1); b[i]=0;
        if(turn==2 && v>best) best=v; if(turn==1 && v<best) best=v;
    }
    return best;
}
static void ttt_ai(){
    int best=-2, bi=-1;
    for(int i=0;i<9;i++) if(!ttt_b[i]){
        ttt_b[i]=2; int v=ttt_mm(ttt_b,1); ttt_b[i]=0;
        if(v>best){best=v; bi=i;}
    }
    if(bi>=0) ttt_b[bi]=2;
}
static void ttt_render(){
    for(int i=0;i<9;i++){
        const char *t = ttt_b[i]==1?"X":ttt_b[i]==2?"O":"";
        lv_label_set_text(ttt_cells[i], t);
        uint32_t c = ttt_b[i]==1?COL_CYAN:ttt_b[i]==2?COL_MAGENTA:COL_WHITE;
        lv_obj_set_style_text_color(ttt_cells[i], lv_color_hex(c),0);
    }
    int w = ttt_check(ttt_b);
    if(w==1) game_set_score("YOU WIN");
    else if(w==2) game_set_score("AI WINS");
    else if(w==3) game_set_score("DRAW");
    else game_set_score("YOUR TURN");
}
static void ttt_tap(lv_event_t* e){
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    if(ttt_b[i] || ttt_check(ttt_b)) {
        if(ttt_check(ttt_b)){ memset(ttt_b,0,sizeof(ttt_b)); ttt_render(); }
        return;
    }
    ttt_b[i]=1;
    if(!ttt_check(ttt_b)) ttt_ai();
    ttt_render();
}
static void open_tictactoe(){
    lv_obj_t *s = game_screen_base("TIC-TAC-TOE", COL_MAGENTA);
    memset(ttt_b,0,sizeof(ttt_b));
    lv_obj_t *grid = lv_obj_create(s); lv_obj_remove_style_all(grid);
    lv_obj_set_size(grid, 300, 300); lv_obj_align(grid, LV_ALIGN_CENTER, 0, 0);
    lv_obj_clear_flag(grid, LV_OBJ_FLAG_SCROLLABLE);
    for(int i=0;i<9;i++){
        int r=i/3, c=i%3;
        lv_obj_t *cell=lv_obj_create(grid); lv_obj_remove_style_all(cell);
        lv_obj_set_size(cell, 92,92); lv_obj_set_pos(cell, c*100, r*100);
        lv_obj_set_style_radius(cell, 12,0);
        lv_obj_set_style_bg_color(cell, lv_color_hex(0x1A1A1A),0);
        lv_obj_set_style_bg_opa(cell, LV_OPA_COVER,0);
        lv_obj_set_style_border_color(cell, lv_color_hex(0x2A2A2A),0);
        lv_obj_set_style_border_width(cell, 1, 0);
        lv_obj_add_flag(cell, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(cell, ttt_tap, LV_EVENT_CLICKED, (void*)(intptr_t)i);
        lv_obj_t *l=lv_label_create(cell); ttt_cells[i]=l;
        lv_obj_set_style_text_font(l,&lv_font_montserrat_48,0); lv_obj_center(l);
    }
    ttt_render();
    game_add_exit(s, COL_MAGENTA);
    open_app_screen(s);
}

// =====================================================================
//   6.  WHACK-A-MOLE
// =====================================================================
static int wm_active=-1; static int wm_score=0; static int wm_misses=0; static uint32_t wm_next=0;
static lv_obj_t* wm_cells[9];
static void wm_render(){
    for(int i=0;i<9;i++){
        bool a = (i==wm_active);
        lv_obj_set_style_bg_color(wm_cells[i], lv_color_hex(a?COL_GREEN:0x1A1A1A),0);
        lv_obj_set_style_shadow_color(wm_cells[i], lv_color_hex(COL_GREEN),0);
        lv_obj_set_style_shadow_opa(wm_cells[i], a?180:0,0);
        lv_obj_set_style_shadow_width(wm_cells[i], a?20:0,0);
    }
    game_set_score("HITS %d  MISS %d", wm_score, wm_misses);
}
static void wm_step(lv_timer_t*){
    uint32_t now = millis();
    if(now > wm_next){
        if(wm_active>=0) wm_misses++;
        wm_active = random(9);
        wm_next = now + 900 - min(wm_score*15, 500);
    }
    wm_render();
}
static void wm_tap(lv_event_t* e){
    int i=(int)(intptr_t)lv_event_get_user_data(e);
    if(i==wm_active){ wm_score++; wm_active=-1; wm_next = millis()+400; wm_render(); }
}
static void open_whack(){
    lv_obj_t *s = game_screen_base("WHACK-A-MOLE", COL_GREEN);
    lv_obj_t *grid = lv_obj_create(s); lv_obj_remove_style_all(grid);
    lv_obj_set_size(grid, 320,320); lv_obj_align(grid, LV_ALIGN_CENTER, 0, 0);
    lv_obj_clear_flag(grid, LV_OBJ_FLAG_SCROLLABLE);
    for(int i=0;i<9;i++){
        int r=i/3, c=i%3;
        lv_obj_t *cell=lv_obj_create(grid); lv_obj_remove_style_all(cell);
        lv_obj_set_size(cell, 96,96); lv_obj_set_pos(cell, c*108, r*108);
        lv_obj_set_style_radius(cell, 48,0);
        lv_obj_set_style_bg_color(cell, lv_color_hex(0x1A1A1A),0);
        lv_obj_set_style_bg_opa(cell, LV_OPA_COVER,0);
        lv_obj_add_flag(cell, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(cell, wm_tap, LV_EVENT_CLICKED, (void*)(intptr_t)i);
        wm_cells[i]=cell;
    }
    wm_active=-1; wm_score=0; wm_misses=0; wm_next=millis()+500;
    g_game_timer = lv_timer_create(wm_step, 80, NULL);
    game_add_exit(s, COL_GREEN);
    open_app_screen(s);
}

// =====================================================================
//   7.  SIMON SAYS
// =====================================================================
static int sm_seq[64]; static int sm_len=1, sm_step_idx=0, sm_show_idx=0;
static int sm_active=-1; static uint32_t sm_flash_until=0; static int sm_mode=0; // 0=show, 1=input
static lv_obj_t* sm_btns[4];
static const uint32_t SM_COLS[4] = {COL_CYAN, COL_MAGENTA, COL_GREEN, COL_YELLOW};
static void sm_render(){
    for(int i=0;i<4;i++){
        bool on = (i==sm_active && millis()<sm_flash_until);
        lv_obj_set_style_bg_color(sm_btns[i], lv_color_hex(on?SM_COLS[i]:0x222222),0);
        lv_obj_set_style_shadow_color(sm_btns[i], lv_color_hex(SM_COLS[i]),0);
        lv_obj_set_style_shadow_opa(sm_btns[i], on?200:0,0);
        lv_obj_set_style_shadow_width(sm_btns[i], on?30:0,0);
    }
    game_set_score("LEN %d %s", sm_len, sm_mode==0?"WATCH":"YOUR TURN");
}
static void sm_tick(lv_timer_t*){
    if(sm_mode==0){
        if(millis() >= sm_flash_until){
            if(sm_active>=0){ sm_active=-1; sm_flash_until = millis()+250; sm_render(); return; }
            if(sm_show_idx < sm_len){
                sm_active = sm_seq[sm_show_idx++];
                sm_flash_until = millis() + 450;
            } else {
                sm_mode = 1; sm_step_idx = 0;
            }
        }
    }
    sm_render();
}
static void sm_tap(lv_event_t* e){
    if(sm_mode!=1) return;
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    sm_active = i; sm_flash_until = millis()+220;
    if(sm_seq[sm_step_idx++] != i){
        sm_len = 1; sm_seq[0]=random(4); sm_mode=0; sm_show_idx=0;
        return;
    }
    if(sm_step_idx >= sm_len){
        if(sm_len<64) sm_seq[sm_len++] = random(4);
        sm_mode=0; sm_show_idx=0;
    }
}
static void open_simon(){
    lv_obj_t *s = game_screen_base("SIMON", COL_PURPLE);
    int OX = (LCD_W-260)/2, OY = 130;
    static const int POS[4][2] = {{0,0},{140,0},{0,140},{140,140}};
    for(int i=0;i<4;i++){
        lv_obj_t *b = lv_obj_create(s); lv_obj_remove_style_all(b);
        lv_obj_set_size(b, 120,120); lv_obj_set_pos(b, OX+POS[i][0], OY+POS[i][1]);
        lv_obj_set_style_radius(b, 60,0);
        lv_obj_set_style_bg_color(b, lv_color_hex(0x222222),0);
        lv_obj_set_style_bg_opa(b, LV_OPA_COVER,0);
        lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(b, sm_tap, LV_EVENT_CLICKED, (void*)(intptr_t)i);
        sm_btns[i] = b;
    }
    sm_len=1; sm_seq[0]=random(4); sm_mode=0; sm_show_idx=0; sm_active=-1;
    g_game_timer = lv_timer_create(sm_tick, 40, NULL);
    game_add_exit(s, COL_PURPLE);
    open_app_screen(s);
}

// =====================================================================
//   8.  DINO RUNNER
// =====================================================================
static float dn_y, dn_v; static int dn_grounded=1, dn_obs_x, dn_obs_h, dn_score=0, dn_dead=0;
static void dn_step(lv_timer_t*){
    int ground = GH-40;
    if(!dn_dead){
        if(!dn_grounded){ dn_v += 0.6f; dn_y += dn_v; }
        if(dn_y >= ground){ dn_y = ground; dn_v = 0; dn_grounded = 1; }
        dn_obs_x -= 6 + min(dn_score/200, 6);
        if(dn_obs_x < -30){ dn_obs_x = GW+random(120); dn_obs_h = 18+random(24); }
        if(dn_obs_x>50 && dn_obs_x<90 && dn_y > ground-dn_obs_h) dn_dead=1;
        if(!dn_dead) dn_score++;
    }
    lv_canvas_fill_bg(g_game_canvas, lv_color_hex(0x050505), LV_OPA_COVER);
    game_canvas_rect(g_game_canvas, 0, ground+20, GW, 2, COL_TEXT_DIM, 0);
    // parallax dots
    for(int i=0;i<20;i++){
        int x = (i*47 - (millis()/20)) % GW; if(x<0) x+=GW;
        game_canvas_rect(g_game_canvas, x, 30+(i*13)%80, 2,2, 0x222222,0);
    }
    game_canvas_rect(g_game_canvas, 60, (int)dn_y, 22, 22, dn_dead?COL_RED:COL_CYAN, 4);
    game_canvas_rect(g_game_canvas, dn_obs_x, ground-dn_obs_h+22, 14, dn_obs_h, COL_MAGENTA, 2);
    game_set_score("DIST %d%s", dn_score, dn_dead?"  TAP TO RESTART":"");
}
static void dn_tap(lv_event_t*){
    if(dn_dead){ dn_dead=0; dn_score=0; dn_obs_x=GW; return; }
    if(dn_grounded){ dn_v = -10.5f; dn_grounded = 0; }
}
static void open_dino(){
    lv_obj_t *s = game_screen_base("DINO", COL_CYAN);
    lv_obj_t *cv = game_add_canvas(s, GW, GH);
    dn_y = GH-40; dn_v=0; dn_grounded=1; dn_obs_x=GW; dn_obs_h=20; dn_score=0; dn_dead=0;
    lv_obj_add_event_cb(cv, dn_tap, LV_EVENT_CLICKED, NULL);
    g_game_timer = lv_timer_create(dn_step, 32, NULL);
    game_add_exit(s, COL_CYAN);
    open_app_screen(s);
}

// =====================================================================
//   9.  PONG-2P  (top & bottom paddles, drag halves)
// =====================================================================
static float p2_bx, p2_by, p2_vx, p2_vy;
static int p2_top_x, p2_bot_x, p2_s_top=0, p2_s_bot=0;
static void p2_step(lv_timer_t*){
    p2_bx += p2_vx; p2_by += p2_vy;
    if(p2_bx<6){ p2_bx=6; p2_vx=fabsf(p2_vx); }
    if(p2_bx>GW-18){ p2_bx=GW-18; p2_vx=-fabsf(p2_vx); }
    if(p2_by < 20 && p2_bx>p2_top_x && p2_bx<p2_top_x+90){ p2_vy=fabsf(p2_vy); }
    if(p2_by > GH-30 && p2_bx>p2_bot_x && p2_bx<p2_bot_x+90){ p2_vy=-fabsf(p2_vy); }
    if(p2_by<0){ p2_s_bot++; p2_bx=GW/2; p2_by=GH/2; p2_vy=3.5f; }
    if(p2_by>GH){ p2_s_top++; p2_bx=GW/2; p2_by=GH/2; p2_vy=-3.5f; }
    lv_canvas_fill_bg(g_game_canvas, lv_color_hex(0x050505), LV_OPA_COVER);
    for(int x=8;x<GW-8;x+=18) game_canvas_rect(g_game_canvas, x, GH/2-1, 10, 2, 0x222222,1);
    game_canvas_rect(g_game_canvas, p2_top_x, 10, 90, 10, COL_MAGENTA, 5);
    game_canvas_rect(g_game_canvas, p2_bot_x, GH-20, 90, 10, COL_CYAN, 5);
    game_canvas_rect(g_game_canvas, (int)p2_bx-6, (int)p2_by-6, 12,12, COL_WHITE, 6);
    game_set_score("TOP %d  ::  BOT %d", p2_s_top, p2_s_bot);
}
static void p2_drag(lv_event_t*){
    lv_point_t p; lv_indev_get_point(lv_indev_get_act(),&p);
    lv_area_t a; lv_obj_get_coords(g_game_canvas,&a);
    int lx = p.x - a.x1 - 45, ly = p.y - a.y1;
    if(lx<0) lx=0; if(lx>GW-90) lx=GW-90;
    if(ly < GH/2) p2_top_x=lx; else p2_bot_x=lx;
}
static void open_pong2p(){
    lv_obj_t *s = game_screen_base("PONG 2P", COL_MAGENTA);
    lv_obj_t *cv = game_add_canvas(s, GW, GH);
    p2_top_x = GW/2-45; p2_bot_x=GW/2-45; p2_s_top=0; p2_s_bot=0;
    p2_bx=GW/2; p2_by=GH/2; p2_vx=2.5f; p2_vy=3.5f;
    lv_obj_add_event_cb(cv, p2_drag, LV_EVENT_PRESSING, NULL);
    g_game_timer = lv_timer_create(p2_step, 28, NULL);
    game_add_exit(s, COL_MAGENTA);
    open_app_screen(s);
}

// =====================================================================
//   10.  REACTION TESTER
// =====================================================================
static int rx_state=0; // 0 idle, 1 waiting, 2 ready, 3 result
static uint32_t rx_wait_until=0, rx_shown=0; static int rx_best=99999, rx_last=0;
static lv_obj_t* rx_zone; static lv_obj_t* rx_msg;
static void rx_render(){
    uint32_t c = rx_state==2?COL_CYAN: rx_state==1?0x222222: 0x111111;
    lv_obj_set_style_bg_color(rx_zone, lv_color_hex(c),0);
    const char* m =
        rx_state==0?"TAP TO START":
        rx_state==1?"WAIT FOR CYAN...":
        rx_state==2?"TAP NOW!":
        "TAP TO RETRY";
    lv_label_set_text(rx_msg, m);
    char b[40];
    if(rx_state==3) snprintf(b,40,"%d ms   BEST %d", rx_last, rx_best);
    else snprintf(b,40,"BEST %d ms", rx_best==99999?0:rx_best);
    game_set_score("%s", b);
}
static void rx_tick(lv_timer_t*){
    if(rx_state==1 && millis()>=rx_wait_until){ rx_state=2; rx_shown=millis(); rx_render(); }
}
static void rx_tap(lv_event_t*){
    if(rx_state==0 || rx_state==3){
        rx_state=1; rx_wait_until = millis() + 1200 + random(2500);
    } else if(rx_state==1){
        rx_state=3; rx_last=-1; // too soon
    } else if(rx_state==2){
        rx_last = (int)(millis() - rx_shown);
        if(rx_last < rx_best) rx_best = rx_last;
        rx_state=3;
    }
    rx_render();
}
static void open_reaction(){
    lv_obj_t *s = game_screen_base("REACTION", COL_CYAN);
    rx_zone = lv_obj_create(s); lv_obj_remove_style_all(rx_zone);
    lv_obj_set_size(rx_zone, 340, 300); lv_obj_align(rx_zone, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_radius(rx_zone, 18,0);
    lv_obj_set_style_bg_color(rx_zone, lv_color_hex(0x111111),0);
    lv_obj_set_style_bg_opa(rx_zone, LV_OPA_COVER,0);
    lv_obj_add_flag(rx_zone, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(rx_zone, rx_tap, LV_EVENT_CLICKED, NULL);
    rx_msg = lv_label_create(rx_zone);
    lv_obj_set_style_text_color(rx_msg, lv_color_hex(COL_WHITE),0);
    lv_obj_set_style_text_font(rx_msg, &lv_font_montserrat_22,0);
    lv_obj_center(rx_msg);
    rx_state=0; rx_render();
    g_game_timer = lv_timer_create(rx_tick, 16, NULL);
    game_add_exit(s, COL_CYAN);
    open_app_screen(s);
}

// ==================== END games_pack1.h ====================

// ==================== INLINED FROM games_pack2.h ====================
// =====================================================================
//  games_pack2.h  -  MEDIUM-PACK (10 games)
//    MINESWEEPER, MEMORY, LIGHTS-OUT, CONNECT4, 15-PUZZLE,
//    INVADERS, ASTEROIDS, DOODLE-DODGE, FROGGER, MAZE
// =====================================================================


// ---- 11. MINESWEEPER (9x9) ------------------------------------------
#define MS_N 9
static int8_t ms_grid[MS_N][MS_N]; // -1 mine
static uint8_t ms_open[MS_N][MS_N], ms_flag[MS_N][MS_N];
static lv_obj_t* ms_cells[MS_N][MS_N];
static uint32_t ms_press_at=0; static int ms_press_i=-1, ms_press_j=-1;
static int ms_dead=0;
static void ms_count(){
    for(int y=0;y<MS_N;y++) for(int x=0;x<MS_N;x++){
        if(ms_grid[y][x]==-1) continue;
        int n=0;
        for(int dy=-1;dy<=1;dy++) for(int dx=-1;dx<=1;dx++){
            int nx=x+dx, ny=y+dy;
            if(nx>=0&&nx<MS_N&&ny>=0&&ny<MS_N && ms_grid[ny][nx]==-1) n++;
        }
        ms_grid[y][x]=n;
    }
}
static void ms_render(){
    for(int y=0;y<MS_N;y++) for(int x=0;x<MS_N;x++){
        lv_obj_t *c = ms_cells[y][x];
        uint32_t bg; const char* txt=""; uint32_t fg=COL_WHITE;
        if(ms_flag[y][x]){ bg=COL_ORANGE; txt="F"; fg=COL_BG; }
        else if(!ms_open[y][x]){ bg=0x222222; }
        else if(ms_grid[y][x]==-1){ bg=COL_RED; txt="*"; }
        else if(ms_grid[y][x]==0){ bg=0x0A0A0A; }
        else { bg=0x111111; static char b[12][2]; int v=ms_grid[y][x]; b[v][0]='0'+v; b[v][1]=0; txt=b[v];
               fg = v==1?COL_CYAN: v==2?COL_GREEN: v==3?COL_YELLOW: COL_RED; }
        lv_obj_set_style_bg_color(c, lv_color_hex(bg),0);
        lv_obj_t *l = (lv_obj_t*)lv_obj_get_child(c,0);
        if(l){ lv_label_set_text(l,txt); lv_obj_set_style_text_color(l,lv_color_hex(fg),0); }
    }
    game_set_score(ms_dead?"BOOM - tap to reset":"tap reveal · hold flag");
}
static void ms_flood(int x, int y){
    if(x<0||x>=MS_N||y<0||y>=MS_N||ms_open[y][x]||ms_flag[y][x]) return;
    ms_open[y][x]=1; if(ms_grid[y][x]!=0) return;
    for(int dy=-1;dy<=1;dy++) for(int dx=-1;dx<=1;dx++) if(dx||dy) ms_flood(x+dx,y+dy);
}
static void ms_new(){
    memset(ms_grid,0,sizeof(ms_grid));
    memset(ms_open,0,sizeof(ms_open));
    memset(ms_flag,0,sizeof(ms_flag)); ms_dead=0;
    int placed=0;
    while(placed<10){ int x=random(MS_N), y=random(MS_N); if(ms_grid[y][x]!=-1){ ms_grid[y][x]=-1; placed++; } }
    ms_count();
}
static void ms_press(lv_event_t *e){
    int idx=(int)(intptr_t)lv_event_get_user_data(e);
    ms_press_i=idx/MS_N; ms_press_j=idx%MS_N; ms_press_at=millis();
}
static void ms_release(lv_event_t *e){
    if(ms_press_i<0) return;
    int y=ms_press_i, x=ms_press_j; ms_press_i=-1;
    if(ms_dead){ ms_new(); ms_render(); return; }
    uint32_t held = millis() - ms_press_at;
    if(held > 450){ ms_flag[y][x] = !ms_flag[y][x]; }
    else { if(!ms_flag[y][x]){ if(ms_grid[y][x]==-1){ ms_dead=1; for(int yy=0;yy<MS_N;yy++)for(int xx=0;xx<MS_N;xx++) ms_open[yy][xx]=1; }
                                else ms_flood(x,y); } }
    ms_render();
}
static void open_minesweeper(){
    lv_obj_t *s = game_screen_base("MINESWEEPER", COL_RED);
    lv_obj_t *grid = lv_obj_create(s); lv_obj_remove_style_all(grid);
    lv_obj_set_size(grid, 360,360); lv_obj_align(grid, LV_ALIGN_CENTER, 0, 0);
    lv_obj_clear_flag(grid, LV_OBJ_FLAG_SCROLLABLE);
    int cs = 36;
    for(int y=0;y<MS_N;y++) for(int x=0;x<MS_N;x++){
        lv_obj_t *c=lv_obj_create(grid); lv_obj_remove_style_all(c);
        lv_obj_set_size(c, cs-2, cs-2); lv_obj_set_pos(c, x*cs, y*cs);
        lv_obj_set_style_radius(c,4,0);
        lv_obj_set_style_bg_color(c,lv_color_hex(0x222222),0);
        lv_obj_set_style_bg_opa(c,LV_OPA_COVER,0);
        lv_obj_add_flag(c, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(c, ms_press, LV_EVENT_PRESSED, (void*)(intptr_t)(y*MS_N+x));
        lv_obj_add_event_cb(c, ms_release, LV_EVENT_RELEASED, (void*)(intptr_t)(y*MS_N+x));
        lv_obj_t *l = lv_label_create(c); lv_obj_center(l);
        lv_obj_set_style_text_font(l,&lv_font_montserrat_14,0);
        ms_cells[y][x] = c;
    }
    ms_new(); ms_render();
    game_add_exit(s, COL_RED);
    open_app_screen(s);
}

// ---- 12. MEMORY MATCH (4x4 = 8 pairs) -------------------------------
static int mm_card[16], mm_flip[16], mm_done[16];
static int mm_first=-1, mm_pairs=0;
static lv_obj_t* mm_cells[16];
static const uint32_t MM_PAL[8] = {COL_CYAN,COL_MAGENTA,COL_GREEN,COL_YELLOW,COL_ORANGE,COL_PURPLE,COL_RED,0x3D8BFF};
static void mm_render(){
    for(int i=0;i<16;i++){
        uint32_t c = (mm_flip[i]||mm_done[i]) ? MM_PAL[mm_card[i]] : 0x222222;
        lv_obj_set_style_bg_color(mm_cells[i], lv_color_hex(c),0);
    }
    game_set_score("PAIRS  %d / 8", mm_pairs);
}
static void mm_resolve(lv_timer_t* t){
    int a=-1,b=-1;
    for(int i=0;i<16;i++) if(mm_flip[i]&&!mm_done[i]){ if(a<0)a=i; else b=i; }
    if(a>=0&&b>=0){
        if(mm_card[a]==mm_card[b]){ mm_done[a]=mm_done[b]=1; mm_pairs++; }
        mm_flip[a]=mm_flip[b]=0;
    }
    mm_first=-1; mm_render();
    lv_timer_del(t);
}
static void mm_tap(lv_event_t* e){
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    if(mm_done[i]||mm_flip[i]) return;
    if(mm_pairs==8){ // reset
        for(int k=0;k<16;k++){mm_flip[k]=0; mm_done[k]=0;} mm_pairs=0;
        int v[16]; for(int k=0;k<8;k++){v[k*2]=k; v[k*2+1]=k;}
        for(int k=15;k>0;k--){ int j=random(k+1); int t=v[k];v[k]=v[j];v[j]=t; }
        memcpy(mm_card,v,sizeof(v)); mm_render(); return;
    }
    mm_flip[i]=1;
    if(mm_first<0){ mm_first=i; mm_render(); }
    else { mm_render(); lv_timer_t *t = lv_timer_create(mm_resolve, 700, NULL); lv_timer_set_repeat_count(t,1); }
}
static void open_memory(){
    lv_obj_t *s = game_screen_base("MEMORY", COL_MAGENTA);
    int v[16]; for(int k=0;k<8;k++){v[k*2]=k; v[k*2+1]=k;}
    for(int k=15;k>0;k--){ int j=random(k+1); int t=v[k];v[k]=v[j];v[j]=t; }
    memcpy(mm_card,v,sizeof(v));
    memset(mm_flip,0,sizeof(mm_flip)); memset(mm_done,0,sizeof(mm_done));
    mm_pairs=0; mm_first=-1;
    lv_obj_t *grid = lv_obj_create(s); lv_obj_remove_style_all(grid);
    lv_obj_set_size(grid, 320,320); lv_obj_align(grid, LV_ALIGN_CENTER, 0, 0);
    lv_obj_clear_flag(grid, LV_OBJ_FLAG_SCROLLABLE);
    for(int i=0;i<16;i++){
        int r=i/4, c=i%4;
        lv_obj_t *cc=lv_obj_create(grid); lv_obj_remove_style_all(cc);
        lv_obj_set_size(cc, 74,74); lv_obj_set_pos(cc, c*80, r*80);
        lv_obj_set_style_radius(cc,10,0);
        lv_obj_set_style_bg_color(cc,lv_color_hex(0x222222),0);
        lv_obj_set_style_bg_opa(cc,LV_OPA_COVER,0);
        lv_obj_add_flag(cc, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(cc, mm_tap, LV_EVENT_CLICKED, (void*)(intptr_t)i);
        mm_cells[i]=cc;
    }
    mm_render();
    game_add_exit(s, COL_MAGENTA);
    open_app_screen(s);
}

// ---- 13. LIGHTS OUT (5x5) -------------------------------------------
static uint8_t lo_g[5][5]; static lv_obj_t* lo_cells[5][5];
static void lo_render(){
    int rem=0;
    for(int y=0;y<5;y++) for(int x=0;x<5;x++){
        bool on=lo_g[y][x]; if(on)rem++;
        lv_obj_set_style_bg_color(lo_cells[y][x], lv_color_hex(on?COL_YELLOW:0x1A1A1A),0);
        lv_obj_set_style_shadow_opa(lo_cells[y][x], on?160:0,0);
        lv_obj_set_style_shadow_width(lo_cells[y][x], on?20:0,0);
        lv_obj_set_style_shadow_color(lo_cells[y][x], lv_color_hex(COL_YELLOW),0);
    }
    game_set_score("LIT  %d / 25", rem);
}
static void lo_tap(lv_event_t* e){
    int i=(int)(intptr_t)lv_event_get_user_data(e);
    int y=i/5,x=i%5;
    static const int DX[5]={0,0,0,-1,1}, DY[5]={0,-1,1,0,0};
    for(int k=0;k<5;k++){int nx=x+DX[k],ny=y+DY[k]; if(nx>=0&&nx<5&&ny>=0&&ny<5) lo_g[ny][nx]^=1;}
    lo_render();
}
static void open_lightsout(){
    lv_obj_t *s = game_screen_base("LIGHTS OUT", COL_YELLOW);
    // random init
    memset(lo_g,0,sizeof(lo_g));
    for(int k=0;k<8;k++){ int y=random(5),x=random(5);
        static const int DX[5]={0,0,0,-1,1}, DY[5]={0,-1,1,0,0};
        for(int q=0;q<5;q++){int nx=x+DX[q],ny=y+DY[q]; if(nx>=0&&nx<5&&ny>=0&&ny<5) lo_g[ny][nx]^=1;}
    }
    lv_obj_t *grid=lv_obj_create(s); lv_obj_remove_style_all(grid);
    lv_obj_set_size(grid,320,320); lv_obj_align(grid,LV_ALIGN_CENTER,0,0);
    lv_obj_clear_flag(grid, LV_OBJ_FLAG_SCROLLABLE);
    for(int i=0;i<25;i++){
        int r=i/5, c=i%5;
        lv_obj_t *cc=lv_obj_create(grid); lv_obj_remove_style_all(cc);
        lv_obj_set_size(cc,58,58); lv_obj_set_pos(cc,c*64,r*64);
        lv_obj_set_style_radius(cc,10,0);
        lv_obj_set_style_bg_color(cc,lv_color_hex(0x1A1A1A),0);
        lv_obj_set_style_bg_opa(cc,LV_OPA_COVER,0);
        lv_obj_add_flag(cc, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(cc, lo_tap, LV_EVENT_CLICKED, (void*)(intptr_t)i);
        lo_cells[r][c]=cc;
    }
    lo_render();
    game_add_exit(s, COL_YELLOW);
    open_app_screen(s);
}

// ---- 14. CONNECT 4 (7 cols x 6 rows) --------------------------------
static int c4_b[6][7]; static int c4_turn=1;
static lv_obj_t* c4_cells[6][7];
static int c4_check(){
    for(int y=0;y<6;y++) for(int x=0;x<7;x++){
        int v=c4_b[y][x]; if(!v) continue;
        int DX[]={1,0,1,1}, DY[]={0,1,1,-1};
        for(int d=0;d<4;d++){
            int n=1;
            for(int k=1;k<4;k++){int nx=x+DX[d]*k, ny=y+DY[d]*k;
                if(nx<0||nx>=7||ny<0||ny>=6||c4_b[ny][nx]!=v) break; n++;}
            if(n>=4) return v;
        }
    }
    return 0;
}
static void c4_render(){
    for(int y=0;y<6;y++) for(int x=0;x<7;x++){
        uint32_t c = c4_b[y][x]==1?COL_CYAN: c4_b[y][x]==2?COL_MAGENTA: 0x1A1A1A;
        lv_obj_set_style_bg_color(c4_cells[y][x], lv_color_hex(c),0);
    }
    int w=c4_check();
    if(w==1) game_set_score("YOU WIN  (tap to reset)");
    else if(w==2) game_set_score("AI WINS  (tap to reset)");
    else game_set_score("DROP A DISC");
}
static int c4_drop(int col, int who){
    for(int y=5;y>=0;y--) if(!c4_b[y][col]){ c4_b[y][col]=who; return y; }
    return -1;
}
static void c4_ai(){
    // try win, then block, then center, then random
    for(int p=2;p>=1;p--){
        for(int x=0;x<7;x++){ int y=c4_drop(x,p); if(y<0) continue; if(c4_check()==p){ if(p==1){c4_b[y][x]=0; c4_b[y][x]=2;} return; } c4_b[y][x]=0; }
    }
    int order[7]={3,2,4,1,5,0,6};
    for(int i=0;i<7;i++){ if(c4_drop(order[i],2)>=0) return; }
}
static void c4_tap(lv_event_t* e){
    int col = (int)(intptr_t)lv_event_get_user_data(e);
    if(c4_check()){ memset(c4_b,0,sizeof(c4_b)); c4_render(); return; }
    if(c4_drop(col,1)<0){ c4_render(); return; }
    if(!c4_check()) c4_ai();
    c4_render();
}
static void open_connect4(){
    lv_obj_t *s = game_screen_base("CONNECT 4", COL_CYAN);
    memset(c4_b,0,sizeof(c4_b));
    lv_obj_t *grid=lv_obj_create(s); lv_obj_remove_style_all(grid);
    lv_obj_set_size(grid,350,300); lv_obj_align(grid,LV_ALIGN_CENTER,0,0);
    lv_obj_clear_flag(grid, LV_OBJ_FLAG_SCROLLABLE);
    int sz=46;
    for(int y=0;y<6;y++) for(int x=0;x<7;x++){
        lv_obj_t *cc=lv_obj_create(grid); lv_obj_remove_style_all(cc);
        lv_obj_set_size(cc,sz-4,sz-4); lv_obj_set_pos(cc, x*sz+4, y*sz+4);
        lv_obj_set_style_radius(cc,(sz-4)/2,0);
        lv_obj_set_style_bg_color(cc,lv_color_hex(0x1A1A1A),0);
        lv_obj_set_style_bg_opa(cc,LV_OPA_COVER,0);
        lv_obj_add_flag(cc, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(cc, c4_tap, LV_EVENT_CLICKED, (void*)(intptr_t)x);
        c4_cells[y][x]=cc;
    }
    c4_render();
    game_add_exit(s, COL_CYAN);
    open_app_screen(s);
}

// ---- 15. 15-PUZZLE --------------------------------------------------
static int fp[16]; static lv_obj_t* fp_cells[16];
static void fp_render(){
    for(int i=0;i<16;i++){
        int v=fp[i];
        lv_obj_set_style_bg_color(fp_cells[i], lv_color_hex(v?COL_CYAN:0x111111),0);
        lv_obj_set_style_bg_opa(fp_cells[i], v?160:LV_OPA_COVER,0);
        lv_obj_t *l=(lv_obj_t*)lv_obj_get_child(fp_cells[i],0);
        if(l){ if(v){char b[6]; snprintf(b,6,"%d",v); lv_label_set_text(l,b);} else lv_label_set_text(l,""); }
    }
    int ok=1; for(int i=0;i<15;i++) if(fp[i]!=i+1){ok=0; break;}
    game_set_score(ok?"SOLVED!":"slide to order");
}
static void fp_tap(lv_event_t* e){
    int i=(int)(intptr_t)lv_event_get_user_data(e);
    int z=-1; for(int k=0;k<16;k++) if(!fp[k]){z=k; break;}
    int zr=z/4,zc=z%4, ir=i/4,ic=i%4;
    if((zr==ir && abs(zc-ic)==1) || (zc==ic && abs(zr-ir)==1)){
        fp[z]=fp[i]; fp[i]=0; fp_render();
    }
}
static void open_fifteen(){
    lv_obj_t *s = game_screen_base("15-PUZZLE", COL_CYAN);
    for(int i=0;i<15;i++) fp[i]=i+1; fp[15]=0;
    for(int k=0;k<200;k++){ int z=-1; for(int q=0;q<16;q++) if(!fp[q]){z=q;break;}
        int zr=z/4,zc=z%4; int dir=random(4);
        int nr=zr+(dir==0?-1:dir==1?1:0), nc=zc+(dir==2?-1:dir==3?1:0);
        if(nr<0||nr>=4||nc<0||nc>=4) continue;
        int ni=nr*4+nc; fp[z]=fp[ni]; fp[ni]=0; }
    lv_obj_t *grid=lv_obj_create(s); lv_obj_remove_style_all(grid);
    lv_obj_set_size(grid,320,320); lv_obj_align(grid,LV_ALIGN_CENTER,0,0);
    lv_obj_clear_flag(grid, LV_OBJ_FLAG_SCROLLABLE);
    for(int i=0;i<16;i++){
        int r=i/4,c=i%4;
        lv_obj_t *cc=lv_obj_create(grid); lv_obj_remove_style_all(cc);
        lv_obj_set_size(cc,74,74); lv_obj_set_pos(cc,c*80,r*80);
        lv_obj_set_style_radius(cc,10,0);
        lv_obj_set_style_bg_color(cc,lv_color_hex(COL_CYAN),0);
        lv_obj_set_style_bg_opa(cc,160,0);
        lv_obj_add_flag(cc, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(cc, fp_tap, LV_EVENT_CLICKED, (void*)(intptr_t)i);
        lv_obj_t *l=lv_label_create(cc); lv_obj_center(l);
        lv_obj_set_style_text_color(l,lv_color_hex(COL_BG),0);
        lv_obj_set_style_text_font(l,&lv_font_montserrat_22,0);
        fp_cells[i]=cc;
    }
    fp_render();
    game_add_exit(s, COL_CYAN);
    open_app_screen(s);
}

// ---- 16. SPACE INVADERS --------------------------------------------
static float si_px; static int si_score=0;
static float si_ax[18], si_ay[18]; static int si_aalive[18]; static float si_dir=1.0f;
static float si_bx[8], si_by[8]; static int si_balive[8]={0};
static void si_step(lv_timer_t*){
    bool flip=false; int alive=0;
    for(int i=0;i<18;i++){ if(!si_aalive[i]) continue; alive++; si_ax[i]+=si_dir; if(si_ax[i]<10||si_ax[i]>GW-30) flip=true; }
    if(flip){ si_dir=-si_dir; for(int i=0;i<18;i++) si_ay[i]+=10; }
    for(int i=0;i<8;i++) if(si_balive[i]){ si_by[i]-=8; if(si_by[i]<0) si_balive[i]=0;
        for(int j=0;j<18;j++) if(si_aalive[j] && si_bx[i]>si_ax[j]&&si_bx[i]<si_ax[j]+20&&si_by[i]>si_ay[j]&&si_by[i]<si_ay[j]+14){ si_aalive[j]=0; si_balive[i]=0; si_score+=10; } }
    if(!alive){ for(int i=0;i<18;i++){si_aalive[i]=1; si_ax[i]=20+(i%6)*50; si_ay[i]=20+(i/6)*28;} }
    lv_canvas_fill_bg(g_game_canvas, lv_color_hex(0x050505), LV_OPA_COVER);
    for(int i=0;i<18;i++) if(si_aalive[i]) game_canvas_rect(g_game_canvas, (int)si_ax[i],(int)si_ay[i],20,14,COL_GREEN,2);
    for(int i=0;i<8;i++) if(si_balive[i]) game_canvas_rect(g_game_canvas, (int)si_bx[i]-2,(int)si_by[i]-4,4,8,COL_CYAN,1);
    game_canvas_rect(g_game_canvas, (int)si_px, GH-26, 36,12, COL_CYAN, 4);
    game_set_score("SCORE  %d", si_score);
}
static void si_drag(lv_event_t*){
    lv_point_t p; lv_indev_get_point(lv_indev_get_act(),&p);
    lv_area_t a; lv_obj_get_coords(g_game_canvas,&a);
    int lx = p.x - a.x1 - 18;
    if(lx<0) lx=0; if(lx>GW-36) lx=GW-36; si_px=lx;
}
static void si_fire(lv_event_t*){
    for(int i=0;i<8;i++) if(!si_balive[i]){ si_balive[i]=1; si_bx[i]=si_px+18; si_by[i]=GH-30; break; }
}
static void open_invaders(){
    lv_obj_t *s = game_screen_base("INVADERS", COL_GREEN);
    lv_obj_t *cv = game_add_canvas(s, GW, GH);
    si_px = GW/2-18; si_score=0; si_dir=1.0f;
    for(int i=0;i<18;i++){si_aalive[i]=1; si_ax[i]=20+(i%6)*50; si_ay[i]=20+(i/6)*28;}
    for(int i=0;i<8;i++) si_balive[i]=0;
    lv_obj_add_event_cb(cv, si_drag, LV_EVENT_PRESSING, NULL);
    lv_obj_add_event_cb(cv, si_fire, LV_EVENT_CLICKED, NULL);
    g_game_timer = lv_timer_create(si_step, 50, NULL);
    game_add_exit(s, COL_GREEN);
    open_app_screen(s);
}

// ---- 17. ASTEROIDS (touch variant) ---------------------------------
static float as_x,as_y,as_vx,as_vy,as_ang; static int as_score=0;
static float as_ox[6], as_oy[6], as_ovx[6], as_ovy[6]; static int as_oalive[6];
static void as_step(lv_timer_t*){
    as_x+=as_vx; as_y+=as_vy; as_vx*=0.99f; as_vy*=0.99f;
    if(as_x<0)as_x+=GW; if(as_x>GW)as_x-=GW; if(as_y<0)as_y+=GH; if(as_y>GH)as_y-=GH;
    for(int i=0;i<6;i++) if(as_oalive[i]){
        as_ox[i]+=as_ovx[i]; as_oy[i]+=as_ovy[i];
        if(as_ox[i]<0)as_ox[i]+=GW; if(as_ox[i]>GW)as_ox[i]-=GW;
        if(as_oy[i]<0)as_oy[i]+=GH; if(as_oy[i]>GH)as_oy[i]-=GH;
        float dx=as_ox[i]-as_x, dy=as_oy[i]-as_y;
        if(dx*dx+dy*dy<400){ as_oalive[i]=0; as_score+=10; }
    }
    int alive=0; for(int i=0;i<6;i++)alive+=as_oalive[i];
    if(!alive) for(int i=0;i<6;i++){as_oalive[i]=1; as_ox[i]=random(GW); as_oy[i]=random(GH); as_ovx[i]=(random(40)-20)/20.0f; as_ovy[i]=(random(40)-20)/20.0f;}
    lv_canvas_fill_bg(g_game_canvas, lv_color_hex(0x050505), LV_OPA_COVER);
    for(int i=0;i<6;i++) if(as_oalive[i]) game_canvas_circle(g_game_canvas, (int)as_ox[i], (int)as_oy[i], 14, COL_MAGENTA);
    int hx=(int)(as_x + cosf(as_ang)*10), hy=(int)(as_y + sinf(as_ang)*10);
    game_canvas_rect(g_game_canvas, hx-3,hy-3,6,6, COL_CYAN, 3);
    game_canvas_rect(g_game_canvas, (int)as_x-4,(int)as_y-4,8,8, COL_WHITE, 4);
    game_set_score("SCORE %d", as_score);
}
static void as_tap(lv_event_t*){
    lv_point_t p; lv_indev_get_point(lv_indev_get_act(),&p);
    lv_area_t a; lv_obj_get_coords(g_game_canvas,&a);
    int lx=p.x-a.x1; int third=GW/3;
    if(lx<third) as_ang -= 0.3f;
    else if(lx>2*third) as_ang += 0.3f;
    else { as_vx += cosf(as_ang)*1.2f; as_vy += sinf(as_ang)*1.2f; }
}
static void open_asteroids(){
    lv_obj_t *s = game_screen_base("ASTEROIDS", COL_CYAN);
    lv_obj_t *cv = game_add_canvas(s, GW, GH);
    as_x=GW/2; as_y=GH/2; as_vx=as_vy=0; as_ang=-1.57f; as_score=0;
    for(int i=0;i<6;i++){as_oalive[i]=1; as_ox[i]=random(GW); as_oy[i]=random(GH); as_ovx[i]=(random(40)-20)/20.0f; as_ovy[i]=(random(40)-20)/20.0f;}
    lv_obj_add_event_cb(cv, as_tap, LV_EVENT_CLICKED, NULL);
    g_game_timer = lv_timer_create(as_step, 40, NULL);
    game_add_exit(s, COL_CYAN);
    open_app_screen(s);
}

// ---- 18. DOODLE DODGE ----------------------------------------------
static float dd_x, dd_y, dd_vy; static int dd_score=0, dd_dead=0;
static float dd_px[6], dd_py[6];
static void dd_step(lv_timer_t*){
    if(!dd_dead){
        dd_vy += 0.45f; dd_y += dd_vy;
        if(dd_y > GH-30) { dd_vy = -10.5f; dd_score++; }
        for(int i=0;i<6;i++){
            dd_py[i] += 0.6f;
            if(dd_py[i] > GH){ dd_py[i]=-20; dd_px[i]=random(GW-50); }
            if(fabsf(dd_x-dd_px[i])<35 && fabsf(dd_y-dd_py[i])<20) dd_dead=1;
        }
    }
    lv_canvas_fill_bg(g_game_canvas, lv_color_hex(0x050505), LV_OPA_COVER);
    for(int i=0;i<6;i++) game_canvas_rect(g_game_canvas, (int)dd_px[i], (int)dd_py[i], 50, 8, COL_RED, 2);
    game_canvas_rect(g_game_canvas, (int)dd_x-12, (int)dd_y-12, 24,24, dd_dead?COL_RED:COL_MAGENTA, 4);
    game_set_score("JUMPS %d%s", dd_score, dd_dead?"  TAP RESTART":"");
}
static void dd_drag(lv_event_t*){
    if(dd_dead) return;
    lv_point_t p; lv_indev_get_point(lv_indev_get_act(),&p);
    lv_area_t a; lv_obj_get_coords(g_game_canvas,&a);
    dd_x = p.x - a.x1;
}
static void dd_tap(lv_event_t*){
    if(dd_dead){ dd_dead=0; dd_score=0; dd_y=GH/2; dd_vy=0; for(int i=0;i<6;i++){dd_px[i]=random(GW-50); dd_py[i]=i*70;} }
}
static void open_doodle(){
    lv_obj_t *s = game_screen_base("DOODLE DODGE", COL_MAGENTA);
    lv_obj_t *cv = game_add_canvas(s, GW, GH);
    dd_x=GW/2; dd_y=GH/2; dd_vy=0; dd_score=0; dd_dead=0;
    for(int i=0;i<6;i++){dd_px[i]=random(GW-50); dd_py[i]=i*70;}
    lv_obj_add_event_cb(cv, dd_drag, LV_EVENT_PRESSING, NULL);
    lv_obj_add_event_cb(cv, dd_tap, LV_EVENT_CLICKED, NULL);
    g_game_timer = lv_timer_create(dd_step, 28, NULL);
    game_add_exit(s, COL_MAGENTA);
    open_app_screen(s);
}

// ---- 19. FROGGER ---------------------------------------------------
static int fg_px,fg_py; static int fg_score=0,fg_dead=0;
static float fg_lane[6]; static int fg_dir[6];
static void fg_step(lv_timer_t*){
    if(!fg_dead){
        for(int i=0;i<6;i++){ fg_lane[i] += fg_dir[i]*(2.0f+i*0.4f);
            if(fg_lane[i]<-80) fg_lane[i]=GW; if(fg_lane[i]>GW) fg_lane[i]=-80; }
        // collision check
        int laneY = 40 + fg_py*40;
        for(int i=0;i<6;i++){
            int y = 40+i*40;
            if(abs(laneY-y) < 18){
                for(int k=0;k<3;k++){ int x=(int)(fg_lane[i]+k*120)%GW; if(x<0)x+=GW;
                    if(fg_px*60+10 < x+50 && fg_px*60+50 > x) fg_dead=1; }
            }
        }
    }
    lv_canvas_fill_bg(g_game_canvas, lv_color_hex(0x050505), LV_OPA_COVER);
    for(int i=0;i<6;i++){
        int y=40+i*40;
        game_canvas_rect(g_game_canvas, 0,y-15,GW,30, 0x111111, 0);
        for(int k=0;k<3;k++){ int x=(int)(fg_lane[i]+k*120)%GW; if(x<0)x+=GW;
            game_canvas_rect(g_game_canvas, x, y-10, 50, 20, i%2?COL_MAGENTA:COL_CYAN, 4); }
    }
    int pxScr = fg_px*60+15, pyScr = 40+fg_py*40 - 12;
    game_canvas_rect(g_game_canvas, pxScr, pyScr, 30, 24, fg_dead?COL_RED:COL_GREEN, 4);
    game_set_score("CROSSINGS %d%s", fg_score, fg_dead?"  swipe to retry":"");
}
static void fg_swipe(lv_event_t*){
    if(fg_dead){ fg_dead=0; fg_px=2; fg_py=0; return; }
    lv_dir_t d=lv_indev_get_gesture_dir(lv_indev_get_act());
    if(d==LV_DIR_LEFT && fg_px>0) fg_px--;
    if(d==LV_DIR_RIGHT && fg_px<5) fg_px++;
    if(d==LV_DIR_TOP){ fg_py++; if(fg_py>=6){ fg_score++; fg_py=0; } }
    if(d==LV_DIR_BOTTOM && fg_py>0) fg_py--;
}
static void open_frogger(){
    lv_obj_t *s = game_screen_base("FROGGER", COL_GREEN);
    game_add_canvas(s, GW, GH);
    fg_px=2; fg_py=0; fg_score=0; fg_dead=0;
    for(int i=0;i<6;i++){ fg_lane[i]=random(GW); fg_dir[i]=(i%2)?1:-1; }
    lv_obj_add_event_cb(s, fg_swipe, LV_EVENT_GESTURE, NULL);
    g_game_timer = lv_timer_create(fg_step, 30, NULL);
    game_add_exit(s, COL_GREEN);
    open_app_screen(s);
}

// ---- 20. MAZE (procedural simple grid) -----------------------------
#define MZ 11
static uint8_t mz[MZ][MZ]; // 1=wall
static int mz_px, mz_py;
static void mz_carve(int x,int y){
    mz[y][x]=0;
    int dirs[4]={0,1,2,3};
    for(int i=3;i>0;i--){ int j=random(i+1); int t=dirs[i];dirs[i]=dirs[j];dirs[j]=t; }
    for(int i=0;i<4;i++){
        int dx=(dirs[i]==0?-1:dirs[i]==1?1:0)*2, dy=(dirs[i]==2?-1:dirs[i]==3?1:0)*2;
        int nx=x+dx, ny=y+dy;
        if(nx>0&&nx<MZ-1&&ny>0&&ny<MZ-1 && mz[ny][nx]){
            mz[y+dy/2][x+dx/2]=0; mz_carve(nx,ny);
        }
    }
}
static void mz_draw(){
    int cs = GW/MZ;
    lv_canvas_fill_bg(g_game_canvas, lv_color_hex(0x050505), LV_OPA_COVER);
    for(int y=0;y<MZ;y++) for(int x=0;x<MZ;x++){
        if(mz[y][x]) game_canvas_rect(g_game_canvas, x*cs, y*cs, cs-1, cs-1, 0x222222, 2);
    }
    game_canvas_rect(g_game_canvas, (MZ-2)*cs, (MZ-2)*cs, cs-1, cs-1, COL_MAGENTA, 4);
    game_canvas_rect(g_game_canvas, mz_px*cs+2, mz_py*cs+2, cs-5, cs-5, COL_CYAN, 4);
    if(mz_px==MZ-2 && mz_py==MZ-2) game_set_score("ESCAPED! swipe to re-gen");
    else game_set_score("swipe to navigate");
}
static void mz_swipe(lv_event_t*){
    if(mz_px==MZ-2 && mz_py==MZ-2){
        for(int y=0;y<MZ;y++)for(int x=0;x<MZ;x++) mz[y][x]=1;
        mz_carve(1,1); mz_px=1; mz_py=1; mz_draw(); return;
    }
    lv_dir_t d=lv_indev_get_gesture_dir(lv_indev_get_act());
    int nx=mz_px, ny=mz_py;
    if(d==LV_DIR_LEFT) nx--; if(d==LV_DIR_RIGHT) nx++;
    if(d==LV_DIR_TOP) ny--; if(d==LV_DIR_BOTTOM) ny++;
    if(nx>=0&&nx<MZ&&ny>=0&&ny<MZ && !mz[ny][nx]){ mz_px=nx; mz_py=ny; mz_draw(); }
}
static void open_maze(){
    lv_obj_t *s = game_screen_base("MAZE", COL_CYAN);
    game_add_canvas(s, GW, GW);
    for(int y=0;y<MZ;y++)for(int x=0;x<MZ;x++) mz[y][x]=1;
    mz_carve(1,1); mz_px=1; mz_py=1;
    lv_obj_add_event_cb(s, mz_swipe, LV_EVENT_GESTURE, NULL);
    mz_draw();
    game_add_exit(s, COL_CYAN);
    open_app_screen(s);
}

// ==================== END games_pack2.h ====================

// ==================== INLINED FROM games_pack3.h ====================
// =====================================================================
//  games_pack3.h  -  SHOWPIECES (10 games)
//    TRON, MATCH-3, PINBALL, WORDLE, SUDOKU, CHECKERS,
//    AIR-HOCKEY, PLATFORMER, TOWER-STACK, CROSSY
// =====================================================================


// ---- 21. TRON LIGHTCYCLES -----------------------------------------
#define TR_W 36
#define TR_H 38
static uint8_t tr_g[TR_H][TR_W];
static int tr_px, tr_py, tr_pdx, tr_pdy;
static int tr_ax, tr_ay, tr_adx, tr_ady;
static int tr_winner=0;
static void tr_step(lv_timer_t*){
    if(tr_winner){ return; }
    int npx=tr_px+tr_pdx, npy=tr_py+tr_pdy;
    int nax=tr_ax+tr_adx, nay=tr_ay+tr_ady;
    bool ph = (npx<0||npx>=TR_W||npy<0||npy>=TR_H||tr_g[npy][npx]);
    bool ah = (nax<0||nax>=TR_W||nay<0||nay>=TR_H||tr_g[nay][nax]);
    if(ph && ah) tr_winner=3;
    else if(ph) tr_winner=2;
    else if(ah) tr_winner=1;
    else {
        tr_px=npx; tr_py=npy; tr_g[tr_py][tr_px]=1;
        // simple AI: avoid walls/trail, prefer toward player
        int choices[4][2]={{1,0},{-1,0},{0,1},{0,-1}};
        // prevent reversing
        int rev_x=-tr_adx, rev_y=-tr_ady;
        int best=-1; int bestscore=-1;
        for(int i=0;i<4;i++){
            int dx=choices[i][0], dy=choices[i][1];
            if(dx==rev_x && dy==rev_y) continue;
            int nx=tr_ax+dx, ny=tr_ay+dy;
            if(nx<0||nx>=TR_W||ny<0||ny>=TR_H||tr_g[ny][nx]) continue;
            int score = 1000 - abs(nx-tr_px)-abs(ny-tr_py) + random(8);
            if(score>bestscore){ bestscore=score; best=i; }
        }
        if(best>=0){ tr_adx=choices[best][0]; tr_ady=choices[best][1]; tr_ax+=tr_adx; tr_ay+=tr_ady; tr_g[tr_ay][tr_ax]=2; }
    }
    int cs = GW/TR_W;
    lv_canvas_fill_bg(g_game_canvas, lv_color_hex(0x050505), LV_OPA_COVER);
    for(int y=0;y<TR_H;y++) for(int x=0;x<TR_W;x++){
        if(tr_g[y][x]==1) game_canvas_rect(g_game_canvas, x*cs,y*cs,cs-1,cs-1,COL_MAGENTA,1);
        else if(tr_g[y][x]==2) game_canvas_rect(g_game_canvas, x*cs,y*cs,cs-1,cs-1,COL_CYAN,1);
    }
    game_canvas_rect(g_game_canvas, tr_px*cs, tr_py*cs, cs+2, cs+2, COL_WHITE, 2);
    game_canvas_rect(g_game_canvas, tr_ax*cs, tr_ay*cs, cs+2, cs+2, COL_WHITE, 2);
    game_set_score(tr_winner==1?"YOU WIN  swipe to restart":
                   tr_winner==2?"AI WINS  swipe to restart":
                   tr_winner==3?"DRAW  swipe to restart":"YOU vs AI");
}
static void tr_swipe(lv_event_t*){
    if(tr_winner){
        memset(tr_g,0,sizeof(tr_g)); tr_winner=0;
        tr_px=4; tr_py=TR_H/2; tr_pdx=1; tr_pdy=0;
        tr_ax=TR_W-5; tr_ay=TR_H/2; tr_adx=-1; tr_ady=0;
        tr_g[tr_py][tr_px]=1; tr_g[tr_ay][tr_ax]=2; return;
    }
    lv_dir_t d=lv_indev_get_gesture_dir(lv_indev_get_act());
    if(d==LV_DIR_LEFT && tr_pdx==0){ tr_pdx=-1; tr_pdy=0; }
    if(d==LV_DIR_RIGHT && tr_pdx==0){ tr_pdx=1; tr_pdy=0; }
    if(d==LV_DIR_TOP && tr_pdy==0){ tr_pdx=0; tr_pdy=-1; }
    if(d==LV_DIR_BOTTOM && tr_pdy==0){ tr_pdx=0; tr_pdy=1; }
}
static void open_tron(){
    lv_obj_t *s = game_screen_base("TRON", COL_CYAN);
    game_add_canvas(s, GW, GH);
    memset(tr_g,0,sizeof(tr_g));
    tr_px=4; tr_py=TR_H/2; tr_pdx=1; tr_pdy=0;
    tr_ax=TR_W-5; tr_ay=TR_H/2; tr_adx=-1; tr_ady=0;
    tr_g[tr_py][tr_px]=1; tr_g[tr_ay][tr_ax]=2; tr_winner=0;
    lv_obj_add_event_cb(s, tr_swipe, LV_EVENT_GESTURE, NULL);
    g_game_timer = lv_timer_create(tr_step, 80, NULL);
    game_add_exit(s, COL_CYAN);
    open_app_screen(s);
}

// ---- 22. MATCH-3 (6x6, swap adjacent, find matches) ----------------
#define M3_N 6
static int m3_g[M3_N][M3_N]; static int m3_score=0, m3_sel=-1;
static const uint32_t M3_PAL[5]={COL_CYAN,COL_MAGENTA,COL_GREEN,COL_YELLOW,COL_ORANGE};
static lv_obj_t* m3_cells[M3_N][M3_N];
static int m3_resolve(){
    int hits=0; uint8_t mark[M3_N][M3_N]={{0}};
    for(int y=0;y<M3_N;y++) for(int x=0;x<M3_N-2;x++)
        if(m3_g[y][x] && m3_g[y][x]==m3_g[y][x+1] && m3_g[y][x]==m3_g[y][x+2]){
            mark[y][x]=mark[y][x+1]=mark[y][x+2]=1; }
    for(int x=0;x<M3_N;x++) for(int y=0;y<M3_N-2;y++)
        if(m3_g[y][x] && m3_g[y][x]==m3_g[y+1][x] && m3_g[y][x]==m3_g[y+2][x]){
            mark[y][x]=mark[y+1][x]=mark[y+2][x]=1; }
    for(int y=0;y<M3_N;y++) for(int x=0;x<M3_N;x++) if(mark[y][x]){ m3_g[y][x]=0; hits++; }
    // gravity + refill
    for(int x=0;x<M3_N;x++){ int wr=M3_N-1;
        for(int y=M3_N-1;y>=0;y--) if(m3_g[y][x]){ m3_g[wr--][x]=m3_g[y][x]; }
        while(wr>=0) m3_g[wr--][x]=0;
        for(int y=0;y<M3_N;y++) if(!m3_g[y][x]) m3_g[y][x]=1+random(5);
    }
    return hits;
}
static void m3_render(){
    for(int y=0;y<M3_N;y++) for(int x=0;x<M3_N;x++){
        lv_obj_set_style_bg_color(m3_cells[y][x], lv_color_hex(M3_PAL[m3_g[y][x]-1]),0);
        bool sel = (m3_sel == y*M3_N+x);
        lv_obj_set_style_border_color(m3_cells[y][x], lv_color_hex(COL_WHITE),0);
        lv_obj_set_style_border_width(m3_cells[y][x], sel?3:0, 0);
    }
    game_set_score("SCORE %d", m3_score);
}
static void m3_tap(lv_event_t* e){
    int i=(int)(intptr_t)lv_event_get_user_data(e);
    if(m3_sel<0){ m3_sel=i; m3_render(); return; }
    int sy=m3_sel/M3_N, sx=m3_sel%M3_N, ny=i/M3_N, nx=i%M3_N;
    if(abs(sy-ny)+abs(sx-nx)==1){
        int t=m3_g[sy][sx]; m3_g[sy][sx]=m3_g[ny][nx]; m3_g[ny][nx]=t;
        int h = m3_resolve();
        if(h==0){ int t2=m3_g[sy][sx]; m3_g[sy][sx]=m3_g[ny][nx]; m3_g[ny][nx]=t2; }
        else { m3_score += h*10; while(m3_resolve()>0) m3_score += 10; }
    }
    m3_sel=-1; m3_render();
}
static void open_match3(){
    lv_obj_t *s = game_screen_base("MATCH-3", COL_MAGENTA);
    for(int y=0;y<M3_N;y++) for(int x=0;x<M3_N;x++) m3_g[y][x]=1+random(5);
    while(m3_resolve()>0); m3_score=0; m3_sel=-1;
    lv_obj_t *grid=lv_obj_create(s); lv_obj_remove_style_all(grid);
    lv_obj_set_size(grid,360,360); lv_obj_align(grid,LV_ALIGN_CENTER,0,0);
    lv_obj_clear_flag(grid, LV_OBJ_FLAG_SCROLLABLE);
    for(int y=0;y<M3_N;y++) for(int x=0;x<M3_N;x++){
        lv_obj_t *cc=lv_obj_create(grid); lv_obj_remove_style_all(cc);
        lv_obj_set_size(cc,52,52); lv_obj_set_pos(cc, x*58, y*58);
        lv_obj_set_style_radius(cc,12,0);
        lv_obj_set_style_bg_opa(cc,LV_OPA_COVER,0);
        lv_obj_add_flag(cc, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(cc, m3_tap, LV_EVENT_CLICKED, (void*)(intptr_t)(y*M3_N+x));
        m3_cells[y][x]=cc;
    }
    m3_render();
    game_add_exit(s, COL_MAGENTA);
    open_app_screen(s);
}

// ---- 23. PINBALL ---------------------------------------------------
static float pb_bx, pb_by, pb_vx, pb_vy; static int pb_score=0, pb_lflip=0, pb_rflip=0;
static void pb_step(lv_timer_t*){
    pb_vy += 0.18f; pb_bx += pb_vx; pb_by += pb_vy;
    if(pb_bx<8){pb_bx=8; pb_vx=fabsf(pb_vx);}
    if(pb_bx>GW-16){pb_bx=GW-16; pb_vx=-fabsf(pb_vx);}
    if(pb_by<8){pb_by=8; pb_vy=fabsf(pb_vy);}
    // bumpers
    static const int bp[3][2]={{GW/2-50,120},{GW/2+50,120},{GW/2,180}};
    for(int i=0;i<3;i++){
        float dx=pb_bx-bp[i][0], dy=pb_by-bp[i][1];
        if(dx*dx+dy*dy<400){ float m=sqrtf(dx*dx+dy*dy)+0.01f; pb_vx=dx/m*4; pb_vy=dy/m*4; pb_score+=50; }
    }
    // flippers
    if(pb_lflip && pb_by>GH-50 && pb_bx<GW/2 && pb_bx>20){ pb_vy=-5.5f; pb_vx+=1.5f; pb_score+=5; }
    if(pb_rflip && pb_by>GH-50 && pb_bx>GW/2 && pb_bx<GW-20){ pb_vy=-5.5f; pb_vx-=1.5f; pb_score+=5; }
    if(pb_by>GH){ pb_score=0; pb_bx=GW/2; pb_by=40; pb_vx=2; pb_vy=0; }
    lv_canvas_fill_bg(g_game_canvas, lv_color_hex(0x050505), LV_OPA_COVER);
    for(int i=0;i<3;i++) game_canvas_circle(g_game_canvas, bp[i][0],bp[i][1],14, COL_YELLOW);
    game_canvas_rect(g_game_canvas, 20, GH-30, 80, 8, pb_lflip?COL_CYAN:0x444444, 4);
    game_canvas_rect(g_game_canvas, GW-100, GH-30, 80, 8, pb_rflip?COL_MAGENTA:0x444444, 4);
    game_canvas_rect(g_game_canvas, (int)pb_bx-5,(int)pb_by-5,10,10, COL_WHITE, 5);
    game_set_score("SCORE %d", pb_score);
}
static void pb_press(lv_event_t*){
    lv_point_t p; lv_indev_get_point(lv_indev_get_act(),&p);
    lv_area_t a; lv_obj_get_coords(g_game_canvas,&a);
    int lx=p.x-a.x1;
    if(lx<GW/2) pb_lflip=1; else pb_rflip=1;
}
static void pb_release(lv_event_t*){ pb_lflip=pb_rflip=0; }
static void open_pinball(){
    lv_obj_t *s = game_screen_base("PINBALL", COL_YELLOW);
    lv_obj_t *cv = game_add_canvas(s, GW, GH);
    pb_bx=GW/2; pb_by=40; pb_vx=2; pb_vy=0; pb_score=0;
    lv_obj_add_event_cb(cv, pb_press, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(cv, pb_release, LV_EVENT_RELEASED, NULL);
    g_game_timer = lv_timer_create(pb_step, 24, NULL);
    game_add_exit(s, COL_YELLOW);
    open_app_screen(s);
}

// ---- 24. WORDLE (mini, fixed 16-word dictionary) -------------------
static const char* WORDS[16] = {
    "ARRAY","BLOCK","CHIPS","DRIVE","ETHIC","FLASK","GAMES","HOVER",
    "INPUT","JUMPY","KIOSK","LASER","MERGE","NEURO","OASIS","PIXEL"
};
static char wd_target[6]; static char wd_grid[6][6]; static int wd_row=0, wd_col=0;
static lv_obj_t* wd_lbls[6][5]; static lv_obj_t* wd_kbd[26];
static void wd_render(){
    for(int r=0;r<6;r++) for(int c=0;c<5;c++){
        char b[2]={wd_grid[r][c]?wd_grid[r][c]:' ',0};
        lv_label_set_text(wd_lbls[r][c], b);
        uint32_t col=0x1A1A1A;
        if(r<wd_row && wd_grid[r][c]){
            if(wd_grid[r][c]==wd_target[c]) col=COL_GREEN;
            else { col=0x444444; for(int k=0;k<5;k++) if(wd_target[k]==wd_grid[r][c]){ col=COL_YELLOW; break; } }
        }
        lv_obj_set_style_bg_color(lv_obj_get_parent(wd_lbls[r][c]), lv_color_hex(col),0);
    }
    bool won=true; for(int c=0;c<5;c++) if(wd_grid[wd_row-1][c]!=wd_target[c]) won=false;
    if(wd_row>0 && won) game_set_score("WIN!  tap to restart");
    else if(wd_row>=6) game_set_score("OUT - was %s", wd_target);
    else game_set_score("ROW %d / 6", wd_row+1);
}
static void wd_key(lv_event_t* e){
    if(wd_row>=6 || (wd_row>0 && [](){ for(int c=0;c<5;c++) if(wd_grid[0][c]) ; return true; }())) { /* allow restart logic below */ }
    int k=(int)(intptr_t)lv_event_get_user_data(e);
    // 26=backspace, 27=enter
    if(k==26){ if(wd_col>0){ wd_col--; wd_grid[wd_row][wd_col]=0; } wd_render(); return; }
    if(k==27){ if(wd_col==5){ wd_row++; wd_col=0; } wd_render(); return; }
    if(wd_col<5 && wd_row<6){ wd_grid[wd_row][wd_col++]='A'+k; wd_render(); }
}
static void open_wordle(){
    lv_obj_t *s = game_screen_base("WORDLE", COL_GREEN);
    strcpy(wd_target, WORDS[random(16)]);
    memset(wd_grid,0,sizeof(wd_grid)); wd_row=0; wd_col=0;
    // grid
    for(int r=0;r<6;r++) for(int c=0;c<5;c++){
        lv_obj_t *box=lv_obj_create(s); lv_obj_remove_style_all(box);
        lv_obj_set_size(box,40,40); lv_obj_set_pos(box, 75+c*50, 90+r*45);
        lv_obj_set_style_radius(box,6,0); lv_obj_set_style_bg_color(box,lv_color_hex(0x1A1A1A),0);
        lv_obj_set_style_bg_opa(box,LV_OPA_COVER,0);
        lv_obj_t *l=lv_label_create(box); wd_lbls[r][c]=l;
        lv_obj_set_style_text_color(l,lv_color_hex(COL_WHITE),0);
        lv_obj_set_style_text_font(l,&lv_font_montserrat_18,0); lv_obj_center(l);
    }
    // mini-keyboard
    const char* ROWS[3]={"QWERTYUIOP","ASDFGHJKL","ZXCVBNM"};
    int yy=370;
    for(int row=0;row<3;row++){
        int n=strlen(ROWS[row]);
        int xs=(LCD_W-n*32)/2;
        for(int i=0;i<n;i++){
            char ch=ROWS[row][i];
            lv_obj_t *b=lv_obj_create(s); lv_obj_remove_style_all(b);
            lv_obj_set_size(b,28,30); lv_obj_set_pos(b, xs+i*32, yy+row*34);
            lv_obj_set_style_radius(b,4,0); lv_obj_set_style_bg_color(b,lv_color_hex(0x222222),0);
            lv_obj_set_style_bg_opa(b,LV_OPA_COVER,0);
            lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_add_event_cb(b, wd_key, LV_EVENT_CLICKED, (void*)(intptr_t)(ch-'A'));
            lv_obj_t *l=lv_label_create(b); char bb[2]={ch,0}; lv_label_set_text(l,bb);
            lv_obj_set_style_text_color(l,lv_color_hex(COL_WHITE),0);
            lv_obj_set_style_text_font(l,&lv_font_montserrat_14,0); lv_obj_center(l);
        }
    }
    // enter / backspace
    lv_obj_t *be=lv_btn_create(s); lv_obj_set_size(be,80,30);
    lv_obj_align(be,LV_ALIGN_BOTTOM_LEFT,20,-60);
    lv_obj_t *le=lv_label_create(be); lv_label_set_text(le,"ENTER");
    lv_obj_set_style_text_color(le,lv_color_hex(COL_CYAN),0); lv_obj_center(le);
    lv_obj_add_event_cb(be, wd_key, LV_EVENT_CLICKED, (void*)(intptr_t)27);
    lv_obj_t *bb=lv_btn_create(s); lv_obj_set_size(bb,40,30);
    lv_obj_align(bb,LV_ALIGN_BOTTOM_RIGHT,-20,-60);
    lv_obj_t *lb=lv_label_create(bb); lv_label_set_text(lb,"<-");
    lv_obj_set_style_text_color(lb,lv_color_hex(COL_MAGENTA),0); lv_obj_center(lb);
    lv_obj_add_event_cb(bb, wd_key, LV_EVENT_CLICKED, (void*)(intptr_t)26);
    wd_render();
    game_add_exit(s, COL_GREEN);
    open_app_screen(s);
}

// ---- 25. SUDOKU (one fixed puzzle for sanity) ----------------------
static const int8_t SUDOKU_FIX[9][9] = {
    {5,3,0,0,7,0,0,0,0},{6,0,0,1,9,5,0,0,0},{0,9,8,0,0,0,0,6,0},
    {8,0,0,0,6,0,0,0,3},{4,0,0,8,0,3,0,0,1},{7,0,0,0,2,0,0,0,6},
    {0,6,0,0,0,0,2,8,0},{0,0,0,4,1,9,0,0,5},{0,0,0,0,8,0,0,7,9}
};
static int sk_g[9][9]; static int sk_sel=-1;
static lv_obj_t* sk_cells[9][9];
static void sk_render(){
    for(int y=0;y<9;y++) for(int x=0;x<9;x++){
        int v=sk_g[y][x];
        char b[3]={0}; if(v) b[0]='0'+v;
        lv_obj_t *l=(lv_obj_t*)lv_obj_get_child(sk_cells[y][x],0);
        if(l) lv_label_set_text(l,b);
        bool sel = (sk_sel == y*9+x);
        uint32_t bg = SUDOKU_FIX[y][x]?0x222222:0x101010;
        if(sel) bg = COL_CYAN;
        lv_obj_set_style_bg_color(sk_cells[y][x], lv_color_hex(bg),0);
        if(l) lv_obj_set_style_text_color(l, lv_color_hex(SUDOKU_FIX[y][x]?COL_WHITE:COL_CYAN),0);
    }
    game_set_score("tap cell · 1-9");
}
static void sk_tap(lv_event_t* e){
    int i=(int)(intptr_t)lv_event_get_user_data(e);
    int y=i/9,x=i%9;
    if(!SUDOKU_FIX[y][x]) sk_sel = i;
    sk_render();
}
static void sk_num(lv_event_t* e){
    int n=(int)(intptr_t)lv_event_get_user_data(e);
    if(sk_sel<0) return;
    int y=sk_sel/9,x=sk_sel%9;
    if(!SUDOKU_FIX[y][x]) sk_g[y][x] = n;
    sk_render();
}
static void open_sudoku(){
    lv_obj_t *s = game_screen_base("SUDOKU", COL_CYAN);
    for(int y=0;y<9;y++) for(int x=0;x<9;x++) sk_g[y][x] = SUDOKU_FIX[y][x];
    sk_sel=-1;
    int OX=(LCD_W-324)/2, OY=90, cs=36;
    for(int y=0;y<9;y++) for(int x=0;x<9;x++){
        lv_obj_t *cc=lv_obj_create(s); lv_obj_remove_style_all(cc);
        lv_obj_set_size(cc,cs-2,cs-2);
        lv_obj_set_pos(cc, OX+x*cs+((x/3)*2), OY+y*cs+((y/3)*2));
        lv_obj_set_style_radius(cc,3,0);
        lv_obj_set_style_bg_color(cc,lv_color_hex(SUDOKU_FIX[y][x]?0x222222:0x101010),0);
        lv_obj_set_style_bg_opa(cc,LV_OPA_COVER,0);
        lv_obj_add_flag(cc, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(cc, sk_tap, LV_EVENT_CLICKED, (void*)(intptr_t)(y*9+x));
        lv_obj_t *l=lv_label_create(cc); lv_obj_center(l);
        lv_obj_set_style_text_font(l,&lv_font_montserrat_14,0);
        sk_cells[y][x]=cc;
    }
    // number pad
    for(int n=1;n<=9;n++){
        lv_obj_t *b=lv_obj_create(s); lv_obj_remove_style_all(b);
        lv_obj_set_size(b,30,30); lv_obj_set_pos(b, 40+(n-1)*34, GW+62);
        lv_obj_set_style_radius(b,6,0);
        lv_obj_set_style_bg_color(b,lv_color_hex(0x222222),0);
        lv_obj_set_style_bg_opa(b,LV_OPA_COVER,0);
        lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(b, sk_num, LV_EVENT_CLICKED, (void*)(intptr_t)n);
        lv_obj_t *l=lv_label_create(b); char bb[2]={'0'+n,0}; lv_label_set_text(l,bb);
        lv_obj_set_style_text_color(l,lv_color_hex(COL_CYAN),0);
        lv_obj_set_style_text_font(l,&lv_font_montserrat_14,0); lv_obj_center(l);
    }
    sk_render();
    game_add_exit(s, COL_CYAN);
    open_app_screen(s);
}

// ---- 26. CHECKERS (simplified vs AI) -------------------------------
static int8_t ck_b[8][8]; static int ck_sel=-1; static int ck_turn=1;
static lv_obj_t* ck_cells[8][8];
static void ck_render(){
    for(int y=0;y<8;y++) for(int x=0;x<8;x++){
        uint32_t bg = ((x+y)&1)? 0x222222 : 0x0A0A0A;
        if(ck_sel == y*8+x) bg = COL_CYAN;
        lv_obj_set_style_bg_color(ck_cells[y][x], lv_color_hex(bg),0);
        lv_obj_t *p=(lv_obj_t*)lv_obj_get_child(ck_cells[y][x],0);
        if(p){
            int v = ck_b[y][x];
            if(v==1||v==2) lv_obj_set_style_bg_color(p, lv_color_hex(v==1?COL_CYAN:COL_MAGENTA),0);
            lv_obj_set_style_bg_opa(p, (v==1||v==2)?LV_OPA_COVER:0, 0);
        }
    }
    game_set_score("checkers (light AI)");
}
static void ck_ai(){
    // very basic: find any cyan piece (player=2 actually since we made 1=player; 2=ai) — we use 2=AI
    for(int y=7;y>=0;y--) for(int x=0;x<8;x++) if(ck_b[y][x]==2){
        int dirs[2][2]={{-1,-1},{-1,1}};
        for(int d=0;d<2;d++){
            int nx=x+dirs[d][1], ny=y+dirs[d][0];
            if(nx<0||nx>=8||ny<0||ny>=8) continue;
            if(!ck_b[ny][nx]){ ck_b[ny][nx]=2; ck_b[y][x]=0; return; }
        }
    }
}
static void ck_tap(lv_event_t* e){
    int i=(int)(intptr_t)lv_event_get_user_data(e);
    int y=i/8,x=i%8;
    if(ck_sel<0){ if(ck_b[y][x]==1) ck_sel=i; }
    else {
        int sy=ck_sel/8, sx=ck_sel%8;
        if(!ck_b[y][x] && abs(sy-y)==1 && abs(sx-x)==1 && y==sy-1){
            ck_b[y][x]=1; ck_b[sy][sx]=0; ck_sel=-1; ck_ai();
        } else ck_sel=-1;
    }
    ck_render();
}
static void open_checkers(){
    lv_obj_t *s = game_screen_base("CHECKERS", COL_MAGENTA);
    memset(ck_b,0,sizeof(ck_b));
    for(int y=0;y<3;y++) for(int x=0;x<8;x++) if((x+y)&1) ck_b[y][x]=2;
    for(int y=5;y<8;y++) for(int x=0;x<8;x++) if((x+y)&1) ck_b[y][x]=1;
    int OX=(LCD_W-320)/2, OY=90, cs=40;
    for(int y=0;y<8;y++) for(int x=0;x<8;x++){
        lv_obj_t *cc=lv_obj_create(s); lv_obj_remove_style_all(cc);
        lv_obj_set_size(cc,cs-2,cs-2); lv_obj_set_pos(cc, OX+x*cs, OY+y*cs);
        lv_obj_set_style_bg_opa(cc, LV_OPA_COVER,0);
        lv_obj_add_flag(cc, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(cc, ck_tap, LV_EVENT_CLICKED, (void*)(intptr_t)(y*8+x));
        lv_obj_t *p=lv_obj_create(cc); lv_obj_remove_style_all(p);
        lv_obj_set_size(p,28,28); lv_obj_center(p);
        lv_obj_set_style_radius(p,14,0);
        ck_cells[y][x]=cc;
    }
    ck_sel=-1; ck_render();
    game_add_exit(s, COL_MAGENTA);
    open_app_screen(s);
}

// ---- 27. AIR HOCKEY ------------------------------------------------
static float ah_bx, ah_by, ah_vx, ah_vy;
static float ah_p1x, ah_p1y, ah_p2x, ah_p2y;
static int ah_s1=0, ah_s2=0;
static void ah_step(lv_timer_t*){
    ah_bx+=ah_vx; ah_by+=ah_vy; ah_vx*=0.992f; ah_vy*=0.992f;
    if(ah_bx<8){ah_bx=8; ah_vx=fabsf(ah_vx);}
    if(ah_bx>GW-16){ah_bx=GW-16; ah_vx=-fabsf(ah_vx);}
    if(ah_by<0){ if(ah_bx>GW/2-40 && ah_bx<GW/2+40){ ah_s1++; ah_bx=GW/2; ah_by=GH/2; ah_vx=ah_vy=0; } else { ah_by=0; ah_vy=fabsf(ah_vy);} }
    if(ah_by>GH){ if(ah_bx>GW/2-40 && ah_bx<GW/2+40){ ah_s2++; ah_bx=GW/2; ah_by=GH/2; ah_vx=ah_vy=0; } else { ah_by=GH; ah_vy=-fabsf(ah_vy);} }
    // AI paddle (top)
    if(ah_bx > ah_p2x+5) ah_p2x += 2.5f;
    else if(ah_bx < ah_p2x-5) ah_p2x -= 2.5f;
    ah_p2y = 50;
    // collide ball <-> paddles
    for(int pi=0;pi<2;pi++){
        float px = pi?ah_p2x:ah_p1x; float py = pi?ah_p2y:ah_p1y;
        float dx=ah_bx-px, dy=ah_by-py;
        if(dx*dx+dy*dy<900){ float m=sqrtf(dx*dx+dy*dy)+0.01f; ah_vx=dx/m*5; ah_vy=dy/m*5; }
    }
    lv_canvas_fill_bg(g_game_canvas, lv_color_hex(0x080820), LV_OPA_COVER);
    game_canvas_rect(g_game_canvas, 0, GH/2-1, GW, 2, 0x444444, 1);
    game_canvas_circle(g_game_canvas, GW/2, GH/2, 50, 0);
    game_canvas_circle(g_game_canvas, (int)ah_p1x, (int)ah_p1y, 22, COL_CYAN);
    game_canvas_circle(g_game_canvas, (int)ah_p2x, (int)ah_p2y, 22, COL_MAGENTA);
    game_canvas_circle(g_game_canvas, (int)ah_bx, (int)ah_by, 8, COL_WHITE);
    game_set_score("AI %d  ::  %d YOU", ah_s2, ah_s1);
}
static void ah_drag(lv_event_t*){
    lv_point_t p; lv_indev_get_point(lv_indev_get_act(),&p);
    lv_area_t a; lv_obj_get_coords(g_game_canvas,&a);
    int lx=p.x-a.x1, ly=p.y-a.y1;
    if(ly > GH/2){ ah_p1x=lx; ah_p1y=ly; }
}
static void open_airhockey(){
    lv_obj_t *s = game_screen_base("AIR HOCKEY", COL_CYAN);
    lv_obj_t *cv = game_add_canvas(s, GW, GH);
    ah_bx=GW/2; ah_by=GH/2; ah_vx=ah_vy=0;
    ah_p1x=GW/2; ah_p1y=GH-60; ah_p2x=GW/2; ah_p2y=50; ah_s1=ah_s2=0;
    lv_obj_add_event_cb(cv, ah_drag, LV_EVENT_PRESSING, NULL);
    g_game_timer = lv_timer_create(ah_step, 24, NULL);
    game_add_exit(s, COL_CYAN);
    open_app_screen(s);
}

// ---- 28. PLATFORMER (tiny endless) ---------------------------------
static float pl_x, pl_y, pl_vy; static int pl_ground=1, pl_score=0, pl_dead=0;
static float pl_plat_x[5], pl_plat_y[5];
static void pl_step(lv_timer_t*){
    if(!pl_dead){
        pl_vy += 0.5f; pl_y += pl_vy;
        bool onP=false;
        for(int i=0;i<5;i++){
            pl_plat_x[i] -= 2.0f;
            if(pl_plat_x[i]<-80){ pl_plat_x[i]=GW+random(80); pl_plat_y[i]=GH-60-random(120); }
            if(pl_x+12 > pl_plat_x[i] && pl_x < pl_plat_x[i]+80 &&
               pl_y+22 > pl_plat_y[i] && pl_y+22 < pl_plat_y[i]+10 && pl_vy>=0){
                pl_y = pl_plat_y[i]-22; pl_vy=0; onP=true;
            }
        }
        pl_ground = onP?1:0;
        if(pl_y > GH){ pl_dead=1; }
        pl_score++;
    }
    lv_canvas_fill_bg(g_game_canvas, lv_color_hex(0x050505), LV_OPA_COVER);
    for(int i=0;i<5;i++) game_canvas_rect(g_game_canvas, (int)pl_plat_x[i],(int)pl_plat_y[i],80,8, COL_GREEN, 3);
    game_canvas_rect(g_game_canvas, (int)pl_x, (int)pl_y, 22,22, pl_dead?COL_RED:COL_CYAN, 4);
    game_set_score("RUN %d%s", pl_score, pl_dead?"  TAP RESTART":"");
}
static void pl_tap(lv_event_t*){
    if(pl_dead){ pl_dead=0; pl_score=0; pl_y=GH/2; pl_vy=0; return; }
    if(pl_ground){ pl_vy=-9.5f; pl_ground=0; }
}
static void open_platformer(){
    lv_obj_t *s = game_screen_base("PLATFORMER", COL_GREEN);
    lv_obj_t *cv = game_add_canvas(s, GW, GH);
    pl_x=60; pl_y=GH/2; pl_vy=0; pl_score=0; pl_dead=0;
    for(int i=0;i<5;i++){pl_plat_x[i]=80+i*100; pl_plat_y[i]=GH-60-random(120);}
    lv_obj_add_event_cb(cv, pl_tap, LV_EVENT_CLICKED, NULL);
    g_game_timer = lv_timer_create(pl_step, 28, NULL);
    game_add_exit(s, COL_GREEN);
    open_app_screen(s);
}

// ---- 29. TOWER STACK -----------------------------------------------
static int tw_w[40], tw_x[40], tw_n=1, tw_dir=1; static float tw_cur_x=0;
static int tw_dead=0;
static void tw_step(lv_timer_t*){
    if(!tw_dead){
        tw_cur_x += tw_dir*3.5f;
        if(tw_cur_x < 0){ tw_cur_x=0; tw_dir=1; }
        if(tw_cur_x + tw_w[tw_n] > GW){ tw_cur_x=GW-tw_w[tw_n]; tw_dir=-1; }
    }
    lv_canvas_fill_bg(g_game_canvas, lv_color_hex(0x050505), LV_OPA_COVER);
    for(int i=0;i<tw_n;i++){
        int y = GH - 20 - i*12;
        if(y<0) break;
        uint32_t c = (i%2)?COL_CYAN:COL_MAGENTA;
        game_canvas_rect(g_game_canvas, tw_x[i], y, tw_w[i], 10, c, 2);
    }
    if(!tw_dead){
        int y = GH - 20 - tw_n*12;
        game_canvas_rect(g_game_canvas, (int)tw_cur_x, y, tw_w[tw_n], 10, COL_YELLOW, 2);
    }
    game_set_score("HEIGHT %d%s", tw_n-1, tw_dead?"  TAP RESTART":"");
}
static void tw_tap(lv_event_t*){
    if(tw_dead){
        tw_n=1; tw_dead=0; tw_w[0]=100; tw_x[0]=(GW-100)/2; tw_w[1]=100; tw_cur_x=0; tw_dir=1; return;
    }
    int left = max(tw_x[tw_n-1], (int)tw_cur_x);
    int right = min(tw_x[tw_n-1]+tw_w[tw_n-1], (int)tw_cur_x+tw_w[tw_n]);
    int nw = right-left;
    if(nw<6){ tw_dead=1; return; }
    tw_x[tw_n]=left; tw_w[tw_n]=nw; tw_n++;
    if(tw_n>=40){ tw_dead=1; return; }
    tw_w[tw_n]=nw; tw_cur_x=0; tw_dir=1;
}
static void open_tower(){
    lv_obj_t *s = game_screen_base("TOWER STACK", COL_YELLOW);
    lv_obj_t *cv = game_add_canvas(s, GW, GH);
    tw_n=1; tw_w[0]=100; tw_x[0]=(GW-100)/2; tw_w[1]=100; tw_cur_x=0; tw_dir=1; tw_dead=0;
    lv_obj_add_event_cb(cv, tw_tap, LV_EVENT_CLICKED, NULL);
    g_game_timer = lv_timer_create(tw_step, 28, NULL);
    game_add_exit(s, COL_YELLOW);
    open_app_screen(s);
}

// ---- 30. CROSSY ROAD lite ------------------------------------------
static int cr_px, cr_py; static int cr_score=0, cr_dead=0;
static float cr_lane[8]; static int cr_dir[8];
static void cr_step(lv_timer_t*){
    if(!cr_dead){
        for(int i=0;i<8;i++){
            cr_lane[i] += cr_dir[i]*(2.0f+(i%3));
            if(cr_lane[i]<-80) cr_lane[i]=GW;
            if(cr_lane[i]>GW) cr_lane[i]=-80;
        }
        // collision
        int laneY = 40+cr_py*36;
        for(int i=0;i<8;i++){
            int y=40+i*36;
            if(i%2 && abs(laneY-y)<14){
                for(int k=0;k<3;k++){ int x=(int)(cr_lane[i]+k*150)%GW; if(x<0)x+=GW;
                    if(cr_px*60+10 < x+60 && cr_px*60+50 > x) cr_dead=1; }
            }
        }
    }
    lv_canvas_fill_bg(g_game_canvas, lv_color_hex(0x081008), LV_OPA_COVER);
    for(int i=0;i<8;i++){
        int y=40+i*36;
        uint32_t bg = (i%2)?0x222222:0x0E2A0E;
        game_canvas_rect(g_game_canvas, 0,y-14,GW,28, bg, 0);
        if(i%2) for(int k=0;k<3;k++){ int x=(int)(cr_lane[i]+k*150)%GW; if(x<0)x+=GW;
            game_canvas_rect(g_game_canvas, x, y-10, 60,20, COL_YELLOW, 4); }
    }
    int px = cr_px*60+15, py = 40+cr_py*36-12;
    game_canvas_rect(g_game_canvas, px, py, 30, 24, cr_dead?COL_RED:COL_CYAN, 4);
    game_set_score("HOPS %d%s", cr_score, cr_dead?"  tap to retry":"");
}
static void cr_tap(lv_event_t*){
    if(cr_dead){ cr_dead=0; cr_score=0; cr_py=0; cr_px=2; return; }
    cr_py++; cr_score++;
    if(cr_py>=8){ cr_py=0; }
}
static void cr_swipe(lv_event_t*){
    lv_dir_t d=lv_indev_get_gesture_dir(lv_indev_get_act());
    if(d==LV_DIR_LEFT && cr_px>0) cr_px--;
    if(d==LV_DIR_RIGHT && cr_px<5) cr_px++;
}
static void open_crossy(){
    lv_obj_t *s = game_screen_base("CROSSY", COL_CYAN);
    lv_obj_t *cv = game_add_canvas(s, GW, GH);
    cr_px=2; cr_py=0; cr_score=0; cr_dead=0;
    for(int i=0;i<8;i++){cr_lane[i]=random(GW); cr_dir[i]=(i%2)?1:-1;}
    lv_obj_add_event_cb(cv, cr_tap, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(s, cr_swipe, LV_EVENT_GESTURE, NULL);
    g_game_timer = lv_timer_create(cr_step, 30, NULL);
    game_add_exit(s, COL_CYAN);
    open_app_screen(s);
}

// ==================== END games_pack3.h ====================


static const GameEntry GAMES[] = {
    // built-ins
    { "SNAKE",        "swipe to steer",        'S', COL_GREEN,   open_snake,        0 },
    { "PONG",         "drag · spin · speed",   'P', COL_CYAN,    open_pong,         0 },
    // easy pack
    { "TETRIS",       "tap rotate · swipe",    'T', COL_CYAN,    open_tetris,       1 },
    { "BREAKOUT",     "drag · break bricks",   'B', COL_MAGENTA, open_breakout,     1 },
    { "2048",         "swipe to merge",        '2', COL_YELLOW,  open_2048,         1 },
    { "FLAPPY",       "tap to flap",           'F', COL_CYAN,    open_flappy,       1 },
    { "TIC-TAC-TOE",  "vs minimax AI",         'X', COL_MAGENTA, open_tictactoe,    1 },
    { "WHACK-A-MOLE", "tap the lit cells",     'W', COL_GREEN,   open_whack,        1 },
    { "SIMON",        "repeat the sequence",   '~', COL_PURPLE,  open_simon,        1 },
    { "DINO RUNNER",  "tap to jump",           'D', COL_CYAN,    open_dino,         1 },
    { "PONG 2P",      "two paddles, one screen",'P',COL_MAGENTA, open_pong2p,       1 },
    { "REACTION",     "tap on cyan flash",     'R', COL_CYAN,    open_reaction,     1 },
    // medium pack
    { "MINESWEEPER",  "tap · long-press flag", 'M', COL_RED,     open_minesweeper,  2 },
    { "MEMORY",       "match the pairs",       'm', COL_MAGENTA, open_memory,       2 },
    { "LIGHTS OUT",   "toggle to clear",       'L', COL_YELLOW,  open_lightsout,    2 },
    { "CONNECT 4",    "drop discs · vs AI",    'C', COL_CYAN,    open_connect4,     2 },
    { "15-PUZZLE",    "slide tiles to order",  '#', COL_CYAN,    open_fifteen,      2 },
    { "INVADERS",     "drag · tap to shoot",   'I', COL_GREEN,   open_invaders,     2 },
    { "ASTEROIDS",    "rotate · thrust · fire",'A', COL_CYAN,    open_asteroids,    2 },
    { "DOODLE DODGE", "tilt-tap · climb",      'd', COL_MAGENTA, open_doodle,       2 },
    { "FROGGER",      "swipe across lanes",    'F', COL_GREEN,   open_frogger,      2 },
    { "MAZE",         "swipe to the exit",     'Z', COL_CYAN,    open_maze,         2 },
    // showpieces
    { "TRON",         "leave a trail · survive",'/', COL_CYAN,   open_tron,         3 },
    { "MATCH-3",      "swap · chain combos",   '*', COL_MAGENTA, open_match3,       3 },
    { "PINBALL",      "two flippers · physics",'O', COL_YELLOW,  open_pinball,      3 },
    { "WORDLE",       "guess in 6 tries",      'W', COL_GREEN,   open_wordle,       3 },
    { "SUDOKU",       "tap · keypad",          '9', COL_CYAN,    open_sudoku,       3 },
    { "CHECKERS",     "vs AI · draughts",      'K', COL_MAGENTA, open_checkers,     3 },
    { "AIR HOCKEY",   "drag · slam puck",      'H', COL_CYAN,    open_airhockey,    3 },
    { "PLATFORMER",   "tap jump · swipe",      '^', COL_GREEN,   open_platformer,   3 },
    { "TOWER STACK",  "time your taps",        'T', COL_YELLOW,  open_tower,        3 },
    { "CROSSY",       "tap to hop forward",    '>', COL_CYAN,    open_crossy,       3 },
};
static const int GAMES_N = sizeof(GAMES)/sizeof(GAMES[0]);

static const char* CAT_NAMES[] = {
    "BUILT-IN",
    "QUICK PLAY",
    "PUZZLES & ARCADE",
    "SHOWPIECES"
};

// =====================================================================
//   Wallpaper, OTA, top-strip  (unchanged from v1)
// =====================================================================
static uint8_t *g_wp_buf=nullptr; static lv_img_dsc_t g_wp_dsc;
static String pick_wallpaper(){
    Preferences p; p.begin("face",true);
    int idx=p.getInt("wp_idx",0); p.end();
    File dir=SD_MMC.open("/WATCH/PHOTOS"); if(!dir) return "";
    File f=dir.openNextFile(); int i=0; String found="";
    while(f){
        String n=f.name(); String lc=n; lc.toLowerCase();
        if(!f.isDirectory() && (lc.endsWith(".raw")||lc.endsWith(".bin"))){
            if(i==idx){ found=String("/WATCH/PHOTOS/")+f.name(); break; }
            i++;
        }
        f=dir.openNextFile();
    }
    return found;
}
static void make_wallpaper_bg(lv_obj_t *scr){
    String p=pick_wallpaper(); if(p.length()==0) return;
    File f=SD_MMC.open(p.c_str(),FILE_READ); if(!f) return;
    size_t sz=LCD_W*LCD_H*2;
    if(!g_wp_buf) g_wp_buf=(uint8_t*)heap_caps_malloc(sz,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
    if(!g_wp_buf) g_wp_buf=(uint8_t*)heap_caps_malloc(sz,MALLOC_CAP_8BIT);
    if(!g_wp_buf){ f.close(); return; }
    f.read(g_wp_buf,sz); f.close();
    g_wp_dsc.header.cf=LV_IMG_CF_TRUE_COLOR;
    g_wp_dsc.header.always_zero=0; g_wp_dsc.header.reserved=0;
    g_wp_dsc.header.w=LCD_W; g_wp_dsc.header.h=LCD_H;
    g_wp_dsc.data_size=sz; g_wp_dsc.data=g_wp_buf;
    lv_obj_t *img=lv_img_create(scr);
    lv_img_set_src(img,&g_wp_dsc);
    lv_obj_align(img,LV_ALIGN_CENTER,0,0); lv_obj_move_background(img);
}

static void flash_launcher_bin(){
    File f=SD_MMC.open("/WATCH/bin/launcheros.bin",FILE_READ);
    if(!f || f.size()<10000){ if(f) f.close(); return_to_launcher(); return; }
    lv_obj_t *ov=lv_obj_create(lv_scr_act()); lv_obj_remove_style_all(ov);
    lv_obj_set_size(ov,LCD_W,LCD_H); lv_obj_align(ov,LV_ALIGN_CENTER,0,0);
    lv_obj_set_style_bg_color(ov,lv_color_hex(COL_BG),0);
    lv_obj_set_style_bg_opa(ov,LV_OPA_COVER,0);
    lv_obj_t *t=lv_label_create(ov); lv_label_set_text(t,"LOADING LAUNCHEROS");
    lv_obj_set_style_text_color(t,lv_color_hex(COL_CYAN),0);
    lv_obj_set_style_text_font(t,&lv_font_montserrat_18,0);
    lv_obj_set_style_text_letter_space(t,3,0);
    lv_obj_align(t,LV_ALIGN_CENTER,0,-40);
    lv_obj_t *bar=lv_bar_create(ov); lv_obj_set_size(bar,280,4);
    lv_obj_align(bar,LV_ALIGN_CENTER,0,20);
    lv_obj_set_style_bg_color(bar,lv_color_hex(COL_PANEL2),LV_PART_MAIN);
    lv_obj_set_style_bg_color(bar,lv_color_hex(COL_CYAN),LV_PART_INDICATOR);
    lv_obj_t *pct=lv_label_create(ov); lv_label_set_text(pct,"0 %");
    lv_obj_set_style_text_color(pct,lv_color_hex(COL_WHITE),0);
    lv_obj_set_style_text_font(pct,&lv_font_montserrat_22,0);
    lv_obj_align(pct,LV_ALIGN_CENTER,0,55);
    lv_timer_handler();
    size_t sz=f.size();
    if(!Update.begin(sz)){ f.close(); return_to_launcher(); return; }
    uint8_t buf[2048]; size_t total=0;
    while(f.available()){
        int n=f.read(buf,sizeof(buf)); if(n<=0) break;
        Update.write(buf,n); total+=n;
        int pp=(int)((total*100)/sz);
        lv_bar_set_value(bar,pp,LV_ANIM_OFF);
        char b[8]; snprintf(b,8,"%d %%",pp); lv_label_set_text(pct,b);
        if((total & 0x3FFF)==0) lv_timer_handler();
    }
    f.close();
    if(Update.end(true)){ delay(300); esp_restart(); } else return_to_launcher();
}

// ---- WiFi --------------------------------------------------------------
struct WifiCred{ String ssid,pass; };
static WifiCred g_creds[6]; static int g_creds_n=0; static int g_gmt=0;
static bool g_wifi_synced=false;
static void load_wifi_config(){
    g_creds_n=0;
    File f=SD_MMC.open("/WATCH/wifi/config.txt",FILE_READ);
    if(!f){ g_creds[g_creds_n++]={HC_WIFI_SSID,HC_WIFI_PASS}; return; }
    while(f.available()){
        String ln=f.readStringUntil('\n'); ln.trim();
        if(ln.length()==0||ln.startsWith("#")) continue;
        int eq=ln.indexOf('='); if(eq<0) continue;
        String k=ln.substring(0,eq); k.trim();
        String v=ln.substring(eq+1); v.trim();
        for(int i=1;i<=5;i++){
            if(k=="WIFI"+String(i)+"_SSID"){ if(g_creds_n<5){g_creds[g_creds_n].ssid=v;} }
            if(k=="WIFI"+String(i)+"_PASS"){ if(g_creds_n<5){g_creds[g_creds_n].pass=v; g_creds_n++;} }
        }
        if(k=="GMT_OFFSET") g_gmt=v.toInt();
    }
    f.close();
    g_creds[g_creds_n++]={HC_WIFI_SSID,HC_WIFI_PASS};
}
static void wifi_quick_sync(){
    if(g_creds_n==0) return;
    WiFi.mode(WIFI_STA);
    for(int i=0;i<g_creds_n;i++){
        WiFi.begin(g_creds[i].ssid.c_str(),g_creds[i].pass.c_str());
        uint32_t t=millis();
        while(WiFi.status()!=WL_CONNECTED && millis()-t<4000) delay(100);
        if(WiFi.status()==WL_CONNECTED){
            configTime(g_gmt*3600,0,"pool.ntp.org","time.google.com");
            struct tm tm; getLocalTime(&tm,3000);
            g_wifi_synced=true; break;
        }
    }
    WiFi.disconnect(true); WiFi.mode(WIFI_OFF);
}

static void sd_ensure_dirs(){
    if(!SD_MMC.exists("/WATCH")) SD_MMC.mkdir("/WATCH");
    if(!SD_MMC.exists("/WATCH/games")) SD_MMC.mkdir("/WATCH/games");
    if(!SD_MMC.exists("/WATCH/games/saves")) SD_MMC.mkdir("/WATCH/games/saves");
}

static void hw_boot(){
    Serial.begin(115200);
    pinMode(BOOT_BTN, INPUT_PULLUP);
    pinMode(PWR_BTN,  INPUT_PULLUP);
    btStop();

    // (1) Display first - cheap, no I2C, gives us a visual heartbeat
    //     even if I2C bring-up later goes wrong.
    if(!gfx->begin()) Serial.println("gfx begin fail");
    gfx->fillScreen(0x0000);

    // (2) Bring the I2C bus up cleanly (9-clock unstick in case the
    //     previous app left a slave mid-transaction).
    pinMode(IIC_SCL, OUTPUT);
    for(int i = 0; i < 9; i++){
        digitalWrite(IIC_SCL, LOW);  delayMicroseconds(5);
        digitalWrite(IIC_SCL, HIGH); delayMicroseconds(5);
    }
    Wire.begin(IIC_SDA, IIC_SCL);
    Wire.setClock(400000);

    // (3) AXP2101 FIRST.  The FT3168 touch IC is powered by an AXP LDO
    //     rail on the Waveshare 2.06 board.  Starting touch before this
    //     rail is on leaves the driver's internal state undefined and
    //     the first LVGL touch poll then faults with LoadProhibited
    //     (the boot loop / Guru Meditation we were chasing).
    if(g_pmu.begin(Wire, AXP2101_SLAVE_ADDRESS, IIC_SDA, IIC_SCL)){
        g_has_pmu = true;
        g_pmu.disableTSPinMeasure();
        g_pmu.enableBattVoltageMeasure();
        // Force the touch / sensor rails ON explicitly.  No-ops if the
        // launcher already had them on; critical after cold boot or
        // wake from deep sleep where they may be off.
        g_pmu.setALDO1Voltage(3300); g_pmu.enableALDO1();
        g_pmu.setBLDO1Voltage(3300); g_pmu.enableBLDO1();
        delay(40);   // rail settle before we pulse TP_RESET
    }

    // (4) Ping the touch controller before talking to it. If it NACKs
    //     (loose flex, dead rail, wrong board variant), DO NOT call
    //     ->begin() or any IIC_Read_Device_Value() — those crash with
    //     LoadProhibited inside the driver. Set g_has_touch=false and
    //     run "headless touch" (BOOT button still exits to launcher).
    bool ack = i2c_ping(FT3168_DEVICE_ADDRESS);
    if(!ack){ delay(120); ack = i2c_ping(FT3168_DEVICE_ADDRESS); }
    if(ack){
        // (4b) NOW reset and start the touch controller.
        pinMode(TP_RESET, OUTPUT);
        digitalWrite(TP_RESET, LOW);  delay(50);
        digitalWrite(TP_RESET, HIGH); delay(200);
        FT3168->begin();
        g_has_touch = true;
    } else {
        g_has_touch = false;
        Serial.println("[GameOS] FT3168 NACK -- skipping touch driver init.");
    }

    // (5) LVGL last - safe to poll touch from here on.
    lv_init();
    size_t px = LCD_W * 60;
    g_lvbuf = (lv_color_t*)heap_caps_malloc(px * sizeof(lv_color_t),
                MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if(!g_lvbuf)
        g_lvbuf = (lv_color_t*)heap_caps_malloc(px * sizeof(lv_color_t),
                    MALLOC_CAP_8BIT);
    lv_disp_draw_buf_init(&g_db, g_lvbuf, NULL, px);
    lv_disp_drv_init(&g_dd);
    g_dd.flush_cb = lv_flush; g_dd.draw_buf = &g_db;
    g_dd.hor_res  = LCD_W;    g_dd.ver_res  = LCD_H;
    lv_disp_drv_register(&g_dd);
    lv_indev_drv_init(&g_id);
    g_id.type = LV_INDEV_TYPE_POINTER;
    g_id.read_cb = lv_touch_gesture;
    lv_indev_drv_register(&g_id);

    SD_MMC.setPins(SDMMC_CLK, SDMMC_CMD, SDMMC_DATA);
    SD_MMC.begin("/sdcard", true);
    sd_ensure_dirs();
    load_wifi_config();
}

// =====================================================================
//   SPLASH  (tracked so we can free it after handing off to the watch-face,
//   otherwise its 125 dim-line objects stay in the LVGL heap forever.)
// =====================================================================
static lv_obj_t *g_splash_scr = nullptr;

static void splash_show(){
    lv_obj_t *s=lv_obj_create(NULL);
    g_splash_scr = s;
    lv_obj_set_style_bg_color(s,lv_color_hex(COL_BG),0);
    lv_obj_set_style_bg_opa(s,LV_OPA_COVER,0);
    lv_obj_clear_flag(s,LV_OBJ_FLAG_SCROLLABLE);
    // (Removed: a 125-iter loop that created an lv_obj per scanline as a
    //  "CRT lines" effect.  It ate ~25 KB of LVGL heap and was a major
    //  contributor to the OOM that took out boot.  The remaining widgets
    //  -- title / subtitle / progress bar -- are the splash visual.)
    lv_obj_t *t=lv_label_create(s); lv_label_set_text(t,"GAME//OS");
    lv_obj_set_style_text_color(t,lv_color_hex(COL_MAGENTA),0);
    lv_obj_set_style_text_font(t,&lv_font_montserrat_48,0);
    lv_obj_align(t,LV_ALIGN_CENTER,0,-30);
    lv_obj_t *t2=lv_label_create(s); lv_label_set_text(t2,"INITIALIZING ARCADE CORE");
    lv_obj_set_style_text_color(t2,lv_color_hex(COL_CYAN),0);
    lv_obj_set_style_text_font(t2,&lv_font_montserrat_14,0);
    lv_obj_set_style_text_letter_space(t2,3,0);
    lv_obj_align(t2,LV_ALIGN_CENTER,0,20);
    lv_obj_t *bar=lv_bar_create(s); lv_obj_set_size(bar,260,4);
    lv_obj_align(bar,LV_ALIGN_CENTER,0,60);
    lv_obj_set_style_bg_color(bar,lv_color_hex(COL_PANEL2),LV_PART_MAIN);
    lv_obj_set_style_bg_color(bar,lv_color_hex(COL_MAGENTA),LV_PART_INDICATOR);
    lv_bar_set_range(bar,0,100); lv_bar_set_value(bar,0,LV_ANIM_OFF);
    lv_obj_t *gc=lv_label_create(s);
    char gcb[40]; snprintf(gcb,40,"%d GAMES READY", GAMES_N);
    lv_label_set_text(gc,gcb);
    lv_obj_set_style_text_color(gc,lv_color_hex(COL_TEXT_DIM),0);
    lv_obj_set_style_text_font(gc,&lv_font_montserrat_12,0);
    lv_obj_align(gc,LV_ALIGN_CENTER,0,90);
    lv_scr_load(s);
    const char *steps[]={"MOUNT SD","SCAN /games","LOAD SAVES","SYNC RTC","READY"};
    uint32_t boot_held = 0;
    for(int p=0;p<=100;p+=4){
        lv_bar_set_value(bar,p,LV_ANIM_OFF);
        if(p%20==0) lv_label_set_text(t2,steps[p/20>4?4:p/20]);
        lv_timer_handler(); delay(28);
        if(p==40) wifi_quick_sync();

        // Hardware safety exit during splash: hold BOOT 1.5 s -> launcher.
        // Without this, if anything later in setup() panics the user is
        // trapped in a boot loop because the loop()-level BOOT handler
        // never gets to run.
        if(digitalRead(BOOT_BTN) == LOW){
            if(!boot_held) boot_held = millis();
            else if(millis() - boot_held > 1500) return_to_launcher();
        } else boot_held = 0;
    }
}

// ---- Watch face ------------------------------------------------------
static lv_obj_t *wf_clk=nullptr,*wf_dt=nullptr; static lv_timer_t *wf_tk=nullptr;
static void wf_upd(lv_timer_t*){
    struct tm tm; if(!getLocalTime(&tm,5)) return;
    char t[8],d[24]; strftime(t,8,"%H:%M",&tm); strftime(d,24,"%a %d %b",&tm);
    if(wf_clk) lv_label_set_text(wf_clk,t);
    if(wf_dt)  lv_label_set_text(wf_dt,d);
}
static void build_screen_face(){
    lv_obj_t *s = lv_obj_create(NULL); screens[SCREEN_FACE] = s;
    lv_obj_set_style_bg_color(s,lv_color_hex(COL_BG),0);
    lv_obj_set_style_bg_opa(s,LV_OPA_COVER,0);
    lv_obj_clear_flag(s,LV_OBJ_FLAG_SCROLLABLE);
    make_wallpaper_bg(s);
    wf_clk = lv_label_create(s); lv_label_set_text(wf_clk,"--:--");
    lv_obj_set_style_text_color(wf_clk,lv_color_hex(COL_WHITE),0);
    lv_obj_set_style_text_font(wf_clk,&lv_font_montserrat_48,0);
    lv_obj_align(wf_clk,LV_ALIGN_CENTER,0,-30);
    wf_dt = lv_label_create(s); lv_label_set_text(wf_dt,"no sync");
    lv_obj_set_style_text_color(wf_dt,lv_color_hex(0xBBBBBB),0);
    lv_obj_set_style_text_font(wf_dt,&lv_font_montserrat_18,0);
    lv_obj_align(wf_dt,LV_ALIGN_CENTER,0,20);
    lv_obj_t *hint=lv_label_create(s);
    lv_label_set_text(hint,"swipe " LV_SYMBOL_RIGHT " for games");
    lv_obj_set_style_text_color(hint,lv_color_hex(COL_TEXT_DIM),0);
    lv_obj_set_style_text_font(hint,&lv_font_montserrat_12,0);
    lv_obj_set_style_text_letter_space(hint,2,0);
    lv_obj_align(hint,LV_ALIGN_BOTTOM_MID,0,-40);
    if(wf_tk) lv_timer_del(wf_tk);
    wf_tk = lv_timer_create(wf_upd, 1000, NULL);
    wf_upd(nullptr);
}

// =====================================================================
//   LAUNCHER  (registry-driven, categories, momentum scroll, page dot)
// =====================================================================
static lv_obj_t* g_apps_list = nullptr;
static lv_obj_t* g_apps_indicator = nullptr;
static lv_obj_t* g_apps_count_lbl = nullptr;

static void make_category_header(lv_obj_t* parent, const char* text, uint32_t accent){
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, LV_PCT(100), 26);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *bar = lv_obj_create(row); lv_obj_remove_style_all(bar);
    lv_obj_set_size(bar, 18, 2);
    lv_obj_align(bar, LV_ALIGN_LEFT_MID, 4, 0);
    lv_obj_set_style_bg_color(bar, lv_color_hex(accent),0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER,0);

    lv_obj_t *l = lv_label_create(row);
    lv_label_set_text(l, text);
    lv_obj_set_style_text_color(l, lv_color_hex(accent),0);
    lv_obj_set_style_text_font(l, &lv_font_montserrat_12,0);
    lv_obj_set_style_text_letter_space(l, 4, 0);
    lv_obj_align(l, LV_ALIGN_LEFT_MID, 30, 0);
}

static void make_game_card(lv_obj_t* parent, const GameEntry* g){
    lv_obj_t *card = lv_obj_create(parent);
    lv_obj_remove_style_all(card);
    lv_obj_set_size(card, LV_PCT(100), 78);
    lv_obj_set_style_bg_color(card, lv_color_hex(COL_PANEL),0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER,0);
    lv_obj_set_style_radius(card, 18,0);
    lv_obj_set_style_border_color(card, lv_color_hex(COL_PANEL2),0);
    lv_obj_set_style_border_width(card, 1,0);
    lv_obj_set_style_pad_all(card, 14, 0);
    lv_obj_set_style_bg_color(card, lv_color_hex(0x1A1A1A), LV_STATE_PRESSED);
    lv_obj_set_style_transform_zoom(card, 252, LV_STATE_PRESSED);
    lv_obj_set_style_shadow_color(card, lv_color_hex(g->accent), LV_STATE_PRESSED);
    lv_obj_set_style_shadow_opa(card, 80, LV_STATE_PRESSED);
    lv_obj_set_style_shadow_width(card, 24, LV_STATE_PRESSED);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    // accent bar
    lv_obj_t *bar = lv_obj_create(card); lv_obj_remove_style_all(bar);
    lv_obj_set_size(bar, 4, 44); lv_obj_align(bar, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_set_style_radius(bar,2,0);
    lv_obj_set_style_bg_color(bar, lv_color_hex(g->accent),0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER,0);

    // icon disc
    lv_obj_t *dot = lv_obj_create(card); lv_obj_remove_style_all(dot);
    lv_obj_set_size(dot, 40, 40); lv_obj_align(dot, LV_ALIGN_LEFT_MID, 18, 0);
    lv_obj_set_style_radius(dot, 20,0);
    lv_obj_set_style_bg_color(dot, lv_color_hex(g->accent),0);
    lv_obj_set_style_bg_opa(dot, 50,0);
    lv_obj_set_style_border_color(dot, lv_color_hex(g->accent),0);
    lv_obj_set_style_border_width(dot,1,0);
    lv_obj_t *dt = lv_label_create(dot);
    char icbuf[2] = { g->icon, 0 };
    lv_label_set_text(dt, icbuf);
    lv_obj_set_style_text_color(dt, lv_color_hex(g->accent),0);
    lv_obj_set_style_text_font(dt, &lv_font_montserrat_22,0);
    lv_obj_center(dt);

    // name
    lv_obj_t *name = lv_label_create(card);
    lv_label_set_text(name, g->name);
    lv_obj_set_style_text_color(name, lv_color_hex(COL_WHITE),0);
    lv_obj_set_style_text_font(name, &lv_font_montserrat_18,0);
    lv_obj_set_style_text_letter_space(name,2,0);
    lv_obj_align(name, LV_ALIGN_LEFT_MID, 68, -10);

    // desc
    lv_obj_t *desc = lv_label_create(card);
    lv_label_set_text(desc, g->desc);
    lv_obj_set_style_text_color(desc, lv_color_hex(COL_TEXT_DIM),0);
    lv_obj_set_style_text_font(desc, &lv_font_montserrat_12,0);
    lv_obj_set_style_text_letter_space(desc,1,0);
    lv_obj_align(desc, LV_ALIGN_LEFT_MID, 68, 14);

    // chevron
    lv_obj_t *chev = lv_label_create(card);
    lv_label_set_text(chev, LV_SYMBOL_RIGHT);
    lv_obj_set_style_text_color(chev, lv_color_hex(0x333333),0);
    lv_obj_set_style_text_font(chev, &lv_font_montserrat_22,0);
    lv_obj_align(chev, LV_ALIGN_RIGHT_MID, -8, 0);

    lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(card, [](lv_event_t* e){
        if (millis() - lastSwipeMs < CLICK_AFTER_SWIPE_MS) return;
        const GameEntry* gg = (const GameEntry*)lv_event_get_user_data(e);
        if(gg && gg->open) gg->open();
    }, LV_EVENT_CLICKED, (void*)g);
}

static void build_screen_apps(){
    lv_obj_t *s = lv_obj_create(NULL); screens[SCREEN_APPS] = s;
    lv_obj_set_style_bg_color(s,lv_color_hex(COL_BG),0);
    lv_obj_set_style_bg_opa(s,LV_OPA_COVER,0);
    lv_obj_clear_flag(s,LV_OBJ_FLAG_SCROLLABLE);

    // title
    lv_obj_t *title=lv_label_create(s); lv_label_set_text(title,"GAME//OS");
    lv_obj_set_style_text_color(title,lv_color_hex(COL_CYAN),0);
    lv_obj_set_style_text_font(title,&lv_font_montserrat_22,0);
    lv_obj_set_style_text_letter_space(title,4,0);
    lv_obj_align(title,LV_ALIGN_TOP_LEFT,24,22);

    // game count badge
    g_apps_count_lbl = lv_label_create(s);
    char gcb[24]; snprintf(gcb, 24, "%d GAMES", GAMES_N);
    lv_label_set_text(g_apps_count_lbl, gcb);
    lv_obj_set_style_text_color(g_apps_count_lbl, lv_color_hex(COL_MAGENTA),0);
    lv_obj_set_style_text_font(g_apps_count_lbl, &lv_font_montserrat_12,0);
    lv_obj_set_style_text_letter_space(g_apps_count_lbl,2,0);
    lv_obj_align(g_apps_count_lbl, LV_ALIGN_TOP_RIGHT, -24, 28);

    // nav hint
    lv_obj_t *hint=lv_label_create(s);
    lv_label_set_text(hint, LV_SYMBOL_LEFT "  watch face   ·   battery  " LV_SYMBOL_RIGHT);
    lv_obj_set_style_text_color(hint,lv_color_hex(0x444444),0);
    lv_obj_set_style_text_font(hint,&lv_font_montserrat_12,0);
    lv_obj_align(hint,LV_ALIGN_TOP_MID,0,54);

    // scrollable list with momentum
    lv_obj_t *list = lv_obj_create(s); g_apps_list = list;
    lv_obj_remove_style_all(list);
    lv_obj_set_size(list, LCD_W - 40, LCD_H - 110);
    lv_obj_align(list, LV_ALIGN_TOP_MID, 0, 80);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(list, 10, 0);
    lv_obj_set_style_pad_hor(list, 4, 0);
    lv_obj_set_style_pad_top(list, 4, 0);
    lv_obj_set_style_pad_bottom(list, 60, 0);
    lv_obj_set_scrollbar_mode(list, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_set_style_bg_color(list, lv_color_hex(COL_CYAN), LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_opa(list, 120, LV_PART_SCROLLBAR);
    lv_obj_set_style_width(list, 3, LV_PART_SCROLLBAR);
    lv_obj_set_scroll_dir(list, LV_DIR_VER);
    lv_obj_set_scroll_snap_y(list, LV_SCROLL_SNAP_NONE);

    // Build sectioned list: category header + its cards
    int cur_cat = -1;
    for(int i=0;i<GAMES_N;i++){
        if((int)GAMES[i].cat != cur_cat){
            cur_cat = GAMES[i].cat;
            uint32_t catColor =
                cur_cat==0?COL_GREEN:
                cur_cat==1?COL_CYAN:
                cur_cat==2?COL_YELLOW: COL_MAGENTA;
            make_category_header(list, CAT_NAMES[cur_cat], catColor);
        }
        make_game_card(list, &GAMES[i]);
    }

    // bottom fade overlay
    lv_obj_t *fade = lv_obj_create(s); lv_obj_remove_style_all(fade);
    lv_obj_set_size(fade, LCD_W, 36);
    lv_obj_align(fade, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_color(fade, lv_color_hex(COL_BG),0);
    lv_obj_set_style_bg_opa(fade, LV_OPA_70,0);
    lv_obj_clear_flag(fade, LV_OBJ_FLAG_CLICKABLE);

    g_apps_built = true;
}

// =====================================================================
//   Battery screen (unchanged from v1)
// =====================================================================
static lv_obj_t *batt_arc=nullptr,*batt_pct_lbl=nullptr,*batt_state=nullptr,*batt_remain=nullptr,*batt_volt=nullptr;
static int batt_remain_min_estimate(int pct){ if(pct<=0) return 0; return (pct*360)/100; }
static void update_battery_screen(){
    if(!batt_arc||!g_has_pmu) return;
    int pct=g_pmu.getBatteryPercent(); if(pct<0)pct=0; if(pct>100)pct=100;
    lv_arc_set_value(batt_arc,pct);
    char b[16]; snprintf(b,16,"%d%%",pct); lv_label_set_text(batt_pct_lbl,b);
    uint32_t col = pct<=15?COL_RED : pct<=30?COL_ORANGE : COL_GREEN;
    lv_obj_set_style_arc_color(batt_arc,lv_color_hex(col),LV_PART_INDICATOR);
    lv_obj_set_style_text_color(batt_pct_lbl,lv_color_hex(col),0);
    if(g_pmu.isCharging()){
        lv_label_set_text(batt_state,LV_SYMBOL_CHARGE "  charging");
        lv_obj_set_style_text_color(batt_state,lv_color_hex(COL_CYAN),0);
        lv_label_set_text(batt_remain,"");
    } else {
        lv_label_set_text(batt_state,"on battery");
        lv_obj_set_style_text_color(batt_state,lv_color_hex(0xBBBBBB),0);
        int mins=batt_remain_min_estimate(pct);
        char r[48]; int h=mins/60,m=mins%60;
        if(h>0) snprintf(r,48,"~ %d h %02d min remaining",h,m);
        else    snprintf(r,48,"~ %d min remaining",m);
        lv_label_set_text(batt_remain,r);
    }
    float mv=g_pmu.getBattVoltage();
    char v[32]; snprintf(v,32,"%.2f V",mv/1000.0f); lv_label_set_text(batt_volt,v);
}
static void build_screen_battery(){
    lv_obj_t *s=lv_obj_create(NULL); screens[SCREEN_BATTERY]=s;
    lv_obj_set_style_bg_color(s,lv_color_hex(COL_BG),0);
    lv_obj_set_style_bg_opa(s,LV_OPA_COVER,0);
    lv_obj_clear_flag(s,LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *title=lv_label_create(s); lv_label_set_text(title,"Battery");
    lv_obj_set_style_text_color(title,lv_color_hex(COL_WHITE),0);
    lv_obj_set_style_text_font(title,&lv_font_montserrat_22,0);
    lv_obj_align(title,LV_ALIGN_TOP_LEFT,24,22);
    lv_obj_t *sub=lv_label_create(s); lv_label_set_text(sub,"AXP2101");
    lv_obj_set_style_text_color(sub,lv_color_hex(COL_CYAN),0);
    lv_obj_set_style_text_font(sub,&lv_font_montserrat_14,0);
    lv_obj_align(sub,LV_ALIGN_TOP_LEFT,24,50);
    batt_arc=lv_arc_create(s);
    lv_obj_set_size(batt_arc,280,280); lv_obj_align(batt_arc,LV_ALIGN_CENTER,0,-10);
    lv_arc_set_rotation(batt_arc,135); lv_arc_set_bg_angles(batt_arc,0,270);
    lv_arc_set_range(batt_arc,0,100); lv_arc_set_value(batt_arc,0);
    lv_obj_remove_style(batt_arc,NULL,LV_PART_KNOB);
    lv_obj_clear_flag(batt_arc,LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_arc_color(batt_arc,lv_color_hex(COL_PANEL2),LV_PART_MAIN);
    lv_obj_set_style_arc_width(batt_arc,14,LV_PART_MAIN);
    lv_obj_set_style_arc_color(batt_arc,lv_color_hex(COL_GREEN),LV_PART_INDICATOR);
    lv_obj_set_style_arc_width(batt_arc,14,LV_PART_INDICATOR);
    batt_pct_lbl=lv_label_create(s); lv_label_set_text(batt_pct_lbl,"--%");
    lv_obj_set_style_text_color(batt_pct_lbl,lv_color_hex(COL_WHITE),0);
    lv_obj_set_style_text_font(batt_pct_lbl,&lv_font_montserrat_48,0);
    lv_obj_align(batt_pct_lbl,LV_ALIGN_CENTER,0,-10);
    batt_state=lv_label_create(s); lv_label_set_text(batt_state,"on battery");
    lv_obj_set_style_text_color(batt_state,lv_color_hex(0xBBBBBB),0);
    lv_obj_set_style_text_font(batt_state,&lv_font_montserrat_18,0);
    lv_obj_align(batt_state,LV_ALIGN_CENTER,0,90);
    batt_remain=lv_label_create(s); lv_label_set_text(batt_remain,"");
    lv_obj_set_style_text_color(batt_remain,lv_color_hex(COL_WHITE),0);
    lv_obj_set_style_text_font(batt_remain,&lv_font_montserrat_16,0);
    lv_obj_align(batt_remain,LV_ALIGN_CENTER,0,125);
    batt_volt=lv_label_create(s); lv_label_set_text(batt_volt,"");
    lv_obj_set_style_text_color(batt_volt,lv_color_hex(COL_TEXT_DIM),0);
    lv_obj_set_style_text_font(batt_volt,&lv_font_montserrat_14,0);
    lv_obj_align(batt_volt,LV_ALIGN_CENTER,0,160);
    lv_obj_t *sleepBtn=lv_btn_create(s); lv_obj_set_size(sleepBtn,175,44);
    lv_obj_align(sleepBtn,LV_ALIGN_BOTTOM_RIGHT,-15,-40);
    lv_obj_set_style_radius(sleepBtn,22,0);
    lv_obj_set_style_bg_color(sleepBtn,lv_color_hex(0x141414),0);
    lv_obj_set_style_border_color(sleepBtn,lv_color_hex(COL_CYAN),0);
    lv_obj_set_style_border_width(sleepBtn,1,0);
    lv_obj_add_event_cb(sleepBtn,[](lv_event_t*){
        if(millis()-lastSwipeMs<CLICK_AFTER_SWIPE_MS) return;
        static_cast<Arduino_CO5300*>(gfx)->setBrightness(0);
        esp_sleep_enable_ext1_wakeup((1ULL<<BOOT_BTN),ESP_EXT1_WAKEUP_ANY_LOW);
        esp_deep_sleep_start();
    },LV_EVENT_CLICKED,NULL);
    lv_obj_t *sl=lv_label_create(sleepBtn);
    lv_label_set_text(sl,LV_SYMBOL_POWER "  Sleep Now");
    lv_obj_set_style_text_color(sl,lv_color_hex(COL_CYAN),0);
    lv_obj_set_style_text_font(sl,&lv_font_montserrat_16,0); lv_obj_center(sl);
    lv_obj_t *launchBtn=lv_btn_create(s); lv_obj_set_size(launchBtn,175,44);
    lv_obj_align(launchBtn,LV_ALIGN_BOTTOM_LEFT,15,-40);
    lv_obj_set_style_radius(launchBtn,22,0);
    lv_obj_set_style_bg_color(launchBtn,lv_color_hex(0x141414),0);
    lv_obj_set_style_border_color(launchBtn,lv_color_hex(COL_GREEN),0);
    lv_obj_set_style_border_width(launchBtn,1,0);
    lv_obj_add_event_cb(launchBtn,[](lv_event_t*){
        if(millis()-lastSwipeMs<CLICK_AFTER_SWIPE_MS) return;
        return_to_launcher();
    },LV_EVENT_CLICKED,NULL);
    lv_obj_t *ll=lv_label_create(launchBtn);
    lv_label_set_text(ll,LV_SYMBOL_HOME "  Launcher");
    lv_obj_set_style_text_color(ll,lv_color_hex(COL_GREEN),0);
    lv_obj_set_style_text_font(ll,&lv_font_montserrat_16,0); lv_obj_center(ll);
}

// ---- Screen lifecycle ------------------------------------------------
static void lv_touch_gesture(lv_indev_drv_t *, lv_indev_data_t *d){
    // CRASH FIX: never touch the FT3168 driver if it failed to ACK at boot.
    if(!g_has_touch){ d->state = LV_INDEV_STATE_REL; return; }
    int32_t x = FT3168->IIC_Read_Device_Value(FT3168->Arduino_IIC_Touch::Value_Information::TOUCH_COORDINATE_X);
    int32_t y = FT3168->IIC_Read_Device_Value(FT3168->Arduino_IIC_Touch::Value_Information::TOUCH_COORDINATE_Y);
    int     f = FT3168->IIC_Read_Device_Value(FT3168->Arduino_IIC_Touch::Value_Information::TOUCH_FINGER_NUMBER);
    // Defensive: if the I2C bus NACKs (PMU/touch wiring fault) the driver
    // can return -1 / garbage.  Treat anything out-of-range as "no touch"
    // instead of feeding negative coords into LVGL hit-testing.
    bool valid = (x >= 0 && x < LCD_W && y >= 0 && y < LCD_H && f >= 0 && f <= 5);
    if(!valid){ f = 0; x = 0; y = 0; }
    bool pressed = (f > 0);
    static int zero_streak = 0;
    if(pressed) zero_streak=0;
    else if(touchActive){ zero_streak++; if(zero_streak<3){pressed=true; x=tc_x; y=tc_y;} }
    if(pressed){
        d->state=LV_INDEV_STATE_PR; d->point.x=x; d->point.y=y;
        if(!touchActive){ touchActive=true; ts_x=x; ts_y=y; ts_ms=millis(); }
        tc_x=x; tc_y=y;
    } else {
        d->state=LV_INDEV_STATE_REL;
        if(touchActive){
            touchActive=false;
            uint32_t dur=millis()-ts_ms;
            int32_t dx=tc_x-ts_x, dy=tc_y-ts_y;
            // Only screen-level swipe when on persistent screens (not inside a game)
            if(g_transient_screen==nullptr &&
               dur < SWIPE_MAX_DURATION_MS &&
               abs(dx) > SWIPE_THRESHOLD_PX && abs(dx) > abs(dy)){
                g_pending_swipe = (dx<0)?+1:-1;
            }
        }
    }
}

static void goto_screen(int s){
    while(s<0) s+=NUM_SCREENS;
    while(s>=NUM_SCREENS) s-=NUM_SCREENS;
    if(s==current_screen) return;
    current_screen=s; g_pending_swipe=0;
    if(g_transient_screen){ lv_obj_del_async(g_transient_screen); g_transient_screen=nullptr; }
    if(screens[s])
        lv_scr_load_anim(screens[s], LV_SCR_LOAD_ANIM_FADE_ON, SCREEN_SWITCH_ANIM_MS, 0, false);
}

static void open_app_screen(lv_obj_t *s){
    lv_obj_t *prev = g_transient_screen; g_transient_screen = s;
    lv_scr_load_anim(s, LV_SCR_LOAD_ANIM_FADE_ON, 220, 0, false);
    if(prev) lv_obj_del_async(prev);
}

static void open_launcher_screen(){
    if(!g_apps_built) build_screen_apps();
    goto_screen(SCREEN_APPS);
}

// =====================================================================
//   SETUP / LOOP
// =====================================================================
void setup(){
    hw_boot();
    splash_show();
    build_screen_face();
    build_screen_apps();
    build_screen_battery();
    current_screen = SCREEN_FACE;
    lv_scr_load_anim(screens[SCREEN_FACE], LV_SCR_LOAD_ANIM_FADE_ON, 260, 0, false);
    // free the splash screen now that we've handed off to the watch face.
    // (lv_scr_load_anim with auto_del=false leaves it parked in LVGL memory.)
    if(g_splash_scr){ lv_obj_del_async(g_splash_scr); g_splash_scr=nullptr; }
}

void loop(){
    lv_timer_handler();
    uint32_t now = millis();
    if(g_pending_swipe != 0 && g_transient_screen == nullptr){
        int ev = g_pending_swipe; g_pending_swipe = 0;
        lastSwipeMs = now; goto_screen(current_screen + ev);
    }
    static uint32_t tB = 0;
    if(now - tB > 1000){
        tB = now;
        if(current_screen == SCREEN_BATTERY) update_battery_screen();
    }
    static uint32_t btn_t = 0;
    if(digitalRead(BOOT_BTN) == LOW){
        if(!btn_t) btn_t = now;
        else if(now - btn_t > 1500) return_to_launcher();
    } else btn_t = 0;
    delay(5);
}
