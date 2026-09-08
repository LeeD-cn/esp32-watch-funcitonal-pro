/**
 * @file watch_face.c
 * @brief 主表盘页面、时间动画、电量和 Wi-Fi 状态显示。
 */
/*
 * ==================== 模块说明 ====================
 *  * 模块职责：
 * - 主表盘页面，负责时间数字动画、日期/星期、电量条和 Wi-Fi 状态图标。
 * - 时间数字采用当前层和下一层两套 label，通过 y 方向动画实现翻页/滑动切换。
 * - 封面图优先读取用户配置图片，没有配置时使用编译期 cover 资源。
 * - 电池与 Wi-Fi 状态定时刷新，但会尽量避免无意义重绘以减少 LVGL 开销。
 *
 * 阅读建议：
 * - 先看本文件顶部的宏定义和静态状态变量，理解硬件参数和运行状态。
 * - 再看 reset/init/start/on_key/destroy 等对外函数，理解页面或驱动的生命周期。
 * - 最后看 static 辅助函数，了解具体寄存器读写、UI 刷新或状态机细节。
 * =======================================================
 */

#include "watch_face.h"
#include "watch_battery.h"
#include "watch_language.h"
#include "watch_wifi.h"
#include "watch_config.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <time.h>


/**
 * @brief 表盘使用的数字字体和中文日期字体。
 */
LV_FONT_DECLARE(sj_narrow_80);
LV_FONT_DECLARE(sj_narrow_54);
LV_FONT_DECLARE(cn_font_20);

#define WATCH_SCREEN_W          240
#define WATCH_SCREEN_H          240


#define TIME_ROW_Y              78
#define DIGIT_BIG_W             38
#define DIGIT_BIG_H             80
#define DIGIT_SMALL_W           25
#define DIGIT_SMALL_H           54
#define COLON_W                 14
#define TIME_TOTAL_W            (DIGIT_BIG_W * 4 + COLON_W + DIGIT_SMALL_W * 2)
#define DATE_Y                  160
#define DATE_WEEK_GAP           8
#define DIGIT_ANIM_MS           500

#define BATTERY_X               200
#define BATTERY_Y               3
#define BATTERY_BODY_W          35
#define BATTERY_BODY_H          18
#define BATTERY_PAD             2
#define BATTERY_REFRESH_MS      5000
#define WIFI_ICON_W             24
#define WIFI_ICON_H             22
#define WIFI_BATTERY_GAP        5


LV_IMG_DECLARE(cover)

/**
 * @brief 获取主表盘封面图片源。
 *
 * 详细说明：
 * - 优先使用用户配置图片，否则回退到内置 cover。
 *
 * @return 函数执行结果或计算得到的值，具体语义见返回路径。
 */
static const void *watch_face_get_cover_src(void)
{
    const lv_img_dsc_t *cfg_cover = watch_config_get_cover_image();
    return cfg_cover != NULL ? (const void *)cfg_cover : (const void *)&cover;
}


/**
 * @brief 单个数字层，包含正文和阴影。
 */
typedef struct{
    lv_obj_t *layer;
    lv_obj_t *label;
    lv_obj_t *shadow;
}digit_layer_t;

/**
 * @brief 一个可滑动切换的数字槽。
 */
typedef struct {
    lv_obj_t *box;

    digit_layer_t cur;
    digit_layer_t next;

    char value;
    bool animing;
    lv_coord_t h;
} digit_slot_t;

/* 六个时间数字槽，分别显示 HHMMSS。 */
static digit_slot_t s_digit[6];
/* 日期文本对象。 */
static lv_obj_t *s_date_label = NULL;
/* 星期文本对象。 */
static lv_obj_t *s_week_label = NULL;
/* 电池外框对象。 */
static lv_obj_t *s_battery_body = NULL;
static lv_obj_t *s_battery_fill_bg = NULL;
static lv_obj_t *s_battery_fill = NULL;
static lv_obj_t *s_battery_label = NULL;
/* Wi-Fi 状态文本/图标对象。 */
static lv_obj_t *s_wifi_label = NULL;
/* 电池周期刷新定时器。 */
static lv_timer_t *s_battery_timer = NULL;

static int s_last_sec = -1;
static int s_last_yday = -1;
static bool s_time_valid_once = false;
static bool s_wifi_state_valid_once = false;
static bool s_last_wifi_connected = false;
static watch_language_t s_last_date_language = WATCH_LANGUAGE_ENGLISH;

static const char *s_week_en[7] = {
    "SUN", "MON", "TUE", "WED", "THU", "FRI", "SAT"
};

static const char *s_week_cn[7] = {
    "周日", "周一", "周二", "周三", "周四", "周五", "周六"
};

/**
 * @brief 重新计算日期和星期标签的水平布局。
 *
 * 详细说明：
 * - 根据两段文本实际宽度居中显示。
 */
static void date_week_layout_update(void)
{
    if(s_date_label == NULL || s_week_label == NULL) {
        return;
    }

    lv_obj_t *parent = lv_obj_get_parent(s_date_label);
    if(parent != NULL) {
        lv_obj_update_layout(parent);
    }

    lv_coord_t date_w = lv_obj_get_width(s_date_label);
    lv_coord_t week_w = lv_obj_get_width(s_week_label);
    lv_coord_t total_w = date_w + DATE_WEEK_GAP + week_w;
    lv_coord_t x = (WATCH_SCREEN_W - total_w) / 2;

    lv_obj_set_pos(s_date_label, x, DATE_Y);
    lv_obj_set_pos(s_week_label, x + date_w + DATE_WEEK_GAP, DATE_Y);
}

/**
 * @brief 设置普通白色文本样式。
 *
 * 详细说明：
 * - 统一字体、颜色、对齐方式和内边距。
 *
 * @param label 输入或输出参数，具体含义见调用处和函数内部使用方式。
 * @param font 输入或输出参数，具体含义见调用处和函数内部使用方式。
 */
static void set_label_font_color(lv_obj_t *label, const lv_font_t *font)
{
    lv_obj_set_style_text_font(label, font, 0);
    lv_obj_set_style_text_color(label, lv_color_white(), 0);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_pad_all(label, 0, 0);
}

/**
 * @brief 设置数字阴影文本样式。
 *
 * 详细说明：
 * - 使用深色和透明度营造数字层次感。
 *
 * @param label 输入或输出参数，具体含义见调用处和函数内部使用方式。
 * @param font 输入或输出参数，具体含义见调用处和函数内部使用方式。
 */
static void set_label_shadow_style(lv_obj_t *label, const lv_font_t *font)
{
    lv_obj_set_style_text_font(label, font, 0);
    lv_obj_set_style_text_color(label, lv_color_hex(0x202020), 0);
    lv_obj_set_style_text_opa(label, LV_OPA_70, 0);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_pad_all(label, 0, 0);
}

/**
 * @brief 设置数字层文本。
 *
 * 详细说明：
 * - 同时更新正文和阴影 label。
 *
 * @param layer 输入或输出参数，具体含义见调用处和函数内部使用方式。
 * @param value 输入或输出参数，具体含义见调用处和函数内部使用方式。
 */
static void digit_layer_set_text(digit_layer_t *layer, char value)
{
    char txt[2] = {value, '\0'};

    if(layer->shadow) {
        lv_label_set_text(layer->shadow, txt);
    }

    if(layer->label) {
        lv_label_set_text(layer->label, txt);
    }
}

/**
 * @brief 创建一个数字显示层。
 *
 * 详细说明：
 * - 每层包含正文和阴影两个 label。
 *
 * @param layer 输入或输出参数，具体含义见调用处和函数内部使用方式。
 * @param parent 输入或输出参数，具体含义见调用处和函数内部使用方式。
 * @param w 输入或输出参数，具体含义见调用处和函数内部使用方式。
 * @param h 输入或输出参数，具体含义见调用处和函数内部使用方式。
 * @param font 输入或输出参数，具体含义见调用处和函数内部使用方式。
 */
static void digit_layer_create(digit_layer_t *layer,
                               lv_obj_t *parent,
                               lv_coord_t w,
                               lv_coord_t h,
                               const lv_font_t *font)
{
    memset(layer, 0, sizeof(*layer));

    layer->layer = lv_obj_create(parent);
    lv_obj_remove_style_all(layer->layer);
    lv_obj_set_size(layer->layer, w, h);
    lv_obj_set_pos(layer->layer, 0, 0);
    lv_obj_set_style_pad_all(layer->layer, 0, 0);
    lv_obj_set_style_border_width(layer->layer, 0, 0);
    lv_obj_set_style_bg_opa(layer->layer, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(layer->layer, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(layer->layer, LV_OBJ_FLAG_CLICKABLE);

    layer->shadow = lv_label_create(layer->layer);
    set_label_shadow_style(layer->shadow, font);
    lv_obj_set_size(layer->shadow, w, h);
    lv_obj_set_pos(layer->shadow, 2, 2);
    lv_label_set_text(layer->shadow, "");

    layer->label = lv_label_create(layer->layer);
    set_label_font_color(layer->label, font);
    lv_obj_set_size(layer->label, w, h);
    lv_obj_set_pos(layer->label, 0, 0);
    lv_label_set_text(layer->label, "");
}

/**
 * @brief 数字滑动动画执行回调。
 *
 * 详细说明：
 * - 根据动画值更新数字层 y 坐标。
 *
 * @param var 输入或输出参数，具体含义见调用处和函数内部使用方式。
 * @param v 输入或输出参数，具体含义见调用处和函数内部使用方式。
 */
static void digit_slide_exec_cb(void *var, int32_t v)
{
    digit_slot_t *d = (digit_slot_t *)var;

    if(d == NULL || d->cur.layer == NULL || d->next.layer == NULL) {
        return;
    }

    lv_obj_set_y(d->cur.layer, -v);
    lv_obj_set_y(d->next.layer, d->h - v);
}

/**
 * @brief 停止单个数字槽仍在运行的动画。
 */
static void digit_slot_stop_anim(digit_slot_t *d)
{
    if(d == NULL) {
        return;
    }

    lv_anim_del(d, digit_slide_exec_cb);
    d->animing = false;
}

/**
 * @brief 停止所有时间数字动画。
 */
static void digit_all_stop_anim(void)
{
    for(int i = 0; i < 6; i++) {
        digit_slot_stop_anim(&s_digit[i]);
    }
}

/**
 * @brief 数字滑动动画结束回调。
 *
 * 详细说明：
 * - 把 next 层变为当前层，并清理动画状态。
 *
 * @param a 输入或输出参数，具体含义见调用处和函数内部使用方式。
 */
static void digit_slide_ready_cb(lv_anim_t *a)
{
    digit_slot_t *d = (digit_slot_t *)a->var;

    if(d == NULL || d->cur.layer == NULL || d->next.layer == NULL) {
        if(d != NULL) {
            d->animing = false;
        }
        return;
    }

    digit_layer_t tmp = d->cur;
    d->cur = d->next;
    d->next = tmp;

    lv_obj_set_y(d->cur.layer, 0);
    lv_obj_set_y(d->next.layer, d->h);

    d->animing = false;
}

/**
 * @brief 更新数字槽内容，非首次更新时播放上下滑动动画。
 *
 * 详细说明：
 * - 字符变化时启动滑动动画，不变时直接保持。
 *
 * @param d 输入或输出参数，具体含义见函数内部使用方式。
 * @param value 输入或输出参数，具体含义见函数内部使用方式。
 */
static void digit_slot_set(digit_slot_t *d, char value)
{
    if(d->value == value) {
        return;
    }


    if(d->value == '\0') {
        d->value = value;
        digit_layer_set_text(&d->cur, value);
        lv_obj_set_y(d->cur.layer, 0);
        lv_obj_set_y(d->next.layer, d->h);
        return;
    }

    if(d->animing) {
    return;
    }


    digit_slot_stop_anim(d);

    d->value = value;
    d->animing = true;

    digit_layer_set_text(&d->next, value);

    lv_obj_set_y(d->cur.layer, 0);
    lv_obj_set_y(d->next.layer, d->h);

    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, d);
    lv_anim_set_values(&a, 0, d->h);
    lv_anim_set_time(&a, DIGIT_ANIM_MS);
    lv_anim_set_ready_cb(&a, digit_slide_ready_cb);
    lv_anim_set_exec_cb(&a, digit_slide_exec_cb);
    lv_anim_set_path_cb(&a, lv_anim_path_linear);
    lv_anim_start(&a);
}

/**
 * @brief 创建可动画切换的数字槽。
 *
 * 详细说明：
 * - 为时分秒各位创建独立容器和双层数字。
 *
 * @param d 输入或输出参数，具体含义见调用处和函数内部使用方式。
 * @param parent 输入或输出参数，具体含义见调用处和函数内部使用方式。
 * @param x 输入或输出参数，具体含义见调用处和函数内部使用方式。
 * @param y 输入或输出参数，具体含义见调用处和函数内部使用方式。
 * @param w 输入或输出参数，具体含义见调用处和函数内部使用方式。
 * @param h 输入或输出参数，具体含义见调用处和函数内部使用方式。
 * @param font 输入或输出参数，具体含义见调用处和函数内部使用方式。
 */
static void digit_slot_create(digit_slot_t *d,
                              lv_obj_t *parent,
                              lv_coord_t x,
                              lv_coord_t y,
                              lv_coord_t w,
                              lv_coord_t h,
                              const lv_font_t *font)
{
    memset(d, 0, sizeof(*d));
    d->value = '\0';
    d->animing = false;
    d->h = h;

    d->box = lv_obj_create(parent);
    lv_obj_remove_style_all(d->box);
    lv_obj_set_size(d->box, w, h);
    lv_obj_set_pos(d->box, x, y);
    lv_obj_set_style_pad_all(d->box, 0, 0);
    lv_obj_set_style_border_width(d->box, 0, 0);
    lv_obj_set_style_bg_opa(d->box, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(d->box, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(d->box, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(d->box, LV_OBJ_FLAG_OVERFLOW_VISIBLE);


    digit_layer_create(&d->cur, d->box, w, h, font);
    lv_obj_set_y(d->cur.layer, 0);


    digit_layer_create(&d->next, d->box, w, h, font);
    lv_obj_set_y(d->next.layer, h);
}

/**
 * @brief 设置 Wi-Fi 图标连接状态。
 *
 * 详细说明：
 * - 根据连接状态显示不同符号或颜色。
 *
 * @param connected 输入或输出参数，具体含义见调用处和函数内部使用方式。
 */
static void wifi_ui_set_connected(bool connected)
{
    if(s_wifi_label == NULL) {
        return;
    }

    if(connected) {
        lv_obj_clear_flag(s_wifi_label, LV_OBJ_FLAG_HIDDEN);
    }
    else {
        lv_obj_add_flag(s_wifi_label, LV_OBJ_FLAG_HIDDEN);
    }
}

/**
 * @brief 刷新 Wi-Fi 状态显示。
 *
 * 详细说明：
 * - 只有状态变化时才更新 UI。
 */
static void update_wifi_ui(void)
{
    bool connected = watch_wifi_is_connected();

    if(s_wifi_state_valid_once && connected == s_last_wifi_connected) {
        return;
    }

    s_wifi_state_valid_once = true;
    s_last_wifi_connected = connected;
    wifi_ui_set_connected(connected);
}

/**
 * @brief 创建 Wi-Fi 状态控件。
 *
 * 详细说明：
 * - 位于电池图标附近，用于提示联网状态。
 *
 * @param parent 输入或输出参数，具体含义见调用处和函数内部使用方式。
 */
static void wifi_ui_create(lv_obj_t *parent)
{
    s_wifi_label = lv_label_create(parent);
    lv_obj_set_style_text_font(s_wifi_label, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(s_wifi_label, lv_color_white(), 0);
    lv_obj_set_style_text_align(s_wifi_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_pad_all(s_wifi_label, 0, 0);
    lv_label_set_text(s_wifi_label, LV_SYMBOL_WIFI);
    lv_obj_set_size(s_wifi_label, WIFI_ICON_W, WIFI_ICON_H);
    lv_obj_set_pos(s_wifi_label,
                   BATTERY_X - WIFI_BATTERY_GAP - WIFI_ICON_W,
                   BATTERY_Y - 1);
    lv_obj_clear_flag(s_wifi_label, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(s_wifi_label, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_move_foreground(s_wifi_label);

    s_wifi_state_valid_once = false;
    update_wifi_ui();
}

/**
 * @brief 设置电池图形百分比。
 *
 * 详细说明：
 * - 更新填充宽度和百分比文本。
 *
 * @param percent 输入或输出参数，具体含义见调用处和函数内部使用方式。
 * @param valid 输入或输出参数，具体含义见调用处和函数内部使用方式。
 * @param charging 输入或输出参数，具体含义见调用处和函数内部使用方式。
 */
static void battery_ui_set_percent(int percent, bool valid, bool charging)
{
    if(s_battery_fill == NULL || s_battery_label == NULL) {
        return;
    }


    lv_obj_set_style_bg_color(s_battery_fill,
                              charging ? lv_color_hex(0x00C853) : lv_color_white(),
                              0);

    if(!valid) {
        lv_obj_set_width(s_battery_fill, 0);
        lv_label_set_text(s_battery_label, "--");
        return;
    }

    if(percent < 0) {
        percent = 0;
    }
    if(percent > 100) {
        percent = 100;
    }


    lv_coord_t inner_w = BATTERY_BODY_W - 2 * BATTERY_PAD;
    lv_coord_t fill_w = (inner_w * percent) / 100;

    lv_obj_set_pos(s_battery_fill, BATTERY_X + BATTERY_PAD, BATTERY_Y + BATTERY_PAD);
    lv_obj_set_size(s_battery_fill, fill_w, BATTERY_BODY_H - 2 * BATTERY_PAD);

    char txt[8];
    snprintf(txt, sizeof(txt), "%d", percent);
    lv_label_set_text(s_battery_label, txt);
}

/**
 * @brief 读取并刷新电池 UI。
 *
 * 详细说明：
 * - 同时考虑充电状态和电量百分比。
 */
static void update_battery_ui(void)
{
    int percent = 0;
    bool charging = false;


    (void)watch_battery_is_charging(&charging);

    if(watch_battery_read_percent(&percent) == ESP_OK) {
        battery_ui_set_percent(percent, true, charging);
    }
    else {
        battery_ui_set_percent(0, false, charging);
    }
}

/**
 * @brief 电池定时刷新回调。
 *
 * 详细说明：
 * - 周期更新电池图标，避免每帧读取 I2C。
 *
 * @param timer 输入或输出参数，具体含义见调用处和函数内部使用方式。
 */
static void battery_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    update_battery_ui();
}

/**
 * @brief 创建电池图标和文本。
 *
 * 详细说明：
 * - 包含外框、填充背景、填充条和数字标签。
 *
 * @param parent 输入或输出参数，具体含义见调用处和函数内部使用方式。
 */
static void battery_ui_create(lv_obj_t *parent)
{

    s_battery_fill_bg = lv_obj_create(parent);
    lv_obj_remove_style_all(s_battery_fill_bg);
    lv_obj_set_size(s_battery_fill_bg,
                    BATTERY_BODY_W - 2 * BATTERY_PAD,
                    BATTERY_BODY_H - 2 * BATTERY_PAD);
    lv_obj_set_pos(s_battery_fill_bg,
                   BATTERY_X + BATTERY_PAD,
                   BATTERY_Y + BATTERY_PAD);
    lv_obj_set_style_bg_color(s_battery_fill_bg, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(s_battery_fill_bg, 180, 0);
    lv_obj_set_style_radius(s_battery_fill_bg, 2, 0);
    lv_obj_clear_flag(s_battery_fill_bg, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(s_battery_fill_bg, LV_OBJ_FLAG_CLICKABLE);


    s_battery_fill = lv_obj_create(parent);
    lv_obj_remove_style_all(s_battery_fill);
    lv_obj_set_size(s_battery_fill, 0, BATTERY_BODY_H - 2 * BATTERY_PAD);
    lv_obj_set_pos(s_battery_fill,
                   BATTERY_X + BATTERY_PAD,
                   BATTERY_Y + BATTERY_PAD);
    lv_obj_set_style_bg_color(s_battery_fill, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(s_battery_fill, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_battery_fill, 2, 0);
    lv_obj_clear_flag(s_battery_fill, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(s_battery_fill, LV_OBJ_FLAG_CLICKABLE);


    s_battery_body = lv_obj_create(parent);
    lv_obj_remove_style_all(s_battery_body);
    lv_obj_set_size(s_battery_body, BATTERY_BODY_W, BATTERY_BODY_H);
    lv_obj_set_pos(s_battery_body, BATTERY_X, BATTERY_Y);
    lv_obj_set_style_bg_opa(s_battery_body, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_battery_body, 2, 0);
    lv_obj_set_style_border_color(s_battery_body, lv_color_white(), 0);
    lv_obj_set_style_radius(s_battery_body, 5, 0);
    lv_obj_set_style_pad_all(s_battery_body, 0, 0);
    lv_obj_clear_flag(s_battery_body, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(s_battery_body, LV_OBJ_FLAG_CLICKABLE);


    s_battery_label = lv_label_create(parent);
    lv_obj_set_style_text_font(s_battery_label, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_battery_label, lv_color_hex(0x808080), 0);
    lv_obj_set_style_text_align(s_battery_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_pad_all(s_battery_label, 0, 0);
    lv_label_set_text(s_battery_label, "--");
    lv_obj_set_size(s_battery_label, BATTERY_BODY_W, BATTERY_BODY_H);
    lv_obj_set_pos(s_battery_label, BATTERY_X, BATTERY_Y + 1);

    lv_obj_move_foreground(s_battery_body);
    lv_obj_move_foreground(s_battery_label);

    update_battery_ui();
}

/**
 * @brief 根据系统时间刷新时分秒、日期和星期。
 *
 * 详细说明：
 * - 时间变化时更新数字槽，日期变化时更新日期文本。
 */
static void update_time_ui(void)
{
    time_t now;
    struct tm t;

    time(&now);
    localtime_r(&now, &t);


    watch_language_t current_language = watch_language_get();

    if(s_last_sec == t.tm_sec &&
       s_time_valid_once &&
       s_last_date_language == current_language) {
        return;
    }

    s_last_sec = t.tm_sec;

    char buf[7];
    snprintf(buf, sizeof(buf), "%02d%02d%02d", t.tm_hour, t.tm_min, t.tm_sec);

    for(int i = 0; i < 6; i++) {
        digit_slot_set(&s_digit[i], buf[i]);
    }


    if(s_date_label && s_week_label &&
       (s_last_yday != t.tm_yday ||
        s_last_date_language != current_language ||
        !s_time_valid_once)) {
        bool chinese = current_language == WATCH_LANGUAGE_CHINESE;
        char date_buf[64];


        lv_obj_set_style_text_font(s_date_label, &lv_font_montserrat_20, 0);
        lv_obj_set_style_text_font(s_week_label,
                                   chinese ? &cn_font_20 : &lv_font_montserrat_20,
                                   0);

        snprintf(date_buf,
                 sizeof(date_buf),
                 "%04d/%d/%d",
                 t.tm_year + 1900,
                 t.tm_mon + 1,
                 t.tm_mday);

        lv_label_set_text(s_date_label, date_buf);
        lv_label_set_text(s_week_label,
                          chinese ? s_week_cn[t.tm_wday] : s_week_en[t.tm_wday]);
        date_week_layout_update();

        s_last_yday = t.tm_yday;
        s_last_date_language = current_language;
    }

    s_time_valid_once = true;
}

/**
 * @brief 主表盘定时器回调。
 *
 * 详细说明：
 * - 驱动时间、Wi-Fi 和电池状态刷新。
 *
 * @param timer 输入或输出参数，具体含义见调用处和函数内部使用方式。
 */
static void watch_face_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    update_time_ui();
    update_wifi_ui();
}

static lv_timer_t *s_watch_face_timer = NULL;

/**
 * @brief watch_face_create_on 对外接口。
 *
 * 详细说明：
 * - 供其他模块调用，隐藏本文件内部状态细节。
 *
 * @param parent 输入或输出参数，具体含义见调用处和函数内部使用方式。
 *
 * @return 函数执行结果或计算得到的值，具体语义见返回路径。
 */
lv_obj_t *watch_face_create_on(lv_obj_t *parent)
{
    digit_all_stop_anim();

    memset(s_digit, 0, sizeof(s_digit));
    s_date_label = NULL;
    s_week_label = NULL;
    s_battery_body = NULL;
    s_battery_fill_bg = NULL;
    s_battery_fill = NULL;
    s_battery_label = NULL;
    s_wifi_label = NULL;
    s_wifi_state_valid_once = false;
    s_last_wifi_connected = false;
    s_last_sec = -1;
    s_last_yday = -1;
    s_last_date_language = watch_language_get();
    s_time_valid_once = false;

    lv_obj_remove_style_all(parent);
    lv_obj_set_size(parent, WATCH_SCREEN_W, WATCH_SCREEN_H);
    lv_obj_set_style_bg_color(parent, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(parent, LV_OPA_COVER, 0);
    lv_obj_clear_flag(parent, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(parent, LV_OBJ_FLAG_CLICKABLE);


    lv_obj_t *img = lv_img_create(parent);
    lv_img_set_src(img, watch_face_get_cover_src());
    lv_obj_align(img, LV_ALIGN_CENTER, 0, 0);

    lv_obj_t *root = lv_obj_create(parent);
    lv_obj_remove_style_all(root);
    lv_obj_set_size(root, WATCH_SCREEN_W, WATCH_SCREEN_H);
    lv_obj_set_pos(root, 0, 0);
    lv_obj_clear_flag(root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(root, LV_OBJ_FLAG_CLICKABLE);

    battery_ui_create(root);
    wifi_ui_create(root);

    const lv_coord_t start_x = (WATCH_SCREEN_W - TIME_TOTAL_W) / 2;
    const lv_coord_t big_y = TIME_ROW_Y;
    const lv_coord_t small_y = TIME_ROW_Y + (DIGIT_BIG_H - DIGIT_SMALL_H);

    lv_coord_t x = start_x;


    digit_slot_create(&s_digit[0], root, x, big_y, DIGIT_BIG_W, DIGIT_BIG_H, &sj_narrow_80);
    x += DIGIT_BIG_W;
    digit_slot_create(&s_digit[1], root, x, big_y, DIGIT_BIG_W, DIGIT_BIG_H, &sj_narrow_80);
    x += DIGIT_BIG_W;


    lv_obj_t *colon_shadow = lv_label_create(root);
    set_label_shadow_style(colon_shadow, &sj_narrow_80);
    lv_label_set_text(colon_shadow, ":");
    lv_obj_set_size(colon_shadow, COLON_W, DIGIT_BIG_H);
    lv_obj_set_pos(colon_shadow, x + 2, big_y + 2);

    lv_obj_t *colon = lv_label_create(root);
    set_label_font_color(colon, &sj_narrow_80);
    lv_label_set_text(colon, ":");
    lv_obj_set_size(colon, COLON_W, DIGIT_BIG_H);
    lv_obj_set_pos(colon, x, big_y);
    x += COLON_W;


    digit_slot_create(&s_digit[2], root, x, big_y, DIGIT_BIG_W, DIGIT_BIG_H, &sj_narrow_80);
    x += DIGIT_BIG_W;
    digit_slot_create(&s_digit[3], root, x, big_y, DIGIT_BIG_W, DIGIT_BIG_H, &sj_narrow_80);
    x += DIGIT_BIG_W;


    digit_slot_create(&s_digit[4], root, x, small_y, DIGIT_SMALL_W, DIGIT_SMALL_H, &sj_narrow_54);
    x += DIGIT_SMALL_W;
    digit_slot_create(&s_digit[5], root, x, small_y, DIGIT_SMALL_W, DIGIT_SMALL_H, &sj_narrow_54);


    s_date_label = lv_label_create(root);
    lv_obj_set_style_text_font(s_date_label, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(s_date_label, lv_color_white(), 0);
    lv_obj_set_style_text_align(s_date_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_letter_space(s_date_label, 1, 0);
    lv_obj_set_style_pad_all(s_date_label, 0, 0);
    lv_label_set_text(s_date_label, "2026/1/1");

    s_week_label = lv_label_create(root);
    lv_obj_set_style_text_font(s_week_label,
                               watch_language_is_chinese() ? &cn_font_20 : &lv_font_montserrat_20,
                               0);
    lv_obj_set_style_text_color(s_week_label, lv_color_white(), 0);
    lv_obj_set_style_text_align(s_week_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_letter_space(s_week_label, 1, 0);
    lv_obj_set_style_pad_all(s_week_label, 0, 0);
    lv_label_set_text(s_week_label, watch_language_is_chinese() ? "周四" : "THU");
    date_week_layout_update();

    update_time_ui();


    if(s_watch_face_timer == NULL) {
        s_watch_face_timer = lv_timer_create(watch_face_timer_cb, 250, NULL);
    }

    if(s_battery_timer == NULL) {
        s_battery_timer = lv_timer_create(battery_timer_cb, BATTERY_REFRESH_MS, NULL);
    }

    return root;
}

/**
 * @brief 创建主表盘页面。
 *
 * 详细说明：
 * - 加载封面、创建时间数字、日期、Wi-Fi 和电池 UI。
 */
void watch_face_create(void)
{
    lv_obj_t *scr = lv_obj_create(NULL);
    watch_face_create_on(scr);
    lv_scr_load(scr);
}

/**
 * @brief 设置表盘页面是否激活。
 *
 * 详细说明：
 * - 进入页面时启动/恢复刷新，离开时可暂停。
 *
 * @param active 输入或输出参数，具体含义见调用处和函数内部使用方式。
 */
void watch_face_set_active(bool active)
{
    if(s_watch_face_timer == NULL) {
        return;
    }

    if(active) {
        lv_timer_resume(s_watch_face_timer);
        if(s_battery_timer) {
            lv_timer_resume(s_battery_timer);
            lv_timer_ready(s_battery_timer);
        }


        lv_timer_ready(s_watch_face_timer);
    } else {
        lv_timer_pause(s_watch_face_timer);
        if(s_battery_timer) {
            lv_timer_pause(s_battery_timer);
        }
    }
}

/**
 * @brief 销毁主表盘页面。
 *
 * 详细说明：
 * - 删除定时器和 LVGL 对象，清空静态状态。
 */
void watch_face_destroy(void)
{
    if(s_watch_face_timer) {
        lv_timer_delete(s_watch_face_timer);
        s_watch_face_timer = NULL;
    }

    if(s_battery_timer) {
        lv_timer_delete(s_battery_timer);
        s_battery_timer = NULL;
    }

    digit_all_stop_anim();

    memset(s_digit, 0, sizeof(s_digit));
    s_date_label = NULL;
    s_week_label = NULL;
    s_battery_body = NULL;
    s_battery_fill = NULL;
    s_battery_label = NULL;
    s_wifi_label = NULL;
    s_wifi_state_valid_once = false;
    s_last_wifi_connected = false;
    s_last_sec = -1;
    s_last_yday = -1;
    s_last_date_language = watch_language_get();
    s_time_valid_once = false;
}
