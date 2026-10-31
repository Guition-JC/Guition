#include "usb_storage_state.h"

static bool valid_role(usb_storage_role_t role)
{
    return role == USB_STORAGE_ROLE_NONE || role == USB_STORAGE_ROLE_DEVICE ||
           role == USB_STORAGE_ROLE_HOST;
}

static bool valid_session_role(usb_storage_role_t role)
{
    return role == USB_STORAGE_ROLE_DEVICE || role == USB_STORAGE_ROLE_HOST;
}

static bool valid_operation(usb_storage_operation_t operation)
{
    return operation >= USB_STORAGE_OPERATION_NONE &&
           operation <= USB_STORAGE_OPERATION_FORMAT;
}

static bool valid_event_kind(usb_storage_event_kind_t kind)
{
    return kind >= USB_STORAGE_EVENT_START && kind <= USB_STORAGE_EVENT_RECOVER;
}

static bool valid_error(usb_storage_error_t error)
{
    return error >= USB_STORAGE_ERROR_NONE && error <= USB_STORAGE_ERROR_INTERNAL;
}

static void set_reason(usb_storage_block_reason_t *reason, usb_storage_block_reason_t value)
{
    if (reason != NULL) {
        *reason = value;
    }
}

static usb_storage_block_reason_t lifecycle_block_reason(usb_storage_lifecycle_t lifecycle)
{
    switch (lifecycle) {
    case USB_STORAGE_LIFECYCLE_IDLE:
        return USB_STORAGE_BLOCK_NONE;
    case USB_STORAGE_LIFECYCLE_STARTING:
        return USB_STORAGE_BLOCK_STARTING;
    case USB_STORAGE_LIFECYCLE_ACTIVE:
        return USB_STORAGE_BLOCK_ACTIVE;
    case USB_STORAGE_LIFECYCLE_BUSY:
        return USB_STORAGE_BLOCK_BUSY;
    case USB_STORAGE_LIFECYCLE_STOPPING:
        return USB_STORAGE_BLOCK_STOPPING;
    case USB_STORAGE_LIFECYCLE_ERROR:
        return USB_STORAGE_BLOCK_ERROR;
    default:
        return USB_STORAGE_BLOCK_INVALID_STATE;
    }
}

static uint32_t next_counter(uint32_t value)
{
    return value == UINT32_MAX ? 1U : value + 1U;
}

static bool generation_required(usb_storage_event_kind_t kind)
{
    return kind != USB_STORAGE_EVENT_START;
}

static usb_storage_error_t validate_event(const usb_storage_event_t *event)
{
    if (!valid_event_kind(event->kind) || !valid_role(event->role) ||
        !valid_operation(event->operation) || !valid_error(event->error)) {
        return USB_STORAGE_ERROR_INVALID_ARGUMENT;
    }

    switch (event->kind) {
    case USB_STORAGE_EVENT_START:
        if (!valid_session_role(event->role) || event->operation != USB_STORAGE_OPERATION_NONE ||
            event->generation != 0U || event->error != USB_STORAGE_ERROR_NONE) {
            return USB_STORAGE_ERROR_INVALID_ARGUMENT;
        }
        break;
    case USB_STORAGE_EVENT_START_COMPLETE:
    case USB_STORAGE_EVENT_STOP_COMPLETE:
    case USB_STORAGE_EVENT_RECOVER:
        if (event->role != USB_STORAGE_ROLE_NONE || event->operation != USB_STORAGE_OPERATION_NONE ||
            event->generation == 0U || event->error != USB_STORAGE_ERROR_NONE) {
            return USB_STORAGE_ERROR_INVALID_ARGUMENT;
        }
        break;
    case USB_STORAGE_EVENT_OPERATION_BEGIN:
    case USB_STORAGE_EVENT_OPERATION_END:
        if (event->role != USB_STORAGE_ROLE_NONE ||
            event->operation == USB_STORAGE_OPERATION_NONE || event->generation == 0U ||
            event->error != USB_STORAGE_ERROR_NONE) {
            return USB_STORAGE_ERROR_INVALID_ARGUMENT;
        }
        break;
    case USB_STORAGE_EVENT_STOP:
        if (event->role != USB_STORAGE_ROLE_NONE || event->operation != USB_STORAGE_OPERATION_NONE ||
            event->error != USB_STORAGE_ERROR_NONE) {
            return USB_STORAGE_ERROR_INVALID_ARGUMENT;
        }
        break;
    case USB_STORAGE_EVENT_FAIL:
        if (event->role != USB_STORAGE_ROLE_NONE || event->operation != USB_STORAGE_OPERATION_NONE ||
            event->generation == 0U || event->error == USB_STORAGE_ERROR_NONE) {
            return USB_STORAGE_ERROR_INVALID_ARGUMENT;
        }
        break;
    default:
        return USB_STORAGE_ERROR_INVALID_ARGUMENT;
    }

    return USB_STORAGE_ERROR_NONE;
}

static usb_storage_error_t check_generation(const usb_storage_state_t *state,
                                            const usb_storage_event_t *event)
{
    if (!generation_required(event->kind) ||
        (event->kind == USB_STORAGE_EVENT_STOP &&
         state->lifecycle == USB_STORAGE_LIFECYCLE_IDLE)) {
        return USB_STORAGE_ERROR_NONE;
    }
    if (event->generation == 0U || event->generation != state->generation) {
        return USB_STORAGE_ERROR_STALE_GENERATION;
    }
    return USB_STORAGE_ERROR_NONE;
}

static void clear_session_resources(usb_storage_state_t *state)
{
    state->role = USB_STORAGE_ROLE_NONE;
    state->operation = USB_STORAGE_OPERATION_NONE;
    state->busy_count = 0U;
}

void usb_storage_state_init(usb_storage_state_t *state)
{
    if (state == NULL) {
        return;
    }

    state->role = USB_STORAGE_ROLE_NONE;
    state->lifecycle = USB_STORAGE_LIFECYCLE_IDLE;
    state->operation = USB_STORAGE_OPERATION_NONE;
    state->generation = 0U;
    state->busy_count = 0U;
    state->last_error = USB_STORAGE_ERROR_NONE;
}

usb_storage_error_t usb_storage_apply(usb_storage_state_t *state,
                                      const usb_storage_event_t *event)
{
    usb_storage_error_t result;

    if (state == NULL || event == NULL) {
        return USB_STORAGE_ERROR_INVALID_ARGUMENT;
    }

    result = validate_event(event);
    if (result != USB_STORAGE_ERROR_NONE) {
        return result;
    }

    result = check_generation(state, event);
    if (result != USB_STORAGE_ERROR_NONE) {
        return result;
    }

    switch (event->kind) {
    case USB_STORAGE_EVENT_START:
        if (state->lifecycle == USB_STORAGE_LIFECYCLE_IDLE) {
            state->role = event->role;
            state->lifecycle = USB_STORAGE_LIFECYCLE_STARTING;
            state->operation = USB_STORAGE_OPERATION_NONE;
            state->busy_count = 0U;
            state->generation = next_counter(state->generation);
            state->last_error = USB_STORAGE_ERROR_NONE;
            return USB_STORAGE_ERROR_NONE;
        }
        if ((state->lifecycle == USB_STORAGE_LIFECYCLE_STARTING ||
             state->lifecycle == USB_STORAGE_LIFECYCLE_ACTIVE ||
             state->lifecycle == USB_STORAGE_LIFECYCLE_BUSY) &&
            state->role == event->role) {
            return USB_STORAGE_ERROR_NONE;
        }
        if (state->lifecycle == USB_STORAGE_LIFECYCLE_STARTING ||
            state->lifecycle == USB_STORAGE_LIFECYCLE_ACTIVE ||
            state->lifecycle == USB_STORAGE_LIFECYCLE_BUSY) {
            return USB_STORAGE_ERROR_ROLE_IN_USE;
        }
        return USB_STORAGE_ERROR_INVALID_TRANSITION;

    case USB_STORAGE_EVENT_START_COMPLETE:
        if (state->lifecycle != USB_STORAGE_LIFECYCLE_STARTING) {
            return USB_STORAGE_ERROR_INVALID_TRANSITION;
        }
        state->lifecycle = USB_STORAGE_LIFECYCLE_ACTIVE;
        return USB_STORAGE_ERROR_NONE;

    case USB_STORAGE_EVENT_OPERATION_BEGIN:
        if (state->lifecycle != USB_STORAGE_LIFECYCLE_ACTIVE &&
            state->lifecycle != USB_STORAGE_LIFECYCLE_BUSY) {
            return USB_STORAGE_ERROR_INVALID_TRANSITION;
        }
        if (state->lifecycle == USB_STORAGE_LIFECYCLE_BUSY &&
            state->operation != event->operation) {
            return USB_STORAGE_ERROR_OPERATION_CONFLICT;
        }
        if (state->busy_count == UINT32_MAX) {
            return USB_STORAGE_ERROR_INTERNAL;
        }
        state->operation = event->operation;
        state->busy_count++;
        state->lifecycle = USB_STORAGE_LIFECYCLE_BUSY;
        return USB_STORAGE_ERROR_NONE;

    case USB_STORAGE_EVENT_OPERATION_END:
        if (state->lifecycle != USB_STORAGE_LIFECYCLE_BUSY || state->busy_count == 0U) {
            return USB_STORAGE_ERROR_INVALID_TRANSITION;
        }
        if (state->operation != event->operation) {
            return USB_STORAGE_ERROR_OPERATION_CONFLICT;
        }
        state->busy_count--;
        if (state->busy_count == 0U) {
            state->operation = USB_STORAGE_OPERATION_NONE;
            state->lifecycle = USB_STORAGE_LIFECYCLE_ACTIVE;
        }
        return USB_STORAGE_ERROR_NONE;

    case USB_STORAGE_EVENT_STOP:
        if (state->lifecycle == USB_STORAGE_LIFECYCLE_IDLE) {
            return USB_STORAGE_ERROR_NONE;
        }
        if (state->lifecycle == USB_STORAGE_LIFECYCLE_STOPPING) {
            return USB_STORAGE_ERROR_NONE;
        }
        if (state->lifecycle == USB_STORAGE_LIFECYCLE_BUSY) {
            return USB_STORAGE_ERROR_BUSY;
        }
        if (state->lifecycle != USB_STORAGE_LIFECYCLE_STARTING &&
            state->lifecycle != USB_STORAGE_LIFECYCLE_ACTIVE) {
            return USB_STORAGE_ERROR_INVALID_TRANSITION;
        }
        state->lifecycle = USB_STORAGE_LIFECYCLE_STOPPING;
        return USB_STORAGE_ERROR_NONE;

    case USB_STORAGE_EVENT_STOP_COMPLETE:
        if (state->lifecycle != USB_STORAGE_LIFECYCLE_STOPPING) {
            return USB_STORAGE_ERROR_INVALID_TRANSITION;
        }
        clear_session_resources(state);
        state->lifecycle = USB_STORAGE_LIFECYCLE_IDLE;
        state->last_error = USB_STORAGE_ERROR_NONE;
        return USB_STORAGE_ERROR_NONE;

    case USB_STORAGE_EVENT_FAIL:
        if (state->lifecycle == USB_STORAGE_LIFECYCLE_IDLE ||
            state->lifecycle == USB_STORAGE_LIFECYCLE_ERROR) {
            return USB_STORAGE_ERROR_INVALID_TRANSITION;
        }
        clear_session_resources(state);
        state->lifecycle = USB_STORAGE_LIFECYCLE_ERROR;
        state->last_error = event->error;
        return USB_STORAGE_ERROR_NONE;

    case USB_STORAGE_EVENT_RECOVER:
        if (state->lifecycle != USB_STORAGE_LIFECYCLE_ERROR) {
            return USB_STORAGE_ERROR_INVALID_TRANSITION;
        }
        state->lifecycle = USB_STORAGE_LIFECYCLE_IDLE;
        state->last_error = USB_STORAGE_ERROR_NONE;
        return USB_STORAGE_ERROR_NONE;

    default:
        return USB_STORAGE_ERROR_INVALID_ARGUMENT;
    }
}

bool usb_storage_can_close(const usb_storage_state_t *state,
                           usb_storage_block_reason_t *reason)
{
    if (state == NULL) {
        set_reason(reason, USB_STORAGE_BLOCK_INVALID_STATE);
        return false;
    }

    const usb_storage_block_reason_t block = lifecycle_block_reason(state->lifecycle);
    if (block != USB_STORAGE_BLOCK_NONE || state->role != USB_STORAGE_ROLE_NONE ||
        state->operation != USB_STORAGE_OPERATION_NONE || state->busy_count != 0U) {
        set_reason(reason, block == USB_STORAGE_BLOCK_NONE ? USB_STORAGE_BLOCK_INVALID_STATE : block);
        return false;
    }

    set_reason(reason, USB_STORAGE_BLOCK_NONE);
    return true;
}

const char *usb_storage_error_name(usb_storage_error_t error)
{
    switch (error) {
    case USB_STORAGE_ERROR_NONE:
        return "none";
    case USB_STORAGE_ERROR_INVALID_ARGUMENT:
        return "invalid-argument";
    case USB_STORAGE_ERROR_INVALID_TRANSITION:
        return "invalid-transition";
    case USB_STORAGE_ERROR_STALE_GENERATION:
        return "stale-generation";
    case USB_STORAGE_ERROR_ROLE_IN_USE:
        return "role-in-use";
    case USB_STORAGE_ERROR_BUSY:
        return "busy";
    case USB_STORAGE_ERROR_OPERATION_CONFLICT:
        return "operation-conflict";
    case USB_STORAGE_ERROR_IO:
        return "io";
    case USB_STORAGE_ERROR_INTERNAL:
        return "internal";
    default:
        return "unknown";
    }
}

const char *usb_storage_block_reason_name(usb_storage_block_reason_t reason)
{
    switch (reason) {
    case USB_STORAGE_BLOCK_NONE:
        return "none";
    case USB_STORAGE_BLOCK_STARTING:
        return "starting";
    case USB_STORAGE_BLOCK_ACTIVE:
        return "active";
    case USB_STORAGE_BLOCK_BUSY:
        return "busy";
    case USB_STORAGE_BLOCK_STOPPING:
        return "stopping";
    case USB_STORAGE_BLOCK_ERROR:
        return "error";
    case USB_STORAGE_BLOCK_INVALID_STATE:
        return "invalid-state";
    default:
        return "unknown";
    }
}
