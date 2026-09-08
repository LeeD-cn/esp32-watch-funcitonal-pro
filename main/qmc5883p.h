#ifndef QMC5883P_H
#define QMC5883P_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief QMC5883P 默认 7-bit I2C 地址.
 */
/**
 * @file qmc5883p.h
 * @brief QMC5883P 三轴磁力计驱动接口。
 */

#define QMC5883P_I2C_ADDR_7BIT          0x2CU

/**
 * @brief QMC5883P 默认芯片 ID.
 */
#define QMC5883P_CHIP_ID_DEFAULT        0x80U

/**
 * @brief QMC5883P 驱动执行结果.
 */
typedef enum {
    /**
     * @brief 执行成功.
     */
    QMC5883P_OK = 0,

    /**
     * @brief 参数为空.
     */
    QMC5883P_ERR_NULL = -1,

    /**
     * @brief I2C 通信失败.
     */
    QMC5883P_ERR_I2C = -2,

    /**
     * @brief 芯片 ID 不匹配.
     */
    QMC5883P_ERR_CHIP_ID = -3,

    /**
     * @brief 数据未就绪.
     */
    QMC5883P_ERR_NOT_READY = -4,

    /**
     * @brief 磁场数据溢出.
     */
    QMC5883P_ERR_OVERFLOW = -5
} qmc5883p_result_t;

/**
 * @brief QMC5883P 原始三轴磁场数据.
 */
typedef struct {
    /**
     * @brief X 轴原始数据.
     */
    int16_t x;

    /**
     * @brief Y 轴原始数据.
     */
    int16_t y;

    /**
     * @brief Z 轴原始数据.
     */
    int16_t z;
} qmc5883p_raw_t;

/**
 * @brief QMC5883P 校准参数.
 */
typedef struct {
    /**
     * @brief X 轴零偏.
     */
    int16_t offset_x;

    /**
     * @brief Y 轴零偏.
     */
    int16_t offset_y;

    /**
     * @brief Z 轴零偏.
     */
    int16_t offset_z;

    /**
     * @brief X 轴比例系数.
     */
    float scale_x;

    /**
     * @brief Y 轴比例系数.
     */
    float scale_y;

    /**
     * @brief Z 轴比例系数.
     */
    float scale_z;

    /**
     * @brief 地磁偏角修正值, 单位为度.
     *
     * @note 东偏为正, 西偏为负. 如果只需要磁北航向角, 这里填 0.0f.
     */
    float declination_deg;
} qmc5883p_calibration_t;

/**
 * @brief I2C 读取寄存器.
 *
 * @param slave_addr_7bit 7-bit 从机地址.
 * @param reg 寄存器地址.
 * @param buf 接收缓冲区.
 * @param len 读取长度.
 * @return true 读取成功.
 * @return false 读取失败.
 */
bool qmc5883p_port_i2c_read(uint8_t slave_addr_7bit, uint8_t reg, uint8_t *buf, uint16_t len);

/**
 * @brief I2C 写入寄存器.
 *
 * @param slave_addr_7bit 7-bit 从机地址.
 * @param reg 寄存器地址.
 * @param buf 发送缓冲区.
 * @param len 写入长度.
 * @return true 写入成功.
 * @return false 写入失败.
 */
bool qmc5883p_port_i2c_write(uint8_t slave_addr_7bit, uint8_t reg, const uint8_t *buf, uint16_t len);

/**
 * @brief 毫秒延时.
 *
 * @param ms 延时毫秒数.
 */
void qmc5883p_port_delay_ms(uint32_t ms);

/**
 * @brief 初始化 QMC5883P.
 *
 * @return qmc5883p_result_t 执行结果.
 */
qmc5883p_result_t qmc5883p_init(void);

/**
 * @brief 进入 Suspend 模式.
 *
 * @return qmc5883p_result_t 执行结果.
 */
qmc5883p_result_t qmc5883p_suspend(void);

/**
 * @brief 软复位 QMC5883P.
 *
 * @return qmc5883p_result_t 执行结果.
 */
qmc5883p_result_t qmc5883p_soft_reset(void);

/**
 * @brief 读取芯片 ID.
 *
 * @param chip_id 芯片 ID 输出指针.
 * @return qmc5883p_result_t 执行结果.
 */
qmc5883p_result_t qmc5883p_read_chip_id(uint8_t *chip_id);

/**
 * @brief 判断三轴数据是否就绪.
 *
 * @param ready 数据就绪标志输出指针.
 * @param overflow 数据溢出标志输出指针.
 * @return qmc5883p_result_t 执行结果.
 */
qmc5883p_result_t qmc5883p_is_data_ready(bool *ready, bool *overflow);

/**
 * @brief 读取三轴原始磁场数据.
 *
 * @param raw 原始数据输出指针.
 * @return qmc5883p_result_t 执行结果.
 */
qmc5883p_result_t qmc5883p_read_raw(qmc5883p_raw_t *raw);

/**
 * @brief 设置默认校准参数.
 *
 * @param calibration 校准参数指针.
 */
void qmc5883p_calibration_default(qmc5883p_calibration_t *calibration);

/**
 * @brief 按校准参数修正原始数据.
 *
 * @param raw 原始数据指针.
 * @param calibration 校准参数指针.
 * @param x 修正后 X 轴输出指针.
 * @param y 修正后 Y 轴输出指针.
 * @param z 修正后 Z 轴输出指针.
 * @return qmc5883p_result_t 执行结果.
 */
qmc5883p_result_t qmc5883p_apply_calibration(const qmc5883p_raw_t *raw,
                                             const qmc5883p_calibration_t *calibration,
                                             float *x,
                                             float *y,
                                             float *z);

/**
 * @brief 根据 XY 平面磁场分量计算地理正北航向角.
 *
 * @param x_north_component X 平面分量, 默认已经映射为正北方向分量.
 * @param y_east_component Y 平面分量, 默认已经映射为正东方向分量.
 * @param declination_deg 地磁偏角修正值, 东偏为正, 西偏为负.
 * @return float 地理正北航向角, 范围为 0.0f 到 360.0f, 0 表示正北, 90 表示正东.
 */
float qmc5883p_calc_true_heading_deg(float x_north_component, float y_east_component, float declination_deg);

/**
 * @brief 扫描 QMC5883P 所在 I2C 总线上的设备地址。
 *
 * @note 该函数主要用于移植或硬件调试。
 */
void qmc5883p_port_i2c_scan(void);

#ifdef __cplusplus
}
#endif

#endif
