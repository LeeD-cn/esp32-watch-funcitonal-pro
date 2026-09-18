#ifndef WATCH_FOCUS_H
#define WATCH_FOCUS_H

#include <stdbool.h>
#include "lvgl.h"
#include "watch_keys.h"

lv_obj_t *watch_focus_create(lv_obj_t *parent);
void watch_focus_reset(void);
void watch_focus_on_key(watch_key_t key);
bool watch_focus_wants_back(void);
void watch_focus_destroy(void);

#endif
