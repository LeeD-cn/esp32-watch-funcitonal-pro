/**
 * @file main.c
 * @brief 智能手表应用入口与系统级电源管理。
 *
 * @details 负责显示、按键、传感器、配置和时间服务初始化，并实现自动息屏、唤醒与关机流程。
 */

#include <stdio.h>
#include <stdbool.h>
#include <stdint.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl.h"
#include "lcd_st7789_official.h"
#include "lvgl_port.h"
#include "watch_ui.h"
#include "watch_keys.h"
#include "time_sync.h"
#include "watch_rtc.h"
#include "watch_battery.h"
#include "watch_bmi270.h"
#include "watch_config.h"
#include "watch_serial_config.h"
#include "driver/gpio.h"
#include "esp_err.h"
#include "esp_sleep.h"
#include "sdkconfig.h"

#define WATCH_LVGL_TASK_STACK_SIZE  8192
#define WATCH_LVGL_TASK_PRIORITY    3
#define WATCH_LVGL_TASK_CORE        1

#define KEY4_GPIO                   GPIO_NUM_21
#define BMI270_INT_GPIO             GPIO_NUM_5
#define POWER_HOLD_GPIO             GPIO_NUM_18

#define KEY4_DEBOUNCE_MS            60
#define KEY4_LONG_PRESS_MS          1200
#define AUTO_OFF_IDLE_MS            15000

/* 1：使用 ESP light sleep；0：使用轮询待机并保持 CPU/USB 活跃。 */
#define WATCH_USE_ESP_LIGHT_SLEEP     1

extern bool watch_settings_auto_off_enabled(void);
extern esp_err_t watch_bmi270_enable_data_ready_interrupt(bool enable);
extern void watch_keys_key4_scan_suspend(void);
extern void watch_keys_key4_scan_resume(void);
extern esp_err_t watch_wifi_stop(void);
extern void lcd_panel_sleep(bool sleep);

/* LCD 面板句柄。 */
static esp_lcd_panel_handle_t s_lcd_panel = NULL;
/* 息屏流程状态。 */
static bool s_watch_sleeping = false;
/* LVGL tick 重置标志。 */
static bool s_lvgl_tick_reset_needed = false;

/**
 * @brief 判断按键事件是否应重置自动息屏计时。
 *
 * @param key 按键事件。
 * @return 属于 KEY1 至 KEY3 的用户活动时返回 true。
 */
static bool watch_is_key1_to_key3_activity(watch_key_t key)
{
    switch(key) {
    case WATCH_KEY_1:
    case WATCH_KEY_2:
    case WATCH_KEY_2_RELEASE:
    case WATCH_KEY_3:
        return true;

    default:
        return false;
    }
}

/**
 * @brief 初始化电源保持 GPIO 并输出高电平。
 */
static void gpio18_init_high(void)
{
    gpio_config_t io_conf = {
        .pin_bit_mask = 1ULL << POWER_HOLD_GPIO,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE
    };

    (void)gpio_hold_dis(POWER_HOLD_GPIO);
    ESP_ERROR_CHECK(gpio_config(&io_conf));
    ESP_ERROR_CHECK(gpio_set_level(POWER_HOLD_GPIO, 1));

    /* 禁用睡眠态 GPIO 配置，保持输出电平。 */
    gpio_sleep_sel_dis(POWER_HOLD_GPIO);
}

/**
 * @brief 保持外部电源自锁有效。
 */
static void power_hold_keep_on(void)
{
    ESP_ERROR_CHECK(gpio_set_level(POWER_HOLD_GPIO, 1));
}

/**
 * @brief 释放外部电源自锁以关闭设备。
 */
static void power_hold_force_off(void)
{
    gpio_config_t io_conf = {
        .pin_bit_mask = 1ULL << POWER_HOLD_GPIO,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE
    };

    /* 解除保持并恢复普通输出模式后拉低。 */
    (void)gpio_hold_dis(POWER_HOLD_GPIO);
    (void)gpio_reset_pin(POWER_HOLD_GPIO);
    ESP_ERROR_CHECK(gpio_config(&io_conf));
    gpio_sleep_sel_dis(POWER_HOLD_GPIO);
    ESP_ERROR_CHECK(gpio_set_level(POWER_HOLD_GPIO, 0));
}

/**
 * @brief 获取 KEY4 按下状态。
 *
 * @return KEY4 为低电平时返回 true。
 */
static bool key4_is_pressed(void)
{
    return gpio_get_level(KEY4_GPIO) == 0;
}

/**
 * @brief 将 BMI270 中断引脚初始化为普通输入。
 */
static void bmi270_int_gpio_init_safe(void)
{
    /* 清除中断和唤醒配置后重新初始化引脚。 */
    (void)gpio_intr_disable(BMI270_INT_GPIO);
    (void)gpio_set_intr_type(BMI270_INT_GPIO, GPIO_INTR_DISABLE);
    (void)gpio_wakeup_disable(BMI270_INT_GPIO);
    (void)gpio_reset_pin(BMI270_INT_GPIO);

    gpio_config_t int_conf = {
        .pin_bit_mask = 1ULL << BMI270_INT_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };

    ESP_ERROR_CHECK(gpio_config(&int_conf));
}

/**
 * @brief 等待 KEY4 松开并完成消抖。
 */
static void key4_wait_release(void)
{
    while(key4_is_pressed()) {
        vTaskDelay(pdMS_TO_TICKS(20));
    }

    vTaskDelay(pdMS_TO_TICKS(KEY4_DEBOUNCE_MS));
}

/**
 * @brief 配置启动阶段使用的 KEY4 输入。
 */
static void key4_config_input_for_boot(void)
{
    gpio_config_t key4_conf = {
        .pin_bit_mask = 1ULL << KEY4_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };

    ESP_ERROR_CHECK(gpio_config(&key4_conf));
}

/**
 * @brief 忽略启动期间尚未释放的 KEY4 按压。
 */
static void key4_ignore_boot_press_until_release(void)
{
    if(!key4_is_pressed()) {
        return;
    }

    key4_wait_release();
}

/**
 * @brief 立即刷新当前 LVGL 屏幕。
 */
static void watch_display_force_lvgl_refresh(void)
{
    lv_disp_t *disp = lv_disp_get_default();
    if(disp == NULL) {
        return;
    }

    lv_obj_t *scr = lv_disp_get_scr_act(disp);
    if(scr != NULL) {
        /* 使当前屏幕全部失效，保证面板内容与 LVGL 状态同步。 */
        lv_obj_invalidate(scr);
    }

    lv_refr_now(disp);
}

/**
 * @brief 唤醒后触发 LVGL 活动并执行整屏刷新。
 */
static void watch_display_refresh_after_wake(void)
{
    lv_disp_t *disp = lv_disp_get_default();
    if(disp != NULL) {
        lv_disp_trig_activity(disp);
    }

    /* 分两次刷新，为面板唤醒和首帧传输预留稳定时间。 */
    vTaskDelay(pdMS_TO_TICKS(20));
    watch_display_force_lvgl_refresh();
    vTaskDelay(pdMS_TO_TICKS(50));
    watch_display_force_lvgl_refresh();
}

/**
 * @brief 设置显示开关状态。
 *
 * @param on true 时唤醒并刷新显示，false 时关闭显示。
 */
static void watch_display_set_on(bool on)
{
    (void)s_lcd_panel;
    lcd_panel_sleep(!on);

    if(on) {
        watch_display_refresh_after_wake();
    }
}

/**
 * @brief 关闭显示并释放电源保持信号。
 */
static void watch_power_off(void)
{
    watch_display_set_on(false);
    power_hold_force_off();

    while(1) {
        (void)gpio_set_level(POWER_HOLD_GPIO, 0);
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

/**
 * @brief 处理息屏期间的 KEY4 按压。
 *
 * @return 短按释放后返回 true；未形成有效短按时返回 false。
 */
static bool watch_sleep_handle_key4_interrupt(void)
{
    vTaskDelay(pdMS_TO_TICKS(KEY4_DEBOUNCE_MS));
    if(!key4_is_pressed()) {
        return false;
    }

    TickType_t press_tick = xTaskGetTickCount();

    while(key4_is_pressed()) {
        TickType_t now = xTaskGetTickCount();

        if((now - press_tick) >= pdMS_TO_TICKS(KEY4_LONG_PRESS_MS)) {
            watch_power_off();
            return false;
        }

        vTaskDelay(pdMS_TO_TICKS(20));
    }

    vTaskDelay(pdMS_TO_TICKS(KEY4_DEBOUNCE_MS));
    return true;
}

/**
 * @brief 配置 KEY4 和 BMI270 的 GPIO 唤醒源。
 *
 * @return ESP_OK 表示配置成功，否则返回对应错误码。
 */
static esp_err_t watch_sleep_gpio_wakeup_enable(void)
{
    /* KEY4 低电平、BMI270 INT 高电平触发唤醒。 */
    esp_err_t ret = gpio_wakeup_enable(KEY4_GPIO, GPIO_INTR_LOW_LEVEL);
    if(ret != ESP_OK) {
        return ret;
    }

    ret = gpio_wakeup_enable(BMI270_INT_GPIO, GPIO_INTR_HIGH_LEVEL);
    if(ret != ESP_OK) {
        (void)gpio_wakeup_disable(KEY4_GPIO);
        return ret;
    }

    ret = esp_sleep_enable_gpio_wakeup();
    if(ret != ESP_OK) {
        (void)gpio_wakeup_disable(KEY4_GPIO);
        (void)gpio_wakeup_disable(BMI270_INT_GPIO);
        return ret;
    }

    return ESP_OK;
}

/**
 * @brief 关闭并清理 GPIO 唤醒源。
 */
static void watch_sleep_gpio_wakeup_disable(void)
{
    (void)gpio_wakeup_disable(KEY4_GPIO);
    (void)gpio_wakeup_disable(BMI270_INT_GPIO);
    (void)esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_GPIO);
}

/**
 * @brief 等待 KEY4 短按或抬腕事件。
 *
 * @return 检测到有效唤醒事件时返回 true。
 */
static bool watch_sleep_wait_key4_or_raise_wrist(void)
{
#if WATCH_USE_ESP_LIGHT_SLEEP
    bool gpio_wakeup_enabled = (watch_sleep_gpio_wakeup_enable() == ESP_OK);
#else
    bool gpio_wakeup_enabled = false;
#endif

    while(1) {
        if(key4_is_pressed()) {
            if(watch_sleep_handle_key4_interrupt()) {
                if(gpio_wakeup_enabled) {
                    watch_sleep_gpio_wakeup_disable();
                }
                return true;
            }

            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }

        if(gpio_get_level(BMI270_INT_GPIO) == 1) {
            if(watch_bmi270_raise_wrist_poll()) {
                if(gpio_wakeup_enabled) {
                    watch_sleep_gpio_wakeup_disable();
                }
                return true;
            }

            /* poll() 同时读取并清除非抬腕的数据就绪中断。 */
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }

        if(gpio_wakeup_enabled) {
#if WATCH_USE_ESP_LIGHT_SLEEP
            (void)esp_light_sleep_start();
#endif
        }
        else {
            /* 轮询模式下保持 CPU/USB 活跃。 */
            vTaskDelay(pdMS_TO_TICKS(20));
        }
    }
}

/**
 * @brief 进入息屏状态并等待按键或抬腕唤醒。
 */
static void watch_enter_light_sleep_until_key4(void)
{
    if(s_watch_sleeping) {
        return;
    }

    s_watch_sleeping = true;

    power_hold_keep_on();

    /* 息屏期间停止 Wi-Fi 以降低功耗。 */
    (void)watch_wifi_stop();

    watch_display_set_on(false);
    watch_keys_clear_events();
    watch_keys_key4_scan_suspend();

    key4_wait_release();
    watch_bmi270_raise_wrist_begin();

    if(watch_bmi270_enable_data_ready_interrupt(true) != ESP_OK) {
        /* BMI270 中断配置失败时仅保留 KEY4 唤醒。 */
        (void)gpio_intr_disable(BMI270_INT_GPIO);
        (void)gpio_set_intr_type(BMI270_INT_GPIO, GPIO_INTR_DISABLE);
    }

    if(watch_sleep_wait_key4_or_raise_wrist()) {
        (void)watch_bmi270_enable_data_ready_interrupt(false);
        key4_wait_release();
        power_hold_keep_on();
        watch_keys_clear_events();
        watch_keys_key4_scan_resume();
        watch_bmi270_raise_wrist_end();
        s_lvgl_tick_reset_needed = true;

        /* 等待外设时钟稳定后再唤醒并刷新显示。 */
        vTaskDelay(pdMS_TO_TICKS(30));
        watch_display_set_on(true);
        s_watch_sleeping = false;
    }
}

/**
 * @brief 处理系统级按键事件。
 *
 * @param key 按键事件。
 * @return 事件已处理时返回 true。
 */
static bool watch_handle_global_key(watch_key_t key)
{
    switch(key) {
    case WATCH_KEY_4_SHORT:
        watch_enter_light_sleep_until_key4();
        return true;

    case WATCH_KEY_4_LONG:
        watch_power_off();
        return true;

    default:
        return false;
    }
}

/**
 * @brief 检查自动息屏条件并执行息屏流程。
 *
 * @param last_key1_to_key3_tick 最近一次用户活动时刻。
 * @return 已执行自动息屏时返回 true。
 */
static bool watch_auto_off_check(TickType_t *last_key1_to_key3_tick)
{
    if(last_key1_to_key3_tick == NULL || !watch_settings_auto_off_enabled()) {
        return false;
    }

    TickType_t now = xTaskGetTickCount();
    if((now - *last_key1_to_key3_tick) < pdMS_TO_TICKS(AUTO_OFF_IDLE_MS)) {
        return false;
    }

    watch_enter_light_sleep_until_key4();
    *last_key1_to_key3_tick = xTaskGetTickCount();
    return true;
}

/**
 * @brief 运行 LVGL、按键分发和自动息屏逻辑。
 *
 * @param arg 任务参数，当前未使用。
 */
static void watch_lvgl_task(void *arg)
{
    (void)arg;

    /* LVGL 对象创建和事件处理均在本任务内执行。 */
    watch_ui_create();

    TickType_t last_tick = xTaskGetTickCount();
    TickType_t last_key1_to_key3_tick = last_tick;

    while(1) {
        watch_key_t key;

        TickType_t now_tick = xTaskGetTickCount();
        TickType_t diff_tick = now_tick - last_tick;

        if(s_lvgl_tick_reset_needed) {
            last_tick = now_tick;
            s_lvgl_tick_reset_needed = false;
        }
        else if(diff_tick > 0) {
            lv_tick_inc((uint32_t)diff_tick * portTICK_PERIOD_MS);
            last_tick = now_tick;
        }

        while(watch_keys_get_event(&key)) {
            if(watch_is_key1_to_key3_activity(key)) {
                last_key1_to_key3_tick = xTaskGetTickCount();
            }

            if(watch_handle_global_key(key)) {
                last_key1_to_key3_tick = xTaskGetTickCount();
                continue;
            }

            watch_ui_on_key(key);
        }

        if(watch_auto_off_check(&last_key1_to_key3_tick)) {
            continue;
        }

        uint32_t wait_ms = lv_timer_handler();

        /* 限制等待时间以兼顾刷新频率和按键响应。 */
        if(wait_ms < 5) {
            wait_ms = 5;
        }
        else if(wait_ms > 10) {
            wait_ms = 10;
        }

        TickType_t delay_ticks = pdMS_TO_TICKS(wait_ms);
        
        if(delay_ticks < 1) {
            delay_ticks = 1;
        }

        vTaskDelay(delay_ticks);
    }
}

/**
 * @brief ESP-IDF 应用入口。
 */
void app_main(void)
{
    gpio18_init_high();

    key4_config_input_for_boot();
    key4_ignore_boot_press_until_release();
    bmi270_int_gpio_init_safe();
    watch_keys_clear_events();

    ESP_ERROR_CHECK(watch_config_init());
    ESP_ERROR_CHECK(watch_serial_config_start());

    esp_lcd_panel_handle_t lcd_panel = lcd_panel_init();
    s_lcd_panel = lcd_panel;
    lvgl_port_init(lcd_panel);

    (void)watch_rtc_restore_or_init_2026();
    (void)watch_battery_init();
    (void)watch_bmi270_init();

    watch_time_sync_start();
    ESP_ERROR_CHECK(watch_keys_init());

    BaseType_t ret = xTaskCreatePinnedToCore(watch_lvgl_task,
                                             "watch_lvgl",
                                             WATCH_LVGL_TASK_STACK_SIZE,
                                             NULL,
                                             WATCH_LVGL_TASK_PRIORITY,
                                             NULL,
                                             WATCH_LVGL_TASK_CORE);

    if(ret != pdPASS) {
        return;
    }
}
