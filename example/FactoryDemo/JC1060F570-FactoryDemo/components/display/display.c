#include <inttypes.h>
#include <string.h>
#include "driver/ledc.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_board_manager.h"
#include "esp_board_manager_includes.h"
#include "lvgl.h"
#include "esp_lv_adapter.h"
#include "nvs.h"

#include "display.h"

#define TAG     "Display"

/** @brief Board-declared LEDC control device that owns the panel backlight PWM. */
#define BACKLIGHT_DEVICE_NAME "lcd_brightness"
#define BACKLIGHT_NVS_NAMESPACE "display"
#define BACKLIGHT_NVS_KEY "backlight"

static lv_display_t *s_disp = NULL;
static lv_indev_t *s_touch = NULL;
static dev_display_lcd_handles_t *lcd_handle;
static dev_lcd_touch_handles_t *touch_handle;
static uint16_t s_screen_width;
static uint16_t s_screen_height;
static uint8_t s_backlight_percent;
static bool s_backlight_ready;

esp_err_t display_init(void)
{

    esp_err_t ret = esp_board_manager_get_device_handle("display_lcd", (void *)&lcd_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to get LCD device");
        return ret;
    }

    if (esp_board_manager_check_name("lcd_touch")) {
        ret = esp_board_manager_get_device_handle("lcd_touch", (void *)&touch_handle);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "Touch device exists but handle is unavailable");
        }
    }

    esp_lv_adapter_config_t adapter_config = ESP_LV_ADAPTER_DEFAULT_CONFIG();

    adapter_config.stack_in_psram = false;
    /** Run the LVGL worker on core 1. */
    adapter_config.task_core_id = 1;
    ret = esp_lv_adapter_init(&adapter_config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize LVGL adapter: %s", esp_err_to_name(ret));
        return ret;
    }

    dev_display_lcd_config_t *lcd_cfg = NULL;
    ret = esp_board_manager_get_device_config("display_lcd", (void **)&lcd_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to get LCD device config: %s", esp_err_to_name(ret));
        return ret;
    }
    if (lcd_cfg->lcd_width == 0 || lcd_cfg->lcd_height == 0) {
        ESP_LOGE(TAG, "Invalid LCD dimensions: %ux%u", lcd_cfg->lcd_width, lcd_cfg->lcd_height);
        return ESP_ERR_INVALID_SIZE;
    }
    s_screen_width = lcd_cfg->lcd_width;
    s_screen_height = lcd_cfg->lcd_height;

    esp_lv_adapter_display_config_t display_config;
    if(strncmp(lcd_cfg->sub_type, ESP_BOARD_DEVICE_LCD_SUB_TYPE_DSI, 3) == 0)
    {
        display_config = ESP_LV_ADAPTER_DISPLAY_MIPI_DEFAULT_CONFIG(
                                                         lcd_handle->panel_handle, lcd_handle->io_handle, lcd_cfg->lcd_width, lcd_cfg->lcd_height, ESP_LV_ADAPTER_ROTATE_0);
    }
    else if (strncmp(lcd_cfg->sub_type, ESP_BOARD_DEVICE_LCD_SUB_TYPE_SPI, 3) == 0)
    {
        display_config = ESP_LV_ADAPTER_DISPLAY_SPI_WITH_PSRAM_DEFAULT_CONFIG(
                                                         lcd_handle->panel_handle, lcd_handle->io_handle, lcd_cfg->lcd_width, lcd_cfg->lcd_height, ESP_LV_ADAPTER_ROTATE_0);
    }
    else if (strncmp(lcd_cfg->sub_type, ESP_BOARD_DEVICE_LCD_SUB_TYPE_RGB, 3) == 0)
    {
        display_config = ESP_LV_ADAPTER_DISPLAY_RGB_DEFAULT_CONFIG(
                                                         lcd_handle->panel_handle, lcd_handle->io_handle, lcd_cfg->lcd_width, lcd_cfg->lcd_height, ESP_LV_ADAPTER_ROTATE_0);
        /** Draw directly into the two panel-owned frame buffers. */
        display_config.tear_avoid_mode = ESP_LV_ADAPTER_TEAR_AVOID_MODE_DOUBLE_DIRECT;
        display_config.profile.use_psram = true;
        /** Use a full-frame buffer height when partial rendering is selected. */
        display_config.profile.buffer_height = lcd_cfg->lcd_height;
    }
    else
    {
        ESP_LOGI(TAG, "The interface isn't MIPI-DSI, SPI, or RGB, please add the configuration in display.c");
        return ESP_FAIL;
    }
    s_disp = esp_lv_adapter_register_display(&display_config);
    if (s_disp == NULL) {
        ESP_LOGE(TAG, "Failed to register LCD display");
        return ESP_FAIL;
    }

    if (touch_handle != NULL) {
        esp_lv_adapter_touch_config_t touch_cfg = {
            .disp = s_disp,
            .handle = touch_handle->touch_handle,
            .scale = { .x = 1.0f, .y = 1.0f },
        };
        s_touch = esp_lv_adapter_register_touch(&touch_cfg);
        if (s_touch == NULL) {
            ESP_LOGW(TAG, "Touch input registration failed");
        } else {
            /** Configure swipe distance and per-sample velocity thresholds in pixels. */
            lv_indev_set_gesture_min_distance(s_touch, 20);
            lv_indev_set_gesture_min_velocity(s_touch, 1);
        }
    }

    ret = esp_lv_adapter_start();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start LVGL adapter: %s", esp_err_to_name(ret));
    }

    return ret;
}

lv_indev_t *display_get_touch_indev(void)
{
    return s_touch;
}

esp_err_t display_get_size(uint16_t *width, uint16_t *height)
{
    if (width == NULL || height == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_screen_width == 0 || s_screen_height == 0) {
        return ESP_ERR_INVALID_STATE;
    }

    *width = s_screen_width;
    *height = s_screen_height;
    return ESP_OK;
}

static uint8_t clamp_backlight_percent(uint8_t percent)
{
    if (percent < DISPLAY_BACKLIGHT_MIN_PERCENT) {
        return DISPLAY_BACKLIGHT_MIN_PERCENT;
    }
    if (percent > DISPLAY_BACKLIGHT_MAX_PERCENT) {
        return DISPLAY_BACKLIGHT_MAX_PERCENT;
    }
    return percent;
}

/**
 * @brief Use the board LEDC device and its configured duty resolution for backlight PWM.
 * @param duty PWM duty value to apply.
 * @return ESP_OK on success, otherwise the device error.
 */
static esp_err_t backlight_write_duty(uint32_t duty)
{
    periph_ledc_handle_t *ledc = NULL;
    esp_err_t ret = esp_board_manager_get_device_handle(BACKLIGHT_DEVICE_NAME, (void **)&ledc);
    if (ret != ESP_OK || ledc == NULL) {
        ESP_LOGW(TAG, "Backlight device '%s' unavailable: %s", BACKLIGHT_DEVICE_NAME,
                 esp_err_to_name(ret));
        return ret == ESP_OK ? ESP_ERR_INVALID_STATE : ret;
    }

    ret = ledc_set_duty(ledc->speed_mode, ledc->channel, duty);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Backlight duty %" PRIu32 " rejected: %s", duty, esp_err_to_name(ret));
        return ret;
    }
    ret = ledc_update_duty(ledc->speed_mode, ledc->channel);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Backlight duty update failed: %s", esp_err_to_name(ret));
        return ret;
    }
    return ESP_OK;
}

/**
 * @brief Full-scale duty of the board's LEDC channel, read from the board definition.
 * @param [out] max_duty Receives the maximum PWM duty value.
 * @return ESP_OK on success, otherwise the device error.
 */
static esp_err_t backlight_max_duty(uint32_t *max_duty)
{
    dev_ledc_ctrl_config_t *device_config = NULL;
    esp_err_t ret = esp_board_manager_get_device_config(BACKLIGHT_DEVICE_NAME, (void **)&device_config);
    if (ret != ESP_OK || device_config == NULL) {
        ESP_LOGW(TAG, "Backlight device config unavailable: %s", esp_err_to_name(ret));
        return ret == ESP_OK ? ESP_ERR_INVALID_STATE : ret;
    }

    periph_ledc_config_t *periph_config = NULL;
    ret = esp_board_manager_get_periph_config(device_config->ledc_name, (void **)&periph_config);
    if (ret != ESP_OK || periph_config == NULL) {
        ESP_LOGW(TAG, "Backlight peripheral '%s' config unavailable: %s",
                 device_config->ledc_name, esp_err_to_name(ret));
        return ret == ESP_OK ? ESP_ERR_INVALID_STATE : ret;
    }

    *max_duty = (1UL << (uint32_t)periph_config->duty_resolution) - 1UL;
    return ESP_OK;
}

esp_err_t display_backlight_set(uint8_t percent)
{
    const uint8_t clamped = clamp_backlight_percent(percent);

    uint32_t max_duty = 0U;
    esp_err_t ret = backlight_max_duty(&max_duty);
    if (ret != ESP_OK) {
        return ret;
    }
    const uint32_t duty = (uint32_t)clamped * max_duty / DISPLAY_BACKLIGHT_MAX_PERCENT;
    ret = backlight_write_duty(duty);
    if (ret != ESP_OK) {
        return ret;
    }

    s_backlight_percent = clamped;
    s_backlight_ready = true;
    return ESP_OK;
}

esp_err_t display_backlight_off(void)
{
    return backlight_write_duty(0U);
}

esp_err_t display_backlight_persist(void)
{
    if (!s_backlight_ready) {
        return ESP_ERR_INVALID_STATE;
    }

    nvs_handle_t handle = 0;
    esp_err_t ret = nvs_open(BACKLIGHT_NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Backlight preference store unavailable: %s", esp_err_to_name(ret));
        return ret;
    }
    ret = nvs_set_u8(handle, BACKLIGHT_NVS_KEY, s_backlight_percent);
    if (ret == ESP_OK) {
        ret = nvs_commit(handle);
    }
    nvs_close(handle);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Storing backlight %u failed: %s", (unsigned)s_backlight_percent,
                 esp_err_to_name(ret));
    }
    return ret;
}

esp_err_t display_backlight_init(void)
{
    uint8_t stored = DISPLAY_BACKLIGHT_DEFAULT_PERCENT;
    nvs_handle_t handle = 0;
    if (nvs_open(BACKLIGHT_NVS_NAMESPACE, NVS_READONLY, &handle) == ESP_OK) {
        uint8_t value = 0;
        if (nvs_get_u8(handle, BACKLIGHT_NVS_KEY, &value) == ESP_OK) {
            stored = value;
        }
        nvs_close(handle);
    }

    const esp_err_t ret = display_backlight_set(stored);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "Backlight level %u%%", (unsigned)s_backlight_percent);
    }
    return ret;
}

uint8_t display_backlight_get(void)
{
    return s_backlight_ready ? s_backlight_percent : 0U;
}

bool lvgl_display_lock(int timeout_ms)
{
    return (esp_lv_adapter_lock(timeout_ms) == ESP_OK);
}

void lvgl_display_unlock()
{
    return esp_lv_adapter_unlock();
}
