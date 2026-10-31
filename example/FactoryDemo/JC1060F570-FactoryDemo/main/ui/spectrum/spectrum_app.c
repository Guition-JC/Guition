#include "spectrum_app.h"

#include <stdbool.h>
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "audio_gmf.h"
#include "audio_input.h"
#include "audio_output.h"
#include "audio_runtime.h"
#include "esp_gmf_audio_element.h"
#include "esp_gmf_element.h"
#include "esp_gmf_io.h"
#include "esp_gmf_io_codec_dev.h"
#include "esp_gmf_pipeline.h"
#include "esp_gmf_port.h"
#include "esp_gmf_task.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"

#include "spectrum_fft.h"
#include "spectrum_bars.h"
#include "spectrum_prefs_store.h"

#include "desktop_language.h"

#define TAG "SpectrumApp"
#define SPECTRUM_SAMPLE_RATE 48000U
#define SPECTRUM_CHANNELS 4U
#define SPECTRUM_BITS 16U
/** @brief Capture input block size in bytes for the 48 kHz TDM stream. */
#define SPECTRUM_INPUT_BYTES 4096U
#define SPECTRUM_TASK_STACK 8192U
#define SPECTRUM_TIMER_PERIOD_MS 50U
#define SPECTRUM_INPUT_GAIN_DB 24.0f

/** @brief Minimum reference, active threshold and curve for normalized display magnitudes. */
#define SPECTRUM_DISPLAY_REFERENCE_MIN 0.0040f
#define SPECTRUM_DISPLAY_ACTIVE_MIN 0.00050f
#define SPECTRUM_DISPLAY_SHAPE 1.50f

/** @brief Frequency tilt applied only to visual grouping. */
#define SPECTRUM_DISPLAY_TILT_VISUAL 1.0f

/** @brief Style panel dimensions in pixels, bounded by the screen width. */
#define SPECTRUM_PAGE_PANEL_MARGIN 24
#define SPECTRUM_PAGE_PANEL_W 984
#define SPECTRUM_PAGE_PANEL_H 392
#define SPECTRUM_PAGE_PANEL_RADIUS 16
#define SPECTRUM_PAGE_CARD_H 192
#define SPECTRUM_PAGE_CARD_GAP 24
#define SPECTRUM_PAGE_CARD_INSET 12
#define SPECTRUM_PAGE_ROW_GAP 16
#define SPECTRUM_PAGE_PREVIEW_COUNT 32U
/** @brief Number of columns in a block-style preview. */
#define SPECTRUM_PAGE_PREVIEW_BLOCKS 16U
#define SPECTRUM_PAGE_BUTTON_GROUP_W 236
#define SPECTRUM_PAGE_BUTTON_LINES_W 176
#define SPECTRUM_PAGE_BUTTON_DONE_W 132
#define SPECTRUM_BUTTON_HEIGHT 44
#define SPECTRUM_BUTTON_GAP 16
#define SPECTRUM_HINT_MS 4000U
#define SPECTRUM_HINT_BOTTOM_INSET 24
/** @brief Vertical plot margin in pixels. */
#define SPECTRUM_PLOT_VERTICAL_INSET 16

#define L(key) desktop_language_lookup(key)

#define SPECTRUM_BG lv_color_hex(0x000000)
#define SPECTRUM_GRID lv_color_hex(0x202020)
#define SPECTRUM_BAR_HUE_SATURATION 88U
#define SPECTRUM_BAR_HUE_VALUE 100U
#define SPECTRUM_TEXT lv_color_hex(0xD9ECFF)
#define SPECTRUM_BUTTON_BG lv_color_hex(0x101318)
#define SPECTRUM_BUTTON_BORDER lv_color_hex(0x2A3340)
#define SPECTRUM_ACCENT lv_color_hex(0x147EFB)
/** @brief One-pixel cell borders drawn on both sides of each grid gap. */
#define SPECTRUM_CELL_BORDER lv_color_hex(0xFFFFFF)
#define SPECTRUM_CELL_BORDER_OPA LV_OPA_50

typedef struct spectrum_app spectrum_app_t;

/** @brief Spectrum levels, peak caps and style shared by the plot renderers. */
typedef struct {
    const float *levels;
    const float *caps;
    size_t count;
    spectrum_style_t style;
} spectrum_plot_t;

/** @brief Style preview selection and its owning application. */
typedef struct {
    spectrum_app_t *app;
    spectrum_style_t style;
} spectrum_style_card_t;

typedef struct {
    esp_gmf_audio_element_t parent;
    spectrum_app_t *app;
} spectrum_element_t;

struct spectrum_app {
    lv_obj_t *parent;
    lv_obj_t *canvas;
    lv_timer_t *timer;
    esp_gmf_pipeline_handle_t pipeline;
    esp_gmf_task_handle_t task;
    esp_gmf_io_handle_t input_io;
    esp_gmf_element_handle_t element;
    spectrum_fft_t fft;

    float latest[SPECTRUM_FFT_BINS]; /**< Noise-gated FFT magnitudes published by the GMF task; raw_frame is the timer snapshot. */
    float raw_frame[SPECTRUM_FFT_BINS];

    float levels[SPECTRUM_MAX_BARS]; /**<
     * PSRAM display buffers: magnitudes, smoothed 0..1 levels and peak caps falling at half the bar rate.
     */
    float displayed[SPECTRUM_MAX_BARS];
    float caps[SPECTRUM_MAX_BARS];
    spectrum_bar_map_t bars;
    size_t bar_count;
    int32_t bar_width;

    spectrum_style_t bar_style; /**< Style used to construct the current bar mapping. */
    spectrum_group_t group;
    spectrum_style_t style;

    float tilt_weights[SPECTRUM_MAX_BARS]; /**< Per-bar display weights rebuilt with the grouping map. */
    lv_obj_t *style_page;
    lv_obj_t *style_cards[SPECTRUM_STYLE_COUNT];
    lv_obj_t *style_group_label;
    lv_obj_t *style_lines_label;
    spectrum_style_card_t style_card_data[SPECTRUM_STYLE_COUNT];

    lv_obj_t *grid; /**< Canvas-owned grid overlay; NULL when borders are disabled or the style is not block. */
    bool grid_lines;
    lv_obj_t *hint;
    lv_timer_t *hint_timer;
    uint32_t displayed_windows;
    uint32_t published_windows;
    bool runtime_acquired;
    bool input_opened;
    bool input_gain_changed;
    float previous_input_gain_db;
    bool output_reconfigured;
    bool output_was_open;
    esp_codec_dev_sample_info_t previous_output_format;
    bool running;
};

static spectrum_app_t *s_app;
/** @brief Keep this cross-task critical-section lock in internal SRAM. */
static portMUX_TYPE s_frame_lock = portMUX_INITIALIZER_UNLOCKED;

static esp_gmf_job_err_t spectrum_element_open(esp_gmf_element_handle_t self, void *para)
{
    (void)self;
    (void)para;
    return ESP_GMF_JOB_ERR_OK;
}

static esp_gmf_job_err_t spectrum_element_close(esp_gmf_element_handle_t self, void *para)
{
    (void)self;
    (void)para;
    return ESP_GMF_JOB_ERR_OK;
}

static esp_gmf_job_err_t spectrum_element_reset(esp_gmf_element_handle_t self, void *para)
{
    spectrum_element_t *element = (spectrum_element_t *)self;
    (void)para;
    if (element != NULL && element->app != NULL) {
        spectrum_fft_reset(&element->app->fft);
    }
    return ESP_GMF_JOB_ERR_OK;
}

static esp_gmf_job_err_t spectrum_element_process(esp_gmf_element_handle_t self, void *para)
{
    spectrum_element_t *element = (spectrum_element_t *)self;
    spectrum_app_t *app = element == NULL ? NULL : element->app;
    esp_gmf_port_handle_t in_port = ESP_GMF_ELEMENT_GET(self)->in;
    esp_gmf_payload_t *in_load = NULL;
    (void)para;

    if (app == NULL || in_port == NULL) {
        return ESP_GMF_JOB_ERR_FAIL;
    }

    const esp_gmf_err_io_t acquire_result = esp_gmf_port_acquire_in(
        in_port, &in_load, SPECTRUM_INPUT_BYTES, ESP_GMF_MAX_DELAY);
    if (acquire_result < ESP_GMF_IO_OK) {
        return acquire_result == ESP_GMF_IO_ABORT ? ESP_GMF_JOB_ERR_ABORT : ESP_GMF_JOB_ERR_FAIL;
    }

    esp_gmf_job_err_t result = ESP_GMF_JOB_ERR_OK;
    if (in_load == NULL || in_load->buf == NULL || in_load->valid_size <= 0) {
        result = ESP_GMF_JOB_ERR_FAIL;
    } else {
        (void)spectrum_fft_feed(&app->fft, in_load->buf, (size_t)in_load->valid_size);
        const uint32_t windows = spectrum_fft_windows_processed(&app->fft);
        if (windows != app->published_windows) {
            portENTER_CRITICAL(&s_frame_lock);
            memcpy(app->latest, spectrum_fft_magnitudes(&app->fft), sizeof(app->latest));
            app->published_windows = windows;
            portEXIT_CRITICAL(&s_frame_lock);
        }
        if (in_load->is_done) {
            result = ESP_GMF_JOB_ERR_DONE;
        }
    }

    const esp_gmf_err_io_t release_result = esp_gmf_port_release_in(
        in_port, in_load, ESP_GMF_MAX_DELAY);
    if (release_result < ESP_GMF_IO_OK && release_result != ESP_GMF_IO_ABORT) {
        result = ESP_GMF_JOB_ERR_FAIL;
    }
    return result;
}

static esp_gmf_err_t spectrum_element_destroy(esp_gmf_obj_handle_t self)
{
    spectrum_element_t *element = (spectrum_element_t *)self;
    if (element == NULL) {
        return ESP_GMF_ERR_INVALID_ARG;
    }
    (void)esp_gmf_audio_el_deinit(&element->parent);
    free(element);
    return ESP_GMF_ERR_OK;
}

static esp_gmf_err_t spectrum_element_init(spectrum_app_t *app,
                                            esp_gmf_element_handle_t *out_element)
{
    if (app == NULL || out_element == NULL) {
        return ESP_GMF_ERR_INVALID_ARG;
    }
    *out_element = NULL;
    spectrum_element_t *element = calloc(1, sizeof(*element));
    if (element == NULL) {
        return ESP_GMF_ERR_MEMORY_LACK;
    }

    esp_gmf_obj_t *object = (esp_gmf_obj_t *)element;
    object->del_obj = spectrum_element_destroy;
    esp_gmf_obj_set_tag(object, "spectrum_capture");

    esp_gmf_element_cfg_t config = {0};
    ESP_GMF_ELEMENT_IN_PORT_ATTR_SET(config.in_attr, ESP_GMF_EL_PORT_CAP_SINGLE,
                                     0, 0, ESP_GMF_PORT_TYPE_BYTE,
                                     SPECTRUM_INPUT_BYTES);
    ESP_GMF_ELEMENT_OUT_PORT_ATTR_SET(config.out_attr, ESP_GMF_EL_PORT_CAP_SINGLE,
                                      0, 0, ESP_GMF_PORT_TYPE_BYTE,
                                      SPECTRUM_INPUT_BYTES);
    if (esp_gmf_audio_el_init(&element->parent, &config) != ESP_GMF_ERR_OK) {
        free(element);
        return ESP_GMF_ERR_FAIL;
    }
    element->app = app;
    ESP_GMF_ELEMENT_GET(element)->ops.open = spectrum_element_open;
    ESP_GMF_ELEMENT_GET(element)->ops.process = spectrum_element_process;
    ESP_GMF_ELEMENT_GET(element)->ops.close = spectrum_element_close;
    ESP_GMF_ELEMENT_GET(element)->ops.reset = spectrum_element_reset;
    *out_element = element;
    return ESP_GMF_ERR_OK;
}

static esp_err_t spectrum_pipeline_create(spectrum_app_t *app)
{
    if (esp_gmf_pipeline_create(&app->pipeline) != ESP_GMF_ERR_OK) {
        return ESP_FAIL;
    }

    esp_gmf_pool_handle_t pool = audio_gmf_get_pool();
    esp_codec_dev_handle_t input_codec = audio_input_get_codec_dev();
    esp_gmf_io_handle_t input_io = NULL;
    esp_gmf_element_handle_t element = NULL;
    esp_gmf_port_handle_t input_port = NULL;

    if (pool == NULL || input_codec == NULL ||
        esp_gmf_pool_new_io(pool, "io_codec_dev", ESP_GMF_IO_DIR_READER,
                            &input_io) != ESP_GMF_ERR_OK ||
        input_io == NULL ||
        esp_gmf_io_codec_dev_set_dev(input_io, input_codec) != ESP_GMF_ERR_OK ||
        spectrum_element_init(app, &element) != ESP_GMF_ERR_OK ||
        element == NULL ||
        esp_gmf_pipeline_register_el(app->pipeline, element) != ESP_GMF_ERR_OK) {
        if (element != NULL) {
            (void)spectrum_element_destroy((esp_gmf_obj_handle_t)element);
        }
        if (input_io != NULL) {
            (void)esp_gmf_io_deinit(input_io);
        }
        (void)esp_gmf_pipeline_destroy(app->pipeline);
        app->pipeline = NULL;
        return ESP_FAIL;
    }
    app->element = element;

    input_port = NEW_ESP_GMF_PORT_IN_BYTE(
        esp_gmf_io_acquire_read, esp_gmf_io_release_read, NULL, input_io,
        SPECTRUM_INPUT_BYTES, ESP_GMF_MAX_DELAY);
    if (input_port == NULL ||
        esp_gmf_element_register_in_port(element, input_port) != ESP_GMF_ERR_OK) {
        if (input_port != NULL && ESP_GMF_ELEMENT_GET(element)->in != input_port) {
            (void)esp_gmf_obj_delete((esp_gmf_obj_handle_t)input_port);
        }
        (void)esp_gmf_pipeline_destroy(app->pipeline);
        app->pipeline = NULL;
        app->element = NULL;
        (void)esp_gmf_io_deinit(input_io);
        return ESP_FAIL;
    }

    if (esp_gmf_pipeline_set_io(app->pipeline, input_io, ESP_GMF_IO_DIR_READER) !=
        ESP_GMF_ERR_OK) {
        (void)esp_gmf_pipeline_destroy(app->pipeline);
        app->pipeline = NULL;
        app->input_io = NULL;
        app->element = NULL;
        return ESP_FAIL;
    }
    /** Consume PCM as a sink without pipeline writer IO or output ports. */
    app->input_io = input_io;

    const esp_gmf_info_sound_t sound = {
        .sample_rates = SPECTRUM_SAMPLE_RATE,
        .bits = SPECTRUM_BITS,
        .channels = SPECTRUM_CHANNELS,
    };
    if (esp_gmf_pipeline_report_info(app->pipeline, ESP_GMF_INFO_SOUND,
                                     (void *)&sound, sizeof(sound)) != ESP_GMF_ERR_OK) {
        return ESP_FAIL;
    }
    return ESP_OK;
}

static void spectrum_pipeline_destroy(spectrum_app_t *app)
{
    if (app->pipeline != NULL && app->running) {
        (void)esp_gmf_pipeline_stop(app->pipeline);
        app->running = false;
    }
    if (app->task != NULL) {
        (void)esp_gmf_task_deinit(app->task);
        app->task = NULL;
    }
    if (app->pipeline != NULL) {
        (void)esp_gmf_pipeline_destroy(app->pipeline);
        app->pipeline = NULL;
        app->input_io = NULL;
        app->element = NULL;
    }
}

static void spectrum_restore_input_gain(spectrum_app_t *app)
{
    if (app == NULL || !app->input_gain_changed) {
        return;
    }
    esp_codec_dev_handle_t input_codec = audio_input_get_codec_dev();
    if (input_codec != NULL &&
        esp_codec_dev_set_in_gain(input_codec, app->previous_input_gain_db) != ESP_CODEC_DEV_OK) {
        ESP_LOGW(TAG, "Failed to restore microphone gain to %.1f dB",
                 app->previous_input_gain_db);
    }
    app->input_gain_changed = false;
}

static void spectrum_restore_output(spectrum_app_t *app)
{
    if (app == NULL || !app->output_reconfigured) {
        return;
    }
    esp_err_t ret;
    if (app->output_was_open) {
        ret = audio_output_reconfigure(&app->previous_output_format);
    } else {
        ret = audio_output_close();
    }
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to restore output format: %s", esp_err_to_name(ret));
    }
    app->output_reconfigured = false;
}

static esp_err_t spectrum_pipeline_start(spectrum_app_t *app)
{
    if (audio_runtime_acquire() != ESP_OK) {
        return ESP_ERR_INVALID_STATE;
    }
    app->runtime_acquired = true;

    if (audio_input_is_open()) {
        return ESP_ERR_INVALID_STATE;
    }
    app->output_was_open = audio_output_is_open();
    if (app->output_was_open) {
        esp_err_t ret = audio_output_get_format(&app->previous_output_format);
        if (ret != ESP_OK) {
            return ret;
        }
        /**
         * Match playback to the 48 kHz capture rate on the shared duplex I2S interface; restore changed
         * settings on exit.
         */
        const esp_codec_dev_sample_info_t capture_peer_format = {
            .sample_rate = SPECTRUM_SAMPLE_RATE,
            .channel = 2U,
            .bits_per_sample = SPECTRUM_BITS,
        };
        if (app->previous_output_format.sample_rate != capture_peer_format.sample_rate ||
            app->previous_output_format.channel != capture_peer_format.channel) {
            ret = audio_output_reconfigure(&capture_peer_format);
            if (ret != ESP_OK) {
                return ret;
            }
            app->output_reconfigured = true;
        } else {
            ESP_LOGI(TAG, "Playback already runs at %u Hz; keeping its format",
                     (unsigned)SPECTRUM_SAMPLE_RATE);
        }
    }
    const esp_codec_dev_sample_info_t format = {
        .sample_rate = SPECTRUM_SAMPLE_RATE,
        .channel = SPECTRUM_CHANNELS,
        .bits_per_sample = SPECTRUM_BITS,
    };
    esp_err_t ret = audio_input_open(&format);
    if (ret != ESP_OK) {
        return ret;
    }
    app->input_opened = true;
    esp_codec_dev_handle_t input_codec = audio_input_get_codec_dev();
    if (input_codec == NULL ||
        esp_codec_dev_get_in_gain(input_codec, &app->previous_input_gain_db) != ESP_CODEC_DEV_OK ||
        esp_codec_dev_set_in_gain(input_codec, SPECTRUM_INPUT_GAIN_DB) != ESP_CODEC_DEV_OK) {
        ESP_LOGE(TAG, "Failed to set spectrum microphone gain to %.1f dB",
                 SPECTRUM_INPUT_GAIN_DB);
        return ESP_FAIL;
    }
    app->input_gain_changed = true;
    ESP_LOGI(TAG, "Spectrum microphone gain set to %.1f dB", SPECTRUM_INPUT_GAIN_DB);
    if (!spectrum_fft_init(&app->fft, SPECTRUM_SAMPLE_RATE, SPECTRUM_CHANNELS,
                           SPECTRUM_BITS)) {
        return ESP_ERR_NO_MEM;
    }
    ret = spectrum_pipeline_create(app);
    if (ret != ESP_OK) {
        return ret;
    }

    esp_gmf_task_cfg_t task_cfg = DEFAULT_ESP_GMF_TASK_CONFIG();
    task_cfg.thread.stack = SPECTRUM_TASK_STACK;
    task_cfg.thread.core = 1;
    task_cfg.thread.stack_in_ext = true;
    task_cfg.name = "spectrum_audio";
    if (esp_gmf_task_init(&task_cfg, &app->task) != ESP_GMF_ERR_OK ||
        esp_gmf_pipeline_bind_task(app->pipeline, app->task) != ESP_GMF_ERR_OK ||
        esp_gmf_pipeline_loading_jobs(app->pipeline) != ESP_GMF_ERR_OK ||
        esp_gmf_pipeline_run(app->pipeline) != ESP_GMF_ERR_OK) {
        return ESP_FAIL;
    }
    app->running = true;
    return ESP_OK;
}

static float spectrum_display_level(float magnitude, float reference)
{
    if (magnitude <= 0.0f || reference <= 0.0f) {
        return 0.0f;
    }
    float normalized = magnitude / reference;
    if (normalized > 1.0f) {
        normalized = 1.0f;
    }
    return powf(normalized, SPECTRUM_DISPLAY_SHAPE);
}

/**
 * @brief Display the active frequency grouping.
 * @param group Frequency grouping mode.
 * @return Translation key for the grouping mode.
 */
static desktop_text_key_t spectrum_group_text_key(spectrum_group_t group)
{
    switch (group) {
    case SPECTRUM_GROUP_VISUAL:
        return DESKTOP_TEXT_SPECTRUM_GROUP_VISUAL;
    case SPECTRUM_GROUP_LOG:
        return DESKTOP_TEXT_SPECTRUM_GROUP_LOG;
    case SPECTRUM_GROUP_LINEAR:
    default:
        return DESKTOP_TEXT_SPECTRUM_GROUP_LINEAR;
    }
}

/**
 * @brief Rebuild the bin-to-bar map and display weights using the laid-out canvas width.
 * @param app Spectrum application state.
 * @param count Number of displayed bars.
 */
static void spectrum_bars_rebuild(spectrum_app_t *app, size_t count)
{
    app->bar_count = count;
    memset(app->displayed, 0, sizeof(app->displayed));
    memset(app->caps, 0, sizeof(app->caps));
    memset(app->levels, 0, sizeof(app->levels));
    spectrum_bar_map_build(&app->bars, count, app->group, SPECTRUM_FFT_BINS,
                           SPECTRUM_FFT_SIZE, SPECTRUM_SAMPLE_RATE);
    /** Calculate static frequency tilt weights for the selected grouping. */
    for (size_t i = 0; i < count; ++i) {
        app->tilt_weights[i] = spectrum_bar_tilt_weight(
            app->group,
            spectrum_bar_center_hz(&app->bars, i, SPECTRUM_FFT_BINS, SPECTRUM_SAMPLE_RATE),
            SPECTRUM_DISPLAY_TILT_VISUAL);
    }
}

static void spectrum_bars_sync(spectrum_app_t *app)
{
    if (app == NULL || app->canvas == NULL) {
        return;
    }
    const int32_t width = lv_obj_get_width(app->canvas);
    const size_t count = spectrum_bars_count_for_style(width, app->style);
    if (count == app->bar_count && width == app->bar_width && app->style == app->bar_style) {
        return;
    }
    app->bar_width = width;
    app->bar_style = app->style;
    spectrum_bars_rebuild(app, count);
}

/**
 * @brief Cycle and persist grouping in linear, visual, log order.
 * @param event LVGL control event.
 */
static void spectrum_group_cb(lv_event_t *event)
{
    spectrum_app_t *app = lv_event_get_user_data(event);
    if (app == NULL) {
        return;
    }
    app->group = spectrum_bars_group_next(app->group);
    spectrum_bars_rebuild(app, app->bar_count);
    if (app->style_group_label != NULL) {
        lv_label_set_text(app->style_group_label, L(spectrum_group_text_key(app->group)));
    }
    if (spectrum_prefs_save_group(app->group) != ESP_OK) {
        ESP_LOGW(TAG, "Failed to persist the spectrum grouping %u", (unsigned)app->group);
    }
    lv_obj_invalidate(app->canvas);
}

static lv_obj_t *spectrum_button_create(lv_obj_t *parent, const char *text, int32_t width,
                                        lv_event_cb_t callback, void *user_data,
                                        lv_obj_t **label_out)
{
    lv_obj_t *button = lv_button_create(parent);
    lv_obj_set_size(button, width, SPECTRUM_BUTTON_HEIGHT);
    lv_obj_set_style_bg_color(button, SPECTRUM_BUTTON_BG, 0);
    lv_obj_set_style_bg_opa(button, LV_OPA_80, 0);
    lv_obj_set_style_radius(button, 12, 0);
    lv_obj_set_style_border_width(button, 1, 0);
    lv_obj_set_style_border_color(button, SPECTRUM_BUTTON_BORDER, 0);
    lv_obj_set_style_bg_color(button, SPECTRUM_ACCENT, LV_STATE_PRESSED);
    lv_obj_add_event_cb(button, callback, LV_EVENT_CLICKED, user_data);
    lv_obj_t *label = lv_label_create(button);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_color(label, SPECTRUM_TEXT, 0);
    lv_obj_set_style_text_font(label, lv_obj_get_style_text_font(parent, 0), 0);
    lv_obj_center(label);
    if (label_out != NULL) {
        *label_out = label;
    }
    return button;
}

/**
 * @brief Map the frequency column to a hue shared by all styles.
 * @param index Frequency column index.
 * @param count Total number of columns.
 * @return Color for the column.
 */
static lv_color_t spectrum_bar_color(size_t index, size_t count)
{
    const uint16_t hue = count == 0U ? 0U : (uint16_t)((index * 360U) / count);
    return lv_color_hsv_to_rgb(hue, SPECTRUM_BAR_HUE_SATURATION, SPECTRUM_BAR_HUE_VALUE);
}

/**
 * @brief Draw mirrored solid bars with two-pixel peak caps outside each end.
 * @param layer LVGL drawing layer.
 * @param area Plot bounds.
 * @param plot Levels, peak caps and style to draw.
 */
static void spectrum_plot_draw_bars(lv_layer_t *layer, const lv_area_t *area,
                                    const spectrum_plot_t *plot)
{
    const int32_t width = lv_area_get_width(area);
    const int32_t height = lv_area_get_height(area);
    const int32_t baseline_y = area->y1 + height / 2;
    const int32_t plot_height = height / 2 - 8;
    if (width <= 0 || plot_height <= 0) {
        return;
    }

    lv_draw_line_dsc_t baseline;
    lv_draw_line_dsc_init(&baseline);
    baseline.color = SPECTRUM_GRID;
    baseline.width = 1;
    baseline.opa = LV_OPA_60;
    baseline.p1.x = area->x1;
    baseline.p1.y = baseline_y;
    baseline.p2.x = area->x2;
    baseline.p2.y = baseline_y;
    lv_draw_line(layer, &baseline);

    for (size_t i = 0; i < plot->count; ++i) {
        int32_t x1 = area->x1;
        int32_t x2 = area->x1;
        spectrum_bars_slot(area->x1, width, i, plot->count, &x1, &x2);
        const int32_t bar_height = spectrum_bar_height(plot->levels[i], plot_height);
        const int32_t cap_height = spectrum_bar_cap_height(plot->caps[i], plot_height);
        const lv_color_t color = spectrum_bar_color(i, plot->count);

        lv_draw_rect_dsc_t bar;
        lv_draw_rect_dsc_init(&bar);
        bar.bg_color = color;
        bar.bg_opa = LV_OPA_COVER;
        bar.radius = 0;
        const lv_area_t bar_area = {
            .x1 = x1,
            .y1 = baseline_y - bar_height,
            .x2 = x2,
            .y2 = baseline_y - 1,
        };
        lv_draw_rect(layer, &bar, &bar_area);

        lv_draw_rect_dsc_t mirror;
        lv_draw_rect_dsc_init(&mirror);
        mirror.bg_color = color;
        mirror.bg_opa = LV_OPA_50;
        mirror.radius = 0;
        const lv_area_t mirror_area = {
            .x1 = x1,
            .y1 = baseline_y + 1,
            .x2 = x2,
            .y2 = baseline_y + bar_height,
        };
        lv_draw_rect(layer, &mirror, &mirror_area);

        const lv_area_t cap_area = {
            .x1 = x1,
            .y1 = baseline_y - cap_height - SPECTRUM_PEAK_CAP_HEIGHT,
            .x2 = x2,
            .y2 = baseline_y - cap_height - 1,
        };
        lv_draw_rect(layer, &bar, &cap_area);

        const lv_area_t mirror_cap_area = {
            .x1 = x1,
            .y1 = baseline_y + cap_height + 1,
            .x2 = x2,
            .y2 = baseline_y + cap_height + SPECTRUM_PEAK_CAP_HEIGHT,
        };
        lv_draw_rect(layer, &mirror, &mirror_cap_area);
    }
}

/** @brief Draw bottom-anchored dot columns by cutting background seams through solid bars and caps. */

/** @brief Dot-column slot, lit extent and peak-cap row. */
typedef struct {
    int32_t slot_x1;
    int32_t slot_x2;
    int32_t last_x2;
    int32_t bar_top;
    int32_t cap_y1;
    int32_t cap_y2;
} spectrum_dot_column_t;

static void spectrum_dot_column(const spectrum_plot_t *plot, const lv_area_t *area,
                                int32_t width, int32_t bottom, int32_t rows, size_t index,
                                spectrum_dot_column_t *out)
{
    int32_t slot_x1 = area->x1;
    int32_t slot_x2 = area->x1;
    spectrum_slot_bounds(area->x1, width, index, plot->count, &slot_x1, &slot_x2);
    /** Leave the slot gap outside the final dot. */
    int32_t last_x1 = slot_x1;
    int32_t last_x2 = slot_x2;
    spectrum_dot_span(slot_x1, slot_x2, SPECTRUM_DOTS_PER_SLOT - 1U,
                      SPECTRUM_DOTS_PER_SLOT, &last_x1, &last_x2);

    const int32_t plot_height = bottom - area->y1 + 1;
    const int32_t lit =
        spectrum_bar_dot_height(plot->levels[index], plot_height, SPECTRUM_DOT_PITCH);
    const int32_t cap_lit =
        spectrum_bar_dot_height(plot->caps[index], plot_height, SPECTRUM_DOT_PITCH);
    int32_t cap_row = cap_lit / SPECTRUM_DOT_PITCH;
    if (cap_row > rows - 1) {
        cap_row = rows - 1;
    }

    out->slot_x1 = slot_x1;
    out->slot_x2 = slot_x2;
    out->last_x2 = last_x2;
    out->bar_top = bottom - lit + 1;
    out->cap_y2 = bottom - cap_row * SPECTRUM_DOT_PITCH;
    out->cap_y1 = out->cap_y2 - SPECTRUM_DOT_SIZE + 1;
}

static void spectrum_plot_draw_dot_matrix(lv_layer_t *layer, const lv_area_t *area,
                                          const spectrum_plot_t *plot)
{
    const int32_t width = lv_area_get_width(area);
    const int32_t bottom = area->y2;
    if (width <= 0 || plot->count == 0U) {
        return;
    }
    const int32_t plot_height = bottom - area->y1 + 1;
    const int32_t rows = (int32_t)spectrum_dot_rows(plot_height);
    if (rows <= 0) {
        return;
    }

    /** Paint solid columns and caps before cutting dot seams. */
    lv_draw_rect_dsc_t column_dsc;
    lv_draw_rect_dsc_init(&column_dsc);
    column_dsc.bg_opa = LV_OPA_COVER;
    column_dsc.radius = 0;

    int32_t content_top = bottom;
    for (size_t i = 0; i < plot->count; ++i) {
        spectrum_dot_column_t column;
        spectrum_dot_column(plot, area, width, bottom, rows, i, &column);
        column_dsc.bg_color = spectrum_bar_color(i, plot->count);

        const lv_area_t bar_area = {
            .x1 = column.slot_x1,
            .y1 = column.bar_top,
            .x2 = column.last_x2,
            .y2 = bottom,
        };
        lv_draw_rect(layer, &column_dsc, &bar_area);

        const lv_area_t cap_area = {
            .x1 = column.slot_x1,
            .y1 = column.cap_y1,
            .x2 = column.last_x2,
            .y2 = column.cap_y2,
        };
        lv_draw_rect(layer, &column_dsc, &cap_area);

        const int32_t top = column.bar_top < column.cap_y1 ? column.bar_top : column.cap_y1;
        if (top < content_top) {
            content_top = top;
        }
    }

    /** Cut horizontal seams up to the tallest column. */
    lv_draw_rect_dsc_t seam;
    lv_draw_rect_dsc_init(&seam);
    seam.bg_color = SPECTRUM_BG;
    seam.bg_opa = LV_OPA_COVER;
    seam.radius = 0;
    for (int32_t y = bottom - SPECTRUM_DOT_SIZE; y >= content_top; y -= SPECTRUM_DOT_PITCH) {
        const lv_area_t seam_area = {.x1 = area->x1, .y1 = y, .x2 = area->x2, .y2 = y};
        lv_draw_rect(layer, &seam, &seam_area);
    }

    /** Cut vertical seams through each column and its cap. */
    for (size_t i = 0; i < plot->count; ++i) {
        spectrum_dot_column_t column;
        spectrum_dot_column(plot, area, width, bottom, rows, i, &column);
        const int32_t column_top =
            column.bar_top < column.cap_y1 ? column.bar_top : column.cap_y1;
        for (size_t dot = 0; dot + 1U < SPECTRUM_DOTS_PER_SLOT; ++dot) {
            int32_t dot_x1 = column.slot_x1;
            int32_t dot_x2 = column.slot_x2;
            spectrum_dot_span(column.slot_x1, column.slot_x2, dot, SPECTRUM_DOTS_PER_SLOT,
                              &dot_x1, &dot_x2);
            const int32_t seam_x = dot_x2 + SPECTRUM_DOT_GAP;
            const lv_area_t seam_area = {
                .x1 = seam_x,
                .y1 = column_top,
                .x2 = seam_x + SPECTRUM_DOT_GAP - 1,
                .y2 = bottom,
            };
            lv_draw_rect(layer, &seam, &seam_area);
        }
    }
}

/**
 * @brief Draw bottom-anchored cell columns with two-pixel gaps and half-cell peak caps.
 * @param layer LVGL drawing layer.
 * @param area Plot bounds.
 * @param plot Levels, peak caps and style to draw.
 */
static void spectrum_plot_draw_blocks(lv_layer_t *layer, const lv_area_t *area,
                                      const spectrum_plot_t *plot)
{
    const int32_t width = lv_area_get_width(area);
    const int32_t bottom = area->y2;
    if (width <= 0 || plot->count == 0U) {
        return;
    }
    const int32_t plot_height = bottom - area->y1 + 1;
    const size_t rows = spectrum_cell_rows(plot_height);
    if (rows == 0U) {
        return;
    }
    const int32_t cap_height = spectrum_cell_cap_height(plot_height, rows);

    lv_draw_rect_dsc_t column_dsc;
    lv_draw_rect_dsc_init(&column_dsc);
    column_dsc.bg_opa = LV_OPA_COVER;
    column_dsc.radius = 0;

    /** Paint columns and track their maximum height. */
    size_t tallest = 1U;
    int32_t content_top = bottom;
    for (size_t i = 0; i < plot->count; ++i) {
        int32_t slot_x1 = area->x1;
        int32_t slot_x2 = area->x1;
        spectrum_slot_bounds(area->x1, width, i, plot->count, &slot_x1, &slot_x2);
        const size_t lit = spectrum_cells_lit(plot->levels[i], rows);
        int32_t top = area->y1;
        int32_t cell_bottom = bottom;
        spectrum_cell_row(area->y1, bottom, lit - 1U, rows, &top, &cell_bottom);
        column_dsc.bg_color = spectrum_bar_color(i, plot->count);
        const lv_area_t bar_area = {
            .x1 = slot_x1,
            .y1 = top,
            .x2 = slot_x2,
            .y2 = bottom,
        };
        lv_draw_rect(layer, &column_dsc, &bar_area);
        if (lit > tallest) {
            tallest = lit;
        }
        if (top < content_top) {
            content_top = top;
        }
    }

    /** Cut horizontal gaps through occupied cell rows. */
    lv_draw_rect_dsc_t gap;
    lv_draw_rect_dsc_init(&gap);
    gap.bg_color = SPECTRUM_BG;
    gap.bg_opa = LV_OPA_COVER;
    gap.radius = 0;
    for (size_t row = 1U; row < tallest; ++row) {
        const int32_t band_bottom =
            bottom - (int32_t)(((int64_t)row * plot_height) / (int64_t)rows);
        const lv_area_t gap_area = {
            .x1 = area->x1,
            .y1 = band_bottom + 1,
            .x2 = area->x2,
            .y2 = band_bottom + SPECTRUM_CELL_GAP,
        };
        lv_draw_rect(layer, &gap, &gap_area);
    }

    /** Cut vertical gaps between cell columns. */
    for (size_t i = 0; i < plot->count; ++i) {
        int32_t slot_x1 = area->x1;
        int32_t slot_x2 = area->x1;
        int32_t cell_x1 = area->x1;
        int32_t cell_x2 = area->x1;
        spectrum_slot_bounds(area->x1, width, i, plot->count, &slot_x1, &slot_x2);
        spectrum_cell_span(slot_x1, slot_x2, &cell_x1, &cell_x2);
        const lv_area_t gap_area = {
            .x1 = cell_x2 + 1,
            .y1 = content_top,
            .x2 = slot_x2,
            .y2 = bottom,
        };
        lv_draw_rect(layer, &gap, &gap_area);
    }

    /** Draw half-cell peak caps above the columns. */
    for (size_t i = 0; i < plot->count; ++i) {
        int32_t slot_x1 = area->x1;
        int32_t slot_x2 = area->x1;
        int32_t cell_x1 = area->x1;
        int32_t cell_x2 = area->x1;
        spectrum_slot_bounds(area->x1, width, i, plot->count, &slot_x1, &slot_x2);
        spectrum_cell_span(slot_x1, slot_x2, &cell_x1, &cell_x2);
        /** Place the cap in the lower half of the row above its peak level. */
        const size_t cap_cells = spectrum_cells_lit(plot->caps[i], rows);
        size_t cap_row = cap_cells;
        if (cap_row > rows - 1U) {
            cap_row = rows - 1U;
        }
        int32_t cap_cell_y1 = area->y1;
        int32_t cap_y2 = bottom;
        spectrum_cell_row(area->y1, bottom, cap_row, rows, &cap_cell_y1, &cap_y2);
        int32_t cap_y1 = cap_y2 - cap_height + 1;
        if (cap_y1 < cap_cell_y1) {
            cap_y1 = cap_cell_y1;
        }
        column_dsc.bg_color = spectrum_bar_color(i, plot->count);
        const lv_area_t cap_area = {
            .x1 = cell_x1,
            .y1 = cap_y1,
            .x2 = cell_x2,
            .y2 = cap_y2,
        };
        lv_draw_rect(layer, &column_dsc, &cap_area);
    }
}

static void spectrum_plot_draw(lv_layer_t *layer, const lv_area_t *area,
                               const spectrum_plot_t *plot)
{
    if (layer == NULL || area == NULL || plot == NULL || plot->levels == NULL ||
        plot->caps == NULL || plot->count == 0U) {
        return;
    }
    switch (plot->style) {
    case SPECTRUM_STYLE_DOT_MATRIX:
        spectrum_plot_draw_dot_matrix(layer, area, plot);
        break;
    case SPECTRUM_STYLE_BLOCKS:
        spectrum_plot_draw_blocks(layer, area, plot);
        break;
    case SPECTRUM_STYLE_BARS:
    default:
        spectrum_plot_draw_bars(layer, area, plot);
        break;
    }
}

/**
 * @brief Draw one-pixel borders around each cell gap across the complete plot.
 * @param layer LVGL drawing layer.
 * @param area Plot bounds.
 * @param count Number of columns.
 */
static void spectrum_cell_grid_draw(lv_layer_t *layer, const lv_area_t *area,
                                    size_t count)
{
    const int32_t width = lv_area_get_width(area);
    const int32_t bottom = area->y2;
    if (width <= 0 || count == 0U) {
        return;
    }
    const int32_t plot_height = bottom - area->y1 + 1;
    const size_t rows = spectrum_cell_rows(plot_height);
    if (rows == 0U) {
        return;
    }

    lv_draw_rect_dsc_t line;
    lv_draw_rect_dsc_init(&line);
    line.bg_color = SPECTRUM_CELL_BORDER;
    line.bg_opa = SPECTRUM_CELL_BORDER_OPA;
    line.radius = 0;

    /** Draw both horizontal edges of each cell row. */
    for (size_t row = 0; row < rows; ++row) {
        int32_t cell_y1 = area->y1;
        int32_t cell_y2 = bottom;
        spectrum_cell_row(area->y1, bottom, row, rows, &cell_y1, &cell_y2);
        const int32_t edges[2] = {cell_y1, cell_y2};
        for (size_t edge = 0; edge < 2U; ++edge) {
            const lv_area_t rect = {
                .x1 = area->x1,
                .y1 = edges[edge],
                .x2 = area->x2,
                .y2 = edges[edge],
            };
            lv_draw_rect(layer, &line, &rect);
        }
    }

    /** Draw both vertical edges of each cell column. */
    for (size_t i = 0; i < count; ++i) {
        int32_t slot_x1 = area->x1;
        int32_t slot_x2 = area->x1;
        int32_t cell_x1 = area->x1;
        int32_t cell_x2 = area->x1;
        spectrum_slot_bounds(area->x1, width, i, count, &slot_x1, &slot_x2);
        spectrum_cell_span(slot_x1, slot_x2, &cell_x1, &cell_x2);
        const int32_t edges[2] = {cell_x1, cell_x2};
        for (size_t edge = 0; edge < 2U; ++edge) {
            const lv_area_t rect = {
                .x1 = edges[edge],
                .y1 = area->y1,
                .x2 = edges[edge],
                .y2 = bottom,
            };
            lv_draw_rect(layer, &line, &rect);
        }
    }
}

static void spectrum_grid_draw_cb(lv_event_t *event)
{
    if (event == NULL || lv_event_get_code(event) != LV_EVENT_DRAW_MAIN) {
        return;
    }
    lv_obj_t *grid = lv_event_get_target_obj(event);
    lv_layer_t *layer = lv_event_get_layer(event);
    if (grid == NULL || layer == NULL) {
        return;
    }
    lv_area_t area;
    lv_obj_get_coords(grid, &area);
    spectrum_cell_grid_draw(
        layer, &area,
        spectrum_bars_count_for_style(lv_area_get_width(&area), SPECTRUM_STYLE_BLOCKS));
}

/**
 * @brief Create or delete the grid overlay for the active style and preference; call outside draw callbacks.
 */
static void spectrum_grid_sync(spectrum_app_t *app)
{
    const bool wanted = app->style == SPECTRUM_STYLE_BLOCKS && app->grid_lines;
    if (wanted == (app->grid != NULL)) {
        return;
    }
    if (!wanted) {
        lv_obj_delete(app->grid);
        app->grid = NULL;
        return;
    }

    /** Attach the grid to the canvas so it shares the canvas geometry and draw pass. */
    lv_obj_t *grid = lv_obj_create(app->canvas);
    app->grid = grid;
    lv_obj_set_size(grid, LV_PCT(100), LV_PCT(100));
    lv_obj_center(grid);
    lv_obj_set_style_bg_opa(grid, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(grid, 0, 0);
    lv_obj_set_style_radius(grid, 0, 0);
    lv_obj_set_style_pad_all(grid, 0, 0);
    /** Pass touch input through to the canvas long-press handler. */
    lv_obj_clear_flag(grid, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(grid, spectrum_grid_draw_cb, LV_EVENT_DRAW_MAIN, NULL);
    /** Keep the grid below the style page and hint. */
    lv_obj_move_to_index(grid, 0);
    ESP_LOGI(TAG, "Spectrum cell borders shown");
}

/**
 * @brief Choose the preview column count for the selected style.
 * @param style Display style to preview.
 * @return Number of preview columns.
 */
static size_t spectrum_preview_count(spectrum_style_t style)
{
    return style == SPECTRUM_STYLE_BLOCKS ? SPECTRUM_PAGE_PREVIEW_BLOCKS
                                          : SPECTRUM_PAGE_PREVIEW_COUNT;
}

/**
 * @brief Generate a fixed descending spectrum with one peak for style previews.
 * @param [out] levels Array receiving preview magnitudes.
 * @param [out] caps Array receiving preview peak levels.
 * @param count Number of entries in each array.
 */
static void spectrum_preview_levels(float *levels, float *caps, size_t count)
{
    for (size_t i = 0; i < count; ++i) {
        const float position = (float)i;
        const float offset = position - 6.0f;
        float level = 0.92f - 0.030f * position;
        level += 0.30f * expf(-0.12f * offset * offset);
        if (level < 0.12f) {
            level = 0.12f;
        }
        if (level > 1.0f) {
            level = 1.0f;
        }
        levels[i] = level;
        caps[i] = level;
    }
}

static void spectrum_preview_draw_cb(lv_event_t *event)
{
    if (event == NULL || lv_event_get_code(event) != LV_EVENT_DRAW_MAIN) {
        return;
    }
    spectrum_style_card_t *card = lv_event_get_user_data(event);
    lv_obj_t *preview = lv_event_get_target_obj(event);
    lv_layer_t *layer = lv_event_get_layer(event);
    if (card == NULL || preview == NULL || layer == NULL) {
        return;
    }

    lv_area_t area;
    lv_obj_get_coords(preview, &area);
    lv_draw_rect_dsc_t background;
    lv_draw_rect_dsc_init(&background);
    background.bg_color = SPECTRUM_BG;
    background.bg_opa = LV_OPA_COVER;
    background.radius = 0;
    lv_draw_rect(layer, &background, &area);

    float levels[SPECTRUM_PAGE_PREVIEW_COUNT];
    float caps[SPECTRUM_PAGE_PREVIEW_COUNT];
    const size_t count = spectrum_preview_count(card->style);
    spectrum_preview_levels(levels, caps, count);
    const spectrum_plot_t plot = {
        .levels = levels,
        .caps = caps,
        .count = count,
        .style = card->style,
    };
    spectrum_plot_draw(layer, &area, &plot);
    /** Render the same cell borders used by the full-size plot. */
    if (plot.style == SPECTRUM_STYLE_BLOCKS && card->app != NULL &&
        card->app->grid_lines) {
        spectrum_cell_grid_draw(layer, &area, count);
    }
}

/**
 * @brief Select the translation key for a spectrum display style.
 * @param style Spectrum display style.
 * @return Translation key for the style.
 */
static desktop_text_key_t spectrum_style_text_key(spectrum_style_t style)
{
    switch (style) {
    case SPECTRUM_STYLE_DOT_MATRIX:
        return DESKTOP_TEXT_SPECTRUM_STYLE_DOT_MATRIX;
    case SPECTRUM_STYLE_BLOCKS:
        return DESKTOP_TEXT_SPECTRUM_STYLE_BLOCKS;
    case SPECTRUM_STYLE_BARS:
    default:
        return DESKTOP_TEXT_SPECTRUM_STYLE_BARS;
    }
}

/**
 * @brief Select the translation key for the block-grid border setting.
 * @param visible Whether grid lines are visible.
 * @return Translation key for the current visibility setting.
 */
static desktop_text_key_t spectrum_lines_text_key(bool visible)
{
    return visible ? DESKTOP_TEXT_SPECTRUM_LINES_ON : DESKTOP_TEXT_SPECTRUM_LINES_OFF;
}

static void spectrum_lines_cb(lv_event_t *event)
{
    spectrum_app_t *app = lv_event_get_user_data(event);
    if (app == NULL) {
        return;
    }
    app->grid_lines = !app->grid_lines;
    if (app->style_lines_label != NULL) {
        lv_label_set_text(app->style_lines_label, L(spectrum_lines_text_key(app->grid_lines)));
    }
    /** Update the grid overlay and refresh previews. */
    spectrum_grid_sync(app);
    if (spectrum_prefs_save_lines(app->grid_lines) != ESP_OK) {
        ESP_LOGW(TAG, "Failed to persist the spectrum cell borders");
    }
    ESP_LOGI(TAG, "Spectrum cell borders %s", app->grid_lines ? "shown" : "hidden");
    lv_obj_invalidate(app->canvas);
}

static void spectrum_style_page_refresh(spectrum_app_t *app)
{
    for (size_t i = 0; i < SPECTRUM_STYLE_COUNT; ++i) {
        lv_obj_t *card = app->style_cards[i];
        if (card == NULL) {
            continue;
        }
        const bool selected = (spectrum_style_t)i == app->style;
        lv_obj_set_style_border_color(
            card, selected ? SPECTRUM_ACCENT : SPECTRUM_BUTTON_BORDER, 0);
        lv_obj_set_style_border_width(card, selected ? 2 : 1, 0);
    }
}

static void spectrum_style_page_close(spectrum_app_t *app)
{
    if (app->style_page != NULL) {
        lv_obj_delete(app->style_page);
        app->style_page = NULL;
    }
    for (size_t i = 0; i < SPECTRUM_STYLE_COUNT; ++i) {
        app->style_cards[i] = NULL;
    }
    app->style_group_label = NULL;
    app->style_lines_label = NULL;
    lv_obj_invalidate(app->canvas);
}

static void spectrum_style_done_cb(lv_event_t *event)
{
    spectrum_app_t *app = lv_event_get_user_data(event);
    if (app != NULL) {
        spectrum_style_page_close(app);
    }
}

/**
 * @brief Close the style page only when the scrim itself is tapped.
 * @param event LVGL backdrop event.
 */
static void spectrum_style_backdrop_cb(lv_event_t *event)
{
    spectrum_app_t *app = lv_event_get_user_data(event);
    if (app == NULL) {
        return;
    }
    if (lv_event_get_target_obj(event) == app->style_page) {
        spectrum_style_page_close(app);
    }
}

/**
 * @brief Apply and persist the selected style immediately.
 * @param event LVGL style-selection event.
 */
static void spectrum_style_pick_cb(lv_event_t *event)
{
    spectrum_style_card_t *card = lv_event_get_user_data(event);
    if (card == NULL || card->app == NULL) {
        return;
    }
    spectrum_app_t *app = card->app;
    app->style = card->style;
    /** Update the grid overlay for the selected style. */
    spectrum_grid_sync(app);
    spectrum_style_page_refresh(app);
    if (spectrum_prefs_save_style(app->style) != ESP_OK) {
        ESP_LOGW(TAG, "Failed to persist the spectrum style %u", (unsigned)app->style);
    }
    ESP_LOGI(TAG, "Spectrum style set to %u", (unsigned)app->style);
    lv_obj_invalidate(app->canvas);
}

/**
 * @brief Create a transparent flex row for responsive style controls.
 * @param parent Parent object.
 * @param width Row width.
 * @param height Row height.
 * @return Created flex row object.
 */
static lv_obj_t *spectrum_page_row(lv_obj_t *parent, int32_t width, int32_t height)
{
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_set_size(row, width, height);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_radius(row, 0, 0);
    lv_obj_set_style_pad_all(row, 0, 0);
    lv_obj_set_style_pad_column(row, SPECTRUM_PAGE_CARD_GAP, 0);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    return row;
}

static void spectrum_style_page_open(spectrum_app_t *app)
{
    if (app == NULL || app->parent == NULL || app->style_page != NULL) {
        return;
    }

    /** Attach the overlay to the canvas without changing the parent flex layout. */
    lv_obj_t *page = lv_obj_create(app->canvas);
    app->style_page = page;
    lv_obj_set_size(page, LV_PCT(100), LV_PCT(100));
    lv_obj_center(page);
    /** Cover the paused spectrum with an opaque style page. */
    lv_obj_set_style_bg_color(page, SPECTRUM_BG, 0);
    lv_obj_set_style_bg_opa(page, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(page, 0, 0);
    lv_obj_set_style_radius(page, 0, 0);
    lv_obj_set_style_pad_all(page, 0, 0);
    lv_obj_clear_flag(page, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(page, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(page, spectrum_style_backdrop_cb, LV_EVENT_CLICKED, app);

    /** Size the panel and preview cards to fit the screen. */
    const int32_t screen_width = lv_obj_get_width(app->parent);
    int32_t panel_width = screen_width - SPECTRUM_PAGE_PANEL_MARGIN * 2;
    if (panel_width > SPECTRUM_PAGE_PANEL_W) {
        panel_width = SPECTRUM_PAGE_PANEL_W;
    }
    if (panel_width <= SPECTRUM_PAGE_ROW_GAP * 2) {
        panel_width = screen_width;
    }
    const int32_t cards_width = panel_width - SPECTRUM_PAGE_ROW_GAP * 2;
    const int32_t card_width =
        (cards_width - SPECTRUM_PAGE_CARD_GAP * (int32_t)(SPECTRUM_STYLE_COUNT - 1U)) /
        (int32_t)SPECTRUM_STYLE_COUNT;

    lv_obj_t *panel = lv_obj_create(page);
    lv_obj_set_size(panel, panel_width, SPECTRUM_PAGE_PANEL_H);
    lv_obj_center(panel);
    lv_obj_set_style_bg_color(panel, SPECTRUM_BUTTON_BG, 0);
    lv_obj_set_style_bg_opa(panel, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(panel, 1, 0);
    lv_obj_set_style_border_color(panel, SPECTRUM_BUTTON_BORDER, 0);
    lv_obj_set_style_radius(panel, SPECTRUM_PAGE_PANEL_RADIUS, 0);
    lv_obj_set_style_pad_all(panel, SPECTRUM_PAGE_ROW_GAP, 0);
    lv_obj_set_style_pad_row(panel, SPECTRUM_PAGE_ROW_GAP, 0);
    lv_obj_clear_flag(panel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(panel, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(panel, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);

    lv_obj_t *title = lv_label_create(panel);
    lv_label_set_text(title, L(DESKTOP_TEXT_SPECTRUM_STYLE_TITLE));
    lv_obj_set_style_text_color(title, SPECTRUM_TEXT, 0);
    lv_obj_set_style_text_font(title, lv_obj_get_style_text_font(app->parent, 0), 0);

    lv_obj_t *cards = spectrum_page_row(panel, cards_width, SPECTRUM_PAGE_CARD_H);
    for (size_t i = 0; i < SPECTRUM_STYLE_COUNT; ++i) {
        spectrum_style_card_t *card_data = &app->style_card_data[i];
        card_data->app = app;
        card_data->style = (spectrum_style_t)i;

        lv_obj_t *card = lv_obj_create(cards);
        app->style_cards[i] = card;
        lv_obj_set_size(card, card_width, SPECTRUM_PAGE_CARD_H);
        lv_obj_set_style_bg_color(card, SPECTRUM_BG, 0);
        lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(card, 1, 0);
        lv_obj_set_style_border_color(card, SPECTRUM_BUTTON_BORDER, 0);
        lv_obj_set_style_radius(card, 10, 0);
        lv_obj_set_style_pad_all(card, 6, 0);
        lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(card, spectrum_style_pick_cb, LV_EVENT_CLICKED, card_data);

        lv_obj_t *name = lv_label_create(card);
        lv_label_set_text(name, L(spectrum_style_text_key((spectrum_style_t)i)));
        lv_obj_set_style_text_color(name, SPECTRUM_TEXT, 0);
        lv_obj_set_style_text_font(name, lv_obj_get_style_text_font(app->parent, 0), 0);
        lv_obj_align(name, LV_ALIGN_TOP_MID, 0, 0);

        lv_obj_t *preview = lv_obj_create(card);
        lv_obj_set_size(preview, card_width - SPECTRUM_PAGE_CARD_INSET * 2,
                        SPECTRUM_PAGE_CARD_H - 46);
        lv_obj_align(preview, LV_ALIGN_BOTTOM_MID, 0, 0);
        lv_obj_set_style_bg_color(preview, SPECTRUM_BG, 0);
        lv_obj_set_style_bg_opa(preview, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(preview, 0, 0);
        lv_obj_set_style_radius(preview, 4, 0);
        lv_obj_set_style_pad_all(preview, 0, 0);
        lv_obj_clear_flag(preview, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(preview, spectrum_preview_draw_cb, LV_EVENT_DRAW_MAIN, card_data);
    }

    lv_obj_t *buttons = spectrum_page_row(
        panel, SPECTRUM_PAGE_BUTTON_GROUP_W + SPECTRUM_BUTTON_GAP +
                   SPECTRUM_PAGE_BUTTON_LINES_W + SPECTRUM_BUTTON_GAP +
                   SPECTRUM_PAGE_BUTTON_DONE_W,
        SPECTRUM_BUTTON_HEIGHT);
    lv_obj_set_style_pad_column(buttons, SPECTRUM_BUTTON_GAP, 0);
    (void)spectrum_button_create(buttons, L(spectrum_group_text_key(app->group)),
                                 SPECTRUM_PAGE_BUTTON_GROUP_W, spectrum_group_cb, app,
                                 &app->style_group_label);
    (void)spectrum_button_create(buttons, L(spectrum_lines_text_key(app->grid_lines)),
                                 SPECTRUM_PAGE_BUTTON_LINES_W, spectrum_lines_cb, app,
                                 &app->style_lines_label);
    (void)spectrum_button_create(buttons, L(DESKTOP_TEXT_SPECTRUM_STYLE_DONE),
                                 SPECTRUM_PAGE_BUTTON_DONE_W, spectrum_style_done_cb, app,
                                 NULL);

    spectrum_style_page_refresh(app);
    lv_obj_move_foreground(page);
    ESP_LOGI(TAG, "Spectrum style page opened");
}

static void spectrum_long_press_cb(lv_event_t *event)
{
    spectrum_app_t *app = lv_event_get_user_data(event);
    if (app != NULL) {
        spectrum_style_page_open(app);
    }
}

static void spectrum_draw_cb(lv_event_t *event)
{
    if (event == NULL || lv_event_get_code(event) != LV_EVENT_DRAW_MAIN) {
        return;
    }
    spectrum_app_t *app = lv_event_get_user_data(event);
    lv_obj_t *canvas = lv_event_get_target_obj(event);
    lv_layer_t *layer = lv_event_get_layer(event);
    if (app == NULL || canvas == NULL || layer == NULL) {
        return;
    }

    lv_area_t area;
    lv_obj_get_coords(canvas, &area);
    lv_draw_rect_dsc_t background;
    lv_draw_rect_dsc_init(&background);
    background.bg_color = SPECTRUM_BG;
    background.bg_opa = LV_OPA_COVER;
    background.radius = 0;
    lv_draw_rect(layer, &background, &area);

    /** The LVGL timer owns displayed levels and caps; the draw callback reads them on the same task. */
    spectrum_bars_sync(app);

    const spectrum_plot_t plot = {
        .levels = app->displayed,
        .caps = app->caps,
        .count = app->bar_count,
        .style = app->style,
    };
    spectrum_plot_draw(layer, &area, &plot);
}

/**
 * @brief Resolve plot height for bottom-anchored or mirrored styles.
 * @param app Spectrum application state.
 * @param canvas_height Canvas height in pixels.
 * @return Height available to the plot in pixels.
 */
static int32_t spectrum_plot_height(const spectrum_app_t *app, int32_t canvas_height)
{
    if (app->style == SPECTRUM_STYLE_DOT_MATRIX) {
        return canvas_height;
    }
    return canvas_height / 2 - 8;
}

static void spectrum_timer_cb(lv_timer_t *timer)
{
    spectrum_app_t *app = timer == NULL ? NULL : lv_timer_get_user_data(timer);
    if (app == NULL || app->canvas == NULL) {
        return;
    }
    spectrum_bars_sync(app);
    spectrum_grid_sync(app);
    if (app->bar_count == 0U) {
        return;
    }

    uint32_t windows;
    portENTER_CRITICAL(&s_frame_lock);
    windows = app->published_windows;
    const bool changed = windows != app->displayed_windows;
    if (changed) {
        memcpy(app->raw_frame, app->latest, sizeof(app->raw_frame));
    }
    portEXIT_CRITICAL(&s_frame_lock);

    float frame_peak = 0.0f;
    if (changed) {
        for (size_t i = 0; i < SPECTRUM_FFT_BINS; ++i) {
            if (app->raw_frame[i] > frame_peak) {
                frame_peak = app->raw_frame[i];
            }
        }
    }

    /** Only FFT frame handoff crosses tasks; display state belongs to the LVGL task. */
    const bool active = changed && frame_peak >= SPECTRUM_DISPLAY_ACTIVE_MIN;
    if (active) {
        spectrum_bar_map_aggregate(&app->bars, app->raw_frame, app->levels);
    }

    /** Normalize display levels against the loudest weighted bar. */
    float weighted_peak = 0.0f;
    if (active) {
        for (size_t i = 0; i < app->bar_count; ++i) {
            const float weighted = app->levels[i] * app->tilt_weights[i];
            if (weighted > weighted_peak) {
                weighted_peak = weighted;
            }
        }
    }
    const float reference = weighted_peak > SPECTRUM_DISPLAY_REFERENCE_MIN
                                ? weighted_peak
                                : SPECTRUM_DISPLAY_REFERENCE_MIN;
    const int32_t plot_height = spectrum_plot_height(app, lv_obj_get_height(app->canvas));
    const float fall_step = spectrum_bar_fall_step(app->style, plot_height);

    for (size_t i = 0; i < app->bar_count; ++i) {
        const float target =
            active ? spectrum_display_level(app->levels[i] * app->tilt_weights[i], reference)
                   : 0.0f;
        /** Apply immediate attack and a bounded pixel release per frame. */
        app->displayed[i] = spectrum_bar_step_level(app->displayed[i], target, fall_step);
        app->caps[i] = spectrum_bar_step_cap(app->caps[i], app->displayed[i], fall_step);
    }
    app->displayed_windows = windows;
    /** Skip covered-canvas redraws while the style page is open. */
    if (app->style_page == NULL) {
        lv_obj_invalidate(app->canvas);
    }
}

static void spectrum_create_canvas(spectrum_app_t *app)
{
    lv_obj_set_style_bg_color(app->parent, SPECTRUM_BG, 0);
    lv_obj_set_style_bg_opa(app->parent, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(app->parent, 0, 0);
    lv_obj_set_style_radius(app->parent, 0, 0);
    lv_obj_set_style_pad_all(app->parent, 0, 0);
    lv_obj_set_style_pad_ver(app->parent, SPECTRUM_PLOT_VERTICAL_INSET, 0);
    lv_obj_clear_flag(app->parent, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(app->parent, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(app->parent, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START);

    app->canvas = lv_obj_create(app->parent);
    lv_obj_set_size(app->canvas, LV_PCT(100), LV_PCT(100));
    lv_obj_set_flex_grow(app->canvas, 1);
    lv_obj_center(app->canvas);
    lv_obj_clear_flag(app->canvas, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(app->canvas, SPECTRUM_BG, 0);
    lv_obj_set_style_bg_opa(app->canvas, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(app->canvas, 0, 0);
    lv_obj_set_style_radius(app->canvas, 0, 0);
    lv_obj_set_style_pad_all(app->canvas, 0, 0);
    /** Use the fullscreen canvas as both plot surface and long-press target. */
    lv_obj_add_flag(app->canvas, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(app->canvas, spectrum_draw_cb, LV_EVENT_DRAW_MAIN, app);
    lv_obj_add_event_cb(app->canvas, spectrum_long_press_cb, LV_EVENT_LONG_PRESSED, app);
}

/**
 * @brief Show the style hint once and persist its completion.
 * @param timer LVGL hint timer.
 */
static void spectrum_hint_timer_cb(lv_timer_t *timer)
{
    spectrum_app_t *app = timer == NULL ? NULL : lv_timer_get_user_data(timer);
    if (app == NULL) {
        return;
    }
    if (app->hint != NULL) {
        lv_obj_delete(app->hint);
        app->hint = NULL;
    }
    /** Clear the handle before LVGL frees this one-shot timer. */
    app->hint_timer = NULL;
}

static void spectrum_maybe_show_hint(spectrum_app_t *app)
{
    bool seen = false;
    if (spectrum_prefs_hint_seen(&seen) == ESP_OK && seen) {
        return;
    }

    /** Place the hint inside the canvas without changing the parent flex layout. */
    app->hint = lv_label_create(app->canvas);
    lv_label_set_text(app->hint, L(DESKTOP_TEXT_SPECTRUM_STYLE_HINT));
    lv_obj_set_style_text_color(app->hint, SPECTRUM_TEXT, 0);
    lv_obj_set_style_text_font(app->hint, lv_obj_get_style_text_font(app->parent, 0), 0);
    lv_obj_set_style_bg_color(app->hint, SPECTRUM_BUTTON_BG, 0);
    lv_obj_set_style_bg_opa(app->hint, LV_OPA_80, 0);
    lv_obj_set_style_border_width(app->hint, 1, 0);
    lv_obj_set_style_border_color(app->hint, SPECTRUM_BUTTON_BORDER, 0);
    lv_obj_set_style_radius(app->hint, 10, 0);
    lv_obj_set_style_pad_hor(app->hint, 12, 0);
    lv_obj_set_style_pad_ver(app->hint, 8, 0);
    lv_obj_align(app->hint, LV_ALIGN_BOTTOM_MID, 0, -SPECTRUM_HINT_BOTTOM_INSET);
    lv_obj_move_foreground(app->hint);

    /** Persist hint completion when it becomes visible. */
    if (spectrum_prefs_mark_hint_seen() != ESP_OK) {
        ESP_LOGW(TAG, "Failed to persist the spectrum style hint marker");
    }
    app->hint_timer = lv_timer_create(spectrum_hint_timer_cb, SPECTRUM_HINT_MS, app);
    if (app->hint_timer != NULL) {
        lv_timer_set_repeat_count(app->hint_timer, 1);
    }
    ESP_LOGI(TAG, "Spectrum style hint shown once");
}

esp_err_t spectrum_app_start(lv_obj_t *parent)
{
    if (parent == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_app != NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    spectrum_app_t *app = heap_caps_calloc(1, sizeof(*app),
                                            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (app == NULL) {
        return ESP_ERR_NO_MEM;
    }
    app->parent = parent;
    /** Load saved grouping and style before building the first bar map. */
    if (spectrum_prefs_load_group(&app->group) != ESP_OK) {
        app->group = SPECTRUM_GROUP_LINEAR;
    }
    if (spectrum_prefs_load_style(&app->style) != ESP_OK) {
        app->style = SPECTRUM_STYLE_BARS;
    }
    /** Read the grid preference owned by Settings. */
    (void)spectrum_prefs_load_lines(&app->grid_lines);
    const esp_err_t ret = spectrum_pipeline_start(app);
    if (ret != ESP_OK) {
        spectrum_pipeline_destroy(app);
        if (app->input_opened) {
            spectrum_restore_input_gain(app);
            (void)audio_input_close();
            app->input_opened = false;
        }
        spectrum_restore_output(app);
        if (app->runtime_acquired) {
            (void)audio_runtime_release();
        }
        spectrum_fft_deinit(&app->fft);
        heap_caps_free(app);
        return ret;
    }
    s_app = app;
    spectrum_create_canvas(app);
    app->timer = lv_timer_create(spectrum_timer_cb, SPECTRUM_TIMER_PERIOD_MS, app);
    if (app->timer == NULL) {
        spectrum_app_stop();
        return ESP_ERR_NO_MEM;
    }
    spectrum_maybe_show_hint(app);
    return ESP_OK;
}

void spectrum_app_stop(void)
{
    spectrum_app_t *app = s_app;
    if (app == NULL) {
        return;
    }
    s_app = NULL;
    if (app->hint_timer != NULL) {
        lv_timer_delete(app->hint_timer);
        app->hint_timer = NULL;
    }
    if (app->timer != NULL) {
        lv_timer_delete(app->timer);
        app->timer = NULL;
    }
    spectrum_pipeline_destroy(app);
    if (app->input_opened) {
        spectrum_restore_input_gain(app);
        (void)audio_input_close();
        app->input_opened = false;
    }
    spectrum_restore_output(app);
    spectrum_fft_deinit(&app->fft);
    if (app->runtime_acquired) {
        (void)audio_runtime_release();
        app->runtime_acquired = false;
    }
    if (app->parent != NULL) {
        lv_obj_clean(app->parent);
    }
    heap_caps_free(app);
}
