// =============================================================================
// pin_config.h  -  Waveshare ESP32-S3-Touch-AMOLED-2.06
// Mirrors examples/arduino/libraries/Mylibrary/pin_config.h from the
// waveshareteam/ESP32-S3-Touch-AMOLED-2.06 repo (verified against v9 demo).
// =============================================================================
#pragma once

#define XPOWERS_CHIP_AXP2101

// ---- AMOLED panel (CO5300 driver, QSPI bus) --------------------------------
#define LCD_SDIO0   4
#define LCD_SDIO1   5
#define LCD_SDIO2   6
#define LCD_SDIO3   7
#define LCD_SCLK    11
#define LCD_CS      12
#define LCD_RESET   8
#define LCD_WIDTH   410
#define LCD_HEIGHT  502

// ---- Touch (FT3168 / FT3x68 via I2C) ---------------------------------------
#define IIC_SDA     15
#define IIC_SCL     14
#define TP_INT      38
#define TP_RESET    9

// ---- SD-MMC (1-bit) --------------------------------------------------------
static const int SDMMC_CLK  = 2;
static const int SDMMC_CMD  = 1;
static const int SDMMC_DATA = 3;
static const int SDMMC_CS   = 17;
