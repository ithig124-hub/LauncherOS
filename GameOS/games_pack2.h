// =====================================================================
//  games_pack2.h  -  MEDIUM-PACK (10 games)
//    MINESWEEPER, MEMORY, LIGHTS-OUT, CONNECT4, 15-PUZZLE,
//    INVADERS, ASTEROIDS, DOODLE-DODGE, FROGGER, MAZE
// =====================================================================
#pragma once

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
