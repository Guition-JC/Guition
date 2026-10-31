#include "audio_volume.h"

#include <string.h>

#include "audio_output.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#define AUDIO_VOLUME_LISTENER_MAX 8U

typedef struct {
    audio_volume_listener_t callback;
    void *context;
} audio_volume_listener_entry_t;

static audio_volume_state_t s_state = {
    .level = AUDIO_VOLUME_DEFAULT,
    .muted = false,
};
static audio_volume_listener_entry_t s_listeners[AUDIO_VOLUME_LISTENER_MAX];
static StaticSemaphore_t s_lock_storage;
static SemaphoreHandle_t s_lock;
static bool s_initialized;

static uint8_t clamp_level(uint8_t level)
{
    return level > AUDIO_VOLUME_MAX ? AUDIO_VOLUME_MAX : level;
}

static void ensure_lock(void)
{
    if (s_lock == NULL) {
        s_lock = xSemaphoreCreateMutexStatic(&s_lock_storage);
    }
}

static esp_err_t apply(uint8_t level, bool muted, audio_volume_origin_t origin)
{
    ensure_lock();
    if (s_lock == NULL || xSemaphoreTake(s_lock, pdMS_TO_TICKS(1000)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    if (!audio_output_is_open()) {
        xSemaphoreGive(s_lock);
        return ESP_ERR_INVALID_STATE;
    }
    if (esp_codec_dev_set_out_vol(audio_output_get_codec_dev(), muted ? 0 : (int)level) != ESP_CODEC_DEV_OK) {
        xSemaphoreGive(s_lock);
        return ESP_FAIL;
    }
    s_state.level = level;
    s_state.muted = muted;
    audio_volume_state_t snapshot = s_state;
    audio_volume_listener_entry_t listeners[AUDIO_VOLUME_LISTENER_MAX];
    memcpy(listeners, s_listeners, sizeof(listeners));
    xSemaphoreGive(s_lock);

    for (size_t index = 0U; index < AUDIO_VOLUME_LISTENER_MAX; ++index) {
        if (listeners[index].callback != NULL) {
            listeners[index].callback(&snapshot, origin, listeners[index].context);
        }
    }
    return ESP_OK;
}

esp_err_t audio_volume_set(uint8_t level)
{
    s_initialized = true;
    return apply(clamp_level(level), s_state.muted, AUDIO_VOLUME_ORIGIN_LOCAL);
}

esp_err_t audio_volume_adjust(bool increase)
{
    s_initialized = true;
    uint8_t level = s_state.level;
    if (increase) {
        level = level >= AUDIO_VOLUME_MAX - AUDIO_VOLUME_STEP
                     ? AUDIO_VOLUME_MAX : (uint8_t)(level + AUDIO_VOLUME_STEP);
    } else {
        level = level <= AUDIO_VOLUME_STEP ? AUDIO_VOLUME_MIN : (uint8_t)(level - AUDIO_VOLUME_STEP);
    }
    return apply(level, s_state.muted, AUDIO_VOLUME_ORIGIN_LOCAL);
}

esp_err_t audio_volume_set_mute(bool muted)
{
    s_initialized = true;
    return apply(s_state.level, muted, AUDIO_VOLUME_ORIGIN_LOCAL);
}

esp_err_t audio_volume_get(audio_volume_state_t *state)
{
    if (state == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    ensure_lock();
    if (s_lock == NULL || xSemaphoreTake(s_lock, pdMS_TO_TICKS(1000)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    *state = s_state;
    xSemaphoreGive(s_lock);
    return s_initialized ? ESP_OK : ESP_ERR_INVALID_STATE;
}

esp_err_t audio_volume_apply_remote(uint8_t level, bool muted)
{
    s_initialized = true;
    return apply(clamp_level(level), muted, AUDIO_VOLUME_ORIGIN_BT_REMOTE);
}

esp_err_t audio_volume_adjust_remote(bool increase)
{
    s_initialized = true;
    uint8_t level = s_state.level;
    if (increase) {
        level = level >= AUDIO_VOLUME_MAX - AUDIO_VOLUME_STEP
                     ? AUDIO_VOLUME_MAX : (uint8_t)(level + AUDIO_VOLUME_STEP);
    } else {
        level = level <= AUDIO_VOLUME_STEP ? AUDIO_VOLUME_MIN : (uint8_t)(level - AUDIO_VOLUME_STEP);
    }
    return apply(level, s_state.muted, AUDIO_VOLUME_ORIGIN_BT_REMOTE);
}

esp_err_t audio_volume_subscribe(audio_volume_listener_t listener, void *context)
{
    if (listener == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    ensure_lock();
    if (s_lock == NULL || xSemaphoreTake(s_lock, pdMS_TO_TICKS(1000)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    for (size_t index = 0U; index < AUDIO_VOLUME_LISTENER_MAX; ++index) {
        if (s_listeners[index].callback == listener && s_listeners[index].context == context) {
            xSemaphoreGive(s_lock);
            return ESP_OK;
        }
    }
    for (size_t index = 0U; index < AUDIO_VOLUME_LISTENER_MAX; ++index) {
        if (s_listeners[index].callback == NULL) {
            s_listeners[index] = (audio_volume_listener_entry_t){listener, context};
            xSemaphoreGive(s_lock);
            return ESP_OK;
        }
    }
    xSemaphoreGive(s_lock);
    return ESP_ERR_NO_MEM;
}

esp_err_t audio_volume_unsubscribe(audio_volume_listener_t listener, void *context)
{
    if (listener == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    ensure_lock();
    if (s_lock == NULL || xSemaphoreTake(s_lock, pdMS_TO_TICKS(1000)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    for (size_t index = 0U; index < AUDIO_VOLUME_LISTENER_MAX; ++index) {
        if (s_listeners[index].callback == listener && s_listeners[index].context == context) {
            s_listeners[index] = (audio_volume_listener_entry_t){0};
            xSemaphoreGive(s_lock);
            return ESP_OK;
        }
    }
    xSemaphoreGive(s_lock);
    return ESP_ERR_NOT_FOUND;
}
