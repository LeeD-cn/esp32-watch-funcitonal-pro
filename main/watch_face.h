/**
 * @file watch_face.h
 * @brief 主表盘页面接口。
 */
#ifndef WATCH_FACE_H
#define WATCH_FACE_H

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 在当前活动屏幕上创建主表盘页面。
 */
void watch_face_create(void);
/**
 * @brief 在指定父对象上创建主表盘页面。
 *
 * @param parent LVGL 父对象。
 * @return 页面根对象，失败时返回 NULL。
 */
lv_obj_t *watch_face_create_on(lv_obj_t *parent);
/**
 * @brief 设置表盘页面是否处于活动刷新状态。
 *
 * @param active true 启用时间、电量等刷新；false 暂停刷新。
 */
void watch_face_set_active(bool active);
/**
 * @brief 销毁主表盘页面和相关定时器。
 */
void watch_face_destroy(void);

#ifdef __cplusplus
}
#endif

#endif