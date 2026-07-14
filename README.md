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
wl-paste --type text  --watch cliphist store
wl-paste --type image --watch cliphist store
```

Some shells already manage these for you (e.g. noctalia-shell with clipboard
history enabled) — in that case there is nothing to do. Otherwise autostart
them from your compositor config. Hyprland:

```
exec-once = wl-paste --type text --watch cliphist store
exec-once = wl-paste --type image --watch cliphist store
```

Lua-based config:

```lua
hl.exec_once("wl-paste --type text --watch cliphist store")
hl.exec_once("wl-paste --type image --watch cliphist store")
```

Without the image watcher, images are never stored and the `[img]` filter will
always be empty.

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
| `Esc` | close |
| `Ctrl+x` | delete selected entry (no-op on pinned) |
| `Ctrl+Shift+X` twice | wipe all history except pinned |
| `Ctrl+f` | cycle filter: all → text → images |

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

Image thumbnails are cached (already scaled) in `~/.cache/yoinkthis/thumbs/`
and cleared automatically on wipe (kept when the wipe spares pinned entries).
