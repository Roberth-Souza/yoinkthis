#include "ui.h"

#include <string.h>

#include "cliphist.h"
#include "thumbs.h"

#define LIST_MAX_HEIGHT 400
#define THUMB_MAX_WIDTH 240
#define THUMB_MAX_HEIGHT 90
#define THUMB_PRELOAD_MARGIN 200.0

/* Rows built synchronously before first paint; the rest arrive in idle
 * batches so startup stays instant regardless of history size. */
#define INITIAL_ROWS 40
#define APPEND_BATCH 150

enum {
    FILTER_ALL,
    FILTER_TEXT,
    FILTER_IMAGE,
    FILTER_MODE_COUNT,
};

struct Ui {
    GtkWidget *prompt;
    GtkWidget *entry;
    GtkWidget *scroller;
    GtkWidget *listbox;
    GPtrArray *rows;    /* GtkListBoxRow*, widgets owned by the container */
    GPtrArray *pending; /* ClipEntry* not yet turned into rows */
    guint pending_pos;
    guint append_idle_id;
    int filter_mode;
    gboolean wipe_armed;
};

static ClipEntry *row_entry(GtkListBoxRow *row) {
    return g_object_get_data(G_OBJECT(row), "clip-entry");
}

static void update_prompt(Ui *ui) {
    const char *text = "yoink |";
    if (ui->wipe_armed)
        text = "wipe all? |";
    else if (ui->filter_mode == FILTER_TEXT)
        text = "yoink [txt] |";
    else if (ui->filter_mode == FILTER_IMAGE)
        text = "yoink [img] |";
    gtk_label_set_text(GTK_LABEL(ui->prompt), text);
}

/* --- thumbnails ---------------------------------------------------------- */

static void on_thumb_ready(GObject *source, GAsyncResult *result,
                           gpointer user_data) {
    (void)source;
    GtkWidget *image = user_data;
    GError *error = NULL;
    GdkPixbuf *pixbuf = thumbs_load_finish(result, &error);
    if (pixbuf != NULL) {
        gtk_image_set_from_pixbuf(GTK_IMAGE(image), pixbuf);
        g_object_unref(pixbuf);
    } else {
        g_warning("thumbnail load failed: %s", error->message);
        g_clear_error(&error);
    }
    g_object_unref(image);
}

/* Requests thumbnails only for image rows inside (or near) the viewport. */
static void queue_visible_thumbs(Ui *ui) {
    GtkAdjustment *adj =
        gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(ui->scroller));
    double page = gtk_adjustment_get_page_size(adj);
    if (page <= 1.0)
        return;
    double low = gtk_adjustment_get_value(adj) - THUMB_PRELOAD_MARGIN;
    double high = gtk_adjustment_get_value(adj) + page + THUMB_PRELOAD_MARGIN;

    for (guint i = 0; i < ui->rows->len; i++) {
        GtkListBoxRow *row = g_ptr_array_index(ui->rows, i);
        if (!gtk_widget_get_visible(GTK_WIDGET(row)))
            continue;
        ClipEntry *entry = row_entry(row);
        if (entry->kind != CLIP_IMAGE)
            continue;
        if (g_object_get_data(G_OBJECT(row), "thumb-requested") != NULL)
            continue;

        GtkAllocation alloc;
        gtk_widget_get_allocation(GTK_WIDGET(row), &alloc);
        if (alloc.y + alloc.height < low || alloc.y > high)
            continue;

        g_object_set_data(G_OBJECT(row), "thumb-requested",
                          GINT_TO_POINTER(1));
        GtkWidget *image = g_object_get_data(G_OBJECT(row), "thumb-image");
        thumbs_load_async(entry->id, THUMB_MAX_WIDTH, THUMB_MAX_HEIGHT,
                          on_thumb_ready, g_object_ref(image));
    }
}

/* --- filtering & selection ----------------------------------------------- */

static gboolean row_matches(Ui *ui, GtkListBoxRow *row,
                            const char *needle_folded) {
    ClipEntry *entry = row_entry(row);
    if (ui->filter_mode == FILTER_TEXT && entry->kind != CLIP_TEXT)
        return FALSE;
    if (ui->filter_mode == FILTER_IMAGE && entry->kind != CLIP_IMAGE)
        return FALSE;
    if (needle_folded == NULL || *needle_folded == '\0')
        return TRUE;

    char *haystack = g_utf8_casefold(entry->preview, -1);
    gboolean hit = strstr(haystack, needle_folded) != NULL;
    g_free(haystack);
    return hit;
}

static void scroll_to_row(Ui *ui, GtkListBoxRow *row) {
    GtkAdjustment *adj =
        gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(ui->scroller));
    GtkAllocation alloc;
    gtk_widget_get_allocation(GTK_WIDGET(row), &alloc);

    double value = gtk_adjustment_get_value(adj);
    double page = gtk_adjustment_get_page_size(adj);
    if (alloc.y < value)
        gtk_adjustment_set_value(adj, alloc.y);
    else if (alloc.y + alloc.height > value + page)
        gtk_adjustment_set_value(adj, alloc.y + alloc.height - page);
}

static void refilter(Ui *ui) {
    const char *query = gtk_entry_get_text(GTK_ENTRY(ui->entry));
    char *needle = g_utf8_casefold(query, -1);

    GtkListBoxRow *first = NULL;
    for (guint i = 0; i < ui->rows->len; i++) {
        GtkListBoxRow *row = g_ptr_array_index(ui->rows, i);
        gboolean visible = row_matches(ui, row, needle);
        gtk_widget_set_visible(GTK_WIDGET(row), visible);
        if (visible && first == NULL)
            first = row;
    }
    g_free(needle);

    GtkAdjustment *adj =
        gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(ui->scroller));
    gtk_adjustment_set_value(adj, 0);
    gtk_list_box_select_row(GTK_LIST_BOX(ui->listbox), first);
}

/* Visible rows in list order; caller frees the array (not the rows). */
static GPtrArray *visible_rows(Ui *ui) {
    GPtrArray *rows = g_ptr_array_new();
    for (guint i = 0; i < ui->rows->len; i++) {
        GtkListBoxRow *row = g_ptr_array_index(ui->rows, i);
        if (gtk_widget_get_visible(GTK_WIDGET(row)))
            g_ptr_array_add(rows, row);
    }
    return rows;
}

void ui_move_selection(Ui *ui, int delta) {
    GPtrArray *rows = visible_rows(ui);
    if (rows->len == 0) {
        g_ptr_array_free(rows, TRUE);
        return;
    }

    GtkListBoxRow *selected =
        gtk_list_box_get_selected_row(GTK_LIST_BOX(ui->listbox));
    int current = -1;
    for (guint i = 0; i < rows->len; i++) {
        if (g_ptr_array_index(rows, i) == selected) {
            current = (int)i;
            break;
        }
    }

    int target = current < 0 ? 0 : current + delta;
    target = CLAMP(target, 0, (int)rows->len - 1);
    GtkListBoxRow *row = g_ptr_array_index(rows, (guint)target);
    gtk_list_box_select_row(GTK_LIST_BOX(ui->listbox), row);
    scroll_to_row(ui, row);
    g_ptr_array_free(rows, TRUE);
}

/* --- actions -------------------------------------------------------------- */

/* Stops the incremental append and frees entries that never became rows. */
static void cancel_pending(Ui *ui) {
    if (ui->append_idle_id != 0) {
        g_source_remove(ui->append_idle_id);
        ui->append_idle_id = 0;
    }
    if (ui->pending != NULL) {
        for (guint i = ui->pending_pos; i < ui->pending->len; i++)
            clip_entry_free(g_ptr_array_index(ui->pending, i));
        g_ptr_array_unref(ui->pending);
        ui->pending = NULL;
    }
}

void ui_activate_selected(Ui *ui) {
    GtkListBoxRow *row =
        gtk_list_box_get_selected_row(GTK_LIST_BOX(ui->listbox));
    if (row == NULL)
        return;

    GError *error = NULL;
    if (!cliphist_copy_to_clipboard(row_entry(row), &error)) {
        g_warning("copy failed: %s", error->message);
        g_clear_error(&error);
    }
    gtk_main_quit();
}

void ui_delete_selected(Ui *ui) {
    GtkListBoxRow *row =
        gtk_list_box_get_selected_row(GTK_LIST_BOX(ui->listbox));
    if (row == NULL)
        return;

    GError *error = NULL;
    if (!cliphist_delete(row_entry(row), &error)) {
        g_warning("delete failed: %s", error->message);
        g_clear_error(&error);
        return;
    }

    GPtrArray *rows = visible_rows(ui);
    GtkListBoxRow *next = NULL;
    for (guint i = 0; i < rows->len; i++) {
        if (g_ptr_array_index(rows, i) == row) {
            if (i + 1 < rows->len)
                next = g_ptr_array_index(rows, i + 1);
            else if (i > 0)
                next = g_ptr_array_index(rows, i - 1);
            break;
        }
    }
    g_ptr_array_free(rows, TRUE);

    g_ptr_array_remove(ui->rows, row);
    gtk_widget_destroy(GTK_WIDGET(row));
    if (next != NULL) {
        gtk_list_box_select_row(GTK_LIST_BOX(ui->listbox), next);
        scroll_to_row(ui, next);
    }
}

void ui_request_wipe(Ui *ui) {
    if (!ui->wipe_armed) {
        ui->wipe_armed = TRUE;
        update_prompt(ui);
        return;
    }

    ui->wipe_armed = FALSE;
    GError *error = NULL;
    if (!cliphist_wipe(&error)) {
        g_warning("wipe failed: %s", error->message);
        g_clear_error(&error);
    } else {
        thumbs_clear_cache();
        cancel_pending(ui);
        for (guint i = 0; i < ui->rows->len; i++)
            gtk_widget_destroy(GTK_WIDGET(g_ptr_array_index(ui->rows, i)));
        g_ptr_array_set_size(ui->rows, 0);
    }
    update_prompt(ui);
}

void ui_disarm_wipe(Ui *ui) {
    if (ui->wipe_armed) {
        ui->wipe_armed = FALSE;
        update_prompt(ui);
    }
}

gboolean ui_wipe_armed(Ui *ui) {
    return ui->wipe_armed;
}

void ui_cycle_filter(Ui *ui) {
    ui->filter_mode = (ui->filter_mode + 1) % FILTER_MODE_COUNT;
    update_prompt(ui);
    refilter(ui);
}

void ui_focus_entry(Ui *ui) {
    gtk_widget_grab_focus(ui->entry);
}

/* --- construction --------------------------------------------------------- */

static GtkWidget *make_row(ClipEntry *entry) {
    GtkWidget *row = gtk_list_box_row_new();
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);

    if (entry->kind == CLIP_IMAGE) {
        GtkWidget *image = gtk_image_new();
        gtk_widget_set_size_request(image, -1, THUMB_MAX_HEIGHT);
        gtk_widget_set_halign(image, GTK_ALIGN_START);
        gtk_box_pack_start(GTK_BOX(box), image, FALSE, FALSE, 0);

        GtkWidget *info = gtk_label_new(entry->img_info);
        gtk_widget_set_name(info, "img-info");
        gtk_widget_set_valign(info, GTK_ALIGN_CENTER);
        gtk_label_set_xalign(GTK_LABEL(info), 0);
        gtk_box_pack_start(GTK_BOX(box), info, TRUE, TRUE, 0);

        g_object_set_data(G_OBJECT(row), "thumb-image", image);
    } else {
        GtkWidget *label = gtk_label_new(entry->preview);
        gtk_label_set_ellipsize(GTK_LABEL(label), PANGO_ELLIPSIZE_END);
        gtk_label_set_xalign(GTK_LABEL(label), 0);
        gtk_box_pack_start(GTK_BOX(box), label, TRUE, TRUE, 0);
    }

    gtk_container_add(GTK_CONTAINER(row), box);
    g_object_set_data_full(G_OBJECT(row), "clip-entry", entry,
                           clip_entry_free);
    return row;
}

static gboolean append_batch(gpointer data) {
    Ui *ui = data;
    const char *query = gtk_entry_get_text(GTK_ENTRY(ui->entry));
    char *needle = g_utf8_casefold(query, -1);

    guint limit = MIN(ui->pending->len, ui->pending_pos + APPEND_BATCH);
    for (; ui->pending_pos < limit; ui->pending_pos++) {
        ClipEntry *entry = g_ptr_array_index(ui->pending, ui->pending_pos);
        GtkWidget *row = make_row(entry); /* row now owns the entry */
        g_ptr_array_add(ui->rows, row);
        gtk_container_add(GTK_CONTAINER(ui->listbox), row);
        gtk_widget_show_all(row);
        gtk_widget_set_visible(
            row, row_matches(ui, GTK_LIST_BOX_ROW(row), needle));
    }
    g_free(needle);

    /* An active query may have had zero matches until this batch. */
    if (gtk_list_box_get_selected_row(GTK_LIST_BOX(ui->listbox)) == NULL) {
        for (guint i = 0; i < ui->rows->len; i++) {
            GtkListBoxRow *row = g_ptr_array_index(ui->rows, i);
            if (gtk_widget_get_visible(GTK_WIDGET(row))) {
                gtk_list_box_select_row(GTK_LIST_BOX(ui->listbox), row);
                break;
            }
        }
    }

    if (ui->pending_pos >= ui->pending->len) {
        g_ptr_array_unref(ui->pending);
        ui->pending = NULL;
        ui->append_idle_id = 0;
        return G_SOURCE_REMOVE;
    }
    return G_SOURCE_CONTINUE;
}

static void on_entry_changed(GtkEditable *editable, gpointer user_data) {
    (void)editable;
    Ui *ui = user_data;
    ui_disarm_wipe(ui);
    refilter(ui);
}

static void on_row_activated(GtkListBox *listbox, GtkListBoxRow *row,
                             gpointer user_data) {
    (void)listbox;
    Ui *ui = user_data;
    gtk_list_box_select_row(GTK_LIST_BOX(ui->listbox), row);
    ui_activate_selected(ui);
}

static void on_list_allocated(GtkWidget *widget, GdkRectangle *allocation,
                              gpointer user_data) {
    (void)widget;
    (void)allocation;
    queue_visible_thumbs(user_data);
}

static void on_scrolled(GtkAdjustment *adjustment, gpointer user_data) {
    (void)adjustment;
    queue_visible_thumbs(user_data);
}

Ui *ui_build(GtkWindow *window) {
    GError *error = NULL;
    GPtrArray *entries = cliphist_list(&error);
    if (entries == NULL) {
        g_printerr("yoinkthis: failed to read cliphist: %s\n", error->message);
        g_clear_error(&error);
        return NULL;
    }

    Ui *ui = g_new0(Ui, 1);
    ui->rows = g_ptr_array_new();
    ui->filter_mode = FILTER_ALL;

    GtkWidget *root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_name(root, "root");

    GtkWidget *inputbar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_set_name(inputbar, "inputbar");

    ui->prompt = gtk_label_new(NULL);
    gtk_widget_set_name(ui->prompt, "prompt");
    gtk_box_pack_start(GTK_BOX(inputbar), ui->prompt, FALSE, FALSE, 0);

    ui->entry = gtk_entry_new();
    gtk_widget_set_name(ui->entry, "entry");
    gtk_entry_set_has_frame(GTK_ENTRY(ui->entry), FALSE);
    gtk_box_pack_start(GTK_BOX(inputbar), ui->entry, TRUE, TRUE, 0);

    gtk_box_pack_start(GTK_BOX(root), inputbar, FALSE, FALSE, 0);

    ui->scroller = gtk_scrolled_window_new(NULL, NULL);
    gtk_widget_set_name(ui->scroller, "scroller");
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(ui->scroller),
                                   GTK_POLICY_NEVER, GTK_POLICY_EXTERNAL);
    gtk_scrolled_window_set_propagate_natural_height(
        GTK_SCROLLED_WINDOW(ui->scroller), TRUE);
    gtk_scrolled_window_set_max_content_height(
        GTK_SCROLLED_WINDOW(ui->scroller), LIST_MAX_HEIGHT);

    ui->listbox = gtk_list_box_new();
    gtk_widget_set_name(ui->listbox, "list");
    gtk_list_box_set_selection_mode(GTK_LIST_BOX(ui->listbox),
                                    GTK_SELECTION_BROWSE);
    gtk_list_box_set_activate_on_single_click(GTK_LIST_BOX(ui->listbox), TRUE);

    guint initial = MIN(entries->len, INITIAL_ROWS);
    for (guint i = 0; i < initial; i++) {
        ClipEntry *entry = g_ptr_array_index(entries, i);
        GtkWidget *row = make_row(entry); /* row now owns the entry */
        g_ptr_array_add(ui->rows, row);
        gtk_container_add(GTK_CONTAINER(ui->listbox), row);
    }
    g_ptr_array_set_free_func(entries, NULL);
    if (entries->len > initial) {
        ui->pending = entries;
        ui->pending_pos = initial;
        ui->append_idle_id = g_idle_add(append_batch, ui);
    } else {
        g_ptr_array_unref(entries);
    }

    gtk_container_add(GTK_CONTAINER(ui->scroller), ui->listbox);
    gtk_box_pack_start(GTK_BOX(root), ui->scroller, TRUE, TRUE, 0);
    gtk_container_add(GTK_CONTAINER(window), root);

    update_prompt(ui);
    refilter(ui);

    g_signal_connect(ui->entry, "changed", G_CALLBACK(on_entry_changed), ui);
    g_signal_connect(ui->listbox, "row-activated",
                     G_CALLBACK(on_row_activated), ui);
    g_signal_connect(ui->listbox, "size-allocate",
                     G_CALLBACK(on_list_allocated), ui);
    g_signal_connect(
        gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(ui->scroller)),
        "value-changed", G_CALLBACK(on_scrolled), ui);

    return ui;
}

void ui_free(Ui *ui) {
    if (ui == NULL)
        return;
    cancel_pending(ui);
    g_ptr_array_free(ui->rows, TRUE);
    g_free(ui);
}
