# UltraOS Architecture & Developer Guide

Internal architecture documentation for developers who want to understand, modify, or extend UltraOS.

---

## 📐 Architecture Overview

```
┌─────────────────────────────────────────────────┐
│              UltraOS Application                 │
├─────────────────────────────────────────────────┤
│  Watch Faces  │  App Launcher  │  Individual Apps│
├─────────────────────────────────────────────────┤
│            LVGL UI Framework (v8.3.x)           │
├─────────────────────────────────────────────────┤
│  Hardware Abstraction Layer (HAL)               │
│  ├─ Display (CO5300 AMOLED via Arduino_GFX)    │
│  ├─ Touch (FT3168 via DriveBus)                │
│  ├─ IMU (QMI8658 via I2C)                      │
│  ├─ PMU (AXP2101 via XPowersLib)               │
│  └─ Storage (SD_MMC)                            │
├─────────────────────────────────────────────────┤
│         Arduino ESP32 Core (2.0.x)              │
├─────────────────────────────────────────────────┤
│              ESP32-S3 Hardware                   │
└─────────────────────────────────────────────────┘
```

---

## 🗂️ File Structure

```
UltraOS/
├── UltraOS.ino           # Main sketch (all code in one file)
├── lv_conf.h             # LVGL configuration
├── partitions.csv        # Flash memory partition table
├── README.md             # User documentation
├── BUILD_GUIDE.md        # Build & flash instructions
└── ARCHITECTURE.md       # This file
```

### Why Single File?
LauncherOS requires apps to be **single `.ino` files** with no companion `.h` or `.cpp` files. All code must be self-contained for easy compilation and distribution.

---

## 🧩 Code Sections

The main `UltraOS.ino` file is organized as follows:

### 1. Shared Boot Block (Lines 1-200)
```cpp
// Standard LauncherOS boot code
// - Pin definitions
// - Hardware initialization
// - LVGL setup
// - Back-to-launcher function
```
**Purpose**: Ensures compatibility with LauncherOS ecosystem.
**DO NOT MODIFY** unless syncing with LauncherOS updates.

### 2. Color Palette (Lines 201-220)
```cpp
#define COL_ORANGE 0xFF9F0A  // Ultra signature color
#define COL_BLUE   0x0A84FF
// ... etc
```
**Purpose**: Centralized color management for consistent theming.
**Modify**: Change colors here to retheme entire app.

### 3. IMU Driver (Lines 221-320)
```cpp
// QMI8658 6-axis IMU (accelerometer + gyroscope)
// - I2C communication
// - Sensor initialization
// - Data reading functions
```
**Purpose**: Motion sensing for step counting and compass.
**Extend**: Add gesture recognition, tap detection.

### 4. Activity Tracking (Lines 321-380)
```cpp
// Step counter using IMU
// - Simple magnitude-delta algorithm
// - Debouncing (300ms)
// - Calorie estimation
```
**Purpose**: Fitness tracking core logic.
**Improve**: Implement machine learning classifier for better accuracy.

### 5. Time & NTP Sync (Lines 381-450)
```cpp
// RTC management
// - NVS time persistence
// - Wi-Fi NTP sync
// - Timezone handling
```
**Purpose**: Keep time accurate across reboots.
**Extend**: Add automatic timezone detection, DST handling.

### 6. Astronomical Calculations (Lines 451-550)
```cpp
// Sunrise/Sunset calculator
// - Julian day conversion
// - Solar position algorithms
// Moon phase calculator
```
**Purpose**: Complications for watch faces.
**Accurate**: ±2 minutes for sunrise/sunset within ±60° latitude.

### 7. Splash Screen (Lines 551-600)
```cpp
// Ultra-style boot animation
// - Orange accent circle
// - Progress bar
// - Fade-in effect
```
**Purpose**: Professional boot experience.
**Customize**: Change colors, add logo, modify animation.

### 8. Watch Faces (Lines 601-800)
```cpp
// Modular Ultra face (default)
// - Central time display
// - 3 activity rings (Move, Exercise, Stand)
// - 4 corner complications
```
**Purpose**: Main UI for time display.
**Extend**: Add Wayfinder, Solar, and custom faces.

### 9. App Launcher (Lines 801-900)
```cpp
// Grid-based app launcher
// - 8 app tiles (3x3 grid)
// - Icon + color per app
// - Tap to launch
```
**Purpose**: Navigate to individual apps.
**Extend**: Add more apps, folders, search.

### 10. Individual Apps (Lines 901-1200)
```cpp
// Stopwatch - Precision timing
// Timer - Countdown (stub)
// Alarm - Wake-up alerts (stub)
// Weather - Forecast (stub)
// Compass - Direction finding (stub)
// Flashlight - Max brightness
// Calculator - Quick math (stub)
// Settings - Preferences (stub)
```
**Purpose**: Utility functions.
**Implement**: Complete stub functions with full UI.

### 11. Main Loop (Lines 1201-end)
```cpp
void setup() { ... }
void loop() { ... }
```
**Purpose**: Arduino entry points.
**Flow**:
1. setup(): Initialize hardware, show splash, build UI
2. loop(): Handle events, update displays, manage power

---

## 🔧 Hardware Abstraction

### Display (CO5300 AMOLED)
```cpp
Arduino_GFX *gfx = new Arduino_CO5300(...);
gfx->fillScreen(0x0000);
gfx->draw16bitRGBBitmap(...);
```
- **Resolution**: 410×502 pixels
- **Color depth**: 16-bit (RGB565)
- **Brightness**: 0-255 (600 nit max)

### Touch (FT3168 Capacitive)
```cpp
FT3168->IIC_Read_Device_Value(TOUCH_COORDINATE_X);
```
- **Max touch points**: 5
- **Gestures**: Swipe, long-press
- **Interrupt**: GPIO 38 (active low)

### IMU (QMI8658)
```cpp
qmi_read_accel(ax, ay, az);  // ±2g full scale
qmi_read_gyro(gx, gy, gz);   // ±512 dps
```
- **Update rate**: 125 Hz
- **Accuracy**: 16-bit resolution
- **Power**: ~0.5 mA continuous

### PMU (AXP2101)
```cpp
g_pmu.getBatteryPercent();   // 0-100%
g_pmu.isCharging();          // bool
g_pmu.getBattVoltage();      // mV
```
- **Battery**: 400 mAh LiPo
- **Charge current**: 500 mA max
- **Voltage**: 3.7V nominal

---

## 🎨 UI Framework (LVGL)

### Object Hierarchy
```
lv_obj (screen)
 ├── lv_label (time text)
 ├── lv_label (date text)
 ├── lv_arc (activity ring 1)
 ├── lv_arc (activity ring 2)
 ├── lv_arc (activity ring 3)
 └── lv_label (complication 1-4)
```

### Creating Widgets
```cpp
lv_obj_t *label = lv_label_create(parent);
lv_label_set_text(label, "Hello");
lv_obj_set_style_text_color(label, lv_color_hex(0xFF9F0A), 0);
lv_obj_set_style_text_font(label, &lv_font_montserrat_48, 0);
lv_obj_align(label, LV_ALIGN_CENTER, 0, 0);
```

### Event Handling
```cpp
lv_obj_add_event_cb(button, [](lv_event_t *e) {
    // Handle click
    Serial.println("Button clicked!");
}, LV_EVENT_CLICKED, NULL);
```

### Animations
```cpp
lv_scr_load_anim(screen, LV_SCR_LOAD_ANIM_FADE_ON, 200, 0, false);
```

---

## 🔄 State Management

### Global State Variables
```cpp
static int g_steps = 0;              // Step counter
static int g_move_calories = 0;      // Calories burned
static int g_exercise_minutes = 0;   // Active minutes
static int g_stand_hours = 0;        // Stand hours
static int g_current_screen = 0;     // 0=face, 1=launcher
static bool g_stopwatch_running = false;
```

### Persistence (NVS)
```cpp
Preferences prefs;
prefs.begin("ultraos", false);
prefs.putInt("steps", g_steps);
int saved_steps = prefs.getInt("steps", 0);
prefs.end();
```

**Namespaces used**:
- `rtcsave`: Time persistence (shared with LauncherOS)
- `ultraos`: App-specific settings
- `face`: Watch face selection (shared)

---

## ⚡ Performance Optimization

### Memory Management
- **LVGL buffer**: 60 lines × 410 px = ~50 KB
- **PSRAM usage**: Store large graphics, reduce SRAM load
- **Heap fragmentation**: Avoid frequent malloc/free

### Power Optimization
```cpp
setCpuFrequencyMhz(160);  // Reduce from 240 MHz when idle
esp_light_sleep_start();  // Sleep between events
```

### Display Updates
```cpp
// Only redraw changed elements
if (old_time != new_time) {
    lv_label_set_text(time_label, new_time);
}
// Batch updates, then refresh
lv_timer_handler();  // Call once per loop iteration
```

---

## 🛠️ Adding New Features

### Example: Add a New App

1. **Define the app entry** (in APPS array):
```cpp
{"MyApp", LV_SYMBOL_STAR, COL_PURPLE, open_myapp}
```

2. **Create the app function**:
```cpp
static lv_obj_t *g_myapp_screen = nullptr;

static void open_myapp() {
    g_myapp_screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(g_myapp_screen, lv_color_hex(COL_BG), 0);
    
    // Add your UI elements here
    lv_obj_t *title = lv_label_create(g_myapp_screen);
    lv_label_set_text(title, "MY APP");
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 30);
    
    // Add back button
    lv_obj_t *back = lv_btn_create(g_myapp_screen);
    lv_obj_set_size(back, 140, 40);
    lv_obj_align(back, LV_ALIGN_TOP_LEFT, 20, 70);
    lv_obj_add_event_cb(back, [](lv_event_t*) {
        lv_scr_load(g_launcher_screen);
    }, LV_EVENT_CLICKED, NULL);
    
    lv_scr_load(g_myapp_screen);
}
```

3. **Update in loop()** (if needed):
```cpp
void loop() {
    // ... existing code ...
    
    // Update your app
    if (g_current_app == APP_MYAPP) {
        update_myapp();
    }
}
```

### Example: Add a New Watch Face

1. **Create build function**:
```cpp
static void build_wayfinder_face() {
    // Clear existing face elements
    // Build new face UI
    // Position time at specific coordinates
    // Add compass rose graphic
    // Add direction indicators
}
```

2. **Add to face switcher**:
```cpp
void switch_watch_face(int face_id) {
    switch(face_id) {
        case 0: build_modular_face(); break;
        case 1: build_wayfinder_face(); break;
        case 2: build_solar_face(); break;
    }
}
```

3. **Implement swipe gesture** to cycle faces:
```cpp
// In touch handler
if (swipe_up && g_current_screen == 0) {
    g_current_face = (g_current_face + 1) % 3;
    switch_watch_face(g_current_face);
}
```

---

## 🧪 Testing & Debugging

### Serial Debugging
```cpp
Serial.println("[DEBUG] Steps: " + String(g_steps));
Serial.printf("[IMU] Accel: %.2f, %.2f, %.2f\n", ax, ay, az);
```

### Memory Leak Detection
```cpp
void loop() {
    static uint32_t last_check = 0;
    if (millis() - last_check > 10000) {
        Serial.printf("Free heap: %d bytes\n", ESP.getFreeHeap());
        last_check = millis();
    }
}
```

### Crash Analysis
```cpp
// Enable core dumps in menuconfig
// Read crash backtrace from serial output
// Use addr2line to decode addresses:
// xtensa-esp32s3-elf-addr2line -e UltraOS.ino.elf 0x4200xxxx
```

---

## 📚 API Reference

### Key Functions

#### Hardware Initialization
```cpp
void launcher_app_boot();      // Init display, touch, I2C, LVGL, SD
void qmi_init();               // Init IMU sensor
void pmu_init();               // Init power management
```

#### Time Management
```cpp
void time_restore_from_nvs();  // Load saved time from NVS
void wifi_load_and_sync();     // Sync time via NTP
```

#### Display Control
```cpp
void build_modular_face();     // Create Modular Ultra watch face
void update_watch_face();      // Refresh time, rings, complications
void goto_screen(int screen);  // Switch between face/launcher
```

#### Activity Tracking
```cpp
void step_counter_update();    // Process IMU data for steps
```

#### Astronomical
```cpp
void calc_sunrise_sunset(int &rh, int &rm, int &sh, int &sm);
int moon_phase_day();          // Returns 0-29 (lunar day)
```

---

## 🔐 Security Considerations

### Wi-Fi Credentials
- Stored in plaintext on SD card (`/WATCH/wifi/config.txt`)
- **TODO**: Encrypt with device-unique key

### NVS Data
- Unencrypted by default
- Enable NVS encryption in menuconfig for production

### OTA Updates
- No signature verification currently
- **TODO**: Implement signed firmware updates

---

## 🚀 Future Enhancements

### High Priority
- [ ] Complete stub apps (Timer, Alarm, Weather, Compass)
- [ ] Wayfinder and Solar watch faces
- [ ] BLE notifications (ANCS/GadgetBridge)
- [ ] Activity data export (FIT format)

### Medium Priority
- [ ] Voice memos with playback
- [ ] Music player from SD card
- [ ] Customizable complications
- [ ] Multiple time zones
- [ ] Tide predictions

### Low Priority
- [ ] Third-party app loader
- [ ] Watch face marketplace
- [ ] Workout auto-detection
- [ ] Sleep stage analysis

---

## 📖 Resources

### Datasheets
- [ESP32-S3 TRM](https://www.espressif.com/sites/default/files/documentation/esp32-s3_technical_reference_manual_en.pdf)
- [CO5300 Display](https://www.waveshare.com/wiki/ESP32-S3-Touch-AMOLED-2.06)
- [QMI8658 IMU](https://www.qstcorp.com/upload/pdf/202202/QMI8658A%20Datasheet%20Rev.%20A.pdf)
- [AXP2101 PMU](https://github.com/lewisxhe/XPowersLib)

### Libraries
- [LVGL Docs](https://docs.lvgl.io/8.3/)
- [Arduino_GFX](https://github.com/moononournation/Arduino_GFX)
- [XPowersLib](https://github.com/lewisxhe/XPowersLib)

### Community
- [LauncherOS Repo](https://github.com/ithig124-hub/LauncherOS)
- [ESP32-S3 Forum](https://www.esp32.com/)
- [LVGL Forum](https://forum.lvgl.io/)

---

## 🤝 Contributing Guidelines

### Code Style
- Use 4-space indentation
- Function names: `snake_case`
- Constants: `UPPER_SNAKE_CASE`
- Colors: Hex format with `COL_` prefix

### Commit Messages
```
feat: Add wayfinder watch face
fix: Correct step counter debouncing
docs: Update API reference
refactor: Optimize battery polling
```

### Pull Request Process
1. Fork repository
2. Create feature branch
3. Write/update tests
4. Update documentation
5. Submit PR with clear description

---

## 📄 License

MIT License - See LICENSE file for full text.

---

**Built with passion for the ESP32 community. Happy hacking! ⚡**
