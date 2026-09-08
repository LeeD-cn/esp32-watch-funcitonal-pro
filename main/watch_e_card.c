/**
 * @file watch_e_card.c
 * @brief Bilibili 电子名片页面及统计数据展示逻辑。
 */
/*
 * ==================== 模块说明 ====================
 *  * 模块职责：
 * - Bilibili 电子名片页面，展示二维码、播放量、点赞数和粉丝数。
 * - 二维码优先使用用户通过配置写入的图片，没有配置时回退到编译期内置资源。
 * - 统计数据通过 watch_bili_stats 后台任务回调刷新，避免页面主动阻塞网络请求。
 * - 页面按 240x240 屏幕做固定坐标布局，适合小尺寸圆角卡片视觉效果。
 *
 * 阅读建议：
 * - 先看本文件顶部的宏定义和静态状态变量，理解硬件参数和运行状态。
 * - 再看 reset/init/start/on_key/destroy 等对外函数，理解页面或驱动的生命周期。
 * - 最后看 static 辅助函数，了解具体寄存器读写、UI 刷新或状态机细节。
 * =======================================================
 */

#include "watch_e_card.h"
#include "stdlib/lv_mem.h"
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "watch_bili_stats.h"
#include "watch_config.h"

#define WATCH_SCREEN_W          240
#define WATCH_SCREEN_H          240
#define E_CARD_BG_COLOR         0xfb7299
#define E_CARD_BILIBILI_W        72
#define E_CARD_BILIBILI_H        32
#define E_CARD_BILIBILI_X        ((WATCH_SCREEN_W - E_CARD_BILIBILI_W) / 2)
#define E_CARD_BILIBILI_Y        3
#define E_CARD_WHITE_RECT_W      220
#define E_CARD_WHITE_RECT_H      180
#define E_CARD_WHITE_RECT_X       10
#define E_CARD_WHITE_RECT_Y       50
#define E_CARD_WHITE_RECT_RADIUS  12
#define E_CARD_QR_CODE_W           74
#define E_CARD_QR_CODE_H           74
#define E_CARD_QR_CODE_X           27
#define E_CARD_QR_CODE_Y           70
#define E_CARD_QR_FRAME_W          90
#define E_CARD_QR_FRAME_H          90
#define E_CARD_QR_FRAME_X          (E_CARD_QR_CODE_X - ((E_CARD_QR_FRAME_W - E_CARD_QR_CODE_W) / 2))
#define E_CARD_QR_FRAME_Y          (E_CARD_QR_CODE_Y - ((E_CARD_QR_FRAME_H - E_CARD_QR_CODE_H) / 2))
#define E_CARD_QR_FRAME_RADIUS      6
#define E_CARD_QR_FRAME_BORDER_W    3
#define E_CARD_STAT_ICON_W          30
#define E_CARD_STAT_ICON_H          30
#define E_CARD_STAT_ICON_GAP_X      16
#define E_CARD_STAT_ICON_GAP_Y      12
#define E_CARD_STAT_ICON_X          (E_CARD_QR_CODE_X + E_CARD_QR_CODE_W + E_CARD_STAT_ICON_GAP_X)-2
#define E_CARD_VIEWS_IMG_Y          E_CARD_QR_CODE_Y-5
#define E_CARD_LIKES_IMG_Y          (E_CARD_VIEWS_IMG_Y + E_CARD_STAT_ICON_H + E_CARD_STAT_ICON_GAP_Y)+10
#define E_CARD_SUBSCRIBERS_IMG_W    50
#define E_CARD_SUBSCRIBERS_IMG_H    50
#define E_CARD_SUBSCRIBERS_IMG_X    (E_CARD_QR_CODE_X + ((E_CARD_QR_CODE_W - E_CARD_SUBSCRIBERS_IMG_W) / 2))-20
#define E_CARD_SUBSCRIBERS_IMG_Y    (E_CARD_QR_CODE_Y + E_CARD_QR_CODE_H + 12)+15
#define E_CARD_STAT_DIVIDER_W       100
#define E_CARD_STAT_DIVIDER_H         1
#define E_CARD_STAT_DIVIDER_X       E_CARD_STAT_ICON_X
#define E_CARD_STAT_DIVIDER_Y       (E_CARD_VIEWS_IMG_Y + E_CARD_STAT_ICON_H + ((E_CARD_LIKES_IMG_Y - (E_CARD_VIEWS_IMG_Y + E_CARD_STAT_ICON_H)) / 2))
#define E_CARD_QR_SUB_DIVIDER_W     200
#define E_CARD_QR_SUB_DIVIDER_H       1
#define E_CARD_QR_SUB_DIVIDER_X     ((WATCH_SCREEN_W - E_CARD_QR_SUB_DIVIDER_W) / 2)
#define E_CARD_QR_SUB_DIVIDER_Y     (E_CARD_QR_CODE_Y + E_CARD_QR_CODE_H + ((E_CARD_SUBSCRIBERS_IMG_Y - (E_CARD_QR_CODE_Y + E_CARD_QR_CODE_H)) / 2))+5
#define E_CARD_DIVIDER_OPA          LV_OPA_20

LV_IMG_DECLARE(bilibili_img);
LV_IMG_DECLARE(QR_code);
LV_IMG_DECLARE(views_img);
LV_IMG_DECLARE(likes_img);
LV_IMG_DECLARE(subscribers_img);

/**
 * @brief 获取二维码图片源，优先使用用户配置图片。
 */
static const void *e_card_get_qr_src(void)
{
    const lv_img_dsc_t *cfg_qr = watch_config_get_qr_image();
    return cfg_qr != NULL ? (const void *)cfg_qr : (const void *)&QR_code;
}


/**
 * @brief 电子名片页面对象集合。
 */
typedef struct {
    lv_obj_t *page;
    lv_obj_t *white_rect;
    lv_obj_t *qr_code;
    lv_obj_t *qr_frame;
    lv_obj_t *views_img;
    lv_obj_t *views_label;
    lv_obj_t *likes_img;
    lv_obj_t *likes_label;
    lv_obj_t *subscribers_img;
    lv_obj_t *subscribers_label;
    lv_obj_t *stat_divider;
    lv_obj_t *qr_sub_divider;
    lv_obj_t *bilibili;
    bool wants_back;
} watch_e_card_ctx_t;

static watch_e_card_ctx_t s_e_card;

/**
 * @brief 安全设置电子名片标签文本。
 *
 * 详细说明：
 * - 对象为空时直接返回，避免页面销毁后异步回调崩溃。
 *
 * @param label 输入或输出参数，具体含义见调用处和函数内部使用方式。
 * @param text 输入或输出参数，具体含义见调用处和函数内部使用方式。
 */
static void e_card_set_label_text(lv_obj_t *label, const char *text)
{
    if(label == NULL || text == NULL) {
        return;
    }

    lv_label_set_text(label, text);
}

/**
 * @brief 格式化统计数字。
 *
 * 详细说明：
 * - 把播放/点赞/粉丝数转换为适合小屏展示的短字符串。
 *
 * @param value 输入或输出参数，具体含义见调用处和函数内部使用方式。
 * @param buf 输入或输出参数，具体含义见调用处和函数内部使用方式。
 * @param buf_size 输入或输出参数，具体含义见调用处和函数内部使用方式。
 */
static void e_card_format_stat_count(unsigned long long value, char *buf, size_t buf_size)
{
    if(buf == NULL || buf_size == 0) {
        return;
    }

    if(value >= 1000000ULL) {
        snprintf(buf, buf_size, "%lluw", value / 10000ULL);
        return;
    }

    watch_bili_stats_format_short(value, buf, buf_size);
}

/**
 * @brief Bilibili 统计异步刷新消息。
 */
typedef struct {
    watch_bili_stats_t stats;
} e_card_stats_async_t;

/**
 * @brief 在 LVGL 线程中应用统计数据。
 *
 * 详细说明：
 * - 由异步回调触发，确保 UI 更新发生在安全上下文。
 *
 * @param param 输入或输出参数，具体含义见调用处和函数内部使用方式。
 */
static void e_card_apply_stats_async(void *param)
{
    e_card_stats_async_t *msg = (e_card_stats_async_t *)param;
    if(msg == NULL) {
        return;
    }

    if(s_e_card.page != NULL && msg->stats.valid) {
        char buf[32];

        e_card_format_stat_count((unsigned long long)msg->stats.views, buf, sizeof(buf));
        e_card_set_label_text(s_e_card.views_label, buf);

        e_card_format_stat_count((unsigned long long)msg->stats.likes, buf, sizeof(buf));
        e_card_set_label_text(s_e_card.likes_label, buf);

        snprintf(buf, sizeof(buf), "%llu", (unsigned long long)msg->stats.subscribers);
        e_card_set_label_text(s_e_card.subscribers_label, buf);
    }

    lv_free(msg);
}

/**
 * @brief Bilibili 统计数据回调，切换到 LVGL 线程更新 UI。
 *
 * 详细说明：
 * - 后台任务拿到新数据后，通过该回调通知页面。
 *
 * @param stats 输入或输出参数，具体含义见函数内部使用方式。
 * @param user_data 输入或输出参数，具体含义见函数内部使用方式。
 */
static void e_card_bili_stats_cb(const watch_bili_stats_t *stats, void *user_data)
{
    (void)user_data;

    if(stats == NULL || !stats->valid) {
        return;
    }

    e_card_stats_async_t *msg = lv_malloc(sizeof(*msg));
    if(msg == NULL) {
        return;
    }

    msg->stats = *stats;
    lv_async_call(e_card_apply_stats_async, msg);
}


void watch_e_card_destroy(void);

/**
 * @brief watch_e_card_create 对外接口。
 *
 * 详细说明：
 * - 供其他模块调用，隐藏本文件内部状态细节。
 *
 * @param parent 输入或输出参数，具体含义见调用处和函数内部使用方式。
 *
 * @return 函数执行结果或计算得到的值，具体语义见返回路径。
 */
lv_obj_t *watch_e_card_create(lv_obj_t *parent)
{
    if(s_e_card.page != NULL) {
        watch_e_card_destroy();
    }

    memset(&s_e_card, 0, sizeof(s_e_card));

    s_e_card.page = lv_obj_create(parent);
    if(s_e_card.page == NULL) {
        return NULL;
    }

    lv_obj_remove_style_all(s_e_card.page);
    lv_obj_set_size(s_e_card.page, WATCH_SCREEN_W, WATCH_SCREEN_H);
    lv_obj_set_pos(s_e_card.page, 0, 0);
    lv_obj_set_style_bg_color(s_e_card.page, lv_color_hex(E_CARD_BG_COLOR), 0);
    lv_obj_set_style_bg_opa(s_e_card.page, LV_OPA_COVER, 0);
    lv_obj_clear_flag(s_e_card.page, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(s_e_card.page, LV_OBJ_FLAG_CLICKABLE);

    s_e_card.white_rect = lv_obj_create(s_e_card.page);
    if(s_e_card.white_rect) {
        lv_obj_remove_style_all(s_e_card.white_rect);
        lv_obj_set_size(s_e_card.white_rect, E_CARD_WHITE_RECT_W, E_CARD_WHITE_RECT_H);
        lv_obj_set_pos(s_e_card.white_rect, E_CARD_WHITE_RECT_X, E_CARD_WHITE_RECT_Y);
        lv_obj_set_style_bg_color(s_e_card.white_rect, lv_color_hex(0xffffff), 0);
        lv_obj_set_style_bg_opa(s_e_card.white_rect, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(s_e_card.white_rect, E_CARD_WHITE_RECT_RADIUS, 0);
        lv_obj_clear_flag(s_e_card.white_rect, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_clear_flag(s_e_card.white_rect, LV_OBJ_FLAG_CLICKABLE);
    }

    s_e_card.qr_code = lv_img_create(s_e_card.page);
    if(s_e_card.qr_code) {
        lv_img_set_src(s_e_card.qr_code, e_card_get_qr_src());
        lv_obj_set_size(s_e_card.qr_code, E_CARD_QR_CODE_W, E_CARD_QR_CODE_H);
        lv_obj_set_pos(s_e_card.qr_code, E_CARD_QR_CODE_X, E_CARD_QR_CODE_Y);
        lv_obj_clear_flag(s_e_card.qr_code, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_clear_flag(s_e_card.qr_code, LV_OBJ_FLAG_CLICKABLE);
    }

    s_e_card.qr_frame = lv_obj_create(s_e_card.page);
    if(s_e_card.qr_frame) {
        lv_obj_remove_style_all(s_e_card.qr_frame);
        lv_obj_set_size(s_e_card.qr_frame, E_CARD_QR_FRAME_W, E_CARD_QR_FRAME_H);
        lv_obj_set_pos(s_e_card.qr_frame, E_CARD_QR_FRAME_X, E_CARD_QR_FRAME_Y);
        lv_obj_set_style_bg_opa(s_e_card.qr_frame, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_color(s_e_card.qr_frame, lv_color_hex(E_CARD_BG_COLOR), 0);
        lv_obj_set_style_border_opa(s_e_card.qr_frame, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(s_e_card.qr_frame, E_CARD_QR_FRAME_BORDER_W, 0);
        lv_obj_set_style_radius(s_e_card.qr_frame, E_CARD_QR_FRAME_RADIUS, 0);
        lv_obj_clear_flag(s_e_card.qr_frame, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_clear_flag(s_e_card.qr_frame, LV_OBJ_FLAG_CLICKABLE);
    }


    s_e_card.views_img = lv_img_create(s_e_card.page);
    if(s_e_card.views_img) {
        lv_img_set_src(s_e_card.views_img, &views_img);
        lv_obj_set_size(s_e_card.views_img, E_CARD_STAT_ICON_W, E_CARD_STAT_ICON_H);
        lv_obj_set_pos(s_e_card.views_img, E_CARD_STAT_ICON_X, E_CARD_VIEWS_IMG_Y);
        lv_obj_clear_flag(s_e_card.views_img, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_clear_flag(s_e_card.views_img, LV_OBJ_FLAG_CLICKABLE);
    }

    s_e_card.views_label = lv_label_create(s_e_card.page);
    if(s_e_card.views_label) {
        lv_label_set_text(s_e_card.views_label, "--");
        lv_obj_set_style_text_color(s_e_card.views_label, lv_color_hex(0x333333), 0);
        lv_obj_set_style_text_font(s_e_card.views_label, &lv_font_montserrat_14, 0);
        lv_obj_set_pos(s_e_card.views_label, E_CARD_STAT_ICON_X + E_CARD_STAT_ICON_W + 4, E_CARD_VIEWS_IMG_Y + 1);
        lv_obj_clear_flag(s_e_card.views_label, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_clear_flag(s_e_card.views_label, LV_OBJ_FLAG_CLICKABLE);
    }

    s_e_card.stat_divider = lv_obj_create(s_e_card.page);
    if(s_e_card.stat_divider) {
        lv_obj_remove_style_all(s_e_card.stat_divider);
        lv_obj_set_size(s_e_card.stat_divider, E_CARD_STAT_DIVIDER_W, E_CARD_STAT_DIVIDER_H);
        lv_obj_set_pos(s_e_card.stat_divider, E_CARD_STAT_DIVIDER_X, E_CARD_STAT_DIVIDER_Y);
        lv_obj_set_style_bg_color(s_e_card.stat_divider, lv_color_hex(0x000000), 0);
        lv_obj_set_style_bg_opa(s_e_card.stat_divider, E_CARD_DIVIDER_OPA, 0);
        lv_obj_clear_flag(s_e_card.stat_divider, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_clear_flag(s_e_card.stat_divider, LV_OBJ_FLAG_CLICKABLE);
    }

    s_e_card.likes_img = lv_img_create(s_e_card.page);
    if(s_e_card.likes_img) {
        lv_img_set_src(s_e_card.likes_img, &likes_img);
        lv_obj_set_size(s_e_card.likes_img, E_CARD_STAT_ICON_W, E_CARD_STAT_ICON_H);
        lv_obj_set_pos(s_e_card.likes_img, E_CARD_STAT_ICON_X, E_CARD_LIKES_IMG_Y);
        lv_obj_clear_flag(s_e_card.likes_img, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_clear_flag(s_e_card.likes_img, LV_OBJ_FLAG_CLICKABLE);
    }

    s_e_card.likes_label = lv_label_create(s_e_card.page);
    if(s_e_card.likes_label) {
        lv_label_set_text(s_e_card.likes_label, "--");
        lv_obj_set_style_text_color(s_e_card.likes_label, lv_color_hex(0x333333), 0);
        lv_obj_set_style_text_font(s_e_card.likes_label, &lv_font_montserrat_14, 0);
        lv_obj_set_pos(s_e_card.likes_label, E_CARD_STAT_ICON_X + E_CARD_STAT_ICON_W + 4, E_CARD_LIKES_IMG_Y + 1);
        lv_obj_clear_flag(s_e_card.likes_label, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_clear_flag(s_e_card.likes_label, LV_OBJ_FLAG_CLICKABLE);
    }

    s_e_card.qr_sub_divider = lv_obj_create(s_e_card.page);
    if(s_e_card.qr_sub_divider) {
        lv_obj_remove_style_all(s_e_card.qr_sub_divider);
        lv_obj_set_size(s_e_card.qr_sub_divider, E_CARD_QR_SUB_DIVIDER_W, E_CARD_QR_SUB_DIVIDER_H);
        lv_obj_set_pos(s_e_card.qr_sub_divider, E_CARD_QR_SUB_DIVIDER_X, E_CARD_QR_SUB_DIVIDER_Y);
        lv_obj_set_style_bg_color(s_e_card.qr_sub_divider, lv_color_hex(0x000000), 0);
        lv_obj_set_style_bg_opa(s_e_card.qr_sub_divider, E_CARD_DIVIDER_OPA, 0);
        lv_obj_clear_flag(s_e_card.qr_sub_divider, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_clear_flag(s_e_card.qr_sub_divider, LV_OBJ_FLAG_CLICKABLE);
    }

    s_e_card.subscribers_img = lv_img_create(s_e_card.page);
    if(s_e_card.subscribers_img) {
        lv_img_set_src(s_e_card.subscribers_img, &subscribers_img);
        lv_obj_set_size(s_e_card.subscribers_img, E_CARD_SUBSCRIBERS_IMG_W, E_CARD_SUBSCRIBERS_IMG_H);
        lv_obj_set_pos(s_e_card.subscribers_img, E_CARD_SUBSCRIBERS_IMG_X, E_CARD_SUBSCRIBERS_IMG_Y);
        lv_obj_clear_flag(s_e_card.subscribers_img, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_clear_flag(s_e_card.subscribers_img, LV_OBJ_FLAG_CLICKABLE);
    }

    s_e_card.subscribers_label = lv_label_create(s_e_card.page);
    if(s_e_card.subscribers_label) {
        lv_label_set_text(s_e_card.subscribers_label, "--");
        lv_obj_set_style_text_color(s_e_card.subscribers_label, lv_color_hex(0x333333), 0);
        lv_obj_set_style_text_font(s_e_card.subscribers_label, &lv_font_montserrat_36, 0);
        lv_obj_set_pos(s_e_card.subscribers_label, E_CARD_SUBSCRIBERS_IMG_X + E_CARD_SUBSCRIBERS_IMG_W + 8, E_CARD_SUBSCRIBERS_IMG_Y + 5);
        lv_obj_clear_flag(s_e_card.subscribers_label, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_clear_flag(s_e_card.subscribers_label, LV_OBJ_FLAG_CLICKABLE);
    }

    s_e_card.bilibili = lv_img_create(s_e_card.page);
    lv_img_set_src(s_e_card.bilibili, &bilibili_img);
    lv_obj_set_size(s_e_card.bilibili, E_CARD_BILIBILI_W, E_CARD_BILIBILI_H);
    lv_obj_set_pos(s_e_card.bilibili, E_CARD_BILIBILI_X, E_CARD_BILIBILI_Y);
    lv_obj_clear_flag(s_e_card.bilibili, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(s_e_card.bilibili, LV_OBJ_FLAG_CLICKABLE);

    s_e_card.wants_back = false;

    watch_bili_stats_t cached;
    if(watch_bili_stats_get_cached(&cached) == ESP_OK) {
        e_card_bili_stats_cb(&cached, NULL);
    }
    watch_bili_stats_start(e_card_bili_stats_cb, NULL);

    return s_e_card.page;
}

/**
 * @brief 创建或重置电子名片页面。
 *
 * 详细说明：
 * - 初始化背景、白色卡片、二维码和统计图标/文本。
 */
void watch_e_card_reset(void)
{
    if(s_e_card.page == NULL) {
        return;
    }

    s_e_card.wants_back = false;
    lv_obj_set_style_bg_color(s_e_card.page, lv_color_hex(E_CARD_BG_COLOR), 0);
    lv_obj_set_style_bg_opa(s_e_card.page, LV_OPA_COVER, 0);

    if(s_e_card.white_rect) {
        lv_obj_set_size(s_e_card.white_rect, E_CARD_WHITE_RECT_W, E_CARD_WHITE_RECT_H);
        lv_obj_set_pos(s_e_card.white_rect, E_CARD_WHITE_RECT_X, E_CARD_WHITE_RECT_Y);
        lv_obj_set_style_bg_color(s_e_card.white_rect, lv_color_hex(0xffffff), 0);
        lv_obj_set_style_bg_opa(s_e_card.white_rect, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(s_e_card.white_rect, E_CARD_WHITE_RECT_RADIUS, 0);
        lv_obj_clear_flag(s_e_card.white_rect, LV_OBJ_FLAG_HIDDEN);
    }

    if(s_e_card.qr_code) {
        lv_img_set_src(s_e_card.qr_code, e_card_get_qr_src());
        lv_obj_set_size(s_e_card.qr_code, E_CARD_QR_CODE_W, E_CARD_QR_CODE_H);
        lv_obj_set_pos(s_e_card.qr_code, E_CARD_QR_CODE_X, E_CARD_QR_CODE_Y);
        lv_obj_clear_flag(s_e_card.qr_code, LV_OBJ_FLAG_HIDDEN);
    }

    if(s_e_card.qr_frame) {
        lv_obj_set_size(s_e_card.qr_frame, E_CARD_QR_FRAME_W, E_CARD_QR_FRAME_H);
        lv_obj_set_pos(s_e_card.qr_frame, E_CARD_QR_FRAME_X, E_CARD_QR_FRAME_Y);
        lv_obj_set_style_bg_opa(s_e_card.qr_frame, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_color(s_e_card.qr_frame, lv_color_hex(E_CARD_BG_COLOR), 0);
        lv_obj_set_style_border_opa(s_e_card.qr_frame, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(s_e_card.qr_frame, E_CARD_QR_FRAME_BORDER_W, 0);
        lv_obj_set_style_radius(s_e_card.qr_frame, E_CARD_QR_FRAME_RADIUS, 0);
        lv_obj_clear_flag(s_e_card.qr_frame, LV_OBJ_FLAG_HIDDEN);
    }


    if(s_e_card.views_img) {
        lv_img_set_src(s_e_card.views_img, &views_img);
        lv_obj_set_size(s_e_card.views_img, E_CARD_STAT_ICON_W, E_CARD_STAT_ICON_H);
        lv_obj_set_pos(s_e_card.views_img, E_CARD_STAT_ICON_X, E_CARD_VIEWS_IMG_Y);
        lv_obj_clear_flag(s_e_card.views_img, LV_OBJ_FLAG_HIDDEN);
    }

    if(s_e_card.views_label) {
        lv_obj_set_style_text_font(s_e_card.views_label, &lv_font_montserrat_26, 0);
        lv_obj_set_pos(s_e_card.views_label, E_CARD_STAT_ICON_X + E_CARD_STAT_ICON_W + 4, E_CARD_VIEWS_IMG_Y + 1);
        lv_obj_clear_flag(s_e_card.views_label, LV_OBJ_FLAG_HIDDEN);
    }

    if(s_e_card.stat_divider) {
        lv_obj_set_size(s_e_card.stat_divider, E_CARD_STAT_DIVIDER_W, E_CARD_STAT_DIVIDER_H);
        lv_obj_set_pos(s_e_card.stat_divider, E_CARD_STAT_DIVIDER_X, E_CARD_STAT_DIVIDER_Y);
        lv_obj_set_style_bg_color(s_e_card.stat_divider, lv_color_hex(0x000000), 0);
        lv_obj_set_style_bg_opa(s_e_card.stat_divider, E_CARD_DIVIDER_OPA, 0);
        lv_obj_clear_flag(s_e_card.stat_divider, LV_OBJ_FLAG_HIDDEN);
    }

    if(s_e_card.likes_img) {
        lv_img_set_src(s_e_card.likes_img, &likes_img);
        lv_obj_set_size(s_e_card.likes_img, E_CARD_STAT_ICON_W, E_CARD_STAT_ICON_H);
        lv_obj_set_pos(s_e_card.likes_img, E_CARD_STAT_ICON_X, E_CARD_LIKES_IMG_Y);
        lv_obj_clear_flag(s_e_card.likes_img, LV_OBJ_FLAG_HIDDEN);
    }

    if(s_e_card.likes_label) {
        lv_obj_set_style_text_font(s_e_card.likes_label, &lv_font_montserrat_26, 0);
        lv_obj_set_pos(s_e_card.likes_label, E_CARD_STAT_ICON_X + E_CARD_STAT_ICON_W + 4, E_CARD_LIKES_IMG_Y + 1);
        lv_obj_clear_flag(s_e_card.likes_label, LV_OBJ_FLAG_HIDDEN);
    }

    if(s_e_card.qr_sub_divider) {
        lv_obj_set_size(s_e_card.qr_sub_divider, E_CARD_QR_SUB_DIVIDER_W, E_CARD_QR_SUB_DIVIDER_H);
        lv_obj_set_pos(s_e_card.qr_sub_divider, E_CARD_QR_SUB_DIVIDER_X, E_CARD_QR_SUB_DIVIDER_Y);
        lv_obj_set_style_bg_color(s_e_card.qr_sub_divider, lv_color_hex(0x000000), 0);
        lv_obj_set_style_bg_opa(s_e_card.qr_sub_divider, E_CARD_DIVIDER_OPA, 0);
        lv_obj_clear_flag(s_e_card.qr_sub_divider, LV_OBJ_FLAG_HIDDEN);
    }

    if(s_e_card.subscribers_img) {
        lv_img_set_src(s_e_card.subscribers_img, &subscribers_img);
        lv_obj_set_size(s_e_card.subscribers_img, E_CARD_SUBSCRIBERS_IMG_W, E_CARD_SUBSCRIBERS_IMG_H);
        lv_obj_set_pos(s_e_card.subscribers_img, E_CARD_SUBSCRIBERS_IMG_X, E_CARD_SUBSCRIBERS_IMG_Y);
        lv_obj_clear_flag(s_e_card.subscribers_img, LV_OBJ_FLAG_HIDDEN);
    }

    if(s_e_card.subscribers_label) {
        lv_obj_set_style_text_font(s_e_card.subscribers_label, &lv_font_montserrat_36, 0);
        lv_obj_set_pos(s_e_card.subscribers_label, E_CARD_SUBSCRIBERS_IMG_X + E_CARD_SUBSCRIBERS_IMG_W + 8, E_CARD_SUBSCRIBERS_IMG_Y + 5);
        lv_obj_clear_flag(s_e_card.subscribers_label, LV_OBJ_FLAG_HIDDEN);
    }

    if(s_e_card.bilibili) {
        lv_obj_set_pos(s_e_card.bilibili, E_CARD_BILIBILI_X, E_CARD_BILIBILI_Y);
        lv_obj_clear_flag(s_e_card.bilibili, LV_OBJ_FLAG_HIDDEN);
    }

    lv_obj_invalidate(s_e_card.page);
}

/**
 * @brief 处理电子名片页面按键。
 *
 * 详细说明：
 * - 通常按返回键设置 wants_back。
 *
 * @param key 输入或输出参数，具体含义见调用处和函数内部使用方式。
 */
void watch_e_card_on_key(watch_key_t key)
{
    if(s_e_card.page == NULL) {
        return;
    }

    if(key == WATCH_KEY_2) {
        s_e_card.wants_back = true;
    }
}

/**
 * @brief 查询电子名片页面是否请求返回。
 *
 * 详细说明：
 * - 供上层 UI 状态机使用。
 *
 * @return 函数执行结果或计算得到的值，具体语义见返回路径。
 */
bool watch_e_card_wants_back(void)
{
    return s_e_card.wants_back;
}

/**
 * @brief 销毁电子名片页面。
 *
 * 详细说明：
 * - 删除 LVGL 对象并清理页面上下文。
 */
void watch_e_card_destroy(void)
{
    if(s_e_card.page) {
        lv_obj_del(s_e_card.page);
        s_e_card.page = NULL;
    }

    memset(&s_e_card, 0, sizeof(s_e_card));
}
