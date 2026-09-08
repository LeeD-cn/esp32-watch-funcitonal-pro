/**
 * @file watch_plane_war_scores.c
 * @brief 飞机大战历史分数页面及 NVS 分数存储。
 */
/*
 * ==================== 模块说明 ====================
 *  * 模块职责：
 * - 飞机大战历史分数页面，负责加载、排序、保存和显示最高分记录。
 * - 分数保存在 NVS 的 plane_scores 命名空间中，默认保留 3 条记录。
 * - 提交新分数时会插入到缓存数组并按从高到低保留前三名。
 * - 页面支持中英文标题，并提供返回焦点供游戏中心状态机读取。
 *
 * 阅读建议：
 * - 先看本文件顶部的宏定义和静态状态变量，理解硬件参数和运行状态。
 * - 再看 reset/init/start/on_key/destroy 等对外函数，理解页面或驱动的生命周期。
 * - 最后看 static 辅助函数，了解具体寄存器读写、UI 刷新或状态机细节。
 * =======================================================
 */

#include "watch_plane_war_scores.h"
#include "watch_language.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "nvs.h"
#include "nvs_flash.h"

#define WATCH_SCREEN_W              240
#define WATCH_SCREEN_H              240

#define SCORES_TOP_Y                8
#define SCORES_BACK_X               12
#define SCORES_SELECTOR_PAD_X       7
#define SCORES_SELECTOR_PAD_Y       4
#define SCORES_SELECTOR_RADIUS      8
#define SCORES_SELECTOR_BORDER_W    2

#define SCORES_ROW_COUNT            3
#define SCORES_ROW_X                24
#define SCORES_ROW_W                192
#define SCORES_ROW_H                40
#define SCORES_ROW_R                10
#define SCORES_ROW_Y                72
#define SCORES_ROW_GAP              14

#define SCORES_NVS_NAMESPACE        "plane_scores"
#define SCORES_NVS_KEY_0            "score0"
#define SCORES_NVS_KEY_1            "score1"
#define SCORES_NVS_KEY_2            "score2"

#define SCORES_TEXT_BUF_LEN         12

LV_FONT_DECLARE(cn_font_26);

/**
 * @brief 历史分数页对象和缓存。
 */
typedef struct {
    lv_obj_t *page;
    lv_obj_t *back_btn;
    lv_obj_t *title;
    lv_obj_t *cursor;
    lv_obj_t *row_bg[SCORES_ROW_COUNT];
    lv_obj_t *score_label[SCORES_ROW_COUNT];

    int cached_scores[SCORES_ROW_COUNT];
    char score_text[SCORES_ROW_COUNT][SCORES_TEXT_BUF_LEN];

    bool wants_back;
} plane_war_scores_ctx_t;

/* 飞机大战历史分数页上下文。 */
static plane_war_scores_ctx_t s_plane_war_scores;

/**
 * @brief plane_war_scores_title_font 辅助函数。
 *
 * 详细说明：
 * - 封装局部逻辑，使主流程更清晰。
 *
 * @return 函数执行结果或计算得到的值，具体语义见返回路径。
 */
static const lv_font_t *plane_war_scores_title_font(void)
{
    return watch_language_is_chinese() ? &cn_font_26 : &lv_font_montserrat_26;
}

/**
 * @brief plane_war_scores_title_text 辅助函数。
 *
 * 详细说明：
 * - 封装局部逻辑，使主流程更清晰。
 *
 * @return 函数执行结果或计算得到的值，具体语义见返回路径。
 */
static const char *plane_war_scores_title_text(void)
{
    return watch_language_is_chinese() ? "历史记录" : "Game Scores";
}

/**
 * @brief plane_war_scores_apply_language 辅助函数。
 *
 * 详细说明：
 * - 封装局部逻辑，使主流程更清晰。
 */
static void plane_war_scores_apply_language(void)
{
    if(s_plane_war_scores.title == NULL) {
        return;
    }

    lv_obj_set_style_text_font(s_plane_war_scores.title,
                               plane_war_scores_title_font(),
                               0);
    lv_label_set_text(s_plane_war_scores.title,
                      plane_war_scores_title_text());
    lv_obj_align(s_plane_war_scores.title, LV_ALIGN_TOP_MID, 0, SCORES_TOP_Y);
}

static const char *s_score_keys[SCORES_ROW_COUNT] = {
    SCORES_NVS_KEY_0,
    SCORES_NVS_KEY_1,
    SCORES_NVS_KEY_2,
};

/**
 * @brief 打开历史分数 NVS 命名空间。
 *
 * 详细说明：
 * - NVS 未初始化时先初始化，再重新打开。
 *
 * @param mode 输入或输出参数，具体含义见函数内部使用方式。
 * @param handle 输入或输出参数，具体含义见函数内部使用方式。
 */
static esp_err_t plane_war_scores_nvs_open(nvs_open_mode_t mode,
                                           nvs_handle_t *handle)
{
    esp_err_t err = nvs_open(SCORES_NVS_NAMESPACE, mode, handle);

    if(err == ESP_ERR_NVS_NOT_INITIALIZED) {
        err = nvs_flash_init();
        if(err != ESP_OK) {
            return err;
        }

        err = nvs_open(SCORES_NVS_NAMESPACE, mode, handle);
    }

    return err;
}

/**
 * @brief 从 NVS 加载历史分数。
 *
 * 详细说明：
 * - 读取失败的条目按 0 处理，保证页面有默认值。
 *
 * @param scores 输入或输出参数，具体含义见调用处和函数内部使用方式。
 */
void watch_plane_war_scores_load(int scores[SCORES_ROW_COUNT])
{
    if(scores == NULL) {
        return;
    }

    for(int i = 0; i < SCORES_ROW_COUNT; i++) {
        scores[i] = 0;
    }

    nvs_handle_t handle;
    esp_err_t err = plane_war_scores_nvs_open(NVS_READONLY, &handle);
    if(err != ESP_OK) {
        return;
    }

    for(int i = 0; i < SCORES_ROW_COUNT; i++) {
        int32_t value = 0;
        err = nvs_get_i32(handle, s_score_keys[i], &value);
        if(err == ESP_OK) {
            scores[i] = (int)value;
        }
    }

    nvs_close(handle);
}

/**
 * @brief 保存历史分数数组。
 *
 * 详细说明：
 * - 把当前前三名写入 NVS 并提交。
 *
 * @param scores 输入或输出参数，具体含义见调用处和函数内部使用方式。
 */
static void plane_war_scores_save(const int scores[SCORES_ROW_COUNT])
{
    if(scores == NULL) {
        return;
    }

    nvs_handle_t handle;
    esp_err_t err = plane_war_scores_nvs_open(NVS_READWRITE, &handle);
    if(err != ESP_OK) {
        return;
    }

    for(int i = 0; i < SCORES_ROW_COUNT; i++) {
        nvs_set_i32(handle, s_score_keys[i], (int32_t)scores[i]);
    }

    nvs_commit(handle);
    nvs_close(handle);
}

/**
 * @brief 提交一条新分数。
 *
 * 详细说明：
 * - 插入后按高分排序并保留前三条。
 *
 * @param score 输入或输出参数，具体含义见调用处和函数内部使用方式。
 */
void watch_plane_war_scores_submit_score(int score)
{
    if(score < 0) {
        score = 0;
    }

    int values[SCORES_ROW_COUNT + 1] = {0};
    watch_plane_war_scores_load(values);
    values[SCORES_ROW_COUNT] = score;

    for(int i = 0; i < SCORES_ROW_COUNT + 1; i++) {
        for(int j = i + 1; j < SCORES_ROW_COUNT + 1; j++) {
            if(values[j] > values[i]) {
                int tmp = values[i];
                values[i] = values[j];
                values[j] = tmp;
            }
        }
    }

    int top_scores[SCORES_ROW_COUNT];
    for(int i = 0; i < SCORES_ROW_COUNT; i++) {
        top_scores[i] = values[i];
    }

    plane_war_scores_save(top_scores);
}

/**
 * @brief 仅在分数变化时更新标签文本，减少不必要重绘。
 *
 * 详细说明：
 * - 把缓存分数格式化成排行榜行文本。
 */
static void plane_war_scores_update_labels(void)
{
    int scores[SCORES_ROW_COUNT] = {0};
    watch_plane_war_scores_load(scores);

    for(int i = 0; i < SCORES_ROW_COUNT; i++) {
        if(s_plane_war_scores.score_label[i] == NULL) {
            continue;
        }


        if(scores[i] == s_plane_war_scores.cached_scores[i]) {
            continue;
        }

        s_plane_war_scores.cached_scores[i] = scores[i];
        snprintf(s_plane_war_scores.score_text[i],
                 SCORES_TEXT_BUF_LEN,
                 "%d",
                 scores[i]);


        lv_label_set_text_static(s_plane_war_scores.score_label[i],
                                 s_plane_war_scores.score_text[i]);
    }
}

/**
 * @brief 刷新历史分数页返回焦点框。
 *
 * 详细说明：
 * - 让用户看到当前可按确认返回。
 */
static void plane_war_scores_update_cursor(void)
{
    if(s_plane_war_scores.page == NULL ||
       s_plane_war_scores.cursor == NULL ||
       s_plane_war_scores.back_btn == NULL) {
        return;
    }

    lv_obj_update_layout(s_plane_war_scores.page);

    lv_coord_t x = lv_obj_get_x(s_plane_war_scores.back_btn) - SCORES_SELECTOR_PAD_X;
    lv_coord_t y = lv_obj_get_y(s_plane_war_scores.back_btn) - SCORES_SELECTOR_PAD_Y;
    lv_coord_t w = lv_obj_get_width(s_plane_war_scores.back_btn) + SCORES_SELECTOR_PAD_X * 2;
    lv_coord_t h = lv_obj_get_height(s_plane_war_scores.back_btn) + SCORES_SELECTOR_PAD_Y * 2;

    if(w < 16 + SCORES_SELECTOR_PAD_X * 2) {
        w = 16 + SCORES_SELECTOR_PAD_X * 2;
    }

    lv_obj_set_pos(s_plane_war_scores.cursor, x, y);
    lv_obj_set_size(s_plane_war_scores.cursor, w, h);
    lv_obj_move_foreground(s_plane_war_scores.cursor);
}

/**
 * @brief watch_plane_war_scores_create 对外接口。
 *
 * 详细说明：
 * - 供其他模块调用，隐藏本文件内部状态细节。
 *
 * @param parent 输入或输出参数，具体含义见调用处和函数内部使用方式。
 *
 * @return 函数执行结果或计算得到的值，具体语义见返回路径。
 */
lv_obj_t *watch_plane_war_scores_create(lv_obj_t *parent)
{
    if(s_plane_war_scores.page) {
        watch_plane_war_scores_destroy();
    }

    memset(&s_plane_war_scores, 0, sizeof(s_plane_war_scores));

    for(int i = 0; i < SCORES_ROW_COUNT; i++) {
        s_plane_war_scores.cached_scores[i] = -1;
        strcpy(s_plane_war_scores.score_text[i], "0");
    }

    s_plane_war_scores.page = lv_obj_create(parent);
    if(s_plane_war_scores.page == NULL) {
        return NULL;
    }

    lv_obj_remove_style_all(s_plane_war_scores.page);
    lv_obj_set_size(s_plane_war_scores.page, WATCH_SCREEN_W, WATCH_SCREEN_H);
    lv_obj_set_pos(s_plane_war_scores.page, 0, 0);
    lv_obj_set_style_bg_color(s_plane_war_scores.page, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_plane_war_scores.page, LV_OPA_COVER, 0);
    lv_obj_clear_flag(s_plane_war_scores.page, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(s_plane_war_scores.page, LV_OBJ_FLAG_CLICKABLE);


    lv_obj_add_flag(s_plane_war_scores.page, LV_OBJ_FLAG_HIDDEN);

    s_plane_war_scores.back_btn = lv_label_create(s_plane_war_scores.page);
    if(s_plane_war_scores.back_btn == NULL) {
        watch_plane_war_scores_destroy();
        return NULL;
    }

    lv_obj_set_style_text_font(s_plane_war_scores.back_btn,
                               &lv_font_montserrat_26,
                               0);
    lv_obj_set_style_text_color(s_plane_war_scores.back_btn,
                                lv_color_white(),
                                0);
    lv_obj_set_style_text_align(s_plane_war_scores.back_btn,
                                LV_TEXT_ALIGN_CENTER,
                                0);
    lv_label_set_text_static(s_plane_war_scores.back_btn, LV_SYMBOL_LEFT);
    lv_obj_set_pos(s_plane_war_scores.back_btn, SCORES_BACK_X, SCORES_TOP_Y);
    lv_obj_clear_flag(s_plane_war_scores.back_btn, LV_OBJ_FLAG_CLICKABLE);

    s_plane_war_scores.title = lv_label_create(s_plane_war_scores.page);
    if(s_plane_war_scores.title == NULL) {
        watch_plane_war_scores_destroy();
        return NULL;
    }

    lv_obj_set_style_text_font(s_plane_war_scores.title,
                               plane_war_scores_title_font(),
                               0);
    lv_obj_set_style_text_color(s_plane_war_scores.title,
                                lv_color_white(),
                                0);
    lv_obj_set_style_text_align(s_plane_war_scores.title,
                                LV_TEXT_ALIGN_CENTER,
                                0);
    lv_label_set_text(s_plane_war_scores.title, plane_war_scores_title_text());
    lv_obj_align(s_plane_war_scores.title, LV_ALIGN_TOP_MID, 0, SCORES_TOP_Y);
    lv_obj_clear_flag(s_plane_war_scores.title, LV_OBJ_FLAG_CLICKABLE);

    s_plane_war_scores.cursor = lv_obj_create(s_plane_war_scores.page);
    if(s_plane_war_scores.cursor == NULL) {
        watch_plane_war_scores_destroy();
        return NULL;
    }

    lv_obj_remove_style_all(s_plane_war_scores.cursor);
    lv_obj_set_size(s_plane_war_scores.cursor, 20, 20);
    lv_obj_set_style_bg_opa(s_plane_war_scores.cursor, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_color(s_plane_war_scores.cursor, lv_color_white(), 0);
    lv_obj_set_style_border_opa(s_plane_war_scores.cursor, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_plane_war_scores.cursor, SCORES_SELECTOR_BORDER_W, 0);
    lv_obj_set_style_radius(s_plane_war_scores.cursor, SCORES_SELECTOR_RADIUS, 0);
    lv_obj_set_style_pad_all(s_plane_war_scores.cursor, 0, 0);
    lv_obj_clear_flag(s_plane_war_scores.cursor, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(s_plane_war_scores.cursor, LV_OBJ_FLAG_CLICKABLE);

    for(int i = 0; i < SCORES_ROW_COUNT; i++) {
        lv_coord_t y = SCORES_ROW_Y + i * (SCORES_ROW_H + SCORES_ROW_GAP);

        s_plane_war_scores.row_bg[i] = lv_obj_create(s_plane_war_scores.page);
        if(s_plane_war_scores.row_bg[i] == NULL) {
            watch_plane_war_scores_destroy();
            return NULL;
        }

        lv_obj_remove_style_all(s_plane_war_scores.row_bg[i]);
        lv_obj_set_size(s_plane_war_scores.row_bg[i],
                        SCORES_ROW_W,
                        SCORES_ROW_H);
        lv_obj_set_pos(s_plane_war_scores.row_bg[i], SCORES_ROW_X, y);
        lv_obj_set_style_bg_color(s_plane_war_scores.row_bg[i],
                                  lv_color_white(),
                                  0);
        lv_obj_set_style_bg_opa(s_plane_war_scores.row_bg[i], LV_OPA_30, 0);
        lv_obj_set_style_radius(s_plane_war_scores.row_bg[i], SCORES_ROW_R, 0);
        lv_obj_set_style_pad_all(s_plane_war_scores.row_bg[i], 0, 0);
        lv_obj_clear_flag(s_plane_war_scores.row_bg[i], LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_clear_flag(s_plane_war_scores.row_bg[i], LV_OBJ_FLAG_CLICKABLE);


        s_plane_war_scores.score_label[i] =
            lv_label_create(s_plane_war_scores.page);
        if(s_plane_war_scores.score_label[i] == NULL) {
            watch_plane_war_scores_destroy();
            return NULL;
        }

        lv_obj_set_style_text_font(s_plane_war_scores.score_label[i],
                                   &lv_font_montserrat_26,
                                   0);
        lv_obj_set_style_text_color(s_plane_war_scores.score_label[i],
                                    lv_color_white(),
                                    0);
        lv_obj_set_style_text_align(s_plane_war_scores.score_label[i],
                                    LV_TEXT_ALIGN_CENTER,
                                    0);
        lv_label_set_long_mode(s_plane_war_scores.score_label[i],
                               LV_LABEL_LONG_CLIP);
        lv_label_set_text_static(s_plane_war_scores.score_label[i],
                                 s_plane_war_scores.score_text[i]);
        lv_obj_set_size(s_plane_war_scores.score_label[i],
                        SCORES_ROW_W,
                        SCORES_ROW_H);
        lv_obj_set_pos(s_plane_war_scores.score_label[i],
                       SCORES_ROW_X,
                       y + 5);
        lv_obj_clear_flag(s_plane_war_scores.score_label[i],
                          LV_OBJ_FLAG_CLICKABLE);
    }

    s_plane_war_scores.wants_back = false;
    plane_war_scores_apply_language();
    plane_war_scores_update_cursor();

    return s_plane_war_scores.page;
}

/**
 * @brief 创建或重置历史分数页。
 *
 * 详细说明：
 * - 加载分数、创建行背景和文字。
 */
void watch_plane_war_scores_reset(void)
{
    if(s_plane_war_scores.page == NULL) {
        return;
    }

    s_plane_war_scores.wants_back = false;


    plane_war_scores_apply_language();
    plane_war_scores_update_cursor();
    plane_war_scores_update_labels();


    lv_obj_clear_flag(s_plane_war_scores.page, LV_OBJ_FLAG_HIDDEN);
}

/**
 * @brief 销毁历史分数页。
 *
 * 详细说明：
 * - 删除 LVGL 对象并清空上下文。
 */
void watch_plane_war_scores_destroy(void)
{
    if(s_plane_war_scores.page) {
        lv_obj_del(s_plane_war_scores.page);
        s_plane_war_scores.page = NULL;
    }

    memset(&s_plane_war_scores, 0, sizeof(s_plane_war_scores));
}

/**
 * @brief 处理历史分数页按键。
 *
 * 详细说明：
 * - 确认或返回键设置 wants_back。
 *
 * @param key 输入或输出参数，具体含义见调用处和函数内部使用方式。
 */
void watch_plane_war_scores_on_key(watch_key_t key)
{
    if(s_plane_war_scores.page == NULL) {
        return;
    }

    if(key == WATCH_KEY_2_RELEASE) {
        return;
    }

    if(key == WATCH_KEY_2) {
        s_plane_war_scores.wants_back = true;
        return;
    }
}

/**
 * @brief 查询历史分数页是否请求返回。
 *
 * 详细说明：
 * - 供游戏中心状态机使用。
 *
 * @return 函数执行结果或计算得到的值，具体语义见返回路径。
 */
bool watch_plane_war_scores_wants_back(void)
{
    return s_plane_war_scores.wants_back;
}
