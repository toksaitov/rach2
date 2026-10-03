# shellcheck shell=sh disable=SC2034 # build_port.sh reads the variables
# strace, the system call tracer. It decodes the calls of the architecture it
# is built for, i386's or x86-64's, the interface the kernel gives, and has
# no stack traces, since libdw and libunwind are not ported, and no
# personality beside that architecture's: an x86-64 Rach2 runs no i386
# program.
VERSION=7.2
CONFIGURE_ARGS="--enable-mpers=no --enable-stacktrace=no --disable-gcc-Werror --without-libselinux"
