/**
 * @file watch_tomato_presets.h
 * @brief 番茄钟本地预设的 NVS 持久化接口。
 */
#ifndef WATCH_TOMATO_PRESETS_H
#define WATCH_TOMATO_PRESETS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define WATCH_TOMATO_PRESET_COUNT        3
#define WATCH_TOMATO_PRESET_MAX_SECONDS  (99U * 3600U + 59U * 60U + 59U)

typedef struct {
    bool valid;
    uint32_t seconds;
} watch_tomato_preset_t;

/** @brief 读取三个稳定槽位；尚未保存的槽位返回 valid=false。 */
esp_err_t watch_tomato_presets_load(
    watch_tomato_preset_t presets[WATCH_TOMATO_PRESET_COUNT]);

/** @brief 保存指定槽位，时长必须处于 1 秒到 99:59:59。 */
esp_err_t watch_tomato_preset_save(size_t slot, uint32_t seconds);

/** @brief 删除指定槽位；删除空槽也视为成功。 */
esp_err_t watch_tomato_preset_delete(size_t slot);

#ifdef __cplusplus
}
#endif

#endif
