#include "watch_focus.h"

#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "watch_host_link.h"

#define FOCUS_REFRESH_MS 100

LV_FONT_DECLARE(presentation_font_20);
LV_FONT_DECLARE(snake_font_20);
LV_FONT_DECLARE(tomato_preset_font_20);
LV_FONT_DECLARE(cn_font_26);
LV_IMG_DECLARE(focus_end_title);

typedef struct {
    lv_obj_t *page;
    lv_obj_t *connection;
    lv_obj_t *project;
    lv_obj_t *time;
    lv_obj_t *detail;
    lv_obj_t *primary;
    lv_obj_t *finish;
    lv_obj_t *selector;
    lv_obj_t *confirm;
    lv_obj_t *confirm_selector;
    lv_obj_t *confirm_cancel;
    lv_obj_t *confirm_accept;
    lv_timer_t *timer;
    uint32_t last_revision;
    watch_host_link_state_t last_link;
    watch_focus_snapshot_t snapshot;
    bool select_finish;
    bool confirm_accept_selected;
    bool confirming;
    bool wants_back;
} focus_ctx_t;

static focus_ctx_t s_focus;

static size_t utf8_decode(const char *text, uint32_t *codepoint)
{
    const uint8_t *p = (const uint8_t *)text;
    if(p[0] < 0x80) { *codepoint = p[0]; return 1; }
    size_t n = (p[0] & 0xE0) == 0xC0 ? 2 : (p[0] & 0xF0) == 0xE0 ? 3 :
               (p[0] & 0xF8) == 0xF0 ? 4 : 0;
    if(n == 0) return 0;
    uint32_t value = p[0] & (0x7FU >> n);
    for(size_t i = 1; i < n; ++i) {
        if(p[i] == 0 || (p[i] & 0xC0) != 0x80) return 0;
        value = (value << 6) | (p[i] & 0x3F);
    }
    *codepoint = value;
    return n;
}

static void project_text(char *out, size_t capacity, const char *input)
{
    size_t used = 0;
    while(*input != '\0' && used + 1 < capacity) {
        uint32_t codepoint = 0;
        size_t bytes = utf8_decode(input, &codepoint);
        lv_font_glyph_dsc_t glyph;
        bool supported = bytes != 0 && lv_font_get_glyph_dsc(
                             &presentation_font_20, &glyph, codepoint, 0);
        if(!supported) {
            out[used++] = '?';
            input += bytes != 0 ? bytes : 1;
        }
        else {
            if(used + bytes >= capacity) break;
            memcpy(out + used, input, bytes);
            used += bytes;
            input += bytes;
        }
    }
    out[used] = '\0';
}

static lv_obj_t *label(lv_obj_t *parent, const char *text, int y,
                       const lv_font_t *font, lv_color_t color)
{
    lv_obj_t *obj = lv_label_create(parent);
    lv_obj_set_width(obj, 220);
    lv_obj_set_pos(obj, 10, y);
    lv_obj_set_style_text_align(obj, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(obj, font, 0);
    lv_obj_set_style_text_color(obj, color, 0);
    lv_label_set_long_mode(obj, LV_LABEL_LONG_WRAP);
    lv_label_set_text(obj, text);
    return obj;
}

static const char *link_text(watch_host_link_state_t state)
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

static const char *state_text(watch_focus_state_t state)
{
    switch(state) {
    case WATCH_FOCUS_READY: return "Ready";
    case WATCH_FOCUS_RUNNING: return "Focusing";
    case WATCH_FOCUS_PAUSED: return "Paused";
    case WATCH_FOCUS_COMPLETED: return "Completed";
    case WATCH_FOCUS_ABORTED: return "Aborted";
    default: return "No task";
    }
}

static bool has_no_active_task(void)
{
    return s_focus.last_link == WATCH_HOST_LINK_ONLINE &&
           (s_focus.snapshot.state == WATCH_FOCUS_NONE ||
            s_focus.snapshot.state == WATCH_FOCUS_COMPLETED ||
            s_focus.snapshot.state == WATCH_FOCUS_ABORTED);
}

static void update_selector(void)
{
    if(s_focus.confirming) return;
    lv_obj_t *target = s_focus.select_finish ? s_focus.finish : s_focus.primary;
    lv_obj_update_layout(s_focus.page);
    lv_obj_set_size(s_focus.selector, lv_obj_get_width(target) + 22,
                    lv_obj_get_height(target) + 10);
    lv_obj_set_pos(s_focus.selector, lv_obj_get_x(target) - 11,
                   lv_obj_get_y(target) - 5);
    lv_obj_move_background(s_focus.selector);
}

static void update_confirm_selector(void)
{
    lv_obj_t *target = s_focus.confirm_accept_selected ?
                       s_focus.confirm_accept : s_focus.confirm_cancel;
    lv_obj_update_layout(s_focus.confirm);
    lv_obj_set_size(s_focus.confirm_selector, lv_obj_get_width(target) + 8,
                    lv_obj_get_height(target) + 8);
    lv_obj_set_pos(s_focus.confirm_selector, lv_obj_get_x(target) - 4,
                   lv_obj_get_y(target) - 4);
    lv_obj_move_background(s_focus.confirm_selector);
}

static lv_obj_t *confirm_button(lv_obj_t *parent, int x, const char *text,
                                const lv_font_t *font)
{
    lv_obj_t *button = lv_obj_create(parent);
    lv_obj_remove_style_all(button);
    lv_obj_set_size(button, 82, 38);
    lv_obj_set_pos(button, x, 66);
    lv_obj_set_style_bg_color(button, lv_color_hex(0x202A3D), 0);
    lv_obj_set_style_bg_opa(button, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(button, lv_color_hex(0x56637A), 0);
    lv_obj_set_style_border_width(button, 1, 0);
    lv_obj_set_style_radius(button, 8, 0);
    lv_obj_clear_flag(button, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *text_label = lv_label_create(button);
    lv_obj_set_style_text_font(text_label, font, 0);
    lv_obj_set_style_text_color(text_label, lv_color_white(), 0);
    lv_label_set_text(text_label, text);
    lv_obj_center(text_label);
    return button;
}

static uint32_t displayed_remaining(const watch_focus_snapshot_t *snapshot)
{
    if(snapshot->state != WATCH_FOCUS_RUNNING) return snapshot->remaining_sec;
    uint32_t elapsed = ((uint32_t)xTaskGetTickCount() - snapshot->received_tick) /
                       pdMS_TO_TICKS(1000);
    return elapsed >= snapshot->remaining_sec ? 0 : snapshot->remaining_sec - elapsed;
}

static void refresh(lv_timer_t *timer)
{
    (void)timer;
    if(s_focus.page == NULL) return;

    watch_host_link_state_t link = watch_host_link_get_state();
    watch_focus_snapshot_t snapshot;
    watch_host_link_get_focus_snapshot(&snapshot);
    if(link != s_focus.last_link) {
        lv_label_set_text(s_focus.connection, link_text(link));
        lv_obj_set_style_text_color(s_focus.connection,
            link == WATCH_HOST_LINK_ONLINE ? lv_color_hex(0x66E08A) : lv_color_hex(0xFFB454), 0);
        s_focus.last_link = link;
    }
    if(snapshot.revision != s_focus.last_revision) {
        bool was_inactive = s_focus.snapshot.state == WATCH_FOCUS_NONE ||
                            s_focus.snapshot.state == WATCH_FOCUS_COMPLETED ||
                            s_focus.snapshot.state == WATCH_FOCUS_ABORTED;
        s_focus.snapshot = snapshot;
        if(was_inactive && snapshot.state >= WATCH_FOCUS_READY &&
           snapshot.state <= WATCH_FOCUS_PAUSED) {
            s_focus.select_finish = false;
        }
        char project[sizeof(snapshot.project)];
        project_text(project, sizeof(project),
                     snapshot.project[0] ? snapshot.project : "Create a task on PC");
        lv_label_set_text(s_focus.project, project);
        s_focus.last_revision = snapshot.revision;
    }

    uint32_t remaining = displayed_remaining(&s_focus.snapshot);
    char text[48];
    snprintf(text, sizeof(text), "%02lu:%02lu",
             (unsigned long)(remaining / 60), (unsigned long)(remaining % 60));
    lv_label_set_text(s_focus.time, text);
    snprintf(text, sizeof(text), "%s  |  %lu min",
             state_text(s_focus.snapshot.state),
             (unsigned long)((s_focus.snapshot.planned_sec + 59) / 60));
    lv_label_set_text(s_focus.detail, text);

    bool online = link == WATCH_HOST_LINK_ONLINE;
    lv_obj_clear_flag(s_focus.primary, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_width(s_focus.finish, 80);
    lv_obj_set_x(s_focus.finish, 130);
    if(!online) {
        lv_obj_set_style_text_font(s_focus.primary, &presentation_font_20, 0);
        lv_label_set_text(s_focus.primary, "重试");
        lv_label_set_text(s_focus.finish, "返回");
    } else if(has_no_active_task()) {
        lv_obj_add_flag(s_focus.primary, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_x(s_focus.finish, 80);
        s_focus.select_finish = true;
        lv_label_set_text(s_focus.finish, "返回");
    } else {
        if(s_focus.snapshot.state == WATCH_FOCUS_READY) {
            lv_obj_set_style_text_font(s_focus.primary, &snake_font_20, 0);
            lv_label_set_text(s_focus.primary, "开始");
        } else {
            lv_obj_set_style_text_font(s_focus.primary, &lv_font_montserrat_26, 0);
            lv_label_set_text(s_focus.primary,
                              s_focus.snapshot.state == WATCH_FOCUS_RUNNING ?
                              LV_SYMBOL_PAUSE : LV_SYMBOL_PLAY);
        }
        lv_label_set_text(s_focus.finish, "结束");
    }
    update_selector();
}

static void show_confirmation(bool show)
{
    s_focus.confirming = show;
    if(show) {
        s_focus.confirm_accept_selected = false; /* 破坏性操作默认取消。 */
        lv_obj_add_flag(s_focus.selector, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(s_focus.confirm, LV_OBJ_FLAG_HIDDEN);
        update_confirm_selector();
    } else {
        s_focus.select_finish = false;
        lv_obj_add_flag(s_focus.confirm, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(s_focus.selector, LV_OBJ_FLAG_HIDDEN);
        update_selector();
    }
}

lv_obj_t *watch_focus_create(lv_obj_t *parent)
{
    watch_focus_destroy();
    memset(&s_focus, 0, sizeof(s_focus));
    s_focus.last_link = (watch_host_link_state_t)-1;

    s_focus.page = lv_obj_create(parent);
    lv_obj_remove_style_all(s_focus.page);
    lv_obj_set_size(s_focus.page, 240, 240);
    lv_obj_set_style_bg_color(s_focus.page, lv_color_hex(0x0B1020), 0);
    lv_obj_set_style_bg_opa(s_focus.page, LV_OPA_COVER, 0);
    lv_obj_clear_flag(s_focus.page, LV_OBJ_FLAG_SCROLLABLE);

    label(s_focus.page, "番茄钟", 6, &cn_font_26, lv_color_white());
    s_focus.connection = label(s_focus.page, "", 33, &presentation_font_20,
                               lv_color_hex(0xFFB454));
    s_focus.project = label(s_focus.page, "Create a task on PC", 62,
                            &presentation_font_20, lv_color_hex(0xC8CDD8));
    lv_obj_set_height(s_focus.project, 46);
    s_focus.time = label(s_focus.page, "00:00", 105, &lv_font_montserrat_40,
                         lv_color_white());
    s_focus.detail = label(s_focus.page, "No task", 153, &lv_font_montserrat_14,
                           lv_color_hex(0x8F99AA));

    s_focus.selector = lv_obj_create(s_focus.page);
    lv_obj_remove_style_all(s_focus.selector);
    lv_obj_set_style_bg_color(s_focus.selector, lv_color_hex(0x244A87), 0);
    lv_obj_set_style_bg_opa(s_focus.selector, LV_OPA_70, 0);
    lv_obj_set_style_border_color(s_focus.selector, lv_color_hex(0x4C7DFF), 0);
    lv_obj_set_style_border_width(s_focus.selector, 2, 0);
    lv_obj_set_style_radius(s_focus.selector, 8, 0);

    s_focus.primary = label(s_focus.page, "PC", 196, &lv_font_montserrat_20, lv_color_white());
    lv_obj_set_width(s_focus.primary, 80);
    lv_obj_set_x(s_focus.primary, 30);
    s_focus.finish = label(s_focus.page, "返回", 196, &snake_font_20, lv_color_white());
    lv_obj_set_width(s_focus.finish, 80);
    lv_obj_set_x(s_focus.finish, 130);

    s_focus.confirm = lv_obj_create(s_focus.page);
    lv_obj_remove_style_all(s_focus.confirm);
    lv_obj_set_size(s_focus.confirm, 220, 118);
    lv_obj_set_pos(s_focus.confirm, 10, 60);
    lv_obj_set_style_bg_color(s_focus.confirm, lv_color_hex(0x151C2C), 0);
    lv_obj_set_style_bg_opa(s_focus.confirm, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(s_focus.confirm, lv_color_hex(0x4C7DFF), 0);
    lv_obj_set_style_border_width(s_focus.confirm, 2, 0);
    lv_obj_set_style_radius(s_focus.confirm, 12, 0);
    lv_obj_t *confirm_title = lv_img_create(s_focus.confirm);
    lv_img_set_src(confirm_title, &focus_end_title);
    lv_obj_align(confirm_title, LV_ALIGN_TOP_MID, 0, 12);

    s_focus.confirm_selector = lv_obj_create(s_focus.confirm);
    lv_obj_remove_style_all(s_focus.confirm_selector);
    lv_obj_set_style_bg_color(s_focus.confirm_selector, lv_color_hex(0x244A87), 0);
    lv_obj_set_style_bg_opa(s_focus.confirm_selector, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(s_focus.confirm_selector, lv_color_hex(0x5B8CFF), 0);
    lv_obj_set_style_border_width(s_focus.confirm_selector, 2, 0);
    lv_obj_set_style_radius(s_focus.confirm_selector, 8, 0);
    s_focus.confirm_cancel = confirm_button(s_focus.confirm, 20, "取消",
                                            &tomato_preset_font_20);
    s_focus.confirm_accept = confirm_button(s_focus.confirm, 118, "确认",
                                            &presentation_font_20);
    lv_obj_add_flag(s_focus.confirm, LV_OBJ_FLAG_HIDDEN);

    watch_host_link_set_focus_active(true);
    s_focus.timer = lv_timer_create(refresh, FOCUS_REFRESH_MS, NULL);
    refresh(NULL);
    return s_focus.page;
}

void watch_focus_reset(void)
{
    s_focus.wants_back = false;
    s_focus.select_finish = false;
    show_confirmation(false);
    watch_host_link_set_focus_active(true);
    (void)watch_host_link_request_focus_sync();
    refresh(NULL);
}

void watch_focus_on_key(watch_key_t key)
{
    if(s_focus.page == NULL || key == WATCH_KEY_2_RELEASE) return;
    if(key == WATCH_KEY_1 || key == WATCH_KEY_3) {
        if(s_focus.confirming) {
            s_focus.confirm_accept_selected = !s_focus.confirm_accept_selected;
            update_confirm_selector();
        } else if(!has_no_active_task()) {
            s_focus.select_finish = !s_focus.select_finish;
            update_selector();
        } else {
            s_focus.select_finish = true;
        }
        return;
    }
    if(key != WATCH_KEY_2) return;

    if(s_focus.confirming) {
        if(s_focus.confirm_accept_selected) {
            (void)watch_host_link_send_focus_action(WATCH_FOCUS_ACTION_ABORT, NULL);
        }
        show_confirmation(false);
        return;
    }

    bool online = watch_host_link_get_state() == WATCH_HOST_LINK_ONLINE;
    if(!online) {
        if(s_focus.select_finish) s_focus.wants_back = true;
        else (void)watch_host_link_request_focus_sync();
        return;
    }
    if(has_no_active_task()) {
        s_focus.wants_back = true;
        return;
    }
    if(s_focus.select_finish) {
        show_confirmation(true);
        return;
    }
    watch_focus_action_t action = s_focus.snapshot.state == WATCH_FOCUS_READY ?
        WATCH_FOCUS_ACTION_START : s_focus.snapshot.state == WATCH_FOCUS_RUNNING ?
        WATCH_FOCUS_ACTION_PAUSE : WATCH_FOCUS_ACTION_RESUME;
    (void)watch_host_link_send_focus_action(action, NULL);
}

bool watch_focus_wants_back(void)
{
    return s_focus.wants_back;
}

void watch_focus_destroy(void)
{
    watch_host_link_set_focus_active(false);
    if(s_focus.timer) lv_timer_delete(s_focus.timer);
    if(s_focus.page) lv_obj_del(s_focus.page);
    memset(&s_focus, 0, sizeof(s_focus));
}
