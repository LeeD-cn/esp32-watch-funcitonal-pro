/**
 * @file watch_serial_config.h
 * @brief 串口配置任务接口。
 */

#ifndef WATCH_SERIAL_CONFIG_H
#define WATCH_SERIAL_CONFIG_H

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 启动串口/USB 配置任务。
 *
 * @return ESP_OK 任务已启动或已经处于启动状态。
 */
esp_err_t watch_serial_config_start(void);

#ifdef __cplusplus
}
#endif

#endif /* WATCH_SERIAL_CONFIG_H */
