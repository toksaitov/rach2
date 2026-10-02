# shellcheck shell=sh disable=SC2034 # build_port.sh reads the variables
# procps-ng: ps, top, w, uptime, free, vmstat, watch, pgrep, pkill, pidof,
# pmap, kill, sysctl and slabtop, which read the machine's processes and
# memory from /proc, as a Debian machine has them; top and watch draw with
# ncurses. systemd and elogind are not in the root, so the session of a
# process is read from /proc alone.
VERSION=4.0.7
DEPENDS=ncurses
CONFIGURE_ARGS="--disable-nls --disable-static --without-systemd --without-elogind --enable-watch8bit"
