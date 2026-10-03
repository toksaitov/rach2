# shellcheck shell=sh disable=SC2034 # the variables are the callers'
# mk/arch.sh: the shell form of mk/arch.mk, the architecture a script builds
# or stages for.
#
# Sourced by each vendored port's build_*.sh script, by vendor/musl/muslgcc
# and by the image scripts under tools/image. The root Makefile names the
# architecture in RACH2_ARCH, `i686` when it is unset, and this file sets from
# it:
#
#   CROSS        the cross toolchain's prefix, `i686-elf-` or `x86_64-elf-`
#   MFLAG        the word-size flag every compile and link of the port takes
#   MUSL_TARGET  musl's configure target
#   HOST_TRIPLE  the --host an autoconf configure is given
#   ARCH_SUFFIX  what this architecture's build tree adds after `build`, as
#                mk/arch.mk says: empty for i686, `/x86_64` for x86-64
#   ARCH_LDFLAGS what a final link adds: 4 KiB segment alignment on x86-64
#   ARCH_QEMU    the emulator that boots this architecture's kernel
#   ARCH_USER_TEXT the address a freestanding program's text is linked at
#   ARCH_VENDOR  where a port's extracted tree, and musl's install, go: vendor/
#                for i686, build/x86_64/vendor-work/ for x86-64
#   MUSL_PREFIX  the ported musl's install directory
#   ARCH_BUILD   the build tree: the kernels, the guest programs, what each
#                port stages, and the images
#
# ARCH itself is taken out of the environment, and so is a `make ARCH=...`
# above this script, which GNU Make passes down in MAKEFLAGS after a ` -- `.
# musl, BusyBox and TinyCC each have a make variable named ARCH of their own,
# with their own values, and an inherited one would override it. The rest of
# MAKEFLAGS is kept: it carries the caller's jobserver.
#
# The caller sets REPO, the repository's root, as an absolute path. The file
# sets variables and exits on an unknown architecture; it runs nothing else,
# so the wrapper can source it on every compile.

RACH2_ARCH="${RACH2_ARCH:-i686}"
export RACH2_ARCH
unset ARCH
unset MFLAGS
MAKEFLAGS="${MAKEFLAGS-}"
case "$MAKEFLAGS" in
    --\ *) MAKEFLAGS= ;;
esac
MAKEFLAGS="${MAKEFLAGS%% -- *}"
export MAKEFLAGS

case "$RACH2_ARCH" in
    i686)
        CROSS=i686-elf-
        MFLAG=-m32
        MUSL_TARGET=i686
        HOST_TRIPLE=i686-linux-musl
        ARCH_SUFFIX=
        ARCH_LDFLAGS=
        ARCH_QEMU=qemu-system-i386
        ARCH_USER_TEXT=0x08048000
        ;;
    x86_64)
        CROSS=x86_64-elf-
        MFLAG=-m64
        MUSL_TARGET=x86_64
        HOST_TRIPLE=x86_64-linux-musl
        ARCH_SUFFIX=/x86_64
        ARCH_LDFLAGS=-Wl,-z,max-page-size=0x1000
        ARCH_QEMU=qemu-system-x86_64
        ARCH_USER_TEXT=0x400000
        ;;
    *)
        echo "rach2: unknown architecture '$RACH2_ARCH'; the architectures are i686 and x86_64" >&2
        exit 1
        ;;
esac

ARCH_BUILD="$REPO/build$ARCH_SUFFIX"
if [ -n "$ARCH_SUFFIX" ]; then
    ARCH_VENDOR="$ARCH_BUILD/vendor-work"
else
    ARCH_VENDOR="$REPO/vendor"
fi
MUSL_PREFIX="$ARCH_VENDOR/musl/install"
