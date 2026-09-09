/**
 * @file watch_tomato_clock.c
 * @brief 番茄钟页面、时间编辑滚轮和倒计时状态机。
 */


/**
 * @section 模块说明
 * 本文件实现番茄钟页面，核心是“焦点导航 + 时间字段编辑 + 计时器状态机”。
 * 用户通过按键在返回、时间设置、正/倒计时、播放/暂停、重置以及 HH/MM/SS 字段之间切换。
 *
 * 状态机重点：
 * - time_set_mode：是否进入时间设置大模式；
 * - field_edit_mode：是否正在编辑某个具体字段；
 * - count_up_mode：true 为正计时，false 为倒计时；
 * - timer_active/timer_running/timer_finished：分别表示计时器已启动、正在运行、已经结束；
 * - wheel_animing/pending_delta：滚轮动画期间暂存连续按键，避免动画与数值错位。
 */

#include "watch_tomato_clock.h"
#include "watch_language.h"
#include "watch_tomato_presets.h"
#include <string.h>
#include <stdio.h>
#include <stdint.h>

LV_FONT_DECLARE(cn_font_26);
LV_FONT_DECLARE(tomato_preset_font_20);

/* 以下宏大多是 UI 坐标、尺寸或任务参数。
 * 修改这类值时建议同时检查：
 * 1. 240x240 屏幕边界是否越界；
 * 2. 选择框/动画目标是否仍然对齐；
 * 3. FreeRTOS 任务栈是否足够容纳 JSON/HTTP/LVGL 临时对象。
 */
#define WATCH_SCREEN_W              240
#define WATCH_SCREEN_H              240

#define TOMATO_COUNTDOWN_Y          102

#define TOMATO_SELECTOR_PAD_X       7
#define TOMATO_SELECTOR_PAD_Y       4
#define TOMATO_SELECTOR_RADIUS      8
#define TOMATO_SELECTOR_BORDER_W    2
#define TOMATO_SELECTOR_ANIM_MS     180

#define TOMATO_FIELD_BG_PAD_X       7
#define TOMATO_FIELD_BG_PAD_Y       1

#define TOMATO_WHEEL_ITEM_H         42
#define TOMATO_WHEEL_VISIBLE_COUNT  3

#define TOMATO_WHEEL_ANIM_MS        120
#define TOMATO_WHEEL_RADIUS         8
#define TOMATO_WHEEL_BORDER_W       1

#define TOMATO_COUNTDOWN_FRAME_W        220
#define TOMATO_COUNTDOWN_FRAME_H        146
#define TOMATO_COUNTDOWN_FRAME_RADIUS   12
#define TOMATO_COUNTDOWN_FRAME_BORDER_W 5

#define TOMATO_COUNTDOWN_FRAME_X        ((WATCH_SCREEN_W - TOMATO_COUNTDOWN_FRAME_W) / 2)
#define TOMATO_COUNTDOWN_FRAME_Y        50

#define TOMATO_TOP_Y                    8

#define TOMATO_BOTTOM_PAD               8

#define TOMATO_DIRECTION_X              42
#define TOMATO_DIRECTION_Y              8

#define TOMATO_RESET_X                  28
#define TOMATO_RESET_Y                  8

#define TOMATO_TIMER_PERIOD_MS          1000
#define TOMATO_BLINK_HALF_PERIOD_MS     500

#define TOMATO_PRESET_LINE_COUNT        5
#define TOMATO_PRESET_TITLE_Y           10
#define TOMATO_PRESET_LINE_Y            48
#define TOMATO_PRESET_LINE_STEP         36

/**
 * @brief 番茄钟页面焦点枚举。
 *
 * 普通模式下焦点在返回/时间设置/方向/播放/重置间移动；
 * 时间设置模式下焦点会进入 HH/MM/SS 字段。
 */
typedef enum {
    TOMATO_FOCUS_BACK = 0,
    TOMATO_FOCUS_TIME_SET,
    TOMATO_FOCUS_PRESETS,
    TOMATO_FOCUS_DIRECTION,
    TOMATO_FOCUS_PLAY_PAUSE,
    TOMATO_FOCUS_RESET,
    TOMATO_FOCUS_HH,
    TOMATO_FOCUS_MM,
    TOMATO_FOCUS_SS,
} tomato_focus_t;

typedef enum {
    TOMATO_PRESET_VIEW_CLOSED = 0,
    TOMATO_PRESET_VIEW_LIST,
    TOMATO_PRESET_VIEW_ACTION,
    TOMATO_PRESET_VIEW_EDIT,
    TOMATO_PRESET_VIEW_DELETE_CONFIRM,
    TOMATO_PRESET_VIEW_NOTICE,
} tomato_preset_view_t;

/**
 * @brief 番茄钟页面上下文。
 *
 * 这里把 UI 对象、焦点、编辑模式、滚轮动画和计时器运行状态集中管理，
 * 避免多个全局变量分散造成状态不同步。
 */
typedef struct {
    lv_obj_t *page;
    lv_obj_t *back_btn;

    lv_obj_t *time_set_bg;
    lv_obj_t *time_set_label;
    lv_obj_t *presets_label;

    lv_obj_t *direction_label;
    lv_obj_t *play_label;
    lv_obj_t *reset_bg;
    lv_obj_t *reset_label;

    lv_obj_t *countdown_frame;
    lv_obj_t *countdown_hh;
    lv_obj_t *countdown_colon_1;
    lv_obj_t *countdown_mm;
    lv_obj_t *countdown_colon_2;
    lv_obj_t *countdown_ss;

    /* 时间字段编辑背景，跟随 HH/MM/SS 当前字段移动。 */
    lv_obj_t *field_bg;

    /* HH/MM/SS 复用同一个数值滚轮。 */
    lv_obj_t *wheel;
    lv_obj_t *wheel_strip;
    lv_obj_t *wheel_item[5];

    char wheel_text[5][4];

    lv_obj_t *cursor;
    lv_timer_t *timer;

    lv_obj_t *preset_panel;
    lv_obj_t *preset_title;
    lv_obj_t *preset_lines[TOMATO_PRESET_LINE_COUNT];
    lv_obj_t *preset_cursor;

    tomato_focus_t focus;
    tomato_focus_t edit_focus;

    bool wants_back;
    bool time_set_mode;
    bool field_edit_mode;
    bool wheel_animing;
    int pending_delta;

    bool count_up_mode;
    bool timer_active;
    bool timer_running;
    bool timer_finished;
    bool reset_pressed;

    bool language_initialized;
    bool language_chinese;

    int hour;
    int min;
    int sec;
    int edit_temp;

    int timer_set_total;
    int timer_current;

    tomato_preset_view_t preset_view;
    tomato_preset_view_t preset_notice_return_view;
    int preset_focus;
    int preset_selected_slot;
    bool preset_field_edit;
    bool preset_is_new;
    int preset_hour;
    int preset_min;
    int preset_sec;
    watch_tomato_preset_t presets[WATCH_TOMATO_PRESET_COUNT];
} tomato_clock_ctx_t;

/* 单例页面状态：该页面同一时间只会存在一个实例。 */
static tomato_clock_ctx_t s_tomato;

static void tomato_cursor_x_anim_cb(void *var, int32_t v)
{
    /* 选择框动画回调：更新 X 坐标。
     */
    lv_obj_set_x((lv_obj_t *)var, v);
}

static void tomato_cursor_y_anim_cb(void *var, int32_t v)
{
    /* 选择框动画回调：更新 Y 坐标。
     */
    lv_obj_set_y((lv_obj_t *)var, v);
}

static void tomato_cursor_w_anim_cb(void *var, int32_t v)
{
    /* 选择框动画回调：更新宽度。
     */
    lv_obj_set_width((lv_obj_t *)var, v);
}

static void tomato_cursor_h_anim_cb(void *var, int32_t v)
{
    /* 选择框动画回调：更新高度。
     */
    lv_obj_set_height((lv_obj_t *)var, v);
}

static void tomato_frame_opa_anim_cb(void *var, int32_t v)
{
    /* 倒计时结束闪烁动画回调，通过透明度变化提示计时完成。
     */
    lv_obj_set_style_opa((lv_obj_t *)var, (lv_opa_t)v, 0);
}

static lv_obj_t *tomato_obj_from_focus(tomato_focus_t focus)
{
    switch(focus) {
    case TOMATO_FOCUS_BACK:
        return s_tomato.back_btn;

    case TOMATO_FOCUS_TIME_SET:
        return s_tomato.time_set_label;

    case TOMATO_FOCUS_PRESETS:
        return s_tomato.presets_label;

    case TOMATO_FOCUS_DIRECTION:
        return s_tomato.direction_label;

    case TOMATO_FOCUS_PLAY_PAUSE:
        return s_tomato.play_label;

    case TOMATO_FOCUS_RESET:
        return s_tomato.reset_label;

    case TOMATO_FOCUS_HH:
        return s_tomato.countdown_hh;

    case TOMATO_FOCUS_MM:
        return s_tomato.countdown_mm;

    case TOMATO_FOCUS_SS:
        return s_tomato.countdown_ss;

    default:
        return NULL;
    }
}

static bool tomato_focus_is_time_field(tomato_focus_t focus)
{
    /* 判断焦点是否落在 HH/MM/SS 字段，用于进入字段编辑模式。
     */
    return focus == TOMATO_FOCUS_HH ||
           focus == TOMATO_FOCUS_MM ||
           focus == TOMATO_FOCUS_SS;
}

static int tomato_field_max(tomato_focus_t focus)
{
    /* 返回指定字段最大值：小时可到 99，分钟和秒为 59。
     */
    if(focus == TOMATO_FOCUS_HH) {
        return 99;
    }

    return 59;
}

static int tomato_field_wrap(tomato_focus_t focus, int value)
{
    /* 字段数值循环处理，超过最大值回到 0，小于 0 回到最大值。
     */
    int max = tomato_field_max(focus);

    if(value < 0) {
        return max;
    }

    if(value > max) {
        return 0;
    }

    return value;
}

static int tomato_field_current_value(tomato_focus_t focus)
{
    /* 读取当前焦点字段对应的 hour/min/sec 值。
     */
    if(focus == TOMATO_FOCUS_HH) {
        return s_tomato.hour;
    }

    if(focus == TOMATO_FOCUS_MM) {
        return s_tomato.min;
    }

    return s_tomato.sec;
}

static int tomato_time_to_total_seconds(int hour, int min, int sec)
{
    /* 把 HH:MM:SS 转成总秒数，计时器内部统一以秒为单位运算。
     */
    return hour * 3600 + min * 60 + sec;
}

static void tomato_total_seconds_to_hms(int total, int *hour, int *min, int *sec)
{
    /* 把总秒数拆回 HH/MM/SS，用于计时过程中刷新显示。
     */
    if(total < 0) {
        total = 0;
    }

    *hour = total / 3600;
    total %= 3600;
    *min = total / 60;
    *sec = total % 60;
}

static void tomato_label_style_26(lv_obj_t *label)
{
    /* 统一番茄钟页面 26 号文本样式。
     */
    lv_obj_set_style_text_font(label, &lv_font_montserrat_26, 0);
    lv_obj_set_style_text_color(label, lv_color_white(), 0);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_clear_flag(label, LV_OBJ_FLAG_CLICKABLE);
}

static void tomato_label_style_20(lv_obj_t *label)
{
    lv_obj_set_style_text_font(label, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(label, lv_color_white(), 0);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_clear_flag(label, LV_OBJ_FLAG_CLICKABLE);
}

static void tomato_cursor_start_anim(lv_obj_t *obj,
                                     lv_anim_exec_xcb_t exec_cb,
                                     int32_t from,
                                     int32_t to)
{
    /* 启动焦点选择框动画，所有坐标和尺寸变化复用此函数。
     */
    lv_anim_t a;

    lv_anim_init(&a);
    lv_anim_set_var(&a, obj);
    lv_anim_set_values(&a, from, to);
    lv_anim_set_duration(&a, TOMATO_SELECTOR_ANIM_MS);
    lv_anim_set_exec_cb(&a, exec_cb);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_start(&a);
}

/**
 * @brief 根据当前焦点更新选择框位置和大小。
 */
static void tomato_cursor_update(bool anim)
{
    /* 根据当前焦点更新选择框的位置和大小，保证焦点反馈跟随控件实际尺寸。
     */
    if(s_tomato.cursor == NULL || s_tomato.back_btn == NULL) {
        return;
    }

    lv_obj_t *target = tomato_obj_from_focus(s_tomato.focus);

    if(target == NULL) {
        return;
    }

    lv_obj_update_layout(s_tomato.page);

    lv_coord_t x = lv_obj_get_x(target) - TOMATO_SELECTOR_PAD_X;
    lv_coord_t y = lv_obj_get_y(target) - TOMATO_SELECTOR_PAD_Y;
    lv_coord_t w = lv_obj_get_width(target) + TOMATO_SELECTOR_PAD_X * 2;
    lv_coord_t h = lv_obj_get_height(target) + TOMATO_SELECTOR_PAD_Y * 2;

    if(w < 16 + TOMATO_SELECTOR_PAD_X * 2) {
        w = 16 + TOMATO_SELECTOR_PAD_X * 2;
    }

    if(!anim || lv_obj_get_width(s_tomato.cursor) <= 0 || lv_obj_get_height(s_tomato.cursor) <= 0) {
        lv_obj_set_pos(s_tomato.cursor, x, y);
        lv_obj_set_size(s_tomato.cursor, w, h);
    }
    else {
        tomato_cursor_start_anim(s_tomato.cursor, tomato_cursor_x_anim_cb, lv_obj_get_x(s_tomato.cursor), x);
        tomato_cursor_start_anim(s_tomato.cursor, tomato_cursor_y_anim_cb, lv_obj_get_y(s_tomato.cursor), y);
        tomato_cursor_start_anim(s_tomato.cursor, tomato_cursor_w_anim_cb, lv_obj_get_width(s_tomato.cursor), w);
        tomato_cursor_start_anim(s_tomato.cursor, tomato_cursor_h_anim_cb, lv_obj_get_height(s_tomato.cursor), h);
    }

    lv_obj_move_foreground(s_tomato.cursor);
}

static void tomato_time_set_bg_update(void)
{
    /* 刷新“时间设置”背景块，进入设置模式时显示，退出时隐藏。
     */
    if(s_tomato.time_set_bg == NULL || s_tomato.time_set_label == NULL) {
        return;
    }

    lv_obj_update_layout(s_tomato.page);

    lv_coord_t label_x = lv_obj_get_x(s_tomato.time_set_label);
    lv_coord_t label_y = lv_obj_get_y(s_tomato.time_set_label);
    lv_coord_t label_w = lv_obj_get_width(s_tomato.time_set_label);
    lv_coord_t label_h = lv_obj_get_height(s_tomato.time_set_label);

    lv_obj_set_size(s_tomato.time_set_bg, label_w + 18, label_h + 8);
    lv_obj_set_pos(s_tomato.time_set_bg, label_x - 9, label_y - 4);

    if(s_tomato.time_set_mode) {
        lv_obj_clear_flag(s_tomato.time_set_bg, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_tomato.time_set_bg, LV_OBJ_FLAG_HIDDEN);
    }
}

static void tomato_reset_bg_update(void)
{
    /* 刷新重置按钮背景，用于给短暂按下状态提供视觉反馈。
     */
    if(s_tomato.reset_bg == NULL || s_tomato.reset_label == NULL) {
        return;
    }

    lv_obj_update_layout(s_tomato.page);

    lv_coord_t label_x = lv_obj_get_x(s_tomato.reset_label);
    lv_coord_t label_y = lv_obj_get_y(s_tomato.reset_label);
    lv_coord_t label_w = lv_obj_get_width(s_tomato.reset_label);
    lv_coord_t label_h = lv_obj_get_height(s_tomato.reset_label);

    lv_obj_set_size(s_tomato.reset_bg, label_w + 18, label_h + 8);
    lv_obj_set_pos(s_tomato.reset_bg, label_x - 9, label_y - 4);

    if(s_tomato.reset_pressed) {
        lv_obj_clear_flag(s_tomato.reset_bg, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_tomato.reset_bg, LV_OBJ_FLAG_HIDDEN);
    }
}

/**
 * @brief 根据当前系统语言刷新番茄钟文本。
 *
 * @param force true 表示强制刷新，false 表示仅在语言变化时刷新。
 */
static void tomato_apply_language(bool force)
{
    bool chinese = watch_language_is_chinese();

    if(!force &&
       s_tomato.language_initialized &&
       s_tomato.language_chinese == chinese) {
        return;
    }

    s_tomato.language_initialized = true;
    s_tomato.language_chinese = chinese;

    if(s_tomato.time_set_label != NULL) {
        lv_obj_set_style_text_font(s_tomato.time_set_label,
                                   chinese ? &tomato_preset_font_20 : &lv_font_montserrat_20,
                                   0);
        lv_label_set_text(s_tomato.time_set_label,
                          chinese ? "时间设置" : "Time Set");
        lv_obj_set_pos(s_tomato.time_set_label, 48, TOMATO_TOP_Y + 2);
    }

    if(s_tomato.presets_label != NULL) {
        lv_obj_set_style_text_font(s_tomato.presets_label,
                                   chinese ? &tomato_preset_font_20 : &lv_font_montserrat_20,
                                   0);
        lv_label_set_text(s_tomato.presets_label,
                          chinese ? "保存的时钟" : "Saved");
        lv_obj_align(s_tomato.presets_label,
                     LV_ALIGN_TOP_RIGHT,
                     -10,
                     TOMATO_TOP_Y + 2);
    }

    if(s_tomato.reset_label != NULL) {
        lv_obj_set_style_text_font(s_tomato.reset_label,
                                   chinese ? &cn_font_26 : &lv_font_montserrat_26,
                                   0);
        lv_label_set_text(s_tomato.reset_label,
                          chinese ? "清零" : "RST");
        lv_obj_align(s_tomato.reset_label,
                     LV_ALIGN_BOTTOM_RIGHT,
                     -TOMATO_RESET_X,
                     -TOMATO_RESET_Y);
    }

    tomato_time_set_bg_update();
    tomato_reset_bg_update();

    if(s_tomato.cursor != NULL) {
        tomato_cursor_update(false);
    }
}

static void tomato_direction_label_update(void)
{
    /* 根据正/倒计时模式刷新加号或减号图标。
     */
    if(s_tomato.direction_label == NULL) {
        return;
    }

    lv_label_set_text(s_tomato.direction_label,
                      s_tomato.count_up_mode ? LV_SYMBOL_PLUS : LV_SYMBOL_MINUS);

    lv_obj_align(s_tomato.direction_label,
             LV_ALIGN_BOTTOM_LEFT,
             TOMATO_DIRECTION_X,
             -TOMATO_DIRECTION_Y);
}

static void tomato_play_label_update(void)
{
    /* 根据运行状态刷新播放或暂停图标。
     */
    if(s_tomato.play_label == NULL) {
        return;
    }

    lv_label_set_text(s_tomato.play_label,
                      s_tomato.timer_running ? LV_SYMBOL_PAUSE : LV_SYMBOL_PLAY);

    lv_obj_align(s_tomato.play_label,
                 LV_ALIGN_BOTTOM_MID,
                 0,
                 -TOMATO_BOTTOM_PAD);
}

static void tomato_countdown_display_time(int hour, int min, int sec)
{
    /* 把小时、分钟、秒分别写入 5 个标签，并重新居中布局整串时间。
     */
    char buf[4];

    snprintf(buf, sizeof(buf), "%02d", hour);
    lv_label_set_text(s_tomato.countdown_hh, buf);

    lv_label_set_text(s_tomato.countdown_colon_1, ":");

    snprintf(buf, sizeof(buf), "%02d", min);
    lv_label_set_text(s_tomato.countdown_mm, buf);

    lv_label_set_text(s_tomato.countdown_colon_2, ":");

    snprintf(buf, sizeof(buf), "%02d", sec);
    lv_label_set_text(s_tomato.countdown_ss, buf);

    lv_obj_update_layout(s_tomato.page);

    lv_coord_t hh_w = lv_obj_get_width(s_tomato.countdown_hh);
    lv_coord_t c1_w = lv_obj_get_width(s_tomato.countdown_colon_1);
    lv_coord_t mm_w = lv_obj_get_width(s_tomato.countdown_mm);
    lv_coord_t c2_w = lv_obj_get_width(s_tomato.countdown_colon_2);
    lv_coord_t ss_w = lv_obj_get_width(s_tomato.countdown_ss);

    lv_coord_t total_w = hh_w + c1_w + mm_w + c2_w + ss_w;
    lv_coord_t x = (WATCH_SCREEN_W - total_w) / 2;
    lv_coord_t y = TOMATO_COUNTDOWN_Y;

    lv_obj_set_pos(s_tomato.countdown_hh, x, y);
    x += hh_w;

    lv_obj_set_pos(s_tomato.countdown_colon_1, x, y);
    x += c1_w;

    lv_obj_set_pos(s_tomato.countdown_mm, x, y);
    x += mm_w;

    lv_obj_set_pos(s_tomato.countdown_colon_2, x, y);
    x += c2_w;

    lv_obj_set_pos(s_tomato.countdown_ss, x, y);
}

/**
 * @brief 将总秒数格式化并显示为 HH:MM:SS。
 */
static void tomato_countdown_display_total(int total)
{
    /* 总秒数显示入口，先拆成 HH/MM/SS 再复用显示函数。
     */
    int hour;
    int min;
    int sec;

    tomato_total_seconds_to_hms(total, &hour, &min, &sec);
    tomato_countdown_display_time(hour, min, sec);
}

static void tomato_countdown_set_time(int hour, int min, int sec)
{
    /* 直接设置编辑时间并刷新显示，常用于初始化或确认字段编辑。
     */
    s_tomato.hour = hour;
    s_tomato.min = min;
    s_tomato.sec = sec;

    tomato_countdown_display_time(s_tomato.hour, s_tomato.min, s_tomato.sec);
}

static void tomato_stop_finish_blink(void)
{
    /* 停止完成闪烁动画并恢复边框不透明。
     */
    if(s_tomato.countdown_frame == NULL) {
        return;
    }

    lv_anim_del(s_tomato.countdown_frame, tomato_frame_opa_anim_cb);
    lv_obj_set_style_opa(s_tomato.countdown_frame, LV_OPA_COVER, 0);
}

static void tomato_start_finish_blink(void)
{
    /* 启动计时完成闪烁动画，循环改变倒计时框透明度。
     */
    if(s_tomato.countdown_frame == NULL) {
        return;
    }

    tomato_stop_finish_blink();

    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, s_tomato.countdown_frame);
    lv_anim_set_values(&a, LV_OPA_COVER, LV_OPA_TRANSP);
    lv_anim_set_time(&a, TOMATO_BLINK_HALF_PERIOD_MS);
    lv_anim_set_playback_time(&a, TOMATO_BLINK_HALF_PERIOD_MS);
    lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_exec_cb(&a, tomato_frame_opa_anim_cb);
    lv_anim_set_path_cb(&a, lv_anim_path_linear);
    lv_anim_start(&a);
}

static void tomato_timer_finish(void)
{
    /* 计时结束处理：暂停定时器、更新状态、刷新播放图标，并启动完成闪烁。
     */
    if(s_tomato.timer) {
        lv_timer_pause(s_tomato.timer);
    }

    s_tomato.timer_active = false;
    s_tomato.timer_running = false;
    s_tomato.timer_finished = true;

    tomato_play_label_update();

    if(s_tomato.focus == TOMATO_FOCUS_PLAY_PAUSE) {
        tomato_cursor_update(false);
    }

    tomato_start_finish_blink();
}

static void tomato_timer_reset_action(void)
{
    /* 按当前设定时间重置计时器，清除完成状态并恢复显示。
     */
    if(s_tomato.timer) {
        lv_timer_pause(s_tomato.timer);
    }

    s_tomato.timer_set_total = tomato_time_to_total_seconds(s_tomato.hour,
                                                            s_tomato.min,
                                                            s_tomato.sec);
    s_tomato.timer_current = s_tomato.timer_set_total;
    s_tomato.timer_active = false;
    s_tomato.timer_running = false;
    s_tomato.timer_finished = false;

    tomato_stop_finish_blink();
    tomato_play_label_update();
    tomato_countdown_display_total(s_tomato.timer_set_total);

    if(s_tomato.focus == TOMATO_FOCUS_PLAY_PAUSE) {
        tomato_cursor_update(false);
    }
}

static void tomato_timer_clear_action(void)
{
    /* 清空计时器和编辑时间，通常用于重置后的第二级清除动作。
     */
    if(s_tomato.timer) {
        lv_timer_pause(s_tomato.timer);
    }

    s_tomato.hour = 0;
    s_tomato.min = 0;
    s_tomato.sec = 0;
    s_tomato.edit_temp = 0;
    s_tomato.timer_set_total = 0;
    s_tomato.timer_current = 0;
    s_tomato.timer_active = false;
    s_tomato.timer_running = false;
    s_tomato.timer_finished = false;

    tomato_stop_finish_blink();
    tomato_play_label_update();
    tomato_countdown_set_time(0, 0, 0);


    if(s_tomato.focus == TOMATO_FOCUS_PLAY_PAUSE ||
       s_tomato.focus == TOMATO_FOCUS_RESET) {
        tomato_cursor_update(false);
    }
}

static void tomato_timer_cb(lv_timer_t *timer)
{
    /* 每秒触发一次的计时回调，根据正计时或倒计时模式更新 timer_current。
     */
    (void)timer;

    if(!s_tomato.timer_running) {
        return;
    }

    if(s_tomato.count_up_mode) {
        if(s_tomato.timer_current < s_tomato.timer_set_total) {
            s_tomato.timer_current++;
        }

        tomato_countdown_display_total(s_tomato.timer_current);

        if(s_tomato.timer_current >= s_tomato.timer_set_total) {
            tomato_timer_finish();
        }
    } else {
        if(s_tomato.timer_current > 0) {
            s_tomato.timer_current--;
        }

        tomato_countdown_display_total(s_tomato.timer_current);

        if(s_tomato.timer_current <= 0) {
            tomato_timer_finish();
        }
    }
}

/**
 * @brief 在启动、暂停和继续计时之间切换。
 */
static void tomato_timer_toggle_play_pause(void)
{
    /* 播放/暂停按钮处理入口，负责首次启动、继续、暂停和结束态重新开始。
     */
    if(s_tomato.timer_running) {
        s_tomato.timer_running = false;

        if(s_tomato.timer) {
            lv_timer_pause(s_tomato.timer);
        }

        tomato_play_label_update();
        tomato_cursor_update(false);
        return;
    }

    if(s_tomato.timer_finished) {
        tomato_timer_reset_action();
    }

    if(!s_tomato.timer_active) {
        s_tomato.timer_set_total = tomato_time_to_total_seconds(s_tomato.hour,
                                                                s_tomato.min,
                                                                s_tomato.sec);
        s_tomato.timer_current = s_tomato.count_up_mode ? 0 : s_tomato.timer_set_total;
        tomato_countdown_display_total(s_tomato.timer_current);
    }

    s_tomato.timer_active = true;
    s_tomato.timer_running = true;
    s_tomato.timer_finished = false;

    tomato_stop_finish_blink();
    tomato_play_label_update();
    tomato_cursor_update(false);

    if(s_tomato.timer_set_total <= 0) {
        tomato_timer_finish();
        return;
    }

    if(s_tomato.timer) {
        lv_timer_reset(s_tomato.timer);
        lv_timer_resume(s_tomato.timer);
    }
}

static void tomato_direction_toggle(void)
{
    /* 切换正计时/倒计时模式。运行中不允许切换，避免计时语义突变。
     */
    s_tomato.count_up_mode = !s_tomato.count_up_mode;

    tomato_direction_label_update();
    tomato_timer_reset_action();

    if(s_tomato.focus == TOMATO_FOCUS_DIRECTION) {
        tomato_cursor_update(false);
    }
}

static const lv_font_t *tomato_preset_font(void)
{
    return s_tomato.language_chinese
               ? &tomato_preset_font_20
               : &lv_font_montserrat_20;
}

static void tomato_preset_format_time(uint32_t seconds, char *out, size_t out_size)
{
    unsigned hour = seconds / 3600U;
    unsigned min = (seconds % 3600U) / 60U;
    unsigned sec = seconds % 60U;

    snprintf(out, out_size, "%02u:%02u:%02u", hour, min, sec);
}

static void tomato_preset_hide_all_lines(void)
{
    for(int i = 0; i < TOMATO_PRESET_LINE_COUNT; ++i) {
        lv_obj_add_flag(s_tomato.preset_lines[i], LV_OBJ_FLAG_HIDDEN);
    }
}

static void tomato_preset_set_line(int index, const char *text)
{
    if(index < 0 || index >= TOMATO_PRESET_LINE_COUNT) {
        return;
    }

    lv_obj_set_style_text_font(s_tomato.preset_lines[index],
                               tomato_preset_font(), 0);
    lv_label_set_text(s_tomato.preset_lines[index], text);
    lv_obj_align(s_tomato.preset_lines[index], LV_ALIGN_TOP_MID, 0,
                 TOMATO_PRESET_LINE_Y + index * TOMATO_PRESET_LINE_STEP);
    lv_obj_clear_flag(s_tomato.preset_lines[index], LV_OBJ_FLAG_HIDDEN);
}

static void tomato_preset_cursor_update(void)
{
    if(s_tomato.preset_cursor == NULL ||
       s_tomato.preset_focus < 0 ||
       s_tomato.preset_focus >= TOMATO_PRESET_LINE_COUNT) {
        return;
    }

    lv_obj_t *target = s_tomato.preset_lines[s_tomato.preset_focus];
    if(lv_obj_has_flag(target, LV_OBJ_FLAG_HIDDEN)) {
        lv_obj_add_flag(s_tomato.preset_cursor, LV_OBJ_FLAG_HIDDEN);
        return;
    }

    lv_obj_update_layout(s_tomato.preset_panel);
    lv_obj_set_pos(s_tomato.preset_cursor,
                   lv_obj_get_x(target) - TOMATO_SELECTOR_PAD_X,
                   lv_obj_get_y(target) - TOMATO_SELECTOR_PAD_Y);
    lv_obj_set_size(s_tomato.preset_cursor,
                    lv_obj_get_width(target) + TOMATO_SELECTOR_PAD_X * 2,
                    lv_obj_get_height(target) + TOMATO_SELECTOR_PAD_Y * 2);
    lv_obj_clear_flag(s_tomato.preset_cursor, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_tomato.preset_cursor);
}

static void tomato_preset_set_title(const char *text)
{
    lv_obj_set_style_text_font(s_tomato.preset_title, tomato_preset_font(), 0);
    lv_label_set_text(s_tomato.preset_title, text);
    lv_obj_align(s_tomato.preset_title, LV_ALIGN_TOP_MID, 0,
                 TOMATO_PRESET_TITLE_Y);
}

static void tomato_preset_render_list(void)
{
    char line[32];
    char time_text[16];

    s_tomato.preset_view = TOMATO_PRESET_VIEW_LIST;
    s_tomato.preset_field_edit = false;
    if(s_tomato.preset_focus < 0 || s_tomato.preset_focus > 3) {
        s_tomato.preset_focus = 0;
    }

    tomato_preset_set_title(s_tomato.language_chinese ? "保存的时钟" : "Saved Timers");
    tomato_preset_hide_all_lines();
    tomato_preset_set_line(0, s_tomato.language_chinese ? "< 返回" : "< Back");

    for(int i = 0; i < WATCH_TOMATO_PRESET_COUNT; ++i) {
        if(s_tomato.presets[i].valid) {
            tomato_preset_format_time(s_tomato.presets[i].seconds,
                                      time_text, sizeof(time_text));
            snprintf(line, sizeof(line), "%d  %s", i + 1, time_text);
        } else {
            snprintf(line, sizeof(line), s_tomato.language_chinese
                     ? "%d  空槽 (新增)" : "%d  Empty (Add)", i + 1);
        }
        tomato_preset_set_line(i + 1, line);
    }

    tomato_preset_cursor_update();
}

static void tomato_preset_render_action(void)
{
    char title[40];
    char time_text[16];
    int slot = s_tomato.preset_selected_slot;

    tomato_preset_format_time(s_tomato.presets[slot].seconds,
                              time_text, sizeof(time_text));
    snprintf(title, sizeof(title), s_tomato.language_chinese
             ? "时钟 %d  %s" : "Timer %d  %s", slot + 1, time_text);

    s_tomato.preset_view = TOMATO_PRESET_VIEW_ACTION;
    s_tomato.preset_focus = 0;
    s_tomato.preset_field_edit = false;
    tomato_preset_set_title(title);
    tomato_preset_hide_all_lines();
    tomato_preset_set_line(0, s_tomato.language_chinese ? "启动" : "Start");
    tomato_preset_set_line(1, s_tomato.language_chinese ? "修改" : "Edit");
    tomato_preset_set_line(2, s_tomato.language_chinese ? "删除" : "Delete");
    tomato_preset_set_line(3, s_tomato.language_chinese ? "返回" : "Back");
    tomato_preset_cursor_update();
}

static void tomato_preset_render_edit(void)
{
    char title[24];
    char line[24];

    snprintf(title, sizeof(title), s_tomato.language_chinese
             ? "%s时钟 %d" : "%s Timer %d",
             s_tomato.preset_is_new
                 ? (s_tomato.language_chinese ? "新增" : "New")
                 : (s_tomato.language_chinese ? "修改" : "Edit"),
             s_tomato.preset_selected_slot + 1);

    s_tomato.preset_view = TOMATO_PRESET_VIEW_EDIT;
    tomato_preset_set_title(title);
    tomato_preset_hide_all_lines();
    tomato_preset_set_line(0, s_tomato.language_chinese ? "< 返回" : "< Back");

    snprintf(line, sizeof(line), s_tomato.language_chinese
             ? "小时  %s%02d%s" : "Hour  %s%02d%s",
             s_tomato.preset_field_edit && s_tomato.preset_focus == 1 ? "[" : "",
             s_tomato.preset_hour,
             s_tomato.preset_field_edit && s_tomato.preset_focus == 1 ? "]" : "");
    tomato_preset_set_line(1, line);
    snprintf(line, sizeof(line), s_tomato.language_chinese
             ? "分  %s%02d%s" : "Min  %s%02d%s",
             s_tomato.preset_field_edit && s_tomato.preset_focus == 2 ? "[" : "",
             s_tomato.preset_min,
             s_tomato.preset_field_edit && s_tomato.preset_focus == 2 ? "]" : "");
    tomato_preset_set_line(2, line);
    snprintf(line, sizeof(line), s_tomato.language_chinese
             ? "秒  %s%02d%s" : "Sec  %s%02d%s",
             s_tomato.preset_field_edit && s_tomato.preset_focus == 3 ? "[" : "",
             s_tomato.preset_sec,
             s_tomato.preset_field_edit && s_tomato.preset_focus == 3 ? "]" : "");
    tomato_preset_set_line(3, line);
    tomato_preset_set_line(4, s_tomato.language_chinese ? "保存" : "Save");
    tomato_preset_cursor_update();
}

static void tomato_preset_render_delete_confirm(void)
{
    char title[24];

    snprintf(title, sizeof(title), s_tomato.language_chinese
             ? "删除时钟 %d?" : "Delete Timer %d?",
             s_tomato.preset_selected_slot + 1);
    s_tomato.preset_view = TOMATO_PRESET_VIEW_DELETE_CONFIRM;
    s_tomato.preset_focus = 0; /* 删除等破坏性操作默认选中取消。 */
    tomato_preset_set_title(title);
    tomato_preset_hide_all_lines();
    tomato_preset_set_line(0, s_tomato.language_chinese ? "取消" : "Cancel");
    tomato_preset_set_line(1, s_tomato.language_chinese ? "删除" : "Delete");
    tomato_preset_cursor_update();
}

static void tomato_preset_show_notice(const char *chinese,
                                      const char *english,
                                      tomato_preset_view_t return_view)
{
    s_tomato.preset_view = TOMATO_PRESET_VIEW_NOTICE;
    s_tomato.preset_notice_return_view = return_view;
    s_tomato.preset_focus = 0;
    tomato_preset_set_title(s_tomato.language_chinese ? chinese : english);
    tomato_preset_hide_all_lines();
    tomato_preset_set_line(0, s_tomato.language_chinese ? "确定" : "OK");
    tomato_preset_cursor_update();
}

static void tomato_preset_close(void)
{
    s_tomato.preset_view = TOMATO_PRESET_VIEW_CLOSED;
    s_tomato.preset_field_edit = false;
    lv_obj_add_flag(s_tomato.preset_panel, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(s_tomato.cursor, LV_OBJ_FLAG_HIDDEN);
    tomato_cursor_update(false);
}

static void tomato_preset_open(void)
{
    esp_err_t ret = watch_tomato_presets_load(s_tomato.presets);

    s_tomato.preset_focus = 0;
    lv_obj_add_flag(s_tomato.cursor, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(s_tomato.preset_panel, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_tomato.preset_panel);
    tomato_preset_render_list();

    if(ret != ESP_OK) {
        tomato_preset_show_notice("读取失败", "Read failed",
                                  TOMATO_PRESET_VIEW_LIST);
    }
}

static void tomato_preset_begin_edit(bool is_new)
{
    int slot = s_tomato.preset_selected_slot;
    uint32_t seconds = is_new ? 0 : s_tomato.presets[slot].seconds;

    s_tomato.preset_is_new = is_new;
    s_tomato.preset_hour = (int)(seconds / 3600U);
    s_tomato.preset_min = (int)((seconds % 3600U) / 60U);
    s_tomato.preset_sec = (int)(seconds % 60U);
    s_tomato.preset_focus = 1;
    s_tomato.preset_field_edit = false;
    tomato_preset_render_edit();
}

static void tomato_preset_start_selected(void)
{
    uint32_t seconds = s_tomato.presets[s_tomato.preset_selected_slot].seconds;

    if(s_tomato.timer_active) {
        tomato_preset_show_notice("请先清零", "Reset timer first",
                                  TOMATO_PRESET_VIEW_ACTION);
        return;
    }

    tomato_stop_finish_blink();
    s_tomato.count_up_mode = false;
    tomato_total_seconds_to_hms((int)seconds,
                                &s_tomato.hour,
                                &s_tomato.min,
                                &s_tomato.sec);
    s_tomato.timer_set_total = (int)seconds;
    s_tomato.timer_current = (int)seconds;
    s_tomato.timer_active = true;
    s_tomato.timer_running = true;
    s_tomato.timer_finished = false;
    s_tomato.focus = TOMATO_FOCUS_PLAY_PAUSE;
    tomato_direction_label_update();
    tomato_play_label_update();
    tomato_countdown_display_total(s_tomato.timer_current);

    if(s_tomato.timer) {
        lv_timer_reset(s_tomato.timer);
        lv_timer_resume(s_tomato.timer);
    }

    tomato_preset_close();
}

static int tomato_preset_focus_count(void)
{
    switch(s_tomato.preset_view) {
    case TOMATO_PRESET_VIEW_LIST:
    case TOMATO_PRESET_VIEW_ACTION:
        return 4;
    case TOMATO_PRESET_VIEW_EDIT:
        return 5;
    case TOMATO_PRESET_VIEW_DELETE_CONFIRM:
        return 2;
    case TOMATO_PRESET_VIEW_NOTICE:
        return 1;
    default:
        return 0;
    }
}

static void tomato_preset_return_from_notice(void)
{
    if(s_tomato.preset_notice_return_view == TOMATO_PRESET_VIEW_ACTION) {
        tomato_preset_render_action();
    } else if(s_tomato.preset_notice_return_view == TOMATO_PRESET_VIEW_EDIT) {
        tomato_preset_render_edit();
    } else {
        s_tomato.preset_focus = 0;
        tomato_preset_render_list();
    }
}

static void tomato_preset_on_key(watch_key_t key)
{
    if(key == WATCH_KEY_2_RELEASE) {
        return;
    }

    if(s_tomato.preset_view == TOMATO_PRESET_VIEW_EDIT &&
       s_tomato.preset_field_edit) {
        int *value = s_tomato.preset_focus == 1 ? &s_tomato.preset_hour
                     : s_tomato.preset_focus == 2 ? &s_tomato.preset_min
                                                   : &s_tomato.preset_sec;
        int max = s_tomato.preset_focus == 1 ? 99 : 59;

        if(key == WATCH_KEY_1) {
            *value = *value <= 0 ? max : *value - 1;
            tomato_preset_render_edit();
        } else if(key == WATCH_KEY_3) {
            *value = *value >= max ? 0 : *value + 1;
            tomato_preset_render_edit();
        } else if(key == WATCH_KEY_2) {
            s_tomato.preset_field_edit = false;
            tomato_preset_render_edit();
        }
        return;
    }

    if(key == WATCH_KEY_1 || key == WATCH_KEY_3) {
        int count = tomato_preset_focus_count();
        int delta = key == WATCH_KEY_1 ? -1 : 1;
        if(count > 0) {
            s_tomato.preset_focus =
                (s_tomato.preset_focus + delta + count) % count;
            tomato_preset_cursor_update();
        }
        return;
    }

    if(key != WATCH_KEY_2) {
        return;
    }

    if(s_tomato.preset_view == TOMATO_PRESET_VIEW_NOTICE) {
        tomato_preset_return_from_notice();
        return;
    }

    if(s_tomato.preset_view == TOMATO_PRESET_VIEW_LIST) {
        if(s_tomato.preset_focus == 0) {
            tomato_preset_close();
            return;
        }

        s_tomato.preset_selected_slot = s_tomato.preset_focus - 1;
        if(s_tomato.presets[s_tomato.preset_selected_slot].valid) {
            tomato_preset_render_action();
        } else {
            tomato_preset_begin_edit(true);
        }
        return;
    }

    if(s_tomato.preset_view == TOMATO_PRESET_VIEW_ACTION) {
        if(s_tomato.preset_focus == 0) {
            tomato_preset_start_selected();
        } else if(s_tomato.preset_focus == 1) {
            tomato_preset_begin_edit(false);
        } else if(s_tomato.preset_focus == 2) {
            tomato_preset_render_delete_confirm();
        } else {
            s_tomato.preset_focus = s_tomato.preset_selected_slot + 1;
            tomato_preset_render_list();
        }
        return;
    }

    if(s_tomato.preset_view == TOMATO_PRESET_VIEW_EDIT) {
        if(s_tomato.preset_focus == 0) {
            s_tomato.preset_focus = s_tomato.preset_selected_slot + 1;
            tomato_preset_render_list();
        } else if(s_tomato.preset_focus >= 1 && s_tomato.preset_focus <= 3) {
            s_tomato.preset_field_edit = true;
            tomato_preset_render_edit();
        } else {
            uint32_t seconds = (uint32_t)tomato_time_to_total_seconds(
                s_tomato.preset_hour, s_tomato.preset_min, s_tomato.preset_sec);
            if(seconds == 0) {
                tomato_preset_show_notice("时间不能为零", "Time cannot be zero",
                                          TOMATO_PRESET_VIEW_EDIT);
            } else if(watch_tomato_preset_save(
                          (size_t)s_tomato.preset_selected_slot, seconds) != ESP_OK) {
                tomato_preset_show_notice("写入失败", "Save failed",
                                          TOMATO_PRESET_VIEW_EDIT);
            } else {
                s_tomato.presets[s_tomato.preset_selected_slot].valid = true;
                s_tomato.presets[s_tomato.preset_selected_slot].seconds = seconds;
                s_tomato.preset_focus = s_tomato.preset_selected_slot + 1;
                tomato_preset_render_list();
            }
        }
        return;
    }

    if(s_tomato.preset_view == TOMATO_PRESET_VIEW_DELETE_CONFIRM) {
        if(s_tomato.preset_focus == 0) {
            tomato_preset_render_action();
        } else if(watch_tomato_preset_delete(
                      (size_t)s_tomato.preset_selected_slot) != ESP_OK) {
            tomato_preset_show_notice("写入失败", "Delete failed",
                                      TOMATO_PRESET_VIEW_ACTION);
        } else {
            s_tomato.presets[s_tomato.preset_selected_slot].valid = false;
            s_tomato.presets[s_tomato.preset_selected_slot].seconds = 0;
            s_tomato.preset_focus = s_tomato.preset_selected_slot + 1;
            tomato_preset_render_list();
        }
    }
}

static void tomato_field_bg_update(void)
{
    /* 编辑 HH/MM/SS 时移动字段背景，突出当前正在修改的字段。
     */
    if(s_tomato.field_bg == NULL) {
        return;
    }

    lv_obj_t *target = tomato_obj_from_focus(s_tomato.edit_focus);

    if(target == NULL || !s_tomato.field_edit_mode) {
        lv_obj_add_flag(s_tomato.field_bg, LV_OBJ_FLAG_HIDDEN);
        return;
    }

    lv_obj_update_layout(s_tomato.page);

    lv_coord_t x = lv_obj_get_x(target);
    lv_coord_t y = lv_obj_get_y(target);
    lv_coord_t w = lv_obj_get_width(target);
    lv_coord_t h = lv_obj_get_height(target);

    lv_obj_set_size(s_tomato.field_bg,
                    w + TOMATO_FIELD_BG_PAD_X * 2,
                    h + TOMATO_FIELD_BG_PAD_Y * 2);

    lv_obj_set_pos(s_tomato.field_bg,
                   x - TOMATO_FIELD_BG_PAD_X,
                   y - TOMATO_FIELD_BG_PAD_Y);

    lv_obj_clear_flag(s_tomato.field_bg, LV_OBJ_FLAG_HIDDEN);
}

static void tomato_wheel_set_label(int index, tomato_focus_t focus, int value)
{
    /* 设置滚轮某一行的显示文本，围绕当前值生成上下邻近值。
     */
    if(index < 0 || index >= 5) {
        return;
    }

    if(s_tomato.wheel_item[index] == NULL) {
        return;
    }

    snprintf(s_tomato.wheel_text[index],
             sizeof(s_tomato.wheel_text[index]),
             "%02d",
             tomato_field_wrap(focus, value));

    lv_label_set_text_static(s_tomato.wheel_item[index],
                             s_tomato.wheel_text[index]);
}

static void tomato_wheel_y_anim_cb(void *var, int32_t v)
{
    /* 滚轮动画回调：移动滚轮条 Y 坐标。
     */
    lv_obj_set_y((lv_obj_t *)var, v);
}

/**
 * @brief 让字段滚轮按指定方向滚动一格。
 */
static void tomato_wheel_step(int delta);
/**
 * @brief 刷新时间字段滚轮的位置、尺寸和候选数字。
 */
static void tomato_wheel_update(void);

static void tomato_wheel_anim_done_cb(lv_anim_t *a)
{
    /* 滚轮动画完成后处理 pending_delta，保证连续按键不会丢失。
     */
    int delta = (int)(intptr_t)a->user_data;

    s_tomato.edit_temp = tomato_field_wrap(s_tomato.edit_focus,
                                           s_tomato.edit_temp + delta);

    /* 滚动完成后刷新 5 个候选数字并回到初始偏移。 */
    tomato_wheel_update();

    s_tomato.wheel_animing = false;

    if(s_tomato.pending_delta != 0) {
        int next_delta = s_tomato.pending_delta;
        s_tomato.pending_delta = 0;
        tomato_wheel_step(next_delta);
    }
}

static void tomato_wheel_update(void)
{
    /* 根据当前编辑字段和临时值刷新滚轮 5 行文本。
     */
    if(s_tomato.wheel == NULL || s_tomato.wheel_strip == NULL) {
        return;
    }

    lv_obj_t *target = tomato_obj_from_focus(s_tomato.edit_focus);

    if(target == NULL) {
        return;
    }

    lv_obj_update_layout(s_tomato.page);

    lv_coord_t target_x = lv_obj_get_x(target);
    lv_coord_t target_y = lv_obj_get_y(target);
    lv_coord_t target_w = lv_obj_get_width(target);
    lv_coord_t target_h = lv_obj_get_height(target);

    lv_coord_t wheel_w = target_w + TOMATO_FIELD_BG_PAD_X * 2;
    lv_coord_t wheel_h = TOMATO_WHEEL_ITEM_H * TOMATO_WHEEL_VISIBLE_COUNT;

    lv_obj_set_size(s_tomato.wheel, wheel_w, wheel_h);

    lv_obj_set_pos(s_tomato.wheel,
                   target_x - TOMATO_FIELD_BG_PAD_X,
                   target_y + target_h / 2 - wheel_h / 2);

    /* strip 保存当前值前后各 2 个候选项，默认只显示中间 3 项。 */
    lv_obj_set_size(s_tomato.wheel_strip,
                    wheel_w,
                    TOMATO_WHEEL_ITEM_H * 5);

    lv_obj_set_pos(s_tomato.wheel_strip, 0, -TOMATO_WHEEL_ITEM_H);

    for(int i = 0; i < 5; i++) {
        lv_obj_set_size(s_tomato.wheel_item[i],
                        wheel_w,
                        TOMATO_WHEEL_ITEM_H);

        lv_obj_set_pos(s_tomato.wheel_item[i],
                       0,
                       TOMATO_WHEEL_ITEM_H * i);

        tomato_wheel_set_label(i,
                               s_tomato.edit_focus,
                               s_tomato.edit_temp + i - 2);
    }

    lv_obj_set_style_text_opa(s_tomato.wheel_item[0], LV_OPA_50, 0);
    lv_obj_set_style_text_opa(s_tomato.wheel_item[1], LV_OPA_50, 0);
    lv_obj_set_style_text_opa(s_tomato.wheel_item[2], LV_OPA_COVER, 0);
    lv_obj_set_style_text_opa(s_tomato.wheel_item[3], LV_OPA_50, 0);
    lv_obj_set_style_text_opa(s_tomato.wheel_item[4], LV_OPA_50, 0);
}

static void tomato_wheel_show(void)
{
    /* 显示滚轮并把其移动到前景层。
     */
    lv_obj_t *target = tomato_obj_from_focus(s_tomato.edit_focus);

    if(target == NULL) {
        return;
    }

    s_tomato.edit_temp = tomato_field_current_value(s_tomato.edit_focus);

    tomato_wheel_update();

    lv_obj_set_style_text_opa(target, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(s_tomato.wheel, LV_OBJ_FLAG_HIDDEN);
}

static void tomato_wheel_hide(void)
{
    /* 隐藏滚轮，退出字段编辑时调用。
     */
    if(s_tomato.wheel) {
        lv_obj_add_flag(s_tomato.wheel, LV_OBJ_FLAG_HIDDEN);
    }

    if(s_tomato.countdown_hh) {
        lv_obj_clear_flag(s_tomato.countdown_hh, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_style_text_opa(s_tomato.countdown_hh, LV_OPA_COVER, 0);
    }

    if(s_tomato.countdown_mm) {
        lv_obj_clear_flag(s_tomato.countdown_mm, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_style_text_opa(s_tomato.countdown_mm, LV_OPA_COVER, 0);
    }

    if(s_tomato.countdown_ss) {
        lv_obj_clear_flag(s_tomato.countdown_ss, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_style_text_opa(s_tomato.countdown_ss, LV_OPA_COVER, 0);
    }
}

static void tomato_wheel_step(int delta)
{
    /* 按键驱动滚轮增加或减少一个单位，带循环和动画处理。
     */
    if(delta == 0) {
        return;
    }

    if(s_tomato.wheel_animing) {
        s_tomato.pending_delta = delta;
        return;
    }

    s_tomato.wheel_animing = true;

    /* 通过移动 strip 实现向上/向下滚动，结束后再刷新候选项。 */
    lv_coord_t from_y = -TOMATO_WHEEL_ITEM_H;
    lv_coord_t to_y;

    if(delta < 0) {
        to_y = 0;
    } else {
        to_y = -TOMATO_WHEEL_ITEM_H * 2;
    }

    lv_obj_set_y(s_tomato.wheel_strip, from_y);

    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, s_tomato.wheel_strip);
    lv_anim_set_values(&a, from_y, to_y);
    lv_anim_set_duration(&a, TOMATO_WHEEL_ANIM_MS);
    lv_anim_set_exec_cb(&a, tomato_wheel_y_anim_cb);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_set_completed_cb(&a, tomato_wheel_anim_done_cb);
    lv_anim_set_user_data(&a, (void *)(intptr_t)delta);
    lv_anim_start(&a);
}

static void tomato_exit_time_set_mode(bool anim)
{
    /* 退出时间设置大模式，隐藏字段背景和滚轮，并恢复普通焦点。
     */
    s_tomato.time_set_mode = false;
    s_tomato.focus = TOMATO_FOCUS_TIME_SET;

    tomato_time_set_bg_update();
    tomato_cursor_update(anim);
}

static void tomato_enter_field_edit_mode(tomato_focus_t focus)
{
    /* 进入某个时间字段的编辑模式，记录临时值并显示滚轮。
     */
    if(!tomato_focus_is_time_field(focus)) {
        return;
    }

    s_tomato.field_edit_mode = true;
    s_tomato.edit_focus = focus;
    s_tomato.wheel_animing = false;
    s_tomato.pending_delta = 0;
    s_tomato.edit_temp = tomato_field_current_value(focus);

    lv_obj_add_flag(s_tomato.cursor, LV_OBJ_FLAG_HIDDEN);

    tomato_field_bg_update();
    tomato_wheel_show();
}

static void tomato_confirm_field_edit_mode(void)
{
    /* 确认字段编辑，把临时值写回 hour/min/sec，并刷新计时器设定。
     */
    if(s_tomato.edit_focus == TOMATO_FOCUS_HH) {
        s_tomato.hour = s_tomato.edit_temp;
    }
    else if(s_tomato.edit_focus == TOMATO_FOCUS_MM) {
        s_tomato.min = s_tomato.edit_temp;
    }
    else if(s_tomato.edit_focus == TOMATO_FOCUS_SS) {
        s_tomato.sec = s_tomato.edit_temp;
    }

    tomato_countdown_set_time(s_tomato.hour, s_tomato.min, s_tomato.sec);

    s_tomato.field_edit_mode = false;
    s_tomato.wheel_animing = false;
    s_tomato.pending_delta = 0;

    tomato_field_bg_update();
    tomato_wheel_hide();

    lv_obj_clear_flag(s_tomato.cursor, LV_OBJ_FLAG_HIDDEN);
    tomato_timer_reset_action();
    tomato_cursor_update(false);
}

void watch_tomato_clock_destroy(void);

/**
 * @brief 创建番茄钟页面。
 */
lv_obj_t *watch_tomato_clock_create(lv_obj_t *parent)
{
    if(s_tomato.page != NULL || s_tomato.timer != NULL) {
        watch_tomato_clock_destroy();
    }

    memset(&s_tomato, 0, sizeof(s_tomato));

    s_tomato.page = lv_obj_create(parent);
    lv_obj_remove_style_all(s_tomato.page);
    lv_obj_set_size(s_tomato.page, WATCH_SCREEN_W, WATCH_SCREEN_H);
    lv_obj_set_pos(s_tomato.page, 0, 0);
    lv_obj_set_style_bg_color(s_tomato.page, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_tomato.page, LV_OPA_COVER, 0);
    lv_obj_clear_flag(s_tomato.page, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(s_tomato.page, LV_OBJ_FLAG_CLICKABLE);

    s_tomato.back_btn = lv_label_create(s_tomato.page);
    tomato_label_style_26(s_tomato.back_btn);
    lv_label_set_text(s_tomato.back_btn, LV_SYMBOL_LEFT);
    lv_obj_set_pos(s_tomato.back_btn, 12, TOMATO_TOP_Y);

    s_tomato.time_set_bg = lv_obj_create(s_tomato.page);
    lv_obj_remove_style_all(s_tomato.time_set_bg);
    lv_obj_set_style_bg_color(s_tomato.time_set_bg, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(s_tomato.time_set_bg, LV_OPA_30, 0);
    lv_obj_set_style_radius(s_tomato.time_set_bg, 8, 0);
    lv_obj_clear_flag(s_tomato.time_set_bg, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(s_tomato.time_set_bg, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(s_tomato.time_set_bg, LV_OBJ_FLAG_HIDDEN);

    s_tomato.time_set_label = lv_label_create(s_tomato.page);
    tomato_label_style_20(s_tomato.time_set_label);
    lv_label_set_text(s_tomato.time_set_label, "Time Set");
    lv_obj_set_pos(s_tomato.time_set_label, 48, TOMATO_TOP_Y + 2);

    s_tomato.presets_label = lv_label_create(s_tomato.page);
    tomato_label_style_20(s_tomato.presets_label);
    lv_label_set_text(s_tomato.presets_label, "Saved");
    lv_obj_align(s_tomato.presets_label, LV_ALIGN_TOP_RIGHT, -10,
                 TOMATO_TOP_Y + 2);

    s_tomato.direction_label = lv_label_create(s_tomato.page);
    tomato_label_style_26(s_tomato.direction_label);
    s_tomato.count_up_mode = false;
    tomato_direction_label_update();

    s_tomato.reset_bg = lv_obj_create(s_tomato.page);
    lv_obj_remove_style_all(s_tomato.reset_bg);
    lv_obj_set_style_bg_color(s_tomato.reset_bg, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(s_tomato.reset_bg, LV_OPA_30, 0);
    lv_obj_set_style_radius(s_tomato.reset_bg, 8, 0);
    lv_obj_clear_flag(s_tomato.reset_bg, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(s_tomato.reset_bg, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(s_tomato.reset_bg, LV_OBJ_FLAG_HIDDEN);

    s_tomato.play_label = lv_label_create(s_tomato.page);
    tomato_label_style_26(s_tomato.play_label);
    lv_label_set_text(s_tomato.play_label, LV_SYMBOL_PLAY);
    lv_obj_align(s_tomato.play_label,
                 LV_ALIGN_BOTTOM_MID,
                 0,
                 -TOMATO_BOTTOM_PAD);

    s_tomato.reset_label = lv_label_create(s_tomato.page);
    tomato_label_style_26(s_tomato.reset_label);
    lv_label_set_text(s_tomato.reset_label, "RST");
    lv_obj_align(s_tomato.reset_label,
             LV_ALIGN_BOTTOM_RIGHT,
             -TOMATO_RESET_X,
             -TOMATO_RESET_Y);

    s_tomato.field_bg = lv_obj_create(s_tomato.page);
    lv_obj_remove_style_all(s_tomato.field_bg);
    lv_obj_set_style_bg_color(s_tomato.field_bg, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(s_tomato.field_bg, LV_OPA_30, 0);
    lv_obj_set_style_radius(s_tomato.field_bg, 8, 0);
    lv_obj_clear_flag(s_tomato.field_bg, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(s_tomato.field_bg, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(s_tomato.field_bg, LV_OBJ_FLAG_HIDDEN);

    s_tomato.countdown_frame = lv_obj_create(s_tomato.page);
    lv_obj_remove_style_all(s_tomato.countdown_frame);

    lv_obj_set_size(s_tomato.countdown_frame,TOMATO_COUNTDOWN_FRAME_W,TOMATO_COUNTDOWN_FRAME_H);

    lv_obj_set_pos(s_tomato.countdown_frame,TOMATO_COUNTDOWN_FRAME_X,TOMATO_COUNTDOWN_FRAME_Y);

    lv_obj_set_style_bg_opa(s_tomato.countdown_frame, LV_OPA_TRANSP, 0);

    lv_obj_set_style_border_color(s_tomato.countdown_frame, lv_color_hex(0xFF0000), 0);
    lv_obj_set_style_border_opa(s_tomato.countdown_frame, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_tomato.countdown_frame,TOMATO_COUNTDOWN_FRAME_BORDER_W,0);

    lv_obj_set_style_radius(s_tomato.countdown_frame,TOMATO_COUNTDOWN_FRAME_RADIUS,0);
    lv_obj_set_style_opa(s_tomato.countdown_frame, LV_OPA_COVER, 0);

    lv_obj_clear_flag(s_tomato.countdown_frame, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(s_tomato.countdown_frame, LV_OBJ_FLAG_CLICKABLE);

    s_tomato.countdown_hh = lv_label_create(s_tomato.page);
    s_tomato.countdown_colon_1 = lv_label_create(s_tomato.page);
    s_tomato.countdown_mm = lv_label_create(s_tomato.page);
    s_tomato.countdown_colon_2 = lv_label_create(s_tomato.page);
    s_tomato.countdown_ss = lv_label_create(s_tomato.page);

    lv_obj_t *countdown_labels[] = {
        s_tomato.countdown_hh,
        s_tomato.countdown_colon_1,
        s_tomato.countdown_mm,
        s_tomato.countdown_colon_2,
        s_tomato.countdown_ss,
    };

    for(int i = 0; i < 5; i++) {
        lv_obj_set_style_text_font(countdown_labels[i], &lv_font_montserrat_40, 0);
        lv_obj_set_style_text_color(countdown_labels[i], lv_color_white(), 0);
        lv_obj_set_style_text_align(countdown_labels[i], LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_clear_flag(countdown_labels[i], LV_OBJ_FLAG_CLICKABLE);
    }

    s_tomato.wheel = lv_obj_create(s_tomato.page);
    lv_obj_remove_style_all(s_tomato.wheel);
    lv_obj_set_style_bg_color(s_tomato.wheel, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_tomato.wheel, LV_OPA_60, 0);
    lv_obj_set_style_border_color(s_tomato.wheel, lv_color_white(), 0);
    lv_obj_set_style_border_opa(s_tomato.wheel, LV_OPA_70, 0);
    lv_obj_set_style_border_width(s_tomato.wheel, TOMATO_WHEEL_BORDER_W, 0);
    lv_obj_set_style_radius(s_tomato.wheel, TOMATO_WHEEL_RADIUS, 0);
    lv_obj_clear_flag(s_tomato.wheel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(s_tomato.wheel, LV_OBJ_FLAG_CLICKABLE);

    /*
     * 关键：滚轮动画时，超出 1x3 框的部分要被裁切。
     * 所以这里不要设置 LV_OBJ_FLAG_OVERFLOW_VISIBLE。
     */
    lv_obj_clear_flag(s_tomato.wheel, LV_OBJ_FLAG_OVERFLOW_VISIBLE);

    lv_obj_add_flag(s_tomato.wheel, LV_OBJ_FLAG_HIDDEN);

    s_tomato.wheel_strip = lv_obj_create(s_tomato.wheel);
    lv_obj_remove_style_all(s_tomato.wheel_strip);
    lv_obj_set_style_bg_opa(s_tomato.wheel_strip, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(s_tomato.wheel_strip, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(s_tomato.wheel_strip, LV_OBJ_FLAG_CLICKABLE);

    for(int i = 0; i < 5; i++) {
        s_tomato.wheel_item[i] = lv_label_create(s_tomato.wheel_strip);

        lv_obj_set_style_text_font(s_tomato.wheel_item[i],
                                   &lv_font_montserrat_40,
                                   0);

        lv_obj_set_style_text_color(s_tomato.wheel_item[i],
                                    lv_color_white(),
                                    0);

        lv_obj_set_style_text_align(s_tomato.wheel_item[i],
                                    LV_TEXT_ALIGN_CENTER,
                                    0);

        lv_obj_set_style_bg_opa(s_tomato.wheel_item[i],
                                LV_OPA_TRANSP,
                                0);

        lv_obj_clear_flag(s_tomato.wheel_item[i],
                          LV_OBJ_FLAG_CLICKABLE);

        snprintf(s_tomato.wheel_text[i],
         sizeof(s_tomato.wheel_text[i]),
         "00");

        lv_label_set_text_static(s_tomato.wheel_item[i],
                                s_tomato.wheel_text[i]);
    }

    s_tomato.cursor = lv_obj_create(s_tomato.page);
    lv_obj_remove_style_all(s_tomato.cursor);
    lv_obj_set_size(s_tomato.cursor, 20, 20);
    lv_obj_set_style_bg_opa(s_tomato.cursor, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_color(s_tomato.cursor, lv_color_white(), 0);
    lv_obj_set_style_border_opa(s_tomato.cursor, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_tomato.cursor, TOMATO_SELECTOR_BORDER_W, 0);
    lv_obj_set_style_radius(s_tomato.cursor, TOMATO_SELECTOR_RADIUS, 0);
    lv_obj_set_style_pad_all(s_tomato.cursor, 0, 0);
    lv_obj_clear_flag(s_tomato.cursor, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(s_tomato.cursor, LV_OBJ_FLAG_CLICKABLE);

    s_tomato.preset_panel = lv_obj_create(s_tomato.page);
    lv_obj_remove_style_all(s_tomato.preset_panel);
    lv_obj_set_size(s_tomato.preset_panel, WATCH_SCREEN_W, WATCH_SCREEN_H);
    lv_obj_set_pos(s_tomato.preset_panel, 0, 0);
    lv_obj_set_style_bg_color(s_tomato.preset_panel, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_tomato.preset_panel, LV_OPA_COVER, 0);
    lv_obj_clear_flag(s_tomato.preset_panel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(s_tomato.preset_panel, LV_OBJ_FLAG_CLICKABLE);

    s_tomato.preset_title = lv_label_create(s_tomato.preset_panel);
    tomato_label_style_20(s_tomato.preset_title);
    lv_obj_align(s_tomato.preset_title, LV_ALIGN_TOP_MID, 0,
                 TOMATO_PRESET_TITLE_Y);

    for(int i = 0; i < TOMATO_PRESET_LINE_COUNT; ++i) {
        s_tomato.preset_lines[i] = lv_label_create(s_tomato.preset_panel);
        tomato_label_style_20(s_tomato.preset_lines[i]);
        lv_obj_align(s_tomato.preset_lines[i], LV_ALIGN_TOP_MID, 0,
                     TOMATO_PRESET_LINE_Y + i * TOMATO_PRESET_LINE_STEP);
    }

    s_tomato.preset_cursor = lv_obj_create(s_tomato.preset_panel);
    lv_obj_remove_style_all(s_tomato.preset_cursor);
    lv_obj_set_style_bg_opa(s_tomato.preset_cursor, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_color(s_tomato.preset_cursor, lv_color_white(), 0);
    lv_obj_set_style_border_opa(s_tomato.preset_cursor, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_tomato.preset_cursor,
                                  TOMATO_SELECTOR_BORDER_W, 0);
    lv_obj_set_style_radius(s_tomato.preset_cursor,
                            TOMATO_SELECTOR_RADIUS, 0);
    lv_obj_clear_flag(s_tomato.preset_cursor, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(s_tomato.preset_cursor, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(s_tomato.preset_panel, LV_OBJ_FLAG_HIDDEN);

    s_tomato.timer = lv_timer_create(tomato_timer_cb, TOMATO_TIMER_PERIOD_MS, NULL);
    lv_timer_pause(s_tomato.timer);

    s_tomato.focus = TOMATO_FOCUS_BACK;
    s_tomato.edit_focus = TOMATO_FOCUS_SS;
    s_tomato.wants_back = false;
    s_tomato.time_set_mode = false;
    s_tomato.field_edit_mode = false;
    s_tomato.wheel_animing = false;
    s_tomato.pending_delta = 0;

    s_tomato.timer_active = false;
    s_tomato.timer_running = false;
    s_tomato.timer_finished = false;
    s_tomato.reset_pressed = false;

    s_tomato.hour = 0;
    s_tomato.min = 0;
    s_tomato.sec = 0;
    s_tomato.edit_temp = 0;
    s_tomato.timer_set_total = 0;
    s_tomato.timer_current = 0;
    s_tomato.preset_view = TOMATO_PRESET_VIEW_CLOSED;
    s_tomato.preset_field_edit = false;
    s_tomato.preset_focus = 0;
    s_tomato.preset_selected_slot = 0;

    tomato_countdown_set_time(0, 0, 0);
    tomato_apply_language(true);
    tomato_field_bg_update();
    tomato_wheel_hide();
    tomato_cursor_update(false);

    return s_tomato.page;
}

/**
 * @brief 重置番茄钟页面状态。
 */
void watch_tomato_clock_reset(void)
{
    /* 页面进入时复位番茄钟 UI 和状态，保证每次进入都有确定初始焦点。
     */
    if(s_tomato.page == NULL) {
        return;
    }

    s_tomato.focus = TOMATO_FOCUS_BACK;
    s_tomato.edit_focus = TOMATO_FOCUS_SS;
    s_tomato.wants_back = false;
    s_tomato.time_set_mode = false;
    s_tomato.field_edit_mode = false;
    s_tomato.wheel_animing = false;
    s_tomato.pending_delta = 0;

    s_tomato.count_up_mode = false;
    s_tomato.timer_active = false;
    s_tomato.timer_running = false;
    s_tomato.timer_finished = false;
    s_tomato.reset_pressed = false;

    s_tomato.hour = 0;
    s_tomato.min = 0;
    s_tomato.sec = 0;
    s_tomato.edit_temp = 0;
    s_tomato.timer_set_total = 0;
    s_tomato.timer_current = 0;
    s_tomato.preset_view = TOMATO_PRESET_VIEW_CLOSED;
    s_tomato.preset_field_edit = false;
    s_tomato.preset_focus = 0;

    if(s_tomato.timer) {
        lv_timer_pause(s_tomato.timer);
    }

    tomato_stop_finish_blink();
    tomato_direction_label_update();
    tomato_play_label_update();
    tomato_countdown_set_time(0, 0, 0);
    tomato_apply_language(true);
    tomato_field_bg_update();
    tomato_wheel_hide();
    lv_obj_add_flag(s_tomato.preset_panel, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(s_tomato.cursor, LV_OBJ_FLAG_HIDDEN);
    tomato_cursor_update(false);
}

/**
 * @brief 销毁番茄钟页面和定时器。
 */
void watch_tomato_clock_destroy(void)
{
    /* 销毁番茄钟定时器和页面对象引用，避免页面离开后定时器继续回调。
     */
    if(s_tomato.timer) {
        lv_timer_delete(s_tomato.timer);
        s_tomato.timer = NULL;
    }

    if(s_tomato.cursor) {
        lv_anim_del(s_tomato.cursor, tomato_cursor_x_anim_cb);
        lv_anim_del(s_tomato.cursor, tomato_cursor_y_anim_cb);
        lv_anim_del(s_tomato.cursor, tomato_cursor_w_anim_cb);
        lv_anim_del(s_tomato.cursor, tomato_cursor_h_anim_cb);
    }

    if(s_tomato.countdown_frame) {
        lv_anim_del(s_tomato.countdown_frame, tomato_frame_opa_anim_cb);
    }

    if(s_tomato.wheel_strip) {
        lv_anim_del(s_tomato.wheel_strip, tomato_wheel_y_anim_cb);
    }

    if(s_tomato.page) {
        lv_obj_del(s_tomato.page);
        s_tomato.page = NULL;
    }

    memset(&s_tomato, 0, sizeof(s_tomato));
}

/**
 * @brief 处理番茄钟页面按键事件。
 */
void watch_tomato_clock_on_key(watch_key_t key)
{
    /* 番茄钟按键状态机，是本页面最核心的交互入口。不同模式下同一按键含义不同。
     */
    if(s_tomato.page == NULL) {
        return;
    }

    tomato_apply_language(false);

    if(s_tomato.preset_view != TOMATO_PRESET_VIEW_CLOSED) {
        tomato_preset_on_key(key);
        return;
    }

    if(key == WATCH_KEY_2_RELEASE) {
        if(s_tomato.reset_pressed) {
            s_tomato.reset_pressed = false;
            tomato_reset_bg_update();
        }
        return;
    }

    /*
     * 正在修改 HH / MM / SS 时：
     * KEY1：当前字段 - 1
     * KEY3：当前字段 + 1
     * KEY2：确认当前字段，退出当前字段修改模式
     */
    if(s_tomato.field_edit_mode) {
        if(key == WATCH_KEY_1) {
            tomato_wheel_step(-1);
            return;
        }

        if(key == WATCH_KEY_3) {
            tomato_wheel_step(1);
            return;
        }

        if(key == WATCH_KEY_2) {
            tomato_confirm_field_edit_mode();
            return;
        }

        return;
    }

    if(key == WATCH_KEY_3) {
        if(s_tomato.time_set_mode) {
            if(s_tomato.focus == TOMATO_FOCUS_SS) {
                /* SS 下按 KEY3：退回 TimeSet，并退出时间设置模式。 */
                tomato_exit_time_set_mode(true);
                return;
            }
            else if(s_tomato.focus == TOMATO_FOCUS_MM) {
                s_tomato.focus = TOMATO_FOCUS_SS;
            }
            else if(s_tomato.focus == TOMATO_FOCUS_HH) {
                s_tomato.focus = TOMATO_FOCUS_MM;
            }
            else {
                s_tomato.focus = TOMATO_FOCUS_HH;
            }
        } else {
            if(s_tomato.focus == TOMATO_FOCUS_BACK) {
                s_tomato.focus = TOMATO_FOCUS_TIME_SET;
            }
            else if(s_tomato.focus == TOMATO_FOCUS_TIME_SET) {
                s_tomato.focus = TOMATO_FOCUS_PRESETS;
            }
            else if(s_tomato.focus == TOMATO_FOCUS_PRESETS) {
                s_tomato.focus = TOMATO_FOCUS_DIRECTION;
            }
            else if(s_tomato.focus == TOMATO_FOCUS_DIRECTION) {
                s_tomato.focus = TOMATO_FOCUS_PLAY_PAUSE;
            }
            else if(s_tomato.focus == TOMATO_FOCUS_PLAY_PAUSE) {
                s_tomato.focus = TOMATO_FOCUS_RESET;
            }
            else if(s_tomato.focus == TOMATO_FOCUS_RESET) {
                s_tomato.focus = TOMATO_FOCUS_BACK;
            }
            else {
                s_tomato.focus = TOMATO_FOCUS_BACK;
            }
        }

        tomato_cursor_update(true);
        return;
    }

    if(key == WATCH_KEY_1) {
        if(s_tomato.time_set_mode) {
            if(s_tomato.focus == TOMATO_FOCUS_SS) {
                s_tomato.focus = TOMATO_FOCUS_MM;
            }
            else if(s_tomato.focus == TOMATO_FOCUS_MM) {
                s_tomato.focus = TOMATO_FOCUS_HH;
            }
            else if(s_tomato.focus == TOMATO_FOCUS_HH) {
                /* HH 下按 KEY1：不再向左移动。 */
                return;
            }
            else {
                s_tomato.focus = TOMATO_FOCUS_HH;
            }
        } else {
            if(s_tomato.focus == TOMATO_FOCUS_TIME_SET) {
                s_tomato.focus = TOMATO_FOCUS_BACK;
            }
            else if(s_tomato.focus == TOMATO_FOCUS_PRESETS) {
                s_tomato.focus = TOMATO_FOCUS_TIME_SET;
            }
            else if(s_tomato.focus == TOMATO_FOCUS_DIRECTION) {
                s_tomato.focus = TOMATO_FOCUS_PRESETS;
            }
            else if(s_tomato.focus == TOMATO_FOCUS_PLAY_PAUSE) {
                s_tomato.focus = TOMATO_FOCUS_DIRECTION;
            }
            else if(s_tomato.focus == TOMATO_FOCUS_RESET) {
                s_tomato.focus = TOMATO_FOCUS_PLAY_PAUSE;
            }
            else {
                return;
            }
        }

        tomato_cursor_update(true);
        return;
    }

    if(key == WATCH_KEY_2) {
        if(s_tomato.focus == TOMATO_FOCUS_BACK) {
            s_tomato.wants_back = true;
        }
        else if(s_tomato.focus == TOMATO_FOCUS_TIME_SET) {
            tomato_timer_reset_action();
            s_tomato.time_set_mode = true;
            tomato_time_set_bg_update();

            /*
             * 进入 TimeSet 模式后，光标自动移动到 HH 下方。
             * 然后 KEY3 按 HH -> MM -> SS -> TimeSet 的顺序向右移动。
             */
            s_tomato.focus = TOMATO_FOCUS_HH;
            tomato_cursor_update(true);
        }
        else if(s_tomato.focus == TOMATO_FOCUS_PRESETS) {
            tomato_preset_open();
        }
        else if(s_tomato.focus == TOMATO_FOCUS_DIRECTION) {
            tomato_direction_toggle();
        }
        else if(s_tomato.focus == TOMATO_FOCUS_PLAY_PAUSE) {
            tomato_timer_toggle_play_pause();
        }
        else if(s_tomato.focus == TOMATO_FOCUS_RESET) {
            s_tomato.reset_pressed = true;
            tomato_reset_bg_update();
            tomato_timer_clear_action();
        }
        else if(tomato_focus_is_time_field(s_tomato.focus)) {
            if(s_tomato.time_set_mode) {
                tomato_enter_field_edit_mode(s_tomato.focus);
            }
        }

        return;
    }
}

/**
 * @brief 查询番茄钟页面是否请求返回。
 */
bool watch_tomato_clock_wants_back(void)
{
    /* 供 UI 调度器判断番茄钟页是否请求返回菜单。
     */
    return s_tomato.wants_back;
}


/* 维护提示
 * 调整番茄钟交互时，请先确认普通模式、时间设置模式和字段编辑模式三套按键语义。
 */
