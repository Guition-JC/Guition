#ifndef MUSIC_PLAYER_H
#define MUSIC_PLAYER_H

#include "music_player_state.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef music_player_error_t (*music_player_backend_fn)(void *context);
typedef music_player_error_t (*music_player_backend_source_fn)(void *context,
                                                                 music_player_source_t source);

/** @brief Injected backend operations and their caller-owned context. */
typedef struct {
    void *context;
    music_player_backend_fn global_init;
    music_player_backend_fn open;
    music_player_backend_fn close;
    music_player_backend_fn stop;
    music_player_backend_source_fn select_source;
} music_player_backend_t;

/** @brief Player state and backend lifecycle controller. */
typedef struct {
    music_player_state_t state;
    music_player_backend_t backend;
    bool process_global_initialized;
    bool operation_active;
} music_player_controller_t;

/**
 * @brief Initialize the controller with backend callbacks and their injected context.
 * @param controller Controller instance.
 * @param backend Backend operations and context.
 * @return The operation status.
 */
music_player_error_t music_player_controller_init(music_player_controller_t *controller,
                                                    const music_player_backend_t *backend);
/**
 * @brief Open playback resources through the configured backend.
 * @param controller Controller instance.
 * @return The operation status.
 */
music_player_error_t music_player_controller_open(music_player_controller_t *controller);
/**
 * @brief Close playback resources and update player state.
 * @param controller Controller instance.
 * @return The operation status.
 */
music_player_error_t music_player_controller_close(music_player_controller_t *controller);
/**
 * @brief Select the local SD or Bluetooth playback source.
 * @param controller Controller instance.
 * @param source Source to use.
 * @return The operation status.
 */
music_player_error_t music_player_controller_select_source(music_player_controller_t *controller,
                                                             music_player_source_t source);
/**
 * @brief Select a track index and reset its media state.
 * @param controller Controller instance.
 * @param track_index Zero-based track index.
 * @return The operation status.
 */
music_player_error_t music_player_controller_set_track_index(music_player_controller_t *controller,
                                                                 uint32_t track_index);
/**
 * @brief Update the track index while retaining current media information.
 * @param controller Controller instance.
 * @param track_index Zero-based track index.
 * @return The operation status.
 */
music_player_error_t music_player_controller_set_track_index_preserve_media(
    music_player_controller_t *controller, uint32_t track_index);
/**
 * @brief Clear track metadata, lyrics and position.
 * @param controller Controller instance.
 * @return The operation status.
 */
music_player_error_t music_player_controller_reset_media(music_player_controller_t *controller);
/**
 * @brief Update the shared playing or paused state.
 * @param controller Controller instance.
 * @param playing Whether playback is active.
 * @return The operation status.
 */
music_player_error_t music_player_controller_update_playback(music_player_controller_t *controller,
                                                               bool playing);
/**
 * @brief Apply available track metadata and timing fields.
 * @param controller Controller instance.
 * @param media Media metadata and timing update.
 * @return The operation status.
 */
music_player_error_t music_player_controller_set_metadata(music_player_controller_t *controller,
                                                            const music_player_media_update_t *media);
/**
 * @brief Update elapsed and total playback time in milliseconds.
 * @param controller Controller instance.
 * @param elapsed_ms Elapsed time in milliseconds.
 * @param duration_ms Duration in milliseconds.
 * @return The operation status.
 */
music_player_error_t music_player_controller_set_progress(music_player_controller_t *controller,
                                                           uint32_t elapsed_ms,
                                                           uint32_t duration_ms);
/**
 * @brief Record a backend playback failure and its detail text.
 * @param controller Controller instance.
 * @param detail Detail text.
 * @return The operation status.
 */
music_player_error_t music_player_controller_on_backend_error(music_player_controller_t *controller,
                                                               const char *detail);
/**
 * @brief Record track completion and its detail text.
 * @param controller Controller instance.
 * @param detail Detail text.
 * @return The operation status.
 */
music_player_error_t music_player_controller_on_backend_end(music_player_controller_t *controller,
                                                             const char *detail);
/**
 * @brief Copy the current player state into a caller-owned snapshot.
 * @param controller Controller instance.
 * @param[out] snapshot Receives the copied state.
 */
void music_player_controller_snapshot(const music_player_controller_t *controller,
                                      music_player_snapshot_t *snapshot);

/**
 * @brief Initialize the shared player controller for UI and backend integration.
 * @param backend Backend operations and context.
 * @return The operation status.
 */
music_player_error_t music_player_init(const music_player_backend_t *backend);
/**
 * @brief Open playback resources through the configured backend.
 * @return The operation status.
 */
music_player_error_t music_player_open(void);
/**
 * @brief Close playback resources and update player state.
 * @return The operation status.
 */
music_player_error_t music_player_close(void);
/**
 * @brief Select the local SD or Bluetooth playback source.
 * @param source Source to use.
 * @return The operation status.
 */
music_player_error_t music_player_select_source(music_player_source_t source);
/**
 * @brief Select a track index and reset its media state.
 * @param track_index Zero-based track index.
 * @return The operation status.
 */
music_player_error_t music_player_set_track_index(uint32_t track_index);
/**
 * @brief Update the track index while retaining current media information.
 * @param track_index Zero-based track index.
 * @return The operation status.
 */
music_player_error_t music_player_set_track_index_preserve_media(uint32_t track_index);
/**
 * @brief Clear track metadata, lyrics and position.
 * @return The operation status.
 */
music_player_error_t music_player_reset_media(void);
/**
 * @brief Update the shared playing or paused state.
 * @param playing Whether playback is active.
 * @return The operation status.
 */
music_player_error_t music_player_update_playback(bool playing);
/**
 * @brief Apply available track metadata and timing fields.
 * @param media Media metadata and timing update.
 * @return The operation status.
 */
music_player_error_t music_player_set_metadata(const music_player_media_update_t *media);
/**
 * @brief Update elapsed and total playback time in milliseconds.
 * @param elapsed_ms Elapsed time in milliseconds.
 * @param duration_ms Duration in milliseconds.
 * @return The operation status.
 */
music_player_error_t music_player_set_progress(uint32_t elapsed_ms, uint32_t duration_ms);
/**
 * @brief Record a backend playback failure and its detail text.
 * @param detail Detail text.
 * @return The operation status.
 */
music_player_error_t music_player_on_backend_error(const char *detail);
/**
 * @brief Record track completion and its detail text.
 * @param detail Detail text.
 * @return The operation status.
 */
music_player_error_t music_player_on_backend_end(const char *detail);
/**
 * @brief Copy the current player state into a caller-owned snapshot.
 * @param[out] snapshot Receives the copied state.
 */
void music_player_snapshot(music_player_snapshot_t *snapshot);

/**
 * @brief Store a bounded copy of cover art under the player lock.
 * @param format_fourcc Cover image format as a FourCC.
 * @param data Data used by the operation.
 * @param size Size of the data.
 * @return true if the cover art was stored successfully.
 */
bool music_player_set_cover_art(uint32_t format_fourcc, const uint8_t *data, size_t size);
/**
 * @brief Clear the cover descriptor and release its owned image buffer.
 */
void music_player_clear_cover_art(void);
/**
 * @brief Copy cover bytes for a matching descriptor revision under the player lock.
 * @param capacity Destination capacity in bytes.
 * @param offset Source byte offset.
 * @return Number of bytes copied, or zero when unavailable or stale.
 */
size_t music_player_cover_art_copy(uint32_t revision, void *destination, size_t capacity,
                                   size_t offset);

#ifdef __cplusplus
}
#endif

#endif
