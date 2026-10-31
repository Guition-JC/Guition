/** SPDX-License-Identifier: GPL-2.0-only */
#define _POSIX_C_SOURCE 200809L

#include "photo_source.h"

#include <dirent.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "esp_log.h"

#define TAG "photo_source"

bool photo_source_scan(const char *posix_root, photo_source_list_t *list)
{
    if (posix_root == NULL || posix_root[0] != '/' || list == NULL) {
        ESP_LOGW(TAG, "invalid scan root");
        return false;
    }

    memset(list, 0, sizeof(*list));

    DIR *directory = opendir(posix_root);
    if (directory == NULL) {
        ESP_LOGI(TAG, "directory unavailable: %s", posix_root);
        return false;
    }

    struct dirent *item = NULL;
    while ((item = readdir(directory)) != NULL && list->count < PHOTO_SOURCE_MAX_DIR_ENTRIES) {
        if (!photo_catalog_name_is_safe(item->d_name)) {
            continue;
        }

        char path[PHOTO_CATALOG_PATH_MAX];
        const int written = snprintf(path, sizeof(path), "%s/%s", posix_root, item->d_name);
        if (written < 0 || (size_t)written >= sizeof(path)) {
            ESP_LOGW(TAG, "path too long under %s", posix_root);
            continue;
        }

        struct stat info;
        if (stat(path, &info) != 0 || !S_ISREG(info.st_mode)) {
            continue;
        }

        photo_dir_entry_t *entry = &list->entries[list->count];
        const int copied = snprintf(entry->name, sizeof(entry->name), "%s", item->d_name);
        if (copied < 0 || (size_t)copied >= sizeof(entry->name)) {
            continue;
        }
        entry->size_bytes = (uint64_t)info.st_size;
        list->count++;
    }

    closedir(directory);
    ESP_LOGI(TAG, "scanned %s: %u files", posix_root, (unsigned)list->count);
    return true;
}
