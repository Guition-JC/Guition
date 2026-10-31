#include "usb_msc_host.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "esp_vfs_fat.h"
#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "usb/msc_host.h"
#include "usb/msc_host_vfs.h"
#include "usb/usb_host.h"
#include "esp_log.h"

#define QUEUE_LENGTH 16
#define TASK_STACK 4096
static const char *const TAG = "USBMSC_Host";
static const char *const s_mount_path = "/usb";
static const char *const s_hello_world_path = USB_MSC_HOST_HELLO_WORLD_PATH;

typedef enum { MSG_START, MSG_STOP, MSG_PROBE, MSG_WRITE_HELLO_WORLD, MSG_SHUTDOWN, MSG_CONNECTED, MSG_DISCONNECTED } msg_id_t;
typedef struct {
    msg_id_t id;
    uint8_t address;
    msc_host_device_handle_t device;
} msg_t;

typedef struct {
    StaticSemaphore_t request_lock_buf, state_lock_buf, done_buf, msc_task_exited_buf, task_exited_buf;
    StaticQueue_t queue_buf;
    uint8_t queue_storage[QUEUE_LENGTH * sizeof(msg_t)];
    SemaphoreHandle_t request_lock, state_lock, done, msc_task_exited, task_exited;
    QueueHandle_t queue;
    TaskHandle_t task, msc_task;
    volatile bool initialized, callbacks_enabled;
    bool host_installed, msc_installed;
    msc_host_device_handle_t device;
    msc_host_vfs_handle_t vfs;
    uint8_t address;
    esp_err_t request_result;
    usb_msc_host_snapshot_t snapshot;
} host_ctx_t;

/** Host task and device handles with the current service snapshot. */
EXT_RAM_BSS_ATTR static host_ctx_t s_ctx;

static void msc_event_task(void *arg)
{
    host_ctx_t *ctx = (host_ctx_t *)arg;
    ESP_LOGI(TAG, "msc-event-task-start");
    esp_err_t result;
    while ((result = msc_host_handle_events(portMAX_DELAY)) == ESP_OK) {
    }
    ESP_LOGI(TAG, "msc-event-task-stop result=%s", esp_err_to_name(result));
    xSemaphoreGive(ctx->msc_task_exited);
    vTaskDelete(NULL);
}

static void host_event(const msc_host_event_t *event, void *arg)
{
    host_ctx_t *ctx = (host_ctx_t *)arg;
    if (ctx == NULL || event == NULL || !ctx->callbacks_enabled ||
        (event->event != MSC_DEVICE_CONNECTED && event->event != MSC_DEVICE_DISCONNECTED)) return;
    ESP_LOGI(TAG, "event=%s address=%u",
             event->event == MSC_DEVICE_CONNECTED ? "attach" : "disconnect",
             event->event == MSC_DEVICE_CONNECTED ? (unsigned int)event->device.address : 0U);
    msg_t msg = {.id = event->event == MSC_DEVICE_CONNECTED ? MSG_CONNECTED : MSG_DISCONNECTED};
    if (msg.id == MSG_CONNECTED) msg.address = event->device.address;
    else msg.device = event->device.handle;
    (void)xQueueSend(ctx->queue, &msg, 0);
}

/**
 * @brief Ledger unwind: VFS -> MSC device -> MSC client -> USB host daemon/library.
 * @param ctx Host context whose resources are released.
 * @return ESP_OK on success, otherwise the first release error.
 */
static esp_err_t release_device(host_ctx_t *ctx)
{
    esp_err_t first = ESP_OK, ret;
    if (ctx->vfs != NULL) {
        ret = msc_host_vfs_unregister(ctx->vfs);
        if (ret == ESP_OK) ctx->vfs = NULL;
        else if (first == ESP_OK) first = ret;
    }
    if (ctx->device != NULL && ctx->vfs == NULL) {
        ret = msc_host_uninstall_device(ctx->device);
        if (ret == ESP_OK) { ctx->device = NULL; ctx->address = 0; }
        else if (first == ESP_OK) first = ret;
    }
    ctx->snapshot.mounted = ctx->vfs != NULL;
    ctx->snapshot.media_present = ctx->device != NULL;
    if (ctx->vfs == NULL) {
        ctx->snapshot.capacity_valid = false;
        ctx->snapshot.total_bytes = 0;
        ctx->snapshot.free_bytes = 0;
    }
    return first;
}

static esp_err_t stop_backend(host_ctx_t *ctx)
{
    ESP_LOGI(TAG, "stop role=host generation=%u", (unsigned int)ctx->snapshot.storage.generation);
    esp_err_t first, ret;
    if (ctx->snapshot.storage.lifecycle == USB_STORAGE_LIFECYCLE_STARTING ||
        ctx->snapshot.storage.lifecycle == USB_STORAGE_LIFECYCLE_ACTIVE) {
        usb_storage_event_t begin = {.kind = USB_STORAGE_EVENT_STOP,
            .generation = ctx->snapshot.storage.generation};
        (void)usb_storage_apply(&ctx->snapshot.storage, &begin);
    }
    ctx->callbacks_enabled = false;
    first = release_device(ctx);
    if (ctx->msc_installed) {
        ret = msc_host_uninstall();
        ESP_LOGI(TAG, "msc-uninstall-result result=%s", esp_err_to_name(ret));
        if (ret == ESP_OK) ctx->msc_installed = false;
        else if (first == ESP_OK) first = ret;
    }
    if (ctx->msc_task != NULL && !ctx->msc_installed) {
        (void)xSemaphoreTake(ctx->msc_task_exited, portMAX_DELAY);
        ctx->msc_task = NULL;
    }
    if (ctx->host_installed) {
        uint32_t event_flags = 0;
        unsigned int consecutive_empty = 0U;
        ret = ESP_OK;
        for (unsigned int attempt = 0; attempt < 20U; attempt++) {
            event_flags = 0;
            ret = usb_host_lib_handle_events(pdMS_TO_TICKS(100), &event_flags);
            if (ret == ESP_ERR_TIMEOUT) {
                consecutive_empty++;
                if (consecutive_empty >= 3U) {
                    ret = ESP_OK;
                    break;
                }
                continue;
            }
            if (ret != ESP_OK) {
                break;
            }
            consecutive_empty = 0U;
            if ((event_flags & USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS) != 0U) {
                (void)usb_host_device_free_all();
            }
            if ((event_flags & USB_HOST_LIB_EVENT_FLAGS_ALL_FREE) != 0U) {
                break;
            }
        }
        if (ret != ESP_OK && first == ESP_OK) first = ret;
        vTaskDelay(pdMS_TO_TICKS(10));
        ret = usb_host_uninstall();
        if (ret == ESP_OK || ret == ESP_ERR_INVALID_STATE) ctx->host_installed = false;
        else if (first == ESP_OK) first = ret;
    }
    ctx->snapshot.host_installed = ctx->host_installed;
    ctx->snapshot.msc_installed = ctx->msc_installed;
    ctx->snapshot.callbacks_enabled = ctx->callbacks_enabled;
    if (ctx->snapshot.storage.lifecycle == USB_STORAGE_LIFECYCLE_STOPPING) {
        usb_storage_event_t done = {.kind = USB_STORAGE_EVENT_STOP_COMPLETE,
            .generation = ctx->snapshot.storage.generation};
        (void)usb_storage_apply(&ctx->snapshot.storage, &done);
    } else if (ctx->snapshot.storage.lifecycle == USB_STORAGE_LIFECYCLE_ERROR) {
        usb_storage_event_t recover = {.kind = USB_STORAGE_EVENT_RECOVER,
            .generation = ctx->snapshot.storage.generation};
        (void)usb_storage_apply(&ctx->snapshot.storage, &recover);
    }
    ESP_LOGI(TAG, "stop-result role=host generation=%u result=%s",
             (unsigned int)ctx->snapshot.storage.generation, esp_err_to_name(first));
    return first;
}

static void fail_state(host_ctx_t *ctx, esp_err_t error)
{
    ESP_LOGE(TAG, "error role=host generation=%u result=%s",
             (unsigned int)ctx->snapshot.storage.generation, esp_err_to_name(error));
    ctx->snapshot.last_error = (int32_t)error;
    if (ctx->snapshot.storage.lifecycle != USB_STORAGE_LIFECYCLE_IDLE &&
        ctx->snapshot.storage.lifecycle != USB_STORAGE_LIFECYCLE_ERROR) {
        usb_storage_event_t event = {.kind = USB_STORAGE_EVENT_FAIL,
            .generation = ctx->snapshot.storage.generation, .error = USB_STORAGE_ERROR_IO};
        (void)usb_storage_apply(&ctx->snapshot.storage, &event);
    }
}

static esp_err_t start_backend(host_ctx_t *ctx)
{
    ESP_LOGI(TAG, "start role=host generation=%u", (unsigned int)ctx->snapshot.storage.generation);
    usb_storage_event_t begin = {.kind = USB_STORAGE_EVENT_START, .role = USB_STORAGE_ROLE_HOST};
    if (ctx->snapshot.storage.lifecycle == USB_STORAGE_LIFECYCLE_ERROR) {
        usb_storage_event_t recover = {.kind = USB_STORAGE_EVENT_RECOVER,
            .generation = ctx->snapshot.storage.generation};
        (void)usb_storage_apply(&ctx->snapshot.storage, &recover);
    }
    esp_err_t ret = usb_storage_apply(&ctx->snapshot.storage, &begin);
    if (ret != USB_STORAGE_ERROR_NONE) return ESP_ERR_INVALID_STATE;
    ESP_LOGI(TAG, "host-heap-before-install dma_internal_free=%u dma_internal_largest=%u internal_free=%u",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    ret = usb_host_install(&(usb_host_config_t){.intr_flags = ESP_INTR_FLAG_LOWMED});
    if (ret != ESP_OK) goto fail;
    ctx->host_installed = true;
    ret = msc_host_install(&(msc_host_driver_config_t){
        .create_backround_task = false, .task_priority = 0, .stack_size = 0,
        .core_id = tskNO_AFFINITY, .callback = host_event, .callback_arg = ctx});
    if (ret != ESP_OK) goto fail;
    ctx->msc_installed = true;
    if (xTaskCreate(msc_event_task, "usb_msc_events", TASK_STACK, ctx, 5, &ctx->msc_task) != pdPASS) {
        ret = ESP_ERR_NO_MEM;
        goto fail;
    }
    ctx->callbacks_enabled = true;
    usb_storage_event_t done = {.kind = USB_STORAGE_EVENT_START_COMPLETE,
        .generation = ctx->snapshot.storage.generation};
    if (usb_storage_apply(&ctx->snapshot.storage, &done) != USB_STORAGE_ERROR_NONE) {
        ret = ESP_ERR_INVALID_STATE;
        goto fail;
    }
    ESP_LOGI(TAG, "start-result role=host generation=%u result=ESP_OK",
             (unsigned int)ctx->snapshot.storage.generation);
    return ESP_OK;
fail:
    ESP_LOGE(TAG, "start-result role=host generation=%u result=%s",
             (unsigned int)ctx->snapshot.storage.generation, esp_err_to_name(ret));
    ctx->callbacks_enabled = false;
    (void)stop_backend(ctx);
    fail_state(ctx, ret);
    return ret;
}

static esp_err_t mount_device(host_ctx_t *ctx, uint8_t address)
{
    ESP_LOGI(TAG, "probe-attach role=host generation=%u address=%u",
             (unsigned int)ctx->snapshot.storage.generation, (unsigned int)address);
    if (!ctx->callbacks_enabled || ctx->device != NULL) return ESP_ERR_INVALID_STATE;
    esp_err_t ret = msc_host_install_device(address, &ctx->device);
    if (ret != ESP_OK) return ret;
    ctx->address = address;
    esp_vfs_fat_mount_config_t mount = VFS_FAT_MOUNT_DEFAULT_CONFIG();
    mount.max_files = 3;
    mount.allocation_unit_size = 4096;
    mount.disk_status_check_enable = true;
    ret = msc_host_vfs_register(ctx->device, s_mount_path, &mount, &ctx->vfs);
    if (ret != ESP_OK) {
        ctx->snapshot.last_error = (int32_t)ret;
        (void)msc_host_uninstall_device(ctx->device);
        ctx->device = NULL;
        ctx->address = 0;
        return ret;
    }
    ctx->snapshot.media_present = true;
    ctx->snapshot.mounted = true;
    ESP_LOGI(TAG, "mount-result role=host generation=%u result=ESP_OK",
             (unsigned int)ctx->snapshot.storage.generation);
    return ESP_OK;
}

static bool host_storage_ready(const host_ctx_t *ctx)
{
    return ctx != NULL && ctx->snapshot.storage.lifecycle == USB_STORAGE_LIFECYCLE_ACTIVE &&
           ctx->snapshot.storage.role == USB_STORAGE_ROLE_HOST && ctx->device != NULL &&
           ctx->vfs != NULL;
}

static esp_err_t read_capacity_backend(host_ctx_t *ctx)
{
    if (!host_storage_ready(ctx)) {
        return ESP_ERR_INVALID_STATE;
    }

    FILE *file = fopen(s_hello_world_path, "rb");
    if (file == NULL) {
        ctx->snapshot.last_error = ESP_FAIL;
        ESP_LOGE(TAG, "read-result role=host generation=%u path=%s result=%s",
                 (unsigned int)ctx->snapshot.storage.generation, s_hello_world_path,
                 esp_err_to_name(ESP_FAIL));
        return ESP_FAIL;
    }
    char probe[sizeof("HelloWorld\n") - 1U];
    const size_t bytes_read = fread(probe, 1U, sizeof(probe), file);
    esp_err_t read_result = bytes_read == sizeof(probe) ? ESP_OK : ESP_FAIL;
    if (fclose(file) != 0) {
        read_result = ESP_FAIL;
    }
    if (read_result != ESP_OK) {
        ctx->snapshot.last_error = (int32_t)read_result;
        ESP_LOGE(TAG, "read-result role=host generation=%u path=%s result=%s",
                 (unsigned int)ctx->snapshot.storage.generation, s_hello_world_path,
                 esp_err_to_name(read_result));
        return read_result;
    }

    uint64_t total_bytes = 0;
    uint64_t free_bytes = 0;
    const esp_err_t info_result = esp_vfs_fat_info(USB_MSC_HOST_MOUNT_PATH,
                                                    &total_bytes, &free_bytes);
    if (info_result != ESP_OK) {
        ctx->snapshot.capacity_valid = false;
        ctx->snapshot.last_error = (int32_t)info_result;
        ESP_LOGE(TAG, "capacity-result role=host generation=%u result=%s",
                 (unsigned int)ctx->snapshot.storage.generation,
                 esp_err_to_name(info_result));
        return info_result;
    }
    ctx->snapshot.total_bytes = total_bytes;
    ctx->snapshot.free_bytes = free_bytes;
    ctx->snapshot.capacity_valid = true;
    ESP_LOGI(TAG, "read-result role=host generation=%u path=%s result=ESP_OK",
             (unsigned int)ctx->snapshot.storage.generation, s_hello_world_path);
    ESP_LOGI(TAG, "capacity-result role=host generation=%u total_bytes=%" PRIu64
             " free_bytes=%" PRIu64 " total_mib=%" PRIu64 " free_mib=%" PRIu64,
             (unsigned int)ctx->snapshot.storage.generation, ctx->snapshot.total_bytes,
             ctx->snapshot.free_bytes, ctx->snapshot.total_bytes / (1024U * 1024U),
             ctx->snapshot.free_bytes / (1024U * 1024U));
    return ESP_OK;
}

static esp_err_t write_hello_world_backend(host_ctx_t *ctx)
{
    if (!host_storage_ready(ctx)) {
        return ESP_ERR_INVALID_STATE;
    }
    time_t now = time(NULL);
    struct tm local_time;
    if (now == (time_t)-1 || localtime_r(&now, &local_time) == NULL) {
        return ESP_FAIL;
    }
    char timestamp[32];
    if (strftime(timestamp, sizeof(timestamp), "%Y-%m-%d %H:%M:%S", &local_time) == 0) {
        return ESP_FAIL;
    }
    FILE *file = fopen(s_hello_world_path, "wb");
    if (file == NULL) {
        ctx->snapshot.last_error = ESP_FAIL;
        return ESP_FAIL;
    }
    const int written = fprintf(file, "HelloWorld\nTime: %s\n", timestamp);
    esp_err_t ret = written < 0 || fflush(file) != 0 || fsync(fileno(file)) != 0
                        ? ESP_FAIL
                        : ESP_OK;
    if (fclose(file) != 0) {
        ret = ESP_FAIL;
    }
    if (ret != ESP_OK) {
        ctx->snapshot.last_error = (int32_t)ret;
    }
    ESP_LOGI(TAG, "write-result role=host generation=%u path=%s result=%s",
             (unsigned int)ctx->snapshot.storage.generation, s_hello_world_path,
             esp_err_to_name(ret));
    return ret;
}

static void process_msg(host_ctx_t *ctx, const msg_t *msg)
{
    ESP_LOGI(TAG, "worker-msg id=%d generation=%u", (int)msg->id,
             (unsigned int)ctx->snapshot.storage.generation);
    xSemaphoreTake(ctx->state_lock, portMAX_DELAY);
    switch (msg->id) {
    case MSG_START: ctx->request_result = start_backend(ctx); xSemaphoreGive(ctx->done); break;
    case MSG_STOP: ctx->request_result = stop_backend(ctx); xSemaphoreGive(ctx->done); break;
    case MSG_PROBE: ctx->request_result = read_capacity_backend(ctx); xSemaphoreGive(ctx->done); break;
    case MSG_WRITE_HELLO_WORLD: ctx->request_result = write_hello_world_backend(ctx); xSemaphoreGive(ctx->done); break;
    case MSG_SHUTDOWN: ctx->request_result = stop_backend(ctx); xSemaphoreGive(ctx->done); break;
    case MSG_CONNECTED: {
        if (!ctx->host_installed || !ctx->msc_installed) break;
        const esp_err_t result = mount_device(ctx, msg->address);
        ESP_LOGI(TAG, "attach-result role=host generation=%u result=%s",
                 (unsigned int)ctx->snapshot.storage.generation, esp_err_to_name(result));
        break;
    }
    case MSG_DISCONNECTED: {
        if (!ctx->host_installed || !ctx->msc_installed) break;
        const esp_err_t result = msg->device == ctx->device ? release_device(ctx) : ESP_ERR_INVALID_STATE;
        ESP_LOGI(TAG, "disconnect-result role=host generation=%u result=%s",
                 (unsigned int)ctx->snapshot.storage.generation, esp_err_to_name(result));
        break;
    }
    }
    ESP_LOGI(TAG, "worker-msg-done id=%d generation=%u", (int)msg->id,
             (unsigned int)ctx->snapshot.storage.generation);
    xSemaphoreGive(ctx->state_lock);
}

static void service_task(void *arg)
{
    host_ctx_t *ctx = (host_ctx_t *)arg;
    msg_t msg;
    bool shutdown = false;
    while (!shutdown) {
        if (ctx->host_installed) {
            uint32_t event_flags = 0;
            const esp_err_t event_result =
                usb_host_lib_handle_events(pdMS_TO_TICKS(10), &event_flags);
            if (event_result != ESP_OK && event_result != ESP_ERR_TIMEOUT && ctx->host_installed) {
                ESP_LOGW(TAG, "event-loop-result result=%s", esp_err_to_name(event_result));
            }
        }
        if (xQueueReceive(ctx->queue, &msg, ctx->host_installed ? 0 : portMAX_DELAY) == pdPASS) {
            process_msg(ctx, &msg);
            shutdown = msg.id == MSG_SHUTDOWN;
        }
    }
    ctx->callbacks_enabled = false;
    xSemaphoreGive(ctx->task_exited);
    vTaskDelete(NULL);
}

static esp_err_t request(msg_id_t id)
{
    host_ctx_t *ctx = &s_ctx;
    ESP_LOGI(TAG, "request id=%d", (int)id);
    if (!ctx->initialized) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(ctx->request_lock, portMAX_DELAY);
    (void)xSemaphoreTake(ctx->done, 0);
    if (xQueueSend(ctx->queue, &(msg_t){.id = id}, portMAX_DELAY) != pdPASS) {
        ESP_LOGE(TAG, "request-result id=%d result=ESP_ERR_TIMEOUT", (int)id);
        xSemaphoreGive(ctx->request_lock);
        return ESP_ERR_TIMEOUT;
    }
    (void)xSemaphoreTake(ctx->done, portMAX_DELAY);
    esp_err_t ret = ctx->request_result;
    ESP_LOGI(TAG, "request-result id=%d result=%s", (int)id, esp_err_to_name(ret));
    xSemaphoreGive(ctx->request_lock);
    return ret;
}

esp_err_t usb_msc_host_init(void)
{
    ESP_LOGI(TAG, "init");
    host_ctx_t *ctx = &s_ctx;
    if (ctx->initialized) return ESP_ERR_INVALID_STATE;
    ctx->request_lock = xSemaphoreCreateMutexStatic(&ctx->request_lock_buf);
    ctx->state_lock = xSemaphoreCreateMutexStatic(&ctx->state_lock_buf);
    ctx->done = xSemaphoreCreateBinaryStatic(&ctx->done_buf);
    ctx->msc_task_exited = xSemaphoreCreateBinaryStatic(&ctx->msc_task_exited_buf);
    ctx->task_exited = xSemaphoreCreateBinaryStatic(&ctx->task_exited_buf);
    ctx->queue = xQueueCreateStatic(QUEUE_LENGTH, sizeof(msg_t), ctx->queue_storage, &ctx->queue_buf);
    usb_storage_state_init(&ctx->snapshot.storage);
    ctx->initialized = true;
    if (xTaskCreate(service_task, "usb_msc_host", TASK_STACK, ctx, 5, &ctx->task) != pdPASS) {
        ctx->initialized = false;
        ESP_LOGE(TAG, "init-result result=ESP_ERR_NO_MEM");
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "init-result result=ESP_OK");
    return ESP_OK;
}

esp_err_t usb_msc_host_start(void) {
    ESP_LOGI(TAG, "start-request role=host");
    return request(MSG_START);
}
esp_err_t usb_msc_host_stop(void) {
    ESP_LOGI(TAG, "stop-request role=host");
    return request(MSG_STOP);
}
esp_err_t usb_msc_host_read_capacity(void)
{
    ESP_LOGI(TAG, "read-capacity-request role=host");
    return request(MSG_PROBE);
}

esp_err_t usb_msc_host_write_hello_world(void)
{
    ESP_LOGI(TAG, "write-hello-world-request role=host");
    return request(MSG_WRITE_HELLO_WORLD);
}

esp_err_t usb_msc_host_deinit(void)
{
    host_ctx_t *ctx = &s_ctx;
    if (!ctx->initialized) return ESP_ERR_INVALID_STATE;
    esp_err_t ret = request(MSG_SHUTDOWN);
    (void)xSemaphoreTake(ctx->task_exited, portMAX_DELAY);
    ctx->task = NULL;
    ctx->initialized = false;
    return ret;
}

esp_err_t usb_msc_host_get_snapshot(usb_msc_host_snapshot_t *snapshot)
{
    if (snapshot == NULL || !s_ctx.initialized) return ESP_ERR_INVALID_ARG;
    xSemaphoreTake(s_ctx.state_lock, portMAX_DELAY);
    *snapshot = s_ctx.snapshot;
    xSemaphoreGive(s_ctx.state_lock);
    return ESP_OK;
}
