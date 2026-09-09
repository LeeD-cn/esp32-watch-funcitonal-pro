/**
 * @file watch_e_card.c
 * @brief 设备信息页面。为兼容现有 UI 路由，暂时保留原模块名。
 */

#include "watch_e_card.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "watch_device_info.h"
#include "watch_language.h"

#define WATCH_SCREEN_W              240
#define WATCH_SCREEN_H              240
#define DEVICE_INFO_BG_COLOR        0x101725
#define DEVICE_INFO_CARD_COLOR      0x1d2a3d
#define DEVICE_INFO_ACCENT_COLOR    0x55c7ff
#define DEVICE_INFO_TEXT_COLOR      0xf4f7fb
#define DEVICE_INFO_MUTED_COLOR     0x9baec4

LV_FONT_DECLARE(device_info_font_20);

static const char *TAG = "watch_device_info_ui";

typedef struct {
    lv_obj_t *page;
    lv_obj_t *owner_value;
    lv_obj_t *name_value;
    lv_obj_t *id_value;
    lv_obj_t *status;
    bool wants_back;
} watch_device_info_ui_t;

static watch_device_info_ui_t s_ui;

static const char *device_info_text(const char *cn, const char *en)
{
    return watch_language_is_chinese() ? cn : en;
}

static void device_info_style_label(lv_obj_t *label,
                                    const lv_font_t *font,
                                    lv_color_t color)
{
    lv_obj_set_style_text_font(label, font, 0);
    lv_obj_set_style_text_color(label, color, 0);
    lv_obj_set_style_bg_opa(label, LV_OPA_TRANSP, 0);
}

static lv_obj_t *device_info_create_row(lv_obj_t *parent,
                                        int32_t y,
                                        const char *caption,
                                        lv_obj_t **value_out)
{
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_remove_style_all(row);
    lv_obj_set_pos(row, 12, y);
    lv_obj_set_size(row, 216, 49);
    lv_obj_set_style_bg_color(row, lv_color_hex(DEVICE_INFO_CARD_COLOR), 0);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(row, 10, 0);
    lv_obj_set_style_pad_left(row, 11, 0);
    lv_obj_set_style_pad_right(row, 9, 0);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *caption_label = lv_label_create(row);
    lv_label_set_text(caption_label, caption);
    device_info_style_label(caption_label, &device_info_font_20,
                            lv_color_hex(DEVICE_INFO_MUTED_COLOR));
    lv_obj_set_pos(caption_label, 0, 2);

    lv_obj_t *value = lv_label_create(row);
    lv_obj_set_width(value, 194);
    lv_label_set_long_mode(value, LV_LABEL_LONG_DOT);
    lv_label_set_text(value, "-");
    device_info_style_label(value, &device_info_font_20,
                            lv_color_hex(DEVICE_INFO_TEXT_COLOR));
    lv_obj_set_pos(value, 0, 24);

    if(value_out != NULL) {
        *value_out = value;
    }
    return row;
}

static void device_info_refresh(void)
{
    watch_device_info_t info;
    esp_err_t ret = watch_device_info_load(&info);
    if(ret != ESP_OK) {
        ESP_LOGW(TAG, "load failed: %s", esp_err_to_name(ret));
        if(s_ui.status != NULL) {
            lv_label_set_text(s_ui.status,
                              device_info_text("读取失败", "Load failed"));
        }
        return;
    }

    const char *owner = info.owner[0] != '\0' ? info.owner :
                        device_info_text("未设置", "Not set");
    const char *name = info.device_name[0] != '\0' ? info.device_name :
                       device_info_text("未设置", "Not set");

    lv_label_set_text(s_ui.owner_value, owner);
    lv_label_set_text(s_ui.name_value, name);
    lv_label_set_text(s_ui.id_value, info.device_id);
    lv_label_set_text(s_ui.status, device_info_text("按下返回", "Press to return"));
}

lv_obj_t *watch_e_card_create(lv_obj_t *parent)
{
    if(parent == NULL) {
        return NULL;
    }
    if(s_ui.page != NULL) {
        watch_e_card_destroy();
    }

    s_ui.page = lv_obj_create(parent);
    lv_obj_remove_style_all(s_ui.page);
    lv_obj_set_size(s_ui.page, WATCH_SCREEN_W, WATCH_SCREEN_H);
    lv_obj_set_pos(s_ui.page, 0, 0);
    lv_obj_set_style_bg_color(s_ui.page, lv_color_hex(DEVICE_INFO_BG_COLOR), 0);
    lv_obj_set_style_bg_opa(s_ui.page, LV_OPA_COVER, 0);
    lv_obj_clear_flag(s_ui.page, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title = lv_label_create(s_ui.page);
    lv_label_set_text(title, device_info_text("设备信息", "Device Info"));
    device_info_style_label(title, &device_info_font_20,
                            lv_color_hex(DEVICE_INFO_ACCENT_COLOR));
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 6);

    device_info_create_row(s_ui.page, 35,
                           device_info_text("拥有者", "Owner"),
                           &s_ui.owner_value);
    device_info_create_row(s_ui.page, 88,
                           device_info_text("设备名称", "Device name"),
                           &s_ui.name_value);
    device_info_create_row(s_ui.page, 141,
                           device_info_text("设备编号", "Device ID"),
                           &s_ui.id_value);

    s_ui.status = lv_label_create(s_ui.page);
    device_info_style_label(s_ui.status, &device_info_font_20,
                            lv_color_hex(DEVICE_INFO_MUTED_COLOR));
    lv_obj_align(s_ui.status, LV_ALIGN_BOTTOM_MID, 0, -5);

    device_info_refresh();
    return s_ui.page;
}

void watch_e_card_reset(void)
{
    s_ui.wants_back = false;
}

void watch_e_card_on_key(watch_key_t key)
{
    if(key == WATCH_KEY_2) {
        s_ui.wants_back = true;
    }
}

bool watch_e_card_wants_back(void)
{
    return s_ui.wants_back;
}

void watch_e_card_destroy(void)
{
    if(s_ui.page != NULL) {
        lv_obj_del(s_ui.page);
    }
    memset(&s_ui, 0, sizeof(s_ui));
}
