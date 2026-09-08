/**
 * @file watch_bmi270.c
 * @brief BMI270 初始化、加速度读取、中断控制和抬腕检测实现。
 */
/*
 * ==================== 模块说明 ====================
 *  * 模块职责：
 * - 负责 BMI270 加速度计初始化、配置文件加载、数据读取、中断映射和抬腕检测。
 * - BMI270 上电后必须加载 Bosch 配置文件，否则内部状态不会进入可用状态。
 * - 为了省电，本模块只打开加速度计，陀螺仪保持关闭。
 * - 抬腕检测基于息屏时的基线姿态、后续加速度变化、Z/Y 轴趋势和稳定性共同判断，减少误触发。
 *
 * 阅读建议：
 * - 先看本文件顶部的宏定义和静态状态变量，理解硬件参数和运行状态。
 * - 再看 reset/init/start/on_key/destroy 等对外函数，理解页面或驱动的生命周期。
 * - 最后看 static 辅助函数，了解具体寄存器读写、UI 刷新或状态机细节。
 * =======================================================
 */


#include "watch_bmi270.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>

#include "driver/i2c.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/* BMI270 与 RTC 共用 I2C1，本文件不重新安装 I2C driver。 */
#define BMI270_I2C_PORT             I2C_NUM_1
#define BMI270_I2C_TIMEOUT_MS       1000

#define BMI270_ADDR_LOW             0x68    /* SDO=GND */
#define BMI270_ADDR_HIGH            0x69    /* SDO=VDDIO */
#define BMI270_CHIP_ID              0x24

#define BMI270_REG_CHIP_ID          0x00
#define BMI270_REG_DATA_8           0x0C    /* ACC_X/Y/Z: 0x0C..0x11 */
#define BMI270_REG_ACC_CONF         0x40
#define BMI270_REG_ACC_RANGE        0x41
#define BMI270_REG_PWR_CONF         0x7C
#define BMI270_REG_PWR_CTRL         0x7D
#define BMI270_REG_INTERNAL_STATUS  0x21
#define BMI270_REG_INIT_CTRL        0x59
#define BMI270_REG_INIT_ADDR_0      0x5B
#define BMI270_REG_INIT_ADDR_1      0x5C
#define BMI270_REG_INIT_DATA        0x5E
#define BMI270_REG_INT_STATUS_1     0x1D
#define BMI270_REG_INT1_IO_CTRL     0x53
#define BMI270_REG_INT2_IO_CTRL     0x54
#define BMI270_REG_INT_LATCH        0x55
#define BMI270_REG_INT_MAP_DATA     0x58

/* BMI270 上电后必须加载 Bosch 配置文件。 */
#include "bmi270_config_file.h"
#define WATCH_BMI270_CONFIG_FILE bmi270_config_file_base
#define WATCH_BMI270_CONFIG_FILE_SIZE sizeof(bmi270_config_file_base)


/* ACC_CONF: 25Hz + normal/avg4 + performance filter. */
#define BMI270_ACC_CONF_25HZ        0xA6
/* ACC_RANGE: ±4g。此时 1g = 8192 LSB。 */
#define BMI270_ACC_RANGE_4G         0x01
#define BMI270_LSB_PER_G_4G         8192

/* PWR_CONF=0x00 关闭 advanced power save；PWR_CTRL=0x04 只开加速度计，陀螺仪保持关闭省电。 */
#define BMI270_PWR_CONF_NORMAL      0x00
#define BMI270_PWR_CTRL_ACC_ONLY    0x04

/* INT 输出配置为 active-high、push-pull，并同时映射到 INT1/INT2。 */
#define BMI270_INT_ACTIVE_HIGH_PP   0x0A
#define BMI270_INT_MAP_DRDY_INT1    0x04
#define BMI270_INT_MAP_DRDY_INT2    0x40
#define BMI270_INT_MAP_DRDY_BOTH    (BMI270_INT_MAP_DRDY_INT1 | BMI270_INT_MAP_DRDY_INT2)

/* 抬腕检测参数，单位主要为 ms 或 mg。 */
#define BMI270_WAKE_SAMPLE_MS       40      /* 25Hz */
#define BMI270_WAKE_WARMUP_SAMPLES  6       /* 息屏后先丢掉约 240ms，避开按键松手晃动 */
#define BMI270_WAKE_MIN_ARM_MOTION  160     /* mg，任意两帧差值超过这个值认为手腕开始运动 */
#define BMI270_WAKE_MIN_Z_RISE      100     /* mg，相比息屏基线，屏幕朝上/朝人方向明显增加 */
#define BMI270_WAKE_FACE_MIN_Z      650     /* mg，屏幕法线至少有一定朝上分量 */
#define BMI270_WAKE_STABLE_DELTA    450     /* mg，满足姿态后连续稳定几帧才亮屏 */
#define BMI270_WAKE_STABLE_SAMPLES  1
#define BMI270_WAKE_TOTAL_MIN       600     /* mg，排除读数异常 */
#define BMI270_WAKE_TOTAL_MAX       1900    /* mg，排除剧烈甩动 */
#define BMI270_WAKE_Y_DROP          380     /* mg，抬腕时 Y 轴通常明显下降 */
#define BMI270_WAKE_Y_NEGATIVE      (-80)   /* mg，Y 轴转负时通常朝向用户 */
#define BMI270_WAKE_POSE_DELTA      430     /* mg，兜底：整体姿态变化量 */
#define BMI270_WAKE_DEBUG_EVERY     5       /* 约 200ms 打印一次调试数据 */

static const char *TAG = "watch_bmi270";

/* 当前实际使用的 BMI270 I2C 地址。 */
static uint8_t s_bmi270_addr = BMI270_ADDR_HIGH;
/* BMI270 是否已经初始化成功。 */
static bool s_bmi270_ready = false;
/* 抬腕检测流程是否正在运行。 */
static bool s_raise_active = false;

/* 息屏时记录的 X 轴基线姿态。 */
static int16_t s_base_x = 0;
/* 息屏时记录的 Y 轴基线姿态。 */
static int16_t s_base_y = 0;
/* 息屏时记录的 Z 轴基线姿态。 */
static int16_t s_base_z = 0;
static int16_t s_last_x = 0;
static int16_t s_last_y = 0;
static int16_t s_last_z = 0;
static bool s_last_valid = false;
static bool s_seen_motion = false;
static uint8_t s_warmup_left = 0;
static uint8_t s_stable_count = 0;
static uint8_t s_debug_log_count = 0;

/**
 * @brief 将小端字节转换为 int16_t。
 *
 * 详细说明：
 * - BMI270 寄存器数据为低字节在前。
 *
 * @param p 输入或输出参数，具体含义见函数内部使用方式。
 */
static int16_t i16_from_le(const uint8_t *p)
{
    return (int16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

/**
 * @brief 读取 BMI270 寄存器。
 *
 * 详细说明：
 * - 封装 I2C 读操作和超时处理。
 *
 * @param reg 输入或输出参数，具体含义见函数内部使用方式。
 * @param data 输入或输出参数，具体含义见函数内部使用方式。
 * @param len 输入或输出参数，具体含义见函数内部使用方式。
 */
static esp_err_t bmi270_read(uint8_t reg, uint8_t *data, size_t len)
{
    return i2c_master_write_read_device(BMI270_I2C_PORT,
                                        s_bmi270_addr,
                                        &reg,
                                        1,
                                        data,
                                        len,
                                        pdMS_TO_TICKS(BMI270_I2C_TIMEOUT_MS));
}

/**
 * @brief 写入 BMI270 单字节寄存器。
 *
 * 详细说明：
 * - 用于配置功耗、量程、中断等寄存器。
 *
 * @param reg 输入或输出参数，具体含义见函数内部使用方式。
 * @param value 输入或输出参数，具体含义见函数内部使用方式。
 */
static esp_err_t bmi270_write_u8(uint8_t reg, uint8_t value)
{
    uint8_t buf[2] = { reg, value };

    return i2c_master_write_to_device(BMI270_I2C_PORT,
                                      s_bmi270_addr,
                                      buf,
                                      sizeof(buf),
                                      pdMS_TO_TICKS(BMI270_I2C_TIMEOUT_MS));
}

/**
 * @brief 连续写入 BMI270 寄存器。
 *
 * 详细说明：
 * - 主要用于加载 Bosch 配置文件。
 *
 * @param reg 输入或输出参数，具体含义见函数内部使用方式。
 * @param data 输入或输出参数，具体含义见函数内部使用方式。
 * @param len 输入或输出参数，具体含义见函数内部使用方式。
 */
static esp_err_t bmi270_write_burst(uint8_t reg, const uint8_t *data, size_t len)
{
    uint8_t buf[33];

    if(data == NULL || len == 0 || len > (sizeof(buf) - 1)) {
        return ESP_ERR_INVALID_ARG;
    }

    buf[0] = reg;
    for(size_t i = 0; i < len; i++) {
        buf[i + 1] = data[i];
    }

    return i2c_master_write_to_device(BMI270_I2C_PORT,
                                      s_bmi270_addr,
                                      buf,
                                      len + 1,
                                      pdMS_TO_TICKS(BMI270_I2C_TIMEOUT_MS));
}

/**
 * @brief 读取 BMI270 内部状态寄存器。
 *
 * 详细说明：
 * - 用于确认配置文件加载是否成功。
 *
 * @param status 输入或输出参数，具体含义见函数内部使用方式。
 */
static esp_err_t bmi270_read_internal_status(uint8_t *status)
{
    if(status == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    return bmi270_read(BMI270_REG_INTERNAL_STATUS, status, 1);
}

/**
 * @brief 按需加载 BMI270 配置文件。
 *
 * 详细说明：
 * - BMI270 初始化必须完成此步骤。
 * - 加载后检查内部状态，避免传感器假可用。
 */
static esp_err_t bmi270_config_load_if_needed(void)
{
    uint8_t status = 0;
    esp_err_t ret = bmi270_read_internal_status(&status);
    if(ret == ESP_OK && ((status & 0x0F) == 0x01)) {
        /* 已经 init_ok，不能重复下载 config。 */
        return ESP_OK;
    }

    ESP_LOGI(TAG, "BMI270 config not loaded, downloading base config file...");

    ret = bmi270_write_u8(BMI270_REG_PWR_CONF, BMI270_PWR_CONF_NORMAL);
    if(ret != ESP_OK) {
        return ret;
    }
    vTaskDelay(pdMS_TO_TICKS(1));

    ret = bmi270_write_u8(BMI270_REG_INIT_CTRL, 0x00);
    if(ret != ESP_OK) {
        return ret;
    }

    const uint8_t *cfg = WATCH_BMI270_CONFIG_FILE;
    const size_t cfg_len = WATCH_BMI270_CONFIG_FILE_SIZE;
    const size_t chunk_len = 32;

    for(size_t off = 0; off < cfg_len; off += chunk_len) {
        size_t n = cfg_len - off;
        if(n > chunk_len) {
            n = chunk_len;
        }

        uint16_t base = (uint16_t)(off / 2U);
        uint8_t addr0 = (uint8_t)(base & 0x0FU);
        uint8_t addr1 = (uint8_t)((base >> 4) & 0xFFU);

        ret = bmi270_write_u8(BMI270_REG_INIT_ADDR_0, addr0);
        if(ret != ESP_OK) {
            return ret;
        }
        ret = bmi270_write_u8(BMI270_REG_INIT_ADDR_1, addr1);
        if(ret != ESP_OK) {
            return ret;
        }
        ret = bmi270_write_burst(BMI270_REG_INIT_DATA, &cfg[off], n);
        if(ret != ESP_OK) {
            return ret;
        }
    }

    ret = bmi270_write_u8(BMI270_REG_INIT_CTRL, 0x01);
    if(ret != ESP_OK) {
        return ret;
    }

    vTaskDelay(pdMS_TO_TICKS(150));
    ret = bmi270_read_internal_status(&status);
    if(ret != ESP_OK) {
        return ret;
    }

    if((status & 0x0F) != 0x01) {
        ESP_LOGW(TAG, "BMI270 config load failed, INTERNAL_STATUS=0x%02X", status);
        return ESP_ERR_INVALID_RESPONSE;
    }

    ESP_LOGI(TAG, "BMI270 config load ok");
    return ESP_OK;
}

/**
 * @brief 尝试指定 I2C 地址并校验芯片 ID。
 *
 * 详细说明：
 * - 兼容 SDO 接地或上拉导致的 0x68/0x69 两种地址。
 *
 * @param addr 输入或输出参数，具体含义见函数内部使用方式。
 */
static esp_err_t bmi270_try_addr(uint8_t addr)
{
    uint8_t old_addr = s_bmi270_addr;
    uint8_t chip_id = 0;

    s_bmi270_addr = addr;
    esp_err_t ret = bmi270_read(BMI270_REG_CHIP_ID, &chip_id, 1);
    if(ret == ESP_OK && chip_id == BMI270_CHIP_ID) {
        ESP_LOGI(TAG, "BMI270 chip found, addr=0x%02X chip_id=0x%02X", addr, chip_id);
        return ESP_OK;
    }

    s_bmi270_addr = old_addr;
    if(ret == ESP_OK) {
        return ESP_ERR_NOT_FOUND;
    }
    return ret;
}

/**
 * @brief 读取三轴加速度并转换为 mg。
 *
 * 详细说明：
 * - 按 ±4g 量程的 LSB/g 系数换算。
 *
 * @param x_mg 输入或输出参数，具体含义见函数内部使用方式。
 * @param y_mg 输入或输出参数，具体含义见函数内部使用方式。
 * @param z_mg 输入或输出参数，具体含义见函数内部使用方式。
 */
static esp_err_t bmi270_read_accel_mg(int16_t *x_mg, int16_t *y_mg, int16_t *z_mg)
{
    uint8_t buf[6];
    esp_err_t ret;

    if(x_mg == NULL || y_mg == NULL || z_mg == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if(!s_bmi270_ready) {
        return ESP_ERR_INVALID_STATE;
    }

    ret = bmi270_read(BMI270_REG_DATA_8, buf, sizeof(buf));
    if(ret != ESP_OK) {
        return ret;
    }

    int16_t raw_x = i16_from_le(&buf[0]);
    int16_t raw_y = i16_from_le(&buf[2]);
    int16_t raw_z = i16_from_le(&buf[4]);

    *x_mg = (int16_t)(((int32_t)raw_x * 1000) / BMI270_LSB_PER_G_4G);
    *y_mg = (int16_t)(((int32_t)raw_y * 1000) / BMI270_LSB_PER_G_4G);
    *z_mg = (int16_t)(((int32_t)raw_z * 1000) / BMI270_LSB_PER_G_4G);

    return ESP_OK;
}

/**
 * @brief 读取状态寄存器以清除 data-ready latch。
 *
 * 详细说明：
 * - 读取状态/数据，避免旧中断影响后续唤醒判断。
 */
static void bmi270_clear_data_ready_latch(void)
{
    uint8_t int_status = 0;

    /*
     * INT_LATCH=1 时，INT_STATUS_1 里的 data-ready 标志和 INT 引脚
     * 需要通过读取 INT_STATUS_1 释放。DATA_8..DATA_13 的读取只负责清
     * STATUS.drdy_acc，不足以释放 latched INT 引脚。
     */
    (void)bmi270_read(BMI270_REG_INT_STATUS_1, &int_status, 1);
}

/**
 * @brief 强制关闭 BMI270 中断输出映射。
 *
 * 详细说明：
 * - 退出抬腕检测或正常运行前清理中断配置。
 */
static void bmi270_force_interrupts_off(void)
{
    uint8_t map_data = 0;

    /*
     * 启动/亮屏默认关闭 BMI270 INT 输出，避免 GPIO5 在系统还没进入
     * 息屏等待前保持高电平或触发普通 GPIO 中断。
     */
    if(bmi270_read(BMI270_REG_INT_MAP_DATA, &map_data, 1) == ESP_OK) {
        map_data &= (uint8_t)~BMI270_INT_MAP_DRDY_BOTH;
        (void)bmi270_write_u8(BMI270_REG_INT_MAP_DATA, map_data);
    }

    (void)bmi270_write_u8(BMI270_REG_INT_LATCH, 0x00);
    (void)bmi270_write_u8(BMI270_REG_INT1_IO_CTRL, 0x00);
    (void)bmi270_write_u8(BMI270_REG_INT2_IO_CTRL, 0x00);
    bmi270_clear_data_ready_latch();
}


/**
 * @brief 初始化 BMI270 加速度计。
 *
 * 详细说明：
 * - 尝试可用地址、加载配置、设置量程和功耗模式。
 */
esp_err_t watch_bmi270_init(void)
{
    ESP_LOGI(TAG, "watch_bmi270 config-file version start");

    if(s_bmi270_ready) {
        return ESP_OK;
    }

    /* 优先尝试高地址，失败后回退到低地址。 */
    esp_err_t ret = bmi270_try_addr(BMI270_ADDR_HIGH);
    if(ret != ESP_OK) {
        ret = bmi270_try_addr(BMI270_ADDR_LOW);
    }

    if(ret != ESP_OK) {
        ESP_LOGW(TAG, "BMI270 not found on shared I2C bus, raise-wrist wake disabled: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = bmi270_config_load_if_needed();
    if(ret != ESP_OK) {
        ESP_LOGW(TAG, "BMI270 config load/check failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = bmi270_write_u8(BMI270_REG_PWR_CONF, BMI270_PWR_CONF_NORMAL);
    if(ret != ESP_OK) {
        ESP_LOGW(TAG, "write PWR_CONF failed: %s", esp_err_to_name(ret));
        return ret;
    }
    vTaskDelay(pdMS_TO_TICKS(2));

    ret = bmi270_write_u8(BMI270_REG_ACC_CONF, BMI270_ACC_CONF_25HZ);
    if(ret != ESP_OK) {
        ESP_LOGW(TAG, "write ACC_CONF failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = bmi270_write_u8(BMI270_REG_ACC_RANGE, BMI270_ACC_RANGE_4G);
    if(ret != ESP_OK) {
        ESP_LOGW(TAG, "write ACC_RANGE failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = bmi270_write_u8(BMI270_REG_PWR_CTRL, BMI270_PWR_CTRL_ACC_ONLY);
    if(ret != ESP_OK) {
        ESP_LOGW(TAG, "write PWR_CTRL failed: %s", esp_err_to_name(ret));
        return ret;
    }

    /* 加速度计上电后等几帧数据稳定。 */
    vTaskDelay(pdMS_TO_TICKS(100));

    s_bmi270_ready = true;

    /* 默认不让 BMI270 INT 脚输出；只有息屏等待时才临时打开。 */
    bmi270_force_interrupts_off();

    int16_t x = 0;
    int16_t y = 0;
    int16_t z = 0;
    ret = bmi270_read_accel_mg(&x, &y, &z);
    if(ret == ESP_OK) {
        ESP_LOGI(TAG,
                 "BMI270 init ok, addr=0x%02X, acc=25Hz +/-4g, first acc: x=%dmg y=%dmg z=%dmg",
                 s_bmi270_addr,
                 x,
                 y,
                 z);
    }
    else {
        ESP_LOGW(TAG, "BMI270 init ok, but first accel read failed: %s", esp_err_to_name(ret));
    }

    return ESP_OK;
}

/**
 * @brief 查询 BMI270 是否就绪。
 *
 * 详细说明：
 * - 供低功耗和抬腕逻辑判断是否可以使用传感器。
 */
bool watch_bmi270_is_ready(void)
{
    return s_bmi270_ready;
}

/**
 * @brief 配置 BMI270 data-ready 中断。
 *
 * 详细说明：
 * - 用于轻睡眠唤醒或调试数据采样。
 *
 * @param enable 输入或输出参数，具体含义见函数内部使用方式。
 */
esp_err_t watch_bmi270_enable_data_ready_interrupt(bool enable)
{
    uint8_t map_data = 0;
    esp_err_t ret;

    if(!s_bmi270_ready) {
        return ESP_ERR_INVALID_STATE;
    }

    ret = bmi270_read(BMI270_REG_INT_MAP_DATA, &map_data, 1);
    if(ret != ESP_OK) {
        ESP_LOGW(TAG, "read INT_MAP_DATA failed: %s", esp_err_to_name(ret));
        return ret;
    }

    if(enable) {
        /*
         * data-ready 在非锁存模式下脉宽只有 1/6400s 左右，ESP32-S3 用 GPIO
         * 边沿中断可能漏掉；改成 latched，让 INT 脚保持高电平，任务读完数据
         * 并读取 INT_STATUS_1 后再释放。
         */
        ret = bmi270_write_u8(BMI270_REG_INT_LATCH, 0x01);
        if(ret != ESP_OK) {
            ESP_LOGW(TAG, "enable INT latch failed: %s", esp_err_to_name(ret));
            return ret;
        }

        ret = bmi270_write_u8(BMI270_REG_INT1_IO_CTRL, BMI270_INT_ACTIVE_HIGH_PP);
        if(ret != ESP_OK) {
            ESP_LOGW(TAG, "enable INT1 output failed: %s", esp_err_to_name(ret));
            return ret;
        }

        ret = bmi270_write_u8(BMI270_REG_INT2_IO_CTRL, BMI270_INT_ACTIVE_HIGH_PP);
        if(ret != ESP_OK) {
            ESP_LOGW(TAG, "enable INT2 output failed: %s", esp_err_to_name(ret));
            return ret;
        }

        map_data |= BMI270_INT_MAP_DRDY_BOTH;
    }
    else {
        map_data &= (uint8_t)~BMI270_INT_MAP_DRDY_BOTH;
    }

    ret = bmi270_write_u8(BMI270_REG_INT_MAP_DATA, map_data);
    if(ret != ESP_OK) {
        ESP_LOGW(TAG, "write INT_MAP_DATA failed: %s", esp_err_to_name(ret));
        return ret;
    }

    if(enable) {
        int16_t x = 0;
        int16_t y = 0;
        int16_t z = 0;

        /* 清掉启用映射前后可能已经挂起的一次 data-ready，等下一帧产生新边沿。 */
        (void)bmi270_read_accel_mg(&x, &y, &z);
        bmi270_clear_data_ready_latch();
    }
    else {
        /* 恢复为普通非锁存、INT 输出关闭状态，避免亮屏期间 GPIO5 常高。 */
        (void)bmi270_write_u8(BMI270_REG_INT_LATCH, 0x00);
        (void)bmi270_write_u8(BMI270_REG_INT1_IO_CTRL, 0x00);
        (void)bmi270_write_u8(BMI270_REG_INT2_IO_CTRL, 0x00);
        bmi270_clear_data_ready_latch();
    }

    ESP_LOGI(TAG,
             "BMI270 data-ready INT %s, INT_MAP_DATA=0x%02X",
             enable ? "enabled" : "disabled",
             map_data);
    return ESP_OK;
}


/**
 * @brief 开始抬腕检测并记录基线。
 *
 * 详细说明：
 * - 记录息屏时的基线姿态，并打开必要中断。
 */
void watch_bmi270_raise_wrist_begin(void)
{
    int16_t x = 0;
    int16_t y = 0;
    int16_t z = 0;

    s_raise_active = false;
    s_last_valid = false;
    s_seen_motion = false;
    s_warmup_left = BMI270_WAKE_WARMUP_SAMPLES;
    s_stable_count = 0;
    s_debug_log_count = 0;
    if(!s_bmi270_ready) {
        return;
    }

    if(bmi270_read_accel_mg(&x, &y, &z) == ESP_OK) {
        s_base_x = x;
        s_base_y = y;
        s_base_z = z;
        s_last_x = x;
        s_last_y = y;
        s_last_z = z;
        s_last_valid = true;
        s_raise_active = true;
        ESP_LOGI(TAG, "raise-wrist baseline: x=%dmg y=%dmg z=%dmg", x, y, z);
    }
}

/**
 * @brief 轮询抬腕动作。
 *
 * 详细说明：
 * - 结合运动幅度、姿态变化和稳定性判断亮屏条件。
 */
bool watch_bmi270_raise_wrist_poll(void)
{
    if(!s_raise_active || !s_bmi270_ready) {
        return false;
    }

    /* 由 data-ready INT 触发轮询，必须每次读取并清除 latch。 */

    int16_t x = 0;
    int16_t y = 0;
    int16_t z = 0;
    esp_err_t ret = bmi270_read_accel_mg(&x, &y, &z);
    bmi270_clear_data_ready_latch();
    if(ret != ESP_OK) {
        ESP_LOGW(TAG, "read accel failed: %s", esp_err_to_name(ret));
        return false;
    }

    if(s_warmup_left > 0) {
        s_base_x = x;
        s_base_y = y;
        s_base_z = z;
        s_last_x = x;
        s_last_y = y;
        s_last_z = z;
        s_last_valid = true;
        s_warmup_left--;
        return false;
    }

    if(!s_last_valid) {
        s_last_x = x;
        s_last_y = y;
        s_last_z = z;
        s_last_valid = true;
        return false;
    }

    int dx = abs((int)x - (int)s_last_x);
    int dy = abs((int)y - (int)s_last_y);
    int dz = abs((int)z - (int)s_last_z);
    int frame_delta = dx + dy + dz;

    int bx = abs((int)x - (int)s_base_x);
    int by = abs((int)y - (int)s_base_y);
    int bz = abs((int)z - (int)s_base_z);
    int pose_delta = bx + by + bz;

    int z_rise = (int)z - (int)s_base_z;
    int y_drop = (int)s_base_y - (int)y;

    s_last_x = x;
    s_last_y = y;
    s_last_z = z;

    if(frame_delta >= BMI270_WAKE_MIN_ARM_MOTION || pose_delta >= BMI270_WAKE_POSE_DELTA) {
        s_seen_motion = true;
    }

    int total_abs = abs((int)x) + abs((int)y) + abs((int)z);
    bool gravity_ok = (total_abs >= BMI270_WAKE_TOTAL_MIN) && (total_abs <= BMI270_WAKE_TOTAL_MAX);

    /* 综合 Z 轴抬升、Y 轴下降和整体姿态变化判断抬腕。 */
    bool z_lift = (z >= BMI270_WAKE_FACE_MIN_Z) && (z_rise >= BMI270_WAKE_MIN_Z_RISE);
    bool y_turn = (z >= BMI270_WAKE_FACE_MIN_Z) && (y_drop >= BMI270_WAKE_Y_DROP);
    bool y_negative = (z >= BMI270_WAKE_FACE_MIN_Z) && (y <= BMI270_WAKE_Y_NEGATIVE);
    bool pose_turn = (z >= BMI270_WAKE_FACE_MIN_Z) && (pose_delta >= BMI270_WAKE_POSE_DELTA);

    bool face_towards_user = z_lift || y_turn || y_negative || pose_turn;
    bool stable_enough = frame_delta <= BMI270_WAKE_STABLE_DELTA;

    s_debug_log_count++;
    if(s_debug_log_count >= BMI270_WAKE_DEBUG_EVERY) {
        s_debug_log_count = 0;
        ESP_LOGI(TAG,
                 "acc x=%d y=%d z=%d base=%d/%d/%d frame=%d pose=%d z_rise=%d y_drop=%d seen=%d face=%d stable=%d total=%d",
                 x,
                 y,
                 z,
                 s_base_x,
                 s_base_y,
                 s_base_z,
                 frame_delta,
                 pose_delta,
                 z_rise,
                 y_drop,
                 s_seen_motion ? 1 : 0,
                 face_towards_user ? 1 : 0,
                 stable_enough ? 1 : 0,
                 total_abs);
    }

    if(s_seen_motion && gravity_ok && face_towards_user && stable_enough) {
        s_stable_count++;
        if(s_stable_count >= BMI270_WAKE_STABLE_SAMPLES) {
            ESP_LOGI(TAG,
                     "raise-wrist wake: x=%dmg y=%dmg z=%dmg base=%d/%d/%d frame=%d pose=%d z_rise=%d y_drop=%d",
                     x,
                     y,
                     z,
                     s_base_x,
                     s_base_y,
                     s_base_z,
                     frame_delta,
                     pose_delta,
                     z_rise,
                     y_drop);
            return true;
        }
    }
    else {
        s_stable_count = 0;
    }

    return false;
}

/**
 * @brief 结束抬腕检测。
 *
 * 详细说明：
 * - 关闭中断并清理检测状态。
 */
void watch_bmi270_raise_wrist_end(void)
{
    s_raise_active = false;
    s_seen_motion = false;
    s_stable_count = 0;
    s_debug_log_count = 0;
}
