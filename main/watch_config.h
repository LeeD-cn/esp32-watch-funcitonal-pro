/**
 * @file watch_config.h
 * @brief 手表配置与自定义图片读取接口。
 */
#ifndef WATCH_CONFIG_H
#define WATCH_CONFIG_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

#define WATCH_CONFIG_WIFI_SSID_MAX      33
#define WATCH_CONFIG_WIFI_PASS_MAX      65
#define WATCH_CONFIG_BILI_UID_MAX       32
#define WATCH_CONFIG_SESSDATA_MAX       1024
#define WATCH_CONFIG_COORD_MAX          32

#define WATCH_CONFIG_QR_W               74
#define WATCH_CONFIG_QR_H               74
#define WATCH_CONFIG_COVER_W            240
#define WATCH_CONFIG_COVER_H            240

/**
 * @brief 手表配置参数。
 *
 * 保存 Wi-Fi、Bilibili 账号凭据和天气坐标等字符串配置。
 */
typedef struct {
    char wifi_ssid[WATCH_CONFIG_WIFI_SSID_MAX];
    char wifi_pass[WATCH_CONFIG_WIFI_PASS_MAX];
    char bili_uid[WATCH_CONFIG_BILI_UID_MAX];
    char sessdata[WATCH_CONFIG_SESSDATA_MAX];
    char latitude[WATCH_CONFIG_COORD_MAX];
    char longitude[WATCH_CONFIG_COORD_MAX];
} watch_config_t;

/**
 * @brief 初始化配置模块和 NVS。
 *
 * @return ESP_OK 初始化成功；其他值表示 NVS 初始化失败。
 */
esp_err_t watch_config_init(void);
/**
 * @brief 从 NVS 读取手表配置。
 *
 * @param cfg 输出配置结构体。
 * @return ESP_OK 读取完成；ESP_ERR_INVALID_ARG 表示参数为空。
 */
esp_err_t watch_config_load(watch_config_t *cfg);
/**
 * @brief 保存手表配置到 NVS。
 *
 * @param cfg 待保存的配置结构体。
 * @return ESP_OK 保存成功；其他值表示 NVS 写入失败。
 */
esp_err_t watch_config_save(const watch_config_t *cfg);
/**
 * @brief 清除当前模块保存的全部配置项。
 *
 * @return ESP_OK 清除成功；其他值表示 NVS 操作失败。
 */
esp_err_t watch_config_clear_all(void);

/**
 * @brief 判断配置中是否包含完整 Wi-Fi 信息。
 *
 * @param cfg 配置结构体。
 * @return true SSID 和密码均非空；false 配置不完整。
 */
bool watch_config_has_wifi(const watch_config_t *cfg);
/**
 * @brief 判断配置中是否包含 Bilibili 统计所需信息。
 *
 * @param cfg 配置结构体。
 * @return true UID 和 SESSDATA 均非空；false 配置不完整。
 */
bool watch_config_has_bili(const watch_config_t *cfg);
/**
 * @brief 判断配置中是否包含天气坐标。
 *
 * @param cfg 配置结构体。
 * @return true 经纬度均非空；false 配置不完整。
 */
bool watch_config_has_weather(const watch_config_t *cfg);

/**
 * @brief 保存一张 RGB565 图片到 Flash 图片槽。
 *
 * @param name 图片名称，支持 qr/qr_code 和 cover。
 * @param data RGB565 原始像素数据。
 * @param size 数据长度，必须与图片尺寸匹配。
 * @param w 图片宽度。
 * @param h 图片高度。
 * @return ESP_OK 保存成功；其他值表示参数、尺寸或 Flash 操作失败。
 */
esp_err_t watch_config_save_image(const char *name,
                                  const uint8_t *data,
                                  size_t size,
                                  int w,
                                  int h);
/**
 * @brief 判断指定图片是否已保存。
 *
 * @param name 图片名称。
 * @return true 图片存在；false 图片不存在或名称无效。
 */
bool watch_config_image_exists(const char *name);
/**
 * @brief 获取运行时二维码图片描述符。
 *
 * @return 成功时返回 LVGL 图片描述符；未保存或加载失败时返回 NULL。
 */
const lv_img_dsc_t *watch_config_get_qr_image(void);
/**
 * @brief 获取运行时封面图片描述符。
 *
 * @return 成功时返回 LVGL 图片描述符；未保存或加载失败时返回 NULL。
 */
const lv_img_dsc_t *watch_config_get_cover_image(void);

#ifdef __cplusplus
}
#endif

#endif
