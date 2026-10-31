#ifndef USB_STORAGE_CONTROLLER_H
#define USB_STORAGE_CONTROLLER_H

#include <stdbool.h>
#include <stdint.h>

#include "usb_storage_state.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Serialize requests and backend callbacks through one worker-owned FIFO. */
typedef struct usb_storage_controller usb_storage_controller_t;

typedef enum {
    USB_STORAGE_BACKEND_EVENT_STARTED = 0,
    USB_STORAGE_BACKEND_EVENT_MEDIA_ATTACHED,
    USB_STORAGE_BACKEND_EVENT_MEDIA_MOUNTED,
    USB_STORAGE_BACKEND_EVENT_OPERATION_COMPLETE,
    USB_STORAGE_BACKEND_EVENT_MEDIA_REMOVED,
    USB_STORAGE_BACKEND_EVENT_STOP_COMPLETE,
    USB_STORAGE_BACKEND_EVENT_FAILED,
} usb_storage_backend_event_kind_t;

typedef struct {
    usb_storage_backend_event_kind_t kind;
    usb_storage_role_t role;
    usb_storage_operation_t operation;
    uint32_t generation;
    int result;
} usb_storage_backend_event_t;

typedef void (*usb_storage_backend_event_cb_t)(const usb_storage_backend_event_t *event,
                                                void *arg);

typedef int (*usb_storage_backend_start_fn_t)(void *context,
                                               usb_storage_role_t role,
                                               uint32_t generation,
                                               usb_storage_backend_event_cb_t callback,
                                               void *callback_arg);
typedef int (*usb_storage_backend_stop_fn_t)(void *context,
                                              usb_storage_role_t role,
                                              uint32_t generation);
typedef int (*usb_storage_backend_read_fn_t)(void *context, uint32_t generation);
typedef int (*usb_storage_backend_write_fn_t)(void *context, uint32_t generation);

/**
 * @brief Submit asynchronous backend operations and report completion with the session generation.
 * No callback may follow STOP_COMPLETE for that generation.
 */
typedef struct {
    void *context;
    usb_storage_backend_start_fn_t start;
    usb_storage_backend_stop_fn_t stop;
    usb_storage_backend_read_fn_t read;
    usb_storage_backend_write_fn_t write;
} usb_storage_backend_t;

typedef enum {
    USB_STORAGE_CONTROLLER_OK = 0,
    USB_STORAGE_CONTROLLER_INVALID_ARGUMENT,
    USB_STORAGE_CONTROLLER_BUSY,
    USB_STORAGE_CONTROLLER_BACKEND_ERROR,
    USB_STORAGE_CONTROLLER_STALE_EVENT,
    USB_STORAGE_CONTROLLER_QUEUE_FULL,
    USB_STORAGE_CONTROLLER_NOT_READY,
} usb_storage_controller_result_t;

typedef enum {
    USB_STORAGE_CONTROLLER_NOTIFICATION_STATE = 0,
    USB_STORAGE_CONTROLLER_NOTIFICATION_CLOSE_BLOCKED,
    USB_STORAGE_CONTROLLER_NOTIFICATION_CLOSE_READY,
    USB_STORAGE_CONTROLLER_NOTIFICATION_ERROR,
} usb_storage_controller_notification_kind_t;

typedef struct {
    usb_storage_controller_notification_kind_t kind;
    usb_storage_state_t state;
    usb_storage_role_t role;
    usb_storage_operation_t operation;
    uint32_t generation;
    usb_storage_block_reason_t block_reason;
    int result;
    bool media_present;
    bool mounted;
    bool backend_active;
    bool stop_requested;
    bool close_requested;
} usb_storage_controller_notification_t;

typedef void (*usb_storage_controller_notify_fn_t)(
    const usb_storage_controller_notification_t *notification,
    void *arg);

/** @brief Controller backends, notifications and queue capacity. */
typedef struct {
    usb_storage_backend_t device;
    usb_storage_backend_t host;
    usb_storage_controller_notify_fn_t notify;
    void *notify_arg;
    uint32_t queue_capacity;
} usb_storage_controller_config_t;

/** @brief Controller lifecycle and pending-work snapshot. */
typedef struct {
    usb_storage_state_t state;
    usb_storage_role_t active_role;
    usb_storage_operation_t operation;
    uint32_t generation;
    bool media_present;
    bool mounted;
    bool backend_active;
    bool backend_stopping;
    bool stop_requested;
    bool close_requested;
    bool worker_busy;
} usb_storage_controller_snapshot_t;

/**
 * @brief Create a serialized USB storage controller and its worker.
 * @param config Configuration to use.
 * @param[out] controller Receives the created controller.
 * @return Controller creation status.
 */
usb_storage_controller_result_t usb_storage_controller_create(
    const usb_storage_controller_config_t *config,
    usb_storage_controller_t **controller);
/**
 * @brief Queue activation of the requested USB role.
 * @param controller Controller instance.
 * @param role Role for the operation.
 * @return Status of the request to queue activation.
 */
usb_storage_controller_result_t usb_storage_controller_start(
    usb_storage_controller_t *controller, usb_storage_role_t role);
/**
 * @brief Queue shutdown of the active USB backend.
 * @param controller Controller instance.
 * @return Status of the request to queue shutdown.
 */
usb_storage_controller_result_t usb_storage_controller_stop(
    usb_storage_controller_t *controller);
/**
 * @brief Queue a read operation on the active backend.
 * @param controller Controller instance.
 * @return Status of the request to queue a read.
 */
usb_storage_controller_result_t usb_storage_controller_read(
    usb_storage_controller_t *controller);
/**
 * @brief Queue a write operation on the active backend.
 * @param controller Controller instance.
 * @return Status of the request to queue a write.
 */
usb_storage_controller_result_t usb_storage_controller_write(
    usb_storage_controller_t *controller);
/**
 * @brief Request closure after active resources become idle.
 * @param controller Controller instance.
 * @return Status of the close request.
 */
usb_storage_controller_result_t usb_storage_controller_request_close(
    usb_storage_controller_t *controller);
/**
 * @brief Queue a backend event for serialized processing.
 * @param controller Controller instance.
 * @param event Event to process.
 * @return Status of the request to queue the event.
 */
int usb_storage_controller_backend_event(
    usb_storage_controller_t *controller,
    const usb_storage_backend_event_t *event);
/**
 * @brief Copy the current controller state.
 * @param controller Controller instance.
 * @param[out] snapshot Receives the current controller state.
 * @return Status of the snapshot request.
 */
usb_storage_controller_result_t usb_storage_controller_get_snapshot(
    usb_storage_controller_t *controller,
    usb_storage_controller_snapshot_t *snapshot);
/**
 * @brief Destroy only when the worker, backend, media and pending release callbacks are idle.
 * @param controller Controller instance.
 * @return Controller destruction status.
 */
usb_storage_controller_result_t usb_storage_controller_destroy(
    usb_storage_controller_t *controller);
/**
 * @brief Return the stable name of a controller result.
 * @param result Result of the operation.
 * @return Stable name of the controller result.
 */
const char *usb_storage_controller_result_name(usb_storage_controller_result_t result);

#ifdef __cplusplus
}
#endif

#endif
