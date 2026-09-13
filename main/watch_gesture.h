#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "watch_gesture_recognizer.h"

typedef struct {
    bool enabled;
    bool running;
    bool ready;
    esp_err_t error;
    uint32_t revision;
} watch_gesture_snapshot_t;

esp_err_t watch_gesture_set_enabled(bool enabled);
void watch_gesture_stop(void);
void watch_gesture_get_snapshot(watch_gesture_snapshot_t *snapshot);
bool watch_gesture_pop_event(watch_gesture_event_t *event);
