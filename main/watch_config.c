/**
 * @file watch_config.c
 * @brief 手表运行参数、账号信息、天气坐标和自定义图片的持久化管理。
 */
/*
 * ==================== 模块说明 ====================
 *  * 模块职责：
 * - 手表持久化配置中心，负责 WiFi、Bilibili、天气坐标以及自定义图片保存/读取。
 * - 小型字符串配置保存到 NVS，大尺寸图片保存到 data 分区固定槽位，避免 NVS 存放大块二进制。
 * - 图片使用 RGB565 原始数据，并带有自定义头部用于校验 magic、版本、格式、尺寸和长度。
 * - 运行时图片可来自 Flash mmap 或堆内存，因此释放时必须区分 mapped 与 malloc 两种来源。
 *
 * 阅读建议：
 * - 先看本文件顶部的宏定义和静态状态变量，理解硬件参数和运行状态。
 * - 再看 reset/init/start/on_key/destroy 等对外函数，理解页面或驱动的生命周期。
 * - 最后看 static 辅助函数，了解具体寄存器读写、UI 刷新或状态机细节。
 * =======================================================
 */

#include "watch_config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "esp_partition.h"

#define WATCH_CONFIG_NAMESPACE      "watch_cfg"
#define WATCH_CONFIG_KEY_WIFI_SSID  "wifi_ssid"
#define WATCH_CONFIG_KEY_WIFI_PASS  "wifi_pass"
#define WATCH_CONFIG_KEY_BILI_UID   "bili_uid"
#define WATCH_CONFIG_KEY_SESSDATA   "sessdata"
#define WATCH_CONFIG_KEY_LATITUDE   "latitude"
#define WATCH_CONFIG_KEY_LONGITUDE  "longitude"
#define WATCH_CONFIG_KEY_HOST_ADDR  "host_addr"
#define WATCH_CONFIG_KEY_HOST_PORT  "host_port"
#define WATCH_CONFIG_KEY_PAIR_TOKEN "pair_token"
#define WATCH_CONFIG_KEY_QR_IMG     "qr_img"
#define WATCH_CONFIG_KEY_COVER_IMG  "cover_img"

static const char *TAG = "watch_config";
/* 手表配置模块的 NVS 是否已经初始化。 */
static bool s_nvs_ready;

/**
 * @brief 运行时图片缓存。
 *
 * 图片可能来自 Flash mmap 或堆内存拷贝，释放时需要区分处理。
 */
typedef struct {
    uint8_t *data;
    const void *mapped_base;
    esp_partition_mmap_handle_t mmap_handle;
    lv_img_dsc_t dsc;
    bool tried_load;
    bool mapped;
} watch_runtime_img_t;

/* 运行时二维码图片缓存。 */
static watch_runtime_img_t s_qr_img;
/* 运行时封面图片缓存。 */
static watch_runtime_img_t s_cover_img;

/**
 * @brief image_name_to_key 辅助函数。
 *
 * 详细说明：
 * - 封装局部逻辑，使主流程更清晰。
 *
 * @param name 输入或输出参数，具体含义见调用处和函数内部使用方式。
 *
 * @return 函数执行结果或计算得到的值，具体语义见返回路径。
 */
static const char *image_name_to_key(const char *name)
{
    if(name == NULL) {
        return NULL;
    }

    if(strcmp(name, "qr_code") == 0 || strcmp(name, "qr") == 0) {
        return WATCH_CONFIG_KEY_QR_IMG;
    }

    if(strcmp(name, "cover") == 0) {
        return WATCH_CONFIG_KEY_COVER_IMG;
    }

    return NULL;
}

/**
 * @brief 释放运行时图片缓存。
 *
 * 详细说明：
 * - 根据图片来源选择 munmap 或 free。
 * - 释放后清零结构体，避免悬空指针。
 *
 * @param img 输入或输出参数，具体含义见调用处和函数内部使用方式。
 */
static void runtime_img_release(watch_runtime_img_t *img)
{
    if(img == NULL) {
        return;
    }

    if(img->mapped) {
        esp_partition_munmap(img->mmap_handle);
    } else {
        free(img->data);
    }

    memset(img, 0, sizeof(*img));
}

/**
 * @brief Flash 图片存储格式和槽位定义。
 *
 * QR 与封面图片保存为 RGB565 原始数据，并通过固定槽位快速定位。
 */
#define WATCH_IMAGE_PART_LABEL          "img_store"
#define WATCH_IMAGE_PART_FALLBACK_LABEL "storage"
#define WATCH_IMAGE_MAGIC               0x474D4957U
#define WATCH_IMAGE_VERSION             1U
#define WATCH_IMAGE_FORMAT_RGB565       1U
#define WATCH_IMAGE_SLOT_QR_OFFSET      0x00000U
#define WATCH_IMAGE_SLOT_COVER_OFFSET   0x10000U
#define WATCH_FLASH_SECTOR_SIZE         0x1000U

/**
 * @brief Flash 图片头。
 *
 * 用于校验图片格式、尺寸和数据长度，避免误读无效分区数据。
 */
typedef struct {
    uint32_t magic;
    uint32_t version;
    uint32_t format;
    uint32_t width;
    uint32_t height;
    uint32_t size;
    uint32_t reserved[10];
} watch_flash_img_hdr_t;

/**
 * @brief 分片写入图片时的临时上下文。
 */
typedef struct {
    bool active;
    char name[16];
    const esp_partition_t *part;
    uint32_t slot_offset;
    uint32_t erase_size;
    uint32_t width;
    uint32_t height;
    uint32_t size;
    uint32_t received_size;
} watch_image_stream_t;

/* 当前分片写入图片的状态上下文。 */
static watch_image_stream_t s_img_stream;

/**
 * @brief 把数值按指定对齐粒度向上取整。
 *
 * 详细说明：
 * - 用于计算 Flash 擦除大小等必须按扇区对齐的长度。
 *
 * @param value 输入或输出参数，具体含义见调用处和函数内部使用方式。
 * @param align 输入或输出参数，具体含义见调用处和函数内部使用方式。
 *
 * @return 函数执行结果或计算得到的值，具体语义见返回路径。
 */
static uint32_t align_up_u32(uint32_t value, uint32_t align)
{
    return (value + align - 1U) & ~(align - 1U);
}

/**
 * @brief 查找用于保存图片的 data 分区。
 */
static const esp_partition_t *watch_image_partition_find(void)
{
    const esp_partition_t *part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA,
                                                           ESP_PARTITION_SUBTYPE_ANY,
                                                           WATCH_IMAGE_PART_LABEL);
    if(part != NULL) {
        return part;
    }


    return esp_partition_find_first(ESP_PARTITION_TYPE_DATA,
                                    ESP_PARTITION_SUBTYPE_ANY,
                                    WATCH_IMAGE_PART_FALLBACK_LABEL);
}

/**
 * @brief 把图片配置键转换为 Flash 槽位信息。
 *
 * 详细说明：
 * - 返回对应偏移、宽高和数据大小。
 *
 * @param key 输入或输出参数，具体含义见调用处和函数内部使用方式。
 * @param slot_offset 输入或输出参数，具体含义见调用处和函数内部使用方式。
 * @param w 输入或输出参数，具体含义见调用处和函数内部使用方式。
 * @param h 输入或输出参数，具体含义见调用处和函数内部使用方式。
 * @param size 输入或输出参数，具体含义见调用处和函数内部使用方式。
 *
 * @return 函数执行结果或计算得到的值，具体语义见返回路径。
 */
static bool image_key_to_slot(const char *key,
                              uint32_t *slot_offset,
                              int *w,
                              int *h,
                              size_t *size)
{
    if(key == NULL) {
        return false;
    }

    if(strcmp(key, WATCH_CONFIG_KEY_QR_IMG) == 0) {
        if(slot_offset) *slot_offset = WATCH_IMAGE_SLOT_QR_OFFSET;
        if(w) *w = WATCH_CONFIG_QR_W;
        if(h) *h = WATCH_CONFIG_QR_H;
        if(size) *size = WATCH_CONFIG_QR_W * WATCH_CONFIG_QR_H * 2U;
        return true;
    }

    if(strcmp(key, WATCH_CONFIG_KEY_COVER_IMG) == 0) {
        if(slot_offset) *slot_offset = WATCH_IMAGE_SLOT_COVER_OFFSET;
        if(w) *w = WATCH_CONFIG_COVER_W;
        if(h) *h = WATCH_CONFIG_COVER_H;
        if(size) *size = WATCH_CONFIG_COVER_W * WATCH_CONFIG_COVER_H * 2U;
        return true;
    }

    return false;
}

/**
 * @brief 把外部图片名称转换为内部 Flash 规格。
 *
 * 详细说明：
 * - 用于串口/配置接口根据名称选择 QR 或封面槽位。
 *
 * @param name 输入或输出参数，具体含义见调用处和函数内部使用方式。
 * @param key 输入或输出参数，具体含义见调用处和函数内部使用方式。
 * @param slot_offset 输入或输出参数，具体含义见调用处和函数内部使用方式。
 * @param w 输入或输出参数，具体含义见调用处和函数内部使用方式。
 * @param h 输入或输出参数，具体含义见调用处和函数内部使用方式。
 * @param size 输入或输出参数，具体含义见调用处和函数内部使用方式。
 *
 * @return 函数执行结果或计算得到的值，具体语义见返回路径。
 */
static esp_err_t image_name_to_flash_spec(const char *name,
                                          const char **key,
                                          uint32_t *slot_offset,
                                          int *w,
                                          int *h,
                                          size_t *size)
{
    const char *k = image_name_to_key(name);
    if(k == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if(!image_key_to_slot(k, slot_offset, w, h, size)) {
        return ESP_ERR_INVALID_ARG;
    }

    if(key != NULL) {
        *key = k;
    }

    return ESP_OK;
}

/**
 * @brief 读取并校验 Flash 图片头。
 *
 * 详细说明：
 * - 检查 magic、版本、格式、尺寸和长度，防止误读无效数据。
 *
 * @param key 输入或输出参数，具体含义见调用处和函数内部使用方式。
 * @param out_part 输入或输出参数，具体含义见调用处和函数内部使用方式。
 * @param out_slot 输入或输出参数，具体含义见调用处和函数内部使用方式。
 * @param out_hdr 输入或输出参数，具体含义见调用处和函数内部使用方式。
 *
 * @return 函数执行结果或计算得到的值，具体语义见返回路径。
 */
static esp_err_t flash_image_read_header(const char *key,
                                         const esp_partition_t **out_part,
                                         uint32_t *out_slot,
                                         watch_flash_img_hdr_t *out_hdr)
{
    uint32_t slot = 0;
    int w = 0;
    int h = 0;
    size_t expected_size = 0;

    if(!image_key_to_slot(key, &slot, &w, &h, &expected_size)) {
        return ESP_ERR_INVALID_ARG;
    }

    const esp_partition_t *part = watch_image_partition_find();
    if(part == NULL) {
        return ESP_ERR_NOT_FOUND;
    }

    if(slot + sizeof(watch_flash_img_hdr_t) > part->size) {
        return ESP_ERR_INVALID_SIZE;
    }

    watch_flash_img_hdr_t hdr = {0};
    esp_err_t ret = esp_partition_read(part, slot, &hdr, sizeof(hdr));
    if(ret != ESP_OK) {
        return ret;
    }

    if(hdr.magic != WATCH_IMAGE_MAGIC ||
       hdr.version != WATCH_IMAGE_VERSION ||
       hdr.format != WATCH_IMAGE_FORMAT_RGB565 ||
       hdr.width != (uint32_t)w ||
       hdr.height != (uint32_t)h ||
       hdr.size != (uint32_t)expected_size) {
        return ESP_ERR_NOT_FOUND;
    }

    if(out_part) {
        *out_part = part;
    }
    if(out_slot) {
        *out_slot = slot;
    }
    if(out_hdr) {
        *out_hdr = hdr;
    }

    return ESP_OK;
}

/**
 * @brief 使指定 Flash 图片槽位失效。
 *
 * 详细说明：
 * - 通常通过擦除或写坏头部实现，用于清除图片。
 *
 * @param key 输入或输出参数，具体含义见调用处和函数内部使用方式。
 */
static void flash_image_invalidate(const char *key)
{
    const esp_partition_t *part = watch_image_partition_find();
    if(part == NULL) {
        return;
    }

    uint32_t slot = 0;
    if(!image_key_to_slot(key, &slot, NULL, NULL, NULL)) {
        return;
    }

    if(slot + WATCH_FLASH_SECTOR_SIZE <= part->size) {
        (void)esp_partition_erase_range(part, slot, WATCH_FLASH_SECTOR_SIZE);
    }
}

/**
 * @brief 开始一次图片分片写入并擦除目标槽位。
 *
 * 详细说明：
 * - 计算槽位、擦除范围并记录接收上下文。
 *
 * @param name 输入或输出参数，具体含义见函数内部使用方式。
 * @param w 输入或输出参数，具体含义见函数内部使用方式。
 * @param h 输入或输出参数，具体含义见函数内部使用方式。
 * @param size 输入或输出参数，具体含义见函数内部使用方式。
 */
static esp_err_t flash_image_stream_begin_internal(const char *name,
                                                   int w,
                                                   int h,
                                                   size_t size)
{
    const char *key = NULL;
    uint32_t slot = 0;
    int expected_w = 0;
    int expected_h = 0;
    size_t expected_size = 0;
    esp_err_t ret = image_name_to_flash_spec(name, &key, &slot, &expected_w, &expected_h, &expected_size);
    if(ret != ESP_OK) {
        return ret;
    }

    if(w != expected_w || h != expected_h || size != expected_size) {
        return ESP_ERR_INVALID_SIZE;
    }

    const esp_partition_t *part = watch_image_partition_find();
    if(part == NULL) {
        ESP_LOGW(TAG, "image partition not found, create data partition label '%s'", WATCH_IMAGE_PART_LABEL);
        return ESP_ERR_NOT_FOUND;
    }

    uint32_t total_size = (uint32_t)(sizeof(watch_flash_img_hdr_t) + size);
    uint32_t erase_size = align_up_u32(total_size, WATCH_FLASH_SECTOR_SIZE);

    if(slot + erase_size > part->size) {
        ESP_LOGW(TAG,
                 "image partition too small for %s: slot=0x%lx erase=0x%lx part_size=0x%lx",
                 name,
                 (unsigned long)slot,
                 (unsigned long)erase_size,
                 (unsigned long)part->size);
        return ESP_ERR_INVALID_SIZE;
    }

    runtime_img_release(strcmp(key, WATCH_CONFIG_KEY_QR_IMG) == 0 ? &s_qr_img : &s_cover_img);

    ret = esp_partition_erase_range(part, slot, erase_size);
    if(ret != ESP_OK) {
        ESP_LOGW(TAG, "erase image slot failed: %s", esp_err_to_name(ret));
        return ret;
    }


    memset(&s_img_stream, 0, sizeof(s_img_stream));
    s_img_stream.active = true;
    strncpy(s_img_stream.name, name, sizeof(s_img_stream.name) - 1);
    s_img_stream.part = part;
    s_img_stream.slot_offset = slot;
    s_img_stream.erase_size = erase_size;
    s_img_stream.width = (uint32_t)w;
    s_img_stream.height = (uint32_t)h;
    s_img_stream.size = (uint32_t)size;
    s_img_stream.received_size = 0;

    ESP_LOGI(TAG,
             "image stream begin %s, part=%s, slot=0x%lx, size=%u",
             name,
             part->label,
             (unsigned long)slot,
             (unsigned)size);

    return ESP_OK;
}

/**
 * @brief 对外开始分片写入图片。
 *
 * 详细说明：
 * - 用于串口配置或网络配置分块上传大图。
 *
 * @param name 输入或输出参数，具体含义见调用处和函数内部使用方式。
 * @param w 输入或输出参数，具体含义见调用处和函数内部使用方式。
 * @param h 输入或输出参数，具体含义见调用处和函数内部使用方式。
 * @param size 输入或输出参数，具体含义见调用处和函数内部使用方式。
 *
 * @return 函数执行结果或计算得到的值，具体语义见返回路径。
 */
esp_err_t watch_config_image_stream_begin(const char *name,
                                          int w,
                                          int h,
                                          size_t size)
{
    return flash_image_stream_begin_internal(name, w, h, size);
}

/**
 * @brief 写入一段图片数据到 Flash。
 *
 * 详细说明：
 * - 检查偏移和长度，避免越界写入槽位。
 *
 * @param name 输入或输出参数，具体含义见调用处和函数内部使用方式。
 * @param offset 输入或输出参数，具体含义见调用处和函数内部使用方式。
 * @param data 输入或输出参数，具体含义见调用处和函数内部使用方式。
 * @param len 输入或输出参数，具体含义见调用处和函数内部使用方式。
 *
 * @return 函数执行结果或计算得到的值，具体语义见返回路径。
 */
esp_err_t watch_config_image_stream_write(const char *name,
                                          size_t offset,
                                          const uint8_t *data,
                                          size_t len)
{
    if(!s_img_stream.active || data == NULL || len == 0) {
        return ESP_ERR_INVALID_STATE;
    }

    if(name == NULL || strcmp(name, s_img_stream.name) != 0) {
        return ESP_ERR_INVALID_ARG;
    }

    if(offset + len > s_img_stream.size) {
        return ESP_ERR_INVALID_SIZE;
    }

    esp_err_t ret = esp_partition_write(s_img_stream.part,
                                        s_img_stream.slot_offset + sizeof(watch_flash_img_hdr_t) + offset,
                                        data,
                                        len);
    if(ret != ESP_OK) {
        ESP_LOGW(TAG, "stream write failed: %s", esp_err_to_name(ret));
        return ret;
    }

    uint32_t end_offset = (uint32_t)(offset + len);
    if(end_offset > s_img_stream.received_size) {
        s_img_stream.received_size = end_offset;
    }

    return ESP_OK;
}

/**
 * @brief 结束分片图片写入并提交头部。
 *
 * 详细说明：
 * - 确认收到完整数据后写入有效头，使图片可被加载。
 *
 * @param name 输入或输出参数，具体含义见调用处和函数内部使用方式。
 *
 * @return 函数执行结果或计算得到的值，具体语义见返回路径。
 */
esp_err_t watch_config_image_stream_end(const char *name)
{
    if(!s_img_stream.active) {
        return ESP_ERR_INVALID_STATE;
    }

    if(name == NULL || strcmp(name, s_img_stream.name) != 0) {
        return ESP_ERR_INVALID_ARG;
    }

    if(s_img_stream.received_size < s_img_stream.size) {
        ESP_LOGW(TAG,
                 "image stream incomplete: name=%s received=%u expected=%u",
                 s_img_stream.name,
                 (unsigned)s_img_stream.received_size,
                 (unsigned)s_img_stream.size);
        return ESP_ERR_INVALID_SIZE;
    }

    watch_flash_img_hdr_t hdr = {0};
    hdr.magic = WATCH_IMAGE_MAGIC;
    hdr.version = WATCH_IMAGE_VERSION;
    hdr.format = WATCH_IMAGE_FORMAT_RGB565;
    hdr.width = s_img_stream.width;
    hdr.height = s_img_stream.height;
    hdr.size = s_img_stream.size;

    esp_err_t ret = esp_partition_write(s_img_stream.part,
                                        s_img_stream.slot_offset,
                                        &hdr,
                                        sizeof(hdr));
    if(ret != ESP_OK) {
        ESP_LOGW(TAG, "final image header write failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGI(TAG,
             "image stream saved %s, size=%u",
             s_img_stream.name,
             (unsigned)s_img_stream.size);

    memset(&s_img_stream, 0, sizeof(s_img_stream));
    return ESP_OK;
}

/**
 * @brief 取消当前分片图片写入。
 *
 * 详细说明：
 * - 清理上下文，避免半包数据被当作有效图片。
 *
 * @param name 输入或输出参数，具体含义见调用处和函数内部使用方式。
 */
void watch_config_image_stream_cancel(const char *name)
{
    if(!s_img_stream.active) {
        return;
    }

    if(name == NULL || strcmp(name, s_img_stream.name) == 0) {
        flash_image_invalidate(image_name_to_key(s_img_stream.name));
        memset(&s_img_stream, 0, sizeof(s_img_stream));
    }
}

/**
 * @brief 将完整图片数据通过分片接口写入 Flash。
 *
 * 详细说明：
 * - 适合已有完整 buffer 的调用场景。
 *
 * @param name 输入或输出参数，具体含义见函数内部使用方式。
 * @param data 输入或输出参数，具体含义见函数内部使用方式。
 * @param size 输入或输出参数，具体含义见函数内部使用方式。
 * @param w 输入或输出参数，具体含义见函数内部使用方式。
 * @param h 输入或输出参数，具体含义见函数内部使用方式。
 */
static esp_err_t flash_image_save_whole(const char *name,
                                        const uint8_t *data,
                                        size_t size,
                                        int w,
                                        int h)
{
    esp_err_t ret = watch_config_image_stream_begin(name, w, h, size);
    if(ret != ESP_OK) {
        return ret;
    }

    ret = watch_config_image_stream_write(name, 0, data, size);
    if(ret != ESP_OK) {
        watch_config_image_stream_cancel(name);
        return ret;
    }

    ret = watch_config_image_stream_end(name);
    if(ret != ESP_OK) {
        watch_config_image_stream_cancel(name);
    }

    return ret;
}


/**
 * @brief 初始化配置子系统。
 *
 * 详细说明：
 * - 初始化 NVS，并准备后续配置读取。
 *
 * @return 函数执行结果或计算得到的值，具体语义见返回路径。
 */
esp_err_t watch_config_init(void)
{
    if(s_nvs_ready) {
        return ESP_OK;
    }

    esp_err_t ret = nvs_flash_init();
    if(ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS needs erase: %s", esp_err_to_name(ret));
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }

    if(ret == ESP_OK) {
        s_nvs_ready = true;
    }

    return ret;
}

/**
 * @brief 安全读取 NVS 字符串。
 *
 * 详细说明：
 * - 读取失败时写入空字符串，避免上层使用未初始化内容。
 *
 * @param nvs 输入或输出参数，具体含义见调用处和函数内部使用方式。
 * @param key 输入或输出参数，具体含义见调用处和函数内部使用方式。
 * @param out 输入或输出参数，具体含义见调用处和函数内部使用方式。
 * @param out_size 输入或输出参数，具体含义见调用处和函数内部使用方式。
 */
static void nvs_get_str_safe(nvs_handle_t nvs,
                             const char *key,
                             char *out,
                             size_t out_size)
{
    if(out == NULL || out_size == 0) {
        return;
    }

    out[0] = '\0';

    size_t len = out_size;
    esp_err_t ret = nvs_get_str(nvs, key, out, &len);
    if(ret != ESP_OK) {
        out[0] = '\0';
    } else {
        out[out_size - 1] = '\0';
    }
}

/**
 * @brief 加载手表配置。
 *
 * 详细说明：
 * - 读取 WiFi、Bilibili、天气坐标等参数。
 *
 * @param cfg 输入或输出参数，具体含义见调用处和函数内部使用方式。
 *
 * @return 函数执行结果或计算得到的值，具体语义见返回路径。
 */
esp_err_t watch_config_load(watch_config_t *cfg)
{
    if(cfg == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(cfg, 0, sizeof(*cfg));
    cfg->host_port = WATCH_CONFIG_HOST_PORT_DEFAULT;

    esp_err_t ret = watch_config_init();
    if(ret != ESP_OK) {
        return ret;
    }

    nvs_handle_t nvs;
    ret = nvs_open(WATCH_CONFIG_NAMESPACE, NVS_READONLY, &nvs);
    if(ret == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_OK;
    }
    if(ret != ESP_OK) {
        return ret;
    }

    nvs_get_str_safe(nvs, WATCH_CONFIG_KEY_WIFI_SSID, cfg->wifi_ssid, sizeof(cfg->wifi_ssid));
    nvs_get_str_safe(nvs, WATCH_CONFIG_KEY_WIFI_PASS, cfg->wifi_pass, sizeof(cfg->wifi_pass));
    nvs_get_str_safe(nvs, WATCH_CONFIG_KEY_BILI_UID, cfg->bili_uid, sizeof(cfg->bili_uid));
    nvs_get_str_safe(nvs, WATCH_CONFIG_KEY_SESSDATA, cfg->sessdata, sizeof(cfg->sessdata));
    nvs_get_str_safe(nvs, WATCH_CONFIG_KEY_LATITUDE, cfg->latitude, sizeof(cfg->latitude));
    nvs_get_str_safe(nvs, WATCH_CONFIG_KEY_LONGITUDE, cfg->longitude, sizeof(cfg->longitude));
    nvs_get_str_safe(nvs, WATCH_CONFIG_KEY_HOST_ADDR, cfg->host_addr, sizeof(cfg->host_addr));
    nvs_get_str_safe(nvs, WATCH_CONFIG_KEY_PAIR_TOKEN, cfg->pair_token, sizeof(cfg->pair_token));
    uint16_t host_port = WATCH_CONFIG_HOST_PORT_DEFAULT;
    if(nvs_get_u16(nvs, WATCH_CONFIG_KEY_HOST_PORT, &host_port) == ESP_OK && host_port != 0) {
        cfg->host_port = host_port;
    }

    nvs_close(nvs);
    return ESP_OK;
}

/**
 * @brief 保存手表配置。
 *
 * 详细说明：
 * - 把配置结构体中的字符串和数值写入 NVS。
 *
 * @param cfg 输入或输出参数，具体含义见调用处和函数内部使用方式。
 *
 * @return 函数执行结果或计算得到的值，具体语义见返回路径。
 */
esp_err_t watch_config_save(const watch_config_t *cfg)
{
    if(cfg == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t ret = watch_config_init();
    if(ret != ESP_OK) {
        return ret;
    }

    nvs_handle_t nvs;
    ret = nvs_open(WATCH_CONFIG_NAMESPACE, NVS_READWRITE, &nvs);
    if(ret != ESP_OK) {
        return ret;
    }

    ret = nvs_set_str(nvs, WATCH_CONFIG_KEY_WIFI_SSID, cfg->wifi_ssid);
    if(ret == ESP_OK) ret = nvs_set_str(nvs, WATCH_CONFIG_KEY_WIFI_PASS, cfg->wifi_pass);
    if(ret == ESP_OK) ret = nvs_set_str(nvs, WATCH_CONFIG_KEY_BILI_UID, cfg->bili_uid);
    if(ret == ESP_OK) ret = nvs_set_str(nvs, WATCH_CONFIG_KEY_SESSDATA, cfg->sessdata);
    if(ret == ESP_OK) ret = nvs_set_str(nvs, WATCH_CONFIG_KEY_LATITUDE, cfg->latitude);
    if(ret == ESP_OK) ret = nvs_set_str(nvs, WATCH_CONFIG_KEY_LONGITUDE, cfg->longitude);
    if(ret == ESP_OK) ret = nvs_set_str(nvs, WATCH_CONFIG_KEY_HOST_ADDR, cfg->host_addr);
    if(ret == ESP_OK) ret = nvs_set_u16(nvs, WATCH_CONFIG_KEY_HOST_PORT,
                                        cfg->host_port == 0 ? WATCH_CONFIG_HOST_PORT_DEFAULT : cfg->host_port);
    if(ret == ESP_OK) ret = nvs_set_str(nvs, WATCH_CONFIG_KEY_PAIR_TOKEN, cfg->pair_token);
    if(ret == ESP_OK) ret = nvs_commit(nvs);

    nvs_close(nvs);
    return ret;
}

bool watch_config_has_host(const watch_config_t *cfg)
{
    return cfg != NULL && cfg->host_addr[0] != '\0' &&
           cfg->host_port != 0 && cfg->pair_token[0] != '\0';
}

/**
 * @brief 清除所有手表配置。
 *
 * 详细说明：
 * - 用于恢复出厂或重新配网。
 *
 * @return 函数执行结果或计算得到的值，具体语义见返回路径。
 */
esp_err_t watch_config_clear_all(void)
{
    esp_err_t ret = watch_config_init();
    if(ret != ESP_OK) {
        return ret;
    }

    nvs_handle_t nvs;
    ret = nvs_open(WATCH_CONFIG_NAMESPACE, NVS_READWRITE, &nvs);
    if(ret == ESP_ERR_NVS_NOT_FOUND) {
        ret = ESP_OK;
    } else if(ret == ESP_OK) {
        ret = nvs_erase_all(nvs);
        if(ret == ESP_OK) {
            ret = nvs_commit(nvs);
        }
        nvs_close(nvs);
    }

    flash_image_invalidate(WATCH_CONFIG_KEY_QR_IMG);
    flash_image_invalidate(WATCH_CONFIG_KEY_COVER_IMG);

    runtime_img_release(&s_qr_img);
    runtime_img_release(&s_cover_img);
    return ret;
}

/**
 * @brief 判断配置中是否包含 WiFi 信息。
 *
 * 详细说明：
 * - SSID 不为空时认为可尝试联网。
 *
 * @param cfg 输入或输出参数，具体含义见调用处和函数内部使用方式。
 *
 * @return 函数执行结果或计算得到的值，具体语义见返回路径。
 */
bool watch_config_has_wifi(const watch_config_t *cfg)
{
    return cfg != NULL && cfg->wifi_ssid[0] != '\0';
}

/**
 * @brief 判断配置中是否包含 Bilibili 信息。
 *
 * 详细说明：
 * - UID 和 SESSDATA 都存在才认为可拉取统计。
 *
 * @param cfg 输入或输出参数，具体含义见调用处和函数内部使用方式。
 *
 * @return 函数执行结果或计算得到的值，具体语义见返回路径。
 */
bool watch_config_has_bili(const watch_config_t *cfg)
{
    return cfg != NULL && cfg->bili_uid[0] != '\0';
}

/**
 * @brief 判断配置中是否包含天气坐标。
 *
 * 详细说明：
 * - 经纬度有效时才可请求天气。
 *
 * @param cfg 输入或输出参数，具体含义见调用处和函数内部使用方式。
 *
 * @return 函数执行结果或计算得到的值，具体语义见返回路径。
 */
bool watch_config_has_weather(const watch_config_t *cfg)
{
    return cfg != NULL && cfg->latitude[0] != '\0' && cfg->longitude[0] != '\0';
}

/**
 * @brief 保存用户自定义图片。
 *
 * 详细说明：
 * - 根据名称选择 QR 或封面槽位并写入 RGB565 数据。
 *
 * @param name 输入或输出参数，具体含义见调用处和函数内部使用方式。
 * @param data 输入或输出参数，具体含义见调用处和函数内部使用方式。
 * @param size 输入或输出参数，具体含义见调用处和函数内部使用方式。
 * @param w 输入或输出参数，具体含义见调用处和函数内部使用方式。
 * @param h 输入或输出参数，具体含义见调用处和函数内部使用方式。
 *
 * @return 函数执行结果或计算得到的值，具体语义见返回路径。
 */
esp_err_t watch_config_save_image(const char *name,
                                  const uint8_t *data,
                                  size_t size,
                                  int w,
                                  int h)
{
    if(data == NULL || size == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    const char *key = NULL;
    uint32_t slot = 0;
    int expected_w = 0;
    int expected_h = 0;
    size_t expected_size = 0;

    esp_err_t ret = image_name_to_flash_spec(name, &key, &slot, &expected_w, &expected_h, &expected_size);
    if(ret != ESP_OK) {
        return ret;
    }

    (void)slot;

    if(w != expected_w || h != expected_h || size != expected_size) {
        return ESP_ERR_INVALID_SIZE;
    }

    ESP_LOGI(TAG, "save image %s to flash partition, %dx%d, size=%u", name, w, h, (unsigned)size);
    ret = flash_image_save_whole(name, data, size, w, h);

    if(ret == ESP_OK) {
        if(strcmp(key, WATCH_CONFIG_KEY_QR_IMG) == 0) {
            runtime_img_release(&s_qr_img);
        } else if(strcmp(key, WATCH_CONFIG_KEY_COVER_IMG) == 0) {
            runtime_img_release(&s_cover_img);
        }
    }

    return ret;
}

/**
 * @brief 判断指定自定义图片是否存在。
 *
 * 详细说明：
 * - 通过读取图片头判断 Flash 中是否有有效图片。
 *
 * @param name 输入或输出参数，具体含义见调用处和函数内部使用方式。
 *
 * @return 函数执行结果或计算得到的值，具体语义见返回路径。
 */
bool watch_config_image_exists(const char *name)
{
    const char *key = image_name_to_key(name);
    if(key == NULL) {
        return false;
    }

    if(flash_image_read_header(key, NULL, NULL, NULL) == ESP_OK) {
        return true;
    }


    if(watch_config_init() != ESP_OK) {
        return false;
    }

    nvs_handle_t nvs;
    esp_err_t ret = nvs_open(WATCH_CONFIG_NAMESPACE, NVS_READONLY, &nvs);
    if(ret != ESP_OK) {
        return false;
    }

    size_t size = 0;
    ret = nvs_get_blob(nvs, key, NULL, &size);
    nvs_close(nvs);
    return ret == ESP_OK && size > 0;
}

/**
 * @brief 填充 LVGL 图片描述符。
 *
 * 详细说明：
 * - 把 RGB565 原始数据包装成 lv_img_dsc_t，供 LVGL 显示。
 *
 * @param img 输入或输出参数，具体含义见调用处和函数内部使用方式。
 * @param data 输入或输出参数，具体含义见调用处和函数内部使用方式。
 * @param size 输入或输出参数，具体含义见调用处和函数内部使用方式。
 * @param w 输入或输出参数，具体含义见调用处和函数内部使用方式。
 * @param h 输入或输出参数，具体含义见调用处和函数内部使用方式。
 */
static void runtime_img_dsc_fill(watch_runtime_img_t *img,
                                 const uint8_t *data,
                                 size_t size,
                                 int w,
                                 int h)
{
    memset(&img->dsc, 0, sizeof(img->dsc));

#if LVGL_VERSION_MAJOR >= 9
#ifdef LV_IMAGE_HEADER_MAGIC
    img->dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
#endif
    img->dsc.header.cf = LV_COLOR_FORMAT_RGB565;
#else
    img->dsc.header.cf = LV_IMG_CF_TRUE_COLOR;
#endif

    img->dsc.header.w = w;
    img->dsc.header.h = h;
    img->dsc.data_size = size;
    img->dsc.data = data;
}

/**
 * @brief 从 Flash 或旧版 NVS 中加载运行时图片。
 */
static const lv_img_dsc_t *runtime_img_load(watch_runtime_img_t *img,
                                            const char *key,
                                            int w,
                                            int h)
{
    if(img == NULL || key == NULL) {
        return NULL;
    }

    if(img->tried_load) {
        return (img->data != NULL || img->mapped) ? &img->dsc : NULL;
    }

    img->tried_load = true;

    const esp_partition_t *part = NULL;
    uint32_t slot = 0;
    watch_flash_img_hdr_t hdr = {0};

    if(flash_image_read_header(key, &part, &slot, &hdr) == ESP_OK) {
        const void *mapped = NULL;
        esp_partition_mmap_handle_t handle = 0;
        size_t map_size = sizeof(watch_flash_img_hdr_t) + hdr.size;

        esp_err_t ret = esp_partition_mmap(part,
                                           slot,
                                           map_size,
                                           ESP_PARTITION_MMAP_DATA,
                                           &mapped,
                                           &handle);
        if(ret == ESP_OK) {
            const uint8_t *data = (const uint8_t *)mapped + sizeof(watch_flash_img_hdr_t);

            img->mapped_base = mapped;
            img->mmap_handle = handle;
            img->mapped = true;
            img->data = (uint8_t *)data;

            runtime_img_dsc_fill(img, data, hdr.size, (int)hdr.width, (int)hdr.height);

            ESP_LOGI(TAG,
                     "loaded image %s from flash partition %s, %dx%d, size=%u",
                     key,
                     part->label,
                     (int)hdr.width,
                     (int)hdr.height,
                     (unsigned)hdr.size);
            return &img->dsc;
        }

        ESP_LOGW(TAG, "mmap image %s failed: %s", key, esp_err_to_name(ret));
    }


    if(watch_config_init() != ESP_OK) {
        return NULL;
    }

    nvs_handle_t nvs;
    esp_err_t ret = nvs_open(WATCH_CONFIG_NAMESPACE, NVS_READONLY, &nvs);
    if(ret != ESP_OK) {
        return NULL;
    }

    size_t size = 0;
    ret = nvs_get_blob(nvs, key, NULL, &size);
    if(ret != ESP_OK) {
        nvs_close(nvs);
        return NULL;
    }

    size_t expected = (size_t)w * (size_t)h * 2U;
    if(size != expected) {
        ESP_LOGW(TAG, "image %s size mismatch: got=%u expected=%u", key, (unsigned)size, (unsigned)expected);
        nvs_close(nvs);
        return NULL;
    }

    uint8_t *data = (uint8_t *)malloc(size);
    if(data == NULL) {
        nvs_close(nvs);
        ESP_LOGW(TAG, "no memory to load image %s from NVS, size=%u", key, (unsigned)size);
        return NULL;
    }

    ret = nvs_get_blob(nvs, key, data, &size);
    nvs_close(nvs);
    if(ret != ESP_OK) {
        free(data);
        return NULL;
    }

    img->data = data;
    runtime_img_dsc_fill(img, data, size, w, h);

    ESP_LOGI(TAG, "loaded image %s from NVS, %dx%d, size=%u", key, w, h, (unsigned)size);
    return &img->dsc;
}

/**
 * @brief watch_config_get_qr_image 对外接口。
 *
 * 详细说明：
 * - 供其他模块调用，隐藏本文件内部状态细节。
 *
 * @return 函数执行结果或计算得到的值，具体语义见返回路径。
 */
const lv_img_dsc_t *watch_config_get_qr_image(void)
{
    return runtime_img_load(&s_qr_img, WATCH_CONFIG_KEY_QR_IMG, WATCH_CONFIG_QR_W, WATCH_CONFIG_QR_H);
}

/**
 * @brief watch_config_get_cover_image 对外接口。
 *
 * 详细说明：
 * - 供其他模块调用，隐藏本文件内部状态细节。
 *
 * @return 函数执行结果或计算得到的值，具体语义见返回路径。
 */
const lv_img_dsc_t *watch_config_get_cover_image(void)
{
    return runtime_img_load(&s_cover_img, WATCH_CONFIG_KEY_COVER_IMG, WATCH_CONFIG_COVER_W, WATCH_CONFIG_COVER_H);
}
