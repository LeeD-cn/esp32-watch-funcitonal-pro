#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    uint32_t date_key;
    uint32_t total;
    uint32_t last_sensor;
    bool initialized;
} watch_step_accumulator_t;

void watch_step_accumulator_init(watch_step_accumulator_t *state,
                                 uint32_t saved_date,
                                 uint32_t saved_total,
                                 uint32_t saved_sensor,
                                 uint32_t current_date,
                                 uint32_t current_sensor);

bool watch_step_accumulator_update(watch_step_accumulator_t *state,
                                   uint32_t current_date,
                                   uint32_t current_sensor);
