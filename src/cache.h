#ifndef YOINKTHIS_CACHE_H
#define YOINKTHIS_CACHE_H

#include <glib.h>

/* Disk caches under $XDG_CACHE_HOME/yoinkthis/<name>/, holding files named
 * "<cliphist id>.<ext>". Everything stored here is regenerable from cliphist,
 * so dropping any of it costs a decode, never data. */

/* Path of a cache subdirectory, which may not exist yet. */
char *cache_dir(const char *name);

/* Path of one entry's file inside a cache subdirectory. */
char *cache_path(const char *name, const char *id, const char *ext);

/* Creates the subdirectory, parents included. */
gboolean cache_ensure_dir(const char *name);

/* Drops cached files whose cliphist id is absent from live_ids, a string set
 * borrowed for the call. An empty set therefore clears the cache outright. */
void cache_prune(const char *name, GHashTable *live_ids);

#endif
