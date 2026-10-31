#include "music_sd_catalog.h"

#include <ctype.h>
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

static void copy_text(char *dst, size_t capacity, const char *src)
{
    size_t length;
    if (dst == NULL || capacity == 0U) {
        return;
    }
    if (src == NULL) {
        dst[0] = '\0';
        return;
    }
    length = bounded_length(src, capacity - 1U);
    memcpy(dst, src, length);
    dst[length] = '\0';
}

static void copy_utf8(char *dst, size_t capacity, const char *src)
{
    size_t length;
    if (dst == NULL || capacity == 0U) {
        return;
    }
    if (src == NULL) {
        dst[0] = '\0';
        return;
    }
    length = bounded_length(src, capacity - 1U);
    while (length > 0U && ((unsigned char)src[length] & 0xC0U) == 0x80U) {
        --length;
    }
    memcpy(dst, src, length);
    dst[length] = '\0';
}

static bool root_is_safe(const char *root)
{
    size_t length = bounded_length(root, MUSIC_SD_CATALOG_ROOT_MAX);
    if (root == NULL || length == 0U || length >= MUSIC_SD_CATALOG_ROOT_MAX || root[0] != '/') {
        return false;
    }
    if (length > 1U && root[length - 1U] == '/') return false;
    for (size_t i = 0U; i < length; ++i) {
        unsigned char ch = (unsigned char)root[i];
        if (ch < 0x20U || ch == 0x7FU || ch == '\\' || ch == ':') return false;
        if (root[i] != '/') continue;
        if (i + 1U < length && root[i + 1U] == '/') return false;
        if (i + 1U < length && root[i + 1U] == '.' &&
            (i + 2U == length || root[i + 2U] == '/')) return false;
        if (i + 2U < length && root[i + 1U] == '.' && root[i + 2U] == '.' &&
            (i + 3U == length || root[i + 3U] == '/')) return false;
    }
    return true;
}

bool music_sd_catalog_name_is_safe(const char *name)
{
    size_t length = bounded_length(name, MUSIC_SD_CATALOG_NAME_MAX);
    if (name == NULL || length == 0U || length >= MUSIC_SD_CATALOG_NAME_MAX || name[0] == '.') {
        return false;
    }
    for (size_t i = 0U; i < length; ++i) {
        unsigned char ch = (unsigned char)name[i];
        if (ch < 0x20U || ch == 0x7FU || ch == '\\' || ch == '/' || ch == ':') {
            return false;
        }
    }
    return true;
}

static bool extension_is(const char *extension, const char *expected)
{
    size_t i = 0U;
    while (extension[i] != '\0' && expected[i] != '\0') {
        if (tolower((unsigned char)extension[i]) != tolower((unsigned char)expected[i])) {
            return false;
        }
        ++i;
    }
    return extension[i] == '\0' && expected[i] == '\0';
}

bool music_sd_catalog_is_supported_name(const char *name, music_sd_media_type_t *type)
{
    const char *dot;
    if (!music_sd_catalog_name_is_safe(name)) {
        return false;
    }
    dot = strrchr(name, '.');
    if (dot == NULL || dot == name || dot[1] == '\0') {
        return false;
    }
    if (extension_is(dot + 1, "mp3")) {
        if (type != NULL) *type = MUSIC_SD_MEDIA_MP3;
        return true;
    }
    if (extension_is(dot + 1, "wav")) {
        if (type != NULL) *type = MUSIC_SD_MEDIA_WAV;
        return true;
    }
    if (extension_is(dot + 1, "aac")) {
        if (type != NULL) *type = MUSIC_SD_MEDIA_AAC;
        return true;
    }
    return false;
}

static bool make_path(char *path, size_t capacity, const char *root, const char *name)
{
    size_t root_length = bounded_length(root, capacity);
    size_t name_length = bounded_length(name, capacity);
    bool root_is_slash = root_length == 1U && root[0] == '/';
    size_t separator = root_is_slash ? 0U : 1U;
    if (root_length >= capacity || name_length >= capacity ||
        root_length + separator + name_length + 1U > capacity) {
        return false;
    }
    memcpy(path, root, root_length);
    if (separator != 0U) path[root_length] = '/';
    memcpy(path + root_length + separator, name, name_length);
    path[root_length + separator + name_length] = '\0';
    return true;
}

static int folded_compare(const char *left, const char *right)
{
    size_t i = 0U;
    while (left[i] != '\0' && right[i] != '\0') {
        int a = tolower((unsigned char)left[i]);
        int b = tolower((unsigned char)right[i]);
        if (a != b) return a < b ? -1 : 1;
        ++i;
    }
    if (left[i] != '\0' || right[i] != '\0') return left[i] == '\0' ? -1 : 1;
    return strcmp(left, right);
}

static void sort_entries(music_sd_catalog_t *catalog)
{
    for (size_t i = 1U; i < catalog->count; ++i) {
        music_sd_catalog_entry_t value = catalog->entries[i];
        size_t j = i;
        while (j > 0U && folded_compare(value.name, catalog->entries[j - 1U].name) < 0) {
            catalog->entries[j] = catalog->entries[j - 1U];
            --j;
        }
        catalog->entries[j] = value;
    }
}

void music_sd_catalog_reset(music_sd_catalog_t *catalog)
{
    if (catalog != NULL) memset(catalog, 0, sizeof(*catalog));
}

static bool catalog_has_name(const music_sd_catalog_t *catalog, const char *name)
{
    for (size_t i = 0U; i < catalog->count; ++i) {
        if (folded_compare(catalog->entries[i].name, name) == 0) {
            return true;
        }
    }
    return false;
}

void music_sd_catalog_begin(music_sd_catalog_t *catalog)
{
    music_sd_catalog_reset(catalog);
}

music_sd_catalog_error_t music_sd_catalog_append(
    music_sd_catalog_t *catalog, const char *root,
    const music_sd_catalog_dir_entry_t *entries, size_t entry_count)
{
    bool appended = false;

    if (catalog == NULL || (entry_count != 0U && entries == NULL)) {
        return MUSIC_SD_CATALOG_INVALID_ARGUMENT;
    }
    if (!root_is_safe(root)) {
        return MUSIC_SD_CATALOG_PATH_UNSAFE;
    }
    for (size_t i = 0U; i < entry_count; ++i) {
        music_sd_media_type_t type;
        const music_sd_catalog_dir_entry_t *source = &entries[i];
        if (source->is_directory ||
            !music_sd_catalog_is_supported_name(source->name, &type) ||
            source->size_bytes > MUSIC_SD_CATALOG_MAX_FILE_SIZE) {
            continue;
        }
        if (catalog->count >= MUSIC_SD_CATALOG_MAX_ENTRIES) {
            return MUSIC_SD_CATALOG_TOO_MANY_ENTRIES;
        }
        /** A card copy shadows the built-in copy of the same name. */
        if (catalog_has_name(catalog, source->name)) {
            continue;
        }
        music_sd_catalog_entry_t *entry = &catalog->entries[catalog->count];
        if (!make_path(entry->path, sizeof(entry->path), root, source->name)) continue;
        copy_text(entry->name, sizeof(entry->name), source->name);
        copy_utf8(entry->title, sizeof(entry->title), source->title);
        if (entry->title[0] == '\0') {
            const char *dot = strrchr(source->name, '.');
            size_t title_length = dot == NULL ? bounded_length(source->name, sizeof(entry->title))
                                              : (size_t)(dot - source->name);
            if (title_length >= sizeof(entry->title)) title_length = sizeof(entry->title) - 1U;
            memcpy(entry->title, source->name, title_length);
            entry->title[title_length] = '\0';
        }
        copy_text(entry->artist, sizeof(entry->artist), source->artist);
        if (entry->artist[0] == '\0') copy_text(entry->artist, sizeof(entry->artist), MUSIC_SD_UNKNOWN_ARTIST_KEY);
        entry->type = type;
        entry->size_bytes = source->size_bytes;
        ++catalog->count;
        appended = true;
    }
    return appended ? MUSIC_SD_CATALOG_OK : MUSIC_SD_CATALOG_EMPTY;
}

music_sd_catalog_error_t music_sd_catalog_finish(music_sd_catalog_t *catalog)
{
    if (catalog == NULL) {
        return MUSIC_SD_CATALOG_INVALID_ARGUMENT;
    }
    sort_entries(catalog);
    return catalog->count == 0U ? MUSIC_SD_CATALOG_EMPTY : MUSIC_SD_CATALOG_OK;
}

music_sd_catalog_error_t music_sd_catalog_scan(
    const char *root, const music_sd_catalog_dir_entry_t *entries,
    size_t entry_count, music_sd_catalog_t *catalog)
{
    if (catalog == NULL || (entry_count != 0U && entries == NULL)) {
        return MUSIC_SD_CATALOG_INVALID_ARGUMENT;
    }
    if (!root_is_safe(root)) {
        return MUSIC_SD_CATALOG_PATH_UNSAFE;
    }
    music_sd_catalog_begin(catalog);
    const music_sd_catalog_error_t appended =
        music_sd_catalog_append(catalog, root, entries, entry_count);
    if (appended == MUSIC_SD_CATALOG_INVALID_ARGUMENT ||
        appended == MUSIC_SD_CATALOG_PATH_UNSAFE) {
        return appended;
    }
    (void)music_sd_catalog_finish(catalog);
    return appended == MUSIC_SD_CATALOG_TOO_MANY_ENTRIES
               ? MUSIC_SD_CATALOG_TOO_MANY_ENTRIES
               : (catalog->count == 0U ? MUSIC_SD_CATALOG_EMPTY : MUSIC_SD_CATALOG_OK);
}
