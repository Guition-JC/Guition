/**
 * SPDX-License-Identifier: GPL-2.0-only
 */
#pragma once

#include <stdbool.h>

#include "esp_err.h"
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Wallpaper operations require the LVGL adapter lock. */

/**
 * @brief Attach the wallpaper below desktop widgets; return false if no image is readable.
 * @param desktop_root Desktop object that owns the wallpaper.
 * @return true if a readable wallpaper was attached.
 */
bool desktop_wallpaper_attach(lv_obj_t *desktop_root);

/**
 * @brief Clear cached pointers before destroying the desktop tree; do not delete its objects.
 */
void desktop_wallpaper_detach(void);

/**
 * @brief Validate, persist and immediately apply the wallpaper path.
 * @param path Wallpaper image path.
 * @return ESP_OK on success, otherwise a validation or persistence error.
 */
esp_err_t desktop_wallpaper_set_from_path(const char *path);

/**
 * @brief Resolve the configured wallpaper again and refresh its image.
 */
void desktop_wallpaper_refresh(void);

#ifdef __cplusplus
}
#endif
