/**
 * SPDX-License-Identifier: GPL-2.0-only
 */
#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Maximum stored path length, including the null terminator. */
#define APP_CLOCK_ASSET_PATH_MAX 160U

/** @brief Maximum entries examined in one directory scan. */
#define APP_CLOCK_ASSET_MAX_ENTRIES 64U

typedef struct {
    bool complete; /**< True when the dial, hour hand and minute hand are present. */

    bool has_second; /**< True when an optional second hand is present. */
    char dial[APP_CLOCK_ASSET_PATH_MAX];
    char hour[APP_CLOCK_ASSET_PATH_MAX];
    char minute[APP_CLOCK_ASSET_PATH_MAX];
    char second[APP_CLOCK_ASSET_PATH_MAX];
} app_clock_asset_set_t;

/**
 * @brief Scan POSIX files and store LVGL paths; match names case-insensitively and prefer PNG.
 * @param posix_root Directory to scan using POSIX file APIs.
 * @param lv_root Corresponding root used to construct LVGL paths.
 * @param[out] set Receives discovered paths; complete reports usability.
 * @return false for invalid arguments or an unreadable directory; true if scanning succeeds.
 */
bool app_clock_assets_scan(const char *posix_root, const char *lv_root,
                             app_clock_asset_set_t *set);

#ifdef __cplusplus
}
#endif
