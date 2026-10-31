/** SPDX-License-Identifier: GPL-2.0-only */
#pragma once

#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#define STANDBY_MAX_PHOTOS 64U
#define STANDBY_MEDIA_NAME_MAX 96U

/** @brief Standby presentation mode. */
typedef enum {
    STANDBY_CLOCK = 0,
    STANDBY_PHOTOS = 1,
    STANDBY_VIDEO = 2,
} standby_mode_t;

/** @brief Selected standby media source and filename. */
typedef struct {
    uint8_t source; /**< 0 = SD, 1 = built-in */
    char name[STANDBY_MEDIA_NAME_MAX];
} standby_media_t;

/** @brief Persistent standby mode and media selection. */
typedef struct {
    standby_mode_t mode;
    uint16_t photo_interval_seconds;
    uint8_t photo_count;
    standby_media_t photos[STANDBY_MAX_PHOTOS];
    standby_media_t video;
} standby_prefs_t;

/**
 * @brief Restore standby preferences from NVS or use defaults.
 */
void standby_prefs_restore(void);
/**
 * @brief Borrow the current standby preferences.
 * @return Borrowed current standby preferences.
 */
const standby_prefs_t *standby_prefs_get(void);
/**
 * @brief Validate and persist standby preferences.
 * @param prefs Preferences.
 * @return ESP_OK on success, or an error code on failure.
 */
esp_err_t standby_prefs_save(const standby_prefs_t *prefs);
