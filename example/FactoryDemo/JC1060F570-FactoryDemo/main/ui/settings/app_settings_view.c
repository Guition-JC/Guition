/**
 * SPDX-License-Identifier: GPL-2.0-only
 */
#include "app_settings_view.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "display.h"
#include "esp_attr.h"
#include "esp_netif_ip_addr.h"
#include "ethernet_service.h"
#include "music_player_service.h"
#include "desktop_font.h"
#include "desktop_fullscreen_layout.h"
#include "power_idle.h"
#include "photo_source.h"
#include "standby_prefs.h"
#include "sleep_timeout.h"
#include "video_player_service.h"
#include "spectrum_prefs_store.h"
#include "time_service.h"
#include "wifi_service.h"
#include "weather_service.h"
#include "include/lvgl/widgets/lv_qrcode.h"

#define L(key) desktop_language_lookup(key)

#define C_CANVAS      lv_color_hex(0xDCE8F5)
#define C_SURFACE     lv_color_hex(0xF7FAFD)
#define C_MUTED       lv_color_hex(0xE2ECF6)
#define C_INK         lv_color_hex(0x122033)
#define C_INK_MUTED   lv_color_hex(0x53667B)
#define C_ACCENT      lv_color_hex(0x147EFB)
#define C_ACCENT_SOFT lv_color_hex(0xD9EAFF)
#define C_DIVIDER     lv_color_hex(0xD9E0E7)

#define SETTINGS_LEFT_PERCENT 46
#define SETTINGS_COLUMN_GAP 20
#define SETTINGS_SECTION_TITLE_HEIGHT 28
#define SETTINGS_ROW_GAP 10
#define SETTINGS_ROW_HEIGHT 104

#define SETTINGS_ROW_HEIGHT_INLINE 56

#define SETTINGS_ROW_HEIGHT_COMPACT 76

#define SETTINGS_ROW_HEIGHT_LANGUAGE_NOTICE 152
#define SETTINGS_ROW_PAD 16

#define SETTINGS_WEATHER_FIELD_COUNT 8
#define SETTINGS_WEATHER_FIELD_COLUMNS 4
#define SETTINGS_WEATHER_FIELD_WIDTH_PERCENT 24
#define SETTINGS_WEATHER_FIELD_HEIGHT 42
#define SETTINGS_WEATHER_FIELD_GAP 6
#define SETTINGS_WEATHER_ROWS \
    ((SETTINGS_WEATHER_FIELD_COUNT + SETTINGS_WEATHER_FIELD_COLUMNS - 1) / \
     SETTINGS_WEATHER_FIELD_COLUMNS)
#define SETTINGS_ROW_HEIGHT_WEATHER                                     \
    (SETTINGS_ROW_PAD * 2 + 24 +                                        \
     SETTINGS_WEATHER_ROWS * SETTINGS_WEATHER_FIELD_HEIGHT +            \
     (SETTINGS_WEATHER_ROWS - 1) * SETTINGS_WEATHER_FIELD_GAP)
#define SETTINGS_WEATHER_URL_MAX 64

#define SETTINGS_WEATHER_CARD_WIDTH 440
#define SETTINGS_WEATHER_CARD_HEIGHT 360
#define SETTINGS_WEATHER_QR_SIZE 200
#define SETTINGS_SLIDER_HEIGHT 8
#define SETTINGS_KNOB_SIZE 20
#define SETTINGS_SLIDER_CLICK_PAD 16
#define SETTINGS_SWITCH_WIDTH 56
#define SETTINGS_SWITCH_HEIGHT 32
#define SETTINGS_BUTTON_HEIGHT 44
#define SETTINGS_BUTTON_PERCENT 47
#define SETTINGS_SLEEP_OPTION_GAP 4

#define SETTINGS_FONT_OPTION_GAP 6
#define SETTINGS_FONT_CARD_WIDTH_PERCENT 62
#define SETTINGS_FONT_CARD_HEIGHT_PERCENT 82

#define SETTINGS_VOLUME_MIN 0
#define SETTINGS_VOLUME_MAX 100

/** @brief Idle timeout options in seconds; zero disables automatic sleep. */
static const uint32_t s_sleep_seconds[] = {
    (uint32_t)POWER_IDLE_TIMEOUT_OFF,
    (uint32_t)POWER_IDLE_TIMEOUT_30S,
    (uint32_t)POWER_IDLE_TIMEOUT_1MIN,
    (uint32_t)POWER_IDLE_TIMEOUT_2MIN,
    (uint32_t)POWER_IDLE_TIMEOUT_5MIN,
};
static const desktop_text_key_t s_sleep_label_keys[] = {
    DESKTOP_TEXT_SETTINGS_SLEEP_OFF,
    DESKTOP_TEXT_SETTINGS_SLEEP_30S,
    DESKTOP_TEXT_SETTINGS_SLEEP_1MIN,
    DESKTOP_TEXT_SETTINGS_SLEEP_2MIN,
    DESKTOP_TEXT_SETTINGS_SLEEP_5MIN,
};
#define SETTINGS_SLEEP_OPTION_COUNT (sizeof(s_sleep_seconds) / sizeof(s_sleep_seconds[0]))
_Static_assert(sizeof(s_sleep_label_keys) / sizeof(s_sleep_label_keys[0]) ==
                   SETTINGS_SLEEP_OPTION_COUNT,
               "sleep timeout labels and seconds must describe the same steps");

static app_settings_view_toast_cb_t s_toast_cb;
static app_settings_view_language_cb_t s_language_cb;
static app_settings_view_font_cb_t s_font_cb;
static lv_obj_t *s_backlight_slider;
static lv_obj_t *s_backlight_value;
static lv_obj_t *s_volume_slider;
static lv_obj_t *s_volume_value;
static lv_obj_t *s_mute_switch;
static lv_obj_t *s_bt_switch;
static lv_obj_t *s_sleep_options;
static lv_obj_t *s_standby_options;
static lv_obj_t *s_standby_interval_options;
static lv_obj_t *s_standby_overlay;
static lv_obj_t *s_standby_list;
static lv_obj_t *s_standby_confirm;
static lv_obj_t *s_settings_parent;
static standby_mode_t s_standby_pending_mode;
static bool s_standby_photo_selected[STANDBY_MAX_PHOTOS];
static size_t s_standby_video_selected;
EXT_RAM_BSS_ATTR static standby_prefs_t s_standby_draft;
EXT_RAM_BSS_ATTR static photo_catalog_t s_standby_photo_catalog;
EXT_RAM_BSS_ATTR static video_player_catalog_entry_t s_standby_videos[VIDEO_PLAYER_MAX_ENTRIES];
static size_t s_standby_video_count;
static lv_obj_t *s_time_source_options;
static lv_obj_t *s_font_overlay;
static lv_obj_t *s_font_list;
static lv_obj_t *s_weather_fields;
static lv_obj_t *s_weather_overlay;
static lv_obj_t *s_weather_qr;
static lv_obj_t *s_weather_url;

/** @brief Font names remain valid for indexed option callbacks. */
static char s_font_names[DESKTOP_FONT_SD_LIST_MAX][DESKTOP_FONT_NAME_MAX];
static size_t s_font_name_count;

static void settings_toast(const char *message)
{
    if (s_toast_cb != NULL) {
        s_toast_cb(message);
    }
}

static void settings_percent_label_set(lv_obj_t *label, int32_t value)
{
    if (label == NULL) {
        return;
    }
    char text[8];
    snprintf(text, sizeof(text), "%d%%", (int)value);
    lv_label_set_text(label, text);
}

static lv_obj_t *settings_section_title(lv_obj_t *parent, const char *text)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_color(label, C_INK_MUTED, 0);

    lv_obj_set_height(label, SETTINGS_SECTION_TITLE_HEIGHT);
    return label;
}

static lv_obj_t *settings_row(lv_obj_t *parent, const char *title)
{
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_remove_style_all(row);
    lv_obj_set_style_bg_color(row, C_SURFACE, 0);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(row, 14, 0);
    lv_obj_set_style_pad_all(row, SETTINGS_ROW_PAD, 0);
    lv_obj_set_width(row, LV_PCT(100));
    lv_obj_set_height(row, SETTINGS_ROW_HEIGHT);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title_label = lv_label_create(row);
    lv_label_set_text(title_label, title);
    lv_obj_set_style_text_color(title_label, C_INK, 0);
    lv_obj_align(title_label, LV_ALIGN_TOP_LEFT, 0, 0);
    return row;
}

static lv_obj_t *settings_row_value(lv_obj_t *row)
{
    lv_obj_t *label = lv_label_create(row);
    lv_label_set_text(label, "");
    lv_obj_set_style_text_color(label, C_INK_MUTED, 0);
    lv_obj_align(label, LV_ALIGN_TOP_RIGHT, 0, 0);
    return label;
}

static lv_obj_t *settings_row_slider(lv_obj_t *row, int32_t minimum, int32_t maximum,
                                     int32_t value)
{
    lv_obj_t *slider = lv_slider_create(row);
    lv_obj_set_width(slider, LV_PCT(100));
    lv_obj_set_height(slider, SETTINGS_SLIDER_HEIGHT);
    lv_obj_align(slider, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_slider_set_range(slider, minimum, maximum);
    lv_slider_set_value(slider, value, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(slider, C_MUTED, LV_PART_MAIN);
    lv_obj_set_style_bg_color(slider, C_ACCENT, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(slider, C_ACCENT, LV_PART_KNOB);
    lv_obj_set_style_radius(slider, LV_RADIUS_CIRCLE, LV_PART_KNOB);
    lv_obj_set_style_width(slider, SETTINGS_KNOB_SIZE, LV_PART_KNOB);
    lv_obj_set_style_height(slider, SETTINGS_KNOB_SIZE, LV_PART_KNOB);

    lv_obj_set_ext_click_area(slider, SETTINGS_SLIDER_CLICK_PAD);
    return slider;
}

static lv_obj_t *settings_row_switch(lv_obj_t *row, bool checked)
{
    lv_obj_t *sw = lv_switch_create(row);
    lv_obj_set_size(sw, SETTINGS_SWITCH_WIDTH, SETTINGS_SWITCH_HEIGHT);
    lv_obj_align(sw, LV_ALIGN_RIGHT_MID, 0, 8);
    lv_obj_set_style_bg_color(sw, C_ACCENT, LV_PART_INDICATOR | LV_STATE_CHECKED);
    if (checked) {
        lv_obj_add_state(sw, LV_STATE_CHECKED);
    }
    return sw;
}

static lv_obj_t *settings_button(lv_obj_t *parent, const char *text, bool selected)
{
    lv_obj_t *button = lv_button_create(parent);
    lv_obj_remove_style_all(button);
    lv_obj_set_style_bg_color(button, selected ? C_ACCENT_SOFT : C_MUTED, 0);
    lv_obj_set_style_bg_opa(button, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(button, selected ? 2 : 1, 0);
    lv_obj_set_style_border_color(button, selected ? C_ACCENT : C_DIVIDER, 0);
    lv_obj_set_style_border_opa(button, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(button, 12, 0);
    lv_obj_set_style_pad_all(button, 0, 0);
    lv_obj_set_style_bg_color(button, C_ACCENT_SOFT, LV_STATE_PRESSED);
    lv_obj_t *label = lv_label_create(button);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_color(label, C_INK, 0);
    lv_obj_center(label);
    return button;
}

static void settings_switch_set(lv_obj_t *sw, bool checked)
{
    if (sw == NULL) {
        return;
    }

    if (checked) {
        lv_obj_add_state(sw, LV_STATE_CHECKED);
    } else {
        lv_obj_remove_state(sw, LV_STATE_CHECKED);
    }
}

/**
 * @brief Read back the applied audio level and mute state.
 */
static void settings_volume_controls_refresh(void)
{
    uint8_t level = 0U;
    bool muted = false;
    if (music_player_service_get_volume(&level, &muted) != ESP_OK) {
        return;
    }
    if (s_volume_slider != NULL) {
        lv_slider_set_value(s_volume_slider, (int32_t)level, LV_ANIM_OFF);
    }
    settings_percent_label_set(s_volume_value, (int32_t)level);
    settings_switch_set(s_mute_switch, muted);
}

/**
 * @brief Apply brightness during dragging and persist it on release.
 * @param event LVGL event that triggered the callback.
 */
static void backlight_slider_cb(lv_event_t *event)
{
    lv_obj_t *slider = lv_event_get_target_obj(event);
    if (slider == NULL) {
        return;
    }

    if (lv_event_get_code(event) == LV_EVENT_RELEASED) {
        (void)display_backlight_persist();
        return;
    }

    const int32_t percent = lv_slider_get_value(slider);
    if (display_backlight_set((uint8_t)percent) != ESP_OK) {
        settings_toast(L(DESKTOP_TEXT_SETTINGS_APPLY_FAILED));
        const uint8_t applied = display_backlight_get();
        if (applied > 0U) {
            lv_slider_set_value(slider, (int32_t)applied, LV_ANIM_OFF);
        }
    }
    settings_percent_label_set(s_backlight_value, lv_slider_get_value(slider));
}

/**
 * @brief Adjust volume, clear mute and persist the final state on release.
 * @param event LVGL event that triggered the callback.
 */
static void volume_slider_cb(lv_event_t *event)
{
    lv_obj_t *slider = lv_event_get_target_obj(event);
    if (slider == NULL) {
        return;
    }

    if (lv_event_get_code(event) == LV_EVENT_RELEASED) {
        (void)music_player_service_save_volume();
        return;
    }

    const int32_t level = lv_slider_get_value(slider);
    if (music_player_service_set_volume((uint8_t)level) != ESP_OK) {
        settings_volume_controls_refresh();
        settings_toast(L(DESKTOP_TEXT_SETTINGS_APPLY_FAILED));
        return;
    }
    settings_percent_label_set(s_volume_value, level);

    if (s_mute_switch != NULL && lv_obj_has_state(s_mute_switch, LV_STATE_CHECKED) &&
        music_player_service_set_mute(false) == ESP_OK) {
        settings_switch_set(s_mute_switch, false);
    }
}

static void mute_switch_cb(lv_event_t *event)
{
    lv_obj_t *sw = lv_event_get_target_obj(event);
    if (sw == NULL) {
        return;
    }
    const bool muted = lv_obj_has_state(sw, LV_STATE_CHECKED);
    if (music_player_service_set_mute(muted) != ESP_OK) {
        bool applied_muted = false;
        uint8_t level = 0U;
        if (music_player_service_get_volume(&level, &applied_muted) == ESP_OK) {
            settings_switch_set(sw, applied_muted);
        }
        settings_toast(L(DESKTOP_TEXT_SETTINGS_APPLY_FAILED));
        return;
    }
    (void)music_player_service_save_volume();
}

/**
 * @brief Switch the Classic Bluetooth controller; audio profiles belong to the music app.
 * @param event LVGL event that triggered the callback.
 */
static void bt_power_switch_cb(lv_event_t *event)
{
    lv_obj_t *sw = lv_event_get_target_obj(event);
    if (sw == NULL) {
        return;
    }
    const bool requested = lv_obj_has_state(sw, LV_STATE_CHECKED);
    const esp_err_t result = music_player_service_set_bt_power(requested);
    if (result != ESP_OK) {
        if (music_player_service_get_bt_power()) {
            lv_obj_add_state(sw, LV_STATE_CHECKED);
        } else {
            lv_obj_remove_state(sw, LV_STATE_CHECKED);
        }
        settings_toast(L(DESKTOP_TEXT_SETTINGS_BLUETOOTH_UNAVAILABLE));
        return;
    }
    settings_toast(requested ? L(DESKTOP_TEXT_SETTINGS_BLUETOOTH_ON)
                             : L(DESKTOP_TEXT_SETTINGS_BLUETOOTH_OFF));
}

static void language_button_cb(lv_event_t *event)
{
    const desktop_language_t language =
        (desktop_language_t)(uintptr_t)lv_event_get_user_data(event);
    if (s_language_cb != NULL) {
        s_language_cb(language);
    }
}

static void settings_sleep_options_build(lv_obj_t *container);

static void settings_sleep_option_cb(lv_event_t *event)
{
    const size_t index = (size_t)(uintptr_t)lv_event_get_user_data(event);
    if (index >= SETTINGS_SLEEP_OPTION_COUNT) {
        return;
    }
    const esp_err_t stored = sleep_timeout_set(s_sleep_seconds[index]);

    settings_sleep_options_build(s_sleep_options);
    if (stored != ESP_OK) {
        settings_toast(L(DESKTOP_TEXT_SETTINGS_APPLY_FAILED));
        return;
    }
    char message[64];
    snprintf(message, sizeof(message), L(DESKTOP_TEXT_SETTINGS_SLEEP_SET),
             L(s_sleep_label_keys[index]));
    settings_toast(message);
}

static void settings_sleep_options_build(lv_obj_t *container)
{
    if (container == NULL) {
        return;
    }
    lv_obj_clean(container);
    const uint32_t current = sleep_timeout_get();
    for (size_t i = 0; i < SETTINGS_SLEEP_OPTION_COUNT; ++i) {
        lv_obj_t *button = settings_button(container, L(s_sleep_label_keys[i]),
                                          s_sleep_seconds[i] == current);

        lv_obj_set_flex_grow(button, 1);
        lv_obj_set_height(button, SETTINGS_BUTTON_HEIGHT);
        lv_obj_add_event_cb(button, settings_sleep_option_cb, LV_EVENT_CLICKED,
                            (void *)(uintptr_t)i);
    }
}

static void build_sleep_row(lv_obj_t *parent)
{
    lv_obj_t *row = settings_row(parent, L(DESKTOP_TEXT_SETTINGS_SLEEP));
    s_sleep_options = lv_obj_create(row);
    lv_obj_remove_style_all(s_sleep_options);
    lv_obj_set_style_bg_opa(s_sleep_options, LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_all(s_sleep_options, 0, 0);
    lv_obj_set_style_pad_column(s_sleep_options, SETTINGS_SLEEP_OPTION_GAP, 0);
    lv_obj_set_size(s_sleep_options, LV_PCT(100), SETTINGS_BUTTON_HEIGHT);
    lv_obj_align(s_sleep_options, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_flex_flow(s_sleep_options, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(s_sleep_options, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(s_sleep_options, LV_OBJ_FLAG_SCROLLABLE);
    settings_sleep_options_build(s_sleep_options);
}

static const desktop_text_key_t s_standby_mode_labels[] = {
    DESKTOP_TEXT_SETTINGS_STANDBY_CLOCK,
    DESKTOP_TEXT_SETTINGS_STANDBY_PHOTOS,
    DESKTOP_TEXT_SETTINGS_STANDBY_VIDEO,
};
static const uint16_t s_standby_intervals[] = {3U, 5U, 10U, 30U};
static void settings_standby_mode_cb(lv_event_t *event);
static void settings_standby_interval_cb(lv_event_t *event);

static void settings_standby_options_build(void)
{
    if (s_standby_options == NULL) return;
    lv_obj_clean(s_standby_options);
    for (size_t i = 0U; i < 3U; ++i) {
        lv_obj_t *button = settings_button(s_standby_options, L(s_standby_mode_labels[i]),
                                           standby_prefs_get()->mode == (standby_mode_t)i);
        lv_obj_set_flex_grow(button, 1);
        lv_obj_set_height(button, SETTINGS_BUTTON_HEIGHT);
        lv_obj_add_event_cb(button, settings_standby_mode_cb, LV_EVENT_CLICKED,
                            (void *)(uintptr_t)i);
    }
}

static void settings_standby_interval_build(void)
{
    if (s_standby_interval_options == NULL) return;
    lv_obj_clean(s_standby_interval_options);
    for (size_t i = 0U; i < 4U; ++i) {
        char label[12];
        snprintf(label, sizeof(label), L(DESKTOP_TEXT_SETTINGS_STANDBY_INTERVAL_VALUE),
                 (unsigned)s_standby_intervals[i]);
        lv_obj_t *button = settings_button(s_standby_interval_options, label,
            standby_prefs_get()->photo_interval_seconds == s_standby_intervals[i]);
        lv_obj_set_flex_grow(button, 1);
        lv_obj_set_height(button, SETTINGS_BUTTON_HEIGHT);
        lv_obj_add_event_cb(button, settings_standby_interval_cb, LV_EVENT_CLICKED,
                            (void *)(uintptr_t)i);
    }
}

static void settings_standby_close(void)
{
    if (s_standby_overlay != NULL) lv_obj_delete(s_standby_overlay);
    s_standby_overlay = NULL;
    s_standby_list = NULL;
    s_standby_confirm = NULL;
}

static void settings_standby_cancel_cb(lv_event_t *event)
{
    (void)event;
    settings_standby_close();
}

static size_t settings_standby_selection_count(void)
{
    if (s_standby_pending_mode == STANDBY_VIDEO)
        return s_standby_video_selected < s_standby_video_count ? 1U : 0U;
    size_t count = 0U;
    for (size_t i = 0U; i < s_standby_photo_catalog.count; ++i)
        if (s_standby_photo_selected[i]) ++count;
    return count;
}

static void settings_standby_item_refresh(size_t index)
{
    lv_obj_t *button = lv_obj_get_child(s_standby_list, (int32_t)index);
    if (button == NULL) return;
    const bool selected = s_standby_pending_mode == STANDBY_VIDEO
        ? s_standby_video_selected == index : s_standby_photo_selected[index];
    const char *name = s_standby_pending_mode == STANDBY_VIDEO
        ? s_standby_videos[index].name : s_standby_photo_catalog.entries[index].name;
    const bool from_sd = s_standby_pending_mode == STANDBY_VIDEO
        ? strncmp(s_standby_videos[index].path, "/sdcard/", 8U) == 0
        : s_standby_photo_catalog.entries[index].source == PHOTO_SOURCE_SD;
    char label[150];
    snprintf(label, sizeof(label), "%s  %s: %s", selected ? "[x]" : "[ ]",
             from_sd ? L(DESKTOP_TEXT_ALBUM_SOURCE_SD) : L(DESKTOP_TEXT_ALBUM_SOURCE_BUILTIN), name);
    lv_label_set_text(lv_obj_get_child(button, 0), label);
    lv_obj_set_style_bg_color(button, selected ? C_ACCENT_SOFT : C_MUTED, 0);
    lv_obj_set_style_border_width(button, selected ? 2 : 1, 0);
    lv_obj_set_style_border_color(button, selected ? C_ACCENT : C_DIVIDER, 0);
}

static void settings_standby_item_cb(lv_event_t *event)
{
    const size_t index = (size_t)(uintptr_t)lv_event_get_user_data(event);
    if (s_standby_pending_mode == STANDBY_VIDEO) {
        if (index >= s_standby_video_count) return;
        const size_t previous = s_standby_video_selected;
        s_standby_video_selected = index;
        if (previous < s_standby_video_count) settings_standby_item_refresh(previous);
    } else {
        if (index >= s_standby_photo_catalog.count) return;
        s_standby_photo_selected[index] = !s_standby_photo_selected[index];
    }
    settings_standby_item_refresh(index);
    if (settings_standby_selection_count() > 0U)
        lv_obj_remove_state(s_standby_confirm, LV_STATE_DISABLED);
    else
        lv_obj_add_state(s_standby_confirm, LV_STATE_DISABLED);
}

static void settings_standby_list_build(void)
{
    lv_obj_clean(s_standby_list);
    const size_t count = s_standby_pending_mode == STANDBY_VIDEO
        ? s_standby_video_count : s_standby_photo_catalog.count;
    if (count == 0U) {
        lv_obj_t *notice = lv_label_create(s_standby_list);
        lv_label_set_text(notice, L(s_standby_pending_mode == STANDBY_VIDEO
            ? DESKTOP_TEXT_SETTINGS_STANDBY_NO_VIDEO : DESKTOP_TEXT_SETTINGS_STANDBY_NO_PHOTOS));
        lv_obj_set_style_text_color(notice, C_INK_MUTED, 0);
        return;
    }
    for (size_t i = 0U; i < count; ++i) {
        const bool selected = s_standby_pending_mode == STANDBY_VIDEO
            ? s_standby_video_selected == i : s_standby_photo_selected[i];
        lv_obj_t *button = settings_button(s_standby_list, "", selected);
        lv_obj_set_width(button, LV_PCT(100));
        lv_obj_set_height(button, SETTINGS_BUTTON_HEIGHT);
        lv_obj_t *text_label = lv_obj_get_child(button, 0);
        lv_obj_set_width(text_label, LV_PCT(92));
        lv_label_set_long_mode(text_label, LV_LABEL_LONG_DOT);
        lv_obj_center(text_label);
        settings_standby_item_refresh(i);
        lv_obj_add_event_cb(button, settings_standby_item_cb, LV_EVENT_CLICKED,
                            (void *)(uintptr_t)i);
    }
}

/**
 * @brief Commit the selected standby mode and media preferences.
 * @param event LVGL event that triggered the callback.
 */
static void settings_standby_confirm_cb(lv_event_t *event)
{
    (void)event;
    if (settings_standby_selection_count() == 0U) return;
    s_standby_draft = *standby_prefs_get();
    s_standby_draft.mode = s_standby_pending_mode;
    if (s_standby_pending_mode == STANDBY_VIDEO) {
        const video_player_catalog_entry_t *entry = &s_standby_videos[s_standby_video_selected];
        s_standby_draft.video.source = strncmp(entry->path, "/sdcard/", 8U) == 0 ? 0U : 1U;
        snprintf(s_standby_draft.video.name, sizeof(s_standby_draft.video.name), "%s", entry->name);
    } else {
        s_standby_draft.photo_count = 0U;
        for (size_t i = 0U; i < s_standby_photo_catalog.count; ++i) {
            if (!s_standby_photo_selected[i]) continue;
            const photo_catalog_entry_t *entry = &s_standby_photo_catalog.entries[i];
            standby_media_t *media = &s_standby_draft.photos[s_standby_draft.photo_count++];
            media->source = entry->source == PHOTO_SOURCE_SD ? 0U : 1U;
            snprintf(media->name, sizeof(media->name), "%s", entry->name);
        }
    }
    if (standby_prefs_save(&s_standby_draft) != ESP_OK) {
        settings_toast(L(DESKTOP_TEXT_SETTINGS_APPLY_FAILED));
        return;
    }
    settings_standby_close();
    settings_standby_options_build();
}

/**
 * @brief Populate standby photo choices from the media catalog.
 */
static void settings_standby_scan_photos(void)
{
    static photo_source_list_t sd;
    static photo_source_list_t builtin;
    photo_source_scan(PHOTO_SOURCE_SD_ROOT, &sd);
    photo_source_scan(PHOTO_SOURCE_BUILTIN_ROOT, &builtin);
    const photo_catalog_source_t sources[] = {
        {PHOTO_SOURCE_SD_ROOT, PHOTO_SOURCE_SD_LV_ROOT, PHOTO_SOURCE_SD, sd.entries, sd.count},
        {PHOTO_SOURCE_BUILTIN_ROOT, PHOTO_SOURCE_BUILTIN_LV_ROOT, PHOTO_SOURCE_BUILTIN,
         builtin.entries, builtin.count},
    };
    (void)photo_catalog_merge(sources, 2U, &s_standby_photo_catalog);
    memset(s_standby_photo_selected, 0, sizeof(s_standby_photo_selected));
    const standby_prefs_t *prefs = standby_prefs_get();
    size_t displayable = 0U;
    for (size_t i = 0U; i < s_standby_photo_catalog.count; ++i) {
        photo_catalog_entry_t *entry = &s_standby_photo_catalog.entries[i];
        lv_image_header_t header;
        if (lv_image_decoder_get_info(entry->lv_path, &header) != LV_RESULT_OK ||
            !photo_catalog_within_decode_budget(entry->lv_path, header.w, header.h)) continue;
        bool selected = false;
        for (size_t j = 0U; j < prefs->photo_count; ++j) {
            if (prefs->photos[j].source == (entry->source == PHOTO_SOURCE_SD ? 0U : 1U) &&
                strcmp(prefs->photos[j].name, entry->name) == 0) selected = true;
        }
        if (displayable != i) s_standby_photo_catalog.entries[displayable] = *entry;
        s_standby_photo_selected[displayable++] = selected;
    }
    s_standby_photo_catalog.count = displayable;
}

static void settings_standby_open(standby_mode_t mode)
{
    s_standby_pending_mode = mode;
    if (mode == STANDBY_PHOTOS) {
        settings_standby_scan_photos();
    } else {
        s_standby_video_count = video_player_service_list(s_standby_videos, VIDEO_PLAYER_MAX_ENTRIES);
        s_standby_video_selected = s_standby_video_count;
        const standby_media_t *saved = &standby_prefs_get()->video;
        for (size_t i = 0U; i < s_standby_video_count; ++i) {
            const bool sd = strncmp(s_standby_videos[i].path, "/sdcard/", 8U) == 0;
            if (saved->source == (sd ? 0U : 1U) && strcmp(saved->name, s_standby_videos[i].name) == 0)
                s_standby_video_selected = i;
        }
    }
    settings_standby_close();
    lv_obj_t *layer = lv_display_get_layer_top(lv_obj_get_display(s_settings_parent));
    s_standby_overlay = lv_obj_create(layer);
    lv_obj_remove_style_all(s_standby_overlay);
    lv_obj_set_style_text_font(s_standby_overlay,
                              lv_obj_get_style_text_font(s_settings_parent, LV_PART_MAIN), 0);
    lv_obj_set_size(s_standby_overlay, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(s_standby_overlay, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_standby_overlay, LV_OPA_50, 0);
    lv_obj_add_flag(s_standby_overlay, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *card = lv_obj_create(s_standby_overlay);
    lv_obj_set_size(card, LV_PCT(72), LV_PCT(80));
    lv_obj_center(card);
    lv_obj_set_style_bg_color(card, C_SURFACE, 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(card, SETTINGS_ROW_PAD, 0);
    lv_obj_set_style_pad_row(card, SETTINGS_ROW_GAP, 0);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *title = lv_label_create(card);
    lv_label_set_text(title, L(mode == STANDBY_VIDEO ? DESKTOP_TEXT_SETTINGS_STANDBY_SELECT_VIDEO
                                                   : DESKTOP_TEXT_SETTINGS_STANDBY_SELECT_PHOTOS));
    lv_obj_set_style_text_color(title, C_INK, 0);
    s_standby_list = lv_obj_create(card);
    lv_obj_remove_style_all(s_standby_list);
    lv_obj_set_width(s_standby_list, LV_PCT(100));
    lv_obj_set_flex_grow(s_standby_list, 1);
    lv_obj_set_style_pad_row(s_standby_list, SETTINGS_ROW_GAP, 0);
    lv_obj_set_flex_flow(s_standby_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scroll_dir(s_standby_list, LV_DIR_VER);
    settings_standby_list_build();
    lv_obj_t *footer = lv_obj_create(card);
    lv_obj_remove_style_all(footer);
    lv_obj_set_size(footer, LV_PCT(100), SETTINGS_BUTTON_HEIGHT);
    lv_obj_set_flex_flow(footer, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(footer, SETTINGS_ROW_GAP, 0);
    lv_obj_t *cancel = settings_button(footer, L(DESKTOP_TEXT_SETTINGS_STANDBY_CANCEL), false);
    lv_obj_set_flex_grow(cancel, 1);
    lv_obj_set_height(cancel, SETTINGS_BUTTON_HEIGHT);
    lv_obj_add_event_cb(cancel, settings_standby_cancel_cb, LV_EVENT_CLICKED, NULL);
    s_standby_confirm = settings_button(footer, L(DESKTOP_TEXT_SETTINGS_STANDBY_CONFIRM), true);
    lv_obj_set_flex_grow(s_standby_confirm, 1);
    lv_obj_set_height(s_standby_confirm, SETTINGS_BUTTON_HEIGHT);
    lv_obj_add_event_cb(s_standby_confirm, settings_standby_confirm_cb, LV_EVENT_CLICKED, NULL);
    if (settings_standby_selection_count() == 0U)
        lv_obj_add_state(s_standby_confirm, LV_STATE_DISABLED);
}

static void settings_standby_mode_cb(lv_event_t *event)
{
    const standby_mode_t mode = (standby_mode_t)(uintptr_t)lv_event_get_user_data(event);
    if (mode == STANDBY_CLOCK) {
        s_standby_draft = *standby_prefs_get();
        s_standby_draft.mode = mode;
        if (standby_prefs_save(&s_standby_draft) != ESP_OK)
            settings_toast(L(DESKTOP_TEXT_SETTINGS_APPLY_FAILED));
        settings_standby_options_build();
    } else {
        settings_standby_open(mode);
    }
}

static void settings_standby_interval_cb(lv_event_t *event)
{
    const size_t index = (size_t)(uintptr_t)lv_event_get_user_data(event);
    if (index >= 4U) return;
    s_standby_draft = *standby_prefs_get();
    s_standby_draft.photo_interval_seconds = s_standby_intervals[index];
    if (standby_prefs_save(&s_standby_draft) != ESP_OK)
        settings_toast(L(DESKTOP_TEXT_SETTINGS_APPLY_FAILED));
    settings_standby_interval_build();
}

static void build_standby_rows(lv_obj_t *parent)
{
    lv_obj_t *row = settings_row(parent, L(DESKTOP_TEXT_SETTINGS_STANDBY));
    s_standby_options = lv_obj_create(row);
    lv_obj_remove_style_all(s_standby_options);
    lv_obj_set_size(s_standby_options, LV_PCT(100), SETTINGS_BUTTON_HEIGHT);
    lv_obj_align(s_standby_options, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_pad_column(s_standby_options, SETTINGS_SLEEP_OPTION_GAP, 0);
    lv_obj_set_flex_flow(s_standby_options, LV_FLEX_FLOW_ROW);
    settings_standby_options_build();

    row = settings_row(parent, L(DESKTOP_TEXT_SETTINGS_STANDBY_INTERVAL));
    s_standby_interval_options = lv_obj_create(row);
    lv_obj_remove_style_all(s_standby_interval_options);
    lv_obj_set_size(s_standby_interval_options, LV_PCT(100), SETTINGS_BUTTON_HEIGHT);
    lv_obj_align(s_standby_interval_options, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_pad_column(s_standby_interval_options, SETTINGS_SLEEP_OPTION_GAP, 0);
    lv_obj_set_flex_flow(s_standby_interval_options, LV_FLEX_FLOW_ROW);
    settings_standby_interval_build();
}

static void spectrum_lines_switch_cb(lv_event_t *event)
{
    lv_obj_t *sw = lv_event_get_target_obj(event);
    if (sw == NULL) {
        return;
    }
    const bool visible = lv_obj_has_state(sw, LV_STATE_CHECKED);
    if (spectrum_prefs_save_lines(visible) != ESP_OK) {
        settings_toast(L(DESKTOP_TEXT_SETTINGS_APPLY_FAILED));
    }
}

static void build_display_sound_column(lv_obj_t *parent)
{
    (void)settings_section_title(parent, L(DESKTOP_TEXT_SETTINGS_DISPLAY_SOUND));

    lv_obj_t *backlight_row = settings_row(parent, L(DESKTOP_TEXT_SETTINGS_BRIGHTNESS));
    lv_obj_set_height(backlight_row, SETTINGS_ROW_HEIGHT_COMPACT);
    s_backlight_value = settings_row_value(backlight_row);
    const uint8_t backlight = display_backlight_get();
    const int32_t backlight_level = backlight > 0U ? (int32_t)backlight
                                                  : (int32_t)DISPLAY_BACKLIGHT_DEFAULT_PERCENT;
    s_backlight_slider = settings_row_slider(backlight_row, (int32_t)DISPLAY_BACKLIGHT_MIN_PERCENT,
                                             (int32_t)DISPLAY_BACKLIGHT_MAX_PERCENT, backlight_level);
    settings_percent_label_set(s_backlight_value, backlight_level);
    lv_obj_add_event_cb(s_backlight_slider, backlight_slider_cb, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_event_cb(s_backlight_slider, backlight_slider_cb, LV_EVENT_RELEASED, NULL);

    uint8_t level = 0U;
    bool muted = false;
    const bool volume_known = music_player_service_get_volume(&level, &muted) == ESP_OK;
    lv_obj_t *volume_row = settings_row(parent, L(DESKTOP_TEXT_SETTINGS_VOLUME));
    lv_obj_set_height(volume_row, SETTINGS_ROW_HEIGHT_COMPACT);
    s_volume_value = settings_row_value(volume_row);
    s_volume_slider = settings_row_slider(volume_row, SETTINGS_VOLUME_MIN, SETTINGS_VOLUME_MAX,
                                          volume_known ? (int32_t)level : 0);
    settings_percent_label_set(s_volume_value, volume_known ? (int32_t)level : 0);
    lv_obj_add_event_cb(s_volume_slider, volume_slider_cb, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_event_cb(s_volume_slider, volume_slider_cb, LV_EVENT_RELEASED, NULL);

    lv_obj_t *mute_row = settings_row(parent, L(DESKTOP_TEXT_SETTINGS_MUTE));
    lv_obj_set_height(mute_row, SETTINGS_ROW_HEIGHT_INLINE);
    s_mute_switch = settings_row_switch(mute_row, volume_known && muted);
    lv_obj_align(s_mute_switch, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_add_event_cb(s_mute_switch, mute_switch_cb, LV_EVENT_VALUE_CHANGED, NULL);

    lv_obj_t *lines_row = settings_row(parent, L(DESKTOP_TEXT_SETTINGS_SPECTRUM_LINES));
    lv_obj_set_height(lines_row, SETTINGS_ROW_HEIGHT_INLINE);

    bool lines_visible = true;
    (void)spectrum_prefs_load_lines(&lines_visible);
    lv_obj_t *lines_switch = settings_row_switch(lines_row, lines_visible);
    lv_obj_align(lines_switch, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_add_event_cb(lines_switch, spectrum_lines_switch_cb, LV_EVENT_VALUE_CHANGED, NULL);
    build_sleep_row(parent);
    build_standby_rows(parent);
}

/**
 * @brief Apply the selected font and notify the shell to rebuild its labels.
 * @param event LVGL event that triggered the callback.
 */
static void settings_font_option_cb(lv_event_t *event)
{
    const size_t index = (size_t)(uintptr_t)lv_event_get_user_data(event);
    const char *chosen = NULL;
    if (index > 0U) {
        if (index > s_font_name_count) {
            return;
        }
        chosen = s_font_names[index - 1U];
    }

    const esp_err_t result = desktop_font_select_sd(chosen);
    if (s_font_cb != NULL) {
        s_font_cb(chosen, result == ESP_OK);
    }
}

static void settings_font_options_build(lv_obj_t *container)
{
    if (container == NULL) {
        return;
    }
    lv_obj_clean(container);

    const char *selected = desktop_font_selected_sd();
    lv_obj_t *builtin = settings_button(container, L(DESKTOP_TEXT_SETTINGS_FONT_BUILTIN),
                                        selected == NULL);
    lv_obj_set_width(builtin, LV_PCT(100));
    lv_obj_set_height(builtin, SETTINGS_BUTTON_HEIGHT);
    lv_obj_add_event_cb(builtin, settings_font_option_cb, LV_EVENT_CLICKED, NULL);

    s_font_name_count = desktop_font_scan_sd(s_font_names, DESKTOP_FONT_SD_LIST_MAX);
    for (size_t i = 0; i < s_font_name_count; ++i) {
        lv_obj_t *button = settings_button(
            container, s_font_names[i],
            selected != NULL && strcmp(selected, s_font_names[i]) == 0);
        lv_obj_set_width(button, LV_PCT(100));
        lv_obj_set_height(button, SETTINGS_BUTTON_HEIGHT);
        lv_obj_add_event_cb(button, settings_font_option_cb, LV_EVENT_CLICKED,
                            (void *)(uintptr_t)(i + 1U));
    }

    if (s_font_name_count == 0U) {
        lv_obj_t *notice = lv_label_create(container);
        lv_label_set_text(notice, L(DESKTOP_TEXT_SETTINGS_FONT_EMPTY));
        lv_obj_set_style_text_color(notice, C_INK_MUTED, 0);
        lv_obj_set_width(notice, LV_PCT(100));
        lv_label_set_long_mode(notice, LV_LABEL_LONG_WRAP);
    }
}

static void settings_font_row_cb(lv_event_t *event)
{
    LV_UNUSED(event);
    if (s_font_overlay == NULL) {
        return;
    }
    settings_font_options_build(s_font_list);
    lv_obj_remove_flag(s_font_overlay, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_font_overlay);
}

static void settings_font_overlay_cb(lv_event_t *event)
{
    if (lv_event_get_target_obj(event) != s_font_overlay) {
        return;
    }
    lv_obj_add_flag(s_font_overlay, LV_OBJ_FLAG_HIDDEN);
}

static void build_font_row(lv_obj_t *parent)
{
    lv_obj_t *row = settings_row(parent, L(DESKTOP_TEXT_SETTINGS_FONT));
    lv_obj_set_height(row, SETTINGS_ROW_HEIGHT_INLINE);
    lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(row, settings_font_row_cb, LV_EVENT_CLICKED, NULL);

    const char *selected = desktop_font_selected_sd();
    lv_obj_t *value = settings_row_value(row);
    lv_obj_set_width(value, LV_PCT(60));
    lv_obj_set_style_text_align(value, LV_TEXT_ALIGN_RIGHT, 0);
    lv_label_set_long_mode(value, LV_LABEL_LONG_DOT);
    lv_label_set_text(value, selected != NULL ? selected : L(DESKTOP_TEXT_SETTINGS_FONT_BUILTIN));
    lv_obj_align(value, LV_ALIGN_RIGHT_MID, 0, 0);
}

static void build_font_overlay(lv_obj_t *parent)
{
    const int32_t pad_left = lv_obj_get_style_pad_left(parent, LV_PART_MAIN);
    const int32_t pad_right = lv_obj_get_style_pad_right(parent, LV_PART_MAIN);
    const int32_t pad_top = lv_obj_get_style_pad_top(parent, LV_PART_MAIN);
    const int32_t pad_bottom = lv_obj_get_style_pad_bottom(parent, LV_PART_MAIN);

    s_font_overlay = lv_obj_create(parent);
    lv_obj_remove_style_all(s_font_overlay);
    lv_obj_set_style_bg_color(s_font_overlay, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(s_font_overlay, LV_OPA_50, 0);
    lv_obj_set_style_pad_all(s_font_overlay, 0, 0);
    lv_obj_set_size(s_font_overlay, LV_PCT(100) + pad_left + pad_right,
                    LV_PCT(100) + pad_top + pad_bottom);
    lv_obj_set_pos(s_font_overlay, -pad_left, -pad_top);
    lv_obj_add_flag(s_font_overlay, LV_OBJ_FLAG_HIDDEN | LV_OBJ_FLAG_IGNORE_LAYOUT);
    lv_obj_clear_flag(s_font_overlay, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(s_font_overlay, settings_font_overlay_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *card = lv_obj_create(s_font_overlay);
    lv_obj_remove_style_all(card);
    lv_obj_set_style_bg_color(card, C_SURFACE, 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(card, 14, 0);
    lv_obj_set_style_pad_all(card, SETTINGS_ROW_PAD, 0);
    lv_obj_set_style_pad_row(card, SETTINGS_ROW_GAP, 0);
    lv_obj_set_size(card, LV_PCT(SETTINGS_FONT_CARD_WIDTH_PERCENT),
                    LV_PCT(SETTINGS_FONT_CARD_HEIGHT_PERCENT));
    lv_obj_center(card);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);

    lv_obj_t *title = lv_label_create(card);
    lv_label_set_text(title, L(DESKTOP_TEXT_SETTINGS_FONT_SELECT));
    lv_obj_set_style_text_color(title, C_INK, 0);
    lv_obj_set_height(title, LV_SIZE_CONTENT);

    s_font_list = lv_obj_create(card);
    lv_obj_remove_style_all(s_font_list);
    lv_obj_set_style_bg_opa(s_font_list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_all(s_font_list, 0, 0);
    lv_obj_set_style_pad_row(s_font_list, SETTINGS_FONT_OPTION_GAP, 0);
    lv_obj_set_width(s_font_list, LV_PCT(100));
    lv_obj_set_flex_grow(s_font_list, 1);
    lv_obj_set_flex_flow(s_font_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scroll_dir(s_font_list, LV_DIR_VER);
}

static void settings_time_source_build(lv_obj_t *container);

/**
 * @brief Request synchronization through the shared network time service.
 * @param event LVGL event that triggered the callback.
 */
static void settings_time_sync_cb(lv_event_t *event)
{
    LV_UNUSED(event);
    const esp_err_t started = time_service_request_sync();
    if (started == ESP_ERR_INVALID_STATE) {
        settings_toast(L(DESKTOP_TEXT_SETTINGS_TIME_SYNC_OFFLINE));
        return;
    }
    settings_toast(started == ESP_OK ? L(DESKTOP_TEXT_SETTINGS_TIME_SYNC_STARTED)
                                     : L(DESKTOP_TEXT_SETTINGS_APPLY_FAILED));
}

static void settings_time_source_cb(lv_event_t *event)
{
    const time_source_t source = (time_source_t)(uintptr_t)lv_event_get_user_data(event);
    const esp_err_t applied = time_service_set_time_source(source);

    settings_time_source_build(s_time_source_options);
    if (applied != ESP_OK) {
        settings_toast(source == TIME_SOURCE_RTC
                           ? L(DESKTOP_TEXT_SETTINGS_TIME_SOURCE_RTC_FAILED)
                           : L(DESKTOP_TEXT_SETTINGS_APPLY_FAILED));
        return;
    }
    char message[64];
    snprintf(message, sizeof(message), L(DESKTOP_TEXT_SETTINGS_TIME_SOURCE_SET),
             source == TIME_SOURCE_RTC ? L(DESKTOP_TEXT_SETTINGS_TIME_SOURCE_RTC)
                                       : L(DESKTOP_TEXT_SETTINGS_TIME_SOURCE_INTERNAL));
    settings_toast(message);
}

static void settings_time_source_build(lv_obj_t *container)
{
    if (container == NULL) {
        return;
    }
    lv_obj_clean(container);
    time_source_t current = TIME_SOURCE_DEFAULT;
    (void)time_service_get_time_source(&current);
    static const desktop_text_key_t keys[] = {
        DESKTOP_TEXT_SETTINGS_TIME_SOURCE_INTERNAL,
        DESKTOP_TEXT_SETTINGS_TIME_SOURCE_RTC,
    };
    for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); ++i) {
        lv_obj_t *button = settings_button(container, L(keys[i]), (time_source_t)i == current);
        lv_obj_set_flex_grow(button, 1);
        lv_obj_set_height(button, SETTINGS_BUTTON_HEIGHT);
        lv_obj_add_event_cb(button, settings_time_source_cb, LV_EVENT_CLICKED,
                            (void *)(uintptr_t)i);
    }
}

static void build_time_rows(lv_obj_t *parent)
{
    lv_obj_t *sync_row = settings_row(parent, L(DESKTOP_TEXT_SETTINGS_TIME_SYNC));
    lv_obj_t *sync_button = settings_button(sync_row, L(DESKTOP_TEXT_SETTINGS_TIME_SYNC_NOW), false);
    lv_obj_set_size(sync_button, LV_PCT(100), SETTINGS_BUTTON_HEIGHT);
    lv_obj_align(sync_button, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_add_event_cb(sync_button, settings_time_sync_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *source_row = settings_row(parent, L(DESKTOP_TEXT_SETTINGS_TIME_SOURCE));
    s_time_source_options = lv_obj_create(source_row);
    lv_obj_remove_style_all(s_time_source_options);
    lv_obj_set_style_bg_opa(s_time_source_options, LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_all(s_time_source_options, 0, 0);
    lv_obj_set_style_pad_column(s_time_source_options, SETTINGS_SLEEP_OPTION_GAP, 0);
    lv_obj_set_size(s_time_source_options, LV_PCT(100), SETTINGS_BUTTON_HEIGHT);
    lv_obj_align(s_time_source_options, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_flex_flow(s_time_source_options, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(s_time_source_options, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(s_time_source_options, LV_OBJ_FLAG_SCROLLABLE);
    settings_time_source_build(s_time_source_options);
}

static void build_language_connection_column(lv_obj_t *parent)
{
    (void)settings_section_title(parent, L(DESKTOP_TEXT_SETTINGS_LANGUAGE_CONNECTION));

    lv_obj_t *language_row = settings_row(parent, L(DESKTOP_TEXT_SETTINGS_LANGUAGE));
    const desktop_language_t current = desktop_language_current();
    const bool chinese = desktop_language_chinese_available();
    lv_obj_t *english = settings_button(language_row, L(DESKTOP_TEXT_SETTINGS_LANGUAGE_ENGLISH),
                                       current == DESKTOP_LANGUAGE_ENGLISH);
    lv_obj_set_size(english, LV_PCT(SETTINGS_BUTTON_PERCENT), SETTINGS_BUTTON_HEIGHT);
    lv_obj_align(english, LV_ALIGN_BOTTOM_LEFT, 0, 0);
    lv_obj_add_event_cb(english, language_button_cb, LV_EVENT_CLICKED,
                        (void *)(uintptr_t)DESKTOP_LANGUAGE_ENGLISH);

    if (chinese) {
        lv_obj_t *chinese_button =
            settings_button(language_row, L(DESKTOP_TEXT_SETTINGS_LANGUAGE_CHINESE),
                            current == DESKTOP_LANGUAGE_CHINESE);
        lv_obj_set_size(chinese_button, LV_PCT(SETTINGS_BUTTON_PERCENT), SETTINGS_BUTTON_HEIGHT);
        lv_obj_align(chinese_button, LV_ALIGN_BOTTOM_RIGHT, 0, 0);
        lv_obj_add_event_cb(chinese_button, language_button_cb, LV_EVENT_CLICKED,
                            (void *)(uintptr_t)DESKTOP_LANGUAGE_CHINESE);
    } else {
        lv_obj_set_height(language_row, SETTINGS_ROW_HEIGHT_LANGUAGE_NOTICE);
        lv_obj_set_width(english, LV_PCT(100));
        lv_obj_align(english, LV_ALIGN_BOTTOM_LEFT, 0, 0);

        lv_obj_t *notice = lv_label_create(language_row);
        lv_label_set_text(notice, L(DESKTOP_TEXT_SETTINGS_LANGUAGE_ENGLISH_ONLY));
        lv_obj_set_style_text_color(notice, C_INK_MUTED, 0);
        lv_obj_set_width(notice, LV_PCT(100));
        lv_label_set_long_mode(notice, LV_LABEL_LONG_WRAP);
        lv_obj_align(notice, LV_ALIGN_TOP_LEFT, 0, 24);
    }

    build_font_row(parent);

    lv_obj_t *bt_row = settings_row(parent, L(DESKTOP_TEXT_SETTINGS_BLUETOOTH));
    lv_obj_set_height(bt_row, SETTINGS_ROW_HEIGHT_INLINE);
    s_bt_switch = settings_row_switch(bt_row, music_player_service_get_bt_power());
    lv_obj_align(s_bt_switch, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_add_event_cb(s_bt_switch, bt_power_switch_cb, LV_EVENT_VALUE_CHANGED, NULL);

    build_time_rows(parent);
}

typedef struct {
    uint8_t bit;
    desktop_text_key_t label_key;
} settings_weather_field_t;

static const settings_weather_field_t s_weather_field_defs[SETTINGS_WEATHER_FIELD_COUNT] = {
    {WEATHER_FIELD_PLACE, DESKTOP_TEXT_SETTINGS_WEATHER_PLACE},
    {WEATHER_FIELD_CONDITION_ICON, DESKTOP_TEXT_SETTINGS_WEATHER_CONDITION_ICON},
    {WEATHER_FIELD_CONDITION_TEXT, DESKTOP_TEXT_SETTINGS_WEATHER_CONDITION_TEXT},
    {WEATHER_FIELD_TEMPERATURE, DESKTOP_TEXT_SETTINGS_WEATHER_TEMPERATURE},
    {WEATHER_FIELD_HUMIDITY, DESKTOP_TEXT_SETTINGS_WEATHER_HUMIDITY},
    {WEATHER_FIELD_WIND_ARROW, DESKTOP_TEXT_SETTINGS_WEATHER_WIND_ARROW},
    {WEATHER_FIELD_WIND_TEXT, DESKTOP_TEXT_SETTINGS_WEATHER_WIND_TEXT},
    {WEATHER_FIELD_WIND_SPEED, DESKTOP_TEXT_SETTINGS_WEATHER_WIND_SPEED},
};

static void settings_weather_fields_build(lv_obj_t *container);

static void settings_weather_field_cb(lv_event_t *event)
{
    const uint8_t bit = (uint8_t)(uintptr_t)lv_event_get_user_data(event);
    weather_config_t config;
    if (weather_service_get_config(&config) != ESP_OK) {
        settings_toast(L(DESKTOP_TEXT_SETTINGS_APPLY_FAILED));
        return;
    }
    config.fields = (uint8_t)(config.fields ^ bit) & WEATHER_FIELD_ALL;
    if (weather_service_set_config(&config) != ESP_OK) {
        settings_toast(L(DESKTOP_TEXT_SETTINGS_APPLY_FAILED));
    }

    settings_weather_fields_build(s_weather_fields);
}

static void settings_weather_fields_build(lv_obj_t *container)
{
    if (container == NULL) {
        return;
    }
    lv_obj_clean(container);

    weather_config_t config;
    const uint8_t fields = weather_service_get_config(&config) == ESP_OK
                               ? config.fields
                               : (uint8_t)WEATHER_FIELD_ALL;
    for (size_t i = 0; i < SETTINGS_WEATHER_FIELD_COUNT; ++i) {
        lv_obj_t *button = settings_button(container, L(s_weather_field_defs[i].label_key),
                                           (fields & s_weather_field_defs[i].bit) != 0U);
        lv_obj_set_width(button, LV_PCT(SETTINGS_WEATHER_FIELD_WIDTH_PERCENT));
        lv_obj_set_height(button, SETTINGS_WEATHER_FIELD_HEIGHT);
        lv_obj_add_event_cb(button, settings_weather_field_cb, LV_EVENT_CLICKED,
                            (void *)(uintptr_t)s_weather_field_defs[i].bit);
    }
}

/**
 * @brief Build the local weather configuration URL from an available IPv4 address.
 * @param[out] out Buffer receiving the NUL-terminated URL.
 * @param capacity Buffer capacity in bytes.
 * @return true when an IPv4 address is available; false otherwise.
 */
static bool settings_weather_web_url(char *out, size_t capacity)
{
    char host[16] = {0};
    wifi_service_connection_info_t wifi = {0};
    if (wifi_service_get_connection_info(&wifi) && wifi.connected && wifi.ipv4[0] != '\0') {
        snprintf(host, sizeof(host), "%s", wifi.ipv4);
    } else {
        ethernet_service_status_t ethernet = {0};
        if (ethernet_service_get_status(&ethernet) == ESP_OK && ethernet.ip_valid) {
            (void)esp_ip4addr_ntoa(&ethernet.ip_info.ip, host, sizeof(host));
        }
    }
    if (host[0] == '\0') {
        return false;
    }
    snprintf(out, capacity, "http://%s/weather", host);
    return true;
}

static void settings_weather_overlay_cb(lv_event_t *event)
{
    if (lv_event_get_target_obj(event) != s_weather_overlay) {
        return;
    }
    lv_obj_add_flag(s_weather_overlay, LV_OBJ_FLAG_HIDDEN);
}

static void settings_weather_web_cb(lv_event_t *event)
{
    LV_UNUSED(event);
    if (s_weather_overlay == NULL) {
        return;
    }

    char url[SETTINGS_WEATHER_URL_MAX];
    const bool online = settings_weather_web_url(url, sizeof(url));
    if (s_weather_qr != NULL) {
        if (online) {
            (void)lv_qrcode_update(s_weather_qr, url, strlen(url));
        }

        lv_obj_set_flag(s_weather_qr, LV_OBJ_FLAG_HIDDEN, !online);
    }
    if (s_weather_url != NULL) {
        char text[SETTINGS_WEATHER_URL_MAX + 48];
        if (online) {
            snprintf(text, sizeof(text), L(DESKTOP_TEXT_SETTINGS_WEATHER_WEB_HINT), url);
        } else {
            snprintf(text, sizeof(text), "%s", L(DESKTOP_TEXT_SETTINGS_WEATHER_WEB_OFFLINE));
        }
        lv_label_set_text(s_weather_url, text);
    }
    lv_obj_remove_flag(s_weather_overlay, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_weather_overlay);
}

static void build_weather_overlay(lv_obj_t *parent)
{
    const int32_t pad_left = lv_obj_get_style_pad_left(parent, LV_PART_MAIN);
    const int32_t pad_right = lv_obj_get_style_pad_right(parent, LV_PART_MAIN);
    const int32_t pad_top = lv_obj_get_style_pad_top(parent, LV_PART_MAIN);
    const int32_t pad_bottom = lv_obj_get_style_pad_bottom(parent, LV_PART_MAIN);

    s_weather_overlay = lv_obj_create(parent);
    lv_obj_remove_style_all(s_weather_overlay);
    lv_obj_set_style_bg_color(s_weather_overlay, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(s_weather_overlay, LV_OPA_50, 0);
    lv_obj_set_style_pad_all(s_weather_overlay, 0, 0);
    lv_obj_set_size(s_weather_overlay, LV_PCT(100) + pad_left + pad_right,
                    LV_PCT(100) + pad_top + pad_bottom);
    lv_obj_set_pos(s_weather_overlay, -pad_left, -pad_top);
    lv_obj_add_flag(s_weather_overlay, LV_OBJ_FLAG_HIDDEN | LV_OBJ_FLAG_IGNORE_LAYOUT);
    lv_obj_clear_flag(s_weather_overlay, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(s_weather_overlay, settings_weather_overlay_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *card = lv_obj_create(s_weather_overlay);
    lv_obj_remove_style_all(card);
    lv_obj_set_style_bg_color(card, C_SURFACE, 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(card, 14, 0);
    lv_obj_set_style_pad_all(card, SETTINGS_ROW_PAD, 0);
    lv_obj_set_style_pad_row(card, SETTINGS_ROW_GAP, 0);

    lv_obj_set_size(card, SETTINGS_WEATHER_CARD_WIDTH, SETTINGS_WEATHER_CARD_HEIGHT);
    lv_obj_center(card);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(card, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);

    lv_obj_t *title = lv_label_create(card);
    lv_label_set_text(title, L(DESKTOP_TEXT_SETTINGS_WEATHER_WEB));
    lv_obj_set_style_text_color(title, C_INK, 0);
    lv_obj_set_height(title, LV_SIZE_CONTENT);

    s_weather_qr = lv_qrcode_create(card);
    lv_qrcode_set_size(s_weather_qr, SETTINGS_WEATHER_QR_SIZE);
    lv_qrcode_set_dark_color(s_weather_qr, C_INK);
    lv_qrcode_set_light_color(s_weather_qr, C_SURFACE);
    lv_qrcode_set_quiet_zone(s_weather_qr, true);

    s_weather_url = lv_label_create(card);
    lv_label_set_text(s_weather_url, "");
    lv_obj_set_style_text_color(s_weather_url, C_INK_MUTED, 0);
    lv_obj_set_width(s_weather_url, LV_PCT(100));
    lv_label_set_long_mode(s_weather_url, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(s_weather_url, LV_TEXT_ALIGN_CENTER, 0);
}

static void build_weather_section(lv_obj_t *parent)
{
    (void)settings_section_title(parent, L(DESKTOP_TEXT_SETTINGS_WEATHER));

    lv_obj_t *fields_row = settings_row(parent, L(DESKTOP_TEXT_SETTINGS_WEATHER_STATUS_BAR));
    lv_obj_set_height(fields_row, SETTINGS_ROW_HEIGHT_WEATHER);
    s_weather_fields = lv_obj_create(fields_row);
    lv_obj_remove_style_all(s_weather_fields);
    lv_obj_set_style_bg_opa(s_weather_fields, LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_all(s_weather_fields, 0, 0);
    lv_obj_set_style_pad_column(s_weather_fields, SETTINGS_WEATHER_FIELD_GAP, 0);
    lv_obj_set_style_pad_row(s_weather_fields, SETTINGS_WEATHER_FIELD_GAP, 0);
    lv_obj_set_width(s_weather_fields, LV_PCT(100));
    lv_obj_set_height(s_weather_fields, SETTINGS_WEATHER_ROWS * SETTINGS_WEATHER_FIELD_HEIGHT +
                                             (SETTINGS_WEATHER_ROWS - 1) *
                                                 SETTINGS_WEATHER_FIELD_GAP);
    lv_obj_align(s_weather_fields, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_flex_flow(s_weather_fields, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_align(s_weather_fields, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START);
    lv_obj_clear_flag(s_weather_fields, LV_OBJ_FLAG_SCROLLABLE);
    settings_weather_fields_build(s_weather_fields);

    lv_obj_t *web_row = settings_row(parent, L(DESKTOP_TEXT_SETTINGS_WEATHER_WEB));
    lv_obj_set_height(web_row, SETTINGS_ROW_HEIGHT_INLINE);
    lv_obj_add_flag(web_row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(web_row, settings_weather_web_cb, LV_EVENT_CLICKED, NULL);

    char url[SETTINGS_WEATHER_URL_MAX];
    const char *value = settings_weather_web_url(url, sizeof(url))
                            ? url
                            : L(DESKTOP_TEXT_SETTINGS_WEATHER_WEB_OFFLINE);
    lv_obj_t *value_label = settings_row_value(web_row);
    lv_obj_set_width(value_label, LV_PCT(70));
    lv_obj_set_style_text_align(value_label, LV_TEXT_ALIGN_RIGHT, 0);
    lv_label_set_long_mode(value_label, LV_LABEL_LONG_DOT);
    lv_label_set_text(value_label, value);
    lv_obj_align(value_label, LV_ALIGN_RIGHT_MID, 0, 0);
}

void app_settings_view_refresh_standby(void)
{
    settings_sleep_options_build(s_sleep_options);
    settings_standby_options_build();
    settings_standby_interval_build();
}

void app_settings_view_destroy(void)
{
    settings_standby_close();
    s_toast_cb = NULL;
    s_language_cb = NULL;
    s_font_cb = NULL;
    s_backlight_slider = NULL;
    s_backlight_value = NULL;
    s_volume_slider = NULL;
    s_volume_value = NULL;
    s_mute_switch = NULL;
    s_bt_switch = NULL;
    s_sleep_options = NULL;
    s_standby_options = NULL;
    s_standby_interval_options = NULL;
    s_settings_parent = NULL;
    s_time_source_options = NULL;
    s_font_overlay = NULL;
    s_font_list = NULL;
    s_weather_fields = NULL;
    s_weather_overlay = NULL;
    s_weather_qr = NULL;
    s_weather_url = NULL;
    s_font_name_count = 0U;
}

/**
 * @brief Create the settings columns, weather controls and selection overlays.
 * @param parent Parent LVGL object.
 * @param toast_cb Callback used to display messages.
 * @param language_cb Callback invoked after changing language.
 * @param font_cb Callback invoked after changing font.
 */
void app_settings_view_build(lv_obj_t *parent,
                               app_settings_view_toast_cb_t toast_cb,
                               app_settings_view_language_cb_t language_cb,
                               app_settings_view_font_cb_t font_cb)
{
    app_settings_view_destroy();
    s_toast_cb = toast_cb;
    s_language_cb = language_cb;
    s_font_cb = font_cb;
    s_settings_parent = parent;
    if (parent == NULL) {
        return;
    }

    const desktop_fullscreen_layout_colors_t colors = {
        .canvas = C_CANVAS,
        .surface = C_SURFACE,
        .ink = C_INK,
        .ink_muted = C_INK_MUTED,
    };
    desktop_fullscreen_layout_t layout;
    if (!desktop_fullscreen_layout_create(parent, &colors, "", "", "", &layout)) {
        return;
    }

    desktop_fullscreen_layout_remove_page_header(&layout);

    lv_obj_set_flex_flow(layout.content, LV_FLEX_FLOW_COLUMN);
    lv_obj_add_flag(layout.content, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(layout.content, LV_DIR_VER);

    lv_obj_set_scrollbar_mode(layout.content, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_style_pad_column(layout.content, 0, 0);
    lv_obj_set_style_pad_row(layout.content, SETTINGS_ROW_GAP, 0);

    lv_obj_t *columns = lv_obj_create(layout.content);
    lv_obj_remove_style_all(columns);
    lv_obj_set_style_bg_opa(columns, LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_all(columns, 0, 0);
    lv_obj_set_style_pad_column(columns, SETTINGS_COLUMN_GAP, 0);
    lv_obj_set_width(columns, LV_PCT(100));
    lv_obj_set_height(columns, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(columns, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(columns, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START);
    lv_obj_clear_flag(columns, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *display_sound = desktop_fullscreen_layout_create_panel(
        columns, C_CANVAS, SETTINGS_LEFT_PERCENT, 0, LV_FLEX_FLOW_COLUMN);
    lv_obj_t *language_connection =
        desktop_fullscreen_layout_create_panel(columns, C_CANVAS, 0, 0, LV_FLEX_FLOW_COLUMN);
    if (display_sound == NULL || language_connection == NULL) {
        return;
    }

    lv_obj_set_height(display_sound, LV_SIZE_CONTENT);
    lv_obj_set_height(language_connection, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_row(display_sound, SETTINGS_ROW_GAP, 0);
    lv_obj_set_style_pad_row(language_connection, SETTINGS_ROW_GAP, 0);

    build_display_sound_column(display_sound);
    build_language_connection_column(language_connection);

    lv_obj_t *weather = lv_obj_create(layout.content);
    lv_obj_remove_style_all(weather);
    lv_obj_set_style_bg_opa(weather, LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_all(weather, 0, 0);
    lv_obj_set_style_pad_row(weather, SETTINGS_ROW_GAP, 0);
    lv_obj_set_width(weather, LV_PCT(100));
    lv_obj_set_height(weather, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(weather, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(weather, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START);
    lv_obj_clear_flag(weather, LV_OBJ_FLAG_SCROLLABLE);
    build_weather_section(weather);

    build_font_overlay(parent);
    build_weather_overlay(parent);
}
