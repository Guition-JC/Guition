#include "usb_msc_device.h"

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "diskio_impl.h"
#include "diskio_sdmmc.h"
#include "esp_board_manager.h"
#include "esp_heap_caps.h"
#include "esp_board_manager_defs.h"
#include "esp_board_manager_includes.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "ff.h"
#include "freertos/semphr.h"
#include "tinyusb.h"
#include "tinyusb_default_config.h"
#include "vfs_fat_internal.h"
#include "dev_fs_fat.h"

/** Acquire SD storage before installing USB MSC; reverse the sequence during shutdown. */

static const char *TAG = "USBMSC_Device";

typedef struct usb_msc_device_context {
    SemaphoreHandle_t lock;
    SemaphoreHandle_t wake;
    usb_storage_state_t state;
    usb_msc_device_config_t config;
    dev_fs_fat_handle_t *sd_handle;
    const dev_fs_fat_config_t *sd_config;
    tinyusb_msc_storage_handle_t storage;
    bool sd_device_acquired;
    bool sd_vfs_detached;
    bool tinyusb_installed;
    bool tinyusb_core_installed;
    bool attached;
    bool mount_transition;
    bool stopping;
    uint32_t active_operations;
    esp_err_t last_error;
    usb_msc_device_resource_state_t resource_state;
} usb_msc_device_context_t;

static void record_first_error(esp_err_t *first_error, esp_err_t error)
{
    if (*first_error == ESP_OK && error != ESP_OK) {
        *first_error = error;
    }
}

static bool valid_operation(usb_storage_operation_t operation)
{
    return operation == USB_STORAGE_OPERATION_READ ||
           operation == USB_STORAGE_OPERATION_WRITE ||
           operation == USB_STORAGE_OPERATION_MOUNT ||
           operation == USB_STORAGE_OPERATION_UNMOUNT ||
           operation == USB_STORAGE_OPERATION_FORMAT;
}

static bool generation_is_stale(usb_msc_device_context_t *ctx, uint32_t generation)
{
    if (generation == 0U || generation != ctx->state.generation) {
        const usb_storage_error_t error = USB_STORAGE_ERROR_STALE_GENERATION;
        ctx->state.last_error = error;
        return true;
    }
    return false;
}

static void signal_wake(usb_msc_device_context_t *ctx)
{
    if (ctx->wake != NULL) {
        (void)xSemaphoreGive(ctx->wake);
    }
}

static void emit_event(usb_msc_device_context_t *ctx,
                       usb_msc_device_event_kind_t kind,
                       tinyusb_msc_mount_point_t mount_point,
                       esp_err_t error)
{
    usb_msc_device_event_cb_t callback;
    usb_msc_device_event_t event;

    xSemaphoreTake(ctx->lock, portMAX_DELAY);
    callback = ctx->config.event_callback;
    event = (usb_msc_device_event_t) {
        .kind = kind,
        .lifecycle = ctx->state.lifecycle,
        .role = ctx->state.role,
        .generation = ctx->state.generation,
        .attached = ctx->attached,
        .storage_idle = !ctx->mount_transition &&
                        (!ctx->attached || mount_point == TINYUSB_MSC_STORAGE_MOUNT_USB),
        .mount_point = mount_point,
        .error = error,
    };
    xSemaphoreGive(ctx->lock);

    ESP_LOGI(TAG, "event kind=%d role=device generation=%u attached=%d mount=%d error=%s",
             (int)kind, (unsigned int)event.generation, event.attached, (int)mount_point,
             esp_err_to_name(error));
    if (callback != NULL) {
        callback(&event, ctx->config.callback_arg);
    }
}

static esp_err_t apply_state(usb_msc_device_context_t *ctx,
                             usb_storage_event_kind_t kind,
                             usb_storage_operation_t operation,
                             uint32_t generation,
                             usb_storage_error_t error)
{
    const usb_storage_event_t event = {
        .kind = kind,
        .role = kind == USB_STORAGE_EVENT_START ? USB_STORAGE_ROLE_DEVICE : USB_STORAGE_ROLE_NONE,
        .operation = operation,
        .generation = generation,
        .error = error,
    };
    xSemaphoreTake(ctx->lock, portMAX_DELAY);
    const usb_storage_error_t state_error = usb_storage_apply(&ctx->state, &event);
    xSemaphoreGive(ctx->lock);
    ESP_LOGI(TAG, "state-event role=device generation=%u kind=%d operation=%d result=%s",
             (unsigned int)ctx->state.generation, (int)kind, (int)operation,
             state_error == USB_STORAGE_ERROR_NONE ? "ESP_OK" : usb_storage_error_name(state_error));
    if (state_error != USB_STORAGE_ERROR_NONE) {
        ESP_LOGE(TAG, "state transition failed: %s", usb_storage_error_name(state_error));
        return ESP_ERR_INVALID_STATE;
    }
    return ESP_OK;
}

static esp_err_t detach_sd_vfs(usb_msc_device_context_t *ctx)
{
    if (ctx->sd_handle == NULL || ctx->sd_handle->card == NULL || ctx->sd_vfs_detached) {
        return ESP_OK;
    }

    const BYTE pdrv = ff_diskio_get_pdrv_card(ctx->sd_handle->card);
    if (pdrv == 0xff) {
        return ESP_ERR_INVALID_STATE;
    }
    char drive[3] = {(char)('0' + pdrv), ':', '\0'};
    if (f_mount(NULL, drive, 0) != FR_OK) {
        return ESP_FAIL;
    }
    ff_diskio_unregister(pdrv);
    const esp_err_t ret = esp_vfs_fat_unregister_path(USB_MSC_DEVICE_SD_MOUNT_PATH);
    if (ret != ESP_OK) {
        return ret;
    }
    ctx->sd_vfs_detached = true;
    ctx->resource_state = USB_MSC_DEVICE_RESOURCE_SD_UNMOUNTED;
    return ESP_OK;
}

static esp_err_t remount_sd_vfs(usb_msc_device_context_t *ctx)
{
    if (ctx->sd_handle == NULL || ctx->sd_handle->card == NULL || !ctx->sd_vfs_detached) {
        return ESP_OK;
    }

    const esp_vfs_fat_mount_config_t mount_config = {
        .format_if_mount_failed = false,
        .max_files = (int)ctx->sd_config->vfs_config.max_files,
        .allocation_unit_size = ctx->sd_config->vfs_config.allocation_unit_size,
    };
    const esp_err_t ret = esp_vfs_fat_mount_initialized(ctx->sd_handle->card,
                                                         USB_MSC_DEVICE_SD_MOUNT_PATH,
                                                         &mount_config);
    if (ret == ESP_OK) {
        ctx->sd_vfs_detached = false;
    }
    if (ret == ESP_OK) {
        ctx->resource_state = USB_MSC_DEVICE_RESOURCE_SD_ACQUIRED;
    }
    return ret;
}

static esp_err_t acquire_sd_device(usb_msc_device_context_t *ctx)
{
    esp_err_t ret = esp_board_manager_init_device_by_name(USB_MSC_DEVICE_SD_DEVICE_NAME);
    if (ret != ESP_OK) {
        return ret;
    }
    ctx->sd_device_acquired = true;

    void *device_handle = NULL;
    ret = esp_board_manager_get_device_handle(USB_MSC_DEVICE_SD_DEVICE_NAME, &device_handle);
    if (ret != ESP_OK || device_handle == NULL) {
        return ret != ESP_OK ? ret : ESP_ERR_INVALID_STATE;
    }
    ctx->sd_handle = (dev_fs_fat_handle_t *)device_handle;

    void *device_config = NULL;
    ret = esp_board_manager_get_device_config(USB_MSC_DEVICE_SD_DEVICE_NAME, &device_config);
    if (ret != ESP_OK || device_config == NULL) {
        return ret != ESP_OK ? ret : ESP_ERR_INVALID_STATE;
    }
    ctx->sd_config = (const dev_fs_fat_config_t *)device_config;
    ctx->resource_state = USB_MSC_DEVICE_RESOURCE_SD_ACQUIRED;
    if (ctx->sd_config->mount_point == NULL ||
        strcmp(ctx->sd_config->mount_point, USB_MSC_DEVICE_SD_MOUNT_PATH) != 0) {
        return ESP_ERR_INVALID_STATE;
    }
    return ESP_OK;
}

static esp_err_t create_msc(usb_msc_device_context_t *ctx)
{
    const tinyusb_msc_storage_config_t storage_config = {
        .medium.card = ctx->sd_handle->card,
        .fat_fs = {
            .base_path = (char *)USB_MSC_DEVICE_SD_MOUNT_PATH,
            .config = {
                .format_if_mount_failed = false,
                .max_files = (int)ctx->sd_config->vfs_config.max_files,
                .allocation_unit_size = ctx->sd_config->vfs_config.allocation_unit_size,
            },
            .do_not_format = true,
        },
        .mount_point = TINYUSB_MSC_STORAGE_MOUNT_USB,
    };
    ESP_LOGI(TAG, "MSC DMA heap before storage creation: free=%u largest=%u",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_DMA),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DMA));
    const esp_err_t ret = tinyusb_msc_new_storage_sdmmc(&storage_config, &ctx->storage);
    if (ret == ESP_OK) {
        ctx->resource_state = USB_MSC_DEVICE_RESOURCE_MSC_STORAGE;
    }
    return ret;
}

static esp_err_t cleanup_start_failure(usb_msc_device_context_t *ctx, esp_err_t first_error)
{
    esp_err_t cleanup_error;

    if (ctx->tinyusb_core_installed) {
        cleanup_error = tinyusb_driver_uninstall();
        record_first_error(&first_error, cleanup_error);
        if (cleanup_error == ESP_OK) {
            ctx->tinyusb_core_installed = false;
        }
    }
    if (ctx->storage != NULL) {
        cleanup_error = tinyusb_msc_delete_storage(ctx->storage);
        record_first_error(&first_error, cleanup_error);
        if (cleanup_error == ESP_OK) {
            ctx->storage = NULL;
        }
    }
    if (ctx->tinyusb_installed) {
        cleanup_error = tinyusb_msc_uninstall_driver();
        record_first_error(&first_error, cleanup_error);
        if (cleanup_error == ESP_OK) {
            ctx->tinyusb_installed = false;
        }
    }
    cleanup_error = remount_sd_vfs(ctx);
    record_first_error(&first_error, cleanup_error);
    if (ctx->sd_device_acquired) {
        cleanup_error = esp_board_manager_deinit_device_by_name(USB_MSC_DEVICE_SD_DEVICE_NAME);
        record_first_error(&first_error, cleanup_error);
        if (cleanup_error == ESP_OK) {
            ctx->sd_device_acquired = false;
        }
    }
    ctx->sd_handle = NULL;
    ctx->sd_config = NULL;
    ctx->resource_state = first_error == ESP_OK ? USB_MSC_DEVICE_RESOURCE_IDLE : USB_MSC_DEVICE_RESOURCE_ERROR;
    return first_error;
}

static bool storage_is_idle(usb_msc_device_context_t *ctx,
                            tinyusb_msc_mount_point_t *mount_point)
{
    tinyusb_msc_mount_point_t current_mount = TINYUSB_MSC_STORAGE_MOUNT_USB;
    if (ctx->storage != NULL && tinyusb_msc_get_storage_mount_point(ctx->storage, &current_mount) != ESP_OK) {
        return false;
    }
    xSemaphoreTake(ctx->lock, portMAX_DELAY);
    const bool idle = !ctx->mount_transition &&
                      (!ctx->attached || current_mount == TINYUSB_MSC_STORAGE_MOUNT_USB);
    xSemaphoreGive(ctx->lock);
    if (mount_point != NULL) {
        *mount_point = current_mount;
    }
    return idle;
}

static esp_err_t wait_for_idle_or_eject(usb_msc_device_context_t *ctx)
{
    const TickType_t start = xTaskGetTickCount();
    tinyusb_msc_mount_point_t mount_point = TINYUSB_MSC_STORAGE_MOUNT_USB;

    while (!storage_is_idle(ctx, &mount_point)) {
        const TickType_t elapsed = xTaskGetTickCount() - start;
        if (elapsed >= ctx->config.wait_timeout) {
            return ESP_ERR_TIMEOUT;
        }
        const TickType_t remaining = ctx->config.wait_timeout - elapsed;
        (void)xSemaphoreTake(ctx->wake, remaining);
    }
    emit_event(ctx, USB_MSC_DEVICE_EVENT_EJECTED, mount_point, ESP_OK);
    return ESP_OK;
}

static esp_err_t cleanup_stop(usb_msc_device_context_t *ctx, esp_err_t first_error)
{
    esp_err_t cleanup_error;

    if (ctx->tinyusb_core_installed) {
        cleanup_error = tinyusb_driver_uninstall();
        record_first_error(&first_error, cleanup_error);
        if (cleanup_error == ESP_OK) {
            ctx->tinyusb_core_installed = false;
        }
    }
    if (ctx->storage != NULL) {
        cleanup_error = tinyusb_msc_delete_storage(ctx->storage);
        record_first_error(&first_error, cleanup_error);
        if (cleanup_error == ESP_OK) {
            ctx->storage = NULL;
        }
    }
    if (ctx->tinyusb_installed) {
        cleanup_error = tinyusb_msc_uninstall_driver();
        record_first_error(&first_error, cleanup_error);
        if (cleanup_error == ESP_OK) {
            ctx->tinyusb_installed = false;
        }
    }
    cleanup_error = remount_sd_vfs(ctx);
    record_first_error(&first_error, cleanup_error);
    if (ctx->sd_device_acquired) {
        cleanup_error = esp_board_manager_deinit_device_by_name(USB_MSC_DEVICE_SD_DEVICE_NAME);
        record_first_error(&first_error, cleanup_error);
        if (cleanup_error == ESP_OK) {
            ctx->sd_device_acquired = false;
        }
    }
    ctx->sd_handle = NULL;
    ctx->sd_config = NULL;
    ctx->resource_state = first_error == ESP_OK ? USB_MSC_DEVICE_RESOURCE_IDLE : USB_MSC_DEVICE_RESOURCE_ERROR;
    return first_error;
}

static void tinyusb_msc_callback(tinyusb_msc_storage_handle_t storage,
                                 tinyusb_msc_event_t *event,
                                 void *arg)
{
    usb_msc_device_context_t *ctx = (usb_msc_device_context_t *)arg;
    (void)storage;
    if (ctx == NULL || event == NULL) {
        ESP_LOGW(TAG, "tinyusb-event dropped context=%p event=%p", (void *)ctx, (void *)event);
        return;
    }

    ESP_LOGI(TAG, "tinyusb-event id=%d mount=%d", (int)event->id, (int)event->mount_point);

    xSemaphoreTake(ctx->lock, portMAX_DELAY);
    ctx->mount_transition = event->id == TINYUSB_MSC_EVENT_MOUNT_START;
    xSemaphoreGive(ctx->lock);
    signal_wake(ctx);

    usb_msc_device_event_kind_t kind = USB_MSC_DEVICE_EVENT_MOUNT_FAILED;
    if (event->id == TINYUSB_MSC_EVENT_MOUNT_START) {
        kind = USB_MSC_DEVICE_EVENT_MOUNT_START;
    } else if (event->id == TINYUSB_MSC_EVENT_MOUNT_COMPLETE) {
        kind = event->mount_point == TINYUSB_MSC_STORAGE_MOUNT_USB
                   ? USB_MSC_DEVICE_EVENT_EJECTED
                   : USB_MSC_DEVICE_EVENT_MOUNT_COMPLETE;
    }
    emit_event(ctx, kind, event->mount_point,
               event->id == TINYUSB_MSC_EVENT_MOUNT_FAILED ? ESP_FAIL : ESP_OK);
}

esp_err_t usb_msc_device_init(const usb_msc_device_config_t *config,
                              usb_msc_device_handle_t *handle)
{
    ESP_LOGI(TAG, "init role=device");
    if (handle == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (*handle != NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    const usb_msc_device_config_t defaults = USB_MSC_DEVICE_CONFIG_DEFAULT();
    const usb_msc_device_config_t *requested = config != NULL ? config : &defaults;
    if (requested->wait_timeout == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    usb_msc_device_context_t *ctx = calloc(1, sizeof(*ctx));
    if (ctx == NULL) {
        return ESP_ERR_NO_MEM;
    }
    ctx->lock = xSemaphoreCreateMutex();
    ctx->wake = xSemaphoreCreateBinary();
    if (ctx->lock == NULL || ctx->wake == NULL) {
        if (ctx->wake != NULL) {
            vSemaphoreDelete(ctx->wake);
        }
        if (ctx->lock != NULL) {
            vSemaphoreDelete(ctx->lock);
        }
        free(ctx);
        return ESP_ERR_NO_MEM;
    }
    ctx->config = *requested;
    usb_storage_state_init(&ctx->state);
    ctx->resource_state = USB_MSC_DEVICE_RESOURCE_IDLE;
    *handle = ctx;
    ESP_LOGI(TAG, "init-result role=device generation=%u result=ESP_OK",
             (unsigned int)ctx->state.generation);
    return ESP_OK;
}

esp_err_t usb_msc_device_deinit(usb_msc_device_handle_t handle)
{
    if (handle == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (handle->storage != NULL || handle->tinyusb_installed ||
        handle->tinyusb_core_installed || handle->sd_device_acquired) {
        return ESP_ERR_INVALID_STATE;
    }
    vSemaphoreDelete(handle->wake);
    vSemaphoreDelete(handle->lock);
    free(handle);
    return ESP_OK;
}

esp_err_t usb_msc_device_start(usb_msc_device_handle_t handle)
{
    ESP_LOGI(TAG, "start role=device generation=%u",
             handle == NULL ? 0U : (unsigned int)handle->state.generation);
    if (handle == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    xSemaphoreTake(handle->lock, portMAX_DELAY);
    if (handle->stopping || handle->storage != NULL ||
        handle->state.lifecycle != USB_STORAGE_LIFECYCLE_IDLE) {
        xSemaphoreGive(handle->lock);
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreGive(handle->lock);

    esp_err_t ret = apply_state(handle, USB_STORAGE_EVENT_START,
                                USB_STORAGE_OPERATION_NONE, 0U,
                                USB_STORAGE_ERROR_NONE);
    if (ret != ESP_OK) {
        return ret;
    }
    emit_event(handle, USB_MSC_DEVICE_EVENT_STATE,
               TINYUSB_MSC_STORAGE_MOUNT_USB, ESP_OK);

    if (handle->config.quiesce != NULL) {
        ret = handle->config.quiesce(handle->config.callback_arg);
        if (ret != ESP_OK) {
            goto fail;
        }
    }
    if (handle->config.flush != NULL) {
        ret = handle->config.flush(handle->config.callback_arg);
        if (ret != ESP_OK) {
            goto fail;
        }
    }
    ret = acquire_sd_device(handle);
    if (ret != ESP_OK) {
        goto fail;
    }
    ret = detach_sd_vfs(handle);
    if (ret != ESP_OK) {
        goto fail;
    }
    const tinyusb_msc_driver_config_t driver_config = {
        .user_flags = {.auto_mount_off = 1},
        .callback = tinyusb_msc_callback,
        .callback_arg = handle,
    };
    ret = tinyusb_msc_install_driver(&driver_config);
    if (ret != ESP_OK) {
        goto fail;
    }
    handle->tinyusb_installed = true;
    handle->resource_state = USB_MSC_DEVICE_RESOURCE_TINYUSB;
    ret = create_msc(handle);
    if (ret != ESP_OK) {
        goto fail;
    }
    tinyusb_config_t tusb_cfg = TINYUSB_DEFAULT_CONFIG();
    ret = tinyusb_driver_install(&tusb_cfg);
    if (ret != ESP_OK) {
        goto fail;
    }
    handle->tinyusb_core_installed = true;
    ret = apply_state(handle, USB_STORAGE_EVENT_START_COMPLETE,
                      USB_STORAGE_OPERATION_NONE, handle->state.generation,
                      USB_STORAGE_ERROR_NONE);
    if (ret != ESP_OK) {
        goto fail;
    }
    emit_event(handle, USB_MSC_DEVICE_EVENT_STATE,
               TINYUSB_MSC_STORAGE_MOUNT_USB, ESP_OK);
    ESP_LOGI(TAG, "start-result role=device generation=%u result=ESP_OK",
             (unsigned int)handle->state.generation);
    return ESP_OK;

fail:
    handle->last_error = ret;
    ret = cleanup_start_failure(handle, ret);
    (void)apply_state(handle, USB_STORAGE_EVENT_FAIL,
                      USB_STORAGE_OPERATION_NONE, handle->state.generation,
                      USB_STORAGE_ERROR_IO);
    (void)apply_state(handle, USB_STORAGE_EVENT_RECOVER,
                      USB_STORAGE_OPERATION_NONE, handle->state.generation,
                      USB_STORAGE_ERROR_NONE);
    emit_event(handle, USB_MSC_DEVICE_EVENT_ERROR,
               TINYUSB_MSC_STORAGE_MOUNT_USB, ret);
    ESP_LOGE(TAG, "start-result role=device generation=%u result=%s",
             (unsigned int)handle->state.generation, esp_err_to_name(ret));
    return ret;
}

esp_err_t usb_msc_device_stop(usb_msc_device_handle_t handle)
{
    ESP_LOGI(TAG, "stop role=device generation=%u",
             handle == NULL ? 0U : (unsigned int)handle->state.generation);
    if (handle == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    xSemaphoreTake(handle->lock, portMAX_DELAY);
    if (handle->storage == NULL) {
        xSemaphoreGive(handle->lock);
        return ESP_OK;
    }
    handle->stopping = true;
    handle->resource_state = USB_MSC_DEVICE_RESOURCE_STOPPING;
    xSemaphoreGive(handle->lock);

    const TickType_t start = xTaskGetTickCount();
    while (true) {
        xSemaphoreTake(handle->lock, portMAX_DELAY);
        const uint32_t active_operations = handle->active_operations;
        xSemaphoreGive(handle->lock);
        if (active_operations == 0U) {
            break;
        }
        const TickType_t elapsed = xTaskGetTickCount() - start;
        if (elapsed >= handle->config.wait_timeout) {
            return ESP_ERR_TIMEOUT;
        }
        (void)xSemaphoreTake(handle->wake, handle->config.wait_timeout - elapsed);
    }

    esp_err_t ret = apply_state(handle, USB_STORAGE_EVENT_STOP,
                                USB_STORAGE_OPERATION_NONE, handle->state.generation,
                                USB_STORAGE_ERROR_NONE);
    if (ret != ESP_OK) {
        return ret;
    }
    emit_event(handle, USB_MSC_DEVICE_EVENT_STATE,
               TINYUSB_MSC_STORAGE_MOUNT_USB, ESP_OK);

    if (handle->storage != NULL) {
        tinyusb_msc_mount_point_t mount_point = TINYUSB_MSC_STORAGE_MOUNT_USB;
        ret = tinyusb_msc_get_storage_mount_point(handle->storage, &mount_point);
        if (ret == ESP_OK && mount_point == TINYUSB_MSC_STORAGE_MOUNT_APP) {
            ret = tinyusb_msc_set_storage_mount_point(handle->storage,
                                                       TINYUSB_MSC_STORAGE_MOUNT_USB);
        }
    }
    if (ret == ESP_OK) {
        ret = wait_for_idle_or_eject(handle);
    }
    if (ret != ESP_OK) {
        handle->last_error = ret;
        emit_event(handle, USB_MSC_DEVICE_EVENT_ERROR,
                   TINYUSB_MSC_STORAGE_MOUNT_USB, ret);
        return ret;
    }
    ret = cleanup_stop(handle, ESP_OK);
    if (ret != ESP_OK) {
        handle->last_error = ret;
        emit_event(handle, USB_MSC_DEVICE_EVENT_ERROR,
                   TINYUSB_MSC_STORAGE_MOUNT_USB, ret);
        return ret;
    }
    handle->stopping = false;
    ret = apply_state(handle, USB_STORAGE_EVENT_STOP_COMPLETE,
                      USB_STORAGE_OPERATION_NONE, handle->state.generation,
                      USB_STORAGE_ERROR_NONE);
    if (ret != ESP_OK) {
        return ret;
    }
    emit_event(handle, USB_MSC_DEVICE_EVENT_STATE,
               TINYUSB_MSC_STORAGE_MOUNT_APP, ESP_OK);
    ESP_LOGI(TAG, "stop-result role=device generation=%u result=ESP_OK",
             (unsigned int)handle->state.generation);
    return ESP_OK;
}

esp_err_t usb_msc_device_begin_operation(usb_msc_device_handle_t handle,
                                          usb_storage_operation_t operation,
                                          uint32_t generation)
{
    ESP_LOGI(TAG, "operation role=device generation=%u operation=%d",
             handle == NULL ? 0U : (unsigned int)generation, (int)operation);
    if (handle == NULL || !valid_operation(operation)) {
        return ESP_ERR_INVALID_ARG;
    }
    xSemaphoreTake(handle->lock, portMAX_DELAY);
    if (handle->stopping || handle->storage == NULL) {
        xSemaphoreGive(handle->lock);
        return ESP_ERR_INVALID_STATE;
    }
    if (generation_is_stale(handle, generation)) {
        xSemaphoreGive(handle->lock);
        return ESP_ERR_INVALID_STATE;
    }
    const usb_storage_event_t event = {
        .kind = USB_STORAGE_EVENT_OPERATION_BEGIN,
        .role = USB_STORAGE_ROLE_NONE,
        .operation = operation,
        .generation = generation,
        .error = USB_STORAGE_ERROR_NONE,
    };
    const usb_storage_error_t state_error = usb_storage_apply(&handle->state, &event);
    if (state_error == USB_STORAGE_ERROR_NONE) {
        handle->active_operations++;
    }
    xSemaphoreGive(handle->lock);
    return state_error == USB_STORAGE_ERROR_NONE ? ESP_OK : ESP_ERR_INVALID_STATE;
}

esp_err_t usb_msc_device_end_operation(usb_msc_device_handle_t handle,
                                        usb_storage_operation_t operation,
                                        uint32_t generation)
{
    if (handle == NULL || !valid_operation(operation)) {
        return ESP_ERR_INVALID_ARG;
    }
    xSemaphoreTake(handle->lock, portMAX_DELAY);
    if (handle->active_operations == 0U) {
        xSemaphoreGive(handle->lock);
        return ESP_ERR_INVALID_STATE;
    }
    if (generation_is_stale(handle, generation)) {
        xSemaphoreGive(handle->lock);
        return ESP_ERR_INVALID_STATE;
    }
    const usb_storage_event_t event = {
        .kind = USB_STORAGE_EVENT_OPERATION_END,
        .role = USB_STORAGE_ROLE_NONE,
        .operation = operation,
        .generation = generation,
        .error = USB_STORAGE_ERROR_NONE,
    };
    const usb_storage_error_t state_error = usb_storage_apply(&handle->state, &event);
    if (state_error == USB_STORAGE_ERROR_NONE) {
        handle->active_operations--;
    }
    xSemaphoreGive(handle->lock);
    if (state_error != USB_STORAGE_ERROR_NONE) {
        ESP_LOGE(TAG, "operation-end role=device generation=%u operation=%d result=invalid-state",
                 (unsigned int)generation, (int)operation);
        return ESP_ERR_INVALID_STATE;
    }
    signal_wake(handle);
    ESP_LOGI(TAG, "operation-end role=device generation=%u operation=%d result=ESP_OK",
             (unsigned int)generation, (int)operation);
    return ESP_OK;
}

esp_err_t usb_msc_device_get_generation(usb_msc_device_handle_t handle,
                                        uint32_t *generation)
{
    if (handle == NULL || generation == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    xSemaphoreTake(handle->lock, portMAX_DELAY);
    *generation = handle->state.generation;
    xSemaphoreGive(handle->lock);
    return ESP_OK;
}

esp_err_t usb_msc_device_get_state(usb_msc_device_handle_t handle,
                                   usb_storage_state_t *state)
{
    if (handle == NULL || state == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    xSemaphoreTake(handle->lock, portMAX_DELAY);
    *state = handle->state;
    xSemaphoreGive(handle->lock);
    return ESP_OK;
}

esp_err_t usb_msc_device_get_resource_state(usb_msc_device_handle_t handle,
                                            usb_msc_device_resource_state_t *resource_state)
{
    if (handle == NULL || resource_state == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    xSemaphoreTake(handle->lock, portMAX_DELAY);
    *resource_state = handle->resource_state;
    xSemaphoreGive(handle->lock);
    return ESP_OK;
}
