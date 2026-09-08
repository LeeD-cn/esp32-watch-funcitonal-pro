/**
 * @file watch_rtc.c
 * @brief PCF85063 RTC 初始化、读写和 ESP32 系统时间同步。
 */


/**
 * @section 模块说明
 * 本文件是“外部 RTC <-> ESP32 系统时间”的桥接层：
 * 1. 负责初始化 PCF85063 所在的 I2C1 总线；
 * 2. 读取 RTC BCD 寄存器并转换为 struct tm；
 * 3. 当 RTC 停振、时间非法或从未校时时，使用 2026-01-01 作为兜底启动时间；
 * 4. 将 RTC 时间同步到 ESP32 系统时间，或将 SNTP/系统时间回写到 RTC。
 *
 * 阅读建议：先看 watch_rtc_restore_or_init_2026()，它体现了上电后的完整恢复流程；
 * 再看 rtc_read_time()/rtc_write_time()，理解 PCF85063 寄存器格式与 BCD 转换。
 */

#include "watch_rtc.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>

#include "driver/gpio.h"
#include "driver/i2c.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/* 中国时区 UTC+8。POSIX TZ 写法里 CST-8 表示本地时间 = UTC + 8。 */
#define WATCH_TZ                    "CST-8"

#define RTC_I2C_PORT                I2C_NUM_1
#define RTC_SDA_IO                  GPIO_NUM_6
#define RTC_SCL_IO                  GPIO_NUM_7
#define RTC_I2C_FREQ_HZ             100000
#define RTC_I2C_TIMEOUT_MS          1000

#define PCF85063_I2C_ADDR           0x51

#define PCF85063_REG_CONTROL_1      0x00
#define PCF85063_REG_CONTROL_2      0x01
#define PCF85063_REG_SECONDS        0x04

#define PCF85063_SECONDS_OS_BIT     0x80
#define PCF85063_CONTROL_2_CLKOUT_OFF 0x07  /* COF[2:0] = 111，关闭 CLKOUT 省电。 */

/* RTC 有效时间范围及无网络时的默认启动时间。 */
#define RTC_MIN_VALID_YEAR          2026
#define RTC_MAX_VALID_YEAR          2099

#define RTC_BOOT_YEAR               2026
#define RTC_BOOT_MON                1
#define RTC_BOOT_MDAY               1
#define RTC_BOOT_HOUR               0
#define RTC_BOOT_MIN                0
#define RTC_BOOT_SEC                0
#define RTC_BOOT_WDAY               4       /* 2026/1/1 是星期四；0=SUN。 */

/* 日志 TAG：用于区分 RTC 模块输出，排查 I2C/时间恢复问题时重点关注。 */
static const char *TAG = "watch_rtc";
/* I2C 初始化标志：防止重复安装同一 I2C driver。 */
static bool s_rtc_i2c_ready = false;

static void watch_tz_init(void)
{
    /* 设置进程时区为中国时区。ESP-IDF 的 localtime/mktime 会读取 TZ 环境变量，因此在读写
     * RTC 前先统一时区，避免 UTC 与本地时间相差 8 小时。
     */
    setenv("TZ", WATCH_TZ, 1);
    tzset();
}

static uint8_t bcd_to_bin(uint8_t bcd)
{
    /* PCF85063 的时间寄存器使用 BCD 编码，本函数把高 4 位十位和低 4 位个位转换为普通整数，便于填入
     * struct tm。
     */
    return (uint8_t)(((bcd >> 4) * 10U) + (bcd & 0x0FU));
}

static uint8_t bin_to_bcd(uint8_t bin)
{
    /* 写回 RTC 前需要把普通整数转换成 BCD，保证寄存器格式与芯片手册一致。
     */
    return (uint8_t)(((bin / 10U) << 4) | (bin % 10U));
}

static int pcf85063_year_to_full(uint8_t yy)
{
    /* PCF85063 只保存 00~99 的年份，这里统一映射到 2000~2099，配合有效年份范围检查使用。
     */
    return 2000 + yy;
}

static bool rtc_tm_is_valid(const struct tm *t)
{
    /* 对 struct tm 做基础范围检查。它不校验每个月的最大天数，只过滤明显非法值，避免把损坏寄存器写入系统时间。
     */
    int year = t->tm_year + 1900;

    if(year < RTC_MIN_VALID_YEAR || year > RTC_MAX_VALID_YEAR) {
        return false;
    }

    if(t->tm_mon < 0 || t->tm_mon > 11) {
        return false;
    }

    if(t->tm_mday < 1 || t->tm_mday > 31) {
        return false;
    }

    if(t->tm_hour < 0 || t->tm_hour > 23) {
        return false;
    }

    if(t->tm_min < 0 || t->tm_min > 59) {
        return false;
    }

    if(t->tm_sec < 0 || t->tm_sec > 59) {
        return false;
    }

    return true;
}

static void rtc_default_tm_2026(struct tm *t)
{
    /* 构造无网络、RTC 无效时的兜底启动时间。选择固定日期可以让系统时间至少落在项目定义的有效年份范围内。
     */
    memset(t, 0, sizeof(*t));
    t->tm_year = RTC_BOOT_YEAR - 1900;
    t->tm_mon = RTC_BOOT_MON - 1;
    t->tm_mday = RTC_BOOT_MDAY;
    t->tm_hour = RTC_BOOT_HOUR;
    t->tm_min = RTC_BOOT_MIN;
    t->tm_sec = RTC_BOOT_SEC;
    t->tm_wday = RTC_BOOT_WDAY;
    t->tm_isdst = -1;
}

static esp_err_t rtc_read_regs(uint8_t reg, uint8_t *data, size_t len)
{
    /* RTC I2C 读寄存器封装：先写寄存器地址，再连续读取指定长度数据。上层不需要关心 I2C 事务细节。
     */
    return i2c_master_write_read_device(RTC_I2C_PORT,
                                        PCF85063_I2C_ADDR,
                                        &reg,
                                        1,
                                        data,
                                        len,
                                        pdMS_TO_TICKS(RTC_I2C_TIMEOUT_MS));
}

static esp_err_t rtc_write_regs(uint8_t reg, const uint8_t *data, size_t len)
{
    /* RTC I2C 写寄存器封装：把起始寄存器地址和数据拼成一个连续缓冲区后一次写入。
     */
    uint8_t buf[16];

    if(len + 1 > sizeof(buf)) {
        return ESP_ERR_INVALID_SIZE;
    }

    buf[0] = reg;
    memcpy(&buf[1], data, len);

    return i2c_master_write_to_device(RTC_I2C_PORT,
                                      PCF85063_I2C_ADDR,
                                      buf,
                                      len + 1,
                                      pdMS_TO_TICKS(RTC_I2C_TIMEOUT_MS));
}

/**
 * @brief 从 PCF85063 读取时间并转换为 struct tm。
 *
 * @param t 输出的本地时间。
 * @param oscillator_ok 输出振荡器状态，true 表示 RTC 未检测到停振。
 * @return ESP_OK 表示读取成功且时间有效。
 */
static esp_err_t rtc_read_time(struct tm *t, bool *oscillator_ok)
{
    /* 读取 PCF85063 的秒、分、时、日、星期、月、年 7 个时间寄存器，并转换为 struct tm；同时检查
     * OS 停振标志和时间合法性。
     */
    uint8_t buf[7];
    esp_err_t ret;

    if(t == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    ret = rtc_read_regs(PCF85063_REG_SECONDS, buf, sizeof(buf));
    if(ret != ESP_OK) {
        ESP_LOGW(TAG, "read RTC failed: %s", esp_err_to_name(ret));
        return ret;
    }

    bool os = (buf[0] & PCF85063_SECONDS_OS_BIT) != 0;
    if(oscillator_ok) {
        *oscillator_ok = !os;
    }

    uint8_t yy = bcd_to_bin(buf[6]);

    memset(t, 0, sizeof(*t));
    t->tm_sec = bcd_to_bin(buf[0] & 0x7F);
    t->tm_min = bcd_to_bin(buf[1] & 0x7F);
    t->tm_hour = bcd_to_bin(buf[2] & 0x3F);
    t->tm_mday = bcd_to_bin(buf[3] & 0x3F);
    t->tm_wday = bcd_to_bin(buf[4] & 0x07);
    t->tm_mon = bcd_to_bin(buf[5] & 0x1F) - 1;
    t->tm_year = pcf85063_year_to_full(yy) - 1900;
    t->tm_isdst = -1;

    if(os) {
        ESP_LOGW(TAG, "RTC oscillator stop flag is set, RTC will be initialized to 2026");
        return ESP_ERR_INVALID_STATE;
    }

    if(!rtc_tm_is_valid(t)) {
        ESP_LOGW(TAG,
                 "RTC time invalid: %04d/%02d/%02d %02d:%02d:%02d",
                 t->tm_year + 1900,
                 t->tm_mon + 1,
                 t->tm_mday,
                 t->tm_hour,
                 t->tm_min,
                 t->tm_sec);
        return ESP_ERR_INVALID_RESPONSE;
    }

    return ESP_OK;
}

/**
 * @brief 将 struct tm 时间写入 PCF85063。
 *
 * @param t 待写入的本地时间。
 * @return ESP_OK 表示写入成功。
 */
static esp_err_t rtc_write_time(const struct tm *t)
{
    /* 把 struct tm 转换为 PCF85063 需要的 BCD 格式并连续写入时间寄存器。写 seconds
     * 寄存器时会顺带清除 OS 停振标志。
     */
    uint8_t buf[7];

    if(t == NULL || !rtc_tm_is_valid(t)) {
        return ESP_ERR_INVALID_ARG;
    }

    buf[0] = bin_to_bcd((uint8_t)t->tm_sec);          /* 写入 seconds 会清 OS 位。 */
    buf[1] = bin_to_bcd((uint8_t)t->tm_min);
    buf[2] = bin_to_bcd((uint8_t)t->tm_hour);         /* 24 小时制。 */
    buf[3] = bin_to_bcd((uint8_t)t->tm_mday);
    buf[4] = bin_to_bcd((uint8_t)t->tm_wday);
    buf[5] = bin_to_bcd((uint8_t)(t->tm_mon + 1));
    buf[6] = bin_to_bcd((uint8_t)((t->tm_year + 1900) % 100));

    return rtc_write_regs(PCF85063_REG_SECONDS, buf, sizeof(buf));
}

/**
 * @brief 用 RTC 时间更新 ESP32 系统时间。
 *
 * @param src 已校验的本地时间。
 * @return ESP_OK 表示系统时间设置成功。
 */
static esp_err_t system_set_time_from_tm(const struct tm *src)
{
    /* 把本地 struct tm 转成 time_t，再调用 settimeofday 更新 ESP32
     * 系统时间。更新后依赖系统 tick 继续走时。
     */
    struct tm t;

    if(src == NULL || !rtc_tm_is_valid(src)) {
        return ESP_ERR_INVALID_ARG;
    }

    watch_tz_init();

    t = *src;
    t.tm_isdst = -1;

    time_t ts = mktime(&t);
    if(ts == (time_t)-1) {
        ESP_LOGW(TAG, "mktime failed");
        return ESP_ERR_INVALID_RESPONSE;
    }

    struct timeval tv = {
        .tv_sec = ts,
        .tv_usec = 0,
    };

    if(settimeofday(&tv, NULL) != 0) {
        ESP_LOGW(TAG, "settimeofday failed");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG,
             "system time set: %04d/%02d/%02d %02d:%02d:%02d",
             t.tm_year + 1900,
             t.tm_mon + 1,
             t.tm_mday,
             t.tm_hour,
             t.tm_min,
             t.tm_sec);

    return ESP_OK;
}

/**
 * @brief 初始化 RTC 所在 I2C 总线并配置 PCF85063 低功耗选项。
 */
esp_err_t watch_rtc_init(void)
{
    /* 初始化 RTC 总线和芯片基础配置。多次调用会快速返回，避免重复安装 I2C driver。
     */
    if(s_rtc_i2c_ready) {
        return ESP_OK;
    }

    watch_tz_init();

    i2c_config_t conf = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = RTC_SDA_IO,
        .scl_io_num = RTC_SCL_IO,
        .sda_pullup_en = GPIO_PULLUP_ENABLE,
        .scl_pullup_en = GPIO_PULLUP_ENABLE,
        .master.clk_speed = RTC_I2C_FREQ_HZ,
        .clk_flags = 0,
    };

    esp_err_t ret = i2c_param_config(RTC_I2C_PORT, &conf);
    if(ret != ESP_OK) {
        ESP_LOGW(TAG, "i2c_param_config failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = i2c_driver_install(RTC_I2C_PORT, I2C_MODE_MASTER, 0, 0, 0);
    if(ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "i2c_driver_install failed: %s", esp_err_to_name(ret));
        return ret;
    }

    s_rtc_i2c_ready = true;

    /* 确保 RTC 没有被 STOP 位停止。 */
    uint8_t control1 = 0x00;
    ret = rtc_write_regs(PCF85063_REG_CONTROL_1, &control1, 1);
    if(ret != ESP_OK) {
        ESP_LOGW(TAG, "RTC control1 write failed: %s", esp_err_to_name(ret));
        return ret;
    }

    /* 关闭未使用的 CLKOUT，降低 RTC 电池电流。 */
    uint8_t control2 = PCF85063_CONTROL_2_CLKOUT_OFF;
    ret = rtc_write_regs(PCF85063_REG_CONTROL_2, &control2, 1);
    if(ret != ESP_OK) {
        ESP_LOGW(TAG, "RTC not found on I2C addr 0x%02X: %s", PCF85063_I2C_ADDR, esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGI(TAG, "PCF85063 init ok, SDA=GPIO%d SCL=GPIO%d", RTC_SDA_IO, RTC_SCL_IO);
    return ESP_OK;
}

/**
 * @brief 从 RTC 恢复系统时间；RTC 无效时写入默认时间。
 */
esp_err_t watch_rtc_restore_or_init_2026(void)
{
    /* 上电恢复入口：优先读取 RTC；若 RTC 无效则写入默认 2026 时间；最后把可用时间同步给系统。
     */
    struct tm t;
    bool oscillator_ok = false;

    esp_err_t ret = watch_rtc_init();
    if(ret != ESP_OK) {
        return ret;
    }

    ret = rtc_read_time(&t, &oscillator_ok);
    if(ret == ESP_OK && oscillator_ok) {
        ESP_LOGI(TAG,
                 "RTC time valid, restore system time: %04d/%02d/%02d %02d:%02d:%02d",
                 t.tm_year + 1900,
                 t.tm_mon + 1,
                 t.tm_mday,
                 t.tm_hour,
                 t.tm_min,
                 t.tm_sec);
        return system_set_time_from_tm(&t);
    }

    ESP_LOGW(TAG, "RTC time invalid or not running, initialize to 2026/01/01 00:00:00");

    rtc_default_tm_2026(&t);

    ret = rtc_write_time(&t);
    if(ret != ESP_OK) {
        ESP_LOGW(TAG, "initialize RTC failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGI(TAG, "RTC initialized to 2026/01/01 00:00:00");
    return system_set_time_from_tm(&t);
}

/**
 * @brief 将当前 RTC 时间同步到 ESP32 系统时间。
 */
esp_err_t watch_rtc_set_system_time_from_rtc(void)
{
    /* 显式从 RTC 拉取时间并同步到 ESP32 系统时间，适合唤醒后或启动阶段调用。
     */
    struct tm t;
    bool oscillator_ok = false;

    esp_err_t ret = watch_rtc_init();
    if(ret != ESP_OK) {
        return ret;
    }

    ret = rtc_read_time(&t, &oscillator_ok);
    if(ret != ESP_OK || !oscillator_ok) {
        ESP_LOGW(TAG, "skip setting system time from RTC");
        return ret;
    }

    return system_set_time_from_tm(&t);
}

/**
 * @brief 将 ESP32 当前系统时间写回 RTC。
 */
esp_err_t watch_rtc_update_from_system(void)
{
    /* 把当前 ESP32 系统时间写回 RTC，通常在 SNTP 校时成功后调用，用于离线时保持时间。
     */
    time_t now;
    struct tm t;

    esp_err_t ret = watch_rtc_init();
    if(ret != ESP_OK) {
        return ret;
    }

    watch_tz_init();

    time(&now);
    localtime_r(&now, &t);

    if(!rtc_tm_is_valid(&t)) {
        ESP_LOGW(TAG, "system time is invalid, skip writing RTC");
        return ESP_ERR_INVALID_STATE;
    }

    ret = rtc_write_time(&t);
    if(ret != ESP_OK) {
        ESP_LOGW(TAG, "write RTC failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGI(TAG,
             "RTC updated from system time: %04d/%02d/%02d %02d:%02d:%02d",
             t.tm_year + 1900,
             t.tm_mon + 1,
             t.tm_mday,
             t.tm_hour,
             t.tm_min,
             t.tm_sec);

    return ESP_OK;
}


/* 维护提示
 * 修改 RTC 寄存器读写时，请同时确认 BCD 掩码、OS 停振位和 struct tm 的月份偏移。
 */
