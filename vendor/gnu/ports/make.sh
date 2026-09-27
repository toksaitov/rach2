# GNU make. Jobs start with vfork and exec until clone can share an address
# space, and the load directive, which needs dlopen, is left out.
VERSION=4.4.1
CONFIGURE_ARGS="--disable-nls --disable-posix-spawn --disable-load --without-guile"
