/**
 * @file watch_host_link.c
 * @brief 基于换行分隔 JSON 的上位机 TCP 长连接。
 *
 * 网络任务只维护连接状态，不直接操作 LVGL。后续专注和演示页面通过公共接口/队列接入。
 */
#include "watch_host_link.h"

#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include "cJSON.h"
#include "esp_log.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/inet.h"
#include "lwip/netdb.h"

#include "watch_config.h"
#include "watch_device_info.h"
#include "watch_wifi.h"

#define HOST_LINK_TASK_STACK       6144
#define HOST_LINK_TASK_PRIORITY    3
#define HOST_LINK_FIRST_DELAY_MS   6000
#define HOST_LINK_RETRY_MS         3000
#define HOST_LINK_CONNECT_MS       5000
#define HOST_LINK_HEARTBEAT_MS     5000
#define HOST_LINK_DEAD_MS          15000
#define HOST_LINK_MAX_LINE         1024
#define HOST_LINK_PROTOCOL_VERSION 1

static const char *TAG = "watch_host_link";
static volatile watch_host_link_state_t s_state = WATCH_HOST_LINK_NOT_CONFIGURED;
static bool s_started;

static void set_state(watch_host_link_state_t state)
{
    if(s_state != state) {
        ESP_LOGI(TAG, "state: %s", watch_host_link_state_text(state));
        s_state = state;
    }
}

watch_host_link_state_t watch_host_link_get_state(void)
{
    return s_state;
}

const char *watch_host_link_state_text(watch_host_link_state_t state)
{
    switch(state) {
    case WATCH_HOST_LINK_NOT_CONFIGURED: return "not configured";
    case WATCH_HOST_LINK_WAITING_WIFI: return "waiting wifi";
    case WATCH_HOST_LINK_CONNECTING: return "connecting";
    case WATCH_HOST_LINK_AUTHENTICATING: return "authenticating";
    case WATCH_HOST_LINK_ONLINE: return "online";
    case WATCH_HOST_LINK_DISCONNECTED: return "disconnected";
    default: return "unknown";
    }
}

static bool socket_send_all(int sock, const char *data, size_t size)
{
    size_t sent = 0;
    while(sent < size) {
        int n = send(sock, data + sent, size - sent, 0);
        if(n > 0) {
            sent += (size_t)n;
            continue;
        }
        if(n < 0 && errno == EINTR) {
            continue;
        }
        return false;
    }
    return true;
}

static bool socket_send_json(int sock, cJSON *root)
{
    char *json = cJSON_PrintUnformatted(root);
    if(json == NULL) {
        return false;
    }
    bool ok = socket_send_all(sock, json, strlen(json)) && socket_send_all(sock, "\n", 1);
    cJSON_free(json);
    return ok;
}

static int connect_with_timeout(const char *address, uint16_t port)
{
    char port_text[8];
    snprintf(port_text, sizeof(port_text), "%u", (unsigned)port);

    struct addrinfo hints = {
        .ai_family = AF_UNSPEC,
        .ai_socktype = SOCK_STREAM,
    };
    struct addrinfo *results = NULL;
    if(getaddrinfo(address, port_text, &hints, &results) != 0) {
        return -1;
    }

    int connected = -1;
    for(struct addrinfo *it = results; it != NULL && connected < 0; it = it->ai_next) {
        int sock = socket(it->ai_family, it->ai_socktype, it->ai_protocol);
        if(sock < 0) {
            continue;
        }

        int flags = fcntl(sock, F_GETFL, 0);
        (void)fcntl(sock, F_SETFL, flags | O_NONBLOCK);
        int ret = connect(sock, it->ai_addr, it->ai_addrlen);
        if(ret < 0 && errno == EINPROGRESS) {
            fd_set write_set;
            FD_ZERO(&write_set);
            FD_SET(sock, &write_set);
            struct timeval timeout = {
                .tv_sec = HOST_LINK_CONNECT_MS / 1000,
                .tv_usec = (HOST_LINK_CONNECT_MS % 1000) * 1000,
            };
            ret = select(sock + 1, NULL, &write_set, NULL, &timeout);
            if(ret > 0) {
                int error = 0;
                socklen_t error_size = sizeof(error);
                if(getsockopt(sock, SOL_SOCKET, SO_ERROR, &error, &error_size) == 0 && error == 0) {
                    ret = 0;
                } else {
                    ret = -1;
                }
            } else {
                ret = -1;
            }
        }

        if(ret == 0) {
            (void)fcntl(sock, F_SETFL, flags & ~O_NONBLOCK);
            connected = sock;
        } else {
            close(sock);
        }
    }

    freeaddrinfo(results);
    return connected;
}

static bool send_hello(int sock, const watch_config_t *cfg)
{
    watch_device_info_t info;
    if(watch_device_info_load(&info) != ESP_OK) {
        return false;
    }

    cJSON *root = cJSON_CreateObject();
    if(root == NULL) {
        return false;
    }
    cJSON_AddNumberToObject(root, "v", HOST_LINK_PROTOCOL_VERSION);
    cJSON_AddStringToObject(root, "type", "hello");
    cJSON_AddStringToObject(root, "device_id", info.device_id);
    cJSON_AddStringToObject(root, "device_name", info.device_name);
    cJSON_AddStringToObject(root, "token", cfg->pair_token);
    cJSON_AddNumberToObject(root, "nonce", esp_random());
    bool ok = socket_send_json(sock, root);
    cJSON_Delete(root);
    return ok;
}

static bool send_heartbeat(int sock, uint32_t sequence)
{
    cJSON *root = cJSON_CreateObject();
    if(root == NULL) {
        return false;
    }
    cJSON_AddNumberToObject(root, "v", HOST_LINK_PROTOCOL_VERSION);
    cJSON_AddStringToObject(root, "type", "heartbeat");
    cJSON_AddNumberToObject(root, "seq", sequence);
    bool ok = socket_send_json(sock, root);
    cJSON_Delete(root);
    return ok;
}

static bool send_pong(int sock, const cJSON *message)
{
    cJSON *root = cJSON_CreateObject();
    if(root == NULL) {
        return false;
    }
    cJSON_AddNumberToObject(root, "v", HOST_LINK_PROTOCOL_VERSION);
    cJSON_AddStringToObject(root, "type", "pong");
    const cJSON *seq = cJSON_GetObjectItem(message, "seq");
    if(cJSON_IsNumber(seq)) {
        cJSON_AddNumberToObject(root, "seq", seq->valuedouble);
    }
    bool ok = socket_send_json(sock, root);
    cJSON_Delete(root);
    return ok;
}

static bool handle_line(int sock, const char *line, bool *authenticated)
{
    cJSON *root = cJSON_Parse(line);
    if(root == NULL || !cJSON_IsObject(root)) {
        cJSON_Delete(root);
        return true; /* 丢弃单条坏消息，连接仍可继续。 */
    }

    const cJSON *version = cJSON_GetObjectItem(root, "v");
    const cJSON *type = cJSON_GetObjectItem(root, "type");
    if(!cJSON_IsNumber(version) || version->valueint != HOST_LINK_PROTOCOL_VERSION ||
       !cJSON_IsString(type) || type->valuestring == NULL) {
        cJSON_Delete(root);
        return false;
    }

    bool keep = true;
    if(strcmp(type->valuestring, "hello_ack") == 0) {
        const cJSON *ok = cJSON_GetObjectItem(root, "ok");
        *authenticated = cJSON_IsTrue(ok);
        keep = *authenticated;
    } else if(strcmp(type->valuestring, "ping") == 0) {
        keep = send_pong(sock, root);
    }

    cJSON_Delete(root);
    return keep;
}

static void run_connection(int sock, const watch_config_t *cfg)
{
    set_state(WATCH_HOST_LINK_AUTHENTICATING);
    if(!send_hello(sock, cfg)) {
        return;
    }

    bool authenticated = false;
    char line[HOST_LINK_MAX_LINE];
    size_t used = 0;
    TickType_t last_rx = xTaskGetTickCount();
    TickType_t last_tx = last_rx;
    uint32_t sequence = 1;

    while(1) {
        fd_set read_set;
        FD_ZERO(&read_set);
        FD_SET(sock, &read_set);
        struct timeval timeout = {.tv_sec = 1, .tv_usec = 0};
        int ready = select(sock + 1, &read_set, NULL, NULL, &timeout);
        if(ready < 0) {
            break;
        }

        if(ready > 0) {
            char input[256];
            int n = recv(sock, input, sizeof(input), 0);
            if(n <= 0) {
                break;
            }
            last_rx = xTaskGetTickCount();
            for(int i = 0; i < n; ++i) {
                char ch = input[i];
                if(ch == '\r') {
                    continue;
                }
                if(ch == '\n') {
                    line[used] = '\0';
                    if(used > 0 && !handle_line(sock, line, &authenticated)) {
                        return;
                    }
                    used = 0;
                    if(authenticated) {
                        set_state(WATCH_HOST_LINK_ONLINE);
                    }
                    continue;
                }
                if(used + 1 >= sizeof(line)) {
                    return;
                }
                line[used++] = ch;
            }
        }

        TickType_t now = xTaskGetTickCount();
        if((now - last_tx) >= pdMS_TO_TICKS(HOST_LINK_HEARTBEAT_MS)) {
            if(!authenticated || !send_heartbeat(sock, sequence++)) {
                break;
            }
            last_tx = now;
        }
        if((now - last_rx) >= pdMS_TO_TICKS(HOST_LINK_DEAD_MS)) {
            break;
        }
    }
}

static void host_link_task(void *arg)
{
    (void)arg;

    /* 让 app_main 的单次 Wi-Fi 初始化和原有校时任务先稳定下来，避免启动阶段争用网络资源。 */
    vTaskDelay(pdMS_TO_TICKS(HOST_LINK_FIRST_DELAY_MS));

    while(1) {
        watch_config_t cfg;
        if(watch_config_load(&cfg) != ESP_OK || !watch_config_has_host(&cfg)) {
            set_state(WATCH_HOST_LINK_NOT_CONFIGURED);
            vTaskDelay(pdMS_TO_TICKS(HOST_LINK_RETRY_MS));
            continue;
        }

        set_state(WATCH_HOST_LINK_WAITING_WIFI);
        if(watch_wifi_wait_connected(pdMS_TO_TICKS(15000)) != ESP_OK) {
            set_state(WATCH_HOST_LINK_DISCONNECTED);
            vTaskDelay(pdMS_TO_TICKS(HOST_LINK_RETRY_MS));
            continue;
        }

        set_state(WATCH_HOST_LINK_CONNECTING);
        int sock = connect_with_timeout(cfg.host_addr, cfg.host_port);
        if(sock < 0) {
            set_state(WATCH_HOST_LINK_DISCONNECTED);
            vTaskDelay(pdMS_TO_TICKS(HOST_LINK_RETRY_MS));
            continue;
        }

        run_connection(sock, &cfg);
        shutdown(sock, SHUT_RDWR);
        close(sock);
        set_state(WATCH_HOST_LINK_DISCONNECTED);
        vTaskDelay(pdMS_TO_TICKS(HOST_LINK_RETRY_MS));
    }
}

esp_err_t watch_host_link_start(void)
{
    if(s_started) {
        return ESP_OK;
    }
    s_started = true;
    BaseType_t ret = xTaskCreate(host_link_task, "host_link", HOST_LINK_TASK_STACK,
                                 NULL, HOST_LINK_TASK_PRIORITY, NULL);
    if(ret != pdPASS) {
        s_started = false;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}
