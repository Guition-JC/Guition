/* SPDX-License-Identifier: GPL-2.0-only */
#include "standby_web.h"

#include <stdlib.h>
#include <string.h>
#include "display.h"
#include "lvgl.h"
#include "app_settings_view.h"
#include "photo_source.h"
#include "power_idle.h"
#include "sleep_timeout.h"
#include "standby_prefs.h"
#include "video_player_service.h"

static const char *const mode_names[] = {"clock", "photos", "video"};

/**
 * @brief Create a caller-owned media source and name object.
 * @param source Media source identifier.
 * @param name Media file name.
 * @return JSON object, or NULL if object allocation fails.
 */
static cJSON *media_json(uint8_t source, const char *name)
{
    cJSON *item = cJSON_CreateObject();
    if (item != NULL) {
        cJSON_AddNumberToObject(item, "source", source);
        cJSON_AddStringToObject(item, "name", name);
    }
    return item;
}

esp_err_t standby_web_add_settings(cJSON *root)
{
    if (!lvgl_display_lock(1000)) return ESP_ERR_TIMEOUT;
    const standby_prefs_t *prefs = standby_prefs_get();
    cJSON_AddNumberToObject(root, "sleep_timeout_seconds", sleep_timeout_get());
    cJSON *standby = cJSON_AddObjectToObject(root, "standby");
    if (standby != NULL) {
        cJSON_AddStringToObject(standby, "mode", mode_names[prefs->mode]);
        cJSON_AddNumberToObject(standby, "photo_interval_seconds", prefs->photo_interval_seconds);
        cJSON *photos = cJSON_AddArrayToObject(standby, "photos");
        for (size_t i = 0; photos != NULL && i < prefs->photo_count; ++i)
            cJSON_AddItemToArray(photos, media_json(prefs->photos[i].source, prefs->photos[i].name));
        cJSON_AddItemToObject(standby, "video", prefs->video.name[0] != '\0'
            ? media_json(prefs->video.source, prefs->video.name) : cJSON_CreateNull());
    }
    lvgl_display_unlock();
    return standby != NULL ? ESP_OK : ESP_ERR_NO_MEM;
}

/**
 * @brief Check that a JSON number is integral and within inclusive bounds.
 * @param value JSON value to check.
 * @param minimum Inclusive lower bound.
 * @param maximum Inclusive upper bound.
 * @return true if the value is an integer within the bounds.
 */
static bool integer(const cJSON *value, int minimum, int maximum)
{
    return cJSON_IsNumber(value) && value->valuedouble >= minimum &&
           value->valuedouble <= maximum && value->valuedouble == value->valueint;
}

/**
 * @brief Validate a media reference and copy its source and safe file name.
 * @param json JSON media reference.
 * @param[out] media Receives the validated source and name.
 * @param video true to validate an MP4 name; false to validate a photo name.
 * @return true if the reference is valid and was copied.
 */
static bool parse_media(const cJSON *json, standby_media_t *media, bool video)
{
    if (!cJSON_IsObject(json)) return false;
    const cJSON *source = cJSON_GetObjectItemCaseSensitive(json, "source");
    const cJSON *name = cJSON_GetObjectItemCaseSensitive(json, "name");
    if (!integer(source, 0, 1) || !cJSON_IsString(name) ||
        strlen(name->valuestring) >= sizeof(media->name) ||
        !photo_catalog_name_is_safe(name->valuestring) ||
        !(video ? video_player_path_is_mp4(name->valuestring)
                : photo_catalog_is_supported_name(name->valuestring))) return false;
    media->source = (uint8_t)source->valueint;
    memcpy(media->name, name->valuestring, strlen(name->valuestring) + 1U);
    return true;
}

const char *standby_web_apply(const cJSON *root)
{
    const cJSON *timeout = cJSON_GetObjectItemCaseSensitive(root, "sleep_timeout_seconds");
    const cJSON *standby = cJSON_GetObjectItemCaseSensitive(root, "standby");
    if (timeout == NULL && standby == NULL) return NULL;
    if (timeout != NULL && (!integer(timeout, 0, 300) ||
        !power_idle_timeout_supported((uint32_t)timeout->valueint))) return "invalid_sleep_timeout";
    standby_prefs_t *draft = NULL;
    if (standby != NULL) {
        if (!cJSON_IsObject(standby)) return "invalid_standby";
        draft = calloc(1, sizeof(*draft));
        if (draft == NULL) return "no_memory";
        const cJSON *mode = cJSON_GetObjectItemCaseSensitive(standby, "mode");
        const cJSON *interval = cJSON_GetObjectItemCaseSensitive(standby, "photo_interval_seconds");
        const cJSON *photos = cJSON_GetObjectItemCaseSensitive(standby, "photos");
        const cJSON *video = cJSON_GetObjectItemCaseSensitive(standby, "video");
        bool valid = cJSON_IsString(mode) && integer(interval, 3, 30) &&
            (interval->valueint == 3 || interval->valueint == 5 ||
             interval->valueint == 10 || interval->valueint == 30) &&
            cJSON_IsArray(photos) && cJSON_GetArraySize(photos) <= STANDBY_MAX_PHOTOS;
        int selected_mode = -1;
        if (cJSON_IsString(mode)) {
            for (int i = 0; i < 3; ++i)
                if (strcmp(mode->valuestring, mode_names[i]) == 0) selected_mode = i;
        }
        valid = valid && selected_mode >= 0;
        if (valid) {
            draft->mode = (standby_mode_t)selected_mode;
            draft->photo_interval_seconds = (uint16_t)interval->valueint;
            const cJSON *item;
            cJSON_ArrayForEach(item, photos) {
                if (!parse_media(item, &draft->photos[draft->photo_count], false)) {
                    valid = false;
                    break;
                }
                ++draft->photo_count;
            }
            if (video != NULL && !cJSON_IsNull(video)) valid = valid && parse_media(video, &draft->video, true);
            if (draft->mode == STANDBY_PHOTOS && draft->photo_count == 0U) valid = false;
            if (draft->mode == STANDBY_VIDEO && draft->video.name[0] == '\0') valid = false;
        }
        if (!valid) {
            free(draft);
            return "invalid_standby";
        }
    }
    if (!lvgl_display_lock(1000)) {
        free(draft);
        return "display_busy";
    }
    esp_err_t result = draft != NULL ? standby_prefs_save(draft) : ESP_OK;
    if (result == ESP_OK && timeout != NULL) result = sleep_timeout_set((uint32_t)timeout->valueint);
    if (result == ESP_OK) app_settings_view_refresh_standby();
    lvgl_display_unlock();
    free(draft);
    return result == ESP_OK ? NULL : "standby_save_failed";
}

cJSON *standby_web_media(void)
{
    photo_catalog_t *catalog = calloc(1, sizeof(*catalog));
    photo_source_list_t *sources = calloc(2, sizeof(*sources));
    video_player_catalog_entry_t *videos = calloc(VIDEO_PLAYER_MAX_ENTRIES, sizeof(*videos));
    cJSON *root = cJSON_CreateObject();
    if (catalog == NULL || sources == NULL || videos == NULL || root == NULL) goto failed;
    photo_source_scan(PHOTO_SOURCE_SD_ROOT, &sources[0]);
    photo_source_scan(PHOTO_SOURCE_BUILTIN_ROOT, &sources[1]);
    const photo_catalog_source_t inputs[] = {
        {PHOTO_SOURCE_SD_ROOT, PHOTO_SOURCE_SD_LV_ROOT, PHOTO_SOURCE_SD, sources[0].entries, sources[0].count},
        {PHOTO_SOURCE_BUILTIN_ROOT, PHOTO_SOURCE_BUILTIN_LV_ROOT, PHOTO_SOURCE_BUILTIN,
         sources[1].entries, sources[1].count},
    };
    (void)photo_catalog_merge(inputs, 2, catalog);
    cJSON *photos = cJSON_AddArrayToObject(root, "photos");
    cJSON *clips = cJSON_AddArrayToObject(root, "videos");
    if (photos == NULL || clips == NULL || !lvgl_display_lock(1000)) goto failed;
    for (size_t i = 0; i < catalog->count; ++i) {
        const photo_catalog_entry_t *entry = &catalog->entries[i];
        lv_image_header_t header;
        if (lv_image_decoder_get_info(entry->lv_path, &header) == LV_RESULT_OK &&
            photo_catalog_within_decode_budget(entry->lv_path, header.w, header.h))
            cJSON_AddItemToArray(photos, media_json(entry->source, entry->name));
    }
    lvgl_display_unlock();
    const size_t count = video_player_service_list(videos, VIDEO_PLAYER_MAX_ENTRIES);
    for (size_t i = 0; i < count; ++i)
        cJSON_AddItemToArray(clips, media_json(strncmp(videos[i].path, "/sdcard/", 8) == 0 ? 0 : 1,
                                             videos[i].name));
    cJSON_AddBoolToObject(root, "ok", true);
    free(catalog);
    free(sources);
    free(videos);
    return root;
failed:
    free(catalog);
    free(sources);
    free(videos);
    cJSON_Delete(root);
    return NULL;
}
