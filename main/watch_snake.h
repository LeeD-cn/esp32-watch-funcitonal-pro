/**
 * @file watch_snake.h
 * @brief 轻量贪吃蛇游戏页面接口。
 */
#ifndef WATCH_SNAKE_H
#define WATCH_SNAKE_H

#include <stdbool.h>

#include "lvgl.h"
#include "watch_keys.h"

#ifdef __cplusplus
extern "C" {
#endif

lv_obj_t *watch_snake_create(lv_obj_t *parent);
void watch_snake_reset(void);
void watch_snake_start(void);
void watch_snake_on_key(watch_key_t key);
bool watch_snake_wants_back(void);
void watch_snake_destroy(void);

#ifdef __cplusplus
}
#endif

#endif
