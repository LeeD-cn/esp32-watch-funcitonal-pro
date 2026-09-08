/**
 * @file watch_settings.h
 * @brief 设置页面对外接口。
 */

#pragma once

#include <stdbool.h>
#include "lvgl.h"
#include "watch_keys.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 创建设置页面。
 *
 * @param parent 父对象。
 * @return lv_obj_t* 设置页根对象，失败返回 NULL。
 */
lv_obj_t *watch_settings_create(lv_obj_t *parent);
/**
 * @brief 重置设置页选择状态和返回标志。
 */
void watch_settings_reset(void);
/**
 * @brief 处理设置页按键事件。
 *
 * @param key 按键值。
 */
void watch_settings_on_key(watch_key_t key);
/**
 * @brief 判断设置页是否请求返回。
 *
 * @return true 请求返回。
 */
bool watch_settings_wants_back(void);
/**
 * @brief 销毁设置页及其子对象。
 */
void watch_settings_destroy(void);

#ifdef __cplusplus
}
#endif
