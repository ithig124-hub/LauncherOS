# UltraOS Build & Flash Guide

Complete step-by-step instructions for building and deploying UltraOS to your ESP32-S3 smartwatch.

## 📋 Prerequisites

### Hardware
- Waveshare ESP32-S3-Touch-AMOLED-2.06 smartwatch
- USB-C cable (data capable, not charge-only)
- microSD card (optional, for LauncherOS integration)
- Computer (Windows/Mac/Linux)

### Software
- Arduino IDE 2.3.0 or newer
- USB drivers (CP210x or CH340, depending on your board)
- Serial monitor software (optional: PuTTY, screen, minicom)

---

## 🔧 Step 1: Arduino IDE Setup

### 1.1 Install Arduino IDE
Download from: https://www.arduino.cc/en/software

### 1.2 Add ESP32 Board Support
1. Open Arduino IDE
2. Go to **File → Preferences** (or **Arduino IDE → Settings** on Mac)
3. In "Additional Board Manager URLs", add:
   ```
   https://espressif.github.io/arduino-esp32/package_esp32_index.json
   ```
4. Click **OK**
5. Go to **Tools → Board → Boards Manager**
6. Search for **"esp32"**
7. Install **"esp32 by Espressif Systems"** (version 2.0.14 or newer)
8. Wait for download and installation to complete

### 1.3 Install Required Libraries
Go to **Sketch → Include Library → Manage Libraries...**

Install these libraries (search by name):
1. **LVGL** by kisvegabor (version 8.3.11 or 8.4.0)
   - ⚠️ IMPORTANT: NOT version 9.x
2. **Arduino_GFX_Library** by moononournation (latest)
3. **Arduino_DriveBus_Library** by moononournation (latest)
4. **XPowersLib** by lewisxhe (latest)

---

## 🔨 Step 2: Configure Board Settings

### 2.1 Select Board
Go to **Tools → Board → esp32 → ESP32S3 Dev Module**

### 2.2 Configure Board Parameters
Set these exact values in the Tools menu:

| Setting | Value |
|---------|-------|
| **USB CDC On Boot** | Enabled |
| **CPU Frequency** | 240MHz (WiFi) |
| **Core Debug Level** | None |
| **USB DFU On Boot** | Disabled |
| **Erase All Flash Before Sketch Upload** | Disabled |
| **Events Run On** | Core 1 |
| **Flash Mode** | QIO 80MHz |
| **Flash Size** | 16MB (128Mb) |
| **Arduino Runs On** | Core 1 |
| **USB Firmware MSC On Boot** | Disabled |
| **Partition Scheme** | **⚠️ Minimal SPIFFS (Large APP with OTA)** |
| **PSRAM** | **OPI PSRAM** |
| **Upload Mode** | UART0 / Hardware CDC |
| **Upload Speed** | 921600 |
| **USB Mode** | Hardware CDC and JTAG |

**CRITICAL**: The partition scheme MUST be **"Minimal SPIFFS (Large APP with OTA)"** for LauncherOS compatibility!

---

## 📝 Step 3: Open and Verify Project

### 3.1 Open UltraOS.ino
1. Navigate to the `/app/UltraOS/` folder
2. Double-click **UltraOS.ino**
3. Arduino IDE should open with the sketch

### 3.2 Verify Required Files
Ensure these files are in the same folder:
- ✅ UltraOS.ino (main sketch)
- ✅ lv_conf.h (LVGL configuration)
- ✅ partitions.csv (partition table)
- ✅ README.md (documentation)

### 3.3 Check Compilation
Click **Verify** (✓ button) to compile without uploading.

Expected output:
```
Sketch uses XXXXX bytes (XX%) of program storage space.
Global variables use XXXXX bytes (XX%) of dynamic memory.
```

If errors occur, check:
- All libraries installed correctly
- Board settings match Step 2.2
- LVGL version is 8.3.x or 8.4.x (NOT 9.x)

---

## 🔌 Step 4: Connect Hardware

### 4.1 Prepare the Watch
1. Ensure watch is charged (>30% battery)
2. Power off the watch (if currently on)

### 4.2 Connect USB
1. Use a **data-capable** USB-C cable (not charge-only!)
2. Connect watch to computer
3. Watch should power on automatically

### 4.3 Install USB Drivers (if needed)

**Windows:**
- If device shows as "Unknown Device", install CH340/CP210x drivers
- Download from: http://www.wch-ic.com/downloads/CH341SER_EXE.html

**Mac:**
- Drivers usually pre-installed
- If issues: https://github.com/adrianmihalko/ch340g-ch34g-ch34x-mac-os-x-driver

**Linux:**
- Usually works out-of-box
- Add your user to `dialout` group:
  ```bash
  sudo usermod -a -G dialout $USER
  ```
- Log out and back in

---

## 🚀 Step 5: Upload Firmware

### 5.1 Select Port
1. Go to **Tools → Port**
2. Select the port showing your ESP32-S3:
   - **Windows**: `COM3`, `COM4`, etc.
   - **Mac**: `/dev/cu.usbserial-XXXX` or `/dev/cu.wchusbserial-XXXX`
   - **Linux**: `/dev/ttyUSB0`, `/dev/ttyACM0`, etc.

If no port appears:
- Try different USB port on computer
- Check USB cable is data-capable
- Install drivers (see Step 4.3)
- Press and hold BOOT button on watch while connecting

### 5.2 Upload
1. Click **Upload** (→ button)
2. Watch for progress:
   ```
   Connecting.....
   Writing at 0x00010000... (XX%)
   ...
   Hard resetting via RTS pin...
   ```
3. Upload complete! Watch will reboot automatically.

### 5.3 First Boot
Watch will:
1. Show **ULTRA WATCH OS** splash screen (orange circle + progress bar)
2. Initialize Wi-Fi sync (if configured)
3. Load to **Modular Ultra** watch face
4. Display time, date, and activity rings

---

## 💾 Step 6: SD Card Setup (Optional for LauncherOS)

### 6.1 Format SD Card
- Format as **FAT32**
- Allocation size: 4096 bytes (default)

### 6.2 Create Folder Structure
```
/WATCH/
├── bin/
│   ├── launcheros.bin    ← Place LauncherOS here
│   └── ultraos.bin        ← Place compiled UltraOS here
├── PHOTOS/                ← Watch face wallpapers (optional)
└── wifi/
    └── config.txt         ← Wi-Fi configuration
```

### 6.3 Create WiFi Config
Create `/WATCH/wifi/config.txt`:
```ini
WIFI1_SSID=YourNetworkName
WIFI1_PASS=YourPassword
GMT_OFFSET=8
```

Replace:
- `YourNetworkName` = your Wi-Fi SSID
- `YourPassword` = your Wi-Fi password
- `GMT_OFFSET` = your timezone offset from UTC (e.g., 8 for UTC+8)

### 6.4 Export and Copy Binary
1. In Arduino IDE: **Sketch → Export Compiled Binary**
2. Find `UltraOS.ino.bin` in your sketch folder
3. Copy to SD card: `/WATCH/bin/ultraos.bin`
4. Insert SD card into watch

---

## 🔍 Step 7: Verify Installation

### 7.1 Serial Monitor Check
1. Open **Tools → Serial Monitor**
2. Set baud rate to **115200**
3. Reset watch (press RESET button or power cycle)
4. Look for:
   ```
   [IMU] QMI8658 @ 0x6B
   [rtc] restored t=1234567890 gmt=28800
   ```

### 7.2 Functionality Test
- ✅ **Touch response**: Tap screen elements
- ✅ **Display**: Bright, vivid colors
- ✅ **Time sync**: Shows correct time (if Wi-Fi configured)
- ✅ **IMU**: Shake watch → steps should increment
- ✅ **Swipe navigation**: Left swipe → app launcher
- ✅ **BOOT button**: Hold 1.5s → should exit (if LauncherOS installed)

---

## 🐛 Troubleshooting

### Upload Fails: "Timed out waiting for packet header"
**Solution:**
1. Hold **BOOT** button on watch
2. Press **RESET** button (or unplug/replug USB)
3. Release **RESET**, keep holding **BOOT**
4. Click **Upload** in Arduino IDE
5. Release **BOOT** when upload starts

### "Brownout detector was triggered"
**Cause**: Insufficient power during boot
**Solution:**
- Use powered USB hub
- Try different USB port
- Charge watch battery to >50%

### Display Stays Black
**Check:**
1. Serial monitor for error messages
2. Display backlight cable connected
3. Re-flash with correct partition scheme

### Touch Not Working
**Check:**
1. Serial output shows: `[touch] FT3168 initialized`
2. If missing, check I2C wiring
3. Reflash with PSRAM set to "OPI PSRAM"

### Steps Always Show 0
**Check:**
1. Serial output shows: `[IMU] QMI8658 @ 0x6B`
2. If missing, IMU not detected
3. Check hardware connections

### Time Shows "--:--"
**Cause**: No Wi-Fi sync and no saved RTC time
**Solution:**
1. Configure `/WATCH/wifi/config.txt`
2. Reboot watch near Wi-Fi access point
3. Watch will sync time automatically

### Out of Memory / Crash
**Cause**: Insufficient PSRAM or memory leak
**Solution:**
1. Verify PSRAM set to "OPI PSRAM"
2. Check LVGL buffer sizes in lv_conf.h
3. Reduce number of active widgets

---

## 📊 Expected Memory Usage

After successful compilation:

| Resource | Usage | Available | % |
|----------|-------|-----------|---|
| Flash | ~1.2 MB | 6.5 MB | 18% |
| SRAM | ~45 KB | 512 KB | 9% |
| PSRAM | ~120 KB | 8 MB | 1.5% |

If your usage significantly differs, check library versions.

---

## ✅ Success Checklist

- [ ] Arduino IDE 2.x installed
- [ ] ESP32 board support added
- [ ] All 4 libraries installed (LVGL, Arduino_GFX, DriveBus, XPowersLib)
- [ ] Board configured as ESP32-S3 Dev Module
- [ ] Partition: Minimal SPIFFS (Large APP with OTA)
- [ ] PSRAM: OPI PSRAM
- [ ] Sketch compiled without errors
- [ ] USB cable connected (data-capable)
- [ ] Port selected correctly
- [ ] Upload successful
- [ ] Watch shows Ultra splash screen
- [ ] Watch face displays correctly
- [ ] Touch is responsive
- [ ] Time syncs (if Wi-Fi configured)
- [ ] Steps increment when shaking

---

## 🎓 Next Steps

1. **Customize**: Edit watch face styles in the code
2. **Extend**: Add new apps using the app launcher framework
3. **Integrate**: Set up LauncherOS for multi-app switching
4. **Share**: Contribute improvements back to the project

---

## 📞 Getting Help

If stuck:
1. Check **Troubleshooting** section above
2. Review Arduino IDE error messages carefully
3. Post issue with:
   - Full error message
   - Board settings screenshot
   - Library versions
   - Serial monitor output

Happy building! ⌚🚀
