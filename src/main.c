#include <gdk/gdkkeysyms.h>
#include <gtk-layer-shell/gtk-layer-shell.h>
#include <gtk/gtk.h>

#include "style_css.h"
#include "ui.h"

#define WINDOW_WIDTH 450
#define HALF_PAGE 5

static void load_css(void) {
    GtkCssProvider *provider = gtk_css_provider_new();
    char *user_css = g_build_filename(g_get_user_config_dir(), "yoinkthis",
                                      "style.css", NULL);
    gboolean loaded = FALSE;

    if (g_file_test(user_css, G_FILE_TEST_IS_REGULAR)) {
        GError *error = NULL;
        loaded = gtk_css_provider_load_from_path(provider, user_css, &error);
        if (!loaded) {
            g_warning("failed to load %s: %s (using built-in theme)",
                      user_css, error->message);
            g_clear_error(&error);
        }
    }
    if (!loaded)
        gtk_css_provider_load_from_data(provider, DEFAULT_CSS, -1, NULL);

    gtk_style_context_add_provider_for_screen(
        gdk_screen_get_default(), GTK_STYLE_PROVIDER(provider),
        GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(provider);
    g_free(user_css);
}

static gboolean on_key_press(GtkWidget *widget, GdkEventKey *event,
                             gpointer user_data) {
    (void)widget;
    Ui *ui = user_data;
    guint key = gdk_keyval_to_lower(event->keyval);
    gboolean ctrl = (event->state & GDK_CONTROL_MASK) != 0;
    gboolean shift = (event->state & GDK_SHIFT_MASK) != 0;

    /* Any key other than the wipe chord cancels a pending wipe confirm. */
    if (ui_wipe_armed(ui) && !(ctrl && shift && key == GDK_KEY_x))
        ui_disarm_wipe(ui);

    switch (event->keyval) {
    case GDK_KEY_Escape:
        gtk_main_quit();
        return TRUE;
    case GDK_KEY_Return:
    case GDK_KEY_KP_Enter:
        ui_activate_selected(ui);
        return TRUE;
    case GDK_KEY_Down:
        ui_move_selection(ui, 1);
        return TRUE;
    case GDK_KEY_Up:
        ui_move_selection(ui, -1);
        return TRUE;
    default:
        break;
    }

    if (!ctrl)
        return FALSE;

    switch (key) {
    case GDK_KEY_j:
        ui_move_selection(ui, 1);
        return TRUE;
    case GDK_KEY_k:
        ui_move_selection(ui, -1);
        return TRUE;
    case GDK_KEY_d:
        ui_move_selection(ui, HALF_PAGE);
        return TRUE;
    case GDK_KEY_u:
        ui_move_selection(ui, -HALF_PAGE);
        return TRUE;
    case GDK_KEY_f:
        ui_cycle_filter(ui);
        return TRUE;
    case GDK_KEY_x:
        if (shift)
            ui_request_wipe(ui);
        else
            ui_delete_selected(ui);
        return TRUE;
    default:
        return FALSE;
    }
}

int main(int argc, char **argv) {
    gtk_init(&argc, &argv);
    load_css();

    GtkWidget *window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(window), "yoinkthis");
    gtk_window_set_default_size(GTK_WINDOW(window), WINDOW_WIDTH, -1);
    gtk_window_set_resizable(GTK_WINDOW(window), FALSE);

    gtk_layer_init_for_window(GTK_WINDOW(window));
    gtk_layer_set_layer(GTK_WINDOW(window), GTK_LAYER_SHELL_LAYER_OVERLAY);
    gtk_layer_set_keyboard_mode(GTK_WINDOW(window),
                                GTK_LAYER_SHELL_KEYBOARD_MODE_EXCLUSIVE);
    gtk_layer_set_namespace(GTK_WINDOW(window), "yoinkthis");

    /* RGBA visual is required for the translucent rofi-like background. */
    GdkVisual *visual =
        gdk_screen_get_rgba_visual(gtk_widget_get_screen(window));
    if (visual != NULL)
        gtk_widget_set_visual(window, visual);

    g_signal_connect(window, "destroy", G_CALLBACK(gtk_main_quit), NULL);

    Ui *ui = ui_build(GTK_WINDOW(window));
    if (ui == NULL)
        return 1;
    g_signal_connect(window, "key-press-event", G_CALLBACK(on_key_press), ui);

    gtk_widget_show_all(window);
    ui_focus_entry(ui);
    gtk_main();

    ui_free(ui);
    return 0;
}
