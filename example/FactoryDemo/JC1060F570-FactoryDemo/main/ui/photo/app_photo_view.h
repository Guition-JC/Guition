/**
 * SPDX-License-Identifier: GPL-2.0-only
 */
#pragma once

#include <stdbool.h>

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*app_photo_toast_cb_t)(const char *message);

/**
 * @brief 创建相册页面，同一时间仅保留当前照片的解码缓存。 调用方须持有 LVGL 适配器锁。
 * @param parent 页面父容器。
 * @param toast_cb 用于显示提示消息的回调。
 * @return 页面创建成功时返回 true，否则返回 false。
 */
bool app_photo_view_build(lv_obj_t *parent, app_photo_toast_cb_t toast_cb);
/**
 * @brief 释放页面资源；调用方须持有 LVGL 适配器锁。
 */
void app_photo_view_cleanup(void);

#ifdef __cplusplus
}
#endif
