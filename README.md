# yoinkthis

Rofi-style clipboard history picker for Hyprland, with inline image thumbnails.
A single small C binary that opens as a centered layer-shell overlay — cold start
well under 100ms.

It is a pure **picker** over the [cliphist](https://github.com/sentriz/cliphist)
database — it never captures clipboard changes itself. See
[Clipboard capture](#clipboard-capture) for the required watchers.

## Requirements

**Build** (Arch package names):

- `gcc`, `make`, `pkgconf`
- `gtk3`
- `gtk-layer-shell`

**Runtime** (spawned as subprocesses):

- `cliphist` — clipboard history database
- `wl-clipboard` — provides `wl-copy`/`wl-paste`
- `xdg-utils` — provides `xdg-open`, used to open links and images

**Compositor:** anything implementing the `wlr-layer-shell` protocol
(Hyprland, Sway, river, …). GNOME and KDE do not implement it.

## Build

```
make
sudo make install    # installs to /usr/local/bin, override with PREFIX=
```

## Clipboard capture

yoinkthis only reads history; something must be writing it. Two `wl-paste`
watchers have to run in the background and store every new copy into cliphist:

```
wl-paste --type text/plain --watch cliphist store
wl-paste --type image      --watch cliphist store
```

Some shells already manage these for you (e.g. noctalia-shell with clipboard
history enabled) — in that case there is nothing to do. Otherwise autostart
them from your compositor config. Hyprland:

```
exec-once = wl-paste --type text/plain --watch cliphist store
exec-once = wl-paste --type image --watch cliphist store
```

Lua-based config:

```lua
hl.exec_once("wl-paste --type text/plain --watch cliphist store")
hl.exec_once("wl-paste --type image --watch cliphist store")
```

Without the image watcher, images are never stored and the `[img]` filter will
always be empty.

Use `text/plain`, not `text`. The `text` shorthand also matches `text/html`, and
several sources offer HTML alongside the image — Firefox's "Copy Image" is one.
With `--type text` that watcher also stores such a copy, so one image ends up in
history twice: once as the picture, once as a blob of HTML markup. `text/plain`
declines those copies and leaves the image watcher to store the real thing.

A single untyped `wl-paste --watch cliphist store` has the same problem, for the
same reason: with no type requested, wl-paste prefers text when the source
offers any.

## Hyprland

Classic `hyprland.conf` syntax:

```
bind = SUPER, V, exec, yoinkthis
```

Lua-based config (hyprland.lua):

```lua
hl.bind(mainMod .. " + V", hl.dsp.exec_cmd("yoinkthis"))
```

Use the full binary path instead of `yoinkthis` if you skipped `make install`.
No window rules needed — the overlay layer and centering come from layer-shell.

## Keys

| Key | Action |
|---|---|
| type | filter entries |
| `Ctrl+j` / `Ctrl+k` (or arrows) | move selection |
| `Ctrl+d` / `Ctrl+u` | jump half page |
| `Enter` / click | copy entry to clipboard and close |
| `Shift+Enter` | open entry: links in the browser, images in the viewer |
| `Esc` | close |
| `Ctrl+x` | delete selected entry (no-op on pinned) |
| `Ctrl+Shift+X` twice | wipe all history except pinned |
| `Ctrl+f` | cycle filter: all → text → images |

## Links

An entry whose whole content is a single `http://` or `https://` URL is shown
in blue. `Shift+Enter` hands it to `xdg-open` and closes the picker, without
touching the clipboard. Nothing happens on any other entry — no other scheme is
accepted, and text with the URL buried in it does not count.

Detection runs on the full entry, not the list preview, so URLs longer than
cliphist's 100-character preview still open. The blue styling does come from
the preview, which is why a very long URL is colored from its first characters.

## Images

`Shift+Enter` on an image entry opens it in an image viewer and closes the
picker, again without touching the clipboard.

cliphist stores image bytes, not paths, so an entry has no location of its own.
Screenshot tools write the file to disk and copy those same bytes, though, so
the original can be recovered by content: yoinkthis compares the entry against
files of the same size and confirms a candidate byte for byte. The size check
rejects nearly everything without opening a file, which is what keeps this to a
few milliseconds over a folder of a few thousand images. It runs on the
keypress, never at startup.

Finding the original is worth the trouble because the viewer then gets a real
file in a real directory: its actual name, and its neighbours to page through.
A scratch copy has neither.

The search covers the XDG pictures directory, a few levels deep, skipping
hidden directories. Override it with `YOINKTHIS_IMAGE_DIRS`, a `:`-separated
list of directories:

```
YOINKTHIS_IMAGE_DIRS=~/Pictures/Screenshots:~/Downloads
```

When nothing matches, which is the normal case for an image copied straight out
of a browser, the entry is written to `~/.cache/yoinkthis/full/` and that copy
is opened instead. The image still opens; only the surrounding directory is
lost.

`xdg-open` picks the viewer, so the handler for the image's type decides what
appears. To bypass that for yoinkthis alone, set `YOINKTHIS_IMAGE_VIEWER`. It
may carry arguments, and the path is appended to them:

```
YOINKTHIS_IMAGE_VIEWER="swayimg --gallery"
```

## Pinning

Selecting a row reveals a translucent pushpin at its right edge — click it to
pin the entry (mouse only, no keybind). Pinned entries:

- float to the top of the list, above the newest-first history;
- always show a solid white pin icon;
- cannot be deleted: `Ctrl+x` ignores them and the `Ctrl+Shift+X` wipe removes
  everything else but keeps them. Unpin first (click the solid pin) to delete.

Pins persist across sessions in `~/.local/state/yoinkthis/pins` (one cliphist
id per line).

Note: cliphist dedupes re-copied content under a new id, so re-copying a
pinned item creates a fresh unpinned entry; the orphaned pin is pruned on the
next launch.

## Theming

The built-in theme mirrors `~/.config/rofi/config.rasi`. To customize, copy
`style.css` to `~/.config/yoinkthis/style.css` and edit — it is plain GTK CSS,
no rebuild needed.

## Caches

Two caches live under `~/.cache/yoinkthis/`: `thumbs/` holds the scaled list
thumbnails, `full/` the copies made for images with no file of their own. Both
are keyed by cliphist id and both are regenerable, so losing either costs a
decode and nothing else.

Deleting an entry drops its cached files, and a wipe drops every file whose
entry is gone. A wipe that spares pinned entries therefore spares their caches
too, and a wipe with nothing pinned empties both directories. Your own image
folders are only ever read, never written or deleted.
