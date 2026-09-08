/**
 * @file time_sync.c
 * @brief WiFi/SNTP 校时与 RTC 回写实现。
 */
/*
 * ==================== 模块说明 ====================
 *  * 模块职责：
 * - 负责 WiFi 可用后的 SNTP 校时，并把成功获取到的系统时间回写到外部 RTC。
 * - 使用中国时区 POSIX TZ 字符串 CST-8，注意 POSIX 写法中负号代表 UTC+8。
 * - 任务启动后先等待一段时间，让 WiFi 和网络栈有机会完成初始化。
 * - 校时成功后进入低频重同步周期，失败则按重试间隔继续尝试。
 *
 * 阅读建议：
 * - 先看本文件顶部的宏定义和静态状态变量，理解硬件参数和运行状态。
 * - 再看 reset/init/start/on_key/destroy 等对外函数，理解页面或驱动的生命周期。
 * - 最后看 static 辅助函数，了解具体寄存器读写、UI 刷新或状态机细节。
 * =======================================================
 */


#include "time_sync.h"

#include <stdbool.h>
#include <stdlib.h>
#include <time.h>

#include "esp_err.h"
#include "esp_log.h"
#include "esp_netif_sntp.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "watch_config.h"
#include "watch_wifi.h"
#include "watch_rtc.h"

/* POSIX TZ：CST-8 表示中国时区 UTC+8。 */
#define WATCH_TZ                         "CST-8"

/* 校时任务会在 WiFi 可用后重试，并在成功后低频重新同步。 */
#define TIME_SYNC_TASK_STACK_SIZE        8192
#define TIME_SYNC_TASK_PRIO              4
#define TIME_SYNC_FIRST_DELAY_MS         3000
#define TIME_SYNC_WIFI_WAIT_MS           60000
#define TIME_SYNC_SNTP_WAIT_MS           30000
#define TIME_SYNC_RETRY_DELAY_MS         30000
#define TIME_SYNC_RESYNC_PERIOD_MS       (6 * 60 * 60 * 1000)

static const char *TAG = "watch_time_sync";
/* 校时任务句柄，用于保证任务只创建一次。 */
static TaskHandle_t s_time_sync_task_handle = NULL;
/* SNTP 客户端是否已经初始化。 */
static bool s_sntp_inited = false;
/* 系统启动后是否至少成功校时一次。 */
static bool s_time_synced_once = false;

/**
 * @brief 判断是否已配置 WiFi。
 *
 * 详细说明：
 * - 只有保存了 WiFi 参数才启动有效校时流程。
 */
static bool time_sync_wifi_configured(void)
{
    watch_config_t cfg;
    esp_err_t ret = watch_config_load(&cfg);
    if(ret != ESP_OK) {
        ESP_LOGW(TAG, "load watch config failed: %s", esp_err_to_name(ret));
        return false;
    }

    return watch_config_has_wifi(&cfg);
}

/**
 * @brief 设置系统时区。
 *
 * 详细说明：
 * - 使用 POSIX TZ 字符串，使 localtime() 得到北京时间。
 */
static void time_sync_set_timezone(void)
{
    setenv("TZ", WATCH_TZ, 1);
    tzset();
}

/**
 * @brief 初始化 SNTP 客户端。
 *
 * 详细说明：
 * - 已初始化时直接复用，避免重复创建 SNTP 实例。
 */
static esp_err_t time_sync_sntp_init_once(void)
{
    if(s_sntp_inited) {
        return ESP_OK;
    }

    /* 默认使用国内网络访问较稳定的 NTP 源。 */
    esp_sntp_config_t config = ESP_NETIF_SNTP_DEFAULT_CONFIG("ntp.aliyun.com");
    esp_err_t ret = esp_netif_sntp_init(&config);
    if(ret == ESP_ERR_INVALID_STATE) {
        /* SNTP 已初始化时直接复用当前实例。 */
        s_sntp_inited = true;
        return ESP_OK;
    }

    if(ret != ESP_OK) {
        ESP_LOGW(TAG, "SNTP init failed: %s", esp_err_to_name(ret));
        return ret;
    }

    s_sntp_inited = true;
    return ESP_OK;
}

/**
 * @brief 执行一次 SNTP 校时并写回 RTC。
 *
 * 详细说明：
 * - 等待网络时间同步完成。
 * - 成功后把系统时间写回 RTC，供离线启动使用。
 */
static bool sync_time_by_sntp_once(void)
{
    time_sync_set_timezone();

    esp_err_t ret = time_sync_sntp_init_once();
    if(ret != ESP_OK) {
        return false;
    }

    ret = esp_netif_sntp_sync_wait(pdMS_TO_TICKS(TIME_SYNC_SNTP_WAIT_MS));
    if(ret != ESP_OK) {
        ESP_LOGW(TAG, "SNTP sync timeout or failed: %s", esp_err_to_name(ret));
        return false;
    }

    time_t now = 0;
    struct tm t = {0};

    time(&now);
    localtime_r(&now, &t);

    /* 过滤明显无效的系统时间。 */
    if(t.tm_year + 1900 < 2024) {
        ESP_LOGW(TAG, "SNTP returned invalid year: %04d", t.tm_year + 1900);
        return false;
    }

    ESP_LOGI(TAG,
             "time synced: %04d/%02d/%02d %02d:%02d:%02d",
             t.tm_year + 1900,
             t.tm_mon + 1,
             t.tm_mday,
             t.tm_hour,
             t.tm_min,
             t.tm_sec);

    /* SNTP 成功后尝试写回外部 RTC。 */
    esp_err_t rtc_ret = watch_rtc_update_from_system();
    if(rtc_ret != ESP_OK) {
        ESP_LOGW(TAG, "update RTC failed after SNTP: %s", esp_err_to_name(rtc_ret));
    }

    s_time_synced_once = true;
    return true;
}

/**
 * @brief 后台校时任务。
 *
 * 详细说明：
 * - 等待 WiFi 可用后进行首次校时。
 * - 成功后按较长周期重新同步，失败则短间隔重试。
 *
 * @param arg 输入或输出参数，具体含义见函数内部使用方式。
 */
static void time_sync_task(void *arg)
{
    (void)arg;

    time_sync_set_timezone();

    /* 延后首次校时，避免与启动阶段网络任务抢资源。 */
    vTaskDelay(pdMS_TO_TICKS(TIME_SYNC_FIRST_DELAY_MS));

    while(1) {
        if(!time_sync_wifi_configured()) {
            ESP_LOGW(TAG, "WiFi not configured, retry time sync later");
            vTaskDelay(pdMS_TO_TICKS(TIME_SYNC_RETRY_DELAY_MS));
            continue;
        }

        esp_err_t wifi_ret = watch_wifi_wait_connected(pdMS_TO_TICKS(TIME_SYNC_WIFI_WAIT_MS));
        if(wifi_ret != ESP_OK) {
            ESP_LOGW(TAG, "WiFi not connected yet, retry time sync later: %s",
                     esp_err_to_name(wifi_ret));
            vTaskDelay(pdMS_TO_TICKS(TIME_SYNC_RETRY_DELAY_MS));
            continue;
        }

        if(sync_time_by_sntp_once()) {
            /* 成功同步后按较低频率周期校时。 */
            vTaskDelay(pdMS_TO_TICKS(TIME_SYNC_RESYNC_PERIOD_MS));
        } else {
            /* NTP 失败时按重试间隔继续同步。 */
            vTaskDelay(pdMS_TO_TICKS(TIME_SYNC_RETRY_DELAY_MS));
        }
    }
}

/**
 * @brief 启动后台校时任务。
 *
 * 详细说明：
 * - 确保任务只创建一次。
 */
void watch_time_sync_start(void)
{
    if(s_time_sync_task_handle != NULL) {
        return;
    }

    BaseType_t ret = xTaskCreate(time_sync_task,
                                 "time_sync",
                                 TIME_SYNC_TASK_STACK_SIZE,
                                 NULL,
                                 TIME_SYNC_TASK_PRIO,
                                 &s_time_sync_task_handle);
    if(ret != pdPASS) {
        s_time_sync_task_handle = NULL;
        ESP_LOGE(TAG, "create time_sync task failed");
    }
}

/**
 * @brief 查询本次运行是否已经完成过校时。
 *
 * 详细说明：
 * - 供 UI 或其他模块判断当前时间可信度。
 */
bool watch_time_sync_is_synced(void)
{
    return s_time_synced_once;
}
