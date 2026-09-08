/**
 * @file watch_game.h
 * @brief 游戏中心页面接口。
 */
#ifndef WATCH_GAME_H
#define WATCH_GAME_H

#include <stdbool.h>
#include "lvgl.h"
#include "watch_keys.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 创建游戏中心页面。
 *
 * @param parent LVGL 父对象。
 * @return 页面根对象，失败时返回 NULL。
 */
lv_obj_t *watch_game_create(lv_obj_t *parent);
/**
 * @brief 重置游戏中心到菜单状态。
 */
void watch_game_reset(void);
/**
 * @brief 分发游戏中心及子页面的按键事件。
 *
 * @param key 按键事件。
 */
void watch_game_on_key(watch_key_t key);
/**
 * @brief 判断游戏中心是否请求返回上级页面。
 *
 * @return true 请求返回；false 继续停留。
 */
bool watch_game_wants_back(void);
/**
 * @brief 清理游戏中心内部子页面对象。
 */
void watch_game_cleanup(void);
/**
 * @brief 销毁游戏中心页面及所有子页面。
 */
void watch_game_destroy(void);

#ifdef __cplusplus
}
#endif

#endif
