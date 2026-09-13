/**
 * @file watch_presentation.c
 * @brief WPS/PowerPoint 演示遥控 UI 与拨轮操作。
 *
 * 网络任务只更新线程安全快照；本页面的 LVGL 定时器负责读取并刷新对象。
 */
#include "watch_presentation.h"

#include <stdio.h>
#include <string.h>

#include "esp_err.h"
#include "watch_gesture.h"
#include "watch_host_link.h"

#define PRESENTATION_REFRESH_MS 100

LV_FONT_DECLARE(presentation_font_20);

typedef struct {
    lv_obj_t *page;
    lv_obj_t *connection;
    lv_obj_t *control;
    lv_obj_t *gesture;
    lv_obj_t *feedback;
    lv_timer_t *timer;
    uint32_t last_revision;
    uint32_t last_gesture_revision;
    watch_host_link_state_t last_link_state;
    bool wants_back;
    bool key2_long_handled;
    bool previous_latched;
    bool next_latched;
} presentation_ctx_t;

static presentation_ctx_t s_presentation;

static lv_obj_t *make_label(lv_obj_t *parent, const char *text, int y,
                            const lv_font_t *font, lv_color_t color)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_obj_set_width(label, 220);
    lv_obj_set_pos(label, 10, y);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(label, font, 0);
    lv_obj_set_style_text_color(label, color, 0);
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
    lv_label_set_text(label, text);
    return label;
}

static const char *connection_text(watch_host_link_state_t state)
{
    switch(state) {
    case WATCH_HOST_LINK_ONLINE: return "电脑连接  在线";
    case WATCH_HOST_LINK_NOT_CONFIGURED: return "电脑连接  未配置";
    case WATCH_HOST_LINK_WAITING_WIFI: return "电脑连接  等待网络";
    case WATCH_HOST_LINK_CONNECTING:
    case WATCH_HOST_LINK_AUTHENTICATING: return "电脑连接  连接中";
    default: return "电脑连接  已断开";
    }
}

static const char *feedback_text(const watch_presentation_snapshot_t *snapshot)
{
    if(snapshot->result == WATCH_PRESENTATION_RESULT_PENDING) return "翻页指令已发送";
    if(snapshot->result == WATCH_PRESENTATION_RESULT_PROCESSED) {
        return strcmp(snapshot->reason, "previous") == 0 ?
               "电脑已处理  上一页" : "电脑已处理  下一页";
    }
    if(snapshot->result != WATCH_PRESENTATION_RESULT_REJECTED) return "等待指令";
    if(strcmp(snapshot->reason, "disabled") == 0) return "电脑端演示控制未开启";
    if(strcmp(snapshot->reason, "watch_inactive") == 0) return "电脑未确认遥控页面";
    if(strcmp(snapshot->reason, "foreground_not_slideshow") == 0) return "请切换到 WPS 全屏放映";
    if(strcmp(snapshot->reason, "send_input_failed") == 0) return "电脑模拟按键失败";
    if(strcmp(snapshot->reason, "expired") == 0) return "指令已过期并丢弃";
    return "电脑拒绝了翻页指令";
}

static void refresh_ui(lv_timer_t *timer)
{
    (void)timer;
    if(s_presentation.page == NULL) return;

    watch_gesture_event_t event;
    while(watch_gesture_pop_event(&event)) {
        uint32_t op_id;
        esp_err_t err = watch_host_link_send_presentation_action(
            event == WATCH_GESTURE_RIGHT, &op_id);
        if(err != ESP_OK) {
            lv_label_set_text(s_presentation.feedback,
                              watch_host_link_get_state() == WATCH_HOST_LINK_ONLINE ?
                              "发送队列忙 请稍后重试" : "电脑未连接 指令已丢弃");
        }
        (void)op_id;
    }

    watch_host_link_state_t state = watch_host_link_get_state();
    watch_presentation_snapshot_t snapshot;
    watch_host_link_get_presentation_snapshot(&snapshot);
    watch_gesture_snapshot_t gesture;
    watch_gesture_get_snapshot(&gesture);

    if(state != s_presentation.last_link_state) {
        lv_label_set_text(s_presentation.connection, connection_text(state));
        lv_obj_set_style_text_color(s_presentation.connection,
                                    state == WATCH_HOST_LINK_ONLINE ?
                                    lv_color_hex(0x66E08A) : lv_color_hex(0xFFB454), 0);
        s_presentation.last_link_state = state;
    }
    if(snapshot.revision != s_presentation.last_revision) {
        lv_label_set_text(s_presentation.control,
                          snapshot.server_enabled ?
                          "演示控制  已开启" : "演示控制  已暂停");
        lv_obj_set_style_text_color(s_presentation.control,
                                    snapshot.server_enabled ?
                                    lv_color_hex(0x66E08A) : lv_color_hex(0xC8CDD8), 0);
        lv_label_set_text(s_presentation.feedback, feedback_text(&snapshot));
        s_presentation.last_revision = snapshot.revision;
    }
    if(gesture.revision != s_presentation.last_gesture_revision) {
        const char *text = "手势识别  暂未启用";
        lv_color_t color = lv_color_hex(0x8F99AA);
        if(gesture.enabled) {
            text = gesture.ready ? "手势识别  已开启" : "手势识别  等待";
            color = gesture.ready ? lv_color_hex(0x66E08A) : lv_color_hex(0xFFB454);
        }
        lv_label_set_text(s_presentation.gesture, text);
        lv_obj_set_style_text_color(s_presentation.gesture, color, 0);
        if(gesture.error != ESP_OK) lv_label_set_text(s_presentation.feedback, "BMI270 error");
        s_presentation.last_gesture_revision = gesture.revision;
    }
}

lv_obj_t *watch_presentation_create(lv_obj_t *parent)
{
    watch_presentation_destroy();
    memset(&s_presentation, 0, sizeof(s_presentation));
    s_presentation.last_link_state = (watch_host_link_state_t)-1;
    s_presentation.last_gesture_revision = UINT32_MAX;

    lv_obj_t *page = lv_obj_create(parent);
    if(page == NULL) return NULL;
    s_presentation.page = page;
    lv_obj_remove_style_all(page);
    lv_obj_set_size(page, 240, 240);
    lv_obj_set_style_bg_color(page, lv_color_hex(0x0B1020), 0);
    lv_obj_set_style_bg_opa(page, LV_OPA_COVER, 0);
    lv_obj_clear_flag(page, LV_OBJ_FLAG_SCROLLABLE);

    make_label(page, "演示遥控", 13, &presentation_font_20, lv_color_white());
    lv_obj_t *line = lv_obj_create(page);
    lv_obj_remove_style_all(line);
    lv_obj_set_size(line, 190, 2);
    lv_obj_set_pos(line, 25, 43);
    lv_obj_set_style_bg_color(line, lv_color_hex(0x4C7DFF), 0);
    lv_obj_set_style_bg_opa(line, LV_OPA_COVER, 0);

    s_presentation.connection = make_label(page, "", 55, &presentation_font_20,
                                           lv_color_hex(0xFFB454));
    s_presentation.control = make_label(page, "演示控制  已暂停", 83,
                                        &presentation_font_20, lv_color_hex(0xC8CDD8));
    s_presentation.gesture = make_label(page, "手势识别  暂未启用", 111,
                                        &presentation_font_20, lv_color_hex(0x8F99AA));

    lv_obj_t *status_line = lv_obj_create(page);
    lv_obj_remove_style_all(status_line);
    lv_obj_set_size(status_line, 150, 1);
    lv_obj_set_pos(status_line, 45, 141);
    lv_obj_set_style_bg_color(status_line, lv_color_hex(0x30405F), 0);
    lv_obj_set_style_bg_opa(status_line, LV_OPA_COVER, 0);

    make_label(page, "上拨上一页 下拨下一页", 151,
               &presentation_font_20, lv_color_hex(0x83B6FF));
    s_presentation.feedback = make_label(page, "等待指令", 181,
                                         &presentation_font_20, lv_color_white());
    make_label(page, "短按手势说明 长按退出", 202,
               &presentation_font_20, lv_color_hex(0x8F99AA));

    watch_host_link_set_presentation_active(true);
    s_presentation.timer = lv_timer_create(refresh_ui, PRESENTATION_REFRESH_MS, NULL);
    refresh_ui(NULL);
    return page;
}

void watch_presentation_reset(void)
{
    watch_gesture_stop();
    s_presentation.wants_back = false;
    s_presentation.key2_long_handled = false;
    watch_host_link_set_presentation_active(true);
    refresh_ui(NULL);
}

void watch_presentation_on_key(watch_key_t key)
{
    if(s_presentation.page == NULL) return;

    if(key == WATCH_KEY_1_RELEASE) {
        s_presentation.previous_latched = false;
        return;
    }
    if(key == WATCH_KEY_3_RELEASE) {
        s_presentation.next_latched = false;
        return;
    }

    if(key == WATCH_KEY_1 || key == WATCH_KEY_3) {
        bool *latched = key == WATCH_KEY_1 ?
                        &s_presentation.previous_latched : &s_presentation.next_latched;
        if(*latched) return;
        *latched = true;
        uint32_t op_id;
        bool next_page = key == WATCH_KEY_3;
        esp_err_t err = watch_host_link_send_presentation_action(next_page, &op_id);
        if(err != ESP_OK) {
            lv_label_set_text(s_presentation.feedback,
                              watch_host_link_get_state() == WATCH_HOST_LINK_ONLINE ?
                              "发送队列忙 请稍后重试" : "电脑未连接 指令已丢弃");
        }
        (void)op_id;
        return;
    }

    if(key == WATCH_KEY_2_LONG) {
        s_presentation.key2_long_handled = true;
        s_presentation.wants_back = true;
        watch_gesture_stop();
        watch_host_link_set_presentation_active(false);
    } else if(key == WATCH_KEY_2_RELEASE) {
        if(!s_presentation.key2_long_handled) {
            watch_gesture_snapshot_t gesture;
            watch_gesture_get_snapshot(&gesture);
            esp_err_t err = watch_gesture_set_enabled(!gesture.enabled);
            if(err != ESP_OK) lv_label_set_text(s_presentation.feedback, "BMI270 error");
        }
        s_presentation.key2_long_handled = false;
    }
}

bool watch_presentation_wants_back(void)
{
    return s_presentation.wants_back;
}

void watch_presentation_destroy(void)
{
    watch_gesture_stop();
    watch_host_link_set_presentation_active(false);
    if(s_presentation.timer != NULL) {
        lv_timer_delete(s_presentation.timer);
        s_presentation.timer = NULL;
    }
    if(s_presentation.page != NULL) {
        lv_obj_del(s_presentation.page);
    }
    memset(&s_presentation, 0, sizeof(s_presentation));
}
