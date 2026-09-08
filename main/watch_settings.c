/**
 * @file watch_settings.c
 * @brief 设置页面，包括亮度、自动息屏和语言切换。
 */


/**
 * @section 模块说明
 * 本文件实现设置页的 UI 状态机，包含亮度编辑、自动息屏开关和语言下拉框。
 * 页面不直接处理触摸事件，而是由实体按键驱动焦点移动和确认动作。
 *
 * 状态变量阅读重点：
 * - selected：当前焦点在哪个设置项；
 * - brightness_editing：亮度条是否处于编辑模式；
 * - language_dropdown_open / language_dropdown_selected：语言下拉框打开状态与临时选项；
 * - s_auto_off_enabled：自动息屏开关的运行时状态。
 */

#include "watch_settings.h"
#include "lcd_st7789_official.h"
#include "watch_language.h"
#include "esp_err.h"
#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"

#include <stdbool.h>
#include <string.h>

/**
 * @brief 查询自动息屏开关状态。
 */
bool watch_settings_auto_off_enabled(void);

/* 设置页背景图由外部 LVGL 图片资源提供。 */
LV_IMG_DECLARE(settings_bg);
LV_FONT_DECLARE(cn_font_26);

/* 以下宏大多是 UI 坐标、尺寸或任务参数。
 * 修改这类值时建议同时检查：
 * 1. 240x240 屏幕边界是否越界；
 * 2. 选择框/动画目标是否仍然对齐；
 * 3. FreeRTOS 任务栈是否足够容纳 JSON/HTTP/LVGL 临时对象。
 */
#define WATCH_SCREEN_W              240
#define WATCH_SCREEN_H              240

#define SETTINGS_TITLE_Y            10
#define SETTINGS_BACK_X             14
#define SETTINGS_BACK_Y             12

#define SETTINGS_ITEM_X             14
#define SETTINGS_ITEM_START_Y       60
#define SETTINGS_ITEM_GAP           42

#define SETTINGS_SELECTOR_PAD_X     7
#define SETTINGS_SELECTOR_PAD_Y     4
#define SETTINGS_SELECTOR_RADIUS    8
#define SETTINGS_SELECTOR_BORDER_W  2
#define SETTINGS_SELECTOR_ANIM_MS    180

#define BRIGHTNESS_MIN              30
#define BRIGHTNESS_MAX              100
#define BRIGHTNESS_STEP             1
#define BRIGHTNESS_SLIDER_X         SETTINGS_ITEM_X
#define BRIGHTNESS_SLIDER_Y         (SETTINGS_ITEM_START_Y + 13)
#define BRIGHTNESS_SLIDER_W         150
#define BRIGHTNESS_SLIDER_H         10
#define BRIGHTNESS_VALUE_X          178
#define BRIGHTNESS_VALUE_Y          SETTINGS_ITEM_START_Y
#define BRIGHTNESS_VALUE_W          58

#define AUTO_OFF_CHECKBOX_X         150
#define AUTO_OFF_CHECKBOX_Y         (SETTINGS_ITEM_START_Y + SETTINGS_ITEM_GAP + 6)
#define AUTO_OFF_CHECKBOX_SIZE      20
#define AUTO_OFF_SELECTOR_W         (AUTO_OFF_CHECKBOX_X + AUTO_OFF_CHECKBOX_SIZE - SETTINGS_ITEM_X)
#define AUTO_OFF_SELECTOR_H         31

#define LANGUAGE_ARROW_GAP              8
#define LANGUAGE_TARGET_H               31
#define LANGUAGE_DROPDOWN_ROW_H         31
#define LANGUAGE_DROPDOWN_TEXT_PAD_X    14
#define LANGUAGE_DROPDOWN_TEXT_PAD_Y    1
#define LANGUAGE_DROPDOWN_SELECTOR_PAD  2
#define LANGUAGE_DROPDOWN_RADIUS        0
#define LANGUAGE_DROPDOWN_DIVIDER_H     1

#define SETTINGS_NVS_NAMESPACE          "watch"
#define SETTINGS_NVS_AUTO_OFF_KEY       "auto_off"

static const char *TAG = "watch_settings";

/**
 * @brief 设置页主焦点枚举。
 *
 * 按键左右/上下移动时修改该枚举；确认键根据当前枚举执行返回、编辑亮度、
 * 切换自动息屏或打开语言下拉框。
 */
typedef enum {
    SETTINGS_SEL_BACK = 0,
    SETTINGS_SEL_BRIGHTNESS,
    SETTINGS_SEL_SCREEN_ON_MODE,
    SETTINGS_SEL_LANGUAGE,
} settings_sel_t;

typedef enum {
    LANGUAGE_OPT_CHINESE = 0,
    LANGUAGE_OPT_ENGLISH,
} language_option_t;

/**
 * @brief 设置页运行上下文。
 *
 * 包含所有 LVGL 对象指针和交互状态。销毁页面时必须清零，
 * 否则下次进入页面可能访问已经释放的 LVGL 对象。
 */
typedef struct {
    lv_obj_t *root;
    lv_obj_t *bg_img;
    lv_obj_t *back_label;
    lv_obj_t *title_label;
    lv_obj_t *brightness_label;
    lv_obj_t *screen_on_mode_label;
    lv_obj_t *screen_on_mode_target;
    lv_obj_t *auto_off_checkbox;
    lv_obj_t *auto_off_check_label;
    lv_obj_t *language_label;
    lv_obj_t *language_arrow_label;
    lv_obj_t *language_target;
    lv_obj_t *language_dropdown_box;
    lv_obj_t *language_dropdown_selector;
    lv_obj_t *language_dropdown_divider;
    lv_obj_t *language_chinese_label;
    lv_obj_t *language_english_label;
    lv_obj_t *selector;

    lv_obj_t *brightness_slider;
    lv_obj_t *brightness_value_label;
    int32_t brightness_value;
    bool brightness_editing;

    bool language_dropdown_open;
    language_option_t language_dropdown_selected;

    settings_sel_t selected;
    bool wants_back;
} watch_settings_ctx_t;

static watch_settings_ctx_t s_settings;
/* 自动息屏开关：主循环通过 watch_settings_auto_off_enabled() 查询。 */
static bool s_auto_off_enabled = false;
/* 自动息屏配置是否已经从 NVS 恢复，避免每次查询都读 Flash。 */
static bool s_auto_off_loaded = false;
/* 设置页 NVS 是否已初始化完成。 */
static bool s_settings_nvs_ready = false;

/**
 * @brief 根据当前焦点移动设置页选择框。
 */
static void settings_update_selector(bool animated);
/**
 * @brief 按当前语言刷新设置页所有可见文本。
 */
static void settings_apply_language(void);

/**
 * @brief 确保设置页使用的 NVS 可用。
 *
 * 详细说明：
 * - 自动息屏配置需要掉电保存，因此这里复用 watch 命名空间。
 * - 如果 NVS 分区损坏或版本不兼容，按 ESP-IDF 推荐流程擦除后重新初始化。
 */
static bool settings_ensure_nvs_ready(void)
{
    if(s_settings_nvs_ready) {
        return true;
    }

    esp_err_t err = nvs_flash_init();
    if(err == ESP_ERR_NVS_NO_FREE_PAGES ||
       err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS init issue, erase and re-init: %s",
                 esp_err_to_name(err));
        err = nvs_flash_erase();
        if(err != ESP_OK) {
            ESP_LOGW(TAG, "Failed to erase NVS: %s", esp_err_to_name(err));
            return false;
        }

        err = nvs_flash_init();
    }

    if(err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to init NVS: %s", esp_err_to_name(err));
        return false;
    }

    s_settings_nvs_ready = true;
    return true;
}

/**
 * @brief 从 NVS 恢复自动息屏开关。
 *
 * 详细说明：
 * - 第一次查询或创建设置页时调用。
 * - 没有保存过配置时保持默认关闭，兼容旧固件。
 */
static void auto_off_load_saved(void)
{
    if(s_auto_off_loaded) {
        return;
    }

    s_auto_off_loaded = true;

    if(!settings_ensure_nvs_ready()) {
        return;
    }

    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open(SETTINGS_NVS_NAMESPACE,
                             NVS_READONLY,
                             &nvs_handle);
    if(err == ESP_ERR_NVS_NOT_FOUND) {
        return;
    }
    if(err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to open NVS for auto-off load: %s",
                 esp_err_to_name(err));
        return;
    }

    uint8_t saved = 0;
    err = nvs_get_u8(nvs_handle, SETTINGS_NVS_AUTO_OFF_KEY, &saved);
    nvs_close(nvs_handle);

    if(err == ESP_ERR_NVS_NOT_FOUND) {
        return;
    }
    if(err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to load auto-off setting: %s",
                 esp_err_to_name(err));
        return;
    }

    s_auto_off_enabled = saved != 0;
    ESP_LOGI(TAG, "Loaded auto-off setting: %s",
             s_auto_off_enabled ? "on" : "off");
}

/**
 * @brief 将自动息屏开关保存到 NVS。
 *
 * @param enabled true 表示开启自动息屏，false 表示关闭自动息屏。
 */
static void auto_off_save(bool enabled)
{
    if(!settings_ensure_nvs_ready()) {
        return;
    }

    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open(SETTINGS_NVS_NAMESPACE,
                             NVS_READWRITE,
                             &nvs_handle);
    if(err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to open NVS for auto-off save: %s",
                 esp_err_to_name(err));
        return;
    }

    err = nvs_set_u8(nvs_handle, SETTINGS_NVS_AUTO_OFF_KEY, enabled ? 1 : 0);
    if(err == ESP_OK) {
        err = nvs_commit(nvs_handle);
    }
    nvs_close(nvs_handle);

    if(err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to save auto-off setting: %s",
                 esp_err_to_name(err));
    }
}

static int32_t brightness_clamp(int32_t value)
{
    /* 限制亮度值在 UI 允许范围内，避免按键连续调整后越界。
     */
    if(value < BRIGHTNESS_MIN) {
        return BRIGHTNESS_MIN;
    }

    if(value > BRIGHTNESS_MAX) {
        return BRIGHTNESS_MAX;
    }

    return value;
}

static void settings_label_style(lv_obj_t *label)
{
    /* 统一设置页标签的字体、颜色、对齐和内边距，减少每个控件重复样式代码。
     */
    lv_obj_set_style_text_font(label, &lv_font_montserrat_26, 0);
    lv_obj_set_style_text_color(label, lv_color_white(), 0);
    lv_obj_set_style_text_opa(label, LV_OPA_COVER, 0);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_pad_all(label, 0, 0);
}

static lv_obj_t *settings_create_label(lv_obj_t *parent, const char *text, lv_coord_t x, lv_coord_t y)
{
    lv_obj_t *label = lv_label_create(parent);
    settings_label_style(label);
    lv_label_set_text(label, text);
    lv_obj_set_pos(label, x, y);
    lv_obj_clear_flag(label, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(label, LV_OBJ_FLAG_CLICKABLE);
    return label;
}


static const lv_font_t *settings_i18n_font_26(void)
{
    return watch_language_is_chinese() ? &cn_font_26 : &lv_font_montserrat_26;
}

static language_option_t settings_language_option_from_watch(watch_language_t language)
{
    return language == WATCH_LANGUAGE_CHINESE ?
           LANGUAGE_OPT_CHINESE :
           LANGUAGE_OPT_ENGLISH;
}

static watch_language_t settings_watch_language_from_option(language_option_t option)
{
    /* 把下拉框临时选项转换回系统语言枚举，用于最终确认保存。
     */
    return option == LANGUAGE_OPT_CHINESE ?
           WATCH_LANGUAGE_CHINESE :
           WATCH_LANGUAGE_ENGLISH;
}

static void settings_label_set_i18n_text(lv_obj_t *label,
                                         const char *en_text,
                                         const char *cn_text)
{
    /* 按当前语言给标签设置中英文文本，并同步切换字体。
     */
    if(label == NULL) {
        return;
    }

    lv_obj_set_style_text_font(label, settings_i18n_font_26(), 0);
    lv_label_set_text(label, watch_language_is_chinese() ? cn_text : en_text);
}

static void settings_update_language_target_layout(void)
{
    /* 根据语言标签和箭头实际宽度动态计算可聚焦区域，避免中英文长度不同导致选择框错位。
     */
    if(s_settings.root == NULL ||
       s_settings.language_label == NULL ||
       s_settings.language_arrow_label == NULL ||
       s_settings.language_target == NULL) {
        return;
    }

    lv_obj_update_layout(s_settings.root);

    lv_obj_set_pos(s_settings.language_arrow_label,
                   lv_obj_get_x(s_settings.language_label) +
                   lv_obj_get_width(s_settings.language_label) +
                   LANGUAGE_ARROW_GAP,
                   SETTINGS_ITEM_START_Y + SETTINGS_ITEM_GAP * 2);

    lv_obj_update_layout(s_settings.root);

    lv_obj_set_size(s_settings.language_target,
                    lv_obj_get_x(s_settings.language_arrow_label) +
                    lv_obj_get_width(s_settings.language_arrow_label) -
                    SETTINGS_ITEM_X,
                    LANGUAGE_TARGET_H);
}

static void auto_off_update_view(void)
{
    /* 把自动息屏布尔值同步到复选框显示，true 显示对勾，false 清空。
     */
    auto_off_load_saved();

    if(s_settings.auto_off_check_label == NULL) {
        return;
    }

    lv_label_set_text(s_settings.auto_off_check_label,
                      s_auto_off_enabled ? LV_SYMBOL_OK : "");
}

static void auto_off_toggle(void)
{
    /* 切换自动息屏运行时状态，立即保存到 NVS，并刷新复选框视图。
     */
    auto_off_load_saved();
    s_auto_off_enabled = !s_auto_off_enabled;
    auto_off_save(s_auto_off_enabled);
    auto_off_update_view();
}


static void language_dropdown_get_box_rect(lv_coord_t *x,
                                           lv_coord_t *y,
                                           lv_coord_t *w,
                                           lv_coord_t *h)
{
    /* 根据语言目标区域计算下拉框的位置和大小，保证下拉框始终贴着语言项展开。
     */
    lv_coord_t target_x = SETTINGS_ITEM_X;
    lv_coord_t target_y = SETTINGS_ITEM_START_Y + SETTINGS_ITEM_GAP * 2;
    lv_coord_t target_w = 0;
    lv_coord_t target_h = LANGUAGE_TARGET_H;

    if(s_settings.language_target != NULL) {
        target_x = lv_obj_get_x(s_settings.language_target);
        target_y = lv_obj_get_y(s_settings.language_target);
        target_w = lv_obj_get_width(s_settings.language_target);
        target_h = lv_obj_get_height(s_settings.language_target);
    }

    if(target_w <= 0) {
        target_w = 140;
    }

    if(x != NULL) {
        *x = target_x - SETTINGS_SELECTOR_PAD_X;
    }

    if(y != NULL) {
        *y = target_y + target_h + 1;
    }

    if(w != NULL) {
        *w = target_w + SETTINGS_SELECTOR_PAD_X * 2;
    }

    if(h != NULL) {
        *h = LANGUAGE_DROPDOWN_ROW_H * 2;
    }
}

static void language_dropdown_set_hidden(bool hidden)
{
    /* 批量隐藏或显示下拉框相关 LVGL 对象，避免逐个对象重复写隐藏逻辑。
     */
    lv_obj_t *objs[] = {
        s_settings.language_dropdown_box,
        s_settings.language_dropdown_selector,
        s_settings.language_dropdown_divider,
        s_settings.language_chinese_label,
        s_settings.language_english_label,
    };

    for(int i = 0; i < (int)(sizeof(objs) / sizeof(objs[0])); i++) {
        if(objs[i] == NULL) {
            continue;
        }

        if(hidden) {
            lv_obj_add_flag(objs[i], LV_OBJ_FLAG_HIDDEN);
        }
        else {
            lv_obj_clear_flag(objs[i], LV_OBJ_FLAG_HIDDEN);
        }
    }
}

static void language_dropdown_update_view(void)
{
    /* 刷新下拉框布局、选中条位置、文字颜色以及前后层级。
     */
    if(s_settings.root == NULL ||
       s_settings.language_dropdown_box == NULL ||
       s_settings.language_dropdown_selector == NULL ||
       s_settings.language_dropdown_divider == NULL ||
       s_settings.language_chinese_label == NULL ||
       s_settings.language_english_label == NULL) {
        return;
    }

    lv_obj_update_layout(s_settings.root);

    lv_coord_t box_x;
    lv_coord_t box_y;
    lv_coord_t box_w;
    lv_coord_t box_h;
    language_dropdown_get_box_rect(&box_x, &box_y, &box_w, &box_h);

    /* 下拉框宽度跟随语言选项目标区域。 */
    lv_obj_set_pos(s_settings.language_dropdown_box, box_x, box_y);
    lv_obj_set_size(s_settings.language_dropdown_box, box_w, box_h);

    lv_coord_t chinese_y = box_y + LANGUAGE_DROPDOWN_TEXT_PAD_Y;
    lv_coord_t english_y = box_y + LANGUAGE_DROPDOWN_ROW_H + LANGUAGE_DROPDOWN_TEXT_PAD_Y;
    lv_coord_t text_x = box_x + LANGUAGE_DROPDOWN_TEXT_PAD_X;

    lv_obj_set_pos(s_settings.language_chinese_label, text_x, chinese_y);
    lv_obj_set_pos(s_settings.language_english_label, text_x, english_y);

    lv_obj_set_pos(s_settings.language_dropdown_divider,
                   box_x + 1,
                   box_y + LANGUAGE_DROPDOWN_ROW_H);
    lv_obj_set_size(s_settings.language_dropdown_divider,
                    box_w - 2,
                    LANGUAGE_DROPDOWN_DIVIDER_H);

    lv_coord_t selector_y = box_y + LANGUAGE_DROPDOWN_SELECTOR_PAD;
    if(s_settings.language_dropdown_selected == LANGUAGE_OPT_ENGLISH) {
        selector_y += LANGUAGE_DROPDOWN_ROW_H;
    }

    lv_obj_set_pos(s_settings.language_dropdown_selector,
                   box_x + LANGUAGE_DROPDOWN_SELECTOR_PAD,
                   selector_y);
    lv_obj_set_size(s_settings.language_dropdown_selector,
                    box_w - LANGUAGE_DROPDOWN_SELECTOR_PAD * 2,
                    LANGUAGE_DROPDOWN_ROW_H - LANGUAGE_DROPDOWN_SELECTOR_PAD * 2);

    lv_obj_set_style_text_color(s_settings.language_chinese_label,
                                s_settings.language_dropdown_selected == LANGUAGE_OPT_CHINESE ?
                                lv_color_black() : lv_color_white(),
                                0);
    lv_obj_set_style_text_color(s_settings.language_english_label,
                                s_settings.language_dropdown_selected == LANGUAGE_OPT_ENGLISH ?
                                lv_color_black() : lv_color_white(),
                                0);

    lv_obj_move_foreground(s_settings.language_dropdown_box);
    lv_obj_move_foreground(s_settings.language_dropdown_selector);
    lv_obj_move_foreground(s_settings.language_dropdown_divider);
    lv_obj_move_foreground(s_settings.language_chinese_label);
    lv_obj_move_foreground(s_settings.language_english_label);
}

/**
 * @brief 打开语言下拉框并恢复上次停留的选项。
 */
static void language_dropdown_open(void)
{
    /* 打开语言下拉框，并把当前已保存或上次停留的语言作为默认选中项。
     */
    if(s_settings.root == NULL) {
        return;
    }

    s_settings.language_dropdown_open = true;
    s_settings.language_dropdown_selected =
        settings_language_option_from_watch(watch_language_get_dropdown_option());

    language_dropdown_set_hidden(false);
    language_dropdown_update_view();
    settings_update_selector(false);
}

/**
 * @brief 确认语言选择并立即刷新页面。
 */
static void language_dropdown_confirm(void)
{
    /* 确认语言选择：保存语言、保存下拉框停留项，并立即刷新本页面文字。
     */
    watch_language_t language =
        settings_watch_language_from_option(s_settings.language_dropdown_selected);

    watch_language_set(language);
    watch_language_set_dropdown_option(language);
    settings_apply_language();
}

static void language_dropdown_close(void)
{
    /* 关闭语言下拉框并恢复普通选择框状态。
     */
    s_settings.language_dropdown_open = false;
    language_dropdown_set_hidden(true);
    settings_update_selector(false);
}

static void language_dropdown_toggle_selected(void)
{
    /* 在中文和英文两个选项之间切换临时选中项，不立即保存。
     */
    s_settings.language_dropdown_selected =
        s_settings.language_dropdown_selected == LANGUAGE_OPT_CHINESE ?
        LANGUAGE_OPT_ENGLISH :
        LANGUAGE_OPT_CHINESE;

    language_dropdown_update_view();
}


static void settings_apply_language(void)
{
    /* 根据当前语言刷新标题、设置项名称、语言项显示和相关布局，是语言切换后的总刷新入口。
     */
    settings_label_set_i18n_text(s_settings.title_label,
                                 "Settings",
                                 "设置");
    settings_label_set_i18n_text(s_settings.brightness_label,
                                 "Brightness",
                                 "亮度");
    settings_label_set_i18n_text(s_settings.screen_on_mode_label,
                                 "Auto-off",
                                 "自动息屏");
    settings_label_set_i18n_text(s_settings.language_label,
                                 "Language",
                                 "语言");
    settings_label_set_i18n_text(s_settings.language_chinese_label,
                                 "Chinese",
                                 "中文");
    settings_label_set_i18n_text(s_settings.language_english_label,
                                 "English",
                                 "英文");

    if(s_settings.language_arrow_label != NULL) {
        lv_obj_set_style_text_font(s_settings.language_arrow_label,
                                   &lv_font_montserrat_26,
                                   0);
        lv_label_set_text(s_settings.language_arrow_label, LV_SYMBOL_DOWN);
    }

    settings_update_language_target_layout();
    language_dropdown_update_view();
    settings_update_selector(false);
}

static lv_obj_t *settings_selected_obj(void)
{
    switch(s_settings.selected) {
    case SETTINGS_SEL_BACK:
        return s_settings.back_label;

    case SETTINGS_SEL_BRIGHTNESS:
        return s_settings.brightness_label;

    case SETTINGS_SEL_SCREEN_ON_MODE:
        return s_settings.screen_on_mode_target != NULL ?
               s_settings.screen_on_mode_target :
               s_settings.screen_on_mode_label;

    case SETTINGS_SEL_LANGUAGE:
        return s_settings.language_target != NULL ?
               s_settings.language_target :
               s_settings.language_label;

    default:
        return s_settings.back_label;
    }
}

static void selector_x_anim_cb(void *var, int32_t v)
{
    /* LVGL 动画回调：只更新选择框 X 坐标。拆分属性便于同时做位置和尺寸动画。
     */
    lv_obj_set_x((lv_obj_t *)var, v);
}

static void selector_y_anim_cb(void *var, int32_t v)
{
    /* LVGL 动画回调：只更新选择框 Y 坐标。
     */
    lv_obj_set_y((lv_obj_t *)var, v);
}

static void selector_w_anim_cb(void *var, int32_t v)
{
    /* LVGL 动画回调：只更新选择框宽度。
     */
    lv_obj_set_width((lv_obj_t *)var, v);
}

static void selector_h_anim_cb(void *var, int32_t v)
{
    /* LVGL 动画回调：只更新选择框高度。
     */
    lv_obj_set_height((lv_obj_t *)var, v);
}

static void selector_start_anim(lv_obj_t *obj,
                                lv_anim_exec_xcb_t exec_cb,
                                int32_t from,
                                int32_t to)
{
    /* 启动选择框单个属性动画，统一使用 ease_out 路径提升焦点移动手感。
     */
    lv_anim_t a;

    lv_anim_init(&a);
    lv_anim_set_var(&a, obj);
    lv_anim_set_values(&a, from, to);
    lv_anim_set_duration(&a, SETTINGS_SELECTOR_ANIM_MS);
    lv_anim_set_exec_cb(&a, exec_cb);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_start(&a);
}

static void settings_update_selector(bool animated)
{
    /* 根据当前 selected 和编辑状态，把选择框移动到对应控件。亮度编辑和下拉框打开时会改变选择框目标。
     */
    if(s_settings.root == NULL || s_settings.selector == NULL) {
        return;
    }

    if(s_settings.brightness_editing) {
        lv_obj_add_flag(s_settings.selector, LV_OBJ_FLAG_HIDDEN);
        return;
    }

    lv_obj_clear_flag(s_settings.selector, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t *target = settings_selected_obj();
    if(target == NULL) {
        return;
    }

    /* 根据当前语言文本宽度更新可选中区域。 */
    lv_obj_update_layout(s_settings.root);

    lv_coord_t x = lv_obj_get_x(target) - SETTINGS_SELECTOR_PAD_X;
    lv_coord_t y = lv_obj_get_y(target) - SETTINGS_SELECTOR_PAD_Y;
    lv_coord_t w = lv_obj_get_width(target) + SETTINGS_SELECTOR_PAD_X * 2;
    lv_coord_t h = lv_obj_get_height(target) + SETTINGS_SELECTOR_PAD_Y * 2;

    /* 初次定位不做动画，按键切换时再启用动画。 */
    if(!animated || lv_obj_get_width(s_settings.selector) <= 0 || lv_obj_get_height(s_settings.selector) <= 0) {
        lv_obj_set_pos(s_settings.selector, x, y);
        lv_obj_set_size(s_settings.selector, w, h);
    }
    else {
        selector_start_anim(s_settings.selector, selector_x_anim_cb, lv_obj_get_x(s_settings.selector), x);
        selector_start_anim(s_settings.selector, selector_y_anim_cb, lv_obj_get_y(s_settings.selector), y);
        selector_start_anim(s_settings.selector, selector_w_anim_cb, lv_obj_get_width(s_settings.selector), w);
        selector_start_anim(s_settings.selector, selector_h_anim_cb, lv_obj_get_height(s_settings.selector), h);
    }

    lv_obj_move_foreground(s_settings.selector);
}

static void brightness_update_view(void)
{
    /* 把亮度数值同步到滑条、百分比文本和 LCD 背光驱动。
     */
    if(s_settings.brightness_slider) {
        lv_slider_set_value(s_settings.brightness_slider,
                            s_settings.brightness_value,
                            LV_ANIM_OFF);
    }

    if(s_settings.brightness_value_label) {
        lv_label_set_text_fmt(s_settings.brightness_value_label,
                              "%d",
                              (int)s_settings.brightness_value);
    }
}

static void brightness_adjust(int32_t delta)
{
    /* 按指定步进调整亮度，先夹紧范围，再刷新 UI 和硬件亮度。
     */
    int32_t new_value = brightness_clamp(s_settings.brightness_value + delta);
    if(new_value == s_settings.brightness_value) {
        return;
    }

    s_settings.brightness_value = new_value;
    brightness_update_view();
}

/**
 * @brief 进入亮度编辑模式并显示滑块。
 */
static void brightness_edit_enter(void)
{
    /* 进入亮度编辑模式。此时左右/上下按键不再移动焦点，而是修改亮度值。
     */
    if(s_settings.root == NULL || s_settings.brightness_editing) {
        return;
    }

    s_settings.brightness_value = brightness_clamp(lcd_get_backlight_percent());
    s_settings.brightness_editing = true;

    lv_obj_add_flag(s_settings.brightness_label, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_settings.selector, LV_OBJ_FLAG_HIDDEN);

    s_settings.brightness_slider = lv_slider_create(s_settings.root);
    lv_obj_set_pos(s_settings.brightness_slider,
                   BRIGHTNESS_SLIDER_X,
                   BRIGHTNESS_SLIDER_Y);
    lv_obj_set_size(s_settings.brightness_slider,
                    BRIGHTNESS_SLIDER_W,
                    BRIGHTNESS_SLIDER_H);
    lv_slider_set_range(s_settings.brightness_slider,
                        BRIGHTNESS_MIN,
                        BRIGHTNESS_MAX);

    lv_obj_set_style_bg_color(s_settings.brightness_slider,
                              lv_color_hex(0x404040),
                              LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_settings.brightness_slider,
                            LV_OPA_COVER,
                            LV_PART_MAIN);
    lv_obj_set_style_radius(s_settings.brightness_slider,
                            BRIGHTNESS_SLIDER_H / 2,
                            LV_PART_MAIN);

    lv_obj_set_style_bg_color(s_settings.brightness_slider,
                              lv_color_white(),
                              LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(s_settings.brightness_slider,
                            LV_OPA_COVER,
                            LV_PART_INDICATOR);
    lv_obj_set_style_radius(s_settings.brightness_slider,
                            BRIGHTNESS_SLIDER_H / 2,
                            LV_PART_INDICATOR);

    lv_obj_set_style_bg_color(s_settings.brightness_slider,
                              lv_color_white(),
                              LV_PART_KNOB);
    lv_obj_set_style_bg_opa(s_settings.brightness_slider,
                            LV_OPA_COVER,
                            LV_PART_KNOB);
    lv_obj_set_style_pad_all(s_settings.brightness_slider, 5, LV_PART_KNOB);
    lv_obj_clear_flag(s_settings.brightness_slider, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(s_settings.brightness_slider, LV_OBJ_FLAG_CLICKABLE);

    s_settings.brightness_value_label = settings_create_label(s_settings.root,
                                                              "",
                                                              BRIGHTNESS_VALUE_X,
                                                              BRIGHTNESS_VALUE_Y);
    lv_obj_set_width(s_settings.brightness_value_label, BRIGHTNESS_VALUE_W);
    lv_obj_set_style_text_font(s_settings.brightness_value_label,
                               &lv_font_montserrat_26,
                               0);
    lv_obj_set_style_text_align(s_settings.brightness_value_label,
                                LV_TEXT_ALIGN_LEFT,
                                0);

    brightness_update_view();

    lv_obj_move_foreground(s_settings.brightness_slider);
    lv_obj_move_foreground(s_settings.brightness_value_label);
}

/**
 * @brief 退出亮度编辑模式，可选择保存当前亮度。
 */
static void brightness_edit_exit(bool save)
{
    /* 退出亮度编辑模式；save 为 true 时会把当前亮度持久化到 NVS。
     */
    if(!s_settings.brightness_editing) {
        return;
    }

    if(save) {
        /* UI 亮度范围由 LCD 驱动映射到实际 PWM 占空比并保存。 */
        lcd_set_backlight_percent((uint8_t)brightness_clamp(s_settings.brightness_value));
    }

    if(s_settings.brightness_slider) {
        lv_obj_del(s_settings.brightness_slider);
        s_settings.brightness_slider = NULL;
    }

    if(s_settings.brightness_value_label) {
        lv_obj_del(s_settings.brightness_value_label);
        s_settings.brightness_value_label = NULL;
    }

    s_settings.brightness_editing = false;

    if(s_settings.brightness_label) {
        lv_obj_clear_flag(s_settings.brightness_label, LV_OBJ_FLAG_HIDDEN);
    }

    if(s_settings.selector) {
        lv_obj_clear_flag(s_settings.selector, LV_OBJ_FLAG_HIDDEN);
    }

    settings_update_selector(false);
}

/**
 * @brief 创建设置页。
 */
lv_obj_t *watch_settings_create(lv_obj_t *parent)
{
    auto_off_load_saved();

    memset(&s_settings, 0, sizeof(s_settings));
    s_settings.brightness_value = brightness_clamp(lcd_get_backlight_percent());

    s_settings.root = lv_obj_create(parent);
    if(s_settings.root == NULL) {
        return NULL;
    }

    lv_obj_remove_style_all(s_settings.root);
    lv_obj_set_size(s_settings.root, WATCH_SCREEN_W, WATCH_SCREEN_H);
    lv_obj_set_pos(s_settings.root, 0, 0);
    lv_obj_set_style_bg_color(s_settings.root, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_settings.root, LV_OPA_COVER, 0);
    lv_obj_clear_flag(s_settings.root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(s_settings.root, LV_OBJ_FLAG_CLICKABLE);

    s_settings.bg_img = lv_img_create(s_settings.root);
    if(s_settings.bg_img != NULL) {
        lv_img_set_src(s_settings.bg_img, &settings_bg);
        lv_obj_set_pos(s_settings.bg_img, 0, 0);
        lv_obj_clear_flag(s_settings.bg_img, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_clear_flag(s_settings.bg_img, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_move_background(s_settings.bg_img);
    }

    s_settings.back_label = settings_create_label(s_settings.root,
                                                  LV_SYMBOL_LEFT,
                                                  SETTINGS_BACK_X,
                                                  SETTINGS_BACK_Y);

    s_settings.title_label = settings_create_label(s_settings.root,
                                                   "Settings",
                                                   0,
                                                   SETTINGS_TITLE_Y);
    lv_obj_set_width(s_settings.title_label, WATCH_SCREEN_W);
    lv_obj_set_style_text_align(s_settings.title_label, LV_TEXT_ALIGN_CENTER, 0);

    s_settings.brightness_label = settings_create_label(s_settings.root,
                                                        "Brightness",
                                                        SETTINGS_ITEM_X,
                                                        SETTINGS_ITEM_START_Y);

    s_settings.screen_on_mode_label = settings_create_label(s_settings.root,
                                                           "Auto-off",
                                                           SETTINGS_ITEM_X,
                                                           SETTINGS_ITEM_START_Y + SETTINGS_ITEM_GAP);

    s_settings.auto_off_checkbox = lv_obj_create(s_settings.root);
    lv_obj_remove_style_all(s_settings.auto_off_checkbox);
    lv_obj_set_pos(s_settings.auto_off_checkbox,
                   AUTO_OFF_CHECKBOX_X,
                   AUTO_OFF_CHECKBOX_Y);
    lv_obj_set_size(s_settings.auto_off_checkbox,
                    AUTO_OFF_CHECKBOX_SIZE,
                    AUTO_OFF_CHECKBOX_SIZE);
    lv_obj_set_style_bg_opa(s_settings.auto_off_checkbox, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_color(s_settings.auto_off_checkbox, lv_color_white(), 0);
    lv_obj_set_style_border_opa(s_settings.auto_off_checkbox, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_settings.auto_off_checkbox, 2, 0);
    lv_obj_set_style_radius(s_settings.auto_off_checkbox, 2, 0);
    lv_obj_clear_flag(s_settings.auto_off_checkbox, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(s_settings.auto_off_checkbox, LV_OBJ_FLAG_CLICKABLE);

    s_settings.auto_off_check_label = settings_create_label(s_settings.auto_off_checkbox,
                                                           "",
                                                           -2,
                                                           0);
    lv_obj_set_size(s_settings.auto_off_check_label,
                    AUTO_OFF_CHECKBOX_SIZE,
                    AUTO_OFF_CHECKBOX_SIZE);
    lv_obj_set_style_text_font(s_settings.auto_off_check_label,
                               LV_FONT_DEFAULT,
                               0);
    lv_obj_set_style_text_align(s_settings.auto_off_check_label,
                                LV_TEXT_ALIGN_CENTER,
                                0);

    s_settings.screen_on_mode_target = lv_obj_create(s_settings.root);
    lv_obj_remove_style_all(s_settings.screen_on_mode_target);
    lv_obj_set_pos(s_settings.screen_on_mode_target,
                   SETTINGS_ITEM_X,
                   SETTINGS_ITEM_START_Y + SETTINGS_ITEM_GAP);
    lv_obj_set_size(s_settings.screen_on_mode_target,
                    AUTO_OFF_SELECTOR_W,
                    AUTO_OFF_SELECTOR_H);
    lv_obj_clear_flag(s_settings.screen_on_mode_target, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(s_settings.screen_on_mode_target, LV_OBJ_FLAG_CLICKABLE);
    auto_off_update_view();

    s_settings.language_label = settings_create_label(s_settings.root,
                                                      "Language",
                                                      SETTINGS_ITEM_X,
                                                      SETTINGS_ITEM_START_Y + SETTINGS_ITEM_GAP * 2);

    lv_obj_update_layout(s_settings.root);

    s_settings.language_arrow_label = settings_create_label(s_settings.root,
                                                            LV_SYMBOL_DOWN,
                                                            lv_obj_get_x(s_settings.language_label) +
                                                            lv_obj_get_width(s_settings.language_label) +
                                                            LANGUAGE_ARROW_GAP,
                                                            SETTINGS_ITEM_START_Y + SETTINGS_ITEM_GAP * 2);
    lv_obj_set_style_text_font(s_settings.language_arrow_label,
                               &lv_font_montserrat_26,
                               0);

    lv_obj_update_layout(s_settings.root);

    s_settings.language_target = lv_obj_create(s_settings.root);
    lv_obj_remove_style_all(s_settings.language_target);
    lv_obj_set_pos(s_settings.language_target,
                   SETTINGS_ITEM_X,
                   SETTINGS_ITEM_START_Y + SETTINGS_ITEM_GAP * 2);
    lv_obj_set_size(s_settings.language_target,
                    lv_obj_get_x(s_settings.language_arrow_label) +
                    lv_obj_get_width(s_settings.language_arrow_label) -
                    SETTINGS_ITEM_X,
                    LANGUAGE_TARGET_H);
    lv_obj_clear_flag(s_settings.language_target, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(s_settings.language_target, LV_OBJ_FLAG_CLICKABLE);

    s_settings.language_dropdown_box = lv_obj_create(s_settings.root);
    lv_obj_remove_style_all(s_settings.language_dropdown_box);
    lv_obj_set_style_bg_opa(s_settings.language_dropdown_box, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_color(s_settings.language_dropdown_box, lv_color_white(), 0);
    lv_obj_set_style_border_opa(s_settings.language_dropdown_box, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_settings.language_dropdown_box, SETTINGS_SELECTOR_BORDER_W, 0);
    lv_obj_set_style_radius(s_settings.language_dropdown_box, LANGUAGE_DROPDOWN_RADIUS, 0);
    lv_obj_clear_flag(s_settings.language_dropdown_box, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(s_settings.language_dropdown_box, LV_OBJ_FLAG_CLICKABLE);

    s_settings.language_dropdown_selector = lv_obj_create(s_settings.root);
    lv_obj_remove_style_all(s_settings.language_dropdown_selector);
    lv_obj_set_style_bg_color(s_settings.language_dropdown_selector, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(s_settings.language_dropdown_selector, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_settings.language_dropdown_selector, LANGUAGE_DROPDOWN_RADIUS, 0);
    lv_obj_clear_flag(s_settings.language_dropdown_selector, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(s_settings.language_dropdown_selector, LV_OBJ_FLAG_CLICKABLE);

    s_settings.language_dropdown_divider = lv_obj_create(s_settings.root);
    lv_obj_remove_style_all(s_settings.language_dropdown_divider);
    lv_obj_set_style_bg_color(s_settings.language_dropdown_divider, lv_color_hex(0x808080), 0);
    lv_obj_set_style_bg_opa(s_settings.language_dropdown_divider, LV_OPA_COVER, 0);
    lv_obj_clear_flag(s_settings.language_dropdown_divider, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(s_settings.language_dropdown_divider, LV_OBJ_FLAG_CLICKABLE);

    s_settings.language_chinese_label = settings_create_label(s_settings.root,
                                                              "Chinese",
                                                              0,
                                                              0);
    lv_obj_set_style_text_align(s_settings.language_chinese_label, LV_TEXT_ALIGN_LEFT, 0);

    s_settings.language_english_label = settings_create_label(s_settings.root,
                                                              "English",
                                                              0,
                                                              0);
    lv_obj_set_style_text_align(s_settings.language_english_label, LV_TEXT_ALIGN_LEFT, 0);

    language_dropdown_update_view();
    language_dropdown_set_hidden(true);

    s_settings.selector = lv_obj_create(s_settings.root);
    lv_obj_remove_style_all(s_settings.selector);
    lv_obj_set_style_bg_opa(s_settings.selector, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_color(s_settings.selector, lv_color_white(), 0);
    lv_obj_set_style_border_opa(s_settings.selector, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_settings.selector, SETTINGS_SELECTOR_BORDER_W, 0);
    lv_obj_set_style_radius(s_settings.selector, SETTINGS_SELECTOR_RADIUS, 0);
    lv_obj_set_style_pad_all(s_settings.selector, 0, 0);
    lv_obj_clear_flag(s_settings.selector, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(s_settings.selector, LV_OBJ_FLAG_CLICKABLE);

    settings_apply_language();
    watch_settings_reset();

    return s_settings.root;
}

/**
 * @brief 重置设置页焦点和返回状态。
 */
void watch_settings_reset(void)
{
    /* 设置页进入时的复位入口，恢复焦点、读取背光亮度，并刷新语言/选择框。
     */
    auto_off_load_saved();
    brightness_edit_exit(false);
    settings_apply_language();
    language_dropdown_close();

    s_settings.selected = SETTINGS_SEL_BACK;
    s_settings.wants_back = false;
    s_settings.brightness_value = brightness_clamp(lcd_get_backlight_percent());
    settings_update_selector(false);
}

/**
 * @brief 处理设置页按键事件。
 */
void watch_settings_on_key(watch_key_t key)
{
    /* 设置页按键状态机：根据当前焦点、编辑模式和下拉框状态解释同一组实体按键。
     */
    if(s_settings.root == NULL) {
        return;
    }

    if(s_settings.brightness_editing) {
        if(key == WATCH_KEY_1) {
            brightness_adjust(BRIGHTNESS_STEP);
            return;
        }

        if(key == WATCH_KEY_3) {
            brightness_adjust(-BRIGHTNESS_STEP);
            return;
        }

        if(key == WATCH_KEY_2) {
            brightness_edit_exit(true);
            return;
        }

        return;
    }

    if(s_settings.language_dropdown_open) {
        if(key == WATCH_KEY_1 || key == WATCH_KEY_3) {
            language_dropdown_toggle_selected();
            return;
        }

        if(key == WATCH_KEY_2) {
            language_dropdown_confirm();
            language_dropdown_close();
            return;
        }

        return;
    }

    if(key == WATCH_KEY_2) {
        if(s_settings.selected == SETTINGS_SEL_BACK) {
            s_settings.wants_back = true;
        }
        else if(s_settings.selected == SETTINGS_SEL_BRIGHTNESS) {
            brightness_edit_enter();
        }
        else if(s_settings.selected == SETTINGS_SEL_SCREEN_ON_MODE) {
            auto_off_toggle();
        }
        else if(s_settings.selected == SETTINGS_SEL_LANGUAGE) {
            language_dropdown_open();
        }
        return;
    }

    if(key == WATCH_KEY_1) {
        switch(s_settings.selected) {
        case SETTINGS_SEL_BACK:
            s_settings.selected = SETTINGS_SEL_LANGUAGE;
            break;

        case SETTINGS_SEL_BRIGHTNESS:
            s_settings.selected = SETTINGS_SEL_BACK;
            break;

        case SETTINGS_SEL_SCREEN_ON_MODE:
            s_settings.selected = SETTINGS_SEL_BRIGHTNESS;
            break;

        case SETTINGS_SEL_LANGUAGE:
            s_settings.selected = SETTINGS_SEL_SCREEN_ON_MODE;
            break;

        default:
            s_settings.selected = SETTINGS_SEL_BACK;
            break;
        }

        settings_update_selector(true);
        return;
    }

    if(key == WATCH_KEY_3) {
        switch(s_settings.selected) {
        case SETTINGS_SEL_BACK:
            s_settings.selected = SETTINGS_SEL_BRIGHTNESS;
            break;

        case SETTINGS_SEL_BRIGHTNESS:
            s_settings.selected = SETTINGS_SEL_SCREEN_ON_MODE;
            break;

        case SETTINGS_SEL_SCREEN_ON_MODE:
            s_settings.selected = SETTINGS_SEL_LANGUAGE;
            break;

        case SETTINGS_SEL_LANGUAGE:
            s_settings.selected = SETTINGS_SEL_BACK;
            break;

        default:
            s_settings.selected = SETTINGS_SEL_BACK;
            break;
        }

        settings_update_selector(true);
        return;
    }
}

/**
 * @brief 查询设置页是否请求返回表盘。
 */
bool watch_settings_wants_back(void)
{
    /* 供页面调度器查询是否请求返回表盘。
     */
    return s_settings.wants_back;
}

bool watch_settings_auto_off_enabled(void)
{
    /* 返回自动息屏开关状态，主流程用它决定是否执行空闲息屏。
     */
    auto_off_load_saved();
    return s_auto_off_enabled;
}

/**
 * @brief 销毁设置页 LVGL 对象并退出编辑状态。
 */
void watch_settings_destroy(void)
{
    /* 销毁设置页相关对象和运行状态，防止下次进入时沿用旧 LVGL 指针。
     */
    if(s_settings.root) {
        lv_obj_del(s_settings.root);
    }

    memset(&s_settings, 0, sizeof(s_settings));
}


/* 维护提示
 * 新增设置项时，请同步扩展 settings_sel_t、settings_update_selector() 和 watch_settings_on_key()。
 */
