#if !defined(ESP_PLATFORM)
#define _POSIX_C_SOURCE 200809L
#endif

#include "music_player.h"
#include "music_player_internal.h"

#include <string.h>

#if defined(ESP_PLATFORM)
#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#else
#include <errno.h>
#include <stdlib.h>
#include <pthread.h>
#include <time.h>
#endif

#define MUSIC_PLAYER_DEFAULT_LOCK_TIMEOUT_MS 100U
#define MUSIC_PLAYER_COVER_ART_LOCK_TIMEOUT_MS 2000U

/** Lazily allocate up to 128 KB of cover art; all access holds the player lock. */
static uint8_t *s_cover_art;
static size_t s_cover_art_size;
static uint32_t s_cover_art_format;
static uint32_t s_cover_art_revision;
#if defined(ESP_PLATFORM)
static StaticSemaphore_t s_cover_art_lock_storage;
static SemaphoreHandle_t s_cover_art_lock_handle;
static portMUX_TYPE s_cover_art_init_guard = portMUX_INITIALIZER_UNLOCKED;
#endif
static bool s_process_global_initialized;
#if defined(ESP_PLATFORM)
/** Player state is task-accessed and protected by its own mutex. */
EXT_RAM_BSS_ATTR static music_player_controller_t s_default_controller;
#else
static music_player_controller_t s_default_controller;
#endif
#if defined(ESP_PLATFORM)
static StaticSemaphore_t s_default_controller_lock_storage;
static SemaphoreHandle_t s_default_controller_lock_handle;
static portMUX_TYPE s_default_controller_lock_init_guard = portMUX_INITIALIZER_UNLOCKED;
#else
static pthread_mutex_t s_default_controller_lock = PTHREAD_MUTEX_INITIALIZER;
#endif

#if defined(ESP_PLATFORM)
static SemaphoreHandle_t default_controller_mutex(void)
{
    portENTER_CRITICAL(&s_default_controller_lock_init_guard);
    if (s_default_controller_lock_handle == NULL) {
        s_default_controller_lock_handle = xSemaphoreCreateMutexStatic(&s_default_controller_lock_storage);
    }
    SemaphoreHandle_t handle = s_default_controller_lock_handle;
    portEXIT_CRITICAL(&s_default_controller_lock_init_guard);
    return handle;
}
#endif

static bool default_controller_lock(void)
{
#if defined(ESP_PLATFORM)
    SemaphoreHandle_t lock = default_controller_mutex();
    return lock != NULL &&
           xSemaphoreTake(lock, pdMS_TO_TICKS(MUSIC_PLAYER_DEFAULT_LOCK_TIMEOUT_MS)) == pdTRUE;
#else
    struct timespec deadline;
    if (clock_gettime(CLOCK_REALTIME, &deadline) != 0) {
        return false;
    }
    deadline.tv_sec += MUSIC_PLAYER_DEFAULT_LOCK_TIMEOUT_MS / 1000U;
    deadline.tv_nsec += (long)(MUSIC_PLAYER_DEFAULT_LOCK_TIMEOUT_MS % 1000U) * 1000000L;
    if (deadline.tv_nsec >= 1000000000L) {
        ++deadline.tv_sec;
        deadline.tv_nsec -= 1000000000L;
    }

    int result;
    do {
        result = pthread_mutex_timedlock(&s_default_controller_lock, &deadline);
    } while (result == EINTR);
    return result == 0;
#endif
}

static void default_controller_unlock(void)
{
#if defined(ESP_PLATFORM)
    (void)xSemaphoreGive(s_default_controller_lock_handle);
#else
    (void)pthread_mutex_unlock(&s_default_controller_lock);
#endif
}

#if defined(ESP_PLATFORM)
static SemaphoreHandle_t cover_art_lock(void)
{
    portENTER_CRITICAL(&s_cover_art_init_guard);
    if (s_cover_art_lock_handle == NULL) {
        s_cover_art_lock_handle = xSemaphoreCreateMutexStatic(&s_cover_art_lock_storage);
    }
    SemaphoreHandle_t handle = s_cover_art_lock_handle;
    portEXIT_CRITICAL(&s_cover_art_init_guard);
    return handle;
}
#endif

static void cover_art_snapshot(music_player_cover_art_descriptor_t *descriptor)
{
    if (descriptor == NULL) {
        return;
    }
#if defined(ESP_PLATFORM)
    SemaphoreHandle_t lock = cover_art_lock();
    if (lock == NULL ||
        xSemaphoreTake(lock, pdMS_TO_TICKS(MUSIC_PLAYER_COVER_ART_LOCK_TIMEOUT_MS)) != pdTRUE) {
        (void)memset(descriptor, 0, sizeof(*descriptor));
        return;
    }
#endif
    descriptor->available = s_cover_art_size != 0U;
    descriptor->format_fourcc = s_cover_art_format;
    descriptor->size = (uint32_t)s_cover_art_size;
    descriptor->revision = s_cover_art_revision;
#if defined(ESP_PLATFORM)
    if (lock != NULL) {
        (void)xSemaphoreGive(lock);
    }
#endif
}

static bool valid_source(music_player_source_t source)
{
    return source >= MUSIC_PLAYER_SOURCE_SD_CARD && source < MUSIC_PLAYER_SOURCE_COUNT;
}

static music_player_error_t record_backend_failure(music_player_controller_t *controller,
                                                     music_player_error_t result,
                                                     const char *detail)
{
    const music_player_state_event_t event = {
        .type = MUSIC_PLAYER_STATE_EVENT_BACKEND_ERROR,
        .error = result,
        .detail = detail,
    };
    if (controller->state.open) {
        (void)music_player_state_reduce(&controller->state, &event);
    } else {
        (void)music_player_state_record_error(&controller->state, result, detail);
    }
    return result;
}

static music_player_error_t call_backend(music_player_controller_t *controller,
                                         music_player_backend_fn callback,
                                         const char *failure_detail)
{
    const music_player_error_t result = callback == NULL ? MUSIC_PLAYER_ERROR_OK : callback(controller->backend.context);
    return result == MUSIC_PLAYER_ERROR_OK ? result : record_backend_failure(controller, result, failure_detail);
}

music_player_error_t music_player_controller_init(music_player_controller_t *controller,
                                                    const music_player_backend_t *backend)
{
    if (controller == NULL || backend == NULL) {
        return MUSIC_PLAYER_ERROR_INVALID_ARGUMENT;
    }
    if (controller->state.initialized) {
        return MUSIC_PLAYER_ERROR_OK;
    }

    music_player_state_reset(&controller->state);
    controller->backend = *backend;
    controller->operation_active = false;
    if (!s_process_global_initialized && backend->global_init != NULL) {
        const music_player_error_t result = backend->global_init(backend->context);
        if (result != MUSIC_PLAYER_ERROR_OK) {
            (void)music_player_state_record_error(&controller->state, result, "global init failed");
            return result;
        }
    }
    s_process_global_initialized = true;
    controller->process_global_initialized = true;
    controller->state.initialized = true;
    return MUSIC_PLAYER_ERROR_OK;
}

music_player_error_t music_player_controller_open(music_player_controller_t *controller)
{
    if (controller == NULL) {
        return MUSIC_PLAYER_ERROR_INVALID_ARGUMENT;
    }
    if (!controller->state.initialized) {
        return MUSIC_PLAYER_ERROR_NOT_INITIALIZED;
    }
    if (controller->state.open) {
        return MUSIC_PLAYER_ERROR_OK;
    }
    if (controller->operation_active) {
        return MUSIC_PLAYER_ERROR_BUSY;
    }

    controller->operation_active = true;
    const music_player_error_t result = call_backend(controller, controller->backend.open, "open failed");
    if (result == MUSIC_PLAYER_ERROR_OK) {
        const music_player_state_event_t event = {.type = MUSIC_PLAYER_STATE_EVENT_OPEN};
        (void)music_player_state_reduce(&controller->state, &event);
    }
    controller->operation_active = false;
    return result;
}

music_player_error_t music_player_controller_close(music_player_controller_t *controller)
{
    if (controller == NULL) {
        return MUSIC_PLAYER_ERROR_INVALID_ARGUMENT;
    }
    if (!controller->state.initialized) {
        return MUSIC_PLAYER_ERROR_NOT_INITIALIZED;
    }
    if (!controller->state.open) {
        return MUSIC_PLAYER_ERROR_OK;
    }
    if (controller->operation_active) {
        return MUSIC_PLAYER_ERROR_BUSY;
    }

    controller->operation_active = true;
    const music_player_error_t result = call_backend(controller, controller->backend.close, "close failed");
    if (result == MUSIC_PLAYER_ERROR_OK) {
        const music_player_state_event_t event = {.type = MUSIC_PLAYER_STATE_EVENT_CLOSE};
        (void)music_player_state_reduce(&controller->state, &event);
    }
    controller->operation_active = false;
    return result;
}

music_player_error_t music_player_controller_select_source(music_player_controller_t *controller,
                                                             music_player_source_t source)
{
    if (controller == NULL) {
        return MUSIC_PLAYER_ERROR_INVALID_ARGUMENT;
    }
    if (!valid_source(source)) {
        return MUSIC_PLAYER_ERROR_INVALID_SOURCE;
    }
    if (!controller->state.initialized) {
        return MUSIC_PLAYER_ERROR_NOT_INITIALIZED;
    }
    if (!controller->state.open) {
        return MUSIC_PLAYER_ERROR_NOT_OPEN;
    }
    if (controller->state.source == source) {
        return MUSIC_PLAYER_ERROR_OK;
    }
    if (controller->operation_active) {
        return MUSIC_PLAYER_ERROR_BUSY;
    }

    controller->operation_active = true;
    music_player_error_t result = call_backend(controller, controller->backend.stop, "stop before source switch failed");
    if (result == MUSIC_PLAYER_ERROR_OK && controller->backend.select_source != NULL) {
        result = controller->backend.select_source(controller->backend.context, source);
        if (result != MUSIC_PLAYER_ERROR_OK) {
            result = record_backend_failure(controller, result, "source select failed");
        }
    }
    if (result == MUSIC_PLAYER_ERROR_OK) {
        const music_player_state_event_t event = {
            .type = MUSIC_PLAYER_STATE_EVENT_SOURCE_SELECTED,
            .source = source,
        };
        result = music_player_state_reduce(&controller->state, &event);
    }
    controller->operation_active = false;
    return result;
}

static music_player_error_t reduce_open_event(music_player_controller_t *controller,
                                              const music_player_state_event_t *event)
{
    if (controller == NULL) {
        return MUSIC_PLAYER_ERROR_INVALID_ARGUMENT;
    }
    if (!controller->state.initialized) {
        return MUSIC_PLAYER_ERROR_NOT_INITIALIZED;
    }
    if (!controller->state.open) {
        return MUSIC_PLAYER_ERROR_NOT_OPEN;
    }
    return music_player_state_reduce(&controller->state, event);
}

music_player_error_t music_player_controller_set_track_index(music_player_controller_t *controller,
                                                                 uint32_t track_index)
{
    if (controller == NULL) {
        return MUSIC_PLAYER_ERROR_INVALID_ARGUMENT;
    }
    const music_player_state_event_t event = {
        .type = MUSIC_PLAYER_STATE_EVENT_TRACK_SELECTED,
        .track_index = track_index,
    };
    return reduce_open_event(controller, &event);
}

music_player_error_t music_player_controller_set_track_index_preserve_media(
    music_player_controller_t *controller, uint32_t track_index)
{
    if (controller == NULL) {
        return MUSIC_PLAYER_ERROR_INVALID_ARGUMENT;
    }
    const music_player_state_event_t event = {
        .type = MUSIC_PLAYER_STATE_EVENT_TRACK_CHANGED,
        .track_index = track_index,
    };
    return reduce_open_event(controller, &event);
}

music_player_error_t music_player_controller_reset_media(music_player_controller_t *controller)
{
    const music_player_state_event_t event = {
        .type = MUSIC_PLAYER_STATE_EVENT_MEDIA_RESET,
    };
    return reduce_open_event(controller, &event);
}

music_player_error_t music_player_controller_update_playback(music_player_controller_t *controller,
                                                               bool playing)
{
    const music_player_state_event_t event = {
        .type = playing ? MUSIC_PLAYER_STATE_EVENT_PLAY : MUSIC_PLAYER_STATE_EVENT_PAUSE,
    };
    return reduce_open_event(controller, &event);
}

music_player_error_t music_player_controller_set_metadata(music_player_controller_t *controller,
                                                            const music_player_media_update_t *media)
{
    const music_player_state_event_t event = {
        .type = MUSIC_PLAYER_STATE_EVENT_METADATA,
        .media = media,
    };
    return reduce_open_event(controller, &event);
}

music_player_error_t music_player_controller_set_progress(music_player_controller_t *controller,
                                                           uint32_t elapsed_ms,
                                                           uint32_t duration_ms)
{
    const music_player_state_event_t event = {
        .type = MUSIC_PLAYER_STATE_EVENT_PROGRESS,
        .elapsed_ms = elapsed_ms,
        .duration_ms = duration_ms,
    };
    return reduce_open_event(controller, &event);
}

music_player_error_t music_player_controller_on_backend_error(music_player_controller_t *controller,
                                                               const char *detail)
{
    const music_player_state_event_t event = {
        .type = MUSIC_PLAYER_STATE_EVENT_BACKEND_ERROR,
        .error = MUSIC_PLAYER_ERROR_BACKEND,
        .detail = detail,
    };
    return reduce_open_event(controller, &event);
}

music_player_error_t music_player_controller_on_backend_end(music_player_controller_t *controller,
                                                             const char *detail)
{
    const music_player_state_event_t event = {
        .type = MUSIC_PLAYER_STATE_EVENT_BACKEND_END,
        .error = MUSIC_PLAYER_ERROR_END_OF_TRACK,
        .detail = detail,
    };
    return reduce_open_event(controller, &event);
}

void music_player_controller_snapshot(const music_player_controller_t *controller,
                                      music_player_snapshot_t *snapshot)
{
    if (controller != NULL && snapshot != NULL) {
        music_player_state_snapshot(&controller->state, snapshot);
        cover_art_snapshot(&snapshot->cover_art);
    }
}

music_player_error_t music_player_init(const music_player_backend_t *backend)
{
    if (!default_controller_lock()) {
        return MUSIC_PLAYER_ERROR_BUSY;
    }
    const music_player_error_t result = music_player_controller_init(&s_default_controller, backend);
    default_controller_unlock();
    return result;
}

music_player_error_t music_player_open(void)
{
    if (!default_controller_lock()) {
        return MUSIC_PLAYER_ERROR_BUSY;
    }
    const music_player_error_t result = music_player_controller_open(&s_default_controller);
    default_controller_unlock();
    return result;
}

music_player_error_t music_player_close(void)
{
    if (!default_controller_lock()) {
        return MUSIC_PLAYER_ERROR_BUSY;
    }
    const music_player_error_t result = music_player_controller_close(&s_default_controller);
    default_controller_unlock();
    return result;
}

music_player_error_t music_player_select_source(music_player_source_t source)
{
    if (!default_controller_lock()) {
        return MUSIC_PLAYER_ERROR_BUSY;
    }
    const music_player_error_t result = music_player_controller_select_source(&s_default_controller, source);
    default_controller_unlock();
    return result;
}

music_player_error_t music_player_set_track_index(uint32_t track_index)
{
    if (!default_controller_lock()) {
        return MUSIC_PLAYER_ERROR_BUSY;
    }
    const music_player_error_t result = music_player_controller_set_track_index(&s_default_controller, track_index);
    default_controller_unlock();
    return result;
}

music_player_error_t music_player_set_track_index_preserve_media(uint32_t track_index)
{
    if (!default_controller_lock()) {
        return MUSIC_PLAYER_ERROR_BUSY;
    }
    const music_player_error_t result =
        music_player_controller_set_track_index_preserve_media(&s_default_controller, track_index);
    default_controller_unlock();
    return result;
}

music_player_error_t music_player_reset_media(void)
{
    if (!default_controller_lock()) {
        return MUSIC_PLAYER_ERROR_BUSY;
    }
    const music_player_error_t result = music_player_controller_reset_media(&s_default_controller);
    default_controller_unlock();
    return result;
}

music_player_error_t music_player_update_playback(bool playing)
{
    if (!default_controller_lock()) {
        return MUSIC_PLAYER_ERROR_BUSY;
    }
    const music_player_error_t result = music_player_controller_update_playback(&s_default_controller, playing);
    default_controller_unlock();
    return result;
}

music_player_error_t music_player_set_metadata(const music_player_media_update_t *media)
{
    if (!default_controller_lock()) {
        return MUSIC_PLAYER_ERROR_BUSY;
    }
    const music_player_error_t result = music_player_controller_set_metadata(&s_default_controller, media);
    default_controller_unlock();
    return result;
}

music_player_error_t music_player_set_progress(uint32_t elapsed_ms, uint32_t duration_ms)
{
    if (!default_controller_lock()) {
        return MUSIC_PLAYER_ERROR_BUSY;
    }
    const music_player_error_t result = music_player_controller_set_progress(&s_default_controller, elapsed_ms, duration_ms);
    default_controller_unlock();
    return result;
}

music_player_error_t music_player_on_backend_error(const char *detail)
{
    if (!default_controller_lock()) {
        return MUSIC_PLAYER_ERROR_BUSY;
    }
    const music_player_error_t result = music_player_controller_on_backend_error(&s_default_controller, detail);
    default_controller_unlock();
    return result;
}

music_player_error_t music_player_on_backend_end(const char *detail)
{
    if (!default_controller_lock()) {
        return MUSIC_PLAYER_ERROR_BUSY;
    }
    const music_player_error_t result = music_player_controller_on_backend_end(&s_default_controller, detail);
    default_controller_unlock();
    return result;
}

void music_player_snapshot(music_player_snapshot_t *snapshot)
{
    if (!default_controller_lock()) {
        if (snapshot != NULL) {
            memset(snapshot, 0, sizeof(*snapshot));
        }
        return;
    }
    music_player_controller_snapshot(&s_default_controller, snapshot);
    default_controller_unlock();
}

bool music_player_set_cover_art(uint32_t format_fourcc, const uint8_t *data, size_t size)
{
    if (data == NULL || size == 0U) {
        music_player_clear_cover_art();
        return true;
    }
    if (size > MUSIC_PLAYER_COVER_ART_MAX_SIZE) {
        music_player_clear_cover_art();
        return false;
    }
#if defined(ESP_PLATFORM)
    SemaphoreHandle_t lock = cover_art_lock();
    if (lock == NULL || xSemaphoreTake(lock, pdMS_TO_TICKS(MUSIC_PLAYER_COVER_ART_LOCK_TIMEOUT_MS)) != pdTRUE) {
        return false;
    }
#endif
    if (s_cover_art == NULL) {
#if defined(ESP_PLATFORM)
        s_cover_art = heap_caps_malloc(MUSIC_PLAYER_COVER_ART_MAX_SIZE, MALLOC_CAP_SPIRAM);
        if (s_cover_art == NULL) {
            (void)xSemaphoreGive(lock);
            return false;
        }
#else
        s_cover_art = malloc(MUSIC_PLAYER_COVER_ART_MAX_SIZE);
        if (s_cover_art == NULL) {
            return false;
        }
#endif
    }
    memcpy(s_cover_art, data, size);
    s_cover_art_size = size;
    s_cover_art_format = format_fourcc;
    ++s_cover_art_revision;
    if (s_cover_art_revision == 0U) {
        ++s_cover_art_revision;
    }
#if defined(ESP_PLATFORM)
    (void)xSemaphoreGive(lock);
#endif
    return true;
}

void music_player_clear_cover_art(void)
{
#if defined(ESP_PLATFORM)
    SemaphoreHandle_t lock = cover_art_lock();
    if (lock == NULL ||
        xSemaphoreTake(lock, pdMS_TO_TICKS(MUSIC_PLAYER_COVER_ART_LOCK_TIMEOUT_MS)) != pdTRUE) {
        return;
    }
#endif
    s_cover_art_size = 0U;
    s_cover_art_format = 0U;
    /** Release the cover-art buffer when the descriptor is cleared. */
    heap_caps_free(s_cover_art);
    s_cover_art = NULL;
    ++s_cover_art_revision;
    if (s_cover_art_revision == 0U) {
        ++s_cover_art_revision;
    }
#if defined(ESP_PLATFORM)
    if (lock != NULL) {
        (void)xSemaphoreGive(lock);
    }
#endif
}

size_t music_player_cover_art_copy(uint32_t revision, void *destination, size_t capacity, size_t offset)
{
    if (destination == NULL || capacity == 0U) {
        return 0U;
    }
#if defined(ESP_PLATFORM)
    SemaphoreHandle_t lock = cover_art_lock();
    if (lock == NULL || xSemaphoreTake(lock, pdMS_TO_TICKS(MUSIC_PLAYER_COVER_ART_LOCK_TIMEOUT_MS)) != pdTRUE) {
        return 0U;
    }
#endif
    if (revision == 0U || revision != s_cover_art_revision || offset >= s_cover_art_size) {
#if defined(ESP_PLATFORM)
        (void)xSemaphoreGive(lock);
#endif
        return 0U;
    }
    size_t copy_size = s_cover_art_size - offset;
    if (copy_size > capacity) {
        copy_size = capacity;
    }
    memcpy(destination, s_cover_art + offset, copy_size);
#if defined(ESP_PLATFORM)
    (void)xSemaphoreGive(lock);
#endif
    return copy_size;
}
