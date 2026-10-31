#ifndef MUSIC_BT_STATE_H
#define MUSIC_BT_STATE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MUSIC_BT_DEVICE_NAME_MAX 32U
#define MUSIC_BT_TITLE_MAX 64U
#define MUSIC_BT_ARTIST_MAX 64U
#define MUSIC_BT_ALBUM_MAX 64U

/** @brief Bluetooth peer connection state. */
typedef enum {
    MUSIC_BT_CONNECTION_CLOSED = 0,
    MUSIC_BT_CONNECTION_CONNECTING,
    MUSIC_BT_CONNECTION_CONNECTED,
} music_bt_connection_t;

/** @brief A2DP stream activity state. */
typedef enum {
    MUSIC_BT_STREAM_STOPPED = 0,
    MUSIC_BT_STREAM_STARTED,
} music_bt_stream_t;

/** @brief Remote AVRCP playback status. */
typedef enum {
    MUSIC_BT_PLAYBACK_STATUS_STOPPED = 0U,
    MUSIC_BT_PLAYBACK_STATUS_PLAYING = 1U,
    MUSIC_BT_PLAYBACK_STATUS_PAUSED = 2U,
    MUSIC_BT_PLAYBACK_STATUS_FWD_SEEK = 3U,
    MUSIC_BT_PLAYBACK_STATUS_REV_SEEK = 4U,
    MUSIC_BT_PLAYBACK_STATUS_ERROR = 0xFFU,
    MUSIC_BT_PLAYBACK_STATUS_UNKNOWN = 0xFFFFFFFFU,
} music_bt_playback_status_t;

/** @brief Bluetooth connection, stream or metadata event. */
typedef enum {
    MUSIC_BT_EVENT_CONNECTING = 0,
    MUSIC_BT_EVENT_CONNECTED,
    MUSIC_BT_EVENT_DISCONNECTED,
    MUSIC_BT_EVENT_STREAM_STARTED,
    MUSIC_BT_EVENT_STREAM_STOPPED,
    MUSIC_BT_EVENT_TRACK_CHANGED,
    MUSIC_BT_EVENT_METADATA,
    MUSIC_BT_EVENT_PLAYBACK_STATUS,
} music_bt_event_type_t;

/** @brief Supported remote transport command. */
typedef enum {
    MUSIC_BT_AVRCP_PREVIOUS = 0,
    MUSIC_BT_AVRCP_PLAY_PAUSE,
    MUSIC_BT_AVRCP_NEXT,
    MUSIC_BT_AVRCP_COUNT,
} music_bt_avrcp_command_t;

/** @brief Bounded copies of peer and track metadata. */
typedef struct {
    char device_name[MUSIC_BT_DEVICE_NAME_MAX];
    char title[MUSIC_BT_TITLE_MAX];
    char artist[MUSIC_BT_ARTIST_MAX];
    char album[MUSIC_BT_ALBUM_MAX];
} music_bt_metadata_t;

/** @brief Generation-tagged Bluetooth event with borrowed metadata strings. */
typedef struct {
    uint32_t generation;
    music_bt_event_type_t type;
    const char *device_name;
    const char *title;
    const char *artist;
    const char *album;
    bool playing;
    music_bt_playback_status_t playback_status;
} music_bt_event_t;

/** @brief Result of applying a Bluetooth event. */
typedef enum {
    MUSIC_BT_EVENT_DROPPED = 0,
    MUSIC_BT_EVENT_APPLIED,
    MUSIC_BT_EVENT_NOOP,
} music_bt_event_result_t;

/** @brief Bluetooth session, playback intent and track metadata. */
typedef struct {
    bool open;
    music_bt_connection_t connection;
    music_bt_stream_t stream;
    bool playing;
    bool playback_status_known;
    bool playback_paused_latched;
    bool playback_start_pending;
    bool playback_stop_pending;
    uint32_t generation;
    uint32_t delivered_events;
    music_bt_metadata_t metadata;
} music_bt_state_t;

/**
 * @brief Initialize closed Bluetooth playback state.
 * @param state State used by the operation.
 */
void music_bt_state_init(music_bt_state_t *state);
/**
 * @brief Start a Bluetooth state session and return its new generation.
 * @param state State used by the operation.
 * @return Generation of the new Bluetooth session.
 */
uint32_t music_bt_state_begin(music_bt_state_t *state);
/**
 * @brief Close the state session and clear peer playback information.
 * @param state State used by the operation.
 */
void music_bt_state_close(music_bt_state_t *state);
/**
 * @brief Apply a matching-generation event to Bluetooth state.
 * @param state State used by the operation.
 * @param event Event to process.
 * @return Result of applying the event to Bluetooth state.
 */
music_bt_event_result_t music_bt_state_apply_event(music_bt_state_t *state,
                                                    const music_bt_event_t *event);
/**
 * @brief Update the requested remote playback state.
 * @param state State used by the operation.
 * @param playing Whether playback is active.
 */
void music_bt_state_set_playing(music_bt_state_t *state, bool playing);

#ifdef __cplusplus
}
#endif

#endif
