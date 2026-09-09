#ifndef WATCH_COMPASS_CALIBRATION_H
#define WATCH_COMPASS_CALIBRATION_H

#include "esp_err.h"
#include "qmc5883p.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 从 NVS 读取磁力计三轴偏置与比例。未保存时返回 ESP_ERR_NVS_NOT_FOUND。 */
esp_err_t watch_compass_calibration_load(qmc5883p_calibration_t *calibration);

/** 保存磁力计三轴偏置与比例。地磁偏角由页面配置，不写入本数据。 */
esp_err_t watch_compass_calibration_save(const qmc5883p_calibration_t *calibration);

#ifdef __cplusplus
}
#endif

#endif
