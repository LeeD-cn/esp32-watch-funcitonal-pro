/**
 * @file watch_game.c
 * @brief 游戏入口页以及飞机大战、结算页、历史分数页之间的状态切换。
 */
/*
 * ==================== 模块说明 ====================
 *  * 模块职责：
 * - 游戏中心入口页，负责在游戏菜单、飞机大战、结算页、历史分数页之间切换。
 * - 本文件主要维护页面状态机，不实现飞机大战具体玩法。
 * - 按键事件会根据当前模式转发给对应子页面，子页面通过 wants_* 标志把状态变化反馈给游戏中心。
 * - 结算分数会暂存在游戏中心，再传递给历史分数页面保存。
 *
 * 阅读建议：
 * - 先看本文件顶部的宏定义和静态状态变量，理解硬件参数和运行状态。
 * - 再看 reset/init/start/on_key/destroy 等对外函数，理解页面或驱动的生命周期。
 * - 最后看 static 辅助函数，了解具体寄存器读写、UI 刷新或状态机细节。
 * =======================================================
 */

#include "watch_game.h"
#include "watch_plane_war.h"
#include "watch_plane_war_over.h"
#include "watch_plane_war_scores.h"
#include "watch_language.h"

#include <stdbool.h>
#include <string.h>

#define WATCH_SCREEN_W      240
#define WATCH_SCREEN_H      240

#define GAME_TOP_Y          8
#define GAME_BACK_X         12
#define GAME_SELECTOR_PAD_X     7
#define GAME_SELECTOR_PAD_Y     4
#define GAME_SELECTOR_RADIUS    8
#define GAME_SELECTOR_BORDER_W  2
#define GAME_SELECTOR_ANIM_MS   180

#define GAME_PLANE_BG_X     24
#define GAME_PLANE_BG_Y     68
#define GAME_PLANE_BG_W     192
#define GAME_PLANE_BG_H     44
#define GAME_PLANE_BG_R     10

LV_FONT_DECLARE(cn_font_26);

/**
 * @brief 游戏菜单可聚焦项。
 */
typedef enum {
    GAME_FOCUS_BACK = 0,
    GAME_FOCUS_PLANE_WAR,
} game_focus_t;

/**
 * @brief 游戏中心当前页面模式。
 */
typedef enum {
    GAME_MODE_MENU = 0,
    GAME_MODE_PLANE_WAR,
    GAME_MODE_PLANE_WAR_OVER,
    GAME_MODE_PLANE_WAR_SCORES,
} game_mode_t;

/**
 * @brief 游戏中心页面上下文。
 */
typedef struct {
    lv_obj_t *page;
    lv_obj_t *back_btn;
    lv_obj_t *title;
    lv_obj_t *cursor;

    lv_obj_t *plane_bg;
    lv_obj_t *plane_label;

    lv_obj_t *plane_war_page;
    lv_obj_t *plane_war_over_page;
    lv_obj_t *plane_war_scores_page;

    int pending_plane_war_score;
    bool has_pending_plane_war_score;

    game_focus_t focus;
    game_mode_t mode;
    bool wants_back;
} game_ctx_t;

/* 游戏中心页面的全局上下文。 */
static game_ctx_t s_game;

/**
 * @brief 获取游戏菜单标题字体。
 *
 * 详细说明：
 * - 中文使用 cn_font_26，英文使用 Montserrat。
 *
 * @return 函数执行结果或计算得到的值，具体语义见返回路径。
 */
static const lv_font_t *game_i18n_font_26(void)
{
    return watch_language_is_chinese() ? &cn_font_26 : &lv_font_montserrat_26;
}

/**
 * @brief 刷新游戏中心语言文本。
 *
 * 详细说明：
 * - 更新标题和飞机大战入口文字。
 */
static void game_apply_language(void)
{
    if(s_game.title) {
        lv_obj_set_style_text_font(s_game.title, game_i18n_font_26(), 0);
        lv_label_set_text(s_game.title, watch_language_is_chinese() ? "游戏" : "Game");
        lv_obj_align(s_game.title, LV_ALIGN_TOP_MID, 0, GAME_TOP_Y);
    }

    if(s_game.plane_label) {
        lv_obj_set_style_text_font(s_game.plane_label, game_i18n_font_26(), 0);
        lv_label_set_text(s_game.plane_label, watch_language_is_chinese() ? "飞机大战" : "Plane War");
        lv_obj_center(s_game.plane_label);
    }
}

/**
 * @brief 设置游戏菜单标签基础样式。
 *
 * 详细说明：
 * - 统一字体、白色文字和居中对齐。
 *
 * @param label 输入或输出参数，具体含义见调用处和函数内部使用方式。
 */
static void game_label_style_26(lv_obj_t *label)
{
    lv_obj_set_style_text_font(label, &lv_font_montserrat_26, 0);
    lv_obj_set_style_text_color(label, lv_color_white(), 0);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_clear_flag(label, LV_OBJ_FLAG_CLICKABLE);
}

/**
 * @brief 根据焦点枚举返回对应 LVGL 对象。
 *
 * 详细说明：
 * - 供选择框定位时获取目标对象尺寸和坐标。
 *
 * @param focus 输入或输出参数，具体含义见调用处和函数内部使用方式。
 *
 * @return 函数执行结果或计算得到的值，具体语义见返回路径。
 */
static lv_obj_t *game_obj_from_focus(game_focus_t focus)
{
    switch(focus) {
    case GAME_FOCUS_BACK:
        return s_game.back_btn;

    case GAME_FOCUS_PLANE_WAR:
        return s_game.plane_label;

    default:
        return NULL;
    }
}

/**
 * @brief 选择框 X 坐标动画回调。
 *
 * 详细说明：
 * - LVGL 动画逐帧设置对象 x。
 *
 * @param var 输入或输出参数，具体含义见调用处和函数内部使用方式。
 * @param v 输入或输出参数，具体含义见调用处和函数内部使用方式。
 */
static void game_cursor_x_anim_cb(void *var, int32_t v)
{
    lv_obj_set_x((lv_obj_t *)var, v);
}

/**
 * @brief 选择框 Y 坐标动画回调。
 *
 * 详细说明：
 * - LVGL 动画逐帧设置对象 y。
 *
 * @param var 输入或输出参数，具体含义见调用处和函数内部使用方式。
 * @param v 输入或输出参数，具体含义见调用处和函数内部使用方式。
 */
static void game_cursor_y_anim_cb(void *var, int32_t v)
{
    lv_obj_set_y((lv_obj_t *)var, v);
}

/**
 * @brief 选择框宽度动画回调。
 *
 * 详细说明：
 * - 用于焦点切换时平滑改变选择框大小。
 *
 * @param var 输入或输出参数，具体含义见调用处和函数内部使用方式。
 * @param v 输入或输出参数，具体含义见调用处和函数内部使用方式。
 */
static void game_cursor_w_anim_cb(void *var, int32_t v)
{
    lv_obj_set_width((lv_obj_t *)var, v);
}

/**
 * @brief 选择框高度动画回调。
 *
 * 详细说明：
 * - 用于焦点切换时平滑改变选择框大小。
 *
 * @param var 输入或输出参数，具体含义见调用处和函数内部使用方式。
 * @param v 输入或输出参数，具体含义见调用处和函数内部使用方式。
 */
static void game_cursor_h_anim_cb(void *var, int32_t v)
{
    lv_obj_set_height((lv_obj_t *)var, v);
}

/**
 * @brief 启动选择框属性动画。
 *
 * 详细说明：
 * - 把当前焦点框平滑移动/缩放到目标对象周围。
 *
 * @param obj 输入或输出参数，具体含义见调用处和函数内部使用方式。
 * @param exec_cb 输入或输出参数，具体含义见调用处和函数内部使用方式。
 * @param from 输入或输出参数，具体含义见调用处和函数内部使用方式。
 * @param to 输入或输出参数，具体含义见调用处和函数内部使用方式。
 */
static void game_cursor_start_anim(lv_obj_t *obj,
                                   lv_anim_exec_xcb_t exec_cb,
                                   int32_t from,
                                   int32_t to)
{
    lv_anim_t a;

    lv_anim_init(&a);
    lv_anim_set_var(&a, obj);
    lv_anim_set_values(&a, from, to);
    lv_anim_set_duration(&a, GAME_SELECTOR_ANIM_MS);
    lv_anim_set_exec_cb(&a, exec_cb);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_start(&a);
}

/**
 * @brief 根据当前焦点更新选择框位置和尺寸。
 *
 * 详细说明：
 * - 根据当前焦点计算选择框位置和尺寸。
 *
 * @param animated 输入或输出参数，具体含义见函数内部使用方式。
 */
static void game_cursor_update(bool animated)
{
    if(s_game.page == NULL || s_game.cursor == NULL) {
        return;
    }

    lv_obj_t *target = game_obj_from_focus(s_game.focus);
    if(target == NULL) {
        return;
    }

    lv_obj_update_layout(s_game.page);

    lv_coord_t x = lv_obj_get_x(target);
    lv_coord_t y = lv_obj_get_y(target);
    lv_coord_t w = lv_obj_get_width(target);
    lv_coord_t h = lv_obj_get_height(target);


    if(s_game.focus == GAME_FOCUS_PLANE_WAR && s_game.plane_bg) {
        x += lv_obj_get_x(s_game.plane_bg);
        y += lv_obj_get_y(s_game.plane_bg);
    }

    x -= GAME_SELECTOR_PAD_X;
    y -= GAME_SELECTOR_PAD_Y;
    w += GAME_SELECTOR_PAD_X * 2;
    h += GAME_SELECTOR_PAD_Y * 2;

    if(w < 16 + GAME_SELECTOR_PAD_X * 2) {
        w = 16 + GAME_SELECTOR_PAD_X * 2;
    }

    if(!animated || lv_obj_get_width(s_game.cursor) <= 0 || lv_obj_get_height(s_game.cursor) <= 0) {
        lv_obj_set_pos(s_game.cursor, x, y);
        lv_obj_set_size(s_game.cursor, w, h);
    }
    else {
        game_cursor_start_anim(s_game.cursor, game_cursor_x_anim_cb, lv_obj_get_x(s_game.cursor), x);
        game_cursor_start_anim(s_game.cursor, game_cursor_y_anim_cb, lv_obj_get_y(s_game.cursor), y);
        game_cursor_start_anim(s_game.cursor, game_cursor_w_anim_cb, lv_obj_get_width(s_game.cursor), w);
        game_cursor_start_anim(s_game.cursor, game_cursor_h_anim_cb, lv_obj_get_height(s_game.cursor), h);
    }

    lv_obj_move_foreground(s_game.cursor);
}

/**
 * @brief 更新游戏菜单选中状态。
 *
 * 详细说明：
 * - 处理焦点变化后的视觉刷新。
 *
 * @param animated 输入或输出参数，具体含义见调用处和函数内部使用方式。
 */
static void game_selection_update(bool animated)
{
    if(s_game.plane_bg) {
        lv_obj_set_style_bg_opa(s_game.plane_bg, LV_OPA_30, 0);
    }

    if(s_game.cursor) {
        lv_obj_clear_flag(s_game.cursor, LV_OBJ_FLAG_HIDDEN);
    }

    game_cursor_update(animated);
}

/**
 * @brief 销毁飞机大战结算子页面。
 *
 * 详细说明：
 * - 切换模式前释放不再显示的页面。
 */
static void game_destroy_plane_war_over(void)
{
    if(s_game.plane_war_over_page) {
        watch_plane_war_over_destroy();
        s_game.plane_war_over_page = NULL;
    }
}

/**
 * @brief 销毁飞机大战分数子页面。
 *
 * 详细说明：
 * - 避免历史分数页面残留在对象树中。
 */
static void game_destroy_plane_war_scores(void)
{
    if(s_game.plane_war_scores_page) {
        watch_plane_war_scores_destroy();
        s_game.plane_war_scores_page = NULL;
    }
}

/**
 * @brief 暂存待提交的飞机大战分数。
 *
 * 详细说明：
 * - 游戏结束回调中记录分数，稍后进入结算/分数页使用。
 */
static void game_store_pending_plane_war_score(void)
{
    if(!s_game.has_pending_plane_war_score) {
        return;
    }

    watch_plane_war_scores_submit_score(s_game.pending_plane_war_score);
    s_game.pending_plane_war_score = 0;
    s_game.has_pending_plane_war_score = false;
}

static void game_show_plane_war_over_mode(void);
static void game_show_plane_war_scores_mode(void);

/**
 * @brief 异步切换到飞机大战结算页。
 *
 * 详细说明：
 * - 从游戏结束回调安全地切换 UI，避免在 timer 中直接改复杂页面。
 *
 * @param user_data 输入或输出参数，具体含义见调用处和函数内部使用方式。
 */
static void game_show_plane_war_over_async(void *user_data)
{
    (void)user_data;

    if(s_game.page == NULL) {
        return;
    }

    game_show_plane_war_over_mode();
}

/**
 * @brief Plane War 结束回调，保存分数并异步切换到结算页。
 *
 * 详细说明：
 * - 由 watch_plane_war.c 调用，通知游戏中心进入结算流程。
 */
static void game_plane_war_over_cb(void)
{


    s_game.pending_plane_war_score = watch_plane_war_get_score();
    s_game.has_pending_plane_war_score = true;
    lv_async_call(game_show_plane_war_over_async, NULL);
}

/**
 * @brief 切换到游戏中心菜单模式。
 *
 * 详细说明：
 * - 隐藏/销毁子页面并显示菜单入口。
 */
static void game_show_menu_mode(void)
{
    s_game.mode = GAME_MODE_MENU;

    game_destroy_plane_war_over();
    game_destroy_plane_war_scores();


    if(s_game.plane_war_page) {
        watch_plane_war_destroy();
        s_game.plane_war_page = NULL;
    } else {
        watch_plane_war_stop();
    }

    lv_obj_clear_flag(s_game.back_btn, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(s_game.title, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(s_game.plane_bg, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(s_game.cursor, LV_OBJ_FLAG_HIDDEN);

    game_apply_language();
    game_selection_update(false);
}

/**
 * @brief 切换到飞机大战游戏模式。
 *
 * 详细说明：
 * - 创建或重置飞机大战页面并启动游戏。
 */
static void game_show_plane_war_mode(void)
{
    game_destroy_plane_war_over();
    game_destroy_plane_war_scores();

    if(s_game.plane_war_page == NULL) {
        s_game.plane_war_page = watch_plane_war_create(s_game.page);
        if(s_game.plane_war_page == NULL) {
            return;
        }
    }

    s_game.mode = GAME_MODE_PLANE_WAR;

    lv_obj_add_flag(s_game.back_btn, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_game.title, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_game.plane_bg, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_game.cursor, LV_OBJ_FLAG_HIDDEN);

    lv_obj_clear_flag(s_game.plane_war_page, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_game.plane_war_page);

    watch_plane_war_start();
}

/**
 * @brief 切换到飞机大战结算模式。
 *
 * 详细说明：
 * - 显示返回、重玩和历史分数入口。
 */
static void game_show_plane_war_over_mode(void)
{
    if(s_game.page == NULL) {
        return;
    }

    s_game.mode = GAME_MODE_PLANE_WAR_OVER;

    game_destroy_plane_war_scores();
    game_store_pending_plane_war_score();


    if(s_game.plane_war_page) {
        watch_plane_war_destroy();
        s_game.plane_war_page = NULL;
    }
    else {
        watch_plane_war_stop();
    }

    lv_obj_add_flag(s_game.back_btn, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_game.title, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_game.plane_bg, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_game.cursor, LV_OBJ_FLAG_HIDDEN);

    if(s_game.plane_war_over_page == NULL) {
        s_game.plane_war_over_page = watch_plane_war_over_create(s_game.page);
        if(s_game.plane_war_over_page == NULL) {
            game_show_menu_mode();
            return;
        }
    }

    watch_plane_war_over_reset();
    lv_obj_clear_flag(s_game.plane_war_over_page, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_game.plane_war_over_page);
}

/**
 * @brief 切换到历史分数模式。
 *
 * 详细说明：
 * - 提交待保存分数并显示排行榜。
 */
static void game_show_plane_war_scores_mode(void)
{
    if(s_game.page == NULL) {
        return;
    }

    s_game.mode = GAME_MODE_PLANE_WAR_SCORES;

    lv_obj_add_flag(s_game.back_btn, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_game.title, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_game.plane_bg, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_game.cursor, LV_OBJ_FLAG_HIDDEN);

    if(s_game.plane_war_page) {
        watch_plane_war_destroy();
        s_game.plane_war_page = NULL;
    }
    else {
        watch_plane_war_stop();
    }


    game_destroy_plane_war_over();

    if(s_game.plane_war_scores_page == NULL) {
        s_game.plane_war_scores_page = watch_plane_war_scores_create(s_game.page);
        if(s_game.plane_war_scores_page == NULL) {
            game_show_plane_war_over_mode();
            return;
        }
    }

    watch_plane_war_scores_reset();
    lv_obj_clear_flag(s_game.plane_war_scores_page, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_game.plane_war_scores_page);
}

void watch_game_destroy(void);

/**
 * @brief watch_game_create 对外接口。
 *
 * 详细说明：
 * - 供其他模块调用，隐藏本文件内部状态细节。
 *
 * @param parent 输入或输出参数，具体含义见调用处和函数内部使用方式。
 *
 * @return 函数执行结果或计算得到的值，具体语义见返回路径。
 */
lv_obj_t *watch_game_create(lv_obj_t *parent)
{
    if(s_game.page != NULL) {
        watch_game_destroy();
    }

    memset(&s_game, 0, sizeof(s_game));
    watch_plane_war_set_game_over_cb(game_plane_war_over_cb);

    s_game.page = lv_obj_create(parent);
    lv_obj_remove_style_all(s_game.page);
    lv_obj_set_size(s_game.page, WATCH_SCREEN_W, WATCH_SCREEN_H);
    lv_obj_set_pos(s_game.page, 0, 0);
    lv_obj_set_style_bg_color(s_game.page, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_game.page, LV_OPA_COVER, 0);
    lv_obj_clear_flag(s_game.page, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(s_game.page, LV_OBJ_FLAG_CLICKABLE);

    s_game.back_btn = lv_label_create(s_game.page);
    game_label_style_26(s_game.back_btn);
    lv_label_set_text(s_game.back_btn, LV_SYMBOL_LEFT);
    lv_obj_set_pos(s_game.back_btn, GAME_BACK_X, GAME_TOP_Y);

    s_game.title = lv_label_create(s_game.page);
    game_label_style_26(s_game.title);
    lv_label_set_text(s_game.title, watch_language_is_chinese() ? "游戏" : "Game");
    lv_obj_align(s_game.title, LV_ALIGN_TOP_MID, 0, GAME_TOP_Y);

    s_game.plane_bg = lv_obj_create(s_game.page);
    lv_obj_remove_style_all(s_game.plane_bg);
    lv_obj_set_size(s_game.plane_bg, GAME_PLANE_BG_W, GAME_PLANE_BG_H);
    lv_obj_set_pos(s_game.plane_bg, GAME_PLANE_BG_X, GAME_PLANE_BG_Y);
    lv_obj_set_style_bg_color(s_game.plane_bg, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(s_game.plane_bg, LV_OPA_30, 0);
    lv_obj_set_style_radius(s_game.plane_bg, GAME_PLANE_BG_R, 0);
    lv_obj_set_style_pad_all(s_game.plane_bg, 0, 0);
    lv_obj_clear_flag(s_game.plane_bg, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(s_game.plane_bg, LV_OBJ_FLAG_CLICKABLE);

    s_game.plane_label = lv_label_create(s_game.plane_bg);
    lv_obj_set_style_text_font(s_game.plane_label, game_i18n_font_26(), 0);
    lv_obj_set_style_text_color(s_game.plane_label, lv_color_white(), 0);
    lv_obj_set_style_text_align(s_game.plane_label, LV_TEXT_ALIGN_LEFT, 0);
    lv_obj_clear_flag(s_game.plane_label, LV_OBJ_FLAG_CLICKABLE);
    lv_label_set_text(s_game.plane_label, watch_language_is_chinese() ? "飞机大战" : "Plane War");
    lv_obj_center(s_game.plane_label);

    s_game.cursor = lv_obj_create(s_game.page);
    lv_obj_remove_style_all(s_game.cursor);
    lv_obj_set_size(s_game.cursor, 20, 20);
    lv_obj_set_style_bg_opa(s_game.cursor, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_color(s_game.cursor, lv_color_white(), 0);
    lv_obj_set_style_border_opa(s_game.cursor, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_game.cursor, GAME_SELECTOR_BORDER_W, 0);
    lv_obj_set_style_radius(s_game.cursor, GAME_SELECTOR_RADIUS, 0);
    lv_obj_set_style_pad_all(s_game.cursor, 0, 0);
    lv_obj_clear_flag(s_game.cursor, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(s_game.cursor, LV_OBJ_FLAG_CLICKABLE);

    s_game.plane_war_page = NULL;
    s_game.plane_war_over_page = NULL;
    s_game.plane_war_scores_page = NULL;
    s_game.pending_plane_war_score = 0;
    s_game.has_pending_plane_war_score = false;

    s_game.focus = GAME_FOCUS_BACK;
    s_game.mode = GAME_MODE_MENU;
    s_game.wants_back = false;

    game_show_menu_mode();

    return s_game.page;
}

/**
 * @brief 创建或重置游戏中心页面。
 *
 * 详细说明：
 * - 初始化菜单 UI、焦点和子页面状态。
 */
void watch_game_reset(void)
{
    if(s_game.page == NULL) {
        return;
    }

    s_game.focus = GAME_FOCUS_BACK;
    s_game.mode = GAME_MODE_MENU;
    s_game.wants_back = false;

    game_show_menu_mode();
}

/**
 * @brief 处理游戏中心按键。
 *
 * 详细说明：
 * - 根据当前模式分发到菜单、游戏、结算或分数页面。
 *
 * @param key 输入或输出参数，具体含义见调用处和函数内部使用方式。
 */
void watch_game_on_key(watch_key_t key)
{
    if(s_game.page == NULL) {
        return;
    }

    if(key == WATCH_KEY_2_RELEASE) {
        return;
    }

    if(s_game.mode == GAME_MODE_PLANE_WAR) {
        watch_plane_war_on_key(key);

        if(watch_plane_war_is_game_over()) {
            game_show_plane_war_over_mode();
            return;
        }

        if(watch_plane_war_wants_back()) {
            s_game.focus = GAME_FOCUS_PLANE_WAR;
            game_show_menu_mode();
        }

        return;
    }

    if(s_game.mode == GAME_MODE_PLANE_WAR_OVER) {
        watch_plane_war_over_on_key(key);

        if(watch_plane_war_over_wants_back()) {
            s_game.focus = GAME_FOCUS_PLANE_WAR;
            game_show_menu_mode();
            return;
        }

        if(watch_plane_war_over_wants_play_again()) {
            game_show_plane_war_mode();
            return;
        }

        if(watch_plane_war_over_wants_scores()) {
            game_show_plane_war_scores_mode();
            return;
        }

        return;
    }

    if(s_game.mode == GAME_MODE_PLANE_WAR_SCORES) {
        watch_plane_war_scores_on_key(key);

        if(watch_plane_war_scores_wants_back()) {
            game_show_plane_war_over_mode();
        }

        return;
    }

    if(key == WATCH_KEY_1 || key == WATCH_KEY_3) {
        if(s_game.focus == GAME_FOCUS_BACK) {
            s_game.focus = GAME_FOCUS_PLANE_WAR;
        } else {
            s_game.focus = GAME_FOCUS_BACK;
        }

        game_selection_update(true);
        return;
    }

    if(key == WATCH_KEY_2) {
        if(s_game.focus == GAME_FOCUS_BACK) {
            s_game.wants_back = true;
        }
        else if(s_game.focus == GAME_FOCUS_PLANE_WAR) {
            game_show_plane_war_mode();
        }

        return;
    }
}

/**
 * @brief 查询游戏中心是否请求返回。
 *
 * 详细说明：
 * - 供主 UI 判断是否回到菜单。
 *
 * @return 函数执行结果或计算得到的值，具体语义见返回路径。
 */
bool watch_game_wants_back(void)
{
    return s_game.wants_back;
}

/**
 * @brief 清理游戏中心临时页面。
 *
 * 详细说明：
 * - 用于模式切换或退出前释放子页面。
 */
void watch_game_cleanup(void)
{
    if(s_game.plane_war_page) {
        watch_plane_war_destroy();
        s_game.plane_war_page = NULL;
    }

    if(s_game.plane_war_over_page) {
        watch_plane_war_over_destroy();
        s_game.plane_war_over_page = NULL;
    }

    if(s_game.plane_war_scores_page) {
        watch_plane_war_scores_destroy();
        s_game.plane_war_scores_page = NULL;
    }

    s_game.mode = GAME_MODE_MENU;
    s_game.wants_back = false;
}

/**
 * @brief 销毁游戏中心所有资源。
 *
 * 详细说明：
 * - 删除主页面和子页面，清空上下文。
 */
void watch_game_destroy(void)
{
    watch_game_cleanup();

    if(s_game.cursor) {
        lv_anim_del(s_game.cursor, game_cursor_x_anim_cb);
        lv_anim_del(s_game.cursor, game_cursor_y_anim_cb);
        lv_anim_del(s_game.cursor, game_cursor_w_anim_cb);
        lv_anim_del(s_game.cursor, game_cursor_h_anim_cb);
    }

    if(s_game.page) {
        lv_obj_del(s_game.page);
        s_game.page = NULL;
    }

    watch_plane_war_set_game_over_cb(NULL);
    memset(&s_game, 0, sizeof(s_game));
}
