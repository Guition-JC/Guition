/** Ethernet-bound iPerf client with one active run and service-owned local addressing. */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ETHERNET_IPERF_IPV4_TEXT_LEN 16
#define ETHERNET_IPERF_MIN_PORT 1U
#define ETHERNET_IPERF_MAX_PORT 65535U
#define ETHERNET_IPERF_MIN_DURATION_SEC 1U
#define ETHERNET_IPERF_MAX_DURATION_SEC 3600U
#define ETHERNET_IPERF_MIN_INTERVAL_SEC 1U
#define ETHERNET_IPERF_MAX_INTERVAL_SEC 60U

/** @brief Protocol selected for the next client run. */
typedef enum {
    ETHERNET_IPERF_PROTOCOL_TCP = 0,
    ETHERNET_IPERF_PROTOCOL_UDP,
} ethernet_iperf_protocol_t;

/** @brief Wrapper lifecycle, retained in the terminal snapshot until the next run. */
typedef enum {
    ETHERNET_IPERF_STATE_IDLE = 0,
    ETHERNET_IPERF_STATE_STARTING,
    ETHERNET_IPERF_STATE_RUNNING,
    ETHERNET_IPERF_STATE_STOPPING,
    ETHERNET_IPERF_STATE_SUCCEEDED,
    ETHERNET_IPERF_STATE_CANCELED,
    ETHERNET_IPERF_STATE_FAILED,
} ethernet_iperf_state_t;

/** @brief Peer and timing parameters for an Ethernet throughput run. */
typedef struct {
    ethernet_iperf_protocol_t protocol;
    const char *peer_ipv4;
    uint32_t port;
    uint32_t duration_sec;
    uint32_t interval_sec;
} ethernet_iperf_config_t;

/** @brief Current or terminal throughput measurements and lifecycle state. */
typedef struct {
    ethernet_iperf_state_t state;
    ethernet_iperf_protocol_t protocol;
    bool active;
    bool terminal;
    bool cancel_requested;
    bool stop_pending;
    bool started;
    bool report_valid;
    char peer_ipv4[ETHERNET_IPERF_IPV4_TEXT_LEN];
    uint16_t port;
    uint32_t duration_sec;
    uint32_t interval_sec;
    uint32_t elapsed_sec;
    uint32_t progress_percent;
    uint64_t period_transfer_bytes;
    uint32_t period_throughput_bps;
    uint32_t average_throughput_bps;
    uint64_t total_transfer_bytes;
    esp_err_t last_error;
    int8_t instance_id;
    uint32_t generation;
} ethernet_iperf_snapshot_t;

/**
 * @brief Start one Ethernet-bound client run; reject an already active run.
 * @param config Numeric unicast IPv4 peer and run parameters. RFC1918 addresses are accepted; special-use, reserved, multicast and broadcast ranges are rejected.
 */
esp_err_t ethernet_iperf_start(const ethernet_iperf_config_t *config);

/**
 * @brief Publish asynchronous cancellation; keep polling until IPERF_CLOSED clears the active run.
 * @return ESP_OK or ESP_ERR_TIMEOUT both leave the stop request pending; do not start another active run.
 */
esp_err_t ethernet_iperf_request_stop(void);

/**
 * @brief Copy the lock-protected current or terminal run snapshot.
 * @param[out] snapshot Receives the copied state.
 * @return ESP_OK on success, or an error code on failure.
 */
esp_err_t ethernet_iperf_get_snapshot(ethernet_iperf_snapshot_t *snapshot);

#ifdef __cplusplus
}
#endif
