#ifndef WATCH_BATTERY_H
#define WATCH_BATTERY_H

#include <stdbool.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @file watch_battery.h
 * @brief CW2015 电量计和 TP4056 充电状态检测接口。
 */

/**
 * @brief 初始化电量计 I2C、充电检测 GPIO 和 CW2015。
 *
 * @return esp_err_t ESP_OK 表示初始化成功。
 */
esp_err_t watch_battery_init(void);

/**
 * @brief 读取电池剩余电量百分比。
 *
 * @param percent 电量百分比输出指针，范围为 0~100。
 * @return esp_err_t ESP_OK 表示读取成功。
 */
esp_err_t watch_battery_read_percent(int *percent);

/**
 * @brief 读取电池电压。
 *
 * @param voltage_mv 电压输出指针，单位为 mV。
 * @return esp_err_t ESP_OK 表示读取成功。
 */
esp_err_t watch_battery_read_voltage_mv(int *voltage_mv);

/**
 * @brief 读取 TP4056 充电状态。
 *
 * @param charging 充电状态输出指针，true 表示正在充电。
 * @return esp_err_t ESP_OK 表示读取成功。
 */
esp_err_t watch_battery_is_charging(bool *charging);

#ifdef __cplusplus
}
#endif

#endif /* WATCH_BATTERY_H */
