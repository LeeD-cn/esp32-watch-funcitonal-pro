#ifndef TIME_SYNC_H
#define TIME_SYNC_H

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @file time_sync.h
 * @brief WiFi/SNTP 校时任务接口。
 */

/**
 * @brief 启动后台校时任务。
 *
 * @note 任务会等待 WiFi 可用后通过 SNTP 同步系统时间，并在成功后写回外部 RTC。
 */
void watch_time_sync_start(void);

#ifdef __cplusplus
}
#endif

#endif
