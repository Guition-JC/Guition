#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "tinyusb_msc.h"
#include "usb_storage_state.h"

#ifdef __cplusplus
extern "C" {
#endif

#define USB_MSC_DEVICE_SD_DEVICE_NAME "fs_sdcard"
#define USB_MSC_DEVICE_SD_MOUNT_PATH "/sdcard"
#define USB_MSC_DEVICE_DEFAULT_WAIT_MS 5000U

typedef struct usb_msc_device_context *usb_msc_device_handle_t;

typedef enum {
    USB_MSC_DEVICE_RESOURCE_IDLE = 0,
    USB_MSC_DEVICE_RESOURCE_SD_ACQUIRED,
    USB_MSC_DEVICE_RESOURCE_SD_UNMOUNTED,
    USB_MSC_DEVICE_RESOURCE_MSC_STORAGE,
    USB_MSC_DEVICE_RESOURCE_TINYUSB,
    USB_MSC_DEVICE_RESOURCE_STOPPING,
    USB_MSC_DEVICE_RESOURCE_ERROR,
} usb_msc_device_resource_state_t;

typedef enum {
    USB_MSC_DEVICE_EVENT_STATE = 0,
    USB_MSC_DEVICE_EVENT_MOUNT_START,
    USB_MSC_DEVICE_EVENT_MOUNT_COMPLETE,
    USB_MSC_DEVICE_EVENT_MOUNT_FAILED,
    USB_MSC_DEVICE_EVENT_EJECTED,
    USB_MSC_DEVICE_EVENT_ATTACHED,
    USB_MSC_DEVICE_EVENT_DETACHED,
    USB_MSC_DEVICE_EVENT_ERROR,
} usb_msc_device_event_kind_t;

/** @brief Generation-tagged USB device lifecycle event. */
typedef struct {
    usb_msc_device_event_kind_t kind;
    usb_storage_lifecycle_t lifecycle;
    usb_storage_role_t role;
    uint32_t generation;
    bool attached;
    bool storage_idle;
    tinyusb_msc_mount_point_t mount_point;
    esp_err_t error;
} usb_msc_device_event_t;

typedef esp_err_t (*usb_msc_device_quiesce_cb_t)(void *arg);
typedef esp_err_t (*usb_msc_device_flush_cb_t)(void *arg);
typedef void (*usb_msc_device_event_cb_t)(const usb_msc_device_event_t *event, void *arg);

/** @brief USB storage lifecycle callbacks and timeout. */
typedef struct {
    usb_msc_device_quiesce_cb_t quiesce;
    usb_msc_device_flush_cb_t flush;
    usb_msc_device_event_cb_t event_callback;
    void *callback_arg;
    TickType_t wait_timeout;
} usb_msc_device_config_t;

#define USB_MSC_DEVICE_CONFIG_DEFAULT() \
    { \
        .quiesce = NULL, \
        .flush = NULL, \
        .event_callback = NULL, \
        .callback_arg = NULL, \
        .wait_timeout = pdMS_TO_TICKS(USB_MSC_DEVICE_DEFAULT_WAIT_MS), \
    }

/**
 * @brief Create a USB mass-storage device context with the supplied callbacks.
 * @param config Configuration to use.
 * @param handle Handle of the resource.
 * @return ESP_OK on success, or an error code on failure.
 */
esp_err_t usb_msc_device_init(const usb_msc_device_config_t *config,
                              usb_msc_device_handle_t *handle);
/**
 * @brief Release the USB mass-storage device context.
 * @param handle Handle of the resource.
 * @return ESP_OK on success, or an error code on failure.
 */
esp_err_t usb_msc_device_deinit(usb_msc_device_handle_t handle);
/**
 * @brief Acquire the SD card and expose it through USB mass storage.
 * @param handle Handle of the resource.
 * @return ESP_OK on success, or an error code on failure.
 */
esp_err_t usb_msc_device_start(usb_msc_device_handle_t handle);
/**
 * @brief Stop USB mass storage and restore local SD access.
 * @param handle Handle of the resource.
 * @return ESP_OK on success, or an error code on failure.
 */
esp_err_t usb_msc_device_stop(usb_msc_device_handle_t handle);
/**
 * @brief Begin an operation for the current session generation.
 * @param handle Handle of the resource.
 * @param operation Operation to perform.
 * @param generation Session generation.
 * @return ESP_OK on success, or an error code on failure.
 */
esp_err_t usb_msc_device_begin_operation(usb_msc_device_handle_t handle,
                                          usb_storage_operation_t operation,
                                          uint32_t generation);
/**
 * @brief Complete an operation for the matching session generation.
 * @param handle Handle of the resource.
 * @param operation Operation to perform.
 * @param generation Session generation.
 * @return ESP_OK on success, or an error code on failure.
 */
esp_err_t usb_msc_device_end_operation(usb_msc_device_handle_t handle,
                                        usb_storage_operation_t operation,
                                        uint32_t generation);
/**
 * @brief Copy the current USB storage session generation.
 * @param handle Handle of the resource.
 * @param[out] generation Receives the session generation.
 * @return ESP_OK on success, or an error code on failure.
 */
esp_err_t usb_msc_device_get_generation(usb_msc_device_handle_t handle,
                                        uint32_t *generation);
/**
 * @brief Copy the device storage lifecycle state.
 * @param handle Handle of the resource.
 * @param[out] state Receives the lifecycle state.
 * @return ESP_OK on success, or an error code on failure.
 */
esp_err_t usb_msc_device_get_state(usb_msc_device_handle_t handle,
                                   usb_storage_state_t *state);
/**
 * @brief Copy the current device resource acquisition state.
 * @param handle Handle of the resource.
 * @param[out] resource_state Receives the resource acquisition state.
 * @return ESP_OK on success, or an error code on failure.
 */
esp_err_t usb_msc_device_get_resource_state(usb_msc_device_handle_t handle,
                                            usb_msc_device_resource_state_t *resource_state);

#ifdef __cplusplus
}
#endif
