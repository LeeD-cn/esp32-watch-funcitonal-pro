/**
 * @file watch_wifi.c
 * @brief Wi-Fi STA 初始化、连接状态维护和自动重连。
 */


/**
 * @section 模块说明
 * 本文件封装 Wi-Fi STA 模式的初始化、启动、停止、连接状态查询和自动重连。
 * 事件回调只做轻量工作：更新 EventGroup 状态位并通知重连任务，真正的 esp_wifi_connect()
 * 放在独立任务中执行，避免在事件回调上下文中做耗时操作。
 *
 * 关键状态：
 * - s_inited：Wi-Fi 驱动和事件处理是否初始化完成；
 * - s_started：当前是否允许连接/重连；
 * - s_retry_num：本轮断线后的重试次数；
 * - WATCH_WIFI_CONNECTED_BIT / WATCH_WIFI_FAIL_BIT：供其他模块等待联网结果。
 */

#include "watch_wifi.h"

#include <string.h>

#include "esp_check.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_netif_ip_addr.h"
#include "esp_wifi.h"
#include "nvs_flash.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"

#include "watch_config.h"

/* Wi-Fi 参数由上位机写入 NVS，运行时从 watch_config 读取。 */
#define WIFI_MAX_RETRY              10
#define WIFI_RECONNECT_TASK_STACK   4096
#define WIFI_RECONNECT_TASK_PRIO    3

static const char *TAG = "watch_wifi";

/* Wi-Fi 状态事件组：连接成功、失败等状态通过 bit 暴露给其他模块。 */
static EventGroupHandle_t s_wifi_event_group;
static esp_netif_t *s_wifi_netif;
static bool s_inited;
/* started 表示当前允许 Wi-Fi 连接/重连；stop 后事件回调不会再自动重连。 */
static bool s_started;
/* 当前断线后的重试次数，超过 WIFI_MAX_RETRY 后置失败位。 */
static int s_retry_num;
static TaskHandle_t s_reconnect_task;

/**
 * @brief 设置备用 DNS，提升国内网络下 HTTP 解析成功率。
 */
static void watch_wifi_set_dns_fallback(void)
{
    /* 设置备用 DNS。国内网络下 223.5.5.5 通常解析稳定，可提高天气/B站等 HTTP 请求成功率。
     */
    if(s_wifi_netif == NULL) {
        return;
    }

    esp_netif_dns_info_t dns = {0};
    dns.ip.type = ESP_IPADDR_TYPE_V4;
    dns.ip.u_addr.ip4.addr = ESP_IP4TOADDR(223, 5, 5, 5);  /* 阿里 DNS */

    esp_err_t ret = esp_netif_set_dns_info(s_wifi_netif,
                                           ESP_NETIF_DNS_FALLBACK,
                                           &dns);
    if(ret == ESP_OK) {
        ESP_LOGI(TAG, "DNS fallback set to 223.5.5.5");
    } else {
        ESP_LOGW(TAG, "set DNS fallback failed: %s", esp_err_to_name(ret));
    }
}

static void watch_wifi_request_connect(void)
{
    /* 通过任务通知唤醒重连任务，让连接动作在普通任务上下文中执行。
     */
    if(s_reconnect_task != NULL) {
        xTaskNotifyGive(s_reconnect_task);
    }
}

/**
 * @brief 独立任务中执行 Wi-Fi 连接和重连。
 */
static void wifi_reconnect_task(void *arg)
{
    /* Wi-Fi 连接/重连任务。收到通知后检查状态并调用 esp_wifi_connect。
     */
    (void)arg;

    while(1) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

        if(!s_started) {
            continue;
        }

        if(watch_wifi_is_connected()) {
            continue;
        }

        if(s_retry_num > 0) {
            ESP_LOGI(TAG, "retry WiFi, count=%d", s_retry_num);
            vTaskDelay(pdMS_TO_TICKS(200));
        } else {
            ESP_LOGI(TAG, "connect WiFi");
        }

        esp_err_t ret = esp_wifi_connect();
        if(ret != ESP_OK && ret != ESP_ERR_WIFI_CONN) {
            ESP_LOGW(TAG, "esp_wifi_connect failed: %s", esp_err_to_name(ret));
        }
    }
}

/**
 * @brief 处理 Wi-Fi 和 IP 事件，维护连接状态位。
 */
static void wifi_event_handler(void *arg,
                               esp_event_base_t event_base,
                               int32_t event_id,
                               void *event_data)
{
    /* Wi-Fi/IP 事件处理函数，维护连接成功/失败事件位，并在断线时触发重连。
     */
    (void)arg;
    (void)event_data;

    /* 事件回调保持轻量，只更新事件位并通知重连任务。 */
    if(event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        if(s_started) {
            watch_wifi_request_connect();
        }
    } else if(event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        if(s_wifi_event_group != NULL) {
            xEventGroupClearBits(s_wifi_event_group, WATCH_WIFI_CONNECTED_BIT);
        }

        if(!s_started) {
            if(s_wifi_event_group != NULL) {
                xEventGroupSetBits(s_wifi_event_group, WATCH_WIFI_FAIL_BIT);
            }
            return;
        }

        if(s_retry_num < WIFI_MAX_RETRY) {
            s_retry_num++;
            watch_wifi_request_connect();
        } else {
            if(s_wifi_event_group != NULL) {
                xEventGroupSetBits(s_wifi_event_group, WATCH_WIFI_FAIL_BIT);
            }
        }
    } else if(event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        s_retry_num = 0;

        if(s_wifi_event_group != NULL) {
            xEventGroupClearBits(s_wifi_event_group, WATCH_WIFI_FAIL_BIT);
            xEventGroupSetBits(s_wifi_event_group, WATCH_WIFI_CONNECTED_BIT);
        }
    }
}

/**
 * @brief 初始化 Wi-Fi STA、事件循环和重连任务。
 */
esp_err_t watch_wifi_init(void)
{
    /* 初始化 NVS、网络栈、事件循环、STA netif、Wi-Fi 驱动、事件回调和重连任务。
     */
    if(s_inited) {
        return ESP_OK;
    }

    esp_err_t ret = nvs_flash_init();
    if(ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_RETURN_ON_ERROR(ret, TAG, "nvs_flash_init failed");

    watch_config_t cfg_watch;
    ESP_RETURN_ON_ERROR(watch_config_load(&cfg_watch), TAG, "watch_config_load failed");

    if(!watch_config_has_wifi(&cfg_watch)) {
        ESP_LOGW(TAG, "WiFi not configured yet; use upper-computer serial tool to write NVS config");
        return ESP_ERR_INVALID_STATE;
    }

    s_wifi_event_group = xEventGroupCreate();
    if(s_wifi_event_group == NULL) {
        return ESP_ERR_NO_MEM;
    }

    ret = esp_netif_init();
    if(ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_RETURN_ON_ERROR(ret, TAG, "esp_netif_init failed");
    }

    ret = esp_event_loop_create_default();
    if(ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_RETURN_ON_ERROR(ret, TAG, "esp_event_loop_create_default failed");
    }

    s_wifi_netif = esp_netif_create_default_wifi_sta();
    if(s_wifi_netif == NULL) {
        ESP_LOGE(TAG, "esp_netif_create_default_wifi_sta failed");
        return ESP_ERR_NO_MEM;
    }

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_wifi_init(&cfg), TAG, "esp_wifi_init failed");

    ESP_RETURN_ON_ERROR(esp_event_handler_instance_register(WIFI_EVENT,
                                                            ESP_EVENT_ANY_ID,
                                                            wifi_event_handler,
                                                            NULL,
                                                            NULL),
                        TAG,
                        "register WIFI_EVENT failed");

    ESP_RETURN_ON_ERROR(esp_event_handler_instance_register(IP_EVENT,
                                                            IP_EVENT_STA_GOT_IP,
                                                            wifi_event_handler,
                                                            NULL,
                                                            NULL),
                        TAG,
                        "register IP_EVENT failed");

    if(s_reconnect_task == NULL) {
        BaseType_t task_ret = xTaskCreate(wifi_reconnect_task,
                                          "wifi_reconn",
                                          WIFI_RECONNECT_TASK_STACK,
                                          NULL,
                                          WIFI_RECONNECT_TASK_PRIO,
                                          &s_reconnect_task);
        if(task_ret != pdPASS) {
            s_reconnect_task = NULL;
            return ESP_ERR_NO_MEM;
        }
    }

    wifi_config_t wifi_config = {0};
    strncpy((char *)wifi_config.sta.ssid, cfg_watch.wifi_ssid, sizeof(wifi_config.sta.ssid) - 1);
    strncpy((char *)wifi_config.sta.password, cfg_watch.wifi_pass, sizeof(wifi_config.sta.password) - 1);

    ESP_LOGI(TAG, "WiFi config loaded from NVS, ssid=%s, pass_len=%d",
             cfg_watch.wifi_ssid, (int)strlen(cfg_watch.wifi_pass));

    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), TAG, "esp_wifi_set_mode failed");
    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_STA, &wifi_config), TAG, "esp_wifi_set_config failed");

    s_inited = true;
    return ESP_OK;
}

/**
 * @brief 启动 Wi-Fi 并请求连接。
 */
esp_err_t watch_wifi_start(void)
{
    /* 根据 NVS 配置设置 STA 参数并启动 Wi-Fi。启动后由事件和重连任务负责实际连接。
     */
    ESP_RETURN_ON_ERROR(watch_wifi_init(), TAG, "watch_wifi_init failed");

    if(s_wifi_event_group != NULL) {
        xEventGroupClearBits(s_wifi_event_group, WATCH_WIFI_FAIL_BIT);
    }

    if(s_started) {
        if(!watch_wifi_is_connected()) {
            watch_wifi_request_connect();
        }
        return ESP_OK;
    }

    s_started = true;
    s_retry_num = 0;

    esp_err_t ret = esp_wifi_start();
    if(ret == ESP_OK) {
        esp_err_t ps_ret = esp_wifi_set_ps(WIFI_PS_NONE);
        if(ps_ret == ESP_OK) {
            ESP_LOGI(TAG, "WiFi power save disabled");
        } else {
            ESP_LOGW(TAG, "disable WiFi power save failed: %s", esp_err_to_name(ps_ret));
        }

        /* 主动补发一次连接请求，覆盖 STA_START 早到的情况。 */
        watch_wifi_request_connect();
    }

    return ret;
}

/**
 * @brief 停止 Wi-Fi，通常用于休眠前降低功耗。
 */
esp_err_t watch_wifi_stop(void)
{
    /* 停止 Wi-Fi 并清理 started/重试状态，同时清除连接状态位。
     */
    if(!s_inited) {
        return ESP_OK;
    }

    if(!s_started) {
        if(s_wifi_event_group != NULL) {
            xEventGroupClearBits(s_wifi_event_group, WATCH_WIFI_CONNECTED_BIT);
            xEventGroupSetBits(s_wifi_event_group, WATCH_WIFI_FAIL_BIT);
        }
        return ESP_OK;
    }

    /* 先关闭 started 状态，避免 stop 过程中的断开事件触发重连。 */
    s_started = false;
    s_retry_num = WIFI_MAX_RETRY;

    if(s_wifi_event_group != NULL) {
        xEventGroupClearBits(s_wifi_event_group, WATCH_WIFI_CONNECTED_BIT);
        xEventGroupSetBits(s_wifi_event_group, WATCH_WIFI_FAIL_BIT);
    }

    esp_err_t ret = esp_wifi_disconnect();
    if(ret != ESP_OK && ret != ESP_ERR_WIFI_NOT_STARTED && ret != ESP_ERR_WIFI_NOT_CONNECT) {
        ESP_LOGW(TAG, "esp_wifi_disconnect failed: %s", esp_err_to_name(ret));
    }

    ret = esp_wifi_stop();
    if(ret == ESP_OK || ret == ESP_ERR_WIFI_NOT_STARTED) {
        ESP_LOGI(TAG, "WiFi stopped for sleep");
        return ESP_OK;
    }

    ESP_LOGW(TAG, "esp_wifi_stop failed: %s", esp_err_to_name(ret));
    return ret;
}

/**
 * @brief 查询当前 Wi-Fi 是否已连接。
 */
bool watch_wifi_is_connected(void)
{
    /* 读取 EventGroup 判断当前是否已获得 IP。
     */
    if(s_wifi_event_group == NULL) {
        return false;
    }

    EventBits_t bits = xEventGroupGetBits(s_wifi_event_group);
    return (bits & WATCH_WIFI_CONNECTED_BIT) != 0;
}

/**
 * @brief 等待 Wi-Fi 连接成功或超时。
 */
esp_err_t watch_wifi_wait_connected(TickType_t timeout_ticks)
{
    /* 阻塞等待连接成功或失败，适合 SNTP、天气等需要网络前置条件的模块。
     */
    ESP_RETURN_ON_ERROR(watch_wifi_start(), TAG, "watch_wifi_start failed");

    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
                                           WATCH_WIFI_CONNECTED_BIT | WATCH_WIFI_FAIL_BIT,
                                           pdFALSE,
                                           pdFALSE,
                                           timeout_ticks);

    if(bits & WATCH_WIFI_CONNECTED_BIT) {
        watch_wifi_set_dns_fallback();
        ESP_LOGI(TAG, "WiFi connected");
        return ESP_OK;
    }

    return ESP_ERR_TIMEOUT;
}


/* 维护提示
 * 修改重连策略时，请避免在事件回调里直接做耗时连接或等待操作。
 */
