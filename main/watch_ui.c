/**
 * @file watch_ui.c
 * @brief 手表页面管理和页面切换动画。
 */


/**
 * @section 模块说明
 * 本文件是整个手表 UI 的页面调度器，负责创建页面、销毁页面和执行上下滑动动画。
 * 各业务页面只暴露 create/reset/on_key/wants_back/destroy 等接口，页面间跳转统一在这里处理。
 *
 * 页面生命周期策略：
 * - 表盘页进入后台时会暂停刷新，避免动画叠加；
 * - 菜单页被保留，目的是保存用户停留的菜单图标索引；
 * - 游戏、天气、指南针等子页面离开后销毁，释放 LVGL 对象和定时器资源。
 */

#include "watch_ui.h"
#include "watch_tomato_clock.h"
#include "watch_game.h"
#include "watch_e_card.h"
#include "watch_weather.h"
#include "watch_compass.h"
#include "watch_presentation.h"
#include "watch_focus.h"
#include "watch_pedometer.h"
#include "watch_settings.h"
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "lvgl.h"
#include "watch_face.h"
#include "watch_menu.h"

/* 以下宏大多是 UI 坐标、尺寸或任务参数。
 * 修改这类值时建议同时检查：
 * 1. 240x240 屏幕边界是否越界；
 * 2. 选择框/动画目标是否仍然对齐；
 * 3. FreeRTOS 任务栈是否足够容纳 JSON/HTTP/LVGL 临时对象。
 */
#define WATCH_SCREEN_W          240
#define WATCH_SCREEN_H          240
#define PAGE_ANIM_MS            420

#define DEBUG_START_FROM_MENU   0

/* 页面模块销毁接口。 */
void watch_face_destroy(void);
void watch_tomato_clock_destroy(void);
void watch_game_destroy(void);
void watch_e_card_destroy(void);
void watch_weather_destroy(void);
void watch_compass_destroy(void);
void watch_presentation_destroy(void);
void watch_focus_destroy(void);
void watch_pedometer_destroy(void);
void watch_settings_destroy(void);

/**
 * @brief UI 页面枚举。
 *
 * 所有页面跳转都通过该枚举表示，避免模块间直接操作彼此的 LVGL 对象。
 */
typedef enum {
    UI_PAGE_FACE = 0,
    UI_PAGE_MENU,
    UI_PAGE_TOMATO_CLOCK,
    UI_PAGE_GAME,
    UI_PAGE_E_CARD,
    UI_PAGE_WEATHER,
    UI_PAGE_COMPASS,
    UI_PAGE_PRESENTATION,
    UI_PAGE_FOCUS,
    UI_PAGE_PEDOMETER,
    UI_PAGE_SETTINGS,
} ui_page_t;

/**
 * @brief UI 调度器上下文。
 *
 * 保存根屏幕、各页面对象指针、当前页、目标页和动画状态。
 * animing 用于防止动画过程中再次触发页面切换。
 */
typedef struct {
    lv_obj_t *scr;
    lv_obj_t *face_page;
    lv_obj_t *menu_page;
    lv_obj_t *tomato_page;
    lv_obj_t *game_page;
    lv_obj_t *e_card_page;
    lv_obj_t *weather_page;
    lv_obj_t *compass_page;
    lv_obj_t *presentation_page;
    lv_obj_t *focus_page;
    lv_obj_t *pedometer_page;
    lv_obj_t *settings_page;

    ui_page_t page;
    ui_page_t target_page;
    ui_page_t leaving_page;
    bool animing;
} watch_ui_ctx_t;

static watch_ui_ctx_t s_ui;

static void anim_y_cb(void *var, int32_t v)
{
    /* 页面滑动动画回调，只改变对象 Y 坐标。页面切换统一使用纵向滑动。
     */
    lv_obj_set_y((lv_obj_t *)var, v);
}

static lv_obj_t **ui_page_slot(ui_page_t page)
{
    switch(page) {
    case UI_PAGE_FACE:
        return &s_ui.face_page;

    case UI_PAGE_MENU:
        return &s_ui.menu_page;

    case UI_PAGE_TOMATO_CLOCK:
        return &s_ui.tomato_page;

    case UI_PAGE_GAME:
        return &s_ui.game_page;

    case UI_PAGE_E_CARD:
        return &s_ui.e_card_page;

    case UI_PAGE_WEATHER:
        return &s_ui.weather_page;

    case UI_PAGE_COMPASS:
        return &s_ui.compass_page;

    case UI_PAGE_PRESENTATION:
        return &s_ui.presentation_page;

    case UI_PAGE_FOCUS:
        return &s_ui.focus_page;

    case UI_PAGE_PEDOMETER:
        return &s_ui.pedometer_page;

    case UI_PAGE_SETTINGS:
        return &s_ui.settings_page;

    default:
        return NULL;
    }
}

static lv_obj_t *ui_create_face_page(void)
{
    lv_obj_t *page = lv_obj_create(s_ui.scr);
    if(page == NULL) {
        return NULL;
    }

    lv_obj_remove_style_all(page);
    lv_obj_set_size(page, WATCH_SCREEN_W, WATCH_SCREEN_H);
    lv_obj_set_pos(page, 0, 0);
    lv_obj_clear_flag(page, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(page, LV_OBJ_FLAG_CLICKABLE);

    watch_face_create_on(page);

    return page;
}

/**
 * @brief 按页面类型懒创建对应 LVGL 页面。
 */
static lv_obj_t *ui_create_page(ui_page_t page)
{
    lv_obj_t **slot = ui_page_slot(page);
    if(slot == NULL) {
        return NULL;
    }

    if(*slot != NULL) {
        return *slot;
    }

    switch(page) {
    case UI_PAGE_FACE:
        *slot = ui_create_face_page();
        break;

    case UI_PAGE_MENU:
        *slot = watch_menu_create(s_ui.scr);
        break;

    case UI_PAGE_TOMATO_CLOCK:
        *slot = watch_tomato_clock_create(s_ui.scr);
        break;

    case UI_PAGE_GAME:
        *slot = watch_game_create(s_ui.scr);
        break;

    case UI_PAGE_E_CARD:
        *slot = watch_e_card_create(s_ui.scr);
        break;

    case UI_PAGE_WEATHER:
        *slot = watch_weather_create(s_ui.scr);
        break;

    case UI_PAGE_COMPASS:
        *slot = watch_compass_create(s_ui.scr);
        break;

    case UI_PAGE_PRESENTATION:
        *slot = watch_presentation_create(s_ui.scr);
        break;

    case UI_PAGE_FOCUS:
        *slot = watch_focus_create(s_ui.scr);
        break;

    case UI_PAGE_PEDOMETER:
        *slot = watch_pedometer_create(s_ui.scr);
        break;

    case UI_PAGE_SETTINGS:
        *slot = watch_settings_create(s_ui.scr);
        break;

    default:
        *slot = NULL;
        break;
    }

    if(*slot) {
        lv_obj_set_pos(*slot, 0, 0);
        lv_obj_clear_flag(*slot, LV_OBJ_FLAG_HIDDEN);
    }

    return *slot;
}

/**
 * @brief 销毁指定页面；菜单页会被保留以保存选中状态。
 */
static void ui_destroy_page(ui_page_t page)
{
    /* 销毁指定页面并调用对应模块的 destroy。菜单页故意保留，用于保存菜单选择状态。
     */
    lv_obj_t **slot = ui_page_slot(page);
    if(slot == NULL || *slot == NULL) {
        return;
    }

    switch(page) {
    case UI_PAGE_FACE:
        watch_face_set_active(false);
        watch_face_destroy();
        lv_obj_del(*slot);
        *slot = NULL;
        break;

    case UI_PAGE_MENU:
        /* 菜单页常驻，用于保留当前选中的图标索引。 */
        break;

    case UI_PAGE_TOMATO_CLOCK:
        watch_tomato_clock_destroy();
        *slot = NULL;
        break;

    case UI_PAGE_GAME:
        watch_game_destroy();
        *slot = NULL;
        break;

    case UI_PAGE_E_CARD:
        watch_e_card_destroy();
        *slot = NULL;
        break;

    case UI_PAGE_WEATHER:
        watch_weather_destroy();
        *slot = NULL;
        break;

    case UI_PAGE_COMPASS:
        watch_compass_destroy();
        *slot = NULL;
        break;

    case UI_PAGE_PRESENTATION:
        watch_presentation_destroy();
        *slot = NULL;
        break;

    case UI_PAGE_FOCUS:
        watch_focus_destroy();
        *slot = NULL;
        break;

    case UI_PAGE_PEDOMETER:
        watch_pedometer_destroy();
        *slot = NULL;
        break;

    case UI_PAGE_SETTINGS:
        watch_settings_destroy();
        *slot = NULL;
        break;

    default:
        break;
    }
}

static void ui_set_active_page(ui_page_t page)
{
    /* 切换表盘 active 状态。只有表盘页可见时才让表盘定时刷新。
     */
    if(page == UI_PAGE_FACE) {
        watch_face_set_active(true);
    }
    else {
        watch_face_set_active(false);
    }
}

/**
 * @brief 检查并记录一次页面切换的目标状态。
 */
static bool ui_begin_switch(ui_page_t target_page)
{
    /* 开始一次页面切换前的公共检查：防止动画重入、避免切到当前页，并确保目标页已创建。
     */
    if(s_ui.animing || s_ui.page == target_page) {
        return false;
    }

    if(ui_create_page(target_page) == NULL) {
        return false;
    }

    s_ui.leaving_page = s_ui.page;
    s_ui.target_page = target_page;
    s_ui.animing = true;

    return true;
}

/**
 * @brief 页面切换动画完成回调。
 */
static void page_anim_done_cb(lv_anim_t *a)
{
    /* 页面切换动画完成后提交目标页、释放旧页面，并恢复当前页 active 状态。
     */
    (void)a;

    ui_page_t old_page = s_ui.leaving_page;

    s_ui.page = s_ui.target_page;
    s_ui.animing = false;

    /* 切换完成后释放旧页面；菜单页由 ui_destroy_page() 内部保留。 */
    if(old_page != s_ui.page) {
        ui_destroy_page(old_page);
    }

    ui_set_active_page(s_ui.page);
}

static void start_y_anim(lv_obj_t *obj,
                         int32_t from,
                         int32_t to,
                         lv_anim_completed_cb_t done_cb)
{
    /* 封装 LVGL Y 方向动画的公共参数。
     */
    if(obj == NULL) {
        return;
    }

    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, obj);
    lv_anim_set_values(&a, from, to);
    lv_anim_set_duration(&a, PAGE_ANIM_MS);
    lv_anim_set_exec_cb(&a, anim_y_cb);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);

    if(done_cb) {
        lv_anim_set_completed_cb(&a, done_cb);
    }

    lv_anim_start(&a);
}

static void switch_to_menu(void)
{
    /* 从表盘上滑进入菜单。表盘向上滑出，菜单从底部滑入。
     */
    if(s_ui.page != UI_PAGE_FACE || !ui_begin_switch(UI_PAGE_MENU)) {
        return;
    }

    /* 切到菜单前暂停表盘刷新，避免动画叠加。 */
    watch_face_set_active(false);
    watch_menu_reset();

    /* 菜单必须在表盘上层，否则滑入时会被表盘挡住。 */
    lv_obj_move_foreground(s_ui.menu_page);

    lv_obj_set_y(s_ui.face_page, 0);
    lv_obj_set_y(s_ui.menu_page, WATCH_SCREEN_H);

    start_y_anim(s_ui.face_page, 0, -WATCH_SCREEN_H, NULL);
    start_y_anim(s_ui.menu_page, WATCH_SCREEN_H, 0, page_anim_done_cb);
}

static void switch_to_face(void)
{
    /* 从菜单返回表盘。菜单向下滑出，表盘留在底层。
     */
    if(s_ui.page != UI_PAGE_MENU || !ui_begin_switch(UI_PAGE_FACE)) {
        return;
    }

    /* 返回动画结束后再恢复表盘刷新。 */
    watch_face_set_active(false);

    /* 表盘固定在底层，菜单页向下滑出并保留选择状态。 */
    lv_obj_set_y(s_ui.face_page, 0);
    lv_obj_set_y(s_ui.menu_page, 0);

    lv_obj_move_foreground(s_ui.menu_page);

    start_y_anim(s_ui.menu_page, 0, WATCH_SCREEN_H, page_anim_done_cb);
}

static void switch_to_settings(void)
{
    /* 从表盘进入设置页。设置页属于表盘快捷入口，不经过菜单。
     */
    if(s_ui.page != UI_PAGE_FACE || !ui_begin_switch(UI_PAGE_SETTINGS)) {
        return;
    }

    /* 进入设置页前暂停表盘刷新，避免动画叠加。 */
    watch_face_set_active(false);
    watch_settings_reset();

    lv_obj_move_foreground(s_ui.settings_page);

    lv_obj_set_y(s_ui.face_page, 0);
    lv_obj_set_y(s_ui.settings_page, -WATCH_SCREEN_H);

    /* 设置页从屏幕上方滑入，表盘同步向下滑出。 */
    start_y_anim(s_ui.face_page, 0, WATCH_SCREEN_H, NULL);
    start_y_anim(s_ui.settings_page, -WATCH_SCREEN_H, 0, page_anim_done_cb);
}

static void switch_settings_to_face(void)
{
    /* 从设置页返回表盘，并在动画结束后恢复表盘刷新。
     */
    if(s_ui.page != UI_PAGE_SETTINGS || !ui_begin_switch(UI_PAGE_FACE)) {
        return;
    }

    /* 返回表盘时重新创建表盘页，设置页在动画结束后销毁。 */
    watch_face_set_active(false);

    lv_obj_set_y(s_ui.face_page, 0);
    lv_obj_set_y(s_ui.settings_page, 0);

    lv_obj_move_foreground(s_ui.settings_page);

    start_y_anim(s_ui.settings_page, 0, -WATCH_SCREEN_H, page_anim_done_cb);
}

static void switch_to_tomato_clock(void)
{
    /* 从菜单进入番茄钟页面。
     */
    if(s_ui.page != UI_PAGE_MENU || !ui_begin_switch(UI_PAGE_TOMATO_CLOCK)) {
        return;
    }

    watch_tomato_clock_reset();

    lv_obj_move_foreground(s_ui.tomato_page);

    lv_obj_set_y(s_ui.menu_page, 0);
    lv_obj_set_y(s_ui.tomato_page, WATCH_SCREEN_H);

    start_y_anim(s_ui.menu_page, 0, -WATCH_SCREEN_H, NULL);
    start_y_anim(s_ui.tomato_page, WATCH_SCREEN_H, 0, page_anim_done_cb);
}

static void switch_tomato_to_menu(void)
{
    /* 从番茄钟返回菜单。
     */
    if(s_ui.page != UI_PAGE_TOMATO_CLOCK || !ui_begin_switch(UI_PAGE_MENU)) {
        return;
    }

    /* 返回菜单时保留进入二级页面前的图标位置。 */
    lv_obj_set_y(s_ui.menu_page, 0);
    lv_obj_set_y(s_ui.tomato_page, 0);

    lv_obj_move_foreground(s_ui.tomato_page);

    start_y_anim(s_ui.tomato_page, 0, WATCH_SCREEN_H, page_anim_done_cb);
}

static void switch_to_focus(void)
{
    if(s_ui.page != UI_PAGE_MENU || !ui_begin_switch(UI_PAGE_FOCUS)) return;
    watch_focus_reset();
    lv_obj_move_foreground(s_ui.focus_page);
    lv_obj_set_y(s_ui.menu_page, 0);
    lv_obj_set_y(s_ui.focus_page, WATCH_SCREEN_H);
    start_y_anim(s_ui.menu_page, 0, -WATCH_SCREEN_H, NULL);
    start_y_anim(s_ui.focus_page, WATCH_SCREEN_H, 0, page_anim_done_cb);
}

static void switch_focus_to_menu(void)
{
    if(s_ui.page != UI_PAGE_FOCUS || !ui_begin_switch(UI_PAGE_MENU)) return;
    lv_obj_set_y(s_ui.menu_page, 0);
    lv_obj_set_y(s_ui.focus_page, 0);
    lv_obj_move_foreground(s_ui.focus_page);
    start_y_anim(s_ui.focus_page, 0, WATCH_SCREEN_H, page_anim_done_cb);
}

static void switch_to_pedometer(void)
{
    if(s_ui.page != UI_PAGE_MENU || !ui_begin_switch(UI_PAGE_PEDOMETER)) return;
    watch_pedometer_reset();
    lv_obj_move_foreground(s_ui.pedometer_page);
    lv_obj_set_y(s_ui.menu_page, 0);
    lv_obj_set_y(s_ui.pedometer_page, WATCH_SCREEN_H);
    start_y_anim(s_ui.menu_page, 0, -WATCH_SCREEN_H, NULL);
    start_y_anim(s_ui.pedometer_page, WATCH_SCREEN_H, 0, page_anim_done_cb);
}

static void switch_pedometer_to_menu(void)
{
    if(s_ui.page != UI_PAGE_PEDOMETER || !ui_begin_switch(UI_PAGE_MENU)) return;
    lv_obj_set_y(s_ui.menu_page, 0);
    lv_obj_set_y(s_ui.pedometer_page, 0);
    lv_obj_move_foreground(s_ui.pedometer_page);
    start_y_anim(s_ui.pedometer_page, 0, WATCH_SCREEN_H, page_anim_done_cb);
}

static void switch_to_game(void)
{
    /* 从菜单进入游戏中心页面。
     */
    if(s_ui.page != UI_PAGE_MENU || !ui_begin_switch(UI_PAGE_GAME)) {
        return;
    }

    watch_game_reset();

    lv_obj_move_foreground(s_ui.game_page);

    lv_obj_set_y(s_ui.menu_page, 0);
    lv_obj_set_y(s_ui.game_page, WATCH_SCREEN_H);

    start_y_anim(s_ui.menu_page, 0, -WATCH_SCREEN_H, NULL);
    start_y_anim(s_ui.game_page, WATCH_SCREEN_H, 0, page_anim_done_cb);
}

static void switch_game_to_menu(void)
{
    /* 从游戏中心返回菜单。
     */
    if(s_ui.page != UI_PAGE_GAME || !ui_begin_switch(UI_PAGE_MENU)) {
        return;
    }

    /* 离开游戏页前先释放 Plane War 子页面资源。 */
    watch_game_cleanup();

    /* 从游戏页返回菜单时保留图标位置。 */
    lv_obj_set_y(s_ui.menu_page, 0);
    lv_obj_set_y(s_ui.game_page, 0);

    lv_obj_move_foreground(s_ui.game_page);

    start_y_anim(s_ui.game_page, 0, WATCH_SCREEN_H, page_anim_done_cb);
}

static void switch_to_e_card(void)
{
    /* 从菜单进入设备信息页面。
     */
    if(s_ui.page != UI_PAGE_MENU || !ui_begin_switch(UI_PAGE_E_CARD)) {
        return;
    }

    watch_e_card_reset();

    lv_obj_move_foreground(s_ui.e_card_page);

    lv_obj_set_y(s_ui.menu_page, 0);
    lv_obj_set_y(s_ui.e_card_page, WATCH_SCREEN_H);

    start_y_anim(s_ui.menu_page, 0, -WATCH_SCREEN_H, NULL);
    start_y_anim(s_ui.e_card_page, WATCH_SCREEN_H, 0, page_anim_done_cb);
}

static void switch_e_card_to_menu(void)
{
    /* 从设备信息返回菜单。
     */
    if(s_ui.page != UI_PAGE_E_CARD || !ui_begin_switch(UI_PAGE_MENU)) {
        return;
    }

    /* 返回菜单时保留设备信息图标位置。 */
    lv_obj_set_y(s_ui.menu_page, 0);
    lv_obj_set_y(s_ui.e_card_page, 0);

    lv_obj_move_foreground(s_ui.e_card_page);

    start_y_anim(s_ui.e_card_page, 0, WATCH_SCREEN_H, page_anim_done_cb);
}

static void switch_to_weather(void)
{
    /* 从菜单进入天气页面。
     */
    if(s_ui.page != UI_PAGE_MENU || !ui_begin_switch(UI_PAGE_WEATHER)) {
        return;
    }

    watch_weather_reset();

    lv_obj_move_foreground(s_ui.weather_page);

    lv_obj_set_y(s_ui.menu_page, 0);
    lv_obj_set_y(s_ui.weather_page, WATCH_SCREEN_H);

    start_y_anim(s_ui.menu_page, 0, -WATCH_SCREEN_H, NULL);
    start_y_anim(s_ui.weather_page, WATCH_SCREEN_H, 0, page_anim_done_cb);
}

static void switch_weather_to_menu(void)
{
    /* 从天气页面返回菜单。
     */
    if(s_ui.page != UI_PAGE_WEATHER || !ui_begin_switch(UI_PAGE_MENU)) {
        return;
    }

    /* 返回菜单时保留天气图标位置。 */
    lv_obj_set_y(s_ui.menu_page, 0);
    lv_obj_set_y(s_ui.weather_page, 0);

    lv_obj_move_foreground(s_ui.weather_page);

    start_y_anim(s_ui.weather_page, 0, WATCH_SCREEN_H, page_anim_done_cb);
}

static void switch_to_compass(void)
{
    /* 从菜单进入指南针页面。
     */
    if(s_ui.page != UI_PAGE_MENU || !ui_begin_switch(UI_PAGE_COMPASS)) {
        return;
    }

    watch_compass_reset();

    lv_obj_move_foreground(s_ui.compass_page);

    lv_obj_set_y(s_ui.menu_page, 0);
    lv_obj_set_y(s_ui.compass_page, WATCH_SCREEN_H);

    start_y_anim(s_ui.menu_page, 0, -WATCH_SCREEN_H, NULL);
    start_y_anim(s_ui.compass_page, WATCH_SCREEN_H, 0, page_anim_done_cb);
}

static void switch_compass_to_menu(void)
{
    /* 从指南针返回菜单。
     */
    if(s_ui.page != UI_PAGE_COMPASS || !ui_begin_switch(UI_PAGE_MENU)) {
        return;
    }

    /* 从指南针页返回菜单时保留图标位置。 */
    lv_obj_set_y(s_ui.menu_page, 0);
    lv_obj_set_y(s_ui.compass_page, 0);

    lv_obj_move_foreground(s_ui.compass_page);

    start_y_anim(s_ui.compass_page, 0, WATCH_SCREEN_H, page_anim_done_cb);
}

static void switch_to_presentation(void)
{
    if(s_ui.page != UI_PAGE_MENU || !ui_begin_switch(UI_PAGE_PRESENTATION)) return;
    watch_presentation_reset();
    lv_obj_move_foreground(s_ui.presentation_page);
    lv_obj_set_y(s_ui.menu_page, 0);
    lv_obj_set_y(s_ui.presentation_page, WATCH_SCREEN_H);
    start_y_anim(s_ui.menu_page, 0, -WATCH_SCREEN_H, NULL);
    start_y_anim(s_ui.presentation_page, WATCH_SCREEN_H, 0, page_anim_done_cb);
}

static void switch_presentation_to_menu(void)
{
    if(s_ui.page != UI_PAGE_PRESENTATION || !ui_begin_switch(UI_PAGE_MENU)) return;
    lv_obj_set_y(s_ui.menu_page, 0);
    lv_obj_set_y(s_ui.presentation_page, 0);
    lv_obj_move_foreground(s_ui.presentation_page);
    start_y_anim(s_ui.presentation_page, 0, WATCH_SCREEN_H, page_anim_done_cb);
}

/**
 * @brief 创建手表 UI 根屏幕和初始页面。
 */
void watch_ui_create(void)
{
    /* 创建根屏幕并初始化首个页面，是 UI 系统入口。
     */
    memset(&s_ui, 0, sizeof(s_ui));

    s_ui.scr = lv_obj_create(NULL);
    lv_obj_remove_style_all(s_ui.scr);
    lv_obj_set_size(s_ui.scr, WATCH_SCREEN_W, WATCH_SCREEN_H);
    lv_obj_set_style_bg_color(s_ui.scr, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_ui.scr, LV_OPA_COVER, 0);
    lv_obj_clear_flag(s_ui.scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(s_ui.scr, LV_OBJ_FLAG_CLICKABLE);

#if DEBUG_START_FROM_MENU

    /* 调试菜单时：开机只创建菜单页。 */
    s_ui.page = UI_PAGE_MENU;
    s_ui.target_page = UI_PAGE_MENU;
    s_ui.leaving_page = UI_PAGE_MENU;
    s_ui.animing = false;

    ui_create_page(UI_PAGE_MENU);
    lv_obj_set_y(s_ui.menu_page, 0);
    lv_obj_move_foreground(s_ui.menu_page);

    watch_face_set_active(false);

#else

    /* 正常模式：开机只创建首页表盘。 */
    s_ui.page = UI_PAGE_FACE;
    s_ui.target_page = UI_PAGE_FACE;
    s_ui.leaving_page = UI_PAGE_FACE;
    s_ui.animing = false;

    ui_create_page(UI_PAGE_FACE);
    lv_obj_set_y(s_ui.face_page, 0);

    watch_face_set_active(true);

#endif

    lv_scr_load(s_ui.scr);
}

/**
 * @brief 根据当前页面分发按键事件。
 */
void watch_ui_on_key(watch_key_t key)
{
    /* 全局按键分发入口：根据当前页面把按键交给对应模块，或触发页面切换。
     */
    if(key == WATCH_KEY_NONE || s_ui.animing) {
        return;
    }

    if(s_ui.page == UI_PAGE_FACE) {
        if(key == WATCH_KEY_1) {
            switch_to_settings();
        }
        else if(key == WATCH_KEY_3) {
            switch_to_menu();
        }
        return;
    }

    if(s_ui.page == UI_PAGE_MENU) {
        if(key == WATCH_KEY_3) {
            watch_menu_next();
        }
        else if(key == WATCH_KEY_1) {
            watch_menu_prev();
        }
        else if(key == WATCH_KEY_2) {
            if(watch_menu_is_back_selected()) {
                switch_to_face();
            }
            else if(watch_menu_is_timer_selected()) {
                switch_to_tomato_clock();
            }
            else if(watch_menu_is_tomato_clock_selected()) {
                switch_to_focus();
            }
            else if(watch_menu_is_pedometer_selected()) {
                switch_to_pedometer();
            }
            else if(watch_menu_is_game_selected()) {
                switch_to_game();
            }
            else if(watch_menu_is_e_card_selected()) {
                switch_to_e_card();
            }
            else if(watch_menu_is_weather_selected()) {
                switch_to_weather();
            }
            else if(watch_menu_is_compass_selected()) {
                switch_to_compass();
            }
            else if(watch_menu_is_presentation_selected()) {
                switch_to_presentation();
            }
        }
        return;
    }

    if(s_ui.page == UI_PAGE_TOMATO_CLOCK) {
        watch_tomato_clock_on_key(key);

        if(watch_tomato_clock_wants_back()) switch_tomato_to_menu();

        return;
    }

    if(s_ui.page == UI_PAGE_GAME) {
        watch_game_on_key(key);

        if(watch_game_wants_back()) {
            switch_game_to_menu();
        }

        return;
    }

    if(s_ui.page == UI_PAGE_E_CARD) {
        watch_e_card_on_key(key);

        if(watch_e_card_wants_back()) {
            switch_e_card_to_menu();
        }

        return;
    }

    if(s_ui.page == UI_PAGE_WEATHER) {
        watch_weather_on_key(key);

        if(watch_weather_wants_back()) {
            switch_weather_to_menu();
        }

        return;
    }

    if(s_ui.page == UI_PAGE_COMPASS) {
        watch_compass_on_key(key);

        if(watch_compass_wants_back()) {
            switch_compass_to_menu();
        }

        return;
    }

    if(s_ui.page == UI_PAGE_PRESENTATION) {
        watch_presentation_on_key(key);
        if(watch_presentation_wants_back()) switch_presentation_to_menu();
        return;
    }

    if(s_ui.page == UI_PAGE_FOCUS) {
        watch_focus_on_key(key);
        if(watch_focus_wants_back()) switch_focus_to_menu();
        return;
    }

    if(s_ui.page == UI_PAGE_PEDOMETER) {
        watch_pedometer_on_key(key);
        if(watch_pedometer_wants_back()) switch_pedometer_to_menu();
        return;
    }

    if(s_ui.page == UI_PAGE_SETTINGS) {
        watch_settings_on_key(key);

        if(watch_settings_wants_back()) {
            switch_settings_to_face();
        }

        return;
    }
}

bool watch_ui_should_keep_awake(void)
{
    return s_ui.page == UI_PAGE_PRESENTATION || s_ui.target_page == UI_PAGE_PRESENTATION;
}


/* 维护提示
 * 新增页面时，请同步扩展 ui_page_t、ui_page_slot()、ui_create_page()、ui_destroy_page() 和按键分发。
 */
