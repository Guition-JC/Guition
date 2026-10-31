#include "video_player_service.h"

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "audio_output.h"
#include "audio_runtime.h"
#include "esp_audio_dec.h"
#include "esp_audio_dec_default.h"
#include "esp_aac_dec.h"
#include "esp_codec_dev.h"
#include "esp_attr.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_video_codec_utils.h"
#include "esp_video_dec.h"
#include "esp_video_dec_default.h"
#include "esp_video_dec_mjpeg.h"
#include "esp_extractor.h"
#include "esp_extractor_defaults.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "video_player_state.h"

#define TAG "VideoPlayer"
/** @brief Playback sources in scan order. */
#define VIDEO_PLAYER_ASSET_ROOT "/assets/video"
#define VIDEO_PLAYER_SDCARD_ROOT "/sdcard/video"
static const char *const s_catalog_roots[] = {VIDEO_PLAYER_ASSET_ROOT, VIDEO_PLAYER_SDCARD_ROOT};
#define VIDEO_PLAYER_TASK_STACK 8192U
#define VIDEO_PLAYER_VIDEO_TASK_STACK 6144U
#define VIDEO_PLAYER_TASK_PRIORITY 4U
#define VIDEO_PLAYER_AUDIO_CORE 0
/** @brief The decode task shares the demux's core on purpose. */
#define VIDEO_PLAYER_VIDEO_CORE 0
#define VIDEO_PLAYER_VIDEO_QUEUE_LENGTH 2U
#define VIDEO_PLAYER_VIDEO_QUEUE_WAIT_MS 20U
#define VIDEO_PLAYER_VIDEO_JOIN_WAIT_MS 2000U
#define VIDEO_PLAYER_LOCK_WAIT_MS 1000U
#define VIDEO_PLAYER_EXIT_WAIT_MS 3000U
#define VIDEO_PLAYER_EXTRACT_POOL_SIZE (256U * 1024U)
#define VIDEO_PLAYER_AUDIO_BUFFER_SIZE (16U * 1024U)

typedef struct {
    FILE *file;
    /** Service-owned preloaded file data used by the extractor. */
    const uint8_t *data;
    uint32_t size;
    uint32_t position;
} video_file_source_t;

typedef struct {
    uint8_t *data;
    uint32_t size;
    char path[VIDEO_PLAYER_PATH_MAX];
} video_player_preload_t;

typedef video_player_catalog_entry_t video_player_entry_t;

typedef struct {
    uint8_t *data;
    uint32_t size;
    uint32_t pts;
    bool end_of_stream;
} video_player_video_packet_t;

typedef struct {
    SemaphoreHandle_t lock;
    TaskHandle_t task;
    TaskHandle_t video_task;
    QueueHandle_t video_queue;
    esp_video_dec_handle_t video_decoder;
    /** Caller-owned RGB565 buffers; a published buffer remains reserved until the view releases it. */
    uint8_t *output_buffers[2];
    uint32_t output_capacity;
    bool output_held_by_view[2];
    uint8_t output_next;
    /** Fixed playback resolution taken from the board panel definition. */
    uint16_t display_width;
    uint16_t display_height;
    uint32_t video_frames_decoded;
    uint32_t video_frames_dropped;
    uint32_t audio_frames_decoded;
    uint32_t audio_frames_dropped;
    video_player_state_t state;
    video_player_entry_t entries[VIDEO_PLAYER_MAX_ENTRIES];
    size_t count;
    size_t selected;
    video_player_preload_t preload;
    uint16_t width;
    uint16_t height;
    uint16_t fps;
    bool entered;
    bool audio_runtime_acquired;
    bool silent;
    bool stop_requested;
    bool paused;
    bool switch_requested;
    bool restart_requested;
    size_t switch_target;
    esp_err_t last_error;
    video_player_frame_cb_t frame_cb;
    void *frame_context;
} video_player_service_state_t;

/** Service metadata resides in PSRAM; frame buffers remain caller-owned. */
EXT_RAM_BSS_ATTR static video_player_service_state_t s_service;
static bool s_extractors_registered;
static bool s_video_decoders_registered;
static bool s_audio_decoders_registered;
static bool s_service_initialized;

static void release_preload_locked(void);
static esp_err_t start_playback_locked(bool preserve_held_buffers);
static void finish_playback_locked(void);

static SemaphoreHandle_t service_lock(void)
{
    if (s_service.lock == NULL && !s_service_initialized) {
        SemaphoreHandle_t lock = xSemaphoreCreateMutex();
        if (lock != NULL) {
            s_service.lock = lock;
            s_service_initialized = true;
        }
    }
    return s_service.lock;
}

static bool lock_service(TickType_t wait)
{
    SemaphoreHandle_t lock = service_lock();
    return lock != NULL && xSemaphoreTake(lock, wait) == pdTRUE;
}

static void unlock_service(void)
{
    xSemaphoreGive(s_service.lock);
}

static int file_read(void *buffer, uint32_t size, void *context)
{
    video_file_source_t *source = context;
    if (source == NULL) {
        return -1;
    }
    if (source->data != NULL) {
        const uint32_t available = source->size - source->position;
        const uint32_t wanted = size < available ? size : available;
        memcpy(buffer, source->data + source->position, wanted);
        source->position += wanted;
        return (int)wanted;
    }
    return source->file == NULL ? -1 : (int)fread(buffer, 1U, size, source->file);
}

static int file_seek(uint32_t position, void *context)
{
    video_file_source_t *source = context;
    if (source == NULL) {
        return -1;
    }
    if (source->data != NULL) {
        if (position > source->size) {
            return -1;
        }
        source->position = position;
        return 0;
    }
    return source->file == NULL ? -1 : fseek(source->file, (long)position, SEEK_SET);
}

static uint32_t file_size(void *context)
{
    video_file_source_t *source = context;
    if (source == NULL) {
        return 0U;
    }
    if (source->data != NULL) {
        return source->size;
    }
    if (source->file == NULL) {
        return 0U;
    }
    const long current = ftell(source->file);
    if (current < 0 || fseek(source->file, 0L, SEEK_END) != 0) {
        return 0U;
    }
    const long end = ftell(source->file);
    (void)fseek(source->file, current, SEEK_SET);
    return end <= 0L || (unsigned long)end > UINT32_MAX ? 0U : (uint32_t)end;
}

static const char *extractor_format_name(esp_extractor_format_t format)
{
    switch (format) {
    case ESP_EXTRACTOR_VIDEO_FORMAT_MJPEG: return "MJPEG";
    case ESP_EXTRACTOR_AUDIO_FORMAT_AAC: return "AAC";
    case ESP_EXTRACTOR_AUDIO_FORMAT_MP3: return "MP3";
    case ESP_EXTRACTOR_VIDEO_FORMAT_H264: return "H264";
    default: return "UNKNOWN";
    }
}

static void log_stream_info(const char *kind, const esp_extractor_stream_info_t *info)
{
    if (info == NULL) {
        ESP_LOGE(TAG, "stream[%s] info is NULL", kind == NULL ? "?" : kind);
        return;
    }
    if (info->stream_type == ESP_EXTRACTOR_STREAM_TYPE_VIDEO) {
        ESP_LOGI(TAG, "stream[%s]: id=%u format=%s(0x%08" PRIx32 ") size=%ux%u fps=%u duration=%" PRIu32 "ms",
                 kind, info->stream_id, extractor_format_name(info->video_info.format),
                 (uint32_t)info->video_info.format, info->video_info.width, info->video_info.height,
                 info->video_info.fps, info->duration);
    } else if (info->stream_type == ESP_EXTRACTOR_STREAM_TYPE_AUDIO) {
        ESP_LOGI(TAG, "stream[%s]: id=%u format=%s(0x%08" PRIx32 ") rate=%" PRIu32 "Hz ch=%u bits=%u duration=%" PRIu32 "ms",
                 kind, info->stream_id, extractor_format_name(info->audio_info.format),
                 (uint32_t)info->audio_info.format, info->audio_info.sample_rate,
                 info->audio_info.channel, info->audio_info.bits_per_sample, info->duration);
    } else {
        ESP_LOGE(TAG, "stream[%s]: unknown stream type=%d", kind, info->stream_type);
    }
}

static bool stream_is_supported(const esp_extractor_stream_info_t *video,
                                const esp_extractor_stream_info_t *audio, bool silent)
{
    if (video == NULL || (!silent && audio == NULL)) {
        ESP_LOGE(TAG, "media validation failed: missing video or audio stream info");
        return false;
    }
    log_stream_info("video", video);
    if (audio != NULL) log_stream_info("audio", audio);
    if (video->video_info.format != ESP_EXTRACTOR_VIDEO_FORMAT_MJPEG) {
        ESP_LOGE(TAG, "media validation failed: video codec is %s, expected MJPEG",
                 extractor_format_name(video->video_info.format));
        return false;
    }
    if (!silent && audio->audio_info.format != ESP_EXTRACTOR_AUDIO_FORMAT_AAC &&
        audio->audio_info.format != ESP_EXTRACTOR_AUDIO_FORMAT_MP3) {
        ESP_LOGE(TAG, "media validation failed: audio codec is %s, expected AAC or MP3",
                 extractor_format_name(audio->audio_info.format));
        return false;
    }
    if (video->video_info.width == 0U || video->video_info.height == 0U) {
        ESP_LOGE(TAG, "media validation failed: invalid video resolution %ux%u",
                 video->video_info.width, video->video_info.height);
        return false;
    }
    if (!silent && (audio->audio_info.sample_rate == 0U || (audio->audio_info.channel != 1U && audio->audio_info.channel != 2U))) {
        ESP_LOGE(TAG, "media validation failed: invalid audio format rate=%" PRIu32 "Hz channels=%u",
                 audio->audio_info.sample_rate, audio->audio_info.channel);
        return false;
    }
    return true;
}

static esp_err_t register_codecs(bool silent)
{
    if (!s_extractors_registered) {
        const esp_extractor_err_t ret = esp_extractor_register_default();
        if (ret != ESP_EXTRACTOR_ERR_OK) {
            ESP_LOGE(TAG, "register extractor failed: ret=%d", ret);
            return ESP_FAIL;
        }
        s_extractors_registered = true;
        ESP_LOGI(TAG, "extractor codecs registered (MP4 enabled=%d)", CONFIG_MP4_EXTRACTOR_SUPPORT);
    }
    if (!s_video_decoders_registered) {
        const esp_vc_err_t ret = esp_video_dec_register_default();
        if (ret != ESP_VC_ERR_OK) {
            ESP_LOGE(TAG, "register video decoders failed: ret=%d (SW MJPEG enabled=%d)",
                     ret, CONFIG_VIDEO_DECODER_SW_MJPEG_SUPPORT);
            return ESP_FAIL;
        }
        s_video_decoders_registered = true;
        ESP_LOGI(TAG, "video decoders registered (SW MJPEG=%d, HW MJPEG=%d)",
                 CONFIG_VIDEO_DECODER_SW_MJPEG_SUPPORT, CONFIG_VIDEO_DECODER_HW_MJPEG_SUPPORT);
    }
    if (!silent && !s_audio_decoders_registered) {
        const esp_audio_err_t ret = esp_audio_dec_register_default();
        if (ret != ESP_AUDIO_ERR_OK) {
            ESP_LOGE(TAG, "register audio decoders failed: ret=%d (AAC=%d MP3=%d)",
                     ret, CONFIG_AUDIO_DECODER_AAC_SUPPORT, CONFIG_AUDIO_DECODER_MP3_SUPPORT);
            return ESP_FAIL;
        }
        s_audio_decoders_registered = true;
        ESP_LOGI(TAG, "audio decoders registered (AAC=%d MP3=%d)",
                 CONFIG_AUDIO_DECODER_AAC_SUPPORT, CONFIG_AUDIO_DECODER_MP3_SUPPORT);
    }
    return ESP_OK;
}

static esp_err_t open_extractor(const char *path, video_file_source_t *source,
                                esp_extractor_config_t *config, esp_extractor_handle_t *extractor)
{
    if (path == NULL || source == NULL || config == NULL || extractor == NULL) return ESP_ERR_INVALID_ARG;
    memset(source, 0, sizeof(*source));
    memset(config, 0, sizeof(*config));
    *extractor = NULL;
    bool from_memory = false;
    if (lock_service(pdMS_TO_TICKS(VIDEO_PLAYER_LOCK_WAIT_MS))) {
        if (s_service.preload.data != NULL && strcmp(path, s_service.preload.path) == 0) {
            source->data = s_service.preload.data;
            source->size = s_service.preload.size;
            from_memory = true;
        }
        unlock_service();
    }
    if (from_memory) {
        ESP_LOGI(TAG, "playback reads the preloaded PSRAM copy: path=%s bytes=%" PRIu32,
                 path, source->size);
    } else {
        source->file = fopen(path, "rb");
        if (source->file == NULL) {
            ESP_LOGE(TAG, "open media file failed: path=%s errno=%d (%s)", path, errno, strerror(errno));
            return ESP_ERR_NOT_FOUND;
        }
        ESP_LOGI(TAG, "open media file: %s", path);
    }
    *config = (esp_extractor_config_t) {
        .type = ESP_EXTRACTOR_TYPE_MP4,
        .extract_mask = ESP_EXTRACT_MASK_AV,
        .in_read_cb = file_read,
        .in_seek_cb = file_seek,
        .in_size_cb = file_size,
        .in_ctx = source,
        .out_pool_size = VIDEO_PLAYER_EXTRACT_POOL_SIZE,
        .out_align = 16U,
    };
    const esp_extractor_err_t open_ret = esp_extractor_open(config, extractor);
    if (open_ret != ESP_EXTRACTOR_ERR_OK) {
        ESP_LOGE(TAG, "extractor open failed: ret=%d path=%s", open_ret, path);
        fclose(source->file);
        source->file = NULL;
        return ESP_FAIL;
    }
    const esp_extractor_err_t parse_ret = esp_extractor_parse_stream(*extractor);
    if (parse_ret != ESP_EXTRACTOR_ERR_OK) {
        ESP_LOGE(TAG, "MP4 parse failed: ret=%d path=%s", parse_ret, path);
        (void)esp_extractor_close(*extractor);
        fclose(source->file);
        source->file = NULL;
        return ESP_FAIL;
    }
    esp_extractor_type_t actual_type = ESP_EXTRACTOR_TYPE_NONE;
    (void)esp_extractor_get_extractor_type(*extractor, &actual_type);
    ESP_LOGI(TAG, "media parse completed: container=%s path=%s",
             actual_type == ESP_EXTRACTOR_TYPE_AVI ? "AVI" :
             actual_type == ESP_EXTRACTOR_TYPE_MP4 ? "MP4" : "OTHER", path);
    return ESP_OK;
}

static void close_extractor(esp_extractor_handle_t extractor, video_file_source_t *source)
{
    if (extractor != NULL) (void)esp_extractor_close(extractor);
    if (source != NULL && source->file != NULL) {
        fclose(source->file);
        source->file = NULL;
    }
}

static esp_err_t read_stream_info(esp_extractor_handle_t extractor,
                                  esp_extractor_stream_info_t *video,
                                  esp_extractor_stream_info_t *audio, bool silent)
{
    uint16_t video_count = 0U;
    uint16_t audio_count = 0U;
    esp_extractor_err_t ret = esp_extractor_get_stream_num(extractor, ESP_EXTRACTOR_STREAM_TYPE_VIDEO, &video_count);
    if (ret != ESP_EXTRACTOR_ERR_OK) {
        ESP_LOGE(TAG, "get video stream count failed: ret=%d", ret);
        return ESP_ERR_NOT_SUPPORTED;
    }
    ret = esp_extractor_get_stream_num(extractor, ESP_EXTRACTOR_STREAM_TYPE_AUDIO, &audio_count);
    if (ret != ESP_EXTRACTOR_ERR_OK) {
        if (!silent) {
            ESP_LOGE(TAG, "get audio stream count failed: ret=%d", ret);
            return ESP_ERR_NOT_SUPPORTED;
        }
        audio_count = 0U;
    }
    ESP_LOGI(TAG, "media streams: video=%u audio=%u", video_count, audio_count);
    if (video_count == 0U || (!silent && audio_count == 0U)) {
        ESP_LOGE(TAG, "media validation failed: MP4 must contain both video and audio streams");
        return ESP_ERR_NOT_SUPPORTED;
    }
    ret = esp_extractor_get_stream_info(extractor, ESP_EXTRACTOR_STREAM_TYPE_VIDEO, 0U, video);
    if (ret != ESP_EXTRACTOR_ERR_OK) {
        ESP_LOGE(TAG, "get video stream info failed: ret=%d", ret);
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (audio_count > 0U) {
        ret = esp_extractor_get_stream_info(extractor, ESP_EXTRACTOR_STREAM_TYPE_AUDIO, 0U, audio);
        if (ret != ESP_EXTRACTOR_ERR_OK && !silent) {
            ESP_LOGE(TAG, "get audio stream info failed: ret=%d", ret);
            return ESP_ERR_NOT_SUPPORTED;
        }
    }
    return stream_is_supported(video, audio_count > 0U && ret == ESP_EXTRACTOR_ERR_OK ? audio : NULL,
                               silent) ? ESP_OK : ESP_ERR_NOT_SUPPORTED;
}

static void set_error_from_task(esp_err_t error)
{
    if (!lock_service(pdMS_TO_TICKS(VIDEO_PLAYER_LOCK_WAIT_MS))) return;
    s_service.last_error = error;
    (void)video_player_state_apply(&s_service.state, VIDEO_PLAYER_EVENT_ERROR);
    unlock_service();
}

static bool task_should_stop_or_pause(bool *paused)
{
    bool stop = true;
    if (lock_service(pdMS_TO_TICKS(VIDEO_PLAYER_LOCK_WAIT_MS))) {
        stop = s_service.stop_requested || !s_service.entered;
        if (paused != NULL) *paused = s_service.paused;
        unlock_service();
    }
    return stop;
}

static const char *mjpeg_decoder_name(uint32_t codec_cc)
{
    return codec_cc == ESP_VIDEO_DEC_HW_MJPEG_TAG ? "hardware" : "software";
}

/**
 * @brief Decoded resolution of one MJPEG frame on the hardware engine.
 * @param width Original frame width in pixels.
 * @param height Original frame height in pixels.
 * @return Resolution aligned for the video decoder.
 */
static esp_video_codec_resolution_t mjpeg_padded_resolution(uint32_t width, uint32_t height)
{
    uint16_t padded_width = width > UINT16_MAX ? UINT16_MAX : (uint16_t)width;
    uint16_t padded_height = height > UINT16_MAX ? UINT16_MAX : (uint16_t)height;
    video_player_align_resolution(&padded_width, &padded_height);
    return (esp_video_codec_resolution_t) {
        .width = padded_width,
        .height = padded_height,
    };
}

/**
 * @brief Bytes one decoded MJPEG frame needs, measured on the 16-padded resolution.
 * @param format Output pixel format.
 * @param width Original frame width in pixels.
 * @param height Original frame height in pixels.
 * @return Required image buffer size in bytes.
 */
static uint32_t mjpeg_frame_bytes(esp_video_codec_pixel_fmt_t format, uint32_t width, uint32_t height)
{
    esp_video_codec_resolution_t padded = mjpeg_padded_resolution(width, height);
    return esp_video_codec_get_image_size(format, &padded);
}

static esp_err_t open_video_decoder(const esp_extractor_stream_info_t *stream, uint32_t capacity,
                                    esp_video_dec_handle_t *decoder)
{
    esp_video_dec_cfg_t config = {
        .codec_type = ESP_VIDEO_CODEC_TYPE_MJPEG,
        .codec_cc = ESP_VIDEO_DEC_HW_MJPEG_TAG,
        .out_fmt = ESP_VIDEO_CODEC_PIXEL_FMT_RGB565_LE,
    };

    /** Select the hardware JPEG decoder. */
    esp_vc_err_t open_ret = esp_video_dec_open(&config, decoder);
#if CONFIG_VIDEO_DECODER_SW_MJPEG_SUPPORT
    if (open_ret != ESP_VC_ERR_OK) {
        ESP_LOGW(TAG, "open hardware MJPEG decoder failed: ret=%d; trying software fallback", open_ret);
        config.codec_cc = ESP_VIDEO_DEC_SW_MJPEG_TAG;
        open_ret = esp_video_dec_open(&config, decoder);
    }
#endif
    if (open_ret != ESP_VC_ERR_OK) {
        ESP_LOGE(TAG, "open MJPEG decoder failed: ret=%d preferred=hardware selected=%s codec_cc=0x%08" PRIx32,
                 open_ret, mjpeg_decoder_name(config.codec_cc), config.codec_cc);
        return ESP_FAIL;
    }

    uint8_t input_align = 0U;
    uint8_t output_align = 0U;
    const esp_vc_err_t align_ret = esp_video_dec_get_frame_align(*decoder, &input_align, &output_align);
    if (align_ret != ESP_VC_ERR_OK) {
        ESP_LOGE(TAG, "get %s MJPEG frame alignment failed: ret=%d", mjpeg_decoder_name(config.codec_cc), align_ret);
        (void)esp_video_dec_close(*decoder);
        *decoder = NULL;
        return ESP_FAIL;
    }

    /** The decoder writes straight into the caller-owned display buffers, so nothing is allocated here: only the buffer size requirement is checked. */
    const esp_video_codec_resolution_t padded = mjpeg_padded_resolution(stream->video_info.width,
                                                                        stream->video_info.height);
    const uint32_t requested = mjpeg_frame_bytes(config.out_fmt, stream->video_info.width,
                                                stream->video_info.height);
    if (requested == 0U || capacity < requested) {
        ESP_LOGE(TAG, "display buffers too small for %ux%u (16-padded %" PRIu32 "x%" PRIu32
                 "): need=%" PRIu32 " capacity=%" PRIu32,
                 stream->video_info.width, stream->video_info.height,
                 padded.width, padded.height, requested, capacity);
        (void)esp_video_dec_close(*decoder);
        *decoder = NULL;
        return ESP_ERR_INVALID_SIZE;
    }
    ESP_LOGI(TAG, "%s MJPEG decoder ready: input_align=%u output_align=%u frame=%ux%u"
             " (16-padded %" PRIu32 "x%" PRIu32 ") frame_bytes=%" PRIu32
             " buffer_capacity=%" PRIu32 " direct_to_display_buffer=1",
             mjpeg_decoder_name(config.codec_cc), input_align, output_align,
             stream->video_info.width, stream->video_info.height,
             padded.width, padded.height, requested, capacity);
    return ESP_OK;
}

static esp_err_t open_audio_decoder(const esp_extractor_stream_info_t *stream,
                                    esp_audio_dec_handle_t *decoder)
{
    if (stream == NULL || decoder == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_aac_dec_cfg_t aac = ESP_AAC_DEC_CONFIG_DEFAULT();
    esp_audio_dec_cfg_t config = {0};
    switch (stream->audio_info.format) {
    case ESP_EXTRACTOR_AUDIO_FORMAT_AAC:
        /** The MP4 extractor adds ADTS headers using AudioSpecificConfig. */
        aac.no_adts_header = false;
        aac.sample_rate = (int32_t)stream->audio_info.sample_rate;
        aac.channel = stream->audio_info.channel;
        aac.bits_per_sample = stream->audio_info.bits_per_sample == 0U ? ESP_AUDIO_BIT16 : stream->audio_info.bits_per_sample;
        config.type = ESP_AUDIO_TYPE_AAC;
        config.cfg = &aac;
        config.cfg_sz = sizeof(aac);
        break;
    case ESP_EXTRACTOR_AUDIO_FORMAT_MP3:
        /** MP3 reads its format from the frame header; no decoder config. */
        config.type = ESP_AUDIO_TYPE_MP3;
        break;
    default:
        return ESP_ERR_NOT_SUPPORTED;
    }
    const esp_audio_err_t ret = esp_audio_dec_open(&config, decoder);
    if (ret != ESP_AUDIO_ERR_OK) {
        ESP_LOGE(TAG, "open %s decoder failed: ret=%d rate=%" PRIu32 "Hz ch=%u",
                 extractor_format_name(stream->audio_info.format), ret,
                 stream->audio_info.sample_rate, stream->audio_info.channel);
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "%s decoder ready: rate=%" PRIu32 "Hz ch=%u",
             extractor_format_name(stream->audio_info.format),
             stream->audio_info.sample_rate, stream->audio_info.channel);
    return ESP_OK;
}

static esp_err_t configure_audio_output(const esp_extractor_stream_info_t *stream)
{
    esp_codec_dev_sample_info_t format = {
        .bits_per_sample = ESP_AUDIO_BIT16,
        /** Keep both physical DAC slots active, including for mono MP3. */
        .channel = ESP_AUDIO_DUAL,
        .sample_rate = stream->audio_info.sample_rate,
    };
    const esp_err_t ret = audio_output_reconfigure(&format);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "configure audio output failed: ret=%s rate=%" PRIu32 "Hz ch=%u bits=%u",
                 esp_err_to_name(ret), format.sample_rate, format.channel, format.bits_per_sample);
    } else {
        ESP_LOGI(TAG, "audio output configured: rate=%" PRIu32 "Hz ch=%u bits=%u",
                 format.sample_rate, format.channel, format.bits_per_sample);
    }
    return ret;
}

static void restore_audio_output(void)
{
    const esp_codec_dev_sample_info_t default_format = {
        .bits_per_sample = ESP_AUDIO_BIT16,
        .channel = ESP_AUDIO_DUAL,
        .sample_rate = 48000U,
    };
    (void)audio_output_reconfigure(&default_format);
}

/**
 * @brief Expand backwards so mono PCM can share the decoder buffer with stereo PCM.
 * @param [in,out] pcm PCM buffer; may be reallocated.
 * @param [in,out] capacity Buffer capacity in bytes; updated on reallocation.
 * @param decoded_size Number of decoded bytes in the buffer.
 * @param channels Number of decoded channels (one or two).
 * @param [out] write_size Receives the output byte count.
 * @return ESP_OK on success, or an error for an unsupported format, invalid size or allocation failure.
 */
static esp_err_t prepare_audio_pcm(uint8_t **pcm, uint32_t *capacity,
                                   uint32_t decoded_size, uint8_t channels,
                                   uint32_t *write_size)
{
    if (channels != 1U && channels != 2U) return ESP_ERR_NOT_SUPPORTED;
    if (decoded_size > *capacity || decoded_size % (channels * sizeof(int16_t)) != 0U) {
        return ESP_ERR_INVALID_SIZE;
    }
    if (decoded_size > INT32_MAX / 2U) return ESP_ERR_INVALID_SIZE;
    *write_size = channels == 1U ? decoded_size * 2U : decoded_size;
    if (*write_size > *capacity) {
        uint8_t *grown = heap_caps_realloc(*pcm, *write_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (grown == NULL) return ESP_ERR_NO_MEM;
        *pcm = grown;
        *capacity = *write_size;
    }
    if (channels == 1U) {
        int16_t *samples = (int16_t *)*pcm;
        for (uint32_t i = decoded_size / sizeof(int16_t); i > 0U; --i) {
            const int16_t sample = samples[i - 1U];
            samples[(i - 1U) * 2U] = sample;
            samples[(i - 1U) * 2U + 1U] = sample;
        }
    }
    return ESP_OK;
}

static esp_err_t decode_audio(esp_audio_dec_handle_t decoder, esp_extractor_frame_info_t *frame,
                              uint8_t **pcm, uint32_t *pcm_capacity, bool *dropped, uint32_t *written)
{
    if (dropped != NULL) {
        *dropped = false;
    }
    *written = 0U;
    esp_audio_dec_in_raw_t raw = {.buffer = frame->frame_buffer, .len = frame->frame_size};
    while (raw.len > 0U) {
        esp_audio_dec_out_frame_t output = {.buffer = *pcm, .len = *pcm_capacity};
        const esp_audio_err_t result = esp_audio_dec_process(decoder, &raw, &output);
        if (result == ESP_AUDIO_ERR_BUFF_NOT_ENOUGH) {
            uint8_t *grown = heap_caps_realloc(*pcm, output.needed_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            if (grown == NULL) {
                ESP_LOGE(TAG, "audio output buffer allocation failed: needed=%" PRIu32,
                         output.needed_size);
                return ESP_ERR_NO_MEM;
            }
            *pcm = grown;
            *pcm_capacity = output.needed_size;
            continue;
        }
        if (result != ESP_AUDIO_ERR_OK) {
            /** A malformed audio access unit must not stop the A/V session. */
            if (result == ESP_AUDIO_ERR_MEM_LACK ||
                result == ESP_AUDIO_ERR_INVALID_PARAMETER ||
                result == ESP_AUDIO_ERR_NOT_SUPPORT) {
                ESP_LOGE(TAG, "audio decoder fatal error: ret=%d input_bytes=%" PRIu32
                         " consumed=%" PRIu32 " pts=%" PRIu32,
                         result, frame->frame_size, raw.consumed, frame->pts);
                return result == ESP_AUDIO_ERR_MEM_LACK ? ESP_ERR_NO_MEM : ESP_FAIL;
            }
            ESP_LOGW(TAG, "drop invalid audio frame: ret=%d input_bytes=%" PRIu32
                     " consumed=%" PRIu32 " pts=%" PRIu32,
                     result, frame->frame_size, raw.consumed, frame->pts);
            const esp_audio_err_t reset_result = esp_audio_dec_reset(decoder);
            if (reset_result != ESP_AUDIO_ERR_OK) {
                ESP_LOGW(TAG, "audio decoder reset after dropped frame failed: ret=%d", reset_result);
            }
            if (dropped != NULL) {
                *dropped = true;
            }
            return ESP_OK;
        }
        if (output.decoded_size > 0U) {
            esp_audio_dec_info_t info = {0};
            if (esp_audio_dec_get_info(decoder, &info) != ESP_AUDIO_ERR_OK || info.bits_per_sample != 16U) {
                ESP_LOGE(TAG, "invalid decoded PCM format");
                return ESP_ERR_NOT_SUPPORTED;
            }
            uint32_t write_size = 0U;
            const esp_err_t prepared = prepare_audio_pcm(pcm, pcm_capacity, output.decoded_size,
                                                         info.channel, &write_size);
            if (prepared != ESP_OK) return prepared;
            const int write_ret = esp_codec_dev_write(audio_output_get_codec_dev(), *pcm, (int)write_size);
            if (write_ret != ESP_CODEC_DEV_OK) {
                ESP_LOGE(TAG, "write decoded audio PCM failed: ret=%d bytes=%" PRIu32, write_ret, output.decoded_size);
                return ESP_FAIL;
            }
            *written += write_size;
        }
        if (raw.consumed == 0U) {
            ESP_LOGW(TAG, "drop audio frame with no decoder progress: input_bytes=%" PRIu32 " pts=%" PRIu32,
                     frame->frame_size, frame->pts);
            (void)esp_audio_dec_reset(decoder);
            if (dropped != NULL) {
                *dropped = true;
            }
            return ESP_OK;
        }
        raw.buffer += raw.consumed;
        raw.len -= raw.consumed;
    }
    return ESP_OK;
}

/**
 * @brief Decode one MJPEG access unit straight into display buffer `buffer_index`.
 * @param decoder Video decoder handle.
 * @param data Encoded MJPEG access unit.
 * @param size Access unit size in bytes.
 * @param pts Presentation timestamp for the frame.
 * @param buffer_index Index of the target display buffer.
 * @return ESP_OK on success, otherwise a decode or buffer error.
 */
static esp_err_t decode_video(esp_video_dec_handle_t decoder, const uint8_t *data,
                              uint32_t size, uint32_t pts, uint8_t buffer_index)
{
    uint8_t *target = NULL;
    uint32_t capacity = 0U;
    video_player_frame_cb_t callback = NULL;
    void *context = NULL;
    if (lock_service(pdMS_TO_TICKS(VIDEO_PLAYER_LOCK_WAIT_MS))) {
        target = s_service.output_buffers[buffer_index];
        capacity = s_service.output_capacity;
        callback = s_service.frame_cb;
        context = s_service.frame_context;
        unlock_service();
    }
    if (target == NULL || capacity == 0U) {
        ESP_LOGE(TAG, "decode without a registered display buffer: index=%u", buffer_index);
        return ESP_ERR_INVALID_STATE;
    }

    esp_video_dec_in_frame_t input = {
        .pts = pts,
        .dts = pts,
        .data = (uint8_t *)data,
        .size = size,
    };
    esp_video_dec_out_frame_t output = {.data = target, .size = capacity};
    esp_vc_err_t result = esp_video_dec_process(decoder, &input, &output);
    if (result == ESP_VC_ERR_BUF_NOT_ENOUGH) {
        /** The panel-fixed buffers are the only buffers the decoder gets, so a frame that still does not fit is a resolution error, not a retry. */
        ESP_LOGE(TAG, "MJPEG frame exceeds the fixed display buffer: capacity=%" PRIu32
                 " needed=%" PRIu32 " (the buffer must hold the 16-padded frame,"
                 " not the visible widthxheight)", capacity, output.decoded_size);
        return ESP_ERR_INVALID_SIZE;
    }
    if (result != ESP_VC_ERR_OK || output.decoded_size == 0U) {
        ESP_LOGE(TAG, "MJPEG decode failed: ret=%d input_bytes=%" PRIu32 " consumed=%" PRIu32 " output_bytes=%" PRIu32,
                 result, size, input.consumed, output.decoded_size);
        return ESP_FAIL;
    }

    esp_video_codec_frame_info_t info = {0};
    if (esp_video_dec_get_frame_info(decoder, &info) != ESP_VC_ERR_OK) return ESP_FAIL;
    if (lock_service(pdMS_TO_TICKS(VIDEO_PLAYER_LOCK_WAIT_MS))) {
        s_service.state.position_ms = pts;
        unlock_service();
    }
    if (callback == NULL) {
        return ESP_OK;
    }
    /** Reserve before publishing: the view may consume and release the frame on the other core before callback returns. Never overwrite that release. */
    if (!lock_service(pdMS_TO_TICKS(VIDEO_PLAYER_LOCK_WAIT_MS))) return ESP_ERR_TIMEOUT;
    s_service.output_held_by_view[buffer_index] = true;
    unlock_service();
    if (!callback(buffer_index, (uint16_t)info.res.width, (uint16_t)info.res.height,
                  info.res.width * 2U, pts, context)) {
        video_player_service_release_output_buffer(buffer_index);
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

/**
 * @brief Pick the next display buffer the view does not own, or -1 when both are busy.
 * @return An available buffer index, or -1 if both buffers are busy.
 */
static int acquire_output_buffer_locked(void)
{
    for (uint8_t step = 0U; step < 2U; ++step) {
        const uint8_t index = (uint8_t)((s_service.output_next + step) % 2U);
        if (s_service.output_buffers[index] != NULL && !s_service.output_held_by_view[index]) {
            s_service.output_next = (uint8_t)((index + 1U) % 2U);
            return (int)index;
        }
    }
    return -1;
}

static void free_video_packet(video_player_video_packet_t *packet)
{
    if (packet != NULL && packet->data != NULL) {
        heap_caps_free(packet->data);
        packet->data = NULL;
    }
}

static void signal_playback_error(esp_err_t error)
{
    if (!lock_service(pdMS_TO_TICKS(VIDEO_PLAYER_LOCK_WAIT_MS))) {
        return;
    }
    s_service.stop_requested = true;
    s_service.last_error = error;
    (void)video_player_state_apply(&s_service.state, VIDEO_PLAYER_EVENT_ERROR);
    unlock_service();
}

static void video_decode_task(void *argument)
{
    (void)argument;
    esp_video_dec_handle_t decoder = NULL;
    QueueHandle_t queue = NULL;
    if (lock_service(pdMS_TO_TICKS(VIDEO_PLAYER_LOCK_WAIT_MS))) {
        decoder = s_service.video_decoder;
        queue = s_service.video_queue;
        unlock_service();
    }
    ESP_LOGI(TAG, "video decode task started on core %d", xPortGetCoreID());

    esp_err_t result = decoder != NULL && queue != NULL ? ESP_OK : ESP_ERR_INVALID_STATE;
    while (result == ESP_OK && !task_should_stop_or_pause(NULL)) {
        video_player_video_packet_t packet = {0};
        if (xQueueReceive(queue, &packet, pdMS_TO_TICKS(VIDEO_PLAYER_VIDEO_QUEUE_WAIT_MS)) != pdTRUE) {
            continue;
        }
        if (packet.end_of_stream) {
            break;
        }
        uint8_t buffer_index = 0U;
        bool have_buffer = false;
        if (lock_service(pdMS_TO_TICKS(VIDEO_PLAYER_LOCK_WAIT_MS))) {
            const int slot = acquire_output_buffer_locked();
            if (slot >= 0) {
                buffer_index = (uint8_t)slot;
                have_buffer = true;
            }
            unlock_service();
        }
        const esp_err_t decode_result = have_buffer
                                            ? decode_video(decoder, packet.data, packet.size,
                                                           packet.pts, buffer_index)
                                            : ESP_OK;
        free_video_packet(&packet);
        if (!have_buffer) {
            /** Drop the video frame when both output buffers remain owned by the view. */
            if (lock_service(pdMS_TO_TICKS(VIDEO_PLAYER_LOCK_WAIT_MS))) {
                ++s_service.video_frames_dropped;
                unlock_service();
            }
            ESP_LOGD(TAG, "drop video frame: no free display buffer pts=%" PRIu32, packet.pts);
            continue;
        }
        if (decode_result == ESP_OK) {
            if (lock_service(pdMS_TO_TICKS(VIDEO_PLAYER_LOCK_WAIT_MS))) {
                ++s_service.video_frames_decoded;
                unlock_service();
            }
            continue;
        }
        if (decode_result == ESP_ERR_INVALID_STATE || decode_result == ESP_ERR_INVALID_SIZE) {
            ESP_LOGE(TAG, "video decode worker cannot continue: pts=%" PRIu32 " error=%s",
                     packet.pts, esp_err_to_name(decode_result));
            signal_playback_error(decode_result);
            result = decode_result;
            break;
        }
        if (decode_result == ESP_ERR_TIMEOUT) {
            if (lock_service(pdMS_TO_TICKS(VIDEO_PLAYER_LOCK_WAIT_MS))) {
                ++s_service.video_frames_dropped;
                unlock_service();
            }
            ESP_LOGD(TAG, "drop decoded frame: the view did not take it pts=%" PRIu32, packet.pts);
            continue;
        }

        /** Discard unsupported MJPEG frames while continuing audio playback. */
        if (lock_service(pdMS_TO_TICKS(VIDEO_PLAYER_LOCK_WAIT_MS))) {
            ++s_service.video_frames_dropped;
            unlock_service();
        }
        ESP_LOGW(TAG, "drop incompatible MJPEG frame: pts=%" PRIu32 " size=%" PRIu32 " error=%s",
                 packet.pts, packet.size, esp_err_to_name(decode_result));
    }

    if (decoder != NULL) {
        (void)esp_video_dec_close(decoder);
    }
    if (lock_service(pdMS_TO_TICKS(VIDEO_PLAYER_LOCK_WAIT_MS))) {
        s_service.video_decoder = NULL;
        s_service.video_task = NULL;
        unlock_service();
    }
    ESP_LOGI(TAG, "video decode task stopped on core %d: %s", xPortGetCoreID(), esp_err_to_name(result));
    vTaskDelete(NULL);
}

static esp_err_t start_video_decode_task(esp_video_dec_handle_t decoder)
{
    QueueHandle_t queue = xQueueCreate(VIDEO_PLAYER_VIDEO_QUEUE_LENGTH, sizeof(video_player_video_packet_t));
    if (queue == NULL) {
        ESP_LOGE(TAG, "create video packet queue failed");
        return ESP_ERR_NO_MEM;
    }
    s_service.video_queue = queue;
    s_service.video_decoder = decoder;
    const BaseType_t created = xTaskCreatePinnedToCore(video_decode_task, "video_decode",
                                                        VIDEO_PLAYER_VIDEO_TASK_STACK, NULL,
                                                        VIDEO_PLAYER_TASK_PRIORITY, &s_service.video_task,
                                                        VIDEO_PLAYER_VIDEO_CORE);
    if (created != pdPASS) {
        ESP_LOGE(TAG, "create video decode task failed: core=%u stack=%u", VIDEO_PLAYER_VIDEO_CORE,
                 VIDEO_PLAYER_VIDEO_TASK_STACK);
        s_service.video_queue = NULL;
        s_service.video_decoder = NULL;
        vQueueDelete(queue);
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "video decoder scheduled on core %u; audio demux/decode uses core %u",
             VIDEO_PLAYER_VIDEO_CORE, VIDEO_PLAYER_AUDIO_CORE);
    return ESP_OK;
}

static esp_err_t queue_video_frame(const esp_extractor_frame_info_t *frame)
{
    if (frame == NULL || frame->frame_buffer == NULL || frame->frame_size == 0U) {
        return ESP_ERR_INVALID_ARG;
    }
    QueueHandle_t queue = NULL;
    if (lock_service(pdMS_TO_TICKS(VIDEO_PLAYER_LOCK_WAIT_MS))) {
        queue = s_service.video_queue;
        unlock_service();
    }
    if (queue == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    video_player_video_packet_t packet = {
        .size = frame->frame_size,
        .pts = frame->pts,
    };
    packet.data = heap_caps_malloc(packet.size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (packet.data == NULL) {
        packet.data = heap_caps_malloc(packet.size, MALLOC_CAP_8BIT);
    }
    if (packet.data == NULL) {
        ESP_LOGW(TAG, "drop video frame: no memory size=%" PRIu32 " pts=%" PRIu32, packet.size, packet.pts);
        return ESP_OK;
    }
    memcpy(packet.data, frame->frame_buffer, packet.size);
    if (xQueueSend(queue, &packet, 0) != pdTRUE) {
        free_video_packet(&packet);
        ESP_LOGD(TAG, "drop video frame: queue full pts=%" PRIu32, frame->pts);
    }
    return ESP_OK;
}

static void stop_video_decode_task(void)
{
    QueueHandle_t queue = NULL;
    bool task_running = false;
    if (lock_service(pdMS_TO_TICKS(VIDEO_PLAYER_LOCK_WAIT_MS))) {
        queue = s_service.video_queue;
        task_running = s_service.video_task != NULL;
        unlock_service();
    }
    if (queue == NULL) {
        return;
    }

    /** The worker can already have exited after dropping an incompatible frame. */
    if (!task_running) {
        video_player_video_packet_t pending = {0};
        size_t released = 0U;
        while (xQueueReceive(queue, &pending, 0) == pdTRUE) {
            free_video_packet(&pending);
            ++released;
        }
        if (lock_service(pdMS_TO_TICKS(VIDEO_PLAYER_LOCK_WAIT_MS))) {
            if (s_service.video_queue == queue && s_service.video_task == NULL) {
                s_service.video_queue = NULL;
                vQueueDelete(queue);
            }
            unlock_service();
        }
        if (released > 0U) {
            ESP_LOGD(TAG, "released %u queued video packets after worker exit", (unsigned)released);
        }
        return;
    }

    video_player_video_packet_t end = {.end_of_stream = true};
    while (xQueueSend(queue, &end, pdMS_TO_TICKS(VIDEO_PLAYER_VIDEO_QUEUE_WAIT_MS)) != pdTRUE) {
        if (task_should_stop_or_pause(NULL)) {
            break;
        }
    }
    const TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(VIDEO_PLAYER_VIDEO_JOIN_WAIT_MS);
    while (true) {
        bool running = false;
        if (lock_service(pdMS_TO_TICKS(VIDEO_PLAYER_LOCK_WAIT_MS))) {
            running = s_service.video_task != NULL;
            unlock_service();
        }
        if (!running || (int32_t)(xTaskGetTickCount() - deadline) >= 0) {
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(VIDEO_PLAYER_VIDEO_QUEUE_WAIT_MS));
    }
    if (lock_service(pdMS_TO_TICKS(VIDEO_PLAYER_LOCK_WAIT_MS))) {
        if (s_service.video_task == NULL && s_service.video_queue == queue) {
            video_player_video_packet_t pending = {0};
            size_t released = 0U;
            while (xQueueReceive(queue, &pending, 0) == pdTRUE) {
                free_video_packet(&pending);
                ++released;
            }
            if (released > 0U) {
                ESP_LOGD(TAG, "released %u queued video packets during shutdown", (unsigned)released);
            }
            s_service.video_queue = NULL;
            vQueueDelete(queue);
        } else if (s_service.video_task != NULL) {
            ESP_LOGW(TAG, "video decode task did not stop before join timeout");
        }
        unlock_service();
    }
}

static void playback_task(void *argument)
{
    (void)argument;
    ESP_LOGI(TAG, "audio demux/decode task started on core %d", xPortGetCoreID());
    char path[VIDEO_PLAYER_PATH_MAX] = {0};
    video_file_source_t source = {0};
    esp_extractor_config_t config = {0};
    esp_extractor_handle_t extractor = NULL;
    esp_extractor_stream_info_t video_info = {0};
    esp_extractor_stream_info_t audio_info = {0};
    esp_video_dec_handle_t video_decoder = NULL;
    esp_audio_dec_handle_t audio_decoder = NULL;
    uint8_t *pcm_buffer = NULL;
    uint32_t output_capacity = 0U;
    uint16_t limit_width = 0U;
    uint16_t limit_height = 0U;
    bool video_worker_started = false;
    uint32_t pcm_capacity = VIDEO_PLAYER_AUDIO_BUFFER_SIZE;
    uint64_t pcm_written = 0U;
    int64_t next_report_us = 0;
    int64_t silent_start_us = 0;
    uint32_t silent_first_pts = 0U;
    bool silent = false;
    esp_err_t result = ESP_OK;

    if (!lock_service(pdMS_TO_TICKS(VIDEO_PLAYER_LOCK_WAIT_MS))) {
        result = ESP_ERR_TIMEOUT;
        goto finish;
    }
    if (s_service.selected >= s_service.count) {
        unlock_service();
        result = ESP_ERR_INVALID_STATE;
        goto finish;
    }
    (void)snprintf(path, sizeof(path), "%s", s_service.entries[s_service.selected].path);
    output_capacity = s_service.output_capacity;
    limit_width = s_service.display_width;
    limit_height = s_service.display_height;
    silent = s_service.silent;
    unlock_service();

    result = open_extractor(path, &source, &config, &extractor);
    if (result == ESP_OK) result = read_stream_info(extractor, &video_info, &audio_info, silent);
    /** Validate frame dimensions after alignment to 16-pixel JPEG blocks. */
    if (result == ESP_OK) {
        const esp_video_codec_resolution_t padded = mjpeg_padded_resolution(video_info.video_info.width,
                                                                            video_info.video_info.height);
        if (!video_player_dimensions_supported((uint16_t)padded.width, (uint16_t)padded.height,
                                               limit_width, limit_height)) {
            ESP_LOGE(TAG, "media validation failed: video %ux%u (16-padded %" PRIu32 "x%" PRIu32
                     ") does not fit the fixed panel resolution %ux%u",
                     video_info.video_info.width, video_info.video_info.height,
                     padded.width, padded.height, limit_width, limit_height);
            result = ESP_ERR_NOT_SUPPORTED;
        }
    }
    if (result == ESP_OK && !silent) result = configure_audio_output(&audio_info);
    if (result == ESP_OK) result = open_video_decoder(&video_info, output_capacity, &video_decoder);
    if (result == ESP_OK && !silent) result = open_audio_decoder(&audio_info, &audio_decoder);
    if (result == ESP_OK && !silent) {
        pcm_buffer = heap_caps_malloc(pcm_capacity, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (pcm_buffer == NULL) pcm_buffer = heap_caps_malloc(pcm_capacity, MALLOC_CAP_8BIT);
        if (pcm_buffer == NULL) result = ESP_ERR_NO_MEM;
    }
    if (result == ESP_OK) {
        result = start_video_decode_task(video_decoder);
        if (result == ESP_OK) {
            video_worker_started = true;
            video_decoder = NULL;
        }
    }
    if (result != ESP_OK) goto finish;

    if (lock_service(pdMS_TO_TICKS(VIDEO_PLAYER_LOCK_WAIT_MS))) {
        s_service.width = video_info.video_info.width;
        s_service.height = video_info.video_info.height;
        s_service.fps = video_info.video_info.fps;
        s_service.state.duration_ms = video_info.duration;
        s_service.last_error = ESP_OK;
        (void)video_player_state_apply(&s_service.state, VIDEO_PLAYER_EVENT_PLAY);
        unlock_service();
    }

    while (!task_should_stop_or_pause(NULL)) {
        bool paused = false;
        if (task_should_stop_or_pause(&paused)) {
            break;
        }
        if (paused) {
            vTaskDelay(pdMS_TO_TICKS(20U));
            continue;
        }
        esp_extractor_frame_info_t frame = {0};
        const esp_extractor_err_t extracted = esp_extractor_read_frame(extractor, &frame);
        if (extracted == ESP_EXTRACTOR_ERR_EOS) {
            if (lock_service(pdMS_TO_TICKS(VIDEO_PLAYER_LOCK_WAIT_MS))) {
                (void)video_player_state_apply(&s_service.state, VIDEO_PLAYER_EVENT_EOS);
                unlock_service();
            }
            break;
        }
        if (extracted != ESP_EXTRACTOR_ERR_OK) {
            ESP_LOGE(TAG, "read MP4 frame failed: ret=%d", extracted);
            result = ESP_FAIL;
            break;
        }
        ESP_LOGD(TAG, "frame: type=%d stream=%u pts=%" PRIu32 " size=%" PRIu32,
                 frame.stream_type, frame.stream_idx, frame.pts, frame.frame_size);
        if (frame.stream_type == ESP_EXTRACTOR_STREAM_TYPE_VIDEO) {
            if (silent) {
                if (silent_start_us == 0) {
                    silent_start_us = esp_timer_get_time();
                    silent_first_pts = frame.pts;
                }
                const int64_t due_us = silent_start_us +
                    (int64_t)(frame.pts - silent_first_pts) * 1000;
                while (esp_timer_get_time() < due_us && !task_should_stop_or_pause(NULL)) {
                    const int64_t remaining_us = due_us - esp_timer_get_time();
                    vTaskDelay(remaining_us > 20000 ? pdMS_TO_TICKS(20U) : 1U);
                }
            }
            result = queue_video_frame(&frame);
        } else if (frame.stream_type == ESP_EXTRACTOR_STREAM_TYPE_AUDIO && !silent) {
            bool audio_dropped = false;
            uint32_t written = 0U;
            result = decode_audio(audio_decoder, &frame, &pcm_buffer, &pcm_capacity, &audio_dropped, &written);
            if (pcm_written == 0U && written > 0U) {
                ESP_LOGI(TAG, "first audio PCM written: bytes=%" PRIu32 " output=stereo pts=%" PRIu32,
                         written, frame.pts);
            }
            pcm_written += written;
            if (result == ESP_OK && lock_service(pdMS_TO_TICKS(VIDEO_PLAYER_LOCK_WAIT_MS))) {
                if (written > 0U) s_service.state.position_ms = frame.pts;
                if (audio_dropped) {
                    ++s_service.audio_frames_dropped;
                } else if (written > 0U) {
                    ++s_service.audio_frames_decoded;
                }
                unlock_service();
            }
        }
        (void)esp_extractor_release_frame(extractor, &frame);
        const int64_t now_us = esp_timer_get_time();
        if (now_us >= next_report_us && lock_service(pdMS_TO_TICKS(VIDEO_PLAYER_LOCK_WAIT_MS))) {
            ESP_LOGI(TAG, "playback output: video=%" PRIu32 " dropped=%" PRIu32
                     " audio=%" PRIu32 " dropped=%" PRIu32 " pcm_bytes=%" PRIu64 " held=%d/%d",
                     s_service.video_frames_decoded, s_service.video_frames_dropped,
                     s_service.audio_frames_decoded, s_service.audio_frames_dropped, pcm_written,
                     s_service.output_held_by_view[0], s_service.output_held_by_view[1]);
            unlock_service();
            next_report_us = now_us + 2000000;
        }
        if (result != ESP_OK) {
            ESP_LOGE(TAG, "frame processing failed: type=%d pts=%" PRIu32 " result=%s",
                     frame.stream_type, frame.pts, esp_err_to_name(result));
            break;
        }
    }

finish:
    if (result != ESP_OK) {
        ESP_LOGE(TAG, "VideoPlayer task stopped with error: %s (%d)", esp_err_to_name(result), result);
        set_error_from_task(result);
    } else if (lock_service(pdMS_TO_TICKS(VIDEO_PLAYER_LOCK_WAIT_MS))) {
        if (s_service.stop_requested && s_service.state.name != NULL) {
            (void)video_player_state_apply(&s_service.state, VIDEO_PLAYER_EVENT_STOP);
        }
        unlock_service();
    }
    if (video_worker_started) {
        stop_video_decode_task();
    }
    if (audio_decoder != NULL) esp_audio_dec_close(audio_decoder);
    if (video_decoder != NULL) (void)esp_video_dec_close(video_decoder);
    if (pcm_buffer != NULL) heap_caps_free(pcm_buffer);
    close_extractor(extractor, &source);
    uint32_t decoded_frames = 0U;
    uint32_t dropped_frames = 0U;
    uint32_t audio_decoded_frames = 0U;
    uint32_t audio_dropped_frames = 0U;
    if (lock_service(pdMS_TO_TICKS(VIDEO_PLAYER_LOCK_WAIT_MS))) {
        decoded_frames = s_service.video_frames_decoded;
        dropped_frames = s_service.video_frames_dropped;
        audio_decoded_frames = s_service.audio_frames_decoded;
        audio_dropped_frames = s_service.audio_frames_dropped;
        unlock_service();
    }
    ESP_LOGI(TAG, "audio demux/decode task cleanup on core %d: result=%s video_decoded=%" PRIu32
             " video_dropped=%" PRIu32 " audio_decoded=%" PRIu32 " audio_dropped=%" PRIu32,
             xPortGetCoreID(), esp_err_to_name(result), decoded_frames, dropped_frames,
             audio_decoded_frames, audio_dropped_frames);
    if (!silent) restore_audio_output();
    if (lock_service(pdMS_TO_TICKS(VIDEO_PLAYER_LOCK_WAIT_MS))) {
        finish_playback_locked();
        unlock_service();
    }
    vTaskDelete(NULL);
}

/**
 * @brief Scan playback sources, limiting the boot catalog to built-in assets.
 * @param include_card Whether to include the card in the scan.
 */
static void scan_catalog_locked(bool include_card)
{
    s_service.count = 0U;
    s_service.selected = 0U;
    for (size_t root = 0U; root < sizeof(s_catalog_roots) / sizeof(s_catalog_roots[0]); ++root) {
        const char *const root_path = s_catalog_roots[root];
        if (!include_card && strcmp(root_path, VIDEO_PLAYER_SDCARD_ROOT) == 0) continue;
        const int64_t scan_start_us = esp_timer_get_time();
        DIR *directory = opendir(root_path);
        if (directory == NULL) {
            ESP_LOGI(TAG, "catalog root %s unavailable after %d ms", root_path,
                     (int)((esp_timer_get_time() - scan_start_us) / 1000));
            continue;
        }
        struct dirent *entry;
        while ((entry = readdir(directory)) != NULL && s_service.count < VIDEO_PLAYER_MAX_ENTRIES) {
            if (!video_player_path_is_mp4(entry->d_name) ||
                strlen(entry->d_name) >= VIDEO_PLAYER_NAME_MAX) continue;
            video_player_entry_t *target = &s_service.entries[s_service.count];
            const int written = snprintf(target->path, sizeof(target->path), "%s/%s", root_path, entry->d_name);
            struct stat status = {0};
            if (written < 0 || (size_t)written >= sizeof(target->path) || stat(target->path, &status) != 0 ||
                !S_ISREG(status.st_mode)) continue;
            const size_t name_length = strnlen(entry->d_name, sizeof(target->name) - 1U);
            memcpy(target->name, entry->d_name, name_length);
            target->name[name_length] = '\0';
            ++s_service.count;
        }
        (void)closedir(directory);
        ESP_LOGI(TAG, "catalog root %s scanned in %d ms", root_path,
                 (int)((esp_timer_get_time() - scan_start_us) / 1000));
    }
    if (s_service.count > 0U) {
        video_player_state_set_media(&s_service.state, s_service.entries[0].name, 0U);
    }
}

size_t video_player_service_list(video_player_catalog_entry_t *entries, size_t capacity)
{
    if (entries == NULL || capacity == 0U) return 0U;
    if (capacity > VIDEO_PLAYER_MAX_ENTRIES) capacity = VIDEO_PLAYER_MAX_ENTRIES;
    size_t count = 0U;
    for (size_t root = 0U; root < sizeof(s_catalog_roots) / sizeof(s_catalog_roots[0]); ++root) {
        DIR *directory = opendir(s_catalog_roots[root]);
        if (directory == NULL) continue;
        struct dirent *item;
        while (count < capacity && (item = readdir(directory)) != NULL) {
            if (!video_player_path_is_mp4(item->d_name) ||
                strlen(item->d_name) >= VIDEO_PLAYER_NAME_MAX) continue;
            video_player_catalog_entry_t *entry = &entries[count];
            const int len = snprintf(entry->path, sizeof(entry->path), "%s/%s",
                                     s_catalog_roots[root], item->d_name);
            struct stat st = {0};
            if (len < 0 || (size_t)len >= sizeof(entry->path) ||
                stat(entry->path, &st) != 0 || !S_ISREG(st.st_mode)) continue;
            const size_t name_length = strlen(item->d_name);
            memcpy(entry->name, item->d_name, name_length + 1U);
            ++count;
        }
        (void)closedir(directory);
    }
    return count;
}

static esp_err_t enter_common(bool include_card, bool silent)
{
    if (!lock_service(pdMS_TO_TICKS(VIDEO_PLAYER_LOCK_WAIT_MS))) return ESP_ERR_TIMEOUT;
    if (s_service.entered) {
        const esp_err_t result = !silent && !s_service.silent ? ESP_OK : ESP_ERR_INVALID_STATE;
        unlock_service();
        return result;
    }
    const esp_err_t registration = register_codecs(silent);
    if (registration != ESP_OK) {
        unlock_service();
        return registration;
    }
    if (!silent) {
        const esp_err_t audio = audio_runtime_acquire();
        if (audio != ESP_OK) {
            unlock_service();
            return audio;
        }
    }
    s_service.entered = true;
    s_service.audio_runtime_acquired = !silent;
    s_service.silent = silent;
    s_service.last_error = ESP_OK;
    s_service.stop_requested = false;
    s_service.paused = false;
    s_service.switch_requested = false;
    s_service.restart_requested = false;
    s_service.width = 0U;
    s_service.height = 0U;
    s_service.fps = 0U;
    s_service.video_frames_decoded = 0U;
    s_service.video_frames_dropped = 0U;
    s_service.audio_frames_decoded = 0U;
    s_service.audio_frames_dropped = 0U;
    video_player_state_init(&s_service.state);
    scan_catalog_locked(include_card);
    ESP_LOGI(TAG, "VideoPlayer entered: assets=%s card=%s files=%u", VIDEO_PLAYER_ASSET_ROOT,
             include_card ? VIDEO_PLAYER_SDCARD_ROOT : "(scanned later)", (unsigned)s_service.count);
    if (s_service.count == 0U) {
        ESP_LOGE(TAG, "VideoPlayer enter failed: no .mp4 files found under %s%s",
                 VIDEO_PLAYER_ASSET_ROOT, include_card ? " or the SD card" : "");
    }
    unlock_service();
    return s_service.count == 0U ? ESP_ERR_NOT_FOUND : ESP_OK;
}

esp_err_t video_player_service_enter(void)
{
    return enter_common(true, false);
}

esp_err_t video_player_service_enter_assets(void)
{
    return enter_common(false, false);
}

esp_err_t video_player_service_enter_silent(void)
{
    const esp_err_t result = enter_common(true, true);
    if (result == ESP_ERR_NOT_FOUND) (void)video_player_service_exit();
    return result;
}

esp_err_t video_player_service_select_path(const char *path)
{
    if (path == NULL) return ESP_ERR_INVALID_ARG;
    if (!lock_service(pdMS_TO_TICKS(VIDEO_PLAYER_LOCK_WAIT_MS))) return ESP_ERR_TIMEOUT;
    if (!s_service.entered || s_service.task != NULL) {
        unlock_service();
        return ESP_ERR_INVALID_STATE;
    }
    for (size_t i = 0; i < s_service.count; ++i) {
        if (strcmp(s_service.entries[i].path, path) == 0) {
            s_service.selected = i;
            video_player_state_set_media(&s_service.state, s_service.entries[i].name, 0U);
            unlock_service();
            return ESP_OK;
        }
    }
    unlock_service();
    return ESP_ERR_NOT_FOUND;
}

esp_err_t video_player_service_exit(void)
{
    (void)video_player_service_stop();
    const TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(VIDEO_PLAYER_EXIT_WAIT_MS);
    while (video_player_service_is_entered()) {
        if (!lock_service(pdMS_TO_TICKS(VIDEO_PLAYER_LOCK_WAIT_MS))) return ESP_ERR_TIMEOUT;
        const bool task_running = s_service.task != NULL || s_service.video_task != NULL;
        if (!task_running) {
            const bool release_audio = s_service.audio_runtime_acquired;
            s_service.entered = false;
            s_service.audio_runtime_acquired = false;
            s_service.silent = false;
            s_service.count = 0U;
            /** The view frees its display buffers right after this call, so no stale pointer may survive the teardown. */
            s_service.output_buffers[0] = NULL;
            s_service.output_buffers[1] = NULL;
            s_service.output_capacity = 0U;
            s_service.output_held_by_view[0] = false;
            s_service.output_held_by_view[1] = false;
            s_service.output_next = 0U;
            /** The preloaded copy only exists to make one playback cheap; it goes back to the heap as soon as the player is done with it. */
            release_preload_locked();
            video_player_state_init(&s_service.state);
            unlock_service();
            return release_audio ? audio_runtime_release() : ESP_OK;
        }
        unlock_service();
        if ((int32_t)(xTaskGetTickCount() - deadline) >= 0) return ESP_ERR_TIMEOUT;
        vTaskDelay(pdMS_TO_TICKS(20U));
    }
    return ESP_OK;
}

bool video_player_service_is_entered(void)
{
    if (!lock_service(pdMS_TO_TICKS(VIDEO_PLAYER_LOCK_WAIT_MS))) return false;
    const bool entered = s_service.entered;
    unlock_service();
    return entered;
}

/**
 * @brief Start playback after the caller locks the service and checks the prerequisites.
 * @param preserve_held_buffers Whether to retain buffers still held by the view.
 * @return ESP_OK on success, otherwise a playback error.
 */
static esp_err_t start_playback_locked(bool preserve_held_buffers)
{
    /** During a switch the LVGL task may still be releasing the old frame. Keep those reservations until the view explicitly returns each buffer. */
    if (!preserve_held_buffers) {
        s_service.output_held_by_view[0] = false;
        s_service.output_held_by_view[1] = false;
        s_service.output_next = 0U;
    }
    s_service.stop_requested = false;
    s_service.paused = false;
    if (s_service.state.status == VIDEO_PLAYER_STATUS_ENDED ||
        s_service.state.status == VIDEO_PLAYER_STATUS_ERROR) {
        s_service.state.position_ms = 0U;
        s_service.state.status = VIDEO_PLAYER_STATUS_READY;
    }
    (void)video_player_state_apply(&s_service.state, VIDEO_PLAYER_EVENT_MEDIA_READY);
    ESP_LOGI(TAG, "start playback: index=%u file=%s", (unsigned)s_service.selected,
             s_service.entries[s_service.selected].path);
    const BaseType_t created = xTaskCreatePinnedToCore(playback_task, "video_audio", VIDEO_PLAYER_TASK_STACK,
                                                        NULL, VIDEO_PLAYER_TASK_PRIORITY, &s_service.task,
                                                        VIDEO_PLAYER_AUDIO_CORE);
    if (created != pdPASS) {
        ESP_LOGE(TAG, "create audio demux/decode task failed: core=%u stack=%u priority=%u",
                 VIDEO_PLAYER_AUDIO_CORE, VIDEO_PLAYER_TASK_STACK, VIDEO_PLAYER_TASK_PRIORITY);
        s_service.task = NULL;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

static void finish_playback_locked(void)
{
    s_service.task = NULL;
    s_service.stop_requested = false;
    if (s_service.restart_requested && s_service.entered) {
        s_service.restart_requested = false;
        const esp_err_t restarted = start_playback_locked(true);
        if (restarted != ESP_OK) {
            s_service.last_error = restarted;
            (void)video_player_state_apply(&s_service.state, VIDEO_PLAYER_EVENT_ERROR);
        }
        return;
    }
    if (!s_service.switch_requested || !s_service.entered) {
        s_service.switch_requested = false;
        return;
    }
    s_service.selected = s_service.switch_target;
    s_service.switch_requested = false;
    video_player_state_set_media(&s_service.state,
                                 s_service.entries[s_service.selected].name, 0U);
    s_service.width = 0U;
    s_service.height = 0U;
    s_service.fps = 0U;
    const esp_err_t switched = start_playback_locked(true);
    if (switched != ESP_OK) {
        s_service.last_error = switched;
        (void)video_player_state_apply(&s_service.state, VIDEO_PLAYER_EVENT_ERROR);
        ESP_LOGE(TAG, "start selected video failed: %s", esp_err_to_name(switched));
    }
}

esp_err_t video_player_service_play(void)
{
    if (!lock_service(pdMS_TO_TICKS(VIDEO_PLAYER_LOCK_WAIT_MS))) return ESP_ERR_TIMEOUT;
    if (!s_service.entered || s_service.count == 0U) {
        unlock_service();
        return ESP_ERR_INVALID_STATE;
    }
    if (s_service.display_width == 0U || s_service.display_height == 0U ||
        s_service.output_buffers[0] == NULL || s_service.output_buffers[1] == NULL ||
        s_service.output_capacity == 0U) {
        ESP_LOGE(TAG, "playback refused: fixed resolution or display buffers are not registered");
        unlock_service();
        return ESP_ERR_INVALID_STATE;
    }
    if (s_service.task != NULL) {
        if (s_service.state.status == VIDEO_PLAYER_STATUS_ENDED &&
            !s_service.switch_requested && !s_service.stop_requested) {
            s_service.restart_requested = true;
            unlock_service();
            return ESP_OK;
        }
        if (!s_service.stop_requested) {
            s_service.paused = false;
            (void)video_player_state_apply(&s_service.state, VIDEO_PLAYER_EVENT_PLAY);
        }
        unlock_service();
        return ESP_OK;
    }
    const bool preserve_held_buffers = s_service.state.status == VIDEO_PLAYER_STATUS_ENDED;
    const esp_err_t started = start_playback_locked(preserve_held_buffers);
    unlock_service();
    return started;
}

esp_err_t video_player_service_pause(void)
{
    if (!lock_service(pdMS_TO_TICKS(VIDEO_PLAYER_LOCK_WAIT_MS))) return ESP_ERR_TIMEOUT;
    if (s_service.task == NULL || !video_player_state_can_pause(&s_service.state)) {
        unlock_service();
        return ESP_ERR_INVALID_STATE;
    }
    s_service.paused = true;
    (void)video_player_state_apply(&s_service.state, VIDEO_PLAYER_EVENT_PAUSE);
    unlock_service();
    return ESP_OK;
}

esp_err_t video_player_service_stop(void)
{
    if (!lock_service(pdMS_TO_TICKS(VIDEO_PLAYER_LOCK_WAIT_MS))) return ESP_ERR_TIMEOUT;
    if (!s_service.entered) {
        unlock_service();
        return ESP_OK;
    }
    s_service.stop_requested = true;
    s_service.paused = false;
    s_service.switch_requested = false;
    s_service.restart_requested = false;
    if (s_service.task == NULL && s_service.count > 0U) (void)video_player_state_apply(&s_service.state, VIDEO_PLAYER_EVENT_STOP);
    unlock_service();
    return ESP_OK;
}

static esp_err_t select_relative(int direction)
{
    if (!lock_service(pdMS_TO_TICKS(VIDEO_PLAYER_LOCK_WAIT_MS))) return ESP_ERR_TIMEOUT;
    if (!s_service.entered || s_service.count == 0U) {
        unlock_service();
        return ESP_ERR_INVALID_STATE;
    }
    if (s_service.task != NULL) {
        /** Coalesce repeated taps while the old decoder is shutting down. */
        const size_t current = s_service.switch_requested ? s_service.switch_target : s_service.selected;
        s_service.switch_target = direction > 0 ? (current + 1U) % s_service.count
                                                  : (current == 0U ? s_service.count - 1U : current - 1U);
        s_service.switch_requested = true;
        s_service.restart_requested = false;
        s_service.stop_requested = true;
        s_service.paused = false;
        unlock_service();
        return ESP_OK;
    }
    if (direction > 0) s_service.selected = (s_service.selected + 1U) % s_service.count;
    else s_service.selected = s_service.selected == 0U ? s_service.count - 1U : s_service.selected - 1U;
    video_player_state_set_media(&s_service.state, s_service.entries[s_service.selected].name, 0U);
    s_service.width = 0U;
    s_service.height = 0U;
    s_service.fps = 0U;
    unlock_service();
    return ESP_OK;
}

esp_err_t video_player_service_next(void) { return select_relative(1); }
esp_err_t video_player_service_previous(void) { return select_relative(-1); }

void video_player_service_get_snapshot(video_player_snapshot_t *snapshot)
{
    if (snapshot == NULL) return;
    memset(snapshot, 0, sizeof(*snapshot));
    if (!lock_service(pdMS_TO_TICKS(VIDEO_PLAYER_LOCK_WAIT_MS))) {
        snapshot->status = VIDEO_PLAYER_STATUS_ERROR;
        snapshot->last_error = ESP_ERR_TIMEOUT;
        return;
    }
    snapshot->status = s_service.state.status;
    snapshot->duration_ms = s_service.state.duration_ms;
    snapshot->position_ms = s_service.state.position_ms;
    snapshot->width = s_service.width;
    snapshot->height = s_service.height;
    snapshot->fps = s_service.fps;
    snapshot->has_media = s_service.count > 0U && s_service.selected < s_service.count;
    snapshot->audio_available = snapshot->has_media && !s_service.silent;
    snapshot->video_available = snapshot->has_media;
    snapshot->last_error = s_service.last_error;
    if (snapshot->has_media) (void)snprintf(snapshot->name, sizeof(snapshot->name), "%s", s_service.entries[s_service.selected].name);
    unlock_service();
}

/**
 * @brief Free the preloaded copy; the caller holds the service lock.
 */
static void release_preload_locked(void)
{
    if (s_service.preload.data != NULL) {
        heap_caps_free(s_service.preload.data);
        s_service.preload.data = NULL;
    }
    s_service.preload.size = 0U;
    s_service.preload.path[0] = '\0';
}

esp_err_t video_player_service_release_preload(void)
{
    if (!lock_service(pdMS_TO_TICKS(VIDEO_PLAYER_LOCK_WAIT_MS))) return ESP_ERR_TIMEOUT;
    const bool held = s_service.preload.data != NULL;
    release_preload_locked();
    unlock_service();
    if (held) {
        ESP_LOGI(TAG, "preloaded copy released");
    }
    return ESP_OK;
}

esp_err_t video_player_service_preload_selected(size_t max_bytes)
{
    char path[VIDEO_PLAYER_PATH_MAX] = {0};
    if (!lock_service(pdMS_TO_TICKS(VIDEO_PLAYER_LOCK_WAIT_MS))) return ESP_ERR_TIMEOUT;
    if (!s_service.entered || s_service.count == 0U || s_service.selected >= s_service.count ||
        s_service.task != NULL) {
        unlock_service();
        return ESP_ERR_INVALID_STATE;
    }
    (void)snprintf(path, sizeof(path), "%s", s_service.entries[s_service.selected].path);
    release_preload_locked();
    unlock_service();

    /** Read preloaded video data outside the service lock. */
    FILE *file = fopen(path, "rb");
    if (file == NULL) {
        ESP_LOGW(TAG, "preload skipped, cannot open %s", path);
        return ESP_ERR_NOT_FOUND;
    }
    if (fseek(file, 0L, SEEK_END) != 0) {
        fclose(file);
        return ESP_FAIL;
    }
    const long length = ftell(file);
    (void)fseek(file, 0L, SEEK_SET);
    if (length <= 0L || (size_t)length > max_bytes) {
        ESP_LOGW(TAG, "preload skipped, %s is %ld bytes (limit %u)", path, length, (unsigned)max_bytes);
        fclose(file);
        return ESP_ERR_INVALID_SIZE;
    }
    uint8_t *data = heap_caps_malloc((size_t)length, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (data == NULL) {
        data = heap_caps_malloc((size_t)length, MALLOC_CAP_8BIT);
    }
    if (data == NULL) {
        ESP_LOGW(TAG, "preload skipped, no %ld bytes available for %s", length, path);
        fclose(file);
        return ESP_ERR_NO_MEM;
    }
    const size_t read = fread(data, 1U, (size_t)length, file);
    fclose(file);
    if (read != (size_t)length) {
        ESP_LOGW(TAG, "preload failed, read %u of %ld bytes from %s", (unsigned)read, length, path);
        heap_caps_free(data);
        return ESP_FAIL;
    }

    if (!lock_service(pdMS_TO_TICKS(VIDEO_PLAYER_LOCK_WAIT_MS))) {
        heap_caps_free(data);
        return ESP_ERR_TIMEOUT;
    }
    release_preload_locked();
    s_service.preload.data = data;
    s_service.preload.size = (uint32_t)length;
    (void)snprintf(s_service.preload.path, sizeof(s_service.preload.path), "%s", path);
    unlock_service();
    ESP_LOGI(TAG, "media preloaded into PSRAM: path=%s bytes=%ld", path, length);
    return ESP_OK;
}

esp_err_t video_player_service_set_frame_callback(video_player_frame_cb_t callback, void *context)
{
    if (!lock_service(pdMS_TO_TICKS(VIDEO_PLAYER_LOCK_WAIT_MS))) return ESP_ERR_TIMEOUT;
    s_service.frame_cb = callback;
    s_service.frame_context = context;
    unlock_service();
    return ESP_OK;
}

esp_err_t video_player_service_set_display_size(uint16_t width, uint16_t height)
{
    if (width == 0U || height == 0U) return ESP_ERR_INVALID_ARG;
    if (!lock_service(pdMS_TO_TICKS(VIDEO_PLAYER_LOCK_WAIT_MS))) return ESP_ERR_TIMEOUT;
    const bool changed = s_service.display_width != width || s_service.display_height != height;
    s_service.display_width = width;
    s_service.display_height = height;
    unlock_service();
    if (changed) {
        ESP_LOGI(TAG, "fixed playback resolution from board panel: %ux%u"
                 " (16-aligned decode target, scaling disabled)", width, height);
    }
    return ESP_OK;
}

esp_err_t video_player_service_set_output_buffers(uint8_t *first, uint8_t *second, uint32_t capacity)
{
    if (!lock_service(pdMS_TO_TICKS(VIDEO_PLAYER_LOCK_WAIT_MS))) return ESP_ERR_TIMEOUT;
    if (s_service.task != NULL || s_service.video_task != NULL) {
        unlock_service();
        return ESP_ERR_INVALID_STATE;
    }
    s_service.output_buffers[0] = first;
    s_service.output_buffers[1] = second;
    s_service.output_capacity = (first != NULL && second != NULL) ? capacity : 0U;
    s_service.output_held_by_view[0] = false;
    s_service.output_held_by_view[1] = false;
    s_service.output_next = 0U;
    unlock_service();
    if (first != NULL && second != NULL) {
        ESP_LOGI(TAG, "display buffers registered: capacity=%" PRIu32 " bytes each, direct decode enabled",
                 capacity);
    } else {
        ESP_LOGI(TAG, "display buffers unregistered");
    }
    return ESP_OK;
}

void video_player_service_release_output_buffer(uint8_t buffer_index)
{
    if (buffer_index > 1U) return;
    if (!lock_service(pdMS_TO_TICKS(VIDEO_PLAYER_LOCK_WAIT_MS))) return;
    s_service.output_held_by_view[buffer_index] = false;
    unlock_service();
}
