#ifndef WATCH_BILI_STATS_H
#define WATCH_BILI_STATS_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @file watch_bili_stats.h
 * @brief Bilibili 数据拉取、缓存和格式化接口。
 */

/**
 * @brief Bilibili 数据统计结果。
 */
typedef struct {
    uint64_t views;        /**< 播放量。 */
    uint64_t likes;        /**< 点赞量。 */
    uint64_t subscribers;  /**< 粉丝数。 */
    bool valid;            /**< 缓存数据是否有效。 */
} watch_bili_stats_t;

/**
 * @brief Bilibili 数据更新回调。
 *
 * @param stats 最新统计数据。
 * @param user_data 用户自定义上下文。
 */
typedef void (*watch_bili_stats_cb_t)(const watch_bili_stats_t *stats, void *user_data);

/**
 * @brief 启动 Bilibili 数据刷新任务。
 *
 * @param cb 数据更新回调，可为 NULL。
 * @param user_data 传递给回调的用户数据。
 */
void watch_bili_stats_start(watch_bili_stats_cb_t cb, void *user_data);

/**
 * @brief 获取最近一次缓存的 Bilibili 数据。
 *
 * @param stats 缓存数据输出指针。
 * @return esp_err_t ESP_OK 表示缓存有效。
 */
esp_err_t watch_bili_stats_get_cached(watch_bili_stats_t *stats);

/**
 * @brief 将大数字格式化为适合表盘显示的短字符串。
 *
 * @param value 原始数值。
 * @param buf 输出缓冲区。
 * @param buf_size 输出缓冲区大小。
 */
void watch_bili_stats_format_short(uint64_t value, char *buf, size_t buf_size);

#ifdef __cplusplus
}
#endif

#endif /* WATCH_BILI_STATS_H */
