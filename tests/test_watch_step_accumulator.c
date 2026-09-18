#include <assert.h>
#include <stdint.h>
#include <stdio.h>

#include "watch_step_accumulator.h"

int main(void)
{
    watch_step_accumulator_t state;

    watch_step_accumulator_init(&state, 20260915, 1200, 800, 20260915, 830);
    assert(state.total == 1230);
    assert(watch_step_accumulator_update(&state, 20260915, 835));
    assert(state.total == 1235);
    assert(!watch_step_accumulator_update(&state, 20260915, 835));

    /* Sensor reset: the new raw value is the count accumulated after reset. */
    assert(watch_step_accumulator_update(&state, 20260915, 3));
    assert(state.total == 1238);

    /* A new local calendar day starts from zero without resetting the sensor. */
    assert(watch_step_accumulator_update(&state, 20260916, 10));
    assert(state.total == 0);
    assert(watch_step_accumulator_update(&state, 20260916, 12));
    assert(state.total == 2);

    watch_step_accumulator_init(&state, 20260915, 999, 300, 20260916, 400);
    assert(state.total == 0);

    puts("watch step accumulator tests passed");
    return 0;
}
