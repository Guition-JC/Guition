#pragma once

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*app_video_player_view_toast_cb_t)(const char *message);

/**
 * @brief 在指定容器中创建视频播放器页面。
 * @param parent 播放页面的父对象。
 * @param toast_cb 显示提示信息的回调。
 */
void app_video_player_view_build(lv_obj_t *parent, app_video_player_view_toast_cb_t toast_cb);

/**
 * @brief 停止刷新并释放页面持有的解码缓冲区。
 */
void app_video_player_view_cleanup(void);

#ifdef __cplusplus
}
#endif
