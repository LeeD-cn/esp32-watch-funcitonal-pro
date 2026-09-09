/**
 * @file watch_device_info.h
 * @brief 手表设备信息的持久化接口。
 */
#ifndef WATCH_DEVICE_INFO_H
#define WATCH_DEVICE_INFO_H

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define WATCH_DEVICE_OWNER_MAX       33
#define WATCH_DEVICE_NAME_MAX        33
#define WATCH_DEVICE_ID_MAX          24

typedef struct {
    char owner[WATCH_DEVICE_OWNER_MAX];
    char device_name[WATCH_DEVICE_NAME_MAX];
    char device_id[WATCH_DEVICE_ID_MAX];
} watch_device_info_t;

/**
 * @brief 读取设备信息；首次读取时写入稳定的默认设备名和设备 ID。
 */
esp_err_t watch_device_info_load(watch_device_info_t *info);

/**
 * @brief 保存可编辑的拥有者和设备名；设备 ID 始终保持只读。
 */
esp_err_t watch_device_info_save(const watch_device_info_t *info);

/**
 * @brief 清除可编辑信息；下次读取时恢复默认值，设备 ID 保持稳定。
 */
esp_err_t watch_device_info_clear(void);

#ifdef __cplusplus
}
#endif

#endif
