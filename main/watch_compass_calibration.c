#include "watch_compass_calibration.h"

#include <math.h>
#include <stdint.h>
#include <string.h>

#include "nvs.h"
#include "watch_config.h"

#define WATCH_COMPASS_CAL_NAMESPACE    "compass_cal"
#define WATCH_COMPASS_CAL_KEY          "data"
#define WATCH_COMPASS_CAL_VERSION      1U

typedef struct {
    uint32_t version;
    int16_t offset_x;
    int16_t offset_y;
    int16_t offset_z;
    uint16_t reserved;
    float scale_x;
    float scale_y;
    float scale_z;
} watch_compass_calibration_data_t;

static bool watch_compass_calibration_valid(
    const watch_compass_calibration_data_t *data)
{
    if(data == NULL || data->version != WATCH_COMPASS_CAL_VERSION) {
        return false;
    }

    return isfinite(data->scale_x) && data->scale_x >= 0.1f && data->scale_x <= 10.0f &&
           isfinite(data->scale_y) && data->scale_y >= 0.1f && data->scale_y <= 10.0f &&
           isfinite(data->scale_z) && data->scale_z >= 0.1f && data->scale_z <= 10.0f;
}

esp_err_t watch_compass_calibration_load(qmc5883p_calibration_t *calibration)
{
    nvs_handle_t handle;
    watch_compass_calibration_data_t data;
    size_t size = sizeof(data);
    esp_err_t ret;

    if(calibration == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    ret = watch_config_init();
    if(ret != ESP_OK) {
        return ret;
    }

    ret = nvs_open(WATCH_COMPASS_CAL_NAMESPACE, NVS_READONLY, &handle);
    if(ret != ESP_OK) {
        return ret;
    }

    memset(&data, 0, sizeof(data));
    ret = nvs_get_blob(handle, WATCH_COMPASS_CAL_KEY, &data, &size);
    nvs_close(handle);

    if(ret != ESP_OK) {
        return ret;
    }
    if(size != sizeof(data) || !watch_compass_calibration_valid(&data)) {
        return ESP_ERR_INVALID_CRC;
    }

    calibration->offset_x = data.offset_x;
    calibration->offset_y = data.offset_y;
    calibration->offset_z = data.offset_z;
    calibration->scale_x = data.scale_x;
    calibration->scale_y = data.scale_y;
    calibration->scale_z = data.scale_z;
    return ESP_OK;
}

esp_err_t watch_compass_calibration_save(const qmc5883p_calibration_t *calibration)
{
    nvs_handle_t handle;
    watch_compass_calibration_data_t data = {
        .version = WATCH_COMPASS_CAL_VERSION,
        .offset_x = calibration != NULL ? calibration->offset_x : 0,
        .offset_y = calibration != NULL ? calibration->offset_y : 0,
        .offset_z = calibration != NULL ? calibration->offset_z : 0,
        .reserved = 0,
        .scale_x = calibration != NULL ? calibration->scale_x : 0.0f,
        .scale_y = calibration != NULL ? calibration->scale_y : 0.0f,
        .scale_z = calibration != NULL ? calibration->scale_z : 0.0f,
    };
    esp_err_t ret;

    if(calibration == NULL || !watch_compass_calibration_valid(&data)) {
        return ESP_ERR_INVALID_ARG;
    }

    ret = watch_config_init();
    if(ret != ESP_OK) {
        return ret;
    }

    ret = nvs_open(WATCH_COMPASS_CAL_NAMESPACE, NVS_READWRITE, &handle);
    if(ret != ESP_OK) {
        return ret;
    }

    ret = nvs_set_blob(handle, WATCH_COMPASS_CAL_KEY, &data, sizeof(data));
    if(ret == ESP_OK) {
        ret = nvs_commit(handle);
    }
    nvs_close(handle);
    return ret;
}
