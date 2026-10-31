#include "aichat_audio_gmf.h"

#include <stdlib.h>
#include <string.h>
#include <inttypes.h>

#include "aichat_audio_format.h"
#include "audio_gmf.h"
#include "audio_input.h"
#include "audio_output.h"
#include "audio_runtime.h"
#include "esp_afe_config.h"
#include "esp_afe_sr_models.h"
#include "esp_gmf_afe.h"
#include "esp_gmf_afe_manager.h"
#include "esp_gmf_element.h"
#include "esp_gmf_io.h"
#include "esp_gmf_io_codec_dev.h"
#include "esp_gmf_pipeline.h"
#include "esp_gmf_port.h"
#include "esp_gmf_task.h"
#include "encoder/impl/esp_opus_enc.h"
#include "sdkconfig.h"

struct aichat_audio_gmf {
    aichat_audio_pcm_cb_t pcm_cb;
    aichat_audio_afe_event_cb_t afe_cb;
    void *context;
    esp_gmf_pipeline_handle_t pipeline;
    esp_gmf_task_handle_t task;
    esp_gmf_io_handle_t input_io;
    esp_gmf_element_handle_t afe_element;
    esp_gmf_afe_manager_handle_t afe_manager;
    srmodel_list_t *models;
    bool runtime_acquired;
    bool input_opened;
    bool input_gain_changed;
    float previous_input_gain_db;
    bool output_reconfigured;
    bool output_was_open;
    esp_codec_dev_sample_info_t previous_output_format;
    bool running;
    bool wake_awake_applied;
    volatile bool encoding_enabled;
    void *opus_encoder;
    int opus_frame_bytes;
    size_t pcm_bytes;
    uint8_t *pcm_frame;
    uint8_t *opus_frame;
    uint32_t encoded_frames;
};

static esp_gmf_err_io_t pcm_acquire(void *handle, esp_gmf_payload_t *load,
                                    int wanted_size, int block_ticks)
{
    (void)handle;
    (void)load;
    (void)wanted_size;
    (void)block_ticks;
    return ESP_GMF_IO_OK;
}

static void apply_keep_awake_after_afe_open(aichat_audio_gmf_t *audio)
{
    if (audio == NULL || audio->wake_awake_applied || audio->afe_element == NULL) {
        return;
    }
    /** Apply AFE commands after the first pipeline output, when its command queue exists. */
    const esp_gmf_err_t ret = esp_gmf_afe_keep_awake(audio->afe_element, true);
    if (ret == ESP_GMF_ERR_OK) {
        audio->wake_awake_applied = true;
        ESP_LOGI("AIChatAudio", "AFE keep-awake enabled after pipeline open");
    } else {
        ESP_LOGW("AIChatAudio", "Failed to enable AFE keep-awake after open: %d", ret);
    }
}

static esp_gmf_err_io_t pcm_release(void *handle, esp_gmf_payload_t *load,
                                    int block_ticks)
{
    aichat_audio_gmf_t *audio = handle;
    (void)block_ticks;
    if (audio == NULL || load == NULL) {
        return ESP_GMF_IO_FAIL;
    }
    apply_keep_awake_after_afe_open(audio);
    if (audio->pcm_cb == NULL || load->buf == NULL || load->valid_size <= 0 ||
        audio->opus_encoder == NULL) {
        return ESP_GMF_IO_OK;
    }
    /** Keep wake detection active; enable Opus encoding only while uplink is open. */
    if (!audio->encoding_enabled) {
        audio->pcm_bytes = 0U;
        return ESP_GMF_IO_OK;
    }

    size_t offset = 0U;
    while (offset < (size_t)load->valid_size) {
        size_t available = (size_t)audio->opus_frame_bytes - audio->pcm_bytes;
        size_t copy = (size_t)load->valid_size - offset;
        if (copy > available) {
            copy = available;
        }
        memcpy(audio->pcm_frame + audio->pcm_bytes, load->buf + offset, copy);
        audio->pcm_bytes += copy;
        offset += copy;
        if (audio->pcm_bytes == (size_t)audio->opus_frame_bytes) {
            esp_audio_enc_in_frame_t in = {
                .buffer = audio->pcm_frame,
                .len = (uint32_t)audio->pcm_bytes,
            };
            esp_audio_enc_out_frame_t out = {
                .buffer = audio->opus_frame,
                .len = 4096U,
                .encoded_bytes = 0U,
            };
            if (esp_opus_enc_process(audio->opus_encoder, &in, &out) == ESP_AUDIO_ERR_OK &&
                out.encoded_bytes > 0U && audio->encoding_enabled) {
                audio->encoded_frames++;
                if ((audio->encoded_frames % 25U) == 0U) {
                    ESP_LOGI("AIChatAudio", "AFE PCM -> Opus encoded frames=%" PRIu32
                             ", last_bytes=%u", audio->encoded_frames,
                             (unsigned)out.encoded_bytes);
                }
                audio->pcm_cb(audio->opus_frame, out.encoded_bytes, audio->context);
            }
            audio->pcm_bytes = 0U;
        }
    }
    return ESP_GMF_IO_OK;
}

static void afe_event(esp_gmf_element_handle_t element, esp_gmf_afe_evt_t *event,
                      void *context)
{
    aichat_audio_gmf_t *audio = context;
    (void)element;
    if (audio != NULL && event != NULL && audio->afe_cb != NULL) {
        audio->afe_cb((int)event->type, event->event_data, audio->context);
    }
}

aichat_audio_gmf_t *aichat_audio_gmf_create(aichat_audio_pcm_cb_t pcm_cb,
                                             aichat_audio_afe_event_cb_t afe_cb,
                                             void *context)
{
    aichat_audio_gmf_t *audio = calloc(1, sizeof(*audio));
    if (audio != NULL) {
        audio->pcm_cb = pcm_cb;
        audio->afe_cb = afe_cb;
        audio->context = context;
    }
    return audio;
}

static void cleanup(aichat_audio_gmf_t *audio)
{
    if (audio == NULL) {
        return;
    }
    if (audio->pipeline != NULL && audio->running) {
        (void)esp_gmf_pipeline_stop(audio->pipeline);
        audio->running = false;
    }
    if (audio->task != NULL) {
        (void)esp_gmf_task_deinit(audio->task);
        audio->task = NULL;
    }
    if (audio->pipeline != NULL) {
        (void)esp_gmf_pipeline_destroy(audio->pipeline);
        audio->pipeline = NULL;
        audio->input_io = NULL;
        audio->afe_element = NULL;
    }
    if (audio->afe_manager != NULL) {
        (void)esp_gmf_afe_manager_destroy(audio->afe_manager);
        audio->afe_manager = NULL;
    }
    if (audio->models != NULL) {
        esp_srmodel_deinit(audio->models);
        audio->models = NULL;
    }
    if (audio->opus_encoder != NULL) {
        (void)esp_opus_enc_close(audio->opus_encoder);
        audio->opus_encoder = NULL;
    }
    free(audio->pcm_frame);
    free(audio->opus_frame);
    audio->pcm_frame = NULL;
    audio->opus_frame = NULL;
    audio->pcm_bytes = 0U;
    audio->wake_awake_applied = false;
    audio->encoding_enabled = false;
    if (audio->input_opened) {
        if (audio->input_gain_changed) {
            esp_codec_dev_handle_t input_codec = audio_input_get_codec_dev();
            if (input_codec != NULL &&
                esp_codec_dev_set_in_gain(input_codec, audio->previous_input_gain_db) !=
                    ESP_CODEC_DEV_OK) {
                ESP_LOGW("AIChatAudio", "Failed to restore microphone gain %.1f dB",
                         audio->previous_input_gain_db);
            }
            audio->input_gain_changed = false;
        }
        (void)audio_input_close();
        audio->input_opened = false;
    }
    if (audio->output_reconfigured) {
        esp_err_t restore_ret;
        if (audio->output_was_open) {
            restore_ret = audio_output_reconfigure(&audio->previous_output_format);
        } else {
            restore_ret = audio_output_close();
        }
        if (restore_ret != ESP_OK) {
            ESP_LOGW("AIChatAudio", "failed to restore output format: %s",
                     esp_err_to_name(restore_ret));
        }
        audio->output_reconfigured = false;
    }
    if (audio->runtime_acquired) {
        (void)audio_runtime_release();
        audio->runtime_acquired = false;
    }
}

static esp_err_t create_afe(aichat_audio_gmf_t *audio)
{
    audio->models = esp_srmodel_init(AICHAT_AUDIO_MODEL_PARTITION);
    if (audio->models == NULL) {
        return ESP_ERR_NOT_FOUND;
    }

    afe_config_t *afe_cfg = afe_config_init("MRMN", audio->models,
                                            AFE_TYPE_SR, AFE_MODE_LOW_COST);
    if (afe_cfg == NULL) {
        return ESP_ERR_NO_MEM;
    }
    afe_cfg->wakenet_init = true;
    afe_cfg->aec_init = true;
    /** Use low-cost speech-recognition AEC to bound feed-task processing. */
    afe_cfg->aec_mode = AEC_MODE_SR_LOW_COST;
    afe_cfg->memory_alloc_mode = AFE_MEMORY_ALLOC_MORE_PSRAM;
    /** Enable VAD to report the end of each listening turn. */
    afe_cfg->vad_init = true;
    afe_cfg->vad_mode = VAD_MODE_4;
    afe_cfg->vad_min_speech_ms = 128;
    afe_cfg->vad_min_noise_ms = 1000;

    esp_gmf_afe_manager_cfg_t manager_cfg =
        DEFAULT_GMF_AFE_MANAGER_CFG(afe_cfg, NULL, NULL, NULL, NULL);
    manager_cfg.feed_task_setting.core = ESP_AFE_MANAGER_FEED_TASK_CORE;
    manager_cfg.feed_task_setting.prio = 6;
    manager_cfg.feed_task_setting.stack_size = ESP_AFE_MANAGER_FEED_TASK_STACK;
    manager_cfg.fetch_task_setting.core = 0;
    manager_cfg.fetch_task_setting.prio = 6;
    manager_cfg.fetch_task_setting.stack_size = ESP_AFE_MANAGER_FETCH_TASK_STACK;

    const esp_gmf_err_t manager_ret =
        esp_gmf_afe_manager_create(&manager_cfg, &audio->afe_manager);
    afe_config_free(afe_cfg);
    if (manager_ret != ESP_GMF_ERR_OK || audio->afe_manager == NULL) {
        return ESP_FAIL;
    }

    esp_gmf_afe_cfg_t element_cfg =
        DEFAULT_GMF_AFE_CFG(audio->afe_manager, afe_event, audio, audio->models);
    if (esp_gmf_afe_init(&element_cfg, (esp_gmf_obj_handle_t *)&audio->afe_element) !=
            ESP_GMF_ERR_OK ||
        audio->afe_element == NULL) {
        return ESP_FAIL;
    }
    return ESP_OK;
}

static esp_err_t create_pipeline(aichat_audio_gmf_t *audio)
{
    if (esp_gmf_pipeline_create(&audio->pipeline) != ESP_GMF_ERR_OK ||
        audio->pipeline == NULL) {
        return ESP_FAIL;
    }

    esp_gmf_pool_handle_t pool = audio_gmf_get_pool();
    esp_codec_dev_handle_t input_codec = audio_input_get_codec_dev();
    if (pool == NULL || input_codec == NULL ||
        esp_gmf_pool_new_io(pool, AICHAT_GMF_CODEC_IO_NAME,
                            ESP_GMF_IO_DIR_READER, &audio->input_io) !=
            ESP_GMF_ERR_OK ||
        audio->input_io == NULL ||
        esp_gmf_io_codec_dev_set_dev(audio->input_io, input_codec) !=
            ESP_GMF_ERR_OK ||
        esp_gmf_pipeline_set_io(audio->pipeline, audio->input_io,
                                ESP_GMF_IO_DIR_READER) != ESP_GMF_ERR_OK ||
        esp_gmf_pipeline_register_el(audio->pipeline, audio->afe_element) !=
            ESP_GMF_ERR_OK) {
        return ESP_FAIL;
    }

    esp_gmf_port_handle_t input_port = NEW_ESP_GMF_PORT_IN_BYTE(
        esp_gmf_io_acquire_read, esp_gmf_io_release_read, NULL, audio->input_io,
        ESP_GMF_ELEMENT_GET(audio->afe_element)->in_attr.data_size, ESP_GMF_MAX_DELAY);
    if (input_port == NULL ||
        esp_gmf_element_register_in_port(audio->afe_element, input_port) !=
            ESP_GMF_ERR_OK) {
        return ESP_FAIL;
    }

    esp_gmf_port_handle_t output_port = NEW_ESP_GMF_PORT_OUT_BYTE(
        pcm_acquire, pcm_release, NULL, audio, 4096U, ESP_GMF_MAX_DELAY);
    if (output_port == NULL ||
        esp_gmf_element_register_out_port(audio->afe_element, output_port) !=
            ESP_GMF_ERR_OK) {
        return ESP_FAIL;
    }

    esp_gmf_info_sound_t info = {
        .sample_rates = AICHAT_AUDIO_AFE_SAMPLE_RATE,
        .bits = AICHAT_AUDIO_BITS,
        .channels = AICHAT_AUDIO_CHANNELS,
    };
    if (esp_gmf_pipeline_report_info(audio->pipeline, ESP_GMF_INFO_SOUND,
                                     &info, sizeof(info)) != ESP_GMF_ERR_OK) {
        return ESP_FAIL;
    }
    return ESP_OK;
}

esp_err_t aichat_audio_gmf_start(aichat_audio_gmf_t *audio)
{
    if (audio == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (audio->running) {
        return ESP_OK;
    }

    esp_err_t ret = audio_runtime_acquire();
    if (ret != ESP_OK) {
        return ret;
    }
    audio->runtime_acquired = true;

    const esp_codec_dev_sample_info_t output_format = {
        .sample_rate = AICHAT_AUDIO_CODEC_SAMPLE_RATE,
        .channel = AICHAT_AUDIO_OUTPUT_CHANNELS,
        .bits_per_sample = AICHAT_AUDIO_BITS,
    };
    audio->output_was_open = audio_output_is_open();
    if (audio->output_was_open) {
        ret = audio_output_get_format(&audio->previous_output_format);
        if (ret != ESP_OK) {
            cleanup(audio);
            return ret;
        }
    }
    ret = audio_output_reconfigure(&output_format);
    if (ret != ESP_OK) {
        cleanup(audio);
        return ret;
    }
    audio->output_reconfigured = true;

    const esp_codec_dev_sample_info_t format = {
        .sample_rate = AICHAT_AUDIO_AFE_SAMPLE_RATE,
        .channel = AICHAT_AUDIO_CHANNELS,
        .bits_per_sample = AICHAT_AUDIO_BITS,
    };
    ret = audio_input_open(&format);
    if (ret != ESP_OK) {
        cleanup(audio);
        return ret;
    }
    audio->input_opened = true;

    esp_codec_dev_handle_t input_codec = audio_input_get_codec_dev();
    if (input_codec == NULL ||
        esp_codec_dev_get_in_gain(input_codec, &audio->previous_input_gain_db) !=
            ESP_CODEC_DEV_OK ||
        esp_codec_dev_set_in_gain(input_codec, AICHAT_AUDIO_INPUT_GAIN_DB) !=
            ESP_CODEC_DEV_OK) {
        ESP_LOGE("AIChatAudio", "Failed to set microphone gain to %.1f dB",
                 AICHAT_AUDIO_INPUT_GAIN_DB);
        cleanup(audio);
        return ESP_FAIL;
    }
    audio->input_gain_changed = true;
    ESP_LOGI("AIChatAudio", "Microphone input gain set to %.1f dB",
             AICHAT_AUDIO_INPUT_GAIN_DB);

    esp_codec_dev_sample_info_t active = {0};
    ret = audio_input_get_format(&active);
    if (ret != ESP_OK || memcmp(&active, &format, sizeof(format)) != 0) {
        cleanup(audio);
        return ESP_ERR_INVALID_STATE;
    }
    ret = create_afe(audio);
    if (ret != ESP_OK) {
        cleanup(audio);
        return ret;
    }

    esp_opus_enc_config_t opus_cfg = ESP_OPUS_ENC_CONFIG_DEFAULT();
    opus_cfg.sample_rate = AICHAT_AUDIO_SAMPLE_RATE;
    opus_cfg.channel = ESP_AUDIO_MONO;
    opus_cfg.bits_per_sample = ESP_AUDIO_BIT16;
    opus_cfg.frame_duration = ESP_OPUS_ENC_FRAME_DURATION_60_MS;
    opus_cfg.application_mode = ESP_OPUS_ENC_APPLICATION_VOIP;
    opus_cfg.bitrate = 90000;
    if (esp_opus_enc_open(&opus_cfg, sizeof(opus_cfg), &audio->opus_encoder) !=
            ESP_AUDIO_ERR_OK ||
        audio->opus_encoder == NULL) {
        cleanup(audio);
        return ESP_FAIL;
    }
    int opus_frame_size = 0;
    int opus_output_size = 0;
    if (esp_opus_enc_get_frame_size(audio->opus_encoder, &opus_frame_size,
                                    &opus_output_size) != ESP_AUDIO_ERR_OK ||
        opus_frame_size <= 0 || opus_output_size <= 0) {
        cleanup(audio);
        return ESP_FAIL;
    }
    audio->opus_frame_bytes = opus_frame_size;
    audio->pcm_frame = malloc((size_t)opus_frame_size);
    audio->opus_frame = malloc((size_t)opus_output_size);
    if (audio->pcm_frame == NULL || audio->opus_frame == NULL) {
        cleanup(audio);
        return ESP_ERR_NO_MEM;
    }

    ret = create_pipeline(audio);
    if (ret != ESP_OK) {
        cleanup(audio);
        return ret;
    }

    esp_gmf_task_cfg_t task_cfg = DEFAULT_ESP_GMF_TASK_CONFIG();
    task_cfg.thread.stack = AICHAT_AUDIO_GMF_TASK_STACK_SIZE;
    task_cfg.thread.core = 1;
    task_cfg.thread.stack_in_ext = true;
    task_cfg.name = "aichat_audio";
    if (esp_gmf_task_init(&task_cfg, &audio->task) != ESP_GMF_ERR_OK ||
        esp_gmf_pipeline_bind_task(audio->pipeline, audio->task) != ESP_GMF_ERR_OK ||
        esp_gmf_pipeline_loading_jobs(audio->pipeline) != ESP_GMF_ERR_OK ||
        esp_gmf_pipeline_run(audio->pipeline) != ESP_GMF_ERR_OK) {
        cleanup(audio);
        return ESP_FAIL;
    }
    audio->running = true;
    if (aichat_audio_gmf_set_listening_profile(audio, false) != ESP_OK) {
        cleanup(audio);
        return ESP_FAIL;
    }
    return ESP_OK;
}

void aichat_audio_gmf_set_uplink_enabled(aichat_audio_gmf_t *audio, bool enabled)
{
    if (audio == NULL) {
        return;
    }
    audio->encoding_enabled = enabled;
    if (!enabled) {
        /** Discard partial 60 ms Opus frames between turns. */
        audio->pcm_bytes = 0U;
    }
}

esp_err_t aichat_audio_gmf_set_listening_profile(aichat_audio_gmf_t *audio,
                                                  bool listening)
{
    if (audio == NULL || audio->afe_manager == NULL || !audio->running) {
        return ESP_ERR_INVALID_STATE;
    }
    /** Enable speech enhancement only during a listening turn. */
    const esp_gmf_err_t ret = esp_gmf_afe_manager_enable_features(
        audio->afe_manager, ESP_AFE_FEATURE_SE, listening);
    if (ret < 0) {
        ESP_LOGW("AIChatAudio", "Failed to %s AFE SE profile: %d",
                 listening ? "enable" : "disable", ret);
        return ESP_FAIL;
    }
    return ESP_OK;
}

esp_err_t aichat_audio_gmf_end_wake_session(aichat_audio_gmf_t *audio)
{
    if (audio == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    aichat_audio_gmf_set_uplink_enabled(audio, false);
    if (audio->afe_element == NULL || !audio->running) {
        return ESP_ERR_INVALID_STATE;
    }
    return esp_gmf_afe_trigger_sleep(audio->afe_element) == ESP_GMF_ERR_OK ? ESP_OK : ESP_FAIL;
}

esp_err_t aichat_audio_gmf_stop(aichat_audio_gmf_t *audio)
{
    if (audio == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    cleanup(audio);
    return ESP_OK;
}

void aichat_audio_gmf_destroy(aichat_audio_gmf_t *audio)
{
    if (audio == NULL) {
        return;
    }
    cleanup(audio);
    free(audio);
}

bool aichat_audio_gmf_is_running(const aichat_audio_gmf_t *audio)
{
    return audio != NULL && audio->running;
}
