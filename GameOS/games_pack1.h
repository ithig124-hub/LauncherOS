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
#pragma once

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
