#include "app_video_player_view.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "assets_storage.h"
#include "audio_volume.h"
#include "display.h"
#include "desktop_font.h"
#include "desktop_language.h"
#include "video_player_service.h"

#define L(key) desktop_language_lookup(key)
#define VIDEO_BYTES_PER_PIXEL 2U
#define VIDEO_BUFFER_ALIGNMENT 64U
#define VIDEO_REFRESH_MS 40U
#define VIDEO_CONTROLS_HIDE_MS 5000U

#define VIDEO_ICON_PLAY "A:/assets/icon/video/play.png"
#define VIDEO_ICON_PAUSE "A:/assets/icon/video/pause.png"
#define VIDEO_ICON_PREVIOUS "A:/assets/icon/video/prev.png"
#define VIDEO_ICON_NEXT "A:/assets/icon/video/next.png"
#define VIDEO_ICON_VOLUME "A:/assets/icon/video/volume.png"

#define VIDEO_PLAY_ICON_WIDTH 102
#define VIDEO_STEP_ICON_WIDTH 62
#define VIDEO_TRANSPORT_GAP 248
#define VIDEO_VOLUME_HUD_WIDTH 64
#define VIDEO_VOLUME_HUD_HEIGHT 190

#define C_CANVAS lv_color_hex(0x000000)
#define C_SURFACE lv_color_hex(0x101318)
#define C_MUTED lv_color_hex(0x28313C)
#define C_INK lv_color_hex(0xF5F7FA)
#define C_INK_MUTED lv_color_hex(0xA8B2BF)
#define C_ACCENT lv_color_hex(0x147EFB)
#define C_WHITE lv_color_hex(0xFFFFFF)

static const char *const TAG = "VideoView";

typedef enum {
    VIDEO_ACTION_PREVIOUS = 0,
    VIDEO_ACTION_PLAY_PAUSE,
    VIDEO_ACTION_STOP,
    VIDEO_ACTION_NEXT,
} video_action_t;

typedef struct {
    lv_obj_t *surface;
    lv_obj_t *frame;
    lv_obj_t *control_panel;
    lv_obj_t *image;
    lv_obj_t *name;
    lv_obj_t *time;
    lv_obj_t *progress;
    lv_obj_t *play_button;
    lv_obj_t *play_icon;
    lv_obj_t *volume_button;
    lv_obj_t *volume_hud;
    lv_obj_t *volume_slider;
    lv_timer_t *timer;
    app_video_player_view_toast_cb_t toast_cb;

    uint8_t *display_buffers[2]; /**< 页面持有 RGB565 双缓冲区，解码器直接写入空闲缓冲区。 当前显示缓冲区须在 LVGL 解除引用后才能归还解码器。 */
    uint8_t ready_index;
    uint8_t displayed_index;
    uint32_t display_capacity;
    uint16_t display_width;
    uint16_t display_height;
    SemaphoreHandle_t frame_mutex;
    uint16_t frame_width;
    uint16_t frame_height;
    uint32_t frame_stride;
    uint16_t displayed_width;
    uint16_t displayed_height;
    bool frame_ready;
    bool has_displayed;
    bool controls_visible;
    bool icons_ready;
    bool active;
    uint32_t last_interaction_tick;
    lv_image_dsc_t image_dsc;
} app_video_view_state_t;

static app_video_view_state_t s_video;

static void style_panel(lv_obj_t *object, lv_color_t color, lv_opa_t opacity)
{
    lv_obj_remove_style_all(object);
    lv_obj_set_style_bg_color(object, color, 0);
    lv_obj_set_style_bg_opa(object, opacity, 0);
    lv_obj_set_style_border_width(object, 0, 0);
    lv_obj_set_style_radius(object, 0, 0);
    lv_obj_set_style_pad_all(object, 0, 0);
    lv_obj_clear_flag(object, LV_OBJ_FLAG_SCROLLABLE);
}

static lv_obj_t *flex_label(lv_obj_t *parent, const char *text, lv_color_t color)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_color(label, color, 0);
    lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
    return label;
}

static lv_obj_t *icon_control(lv_obj_t *parent, const char *path, const char *symbol,
                              lv_event_cb_t callback, void *user_data)
{
    lv_obj_t *control = NULL;
    if (s_video.icons_ready) {
        control = lv_image_create(parent);
        lv_image_set_src(control, path);
    } else {
        control = lv_label_create(parent);
        lv_label_set_text(control, symbol);
        lv_obj_set_style_text_color(control, C_INK, 0);
    }
    lv_obj_add_flag(control, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(control, callback, LV_EVENT_CLICKED, user_data);
    return control;
}

static void set_play_icon(bool playing)
{
    if (s_video.play_icon == NULL) {
        return;
    }
    if (s_video.icons_ready) {
        lv_image_set_src(s_video.play_icon, playing ? VIDEO_ICON_PAUSE : VIDEO_ICON_PLAY);
    } else {
        lv_label_set_text(s_video.play_icon, playing ? LV_SYMBOL_PAUSE : LV_SYMBOL_PLAY);
    }
}

static void format_time(uint32_t milliseconds, char *buffer, size_t size)
{
    const uint32_t seconds = milliseconds / 1000U;
    (void)snprintf(buffer, size, "%02" PRIu32 ":%02" PRIu32,
                   seconds / 60U, seconds % 60U);
}

static void show_controls(void)
{
    if (!s_video.active || s_video.control_panel == NULL) {
        return;
    }
    lv_obj_remove_flag(s_video.control_panel, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_video.control_panel);
    s_video.controls_visible = true;
    s_video.last_interaction_tick = lv_tick_get();
}

static void video_touch_cb(lv_event_t *event)
{
    (void)event;
    show_controls();
}

static void volume_hud_refresh(void)
{
    if (s_video.volume_slider == NULL) {
        return;
    }
    audio_volume_state_t state = {0};

    (void)audio_volume_get(&state);
    lv_slider_set_value(s_video.volume_slider, (int32_t)state.level, LV_ANIM_OFF);
}

static void volume_slider_cb(lv_event_t *event)
{
    lv_obj_t *slider = lv_event_get_target(event);
    if (!s_video.active || slider == NULL) {
        return;
    }

    if (audio_volume_set((uint8_t)lv_slider_get_value(slider)) != ESP_OK) {
        volume_hud_refresh();
        return;
    }
    show_controls();
}

static void volume_button_cb(lv_event_t *event)
{
    (void)event;
    if (!s_video.active || s_video.volume_hud == NULL) {
        return;
    }
    volume_hud_refresh();
    lv_obj_remove_flag(s_video.volume_hud, LV_OBJ_FLAG_HIDDEN);
    show_controls();
}

/**
 * @brief 按帧尺寸更新图像描述符和显示比例。
 * @param width 帧宽度，单位为像素。
 * @param height 帧高度，单位为像素。
 */
static void apply_frame_geometry(uint16_t width, uint16_t height)
{
    if (s_video.frame == NULL || s_video.image == NULL ||
        !video_player_dimensions_supported(width, height, s_video.display_width,
                                           s_video.display_height)) {
        return;
    }
    if (s_video.displayed_width == width && s_video.displayed_height == height) {
        return;
    }
    s_video.displayed_width = width;
    s_video.displayed_height = height;
    lv_obj_set_size(s_video.frame, width, height);
    lv_obj_center(s_video.frame);
    lv_obj_set_size(s_video.image, width, height);
    lv_obj_center(s_video.image);
    lv_obj_set_size(s_video.control_panel, width, height);
    lv_obj_center(s_video.control_panel);
    lv_obj_move_foreground(s_video.control_panel);
}

static void reset_frame_geometry(void)
{
    if (s_video.surface == NULL || s_video.frame == NULL) {
        return;
    }
    if (s_video.frame_mutex != NULL && xSemaphoreTake(s_video.frame_mutex, 0) == pdTRUE) {
        if (s_video.frame_ready) {
            video_player_service_release_output_buffer(s_video.ready_index);
        }
        s_video.frame_ready = false;
        xSemaphoreGive(s_video.frame_mutex);
    }
    if (s_video.image != NULL) {
        lv_obj_add_flag(s_video.image, LV_OBJ_FLAG_HIDDEN);
        lv_image_set_src(s_video.image, NULL);
    }

    if (s_video.has_displayed) {
        video_player_service_release_output_buffer(s_video.displayed_index);
        s_video.has_displayed = false;
    }
    lv_obj_update_layout(s_video.surface);
    const int32_t width = lv_obj_get_content_width(s_video.surface);
    const int32_t height = lv_obj_get_content_height(s_video.surface);
    if (width <= 0 || height <= 0) {
        return;
    }
    s_video.displayed_width = 0U;
    s_video.displayed_height = 0U;
    lv_obj_set_size(s_video.frame, width, height);
    lv_obj_center(s_video.frame);
    lv_obj_set_size(s_video.image, width, height);
    lv_obj_center(s_video.image);
    lv_obj_add_flag(s_video.image, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_size(s_video.control_panel, width, height);
    lv_obj_center(s_video.control_panel);
    show_controls();
}

/**
 * @brief 将待显示帧提交给 LVGL，并归还已释放的显示缓冲区。
 */
static void view_render(void)
{
    if (!s_video.active) {
        return;
    }
    video_player_snapshot_t snapshot = {0};
    video_player_service_get_snapshot(&snapshot);
    lv_label_set_text(s_video.name,
                      snapshot.has_media ? snapshot.name : L(DESKTOP_TEXT_VIDEO_NO_MEDIA));

    char position[12];
    char duration[12];
    char time[32];
    format_time(snapshot.position_ms, position, sizeof(position));
    format_time(snapshot.duration_ms, duration, sizeof(duration));
    (void)snprintf(time, sizeof(time), "%s / %s", position, duration);
    lv_label_set_text(s_video.time, time);
    lv_bar_set_range(s_video.progress, 0,
                     snapshot.duration_ms == 0U ? 1 : (int32_t)snapshot.duration_ms);
    lv_bar_set_value(s_video.progress, (int32_t)snapshot.position_ms, LV_ANIM_OFF);

    set_play_icon(snapshot.status == VIDEO_PLAYER_STATUS_PLAYING);
}

/**
 * @brief 在互斥锁内提交解码帧；待显示帧尚未消费时拒绝新帧。
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
    if (buffer_index > 1U || s_video.frame_mutex == NULL) {
        return false;
    }
    if (xSemaphoreTake(s_video.frame_mutex, pdMS_TO_TICKS(10U)) != pdTRUE) {
        return false;
    }
    bool accepted = false;

    if (s_video.active && !s_video.frame_ready && s_video.display_buffers[buffer_index] != NULL &&
        video_player_dimensions_supported(width, height, s_video.display_width,
                                          s_video.display_height)) {
        s_video.ready_index = buffer_index;
        s_video.frame_width = width;
        s_video.frame_height = height;
        s_video.frame_stride = stride == 0U ? (uint32_t)width * VIDEO_BYTES_PER_PIXEL : stride;
        s_video.frame_ready = true;
        accepted = true;
    }
    xSemaphoreGive(s_video.frame_mutex);
    return accepted;
}

static void action_cb(lv_event_t *event)
{
    show_controls();
    const video_action_t action = (video_action_t)(uintptr_t)lv_event_get_user_data(event);
    video_player_snapshot_t snapshot = {0};
    video_player_service_get_snapshot(&snapshot);
    esp_err_t result = ESP_OK;
    switch (action) {
    case VIDEO_ACTION_PREVIOUS:
        result = video_player_service_previous();
        if (result == ESP_OK) reset_frame_geometry();
        break;
    case VIDEO_ACTION_PLAY_PAUSE:
        result = snapshot.status == VIDEO_PLAYER_STATUS_PLAYING
                     ? video_player_service_pause()
                     : video_player_service_play();
        break;
    case VIDEO_ACTION_STOP:
        result = video_player_service_stop();
        break;
    case VIDEO_ACTION_NEXT:
        result = video_player_service_next();
        if (result == ESP_OK) reset_frame_geometry();
        break;
    default:
        result = ESP_ERR_INVALID_ARG;
        break;
    }
    if (result != ESP_OK && s_video.toast_cb != NULL) {
        s_video.toast_cb(snapshot.has_media
                             ? L(DESKTOP_TEXT_VIDEO_FORMAT_ERROR)
                             : L(DESKTOP_TEXT_VIDEO_NO_MEDIA));
    }
    view_render();
}

/**
 * @brief 刷新播放状态、视频帧和控件自动隐藏状态。
 * @param timer LVGL 刷新定时器。
 */
static void refresh_cb(lv_timer_t *timer)
{
    (void)timer;
    bool frame_ready = false;
    uint8_t buffer_index = 0U;
    uint16_t width = 0U;
    uint16_t height = 0U;
    uint32_t stride = 0U;
    if (s_video.frame_mutex != NULL &&
        xSemaphoreTake(s_video.frame_mutex, 0) == pdTRUE) {
        if (s_video.frame_ready) {
            frame_ready = true;
            buffer_index = s_video.ready_index;
            width = s_video.frame_width;
            height = s_video.frame_height;
            stride = s_video.frame_stride;
            s_video.frame_ready = false;
        }
        xSemaphoreGive(s_video.frame_mutex);
    }
    if (frame_ready &&
        video_player_dimensions_supported(width, height, s_video.display_width,
                                          s_video.display_height)) {
        apply_frame_geometry(width, height);
        s_video.image_dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
        s_video.image_dsc.header.cf = LV_COLOR_FORMAT_RGB565;
        s_video.image_dsc.header.w = width;
        s_video.image_dsc.header.h = height;
        s_video.image_dsc.header.stride = stride;
        s_video.image_dsc.data_size = stride * height;
        s_video.image_dsc.data = s_video.display_buffers[buffer_index];
        lv_image_set_src(s_video.image, &s_video.image_dsc);
        lv_obj_remove_flag(s_video.image, LV_OBJ_FLAG_HIDDEN);
        if (s_video.has_displayed && s_video.displayed_index != buffer_index) {
            video_player_service_release_output_buffer(s_video.displayed_index);
        }
        s_video.displayed_index = buffer_index;
        s_video.has_displayed = true;
    }
    view_render();
    if (s_video.controls_visible &&
        lv_tick_elaps(s_video.last_interaction_tick) >= VIDEO_CONTROLS_HIDE_MS) {
        lv_obj_add_flag(s_video.control_panel, LV_OBJ_FLAG_HIDDEN);
        if (s_video.volume_hud != NULL) {
            lv_obj_add_flag(s_video.volume_hud, LV_OBJ_FLAG_HIDDEN);
        }
        s_video.controls_visible = false;
    }
}

void app_video_player_view_build(lv_obj_t *parent,
                                   app_video_player_view_toast_cb_t toast_cb)
{
    app_video_player_view_cleanup();
    if (parent == NULL) {
        return;
    }
    uint16_t display_width = 0U;
    uint16_t display_height = 0U;
    if (display_get_size(&display_width, &display_height) != ESP_OK) {
        ESP_LOGE(TAG, "read the board panel size failed");
        if (toast_cb != NULL) {
            toast_cb(L(DESKTOP_TEXT_VIDEO_ERROR));
        }
        return;
    }

    video_player_align_resolution(&display_width, &display_height);
    s_video.active = true;
    s_video.toast_cb = toast_cb;

    s_video.display_width = display_width;
    s_video.display_height = display_height;
    s_video.display_capacity = (uint32_t)display_width * display_height * VIDEO_BYTES_PER_PIXEL;
    (void)video_player_service_set_display_size(display_width, display_height);
    s_video.frame_mutex = xSemaphoreCreateMutex();
    for (size_t i = 0; i < 2U; ++i) {
        s_video.display_buffers[i] = heap_caps_aligned_alloc(
            VIDEO_BUFFER_ALIGNMENT, s_video.display_capacity,
            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (s_video.display_buffers[i] == NULL) {
            s_video.display_buffers[i] = heap_caps_aligned_alloc(
                VIDEO_BUFFER_ALIGNMENT, s_video.display_capacity, MALLOC_CAP_8BIT);
        }
    }
    if (s_video.frame_mutex == NULL || s_video.display_buffers[0] == NULL ||
        s_video.display_buffers[1] == NULL) {
        ESP_LOGE(TAG, "Video frame buffer allocation failed");
        if (toast_cb != NULL) {
            toast_cb(L(DESKTOP_TEXT_VIDEO_ERROR));
        }
    } else {
        (void)video_player_service_set_output_buffers(s_video.display_buffers[0],
                                                      s_video.display_buffers[1],
                                                      s_video.display_capacity);
    }

    s_video.surface = lv_obj_create(parent);
    style_panel(s_video.surface, C_CANVAS, LV_OPA_COVER);
    lv_obj_set_size(s_video.surface, LV_PCT(100), LV_PCT(100));
    lv_obj_center(s_video.surface);
    lv_obj_add_flag(s_video.surface, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_video.surface, video_touch_cb, LV_EVENT_PRESSED, NULL);

    s_video.frame = lv_obj_create(s_video.surface);
    style_panel(s_video.frame, C_CANVAS, LV_OPA_COVER);
    lv_obj_add_flag(s_video.frame, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_video.frame, video_touch_cb, LV_EVENT_PRESSED, NULL);

    s_video.image = lv_image_create(s_video.frame);
    lv_obj_clear_flag(s_video.image, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(s_video.image, LV_OBJ_FLAG_HIDDEN);

    s_video.control_panel = lv_obj_create(s_video.frame);
    style_panel(s_video.control_panel, C_SURFACE, LV_OPA_TRANSP);
    lv_obj_add_flag(s_video.control_panel, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_video.control_panel, video_touch_cb, LV_EVENT_PRESSED, NULL);

    s_video.icons_ready = assets_storage_icons_ready();

    const int32_t step_offset =
        VIDEO_PLAY_ICON_WIDTH / 2 + VIDEO_TRANSPORT_GAP + VIDEO_STEP_ICON_WIDTH / 2;
    s_video.play_button = icon_control(s_video.control_panel, VIDEO_ICON_PLAY, LV_SYMBOL_PLAY,
                                       action_cb, (void *)(uintptr_t)VIDEO_ACTION_PLAY_PAUSE);
    s_video.play_icon = s_video.play_button;
    lv_obj_align(s_video.play_button, LV_ALIGN_CENTER, 0, 0);
    lv_obj_t *previous = icon_control(s_video.control_panel, VIDEO_ICON_PREVIOUS, LV_SYMBOL_LEFT,
                                      action_cb, (void *)(uintptr_t)VIDEO_ACTION_PREVIOUS);
    lv_obj_align(previous, LV_ALIGN_CENTER, -step_offset, 0);
    lv_obj_t *next = icon_control(s_video.control_panel, VIDEO_ICON_NEXT, LV_SYMBOL_RIGHT,
                                  action_cb, (void *)(uintptr_t)VIDEO_ACTION_NEXT);
    lv_obj_align(next, LV_ALIGN_CENTER, step_offset, 0);

    lv_obj_t *info = lv_obj_create(s_video.control_panel);
    style_panel(info, C_SURFACE, LV_OPA_60);
    lv_obj_set_size(info, LV_PCT(100), 44);
    lv_obj_align(info, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_pad_left(info, 20, 0);
    lv_obj_set_style_pad_right(info, 20, 0);
    lv_obj_set_flex_flow(info, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(info, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    s_video.name = flex_label(info, L(DESKTOP_TEXT_VIDEO_NO_MEDIA), C_INK);

    lv_obj_t *bottom = lv_obj_create(s_video.control_panel);
    style_panel(bottom, C_SURFACE, LV_OPA_TRANSP);
    lv_obj_set_size(bottom, LV_PCT(96), 20);
    lv_obj_align(bottom, LV_ALIGN_BOTTOM_MID, 0, -10);
    lv_obj_set_flex_flow(bottom, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(bottom, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(bottom, 12, 0);
    s_video.time = flex_label(bottom, "00:00 / 00:00", C_INK_MUTED);
    s_video.progress = lv_bar_create(bottom);
    lv_obj_set_height(s_video.progress, 8);
    lv_obj_set_flex_grow(s_video.progress, 1);

    lv_obj_set_style_bg_color(s_video.progress, C_WHITE, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_video.progress, LV_OPA_50, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_video.progress, C_WHITE, LV_PART_INDICATOR);

    s_video.volume_button = icon_control(s_video.control_panel, VIDEO_ICON_VOLUME,
                                         LV_SYMBOL_AUDIO, volume_button_cb, NULL);
    lv_obj_align(s_video.volume_button, LV_ALIGN_BOTTOM_RIGHT, -20, -34);

    s_video.volume_hud = lv_obj_create(s_video.control_panel);
    style_panel(s_video.volume_hud, C_SURFACE, LV_OPA_80);
    lv_obj_set_style_radius(s_video.volume_hud, 18, 0);
    lv_obj_set_style_pad_all(s_video.volume_hud, 12, 0);
    lv_obj_set_size(s_video.volume_hud, VIDEO_VOLUME_HUD_WIDTH, VIDEO_VOLUME_HUD_HEIGHT);
    lv_obj_align(s_video.volume_hud, LV_ALIGN_BOTTOM_RIGHT, -22, -104);
    lv_obj_add_flag(s_video.volume_hud, LV_OBJ_FLAG_HIDDEN);
    s_video.volume_slider = lv_slider_create(s_video.volume_hud);
    lv_obj_set_size(s_video.volume_slider, 14, 150);
    lv_slider_set_orientation(s_video.volume_slider, LV_SLIDER_ORIENTATION_VERTICAL);
    lv_slider_set_range(s_video.volume_slider, AUDIO_VOLUME_MIN, AUDIO_VOLUME_MAX);
    lv_obj_set_style_bg_color(s_video.volume_slider, C_MUTED, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_video.volume_slider, C_ACCENT, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(s_video.volume_slider, C_INK, LV_PART_KNOB);
    lv_obj_set_style_radius(s_video.volume_slider, LV_RADIUS_CIRCLE, LV_PART_KNOB);
    lv_obj_set_style_width(s_video.volume_slider, 18, LV_PART_KNOB);
    lv_obj_set_style_height(s_video.volume_slider, 18, LV_PART_KNOB);

    lv_obj_set_ext_click_area(s_video.volume_slider, 14);
    lv_obj_center(s_video.volume_slider);
    lv_obj_add_event_cb(s_video.volume_slider, volume_slider_cb, LV_EVENT_VALUE_CHANGED, NULL);

    const lv_font_t *font = desktop_font_get_primary();
    if (font != NULL) {
        lv_obj_set_style_text_font(s_video.surface, font, 0);
    }
    reset_frame_geometry();
    (void)video_player_service_set_frame_callback(frame_cb, NULL);
    s_video.timer = lv_timer_create(refresh_cb, VIDEO_REFRESH_MS, NULL);
    view_render();
}

void app_video_player_view_cleanup(void)
{
    s_video.active = false;
    (void)video_player_service_set_frame_callback(NULL, NULL);
    if (s_video.timer != NULL) {
        lv_timer_delete(s_video.timer);
    }
    if (s_video.image != NULL) {
        lv_obj_add_flag(s_video.image, LV_OBJ_FLAG_HIDDEN);

        lv_image_set_src(s_video.image, NULL);
    }
    if (s_video.has_displayed) {
        video_player_service_release_output_buffer(s_video.displayed_index);
        s_video.has_displayed = false;
    }
    const bool unregistered =
        video_player_service_set_output_buffers(NULL, NULL, 0U) == ESP_OK;
    if (s_video.frame_mutex != NULL) {
        (void)xSemaphoreTake(s_video.frame_mutex, portMAX_DELAY);
        if (unregistered) {
            for (size_t i = 0; i < 2U; ++i) {
                if (s_video.display_buffers[i] != NULL) {
                    heap_caps_free(s_video.display_buffers[i]);
                    s_video.display_buffers[i] = NULL;
                }
            }
        } else {
            ESP_LOGW(TAG, "decoder still owns the display buffers; keeping them to avoid a use-after-free");
        }
        xSemaphoreGive(s_video.frame_mutex);
        vSemaphoreDelete(s_video.frame_mutex);
    }
    memset(&s_video, 0, sizeof(s_video));
}
