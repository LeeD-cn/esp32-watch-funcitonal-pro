/**
 * @file watch_plane_war_over.c
 * @brief 飞机大战结束页面及返回、重玩、查看分数入口。
 */
/*
 * ==================== 模块说明 ====================
 *  * 模块职责：
 * - 飞机大战结算页面，显示 Game Over 图案以及返回、重玩、历史分数三个入口。
 * - 页面使用选择框表示当前焦点，按左右键切换，确认键设置对应 wants_* 标志。
 * - 结算页不直接切换上层页面，只把用户意图暴露给 watch_game.c 的状态机处理。
 * - 选择框移动使用 LVGL 动画，增强小屏幕操作反馈。
 *
 * 阅读建议：
 * - 先看本文件顶部的宏定义和静态状态变量，理解硬件参数和运行状态。
 * - 再看 reset/init/start/on_key/destroy 等对外函数，理解页面或驱动的生命周期。
 * - 最后看 static 辅助函数，了解具体寄存器读写、UI 刷新或状态机细节。
 * =======================================================
 */

#include "watch_plane_war_over.h"

#include <stdbool.h>
#include <string.h>

#define WATCH_SCREEN_W              240
#define WATCH_SCREEN_H              240

#define PLANE_WAR_OVER_TITLE_Y      58

#define PLANE_WAR_OVER_BOX_W        48
#define PLANE_WAR_OVER_BOX_H        48
#define PLANE_WAR_OVER_BOX_R        8
#define PLANE_WAR_OVER_BOX_Y        125
#define PLANE_WAR_OVER_BOX_ANIM_MS  160

#define PLANE_WAR_OVER_ICON_W       40
#define PLANE_WAR_OVER_ICON_H       40
#define PLANE_WAR_OVER_ICON_Y       129

#define PLANE_WAR_OVER_RETURN_X     24
#define PLANE_WAR_OVER_PLAY_X       96
#define PLANE_WAR_OVER_SCORES_X     168

LV_IMG_DECLARE(return_img);
LV_IMG_DECLARE(play_again_img);
LV_IMG_DECLARE(game_scores_img);
LV_IMG_DECLARE(game_over_img);

/**
 * @brief 结算页可聚焦操作。
 */
typedef enum {
    PLANE_WAR_OVER_FOCUS_RETURN = 0,
    PLANE_WAR_OVER_FOCUS_PLAY_AGAIN,
    PLANE_WAR_OVER_FOCUS_SCORES,
} plane_war_over_focus_t;

/**
 * @brief 飞机大战结算页上下文。
 */
typedef struct {
    lv_obj_t *page;
    lv_obj_t *title;
    lv_obj_t *selector_box;
    lv_obj_t *return_img;
    lv_obj_t *play_again_img;
    lv_obj_t *game_scores_img;

    plane_war_over_focus_t focus;
    bool wants_back;
    bool wants_play_again;
    bool wants_scores;
} plane_war_over_ctx_t;

/* 飞机大战结算页上下文。 */
static plane_war_over_ctx_t s_plane_war_over;

/**
 * @brief 根据结算页焦点返回图标 X 坐标。
 *
 * 详细说明：
 * - 供选择框定位使用。
 *
 * @param focus 输入或输出参数，具体含义见调用处和函数内部使用方式。
 *
 * @return 函数执行结果或计算得到的值，具体语义见返回路径。
 */
static lv_coord_t plane_war_over_focus_x(plane_war_over_focus_t focus)
{
    switch(focus) {
    case PLANE_WAR_OVER_FOCUS_RETURN:
        return PLANE_WAR_OVER_RETURN_X;

    case PLANE_WAR_OVER_FOCUS_PLAY_AGAIN:
        return PLANE_WAR_OVER_PLAY_X;

    case PLANE_WAR_OVER_FOCUS_SCORES:
        return PLANE_WAR_OVER_SCORES_X;

    default:
        return PLANE_WAR_OVER_RETURN_X;
    }
}

/**
 * @brief 更新结算页选择框位置。
 *
 * 详细说明：
 * - 支持立即切换或动画切换。
 *
 * @param focus 输入或输出参数，具体含义见函数内部使用方式。
 * @param anim 输入或输出参数，具体含义见函数内部使用方式。
 */
static void plane_war_over_selector_set_focus(plane_war_over_focus_t focus,
                                               bool anim)
{
    if(s_plane_war_over.selector_box == NULL) {
        return;
    }

    s_plane_war_over.focus = focus;

    lv_coord_t target_x = plane_war_over_focus_x(focus);

    if(!anim) {
        lv_anim_del(s_plane_war_over.selector_box, NULL);
        lv_obj_set_pos(s_plane_war_over.selector_box,
                       target_x,
                       PLANE_WAR_OVER_BOX_Y);
        return;
    }

    lv_anim_del(s_plane_war_over.selector_box, NULL);

    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, s_plane_war_over.selector_box);
    lv_anim_set_exec_cb(&a, (lv_anim_exec_xcb_t)lv_obj_set_x);
    lv_anim_set_values(&a,
                       lv_obj_get_x(s_plane_war_over.selector_box),
                       target_x);
    lv_anim_set_time(&a, PLANE_WAR_OVER_BOX_ANIM_MS);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_start(&a);
}

/**
 * @brief 创建结算页操作图标。
 *
 * 详细说明：
 * - 统一设置图标资源和坐标。
 *
 * @param img_obj 输入或输出参数，具体含义见调用处和函数内部使用方式。
 * @param src 输入或输出参数，具体含义见调用处和函数内部使用方式。
 * @param x 输入或输出参数，具体含义见调用处和函数内部使用方式。
 */
static void plane_war_over_create_icon(lv_obj_t **img_obj,
                                       const void *src,
                                       lv_coord_t x)
{
    *img_obj = lv_img_create(s_plane_war_over.page);
    if(*img_obj == NULL) {
        return;
    }

    lv_img_set_src(*img_obj, src);
    lv_obj_set_size(*img_obj,
                    PLANE_WAR_OVER_ICON_W,
                    PLANE_WAR_OVER_ICON_H);
    lv_obj_set_pos(*img_obj, x + 4, PLANE_WAR_OVER_ICON_Y);
    lv_obj_clear_flag(*img_obj, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(*img_obj, LV_OBJ_FLAG_CLICKABLE);
}

/**
 * @brief watch_plane_war_over_create 对外接口。
 *
 * 详细说明：
 * - 供其他模块调用，隐藏本文件内部状态细节。
 *
 * @param parent 输入或输出参数，具体含义见调用处和函数内部使用方式。
 *
 * @return 函数执行结果或计算得到的值，具体语义见返回路径。
 */
lv_obj_t *watch_plane_war_over_create(lv_obj_t *parent)
{
    if(s_plane_war_over.page) {
        watch_plane_war_over_destroy();
    }

    memset(&s_plane_war_over, 0, sizeof(s_plane_war_over));

    s_plane_war_over.page = lv_obj_create(parent);
    if(s_plane_war_over.page == NULL) {
        return NULL;
    }

    lv_obj_remove_style_all(s_plane_war_over.page);
    lv_obj_set_size(s_plane_war_over.page, WATCH_SCREEN_W, WATCH_SCREEN_H);
    lv_obj_set_pos(s_plane_war_over.page, 0, 0);
    lv_obj_set_style_bg_color(s_plane_war_over.page, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_plane_war_over.page, LV_OPA_COVER, 0);
    lv_obj_clear_flag(s_plane_war_over.page, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(s_plane_war_over.page, LV_OBJ_FLAG_CLICKABLE);

    s_plane_war_over.title = lv_img_create(s_plane_war_over.page);

    if(s_plane_war_over.title == NULL) {
        watch_plane_war_over_destroy();
        return NULL;
    }
    lv_img_set_src(s_plane_war_over.title, &game_over_img);
    lv_obj_set_size(s_plane_war_over.title, WATCH_SCREEN_W, 45);
    lv_obj_align(s_plane_war_over.title,
                 LV_ALIGN_TOP_MID,
                 0,
                 PLANE_WAR_OVER_TITLE_Y);
    lv_obj_clear_flag(s_plane_war_over.title, LV_OBJ_FLAG_CLICKABLE);

    plane_war_over_create_icon(&s_plane_war_over.return_img,
                               &return_img,
                               PLANE_WAR_OVER_RETURN_X);
    if(s_plane_war_over.return_img == NULL) {
        watch_plane_war_over_destroy();
        return NULL;
    }

    plane_war_over_create_icon(&s_plane_war_over.play_again_img,
                               &play_again_img,
                               PLANE_WAR_OVER_PLAY_X);
    if(s_plane_war_over.play_again_img == NULL) {
        watch_plane_war_over_destroy();
        return NULL;
    }

    plane_war_over_create_icon(&s_plane_war_over.game_scores_img,
                               &game_scores_img,
                               PLANE_WAR_OVER_SCORES_X);
    if(s_plane_war_over.game_scores_img == NULL) {
        watch_plane_war_over_destroy();
        return NULL;
    }

    s_plane_war_over.selector_box = lv_obj_create(s_plane_war_over.page);
    if(s_plane_war_over.selector_box == NULL) {
        watch_plane_war_over_destroy();
        return NULL;
    }

    lv_obj_remove_style_all(s_plane_war_over.selector_box);
    lv_obj_set_size(s_plane_war_over.selector_box,
                    PLANE_WAR_OVER_BOX_W,
                    PLANE_WAR_OVER_BOX_H);
    lv_obj_set_pos(s_plane_war_over.selector_box,
                   PLANE_WAR_OVER_RETURN_X,
                   PLANE_WAR_OVER_BOX_Y);
    lv_obj_set_style_bg_color(s_plane_war_over.selector_box,
                              lv_color_white(),
                              0);
    lv_obj_set_style_bg_opa(s_plane_war_over.selector_box, LV_OPA_20, 0);
    lv_obj_set_style_border_color(s_plane_war_over.selector_box,
                                  lv_color_white(),
                                  0);
    lv_obj_set_style_border_width(s_plane_war_over.selector_box, 2, 0);
    lv_obj_set_style_radius(s_plane_war_over.selector_box,
                            PLANE_WAR_OVER_BOX_R,
                            0);
    lv_obj_set_style_pad_all(s_plane_war_over.selector_box, 0, 0);
    lv_obj_clear_flag(s_plane_war_over.selector_box, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(s_plane_war_over.selector_box, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_move_foreground(s_plane_war_over.selector_box);

    s_plane_war_over.focus = PLANE_WAR_OVER_FOCUS_RETURN;
    s_plane_war_over.wants_back = false;
    s_plane_war_over.wants_play_again = false;
    s_plane_war_over.wants_scores = false;

    return s_plane_war_over.page;
}

/**
 * @brief 创建或重置结算页。
 *
 * 详细说明：
 * - 初始化 Game Over 标题、操作图标和默认焦点。
 */
void watch_plane_war_over_reset(void)
{
    if(s_plane_war_over.page == NULL) {
        return;
    }

    s_plane_war_over.wants_back = false;
    s_plane_war_over.wants_play_again = false;
    s_plane_war_over.wants_scores = false;
    plane_war_over_selector_set_focus(PLANE_WAR_OVER_FOCUS_RETURN, false);
    lv_obj_clear_flag(s_plane_war_over.page, LV_OBJ_FLAG_HIDDEN);
}

/**
 * @brief 销毁结算页。
 *
 * 详细说明：
 * - 删除页面对象并清空上下文。
 */
void watch_plane_war_over_destroy(void)
{
    if(s_plane_war_over.page) {
        lv_obj_del(s_plane_war_over.page);
        s_plane_war_over.page = NULL;
    }

    memset(&s_plane_war_over, 0, sizeof(s_plane_war_over));
}

/**
 * @brief 处理结算页按键。
 *
 * 详细说明：
 * - 左右切换焦点，确认后设置返回/重玩/分数标志。
 *
 * @param key 输入或输出参数，具体含义见调用处和函数内部使用方式。
 */
void watch_plane_war_over_on_key(watch_key_t key)
{
    if(s_plane_war_over.page == NULL) {
        return;
    }

    if(key == WATCH_KEY_2_RELEASE) {
        return;
    }

    if(key == WATCH_KEY_3) {
        if(s_plane_war_over.focus == PLANE_WAR_OVER_FOCUS_RETURN) {
            plane_war_over_selector_set_focus(PLANE_WAR_OVER_FOCUS_PLAY_AGAIN,
                                              true);
        }
        else if(s_plane_war_over.focus == PLANE_WAR_OVER_FOCUS_PLAY_AGAIN) {
            plane_war_over_selector_set_focus(PLANE_WAR_OVER_FOCUS_SCORES,
                                              true);
        }
        else {
            plane_war_over_selector_set_focus(PLANE_WAR_OVER_FOCUS_RETURN,
                                              true);
        }

        return;
    }

    if(key == WATCH_KEY_1) {
        if(s_plane_war_over.focus == PLANE_WAR_OVER_FOCUS_RETURN) {
            plane_war_over_selector_set_focus(PLANE_WAR_OVER_FOCUS_SCORES,
                                              true);
        }
        else if(s_plane_war_over.focus == PLANE_WAR_OVER_FOCUS_PLAY_AGAIN) {
            plane_war_over_selector_set_focus(PLANE_WAR_OVER_FOCUS_RETURN,
                                              true);
        }
        else {
            plane_war_over_selector_set_focus(PLANE_WAR_OVER_FOCUS_PLAY_AGAIN,
                                              true);
        }

        return;
    }

    if(key == WATCH_KEY_2) {
        if(s_plane_war_over.focus == PLANE_WAR_OVER_FOCUS_RETURN) {
            s_plane_war_over.wants_back = true;
        }
        else if(s_plane_war_over.focus == PLANE_WAR_OVER_FOCUS_PLAY_AGAIN) {
            s_plane_war_over.wants_play_again = true;
        }
        else if(s_plane_war_over.focus == PLANE_WAR_OVER_FOCUS_SCORES) {
            s_plane_war_over.wants_scores = true;
        }

        return;
    }
}

/**
 * @brief 查询是否选择返回。
 *
 * 详细说明：
 * - 供游戏中心读取并切换回菜单。
 *
 * @return 函数执行结果或计算得到的值，具体语义见返回路径。
 */
bool watch_plane_war_over_wants_back(void)
{
    return s_plane_war_over.wants_back;
}

/**
 * @brief 查询是否选择重玩。
 *
 * 详细说明：
 * - 供游戏中心重新进入游戏模式。
 *
 * @return 函数执行结果或计算得到的值，具体语义见返回路径。
 */
bool watch_plane_war_over_wants_play_again(void)
{
    return s_plane_war_over.wants_play_again;
}

/**
 * @brief 查询是否选择历史分数。
 *
 * 详细说明：
 * - 供游戏中心进入分数页面。
 *
 * @return 函数执行结果或计算得到的值，具体语义见返回路径。
 */
bool watch_plane_war_over_wants_scores(void)
{
    return s_plane_war_over.wants_scores;
}
