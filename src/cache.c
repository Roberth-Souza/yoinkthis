#include "cache.h"

#include <glib/gstdio.h>
#include <string.h>

char *cache_dir(const char *name) {
    return g_build_filename(g_get_user_cache_dir(), "yoinkthis", name, NULL);
}

char *cache_path(const char *name, const char *id, const char *ext) {
    char *dir = cache_dir(name);
    char *file = g_strdup_printf("%s.%s", id, ext);
    char *path = g_build_filename(dir, file, NULL);
    g_free(file);
    g_free(dir);
    return path;
}

gboolean cache_ensure_dir(const char *name) {
    char *dir = cache_dir(name);
    gboolean ok = g_mkdir_with_parents(dir, 0755) == 0;
    g_free(dir);
    return ok;
}

void cache_prune(const char *name, GHashTable *live_ids) {
    char *dir = cache_dir(name);
    GDir *handle = g_dir_open(dir, 0, NULL);
    if (handle == NULL) {
        g_free(dir);
        return;
    }

    const char *file;
    while ((file = g_dir_read_name(handle)) != NULL) {
        /* Cache names are "<id>.<ext>" and ids are digits, so the first dot
         * always ends the id. */
        const char *dot = strchr(file, '.');
        char *id = dot != NULL ? g_strndup(file, dot - file) : g_strdup(file);
        gboolean live = g_hash_table_contains(live_ids, id);
        g_free(id);
        if (live)
            continue;

        char *path = g_build_filename(dir, file, NULL);
        g_unlink(path);
        g_free(path);
    }

    g_dir_close(handle);
    g_free(dir);
}
