#include "music_player_service.h"

#include <string.h>

#include "sdkconfig.h"
#if CONFIG_BT_ENABLED
#include "esp_bt.h"
#endif
#include "esp_attr.h"
#include "esp_bt_audio.h"
#include "esp_bt_audio_classic.h"
#include "esp_bt_audio_defs.h"
#include "esp_bt_audio_event.h"
#include "esp_bt_audio_host.h"
#include "esp_bt_audio_playback.h"
#include "audio_runtime.h"
#include "audio_gmf.h"
#include "audio_output.h"
#include "audio_volume.h"
#include "esp_bt_audio_vol.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "memory_monitor.h"
#include "music_bt_power_store.h"
#include "music_volume_store.h"
#include "music_bt_gmf.h"
#include "music_player.h"
#include "music_bt_source.h"
#include "music_sd_source.h"

#define TAG "MusicService"
/** @brief Advertise the model name with the last three Bluetooth address bytes. */
#define MUSIC_BT_DEVICE_NAME_PREFIX "ESP32-S31"
#define MUSIC_BT_DISCONNECT_TIMEOUT_MS 2000U
#define MUSIC_BT_CALLBACK_QUIESCE_TIMEOUT_MS 2000U
#define MUSIC_BT_PLAYBACK_NOTIFICATION_MASK (ESP_BT_AUDIO_PLAYBACK_EVENT_PLAY_STATUS_CHANGE | \
                                               ESP_BT_AUDIO_PLAYBACK_EVENT_TRACK_CHANGE | \
                                               ESP_BT_AUDIO_PLAYBACK_EVENT_TRACK_REACHED_END | \
                                               ESP_BT_AUDIO_PLAYBACK_EVENT_TRACK_REACHED_START | \
                                               ESP_BT_AUDIO_PLAYBACK_EVENT_PLAY_POS_CHANGED | \
                                               ESP_BT_AUDIO_PLAYBACK_EVENT_NOW_PLAYING_CHANGE | \
                                               ESP_BT_AUDIO_PLAYBACK_EVENT_AVAILABLE_PLAYERS_CHANGE | \
                                               ESP_BT_AUDIO_PLAYBACK_EVENT_ADDRESSED_PLAYER_CHANGE)
#define MUSIC_SERVICE_EVENT_QUEUE_CAPACITY 16U
#define MUSIC_SERVICE_EVENT_TEXT_MAX MUSIC_BT_ALBUM_MAX
#define MUSIC_SERVICE_EVENT_METADATA_MAX MUSIC_BT_ALBUM_MAX
#define MUSIC_SERVICE_TASK_STACK_SIZE 4096U
#define MUSIC_SERVICE_LOCK_TIMEOUT_MS 2000U
#define MUSIC_BT_GMF_LOCK_TIMEOUT_MS 2000U
typedef enum {
    MUSIC_SERVICE_EVENT_SD_END = 0,
    MUSIC_SERVICE_EVENT_SD_ERROR,
    MUSIC_SERVICE_EVENT_BT_CONNECTION,
    MUSIC_SERVICE_EVENT_BT_STREAM,
    MUSIC_SERVICE_EVENT_BT_PLAYBACK,
    MUSIC_SERVICE_EVENT_BT_METADATA,
    MUSIC_SERVICE_EVENT_BT_VOLUME,
} music_service_event_type_t;

typedef struct {
    music_service_event_type_t type;
    uint32_t generation;
    uint32_t bt_generation;
    union {
        struct {
            music_player_error_t error;
            uint32_t source_generation;
            char detail[MUSIC_SERVICE_EVENT_TEXT_MAX];
        } backend;
        struct {
            uint8_t peer_addr[ESP_BD_ADDR_LEN];
            bool connected;
        } connection;
        struct {
            esp_bt_audio_stream_handle_t handle;
            esp_bt_audio_stream_state_t state;
            bool sink;
        } stream;
        struct {
            uint32_t events;
            uint32_t play_status;
            uint32_t position;
        } playback;
        struct {
            bool absolute;
            uint8_t level;
            bool muted;
            bool increase;
        } volume;
        /** Queue text metadata only; copy cover-art payloads directly into player storage. */
        struct {
            uint32_t type;
            uint32_t length;
            uint8_t value[MUSIC_SERVICE_EVENT_METADATA_MAX];
        } metadata;
    } data;
} music_service_event_t;

/** @brief The Classic Bluetooth source is the remote A2DP source; audio is rendered locally. */
typedef struct {
    SemaphoreHandle_t lock;
    SemaphoreHandle_t bt_disconnect_sem;
    SemaphoreHandle_t bt_callback_idle_sem;
    SemaphoreHandle_t bt_gmf_lock;
    music_service_event_t event_queue[MUSIC_SERVICE_EVENT_QUEUE_CAPACITY];
    size_t event_queue_head;
    size_t event_queue_tail;
    size_t event_queue_count;
    TaskHandle_t service_task;
    volatile bool service_task_stop;
    uint32_t event_drop_count;
    uint32_t event_queue_full_count;
    uint32_t event_queue_full_reported_count;
    uint32_t event_coalesced_count;
    uint32_t event_coalesced_reported_count;
    uint32_t event_state_evicted_count;
    uint32_t event_state_evicted_reported_count;
    uint32_t event_lock_timeout_count;
    uint32_t event_lock_timeout_reported_count;
    esp_gmf_pool_handle_t pool;
    dev_audio_codec_handles_t *dac;
    bool controller_ready;
    bool controller_enabled;
    /** Host availability; bt_audio_ready additionally tracks the audio session. */
    bool bt_host_ready;
    bool bt_audio_ready;
    bool bt_source_configured;
    bool bt_callbacks_enabled;
    bool bt_callbacks_quiescing;
    bool bt_disconnect_waiting;
    uint32_t bt_callbacks_in_flight;
    bool bt_peer_valid;
    bool bt_peer_connected;
    uint8_t bt_peer_addr[ESP_BD_ADDR_LEN];
    /** Track the live A2DP stream across source switches for pipeline reattachment. */
    esp_bt_audio_stream_handle_t bt_sink_stream;
    bool bt_sink_stream_started;
    bool sd_source_opened;
    bool entered;
    bool playing;
    /** Settings owns Bluetooth power; the music app owns playback pipeline use. */
    bool bt_power_owned;
    bool bt_power_persistent;
    bool bt_power_app_owned;
    bool source_switching;
    bool bt_playback_events_suppressed;
    bool sd_track_loaded;
    bool player_initialized;
    bool player_opened;
    bool exit_pending;
    bool controller_initialized_owned;
    bool controller_enabled_owned;
    bool bt_audio_initialized_owned;
    bool volume_listener_registered;
    bool audio_runtime_acquired;
    uint32_t event_generation;
    uint32_t bt_track_index;
    uint32_t bt_elapsed_ms;
    uint32_t bt_duration_ms;
    music_player_source_t source;
    music_sd_source_t sd;
    music_bt_source_t bt;
    music_bt_gmf_t bt_gmf;
} music_service_state_t;

static StaticSemaphore_t s_service_lock_storage;
static StaticSemaphore_t s_bt_disconnect_sem_storage;
static StaticSemaphore_t s_bt_callback_idle_sem_storage;
static StaticSemaphore_t s_bt_gmf_lock_storage;
static portMUX_TYPE s_bt_event_guard = portMUX_INITIALIZER_UNLOCKED;
static portMUX_TYPE s_service_lock_init_guard = portMUX_INITIALIZER_UNLOCKED;
/** The service state contains the bounded SD catalog and is accessed only from task context. */
EXT_RAM_BSS_ATTR static music_service_state_t s_service;

static void music_bt_audio_event_cb(esp_bt_audio_event_t event, void *event_data, void *user_data);

static void music_volume_listener(const audio_volume_state_t *state,
                                  audio_volume_origin_t origin,
                                  void *context)
{
    music_service_state_t *service = context;
    if (state == NULL || service == NULL || origin == AUDIO_VOLUME_ORIGIN_BT_REMOTE) {
        return;
    }
    bool notify = false;
    portENTER_CRITICAL(&s_bt_event_guard);
    notify = service->bt_audio_ready && service->bt_source_configured &&
             service->bt_peer_valid && service->bt_peer_connected &&
             service->bt_callbacks_enabled;
    portEXIT_CRITICAL(&s_bt_event_guard);
    if (notify) {
        const uint32_t level = state->muted ? AUDIO_VOLUME_MIN : state->level;
        const esp_err_t ret = esp_bt_audio_vol_notify(level);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "notify Classic Bluetooth volume %u failed: %s",
                     (unsigned)level, esp_err_to_name(ret));
        }
    }
}

static esp_err_t music_volume_listener_register_locked(void)
{
    if (s_service.volume_listener_registered) {
        return ESP_OK;
    }
    const esp_err_t ret = audio_volume_subscribe(music_volume_listener, &s_service);
    if (ret == ESP_OK) {
        s_service.volume_listener_registered = true;
    }
    return ret;
}

static void music_volume_listener_unregister_locked(void)
{
    if (s_service.volume_listener_registered) {
        (void)audio_volume_unsubscribe(music_volume_listener, &s_service);
        s_service.volume_listener_registered = false;
    }
}
static esp_err_t music_bt_cleanup_locked(void);
static esp_err_t music_sd_open_locked(void);
static void music_service_task(void *context);
static void music_service_apply_event_locked(const music_service_event_t *event);
static void music_sd_service_event_cb(void *context, music_player_error_t error,
                                       uint32_t source_generation, const char *detail);

/** The service owns hardware commands; the unified controller is a thread-safe state mirror. */
static const music_player_backend_t s_player_backend = {0};

#define MUSIC_BT_METADATA_REQUEST_MASK (ESP_BT_AUDIO_PLAYBACK_METADATA_TITLE | \
                                        ESP_BT_AUDIO_PLAYBACK_METADATA_ARTIST | \
                                        ESP_BT_AUDIO_PLAYBACK_METADATA_ALBUM | \
                                        ESP_BT_AUDIO_PLAYBACK_METADATA_PLAYING_TIME | \
                                        ESP_BT_AUDIO_PLAYBACK_METADATA_COVER_ART)

static music_player_error_t player_error(esp_err_t ret)
{
    return ret == ESP_OK ? MUSIC_PLAYER_ERROR_OK : MUSIC_PLAYER_ERROR_BACKEND;
}

static void record_cleanup_error(esp_err_t *first_error, const char *step, esp_err_t ret)
{
    if (ret == ESP_OK) {
        return;
    }
    ESP_LOGE(TAG, "%s failed: %s", step, esp_err_to_name(ret));
    if (*first_error == ESP_OK) {
        *first_error = ret;
    }
}

static SemaphoreHandle_t music_service_lock(void)
{
    portENTER_CRITICAL(&s_service_lock_init_guard);
    if (s_service.lock == NULL) {
        s_service.lock = xSemaphoreCreateMutexStatic(&s_service_lock_storage);
    }
    if (s_service.bt_disconnect_sem == NULL) {
        s_service.bt_disconnect_sem = xSemaphoreCreateBinaryStatic(&s_bt_disconnect_sem_storage);
    }
    if (s_service.bt_callback_idle_sem == NULL) {
        s_service.bt_callback_idle_sem = xSemaphoreCreateBinaryStatic(&s_bt_callback_idle_sem_storage);
    }
    if (s_service.bt_gmf_lock == NULL) {
        s_service.bt_gmf_lock = xSemaphoreCreateMutexStatic(&s_bt_gmf_lock_storage);
    }
    SemaphoreHandle_t lock = s_service.lock;
    portEXIT_CRITICAL(&s_service_lock_init_guard);
    return lock;
}

static size_t music_service_event_queue_index(const music_service_state_t *service, size_t offset)
{
    return (service->event_queue_head + offset) % MUSIC_SERVICE_EVENT_QUEUE_CAPACITY;
}

static bool music_service_event_can_coalesce(const music_service_event_t *event)
{
    return event != NULL &&
           (event->type == MUSIC_SERVICE_EVENT_BT_PLAYBACK ||
            event->type == MUSIC_SERVICE_EVENT_BT_METADATA);
}

static bool music_service_event_merge_locked(music_service_event_t *pending,
                                              const music_service_event_t *incoming)
{
    if (pending == NULL || incoming == NULL || pending->generation != incoming->generation ||
        pending->bt_generation != incoming->bt_generation || pending->type != incoming->type) {
        return false;
    }
    if (incoming->type == MUSIC_SERVICE_EVENT_BT_PLAYBACK) {
        pending->data.playback.events |= incoming->data.playback.events;
        if ((incoming->data.playback.events & ESP_BT_AUDIO_PLAYBACK_EVENT_PLAY_STATUS_CHANGE) != 0U) {
            pending->data.playback.play_status = incoming->data.playback.play_status;
        }
        if ((incoming->data.playback.events & ESP_BT_AUDIO_PLAYBACK_EVENT_PLAY_POS_CHANGED) != 0U) {
            pending->data.playback.position = incoming->data.playback.position;
        }
        return true;
    }
    if (incoming->type == MUSIC_SERVICE_EVENT_BT_METADATA &&
        pending->data.metadata.type == incoming->data.metadata.type) {
        *pending = *incoming;
        return true;
    }
    return false;
}

static bool music_service_event_merge_queued_locked(music_service_state_t *service,
                                                     const music_service_event_t *incoming)
{
    if (!music_service_event_can_coalesce(incoming)) {
        return false;
    }
    for (size_t offset = 0U; offset < service->event_queue_count; ++offset) {
        music_service_event_t *pending =
            &service->event_queue[music_service_event_queue_index(service, offset)];
        if (music_service_event_merge_locked(pending, incoming)) {
            ++service->event_coalesced_count;
            return true;
        }
    }
    return false;
}

static bool music_service_event_replace_stale_locked(music_service_state_t *service,
                                                       const music_service_event_t *incoming)
{
    if (!music_service_event_can_coalesce(incoming)) {
        return false;
    }
    for (size_t offset = 0U; offset < service->event_queue_count; ++offset) {
        music_service_event_t *pending =
            &service->event_queue[music_service_event_queue_index(service, offset)];
        if (pending->type != incoming->type) {
            continue;
        }
        if (incoming->type == MUSIC_SERVICE_EVENT_BT_METADATA &&
            pending->data.metadata.type != incoming->data.metadata.type) {
            continue;
        }
        if (pending->generation == incoming->generation &&
            pending->bt_generation == incoming->bt_generation) {
            continue;
        }
        *pending = *incoming;
        ++service->event_coalesced_count;
        return true;
    }
    return false;
}

static bool music_service_event_is_lossless(const music_service_event_t *event)
{
    return event != NULL && !music_service_event_can_coalesce(event);
}

static bool music_service_event_evict_state_locked(music_service_state_t *service)
{
    if (service == NULL) {
        return false;
    }
    for (size_t offset = 0U; offset < service->event_queue_count; ++offset) {
        music_service_event_t *candidate =
            &service->event_queue[music_service_event_queue_index(service, offset)];
        if (!music_service_event_can_coalesce(candidate)) {
            continue;
        }
        for (size_t shift = offset; shift + 1U < service->event_queue_count; ++shift) {
            service->event_queue[music_service_event_queue_index(service, shift)] =
                service->event_queue[music_service_event_queue_index(service, shift + 1U)];
        }
        service->event_queue_tail =
            (service->event_queue_tail + MUSIC_SERVICE_EVENT_QUEUE_CAPACITY - 1U) %
            MUSIC_SERVICE_EVENT_QUEUE_CAPACITY;
        --service->event_queue_count;
        ++service->event_state_evicted_count;
        return true;
    }
    return false;
}

static void music_service_event_append_locked(music_service_state_t *service,
                                               const music_service_event_t *event)
{
    service->event_queue[service->event_queue_tail] = *event;
    service->event_queue_tail =
        (service->event_queue_tail + 1U) % MUSIC_SERVICE_EVENT_QUEUE_CAPACITY;
    ++service->event_queue_count;
}

static bool music_service_enqueue_event(music_service_state_t *service,
                                         const music_service_event_t *event)
{
    TaskHandle_t task = NULL;
    bool accepted = false;

    if (service == NULL || event == NULL) {
        return false;
    }
    portENTER_CRITICAL(&s_bt_event_guard);
    if (service->service_task == NULL) {
        ++service->event_drop_count;
    } else if (music_service_event_merge_queued_locked(service, event)) {
        task = service->service_task;
        accepted = true;
    } else if (service->event_queue_count >= MUSIC_SERVICE_EVENT_QUEUE_CAPACITY &&
               music_service_event_replace_stale_locked(service, event)) {
        task = service->service_task;
        accepted = true;
    } else if (service->event_queue_count >= MUSIC_SERVICE_EVENT_QUEUE_CAPACITY &&
               music_service_event_is_lossless(event) &&
               music_service_event_evict_state_locked(service)) {
        music_service_event_append_locked(service, event);
        task = service->service_task;
        accepted = true;
    } else if (service->event_queue_count < MUSIC_SERVICE_EVENT_QUEUE_CAPACITY) {
        music_service_event_append_locked(service, event);
        task = service->service_task;
        accepted = true;
    } else {
        ++service->event_drop_count;
        ++service->event_queue_full_count;
    }
    portEXIT_CRITICAL(&s_bt_event_guard);
    if (accepted && task != NULL) {
        (void)xTaskNotifyGive(task);
    }
    return accepted;
}

static void music_service_report_event_queue_drops(music_service_state_t *service)
{
    uint32_t full_count;
    uint32_t coalesced_count;
    uint32_t state_evicted_count;
    uint32_t lock_timeout_count;

    portENTER_CRITICAL(&s_bt_event_guard);
    full_count = service->event_queue_full_count;
    coalesced_count = service->event_coalesced_count;
    state_evicted_count = service->event_state_evicted_count;
    lock_timeout_count = service->event_lock_timeout_count;
    if (full_count != service->event_queue_full_reported_count) {
        service->event_queue_full_reported_count = full_count;
    } else {
        full_count = 0U;
    }
    if (coalesced_count != service->event_coalesced_reported_count) {
        service->event_coalesced_reported_count = coalesced_count;
    } else {
        coalesced_count = 0U;
    }
    if (state_evicted_count != service->event_state_evicted_reported_count) {
        service->event_state_evicted_reported_count = state_evicted_count;
    } else {
        state_evicted_count = 0U;
    }
    if (lock_timeout_count != service->event_lock_timeout_reported_count) {
        service->event_lock_timeout_reported_count = lock_timeout_count;
    } else {
        lock_timeout_count = 0U;
    }
    portEXIT_CRITICAL(&s_bt_event_guard);
    if (full_count != 0U) {
        ESP_LOGW(TAG, "event queue full; dropped %u event(s) total",
                 (unsigned)full_count);
    }
    if (coalesced_count != 0U) {
        ESP_LOGI(TAG, "coalesced %u Bluetooth state event(s) total",
                 (unsigned)coalesced_count);
    }
    if (state_evicted_count != 0U) {
        ESP_LOGW(TAG, "evicted %u coalescible state event(s) to retain critical events, total %u",
                 (unsigned)state_evicted_count, (unsigned)state_evicted_count);
    }
    if (lock_timeout_count != 0U) {
        ESP_LOGW(TAG, "event service lock timeout; retained queued event(s), total %u",
                 (unsigned)lock_timeout_count);
    }
}

static esp_err_t music_service_start_task_locked(void)
{
    if (s_service.service_task != NULL) {
        return ESP_OK;
    }
    portENTER_CRITICAL(&s_bt_event_guard);
    s_service.event_queue_head = 0U;
    s_service.event_queue_tail = 0U;
    s_service.event_queue_count = 0U;
    s_service.service_task_stop = false;
    portEXIT_CRITICAL(&s_bt_event_guard);
    const BaseType_t created = xTaskCreate(music_service_task, "music_service",
                                           MUSIC_SERVICE_TASK_STACK_SIZE, &s_service,
                                           tskIDLE_PRIORITY + 1U, &s_service.service_task);
    return created == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}

static void music_service_stop_task_locked(void)
{
    if (s_service.service_task == NULL) {
        return;
    }
    portENTER_CRITICAL(&s_bt_event_guard);
    TaskHandle_t task = s_service.service_task;
    s_service.service_task = NULL;
    s_service.event_queue_head = 0U;
    s_service.event_queue_tail = 0U;
    s_service.event_queue_count = 0U;
    s_service.service_task_stop = true;
    portEXIT_CRITICAL(&s_bt_event_guard);
    (void)xTaskNotifyGive(task);
    vTaskDelete(task);
}

/**
 * @brief Update peer-link state directly from stack callbacks using the event guard.
 * @param service Service state to update.
 * @param connection Bluetooth connection event.
 */
static void music_bt_record_connection_event(music_service_state_t *service,
                                              const esp_bt_audio_event_connection_st_t *connection)
{
    bool signal_disconnect = false;

    if (service == NULL || connection == NULL) {
        return;
    }
    portENTER_CRITICAL(&s_bt_event_guard);
    if (connection->connected) {
        memcpy(service->bt_peer_addr, connection->addr, ESP_BD_ADDR_LEN);
        service->bt_peer_valid = true;
        service->bt_peer_connected = true;
    } else if (service->bt_peer_valid &&
               memcmp(service->bt_peer_addr, connection->addr, ESP_BD_ADDR_LEN) == 0) {
        service->bt_peer_connected = false;
        signal_disconnect = service->bt_disconnect_waiting;
    }
    portEXIT_CRITICAL(&s_bt_event_guard);

    if (signal_disconnect && service->bt_disconnect_sem != NULL) {
        (void)xSemaphoreGive(service->bt_disconnect_sem);
    }
}

static void music_bt_clear_peer_state(void)
{
    portENTER_CRITICAL(&s_bt_event_guard);
    s_service.bt_peer_valid = false;
    s_service.bt_peer_connected = false;
    (void)memset(s_service.bt_peer_addr, 0, sizeof(s_service.bt_peer_addr));
    portEXIT_CRITICAL(&s_bt_event_guard);
}

/**
 * @brief Copy completed cover art into bounded player storage; retain existing art when the new payload is too large.
 * @param metadata Playback metadata containing cover art.
 */
static void music_bt_store_cover_art(const esp_bt_audio_event_playback_metadata_t *metadata)
{
    if (metadata->length < sizeof(esp_bt_audio_playback_cover_art_t)) {
        return;
    }
    const esp_bt_audio_playback_cover_art_t *art =
        (const esp_bt_audio_playback_cover_art_t *)metadata->value;
    if (art->data == NULL || art->size == 0U) {
        return;
    }
    if (art->size > MUSIC_PLAYER_COVER_ART_MAX_SIZE) {
        ESP_LOGW(TAG, "Classic Bluetooth cover art exceeds application limit (%u bytes)",
                 (unsigned)MUSIC_PLAYER_COVER_ART_MAX_SIZE);
        return;
    }
    /** Determine the received image format from its payload bytes. */
    static const uint8_t png_signature[8] = {0x89U, 'P', 'N', 'G', 0x0DU, 0x0AU, 0x1AU, 0x0AU};
    uint32_t format = 0U;
    if (art->size >= 2U && art->data[0] == 0xFFU && art->data[1] == 0xD8U) {
        format = ESP_BT_AUDIO_FOURCC_JPEG;
    } else if (art->size >= sizeof(png_signature) &&
               memcmp(art->data, png_signature, sizeof(png_signature)) == 0) {
        format = ESP_BT_AUDIO_FOURCC_PNG;
    }
    if (format == 0U) {
        ESP_LOGW(TAG, "Classic Bluetooth cover art in an unsupported format (%u bytes)",
                 (unsigned)art->size);
        return;
    }
    if (!music_player_set_cover_art(format, art->data, art->size)) {
        ESP_LOGW(TAG, "Classic Bluetooth cover art rejected");
        return;
    }
    ESP_LOGI(TAG, "Classic Bluetooth cover art received: %u bytes %s", (unsigned)art->size,
             format == ESP_BT_AUDIO_FOURCC_PNG ? "png" : "jpeg");
}

static bool music_bt_event_begin(music_service_state_t *service, esp_bt_audio_event_t event)
{
    bool enabled;

    portENTER_CRITICAL(&s_bt_event_guard);
    /** Track links and streams while the host is active; gate media reports on callbacks and quiesce state. */
    enabled = service->bt_callbacks_enabled ||
              ((event == ESP_BT_AUDIO_EVENT_CONNECTION_STATE_CHG ||
                event == ESP_BT_AUDIO_EVENT_STREAM_STATE_CHG) &&
               !service->bt_callbacks_quiescing);
    if (enabled) {
        ++service->bt_callbacks_in_flight;
    }
    portEXIT_CRITICAL(&s_bt_event_guard);
    return enabled;
}

static void music_bt_event_end(music_service_state_t *service)
{
    bool signal_idle = false;

    portENTER_CRITICAL(&s_bt_event_guard);
    if (service->bt_callbacks_in_flight > 0U) {
        --service->bt_callbacks_in_flight;
    }
    if (service->bt_callbacks_in_flight == 0U && !service->bt_callbacks_enabled) {
        signal_idle = true;
    }
    portEXIT_CRITICAL(&s_bt_event_guard);

    if (signal_idle && service->bt_callback_idle_sem != NULL) {
        (void)xSemaphoreGive(service->bt_callback_idle_sem);
    }
}

static esp_err_t music_bt_disable_callbacks_locked(void)
{
    const TickType_t timeout = pdMS_TO_TICKS(MUSIC_BT_CALLBACK_QUIESCE_TIMEOUT_MS) == 0U
                                    ? 1U : pdMS_TO_TICKS(MUSIC_BT_CALLBACK_QUIESCE_TIMEOUT_MS);
    bool callbacks_in_flight;

    while (s_service.bt_callback_idle_sem != NULL &&
           xSemaphoreTake(s_service.bt_callback_idle_sem, 0U) == pdTRUE) {
    }
    portENTER_CRITICAL(&s_bt_event_guard);
    s_service.bt_callbacks_enabled = false;
    s_service.bt_callbacks_quiescing = true;
    callbacks_in_flight = s_service.bt_callbacks_in_flight != 0U;
    portEXIT_CRITICAL(&s_bt_event_guard);

    if (!callbacks_in_flight) {
        return ESP_OK;
    }
    if (s_service.bt_callback_idle_sem == NULL ||
        xSemaphoreTake(s_service.bt_callback_idle_sem, timeout) != pdTRUE) {
        portENTER_CRITICAL(&s_bt_event_guard);
        s_service.bt_callbacks_enabled = true;
        s_service.bt_callbacks_quiescing = false;
        portEXIT_CRITICAL(&s_bt_event_guard);
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

static esp_err_t music_audio_runtime_check_locked(void)
{
    if (!audio_runtime_is_initialized()) {
        return ESP_ERR_INVALID_STATE;
    }
    s_service.pool = audio_gmf_get_pool();
    if (audio_output_get_handles(&s_service.dac) != ESP_OK) {
        s_service.dac = NULL;
    }
    return s_service.pool != NULL && s_service.dac != NULL ? ESP_OK : ESP_ERR_INVALID_STATE;
}

static esp_err_t music_bt_gmf_lock_take(void)
{
    if (s_service.bt_gmf_lock == NULL ||
        xSemaphoreTake(s_service.bt_gmf_lock, pdMS_TO_TICKS(MUSIC_BT_GMF_LOCK_TIMEOUT_MS)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

static void music_bt_gmf_lock_give(void)
{
    if (s_service.bt_gmf_lock != NULL) {
        xSemaphoreGive(s_service.bt_gmf_lock);
    }
}

static esp_err_t music_bt_gmf_init_serialized(music_service_state_t *service)
{
    esp_err_t ret = music_bt_gmf_lock_take();
    if (ret == ESP_OK) {
        ret = music_bt_gmf_init(&service->bt_gmf, service->pool, service->dac);
        music_bt_gmf_lock_give();
    }
    return ret;
}

static esp_err_t music_bt_gmf_deinit_serialized(music_service_state_t *service)
{
    esp_err_t ret = music_bt_gmf_lock_take();
    if (ret == ESP_OK) {
        ret = music_bt_gmf_deinit(&service->bt_gmf);
        music_bt_gmf_lock_give();
    }
    return ret;
}

static esp_err_t music_bt_gmf_stream_state_serialized(music_service_state_t *service,
                                                       esp_bt_audio_stream_handle_t stream,
                                                       esp_bt_audio_stream_state_t state)
{
    esp_err_t ret = music_bt_gmf_lock_take();
    if (ret == ESP_OK) {
        ret = music_bt_gmf_on_stream_state(&service->bt_gmf, stream, state);
        music_bt_gmf_lock_give();
    }
    return ret;
}

static music_player_error_t bt_noop(void *context)
{
    (void)context;
    return MUSIC_PLAYER_ERROR_OK;
}

static music_player_error_t bt_pipeline_start(void *context)
{
    music_service_state_t *service = context;
    return player_error(music_bt_gmf_init_serialized(service));
}

static music_player_error_t bt_pipeline_stop(void *context)
{
    music_service_state_t *service = context;
    return player_error(music_bt_gmf_deinit_serialized(service));
}

static music_player_error_t bt_play(void *context)
{
    (void)context;
    return player_error(esp_bt_audio_playback_play());
}

static music_player_error_t bt_pause(void *context)
{
    (void)context;
    return player_error(esp_bt_audio_playback_pause());
}

static music_player_error_t bt_previous(void *context)
{
    (void)context;
    return player_error(esp_bt_audio_playback_prev());
}

static music_player_error_t bt_next(void *context)
{
    (void)context;
    return player_error(esp_bt_audio_playback_next());
}

static const music_bt_source_backend_t s_bt_backend = {
    .host_init = bt_noop,
    .bt_audio_init = bt_noop,
    .pipeline_io_start = bt_pipeline_start,
    .stream_stop = bt_pipeline_stop,
    .pipeline_io_destroy = bt_noop,
    .bt_audio_deinit = bt_noop,
    .host_deinit = bt_noop,
    .playback_prev = bt_previous,
    .playback_play = bt_play,
    .playback_pause = bt_pause,
    .playback_next = bt_next,
};

static void music_bt_controller_state_sync_locked(void)
{
#if CONFIG_BT_ENABLED
    const esp_bt_controller_status_t status = esp_bt_controller_get_status();
    s_service.controller_enabled = status == ESP_BT_CONTROLLER_STATUS_ENABLED;
    s_service.controller_ready = status == ESP_BT_CONTROLLER_STATUS_INITED ||
                                 status == ESP_BT_CONTROLLER_STATUS_ENABLED;
#endif
}

static bool music_bt_audio_inactive_locked(void)
{
    bool peer_connected;
    portENTER_CRITICAL(&s_bt_event_guard);
    peer_connected = s_service.bt_peer_valid && s_service.bt_peer_connected;
    portEXIT_CRITICAL(&s_bt_event_guard);
    return !s_service.bt_source_configured && !s_service.bt_audio_ready &&
           !s_service.bt_callbacks_enabled && !peer_connected;
}

/**
 * @brief Release the GMF registration before creating the local pipeline; retain the powered Bluetooth host.
 * @return true if the Bluetooth source pipeline is not configured.
 */
static bool music_bt_pipeline_closed_locked(void)
{
    return !s_service.bt_source_configured;
}

static bool music_bt_power_inactive_locked(void)
{
    music_bt_controller_state_sync_locked();
    return !s_service.controller_ready && !s_service.controller_enabled;
}

/**
 * @brief Build the discoverable name from the model and Bluetooth address suffix.
 * @return Pointer to the persistent Bluetooth device name.
 */
static const char *music_bt_device_name(void)
{
    static char name[ESP_BT_AUDIO_HOST_MAX_DEV_NAME_LEN];
    if (name[0] != '\0') {
        return name;
    }
    uint8_t mac[6] = {0};
    if (esp_read_mac(mac, ESP_MAC_BT) != ESP_OK) {
        (void)snprintf(name, sizeof(name), "%s", MUSIC_BT_DEVICE_NAME_PREFIX);
        return name;
    }
    (void)snprintf(name, sizeof(name), "%s-%02X%02X%02X", MUSIC_BT_DEVICE_NAME_PREFIX,
                   mac[3], mac[4], mac[5]);
    return name;
}

/**
 * @brief Start the Bluedroid host with A2DP and AVRCP roles; the music app attaches its playback pipeline later.
 * @return ESP_OK on success, otherwise an initialization error.
 */
static esp_err_t music_bt_host_start_locked(void)
{
#if CONFIG_BT_ENABLED && CONFIG_BT_CLASSIC_ENABLED && CONFIG_BT_BLUEDROID_ENABLED
    if (s_service.bt_host_ready) {
        return ESP_OK;
    }
    esp_bt_audio_host_bluedroid_cfg_t host_cfg = ESP_BT_AUDIO_HOST_BLUEDROID_CFG_DEFAULT();
    (void)snprintf(host_cfg.dev_name, sizeof(host_cfg.dev_name), "%s", music_bt_device_name());
    esp_bt_audio_config_t bt_cfg = {
        .host_config = &host_cfg,
        .event_cb = music_bt_audio_event_cb,
        .event_user_ctx = &s_service,
        .classic.roles = ESP_BT_AUDIO_CLASSIC_ROLE_A2DP_SNK |
                         ESP_BT_AUDIO_CLASSIC_ROLE_AVRC_CT |
                         ESP_BT_AUDIO_CLASSIC_ROLE_AVRC_TG,
    };
    esp_err_t ret = esp_bt_audio_init(&bt_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Bluetooth host start failed: %s", esp_err_to_name(ret));
        return ret;
    }
    s_service.bt_host_ready = true;
    s_service.bt_audio_ready = true;
    s_service.bt_audio_initialized_owned = true;
    /** Clear callback quiescence when bringing up a fresh host. */
    portENTER_CRITICAL(&s_bt_event_guard);
    s_service.bt_callbacks_quiescing = false;
    portEXIT_CRITICAL(&s_bt_event_guard);
    ESP_LOGI(TAG, "Bluetooth host '%s' is up", music_bt_device_name());
    /** Log whether AVRCP cover-art support is enabled in this build. */
#if CONFIG_BT_AVRCP_CT_COVER_ART_ENABLED
    ESP_LOGI(TAG, "Classic Bluetooth cover art enabled: AVRCP 1.6, BIP/OBEX client built in");
#else
    ESP_LOGW(TAG, "Classic Bluetooth cover art disabled in this build; album art can not arrive");
#endif
    ret = esp_bt_audio_classic_set_scan_mode(true, true);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Bluetooth scan mode failed: %s", esp_err_to_name(ret));
    }
    return ESP_OK;
#else
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

static void music_bt_host_stop_locked(void)
{
    if (!s_service.bt_host_ready) {
        return;
    }
    esp_bt_audio_deinit();
    s_service.bt_host_ready = false;
    s_service.bt_audio_ready = false;
    s_service.bt_audio_initialized_owned = false;
}

/**
 * @brief Start the radio, discoverable host and audio profiles together.
 * @return ESP_OK on success, otherwise a Bluetooth power error.
 */
static esp_err_t music_bt_power_acquire_locked(void)
{
#if CONFIG_BT_ENABLED && CONFIG_BT_CLASSIC_ENABLED && CONFIG_BT_BLUEDROID_ENABLED
    esp_err_t ret = ESP_OK;
    music_bt_controller_state_sync_locked();
    if (!s_service.controller_ready) {
        esp_bt_controller_config_t controller_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
        ret = esp_bt_controller_init(&controller_cfg);
        music_bt_controller_state_sync_locked();
        if (s_service.controller_ready) s_service.controller_initialized_owned = true;
    }
    if (ret == ESP_OK && !s_service.controller_enabled) {
        ret = esp_bt_controller_enable(ESP_BT_MODE_CLASSIC_BT);
        music_bt_controller_state_sync_locked();
        if (s_service.controller_enabled) s_service.controller_enabled_owned = true;
    }
    if (ret == ESP_OK) {
        ret = music_bt_host_start_locked();
    }
    if (ret == ESP_OK) {
        s_service.bt_power_owned = true;
        memory_monitor_report("music-bt-power-on");
    }
    return ret;
#else
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

static esp_err_t music_bt_power_release_locked(void)
{
    esp_err_t first_error = ESP_OK;
    if (!music_bt_audio_inactive_locked()) {
        return ESP_ERR_INVALID_STATE;
    }
    if (music_bt_power_inactive_locked()) {
        /** Nothing to release; the stored user intent stays untouched. */
        s_service.bt_power_owned = false;
        s_service.bt_power_app_owned = false;
        return ESP_OK;
    }
#if CONFIG_BT_ENABLED
    music_bt_host_stop_locked();
    music_bt_controller_state_sync_locked();
    if (s_service.controller_enabled && s_service.controller_enabled_owned) {
        const esp_err_t ret = esp_bt_controller_disable();
        record_cleanup_error(&first_error, "disable Bluetooth controller", ret);
        music_bt_controller_state_sync_locked();
        if (ret == ESP_OK) s_service.controller_enabled_owned = false;
    }
    if (s_service.controller_ready && !s_service.controller_enabled &&
        s_service.controller_initialized_owned) {
        const esp_err_t ret = esp_bt_controller_deinit();
        record_cleanup_error(&first_error, "deinitialize Bluetooth controller", ret);
        music_bt_controller_state_sync_locked();
        if (ret == ESP_OK) s_service.controller_initialized_owned = false;
    }
#endif
    if (first_error == ESP_OK) {
        s_service.bt_power_owned = false;
        s_service.bt_power_persistent = false;
        s_service.bt_power_app_owned = false;
        memory_monitor_report("music-bt-power-off");
    }
    return first_error;
}

static esp_err_t music_bt_audio_acquire_locked(void)
{
#if CONFIG_BT_ENABLED && CONFIG_BT_CLASSIC_ENABLED && CONFIG_BT_BLUEDROID_ENABLED
    /** Attach playback resources to the existing Bluetooth host and profiles. */
    const esp_err_t ret = music_bt_host_start_locked();
    if (ret != ESP_OK) {
        return ret;
    }
    /** Enable page scan so a phone can connect to the Bluetooth source. */
    const esp_err_t scan_ret = esp_bt_audio_classic_set_scan_mode(true, true);
    if (scan_ret != ESP_OK) {
        ESP_LOGW(TAG, "Bluetooth scan mode failed: %s", esp_err_to_name(scan_ret));
    }
    return ESP_OK;
#else
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

/**
 * @brief Power plus the audio session: the Music app's Bluetooth source needs both.
 * @return ESP_OK on success, otherwise a Bluetooth initialization error.
 */
static esp_err_t music_bt_stack_init_locked(void)
{
    const bool was_powered = s_service.controller_ready || s_service.controller_enabled;
    esp_err_t ret = music_bt_power_acquire_locked();
    if (ret == ESP_OK) {
        if (!was_powered && !s_service.bt_power_persistent) {
            /** The app asked for Bluetooth only to play this source: release the radio again when the source is handed back to the SD card. */
            s_service.bt_power_app_owned = true;
        }
        ret = music_bt_audio_acquire_locked();
    }
    return ret;
}

static uint32_t parse_bt_duration_ms(const uint8_t *value, uint32_t length)
{
    uint32_t first = 0U;
    uint32_t second = 0U;
    bool colon = false;
    bool have_digit = false;

    if (value == NULL) {
        return 0U;
    }
    for (uint32_t i = 0U; i < length; ++i) {
        const uint8_t ch = value[i];
        if (ch == ':') {
            if (colon) {
                return 0U;
            }
            colon = true;
            continue;
        }
        if (ch < '0' || ch > '9') {
            continue;
        }
        have_digit = true;
        uint32_t *target = colon ? &second : &first;
        if (*target > (UINT32_MAX - (uint32_t)(ch - '0')) / 10U) {
            return 0U;
        }
        *target = *target * 10U + (uint32_t)(ch - '0');
    }
    if (!have_digit) {
        return 0U;
    }
    if (colon) {
        if (first > UINT32_MAX / 60U || first * 60U > UINT32_MAX - second) {
            return 0U;
        }
        const uint32_t total_seconds = first * 60U + second;
        return total_seconds > UINT32_MAX / 1000U ? 0U : total_seconds * 1000U;
    }
    return first;
}

static void music_sync_bt_metadata(const music_service_state_t *service,
                                   const music_bt_state_t *state)
{
    const music_player_media_update_t media = {
        .title = state->metadata.title,
        .artist = state->metadata.artist,
        .album = state->metadata.album,
        .elapsed_ms = service->bt_elapsed_ms,
        .duration_ms = service->bt_duration_ms,
        .update_position = false,
        .update_duration = false,
    };
    (void)music_player_set_metadata(&media);
}

static void bt_state_event(void *context, const music_bt_event_t *event,
                           const music_bt_state_t *state)
{
    music_service_state_t *service = context;

    if (service == NULL || state == NULL || event == NULL) {
        return;
    }
    const bool playback_changed = service->playing != state->playing;
    const bool media_reset = event->type == MUSIC_BT_EVENT_TRACK_CHANGED ||
                             event->type == MUSIC_BT_EVENT_DISCONNECTED;
    service->playing = state->playing;
    if (event->type == MUSIC_BT_EVENT_TRACK_CHANGED) {
        /** Retain cover art until a replacement image or disconnect arrives. */
        (void)music_player_set_track_index_preserve_media(++service->bt_track_index);
    } else if (event->type == MUSIC_BT_EVENT_DISCONNECTED) {
        service->bt_elapsed_ms = 0U;
        service->bt_duration_ms = 0U;
        /** The picture belongs to a track on a phone that is gone. */
        music_player_clear_cover_art();
        (void)music_player_reset_media();
    } else if (event->type == MUSIC_BT_EVENT_METADATA) {
        music_sync_bt_metadata(service, state);
    }
    if (playback_changed || media_reset) {
        (void)music_player_update_playback(state->playing);
    }
}

static void music_service_apply_event_locked(const music_service_event_t *event)
{
    music_service_state_t *service = &s_service;
    music_bt_event_t bt_event = {0};
    music_bt_event_result_t result = MUSIC_BT_EVENT_DROPPED;

    if (event == NULL || event->generation != s_service.event_generation) {
        return;
    }
    if (event->type == MUSIC_SERVICE_EVENT_SD_END ||
        event->type == MUSIC_SERVICE_EVENT_SD_ERROR) {
        /** Discard playback reports from a superseded track generation. */
        if (event->data.backend.source_generation != s_service.sd.generation) {
            return;
        }
        s_service.playing = false;
        if (event->type == MUSIC_SERVICE_EVENT_SD_END) {
            (void)music_player_on_backend_end(event->data.backend.detail);
        } else {
            (void)music_player_on_backend_error(event->data.backend.detail);
        }
        return;
    }
    if (event->bt_generation != music_bt_source_generation(&s_service.bt)) {
        return;
    }
    if (event->type == MUSIC_SERVICE_EVENT_BT_PLAYBACK &&
        (service->source_switching || service->bt_playback_events_suppressed)) {
        return;
    }
    bt_event.generation = event->bt_generation;
    bt_event.playback_status = MUSIC_BT_PLAYBACK_STATUS_UNKNOWN;
    switch (event->type) {
        case MUSIC_SERVICE_EVENT_BT_CONNECTION: {
            esp_bt_audio_event_connection_st_t connection_data = {
                .tech = ESP_BT_AUDIO_TECH_CLASSIC,
                .connected = event->data.connection.connected,
            };
            memcpy(connection_data.addr, event->data.connection.peer_addr, ESP_BD_ADDR_LEN);
            const esp_bt_audio_event_connection_st_t *connection = &connection_data;
            music_bt_record_connection_event(service, connection);
            bt_event.type = event->data.connection.connected ? MUSIC_BT_EVENT_CONNECTED : MUSIC_BT_EVENT_DISCONNECTED;
            (void)music_bt_source_on_event(&s_service.bt, &bt_event, &result);
            break;
        }
        case MUSIC_SERVICE_EVENT_BT_STREAM: {
            if (event->data.stream.state == ESP_BT_AUDIO_STREAM_STATE_ALLOCATED &&
                event->data.stream.sink) {
                const esp_err_t ret = esp_bt_audio_playback_reg_notifications(
                    MUSIC_BT_PLAYBACK_NOTIFICATION_MASK);
                if (ret != ESP_OK) {
                    ESP_LOGW(TAG, "Classic Bluetooth playback notifications failed: %s",
                             esp_err_to_name(ret));
                }
                (void)esp_bt_audio_playback_request_metadata(MUSIC_BT_METADATA_REQUEST_MASK);
            } else if (event->data.stream.state == ESP_BT_AUDIO_STREAM_STATE_STARTED) {
                bt_event.type = MUSIC_BT_EVENT_STREAM_STARTED;
                (void)music_bt_source_on_event(&s_service.bt, &bt_event, &result);
                audio_volume_state_t volume = {0};
                if (audio_volume_get(&volume) == ESP_OK) {
                    const esp_err_t notify_ret = esp_bt_audio_vol_notify(
                        volume.muted ? AUDIO_VOLUME_MIN : volume.level);
                    if (notify_ret != ESP_OK) {
                        ESP_LOGW(TAG, "initial Classic Bluetooth volume notify failed: %s",
                                 esp_err_to_name(notify_ret));
                    }
                }
            } else if (event->data.stream.state == ESP_BT_AUDIO_STREAM_STATE_STOPPED ||
                       event->data.stream.state == ESP_BT_AUDIO_STREAM_STATE_RELEASED) {
                bt_event.type = MUSIC_BT_EVENT_STREAM_STOPPED;
                (void)music_bt_source_on_event(&s_service.bt, &bt_event, &result);
            }
            break;
        }
        case MUSIC_SERVICE_EVENT_BT_PLAYBACK:
            if ((event->data.playback.events & ESP_BT_AUDIO_PLAYBACK_EVENT_PLAY_STATUS_CHANGE) != 0U) {
                ESP_LOGI(TAG, "Classic Bluetooth playback status=%u stream=%d",
                         (unsigned)event->data.playback.play_status,
                         (int)s_service.bt.state.stream);
            }
            if ((event->data.playback.events & ESP_BT_AUDIO_PLAYBACK_EVENT_TRACK_CHANGE) != 0U) {
                /** Clear art for a changed track and request its metadata. */
                bt_event.type = MUSIC_BT_EVENT_TRACK_CHANGED;
                (void)music_bt_source_on_event(&s_service.bt, &bt_event, &result);
                (void)esp_bt_audio_playback_request_metadata(MUSIC_BT_METADATA_REQUEST_MASK);
            } else if ((event->data.playback.events & ESP_BT_AUDIO_PLAYBACK_EVENT_NOW_PLAYING_CHANGE) != 0U) {
                /** Refresh repeated track metadata while retaining its cover art. */
                (void)esp_bt_audio_playback_request_metadata(MUSIC_BT_METADATA_REQUEST_MASK);
            }
            if ((event->data.playback.events & ESP_BT_AUDIO_PLAYBACK_EVENT_PLAY_STATUS_CHANGE) != 0U) {
                bt_event.type = MUSIC_BT_EVENT_PLAYBACK_STATUS;
                bt_event.playback_status = (music_bt_playback_status_t)event->data.playback.play_status;
                bt_event.playing = event->data.playback.play_status == ESP_BT_AUDIO_PLAYBACK_STATUS_PLAYING;
                (void)music_bt_source_on_event(&s_service.bt, &bt_event, &result);
            }
            if ((event->data.playback.events & ESP_BT_AUDIO_PLAYBACK_EVENT_PLAY_POS_CHANGED) != 0U) {
                s_service.bt_elapsed_ms = event->data.playback.position;
                (void)music_player_set_progress(s_service.bt_elapsed_ms, s_service.bt_duration_ms);
            }
            if ((event->data.playback.events & ESP_BT_AUDIO_PLAYBACK_EVENT_TRACK_REACHED_END) != 0U) {
                bt_event.type = MUSIC_BT_EVENT_PLAYBACK_STATUS;
                bt_event.playing = false;
                (void)music_bt_source_on_event(&s_service.bt, &bt_event, &result);
                s_service.playing = false;
                (void)music_player_on_backend_end("Bluetooth track reached end");
            }
            break;
        case MUSIC_SERVICE_EVENT_BT_VOLUME:
            if (event->data.volume.absolute) {
                if (audio_volume_apply_remote(event->data.volume.level,
                                              event->data.volume.muted) != ESP_OK) {
                    ESP_LOGW(TAG, "failed to apply remote absolute volume");
                }
            } else if (audio_volume_adjust_remote(event->data.volume.increase) != ESP_OK) {
                ESP_LOGW(TAG, "failed to apply remote relative volume");
            }
            break;
        case MUSIC_SERVICE_EVENT_BT_METADATA: {
            /** Cover art bypasses this text-event queue and is stored directly by the callback. */
            if (event->data.metadata.type == ESP_BT_AUDIO_PLAYBACK_METADATA_PLAYING_TIME) {
                const uint32_t duration_ms = parse_bt_duration_ms(event->data.metadata.value,
                                                                  event->data.metadata.length);
                if (duration_ms != 0U) {
                    s_service.bt_duration_ms = duration_ms;
                    const music_player_media_update_t duration = {
                        .duration_ms = duration_ms,
                        .update_duration = true,
                    };
                    (void)music_player_set_metadata(&duration);
                }
                break;
            }
            bt_event.type = MUSIC_BT_EVENT_METADATA;
            if (event->data.metadata.type == ESP_BT_AUDIO_PLAYBACK_METADATA_TITLE) {
                bt_event.title = (const char *)event->data.metadata.value;
            } else if (event->data.metadata.type == ESP_BT_AUDIO_PLAYBACK_METADATA_ARTIST) {
                bt_event.artist = (const char *)event->data.metadata.value;
            } else if (event->data.metadata.type == ESP_BT_AUDIO_PLAYBACK_METADATA_ALBUM) {
                bt_event.album = (const char *)event->data.metadata.value;
            } else {
                break;
            }
            (void)music_bt_source_on_event(&s_service.bt, &bt_event, &result);
            break;
        }
        default:
            break;
    }
}

static void music_service_task(void *context)
{
    music_service_state_t *service = context;
    music_service_event_t event;
    bool retry_queued_event = false;

    for (;;) {
        if (!retry_queued_event) {

            (void)ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        }
        retry_queued_event = false;
        if (service->service_task_stop) {
            break;
        }
        bool have_event;
        do {
            portENTER_CRITICAL(&s_bt_event_guard);
            have_event = service->event_queue_count != 0U;
            portEXIT_CRITICAL(&s_bt_event_guard);
            if (!have_event) {
                break;
            }
            if (xSemaphoreTake(service->lock, pdMS_TO_TICKS(MUSIC_SERVICE_LOCK_TIMEOUT_MS)) != pdTRUE) {
                portENTER_CRITICAL(&s_bt_event_guard);
                ++service->event_lock_timeout_count;
                portEXIT_CRITICAL(&s_bt_event_guard);
                retry_queued_event = true;
                break;
            }
            portENTER_CRITICAL(&s_bt_event_guard);
            have_event = service->event_queue_count != 0U;
            if (have_event) {
                event = service->event_queue[service->event_queue_head];
                service->event_queue_head =
                    (service->event_queue_head + 1U) % MUSIC_SERVICE_EVENT_QUEUE_CAPACITY;
                --service->event_queue_count;
            }
            portEXIT_CRITICAL(&s_bt_event_guard);
            if (have_event) {
                music_service_apply_event_locked(&event);
            }
            xSemaphoreGive(service->lock);
        } while (have_event);
        music_service_report_event_queue_drops(service);
        if (retry_queued_event) {
            vTaskDelay(pdMS_TO_TICKS(10U));
        }
    }
    vTaskDelete(NULL);
}

static void music_sd_service_event_cb(void *context, music_player_error_t error,
                                       uint32_t source_generation, const char *detail)
{
    music_service_state_t *service = context;
    /** Treat successful start reports as playback state, reserving error handling for failures and track end. */
    if (error == MUSIC_PLAYER_ERROR_OK) {
        return;
    }
    music_service_event_t event = {
        .type = error == MUSIC_PLAYER_ERROR_END_OF_TRACK ? MUSIC_SERVICE_EVENT_SD_END
                                                          : MUSIC_SERVICE_EVENT_SD_ERROR,
        .generation = service == NULL ? 0U : service->event_generation,
        .data.backend.error = error,
        .data.backend.source_generation = source_generation,
    };
    if (service == NULL) {
        return;
    }
    if (detail != NULL) {
        const size_t length = strlen(detail) < sizeof(event.data.backend.detail) - 1U
                                  ? strlen(detail) : sizeof(event.data.backend.detail) - 1U;
        memcpy(event.data.backend.detail, detail, length);
        event.data.backend.detail[length] = '\0';
    }
    (void)music_service_enqueue_event(service, &event);
}

static void music_bt_audio_event_cb(esp_bt_audio_event_t event, void *event_data, void *user_data)
{
    music_service_state_t *service = user_data;
    music_service_event_t mapped = {0};
    bool enqueue = false;

    if (service == NULL || !service->bt_audio_ready) {
        return;
    }
    if (!music_bt_event_begin(service, event)) {
        return;
    }
    mapped.generation = service->event_generation;
    mapped.bt_generation = music_bt_source_generation(&service->bt);
    switch (event) {
        case ESP_BT_AUDIO_EVENT_CONNECTION_STATE_CHG: {
            const esp_bt_audio_event_connection_st_t *connection = event_data;
            if (connection != NULL && connection->tech == ESP_BT_AUDIO_TECH_CLASSIC) {
                music_bt_record_connection_event(service, connection);
                mapped.type = MUSIC_SERVICE_EVENT_BT_CONNECTION;
                mapped.data.connection.connected = connection->connected;
                memcpy(mapped.data.connection.peer_addr, connection->addr, sizeof(mapped.data.connection.peer_addr));
                enqueue = true;
            }
            break;
        }
        case ESP_BT_AUDIO_EVENT_STREAM_STATE_CHG: {
            const esp_bt_audio_event_stream_st_t *stream = event_data;
            if (stream != NULL && stream->stream_handle != NULL) {
                mapped.type = MUSIC_SERVICE_EVENT_BT_STREAM;
                mapped.data.stream.handle = stream->stream_handle;
                mapped.data.stream.state = stream->state;
                esp_bt_audio_stream_dir_t direction = ESP_BT_AUDIO_STREAM_DIR_UNKNOWN;
                mapped.data.stream.sink =
                    esp_bt_audio_stream_get_dir(stream->stream_handle, &direction) == ESP_OK &&
                    direction == ESP_BT_AUDIO_STREAM_DIR_SINK;
                if (mapped.data.stream.sink) {
                    portENTER_CRITICAL(&s_bt_event_guard);
                    service->bt_sink_stream = stream->state == ESP_BT_AUDIO_STREAM_STATE_RELEASED
                                                  ? NULL
                                                  : stream->stream_handle;
                    service->bt_sink_stream_started =
                        stream->state == ESP_BT_AUDIO_STREAM_STATE_STARTED;
                    portEXIT_CRITICAL(&s_bt_event_guard);
                }
                /** Apply GMF stream transitions before the Bluetooth component destroys the stream handle. */
                if (service->bt_source_configured) {
                    const esp_err_t ret = music_bt_gmf_stream_state_serialized(service,
                                                                                 stream->stream_handle,
                                                                                 stream->state);
                    if (ret != ESP_OK) {
                        ESP_LOGE(TAG, "Classic Bluetooth GMF stream state %d failed: %s",
                                 stream->state, esp_err_to_name(ret));
                    }
                }
                mapped.data.stream.handle = NULL;
                /** Only the active source mirrors Bluetooth state into the player. */
                enqueue = service->bt_callbacks_enabled;
            }
            break;
        }
        case ESP_BT_AUDIO_EVENT_PLAYBACK_STATUS_CHG: {
            const esp_bt_audio_event_playback_st_t *status = event_data;
            if (status != NULL) {
                mapped.type = MUSIC_SERVICE_EVENT_BT_PLAYBACK;
                mapped.data.playback.events = status->event;
                mapped.data.playback.play_status = status->evt_param.play_status;
                if ((status->event & ESP_BT_AUDIO_PLAYBACK_EVENT_TRACK_REACHED_END) != 0U) {
                    mapped.data.playback.play_status = ESP_BT_AUDIO_PLAYBACK_STATUS_STOPPED;
                }
                mapped.data.playback.position = status->evt_param.position;
                enqueue = true;
            }
            break;
        }
        case ESP_BT_AUDIO_EVENT_PLAYBACK_METADATA: {
            const esp_bt_audio_event_playback_metadata_t *metadata = event_data;
            if (metadata != NULL && metadata->value != NULL && metadata->length != 0U) {
                if (metadata->type == ESP_BT_AUDIO_PLAYBACK_METADATA_COVER_ART) {
                    /** Copy the structured image payload directly into player storage. */
                    music_bt_store_cover_art(metadata);
                    break;
                }
                if (metadata->value[0] == '\0') {
                    break;
                }
                mapped.type = MUSIC_SERVICE_EVENT_BT_METADATA;
                mapped.data.metadata.type = metadata->type;
                mapped.data.metadata.length = metadata->length < sizeof(mapped.data.metadata.value) - 1U
                                             ? metadata->length : sizeof(mapped.data.metadata.value) - 1U;
                memcpy(mapped.data.metadata.value, metadata->value, mapped.data.metadata.length);
                mapped.data.metadata.value[mapped.data.metadata.length] = '\0';
                enqueue = true;
            }
            break;
        }
        case ESP_BT_AUDIO_EVENT_VOL_ABSOLUTE: {
            const esp_bt_audio_event_vol_absolute_t *volume = event_data;
            if (volume != NULL && volume->context == ESP_BT_AUDIO_STREAM_CONTEXT_MEDIA) {
                mapped.type = MUSIC_SERVICE_EVENT_BT_VOLUME;
                mapped.data.volume.absolute = true;
                mapped.data.volume.level = volume->vol;
                mapped.data.volume.muted = volume->mute;
                enqueue = true;
            }
            break;
        }
        case ESP_BT_AUDIO_EVENT_VOL_RELATIVE: {
            const esp_bt_audio_event_vol_relative_t *volume = event_data;
            if (volume != NULL && volume->context == ESP_BT_AUDIO_STREAM_CONTEXT_MEDIA) {
                mapped.type = MUSIC_SERVICE_EVENT_BT_VOLUME;
                mapped.data.volume.absolute = false;
                mapped.data.volume.increase = volume->up_down;
                enqueue = true;
            }
            break;
        }
        default:
            break;
    }
    if (enqueue) {
        (void)music_service_enqueue_event(service, &mapped);
    }
    music_bt_event_end(service);
}

static esp_err_t music_bt_source_init_locked(void)
{
    if (s_service.bt_source_configured) return ESP_OK;
    esp_err_t ret = music_bt_stack_init_locked();
    if (ret != ESP_OK) {
        const esp_err_t cleanup_error = music_bt_cleanup_locked();
        return cleanup_error == ESP_OK ? ret : cleanup_error;
    }
    const music_bt_source_config_t config = {
        .backend = &s_bt_backend,
        .backend_context = &s_service,
        .event_cb = bt_state_event,
        .event_context = &s_service,
    };
    if (music_bt_source_init(&s_service.bt, &config) != MUSIC_PLAYER_ERROR_OK) {
        ret = ESP_FAIL;
        const esp_err_t cleanup_error = music_bt_cleanup_locked();
        return cleanup_error == ESP_OK ? ret : cleanup_error;
    }
    s_service.bt_source_configured = true;
    if (music_bt_source_open(&s_service.bt) != MUSIC_PLAYER_ERROR_OK) {
        ret = ESP_FAIL;
        const esp_err_t cleanup_error = music_bt_cleanup_locked();
        return cleanup_error == ESP_OK ? ret : cleanup_error;
    }
    portENTER_CRITICAL(&s_bt_event_guard);
    s_service.bt_callbacks_enabled = true;
    s_service.bt_callbacks_quiescing = false;
    esp_bt_audio_stream_handle_t live_stream =
        s_service.bt_sink_stream_started ? s_service.bt_sink_stream : NULL;
    portEXIT_CRITICAL(&s_bt_event_guard);
    /** Request metadata and cover art when re-entering the Bluetooth source. */
    (void)esp_bt_audio_playback_reg_notifications(MUSIC_BT_PLAYBACK_NOTIFICATION_MASK);
    (void)esp_bt_audio_playback_request_metadata(MUSIC_BT_METADATA_REQUEST_MASK);
    /** Attach the new pipeline to an already active A2DP stream. */
    if (live_stream != NULL) {
        const esp_err_t attach =
            music_bt_gmf_stream_state_serialized(&s_service, live_stream,
                                                 ESP_BT_AUDIO_STREAM_STATE_ALLOCATED);
        if (attach == ESP_OK) {
            (void)music_bt_gmf_stream_state_serialized(&s_service, live_stream,
                                                       ESP_BT_AUDIO_STREAM_STATE_STARTED);
        } else {
            ESP_LOGW(TAG, "re-attach live Bluetooth stream failed: %s", esp_err_to_name(attach));
        }
    }
    memory_monitor_report("classic-bt-open");
    return ESP_OK;
}

static esp_err_t music_bt_cleanup_locked(void)
{
    esp_err_t first_error = ESP_OK;

    /** Close only the playback pipeline when leaving a source whose Bluetooth power is owned by settings. */
    if (s_service.bt_power_persistent) {
        if (s_service.bt_source_configured) {
            /** Ask the phone to pause before handing playback to the local source. */
            const esp_err_t pause_ret = esp_bt_audio_playback_pause();
            if (pause_ret != ESP_OK) {
                ESP_LOGW(TAG, "pause the remote Classic Bluetooth player failed: %s",
                         esp_err_to_name(pause_ret));
            } else {
                ESP_LOGI(TAG, "remote Classic Bluetooth playback paused for the local source");
            }
            const esp_err_t ret = player_error(music_bt_source_close(&s_service.bt));
            record_cleanup_error(&first_error, "close Classic Bluetooth GMF source", ret);
            s_service.bt_source_configured = false;
        }
        memory_monitor_report("music-bt-power-retained");
        return first_error;
    }

    bool peer_connected = false;
    bool teardown_safe = true;
    uint8_t peer_addr[ESP_BD_ADDR_LEN] = {0};

    /** Close the playback pipeline before attempting link teardown. */
    if (s_service.bt_source_configured) {
        const esp_err_t ret = player_error(music_bt_source_close(&s_service.bt));
        record_cleanup_error(&first_error, "close Classic Bluetooth GMF source", ret);
        s_service.bt_source_configured = false;
    }

    if (s_service.bt_audio_ready) {
        /** A failed cleanup leaves the stack alive; make the next cleanup attempt able to observe peers. */
        portENTER_CRITICAL(&s_bt_event_guard);
        if (!s_service.bt_callbacks_enabled && s_service.bt_callbacks_in_flight == 0U) {
            s_service.bt_callbacks_enabled = s_service.bt_source_configured;
            s_service.bt_callbacks_quiescing = false;
        }
        portEXIT_CRITICAL(&s_bt_event_guard);

        const esp_err_t ret = esp_bt_audio_classic_set_scan_mode(false, false);
        record_cleanup_error(&first_error, "disable Classic Bluetooth scan mode", ret);

        while (s_service.bt_disconnect_sem != NULL &&
               xSemaphoreTake(s_service.bt_disconnect_sem, 0U) == pdTRUE) {
        }
        portENTER_CRITICAL(&s_bt_event_guard);
        peer_connected = s_service.bt_peer_valid && s_service.bt_peer_connected;
        if (peer_connected) {
            (void)memcpy(peer_addr, s_service.bt_peer_addr, sizeof(peer_addr));
        }
        s_service.bt_disconnect_waiting = peer_connected;
        portEXIT_CRITICAL(&s_bt_event_guard);

        if (peer_connected) {
            const esp_err_t disconnect_ret =
                esp_bt_audio_classic_disconnect(ESP_BT_AUDIO_CLASSIC_ROLE_A2DP_SNK, peer_addr);
            if (disconnect_ret != ESP_OK) {
                record_cleanup_error(&first_error, "disconnect Classic Bluetooth A2DP peer", disconnect_ret);
                teardown_safe = false;
            } else {
                const TickType_t disconnect_timeout = pdMS_TO_TICKS(MUSIC_BT_DISCONNECT_TIMEOUT_MS) == 0U
                                                           ? 1U : pdMS_TO_TICKS(MUSIC_BT_DISCONNECT_TIMEOUT_MS);
                const BaseType_t disconnect_wait_ret =
                    s_service.bt_disconnect_sem != NULL
                        ? xSemaphoreTake(s_service.bt_disconnect_sem, disconnect_timeout)
                        : pdFALSE;
                if (disconnect_wait_ret != pdTRUE) {
                    if (first_error == ESP_OK) {
                        first_error = ESP_ERR_TIMEOUT;
                    }
                    ESP_LOGW(TAG, "timed out waiting for Classic Bluetooth A2DP disconnect");
                    teardown_safe = false;
                }
            }
            portENTER_CRITICAL(&s_bt_event_guard);
            if (s_service.bt_peer_connected) {
                teardown_safe = false;
            }
            s_service.bt_disconnect_waiting = false;
            portEXIT_CRITICAL(&s_bt_event_guard);
        } else {
            portENTER_CRITICAL(&s_bt_event_guard);
            s_service.bt_disconnect_waiting = false;
            portEXIT_CRITICAL(&s_bt_event_guard);
        }
    }

    if (!teardown_safe) {
        ESP_LOGE(TAG, "keeping Bluetooth audio/controller alive after unconfirmed A2DP teardown");
        return first_error;
    }

    const esp_err_t callback_ret = music_bt_disable_callbacks_locked();
    record_cleanup_error(&first_error, "quiesce Classic Bluetooth callbacks", callback_ret);
    if (callback_ret != ESP_OK) {
        return first_error;
    }
    portENTER_CRITICAL(&s_bt_event_guard);
    peer_connected = s_service.bt_peer_valid && s_service.bt_peer_connected;
    if (peer_connected) {
        s_service.bt_callbacks_enabled = s_service.bt_source_configured;
        s_service.bt_callbacks_quiescing = false;
    }
    portEXIT_CRITICAL(&s_bt_event_guard);
    if (peer_connected) {
        record_cleanup_error(&first_error, "confirm no Classic Bluetooth peer after callback quiesce",
                             ESP_ERR_INVALID_STATE);
        return first_error;
    }
    if (s_service.bt_audio_ready && !teardown_safe) {
        return first_error;
    }
    /** The playback pipeline is gone and the settings switch is off (or the app powered Bluetooth up on its own): release the host and the radio. */
    music_bt_host_stop_locked();
    const esp_err_t power_ret = music_bt_power_release_locked();
    record_cleanup_error(&first_error, "release Classic Bluetooth power", power_ret);
    music_bt_clear_peer_state();
    return first_error;
}

static esp_err_t music_sd_close_locked(void)
{
    if (!s_service.sd_source_opened) return ESP_OK;
    const esp_err_t ret = player_error(music_sd_source_close(&s_service.sd));
    /** music_sd_source_close clears its lifecycle state even when cleanup reports an error. */
    s_service.sd_source_opened = false;
    s_service.sd_track_loaded = false;
    return ret;
}

static esp_err_t music_service_cleanup_locked(void)
{
    esp_err_t first_error = ESP_OK;
    const esp_err_t bt_cleanup = music_bt_cleanup_locked();
    record_cleanup_error(&first_error, "close Classic Bluetooth state", bt_cleanup);
    if (bt_cleanup == ESP_OK) music_service_stop_task_locked();
    if (s_service.player_opened && bt_cleanup == ESP_OK) {
        const esp_err_t ret = player_error(music_player_close());
        record_cleanup_error(&first_error, "close unified music player state", ret);
        if (ret == ESP_OK) {
            s_service.player_opened = false;
        }
    }
    music_player_clear_cover_art();
    const esp_err_t sd_ret = music_sd_close_locked();
    record_cleanup_error(&first_error, "close SD-card GMF source", sd_ret);
    music_volume_listener_unregister_locked();
    if (s_service.audio_runtime_acquired) {
        (void)audio_runtime_release();
        s_service.audio_runtime_acquired = false;
    }
    s_service.dac = NULL;
    s_service.pool = NULL;
    /** The SD card is globally mounted and must outlive Music source switches and app exit. */
    return first_error;
}

static esp_err_t music_sd_open_locked(void)
{
    if (s_service.sd_source_opened) return ESP_OK;
    /** NULL/0 selects the default local roots: the SD card, then the built-in SPIFFS music directory shipped in the assets partition. */
    const music_player_error_t ret = music_sd_source_open(&s_service.sd, s_service.pool,
                                                           s_service.dac, NULL, 0U,
                                                           music_sd_service_event_cb, &s_service);
    if (ret != MUSIC_PLAYER_ERROR_OK) return ESP_FAIL;
    s_service.sd_source_opened = true;
    return ESP_OK;
}

/**
 * @brief Update local service playback and the shared player state together.
 * @param playing Whether SD playback is active.
 */
static void music_sd_set_playing_locked(bool playing)
{
    s_service.playing = playing;
    (void)music_player_update_playback(playing);
}

static esp_err_t music_sd_play_locked(size_t index)
{
    if (music_sd_open_locked() != ESP_OK) return ESP_FAIL;
    const music_player_error_t ret = music_sd_source_start(&s_service.sd, index);
    if (ret == MUSIC_PLAYER_ERROR_OK) {
        s_service.sd_track_loaded = true;
        const music_sd_catalog_entry_t *entry = &s_service.sd.catalog.entries[index];
        const music_player_media_update_t media = {
            .title = entry->title,
            .artist = entry->artist,
            .elapsed_ms = 0U,
            .duration_ms = entry->duration_ms,
            .update_position = true,
            .update_duration = true,
        };
        (void)music_player_set_track_index((uint32_t)index);
        (void)music_player_set_metadata(&media);
        /** Last: selecting a track resets the mirror to PAUSED. */
        music_sd_set_playing_locked(true);
    }
    return player_error(ret == MUSIC_PLAYER_ERROR_OK ? ESP_OK : ESP_FAIL);
}

esp_err_t music_player_service_enter(void)
{
    SemaphoreHandle_t lock = music_service_lock();
    if (lock == NULL) return ESP_ERR_NO_MEM;
    if (xSemaphoreTake(lock, pdMS_TO_TICKS(MUSIC_SERVICE_LOCK_TIMEOUT_MS)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    if (s_service.exit_pending) {
        const esp_err_t retry = music_service_cleanup_locked();
        if (retry != ESP_OK) {
            xSemaphoreGive(lock);
            return retry;
        }
        s_service.exit_pending = false;
        ++s_service.event_generation;
    }
    if (s_service.entered) {
        xSemaphoreGive(lock);
        return ESP_OK;
    }
    if (s_service.event_generation == 0U) {
        s_service.event_generation = 1U;
    }
    esp_err_t ret = music_audio_runtime_check_locked();
    s_service.bt_playback_events_suppressed = false;
    if (ret == ESP_OK) {
        ret = music_volume_listener_register_locked();
    }
    if (ret == ESP_OK) {
        ret = audio_runtime_acquire();
        if (ret == ESP_OK) {
            s_service.audio_runtime_acquired = true;
        }
    }
    /** The board manager mounts FS_SDCARD globally at boot; Music only consumes /sdcard/music. */
    if (ret == ESP_OK) {
        music_player_error_t player_ret = MUSIC_PLAYER_ERROR_OK;
        if (!s_service.player_initialized) {
            player_ret = music_player_init(&s_player_backend);
            if (player_ret == MUSIC_PLAYER_ERROR_OK) {
                s_service.player_initialized = true;
            }
        }
        if (player_ret == MUSIC_PLAYER_ERROR_OK && !s_service.player_opened) {
            player_ret = music_player_open();
            if (player_ret == MUSIC_PLAYER_ERROR_OK) {
                s_service.player_opened = true;
            }
        }
        if (player_ret != MUSIC_PLAYER_ERROR_OK) {
            ret = ESP_FAIL;
        }
    }
    if (ret == ESP_OK) {
        s_service.entered = true;
        ret = music_service_start_task_locked();
        if (ret != ESP_OK) s_service.entered = false;
    }
    if (ret != ESP_OK) {
        const esp_err_t enter_error = ret;
        const esp_err_t cleanup_error = music_service_cleanup_locked();
        s_service.exit_pending = cleanup_error != ESP_OK;
        ret = cleanup_error == ESP_OK ? enter_error : cleanup_error;
        ESP_LOGE(TAG, "enter failed: %s", esp_err_to_name(ret));
    } else {
        s_service.entered = true;
        ESP_LOGI(TAG, "entered: source=%s", s_service.source == MUSIC_PLAYER_SOURCE_SD_CARD ?
                 "sd-card" : "classic-bt");
        memory_monitor_report("music-enter");
    }
    xSemaphoreGive(lock);
    return ret;
}

esp_err_t music_player_service_return_home(void)
{
    SemaphoreHandle_t lock = music_service_lock();
    if (lock == NULL) {
        return ESP_ERR_NO_MEM;
    }
    if (xSemaphoreTake(lock, pdMS_TO_TICKS(MUSIC_SERVICE_LOCK_TIMEOUT_MS)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    const bool was_bt_source = s_service.entered &&
                               s_service.source == MUSIC_PLAYER_SOURCE_CLASSIC_BT;
    if (was_bt_source) {
        /** Disable late AVRCP playback updates during the handoff to SD. */
        s_service.bt_playback_events_suppressed = true;
        /** Save the active source before changing selection and tearing down its Bluetooth session. */
        s_service.playing = false;
        music_bt_state_set_playing(&s_service.bt.state, false);
        (void)music_player_update_playback(false);
    }
    xSemaphoreGive(lock);

    if (was_bt_source) {
        const esp_err_t source_ret =
            music_player_service_select_source(MUSIC_PLAYER_SOURCE_SD_CARD);
        if (source_ret != ESP_OK) {
            return source_ret;
        }
    }
    return music_player_service_exit();
}

esp_err_t music_player_service_exit(void)
{
    SemaphoreHandle_t lock = music_service_lock();
    if (lock == NULL) return ESP_OK;
    if (xSemaphoreTake(lock, pdMS_TO_TICKS(MUSIC_SERVICE_LOCK_TIMEOUT_MS)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    const esp_err_t cleanup_error = music_service_cleanup_locked();
    if (cleanup_error == ESP_OK) {
        s_service.playing = false;
        s_service.entered = false;
        s_service.source = MUSIC_PLAYER_SOURCE_SD_CARD;
        s_service.sd_track_loaded = false;
        s_service.player_initialized = false;
        s_service.bt_playback_events_suppressed = false;
        ++s_service.event_generation;
    } else {
        s_service.exit_pending = true;
        ESP_LOGE(TAG, "exit pending; retry required: %s", esp_err_to_name(cleanup_error));
    }
    xSemaphoreGive(lock);
    memory_monitor_report("music-exit");
    return cleanup_error;
}

static esp_err_t music_select_source_locked(music_player_source_t source)
{
    if (!s_service.entered || s_service.source_switching) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_service.playing) {
        return ESP_ERR_INVALID_STATE;
    }
    if (source == s_service.source) {
        return ESP_OK;
    }
    s_service.source_switching = true;
    s_service.bt_playback_events_suppressed = true;
    ++s_service.event_generation;

    /** Release the local GMF pipeline before registering the Bluetooth source; retain the shared SD mount. */
    bool restore_sd_source = false;
    if (source == MUSIC_PLAYER_SOURCE_CLASSIC_BT &&
        s_service.source == MUSIC_PLAYER_SOURCE_SD_CARD && s_service.sd_source_opened) {
        const esp_err_t ret = music_sd_close_locked();
        if (ret != ESP_OK) {
            const esp_err_t restore_ret = music_sd_open_locked();
            if (restore_ret != ESP_OK) {
                ESP_LOGE(TAG, "restore SD-card GMF source after close failure failed: %s",
                         esp_err_to_name(restore_ret));
            }
            s_service.bt_playback_events_suppressed = false;
            s_service.source_switching = false;
            return restore_ret == ESP_OK ? ret : restore_ret;
        }
        restore_sd_source = true;
    }

    if (source == MUSIC_PLAYER_SOURCE_CLASSIC_BT) {
        /** Start Bluetooth alongside Wi-Fi and return stack initialization failures to the caller. */
        const esp_err_t bt_ret = music_bt_source_init_locked();
        if (bt_ret != ESP_OK) {
            esp_err_t sd_rollback = ESP_OK;
            if (restore_sd_source) {
                sd_rollback = music_sd_open_locked();
            }
            if (sd_rollback != ESP_OK) {
                ESP_LOGE(TAG, "restore SD-card GMF source failed: %s", esp_err_to_name(sd_rollback));
            }
            s_service.bt_playback_events_suppressed = false;
            s_service.source_switching = false;
            return sd_rollback == ESP_OK ? bt_ret : sd_rollback;
        }
        if (music_player_select_source(source) != MUSIC_PLAYER_ERROR_OK) {
            const esp_err_t cleanup_ret = music_bt_cleanup_locked();
            const esp_err_t sd_ret = music_sd_open_locked();
            s_service.exit_pending = cleanup_ret != ESP_OK || sd_ret != ESP_OK;
            s_service.bt_playback_events_suppressed = false;
            s_service.source_switching = false;
            return cleanup_ret != ESP_OK ? cleanup_ret : (sd_ret != ESP_OK ? sd_ret : ESP_FAIL);
        }
        s_service.source = source;
        s_service.bt_playback_events_suppressed = false;
        s_service.source_switching = false;
        memory_monitor_report("music-bt-source-ready");
        return ESP_OK;
    }

    /** Pause the phone and close its playback pipeline; settings controls remaining Bluetooth power. */
    const esp_err_t bt_ret = music_bt_cleanup_locked();
    if (!music_bt_pipeline_closed_locked()) {
        s_service.bt_playback_events_suppressed = false;
        s_service.source_switching = false;
        return bt_ret == ESP_OK ? ESP_ERR_INVALID_STATE : bt_ret;
    }
    if (bt_ret != ESP_OK) {
        /** Retain Bluetooth power state when teardown remains incomplete so cleanup can retry. */
        ESP_LOGW(TAG, "local source selected with a pending Bluetooth teardown: %s",
                 esp_err_to_name(bt_ret));
        s_service.exit_pending = true;
    }
    if (music_player_select_source(source) != MUSIC_PLAYER_ERROR_OK) {
        const esp_err_t bt_rollback = music_bt_source_init_locked();
        s_service.exit_pending = bt_rollback != ESP_OK;
        s_service.bt_playback_events_suppressed = false;
        s_service.source_switching = false;
        return bt_rollback != ESP_OK ? bt_rollback : ESP_FAIL;
    }
    s_service.source = source;
    /** Bluetooth reports never drive the player state while another source is selected, even if the phone stays connected. */
    s_service.bt_playback_events_suppressed = source != MUSIC_PLAYER_SOURCE_CLASSIC_BT;
    s_service.source_switching = false;
    return bt_ret;
}

esp_err_t music_player_service_select_source(music_player_source_t source)
{
    if (source != MUSIC_PLAYER_SOURCE_SD_CARD && source != MUSIC_PLAYER_SOURCE_CLASSIC_BT) {
        return ESP_ERR_INVALID_ARG;
    }
    SemaphoreHandle_t lock = music_service_lock();
    if (lock == NULL) return ESP_ERR_NO_MEM;
    if (xSemaphoreTake(lock, pdMS_TO_TICKS(MUSIC_SERVICE_LOCK_TIMEOUT_MS)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    const esp_err_t ret = music_select_source_locked(source);
    xSemaphoreGive(lock);
    return ret;
}

music_player_source_t music_player_service_get_source(void)
{
    SemaphoreHandle_t lock = music_service_lock();
    if (lock == NULL) return MUSIC_PLAYER_SOURCE_SD_CARD;
    if (xSemaphoreTake(lock, pdMS_TO_TICKS(MUSIC_SERVICE_LOCK_TIMEOUT_MS)) != pdTRUE) {
        return MUSIC_PLAYER_SOURCE_SD_CARD;
    }
    const music_player_source_t source = s_service.source;
    xSemaphoreGive(lock);
    return source;
}

static void music_bt_power_persist(bool enabled)
{
    const esp_err_t ret = music_bt_power_store_save(enabled);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "persist Bluetooth power %d failed: %s",
                 enabled ? 1 : 0, esp_err_to_name(ret));
    }
}

/**
 * @brief Apply the settings-owned Bluetooth power intent and hand active playback to SD before shutdown.
 * @param enabled Whether Bluetooth power is requested.
 * @return ESP_OK on success, otherwise an error code.
 */
esp_err_t music_player_service_set_bt_power(bool enabled)
{
    SemaphoreHandle_t lock = music_service_lock();
    if (lock == NULL) return ESP_ERR_NO_MEM;
    if (xSemaphoreTake(lock, pdMS_TO_TICKS(MUSIC_SERVICE_LOCK_TIMEOUT_MS)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    esp_err_t ret = ESP_OK;
    if (enabled) {
        /** Bring up the radio, discoverable host and A2DP/AVRCP roles. */
        const esp_err_t power_ret = music_bt_power_acquire_locked();
        if (power_ret == ESP_OK) {
            s_service.bt_power_persistent = true;
        } else {
            ret = power_ret;
            ESP_LOGE(TAG, "Bluetooth power on failed: %s", esp_err_to_name(power_ret));
        }
    } else {
        s_service.bt_power_persistent = false;
        if (s_service.entered && s_service.source == MUSIC_PLAYER_SOURCE_CLASSIC_BT) {
            if (s_service.playing) {
                s_service.playing = false;
                music_bt_state_set_playing(&s_service.bt.state, false);
                (void)music_player_update_playback(false);
            }
            const esp_err_t source_ret = music_select_source_locked(MUSIC_PLAYER_SOURCE_SD_CARD);
            if (source_ret != ESP_OK) {
                /** The source could not be handed back: keep Bluetooth alive so the visible state still matches the radio. */
                s_service.bt_power_persistent = true;
                ret = source_ret;
            }
        }
        if (ret == ESP_OK) {
            const esp_err_t cleanup_ret = music_bt_cleanup_locked();
            if (cleanup_ret != ESP_OK) {
                ESP_LOGE(TAG, "Bluetooth power off failed: %s", esp_err_to_name(cleanup_ret));
                ret = cleanup_ret;
            }
        }
    }
    if (ret == ESP_OK) {
        music_bt_power_persist(enabled);
        memory_monitor_report(enabled ? "music-bt-settings-on" : "music-bt-settings-off");
    }
    xSemaphoreGive(lock);
    return ret;
}

bool music_player_service_get_bt_power(void)
{
    SemaphoreHandle_t lock = music_service_lock();
    if (lock == NULL) return false;
    if (xSemaphoreTake(lock, pdMS_TO_TICKS(MUSIC_SERVICE_LOCK_TIMEOUT_MS)) != pdTRUE) {
        return false;
    }
    music_bt_controller_state_sync_locked();
    const bool powered = s_service.controller_enabled || s_service.controller_ready;
    xSemaphoreGive(lock);
    return powered;
}

bool music_player_service_bt_connected(void)
{
    bool connected;
    portENTER_CRITICAL(&s_bt_event_guard);
    connected = s_service.bt_peer_valid && s_service.bt_peer_connected;
    portEXIT_CRITICAL(&s_bt_event_guard);
    return connected;
}

void music_player_service_restore_bt_power(void)
{
    bool enabled = false;
    const esp_err_t stored = music_bt_power_store_load(&enabled);
    if (stored != ESP_OK) {
        if (stored != ESP_ERR_NOT_FOUND) {
            ESP_LOGW(TAG, "read stored Bluetooth power failed: %s", esp_err_to_name(stored));
        }
        return;
    }
    if (!enabled) {
        return;
    }
    const esp_err_t ret = music_player_service_set_bt_power(true);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "stored Bluetooth power is on but the radio did not start: %s",
                 esp_err_to_name(ret));
    } else {
        ESP_LOGI(TAG, "restored Bluetooth power: on");
    }
}

static esp_err_t music_playback_boundary_locked(const char *operation)
{
    if (s_service.source == MUSIC_PLAYER_SOURCE_SD_CARD) {
        const size_t count = s_service.sd.catalog.count;
        if (strcmp(operation, "play") == 0) {
            if (s_service.playing) return ESP_OK;
            if (s_service.sd_track_loaded) {
                const music_player_error_t ret = music_sd_source_resume(&s_service.sd);
                if (ret == MUSIC_PLAYER_ERROR_OK) {
                    music_sd_set_playing_locked(true);
                    return ESP_OK;
                }
            }
            return music_sd_play_locked(s_service.sd_track_loaded ? s_service.sd.current_index : 0U);
        }
        if (strcmp(operation, "pause") == 0) {
            if (!s_service.playing) return ESP_OK;
            const music_player_error_t ret = music_sd_source_pause(&s_service.sd);
            if (ret == MUSIC_PLAYER_ERROR_OK) { music_sd_set_playing_locked(false); }
            return player_error(ret == MUSIC_PLAYER_ERROR_OK ? ESP_OK : ESP_FAIL);
        }
        if (count == 0U) return ESP_ERR_NOT_FOUND;
        const size_t offset = strcmp(operation, "next") == 0 ? 1U : count - 1U;
        return music_sd_play_locked((s_service.sd.current_index + offset) % count);
    }
    if (!s_service.bt_source_configured) return ESP_ERR_INVALID_STATE;
    music_bt_avrcp_command_t command = MUSIC_BT_AVRCP_PLAY_PAUSE;
    if (strcmp(operation, "next") == 0) command = MUSIC_BT_AVRCP_NEXT;
    if (strcmp(operation, "previous") == 0) command = MUSIC_BT_AVRCP_PREVIOUS;
    const music_player_error_t ret = music_bt_source_command(&s_service.bt, command);
    return player_error(ret == MUSIC_PLAYER_ERROR_OK ? ESP_OK : ESP_FAIL);
}

esp_err_t music_player_service_play(void)
{
    SemaphoreHandle_t lock = music_service_lock();
    if (lock == NULL) return ESP_ERR_NO_MEM;
    if (xSemaphoreTake(lock, pdMS_TO_TICKS(MUSIC_SERVICE_LOCK_TIMEOUT_MS)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    const esp_err_t ret = !s_service.entered ? ESP_ERR_INVALID_STATE : music_playback_boundary_locked("play");
    xSemaphoreGive(lock);
    return ret;
}

esp_err_t music_player_service_pause(void)
{
    SemaphoreHandle_t lock = music_service_lock();
    if (lock == NULL) return ESP_ERR_NO_MEM;
    if (xSemaphoreTake(lock, pdMS_TO_TICKS(MUSIC_SERVICE_LOCK_TIMEOUT_MS)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    const esp_err_t ret = !s_service.entered ? ESP_ERR_INVALID_STATE : music_playback_boundary_locked("pause");
    xSemaphoreGive(lock);
    return ret;
}

esp_err_t music_player_service_next(void)
{
    SemaphoreHandle_t lock = music_service_lock();
    if (lock == NULL) return ESP_ERR_NO_MEM;
    if (xSemaphoreTake(lock, pdMS_TO_TICKS(MUSIC_SERVICE_LOCK_TIMEOUT_MS)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    const esp_err_t ret = !s_service.entered ? ESP_ERR_INVALID_STATE : music_playback_boundary_locked("next");
    xSemaphoreGive(lock);
    return ret;
}

esp_err_t music_player_service_previous(void)
{
    SemaphoreHandle_t lock = music_service_lock();
    if (lock == NULL) return ESP_ERR_NO_MEM;
    if (xSemaphoreTake(lock, pdMS_TO_TICKS(MUSIC_SERVICE_LOCK_TIMEOUT_MS)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    const esp_err_t ret = !s_service.entered ? ESP_ERR_INVALID_STATE : music_playback_boundary_locked("previous");
    xSemaphoreGive(lock);
    return ret;
}

bool music_player_service_is_playing(void)
{
    SemaphoreHandle_t lock = music_service_lock();
    if (lock == NULL) return false;
    if (xSemaphoreTake(lock, pdMS_TO_TICKS(MUSIC_SERVICE_LOCK_TIMEOUT_MS)) != pdTRUE) {
        return false;
    }
    const bool playing = s_service.source == MUSIC_PLAYER_SOURCE_CLASSIC_BT ?
                         s_service.bt.state.playing : s_service.playing;
    xSemaphoreGive(lock);
    return playing;
}
esp_err_t music_player_service_set_volume(uint8_t level)
{
    return audio_volume_set(level);
}

esp_err_t music_player_service_get_volume(uint8_t *level, bool *muted)
{
    if (level == NULL || muted == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    audio_volume_state_t state = {0};
    const esp_err_t ret = audio_volume_get(&state);
    if (ret != ESP_OK) {
        return ret;
    }
    *level = state.level;
    *muted = state.muted;
    return ESP_OK;
}

esp_err_t music_player_service_set_mute(bool muted)
{
    return audio_volume_set_mute(muted);
}

void music_player_service_restore_volume(void)
{
    uint8_t level = 0U;
    bool muted = false;
    const esp_err_t stored = music_volume_store_load(&level, &muted);
    if (stored != ESP_OK) {
        if (stored != ESP_ERR_NOT_FOUND) {
            ESP_LOGW(TAG, "read stored volume failed: %s", esp_err_to_name(stored));
        }
        return;
    }

    esp_err_t ret = music_player_service_set_volume(level);
    if (ret == ESP_OK) {
        ret = music_player_service_set_mute(muted);
    }
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "stored volume %u (muted=%d) not applied: %s", (unsigned)level,
                 muted ? 1 : 0, esp_err_to_name(ret));
        return;
    }
    ESP_LOGI(TAG, "restored volume %u (muted=%d)", (unsigned)level, muted ? 1 : 0);
}

esp_err_t music_player_service_save_volume(void)
{
    uint8_t level = 0U;
    bool muted = false;
    const esp_err_t ret = music_player_service_get_volume(&level, &muted);
    if (ret != ESP_OK) {
        return ret;
    }
    return music_volume_store_save(level, muted);
}
