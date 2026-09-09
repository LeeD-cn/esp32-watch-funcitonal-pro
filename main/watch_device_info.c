/**
 * @file watch_device_info.c
 * @brief 拥有者、设备名和只读设备 ID 的 NVS 持久化实现。
 */

#include "watch_device_info.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "esp_mac.h"
#include "nvs.h"
#include "watch_config.h"

#define WATCH_DEVICE_NAMESPACE       "device_info"
#define WATCH_DEVICE_KEY_VERSION     "version"
#define WATCH_DEVICE_KEY_OWNER       "owner"
#define WATCH_DEVICE_KEY_NAME        "name"
#define WATCH_DEVICE_KEY_ID          "device_id"
#define WATCH_DEVICE_DATA_VERSION    1
#define WATCH_DEVICE_DEFAULT_NAME    "ESP32-S3 Watch"

static bool device_info_string_valid(const char *text, size_t capacity)
{
    return text != NULL && memchr(text, '\0', capacity) != NULL;
}

static void device_info_make_id(char *out, size_t out_size)
{
    uint8_t mac[6] = {0};

    if(out == NULL || out_size == 0) {
        return;
    }

    if(esp_read_mac(mac, ESP_MAC_WIFI_STA) != ESP_OK) {
        snprintf(out, out_size, "S3-UNKNOWN");
        return;
    }

    /* MAC 来自芯片只读身份，格式化后首次写入 NVS，之后保持不变。 */
    snprintf(out, out_size,
             "S3-%02X%02X-%02X%02X-%02X%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

static esp_err_t device_info_get_string(nvs_handle_t handle,
                                        const char *key,
                                        char *out,
                                        size_t out_size,
                                        bool *found)
{
    size_t length = out_size;
    esp_err_t ret;

    if(out == NULL || out_size == 0 || found == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    out[0] = '\0';
    *found = false;
    ret = nvs_get_str(handle, key, out, &length);
    if(ret == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_OK;
    }
    if(ret != ESP_OK) {
        return ret;
    }

    out[out_size - 1] = '\0';
    *found = true;
    return ESP_OK;
}

esp_err_t watch_device_info_load(watch_device_info_t *info)
{
    if(info == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(info, 0, sizeof(*info));

    esp_err_t ret = watch_config_init();
    if(ret != ESP_OK) {
        return ret;
    }

    nvs_handle_t handle;
    ret = nvs_open(WATCH_DEVICE_NAMESPACE, NVS_READWRITE, &handle);
    if(ret != ESP_OK) {
        return ret;
    }

    bool owner_found = false;
    bool name_found = false;
    bool id_found = false;
    bool needs_commit = false;
    uint8_t stored_version = 0;

    ret = device_info_get_string(handle, WATCH_DEVICE_KEY_OWNER,
                                 info->owner, sizeof(info->owner), &owner_found);
    if(ret == ESP_OK) {
        ret = device_info_get_string(handle, WATCH_DEVICE_KEY_NAME,
                                     info->device_name, sizeof(info->device_name), &name_found);
    }
    if(ret == ESP_OK) {
        ret = device_info_get_string(handle, WATCH_DEVICE_KEY_ID,
                                     info->device_id, sizeof(info->device_id), &id_found);
    }
    if(ret == ESP_OK) {
        ret = nvs_get_u8(handle, WATCH_DEVICE_KEY_VERSION, &stored_version);
        if(ret == ESP_ERR_NVS_NOT_FOUND) {
            stored_version = 0;
            ret = ESP_OK;
        }
    }

    if(ret == ESP_OK && !name_found) {
        snprintf(info->device_name, sizeof(info->device_name), "%s", WATCH_DEVICE_DEFAULT_NAME);
    }
    if(ret == ESP_OK && (!id_found || info->device_id[0] == '\0')) {
        device_info_make_id(info->device_id, sizeof(info->device_id));
        id_found = false;
    }

    if(ret == ESP_OK && !owner_found) {
        ret = nvs_set_str(handle, WATCH_DEVICE_KEY_OWNER, info->owner);
        needs_commit = ret == ESP_OK;
    }
    if(ret == ESP_OK && !name_found) {
        ret = nvs_set_str(handle, WATCH_DEVICE_KEY_NAME, info->device_name);
        needs_commit = ret == ESP_OK;
    }
    if(ret == ESP_OK && !id_found) {
        ret = nvs_set_str(handle, WATCH_DEVICE_KEY_ID, info->device_id);
        needs_commit = ret == ESP_OK;
    }
    if(ret == ESP_OK && stored_version != WATCH_DEVICE_DATA_VERSION) {
        ret = nvs_set_u8(handle, WATCH_DEVICE_KEY_VERSION, WATCH_DEVICE_DATA_VERSION);
        needs_commit = ret == ESP_OK;
    }
    if(ret == ESP_OK && needs_commit) {
        ret = nvs_commit(handle);
    }

    nvs_close(handle);
    return ret;
}

esp_err_t watch_device_info_save(const watch_device_info_t *info)
{
    if(info == NULL ||
       !device_info_string_valid(info->owner, sizeof(info->owner)) ||
       !device_info_string_valid(info->device_name, sizeof(info->device_name))) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t ret = watch_config_init();
    if(ret != ESP_OK) {
        return ret;
    }

    nvs_handle_t handle;
    ret = nvs_open(WATCH_DEVICE_NAMESPACE, NVS_READWRITE, &handle);
    if(ret != ESP_OK) {
        return ret;
    }

    ret = nvs_set_str(handle, WATCH_DEVICE_KEY_OWNER, info->owner);
    if(ret == ESP_OK) {
        ret = nvs_set_str(handle, WATCH_DEVICE_KEY_NAME, info->device_name);
    }

    char stored_id[WATCH_DEVICE_ID_MAX] = {0};
    bool id_found = false;
    if(ret == ESP_OK) {
        ret = device_info_get_string(handle, WATCH_DEVICE_KEY_ID,
                                     stored_id, sizeof(stored_id), &id_found);
    }
    if(ret == ESP_OK && (!id_found || stored_id[0] == '\0')) {
        device_info_make_id(stored_id, sizeof(stored_id));
        ret = nvs_set_str(handle, WATCH_DEVICE_KEY_ID, stored_id);
    }
    if(ret == ESP_OK) {
        ret = nvs_set_u8(handle, WATCH_DEVICE_KEY_VERSION, WATCH_DEVICE_DATA_VERSION);
    }
    if(ret == ESP_OK) {
        ret = nvs_commit(handle);
    }

    nvs_close(handle);
    return ret;
}

esp_err_t watch_device_info_clear(void)
{
    esp_err_t ret = watch_config_init();
    if(ret != ESP_OK) {
        return ret;
    }

    nvs_handle_t handle;
    ret = nvs_open(WATCH_DEVICE_NAMESPACE, NVS_READWRITE, &handle);
    if(ret == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_OK;
    }
    if(ret != ESP_OK) {
        return ret;
    }

    /* 保留 device_id，恢复出厂后仍能识别为同一只手表。 */
    ret = nvs_erase_key(handle, WATCH_DEVICE_KEY_OWNER);
    if(ret == ESP_ERR_NVS_NOT_FOUND) {
        ret = ESP_OK;
    }
    if(ret == ESP_OK) {
        esp_err_t name_ret = nvs_erase_key(handle, WATCH_DEVICE_KEY_NAME);
        if(name_ret != ESP_OK && name_ret != ESP_ERR_NVS_NOT_FOUND) {
            ret = name_ret;
        }
    }
    if(ret == ESP_OK) {
        ret = nvs_commit(handle);
    }

    nvs_close(handle);
    return ret;
}
