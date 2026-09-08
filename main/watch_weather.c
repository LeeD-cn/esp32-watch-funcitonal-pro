/**
 * @file watch_weather.c
 * @brief 天气页面、Open-Meteo 数据获取和天气图标绘制。
 */


/**
 * @section 模块说明
 * 本文件负责天气页面 UI 绘制和 Open-Meteo 数据拉取。
 * 网络请求在独立 FreeRTOS 任务中完成，LVGL 对象更新则通过页面定时器回到 UI 线程执行，
 * 这样可以避免在 HTTP 回调或后台任务里直接操作 LVGL 导致线程安全问题。
 *
 * 数据流：配置坐标 -> 拼接 Open-Meteo URL -> HTTP 获取 JSON -> 解析温度/WMO 天气码 ->
 * 映射到内部图标类型 -> 标记 data_dirty -> UI 定时器刷新页面。
 */

#include "watch_weather.h"
#include "watch_wifi.h"
#include "watch_config.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_err.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/* 以下宏大多是 UI 坐标、尺寸或任务参数。
 * 修改这类值时建议同时检查：
 * 1. 240x240 屏幕边界是否越界；
 * 2. 选择框/动画目标是否仍然对齐；
 * 3. FreeRTOS 任务栈是否足够容纳 JSON/HTTP/LVGL 临时对象。
 */
#define WATCH_SCREEN_W              240
#define WATCH_SCREEN_H              240

#define WEATHER_CARD_W              180
#define WEATHER_CARD_H              180
#define WEATHER_CARD_X              ((WATCH_SCREEN_W - WEATHER_CARD_W) / 2)
#define WEATHER_CARD_Y              ((WATCH_SCREEN_H - WEATHER_CARD_H) / 2)
#define WEATHER_CARD_RADIUS         18

#define WEATHER_BG_COLOR            0xA6D9F1
#define WEATHER_CARD_SHADOW_W       16
#define WEATHER_CARD_SHADOW_OFS     4

#define WEATHER_TEMP_Y              30
#define WEATHER_RANGE_Y             120
#define WEATHER_ICON_X              112
#define WEATHER_ICON_Y              114
#define WEATHER_ICON_W              46
#define WEATHER_ICON_H              46

#define WEATHER_URL_BUF_SIZE        256
#define WEATHER_HTTP_BUF_SIZE       4096
#define WEATHER_HTTP_TIMEOUT_MS     12000
#define WEATHER_UPDATE_PERIOD_MS    300
#define WEATHER_FETCH_STACK_SIZE    8192
#define WEATHER_FETCH_PRIO          4

LV_FONT_DECLARE(font_100);
LV_IMG_DECLARE(weather_bg);

/**
 * @brief 项目内部天气图标类型。
 *
 * Open-Meteo 的天气码种类很多，本页面只抽象成几类适合 240x240 小屏显示的图标。
 */
typedef enum {
    WEATHER_ICON_SUN = 0,
    WEATHER_ICON_CLOUD,
    WEATHER_ICON_RAIN,
    WEATHER_ICON_SNOW,
    WEATHER_ICON_THUNDER,
    WEATHER_ICON_FOG,
} weather_icon_type_t;

/**
 * @brief HTTP 响应累积缓冲区。
 *
 * esp_http_client 会分多次回调数据，本结构记录已写入长度和是否溢出。
 */
typedef struct {
    char *buf;
    int len;
    int max_len;
    bool overflow;
} weather_http_buf_t;

typedef struct {
    int current_temp_c;
    int min_temp_c;
    int max_temp_c;
    weather_icon_type_t icon_type;
} weather_data_t;

/**
 * @brief 天气页运行上下文。
 *
 * data_dirty 用于跨任务通知 UI 定时器刷新；fetching 防止重复创建网络任务；
 * fetch_seq 可用于区分不同请求批次，避免旧请求覆盖新请求。
 */
typedef struct {
    lv_obj_t *page;
    lv_obj_t *bg_img;
    lv_obj_t *card;
    lv_obj_t *temp_label;
    lv_obj_t *range_label;
    lv_obj_t *icon_box;
    lv_timer_t *update_timer;

    TaskHandle_t fetch_task;
    uint32_t fetch_seq;

    int current_temp_c;
    int min_temp_c;
    int max_temp_c;
    weather_icon_type_t icon_type;
    bool has_weather;
    bool data_dirty;
    bool fetching;
    bool wants_back;
} watch_weather_ctx_t;

static const char *TAG = "watch_weather";
static watch_weather_ctx_t s_weather;
static uint32_t s_weather_seq_gen;
static weather_data_t s_weather_cache;
static bool s_weather_cache_valid;

static bool weather_data_equals(const weather_data_t *left, const weather_data_t *right)
{
    /* 比较两份天气数据是否完全一致，用于避免重复刷新 UI。
     */
    if(left == NULL || right == NULL) {
        return false;
    }

    return left->current_temp_c == right->current_temp_c
           && left->min_temp_c == right->min_temp_c
           && left->max_temp_c == right->max_temp_c
           && left->icon_type == right->icon_type;
}

static void weather_ctx_set_data(const weather_data_t *data, bool mark_dirty)
{
    /* 把天气数据写入页面上下文，并根据需要标记 data_dirty，等待 UI 定时器渲染。
     */
    if(data == NULL) {
        return;
    }

    s_weather.current_temp_c = data->current_temp_c;
    s_weather.min_temp_c = data->min_temp_c;
    s_weather.max_temp_c = data->max_temp_c;
    s_weather.icon_type = data->icon_type;
    s_weather.has_weather = true;

    if(mark_dirty) {
        s_weather.data_dirty = true;
    }
}

static bool weather_ctx_equals_data(const weather_data_t *data)
{
    /* 比较当前页面显示数据与新数据是否一致。
     */
    weather_data_t current;

    if(data == NULL || !s_weather.has_weather) {
        return false;
    }

    current.current_temp_c = s_weather.current_temp_c;
    current.min_temp_c = s_weather.min_temp_c;
    current.max_temp_c = s_weather.max_temp_c;
    current.icon_type = s_weather.icon_type;

    return weather_data_equals(&current, data);
}

static bool weather_cache_save_success(const weather_data_t *data)
{
    /* 保存最近一次成功拉取的数据到 RAM 缓存，并返回是否发生变化。
     */
    bool changed;

    if(data == NULL) {
        return false;
    }

    changed = !s_weather_cache_valid || !weather_data_equals(&s_weather_cache, data);

    if(changed) {
        s_weather_cache = *data;
        s_weather_cache_valid = true;
    }

    return changed;
}

static bool weather_load_cache_to_ctx(bool mark_dirty)
{
    /* 把 RAM 缓存恢复到页面上下文，进入页面时可先显示旧数据，避免空白。
     */
    if(!s_weather_cache_valid) {
        return false;
    }

    weather_ctx_set_data(&s_weather_cache, mark_dirty);
    return true;
}

static void weather_icon_clear(void)
{
    /* 清空图标容器中的旧图形对象，为重绘天气图标做准备。
     */
    if(s_weather.icon_box == NULL) {
        return;
    }

    lv_obj_clean(s_weather.icon_box);
}

static lv_obj_t *weather_shape_create(lv_obj_t *parent,
                                      lv_coord_t x,
                                      lv_coord_t y,
                                      lv_coord_t w,
                                      lv_coord_t h,
                                      lv_color_t color,
                                      lv_coord_t radius)
{
    lv_obj_t *obj = lv_obj_create(parent);
    lv_obj_remove_style_all(obj);
    lv_obj_set_size(obj, w, h);
    lv_obj_set_pos(obj, x, y);
    lv_obj_set_style_bg_color(obj, color, 0);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(obj, radius, 0);
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_CLICKABLE);
    return obj;
}

static void weather_icon_draw_sun(lv_obj_t *parent)
{
    /* 绘制晴天图标，由中心圆和八个光芒块组成。
     */
    lv_color_t sun = lv_color_hex(0xFFC928);

    weather_shape_create(parent, 17, 15, 18, 18, sun, LV_RADIUS_CIRCLE);
    weather_shape_create(parent, 22,  4,  4,  8, sun, 2);
    weather_shape_create(parent, 22, 36,  4,  8, sun, 2);
    weather_shape_create(parent,  6, 22,  8,  4, sun, 2);
    weather_shape_create(parent, 38, 22,  8,  4, sun, 2);
    weather_shape_create(parent, 10,  9,  6,  4, sun, 2);
    weather_shape_create(parent, 36, 35,  6,  4, sun, 2);
    weather_shape_create(parent, 36,  9,  6,  4, sun, 2);
    weather_shape_create(parent, 10, 35,  6,  4, sun, 2);
}

static void weather_icon_draw_cloud_core(lv_obj_t *parent)
{
    /* 绘制云朵核心形状，其他阴雨雪雾图标会复用这部分。
     */
    lv_color_t cloud = lv_color_hex(0xDCEAF2);
    lv_color_t shade = lv_color_hex(0xB9D0DC);

    weather_shape_create(parent, 8, 22, 34, 16, shade, 8);
    weather_shape_create(parent, 10, 20, 34, 16, cloud, 8);
    weather_shape_create(parent, 13, 15, 15, 15, cloud, LV_RADIUS_CIRCLE);
    weather_shape_create(parent, 24, 10, 19, 19, cloud, LV_RADIUS_CIRCLE);
    weather_shape_create(parent, 5, 23, 16, 15, cloud, LV_RADIUS_CIRCLE);
}

static void weather_icon_draw_cloud(lv_obj_t *parent)
{
    /* 绘制多云图标。
     */
    weather_icon_draw_cloud_core(parent);
}

static void weather_icon_draw_rain(lv_obj_t *parent)
{
    /* 绘制雨天图标，在云朵下方增加雨滴。
     */
    weather_icon_draw_cloud_core(parent);
    lv_color_t rain = lv_color_hex(0x2E9BDB);

    weather_shape_create(parent, 13, 39, 3, 8, rain, 2);
    weather_shape_create(parent, 24, 39, 3, 8, rain, 2);
    weather_shape_create(parent, 35, 39, 3, 8, rain, 2);
}

static void weather_icon_draw_snow(lv_obj_t *parent)
{
    /* 绘制雪天图标，在云朵下方增加雪点。
     */
    weather_icon_draw_cloud_core(parent);
    lv_color_t snow = lv_color_hex(0x7BC6E8);

    weather_shape_create(parent, 13, 40, 5, 5, snow, LV_RADIUS_CIRCLE);
    weather_shape_create(parent, 25, 40, 5, 5, snow, LV_RADIUS_CIRCLE);
    weather_shape_create(parent, 37, 40, 5, 5, snow, LV_RADIUS_CIRCLE);
}

static void weather_icon_draw_thunder(lv_obj_t *parent)
{
    /* 绘制雷暴图标，在云朵下方增加闪电。
     */
    weather_icon_draw_cloud_core(parent);
    lv_color_t bolt = lv_color_hex(0xFFC928);

    weather_shape_create(parent, 23, 35, 10, 4, bolt, 1);
    weather_shape_create(parent, 28, 38,  5, 9, bolt, 1);
    weather_shape_create(parent, 18, 43, 12, 4, bolt, 1);
}

static void weather_icon_draw_fog(lv_obj_t *parent)
{
    /* 绘制雾天图标，在云朵下方增加雾线。
     */
    weather_icon_draw_cloud_core(parent);
    lv_color_t fog = lv_color_hex(0x9DB2BD);

    weather_shape_create(parent,  8, 39, 34, 3, fog, 2);
    weather_shape_create(parent, 13, 45, 28, 3, fog, 2);
}

static void weather_icon_update(weather_icon_type_t type)
{
    /* 根据内部图标枚举清空并重绘天气图标。
     */
    if(s_weather.icon_box == NULL) {
        return;
    }

    weather_icon_clear();

    switch(type) {
    case WEATHER_ICON_SUN:
        weather_icon_draw_sun(s_weather.icon_box);
        break;
    case WEATHER_ICON_CLOUD:
        weather_icon_draw_cloud(s_weather.icon_box);
        break;
    case WEATHER_ICON_RAIN:
        weather_icon_draw_rain(s_weather.icon_box);
        break;
    case WEATHER_ICON_SNOW:
        weather_icon_draw_snow(s_weather.icon_box);
        break;
    case WEATHER_ICON_THUNDER:
        weather_icon_draw_thunder(s_weather.icon_box);
        break;
    case WEATHER_ICON_FOG:
        weather_icon_draw_fog(s_weather.icon_box);
        break;
    default:
        weather_icon_draw_cloud(s_weather.icon_box);
        break;
    }
}

/**
 * @brief 将 Open-Meteo WMO 天气代码转换为页面图标类型。
 */
static weather_icon_type_t weather_icon_from_code(int code)
{
    /* 把 Open-Meteo/WMO 天气码归类成项目内的几种图标类型。
     */
    /* Open-Meteo 返回 WMO 天气代码，这里映射为页面图标类型。 */
    switch(code) {
    case 0:     
    case 1:     
        return WEATHER_ICON_SUN;

    case 2:     
    case 3:     
        return WEATHER_ICON_CLOUD;

    case 45:    
    case 48:    
        return WEATHER_ICON_FOG;

    case 51:    
    case 53:
    case 55:
    case 56:    
    case 57:
    case 61:    
    case 63:
    case 65:
    case 66:    
    case 67:
    case 80:    
    case 81:
    case 82:
        return WEATHER_ICON_RAIN;

    case 71:    
    case 73:
    case 75:
    case 77:    
    case 85:    
    case 86:
        return WEATHER_ICON_SNOW;

    case 95:    
    case 96:
    case 99:
        return WEATHER_ICON_THUNDER;

    default:
        return WEATHER_ICON_CLOUD;
    }
}

static esp_err_t weather_http_event_handler(esp_http_client_event_t *evt)
{
    /* HTTP 客户端事件回调，负责把响应体分段追加到缓冲区，并记录是否溢出。
     */
    weather_http_buf_t *http_buf = (weather_http_buf_t *)evt->user_data;

    if(evt->event_id == HTTP_EVENT_ON_DATA && http_buf != NULL && evt->data != NULL) {
        int copy_len = evt->data_len;
        if(http_buf->len + copy_len >= http_buf->max_len) {
            copy_len = http_buf->max_len - http_buf->len - 1;
            http_buf->overflow = true;
        }

        if(copy_len > 0) {
            memcpy(http_buf->buf + http_buf->len, evt->data, copy_len);
            http_buf->len += copy_len;
            http_buf->buf[http_buf->len] = '\0';
        }
    }

    return ESP_OK;
}

static int weather_round_temp(double value)
{
    /* 把浮点温度四舍五入为整数摄氏度，适合小屏显示。
     */
    if(value >= 0.0) {
        return (int)(value + 0.5);
    }

    return (int)(value - 0.5);
}

/**
 * @brief 解析 Open-Meteo JSON，提取当前温度、当天温度范围和天气代码。
 */
static bool weather_parse_open_meteo_json(const char *json,
                                          int *current_temp_c,
                                          int *min_temp_c,
                                          int *max_temp_c,
                                          weather_icon_type_t *icon_type)
{
    /* 解析 Open-Meteo JSON，提取当前温度、当天最高/最低温和天气码。
     */
    bool ok = false;
    cJSON *root = cJSON_Parse(json);
    if(root == NULL) {
        const char *err = cJSON_GetErrorPtr();
        ESP_LOGW(TAG, "weather json parse failed near: %.32s", err ? err : "null");
        return false;
    }

    cJSON *current = cJSON_GetObjectItem(root, "current");
    cJSON *daily = cJSON_GetObjectItem(root, "daily");

    if(cJSON_IsObject(current) && cJSON_IsObject(daily)) {
        cJSON *temp = cJSON_GetObjectItem(current, "temperature_2m");
        cJSON *weather_code = cJSON_GetObjectItem(current, "weather_code");

        cJSON *min_array = cJSON_GetObjectItem(daily, "temperature_2m_min");
        cJSON *max_array = cJSON_GetObjectItem(daily, "temperature_2m_max");
        cJSON *min0 = cJSON_GetArrayItem(min_array, 0);
        cJSON *max0 = cJSON_GetArrayItem(max_array, 0);

        if(cJSON_IsNumber(temp) && cJSON_IsNumber(min0) && cJSON_IsNumber(max0)) {
            *current_temp_c = weather_round_temp(temp->valuedouble);
            *min_temp_c = weather_round_temp(min0->valuedouble);
            *max_temp_c = weather_round_temp(max0->valuedouble);

            if(icon_type != NULL) {
                if(cJSON_IsNumber(weather_code)) {
                    *icon_type = weather_icon_from_code(weather_code->valueint);
                } else {
                    *icon_type = WEATHER_ICON_CLOUD;
                }
            }

            ok = true;
        }
    }

    if(!ok) {
        ESP_LOGW(TAG, "open meteo json missing fields: %.160s", json);
    }

    cJSON_Delete(root);
    return ok;
}


/**
 * @brief 从 Open-Meteo 获取一次天气数据。
 */
static bool weather_fetch_once(int *current_temp_c,
                               int *min_temp_c,
                               int *max_temp_c,
                               weather_icon_type_t *icon_type)
{
    /* 执行一次完整天气请求：读取坐标配置、拼 URL、HTTP GET、解析 JSON。
     */
    char *buf = calloc(1, WEATHER_HTTP_BUF_SIZE);
    if(buf == NULL) {
        return false;
    }

    weather_http_buf_t http_buf = {
        .buf = buf,
        .len = 0,
        .max_len = WEATHER_HTTP_BUF_SIZE,
    };

    watch_config_t cfg_watch;
    if(watch_config_load(&cfg_watch) != ESP_OK || !watch_config_has_weather(&cfg_watch)) {
        ESP_LOGW(TAG, "weather location not configured yet");
        free(buf);
        return false;
    }

    char weather_url[WEATHER_URL_BUF_SIZE];
    snprintf(weather_url, sizeof(weather_url),
             "http://api.open-meteo.com/v1/forecast?latitude=%s&longitude=%s&current=temperature_2m,weather_code&daily=temperature_2m_max,temperature_2m_min&forecast_days=1&timezone=auto",
             cfg_watch.latitude,
             cfg_watch.longitude);

    esp_http_client_config_t config = {
        .url = weather_url,
        .timeout_ms = WEATHER_HTTP_TIMEOUT_MS,
        .event_handler = weather_http_event_handler,
        .user_data = &http_buf,
        .buffer_size = 1024,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if(client == NULL) {
        ESP_LOGE(TAG, "esp_http_client_init failed");
        free(buf);
        return false;
    }

    esp_http_client_set_header(client, "User-Agent", "ESP32-Watch/1.0");
    esp_http_client_set_header(client, "Accept", "application/json");

    ESP_LOGI(TAG, "weather http start: %s", weather_url);
    esp_err_t ret = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    ESP_LOGI(TAG, "weather http done, ret=%s, status=%d, len=%d, overflow=%d",
             esp_err_to_name(ret), status, http_buf.len, http_buf.overflow ? 1 : 0);

    bool ok = false;
    if(ret == ESP_OK && status == 200 && http_buf.len > 0 && !http_buf.overflow) {
        if(buf[0] != '{') {
            ESP_LOGW(TAG, "weather response is not json: %.120s", buf);
        } else {
            ok = weather_parse_open_meteo_json(buf, current_temp_c, min_temp_c, max_temp_c, icon_type);
            if(ok) {
                ESP_LOGI(TAG, "weather parse ok: now=%d, min=%d, max=%d",
                         *current_temp_c, *min_temp_c, *max_temp_c);
            }
        }
    } else {
        ESP_LOGW(TAG, "weather http failed, ret=%s, status=%d, len=%d, overflow=%d",
                 esp_err_to_name(ret), status, http_buf.len, http_buf.overflow ? 1 : 0);
    }

    free(buf);
    return ok;
}

static void weather_render_current(void)
{
    /* 把上下文中的天气数据渲染到 LVGL 标签和图标对象。
     */
    if(s_weather.temp_label == NULL || s_weather.range_label == NULL) {
        return;
    }

    if(s_weather.has_weather) {
        lv_label_set_text_fmt(s_weather.temp_label, "%d°", s_weather.current_temp_c);
        lv_label_set_text_fmt(s_weather.range_label, "%d°~%d°", s_weather.min_temp_c, s_weather.max_temp_c);
        weather_icon_update(s_weather.icon_type);
    } else {
        lv_label_set_text(s_weather.temp_label, "--°");
        lv_label_set_text(s_weather.range_label, "--°~--°");
        weather_icon_update(WEATHER_ICON_CLOUD);
    }
}

static void weather_update_timer_cb(lv_timer_t *timer)
{
    /* 页面定时器回调，只在 data_dirty 时刷新 UI，避免后台任务直接操作 LVGL。
     */
    (void)timer;

    if(!s_weather.data_dirty) {
        return;
    }

    s_weather.data_dirty = false;
    weather_render_current();
}

static void weather_fetch_task(void *arg)
{
    /* 天气拉取后台任务，完成后把结果写入缓存和页面上下文。
     */
    uint32_t seq = (uint32_t)(uintptr_t)arg;
    weather_data_t fetched_data = {0};
    bool ok = false;

    fetched_data.icon_type = WEATHER_ICON_CLOUD;

    ESP_LOGI(TAG, "weather fetch task start");

    if(watch_wifi_wait_connected(pdMS_TO_TICKS(15000)) == ESP_OK) {
        ESP_LOGI(TAG, "wifi ready, fetching weather");
        ok = weather_fetch_once(&fetched_data.current_temp_c,
                                &fetched_data.min_temp_c,
                                &fetched_data.max_temp_c,
                                &fetched_data.icon_type);
    } else {
        ESP_LOGW(TAG, "wifi connect timeout");
    }

    ESP_LOGI(TAG, "weather fetch task done, ok=%d", ok ? 1 : 0);

    if(ok) {
        weather_cache_save_success(&fetched_data);
    }

    if(seq == s_weather.fetch_seq) {
        if(ok) {
            if(!weather_ctx_equals_data(&fetched_data)) {
                weather_ctx_set_data(&fetched_data, true);
            }
        } else if(!s_weather.has_weather) {
            weather_load_cache_to_ctx(true);
        }

        s_weather.fetching = false;
        s_weather.fetch_task = NULL;
    }

    vTaskDelete(NULL);
}

/**
 * @brief 启动异步天气刷新任务。
 */
static void weather_start_fetch(void)
{
    /* 启动一次天气拉取任务，并用 fetching 标志避免重复并发请求。
     */
    if(s_weather.fetching) {
        return;
    }

    s_weather.fetching = true;
    s_weather.fetch_seq = ++s_weather_seq_gen;

    ESP_LOGI(TAG, "create weather fetch task");

    BaseType_t ret = xTaskCreate(weather_fetch_task,
                                 "weather_fetch",
                                 WEATHER_FETCH_STACK_SIZE,
                                 (void *)(uintptr_t)s_weather.fetch_seq,
                                 WEATHER_FETCH_PRIO,
                                 &s_weather.fetch_task);
    if(ret != pdPASS) {
        s_weather.fetching = false;
        s_weather.fetch_task = NULL;
        ESP_LOGE(TAG, "create weather fetch task failed");
    }
}

/**
 * @brief 创建天气页面并触发首次刷新。
 */
lv_obj_t *watch_weather_create(lv_obj_t *parent)
{
    if(s_weather.page != NULL) {
        watch_weather_destroy();
    }

    memset(&s_weather, 0, sizeof(s_weather));

    s_weather.page = lv_obj_create(parent);
    lv_obj_remove_style_all(s_weather.page);
    lv_obj_set_size(s_weather.page, WATCH_SCREEN_W, WATCH_SCREEN_H);
    lv_obj_set_pos(s_weather.page, 0, 0);
    lv_obj_set_style_bg_opa(s_weather.page, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(s_weather.page, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(s_weather.page, LV_OBJ_FLAG_CLICKABLE);

    s_weather.bg_img = lv_img_create(s_weather.page);
    lv_img_set_src(s_weather.bg_img, &weather_bg);
    lv_obj_set_pos(s_weather.bg_img, 0, 0);
    lv_obj_set_size(s_weather.bg_img, WATCH_SCREEN_W, WATCH_SCREEN_H);
    lv_obj_clear_flag(s_weather.bg_img, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(s_weather.bg_img, LV_OBJ_FLAG_CLICKABLE);

    s_weather.card = lv_obj_create(s_weather.page);
    lv_obj_remove_style_all(s_weather.card);
    lv_obj_set_size(s_weather.card, WEATHER_CARD_W, WEATHER_CARD_H);
    lv_obj_set_pos(s_weather.card, WEATHER_CARD_X, WEATHER_CARD_Y);
    lv_obj_set_style_bg_color(s_weather.card, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(s_weather.card, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_weather.card, WEATHER_CARD_RADIUS, 0);
    lv_obj_set_style_shadow_width(s_weather.card, WEATHER_CARD_SHADOW_W, 0);
    lv_obj_set_style_shadow_color(s_weather.card, lv_color_hex(0x6BA9C8), 0);
    lv_obj_set_style_shadow_opa(s_weather.card, LV_OPA_40, 0);
    lv_obj_set_style_shadow_ofs_x(s_weather.card, 0, 0);
    lv_obj_set_style_shadow_ofs_y(s_weather.card, WEATHER_CARD_SHADOW_OFS, 0);
    lv_obj_clear_flag(s_weather.card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(s_weather.card, LV_OBJ_FLAG_CLICKABLE);

    s_weather.temp_label = lv_label_create(s_weather.card);
    lv_obj_set_style_text_font(s_weather.temp_label, &font_100, 0);
    lv_obj_set_style_text_color(s_weather.temp_label, lv_color_hex(0x028ad6), 0);
    lv_obj_set_style_text_align(s_weather.temp_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(s_weather.temp_label, "--°");
    lv_obj_set_width(s_weather.temp_label, WEATHER_CARD_W);
    lv_obj_set_pos(s_weather.temp_label, 0, WEATHER_TEMP_Y);
    lv_obj_clear_flag(s_weather.temp_label, LV_OBJ_FLAG_CLICKABLE);

    s_weather.range_label = lv_label_create(s_weather.card);
    lv_obj_set_style_text_font(s_weather.range_label, &lv_font_montserrat_26, 0);
    lv_obj_set_style_text_color(s_weather.range_label, lv_color_hex(0x2D3A40), 0);
    lv_label_set_text(s_weather.range_label, "--°~--°");
    lv_obj_set_pos(s_weather.range_label, 22, WEATHER_RANGE_Y);
    lv_obj_clear_flag(s_weather.range_label, LV_OBJ_FLAG_CLICKABLE);

    s_weather.icon_box = lv_obj_create(s_weather.card);
    lv_obj_remove_style_all(s_weather.icon_box);
    lv_obj_set_size(s_weather.icon_box, WEATHER_ICON_W, WEATHER_ICON_H);
    lv_obj_set_pos(s_weather.icon_box, WEATHER_ICON_X, WEATHER_ICON_Y);
    lv_obj_set_style_bg_opa(s_weather.icon_box, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(s_weather.icon_box, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(s_weather.icon_box, LV_OBJ_FLAG_CLICKABLE);
    weather_icon_update(WEATHER_ICON_CLOUD);

    weather_load_cache_to_ctx(false);
    weather_render_current();

    s_weather.update_timer = lv_timer_create(weather_update_timer_cb, WEATHER_UPDATE_PERIOD_MS, NULL);
    s_weather.data_dirty = false;

    weather_start_fetch();

    return s_weather.page;
}

/**
 * @brief 重置天气页面返回状态并请求刷新。
 */
void watch_weather_reset(void)
{
    /* 进入天气页时重置返回状态、加载缓存并触发一次网络刷新。
     */
    s_weather.wants_back = false;
    weather_load_cache_to_ctx(true);
    weather_start_fetch();
}

/**
 * @brief 处理天气页面按键事件。
 */
void watch_weather_on_key(watch_key_t key)
{
    /* 天气页按键处理，目前主要响应返回键。
     */
    if(s_weather.page == NULL) {
        return;
    }

    if(key == WATCH_KEY_2) {
        s_weather.wants_back = true;
    }
}

/**
 * @brief 查询天气页面是否请求返回。
 */
bool watch_weather_wants_back(void)
{
    /* 供 UI 调度器判断天气页是否请求返回菜单。
     */
    return s_weather.wants_back;
}

/**
 * @brief 销毁天气页面和刷新定时器。
 */
void watch_weather_destroy(void)
{
    /* 销毁天气页定时器并清空上下文，离开页面后停止 UI 刷新。
     */
    s_weather.fetch_seq = ++s_weather_seq_gen;

    if(s_weather.update_timer) {
        lv_timer_del(s_weather.update_timer);
        s_weather.update_timer = NULL;
    }

    if(s_weather.page) {
        lv_obj_del(s_weather.page);
        s_weather.page = NULL;
    }

    memset(&s_weather, 0, sizeof(s_weather));
}


/* 维护提示
 * 调整天气请求字段时，请同步修改 URL 参数、JSON 解析路径和缓存结构。
 */
