#include "imgfile.h"

#include <glib/gstdio.h>
#include <string.h>
#include <sys/stat.h>

#include "cache.h"

#define CACHE_NAME "full"

/* Screenshot tools drop their output a level or two under the pictures
 * directory, so a shallow walk finds them without wandering off into
 * unrelated trees. */
#define MAX_SEARCH_DEPTH 6

/* Format word of "png 435x466 · 122 KiB", used as the cache extension. */
static char *entry_extension(const ClipEntry *entry) {
    const char *end =
        entry->img_info != NULL ? strchr(entry->img_info, ' ') : NULL;
    if (end == NULL)
        return g_strdup("png");
    return g_ascii_strdown(entry->img_info, end - entry->img_info);
}

/* Environment variables are not expanded by a shell when read back, so a
 * leading "~/" would otherwise be taken literally. */
static char *expand_home(const char *dir) {
    if (dir[0] != '~' || (dir[1] != '/' && dir[1] != '\0'))
        return g_strdup(dir);
    return g_build_filename(g_get_home_dir(), dir + 1, NULL);
}

/* Directories to search, NULL when there is nowhere sensible to look. */
static char **search_dirs(void) {
    const char *configured = g_getenv("YOINKTHIS_IMAGE_DIRS");
    if (configured != NULL && *configured != '\0') {
        char **dirs = g_strsplit(configured, G_SEARCHPATH_SEPARATOR_S, -1);
        for (char **dir = dirs; *dir != NULL; dir++) {
            char *expanded = expand_home(*dir);
            g_free(*dir);
            *dir = expanded;
        }
        return dirs;
    }

    const char *pictures = g_get_user_special_dir(G_USER_DIRECTORY_PICTURES);
    if (pictures == NULL)
        return NULL;

    char **dirs = g_new0(char *, 2);
    dirs[0] = g_strdup(pictures);
    return dirs;
}

/* Depth-first hunt for a regular file holding exactly these bytes. Size is
 * compared first and rejects almost every candidate without opening it, which
 * is what keeps the walk cheap on a large collection. */
static char *find_match(const char *dir, const char *want, gsize want_len,
                        int depth) {
    GDir *handle = g_dir_open(dir, 0, NULL);
    if (handle == NULL)
        return NULL;

    char *found = NULL;
    const char *name;
    while (found == NULL && (name = g_dir_read_name(handle)) != NULL) {
        if (name[0] == '.') /* hidden entries, never a screenshot target */
            continue;

        char *path = g_build_filename(dir, name, NULL);
        GStatBuf st;
        if (g_stat(path, &st) != 0) {
            g_free(path);
            continue;
        }

        if (S_ISDIR(st.st_mode)) {
            /* Bounded depth also breaks any symlink cycle. */
            if (depth < MAX_SEARCH_DEPTH)
                found = find_match(path, want, want_len, depth + 1);
        } else if (S_ISREG(st.st_mode) && (gsize)st.st_size == want_len) {
            char *data = NULL;
            gsize len = 0;
            if (g_file_get_contents(path, &data, &len, NULL) &&
                len == want_len && memcmp(data, want, want_len) == 0)
                found = g_strdup(path);
            g_free(data);
        }
        g_free(path);
    }

    g_dir_close(handle);
    return found;
}

/* Writes the entry to the cache so there is something to open when the
 * original is gone or never existed. */
static char *cache_entry(const ClipEntry *entry, const char *data, gsize len,
                         GError **error) {
    char *ext = entry_extension(entry);
    char *path = cache_path(CACHE_NAME, entry->id, ext);
    g_free(ext);

    if (g_file_test(path, G_FILE_TEST_IS_REGULAR))
        return path;

    if (!cache_ensure_dir(CACHE_NAME)) {
        g_set_error(error, G_FILE_ERROR, G_FILE_ERROR_FAILED,
                    "could not create the %s cache directory", CACHE_NAME);
        g_free(path);
        return NULL;
    }

    if (!g_file_set_contents(path, data, len, error)) {
        g_free(path);
        return NULL;
    }
    return path;
}

char *imgfile_resolve(const ClipEntry *entry, GError **error) {
    if (entry->kind != CLIP_IMAGE) {
        g_set_error(error, G_FILE_ERROR, G_FILE_ERROR_INVAL,
                    "entry %s is not an image", entry->id);
        return NULL;
    }

    GBytes *bytes = cliphist_decode(entry->id, error);
    if (bytes == NULL)
        return NULL;

    gsize len = 0;
    const char *data = g_bytes_get_data(bytes, &len);

    char *path = NULL;
    char **dirs = search_dirs();
    if (dirs != NULL) {
        for (char **dir = dirs; path == NULL && *dir != NULL; dir++) {
            if (**dir != '\0')
                path = find_match(*dir, data, len, 0);
        }
        g_strfreev(dirs);
    }

    if (path == NULL)
        path = cache_entry(entry, data, len, error);

    g_bytes_unref(bytes);
    return path;
}

void imgfile_prune_cache(GHashTable *live_ids) {
    cache_prune(CACHE_NAME, live_ids);
}
