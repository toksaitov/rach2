# shellcheck shell=sh disable=SC2034 # the variables are the callers'
# vendor/gnu/arch.sh: the GNU layer's names for the architecture a build is
# for, which mk/arch.sh reads from RACH2_ARCH, `i686` when it is unset.
#
# Sourced by the GNU layer's scripts, which set REPO first. It sets:
#
#   GNU_OUT       the layer's build tree: build/vendor/gnu for i686, as it
#                 always was, and build/x86_64/vendor/gnu for x86-64
#   GNU_TARGET    the toolchain's target, `i686-linux-gnu` or
#                 `x86_64-rach2-linux-gnu`; the build host is an x86-64
#                 Linux, whose own compilers are named x86_64-linux-gnu-gcc
#                 and the rest, and GCC and autoconf take a target of the
#                 host's own triplet for a native build, so the x86-64 one
#                 has a vendor of its own, as Linux From Scratch's does
#   GNU_GCC_ARCH  the instruction set GCC is configured for (--with-arch)
#   GNU_LOADER    glibc's dynamic loader, the interpreter a program asks
#                 for: /lib/ld-linux.so.2 on i686, /lib64/ld-linux-x86-64.so.2
#                 on x86-64, where the shared libraries are in /lib and
#                 /usr/lib on both
#   GNU_DEB_ARCH  Debian's name for the architecture, `i386` or `amd64`
#   GNU_MULTIARCH the directory Debian's packages keep their libraries in
#                 under /usr/lib, `i386-linux-gnu` or `x86_64-linux-gnu`

. "$REPO/mk/arch.sh"

GNU_OUT="$REPO/build$ARCH_SUFFIX/vendor/gnu"
case "$RACH2_ARCH" in
    i686)
        GNU_TARGET=i686-linux-gnu
        GNU_GCC_ARCH=i686
        GNU_LOADER=/lib/ld-linux.so.2
        GNU_DEB_ARCH=i386
        GNU_MULTIARCH=i386-linux-gnu
        ;;
    x86_64)
        GNU_TARGET=x86_64-rach2-linux-gnu
        GNU_GCC_ARCH=x86-64
        GNU_LOADER=/lib64/ld-linux-x86-64.so.2
        GNU_DEB_ARCH=amd64
        GNU_MULTIARCH=x86_64-linux-gnu
        ;;
esac
