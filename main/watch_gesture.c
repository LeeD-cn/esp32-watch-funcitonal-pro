#include "watch_gesture.h"

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "watch_bmi270.h"

#define GESTURE_TASK_STACK 4096
#define GESTURE_TASK_PRIO  4
#define GESTURE_QUEUE_LEN  2

static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static TaskHandle_t s_task;
static bool s_starting;
static bool s_stop_requested;
static watch_gesture_snapshot_t s_snapshot;
static StaticQueue_t s_queue_struct;
static uint8_t s_queue_storage[GESTURE_QUEUE_LEN * sizeof(watch_gesture_event_t)];
static QueueHandle_t s_queue;

static void update_runtime(bool running, bool ready, esp_err_t error)
{
    portENTER_CRITICAL(&s_lock);
    if(s_snapshot.running != running || s_snapshot.ready != ready || s_snapshot.error != error) {
        s_snapshot.running = running;
        s_snapshot.ready = ready;
        s_snapshot.error = error;
        s_snapshot.revision++;
    }
    portEXIT_CRITICAL(&s_lock);
}

static bool should_stop(void)
{
    portENTER_CRITICAL(&s_lock);
    bool stop = s_stop_requested;
    portEXIT_CRITICAL(&s_lock);
    return stop;
}

static void gesture_task(void *arg)
{
    (void)arg;
    for(;;) {
        portENTER_CRITICAL(&s_lock);
        bool starting = s_starting;
        portEXIT_CRITICAL(&s_lock);
        if(!starting) break;
        vTaskDelay(1);
    }
    esp_err_t err = watch_bmi270_motion_begin();
    if(err == ESP_OK) {
        vTaskDelay(pdMS_TO_TICKS(250));
        watch_gesture_recognizer_t recognizer;
        watch_gesture_recognizer_reset(&recognizer);
        update_runtime(true, false, ESP_OK);
        while(!should_stop()) {
            watch_bmi270_motion_sample_t sample;
            err = watch_bmi270_motion_read(&sample);
            if(err == ESP_OK) {
                watch_gesture_event_t event =
                    watch_gesture_recognizer_update(&recognizer, &sample);
                update_runtime(true, watch_gesture_recognizer_ready(&recognizer), ESP_OK);
                if(event != WATCH_GESTURE_NONE) (void)xQueueSend(s_queue, &event, 0);
            } else if(err != ESP_ERR_NOT_FOUND) {
                break;
            }
            vTaskDelay(1);
        }
        esp_err_t restore = watch_bmi270_motion_end();
        if(restore != ESP_OK) err = restore;
    }

    xQueueReset(s_queue);
    portENTER_CRITICAL(&s_lock);
    s_snapshot.running = false;
    s_snapshot.ready = false;
    if(err != ESP_OK && !s_stop_requested) {
        s_snapshot.enabled = false;
        s_snapshot.error = err;
    }
    s_snapshot.revision++;
    s_task = NULL;
    portEXIT_CRITICAL(&s_lock);
    vTaskDelete(NULL);
}

esp_err_t watch_gesture_set_enabled(bool enabled)
{
    if(!enabled) {
        watch_gesture_stop();
        return ESP_OK;
    }
    if(s_queue == NULL) {
        s_queue = xQueueCreateStatic(GESTURE_QUEUE_LEN, sizeof(watch_gesture_event_t),
                                     s_queue_storage, &s_queue_struct);
    }
    xQueueReset(s_queue);
    portENTER_CRITICAL(&s_lock);
    if(s_task != NULL || s_starting || s_snapshot.enabled) {
        portEXIT_CRITICAL(&s_lock);
        return ESP_OK;
    }
    s_stop_requested = false;
    s_snapshot.enabled = true;
    s_snapshot.running = false;
    s_snapshot.ready = false;
    s_snapshot.error = ESP_OK;
    s_snapshot.revision++;
    s_starting = true;
    portEXIT_CRITICAL(&s_lock);
    TaskHandle_t task = NULL;
    BaseType_t created = xTaskCreate(gesture_task, "watch_gesture", GESTURE_TASK_STACK,
                                     NULL, GESTURE_TASK_PRIO, &task);
    portENTER_CRITICAL(&s_lock);
    s_task = created == pdPASS ? task : NULL;
    s_starting = false;
    if(created != pdPASS) {
        s_snapshot.enabled = false;
        s_snapshot.error = ESP_ERR_NO_MEM;
        s_snapshot.revision++;
    }
    portEXIT_CRITICAL(&s_lock);
    return created == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}

void watch_gesture_stop(void)
{
    portENTER_CRITICAL(&s_lock);
    s_stop_requested = true;
    s_snapshot.enabled = false;
    s_snapshot.ready = false;
    s_snapshot.revision++;
    bool active = s_starting || s_task != NULL;
    portEXIT_CRITICAL(&s_lock);
    for(unsigned i = 0; active && i < 700; ++i) {
        vTaskDelay(1);
        portENTER_CRITICAL(&s_lock);
        active = s_starting || s_task != NULL;
        portEXIT_CRITICAL(&s_lock);
    }
}

void watch_gesture_get_snapshot(watch_gesture_snapshot_t *snapshot)
{
    if(snapshot == NULL) return;
    portENTER_CRITICAL(&s_lock);
    *snapshot = s_snapshot;
    portEXIT_CRITICAL(&s_lock);
}

bool watch_gesture_pop_event(watch_gesture_event_t *event)
{
    return event != NULL && s_queue != NULL && xQueueReceive(s_queue, event, 0) == pdTRUE;
}
