/**
 * @file watch_plane_war.h
 * @brief 飞机大战游戏页面接口。
 */
#ifndef WATCH_PLANE_WAR_H
#define WATCH_PLANE_WAR_H

#include <stdbool.h>
#include "lvgl.h"
#include "watch_keys.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 创建飞机大战游戏页面。
 *
 * @param parent LVGL 父对象。
 * @return 页面根对象，失败时返回 NULL。
 */
lv_obj_t *watch_plane_war_create(lv_obj_t *parent);

/**
 * @brief 重置飞机大战游戏状态。
 */
void watch_plane_war_reset(void);
/**
 * @brief 开始或恢复游戏定时器。
 */
void watch_plane_war_start(void);
/**
 * @brief 暂停游戏定时器和运行状态。
 */
void watch_plane_war_stop(void);

/**
 * @brief 处理飞机大战页面按键事件。
 *
 * @param key 按键事件。
 */
void watch_plane_war_on_key(watch_key_t key);
/**
 * @brief 判断飞机大战页面是否请求返回。
 *
 * @return true 请求返回；false 继续游戏。
 */
bool watch_plane_war_wants_back(void);
/**
 * @brief 销毁飞机大战页面、定时器和游戏对象。
 */
void watch_plane_war_destroy(void);
/**
 * @brief 游戏结束回调类型。
 */
typedef void (*watch_plane_war_game_over_cb_t)(void);
/**
 * @brief 设置游戏结束回调。
 *
 * @param cb 游戏结束时调用的回调，传 NULL 可取消。
 */
void watch_plane_war_set_game_over_cb(watch_plane_war_game_over_cb_t cb);
/**
 * @brief 判断当前游戏是否已经结束。
 *
 * @return true 已结束；false 未结束。
 */
bool watch_plane_war_is_game_over(void);
void watch_plane_war_set_game_over_cb(watch_plane_war_game_over_cb_t cb);
bool watch_plane_war_is_game_over(void);
/**
 * @brief 获取当前游戏分数。
 *
 * @return 当前分数。
 */
int watch_plane_war_get_score(void);

#ifdef __cplusplus
}
#endif

#endif
