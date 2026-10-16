/**
 * @file lv_conf.h
 * LVGL configuration for UltraOS
 * Based on LauncherOS requirements
 */

#ifndef LV_CONF_H
#define LV_CONF_H

#define LV_USE_DEV_VERSION
#define LV_CONF_SKIP

/* Color settings */
#define LV_COLOR_DEPTH 16
#define LV_COLOR_16_SWAP 0

/* Memory settings */
#define LV_MEM_CUSTOM 0
#define LV_MEM_SIZE (64U * 1024U)
#define LV_MEM_ADR 0

/* Display settings */
#define LV_DPI_DEF 130

/* Drawing settings */
#define LV_USE_GPU_ARM2D 0
#define LV_USE_GPU_STM32_DMA2D 0

/* Logging */
#define LV_USE_LOG 1
#define LV_LOG_LEVEL LV_LOG_LEVEL_WARN
#define LV_LOG_PRINTF 1

/* Fonts */
#define LV_FONT_MONTSERRAT_12 1
#define LV_FONT_MONTSERRAT_14 1
#define LV_FONT_MONTSERRAT_16 1
#define LV_FONT_MONTSERRAT_18 1
#define LV_FONT_MONTSERRAT_22 1
#define LV_FONT_MONTSERRAT_28 1
#define LV_FONT_MONTSERRAT_48 1

/* Widgets */
#define LV_USE_ARC 1
#define LV_USE_BAR 1
#define LV_USE_BTN 1
#define LV_USE_BTNMATRIX 1
#define LV_USE_CANVAS 1
#define LV_USE_CHECKBOX 1
#define LV_USE_DROPDOWN 1
#define LV_USE_IMG 1
#define LV_USE_LABEL 1
#define LV_USE_LINE 1
#define LV_USE_ROLLER 1
#define LV_USE_SLIDER 1
#define LV_USE_SWITCH 1
#define LV_USE_TEXTAREA 1
#define LV_USE_TABLE 1

/* Themes */
#define LV_USE_THEME_DEFAULT 1
#define LV_USE_THEME_BASIC 1

/* Other settings */
#define LV_TICK_CUSTOM 1
#define LV_USE_USER_DATA 1
#define LV_ENABLE_GC 0

/* Performance */
#define LV_REFR_PERIOD 33
#define LV_INV_BUF_SIZE 64
#define LV_USE_PERF_MONITOR 0

/* Input device */
#define LV_INDEV_DEF_READ_PERIOD 30
#define LV_INDEV_DEF_DRAG_LIMIT 10
#define LV_INDEV_DEF_DRAG_THROW 10
#define LV_INDEV_DEF_LONG_PRESS_TIME 400
#define LV_INDEV_DEF_LONG_PRESS_REP_TIME 100
#define LV_INDEV_DEF_GESTURE_LIMIT 50
#define LV_INDEV_DEF_GESTURE_MIN_VELOCITY 3

#endif /*LV_CONF_H*/
