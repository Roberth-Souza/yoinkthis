#include "ui.h"

#include <string.h>

#include "cliphist.h"
#include "pins.h"
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
    GHashTable *pins;            /* pinned id set, mirrored on disk */
    GtkListBoxRow *selected_row; /* weak-pointer tracked */
};

static ClipEntry *row_entry(GtkListBoxRow *row) {
    return g_object_get_data(G_OBJECT(row), "clip-entry");
}

/* --- pin icon -------------------------------------------------------------- */

/* Stroke-only pushpin, needle to the bottom-left; recolored only via CSS
 * opacity so the white outline stays visible on the dark theme. */
static const char PIN_SVG[] =
    "<svg xmlns='http://www.w3.org/2000/svg' width='24' height='24'"
    " viewBox='-2 -2 28 28' fill='none' stroke='#ffffff' stroke-width='2'"
    " stroke-linecap='round' stroke-linejoin='round'>"
    "<g transform='rotate(45 12 12)'>"
    "<path d='M12 17v5'/>"
    "<path d='M9 10.76a2 2 0 0 1-1.11 1.79l-1.78.9A2 2 0 0 0 5 15.24V16"
    "a1 1 0 0 0 1 1h12a1 1 0 0 0 1-1v-.76a2 2 0 0 0-1.11-1.79l-1.78-.9"
    "A2 2 0 0 1 15 10.76V7a1 1 0 0 1 1-1 2 2 0 0 0 0-4H8a2 2 0 0 0 0 4"
    "a1 1 0 0 1 1 1z'/>"
    "</g></svg>";

#define PIN_ICON_SIZE 16

static GdkPixbuf *pin_pixbuf(void) {
    static GdkPixbuf *pixbuf = NULL;
    static gboolean tried = FALSE;
    if (!tried) {
        tried = TRUE;
        GInputStream *stream = g_memory_input_stream_new_from_data(
            PIN_SVG, sizeof(PIN_SVG) - 1, NULL);
        GError *error = NULL;
        pixbuf = gdk_pixbuf_new_from_stream_at_scale(
            stream, PIN_ICON_SIZE, PIN_ICON_SIZE, TRUE, NULL, &error);
        if (pixbuf == NULL) {
            g_warning("pin icon SVG failed to load: %s", error->message);
            g_clear_error(&error);
        }
        g_object_unref(stream);
    }
    return pixbuf;
}

/* Pinned rows always show a solid pin; unpinned rows only show a translucent
 * one while selected (CSS handles the opacity difference). */
static void update_pin_button(GtkListBoxRow *row, gboolean selected) {
    GtkWidget *button = g_object_get_data(G_OBJECT(row), "pin-button");
    if (button == NULL)
        return;
    ClipEntry *entry = row_entry(row);
    GtkStyleContext *context = gtk_widget_get_style_context(button);
    if (entry->pinned)
        gtk_style_context_add_class(context, "pinned");
    else
        gtk_style_context_remove_class(context, "pinned");
    gtk_widget_set_visible(button, entry->pinned || selected);
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
    if (row == NULL || row_entry(row)->pinned)
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

/* Batch-deletes everything except pinned entries, including entries still
 * pending realization (those exist in cliphist too and are never pinned). */
static void wipe_except_pinned(Ui *ui) {
    GPtrArray *doomed = g_ptr_array_new();
    for (guint i = 0; i < ui->rows->len; i++) {
        ClipEntry *entry = row_entry(g_ptr_array_index(ui->rows, i));
        if (!entry->pinned)
            g_ptr_array_add(doomed, entry);
    }
    if (ui->pending != NULL) {
        for (guint i = ui->pending_pos; i < ui->pending->len; i++)
            g_ptr_array_add(doomed, g_ptr_array_index(ui->pending, i));
    }

    GError *error = NULL;
    gboolean ok = cliphist_delete_entries(doomed, &error);
    g_ptr_array_free(doomed, TRUE);
    if (!ok) {
        g_warning("wipe failed: %s", error->message);
        g_clear_error(&error);
        return;
    }

    cancel_pending(ui);
    for (guint i = ui->rows->len; i > 0; i--) {
        GtkListBoxRow *row = g_ptr_array_index(ui->rows, i - 1);
        if (row_entry(row)->pinned)
            continue;
        g_ptr_array_remove_index(ui->rows, i - 1);
        gtk_widget_destroy(GTK_WIDGET(row));
    }

    GPtrArray *rows = visible_rows(ui);
    if (rows->len > 0) {
        GtkListBoxRow *first = g_ptr_array_index(rows, 0);
        gtk_list_box_select_row(GTK_LIST_BOX(ui->listbox), first);
        scroll_to_row(ui, first);
    }
    g_ptr_array_free(rows, TRUE);
}

void ui_request_wipe(Ui *ui) {
    if (!ui->wipe_armed) {
        ui->wipe_armed = TRUE;
        update_prompt(ui);
        return;
    }

    ui->wipe_armed = FALSE;
    if (g_hash_table_size(ui->pins) > 0) {
        /* Keep the thumb cache: pinned image entries still need it. */
        wipe_except_pinned(ui);
        update_prompt(ui);
        return;
    }

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

/* --- pinning --------------------------------------------------------------- */

static void track_selected_row(Ui *ui, GtkListBoxRow *row) {
    if (ui->selected_row != NULL)
        g_object_remove_weak_pointer(G_OBJECT(ui->selected_row),
                                     (gpointer *)&ui->selected_row);
    ui->selected_row = row;
    if (row != NULL)
        g_object_add_weak_pointer(G_OBJECT(row),
                                  (gpointer *)&ui->selected_row);
}

static void on_row_selected(GtkListBox *listbox, GtkListBoxRow *row,
                            gpointer user_data) {
    (void)listbox;
    Ui *ui = user_data;
    if (ui->selected_row != NULL && ui->selected_row != row &&
        !gtk_widget_in_destruction(GTK_WIDGET(ui->selected_row)))
        update_pin_button(ui->selected_row, FALSE);
    track_selected_row(ui, row);
    if (row != NULL)
        update_pin_button(row, TRUE);
}

/* Index a row belongs at: pinned rows group at the top in pin order,
 * unpinned rows follow in their original newest-first (seq) order. */
static guint pin_target_index(Ui *ui, GtkListBoxRow *row) {
    ClipEntry *entry = row_entry(row);
    guint index = 0;
    for (guint i = 0; i < ui->rows->len; i++) {
        GtkListBoxRow *other = g_ptr_array_index(ui->rows, i);
        if (other == row)
            continue;
        ClipEntry *other_entry = row_entry(other);
        if (entry->pinned) {
            if (other_entry->pinned)
                index++;
        } else if (other_entry->pinned || other_entry->seq < entry->seq) {
            index++;
        }
    }
    return index;
}

/* GtkListBox has no move; re-insert at the target position. The extra ref
 * keeps the row alive across the container remove. */
static void move_row(Ui *ui, GtkListBoxRow *row, guint index) {
    g_object_ref(row);
    g_ptr_array_remove(ui->rows, row);
    gtk_container_remove(GTK_CONTAINER(ui->listbox), GTK_WIDGET(row));
    gtk_list_box_insert(GTK_LIST_BOX(ui->listbox), GTK_WIDGET(row),
                        (gint)index);
    g_ptr_array_insert(ui->rows, (gint)index, row);
    g_object_unref(row);
}

static void on_pin_clicked(GtkButton *button, gpointer user_data) {
    Ui *ui = user_data;
    GtkListBoxRow *row = GTK_LIST_BOX_ROW(
        gtk_widget_get_ancestor(GTK_WIDGET(button), GTK_TYPE_LIST_BOX_ROW));
    ClipEntry *entry = row_entry(row);

    entry->pinned = !entry->pinned;
    if (entry->pinned)
        g_hash_table_add(ui->pins, g_strdup(entry->id));
    else
        g_hash_table_remove(ui->pins, entry->id);

    GError *error = NULL;
    if (!pins_save(ui->pins, &error)) {
        g_warning("failed to save pins: %s", error->message);
        g_clear_error(&error);
        entry->pinned = !entry->pinned;
        if (entry->pinned)
            g_hash_table_add(ui->pins, g_strdup(entry->id));
        else
            g_hash_table_remove(ui->pins, entry->id);
        return;
    }

    move_row(ui, row, pin_target_index(ui, row));
    gtk_list_box_select_row(GTK_LIST_BOX(ui->listbox), row);
    scroll_to_row(ui, row);
    update_pin_button(row, TRUE);
}

/* --- construction --------------------------------------------------------- */

static GtkWidget *make_row(Ui *ui, ClipEntry *entry) {
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

    GdkPixbuf *pixbuf = pin_pixbuf();
    GtkWidget *icon =
        pixbuf != NULL
            ? gtk_image_new_from_pixbuf(pixbuf)
            : gtk_image_new_from_icon_name("view-pin-symbolic",
                                           GTK_ICON_SIZE_MENU);
    GtkWidget *button = gtk_button_new();
    gtk_widget_set_name(button, "pin-button");
    gtk_button_set_relief(GTK_BUTTON(button), GTK_RELIEF_NONE);
    gtk_widget_set_can_focus(button, FALSE);
    gtk_widget_set_valign(button, GTK_ALIGN_CENTER);
    gtk_container_add(GTK_CONTAINER(button), icon);
    gtk_widget_show(icon);
    /* Visibility is driven by pin/selection state, not show_all. */
    gtk_widget_set_no_show_all(button, TRUE);
    gtk_box_pack_end(GTK_BOX(box), button, FALSE, FALSE, 0);
    g_object_set_data(G_OBJECT(row), "pin-button", button);
    g_signal_connect(button, "clicked", G_CALLBACK(on_pin_clicked), ui);

    gtk_container_add(GTK_CONTAINER(row), box);
    g_object_set_data_full(G_OBJECT(row), "clip-entry", entry,
                           clip_entry_free);
    update_pin_button(GTK_LIST_BOX_ROW(row), FALSE);
    return row;
}

static gboolean append_batch(gpointer data) {
    Ui *ui = data;
    const char *query = gtk_entry_get_text(GTK_ENTRY(ui->entry));
    char *needle = g_utf8_casefold(query, -1);

    guint limit = MIN(ui->pending->len, ui->pending_pos + APPEND_BATCH);
    for (; ui->pending_pos < limit; ui->pending_pos++) {
        ClipEntry *entry = g_ptr_array_index(ui->pending, ui->pending_pos);
        GtkWidget *row = make_row(ui, entry); /* row now owns the entry */
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
    ui->pins = pins_load();

    /* Pinned entries float to the top; seq keeps the newest-first position
     * so an unpinned row can slot back where it belongs. */
    GPtrArray *ordered = g_ptr_array_new();
    for (guint i = 0; i < entries->len; i++) {
        ClipEntry *entry = g_ptr_array_index(entries, i);
        entry->seq = i;
        entry->pinned = g_hash_table_contains(ui->pins, entry->id);
        if (entry->pinned)
            g_ptr_array_add(ordered, entry);
    }
    guint pinned_count = ordered->len;
    for (guint i = 0; i < entries->len; i++) {
        ClipEntry *entry = g_ptr_array_index(entries, i);
        if (!entry->pinned)
            g_ptr_array_add(ordered, entry);
    }
    g_ptr_array_set_free_func(entries, NULL);
    g_ptr_array_unref(entries);
    entries = ordered;

    /* Drop pins whose entry no longer exists in cliphist. */
    if (g_hash_table_size(ui->pins) != pinned_count) {
        g_hash_table_remove_all(ui->pins);
        for (guint i = 0; i < pinned_count; i++) {
            ClipEntry *entry = g_ptr_array_index(entries, i);
            g_hash_table_add(ui->pins, g_strdup(entry->id));
        }
        if (!pins_save(ui->pins, &error)) {
            g_warning("failed to save pins: %s", error->message);
            g_clear_error(&error);
        }
    }

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

    /* Realize every pinned row up front so pending holds only unpinned
     * entries; batches append at the end, keeping list order == rows order. */
    guint initial = MIN(entries->len, MAX((guint)INITIAL_ROWS, pinned_count));
    for (guint i = 0; i < initial; i++) {
        ClipEntry *entry = g_ptr_array_index(entries, i);
        GtkWidget *row = make_row(ui, entry); /* row now owns the entry */
        g_ptr_array_add(ui->rows, row);
        gtk_container_add(GTK_CONTAINER(ui->listbox), row);
    }
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

    g_signal_connect(ui->entry, "changed", G_CALLBACK(on_entry_changed), ui);
    g_signal_connect(ui->listbox, "row-activated",
                     G_CALLBACK(on_row_activated), ui);
    g_signal_connect(ui->listbox, "row-selected",
                     G_CALLBACK(on_row_selected), ui);
    g_signal_connect(ui->listbox, "size-allocate",
                     G_CALLBACK(on_list_allocated), ui);
    g_signal_connect(
        gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(ui->scroller)),
        "value-changed", G_CALLBACK(on_scrolled), ui);

    refilter(ui);

    return ui;
}

void ui_free(Ui *ui) {
    if (ui == NULL)
        return;
    cancel_pending(ui);
    track_selected_row(ui, NULL);
    g_hash_table_unref(ui->pins);
    g_ptr_array_free(ui->rows, TRUE);
    g_free(ui);
}
