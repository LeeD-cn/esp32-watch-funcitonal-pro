#pragma once

#include <stdbool.h>
#include <stdint.h>
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
 * @brief 一次三轴加速度采样。
 *
 * 坐标为 BMI270 原生器件坐标，单位为 mg；调用者负责按 PCB 安装方向
 * 映射到自己的页面坐标。timestamp_us 使用 ESP 单调时钟。
 */
typedef struct {
    int16_t x_mg;
    int16_t y_mg;
    int16_t z_mg;
    int64_t timestamp_us;
    bool valid;
} watch_bmi270_accel_sample_t;

/**
 * @brief 读取一帧带时间戳的三轴加速度。
 *
 * @param sample 输出采样；读取失败时 valid 为 false。
 * @return ESP_OK 读取成功；其他值表示参数、初始化状态或 I2C 错误。
 */
esp_err_t watch_bmi270_read_acceleration(watch_bmi270_accel_sample_t *sample);

/** Enable the BMI270 wrist-worn hardware step counter. */
esp_err_t watch_bmi270_step_counter_enable(void);

/** Read the sensor's monotonic 32-bit step count since its last reset. */
esp_err_t watch_bmi270_step_counter_read(uint32_t *steps);

/* Diagnostic six-axis mode: raw native axes, ±4g and ±1000 degrees/s.
 * One caller owns begin/read/end; normal readers are suspended during this mode. */
typedef struct {
    int64_t timestamp_us;
    int16_t accel[3];
    int16_t gyro[3];
    uint32_t sensor_ticks; /* 24-bit free-running clock, NOT sample timestamp. */
} watch_bmi270_motion_sample_t;
esp_err_t watch_bmi270_motion_begin(void);
esp_err_t watch_bmi270_motion_read(watch_bmi270_motion_sample_t *sample);
esp_err_t watch_bmi270_motion_end(void);

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
