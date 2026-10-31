#ifndef USB_MSC_HOST_H
#define USB_MSC_HOST_H

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "usb_storage_state.h"

#ifdef __cplusplus
extern "C" {
#endif

#define USB_MSC_HOST_MOUNT_PATH "/usb"
#define USB_MSC_HOST_HELLO_WORLD_PATH "/usb/HelloWorld.txt"

/** @brief USB host media, capacity and lifecycle snapshot. */
typedef struct {
    usb_storage_state_t storage;
    bool host_installed;
    bool msc_installed;
    bool media_present;
    bool mounted;
    bool callbacks_enabled;
    bool capacity_valid;
    uint64_t total_bytes;
    uint64_t free_bytes;
    int32_t last_error;
} usb_msc_host_snapshot_t;

/**
 * @brief Initialize the static service task and its serialized request path.
 * @return ESP_OK on success, or an error code on failure.
 */
esp_err_t usb_msc_host_init(void);
/**
 * @brief Acquire the host daemon and managed MSC client.
 * @return ESP_OK on success, or an error code on failure.
 */
esp_err_t usb_msc_host_start(void);
/**
 * @brief Stop the host, unmounting and releasing resources in reverse order.
 * @return ESP_OK on success, or an error code on failure.
 */
esp_err_t usb_msc_host_stop(void);
/**
 * @brief Stop if needed and release the service task; safe to call once per init.
 * @return ESP_OK on success, or an error code on failure.
 */
esp_err_t usb_msc_host_deinit(void);
/**
 * @brief Read and log total/free space of the mounted USB drive.
 * @return ESP_OK on success, or an error code on failure.
 */
esp_err_t usb_msc_host_read_capacity(void);
/**
 * @brief Create /usb/HelloWorld.txt with HelloWorld and the current local time.
 * @return ESP_OK on success, or an error code on failure.
 */
esp_err_t usb_msc_host_write_hello_world(void);
/**
 * @brief Copy the USB host lifecycle, media and capacity snapshot.
 * @param[out] snapshot Receives the copied state.
 * @return ESP_OK on success, or an error code on failure.
 */
esp_err_t usb_msc_host_get_snapshot(usb_msc_host_snapshot_t *snapshot);

#ifdef __cplusplus
}
#endif

#endif
