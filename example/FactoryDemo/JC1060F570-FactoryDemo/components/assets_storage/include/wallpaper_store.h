/** SPDX-License-Identifier: GPL-2.0-only */
#pragma once

#include <stdbool.h>
#include <stddef.h>

#if !defined(WALLPAPER_STORE_HOST_TEST)
#include "esp_err.h"
#endif

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Maximum wallpaper path storage in bytes, including the terminating NUL. */
#define WALLPAPER_PATH_MAX 256U

/** @brief Default desktop wallpaper, matching the built-in SPIFFS asset set. */
#define WALLPAPER_DEFAULT_PATH "A:/assets/image/2.jpg"

/**
 * @brief True for a bounded, absolute, traversal-free JPEG/PNG/sliced image path.
 * @param path Path to use.
 * @return True for a bounded, absolute, traversal-free JPEG/PNG/sliced image path.
 */
bool wallpaper_store_path_is_valid(const char *path);

/**
 * @brief Convert an accepted "A:/..." LVGL path into its POSIX form for stat().
 * @param path Path to use.
 * @param[out] out Receives the POSIX path.
 * @param capacity Capacity of the destination buffer.
 * @return true if the LVGL path was converted to POSIX form.
 */
bool wallpaper_store_posix_path(const char *path, char *out, size_t capacity);

#if !defined(WALLPAPER_STORE_HOST_TEST)

/**
 * @brief Load the stored wallpaper; missing or invalid records report found=false.
 * @param[out] path_out Receives the stored wallpaper path.
 * @param capacity Capacity of the destination buffer.
 * @param[out] found Receives whether a valid record was found.
 * @return ESP_OK on success, or an error code on failure.
 */
esp_err_t wallpaper_store_load(char *path_out, size_t capacity, bool *found);

/**
 * @brief Persist a validated wallpaper path; invalid input is rejected, never stored.
 * @param path Path to use.
 * @return ESP_OK on success, or an error code on failure.
 */
esp_err_t wallpaper_store_save(const char *path);

/**
 * @brief Erase the stored wallpaper path.
 * @return ESP_OK on success, or an error code on failure.
 */
esp_err_t wallpaper_store_clear(void);

#endif

#ifdef __cplusplus
}
#endif
