#ifndef USB_STORAGE_STATE_H
#define USB_STORAGE_STATE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/** @brief USB session role. Use usb_storage_apply() for state transitions and usb_storage_can_close() for close eligibility. */
typedef enum {
    USB_STORAGE_ROLE_NONE = 0,
    USB_STORAGE_ROLE_DEVICE,
    USB_STORAGE_ROLE_HOST,
} usb_storage_role_t;

typedef enum {
    USB_STORAGE_LIFECYCLE_IDLE = 0,
    USB_STORAGE_LIFECYCLE_STARTING,
    USB_STORAGE_LIFECYCLE_ACTIVE,
    USB_STORAGE_LIFECYCLE_BUSY,
    USB_STORAGE_LIFECYCLE_STOPPING,
    USB_STORAGE_LIFECYCLE_ERROR,
} usb_storage_lifecycle_t;

typedef enum {
    USB_STORAGE_OPERATION_NONE = 0,
    USB_STORAGE_OPERATION_READ,
    USB_STORAGE_OPERATION_WRITE,
    USB_STORAGE_OPERATION_MOUNT,
    USB_STORAGE_OPERATION_UNMOUNT,
    USB_STORAGE_OPERATION_FORMAT,
} usb_storage_operation_t;

typedef enum {
    USB_STORAGE_EVENT_START = 0,
    USB_STORAGE_EVENT_START_COMPLETE,
    USB_STORAGE_EVENT_OPERATION_BEGIN,
    USB_STORAGE_EVENT_OPERATION_END,
    USB_STORAGE_EVENT_STOP,
    USB_STORAGE_EVENT_STOP_COMPLETE,
    USB_STORAGE_EVENT_FAIL,
    USB_STORAGE_EVENT_RECOVER,
} usb_storage_event_kind_t;

typedef enum {
    USB_STORAGE_ERROR_NONE = 0,
    USB_STORAGE_ERROR_INVALID_ARGUMENT,
    USB_STORAGE_ERROR_INVALID_TRANSITION,
    USB_STORAGE_ERROR_STALE_GENERATION,
    USB_STORAGE_ERROR_ROLE_IN_USE,
    USB_STORAGE_ERROR_BUSY,
    USB_STORAGE_ERROR_OPERATION_CONFLICT,
    USB_STORAGE_ERROR_IO,
    USB_STORAGE_ERROR_INTERNAL,
} usb_storage_error_t;

typedef enum {
    USB_STORAGE_BLOCK_NONE = 0,
    USB_STORAGE_BLOCK_STARTING,
    USB_STORAGE_BLOCK_ACTIVE,
    USB_STORAGE_BLOCK_BUSY,
    USB_STORAGE_BLOCK_STOPPING,
    USB_STORAGE_BLOCK_ERROR,
    USB_STORAGE_BLOCK_INVALID_STATE,
} usb_storage_block_reason_t;

typedef struct {
    usb_storage_event_kind_t kind;
    usb_storage_role_t role;
    usb_storage_operation_t operation;
    uint32_t generation;
    usb_storage_error_t error;
} usb_storage_event_t;

/** @brief Authoritative USB role, operation and generation state. */
typedef struct {
    usb_storage_role_t role;
    usb_storage_lifecycle_t lifecycle;
    usb_storage_operation_t operation;
    uint32_t generation;
    uint32_t busy_count;
    usb_storage_error_t last_error;
} usb_storage_state_t;

/**
 * @brief Initialize an idle USB storage session.
 * @param state State used by the operation.
 */
void usb_storage_state_init(usb_storage_state_t *state);

/**
 * @brief Validate and apply a generation-tagged session event.
 * @param state State used by the operation.
 * @param event Event to process.
 * @return The operation status.
 */
usb_storage_error_t usb_storage_apply(usb_storage_state_t *state,
                                      const usb_storage_event_t *event);

/**
 * @brief Check close eligibility and report any blocking reason.
 * @param state State used by the operation.
 * @param reason Reason for the operation.
 * @return true if the session can close; false if a resource blocks closure.
 */
bool usb_storage_can_close(const usb_storage_state_t *state,
                           usb_storage_block_reason_t *reason);

/**
 * @brief Return the stable name of a USB storage error.
 * @param error Error value.
 * @return Stable name of the error.
 */
const char *usb_storage_error_name(usb_storage_error_t error);
/**
 * @brief Return the stable name of a close-blocking reason.
 * @param reason Reason for the operation.
 * @return Stable name of the close-blocking reason.
 */
const char *usb_storage_block_reason_name(usb_storage_block_reason_t reason);

#endif
