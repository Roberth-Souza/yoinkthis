# yoinkthis

Rofi-style clipboard history picker for Hyprland, with inline image thumbnails.
A single small C binary that opens as a centered layer-shell overlay — cold start
well under 100ms.

It is a pure **picker** over the [cliphist](https://github.com/sentriz/cliphist)
database. Something must feed cliphist (noctalia-shell already does on this setup);
otherwise run:

```
wl-paste --type text  --watch cliphist store
wl-paste --type image --watch cliphist store
```

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
| `Ctrl+x` | delete selected entry |
| `Ctrl+Shift+X` twice | wipe all history |
| `Ctrl+f` | cycle filter: all → text → images |

## Theming

The built-in theme mirrors `~/.config/rofi/config.rasi`. To customize, copy
`style.css` to `~/.config/yoinkthis/style.css` and edit — it is plain GTK CSS,
no rebuild needed.

Image thumbnails are cached (already scaled) in `~/.cache/yoinkthis/thumbs/`
and cleared automatically on wipe.
