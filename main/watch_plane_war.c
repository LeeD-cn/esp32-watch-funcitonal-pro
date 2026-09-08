/**
 * @file watch_plane_war.c
 * @brief 飞机大战游戏主逻辑，包括移动、射击、敌机、爆炸、生命和分数。
 */
/*
 * ==================== 模块说明 ====================
 *  * 模块职责：
 * - 飞机大战核心玩法实现，包括玩家移动、自动射击、敌机生成、碰撞检测、爆炸动画、生命和分数。
 * - 游戏对象采用固定大小对象池，避免运行时频繁创建/销毁 LVGL 对象造成碎片。
 * - 主循环由 LVGL timer 驱动，按固定帧间隔更新玩家、子弹、敌机和特效。
 * - 子弹、敌机、爆炸等对象只切换 active/hidden 状态，提高嵌入式设备上的运行稳定性。
 *
 * 阅读建议：
 * - 先看本文件顶部的宏定义和静态状态变量，理解硬件参数和运行状态。
 * - 再看 reset/init/start/on_key/destroy 等对外函数，理解页面或驱动的生命周期。
 * - 最后看 static 辅助函数，了解具体寄存器读写、UI 刷新或状态机细节。
 * =======================================================
 */

#include "watch_plane_war.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "driver/gpio.h"
#include "esp_random.h"

#define WATCH_SCREEN_W              240
#define WATCH_SCREEN_H              240

#define PLANE_WAR_SCORE_STEP        10
#define PLANE_WAR_SCORE_MARGIN_X    8
#define PLANE_WAR_SCORE_MARGIN_Y    4


/**
 * @brief 生命值显示参数。
 */
#define PLANE_WAR_LIFE_MAX          3
#define PLANE_WAR_HEART_W           30
#define PLANE_WAR_HEART_H           30
#define PLANE_WAR_HEART_MARGIN_X    4
#define PLANE_WAR_HEART_MARGIN_Y    4
#define PLANE_WAR_HEART_GAP         -4

#define PLANE_WAR_HEART_START_X     (WATCH_SCREEN_W - PLANE_WAR_SCORE_MARGIN_X - PLANE_WAR_HEART_W)
#define PLANE_WAR_HEART_START_Y     (PLANE_WAR_SCORE_MARGIN_Y + PLANE_WAR_SCORE_LABEL_H + PLANE_WAR_HEART_GAP)


#define PLANE_WAR_SCORE_SHOW_MS     800
#define PLANE_WAR_SCORE_ALWAYS_SHOW 1
#define PLANE_WAR_SCORE_LABEL_W     80
#define PLANE_WAR_SCORE_LABEL_H     24


/**
 * @brief 游戏内直接读取左右移动按键的 GPIO。
 */
#define PLANE_WAR_KEY1_GPIO         35
#define PLANE_WAR_KEY3_GPIO         33

#define PLANE_WAR_JET_W             40
#define PLANE_WAR_JET_H             40
#define PLANE_WAR_JET_START_X       ((WATCH_SCREEN_W - PLANE_WAR_JET_W) / 2)
#define PLANE_WAR_JET_START_Y       (WATCH_SCREEN_H - PLANE_WAR_JET_H)


#define PLANE_WAR_MOVE_STEP         6
#define PLANE_WAR_TIMER_MS          40


/**
 * @brief 子弹池参数。
 */
#define PLANE_WAR_BULLET_COUNT      10
#define PLANE_WAR_BULLET_W          3
#define PLANE_WAR_BULLET_H          8
#define PLANE_WAR_BULLET_SPEED      6
#define PLANE_WAR_SHOOT_INTERVAL_MS 200


/**
 * @brief 敌机池和敌机生成参数。
 */
#define PLANE_WAR_HOSTILE_COUNT     3
#define PLANE_WAR_HOSTILE_W         30
#define PLANE_WAR_HOSTILE_H         30
#define PLANE_WAR_HOSTILE_SPEED     3
#define PLANE_WAR_HOSTILE_X_SPEED_MAX 3
#define PLANE_WAR_HOSTILE_HP        2
#define PLANE_WAR_HOSTILE_MIN_MS    500
#define PLANE_WAR_HOSTILE_MAX_MS    2000


/**
 * @brief 预创建爆炸特效对象，运行时只更新位置、尺寸和透明度。
 */
#define PLANE_WAR_EXPLOSION_MS              560
#define PLANE_WAR_EXPLOSION_PARTICLE_COUNT 8
#define PLANE_WAR_EXPLOSION_PARTICLE_SIZE  5
#define PLANE_WAR_EXPLOSION_CORE_MIN_SIZE  10
#define PLANE_WAR_EXPLOSION_CORE_MAX_SIZE  36
#define PLANE_WAR_EXPLOSION_RING_MIN_SIZE  16
#define PLANE_WAR_EXPLOSION_RING_MAX_SIZE  58
#define PLANE_WAR_EXPLOSION_PARTICLE_DIST  34


#define PLANE_WAR_BULLET_OFFSET_X   0
#define PLANE_WAR_BULLET_OFFSET_Y   5


LV_IMG_DECLARE(our_jet);
LV_IMG_DECLARE(hostile_jet);
LV_IMG_DECLARE(heart);
LV_IMG_DECLARE(plane_war_bg);

/**
 * @brief 子弹对象状态。
 */
typedef struct {
    lv_obj_t *obj;
    int x;
    int y;
    bool active;
} plane_bullet_t;

/**
 * @brief 敌机对象状态及爆炸特效对象。
 */
typedef struct {
    lv_obj_t *obj;
    lv_obj_t *explosion_core;
    lv_obj_t *explosion_ring;
    lv_obj_t *explosion_particle[PLANE_WAR_EXPLOSION_PARTICLE_COUNT];

    int x;
    int y;
    int vx;
    int hp;

    int explosion_cx;
    int explosion_cy;

    bool active;
    bool exploding;
    uint32_t explosion_start_tick;
} plane_hostile_t;

/* 飞机大战结束时通知上层的回调函数。 */
static watch_plane_war_game_over_cb_t s_plane_war_game_over_cb = NULL;

/**
 * @brief 飞机大战运行上下文。
 */
typedef struct {
    lv_obj_t *page;
    lv_obj_t *bg_img;
    lv_obj_t *jet_img;
    lv_obj_t *score_label;
    lv_obj_t *life_img[PLANE_WAR_LIFE_MAX];

    lv_timer_t *timer;

    plane_bullet_t bullet[PLANE_WAR_BULLET_COUNT];
    plane_hostile_t hostile[PLANE_WAR_HOSTILE_COUNT];

    int jet_x;
    int jet_y;
    int score;
    int life;

    uint32_t score_hide_tick;
    uint32_t last_shot_tick;
    uint32_t next_hostile_tick;

    bool active;
    bool wants_back;
    bool game_over;
} plane_war_ctx_t;

/* 飞机大战完整运行上下文和对象池。 */
static plane_war_ctx_t s_plane_war;

static void plane_war_bullets_reset(void);
static void plane_war_hostiles_reset(void);

/**
 * @brief 根据当前生命值刷新心形图标。
 *
 * 详细说明：
 * - 生命值内的心形显示，超出的心形隐藏。
 */
static void plane_war_life_update(void)
{
    for(int i = 0; i < PLANE_WAR_LIFE_MAX; i++) {
        lv_obj_t *heart_img = s_plane_war.life_img[i];

        if(heart_img == NULL) {
            continue;
        }

        if(i < s_plane_war.life) {
            lv_obj_clear_flag(heart_img, LV_OBJ_FLAG_HIDDEN);
        }
        else {
            lv_obj_add_flag(heart_img, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

/**
 * @brief 停止游戏并触发上层结算回调。
 *
 * 详细说明：
 * - 防止重复进入 Game Over。
 * - 停止对象池和定时器，再通知上层状态机。
 */
static void plane_war_trigger_game_over(void)
{
    if(s_plane_war.game_over) {
        return;
    }

    s_plane_war.game_over = true;
    s_plane_war.active = false;

    plane_war_bullets_reset();
    plane_war_hostiles_reset();

    if(s_plane_war.timer) {
        lv_timer_pause(s_plane_war.timer);
    }

    if(s_plane_war_game_over_cb) {
        s_plane_war_game_over_cb();
    }
}

/**
 * @brief 扣除一条生命。
 *
 * 详细说明：
 * - 生命归零后触发 Game Over。
 */
static void plane_war_life_lose_one(void)
{
    if(s_plane_war.life <= 0) {
        return;
    }

    s_plane_war.life--;
    plane_war_life_update();

    if(s_plane_war.life <= 0) {
        plane_war_trigger_game_over();
    }
}

/**
 * @brief 重置玩家生命值。
 *
 * 详细说明：
 * - 开始新游戏时恢复最大生命。
 */
static void plane_war_life_reset(void)
{
    s_plane_war.life = PLANE_WAR_LIFE_MAX;
    plane_war_life_update();
}

/**
 * @brief 临时显示分数文本。
 *
 * 详细说明：
 * - 得分后短时间显示，便于玩家获得反馈。
 *
 * @param now 输入或输出参数，具体含义见调用处和函数内部使用方式。
 */
static void plane_war_score_show_temporarily(uint32_t now)
{
    if(s_plane_war.score_label == NULL) {
        return;
    }

#if PLANE_WAR_SCORE_ALWAYS_SHOW
    (void)now;
    lv_obj_clear_flag(s_plane_war.score_label, LV_OBJ_FLAG_HIDDEN);
#else
    s_plane_war.score_hide_tick = now + PLANE_WAR_SCORE_SHOW_MS;
    lv_obj_clear_flag(s_plane_war.score_label, LV_OBJ_FLAG_HIDDEN);
#endif
}

/**
 * @brief 刷新分数文本，避免在游戏循环中重复布局。
 *
 * 详细说明：
 * - 把整数分数格式化到 LVGL label。
 */
static void plane_war_score_update_text(void)
{
    if(s_plane_war.score_label == NULL) {
        return;
    }


    lv_label_set_text_fmt(s_plane_war.score_label, "%d", s_plane_war.score);
}

/**
 * @brief 根据时间控制分数显示/隐藏。
 *
 * 详细说明：
 * - 可配置为常显或短暂显示。
 *
 * @param now 输入或输出参数，具体含义见调用处和函数内部使用方式。
 */
static void plane_war_score_update_visibility(uint32_t now)
{
#if PLANE_WAR_SCORE_ALWAYS_SHOW
    (void)now;
#else
    if(s_plane_war.score_label == NULL) {
        return;
    }

    if(s_plane_war.score_hide_tick == 0) {
        return;
    }

    if((int32_t)(now - s_plane_war.score_hide_tick) >= 0) {
        s_plane_war.score_hide_tick = 0;
        lv_obj_add_flag(s_plane_war.score_label, LV_OBJ_FLAG_HIDDEN);
    }
#endif
}

/**
 * @brief 重置游戏分数。
 *
 * 详细说明：
 * - 开始新局时清零并刷新 UI。
 */
static void plane_war_score_reset(void)
{
    s_plane_war.score = 0;
    s_plane_war.score_hide_tick = 0;
    plane_war_score_update_text();

#if PLANE_WAR_SCORE_ALWAYS_SHOW
    if(s_plane_war.score_label) {
        lv_obj_clear_flag(s_plane_war.score_label, LV_OBJ_FLAG_HIDDEN);
    }
#else
    if(s_plane_war.score_label) {
        lv_obj_add_flag(s_plane_war.score_label, LV_OBJ_FLAG_HIDDEN);
    }
#endif
}

/**
 * @brief 增加分数。
 *
 * 详细说明：
 * - 击中或击毁敌机时调用。
 *
 * @param add_score 输入或输出参数，具体含义见调用处和函数内部使用方式。
 * @param now 输入或输出参数，具体含义见调用处和函数内部使用方式。
 */
static void plane_war_score_add(int add_score, uint32_t now)
{
    s_plane_war.score += add_score;
    plane_war_score_update_text();
    plane_war_score_show_temporarily(now);
}

/**
 * @brief 直接读取游戏移动键是否按下。
 *
 * 详细说明：
 * - 游戏内为了更流畅移动，直接轮询 GPIO。
 *
 * @param gpio 输入或输出参数，具体含义见调用处和函数内部使用方式。
 *
 * @return 函数执行结果或计算得到的值，具体语义见返回路径。
 */
static bool plane_war_key_is_down(gpio_num_t gpio)
{


    return gpio_get_level(gpio) == 0;
}

/**
 * @brief 限制玩家飞机 X 坐标在屏幕内。
 *
 * 详细说明：
 * - 防止飞机移动出屏幕边界。
 *
 * @param x 输入或输出参数，具体含义见调用处和函数内部使用方式。
 *
 * @return 函数执行结果或计算得到的值，具体语义见返回路径。
 */
static int plane_war_clamp_jet_x(int x)
{
    if(x < 0) {
        return 0;
    }

    if(x > WATCH_SCREEN_W - PLANE_WAR_JET_W) {
        return WATCH_SCREEN_W - PLANE_WAR_JET_W;
    }

    return x;
}

/**
 * @brief 设置玩家飞机位置。
 *
 * 详细说明：
 * - 更新上下文坐标并移动 LVGL 图片对象。
 *
 * @param x 输入或输出参数，具体含义见调用处和函数内部使用方式。
 * @param y 输入或输出参数，具体含义见调用处和函数内部使用方式。
 */
static void plane_war_set_jet_pos(int x, int y)
{
    s_plane_war.jet_x = plane_war_clamp_jet_x(x);
    s_plane_war.jet_y = y;

    if(s_plane_war.jet_img) {
        lv_obj_set_pos(s_plane_war.jet_img,
                       s_plane_war.jet_x,
                       s_plane_war.jet_y);
    }
}

/**
 * @brief 重置所有子弹对象。
 *
 * 详细说明：
 * - 隐藏并标记为空闲，便于重新开始游戏。
 */
static void plane_war_bullets_reset(void)
{
    for(int i = 0; i < PLANE_WAR_BULLET_COUNT; i++) {
        s_plane_war.bullet[i].active = false;
        s_plane_war.bullet[i].x = 0;
        s_plane_war.bullet[i].y = 0;

        if(s_plane_war.bullet[i].obj) {
            lv_obj_add_flag(s_plane_war.bullet[i].obj, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

/**
 * @brief 生成指定范围内的随机整数。
 *
 * 详细说明：
 * - 用于敌机出生位置、速度和刷新间隔。
 *
 * @param min 输入或输出参数，具体含义见调用处和函数内部使用方式。
 * @param max 输入或输出参数，具体含义见调用处和函数内部使用方式。
 *
 * @return 函数执行结果或计算得到的值，具体语义见返回路径。
 */
static uint32_t plane_war_random_range(uint32_t min, uint32_t max)
{
    if(max <= min) {
        return min;
    }

    return min + (esp_random() % (max - min + 1));
}

/**
 * @brief 安排下一架敌机生成时间。
 *
 * 详细说明：
 * - 使用随机间隔提高游戏变化。
 *
 * @param now 输入或输出参数，具体含义见调用处和函数内部使用方式。
 */
static void plane_war_schedule_next_hostile(uint32_t now)
{
    s_plane_war.next_hostile_tick =
        now + plane_war_random_range(PLANE_WAR_HOSTILE_MIN_MS,
                                     PLANE_WAR_HOSTILE_MAX_MS);
}

static const int s_explosion_dir_x[PLANE_WAR_EXPLOSION_PARTICLE_COUNT] = {
    0, 7, 10, 7, 0, -7, -10, -7
};

static const int s_explosion_dir_y[PLANE_WAR_EXPLOSION_PARTICLE_COUNT] = {
    -10, -7, 0, 7, 10, 7, 0, -7
};

/**
 * @brief 计算爆炸动画透明度。
 *
 * 详细说明：
 * - 根据经过时间让特效逐渐淡出。
 *
 * @param elapsed 输入或输出参数，具体含义见调用处和函数内部使用方式。
 * @param total 输入或输出参数，具体含义见调用处和函数内部使用方式。
 * @param max_opa 输入或输出参数，具体含义见调用处和函数内部使用方式。
 *
 * @return 函数执行结果或计算得到的值，具体语义见返回路径。
 */
static lv_opa_t plane_war_explosion_fade_opa(uint32_t elapsed,
                                             uint32_t total,
                                             lv_opa_t max_opa)
{
    if(elapsed >= total) {
        return LV_OPA_TRANSP;
    }

    return (lv_opa_t)((uint32_t)max_opa * (total - elapsed) / total);
}

/**
 * @brief 隐藏敌机爆炸特效对象。
 *
 * 详细说明：
 * - 爆炸结束后释放显示资源但保留对象复用。
 *
 * @param h 输入或输出参数，具体含义见调用处和函数内部使用方式。
 */
static void plane_war_explosion_visuals_hide(plane_hostile_t *h)
{
    if(h->explosion_core) {
        lv_obj_add_flag(h->explosion_core, LV_OBJ_FLAG_HIDDEN);
    }

    if(h->explosion_ring) {
        lv_obj_add_flag(h->explosion_ring, LV_OBJ_FLAG_HIDDEN);
    }

    for(int i = 0; i < PLANE_WAR_EXPLOSION_PARTICLE_COUNT; i++) {
        if(h->explosion_particle[i]) {
            lv_obj_add_flag(h->explosion_particle[i], LV_OBJ_FLAG_HIDDEN);
        }
    }
}

/**
 * @brief 隐藏单个敌机及其特效。
 *
 * 详细说明：
 * - 把敌机对象恢复到空闲状态。
 *
 * @param h 输入或输出参数，具体含义见调用处和函数内部使用方式。
 */
static void plane_war_hostile_hide(plane_hostile_t *h)
{
    h->active = false;
    h->exploding = false;
    h->x = 0;
    h->y = 0;
    h->vx = 0;
    h->hp = 0;
    h->explosion_cx = 0;
    h->explosion_cy = 0;
    h->explosion_start_tick = 0;

    if(h->obj) {
        lv_obj_add_flag(h->obj, LV_OBJ_FLAG_HIDDEN);
    }

    plane_war_explosion_visuals_hide(h);
}

/**
 * @brief 隐藏敌机本体并启动爆炸动画。
 *
 * 详细说明：
 * - 记录爆炸中心和开始时间，并隐藏敌机本体。
 *
 * @param h 输入或输出参数，具体含义见函数内部使用方式。
 * @param now 输入或输出参数，具体含义见函数内部使用方式。
 */
static void plane_war_hostile_start_explosion(plane_hostile_t *h,
                                              uint32_t now)
{
    h->active = false;
    h->exploding = true;
    h->vx = 0;
    h->hp = 0;
    h->explosion_start_tick = now;
    h->explosion_cx = h->x + PLANE_WAR_HOSTILE_W / 2;
    h->explosion_cy = h->y + PLANE_WAR_HOSTILE_H / 2;


    if(h->obj) {
        lv_obj_add_flag(h->obj, LV_OBJ_FLAG_HIDDEN);
    }

    if(h->explosion_core) {
        lv_obj_clear_flag(h->explosion_core, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(h->explosion_core);
    }

    if(h->explosion_ring) {
        lv_obj_clear_flag(h->explosion_ring, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(h->explosion_ring);
    }

    for(int i = 0; i < PLANE_WAR_EXPLOSION_PARTICLE_COUNT; i++) {
        if(h->explosion_particle[i]) {
            lv_obj_clear_flag(h->explosion_particle[i], LV_OBJ_FLAG_HIDDEN);
            lv_obj_move_foreground(h->explosion_particle[i]);
        }
    }
}

/**
 * @brief 更新敌机爆炸动画帧。
 *
 * 详细说明：
 * - 根据时间调整核心、圆环和粒子位置/透明度。
 *
 * @param h 输入或输出参数，具体含义见调用处和函数内部使用方式。
 * @param now 输入或输出参数，具体含义见调用处和函数内部使用方式。
 */
static void plane_war_hostile_explosion_update(plane_hostile_t *h,
                                               uint32_t now)
{
    uint32_t elapsed = now - h->explosion_start_tick;

    if(elapsed >= PLANE_WAR_EXPLOSION_MS) {
        plane_war_hostile_hide(h);
        return;
    }

    uint32_t progress = elapsed * 1000U / PLANE_WAR_EXPLOSION_MS;
    lv_opa_t fade_opa =
        plane_war_explosion_fade_opa(elapsed,
                                     PLANE_WAR_EXPLOSION_MS,
                                     LV_OPA_COVER);


    if(h->explosion_core) {
        int core_size;

        if(progress < 260U) {
            core_size = PLANE_WAR_EXPLOSION_CORE_MIN_SIZE +
                        (PLANE_WAR_EXPLOSION_CORE_MAX_SIZE -
                         PLANE_WAR_EXPLOSION_CORE_MIN_SIZE) *
                        (int)progress / 260;
        }
        else {
            core_size = PLANE_WAR_EXPLOSION_CORE_MAX_SIZE -
                        (PLANE_WAR_EXPLOSION_CORE_MAX_SIZE -
                         PLANE_WAR_EXPLOSION_CORE_MIN_SIZE) *
                        (int)(progress - 260U) / 740;
        }

        if(core_size < PLANE_WAR_EXPLOSION_CORE_MIN_SIZE) {
            core_size = PLANE_WAR_EXPLOSION_CORE_MIN_SIZE;
        }

        lv_color_t core_color;
        if(progress < 280U) {
            core_color = lv_color_hex(0xFFF06A);
        }
        else if(progress < 620U) {
            core_color = lv_color_hex(0xFF8A00);
        }
        else {
            core_color = lv_color_hex(0xE43110);
        }

        lv_obj_set_size(h->explosion_core, core_size, core_size);
        lv_obj_set_pos(h->explosion_core,
                       h->explosion_cx - core_size / 2,
                       h->explosion_cy - core_size / 2);
        lv_obj_set_style_radius(h->explosion_core, core_size / 2, 0);
        lv_obj_set_style_bg_color(h->explosion_core, core_color, 0);
        lv_obj_set_style_bg_opa(h->explosion_core, fade_opa, 0);
        lv_obj_clear_flag(h->explosion_core, LV_OBJ_FLAG_HIDDEN);
    }


    if(h->explosion_ring) {
        int ring_size = PLANE_WAR_EXPLOSION_RING_MIN_SIZE +
                        (PLANE_WAR_EXPLOSION_RING_MAX_SIZE -
                         PLANE_WAR_EXPLOSION_RING_MIN_SIZE) *
                        (int)progress / 1000;
        lv_opa_t ring_opa =
            plane_war_explosion_fade_opa(elapsed,
                                         PLANE_WAR_EXPLOSION_MS,
                                         LV_OPA_70);

        lv_obj_set_size(h->explosion_ring, ring_size, ring_size);
        lv_obj_set_pos(h->explosion_ring,
                       h->explosion_cx - ring_size / 2,
                       h->explosion_cy - ring_size / 2);
        lv_obj_set_style_radius(h->explosion_ring, ring_size / 2, 0);
        lv_obj_set_style_border_opa(h->explosion_ring, ring_opa, 0);
        lv_obj_clear_flag(h->explosion_ring, LV_OBJ_FLAG_HIDDEN);
    }


    for(int i = 0; i < PLANE_WAR_EXPLOSION_PARTICLE_COUNT; i++) {
        lv_obj_t *particle = h->explosion_particle[i];

        if(particle == NULL) {
            continue;
        }

        int particle_size = PLANE_WAR_EXPLOSION_PARTICLE_SIZE -
                            (int)progress / 360;
        if(particle_size < 2) {
            particle_size = 2;
        }

        int dist = PLANE_WAR_EXPLOSION_PARTICLE_DIST * (int)progress / 1000;
        int gravity = ((int)progress * (int)progress) / 52000;

        int px = h->explosion_cx +
                 s_explosion_dir_x[i] * dist / 10 -
                 particle_size / 2;
        int py = h->explosion_cy +
                 s_explosion_dir_y[i] * dist / 10 +
                 gravity -
                 particle_size / 2;

        lv_color_t particle_color;
        if((i % 3) == 0) {
            particle_color = lv_color_hex(0xFFF06A);
        }
        else if((i % 3) == 1) {
            particle_color = lv_color_hex(0xFF8A00);
        }
        else {
            particle_color = lv_color_hex(0xFF3B1F);
        }

        lv_obj_set_size(particle, particle_size, particle_size);
        lv_obj_set_pos(particle, px, py);
        lv_obj_set_style_radius(particle, particle_size / 2, 0);
        lv_obj_set_style_bg_color(particle, particle_color, 0);
        lv_obj_set_style_bg_opa(particle, fade_opa, 0);
        lv_obj_clear_flag(particle, LV_OBJ_FLAG_HIDDEN);
    }
}

/**
 * @brief 重置所有敌机对象。
 *
 * 详细说明：
 * - 隐藏敌机和爆炸特效，清空活动状态。
 */
static void plane_war_hostiles_reset(void)
{
    for(int i = 0; i < PLANE_WAR_HOSTILE_COUNT; i++) {
        plane_war_hostile_hide(&s_plane_war.hostile[i]);
    }
}

/**
 * @brief 检测两个矩形是否碰撞。
 *
 * 详细说明：
 * - 用于子弹与敌机、敌机与玩家之间的碰撞判断。
 *
 * @param ax 输入或输出参数，具体含义见调用处和函数内部使用方式。
 * @param ay 输入或输出参数，具体含义见调用处和函数内部使用方式。
 * @param aw 输入或输出参数，具体含义见调用处和函数内部使用方式。
 * @param ah 输入或输出参数，具体含义见调用处和函数内部使用方式。
 * @param bx 输入或输出参数，具体含义见调用处和函数内部使用方式。
 * @param by 输入或输出参数，具体含义见调用处和函数内部使用方式。
 * @param bw 输入或输出参数，具体含义见调用处和函数内部使用方式。
 * @param bh 输入或输出参数，具体含义见调用处和函数内部使用方式。
 *
 * @return 函数执行结果或计算得到的值，具体语义见返回路径。
 */
static bool plane_war_rect_hit(int ax, int ay, int aw, int ah,
                               int bx, int by, int bw, int bh)
{
    return ax < bx + bw &&
           ax + aw > bx &&
           ay < by + bh &&
           ay + ah > by;
}

/**
 * @brief 隐藏单颗子弹。
 *
 * 详细说明：
 * - 子弹飞出屏幕或命中目标后调用。
 *
 * @param b 输入或输出参数，具体含义见调用处和函数内部使用方式。
 */
static void plane_war_bullet_hide(plane_bullet_t *b)
{
    b->active = false;
    b->x = 0;
    b->y = 0;

    if(b->obj) {
        lv_obj_add_flag(b->obj, LV_OBJ_FLAG_HIDDEN);
    }
}

/**
 * @brief 生成一架敌机。
 *
 * 详细说明：
 * - 从对象池找空闲项并设置初始位置、速度和生命。
 */
static void plane_war_spawn_hostile(void)
{
    for(int i = 0; i < PLANE_WAR_HOSTILE_COUNT; i++) {
        plane_hostile_t *h = &s_plane_war.hostile[i];

        if(h->active || h->exploding) {
            continue;
        }

        h->active = true;
        h->exploding = false;
        h->explosion_start_tick = 0;
        h->hp = PLANE_WAR_HOSTILE_HP;
        h->x = (int)plane_war_random_range(0,
                                           WATCH_SCREEN_W - PLANE_WAR_HOSTILE_W);


        h->vx = (int)plane_war_random_range(1, PLANE_WAR_HOSTILE_X_SPEED_MAX);
        if(plane_war_random_range(0, 1) == 0) {
            h->vx = -h->vx;
        }


        h->y = -PLANE_WAR_HOSTILE_H;

        if(h->obj) {
            lv_obj_set_pos(h->obj, h->x, h->y);
            lv_obj_clear_flag(h->obj, LV_OBJ_FLAG_HIDDEN);
        }

        return;
    }
}

/**
 * @brief 更新所有敌机状态。
 *
 * 详细说明：
 * - 移动敌机、检测越界、更新爆炸动画和玩家碰撞。
 *
 * @param now 输入或输出参数，具体含义见调用处和函数内部使用方式。
 */
static void plane_war_hostiles_update(uint32_t now)
{
    if((int32_t)(now - s_plane_war.next_hostile_tick) >= 0) {
        plane_war_spawn_hostile();
        plane_war_schedule_next_hostile(now);
    }

    for(int i = 0; i < PLANE_WAR_HOSTILE_COUNT; i++) {
        plane_hostile_t *h = &s_plane_war.hostile[i];

        if(h->exploding) {
            plane_war_hostile_explosion_update(h, now);
            continue;
        }

        if(!h->active) {
            continue;
        }

        h->y += PLANE_WAR_HOSTILE_SPEED;
        h->x += h->vx;


        if(h->x < 0) {
            h->x = 0;
            h->vx = -h->vx;
        }
        else if(h->x > WATCH_SCREEN_W - PLANE_WAR_HOSTILE_W) {
            h->x = WATCH_SCREEN_W - PLANE_WAR_HOSTILE_W;
            h->vx = -h->vx;
        }

        if(h->y > WATCH_SCREEN_H) {


            plane_war_hostile_hide(h);
            plane_war_life_lose_one();

            if(s_plane_war.game_over) {
                return;
            }

            continue;
        }

        if(h->obj) {
            lv_obj_set_pos(h->obj, h->x, h->y);
        }
    }
}

/**
 * @brief 检测子弹是否击中敌机。
 *
 * 详细说明：
 * - 命中后扣除敌机生命并隐藏子弹。
 *
 * @param now 输入或输出参数，具体含义见调用处和函数内部使用方式。
 */
static void plane_war_check_bullet_hostile_hit(uint32_t now)
{
    for(int i = 0; i < PLANE_WAR_BULLET_COUNT; i++) {
        plane_bullet_t *b = &s_plane_war.bullet[i];

        if(!b->active) {
            continue;
        }

        for(int j = 0; j < PLANE_WAR_HOSTILE_COUNT; j++) {
            plane_hostile_t *h = &s_plane_war.hostile[j];

            if(!h->active) {
                continue;
            }

            if(!plane_war_rect_hit(b->x, b->y,
                                   PLANE_WAR_BULLET_W,
                                   PLANE_WAR_BULLET_H,
                                   h->x, h->y,
                                   PLANE_WAR_HOSTILE_W,
                                   PLANE_WAR_HOSTILE_H)) {
                continue;
            }


            plane_war_bullet_hide(b);

            if(h->hp > 0) {
                h->hp--;
            }

            if(h->hp <= 0) {
                plane_war_score_add(PLANE_WAR_SCORE_STEP, now);
                plane_war_hostile_start_explosion(h, now);
            }

            break;
        }
    }
}

/**
 * @brief 生成玩家子弹。
 *
 * 详细说明：
 * - 受射击间隔限制，防止子弹过密。
 */
static void plane_war_spawn_bullet(void)
{
    for(int i = 0; i < PLANE_WAR_BULLET_COUNT; i++) {
        plane_bullet_t *b = &s_plane_war.bullet[i];

        if(b->active) {
            continue;
        }

        b->active = true;


        b->x = s_plane_war.jet_x +
               PLANE_WAR_JET_W / 2 -
               PLANE_WAR_BULLET_W / 2 +
               PLANE_WAR_BULLET_OFFSET_X;

        b->y = s_plane_war.jet_y -
               PLANE_WAR_BULLET_H +
               PLANE_WAR_BULLET_OFFSET_Y;

        if(b->obj) {
            lv_obj_set_pos(b->obj, b->x, b->y);
            lv_obj_clear_flag(b->obj, LV_OBJ_FLAG_HIDDEN);
        }

        return;
    }
}

/**
 * @brief 更新所有子弹。
 *
 * 详细说明：
 * - 向上移动子弹、处理越界和命中检测。
 */
static void plane_war_bullets_update(void)
{
    for(int i = 0; i < PLANE_WAR_BULLET_COUNT; i++) {
        plane_bullet_t *b = &s_plane_war.bullet[i];

        if(!b->active) {
            continue;
        }

        b->y -= PLANE_WAR_BULLET_SPEED;

        if(b->y + PLANE_WAR_BULLET_H < 0) {
            b->active = false;

            if(b->obj) {
                lv_obj_add_flag(b->obj, LV_OBJ_FLAG_HIDDEN);
            }

            continue;
        }

        if(b->obj) {
            lv_obj_set_pos(b->obj, b->x, b->y);
        }
    }
}

/**
 * @brief 游戏主循环定时器，更新移动、射击、敌机和碰撞。
 *
 * 详细说明：
 * - 每帧处理移动、射击、敌机刷新、碰撞和 UI 状态。
 *
 * @param timer 输入或输出参数，具体含义见函数内部使用方式。
 */
static void plane_war_timer_cb(lv_timer_t *timer)
{
    (void)timer;

    if(s_plane_war.page == NULL || !s_plane_war.active) {
        return;
    }

    uint32_t now = lv_tick_get();


    int dir = 0;

    if(plane_war_key_is_down(PLANE_WAR_KEY1_GPIO)) {
        dir = -1;
    }
    else if(plane_war_key_is_down(PLANE_WAR_KEY3_GPIO)) {
        dir = 1;
    }

    if(dir != 0) {
        int next_x = s_plane_war.jet_x + dir * PLANE_WAR_MOVE_STEP;
        plane_war_set_jet_pos(next_x, s_plane_war.jet_y);
    }


    if(now - s_plane_war.last_shot_tick >= PLANE_WAR_SHOOT_INTERVAL_MS) {
        plane_war_spawn_bullet();
        s_plane_war.last_shot_tick = now;
    }

    plane_war_bullets_update();
    plane_war_hostiles_update(now);
    plane_war_check_bullet_hostile_hit(now);
    plane_war_score_update_visibility(now);
}


/**
 * @brief 注册游戏结束回调。
 *
 * 详细说明：
 * - 由游戏中心传入，用于收到 Game Over 通知。
 *
 * @param cb 输入或输出参数，具体含义见调用处和函数内部使用方式。
 */
void watch_plane_war_set_game_over_cb(watch_plane_war_game_over_cb_t cb)
{
    s_plane_war_game_over_cb = cb;
}

/**
 * @brief 查询游戏是否结束。
 *
 * 详细说明：
 * - 上层可据此决定是否切换页面。
 *
 * @return 函数执行结果或计算得到的值，具体语义见返回路径。
 */
bool watch_plane_war_is_game_over(void)
{
    return s_plane_war.game_over;
}

/**
 * @brief 获取当前分数。
 *
 * 详细说明：
 * - 结算页和历史分数页使用。
 *
 * @return 函数执行结果或计算得到的值，具体语义见返回路径。
 */
int watch_plane_war_get_score(void)
{
    return s_plane_war.score;
}

/**
 * @brief 销毁飞机大战页面。
 *
 * 详细说明：
 * - 删除 timer 和 LVGL 对象，清空游戏上下文。
 */
void watch_plane_war_destroy(void)
{
    if(s_plane_war.timer) {
        lv_timer_delete(s_plane_war.timer);
        s_plane_war.timer = NULL;
    }

    if(s_plane_war.page) {
        lv_obj_del(s_plane_war.page);
        s_plane_war.page = NULL;
    }

    memset(&s_plane_war, 0, sizeof(s_plane_war));
}


/**
 * @brief watch_plane_war_create 对外接口。
 *
 * 详细说明：
 * - 供其他模块调用，隐藏本文件内部状态细节。
 *
 * @param parent 输入或输出参数，具体含义见调用处和函数内部使用方式。
 *
 * @return 函数执行结果或计算得到的值，具体语义见返回路径。
 */
lv_obj_t *watch_plane_war_create(lv_obj_t *parent)
{
    if(s_plane_war.timer || s_plane_war.page) {
        watch_plane_war_destroy();
    }

    memset(&s_plane_war, 0, sizeof(s_plane_war));

    s_plane_war.page = lv_obj_create(parent);
    if(s_plane_war.page == NULL) {
        return NULL;
    }

    lv_obj_remove_style_all(s_plane_war.page);
    lv_obj_set_size(s_plane_war.page, WATCH_SCREEN_W, WATCH_SCREEN_H);
    lv_obj_set_pos(s_plane_war.page, 0, 0);
    lv_obj_set_style_bg_color(s_plane_war.page, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_plane_war.page, LV_OPA_COVER, 0);
    lv_obj_clear_flag(s_plane_war.page, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(s_plane_war.page, LV_OBJ_FLAG_CLICKABLE);


    s_plane_war.bg_img = lv_img_create(s_plane_war.page);

    if(s_plane_war.bg_img == NULL) {
        watch_plane_war_destroy();
        return NULL;
    }

    lv_img_set_src(s_plane_war.bg_img, &plane_war_bg);
    lv_obj_set_pos(s_plane_war.bg_img, 0, 0);
    lv_obj_clear_flag(s_plane_war.bg_img, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(s_plane_war.bg_img, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_move_background(s_plane_war.bg_img);


    for(int i = 0; i < PLANE_WAR_BULLET_COUNT; i++) {
        s_plane_war.bullet[i].obj = lv_obj_create(s_plane_war.page);

        if(s_plane_war.bullet[i].obj == NULL) {
            watch_plane_war_destroy();
            return NULL;
        }

        lv_obj_remove_style_all(s_plane_war.bullet[i].obj);
        lv_obj_set_size(s_plane_war.bullet[i].obj,
                        PLANE_WAR_BULLET_W,
                        PLANE_WAR_BULLET_H);
        lv_obj_set_style_bg_color(s_plane_war.bullet[i].obj,
                                  lv_color_white(),
                                  0);
        lv_obj_set_style_bg_opa(s_plane_war.bullet[i].obj,
                                LV_OPA_COVER,
                                0);
        lv_obj_set_style_radius(s_plane_war.bullet[i].obj,
                                0,
                                0);
        lv_obj_clear_flag(s_plane_war.bullet[i].obj, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_clear_flag(s_plane_war.bullet[i].obj, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_flag(s_plane_war.bullet[i].obj, LV_OBJ_FLAG_HIDDEN);
    }


    for(int i = 0; i < PLANE_WAR_HOSTILE_COUNT; i++) {
        s_plane_war.hostile[i].obj = lv_img_create(s_plane_war.page);

        if(s_plane_war.hostile[i].obj == NULL) {
            watch_plane_war_destroy();
            return NULL;
        }

        lv_img_set_src(s_plane_war.hostile[i].obj, &hostile_jet);
        lv_obj_clear_flag(s_plane_war.hostile[i].obj, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_clear_flag(s_plane_war.hostile[i].obj, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_flag(s_plane_war.hostile[i].obj, LV_OBJ_FLAG_HIDDEN);


        s_plane_war.hostile[i].explosion_ring = lv_obj_create(s_plane_war.page);
        if(s_plane_war.hostile[i].explosion_ring == NULL) {
            watch_plane_war_destroy();
            return NULL;
        }

        lv_obj_remove_style_all(s_plane_war.hostile[i].explosion_ring);
        lv_obj_set_size(s_plane_war.hostile[i].explosion_ring,
                        PLANE_WAR_EXPLOSION_RING_MIN_SIZE,
                        PLANE_WAR_EXPLOSION_RING_MIN_SIZE);
        lv_obj_set_style_bg_opa(s_plane_war.hostile[i].explosion_ring,
                                LV_OPA_TRANSP,
                                0);
        lv_obj_set_style_border_color(s_plane_war.hostile[i].explosion_ring,
                                      lv_color_hex(0xFFB000),
                                      0);
        lv_obj_set_style_border_width(s_plane_war.hostile[i].explosion_ring,
                                      2,
                                      0);
        lv_obj_set_style_border_opa(s_plane_war.hostile[i].explosion_ring,
                                    LV_OPA_TRANSP,
                                    0);
        lv_obj_clear_flag(s_plane_war.hostile[i].explosion_ring,
                          LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_clear_flag(s_plane_war.hostile[i].explosion_ring,
                          LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_flag(s_plane_war.hostile[i].explosion_ring,
                        LV_OBJ_FLAG_HIDDEN);

        s_plane_war.hostile[i].explosion_core = lv_obj_create(s_plane_war.page);
        if(s_plane_war.hostile[i].explosion_core == NULL) {
            watch_plane_war_destroy();
            return NULL;
        }

        lv_obj_remove_style_all(s_plane_war.hostile[i].explosion_core);
        lv_obj_set_size(s_plane_war.hostile[i].explosion_core,
                        PLANE_WAR_EXPLOSION_CORE_MIN_SIZE,
                        PLANE_WAR_EXPLOSION_CORE_MIN_SIZE);
        lv_obj_set_style_bg_color(s_plane_war.hostile[i].explosion_core,
                                  lv_color_hex(0xFFF06A),
                                  0);
        lv_obj_set_style_bg_opa(s_plane_war.hostile[i].explosion_core,
                                LV_OPA_TRANSP,
                                0);
        lv_obj_clear_flag(s_plane_war.hostile[i].explosion_core,
                          LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_clear_flag(s_plane_war.hostile[i].explosion_core,
                          LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_flag(s_plane_war.hostile[i].explosion_core,
                        LV_OBJ_FLAG_HIDDEN);

        for(int j = 0; j < PLANE_WAR_EXPLOSION_PARTICLE_COUNT; j++) {
            s_plane_war.hostile[i].explosion_particle[j] =
                lv_obj_create(s_plane_war.page);

            if(s_plane_war.hostile[i].explosion_particle[j] == NULL) {
                watch_plane_war_destroy();
                return NULL;
            }

            lv_obj_remove_style_all(s_plane_war.hostile[i].explosion_particle[j]);
            lv_obj_set_size(s_plane_war.hostile[i].explosion_particle[j],
                            PLANE_WAR_EXPLOSION_PARTICLE_SIZE,
                            PLANE_WAR_EXPLOSION_PARTICLE_SIZE);
            lv_obj_set_style_bg_color(s_plane_war.hostile[i].explosion_particle[j],
                                      lv_color_hex(0xFF8A00),
                                      0);
            lv_obj_set_style_bg_opa(s_plane_war.hostile[i].explosion_particle[j],
                                    LV_OPA_TRANSP,
                                    0);
            lv_obj_clear_flag(s_plane_war.hostile[i].explosion_particle[j],
                              LV_OBJ_FLAG_SCROLLABLE);
            lv_obj_clear_flag(s_plane_war.hostile[i].explosion_particle[j],
                              LV_OBJ_FLAG_CLICKABLE);
            lv_obj_add_flag(s_plane_war.hostile[i].explosion_particle[j],
                            LV_OBJ_FLAG_HIDDEN);
        }
    }


    s_plane_war.jet_img = lv_img_create(s_plane_war.page);

    if(s_plane_war.jet_img == NULL) {
        watch_plane_war_destroy();
        return NULL;
    }

    lv_img_set_src(s_plane_war.jet_img, &our_jet);
    lv_obj_clear_flag(s_plane_war.jet_img, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(s_plane_war.jet_img, LV_OBJ_FLAG_CLICKABLE);


    for(int i = 0; i < PLANE_WAR_LIFE_MAX; i++) {
        s_plane_war.life_img[i] = lv_img_create(s_plane_war.page);

        if(s_plane_war.life_img[i] == NULL) {
            watch_plane_war_destroy();
            return NULL;
        }

        lv_img_set_src(s_plane_war.life_img[i], &heart);
        lv_obj_set_pos(s_plane_war.life_img[i],
                        PLANE_WAR_HEART_START_X,
                        PLANE_WAR_HEART_START_Y +
                        i * (PLANE_WAR_HEART_H + PLANE_WAR_HEART_GAP));
        lv_obj_clear_flag(s_plane_war.life_img[i], LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_clear_flag(s_plane_war.life_img[i], LV_OBJ_FLAG_CLICKABLE);
        lv_obj_clear_flag(s_plane_war.life_img[i], LV_OBJ_FLAG_HIDDEN);
    }


    s_plane_war.score_label = lv_label_create(s_plane_war.page);

    if(s_plane_war.score_label == NULL) {
        watch_plane_war_destroy();
        return NULL;
    }

    lv_obj_set_style_text_font(s_plane_war.score_label,
                               &lv_font_montserrat_20,
                               0);
    lv_obj_set_style_text_color(s_plane_war.score_label,
                                lv_color_white(),
                                0);
    lv_obj_set_style_text_align(s_plane_war.score_label,
                                LV_TEXT_ALIGN_RIGHT,
                                0);
    lv_obj_set_size(s_plane_war.score_label,
                    PLANE_WAR_SCORE_LABEL_W,
                    PLANE_WAR_SCORE_LABEL_H);
    lv_obj_set_pos(s_plane_war.score_label,
                   WATCH_SCREEN_W - PLANE_WAR_SCORE_LABEL_W - PLANE_WAR_SCORE_MARGIN_X,
                   PLANE_WAR_SCORE_MARGIN_Y);
    lv_obj_clear_flag(s_plane_war.score_label, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(s_plane_war.score_label, LV_OBJ_FLAG_CLICKABLE);

#if PLANE_WAR_SCORE_ALWAYS_SHOW
    lv_obj_clear_flag(s_plane_war.score_label, LV_OBJ_FLAG_HIDDEN);
#else
    lv_obj_add_flag(s_plane_war.score_label, LV_OBJ_FLAG_HIDDEN);
#endif

    s_plane_war.timer = lv_timer_create(plane_war_timer_cb,
                                        PLANE_WAR_TIMER_MS,
                                        NULL);

    if(s_plane_war.timer == NULL) {
        watch_plane_war_destroy();
        return NULL;
    }

    lv_timer_pause(s_plane_war.timer);

    s_plane_war.active = false;
    s_plane_war.wants_back = false;
    s_plane_war.game_over = false;
    s_plane_war.score = 0;
    s_plane_war.life = PLANE_WAR_LIFE_MAX;
    s_plane_war.last_shot_tick = 0;
    s_plane_war.next_hostile_tick = 0;

    plane_war_set_jet_pos(PLANE_WAR_JET_START_X, PLANE_WAR_JET_START_Y);
    plane_war_life_reset();
    plane_war_score_reset();
    plane_war_bullets_reset();
    plane_war_hostiles_reset();

    return s_plane_war.page;
}

/**
 * @brief 创建或重置飞机大战页面。
 *
 * 详细说明：
 * - 初始化背景、玩家、对象池、生命和分数。
 */
void watch_plane_war_reset(void)
{
    if(s_plane_war.page == NULL) {
        return;
    }

    s_plane_war.active = false;
    s_plane_war.wants_back = false;
    s_plane_war.game_over = false;
    uint32_t now = lv_tick_get();
    s_plane_war.last_shot_tick = now;
    plane_war_schedule_next_hostile(now);

    plane_war_set_jet_pos(PLANE_WAR_JET_START_X, PLANE_WAR_JET_START_Y);
    plane_war_life_reset();
    plane_war_score_reset();
    plane_war_bullets_reset();
    plane_war_hostiles_reset();

    if(s_plane_war.timer) {
        lv_timer_pause(s_plane_war.timer);
    }
}

/**
 * @brief 开始或恢复游戏。
 *
 * 详细说明：
 * - 启动定时器并设置 active 状态。
 */
void watch_plane_war_start(void)
{
    if(s_plane_war.page == NULL) {
        return;
    }

    s_plane_war.active = true;
    s_plane_war.wants_back = false;
    s_plane_war.game_over = false;
    uint32_t now = lv_tick_get();
    s_plane_war.last_shot_tick = now;

    plane_war_set_jet_pos(PLANE_WAR_JET_START_X, PLANE_WAR_JET_START_Y);
    plane_war_life_reset();
    plane_war_score_reset();
    plane_war_bullets_reset();
    plane_war_hostiles_reset();
    plane_war_spawn_hostile();
    plane_war_schedule_next_hostile(now);

    if(s_plane_war.timer) {
        lv_timer_reset(s_plane_war.timer);
        lv_timer_resume(s_plane_war.timer);
    }
}

/**
 * @brief 暂停游戏。
 *
 * 详细说明：
 * - 停止定时器和 active 状态。
 */
void watch_plane_war_stop(void)
{
    if(s_plane_war.page == NULL) {
        return;
    }

    s_plane_war.active = false;
    s_plane_war.wants_back = false;
    s_plane_war.game_over = false;
    plane_war_bullets_reset();
    plane_war_hostiles_reset();

    if(s_plane_war.timer) {
        lv_timer_pause(s_plane_war.timer);
    }
}

/**
 * @brief 处理飞机大战按键。
 *
 * 详细说明：
 * - 主要处理返回等离散按键，移动键由 GPIO 轮询。
 *
 * @param key 输入或输出参数，具体含义见调用处和函数内部使用方式。
 */
void watch_plane_war_on_key(watch_key_t key)
{
    if(s_plane_war.page == NULL || !s_plane_war.active) {
        return;
    }

    if(key == WATCH_KEY_2_RELEASE) {
        return;
    }


    if(key == WATCH_KEY_2) {
        s_plane_war.wants_back = true;
        return;
    }
}

/**
 * @brief 查询飞机大战是否请求返回。
 *
 * 详细说明：
 * - 供游戏中心状态机使用。
 *
 * @return 函数执行结果或计算得到的值，具体语义见返回路径。
 */
bool watch_plane_war_wants_back(void)
{
    return s_plane_war.wants_back;
}
