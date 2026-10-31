/** SPDX-License-Identifier: GPL-2.0-only */
#include "time_service.h"

#include <string.h>

#include "rtc_time.h"
#include "time_settings.h"

#ifndef CONFIG_FACTORY_SNTP_SERVER
#define CONFIG_FACTORY_SNTP_SERVER "pool.ntp.org"
#endif

#ifndef TIME_SERVICE_HOST_TEST
#include "esp_log.h"
#include "esp_netif_sntp.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#endif

#define TIME_SERVICE_WORKER_STACK_SIZE 4096U
#define TIME_SERVICE_WORKER_PRIORITY 4U

#ifndef TIME_SERVICE_HOST_TEST
static const char *TAG = "time_service";
static SemaphoreHandle_t s_lock;
static TaskHandle_t s_worker;
#endif

static time_service_snapshot_t s_snapshot = {
    .sync_state = TIME_SYNC_UNSYNCHRONIZED,
    .rtc_state = RTC_STATE_UNAVAILABLE,
    .timezone_offset_minutes = TIME_SETTINGS_DEFAULT_OFFSET_MINUTES,
};
static struct timeval s_pending_time;
static bool s_initialized;

#ifdef TIME_SERVICE_HOST_TEST
static time_service_test_ops_t s_test_ops;
static bool s_test_ops_configured;
#define SERVICE_LOCK() ((void)0)
#define SERVICE_UNLOCK() ((void)0)
#else
#define SERVICE_LOCK() xSemaphoreTake(s_lock, portMAX_DELAY)
#define SERVICE_UNLOCK() xSemaphoreGive(s_lock)
#endif

static void record_first_error(esp_err_t error)
{
    if (error != ESP_OK && s_snapshot.init_error == ESP_OK) {
        s_snapshot.init_error = error;
    }
}

static void update_rtc_snapshot(esp_err_t operation_error)
{
    s_snapshot.rtc_error = operation_error;
#ifndef TIME_SERVICE_HOST_TEST
    rtc_time_result_t result = {0};
    if (rtc_time_get_result(&result) == ESP_OK) {
        s_snapshot.rtc_state = result.state;
    } else if (operation_error != ESP_OK) {
        s_snapshot.rtc_state = RTC_STATE_ERROR;
    }
#else
    s_snapshot.rtc_state = operation_error == ESP_OK ? RTC_STATE_HEALTHY : RTC_STATE_ERROR;
#endif
}

#ifndef TIME_SERVICE_HOST_TEST
static esp_err_t platform_timezone_load(int32_t *offset_minutes)
{
    return time_settings_load(offset_minutes);
}

static esp_err_t platform_timezone_apply(int32_t offset_minutes)
{
    return time_settings_apply(offset_minutes);
}

static esp_err_t platform_timezone_clear(void)
{
    return time_settings_clear();
}

static esp_err_t platform_rtc_init(void)
{
    return rtc_time_init();
}

static esp_err_t platform_rtc_reconcile(int64_t synchronized_epoch)
{
    return rtc_time_reconcile(synchronized_epoch);
}

/**
 * @brief Load the time source stored separately from timezone preferences.
 * @param [out] source Receives the stored or default time source.
 * @return ESP_OK on success, otherwise a storage error.
 */
static esp_err_t platform_time_source_load(time_source_t *source)
{
    *source = TIME_SOURCE_DEFAULT;
    nvs_handle_t handle = 0;
    esp_err_t ret = nvs_open("time_src", NVS_READONLY, &handle);
    if (ret == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_OK;
    }
    if (ret != ESP_OK) {
        return ret;
    }
    uint8_t stored = 0U;
    ret = nvs_get_u8(handle, "source", &stored);
    nvs_close(handle);
    if (ret == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_OK;
    }
    if (ret != ESP_OK) {
        return ret;
    }
    if (time_source_is_valid((time_source_t)stored)) {
        *source = (time_source_t)stored;
    }
    return ESP_OK;
}

static esp_err_t platform_time_source_save(time_source_t source)
{
    nvs_handle_t handle = 0;
    esp_err_t ret = nvs_open("time_src", NVS_READWRITE, &handle);
    if (ret != ESP_OK) {
        return ret;
    }
    ret = nvs_set_u8(handle, "source", (uint8_t)source);
    if (ret == ESP_OK) {
        ret = nvs_commit(handle);
    }
    nvs_close(handle);
    return ret;
}

static esp_err_t platform_rtc_read_now(int64_t *epoch)
{
    return rtc_time_read_now(epoch);
}

static esp_err_t platform_system_time_set(int64_t epoch)
{
    const struct timeval tv = {.tv_sec = (time_t)epoch, .tv_usec = 0};
    return settimeofday(&tv, NULL) == 0 ? ESP_OK : ESP_FAIL;
}
#else
static esp_err_t platform_timezone_load(int32_t *offset_minutes)
{
    return s_test_ops.timezone_load(s_test_ops.context, offset_minutes);
}

static esp_err_t platform_timezone_apply(int32_t offset_minutes)
{
    return s_test_ops.timezone_apply(s_test_ops.context, offset_minutes);
}

static esp_err_t platform_timezone_clear(void)
{
    return s_test_ops.timezone_clear(s_test_ops.context);
}

static esp_err_t platform_rtc_init(void)
{
    return s_test_ops.rtc_init(s_test_ops.context);
}

static esp_err_t platform_rtc_reconcile(int64_t synchronized_epoch)
{
    return s_test_ops.rtc_reconcile(s_test_ops.context, synchronized_epoch);
}

static esp_err_t platform_time_source_load(time_source_t *source)
{
    return s_test_ops.time_source_load(s_test_ops.context, source);
}

static esp_err_t platform_time_source_save(time_source_t source)
{
    return s_test_ops.time_source_save(s_test_ops.context, source);
}

static esp_err_t platform_rtc_read_now(int64_t *epoch)
{
    return s_test_ops.rtc_read_now(s_test_ops.context, epoch);
}

static esp_err_t platform_system_time_set(int64_t epoch)
{
    return s_test_ops.system_time_set(s_test_ops.context, epoch);
}
#endif

static void synchronization_callback(struct timeval *tv)
{
    if (tv == NULL || !s_initialized) {
        return;
    }

    SERVICE_LOCK();
    s_pending_time = *tv;
    s_snapshot.sync_pending = true;
    ++s_snapshot.sync_callbacks;
    SERVICE_UNLOCK();

#ifndef TIME_SERVICE_HOST_TEST
    if (s_worker != NULL) {
        xTaskNotifyGive(s_worker);
    }
#endif
}

static bool process_pending_synchronization(void)
{
    struct timeval synchronized_time = {0};

    SERVICE_LOCK();
    if (!s_snapshot.sync_pending) {
        SERVICE_UNLOCK();
        return false;
    }
    synchronized_time = s_pending_time;
    s_snapshot.sync_pending = false;
    ++s_snapshot.worker_runs;
    SERVICE_UNLOCK();

    const int64_t synchronized_epoch = (int64_t)synchronized_time.tv_sec;
    if (!time_policy_is_sane_system_epoch(synchronized_epoch)) {
        SERVICE_LOCK();
        s_snapshot.sync_state = TIME_SYNC_UNSYNCHRONIZED;
        s_snapshot.reconcile_error = ESP_ERR_INVALID_STATE;
        SERVICE_UNLOCK();
#ifndef TIME_SERVICE_HOST_TEST
        ESP_LOGW(TAG, "discarded invalid SNTP epoch: %lld", (long long)synchronized_epoch);
#endif
        return true;
    }

    const esp_err_t reconcile_error = platform_rtc_reconcile(synchronized_epoch);
    SERVICE_LOCK();
    s_snapshot.sync_state = TIME_SYNC_SYNCHRONIZED;
    s_snapshot.synchronized_epoch = synchronized_epoch;
    s_snapshot.last_sync = synchronized_time;
    s_snapshot.reconcile_error = reconcile_error;
    update_rtc_snapshot(reconcile_error);
    SERVICE_UNLOCK();

#ifndef TIME_SERVICE_HOST_TEST
    if (reconcile_error == ESP_OK) {
        ESP_LOGI(TAG, "SNTP synchronized at epoch %lld", (long long)synchronized_epoch);
    } else {
        ESP_LOGW(TAG, "SNTP synchronized; RTC reconcile failed: %s",
                 esp_err_to_name(reconcile_error));
    }
#endif
    return true;
}

#ifndef TIME_SERVICE_HOST_TEST
static void synchronization_worker(void *argument)
{
    (void)argument;
    for (;;) {
        (void)ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        (void)process_pending_synchronization();
    }
}
#endif

bool time_source_is_valid(time_source_t source)
{
    return source == TIME_SOURCE_INTERNAL || source == TIME_SOURCE_RTC;
}

/**
 * @brief Seed the system clock from the chip. Called at boot and whenever the user picks RTC, so
 * the whole UI (which reads time(NULL)) follows the chip.
 */
static esp_err_t apply_time_source(time_source_t source)
{
    if (source != TIME_SOURCE_RTC) {
        return ESP_OK;
    }

    int64_t epoch = 0;
    const esp_err_t read_error = platform_rtc_read_now(&epoch);
    if (read_error != ESP_OK || !time_policy_is_sane_system_epoch(epoch)) {
        SERVICE_LOCK();
        s_snapshot.rtc_error = read_error != ESP_OK ? read_error : ESP_ERR_INVALID_STATE;
        update_rtc_snapshot(s_snapshot.rtc_error);
        SERVICE_UNLOCK();
        return read_error != ESP_OK ? read_error : ESP_ERR_INVALID_STATE;
    }

    const esp_err_t set_error = platform_system_time_set(epoch);
    SERVICE_LOCK();
    s_snapshot.rtc_error = set_error;
    if (set_error == ESP_OK) {
        s_snapshot.sync_state = TIME_SYNC_SYNCHRONIZED;
        s_snapshot.synchronized_epoch = epoch;
    }
    update_rtc_snapshot(set_error);
    SERVICE_UNLOCK();

#ifndef TIME_SERVICE_HOST_TEST
    if (set_error == ESP_OK) {
        ESP_LOGI(TAG, "system clock seeded from RTC at epoch %lld", (long long)epoch);
    } else {
        ESP_LOGW(TAG, "RTC time source failed, keeping the internal clock: %s",
                 esp_err_to_name(set_error));
    }
#endif
    return set_error;
}

static esp_err_t initialize_time_source(void)
{
    time_source_t source = TIME_SOURCE_DEFAULT;
    const esp_err_t load_error = platform_time_source_load(&source);
    if (load_error != ESP_OK || !time_source_is_valid(source)) {
        source = TIME_SOURCE_DEFAULT;
    }
    SERVICE_LOCK();
    s_snapshot.time_source = source;
    SERVICE_UNLOCK();
    return apply_time_source(source);
}

static esp_err_t initialize_timezone(void)
{
    int32_t offset_minutes = TIME_SETTINGS_DEFAULT_OFFSET_MINUTES;
    esp_err_t error = platform_timezone_load(&offset_minutes);
    if (error == ESP_ERR_INVALID_RESPONSE) {
        const esp_err_t clear_error = platform_timezone_clear();
        if (clear_error != ESP_OK) {
            error = clear_error;
        } else {
            error = ESP_OK;
        }
        offset_minutes = TIME_SETTINGS_DEFAULT_OFFSET_MINUTES;
    } else if (error != ESP_OK) {
        offset_minutes = TIME_SETTINGS_DEFAULT_OFFSET_MINUTES;
    }

    const esp_err_t apply_error = platform_timezone_apply(offset_minutes);
    SERVICE_LOCK();
    s_snapshot.timezone_offset_minutes = offset_minutes;
    s_snapshot.timezone_error = apply_error != ESP_OK ? apply_error : error;
    record_first_error(s_snapshot.timezone_error);
    SERVICE_UNLOCK();
    return s_snapshot.timezone_error;
}

static esp_err_t initialize_rtc(void)
{
    const esp_err_t error = platform_rtc_init();
    SERVICE_LOCK();
    update_rtc_snapshot(error);
    record_first_error(error);
    SERVICE_UNLOCK();
    return error;
}

static esp_err_t initialize_sntp(void)
{
#ifdef TIME_SERVICE_HOST_TEST
    return s_test_ops.sntp_init(s_test_ops.context, CONFIG_FACTORY_SNTP_SERVER, false, false, true,
                                synchronization_callback);
#else
    esp_sntp_config_t config = ESP_NETIF_SNTP_DEFAULT_CONFIG(CONFIG_FACTORY_SNTP_SERVER);
    config.start = false;
    config.smooth_sync = false;
    config.wait_for_sync = true;
    config.sync_cb = synchronization_callback;
    return esp_netif_sntp_init(&config);
#endif
}

static bool create_worker(void)
{
#ifdef TIME_SERVICE_HOST_TEST
    return s_test_ops.worker_create(s_test_ops.context);
#else
    return xTaskCreate(synchronization_worker, "time_sync", TIME_SERVICE_WORKER_STACK_SIZE, NULL,
                       TIME_SERVICE_WORKER_PRIORITY, &s_worker) == pdPASS;
#endif
}

static void deinitialize_sntp(void)
{
#ifdef TIME_SERVICE_HOST_TEST
    s_test_ops.sntp_deinit(s_test_ops.context);
#else
    esp_netif_sntp_deinit();
#endif
}

esp_err_t time_service_init(void)
{
#ifdef TIME_SERVICE_HOST_TEST
    if (!s_test_ops_configured) {
        return ESP_ERR_INVALID_STATE;
    }
#else
    if (s_lock == NULL) {
        s_lock = xSemaphoreCreateMutex();
        if (s_lock == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }
#endif

    SERVICE_LOCK();
    if (s_initialized) {
        const esp_err_t prior_error = s_snapshot.init_error;
        SERVICE_UNLOCK();
        return prior_error;
    }
    s_snapshot = (time_service_snapshot_t){
        .sync_state = TIME_SYNC_UNSYNCHRONIZED,
        .rtc_state = RTC_STATE_UNAVAILABLE,
        .timezone_offset_minutes = TIME_SETTINGS_DEFAULT_OFFSET_MINUTES,
    };
    memset(&s_pending_time, 0, sizeof(s_pending_time));
    SERVICE_UNLOCK();

    (void)initialize_timezone();
    (void)initialize_rtc();
    (void)initialize_time_source();

    esp_err_t error = initialize_sntp();
    if (error != ESP_OK) {
        SERVICE_LOCK();
        s_snapshot.sntp_error = error;
        record_first_error(error);
        SERVICE_UNLOCK();
        return error;
    }

    SERVICE_LOCK();
    s_snapshot.sntp_initialized = true;
    SERVICE_UNLOCK();
    if (!create_worker()) {
        deinitialize_sntp();
        SERVICE_LOCK();
        s_snapshot.sntp_initialized = false;
        s_snapshot.sntp_error = ESP_ERR_NO_MEM;
        record_first_error(ESP_ERR_NO_MEM);
        SERVICE_UNLOCK();
        return ESP_ERR_NO_MEM;
    }

    SERVICE_LOCK();
    s_initialized = true;
    s_snapshot.initialized = true;
    s_snapshot.worker_ready = true;
    error = s_snapshot.init_error;
    SERVICE_UNLOCK();
    return error;
}

/**
 * @brief SNTP's start also restarts an already-running client, so the same call serves the GOT_IP
 * path and the settings page's manual resync.
 */
static esp_err_t start_sntp(void)
{
#ifdef TIME_SERVICE_HOST_TEST
    const esp_err_t error = s_test_ops.sntp_start(s_test_ops.context);
#else
    const esp_err_t error = esp_netif_sntp_start();
#endif
    SERVICE_LOCK();
    s_snapshot.sntp_error = error;
    SERVICE_UNLOCK();
    return error;
}

esp_err_t time_service_notify_network_ready(void)
{
    SERVICE_LOCK();
    if (!s_initialized || !s_snapshot.sntp_initialized) {
        SERVICE_UNLOCK();
        return ESP_ERR_INVALID_STATE;
    }
    s_snapshot.network_ready = true;
    ++s_snapshot.got_ip_events;
    ++s_snapshot.sntp_start_calls;
    SERVICE_UNLOCK();

    return start_sntp();
}

esp_err_t time_service_request_sync(void)
{
    SERVICE_LOCK();
    const bool usable = s_initialized && s_snapshot.sntp_initialized && s_snapshot.network_ready;
    if (usable) {
        ++s_snapshot.sntp_start_calls;
    }
    SERVICE_UNLOCK();
    if (!usable) {
        return ESP_ERR_INVALID_STATE;
    }
    return start_sntp();
}

void time_service_notify_network_lost(void)
{
    SERVICE_LOCK();
    if (s_initialized) {
        s_snapshot.network_ready = false;
        ++s_snapshot.disconnect_events;
    }
    SERVICE_UNLOCK();
}

esp_err_t time_service_get_snapshot(time_service_snapshot_t *snapshot)
{
    if (snapshot == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
#ifndef TIME_SERVICE_HOST_TEST
    if (s_lock == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
#endif
    SERVICE_LOCK();
    *snapshot = s_snapshot;
    SERVICE_UNLOCK();
    return ESP_OK;
}

esp_err_t time_service_update_timezone_offset(int32_t offset_minutes)
{
    if (!time_policy_is_valid_offset(offset_minutes)) {
        return ESP_ERR_INVALID_ARG;
    }
#ifndef TIME_SERVICE_HOST_TEST
    if (s_lock == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
#endif
    SERVICE_LOCK();
    if (!s_initialized) {
        SERVICE_UNLOCK();
        return ESP_ERR_INVALID_STATE;
    }
    s_snapshot.timezone_offset_minutes = offset_minutes;
    s_snapshot.timezone_error = ESP_OK;
    SERVICE_UNLOCK();
    return ESP_OK;
}

esp_err_t time_service_get_time_source(time_source_t *source)
{
    if (source == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
#ifndef TIME_SERVICE_HOST_TEST
    if (s_lock == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
#endif
    SERVICE_LOCK();
    *source = s_snapshot.time_source;
    SERVICE_UNLOCK();
    return ESP_OK;
}

esp_err_t time_service_set_time_source(time_source_t source)
{
    if (!time_source_is_valid(source)) {
        return ESP_ERR_INVALID_ARG;
    }
#ifndef TIME_SERVICE_HOST_TEST
    if (s_lock == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
#endif
    SERVICE_LOCK();
    const bool initialized = s_initialized;
    if (initialized) {
        s_snapshot.time_source = source;
    }
    SERVICE_UNLOCK();
    if (!initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    const esp_err_t save_error = platform_time_source_save(source);
    const esp_err_t apply_error = apply_time_source(source);
    return save_error != ESP_OK ? save_error : apply_error;
}

#ifdef TIME_SERVICE_HOST_TEST
esp_err_t time_service_test_set_ops(const time_service_test_ops_t *ops)
{
    if (ops == NULL || ops->timezone_load == NULL || ops->timezone_apply == NULL ||
        ops->timezone_clear == NULL || ops->rtc_init == NULL || ops->rtc_reconcile == NULL ||
        ops->sntp_init == NULL || ops->sntp_deinit == NULL || ops->sntp_start == NULL ||
        ops->worker_create == NULL || ops->time_source_load == NULL ||
        ops->time_source_save == NULL || ops->rtc_read_now == NULL ||
        ops->system_time_set == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    s_test_ops = *ops;
    s_test_ops_configured = true;
    return ESP_OK;
}

void time_service_test_reset(void)
{
    memset(&s_test_ops, 0, sizeof(s_test_ops));
    s_test_ops_configured = false;
    s_initialized = false;
    memset(&s_pending_time, 0, sizeof(s_pending_time));
    s_snapshot = (time_service_snapshot_t){
        .sync_state = TIME_SYNC_UNSYNCHRONIZED,
        .rtc_state = RTC_STATE_UNAVAILABLE,
        .timezone_offset_minutes = TIME_SETTINGS_DEFAULT_OFFSET_MINUTES,
    };
}

void time_service_test_invoke_sync_callback(const struct timeval *tv)
{
    if (tv == NULL) {
        synchronization_callback(NULL);
        return;
    }
    struct timeval copy = *tv;
    synchronization_callback(&copy);
}

bool time_service_test_drain_worker(void)
{
    return process_pending_synchronization();
}
#endif
