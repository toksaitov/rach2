# shellcheck shell=sh disable=SC2034 # build_port.sh reads the variables
# GNU make. Jobs start with vfork and exec until clone can share an address
# space. The load directive loads a shared object with dlopen, so a static
# build leaves it out.
VERSION=4.4.1
CONFIGURE_ARGS="--disable-nls --disable-posix-spawn --without-guile"
if [ "$GNU_LINK" = static ]; then
    CONFIGURE_ARGS="$CONFIGURE_ARGS --disable-load"
fi
