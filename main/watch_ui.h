/**
 * @file watch_ui.h
 * @brief 手表 UI 页面管理接口。
 */

#ifndef WATCH_UI_H
#define WATCH_UI_H

#include <stdbool.h>
#include "watch_keys.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 创建 UI 根屏幕和默认页面。
 */
void watch_ui_create(void);
/**
 * @brief 将按键事件分发给当前页面。
 *
 * @param key 按键值。
 */
void watch_ui_on_key(watch_key_t key);

/** 演示遥控页面可见时保持亮屏和 Wi-Fi，KEY4 主动息屏仍由系统处理。 */
bool watch_ui_should_keep_awake(void);

#ifdef __cplusplus
}
#endif

#endif
