#include "watch_imu_capture.h"
#include "watch_bmi270.h"
#include <inttypes.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/* Single serial owner. Gate and active share one critical section so KEY4
 * cannot race a new capture between cancellation and entering sleep. */
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static bool s_active;
static bool s_suspended;
static atomic_bool s_cancel;

bool watch_imu_capture_active(void)
{
    portENTER_CRITICAL(&s_lock);
    bool active = s_active;
    portEXIT_CRITICAL(&s_lock);
    return active;
}

void watch_imu_capture_suspend(bool suspend)
{
    portENTER_CRITICAL(&s_lock);
    s_suspended = suspend;
    if(suspend) atomic_store(&s_cancel, true);
    portEXIT_CRITICAL(&s_lock);
    if(suspend) {
        while(watch_imu_capture_active()) vTaskDelay(1);
    }
}

static bool wait_until(int64_t deadline)
{
    while(esp_timer_get_time() < deadline) {
        if(atomic_load(&s_cancel)) return false;
        vTaskDelay(1);
    }
    return !atomic_load(&s_cancel);
}

void watch_imu_capture_run(unsigned seconds, unsigned delay_ms, bool (*write_line)(const char *))
{
    if(seconds < 1 || seconds > 15 || delay_ms > 10000) {
        write_line("{\"imu\":\"error\",\"reason\":\"invalid_duration\"}");
        return;
    }
    portENTER_CRITICAL(&s_lock);
    bool allowed = !s_active && !s_suspended;
    if(allowed) {
        s_active = true;
        atomic_store(&s_cancel, false);
    }
    portEXIT_CRITICAL(&s_lock);
    if(!allowed) {
        write_line("{\"imu\":\"error\",\"reason\":\"busy_or_asleep\"}");
        return;
    }

    /* Bounded RAM only: acquisition never waits for serial output or writes Flash. */
    unsigned capacity = seconds * 110 + 8;
    watch_bmi270_motion_sample_t *samples = calloc(capacity, sizeof(*samples));
    unsigned count = 0, not_ready = 0, errors = 0;
    esp_err_t result = samples ? watch_bmi270_motion_begin() : ESP_ERR_NO_MEM;
    esp_err_t restore = ESP_OK;
    char line[240];
    if(result == ESP_OK) {
        if(!write_line("{\"imu\":\"preparing\",\"version\":1,\"odr_hz\":100,\"accel_range_g\":4,\"gyro_range_dps\":1000}"))
            atomic_store(&s_cancel, true);
        /* Gyro startup settles before countdown; discard old data before recording. */
        bool ready = wait_until(esp_timer_get_time() + 250000 + (int64_t)delay_ms * 1000);
        watch_bmi270_motion_sample_t discarded;
        if(ready) (void)watch_bmi270_motion_read(&discarded);
        if(ready && !write_line("{\"imu\":\"recording\"}")) atomic_store(&s_cancel, true);
        int64_t deadline = esp_timer_get_time() + (int64_t)seconds * 1000000;
        while(ready && !atomic_load(&s_cancel) && esp_timer_get_time() < deadline) {
            watch_bmi270_motion_sample_t sample;
            esp_err_t err = watch_bmi270_motion_read(&sample);
            if(err == ESP_OK) {
                if(count == capacity) { result = ESP_ERR_NO_MEM; break; }
                samples[count++] = sample;
            } else if(err == ESP_ERR_NOT_FOUND) {
                ++not_ready;
            } else {
                ++errors;
                result = err;
                break;
            }
            vTaskDelay(1); /* Current tick is 10 ms. Actual timing is measured, not assumed. */
        }
        restore = watch_bmi270_motion_end();
        /* Keep active until export completes so automatic sleep cannot cut USB. */
        if(!atomic_load(&s_cancel)) {
            if(!write_line("{\"imu\":\"exporting\"}")) atomic_store(&s_cancel, true);
            for(unsigned i = 0; i < count && !atomic_load(&s_cancel); ++i) {
                const watch_bmi270_motion_sample_t *s = &samples[i];
                snprintf(line, sizeof(line),
                         "{\"imu\":\"sample\",\"v\":[%u,%" PRId64 ",%" PRIu32 ",%d,%d,%d,%d,%d,%d]}",
                         i, s->timestamp_us, s->sensor_ticks,
                         s->accel[0], s->accel[1], s->accel[2], s->gyro[0], s->gyro[1], s->gyro[2]);
                if(!write_line(line)) {
                    atomic_store(&s_cancel, true);
                } else {
                    /* Native USB accepts data into a small TX ring before the PC
                     * has consumed it. Pace the offline export so a valid write
                     * cannot later become a missing frame at the receiver. */
                    vTaskDelay(1);
                }
            }
        }
    }
    snprintf(line, sizeof(line),
             "{\"imu\":\"done\",\"count\":%u,\"not_ready\":%u,\"errors\":%u,\"result\":%d,\"restore\":%d,\"cancelled\":%s}",
             count, not_ready, errors, result, restore, atomic_load(&s_cancel) ? "true" : "false");
    write_line(line);
    free(samples);
    portENTER_CRITICAL(&s_lock);
    s_active = false;
    portEXIT_CRITICAL(&s_lock);
}
