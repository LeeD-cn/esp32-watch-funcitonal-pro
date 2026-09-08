/**
 * @file watch_language(1).c
 * @brief 系统语言选择、下拉选项状态和 NVS 持久化。
 */
/*
 * ==================== 模块说明 ====================
 *  * 模块职责：
 * - 系统语言状态管理，负责中英文切换、下拉选项暂存和 NVS 持久化。
 * - NVS 中保存稳定数值编码，避免以后修改枚举顺序导致旧配置解析错误。
 * - 语言加载采用 lazy load，首次读取时才从 NVS 初始化，减少启动路径依赖。
 * - 下拉选项与当前生效语言分开保存，便于设置页先选择、再确认应用。
 *
 * 阅读建议：
 * - 先看本文件顶部的宏定义和静态状态变量，理解硬件参数和运行状态。
 * - 再看 reset/init/start/on_key/destroy 等对外函数，理解页面或驱动的生命周期。
 * - 最后看 static 辅助函数，了解具体寄存器读写、UI 刷新或状态机细节。
 * =======================================================
 */

#include "watch_language.h"

#include "nvs.h"
#include "nvs_flash.h"

#include <stdbool.h>
#include <stdint.h>

/**
 * @brief 语言设置的 NVS 命名空间和键名。
 */
#define WATCH_LANGUAGE_NVS_NAMESPACE    "watch_lang"
#define WATCH_LANGUAGE_NVS_KEY          "language"


/**
 * @brief NVS 中保存的稳定语言编码，避免依赖枚举顺序。
 */
#define WATCH_LANGUAGE_NVS_ENGLISH      0
#define WATCH_LANGUAGE_NVS_CHINESE      1

/* 当前生效的系统语言。 */
static watch_language_t s_watch_language = WATCH_LANGUAGE_ENGLISH;
/* 设置页下拉框中暂存的语言选择。 */
static watch_language_t s_watch_language_dropdown_option = WATCH_LANGUAGE_ENGLISH;

/* 是否已经从 NVS 加载过语言设置。 */
static bool s_language_loaded = false;
/* 语言模块的 NVS 初始化状态。 */
static bool s_nvs_init_done = false;

/**
 * @brief 规范化语言枚举值。
 *
 * 详细说明：
 * - 非法值统一回退到英文，避免异常配置影响 UI。
 *
 * @param language 输入或输出参数，具体含义见调用处和函数内部使用方式。
 *
 * @return 函数执行结果或计算得到的值，具体语义见返回路径。
 */
static watch_language_t language_normalize(watch_language_t language)
{
    return language == WATCH_LANGUAGE_CHINESE ?
           WATCH_LANGUAGE_CHINESE :
           WATCH_LANGUAGE_ENGLISH;
}

/**
 * @brief 把语言枚举转换成 NVS 稳定编码。
 *
 * 详细说明：
 * - 避免直接保存枚举值带来的兼容性风险。
 *
 * @param language 输入或输出参数，具体含义见调用处和函数内部使用方式。
 *
 * @return 函数执行结果或计算得到的值，具体语义见返回路径。
 */
static uint8_t language_to_nvs_value(watch_language_t language)
{
    return language_normalize(language) == WATCH_LANGUAGE_CHINESE ?
           WATCH_LANGUAGE_NVS_CHINESE :
           WATCH_LANGUAGE_NVS_ENGLISH;
}

/**
 * @brief 把 NVS 编码还原成语言枚举。
 *
 * 详细说明：
 * - 未知值回退到英文。
 *
 * @param value 输入或输出参数，具体含义见调用处和函数内部使用方式。
 *
 * @return 函数执行结果或计算得到的值，具体语义见返回路径。
 */
static watch_language_t language_from_nvs_value(uint8_t value)
{
    return value == WATCH_LANGUAGE_NVS_CHINESE ?
           WATCH_LANGUAGE_CHINESE :
           WATCH_LANGUAGE_ENGLISH;
}

/**
 * @brief 初始化 NVS，必要时按 ESP-IDF 规范重新格式化。
 *
 * 详细说明：
 * - 处理分区无空间或版本不兼容情况。
 */
static bool language_nvs_init(void)
{
    if(s_nvs_init_done) {
        return true;
    }

    esp_err_t err = nvs_flash_init();


    if(err == ESP_ERR_NVS_NO_FREE_PAGES ||
       err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        err = nvs_flash_erase();
        if(err == ESP_OK) {
            err = nvs_flash_init();
        }
    }

    if(err == ESP_OK) {
        s_nvs_init_done = true;
        return true;
    }

    return false;
}

/**
 * @brief 首次访问语言配置时从 NVS 加载。
 *
 * 详细说明：
 * - lazy load，减少启动阶段依赖。
 */
static void language_load_once(void)
{
    if(s_language_loaded) {
        return;
    }

    s_language_loaded = true;

    if(!language_nvs_init()) {
        s_watch_language = WATCH_LANGUAGE_ENGLISH;
        s_watch_language_dropdown_option = s_watch_language;
        return;
    }

    nvs_handle_t handle;
    esp_err_t err = nvs_open(WATCH_LANGUAGE_NVS_NAMESPACE,
                             NVS_READONLY,
                             &handle);
    if(err != ESP_OK) {
        s_watch_language = WATCH_LANGUAGE_ENGLISH;
        s_watch_language_dropdown_option = s_watch_language;
        return;
    }

    uint8_t value = WATCH_LANGUAGE_NVS_ENGLISH;
    err = nvs_get_u8(handle, WATCH_LANGUAGE_NVS_KEY, &value);
    nvs_close(handle);

    if(err == ESP_OK) {
        s_watch_language = language_from_nvs_value(value);
    }
    else {
        s_watch_language = WATCH_LANGUAGE_ENGLISH;
    }

    s_watch_language_dropdown_option = s_watch_language;
}

/**
 * @brief 保存当前语言到 NVS。
 *
 * 详细说明：
 * - 配置页切换语言后调用。
 *
 * @param language 输入或输出参数，具体含义见调用处和函数内部使用方式。
 */
static void language_save(watch_language_t language)
{
    if(!language_nvs_init()) {
        return;
    }

    nvs_handle_t handle;
    esp_err_t err = nvs_open(WATCH_LANGUAGE_NVS_NAMESPACE,
                             NVS_READWRITE,
                             &handle);
    if(err != ESP_OK) {
        return;
    }

    uint8_t value = language_to_nvs_value(language);

    err = nvs_set_u8(handle, WATCH_LANGUAGE_NVS_KEY, value);
    if(err == ESP_OK) {
        (void)nvs_commit(handle);
    }

    nvs_close(handle);
}

/**
 * @brief 获取当前生效语言。
 *
 * 详细说明：
 * - 调用前会确保语言配置已加载。
 *
 * @return 函数执行结果或计算得到的值，具体语义见返回路径。
 */
watch_language_t watch_language_get(void)
{
    language_load_once();
    return s_watch_language;
}

/**
 * @brief 判断当前是否为中文。
 *
 * 详细说明：
 * - UI 模块常用的便捷判断函数。
 *
 * @return 函数执行结果或计算得到的值，具体语义见返回路径。
 */
bool watch_language_is_chinese(void)
{
    return watch_language_get() == WATCH_LANGUAGE_CHINESE;
}

/**
 * @brief 设置当前生效语言。
 *
 * 详细说明：
 * - 会规范化输入并保存到 NVS。
 *
 * @param language 输入或输出参数，具体含义见调用处和函数内部使用方式。
 */
void watch_language_set(watch_language_t language)
{
    language_load_once();

    s_watch_language = language_normalize(language);
    s_watch_language_dropdown_option = s_watch_language;

    language_save(s_watch_language);
}

/**
 * @brief 获取设置页下拉框暂存语言。
 *
 * 详细说明：
 * - 允许用户选择但暂不应用。
 *
 * @return 函数执行结果或计算得到的值，具体语义见返回路径。
 */
watch_language_t watch_language_get_dropdown_option(void)
{
    language_load_once();
    return s_watch_language_dropdown_option;
}

/**
 * @brief 设置下拉框暂存语言。
 *
 * 详细说明：
 * - 不立即改变系统语言，只更新临时选择。
 *
 * @param language 输入或输出参数，具体含义见调用处和函数内部使用方式。
 */
void watch_language_set_dropdown_option(watch_language_t language)
{
    language_load_once();
    s_watch_language_dropdown_option = language_normalize(language);
}