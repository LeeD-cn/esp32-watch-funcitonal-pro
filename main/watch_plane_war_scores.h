/**
 * @file watch_plane_war_scores.h
 * @brief 飞机大战历史分数页面接口。
 */
#ifndef WATCH_PLANE_WAR_SCORES_H
#define WATCH_PLANE_WAR_SCORES_H

#include <stdbool.h>

#include "watch_game.h"

#define WATCH_PLANE_WAR_SCORES_COUNT 3

/**
 * @brief 创建飞机大战历史分数页面。
 *
 * @param parent LVGL 父对象。
 * @return 页面根对象，失败时返回 NULL。
 */
lv_obj_t *watch_plane_war_scores_create(lv_obj_t *parent);
/**
 * @brief 重置历史分数页面并刷新分数显示。
 */
void watch_plane_war_scores_reset(void);
/**
 * @brief 销毁历史分数页面及其 LVGL 对象。
 */
void watch_plane_war_scores_destroy(void);
/**
 * @brief 处理历史分数页面按键事件。
 *
 * @param key 按键事件。
 */
void watch_plane_war_scores_on_key(watch_key_t key);
/**
 * @brief 判断历史分数页面是否请求返回。
 *
 * @return true 请求返回。
 */
bool watch_plane_war_scores_wants_back(void);

/**
 * @brief 读取历史最高分列表。
 *
 * @param scores 输出数组，长度至少为 WATCH_PLANE_WAR_SCORES_COUNT。
 */
void watch_plane_war_scores_load(int scores[WATCH_PLANE_WAR_SCORES_COUNT]);
/**
 * @brief 提交新分数并更新历史最高分。
 *
 * @param score 新游戏分数，负数会按 0 处理。
 */
void watch_plane_war_scores_submit_score(int score);

#endif
