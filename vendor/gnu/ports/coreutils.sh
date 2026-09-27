# GNU coreutils. gnulib's own posix_spawn, which starts the child with vfork,
# replaces glibc's until clone can share an address space; stdbuf needs a
# dynamic loader for its preloaded library. A cross build cannot run gnulib's
# mktime test, and its replacement would collide with glibc's in a static
# link, so glibc's is declared working.
VERSION=9.12
CACHE="gl_cv_func_posix_spawn_works=no utils_cv_stdbuf_supported=no gl_cv_func_working_mktime=yes"
CONFIGURE_ARGS="--disable-nls --disable-acl --disable-xattr --disable-libcap --without-selinux --without-libgmp --without-openssl"
