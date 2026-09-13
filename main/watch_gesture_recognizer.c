#include "watch_gesture_recognizer.h"

#include <math.h>
#include <string.h>

/* Values come from left-wrist, face-outward captures. Keep the Python replay
 * constants in tools/gesture_replay.py aligned when tuning them. */
#define READY_MIN_US              150000
#define ARM_GRACE_US              450000
#define EVENT_END_US               80000
#define EVENT_MAX_US              900000
#define LOCK_RELEASE_US           500000
#define EVENT_START_DPS            100.0f
#define EVENT_END_DPS               60.0f
#define MIN_PEAK_DPS               240.0f
#define MIN_EXCESS_ANGLE_DEG        12.0f

static bool sample_is_ready_pose(const watch_bmi270_motion_sample_t *sample)
{
    float ax = sample->accel[0] * (1000.0f / 8192.0f);
    float ay = sample->accel[1] * (1000.0f / 8192.0f);
    float az = sample->accel[2] * (1000.0f / 8192.0f);
    float gx = sample->gyro[0] * (1000.0f / 32768.0f);
    float gy = sample->gyro[1] * (1000.0f / 32768.0f);
    float gz = sample->gyro[2] * (1000.0f / 32768.0f);
    float accel_sq = ax * ax + ay * ay + az * az;
    float gyro_sq = gx * gx + gy * gy + gz * gz;
    return accel_sq >= 750.0f * 750.0f && accel_sq <= 1250.0f * 1250.0f &&
           ax >= 500.0f && ax <= 1250.0f && ay >= -700.0f && ay <= 100.0f &&
           az >= -400.0f && az <= 850.0f && gyro_sq <= 55.0f * 55.0f;
}

void watch_gesture_recognizer_reset(watch_gesture_recognizer_t *recognizer)
{
    if(recognizer == NULL) return;
    memset(recognizer, 0, sizeof(*recognizer));
    recognizer->phase = WATCH_GESTURE_WAITING;
    recognizer->since_ready_us = ARM_GRACE_US + 1;
}

bool watch_gesture_recognizer_ready(const watch_gesture_recognizer_t *recognizer)
{
    return recognizer != NULL && recognizer->phase == WATCH_GESTURE_ARMED;
}

watch_gesture_event_t watch_gesture_recognizer_update(
    watch_gesture_recognizer_t *r, const watch_bmi270_motion_sample_t *sample)
{
    if(r == NULL || sample == NULL) return WATCH_GESTURE_NONE;
    int64_t dt_us = r->last_timestamp_us == 0 ? 10000 :
                    sample->timestamp_us - r->last_timestamp_us;
    r->last_timestamp_us = sample->timestamp_us;
    if(dt_us < 0) dt_us = 0;
    if(dt_us > 30000) dt_us = 30000;
    bool pose = sample_is_ready_pose(sample);
    float gy = sample->gyro[1] * (1000.0f / 32768.0f);

    if(r->phase == WATCH_GESTURE_WAITING || r->phase == WATCH_GESTURE_ARMED) {
        if(pose) {
            r->stable_us += dt_us;
            r->since_ready_us = 0;
            if(r->stable_us >= READY_MIN_US) r->phase = WATCH_GESTURE_ARMED;
        } else {
            r->stable_us = 0;
            r->since_ready_us += dt_us;
        }
        if(r->phase == WATCH_GESTURE_ARMED && r->since_ready_us <= ARM_GRACE_US &&
           fabsf(gy) >= EVENT_START_DPS) {
            r->phase = WATCH_GESTURE_TRACKING;
            r->direction = gy > 0 ? 1 : -1;
            r->peak_dps = fabsf(gy);
            r->excess_angle_deg = (fabsf(gy) - EVENT_START_DPS) * (dt_us / 1000000.0f);
            r->event_us = dt_us;
            r->quiet_us = 0;
        } else if(r->phase == WATCH_GESTURE_ARMED && r->since_ready_us > ARM_GRACE_US) {
            r->phase = WATCH_GESTURE_WAITING;
        }
    } else if(r->phase == WATCH_GESTURE_TRACKING) {
        float directed = r->direction * gy;
        r->event_us += dt_us;
        if(directed > r->peak_dps) r->peak_dps = directed;
        if(directed > EVENT_START_DPS) {
            r->excess_angle_deg += (directed - EVENT_START_DPS) * (dt_us / 1000000.0f);
        }
        r->quiet_us = directed < EVENT_END_DPS ? r->quiet_us + dt_us : 0;
        if(r->peak_dps >= MIN_PEAK_DPS &&
           r->excess_angle_deg >= MIN_EXCESS_ANGLE_DEG) {
            r->phase = WATCH_GESTURE_LOCKED;
            r->stable_us = 0;
            return r->direction > 0 ? WATCH_GESTURE_RIGHT : WATCH_GESTURE_LEFT;
        }
        if(r->quiet_us >= EVENT_END_US || r->event_us >= EVENT_MAX_US) {
            r->phase = WATCH_GESTURE_WAITING;
            r->stable_us = 0;
        }
    } else {
        /* Endpoint stopping is shorter than this; release after the arm has
         * actually left the chest pose and settled elsewhere. */
        if(!pose) {
            r->stable_us += dt_us;
            if(r->stable_us >= LOCK_RELEASE_US) {
                r->phase = WATCH_GESTURE_WAITING;
                r->stable_us = 0;
            }
        } else {
            r->stable_us = 0;
        }
    }
    return WATCH_GESTURE_NONE;
}
