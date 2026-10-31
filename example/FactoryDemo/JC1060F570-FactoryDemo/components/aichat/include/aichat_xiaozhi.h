#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "esp_xiaozhi_chat.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Owned Xiaozhi protocol client. */
typedef struct aichat_xiaozhi aichat_xiaozhi_t;
/** @brief Owned encoded-audio playback pipeline. */
typedef struct aichat_audio_playback aichat_audio_playback_t;

/** @brief Protocol event delivered to the application. */
typedef enum {
    AICHAT_XIAOZHI_EVENT_CONNECTED = 0,
    AICHAT_XIAOZHI_EVENT_DISCONNECTED,
    AICHAT_XIAOZHI_EVENT_AUDIO_CHANNEL_OPENED,
    AICHAT_XIAOZHI_EVENT_AUDIO_CHANNEL_CLOSED,
    AICHAT_XIAOZHI_EVENT_SERVER_GOODBYE,
    AICHAT_XIAOZHI_EVENT_CHAT_TEXT,
    AICHAT_XIAOZHI_EVENT_CHAT_TTS_STATE,
    AICHAT_XIAOZHI_EVENT_CHAT_ERROR,
    AICHAT_XIAOZHI_EVENT_CHAT_EMOJI,
    AICHAT_XIAOZHI_EVENT_CHAT_SYSTEM_CMD,
} aichat_xiaozhi_event_t;

/** @brief Device activation and account-binding status. */
typedef enum {
    AICHAT_XIAOZHI_BINDING_UNKNOWN = 0,
    AICHAT_XIAOZHI_BINDING_BOUND,
    AICHAT_XIAOZHI_BINDING_REQUIRED,
    AICHAT_XIAOZHI_BINDING_UNAVAILABLE,
} aichat_xiaozhi_binding_state_t;

#define AICHAT_XIAOZHI_STATUS_TEXT_MAX 128U
/** @brief Prefer the WebSocket transport when the service exposes both transports. */
#define AICHAT_XIAOZHI_PREFER_WEBSOCKET 1
/** @brief Protocol wake word expected by the Xiaozhi service; keep it independent of UI language. */
#define AICHAT_XIAOZHI_WAKE_WORD "你好小智"

/** @brief Client configuration and activation snapshot. */
typedef struct {
    aichat_xiaozhi_binding_state_t binding;
    bool configured;
    bool has_mqtt_config;
    bool has_websocket_config;
    char activation_code[AICHAT_XIAOZHI_STATUS_TEXT_MAX];
    char activation_message[AICHAT_XIAOZHI_STATUS_TEXT_MAX];
} aichat_xiaozhi_status_t;

typedef void (*aichat_xiaozhi_audio_cb_t)(const uint8_t *data, size_t len, void *context);
typedef void (*aichat_xiaozhi_event_cb_t)(aichat_xiaozhi_event_t event, const void *data, void *context);

/**
 * @brief Create a Xiaozhi client and register its audio and event callbacks.
 * @param audio_cb Audio callback.
 * @param event_cb Event callback.
 * @param context Caller-supplied context.
 * @return Created transport instance.
 */
aichat_xiaozhi_t *aichat_xiaozhi_create(aichat_xiaozhi_audio_cb_t audio_cb,
                                        aichat_xiaozhi_event_cb_t event_cb,
                                        void *context);
/**
 * @brief Copy client configuration and activation status.
 * @param client Client instance.
 * @param[out] status Receives the client configuration and activation status.
 * @return ESP_OK on success, or an error code on failure.
 */
esp_err_t aichat_xiaozhi_get_status(const aichat_xiaozhi_t *client,
                                    aichat_xiaozhi_status_t *status);
/**
 * @brief Report whether the transport is connected.
 * @param client Client instance.
 * @return true if the transport is connected; false otherwise.
 */
bool aichat_xiaozhi_is_connected(const aichat_xiaozhi_t *client);
/**
 * @brief Report whether the audio session is open and current.
 * @param client Client instance.
 * @return true if the audio session is open and current; false otherwise.
 */
bool aichat_xiaozhi_is_session_ready(const aichat_xiaozhi_t *client);
/**
 * @brief Attach a borrowed playback instance for received audio.
 * @param client Client instance.
 * @param playback Playback state.
 * @return ESP_OK on success, or an error code on failure.
 */
esp_err_t aichat_xiaozhi_set_playback(aichat_xiaozhi_t *client,
                                      aichat_audio_playback_t *playback);
/**
 * @brief Start the client transport and audio transmit worker.
 * @param client Client instance.
 * @return ESP_OK on success, or an error code on failure.
 */
esp_err_t aichat_xiaozhi_start(aichat_xiaozhi_t *client);
/**
 * @brief Open a fresh protocol audio session.
 * @param client Client instance.
 * @return ESP_OK on success, or an error code on failure.
 */
esp_err_t aichat_xiaozhi_open_audio(aichat_xiaozhi_t *client);
/**
 * @brief Enable or disable audio transmission for the current session.
 * @param client Client instance.
 * @param enabled Whether the feature is enabled.
 */
void aichat_xiaozhi_set_uplink_enabled(aichat_xiaozhi_t *client, bool enabled);
/**
 * @brief Copy an uplink packet into the transmit queue; len is measured in bytes.
 * @param client Client instance.
 * @param data Data used by the operation.
 * @param len Length of the supplied data.
 * @return ESP_OK on success, or an error code on failure.
 */
esp_err_t aichat_xiaozhi_enqueue_audio(aichat_xiaozhi_t *client,
                                       const uint8_t *data, size_t len);
/**
 * @brief Send encoded audio through the active uplink; len is measured in bytes.
 * @param client Client instance.
 * @param data Data used by the operation.
 * @param len Length of the supplied data.
 * @return ESP_OK on success, or an error code on failure.
 */
esp_err_t aichat_xiaozhi_send_audio(aichat_xiaozhi_t *client,
                                    const uint8_t *data, size_t len);
/**
 * @brief Send the detected wake word to the current audio session.
 * @param client Client instance.
 * @param wake_word Wake word.
 * @return ESP_OK on success, or an error code on failure.
 */
esp_err_t aichat_xiaozhi_send_wake_word(aichat_xiaozhi_t *client,
                                        const char *wake_word);
/**
 * @brief Request automatic listening for the current session.
 * @param client Client instance.
 * @return ESP_OK on success, or an error code on failure.
 */
esp_err_t aichat_xiaozhi_send_start_listening(aichat_xiaozhi_t *client);
/**
 * @brief Notify the server that local listening has ended.
 * @param client Client instance.
 * @return ESP_OK on success, or an error code on failure.
 */
esp_err_t aichat_xiaozhi_send_stop_listening(aichat_xiaozhi_t *client);
/**
 * @brief Request cancellation of the current server response.
 * @param client Client instance.
 * @return ESP_OK on success, or an error code on failure.
 */
esp_err_t aichat_xiaozhi_abort_speaking(aichat_xiaozhi_t *client);
/**
 * @brief Interrupt the current response and local playback.
 * @param client Client instance.
 * @return ESP_OK on success, or an error code on failure.
 */
esp_err_t aichat_xiaozhi_interrupt_playback(aichat_xiaozhi_t *client);
/**
 * @brief Stop the protocol session, transport and transmit worker.
 * @param client Client instance.
 * @return ESP_OK on success, or an error code on failure.
 */
esp_err_t aichat_xiaozhi_stop(aichat_xiaozhi_t *client);
/**
 * @brief Stop the client and free its owned resources.
 * @param client Client instance.
 */
void aichat_xiaozhi_destroy(aichat_xiaozhi_t *client);

#ifdef __cplusplus
}
#endif
