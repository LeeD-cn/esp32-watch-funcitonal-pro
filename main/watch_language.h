/**
 * @file watch_language.h
 * @brief 系统语言状态接口。
 */
#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 系统语言类型。
 */
typedef enum {
    WATCH_LANGUAGE_ENGLISH = 0,
    WATCH_LANGUAGE_CHINESE,
} watch_language_t;

/**
 * @brief 获取当前系统语言。
 *
 * @return 当前语言。
 */
watch_language_t watch_language_get(void);
/**
 * @brief 判断当前系统语言是否为中文。
 *
 * @return true 中文；false 英文。
 */
bool watch_language_is_chinese(void);
/**
 * @brief 设置并保存系统语言。
 *
 * @param language 目标语言，非法值会按英文处理。
 */
void watch_language_set(watch_language_t language);


/**
 * @brief 获取语言设置下拉框当前记忆选项。
 *
 * @return 下拉框选中的语言。
 */
watch_language_t watch_language_get_dropdown_option(void);
/**
 * @brief 设置语言下拉框记忆选项。
 *
 * @param language 下拉框选中的语言。
 */
void watch_language_set_dropdown_option(watch_language_t language);

#ifdef __cplusplus
}
#endif
