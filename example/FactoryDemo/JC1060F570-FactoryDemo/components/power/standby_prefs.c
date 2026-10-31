/** SPDX-License-Identifier: GPL-2.0-only */
#include "standby_prefs.h"

#include <string.h>
#include <stdbool.h>
#include <stddef.h>
#include "esp_attr.h"
#include "nvs.h"

#define STANDBY_NVS_NAMESPACE "power_sleep"
#define STANDBY_NVS_KEY "standby_v1"

/** @brief A fixed layout and version make a damaged or old record fall back to clock. */
typedef struct {
    uint8_t version;
    uint8_t mode;
    uint16_t interval;
    uint8_t photo_count;
    standby_media_t media[STANDBY_MAX_PHOTOS + 1U];
} standby_record_t;

static standby_prefs_t s_prefs = {.mode = STANDBY_CLOCK, .photo_interval_seconds = 5U};
EXT_RAM_BSS_ATTR static standby_record_t s_record;
EXT_RAM_BSS_ATTR static standby_prefs_t s_candidate;

static bool valid_media(const standby_media_t *item)
{
    if (item->source > 1U || item->name[0] == '\0' ||
        memchr(item->name, '\0', sizeof(item->name)) == NULL ||
        item->name[0] == '.') return false;
    for (const char *p = item->name; *p != '\0'; ++p) {
        if (*p == '/' || *p == '\\' || *p == ':') return false;
    }
    return true;
}

static bool valid(const standby_prefs_t *prefs)
{
    if (prefs == NULL || prefs->mode > STANDBY_VIDEO || prefs->photo_count > STANDBY_MAX_PHOTOS) return false;
    if (prefs->photo_interval_seconds != 3U && prefs->photo_interval_seconds != 5U &&
        prefs->photo_interval_seconds != 10U && prefs->photo_interval_seconds != 30U) return false;
    for (size_t i = 0; i < prefs->photo_count; ++i) {
        if (!valid_media(&prefs->photos[i])) return false;
    }
    if (prefs->mode == STANDBY_PHOTOS && prefs->photo_count == 0U) return false;
    if (prefs->mode == STANDBY_VIDEO && !valid_media(&prefs->video)) return false;
    return true;
}

void standby_prefs_restore(void)
{
    memset(&s_prefs, 0, sizeof(s_prefs));
    s_prefs.mode = STANDBY_CLOCK;
    s_prefs.photo_interval_seconds = 5U;
    nvs_handle_t handle;
    if (nvs_open(STANDBY_NVS_NAMESPACE, NVS_READONLY, &handle) != ESP_OK) return;
    size_t length = sizeof(s_record);
    const esp_err_t result = nvs_get_blob(handle, STANDBY_NVS_KEY, &s_record, &length);
    nvs_close(handle);
    if (result != ESP_OK || s_record.version != 1U ||
        s_record.photo_count > STANDBY_MAX_PHOTOS ||
        length != offsetof(standby_record_t, media) +
                  ((size_t)s_record.photo_count + 1U) * sizeof(standby_media_t)) return;
    memset(&s_candidate, 0, sizeof(s_candidate));
    s_candidate.mode = (standby_mode_t)s_record.mode;
    s_candidate.photo_interval_seconds = s_record.interval;
    s_candidate.photo_count = s_record.photo_count;
    memcpy(s_candidate.photos, s_record.media,
           (size_t)s_candidate.photo_count * sizeof(standby_media_t));
    s_candidate.video = s_record.media[s_candidate.photo_count];
    if (valid(&s_candidate)) s_prefs = s_candidate;
}

const standby_prefs_t *standby_prefs_get(void)
{
    return &s_prefs;
}

esp_err_t standby_prefs_save(const standby_prefs_t *prefs)
{
    if (!valid(prefs)) return ESP_ERR_INVALID_ARG;
    memset(&s_record, 0, sizeof(s_record));
    s_record.version = 1U;
    s_record.mode = (uint8_t)prefs->mode;
    s_record.interval = prefs->photo_interval_seconds;
    s_record.photo_count = prefs->photo_count;
    memcpy(s_record.media, prefs->photos,
           (size_t)prefs->photo_count * sizeof(standby_media_t));
    s_record.media[prefs->photo_count] = prefs->video;
    nvs_handle_t handle;
    esp_err_t result = nvs_open(STANDBY_NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (result != ESP_OK) return result;
    const size_t length = offsetof(standby_record_t, media) +
                          ((size_t)prefs->photo_count + 1U) * sizeof(standby_media_t);
    result = nvs_set_blob(handle, STANDBY_NVS_KEY, &s_record, length);
    if (result == ESP_OK) result = nvs_commit(handle);
    nvs_close(handle);
    if (result == ESP_OK) s_prefs = *prefs;
    return result;
}
