#ifndef WATCH_COMPASS_H
#define WATCH_COMPASS_H

#include <stdbool.h>

#include "lvgl.h"
#include "watch_ui.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 创建指南针页面.
 *
 * @param parent 父对象.
 * @return lv_obj_t* 指南针页面根对象.
 */
lv_obj_t *watch_compass_create(lv_obj_t *parent);

/**
 * @brief 重置指南针页面状态.
 */
void watch_compass_reset(void);

/**
 * @brief 处理指南针页面按键事件.
 *
 * @param key 按键值.
 */
void watch_compass_on_key(watch_key_t key);

/**
 * @brief 判断指南针页面是否请求返回上级菜单.
 *
 * @return true 请求返回.
 * @return false 不请求返回.
 */
bool watch_compass_wants_back(void);

/**
 * @brief 销毁指南针页面所有组件.
 */
void watch_compass_destroy(void);

#ifdef __cplusplus
}
#endif

#endif
