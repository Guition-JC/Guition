#ifndef MUSIC_SD_CATALOG_H
#define MUSIC_SD_CATALOG_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MUSIC_SD_CATALOG_MAX_ENTRIES 64U
#define MUSIC_SD_CATALOG_ROOT_MAX 192U
#define MUSIC_SD_CATALOG_NAME_MAX 96U
#define MUSIC_SD_CATALOG_PATH_MAX (MUSIC_SD_CATALOG_ROOT_MAX + MUSIC_SD_CATALOG_NAME_MAX + 2U)
#define MUSIC_SD_CATALOG_TITLE_MAX 64U
#define MUSIC_SD_CATALOG_ARTIST_MAX 64U
#define MUSIC_SD_CATALOG_MAX_FILE_SIZE (64ULL * 1024ULL * 1024ULL)
#define MUSIC_SD_UNKNOWN_TITLE_KEY "music.unknown_title"
#define MUSIC_SD_UNKNOWN_ARTIST_KEY "music.unknown_artist"
#define MUSIC_SD_LYRICS_UNAVAILABLE_KEY "music.lyrics_unavailable"

/** @brief Supported local audio container. */
typedef enum {
    MUSIC_SD_MEDIA_MP3 = 0,
    MUSIC_SD_MEDIA_WAV,
    MUSIC_SD_MEDIA_AAC,
} music_sd_media_type_t;

/** @brief Catalog scan or merge result. */
typedef enum {
    MUSIC_SD_CATALOG_OK = 0,
    MUSIC_SD_CATALOG_INVALID_ARGUMENT,
    MUSIC_SD_CATALOG_PATH_UNSAFE,
    MUSIC_SD_CATALOG_SD_UNAVAILABLE,
    MUSIC_SD_CATALOG_EMPTY,
    MUSIC_SD_CATALOG_TOO_MANY_ENTRIES,
} music_sd_catalog_error_t;

/** @brief Borrowed directory-entry data supplied to catalog scanning. */
typedef struct {
    const char *name;
    const char *title;
    const char *artist;
    uint64_t size_bytes;
    bool is_directory;
} music_sd_catalog_dir_entry_t;

/** @brief Owned local track path, media metadata and duration. */
typedef struct {
    char name[MUSIC_SD_CATALOG_NAME_MAX];
    char path[MUSIC_SD_CATALOG_PATH_MAX];
    char title[MUSIC_SD_CATALOG_TITLE_MAX];
    char artist[MUSIC_SD_CATALOG_ARTIST_MAX];
    music_sd_media_type_t type;
    uint64_t size_bytes;
    uint32_t duration_ms;
} music_sd_catalog_entry_t;

/** @brief Bounded collection of local tracks. */
typedef struct {
    music_sd_catalog_entry_t entries[MUSIC_SD_CATALOG_MAX_ENTRIES];
    size_t count;
} music_sd_catalog_t;

/**
 * @brief Clear all local catalog entries.
 * @param catalog Catalog instance.
 */
void music_sd_catalog_reset(music_sd_catalog_t *catalog);

/**
 * @brief Begin a multi-root catalog merge; append roots in priority order, then finish to sort. First matching name wins.
 * @param catalog Catalog instance.
 */
void music_sd_catalog_begin(music_sd_catalog_t *catalog);
/**
 * @brief Merge directory entries under a root; retain the first entry for duplicate names.
 * @param catalog Catalog instance.
 * @param root Root path.
 * @param entries Catalog entries.
 * @param entry_count Number of entries.
 * @return The operation status.
 */
music_sd_catalog_error_t music_sd_catalog_append(
    music_sd_catalog_t *catalog, const char *root,
    const music_sd_catalog_dir_entry_t *entries, size_t entry_count);
/**
 * @brief Sort the merged catalog and report empty or invalid results.
 * @param catalog Catalog instance.
 * @return The operation status.
 */
music_sd_catalog_error_t music_sd_catalog_finish(music_sd_catalog_t *catalog);

/**
 * @brief Build a bounded catalog from the supplied directory entries.
 * @param root Root path.
 * @param entries Catalog entries.
 * @param entry_count Number of entries.
 * @param catalog Catalog instance.
 * @return The operation status.
 */
music_sd_catalog_error_t music_sd_catalog_scan(
    const char *root, const music_sd_catalog_dir_entry_t *entries,
    size_t entry_count, music_sd_catalog_t *catalog);
/**
 * @brief Check that a local filename contains no unsafe path components.
 * @param name Name to use.
 * @return true if the name contains no unsafe path components.
 */
bool music_sd_catalog_name_is_safe(const char *name);
/**
 * @brief Recognize a supported filename extension and report its media type.
 * @param name Name to use.
 * @param type Type to use.
 * @return true if the filename extension is supported.
 */
bool music_sd_catalog_is_supported_name(const char *name, music_sd_media_type_t *type);

#ifdef __cplusplus
}
#endif

#endif
