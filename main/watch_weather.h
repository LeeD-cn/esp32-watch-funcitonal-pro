/**
 * @file watch_weather.h
 * @brief 天气页面对外接口。
 */

#ifndef WATCH_WEATHER_H
#define WATCH_WEATHER_H

#include <stdbool.h>
#include "lvgl.h"
#include "watch_keys.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 创建天气页面。
 *
 * @param parent 父对象。
 * @return lv_obj_t* 页面根对象，失败返回 NULL。
 */
lv_obj_t *watch_weather_create(lv_obj_t *parent);
/**
 * @brief 处理天气页面按键事件。
 *
 * @param key 按键值。
 */
void watch_weather_on_key(watch_key_t key);
/**
 * @brief 判断天气页面是否请求返回。
 *
 * @return true 请求返回。
 */
bool watch_weather_wants_back(void);
/**
 * @brief 重置天气页面状态。
 */
void watch_weather_reset(void);
/**
 * @brief 销毁天气页面及相关定时器。
 */
void watch_weather_destroy(void);

#ifdef __cplusplus
}
#endif

#endif /* WATCH_WEATHER_H */
