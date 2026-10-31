/* SPDX-License-Identifier: MIT */
#include <stdint.h>
#include <string.h>
#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "dev_display_lcd.h"
#include "esp_attr.h"
#include "esp_board_manager.h"
#include "esp_check.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_ops.h"
#include "esp_log.h"

static const char *TAG = "board_init";

static bool IRAM_ATTR signal_completion(void *user_ctx)
{
    BaseType_t task_woken = pdFALSE;
    xSemaphoreGiveFromISR((SemaphoreHandle_t)user_ctx, &task_woken);
    return task_woken == pdTRUE;
}

static bool IRAM_ATTR color_transfer_done(esp_lcd_panel_io_handle_t io,
                                         esp_lcd_panel_io_event_data_t *event,
                                         void *user_ctx)
{
    (void)io;
    (void)event;
    return signal_completion(user_ctx);
}

#if CONFIG_ESP_BOARD_DEV_DISPLAY_LCD_SUB_DSI_SUPPORT
static bool IRAM_ATTR dsi_frame_complete(esp_lcd_panel_handle_t panel,
                                        esp_lcd_dpi_panel_event_data_t *event,
                                        void *user_ctx)
{
    (void)panel;
    (void)event;
    return signal_completion(user_ctx);
}
#endif

#if CONFIG_ESP_BOARD_DEV_DISPLAY_LCD_SUB_RGB_SUPPORT || CONFIG_ESP_BOARD_DEV_DISPLAY_LCD_SUB_RGB_3WIRE_SPI_SUPPORT
static bool IRAM_ATTR rgb_frame_complete(esp_lcd_panel_handle_t panel,
                                        const esp_lcd_rgb_panel_event_data_t *event,
                                        void *user_ctx)
{
    (void)panel;
    (void)event;
    return signal_completion(user_ctx);
}
#endif

static size_t encode_pixel(dev_display_lcd_frame_format_t format, uint32_t rgb, uint8_t pixel[3])
{
    const uint8_t red = rgb >> 16;
    const uint8_t green = rgb >> 8;
    const uint8_t blue = rgb;
    const uint16_t rgb565 = ((red >> 3) << 11) | ((green >> 2) << 5) | (blue >> 3);

    switch (format) {
    case DEV_DISPLAY_LCD_FRAME_FORMAT_RGB565_LE:
        pixel[0] = rgb565;
        pixel[1] = rgb565 >> 8;
        return 2;
    case DEV_DISPLAY_LCD_FRAME_FORMAT_RGB565_BE:
        pixel[0] = rgb565 >> 8;
        pixel[1] = rgb565;
        return 2;
    case DEV_DISPLAY_LCD_FRAME_FORMAT_RGB888:
        pixel[0] = red;
        pixel[1] = green;
        pixel[2] = blue;
        return 3;
    case DEV_DISPLAY_LCD_FRAME_FORMAT_BGR888:
        pixel[0] = blue;
        pixel[1] = green;
        pixel[2] = red;
        return 3;
    default:
        return 0;
    }
}

static esp_err_t display_color_test(void)
{
    dev_display_lcd_handles_t *lcd = NULL;
    dev_display_lcd_config_t *config = NULL;
    ESP_RETURN_ON_ERROR(esp_board_manager_get_device_handle("display_lcd", (void **)&lcd), TAG, "LCD handle");
    ESP_RETURN_ON_ERROR(esp_board_manager_get_device_config("display_lcd", (void **)&config), TAG, "LCD configuration");
    ESP_RETURN_ON_FALSE(lcd && lcd->panel_handle && config && config->sub_type &&
                        config->lcd_width && config->lcd_height,
                        ESP_ERR_INVALID_STATE, TAG, "Invalid LCD configuration");

    const int width = config->lcd_width;
    const int height = config->lcd_height;
    uint8_t pixel[3];
    const size_t pixel_bytes = encode_pixel(config->frame_format, 0, pixel);
    ESP_RETURN_ON_FALSE(pixel_bytes, ESP_ERR_NOT_SUPPORTED, TAG, "Unsupported LCD frame format");

    void *buffer = NULL;
    void *frame_buffers[2] = {NULL, NULL};
    unsigned back_buffer = 1;
    int buffer_rows = height;
    SemaphoreHandle_t transfer_done = NULL;
    SemaphoreHandle_t frame_done = NULL;
    unsigned frame_events = 0;

#if CONFIG_ESP_BOARD_DEV_DISPLAY_LCD_SUB_DSI_SUPPORT
    if (strcmp(config->sub_type, ESP_BOARD_DEVICE_LCD_SUB_TYPE_DSI) == 0) {
        ESP_RETURN_ON_ERROR(esp_lcd_dpi_panel_get_frame_buffer(lcd->panel_handle, 2,
                            &frame_buffers[0], &frame_buffers[1]), TAG, "DSI requires at least two frame buffers");
        frame_done = xSemaphoreCreateBinary();
        ESP_RETURN_ON_FALSE(frame_done, ESP_ERR_NO_MEM, TAG, "Frame semaphore allocation failed");
        const esp_lcd_dpi_panel_event_callbacks_t callbacks = {
            .on_frame_buf_complete = dsi_frame_complete,
        };
        ESP_RETURN_ON_ERROR(esp_lcd_dpi_panel_register_event_callbacks(lcd->panel_handle, &callbacks, frame_done),
                            TAG, "DSI frame callback");
        frame_events = 2;
        buffer = frame_buffers[back_buffer];
    }
#endif
#if CONFIG_ESP_BOARD_DEV_DISPLAY_LCD_SUB_RGB_SUPPORT || CONFIG_ESP_BOARD_DEV_DISPLAY_LCD_SUB_RGB_3WIRE_SPI_SUPPORT
    if (strcmp(config->sub_type, ESP_BOARD_DEVICE_LCD_SUB_TYPE_RGB) == 0 ||
        strcmp(config->sub_type, ESP_BOARD_DEVICE_LCD_SUB_TYPE_RGB_3WIRE_SPI) == 0) {
        const esp_lcd_rgb_panel_config_t *rgb_config = NULL;
#if CONFIG_ESP_BOARD_DEV_DISPLAY_LCD_SUB_RGB_SUPPORT
        if (strcmp(config->sub_type, ESP_BOARD_DEVICE_LCD_SUB_TYPE_RGB) == 0) {
            rgb_config = &config->sub_cfg.rgb.panel_config;
        }
#endif
#if CONFIG_ESP_BOARD_DEV_DISPLAY_LCD_SUB_RGB_3WIRE_SPI_SUPPORT
        if (strcmp(config->sub_type, ESP_BOARD_DEVICE_LCD_SUB_TYPE_RGB_3WIRE_SPI) == 0) {
            rgb_config = &config->sub_cfg.rgb_3wire_spi.rgb_panel_config;
        }
#endif
        ESP_RETURN_ON_FALSE(rgb_config && !rgb_config->flags.refresh_on_demand,
                            ESP_ERR_NOT_SUPPORTED, TAG, "RGB requires continuous refresh");
#if CONFIG_IDF_TARGET_ESP32S3
        /* S3 DMA EOF can precede a descriptor switch; bounce mode supplies a safe frame boundary. */
        ESP_RETURN_ON_FALSE(rgb_config->bounce_buffer_size_px, ESP_ERR_NOT_SUPPORTED,
                            TAG, "ESP32-S3 RGB requires a bounce buffer for synchronized refresh");
#endif
        ESP_RETURN_ON_ERROR(esp_lcd_rgb_panel_get_frame_buffer(lcd->panel_handle, 2,
                            &frame_buffers[0], &frame_buffers[1]), TAG, "RGB requires at least two frame buffers");
        frame_done = xSemaphoreCreateBinary();
        ESP_RETURN_ON_FALSE(frame_done, ESP_ERR_NO_MEM, TAG, "Frame semaphore allocation failed");
        const esp_lcd_rgb_panel_event_callbacks_t callbacks = {
            .on_frame_buf_complete = rgb_frame_complete,
        };
        ESP_RETURN_ON_ERROR(esp_lcd_rgb_panel_register_event_callbacks(lcd->panel_handle, &callbacks, frame_done),
                            TAG, "RGB frame callback");
        /* Direct AXI RGB reports a link switch once; bounce mode reports every frame. */
        frame_events = rgb_config->bounce_buffer_size_px ? 2 : 1;
        buffer = frame_buffers[back_buffer];
    }
#endif
    if (strcmp(config->sub_type, ESP_BOARD_DEVICE_LCD_SUB_TYPE_SPI) == 0) {
        ESP_RETURN_ON_FALSE(lcd->io_handle, ESP_ERR_INVALID_STATE, TAG, "Missing SPI LCD IO");
        const bool full_frame = config->chip && strcmp(config->chip, "axs15231b") == 0;
        /* AXS15231B requires one complete frame per RAMWR command. */
        buffer_rows = full_frame ? height : (height < 10 ? height : 10);
        const uint32_t caps = full_frame ? (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
                                        : (MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
        buffer = heap_caps_malloc((size_t)width * buffer_rows * pixel_bytes, caps);
        ESP_RETURN_ON_FALSE(buffer, ESP_ERR_NO_MEM, TAG, "SPI draw buffer allocation failed");
        transfer_done = xSemaphoreCreateBinary();
        if (!transfer_done) {
            heap_caps_free(buffer);
            return ESP_ERR_NO_MEM;
        }
        const esp_lcd_panel_io_callbacks_t callbacks = {
            .on_color_trans_done = color_transfer_done,
        };
        esp_err_t ret = esp_lcd_panel_io_register_event_callbacks(lcd->io_handle, &callbacks, transfer_done);
        if (ret != ESP_OK) {
            vSemaphoreDelete(transfer_done);
            heap_caps_free(buffer);
            return ret;
        }
    }
    ESP_RETURN_ON_FALSE(buffer, ESP_ERR_NOT_SUPPORTED, TAG, "Unsupported LCD interface: %s", config->sub_type);

    static const struct {
        const char *name;
        uint32_t rgb;
    } colors[] = {
        {"red",   0xFF0000},
        {"green", 0x00FF00},
        {"blue",  0x0000FF},
        {"white", 0xFFFFFF},
        {"black", 0x000000},
    };
    ESP_LOGI(TAG, "Color test: %dx%d, frame format %d", width, height, (int)config->frame_format);

    for (;;) {
        for (size_t color = 0; color < sizeof(colors) / sizeof(colors[0]); ++color) {
            encode_pixel(config->frame_format, colors[color].rgb, pixel);
            for (size_t i = 0; i < (size_t)width * buffer_rows; ++i) {
                memcpy((uint8_t *)buffer + i * pixel_bytes, pixel, pixel_bytes);
            }
            for (int y = 0; y < height; y += buffer_rows) {
                const int end_y = y + buffer_rows < height ? y + buffer_rows : height;
                if (frame_done && frame_events == 1) {
                    /* A link-switch event can arrive before draw_bitmap returns. */
                    xSemaphoreTake(frame_done, 0);
                }
                ESP_RETURN_ON_ERROR(esp_lcd_panel_draw_bitmap(lcd->panel_handle, 0, y, width, end_y, buffer), TAG, "Draw color");
                if (transfer_done) {
                    /* SPI DMA must finish before the draw buffer is reused. */
                    ESP_RETURN_ON_FALSE(xSemaphoreTake(transfer_done, pdMS_TO_TICKS(2000)) == pdTRUE,
                                        ESP_ERR_TIMEOUT, TAG, "SPI color transfer timed out");
                }
            }
            if (frame_done) {
                if (frame_events == 2) {
                    /* Discard old periodic events after submission. An in-flight ISR may
                     * have sampled the old buffer; two new events cover that race. */
                    xSemaphoreTake(frame_done, 0);
                }
                for (unsigned event = 0; event < frame_events; ++event) {
                    ESP_RETURN_ON_FALSE(xSemaphoreTake(frame_done, pdMS_TO_TICKS(2000)) == pdTRUE,
                                        ESP_ERR_TIMEOUT, TAG, "LCD frame switch timed out");
                }
                back_buffer ^= 1;
                buffer = frame_buffers[back_buffer];
            }
            ESP_LOGI(TAG, "%s", colors[color].name);
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
    }
}

void app_main(void)
{
    /* Obtain only display resources. Do not initialize every declared bus. */
    ESP_ERROR_CHECK(esp_board_manager_init_device_by_name("display_lcd"));
#if CONFIG_GUITION_TOUCH_RESISTIVE
    const char *touch_name = "lcd_touch_resistive";
#elif CONFIG_GUITION_TOUCH_NONE
    const char *touch_name = NULL;
#else
    const char *touch_name = "lcd_touch";
#endif
    if (touch_name && esp_board_manager_check_name(touch_name)) {
        ESP_ERROR_CHECK(esp_board_manager_init_device_by_name(touch_name));
    }
    if (esp_board_manager_check_name("lcd_brightness")) {
        ESP_ERROR_CHECK(esp_board_manager_init_device_by_name("lcd_brightness"));
    }
    ESP_ERROR_CHECK(display_color_test());
}
