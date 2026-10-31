#include "aichat_audio_playback.h"

#include <stdlib.h>
#include <string.h>

#include "audio_gmf.h"
#include "audio_output.h"
#include "audio_runtime.h"
#include "aichat_audio_format.h"
#include "esp_gmf_asrc.h"
#include "esp_gmf_io_codec_dev.h"
#include "esp_gmf_audio_dec.h"
#include "esp_gmf_element.h"
#include "esp_gmf_pipeline.h"
#include "esp_gmf_payload.h"
#include "esp_gmf_pool.h"
#include "esp_gmf_port.h"
#include "esp_gmf_task.h"
#include "decoder/impl/esp_opus_dec.h"
#include "simple_dec/esp_audio_simple_dec.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

#define AICHAT_PLAYBACK_QUEUE_LENGTH 8U
#define AICHAT_PLAYBACK_PACKET_MAX_SIZE 4096U
#define AICHAT_PLAYBACK_OUTPUT_BUFFER_SIZE 8192U
/** @brief Reserve 7680 bytes for a 120 ms, 16 kHz, 16-bit mono Opus frame. */
#define AICHAT_PLAYBACK_DECODER_OUTPUT_BUFFER_SIZE 8192U
#define AICHAT_PLAYBACK_TASK_STACK_SIZE 32768U
#define AICHAT_PLAYBACK_OUTPUT_SAMPLE_RATE AICHAT_AUDIO_CODEC_SAMPLE_RATE
#define AICHAT_PLAYBACK_OUTPUT_CHANNELS AICHAT_AUDIO_OUTPUT_CHANNELS

struct aichat_audio_packet {
    uint8_t *data;
    size_t len;
    uint64_t pts;
};

/** @brief Sentinel used only to wake the blocking input callback during shutdown. */
#define AICHAT_PLAYBACK_STOP_PACKET ((struct aichat_audio_packet *)(uintptr_t)1U)

struct aichat_audio_playback {
    QueueHandle_t queue;
    esp_gmf_pipeline_handle_t pipeline;
    esp_gmf_task_handle_t task;
    esp_gmf_element_handle_t decoder;
    esp_gmf_element_handle_t asrc;
    esp_codec_dev_handle_t output_codec;
    esp_gmf_io_handle_t output_io;
    bool runtime_acquired;
    volatile bool stopping;
    bool running;
};

static bool frame_duration_to_enum(uint32_t duration_ms,
                                   esp_opus_dec_frame_duration_t *duration)
{
    if (duration == NULL) {
        return false;
    }
    switch (duration_ms) {
        case 2U: *duration = ESP_OPUS_DEC_FRAME_DURATION_2_5_MS; return true;
        case 5U: *duration = ESP_OPUS_DEC_FRAME_DURATION_5_MS; return true;
        case 10U: *duration = ESP_OPUS_DEC_FRAME_DURATION_10_MS; return true;
        case 20U: *duration = ESP_OPUS_DEC_FRAME_DURATION_20_MS; return true;
        case 40U: *duration = ESP_OPUS_DEC_FRAME_DURATION_40_MS; return true;
        case 60U: *duration = ESP_OPUS_DEC_FRAME_DURATION_60_MS; return true;
        case 80U: *duration = ESP_OPUS_DEC_FRAME_DURATION_80_MS; return true;
        case 100U: *duration = ESP_OPUS_DEC_FRAME_DURATION_100_MS; return true;
        case 120U: *duration = ESP_OPUS_DEC_FRAME_DURATION_120_MS; return true;
        default: return false;
    }
}

static esp_gmf_err_io_t packet_acquire(void *context, esp_gmf_payload_t *load,
                                       int wanted_size, int block_ticks)
{
    aichat_audio_playback_t *playback = context;
    struct aichat_audio_packet *packet = NULL;
    (void)wanted_size;
    (void)block_ticks;
    if (playback == NULL || load == NULL || playback->queue == NULL) {
        return ESP_GMF_IO_FAIL;
    }
    /** Queue network packets; codec IO owns pipeline shutdown. */
    if (xQueueReceive(playback->queue, &packet, pdMS_TO_TICKS(20)) != pdTRUE) {
        load->valid_size = 0U;
        load->is_done = false;
        vTaskDelay(1);
        return ESP_GMF_IO_OK;
    }
    if (packet == AICHAT_PLAYBACK_STOP_PACKET) {
        load->valid_size = 0U;
        load->is_done = true;
        return ESP_GMF_IO_OK;
    }
    if (packet == NULL || packet->len > load->buf_length) {
        if (packet != NULL && packet != AICHAT_PLAYBACK_STOP_PACKET) {
            free(packet->data);
            free(packet);
        }
        return ESP_GMF_IO_FAIL;
    }
    memcpy(load->buf, packet->data, packet->len);
    load->valid_size = packet->len;
    load->pts = packet->pts;
    free(packet->data);
    free(packet);
    return ESP_GMF_IO_OK;
}

static esp_gmf_err_io_t packet_release(void *context, esp_gmf_payload_t *load,
                                       int block_ticks)
{
    (void)context;
    (void)load;
    (void)block_ticks;
    return ESP_GMF_IO_OK;
}

static void drain_queue(aichat_audio_playback_t *playback)
{
    struct aichat_audio_packet *packet = NULL;
    if (playback == NULL || playback->queue == NULL) {
        return;
    }
    while (xQueueReceive(playback->queue, &packet, 0) == pdTRUE) {
        if (packet != NULL && packet != AICHAT_PLAYBACK_STOP_PACKET) {
            free(packet->data);
            free(packet);
        }
    }
}

static void cleanup(aichat_audio_playback_t *playback)
{
    if (playback == NULL) {
        return;
    }
    playback->stopping = true;
    if (playback->queue != NULL) {
        struct aichat_audio_packet *stop_packet = AICHAT_PLAYBACK_STOP_PACKET;
        if (xQueueSendToFront(playback->queue, &stop_packet, 0) != pdTRUE) {
            struct aichat_audio_packet *discarded = NULL;
            if (xQueueReceive(playback->queue, &discarded, 0) == pdTRUE &&
                discarded != AICHAT_PLAYBACK_STOP_PACKET && discarded != NULL) {
                free(discarded->data);
                free(discarded);
            }
            (void)xQueueSendToFront(playback->queue, &stop_packet, 0);
        }
    }
    if (playback->pipeline != NULL && playback->running) {
        (void)esp_gmf_pipeline_stop(playback->pipeline);
        playback->running = false;
    }
    if (playback->task != NULL) {
        (void)esp_gmf_task_deinit(playback->task);
        playback->task = NULL;
    }
    if (playback->pipeline != NULL) {
        (void)esp_gmf_pipeline_destroy(playback->pipeline);
        playback->pipeline = NULL;
        playback->decoder = NULL;
        playback->asrc = NULL;
        playback->output_io = NULL;
    }
    drain_queue(playback);
    if (playback->runtime_acquired) {
        (void)audio_runtime_release();
        playback->runtime_acquired = false;
    }
    playback->output_codec = NULL;
}

aichat_audio_playback_t *aichat_audio_playback_create(void)
{
    aichat_audio_playback_t *playback = calloc(1, sizeof(*playback));
    if (playback == NULL) {
        return NULL;
    }
    playback->queue = xQueueCreate(AICHAT_PLAYBACK_QUEUE_LENGTH,
                                   sizeof(struct aichat_audio_packet *));
    if (playback->queue == NULL) {
        free(playback);
        return NULL;
    }
    return playback;
}

esp_err_t aichat_audio_playback_start(aichat_audio_playback_t *playback,
                                      uint32_t sample_rate,
                                      uint32_t frame_duration_ms)
{
    if (playback == NULL || sample_rate == 0U) {
        return ESP_ERR_INVALID_ARG;
    }
    if (playback->running) {
        return ESP_OK;
    }
    playback->stopping = false;

    esp_opus_dec_frame_duration_t duration;
    if (!frame_duration_to_enum(frame_duration_ms, &duration)) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t ret = audio_runtime_acquire();
    if (ret != ESP_OK) {
        return ret;
    }
    playback->runtime_acquired = true;
    playback->output_codec = audio_output_get_codec_dev();
    esp_gmf_pool_handle_t pool = audio_gmf_get_pool();
    if (playback->output_codec == NULL || pool == NULL ||
        esp_gmf_pipeline_create(&playback->pipeline) != ESP_GMF_ERR_OK ||
        esp_gmf_pool_new_io(pool, AICHAT_GMF_CODEC_IO_NAME,
                            ESP_GMF_IO_DIR_WRITER, &playback->output_io) !=
            ESP_GMF_ERR_OK ||
        playback->output_io == NULL ||
        esp_gmf_io_codec_dev_set_dev(playback->output_io,
                                     playback->output_codec) != ESP_GMF_ERR_OK ||
        esp_gmf_pipeline_set_io(playback->pipeline, playback->output_io,
                                ESP_GMF_IO_DIR_WRITER) != ESP_GMF_ERR_OK) {
        cleanup(playback);
        return ESP_FAIL;
    }
    if (esp_gmf_pool_new_element(pool, "aud_dec", &playback->decoder) != ESP_GMF_ERR_OK ||
        esp_gmf_pool_new_element(pool, "aud_asrc", &playback->asrc) != ESP_GMF_ERR_OK) {
        cleanup(playback);
        return ESP_FAIL;
    }

    esp_opus_dec_cfg_t opus_cfg = ESP_OPUS_DEC_CONFIG_DEFAULT();
    opus_cfg.sample_rate = sample_rate;
    opus_cfg.channel = ESP_AUDIO_MONO;
    opus_cfg.frame_duration = duration;
    esp_audio_simple_dec_cfg_t decoder_cfg = {
        .dec_type = ESP_AUDIO_SIMPLE_DEC_TYPE_RAW_OPUS,
        .dec_cfg = &opus_cfg,
        .cfg_size = sizeof(opus_cfg),
        .use_frame_dec = true,
    };
    if (esp_gmf_audio_dec_reconfig(playback->decoder, &decoder_cfg) != ESP_GMF_ERR_OK ||
        esp_gmf_asrc_set_dest_rate(playback->asrc,
                                   AICHAT_PLAYBACK_OUTPUT_SAMPLE_RATE) != ESP_GMF_ERR_OK ||
        esp_gmf_asrc_set_dest_ch(playback->asrc,
                                 AICHAT_PLAYBACK_OUTPUT_CHANNELS) != ESP_GMF_ERR_OK) {
        cleanup(playback);
        return ESP_FAIL;
    }

    /** Create decoder output and ASRC input ports before linking the elements. */
    esp_gmf_port_handle_t intermediate_out = NEW_ESP_GMF_PORT_OUT_BLOCK(
        NULL, NULL, NULL, NULL,
        ESP_GMF_ELEMENT_GET(playback->decoder)->out_attr.data_size,
        ESP_GMF_MAX_DELAY);
    esp_gmf_port_handle_t intermediate_in = NEW_ESP_GMF_PORT_IN_BLOCK(
        NULL, NULL, NULL, NULL,
        ESP_GMF_ELEMENT_GET(playback->asrc)->in_attr.data_size,
        ESP_GMF_MAX_DELAY);
    esp_gmf_payload_t *decoder_output_payload = NULL;
    if (intermediate_out == NULL || intermediate_in == NULL ||
        esp_gmf_payload_new_with_len(AICHAT_PLAYBACK_DECODER_OUTPUT_BUFFER_SIZE,
                                     &decoder_output_payload) != ESP_GMF_ERR_OK ||
        decoder_output_payload == NULL ||
        esp_gmf_port_set_payload(intermediate_out, decoder_output_payload) != ESP_GMF_ERR_OK ||
        esp_gmf_element_register_out_port(playback->decoder, intermediate_out) != ESP_GMF_ERR_OK ||
        esp_gmf_element_register_in_port(playback->asrc, intermediate_in) != ESP_GMF_ERR_OK ||
        esp_gmf_pipeline_register_el(playback->pipeline, playback->decoder) != ESP_GMF_ERR_OK ||
        esp_gmf_pipeline_register_el(playback->pipeline, playback->asrc) != ESP_GMF_ERR_OK) {
        if (decoder_output_payload != NULL && intermediate_out != NULL) {
            /** Once registered, pipeline destruction owns the payload. */
            if (ESP_GMF_ELEMENT_GET(playback->decoder)->out != intermediate_out) {
                esp_gmf_payload_delete(decoder_output_payload);
            }
        } else if (decoder_output_payload != NULL) {
            esp_gmf_payload_delete(decoder_output_payload);
        }
        cleanup(playback);
        return ESP_FAIL;
    }

    esp_gmf_port_handle_t input_port = NEW_ESP_GMF_PORT_IN_BYTE(
        packet_acquire, packet_release, NULL, playback,
        ESP_GMF_ELEMENT_GET(playback->decoder)->in_attr.data_size,
        pdMS_TO_TICKS(100));
    esp_gmf_port_handle_t output_port = NEW_ESP_GMF_PORT_OUT_BYTE(
        esp_gmf_io_acquire_write, esp_gmf_io_release_write, NULL,
        playback->output_io, AICHAT_PLAYBACK_OUTPUT_BUFFER_SIZE,
        ESP_GMF_MAX_DELAY);
    if (input_port == NULL || output_port == NULL ||
        esp_gmf_element_register_in_port(playback->decoder, input_port) != ESP_GMF_ERR_OK ||
        esp_gmf_element_register_out_port(playback->asrc, output_port) != ESP_GMF_ERR_OK) {
        cleanup(playback);
        return ESP_FAIL;
    }

    esp_gmf_task_cfg_t task_cfg = DEFAULT_ESP_GMF_TASK_CONFIG();
    task_cfg.thread.stack = AICHAT_PLAYBACK_TASK_STACK_SIZE;
    task_cfg.thread.stack_in_ext = true;
    task_cfg.name = "aichat_playback";
    if (esp_gmf_task_init(&task_cfg, &playback->task) != ESP_GMF_ERR_OK ||
        esp_gmf_pipeline_bind_task(playback->pipeline, playback->task) != ESP_GMF_ERR_OK ||
        esp_gmf_pipeline_loading_jobs(playback->pipeline) != ESP_GMF_ERR_OK ||
        esp_gmf_pipeline_run(playback->pipeline) != ESP_GMF_ERR_OK) {
        cleanup(playback);
        return ESP_FAIL;
    }
    playback->running = true;
    return ESP_OK;
}

esp_err_t aichat_audio_playback_enqueue(aichat_audio_playback_t *playback,
                                        const uint8_t *data, size_t len)
{
    if (playback == NULL || playback->queue == NULL || data == NULL || len == 0U) {
        return ESP_ERR_INVALID_ARG;
    }
    if (len > AICHAT_PLAYBACK_PACKET_MAX_SIZE) {
        return ESP_ERR_INVALID_SIZE;
    }
    struct aichat_audio_packet *packet = calloc(1, sizeof(*packet));
    if (packet == NULL) {
        return ESP_ERR_NO_MEM;
    }
    packet->data = malloc(len);
    if (packet->data == NULL) {
        free(packet);
        return ESP_ERR_NO_MEM;
    }
    memcpy(packet->data, data, len);
    packet->len = len;
    packet->pts = xTaskGetTickCount();
    if (xQueueSend(playback->queue, &packet, 0) != pdTRUE) {
        free(packet->data);
        free(packet);
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

esp_err_t aichat_audio_playback_stop(aichat_audio_playback_t *playback)
{
    if (playback == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    cleanup(playback);
    return ESP_OK;
}

void aichat_audio_playback_destroy(aichat_audio_playback_t *playback)
{
    if (playback == NULL) {
        return;
    }
    cleanup(playback);
    if (playback->queue != NULL) {
        vQueueDelete(playback->queue);
    }
    free(playback);
}
