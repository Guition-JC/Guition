/** SPDX-License-Identifier: GPL-2.0-only */
#define _POSIX_C_SOURCE 200809L

#include "wallpaper_store.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

#if !defined(WALLPAPER_STORE_HOST_TEST)

#include "esp_log.h"
#include "nvs.h"

#define TAG "wallpaper_store"
#define NVS_NAMESPACE "photo_album"
#define NVS_KEY_WALLPAPER "wallpaper"

#endif

static char lower_ascii(char value)
{
    return (char)tolower((unsigned char)value);
}

static bool extension_is_image(const char *extension)
{
    static const char *const allowed[] = {"jpg", "jpeg", "png", "sjpg", "spng"};
    if (extension == NULL || extension[0] == '\0') {
        return false;
    }
    for (size_t i = 0U; i < sizeof(allowed) / sizeof(allowed[0]); ++i) {
        const char *expected = allowed[i];
        size_t index = 0U;
        while (extension[index] != '\0' && expected[index] != '\0' &&
               lower_ascii(extension[index]) == expected[index]) {
            ++index;
        }
        if (extension[index] == '\0' && expected[index] == '\0') {
            return true;
        }
    }
    return false;
}

/**
 * @brief Reject any ".." path component without rejecting names such as "..jpg".
 * @param path Path to inspect.
 * @return true if the path contains a parent component.
 */
static bool has_parent_component(const char *path)
{
    const char *cursor = path;
    while (*cursor != '\0') {
        while (*cursor == '/') {
            ++cursor;
        }
        const char *start = cursor;
        while (*cursor != '\0' && *cursor != '/') {
            ++cursor;
        }
        const size_t length = (size_t)(cursor - start);
        if (length == 2U && start[0] == '.' && start[1] == '.') {
            return true;
        }
    }
    return false;
}

bool wallpaper_store_path_is_valid(const char *path)
{
    if (path == NULL) {
        return false;
    }

    const size_t length = strnlen(path, WALLPAPER_PATH_MAX + 1U);
    if (length == 0U || length >= WALLPAPER_PATH_MAX) {
        return false;
    }

    const bool lv_absolute = length >= 3U && path[0] == 'A' && path[1] == ':' && path[2] == '/';
    if (!lv_absolute && path[0] != '/') {
        return false;
    }

    const char *body = lv_absolute ? path + 3 : path + 1;
    if (body[0] == '\0') {
        return false;
    }

    for (const char *cursor = body; *cursor != '\0'; ++cursor) {
        const unsigned char ch = (unsigned char)*cursor;
        if (ch < 0x20U || ch == 0x7FU || ch == '\\' || ch == ':') {
            return false;
        }
        if (cursor[0] == '/' && cursor[1] == '/') {
            return false;
        }
    }
    if (has_parent_component(body)) {
        return false;
    }

    const char *dot = strrchr(body, '.');
    if (dot == NULL || dot == body || dot[1] == '\0') {
        return false;
    }
    return extension_is_image(dot + 1);
}

bool wallpaper_store_posix_path(const char *path, char *out, size_t capacity)
{
    if (!wallpaper_store_path_is_valid(path) || out == NULL || capacity == 0U) {
        return false;
    }

    const char *body = path;
    if (path[0] == 'A' && path[1] == ':' && path[2] == '/') {
        body = path + 2;
    }
    const int written = snprintf(out, capacity, "%s", body);
    if (written < 0 || (size_t)written >= capacity) {
        out[0] = '\0';
        return false;
    }
    return true;
}

#if !defined(WALLPAPER_STORE_HOST_TEST)

esp_err_t wallpaper_store_load(char *path_out, size_t capacity, bool *found)
{
    if (path_out == NULL || capacity == 0U || found == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(path_out, 0, capacity);
    *found = false;

    nvs_handle_t handle;
    esp_err_t ret = nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle);
    if (ret == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_OK;
    }
    if (ret != ESP_OK) {
        return ret;
    }

    size_t required = 0U;
    ret = nvs_get_str(handle, NVS_KEY_WALLPAPER, NULL, &required);
    if (ret == ESP_ERR_NVS_NOT_FOUND) {
        ret = ESP_OK;
        goto cleanup;
    }
    if (ret != ESP_OK) {
        goto cleanup;
    }
    if (required == 0U || required > capacity || required > WALLPAPER_PATH_MAX) {
        ESP_LOGW(TAG, "stored wallpaper length %u rejected", (unsigned)required);
        ret = ESP_ERR_INVALID_RESPONSE;
        goto cleanup;
    }

    ret = nvs_get_str(handle, NVS_KEY_WALLPAPER, path_out, &required);
    if (ret != ESP_OK) {
        goto cleanup;
    }
    if (!wallpaper_store_path_is_valid(path_out)) {
        ESP_LOGW(TAG, "stored wallpaper rejected: %s", path_out);
        ret = ESP_ERR_INVALID_RESPONSE;
        goto cleanup;
    }
    *found = true;

cleanup:
    nvs_close(handle);
    if (ret != ESP_OK) {
        memset(path_out, 0, capacity);
        *found = false;
    }
    return ret;
}

esp_err_t wallpaper_store_save(const char *path)
{
    if (!wallpaper_store_path_is_valid(path)) {
        ESP_LOGW(TAG, "refusing to store invalid wallpaper path (max %u bytes)",
                 (unsigned)WALLPAPER_PATH_MAX);
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t handle;
    esp_err_t ret = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = nvs_set_str(handle, NVS_KEY_WALLPAPER, path);
    if (ret == ESP_OK) {
        ret = nvs_commit(handle);
    }
    nvs_close(handle);

    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "storing wallpaper failed: %s", esp_err_to_name(ret));
    }
    return ret;
}

esp_err_t wallpaper_store_clear(void)
{
    nvs_handle_t handle;
    esp_err_t ret = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (ret == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_OK;
    }
    if (ret != ESP_OK) {
        return ret;
    }

    ret = nvs_erase_key(handle, NVS_KEY_WALLPAPER);
    if (ret == ESP_ERR_NVS_NOT_FOUND) {
        ret = ESP_OK;
    }
    if (ret == ESP_OK) {
        ret = nvs_commit(handle);
    }
    nvs_close(handle);
    return ret;
}

#endif
