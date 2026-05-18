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
#include "games_pack1.h"
#include "games_pack2.h"
#include "games_pack3.h"

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
    pinMode(BOOT_BTN,INPUT_PULLUP); pinMode(PWR_BTN,INPUT_PULLUP); btStop();
    if(!gfx->begin()) Serial.println("gfx begin fail");
    gfx->fillScreen(0x0000);
    pinMode(IIC_SCL,OUTPUT);
    for(int i=0;i<9;i++){digitalWrite(IIC_SCL,LOW);delayMicroseconds(5);digitalWrite(IIC_SCL,HIGH);delayMicroseconds(5);}
    Wire.begin(IIC_SDA,IIC_SCL);
    pinMode(TP_RESET,OUTPUT); digitalWrite(TP_RESET,LOW); delay(50); digitalWrite(TP_RESET,HIGH); delay(200);
    FT3168->begin();
    if(g_pmu.begin(Wire,AXP2101_SLAVE_ADDRESS,IIC_SDA,IIC_SCL)){g_has_pmu=true; g_pmu.disableTSPinMeasure(); g_pmu.enableBattVoltageMeasure();}
    lv_init();
    size_t px=LCD_W*60;
    g_lvbuf=(lv_color_t*)heap_caps_malloc(px*sizeof(lv_color_t),MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT);
    if(!g_lvbuf) g_lvbuf=(lv_color_t*)heap_caps_malloc(px*sizeof(lv_color_t),MALLOC_CAP_8BIT);
    lv_disp_draw_buf_init(&g_db,g_lvbuf,NULL,px);
    lv_disp_drv_init(&g_dd); g_dd.flush_cb=lv_flush; g_dd.draw_buf=&g_db; g_dd.hor_res=LCD_W; g_dd.ver_res=LCD_H;
    lv_disp_drv_register(&g_dd);
    lv_indev_drv_init(&g_id); g_id.type=LV_INDEV_TYPE_POINTER; g_id.read_cb=lv_touch_gesture;
    lv_indev_drv_register(&g_id);
    SD_MMC.setPins(SDMMC_CLK,SDMMC_CMD,SDMMC_DATA);
    SD_MMC.begin("/sdcard",true);
    sd_ensure_dirs();
    load_wifi_config();
}

static void splash_show(){
    lv_obj_t *s=lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s,lv_color_hex(COL_BG),0);
    lv_obj_set_style_bg_opa(s,LV_OPA_COVER,0);
    lv_obj_clear_flag(s,LV_OBJ_FLAG_SCROLLABLE);
    for(int y=0;y<LCD_H;y+=4){
        lv_obj_t *ln=lv_obj_create(s); lv_obj_remove_style_all(ln);
        lv_obj_set_size(ln,LCD_W,1); lv_obj_set_pos(ln,0,y);
        lv_obj_set_style_bg_color(ln,lv_color_hex(0x111111),0);
        lv_obj_set_style_bg_opa(ln,80,0);
    }
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
    for(int p=0;p<=100;p+=4){
        lv_bar_set_value(bar,p,LV_ANIM_OFF);
        if(p%20==0) lv_label_set_text(t2,steps[p/20>4?4:p/20]);
        lv_timer_handler(); delay(28);
        if(p==40) wifi_quick_sync();
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
    int32_t x = FT3168->IIC_Read_Device_Value(FT3168->Arduino_IIC_Touch::Value_Information::TOUCH_COORDINATE_X);
    int32_t y = FT3168->IIC_Read_Device_Value(FT3168->Arduino_IIC_Touch::Value_Information::TOUCH_COORDINATE_Y);
    int     f = FT3168->IIC_Read_Device_Value(FT3168->Arduino_IIC_Touch::Value_Information::TOUCH_FINGER_NUMBER);
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
