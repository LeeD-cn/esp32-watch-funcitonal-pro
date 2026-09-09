/**
 * @file watch_e_card.h
 * @brief 设备信息页面接口；为减少路由改动暂时保留原文件名和函数名。
 */
#ifndef WATCH_E_CARD_H
#define WATCH_E_CARD_H

#include <stdbool.h>

#include "lvgl.h"
#include "watch_ui.h"

/**
 * @brief 创建设备信息页面。
 *
 * @param parent LVGL 父对象。
 * @return 页面根对象，失败时返回 NULL。
 */
lv_obj_t *watch_e_card_create(lv_obj_t *parent);
/**
 * @brief 重置设备信息页面状态。
 */
void watch_e_card_reset(void);
/**
 * @brief 处理设备信息页面按键事件。
 *
 * @param key 按键事件。
 */
void watch_e_card_on_key(watch_key_t key);
/**
 * @brief 判断设备信息页面是否请求返回。
 *
 * @return true 请求返回；false 继续停留。
 */
bool watch_e_card_wants_back(void);
/**
 * @brief 销毁设备信息页面及其 LVGL 对象。
 */
void watch_e_card_destroy(void);

#endif
