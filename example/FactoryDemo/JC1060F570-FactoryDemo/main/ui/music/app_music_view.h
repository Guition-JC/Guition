#pragma once

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*app_music_view_toast_cb_t)(const char *message);

/**
 * @brief 在指定容器中创建音乐页面。
 * @param parent 页面父容器。
 * @param toast_cb 用于显示提示消息的回调。
 */
void app_music_view_build(lv_obj_t *parent, app_music_view_toast_cb_t toast_cb);

/**
 * @brief 停止定时器和动画，使未完成的回调失效。
 */
void app_music_view_cleanup(void);

#ifdef __cplusplus
}
#endif
