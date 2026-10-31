/** SPDX-License-Identifier: GPL-2.0-only */
#include "photo_catalog.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

static size_t bounded_length(const char *text, size_t limit)
{
    size_t length = 0U;
    if (text != NULL) {
        while (length < limit && text[length] != '\0') {
            ++length;
        }
    }
    return length;
}

static char lower_ascii(char value)
{
    return (char)tolower((unsigned char)value);
}

bool photo_catalog_name_is_safe(const char *name)
{
    const size_t length = bounded_length(name, PHOTO_CATALOG_NAME_MAX);
    if (name == NULL || length == 0U || length >= PHOTO_CATALOG_NAME_MAX || name[0] == '.') {
        return false;
    }
    for (size_t i = 0U; i < length; ++i) {
        const unsigned char ch = (unsigned char)name[i];
        if (ch < 0x20U || ch == 0x7FU || ch == '\\' || ch == '/' || ch == ':') {
            return false;
        }
    }
    return true;
}

const char *photo_catalog_extension(const char *name)
{
    const char *dot = NULL;
    if (name == NULL) {
        return "";
    }
    dot = strrchr(name, '.');
    if (dot == NULL || dot == name || dot[1] == '\0') {
        return "";
    }
    return dot + 1;
}

static bool extension_is(const char *extension, const char *expected)
{
    size_t i = 0U;
    while (extension[i] != '\0' && expected[i] != '\0') {
        if (lower_ascii(extension[i]) != expected[i]) {
            return false;
        }
        ++i;
    }
    return extension[i] == '\0' && expected[i] == '\0';
}

bool photo_catalog_is_sliced_name(const char *name)
{
    if (!photo_catalog_name_is_safe(name)) {
        return false;
    }
    const char *extension = photo_catalog_extension(name);
    return extension_is(extension, "sjpg") || extension_is(extension, "spng");
}

bool photo_catalog_is_supported_name(const char *name)
{
    if (!photo_catalog_name_is_safe(name)) {
        return false;
    }
    const char *extension = photo_catalog_extension(name);
    return extension_is(extension, "jpg") || extension_is(extension, "jpeg") ||
           extension_is(extension, "png") || extension_is(extension, "sjpg") ||
           extension_is(extension, "spng");
}

uint32_t photo_catalog_bytes_per_pixel(const char *path)
{
    const char *extension = photo_catalog_extension(path);
    if (extension_is(extension, "png") || extension_is(extension, "spng")) {
        return PHOTO_CATALOG_PNG_BPP;
    }
    return PHOTO_CATALOG_JPEG_BPP;
}

void photo_catalog_reset(photo_catalog_t *catalog)
{
    if (catalog == NULL) {
        return;
    }
    memset(catalog, 0, sizeof(*catalog));
}

/**
 * @brief Compare the file name without its extension, case-insensitively.
 * @param left First file name.
 * @param right Second file name.
 * @return true if the base names match, ignoring case.
 */
static bool base_name_matches(const char *left, const char *right)
{
    const size_t left_length = (size_t)(strrchr(left, '.') == NULL
                                            ? bounded_length(left, PHOTO_CATALOG_NAME_MAX)
                                            : (size_t)(strrchr(left, '.') - left));
    const size_t right_length = (size_t)(strrchr(right, '.') == NULL
                                             ? bounded_length(right, PHOTO_CATALOG_NAME_MAX)
                                             : (size_t)(strrchr(right, '.') - right));
    if (left_length == 0U || left_length != right_length) {
        return false;
    }
    for (size_t i = 0U; i < left_length; ++i) {
        if (lower_ascii(left[i]) != lower_ascii(right[i])) {
            return false;
        }
    }
    return true;
}

bool photo_catalog_build_paths(const char *posix_root, const char *lv_root, const char *name,
                               char *posix_out, size_t posix_capacity,
                               char *lv_out, size_t lv_capacity)
{
    if (posix_root == NULL || lv_root == NULL || !photo_catalog_name_is_safe(name) ||
        posix_out == NULL || lv_out == NULL || posix_capacity == 0U || lv_capacity == 0U) {
        return false;
    }

    const int posix_written = snprintf(posix_out, posix_capacity, "%s/%s", posix_root, name);
    if (posix_written < 0 || (size_t)posix_written >= posix_capacity) {
        posix_out[0] = '\0';
        return false;
    }
    const int lv_written = snprintf(lv_out, lv_capacity, "%s/%s", lv_root, name);
    if (lv_written < 0 || (size_t)lv_written >= lv_capacity) {
        lv_out[0] = '\0';
        return false;
    }
    return true;
}

static bool source_is_valid(const photo_catalog_source_t *source)
{
    return source != NULL && source->posix_root != NULL && source->lv_root != NULL &&
           source->posix_root[0] != '\0' && source->lv_root[0] != '\0' &&
           (source->entries != NULL || source->count == 0U);
}

photo_catalog_error_t photo_catalog_merge(const photo_catalog_source_t *sources,
                                          size_t source_count,
                                          photo_catalog_t *catalog)
{
    if (sources == NULL || source_count == 0U || catalog == NULL) {
        return PHOTO_CATALOG_INVALID_ARGUMENT;
    }

    photo_catalog_reset(catalog);
    bool truncated = false;

    for (size_t source_index = 0U; source_index < source_count; ++source_index) {
        const photo_catalog_source_t *source = &sources[source_index];
        if (!source_is_valid(source)) {
            return PHOTO_CATALOG_INVALID_ARGUMENT;
        }

        for (size_t i = 0U; i < source->count; ++i) {
            const photo_dir_entry_t *candidate = &source->entries[i];
            if (!photo_catalog_is_supported_name(candidate->name)) {
                continue;
            }

            /** Prefer the sliced file when the directory offers both variants. */
            bool sliced_available = false;
            for (size_t k = 0U; k < source->count; ++k) {
                if (photo_catalog_is_sliced_name(source->entries[k].name) &&
                    base_name_matches(source->entries[k].name, candidate->name)) {
                    sliced_available = true;
                    break;
                }
            }
            if (sliced_available && !photo_catalog_is_sliced_name(candidate->name)) {
                continue;
            }

            /** One representative per base name: the first sliced entry, or the first entry when the directory has no sliced variant. */
            bool seen_earlier = false;
            for (size_t j = 0U; j < i; ++j) {
                const bool earlier_is_comparable =
                    sliced_available ? photo_catalog_is_sliced_name(source->entries[j].name)
                                     : photo_catalog_is_supported_name(source->entries[j].name);
                if (earlier_is_comparable &&
                    base_name_matches(source->entries[j].name, candidate->name)) {
                    seen_earlier = true;
                    break;
                }
            }
            if (seen_earlier) {
                continue;
            }

            if (catalog->count >= PHOTO_CATALOG_MAX_ENTRIES) {
                truncated = true;
                break;
            }

            photo_catalog_entry_t *entry = &catalog->entries[catalog->count];
            memcpy(entry->name, candidate->name, sizeof(entry->name));
            entry->name[sizeof(entry->name) - 1U] = '\0';
            entry->source = source->source;
            entry->sliced = photo_catalog_is_sliced_name(candidate->name);
            char posix_path[PHOTO_CATALOG_PATH_MAX];
            if (!photo_catalog_build_paths(source->posix_root, source->lv_root, candidate->name,
                                           posix_path, sizeof(posix_path),
                                           entry->lv_path, sizeof(entry->lv_path))) {
                memset(entry, 0, sizeof(*entry));
                continue;
            }
            catalog->count++;
        }

        if (truncated) {
            break;
        }
    }

    if (catalog->count == 0U) {
        return PHOTO_CATALOG_EMPTY;
    }
    return truncated ? PHOTO_CATALOG_TOO_MANY_ENTRIES : PHOTO_CATALOG_OK;
}

size_t photo_catalog_next_index(size_t index, size_t count)
{
    if (count == 0U) {
        return 0U;
    }
    return (index + 1U) % count;
}

size_t photo_catalog_prev_index(size_t index, size_t count)
{
    if (count == 0U) {
        return 0U;
    }
    return (index + count - 1U) % count;
}

uint16_t photo_catalog_scale_for_screen(uint16_t source_w, uint16_t source_h,
                                        uint16_t screen_w, uint16_t screen_h)
{
    if (source_w == 0U || source_h == 0U || screen_w == 0U || screen_h == 0U) {
        return 256U;
    }
    const uint32_t scale_x = ((uint32_t)screen_w * 256U) / source_w;
    const uint32_t scale_y = ((uint32_t)screen_h * 256U) / source_h;
    const uint32_t scale = scale_x < scale_y ? scale_x : scale_y;
    return (uint16_t)(scale < 256U ? scale : 256U);
}

uint32_t photo_catalog_decode_bytes(uint32_t width, uint32_t height, uint32_t bytes_per_pixel)
{
    const uint64_t pixels = (uint64_t)width * (uint64_t)height;
    const uint64_t bytes = pixels * (uint64_t)bytes_per_pixel;
    return bytes > UINT32_MAX ? UINT32_MAX : (uint32_t)bytes;
}

bool photo_catalog_within_decode_budget(const char *path, uint32_t width, uint32_t height)
{
    if (width == 0U || height == 0U) {
        return false;
    }
    const uint32_t bytes = photo_catalog_decode_bytes(width, height,
                                                      photo_catalog_bytes_per_pixel(path));
    return bytes <= PHOTO_CATALOG_DECODE_BUDGET_BYTES;
}

const char *photo_catalog_source_name(photo_source_kind_t source)
{
    return source == PHOTO_SOURCE_SD ? "sd" : "builtin";
}

const char *photo_catalog_error_name(photo_catalog_error_t error)
{
    switch (error) {
    case PHOTO_CATALOG_OK:
        return "ok";
    case PHOTO_CATALOG_INVALID_ARGUMENT:
        return "invalid-argument";
    case PHOTO_CATALOG_EMPTY:
        return "empty";
    case PHOTO_CATALOG_TOO_MANY_ENTRIES:
        return "too-many-entries";
    default:
        return "unknown";
    }
}
