#include "watch_steps.h"

#include <string.h>
#include <time.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"
#include "watch_bmi270.h"
#include "watch_step_accumulator.h"

#define STEPS_TASK_STACK        4096
#define STEPS_TASK_PRIORITY     2
#define STEPS_POLL_MS           1000
#define STEPS_SAVE_INTERVAL_MS  (5 * 60 * 1000)
#define STEPS_SAVE_DELTA        100
#define STEPS_NVS_NAMESPACE     "watch_steps"
#define STEPS_HISTORY_KEY       "history"
#define STEPS_HISTORY_MAGIC     0x53544550U
#define STEPS_HISTORY_VERSION   1U

static const char *TAG = "watch_steps";
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static TaskHandle_t s_task;
static bool s_starting;
static bool s_suspend_requested;
static bool s_suspended;
static watch_steps_snapshot_t s_snapshot;

typedef struct {
    uint32_t magic;
    uint16_t version;
    uint16_t count;
    watch_step_day_t days[WATCH_STEPS_HISTORY_DAYS];
} steps_history_t;

static steps_history_t s_history;
/* Task-owned working copy lives in BSS so NVS/history setup cannot exhaust the task stack. */
static steps_history_t s_working_history;

static uint32_t current_date_key(void)
{
    time_t now;
    struct tm local;
    time(&now);
    localtime_r(&now, &local);
    if(local.tm_year + 1900 < 2024) return 0;
    return (uint32_t)(local.tm_year + 1900) * 10000U +
           (uint32_t)(local.tm_mon + 1) * 100U + (uint32_t)local.tm_mday;
}

static void load_saved(uint32_t *date, uint32_t *total, uint32_t *sensor)
{
    *date = *total = *sensor = 0;
    nvs_handle_t nvs;
    if(nvs_open(STEPS_NVS_NAMESPACE, NVS_READONLY, &nvs) != ESP_OK) return;
    (void)nvs_get_u32(nvs, "date", date);
    (void)nvs_get_u32(nvs, "total", total);
    (void)nvs_get_u32(nvs, "sensor", sensor);
    nvs_close(nvs);
}

static bool history_upsert(steps_history_t *history, uint32_t date, uint32_t steps)
{
    if(history == NULL || date == 0) return false;
    size_t found = history->count;
    for(size_t i = 0; i < history->count; ++i) {
        if(history->days[i].date_key == date) {
            found = i;
            break;
        }
    }
    size_t count = history->count;
    if(found < count) {
        if(history->days[found].steps == steps) return false;
        memmove(&history->days[found], &history->days[found + 1],
                (count - found - 1) * sizeof(history->days[0]));
        count--;
    }
    size_t insert = 0;
    while(insert < count && history->days[insert].date_key > date) insert++;
    if(insert >= WATCH_STEPS_HISTORY_DAYS) return false;
    size_t new_count = count < WATCH_STEPS_HISTORY_DAYS ? count + 1 : count;
    memmove(&history->days[insert + 1], &history->days[insert],
            (new_count - insert - 1) * sizeof(history->days[0]));
    history->days[insert] = (watch_step_day_t){ .date_key = date, .steps = steps };
    history->count = (uint16_t)new_count;
    return true;
}

static uint32_t next_date_key(uint32_t date)
{
    uint32_t year = date / 10000U;
    uint32_t month = (date / 100U) % 100U;
    uint32_t day = date % 100U;
    static const uint8_t days_per_month[] =
        { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    if(year < 2024 || month < 1 || month > 12 || day < 1) return 0;
    uint32_t days = days_per_month[month - 1];
    bool leap = (year % 4U == 0 && year % 100U != 0) || year % 400U == 0;
    if(month == 2 && leap) days++;
    if(day < days) day++;
    else {
        day = 1;
        if(month < 12) month++;
        else {
            month = 1;
            year++;
        }
    }
    return year * 10000U + month * 100U + day;
}

static void history_fill_gap(steps_history_t *history, uint32_t older, uint32_t newer)
{
    uint32_t date = next_date_key(older);
    for(unsigned guard = 0; date != 0 && date < newer && guard < 366; ++guard) {
        (void)history_upsert(history, date, 0);
        date = next_date_key(date);
    }
}

static void load_history(steps_history_t *history)
{
    memset(history, 0, sizeof(*history));
    nvs_handle_t nvs;
    size_t size = sizeof(*history);
    if(nvs_open(STEPS_NVS_NAMESPACE, NVS_READONLY, &nvs) == ESP_OK) {
        esp_err_t ret = nvs_get_blob(nvs, STEPS_HISTORY_KEY, history, &size);
        nvs_close(nvs);
        if(ret == ESP_OK && size == sizeof(*history) &&
           history->magic == STEPS_HISTORY_MAGIC &&
           history->version == STEPS_HISTORY_VERSION &&
           history->count <= WATCH_STEPS_HISTORY_DAYS) return;
    }
    memset(history, 0, sizeof(*history));
    history->magic = STEPS_HISTORY_MAGIC;
    history->version = STEPS_HISTORY_VERSION;
    /* Product baseline requested by the user: seed 2026-09-08 through 09-14. */
    for(uint32_t date = 20260908; date <= 20260914; ++date)
        (void)history_upsert(history, date, 0);
}

static void publish_history(const steps_history_t *history)
{
    portENTER_CRITICAL(&s_lock);
    s_history = *history;
    portEXIT_CRITICAL(&s_lock);
}

static esp_err_t save_values(uint32_t date, uint32_t total, uint32_t sensor,
                             const steps_history_t *history)
{
    nvs_handle_t nvs = 0;
    esp_err_t ret = nvs_open(STEPS_NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if(ret == ESP_OK) ret = nvs_set_u32(nvs, "date", date);
    if(ret == ESP_OK) ret = nvs_set_u32(nvs, "total", total);
    if(ret == ESP_OK) ret = nvs_set_u32(nvs, "sensor", sensor);
    if(ret == ESP_OK && history != NULL)
        ret = nvs_set_blob(nvs, STEPS_HISTORY_KEY, history, sizeof(*history));
    if(ret == ESP_OK) ret = nvs_commit(nvs);
    if(nvs != 0) nvs_close(nvs);
    return ret;
}

static void publish(const watch_step_accumulator_t *state, esp_err_t error, bool running)
{
    portENTER_CRITICAL(&s_lock);
    bool changed = s_snapshot.today != state->total ||
                   s_snapshot.sensor_total != state->last_sensor ||
                   s_snapshot.date_key != state->date_key ||
                   s_snapshot.error != error || s_snapshot.running != running;
    s_snapshot.today = state->total;
    s_snapshot.sensor_total = state->last_sensor;
    s_snapshot.date_key = state->date_key;
    s_snapshot.error = error;
    s_snapshot.running = running;
    if(changed) s_snapshot.revision++;
    portEXIT_CRITICAL(&s_lock);
}

static void steps_task(void *arg)
{
    (void)arg;
    for(;;) {
        portENTER_CRITICAL(&s_lock);
        bool starting = s_starting;
        portEXIT_CRITICAL(&s_lock);
        if(!starting) break;
        vTaskDelay(1);
    }
    watch_step_accumulator_t state = {0};
    uint32_t saved_date, saved_total, saved_sensor, sensor;
    esp_err_t ret = watch_bmi270_step_counter_enable();
    if(ret == ESP_OK) ret = watch_bmi270_step_counter_read(&sensor);
    if(ret != ESP_OK) {
        publish(&state, ret, false);
        ESP_LOGW(TAG, "BMI270 step counter unavailable: %s", esp_err_to_name(ret));
        portENTER_CRITICAL(&s_lock);
        s_task = NULL;
        portEXIT_CRITICAL(&s_lock);
        vTaskDelete(NULL);
        return;
    }

    load_saved(&saved_date, &saved_total, &saved_sensor);
    steps_history_t *history = &s_working_history;
    load_history(history);
    uint32_t today = current_date_key();
    uint32_t latest_date = saved_date != 0 ? saved_date :
                           (history->count > 0 ? history->days[0].date_key : 0);
    if(saved_date != 0 && saved_date != today)
        (void)history_upsert(history, saved_date, saved_total);
    history_fill_gap(history, latest_date, today);
    watch_step_accumulator_init(&state, saved_date, saved_total, saved_sensor,
                                today, sensor);
    (void)history_upsert(history, state.date_key, state.total);
    publish_history(history);
    publish(&state, ESP_OK, true);
    uint32_t saved_date_key = state.date_key;
    uint32_t saved_at_count = state.total;
    uint32_t last_logged = state.total;
    TickType_t saved_at_tick = xTaskGetTickCount();
    ESP_LOGI(TAG, "hardware step counter started, today=%lu raw=%lu stack_free=%u",
             (unsigned long)state.total, (unsigned long)sensor,
             (unsigned)uxTaskGetStackHighWaterMark(NULL));

    for(;;) {
        (void)ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(STEPS_POLL_MS));
        portENTER_CRITICAL(&s_lock);
        bool suspend = s_suspend_requested;
        if(suspend) s_suspended = true;
        portEXIT_CRITICAL(&s_lock);
        if(suspend) {
            do {
                (void)ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
                portENTER_CRITICAL(&s_lock);
                suspend = s_suspend_requested;
                portEXIT_CRITICAL(&s_lock);
            } while(suspend);
            portENTER_CRITICAL(&s_lock);
            s_suspended = false;
            portEXIT_CRITICAL(&s_lock);
            continue;
        }
        ret = watch_bmi270_step_counter_read(&sensor);
        if(ret != ESP_OK) {
            publish(&state, ret, true);
            continue;
        }
        uint32_t previous_date = state.date_key;
        uint32_t previous_total = state.total;
        bool changed = watch_step_accumulator_update(&state, current_date_key(), sensor);
        if(state.date_key != previous_date) {
            (void)history_upsert(history, previous_date, previous_total);
            history_fill_gap(history, previous_date, state.date_key);
        }
        if(changed) {
            (void)history_upsert(history, state.date_key, state.total);
            publish_history(history);
        }
        publish(&state, ESP_OK, true);
        if(changed && state.total / 10 != last_logged / 10) {
            ESP_LOGI(TAG, "today=%lu raw=%lu", (unsigned long)state.total,
                     (unsigned long)state.last_sensor);
            last_logged = state.total;
        }
        TickType_t now = xTaskGetTickCount();
        if(changed && (state.date_key != saved_date_key ||
           (state.total >= saved_at_count && state.total - saved_at_count >= STEPS_SAVE_DELTA) ||
           now - saved_at_tick >= pdMS_TO_TICKS(STEPS_SAVE_INTERVAL_MS))) {
            if(save_values(state.date_key, state.total, state.last_sensor, history) == ESP_OK) {
                saved_date_key = state.date_key;
                saved_at_count = state.total;
                saved_at_tick = now;
            }
        }
    }
}

esp_err_t watch_steps_start(void)
{
    portENTER_CRITICAL(&s_lock);
    if(s_task != NULL || s_starting) {
        portEXIT_CRITICAL(&s_lock);
        return ESP_OK;
    }
    s_starting = true;
    portEXIT_CRITICAL(&s_lock);

    TaskHandle_t task = NULL;
    BaseType_t created = xTaskCreate(steps_task, "watch_steps", STEPS_TASK_STACK,
                                     NULL, STEPS_TASK_PRIORITY, &task);
    portENTER_CRITICAL(&s_lock);
    s_task = created == pdPASS ? task : NULL;
    s_starting = false;
    portEXIT_CRITICAL(&s_lock);
    return created == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}

void watch_steps_get_snapshot(watch_steps_snapshot_t *snapshot)
{
    if(snapshot == NULL) return;
    portENTER_CRITICAL(&s_lock);
    *snapshot = s_snapshot;
    portEXIT_CRITICAL(&s_lock);
}

size_t watch_steps_get_history(watch_step_day_t *entries, size_t capacity)
{
    if(entries == NULL || capacity == 0) return 0;
    portENTER_CRITICAL(&s_lock);
    size_t count = s_history.count < capacity ? s_history.count : capacity;
    memcpy(entries, s_history.days, count * sizeof(entries[0]));
    portEXIT_CRITICAL(&s_lock);
    return count;
}

esp_err_t watch_steps_flush(void)
{
    watch_steps_snapshot_t snapshot;
    steps_history_t history;
    watch_steps_get_snapshot(&snapshot);
    if(!snapshot.running) return snapshot.error == ESP_OK ? ESP_ERR_INVALID_STATE : snapshot.error;
    portENTER_CRITICAL(&s_lock);
    history = s_history;
    portEXIT_CRITICAL(&s_lock);
    (void)history_upsert(&history, snapshot.date_key, snapshot.today);
    publish_history(&history);
    return save_values(snapshot.date_key, snapshot.today, snapshot.sensor_total, &history);
}

void watch_steps_set_suspended(bool suspended)
{
    portENTER_CRITICAL(&s_lock);
    s_suspend_requested = suspended;
    TaskHandle_t task = s_task;
    portEXIT_CRITICAL(&s_lock);
    if(task == NULL) return;
    xTaskNotifyGive(task);
    if(!suspended) return;

    /* Wait until no feature-page I2C transaction can overlap light sleep. */
    TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(1100);
    for(;;) {
        portENTER_CRITICAL(&s_lock);
        bool ready = s_suspended || s_task == NULL;
        portEXIT_CRITICAL(&s_lock);
        if(ready) return;
        if((int32_t)(xTaskGetTickCount() - deadline) >= 0) return;
        vTaskDelay(1);
    }
}
