#ifndef LVGL_PORT_H
#define LVGL_PORT_H

#include "lvgl.h"
#include "lcd_st7789_official.h"

/**
 * @file lvgl_port.h
 * @brief LVGL 与 ESP LCD 面板的适配接口。
 */

/**
 * @brief 初始化 LVGL 显示对象、双缓冲区和 LCD 刷新回调。
 *
 * @param lcd_panel 已初始化的 ESP LCD 面板句柄。
 */
void lvgl_port_init(esp_lcd_panel_handle_t lcd_panel);

#endif
