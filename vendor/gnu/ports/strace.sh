# shellcheck shell=sh disable=SC2034 # build_port.sh reads the variables
# strace, the system call tracer. It decodes the calls of i386 Linux, the
# interface the kernel gives, and has no stack traces, since libdw and
# libunwind are not ported, and no personalities beside i386's.
VERSION=7.2
CONFIGURE_ARGS="--enable-mpers=no --enable-stacktrace=no --disable-gcc-Werror --without-libselinux"
