# lumdm

Tiny, themeable TTY display manager for X11 and Wayland sessions. C23, only dependency: libpam.

    make STD=c23          # GCC 13: make STD=c2x
    ./lumdm --preview     # try the UI without root

Config: `/etc/lumdm.conf` (system), `~/.config/lumdm.conf` (per-user overrides).
Needs systemd-logind or elogind for Wayland and rootless Xorg.
The ASCII starfield and mountain wallpaper is recoloured from the active theme.

## Keys

`Tab`/arrows move between fields, `Left`/`Right` switch session, `Enter` logs in.
`PageUp`/`PageDown` (or `+`/`-` when not typing) zoom the UI; `0` resets it.
The chosen theme and scale are remembered in `/var/lib/lumdm/state`.

Function key actions: `poweroff reboot suspend hibernate theme showpw reload
restart stop scale+ scale- scale0 exec:<command>` (`exec:` is system-config only).
