/** SPDX-License-Identifier: GPL-2.0-only */
#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "photo_catalog.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Upper bound of one directory scan; extra entries are ignored. */
#define PHOTO_SOURCE_MAX_DIR_ENTRIES 64U

/** @brief Photo roots: the SD card directory and the SPIFFS image directory. */
#define PHOTO_SOURCE_SD_ROOT "/sdcard/image"
#define PHOTO_SOURCE_SD_LV_ROOT "A:/sdcard/image"
#define PHOTO_SOURCE_BUILTIN_ROOT "/assets/image"
#define PHOTO_SOURCE_BUILTIN_LV_ROOT "A:/assets/image"

/** @brief Bounded directory scan result. */
typedef struct {
    photo_dir_entry_t entries[PHOTO_SOURCE_MAX_DIR_ENTRIES];
    size_t count;
} photo_source_list_t;

/**
 * @brief Scan regular files with safe names; the catalog filters extensions.
 * @return true when the directory was opened, false for invalid or unavailable roots.
 */
bool photo_source_scan(const char *posix_root, photo_source_list_t *list);

#ifdef __cplusplus
}
#endif
