#include "app_wifi_connection_view.h"

#include <stdio.h>

#include "app_wifi_widgets.h"
#include "desktop_language.h"
#include "wifi_service.h"

#define L(key) desktop_language_lookup(key)

static lv_obj_t *s_ssid_label;
static lv_obj_t *s_ip_label;
static lv_obj_t *s_signal_label;
static lv_obj_t *s_disconnect_button;

static lv_obj_t *make_connection_field(lv_obj_t *parent, const char *title, const char *value)
{
    lv_obj_t *field = lv_obj_create(parent);
    app_wifi_style_panel(field, APP_WIFI_C_SURFACE, 0);
    lv_obj_set_width(field, LV_PCT(26));
    lv_obj_set_height(field, LV_PCT(100));
    lv_obj_set_flex_flow(field, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(field, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER);

    lv_obj_t *title_label = app_wifi_make_flex_label(field, title, APP_WIFI_C_INK_MUTED);
    lv_obj_set_style_text_align(title_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_t *value_label = app_wifi_make_flex_label(field, value, APP_WIFI_C_INK);
    lv_obj_set_style_text_align(value_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(value_label, LV_LABEL_LONG_DOT);
    return value_label;
}

/**
 * @brief Disconnect the active station connection.
 * @param event LVGL button event.
 */
static void disconnect_button_cb(lv_event_t *event)
{
    (void)event;
    const esp_err_t ret = wifi_service_disconnect();
    app_wifi_connection_view_update();
    if (ret != ESP_OK) {
        lv_label_set_text(s_signal_label, L(DESKTOP_TEXT_WIFI_ERROR_DISCONNECT_FAILED));
    }
}

void app_wifi_connection_view_reset(void)
{
    s_ssid_label = NULL;
    s_ip_label = NULL;
    s_signal_label = NULL;
    s_disconnect_button = NULL;
}

void app_wifi_connection_view_create(lv_obj_t *parent)
{
    app_wifi_connection_view_reset();

    lv_obj_t *card = lv_obj_create(parent);
    app_wifi_style_panel(card, APP_WIFI_C_SURFACE, 14);
    lv_obj_set_width(card, LV_PCT(100));
    lv_obj_set_height(card, LV_PCT(15));
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(card, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    s_ssid_label = make_connection_field(card, L(DESKTOP_TEXT_WIFI_FIELD_CURRENT),
                                         L(DESKTOP_TEXT_WIFI_FIELD_DISCONNECTED));
    s_ip_label = make_connection_field(card, "IPv4", "--");
    s_signal_label = make_connection_field(card, L(DESKTOP_TEXT_WIFI_FIELD_SIGNAL), "--");
    s_disconnect_button = app_wifi_make_button(card, L(DESKTOP_TEXT_WIFI_ACTION_DISCONNECT),
                                                0, 0, 1, 1,
                                                APP_WIFI_C_MUTED, disconnect_button_cb, NULL);
    lv_obj_set_width(s_disconnect_button, LV_PCT(20));
    lv_obj_set_height(s_disconnect_button, LV_PCT(80));
    app_wifi_connection_view_update();
}

/**
 * @brief Read the service status and update network detail fields.
 */
void app_wifi_connection_view_update(void)
{
    if (s_ssid_label == NULL || s_ip_label == NULL || s_signal_label == NULL) {
        return;
    }

    wifi_service_connection_info_t info = {0};
    const bool connected = wifi_service_get_connection_info(&info);
    if (connected && wifi_service_get_state() != WIFI_SERVICE_STATE_CONNECTING) {
        lv_obj_remove_state(s_disconnect_button, LV_STATE_DISABLED);
    } else {
        lv_obj_add_state(s_disconnect_button, LV_STATE_DISABLED);
    }
    if (!connected) {
        lv_label_set_text(s_ssid_label, L(DESKTOP_TEXT_WIFI_FIELD_DISCONNECTED));
        lv_label_set_text(s_ip_label, "--");
        lv_label_set_text(s_signal_label, "--");
        return;
    }

    char ip_text[48];
    char signal_text[48];
    snprintf(ip_text, sizeof(ip_text), "%s", info.ipv4);
    snprintf(signal_text, sizeof(signal_text), L(DESKTOP_TEXT_WIFI_FIELD_SIGNAL_VALUE),
             info.rssi, (unsigned int)info.channel);
    lv_label_set_text(s_ssid_label, info.ssid);
    lv_label_set_text(s_ip_label, ip_text);
    lv_label_set_text(s_signal_label, signal_text);
}
