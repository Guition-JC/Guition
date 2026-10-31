#define _POSIX_C_SOURCE 200809L

#include "music_sd_source.h"

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "esp_attr.h"
#include "audio_gmf.h"
#include "esp_audio_simple_dec.h"
#include "esp_gmf_audio_dec.h"
#include "esp_gmf_asrc.h"
#include "esp_gmf_err.h"
#include "esp_gmf_obj.h"
#include "esp_gmf_io.h"
#include "esp_log.h"

#define TAG "MusicSdSource"

#define MUSIC_SD_SOURCE_ROOT_SD "/sdcard/music"
/** @brief Built-in demo tracks shipped in the SPIFFS assets partition and mounted at /assets. */
#define MUSIC_SD_SOURCE_ROOT_BUILTIN "/assets/music"
#define MUSIC_SD_SOURCE_MAX_DIR_ENTRIES (MUSIC_SD_CATALOG_MAX_ENTRIES * 2U)

#define MUSIC_SD_DAC_SAMPLE_RATE 48000U
#define MUSIC_SD_DAC_CHANNELS 2U
#define MUSIC_SD_GMF_TASK_STACK_SIZE 4096U

static const char *const s_local_roots[] = {
    MUSIC_SD_SOURCE_ROOT_SD,
    MUSIC_SD_SOURCE_ROOT_BUILTIN,
};

static music_player_error_t gmf_error(esp_gmf_err_t error)
{
    return error == ESP_GMF_ERR_OK ? MUSIC_PLAYER_ERROR_OK : MUSIC_PLAYER_ERROR_BACKEND;
}

static void report(music_sd_source_t *source, music_player_error_t error, const char *detail)
{
    if (source->event_cb != NULL) {
        source->event_cb(source->event_context, error, source->generation, detail);
    }
}

static void record_first(music_player_error_t *first, music_player_error_t error)
{
    if (*first == MUSIC_PLAYER_ERROR_OK && error != MUSIC_PLAYER_ERROR_OK) {
        *first = error;
    }
}

static esp_gmf_err_t pipeline_event(esp_gmf_event_pkt_t *packet, void *context)
{
    music_sd_source_t *source = context;
    if (source == NULL || packet == NULL || packet->type != ESP_GMF_EVT_TYPE_CHANGE_STATE) {
        return ESP_GMF_ERR_OK;
    }
    if (packet->sub == ESP_GMF_EVENT_STATE_FINISHED) {
        source->running = false;
        report(source, MUSIC_PLAYER_ERROR_END_OF_TRACK, "sd track finished");
    } else if (packet->sub == ESP_GMF_EVENT_STATE_ERROR) {
        source->running = false;
        report(source, MUSIC_PLAYER_ERROR_BACKEND, "sd pipeline error");
    }
    return ESP_GMF_ERR_OK;
}

static esp_gmf_element_handle_t find_asrc(esp_gmf_pipeline_handle_t pipeline)
{
    const void *iterator = NULL;
    esp_gmf_element_handle_t element = NULL;
    while (esp_gmf_pipeline_iterate_element(pipeline, &iterator, &element) == ESP_GMF_ERR_OK) {
        char *tag = NULL;
        esp_gmf_obj_get_tag((esp_gmf_obj_handle_t)element, &tag);
        if (tag != NULL && strcmp(tag, "aud_asrc") == 0) return element;
    }
    return NULL;
}

/**
 * @brief Set the DAC resampling format and let the element derive channel weights from decoded stream information.
 * @param pipeline Pipeline containing the resampling element.
 * @return MUSIC_PLAYER_ERROR_OK on success, otherwise a player error.
 */
static music_player_error_t configure_asrc(esp_gmf_pipeline_handle_t pipeline)
{
    esp_gmf_element_handle_t asrc = find_asrc(pipeline);
    if (asrc == NULL || esp_gmf_asrc_set_dest_rate(asrc, MUSIC_SD_DAC_SAMPLE_RATE) != ESP_GMF_ERR_OK ||
        esp_gmf_asrc_set_dest_ch(asrc, MUSIC_SD_DAC_CHANNELS) != ESP_GMF_ERR_OK) {
        return MUSIC_PLAYER_ERROR_BACKEND;
    }
    return MUSIC_PLAYER_ERROR_OK;
}

/**
 * @brief Select the decoder container from the catalog extension, retaining header probing as fallback.
 * @param pipeline Pipeline containing the decoder.
 * @param type Media type used to choose a decoder container.
 * @return MUSIC_PLAYER_ERROR_OK on success, otherwise a player error.
 */
static music_player_error_t configure_decoder(esp_gmf_pipeline_handle_t pipeline,
                                               music_sd_media_type_t type)
{
    esp_audio_simple_dec_cfg_t config = DEFAULT_ESP_GMF_AUDIO_DEC_CONFIG();
    switch (type) {
        case MUSIC_SD_MEDIA_MP3:
            config.dec_type = ESP_AUDIO_SIMPLE_DEC_TYPE_MP3;
            break;
        case MUSIC_SD_MEDIA_WAV:
            config.dec_type = ESP_AUDIO_SIMPLE_DEC_TYPE_WAV;
            break;
        case MUSIC_SD_MEDIA_AAC:
            config.dec_type = ESP_AUDIO_SIMPLE_DEC_TYPE_AAC;
            break;
        default:
            return MUSIC_PLAYER_ERROR_INVALID_ARGUMENT;
    }
    config.use_frame_dec = false;

    esp_gmf_element_handle_t decoder = NULL;
    if (esp_gmf_pipeline_get_head_el(pipeline, &decoder) != ESP_GMF_ERR_OK || decoder == NULL) {
        return MUSIC_PLAYER_ERROR_BACKEND;
    }
    return esp_gmf_audio_dec_reconfig(decoder, &config) == ESP_GMF_ERR_OK ? MUSIC_PLAYER_ERROR_OK
                                                                          : MUSIC_PLAYER_ERROR_BACKEND;
}

static music_player_error_t enumerate_root(const char *root, music_sd_catalog_t *catalog)
{
    DIR *directory = opendir(root);
    if (directory == NULL) {
        return MUSIC_PLAYER_ERROR_SD_UNAVAILABLE;
    }
    /** Allocate directory-scan staging tables in PSRAM before copying entries into the catalog. */
    EXT_RAM_BSS_ATTR static music_sd_catalog_dir_entry_t
        entries[MUSIC_SD_SOURCE_MAX_DIR_ENTRIES];
    EXT_RAM_BSS_ATTR static char
        names[MUSIC_SD_SOURCE_MAX_DIR_ENTRIES][MUSIC_SD_CATALOG_NAME_MAX];
    size_t count = 0U;
    struct dirent *item;
    while ((item = readdir(directory)) != NULL && count < MUSIC_SD_SOURCE_MAX_DIR_ENTRIES) {
        size_t length = strlen(item->d_name);
        if (length >= MUSIC_SD_CATALOG_NAME_MAX) {
            continue;
        }
        memcpy(names[count], item->d_name, length);
        names[count][length] = '\0';
        char path[MUSIC_SD_CATALOG_PATH_MAX];
        int written = snprintf(path, sizeof(path), "%s/%s", root, names[count]);
        struct stat info;
        if (written < 0 || (size_t)written >= sizeof(path) || stat(path, &info) != 0) {
            continue;
        }
        entries[count].name = names[count];
        entries[count].title = NULL;
        entries[count].artist = NULL;
        entries[count].size_bytes = S_ISREG(info.st_mode) ? (uint64_t)info.st_size : 0U;
        entries[count].is_directory = !S_ISREG(info.st_mode);
        ++count;
    }
    (void)closedir(directory);
    const music_sd_catalog_error_t result = music_sd_catalog_append(catalog, root, entries, count);
    if (result == MUSIC_SD_CATALOG_OK || result == MUSIC_SD_CATALOG_EMPTY ||
        result == MUSIC_SD_CATALOG_TOO_MANY_ENTRIES) {
        return MUSIC_PLAYER_ERROR_OK;
    }
    return result == MUSIC_SD_CATALOG_INVALID_ARGUMENT ? MUSIC_PLAYER_ERROR_INVALID_ARGUMENT
                                                       : MUSIC_PLAYER_ERROR_BACKEND;
}

music_player_error_t music_sd_source_open(music_sd_source_t *source,
                                           esp_gmf_pool_handle_t pool,
                                           dev_audio_codec_handles_t *dac,
                                           const char *const *roots,
                                           size_t root_count,
                                           music_sd_source_event_cb_t event_cb,
                                           void *event_context)
{
    if (source == NULL || pool == NULL || dac == NULL || dac->codec_dev == NULL) {
        return MUSIC_PLAYER_ERROR_INVALID_ARGUMENT;
    }
    if (source->opened) {
        return MUSIC_PLAYER_ERROR_OK;
    }
    memset(source, 0, sizeof(*source));
    source->pool = pool;
    source->dac = dac;
    source->event_cb = event_cb;
    source->event_context = event_context;
    if (roots == NULL || root_count == 0U) {
        roots = s_local_roots;
        root_count = sizeof(s_local_roots) / sizeof(s_local_roots[0]);
    }
    size_t readable_roots = 0U;
    music_sd_catalog_begin(&source->catalog);
    for (size_t index = 0U; index < root_count; ++index) {
        if (roots[index] == NULL) {
            continue;
        }
        const music_player_error_t scan = enumerate_root(roots[index], &source->catalog);
        if (scan == MUSIC_PLAYER_ERROR_OK) {
            readable_roots++;
        } else if (scan != MUSIC_PLAYER_ERROR_SD_UNAVAILABLE) {
            (void)music_sd_source_close(source);
            return scan;
        }
        if (source->catalog.count >= MUSIC_SD_CATALOG_MAX_ENTRIES) {
            break;
        }
    }
    const music_sd_catalog_error_t merged = music_sd_catalog_finish(&source->catalog);
    if (merged != MUSIC_SD_CATALOG_OK) {
        const music_player_error_t error = readable_roots == 0U ? MUSIC_PLAYER_ERROR_SD_UNAVAILABLE
                                                                : MUSIC_PLAYER_ERROR_BACKEND;
        (void)music_sd_source_close(source);
        return error;
    }
    source->opened = true;
    return MUSIC_PLAYER_ERROR_OK;
}

music_player_error_t music_sd_source_start(music_sd_source_t *source, size_t index)
{
    if (source == NULL || !source->opened) return MUSIC_PLAYER_ERROR_NOT_OPEN;
    if (index >= source->catalog.count) return MUSIC_PLAYER_ERROR_INVALID_ARGUMENT;
    music_player_error_t error = music_sd_source_stop(source);
    if (error != MUSIC_PLAYER_ERROR_OK) return error;
    ++source->generation;
    /** Local files are decoded by the parse-mode element that probes the stream header, so MP3, WAV and AAC entries all play through this pipeline. */
    const char *stage = "create the pipeline";
    const char *elements[] = {AUDIO_GMF_FILE_DECODER_TAG, "aud_asrc"};
    esp_gmf_err_t ret = esp_gmf_pool_new_pipeline(source->pool, "sd_io_file", elements, 2,
                                                   "sd_io_codec_dev", &source->pipeline);
    if (ret != ESP_GMF_ERR_OK) goto fail;
    stage = "attach the pipeline event";
    ret = esp_gmf_pipeline_set_event(source->pipeline, pipeline_event, source);
    if (ret != ESP_GMF_ERR_OK) goto fail;
    stage = "create the GMF task";
    esp_gmf_task_cfg_t task_cfg = DEFAULT_ESP_GMF_TASK_CONFIG();
    task_cfg.thread.stack = MUSIC_SD_GMF_TASK_STACK_SIZE;
    task_cfg.thread.stack_in_ext = true;
    task_cfg.name = "music_sd_task";
    ret = esp_gmf_task_init(&task_cfg, &source->task);
    if (ret != ESP_GMF_ERR_OK) goto fail;
    stage = "bind the GMF task";
    ret = esp_gmf_pipeline_bind_task(source->pipeline, source->task);
    if (ret != ESP_GMF_ERR_OK) goto fail;
    stage = "set the input URI";
    char uri[MUSIC_SD_CATALOG_PATH_MAX + 8U];
    int written = snprintf(uri, sizeof(uri), "file://%s", source->catalog.entries[index].path);
    if (written < 0 || (size_t)written >= sizeof(uri)) goto fail;
    ret = esp_gmf_pipeline_set_in_uri(source->pipeline, uri);
    if (ret != ESP_GMF_ERR_OK) goto fail;
    stage = "configure the decoder";
    if (configure_decoder(source->pipeline, source->catalog.entries[index].type) !=
        MUSIC_PLAYER_ERROR_OK) {
        goto fail;
    }
    stage = "configure the resampler";
    if (configure_asrc(source->pipeline) != MUSIC_PLAYER_ERROR_OK) goto fail;
    stage = "load the pipeline jobs";
    ret = esp_gmf_pipeline_loading_jobs(source->pipeline);
    if (ret != ESP_GMF_ERR_OK) goto fail;
    stage = "run the pipeline";
    ret = esp_gmf_pipeline_run(source->pipeline);
    if (ret != ESP_GMF_ERR_OK) goto fail;
    source->current_index = index;
    source->running = true;
    report(source, MUSIC_PLAYER_ERROR_OK, "sd track started");
    return MUSIC_PLAYER_ERROR_OK;
fail:
    /** Log the track path associated with a pipeline-open failure. */
    ESP_LOGE(TAG, "Local playback of '%s' failed at %s", source->catalog.entries[index].path,
             stage);
    report(source, MUSIC_PLAYER_ERROR_BACKEND, "sd pipeline start failed");
    (void)music_sd_source_stop(source);
    return MUSIC_PLAYER_ERROR_BACKEND;
}

music_player_error_t music_sd_source_pause(music_sd_source_t *source)
{
    if (source == NULL || !source->opened) return MUSIC_PLAYER_ERROR_NOT_OPEN;
    if (!source->running) return MUSIC_PLAYER_ERROR_OK;
    return gmf_error(esp_gmf_pipeline_pause(source->pipeline));
}

music_player_error_t music_sd_source_resume(music_sd_source_t *source)
{
    if (source == NULL || !source->opened) return MUSIC_PLAYER_ERROR_NOT_OPEN;
    /** Reject resume for completed or failed pipelines so the caller can restart the track. */
    if (!source->running) return MUSIC_PLAYER_ERROR_INVALID_COMMAND;
    return gmf_error(esp_gmf_pipeline_resume(source->pipeline));
}

music_player_error_t music_sd_source_stop(music_sd_source_t *source)
{
    if (source == NULL) return MUSIC_PLAYER_ERROR_INVALID_ARGUMENT;
    music_player_error_t first = MUSIC_PLAYER_ERROR_OK;
    if (source->pipeline != NULL && source->task != NULL) {
        record_first(&first, gmf_error(esp_gmf_pipeline_stop(source->pipeline)));
    }
    if (source->task != NULL) {
        record_first(&first, gmf_error(esp_gmf_task_deinit(source->task)));
        source->task = NULL;
    }
    if (source->pipeline != NULL) {
        record_first(&first, gmf_error(esp_gmf_pipeline_destroy(source->pipeline)));
        source->pipeline = NULL;
    }
    source->running = false;
    return first;
}

music_player_error_t music_sd_source_close(music_sd_source_t *source)
{
    if (source == NULL) return MUSIC_PLAYER_ERROR_INVALID_ARGUMENT;
    music_player_error_t first = music_sd_source_stop(source);
    source->dac = NULL;
    source->event_cb = NULL;
    source->event_context = NULL;
    source->opened = false;
    source->pool = NULL;
    source->catalog.count = 0U;
    return first;
}

const music_sd_catalog_t *music_sd_source_catalog(const music_sd_source_t *source)
{
    return source == NULL ? NULL : &source->catalog;
}
