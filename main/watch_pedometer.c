#include "watch_pedometer.h"

#include <stdio.h>
#include <string.h>

#include "watch_steps.h"

#define SCREEN_W          240
#define SCREEN_H          240
#define HISTORY_ROWS      8
#define ROW_Y             43
#define ROW_H             23
#define CURSOR_PAD_X      7
#define CURSOR_PAD_Y      2

LV_IMG_DECLARE(step_title);
LV_IMG_DECLARE(steps_today_caption);
LV_IMG_DECLARE(steps_history_label);

typedef enum {
    PEDOMETER_TODAY,
    PEDOMETER_HISTORY,
} pedometer_mode_t;

typedef struct {
    lv_obj_t *page;
    lv_obj_t *back;
    lv_obj_t *title;
    lv_obj_t *today_caption;
    lv_obj_t *today_value;
    lv_obj_t *history_button;
    lv_obj_t *history_button_text;
    lv_obj_t *rows[HISTORY_ROWS];
    lv_obj_t *cursor;
    lv_timer_t *timer;
    watch_step_day_t history[WATCH_STEPS_HISTORY_DAYS];
    size_t history_count;
    size_t history_top;
    int history_selected;
    bool today_back_selected;
    bool wants_back;
    pedometer_mode_t mode;
} pedometer_ctx_t;

static pedometer_ctx_t s_pedometer;

static void style_plain(lv_obj_t *obj)
{
    lv_obj_remove_style_all(obj);
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_CLICKABLE);
}

static void color_alpha_image(lv_obj_t *image)
{
    lv_obj_set_style_image_recolor(image, lv_color_white(), 0);
    lv_obj_set_style_image_recolor_opa(image, LV_OPA_COVER, 0);
}

static void cursor_to(lv_obj_t *target)
{
    if(target == NULL || s_pedometer.cursor == NULL) return;
    lv_obj_update_layout(s_pedometer.page);
    lv_obj_set_pos(s_pedometer.cursor,
                   lv_obj_get_x(target) - CURSOR_PAD_X,
                   lv_obj_get_y(target) - CURSOR_PAD_Y);
    lv_obj_set_size(s_pedometer.cursor,
                    lv_obj_get_width(target) + CURSOR_PAD_X * 2,
                    lv_obj_get_height(target) + CURSOR_PAD_Y * 2);
    lv_obj_move_foreground(s_pedometer.cursor);
}

static void update_history_rows(void)
{
    s_pedometer.history_count = watch_steps_get_history(
        s_pedometer.history, WATCH_STEPS_HISTORY_DAYS);
    if(s_pedometer.history_count == 0) {
        s_pedometer.history_selected = -1;
        s_pedometer.history_top = 0;
    } else if(s_pedometer.history_selected >= (int)s_pedometer.history_count) {
        s_pedometer.history_selected = (int)s_pedometer.history_count - 1;
    }

    for(size_t row = 0; row < HISTORY_ROWS; ++row) {
        size_t index = s_pedometer.history_top + row;
        if(index >= s_pedometer.history_count) {
            lv_obj_add_flag(s_pedometer.rows[row], LV_OBJ_FLAG_HIDDEN);
            continue;
        }
        uint32_t date = s_pedometer.history[index].date_key;
        char text[24];
        snprintf(text, sizeof(text), "%lu.%lu: %lu",
                 (unsigned long)((date / 100U) % 100U),
                 (unsigned long)(date % 100U),
                 (unsigned long)s_pedometer.history[index].steps);
        lv_label_set_text(s_pedometer.rows[row], text);
        lv_obj_clear_flag(s_pedometer.rows[row], LV_OBJ_FLAG_HIDDEN);
    }
}

static void update_cursor(void)
{
    if(s_pedometer.mode == PEDOMETER_TODAY) {
        cursor_to(s_pedometer.today_back_selected ?
                  s_pedometer.back : s_pedometer.history_button);
        return;
    }
    if(s_pedometer.history_selected < 0) {
        cursor_to(s_pedometer.back);
        return;
    }
    size_t row = (size_t)s_pedometer.history_selected - s_pedometer.history_top;
    if(row < HISTORY_ROWS) cursor_to(s_pedometer.rows[row]);
}

static void show_mode(pedometer_mode_t mode)
{
    s_pedometer.mode = mode;
    bool today = mode == PEDOMETER_TODAY;
    lv_img_set_src(s_pedometer.title,
                   today ? &step_title : &steps_history_label);
    lv_obj_align(s_pedometer.title, LV_ALIGN_TOP_MID, 0, 3);

    lv_obj_t *today_objects[] = {
        s_pedometer.today_caption, s_pedometer.today_value,
        s_pedometer.history_button, s_pedometer.history_button_text,
    };
    for(size_t i = 0; i < sizeof(today_objects) / sizeof(today_objects[0]); ++i) {
        if(today) lv_obj_clear_flag(today_objects[i], LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(today_objects[i], LV_OBJ_FLAG_HIDDEN);
    }
    for(size_t i = 0; i < HISTORY_ROWS; ++i) {
        if(today) lv_obj_add_flag(s_pedometer.rows[i], LV_OBJ_FLAG_HIDDEN);
    }
    if(!today) update_history_rows();
    update_cursor();
}

static void refresh(lv_timer_t *timer)
{
    (void)timer;
    watch_steps_snapshot_t snapshot;
    watch_steps_get_snapshot(&snapshot);
    char value[16];
    if(snapshot.running)
        snprintf(value, sizeof(value), "%lu", (unsigned long)snapshot.today);
    else
        strcpy(value, "--");
    lv_label_set_text(s_pedometer.today_value, value);
    if(s_pedometer.mode == PEDOMETER_HISTORY) update_history_rows();
    update_cursor();
}

lv_obj_t *watch_pedometer_create(lv_obj_t *parent)
{
    if(s_pedometer.page != NULL) watch_pedometer_destroy();
    memset(&s_pedometer, 0, sizeof(s_pedometer));

    s_pedometer.page = lv_obj_create(parent);
    style_plain(s_pedometer.page);
    lv_obj_set_size(s_pedometer.page, SCREEN_W, SCREEN_H);
    lv_obj_set_style_bg_color(s_pedometer.page, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_pedometer.page, LV_OPA_COVER, 0);

    s_pedometer.back = lv_label_create(s_pedometer.page);
    lv_obj_set_style_text_font(s_pedometer.back, &lv_font_montserrat_26, 0);
    lv_obj_set_style_text_color(s_pedometer.back, lv_color_white(), 0);
    lv_label_set_text_static(s_pedometer.back, LV_SYMBOL_LEFT);
    lv_obj_set_pos(s_pedometer.back, 12, 7);

    s_pedometer.title = lv_img_create(s_pedometer.page);
    lv_img_set_src(s_pedometer.title, &step_title);
    color_alpha_image(s_pedometer.title);
    lv_obj_align(s_pedometer.title, LV_ALIGN_TOP_MID, 0, 3);

    s_pedometer.today_caption = lv_img_create(s_pedometer.page);
    lv_img_set_src(s_pedometer.today_caption, &steps_today_caption);
    color_alpha_image(s_pedometer.today_caption);
    lv_obj_set_pos(s_pedometer.today_caption, 16, 78);

    s_pedometer.today_value = lv_label_create(s_pedometer.page);
    lv_obj_set_style_text_font(s_pedometer.today_value, &lv_font_montserrat_34, 0);
    lv_obj_set_style_text_color(s_pedometer.today_value, lv_color_hex(0x42D65C), 0);
    lv_obj_set_style_text_align(s_pedometer.today_value, LV_TEXT_ALIGN_LEFT, 0);
    lv_obj_set_size(s_pedometer.today_value, 102, 40);
    lv_obj_set_pos(s_pedometer.today_value, 130, 74);

    s_pedometer.history_button = lv_obj_create(s_pedometer.page);
    style_plain(s_pedometer.history_button);
    lv_obj_set_size(s_pedometer.history_button, 160, 48);
    lv_obj_set_pos(s_pedometer.history_button, 40, 147);
    lv_obj_set_style_bg_color(s_pedometer.history_button, lv_color_hex(0x183C25), 0);
    lv_obj_set_style_bg_opa(s_pedometer.history_button, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_pedometer.history_button, 10, 0);

    s_pedometer.history_button_text = lv_img_create(s_pedometer.history_button);
    lv_img_set_src(s_pedometer.history_button_text, &steps_history_label);
    color_alpha_image(s_pedometer.history_button_text);
    lv_obj_center(s_pedometer.history_button_text);

    for(size_t i = 0; i < HISTORY_ROWS; ++i) {
        s_pedometer.rows[i] = lv_label_create(s_pedometer.page);
        lv_obj_set_style_text_font(s_pedometer.rows[i], &lv_font_montserrat_20, 0);
        lv_obj_set_style_text_color(s_pedometer.rows[i], lv_color_white(), 0);
        lv_obj_set_style_text_align(s_pedometer.rows[i], LV_TEXT_ALIGN_LEFT, 0);
        lv_obj_set_pos(s_pedometer.rows[i], 48, ROW_Y + (int)i * ROW_H);
        lv_label_set_text_static(s_pedometer.rows[i], "");
        lv_obj_add_flag(s_pedometer.rows[i], LV_OBJ_FLAG_HIDDEN);
    }

    s_pedometer.cursor = lv_obj_create(s_pedometer.page);
    style_plain(s_pedometer.cursor);
    lv_obj_set_style_bg_opa(s_pedometer.cursor, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_color(s_pedometer.cursor, lv_color_white(), 0);
    lv_obj_set_style_border_opa(s_pedometer.cursor, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_pedometer.cursor, 2, 0);
    lv_obj_set_style_radius(s_pedometer.cursor, 6, 0);

    s_pedometer.timer = lv_timer_create(refresh, 500, NULL);
    watch_pedometer_reset();
    refresh(NULL);
    return s_pedometer.page;
}

void watch_pedometer_reset(void)
{
    if(s_pedometer.page == NULL) return;
    s_pedometer.wants_back = false;
    s_pedometer.today_back_selected = true;
    s_pedometer.history_selected = -1;
    s_pedometer.history_top = 0;
    show_mode(PEDOMETER_TODAY);
}

void watch_pedometer_on_key(watch_key_t key)
{
    if(s_pedometer.page == NULL || key == WATCH_KEY_2_RELEASE) return;
    if(s_pedometer.mode == PEDOMETER_TODAY) {
        if(key == WATCH_KEY_1 || key == WATCH_KEY_3) {
            s_pedometer.today_back_selected = !s_pedometer.today_back_selected;
            update_cursor();
        } else if(key == WATCH_KEY_2) {
            if(s_pedometer.today_back_selected) s_pedometer.wants_back = true;
            else {
                s_pedometer.history_selected = -1;
                s_pedometer.history_top = 0;
                show_mode(PEDOMETER_HISTORY);
            }
        }
        return;
    }

    if(key == WATCH_KEY_1) {
        if(s_pedometer.history_selected == 0) {
            s_pedometer.history_selected = -1;
            s_pedometer.history_top = 0;
        } else if(s_pedometer.history_selected > 0) {
            s_pedometer.history_selected--;
            if((size_t)s_pedometer.history_selected < s_pedometer.history_top)
                s_pedometer.history_top--;
        }
    } else if(key == WATCH_KEY_3) {
        if(s_pedometer.history_count == 0) {
            s_pedometer.history_selected = -1;
        } else if(s_pedometer.history_selected < 0) {
            s_pedometer.history_selected = 0;
        } else if((size_t)(s_pedometer.history_selected + 1) < s_pedometer.history_count) {
            s_pedometer.history_selected++;
            if((size_t)s_pedometer.history_selected >= s_pedometer.history_top + HISTORY_ROWS)
                s_pedometer.history_top++;
        } else {
            s_pedometer.history_selected = 0;
            s_pedometer.history_top = 0;
        }
    } else if(key == WATCH_KEY_2 && s_pedometer.history_selected < 0) {
        s_pedometer.today_back_selected = false;
        show_mode(PEDOMETER_TODAY);
        return;
    }
    update_history_rows();
    update_cursor();
}

bool watch_pedometer_wants_back(void)
{
    return s_pedometer.wants_back;
}

void watch_pedometer_destroy(void)
{
    if(s_pedometer.timer != NULL) lv_timer_delete(s_pedometer.timer);
    if(s_pedometer.page != NULL) lv_obj_del(s_pedometer.page);
    memset(&s_pedometer, 0, sizeof(s_pedometer));
}
