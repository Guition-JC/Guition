#include "usb_storage_controller.h"

static const char *const USB_STORAGE_TAG = "USBStorage";

#if defined(ESP_PLATFORM)
#include "esp_log.h"
#else
static inline void usb_storage_log_sink(const char *tag, const char *format, ...)
{
    (void)tag;
    (void)format;
}
#define ESP_LOGI(...) usb_storage_log_sink(__VA_ARGS__)
#define ESP_LOGW(...) usb_storage_log_sink(__VA_ARGS__)
#define ESP_LOGE(...) usb_storage_log_sink(__VA_ARGS__)
#endif

#include <errno.h>
#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <time.h>

#define USB_STORAGE_CONTROLLER_DEFAULT_QUEUE_CAPACITY 32U

typedef enum {
    COMMAND_START = 0,
    COMMAND_STOP,
    COMMAND_READ,
    COMMAND_WRITE,
    COMMAND_CLOSE,
    COMMAND_BACKEND_EVENT,
    COMMAND_QUEUE_OVERFLOW,
} command_kind_t;

typedef struct {
    command_kind_t kind;
    usb_storage_role_t role;
    usb_storage_backend_event_t event;
} command_t;

struct usb_storage_controller {
    pthread_mutex_t mutex;
    pthread_cond_t work_condition;
    pthread_t worker;
    command_t *commands;
    size_t capacity;
    size_t head;
    size_t count;
    bool worker_started;
    bool worker_busy;
    bool accepting;
    bool shutting_down;
    bool queue_overflow;
    unsigned callbacks_in_flight;

    usb_storage_backend_t device;
    usb_storage_backend_t host;
    usb_storage_controller_notify_fn_t notify;
    void *notify_arg;

    usb_storage_state_t state;
    usb_storage_role_t active_role;
    bool media_present;
    bool mounted;
    bool backend_active;
    bool backend_stopping;
    bool stop_requested;
    bool close_requested;
    bool start_queued;
};

static const char *role_name(usb_storage_role_t role)
{
    return role == USB_STORAGE_ROLE_DEVICE ? "device" :
           role == USB_STORAGE_ROLE_HOST ? "host" : "none";
}

static const char *command_name(command_kind_t kind)
{
    switch (kind) {
    case COMMAND_START: return "start";
    case COMMAND_STOP: return "stop";
    case COMMAND_READ: return "read";
    case COMMAND_WRITE: return "write";
    case COMMAND_CLOSE: return "close";
    case COMMAND_BACKEND_EVENT: return "backend-event";
    case COMMAND_QUEUE_OVERFLOW: return "queue-overflow";
    default: return "unknown";
    }
}

static bool valid_role(usb_storage_role_t role)
{
    return role == USB_STORAGE_ROLE_DEVICE || role == USB_STORAGE_ROLE_HOST;
}

static bool valid_backend_event_kind(usb_storage_backend_event_kind_t kind)
{
    return kind >= USB_STORAGE_BACKEND_EVENT_STARTED &&
           kind <= USB_STORAGE_BACKEND_EVENT_FAILED;
}

static bool valid_operation(usb_storage_operation_t operation)
{
    return operation >= USB_STORAGE_OPERATION_NONE &&
           operation <= USB_STORAGE_OPERATION_FORMAT;
}

static usb_storage_backend_t *backend_for_role(usb_storage_controller_t *controller,
                                                usb_storage_role_t role)
{
    if (role == USB_STORAGE_ROLE_DEVICE) return &controller->device;
    if (role == USB_STORAGE_ROLE_HOST) return &controller->host;
    return NULL;
}

static bool backend_is_complete(const usb_storage_backend_t *backend)
{
    return backend != NULL && backend->context != NULL && backend->start != NULL &&
           backend->stop != NULL;
}

static bool enqueue_locked(usb_storage_controller_t *controller, const command_t *command)
{
    if (controller->count == controller->capacity) {
        ESP_LOGW(USB_STORAGE_TAG, "queue-full generation=%u",
                 (unsigned int)controller->state.generation);
        return false;
    }
    const size_t index = (controller->head + controller->count) % controller->capacity;
    controller->commands[index] = *command;
    controller->count++;
    (void)pthread_cond_signal(&controller->work_condition);
    return true;
}

static bool dequeue_locked(usb_storage_controller_t *controller, command_t *command)
{
    if (controller->count == 0U) return false;
    *command = controller->commands[controller->head];
    controller->head = (controller->head + 1U) % controller->capacity;
    controller->count--;
    return true;
}

static void fill_notification_locked(const usb_storage_controller_t *controller,
                                     usb_storage_controller_notification_kind_t kind,
                                     int result,
                                     usb_storage_block_reason_t reason,
                                     usb_storage_controller_notification_t *notification)
{
    *notification = (usb_storage_controller_notification_t) {
        .kind = kind,
        .state = controller->state,
        .role = controller->active_role,
        .operation = controller->state.operation,
        .generation = controller->state.generation,
        .block_reason = reason,
        .result = result,
        .media_present = controller->media_present,
        .mounted = controller->mounted,
        .backend_active = controller->backend_active,
        .stop_requested = controller->stop_requested,
        .close_requested = controller->close_requested,
    };
}

static void notify(usb_storage_controller_t *controller,
                   usb_storage_controller_notification_kind_t kind,
                   int result,
                   usb_storage_block_reason_t reason)
{
    usb_storage_controller_notify_fn_t callback;
    void *callback_arg;
    usb_storage_controller_notification_t notification;

    (void)pthread_mutex_lock(&controller->mutex);
    callback = controller->notify;
    callback_arg = controller->notify_arg;
    fill_notification_locked(controller, kind, result, reason, &notification);
    (void)pthread_mutex_unlock(&controller->mutex);
    if (callback != NULL) callback(&notification, callback_arg);
}

static void backend_event_callback(const usb_storage_backend_event_t *event, void *arg)
{
    if (event == NULL) {
        ESP_LOGW(USB_STORAGE_TAG, "backend-event null");
        return;
    }
    ESP_LOGI(USB_STORAGE_TAG, "backend-event role=%s generation=%u kind=%d operation=%d result=%d",
             role_name(event->role), (unsigned int)event->generation, (int)event->kind,
             (int)event->operation, event->result);
    (void)usb_storage_controller_backend_event((usb_storage_controller_t *)arg, event);
}

static void record_backend_error(usb_storage_controller_t *controller, int result)
{
    (void)pthread_mutex_lock(&controller->mutex);
    controller->state.last_error = USB_STORAGE_ERROR_IO;
    (void)pthread_mutex_unlock(&controller->mutex);
    notify(controller, USB_STORAGE_CONTROLLER_NOTIFICATION_ERROR, result,
           USB_STORAGE_BLOCK_INVALID_STATE);
}

static bool state_is_idle_locked(const usb_storage_controller_t *controller)
{
    usb_storage_block_reason_t reason = USB_STORAGE_BLOCK_INVALID_STATE;
    return usb_storage_can_close(&controller->state, &reason) &&
           controller->active_role == USB_STORAGE_ROLE_NONE &&
           !controller->backend_active && !controller->backend_stopping &&
           !controller->media_present && !controller->mounted;
}

static void mark_failure_locked(usb_storage_controller_t *controller, int result)
{
    if (controller->state.lifecycle != USB_STORAGE_LIFECYCLE_IDLE &&
        controller->state.lifecycle != USB_STORAGE_LIFECYCLE_ERROR) {
        const usb_storage_event_t event = {
            .kind = USB_STORAGE_EVENT_FAIL,
            .generation = controller->state.generation,
            .error = USB_STORAGE_ERROR_IO,
        };
        (void)usb_storage_apply(&controller->state, &event);
    }
    controller->stop_requested = true;
    controller->state.last_error = USB_STORAGE_ERROR_IO;
    (void)result;
}

static void issue_stop(usb_storage_controller_t *controller)
{
    usb_storage_backend_t backend;
    usb_storage_role_t role;
    uint32_t generation;
    bool call_stop = false;

    (void)pthread_mutex_lock(&controller->mutex);
    if (!controller->backend_active || controller->backend_stopping) {
        (void)pthread_mutex_unlock(&controller->mutex);
        return;
    }
    if (controller->state.lifecycle == USB_STORAGE_LIFECYCLE_BUSY) {
        controller->stop_requested = true;
        (void)pthread_mutex_unlock(&controller->mutex);
        return;
    }
    if (controller->state.lifecycle == USB_STORAGE_LIFECYCLE_STARTING ||
        controller->state.lifecycle == USB_STORAGE_LIFECYCLE_ACTIVE) {
        const usb_storage_event_t event = {
            .kind = USB_STORAGE_EVENT_STOP,
            .generation = controller->state.generation,
        };
        (void)usb_storage_apply(&controller->state, &event);
    }
    controller->stop_requested = true;
    controller->backend_stopping = true;
    role = controller->active_role;
    generation = controller->state.generation;
    backend = *backend_for_role(controller, role);
    call_stop = true;
    (void)pthread_mutex_unlock(&controller->mutex);

    notify(controller, USB_STORAGE_CONTROLLER_NOTIFICATION_STATE, 0,
           USB_STORAGE_BLOCK_NONE);
    if (call_stop) {
        ESP_LOGI(USB_STORAGE_TAG, "backend-stop role=%s generation=%u", role_name(role),
                 (unsigned int)generation);
        const int result = backend.stop(backend.context, role, generation);
        ESP_LOGI(USB_STORAGE_TAG, "backend-stop-result role=%s generation=%u result=%d",
                 role_name(role), (unsigned int)generation, result);
        if (result != 0) record_backend_error(controller, result);
    }
}

static void start_role(usb_storage_controller_t *controller, usb_storage_role_t role)
{
    usb_storage_backend_t backend;
    uint32_t generation;
    bool call_start = false;

    (void)pthread_mutex_lock(&controller->mutex);
    controller->start_queued = false;
    if (!valid_role(role) || !state_is_idle_locked(controller)) {
        ESP_LOGW(USB_STORAGE_TAG, "start rejected role=%s generation=%u", role_name(role),
                 (unsigned int)controller->state.generation);
        (void)pthread_mutex_unlock(&controller->mutex);
        return;
    }
    const usb_storage_event_t event = {
        .kind = USB_STORAGE_EVENT_START,
        .role = role,
    };
    if (usb_storage_apply(&controller->state, &event) != USB_STORAGE_ERROR_NONE) {
        (void)pthread_mutex_unlock(&controller->mutex);
        notify(controller, USB_STORAGE_CONTROLLER_NOTIFICATION_ERROR, -1,
               USB_STORAGE_BLOCK_INVALID_STATE);
        return;
    }
    controller->active_role = role;
    controller->media_present = false;
    controller->mounted = false;
    controller->backend_active = true;
    controller->backend_stopping = false;
    controller->stop_requested = false;
    generation = controller->state.generation;
    backend = *backend_for_role(controller, role);
    ESP_LOGI(USB_STORAGE_TAG, "start role=%s generation=%u", role_name(role),
             (unsigned int)generation);
    call_start = true;
    (void)pthread_mutex_unlock(&controller->mutex);

    notify(controller, USB_STORAGE_CONTROLLER_NOTIFICATION_STATE, 0,
           USB_STORAGE_BLOCK_NONE);
    if (call_start) {
        const int result = backend.start(backend.context, role, generation,
                                         backend_event_callback, controller);
        ESP_LOGI(USB_STORAGE_TAG, "backend-start-result role=%s generation=%u result=%d",
                 role_name(role), (unsigned int)generation, result);
        if (result != 0) {
            (void)pthread_mutex_lock(&controller->mutex);
            if (controller->backend_active && controller->state.generation == generation) {
                mark_failure_locked(controller, result);
                controller->backend_stopping = true;
            }
            (void)pthread_mutex_unlock(&controller->mutex);
            notify(controller, USB_STORAGE_CONTROLLER_NOTIFICATION_ERROR, result,
                   USB_STORAGE_BLOCK_ERROR);
            const int stop_result = backend.stop(backend.context, role, generation);
            if (stop_result != 0) record_backend_error(controller, stop_result);
        }
    }
}

static void finish_stop(usb_storage_controller_t *controller)
{
    ESP_LOGI(USB_STORAGE_TAG, "finish-stop generation=%u",
             (unsigned int)controller->state.generation);
    bool close_ready;

    (void)pthread_mutex_lock(&controller->mutex);
    controller->backend_active = false;
    controller->backend_stopping = false;
    controller->media_present = false;
    controller->mounted = false;
    if (controller->state.lifecycle == USB_STORAGE_LIFECYCLE_STOPPING) {
        const usb_storage_event_t event = {
            .kind = USB_STORAGE_EVENT_STOP_COMPLETE,
            .generation = controller->state.generation,
        };
        (void)usb_storage_apply(&controller->state, &event);
    } else if (controller->state.lifecycle == USB_STORAGE_LIFECYCLE_ERROR) {
        const usb_storage_event_t event = {
            .kind = USB_STORAGE_EVENT_RECOVER,
            .generation = controller->state.generation,
        };
        (void)usb_storage_apply(&controller->state, &event);
    }
    controller->active_role = USB_STORAGE_ROLE_NONE;
    controller->stop_requested = false;
    close_ready = controller->close_requested && state_is_idle_locked(controller);
    controller->close_requested = false;
    (void)pthread_mutex_unlock(&controller->mutex);

    notify(controller, USB_STORAGE_CONTROLLER_NOTIFICATION_STATE, 0,
           USB_STORAGE_BLOCK_NONE);
    if (close_ready) {
        notify(controller, USB_STORAGE_CONTROLLER_NOTIFICATION_CLOSE_READY, 0,
               USB_STORAGE_BLOCK_NONE);
    }
}

static void complete_operation(usb_storage_controller_t *controller,
                               const usb_storage_backend_event_t *event)
{
    bool stop_after = false;

    (void)pthread_mutex_lock(&controller->mutex);
    if (event->result != 0) {
        mark_failure_locked(controller, event->result);
        stop_after = true;
    } else if (controller->state.lifecycle == USB_STORAGE_LIFECYCLE_BUSY &&
               controller->state.operation == event->operation) {
        const usb_storage_event_t state_event = {
            .kind = USB_STORAGE_EVENT_OPERATION_END,
            .operation = event->operation,
            .generation = event->generation,
        };
        if (usb_storage_apply(&controller->state, &state_event) != USB_STORAGE_ERROR_NONE) {
            mark_failure_locked(controller, -1);
            stop_after = true;
        } else {
            stop_after = controller->stop_requested;
        }
    }
    (void)pthread_mutex_unlock(&controller->mutex);

    if (event->result != 0) {
        notify(controller, USB_STORAGE_CONTROLLER_NOTIFICATION_ERROR, event->result,
               USB_STORAGE_BLOCK_BUSY);
    } else {
        notify(controller, USB_STORAGE_CONTROLLER_NOTIFICATION_STATE, 0,
               USB_STORAGE_BLOCK_NONE);
    }
    if (stop_after) issue_stop(controller);
}

static void process_backend_event(usb_storage_controller_t *controller,
                                  const usb_storage_backend_event_t *event)
{
    bool issue_stop_after = false;
    if (event != NULL) {
        ESP_LOGI(USB_STORAGE_TAG, "process-backend-event role=%s generation=%u kind=%d operation=%d result=%d",
                 role_name(event->role), (unsigned int)event->generation, (int)event->kind,
                 (int)event->operation, event->result);
    }
    bool finish_stop_after = false;

    (void)pthread_mutex_lock(&controller->mutex);
    if (!valid_role(event->role) || event->generation == 0U ||
        event->generation != controller->state.generation ||
        event->role != controller->active_role) {
        ESP_LOGW(USB_STORAGE_TAG, "stale-event role=%s generation=%u active-generation=%u",
                 role_name(event->role), (unsigned int)event->generation,
                 (unsigned int)controller->state.generation);
        (void)pthread_mutex_unlock(&controller->mutex);
        notify(controller, USB_STORAGE_CONTROLLER_NOTIFICATION_ERROR,
               USB_STORAGE_CONTROLLER_STALE_EVENT, USB_STORAGE_BLOCK_INVALID_STATE);
        return;
    }

    switch (event->kind) {
    case USB_STORAGE_BACKEND_EVENT_STARTED:
        if (controller->state.lifecycle == USB_STORAGE_LIFECYCLE_STARTING) {
            const usb_storage_event_t state_event = {
                .kind = USB_STORAGE_EVENT_START_COMPLETE,
                .generation = event->generation,
            };
            (void)usb_storage_apply(&controller->state, &state_event);
        }
        break;
    case USB_STORAGE_BACKEND_EVENT_MEDIA_ATTACHED:
        controller->media_present = true;
        break;
    case USB_STORAGE_BACKEND_EVENT_MEDIA_MOUNTED:
        controller->media_present = true;
        controller->mounted = true;
        break;
    case USB_STORAGE_BACKEND_EVENT_OPERATION_COMPLETE:
        (void)pthread_mutex_unlock(&controller->mutex);
        complete_operation(controller, event);
        return;
    case USB_STORAGE_BACKEND_EVENT_MEDIA_REMOVED:
        controller->media_present = false;
        controller->mounted = false;
        if (controller->state.lifecycle != USB_STORAGE_LIFECYCLE_IDLE &&
            controller->state.lifecycle != USB_STORAGE_LIFECYCLE_ERROR) {
            mark_failure_locked(controller, event->result == 0 ? -1 : event->result);
            issue_stop_after = true;
        }
        break;
    case USB_STORAGE_BACKEND_EVENT_FAILED:
        mark_failure_locked(controller, event->result == 0 ? -1 : event->result);
        issue_stop_after = true;
        break;
    case USB_STORAGE_BACKEND_EVENT_STOP_COMPLETE:
        if (controller->backend_stopping) finish_stop_after = true;
        break;
    default:
        break;
    }
    (void)pthread_mutex_unlock(&controller->mutex);

    notify(controller, USB_STORAGE_CONTROLLER_NOTIFICATION_STATE,
           event->result, USB_STORAGE_BLOCK_NONE);
    if (issue_stop_after) issue_stop(controller);
    if (finish_stop_after) finish_stop(controller);
}

static void process_read_write(usb_storage_controller_t *controller,
                               usb_storage_operation_t operation)
{
    usb_storage_backend_t backend;
    uint32_t generation = 0U;
    int result;
    bool valid = false;

    (void)pthread_mutex_lock(&controller->mutex);
    if (controller->state.lifecycle == USB_STORAGE_LIFECYCLE_ACTIVE &&
        controller->active_role == USB_STORAGE_ROLE_HOST && controller->media_present &&
        controller->mounted && operation != USB_STORAGE_OPERATION_NONE) {
        const usb_storage_event_t event = {
            .kind = USB_STORAGE_EVENT_OPERATION_BEGIN,
            .operation = operation,
            .generation = controller->state.generation,
        };
        valid = usb_storage_apply(&controller->state, &event) == USB_STORAGE_ERROR_NONE;
    }
    if (valid) {
        backend = *backend_for_role(controller, USB_STORAGE_ROLE_HOST);
        generation = controller->state.generation;
    }
    (void)pthread_mutex_unlock(&controller->mutex);

    if (!valid) {
        ESP_LOGW(USB_STORAGE_TAG, "operation=%d rejected generation=%u", (int)operation,
                 (unsigned int)generation);
        notify(controller, USB_STORAGE_CONTROLLER_NOTIFICATION_ERROR,
               USB_STORAGE_CONTROLLER_NOT_READY, USB_STORAGE_BLOCK_INVALID_STATE);
        return;
    }
    notify(controller, USB_STORAGE_CONTROLLER_NOTIFICATION_STATE, 0,
           USB_STORAGE_BLOCK_BUSY);
    ESP_LOGI(USB_STORAGE_TAG, "operation=%d role=host generation=%u",
             (int)operation, (unsigned int)generation);
    result = operation == USB_STORAGE_OPERATION_READ
                 ? backend.read(backend.context, generation)
                 : backend.write(backend.context, generation);
    ESP_LOGI(USB_STORAGE_TAG, "operation-result=%d role=host generation=%u result=%d",
             (int)operation, (unsigned int)generation, result);
    if (result != 0) {
        (void)pthread_mutex_lock(&controller->mutex);
        mark_failure_locked(controller, result);
        (void)pthread_mutex_unlock(&controller->mutex);
        notify(controller, USB_STORAGE_CONTROLLER_NOTIFICATION_ERROR, result,
               USB_STORAGE_BLOCK_BUSY);
        issue_stop(controller);
    }
}

static void process_stop_command(usb_storage_controller_t *controller, bool close_request)
{
    usb_storage_block_reason_t reason = USB_STORAGE_BLOCK_NONE;
    (void)pthread_mutex_lock(&controller->mutex);
    (void)usb_storage_can_close(&controller->state, &reason);
    (void)pthread_mutex_unlock(&controller->mutex);
    if (close_request && reason != USB_STORAGE_BLOCK_NONE) {
        notify(controller, USB_STORAGE_CONTROLLER_NOTIFICATION_CLOSE_BLOCKED, 0, reason);
    }
    issue_stop(controller);
}

static void process_command(usb_storage_controller_t *controller, const command_t *command)
{
    ESP_LOGI(USB_STORAGE_TAG, "worker-command=%s role=%s generation=%u",
             command_name(command->kind), role_name(command->role),
             (unsigned int)controller->state.generation);
    switch (command->kind) {
    case COMMAND_START:
        start_role(controller, command->role);
        break;
    case COMMAND_STOP:
        process_stop_command(controller, false);
        break;
    case COMMAND_CLOSE:
        process_stop_command(controller, true);
        break;
    case COMMAND_READ:
        process_read_write(controller, USB_STORAGE_OPERATION_READ);
        break;
    case COMMAND_WRITE:
        process_read_write(controller, USB_STORAGE_OPERATION_WRITE);
        break;
    case COMMAND_BACKEND_EVENT:
        process_backend_event(controller, &command->event);
        break;
    case COMMAND_QUEUE_OVERFLOW:
        (void)pthread_mutex_lock(&controller->mutex);
        mark_failure_locked(controller, USB_STORAGE_CONTROLLER_QUEUE_FULL);
        (void)pthread_mutex_unlock(&controller->mutex);
        notify(controller, USB_STORAGE_CONTROLLER_NOTIFICATION_ERROR,
               USB_STORAGE_CONTROLLER_QUEUE_FULL, USB_STORAGE_BLOCK_INVALID_STATE);
        issue_stop(controller);
        break;
    default:
        break;
    }
}

static void *controller_worker(void *arg)
{
    usb_storage_controller_t *controller = (usb_storage_controller_t *)arg;

    while (true) {
        command_t command;
        bool should_exit = false;
        (void)pthread_mutex_lock(&controller->mutex);
        while (controller->count == 0U && !controller->queue_overflow &&
               !controller->shutting_down) {
            (void)pthread_cond_wait(&controller->work_condition, &controller->mutex);
        }
        if (controller->count == 0U && controller->queue_overflow) {
            controller->queue_overflow = false;
            command = (command_t) {.kind = COMMAND_QUEUE_OVERFLOW};
        } else if (controller->count != 0U) {
            (void)dequeue_locked(controller, &command);
        } else {
            should_exit = true;
        }
        if (should_exit) {
            (void)pthread_mutex_unlock(&controller->mutex);
            break;
        }
        controller->worker_busy = true;
        ESP_LOGI(USB_STORAGE_TAG, "worker-state busy=1 generation=%u queue=%u",
                 (unsigned int)controller->state.generation, (unsigned int)controller->count);
        (void)pthread_mutex_unlock(&controller->mutex);

        process_command(controller, &command);

        (void)pthread_mutex_lock(&controller->mutex);
        controller->worker_busy = false;
        ESP_LOGI(USB_STORAGE_TAG, "worker-state busy=0 generation=%u lifecycle=%d",
                 (unsigned int)controller->state.generation, (int)controller->state.lifecycle);
        (void)pthread_mutex_unlock(&controller->mutex);
    }
    return NULL;
}

static usb_storage_controller_result_t enqueue_request(usb_storage_controller_t *controller,
                                                         command_kind_t kind,
                                                         usb_storage_role_t role)
{
    const command_t command = {.kind = kind, .role = role};
    (void)pthread_mutex_lock(&controller->mutex);
    ESP_LOGI(USB_STORAGE_TAG, "request command=%s role=%s generation=%u",
             command_name(kind), role_name(role),
             (unsigned int)controller->state.generation);
    if (!controller->accepting) {
        (void)pthread_mutex_unlock(&controller->mutex);
        ESP_LOGW(USB_STORAGE_TAG, "request-result command=%s result=not-ready",
                 command_name(kind));
        return USB_STORAGE_CONTROLLER_NOT_READY;
    }
    if (!enqueue_locked(controller, &command)) {
        (void)pthread_mutex_unlock(&controller->mutex);
        ESP_LOGW(USB_STORAGE_TAG, "request-result command=%s result=queue-full",
                 command_name(kind));
        return USB_STORAGE_CONTROLLER_QUEUE_FULL;
    }
    (void)pthread_mutex_unlock(&controller->mutex);
    ESP_LOGI(USB_STORAGE_TAG, "request-result command=%s result=ok", command_name(kind));
    return USB_STORAGE_CONTROLLER_OK;
}

usb_storage_controller_result_t usb_storage_controller_create(
    const usb_storage_controller_config_t *config,
    usb_storage_controller_t **controller)
{
    if (config == NULL || controller == NULL || *controller != NULL ||
        !backend_is_complete(&config->device) || !backend_is_complete(&config->host) ||
        config->host.read == NULL || config->host.write == NULL) {
        return USB_STORAGE_CONTROLLER_INVALID_ARGUMENT;
    }

    const uint32_t requested_capacity = config->queue_capacity == 0U
                                            ? USB_STORAGE_CONTROLLER_DEFAULT_QUEUE_CAPACITY
                                            : config->queue_capacity;
    usb_storage_controller_t *created = calloc(1U, sizeof(*created));
    if (created == NULL) return USB_STORAGE_CONTROLLER_BACKEND_ERROR;
    created->commands = calloc(requested_capacity, sizeof(*created->commands));
    if (created->commands == NULL) {
        free(created);
        return USB_STORAGE_CONTROLLER_BACKEND_ERROR;
    }
    bool mutex_initialized = false;
    bool work_condition_initialized = false;
    if (pthread_mutex_init(&created->mutex, NULL) != 0) {
        goto synchronization_failure;
    }
    mutex_initialized = true;
    if (pthread_cond_init(&created->work_condition, NULL) != 0) {
        goto synchronization_failure;
    }
    work_condition_initialized = true;
    goto synchronization_ready;

synchronization_failure:
    if (work_condition_initialized) (void)pthread_cond_destroy(&created->work_condition);
    if (mutex_initialized) (void)pthread_mutex_destroy(&created->mutex);
    free(created->commands);
    free(created);
    return USB_STORAGE_CONTROLLER_BACKEND_ERROR;

synchronization_ready:
    created->capacity = requested_capacity;
    created->device = config->device;
    created->host = config->host;
    created->notify = config->notify;
    created->notify_arg = config->notify_arg;
    usb_storage_state_init(&created->state);
    created->accepting = true;
    if (pthread_create(&created->worker, NULL, controller_worker, created) != 0) {
        (void)pthread_cond_destroy(&created->work_condition);
        (void)pthread_mutex_destroy(&created->mutex);
        free(created->commands);
        free(created);
        return USB_STORAGE_CONTROLLER_BACKEND_ERROR;
    }
    created->worker_started = true;
    *controller = created;
    ESP_LOGI(USB_STORAGE_TAG, "init result=ok generation=%u",
             (unsigned int)created->state.generation);
    return USB_STORAGE_CONTROLLER_OK;
}

usb_storage_controller_result_t usb_storage_controller_start(
    usb_storage_controller_t *controller, usb_storage_role_t role)
{
    if (controller == NULL || !valid_role(role)) return USB_STORAGE_CONTROLLER_INVALID_ARGUMENT;
    (void)pthread_mutex_lock(&controller->mutex);
    if (!controller->accepting) {
        (void)pthread_mutex_unlock(&controller->mutex);
        return USB_STORAGE_CONTROLLER_NOT_READY;
    }
    if (!state_is_idle_locked(controller) || controller->start_queued) {
        (void)pthread_mutex_unlock(&controller->mutex);
        return USB_STORAGE_CONTROLLER_BUSY;
    }
    const command_t command = {.kind = COMMAND_START, .role = role};
    if (!enqueue_locked(controller, &command)) {
        (void)pthread_mutex_unlock(&controller->mutex);
        return USB_STORAGE_CONTROLLER_QUEUE_FULL;
    }
    controller->start_queued = true;
    ESP_LOGI(USB_STORAGE_TAG, "request command=start role=%s generation=%u result=ok",
             role_name(role), (unsigned int)controller->state.generation);
    (void)pthread_mutex_unlock(&controller->mutex);
    return USB_STORAGE_CONTROLLER_OK;
}

usb_storage_controller_result_t usb_storage_controller_stop(
    usb_storage_controller_t *controller)
{
    if (controller == NULL) return USB_STORAGE_CONTROLLER_INVALID_ARGUMENT;
    return enqueue_request(controller, COMMAND_STOP, USB_STORAGE_ROLE_NONE);
}

usb_storage_controller_result_t usb_storage_controller_read(
    usb_storage_controller_t *controller)
{
    if (controller == NULL) return USB_STORAGE_CONTROLLER_INVALID_ARGUMENT;
    return enqueue_request(controller, COMMAND_READ, USB_STORAGE_ROLE_NONE);
}

usb_storage_controller_result_t usb_storage_controller_write(
    usb_storage_controller_t *controller)
{
    if (controller == NULL) return USB_STORAGE_CONTROLLER_INVALID_ARGUMENT;
    return enqueue_request(controller, COMMAND_WRITE, USB_STORAGE_ROLE_NONE);
}

usb_storage_controller_result_t usb_storage_controller_request_close(
    usb_storage_controller_t *controller)
{
    if (controller == NULL) return USB_STORAGE_CONTROLLER_INVALID_ARGUMENT;
    ESP_LOGI(USB_STORAGE_TAG, "request command=close");
    (void)pthread_mutex_lock(&controller->mutex);
    if (state_is_idle_locked(controller)) {
        (void)pthread_mutex_unlock(&controller->mutex);
        return USB_STORAGE_CONTROLLER_OK;
    }
    if (!controller->accepting) {
        (void)pthread_mutex_unlock(&controller->mutex);
        return USB_STORAGE_CONTROLLER_NOT_READY;
    }
    if (controller->close_requested) {
        (void)pthread_mutex_unlock(&controller->mutex);
        return USB_STORAGE_CONTROLLER_BUSY;
    }
    controller->close_requested = true;
    ESP_LOGI(USB_STORAGE_TAG, "request command=close role=%s generation=%u",
             role_name(controller->active_role), (unsigned int)controller->state.generation);
    const command_t command = {.kind = COMMAND_CLOSE};
    if (!enqueue_locked(controller, &command)) {
        controller->close_requested = false;
        (void)pthread_mutex_unlock(&controller->mutex);
        return USB_STORAGE_CONTROLLER_QUEUE_FULL;
    }
    (void)pthread_mutex_unlock(&controller->mutex);
    return USB_STORAGE_CONTROLLER_BUSY;
}

int usb_storage_controller_backend_event(
    usb_storage_controller_t *controller,
    const usb_storage_backend_event_t *event)
{
    if (controller == NULL || event == NULL || !valid_backend_event_kind(event->kind) ||
        !valid_role(event->role) || !valid_operation(event->operation) ||
        event->generation == 0U) {
        return USB_STORAGE_CONTROLLER_INVALID_ARGUMENT;
    }
    (void)pthread_mutex_lock(&controller->mutex);
    if (!controller->accepting) {
        (void)pthread_mutex_unlock(&controller->mutex);
        return USB_STORAGE_CONTROLLER_NOT_READY;
    }
    controller->callbacks_in_flight++;
    const command_t command = {.kind = COMMAND_BACKEND_EVENT, .event = *event};
    const bool queued = enqueue_locked(controller, &command);
    if (!queued) {
        controller->queue_overflow = true;
        (void)pthread_cond_signal(&controller->work_condition);
    }
    controller->callbacks_in_flight--;
    (void)pthread_mutex_unlock(&controller->mutex);
    return queued ? USB_STORAGE_CONTROLLER_OK : USB_STORAGE_CONTROLLER_QUEUE_FULL;
}

usb_storage_controller_result_t usb_storage_controller_get_snapshot(
    usb_storage_controller_t *controller,
    usb_storage_controller_snapshot_t *snapshot)
{
    if (controller == NULL || snapshot == NULL) return USB_STORAGE_CONTROLLER_INVALID_ARGUMENT;
    (void)pthread_mutex_lock(&controller->mutex);
    *snapshot = (usb_storage_controller_snapshot_t) {
        .state = controller->state,
        .active_role = controller->active_role,
        .operation = controller->state.operation,
        .generation = controller->state.generation,
        .media_present = controller->media_present,
        .mounted = controller->mounted,
        .backend_active = controller->backend_active,
        .backend_stopping = controller->backend_stopping,
        .stop_requested = controller->stop_requested,
        .close_requested = controller->close_requested,
        .worker_busy = controller->worker_busy,
    };
    (void)pthread_mutex_unlock(&controller->mutex);
    return USB_STORAGE_CONTROLLER_OK;
}

usb_storage_controller_result_t usb_storage_controller_destroy(
    usb_storage_controller_t *controller)
{
    if (controller == NULL) return USB_STORAGE_CONTROLLER_INVALID_ARGUMENT;
    (void)pthread_mutex_lock(&controller->mutex);
    if (!state_is_idle_locked(controller) || controller->count != 0U || controller->worker_busy ||
        controller->callbacks_in_flight != 0U || controller->queue_overflow) {
        (void)pthread_mutex_unlock(&controller->mutex);
        return USB_STORAGE_CONTROLLER_BUSY;
    }
    ESP_LOGI(USB_STORAGE_TAG, "destroy generation=%u result=ok",
             (unsigned int)controller->state.generation);
    controller->accepting = false;
    controller->shutting_down = true;
    (void)pthread_cond_signal(&controller->work_condition);
    (void)pthread_mutex_unlock(&controller->mutex);
    if (controller->worker_started) (void)pthread_join(controller->worker, NULL);
    (void)pthread_cond_destroy(&controller->work_condition);
    (void)pthread_mutex_destroy(&controller->mutex);
    free(controller->commands);
    free(controller);
    return USB_STORAGE_CONTROLLER_OK;
}

const char *usb_storage_controller_result_name(usb_storage_controller_result_t result)
{
    switch (result) {
    case USB_STORAGE_CONTROLLER_OK:
        return "ok";
    case USB_STORAGE_CONTROLLER_INVALID_ARGUMENT:
        return "invalid-argument";
    case USB_STORAGE_CONTROLLER_BUSY:
        return "busy";
    case USB_STORAGE_CONTROLLER_BACKEND_ERROR:
        return "backend-error";
    case USB_STORAGE_CONTROLLER_STALE_EVENT:
        return "stale-event";
    case USB_STORAGE_CONTROLLER_QUEUE_FULL:
        return "queue-full";
    case USB_STORAGE_CONTROLLER_NOT_READY:
        return "not-ready";
    default:
        return "unknown";
    }
}
