#ifndef MUSIC_BT_SOURCE_H
#define MUSIC_BT_SOURCE_H

#include <stdbool.h>
#include <stdint.h>

#include "music_bt_state.h"
#include "music_player_state.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef music_player_error_t music_bt_source_error_t;
typedef music_player_error_t (*music_bt_source_lifecycle_fn_t)(void *context);
typedef music_player_error_t (*music_bt_source_command_fn_t)(void *context);

/** @brief Backend operations for Bluetooth source lifecycle and commands. */
typedef struct {
    music_bt_source_lifecycle_fn_t host_init;
    music_bt_source_lifecycle_fn_t bt_audio_init;
    music_bt_source_lifecycle_fn_t pipeline_io_start;
    music_bt_source_lifecycle_fn_t stream_stop;
    music_bt_source_lifecycle_fn_t pipeline_io_destroy;
    music_bt_source_lifecycle_fn_t bt_audio_deinit;
    music_bt_source_lifecycle_fn_t host_deinit;
    music_bt_source_command_fn_t playback_prev;
    music_bt_source_command_fn_t playback_play;
    music_bt_source_command_fn_t playback_pause;
    music_bt_source_command_fn_t playback_next;
} music_bt_source_backend_t;

typedef void (*music_bt_source_event_cb_t)(void *context,
                                            const music_bt_event_t *event,
                                            const music_bt_state_t *state);

/** @brief Bluetooth backend and event-listener configuration. */
typedef struct {
    const music_bt_source_backend_t *backend;
    void *backend_context;
    music_bt_source_event_cb_t event_cb;
    void *event_context;
} music_bt_source_config_t;

/** @brief Bluetooth source session and resource lifecycle state. */
typedef struct {
    music_bt_state_t state;
    const music_bt_source_backend_t *backend;
    void *backend_context;
    music_bt_source_event_cb_t event_cb;
    void *event_context;
    bool configured;
    bool host_initialized;
    bool bt_audio_initialized;
    bool pipeline_io_started;
} music_bt_source_t;

/**
 * @brief Configure source backend operations and event callbacks.
 * @param source Source to use.
 * @param config Configuration to use.
 * @return The operation status.
 */
music_bt_source_error_t music_bt_source_init(music_bt_source_t *source,
                                               const music_bt_source_config_t *config);
/**
 * @brief Open the Bluetooth source through its configured backend.
 * @param source Source to use.
 * @return The operation status.
 */
music_bt_source_error_t music_bt_source_open(music_bt_source_t *source);
/**
 * @brief Close source resources and its current event generation.
 * @param source Source to use.
 * @return The operation status.
 */
music_bt_source_error_t music_bt_source_close(music_bt_source_t *source);
/**
 * @brief Apply an event to the source state and notify its listener.
 * @param source Source to use.
 * @param event Event to process.
 * @param[out] result Receives the event result.
 * @return The operation status.
 */
music_bt_source_error_t music_bt_source_on_event(music_bt_source_t *source,
                                                   const music_bt_event_t *event,
                                                   music_bt_event_result_t *result);
/**
 * @brief Forward a supported playback command to the remote peer.
 * @param source Source to use.
 * @param command Command to execute.
 * @return The operation status.
 */
music_bt_source_error_t music_bt_source_command(music_bt_source_t *source,
                                                 music_bt_avrcp_command_t command);
/**
 * @brief Return the source session generation used to filter stale events.
 * @param source Source to use.
 * @return The source session generation used to filter stale events.
 */
uint32_t music_bt_source_generation(const music_bt_source_t *source);

#ifdef __cplusplus
}
#endif

#endif
