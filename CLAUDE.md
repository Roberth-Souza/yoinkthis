# yoinkthis — rofi-style clipboard picker for Hyprland

Centered layer-shell overlay listing clipboard history with inline image thumbnails.
Pure **reader/picker** over the shared cliphist database: clipboard *capture* is owned by
noctalia-shell, which runs `wl-paste --type text/image --watch cliphist store`. Never add
watchers or storage logic here.

## Stack
C11 + GTK3 + gtk-layer-shell. Runtime helpers spawned as subprocesses: `cliphist`, `wl-copy`.
No other dependencies — keep it that way.

## Build & run
- `make` → `./yoinkthis`
- `make install` → `$(PREFIX)/bin` (default `/usr/local`)
- `make clean`

## File map
- `src/main.c` — gtk_init + plain `gtk_main` (deliberately **not** GtkApplication: avoids
  D-Bus registration on the startup path), layer-shell window (OVERLAY layer, exclusive
  keyboard, no anchors → compositor centers), CSS loading, global keybindings.
- `src/ui.c/.h` — inputbar (prompt + entry) and GtkListBox rows; manual row visibility
  filtering (no GtkListBox filter func — we need the visible set for nvim navigation),
  selection movement, viewport-lazy thumbnail requests.
- `src/cliphist.c/.h` — GSubprocess wrappers: `list`, `decode`, `delete`, `wipe`.
  Copy = decode to memory → pipe into `wl-copy` stdin (no shell involved).
  `ClipEntry.line` keeps the raw undecoded line bytes — required by `cliphist delete`.
- `src/thumbs.c/.h` — GTask worker-thread thumbnails; disk cache of already-scaled PNGs at
  `~/.cache/yoinkthis/thumbs/<id>.png` (cleared on wipe).
- `style.css` — default theme; embedded at build time into `src/style_css.h` (generated,
  never edit or commit that header). User override: `~/.config/yoinkthis/style.css`.

## Keybindings
| Key | Action |
|---|---|
| type | filter entries |
| `Ctrl+j/k`, `Down/Up` | move selection |
| `Ctrl+d/u` | jump half page |
| `Enter` / click | copy entry to clipboard and close |
| `Esc` | close |
| `Ctrl+x` | delete selected entry |
| `Ctrl+Shift+X` (twice) | wipe all history |
| `Ctrl+f` | cycle filter: all → text → images |

## Hard constraints
- **Instant startup** (<100ms to visible window): nothing synchronous on the main path
  besides `cliphist list`; image decoding always async in worker threads.
- **Visual parity with rofi**: colors, font, border, paddings mirror
  `~/.config/rofi/config.rasi` 1:1 (bg `rgba(5,5,5,0.95)`, 2px white border,
  JetBrainsMono Nerd Font 12, selection `rgba(68,68,68,0.6)`).
- Builds clean with `-Wall -Wextra -Wpedantic`.

## Conventions
- English comments; comments only for what the code can't say.
- Conventional Commits; **no AI attribution anywhere**.
