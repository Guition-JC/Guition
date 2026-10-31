#include "app_wifi_view.h"

#include <stdio.h>
#include <string.h>

#include "wifi_service.h"

#include "app_wifi_widgets.h"
#include "app_wifi_connection_view.h"
#include "app_wifi_provisioning_view.h"
#include "desktop_language.h"
#include "esp_attr.h"
#include "esp_log.h"

#define L(key) desktop_language_lookup(key)

#define C_CANVAS APP_WIFI_C_CANVAS
#define C_SURFACE APP_WIFI_C_SURFACE
#define C_MUTED APP_WIFI_C_MUTED
#define C_INK APP_WIFI_C_INK
#define C_INK_MUTED APP_WIFI_C_INK_MUTED
#define C_ACCENT APP_WIFI_C_ACCENT
#define C_ACCENT_SOFT APP_WIFI_C_ACCENT_SOFT
#define C_DANGER APP_WIFI_C_DANGER

typedef enum {
    WIFI_VIEW_LIST,
    WIFI_VIEW_PASSWORD,
} wifi_view_page_t;

static lv_obj_t *s_parent;
static lv_obj_t *s_status_label;
static lv_obj_t *s_header_actions;
static lv_obj_t *s_keyboard;
static lv_obj_t *s_password_field;
static lv_timer_t *s_poll_timer;
static lv_timer_t *s_scan_start_timer;
static app_wifi_toast_cb_t s_toast;

EXT_RAM_BSS_ATTR static wifi_service_ap_t s_aps[WIFI_SERVICE_MAX_APS];
static size_t s_ap_count;
static size_t s_selected_ap;
static wifi_view_page_t s_page;
static bool s_scan_rendered;
static bool s_scan_loading;
static bool s_connection_submitted;
static bool s_qr_visible;
static lv_obj_t *s_connect_button;

static const char *auth_text(wifi_auth_mode_t authmode)
{
    return authmode == WIFI_AUTH_OPEN ? L(DESKTOP_TEXT_WIFI_AP_OPEN)
                                      : L(DESKTOP_TEXT_WIFI_AP_SECURED);
}

static const char *state_text(wifi_service_state_t state)
{
    switch (state) {
    case WIFI_SERVICE_STATE_SCANNING:
        return L(DESKTOP_TEXT_WIFI_STATE_SCANNING);
    case WIFI_SERVICE_STATE_CONNECTING:
        return L(DESKTOP_TEXT_WIFI_STATE_CONNECTING);
    case WIFI_SERVICE_STATE_CONNECTED:
        return L(DESKTOP_TEXT_WIFI_STATE_CONNECTED);
    case WIFI_SERVICE_STATE_FAILED:
        return L(DESKTOP_TEXT_WIFI_STATE_FAILED);
    case WIFI_SERVICE_STATE_READY:
        return L(DESKTOP_TEXT_WIFI_STATE_PICK);
    case WIFI_SERVICE_STATE_IDLE:
        return L(DESKTOP_TEXT_WIFI_STATE_READY);
    default:
        return L(DESKTOP_TEXT_WIFI_STATE_UNKNOWN);
    }
}

static void set_status(const char *text, lv_color_t color)
{
    if (s_status_label != NULL) {
        lv_label_set_text(s_status_label, text);
        lv_obj_set_style_text_color(s_status_label, color, 0);
    }
}

static lv_obj_t *make_flex_button(lv_obj_t *parent, const char *text, lv_color_t color,
                                  lv_event_cb_t callback, void *user_data)
{
    lv_obj_t *button = app_wifi_make_button(parent, text, 0, 0, 1, 1, color, callback, user_data);
    lv_obj_set_width(button, LV_PCT(100));
    lv_obj_set_height(button, LV_PCT(82));
    lv_obj_set_flex_grow(button, 1);
    return button;
}

static void render_list(void);

static void scan_button_cb(lv_event_t *event)
{
    (void)event;
    s_scan_rendered = false;
    s_scan_loading = true;
    set_status(L(DESKTOP_TEXT_WIFI_STATE_SCANNING), C_ACCENT);
    const esp_err_t ret = wifi_service_start_scan();
    if (ret != ESP_OK) {
        char message[96];
        snprintf(message, sizeof(message), L(DESKTOP_TEXT_WIFI_ERROR_SCAN_START),
                 esp_err_to_name(ret));
        ESP_LOGE("app_wifi_view", "%s", message);
        s_scan_loading = false;
        render_list();
        set_status(message, C_DANGER);
    }
}

static void qr_button_cb(lv_event_t *event);
static void deferred_scan_start_cb(lv_timer_t *timer);
static void create_header(const char *title, const char *subtitle);
static void render_password(void);
static void back_to_list_cb(lv_event_t *event);
static void ap_select_cb(lv_event_t *event);

/**
 * @brief Render scanned access points or the current scan state.
 */
static void render_list(void)
{
    s_qr_visible = wifi_service_get_provisioning_qr_payload()[0] != '\0';
    s_connect_button = NULL;
    app_wifi_connection_view_reset();
    lv_obj_clean(s_parent);
    create_header("", "");
    make_flex_button(s_header_actions, L(DESKTOP_TEXT_WIFI_ACTION_SCAN), C_ACCENT_SOFT,
                     scan_button_cb, NULL);
    make_flex_button(s_header_actions, L(DESKTOP_TEXT_WIFI_ACTION_QR), C_MUTED,
                     qr_button_cb, NULL);

    app_wifi_connection_view_create(s_parent);

    lv_obj_t *content = lv_obj_create(s_parent);
    app_wifi_style_panel(content, C_CANVAS, 0);
    lv_obj_set_width(content, LV_PCT(100));
    lv_obj_set_flex_grow(content, 1);
    lv_obj_set_flex_flow(content, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(content, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_column(content, 8, 0);

    lv_obj_t *list = lv_obj_create(content);
    app_wifi_style_panel(list, C_CANVAS, 0);
    lv_obj_set_width(list, LV_PCT(64));
    lv_obj_set_height(list, LV_PCT(100));
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(list, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(list, 8, 0);
    lv_obj_set_scroll_dir(list, LV_DIR_VER);

    if (s_scan_loading) {
        lv_obj_t *loading_panel = lv_obj_create(list);
        app_wifi_style_panel(loading_panel, C_SURFACE, 12);
        lv_obj_set_width(loading_panel, LV_PCT(100));
        lv_obj_set_height(loading_panel, s_ap_count == 0 ? LV_PCT(100) : 76);
        lv_obj_set_flex_flow(loading_panel, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(loading_panel, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_row(loading_panel, 8, 0);

        lv_obj_t *spinner = lv_spinner_create(loading_panel);
        lv_obj_set_size(spinner, s_ap_count == 0 ? 58 : 42, s_ap_count == 0 ? 58 : 42);
        lv_spinner_set_anim_params(spinner, 900, 105);
        lv_obj_set_style_arc_width(spinner, 6, LV_PART_MAIN);
        lv_obj_set_style_arc_color(spinner, C_ACCENT_SOFT, LV_PART_MAIN);
        lv_obj_set_style_arc_width(spinner, 6, LV_PART_INDICATOR);
        lv_obj_set_style_arc_color(spinner, C_ACCENT, LV_PART_INDICATOR);

        lv_obj_t *loading_label = app_wifi_make_flex_label(
            loading_panel,
            s_ap_count == 0 ? L(DESKTOP_TEXT_WIFI_LOADING_SEARCH)
                            : L(DESKTOP_TEXT_WIFI_LOADING_UPDATE),
            C_INK_MUTED);
        lv_obj_set_width(loading_label, LV_PCT(100));
        lv_obj_set_style_text_align(loading_label, LV_TEXT_ALIGN_CENTER, 0);
    } else if (s_ap_count == 0) {
        lv_obj_t *empty_label =
            app_wifi_make_flex_label(list, L(DESKTOP_TEXT_WIFI_EMPTY), C_INK_MUTED);
        lv_obj_set_height(empty_label, LV_PCT(100));
        lv_obj_set_style_text_align(empty_label, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_pad_top(empty_label, 24, 0);
    }

    for (size_t index = 0; index < s_ap_count; ++index) {
        const wifi_service_ap_t *ap = &s_aps[index];
        char detail[96];
        snprintf(detail, sizeof(detail), L(DESKTOP_TEXT_WIFI_AP_DETAIL),
                 auth_text(ap->authmode), ap->rssi, (unsigned int)ap->channel);

        lv_obj_t *row = app_wifi_make_button(list,
                                               ap->ssid[0] == '\0'
                                                   ? L(DESKTOP_TEXT_WIFI_AP_HIDDEN)
                                                   : ap->ssid,
                                               0, 0, 1, 1,
                                               index % 2 == 0 ? C_SURFACE : C_MUTED, ap_select_cb,
                                               (void *)(uintptr_t)index);
        lv_obj_set_width(row, LV_PCT(100));
        lv_obj_set_height(row, LV_PCT(17));
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_hor(row, 2, 0);

        lv_obj_t *row_label = lv_obj_get_child(row, 0);
        lv_obj_set_width(row_label, LV_PCT(58));
        lv_obj_set_style_text_color(row_label, C_INK, 0);
        lv_label_set_long_mode(row_label, LV_LABEL_LONG_DOT);
        lv_obj_set_style_text_align(row_label, LV_TEXT_ALIGN_LEFT, 0);

        lv_obj_t *detail_label = app_wifi_make_flex_label(row, detail, C_INK_MUTED);
        lv_obj_set_width(detail_label, LV_PCT(40));
        lv_label_set_long_mode(detail_label, LV_LABEL_LONG_DOT);
        lv_obj_set_style_text_align(detail_label, LV_TEXT_ALIGN_RIGHT, 0);
    }

    app_wifi_provisioning_view_create(content);
}

static void back_to_list_cb(lv_event_t *event)
{
    (void)event;
    s_page = WIFI_VIEW_LIST;
    render_list();
}

static void ap_select_cb(lv_event_t *event)
{
    const size_t index = (size_t)(uintptr_t)lv_event_get_user_data(event);
    if (index >= s_ap_count) {
        return;
    }
    s_connection_submitted = false;
    s_selected_ap = index;
    s_page = WIFI_VIEW_PASSWORD;
    render_password();
}

static void text_area_event_cb(lv_event_t *event)
{
    const lv_event_code_t code = lv_event_get_code(event);
    lv_obj_t *text_area = lv_event_get_target_obj(event);
    if (code == LV_EVENT_FOCUSED) {
        lv_keyboard_set_textarea(s_keyboard, text_area);
        lv_obj_remove_flag(s_keyboard, LV_OBJ_FLAG_HIDDEN);
    } else if (code == LV_EVENT_DEFOCUSED) {
        lv_keyboard_set_textarea(s_keyboard, NULL);
        lv_obj_add_flag(s_keyboard, LV_OBJ_FLAG_HIDDEN);
    }
}

static void connect_button_cb(lv_event_t *event)
{
    (void)event;
    const wifi_service_ap_t *ap = &s_aps[s_selected_ap];
    const char *password = lv_textarea_get_text(s_password_field);
    const wifi_service_security_t security = ap->authmode == WIFI_AUTH_OPEN
                                                 ? WIFI_SERVICE_SECURITY_OPEN
                                                 : WIFI_SERVICE_SECURITY_PSK;
    const size_t password_length = strlen(password);
    if (security == WIFI_SERVICE_SECURITY_OPEN && password_length != 0) {
        set_status(L(DESKTOP_TEXT_WIFI_PASSWORD_OPEN_HINT), C_DANGER);
        return;
    }
    if (security == WIFI_SERVICE_SECURITY_PSK &&
        (password_length < 8 || password_length > WIFI_SERVICE_PASSWORD_MAX_LEN)) {
        set_status(L(DESKTOP_TEXT_WIFI_PASSWORD_TOO_SHORT), C_DANGER);
        return;
    }
    const esp_err_t ret = wifi_service_connect(ap->ssid, security, password);
    lv_keyboard_set_textarea(s_keyboard, NULL);
    lv_obj_add_flag(s_keyboard, LV_OBJ_FLAG_HIDDEN);
    if (ret == ESP_OK) {
        s_connection_submitted = true;
        lv_obj_add_state(s_connect_button, LV_STATE_DISABLED);
        set_status(L(DESKTOP_TEXT_WIFI_STATE_CONNECTING), C_ACCENT);
        if (s_toast != NULL) {
            s_toast(L(DESKTOP_TEXT_WIFI_TOAST_SUBMITTED));
        }
    } else {
        set_status(ret == ESP_ERR_INVALID_STATE ? L(DESKTOP_TEXT_WIFI_ERROR_BUSY)
                                                : L(DESKTOP_TEXT_WIFI_ERROR_REQUEST_FAILED),
                   C_DANGER);
    }
}

static void qr_button_cb(lv_event_t *event)
{
    (void)event;
    const esp_err_t ret = wifi_service_start_provisioning();
    if (ret != ESP_OK) {
        char message[96];
        snprintf(message, sizeof(message), L(DESKTOP_TEXT_WIFI_ERROR_PROVISIONING_START),
                 esp_err_to_name(ret));
        ESP_LOGE("app_wifi_view", "%s", message);
        set_status(message, C_DANGER);
        return;
    }
    s_page = WIFI_VIEW_LIST;
    render_list();
}

static void create_header(const char *title, const char *subtitle)
{
    app_wifi_style_panel(s_parent, C_CANVAS, 0);
    lv_obj_set_size(s_parent, LV_PCT(100), LV_PCT(100));
    lv_obj_update_layout(s_parent);
    const int32_t layout_width = lv_obj_get_width(s_parent);
    const int32_t layout_height = lv_obj_get_height(s_parent);
    const int32_t safe_x = layout_width * 2 / 100;
    const int32_t safe_top = layout_height * 3 / 100;
    const int32_t safe_bottom = layout_height * 2 / 100;
    lv_obj_set_style_pad_left(s_parent, safe_x, 0);
    lv_obj_set_style_pad_right(s_parent, safe_x, 0);
    lv_obj_set_style_pad_top(s_parent, safe_top, 0);
    lv_obj_set_style_pad_bottom(s_parent, safe_bottom, 0);
    lv_obj_set_flex_flow(s_parent, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_parent, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(s_parent, 8, 0);

    lv_obj_t *header = lv_obj_create(s_parent);
    app_wifi_style_panel(header, C_CANVAS, 0);
    lv_obj_set_width(header, LV_PCT(100));
    lv_obj_set_height(header, LV_PCT(15));
    lv_obj_set_flex_flow(header, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(header, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t *title_block = lv_obj_create(header);
    app_wifi_style_panel(title_block, C_CANVAS, 0);
    lv_obj_set_width(title_block, LV_PCT(64));
    lv_obj_set_height(title_block, LV_PCT(100));
    lv_obj_set_flex_flow(title_block, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(title_block, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);

    const bool has_title = title != NULL && title[0] != '\0';
    const bool has_subtitle = subtitle != NULL && subtitle[0] != '\0';
    lv_obj_t *title_label = app_wifi_make_flex_label(title_block, has_title ? title : "",
                                                       C_INK);
    lv_obj_set_style_text_align(title_label, LV_TEXT_ALIGN_LEFT, 0);
    if (has_title) {
        lv_obj_set_height(title_label, LV_PCT(35));
    } else {
        lv_obj_add_flag(title_label, LV_OBJ_FLAG_HIDDEN);
    }
    lv_obj_t *subtitle_label = app_wifi_make_flex_label(
        title_block, has_subtitle ? subtitle : "", C_INK_MUTED);
    lv_obj_set_style_text_align(subtitle_label, LV_TEXT_ALIGN_LEFT, 0);
    if (has_subtitle) {
        lv_obj_set_height(subtitle_label, LV_PCT(35));
    } else {
        lv_obj_add_flag(subtitle_label, LV_OBJ_FLAG_HIDDEN);
    }
    s_status_label = app_wifi_make_flex_label(title_block, state_text(wifi_service_get_state()), C_ACCENT);
    lv_obj_set_height(s_status_label,
                      (has_title || has_subtitle) ? LV_PCT(30) : LV_SIZE_CONTENT);
    lv_obj_set_style_text_align(s_status_label, LV_TEXT_ALIGN_LEFT, 0);

    s_header_actions = lv_obj_create(header);
    app_wifi_style_panel(s_header_actions, C_CANVAS, 0);
    lv_obj_set_width(s_header_actions, LV_PCT(35));
    lv_obj_set_height(s_header_actions, LV_PCT(100));
    lv_obj_set_flex_flow(s_header_actions, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(s_header_actions, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(s_header_actions, 8, 0);
}

/**
 * @brief Build credentials entry for the selected access point.
 */
static void render_password(void)
{
    const wifi_service_ap_t *ap = &s_aps[s_selected_ap];
    app_wifi_connection_view_reset();
    lv_obj_clean(s_parent);
    create_header(L(DESKTOP_TEXT_WIFI_PAGE_TITLE), ap->ssid);
    make_flex_button(s_header_actions, L(DESKTOP_TEXT_WIFI_ACTION_BACK), C_MUTED,
                     back_to_list_cb, NULL);

    lv_obj_t *form = lv_obj_create(s_parent);
    app_wifi_style_panel(form, C_CANVAS, 0);
    lv_obj_set_width(form, LV_PCT(100));
    lv_obj_set_flex_grow(form, 1);
    lv_obj_set_flex_flow(form, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(form, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(form, 8, 0);

    lv_obj_t *password_title =
        app_wifi_make_flex_label(form, L(DESKTOP_TEXT_WIFI_PASSWORD_LABEL), C_INK);
    lv_obj_set_height(password_title, LV_PCT(12));

    lv_obj_t *input_row = lv_obj_create(form);
    app_wifi_style_panel(input_row, C_CANVAS, 0);
    lv_obj_set_width(input_row, LV_PCT(100));
    lv_obj_set_height(input_row, LV_PCT(20));
    lv_obj_set_flex_flow(input_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(input_row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(input_row, 8, 0);

    s_password_field = lv_textarea_create(input_row);
    lv_obj_set_width(s_password_field, LV_PCT(78));
    lv_obj_set_height(s_password_field, LV_PCT(100));
    lv_textarea_set_one_line(s_password_field, true);
    lv_textarea_set_password_mode(s_password_field, ap->authmode != WIFI_AUTH_OPEN);
    lv_textarea_set_placeholder_text(s_password_field,
                                     ap->authmode == WIFI_AUTH_OPEN
                                         ? L(DESKTOP_TEXT_WIFI_PASSWORD_OPEN_HINT)
                                         : L(DESKTOP_TEXT_WIFI_PASSWORD_PLACEHOLDER));
    lv_obj_add_event_cb(s_password_field, text_area_event_cb, LV_EVENT_ALL, NULL);

    lv_obj_t *connect_button = app_wifi_make_button(
        input_row, L(DESKTOP_TEXT_WIFI_ACTION_CONNECT), 0, 0, 1, 1, C_ACCENT_SOFT,
        connect_button_cb, NULL);
    s_connect_button = connect_button;
    lv_obj_set_width(connect_button, LV_PCT(20));
    lv_obj_set_height(connect_button, LV_PCT(100));

    s_keyboard = lv_keyboard_create(form);
    lv_obj_set_width(s_keyboard, LV_PCT(100));
    lv_obj_set_height(s_keyboard, LV_PCT(58));
    lv_keyboard_set_mode(s_keyboard, LV_KEYBOARD_MODE_TEXT_LOWER);
    lv_obj_add_flag(s_keyboard, LV_OBJ_FLAG_HIDDEN);
    lv_keyboard_set_textarea(s_keyboard, s_password_field);
    set_status(L(DESKTOP_TEXT_WIFI_PASSWORD_REQUIRED), C_ACCENT);
}

/**
 * @brief Start the initial scan after the page has been displayed.
 * @param timer LVGL timer that starts the scan.
 */
static void deferred_scan_start_cb(lv_timer_t *timer)
{
    (void)timer;
    s_scan_start_timer = NULL;

    if (s_parent == NULL || s_page != WIFI_VIEW_LIST) {
        return;
    }

    s_scan_loading = true;
    set_status(L(DESKTOP_TEXT_WIFI_STATE_SCANNING), C_ACCENT);
    const esp_err_t ret = wifi_service_start_scan();
    if (ret != ESP_OK) {
        char message[96];
        snprintf(message, sizeof(message), L(DESKTOP_TEXT_WIFI_ERROR_SCAN_START),
                 esp_err_to_name(ret));
        ESP_LOGE("app_wifi_view", "%s", message);
        s_scan_loading = false;
        render_list();
        set_status(message, C_DANGER);
    }
}

/**
 * @brief Refresh scan results and connection state from the Wi-Fi service.
 * @param timer LVGL polling timer.
 */
static void poll_cb(lv_timer_t *timer)
{
    (void)timer;
    const wifi_service_state_t state = wifi_service_get_state();
    if (s_page == WIFI_VIEW_LIST) {
        app_wifi_connection_view_update();
        if (state == WIFI_SERVICE_STATE_SCANNING) {
            s_scan_rendered = false;
        }
        if (s_qr_visible != (wifi_service_get_provisioning_qr_payload()[0] != '\0')) {
            render_list();
        }
        if ((state == WIFI_SERVICE_STATE_READY || state == WIFI_SERVICE_STATE_CONNECTED ||
             state == WIFI_SERVICE_STATE_FAILED) && !s_scan_rendered) {
            if (state != WIFI_SERVICE_STATE_FAILED) {
                s_ap_count = wifi_service_get_scan_results(s_aps, WIFI_SERVICE_MAX_APS);
            }
            s_scan_loading = false;
            s_scan_rendered = true;
            render_list();
        }
        if (s_status_label != NULL) {
            set_status(state_text(state), state == WIFI_SERVICE_STATE_FAILED ? C_DANGER : C_ACCENT);
        }
    }
    if (s_page == WIFI_VIEW_PASSWORD && s_connection_submitted && state == WIFI_SERVICE_STATE_FAILED) {
        s_connection_submitted = false;
        lv_obj_remove_state(s_connect_button, LV_STATE_DISABLED);
        set_status(L(DESKTOP_TEXT_WIFI_ERROR_CONNECT_FAILED), C_DANGER);
    }
    wifi_service_connection_info_t connection = {0};
    if (s_page == WIFI_VIEW_PASSWORD && s_connection_submitted &&
        state == WIFI_SERVICE_STATE_CONNECTED && wifi_service_get_connection_info(&connection) &&
        strcmp(connection.ssid, s_aps[s_selected_ap].ssid) == 0) {
        s_connection_submitted = false;
        if (s_toast != NULL) {
            s_toast(L(DESKTOP_TEXT_WIFI_TOAST_CONNECTED));
        }
        s_page = WIFI_VIEW_LIST;
        s_scan_rendered = true;
        render_list();
    }
}

/**
 * @brief Release timers and the UI-owned copy of the scan list.
 */
void app_wifi_view_cleanup(void)
{
    if (s_poll_timer != NULL) {
        lv_timer_delete(s_poll_timer);
        s_poll_timer = NULL;
    }
    if (s_scan_start_timer != NULL) {
        lv_timer_delete(s_scan_start_timer);
        s_scan_start_timer = NULL;
    }
    s_parent = NULL;
    s_status_label = NULL;
    s_header_actions = NULL;
    s_keyboard = NULL;
    s_password_field = NULL;
    s_connect_button = NULL;
    s_connection_submitted = false;
    s_toast = NULL;
    app_wifi_connection_view_reset();
}

void app_wifi_view_build(lv_obj_t *parent, app_wifi_toast_cb_t toast_cb)
{
    if (s_poll_timer != NULL) {
        lv_timer_delete(s_poll_timer);
        s_poll_timer = NULL;
    }
    if (s_scan_start_timer != NULL) {
        lv_timer_delete(s_scan_start_timer);
        s_scan_start_timer = NULL;
    }
    s_parent = parent;
    s_toast = toast_cb;
    s_connection_submitted = false;
    s_ap_count = 0;
    s_selected_ap = 0;
    s_scan_rendered = false;
    s_scan_loading = true;
    s_page = WIFI_VIEW_LIST;

    render_list();
    set_status(L(DESKTOP_TEXT_WIFI_STATE_READY), C_ACCENT);

    s_scan_start_timer = lv_timer_create(deferred_scan_start_cb, 50, NULL);
    if (s_scan_start_timer == NULL) {
        ESP_LOGE("app_wifi_view", "Wi-Fi deferred scan timer allocation failed");
        set_status(L(DESKTOP_TEXT_WIFI_ERROR_SCAN_UNAVAILABLE), C_DANGER);
    } else {
        lv_timer_set_repeat_count(s_scan_start_timer, 1);
    }
    s_poll_timer = lv_timer_create(poll_cb, 500, NULL);
    if (s_poll_timer == NULL) {
        ESP_LOGE("app_wifi_view", "Wi-Fi poll timer allocation failed");
        set_status(L(DESKTOP_TEXT_WIFI_ERROR_POLL_UNAVAILABLE), C_DANGER);
    }
}
