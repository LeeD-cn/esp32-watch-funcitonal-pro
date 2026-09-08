/**
 * @file watch_bili_stats.c
 * @brief Bilibili 播放、点赞和粉丝数据获取实现。
 */
/*
 * ==================== 模块说明 ====================
 *  * 模块职责：
 * - 负责从 Bilibili 接口拉取播放、点赞和粉丝数据，并缓存到 NVS。
 * - HTTP 请求使用 esp_http_client 和系统证书包，接口返回 JSON 后通过 cJSON 解析。
 * - 后台任务会按固定周期刷新数据，并通过回调把变化通知到页面层。
 * - 配置依赖 watch_config 中保存的 UID 与 SESSDATA，未配置时不会发起有效统计请求。
 *
 * 阅读建议：
 * - 先看本文件顶部的宏定义和静态状态变量，理解硬件参数和运行状态。
 * - 再看 reset/init/start/on_key/destroy 等对外函数，理解页面或驱动的生命周期。
 * - 最后看 static 辅助函数，了解具体寄存器读写、UI 刷新或状态机细节。
 * =======================================================
 */


#include "watch_bili_stats.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "watch_wifi.h"
#include "watch_config.h"

#define BILI_HTTP_BUF_SIZE      4096
#define BILI_URL_BUF_SIZE       192
#define BILI_COOKIE_BUF_SIZE    1200
#define BILI_UPDATE_INTERVAL_MS (10 * 60 * 1000)

static const char *TAG = "watch_bili_stats";
/* Bilibili 统计数据缓存，页面优先显示这里的最近结果。 */
static watch_bili_stats_t s_cached_stats;
/* Bilibili 后台刷新任务是否已经启动。 */
static bool s_task_started;
/* 统计数据变化回调，通常由电子名片页面注册。 */
static watch_bili_stats_cb_t s_cb;
/* 传给统计回调的用户上下文指针。 */
static void *s_cb_user_data;

/**
 * @brief 从 NVS 读取最近一次 Bilibili 统计缓存。
 *
 * 详细说明：
 * - 即使网络暂不可用，页面仍可显示上次数据。
 */
static esp_err_t load_cached_stats(void)
{
    nvs_handle_t nvs;
    esp_err_t ret = nvs_open("bili_stats", NVS_READONLY, &nvs);
    if(ret != ESP_OK) {
        return ret;
    }

    uint64_t views = 0;
    uint64_t likes = 0;
    uint64_t subscribers = 0;

    ret = nvs_get_u64(nvs, "views", &views);
    ret |= nvs_get_u64(nvs, "likes", &likes);
    ret |= nvs_get_u64(nvs, "subs", &subscribers);
    nvs_close(nvs);

    if(ret == ESP_OK) {
        s_cached_stats.views = views;
        s_cached_stats.likes = likes;
        s_cached_stats.subscribers = subscribers;
        s_cached_stats.valid = true;
    }

    return ret;
}

/**
 * @brief 将 Bilibili 统计数据保存到 NVS。
 *
 * 详细说明：
 * - 刷新成功后保存，供下次启动快速展示。
 *
 * @param stats 输入或输出参数，具体含义见函数内部使用方式。
 */
static void save_cached_stats(const watch_bili_stats_t *stats)
{
    nvs_handle_t nvs;
    if(nvs_open("bili_stats", NVS_READWRITE, &nvs) != ESP_OK) {
        return;
    }

    nvs_set_u64(nvs, "views", stats->views);
    nvs_set_u64(nvs, "likes", stats->likes);
    nvs_set_u64(nvs, "subs", stats->subscribers);
    nvs_commit(nvs);
    nvs_close(nvs);
}

/**
 * @brief 发起 HTTP GET 请求并读取 JSON 响应。
 *
 * 详细说明：
 * - 设置 Cookie/UA 等请求信息。
 * - 把响应体读入调用者提供的缓冲区。
 *
 * @param url 输入或输出参数，具体含义见函数内部使用方式。
 * @param bili_uid 输入或输出参数，具体含义见函数内部使用方式。
 * @param sessdata 输入或输出参数，具体含义见函数内部使用方式。
 * @param buf 输入或输出参数，具体含义见函数内部使用方式。
 * @param buf_size 输入或输出参数，具体含义见函数内部使用方式。
 */
static esp_err_t http_get_json(const char *url,
                               const char *bili_uid,
                               const char *sessdata,
                               char *buf,
                               size_t buf_size)
{
    esp_http_client_config_t config = {
        .url = url,
        .method = HTTP_METHOD_GET,
        .timeout_ms = 10000,
        .buffer_size = 1024,
        .buffer_size_tx = 512,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if(client == NULL) {
        return ESP_ERR_NO_MEM;
    }

    esp_http_client_set_header(client, "User-Agent", "Mozilla/5.0");

    char referer[96];
    snprintf(referer, sizeof(referer), "https://space.bilibili.com/%s", bili_uid ? bili_uid : "");
    esp_http_client_set_header(client, "Referer", referer);
    esp_http_client_set_header(client, "Accept", "application/json, text/plain, */*");
    esp_http_client_set_header(client, "Accept-Encoding", "identity");

    ESP_LOGI(TAG, "SESSDATA len=%d", sessdata ? (int)strlen(sessdata) : 0);

    if(sessdata != NULL && strlen(sessdata) > 0) {
        char cookie[BILI_COOKIE_BUF_SIZE];
        snprintf(cookie, sizeof(cookie), "SESSDATA=%s", sessdata);
        esp_http_client_set_header(client, "Cookie", cookie);
    }

    esp_err_t ret = esp_http_client_open(client, 0);
    if(ret != ESP_OK) {
        ESP_LOGW(TAG, "HTTP open failed: %s url=%s", esp_err_to_name(ret), url);
        esp_http_client_cleanup(client);
        return ret;
    }

    int64_t content_length = esp_http_client_fetch_headers(client);
    if(content_length < 0) {
        ESP_LOGW(TAG, "HTTP fetch headers failed url=%s", url);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return ESP_FAIL;
    }

    int status = esp_http_client_get_status_code(client);

    int total = 0;
    while(total < (int)buf_size - 1) {
        int read_len = esp_http_client_read(client,
                                            buf + total,
                                            (int)buf_size - 1 - total);
        if(read_len < 0) {
            ESP_LOGW(TAG, "HTTP read failed url=%s", url);
            esp_http_client_close(client);
            esp_http_client_cleanup(client);
            return ESP_FAIL;
        }

        if(read_len == 0) {
            break;
        }

        total += read_len;
    }

    buf[total] = '\0';

    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    if(status != 200 || total <= 0) {
        ESP_LOGW(TAG, "HTTP GET failed status=%d len=%d url=%s", status, total, url);
        return ESP_FAIL;
    }

    return ESP_OK;
}

/**
 * @brief 解析播放量和点赞量接口响应。
 *
 * 详细说明：
 * - 从接口返回结构中提取 views 和 likes。
 *
 * @param json 输入或输出参数，具体含义见函数内部使用方式。
 * @param views 输入或输出参数，具体含义见函数内部使用方式。
 * @param likes 输入或输出参数，具体含义见函数内部使用方式。
 */
static esp_err_t parse_upstat(const char *json, uint64_t *views, uint64_t *likes)
{
    cJSON *root = cJSON_Parse(json);
    if(root == NULL) {
        return ESP_FAIL;
    }

    esp_err_t ret = ESP_FAIL;
    cJSON *data = cJSON_GetObjectItem(root, "data");
    cJSON *archive = cJSON_GetObjectItem(data, "archive");
    cJSON *view = cJSON_GetObjectItem(archive, "view");
    cJSON *likes_obj = cJSON_GetObjectItem(data, "likes");

    if(cJSON_IsNumber(view) && cJSON_IsNumber(likes_obj)) {
        *views = (uint64_t)view->valuedouble;
        *likes = (uint64_t)likes_obj->valuedouble;
        ret = ESP_OK;
    }

    cJSON_Delete(root);
    return ret;
}

/**
 * @brief 解析粉丝数接口响应。
 *
 * 详细说明：
 * - 从 relation/stat 接口中提取 subscriber 数。
 *
 * @param json 输入或输出参数，具体含义见函数内部使用方式。
 * @param subscribers 输入或输出参数，具体含义见函数内部使用方式。
 */
static esp_err_t parse_relation(const char *json, uint64_t *subscribers)
{
    cJSON *root = cJSON_Parse(json);
    if(root == NULL) {
        return ESP_FAIL;
    }

    esp_err_t ret = ESP_FAIL;
    cJSON *data = cJSON_GetObjectItem(root, "data");
    cJSON *follower = cJSON_GetObjectItem(data, "follower");

    if(cJSON_IsNumber(follower)) {
        *subscribers = (uint64_t)follower->valuedouble;
        ret = ESP_OK;
    }

    cJSON_Delete(root);
    return ret;
}

/**
 * @brief 拉取并合并 Bilibili 统计数据。
 *
 * 详细说明：
 * - 组合两个接口结果，得到播放、点赞和粉丝数。
 *
 * @param stats 输入或输出参数，具体含义见函数内部使用方式。
 */
static esp_err_t fetch_bili_stats(watch_bili_stats_t *stats)
{
    watch_config_t cfg;
    esp_err_t ret = watch_config_load(&cfg);
    if(ret != ESP_OK) {
        return ret;
    }

    if(!watch_config_has_bili(&cfg)) {
        ESP_LOGW(TAG, "Bilibili UID not configured yet");
        return ESP_ERR_INVALID_STATE;
    }

    char upstat_url[BILI_URL_BUF_SIZE];
    char relation_url[BILI_URL_BUF_SIZE];
    snprintf(upstat_url, sizeof(upstat_url),
             "https://api.bilibili.com/x/space/upstat?mid=%s&jsonp=jsonp",
             cfg.bili_uid);
    snprintf(relation_url, sizeof(relation_url),
             "https://api.bilibili.com/x/relation/stat?vmid=%s&jsonp=jsonp",
             cfg.bili_uid);

    char *json = calloc(1, BILI_HTTP_BUF_SIZE);
    if(json == NULL) {
        return ESP_ERR_NO_MEM;
    }

    watch_bili_stats_t next = {0};
    ret = ESP_FAIL;

    for(int i = 0; i < 3; i++) {
        memset(json, 0, BILI_HTTP_BUF_SIZE);

        ret = http_get_json(upstat_url, cfg.bili_uid, cfg.sessdata, json, BILI_HTTP_BUF_SIZE);
        if(ret == ESP_OK) {
            ESP_LOGI(TAG, "upstat json: %.512s", json);
            ret = parse_upstat(json, &next.views, &next.likes);
            if(ret == ESP_OK) {
                break;
            }
        }

        ESP_LOGW(TAG, "fetch upstat retry %d/3", i + 1);
        vTaskDelay(pdMS_TO_TICKS(2000));
    }

    if(ret != ESP_OK) {
        ESP_LOGW(TAG, "fetch upstat failed; if data is empty, configure SESSDATA from upper-computer tool");
        free(json);
        return ret;
    }

    memset(json, 0, BILI_HTTP_BUF_SIZE);
    ret = http_get_json(relation_url, cfg.bili_uid, cfg.sessdata, json, BILI_HTTP_BUF_SIZE);
    if(ret == ESP_OK) {
        ret = parse_relation(json, &next.subscribers);
    }

    free(json);

    if(ret == ESP_OK) {
        next.valid = true;
        *stats = next;
    }

    return ret;
}

/**
 * @brief 通知 UI 或调用者统计数据已更新。
 *
 * 详细说明：
 * - 通过回调把缓存数据传给 UI 层。
 *
 * @param stats 输入或输出参数，具体含义见函数内部使用方式。
 */
static void notify_stats_changed(const watch_bili_stats_t *stats)
{
    if(s_cb != NULL) {
        s_cb(stats, s_cb_user_data);
    }
}

/**
 * @brief Bilibili 数据周期刷新任务。
 *
 * 详细说明：
 * - 循环等待 WiFi、读取配置、请求接口并更新缓存。
 *
 * @param arg 输入或输出参数，具体含义见函数内部使用方式。
 */
static void bili_stats_task(void *arg)
{
    (void)arg;

    load_cached_stats();
    if(s_cached_stats.valid) {
        notify_stats_changed(&s_cached_stats);
    }

    while(1) {
        if(watch_wifi_wait_connected(portMAX_DELAY) == ESP_OK) {
            watch_bili_stats_t latest;
            if(fetch_bili_stats(&latest) == ESP_OK) {
                s_cached_stats = latest;
                save_cached_stats(&s_cached_stats);
                notify_stats_changed(&s_cached_stats);
            }
        }

        vTaskDelay(pdMS_TO_TICKS(BILI_UPDATE_INTERVAL_MS));
    }
}

/**
 * @brief 启动 Bilibili 数据刷新任务。
 *
 * 详细说明：
 * - 注册回调并确保任务只启动一次。
 *
 * @param cb 输入或输出参数，具体含义见函数内部使用方式。
 * @param user_data 输入或输出参数，具体含义见函数内部使用方式。
 */
void watch_bili_stats_start(watch_bili_stats_cb_t cb, void *user_data)
{
    s_cb = cb;
    s_cb_user_data = user_data;

    if(s_cached_stats.valid) {
        notify_stats_changed(&s_cached_stats);
    }

    if(!s_task_started) {
        s_task_started = true;
        xTaskCreate(bili_stats_task, "bili_stats", 8192, NULL, 4, NULL);
    }
}

/**
 * @brief 获取缓存的 Bilibili 统计数据。
 *
 * 详细说明：
 * - 用于页面创建时立即显示已有数据。
 *
 * @param stats 输入或输出参数，具体含义见函数内部使用方式。
 */
esp_err_t watch_bili_stats_get_cached(watch_bili_stats_t *stats)
{
    if(stats == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if(!s_cached_stats.valid) {
        load_cached_stats();
    }

    if(!s_cached_stats.valid) {
        return ESP_ERR_NOT_FOUND;
    }

    *stats = s_cached_stats;
    return ESP_OK;
}

/**
 * @brief 将数值压缩为短格式字符串。
 *
 * 详细说明：
 * - 例如把较大的播放量压缩成适合小屏显示的形式。
 *
 * @param value 输入或输出参数，具体含义见函数内部使用方式。
 * @param buf 输入或输出参数，具体含义见函数内部使用方式。
 * @param buf_size 输入或输出参数，具体含义见函数内部使用方式。
 */
void watch_bili_stats_format_short(uint64_t value, char *buf, size_t buf_size)
{
    if(buf == NULL || buf_size == 0) {
        return;
    }

    if(value >= 10000ULL) {
        unsigned long whole_tenth = (unsigned long)((value + 500ULL) / 1000ULL); /* 四舍五入到 0.1w */
        snprintf(buf, buf_size, "%lu.%luw", whole_tenth / 10UL, whole_tenth % 10UL);
    } else {
        snprintf(buf, buf_size, "%llu", (unsigned long long)value);
    }
}
