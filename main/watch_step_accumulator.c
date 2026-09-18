#include "watch_step_accumulator.h"

#include <limits.h>

static uint32_t add_saturated(uint32_t value, uint32_t delta)
{
    return delta > UINT32_MAX - value ? UINT32_MAX : value + delta;
}

void watch_step_accumulator_init(watch_step_accumulator_t *state,
                                 uint32_t saved_date,
                                 uint32_t saved_total,
                                 uint32_t saved_sensor,
                                 uint32_t current_date,
                                 uint32_t current_sensor)
{
    if(state == NULL) return;
    state->date_key = current_date;
    state->total = saved_date == current_date ? saved_total : 0;
    if(saved_date == current_date) {
        uint32_t missed = current_sensor >= saved_sensor ?
                          current_sensor - saved_sensor : current_sensor;
        state->total = add_saturated(state->total, missed);
    }
    state->last_sensor = current_sensor;
    state->initialized = true;
}

bool watch_step_accumulator_update(watch_step_accumulator_t *state,
                                   uint32_t current_date,
                                   uint32_t current_sensor)
{
    if(state == NULL || !state->initialized) return false;
    if(current_date != 0 && current_date != state->date_key) {
        state->date_key = current_date;
        state->total = 0;
        state->last_sensor = current_sensor;
        return true;
    }

    uint32_t delta = current_sensor >= state->last_sensor ?
                     current_sensor - state->last_sensor : current_sensor;
    state->last_sensor = current_sensor;
    if(delta == 0) return false;
    state->total = add_saturated(state->total, delta);
    return true;
}
