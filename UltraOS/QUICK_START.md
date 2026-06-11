# UltraOS Quick Start Guide

⚡ **5-Minute Setup** - Get UltraOS running on your ESP32-S3 smartwatch in under 5 minutes!

---

## 🎯 What You Need

- ✅ Waveshare ESP32-S3-Touch-AMOLED-2.06 watch
- ✅ USB-C cable (data-capable!)
- ✅ Arduino IDE 2.x installed
- ✅ 5 minutes of your time

---

## 🚀 Step 1: Install Arduino IDE

**Download**: https://www.arduino.cc/en/software

- Windows: Download `.exe`, run installer
- Mac: Download `.dmg`, drag to Applications
- Linux: Download `.AppImage`, make executable

---

## 📦 Step 2: Add ESP32 Support (1 minute)

1. Open Arduino IDE
2. **File → Preferences**
3. In "Additional Board Manager URLs", paste:
   ```
   https://espressif.github.io/arduino-esp32/package_esp32_index.json
   ```
4. **Tools → Board → Boards Manager**
5. Search **"esp32"**, install **"esp32 by Espressif"**
6. Wait for download (~200 MB)

---

## 📚 Step 3: Install Libraries (2 minutes)

**Sketch → Include Library → Manage Libraries**

Search and install:
1. **LVGL** (version 8.3.11 or 8.4.0 - NOT 9.x!)
2. **Arduino_GFX_Library**
3. **Arduino_DriveBus_Library**
4. **XPowersLib**

Click "Install All" for dependencies.

---

## ⚙️ Step 4: Configure Board (30 seconds)

**Tools menu** - Set these:

| Setting | Value |
|---------|-------|
| Board | **ESP32S3 Dev Module** |
| USB CDC On Boot | **Enabled** |
| Flash Size | **16MB (128Mb)** |
| Partition Scheme | **Minimal SPIFFS (Large APP)** ⚠️ |
| PSRAM | **OPI PSRAM** ⚠️ |
| CPU Frequency | **240MHz (WiFi)** |
| Upload Speed | **921600** |

**CRITICAL**: Partition and PSRAM settings must be exact!

---

## 📝 Step 5: Open UltraOS (10 seconds)

1. Navigate to `/app/UltraOS/` folder
2. Double-click **UltraOS.ino**
3. Arduino IDE opens the sketch

---

## 🔌 Step 6: Connect Watch (30 seconds)

1. Plug USB-C cable into watch
2. Connect other end to computer
3. Watch powers on automatically
4. **Tools → Port** → Select your ESP32 port
   - Windows: `COM3`, `COM4`, etc.
   - Mac: `/dev/cu.usbserial-XXXX`
   - Linux: `/dev/ttyUSB0`, `/dev/ttyACM0`

**No port?** Hold BOOT button while connecting.

---

## 🚀 Step 7: Upload! (1 minute)

1. Click **Upload** button (→)
2. Watch progress bar
3. When done, watch reboots automatically
4. **Done!** 🎉

You'll see:
1. Orange "ULTRA WATCH OS" splash screen
2. Progress bar animation
3. Modular Ultra watch face with activity rings

---

## 🎮 Using UltraOS

### Navigation
- **Swipe Left**: Watch Face → Apps
- **Swipe Right**: Apps → Watch Face
- **Swipe Down** (face): Quick flashlight
- **Hold BOOT (1.5s)**: Exit to LauncherOS

### Apps Available
- ⏱️ **Stopwatch** - Precision timing
- ⏰ **Timer** - Countdown (coming soon)
- 🔔 **Alarm** - Wake alerts (coming soon)
- 🌤️ **Weather** - Forecast (coming soon)
- 🧭 **Compass** - Direction (coming soon)
- 🔦 **Flashlight** - Max brightness
- 🔢 **Calculator** - Quick math (coming soon)
- ⚙️ **Settings** - Preferences (coming soon)

### Watch Face Elements
- **Center**: Current time
- **Top**: Date (uppercase)
- **Rings**: Activity progress (Move/Exercise/Stand)
- **Corners**: Complications (steps, battery, sunrise, moon)

---

## 🐛 Quick Troubleshooting

### "Compilation error"
→ Check LVGL version is 8.3.x or 8.4.x (NOT 9.x)

### "Upload failed"
→ Hold BOOT button, press RESET, then upload

### Display stays black
→ Check partition scheme is "Minimal SPIFFS"

### Touch doesn't work
→ Verify PSRAM is set to "OPI PSRAM"

### No serial port
→ Install CH340 or CP210x USB drivers

---

## ⚡ Optional: Wi-Fi Time Sync

1. Insert microSD card into PC
2. Create folder: `/WATCH/wifi/`
3. Create file: `/WATCH/wifi/config.txt`
4. Add your Wi-Fi credentials:
   ```ini
   WIFI1_SSID=YourNetworkName
   WIFI1_PASS=YourPassword
   GMT_OFFSET=8
   ```
5. Insert card into watch
6. Reboot watch near Wi-Fi

Time will sync automatically!

---

## 📖 Next Steps

1. ✅ **Explore** - Try all the apps and gestures
2. ✅ **Customize** - Edit colors and text in the code
3. ✅ **Extend** - Add your own apps (see ARCHITECTURE.md)
4. ✅ **Integrate** - Use with LauncherOS for multi-app setup

---

## 📚 Full Documentation

- **README.md** - Feature overview & screenshots
- **BUILD_GUIDE.md** - Detailed build instructions
- **ARCHITECTURE.md** - Code structure & dev guide

---

## 💬 Need Help?

**GitHub Issues**: Report bugs or ask questions
**Serial Monitor**: Check debug output (115200 baud)
**Community**: Join ESP32 forums

---

## 🎉 Success!

You now have a fully functional Apple Watch Ultra-inspired OS on your ESP32-S3 smartwatch!

**Features working**:
- ✅ Beautiful AMOLED watch face
- ✅ Step counter via IMU
- ✅ Activity rings
- ✅ Battery monitoring
- ✅ Stopwatch app
- ✅ Flashlight mode
- ✅ App launcher
- ✅ Astronomical complications

**Enjoy your Ultra watch! ⌚🚀**

---

*Built with ❤️ by the ESP32 community*
