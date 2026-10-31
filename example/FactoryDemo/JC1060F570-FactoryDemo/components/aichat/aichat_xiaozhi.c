#include "aichat_xiaozhi.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "aichat_audio_format.h"
#include "aichat_audio_playback.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mcp_engine.h"
#include "esp_xiaozhi_info.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#define AICHAT_XIAOZHI_AUDIO_QUEUE_LENGTH 12U
#define AICHAT_XIAOZHI_AUDIO_PACKET_MAX 4096U
#define AICHAT_XIAOZHI_AUDIO_TX_TASK_STACK 4096U
#define AICHAT_XIAOZHI_AUDIO_TX_TASK_PRIORITY 5U

struct aichat_audio_packet {
    uint8_t *data;
    size_t len;
};

/** The component owns callback buffers only for the callback duration. */
struct aichat_xiaozhi {
    esp_xiaozhi_chat_handle_t handle;
    esp_mcp_t *mcp;
    aichat_xiaozhi_audio_cb_t audio_cb;
    aichat_xiaozhi_event_cb_t event_cb;
    aichat_audio_playback_t *playback;
    void *context;
    QueueHandle_t audio_queue;
    TaskHandle_t audio_tx_task;
    SemaphoreHandle_t audio_tx_done;
    volatile bool audio_tx_stop;
    volatile bool uplink_enabled;
    bool started;
    bool connected;
    bool audio_open;
    volatile bool session_stale;
    bool suppress_next_audio_closed;
    bool events_registered;
    aichat_xiaozhi_status_t status;
};

static void notify_event(aichat_xiaozhi_t *client, aichat_xiaozhi_event_t event,
                         const void *data)
{
    if (client != NULL && client->event_cb != NULL) {
        client->event_cb(event, data, client->context);
    }
}

static void audio_cb(const uint8_t *data, int len, void *ctx)
{
    aichat_xiaozhi_t *client = ctx;
    if (client == NULL || data == NULL || len <= 0 || client->session_stale ||
        !client->started || !client->audio_open) {
        return;
    }
    if (client->playback != NULL) {
        (void)aichat_audio_playback_enqueue(client->playback, data, (size_t)len);
    }
    if (client->audio_cb != NULL) {
        client->audio_cb(data, (size_t)len, client->context);
    }
}

static void protocol_event_cb(esp_xiaozhi_chat_event_t event, void *data, void *ctx)
{
    aichat_xiaozhi_t *client = ctx;
    switch (event) {
    case ESP_XIAOZHI_CHAT_EVENT_CHAT_TEXT:
        notify_event(client, AICHAT_XIAOZHI_EVENT_CHAT_TEXT, data);
        break;
    case ESP_XIAOZHI_CHAT_EVENT_CHAT_TTS_STATE:
        notify_event(client, AICHAT_XIAOZHI_EVENT_CHAT_TTS_STATE, data);
        break;
    case ESP_XIAOZHI_CHAT_EVENT_CHAT_ERROR:
        notify_event(client, AICHAT_XIAOZHI_EVENT_CHAT_ERROR, data);
        break;
    case ESP_XIAOZHI_CHAT_EVENT_CHAT_EMOJI:
        notify_event(client, AICHAT_XIAOZHI_EVENT_CHAT_EMOJI, data);
        break;
    case ESP_XIAOZHI_CHAT_EVENT_CHAT_SYSTEM_CMD:
        notify_event(client, AICHAT_XIAOZHI_EVENT_CHAT_SYSTEM_CMD, data);
        break;
    default:
        break;
    }
}

static void chat_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    aichat_xiaozhi_t *client = arg;
    (void)event_base;
    (void)event_data;
    if (client == NULL || !client->started) {
        return;
    }
    switch (event_id) {
    case ESP_XIAOZHI_CHAT_EVENT_CONNECTED:
        client->connected = true;
        notify_event(client, AICHAT_XIAOZHI_EVENT_CONNECTED, NULL);
        break;
    case ESP_XIAOZHI_CHAT_EVENT_DISCONNECTED:
        client->connected = false;
        client->audio_open = false;
        client->uplink_enabled = false;
        client->session_stale = true;
        notify_event(client, AICHAT_XIAOZHI_EVENT_DISCONNECTED, NULL);
        break;
    case ESP_XIAOZHI_CHAT_EVENT_AUDIO_CHANNEL_OPENED:
        client->audio_open = true;
        client->session_stale = false;
        notify_event(client, AICHAT_XIAOZHI_EVENT_AUDIO_CHANNEL_OPENED, NULL);
        break;
    case ESP_XIAOZHI_CHAT_EVENT_AUDIO_CHANNEL_CLOSED:
        client->audio_open = false;
        client->uplink_enabled = false;
        client->session_stale = true;
        if (client->suppress_next_audio_closed) {
            /** Treat the deliberate stale-session channel closure as a normal transition. */
            client->suppress_next_audio_closed = false;
            break;
        }
        notify_event(client, AICHAT_XIAOZHI_EVENT_AUDIO_CHANNEL_CLOSED, NULL);
        break;
    case ESP_XIAOZHI_CHAT_EVENT_SERVER_GOODBYE:
        client->uplink_enabled = false;
        client->session_stale = true;
        notify_event(client, AICHAT_XIAOZHI_EVENT_SERVER_GOODBYE, NULL);
        break;
    default:
        break;
    }
}

static void audio_tx_task(void *arg)
{
    aichat_xiaozhi_t *client = arg;
    struct aichat_audio_packet packet = {0};
    while (client != NULL && (!client->audio_tx_stop ||
                              uxQueueMessagesWaiting(client->audio_queue) > 0U)) {
        if (xQueueReceive(client->audio_queue, &packet, pdMS_TO_TICKS(100)) != pdTRUE) {
            continue;
        }
        if (packet.data != NULL) {
            if (!client->audio_tx_stop && client->uplink_enabled && client->started &&
                client->audio_open && !client->session_stale) {
                (void)aichat_xiaozhi_send_audio(client, packet.data, packet.len);
            }
            free(packet.data);
            packet.data = NULL;
        }
    }
    while (client != NULL && xQueueReceive(client->audio_queue, &packet, 0) == pdTRUE) {
        free(packet.data);
        packet.data = NULL;
    }
    if (client->audio_tx_done != NULL) {
        xSemaphoreGive(client->audio_tx_done);
    }
    vTaskDelete(NULL);
}

static esp_err_t start_audio_tx(aichat_xiaozhi_t *client)
{
    if (client->audio_queue == NULL) {
        client->audio_queue = xQueueCreate(AICHAT_XIAOZHI_AUDIO_QUEUE_LENGTH,
                                            sizeof(struct aichat_audio_packet));
        if (client->audio_queue == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }
    client->audio_tx_done = xSemaphoreCreateBinary();
    if (client->audio_tx_done == NULL) {
        vQueueDelete(client->audio_queue);
        client->audio_queue = NULL;
        return ESP_ERR_NO_MEM;
    }
    client->audio_tx_stop = false;
    if (xTaskCreate(audio_tx_task, "aichat_tx", AICHAT_XIAOZHI_AUDIO_TX_TASK_STACK,
                    client, AICHAT_XIAOZHI_AUDIO_TX_TASK_PRIORITY,
                    &client->audio_tx_task) != pdPASS) {
        vSemaphoreDelete(client->audio_tx_done);
        client->audio_tx_done = NULL;
        vQueueDelete(client->audio_queue);
        client->audio_queue = NULL;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

static void drain_audio_queue(aichat_xiaozhi_t *client)
{
    struct aichat_audio_packet packet = {0};
    if (client == NULL || client->audio_queue == NULL) {
        return;
    }
    while (xQueueReceive(client->audio_queue, &packet, 0) == pdTRUE) {
        free(packet.data);
        packet.data = NULL;
    }
}

static void stop_audio_tx(aichat_xiaozhi_t *client)
{
    if (client == NULL || client->audio_queue == NULL) {
        return;
    }
    client->audio_tx_stop = true;
    if (client->audio_tx_done != NULL) {
        (void)xSemaphoreTake(client->audio_tx_done, portMAX_DELAY);
    }
    client->audio_tx_task = NULL;
    vSemaphoreDelete(client->audio_tx_done);
    client->audio_tx_done = NULL;
    vQueueDelete(client->audio_queue);
    client->audio_queue = NULL;
}

aichat_xiaozhi_t *aichat_xiaozhi_create(aichat_xiaozhi_audio_cb_t audio_callback,
                                         aichat_xiaozhi_event_cb_t event_callback,
                                         void *context)
{
    aichat_xiaozhi_t *client = calloc(1, sizeof(*client));
    if (client != NULL) {
        client->audio_cb = audio_callback;
        client->event_cb = event_callback;
        client->context = context;
        client->status.binding = AICHAT_XIAOZHI_BINDING_UNKNOWN;
    }
    return client;
}

esp_err_t aichat_xiaozhi_get_status(const aichat_xiaozhi_t *client,
                                    aichat_xiaozhi_status_t *status)
{
    if (client == NULL || status == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *status = client->status;
    return ESP_OK;
}

bool aichat_xiaozhi_is_connected(const aichat_xiaozhi_t *client)
{
    return client != NULL && client->connected;
}

bool aichat_xiaozhi_is_session_ready(const aichat_xiaozhi_t *client)
{
    return client != NULL && client->audio_open && !client->session_stale;
}

esp_err_t aichat_xiaozhi_set_playback(aichat_xiaozhi_t *client,
                                      aichat_audio_playback_t *playback)
{
    if (client == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (client->started || client->audio_open) {
        return ESP_ERR_INVALID_STATE;
    }
    client->playback = playback;
    return ESP_OK;
}

static void copy_status_text(char *dst, size_t dst_size, const char *src)
{
    if (dst == NULL || dst_size == 0U) {
        return;
    }
    if (src == NULL) {
        dst[0] = '\0';
        return;
    }
    (void)snprintf(dst, dst_size, "%s", src);
}

esp_err_t aichat_xiaozhi_start(aichat_xiaozhi_t *client)
{
    if (client == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (client->started) {
        return ESP_OK;
    }

    memset(&client->status, 0, sizeof(client->status));
    client->status.binding = AICHAT_XIAOZHI_BINDING_UNKNOWN;
    esp_xiaozhi_chat_info_t info = {0};
    esp_err_t ret = esp_xiaozhi_chat_get_info(&info);
    if (ret != ESP_OK) {
        client->status.binding = AICHAT_XIAOZHI_BINDING_UNAVAILABLE;
        client->status.configured = false;
        (void)esp_xiaozhi_chat_free_info(&info);
        return ret;
    }

    client->status.has_mqtt_config = info.has_mqtt_config;
    client->status.has_websocket_config = info.has_websocket_config;
    client->status.configured = info.has_mqtt_config || info.has_websocket_config;
    copy_status_text(client->status.activation_code,
                     sizeof(client->status.activation_code), info.activation_code);
    copy_status_text(client->status.activation_message,
                     sizeof(client->status.activation_message), info.activation_message);
    const bool activation_required = info.has_activation_code ||
                                     info.has_activation_challenge ||
                                     (info.activation_message != NULL &&
                                      info.activation_message[0] != '\0');
    client->status.binding = !client->status.configured
                                 ? AICHAT_XIAOZHI_BINDING_UNAVAILABLE
                                 : activation_required
                                     ? AICHAT_XIAOZHI_BINDING_REQUIRED
                                     : AICHAT_XIAOZHI_BINDING_BOUND;
    (void)esp_xiaozhi_chat_free_info(&info);
    if (client->status.binding != AICHAT_XIAOZHI_BINDING_BOUND) {
        return ESP_ERR_INVALID_STATE;
    }

    ret = esp_mcp_create(&client->mcp);
    if (ret != ESP_OK) {
        return ret;
    }
    esp_xiaozhi_chat_config_t config = ESP_XIAOZHI_CHAT_DEFAULT_CONFIG();
    config.audio_type = ESP_XIAOZHI_CHAT_AUDIO_TYPE_OPUS;
    config.audio_callback = audio_cb;
    config.event_callback = protocol_event_cb;
    config.audio_callback_ctx = client;
    config.event_callback_ctx = client;
    config.mcp_engine = client->mcp;
    config.owns_mcp_engine = true;
    const bool use_mqtt = client->status.has_mqtt_config &&
#if AICHAT_XIAOZHI_PREFER_WEBSOCKET
                          !client->status.has_websocket_config;
#else
                          true;
#endif
    config.has_mqtt_config = use_mqtt;
    config.has_websocket_config = !use_mqtt && client->status.has_websocket_config;
    ESP_LOGI("AIChatXiaozhi", "transport=%s mqtt_config=%d websocket_config=%d",
             use_mqtt ? "MQTT" : "WebSocket", client->status.has_mqtt_config,
             client->status.has_websocket_config);

    ret = esp_xiaozhi_chat_init(&config, &client->handle);
    if (ret != ESP_OK) {
        (void)esp_mcp_destroy(client->mcp);
        client->mcp = NULL;
        return ret;
    }
    ret = esp_event_handler_register(ESP_XIAOZHI_CHAT_EVENTS, ESP_EVENT_ANY_ID,
                                     chat_event_handler, client);
    if (ret != ESP_OK) {
        (void)esp_xiaozhi_chat_deinit(client->handle);
        client->handle = 0;
        client->mcp = NULL;
        return ret;
    }
    client->events_registered = true;
    client->started = true;
    ret = esp_xiaozhi_chat_start(client->handle);
    if (ret != ESP_OK) {
        (void)esp_event_handler_unregister(ESP_XIAOZHI_CHAT_EVENTS, ESP_EVENT_ANY_ID,
                                           chat_event_handler);
        client->events_registered = false;
        (void)esp_xiaozhi_chat_deinit(client->handle);
        client->handle = 0;
        client->mcp = NULL;
        client->started = false;
        return ret;
    }
    return ESP_OK;
}

esp_err_t aichat_xiaozhi_open_audio(aichat_xiaozhi_t *client)
{
    if (client == NULL || !client->started || !client->connected) {
        return ESP_ERR_INVALID_STATE;
    }
    if (client->audio_open && !client->session_stale) {
        return ESP_OK;
    }
    if (client->audio_open) {
        /** Close the invalidated session before opening the next audio channel. */
        client->uplink_enabled = false;
        drain_audio_queue(client);
        stop_audio_tx(client);
        client->suppress_next_audio_closed = true;
        (void)esp_xiaozhi_chat_close_audio_channel(client->handle);
        client->audio_open = false;
    }

    esp_xiaozhi_chat_audio_t params = {
        .format = "opus",
        .sample_rate = AICHAT_AUDIO_SAMPLE_RATE,
        .channels = 1,
        .frame_duration = AICHAT_AUDIO_FRAME_DURATION_MS,
    };
    esp_err_t ret = esp_xiaozhi_chat_open_audio_channel(client->handle, &params, NULL, 0);
    if (ret != ESP_OK) {
        return ret;
    }
    client->audio_open = true;
    ret = start_audio_tx(client);
    if (ret != ESP_OK) {
        (void)esp_xiaozhi_chat_close_audio_channel(client->handle);
        client->audio_open = false;
        return ret;
    }
    if (client->playback != NULL) {
        ret = aichat_audio_playback_start(client->playback, AICHAT_AUDIO_SAMPLE_RATE,
                                          AICHAT_AUDIO_FRAME_DURATION_MS);
        if (ret != ESP_OK) {
            stop_audio_tx(client);
            (void)esp_xiaozhi_chat_close_audio_channel(client->handle);
            client->audio_open = false;
            return ret;
        }
    }
    client->session_stale = false;
    return ESP_OK;
}

void aichat_xiaozhi_set_uplink_enabled(aichat_xiaozhi_t *client, bool enabled)
{
    if (client == NULL) {
        return;
    }
    client->uplink_enabled = enabled && client->started && client->audio_open &&
                             !client->session_stale;
    if (!client->uplink_enabled) {
        drain_audio_queue(client);
    }
}

esp_err_t aichat_xiaozhi_enqueue_audio(aichat_xiaozhi_t *client,
                                       const uint8_t *data, size_t len)
{
    if (client == NULL || !client->uplink_enabled || !client->audio_open ||
        client->session_stale || client->audio_queue == NULL || data == NULL || len == 0U) {
        return ESP_ERR_INVALID_STATE;
    }
    if (len > AICHAT_XIAOZHI_AUDIO_PACKET_MAX) {
        return ESP_ERR_INVALID_SIZE;
    }
    struct aichat_audio_packet packet = {
        .data = malloc(len),
        .len = len,
    };
    if (packet.data == NULL) {
        return ESP_ERR_NO_MEM;
    }
    memcpy(packet.data, data, len);
    if (xQueueSend(client->audio_queue, &packet, 0) != pdTRUE) {
        free(packet.data);
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

esp_err_t aichat_xiaozhi_send_audio(aichat_xiaozhi_t *client,
                                    const uint8_t *data, size_t len)
{
    if (client == NULL || !client->uplink_enabled || !client->audio_open ||
        client->session_stale || data == NULL || len == 0U) {
        return ESP_ERR_INVALID_STATE;
    }
    return esp_xiaozhi_chat_send_audio_data(client->handle, (const char *)data, len);
}

esp_err_t aichat_xiaozhi_send_wake_word(aichat_xiaozhi_t *client,
                                        const char *wake_word)
{
    if (client == NULL || !client->audio_open || client->session_stale) {
        return ESP_ERR_INVALID_STATE;
    }
    return esp_xiaozhi_chat_send_wake_word(client->handle, wake_word);
}

esp_err_t aichat_xiaozhi_send_start_listening(aichat_xiaozhi_t *client)
{
    if (client == NULL || !client->audio_open || client->session_stale) {
        return ESP_ERR_INVALID_STATE;
    }
    return esp_xiaozhi_chat_send_start_listening(client->handle,
                ESP_XIAOZHI_CHAT_LISTENING_MODE_AUTO);
}

esp_err_t aichat_xiaozhi_send_stop_listening(aichat_xiaozhi_t *client)
{
    if (client == NULL || !client->audio_open || client->session_stale) {
        return ESP_ERR_INVALID_STATE;
    }
    return esp_xiaozhi_chat_send_stop_listening(client->handle);
}

esp_err_t aichat_xiaozhi_abort_speaking(aichat_xiaozhi_t *client)
{
    if (client == NULL || !client->audio_open || client->session_stale) {
        return ESP_ERR_INVALID_STATE;
    }
    return esp_xiaozhi_chat_send_abort_speaking(
        client->handle, ESP_XIAOZHI_CHAT_ABORT_SPEAKING_REASON_WAKE_WORD_DETECTED);
}

esp_err_t aichat_xiaozhi_interrupt_playback(aichat_xiaozhi_t *client)
{
    if (client == NULL || client->playback == NULL) {
        return ESP_OK;
    }
    esp_err_t ret = aichat_audio_playback_stop(client->playback);
    if (ret != ESP_OK || !client->audio_open) {
        return ret;
    }
    return aichat_audio_playback_start(client->playback, AICHAT_AUDIO_SAMPLE_RATE,
                                       AICHAT_AUDIO_FRAME_DURATION_MS);
}

esp_err_t aichat_xiaozhi_stop(aichat_xiaozhi_t *client)
{
    if (client == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    client->started = false;
    client->audio_open = false;
    client->connected = false;
    client->uplink_enabled = false;
    client->session_stale = true;
    client->suppress_next_audio_closed = false;
    stop_audio_tx(client);
    if (client->handle != 0U) {
        if (client->events_registered) {
            (void)esp_event_handler_unregister(ESP_XIAOZHI_CHAT_EVENTS, ESP_EVENT_ANY_ID,
                                               chat_event_handler);
            client->events_registered = false;
        }
        (void)esp_xiaozhi_chat_close_audio_channel(client->handle);
        (void)esp_xiaozhi_chat_stop(client->handle);
        (void)esp_xiaozhi_chat_deinit(client->handle);
        client->handle = 0U;
        client->mcp = NULL;
    }
    if (client->playback != NULL) {
        (void)aichat_audio_playback_stop(client->playback);
    }
    return ESP_OK;
}

void aichat_xiaozhi_destroy(aichat_xiaozhi_t *client)
{
    if (client == NULL) {
        return;
    }
    (void)aichat_xiaozhi_stop(client);
    free(client);
}
