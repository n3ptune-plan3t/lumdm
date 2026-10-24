# lumdm

Tiny, themeable TTY display manager for X11 and Wayland sessions. C23, only dependency: libpam.

    make STD=c23          # GCC 13: make STD=c2x
    ./lumdm --preview     # try the UI without root

Config: `/etc/lumdm.conf` (system), `~/.config/lumdm.conf` (per-user overrides).
Needs systemd-logind or elogind for Wayland and rootless Xorg.
