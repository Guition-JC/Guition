/**
 * SPDX-License-Identifier: GPL-2.0-only
 */
#include "desktop_sleep_view.h"

#include <stdio.h>
#include <string.h>
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_attr.h"
#include "image/lv_image_decoder_private.h"
#include "misc/cache/instance/lv_image_cache.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "app_clock_view.h"
#include "desktop_big_buffer.h"
#include "photo_catalog.h"
#include "standby_prefs.h"
#include "video_player_service.h"

#define TAG "SleepView"
#define VIDEO_REFRESH_MS 40U
#define VIDEO_BUFFER_ALIGNMENT 64U

static lv_obj_t *s_overlay;
static lv_obj_t *s_image;
static lv_timer_t *s_timer;
static bool s_owns_clock;
static standby_mode_t s_mode;
static uint16_t s_width;
static uint16_t s_height;
static size_t s_photo_index;
static char s_photo_path[256];
static desktop_sleep_view_time_cb_t s_time_cb;
static void *s_time_context;

/** @brief 进入待机时保存配置快照，本次待机期间保持不变。 */
EXT_RAM_BSS_ATTR static standby_prefs_t s_session_prefs;

typedef struct {
    uint8_t *buffers[2];
    SemaphoreHandle_t mutex;
    uint8_t ready_index;
    uint8_t displayed_index;
    uint16_t frame_width;
    uint16_t frame_height;
    uint32_t stride;
    uint16_t limit_width;
    uint16_t limit_height;
    bool ready;
    bool displayed;
    bool active;
    bool owns_service;
    lv_image_dsc_t descriptor;
} sleep_video_t;
static sleep_video_t s_video;

/**
 * @brief 解除图像引用并释放照片缓存。
 */
static void release_photo(void)
{
    if (s_photo_path[0] == '\0') return;
    if (s_image != NULL) lv_image_set_src(s_image, NULL);
    lv_image_cache_drop(s_photo_path);
    s_photo_path[0] = '\0';
}

/**
 * @brief 停止待机视频并释放页面持有的帧缓冲区。
 */
static void release_video(void)
{
    s_video.active = false;
    if (s_video.owns_service) (void)video_player_service_set_frame_callback(NULL, NULL);
    if (s_timer != NULL) {
        lv_timer_delete(s_timer);
        s_timer = NULL;
    }
    if (s_image != NULL) {
        lv_obj_delete(s_image);
        s_image = NULL;
    }
    const esp_err_t result = s_video.owns_service ? video_player_service_exit() : ESP_OK;
    if (result == ESP_OK) {
        for (size_t i = 0U; i < 2U; ++i) {
            if (s_video.buffers[i] != NULL) heap_caps_free(s_video.buffers[i]);
        }
        if (s_video.mutex != NULL) vSemaphoreDelete(s_video.mutex);
        memset(&s_video, 0, sizeof(s_video));
    } else {
        ESP_LOGE(TAG, "video decoder did not stop; preserving its buffers");
    }
}

static void show_clock(void)
{
    if (s_mode == STANDBY_VIDEO) release_video();
    s_mode = STANDBY_CLOCK;
    if (s_image != NULL) {
        lv_obj_delete(s_image);
        s_image = NULL;
    }
    if (s_timer != NULL) {
        lv_timer_delete(s_timer);
        s_timer = NULL;
    }
    release_photo();
    s_owns_clock = app_clock_view_build_sleep(s_overlay, s_time_cb, s_time_context);
}

static bool show_photo(size_t index)
{
    const standby_prefs_t *prefs = &s_session_prefs;
    const standby_media_t *media = &prefs->photos[index];
    const char *root = media->source == 0U ? "A:/sdcard/image" : "A:/assets/image";
    char path[sizeof(s_photo_path)];
    const int len = snprintf(path, sizeof(path), "%s/%s", root, media->name);
    if (len < 0 || (size_t)len >= sizeof(path)) return false;
    lv_image_header_t header;
    if (lv_image_decoder_get_info(path, &header) != LV_RESULT_OK ||
        !photo_catalog_within_decode_budget(path, header.w, header.h)) return false;
    release_photo();
    (void)desktop_big_buffer_reserve((size_t)header.w * (size_t)header.h * 2U);
    lv_image_decoder_dsc_t decoded;
    if (lv_image_decoder_open(&decoded, path, NULL) != LV_RESULT_OK) return false;
    lv_image_decoder_close(&decoded);
    (void)snprintf(s_photo_path, sizeof(s_photo_path), "%s", path);
    lv_image_set_src(s_image, s_photo_path);
    lv_image_set_scale(s_image, photo_catalog_scale_for_screen(header.w, header.h, s_width, s_height));
    lv_obj_center(s_image);
    s_photo_index = index;
    return true;
}

static void photo_next(lv_timer_t *timer)
{
    (void)timer;
    const standby_prefs_t *prefs = &s_session_prefs;
    for (size_t step = 1U; step <= prefs->photo_count; ++step) {
        const size_t candidate = (s_photo_index + step) % prefs->photo_count;
        if (candidate == s_photo_index) continue;
        if (show_photo(candidate)) return;
    }

    lv_image_header_t header;
    if (lv_image_decoder_get_info(s_photo_path, &header) != LV_RESULT_OK) {
        ESP_LOGW(TAG, "all configured photos are unavailable; showing clock");
        show_clock();
    }
}

/**
 * @brief 在互斥锁内接收解码器提交的待显示帧。
 * @param index 解码帧所在的缓冲区索引。
 * @param width 帧宽度，单位为像素。
 * @param height 帧高度，单位为像素。
 * @param stride 帧行跨度，单位为字节。
 * @param pts_ms 帧显示时间戳，单位为毫秒；此回调不使用。
 * @param context 回调上下文；此回调不使用。
 * @return 接受帧时返回 true，否则返回 false。
 */
static bool frame_cb(uint8_t index, uint16_t width, uint16_t height,
                     uint32_t stride, uint32_t pts_ms, void *context)
{
    (void)pts_ms;
    (void)context;
    if (index > 1U || s_video.mutex == NULL || xSemaphoreTake(s_video.mutex, 0) != pdTRUE)
        return false;
    const bool accepted = s_video.active && !s_video.ready && s_video.buffers[index] != NULL &&
        video_player_dimensions_supported(width, height, s_video.limit_width, s_video.limit_height);
    if (accepted) {
        s_video.ready_index = index;
        s_video.frame_width = width;
        s_video.frame_height = height;
        s_video.stride = stride == 0U ? (uint32_t)width * 2U : stride;
        s_video.ready = true;
    }
    xSemaphoreGive(s_video.mutex);
    return accepted;
}

/**
 * @brief 消费待显示帧并刷新视频播放状态。
 * @param timer LVGL 视频刷新定时器。
 */
static void video_tick(lv_timer_t *timer)
{
    (void)timer;
    uint8_t index = 0U;
    uint16_t width = 0U;
    uint16_t height = 0U;
    uint32_t stride = 0U;
    bool ready = false;
    if (s_video.mutex != NULL && xSemaphoreTake(s_video.mutex, 0) == pdTRUE) {
        ready = s_video.ready;
        if (ready) {
            index = s_video.ready_index;
            width = s_video.frame_width;
            height = s_video.frame_height;
            stride = s_video.stride;
            s_video.ready = false;
        }
        xSemaphoreGive(s_video.mutex);
    }
    if (ready) {
        s_video.descriptor.header.magic = LV_IMAGE_HEADER_MAGIC;
        s_video.descriptor.header.cf = LV_COLOR_FORMAT_RGB565;
        s_video.descriptor.header.w = width;
        s_video.descriptor.header.h = height;
        s_video.descriptor.header.stride = stride;
        s_video.descriptor.data_size = stride * height;
        s_video.descriptor.data = s_video.buffers[index];
        lv_obj_set_size(s_image, width, height);
        lv_obj_center(s_image);
        lv_image_set_src(s_image, &s_video.descriptor);
        if (s_video.displayed && s_video.displayed_index != index)
            video_player_service_release_output_buffer(s_video.displayed_index);
        s_video.displayed_index = index;
        s_video.displayed = true;
    }
    video_player_snapshot_t snapshot;
    video_player_service_get_snapshot(&snapshot);
    if (snapshot.status == VIDEO_PLAYER_STATUS_ENDED) {
        if (video_player_service_play() != ESP_OK) {
            ESP_LOGW(TAG, "video replay failed; showing clock");
            show_clock();
        }
    } else if (snapshot.status == VIDEO_PLAYER_STATUS_ERROR) {
        ESP_LOGW(TAG, "video playback failed; showing clock");
        show_clock();
    }
}

static bool start_video(void)
{
    const standby_media_t *media = &s_session_prefs.video;
    const char *root = media->source == 0U ? "/sdcard/video" : "/assets/video";
    char path[VIDEO_PLAYER_PATH_MAX];
    const int len = snprintf(path, sizeof(path), "%s/%s", root, media->name);
    if (len < 0 || (size_t)len >= sizeof(path)) return false;
    if (video_player_service_enter_silent() != ESP_OK) return false;
    s_video.owns_service = true;
    s_video.active = true;
    s_video.limit_width = s_width;
    s_video.limit_height = s_height;
    video_player_align_resolution(&s_video.limit_width, &s_video.limit_height);
    const uint32_t capacity = (uint32_t)s_video.limit_width * s_video.limit_height * 2U;
    s_video.mutex = xSemaphoreCreateMutex();
    for (size_t i = 0U; i < 2U; ++i) {
        s_video.buffers[i] = heap_caps_aligned_alloc(VIDEO_BUFFER_ALIGNMENT, capacity,
                                                    MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }
    if (s_video.mutex == NULL || s_video.buffers[0] == NULL || s_video.buffers[1] == NULL ||
        video_player_service_set_display_size(s_video.limit_width, s_video.limit_height) != ESP_OK ||
        video_player_service_set_output_buffers(s_video.buffers[0], s_video.buffers[1], capacity) != ESP_OK ||
        video_player_service_select_path(path) != ESP_OK) return false;
    s_image = lv_image_create(s_overlay);
    lv_obj_center(s_image);
    if (video_player_service_set_frame_callback(frame_cb, NULL) != ESP_OK ||
        video_player_service_play() != ESP_OK) return false;
    s_timer = lv_timer_create(video_tick, VIDEO_REFRESH_MS, NULL);
    return s_timer != NULL;
}

void desktop_sleep_view_refresh(void)
{
    if (s_owns_clock) app_clock_view_refresh_sleep();
}

void desktop_sleep_view_destroy(void)
{
    if (s_owns_clock) {
        app_clock_view_destroy();
        s_owns_clock = false;
    }
    if (s_mode == STANDBY_VIDEO || s_video.owns_service) release_video();
    if (s_timer != NULL) {
        lv_timer_delete(s_timer);
        s_timer = NULL;
    }
    release_photo();
    if (s_overlay != NULL) {
        lv_obj_delete(s_overlay);
        s_overlay = NULL;
    }
    s_image = NULL;
}

void desktop_sleep_view_build(lv_obj_t *parent, uint16_t screen_width, uint16_t screen_height,
                            desktop_sleep_view_time_cb_t time_cb, void *time_context)
{
    if (parent == NULL || s_overlay != NULL || screen_width == 0U || screen_height == 0U) return;
    s_width = screen_width;
    s_height = screen_height;
    s_time_cb = time_cb;
    s_time_context = time_context;
    s_overlay = lv_obj_create(parent);
    if (s_overlay == NULL) return;
    lv_obj_remove_style_all(s_overlay);
    lv_obj_set_style_bg_color(s_overlay, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_overlay, LV_OPA_COVER, 0);
    lv_obj_set_size(s_overlay, screen_width, screen_height);
    lv_obj_set_pos(s_overlay, 0, 0);
    lv_obj_clear_flag(s_overlay, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_overlay, LV_OBJ_FLAG_CLICKABLE);
    s_session_prefs = *standby_prefs_get();
    const standby_prefs_t *prefs = &s_session_prefs;
    s_mode = prefs->mode;
    if (s_mode == STANDBY_PHOTOS) {
        s_image = lv_image_create(s_overlay);
        if (s_image == NULL) {
            show_clock();
            if (!desktop_sleep_view_is_built()) desktop_sleep_view_destroy();
            return;
        }
        lv_obj_center(s_image);
        for (size_t i = 0U; i < prefs->photo_count; ++i) {
            if (show_photo(i)) {
                s_timer = lv_timer_create(photo_next, prefs->photo_interval_seconds * 1000U, NULL);
                break;
            }
        }
        if (s_timer == NULL) show_clock();
    } else if (s_mode == STANDBY_VIDEO) {
        if (!start_video()) show_clock();
    } else {
        show_clock();
    }
    if (!desktop_sleep_view_is_built()) desktop_sleep_view_destroy();
}

bool desktop_sleep_view_is_built(void)
{
    return s_overlay != NULL && (s_owns_clock || (s_mode == STANDBY_PHOTOS && s_image != NULL) ||
                                 (s_mode == STANDBY_VIDEO && s_timer != NULL));
}
