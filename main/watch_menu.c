/**
 * @file watch_menu.c
 * @brief 主菜单页面、菜单图标、标题和底部滑动指示器。
 */
/*
 * ==================== 模块说明 ====================
 *  * 模块职责：
 * - 主菜单页面，负责菜单标题、图标切换和底部滑动指示器。
 * - 菜单项包含返回、番茄钟、游戏、设备信息、天气和指南针。
 * - 页面支持中英文标题切换，字体根据当前语言选择中文字体或 Montserrat。
 * - 底部滑块根据当前 index 计算位置，用于提示用户当前所在菜单项。
 *
 * 阅读建议：
 * - 先看本文件顶部的宏定义和静态状态变量，理解硬件参数和运行状态。
 * - 再看 reset/init/start/on_key/destroy 等对外函数，理解页面或驱动的生命周期。
 * - 最后看 static 辅助函数，了解具体寄存器读写、UI 刷新或状态机细节。
 * =======================================================
 */

#include "watch_menu.h"
#include "watch_language.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#define WATCH_SCREEN_W          240
#define WATCH_SCREEN_H          240

#define MENU_ITEM_COUNT         7

#define MENU_TITLE_Y            13
#define MENU_LINE_X             24
#define MENU_LINE_Y             42
#define MENU_LINE_W             192
#define MENU_LINE_H             2

#define MENU_ICON_W             120
#define MENU_ICON_H             120
#define MENU_ICON_X             ((WATCH_SCREEN_W - MENU_ICON_W) / 2)
#define MENU_ICON_Y             72

#define MENU_ICON_AREA_X        0
#define MENU_ICON_AREA_Y        MENU_ICON_Y
#define MENU_ICON_AREA_W        WATCH_SCREEN_W
#define MENU_ICON_AREA_H        MENU_ICON_H

#define MENU_SLIDER_W           144
#define MENU_SLIDER_H           4
#define MENU_SLIDER_GAP         4
#define MENU_SLIDER_SEG_W       ((MENU_SLIDER_W - (MENU_ITEM_COUNT - 1) * MENU_SLIDER_GAP) / MENU_ITEM_COUNT)
#define MENU_SLIDER_X           ((WATCH_SCREEN_W - MENU_SLIDER_W) / 2)
#define MENU_SLIDER_Y           222

LV_FONT_DECLARE(cn_font_26);
LV_FONT_DECLARE(device_info_font_20);
LV_FONT_DECLARE(presentation_font_20);
LV_IMG_DECLARE(menu_bg);

/**
 * @brief 主菜单页面对象和当前索引。
 */
typedef struct {
    lv_obj_t *page;
    lv_obj_t *bg_img;
    lv_obj_t *title;
    lv_obj_t *icon_area;
    lv_obj_t *icon_cur;

    lv_obj_t *slider_area;
    lv_obj_t *slider_thumb;

    uint8_t index;
} watch_menu_ctx_t;

/* 主菜单页面对象和当前菜单索引。 */
static watch_menu_ctx_t s_menu;

/**
 * @brief 菜单标题的中英文文本。
 */
typedef struct {
    const char *en;
    const char *cn;
} menu_title_i18n_t;

static const menu_title_i18n_t s_menu_titles[MENU_ITEM_COUNT] = {
    {"Back", "返回"},
    {"Tomato Clock", "番茄钟"},
    {"Game", "游戏"},
    {"Device Info", "设备信息"},
    {"Weather", "天气"},
    {"Compass", "指南针"},
    {"Presentation", "演示遥控"},
};

/**
 * @brief 根据菜单索引返回标题文本。
 *
 * 详细说明：
 * - 支持中英文，索引越界时回退到第一项。
 *
 * @param index 输入或输出参数，具体含义见调用处和函数内部使用方式。
 *
 * @return 函数执行结果或计算得到的值，具体语义见返回路径。
 */
static const char *menu_title_text(uint8_t index)
{
    if(index >= MENU_ITEM_COUNT) {
        index = 0;
    }

    return watch_language_is_chinese() ?
           s_menu_titles[index].cn :
           s_menu_titles[index].en;
}

/**
 * @brief 根据当前语言选择菜单标题字体。
 *
 * 详细说明：
 * - 中文使用专用中文字库，英文使用 Montserrat。
 *
 * @return 函数执行结果或计算得到的值，具体语义见返回路径。
 */
static const lv_font_t *menu_title_font(void)
{
    if(watch_language_is_chinese() && s_menu.index == 3) {
        return &device_info_font_20;
    }
    if(watch_language_is_chinese() && s_menu.index == 6) {
        return &presentation_font_20;
    }
    return watch_language_is_chinese() ? &cn_font_26 : &lv_font_montserrat_26;
}

LV_IMG_DECLARE(back_icon);
LV_IMG_DECLARE(tomato_clock_icon);
LV_IMG_DECLARE(game_icon);
LV_IMG_DECLARE(e_card_icon);
LV_IMG_DECLARE(weather_icon);
LV_IMG_DECLARE(compass_icon);


static const lv_img_dsc_t *s_menu_icons[6] = {
    &back_icon,
    &tomato_clock_icon,
    &game_icon,
    &e_card_icon,
    &weather_icon,
    &compass_icon,

};


/**
 * @brief 设置当前菜单图标并在图标区域内居中。
 *
 * 详细说明：
 * - 根据 index 切换 LVGL 图片源。
 *
 * @param icon 输入或输出参数，具体含义见函数内部使用方式。
 * @param index 输入或输出参数，具体含义见函数内部使用方式。
 */
static lv_obj_t *menu_icon_create(lv_obj_t *parent, uint8_t index)
{
    if(parent == NULL || index >= MENU_ITEM_COUNT) {
        return NULL;
    }

    if(index < 6) {
        lv_obj_t *icon = lv_img_create(parent);
        lv_img_set_src(icon, s_menu_icons[index]);
        lv_obj_align(icon, LV_ALIGN_CENTER, 0, 0);
        lv_obj_clear_flag(icon, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_clear_flag(icon, LV_OBJ_FLAG_CLICKABLE);
        return icon;
    }

    /* 演示遥控图标用 LVGL 图形绘制，避免为简单线框增加大尺寸位图资源。 */
    lv_obj_t *screen = lv_obj_create(parent);
    lv_obj_remove_style_all(screen);
    lv_obj_set_size(screen, 96, 70);
    lv_obj_align(screen, LV_ALIGN_CENTER, 0, -5);
    lv_obj_set_style_border_width(screen, 4, 0);
    lv_obj_set_style_border_color(screen, lv_color_white(), 0);
    lv_obj_set_style_radius(screen, 8, 0);
    lv_obj_set_style_bg_color(screen, lv_color_hex(0x20345E), 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
    lv_obj_clear_flag(screen, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *play = lv_label_create(screen);
    lv_obj_set_style_text_font(play, &lv_font_montserrat_36, 0);
    lv_obj_set_style_text_color(play, lv_color_hex(0x65A4FF), 0);
    lv_label_set_text(play, LV_SYMBOL_PLAY);
    lv_obj_center(play);

    lv_obj_t *stand = lv_obj_create(parent);
    lv_obj_remove_style_all(stand);
    lv_obj_set_size(stand, 5, 18);
    lv_obj_align(stand, LV_ALIGN_CENTER, 0, 39);
    lv_obj_set_style_bg_color(stand, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(stand, LV_OPA_COVER, 0);
    return screen;
}

/**
 * @brief 刷新底部滑动指示器位置。
 *
 * 详细说明：
 * - 根据当前菜单 index 计算滑块 x 坐标。
 */
static void menu_slider_update(void)
{
    if(s_menu.slider_thumb == NULL) {
        return;
    }

    lv_coord_t x = s_menu.index * (MENU_SLIDER_SEG_W + MENU_SLIDER_GAP);
    lv_obj_set_x(s_menu.slider_thumb, x);
}

/**
 * @brief 创建底部六段式菜单位置指示器。
 *
 * 详细说明：
 * - 包含背景区域和当前项滑块。
 *
 * @param parent 输入或输出参数，具体含义见函数内部使用方式。
 */
static void menu_slider_create(lv_obj_t *parent)
{
    s_menu.slider_area = lv_obj_create(parent);
    lv_obj_remove_style_all(s_menu.slider_area);
    lv_obj_set_size(s_menu.slider_area, MENU_SLIDER_W, MENU_SLIDER_H);
    lv_obj_set_pos(s_menu.slider_area, MENU_SLIDER_X, MENU_SLIDER_Y);
    lv_obj_set_style_bg_opa(s_menu.slider_area, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(s_menu.slider_area, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(s_menu.slider_area, LV_OBJ_FLAG_CLICKABLE);

    for(int i = 0; i < MENU_ITEM_COUNT; i++) {
        lv_obj_t *seg = lv_obj_create(s_menu.slider_area);
        lv_obj_remove_style_all(seg);
        lv_obj_set_size(seg, MENU_SLIDER_SEG_W, MENU_SLIDER_H);
        lv_obj_set_pos(seg, i * (MENU_SLIDER_SEG_W + MENU_SLIDER_GAP), 0);
        lv_obj_set_style_bg_color(seg, lv_color_hex(0x404040), 0);
        lv_obj_set_style_bg_opa(seg, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(seg, MENU_SLIDER_H / 2, 0);
        lv_obj_clear_flag(seg, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_clear_flag(seg, LV_OBJ_FLAG_CLICKABLE);
    }

    s_menu.slider_thumb = lv_obj_create(s_menu.slider_area);
    lv_obj_remove_style_all(s_menu.slider_thumb);
    lv_obj_set_size(s_menu.slider_thumb, MENU_SLIDER_SEG_W, MENU_SLIDER_H);
    lv_obj_set_pos(s_menu.slider_thumb, 0, 0);
    lv_obj_set_style_bg_color(s_menu.slider_thumb, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(s_menu.slider_thumb, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_menu.slider_thumb, MENU_SLIDER_H / 2, 0);
    lv_obj_clear_flag(s_menu.slider_thumb, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(s_menu.slider_thumb, LV_OBJ_FLAG_CLICKABLE);
}

/**
 * @brief 刷新当前菜单项显示。
 *
 * 详细说明：
 * - 同步更新标题、图标和底部指示器。
 */
static void menu_update_current(void)
{
    if(s_menu.title) {
        lv_obj_set_style_text_font(s_menu.title, menu_title_font(), 0);
        lv_label_set_text(s_menu.title, menu_title_text(s_menu.index));
    }

    if(s_menu.icon_cur) {
        lv_obj_clean(s_menu.icon_area);
        s_menu.icon_cur = menu_icon_create(s_menu.icon_area, s_menu.index);
    }

    menu_slider_update();

    if(s_menu.icon_area) {
        lv_obj_invalidate(s_menu.icon_area);
    }

    if(s_menu.slider_area) {
        lv_obj_invalidate(s_menu.slider_area);
    }
}

void watch_menu_destroy(void);

/**
 * @brief watch_menu_create 对外接口。
 *
 * 详细说明：
 * - 供其他模块调用，隐藏本文件内部状态细节。
 *
 * @param parent 输入或输出参数，具体含义见调用处和函数内部使用方式。
 *
 * @return 函数执行结果或计算得到的值，具体语义见返回路径。
 */
lv_obj_t *watch_menu_create(lv_obj_t *parent)
{
    if(s_menu.page != NULL) {
        watch_menu_destroy();
    }

    memset(&s_menu, 0, sizeof(s_menu));

    s_menu.page = lv_obj_create(parent);
    lv_obj_remove_style_all(s_menu.page);
    lv_obj_set_size(s_menu.page, WATCH_SCREEN_W, WATCH_SCREEN_H);
    lv_obj_set_pos(s_menu.page, 0, 0);
    lv_obj_set_style_bg_color(s_menu.page, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_menu.page, LV_OPA_COVER, 0);
    lv_obj_clear_flag(s_menu.page, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(s_menu.page, LV_OBJ_FLAG_CLICKABLE);


    s_menu.bg_img = lv_img_create(s_menu.page);
    if(s_menu.bg_img != NULL) {
        lv_img_set_src(s_menu.bg_img, &menu_bg);
        lv_obj_set_pos(s_menu.bg_img, 0, 0);
        lv_obj_clear_flag(s_menu.bg_img, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_clear_flag(s_menu.bg_img, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_move_background(s_menu.bg_img);
    }

    s_menu.title = lv_label_create(s_menu.page);
    lv_obj_set_style_text_font(s_menu.title, menu_title_font(), 0);
    lv_obj_set_style_text_color(s_menu.title, lv_color_white(), 0);
    lv_obj_set_style_text_align(s_menu.title, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(s_menu.title, menu_title_text(0));
    lv_obj_set_width(s_menu.title, WATCH_SCREEN_W);
    lv_obj_set_pos(s_menu.title, 0, MENU_TITLE_Y);

    lv_obj_t *line = lv_obj_create(s_menu.page);
    lv_obj_remove_style_all(line);
    lv_obj_set_size(line, MENU_LINE_W, MENU_LINE_H);
    lv_obj_set_pos(line, MENU_LINE_X, MENU_LINE_Y);
    lv_obj_set_style_bg_color(line, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(line, LV_OPA_COVER, 0);

    s_menu.index = 0;

    s_menu.icon_area = lv_obj_create(s_menu.page);
    lv_obj_remove_style_all(s_menu.icon_area);
    lv_obj_set_size(s_menu.icon_area, MENU_ICON_AREA_W, MENU_ICON_AREA_H);
    lv_obj_set_pos(s_menu.icon_area, MENU_ICON_AREA_X, MENU_ICON_AREA_Y);


    lv_obj_set_style_bg_opa(s_menu.icon_area, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(s_menu.icon_area, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(s_menu.icon_area, LV_OBJ_FLAG_CLICKABLE);


    lv_obj_clear_flag(s_menu.icon_area, LV_OBJ_FLAG_OVERFLOW_VISIBLE);

    s_menu.icon_cur = menu_icon_create(s_menu.icon_area, 0);


    lv_obj_t *left_arrow = lv_label_create(s_menu.page);
    lv_obj_set_style_text_font(left_arrow, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(left_arrow, lv_color_white(), 0);
    lv_label_set_text(left_arrow, LV_SYMBOL_LEFT);
    lv_obj_align(left_arrow, LV_ALIGN_LEFT_MID, 10, 8);
    lv_obj_clear_flag(left_arrow, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *right_arrow = lv_label_create(s_menu.page);
    lv_obj_set_style_text_font(right_arrow, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(right_arrow, lv_color_white(), 0);
    lv_label_set_text(right_arrow, LV_SYMBOL_RIGHT);
    lv_obj_align(right_arrow, LV_ALIGN_RIGHT_MID, -10, 8);
    lv_obj_clear_flag(right_arrow, LV_OBJ_FLAG_CLICKABLE);


    menu_slider_create(s_menu.page);
    menu_slider_update();

    return s_menu.page;
}

/**
 * @brief 切换到下一个菜单项。
 *
 * 详细说明：
 * - 索引到末尾后循环回到开头。
 */
void watch_menu_next(void)
{
    if(s_menu.page == NULL) {
        return;
    }

    s_menu.index = (s_menu.index + 1) % MENU_ITEM_COUNT;
    menu_update_current();
}

/**
 * @brief 切换到上一个菜单项。
 *
 * 详细说明：
 * - 索引到开头前循环到末尾。
 */
void watch_menu_prev(void)
{
    if(s_menu.page == NULL) {
        return;
    }

    if(s_menu.index == 0) {
        s_menu.index = MENU_ITEM_COUNT - 1;
    } else {
        s_menu.index--;
    }

    menu_update_current();
}

/**
 * @brief 判断当前是否选中返回项。
 *
 * 详细说明：
 * - 供主 UI 确认键处理。
 *
 * @return 函数执行结果或计算得到的值，具体语义见返回路径。
 */
bool watch_menu_is_back_selected(void)
{
    return s_menu.index == 0;
}

/**
 * @brief 判断当前是否选中番茄钟。
 *
 * 详细说明：
 * - 供主 UI 路由到番茄钟页面。
 *
 * @return 函数执行结果或计算得到的值，具体语义见返回路径。
 */
bool watch_menu_is_tomato_clock_selected(void)
{
    return s_menu.index == 1;
}

/**
 * @brief 判断当前是否选中游戏。
 *
 * 详细说明：
 * - 供主 UI 路由到游戏中心。
 *
 * @return 函数执行结果或计算得到的值，具体语义见返回路径。
 */
bool watch_menu_is_game_selected(void)
{
    return s_menu.index == 2;
}

/**
 * @brief 判断当前是否选中设备信息。
 *
 * 详细说明：
 * - 供主 UI 路由到设备信息页面。
 *
 * @return 函数执行结果或计算得到的值，具体语义见返回路径。
 */
bool watch_menu_is_e_card_selected(void)
{
    return s_menu.index == 3;
}

/**
 * @brief 判断当前是否选中天气。
 *
 * 详细说明：
 * - 供主 UI 路由到天气页面。
 *
 * @return 函数执行结果或计算得到的值，具体语义见返回路径。
 */
bool watch_menu_is_weather_selected(void)
{
    return s_menu.index == 4;
}

/**
 * @brief 判断当前是否选中指南针。
 *
 * 详细说明：
 * - 供主 UI 路由到指南针页面。
 *
 * @return 函数执行结果或计算得到的值，具体语义见返回路径。
 */
bool watch_menu_is_compass_selected(void)
{
    return s_menu.index == 5;
}

bool watch_menu_is_presentation_selected(void)
{
    return s_menu.index == 6;
}

/**
 * @brief 创建或重置主菜单页面。
 *
 * 详细说明：
 * - 初始化背景、标题、图标和滑动指示器。
 */
void watch_menu_reset(void)
{
    if(s_menu.page == NULL) {
        return;
    }

    s_menu.index = 0;
    menu_update_current();
}


/**
 * @brief 销毁主菜单页面。
 *
 * 详细说明：
 * - 删除 LVGL 页面并清空上下文。
 */
void watch_menu_destroy(void)
{
    if(s_menu.page) {
        lv_obj_del(s_menu.page);
        s_menu.page = NULL;
    }

    memset(&s_menu, 0, sizeof(s_menu));
}
