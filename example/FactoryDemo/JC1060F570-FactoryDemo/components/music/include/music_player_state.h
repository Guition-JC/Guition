#ifndef MUSIC_PLAYER_STATE_H
#define MUSIC_PLAYER_STATE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MUSIC_PLAYER_SOURCE_NAME_MAX 16U
#define MUSIC_PLAYER_TRACK_TITLE_MAX 64U
#define MUSIC_PLAYER_ARTIST_MAX 64U
#define MUSIC_PLAYER_LYRIC_MAX 96U
#define MUSIC_PLAYER_DETAIL_MAX 96U
#define MUSIC_PLAYER_ALBUM_MAX 64U
#define MUSIC_PLAYER_COVER_ART_MAX_SIZE (128U * 1024U)

/** @brief Selectable local or Bluetooth playback source. */
typedef enum {
    MUSIC_PLAYER_SOURCE_SD_CARD = 0,
    MUSIC_PLAYER_SOURCE_CLASSIC_BT,
    MUSIC_PLAYER_SOURCE_COUNT,
} music_player_source_t;

/** @brief Player command and backend error code. */
typedef enum {
    MUSIC_PLAYER_ERROR_OK = 0,
    MUSIC_PLAYER_ERROR_INVALID_ARGUMENT,
    MUSIC_PLAYER_ERROR_NOT_INITIALIZED,
    MUSIC_PLAYER_ERROR_NOT_OPEN,
    MUSIC_PLAYER_ERROR_INVALID_SOURCE,
    MUSIC_PLAYER_ERROR_INVALID_COMMAND,
    MUSIC_PLAYER_ERROR_BACKEND,
    MUSIC_PLAYER_ERROR_END_OF_TRACK,
    MUSIC_PLAYER_ERROR_BUSY,
    MUSIC_PLAYER_ERROR_SD_UNAVAILABLE,
} music_player_error_t;

/** @brief Player transport state. */
typedef enum {
    MUSIC_PLAYER_PLAYBACK_CLOSED = 0,
    MUSIC_PLAYER_PLAYBACK_PAUSED,
    MUSIC_PLAYER_PLAYBACK_PLAYING,
    MUSIC_PLAYER_PLAYBACK_ENDED,
    MUSIC_PLAYER_PLAYBACK_ERROR,
} music_player_playback_t;

/** @brief Player lifecycle and media update event. */
typedef enum {
    MUSIC_PLAYER_STATE_EVENT_OPEN = 0,
    MUSIC_PLAYER_STATE_EVENT_CLOSE,
    MUSIC_PLAYER_STATE_EVENT_SOURCE_SELECTED,
    MUSIC_PLAYER_STATE_EVENT_TRACK_SELECTED,
    MUSIC_PLAYER_STATE_EVENT_TRACK_CHANGED,
    MUSIC_PLAYER_STATE_EVENT_MEDIA_RESET,
    MUSIC_PLAYER_STATE_EVENT_PLAY,
    MUSIC_PLAYER_STATE_EVENT_PAUSE,
    MUSIC_PLAYER_STATE_EVENT_METADATA,
    MUSIC_PLAYER_STATE_EVENT_PROGRESS,
    MUSIC_PLAYER_STATE_EVENT_BACKEND_ERROR,
    MUSIC_PLAYER_STATE_EVENT_BACKEND_END,
} music_player_state_event_type_t;

/** @brief Optional media text and playback timing update. */
typedef struct {
    const char *title;
    const char *artist;
    const char *album;
    const char *current_lyric;
    const char *next_lyric;
    uint32_t elapsed_ms;
    uint32_t duration_ms;
    bool update_position; /**< true supplies a known elapsed position. */
    bool update_duration; /**< true supplies a nonzero duration, independently of position. */
} music_player_media_update_t;

/** @brief Input to the player state reducer. */
typedef struct {
    music_player_state_event_type_t type;
    music_player_source_t source;
    music_player_error_t error;
    const char *detail;
    const music_player_media_update_t *media;
    uint32_t elapsed_ms;
    uint32_t duration_ms;
    uint32_t track_index;
} music_player_state_event_t;

/** @brief Cover-art format, byte size and revision for copying image data. */
typedef struct {
    bool available;
    uint32_t format_fourcc;
    uint32_t size;
    uint32_t revision;
} music_player_cover_art_descriptor_t;

/** @brief Player lifecycle, transport and bounded media state. */
typedef struct {
    bool initialized;
    bool open;
    music_player_source_t source;
    music_player_playback_t playback;
    music_player_error_t last_error;
    uint32_t track_index;
    uint32_t revision;
    uint32_t elapsed_ms;
    uint32_t duration_ms;
    bool position_known;
    char track_title[MUSIC_PLAYER_TRACK_TITLE_MAX];
    char artist[MUSIC_PLAYER_ARTIST_MAX];
    char album[MUSIC_PLAYER_ALBUM_MAX];
    char current_lyric[MUSIC_PLAYER_LYRIC_MAX];
    char next_lyric[MUSIC_PLAYER_LYRIC_MAX];
    char detail[MUSIC_PLAYER_DETAIL_MAX];
} music_player_state_t;

/** @brief Display-ready copy of player state and cover descriptor. */
typedef struct {
    bool initialized;
    bool open;
    music_player_source_t source;
    music_player_playback_t playback;
    music_player_error_t last_error;
    uint32_t track_index;
    uint32_t revision;
    uint32_t elapsed_ms;
    uint32_t duration_ms;
    bool position_known;
    char source_name[MUSIC_PLAYER_SOURCE_NAME_MAX];
    char track_title[MUSIC_PLAYER_TRACK_TITLE_MAX];
    char artist[MUSIC_PLAYER_ARTIST_MAX];
    char album[MUSIC_PLAYER_ALBUM_MAX];
    char current_lyric[MUSIC_PLAYER_LYRIC_MAX];
    char next_lyric[MUSIC_PLAYER_LYRIC_MAX];
    char detail[MUSIC_PLAYER_DETAIL_MAX];
    music_player_cover_art_descriptor_t cover_art;
} music_player_snapshot_t;

/**
 * @brief Reset the player state to its initial defaults.
 * @param state State used by the operation.
 */
void music_player_state_reset(music_player_state_t *state);
/**
 * @brief Apply a player event and update state or return a transition error.
 * @param state State used by the operation.
 * @param event Event to process.
 * @return The operation status.
 */
music_player_error_t music_player_state_reduce(music_player_state_t *state,
                                                const music_player_state_event_t *event);
/**
 * @brief Copy state into a display-ready player snapshot.
 * @param state State used by the operation.
 * @param[out] snapshot Receives the copied state.
 */
void music_player_state_snapshot(const music_player_state_t *state,
                                 music_player_snapshot_t *snapshot);
/**
 * @brief Return the readable name of a playback source.
 * @param source Source to use.
 * @return Readable playback-source name.
 */
const char *music_player_source_name(music_player_source_t source);

#ifdef __cplusplus
}
#endif

#endif
