/** Ethernet initialization and lock-protected status snapshots. */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "esp_eth.h"
#include "esp_netif.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ETHERNET_SERVICE_MAC_LEN 6

/** @brief Ethernet link, addressing and driver-status snapshot. */
typedef struct {
    bool enabled;
    bool configured;
    bool initialized;
    /** PHY register probe completed independently of link state. */
    bool phy_probe_complete;
    bool phy_communication_ok;
    bool link_up;
    bool ip_valid;
    bool rx_packets_valid;
    bool tx_packets_valid;
    bool rx_bytes_valid;
    bool tx_bytes_valid;
    bool speed_valid;
    bool duplex_valid;
    esp_netif_ip_info_t ip_info;
    uint8_t mac[ETHERNET_SERVICE_MAC_LEN];
    uint32_t rx_packets;
    uint32_t tx_packets;
    uint32_t rx_bytes;
    uint32_t tx_bytes;
    eth_speed_t speed;
    eth_duplex_t duplex;
    esp_err_t last_error;
    uint32_t unavailable_count;
    uint32_t unavailable_init_count;
    uint32_t unavailable_status_count;
    uint32_t unavailable_dhcp_renew_count;
    uint32_t unavailable_mac_count;
    uint32_t unavailable_speed_count;
    uint32_t unavailable_duplex_count;
    uint32_t init_failure_count;
    uint32_t link_loss_count;
    uint32_t ip_loss_count;
    uint32_t dhcp_renew_count;
    uint32_t dhcp_renew_failure_count;
} ethernet_service_status_t;

/**
 * @brief Schedule Ethernet initialization without waiting for hardware, link or DHCP.
 * @return ESP_OK on success, or an error code on failure.
 */
esp_err_t ethernet_service_init(void);

/**
 * @brief Copy Ethernet status with a bounded mutex wait.
 * @param[out] status Receives the Ethernet status.
 * @return ESP_OK on success, or an error code on failure.
 */
esp_err_t ethernet_service_get_status(ethernet_service_status_t *status);

/**
 * @brief Request DHCP lease renewal without waiting for the lease exchange.
 * @return ESP_ERR_INVALID_STATE if a renewal is already in progress; IP events update the result.
 */
esp_err_t ethernet_service_dhcp_renew(void);

#ifdef __cplusplus
}
#endif
