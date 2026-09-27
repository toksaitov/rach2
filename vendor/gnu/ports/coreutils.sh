# shellcheck shell=sh disable=SC2034 # build_port.sh reads the variables
# GNU coreutils. A cross build cannot run gnulib's mktime test, and its
# replacement would collide with glibc's in a static link, so glibc's is
# declared working. stdbuf works by preloading a shared library, so a static
# build leaves it out.
VERSION=9.12
CACHE="gl_cv_func_working_mktime=yes"
if [ "$GNU_LINK" = static ]; then
    CACHE="$CACHE utils_cv_stdbuf_supported=no"
fi
CONFIGURE_ARGS="--disable-nls --disable-acl --disable-xattr --disable-libcap --without-selinux --without-libgmp --without-openssl"
