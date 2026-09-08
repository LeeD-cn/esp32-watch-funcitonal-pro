/**
 * @file watch_keys.c
 * @brief 手表实体按键初始化、消抖、长按和重复按键事件生成。
 */
/*
 * ==================== 模块说明 ====================
 *  * 模块职责：
 * - 实体按键驱动层，负责 GPIO 初始化、消抖、长按检测、重复按键事件和 FreeRTOS 队列分发。
 * - KEY1~KEY3 通过 GPIO 中断触发，再由任务层做消抖和重复事件生成。
 * - KEY4 是电源键，单独用轮询任务区分短按和长按，避免与普通按键逻辑混在一起。
 * - 对外提供统一的 watch_keys_get_event()，上层 UI 不需要直接读取 GPIO。
 *
 * 阅读建议：
 * - 先看本文件顶部的宏定义和静态状态变量，理解硬件参数和运行状态。
 * - 再看 reset/init/start/on_key/destroy 等对外函数，理解页面或驱动的生命周期。
 * - 最后看 static 辅助函数，了解具体寄存器读写、UI 刷新或状态机细节。
 * =======================================================
 */

#include "watch_keys.h"

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#define KEY1_GPIO               35
#define KEY2_GPIO               34
#define KEY3_GPIO               33
#define KEY4_GPIO               21

#define GPIO_EVT_QUEUE_LEN      8
#define KEY_EVT_QUEUE_LEN       8
#define KEY_DEBOUNCE_MS         80
#define KEY_REPEAT_START_MS     450
#define KEY_REPEAT_INTERVAL_MS  120
#define KEY4_SCAN_INTERVAL_MS   20
#define KEY4_LONG_PRESS_MS      1200


/**
 * @brief KEY1~KEY3 的 GPIO 与逻辑按键映射。
 */
typedef struct {
    gpio_num_t gpio;
    watch_key_t key;
    TickType_t last_tick;
} key_map_t;

/* GPIO 中断事件队列，ISR 只写入 GPIO 编号。 */
static QueueHandle_t s_gpio_evt_queue = NULL;
/* 上层读取的按键事件队列。 */
static QueueHandle_t s_key_evt_queue = NULL;
/* KEY1~KEY3 扫描任务句柄。 */
static TaskHandle_t s_key_task_handle = NULL;
/* KEY4 电源键扫描任务句柄。 */
static TaskHandle_t s_key4_task_handle = NULL;

static key_map_t s_key_map[] = {
    {KEY1_GPIO, WATCH_KEY_1, 0},
    {KEY2_GPIO, WATCH_KEY_2, 0},
    {KEY3_GPIO, WATCH_KEY_3, 0},
};

/**
 * @brief 根据 GPIO 号查找按键映射索引。
 *
 * 详细说明：
 * - 用于中断任务把 GPIO 事件转换成 watch_key_t。
 *
 * @param gpio 输入或输出参数，具体含义见调用处和函数内部使用方式。
 *
 * @return 函数执行结果或计算得到的值，具体语义见返回路径。
 */
static int key_index_from_gpio(gpio_num_t gpio)
{
    for(int i = 0; i < (int)(sizeof(s_key_map) / sizeof(s_key_map[0])); i++) {
        if(s_key_map[i].gpio == gpio) {
            return i;
        }
    }

    return -1;
}

/**
 * @brief GPIO 中断服务函数。
 *
 * 详细说明：
 * - 只把 GPIO 号投递到队列，复杂消抖逻辑放到任务中处理。
 *
 * @param arg 输入或输出参数，具体含义见调用处和函数内部使用方式。
 */
static void key_gpio_isr_handler(void *arg)
{
    uint32_t gpio_num = (uint32_t)(uintptr_t)arg;

    if(s_gpio_evt_queue) {
        xQueueSendFromISR(s_gpio_evt_queue, &gpio_num, NULL);
    }
}

/**
 * @brief 向按键事件队列发送事件。
 *
 * 详细说明：
 * - 上层通过统一队列读取按键。
 *
 * @param key 输入或输出参数，具体含义见调用处和函数内部使用方式。
 */
static void key_send_event(watch_key_t key)
{
    if(s_key_evt_queue) {
        xQueueSend(s_key_evt_queue, &key, 0);
    }
}

/**
 * @brief 轮询 KEY4，区分短按和长按电源键事件。
 *
 * 详细说明：
 * - 轮询低电平按下时长，区分短按和长按。
 *
 * @param arg 输入或输出参数，具体含义见函数内部使用方式。
 */
static void key4_scan_task(void *arg)
{
    (void)arg;


    while(gpio_get_level(KEY4_GPIO) == 0) {
        vTaskDelay(pdMS_TO_TICKS(KEY4_SCAN_INTERVAL_MS));
    }
    vTaskDelay(pdMS_TO_TICKS(KEY_DEBOUNCE_MS));

    while(1) {
        if(gpio_get_level(KEY4_GPIO) != 0) {
            vTaskDelay(pdMS_TO_TICKS(KEY4_SCAN_INTERVAL_MS));
            continue;
        }

        vTaskDelay(pdMS_TO_TICKS(KEY_DEBOUNCE_MS));
        if(gpio_get_level(KEY4_GPIO) != 0) {
            continue;
        }

        TickType_t press_tick = xTaskGetTickCount();
        bool long_sent = false;

        while(gpio_get_level(KEY4_GPIO) == 0) {
            TickType_t now = xTaskGetTickCount();

            if(!long_sent && (now - press_tick) >= pdMS_TO_TICKS(KEY4_LONG_PRESS_MS)) {
                key_send_event(WATCH_KEY_4_LONG);
                long_sent = true;
            }

            vTaskDelay(pdMS_TO_TICKS(KEY4_SCAN_INTERVAL_MS));
        }

        vTaskDelay(pdMS_TO_TICKS(KEY_DEBOUNCE_MS));

        if(!long_sent) {
            key_send_event(WATCH_KEY_4_SHORT);
        }
    }
}

/**
 * @brief 处理 KEY1~KEY3 GPIO 中断并生成消抖后的按键事件。
 *
 * 详细说明：
 * - 处理中断后的消抖、按下/释放和重复事件。
 *
 * @param arg 输入或输出参数，具体含义见函数内部使用方式。
 */
static void key_scan_task(void *arg)
{
    (void)arg;

    uint32_t gpio_num;

    while(1) {
        if(xQueueReceive(s_gpio_evt_queue, &gpio_num, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        int idx = key_index_from_gpio((gpio_num_t)gpio_num);
        if(idx < 0) {
            continue;
        }

        TickType_t now = xTaskGetTickCount();


        if(gpio_get_level(s_key_map[idx].gpio) == 0) {
            if((now - s_key_map[idx].last_tick) < pdMS_TO_TICKS(KEY_DEBOUNCE_MS)) {
                continue;
            }


            vTaskDelay(pdMS_TO_TICKS(20));

            if(gpio_get_level(s_key_map[idx].gpio) == 0) {
                s_key_map[idx].last_tick = xTaskGetTickCount();

                watch_key_t key = s_key_map[idx].key;
                key_send_event(key);


                if(key == WATCH_KEY_1 || key == WATCH_KEY_3) {
                    vTaskDelay(pdMS_TO_TICKS(KEY_REPEAT_START_MS));

                    while(gpio_get_level(s_key_map[idx].gpio) == 0) {
                        key_send_event(key);
                        vTaskDelay(pdMS_TO_TICKS(KEY_REPEAT_INTERVAL_MS));
                    }

                    s_key_map[idx].last_tick = xTaskGetTickCount();
                }
            }
        }
        else {


            vTaskDelay(pdMS_TO_TICKS(20));

            if(gpio_get_level(s_key_map[idx].gpio) == 1 &&
               s_key_map[idx].key == WATCH_KEY_2) {
                key_send_event(WATCH_KEY_2_RELEASE);
            }
        }
    }
}

/**
 * @brief 初始化所有实体按键。
 *
 * 详细说明：
 * - 配置 GPIO、中断、队列和扫描任务。
 *
 * @return 函数执行结果或计算得到的值，具体语义见返回路径。
 */
esp_err_t watch_keys_init(void)
{
    if(s_gpio_evt_queue != NULL && s_key_evt_queue != NULL) {
        return ESP_OK;
    }

    s_gpio_evt_queue = xQueueCreate(GPIO_EVT_QUEUE_LEN, sizeof(uint32_t));
    s_key_evt_queue = xQueueCreate(KEY_EVT_QUEUE_LEN, sizeof(watch_key_t));

    if(s_gpio_evt_queue == NULL || s_key_evt_queue == NULL) {
        return ESP_ERR_NO_MEM;
    }

    uint64_t normal_key_pin_mask =
        (1ULL << KEY1_GPIO) |
        (1ULL << KEY2_GPIO) |
        (1ULL << KEY3_GPIO);

    gpio_config_t normal_key_conf = {
        .pin_bit_mask = normal_key_pin_mask,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_ANYEDGE,
    };

    esp_err_t ret = gpio_config(&normal_key_conf);
    if(ret != ESP_OK) {
        return ret;
    }

    gpio_config_t key4_conf = {
        .pin_bit_mask = 1ULL << KEY4_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };

    ret = gpio_config(&key4_conf);
    if(ret != ESP_OK) {
        return ret;
    }


    ret = gpio_install_isr_service(0);
    if(ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        return ret;
    }

    ret = gpio_isr_handler_add(KEY1_GPIO, key_gpio_isr_handler, (void *)(uintptr_t)KEY1_GPIO);
    if(ret != ESP_OK) {
        return ret;
    }

    ret = gpio_isr_handler_add(KEY2_GPIO, key_gpio_isr_handler, (void *)(uintptr_t)KEY2_GPIO);
    if(ret != ESP_OK) {
        return ret;
    }

    ret = gpio_isr_handler_add(KEY3_GPIO, key_gpio_isr_handler, (void *)(uintptr_t)KEY3_GPIO);
    if(ret != ESP_OK) {
        return ret;
    }

    BaseType_t ok = xTaskCreate(key_scan_task,
                                "watch_keys",
                                2048,
                                NULL,
                                5,
                                &s_key_task_handle);
    if(ok != pdPASS) {
        return ESP_ERR_NO_MEM;
    }

    ok = xTaskCreate(key4_scan_task,
                     "watch_key4",
                     2048,
                     NULL,
                     5,
                     &s_key4_task_handle);

    return ok == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}


/**
 * @brief 暂停 KEY4 扫描任务。
 *
 * 详细说明：
 * - 轻睡眠前调用，避免任务与 GPIO 唤醒逻辑冲突。
 */
void watch_keys_key4_scan_suspend(void)
{
    if(s_key4_task_handle != NULL) {
        vTaskSuspend(s_key4_task_handle);
    }
}

/**
 * @brief 恢复 KEY4 扫描任务。
 *
 * 详细说明：
 * - 唤醒后继续正常处理电源键。
 */
void watch_keys_key4_scan_resume(void)
{
    if(s_key4_task_handle != NULL) {
        vTaskResume(s_key4_task_handle);
    }
}

/**
 * @brief 从按键队列读取一个事件。
 *
 * 详细说明：
 * - 支持阻塞等待或超时等待。
 *
 * @param key 输入或输出参数，具体含义见调用处和函数内部使用方式。
 *
 * @return 函数执行结果或计算得到的值，具体语义见返回路径。
 */
bool watch_keys_get_event(watch_key_t *key)
{
    if(key == NULL || s_key_evt_queue == NULL) {
        return false;
    }

    return xQueueReceive(s_key_evt_queue, key, 0) == pdTRUE;
}

/**
 * @brief 清空残留按键事件。
 *
 * 详细说明：
 * - 常用于页面切换或唤醒后丢弃旧事件。
 */
void watch_keys_clear_events(void)
{
    uint32_t gpio_num;
    watch_key_t key;

    if(s_gpio_evt_queue != NULL) {
        while(xQueueReceive(s_gpio_evt_queue, &gpio_num, 0) == pdTRUE) {
        }
    }

    if(s_key_evt_queue != NULL) {
        while(xQueueReceive(s_key_evt_queue, &key, 0) == pdTRUE) {
        }
    }
}
