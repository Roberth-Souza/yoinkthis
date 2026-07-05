#ifndef YOINKTHIS_CLIPHIST_H
#define YOINKTHIS_CLIPHIST_H

#include <glib.h>

typedef enum {
    CLIP_TEXT,
    CLIP_IMAGE,
} ClipKind;

typedef struct {
    char *id;       /* numeric cliphist id (validated digits-only) */
    char *line;     /* raw "id\tpreview" bytes, required by `cliphist delete` */
    char *preview;  /* UTF-8-sanitized preview for display */
    char *img_info; /* "png 435x466 · 122 KiB" for images, NULL otherwise */
    ClipKind kind;
} ClipEntry;

/* Returns array of ClipEntry* (newest first) with clip_entry_free as free func,
 * or NULL on error. */
GPtrArray *cliphist_list(GError **error);

/* Raw entry content; caller owns the returned GBytes. */
GBytes *cliphist_decode(const char *id, GError **error);

/* decode → wl-copy stdin. wl-copy daemonizes itself to serve pastes. */
gboolean cliphist_copy_to_clipboard(const ClipEntry *entry, GError **error);

gboolean cliphist_delete(const ClipEntry *entry, GError **error);
gboolean cliphist_wipe(GError **error);

void clip_entry_free(gpointer data);

#endif
