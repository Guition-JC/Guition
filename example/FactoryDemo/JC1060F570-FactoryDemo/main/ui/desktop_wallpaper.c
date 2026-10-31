/**
 * SPDX-License-Identifier: GPL-2.0-only
 */
#define _POSIX_C_SOURCE 200809L

#include "desktop_wallpaper.h"

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "esp_attr.h"
#include "esp_log.h"
#include "wallpaper_store.h"

#define TAG "desktop_wallpaper"

static const char *const s_default_path = WALLPAPER_DEFAULT_PATH;

static lv_obj_t *s_image;
/** @brief Persist a copy of the wallpaper path in NVS. */
EXT_RAM_BSS_ATTR static char s_applied_path[WALLPAPER_PATH_MAX];

static bool path_is_readable(const char *path)
{
    char posix[WALLPAPER_PATH_MAX];
    struct stat info;

    if (!wallpaper_store_posix_path(path, posix, sizeof(posix))) {
        return false;
    }
    return stat(posix, &info) == 0 && S_ISREG(info.st_mode);
}

/**
 * @brief Resolve the wallpaper from the stored path, then the built-in default.
 * @param [out] out Buffer receiving the resolved wallpaper path.
 * @param capacity Buffer capacity in bytes.
 */
static void resolve_path(char *out, size_t capacity)
{
    char stored[WALLPAPER_PATH_MAX];
    bool found = false;
    const esp_err_t load_ret = wallpaper_store_load(stored, sizeof(stored), &found);

    if (load_ret == ESP_OK && found) {
        if (path_is_readable(stored)) {
            snprintf(out, capacity, "%s", stored);
            return;
        }
        ESP_LOGW(TAG, "stored wallpaper is not readable: %s", stored);
    } else if (load_ret != ESP_OK) {
        ESP_LOGW(TAG, "stored wallpaper record rejected: %s", esp_err_to_name(load_ret));
    }

    if (path_is_readable(s_default_path)) {
        snprintf(out, capacity, "%s", s_default_path);
        return;
    }

    ESP_LOGW(TAG, "default wallpaper unavailable: %s", s_default_path);
    out[0] = '\0';
}

void desktop_wallpaper_refresh(void)
{
    if (s_image == NULL) {
        return;
    }

    char path[WALLPAPER_PATH_MAX];
    resolve_path(path, sizeof(path));

    if (path[0] == '\0') {
        lv_image_set_src(s_image, NULL);
        s_applied_path[0] = '\0';
        return;
    }
    if (strcmp(s_applied_path, path) == 0) {
        return;
    }

    lv_image_set_src(s_image, path);
    snprintf(s_applied_path, sizeof(s_applied_path), "%s", path);
    ESP_LOGI(TAG, "wallpaper applied: %s", s_applied_path);
}

bool desktop_wallpaper_attach(lv_obj_t *desktop_root)
{
    s_image = NULL;
    s_applied_path[0] = '\0';

    if (desktop_root == NULL) {
        return false;
    }

    s_image = lv_image_create(desktop_root);
    if (s_image == NULL) {
        ESP_LOGE(TAG, "wallpaper layer allocation failed");
        return false;
    }

    lv_obj_remove_style_all(s_image);
    lv_obj_set_pos(s_image, 0, 0);
    lv_obj_set_size(s_image, lv_obj_get_width(desktop_root), lv_obj_get_height(desktop_root));
    lv_obj_clear_flag(s_image, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    /** Scale the image to cover the panel while preserving its aspect ratio. */
    lv_image_set_inner_align(s_image, LV_IMAGE_ALIGN_COVER);
    lv_obj_move_to_index(s_image, 0);

    desktop_wallpaper_refresh();
    return s_applied_path[0] != '\0';
}

void desktop_wallpaper_detach(void)
{
    /** Clear the cached pointer; the desktop tree owns the image object. */
    s_image = NULL;
    s_applied_path[0] = '\0';
}

esp_err_t desktop_wallpaper_set_from_path(const char *path)
{
    if (!wallpaper_store_path_is_valid(path)) {
        ESP_LOGW(TAG, "rejected wallpaper path (limit %u bytes)", (unsigned)WALLPAPER_PATH_MAX);
        return ESP_ERR_INVALID_ARG;
    }
    if (!path_is_readable(path)) {
        ESP_LOGW(TAG, "wallpaper file is not readable: %s", path);
        return ESP_ERR_NOT_FOUND;
    }

    const esp_err_t ret = wallpaper_store_save(path);
    if (ret != ESP_OK) {
        return ret;
    }

    s_applied_path[0] = '\0';
    desktop_wallpaper_refresh();
    return ESP_OK;
}
