#pragma once

#include <stdbool.h>
#include "lvgl.h"
#include "watch_keys.h"

lv_obj_t *watch_pedometer_create(lv_obj_t *parent);
void watch_pedometer_reset(void);
void watch_pedometer_on_key(watch_key_t key);
bool watch_pedometer_wants_back(void);
void watch_pedometer_destroy(void);
