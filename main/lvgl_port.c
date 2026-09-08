/**
 * @file lvgl_port.c
 * @brief LVGL 显示刷新与 ST7789 面板适配实现。
 */
/*
 * ==================== 模块说明 ====================
 *  * 模块职责：
 * - 负责把 LVGL 的 draw buffer 与 ESP-IDF LCD 面板刷新接口连接起来，是 UI 与物理屏幕之间的适配层。
 * - LVGL 只关心待刷新区域 area 和像素缓冲区 px_map，本文件把这些数据提交给 esp_lcd_panel_draw_bitmap()。
 * - 刷新完成不是立刻通知 LVGL，而是在 LCD DMA/传输完成回调中调用 lv_display_flush_ready()，避免缓冲区被过早复用。
 * - 双缓冲使用 heap_caps_malloc() 分配，优先满足 DMA 可访问内存要求。
 *
 * 阅读建议：
 * - 先看本文件顶部的宏定义和静态状态变量，理解硬件参数和运行状态。
 * - 再看 reset/init/start/on_key/destroy 等对外函数，理解页面或驱动的生命周期。
 * - 最后看 static 辅助函数，了解具体寄存器读写、UI 刷新或状态机细节。
 * =======================================================
 */


#include "lvgl_port.h"

#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_io.h"
#include "esp_heap_caps.h"
#include "esp_log.h"

#define DISP_HOR_RES  240
#define DISP_VER_RES  240


#define DISP_BUF_LINES      40
#define DISP_BUF_PIXELS     (DISP_HOR_RES * DISP_BUF_LINES)

static const char *TAG = "LVGL_PORT";

/* LVGL display 对象，全局保存便于端口层管理。 */
static lv_display_t *disp;
/* 底层 LCD 面板句柄，刷新回调会使用它提交像素数据。 */
static esp_lcd_panel_handle_t lcd_panel_handle;

/* LVGL 第一块绘图缓冲区。 */
static void *s_buf1 = NULL;
/* LVGL 第二块绘图缓冲区，用于双缓冲减少刷新等待。 */
static void *s_buf2 = NULL;
/* 单块 LVGL 绘图缓冲区字节数。 */
static size_t s_buf_size_bytes = 0;

/**
 * @brief LCD 颜色数据传输完成回调。
 *
 * 详细说明：
 * - 在 DMA/SPI 刷新真正结束后通知 LVGL 当前 draw buffer 可复用。
 * - 返回 false 表示不需要唤醒更高优先级任务。
 *
 * @param panel_io 输入或输出参数，具体含义见函数内部使用方式。
 * @param edata 输入或输出参数，具体含义见函数内部使用方式。
 * @param user_ctx 输入或输出参数，具体含义见函数内部使用方式。
 */
static bool lcd_flush_done_cb(esp_lcd_panel_io_handle_t panel_io,
                              esp_lcd_panel_io_event_data_t *edata,
                              void *user_ctx)
{
    (void)panel_io;
    (void)edata;

    lv_display_t *display = (lv_display_t *)user_ctx;

    /* DMA 传输完成后再释放 LVGL draw buffer。 */
    lv_display_flush_ready(display);

    return false;
}

/**
 * @brief LVGL 刷新回调，将区域像素提交给 LCD 面板。
 *
 * 详细说明：
 * - 把 LVGL 给出的 area 坐标转换为 esp_lcd_panel_draw_bitmap() 的右/下开区间坐标。
 * - 不在这里调用 flush_ready，避免异步传输尚未完成时复用缓冲区。
 *
 * @param display 输入或输出参数，具体含义见函数内部使用方式。
 * @param area 输入或输出参数，具体含义见函数内部使用方式。
 * @param px_map 输入或输出参数，具体含义见函数内部使用方式。
 */
static void lcd_flush_cb(lv_display_t *display,
                         const lv_area_t *area,
                         uint8_t *px_map)
{
    (void)display;

    esp_lcd_panel_draw_bitmap(lcd_panel_handle,
                              area->x1,
                              area->y1,
                              area->x2 + 1,
                              area->y2 + 1,
                              px_map);

    /* flush_ready 由传输完成回调触发。 */
}

/**
 * @brief 初始化 LVGL 显示端口。
 *
 * 详细说明：
 * - 创建 LVGL display，注册 flush 回调并分配双缓冲。
 * - 把 LCD 面板句柄保存到静态变量，供刷新回调使用。
 *
 * @param lcd_panel 输入或输出参数，具体含义见函数内部使用方式。
 */
void lvgl_port_init(esp_lcd_panel_handle_t lcd_panel)
{
    lcd_panel_handle = lcd_panel;

    lv_init();

    disp = lv_display_create(DISP_HOR_RES, DISP_VER_RES);
    lv_display_set_flush_cb(disp, lcd_flush_cb);
    lv_display_set_color_format(disp, LV_COLOR_FORMAT_RGB565_SWAPPED);

    s_buf_size_bytes = DISP_BUF_PIXELS * sizeof(uint16_t);

    s_buf1 = heap_caps_malloc(s_buf_size_bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
    s_buf2 = heap_caps_malloc(s_buf_size_bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);

    if(s_buf1 == NULL || s_buf2 == NULL) {
        ESP_LOGW(TAG, "Internal DMA buffer alloc failed, try MALLOC_CAP_DMA only");

        if(s_buf1) {
            heap_caps_free(s_buf1);
            s_buf1 = NULL;
        }

        if(s_buf2) {
            heap_caps_free(s_buf2);
            s_buf2 = NULL;
        }

        s_buf1 = heap_caps_malloc(s_buf_size_bytes, MALLOC_CAP_DMA);
        s_buf2 = heap_caps_malloc(s_buf_size_bytes, MALLOC_CAP_DMA);
    }

    if(s_buf1 == NULL || s_buf2 == NULL) {
        ESP_LOGE(TAG, "Failed to allocate LVGL draw buffers");
        abort();
    }

    ESP_LOGI(TAG,
             "LVGL partial double buffer: %d lines, %u bytes each",
             DISP_BUF_LINES,
             (unsigned int)s_buf_size_bytes);

    lv_display_set_buffers(disp,
                           s_buf1,
                           s_buf2,
                           s_buf_size_bytes,
                           LV_DISPLAY_RENDER_MODE_PARTIAL);

    esp_lcd_panel_io_handle_t io_handle = lcd_panel_get_io_handle();

    const esp_lcd_panel_io_callbacks_t cbs = {
        .on_color_trans_done = lcd_flush_done_cb,
    };

    ESP_ERROR_CHECK(esp_lcd_panel_io_register_event_callbacks(io_handle,
                                                              &cbs,
                                                              disp));
}