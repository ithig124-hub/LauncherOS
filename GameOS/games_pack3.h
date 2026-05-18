// =====================================================================
//  games_pack3.h  -  SHOWPIECES (10 games)
//    TRON, MATCH-3, PINBALL, WORDLE, SUDOKU, CHECKERS,
//    AIR-HOCKEY, PLATFORMER, TOWER-STACK, CROSSY
// =====================================================================
#pragma once

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
