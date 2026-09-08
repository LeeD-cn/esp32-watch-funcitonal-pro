/**
 * @file watch_wifi.h
 * @brief Wi-Fi STA 控制接口。
 */

#ifndef WATCH_WIFI_H
#define WATCH_WIFI_H

#include <stdbool.h>
#include "esp_err.h"
#include "freertos/FreeRTOS.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Wi-Fi 已获得 IP 的事件位。 */
#define WATCH_WIFI_CONNECTED_BIT  BIT0
/** Wi-Fi 连接失败或已停止的事件位。 */
#define WATCH_WIFI_FAIL_BIT       BIT1

/**
 * @brief 初始化 Wi-Fi STA 和连接状态资源。
 *
 * @return ESP_OK 初始化成功。
 */
esp_err_t watch_wifi_init(void);
/**
 * @brief 启动 Wi-Fi 并开始连接。
 *
 * @return ESP_OK 启动成功。
 */
esp_err_t watch_wifi_start(void);
/**
 * @brief 判断 Wi-Fi 是否已连接。
 *
 * @return true 已连接。
 */
bool watch_wifi_is_connected(void);
/**
 * @brief 阻塞等待 Wi-Fi 连接完成。
 *
 * @param timeout_ticks 等待超时时间。
 * @return ESP_OK 已连接，ESP_ERR_TIMEOUT 超时。
 */
esp_err_t watch_wifi_wait_connected(TickType_t timeout_ticks);
/**
 * @brief 停止 Wi-Fi 并清理连接状态。
 *
 * @return ESP_OK 停止成功或本来未启动。
 */
esp_err_t watch_wifi_stop(void);
bool watch_wifi_is_connected(void);

#ifdef __cplusplus
}
#endif

#endif /* WATCH_WIFI_H */
