#include "app_ethernet_view.h"

#include "lwip/inet.h"
#include "lwip/sockets.h"
#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ethernet_iperf.h"
#include "ethernet_service.h"
#include "esp_log.h"
#include "lvgl.h"
#include "desktop_language.h"
#include "desktop_fullscreen_layout.h"

#define L(key) desktop_language_lookup(key)

#define C_SURFACE     lv_color_hex(0xF7FAFD)
#define C_MUTED       lv_color_hex(0xE2ECF6)
#define C_INK         lv_color_hex(0x122033)
#define C_INK_MUTED   lv_color_hex(0x53667B)
#define C_ACCENT      lv_color_hex(0x147EFB)
#define C_ACCENT_SOFT lv_color_hex(0xD9EAFF)
#define C_SUCCESS     lv_color_hex(0x22A96B)
#define C_WARNING     lv_color_hex(0xED9D33)
#define C_DANGER      lv_color_hex(0xE05151)
#define C_DIVIDER     lv_color_hex(0xD9E0E7)

#define ETHERNET_VIEW_POLL_MS 500U
#define ETHERNET_VIEW_PEER_DEFAULT "192.0.2.10"
#define ETHERNET_VIEW_PORT_DEFAULT "5001"
#define ETHERNET_VIEW_DURATION_DEFAULT "30"
#define ETHERNET_VIEW_INTERVAL_DEFAULT "3"

static lv_obj_t *s_parent;
static lv_obj_t *s_link_value;
static lv_obj_t *s_ip_value;
static lv_obj_t *s_netmask_value;
static lv_obj_t *s_gateway_value;
static lv_obj_t *s_mac_value;
static lv_obj_t *s_speed_value;
static lv_obj_t *s_duplex_value;
static lv_obj_t *s_packets_value;
static lv_obj_t *s_peer_field;
static lv_obj_t *s_port_field;
static lv_obj_t *s_duration_field;
static lv_obj_t *s_interval_field;
static lv_obj_t *s_tcp_button;
static lv_obj_t *s_udp_button;
static lv_obj_t *s_start_button;
static lv_obj_t *s_stop_button;
static lv_obj_t *s_renew_button;
static lv_obj_t *s_run_status;
static lv_obj_t *s_run_metrics;
static lv_obj_t *s_run_result;
static lv_obj_t *s_keyboard;
static lv_timer_t *s_poll_timer;
static app_ethernet_toast_cb_t s_toast;
static ethernet_iperf_protocol_t s_protocol = ETHERNET_IPERF_PROTOCOL_TCP;
static int8_t s_seen_instance_id = -1;

/** @brief Zero indicates that the page owns no throughput run. */
static uint32_t s_view_generation;
static bool s_stop_requested;
static bool s_generation_capture_pending;

static void style_plain(lv_obj_t *obj, lv_color_t color, int32_t radius)
{
    lv_obj_remove_style_all(obj);
    lv_obj_set_style_bg_color(obj, color, 0);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(obj, radius, 0);
    lv_obj_set_style_border_width(obj, 0, 0);
}

static lv_obj_t *make_label(lv_obj_t *parent, const char *text, lv_color_t color,
                            int32_t x, int32_t y)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_label_set_text(label, text != NULL ? text : "");
    lv_obj_set_pos(label, x, y);
    lv_obj_set_style_text_color(label, color, 0);
    return label;
}

static lv_obj_t *make_card(lv_obj_t *parent, int32_t x, int32_t y,
                           int32_t width, int32_t height)
{
    lv_obj_t *card = lv_obj_create(parent);
    style_plain(card, C_SURFACE, 14);
    lv_obj_set_pos(card, x, y);
    lv_obj_set_size(card, width, height);
    return card;
}

static lv_obj_t *make_button(lv_obj_t *parent, const char *text, int32_t x, int32_t y,
                             int32_t width, int32_t height, lv_color_t color,
                             lv_event_cb_t callback, void *user_data)
{
    lv_obj_t *button = lv_button_create(parent);
    style_plain(button, color, 12);
    lv_obj_set_pos(button, x, y);
    lv_obj_set_size(button, width, height);
    lv_obj_set_style_bg_color(button, C_ACCENT_SOFT, LV_STATE_PRESSED);
    if (callback != NULL) {
        lv_obj_add_event_cb(button, callback, LV_EVENT_CLICKED, user_data);
    }
    lv_obj_t *label = make_label(button, text, C_INK, 0, 0);
    lv_obj_center(label);
    return button;
}

static void set_value(lv_obj_t *label, const char *text, lv_color_t color)
{
    if (label != NULL) {
        lv_label_set_text(label, text != NULL ? text : "");
        lv_obj_set_style_text_color(label, color, 0);
    }
}

static void set_status(const char *text, lv_color_t color)
{
    set_value(s_run_status, text, color);
}

/**
 * @brief Parse a decimal field and reject values outside uint32_t range.
 * @param field LVGL text field containing the decimal number.
 * @param[out] value Receives the parsed number on success.
 * @return true if the field contains a valid uint32_t value.
 */
static bool parse_uint32_field(lv_obj_t *field, uint32_t *value)
{
    char *end = NULL;
    unsigned long parsed;
    const char *text;

    if (field == NULL || value == NULL) {
        return false;
    }
    text = lv_textarea_get_text(field);
    if (text == NULL || text[0] == '\0' || text[0] == '-' || text[0] == '+') {
        return false;
    }
    errno = 0;
    parsed = strtoul(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0' || parsed > UINT32_MAX) {
        return false;
    }
    *value = (uint32_t)parsed;
    return true;
}

static bool parse_peer_field(lv_obj_t *field, char *peer, size_t peer_size)
{
    struct in_addr address;
    const char *text;

    if (field == NULL || peer == NULL || peer_size < ETHERNET_IPERF_IPV4_TEXT_LEN) {
        return false;
    }
    text = lv_textarea_get_text(field);
    if (text == NULL || inet_pton(AF_INET, text, &address) != 1 || address.s_addr == htonl(INADDR_ANY)) {
        return false;
    }
    return inet_ntop(AF_INET, &address, peer, peer_size) != NULL;
}

static bool mac_is_zero(const uint8_t mac[ETHERNET_SERVICE_MAC_LEN])
{
    for (size_t index = 0; index < ETHERNET_SERVICE_MAC_LEN; ++index) {
        if (mac[index] != 0U) {
            return false;
        }
    }
    return true;
}

static const char *speed_text(const ethernet_service_status_t *status)
{
    if (!status->speed_valid) {
        return L(DESKTOP_TEXT_ETHERNET_SPEED_UNAVAILABLE);
    }
    switch (status->speed) {
    case ETH_SPEED_10M:
        return L(DESKTOP_TEXT_ETHERNET_SPEED_10);
    case ETH_SPEED_100M:
        return L(DESKTOP_TEXT_ETHERNET_SPEED_100);
    case ETH_SPEED_1000M:
        return L(DESKTOP_TEXT_ETHERNET_SPEED_1000);
    default:
        return L(DESKTOP_TEXT_ETHERNET_SPEED_UNAVAILABLE);
    }
}

static const char *duplex_text(const ethernet_service_status_t *status)
{
    if (!status->duplex_valid) {
        return L(DESKTOP_TEXT_ETHERNET_DUPLEX_UNAVAILABLE);
    }
    return status->duplex == ETH_DUPLEX_FULL ? L(DESKTOP_TEXT_ETHERNET_FULL_DUPLEX)
                                              : L(DESKTOP_TEXT_ETHERNET_HALF_DUPLEX);
}

/**
 * @brief Refresh link, address and driver status fields.
 * @param status Ethernet status snapshot.
 * @param ret Result of retrieving the status.
 */
static void update_network(const ethernet_service_status_t *status, esp_err_t ret)
{
    char ip_text[ETHERNET_IPERF_IPV4_TEXT_LEN];
    char netmask_text[ETHERNET_IPERF_IPV4_TEXT_LEN];
    char gateway_text[ETHERNET_IPERF_IPV4_TEXT_LEN];
    char mac_text[24];
    char packets_text[96];
    const bool available = ret == ESP_OK && status->enabled && status->configured && status->initialized;
    const bool link_up = available && status->link_up;
    const bool ip_valid = link_up && status->ip_valid;

    if (!available) {
        set_value(s_link_value, L(DESKTOP_TEXT_ETHERNET_UNAVAILABLE), C_WARNING);
        set_value(s_ip_value, L(DESKTOP_TEXT_ETHERNET_IP_UNAVAILABLE), C_WARNING);
        set_value(s_netmask_value, L(DESKTOP_TEXT_ETHERNET_NETMASK_UNAVAILABLE), C_WARNING);
        set_value(s_gateway_value, L(DESKTOP_TEXT_ETHERNET_GATEWAY_UNAVAILABLE), C_WARNING);
        set_value(s_mac_value, L(DESKTOP_TEXT_ETHERNET_MAC_UNAVAILABLE), C_WARNING);
        set_value(s_speed_value, L(DESKTOP_TEXT_ETHERNET_SPEED_UNAVAILABLE), C_WARNING);
        set_value(s_duplex_value, L(DESKTOP_TEXT_ETHERNET_DUPLEX_UNAVAILABLE), C_WARNING);
        set_value(s_packets_value, L(DESKTOP_TEXT_ETHERNET_PACKETS_UNAVAILABLE), C_WARNING);
        return;
    }

    set_value(s_link_value, link_up ? L(DESKTOP_TEXT_ETHERNET_LINK_UP)
                                    : L(DESKTOP_TEXT_ETHERNET_LINK_DOWN),
              link_up ? C_SUCCESS : C_WARNING);
    if (ip_valid) {
        snprintf(ip_text, sizeof(ip_text), IPSTR, IP2STR(&status->ip_info.ip));
        set_value(s_ip_value, ip_text, C_INK);
    } else {
        set_value(s_ip_value, L(DESKTOP_TEXT_ETHERNET_IP_UNAVAILABLE), C_WARNING);
    }
    if (ip_valid && status->ip_info.netmask.addr != 0) {
        snprintf(netmask_text, sizeof(netmask_text), IPSTR, IP2STR(&status->ip_info.netmask));
        set_value(s_netmask_value, netmask_text, C_INK);
    } else {
        set_value(s_netmask_value, L(DESKTOP_TEXT_ETHERNET_NETMASK_UNAVAILABLE), C_WARNING);
    }
    if (ip_valid && status->ip_info.gw.addr != 0) {
        snprintf(gateway_text, sizeof(gateway_text), IPSTR, IP2STR(&status->ip_info.gw));
        set_value(s_gateway_value, gateway_text, C_INK);
    } else {
        set_value(s_gateway_value, L(DESKTOP_TEXT_ETHERNET_GATEWAY_UNAVAILABLE), C_WARNING);
    }
    if (!mac_is_zero(status->mac)) {
        snprintf(mac_text, sizeof(mac_text), "%02X:%02X:%02X:%02X:%02X:%02X",
                 status->mac[0], status->mac[1], status->mac[2], status->mac[3],
                 status->mac[4], status->mac[5]);
        set_value(s_mac_value, mac_text, C_INK);
    } else {
        set_value(s_mac_value, L(DESKTOP_TEXT_ETHERNET_MAC_UNAVAILABLE), C_WARNING);
    }
    set_value(s_speed_value, speed_text(status), status->speed_valid ? C_INK : C_WARNING);
    set_value(s_duplex_value, duplex_text(status), status->duplex_valid ? C_INK : C_WARNING);
    if (status->rx_packets_valid && status->tx_packets_valid &&
        status->rx_bytes_valid && status->tx_bytes_valid) {
        snprintf(packets_text, sizeof(packets_text), L(DESKTOP_TEXT_ETHERNET_PACKETS_RESULT),
                 (unsigned int)status->rx_packets, (unsigned int)status->rx_bytes,
                 (unsigned int)status->tx_packets, (unsigned int)status->tx_bytes);
        set_value(s_packets_value, packets_text, C_INK);
    } else {
        set_value(s_packets_value, L(DESKTOP_TEXT_ETHERNET_PACKETS_UNAVAILABLE), C_WARNING);
    }
}

static const char *run_state_text(ethernet_iperf_state_t state)
{
    switch (state) {
    case ETHERNET_IPERF_STATE_STARTING:
        return L(DESKTOP_TEXT_ETHERNET_IPERF_STARTING);
    case ETHERNET_IPERF_STATE_RUNNING:
        return L(DESKTOP_TEXT_ETHERNET_IPERF_RUNNING);
    case ETHERNET_IPERF_STATE_STOPPING:
        return L(DESKTOP_TEXT_ETHERNET_IPERF_STOPPING);
    case ETHERNET_IPERF_STATE_SUCCEEDED:
        return L(DESKTOP_TEXT_ETHERNET_IPERF_SUCCEEDED);
    case ETHERNET_IPERF_STATE_CANCELED:
        return L(DESKTOP_TEXT_ETHERNET_IPERF_CANCELED);
    case ETHERNET_IPERF_STATE_FAILED:
        return L(DESKTOP_TEXT_ETHERNET_IPERF_FAILED);
    case ETHERNET_IPERF_STATE_IDLE:
    default:
        return L(DESKTOP_TEXT_ETHERNET_IPERF_READY);
    }
}

static lv_color_t run_state_color(ethernet_iperf_state_t state)
{
    switch (state) {
    case ETHERNET_IPERF_STATE_SUCCEEDED:
        return C_SUCCESS;
    case ETHERNET_IPERF_STATE_FAILED:
        return C_DANGER;
    case ETHERNET_IPERF_STATE_CANCELED:
        return C_WARNING;
    case ETHERNET_IPERF_STATE_RUNNING:
    case ETHERNET_IPERF_STATE_STARTING:
    case ETHERNET_IPERF_STATE_STOPPING:
        return C_ACCENT;
    case ETHERNET_IPERF_STATE_IDLE:
    default:
        return C_INK_MUTED;
    }
}

static void set_controls_enabled(bool enabled)
{
    lv_obj_t *controls[] = {s_tcp_button, s_udp_button, s_peer_field, s_port_field,
                            s_duration_field, s_interval_field, s_start_button,
                            s_renew_button};
    for (size_t index = 0; index < sizeof(controls) / sizeof(controls[0]); ++index) {
        if (controls[index] == NULL) {
            continue;
        }
        if (enabled) {
            lv_obj_clear_state(controls[index], LV_STATE_DISABLED);
        } else {
            lv_obj_add_state(controls[index], LV_STATE_DISABLED);
        }
        lv_obj_set_style_opa(controls[index], enabled ? LV_OPA_COVER : LV_OPA_50,
                             LV_STATE_DISABLED);
    }
    if (s_stop_button != NULL) {
        if (enabled) {
            lv_obj_add_state(s_stop_button, LV_STATE_DISABLED);
        } else {
            lv_obj_clear_state(s_stop_button, LV_STATE_DISABLED);
        }
        lv_obj_set_style_opa(s_stop_button, enabled ? LV_OPA_50 : LV_OPA_COVER, LV_STATE_DISABLED);
    }
    if (s_keyboard != NULL && !enabled) {
        lv_obj_add_flag(s_keyboard, LV_OBJ_FLAG_HIDDEN);
        lv_keyboard_set_textarea(s_keyboard, NULL);
    }
}

static void select_protocol_cb(lv_event_t *event)
{
    const ethernet_iperf_protocol_t protocol =
        (ethernet_iperf_protocol_t)(uintptr_t)lv_event_get_user_data(event);
    s_protocol = protocol;
    lv_obj_set_style_bg_color(s_tcp_button,
                              protocol == ETHERNET_IPERF_PROTOCOL_TCP ? C_ACCENT_SOFT : C_MUTED, 0);
    lv_obj_set_style_bg_color(s_udp_button,
                              protocol == ETHERNET_IPERF_PROTOCOL_UDP ? C_ACCENT_SOFT : C_MUTED, 0);
}

static void keyboard_cb(lv_event_t *event)
{
    if (lv_event_get_code(event) == LV_EVENT_READY || lv_event_get_code(event) == LV_EVENT_CANCEL) {
        lv_obj_add_flag(s_keyboard, LV_OBJ_FLAG_HIDDEN);
        lv_keyboard_set_textarea(s_keyboard, NULL);
    }
}

static void field_cb(lv_event_t *event)
{
    if (lv_event_get_code(event) == LV_EVENT_FOCUSED && s_keyboard != NULL) {
        lv_keyboard_set_textarea(s_keyboard, lv_event_get_target_obj(event));
        lv_obj_remove_flag(s_keyboard, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(s_keyboard);
    }
}

static void renew_cb(lv_event_t *event)
{
    (void)event;
    const esp_err_t ret = ethernet_service_dhcp_renew();
    if (ret == ESP_OK) {
        set_status(L(DESKTOP_TEXT_ETHERNET_RENEW), C_ACCENT);
        if (s_toast != NULL) {
            s_toast(L(DESKTOP_TEXT_WIFI_RENEWED));
        }
    } else {
        set_status(L(DESKTOP_TEXT_ETHERNET_UNAVAILABLE), C_WARNING);
    }
}

/**
 * @brief Validate throughput parameters and capture the new run generation.
 * @param event LVGL event that triggered the callback.
 */
static void start_cb(lv_event_t *event)
{
    char peer[ETHERNET_IPERF_IPV4_TEXT_LEN];
    uint32_t port;
    uint32_t duration;
    uint32_t interval;
    ethernet_service_status_t network;
    ethernet_iperf_config_t config;
    ethernet_iperf_snapshot_t snapshot;
    esp_err_t ret;

    (void)event;
    if (!parse_peer_field(s_peer_field, peer, sizeof(peer)) ||
        !parse_uint32_field(s_port_field, &port) ||
        !parse_uint32_field(s_duration_field, &duration) ||
        !parse_uint32_field(s_interval_field, &interval) ||
        port < ETHERNET_IPERF_MIN_PORT || port > ETHERNET_IPERF_MAX_PORT ||
        duration < ETHERNET_IPERF_MIN_DURATION_SEC || duration > ETHERNET_IPERF_MAX_DURATION_SEC ||
        interval < ETHERNET_IPERF_MIN_INTERVAL_SEC || interval > ETHERNET_IPERF_MAX_INTERVAL_SEC ||
        interval > duration) {
        set_status(L(DESKTOP_TEXT_ETHERNET_IPERF_INVALID_INPUT), C_DANGER);
        return;
    }

    ret = ethernet_service_get_status(&network);
    if (ret != ESP_OK || !network.enabled || !network.configured || !network.initialized ||
        !network.link_up || !network.ip_valid) {
        set_status(L(DESKTOP_TEXT_ETHERNET_IPERF_LINK_REQUIRED), C_WARNING);
        return;
    }

    config = (ethernet_iperf_config_t){
        .protocol = s_protocol,
        .peer_ipv4 = peer,
        .port = port,
        .duration_sec = duration,
        .interval_sec = interval,
    };
    ret = ethernet_iperf_start(&config);
    if (ret != ESP_OK && ret != ESP_ERR_TIMEOUT) {
        if (ret == ESP_ERR_INVALID_STATE) {
            const bool link_ready = ethernet_service_get_status(&network) == ESP_OK &&
                                     network.enabled && network.configured && network.initialized &&
                                     network.link_up && network.ip_valid;
            set_status(link_ready ? L(DESKTOP_TEXT_ETHERNET_IPERF_RUNNING)
                                  : L(DESKTOP_TEXT_ETHERNET_IPERF_LINK_REQUIRED),
                       link_ready ? C_DANGER : C_WARNING);
        } else {
            set_status(L(DESKTOP_TEXT_ETHERNET_IPERF_FAILED), C_DANGER);
        }
        return;
    }
    snapshot = (ethernet_iperf_snapshot_t){0};
    s_generation_capture_pending = true;
    if (ethernet_iperf_get_snapshot(&snapshot) == ESP_OK && snapshot.generation != 0U) {
        s_view_generation = snapshot.generation;
        s_generation_capture_pending = false;
    } else {
        ESP_LOGW("app_ethernet_view", "iPerf generation capture deferred; retrying from poll");
    }
    s_seen_instance_id = -1;
    s_stop_requested = false;
    set_controls_enabled(false);
    set_status(L(DESKTOP_TEXT_ETHERNET_IPERF_STARTING), C_ACCENT);
    set_value(s_run_result, L(DESKTOP_TEXT_ETHERNET_IPERF_NO_RESULT), C_INK_MUTED);
}

static void stop_cb(lv_event_t *event)
{
    (void)event;
    if (s_stop_requested) {
        return;
    }
    const esp_err_t ret = ethernet_iperf_request_stop();
    if (ret == ESP_OK) {
        s_stop_requested = true;
        set_status(L(DESKTOP_TEXT_ETHERNET_IPERF_STOPPING), C_ACCENT);
    } else if (ret == ESP_ERR_TIMEOUT) {
        s_stop_requested = true;
        set_status(L(DESKTOP_TEXT_ETHERNET_IPERF_STOPPING), C_ACCENT);
    } else if (ret != ESP_ERR_INVALID_STATE) {
        set_status(L(DESKTOP_TEXT_ETHERNET_IPERF_FAILED), C_DANGER);
    }
}

/**
 * @brief Refresh throughput metrics and resolve pending run ownership.
 * @param snapshot Throughput run snapshot.
 * @param ret Result of retrieving the snapshot.
 */
static void update_run(const ethernet_iperf_snapshot_t *snapshot, esp_err_t ret)
{
    char progress[64];
    char throughput[96];
    char transfer[96];
    char metrics[280];
    char result[128];
    const bool owned = ret == ESP_OK && snapshot->generation != 0U &&
                      snapshot->generation == s_view_generation;
    const bool active = owned && snapshot->active;

    if (ret != ESP_OK) {
        return;
    }
    if (owned && (active || snapshot->terminal) && s_seen_instance_id < 0 &&
        snapshot->instance_id > 0) {
        s_seen_instance_id = snapshot->instance_id;
    }
    const bool terminal = owned && snapshot->terminal &&
                          snapshot->instance_id > 0 && snapshot->instance_id == s_seen_instance_id;
    set_controls_enabled(!active);
    if (s_stop_button != NULL && active) {
        if (snapshot->stop_pending) {
            lv_obj_add_state(s_stop_button, LV_STATE_DISABLED);
        } else {
            lv_obj_clear_state(s_stop_button, LV_STATE_DISABLED);
        }
    }
    if (!active) {
        s_stop_requested = false;
    }
    if (active || terminal) {
        set_status(run_state_text(snapshot->state), run_state_color(snapshot->state));
        snprintf(progress, sizeof(progress), L(DESKTOP_TEXT_ETHERNET_IPERF_PROGRESS),
                 (unsigned int)snapshot->progress_percent, (unsigned int)snapshot->elapsed_sec,
                 (unsigned int)snapshot->duration_sec);
        if (snapshot->report_valid) {
            snprintf(throughput, sizeof(throughput), L(DESKTOP_TEXT_ETHERNET_IPERF_THROUGHPUT),
                     (unsigned int)snapshot->period_throughput_bps,
                     (unsigned int)snapshot->average_throughput_bps);
            snprintf(transfer, sizeof(transfer), L(DESKTOP_TEXT_ETHERNET_IPERF_TRANSFER),
                     (unsigned long long)snapshot->period_transfer_bytes,
                     (unsigned long long)snapshot->total_transfer_bytes);
            snprintf(metrics, sizeof(metrics), "%s\n%s\n%s", progress, throughput, transfer);
        } else {
            snprintf(metrics, sizeof(metrics), "%s", progress);
        }
        set_value(s_run_metrics, metrics, run_state_color(snapshot->state));
    } else if (s_seen_instance_id < 0) {
        set_status(L(DESKTOP_TEXT_ETHERNET_IPERF_READY), C_INK_MUTED);
        set_value(s_run_metrics, "", C_INK_MUTED);
    }
    if (terminal && snapshot->report_valid) {
        snprintf(result, sizeof(result), L(DESKTOP_TEXT_ETHERNET_IPERF_RESULT),
                 run_state_text(snapshot->state), (unsigned int)snapshot->elapsed_sec,
                 (unsigned long long)snapshot->total_transfer_bytes);
        set_value(s_run_result, result, run_state_color(snapshot->state));
    }
}

static void poll_cb(lv_timer_t *timer)
{
    ethernet_service_status_t network = {0};
    ethernet_iperf_snapshot_t snapshot = {0};
    esp_err_t network_ret;
    esp_err_t run_ret;

    (void)timer;
    network_ret = ethernet_service_get_status(&network);
    update_network(&network, network_ret);
    run_ret = ethernet_iperf_get_snapshot(&snapshot);
    if (s_generation_capture_pending && run_ret == ESP_OK && snapshot.generation != 0U) {
        s_view_generation = snapshot.generation;
        s_generation_capture_pending = false;
    }
    update_run(&snapshot, run_ret);
}

static void make_flex_field(lv_obj_t *parent, const char *label, const char *value,
                            int32_t width_percent, lv_obj_t **field)
{
    lv_obj_t *group = lv_obj_create(parent);
    style_plain(group, C_SURFACE, 0);
    lv_obj_set_width(group, LV_PCT(width_percent));
    lv_obj_set_height(group, 76);
    lv_obj_set_flex_flow(group, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(group, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(group, 4, 0);
    lv_obj_t *caption = make_label(group, label, C_INK_MUTED, 0, 0);
    lv_obj_set_width(caption, LV_PCT(100));
    *field = lv_textarea_create(group);
    style_plain(*field, C_SURFACE, 10);
    lv_obj_set_size(*field, LV_PCT(100), 48);
    lv_obj_set_style_border_width(*field, 1, 0);
    lv_obj_set_style_border_color(*field, C_DIVIDER, 0);
    lv_obj_set_style_text_color(*field, C_INK, 0);
    lv_obj_set_style_pad_left(*field, 10, 0);
    lv_textarea_set_one_line(*field, true);
    lv_textarea_set_text(*field, value);
    lv_obj_add_event_cb(*field, field_cb, LV_EVENT_ALL, NULL);
}

static lv_obj_t *make_flex_value_card(lv_obj_t *parent, const char *title)
{
    lv_obj_t *card = make_card(parent, 0, 0, 1, 48);
    lv_obj_set_width(card, LV_PCT(48));
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(card, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_hor(card, 14, 0);
    lv_obj_set_style_pad_column(card, 8, 0);
    make_label(card, title, C_INK_MUTED, 0, 0);
    lv_obj_t *value = make_label(card, L(DESKTOP_TEXT_ETHERNET_UNAVAILABLE), C_INK, 0, 0);
    lv_obj_set_flex_grow(value, 1);
    lv_obj_set_style_text_align(value, LV_TEXT_ALIGN_RIGHT, 0);
    lv_label_set_long_mode(value, LV_LABEL_LONG_DOT);
    return value;
}

static void build_fields(lv_obj_t *panel)
{
    lv_obj_set_style_pad_all(panel, 12, 0);
    lv_obj_set_style_pad_row(panel, 8, 0);
    lv_obj_t *title = make_label(panel, L(DESKTOP_TEXT_ETHERNET_IPERF_TITLE), C_INK, 0, 0);
    lv_obj_set_width(title, LV_PCT(100));

    lv_obj_t *protocols = lv_obj_create(panel);
    style_plain(protocols, C_SURFACE, 0);
    lv_obj_set_size(protocols, LV_PCT(100), 50);
    lv_obj_set_flex_flow(protocols, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(protocols, 8, 0);
    lv_obj_set_flex_align(protocols, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    s_tcp_button = make_button(protocols, L(DESKTOP_TEXT_ETHERNET_IPERF_TCP), 0, 0, 1, 48,
                               C_ACCENT_SOFT, select_protocol_cb,
                               (void *)(uintptr_t)ETHERNET_IPERF_PROTOCOL_TCP);
    s_udp_button = make_button(protocols, L(DESKTOP_TEXT_ETHERNET_IPERF_UDP), 0, 0, 1, 48,
                               C_MUTED, select_protocol_cb,
                               (void *)(uintptr_t)ETHERNET_IPERF_PROTOCOL_UDP);
    lv_obj_set_width(s_tcp_button, LV_PCT(49));
    lv_obj_set_width(s_udp_button, LV_PCT(49));

    lv_obj_t *fields = lv_obj_create(panel);
    style_plain(fields, C_SURFACE, 0);
    lv_obj_set_width(fields, LV_PCT(100));
    lv_obj_set_height(fields, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(fields, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_row(fields, 8, 0);
    lv_obj_set_style_pad_column(fields, 8, 0);
    lv_obj_set_flex_align(fields, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START);
    make_flex_field(fields, L(DESKTOP_TEXT_ETHERNET_IPERF_PEER), ETHERNET_VIEW_PEER_DEFAULT,
                    48, &s_peer_field);
    make_flex_field(fields, L(DESKTOP_TEXT_ETHERNET_IPERF_PORT), ETHERNET_VIEW_PORT_DEFAULT,
                    23, &s_port_field);
    make_flex_field(fields, L(DESKTOP_TEXT_ETHERNET_IPERF_DURATION), ETHERNET_VIEW_DURATION_DEFAULT,
                    23, &s_duration_field);
    make_flex_field(fields, L(DESKTOP_TEXT_ETHERNET_IPERF_INTERVAL), ETHERNET_VIEW_INTERVAL_DEFAULT,
                    23, &s_interval_field);
    lv_textarea_set_accepted_chars(s_peer_field, "0123456789.");
    lv_textarea_set_accepted_chars(s_port_field, "0123456789");
    lv_textarea_set_accepted_chars(s_duration_field, "0123456789");
    lv_textarea_set_accepted_chars(s_interval_field, "0123456789");

    lv_obj_t *actions = lv_obj_create(panel);
    style_plain(actions, C_SURFACE, 0);
    lv_obj_set_size(actions, LV_PCT(100), 50);
    lv_obj_set_flex_flow(actions, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(actions, 8, 0);
    lv_obj_set_flex_align(actions, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    s_start_button = make_button(actions, L(DESKTOP_TEXT_ETHERNET_IPERF_START), 0, 0, 1, 48,
                                 C_ACCENT_SOFT, start_cb, NULL);
    s_stop_button = make_button(actions, L(DESKTOP_TEXT_ETHERNET_IPERF_STOP), 0, 0, 1, 48,
                                C_MUTED, stop_cb, NULL);
    s_renew_button = make_button(actions, L(DESKTOP_TEXT_ETHERNET_RENEW), 0, 0, 1, 48,
                                 C_MUTED, renew_cb, NULL);
    lv_obj_set_width(s_start_button, LV_PCT(32));
    lv_obj_set_width(s_stop_button, LV_PCT(32));
    lv_obj_set_width(s_renew_button, LV_PCT(32));

    s_run_status = make_label(panel, L(DESKTOP_TEXT_ETHERNET_IPERF_READY), C_INK_MUTED, 0, 0);
    s_run_metrics = make_label(panel, "", C_INK_MUTED, 0, 0);
    s_run_result = make_label(panel, L(DESKTOP_TEXT_ETHERNET_IPERF_NO_RESULT), C_INK_MUTED, 0, 0);
    lv_obj_set_width(s_run_status, LV_PCT(100));
    lv_obj_set_width(s_run_metrics, LV_PCT(100));
    lv_obj_set_width(s_run_result, LV_PCT(100));
    lv_label_set_long_mode(s_run_metrics, LV_LABEL_LONG_WRAP);
    lv_label_set_long_mode(s_run_result, LV_LABEL_LONG_WRAP);
    set_controls_enabled(true);
    lv_obj_add_state(s_stop_button, LV_STATE_DISABLED);
}

/**
 * @brief Stop polling, request throughput shutdown and clear page state.
 */
void app_ethernet_view_cleanup(void)
{
    if (s_poll_timer != NULL) {
        lv_timer_delete(s_poll_timer);
        s_poll_timer = NULL;
    }
    const esp_err_t ret = ethernet_iperf_request_stop();
    if (ret == ESP_OK) {
        ESP_LOGI("app_ethernet_view", "iPerf cleanup requested asynchronous stop");
    } else if (ret == ESP_ERR_TIMEOUT) {
        ESP_LOGI("app_ethernet_view", "iPerf cleanup queued asynchronous stop while busy");
    } else if (ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGW("app_ethernet_view", "iPerf cleanup request returned %s",
                 esp_err_to_name(ret));
    }
    s_parent = NULL;
    s_link_value = NULL;
    s_ip_value = NULL;
    s_netmask_value = NULL;
    s_gateway_value = NULL;
    s_mac_value = NULL;
    s_speed_value = NULL;
    s_duplex_value = NULL;
    s_packets_value = NULL;
    s_peer_field = NULL;
    s_port_field = NULL;
    s_duration_field = NULL;
    s_interval_field = NULL;
    s_tcp_button = NULL;
    s_udp_button = NULL;
    s_start_button = NULL;
    s_stop_button = NULL;
    s_renew_button = NULL;
    s_run_status = NULL;
    s_run_metrics = NULL;
    s_run_result = NULL;
    s_keyboard = NULL;
    s_toast = NULL;
    s_seen_instance_id = -1;
    s_view_generation = 0U;
    s_generation_capture_pending = false;
    s_stop_requested = false;
}

void app_ethernet_view_build(lv_obj_t *parent, app_ethernet_toast_cb_t toast_cb)
{
    app_ethernet_view_cleanup();
    s_parent = parent;
    s_toast = toast_cb;
    s_protocol = ETHERNET_IPERF_PROTOCOL_TCP;
    s_seen_instance_id = -1;
    s_view_generation = 0U;
    s_generation_capture_pending = false;
    s_stop_requested = false;

    const desktop_fullscreen_layout_colors_t colors = {
        .canvas = lv_color_hex(0xDCE8F5),
        .surface = C_SURFACE,
        .ink = C_INK,
        .ink_muted = C_INK_MUTED,
    };
    desktop_fullscreen_layout_t layout;
    if (!desktop_fullscreen_layout_create(parent, &colors, "", "", "", &layout)) {
        return;
    }

    desktop_fullscreen_layout_remove_page_header(&layout);

    lv_obj_t *network = desktop_fullscreen_layout_create_panel(
        layout.content, colors.canvas, 42, 0, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(network, 8, 0);
    lv_obj_t *values = lv_obj_create(network);
    style_plain(values, colors.canvas, 0);
    lv_obj_set_width(values, LV_PCT(100));
    lv_obj_set_flex_grow(values, 1);
    lv_obj_set_flex_flow(values, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_row(values, 8, 0);
    lv_obj_set_style_pad_column(values, 8, 0);
    lv_obj_set_flex_align(values, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START);
    s_link_value = make_flex_value_card(values, L(DESKTOP_TEXT_ETHERNET_LINK));
    s_ip_value = make_flex_value_card(values, L(DESKTOP_TEXT_ETHERNET_IP));
    s_netmask_value = make_flex_value_card(values, L(DESKTOP_TEXT_ETHERNET_NETMASK));
    s_gateway_value = make_flex_value_card(values, L(DESKTOP_TEXT_ETHERNET_GATEWAY));
    s_mac_value = make_flex_value_card(values, L(DESKTOP_TEXT_ETHERNET_MAC));
    s_speed_value = make_flex_value_card(values, L(DESKTOP_TEXT_ETHERNET_RATE));
    s_duplex_value = make_flex_value_card(values, L(DESKTOP_TEXT_ETHERNET_DUPLEX));
    s_packets_value = make_flex_value_card(values, L(DESKTOP_TEXT_ETHERNET_PACKETS));

    lv_obj_t *iperf = desktop_fullscreen_layout_create_panel(
        layout.content, C_SURFACE, 57, 14, LV_FLEX_FLOW_COLUMN);
    build_fields(iperf);

    s_keyboard = lv_keyboard_create(parent);
    lv_keyboard_set_mode(s_keyboard, LV_KEYBOARD_MODE_NUMBER);
    lv_obj_set_size(s_keyboard, LV_PCT(92), LV_PCT(40));
    lv_obj_align(s_keyboard, LV_ALIGN_BOTTOM_MID, 0, -24);
    lv_obj_add_flag(s_keyboard, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(s_keyboard, keyboard_cb, LV_EVENT_ALL, NULL);
    s_poll_timer = lv_timer_create(poll_cb, ETHERNET_VIEW_POLL_MS, NULL);
    if (s_poll_timer == NULL) {
        set_status(L(DESKTOP_TEXT_ETHERNET_UNAVAILABLE), C_WARNING);
        ESP_LOGE("app_ethernet_view", "Ethernet status timer allocation failed");
    }
    poll_cb(NULL);
}
