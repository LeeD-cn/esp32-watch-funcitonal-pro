/**
 * @file watch_e_card.h
 * @brief Bilibili 电子名片页面接口。
 */
#ifndef WATCH_E_CARD_H
#define WATCH_E_CARD_H

#include <stdbool.h>

#include "lvgl.h"
#include "watch_ui.h"

/**
 * @brief 创建 Bilibili 电子名片页面。
 *
 * @param parent LVGL 父对象。
 * @return 页面根对象，失败时返回 NULL。
 */
lv_obj_t *watch_e_card_create(lv_obj_t *parent);
/**
 * @brief 重置电子名片页面状态。
 */
void watch_e_card_reset(void);
/**
 * @brief 处理电子名片页面按键事件。
 *
 * @param key 按键事件。
 */
void watch_e_card_on_key(watch_key_t key);
/**
 * @brief 判断电子名片页面是否请求返回。
 *
 * @return true 请求返回；false 继续停留。
 */
bool watch_e_card_wants_back(void);
/**
 * @brief 销毁电子名片页面及其 LVGL 对象。
 */
void watch_e_card_destroy(void);

#endif
