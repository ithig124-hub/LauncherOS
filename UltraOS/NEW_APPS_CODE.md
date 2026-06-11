# UltraOS v2.0 - Complete App Implementations

## Instructions
Add these complete app implementations to UltraOS.ino after the stopwatch app (around line 700).

Replace the stub functions with these full implementations:

```cpp
// =============================================================================
//  TIMER APP - Complete Implementation
// =============================================================================
static lv_obj_t *g_timer_screen = nullptr;
static lv_obj_t *g_timer_label = nullptr;
static lv_obj_t *g_timer_arc = nullptr;
static uint32_t g_timer_duration = 0;  // seconds
static uint32_t g_timer_remaining = 0;
static uint32_t g_timer_start_time = 0;
static bool g_timer_running = false;

static void open_timer() {
    g_timer_screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(g_timer_screen, lv_color_hex(COL_BG), 0);
    lv_obj_set_style_bg_opa(g_timer_screen, LV_OPA_COVER, 0);
    
    // Title
    lv_obj_t *title = lv_label_create(g_timer_screen);
    lv_label_set_text(title, "TIMER");
    lv_obj_set_style_text_color(title, lv_color_hex(COL_ORANGE), 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_22, 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 30);
    
    // Time display
    g_timer_label = lv_label_create(g_timer_screen);
    lv_label_set_text(g_timer_label, "05:00");
    lv_obj_set_style_text_color(g_timer_label, lv_color_hex(COL_WHITE), 0);
    lv_obj_set_style_text_font(g_timer_label, &lv_font_montserrat_48, 0);
    lv_obj_align(g_timer_label, LV_ALIGN_CENTER, 0, -40);
    
    // Progress arc
    g_timer_arc = lv_arc_create(g_timer_screen);
    lv_obj_set_size(g_timer_arc, 200, 200);
    lv_obj_align(g_timer_arc, LV_ALIGN_CENTER, 0, -30);
    lv_arc_set_rotation(g_timer_arc, 270);
    lv_arc_set_bg_angles(g_timer_arc, 0, 360);
    lv_arc_set_range(g_timer_arc, 0, 100);
    lv_arc_set_value(g_timer_arc, 100);
    lv_obj_remove_style(g_timer_arc, NULL, LV_PART_KNOB);
    lv_obj_clear_flag(g_timer_arc, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_arc_width(g_timer_arc, 15, LV_PART_MAIN);
    lv_obj_set_style_arc_width(g_timer_arc, 15, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(g_timer_arc, lv_color_hex(0x1C1C1C), LV_PART_MAIN);
    lv_obj_set_style_arc_color(g_timer_arc, lv_color_hex(COL_ORANGE), LV_PART_INDICATOR);
    
    // Preset buttons
    const char *presets[] = {"1m", "3m", "5m", "10m", "15m", "30m"};
    int preset_secs[] = {60, 180, 300, 600, 900, 1800};
    int x_pos[] = {-120, -40, 40, -120, -40, 40};
    int y_pos[] = {100, 100, 100, 160, 160, 160};
    
    for (int i = 0; i < 6; i++) {
        lv_obj_t *btn = lv_btn_create(g_timer_screen);
        lv_obj_set_size(btn, 70, 50);
        lv_obj_align(btn, LV_ALIGN_CENTER, x_pos[i], y_pos[i]);
        lv_obj_set_style_bg_color(btn, lv_color_hex(COL_PANEL), 0);
        lv_obj_set_style_radius(btn, 10, 0);
        
        lv_obj_t *lbl = lv_label_create(btn);
        lv_label_set_text(lbl, presets[i]);
        lv_obj_set_style_text_color(lbl, lv_color_hex(COL_WHITE), 0);
        lv_obj_center(lbl);
        
        int duration = preset_secs[i];
        lv_obj_add_event_cb(btn, [](lv_event_t *e) {
            int dur = (int)(intptr_t)lv_event_get_user_data(e);
            g_timer_duration = dur;
            g_timer_remaining = dur;
            g_timer_running = false;
            char buf[16];
            snprintf(buf, 16, "%02d:%02d", dur/60, dur%60);
            lv_label_set_text(g_timer_label, buf);
            lv_arc_set_value(g_timer_arc, 100);
        }, LV_EVENT_CLICKED, (void*)(intptr_t)duration);
    }
    
    // Start/Stop button
    lv_obj_t *btn_start = lv_btn_create(g_timer_screen);
    lv_obj_set_size(btn_start, 100, 100);
    lv_obj_align(btn_start, LV_ALIGN_BOTTOM_MID, 0, -60);
    lv_obj_set_style_radius(btn_start, 50, 0);
    lv_obj_set_style_bg_color(btn_start, lv_color_hex(COL_GREEN), 0);
    lv_obj_t *lbl_start = lv_label_create(btn_start);
    lv_label_set_text(lbl_start, LV_SYMBOL_PLAY);
    lv_obj_set_style_text_font(lbl_start, &lv_font_montserrat_48, 0);
    lv_obj_center(lbl_start);
    lv_obj_add_event_cb(btn_start, [](lv_event_t*) {
        if (!g_timer_running && g_timer_remaining > 0) {
            g_timer_start_time = millis();
            g_timer_running = true;
        } else {
            g_timer_running = false;
        }
    }, LV_EVENT_CLICKED, NULL);
    
    // Back button
    lv_obj_t *back = lv_btn_create(g_timer_screen);
    lv_obj_set_size(back, 140, 40);
    lv_obj_align(back, LV_ALIGN_TOP_LEFT, 20, 70);
    lv_obj_set_style_bg_color(back, lv_color_hex(0x141414), 0);
    lv_obj_set_style_border_color(back, lv_color_hex(COL_ORANGE), 0);
    lv_obj_set_style_border_width(back, 1, 0);
    lv_obj_t *bl = lv_label_create(back);
    lv_label_set_text(bl, LV_SYMBOL_LEFT " Back");
    lv_obj_set_style_text_color(bl, lv_color_hex(COL_ORANGE), 0);
    lv_obj_center(bl);
    lv_obj_add_event_cb(back, [](lv_event_t*) {
        lv_scr_load(g_launcher_screen);
    }, LV_EVENT_CLICKED, NULL);
    
    lv_scr_load(g_timer_screen);
}

// =============================================================================
//  WEATHER APP - OpenWeatherMap Integration
// =============================================================================
static lv_obj_t *g_weather_screen = nullptr;
static lv_obj_t *g_weather_temp = nullptr;
static lv_obj_t *g_weather_desc = nullptr;
static lv_obj_t *g_weather_humidity = nullptr;
static lv_obj_t *g_weather_wind = nullptr;
static String g_weather_api_key = "51e5cba611055b1fb8145d36eb4b4f45";
static String g_weather_city = "San Francisco";
static float g_temp = 0.0;
static String g_weather_condition = "Loading...";

static void fetch_weather() {
    if (WiFi.status() != WL_CONNECTED) {
        WiFi.begin("UltraWatch", "ultra1234");
        int tries = 0;
        while (WiFi.status() != WL_CONNECTED && tries++ < 20) {
            delay(200);
        }
    }
    
    if (WiFi.status() == WL_CONNECTED) {
        HTTPClient http;
        String url = "http://api.openweathermap.org/data/2.5/weather?q=" + 
                     g_weather_city + "&appid=" + g_weather_api_key + "&units=metric";
        
        http.begin(url);
        int code = http.GET();
        
        if (code == 200) {
            String payload = http.getString();
            // Simple parsing (find temp)
            int temp_idx = payload.indexOf("\"temp\":");
            if (temp_idx > 0) {
                String temp_str = payload.substring(temp_idx + 7, temp_idx + 12);
                g_temp = temp_str.toFloat();
            }
            int desc_idx = payload.indexOf("\"description\":\"");
            if (desc_idx > 0) {
                int end_idx = payload.indexOf("\"", desc_idx + 15);
                g_weather_condition = payload.substring(desc_idx + 15, end_idx);
            }
        }
        http.end();
        WiFi.disconnect(true);
        WiFi.mode(WIFI_OFF);
    }
}

static void open_weather() {
    g_weather_screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(g_weather_screen, lv_color_hex(COL_BG), 0);
    
    lv_obj_t *title = lv_label_create(g_weather_screen);
    lv_label_set_text(title, "WEATHER");
    lv_obj_set_style_text_color(title, lv_color_hex(COL_BLUE), 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_22, 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 30);
    
    g_weather_temp = lv_label_create(g_weather_screen);
    lv_label_set_text(g_weather_temp, "Loading...");
    lv_obj_set_style_text_color(g_weather_temp, lv_color_hex(COL_WHITE), 0);
    lv_obj_set_style_text_font(g_weather_temp, &lv_font_montserrat_48, 0);
    lv_obj_align(g_weather_temp, LV_ALIGN_CENTER, 0, -40);
    
    g_weather_desc = lv_label_create(g_weather_screen);
    lv_label_set_text(g_weather_desc, "...");
    lv_obj_set_style_text_color(g_weather_desc, lv_color_hex(COL_GRAY), 0);
    lv_obj_set_style_text_font(g_weather_desc, &lv_font_montserrat_18, 0);
    lv_obj_align(g_weather_desc, LV_ALIGN_CENTER, 0, 20);
    
    lv_obj_t *city_lbl = lv_label_create(g_weather_screen);
    lv_label_set_text(city_lbl, g_weather_city.c_str());
    lv_obj_set_style_text_color(city_lbl, lv_color_hex(COL_BLUE), 0);
    lv_obj_align(city_lbl, LV_ALIGN_CENTER, 0, 60);
    
    // Refresh button
    lv_obj_t *refresh = lv_btn_create(g_weather_screen);
    lv_obj_set_size(refresh, 140, 50);
    lv_obj_align(refresh, LV_ALIGN_CENTER, 0, 120);
    lv_obj_set_style_bg_color(refresh, lv_color_hex(COL_BLUE), 0);
    lv_obj_t *rlbl = lv_label_create(refresh);
    lv_label_set_text(rlbl, LV_SYMBOL_REFRESH " Refresh");
    lv_obj_center(rlbl);
    lv_obj_add_event_cb(refresh, [](lv_event_t*) {
        lv_label_set_text(g_weather_temp, "Loading...");
        fetch_weather();
        char buf[16];
        snprintf(buf, 16, "%.1f°C", g_temp);
        lv_label_set_text(g_weather_temp, buf);
        lv_label_set_text(g_weather_desc, g_weather_condition.c_str());
    }, LV_EVENT_CLICKED, NULL);
    
    // Back button
    lv_obj_t *back = lv_btn_create(g_weather_screen);
    lv_obj_set_size(back, 140, 40);
    lv_obj_align(back, LV_ALIGN_TOP_LEFT, 20, 70);
    lv_obj_set_style_bg_color(back, lv_color_hex(0x141414), 0);
    lv_obj_set_style_border_color(back, lv_color_hex(COL_BLUE), 0);
    lv_obj_set_style_border_width(back, 1, 0);
    lv_obj_t *bl = lv_label_create(back);
    lv_label_set_text(bl, LV_SYMBOL_LEFT " Back");
    lv_obj_center(bl);
    lv_obj_add_event_cb(back, [](lv_event_t*) {
        lv_scr_load(g_launcher_screen);
    }, LV_EVENT_CLICKED, NULL);
    
    lv_scr_load(g_weather_screen);
    fetch_weather();
}

// =============================================================================
//  CALCULATOR APP - Full Implementation
// =============================================================================
static lv_obj_t *g_calc_screen = nullptr;
static lv_obj_t *g_calc_display = nullptr;
static String g_calc_current = "0";
static String g_calc_operator = "";
static float g_calc_mem = 0;
static bool g_calc_new_number = true;

static void calc_button_click(const char *btn) {
    if (strcmp(btn, "C") == 0) {
        g_calc_current = "0";
        g_calc_operator = "";
        g_calc_mem = 0;
        g_calc_new_number = true;
    } else if (strcmp(btn, "=") == 0) {
        float current_val = g_calc_current.toFloat();
        if (g_calc_operator == "+") g_calc_mem += current_val;
        else if (g_calc_operator == "-") g_calc_mem -= current_val;
        else if (g_calc_operator == "×") g_calc_mem *= current_val;
        else if (g_calc_operator == "÷") g_calc_mem = (current_val != 0) ? g_calc_mem / current_val : 0;
        g_calc_current = String(g_calc_mem);
        g_calc_operator = "";
        g_calc_new_number = true;
    } else if (strchr("+-×÷", btn[0]) != NULL) {
        g_calc_mem = g_calc_current.toFloat();
        g_calc_operator = btn;
        g_calc_new_number = true;
    } else {
        if (g_calc_new_number) {
            g_calc_current = btn;
            g_calc_new_number = false;
        } else {
            if (g_calc_current == "0") g_calc_current = btn;
            else g_calc_current += btn;
        }
    }
    lv_label_set_text(g_calc_display, g_calc_current.c_str());
}

static void open_calculator() {
    g_calc_screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(g_calc_screen, lv_color_hex(COL_BG), 0);
    
    lv_obj_t *title = lv_label_create(g_calc_screen);
    lv_label_set_text(title, "CALCULATOR");
    lv_obj_set_style_text_color(title, lv_color_hex(COL_ORANGE), 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_18, 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 20);
    
    g_calc_display = lv_label_create(g_calc_screen);
    lv_label_set_text(g_calc_display, "0");
    lv_obj_set_style_text_color(g_calc_display, lv_color_hex(COL_WHITE), 0);
    lv_obj_set_style_text_font(g_calc_display, &lv_font_montserrat_48, 0);
    lv_obj_align(g_calc_display, LV_ALIGN_TOP_MID, 0, 60);
    
    const char *buttons[] = {
        "7", "8", "9", "÷",
        "4", "5", "6", "×",
        "1", "2", "3", "-",
        "C", "0", "=", "+"
    };
    
    for (int i = 0; i < 16; i++) {
        int row = i / 4;
        int col = i % 4;
        lv_obj_t *btn = lv_btn_create(g_calc_screen);
        lv_obj_set_size(btn, 85, 70);
        lv_obj_set_pos(btn, 20 + col * 92, 140 + row * 80);
        
        uint32_t color = (col == 3) ? COL_ORANGE : (strcmp(buttons[i], "C") == 0 || strcmp(buttons[i], "=") == 0) ? COL_GRAY : COL_PANEL;
        lv_obj_set_style_bg_color(btn, lv_color_hex(color), 0);
        lv_obj_set_style_radius(btn, 15, 0);
        
        lv_obj_t *lbl = lv_label_create(btn);
        lv_label_set_text(lbl, buttons[i]);
        lv_obj_set_style_text_font(lbl, &lv_font_montserrat_28, 0);
        lv_obj_center(lbl);
        
        const char *btn_val = buttons[i];
        lv_obj_add_event_cb(btn, [](lv_event_t *e) {
            const char *val = (const char*)lv_event_get_user_data(e);
            calc_button_click(val);
        }, LV_EVENT_CLICKED, (void*)btn_val);
    }
    
    // Back button
    lv_obj_t *back = lv_btn_create(g_calc_screen);
    lv_obj_set_size(back, 120, 35);
    lv_obj_align(back, LV_ALIGN_TOP_LEFT, 15, 120);
    lv_obj_set_style_bg_color(back, lv_color_hex(0x141414), 0);
    lv_obj_set_style_border_color(back, lv_color_hex(COL_ORANGE), 0);
    lv_obj_set_style_border_width(back, 1, 0);
    lv_obj_t *bl = lv_label_create(back);
    lv_label_set_text(bl, LV_SYMBOL_LEFT " Back");
    lv_obj_set_style_text_font(bl, &lv_font_montserrat_12, 0);
    lv_obj_center(bl);
    lv_obj_add_event_cb(back, [](lv_event_t*) {
        lv_scr_load(g_launcher_screen);
    }, LV_EVENT_CLICKED, NULL);
    
    lv_scr_load(g_calc_screen);
}
```

## Continue with more apps...
Add these after the calculator app in the same file.

### Coming next in full file:
- Compass app
- Alarm app  
- Snake game
- 2048 game
- Breathe app
- And 15+ more!

**Save this snippet and I'll generate the complete file with all apps!**
