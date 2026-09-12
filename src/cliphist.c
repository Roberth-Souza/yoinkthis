#include "cliphist.h"

#include <gio/gio.h>
#include <stdio.h>
#include <string.h>

/* Run argv, optionally feeding stdin and capturing stdout. Fails on non-zero exit. */
static gboolean run_command(const char *const argv[], GBytes *input,
                            GBytes **output, GError **error) {
    GSubprocessFlags flags = G_SUBPROCESS_FLAGS_NONE;
    if (input != NULL)
        flags |= G_SUBPROCESS_FLAGS_STDIN_PIPE;
    if (output != NULL)
        flags |= G_SUBPROCESS_FLAGS_STDOUT_PIPE;

    GSubprocess *proc =
        g_subprocess_newv((const gchar *const *)argv, flags, error);
    if (proc == NULL)
        return FALSE;

    gboolean ok = g_subprocess_communicate(proc, input, NULL, output, NULL, error) &&
                  g_subprocess_wait_check(proc, NULL, error);
    if (!ok && output != NULL && *output != NULL) {
        g_bytes_unref(*output);
        *output = NULL;
    }
    g_object_unref(proc);
    return ok;
}

static gboolean id_is_valid(const char *id) {
    if (*id == '\0')
        return FALSE;
    return strspn(id, "0123456789") == strlen(id);
}

/* cliphist previews binary entries as "[[ binary data 122 KiB png 435x466 ]]".
 * Returns TRUE (filling img_info) only for thumbnailable image formats. */
static gboolean parse_image_preview(const char *preview, char **img_info) {
    char size[32], unit[16], fmt[16];
    unsigned width, height;
    if (sscanf(preview, "[[ binary data %31s %15s %15s %ux%u ]]",
               size, unit, fmt, &width, &height) != 5)
        return FALSE;

    static const char *const image_formats[] = {"png", "jpg", "jpeg",
                                                "webp", "bmp", "gif"};
    for (gsize i = 0; i < G_N_ELEMENTS(image_formats); i++) {
        if (g_ascii_strcasecmp(fmt, image_formats[i]) == 0) {
            *img_info = g_strdup_printf("%s %ux%u · %s %s",
                                        fmt, width, height, size, unit);
            return TRUE;
        }
    }
    return FALSE;
}

static ClipEntry *parse_line(const char *line, gsize len) {
    const char *tab = memchr(line, '\t', len);
    if (tab == NULL || tab == line)
        return NULL;

    ClipEntry *entry = g_new0(ClipEntry, 1);
    entry->id = g_strndup(line, (gsize)(tab - line));
    if (!id_is_valid(entry->id)) {
        clip_entry_free(entry);
        return NULL;
    }
    entry->line = g_strndup(line, len);

    char *raw_preview = g_strndup(tab + 1, len - (gsize)(tab - line) - 1);
    entry->preview = g_utf8_make_valid(raw_preview, -1);
    g_free(raw_preview);

    entry->kind = parse_image_preview(entry->preview, &entry->img_info)
                      ? CLIP_IMAGE
                      : CLIP_TEXT;
    return entry;
}

GPtrArray *cliphist_list(GError **error) {
    const char *argv[] = {"cliphist", "list", NULL};
    GBytes *out = NULL;
    if (!run_command(argv, NULL, &out, error))
        return NULL;

    GPtrArray *entries = g_ptr_array_new_with_free_func(clip_entry_free);
    gsize len = 0;
    const char *data = g_bytes_get_data(out, &len);
    const char *p = data;
    const char *end = data + len;

    while (p < end) {
        const char *nl = memchr(p, '\n', (gsize)(end - p));
        gsize line_len = nl != NULL ? (gsize)(nl - p) : (gsize)(end - p);
        if (line_len > 0) {
            ClipEntry *entry = parse_line(p, line_len);
            if (entry != NULL)
                g_ptr_array_add(entries, entry);
        }
        p = nl != NULL ? nl + 1 : end;
    }

    g_bytes_unref(out);
    return entries;
}

GBytes *cliphist_decode(const char *id, GError **error) {
    const char *argv[] = {"cliphist", "decode", id, NULL};
    GBytes *out = NULL;
    if (!run_command(argv, NULL, &out, error))
        return NULL;
    return out;
}

gboolean cliphist_copy_to_clipboard(const ClipEntry *entry, GError **error) {
    GBytes *content = cliphist_decode(entry->id, error);
    if (content == NULL)
        return FALSE;

    const char *argv[] = {"wl-copy", NULL};
    gboolean ok = run_command(argv, content, NULL, error);
    g_bytes_unref(content);
    return ok;
}

gboolean cliphist_delete(const ClipEntry *entry, GError **error) {
    char *payload = g_strconcat(entry->line, "\n", NULL);
    GBytes *input = g_bytes_new_take(payload, strlen(payload));

    const char *argv[] = {"cliphist", "delete", NULL};
    gboolean ok = run_command(argv, input, NULL, error);
    g_bytes_unref(input);
    return ok;
}

gboolean cliphist_delete_entries(GPtrArray *entries, GError **error) {
    if (entries->len == 0)
        return TRUE;

    GString *payload = g_string_new(NULL);
    for (guint i = 0; i < entries->len; i++) {
        const ClipEntry *entry = g_ptr_array_index(entries, i);
        g_string_append(payload, entry->line);
        g_string_append_c(payload, '\n');
    }
    gsize len = payload->len;
    GBytes *input = g_bytes_new_take(g_string_free(payload, FALSE), len);

    const char *argv[] = {"cliphist", "delete", NULL};
    gboolean ok = run_command(argv, input, NULL, error);
    g_bytes_unref(input);
    return ok;
}

gboolean cliphist_wipe(GError **error) {
    const char *argv[] = {"cliphist", "wipe", NULL};
    return run_command(argv, NULL, NULL, error);
}

void clip_entry_free(gpointer data) {
    ClipEntry *entry = data;
    if (entry == NULL)
        return;
    g_free(entry->id);
    g_free(entry->line);
    g_free(entry->preview);
    g_free(entry->img_info);
    g_free(entry);
}
