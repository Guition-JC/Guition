#ifndef MUSIC_PLAYER_INTERNAL_H
#define MUSIC_PLAYER_INTERNAL_H

#include "music_player_state.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Record a player error and its bounded detail text.
 * @param state State used by the operation.
 * @param error Error value.
 * @param detail Detail text.
 * @return The operation status.
 */
music_player_error_t music_player_state_record_error(music_player_state_t *state,
                                                      music_player_error_t error,
                                                      const char *detail);

#ifdef __cplusplus
}
#endif

#endif
