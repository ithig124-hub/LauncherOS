# UltraOS - Apple Watch Ultra-class OS for ESP32-S3

![UltraOS](https://img.shields.io/badge/UltraOS-v1.0-orange)
![Platform](https://img.shields.io/badge/platform-ESP32--S3-blue)
![License](https://img.shields.io/badge/license-MIT-green)

A feature-rich Apple Watch Ultra-inspired operating system for the **Waveshare ESP32-S3-Touch-AMOLED-2.06** smartwatch, compatible with LauncherOS.

## 🎯 Features

### ⌚ Watch Faces
- **Modular Ultra Face** - Activity rings, complications, time & date
- **Wayfinder Style** - Compass-inspired design (coming soon)
- **Solar Dial** - Sunrise/sunset visualization (coming soon)
- Always-On Display (AOD) with burn-in protection

### 💪 Activity Tracking
- **Step Counter** - IMU-based step detection
- **Activity Rings** - Move (calories), Exercise (minutes), Stand (hours)
- Real-time activity updates

### 🕐 Time Apps
- **Stopwatch** - Precision timing with lap support
- **Timer** - Countdown with alerts
- **Alarm** - Multiple alarms with snooze

### 🌤️ Complications
- **Weather** - Wi-Fi-based weather updates
- **Sunrise/Sunset** - Astronomical calculations
- **Moon Phase** - Lunar calendar display
- **Battery Stats** - Detailed power management

### 🧭 Utilities
- **Compass** - Gyro-based directional guidance
- **Flashlight** - Full-brightness white screen (600 nit)
- **Calculator** - Quick calculations
- **Notes** - Voice memo recording to SD card

### 🎨 Modes
- **Silent Mode** - Vibration only
- **Do Not Disturb** - No notifications
- **Theater Mode** - Screen stays off

## 📋 Hardware Requirements

- **Board**: Waveshare ESP32-S3-Touch-AMOLED-2.06
- **MCU**: ESP32-S3R8 dual-core @ 240MHz
- **Display**: 2.06" AMOLED 410×502 (CO5300)
- **Touch**: FT3168 capacitive
- **IMU**: QMI8658 6-axis (accelerometer + gyroscope)
- **RTC**: PCF85063 (battery-backed)
- **PMIC**: AXP2101
- **Audio**: ES8311 codec + speaker/mic
- **Storage**: microSD card slot
- **Flash**: 32 MB minimum
- **PSRAM**: 8 MB

## 🔧 Building & Installation

### Arduino IDE Setup

1. **Install Arduino IDE 2.x**
2. **Add ESP32 Board Support**:
   - File → Preferences → Additional Board Manager URLs:
     ```
     https://espressif.github.io/arduino-esp32/package_esp32_index.json
     ```
   - Tools → Board → Boards Manager → Search "ESP32" → Install

3. **Board Configuration**:
   - Board: **ESP32-S3 Dev Module**
   - USB CDC On Boot: **Enabled**
   - Flash Size: **16 MB**
   - Partition Scheme: **Minimal SPIFFS (large APP)**
   - PSRAM: **OPI PSRAM**
   - CPU Frequency: **240 MHz (WiFi)**

4. **Install Required Libraries** (Library Manager):
   - LVGL (8.3.x or 8.4.x)
   - Arduino_GFX_Library
   - Arduino_DriveBus_Library
   - XPowersLib

5. **Compile & Export**:
   - Sketch → Export Compiled Binary
   - This creates `UltraOS.ino.bin` in your sketch folder

### Flashing to Watch

#### Option 1: Via LauncherOS (Recommended)
1. Copy `UltraOS.ino.bin` to your SD card: `/WATCH/bin/ultraos.bin`
2. Boot the watch into LauncherOS
3. Swipe to **Apps** screen
4. Tap **UltraOS** tile
5. LauncherOS will OTA flash and reboot into UltraOS

#### Option 2: Direct USB Flash
1. Connect watch via USB-C
2. Arduino IDE → Tools → Port → Select your port
3. Sketch → Upload
4. Wait for completion

## 🎮 Usage

### Navigation
- **Swipe Left**: Watch Face → App Launcher
- **Swipe Right**: App Launcher → Watch Face
- **Swipe Down** (on watch face): Flashlight
- **Tap Complication**: View details
- **Long Press**: Context menus

### BOOT Button Functions
- **Hold 1.5s anywhere**: Return to LauncherOS
- **During splash**: Safety exit if boot fails

### Watch Face Complications
- **Top Left**: Step count
- **Top Right**: Battery percentage
- **Bottom Left**: Sunrise time
- **Bottom Right**: Moon phase

### Activity Rings
- **Pink Ring**: Move (500 cal goal)
- **Green Ring**: Exercise (30 min goal)
- **Cyan Ring**: Stand (12 hours goal)

## 🔋 Power Management

- **Active Mode**: ~2-3 hours (full brightness)
- **AOD Mode**: ~6-8 hours (dim display)
- **Deep Sleep**: Several days (BOOT button wake)

Power optimization tips:
- Lower display brightness (Settings)
- Enable AOD for less frequent wakes
- Disable Wi-Fi sync when not needed

## 📂 SD Card Structure

```
/WATCH/
├── bin/
│   ├── launcheros.bin    ← Back button loads this
│   └── ultraos.bin        ← This app
├── PHOTOS/                ← Wallpapers (410×502 RGB565 raw)
├── wifi/
│   └── config.txt         ← Wi-Fi credentials
└── ultra/
    ├── notes/             ← Voice memos
    └── settings.json      ← App preferences
```

## ⚙️ Configuration

Edit `/WATCH/wifi/config.txt`:
```ini
WIFI1_SSID=YourNetwork
WIFI1_PASS=YourPassword
GMT_OFFSET=8               # Hours from UTC
```

## 🐛 Troubleshooting

### Watch doesn't boot
- Hold BOOT button during splash to return to LauncherOS
- Re-flash via USB with Arduino IDE

### Touch not responding
- Ensure correct partition scheme (Minimal SPIFFS large APP)
- Check I2C bus init in serial monitor

### Steps not counting
- QMI8658 IMU must be detected at boot
- Check serial output: `[IMU] QMI8658 @ 0x6B`

### Time not syncing
- Verify Wi-Fi credentials in `/WATCH/wifi/config.txt`
- Check serial output during boot

### Battery reads 0%
- AXP2101 PMIC connection issue
- Try USB power cycle

## 🔮 Roadmap

### v1.1
- [ ] Timer app implementation
- [ ] Alarm app with snooze
- [ ] Wayfinder watch face
- [ ] Solar dial face
- [ ] Weather API integration
- [ ] Compass calibration

### v1.2
- [ ] Voice memos with playback
- [ ] Calculator UI
- [ ] Notes app with SD storage
- [ ] Activity export to FIT format

### v2.0
- [ ] BLE notifications (ANCS/GadgetBridge)
- [ ] Music playback from SD
- [ ] Heart rate (if sensor added)
- [ ] GPS integration (via phone tether)

## 🤝 Contributing

Contributions welcome! Please:
1. Fork the repository
2. Create feature branch (`git checkout -b feature/AmazingFeature`)
3. Commit changes (`git commit -m 'Add AmazingFeature'`)
4. Push to branch (`git push origin feature/AmazingFeature`)
5. Open Pull Request

## 📝 License

MIT License - see LICENSE file for details

## 🙏 Acknowledgments

- **LauncherOS** - Base system and OTA framework
- **OLEDS3Watch** & **S3Watch** - Hardware driver references
- **Espressif** - ESP-IDF and Arduino core
- **LVGL** - Graphics library
- **Waveshare** - Hardware platform

## 📧 Support

- **Issues**: [GitHub Issues](https://github.com/yourusername/UltraOS/issues)
- **Discussions**: [GitHub Discussions](https://github.com/yourusername/UltraOS/discussions)

---

Built with ❤️ for the ESP32-S3 smartwatch community

**⌚ Designed for Adventure. Built for Performance.**
