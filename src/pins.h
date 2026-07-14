#ifndef YOINKTHIS_PINS_H
#define YOINKTHIS_PINS_H

#include <glib.h>

/* Pinned cliphist ids persisted at $XDG_STATE_HOME/yoinkthis/pins,
 * one id per line. */

/* Returns a string set (id -> itself) with g_free key destroy; empty set
 * when the pin file does not exist yet. Caller owns the table. */
GHashTable *pins_load(void);

gboolean pins_save(GHashTable *pins, GError **error);

#endif
