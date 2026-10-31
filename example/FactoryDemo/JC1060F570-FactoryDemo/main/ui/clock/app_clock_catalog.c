/**
 * SPDX-License-Identifier: GPL-2.0-only
 */
#include "app_clock_catalog.h"

#include <dirent.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include "cJSON.h"

#define MANIFEST_BYTES_MAX 8192
#define SCAN_ENTRIES_MAX 256

static bool safe_name(const char *name)
{
    if (!name || !*name || *name == '.' || strlen(name) >= CLOCK_FACE_ID_MAX - 4) return false;
    for (const unsigned char *p = (const unsigned char *)name; *p; ++p) {
        if (*p < 32 || *p == 127 || *p == '/' || *p == '\\' || *p == ':') return false;
    }
    return true;
}

static bool pair(const cJSON *object, const char *key, int32_t *x, int32_t *y)
{
    const cJSON *a = cJSON_GetObjectItemCaseSensitive(object, key);
    if (!cJSON_IsArray(a) || cJSON_GetArraySize(a) != 2) return false;
    const cJSON *first = cJSON_GetArrayItem(a, 0), *second = cJSON_GetArrayItem(a, 1);
    if (!cJSON_IsNumber(first) || !cJSON_IsNumber(second)) return false;
    double v[2] = {first->valuedouble, second->valuedouble};
    for (size_t i = 0; i < 2; ++i) {
        if (!isfinite(v[i]) || v[i] < 0 || v[i] > 4096 || floor(v[i]) != v[i]) return false;
    }
    *x = (int32_t)v[0]; *y = (int32_t)v[1];
    return true;
}

static bool image_file(const cJSON *object, const char *key, const char *root,
                        clock_face_part_t *part)
{
    const cJSON *file = cJSON_GetObjectItemCaseSensitive(object, key);
    if (!cJSON_IsString(file) || !safe_name(file->valuestring)) return false;
    const char *ext = strrchr(file->valuestring, '.');
    if (!ext || (strcmp(ext, ".png") && strcmp(ext, ".jpg") && strcmp(ext, ".jpeg"))) return false;
    int n = snprintf(part->path, sizeof(part->path), "%s/%s", root, file->valuestring);
    struct stat st;
    return n > 0 && (size_t)n < sizeof(part->path) && stat(part->path, &st) == 0 &&
           S_ISREG(st.st_mode) && st.st_size > 0;
}

static bool dimensions(const clock_face_part_t *part, int32_t w, int32_t h)
{
    return part->width > 0 && part->height > 0 && part->width <= w && part->height <= h;
}

/**
 * @brief Validate manifest geometry and required face images for the target panel.
 * @param root Directory containing the manifest and images.
 * @param width Panel width in pixels.
 * @param height Panel height in pixels.
 * @param[out] face Receives validated face geometry and paths.
 * @return true if the manifest and images are valid.
 */
bool clock_face_manifest(const char *root, int32_t width, int32_t height,
                         clock_face_spec_t *face)
{
    if (!root || !face || width <= 0 || height <= 0) return false;
    memset(face, 0, sizeof(*face));
    char path[CLOCK_FACE_PATH_MAX];
    int n = snprintf(path, sizeof(path), "%s/placement.json", root);
    if (n < 0 || (size_t)n >= sizeof(path)) return false;
    FILE *fp = fopen(path, "rb");
    if (!fp) return false;
    char *text = malloc(MANIFEST_BYTES_MAX + 1);
    if (!text) { fclose(fp); return false; }
    size_t length = fread(text, 1, MANIFEST_BYTES_MAX, fp);
    bool fits = !ferror(fp) && fgetc(fp) == EOF;
    fclose(fp);
    text[length] = '\0';
    cJSON *json = fits ? cJSON_ParseWithLengthOpts(text, length + 1, NULL, true) : NULL;
    free(text);
    if (!json) return false;

    int32_t w = 0, h = 0;
    clock_face_part_t *dial = &face->part[CLOCK_PART_DIAL];
    bool ok = cJSON_IsObject(json) && pair(json, "screen", &w, &h) && w == width && h == height &&
              pair(json, "dial_size", &dial->width, &dial->height) && dimensions(dial, w, h) &&
              pair(json, "dial_top_left", &face->dial_x, &face->dial_y) &&
              face->dial_x + dial->width <= w && face->dial_y + dial->height <= h &&
              pair(json, "screen_pivot", &face->center_x, &face->center_y) &&
              face->center_x >= face->dial_x && face->center_x < face->dial_x + dial->width &&
              face->center_y >= face->dial_y && face->center_y < face->dial_y + dial->height &&
              image_file(json, "dial_file", root, dial);
    const cJSON *hands = cJSON_GetObjectItemCaseSensitive(json, "hands");
    static const char *keys[] = {"hour_hand_12", "minute_hand_12", "second_hand_12", "center_cap"};
    ok = ok && cJSON_IsObject(hands);
    for (size_t i = 1; ok && i < CLOCK_FACE_PARTS; ++i) {
        const cJSON *hand = cJSON_GetObjectItemCaseSensitive(hands, keys[i - 1]);
        if (!hand && i >= CLOCK_PART_SECOND) continue;
        clock_face_part_t *part = &face->part[i];
        ok = cJSON_IsObject(hand) && pair(hand, "size", &part->width, &part->height) &&
             dimensions(part, w, h) && pair(hand, "pivot", &part->pivot_x, &part->pivot_y) &&
             part->pivot_x < part->width && part->pivot_y < part->height &&
             image_file(hand, "file", root, part);
    }
    cJSON_Delete(json);
    if (!ok) memset(face, 0, sizeof(*face));
    return ok;
}

static int by_id(const void *a, const void *b)
{
    return strcmp(((const clock_face_spec_t *)a)->id, ((const clock_face_spec_t *)b)->id);
}

/**
 * @brief Collect built-in faces and SD manifests after the drawn fallback.
 * @param[out] catalog Catalog to populate.
 * @param builtin_root Built-in face directory.
 * @param sd_root SD directory containing face subdirectories, or NULL.
 * @param width Panel width in pixels.
 * @param height Panel height in pixels.
 */
void clock_face_catalog_scan(clock_face_catalog_t *catalog, const char *builtin_root,
                             const char *sd_root, int32_t width, int32_t height)
{
    if (!catalog) return;
    memset(catalog, 0, sizeof(*catalog));
    catalog->count = 1;
    strcpy(catalog->faces[0].id, "drawn");

    if (clock_face_manifest(builtin_root, width, height, &catalog->faces[1])) {
        strcpy(catalog->faces[1].id, "builtin:classic");
        ++catalog->count;
    }
    size_t sd_first = catalog->count;
    DIR *dir = sd_root ? opendir(sd_root) : NULL;
    if (!dir) return;
    struct dirent *entry;
    size_t scanned = 0;
    while ((entry = readdir(dir)) != NULL && scanned++ < SCAN_ENTRIES_MAX &&
           catalog->count < CLOCK_CATALOG_MAX) {
        if (!safe_name(entry->d_name)) continue;
        char root[CLOCK_FACE_PATH_MAX];
        int n = snprintf(root, sizeof(root), "%s/%s", sd_root, entry->d_name);
        if (n < 0 || (size_t)n >= sizeof(root)) continue;
        clock_face_spec_t *face = &catalog->faces[catalog->count];
        if (!clock_face_manifest(root, width, height, face)) continue;
        snprintf(face->id, sizeof(face->id), "sd:%.*s", CLOCK_FACE_ID_MAX - 4, entry->d_name);
        face->sd = true;
        ++catalog->count;
    }
    closedir(dir);
    qsort(catalog->faces + sd_first, catalog->count - sd_first, sizeof(catalog->faces[0]), by_id);
}

/**
 * @brief Wrap through the catalog while skipping failed entries.
 * @param catalog Catalog to search.
 * @param current Current face index.
 * @param direction Negative searches backward; zero or positive searches forward.
 * @return Index of the next non-failed face, or zero if none can be selected.
 */
size_t clock_face_neighbor(const clock_face_catalog_t *catalog, size_t current, int direction)
{
    if (!catalog || !catalog->count || current >= catalog->count) return 0;
    size_t candidate = current;
    for (size_t i = 0; i < catalog->count; ++i) {
        candidate = direction < 0 ? (candidate + catalog->count - 1) % catalog->count :
                                   (candidate + 1) % catalog->count;
        if (!catalog->faces[candidate].failed) return candidate;
    }
    return 0;
}

/**
 * @brief Build the unique visible and adjacent preload window.
 * @param catalog Catalog to search.
 * @param current Current face index.
 * @param sleep Whether to retain only the current face.
 * @param[out] indices Array receiving up to three indices.
 * @return Number of indices written, or zero for invalid input.
 */
size_t clock_face_window(const clock_face_catalog_t *catalog, size_t current,
                         bool sleep, size_t indices[3])
{
    if (!catalog || !catalog->count || current >= catalog->count || !indices) return 0;
    size_t count = 1;
    indices[0] = current;
    if (sleep) return count;
    for (int direction = -1; direction <= 1; direction += 2) {
        size_t next = clock_face_neighbor(catalog, current, direction);
        bool found = false;
        for (size_t i = 0; i < count; ++i) found |= indices[i] == next;
        if (!found) indices[count++] = next;
    }
    return count;
}
