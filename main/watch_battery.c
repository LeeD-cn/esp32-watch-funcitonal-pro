/**
 * @file watch_battery.c
 * @brief CW2015 电量计读取和 TP4056 充电状态检测实现。
 */
/*
 * ==================== 模块说明 ====================
 *  * 模块职责：
 * - 封装 CW2015 电量计读取和 TP4056 充电状态检测。
 * - CW2015 通过 I2C 读取电压/SOC，TP4056 CHRG 引脚用 GPIO 输入判断是否正在充电。
 * - 包含芯片探测、快速启动、睡眠/正常模式寄存器处理，尽量避免在未探测成功时反复访问总线。
 * - 当电量计数据不可用时，通过单节锂电池电压曲线估算百分比作为兜底。
 *
 * 阅读建议：
 * - 先看本文件顶部的宏定义和静态状态变量，理解硬件参数和运行状态。
 * - 再看 reset/init/start/on_key/destroy 等对外函数，理解页面或驱动的生命周期。
 * - 最后看 static 辅助函数，了解具体寄存器读写、UI 刷新或状态机细节。
 * =======================================================
 */


#include "watch_battery.h"

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include "driver/gpio.h"
#include "driver/i2c.h"
#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/* CW2015CHBD 与 PCF85063 RTC、BMI270 共用 I2C：GPIO6=SDA，GPIO7=SCL。 */
#define BAT_I2C_PORT                I2C_NUM_1
#define BAT_SDA_IO                  GPIO_NUM_6
#define BAT_SCL_IO                  GPIO_NUM_7
#define BAT_CHRG_IO                 GPIO_NUM_36  /* TP4056 CHRG：低电平表示正在充电。 */
#define BAT_I2C_CLK_HZ              100000
#define BAT_I2C_TIMEOUT_MS          120

#define CW2015_I2C_ADDR             0x62

#define CW2015_REG_VERSION          0x00
#define CW2015_REG_VCELL            0x02    /* 0x02~0x03 */
#define CW2015_REG_SOC              0x04    /* 0x04~0x05 */
#define CW2015_REG_RRT_ALERT        0x06
#define CW2015_REG_CONFIG           0x08
#define CW2015_REG_MODE             0x0A

/* CW2015 MODE: bit[7:6] = sleep control，00 normal，11 sleep。bit[5:4] = quick start。 */
#define CW2015_MODE_SLEEP_MASK      0xC0
#define CW2015_MODE_SLEEP           0xC0
#define CW2015_MODE_NORMAL          0x00
#define CW2015_MODE_QUICK_START     0x30
#define CW2015_MODE_RESTART         0x0F

static const char *TAG = "watch_battery";
/* 电量计 I2C 总线是否已初始化。 */
static bool s_battery_i2c_ready = false;
/* TP4056 CHRG GPIO 是否已配置完成。 */
static bool s_chrg_gpio_ready = false;
/* CW2015 电量计是否探测成功并可用。 */
static bool s_battery_chip_ready = false;
/* 是否已经执行过电量计探测流程。 */
static bool s_battery_probe_done = false;
/* 是否已经输出过一次电池状态日志，避免日志刷屏。 */
static bool s_battery_logged_once = false;
/* 是否已经对 CW2015 执行过 quick start。 */
static bool s_quick_start_done = false;
static esp_err_t s_battery_probe_ret = ESP_ERR_INVALID_STATE;

/**
 * @brief 初始化电量计所在 I2C 总线。
 *
 * 详细说明：
 * - 安装或复用 I2C driver，确保共享总线配置一致。
 * - 不删除已有 driver，避免影响 QMC5883P、RTC、BMI270 等同总线设备。
 */
static esp_err_t battery_i2c_init(void)
{
    if(s_battery_i2c_ready) {
        return ESP_OK;
    }

    /*
     * 共享 I2C 总线：GPIO6/GPIO7 同时挂了 PCF85063、CW2015、BMI270、QMC5883P。
     * 因此这里绝不能 i2c_driver_delete()，也不能把“driver 已被别的模块安装”当成失败。
     */
    i2c_config_t conf = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = BAT_SDA_IO,
        .scl_io_num = BAT_SCL_IO,
        .sda_pullup_en = GPIO_PULLUP_ENABLE,
        .scl_pullup_en = GPIO_PULLUP_ENABLE,
        .master.clk_speed = BAT_I2C_CLK_HZ,
        .clk_flags = 0,
    };

    esp_err_t ret = i2c_param_config(BAT_I2C_PORT, &conf);
    if(ret != ESP_OK) {
        ESP_LOGW(TAG, "I2C param config failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = i2c_driver_install(BAT_I2C_PORT, I2C_MODE_MASTER, 0, 0, 0);
    if(ret == ESP_OK || ret == ESP_ERR_INVALID_STATE) {
        s_battery_i2c_ready = true;
        return ESP_OK;
    }

    /*
     * ESP-IDF 旧版 I2C 驱动在重复 install 时，有些情况下会打印
     * "i2c driver install error" 并返回 ESP_FAIL，而不是 ESP_ERR_INVALID_STATE。
     * 启动日志显示 RTC 已经成功安装 I2C1，所以这里把 ESP_FAIL 当作“尝试复用现有 driver”。
     * 后续真正的读写事务会验证总线是否可用。
     */
    if(ret == ESP_FAIL) {
        ESP_LOGW(TAG,
                 "I2C driver install returned ESP_FAIL, reuse existing I2C driver on port %d",
                 BAT_I2C_PORT);
        s_battery_i2c_ready = true;
        return ESP_OK;
    }

    ESP_LOGW(TAG, "I2C driver install failed: %s", esp_err_to_name(ret));
    return ret;
}

/**
 * @brief 标记 I2C 总线需要重新确认初始化状态。
 *
 * 详细说明：
 * - 当一次读写失败后，仅清除本模块的 ready 标志。
 * - 下一次访问会重新执行 param_config/install；如果 driver 已存在则直接复用。
 * - 不调用 i2c_driver_delete，避免破坏共享总线上的其他设备。
 */
static void battery_i2c_mark_need_reinit(void)
{
    s_battery_i2c_ready = false;
}

/**
 * @brief 初始化 TP4056 CHRG 检测 GPIO。
 *
 * 详细说明：
 * - CHRG 低电平表示正在充电。
 */
static esp_err_t battery_chrg_gpio_init(void)
{
    if(s_chrg_gpio_ready) {
        return ESP_OK;
    }

    /* CHRG 为开漏输出，启用内部上拉避免悬空。 */
    gpio_config_t conf = {
        .pin_bit_mask = 1ULL << BAT_CHRG_IO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };

    esp_err_t ret = gpio_config(&conf);
    if(ret != ESP_OK) {
        ESP_LOGW(TAG, "CHRG GPIO config failed: %s", esp_err_to_name(ret));
        return ret;
    }

    s_chrg_gpio_ready = true;
    return ESP_OK;
}

/**
 * @brief 读取 CW2015 连续寄存器。
 *
 * 详细说明：
 * - 用于读取电压、SOC、版本等寄存器。
 *
 * @param reg 输入或输出参数，具体含义见函数内部使用方式。
 * @param data 输入或输出参数，具体含义见函数内部使用方式。
 * @param len 输入或输出参数，具体含义见函数内部使用方式。
 */
static esp_err_t battery_read_regs(uint8_t reg, uint8_t *data, size_t len)
{
    if(!s_battery_i2c_ready) {
        esp_err_t ret = battery_i2c_init();
        if(ret != ESP_OK) {
            return ret;
        }
    }

    return i2c_master_write_read_device(BAT_I2C_PORT,
                                        CW2015_I2C_ADDR,
                                        &reg,
                                        1,
                                        data,
                                        len,
                                        pdMS_TO_TICKS(BAT_I2C_TIMEOUT_MS));
}

/**
 * @brief 写入 CW2015 单个寄存器。
 *
 * 详细说明：
 * - 用于模式切换、快速启动等控制操作。
 *
 * @param reg 输入或输出参数，具体含义见函数内部使用方式。
 * @param value 输入或输出参数，具体含义见函数内部使用方式。
 */
static esp_err_t battery_write_reg(uint8_t reg, uint8_t value)
{
    if(!s_battery_i2c_ready) {
        esp_err_t ret = battery_i2c_init();
        if(ret != ESP_OK) {
            return ret;
        }
    }

    uint8_t buf[2] = {reg, value};

    return i2c_master_write_to_device(BAT_I2C_PORT,
                                      CW2015_I2C_ADDR,
                                      buf,
                                      sizeof(buf),
                                      pdMS_TO_TICKS(BAT_I2C_TIMEOUT_MS));
}

/**
 * @brief 在不重新初始化的情况下读取电池电压。
 *
 * 详细说明：
 * - 适合内部流程多次读取，减少初始化开销。
 *
 * @param voltage_mv 输入或输出参数，具体含义见函数内部使用方式。
 */
static esp_err_t battery_read_voltage_mv_no_init(int *voltage_mv)
{
    if(voltage_mv == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t vcell[2] = {0};
    esp_err_t ret = battery_read_regs(CW2015_REG_VCELL, vcell, sizeof(vcell));
    if(ret != ESP_OK) {
        ESP_LOGW(TAG, "read VCELL failed: %s", esp_err_to_name(ret));
        return ret;
    }

    uint16_t raw = ((uint16_t)vcell[0] << 8) | vcell[1];
    raw &= 0x3FFF;

    /* CW2015 VCELL LSB 约 305uV。 */
    *voltage_mv = (int)(((uint32_t)raw * 305U) / 1000U);
    return ESP_OK;
}

/**
 * @brief 将单节锂电电压估算为电量百分比。
 *
 * 详细说明：
 * - 作为 CW2015 SOC 不可用时的兜底算法。
 * - 曲线仅为近似值，实际百分比会受电池和负载影响。
 *
 * @param mv 输入或输出参数，具体含义见函数内部使用方式。
 */
static int voltage_to_percent_1s_liion(int mv)
{
    /* CW2015 SOC 不可靠时，用单节锂电电压估算百分比。 */
    if(mv >= 4200) {
        return 100;
    }
    if(mv <= 3300) {
        return 0;
    }

    typedef struct {
        int mv;
        int pct;
    } point_t;

    static const point_t table[] = {
        {4200, 100},
        {4100, 90},
        {4000, 80},
        {3920, 70},
        {3850, 60},
        {3790, 50},
        {3740, 40},
        {3700, 30},
        {3630, 20},
        {3550, 10},
        {3300, 0},
    };

    for(size_t i = 0; i + 1 < sizeof(table) / sizeof(table[0]); i++) {
        if(mv <= table[i].mv && mv >= table[i + 1].mv) {
            int x0 = table[i].mv;
            int y0 = table[i].pct;
            int x1 = table[i + 1].mv;
            int y1 = table[i + 1].pct;

            return y1 + ((mv - x1) * (y0 - y1)) / (x0 - x1);
        }
    }

    return 0;
}

/**
 * @brief 初始化电量检测模块。
 *
 * 详细说明：
 * - 完成 I2C、GPIO 和 CW2015 探测。
 */
esp_err_t watch_battery_init(void)
{
    if(s_battery_chip_ready) {
        return ESP_OK;
    }

    /* 探测失败后不再反复访问 I2C，避免周期刷新造成异常。 */
    if(s_battery_probe_done && s_battery_probe_ret != ESP_OK) {
        return s_battery_probe_ret;
    }

    esp_err_t ret = battery_i2c_init();
    if(ret != ESP_OK) {
        /* I2C 初始化失败通常是共享总线暂态问题，不永久锁死电池探测。 */
        s_battery_chip_ready = false;
        s_battery_probe_done = false;
        s_battery_probe_ret = ret;
        return ret;
    }

    /* CHRG GPIO 与 CW2015 I2C 互不影响。 */
    esp_err_t chrg_ret = battery_chrg_gpio_init();
    if(chrg_ret != ESP_OK) {
        ESP_LOGW(TAG, "CHRG GPIO init failed: %s", esp_err_to_name(chrg_ret));
    }

    uint8_t version = 0;
    ret = battery_read_regs(CW2015_REG_VERSION, &version, 1);
    if(ret != ESP_OK) {
        ESP_LOGW(TAG, "CW2015 not found at 0x%02X: %s; battery UI will show --", CW2015_I2C_ADDR, esp_err_to_name(ret));
        s_battery_chip_ready = false;
        s_battery_probe_done = true;
        s_battery_probe_ret = ret;
        return ret;
    }

    uint8_t mode = 0;
    ret = battery_read_regs(CW2015_REG_MODE, &mode, 1);
    if(ret != ESP_OK) {
        ESP_LOGW(TAG, "read MODE failed: %s", esp_err_to_name(ret));
        s_battery_chip_ready = false;
        s_battery_probe_done = true;
        s_battery_probe_ret = ret;
        return ret;
    }

    if((mode & CW2015_MODE_SLEEP_MASK) == CW2015_MODE_SLEEP) {
        ret = battery_write_reg(CW2015_REG_MODE, CW2015_MODE_NORMAL);
        if(ret != ESP_OK) {
            ESP_LOGW(TAG, "wake CW2015 failed: %s", esp_err_to_name(ret));
            s_battery_chip_ready = false;
            s_battery_probe_done = true;
            s_battery_probe_ret = ret;
            return ret;
        }
        vTaskDelay(pdMS_TO_TICKS(50));
        mode = CW2015_MODE_NORMAL;
    }

    /* 上电后仅执行一次 quick start，避免反复重置估算状态。 */
    if(!s_quick_start_done) {
        (void)battery_write_reg(CW2015_REG_MODE, CW2015_MODE_QUICK_START);
        vTaskDelay(pdMS_TO_TICKS(20));
        (void)battery_write_reg(CW2015_REG_MODE, CW2015_MODE_NORMAL);
        vTaskDelay(pdMS_TO_TICKS(120));
        s_quick_start_done = true;
    }

    s_battery_chip_ready = true;
    s_battery_probe_done = true;
    s_battery_probe_ret = ESP_OK;

    if(!s_battery_logged_once) {
        uint8_t config = 0;
        uint8_t soc[2] = {0};
        int mv = 0;

        (void)battery_read_regs(CW2015_REG_CONFIG, &config, 1);
        (void)battery_read_regs(CW2015_REG_SOC, soc, sizeof(soc));
        (void)battery_read_voltage_mv_no_init(&mv);

        ESP_LOGI(TAG,
                 "CW2015 init ok, version=0x%02X mode=0x%02X config=0x%02X soc=%u.%02u vcell=%dmV, SDA=GPIO%d SCL=GPIO%d",
                 version,
                 mode,
                 config,
                 soc[0],
                 ((unsigned)soc[1] * 100U) / 256U,
                 mv,
                 BAT_SDA_IO,
                 BAT_SCL_IO);

        s_battery_logged_once = true;
    }

    return ESP_OK;
}

/**
 * @brief 读取电池电压。
 *
 * 详细说明：
 * - 对外接口，内部会确保初始化流程已执行。
 *
 * @param voltage_mv 输入或输出参数，具体含义见函数内部使用方式。
 */
esp_err_t watch_battery_read_voltage_mv(int *voltage_mv)
{
    if(voltage_mv == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t ret = watch_battery_init();
    if(ret != ESP_OK || !s_battery_chip_ready) {
        return ret != ESP_OK ? ret : ESP_ERR_INVALID_STATE;
    }

    return battery_read_voltage_mv_no_init(voltage_mv);
}

/**
 * @brief 读取充电状态。
 *
 * 详细说明：
 * - 读取 TP4056 CHRG 引脚并转换为布尔状态。
 *
 * @param charging 输入或输出参数，具体含义见函数内部使用方式。
 */
esp_err_t watch_battery_is_charging(bool *charging)
{
    if(charging == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t ret = battery_chrg_gpio_init();
    if(ret != ESP_OK) {
        return ret;
    }

    /* TP4056 CHRG 为低电平时表示正在充电。 */
    *charging = (gpio_get_level(BAT_CHRG_IO) == 0);
    return ESP_OK;
}

/**
 * @brief 读取电池电量百分比。
 *
 * 详细说明：
 * - 优先使用 CW2015 SOC。
 * - 异常时回退到电压估算，保证 UI 有可显示数据。
 *
 * @param percent 输入或输出参数，具体含义见函数内部使用方式。
 */
esp_err_t watch_battery_read_percent(int *percent)
{
    if(percent == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t ret = watch_battery_init();
    if(ret != ESP_OK || !s_battery_chip_ready) {
        return ret != ESP_OK ? ret : ESP_ERR_INVALID_STATE;
    }

    uint8_t soc[2] = {0};
    esp_err_t soc_ret = battery_read_regs(CW2015_REG_SOC, soc, sizeof(soc));

    int voltage_mv = 0;
    esp_err_t vol_ret = battery_read_voltage_mv_no_init(&voltage_mv);

    if(soc_ret == ESP_OK) {
        int value = (int)soc[0];
        if(soc[1] >= 128 && value < 100) {
            value++;
        }

        if(value >= 0 && value <= 100) {
            /*
             * 如果 SOC 读数为 0，但电池电压明显不是空电，说明芯片还没完成配置/估算，
             * 先用电压兜底，否则屏幕会一直显示 0 或 --。
             */
            if(!(value == 0 && vol_ret == ESP_OK && voltage_mv > 3400)) {
                *percent = value;
                ESP_LOGI(TAG,
                         "battery SOC=%d%% raw=%u.%02u vcell=%dmV",
                         value,
                         soc[0],
                         ((unsigned)soc[1] * 100U) / 256U,
                         voltage_mv);
                return ESP_OK;
            }
        }
    } else {
        ESP_LOGW(TAG, "read SOC failed: %s", esp_err_to_name(soc_ret));
    }

    if(vol_ret == ESP_OK) {
        int value = voltage_to_percent_1s_liion(voltage_mv);
        if(value < 0) {
            value = 0;
        }
        if(value > 100) {
            value = 100;
        }

        *percent = value;
        ESP_LOGI(TAG,
                 "battery fallback by voltage: %d%%, vcell=%dmV, soc_raw=%u.%02u",
                 value,
                 voltage_mv,
                 soc[0],
                 ((unsigned)soc[1] * 100U) / 256U);
        return ESP_OK;
    }

    /* SOC 和电压都失败时，说明共享 I2C 总线可能刚被其他页面切换影响。
     * 不删除 driver，只清除本模块 ready 标志并复用/重新确认初始化后再读一次，
     * 避免首页电量直接显示 --。
     */
    ESP_LOGW(TAG,
             "battery read failed, retry I2C once: soc=%s, vcell=%s",
             esp_err_to_name(soc_ret),
             esp_err_to_name(vol_ret));

    battery_i2c_mark_need_reinit();
    ret = battery_i2c_init();
    if(ret == ESP_OK) {
        memset(soc, 0, sizeof(soc));
        voltage_mv = 0;

        soc_ret = battery_read_regs(CW2015_REG_SOC, soc, sizeof(soc));
        vol_ret = battery_read_voltage_mv_no_init(&voltage_mv);

        if(soc_ret == ESP_OK) {
            int value = (int)soc[0];
            if(soc[1] >= 128 && value < 100) {
                value++;
            }

            if(value >= 0 && value <= 100 && !(value == 0 && vol_ret == ESP_OK && voltage_mv > 3400)) {
                *percent = value;
                ESP_LOGI(TAG,
                         "battery SOC=%d%% after retry raw=%u.%02u vcell=%dmV",
                         value,
                         soc[0],
                         ((unsigned)soc[1] * 100U) / 256U,
                         voltage_mv);
                return ESP_OK;
            }
        }

        if(vol_ret == ESP_OK) {
            int value = voltage_to_percent_1s_liion(voltage_mv);
            if(value < 0) {
                value = 0;
            }
            if(value > 100) {
                value = 100;
            }

            *percent = value;
            ESP_LOGI(TAG,
                     "battery fallback by voltage after retry: %d%%, vcell=%dmV, soc_raw=%u.%02u",
                     value,
                     voltage_mv,
                     soc[0],
                     ((unsigned)soc[1] * 100U) / 256U);
            return ESP_OK;
        }
    }

    return soc_ret != ESP_OK ? soc_ret : vol_ret;
}
