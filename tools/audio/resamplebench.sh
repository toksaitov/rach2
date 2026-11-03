#!/bin/sh
# resamplebench.sh: build tools/audio/resamplebench.c with the i686 and the
# x86-64 kernel compilers, at no optimization and with the kernel's code
# generation flags, by the rules of mk/audio-tools.mk, and run each build
# on this Linux host.
#
# Usage: sh tools/audio/resamplebench.sh [ARCH...]
#
# ARCH is i686 or x86_64, both when none is given. The x86-64 build takes
# the small code model instead of the kernel's, since it runs at a user
# address; the code of a loop is the same. The i686 build needs a host
# kernel that runs 32-bit programs.
set -eu

top=$(cd "$(dirname "$0")/../.." && pwd)
[ $# -gt 0 ] || set -- i686 x86_64
for arch in "$@"; do
    case $arch in
    i686 | x86_64) ;;
    *)
        echo "resamplebench: unknown arch: $arch" >&2
        exit 2
        ;;
    esac
    make -s -C "$top" "build/tools/audio/resamplebench-$arch"
    echo "resamplebench: $arch"
    "$top/build/tools/audio/resamplebench-$arch"
done
