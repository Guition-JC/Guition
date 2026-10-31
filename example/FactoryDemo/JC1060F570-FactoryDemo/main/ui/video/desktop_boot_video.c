/**
 * SPDX-License-Identifier: GPL-2.0-only
 */
#include "desktop_boot_video.h"

#include <inttypes.h>
#include <stddef.h>
#include <string.h>

#include "display.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lvgl.h"
#include "video_player_service.h"

#define TAG "BootVideo"

#define BOOT_VIDEO_REFRESH_MS 40U
#define BOOT_VIDEO_POLL_MS 50U
#define BOOT_VIDEO_FIRST_FRAME_WAIT_MS 1500U
#define BOOT_VIDEO_YIELD_MS 5U
#define BOOT_VIDEO_YIELD_MIN_MS 20U
#define BOOT_VIDEO_BYTES_PER_PIXEL 2U
#define BOOT_VIDEO_BUFFER_ALIGNMENT 64U
#define BOOT_VIDEO_LOCK_WAIT_MS 10000

/** @brief 启动视频预加载容量上限，单位为字节。 */
#define BOOT_VIDEO_PRELOAD_MAX_BYTES (1536U * 1024U)

/** @brief 允许桌面提前绘制的时间，单位为毫秒。 */
#define BOOT_VIDEO_REVEAL_LEAD_MS 800U

typedef struct {
    lv_obj_t *backdrop;
    lv_obj_t *image;
    lv_timer_t *timer;

    uint8_t *display_buffers[2]; /**< 启动层持有 RGB565 双缓冲区，解码器直接写入。 */
    uint32_t display_capacity;
    uint16_t display_width;
    uint16_t display_height;
    SemaphoreHandle_t frame_mutex;
    uint8_t ready_index;
    uint8_t displayed_index;
    uint16_t frame_width;
    uint16_t frame_height;
    uint32_t frame_stride;
    uint16_t displayed_width;
    uint16_t displayed_height;
    bool frame_ready;
    bool has_displayed;
    bool active;
    bool playing;
    bool backlight_lit;
    TaskHandle_t build_task;
    int64_t started_us;
    bool first_frame_logged;
    int64_t last_yield_us;

    uint32_t position_ms;
    uint32_t duration_ms;
    volatile bool frame_shown;
    lv_image_dsc_t image_dsc;
} boot_video_state_t;

static boot_video_state_t s_boot;

static uint32_t s_build_slices;
static int64_t s_build_slice_us;
/**
 * @brief 在互斥锁内提交解码帧，等待渲染定时器消费。
 * @param buffer_index 解码帧所在的缓冲区索引。
 * @param width 帧宽度，单位为像素。
 * @param height 帧高度，单位为像素。
 * @param stride 帧行跨度，单位为字节。
 * @param pts_ms 帧显示时间戳，单位为毫秒；此回调不使用。
 * @param context 回调上下文；此回调不使用。
 * @return 接受帧时返回 true，否则返回 false。
 */
static bool frame_cb(uint8_t buffer_index, uint16_t width, uint16_t height,
                     uint32_t stride, uint32_t pts_ms, void *context)
{
    (void)pts_ms;
    (void)context;
    if (buffer_index > 1U || s_boot.frame_mutex == NULL) {
        return false;
    }
    if (xSemaphoreTake(s_boot.frame_mutex, pdMS_TO_TICKS(10U)) != pdTRUE) {
        return false;
    }
    bool accepted = false;

    if (s_boot.active && !s_boot.frame_ready && s_boot.display_buffers[buffer_index] != NULL &&
        video_player_dimensions_supported(width, height, s_boot.display_width,
                                          s_boot.display_height)) {
        if (!s_boot.first_frame_logged) {
            s_boot.first_frame_logged = true;
            ESP_LOGI(TAG, "first decoded frame %d ms after playback started",
                     (int)((esp_timer_get_time() - s_boot.started_us) / 1000));
        }
        s_boot.ready_index = buffer_index;
        s_boot.frame_width = width;
        s_boot.frame_height = height;
        s_boot.frame_stride = stride == 0U ? (uint32_t)width * BOOT_VIDEO_BYTES_PER_PIXEL : stride;
        s_boot.frame_ready = true;
        accepted = true;
    }
    xSemaphoreGive(s_boot.frame_mutex);
    return accepted;
}

static void apply_frame_geometry(uint16_t width, uint16_t height)
{
    if (s_boot.image == NULL ||
        (s_boot.displayed_width == width && s_boot.displayed_height == height)) {
        return;
    }
    s_boot.displayed_width = width;
    s_boot.displayed_height = height;
    lv_obj_set_size(s_boot.image, width, height);
    lv_obj_center(s_boot.image);
}

static void refresh_cb(lv_timer_t *timer)
{
    (void)timer;
    if (!s_boot.active || s_boot.frame_mutex == NULL) {
        return;
    }

    const bool frame_was_shown = s_boot.frame_shown;
    bool frame_ready = false;
    uint8_t buffer_index = 0U;
    uint16_t width = 0U;
    uint16_t height = 0U;
    uint32_t stride = 0U;
    if (xSemaphoreTake(s_boot.frame_mutex, 0) == pdTRUE) {
        if (s_boot.frame_ready) {
            frame_ready = true;
            buffer_index = s_boot.ready_index;
            width = s_boot.frame_width;
            height = s_boot.frame_height;
            stride = s_boot.frame_stride;
            s_boot.frame_ready = false;
        }
        xSemaphoreGive(s_boot.frame_mutex);
    }
    if (!frame_ready ||
        !video_player_dimensions_supported(width, height, s_boot.display_width,
                                           s_boot.display_height)) {
        return;
    }
    apply_frame_geometry(width, height);
    s_boot.image_dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
    s_boot.image_dsc.header.cf = LV_COLOR_FORMAT_RGB565;
    s_boot.image_dsc.header.w = width;
    s_boot.image_dsc.header.h = height;
    s_boot.image_dsc.header.stride = stride;
    s_boot.image_dsc.data_size = stride * height;
    s_boot.image_dsc.data = s_boot.display_buffers[buffer_index];
    lv_image_set_src(s_boot.image, &s_boot.image_dsc);
    lv_obj_remove_flag(s_boot.image, LV_OBJ_FLAG_HIDDEN);
    if (s_boot.has_displayed && s_boot.displayed_index != buffer_index) {
        video_player_service_release_output_buffer(s_boot.displayed_index);
    }
    s_boot.displayed_index = buffer_index;
    s_boot.has_displayed = true;
    s_boot.frame_shown = true;
    if (frame_was_shown && !s_boot.backlight_lit) {
        s_boot.backlight_lit = true;
        (void)display_backlight_init();
    }
}

/**
 * @brief 释放启动层与播放器资源；调用方须持有显示锁。
 */
static void teardown_locked(void)
{
    s_boot.active = false;
    (void)video_player_service_set_frame_callback(NULL, NULL);
    if (s_boot.timer != NULL) {
        lv_timer_delete(s_boot.timer);
        s_boot.timer = NULL;
    }

    if (s_boot.image != NULL) {
        lv_obj_delete(s_boot.image);
        s_boot.image = NULL;
    }
    if (s_boot.backdrop != NULL) {
        lv_obj_delete(s_boot.backdrop);
        s_boot.backdrop = NULL;
    }
    if (s_boot.has_displayed) {
        video_player_service_release_output_buffer(s_boot.displayed_index);
        s_boot.has_displayed = false;
    }

    (void)video_player_service_exit();
    const bool unregistered =
        video_player_service_set_output_buffers(NULL, NULL, 0U) == ESP_OK;
    if (s_boot.frame_mutex != NULL) {
        (void)xSemaphoreTake(s_boot.frame_mutex, portMAX_DELAY);
        if (unregistered) {
            for (size_t i = 0; i < 2U; ++i) {
                if (s_boot.display_buffers[i] != NULL) {
                    heap_caps_free(s_boot.display_buffers[i]);
                    s_boot.display_buffers[i] = NULL;
                }
            }
        } else {
            ESP_LOGW(TAG, "decoder still owns the display buffers; keeping them to avoid a use-after-free");
        }
        xSemaphoreGive(s_boot.frame_mutex);
        vSemaphoreDelete(s_boot.frame_mutex);
        s_boot.frame_mutex = NULL;
    }
    memset(&s_boot, 0, sizeof(s_boot));

}

/**
 * @brief 预加载内置视频并创建启动层；调用方须持有显示锁。
 * @return 成功启动时返回 true。
 */
static bool start_locked(void)
{
    uint16_t width = 0U;
    uint16_t height = 0U;
    if (display_get_size(&width, &height) != ESP_OK) {
        ESP_LOGW(TAG, "board panel size unavailable; not playing a boot video");
        return false;
    }

    const uint16_t panel_width = width;
    const uint16_t panel_height = height;
    video_player_align_resolution(&width, &height);
    s_boot.display_width = width;
    s_boot.display_height = height;
    s_boot.display_capacity = (uint32_t)width * height * BOOT_VIDEO_BYTES_PER_PIXEL;
    s_boot.frame_mutex = xSemaphoreCreateMutex();
    for (size_t i = 0; i < 2U; ++i) {
        s_boot.display_buffers[i] = heap_caps_aligned_alloc(BOOT_VIDEO_BUFFER_ALIGNMENT,
                                                           s_boot.display_capacity,
                                                           MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (s_boot.display_buffers[i] == NULL) {
            s_boot.display_buffers[i] = heap_caps_aligned_alloc(BOOT_VIDEO_BUFFER_ALIGNMENT,
                                                               s_boot.display_capacity,
                                                               MALLOC_CAP_8BIT);
        }
    }
    if (s_boot.frame_mutex == NULL || s_boot.display_buffers[0] == NULL ||
        s_boot.display_buffers[1] == NULL) {
        ESP_LOGW(TAG, "boot video display buffers unavailable");
        return false;
    }

    const esp_err_t entered = video_player_service_enter_assets();
    if (entered != ESP_OK) {
        ESP_LOGI(TAG, "no boot video to play: %s", esp_err_to_name(entered));
        return false;
    }
    (void)video_player_service_set_display_size(width, height);
    (void)video_player_service_set_output_buffers(s_boot.display_buffers[0],
                                                  s_boot.display_buffers[1],
                                                  s_boot.display_capacity);

    if (video_player_service_preload_selected(BOOT_VIDEO_PRELOAD_MAX_BYTES) != ESP_OK) {
        ESP_LOGI(TAG, "playing from flash; the clip will share the flash with the desktop build");
    }

    lv_obj_t *overlay = lv_display_get_layer_top(lv_display_get_default());
    s_boot.backdrop = lv_obj_create(overlay);
    lv_obj_remove_style_all(s_boot.backdrop);
    lv_obj_set_style_bg_color(s_boot.backdrop, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(s_boot.backdrop, LV_OPA_COVER, 0);
    /* Keep the visible canvas at panel size; aligned decoder buffers can be larger. */
    lv_obj_set_size(s_boot.backdrop, panel_width, panel_height);
    lv_obj_center(s_boot.backdrop);
    lv_obj_clear_flag(s_boot.backdrop, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_SCROLL_CHAIN_HOR |
                                       LV_OBJ_FLAG_SCROLL_CHAIN_VER);

    lv_obj_add_flag(s_boot.backdrop, LV_OBJ_FLAG_CLICKABLE);

    s_boot.image = lv_image_create(s_boot.backdrop);
    lv_obj_clear_flag(s_boot.image, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE |
                                    LV_OBJ_FLAG_SCROLL_CHAIN_HOR | LV_OBJ_FLAG_SCROLL_CHAIN_VER);
    lv_obj_set_size(s_boot.image, width, height);
    lv_obj_center(s_boot.image);
    lv_obj_add_flag(s_boot.image, LV_OBJ_FLAG_HIDDEN);

    s_boot.active = true;
    s_boot.playing = true;
    s_boot.started_us = esp_timer_get_time();
    (void)video_player_service_set_frame_callback(frame_cb, NULL);
    s_boot.timer = lv_timer_create(refresh_cb, BOOT_VIDEO_REFRESH_MS, NULL);
    if (video_player_service_play() != ESP_OK) {
        ESP_LOGW(TAG, "boot video refused to start");
        return false;
    }

    s_boot.build_task = xTaskGetCurrentTaskHandle();
    ESP_LOGI(TAG, "boot video playing on a %ux%u panel", width, height);
    return true;
}

/**
 * @brief 等待并重新取得 LVGL 显示锁。
 */
static void lock_display(void)
{
    while (!lvgl_display_lock(BOOT_VIDEO_LOCK_WAIT_MS)) {
        ESP_LOGW(TAG, "LVGL lock busy after a build slice; retrying");
        vTaskDelay(pdMS_TO_TICKS(BOOT_VIDEO_POLL_MS));
    }
}

esp_err_t desktop_boot_video_start(void)
{
    if (!lvgl_display_lock(BOOT_VIDEO_LOCK_WAIT_MS)) {
        ESP_LOGW(TAG, "LVGL lock busy; not playing a boot video");
        return ESP_ERR_TIMEOUT;
    }
    const bool started = start_locked();
    lvgl_display_unlock();
    if (!started) {
        lock_display();
        teardown_locked();
        lvgl_display_unlock();
        return ESP_ERR_NOT_FOUND;
    }

    const TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(BOOT_VIDEO_FIRST_FRAME_WAIT_MS);
    while (!s_boot.frame_shown && (int32_t)(xTaskGetTickCount() - deadline) < 0) {
        vTaskDelay(pdMS_TO_TICKS(BOOT_VIDEO_POLL_MS));
    }
    if (!s_boot.frame_shown) {
        ESP_LOGW(TAG, "boot video has no frame yet; lighting the panel for the desktop build");
        (void)display_backlight_init();
    }
    return ESP_OK;
}

void desktop_boot_video_finish(uint32_t timeout_ms)
{
    if (!s_boot.playing) {
        (void)display_backlight_init();
        return;
    }

    const TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(timeout_ms);
    uint32_t last_duration_ms = 0U;
    for (;;) {
        video_player_snapshot_t snapshot = {0};
        video_player_service_get_snapshot(&snapshot);
        last_duration_ms = snapshot.duration_ms;
        s_boot.position_ms = snapshot.position_ms;
        s_boot.duration_ms = snapshot.duration_ms;
        if (snapshot.status == VIDEO_PLAYER_STATUS_ENDED ||
            snapshot.status == VIDEO_PLAYER_STATUS_ERROR) {
            break;
        }

        if (!video_player_service_is_entered()) {
            ESP_LOGW(TAG, "boot video player was released while it was covering the boot");
            break;
        }
        if ((int32_t)(xTaskGetTickCount() - deadline) >= 0) {
            ESP_LOGW(TAG, "boot video still running after %" PRIu32 " ms; stopping it",
                     timeout_ms);
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(BOOT_VIDEO_POLL_MS));
    }

    ESP_LOGI(TAG, "desktop built behind the clip: slices=%" PRIu32 " lvgl=%" PRId64 " ms",
             s_build_slices, s_build_slice_us / 1000);
    ESP_LOGI(TAG, "clip ended after %d ms on the clock (video says %" PRIu32 " ms)",
             (int)((esp_timer_get_time() - s_boot.started_us) / 1000),
             last_duration_ms);
    lock_display();
    teardown_locked();
    lvgl_display_unlock();

    (void)display_backlight_init();
}

bool desktop_boot_video_is_playing(void)
{
    return s_boot.playing;
}

bool desktop_boot_video_covers_desktop(void)
{
    if (!s_boot.playing) {
        return false;
    }

    if (s_boot.duration_ms == 0U) {
        return true;
    }
    return s_boot.position_ms + BOOT_VIDEO_REVEAL_LEAD_MS < s_boot.duration_ms;
}

void desktop_boot_video_yield(void)
{
    if (!s_boot.playing || s_boot.build_task != xTaskGetCurrentTaskHandle()) {
        return;
    }
    const int64_t now_us = esp_timer_get_time();
    if (now_us - s_boot.last_yield_us < (int64_t)BOOT_VIDEO_YIELD_MIN_MS * 1000) {
        return;
    }
    s_boot.last_yield_us = now_us;
    const int64_t start_us = esp_timer_get_time();
    lvgl_display_unlock();
    vTaskDelay(pdMS_TO_TICKS(BOOT_VIDEO_YIELD_MS));
    lock_display();
    s_build_slice_us += esp_timer_get_time() - start_us;
    ++s_build_slices;
}
