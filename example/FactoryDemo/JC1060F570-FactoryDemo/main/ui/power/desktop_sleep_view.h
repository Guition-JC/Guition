/**
 * SPDX-License-Identifier: GPL-2.0-only
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 提供当前本地时分秒。
 * @param context 时间回调上下文。
 * @param[out] hour 接收小时。
 * @param[out] minute 接收分钟。
 * @param[out] second 接收秒。
 * @return 时间可用时返回 true；时钟未就绪时返回 false。
 */
typedef bool (*desktop_sleep_view_time_cb_t)(void *context, uint8_t *hour, uint8_t *minute,
                                            uint8_t *second);

/**
 * @brief 创建待机页面；页面已存在时忽略重复创建请求。
 * @param parent 待机页面的父对象。
 * @param screen_width 屏幕宽度，单位为像素。
 * @param screen_height 屏幕高度，单位为像素。
 * @param time_cb 获取当前时间的回调。
 * @param time_context 传给时间回调的上下文。
 */
void desktop_sleep_view_build(lv_obj_t *parent, uint16_t screen_width, uint16_t screen_height,
                            desktop_sleep_view_time_cb_t time_cb, void *time_context);

/**
 * @brief 时钟模式下刷新本地时间，包含秒值。
 */
void desktop_sleep_view_refresh(void);

/**
 * @brief 删除待机页面及其资源；未创建时也可调用。
 */
void desktop_sleep_view_destroy(void);

/**
 * @brief 查询待机页面是否已创建。
 * @return 页面已创建时返回 true。
 */
bool desktop_sleep_view_is_built(void);

#ifdef __cplusplus
}
#endif
