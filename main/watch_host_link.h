/**
 * @file watch_host_link.h
 * @brief 手表到 Windows 上位机的公共 TCP 连接。
 */
#ifndef WATCH_HOST_LINK_H
#define WATCH_HOST_LINK_H

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

/** 启动后台连接任务；重复调用不会重复创建。 */
esp_err_t watch_host_link_start(void);

/** 获取供后续页面显示的连接状态。 */
watch_host_link_state_t watch_host_link_get_state(void);

/** 获取简短、稳定的状态文字。 */
const char *watch_host_link_state_text(watch_host_link_state_t state);

#ifdef __cplusplus
}
#endif

#endif
