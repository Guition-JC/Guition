#include "audio_gmf.h"

#include "audio_runtime_internal.h"
#include "esp_audio_simple_dec.h"
#include "esp_gmf_audio_dec.h"
#include "esp_gmf_element.h"
#include "esp_gmf_err.h"
#include "esp_gmf_io.h"
#include "esp_gmf_io_bt.h"
#include "esp_gmf_io_codec_dev.h"
#include "esp_gmf_io_file.h"
#include "gmf_loader_setup_defaults.h"

static bool s_initialized;
static bool s_io_loader_ready;
static bool s_codec_loader_ready;
static bool s_effects_loader_ready;

static esp_err_t gmf_error(esp_gmf_err_t ret)
{
    return ret == ESP_GMF_ERR_OK ? ESP_OK : ESP_FAIL;
}

static void deinit_unregistered_io(esp_gmf_io_handle_t *io)
{
    if (*io != NULL) {
        (void)esp_gmf_io_deinit(*io);
        *io = NULL;
    }
}

/**
 * @brief Register a parse-mode decoder for continuous MP3, WAV and AAC file streams.
 * @param pool GMF pool in which to register the decoder.
 * @return ESP_OK on success, ESP_ERR_NO_MEM if decoder creation fails, or ESP_FAIL if registration fails.
 */
static esp_err_t register_file_decoder(esp_gmf_pool_handle_t pool)
{
    esp_audio_simple_dec_cfg_t config = DEFAULT_ESP_GMF_AUDIO_DEC_CONFIG();
    /** Probe default: an MP3 stream is the common local case. */
    config.dec_type = ESP_AUDIO_SIMPLE_DEC_TYPE_MP3;
    config.use_frame_dec = false;

    esp_gmf_element_handle_t decoder = NULL;
    if (esp_gmf_audio_dec_init(&config, &decoder) != ESP_GMF_ERR_OK || decoder == NULL) {
        return ESP_ERR_NO_MEM;
    }
    if (esp_gmf_pool_register_element(pool, decoder, AUDIO_GMF_FILE_DECODER_TAG) != ESP_GMF_ERR_OK) {
        (void)esp_gmf_element_deinit(decoder);
        return ESP_FAIL;
    }
    /** The pool owns the element now and deletes it in esp_gmf_pool_deinit(). */
    return ESP_OK;
}

esp_err_t audio_gmf_init(void)
{
    if (s_initialized) {
        return ESP_OK;
    }
    dev_audio_codec_handles_t *dac = audio_runtime_dac_handles();
    if (dac == NULL || dac->codec_dev == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_gmf_pool_handle_t pool = NULL;
    esp_gmf_io_handle_t sd_file_io = NULL;
    esp_gmf_io_handle_t output_io = NULL;
    esp_gmf_io_handle_t bt_io = NULL;
    esp_gmf_io_handle_t bt_codec_io = NULL;
    bool sd_registered = false;
    bool output_registered = false;
    bool bt_registered = false;
    bool bt_codec_registered = false;
    esp_gmf_err_t ret = esp_gmf_pool_init(&pool);
    if (ret != ESP_GMF_ERR_OK) {
        return gmf_error(ret);
    }
    ret = gmf_loader_setup_io_default(pool);
    if (ret == ESP_GMF_ERR_OK) {
        s_io_loader_ready = true;
        ret = gmf_loader_setup_audio_codec_default(pool);
    }
    if (ret == ESP_GMF_ERR_OK) {
        s_codec_loader_ready = true;
        ret = gmf_loader_setup_audio_effects_default(pool);
    }
    if (ret == ESP_GMF_ERR_OK) {
        s_effects_loader_ready = true;
    }
    if (ret == ESP_GMF_ERR_OK) {
        ret = register_file_decoder(pool);
    }
    if (ret != ESP_GMF_ERR_OK) {
        goto fail;
    }

    file_io_cfg_t file_cfg = FILE_IO_CFG_DEFAULT();
    file_cfg.dir = ESP_GMF_IO_DIR_READER;
    file_cfg.name = "sd_io_file";
    ret = esp_gmf_io_file_init(&file_cfg, &sd_file_io);
    if (ret != ESP_GMF_ERR_OK || esp_gmf_pool_register_io(pool, sd_file_io, NULL) != ESP_GMF_ERR_OK) {
        goto fail;
    }
    sd_registered = true;

    codec_dev_io_cfg_t codec_cfg = ESP_GMF_IO_CODEC_DEV_CFG_DEFAULT();
    codec_cfg.dir = ESP_GMF_IO_DIR_WRITER;
    codec_cfg.name = "sd_io_codec_dev";
    codec_cfg.dev = dac->codec_dev;
    ret = esp_gmf_io_codec_dev_init(&codec_cfg, &output_io);
    if (ret != ESP_GMF_ERR_OK || esp_gmf_pool_register_io(pool, output_io, NULL) != ESP_GMF_ERR_OK) {
        goto fail;
    }
    output_registered = true;

    codec_dev_io_cfg_t bt_codec_cfg = ESP_GMF_IO_CODEC_DEV_CFG_DEFAULT();
    bt_codec_cfg.dir = ESP_GMF_IO_DIR_WRITER;
    bt_codec_cfg.name = "music_bt_codec";
    bt_codec_cfg.dev = dac->codec_dev;
    ret = esp_gmf_io_codec_dev_init(&bt_codec_cfg, &bt_codec_io);
    if (ret != ESP_GMF_ERR_OK || esp_gmf_pool_register_io(pool, bt_codec_io, NULL) != ESP_GMF_ERR_OK) {
        goto fail;
    }
    bt_codec_registered = true;

    bt_io_cfg_t bt_cfg = ESP_GMF_BT_IO_CFG_DEFAULT();
    bt_cfg.dir = ESP_GMF_IO_DIR_READER;
    ret = esp_gmf_io_bt_init(&bt_cfg, &bt_io);
    if (ret != ESP_GMF_ERR_OK || esp_gmf_pool_register_io(pool, bt_io, NULL) != ESP_GMF_ERR_OK) {
        goto fail;
    }
    bt_registered = true;

    audio_runtime_set_gmf_resources(pool, sd_file_io, output_io, bt_io);
    s_initialized = true;
    return ESP_OK;

fail:
    if (bt_registered) {
        bt_io = NULL;
    }
    if (bt_codec_registered) {
        bt_codec_io = NULL;
    }
    if (output_registered) {
        output_io = NULL;
    }
    if (sd_registered) {
        sd_file_io = NULL;
    }
    deinit_unregistered_io(&bt_codec_io);
    deinit_unregistered_io(&bt_io);
    deinit_unregistered_io(&output_io);
    deinit_unregistered_io(&sd_file_io);
    if (s_effects_loader_ready) {
        (void)gmf_loader_teardown_audio_effects_default(pool);
        s_effects_loader_ready = false;
    }
    if (s_codec_loader_ready) {
        (void)gmf_loader_teardown_audio_codec_default(pool);
        s_codec_loader_ready = false;
    }
    if (s_io_loader_ready) {
        (void)gmf_loader_teardown_io_default(pool);
        s_io_loader_ready = false;
    }
    (void)esp_gmf_pool_deinit(pool);
    return gmf_error(ret);
}

esp_gmf_pool_handle_t audio_gmf_get_pool(void)
{
    return audio_runtime_pool();
}

esp_gmf_io_handle_t audio_gmf_get_sd_file_io(void)
{
    return audio_runtime_sd_file_io();
}

esp_gmf_io_handle_t audio_gmf_get_output_io(void)
{
    return audio_runtime_output_io();
}

esp_gmf_io_handle_t audio_gmf_get_bt_io(void)
{
    return audio_runtime_bt_io();
}

esp_err_t audio_gmf_deinit(void)
{
    esp_gmf_pool_handle_t pool = audio_runtime_pool();
    if (!s_initialized || pool == NULL) {
        return ESP_OK;
    }
    if (s_effects_loader_ready) {
        (void)gmf_loader_teardown_audio_effects_default(pool);
        s_effects_loader_ready = false;
    }
    if (s_codec_loader_ready) {
        (void)gmf_loader_teardown_audio_codec_default(pool);
        s_codec_loader_ready = false;
    }
    if (s_io_loader_ready) {
        (void)gmf_loader_teardown_io_default(pool);
        s_io_loader_ready = false;
    }
    const esp_err_t result = gmf_error(esp_gmf_pool_deinit(pool));
    if (result == ESP_OK) {
        audio_runtime_clear_gmf_resources();
        s_initialized = false;
    }
    return result;
}
