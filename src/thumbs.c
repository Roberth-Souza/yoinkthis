#include "thumbs.h"

#include "cache.h"
#include "cliphist.h"

#define CACHE_NAME "thumbs"

typedef struct {
    char *id;
    int max_width;
    int max_height;
} ThumbRequest;

static void thumb_request_free(gpointer data) {
    ThumbRequest *req = data;
    g_free(req->id);
    g_free(req);
}

/* Returns a new reference, downscaled only when the source exceeds bounds. */
static GdkPixbuf *scale_to_fit(GdkPixbuf *src, int max_width, int max_height) {
    int width = gdk_pixbuf_get_width(src);
    int height = gdk_pixbuf_get_height(src);
    if (width <= max_width && height <= max_height)
        return g_object_ref(src);

    double scale = MIN((double)max_width / width, (double)max_height / height);
    int new_width = MAX(1, (int)(width * scale));
    int new_height = MAX(1, (int)(height * scale));
    return gdk_pixbuf_scale_simple(src, new_width, new_height,
                                   GDK_INTERP_BILINEAR);
}

static GdkPixbuf *decode_and_scale(ThumbRequest *req, GError **error) {
    GBytes *bytes = cliphist_decode(req->id, error);
    if (bytes == NULL)
        return NULL;

    GdkPixbufLoader *loader = gdk_pixbuf_loader_new();
    gsize size = 0;
    const guchar *data = g_bytes_get_data(bytes, &size);
    gboolean ok = gdk_pixbuf_loader_write(loader, data, size, error);
    if (ok)
        ok = gdk_pixbuf_loader_close(loader, error);
    else
        gdk_pixbuf_loader_close(loader, NULL);
    g_bytes_unref(bytes);

    GdkPixbuf *full = ok ? gdk_pixbuf_loader_get_pixbuf(loader) : NULL;
    if (full == NULL) {
        if (error != NULL && *error == NULL)
            g_set_error(error, GDK_PIXBUF_ERROR,
                        GDK_PIXBUF_ERROR_CORRUPT_IMAGE,
                        "no pixbuf produced for entry %s", req->id);
        g_object_unref(loader);
        return NULL;
    }

    GdkPixbuf *thumb = scale_to_fit(full, req->max_width, req->max_height);
    g_object_unref(loader);
    return thumb;
}

static void thumb_task(GTask *task, gpointer source, gpointer task_data,
                       GCancellable *cancellable) {
    (void)source;
    (void)cancellable;
    ThumbRequest *req = task_data;
    char *path = cache_path(CACHE_NAME, req->id, "png");

    GdkPixbuf *thumb = gdk_pixbuf_new_from_file(path, NULL);
    if (thumb == NULL) {
        GError *error = NULL;
        thumb = decode_and_scale(req, &error);
        if (thumb == NULL) {
            g_free(path);
            g_task_return_error(task, error);
            return;
        }
        if (cache_ensure_dir(CACHE_NAME))
            gdk_pixbuf_save(thumb, path, "png", NULL, NULL);
    }

    g_free(path);
    g_task_return_pointer(task, thumb, g_object_unref);
}

void thumbs_load_async(const char *id, int max_width, int max_height,
                       GAsyncReadyCallback callback, gpointer user_data) {
    ThumbRequest *req = g_new0(ThumbRequest, 1);
    req->id = g_strdup(id);
    req->max_width = max_width;
    req->max_height = max_height;

    GTask *task = g_task_new(NULL, NULL, callback, user_data);
    g_task_set_task_data(task, req, thumb_request_free);
    g_task_run_in_thread(task, thumb_task);
    g_object_unref(task);
}

GdkPixbuf *thumbs_load_finish(GAsyncResult *result, GError **error) {
    return g_task_propagate_pointer(G_TASK(result), error);
}

void thumbs_prune_cache(GHashTable *live_ids) {
    cache_prune(CACHE_NAME, live_ids);
}
