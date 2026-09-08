/**
 * @file watch_keys.h
 * @brief 手表实体按键事件接口。
 */
#ifndef WATCH_KEYS_H
#define WATCH_KEYS_H

#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 手表按键事件类型。
 */
typedef enum {
    WATCH_KEY_NONE = 0,
    WATCH_KEY_1,
    WATCH_KEY_2,
    WATCH_KEY_3,
    WATCH_KEY_2_RELEASE,
    WATCH_KEY_4_SHORT,
    WATCH_KEY_4_LONG,
} watch_key_t;

/**
 * @brief 初始化按键 GPIO、ISR 队列和扫描任务。
 *
 * @return ESP_OK 初始化成功；其他值表示 GPIO 或队列初始化失败。
 */
esp_err_t watch_keys_init(void);
/**
 * @brief 读取一个按键事件。
 *
 * @param key 输出按键事件。
 * @return true 读取到事件；false 队列为空或参数无效。
 */
bool watch_keys_get_event(watch_key_t *key);
/**
 * @brief 清空待处理按键事件队列。
 */
void watch_keys_clear_events(void);
/**
 * @brief 暂停 KEY4 电源键扫描任务。
 */
void watch_keys_key4_scan_suspend(void);
/**
 * @brief 恢复 KEY4 电源键扫描任务。
 */
void watch_keys_key4_scan_resume(void);

#ifdef __cplusplus
}
#endif

#endif
