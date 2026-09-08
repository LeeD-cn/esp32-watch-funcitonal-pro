/**
 * @file watch_rtc.h
 * @brief PCF85063 RTC 对外接口。
 */

#ifndef WATCH_RTC_H
#define WATCH_RTC_H

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 初始化 PCF85063 RTC 和共用 I2C 总线。
 *
 * @return ESP_OK 初始化成功。
 */
esp_err_t watch_rtc_init(void);
/**
 * @brief 开机时从 RTC 恢复系统时间；RTC 无效时初始化为 2026-01-01。
 *
 * @return ESP_OK 恢复或初始化成功。
 */
esp_err_t watch_rtc_restore_or_init_2026(void);
/**
 * @brief 读取 RTC 时间并设置为 ESP32 系统时间。
 *
 * @return ESP_OK 同步成功。
 */
esp_err_t watch_rtc_set_system_time_from_rtc(void);
/**
 * @brief 将当前 ESP32 系统时间写入 RTC。
 *
 * @return ESP_OK 写入成功。
 */
esp_err_t watch_rtc_update_from_system(void);

#ifdef __cplusplus
}
#endif

#endif /* WATCH_RTC_H */
