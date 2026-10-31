#include "app_wifi_provisioning_view.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"

#include "include/lvgl/widgets/lv_qrcode.h"
#include "app_wifi_widgets.h"
#include "desktop_language.h"
#include "wifi_service.h"

#define L(key) desktop_language_lookup(key)

/**
 * @brief Compute a square QR canvas size from the panel content area.
 * @param panel Panel containing the QR code.
 * @return Square canvas size in pixels.
 */
static int32_t qr_size_for_panel(lv_obj_t *panel)
{
    const int32_t width = lv_obj_get_content_width(panel);
    const int32_t height = lv_obj_get_content_height(panel);
    const int32_t width_size = (width * 62) / 100;
    const int32_t height_size = (height * 52) / 100;
    return width_size < height_size ? width_size : height_size;
}

/**
 * @brief Resize and redraw the QR canvas when the panel dimensions change.
 * @param panel Panel containing the QR code.
 */
static void update_qr_size(lv_obj_t *panel)
{
    lv_obj_t *qr = lv_obj_get_child(panel, 1);
    if (qr == NULL) {
        return;
    }

    const int32_t size = qr_size_for_panel(panel);
    if (size > 0) {
        lv_draw_buf_t *buffer = lv_canvas_get_draw_buf(qr);
        if (buffer == NULL || buffer->header.w != (uint32_t)size) {
            lv_qrcode_set_size(qr, size);
        }

        const char *payload = wifi_service_get_provisioning_qr_payload();
        if (lv_qrcode_update(qr, payload, strlen(payload)) != LV_RESULT_OK) {
            ESP_LOGW("wifi_qr", "QR rendering failed (size=%ld)", (long)size);
        }
    }
}

static void qr_panel_size_changed_cb(lv_event_t *event)
{
    update_qr_size(lv_event_get_target_obj(event));
}

void app_wifi_provisioning_view_create(lv_obj_t *parent)
{
    lv_obj_t *panel = lv_obj_create(parent);
    app_wifi_style_panel(panel, APP_WIFI_C_SURFACE, 14);
    lv_obj_set_width(panel, LV_PCT(34));
    lv_obj_set_height(panel, LV_PCT(100));
    lv_obj_set_flex_flow(panel, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(panel, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_ver(panel, 3, 0);
    lv_obj_set_style_pad_hor(panel, 4, 0);

    lv_obj_t *title = app_wifi_make_flex_label(
        panel, L(DESKTOP_TEXT_WIFI_ACTION_QR), APP_WIFI_C_INK_MUTED);
    lv_obj_set_style_text_align(title, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_height(title, LV_PCT(14));

    if (wifi_service_get_provisioning_ssid()[0] == '\0') {
        lv_obj_t *message = app_wifi_make_flex_label(
            panel, L(DESKTOP_TEXT_WIFI_QR_IDLE_HINT), APP_WIFI_C_INK);
        lv_obj_set_height(message, LV_PCT(42));
        lv_obj_set_style_text_align(message, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_pad_top(message, 12, 0);

        lv_obj_t *hint = app_wifi_make_flex_label(
            panel, L(DESKTOP_TEXT_WIFI_QR_PHONE_HINT), APP_WIFI_C_INK_MUTED);
        lv_obj_set_height(hint, LV_PCT(30));
        lv_obj_set_style_text_align(hint, LV_TEXT_ALIGN_CENTER, 0);
        return;
    }

    lv_obj_t *qr = lv_qrcode_create(panel);
    lv_qrcode_set_dark_color(qr, APP_WIFI_C_SYSTEM);
    lv_qrcode_set_light_color(qr, APP_WIFI_C_SYSTEM_TEXT);
    lv_qrcode_set_quiet_zone(qr, true);

    lv_obj_set_style_border_color(qr, APP_WIFI_C_SYSTEM_TEXT, 0);
    lv_obj_set_style_border_width(qr, 5, 0);
    lv_obj_add_event_cb(panel, qr_panel_size_changed_cb, LV_EVENT_SIZE_CHANGED, NULL);

    char access_text[192];
    snprintf(access_text, sizeof(access_text), L(DESKTOP_TEXT_WIFI_QR_ACCESS),
             wifi_service_get_provisioning_ssid(), wifi_service_get_provisioning_password(),
             wifi_service_get_provisioning_token());
    lv_obj_t *access_label = app_wifi_make_flex_label(panel, access_text, APP_WIFI_C_INK);
    lv_obj_set_height(access_label, LV_PCT(28));
    lv_obj_set_style_text_align(access_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(access_label, LV_LABEL_LONG_WRAP);

    lv_obj_update_layout(panel);
    update_qr_size(panel);
}
