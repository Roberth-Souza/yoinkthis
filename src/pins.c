#include "pins.h"

#include <string.h>

static char *pins_path(void) {
    return g_build_filename(g_get_user_state_dir(), "yoinkthis", "pins", NULL);
}

GHashTable *pins_load(void) {
    GHashTable *pins =
        g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);

    char *path = pins_path();
    char *data = NULL;
    if (g_file_get_contents(path, &data, NULL, NULL)) {
        char **lines = g_strsplit(data, "\n", -1);
        for (char **line = lines; *line != NULL; line++) {
            char *id = g_strstrip(*line);
            if (*id != '\0')
                g_hash_table_add(pins, g_strdup(id));
        }
        g_strfreev(lines);
        g_free(data);
    }
    g_free(path);
    return pins;
}

gboolean pins_save(GHashTable *pins, GError **error) {
    GString *content = g_string_new(NULL);
    GHashTableIter iter;
    gpointer key;
    g_hash_table_iter_init(&iter, pins);
    while (g_hash_table_iter_next(&iter, &key, NULL))
        g_string_append_printf(content, "%s\n", (const char *)key);

    char *path = pins_path();
    char *dir = g_path_get_dirname(path);
    g_mkdir_with_parents(dir, 0755);
    g_free(dir);

    gboolean ok = g_file_set_contents(path, content->str,
                                      (gssize)content->len, error);
    g_free(path);
    g_string_free(content, TRUE);
    return ok;
}
