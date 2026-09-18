/**
 * @file watch_tomato_clock.h
 * @brief 番茄钟页面对外接口。
 */

#ifndef WATCH_TOMATO_CLOCK_H
#define WATCH_TOMATO_CLOCK_H

#include <stdbool.h>
#include "lvgl.h"
#include "watch_keys.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 创建番茄钟页面。
 *
 * @param parent 父对象。
 * @return lv_obj_t* 页面根对象，失败返回 NULL。
 */
lv_obj_t *watch_tomato_clock_create(lv_obj_t *parent);
/**
 * @brief 重置番茄钟页面状态。
 */
void watch_tomato_clock_reset(void);
/**
 * @brief 处理番茄钟页面按键事件。
 *
 * @param key 按键值。
 */
void watch_tomato_clock_on_key(watch_key_t key);
/**
 * @brief 判断番茄钟页面是否请求返回。
 *
 * @return true 请求返回。
 */
bool watch_tomato_clock_wants_back(void);
/**
 * @brief 销毁番茄钟页面及相关定时器。
 */
void watch_tomato_clock_destroy(void);

#ifdef __cplusplus
}
#endif

#endif
