#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

typedef struct {
    uint32_t today;
    uint32_t sensor_total;
    uint32_t date_key;
    uint32_t revision;
    esp_err_t error;
    bool running;
} watch_steps_snapshot_t;

#define WATCH_STEPS_HISTORY_DAYS 31

typedef struct {
    uint32_t date_key;
    uint32_t steps;
} watch_step_day_t;

esp_err_t watch_steps_start(void);
void watch_steps_get_snapshot(watch_steps_snapshot_t *snapshot);
/** Copy newest-first daily records and return the number copied. */
size_t watch_steps_get_history(watch_step_day_t *entries, size_t capacity);
esp_err_t watch_steps_flush(void);
void watch_steps_set_suspended(bool suspended);
