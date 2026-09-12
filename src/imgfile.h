#ifndef YOINKTHIS_IMGFILE_H
#define YOINKTHIS_IMGFILE_H

#include <glib.h>

#include "cliphist.h"

/* Filesystem path to hand an image viewer for an image entry.
 *
 * cliphist stores image bytes, not paths, so an entry has no location of its
 * own. Screenshot tools nevertheless write the file to disk and copy the very
 * same bytes, so the original can usually be recovered by content. Finding it
 * matters: a viewer opened on the real file sees its siblings, its name and
 * its directory, none of which a scratch copy has.
 *
 * Falls back to a cached copy of the entry when no file matches. Returns NULL
 * with error set. Caller owns the string. */
char *imgfile_resolve(const ClipEntry *entry, GError **error);

/* Drops cached copies whose cliphist id is absent from live_ids. */
void imgfile_prune_cache(GHashTable *live_ids);

#endif
