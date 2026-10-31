/** SPDX-License-Identifier: GPL-2.0-only */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Bounded catalog limits; all path storage is fixed size and never truncated silently. */
#define PHOTO_CATALOG_MAX_ENTRIES 64U
#define PHOTO_CATALOG_NAME_MAX 96U
#define PHOTO_CATALOG_PATH_MAX 256U

/** @brief Decoded footprint per source pixel: JPEG family decodes to RGB565, PNG to ARGB8888. */
#define PHOTO_CATALOG_JPEG_BPP 2U
#define PHOTO_CATALOG_PNG_BPP 4U

/** @brief Initial per-image decode budget (4 MiB). Images above this are skipped with a notice. */
#define PHOTO_CATALOG_DECODE_BUDGET_BYTES (4U * 1024U * 1024U)

/** @brief Photo storage source. */
typedef enum {
    PHOTO_SOURCE_SD = 0,
    PHOTO_SOURCE_BUILTIN = 1,
} photo_source_kind_t;

/** @brief Photo catalog operation result. */
typedef enum {
    PHOTO_CATALOG_OK = 0,
    PHOTO_CATALOG_INVALID_ARGUMENT,
    PHOTO_CATALOG_EMPTY,
    PHOTO_CATALOG_TOO_MANY_ENTRIES,
} photo_catalog_error_t;

/** @brief One raw directory entry produced by the scanner. */
typedef struct {
    char name[PHOTO_CATALOG_NAME_MAX];
    uint64_t size_bytes;
} photo_dir_entry_t;

/** @brief One scanned directory: the same names exist under a POSIX root and an LVGL drive path. */
typedef struct {
    const char *posix_root;
    const char *lv_root;
    photo_source_kind_t source;
    const photo_dir_entry_t *entries;
    size_t count;
} photo_catalog_source_t;

/** @brief Resolved photo path and storage source. */
typedef struct {
    char name[PHOTO_CATALOG_NAME_MAX];
    char lv_path[PHOTO_CATALOG_PATH_MAX];
    photo_source_kind_t source;
    bool sliced;
} photo_catalog_entry_t;

/** @brief Bounded merged photo catalog. */
typedef struct {
    photo_catalog_entry_t entries[PHOTO_CATALOG_MAX_ENTRIES];
    size_t count;
} photo_catalog_t;

/**
 * @brief True when a directory entry name may be handed to the filesystem.
 * @param name Name to use.
 * @return True when a directory entry name may be handed to the filesystem.
 */
bool photo_catalog_name_is_safe(const char *name);

/**
 * @brief True for displayable images: jpg, jpeg, png and their sliced variants.
 * @param name Name to use.
 * @return True for displayable images: jpg, jpeg, png and their sliced variants.
 */
bool photo_catalog_is_supported_name(const char *name);

/**
 * @brief True for esp_lv_decoder sliced formats (_SJPG__/_SPNG__): .sjpg / .spng.
 * @param name Name to use.
 * @return True for esp_lv_decoder sliced formats (_SJPG__/_SPNG__): .sjpg / .spng.
 */
bool photo_catalog_is_sliced_name(const char *name);

/**
 * @brief Return the extension without its dot, or an empty string when absent.
 * @param name Name to use.
 * @return Extension without its dot, or an empty string if absent.
 */
const char *photo_catalog_extension(const char *name);

/**
 * @brief Bytes per decoded source pixel for a path/name; unknown extensions count as JPEG.
 * @param path Path to use.
 * @return Bytes per decoded source pixel for a path/name; unknown extensions count as JPEG.
 */
uint32_t photo_catalog_bytes_per_pixel(const char *path);

/**
 * @brief Clear the photo catalog.
 * @param catalog Catalog instance.
 */
void photo_catalog_reset(photo_catalog_t *catalog);

/**
 * @brief Merge sources in order, preferring sliced images for duplicate base names. Return EMPTY
 * for no entries or TOO_MANY_ENTRIES with a usable truncated catalog.
 */
photo_catalog_error_t photo_catalog_merge(const photo_catalog_source_t *sources,
                                          size_t source_count,
                                          photo_catalog_t *catalog);

/**
 * @brief Wrap-around index helpers; count == 0 returns 0.
 * @param index Index of the item.
 * @param count Number of items.
 * @return Next index with wraparound, or zero if the catalog is empty.
 */
size_t photo_catalog_next_index(size_t index, size_t count);
/**
 * @brief Return the preceding index with wraparound; an empty catalog yields zero.
 * @param index Index of the item.
 * @param count Number of items.
 * @return The preceding index with wraparound; an empty catalog yields zero.
 */
size_t photo_catalog_prev_index(size_t index, size_t count);

/**
 * @brief LVGL image scale (256 == 100%) that fits the source into the screen without ever
 * upscaling: images at or below the screen size stay at 256.
 */
uint16_t photo_catalog_scale_for_screen(uint16_t source_w, uint16_t source_h,
                                        uint16_t screen_w, uint16_t screen_h);

/**
 * @brief Compute decoded byte size with saturation on overflow.
 * @param width Width in pixels.
 * @param height Height in pixels.
 * @param bytes_per_pixel Number of bytes per pixel.
 * @return Decoded byte size, saturated on overflow.
 */
uint32_t photo_catalog_decode_bytes(uint32_t width, uint32_t height, uint32_t bytes_per_pixel);

/**
 * @brief True when the decoded size of width x height fits PHOTO_CATALOG_DECODE_BUDGET_BYTES.
 * @param path Path to use.
 * @param width Width in pixels.
 * @param height Height in pixels.
 * @return True when the decoded size of width x height fits PHOTO_CATALOG_DECODE_BUDGET_BYTES.
 */
bool photo_catalog_within_decode_budget(const char *path, uint32_t width, uint32_t height);

/**
 * @brief Build "<root>/<name>" for both the POSIX root and the LVGL drive root.
 * @param posix_root POSIX root path.
 * @param lv_root LVGL root path.
 * @param name Name to use.
 * @param posix_out Destination for the POSIX path.
 * @param posix_capacity POSIX path buffer capacity.
 * @param lv_out Destination for the LVGL path.
 * @param lv_capacity LVGL path buffer capacity.
 * @return true if both paths were built successfully.
 */
bool photo_catalog_build_paths(const char *posix_root, const char *lv_root, const char *name,
                               char *posix_out, size_t posix_capacity,
                               char *lv_out, size_t lv_capacity);

/**
 * @brief Return the stable name of a photo source.
 * @param source Source to use.
 * @return Stable name of the photo source.
 */
const char *photo_catalog_source_name(photo_source_kind_t source);
/**
 * @brief Return the stable name of a catalog result.
 * @param error Error value.
 * @return Stable name of the catalog result.
 */
const char *photo_catalog_error_name(photo_catalog_error_t error);

#ifdef __cplusplus
}
#endif
