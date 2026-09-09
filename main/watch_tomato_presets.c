/**
 * @file watch_tomato_presets.c
 * @brief 三个番茄钟倒计时预设的 NVS 实现。
 */

#include "watch_tomato_presets.h"

#include <stddef.h>
#include <string.h>

#include "nvs.h"
#include "watch_config.h"

#define TOMATO_PRESET_NAMESPACE     "tomato_presets"
#define TOMATO_PRESET_KEY_VERSION   "version"
#define TOMATO_PRESET_DATA_VERSION  1

static const char *const s_valid_keys[WATCH_TOMATO_PRESET_COUNT] = {
    "valid0", "valid1", "valid2"
};

static const char *const s_seconds_keys[WATCH_TOMATO_PRESET_COUNT] = {
    "seconds0", "seconds1", "seconds2"
};

static bool tomato_preset_seconds_valid(uint32_t seconds)
{
    return seconds > 0 && seconds <= WATCH_TOMATO_PRESET_MAX_SECONDS;
}

esp_err_t watch_tomato_presets_load(
    watch_tomato_preset_t presets[WATCH_TOMATO_PRESET_COUNT])
{
    if(presets == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(presets, 0,
           sizeof(watch_tomato_preset_t) * WATCH_TOMATO_PRESET_COUNT);

    esp_err_t ret = watch_config_init();
    if(ret != ESP_OK) {
        return ret;
    }

    nvs_handle_t handle;
    ret = nvs_open(TOMATO_PRESET_NAMESPACE, NVS_READONLY, &handle);
    if(ret == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_OK;
    }
    if(ret != ESP_OK) {
        return ret;
    }

    uint8_t version = 0;
    ret = nvs_get_u8(handle, TOMATO_PRESET_KEY_VERSION, &version);
    if(ret == ESP_ERR_NVS_NOT_FOUND) {
        ret = ESP_OK;
    }

    if(ret == ESP_OK && version == TOMATO_PRESET_DATA_VERSION) {
        for(size_t i = 0; i < WATCH_TOMATO_PRESET_COUNT; ++i) {
            uint8_t valid = 0;
            uint32_t seconds = 0;

            esp_err_t valid_ret = nvs_get_u8(handle, s_valid_keys[i], &valid);
            esp_err_t seconds_ret = nvs_get_u32(handle, s_seconds_keys[i], &seconds);

            if(valid_ret != ESP_OK && valid_ret != ESP_ERR_NVS_NOT_FOUND) {
                ret = valid_ret;
                break;
            }
            if(seconds_ret != ESP_OK && seconds_ret != ESP_ERR_NVS_NOT_FOUND) {
                ret = seconds_ret;
                break;
            }

            /* 无效或不完整的槽位按空槽处理，不把损坏数据用于计时。 */
            presets[i].valid = valid == 1 &&
                               seconds_ret == ESP_OK &&
                               tomato_preset_seconds_valid(seconds);
            presets[i].seconds = presets[i].valid ? seconds : 0;
        }
    }

    nvs_close(handle);
    return ret;
}

esp_err_t watch_tomato_preset_save(size_t slot, uint32_t seconds)
{
    if(slot >= WATCH_TOMATO_PRESET_COUNT ||
       !tomato_preset_seconds_valid(seconds)) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t ret = watch_config_init();
    if(ret != ESP_OK) {
        return ret;
    }

    nvs_handle_t handle;
    ret = nvs_open(TOMATO_PRESET_NAMESPACE, NVS_READWRITE, &handle);
    if(ret != ESP_OK) {
        return ret;
    }

    ret = nvs_set_u32(handle, s_seconds_keys[slot], seconds);
    if(ret == ESP_OK) {
        ret = nvs_set_u8(handle, s_valid_keys[slot], 1);
    }
    if(ret == ESP_OK) {
        ret = nvs_set_u8(handle, TOMATO_PRESET_KEY_VERSION,
                         TOMATO_PRESET_DATA_VERSION);
    }
    if(ret == ESP_OK) {
        ret = nvs_commit(handle);
    }

    nvs_close(handle);
    return ret;
}

esp_err_t watch_tomato_preset_delete(size_t slot)
{
    if(slot >= WATCH_TOMATO_PRESET_COUNT) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t ret = watch_config_init();
    if(ret != ESP_OK) {
        return ret;
    }

    nvs_handle_t handle;
    ret = nvs_open(TOMATO_PRESET_NAMESPACE, NVS_READWRITE, &handle);
    if(ret != ESP_OK) {
        return ret;
    }

    /* valid 先置零；即使旧 seconds 仍存在，也不会被当成有效预设。 */
    ret = nvs_set_u8(handle, s_valid_keys[slot], 0);
    if(ret == ESP_OK) {
        esp_err_t erase_ret = nvs_erase_key(handle, s_seconds_keys[slot]);
        if(erase_ret != ESP_OK && erase_ret != ESP_ERR_NVS_NOT_FOUND) {
            ret = erase_ret;
        }
    }
    if(ret == ESP_OK) {
        ret = nvs_set_u8(handle, TOMATO_PRESET_KEY_VERSION,
                         TOMATO_PRESET_DATA_VERSION);
    }
    if(ret == ESP_OK) {
        ret = nvs_commit(handle);
    }

    nvs_close(handle);
    return ret;
}
