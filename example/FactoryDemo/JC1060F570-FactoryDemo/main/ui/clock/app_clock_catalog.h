/**
 * SPDX-License-Identifier: GPL-2.0-only
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define CLOCK_CATALOG_MAX 64
#define CLOCK_FACE_PATH_MAX 192
#define CLOCK_FACE_ID_MAX 80
#define CLOCK_FACE_PARTS 5

enum { CLOCK_PART_DIAL, CLOCK_PART_HOUR, CLOCK_PART_MINUTE,
       CLOCK_PART_SECOND, CLOCK_PART_CAP };

typedef struct {
    char path[CLOCK_FACE_PATH_MAX]; /**< POSIX path; the renderer prepends A:. */
    int32_t width, height;
    int32_t pivot_x, pivot_y;
} clock_face_part_t;

typedef struct {
    char id[CLOCK_FACE_ID_MAX];
    bool sd;

    bool failed; /**< Skip failed assets until the next catalog scan. */
    int32_t dial_x, dial_y, center_x, center_y;
    clock_face_part_t part[CLOCK_FACE_PARTS];
} clock_face_spec_t;

typedef struct {
    size_t count;

    clock_face_spec_t faces[CLOCK_CATALOG_MAX]; /**< Index zero is the drawn face. */
} clock_face_catalog_t;

/**
 * @brief Load and validate face geometry and image paths for the panel size.
 * @param root Directory containing placement.json and image assets.
 * @param width Panel width in pixels.
 * @param height Panel height in pixels.
 * @param[out] face Receives the face geometry and asset paths.
 * @return true when the manifest and its assets pass validation.
 */
bool clock_face_manifest(const char *root, int32_t width, int32_t height,
                         clock_face_spec_t *face);

/**
 * @brief Discover built-in and SD faces after the default drawn face.
 * @param[out] catalog Catalog to initialize and populate.
 * @param builtin_root Built-in face directory.
 * @param sd_root SD directory containing face subdirectories, or NULL.
 * @param width Panel width in pixels.
 * @param height Panel height in pixels.
 */
void clock_face_catalog_scan(clock_face_catalog_t *catalog, const char *builtin_root,
                             const char *sd_root, int32_t width, int32_t height);

/**
 * @brief Find the next usable face in the requested direction.
 * @param catalog Catalog to search.
 * @param current Index of the current face.
 * @param direction Negative searches backward; zero or positive searches forward.
 * @return Index of the next non-failed face, or zero if none can be selected.
 */
size_t clock_face_neighbor(const clock_face_catalog_t *catalog, size_t current, int direction);

/**
 * @brief Return unique current/previous/next indices; sleep mode keeps only the current face.
 * @param catalog Catalog to search.
 * @param current Index of the current face.
 * @param sleep Whether to retain only the current face.
 * @param[out] indices Array receiving up to three face indices.
 * @return Number of indices written, or zero for invalid input.
 */
size_t clock_face_window(const clock_face_catalog_t *catalog, size_t current,
                         bool sleep, size_t indices[3]);
