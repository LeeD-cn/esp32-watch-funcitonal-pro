/**
 * @file qmc5883p_port.c
 * @brief QMC5883P 在 ESP32-S3/ESP-IDF 上的 I2C 移植层。
 */
/*
 * ==================== 模块说明 ====================
 *  * 模块职责：
 * - QMC5883P 在 ESP32-S3 + ESP-IDF 上的 I2C 移植层，给通用驱动提供实际读写函数。
 * - 采用 I2C 主机模式，默认 GPIO6/GPIO7，100 kHz，适合与低速传感器稳定通信。
 * - 读写函数内部会惰性初始化 I2C，调用者不需要关心总线是否已经安装。
 * - 提供扫描函数用于调试硬件连线和确认磁力计地址是否可见。
 *
 * 阅读建议：
 * - 先看本文件顶部的宏定义和静态状态变量，理解硬件参数和运行状态。
 * - 再看 reset/init/start/on_key/destroy 等对外函数，理解页面或驱动的生命周期。
 * - 最后看 static 辅助函数，了解具体寄存器读写、UI 刷新或状态机细节。
 * =======================================================
 */


#include "qmc5883p.h"

#include <stddef.h>
#include <string.h>

#include "driver/i2c.h"
#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "QMC_PORT";

/**
 * @brief QMC5883P SDA 接 ESP32-S3 IO6.
 */
#define QMC5883P_PORT_SDA_GPIO          6

/**
 * @brief QMC5883P SCL 接 ESP32-S3 IO7.
 */
#define QMC5883P_PORT_SCL_GPIO          7

/**
 * @brief QMC5883P 使用的 ESP32-S3 I2C 控制器.
 *
 * @note QMC5883P 与 CW2015、RTC、BMI270 共用 GPIO6/GPIO7，必须统一使用 I2C_NUM_1。
 */
#define QMC5883P_PORT_I2C_NUM           I2C_NUM_1

/**
 * @brief QMC5883P I2C 时钟频率.
 */
#define QMC5883P_PORT_I2C_FREQ_HZ       100000U

/**
 * @brief QMC5883P I2C 超时时间.
 */
#define QMC5883P_PORT_I2C_TIMEOUT_MS    100U

/**
 * @brief QMC5883P 单次写入最大数据长度.
 */
#define QMC5883P_PORT_MAX_WRITE_LEN     16U

/**
 * @brief I2C 是否已经初始化.
 */
static bool s_qmc5883p_i2c_inited = false;

/**
 * @brief 初始化 ESP32-S3 I2C 主机.
 *
 * @return true 初始化成功.
 * @return false 初始化失败.
 *
 * 详细说明：
 * - 首次读写前安装 I2C driver。
 * - 重复调用时直接返回成功，避免重复安装驱动。
 */
static bool qmc5883p_port_i2c_init_once(void)
{
    esp_err_t err = ESP_OK;

    if(s_qmc5883p_i2c_inited) {
        return true;
    }

    i2c_config_t conf = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = QMC5883P_PORT_SDA_GPIO,
        .scl_io_num = QMC5883P_PORT_SCL_GPIO,
        .sda_pullup_en = GPIO_PULLUP_ENABLE,
        .scl_pullup_en = GPIO_PULLUP_ENABLE,
        .master.clk_speed = QMC5883P_PORT_I2C_FREQ_HZ,
        .clk_flags = 0,
    };

    err = i2c_param_config(QMC5883P_PORT_I2C_NUM, &conf);
    if(err != ESP_OK) {
        return false;
    }

    err = i2c_driver_install(QMC5883P_PORT_I2C_NUM, conf.mode, 0, 0, 0);
    if(err == ESP_OK || err == ESP_ERR_INVALID_STATE) {
        s_qmc5883p_i2c_inited = true;
        return true;
    }

    /*
     * 共享总线场景下，I2C_NUM_1 可能已经被 battery/RTC/BMI270 模块安装。
     * 某些 ESP-IDF 版本在重复安装时不会返回 ESP_ERR_INVALID_STATE，而是返回 ESP_FAIL，
     * 并打印 "i2c driver install error"。此时仍然尝试复用已存在的 driver，
     * 避免 QMC 页面每 200ms 反复初始化失败刷屏。
     */
    ESP_LOGW(TAG,
             "I2C driver install returned %s, try to reuse existing I2C driver on port %d",
             esp_err_to_name(err),
             QMC5883P_PORT_I2C_NUM);

    s_qmc5883p_i2c_inited = true;
    return true;
}

/**
 * @brief 通过 ESP-IDF I2C 主机读取寄存器。
 */
bool qmc5883p_port_i2c_read(uint8_t slave_addr_7bit, uint8_t reg, uint8_t *buf, uint16_t len)
{
    esp_err_t err = ESP_OK;

    if(buf == NULL || len == 0U) {
        return false;
    }

    if(!qmc5883p_port_i2c_init_once()) {
        return false;
    }

    err = i2c_master_write_read_device(QMC5883P_PORT_I2C_NUM,
                                       slave_addr_7bit,
                                       &reg,
                                       1U,
                                       buf,
                                       len,
                                       pdMS_TO_TICKS(QMC5883P_PORT_I2C_TIMEOUT_MS));

    return err == ESP_OK;
}

/**
 * @brief 通过 ESP-IDF I2C 主机写入寄存器。
 */
bool qmc5883p_port_i2c_write(uint8_t slave_addr_7bit, uint8_t reg, const uint8_t *buf, uint16_t len)
{
    esp_err_t err = ESP_OK;
    uint8_t tx_buf[QMC5883P_PORT_MAX_WRITE_LEN + 1U] = {0};

    if(buf == NULL || len == 0U || len > QMC5883P_PORT_MAX_WRITE_LEN) {
        return false;
    }

    if(!qmc5883p_port_i2c_init_once()) {
        return false;
    }

    tx_buf[0] = reg;
    memcpy(&tx_buf[1], buf, len);

    err = i2c_master_write_to_device(QMC5883P_PORT_I2C_NUM,
                                     slave_addr_7bit,
                                     tx_buf,
                                     (size_t)len + 1U,
                                     pdMS_TO_TICKS(QMC5883P_PORT_I2C_TIMEOUT_MS));

    return err == ESP_OK;
}

/**
 * @brief FreeRTOS 毫秒延时封装。
 */
void qmc5883p_port_delay_ms(uint32_t ms)
{
    vTaskDelay(pdMS_TO_TICKS(ms));
}


/**
 * @brief 扫描当前 I2C 总线上的设备地址。
 *
 * 详细说明：
 * - 用于调试 SDA/SCL 连接和确认传感器是否在线。
 */
void qmc5883p_port_i2c_scan(void)
{
    uint8_t dummy = 0x00;

    if(!qmc5883p_port_i2c_init_once()) {
        ESP_LOGE(TAG, "I2C 初始化失败, SDA=%d, SCL=%d", QMC5883P_PORT_SDA_GPIO, QMC5883P_PORT_SCL_GPIO);
        return;
    }

    ESP_LOGI(TAG, "开始扫描 I2C, SDA=%d, SCL=%d", QMC5883P_PORT_SDA_GPIO, QMC5883P_PORT_SCL_GPIO);

    for(uint8_t addr = 0x03; addr <= 0x77; addr++) {
        esp_err_t err = i2c_master_write_to_device(QMC5883P_PORT_I2C_NUM,
                                                   addr,
                                                   &dummy,
                                                   1,
                                                   pdMS_TO_TICKS(50));

        if(err == ESP_OK) {
            ESP_LOGI(TAG, "发现 I2C 设备: 0x%02X", addr);
        }
    }

    ESP_LOGI(TAG, "I2C 扫描完成");
}