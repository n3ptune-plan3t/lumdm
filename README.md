# lumdm

Tiny, themeable TTY display manager for X11 and Wayland sessions.

    make STD=c23          # GCC 13 or clang: make STD=c2x
    ./lumdm --preview     # try the UI without root
    ./lumdm --check user  # show the effective config

    
only dependencies: libpam and libm

Config: `/etc/lumdm.conf` (system), `~/.config/lumdm.conf` (per-user overrides; symlinks are fine).
Needs systemd-logind or elogind for Wayland and rootless Xorg.

## Wallpapers

ASCII scenes recoloured from the active theme: `festival`, `ocean`, `forest`,
`none`, or `random`. Set `wallpaper = ...` or press the key bound to `wallpaper` to cycle.

## Fonts

    font = JetBrains Mono     # name or absolute path of any installed .ttf/.otf/.ttc  (system file only)
    font_size = 16            # cell height in px

The font is rasterised into a 256-glyph console font (ASCII + Latin-1; box drawing, rounded corners, blocks,
bullets are drawn by lumdm so they always fit the cell), loaded with `KDFONTOP`, and the original console font
is restored before a session starts and when lumdm exits. Rasterising runs in an unprivileged child process.
With a font loaded, the zoom keys change the real font size instead of stretching the panel.
Preview a font without touching the console: `./lumdm --font-test "DejaVu Sans Mono" 16`.

## Keys

`Tab`/arrows move between fields, `Left`/`Right` switch session, `Enter` logs in.
`PageUp`/`PageDown` (or `+`/`-` when not typing) zoom the UI; `0` resets it.

Function key actions: `poweroff reboot suspend hibernate theme wallpaper showpw reload restart stop scale+
scale- scale0 exec:<command>` (`exec:` is system-config only). Keys and values are case-insensitive;
`fN_label = text` sets the bottom-bar label.
