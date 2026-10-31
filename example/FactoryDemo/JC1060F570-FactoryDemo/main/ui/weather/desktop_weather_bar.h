#pragma once

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 创建系统栏天气区域；调用方须持有 LVGL 适配器锁。
 * @param system_bar 天气区域的父对象。
 */
void desktop_weather_bar_build(lv_obj_t *system_bar);

/**
 * @brief 清空控件引用，在重建界面树前调用。
 */
void desktop_weather_bar_reset(void);

/**
 * @brief 注册天气服务监听器；服务就绪后可重复调用。
 */
void desktop_weather_bar_register(void);

/**
 * @brief 在 LVGL 任务中应用尚未完成的天气更新。
 */
void desktop_weather_bar_tick(void);

#ifdef __cplusplus
}
#endif
