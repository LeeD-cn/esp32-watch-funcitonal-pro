/**
 * @file watch_compass.c
 * @brief 指南针页面绘制、语言切换和 QMC5883P 航向角刷新实现。
 */
/*
 * ==================== 模块说明 ====================
 *  * 模块职责：
 * - 指南针页面 UI 和 QMC5883P 航向角刷新逻辑。
 * - 页面由圆盘、60 条刻度、旋转方位、指北箭头、朝向基准和角度标签组成。
 * - 传感器角度会叠加地磁偏角，并按当前模块安装方向映射到表盘显示方向。
 * - 支持中英文方向文字切换，进入页面后通过 LVGL timer 周期刷新航向角。
 *
 * 阅读建议：
 * - 先看本文件顶部的宏定义和静态状态变量，理解硬件参数和运行状态。
 * - 再看 reset/init/start/on_key/destroy 等对外函数，理解页面或驱动的生命周期。
 * - 最后看 static 辅助函数，了解具体寄存器读写、UI 刷新或状态机细节。
 * =======================================================
 */


#include "watch_compass.h"
#include "qmc5883p.h"
#include "watch_language.h"

#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

LV_FONT_DECLARE(cn_font_26);

#define WATCH_SCREEN_W                  240
#define WATCH_SCREEN_H                  240
#define WATCH_COMPASS_TIMER_PERIOD_MS   200

#define WATCH_COMPASS_CENTER_X          (WATCH_SCREEN_W / 2)
#define WATCH_COMPASS_CENTER_Y          (WATCH_SCREEN_H / 2)
#define WATCH_COMPASS_RADIUS            100
#define WATCH_COMPASS_RING_SIZE         (WATCH_COMPASS_RADIUS * 2)
#define WATCH_COMPASS_RING_X            (WATCH_COMPASS_CENTER_X - WATCH_COMPASS_RADIUS)
#define WATCH_COMPASS_RING_Y            (WATCH_COMPASS_CENTER_Y - WATCH_COMPASS_RADIUS)

/* 外圈尺寸。 */
#define WATCH_COMPASS_OUTER_RING_RADIUS 108
#define WATCH_COMPASS_OUTER_RING_SIZE   (WATCH_COMPASS_OUTER_RING_RADIUS * 2)
#define WATCH_COMPASS_OUTER_RING_X      (WATCH_COMPASS_CENTER_X - WATCH_COMPASS_OUTER_RING_RADIUS)
#define WATCH_COMPASS_OUTER_RING_Y      (WATCH_COMPASS_CENTER_Y - WATCH_COMPASS_OUTER_RING_RADIUS)

/* 圆盘刻度配置。 */
#define WATCH_COMPASS_TICK_COUNT        60
#define WATCH_COMPASS_TICK_STEP_DEG     6.0f
#define WATCH_COMPASS_TICK_OUTER_R      98
#define WATCH_COMPASS_TICK_MINOR_IN_R   92
#define WATCH_COMPASS_TICK_MAJOR_IN_R   86
#define WATCH_COMPASS_TICK_CARD_IN_R    80
#define WATCH_COMPASS_CARDINAL_R        74
#define WATCH_COMPASS_CARDINAL_COUNT    4
#define WATCH_COMPASS_PI                3.14159265358979323846f

/* 中央指北箭头。角度与旋转圆盘使用同一屏幕坐标定义。 */
#define WATCH_COMPASS_NEEDLE_NORTH_R    53
#define WATCH_COMPASS_NEEDLE_SOUTH_R    31
#define WATCH_COMPASS_ARROW_BASE_R      37
#define WATCH_COMPASS_ARROW_HALF_W      9

/* 航向显示固定在圆盘下部，为阶段 5 的调平提示预留中心附近空间。 */
#define WATCH_COMPASS_HEADING_Y_OFFSET  54

/* 圆周指数平滑系数。用向量平滑可正确处理 359° 到 0° 的跨界。 */
#define WATCH_COMPASS_SMOOTH_ALPHA      0.24f

/**
 * @brief 地磁偏角配置, 单位为度.
 *
 * @note 东偏为正数, 西偏为负数. 例如东偏 5.8 度填 5.8f, 西偏 3.2 度填 -3.2f.
 */
#ifndef WATCH_COMPASS_DECLINATION_DEG
#define WATCH_COMPASS_DECLINATION_DEG   -3.2f
#endif

/**
 * @brief 磁力计安装方向校正角，单位为度。
 *
 * @note 手表显示值比标准指南针小 15 度，因此在磁航向上增加 15 度。
 *       该参数用于修正传感器与表盘之间的固定安装角，不属于地磁偏角。
 */
#ifndef WATCH_COMPASS_MOUNT_OFFSET_DEG
#define WATCH_COMPASS_MOUNT_OFFSET_DEG  15.0f
#endif

/**
 * @brief 正北方向分量映射.
 *
 * @note 默认认为 QMC5883P 的 X+ 方向指向表盘上方, 也就是手表正前方.
 * @note 模块安装方向不同，可调整这里的轴映射.
 */
#ifndef WATCH_COMPASS_NORTH_COMPONENT
#define WATCH_COMPASS_NORTH_COMPONENT(x, y)     (x)
#endif

/**
 * @brief 正东方向分量映射.
 *
 * @note 当前实测方向反了，因此把 Y 轴取反，使角度按顺时针从 0 度增加到 360 度.
 * @note 模块安装方向变化时，可调整这里的轴映射.
 */
#ifndef WATCH_COMPASS_EAST_COMPONENT
#define WATCH_COMPASS_EAST_COMPONENT(x, y)      (-(y))
#endif

/**
 * @brief 指南针页面上下文.
 */
typedef struct {
    /**
     * @brief 页面根对象.
     */
    lv_obj_t *page;

    /**
     * @brief 黄色外圈，套在绿色圆外面.
     */
    lv_obj_t *outer_ring;

    /**
     * @brief 半径为 100 的绿色圆.
     */
    lv_obj_t *ring;

    /**
     * @brief 60 条圆周刻度线.
     */
    lv_obj_t *tick_line[WATCH_COMPASS_TICK_COUNT];
    lv_point_precise_t tick_points[WATCH_COMPASS_TICK_COUNT][2];

    /**
     * @brief N/E/S/W 四个方向字母.
     */
    lv_obj_t *cardinal_label[WATCH_COMPASS_CARDINAL_COUNT];

    /**
     * @brief 当前方向文字是否使用中文.
     */
    bool cardinal_chinese;

    /* 指北箭头：北向红色轴线和箭头两翼，南向为灰色尾线。 */
    lv_obj_t *north_line;
    lv_obj_t *north_arrow_left;
    lv_obj_t *north_arrow_right;
    lv_obj_t *south_line;
    lv_point_precise_t north_points[2];
    lv_point_precise_t north_arrow_left_points[2];
    lv_point_precise_t north_arrow_right_points[2];
    lv_point_precise_t south_points[2];

    /**
     * @brief 中心小圆点.
     */
    lv_obj_t *center_dot;

    /**
     * @brief 屏幕顶部固定朝向基准.
     */
    lv_obj_t *reference_left;
    lv_obj_t *reference_right;
    lv_point_precise_t reference_left_points[2];
    lv_point_precise_t reference_right_points[2];

    /**
     * @brief 航向角文本对象.
     */
    lv_obj_t *heading_label;

    /**
     * @brief 指南针刷新定时器.
     */
    lv_timer_t *timer;

    /**
     * @brief 是否请求返回上级菜单.
     */
    bool wants_back;

    /**
     * @brief 传感器是否初始化完成.
     */
    bool sensor_ready;

    /* 航向圆周平滑状态，保存单位向量而不是角度以避免北向跨界跳变。 */
    bool heading_filter_ready;
    float heading_filter_x;
    float heading_filter_y;

    /**
     * @brief QMC5883P 校准参数.
     */
    qmc5883p_calibration_t calibration;
} watch_compass_ctx_t;

/**
 * @brief 指南针页面静态上下文.
 */
static watch_compass_ctx_t s_compass;

static const char *s_cardinal_text_en[WATCH_COMPASS_CARDINAL_COUNT] = {
    "N", "E", "S", "W"
};

/*
 * 数组顺序与 s_cardinal_deg 保持一致：
 * 0°=北/N，90°=东/E，180°=南/S，270°=西/W。
 */
static const char *s_cardinal_text_cn[WATCH_COMPASS_CARDINAL_COUNT] = {
    "北", "东", "南", "西"
};

static const float s_cardinal_deg[WATCH_COMPASS_CARDINAL_COUNT] = {
    0.0f, 90.0f, 180.0f, 270.0f
};

/**
 * @brief 根据当前语言刷新四个方位文字。
 *
 * 详细说明：
 * - 切换 N/E/S/W 或中文方向标识。
 *
 * @param force 输入或输出参数，具体含义见函数内部使用方式。
 */
static void watch_compass_apply_language(bool force)
{
    bool chinese = watch_language_is_chinese();
    const char * const *cardinal_text = chinese ?
        s_cardinal_text_cn :
        s_cardinal_text_en;
    const lv_font_t *cardinal_font = chinese ?
        &cn_font_26 :
        &lv_font_montserrat_26;

    if(!force && s_compass.cardinal_chinese == chinese) {
        return;
    }

    s_compass.cardinal_chinese = chinese;

    for(int i = 0; i < WATCH_COMPASS_CARDINAL_COUNT; i++) {
        if(s_compass.cardinal_label[i] == NULL) {
            continue;
        }

        lv_obj_set_style_text_font(s_compass.cardinal_label[i], cardinal_font, 0);
        lv_obj_set_style_text_color(s_compass.cardinal_label[i],
                                    i == 0 ? lv_color_hex(0xff4d5a) : lv_color_hex(0xd8e6f3),
                                    0);
        lv_label_set_text(s_compass.cardinal_label[i], cardinal_text[i]);
    }
}

/**
 * @brief 将浮点坐标四舍五入为 LVGL 坐标。
 *
 * 详细说明：
 * - 减少三角函数计算后的显示抖动。
 *
 * @param value 输入或输出参数，具体含义见函数内部使用方式。
 */
static lv_coord_t watch_compass_round_coord(float value)
{
    if(value >= 0.0f) {
        return (lv_coord_t)(value + 0.5f);
    }

    return (lv_coord_t)(value - 0.5f);
}

/**
 * @brief 将航向角归一化到 0～360 度范围。
 *
 * @param heading_deg 待归一化的航向角。
 * @return 归一化后的航向角，范围为 [0, 360)。
 */
static float watch_compass_normalize_heading(float heading_deg)
{
    while(heading_deg < 0.0f) {
        heading_deg += 360.0f;
    }

    while(heading_deg >= 360.0f) {
        heading_deg -= 360.0f;
    }

    return heading_deg;
}

/**
 * @brief 对航向角进行跨 0° 安全的圆周平滑。
 *
 * @note 平滑只改善界面抖动，不替代磁力计校准或阶段 5 的倾斜补偿。
 */
static float watch_compass_smooth_heading(float heading_deg)
{
    float heading_rad = heading_deg * WATCH_COMPASS_PI / 180.0f;
    float sample_x = cosf(heading_rad);
    float sample_y = sinf(heading_rad);

    if(!s_compass.heading_filter_ready) {
        s_compass.heading_filter_x = sample_x;
        s_compass.heading_filter_y = sample_y;
        s_compass.heading_filter_ready = true;
    }
    else {
        s_compass.heading_filter_x += WATCH_COMPASS_SMOOTH_ALPHA *
                                      (sample_x - s_compass.heading_filter_x);
        s_compass.heading_filter_y += WATCH_COMPASS_SMOOTH_ALPHA *
                                      (sample_y - s_compass.heading_filter_y);
    }

    return watch_compass_normalize_heading(
        atan2f(s_compass.heading_filter_y, s_compass.heading_filter_x) *
        180.0f / WATCH_COMPASS_PI);
}

/**
 * @brief 把“相对屏幕顶部顺时针”的角度转换成屏幕坐标.
 *
 * @param screen_angle_deg 0 度在屏幕正上方, 顺时针增加.
 * @param radius 距离圆心的半径.
 * @param x 输出 x 坐标.
 * @param y 输出 y 坐标.
 */
/**
 * @brief 将圆盘角度和半径转换为屏幕坐标。
 *
 * 详细说明：
 * - 角度以表盘中心为原点，转换后用于刻度线/文字布局。
 *
 * @param screen_angle_deg 输入或输出参数，具体含义见函数内部使用方式。
 * @param radius 输入或输出参数，具体含义见函数内部使用方式。
 * @param x 输入或输出参数，具体含义见函数内部使用方式。
 * @param y 输入或输出参数，具体含义见函数内部使用方式。
 */
static void watch_compass_polar_to_xy(float screen_angle_deg,
                                      int radius,
                                      lv_coord_t *x,
                                      lv_coord_t *y)
{
    float rad = screen_angle_deg * WATCH_COMPASS_PI / 180.0f;

    if(x == NULL || y == NULL) {
        return;
    }

    *x = watch_compass_round_coord((float)WATCH_COMPASS_CENTER_X + sinf(rad) * (float)radius);
    *y = watch_compass_round_coord((float)WATCH_COMPASS_CENTER_Y - cosf(rad) * (float)radius);
}

/**
 * @brief 显示指南针文本.
 *
 * @param text 待显示文本.
 */
/**
 * @brief 更新中心航向文本。
 *
 * 详细说明：
 * - 统一设置字体、颜色、位置和文本。
 *
 * @param text 输入或输出参数，具体含义见函数内部使用方式。
 */
static void watch_compass_show_text(const char *text)
{
    if(s_compass.heading_label == NULL || text == NULL) {
        return;
    }

    lv_label_set_text(s_compass.heading_label, text);
    lv_obj_align(s_compass.heading_label, LV_ALIGN_CENTER, 0, WATCH_COMPASS_HEADING_Y_OFFSET);
}

/**
 * @brief 设置方向图形是否可见。
 */
static void watch_compass_set_direction_visible(bool visible)
{
    lv_obj_t *objects[] = {
        s_compass.north_line,
        s_compass.north_arrow_left,
        s_compass.north_arrow_right,
        s_compass.south_line,
        s_compass.center_dot,
    };

    for(size_t i = 0; i < sizeof(objects) / sizeof(objects[0]); i++) {
        if(objects[i] == NULL) {
            continue;
        }

        if(visible) {
            lv_obj_clear_flag(objects[i], LV_OBJ_FLAG_HIDDEN);
        }
        else {
            lv_obj_add_flag(objects[i], LV_OBJ_FLAG_HIDDEN);
        }
    }
}

/**
 * @brief 显示传感器错误并隐藏无效方向箭头。
 */
static void watch_compass_show_error(const char *text)
{
    s_compass.heading_filter_ready = false;
    watch_compass_set_direction_visible(false);
    watch_compass_show_text(text);
}

/**
 * @brief 把航向角格式化为 000 到 359 度.
 *
 * @param heading_deg 航向角浮点值.
 * @param text 文本缓冲区.
 * @param text_len 文本缓冲区长度.
 */
/**
 * @brief 格式化航向角显示文本。
 *
 * 详细说明：
 * - 把浮点角度转换成适合小屏显示的整数度数。
 *
 * @param heading_deg 输入或输出参数，具体含义见函数内部使用方式。
 * @param text 输入或输出参数，具体含义见函数内部使用方式。
 * @param text_len 输入或输出参数，具体含义见函数内部使用方式。
 */
static void watch_compass_format_heading(float heading_deg, char *text, uint16_t text_len)
{
    int32_t heading_int = 0;

    if(text == NULL || text_len == 0U) {
        return;
    }

    heading_int = (int32_t)(heading_deg + 0.5f);
    if(heading_int >= 360) {
        heading_int = 0;
    }

    snprintf(text, text_len, "%03ld°", (long)heading_int);
}

/**
 * @brief 根据当前航向角刷新圆形刻度和 N/E/S/W 字母位置.
 *
 * @note heading_deg 表示手表正上方当前指向的地理方向。
 *       圆盘上的真实方位 bearing 要显示到屏幕上的位置为 bearing - heading。
 *       这样旋转手表时，N/E/S/W 会始终指向真实的东南西北方向。
 */
/**
 * @brief 根据当前航向旋转刻度和方位文字。
 *
 * 详细说明：
 * - 刷新刻度和方向文字的位置或显示状态。
 *
 * @param heading_deg 输入或输出参数，具体含义见函数内部使用方式。
 */
static void watch_compass_update_dial(float heading_deg)
{
    for(int i = 0; i < WATCH_COMPASS_TICK_COUNT; i++) {
        int inner_r = WATCH_COMPASS_TICK_MINOR_IN_R;
        float bearing_deg = (float)i * WATCH_COMPASS_TICK_STEP_DEG;
        float screen_angle_deg = bearing_deg - heading_deg;
        lv_coord_t x1 = 0;
        lv_coord_t y1 = 0;
        lv_coord_t x2 = 0;
        lv_coord_t y2 = 0;

        if(s_compass.tick_line[i] == NULL) {
            continue;
        }

        if((i % 15) == 0) {
            inner_r = WATCH_COMPASS_TICK_CARD_IN_R;
        }
        else if((i % 5) == 0) {
            inner_r = WATCH_COMPASS_TICK_MAJOR_IN_R;
        }

        watch_compass_polar_to_xy(screen_angle_deg, inner_r, &x1, &y1);
        watch_compass_polar_to_xy(screen_angle_deg, WATCH_COMPASS_TICK_OUTER_R, &x2, &y2);

        s_compass.tick_points[i][0].x = x1;
        s_compass.tick_points[i][0].y = y1;
        s_compass.tick_points[i][1].x = x2;
        s_compass.tick_points[i][1].y = y2;
        lv_line_set_points(s_compass.tick_line[i], s_compass.tick_points[i], 2);
    }

    for(int i = 0; i < WATCH_COMPASS_CARDINAL_COUNT; i++) {
        float screen_angle_deg = s_cardinal_deg[i] - heading_deg;
        lv_coord_t x = 0;
        lv_coord_t y = 0;

        if(s_compass.cardinal_label[i] == NULL) {
            continue;
        }

        watch_compass_polar_to_xy(screen_angle_deg, WATCH_COMPASS_CARDINAL_R, &x, &y);
        lv_obj_set_pos(s_compass.cardinal_label[i], x - 14, y - 14);
    }
}

/**
 * @brief 更新中央指北箭头。
 *
 * @note 航向表示屏幕上方指向的方位，因此真实北方在屏幕上的角度为 -heading。
 */
static void watch_compass_update_needle(float heading_deg)
{
    float north_screen_deg = -heading_deg;
    float north_rad = north_screen_deg * WATCH_COMPASS_PI / 180.0f;
    float perpendicular_x = cosf(north_rad);
    float perpendicular_y = sinf(north_rad);
    lv_coord_t tip_x = 0;
    lv_coord_t tip_y = 0;
    lv_coord_t base_x = 0;
    lv_coord_t base_y = 0;
    lv_coord_t south_x = 0;
    lv_coord_t south_y = 0;

    if(s_compass.north_line == NULL ||
       s_compass.north_arrow_left == NULL ||
       s_compass.north_arrow_right == NULL ||
       s_compass.south_line == NULL) {
        return;
    }

    watch_compass_polar_to_xy(north_screen_deg, WATCH_COMPASS_NEEDLE_NORTH_R, &tip_x, &tip_y);
    watch_compass_polar_to_xy(north_screen_deg, WATCH_COMPASS_ARROW_BASE_R, &base_x, &base_y);
    watch_compass_polar_to_xy(north_screen_deg + 180.0f,
                              WATCH_COMPASS_NEEDLE_SOUTH_R,
                              &south_x,
                              &south_y);

    s_compass.north_points[0] = (lv_point_precise_t){WATCH_COMPASS_CENTER_X, WATCH_COMPASS_CENTER_Y};
    s_compass.north_points[1] = (lv_point_precise_t){tip_x, tip_y};
    lv_line_set_points(s_compass.north_line, s_compass.north_points, 2);

    s_compass.north_arrow_left_points[0] = (lv_point_precise_t){tip_x, tip_y};
    s_compass.north_arrow_left_points[1] = (lv_point_precise_t){
        watch_compass_round_coord((float)base_x + perpendicular_x * WATCH_COMPASS_ARROW_HALF_W),
        watch_compass_round_coord((float)base_y + perpendicular_y * WATCH_COMPASS_ARROW_HALF_W),
    };
    lv_line_set_points(s_compass.north_arrow_left,
                       s_compass.north_arrow_left_points,
                       2);

    s_compass.north_arrow_right_points[0] = (lv_point_precise_t){tip_x, tip_y};
    s_compass.north_arrow_right_points[1] = (lv_point_precise_t){
        watch_compass_round_coord((float)base_x - perpendicular_x * WATCH_COMPASS_ARROW_HALF_W),
        watch_compass_round_coord((float)base_y - perpendicular_y * WATCH_COMPASS_ARROW_HALF_W),
    };
    lv_line_set_points(s_compass.north_arrow_right,
                       s_compass.north_arrow_right_points,
                       2);

    s_compass.south_points[0] = (lv_point_precise_t){WATCH_COMPASS_CENTER_X, WATCH_COMPASS_CENTER_Y};
    s_compass.south_points[1] = (lv_point_precise_t){south_x, south_y};
    lv_line_set_points(s_compass.south_line, s_compass.south_points, 2);

    watch_compass_set_direction_visible(true);
}

/**
 * @brief 创建一条页面坐标系中的非交互线条。
 */
static lv_obj_t *watch_compass_create_line(uint32_t color, int width, lv_opa_t opa)
{
    lv_obj_t *line = lv_line_create(s_compass.page);

    lv_obj_set_pos(line, 0, 0);
    lv_obj_set_style_line_color(line, lv_color_hex(color), 0);
    lv_obj_set_style_line_width(line, width, 0);
    lv_obj_set_style_line_rounded(line, true, 0);
    lv_obj_set_style_line_opa(line, opa, 0);
    lv_obj_clear_flag(line, LV_OBJ_FLAG_CLICKABLE);

    return line;
}

/**
 * @brief 创建中央指北箭头和顶部固定朝向基准。
 */
static void watch_compass_create_direction_indicator(void)
{
    s_compass.south_line = watch_compass_create_line(0x8ca0b3, 4, LV_OPA_80);
    s_compass.north_line = watch_compass_create_line(0xff3347, 5, LV_OPA_COVER);
    s_compass.north_arrow_left = watch_compass_create_line(0xff3347, 4, LV_OPA_COVER);
    s_compass.north_arrow_right = watch_compass_create_line(0xff3347, 4, LV_OPA_COVER);

    s_compass.reference_left_points[0] = (lv_point_precise_t){112, 18};
    s_compass.reference_left_points[1] = (lv_point_precise_t){120, 10};
    s_compass.reference_right_points[0] = (lv_point_precise_t){120, 10};
    s_compass.reference_right_points[1] = (lv_point_precise_t){128, 18};
    s_compass.reference_left = watch_compass_create_line(0xffd45a, 3, LV_OPA_COVER);
    s_compass.reference_right = watch_compass_create_line(0xffd45a, 3, LV_OPA_COVER);
    lv_line_set_points(s_compass.reference_left, s_compass.reference_left_points, 2);
    lv_line_set_points(s_compass.reference_right, s_compass.reference_right_points, 2);

    watch_compass_update_needle(0.0f);
}

/**
 * @brief 创建半径为 100 的圆、刻度和 N/E/S/W 字母.
 */
/**
 * @brief 创建指南针圆盘、刻度和方位标签。
 *
 * 详细说明：
 * - 一次性创建外圈、刻度、方向文字和中心点。
 */
static void watch_compass_create_dial(void)
{
    s_compass.outer_ring = lv_obj_create(s_compass.page);
    lv_obj_remove_style_all(s_compass.outer_ring);
    lv_obj_set_size(s_compass.outer_ring,
                    WATCH_COMPASS_OUTER_RING_SIZE,
                    WATCH_COMPASS_OUTER_RING_SIZE);
    lv_obj_set_pos(s_compass.outer_ring,
                   WATCH_COMPASS_OUTER_RING_X,
                   WATCH_COMPASS_OUTER_RING_Y);
    lv_obj_set_style_radius(s_compass.outer_ring, WATCH_COMPASS_OUTER_RING_RADIUS, 0);
    lv_obj_set_style_bg_opa(s_compass.outer_ring, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_compass.outer_ring, 2, 0);
    lv_obj_set_style_border_color(s_compass.outer_ring, lv_color_hex(0x29485f), 0);
    lv_obj_set_style_border_opa(s_compass.outer_ring, LV_OPA_COVER, 0);
    lv_obj_clear_flag(s_compass.outer_ring, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(s_compass.outer_ring, LV_OBJ_FLAG_CLICKABLE);

    s_compass.ring = lv_obj_create(s_compass.page);
    lv_obj_remove_style_all(s_compass.ring);
    lv_obj_set_size(s_compass.ring, WATCH_COMPASS_RING_SIZE, WATCH_COMPASS_RING_SIZE);
    lv_obj_set_pos(s_compass.ring, WATCH_COMPASS_RING_X, WATCH_COMPASS_RING_Y);
    lv_obj_set_style_radius(s_compass.ring, WATCH_COMPASS_RADIUS, 0);
    lv_obj_set_style_bg_opa(s_compass.ring, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_compass.ring, 2, 0);
    lv_obj_set_style_border_color(s_compass.ring, lv_color_hex(0x46c7e8), 0);
    lv_obj_set_style_border_opa(s_compass.ring, LV_OPA_COVER, 0);
    lv_obj_clear_flag(s_compass.ring, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(s_compass.ring, LV_OBJ_FLAG_CLICKABLE);

    for(int i = 0; i < WATCH_COMPASS_TICK_COUNT; i++) {
        bool cardinal_tick = ((i % 15) == 0);
        bool major_tick = ((i % 5) == 0);

        s_compass.tick_line[i] = lv_line_create(s_compass.page);
        lv_obj_set_pos(s_compass.tick_line[i], 0, 0);
        lv_obj_set_style_line_width(s_compass.tick_line[i], cardinal_tick ? 3 : (major_tick ? 2 : 1), 0);
        lv_obj_set_style_line_color(s_compass.tick_line[i],
                                    cardinal_tick ? lv_color_hex(0xf4f8fb) : lv_color_hex(0x57839e),
                                    0);
        lv_obj_set_style_line_opa(s_compass.tick_line[i], cardinal_tick ? LV_OPA_COVER : LV_OPA_60, 0);
        lv_obj_clear_flag(s_compass.tick_line[i], LV_OBJ_FLAG_CLICKABLE);
    }

    for(int i = 0; i < WATCH_COMPASS_CARDINAL_COUNT; i++) {
        s_compass.cardinal_label[i] = lv_label_create(s_compass.page);
        lv_obj_set_size(s_compass.cardinal_label[i], 28, 28);
        lv_obj_set_style_text_font(s_compass.cardinal_label[i], &lv_font_montserrat_26, 0);
        lv_obj_set_style_text_color(s_compass.cardinal_label[i],
                                    i == 0 ? lv_color_hex(0xff4d5a) : lv_color_hex(0xd8e6f3),
                                    0);
        lv_obj_set_style_text_align(s_compass.cardinal_label[i], LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_pad_all(s_compass.cardinal_label[i], 0, 0);
        lv_label_set_text(s_compass.cardinal_label[i], s_cardinal_text_en[i]);
        lv_obj_clear_flag(s_compass.cardinal_label[i], LV_OBJ_FLAG_CLICKABLE);
    }

    watch_compass_apply_language(true);

    watch_compass_create_direction_indicator();

    s_compass.center_dot = lv_obj_create(s_compass.page);
    lv_obj_remove_style_all(s_compass.center_dot);
    lv_obj_set_size(s_compass.center_dot, 10, 10);
    lv_obj_set_pos(s_compass.center_dot, WATCH_COMPASS_CENTER_X - 5, WATCH_COMPASS_CENTER_Y - 5);
    lv_obj_set_style_radius(s_compass.center_dot, 5, 0);
    lv_obj_set_style_bg_color(s_compass.center_dot, lv_color_hex(0xf4f8fb), 0);
    lv_obj_set_style_bg_opa(s_compass.center_dot, LV_OPA_COVER, 0);
    lv_obj_clear_flag(s_compass.center_dot, LV_OBJ_FLAG_CLICKABLE);

    watch_compass_update_dial(0.0f);
}

/**
 * @brief 尝试初始化 QMC5883P.
 */
/**
 * @brief 尝试初始化指南针传感器。
 *
 * 详细说明：
 * - 成功后设置校准参数，失败则页面显示提示文本。
 */
static void watch_compass_try_init_sensor(void)
{
    qmc5883p_result_t result = QMC5883P_OK;

    qmc5883p_calibration_default(&s_compass.calibration);
    s_compass.calibration.declination_deg = WATCH_COMPASS_DECLINATION_DEG;

    qmc5883p_port_i2c_scan();
    result = qmc5883p_init();
    if(result == QMC5883P_OK) {
        s_compass.sensor_ready = true;
        return;
    }

    s_compass.sensor_ready = false;
}

/**
 * @brief 更新指南针航向角.
 */
/**
 * @brief 读取磁力计并刷新航向角。
 *
 * 详细说明：
 * - 数据就绪时读取 QMC5883P 原始值并计算真航向。
 */
static void watch_compass_update_heading(void)
{
    qmc5883p_raw_t raw = {0};
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    float north_component = 0.0f;
    float east_component = 0.0f;
    float heading_deg = 0.0f;
    char text[16] = {0};
    qmc5883p_result_t result = QMC5883P_OK;

    if(s_compass.page == NULL || s_compass.heading_label == NULL) {
        return;
    }

    watch_compass_apply_language(false);

    if(!s_compass.sensor_ready) {
        watch_compass_try_init_sensor();
        if(!s_compass.sensor_ready) {
            watch_compass_show_error("NO SENSOR");
            return;
        }
    }

    result = qmc5883p_read_raw(&raw);
    if(result == QMC5883P_ERR_NOT_READY) {
        return;
    }

    if(result == QMC5883P_ERR_OVERFLOW) {
        watch_compass_show_error("OVERFLOW");
        return;
    }

    if(result != QMC5883P_OK) {
        s_compass.sensor_ready = false;
        watch_compass_show_error("SENSOR ERR");
        return;
    }

    result = qmc5883p_apply_calibration(&raw, &s_compass.calibration, &x, &y, &z);
    if(result != QMC5883P_OK) {
        watch_compass_show_error("DATA ERR");
        return;
    }

    (void)z;

    north_component = WATCH_COMPASS_NORTH_COMPONENT(x, y);
    east_component = WATCH_COMPASS_EAST_COMPONENT(x, y);
    heading_deg = qmc5883p_calc_true_heading_deg(north_component,
                                                 east_component,
                                                 s_compass.calibration.declination_deg);
    heading_deg = watch_compass_normalize_heading(
        heading_deg + WATCH_COMPASS_MOUNT_OFFSET_DEG);
    heading_deg = watch_compass_smooth_heading(heading_deg);

    watch_compass_update_dial(heading_deg);
    watch_compass_update_needle(heading_deg);
    watch_compass_format_heading(heading_deg, text, sizeof(text));
    watch_compass_show_text(text);
}

/**
 * @brief 指南针刷新定时器回调.
 *
 * @param timer LVGL 定时器对象.
 */
/**
 * @brief 指南针定时刷新回调。
 *
 * 详细说明：
 * - 由 LVGL 定时器调用，避免阻塞主循环。
 *
 * @param timer 输入或输出参数，具体含义见函数内部使用方式。
 */
static void watch_compass_timer_cb(lv_timer_t *timer)
{
    (void)timer;

    watch_compass_update_heading();
}

/**
 * @brief 创建航向角显示标签.
 */
/**
 * @brief 创建航向角显示标签。
 *
 * 详细说明：
 * - 用于显示当前角度或传感器错误状态。
 */
static void watch_compass_create_heading_label(void)
{
    s_compass.heading_label = lv_label_create(s_compass.page);
    lv_obj_set_style_text_font(s_compass.heading_label, &lv_font_montserrat_26, 0);
    lv_obj_set_style_text_color(s_compass.heading_label, lv_color_hex(0xf4f8fb), 0);
    lv_obj_set_style_text_align(s_compass.heading_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_bg_color(s_compass.heading_label, lv_color_hex(0x102638), 0);
    lv_obj_set_style_bg_opa(s_compass.heading_label, LV_OPA_90, 0);
    lv_obj_set_style_border_width(s_compass.heading_label, 1, 0);
    lv_obj_set_style_border_color(s_compass.heading_label, lv_color_hex(0x46c7e8), 0);
    lv_obj_set_style_radius(s_compass.heading_label, 12, 0);
    lv_obj_set_style_pad_left(s_compass.heading_label, 10, 0);
    lv_obj_set_style_pad_right(s_compass.heading_label, 10, 0);
    lv_obj_set_style_pad_top(s_compass.heading_label, 3, 0);
    lv_obj_set_style_pad_bottom(s_compass.heading_label, 3, 0);
    lv_label_set_text(s_compass.heading_label, "--");
    lv_obj_align(s_compass.heading_label, LV_ALIGN_CENTER, 0, WATCH_COMPASS_HEADING_Y_OFFSET);
    lv_obj_clear_flag(s_compass.heading_label, LV_OBJ_FLAG_CLICKABLE);
}

/**
 * @brief 创建指南针页面。
 */
lv_obj_t *watch_compass_create(lv_obj_t *parent)
{
    if(s_compass.page != NULL) {
        watch_compass_destroy();
    }

    memset(&s_compass, 0, sizeof(s_compass));

    s_compass.page = lv_obj_create(parent);
    lv_obj_remove_style_all(s_compass.page);
    lv_obj_set_size(s_compass.page, WATCH_SCREEN_W, WATCH_SCREEN_H);
    lv_obj_set_pos(s_compass.page, 0, 0);
    lv_obj_set_style_bg_color(s_compass.page, lv_color_hex(0x07141f), 0);
    lv_obj_set_style_bg_opa(s_compass.page, LV_OPA_COVER, 0);
    lv_obj_clear_flag(s_compass.page, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(s_compass.page, LV_OBJ_FLAG_CLICKABLE);

    watch_compass_create_dial();
    watch_compass_create_heading_label();
    watch_compass_try_init_sensor();
    watch_compass_update_heading();

    s_compass.timer = lv_timer_create(watch_compass_timer_cb, WATCH_COMPASS_TIMER_PERIOD_MS, NULL);
    s_compass.wants_back = false;

    return s_compass.page;
}

/**
 * @brief 重置指南针页面状态。
 *
 * 详细说明：
 * - 初始化 UI、语言和传感器，并启动刷新定时器。
 */
void watch_compass_reset(void)
{
    s_compass.wants_back = false;
    s_compass.heading_filter_ready = false;
    watch_compass_apply_language(true);

    if(s_compass.heading_label != NULL) {
        watch_compass_update_heading();
    }
}

/**
 * @brief 处理指南针页面按键。
 *
 * 详细说明：
 * - 通常用于返回上级菜单。
 *
 * @param key 输入或输出参数，具体含义见函数内部使用方式。
 */
void watch_compass_on_key(watch_key_t key)
{
    if(s_compass.page == NULL) {
        return;
    }

    if(key == WATCH_KEY_2) {
        s_compass.wants_back = true;
    }
}

/**
 * @brief 查询是否请求返回上级菜单。
 *
 * 详细说明：
 * - 供上层页面状态机轮询。
 */
bool watch_compass_wants_back(void)
{
    return s_compass.wants_back;
}

/**
 * @brief 销毁指南针页面资源。
 *
 * 详细说明：
 * - 删除 LVGL 对象和定时器，避免离开页面后继续刷新。
 */
void watch_compass_destroy(void)
{
    if(s_compass.timer != NULL) {
        lv_timer_del(s_compass.timer);
        s_compass.timer = NULL;
    }

    if(s_compass.sensor_ready) {
        qmc5883p_suspend();
    }

    if(s_compass.page != NULL) {
        lv_obj_del(s_compass.page);
        s_compass.page = NULL;
    }

    memset(&s_compass, 0, sizeof(s_compass));
}
