#ifndef YOINKTHIS_UI_H
#define YOINKTHIS_UI_H

#include <gtk/gtk.h>

typedef struct Ui Ui;

/* Builds the widget tree inside window and populates it from cliphist.
 * Returns NULL (with a message on stderr) when cliphist can't be read. */
Ui *ui_build(GtkWindow *window);

void ui_focus_entry(Ui *ui);
void ui_move_selection(Ui *ui, int delta);
void ui_activate_selected(Ui *ui); /* copy to clipboard, then quit */
void ui_open_selected(Ui *ui); /* open bare http(s) entries in the browser */
void ui_delete_selected(Ui *ui);
void ui_request_wipe(Ui *ui); /* two-press confirm */
void ui_disarm_wipe(Ui *ui);
gboolean ui_wipe_armed(Ui *ui);
void ui_cycle_filter(Ui *ui); /* all → text → images */
void ui_free(Ui *ui);

#endif
