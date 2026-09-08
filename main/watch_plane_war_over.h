/**
 * @file watch_plane_war_over.h
 * @brief 飞机大战结束页面接口。
 */
#ifndef WATCH_PLANE_WAR_OVER_H
#define WATCH_PLANE_WAR_OVER_H

#include <stdbool.h>

#include "watch_game.h"

/**
 * @brief 创建飞机大战结束页面。
 *
 * @param parent LVGL 父对象。
 * @return 页面根对象，失败时返回 NULL。
 */
lv_obj_t *watch_plane_war_over_create(lv_obj_t *parent);
/**
 * @brief 重置结束页面的选中项和请求状态。
 */
void watch_plane_war_over_reset(void);
/**
 * @brief 销毁结束页面及其 LVGL 对象。
 */
void watch_plane_war_over_destroy(void);
/**
 * @brief 处理结束页面按键事件。
 *
 * @param key 按键事件。
 */
void watch_plane_war_over_on_key(watch_key_t key);
/**
 * @brief 判断是否请求返回游戏菜单。
 *
 * @return true 请求返回。
 */
bool watch_plane_war_over_wants_back(void);
/**
 * @brief 判断是否请求重新开始游戏。
 *
 * @return true 请求重玩。
 */
bool watch_plane_war_over_wants_play_again(void);
/**
 * @brief 判断是否请求进入历史分数页面。
 *
 * @return true 请求查看分数。
 */
bool watch_plane_war_over_wants_scores(void);

#endif
