# shellcheck shell=sh disable=SC2034 # build_port.sh reads the variables
# htop, the interactive process viewer, with ncurses' wide-character
# library. Its configure would run a program to see whether
# sched_setaffinity works, which a cross build cannot, so affinity is asked
# for by name; the kernel has the call. Sensors, capabilities, delay
# accounting, hwloc and libunwind are left out, since the root has none of
# their libraries.
VERSION=3.5.3
DEPENDS=ncurses
CONFIGURE_ARGS="--enable-unicode --enable-affinity --disable-hwloc --disable-capabilities --disable-delayacct --disable-sensors --without-libunwind"
