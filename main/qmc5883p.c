/**
 * @file qmc5883p.c
 * @brief QMC5883P 磁力计寄存器驱动和航向角计算实现。
 */
/*
 * ==================== 模块说明 ====================
 *  * 模块职责：
 * - QMC5883P 的纯寄存器驱动层，不依赖具体平台 I2C 实现，便于移植。
 * - 默认提供 weak 版本的读写/延时函数，实际工程可由 qmc5883p_port.c 覆盖。
 * - 初始化流程包含软复位、轴符号配置、量程/采样率设置和普通工作模式配置。
 * - 航向角计算先做硬铁偏置和软铁比例校准，再使用 atan2f 得到 0~360 度方向。
 *
 * 阅读建议：
 * - 先看本文件顶部的宏定义和静态状态变量，理解硬件参数和运行状态。
 * - 再看 reset/init/start/on_key/destroy 等对外函数，理解页面或驱动的生命周期。
 * - 最后看 static 辅助函数，了解具体寄存器读写、UI 刷新或状态机细节。
 * =======================================================
 */


#include "qmc5883p.h"

#include <math.h>
#include <stddef.h>

#define QMC5883P_REG_CHIP_ID            0x00U
#define QMC5883P_REG_X_LSB              0x01U
#define QMC5883P_REG_STATUS             0x09U
#define QMC5883P_REG_CONTROL_1          0x0AU
#define QMC5883P_REG_CONTROL_2          0x0BU
#define QMC5883P_REG_AXIS_SIGN          0x29U

#define QMC5883P_STATUS_DRDY_MASK       0x01U
#define QMC5883P_STATUS_OVFL_MASK       0x02U

#define QMC5883P_CONTROL_1_SUSPEND      0x00U
#define QMC5883P_CONTROL_1_NORMAL_200HZ 0xCDU

#define QMC5883P_CONTROL_2_RESET        0x80U
#define QMC5883P_CONTROL_2_8GAUSS_SR_ON 0x08U

#define QMC5883P_AXIS_SIGN_DEFAULT      0x06U

#define QMC5883P_PI                     3.14159265358979323846f
#define QMC5883P_HEADING_EPSILON        0.000001f

#ifndef QMC5883P_USE_WEAK_PORT
#define QMC5883P_USE_WEAK_PORT          1
#endif

#if QMC5883P_USE_WEAK_PORT

#ifndef QMC5883P_WEAK
#if defined(__GNUC__)
#define QMC5883P_WEAK                   __attribute__((weak))
#elif defined(__ICCARM__) || defined(__CC_ARM) || defined(__ARMCC_VERSION)
#define QMC5883P_WEAK                   __weak
#else
#define QMC5883P_WEAK
#endif
#endif

/**
 * @brief 默认弱定义 I2C 读取接口。
 */
QMC5883P_WEAK bool qmc5883p_port_i2c_read(uint8_t slave_addr_7bit,
                                          uint8_t reg,
                                          uint8_t *buf,
                                          uint16_t len)
{
    (void)slave_addr_7bit;
    (void)reg;
    (void)buf;
    (void)len;

    return false;
}

/**
 * @brief 默认弱定义 I2C 写入接口。
 */
QMC5883P_WEAK bool qmc5883p_port_i2c_write(uint8_t slave_addr_7bit,
                                           uint8_t reg,
                                           const uint8_t *buf,
                                           uint16_t len)
{
    (void)slave_addr_7bit;
    (void)reg;
    (void)buf;
    (void)len;

    return false;
}

/**
 * @brief 默认弱定义毫秒延时接口。
 */
QMC5883P_WEAK void qmc5883p_port_delay_ms(uint32_t ms)
{
    (void)ms;
}

#endif

/**
 * @brief 写入 QMC5883P 单字节寄存器。
 *
 * 详细说明：
 * - 封装平台写接口，统一返回驱动层错误码。
 *
 * @param reg 输入或输出参数，具体含义见函数内部使用方式。
 * @param value 输入或输出参数，具体含义见函数内部使用方式。
 */
static qmc5883p_result_t qmc5883p_write_u8(uint8_t reg, uint8_t value)
{
    if(!qmc5883p_port_i2c_write(QMC5883P_I2C_ADDR_7BIT, reg, &value, 1U)) {
        return QMC5883P_ERR_I2C;
    }

    return QMC5883P_OK;
}

/**
 * @brief 读取 QMC5883P 连续寄存器。
 *
 * 详细说明：
 * - 用于读取芯片 ID、状态位以及三轴原始数据。
 *
 * @param reg 输入或输出参数，具体含义见函数内部使用方式。
 * @param buf 输入或输出参数，具体含义见函数内部使用方式。
 * @param len 输入或输出参数，具体含义见函数内部使用方式。
 */
static qmc5883p_result_t qmc5883p_read_bytes(uint8_t reg, uint8_t *buf, uint16_t len)
{
    if(buf == NULL || len == 0U) {
        return QMC5883P_ERR_NULL;
    }

    if(!qmc5883p_port_i2c_read(QMC5883P_I2C_ADDR_7BIT, reg, buf, len)) {
        return QMC5883P_ERR_I2C;
    }

    return QMC5883P_OK;
}

/**
 * @brief 读取 QMC5883P 芯片 ID。
 *
 * 详细说明：
 * - 用于确认 I2C 地址和器件是否响应。
 *
 * @param chip_id 输入或输出参数，具体含义见函数内部使用方式。
 */
qmc5883p_result_t qmc5883p_read_chip_id(uint8_t *chip_id)
{
    if(chip_id == NULL) {
        return QMC5883P_ERR_NULL;
    }

    return qmc5883p_read_bytes(QMC5883P_REG_CHIP_ID, chip_id, 1U);
}

/**
 * @brief 软件复位 QMC5883P。
 *
 * 详细说明：
 * - 复位后延时等待内部状态稳定。
 */
qmc5883p_result_t qmc5883p_soft_reset(void)
{
    qmc5883p_result_t result = qmc5883p_write_u8(QMC5883P_REG_CONTROL_2, QMC5883P_CONTROL_2_RESET);

    if(result != QMC5883P_OK) {
        return result;
    }

    qmc5883p_port_delay_ms(2U);

    return QMC5883P_OK;
}

/**
 * @brief 让 QMC5883P 进入 suspend 模式。
 *
 * 详细说明：
 * - 用于降低功耗或初始化前清理工作状态。
 */
qmc5883p_result_t qmc5883p_suspend(void)
{
    return qmc5883p_write_u8(QMC5883P_REG_CONTROL_1, QMC5883P_CONTROL_1_SUSPEND);
}

/**
 * @brief 初始化 QMC5883P 工作模式。
 *
 * 详细说明：
 * - 完成芯片识别、复位、轴符号和控制寄存器配置。
 */
qmc5883p_result_t qmc5883p_init(void)
{
    uint8_t chip_id = 0U;
    qmc5883p_result_t result = QMC5883P_OK;

    result = qmc5883p_soft_reset();
    if(result != QMC5883P_OK) {
        return result;
    }

    result = qmc5883p_read_chip_id(&chip_id);
    if(result != QMC5883P_OK) {
        return result;
    }

    if(chip_id != QMC5883P_CHIP_ID_DEFAULT) {
        return QMC5883P_ERR_CHIP_ID;
    }

    result = qmc5883p_write_u8(QMC5883P_REG_AXIS_SIGN, QMC5883P_AXIS_SIGN_DEFAULT);
    if(result != QMC5883P_OK) {
        return result;
    }

    result = qmc5883p_write_u8(QMC5883P_REG_CONTROL_2, QMC5883P_CONTROL_2_8GAUSS_SR_ON);
    if(result != QMC5883P_OK) {
        return result;
    }

    result = qmc5883p_write_u8(QMC5883P_REG_CONTROL_1, QMC5883P_CONTROL_1_NORMAL_200HZ);
    if(result != QMC5883P_OK) {
        return result;
    }

    qmc5883p_port_delay_ms(10U);

    return QMC5883P_OK;
}

/**
 * @brief 查询磁场数据是否就绪。
 *
 * 详细说明：
 * - 读取 STATUS 寄存器的 DRDY 位。
 * - 如果发生溢出，可由调用方决定是否丢弃本次数据。
 *
 * @param ready 输入或输出参数，具体含义见函数内部使用方式。
 * @param overflow 输入或输出参数，具体含义见函数内部使用方式。
 */
qmc5883p_result_t qmc5883p_is_data_ready(bool *ready, bool *overflow)
{
    uint8_t status = 0U;
    qmc5883p_result_t result = QMC5883P_OK;

    if(ready == NULL || overflow == NULL) {
        return QMC5883P_ERR_NULL;
    }

    result = qmc5883p_read_bytes(QMC5883P_REG_STATUS, &status, 1U);
    if(result != QMC5883P_OK) {
        return result;
    }

    *ready = ((status & QMC5883P_STATUS_DRDY_MASK) != 0U);
    *overflow = ((status & QMC5883P_STATUS_OVFL_MASK) != 0U);

    return QMC5883P_OK;
}

/**
 * @brief 读取三轴原始磁场数据。
 *
 * 详细说明：
 * - 一次读取 6 字节并按小端格式转换为 int16。
 *
 * @param raw 输入或输出参数，具体含义见函数内部使用方式。
 */
qmc5883p_result_t qmc5883p_read_raw(qmc5883p_raw_t *raw)
{
    bool ready = false;
    bool overflow = false;
    uint8_t data[6] = {0};
    qmc5883p_result_t result = QMC5883P_OK;

    if(raw == NULL) {
        return QMC5883P_ERR_NULL;
    }

    result = qmc5883p_is_data_ready(&ready, &overflow);
    if(result != QMC5883P_OK) {
        return result;
    }

    if(overflow) {
        return QMC5883P_ERR_OVERFLOW;
    }

    if(!ready) {
        return QMC5883P_ERR_NOT_READY;
    }

    result = qmc5883p_read_bytes(QMC5883P_REG_X_LSB, data, sizeof(data));
    if(result != QMC5883P_OK) {
        return result;
    }

    raw->x = (int16_t)((uint16_t)data[0] | ((uint16_t)data[1] << 8U));
    raw->y = (int16_t)((uint16_t)data[2] | ((uint16_t)data[3] << 8U));
    raw->z = (int16_t)((uint16_t)data[4] | ((uint16_t)data[5] << 8U));

    return QMC5883P_OK;
}

/**
 * @brief 填充默认校准参数。
 *
 * 详细说明：
 * - 默认只做单位比例，不做偏置补偿。
 *
 * @param calibration 输入或输出参数，具体含义见函数内部使用方式。
 */
void qmc5883p_calibration_default(qmc5883p_calibration_t *calibration)
{
    if(calibration == NULL) {
        return;
    }

    calibration->offset_x = 0;
    calibration->offset_y = 0;
    calibration->offset_z = 0;
    calibration->scale_x = 1.0f;
    calibration->scale_y = 1.0f;
    calibration->scale_z = 1.0f;
    calibration->declination_deg = 0.0f;
}

/**
 * @brief 对原始磁场数据应用零偏和比例校准。
 *
 * 详细说明：
 * - 先减去 offset，再乘以 scale，用于硬铁/软铁修正。
 *
 * @param raw 输入或输出参数，具体含义见函数内部使用方式。
 * @param calibration 输入或输出参数，具体含义见函数内部使用方式。
 * @param x 输入或输出参数，具体含义见函数内部使用方式。
 * @param y 输入或输出参数，具体含义见函数内部使用方式。
 * @param z 输入或输出参数，具体含义见函数内部使用方式。
 */
qmc5883p_result_t qmc5883p_apply_calibration(const qmc5883p_raw_t *raw,
                                             const qmc5883p_calibration_t *calibration,
                                             float *x,
                                             float *y,
                                             float *z)
{
    if(raw == NULL || calibration == NULL || x == NULL || y == NULL || z == NULL) {
        return QMC5883P_ERR_NULL;
    }

    *x = ((float)raw->x - (float)calibration->offset_x) * calibration->scale_x;
    *y = ((float)raw->y - (float)calibration->offset_y) * calibration->scale_y;
    *z = ((float)raw->z - (float)calibration->offset_z) * calibration->scale_z;

    return QMC5883P_OK;
}

/**
 * @brief 计算地理正北航向角。
 *
 * 详细说明：
 * - 使用 atan2f 计算平面角度。
 * - 结果统一归一化到 0~360 度范围。
 *
 * @param x_north_component 输入或输出参数，具体含义见函数内部使用方式。
 * @param y_east_component 输入或输出参数，具体含义见函数内部使用方式。
 * @param declination_deg 输入或输出参数，具体含义见函数内部使用方式。
 */
float qmc5883p_calc_true_heading_deg(float x_north_component, float y_east_component, float declination_deg)
{
    float heading_deg = 0.0f;

    if(fabsf(x_north_component) < QMC5883P_HEADING_EPSILON && fabsf(y_east_component) < QMC5883P_HEADING_EPSILON) {
        return 0.0f;
    }

    /*
     * atan2f(东向分量, 北向分量) 可以得到以正北为 0 度, 顺时针增加的航向角.
     * x_north_component > 0 且 y_east_component = 0 时为 0 度.
     * x_north_component = 0 且 y_east_component > 0 时为 90 度.
     */
    heading_deg = atan2f(y_east_component, x_north_component) * 180.0f / QMC5883P_PI;
    heading_deg += declination_deg;

    while(heading_deg < 0.0f) {
        heading_deg += 360.0f;
    }

    while(heading_deg >= 360.0f) {
        heading_deg -= 360.0f;
    }

    return heading_deg;
}
