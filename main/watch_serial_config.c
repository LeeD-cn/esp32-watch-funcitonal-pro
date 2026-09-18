/**
 * @file watch_serial_config.c
 * @brief 通过串口/USB Serial-JTAG 接收上位机配置和图片数据。
 */


/**
 * @section 模块说明
 * 本文件提供上位机配置入口，数据通道优先使用 ESP32-S3 USB Serial/JTAG，失败时回退 stdio。
 * 支持的典型命令包括：
 * - set_config/get_config：写入或读取 Wi-Fi、Bilibili、天气坐标等配置；
 * - img_begin/img_chunk/img_end：分片传输二维码或封面 RGB565 图片；
 * - 图片大数据优先走 watch_config_image_stream_* 流式写 Flash，避免一次性申请大块 RAM。
 *
 * 串口协议的关键约束：每行必须是一条完整 JSON；图片分片必须按 offset 顺序写入；
 * 任一阶段失败都会通过 JSON 回复错误并清理接收上下文。
 */

#include "watch_serial_config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mbedtls/base64.h"
#include "sdkconfig.h"

#if CONFIG_IDF_TARGET_ESP32S3
#include "driver/usb_serial_jtag.h"
#endif

#include "watch_config.h"
#include "watch_device_info.h"
#include "watch_imu_capture.h"
#include "watch_steps.h"

/* 大图采用 watch_config.c 的流式写入接口，避免占用大块连续 RAM。 */
extern esp_err_t watch_config_image_stream_begin(const char *name, int w, int h, size_t size);
extern esp_err_t watch_config_image_stream_write(const char *name, size_t offset, const uint8_t *data, size_t len);
extern esp_err_t watch_config_image_stream_end(const char *name);
extern void watch_config_image_stream_cancel(const char *name);

#define SERIAL_CONFIG_TASK_STACK     8192
#define SERIAL_CONFIG_TASK_PRIO      4
#define SERIAL_CONFIG_LINE_MAX       2048
#define SERIAL_CONFIG_USB_RX_BUF     4096
#define SERIAL_CONFIG_USB_TX_BUF     4096

static const char *TAG = "serial_config";
static bool s_task_started;
static bool s_serial_io_inited;
static bool s_use_usb_serial_jtag;

/**
 * @brief 初始化串口收发通道。
 */
static void serial_io_init(void)
{
    /* 初始化串口配置通道。ESP32-S3 优先使用 USB Serial/JTAG 驱动，以获得更稳定的大图片分片传输。
     */
    if(s_serial_io_inited) {
        return;
    }

    s_serial_io_inited = true;

#if CONFIG_IDF_TARGET_ESP32S3
    /* ESP32-S3 优先使用 USB Serial/JTAG 直连，避免依赖控制台 VFS。 */
    usb_serial_jtag_driver_config_t usb_cfg = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
    /* 扩大 USB 收发缓冲，提高图片分片传输稳定性。 */
    usb_cfg.rx_buffer_size = SERIAL_CONFIG_USB_RX_BUF;
    usb_cfg.tx_buffer_size = SERIAL_CONFIG_USB_TX_BUF;
    esp_err_t usb_ret = usb_serial_jtag_driver_install(&usb_cfg);
    if(usb_ret == ESP_OK || usb_ret == ESP_ERR_INVALID_STATE) {
        s_use_usb_serial_jtag = true;
        ESP_LOGI(TAG, "serial config uses USB Serial/JTAG direct driver");
    } else {
        ESP_LOGW(TAG, "USB Serial/JTAG driver install failed: %s, fallback to stdio",
                 esp_err_to_name(usb_ret));
    }
#endif

    setvbuf(stdin, NULL, _IONBF, 0);
    setvbuf(stdout, NULL, _IONBF, 0);
}

static int serial_read_char(int timeout_ms)
{
    /* 按单字节读取串口输入。USB 驱动有超时等待；stdio 模式下读不到字符则延时，避免任务空转占满 CPU。
     */
#if CONFIG_IDF_TARGET_ESP32S3
    if(s_use_usb_serial_jtag) {
        uint8_t ch = 0;
        int len = usb_serial_jtag_read_bytes(&ch, 1, pdMS_TO_TICKS(timeout_ms));
        if(len == 1) {
            return (int)ch;
        }
        return -1;
    }
#endif

    int ch = getchar();
    if(ch < 0) {
        vTaskDelay(pdMS_TO_TICKS(timeout_ms));
    }
    return ch;
}

/* Bounded diagnostic export: unplugging USB must not block power-key handling
 * for one timeout per sample. A short write aborts the complete export. */
static bool serial_write_capture_line(const char *text)
{
#if CONFIG_IDF_TARGET_ESP32S3
    if(s_use_usb_serial_jtag) {
        char line[256];
        int len = snprintf(line, sizeof(line), "%s\n", text);
        if(len < 0 || len >= (int)sizeof(line)) return false;
        return usb_serial_jtag_write_bytes(line, len, pdMS_TO_TICKS(100)) == len;
    }
#endif
    return false; /* Capture requires native USB; leave legacy config fallback alone. */
}

static void serial_write_line(const char *text)
{
    /* 向上位机发送一行文本。所有响应都以换行结束，便于上位机按行解析 JSON。
     */
    if(text == NULL) {
        return;
    }

#if CONFIG_IDF_TARGET_ESP32S3
    if(s_use_usb_serial_jtag) {
        size_t len = strlen(text);
        if(len > 0) {
            (void)usb_serial_jtag_write_bytes((const uint8_t *)text,
                                              len,
                                              pdMS_TO_TICKS(1000));
        }
        (void)usb_serial_jtag_write_bytes((const uint8_t *)"\n",
                                          1,
                                          pdMS_TO_TICKS(1000));
        return;
    }
#endif

    printf("%s\n", text);
    fflush(stdout);
}


/**
 * @brief 可接收图片规格表。
 *
 * 每种图片都固定名称、宽高和 RGB565 字节数；串口命令必须匹配这里的规格，
 * 这样可以避免上位机传入错误尺寸导致 Flash 槽位越界。
 */
typedef struct {
    const char *name;
    int w;
    int h;
    size_t size;
} image_spec_t;

static const image_spec_t s_image_specs[] = {
    {"qr_code", WATCH_CONFIG_QR_W, WATCH_CONFIG_QR_H, WATCH_CONFIG_QR_W * WATCH_CONFIG_QR_H * 2U},
    {"cover",   WATCH_CONFIG_COVER_W, WATCH_CONFIG_COVER_H, WATCH_CONFIG_COVER_W * WATCH_CONFIG_COVER_H * 2U},
};

/**
 * @brief 当前图片接收事务上下文。
 *
 * active 表示已经收到 img_begin；expected_offset 用于强制顺序写入；
 * streaming 表示数据直接写入 Flash 流，而不是先完整缓存到 RAM。
 */
typedef struct {
    bool active;
    char name[16];
    int w;
    int h;
    size_t size;
    size_t expected_offset;
    size_t received_size;
    bool streaming;
    uint8_t *data;
} image_rx_ctx_t;

/* 单例图片接收状态：串口协议一次只允许传输一张图片。 */
static image_rx_ctx_t s_img_rx;

static void reply_json(cJSON *root)
{
    /* 把 cJSON 对象压缩成单行 JSON 并通过串口发回，随后释放临时字符串。
     */
    if(root == NULL) {
        return;
    }

    char *text = cJSON_PrintUnformatted(root);
    if(text != NULL) {
        serial_write_line(text);
        free(text);
    }
}

static void reply_simple(bool ok, const char *msg)
{
    /* 发送只包含 ok/msg 的通用回复，用于大多数成功或失败场景。
     */
    cJSON *root = cJSON_CreateObject();
    if(root == NULL) {
        return;
    }

    cJSON_AddBoolToObject(root, "ok", ok);
    cJSON_AddStringToObject(root, "msg", msg ? msg : "");
    reply_json(root);
    cJSON_Delete(root);
}

static void reply_named(bool ok, const char *msg, const char *name)
{
    /* 发送带 name 字段的回复，用于图片相关命令，让上位机知道当前响应对应哪个资源。
     */
    cJSON *root = cJSON_CreateObject();
    if(root == NULL) {
        return;
    }

    cJSON_AddBoolToObject(root, "ok", ok);
    cJSON_AddStringToObject(root, "msg", msg ? msg : "");
    if(name != NULL) {
        cJSON_AddStringToObject(root, "name", name);
    }

    reply_json(root);
    cJSON_Delete(root);
}

static void reply_chunk_ok(const char *name, int seq)
{
    /* 图片分片写入成功后返回序号确认，便于上位机做断点或重传判断。
     */
    cJSON *root = cJSON_CreateObject();
    if(root == NULL) {
        return;
    }

    cJSON_AddBoolToObject(root, "ok", true);
    cJSON_AddStringToObject(root, "msg", "chunk ok");
    cJSON_AddStringToObject(root, "name", name ? name : "");
    cJSON_AddNumberToObject(root, "seq", seq);
    reply_json(root);
    cJSON_Delete(root);
}

static const image_spec_t *find_image_spec(const char *name)
{
    if(name == NULL) {
        return NULL;
    }

    for(size_t i = 0; i < sizeof(s_image_specs) / sizeof(s_image_specs[0]); i++) {
        if(strcmp(name, s_image_specs[i].name) == 0) {
            return &s_image_specs[i];
        }
    }

    return NULL;
}

static void image_rx_reset(void)
{
    /* 清理图片接收上下文；如果正在进行流式写入，还会通知 watch_config 取消当前图片写入事务。
     */
    if(s_img_rx.active && s_img_rx.streaming) {
        watch_config_image_stream_cancel(s_img_rx.name);
    }

    free(s_img_rx.data);
    memset(&s_img_rx, 0, sizeof(s_img_rx));
}

static bool json_get_string_if_present(cJSON *root,
                                       const char *key,
                                       char *dst,
                                       size_t dst_size,
                                       size_t max_len)
{
    /* 从 JSON 中读取可选字符串字段，并同时做类型和长度检查，防止超过 watch_config_t 中的固定缓冲区。
     */
    if(root == NULL || key == NULL || dst == NULL || dst_size == 0) {
        return true;
    }

    cJSON *item = cJSON_GetObjectItem(root, key);
    if(item == NULL) {
        return true;
    }

    if(!cJSON_IsString(item) || item->valuestring == NULL) {
        reply_simple(false, "invalid string field");
        return false;
    }

    size_t len = strlen(item->valuestring);
    if(len >= max_len || len >= dst_size) {
        reply_simple(false, "string too long");
        return false;
    }

    memcpy(dst, item->valuestring, len + 1);
    return true;
}

static bool json_get_ascii_string_if_present(cJSON *root,
                                             const char *key,
                                             char *dst,
                                             size_t dst_size,
                                             size_t max_len)
{
    cJSON *item = cJSON_GetObjectItem(root, key);
    if(item == NULL) {
        return true;
    }
    if(!cJSON_IsString(item) || item->valuestring == NULL) {
        reply_simple(false, "invalid string field");
        return false;
    }

    size_t len = strlen(item->valuestring);
    if(len >= max_len || len >= dst_size) {
        reply_simple(false, "string too long");
        return false;
    }
    for(size_t i = 0; i < len; i++) {
        unsigned char ch = (unsigned char)item->valuestring[i];
        if(ch < 0x20 || ch > 0x7e) {
            reply_simple(false, "owner and device_name require ASCII");
            return false;
        }
    }

    memcpy(dst, item->valuestring, len + 1);
    return true;
}

/**
 * @brief 处理 set_config 命令并保存联网配置和设备信息。
 */
static bool handle_set_config(cJSON *root)
{
    /* 处理配置写入命令：先加载旧配置，再用 JSON 中存在的字段覆盖，最后整体保存到 NVS。
     */
    watch_config_t cfg;
    watch_device_info_t device_info;
    esp_err_t ret = watch_config_load(&cfg);
    if(ret != ESP_OK) {
        reply_simple(false, "load config failed");
        return false;
    }

    ret = watch_device_info_load(&device_info);
    if(ret != ESP_OK) {
        reply_simple(false, "load device info failed");
        return false;
    }

    if(!json_get_string_if_present(root, "wifi_ssid", cfg.wifi_ssid, sizeof(cfg.wifi_ssid), WATCH_CONFIG_WIFI_SSID_MAX)) return false;
    if(!json_get_string_if_present(root, "wifi_pass", cfg.wifi_pass, sizeof(cfg.wifi_pass), WATCH_CONFIG_WIFI_PASS_MAX)) return false;
    if(!json_get_string_if_present(root, "bili_uid", cfg.bili_uid, sizeof(cfg.bili_uid), WATCH_CONFIG_BILI_UID_MAX)) return false;
    if(!json_get_string_if_present(root, "sessdata", cfg.sessdata, sizeof(cfg.sessdata), WATCH_CONFIG_SESSDATA_MAX)) return false;
    if(!json_get_string_if_present(root, "latitude", cfg.latitude, sizeof(cfg.latitude), WATCH_CONFIG_COORD_MAX)) return false;
    if(!json_get_string_if_present(root, "longitude", cfg.longitude, sizeof(cfg.longitude), WATCH_CONFIG_COORD_MAX)) return false;
    if(!json_get_string_if_present(root, "host_addr", cfg.host_addr, sizeof(cfg.host_addr), WATCH_CONFIG_HOST_ADDR_MAX)) return false;
    if(!json_get_string_if_present(root, "pair_token", cfg.pair_token, sizeof(cfg.pair_token), WATCH_CONFIG_PAIR_TOKEN_MAX)) return false;
    cJSON *host_port = cJSON_GetObjectItem(root, "host_port");
    if(host_port != NULL) {
        if(!cJSON_IsNumber(host_port) || host_port->valuedouble < 1 || host_port->valuedouble > 65535 ||
           host_port->valuedouble != (double)host_port->valueint) {
            reply_simple(false, "invalid host_port");
            return false;
        }
        cfg.host_port = (uint16_t)host_port->valueint;
    }
    if(!json_get_ascii_string_if_present(root, "owner", device_info.owner, sizeof(device_info.owner), WATCH_DEVICE_OWNER_MAX)) return false;
    if(!json_get_ascii_string_if_present(root, "device_name", device_info.device_name, sizeof(device_info.device_name), WATCH_DEVICE_NAME_MAX)) return false;

    if(cJSON_GetObjectItem(root, "device_id") != NULL) {
        reply_simple(false, "device_id is read only");
        return false;
    }

    ret = watch_config_save(&cfg);
    if(ret != ESP_OK) {
        ESP_LOGW(TAG, "save config failed: %s", esp_err_to_name(ret));
        reply_simple(false, "save config failed");
        return false;
    }

    ret = watch_device_info_save(&device_info);
    if(ret != ESP_OK) {
        ESP_LOGW(TAG, "save device info failed: %s", esp_err_to_name(ret));
        reply_simple(false, "save device info failed");
        return false;
    }

    reply_simple(true, "config saved");
    return true;
}

static void handle_get_config(void)
{
    /* 读取当前配置并以 JSON 返回。敏感字段是否完整输出取决于本函数后续构造的字段策略。
     */
    watch_config_t cfg;
    watch_device_info_t device_info;
    esp_err_t ret = watch_config_load(&cfg);
    if(ret != ESP_OK) {
        reply_simple(false, "load config failed");
        return;
    }

    ret = watch_device_info_load(&device_info);
    if(ret != ESP_OK) {
        reply_simple(false, "load device info failed");
        return;
    }

    cJSON *root = cJSON_CreateObject();
    if(root == NULL) {
        reply_simple(false, "no memory");
        return;
    }

    cJSON_AddBoolToObject(root, "ok", true);
    cJSON_AddStringToObject(root, "msg", "config");
    cJSON_AddBoolToObject(root, "configured", watch_config_has_wifi(&cfg));
    cJSON_AddStringToObject(root, "wifi_ssid", cfg.wifi_ssid);
    cJSON_AddStringToObject(root, "bili_uid", cfg.bili_uid);
    cJSON_AddStringToObject(root, "latitude", cfg.latitude);
    cJSON_AddStringToObject(root, "longitude", cfg.longitude);
    cJSON_AddStringToObject(root, "host_addr", cfg.host_addr);
    cJSON_AddNumberToObject(root, "host_port", cfg.host_port);
    cJSON_AddStringToObject(root, "owner", device_info.owner);
    cJSON_AddStringToObject(root, "device_name", device_info.device_name);
    cJSON_AddStringToObject(root, "device_id", device_info.device_id);
    cJSON_AddBoolToObject(root, "has_wifi_pass", cfg.wifi_pass[0] != '\0');
    cJSON_AddBoolToObject(root, "has_sessdata", cfg.sessdata[0] != '\0');
    cJSON_AddBoolToObject(root, "has_pair_token", cfg.pair_token[0] != '\0');
    cJSON_AddBoolToObject(root, "has_qr_code", watch_config_image_exists("qr_code"));
    cJSON_AddBoolToObject(root, "has_cover", watch_config_image_exists("cover"));

    reply_json(root);
    cJSON_Delete(root);
}

/**
 * @brief 开始接收一张图片，校验名称、尺寸和数据长度。
 */
static bool handle_img_begin(cJSON *root)
{
    /* 开始一次图片接收事务：校验 name/尺寸/大小，初始化 RAM 或 Flash 流式写入上下文。
     */
    cJSON *name_obj = cJSON_GetObjectItem(root, "name");
    cJSON *w_obj = cJSON_GetObjectItem(root, "w");
    cJSON *h_obj = cJSON_GetObjectItem(root, "h");
    cJSON *size_obj = cJSON_GetObjectItem(root, "size");
    cJSON *fmt_obj = cJSON_GetObjectItem(root, "format");

    if(!cJSON_IsString(name_obj) || !cJSON_IsNumber(w_obj) ||
       !cJSON_IsNumber(h_obj) || !cJSON_IsNumber(size_obj)) {
        reply_simple(false, "invalid img_begin");
        return false;
    }

    if(fmt_obj != NULL && (!cJSON_IsString(fmt_obj) || strcmp(fmt_obj->valuestring, "rgb565") != 0)) {
        reply_named(false, "unsupported image format", name_obj->valuestring);
        return false;
    }

    const image_spec_t *spec = find_image_spec(name_obj->valuestring);
    if(spec == NULL) {
        reply_named(false, "unsupported image name", name_obj->valuestring);
        return false;
    }

    int w = w_obj->valueint;
    int h = h_obj->valueint;
    size_t size = (size_t)size_obj->valuedouble;

    if(w != spec->w || h != spec->h || size != spec->size) {
        reply_named(false, "image size mismatch", spec->name);
        return false;
    }

    image_rx_reset();

    /* 大尺寸 cover 图片边收边写 flash；小尺寸 QR 图片继续使用内存缓冲。 */
    bool use_stream = (strcmp(spec->name, "cover") == 0);

    if(use_stream) {
        esp_err_t ret = watch_config_image_stream_begin(spec->name, w, h, size);
        if(ret != ESP_OK) {
            ESP_LOGW(TAG, "image stream begin failed: %s", esp_err_to_name(ret));
            reply_named(false, "image stream begin failed", spec->name);
            return false;
        }

        s_img_rx.streaming = true;
    } else {
        s_img_rx.data = (uint8_t *)malloc(size);
        if(s_img_rx.data == NULL) {
            reply_named(false, "no memory for image", spec->name);
            return false;
        }

        s_img_rx.streaming = false;
    }

    s_img_rx.active = true;
    strncpy(s_img_rx.name, spec->name, sizeof(s_img_rx.name) - 1);
    s_img_rx.w = w;
    s_img_rx.h = h;
    s_img_rx.size = size;
    s_img_rx.expected_offset = 0;
    s_img_rx.received_size = 0;

    reply_named(true, "img_begin ok", spec->name);
    return true;
}

/**
 * @brief 接收并写入图片分片数据。
 */
static bool handle_img_chunk(cJSON *root)
{
    /* 处理图片分片：校验顺序 offset、Base64 解码、写入目标缓冲区或 Flash 流，最后返回分片确认。
     */
    if(!s_img_rx.active || (!s_img_rx.streaming && s_img_rx.data == NULL)) {
        reply_simple(false, "img not begun");
        return false;
    }

    cJSON *name_obj = cJSON_GetObjectItem(root, "name");
    cJSON *seq_obj = cJSON_GetObjectItem(root, "seq");
    cJSON *offset_obj = cJSON_GetObjectItem(root, "offset");
    cJSON *data_obj = cJSON_GetObjectItem(root, "data");

    if(!cJSON_IsString(name_obj) || !cJSON_IsNumber(seq_obj) ||
       !cJSON_IsNumber(offset_obj) || !cJSON_IsString(data_obj) || data_obj->valuestring == NULL) {
        reply_named(false, "invalid img_chunk", s_img_rx.name);
        return false;
    }

    if(strcmp(name_obj->valuestring, s_img_rx.name) != 0) {
        reply_named(false, "image name mismatch", s_img_rx.name);
        return false;
    }

    size_t offset = (size_t)offset_obj->valuedouble;
    if(offset >= s_img_rx.size) {
        reply_named(false, "chunk offset overflow", s_img_rx.name);
        return false;
    }

    /* 以 PC 端传入的 offset 为准写入，并记录最大已接收位置。 */
    if(offset != s_img_rx.expected_offset) {
        ESP_LOGW(TAG,
                 "chunk offset not sequential, name=%s got=%u expected=%u seq=%d",
                 s_img_rx.name,
                 (unsigned)offset,
                 (unsigned)s_img_rx.expected_offset,
                 seq_obj->valueint);
    }

    const unsigned char *b64 = (const unsigned char *)data_obj->valuestring;
    size_t b64_len = strlen(data_obj->valuestring);
    size_t out_max = (b64_len * 3U) / 4U + 4U;
    uint8_t *decoded = (uint8_t *)malloc(out_max);
    if(decoded == NULL) {
        reply_named(false, "no memory for chunk", s_img_rx.name);
        return false;
    }

    size_t decoded_len = 0;
    int dec_ret = mbedtls_base64_decode(decoded, out_max, &decoded_len, b64, b64_len);
    if(dec_ret != 0) {
        free(decoded);
        reply_named(false, "base64 decode failed", s_img_rx.name);
        return false;
    }

    if(offset + decoded_len > s_img_rx.size) {
        free(decoded);
        reply_named(false, "chunk overflow", s_img_rx.name);
        return false;
    }

    if(s_img_rx.streaming) {
        esp_err_t ret = watch_config_image_stream_write(s_img_rx.name, offset, decoded, decoded_len);
        free(decoded);

        if(ret != ESP_OK) {
            ESP_LOGW(TAG, "stream chunk write failed: %s", esp_err_to_name(ret));
            reply_named(false, "chunk write failed", s_img_rx.name);
            return false;
        }
    } else {
        memcpy(s_img_rx.data + offset, decoded, decoded_len);
        free(decoded);
    }

    size_t end_offset = offset + decoded_len;
    if(end_offset > s_img_rx.received_size) {
        s_img_rx.received_size = end_offset;
    }

    /*
     * expected_offset 只用于日志和顺序判断，不再作为硬性失败条件。
     */
    if(offset == s_img_rx.expected_offset) {
        s_img_rx.expected_offset = end_offset;
    } else if(end_offset > s_img_rx.expected_offset) {
        s_img_rx.expected_offset = end_offset;
    }

    reply_chunk_ok(s_img_rx.name, seq_obj->valueint);
    return true;
}

/**
 * @brief 完成图片传输并提交保存结果。
 */
static bool handle_img_end(cJSON *root)
{
    /* 结束图片事务：检查总长度是否完整，提交图片数据，并清理接收上下文。
     */
    if(!s_img_rx.active || (!s_img_rx.streaming && s_img_rx.data == NULL)) {
        reply_simple(false, "img not begun");
        return false;
    }

    cJSON *name_obj = cJSON_GetObjectItem(root, "name");
    cJSON *size_obj = cJSON_GetObjectItem(root, "size");

    if(!cJSON_IsString(name_obj) || !cJSON_IsNumber(size_obj)) {
        reply_named(false, "invalid img_end", s_img_rx.name);
        return false;
    }

    if(strcmp(name_obj->valuestring, s_img_rx.name) != 0) {
        reply_named(false, "image name mismatch", s_img_rx.name);
        return false;
    }

    size_t size = (size_t)size_obj->valuedouble;
    if(size != s_img_rx.size || s_img_rx.received_size < s_img_rx.size) {
        ESP_LOGW(TAG,
                 "image incomplete, name=%s received=%u expected=%u seq_offset=%u",
                 s_img_rx.name,
                 (unsigned)s_img_rx.received_size,
                 (unsigned)s_img_rx.size,
                 (unsigned)s_img_rx.expected_offset);
        reply_named(false, "image incomplete", s_img_rx.name);
        return false;
    }

    esp_err_t ret = ESP_OK;

    if(s_img_rx.streaming) {
        ret = watch_config_image_stream_end(s_img_rx.name);
    } else {
        ret = watch_config_save_image(s_img_rx.name,
                                      s_img_rx.data,
                                      s_img_rx.size,
                                      s_img_rx.w,
                                      s_img_rx.h);
    }

    if(ret != ESP_OK) {
        ESP_LOGW(TAG, "save image failed: %s", esp_err_to_name(ret));
        reply_named(false, "image save failed", s_img_rx.name);
        image_rx_reset();
        return false;
    }

    char saved_name[sizeof(s_img_rx.name)];
    strncpy(saved_name, s_img_rx.name, sizeof(saved_name) - 1);
    saved_name[sizeof(saved_name) - 1] = '\0';

    s_img_rx.active = false;
    image_rx_reset();

    reply_named(true, "image saved", saved_name);
    return true;
}

/**
 * @brief 根据 JSON command 字段分发串口配置命令。
 */
static void handle_command(cJSON *root)
{
    /* 解析 cmd 字段并分发到具体处理函数，是串口 JSON 协议的命令路由入口。
     */
    cJSON *cmd_obj = cJSON_GetObjectItem(root, "cmd");
    if(!cJSON_IsString(cmd_obj) || cmd_obj->valuestring == NULL) {
        reply_simple(false, "missing cmd");
        return;
    }

    const char *cmd = cmd_obj->valuestring;

    if(strcmp(cmd, "imu_capture") == 0) {
        cJSON *seconds = cJSON_GetObjectItem(root, "seconds");
        cJSON *delay = cJSON_GetObjectItem(root, "delay_ms");
        if(s_img_rx.active || !cJSON_IsNumber(seconds) || !cJSON_IsNumber(delay) ||
           seconds->valuedouble < 1 || seconds->valuedouble > 15 ||
           delay->valuedouble < 0 || delay->valuedouble > 10000 ||
           seconds->valuedouble != seconds->valueint || delay->valuedouble != delay->valueint) {
            serial_write_line("{\"imu\":\"error\",\"reason\":\"invalid_request_or_image_busy\"}");
        } else {
            watch_imu_capture_run((unsigned)seconds->valueint, (unsigned)delay->valueint, serial_write_capture_line);
        }
    } else if(strcmp(cmd, "get_steps") == 0) {
        watch_steps_snapshot_t steps;
        char reply[192];
        watch_steps_get_snapshot(&steps);
        snprintf(reply, sizeof(reply),
                 "{\"steps\":%lu,\"sensor_steps\":%lu,\"date\":%lu,"
                 "\"running\":%s,\"error\":%ld}",
                 (unsigned long)steps.today, (unsigned long)steps.sensor_total,
                 (unsigned long)steps.date_key, steps.running ? "true" : "false",
                 (long)steps.error);
        serial_write_line(reply);
    } else if(strcmp(cmd, "set_config") == 0) {
        handle_set_config(root);
    } else if(strcmp(cmd, "get_config") == 0) {
        handle_get_config();
    } else if(strcmp(cmd, "clear_config") == 0) {
        esp_err_t ret = watch_config_clear_all();
        if(ret == ESP_OK) {
            ret = watch_device_info_clear();
        }
        if(ret == ESP_OK) {
            reply_simple(true, "rebooting");
            vTaskDelay(pdMS_TO_TICKS(200));
            esp_restart();
        } else {
            reply_simple(false, "clear config failed");
        }
    } else if(strcmp(cmd, "img_begin") == 0) {
        handle_img_begin(root);
    } else if(strcmp(cmd, "img_chunk") == 0) {
        handle_img_chunk(root);
    } else if(strcmp(cmd, "img_end") == 0) {
        handle_img_end(root);
    } else if(strcmp(cmd, "img_cancel") == 0) {
        image_rx_reset();
        reply_simple(true, "img_cancel ok");
    } else if(strcmp(cmd, "reboot") == 0) {
        reply_simple(true, "rebooting");
        vTaskDelay(pdMS_TO_TICKS(200));
        esp_restart();
    } else {
        reply_simple(false, "unknown cmd");
    }
}

static void serial_config_task(void *arg)
{
    /* 串口配置后台任务：按行累积输入，解析 JSON，执行命令；异常行会丢弃并返回错误。
     */
    (void)arg;

    serial_io_init();

    char line[SERIAL_CONFIG_LINE_MAX];
    size_t pos = 0;
    bool overflow = false;

    ESP_LOGI(TAG, "serial config task started");

    while(1) {
        int ch = serial_read_char(10);
        if(ch < 0) {
            continue;
        }

        if(ch == '\r') {
            continue;
        }

        if(ch == '\n') {
            if(overflow) {
                overflow = false;
                pos = 0;
                reply_simple(false, "line too long");
                continue;
            }

            line[pos] = '\0';
            pos = 0;

            if(line[0] == '\0') {
                continue;
            }

            cJSON *root = cJSON_Parse(line);
            if(root == NULL || !cJSON_IsObject(root)) {
                if(root != NULL) {
                    cJSON_Delete(root);
                }
                reply_simple(false, "bad json");
                continue;
            }

            handle_command(root);
            cJSON_Delete(root);
            continue;
        }

        if(pos + 1 >= sizeof(line)) {
            overflow = true;
            continue;
        }

        if(!overflow) {
            line[pos++] = (char)ch;
        }
    }
}

/**
 * @brief 启动串口配置任务。
 */
esp_err_t watch_serial_config_start(void)
{
    /* 启动串口配置任务。使用 s_task_started 防止重复创建 FreeRTOS 任务。
     */
    if(s_task_started) {
        return ESP_OK;
    }

    s_task_started = true;
    BaseType_t ret = xTaskCreate(serial_config_task,
                                 "serial_config",
                                 SERIAL_CONFIG_TASK_STACK,
                                 NULL,
                                 SERIAL_CONFIG_TASK_PRIO,
                                 NULL);
    if(ret != pdPASS) {
        s_task_started = false;
        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
}


/* 维护提示
 * 新增串口命令时，请同时更新 handle_command() 分发逻辑、上位机协议文档和错误回复格式。
 */
