#include "music_bt_gmf.h"

#include <string.h>

#include "audio_gmf.h"
#include "esp_audio_simple_dec.h"
#include "esp_bt_audio_defs.h"
#include "esp_gmf_asrc.h"
#include "esp_gmf_audio_dec.h"
#include "esp_gmf_err.h"
#include "esp_gmf_io.h"
#include "esp_gmf_io_bt.h"
#include "esp_gmf_obj.h"
#include "esp_gmf_task.h"
#include "esp_log.h"

#define TAG "MusicBtGmf"
#define MUSIC_BT_GMF_DAC_SAMPLE_RATE 48000U
#define MUSIC_BT_GMF_DAC_CHANNELS 2U
#define MUSIC_BT_GMF_DAC_BITS 16U
#define MUSIC_BT_GMF_MAX_CHANNELS 4U
#define MUSIC_BT_GMF_TASK_STACK_SIZE 4096U

static esp_audio_simple_dec_type_t decoder_type(esp_bt_audio_stream_codec_type_t codec_type)
{
    switch (codec_type) {
        case ESP_BT_AUDIO_STREAM_CODEC_SBC:
            return ESP_AUDIO_SIMPLE_DEC_TYPE_SBC;
        case ESP_BT_AUDIO_STREAM_CODEC_AAC:
            return ESP_AUDIO_SIMPLE_DEC_TYPE_AAC;
        default:
            return ESP_AUDIO_SIMPLE_DEC_TYPE_NONE;
    }
}

static esp_gmf_element_handle_t find_asrc(esp_gmf_pipeline_handle_t pipeline)
{
    const void *iterator = NULL;
    esp_gmf_element_handle_t element = NULL;
    while (esp_gmf_pipeline_iterate_element(pipeline, &iterator, &element) == ESP_GMF_ERR_OK) {
        char *tag = NULL;
        esp_gmf_obj_get_tag((esp_gmf_obj_handle_t)element, &tag);
        if (tag != NULL && strcmp(tag, "aud_asrc") == 0) {
            return element;
        }
    }
    return NULL;
}

static esp_err_t configure_asrc(esp_gmf_pipeline_handle_t pipeline, uint32_t input_channels)
{
    if (input_channels == 0U || input_channels > MUSIC_BT_GMF_MAX_CHANNELS) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_gmf_element_handle_t asrc = find_asrc(pipeline);
    if (asrc == NULL) {
        return ESP_FAIL;
    }
    esp_gmf_err_t ret = esp_gmf_asrc_set_dest_rate(asrc, MUSIC_BT_GMF_DAC_SAMPLE_RATE);
    if (ret != ESP_GMF_ERR_OK) {
        return ESP_FAIL;
    }
    ret = esp_gmf_asrc_set_dest_ch(asrc, MUSIC_BT_GMF_DAC_CHANNELS);
    if (ret != ESP_GMF_ERR_OK) {
        return ESP_FAIL;
    }
    esp_asrc_cfg_t *config = (esp_asrc_cfg_t *)OBJ_GET_CFG(asrc);
    if (config == NULL) {
        return ESP_FAIL;
    }
    static float weights[MUSIC_BT_GMF_MAX_CHANNELS * MUSIC_BT_GMF_DAC_CHANNELS];
    memset(weights, 0, sizeof(weights));
    for (uint32_t channel = 0U; channel < MUSIC_BT_GMF_DAC_CHANNELS; ++channel) {
        weights[channel * input_channels + (channel < input_channels ? channel : input_channels - 1U)] = 1.0f;
    }
    config->weight = weights;
    config->weight_len = input_channels * MUSIC_BT_GMF_DAC_CHANNELS;
    return ESP_OK;
}

static esp_gmf_err_t pipeline_event(esp_gmf_event_pkt_t *packet, void *context)
{
    (void)context;
    if (packet != NULL && packet->type == ESP_GMF_EVT_TYPE_CHANGE_STATE) {
        ESP_LOGI(TAG, "Bluetooth GMF pipeline state=%d", packet->sub);
    }
    return ESP_GMF_ERR_OK;
}

static esp_err_t validate_runtime_io(esp_gmf_pool_handle_t pool,
                                     dev_audio_codec_handles_t *dac)
{
    return pool != NULL && dac != NULL && dac->codec_dev != NULL ? ESP_OK : ESP_ERR_INVALID_STATE;
}

esp_err_t music_bt_gmf_init(music_bt_gmf_t *source,
                            esp_gmf_pool_handle_t pool,
                            dev_audio_codec_handles_t *dac)
{
    if (source == NULL || validate_runtime_io(pool, dac) != ESP_OK) return ESP_ERR_INVALID_ARG;
    if (source->pipeline != NULL) return ESP_OK;
    source->pool = pool;
    source->dac = dac;

    const char *elements[] = {AUDIO_GMF_FRAME_DECODER_TAG, "aud_asrc"};
    esp_gmf_err_t gmf_ret = esp_gmf_pool_new_pipeline(source->pool, "io_bt", elements,
                                                       2U, "music_bt_codec", &source->pipeline);
    if (gmf_ret != ESP_GMF_ERR_OK) {
        source->pipeline = NULL;
        return ESP_FAIL;
    }
    gmf_ret = esp_gmf_pipeline_set_event(source->pipeline, pipeline_event, source);
    if (gmf_ret != ESP_GMF_ERR_OK) {
        (void)music_bt_gmf_deinit(source);
        return ESP_FAIL;
    }
    esp_gmf_task_cfg_t task_config = DEFAULT_ESP_GMF_TASK_CONFIG();
    task_config.thread.stack = MUSIC_BT_GMF_TASK_STACK_SIZE;
    task_config.thread.stack_in_ext = true;
    task_config.name = "music_bt_task";
    gmf_ret = esp_gmf_task_init(&task_config, &source->task);
    if (gmf_ret != ESP_GMF_ERR_OK) {
        (void)music_bt_gmf_deinit(source);
        return ESP_FAIL;
    }
    gmf_ret = esp_gmf_pipeline_bind_task(source->pipeline, source->task);
    if (gmf_ret != ESP_GMF_ERR_OK) {
        (void)music_bt_gmf_deinit(source);
        return ESP_FAIL;
    }
    return ESP_OK;
}

esp_err_t music_bt_gmf_on_stream_state(music_bt_gmf_t *source,
                                        esp_bt_audio_stream_handle_t stream,
                                        esp_bt_audio_stream_state_t state)
{
    if (source == NULL || source->pipeline == NULL || stream == NULL) return ESP_ERR_INVALID_STATE;
    esp_bt_audio_stream_dir_t direction = ESP_BT_AUDIO_STREAM_DIR_UNKNOWN;
    if (esp_bt_audio_stream_get_dir(stream, &direction) != ESP_OK ||
        direction != ESP_BT_AUDIO_STREAM_DIR_SINK) {
        return ESP_ERR_INVALID_ARG;
    }

    if (state == ESP_BT_AUDIO_STREAM_STATE_ALLOCATED) {
        esp_bt_audio_stream_codec_info_t codec_info = {0};
        if (esp_bt_audio_stream_get_codec_info(stream, &codec_info) != ESP_OK) {
            return ESP_FAIL;
        }
        esp_audio_simple_dec_cfg_t decoder_config = {
            .dec_type = decoder_type(codec_info.codec_type),
            .dec_cfg = codec_info.codec_cfg,
            .cfg_size = codec_info.cfg_size,
            .use_frame_dec = true,
        };
        esp_gmf_element_handle_t decoder = NULL;
        if (decoder_config.dec_type == ESP_AUDIO_SIMPLE_DEC_TYPE_NONE ||
            esp_gmf_pipeline_get_head_el(source->pipeline, &decoder) != ESP_GMF_ERR_OK ||
            decoder == NULL ||
            esp_gmf_audio_dec_reconfig(decoder, &decoder_config) != ESP_GMF_ERR_OK ||
            esp_gmf_io_bt_set_stream(ESP_GMF_PIPELINE_GET_IN_INSTANCE(source->pipeline), stream) != ESP_GMF_ERR_OK ||
            configure_asrc(source->pipeline, __builtin_popcount(codec_info.channels)) != ESP_OK ||
            esp_gmf_pipeline_loading_jobs(source->pipeline) != ESP_GMF_ERR_OK) {
            return ESP_FAIL;
        }
        source->stream = stream;
        return ESP_OK;
    }
    if (state == ESP_BT_AUDIO_STREAM_STATE_STARTED) {
        if (source->stream != stream) {
            return ESP_ERR_INVALID_STATE;
        }
        esp_gmf_err_t ret = esp_gmf_pipeline_run(source->pipeline);
        source->running = ret == ESP_GMF_ERR_OK;
        return source->running ? ESP_OK : ESP_FAIL;
    }
    if (state == ESP_BT_AUDIO_STREAM_STATE_STOPPED ||
        state == ESP_BT_AUDIO_STREAM_STATE_RELEASED) {
        esp_gmf_err_t ret = esp_gmf_pipeline_stop(source->pipeline);
        if (ret == ESP_GMF_ERR_OK) {
            ret = esp_gmf_pipeline_reset(source->pipeline);
        }
        source->running = false;
        if (state == ESP_BT_AUDIO_STREAM_STATE_RELEASED) {
            source->stream = NULL;
        }
        return ret == ESP_GMF_ERR_OK ? ESP_OK : ESP_FAIL;
    }
    return ESP_OK;
}

esp_err_t music_bt_gmf_deinit(music_bt_gmf_t *source)
{
    if (source == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t first = ESP_OK;
    if (source->pipeline != NULL && source->task != NULL) {
        if (source->running) {
            esp_gmf_err_t ret = esp_gmf_pipeline_stop(source->pipeline);
            if (ret != ESP_GMF_ERR_OK && first == ESP_OK) {
                first = ESP_FAIL;
            }
        }
    }
    if (source->task != NULL) {
        esp_gmf_err_t ret = esp_gmf_task_deinit(source->task);
        if (ret != ESP_GMF_ERR_OK && first == ESP_OK) {
            first = ESP_FAIL;
        }
        source->task = NULL;
    }
    if (source->pipeline != NULL) {
        esp_gmf_err_t ret = esp_gmf_pipeline_destroy(source->pipeline);
        if (ret != ESP_GMF_ERR_OK && first == ESP_OK) {
            first = ESP_FAIL;
        }
        source->pipeline = NULL;
    }
    source->stream = NULL;
    source->running = false;
    return first;
}
