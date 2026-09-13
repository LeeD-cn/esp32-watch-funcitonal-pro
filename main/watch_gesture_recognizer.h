#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "watch_bmi270.h"

typedef enum {
    WATCH_GESTURE_NONE = 0,
    WATCH_GESTURE_LEFT,
    WATCH_GESTURE_RIGHT,
} watch_gesture_event_t;

typedef enum {
    WATCH_GESTURE_WAITING = 0,
    WATCH_GESTURE_ARMED,
    WATCH_GESTURE_TRACKING,
    WATCH_GESTURE_LOCKED,
} watch_gesture_phase_t;

typedef struct {
    watch_gesture_phase_t phase;
    int64_t last_timestamp_us;
    int64_t stable_us;
    int64_t since_ready_us;
    int64_t event_us;
    int64_t quiet_us;
    int direction;
    float peak_dps;
    float excess_angle_deg;
} watch_gesture_recognizer_t;

void watch_gesture_recognizer_reset(watch_gesture_recognizer_t *recognizer);
watch_gesture_event_t watch_gesture_recognizer_update(
    watch_gesture_recognizer_t *recognizer,
    const watch_bmi270_motion_sample_t *sample);
bool watch_gesture_recognizer_ready(const watch_gesture_recognizer_t *recognizer);
