#pragma once

#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @file watch_bmi270.h
 * @brief BMI270 加速度计初始化、数据就绪中断和抬腕检测接口。
 *
 * @note BMI270 与 PCF85063 RTC / CW2015 电量计共用 I2C1，本驱动只使用已初始化的总线。
 */

/**
 * @brief 初始化 BMI270 并配置加速度计。
 *
 * @return esp_err_t ESP_OK 表示初始化成功。
 */
esp_err_t watch_bmi270_init(void);

/**
 * @brief 查询 BMI270 是否已初始化成功。
 *
 * @return true 已就绪。
 * @return false 未就绪。
 */
bool watch_bmi270_is_ready(void);

/**
 * @brief 进入抬腕检测状态并建立息屏姿态基线。
 */
void watch_bmi270_raise_wrist_begin(void);

/**
 * @brief 轮询加速度数据并判断是否满足抬腕亮屏条件。
 *
 * @return true 检测到抬腕动作。
 * @return false 未检测到抬腕动作。
 */
bool watch_bmi270_raise_wrist_poll(void);

/**
 * @brief 退出抬腕检测状态。
 */
void watch_bmi270_raise_wrist_end(void);

/**
 * @brief 打开或关闭 BMI270 data-ready 中断映射。
 *
 * @param enable true 打开中断，false 关闭中断。
 * @return esp_err_t ESP_OK 表示配置成功。
 */
esp_err_t watch_bmi270_enable_data_ready_interrupt(bool enable);

#ifdef __cplusplus
}
#endif
