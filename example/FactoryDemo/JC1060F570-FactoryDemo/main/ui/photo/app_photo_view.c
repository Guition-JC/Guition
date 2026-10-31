/**
 * SPDX-License-Identifier: GPL-2.0-only
 */
#include "app_photo_view.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "display.h"

#include "image/lv_image_decoder_private.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_attr.h"
#include "misc/cache/instance/lv_image_cache.h"
#include "desktop_big_buffer.h"
#include "desktop_language.h"
#include "desktop_wallpaper.h"
#include "photo_catalog.h"
#include "photo_source.h"

#define TAG "PhotoAlbum"
#define L(key) desktop_language_lookup(key)

#define PHOTO_PANEL_WIDTH 540
#define PHOTO_PANEL_HEIGHT 224
#define PHOTO_OVERLAY_MARGIN 18

#define PHOTO_ADVANCE_PERIOD_MS 5000

#define C_CANVAS lv_color_hex(0x000000)
#define C_SURFACE lv_color_hex(0x141A22)
#define C_MUTED lv_color_hex(0x2A3340)
#define C_INK lv_color_hex(0xF5F7FA)
#define C_INK_MUTED lv_color_hex(0xB4BFCC)
#define C_ACCENT lv_color_hex(0x147EFB)
#define C_ACCENT_PRESSED lv_color_hex(0x0F63C8)
#define C_SCRIM lv_color_hex(0x000000)

/** @brief 照片目录与合并索引保存在 PSRAM 中。 */
static EXT_RAM_BSS_ATTR photo_source_list_t s_sd_list;
static EXT_RAM_BSS_ATTR photo_source_list_t s_builtin_list;
static EXT_RAM_BSS_ATTR photo_catalog_t s_catalog;

static lv_obj_t *s_root;
static lv_obj_t *s_image;
static lv_obj_t *s_source_label;
static lv_obj_t *s_position_label;
static lv_obj_t *s_notice_label;
static lv_obj_t *s_panel;
static lv_obj_t *s_panel_detail;
static app_photo_toast_cb_t s_toast_cb;
static size_t s_index;
static uint16_t s_screen_width;
static uint16_t s_screen_height;
static bool s_panel_open;
static bool s_have_image;
static lv_timer_t *s_advance_timer;

/** @brief 当前照片路径；对应的解码缓存由 LVGL 管理。 */
EXT_RAM_BSS_ATTR static char s_current_path[PHOTO_CATALOG_PATH_MAX];

/**
 * @brief 解除图像引用，再释放当前照片的解码缓存。
 */
static void photo_cache_release(void)
{
    if (s_current_path[0] == '\0') {
        return;
    }
    if (s_image != NULL) {
        lv_image_set_src(s_image, NULL);
    }
    lv_image_cache_drop(s_current_path);
    s_current_path[0] = '\0';
}

static void photo_toast(const char *message)
{
    if (s_toast_cb != NULL && message != NULL) {
        s_toast_cb(message);
    }
}

static void style_plain(lv_obj_t *object, lv_color_t color, int32_t radius)
{
    lv_obj_remove_style_all(object);
    lv_obj_set_style_bg_color(object, color, 0);
    lv_obj_set_style_bg_opa(object, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(object, 0, 0);
    lv_obj_set_style_radius(object, radius, 0);
    lv_obj_set_style_pad_all(object, 0, 0);
    lv_obj_clear_flag(object, LV_OBJ_FLAG_SCROLLABLE);
}

static lv_obj_t *make_label(lv_obj_t *parent, const char *text, lv_color_t color)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_color(label, color, 0);
    lv_obj_clear_flag(label, LV_OBJ_FLAG_CLICKABLE);
    return label;
}

static lv_obj_t *make_button(lv_obj_t *parent, const char *text, lv_color_t background,
                             int32_t x, int32_t y, int32_t width, int32_t height,
                             lv_event_cb_t callback)
{
    lv_obj_t *button = lv_button_create(parent);
    style_plain(button, background, 10);
    lv_obj_set_pos(button, x, y);
    lv_obj_set_size(button, width, height);
    lv_obj_set_style_bg_color(button, C_ACCENT_PRESSED, LV_STATE_PRESSED);
    lv_obj_add_event_cb(button, callback, LV_EVENT_CLICKED, NULL);

    lv_obj_t *label = make_label(button, text, C_INK);
    lv_obj_center(label);
    return button;
}

static void set_notice(const char *text)
{
    if (s_notice_label == NULL) {
        return;
    }
    if (text == NULL || text[0] == '\0') {
        lv_obj_add_flag(s_notice_label, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    lv_label_set_text(s_notice_label, text);
    lv_obj_remove_flag(s_notice_label, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_notice_label);
}

static void update_overlay_labels(void)
{
    if (s_catalog.count == 0U) {
        lv_label_set_text(s_source_label, "");
        lv_label_set_text(s_position_label, "");
        return;
    }

    const photo_catalog_entry_t *entry = &s_catalog.entries[s_index];
    const char *source = entry->source == PHOTO_SOURCE_SD ? L(DESKTOP_TEXT_ALBUM_SOURCE_SD)
                                                          : L(DESKTOP_TEXT_ALBUM_SOURCE_BUILTIN);
    lv_label_set_text(s_source_label, source);

    char position[32];
    snprintf(position, sizeof(position), L(DESKTOP_TEXT_ALBUM_POSITION),
             (unsigned)(s_index + 1U), (unsigned)s_catalog.count);
    lv_label_set_text(s_position_label, position);
}

static void update_path_buffer(void)
{
    s_current_path[0] = '\0';
    if (s_catalog.count == 0U) {
        return;
    }

    snprintf(s_current_path, sizeof(s_current_path), "%s", s_catalog.entries[s_index].lv_path);
}

/**
 * @brief 加载目录中的照片；尺寸、内存预算或解码校验失败时返回 false。
 * @param index 照片目录中的条目索引。
 * @param[out] over_budget 图像超过解码预算时设为 true。
 * @return 成功显示照片时返回 true，否则返回 false。
 */
static bool show_entry(size_t index, bool *over_budget)
{
    const photo_catalog_entry_t *entry = &s_catalog.entries[index];
    lv_image_header_t header;

    *over_budget = false;
    if (lv_image_decoder_get_info(entry->lv_path, &header) != LV_RESULT_OK) {
        ESP_LOGW(TAG, "image info failed: %s", entry->lv_path);
        return false;
    }
    if (header.w == 0 || header.h == 0) {
        ESP_LOGW(TAG, "image reports empty size: %s", entry->lv_path);
        return false;
    }
    if (!photo_catalog_within_decode_budget(entry->lv_path, header.w, header.h)) {
        ESP_LOGW(TAG, "image over decode budget: %s (%" PRIu32 "x%" PRIu32 ")",
                 entry->lv_path, (uint32_t)header.w, (uint32_t)header.h);
        *over_budget = true;
        return false;
    }

    photo_cache_release();

    ESP_LOGI(TAG, "decode %u/%u %s psram free=%u largest=%u", (unsigned)(index + 1U),
             (unsigned)s_catalog.count, entry->lv_path,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM));

    (void)desktop_big_buffer_reserve((size_t)header.w * (size_t)header.h * 2U);

    lv_image_decoder_dsc_t decoder_dsc;
    if (lv_image_decoder_open(&decoder_dsc, entry->lv_path, NULL) != LV_RESULT_OK) {
        ESP_LOGW(TAG, "decode failed: %s", entry->lv_path);
        return false;
    }
    lv_image_decoder_close(&decoder_dsc);

    lv_image_set_src(s_image, entry->lv_path);
    const uint16_t scale = photo_catalog_scale_for_screen((uint16_t)header.w, (uint16_t)header.h,
                                                         s_screen_width, s_screen_height);
    lv_image_set_scale(s_image, scale);
    lv_obj_remove_flag(s_image, LV_OBJ_FLAG_HIDDEN);
    s_have_image = true;
    ESP_LOGI(TAG, "show %u/%u %s scale=%u source=%s", (unsigned)(index + 1U),
             (unsigned)s_catalog.count, entry->lv_path, (unsigned)scale,
             photo_catalog_source_name(entry->source));
    return true;
}

static void show_index(size_t index)
{
    if (s_image == NULL) {
        return;
    }
    if (s_catalog.count == 0U) {
        lv_image_set_src(s_image, NULL);
        lv_obj_add_flag(s_image, LV_OBJ_FLAG_HIDDEN);
        s_have_image = false;
        update_overlay_labels();
        update_path_buffer();
        set_notice(L(DESKTOP_TEXT_ALBUM_EMPTY));
        return;
    }

    const size_t count = s_catalog.count;
    size_t candidate = index % count;
    bool saw_over_budget = false;
    bool saw_decode_failure = false;

    for (size_t attempt = 0U; attempt < count; ++attempt) {
        bool over_budget = false;
        if (show_entry(candidate, &over_budget)) {
            s_index = candidate;
            set_notice(NULL);
            update_overlay_labels();
            update_path_buffer();
            return;
        }
        saw_over_budget = saw_over_budget || over_budget;
        saw_decode_failure = saw_decode_failure || !over_budget;
        candidate = photo_catalog_next_index(candidate, count);
    }

    lv_image_set_src(s_image, NULL);
    lv_obj_add_flag(s_image, LV_OBJ_FLAG_HIDDEN);
    s_have_image = false;
    s_index = index % count;
    update_overlay_labels();
    update_path_buffer();
    set_notice(saw_decode_failure ? L(DESKTOP_TEXT_ALBUM_DECODE_FAILED)
                                  : (saw_over_budget ? L(DESKTOP_TEXT_ALBUM_TOO_LARGE)
                                                     : L(DESKTOP_TEXT_ALBUM_EMPTY)));
}

static void close_panel(void)
{
    s_panel_open = false;
    if (s_panel != NULL) {
        lv_obj_add_flag(s_panel, LV_OBJ_FLAG_HIDDEN);
    }
}

static void panel_backdrop_cb(lv_event_t *event)
{
    if (lv_event_get_target_obj(event) == s_panel) {
        close_panel();
    }
}

static void panel_cancel_cb(lv_event_t *event)
{
    (void)event;
    close_panel();
}

static void panel_apply_cb(lv_event_t *event)
{
    (void)event;
    if (!s_have_image || s_current_path[0] == '\0') {
        close_panel();
        return;
    }

    const esp_err_t ret = desktop_wallpaper_set_from_path(s_current_path);
    close_panel();
    photo_toast(ret == ESP_OK ? L(DESKTOP_TEXT_ALBUM_WALLPAPER_APPLIED)
                              : L(DESKTOP_TEXT_ALBUM_WALLPAPER_FAILED));
    ESP_LOGI(TAG, "set-wallpaper path=%s result=%s", s_current_path, esp_err_to_name(ret));
}

static void open_panel(void)
{
    if (s_panel == NULL || !s_have_image) {
        return;
    }

    lv_label_set_text(s_panel_detail, s_current_path);
    lv_obj_remove_flag(s_panel, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_panel);
    s_panel_open = true;
}

static void photo_gesture_cb(lv_event_t *event)
{
    const lv_dir_t direction = lv_indev_get_gesture_dir(lv_indev_active());

    ESP_LOGI(TAG, "gesture dir=%s target=%s panel=%d entries=%u index=%u/%u",
             direction == LV_DIR_LEFT ? "left" : (direction == LV_DIR_RIGHT ? "right" : "other"),
             lv_event_get_target_obj(event) == s_root ? "album-root" : "other",
             s_panel_open ? 1 : 0, (unsigned)s_catalog.count,
             (unsigned)(s_index + 1U), (unsigned)s_catalog.count);
    if (s_panel_open || s_catalog.count == 0U) {
        return;
    }

    if (direction == LV_DIR_LEFT) {
        show_index(photo_catalog_next_index(s_index, s_catalog.count));
    } else if (direction == LV_DIR_RIGHT) {
        show_index(photo_catalog_prev_index(s_index, s_catalog.count));
    }

    if (s_advance_timer != NULL) {
        lv_timer_reset(s_advance_timer);
    }
}

static void photo_long_press_cb(lv_event_t *event)
{
    (void)event;
    if (!s_panel_open) {
        open_panel();
    }
}

/**
 * @brief 在 LVGL 任务中轮播照片，壁纸确认面板打开时暂停。
 * @param timer 触发回调的 LVGL 定时器。
 */
static void photo_advance_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    if (s_panel_open || s_catalog.count < 2U) {
        return;
    }
    show_index(photo_catalog_next_index(s_index, s_catalog.count));
}

static void build_panel(void)
{
    s_panel = lv_obj_create(s_root);
    style_plain(s_panel, C_SCRIM, 0);
    lv_obj_set_style_bg_opa(s_panel, LV_OPA_50, 0);
    lv_obj_set_size(s_panel, s_screen_width, s_screen_height);
    lv_obj_set_pos(s_panel, 0, 0);
    lv_obj_add_flag(s_panel, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_panel, panel_backdrop_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_add_flag(s_panel, LV_OBJ_FLAG_HIDDEN);

    const int32_t panel_x = (s_screen_width > PHOTO_PANEL_WIDTH)
                                ? (int32_t)((s_screen_width - PHOTO_PANEL_WIDTH) / 2U)
                                : 0;
    const int32_t panel_y = (s_screen_height > PHOTO_PANEL_HEIGHT)
                                ? (int32_t)((s_screen_height - PHOTO_PANEL_HEIGHT) / 2U)
                                : 0;

    lv_obj_t *card = lv_obj_create(s_panel);
    style_plain(card, C_SURFACE, 16);
    lv_obj_set_size(card, PHOTO_PANEL_WIDTH, PHOTO_PANEL_HEIGHT);
    lv_obj_set_pos(card, panel_x, panel_y);
    lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_EVENT_BUBBLE);

    lv_obj_t *title = make_label(card, L(DESKTOP_TEXT_ALBUM_SET_WALLPAPER), C_INK);
    lv_obj_set_pos(title, 24, 22);

    s_panel_detail = make_label(card, "", C_INK_MUTED);
    lv_obj_set_pos(s_panel_detail, 24, 56);
    lv_obj_set_width(s_panel_detail, PHOTO_PANEL_WIDTH - 48);
    lv_label_set_long_mode(s_panel_detail, LV_LABEL_LONG_MODE_DOTS);

    make_button(card, L(DESKTOP_TEXT_ALBUM_CANCEL), C_MUTED, 24, 150, 230, 52, panel_cancel_cb);
    make_button(card, L(DESKTOP_TEXT_ALBUM_APPLY), C_ACCENT, 286, 150, 230, 52, panel_apply_cb);
}

static void build_overlay(void)
{
    s_source_label = make_label(s_root, "", C_INK);
    lv_obj_set_pos(s_source_label, PHOTO_OVERLAY_MARGIN, 12);
    lv_obj_set_style_bg_color(s_source_label, C_SCRIM, 0);
    lv_obj_set_style_bg_opa(s_source_label, LV_OPA_60, 0);
    lv_obj_set_style_pad_hor(s_source_label, 10, 0);
    lv_obj_set_style_pad_ver(s_source_label, 4, 0);
    lv_obj_set_style_radius(s_source_label, 8, 0);
    lv_obj_set_style_text_color(s_source_label, C_INK, 0);

    s_position_label = make_label(s_root, "", C_INK);
    lv_obj_set_style_bg_color(s_position_label, C_SCRIM, 0);
    lv_obj_set_style_bg_opa(s_position_label, LV_OPA_60, 0);
    lv_obj_set_style_pad_hor(s_position_label, 10, 0);
    lv_obj_set_style_pad_ver(s_position_label, 4, 0);
    lv_obj_set_style_radius(s_position_label, 8, 0);
    lv_obj_set_style_text_color(s_position_label, C_INK, 0);
    lv_obj_align(s_position_label, LV_ALIGN_TOP_RIGHT, -PHOTO_OVERLAY_MARGIN, 12);

    s_notice_label = make_label(s_root, "", C_INK);
    lv_obj_set_width(s_notice_label, s_screen_width - 2 * PHOTO_OVERLAY_MARGIN);
    lv_obj_set_style_text_align(s_notice_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(s_notice_label, LV_ALIGN_CENTER, 0, 0);
    lv_obj_add_flag(s_notice_label, LV_OBJ_FLAG_HIDDEN);
}

bool app_photo_view_build(lv_obj_t *parent, app_photo_toast_cb_t toast_cb)
{
    app_photo_view_cleanup();
    if (parent == NULL) {
        return false;
    }

    s_toast_cb = toast_cb;
    s_screen_width = (uint16_t)lv_obj_get_width(parent);
    s_screen_height = (uint16_t)lv_obj_get_height(parent);
    if (s_screen_width == 0U || s_screen_height == 0U) {
        if (display_get_size(&s_screen_width, &s_screen_height) != ESP_OK) {
            ESP_LOGE(TAG, "screen size unavailable");
            return false;
        }
    }

    const bool sd_scanned = photo_source_scan(PHOTO_SOURCE_SD_ROOT, &s_sd_list);
    const bool builtin_scanned = photo_source_scan(PHOTO_SOURCE_BUILTIN_ROOT, &s_builtin_list);

    photo_catalog_source_t sources[2];
    size_t source_count = 0U;
    if (sd_scanned) {
        sources[source_count].posix_root = PHOTO_SOURCE_SD_ROOT;
        sources[source_count].lv_root = PHOTO_SOURCE_SD_LV_ROOT;
        sources[source_count].source = PHOTO_SOURCE_SD;
        sources[source_count].entries = s_sd_list.entries;
        sources[source_count].count = s_sd_list.count;
        source_count++;
    }
    if (builtin_scanned) {
        sources[source_count].posix_root = PHOTO_SOURCE_BUILTIN_ROOT;
        sources[source_count].lv_root = PHOTO_SOURCE_BUILTIN_LV_ROOT;
        sources[source_count].source = PHOTO_SOURCE_BUILTIN;
        sources[source_count].entries = s_builtin_list.entries;
        sources[source_count].count = s_builtin_list.count;
        source_count++;
    }

    const photo_catalog_error_t merge =
        photo_catalog_merge(sources, source_count, &s_catalog);
    ESP_LOGI(TAG, "catalog=%s entries=%u sd=%u(%s) builtin=%u(%s)",
             photo_catalog_error_name(merge), (unsigned)s_catalog.count,
             (unsigned)s_sd_list.count, sd_scanned ? "ok" : "unavailable",
             (unsigned)s_builtin_list.count, builtin_scanned ? "ok" : "unavailable");

    s_root = lv_obj_create(parent);
    if (s_root == NULL) {
        ESP_LOGE(TAG, "album root allocation failed");
        return false;
    }
    style_plain(s_root, C_CANVAS, 0);
    lv_obj_set_pos(s_root, 0, 0);
    lv_obj_set_size(s_root, s_screen_width, s_screen_height);
    lv_obj_add_flag(s_root, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_clear_flag(s_root, LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_add_event_cb(s_root, photo_gesture_cb, LV_EVENT_GESTURE, NULL);
    lv_obj_add_event_cb(s_root, photo_long_press_cb, LV_EVENT_LONG_PRESSED, NULL);

    s_image = lv_image_create(s_root);
    if (s_image == NULL) {
        ESP_LOGE(TAG, "album image allocation failed");
        app_photo_view_cleanup();
        return false;
    }
    lv_obj_set_size(s_image, s_screen_width, s_screen_height);
    lv_obj_set_pos(s_image, 0, 0);

    lv_obj_clear_flag(s_image, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_image_set_inner_align(s_image, LV_IMAGE_ALIGN_CENTER);

    build_overlay();
    build_panel();
    show_index(0U);
    s_advance_timer = lv_timer_create(photo_advance_timer_cb, PHOTO_ADVANCE_PERIOD_MS, NULL);
    return true;
}

void app_photo_view_cleanup(void)
{
    photo_cache_release();
    if (s_advance_timer != NULL) {
        lv_timer_delete(s_advance_timer);
        s_advance_timer = NULL;
    }
    if (s_root != NULL) {
        lv_obj_delete(s_root);
    }
    s_root = NULL;
    s_image = NULL;
    s_source_label = NULL;
    s_position_label = NULL;
    s_notice_label = NULL;
    s_panel = NULL;
    s_panel_detail = NULL;
    s_toast_cb = NULL;
    s_index = 0U;
    s_panel_open = false;
    s_have_image = false;
    s_current_path[0] = '\0';
    photo_catalog_reset(&s_catalog);
    memset(&s_sd_list, 0, sizeof(s_sd_list));
    memset(&s_builtin_list, 0, sizeof(s_builtin_list));
}
