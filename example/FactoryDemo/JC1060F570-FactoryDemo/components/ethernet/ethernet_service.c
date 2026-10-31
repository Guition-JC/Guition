#include "ethernet_service.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif_net_stack.h"
#include "ethernet_init.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lwip/netif.h"
#include "sdkconfig.h"

#define ETHERNET_SERVICE_TAG "ethernet_service"
#define ETHERNET_SERVICE_LOCK_TIMEOUT_MS 100
#define ETHERNET_SERVICE_TASK_STACK 4096
#define ETHERNET_SERVICE_TASK_PRIORITY 4
#define ETHERNET_SERVICE_EVENT_POLL_MS 100
#define ETHERNET_SERVICE_PHY_POLL_MS 1000

#if (defined(CONFIG_ETHERNET_INTERNAL_SUPPORT) && CONFIG_ETHERNET_INTERNAL_SUPPORT) || \
    (defined(CONFIG_ETHERNET_SPI_SUPPORT) && CONFIG_ETHERNET_SPI_SUPPORT) || \
    (defined(CONFIG_ETHERNET_OPENETH_SUPPORT) && CONFIG_ETHERNET_OPENETH_SUPPORT)
#define FACTORY_ETHERNET_CONFIGURED 1
#else
#define FACTORY_ETHERNET_CONFIGURED 0
#endif

typedef enum {
    UNAVAILABLE_INIT,
    UNAVAILABLE_STATUS,
    UNAVAILABLE_DHCP_RENEW,
} unavailable_reason_t;

static SemaphoreHandle_t s_lock;
static SemaphoreHandle_t s_dhcp_renew_lock;
static ethernet_service_status_t s_status;
static esp_netif_t *s_primary_netif;

#if FACTORY_ETHERNET_CONFIGURED
static TaskHandle_t s_worker;
static esp_eth_handle_t *s_eth_handles;
static uint8_t s_eth_port_count;
static esp_netif_t **s_eth_netifs;
static esp_eth_netif_glue_handle_t *s_eth_glues;
static esp_eth_handle_t s_primary_eth_handle;
static esp_event_handler_instance_t s_eth_event_instance;
static esp_event_handler_instance_t s_ip_event_instance;
typedef struct {
    bool link_state_pending;
    bool link_up;
    bool link_loss_pending;
    bool link_details_pending;
    bool ip_state_pending;
    bool ip_valid;
    esp_netif_ip_info_t ip_info;
    bool ip_loss_pending;
} pending_network_events_t;
static portMUX_TYPE s_pending_events_mux = portMUX_INITIALIZER_UNLOCKED;
static pending_network_events_t s_pending_events;
#endif

static bool lock_status(TickType_t timeout)
{
    return s_lock != NULL && xSemaphoreTake(s_lock, timeout) == pdTRUE;
}

static void set_last_error(esp_err_t error)
{
    if (lock_status(pdMS_TO_TICKS(ETHERNET_SERVICE_LOCK_TIMEOUT_MS))) {
        s_status.last_error = error;
        xSemaphoreGive(s_lock);
    }
}

static void mark_unavailable(unavailable_reason_t reason, esp_err_t error)
{
    if (!lock_status(pdMS_TO_TICKS(ETHERNET_SERVICE_LOCK_TIMEOUT_MS))) {
        return;
    }

    s_status.unavailable_count++;
    switch (reason) {
    case UNAVAILABLE_INIT:
        s_status.unavailable_init_count++;
        break;
    case UNAVAILABLE_STATUS:
        s_status.unavailable_status_count++;
        break;
    case UNAVAILABLE_DHCP_RENEW:
        s_status.unavailable_dhcp_renew_count++;
        break;
    }
    s_status.last_error = error;
    xSemaphoreGive(s_lock);
}

static void refresh_live_status_locked(void)
{
#if FACTORY_ETHERNET_CONFIGURED
    if (s_primary_netif == NULL) {
        return;
    }

    esp_netif_ip_info_t ip_info = {0};
    const esp_err_t ip_ret = esp_netif_get_ip_info(s_primary_netif, &ip_info);
    if (ip_ret == ESP_OK && s_status.link_up && ip_info.ip.addr != 0) {
        s_status.ip_info = ip_info;
        s_status.ip_valid = true;
    } else if (!s_status.link_up || ip_info.ip.addr == 0) {
        s_status.ip_valid = false;
        memset(&s_status.ip_info, 0, sizeof(s_status.ip_info));
    }

#if MIB2_STATS
    struct netif *netif = esp_netif_get_netif_impl(s_primary_netif);
    if (netif != NULL && s_status.link_up) {
        s_status.rx_packets = netif->mib2_counters.ifinucastpkts +
                              netif->mib2_counters.ifinnucastpkts;
        s_status.tx_packets = netif->mib2_counters.ifoutucastpkts +
                              netif->mib2_counters.ifoutnucastpkts;
        s_status.rx_bytes = netif->mib2_counters.ifinoctets;
        s_status.tx_bytes = netif->mib2_counters.ifoutoctets;
        s_status.rx_packets_valid = true;
        s_status.tx_packets_valid = true;
        s_status.rx_bytes_valid = true;
        s_status.tx_bytes_valid = true;
    } else {
        s_status.rx_packets_valid = false;
        s_status.tx_packets_valid = false;
        s_status.rx_bytes_valid = false;
        s_status.tx_bytes_valid = false;
    }
#else
    /** Leave packet counters unavailable when the driver exposes no counter ioctl. */
    s_status.rx_packets_valid = false;
    s_status.tx_packets_valid = false;
    s_status.rx_bytes_valid = false;
    s_status.tx_bytes_valid = false;
#endif
#else
    s_status.rx_packets_valid = false;
    s_status.tx_packets_valid = false;
    s_status.rx_bytes_valid = false;
    s_status.tx_bytes_valid = false;
#endif
}

#if FACTORY_ETHERNET_CONFIGURED

static bool is_primary_eth_handle(esp_eth_handle_t handle)
{
    return handle != NULL && handle == s_primary_eth_handle;
}

static bool is_primary_netif(esp_netif_t *netif)
{
    return netif != NULL && netif == s_primary_netif;
}

#if defined(CONFIG_ETHERNET_PHY_IP101) && CONFIG_ETHERNET_PHY_IP101
static void update_phy_communication(esp_eth_handle_t eth_handle)
{
    uint32_t id1 = 0;
    uint32_t id2 = 0;
    esp_eth_phy_reg_rw_data_t reg = { .reg_addr = 2, .reg_value_p = &id1 };
    esp_err_t ret = esp_eth_ioctl(eth_handle, ETH_CMD_READ_PHY_REG, &reg);
    if (ret == ESP_OK) {
        reg.reg_addr = 3;
        reg.reg_value_p = &id2;
        ret = esp_eth_ioctl(eth_handle, ETH_CMD_READ_PHY_REG, &reg);
    }

    /** Reject all-zero and all-one PHY register values even when MDIO succeeds. */
    const bool valid_id = (id1 & 0xffffU) != 0 && (id1 & 0xffffU) != 0xffffU &&
                          (id2 & 0xffffU) != 0 && (id2 & 0xffffU) != 0xffffU;
    if (lock_status(pdMS_TO_TICKS(ETHERNET_SERVICE_LOCK_TIMEOUT_MS))) {
        s_status.phy_probe_complete = true;
        s_status.phy_communication_ok = ret == ESP_OK && valid_id;
        xSemaphoreGive(s_lock);
    }
}
#endif

static void update_link_details(esp_eth_handle_t eth_handle)
{
    uint8_t mac[ETHERNET_SERVICE_MAC_LEN] = {0};
    eth_speed_t speed = ETH_SPEED_MAX;
    eth_duplex_t duplex = ETH_DUPLEX_HALF;
    const esp_err_t mac_ret = esp_eth_ioctl(eth_handle, ETH_CMD_G_MAC_ADDR, mac);
    const esp_err_t speed_ret = esp_eth_ioctl(eth_handle, ETH_CMD_G_SPEED, &speed);
    const esp_err_t duplex_ret = esp_eth_ioctl(eth_handle, ETH_CMD_G_DUPLEX_MODE, &duplex);

    if (!lock_status(pdMS_TO_TICKS(ETHERNET_SERVICE_LOCK_TIMEOUT_MS))) {
        return;
    }
    if (mac_ret == ESP_OK) {
        memcpy(s_status.mac, mac, sizeof(s_status.mac));
    }
    s_status.speed = speed;
    s_status.duplex = duplex;
    s_status.speed_valid = speed_ret == ESP_OK;
    s_status.duplex_valid = duplex_ret == ESP_OK;
    if (mac_ret != ESP_OK) {
        s_status.unavailable_count++;
        s_status.unavailable_mac_count++;
    }
    if (speed_ret != ESP_OK) {
        s_status.unavailable_count++;
        s_status.unavailable_speed_count++;
    }
    if (duplex_ret != ESP_OK) {
        s_status.unavailable_count++;
        s_status.unavailable_duplex_count++;
    }
    if (mac_ret != ESP_OK || speed_ret != ESP_OK || duplex_ret != ESP_OK) {
        s_status.last_error = mac_ret != ESP_OK ? mac_ret :
                              speed_ret != ESP_OK ? speed_ret : duplex_ret;
    }
    xSemaphoreGive(s_lock);
}

static void queue_ethernet_event(esp_eth_handle_t eth_handle, int32_t event_id)
{
    portENTER_CRITICAL(&s_pending_events_mux);
    switch (event_id) {
    case ETHERNET_EVENT_CONNECTED:
        s_pending_events.link_state_pending = true;
        s_pending_events.link_up = true;
        s_pending_events.link_details_pending = true;
        break;
    case ETHERNET_EVENT_DISCONNECTED:
    case ETHERNET_EVENT_STOP:
        s_pending_events.link_state_pending = true;
        s_pending_events.link_up = false;
        s_pending_events.link_loss_pending = true;
        break;
    default:
        break;
    }
    portEXIT_CRITICAL(&s_pending_events_mux);

    if (event_id == ETHERNET_EVENT_CONNECTED) {
        ESP_LOGI(ETHERNET_SERVICE_TAG, "Ethernet link up (state deferred)");
    } else if (event_id == ETHERNET_EVENT_DISCONNECTED) {
        ESP_LOGW(ETHERNET_SERVICE_TAG, "Ethernet link down (state deferred)");
    }
    (void)eth_handle;
}

static void queue_ip_event(const ip_event_got_ip_t *event, bool ip_valid)
{
    portENTER_CRITICAL(&s_pending_events_mux);
    s_pending_events.ip_state_pending = true;
    s_pending_events.ip_valid = ip_valid;
    if (ip_valid && event != NULL) {
        s_pending_events.ip_info = event->ip_info;
    } else {
        memset(&s_pending_events.ip_info, 0, sizeof(s_pending_events.ip_info));
        s_pending_events.ip_loss_pending = true;
    }
    portEXIT_CRITICAL(&s_pending_events_mux);
}

static void process_pending_network_events(void)
{
    pending_network_events_t pending = {0};
    bool process_link_loss = false;
    bool process_ip_loss = false;

    if (!lock_status(pdMS_TO_TICKS(ETHERNET_SERVICE_LOCK_TIMEOUT_MS))) {
        return;
    }
    portENTER_CRITICAL(&s_pending_events_mux);
    /** Publish link/IP loss before applying a recovered state. */
    if (s_pending_events.link_loss_pending) {
        process_link_loss = true;
        s_pending_events.link_loss_pending = false;
    } else if (s_pending_events.ip_loss_pending) {
        process_ip_loss = true;
        s_pending_events.ip_loss_pending = false;
    } else {
        pending = s_pending_events;
        memset(&s_pending_events, 0, sizeof(s_pending_events));
    }
    portEXIT_CRITICAL(&s_pending_events_mux);

    if (process_link_loss) {
        if (s_status.link_up) {
            s_status.link_loss_count++;
        }
        s_status.link_up = false;
        if (s_status.ip_valid) {
            s_status.ip_loss_count++;
        }
        s_status.ip_valid = false;
        memset(&s_status.ip_info, 0, sizeof(s_status.ip_info));
        s_status.speed = ETH_SPEED_MAX;
        s_status.duplex = ETH_DUPLEX_HALF;
        s_status.speed_valid = false;
        s_status.duplex_valid = false;
        s_status.rx_packets_valid = false;
        s_status.tx_packets_valid = false;
        s_status.rx_bytes_valid = false;
        s_status.tx_bytes_valid = false;
        xSemaphoreGive(s_lock);
        return;
    }
    if (process_ip_loss) {
        if (s_status.ip_valid) {
            s_status.ip_loss_count++;
        }
        s_status.ip_valid = false;
        memset(&s_status.ip_info, 0, sizeof(s_status.ip_info));
        xSemaphoreGive(s_lock);
        return;
    }

    if (pending.link_state_pending) {
        s_status.link_up = pending.link_up;
        if (!pending.link_up) {
            s_status.ip_valid = false;
            memset(&s_status.ip_info, 0, sizeof(s_status.ip_info));
            s_status.speed = ETH_SPEED_MAX;
            s_status.duplex = ETH_DUPLEX_HALF;
            s_status.speed_valid = false;
            s_status.duplex_valid = false;
            s_status.rx_packets_valid = false;
            s_status.tx_packets_valid = false;
            s_status.rx_bytes_valid = false;
            s_status.tx_bytes_valid = false;
        }
    }
    if (pending.ip_state_pending) {
        if (pending.ip_valid && s_status.link_up) {
            s_status.ip_info = pending.ip_info;
            s_status.ip_valid = true;
            s_status.last_error = ESP_OK;
        } else {
            s_status.ip_valid = false;
            memset(&s_status.ip_info, 0, sizeof(s_status.ip_info));
        }
    }
    xSemaphoreGive(s_lock);

    if (pending.link_details_pending && pending.link_up) {
        update_link_details(s_primary_eth_handle);
    }
    if (pending.ip_state_pending) {
        ESP_LOGI(ETHERNET_SERVICE_TAG, "%s deferred IPv4 event",
                 pending.ip_valid ? "processed" : "processed lost");
    }
}

static void ethernet_event_handler(void *arg, esp_event_base_t event_base,
                                   int32_t event_id, void *event_data)
{
    (void)arg;
    if (event_base != ETH_EVENT || event_data == NULL) {
        return;
    }

    const esp_eth_handle_t eth_handle = *(esp_eth_handle_t *)event_data;
    if (is_primary_eth_handle(eth_handle)) {
        queue_ethernet_event(eth_handle, event_id);
    }
}

static void ip_event_handler(void *arg, esp_event_base_t event_base,
                             int32_t event_id, void *event_data)
{
    (void)arg;
    if (event_base != IP_EVENT || event_data == NULL) {
        return;
    }

    const ip_event_got_ip_t *event = event_data;
    if (!is_primary_netif(event->esp_netif)) {
        return;
    }
    if (event_id == IP_EVENT_ETH_GOT_IP) {
        queue_ip_event(event, true);
    } else if (event_id == IP_EVENT_ETH_LOST_IP) {
        queue_ip_event(event, false);
        ESP_LOGW(ETHERNET_SERVICE_TAG, "Ethernet lost IPv4 address (state deferred)");
    }
}

static esp_err_t cleanup_runtime(void)
{
    esp_err_t ret = ESP_OK;
    bool handlers_unregistered = true;
    if (s_eth_event_instance != NULL) {
        const esp_err_t unregister_ret = esp_event_handler_instance_unregister(
            ETH_EVENT, ESP_EVENT_ANY_ID, s_eth_event_instance);
        if (ret == ESP_OK && unregister_ret != ESP_OK) {
            ret = unregister_ret;
        }
        if (unregister_ret == ESP_OK) {
            s_eth_event_instance = NULL;
        } else {
            handlers_unregistered = false;
        }
    }
    if (s_ip_event_instance != NULL) {
        const esp_err_t unregister_ret = esp_event_handler_instance_unregister(
            IP_EVENT, ESP_EVENT_ANY_ID, s_ip_event_instance);
        if (ret == ESP_OK && unregister_ret != ESP_OK) {
            ret = unregister_ret;
        }
        if (unregister_ret == ESP_OK) {
            s_ip_event_instance = NULL;
        } else {
            handlers_unregistered = false;
        }
    }
    if (!handlers_unregistered) {
        return ret;
    }

    /** Stop drivers before deleting netif glue and netifs, then deinitialize the drivers. */
    if (s_eth_handles != NULL) {
        for (uint8_t index = 0; index < s_eth_port_count; ++index) {
            if (s_eth_handles[index] != NULL) {
                const esp_err_t stop_ret = esp_eth_stop(s_eth_handles[index]);
                if (stop_ret != ESP_OK && stop_ret != ESP_ERR_INVALID_STATE) {
                    if (ret == ESP_OK) {
                        ret = stop_ret;
                    }
                    ESP_LOGW(ETHERNET_SERVICE_TAG, "Ethernet %u stop during cleanup failed: %s",
                             (unsigned int)index, esp_err_to_name(stop_ret));
                }
            }
        }
    }

    for (uint8_t index = 0; index < s_eth_port_count; ++index) {
        if (s_eth_glues != NULL && s_eth_glues[index] != NULL) {
            esp_eth_del_netif_glue(s_eth_glues[index]);
            s_eth_glues[index] = NULL;
        }
        if (s_eth_netifs != NULL && s_eth_netifs[index] != NULL) {
            esp_netif_destroy(s_eth_netifs[index]);
            s_eth_netifs[index] = NULL;
        }
    }
    free(s_eth_glues);
    free(s_eth_netifs);
    s_eth_glues = NULL;
    s_eth_netifs = NULL;
    s_primary_netif = NULL;
    s_primary_eth_handle = NULL;
    if (s_eth_handles != NULL) {
        const esp_err_t deinit_ret = ethernet_deinit_all(s_eth_handles);
        if (deinit_ret != ESP_OK) {
            ESP_LOGW(ETHERNET_SERVICE_TAG, "Ethernet deinit during cleanup failed: %s",
                     esp_err_to_name(deinit_ret));
            if (ret == ESP_OK) {
                ret = deinit_ret;
            }
        } else {
            /** Release the handle-array ownership record only after ethernet_deinit_all succeeds. */
            s_eth_handles = NULL;
            s_eth_port_count = 0;
        }
    } else {
        s_eth_port_count = 0;
    }
    return ret;
}

static esp_err_t initialize_runtime(void)
{
    if (s_eth_handles != NULL || s_eth_port_count != 0U) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t ret = esp_netif_init();
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        return ret;
    }
    ret = esp_event_loop_create_default();
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        return ret;
    }

    ret = ethernet_init_all(&s_eth_handles, &s_eth_port_count);
    if (ret != ESP_OK) {
        cleanup_runtime();
        return ret;
    }
    if (s_eth_handles == NULL || s_eth_port_count != 1) {
        const esp_err_t contract_ret = s_eth_handles == NULL ?
                                       ESP_ERR_INVALID_STATE : ESP_ERR_NOT_SUPPORTED;
        ESP_LOGE(ETHERNET_SERVICE_TAG,
                 "Ethernet single-port contract rejected: handles=%p count=%u",
                 (void *)s_eth_handles, (unsigned int)s_eth_port_count);
        cleanup_runtime();
        return contract_ret;
    }

    s_eth_netifs = calloc(s_eth_port_count, sizeof(*s_eth_netifs));
    s_eth_glues = calloc(s_eth_port_count, sizeof(*s_eth_glues));
    if (s_eth_netifs == NULL || s_eth_glues == NULL) {
        cleanup_runtime();
        return ESP_ERR_NO_MEM;
    }

    for (uint8_t index = 0; index < s_eth_port_count; ++index) {
        esp_netif_inherent_config_t inherent = ESP_NETIF_INHERENT_DEFAULT_ETH();
        char if_key[16];
        char if_desc[16];
        snprintf(if_key, sizeof(if_key), "ETH_%u", (unsigned int)index);
        snprintf(if_desc, sizeof(if_desc), "eth%u", (unsigned int)index);
        inherent.if_key = if_key;
        inherent.if_desc = if_desc;
        inherent.route_prio -= (int)index * 5;
        esp_netif_config_t config = {
            .base = &inherent,
            .stack = ESP_NETIF_NETSTACK_DEFAULT_ETH,
        };

        s_eth_netifs[index] = esp_netif_new(&config);
        if (s_eth_netifs[index] == NULL) {
            cleanup_runtime();
            return ESP_ERR_NO_MEM;
        }
        s_eth_glues[index] = esp_eth_new_netif_glue(s_eth_handles[index]);
        if (s_eth_glues[index] == NULL) {
            cleanup_runtime();
            return ESP_ERR_NO_MEM;
        }
        ret = esp_netif_attach(s_eth_netifs[index], s_eth_glues[index]);
        if (ret != ESP_OK) {
            cleanup_runtime();
            return ret;
        }
    }

    s_primary_eth_handle = s_eth_handles[0];
    s_primary_netif = s_eth_netifs[0];
    ret = esp_event_handler_instance_register(ETH_EVENT, ESP_EVENT_ANY_ID,
                                               ethernet_event_handler, NULL,
                                               &s_eth_event_instance);
    if (ret != ESP_OK) {
        cleanup_runtime();
        return ret;
    }
    ret = esp_event_handler_instance_register(IP_EVENT, ESP_EVENT_ANY_ID,
                                               ip_event_handler, NULL,
                                               &s_ip_event_instance);
    if (ret != ESP_OK) {
        cleanup_runtime();
        return ret;
    }

    update_link_details(s_primary_eth_handle);
    for (uint8_t index = 0; index < s_eth_port_count; ++index) {
        ret = esp_eth_start(s_eth_handles[index]);
        if (ret != ESP_OK) {
            cleanup_runtime();
            return ret;
        }
    }

    if (lock_status(pdMS_TO_TICKS(ETHERNET_SERVICE_LOCK_TIMEOUT_MS))) {
        s_status.initialized = true;
        s_status.last_error = ESP_OK;
        xSemaphoreGive(s_lock);
    }
#if defined(CONFIG_ETHERNET_PHY_IP101) && CONFIG_ETHERNET_PHY_IP101
    update_phy_communication(s_primary_eth_handle);
#endif
    ESP_LOGI(ETHERNET_SERVICE_TAG, "Ethernet runtime initialized; waiting for link and DHCP");
    return ESP_OK;
}

static void ethernet_worker(void *arg)
{
    (void)arg;
    for (;;) {
        const esp_err_t ret = initialize_runtime();
        if (ret == ESP_OK) {
            break;
        }
        mark_unavailable(UNAVAILABLE_INIT, ret);
        if (lock_status(pdMS_TO_TICKS(ETHERNET_SERVICE_LOCK_TIMEOUT_MS))) {
            s_status.init_failure_count++;
            s_status.initialized = false;
            s_status.link_up = false;
            s_status.ip_valid = false;
            s_status.speed = ETH_SPEED_MAX;
            s_status.duplex = ETH_DUPLEX_HALF;
            s_status.speed_valid = false;
            s_status.duplex_valid = false;
            memset(&s_status.ip_info, 0, sizeof(s_status.ip_info));
            xSemaphoreGive(s_lock);
        }
        const esp_err_t cleanup_ret = cleanup_runtime();
        ESP_LOGW(ETHERNET_SERVICE_TAG, "Ethernet runtime unavailable: %s%s%s",
                 esp_err_to_name(ret), cleanup_ret == ESP_OK ? "" : "; cleanup retry pending: ",
                 cleanup_ret == ESP_OK ? "" : esp_err_to_name(cleanup_ret));
        if (cleanup_ret != ESP_OK) {
            /** Retain failed driver teardown state and reject a new runtime until it is released. */
            vTaskDelay(pdMS_TO_TICKS(ETHERNET_SERVICE_EVENT_POLL_MS));
            continue;
        }
        vTaskDelay(pdMS_TO_TICKS(ETHERNET_SERVICE_EVENT_POLL_MS));
    }

    /** Apply callback-published link/IP events in the service task. */
    for (;;) {
        process_pending_network_events();
#if defined(CONFIG_ETHERNET_PHY_IP101) && CONFIG_ETHERNET_PHY_IP101
        static TickType_t last_phy_probe;
        const TickType_t now = xTaskGetTickCount();
        if ((TickType_t)(now - last_phy_probe) >= pdMS_TO_TICKS(ETHERNET_SERVICE_PHY_POLL_MS)) {
            update_phy_communication(s_primary_eth_handle);
            last_phy_probe = now;
        }
#endif
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(ETHERNET_SERVICE_EVENT_POLL_MS));
    }
}

#endif

esp_err_t ethernet_service_init(void)
{
    if (s_lock != NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    s_lock = xSemaphoreCreateMutex();
    if (s_lock == NULL) {
        return ESP_ERR_NO_MEM;
    }
    s_dhcp_renew_lock = xSemaphoreCreateMutex();
    if (s_dhcp_renew_lock == NULL) {
        vSemaphoreDelete(s_lock);
        s_lock = NULL;
        return ESP_ERR_NO_MEM;
    }
    memset(&s_status, 0, sizeof(s_status));
    s_status.enabled = FACTORY_ETHERNET_CONFIGURED != 0;
    s_status.configured = FACTORY_ETHERNET_CONFIGURED != 0;
    s_status.speed = ETH_SPEED_MAX;

#if !FACTORY_ETHERNET_CONFIGURED
    mark_unavailable(UNAVAILABLE_INIT, ESP_ERR_INVALID_STATE);
    ESP_LOGW(ETHERNET_SERVICE_TAG, "Ethernet runtime unavailable: no official Ethernet transport is configured");
    vSemaphoreDelete(s_dhcp_renew_lock);
    s_dhcp_renew_lock = NULL;
    vSemaphoreDelete(s_lock);
    s_lock = NULL;
    return ESP_ERR_INVALID_STATE;
#else
    if (xTaskCreate(ethernet_worker, "ethernet_init", ETHERNET_SERVICE_TASK_STACK,
                    NULL, ETHERNET_SERVICE_TASK_PRIORITY, &s_worker) != pdPASS) {
        s_worker = NULL;
        mark_unavailable(UNAVAILABLE_INIT, ESP_ERR_NO_MEM);
        vSemaphoreDelete(s_dhcp_renew_lock);
        s_dhcp_renew_lock = NULL;
        vSemaphoreDelete(s_lock);
        s_lock = NULL;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
#endif
}

esp_err_t ethernet_service_get_status(ethernet_service_status_t *status)
{
    if (status == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_lock == NULL) {
        memset(status, 0, sizeof(*status));
        status->speed = ETH_SPEED_MAX;
        return ESP_ERR_INVALID_STATE;
    }
    if (!lock_status(pdMS_TO_TICKS(ETHERNET_SERVICE_LOCK_TIMEOUT_MS))) {
        return ESP_ERR_TIMEOUT;
    }
    refresh_live_status_locked();
    if (!s_status.initialized) {
        s_status.unavailable_count++;
        s_status.unavailable_status_count++;
    }
    *status = s_status;
    xSemaphoreGive(s_lock);
    return ESP_OK;
}

esp_err_t ethernet_service_dhcp_renew(void)
{
    esp_netif_t *netif = NULL;
    if (s_lock == NULL || s_dhcp_renew_lock == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (xSemaphoreTake(s_dhcp_renew_lock, 0) != pdTRUE) {
        ESP_LOGW(ETHERNET_SERVICE_TAG, "DHCP renew already in flight");
        return ESP_ERR_INVALID_STATE;
    }
    if (!lock_status(pdMS_TO_TICKS(ETHERNET_SERVICE_LOCK_TIMEOUT_MS))) {
        xSemaphoreGive(s_dhcp_renew_lock);
        return ESP_ERR_TIMEOUT;
    }
    if (!s_status.initialized || s_primary_netif == NULL) {
        s_status.unavailable_count++;
        s_status.unavailable_dhcp_renew_count++;
        s_status.last_error = ESP_ERR_INVALID_STATE;
        xSemaphoreGive(s_lock);
        xSemaphoreGive(s_dhcp_renew_lock);
        return ESP_ERR_INVALID_STATE;
    }
    netif = s_primary_netif;
    xSemaphoreGive(s_lock);

    esp_err_t ret = esp_netif_dhcpc_stop(netif);
    if (ret != ESP_OK && ret != ESP_ERR_ESP_NETIF_DHCP_ALREADY_STOPPED) {
        set_last_error(ret);
    } else {
        ret = esp_netif_dhcpc_start(netif);
        if (ret == ESP_ERR_ESP_NETIF_DHCP_ALREADY_STARTED) {
            ret = ESP_OK;
        }
    }

    if (lock_status(pdMS_TO_TICKS(ETHERNET_SERVICE_LOCK_TIMEOUT_MS))) {
        if (ret == ESP_OK) {
            s_status.dhcp_renew_count++;
            s_status.last_error = ESP_OK;
        } else {
            s_status.dhcp_renew_failure_count++;
            s_status.last_error = ret;
        }
        xSemaphoreGive(s_lock);
    }
    xSemaphoreGive(s_dhcp_renew_lock);
    return ret;
}
