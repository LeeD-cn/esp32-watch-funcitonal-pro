/**
 * @file watch_presentation.h
 * @brief 演示遥控页面。
 */
#ifndef WATCH_PRESENTATION_H
#define WATCH_PRESENTATION_H

#include <stdbool.h>
#include "lvgl.h"
#include "watch_keys.h"

#ifdef __cplusplus
extern "C" {
#endif

lv_obj_t *watch_presentation_create(lv_obj_t *parent);
void watch_presentation_reset(void);
void watch_presentation_on_key(watch_key_t key);
bool watch_presentation_wants_back(void);
void watch_presentation_destroy(void);

#ifdef __cplusplus
}
#endif

#endif
