#ifndef YOINKTHIS_THUMBS_H
#define YOINKTHIS_THUMBS_H

#include <gdk-pixbuf/gdk-pixbuf.h>
#include <gio/gio.h>

/* Loads (decode + scale) a thumbnail for a cliphist image entry in a worker
 * thread. Scaled results are cached on disk, so repeated opens are instant. */
void thumbs_load_async(const char *id, int max_width, int max_height,
                       GAsyncReadyCallback callback, gpointer user_data);

/* Returns a new GdkPixbuf reference, or NULL with error set. */
GdkPixbuf *thumbs_load_finish(GAsyncResult *result, GError **error);

/* Drops cached thumbnails whose cliphist id is absent from live_ids. */
void thumbs_prune_cache(GHashTable *live_ids);

#endif
