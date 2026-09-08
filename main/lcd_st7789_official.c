#include "lcd_st7789_official.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "driver/ledc.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"
#include "nvs_flash.h"

#include <stdbool.h>
#include <stdint.h>

/**
 * @file lcd_st7789_official.c
 * @brief ST7789 显示面板与背光控制。
 *
 * @details 封装面板初始化、LEDC 背光调节、亮度持久化以及休眠与唤醒控制。
 */

static const char *TAG = "LCD_ST7789_OFFICIAL";

#define LCD_BACKLIGHT_LEDC_MODE        LEDC_LOW_SPEED_MODE
#define LCD_BACKLIGHT_LEDC_TIMER       LEDC_TIMER_0
#define LCD_BACKLIGHT_LEDC_CHANNEL     LEDC_CHANNEL_0
#define LCD_BACKLIGHT_PWM_FREQ_HZ      5000
#define LCD_BACKLIGHT_PWM_DUTY_RES     LEDC_TIMER_10_BIT
#define LCD_BACKLIGHT_PWM_MAX_DUTY     ((1U << 10) - 1U)

/* UI 亮度范围映射到受限 PWM 占空比，避免背光过亮。 */
#define LCD_BACKLIGHT_UI_MIN_PERCENT       30U
#define LCD_BACKLIGHT_UI_MAX_PERCENT       100U
#define LCD_BACKLIGHT_PWM_MIN_PERCENT      30U
#define LCD_BACKLIGHT_PWM_LIMIT_PERCENT    70U

#define LCD_BACKLIGHT_NVS_NAMESPACE        "watch"
#define LCD_BACKLIGHT_NVS_KEY              "brightness"

#define ST7789_CMD_DISPOFF                  0x28
#define ST7789_CMD_DISPON                   0x29
#define ST7789_CMD_SLPIN                    0x10
#define ST7789_CMD_SLPOUT                   0x11

/* 1：使用 SLPIN/SLPOUT；0：仅使用 DISPOFF/DISPON。 */
#define LCD_PANEL_USE_SLEEP_IN              1

/* 1：休眠时仅关闭背光；0：同时控制面板显示状态。 */
#define LCD_PANEL_BACKLIGHT_ONLY_SLEEP      0

/* ST7789 面板 IO 句柄。 */
static esp_lcd_panel_io_handle_t s_lcd_io_handle = NULL;
/* ST7789 面板句柄。 */
static esp_lcd_panel_handle_t s_lcd_panel_handle = NULL;
/* 背光 PWM 初始化状态。 */
static bool s_backlight_pwm_inited = false;
/* 当前 UI 亮度百分比，0 表示关闭。 */
static uint8_t s_backlight_percent = 100;
/* 最近一次非零亮度百分比。 */
static uint8_t s_backlight_last_nonzero_percent = 100;
/* NVS 初始化状态。 */
static bool s_backlight_nvs_ready = false;
/* 面板逻辑休眠状态。 */
static bool s_lcd_panel_sleeping = false;

/**
 * @brief 规范化 UI 亮度百分比。
 *
 * @param percent UI 亮度百分比，0 表示关闭背光。
 * @return 0，或限制在允许范围内的亮度百分比。
 */
static uint8_t lcd_backlight_normalize_percent(uint8_t percent)
{
    if(percent == 0) {
        return 0;
    }

    if(percent < LCD_BACKLIGHT_UI_MIN_PERCENT) {
        return LCD_BACKLIGHT_UI_MIN_PERCENT;
    }

    if(percent > LCD_BACKLIGHT_UI_MAX_PERCENT) {
        return LCD_BACKLIGHT_UI_MAX_PERCENT;
    }

    return percent;
}

/**
 * @brief 将 UI 亮度百分比转换为 LEDC 占空比。
 *
 * @param percent UI 亮度百分比，0 表示关闭背光。
 * @return LEDC 占空比。
 */
static uint32_t lcd_backlight_percent_to_duty(uint8_t percent)
{
    percent = lcd_backlight_normalize_percent(percent);
    if(percent == 0) {
        return 0;
    }

    uint32_t ui_range = LCD_BACKLIGHT_UI_MAX_PERCENT -
                        LCD_BACKLIGHT_UI_MIN_PERCENT;
    uint32_t pwm_range = LCD_BACKLIGHT_PWM_LIMIT_PERCENT -
                         LCD_BACKLIGHT_PWM_MIN_PERCENT;
    uint32_t pwm_percent = LCD_BACKLIGHT_PWM_MIN_PERCENT +
                           (((uint32_t)percent - LCD_BACKLIGHT_UI_MIN_PERCENT) *
                            pwm_range + ui_range / 2U) / ui_range;

    return (pwm_percent * LCD_BACKLIGHT_PWM_MAX_DUTY) / 100U;
}

/**
 * @brief 初始化亮度配置使用的 NVS。
 *
 * @return 初始化成功返回 true，否则返回 false。
 */
static bool lcd_backlight_ensure_nvs_ready(void)
{
    if(s_backlight_nvs_ready) {
        return true;
    }

    esp_err_t err = nvs_flash_init();
    if(err == ESP_ERR_NVS_NO_FREE_PAGES ||
       err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        /* 分区无可用页或版本不兼容时重建 NVS。 */
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

    s_backlight_nvs_ready = true;
    return true;
}

/**
 * @brief 从 NVS 恢复已保存的非零背光亮度。
 */
static void lcd_backlight_load_saved_percent(void)
{
    if(!lcd_backlight_ensure_nvs_ready()) {
        return;
    }

    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open(LCD_BACKLIGHT_NVS_NAMESPACE,
                             NVS_READONLY,
                             &nvs_handle);
    if(err == ESP_ERR_NVS_NOT_FOUND) {
        return;
    }
    if(err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to open NVS for backlight load: %s",
                 esp_err_to_name(err));
        return;
    }

    uint8_t saved_percent = 0;
    err = nvs_get_u8(nvs_handle, LCD_BACKLIGHT_NVS_KEY, &saved_percent);
    nvs_close(nvs_handle);

    if(err == ESP_ERR_NVS_NOT_FOUND) {
        return;
    }
    if(err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to load backlight brightness: %s",
                 esp_err_to_name(err));
        return;
    }

    saved_percent = lcd_backlight_normalize_percent(saved_percent);
    if(saved_percent == 0) {
        return;
    }

    s_backlight_percent = saved_percent;
    s_backlight_last_nonzero_percent = saved_percent;
    ESP_LOGI(TAG, "Loaded backlight brightness: %u", saved_percent);
}

/**
 * @brief 将非零背光亮度保存到 NVS。
 *
 * @param percent 待保存的 UI 亮度百分比。
 */
static void lcd_backlight_save_percent(uint8_t percent)
{
    percent = lcd_backlight_normalize_percent(percent);
    if(percent == 0) {
        return;
    }

    if(!lcd_backlight_ensure_nvs_ready()) {
        return;
    }

    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open(LCD_BACKLIGHT_NVS_NAMESPACE,
                             NVS_READWRITE,
                             &nvs_handle);
    if(err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to open NVS for backlight save: %s",
                 esp_err_to_name(err));
        return;
    }

    err = nvs_set_u8(nvs_handle, LCD_BACKLIGHT_NVS_KEY, percent);
    if(err == ESP_OK) {
        err = nvs_commit(nvs_handle);
    }
    nvs_close(nvs_handle);

    if(err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to save backlight brightness: %s",
                 esp_err_to_name(err));
    }
}

/**
 * @brief 应用背光亮度并更新运行状态。
 *
 * @param percent UI 亮度百分比，0 表示关闭背光。
 * @param save true 时将非零亮度保存到 NVS。
 */
static void lcd_apply_backlight_percent(uint8_t percent, bool save)
{
    percent = lcd_backlight_normalize_percent(percent);

    s_backlight_percent = percent;
    if(percent > 0) {
        s_backlight_last_nonzero_percent = percent;

        if(save) {
            lcd_backlight_save_percent(percent);
        }
    }

    if(!s_backlight_pwm_inited) {
        return;
    }

    uint32_t duty = lcd_backlight_percent_to_duty(percent);
    ESP_ERROR_CHECK(ledc_set_duty(LCD_BACKLIGHT_LEDC_MODE,
                                  LCD_BACKLIGHT_LEDC_CHANNEL,
                                  duty));
    ESP_ERROR_CHECK(ledc_update_duty(LCD_BACKLIGHT_LEDC_MODE,
                                     LCD_BACKLIGHT_LEDC_CHANNEL));
}

/**
 * @brief 停止背光 PWM 输出。
 */
static void lcd_backlight_pwm_stop_output(void)
{
    if(!s_backlight_pwm_inited) {
        return;
    }

    (void)ledc_stop(LCD_BACKLIGHT_LEDC_MODE, LCD_BACKLIGHT_LEDC_CHANNEL, 0);
}

/**
 * @brief 初始化背光 LEDC 定时器和通道。
 */
static void lcd_backlight_pwm_init(void)
{
    ledc_timer_config_t timer_config = {
        .speed_mode = LCD_BACKLIGHT_LEDC_MODE,
        .timer_num = LCD_BACKLIGHT_LEDC_TIMER,
        .duty_resolution = LCD_BACKLIGHT_PWM_DUTY_RES,
        .freq_hz = LCD_BACKLIGHT_PWM_FREQ_HZ,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    ESP_ERROR_CHECK(ledc_timer_config(&timer_config));

    ledc_channel_config_t channel_config = {
        .gpio_num = PIN_NUM_BL,
        .speed_mode = LCD_BACKLIGHT_LEDC_MODE,
        .channel = LCD_BACKLIGHT_LEDC_CHANNEL,
        .intr_type = LEDC_INTR_DISABLE,
        .timer_sel = LCD_BACKLIGHT_LEDC_TIMER,
        .duty = lcd_backlight_percent_to_duty(s_backlight_percent),
        .hpoint = 0,
    };
    ESP_ERROR_CHECK(ledc_channel_config(&channel_config));

    s_backlight_pwm_inited = true;
}

/**
 * @brief 获取 ST7789 面板 IO 句柄。
 *
 * @return 面板 IO 句柄；面板未初始化时返回 NULL。
 */
esp_lcd_panel_io_handle_t lcd_panel_get_io_handle(void)
{
    return s_lcd_io_handle;
}

/**
 * @brief 初始化 SPI 总线、ST7789 面板和背光。
 *
 * @return 初始化完成的面板句柄。
 */
esp_lcd_panel_handle_t lcd_panel_init(void)
{
    spi_bus_config_t bus_cfg = {
        .sclk_io_num = PIN_NUM_SCLK,
        .mosi_io_num = PIN_NUM_MOSI,
        .miso_io_num = PIN_NUM_MISO,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,

        .max_transfer_sz = LCD_H_RES * 120 * sizeof(uint16_t),
    };
    ESP_ERROR_CHECK(spi_bus_initialize(LCD_HOST, &bus_cfg, SPI_DMA_CH_AUTO));

    esp_lcd_panel_io_spi_config_t io_config = {
        .dc_gpio_num = PIN_NUM_DC,
        .cs_gpio_num = PIN_NUM_CS,
        .pclk_hz = LCD_PIXEL_CLOCK_HZ,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
        .spi_mode = 3,

        /* 允许双缓冲场景下存在两笔待处理传输。 */
        .trans_queue_depth = 2,
    };

    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)LCD_HOST,
                                             &io_config,
                                             &s_lcd_io_handle));

    esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = PIN_NUM_RST,
        .color_space = ESP_LCD_COLOR_SPACE_RGB,
        .bits_per_pixel = 16,
    };

    esp_lcd_panel_handle_t panel_handle = NULL;
    ESP_ERROR_CHECK(esp_lcd_new_panel_st7789(s_lcd_io_handle,
                                             &panel_config,
                                             &panel_handle));
    s_lcd_panel_handle = panel_handle;

    ESP_ERROR_CHECK(esp_lcd_panel_reset(panel_handle));
    ESP_ERROR_CHECK(esp_lcd_panel_init(panel_handle));

    ESP_ERROR_CHECK(esp_lcd_panel_set_gap(panel_handle, 0, 0));
    ESP_ERROR_CHECK(esp_lcd_panel_swap_xy(panel_handle, false));
    ESP_ERROR_CHECK(esp_lcd_panel_mirror(panel_handle, false, false));
    ESP_ERROR_CHECK(esp_lcd_panel_invert_color(panel_handle, true));
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel_handle, true));

    lcd_backlight_load_saved_percent();
    lcd_backlight_pwm_init();
    lcd_apply_backlight_percent(s_backlight_percent, false);

    ESP_LOGI(TAG, "ST7789 panel init done");
    return panel_handle;
}

/**
 * @brief 设置背光亮度并保存非零值。
 *
 * @param percent UI 亮度百分比，0 表示关闭背光。
 */
void lcd_set_backlight_percent(uint8_t percent)
{
    lcd_apply_backlight_percent(percent, true);
}

/**
 * @brief 获取当前或最近一次非零背光亮度。
 *
 * @return UI 亮度百分比。
 */
uint8_t lcd_get_backlight_percent(void)
{
    if(s_backlight_percent > 0) {
        return s_backlight_percent;
    }

    return s_backlight_last_nonzero_percent;
}

/**
 * @brief 打开或关闭背光。
 *
 * @param on true 时恢复最近一次非零亮度，false 时关闭背光。
 */
void lcd_set_backlight(bool on)
{
    if(on) {
        lcd_apply_backlight_percent(s_backlight_last_nonzero_percent, false);
    }
    else {
        uint8_t keep_percent = s_backlight_last_nonzero_percent;

        /* 保持 LEDC 通道运行，仅将占空比设为 0。 */
        lcd_apply_backlight_percent(0, false);
        s_backlight_last_nonzero_percent = keep_percent;
    }
}

/**
 * @brief 设置面板休眠状态。
 *
 * @param sleep true 时休眠，false 时唤醒。
 */
void lcd_panel_sleep(bool sleep)
{
    if(s_lcd_io_handle == NULL) {
        lcd_set_backlight(!sleep);
        return;
    }

#if LCD_PANEL_BACKLIGHT_ONLY_SLEEP
    /* 仅控制背光，保持面板控制器和 GRAM 运行。 */
    lcd_set_backlight(!sleep);
    return;
#endif

    if(sleep) {
        /* 先关闭背光，避免面板状态切换时闪烁。 */
        lcd_set_backlight(false);

        if(s_lcd_panel_sleeping) {
            return;
        }

        if(s_lcd_panel_handle != NULL) {
            (void)esp_lcd_panel_disp_on_off(s_lcd_panel_handle, false);
        }

        /* 补发 DISPOFF，确保面板状态与驱动状态一致。 */
        (void)esp_lcd_panel_io_tx_param(s_lcd_io_handle, ST7789_CMD_DISPOFF, NULL, 0);
        vTaskDelay(pdMS_TO_TICKS(30));

#if LCD_PANEL_USE_SLEEP_IN
        (void)esp_lcd_panel_io_tx_param(s_lcd_io_handle, ST7789_CMD_SLPIN, NULL, 0);
        vTaskDelay(pdMS_TO_TICKS(150));
#endif

        s_lcd_panel_sleeping = true;
    }
    else {
        /* 唤醒时始终补发 DISPON，以同步面板实际状态。 */
#if LCD_PANEL_USE_SLEEP_IN
        if(s_lcd_panel_sleeping) {
            (void)esp_lcd_panel_io_tx_param(s_lcd_io_handle, ST7789_CMD_SLPOUT, NULL, 0);
            vTaskDelay(pdMS_TO_TICKS(180));
        }
#else
        vTaskDelay(pdMS_TO_TICKS(20));
#endif

        if(s_lcd_panel_handle != NULL) {
            (void)esp_lcd_panel_disp_on_off(s_lcd_panel_handle, true);
        }

        /* 补发 DISPON，确保显示输出已开启。 */
        (void)esp_lcd_panel_io_tx_param(s_lcd_io_handle, ST7789_CMD_DISPON, NULL, 0);
        vTaskDelay(pdMS_TO_TICKS(50));

        s_lcd_panel_sleeping = false;
        lcd_set_backlight(true);
    }
}
