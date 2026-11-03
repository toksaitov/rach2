#!/bin/sh
# resamplebench.sh: build tools/audio/resamplebench.c with the i686 and the
# x86-64 kernel compilers, at no optimization and with the kernel's code
# generation flags, and run each build on this Linux host.
#
# Usage: sh tools/audio/resamplebench.sh [ARCH...]
#
# ARCH is i686 or x86_64, both when none is given. The x86-64 build takes
# the small code model instead of the kernel's, since it runs at a user
# address; the code of a loop is the same. The i686 build needs a host
# kernel that runs 32-bit programs.
set -eu

top=$(cd "$(dirname "$0")/../.." && pwd)
out=$top/build/tmp/resamplebench
mkdir -p "$out"
[ $# -gt 0 ] || set -- i686 x86_64
for arch in "$@"; do
    case $arch in
    i686) cc=i686-elf-gcc flags='-m32 -mno-sse -mno-sse2' ;;
    x86_64) cc=x86_64-elf-gcc flags='-m64 -mno-red-zone -mno-mmx -mno-sse -mno-sse2 -fno-asynchronous-unwind-tables' ;;
    *)
        echo "resamplebench: unknown arch: $arch" >&2
        exit 2
        ;;
    esac
    # shellcheck disable=SC2086 # the flags are words
    "$cc" $flags -O0 -ansi -Wall -Wextra -ffreestanding -fno-pie -nostdlib -static \
        -iquote "$top/rach2/sys" -o "$out/resamplebench-$arch" "$top/tools/audio/resamplebench.c"
    echo "resamplebench: $arch"
    "$out/resamplebench-$arch"
done
