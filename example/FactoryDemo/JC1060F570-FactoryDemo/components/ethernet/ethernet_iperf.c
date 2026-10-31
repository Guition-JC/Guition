#include "ethernet_iperf.h"

#include <arpa/inet.h>
#include <errno.h>
#include <string.h>

#include "ethernet_service.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "freertos/portmacro.h"
#include "iperf.h"

#define ETHERNET_IPERF_TAG "ethernet_iperf"
#define ETHERNET_IPERF_LOCK_TIMEOUT_MS 100U
#define ETHERNET_IPERF_NETWORK_POLL_MS 100U
#define ETHERNET_IPERF_NETWORK_TASK_STACK 3072U
#define ETHERNET_IPERF_NETWORK_TASK_PRIORITY 5U

/** Require the instance-based iPerf component API. */
_Static_assert(__builtin_types_compatible_p(__typeof__(&iperf_start_instance),
                                            iperf_id_t (*)(const iperf_cfg_t *)),
               "official iperf_start_instance signature changed");
_Static_assert(__builtin_types_compatible_p(__typeof__(&iperf_stop_instance),
                                            esp_err_t (*)(iperf_id_t)),
               "official iperf_stop_instance signature changed");
_Static_assert(__builtin_types_compatible_p(__typeof__(&iperf_get_traffic_report),
                                            esp_err_t (*)(iperf_id_t, iperf_traffic_report_t *)),
               "official iperf_get_traffic_report signature changed");
_Static_assert(sizeof(((iperf_cfg_t *)0)->bw_lim) == sizeof(int32_t),
               "official iperf bandwidth field must remain int32_t");

static StaticSemaphore_t s_lock_storage;
static SemaphoreHandle_t s_lock;
static portMUX_TYPE s_lock_init_mux = portMUX_INITIALIZER_UNLOCKED;
static portMUX_TYPE s_lifecycle_mux = portMUX_INITIALIZER_UNLOCKED;
static ethernet_iperf_snapshot_t s_snapshot = {
    .state = ETHERNET_IPERF_STATE_IDLE,
    .protocol = ETHERNET_IPERF_PROTOCOL_TCP,
    .instance_id = -1,
};
static bool s_cancel_requested;
static bool s_started;
static iperf_id_t s_instance_id = -1;
static uint32_t s_run_generation;
static TaskHandle_t s_network_monitor_task;
static bool s_run_active;
static uint32_t s_lifecycle_generation;
static bool s_stop_request_pending;
static uint32_t s_stop_request_generation;
static iperf_id_t s_pending_instance_id = -1;
static uint32_t s_pending_instance_generation;
static bool s_failure_pending;
static uint32_t s_failure_generation;
static iperf_id_t s_failure_instance_id = -1;
static esp_err_t s_failure_error;
typedef struct {
    uint32_t generation;
    iperf_id_t instance_id;
    iperf_state_t state;
    bool report_valid;
    iperf_traffic_report_t report;
} pending_callback_t;
static pending_callback_t s_callback_pending;
typedef struct {
    uint32_t generation;
} network_monitor_context_t;
static network_monitor_context_t s_network_monitor_context;
static uint32_t s_start_link_loss_count;
static uint32_t s_start_ip_loss_count;

static bool ensure_lock(void)
{
    bool initialized;

    portENTER_CRITICAL(&s_lock_init_mux);
    if (s_lock == NULL) {
        s_lock = xSemaphoreCreateMutexStatic(&s_lock_storage);
    }
    initialized = s_lock != NULL;
    portEXIT_CRITICAL(&s_lock_init_mux);
    return initialized;
}

static bool lock_wrapper(TickType_t timeout)
{
    return s_lock != NULL && xSemaphoreTake(s_lock, timeout) == pdTRUE;
}

static bool lifecycle_run_is_active(void)
{
    bool active;

    portENTER_CRITICAL(&s_lifecycle_mux);
    active = s_run_active;
    portEXIT_CRITICAL(&s_lifecycle_mux);
    return active;
}

static bool monitor_task_exists(void)
{
    bool exists;

    portENTER_CRITICAL(&s_lifecycle_mux);
    exists = s_network_monitor_task != NULL;
    portEXIT_CRITICAL(&s_lifecycle_mux);
    return exists;
}

static void lifecycle_begin_run(uint32_t generation)
{
    portENTER_CRITICAL(&s_lifecycle_mux);
    s_run_active = true;
    s_lifecycle_generation = generation;
    s_stop_request_pending = false;
    s_stop_request_generation = 0U;
    s_pending_instance_id = -1;
    s_pending_instance_generation = 0U;
    memset(&s_callback_pending, 0, sizeof(s_callback_pending));
    s_failure_pending = false;
    portEXIT_CRITICAL(&s_lifecycle_mux);
}

static void lifecycle_mark_inactive(uint32_t generation)
{
    portENTER_CRITICAL(&s_lifecycle_mux);
    if (s_lifecycle_generation == generation) {
        s_run_active = false;
        s_stop_request_pending = false;
        s_stop_request_generation = 0U;
    }
    portEXIT_CRITICAL(&s_lifecycle_mux);
}

static void lifecycle_queue_stop_request(void)
{
    portENTER_CRITICAL(&s_lifecycle_mux);
    if (s_run_active) {
        s_stop_request_pending = true;
        s_stop_request_generation = s_lifecycle_generation;
    }
    portEXIT_CRITICAL(&s_lifecycle_mux);
}

static bool lifecycle_stop_request_pending(uint32_t generation)
{
    bool pending;

    portENTER_CRITICAL(&s_lifecycle_mux);
    pending = s_stop_request_pending && s_stop_request_generation == generation;
    portEXIT_CRITICAL(&s_lifecycle_mux);
    return pending;
}

static void lifecycle_clear_stop_request(uint32_t generation)
{
    portENTER_CRITICAL(&s_lifecycle_mux);
    if (s_stop_request_generation == generation) {
        s_stop_request_pending = false;
        s_stop_request_generation = 0U;
    }
    portEXIT_CRITICAL(&s_lifecycle_mux);
}

static void lifecycle_publish_instance(uint32_t generation, iperf_id_t instance_id)
{
    portENTER_CRITICAL(&s_lifecycle_mux);
    if (s_lifecycle_generation == generation && s_run_active) {
        s_pending_instance_id = instance_id;
        s_pending_instance_generation = generation;
    }
    portEXIT_CRITICAL(&s_lifecycle_mux);
}

static iperf_id_t lifecycle_take_pending_instance(uint32_t generation)
{
    iperf_id_t instance_id = -1;

    portENTER_CRITICAL(&s_lifecycle_mux);
    if (s_pending_instance_generation == generation) {
        instance_id = s_pending_instance_id;
        s_pending_instance_id = -1;
        s_pending_instance_generation = 0U;
    }
    portEXIT_CRITICAL(&s_lifecycle_mux);
    return instance_id;
}

static void lifecycle_queue_failure(uint32_t generation, iperf_id_t instance_id,
                                    esp_err_t error)
{
    portENTER_CRITICAL(&s_lifecycle_mux);
    if (s_lifecycle_generation == generation) {
        s_run_active = false;
        s_failure_pending = true;
        s_failure_generation = generation;
        s_failure_instance_id = instance_id;
        s_failure_error = error;
    }
    portEXIT_CRITICAL(&s_lifecycle_mux);
}

static bool lifecycle_take_failure(uint32_t generation, iperf_id_t instance_id,
                                   esp_err_t *error)
{
    bool pending = false;

    portENTER_CRITICAL(&s_lifecycle_mux);
    if (s_failure_pending && s_failure_generation == generation &&
        (s_failure_instance_id <= 0 || s_failure_instance_id == instance_id)) {
        pending = true;
        if (error != NULL) {
            *error = s_failure_error;
        }
        s_failure_pending = false;
    }
    portEXIT_CRITICAL(&s_lifecycle_mux);
    return pending;
}

static void lifecycle_queue_callback(uint32_t generation, iperf_id_t instance_id,
                                     iperf_state_t state,
                                     const iperf_traffic_report_t *report)
{
    portENTER_CRITICAL(&s_lifecycle_mux);
    if (s_lifecycle_generation == generation && s_run_active &&
        (state == IPERF_STARTED || state == IPERF_RUNNING ||
         state == IPERF_STOPPED || state == IPERF_CLOSED)) {
        s_callback_pending.generation = generation;
        s_callback_pending.instance_id = instance_id;
        s_callback_pending.state = state;
        s_callback_pending.report_valid = report != NULL;
        if (report != NULL) {
            s_callback_pending.report = *report;
        }
    }
    portEXIT_CRITICAL(&s_lifecycle_mux);
}

static bool lifecycle_take_callback(uint32_t generation, iperf_id_t instance_id,
                                    iperf_state_t *state,
                                    iperf_traffic_report_t *report, bool *report_valid)
{
    bool pending = false;

    portENTER_CRITICAL(&s_lifecycle_mux);
    if (s_callback_pending.generation == generation &&
        (instance_id <= 0 || s_callback_pending.instance_id == instance_id)) {
        pending = true;
        if (state != NULL) {
            *state = s_callback_pending.state;
        }
        if (report_valid != NULL) {
            *report_valid = s_callback_pending.report_valid;
        }
        if (report != NULL && s_callback_pending.report_valid) {
            *report = s_callback_pending.report;
        }
        memset(&s_callback_pending, 0, sizeof(s_callback_pending));
    }
    portEXIT_CRITICAL(&s_lifecycle_mux);
    return pending;
}

static void clear_monitor_task(void)
{
    const TaskHandle_t current = xTaskGetCurrentTaskHandle();

    portENTER_CRITICAL(&s_lifecycle_mux);
    if (s_network_monitor_task == current) {
        s_network_monitor_task = NULL;
    }
    portEXIT_CRITICAL(&s_lifecycle_mux);
}

static bool protocol_is_valid(ethernet_iperf_protocol_t protocol)
{
    return protocol == ETHERNET_IPERF_PROTOCOL_TCP ||
           protocol == ETHERNET_IPERF_PROTOCOL_UDP;
}

static bool ipv4_in_range(uint32_t address, uint32_t network, uint32_t mask)
{
    return (address & mask) == network;
}

static bool ipv4_is_non_peer_range(uint32_t address)
{
    return ipv4_in_range(address, 0x00000000U, 0xff000000U) || /**< this network */
           ipv4_in_range(address, 0x64400000U, 0xffc00000U) || /**< shared address space */
           ipv4_in_range(address, 0x7f000000U, 0xff000000U) || /**< loopback */
           ipv4_in_range(address, 0xa9fe0000U, 0xffff0000U) || /**< link-local */
           ipv4_in_range(address, 0xc0000000U, 0xffffff00U) || /**< IETF protocol assignments */
           ipv4_in_range(address, 0xc0000200U, 0xffffff00U) || /**< TEST-NET-1 */
           ipv4_in_range(address, 0xc01fc400U, 0xffffff00U) || /**< AS112-v4 */
           ipv4_in_range(address, 0xc034c100U, 0xffffff00U) || /**< AMT */
           ipv4_in_range(address, 0xc0586300U, 0xffffff00U) || /**< deprecated 6to4 anycast */
           ipv4_in_range(address, 0xc6120000U, 0xfffe0000U) || /**< benchmarking */
           ipv4_in_range(address, 0xc6336400U, 0xffffff00U) || /**< TEST-NET-2 */
           ipv4_in_range(address, 0xcb007100U, 0xffffff00U) || /**< TEST-NET-3 */
           ipv4_in_range(address, 0xe0000000U, 0xf0000000U) || /**< multicast */
           ipv4_in_range(address, 0xf0000000U, 0xf0000000U) || /**< reserved */
           address == INADDR_BROADCAST;
}

static bool parse_peer_ipv4(const char *text, esp_ip_addr_t *address,
                            char canonical[ETHERNET_IPERF_IPV4_TEXT_LEN])
{
    struct in_addr parsed;

    if (text == NULL || address == NULL || canonical == NULL ||
        inet_pton(AF_INET, text, &parsed) != 1) {
        return false;
    }

    const uint32_t host_addr = ntohl(parsed.s_addr);
    if (ipv4_is_non_peer_range(host_addr)) {
        return false;
    }
    if (inet_ntop(AF_INET, &parsed, canonical, ETHERNET_IPERF_IPV4_TEXT_LEN) == NULL) {
        return false;
    }

    memset(address, 0, sizeof(*address));
    address->type = ESP_IPADDR_TYPE_V4;
    address->u_addr.ip4.addr = parsed.s_addr;
    return true;
}

static esp_err_t validate_config(const ethernet_iperf_config_t *config,
                                 esp_ip_addr_t *destination,
                                 char canonical_peer[ETHERNET_IPERF_IPV4_TEXT_LEN])
{
    if (config == NULL || destination == NULL || canonical_peer == NULL ||
        !protocol_is_valid(config->protocol) ||
        config->port < ETHERNET_IPERF_MIN_PORT ||
        config->port > ETHERNET_IPERF_MAX_PORT ||
        config->duration_sec < ETHERNET_IPERF_MIN_DURATION_SEC ||
        config->duration_sec > ETHERNET_IPERF_MAX_DURATION_SEC ||
        config->interval_sec < ETHERNET_IPERF_MIN_INTERVAL_SEC ||
        config->interval_sec > ETHERNET_IPERF_MAX_INTERVAL_SEC ||
        config->interval_sec > config->duration_sec) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!parse_peer_ipv4(config->peer_ipv4, destination, canonical_peer)) {
        return ESP_ERR_INVALID_ARG;
    }
    return ESP_OK;
}

static uint32_t protocol_flag(ethernet_iperf_protocol_t protocol)
{
    return protocol == ETHERNET_IPERF_PROTOCOL_UDP ? IPERF_FLAG_UDP : IPERF_FLAG_TCP;
}

static uint32_t report_throughput_bps(double bytes, uint32_t elapsed_sec)
{
    const double bits_per_sec = elapsed_sec == 0U ? 0.0 : (bytes * 8.0) / elapsed_sec;

    if (bits_per_sec <= 0.0) {
        return 0U;
    }
    if (bits_per_sec >= (double)UINT32_MAX) {
        return UINT32_MAX;
    }
    return (uint32_t)bits_per_sec;
}

static void snapshot_report(const iperf_traffic_report_t *report)
{
    uint32_t progress;

    if (report == NULL) {
        return;
    }
    s_snapshot.report_valid = true;
    s_snapshot.elapsed_sec = report->end_sec;
    s_snapshot.period_transfer_bytes = report->period_bytes > 0.0 ?
                                       (uint64_t)report->period_bytes : 0U;
    s_snapshot.period_throughput_bps = report_throughput_bps(
        report->period_bytes, report->end_sec > report->period_start_sec ?
                              report->end_sec - report->period_start_sec : 0U);
    s_snapshot.average_throughput_bps = report_throughput_bps(
        (double)report->total_transfer_bytes, report->end_sec);
    s_snapshot.total_transfer_bytes = report->total_transfer_bytes;
    progress = s_snapshot.duration_sec == 0U ? 0U :
               (uint32_t)(((uint64_t)report->end_sec * 100U) /
                          s_snapshot.duration_sec);
    s_snapshot.progress_percent = progress > 100U ? 100U : progress;
}

static void apply_closed_locked(iperf_id_t instance_id, const iperf_traffic_report_t *report,
                                bool report_ok)
{
    if (report_ok) {
        snapshot_report(report);
    }
    s_snapshot.active = false;
    s_snapshot.terminal = true;
    s_snapshot.stop_pending = false;
    if (s_cancel_requested) {
        s_snapshot.state = ETHERNET_IPERF_STATE_CANCELED;
        s_snapshot.last_error = ESP_OK;
    } else if (s_started && report_ok && report->end_sec >= s_snapshot.duration_sec) {
        s_snapshot.state = ETHERNET_IPERF_STATE_SUCCEEDED;
        s_snapshot.last_error = ESP_OK;
    } else {
        s_snapshot.state = ETHERNET_IPERF_STATE_FAILED;
        s_snapshot.last_error = ESP_FAIL;
    }
    s_instance_id = instance_id;
    s_snapshot.instance_id = instance_id;
    lifecycle_mark_inactive(s_snapshot.generation);
}

static void apply_callback_locked(uint32_t generation, iperf_id_t instance_id, iperf_state_t state,
                                  const iperf_traffic_report_t *report, bool report_ok)
{
    if (generation == 0U || s_snapshot.generation != generation ||
        (s_instance_id > 0 && s_instance_id != instance_id) || !s_snapshot.active) {
        return;
    }
    s_instance_id = instance_id;
    s_snapshot.instance_id = instance_id;
    s_snapshot.started = s_started;
    s_snapshot.cancel_requested = s_cancel_requested;
    if (report_ok) {
        snapshot_report(report);
    }
    switch (state) {
    case IPERF_STARTED:
    case IPERF_RUNNING:
        s_started = true;
        s_snapshot.started = true;
        s_snapshot.state = s_cancel_requested ? ETHERNET_IPERF_STATE_STOPPING
                                               : ETHERNET_IPERF_STATE_RUNNING;
        break;
    case IPERF_STOPPED:
        s_snapshot.state = ETHERNET_IPERF_STATE_STOPPING;
        break;
    case IPERF_CLOSED:
        apply_closed_locked(instance_id, report, report_ok);
        break;
    default:
        break;
    }
}

static void reconcile_pending_locked(void)
{
    esp_err_t failure_error;
    iperf_traffic_report_t report = {0};
    iperf_state_t state = IPERF_CLOSED;
    bool report_valid = false;

    if (s_snapshot.active &&
        lifecycle_take_failure(s_snapshot.generation, s_instance_id, &failure_error)) {
        s_snapshot.active = false;
        s_snapshot.terminal = true;
        s_snapshot.stop_pending = false;
        s_snapshot.state = ETHERNET_IPERF_STATE_FAILED;
        s_snapshot.last_error = failure_error;
        s_snapshot.instance_id = -1;
        s_instance_id = -1;
        lifecycle_mark_inactive(s_snapshot.generation);
    }
    if (s_snapshot.active && lifecycle_take_callback(s_snapshot.generation, s_instance_id,
                                                     &state, &report, &report_valid)) {
        apply_callback_locked(s_snapshot.generation, s_instance_id, state, &report, report_valid);
    }
}

static void iperf_state_handler(iperf_id_t instance_id, iperf_state_data_t *data, void *priv)
{
    const uint32_t generation = (uint32_t)(uintptr_t)priv;
    iperf_traffic_report_t report = {0};
    const bool report_state = data != NULL &&
                              (data->state == IPERF_RUNNING || data->state == IPERF_CLOSED);
    const bool report_ok = report_state &&
                           iperf_get_traffic_report(instance_id, &report) == ESP_OK;

    if (data == NULL || !lock_wrapper(pdMS_TO_TICKS(ETHERNET_IPERF_LOCK_TIMEOUT_MS))) {
        if (data != NULL) {
            lifecycle_queue_callback(generation, instance_id, data->state,
                                     report_ok ? &report : NULL);
        }
        return;
    }
    reconcile_pending_locked();
    apply_callback_locked(generation, instance_id, data->state, &report, report_ok);
    xSemaphoreGive(s_lock);
}

static void mark_failure(uint32_t generation, iperf_id_t instance_id, esp_err_t error)
{
    lifecycle_queue_failure(generation, instance_id, error);
    if (lock_wrapper(pdMS_TO_TICKS(ETHERNET_IPERF_LOCK_TIMEOUT_MS))) {
        reconcile_pending_locked();
        xSemaphoreGive(s_lock);
    }
}

static void mark_start_failure(esp_err_t error)
{
    uint32_t generation;

    portENTER_CRITICAL(&s_lifecycle_mux);
    generation = s_lifecycle_generation;
    portEXIT_CRITICAL(&s_lifecycle_mux);
    mark_failure(generation, -1, error);
}

static bool service_stop_request(uint32_t generation)
{
    iperf_id_t instance_id;

    if (!lifecycle_stop_request_pending(generation) ||
        !lock_wrapper(pdMS_TO_TICKS(ETHERNET_IPERF_LOCK_TIMEOUT_MS))) {
        return false;
    }
    reconcile_pending_locked();
    if (!s_snapshot.active || s_snapshot.generation != generation) {
        xSemaphoreGive(s_lock);
        return false;
    }
    if (s_instance_id <= 0) {
        s_instance_id = lifecycle_take_pending_instance(generation);
        if (s_instance_id > 0) {
            s_snapshot.instance_id = s_instance_id;
        }
    }
    if (s_instance_id <= 0) {
        /** Keep stop pending until the supervisor can adopt the published core instance ID. */
        xSemaphoreGive(s_lock);
        return false;
    }
    s_cancel_requested = true;
    s_snapshot.cancel_requested = true;
    s_snapshot.stop_pending = true;
    s_snapshot.state = ETHERNET_IPERF_STATE_STOPPING;
    instance_id = s_instance_id;
    lifecycle_clear_stop_request(generation);
    xSemaphoreGive(s_lock);

    const esp_err_t ret = iperf_stop_instance(instance_id);
    if (ret != ESP_OK) {
        /** Retain the active lifecycle and retry stopping until IPERF_CLOSED is reconciled. */
        ESP_LOGW(ETHERNET_IPERF_TAG,
                 "stop request for generation=%u instance=%d failed: %s; retrying",
                 (unsigned int)generation, (int)instance_id, esp_err_to_name(ret));
        lifecycle_queue_stop_request();
    }
    return true;
}

static void network_monitor_task(void *arg)
{
    const network_monitor_context_t *context = (const network_monitor_context_t *)arg;
    const uint32_t generation = context != NULL ? context->generation : 0U;

    /** Wait for xTaskCreate to publish the task handle. */
    while (!monitor_task_exists()) {
        vTaskDelay(1);
    }
    for (;;) {
        bool active;
        bool instance_started;
        bool cancel_requested;

        (void)service_stop_request(generation);
        if (!lock_wrapper(pdMS_TO_TICKS(ETHERNET_IPERF_LOCK_TIMEOUT_MS))) {
            vTaskDelay(pdMS_TO_TICKS(ETHERNET_IPERF_NETWORK_POLL_MS));
            continue;
        }
        reconcile_pending_locked();
        active = s_snapshot.active && s_snapshot.generation == generation;
        instance_started = s_instance_id > 0;
        cancel_requested = s_cancel_requested;
        xSemaphoreGive(s_lock);
        if (!active) {
            break;
        }
        if (cancel_requested || !instance_started) {
            vTaskDelay(pdMS_TO_TICKS(ETHERNET_IPERF_NETWORK_POLL_MS));
            continue;
        }

        ethernet_service_status_t status = {0};
        const esp_err_t ret = ethernet_service_get_status(&status);
        if (ret == ESP_ERR_TIMEOUT) {
            vTaskDelay(pdMS_TO_TICKS(ETHERNET_IPERF_NETWORK_POLL_MS));
            continue;
        }
        const bool network_loss_observed =
            status.link_loss_count != s_start_link_loss_count ||
            status.ip_loss_count != s_start_ip_loss_count;
        const bool network_ready = ret == ESP_OK && status.enabled && status.configured &&
                                   status.initialized && status.link_up && status.ip_valid &&
                                   status.ip_info.ip.addr != 0 && !network_loss_observed;
        if (!network_ready) {
            ESP_LOGW(ETHERNET_IPERF_TAG,
                     "Ethernet link or IPv4 lost; requesting cancellation for generation=%u",
                     (unsigned int)generation);
            const esp_err_t stop_ret = ethernet_iperf_request_stop();
            if (stop_ret != ESP_OK && stop_ret != ESP_ERR_TIMEOUT &&
                stop_ret != ESP_ERR_INVALID_STATE) {
                ESP_LOGW(ETHERNET_IPERF_TAG, "network-loss cancellation failed: %s",
                         esp_err_to_name(stop_ret));
            }
            vTaskDelay(pdMS_TO_TICKS(ETHERNET_IPERF_NETWORK_POLL_MS));
            continue;
        }
        vTaskDelay(pdMS_TO_TICKS(ETHERNET_IPERF_NETWORK_POLL_MS));
    }

    /** Clear the task handle independently of the wrapper mutex before self-deletion. */
    clear_monitor_task();
    vTaskDelete(NULL);
}

esp_err_t ethernet_iperf_start(const ethernet_iperf_config_t *config)
{
    esp_ip_addr_t destination;
    esp_ip_addr_t source;
    char canonical_peer[ETHERNET_IPERF_IPV4_TEXT_LEN];
    ethernet_service_status_t ethernet_status;
    iperf_cfg_t iperf_config = {0};
    uint32_t generation;
    TaskHandle_t monitor_task = NULL;
    esp_err_t ret;

    if (!ensure_lock()) {
        return ESP_ERR_NO_MEM;
    }
    ret = validate_config(config, &destination, canonical_peer);
    if (ret != ESP_OK) {
        return ret;
    }
    ret = ethernet_service_get_status(&ethernet_status);
    if (ret != ESP_OK) {
        return ret;
    }
    if (!ethernet_status.enabled || !ethernet_status.configured || !ethernet_status.initialized ||
        !ethernet_status.link_up || !ethernet_status.ip_valid ||
        ethernet_status.ip_info.ip.addr == htonl(INADDR_ANY)) {
        return ESP_ERR_INVALID_STATE;
    }
    s_start_link_loss_count = ethernet_status.link_loss_count;
    s_start_ip_loss_count = ethernet_status.ip_loss_count;
    memset(&source, 0, sizeof(source));
    source.type = ESP_IPADDR_TYPE_V4;
    source.u_addr.ip4 = ethernet_status.ip_info.ip;

    if (!lock_wrapper(pdMS_TO_TICKS(ETHERNET_IPERF_LOCK_TIMEOUT_MS))) {
        return ESP_ERR_TIMEOUT;
    }
    reconcile_pending_locked();
    if (lifecycle_run_is_active() || s_snapshot.active) {
        xSemaphoreGive(s_lock);
        return ESP_ERR_INVALID_STATE;
    }
    if (monitor_task_exists()) {
        xSemaphoreGive(s_lock);
        return ESP_ERR_INVALID_STATE;
    }
    memset(&s_snapshot, 0, sizeof(s_snapshot));
    s_snapshot.state = ETHERNET_IPERF_STATE_STARTING;
    s_snapshot.protocol = config->protocol;
    s_snapshot.active = true;
    s_snapshot.terminal = false;
    s_snapshot.cancel_requested = false;
    s_snapshot.started = false;
    s_snapshot.port = (uint16_t)config->port;
    s_snapshot.duration_sec = config->duration_sec;
    s_snapshot.interval_sec = config->interval_sec;
    s_snapshot.last_error = ESP_OK;
    s_snapshot.instance_id = -1;
    s_snapshot.generation = ++s_run_generation;
    if (s_snapshot.generation == 0U) {
        s_snapshot.generation = ++s_run_generation;
    }
    memcpy(s_snapshot.peer_ipv4, canonical_peer, sizeof(s_snapshot.peer_ipv4));
    s_cancel_requested = false;
    s_started = false;
    s_instance_id = -1;
    generation = s_snapshot.generation;
    s_network_monitor_context.generation = generation;
    lifecycle_begin_run(generation);
    xSemaphoreGive(s_lock);

    if (xTaskCreate(network_monitor_task, "iperf_net", ETHERNET_IPERF_NETWORK_TASK_STACK,
                    &s_network_monitor_context, ETHERNET_IPERF_NETWORK_TASK_PRIORITY,
                    &monitor_task) != pdPASS) {
        mark_start_failure(ESP_ERR_NO_MEM);
        return ESP_ERR_NO_MEM;
    }
    portENTER_CRITICAL(&s_lifecycle_mux);
    s_network_monitor_task = monitor_task;
    portEXIT_CRITICAL(&s_lifecycle_mux);

    iperf_config.destination = destination;
    iperf_config.source = source;
    iperf_config.format = MBITS_PER_SEC;
    iperf_config.state_handler = iperf_state_handler;
    iperf_config.state_handler_priv = (void *)(uintptr_t)generation;
    iperf_config.flag = protocol_flag(config->protocol) | IPERF_FLAG_CLIENT;
    iperf_config.interval = config->interval_sec;
    iperf_config.time = config->duration_sec;
    iperf_config.bw_lim = IPERF_DEFAULT_NO_BW_LIMIT;
    iperf_config.dport = (uint16_t)config->port;
    iperf_config.sport = 0;
    iperf_config.tos = 0;

    const iperf_id_t instance_id = iperf_start_instance(&iperf_config);
    if (instance_id <= 0) {
        const esp_err_t start_error = (errno == ENOMEM || errno == ENOBUFS) ?
                                      ESP_ERR_NO_MEM : ESP_FAIL;
        mark_start_failure(start_error);
        return start_error;
    }
    lifecycle_publish_instance(generation, instance_id);
    if (lock_wrapper(pdMS_TO_TICKS(ETHERNET_IPERF_LOCK_TIMEOUT_MS))) {
        if (s_instance_id <= 0) {
            s_instance_id = instance_id;
        }
        s_snapshot.instance_id = instance_id;
        reconcile_pending_locked();
        if (!s_snapshot.active) {
            (void)lifecycle_take_pending_instance(generation);
            xSemaphoreGive(s_lock);
            return ESP_ERR_INVALID_STATE;
        }
        if (s_snapshot.cancel_requested ||
            s_snapshot.generation != s_network_monitor_context.generation) {
            s_cancel_requested = true;
            s_snapshot.cancel_requested = true;
            s_snapshot.stop_pending = true;
            s_snapshot.state = ETHERNET_IPERF_STATE_STOPPING;
            lifecycle_queue_stop_request();
            (void)lifecycle_take_pending_instance(generation);
            xSemaphoreGive(s_lock);
            return ESP_ERR_INVALID_STATE;
        }
        (void)lifecycle_take_pending_instance(generation);
        xSemaphoreGive(s_lock);
    } else {
        /** Let the monitor adopt the new instance and service its pending stop request. */
        lifecycle_queue_stop_request();
        return ESP_ERR_TIMEOUT;
    }
    ESP_LOGI(ETHERNET_IPERF_TAG, "started %s client id=%d peer=%s:%u duration=%u interval=%u",
             config->protocol == ETHERNET_IPERF_PROTOCOL_UDP ? "UDP" : "TCP",
             instance_id, canonical_peer, (unsigned int)config->port,
             (unsigned int)config->duration_sec, (unsigned int)config->interval_sec);
    return ESP_OK;
}

esp_err_t ethernet_iperf_request_stop(void)
{
    if (!ensure_lock()) {
        return ESP_ERR_NO_MEM;
    }
    if (!lifecycle_run_is_active()) {
        return ESP_ERR_INVALID_STATE;
    }

    /** Publish cancellation without waiting for the wrapper mutex or core teardown. */
    if (xSemaphoreTake(s_lock, 0) != pdTRUE) {
        lifecycle_queue_stop_request();
        return ESP_ERR_TIMEOUT;
    }
    reconcile_pending_locked();
    if (!s_snapshot.active || s_cancel_requested) {
        xSemaphoreGive(s_lock);
        return ESP_ERR_INVALID_STATE;
    }
    s_cancel_requested = true;
    s_snapshot.cancel_requested = true;
    s_snapshot.stop_pending = true;
    s_snapshot.state = ETHERNET_IPERF_STATE_STOPPING;
    lifecycle_queue_stop_request();
    xSemaphoreGive(s_lock);
    return ESP_OK;
}

esp_err_t ethernet_iperf_get_snapshot(ethernet_iperf_snapshot_t *snapshot)
{
    if (snapshot == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!ensure_lock()) {
        return ESP_ERR_NO_MEM;
    }
    if (!lock_wrapper(pdMS_TO_TICKS(ETHERNET_IPERF_LOCK_TIMEOUT_MS))) {
        return ESP_ERR_TIMEOUT;
    }
    reconcile_pending_locked();
    *snapshot = s_snapshot;
    if (lifecycle_stop_request_pending(snapshot->generation)) {
        snapshot->cancel_requested = true;
        snapshot->stop_pending = true;
        snapshot->state = ETHERNET_IPERF_STATE_STOPPING;
    }
    xSemaphoreGive(s_lock);
    return ESP_OK;
}
