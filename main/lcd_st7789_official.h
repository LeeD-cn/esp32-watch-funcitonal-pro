#ifndef LCD_ST7789_OFFICIAL_H
#define LCD_ST7789_OFFICIAL_H

#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_panel_ops.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include <stdint.h>

/**
 * @file lcd_st7789_official.h
 * @brief ST7789 屏幕初始化、背光控制和休眠接口。
 */

/* ST7789 屏幕 SPI 与背光硬件配置。 */
#define LCD_HOST            SPI2_HOST
#define PIN_NUM_SCLK        13
#define PIN_NUM_MOSI        12
#define PIN_NUM_MISO        -1
#define PIN_NUM_CS          -1
#define PIN_NUM_DC          14
#define PIN_NUM_RST         11
#define PIN_NUM_BL          10

#define LCD_H_RES           240
#define LCD_V_RES           240
#define LCD_PIXEL_CLOCK_HZ  40 * 1000 * 1000

/**
 * @brief 初始化 ST7789 LCD 面板和背光 PWM。
 *
 * @return esp_lcd_panel_handle_t LCD 面板句柄。
 */
esp_lcd_panel_handle_t lcd_panel_init(void);

/**
 * @brief 获取 LCD SPI 面板 IO 句柄。
 *
 * @return esp_lcd_panel_io_handle_t 面板 IO 句柄，未初始化时为 NULL。
 */
esp_lcd_panel_io_handle_t lcd_panel_get_io_handle(void);

/**
 * @brief 打开或关闭 LCD 背光。
 *
 * @param on true 打开背光，false 关闭背光。
 */
void lcd_set_backlight(bool on);

/**
 * @brief 设置背光亮度百分比。
 *
 * @param percent 亮度百分比，0 表示关闭，非零值会被限制在 UI 允许范围内。
 */
void lcd_set_backlight_percent(uint8_t percent);

/**
 * @brief 获取当前或最近一次非零背光亮度。
 *
 * @return uint8_t 亮度百分比。
 */
uint8_t lcd_get_backlight_percent(void);

/**
 * @brief 控制 ST7789 进入或退出低功耗休眠。
 *
 * @param sleep true 进入休眠，false 唤醒显示。
 */
void lcd_panel_sleep(bool sleep);

#endif
