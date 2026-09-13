/**
 * @file watch_menu.h
 * @brief 主菜单页面接口。
 */
#ifndef WATCH_MENU_H
#define WATCH_MENU_H

#include <stdbool.h>
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 创建主菜单页面。
 *
 * @param parent LVGL 父对象。
 * @return 菜单页根对象，失败时返回 NULL。
 */
lv_obj_t *watch_menu_create(lv_obj_t *parent);
/**
 * @brief 将菜单选择移动到下一项。
 */
void watch_menu_next(void);
/**
 * @brief 将菜单选择移动到上一项。
 */
void watch_menu_prev(void);
/**
 * @brief 判断当前是否选择返回项。
 *
 * @return true 已选择返回项。
 */
bool watch_menu_is_back_selected(void);
/**
 * @brief 重置菜单选择到默认项。
 */
void watch_menu_reset(void);
/**
 * @brief 判断当前是否选择番茄钟。
 *
 * @return true 已选择番茄钟。
 */
bool watch_menu_is_tomato_clock_selected(void);
/**
 * @brief 判断当前是否选择游戏。
 *
 * @return true 已选择游戏。
 */
bool watch_menu_is_game_selected(void);
/**
 * @brief 判断当前是否选择电子名片。
 *
 * @return true 已选择电子名片。
 */
bool watch_menu_is_e_card_selected(void);
/**
 * @brief 判断当前是否选择天气。
 *
 * @return true 已选择天气。
 */
bool watch_menu_is_weather_selected(void);
/**
 * @brief 判断当前是否选择指南针。
 *
 * @return true 已选择指南针。
 */
bool watch_menu_is_compass_selected(void);
/** 判断当前是否选择演示遥控。 */
bool watch_menu_is_presentation_selected(void);


#ifdef __cplusplus
}
#endif

#endif
