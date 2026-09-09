/**
 * @file watch_snake.c
 * @brief 使用单个自绘棋盘和固定坐标数组实现的贪吃蛇。
 */

#include "watch_snake.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_random.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "watch_language.h"

#define WATCH_SCREEN_W             240
#define WATCH_SCREEN_H             240

#define SNAKE_GRID_SIZE            12
#define SNAKE_CELL_SIZE            14
#define SNAKE_CELL_COUNT           (SNAKE_GRID_SIZE * SNAKE_GRID_SIZE)
#define SNAKE_BOARD_SIZE           (SNAKE_GRID_SIZE * SNAKE_CELL_SIZE)
#define SNAKE_BOARD_X              36
#define SNAKE_BOARD_Y              38

#define SNAKE_INITIAL_PERIOD_MS    330
#define SNAKE_MIN_PERIOD_MS        150
#define SNAKE_SPEED_STEP_MS        20
#define SNAKE_SPEED_SCORE_STEP     3

#define SNAKE_NVS_NAMESPACE        "snake_score"
#define SNAKE_NVS_BEST_KEY         "best"

LV_FONT_DECLARE(snake_font_20);

static const char *TAG = "watch_snake";

typedef enum {
    SNAKE_STATE_READY = 0,
    SNAKE_STATE_RUNNING,
    SNAKE_STATE_PAUSED,
    SNAKE_STATE_GAME_OVER,
    SNAKE_STATE_WIN,
} snake_state_t;

typedef enum {
    SNAKE_DIR_UP = 0,
    SNAKE_DIR_RIGHT,
    SNAKE_DIR_DOWN,
    SNAKE_DIR_LEFT,
} snake_dir_t;

typedef struct {
    lv_obj_t *page;
    lv_obj_t *title;
    lv_obj_t *score_label;
    lv_obj_t *board;
    lv_obj_t *hint_label;
    lv_obj_t *overlay;
    lv_obj_t *overlay_title;
    lv_obj_t *overlay_options;
    lv_timer_t *timer;

    /* 每个格子编码为 y * 12 + x，整条蛇固定只占 144 字节。 */
    uint8_t body[SNAKE_CELL_COUNT];
    uint8_t length;
    uint8_t food;
    uint16_t score;
    uint16_t best_score;
    snake_dir_t direction;
    snake_dir_t pending_direction;
    snake_state_t state;
    bool turn_locked;
    bool secondary_selected;
    bool wants_back;

    char score_text[40];
    char overlay_text[48];
} snake_ctx_t;

static snake_ctx_t s_snake;

static const lv_font_t *snake_ui_font(void)
{
    return watch_language_is_chinese() ? &snake_font_20 : &lv_font_montserrat_20;
}

static void snake_log_heap(const char *point)
{
    ESP_LOGI(TAG, "%s: free=%u, largest=%u",
             point,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
}

static bool snake_cell_is_occupied(uint8_t cell)
{
    for(uint8_t i = 0; i < s_snake.length; i++) {
        if(s_snake.body[i] == cell) {
            return true;
        }
    }
    return false;
}

static bool snake_place_food(void)
{
    uint16_t empty_count = SNAKE_CELL_COUNT - s_snake.length;
    if(empty_count == 0) {
        return false;
    }

    uint16_t target = (uint16_t)(esp_random() % empty_count);
    for(uint16_t cell = 0; cell < SNAKE_CELL_COUNT; cell++) {
        if(snake_cell_is_occupied((uint8_t)cell)) {
            continue;
        }
        if(target == 0) {
            s_snake.food = (uint8_t)cell;
            return true;
        }
        target--;
    }

    return false;
}

static esp_err_t snake_nvs_open(nvs_open_mode_t mode, nvs_handle_t *handle)
{
    esp_err_t err = nvs_open(SNAKE_NVS_NAMESPACE, mode, handle);
    if(err == ESP_ERR_NVS_NOT_INITIALIZED) {
        err = nvs_flash_init();
        if(err == ESP_OK) {
            err = nvs_open(SNAKE_NVS_NAMESPACE, mode, handle);
        }
    }
    return err;
}

static uint16_t snake_load_best_score(void)
{
    nvs_handle_t handle;
    uint16_t value = 0;
    esp_err_t err = snake_nvs_open(NVS_READONLY, &handle);
    if(err != ESP_OK) {
        return 0;
    }

    err = nvs_get_u16(handle, SNAKE_NVS_BEST_KEY, &value);
    nvs_close(handle);
    return err == ESP_OK ? value : 0;
}

static void snake_save_best_score(void)
{
    if(s_snake.score <= s_snake.best_score) {
        return;
    }

    nvs_handle_t handle;
    esp_err_t err = snake_nvs_open(NVS_READWRITE, &handle);
    if(err == ESP_OK) {
        err = nvs_set_u16(handle, SNAKE_NVS_BEST_KEY, s_snake.score);
        if(err == ESP_OK) {
            err = nvs_commit(handle);
        }
        nvs_close(handle);
    }

    if(err == ESP_OK) {
        s_snake.best_score = s_snake.score;
    }
    else {
        ESP_LOGW(TAG, "save best score failed: %s", esp_err_to_name(err));
    }
}

static void snake_update_score_label(void)
{
    if(s_snake.score_label == NULL) {
        return;
    }

    if(watch_language_is_chinese()) {
        snprintf(s_snake.score_text, sizeof(s_snake.score_text),
                 "分数 %u  最高 %u",
                 (unsigned)s_snake.score, (unsigned)s_snake.best_score);
    }
    else {
        snprintf(s_snake.score_text, sizeof(s_snake.score_text),
                 "Score %u  Best %u",
                 (unsigned)s_snake.score, (unsigned)s_snake.best_score);
    }
    lv_label_set_text_static(s_snake.score_label, s_snake.score_text);
}

static void snake_update_overlay(void)
{
    if(s_snake.overlay == NULL) {
        return;
    }

    if(s_snake.state == SNAKE_STATE_RUNNING) {
        lv_obj_add_flag(s_snake.overlay, LV_OBJ_FLAG_HIDDEN);
        return;
    }

    lv_obj_clear_flag(s_snake.overlay, LV_OBJ_FLAG_HIDDEN);
    const bool zh = watch_language_is_chinese();
    const char *title = "";
    const char *primary = "";
    const char *secondary = "";

    switch(s_snake.state) {
    case SNAKE_STATE_READY:
        title = zh ? "贪吃蛇" : "Snake";
        primary = zh ? "开始" : "Start";
        secondary = zh ? "返回" : "Back";
        break;
    case SNAKE_STATE_PAUSED:
        title = zh ? "暂停" : "Paused";
        primary = zh ? "继续" : "Continue";
        secondary = zh ? "结束" : "End";
        break;
    case SNAKE_STATE_GAME_OVER:
        title = zh ? "结束" : "Game Over";
        primary = zh ? "再来一次" : "Retry";
        secondary = zh ? "返回" : "Back";
        break;
    case SNAKE_STATE_WIN:
        title = zh ? "胜利" : "You Win";
        primary = zh ? "再来一次" : "Retry";
        secondary = zh ? "返回" : "Back";
        break;
    default:
        return;
    }

    lv_label_set_text_static(s_snake.overlay_title, title);
    snprintf(s_snake.overlay_text, sizeof(s_snake.overlay_text),
             s_snake.secondary_selected ? "%s    >%s<" : ">%s<    %s",
             primary, secondary);
    lv_label_set_text_static(s_snake.overlay_options, s_snake.overlay_text);
}

static void snake_set_timer_period(void)
{
    if(s_snake.timer == NULL) {
        return;
    }

    uint32_t reduction = (s_snake.score / SNAKE_SPEED_SCORE_STEP) * SNAKE_SPEED_STEP_MS;
    uint32_t period = reduction >= (SNAKE_INITIAL_PERIOD_MS - SNAKE_MIN_PERIOD_MS)
                    ? SNAKE_MIN_PERIOD_MS
                    : SNAKE_INITIAL_PERIOD_MS - reduction;
    lv_timer_set_period(s_snake.timer, period);
}

static void snake_finish(snake_state_t state)
{
    s_snake.state = state;
    s_snake.secondary_selected = false;
    if(s_snake.timer) {
        lv_timer_pause(s_snake.timer);
    }
    snake_save_best_score();
    snake_update_score_label();
    snake_update_overlay();
    if(s_snake.board) {
        lv_obj_invalidate(s_snake.board);
    }
}

static void snake_step(void)
{
    s_snake.direction = s_snake.pending_direction;

    int x = s_snake.body[0] % SNAKE_GRID_SIZE;
    int y = s_snake.body[0] / SNAKE_GRID_SIZE;
    switch(s_snake.direction) {
    case SNAKE_DIR_UP:    y--; break;
    case SNAKE_DIR_RIGHT: x++; break;
    case SNAKE_DIR_DOWN:  y++; break;
    case SNAKE_DIR_LEFT:  x--; break;
    }

    if(x < 0 || x >= SNAKE_GRID_SIZE || y < 0 || y >= SNAKE_GRID_SIZE) {
        snake_finish(SNAKE_STATE_GAME_OVER);
        return;
    }

    uint8_t new_head = (uint8_t)(y * SNAKE_GRID_SIZE + x);
    bool grows = new_head == s_snake.food;
    uint8_t collision_count = grows ? s_snake.length : (uint8_t)(s_snake.length - 1);
    for(uint8_t i = 0; i < collision_count; i++) {
        if(s_snake.body[i] == new_head) {
            snake_finish(SNAKE_STATE_GAME_OVER);
            return;
        }
    }

    if(grows) {
        for(uint8_t i = s_snake.length; i > 0; i--) {
            s_snake.body[i] = s_snake.body[i - 1];
        }
        s_snake.length++;
        s_snake.score++;
        s_snake.body[0] = new_head;
        snake_update_score_label();
        snake_set_timer_period();

        if(s_snake.length == SNAKE_CELL_COUNT || !snake_place_food()) {
            snake_finish(SNAKE_STATE_WIN);
            return;
        }
    }
    else {
        for(uint8_t i = (uint8_t)(s_snake.length - 1); i > 0; i--) {
            s_snake.body[i] = s_snake.body[i - 1];
        }
        s_snake.body[0] = new_head;
    }

    /* 一个移动节拍只接受一次转向，防止拨轮长按重复触发。 */
    s_snake.turn_locked = false;
    lv_obj_invalidate(s_snake.board);
}

static void snake_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    if(s_snake.state == SNAKE_STATE_RUNNING) {
        snake_step();
    }
}

static void snake_draw_cell(lv_layer_t *layer, const lv_area_t *board_coords,
                            uint8_t cell, lv_color_t color, int radius)
{
    int x = cell % SNAKE_GRID_SIZE;
    int y = cell / SNAKE_GRID_SIZE;
    lv_area_t area = {
        .x1 = board_coords->x1 + x * SNAKE_CELL_SIZE + 2,
        .y1 = board_coords->y1 + y * SNAKE_CELL_SIZE + 2,
        .x2 = board_coords->x1 + (x + 1) * SNAKE_CELL_SIZE - 3,
        .y2 = board_coords->y1 + (y + 1) * SNAKE_CELL_SIZE - 3,
    };
    lv_draw_rect_dsc_t dsc;
    lv_draw_rect_dsc_init(&dsc);
    dsc.base.layer = layer;
    dsc.bg_color = color;
    dsc.bg_opa = LV_OPA_COVER;
    dsc.radius = radius;
    lv_draw_rect(layer, &dsc, &area);
}

static void snake_board_draw_cb(lv_event_t *event)
{
    if(lv_event_get_code(event) != LV_EVENT_DRAW_MAIN || s_snake.board == NULL) {
        return;
    }

    lv_layer_t *layer = lv_event_get_layer(event);
    lv_area_t coords;
    lv_obj_get_coords(s_snake.board, &coords);

    snake_draw_cell(layer, &coords, s_snake.food,
                    lv_color_hex(0xFF5A5F), LV_RADIUS_CIRCLE);
    for(uint8_t i = s_snake.length; i > 0; i--) {
        uint8_t index = (uint8_t)(i - 1);
        snake_draw_cell(layer, &coords, s_snake.body[index],
                        index == 0 ? lv_color_hex(0xB7F34A) : lv_color_hex(0x46C96F),
                        index == 0 ? 4 : 2);
    }
}

static void snake_prepare_round(void)
{
    const uint8_t center_y = SNAKE_GRID_SIZE / 2;
    const uint8_t center_x = SNAKE_GRID_SIZE / 2;
    s_snake.length = 3;
    s_snake.body[0] = center_y * SNAKE_GRID_SIZE + center_x;
    s_snake.body[1] = center_y * SNAKE_GRID_SIZE + center_x - 1;
    s_snake.body[2] = center_y * SNAKE_GRID_SIZE + center_x - 2;
    s_snake.direction = SNAKE_DIR_RIGHT;
    s_snake.pending_direction = SNAKE_DIR_RIGHT;
    s_snake.score = 0;
    s_snake.turn_locked = false;
    s_snake.secondary_selected = false;
    s_snake.wants_back = false;
    (void)snake_place_food();
    snake_set_timer_period();
    snake_update_score_label();
    if(s_snake.board) {
        lv_obj_invalidate(s_snake.board);
    }
}

lv_obj_t *watch_snake_create(lv_obj_t *parent)
{
    if(parent == NULL) {
        return NULL;
    }
    if(s_snake.page != NULL) {
        watch_snake_destroy();
    }

    memset(&s_snake, 0, sizeof(s_snake));
    snake_log_heap("before create");
    s_snake.best_score = snake_load_best_score();

    s_snake.page = lv_obj_create(parent);
    if(s_snake.page == NULL) {
        return NULL;
    }
    lv_obj_remove_style_all(s_snake.page);
    lv_obj_set_size(s_snake.page, WATCH_SCREEN_W, WATCH_SCREEN_H);
    lv_obj_set_style_bg_color(s_snake.page, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_snake.page, LV_OPA_COVER, 0);
    lv_obj_clear_flag(s_snake.page, LV_OBJ_FLAG_SCROLLABLE);

    s_snake.title = lv_label_create(s_snake.page);
    s_snake.score_label = lv_label_create(s_snake.page);
    s_snake.board = lv_obj_create(s_snake.page);
    s_snake.hint_label = lv_label_create(s_snake.page);
    s_snake.overlay = lv_obj_create(s_snake.page);
    s_snake.overlay_title = lv_label_create(s_snake.overlay);
    s_snake.overlay_options = lv_label_create(s_snake.overlay);
    if(s_snake.title == NULL || s_snake.score_label == NULL || s_snake.board == NULL ||
       s_snake.hint_label == NULL || s_snake.overlay == NULL ||
       s_snake.overlay_title == NULL || s_snake.overlay_options == NULL) {
        watch_snake_destroy();
        return NULL;
    }

    const lv_font_t *font = snake_ui_font();
    lv_obj_set_style_text_font(s_snake.title, font, 0);
    lv_obj_set_style_text_color(s_snake.title, lv_color_white(), 0);
    lv_label_set_text_static(s_snake.title, watch_language_is_chinese() ? "贪吃蛇" : "Snake");
    lv_obj_set_pos(s_snake.title, 12, 7);

    lv_obj_set_style_text_font(s_snake.score_label, font, 0);
    lv_obj_set_style_text_color(s_snake.score_label, lv_color_white(), 0);
    lv_obj_set_style_text_align(s_snake.score_label, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_size(s_snake.score_label, 150, 26);
    lv_obj_set_pos(s_snake.score_label, 82, 7);

    lv_obj_remove_style_all(s_snake.board);
    lv_obj_set_size(s_snake.board, SNAKE_BOARD_SIZE, SNAKE_BOARD_SIZE);
    lv_obj_set_pos(s_snake.board, SNAKE_BOARD_X, SNAKE_BOARD_Y);
    lv_obj_set_style_bg_color(s_snake.board, lv_color_hex(0x111827), 0);
    lv_obj_set_style_bg_opa(s_snake.board, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(s_snake.board, lv_color_hex(0x506070), 0);
    lv_obj_set_style_border_width(s_snake.board, 1, 0);
    lv_obj_clear_flag(s_snake.board, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(s_snake.board, snake_board_draw_cb, LV_EVENT_DRAW_MAIN, NULL);

    lv_obj_set_style_text_font(s_snake.hint_label, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(s_snake.hint_label, lv_color_hex(0xB8C0CC), 0);
    lv_label_set_text_static(s_snake.hint_label, "K1 <   K2 ||   K3 >");
    lv_obj_align(s_snake.hint_label, LV_ALIGN_BOTTOM_MID, 0, -5);

    lv_obj_remove_style_all(s_snake.overlay);
    lv_obj_set_size(s_snake.overlay, 192, 94);
    lv_obj_set_pos(s_snake.overlay, 24, 76);
    lv_obj_set_style_bg_color(s_snake.overlay, lv_color_hex(0x202733), 0);
    lv_obj_set_style_bg_opa(s_snake.overlay, LV_OPA_90, 0);
    lv_obj_set_style_border_color(s_snake.overlay, lv_color_white(), 0);
    lv_obj_set_style_border_width(s_snake.overlay, 1, 0);
    lv_obj_set_style_radius(s_snake.overlay, 10, 0);
    lv_obj_clear_flag(s_snake.overlay, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_set_style_text_font(s_snake.overlay_title, font, 0);
    lv_obj_set_style_text_color(s_snake.overlay_title, lv_color_white(), 0);
    lv_obj_align(s_snake.overlay_title, LV_ALIGN_TOP_MID, 0, 12);

    lv_obj_set_style_text_font(s_snake.overlay_options, font, 0);
    lv_obj_set_style_text_color(s_snake.overlay_options, lv_color_hex(0xB7F34A), 0);
    lv_obj_set_style_text_align(s_snake.overlay_options, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(s_snake.overlay_options, 180);
    lv_obj_align(s_snake.overlay_options, LV_ALIGN_BOTTOM_MID, 0, -12);

    s_snake.timer = lv_timer_create(snake_timer_cb, SNAKE_INITIAL_PERIOD_MS, NULL);
    if(s_snake.timer == NULL) {
        watch_snake_destroy();
        return NULL;
    }
    lv_timer_pause(s_snake.timer);

    watch_snake_reset();
    snake_log_heap("after create");
    return s_snake.page;
}

void watch_snake_reset(void)
{
    if(s_snake.page == NULL) {
        return;
    }
    if(s_snake.timer) {
        lv_timer_pause(s_snake.timer);
    }
    snake_prepare_round();
    s_snake.state = SNAKE_STATE_READY;
    snake_update_overlay();
}

void watch_snake_start(void)
{
    if(s_snake.page == NULL || s_snake.timer == NULL) {
        return;
    }
    snake_prepare_round();
    s_snake.state = SNAKE_STATE_RUNNING;
    snake_update_overlay();
    lv_timer_reset(s_snake.timer);
    lv_timer_resume(s_snake.timer);
    snake_log_heap("round started");
}

void watch_snake_on_key(watch_key_t key)
{
    if(s_snake.page == NULL || key == WATCH_KEY_2_RELEASE) {
        return;
    }

    if(s_snake.state == SNAKE_STATE_RUNNING) {
        if(key == WATCH_KEY_2) {
            s_snake.state = SNAKE_STATE_PAUSED;
            s_snake.secondary_selected = false;
            lv_timer_pause(s_snake.timer);
            snake_update_overlay();
        }
        else if(!s_snake.turn_locked && (key == WATCH_KEY_1 || key == WATCH_KEY_3)) {
            int turn = key == WATCH_KEY_1 ? -1 : 1;
            s_snake.pending_direction = (snake_dir_t)((s_snake.direction + turn + 4) % 4);
            s_snake.turn_locked = true;
        }
        return;
    }

    if(key == WATCH_KEY_1 || key == WATCH_KEY_3) {
        s_snake.secondary_selected = !s_snake.secondary_selected;
        snake_update_overlay();
        return;
    }

    if(key != WATCH_KEY_2) {
        return;
    }

    if(s_snake.state == SNAKE_STATE_READY) {
        if(s_snake.secondary_selected) {
            s_snake.wants_back = true;
        }
        else {
            watch_snake_start();
        }
    }
    else if(s_snake.state == SNAKE_STATE_PAUSED) {
        if(s_snake.secondary_selected) {
            snake_save_best_score();
            s_snake.wants_back = true;
        }
        else {
            s_snake.state = SNAKE_STATE_RUNNING;
            snake_update_overlay();
            lv_timer_reset(s_snake.timer);
            lv_timer_resume(s_snake.timer);
        }
    }
    else if(s_snake.state == SNAKE_STATE_GAME_OVER || s_snake.state == SNAKE_STATE_WIN) {
        if(s_snake.secondary_selected) {
            s_snake.wants_back = true;
        }
        else {
            watch_snake_start();
        }
    }
}

bool watch_snake_wants_back(void)
{
    return s_snake.wants_back;
}

void watch_snake_destroy(void)
{
    if(s_snake.page == NULL) {
        return;
    }

    snake_log_heap("before destroy");
    if(s_snake.timer) {
        lv_timer_delete(s_snake.timer);
        s_snake.timer = NULL;
    }
    lv_obj_del(s_snake.page);
    s_snake.page = NULL;
    memset(&s_snake, 0, sizeof(s_snake));
    snake_log_heap("after destroy");
}
