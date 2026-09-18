/**
 * @file watch_host_link.h
 * @brief 手表到 Windows 上位机的公共 TCP 连接。
 */
#ifndef WATCH_HOST_LINK_H
#define WATCH_HOST_LINK_H

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    WATCH_HOST_LINK_NOT_CONFIGURED = 0,
    WATCH_HOST_LINK_WAITING_WIFI,
    WATCH_HOST_LINK_CONNECTING,
    WATCH_HOST_LINK_AUTHENTICATING,
    WATCH_HOST_LINK_ONLINE,
    WATCH_HOST_LINK_DISCONNECTED,
} watch_host_link_state_t;

typedef enum {
    WATCH_PRESENTATION_RESULT_NONE = 0,
    WATCH_PRESENTATION_RESULT_PENDING,
    WATCH_PRESENTATION_RESULT_PROCESSED,
    WATCH_PRESENTATION_RESULT_REJECTED,
} watch_presentation_result_t;

typedef struct {
    bool server_enabled;
    watch_presentation_result_t result;
    uint32_t op_id;
    uint32_t revision;
    char reason[48];
} watch_presentation_snapshot_t;

typedef enum {
    WATCH_FOCUS_NONE = 0,
    WATCH_FOCUS_READY,
    WATCH_FOCUS_RUNNING,
    WATCH_FOCUS_PAUSED,
    WATCH_FOCUS_COMPLETED,
    WATCH_FOCUS_ABORTED,
} watch_focus_state_t;

typedef enum {
    WATCH_FOCUS_ACTION_START = 0,
    WATCH_FOCUS_ACTION_PAUSE,
    WATCH_FOCUS_ACTION_RESUME,
    WATCH_FOCUS_ACTION_ABORT,
} watch_focus_action_t;

typedef struct {
    watch_focus_state_t state;
    uint32_t version;
    uint32_t planned_sec;
    uint32_t focused_sec;
    uint32_t remaining_sec;
    uint32_t received_tick;
    uint32_t revision;
    char task_id[48];
    char project[96];
} watch_focus_snapshot_t;

/** 启动后台连接任务；重复调用不会重复创建。 */
esp_err_t watch_host_link_start(void);

/** 获取供后续页面显示的连接状态。 */
watch_host_link_state_t watch_host_link_get_state(void);

/** 获取简短、稳定的状态文字。 */
const char *watch_host_link_state_text(watch_host_link_state_t state);

/** 告知电脑演示遥控页面是否处于活动状态；重连后会自动同步。 */
void watch_host_link_set_presentation_active(bool active);

/** 将一次翻页操作放入网络任务队列；断线时立即拒绝，不会留到重连后发送。 */
esp_err_t watch_host_link_send_presentation_action(bool next_page, uint32_t *op_id);

/** 复制电脑端演示开关及最近一次操作结果，供 LVGL 任务轮询显示。 */
void watch_host_link_get_presentation_snapshot(watch_presentation_snapshot_t *snapshot);

/** 告知电脑协同专注页面是否活动；重连时自动请求完整快照。 */
void watch_host_link_set_focus_active(bool active);

/** 请求电脑重发当前专注任务快照。 */
esp_err_t watch_host_link_request_focus_sync(void);

/** 发送专注控制请求；断线或离页时不缓存。 */
esp_err_t watch_host_link_send_focus_action(watch_focus_action_t action,
                                            uint32_t *op_id);

/** 复制电脑端专注任务快照。 */
void watch_host_link_get_focus_snapshot(watch_focus_snapshot_t *snapshot);

#ifdef __cplusplus
}
#endif

#endif
