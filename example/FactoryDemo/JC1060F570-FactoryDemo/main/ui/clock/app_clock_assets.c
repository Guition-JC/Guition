/**
 * SPDX-License-Identifier: GPL-2.0-only
 */
#define _POSIX_C_SOURCE 200809L

#include "app_clock_assets.h"

#include <dirent.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <sys/stat.h>

#include "esp_log.h"

#define TAG "clock_assets"

typedef enum {
    CLOCK_ASSET_DIAL = 0,
    CLOCK_ASSET_HOUR,
    CLOCK_ASSET_MINUTE,
    CLOCK_ASSET_SECOND,
    CLOCK_ASSET_SLOT_COUNT,
} clock_asset_slot_t;

#define CLOCK_ASSET_RANK_NONE 99

/**
 * @brief Reject path separators and unsupported filename characters.
 * @param name File name to check.
 * @return true if the name contains only supported characters and no path separators.
 */
static bool name_is_safe(const char *name)
{
    if (name == NULL || name[0] == '\0' || name[0] == '.') {
        return false;
    }
    const size_t length = strlen(name);
    if (length >= (size_t)APP_CLOCK_ASSET_PATH_MAX) {
        return false;
    }
    for (size_t index = 0; index < length; ++index) {
        const unsigned char ch = (unsigned char)name[index];
        if (ch < 0x20U || ch == 0x7FU || ch == '\\' || ch == '/' || ch == ':') {
            return false;
        }
    }
    return true;
}

static bool equals_ignore_case(const char *left, size_t left_length, const char *right)
{
    size_t index = 0U;
    for (; index < left_length && right[index] != '\0'; ++index) {
        unsigned char a = (unsigned char)left[index];
        unsigned char b = (unsigned char)right[index];
        if (a >= 'A' && a <= 'Z') {
            a = (unsigned char)(a - 'A' + 'a');
        }
        if (b >= 'A' && b <= 'Z') {
            b = (unsigned char)(b - 'A' + 'a');
        }
        if (a != b) {
            return false;
        }
    }
    return index == left_length && right[index] == '\0';
}

/**
 * @brief Rank supported image extensions with PNG ahead of JPEG and JPG.
 * @param extension Extension text to compare.
 * @param length Length of extension in bytes.
 * @return 0 for PNG, 1 for JPEG, or CLOCK_ASSET_RANK_NONE for unsupported extensions.
 */
static int32_t extension_rank(const char *extension, size_t length)
{
    if (equals_ignore_case(extension, length, "png")) {
        return 0;
    }
    if (equals_ignore_case(extension, length, "jpg") ||
        equals_ignore_case(extension, length, "jpeg")) {
        return 1;
    }
    return CLOCK_ASSET_RANK_NONE;
}

static clock_asset_slot_t slot_for_base(const char *base, size_t length, bool *matched)
{
    *matched = true;
    if (equals_ignore_case(base, length, "dial")) {
        return CLOCK_ASSET_DIAL;
    }
    if (equals_ignore_case(base, length, "hour")) {
        return CLOCK_ASSET_HOUR;
    }
    if (equals_ignore_case(base, length, "minute")) {
        return CLOCK_ASSET_MINUTE;
    }
    if (equals_ignore_case(base, length, "second")) {
        return CLOCK_ASSET_SECOND;
    }
    *matched = false;
    return CLOCK_ASSET_DIAL;
}

static char *slot_buffer(app_clock_asset_set_t *set, clock_asset_slot_t slot)
{
    switch (slot) {
    case CLOCK_ASSET_DIAL:
        return set->dial;
    case CLOCK_ASSET_HOUR:
        return set->hour;
    case CLOCK_ASSET_MINUTE:
        return set->minute;
    default:
        return set->second;
    }
}

/**
 * @brief Scan a bounded directory and resolve a complete dial/hour/minute asset set.
 * @param posix_root Directory to scan using POSIX file APIs.
 * @param lv_root Corresponding root used to construct LVGL paths.
 * @param[out] set Receives the discovered paths and completeness flags.
 * @return false for invalid arguments or an unreadable directory; true if scanning succeeds.
 */
bool app_clock_assets_scan(const char *posix_root, const char *lv_root,
                             app_clock_asset_set_t *set)
{
    if (posix_root == NULL || lv_root == NULL || set == NULL || posix_root[0] != '/') {
        return false;
    }

    memset(set, 0, sizeof(*set));

    DIR *directory = opendir(posix_root);
    if (directory == NULL) {
        ESP_LOGI(TAG, "no clock face directory at %s; the drawn face will be used",
                 posix_root);
        return false;
    }

    int32_t rank[CLOCK_ASSET_SLOT_COUNT];
    for (size_t index = 0; index < (size_t)CLOCK_ASSET_SLOT_COUNT; ++index) {
        rank[index] = CLOCK_ASSET_RANK_NONE;
    }

    size_t scanned = 0U;
    struct dirent *item = NULL;
    while ((item = readdir(directory)) != NULL && scanned < APP_CLOCK_ASSET_MAX_ENTRIES) {
        ++scanned;
        if (!name_is_safe(item->d_name)) {
            continue;
        }

        const char *dot = strrchr(item->d_name, '.');
        if (dot == NULL || dot == item->d_name || dot[1] == '\0') {
            continue;
        }
        const char *extension = dot + 1;
        const int32_t candidate_rank = extension_rank(extension, strlen(extension));
        if (candidate_rank >= CLOCK_ASSET_RANK_NONE) {
            continue;
        }

        bool matched = false;
        const clock_asset_slot_t slot =
            slot_for_base(item->d_name, (size_t)(dot - item->d_name), &matched);
        if (!matched || candidate_rank >= rank[(size_t)slot]) {
            continue;
        }

        char path[APP_CLOCK_ASSET_PATH_MAX];
        const int written = snprintf(path, sizeof(path), "%s/%s", posix_root, item->d_name);
        if (written < 0 || (size_t)written >= sizeof(path)) {
            ESP_LOGW(TAG, "path too long under %s", posix_root);
            continue;
        }
        struct stat info;
        if (stat(path, &info) != 0 || !S_ISREG(info.st_mode)) {
            continue;
        }

        char lv_path[APP_CLOCK_ASSET_PATH_MAX];
        const int copied = snprintf(lv_path, sizeof(lv_path), "%s/%s", lv_root, item->d_name);
        if (copied < 0 || (size_t)copied >= sizeof(lv_path)) {
            ESP_LOGW(TAG, "LVGL path too long for %s", item->d_name);
            continue;
        }
        memcpy(slot_buffer(set, slot), lv_path, (size_t)copied + 1U);
        rank[(size_t)slot] = candidate_rank;
    }

    closedir(directory);

    set->has_second = rank[CLOCK_ASSET_SECOND] < CLOCK_ASSET_RANK_NONE;
    set->complete = rank[CLOCK_ASSET_DIAL] < CLOCK_ASSET_RANK_NONE &&
                    rank[CLOCK_ASSET_HOUR] < CLOCK_ASSET_RANK_NONE &&
                    rank[CLOCK_ASSET_MINUTE] < CLOCK_ASSET_RANK_NONE;
    ESP_LOGI(TAG, "scanned %s: complete=%d second=%d", posix_root, (int)set->complete,
             (int)set->has_second);
    return true;
}
