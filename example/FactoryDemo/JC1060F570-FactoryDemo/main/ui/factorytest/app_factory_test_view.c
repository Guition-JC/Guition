/**
 * SPDX-License-Identifier: GPL-2.0-only
 */
#include "app_factory_test_view.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_board_manager.h"
#include "driver/i2c_master.h"
#include "esp_rom_sys.h"
#include "ethernet_service.h"
#include "ft_audio_probe.h"
#include "ft_rtc_sequence.h"
#include "ft_screen_sequence.h"
#include "ft_sd_probe.h"
#include "factory_test_result.h"
#include "music_player_service.h"
#include "app_ft_touch_view.h"
#include "desktop_font.h"
#include "desktop_fullscreen_layout.h"
#include "desktop_language.h"
#include "wifi_service.h"
#include "lwip/inet.h"
#include "sdkconfig.h"

#define L(key) desktop_language_lookup(key)

#define FT_TAG "FactoryTest"

#define C_CANVAS      lv_color_hex(0xDCE8F5)
#define C_SURFACE     lv_color_hex(0xF7FAFD)
#define C_INK         lv_color_hex(0x122033)
#define C_INK_MUTED   lv_color_hex(0x53667B)
#define C_ACCENT      lv_color_hex(0x147EFB)
#define C_ACCENT_SOFT lv_color_hex(0xD9EAFF)
#define C_SUCCESS     lv_color_hex(0x22A96B)
#define C_DANGER      lv_color_hex(0xE05151)
#define C_WARNING     lv_color_hex(0xED9D33)
#define C_DIVIDER     lv_color_hex(0xD9E0E7)

#define FT_LEFT_PERCENT 58
#define FT_COLUMN_GAP 16
#define FT_ROW_GAP 8
#define FT_ROW_HEIGHT 54
#define FT_ROW_PAD 14
#define FT_SUBVIEW_PAD 18
#define FT_DURATION_FIELD_WIDTH 74
#define FT_ACTION_BUTTON_WIDTH 108
#define FT_DIALOG_WIDTH 460
#define FT_DIALOG_HEIGHT 200
#define FT_WIFI_POLL_MS 500
#define FT_AUDIO_POLL_MS 200
#define FT_RTC_POLL_MS 200
#define FT_RTC_TIMEOUT_MS 5000
#define FT_DETAIL_MAX 160

typedef enum {
    FT_ITEM_SCREEN,
    FT_ITEM_TOUCH,
    FT_ITEM_SD,
    FT_ITEM_ETHERNET,
    FT_ITEM_WIFI,
    FT_ITEM_AUDIO,
    FT_ITEM_RTC,
    FT_ITEM_COUNT,
} ft_item_t;

typedef enum {
    FT_VIEW_MENU = 0,
    FT_VIEW_SCREEN,
    FT_VIEW_TOUCH,
    FT_VIEW_SD,
    FT_VIEW_ETHERNET,
    FT_VIEW_WIFI,
    FT_VIEW_AUDIO,
    FT_VIEW_RTC,
} ft_view_t;

static const desktop_text_key_t s_item_name_keys[FT_ITEM_COUNT] = {
    DESKTOP_TEXT_FACTORY_ITEM_SCREEN,
    DESKTOP_TEXT_FACTORY_ITEM_TOUCH,
    DESKTOP_TEXT_FACTORY_ITEM_SD,
    DESKTOP_TEXT_FACTORY_ITEM_ETHERNET,
    DESKTOP_TEXT_FACTORY_ITEM_WIFI,
    DESKTOP_TEXT_FACTORY_ITEM_AUDIO,
    DESKTOP_TEXT_FACTORY_ITEM_RTC,
};

static lv_obj_t *s_parent;
static lv_obj_t *s_host;
static desktop_fullscreen_layout_t s_layout;
static lv_obj_t *s_stage;
static lv_obj_t *s_dialog;
static lv_obj_t *s_keyboard;
static lv_obj_t *s_duration_field;
static lv_obj_t *s_result_labels[FT_ITEM_COUNT];
static lv_obj_t *s_detail_label;
static lv_obj_t *s_wifi_status_label;
static lv_obj_t *s_eth_phy_label;
static lv_obj_t *s_eth_link_label;
static lv_obj_t *s_wifi_list;
static lv_obj_t *s_audio_state_label;
static lv_obj_t *s_audio_take_label;
static lv_obj_t *s_rtc_label;
static lv_obj_t *s_rtc_detail_label;
static i2c_master_dev_handle_t s_rtc_device;
static uint32_t s_rtc_started;
static ft_rtc_sequence_t s_rtc_sequence;
static lv_timer_t *s_screen_timer;
static lv_timer_t *s_poll_timer;
static factory_test_result_t s_results[FT_ITEM_COUNT];
static char s_detail[FT_DETAIL_MAX];
static ft_view_t s_view = FT_VIEW_MENU;

EXT_RAM_BSS_ATTR static wifi_service_ap_t s_wifi_aps[WIFI_SERVICE_MAX_APS];
static uint8_t s_screen_index;
static uint8_t s_screen_seconds = FT_SCREEN_DURATION_DEFAULT_SECONDS;
static bool s_wifi_rendered;
static int32_t s_full_width;
static int32_t s_full_height;
static app_factory_test_toast_cb_t s_toast_cb;
static app_factory_test_cover_cb_t s_cover_cb;
static void *s_cover_user_data;
static int32_t s_pad_left;
static int32_t s_pad_right;
static int32_t s_pad_top;
static int32_t s_pad_bottom;

static void ft_style_plain(lv_obj_t *object, lv_color_t background, int32_t radius)
{
    lv_obj_remove_style_all(object);
    lv_obj_set_style_bg_color(object, background, 0);
    lv_obj_set_style_bg_opa(object, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(object, radius, 0);
    lv_obj_set_style_border_width(object, 0, 0);
    lv_obj_set_style_shadow_width(object, 0, 0);
    lv_obj_set_style_pad_all(object, 0, 0);
}

static lv_obj_t *ft_label(lv_obj_t *parent, const char *text, lv_color_t color)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_label_set_text(label, text == NULL ? "" : text);
    lv_obj_set_style_text_color(label, color, 0);
    lv_obj_clear_flag(label, LV_OBJ_FLAG_CLICKABLE);
    return label;
}

static lv_obj_t *ft_button(lv_obj_t *parent, const char *text, lv_color_t background,
                           lv_event_cb_t callback, void *user_data)
{
    lv_obj_t *button = lv_button_create(parent);
    ft_style_plain(button, background, 10);
    lv_obj_set_style_bg_color(button, C_ACCENT_SOFT, LV_STATE_PRESSED);
    if (callback != NULL) {
        lv_obj_add_event_cb(button, callback, LV_EVENT_CLICKED, user_data);
    }
    lv_obj_t *caption = ft_label(button, text, C_INK);
    lv_obj_center(caption);
    return button;
}

static void ft_toast(const char *message)
{
    if (s_toast_cb != NULL) {
        s_toast_cb(message);
    }
}

/**
 * @brief Update the shell exit strip when entering or leaving a diagnostic view.
 * @param covered true when a diagnostic view covers the menu.
 */
static void ft_cover(bool covered)
{
    if (s_cover_cb != NULL) {
        s_cover_cb(covered, s_cover_user_data);
    }
}

static void ft_set_detail(const char *text)
{
    if (text == NULL) {
        s_detail[0] = '\0';
    } else {
        snprintf(s_detail, sizeof(s_detail), "%s", text);
    }
    if (s_detail_label != NULL) {
        lv_label_set_text(s_detail_label, s_detail);
    }
}

static const char *ft_result_text(factory_test_result_t result)
{
    switch (result) {
    case FACTORY_TEST_RESULT_RUNNING:
        return L(DESKTOP_TEXT_FACTORY_RESULT_RUNNING);
    case FACTORY_TEST_RESULT_PASS:
        return L(DESKTOP_TEXT_FACTORY_RESULT_PASS);
    case FACTORY_TEST_RESULT_FAIL:
        return L(DESKTOP_TEXT_FACTORY_RESULT_FAIL);
    case FACTORY_TEST_RESULT_RESERVED:
        return L(DESKTOP_TEXT_FACTORY_RESULT_RESERVED);
    case FACTORY_TEST_RESULT_PENDING:
    default:
        return L(DESKTOP_TEXT_FACTORY_RESULT_PENDING);
    }
}

static lv_color_t ft_result_color(factory_test_result_t result)
{
    switch (result) {
    case FACTORY_TEST_RESULT_PASS:
        return C_SUCCESS;
    case FACTORY_TEST_RESULT_FAIL:
        return C_DANGER;
    case FACTORY_TEST_RESULT_RESERVED:
        return C_WARNING;
    case FACTORY_TEST_RESULT_RUNNING:
        return C_ACCENT;
    case FACTORY_TEST_RESULT_PENDING:
    default:
        return C_INK_MUTED;
    }
}

static void ft_render_results(void)
{
    for (size_t index = 0U; index < FT_ITEM_COUNT; ++index) {
        if (s_result_labels[index] == NULL) {
            continue;
        }
        lv_label_set_text(s_result_labels[index], ft_result_text(s_results[index]));
        lv_obj_set_style_text_color(s_result_labels[index],
                                    ft_result_color(s_results[index]), 0);
    }
}

/**
 * @brief Use the full panel for diagnostics or restore the saved page padding.
 * @param full_bleed true to use the full panel; false to restore padding.
 */
static void ft_set_padding(bool full_bleed)
{
    if (s_parent == NULL) {
        return;
    }
    lv_obj_set_style_pad_left(s_parent, full_bleed ? 0 : s_pad_left, 0);
    lv_obj_set_style_pad_right(s_parent, full_bleed ? 0 : s_pad_right, 0);
    lv_obj_set_style_pad_top(s_parent, full_bleed ? 0 : s_pad_top, 0);
    lv_obj_set_style_pad_bottom(s_parent, full_bleed ? 0 : s_pad_bottom, 0);
}

static void ft_stop_timers(void)
{
    if (s_screen_timer != NULL) {
        lv_timer_delete(s_screen_timer);
        s_screen_timer = NULL;
    }
    if (s_poll_timer != NULL) {
        lv_timer_delete(s_poll_timer);
        s_poll_timer = NULL;
    }
    if (s_rtc_device != NULL) {
        i2c_master_bus_rm_device(s_rtc_device);
        s_rtc_device = NULL;
    }
}

static void ft_hide_dialog(void)
{
    if (s_dialog != NULL) {
        lv_obj_add_flag(s_dialog, LV_OBJ_FLAG_HIDDEN);
    }
}

static void ft_hide_keyboard(void)
{
    if (s_keyboard == NULL) {
        return;
    }
    lv_keyboard_set_textarea(s_keyboard, NULL);
    lv_obj_add_flag(s_keyboard, LV_OBJ_FLAG_HIDDEN);
}

/**
 * @brief Clear references to widgets deleted with the content container.
 */
static void ft_forget_menu_pointers(void)
{
    s_detail_label = NULL;

    s_wifi_status_label = NULL;
    s_eth_phy_label = NULL;
    s_eth_link_label = NULL;
    s_wifi_list = NULL;
    s_audio_state_label = NULL;
    s_audio_take_label = NULL;
    s_rtc_label = NULL;
    s_rtc_detail_label = NULL;
    for (size_t index = 0U; index < FT_ITEM_COUNT; ++index) {
        s_result_labels[index] = NULL;
    }
}

static void ft_clean_stage(void)
{
    if (s_stage != NULL) {
        lv_obj_clean(s_stage);
        lv_obj_add_flag(s_stage, LV_OBJ_FLAG_HIDDEN);
    }
}

static void ft_build_menu(void);

/**
 * @brief Stop audio work and restore the diagnostics menu.
 * @param event LVGL event that triggered the callback.
 */
static void ft_back_cb(lv_event_t *event)
{
    (void)event;

    ft_audio_probe_stop();
    ft_clean_stage();
    app_ft_touch_view_cleanup();
    ft_stop_timers();
    ft_set_padding(false);
    s_view = FT_VIEW_MENU;
    if (s_layout.content != NULL) {
        lv_obj_remove_flag(s_layout.content, LV_OBJ_FLAG_HIDDEN);
    }
    ft_build_menu();
    ft_cover(false);
}

static lv_obj_t *ft_begin_subview(desktop_text_key_t title_key)
{
    if (s_layout.content == NULL) {
        return NULL;
    }
    if (s_keyboard != NULL) {
        lv_obj_add_flag(s_keyboard, LV_OBJ_FLAG_HIDDEN);
    }
    lv_obj_clean(s_layout.content);
    ft_forget_menu_pointers();
    lv_obj_set_flex_flow(s_layout.content, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(s_layout.content, FT_COLUMN_GAP, 0);

    lv_obj_t *card = lv_obj_create(s_layout.content);
    ft_style_plain(card, C_SURFACE, 16);
    lv_obj_set_width(card, LV_PCT(100));
    lv_obj_set_height(card, LV_PCT(100));
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(card, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_all(card, FT_SUBVIEW_PAD, 0);
    lv_obj_set_style_pad_row(card, 12, 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *header = lv_obj_create(card);
    ft_style_plain(header, C_SURFACE, 0);
    lv_obj_set_width(header, LV_PCT(100));
    lv_obj_set_height(header, 48);
    lv_obj_set_flex_flow(header, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(header, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    ft_label(header, L(title_key), C_INK);

    lv_obj_t *back = ft_button(header, L(DESKTOP_TEXT_FACTORY_BACK), C_ACCENT_SOFT,
                               ft_back_cb, NULL);
    lv_obj_set_size(back, 160, 42);
    return card;
}

static void ft_open_view(ft_view_t view)
{
    s_view = view;
    ft_cover(true);
}

static void ft_screen_finish(void)
{
    ft_clean_stage();
    ft_stop_timers();
    ft_set_padding(false);
    s_view = FT_VIEW_MENU;
    if (s_layout.content != NULL) {
        lv_obj_remove_flag(s_layout.content, LV_OBJ_FLAG_HIDDEN);
    }
    ft_build_menu();
    ft_cover(false);
    ft_set_detail(L(DESKTOP_TEXT_FACTORY_SCREEN_DONE));
    s_results[FT_ITEM_SCREEN] = FACTORY_TEST_RESULT_PASS;
    ft_render_results();
}

static void ft_screen_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    if (s_stage == NULL || !ft_screen_sequence_advance(&s_screen_index)) {
        ft_screen_finish();
        return;
    }
    lv_obj_set_style_bg_color(s_stage, lv_color_hex(ft_screen_color_hex(s_screen_index)), 0);
}

static void ft_screen_long_press_cb(lv_event_t *event)
{
    (void)event;

    ft_screen_finish();
}

/**
 * @brief Display the timed fullscreen color sequence.
 */
static void ft_start_screen_view(void)
{
    if (s_stage == NULL || s_layout.content == NULL) {
        return;
    }
    if (s_screen_timer != NULL) {
        lv_timer_delete(s_screen_timer);
        s_screen_timer = NULL;
    }

    ft_open_view(FT_VIEW_SCREEN);
    lv_obj_add_flag(s_layout.content, LV_OBJ_FLAG_HIDDEN);
    ft_set_padding(true);

    s_screen_index = 0U;
    lv_obj_remove_flag(s_stage, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_style_bg_color(s_stage, lv_color_hex(ft_screen_color_hex(0U)), 0);
    lv_obj_move_foreground(s_stage);

    s_screen_timer = lv_timer_create(ft_screen_timer_cb,
                                     ft_screen_duration_ms(s_screen_seconds), NULL);
    if (s_screen_timer == NULL) {
        ESP_LOGE(FT_TAG, "screen timer allocation failed");
        ft_screen_finish();
    }
}

static void ft_touch_exit_cb(void *user_data)
{
    (void)user_data;
    app_ft_touch_view_cleanup();
    ft_clean_stage();
    ft_set_padding(false);
    s_view = FT_VIEW_MENU;
    if (s_layout.content != NULL) {
        lv_obj_remove_flag(s_layout.content, LV_OBJ_FLAG_HIDDEN);
    }
    ft_build_menu();
    ft_cover(false);
    ft_set_detail(L(DESKTOP_TEXT_FACTORY_TOUCH_DONE));
    s_results[FT_ITEM_TOUCH] = FACTORY_TEST_RESULT_PASS;
    ft_render_results();
}

/**
 * @brief Open the touch-dot diagnostic surface.
 */
static void ft_start_touch_view(void)
{
    if (s_parent == NULL || s_stage == NULL || s_layout.content == NULL) {
        return;
    }
    ft_open_view(FT_VIEW_TOUCH);
    lv_obj_add_flag(s_layout.content, LV_OBJ_FLAG_HIDDEN);
    ft_set_padding(true);

    if (!app_ft_touch_view_build(s_parent, s_full_width, s_full_height,
                                   ft_touch_exit_cb, NULL)) {
        ESP_LOGE(FT_TAG, "touch canvas startup failed");
        ft_toast(L(DESKTOP_TEXT_FACTORY_TOUCH_FAILED));
        if (s_layout.content != NULL) {
            lv_obj_remove_flag(s_layout.content, LV_OBJ_FLAG_HIDDEN);
        }
        ft_set_padding(false);
        s_view = FT_VIEW_MENU;
        ft_cover(false);
        s_results[FT_ITEM_TOUCH] = FACTORY_TEST_RESULT_FAIL;
        ft_render_results();
    }
}

static void ft_sd_render(const ft_sd_probe_report_t *report)
{
    char detail[FT_DETAIL_MAX];
    switch (report->status) {
    case FT_SD_STATUS_OK:
        s_results[FT_ITEM_SD] = FACTORY_TEST_RESULT_PASS;
        snprintf(detail, sizeof(detail), "%s %s",
                 report->mounted_before ? L(DESKTOP_TEXT_FACTORY_SD_ALREADY)
                                        : L(DESKTOP_TEXT_FACTORY_SD_MOUNTED_NOW),
                 report->card_name[0] != '\0' ? report->card_name : "");
        ft_set_detail(detail);
        break;
    case FT_SD_STATUS_MOUNT_FAILED:
        s_results[FT_ITEM_SD] = FACTORY_TEST_RESULT_FAIL;
        ft_set_detail(L(DESKTOP_TEXT_FACTORY_SD_MOUNT_FAILED));
        break;
    case FT_SD_STATUS_COMM_FAILED:
        s_results[FT_ITEM_SD] = FACTORY_TEST_RESULT_FAIL;
        ft_set_detail(L(DESKTOP_TEXT_FACTORY_SD_COMM_FAILED));
        break;
    case FT_SD_STATUS_UNAVAILABLE:
    default:
        s_results[FT_ITEM_SD] = FACTORY_TEST_RESULT_FAIL;
        ft_set_detail(L(DESKTOP_TEXT_FACTORY_SD_UNAVAILABLE));
        break;
    }
    if (report->status == FT_SD_STATUS_OK && report->capacity_bytes > 0U) {
        char capacity[FT_DETAIL_MAX];
        snprintf(capacity, sizeof(capacity), L(DESKTOP_TEXT_FACTORY_SD_CAPACITY),
                 (unsigned)(report->capacity_bytes / (1024U * 1024U)));
        ft_set_detail(capacity);
    }
    ft_render_results();
}

/**
 * @brief Run the SD probe; only unmount a card mounted by this probe.
 */
static void ft_start_sd_view(void)
{
    ft_open_view(FT_VIEW_SD);
    lv_obj_t *card = ft_begin_subview(DESKTOP_TEXT_FACTORY_ITEM_SD);
    if (card == NULL) {
        return;
    }

    ft_sd_probe_report_t report;
    ft_sd_probe_run(ft_sd_probe_default_ops(), &report);
    ESP_LOGI(FT_TAG, "sd probe: status=%d mounted_before=%d mounted_by_probe=%d error=%d",
             (int)report.status, (int)report.mounted_before, (int)report.mounted_by_probe,
             report.last_error);

    lv_obj_t *result = ft_label(card, "", C_INK);
    lv_obj_set_style_text_font(result, desktop_font_get_title(), 0);
    ft_sd_render(&report);
    lv_label_set_text(result, ft_result_text(s_results[FT_ITEM_SD]));
    lv_obj_set_style_text_color(result, ft_result_color(s_results[FT_ITEM_SD]), 0);
    ft_label(card, s_detail, C_INK_MUTED);
    ESP_LOGI(FT_TAG, "sd result: %s", factory_test_result_name(s_results[FT_ITEM_SD]));

    ft_sd_probe_release(ft_sd_probe_default_ops(), &report);
}

static void ft_sd_confirm_cb(lv_event_t *event)
{
    (void)event;
    ft_hide_dialog();
    ft_start_sd_view();
}

static void ft_sd_cancel_cb(lv_event_t *event)
{
    (void)event;

    ft_hide_dialog();
    ft_set_detail(L(DESKTOP_TEXT_FACTORY_SD_CANCELLED));
}

static void ft_show_sd_dialog(void)
{
    if (s_dialog == NULL) {
        return;
    }
    lv_obj_remove_flag(s_dialog, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_dialog);
}

#if CONFIG_ETHERNET_PHY_IP101
static void ft_ethernet_poll_cb(lv_timer_t *timer)
{
    (void)timer;
    if (s_eth_phy_label == NULL || s_eth_link_label == NULL) {
        return;
    }

    ethernet_service_status_t status = {0};
    const esp_err_t ret = ethernet_service_get_status(&status);
    const char *phy = L(DESKTOP_TEXT_FACTORY_ETH_PHY_WAIT);
    const char *link = L(DESKTOP_TEXT_FACTORY_ETH_LINK_WAIT);
    lv_color_t phy_color = C_ACCENT;
    lv_color_t link_color = C_INK_MUTED;
    factory_test_result_t result = FACTORY_TEST_RESULT_RUNNING;
    char ip[16];
    char link_text[64];

    if (ret != ESP_OK || (!status.phy_probe_complete && status.init_failure_count > 0)) {
        phy = L(DESKTOP_TEXT_FACTORY_ETH_INIT_FAILED);
        phy_color = C_DANGER;
        result = FACTORY_TEST_RESULT_FAIL;
    } else if (status.phy_probe_complete) {
        phy = L(status.phy_communication_ok ? DESKTOP_TEXT_FACTORY_ETH_PHY_OK
                                             : DESKTOP_TEXT_FACTORY_ETH_PHY_FAILED);
        phy_color = status.phy_communication_ok ? C_SUCCESS : C_DANGER;
        if (!status.phy_communication_ok) {
            result = FACTORY_TEST_RESULT_FAIL;
        }
    }

    if (status.initialized) {
        if (!status.link_up) {
            link = L(DESKTOP_TEXT_FACTORY_ETH_LINK_DOWN);
        } else if (!status.ip_valid || status.ip_info.ip.addr == 0) {
            link = L(DESKTOP_TEXT_FACTORY_ETH_DHCP_WAIT);
        } else {
            snprintf(ip, sizeof(ip), IPSTR, IP2STR(&status.ip_info.ip));
            snprintf(link_text, sizeof(link_text), L(DESKTOP_TEXT_FACTORY_ETH_IP), ip);
            link = link_text;
            link_color = C_SUCCESS;
            if (status.phy_probe_complete && status.phy_communication_ok) {
                result = FACTORY_TEST_RESULT_PASS;
            }
        }
    }
    lv_label_set_text(s_eth_phy_label, phy);
    lv_obj_set_style_text_color(s_eth_phy_label, phy_color, 0);
    lv_label_set_text(s_eth_link_label, link);
    lv_obj_set_style_text_color(s_eth_link_label, link_color, 0);
    s_results[FT_ITEM_ETHERNET] = result;
    ft_set_detail(link);
    ft_render_results();
}

/**
 * @brief Start Ethernet diagnostics and periodic status refresh.
 */
static void ft_start_ethernet_view(void)
{
    ft_open_view(FT_VIEW_ETHERNET);
    lv_obj_t *card = ft_begin_subview(DESKTOP_TEXT_FACTORY_ITEM_ETHERNET);
    if (card == NULL) {
        return;
    }
    s_eth_phy_label = ft_label(card, L(DESKTOP_TEXT_FACTORY_ETH_PHY_WAIT), C_ACCENT);
    s_eth_link_label = ft_label(card, L(DESKTOP_TEXT_FACTORY_ETH_LINK_WAIT), C_INK_MUTED);
    ft_ethernet_poll_cb(NULL);
    s_poll_timer = lv_timer_create(ft_ethernet_poll_cb, FT_WIFI_POLL_MS, NULL);
    if (s_poll_timer == NULL) {
        ESP_LOGE(FT_TAG, "Ethernet poll timer allocation failed");
        s_results[FT_ITEM_ETHERNET] = FACTORY_TEST_RESULT_FAIL;
    }
}
#endif

static void ft_wifi_render(size_t count)
{
    if (s_wifi_list == NULL) {
        return;
    }
    lv_obj_clean(s_wifi_list);

    if (count == 0U) {
        if (s_wifi_status_label != NULL) {
            lv_label_set_text(s_wifi_status_label, L(DESKTOP_TEXT_FACTORY_WIFI_EMPTY));
        }
        s_results[FT_ITEM_WIFI] = FACTORY_TEST_RESULT_FAIL;
        ft_render_results();
        return;
    }

    char detail[FT_DETAIL_MAX];
    if (s_wifi_status_label != NULL) {
        snprintf(detail, sizeof(detail), L(DESKTOP_TEXT_FACTORY_WIFI_COUNT), (unsigned)count);
        lv_label_set_text(s_wifi_status_label, detail);
    }
    for (size_t index = 0U; index < count; ++index) {
        lv_obj_t *row = lv_obj_create(s_wifi_list);
        ft_style_plain(row, C_CANVAS, 10);
        lv_obj_set_width(row, LV_PCT(100));
        lv_obj_set_height(row, 44);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

        lv_obj_t *name = ft_label(row, s_wifi_aps[index].ssid[0] != '\0'
                                           ? s_wifi_aps[index].ssid
                                           : L(DESKTOP_TEXT_FACTORY_WIFI_HIDDEN),
                                  C_INK);
        lv_obj_align(name, LV_ALIGN_LEFT_MID, 12, 0);

        char meta[48];
        snprintf(meta, sizeof(meta), L(DESKTOP_TEXT_FACTORY_WIFI_META),
                 (int)s_wifi_aps[index].rssi, (unsigned)s_wifi_aps[index].channel);
        lv_obj_t *meta_label = ft_label(row, meta, C_INK_MUTED);
        lv_obj_align(meta_label, LV_ALIGN_RIGHT_MID, -12, 0);
    }

    s_results[FT_ITEM_WIFI] = FACTORY_TEST_RESULT_PASS;
    ft_render_results();
}

static void ft_wifi_poll_cb(lv_timer_t *timer)
{
    (void)timer;
    if (s_wifi_list == NULL) {
        return;
    }
    bool scanning = false;
    const size_t count = wifi_service_get_scan_snapshot(s_wifi_aps, WIFI_SERVICE_MAX_APS,
                                                        &scanning);
    if (scanning) {
        if (s_wifi_status_label != NULL) {
            lv_label_set_text(s_wifi_status_label, L(DESKTOP_TEXT_FACTORY_WIFI_SCANNING));
        }
        return;
    }
    if (s_wifi_rendered) {
        return;
    }
    s_wifi_rendered = true;
    ft_wifi_render(count);
}

/**
 * @brief Start the Wi-Fi driver and scan for nearby networks.
 */
static void ft_start_wifi_view(void)
{
    ft_open_view(FT_VIEW_WIFI);
    lv_obj_t *card = ft_begin_subview(DESKTOP_TEXT_FACTORY_ITEM_WIFI);
    if (card == NULL) {
        return;
    }

    s_wifi_status_label = ft_label(card, L(DESKTOP_TEXT_FACTORY_WIFI_SCANNING), C_ACCENT);
    s_wifi_list = lv_obj_create(card);
    ft_style_plain(s_wifi_list, C_SURFACE, 0);
    lv_obj_set_width(s_wifi_list, LV_PCT(100));
    lv_obj_set_flex_grow(s_wifi_list, 1);
    lv_obj_set_flex_flow(s_wifi_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(s_wifi_list, 6, 0);
    lv_obj_clear_flag(s_wifi_list, LV_OBJ_FLAG_SCROLLABLE);

    s_wifi_rendered = false;

    if (!wifi_service_is_started()) {
        (void)wifi_service_start();
    }
    const esp_err_t ret = wifi_service_start_scan();
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(FT_TAG, "wifi scan start failed: %s", esp_err_to_name(ret));
        if (s_wifi_status_label != NULL) {
            lv_label_set_text(s_wifi_status_label, L(DESKTOP_TEXT_FACTORY_WIFI_FAILED));
        }
        s_results[FT_ITEM_WIFI] = FACTORY_TEST_RESULT_FAIL;
        ft_render_results();
    }

    if (s_poll_timer != NULL) {
        lv_timer_delete(s_poll_timer);
    }
    s_poll_timer = lv_timer_create(ft_wifi_poll_cb, FT_WIFI_POLL_MS, NULL);
    if (s_poll_timer == NULL) {
        ESP_LOGE(FT_TAG, "wifi poll timer allocation failed");
    }
}

static void ft_audio_render(void)
{
    const ft_audio_probe_state_t state = ft_audio_probe_state();
    if (s_audio_state_label != NULL) {
        const char *text = L(DESKTOP_TEXT_FACTORY_AUDIO_IDLE);
        if (state == FT_AUDIO_RECORDING) {
            text = L(DESKTOP_TEXT_FACTORY_AUDIO_RECORDING);
        } else if (state == FT_AUDIO_PLAYING) {
            text = L(DESKTOP_TEXT_FACTORY_AUDIO_PLAYING);
        }
        lv_label_set_text(s_audio_state_label, text);
    }
    if (s_audio_take_label != NULL) {
        lv_label_set_text(s_audio_take_label,
                          ft_audio_probe_has_recording() ? L(DESKTOP_TEXT_FACTORY_AUDIO_TAKE_READY)
                                                         : L(DESKTOP_TEXT_FACTORY_AUDIO_NO_TAKE));
    }
}

static void ft_audio_stop_polling(void)
{
    if (s_poll_timer != NULL) {
        lv_timer_delete(s_poll_timer);
        s_poll_timer = NULL;
    }
}

static void ft_audio_poll_cb(lv_timer_t *timer)
{
    (void)timer;
    if (ft_audio_probe_state() != FT_AUDIO_IDLE) {
        ft_audio_render();
        return;
    }

    ft_audio_stop_polling();
    ft_audio_render();
    const esp_err_t error = ft_audio_probe_last_error();
    if (error == ESP_OK) {
        s_results[FT_ITEM_AUDIO] = FACTORY_TEST_RESULT_PASS;
        ft_set_detail(L(DESKTOP_TEXT_FACTORY_AUDIO_DONE));
    } else {
        s_results[FT_ITEM_AUDIO] = FACTORY_TEST_RESULT_FAIL;
        ft_set_detail(L(DESKTOP_TEXT_FACTORY_AUDIO_FAILED));
    }
    ft_render_results();
}

static void ft_audio_start_polling(void)
{
    ft_audio_stop_polling();
    s_poll_timer = lv_timer_create(ft_audio_poll_cb, FT_AUDIO_POLL_MS, NULL);
    if (s_poll_timer == NULL) {
        ESP_LOGE(FT_TAG, "audio poll timer allocation failed");
    }
}

/**
 * @brief Release music playback before using the shared codec for recording.
 * @param event LVGL event that triggered the callback.
 */
static void ft_audio_record_cb(lv_event_t *event)
{
    (void)event;

    (void)music_player_service_return_home();
    const esp_err_t ret = ft_audio_probe_record_start();
    if (ret != ESP_OK) {
        ft_toast(L(DESKTOP_TEXT_FACTORY_AUDIO_FAILED));
        s_results[FT_ITEM_AUDIO] = FACTORY_TEST_RESULT_FAIL;
        ft_render_results();
        return;
    }
    s_results[FT_ITEM_AUDIO] = FACTORY_TEST_RESULT_RUNNING;
    ft_render_results();
    ft_audio_render();
    ft_audio_start_polling();
}

static void ft_audio_play_cb(lv_event_t *event)
{
    (void)event;
    if (!ft_audio_probe_has_recording()) {
        ft_toast(L(DESKTOP_TEXT_FACTORY_AUDIO_NO_TAKE));
        ft_set_detail(L(DESKTOP_TEXT_FACTORY_AUDIO_NO_TAKE));
        return;
    }
    (void)music_player_service_return_home();
    const esp_err_t ret = ft_audio_probe_play_start();
    if (ret != ESP_OK) {
        ft_toast(L(DESKTOP_TEXT_FACTORY_AUDIO_FAILED));
        s_results[FT_ITEM_AUDIO] = FACTORY_TEST_RESULT_FAIL;
        ft_render_results();
        return;
    }
    s_results[FT_ITEM_AUDIO] = FACTORY_TEST_RESULT_RUNNING;
    ft_render_results();
    ft_audio_render();
    ft_audio_start_polling();
}

static void ft_start_audio_view(void)
{
    ft_open_view(FT_VIEW_AUDIO);
    lv_obj_t *card = ft_begin_subview(DESKTOP_TEXT_FACTORY_ITEM_AUDIO);
    if (card == NULL) {
        return;
    }

    lv_obj_t *buttons = lv_obj_create(card);
    ft_style_plain(buttons, C_SURFACE, 0);
    lv_obj_set_width(buttons, LV_PCT(100));
    lv_obj_set_height(buttons, 56);
    lv_obj_set_flex_flow(buttons, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(buttons, 12, 0);
    lv_obj_clear_flag(buttons, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *record = ft_button(buttons, L(DESKTOP_TEXT_FACTORY_AUDIO_RECORD), C_ACCENT_SOFT,
                                 ft_audio_record_cb, NULL);
    lv_obj_set_size(record, 200, 52);
    lv_obj_t *play = ft_button(buttons, L(DESKTOP_TEXT_FACTORY_AUDIO_PLAY), C_ACCENT_SOFT,
                               ft_audio_play_cb, NULL);
    lv_obj_set_size(play, 200, 52);

    s_audio_state_label = ft_label(card, "", C_INK);
    s_audio_take_label = ft_label(card, "", C_INK_MUTED);
    ft_audio_render();
}

static void ft_rtc_finish(esp_err_t error)
{
    const bool passed = error == ESP_OK;
    s_results[FT_ITEM_RTC] = passed ? FACTORY_TEST_RESULT_PASS : FACTORY_TEST_RESULT_FAIL;
    char detail[FT_DETAIL_MAX];
    if (passed) {
        snprintf(detail, sizeof(detail), "%s (%02u)", L(DESKTOP_TEXT_FACTORY_RTC_OK),
                 (unsigned)s_rtc_sequence.second);
    } else {
        snprintf(detail, sizeof(detail), "%s (%s)", L(DESKTOP_TEXT_FACTORY_RTC_FAILED),
                 esp_err_to_name(error));
    }
    ft_set_detail(detail);
    lv_label_set_text(s_rtc_detail_label, detail);
    lv_label_set_text(s_rtc_label, ft_result_text(s_results[FT_ITEM_RTC]));
    lv_obj_set_style_text_color(s_rtc_label, ft_result_color(s_results[FT_ITEM_RTC]), 0);
    ft_render_results();
    ft_stop_timers();
}

static void ft_rtc_poll_cb(lv_timer_t *timer)
{
    (void)timer;
    const uint8_t command = 0; /* RX8025 seconds register, address shifted left by four. */
    uint8_t raw = 0;
    esp_err_t ret = i2c_master_transmit_receive(s_rtc_device, &command, 1, &raw, 1, 100);
    esp_rom_delay_us(100);
    if (ret != ESP_OK) {
        ft_rtc_finish(ret);
        return;
    }
    if (!ft_rtc_sequence_sample(&s_rtc_sequence, raw)) {
        ft_rtc_finish(ESP_ERR_INVALID_RESPONSE);
        return;
    }
    char detail[FT_DETAIL_MAX];
    snprintf(detail, sizeof(detail), "RX8025: %02u", (unsigned)s_rtc_sequence.second);
    lv_label_set_text(s_rtc_detail_label, detail);
    if (s_rtc_sequence.transitions >= 2U) {
        ft_rtc_finish(ESP_OK);
    } else if (lv_tick_elaps(s_rtc_started) >= FT_RTC_TIMEOUT_MS) {
        ft_rtc_finish(ESP_ERR_TIMEOUT);
    }
}

/** @brief Verify the seconds register advances without relying on calendar or status registers. */
static void ft_start_rtc_view(void)
{
    ft_open_view(FT_VIEW_RTC);
    lv_obj_t *card = ft_begin_subview(DESKTOP_TEXT_FACTORY_ITEM_RTC);
    if (card == NULL) {
        return;
    }

    s_rtc_label = ft_label(card, "", C_INK);
    lv_obj_set_style_text_font(s_rtc_label, desktop_font_get_title(), 0);
    s_rtc_detail_label = ft_label(card, "", C_INK_MUTED);
    s_results[FT_ITEM_RTC] = FACTORY_TEST_RESULT_RUNNING;
    lv_label_set_text(s_rtc_label, ft_result_text(s_results[FT_ITEM_RTC]));
    lv_obj_set_style_text_color(s_rtc_label, ft_result_color(s_results[FT_ITEM_RTC]), 0);
    ft_render_results();

    void *bus = NULL;
    esp_err_t ret = esp_board_manager_get_periph_handle("i2c_master", &bus);
    if (ret == ESP_OK && bus == NULL) {
        ret = ESP_ERR_NOT_FOUND;
    }
    if (ret == ESP_OK) {
        const i2c_device_config_t config = {
            .dev_addr_length = I2C_ADDR_BIT_LEN_7,
            .device_address = 0x32,
            .scl_speed_hz = 400000,
        };
        ret = i2c_master_bus_add_device((i2c_master_bus_handle_t)bus, &config, &s_rtc_device);
    }
    if (ret != ESP_OK) {
        ft_rtc_finish(ret);
        return;
    }
    s_rtc_sequence = (ft_rtc_sequence_t){0};
    s_rtc_started = lv_tick_get();
    s_poll_timer = lv_timer_create(ft_rtc_poll_cb, FT_RTC_POLL_MS, NULL);
    if (s_poll_timer == NULL) {
        ft_rtc_finish(ESP_ERR_NO_MEM);
    }
}

static void ft_item_cb(lv_event_t *event)
{
    const ft_item_t item = (ft_item_t)(uintptr_t)lv_event_get_user_data(event);
    switch (item) {
    case FT_ITEM_SCREEN:
        ft_start_screen_view();
        break;
    case FT_ITEM_TOUCH:
        ft_start_touch_view();
        break;
    case FT_ITEM_SD:
        ft_show_sd_dialog();
        break;
    case FT_ITEM_ETHERNET:
#if CONFIG_ETHERNET_PHY_IP101
        ft_start_ethernet_view();
#endif
        break;
    case FT_ITEM_WIFI:
        ft_start_wifi_view();
        break;
    case FT_ITEM_AUDIO:
        ft_start_audio_view();
        break;
    case FT_ITEM_RTC:
        ft_start_rtc_view();
        break;
    default:
        break;
    }
}

static void ft_duration_event_cb(lv_event_t *event)
{
    const lv_event_code_t code = lv_event_get_code(event);
    if (s_keyboard == NULL) {
        return;
    }
    if (code == LV_EVENT_CLICKED || code == LV_EVENT_FOCUSED) {
        lv_keyboard_set_mode(s_keyboard, LV_KEYBOARD_MODE_NUMBER);
        lv_keyboard_set_textarea(s_keyboard, s_duration_field);
        lv_obj_remove_flag(s_keyboard, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(s_keyboard);
        return;
    }
    if (code == LV_EVENT_READY || code == LV_EVENT_CANCEL) {
        ft_hide_keyboard();
    }
}

static void ft_duration_committed(void)
{
    if (s_duration_field == NULL) {
        return;
    }
    uint8_t seconds = 0U;
    const char *text = lv_textarea_get_text(s_duration_field);
    if (!ft_screen_duration_parse(text, &seconds)) {
        char fallback[4];
        snprintf(fallback, sizeof(fallback), "%u", (unsigned)s_screen_seconds);
        lv_textarea_set_text(s_duration_field, fallback);
        ft_toast(L(DESKTOP_TEXT_FACTORY_SCREEN_DURATION_INVALID));
        return;
    }
    s_screen_seconds = seconds;
}

static void ft_duration_start_cb(lv_event_t *event)
{
    (void)event;
    ft_duration_committed();
    ft_start_screen_view();
}

static lv_obj_t *ft_build_menu_row(lv_obj_t *parent, ft_item_t item)
{
    lv_obj_t *row = lv_obj_create(parent);
    ft_style_plain(row, C_SURFACE, 12);
    lv_obj_set_width(row, LV_PCT(100));
    lv_obj_set_height(row, FT_ROW_HEIGHT);
    lv_obj_set_style_pad_left(row, FT_ROW_PAD, 0);
    lv_obj_set_style_pad_right(row, FT_ROW_PAD, 0);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(row, ft_item_cb, LV_EVENT_CLICKED, (void *)(uintptr_t)item);

    lv_obj_t *name = ft_label(row, L(s_item_name_keys[item]), C_INK);
    lv_obj_align(name, LV_ALIGN_LEFT_MID, 0, 0);

    if (item == FT_ITEM_SCREEN) {
        lv_obj_t *hint = ft_label(row, L(DESKTOP_TEXT_FACTORY_SCREEN_HINT), C_INK_MUTED);
        lv_obj_align_to(hint, name, LV_ALIGN_OUT_BOTTOM_LEFT, 0, 2);

        s_duration_field = lv_textarea_create(row);
        lv_obj_set_size(s_duration_field, FT_DURATION_FIELD_WIDTH, 40);
        lv_obj_align(s_duration_field, LV_ALIGN_RIGHT_MID, -(FT_ACTION_BUTTON_WIDTH + 10), 0);
        lv_textarea_set_one_line(s_duration_field, true);
        lv_textarea_set_max_length(s_duration_field, 2);
        lv_textarea_set_accepted_chars(s_duration_field, "0123456789");
        char seconds_text[4];
        snprintf(seconds_text, sizeof(seconds_text), "%u", (unsigned)s_screen_seconds);
        lv_textarea_set_text(s_duration_field, seconds_text);
        lv_obj_add_event_cb(s_duration_field, ft_duration_event_cb, LV_EVENT_ALL, NULL);

        lv_obj_t *start = ft_button(row, L(DESKTOP_TEXT_FACTORY_SCREEN_START), C_ACCENT_SOFT,
                                    ft_duration_start_cb, NULL);
        lv_obj_set_size(start, FT_ACTION_BUTTON_WIDTH, 42);
        lv_obj_align(start, LV_ALIGN_RIGHT_MID, 0, 0);

        lv_obj_clear_flag(row, LV_OBJ_FLAG_CLICKABLE);
    }
    return row;
}

static void ft_build_menu(void)
{
    if (s_layout.content == NULL) {
        return;
    }
    lv_obj_clean(s_layout.content);
    ft_forget_menu_pointers();
    lv_obj_set_flex_flow(s_layout.content, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(s_layout.content, FT_COLUMN_GAP, 0);

    lv_obj_t *left = desktop_fullscreen_layout_create_panel(s_layout.content, C_CANVAS,
                                                          FT_LEFT_PERCENT, 0,
                                                          LV_FLEX_FLOW_COLUMN);
    lv_obj_t *right = desktop_fullscreen_layout_create_panel(s_layout.content, C_CANVAS, 0, 0,
                                                           LV_FLEX_FLOW_COLUMN);
    if (left == NULL || right == NULL) {
        return;
    }
    lv_obj_set_style_pad_row(left, FT_ROW_GAP, 0);
    lv_obj_set_style_pad_row(right, FT_ROW_GAP, 0);

    for (size_t index = 0U; index < FT_ITEM_COUNT; ++index) {
#if !CONFIG_ETHERNET_PHY_IP101
        if (index == FT_ITEM_ETHERNET) {
            continue;
        }
#endif
        (void)ft_build_menu_row(left, (ft_item_t)index);
    }

    ft_label(right, L(DESKTOP_TEXT_FACTORY_RESULT_TITLE), C_INK_MUTED);
    for (size_t index = 0U; index < FT_ITEM_COUNT; ++index) {
#if !CONFIG_ETHERNET_PHY_IP101
        if (index == FT_ITEM_ETHERNET) {
            continue;
        }
#endif
        lv_obj_t *row = lv_obj_create(right);
        ft_style_plain(row, C_SURFACE, 10);
        lv_obj_set_width(row, LV_PCT(100));
        lv_obj_set_height(row, 40);
        lv_obj_set_style_pad_left(row, 12, 0);
        lv_obj_set_style_pad_right(row, 12, 0);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_t *name = ft_label(row, L(s_item_name_keys[index]), C_INK);
        lv_obj_align(name, LV_ALIGN_LEFT_MID, 0, 0);
        s_result_labels[index] = ft_label(row, "", C_INK_MUTED);
        lv_obj_align(s_result_labels[index], LV_ALIGN_RIGHT_MID, 0, 0);
    }

    s_detail_label = ft_label(right, s_detail[0] != '\0' ? s_detail
                                                         : L(DESKTOP_TEXT_FACTORY_DETAIL_IDLE),
                              C_INK_MUTED);
    lv_label_set_long_mode(s_detail_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(s_detail_label, LV_PCT(100));
    lv_obj_set_flex_grow(s_detail_label, 1);
    ft_render_results();
}

static void ft_build_dialog(void)
{
    if (s_parent == NULL) {
        return;
    }
    s_dialog = lv_obj_create(s_parent);
    ft_style_plain(s_dialog, lv_color_hex(0x0B1220), 0);
    lv_obj_set_style_bg_opa(s_dialog, LV_OPA_50, 0);
    lv_obj_set_size(s_dialog, LV_PCT(100), LV_PCT(100));
    lv_obj_set_pos(s_dialog, 0, 0);
    lv_obj_clear_flag(s_dialog, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_dialog, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(s_dialog, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t *card = lv_obj_create(s_dialog);
    ft_style_plain(card, C_SURFACE, 16);
    lv_obj_set_size(card, FT_DIALOG_WIDTH, FT_DIALOG_HEIGHT);
    lv_obj_center(card);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *message = ft_label(card, L(DESKTOP_TEXT_FACTORY_SD_PROMPT), C_INK);
    lv_obj_align(message, LV_ALIGN_TOP_MID, 0, 34);
    lv_label_set_long_mode(message, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(message, FT_DIALOG_WIDTH - 48);
    lv_obj_set_style_text_align(message, LV_TEXT_ALIGN_CENTER, 0);

    lv_obj_t *cancel = ft_button(card, L(DESKTOP_TEXT_FACTORY_CANCEL), C_ACCENT_SOFT,
                                 ft_sd_cancel_cb, NULL);
    lv_obj_set_size(cancel, 176, 52);
    lv_obj_align(cancel, LV_ALIGN_BOTTOM_LEFT, 30, -28);

    lv_obj_t *confirm = ft_button(card, L(DESKTOP_TEXT_FACTORY_CONFIRM), C_ACCENT,
                                  ft_sd_confirm_cb, NULL);
    lv_obj_set_size(confirm, 176, 52);
    lv_obj_align(confirm, LV_ALIGN_BOTTOM_RIGHT, -30, -28);
}

static void ft_build_keyboard(void)
{
    if (s_parent == NULL) {
        return;
    }
    s_keyboard = lv_keyboard_create(s_parent);
    lv_obj_set_width(s_keyboard, LV_PCT(60));
    lv_obj_set_height(s_keyboard, LV_PCT(45));
    lv_obj_align(s_keyboard, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_keyboard_set_mode(s_keyboard, LV_KEYBOARD_MODE_NUMBER);
    lv_obj_add_flag(s_keyboard, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(s_keyboard, ft_duration_event_cb, LV_EVENT_READY, NULL);
    lv_obj_add_event_cb(s_keyboard, ft_duration_event_cb, LV_EVENT_CANCEL, NULL);
}

/**
 * @brief Release page resources; exit-strip visibility remains owned by the shell.
 */
void app_factory_test_view_cleanup(void)
{
    ft_stop_timers();
    app_ft_touch_view_cleanup();
    ft_audio_probe_cleanup();

    s_parent = NULL;
    s_host = NULL;
    s_stage = NULL;
    s_dialog = NULL;
    s_keyboard = NULL;
    s_duration_field = NULL;
    s_detail_label = NULL;
    s_wifi_status_label = NULL;
    s_eth_phy_label = NULL;
    s_eth_link_label = NULL;
    s_wifi_list = NULL;
    s_audio_state_label = NULL;
    s_audio_take_label = NULL;
    s_rtc_label = NULL;
    s_rtc_detail_label = NULL;
    for (size_t index = 0U; index < FT_ITEM_COUNT; ++index) {
        s_result_labels[index] = NULL;
    }
    memset(&s_layout, 0, sizeof(s_layout));
    s_view = FT_VIEW_MENU;
    s_wifi_rendered = false;
    s_toast_cb = NULL;
    s_cover_cb = NULL;
    s_cover_user_data = NULL;
}

void app_factory_test_view_build(lv_obj_t *parent,
                                   app_factory_test_toast_cb_t toast_cb,
                                   app_factory_test_cover_cb_t cover_cb,
                                   void *user_data)
{
    app_factory_test_view_cleanup();
    if (parent == NULL) {
        return;
    }

    s_parent = parent;
    s_toast_cb = toast_cb;
    s_cover_cb = cover_cb;
    s_cover_user_data = user_data;
    s_detail[0] = '\0';
    s_screen_seconds = FT_SCREEN_DURATION_DEFAULT_SECONDS;
    for (size_t index = 0U; index < FT_ITEM_COUNT; ++index) {
        s_results[index] = FACTORY_TEST_RESULT_PENDING;
    }
    const desktop_fullscreen_layout_colors_t colors = {
        .canvas = C_CANVAS,
        .surface = C_SURFACE,
        .ink = C_INK,
        .ink_muted = C_INK_MUTED,
    };

    lv_display_t *display = lv_obj_get_display(parent);
    s_full_width = display != NULL ? lv_display_get_horizontal_resolution(display) : 0;
    s_full_height = display != NULL ? lv_display_get_vertical_resolution(display) : 0;
    if (s_full_width <= 0) {
        s_full_width = lv_obj_get_width(parent);
    }
    if (s_full_height <= 0) {
        s_full_height = lv_obj_get_height(parent);
    }

    s_pad_left = lv_obj_get_style_pad_left(parent, LV_PART_MAIN);
    s_pad_right = lv_obj_get_style_pad_right(parent, LV_PART_MAIN);
    s_pad_top = lv_obj_get_style_pad_top(parent, LV_PART_MAIN);
    s_pad_bottom = lv_obj_get_style_pad_bottom(parent, LV_PART_MAIN);

    s_host = lv_obj_create(parent);
    ft_style_plain(s_host, C_CANVAS, 0);
    lv_obj_set_size(s_host, s_full_width - s_pad_left - s_pad_right,
                    s_full_height - s_pad_top - s_pad_bottom);
    lv_obj_clear_flag(s_host, LV_OBJ_FLAG_SCROLLABLE);
    if (!desktop_fullscreen_layout_create(s_host, &colors, "", "", "", &s_layout)) {
        return;
    }
    desktop_fullscreen_layout_remove_page_header(&s_layout);

    s_stage = lv_obj_create(parent);
    ft_style_plain(s_stage, lv_color_hex(0x000000), 0);
    lv_obj_set_size(s_stage, s_full_width, s_full_height);
    lv_obj_set_pos(s_stage, 0, 0);
    lv_obj_clear_flag(s_stage, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_stage, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_stage, ft_screen_long_press_cb, LV_EVENT_LONG_PRESSED, NULL);
    lv_obj_add_flag(s_stage, LV_OBJ_FLAG_HIDDEN);

    ft_build_menu();
    ft_build_dialog();
    ft_build_keyboard();
    ESP_LOGI(FT_TAG, "factory test page ready");
}
