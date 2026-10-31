/**
 * SPDX-License-Identifier: GPL-2.0-only
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief 启动视频最长等待时间，单位为毫秒。 */
#define DESKTOP_BOOT_VIDEO_TIMEOUT_MS 8000U

/**
 * @brief 在显示初始化后播放内置启动视频，并在首帧显示时打开背光。 函数自行获取显示锁，调用方不得持有该锁。
 * @return 启动成功返回 ESP_OK，否则返回错误码。
 */
esp_err_t desktop_boot_video_start(void);

/**
 * @brief 最多等待 timeout_ms 毫秒，然后移除启动视频并显示桌面。 启动失败时也可调用，返回前确保背光开启。
 * @param timeout_ms 等待视频的最长时间，单位为毫秒。
 */
void desktop_boot_video_finish(uint32_t timeout_ms);

/**
 * @brief 查询启动视频是否仍在播放。
 * @return 视频仍在播放时返回 true。
 */
bool desktop_boot_video_is_playing(void);

/**
 * @brief 查询桌面渲染是否仍需等待；视频结束前提前允许桌面绘制。
 * @return 桌面渲染仍需等待时返回 true。
 */
bool desktop_boot_video_covers_desktop(void);

/**
 * @brief 调用方须持有显示锁；临时释放并重新获取锁以驱动视频刷新。 无启动视频时不执行操作。
 */
void desktop_boot_video_yield(void);

#ifdef __cplusplus
}
#endif
