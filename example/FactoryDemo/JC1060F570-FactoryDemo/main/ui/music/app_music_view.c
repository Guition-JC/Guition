#include "app_music_view.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "music_player.h"
#include "esp_bt_audio_defs.h"
#include "driver/jpeg_decode.h"
#if defined(ESP_PLATFORM)
#include "esp_attr.h"
#endif
#include "music_player_service.h"
#include "esp_err.h"
#include "desktop_font.h"
#include "desktop_fullscreen_layout.h"
#include "desktop_language.h"
#include "app_music_anim.h"
#include "app_music_assets.h"
#include "assets_storage.h"

#include "image/lv_image_decoder_private.h"
#include "esp_log.h"
#include "sdkconfig.h"
#include "desktop_big_buffer.h"
#include "misc/cache/instance/lv_image_cache.h"

#define TAG "MusicView"

#define L(key) desktop_language_lookup(key)

#define MUSIC_ARM_ANIM_MS 450
#define MUSIC_DISC_TURN_MS 10000
#define MUSIC_POLL_MS 500
#if CONFIG_ESP_BOARD_JC8048F550

#define MUSIC_DISC_PIVOT_X 128
#define MUSIC_DISC_PIVOT_Y 128
#define MUSIC_ALBUM_SIZE 158
#define MUSIC_TONEARM_PIVOT_X 101
#define MUSIC_TONEARM_PIVOT_Y 20
#define MUSIC_ARTWORK_SIZE 300
#define MUSIC_TONEARM_OFFSET_X 68
#define MUSIC_ARTWORK_OFFSET_X 0
#define MUSIC_BUTTON_GAP 16
#define MUSIC_LEFT_PERCENT 56
#define MUSIC_RIGHT_PERCENT 42
#define MUSIC_LEFT_PAD 16
#define MUSIC_VOLUME_HUD_WIDTH 340
#else
#define MUSIC_DISC_PIVOT_X 152
#define MUSIC_DISC_PIVOT_Y 152
#define MUSIC_ALBUM_SIZE 188
#define MUSIC_TONEARM_PIVOT_X 120
#define MUSIC_TONEARM_PIVOT_Y 24
#define MUSIC_ARTWORK_SIZE 340
#define MUSIC_TONEARM_OFFSET_X 81
#define MUSIC_ARTWORK_OFFSET_X (-20)
#define MUSIC_BUTTON_GAP 32
#define MUSIC_LEFT_PERCENT 62
#define MUSIC_RIGHT_PERCENT 36
#define MUSIC_LEFT_PAD 20
#define MUSIC_VOLUME_HUD_WIDTH 400
#endif

#define MUSIC_ARTWORK_DROP 0

#define MUSIC_TONEARM_OFFSET_Y 0

#define MUSIC_ICON_DIR "A:/assets/icon/music/"
#define MUSIC_ICON_PROBE_POSIX "/assets/icon/music/icon_c.png"
#define MUSIC_ICON_SIZE 40
#define MUSIC_BUTTON_SIZE 56
#define MUSIC_PLAY_BUTTON_SIZE 79

#define MUSIC_CONTROLS_WIDTH \
    (MUSIC_BUTTON_SIZE * 4 + MUSIC_PLAY_BUTTON_SIZE + MUSIC_BUTTON_GAP * 4)
#define MUSIC_CONTROLS_HEIGHT MUSIC_PLAY_BUTTON_SIZE

#define MUSIC_BACKGROUND_PATH "A:/assets/image/4.jpg"
#define MUSIC_BACKGROUND_PROBE_POSIX "/assets/image/4.jpg"

#define MUSIC_PROGRESS_TRACK_OPA 64

#define MUSIC_PROGRESS_GAP 12

#define MUSIC_LYRICS_TOP 90

#define MUSIC_VOLUME_HUD_HEIGHT 56
#define MUSIC_VOLUME_HUD_GAP 12
#define MUSIC_VOLUME_HUD_VALUE_WIDTH 44
#define MUSIC_VOLUME_HUD_HIDE_MS 3000

#define MUSIC_VOLUME_LEVEL_MIN 0
#define MUSIC_VOLUME_LEVEL_MAX 100

#define MUSIC_COVER_ART_MAX_DIMENSION 4096U
#define MUSIC_COVER_ART_HW_JPEG_MIN_DIMENSION 64U
#define MUSIC_COVER_ART_HW_JPEG_ALIGNMENT 16U
#define MUSIC_COVER_ART_DECODED_BYTES_PER_PIXEL 2U

#define MUSIC_COVER_ART_PNG_BYTES_PER_PIXEL 4U

#define MUSIC_COVER_ART_DECODED_BYTES_MAX (4U * 1024U * 1024U)

#define C_CANVAS lv_color_hex(0x000000)
#define C_SURFACE lv_color_hex(0x080808)
#define C_MUTED lv_color_hex(0x242424)
#define C_INK lv_color_hex(0xF5F5F5)
#define C_INK_MUTED lv_color_hex(0x929292)
#define C_ACCENT lv_color_hex(0x3D9BFF)
#define C_ACCENT_SOFT lv_color_hex(0x142A3D)
#define C_DIVIDER lv_color_hex(0x2A2A2A)

#define C_DISC lv_color_hex(0x32314A)

#define MUSIC_TRACK_COUNT 3U

typedef enum {
    MUSIC_ACTION_PREVIOUS = 0,
    MUSIC_ACTION_PLAY,
    MUSIC_ACTION_NEXT,
    MUSIC_ACTION_VOLUME,
} music_action_t;

typedef enum {
    MUSIC_BUTTON_VOLUME = 0,
    MUSIC_BUTTON_PREVIOUS,
    MUSIC_BUTTON_PLAY,
    MUSIC_BUTTON_NEXT,
    MUSIC_BUTTON_SOURCE,
    MUSIC_BUTTON_COUNT,
} music_button_id_t;

typedef struct {
    lv_obj_t *button;

    lv_obj_t *icon;

    const char *released;
    const char *pressed;
} music_button_t;

typedef struct {
    lv_obj_t *title;
    lv_obj_t *artist;
    lv_obj_t *album;
    lv_obj_t *controls;
    lv_obj_t *volume_hud;
    lv_obj_t *volume_slider;
    lv_obj_t *volume_value;
    lv_obj_t *time;
    lv_obj_t *progress;
    lv_obj_t *progress_block;
    lv_obj_t *disc;
    lv_obj_t *tonearm;
    lv_obj_t *album_image;
    lv_obj_t *background;
    music_button_t buttons[MUSIC_BUTTON_COUNT];
    lv_timer_t *poll_timer;
    lv_timer_t *volume_hide_timer;
    app_music_anim_t animation;
    app_music_view_toast_cb_t toast_cb;
    music_player_snapshot_t snapshot;
    uint32_t cover_art_seen_revision;
    bool cover_art_active;
    uint32_t generation;
    uint32_t arm_generation;
    uint32_t track_index;
    music_player_source_t selected_source;
    bool snapshot_valid;
    bool playing;
    bool active;
    bool use_icon_assets;
} app_music_view_state_t;

#if defined(ESP_PLATFORM)
EXT_RAM_BSS_ATTR static lv_image_dsc_t s_cover_art_image;
#else
static lv_image_dsc_t s_cover_art_image;
#endif

static uint8_t *s_cover_art;

/**
 * @brief 获取页面持有的封面缓冲区；调用方须持有 LVGL 锁。
 * @return 页面持有的缓冲区；分配失败时为 NULL。
 */
static uint8_t *cover_art_buffer(void)
{
    if (s_cover_art == NULL) {
        s_cover_art = heap_caps_malloc(MUSIC_PLAYER_COVER_ART_MAX_SIZE, MALLOC_CAP_SPIRAM);
    }
    return s_cover_art;
}

#if defined(ESP_PLATFORM)
EXT_RAM_BSS_ATTR static app_music_view_state_t s_music;
#else
static app_music_view_state_t s_music;
#endif

static const desktop_text_key_t s_track_keys[MUSIC_TRACK_COUNT] = {
    DESKTOP_TEXT_MUSIC_TRACK_1,
    DESKTOP_TEXT_MUSIC_TRACK_2,
    DESKTOP_TEXT_MUSIC_TRACK_3,
};

static const desktop_text_key_t s_artist_keys[MUSIC_TRACK_COUNT] = {
    DESKTOP_TEXT_MUSIC_ARTIST_1,
    DESKTOP_TEXT_MUSIC_ARTIST_2,
    DESKTOP_TEXT_MUSIC_ARTIST_3,
};

static const char *const s_button_icons[MUSIC_BUTTON_COUNT] = {
    MUSIC_ICON_DIR "icon_vol.png",
    MUSIC_ICON_DIR "icon_prev.png",
    MUSIC_ICON_DIR "icon_play.png",
    MUSIC_ICON_DIR "icon_next.png",
    MUSIC_ICON_DIR "icon_bt.png",
};

static const char *const s_button_pressed_icons[MUSIC_BUTTON_COUNT] = {
    MUSIC_ICON_DIR "icon_vol_p.png",
    MUSIC_ICON_DIR "icon_prev_p.png",
    NULL,
    MUSIC_ICON_DIR "icon_next_p.png",
    MUSIC_ICON_DIR "icon_bt_p.png",
};

static const char *const s_button_glyphs[MUSIC_BUTTON_COUNT] = {
    LV_SYMBOL_AUDIO,
    LV_SYMBOL_PREV,
    LV_SYMBOL_PLAY,
    LV_SYMBOL_NEXT,
    LV_SYMBOL_BLUETOOTH,
};

static lv_obj_t *music_label(lv_obj_t *parent, const char *text, lv_color_t color,
                             int32_t x, int32_t y)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_label_set_text(label, text);
    lv_obj_set_pos(label, x, y);
    lv_obj_set_style_text_color(label, color, 0);
    lv_obj_set_style_text_font(label, &lv_font_montserrat_14, 0);
    return label;
}

static bool music_file_readable(const char *posix_path)
{
    FILE *probe = fopen(posix_path, "rb");
    if (probe == NULL) {
        return false;
    }
    fclose(probe);
    return true;
}

static void music_panel_make_transparent(lv_obj_t *panel)
{
    if (panel != NULL) {
        lv_obj_set_style_bg_opa(panel, LV_OPA_TRANSP, 0);
    }
}

static void music_button_feedback_cb(lv_event_t *event)
{
    music_button_t *button = lv_event_get_user_data(event);
    if (button == NULL || button->icon == NULL || button->pressed == NULL) {
        return;
    }

    const bool pressed = lv_obj_has_state(lv_event_get_target(event), LV_STATE_PRESSED);
    lv_image_set_src(button->icon, pressed ? button->pressed : button->released);
}

static void music_button_set_icon(music_button_t *button, const char *icon, const char *glyph)
{
    if (button == NULL || button->icon == NULL) {
        return;
    }
    if (s_music.use_icon_assets) {
        button->released = icon;
        lv_image_set_src(button->icon, icon);
        return;
    }
    lv_label_set_text(button->icon, glyph);
}

static void music_button_create(lv_obj_t *row, music_button_id_t id, lv_event_cb_t callback,
                                void *user_data)
{
    music_button_t *button = &s_music.buttons[id];
    const bool play_button = id == MUSIC_BUTTON_PLAY;
    const int32_t size = play_button ? MUSIC_PLAY_BUTTON_SIZE : MUSIC_BUTTON_SIZE;

    button->pressed = s_music.use_icon_assets ? s_button_pressed_icons[id] : NULL;

    button->button = lv_button_create(row);
    lv_obj_set_size(button->button, size, size);
    lv_obj_set_style_bg_opa(button->button, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(button->button, 0, 0);
    lv_obj_set_style_shadow_width(button->button, 0, 0);
    lv_obj_set_style_radius(button->button, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_pad_all(button->button, 0, 0);
    lv_obj_clear_flag(button->button, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(button->button, callback, LV_EVENT_CLICKED, user_data);

    if (button->pressed != NULL) {
        lv_obj_add_event_cb(button->button, music_button_feedback_cb, LV_EVENT_PRESSED, button);
        lv_obj_add_event_cb(button->button, music_button_feedback_cb, LV_EVENT_RELEASED, button);
        lv_obj_add_event_cb(button->button, music_button_feedback_cb, LV_EVENT_PRESS_LOST, button);
    }

    if (play_button && s_music.use_icon_assets) {
        lv_obj_t *base = lv_image_create(button->button);
        lv_image_set_src(base, MUSIC_ICON_DIR "icon_c.png");
        lv_obj_clear_flag(base, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_center(base);
    }

    if (s_music.use_icon_assets) {
        button->icon = lv_image_create(button->button);
        lv_obj_set_size(button->icon, MUSIC_ICON_SIZE, MUSIC_ICON_SIZE);
        button->released = s_button_icons[id];
        lv_image_set_src(button->icon, button->released);
    } else {
        button->icon = lv_label_create(button->button);
        lv_label_set_text(button->icon, s_button_glyphs[id]);
        lv_obj_set_style_text_font(button->icon, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(button->icon, C_INK, 0);
    }
    lv_obj_clear_flag(button->icon, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_center(button->icon);
}

static const lv_font_t *music_text_font(const char *text)
{
    if (text != NULL) {
        for (const unsigned char *cursor = (const unsigned char *)text; *cursor != '\0'; ++cursor) {
            if (*cursor >= 0x80U) {
                return desktop_font_get_primary();
            }
        }
    }
    return &lv_font_montserrat_14;
}

static void music_set_track_text(lv_obj_t *label, const char *text)
{
    if (label == NULL) {
        return;
    }
    lv_obj_set_style_text_font(label, music_text_font(text), 0);
    lv_label_set_text(label, text);
}

static void music_set_lyric_text(lv_obj_t *label, const char *text)
{
    if (label == NULL) {
        return;
    }
#if CONFIG_ESP_BOARD_JC8048F550
    lv_obj_set_style_text_font(label, desktop_font_get_artist(), 0);
#else
    lv_obj_set_style_text_font(label, desktop_font_get_lyric(), 0);
#endif
    lv_label_set_text(label, text);
}

static void music_set_artist_text(lv_obj_t *label, const char *text)
{
    if (label == NULL) {
        return;
    }
#if CONFIG_ESP_BOARD_JC8048F550
    lv_obj_set_style_text_font(label, desktop_font_get_title(), 0);
#else
    lv_obj_set_style_text_font(label, desktop_font_get_artist(), 0);
#endif
    lv_label_set_text(label, text);
}

static uint32_t bounded_track_index(uint32_t track_index)
{
    return track_index < MUSIC_TRACK_COUNT ? track_index : 0U;
}

static const char *music_snapshot_text(const char *snapshot_text, desktop_text_key_t fallback)
{
    return snapshot_text != NULL && snapshot_text[0] != '\0' ? snapshot_text : L(fallback);
}

static bool music_jpeg_hw_fast_path_eligible(uint32_t width, uint32_t height)
{
    return width >= MUSIC_COVER_ART_HW_JPEG_MIN_DIMENSION &&
           height >= MUSIC_COVER_ART_HW_JPEG_MIN_DIMENSION &&
           (width % MUSIC_COVER_ART_HW_JPEG_ALIGNMENT) == 0U &&
           (height % MUSIC_COVER_ART_HW_JPEG_ALIGNMENT) == 0U;
}

/**
 * @brief 计算按 JPEG 块对齐后的 RGB565 缓冲区大小。
 * @param width 图像宽度，单位为像素。
 * @param height 图像高度，单位为像素。
 * @param[out] decoded_bytes 成功时接收所需缓冲区字节数。
 * @return 尺寸有效且计算结果未超过内存上限时返回 true，否则返回 false。
 */
static bool music_get_padded_decoded_bytes(uint32_t width, uint32_t height, size_t *decoded_bytes)
{
    if (width == 0U || height == 0U || decoded_bytes == NULL) {
        return false;
    }

    const bool hardware_fast_path = music_jpeg_hw_fast_path_eligible(width, height);
    const uint64_t padded_width = hardware_fast_path
                                      ? width
                                      : (((uint64_t)width + MUSIC_COVER_ART_HW_JPEG_ALIGNMENT - 1U) /
                                         MUSIC_COVER_ART_HW_JPEG_ALIGNMENT) *
                                            MUSIC_COVER_ART_HW_JPEG_ALIGNMENT;
    const uint64_t padded_height = hardware_fast_path
                                       ? height
                                       : (((uint64_t)height + MUSIC_COVER_ART_HW_JPEG_ALIGNMENT - 1U) /
                                          MUSIC_COVER_ART_HW_JPEG_ALIGNMENT) *
                                             MUSIC_COVER_ART_HW_JPEG_ALIGNMENT;
    if (padded_width > UINT64_MAX / padded_height) {
        return false;
    }
    const uint64_t padded_pixels = padded_width * padded_height;
    if (padded_pixels > UINT64_MAX / MUSIC_COVER_ART_DECODED_BYTES_PER_PIXEL) {
        return false;
    }
    const uint64_t bytes = padded_pixels * MUSIC_COVER_ART_DECODED_BYTES_PER_PIXEL;
    if (bytes > MUSIC_COVER_ART_DECODED_BYTES_MAX || bytes > SIZE_MAX || bytes > UINT32_MAX) {
        return false;
    }

    *decoded_bytes = (size_t)bytes;
    return true;
}

/**
 * @brief 校验 JPEG 尺寸与解码内存上限。
 * @param data JPEG 数据。
 * @param size 数据长度，单位为字节。
 * @param[out] width 成功时接收图像宽度，单位为像素。
 * @param[out] height 成功时接收图像高度，单位为像素。
 * @return 图像信息有效且满足尺寸和内存限制时返回 true。
 */
static bool music_get_safe_jpeg_dimensions(const uint8_t *data, size_t size, uint16_t *width,
                                           uint16_t *height)
{
    jpeg_decode_picture_info_t picture_info = {0};
    size_t padded_decoded_bytes = 0U;

    if (data == NULL || width == NULL || height == NULL || size == 0U || size > UINT32_MAX ||
        jpeg_decoder_get_info(data, (uint32_t)size, &picture_info) != ESP_OK) {
        return false;
    }
    if (picture_info.width == 0U || picture_info.height == 0U ||
        picture_info.width > MUSIC_COVER_ART_MAX_DIMENSION ||
        picture_info.height > MUSIC_COVER_ART_MAX_DIMENSION ||
        picture_info.width > UINT16_MAX || picture_info.height > UINT16_MAX ||
        !music_get_padded_decoded_bytes(picture_info.width, picture_info.height,
                                        &padded_decoded_bytes)) {
        return false;
    }

    *width = (uint16_t)picture_info.width;
    *height = (uint16_t)picture_info.height;
    return true;
}

/**
 * @brief 校验 PNG 尺寸与 ARGB8888 解码内存上限。
 * @param data PNG 数据。
 * @param size 数据长度，单位为字节。
 * @param[out] width 成功时接收图像宽度，单位为像素。
 * @param[out] height 成功时接收图像高度，单位为像素。
 * @return 图像信息有效且满足尺寸和内存限制时返回 true。
 */
static bool music_get_safe_png_dimensions(const uint8_t *data, size_t size, uint16_t *width,
                                          uint16_t *height)
{
    static const uint8_t signature[8] = {0x89U, 'P', 'N', 'G', 0x0DU, 0x0AU, 0x1AU, 0x0AU};

    if (data == NULL || width == NULL || height == NULL || size < 24U ||
        memcmp(data, signature, sizeof(signature)) != 0) {
        return false;
    }
    const uint32_t png_width = ((uint32_t)data[16] << 24) | ((uint32_t)data[17] << 16) |
                               ((uint32_t)data[18] << 8) | (uint32_t)data[19];
    const uint32_t png_height = ((uint32_t)data[20] << 24) | ((uint32_t)data[21] << 16) |
                                ((uint32_t)data[22] << 8) | (uint32_t)data[23];
    if (png_width == 0U || png_height == 0U ||
        png_width > MUSIC_COVER_ART_MAX_DIMENSION || png_height > MUSIC_COVER_ART_MAX_DIMENSION ||
        png_width > UINT16_MAX || png_height > UINT16_MAX) {
        return false;
    }
    const uint64_t pixels = (uint64_t)png_width * (uint64_t)png_height;
    if (pixels > MUSIC_COVER_ART_DECODED_BYTES_MAX / MUSIC_COVER_ART_PNG_BYTES_PER_PIXEL) {
        return false;
    }

    *width = (uint16_t)png_width;
    *height = (uint16_t)png_height;
    return true;
}

static void music_set_album_fallback(void)
{
    if (s_music.album_image == NULL) {
        return;
    }
    s_music.cover_art_active = false;
    lv_image_cache_drop(&s_cover_art_image);
    memset(&s_cover_art_image, 0, sizeof(s_cover_art_image));
    lv_obj_set_size(s_music.album_image, MUSIC_ALBUM_SIZE, MUSIC_ALBUM_SIZE);
    lv_image_set_inner_align(s_music.album_image, LV_IMAGE_ALIGN_CONTAIN);
    lv_image_set_src(s_music.album_image, &img_album);
    lv_obj_center(s_music.album_image);
}

/**
 * @brief 更新曲目封面，无可用封面时使用默认图像。
 * @param snapshot 播放器状态快照，包含封面信息。
 */
static void music_update_cover_art(const music_player_snapshot_t *snapshot)
{
    if (snapshot == NULL) {
        music_set_album_fallback();
        return;
    }

    const bool jpeg = snapshot->cover_art.format_fourcc == ESP_BT_AUDIO_FOURCC_JPEG;
    const bool png = snapshot->cover_art.format_fourcc == ESP_BT_AUDIO_FOURCC_PNG;
    if (!snapshot->initialized || !snapshot->open ||
        snapshot->source != MUSIC_PLAYER_SOURCE_CLASSIC_BT ||
        (!jpeg && !png) || !snapshot->cover_art.available) {
        if (s_music.cover_art_active) {
            ESP_LOGW(TAG, "cover art released: initialized=%d open=%d source=%d "
                          "format=0x%08x available=%d",
                     (int)snapshot->initialized, (int)snapshot->open, (int)snapshot->source,
                     (unsigned)snapshot->cover_art.format_fourcc,
                     (int)snapshot->cover_art.available);
            s_music.cover_art_seen_revision = 0U;
            music_set_album_fallback();
        }
        return;
    }
    if (snapshot->cover_art.size == 0U ||
        snapshot->cover_art.size > MUSIC_PLAYER_COVER_ART_MAX_SIZE ||
        snapshot->cover_art.revision == 0U ||
        snapshot->cover_art.revision == s_music.cover_art_seen_revision) {
        return;
    }

    uint8_t *const art = cover_art_buffer();
    if (art == NULL) {
        ESP_LOGW(TAG, "cover art buffer unavailable; keeping the album art");
        return;
    }
    const size_t copied = music_player_cover_art_copy(snapshot->cover_art.revision, art,
                                                      snapshot->cover_art.size, 0U);
    if (copied != snapshot->cover_art.size) {
        return;
    }

    uint16_t width = 0U;
    uint16_t height = 0U;
    const bool sized = jpeg ? music_get_safe_jpeg_dimensions(art, copied, &width, &height)
                            : music_get_safe_png_dimensions(art, copied, &width, &height);
    if (!sized) {
        ESP_LOGW(TAG, "cover art rejected: revision=%u size=%u format=0x%08x",
                 (unsigned)snapshot->cover_art.revision, (unsigned)snapshot->cover_art.size,
                 (unsigned)snapshot->cover_art.format_fourcc);
        s_music.cover_art_seen_revision = snapshot->cover_art.revision;
        music_set_album_fallback();
        return;
    }

    lv_image_cache_drop(&s_cover_art_image);
    s_cover_art_image = (lv_image_dsc_t){
        .header.magic = LV_IMAGE_HEADER_MAGIC,
        .data_size = (uint32_t)copied,
        .data = art,
    };
    s_music.cover_art_seen_revision = snapshot->cover_art.revision;
    s_music.cover_art_active = true;
    ESP_LOGI(TAG, "cover art shown: revision=%u size=%u %ux%u",
             (unsigned)snapshot->cover_art.revision, (unsigned)copied,
             (unsigned)width, (unsigned)height);
    lv_obj_set_size(s_music.album_image, MUSIC_ALBUM_SIZE, MUSIC_ALBUM_SIZE);
    lv_image_set_inner_align(s_music.album_image, LV_IMAGE_ALIGN_CONTAIN);
    lv_image_set_src(s_music.album_image, &s_cover_art_image);
    lv_obj_center(s_music.album_image);
}

static void music_format_time(char *buffer, size_t buffer_size, uint32_t elapsed_ms,
                              uint32_t duration_ms, bool position_known)
{
    const uint32_t elapsed_seconds = elapsed_ms / 1000U;
    const uint32_t duration_seconds = duration_ms / 1000U;
    const char *elapsed_text = position_known ? NULL : "--:--";
    const char *duration_text = duration_ms == 0U ? "--:--" : NULL;
    if (elapsed_text == NULL && duration_text == NULL) {
        (void)snprintf(buffer, buffer_size, "%02u:%02u / %02u:%02u",
                       (unsigned)(elapsed_seconds / 60U), (unsigned)(elapsed_seconds % 60U),
                       (unsigned)(duration_seconds / 60U), (unsigned)(duration_seconds % 60U));
    } else if (elapsed_text == NULL) {
        (void)snprintf(buffer, buffer_size, "%02u:%02u / %s",
                       (unsigned)(elapsed_seconds / 60U), (unsigned)(elapsed_seconds % 60U),
                       duration_text);
    } else if (duration_text == NULL) {
        (void)snprintf(buffer, buffer_size, "%s / %02u:%02u", elapsed_text,
                       (unsigned)(duration_seconds / 60U), (unsigned)(duration_seconds % 60U));
    } else {
        (void)snprintf(buffer, buffer_size, "%s / %s", elapsed_text, duration_text);
    }
}

static void music_set_progress(uint32_t elapsed_ms, uint32_t duration_ms, bool position_known)
{
    if (s_music.progress == NULL) {
        return;
    }
    uint64_t value = 0U;
    if (position_known && duration_ms != 0U) {
        value = ((uint64_t)elapsed_ms * 100ULL) / duration_ms;
    }
    lv_bar_set_value(s_music.progress, (int32_t)(value > 100U ? 100U : value), LV_ANIM_OFF);
}

static void music_sync_transport_icons(void)
{
    if (!s_music.active) {
        return;
    }

    music_button_set_icon(&s_music.buttons[MUSIC_BUTTON_PLAY],
                          s_music.playing ? MUSIC_ICON_DIR "icon_pause.png"
                                          : MUSIC_ICON_DIR "icon_play.png",
                          s_music.playing ? LV_SYMBOL_PAUSE : LV_SYMBOL_PLAY);

    music_button_t *source = &s_music.buttons[MUSIC_BUTTON_SOURCE];
    if (source->button == NULL) {
        return;
    }
    const bool bluetooth = s_music.selected_source == MUSIC_PLAYER_SOURCE_CLASSIC_BT;
    if (s_music.use_icon_assets) {
        music_button_set_icon(source,
                              bluetooth ? MUSIC_ICON_DIR "icon_bt_p.png"
                                        : MUSIC_ICON_DIR "icon_bt.png",
                              LV_SYMBOL_BLUETOOTH);
        return;
    }

    lv_obj_set_style_bg_color(source->button, C_DISC, 0);
    lv_obj_set_style_bg_opa(source->button, bluetooth ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
    lv_obj_invalidate(source->button);
}

static void music_update_labels(void)
{
    if (!s_music.active) {
        return;
    }

    const uint32_t track = bounded_track_index(s_music.track_index);
    const char *title = s_music.snapshot_valid
                            ? music_snapshot_text(s_music.snapshot.track_title, s_track_keys[track])
                            : L(s_track_keys[track]);
    const char *artist = s_music.snapshot_valid
                             ? music_snapshot_text(s_music.snapshot.artist, s_artist_keys[track])
                             : L(s_artist_keys[track]);
    const char *album = s_music.snapshot_valid ? s_music.snapshot.album : "";
    const uint32_t elapsed_ms = s_music.snapshot_valid ? s_music.snapshot.elapsed_ms : 0U;
    const uint32_t duration_ms = s_music.snapshot_valid ? s_music.snapshot.duration_ms : 0U;
    char time_text[32];
    const bool position_known = s_music.snapshot_valid && s_music.snapshot.position_known;
    music_format_time(time_text, sizeof(time_text), elapsed_ms, duration_ms, position_known);

    music_set_lyric_text(s_music.title, title);
    music_set_artist_text(s_music.artist, artist);
    music_set_track_text(s_music.album, album);
    music_set_track_text(s_music.time, time_text);
    music_sync_transport_icons();
    music_set_progress(elapsed_ms, duration_ms, position_known);
}

static void music_source_switch_cb(lv_event_t *event)
{
    (void)event;
    if (!s_music.active) {
        return;
    }

    const music_player_source_t next_source =
        s_music.selected_source == MUSIC_PLAYER_SOURCE_SD_CARD
            ? MUSIC_PLAYER_SOURCE_CLASSIC_BT
            : MUSIC_PLAYER_SOURCE_SD_CARD;

    if (music_player_service_is_playing()) {
        (void)music_player_service_pause();
        s_music.playing = false;
        music_update_labels();
    }
    const esp_err_t result = music_player_service_select_source(next_source);
    if (result != ESP_OK) {
        music_update_labels();
        if (s_music.toast_cb != NULL) {
            s_music.toast_cb(L(DESKTOP_TEXT_MUSIC_UNAVAILABLE));
        }
        return;
    }

    s_music.selected_source = next_source;
    music_update_labels();
}

static void music_arm_rotation_cb(void *object, int32_t angle)
{
    if (!s_music.active || object != s_music.tonearm) {
        return;
    }
    s_music.animation.arm_angle = angle;
    lv_image_set_rotation(object, angle);
}

static void music_disc_rotation_cb(void *object, int32_t angle)
{
    if (!s_music.active || object != s_music.disc) {
        return;
    }
    lv_image_set_rotation(object, angle);
}

static void music_stop_disc(void)
{
    if (s_music.disc != NULL) {
        lv_anim_delete(s_music.disc, NULL);
    }
}

static void music_stop_arm(void)
{
    if (s_music.tonearm != NULL) {
        lv_anim_delete(s_music.tonearm, NULL);
    }
}

static void music_stop_all_animations(void)
{
    music_stop_disc();
    music_stop_arm();
}

static void music_start_disc(void)
{
    if (!s_music.active || s_music.disc == NULL) {
        return;
    }

    lv_anim_t animation;
    lv_anim_init(&animation);
    lv_anim_set_var(&animation, s_music.disc);
    lv_anim_set_exec_cb(&animation, music_disc_rotation_cb);
    lv_anim_set_values(&animation, APP_MUSIC_DISC_START_ANGLE,
                       APP_MUSIC_DISC_END_ANGLE);
    lv_anim_set_duration(&animation, MUSIC_DISC_TURN_MS);
    lv_anim_set_repeat_count(&animation, LV_ANIM_REPEAT_INFINITE);
    lv_anim_start(&animation);
}

static void music_arm_completed_cb(lv_anim_t *animation)
{
    const uint32_t callback_generation = (uint32_t)(uintptr_t)lv_anim_get_user_data(animation);
    if (!s_music.active || callback_generation != s_music.arm_generation) {
        return;
    }

    if (s_music.animation.state == APP_MUSIC_ANIM_ARMING &&
        app_music_anim_reduce(&s_music.animation, APP_MUSIC_ANIM_EVENT_ARM_ENGAGED) ==
            APP_MUSIC_ANIM_EFFECT_START_DISC) {
        music_start_disc();
    } else if (s_music.animation.state == APP_MUSIC_ANIM_RETURNING) {
        (void)app_music_anim_reduce(&s_music.animation, APP_MUSIC_ANIM_EVENT_ARM_RESTED);
    }
}

static void music_start_arm(int32_t start, int32_t end)
{
    if (!s_music.active || s_music.tonearm == NULL) {
        return;
    }

    s_music.arm_generation = ++s_music.generation;
    lv_anim_t animation;
    lv_anim_init(&animation);
    lv_anim_set_var(&animation, s_music.tonearm);
    lv_anim_set_exec_cb(&animation, music_arm_rotation_cb);
    lv_anim_set_values(&animation, start, end);
    lv_anim_set_duration(&animation, MUSIC_ARM_ANIM_MS);
    lv_anim_set_user_data(&animation, (void *)(uintptr_t)s_music.arm_generation);
    lv_anim_set_completed_cb(&animation, music_arm_completed_cb);
    lv_anim_start(&animation);
}

/**
 * @brief 执行动画状态机返回的动作。
 * @param event 要交给动画状态机处理的播放事件。
 */
static void music_dispatch(app_music_anim_event_t event)
{
    const app_music_anim_effect_t effect = app_music_anim_reduce(&s_music.animation, event);
    switch (effect) {
        case APP_MUSIC_ANIM_EFFECT_ENGAGE_ARM:
            music_stop_all_animations();
            music_start_arm(s_music.animation.arm_angle, APP_MUSIC_ARM_PLAY_ANGLE);
            break;
        case APP_MUSIC_ANIM_EFFECT_START_DISC:
            music_start_disc();
            break;
        case APP_MUSIC_ANIM_EFFECT_RETURN_ARM:

            music_stop_disc();
            music_stop_arm();
            music_start_arm(s_music.animation.arm_angle, APP_MUSIC_ARM_IDLE_ANGLE);
            break;
        case APP_MUSIC_ANIM_EFFECT_STOP_ALL:
            ++s_music.generation;
            music_stop_all_animations();
            break;
        case APP_MUSIC_ANIM_EFFECT_NONE:
            break;
    }
}

/**
 * @brief 将播放器快照同步到界面与动画状态。
 * @param snapshot 要同步到界面的播放器状态快照。
 */
static void music_apply_snapshot(const music_player_snapshot_t *snapshot)
{
    if (snapshot == NULL || !snapshot->initialized || !snapshot->open) {
        return;
    }

    const bool playing = snapshot->playback == MUSIC_PLAYER_PLAYBACK_PLAYING;
    const bool ended = snapshot->playback == MUSIC_PLAYER_PLAYBACK_ENDED;
    const bool failed = snapshot->playback == MUSIC_PLAYER_PLAYBACK_ERROR &&
                        (!s_music.snapshot_valid ||
                         s_music.snapshot.playback != MUSIC_PLAYER_PLAYBACK_ERROR);
    if (snapshot->source != s_music.selected_source) {
        s_music.selected_source = snapshot->source;
        music_dispatch(APP_MUSIC_ANIM_EVENT_SOURCE_SWITCH);
    }
    if (ended) {
        s_music.playing = false;
        music_dispatch(APP_MUSIC_ANIM_EVENT_END);
    } else if (playing != s_music.playing) {
        s_music.playing = playing;
        music_dispatch(playing ? APP_MUSIC_ANIM_EVENT_PLAY : APP_MUSIC_ANIM_EVENT_PAUSE);
    }
    s_music.track_index = snapshot->track_index;
    s_music.snapshot_valid = true;
    s_music.snapshot = *snapshot;
    music_update_labels();

    if (failed && s_music.toast_cb != NULL) {
        s_music.toast_cb(L(DESKTOP_TEXT_MUSIC_ERROR));
    }
}

static void music_volume_label_update(void)
{
    if (s_music.volume_value == NULL || s_music.volume_slider == NULL) {
        return;
    }
    char text[8];
    snprintf(text, sizeof(text), "%d%%", (int)lv_slider_get_value(s_music.volume_slider));
    lv_label_set_text(s_music.volume_value, text);
}

static void music_volume_hud_refresh(void)
{
    uint8_t level = 0U;
    bool muted = false;
    if (s_music.volume_slider == NULL ||
        music_player_service_get_volume(&level, &muted) != ESP_OK) {
        return;
    }
    lv_slider_set_value(s_music.volume_slider, (int32_t)level, LV_ANIM_OFF);
    music_volume_label_update();
}

static void music_volume_hud_arm_hide(void)
{
    if (s_music.volume_hide_timer == NULL) {
        return;
    }

    lv_timer_set_repeat_count(s_music.volume_hide_timer, 1);
    lv_timer_resume(s_music.volume_hide_timer);
    lv_timer_reset(s_music.volume_hide_timer);
}

static void music_volume_hud_show(void)
{
    if (!s_music.active || s_music.volume_hud == NULL) {
        return;
    }
    music_volume_hud_refresh();
    lv_obj_remove_flag(s_music.volume_hud, LV_OBJ_FLAG_HIDDEN);
    music_volume_hud_arm_hide();
}

static void music_volume_hud_hide_cb(lv_timer_t *timer)
{
    (void)timer;
    if (s_music.volume_hud == NULL) {
        return;
    }
    lv_obj_add_flag(s_music.volume_hud, LV_OBJ_FLAG_HIDDEN);
}

static void music_volume_slider_cb(lv_event_t *event)
{
    if (!s_music.active) {
        return;
    }
    lv_obj_t *slider = lv_event_get_target(event);
    if (slider == NULL) {
        return;
    }

    if (music_player_service_set_volume((uint8_t)lv_slider_get_value(slider)) != ESP_OK) {
        music_volume_hud_refresh();
        return;
    }
    music_volume_label_update();
    music_volume_hud_arm_hide();
}

/**
 * @brief 在 LVGL 定时器中读取播放器状态并刷新页面。
 * @param timer 触发回调的 LVGL 定时器。
 */
static void music_poll_cb(lv_timer_t *timer)
{
    (void)timer;
    if (!s_music.active) {
        return;
    }

    if (s_music.volume_hud != NULL &&
        !lv_obj_has_flag(s_music.volume_hud, LV_OBJ_FLAG_HIDDEN)) {
        music_volume_hud_refresh();
    }

    music_player_snapshot_t snapshot = {0};
    music_player_snapshot(&snapshot);
    music_update_cover_art(&snapshot);
    music_apply_snapshot(&snapshot);
    if (!s_music.snapshot_valid) {
        const bool playing = music_player_service_is_playing();
        if (playing != s_music.playing) {
            s_music.playing = playing;
            music_dispatch(playing ? APP_MUSIC_ANIM_EVENT_PLAY : APP_MUSIC_ANIM_EVENT_PAUSE);
            music_update_labels();
        }
    }
}

static void music_action_cb(lv_event_t *event)
{
    const music_action_t action = (music_action_t)(uintptr_t)lv_event_get_user_data(event);
    esp_err_t result;

    if (action == MUSIC_ACTION_VOLUME) {
        music_volume_hud_show();
        return;
    }

    if (action == MUSIC_ACTION_PLAY) {
        result = s_music.playing ? music_player_service_pause() : music_player_service_play();
    } else if (action == MUSIC_ACTION_NEXT) {
        result = music_player_service_next();
    } else {
        result = music_player_service_previous();
    }

    if (result != ESP_OK) {
        music_update_labels();
        if (s_music.toast_cb != NULL) {
            s_music.toast_cb(L(DESKTOP_TEXT_MUSIC_UNAVAILABLE));
        }
        return;
    }
    if (action == MUSIC_ACTION_PLAY) {
        const bool playing = music_player_service_is_playing();
        if (playing != s_music.playing) {
            s_music.playing = playing;
            music_dispatch(playing ? APP_MUSIC_ANIM_EVENT_PLAY
                                   : APP_MUSIC_ANIM_EVENT_PAUSE);
        }
    }
    music_update_labels();
}

void app_music_view_cleanup(void)
{
    if (s_music.poll_timer != NULL) {
        lv_timer_delete(s_music.poll_timer);
        s_music.poll_timer = NULL;
    }
    if (s_music.volume_hide_timer != NULL) {
        lv_timer_delete(s_music.volume_hide_timer);
        s_music.volume_hide_timer = NULL;
    }
    s_music.active = false;
    ++s_music.generation;
    music_dispatch(APP_MUSIC_ANIM_EVENT_CLOSE);
    if (s_music.disc != NULL) {
        lv_image_set_rotation(s_music.disc, APP_MUSIC_DISC_START_ANGLE);
    }
    if (s_music.tonearm != NULL) {
        lv_image_set_rotation(s_music.tonearm, APP_MUSIC_ARM_IDLE_ANGLE);
    }

    music_set_album_fallback();
    memset(&s_music, 0, sizeof(s_music));
    app_music_anim_reset(&s_music.animation);

    heap_caps_free(s_cover_art);
    s_cover_art = NULL;
}

static void music_volume_hud_create(lv_obj_t *root, lv_obj_t *anchor)
{
    s_music.volume_hud = lv_obj_create(root);
    if (s_music.volume_hud == NULL) {
        return;
    }
    lv_obj_add_flag(s_music.volume_hud, LV_OBJ_FLAG_IGNORE_LAYOUT);
    lv_obj_add_flag(s_music.volume_hud, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_size(s_music.volume_hud, MUSIC_VOLUME_HUD_WIDTH, MUSIC_VOLUME_HUD_HEIGHT);
    if (anchor != NULL) {
        lv_obj_align_to(s_music.volume_hud, anchor, LV_ALIGN_OUT_TOP_MID, 0,
                        -MUSIC_VOLUME_HUD_GAP);
    }
    lv_obj_set_style_bg_color(s_music.volume_hud, C_SURFACE, 0);
    lv_obj_set_style_bg_opa(s_music.volume_hud, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_music.volume_hud, 1, 0);
    lv_obj_set_style_border_color(s_music.volume_hud, C_DIVIDER, 0);
    lv_obj_set_style_radius(s_music.volume_hud, 14, 0);
    lv_obj_set_style_pad_all(s_music.volume_hud, 16, 0);
    lv_obj_set_style_pad_column(s_music.volume_hud, 12, 0);
    lv_obj_clear_flag(s_music.volume_hud, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(s_music.volume_hud, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(s_music.volume_hud, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);

    (void)music_label(s_music.volume_hud, LV_SYMBOL_AUDIO, C_INK, 0, 0);

    s_music.volume_slider = lv_slider_create(s_music.volume_hud);
    lv_obj_set_height(s_music.volume_slider, 8);
    lv_obj_set_flex_grow(s_music.volume_slider, 1);
    lv_slider_set_range(s_music.volume_slider, MUSIC_VOLUME_LEVEL_MIN, MUSIC_VOLUME_LEVEL_MAX);
    lv_obj_set_style_bg_color(s_music.volume_slider, C_MUTED, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_music.volume_slider, C_INK, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(s_music.volume_slider, C_INK, LV_PART_KNOB);
    lv_obj_set_style_radius(s_music.volume_slider, LV_RADIUS_CIRCLE, LV_PART_KNOB);
    lv_obj_add_event_cb(s_music.volume_slider, music_volume_slider_cb,
                        LV_EVENT_VALUE_CHANGED, NULL);

    s_music.volume_value = music_label(s_music.volume_hud, "", C_INK, 0, 0);
    lv_obj_set_width(s_music.volume_value, MUSIC_VOLUME_HUD_VALUE_WIDTH);
    lv_obj_set_style_text_align(s_music.volume_value, LV_TEXT_ALIGN_RIGHT, 0);
    music_volume_hud_refresh();

    s_music.volume_hide_timer = lv_timer_create(music_volume_hud_hide_cb,
                                                MUSIC_VOLUME_HUD_HIDE_MS, NULL);
    if (s_music.volume_hide_timer != NULL) {
        lv_timer_set_auto_delete(s_music.volume_hide_timer, false);
        lv_timer_set_repeat_count(s_music.volume_hide_timer, 0);
        lv_timer_pause(s_music.volume_hide_timer);
    }
}

/**
 * @brief 加载页面背景并持有其解码数据。
 * @param root 背景图像的父容器。
 */
static void music_background_create(lv_obj_t *root)
{
    if (root == NULL || !music_file_readable(MUSIC_BACKGROUND_PROBE_POSIX)) {
        return;
    }

    lv_obj_t *image = lv_image_create(root);
    lv_obj_remove_style_all(image);
    lv_obj_add_flag(image, LV_OBJ_FLAG_IGNORE_LAYOUT);
    lv_obj_clear_flag(image, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(image, lv_obj_get_width(root), lv_obj_get_height(root));
    lv_obj_set_pos(image, -lv_obj_get_style_pad_left(root, LV_PART_MAIN),
                   -lv_obj_get_style_pad_top(root, LV_PART_MAIN));
    lv_image_set_inner_align(image, LV_IMAGE_ALIGN_COVER);
    lv_obj_move_to_index(image, 0);

    const size_t backdrop_bytes = (size_t)lv_obj_get_width(root) *
                                  (size_t)lv_obj_get_height(root) *
                                  lv_color_format_get_size(LV_COLOR_FORMAT_RGB565);
    if (!desktop_big_buffer_reserve(backdrop_bytes)) {
        ESP_LOGW(TAG, "music backdrop has no PSRAM block; drawing on the canvas");
        lv_obj_delete(image);
        return;
    }
    lv_image_set_src(image, MUSIC_BACKGROUND_PATH);
    lv_image_decoder_dsc_t decoder_dsc;
    if (lv_image_decoder_open(&decoder_dsc, MUSIC_BACKGROUND_PATH, NULL) != LV_RESULT_OK) {
        ESP_LOGW(TAG, "music backdrop could not be decoded: %s", MUSIC_BACKGROUND_PATH);
        lv_obj_delete(image);
        return;
    }
    lv_image_decoder_close(&decoder_dsc);
    s_music.background = image;
}

static void music_control_row_create(lv_obj_t *root)
{
    s_music.controls = lv_obj_create(root);
    lv_obj_set_size(s_music.controls, MUSIC_CONTROLS_WIDTH, MUSIC_CONTROLS_HEIGHT);
    lv_obj_set_style_bg_opa(s_music.controls, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_music.controls, 0, 0);
    lv_obj_set_style_pad_all(s_music.controls, 0, 0);
    lv_obj_set_style_pad_column(s_music.controls, MUSIC_BUTTON_GAP, 0);
    lv_obj_clear_flag(s_music.controls, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_music.controls, LV_OBJ_FLAG_IGNORE_LAYOUT);
    lv_obj_set_flex_flow(s_music.controls, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(s_music.controls, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);

    music_button_create(s_music.controls, MUSIC_BUTTON_VOLUME, music_action_cb,
                        (void *)(uintptr_t)MUSIC_ACTION_VOLUME);
    music_button_create(s_music.controls, MUSIC_BUTTON_PREVIOUS, music_action_cb,
                        (void *)(uintptr_t)MUSIC_ACTION_PREVIOUS);
    music_button_create(s_music.controls, MUSIC_BUTTON_PLAY, music_action_cb,
                        (void *)(uintptr_t)MUSIC_ACTION_PLAY);
    music_button_create(s_music.controls, MUSIC_BUTTON_NEXT, music_action_cb,
                        (void *)(uintptr_t)MUSIC_ACTION_NEXT);
    music_button_create(s_music.controls, MUSIC_BUTTON_SOURCE, music_source_switch_cb, NULL);
}

static void music_control_row_align(lv_obj_t *rail)
{
    if (s_music.controls == NULL || s_music.disc == NULL || rail == NULL) {
        return;
    }

    lv_obj_update_layout(s_music.controls);
    lv_obj_update_layout(s_music.disc);
    lv_obj_update_layout(rail);

    lv_obj_align_to(s_music.controls, s_music.disc, LV_ALIGN_OUT_BOTTOM_MID, 0,
                    -(MUSIC_CONTROLS_HEIGHT / 2));

    lv_obj_update_layout(s_music.controls);
    const int32_t centre_y = lv_obj_get_y(s_music.controls);
    lv_obj_align_to(s_music.controls, rail, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_y(s_music.controls, centre_y);
    lv_obj_update_layout(s_music.controls);
}

static void music_progress_block_create(lv_obj_t *rail, lv_obj_t *anchor)
{
    lv_obj_t *block = lv_obj_create(rail);
    s_music.progress_block = block;
    lv_obj_set_width(block, LV_PCT(100));
    lv_obj_set_height(block, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(block, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(block, 0, 0);
    lv_obj_set_style_pad_all(block, 0, 0);
    lv_obj_set_style_pad_row(block, 10, 0);
    lv_obj_clear_flag(block, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(block, LV_OBJ_FLAG_IGNORE_LAYOUT);
    lv_obj_set_flex_flow(block, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(block, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START);

    s_music.time = music_label(block, "", C_INK, 0, 0);
    lv_obj_set_width(s_music.time, LV_PCT(100));

    s_music.progress = lv_bar_create(block);
    lv_obj_set_size(s_music.progress, LV_PCT(100), 8);
    lv_bar_set_range(s_music.progress, 0, 100);

    lv_obj_set_style_bg_color(s_music.progress, C_INK, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_music.progress, MUSIC_PROGRESS_TRACK_OPA, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_music.progress, C_INK, LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(s_music.progress, LV_OPA_COVER, LV_PART_INDICATOR);

    if (anchor == NULL) {
        return;
    }

    lv_obj_align_to(block, anchor, LV_ALIGN_OUT_TOP_LEFT, 0, -MUSIC_PROGRESS_GAP);
    lv_obj_set_x(block, 0);
    lv_obj_update_layout(block);
}

void app_music_view_build(lv_obj_t *parent, app_music_view_toast_cb_t toast_cb)
{
    if (parent == NULL) {
        return;
    }

    app_music_view_cleanup();
    s_music.active = true;
    s_music.toast_cb = toast_cb;
    s_music.selected_source = music_player_service_get_source();
    s_music.playing = music_player_service_is_playing();
    app_music_anim_reset(&s_music.animation);

    s_music.use_icon_assets =
        assets_storage_icons_ready() && music_file_readable(MUSIC_ICON_PROBE_POSIX);

    const desktop_fullscreen_layout_colors_t colors = {
        .canvas = C_CANVAS,

        .surface = C_CANVAS,
        .ink = C_INK,
        .ink_muted = C_INK_MUTED,
    };
    desktop_fullscreen_layout_t layout;
    if (!desktop_fullscreen_layout_create(parent, &colors, "", "", "", &layout)) {
        return;
    }

    desktop_fullscreen_layout_remove_page_header(&layout);
    music_background_create(layout.root);

    music_panel_make_transparent(layout.content);

    lv_obj_t *left = desktop_fullscreen_layout_create_panel(
        layout.content, C_CANVAS, MUSIC_LEFT_PERCENT, 0, LV_FLEX_FLOW_COLUMN);
    music_panel_make_transparent(left);
    lv_obj_set_style_pad_all(left, MUSIC_LEFT_PAD, 0);
    lv_obj_set_style_pad_row(left, 10, 0);

    lv_obj_t *meta = desktop_fullscreen_layout_create_panel(
        left, C_CANVAS, 100, 0, LV_FLEX_FLOW_COLUMN);
    music_panel_make_transparent(meta);
    lv_obj_set_height(meta, 0);
    lv_obj_set_flex_grow(meta, 1);
    lv_obj_set_style_pad_row(meta, 10, 0);

    lv_obj_set_style_pad_top(meta, MUSIC_LYRICS_TOP, 0);

    lv_obj_set_flex_align(meta, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START);
    s_music.title = music_label(meta, "", C_INK, 0, 0);
    lv_obj_set_width(s_music.title, LV_PCT(100));
    s_music.artist = music_label(meta, "", C_INK_MUTED, 0, 0);
    lv_obj_set_width(s_music.artist, LV_PCT(100));
    s_music.album = music_label(meta, "", C_INK_MUTED, 0, 0);
    lv_obj_set_width(s_music.album, LV_PCT(100));

    lv_obj_t *right = desktop_fullscreen_layout_create_panel(
        layout.content, C_CANVAS, MUSIC_RIGHT_PERCENT, 0, LV_FLEX_FLOW_COLUMN);
    music_panel_make_transparent(right);

    lv_obj_set_layout(right, LV_LAYOUT_NONE);
    lv_obj_t *artwork = lv_obj_create(right);
    lv_obj_set_size(artwork, MUSIC_ARTWORK_SIZE, MUSIC_ARTWORK_SIZE);

    lv_obj_align(artwork, LV_ALIGN_RIGHT_MID, MUSIC_ARTWORK_OFFSET_X, MUSIC_ARTWORK_DROP);
    music_panel_make_transparent(artwork);
    lv_obj_set_style_border_width(artwork, 0, 0);
    lv_obj_set_style_pad_all(artwork, 0, 0);
    lv_obj_clear_flag(artwork, LV_OBJ_FLAG_SCROLLABLE);

    s_music.album_image = lv_image_create(artwork);
    lv_obj_set_size(s_music.album_image, MUSIC_ALBUM_SIZE, MUSIC_ALBUM_SIZE);
    lv_image_set_inner_align(s_music.album_image, LV_IMAGE_ALIGN_CONTAIN);
    lv_image_set_src(s_music.album_image, &img_album);
    lv_obj_center(s_music.album_image);

    s_music.disc = lv_image_create(artwork);
    lv_image_set_src(s_music.disc, &img_disc);
    lv_obj_center(s_music.disc);
    lv_image_set_pivot(s_music.disc, MUSIC_DISC_PIVOT_X, MUSIC_DISC_PIVOT_Y);
    lv_image_set_rotation(s_music.disc, APP_MUSIC_DISC_START_ANGLE);

    s_music.tonearm = lv_image_create(artwork);
    lv_image_set_src(s_music.tonearm, &img_tonearm);

    lv_obj_align(s_music.tonearm, LV_ALIGN_CENTER, MUSIC_TONEARM_OFFSET_X,
                 MUSIC_TONEARM_OFFSET_Y);
    lv_image_set_pivot(s_music.tonearm, MUSIC_TONEARM_PIVOT_X, MUSIC_TONEARM_PIVOT_Y);
    lv_image_set_rotation(s_music.tonearm, APP_MUSIC_ARM_IDLE_ANGLE);

    music_control_row_create(layout.root);
    music_control_row_align(left);
    music_progress_block_create(left, s_music.controls);

    music_volume_hud_create(layout.root, s_music.progress_block);

    music_update_labels();
    s_music.poll_timer = lv_timer_create(music_poll_cb, MUSIC_POLL_MS, NULL);
}
