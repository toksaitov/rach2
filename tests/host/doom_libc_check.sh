#!/bin/sh
# Compile Doom's C library, ports/doom/crt0.c and ports/doom/include, with the
# freestanding fixture tests/host/fixtures/doom_libc.c for the i686 and x86-64
# ABIs, and run both programs. A program passes when it exits 0 and prints its
# line of 64-bit formats. The host suite runs the check, and
# tests/proofs/doom-libc.json builds and runs its faults with it.
#
# Usage: sh tests/host/doom_libc_check.sh
#        sh tests/host/doom_libc_check.sh build SRC DIR
#        sh tests/host/doom_libc_check.sh run DIR
# With no operand it builds from the tree in a scratch directory and runs.
# `build` copies the library into DIR/lib, each file from SRC/ports/doom where
# SRC has it and from the tree otherwise, and compiles DIR/doom_libc-i686 and
# DIR/doom_libc-x86_64. `run` runs those two for at most 10 seconds each,
# prints `doom-libc-check: ARCH: status N` for each, and ends with
# `doom-libc-check: result: pass` and status 0, or
# `doom-libc-check: result: fail: ARCH...` and status 1; a program that ends
# with a status above 1 makes it exit 2.
set -eu
repo=$(CDPATH='' cd -- "$(dirname -- "$0")/../.." && pwd)
cd "$repo"
format='doom-libc-format: -9223372036854775808 18446744073709551615 ffffffffffffffff 37'

# build SRC DIR: copy the library into DIR/lib and compile both programs.
build()
{
    rm -rf "${2:?}/lib"
    mkdir -p "$2/lib"
    cp -R ports/doom/include ports/doom/crt0.c "$2/lib/"
    if [ "$1" != "$repo" ]; then
        (cd "$1/ports/doom" && find . -type f) | while IFS= read -r f; do
            cp "$1/ports/doom/$f" "$2/lib/$f"
        done
    fi
    for arch in i686 x86_64; do
        "${arch}-elf-gcc" -std=gnu99 -Os -ffreestanding -fno-pie -no-pie \
            -nostdlib -static -Wl,--build-id=none -I "$2/lib/include" \
            "$2/lib/crt0.c" tests/host/fixtures/doom_libc.c -o "$2/doom_libc-$arch"
    done
}

# run DIR: run both programs and judge them.
run()
{
    failed=
    worst=0
    for arch in i686 x86_64; do
        out="$1/doom_libc-$arch.out"
        rm -f "$1/doom_libc-$arch.data"
        status=0
        perl -e 'alarm 10; exec @ARGV or die "doom-libc-check: exec: $!\n"' \
            "$1/doom_libc-$arch" "$1/doom_libc-$arch.data" >"$out" 2>&1 || status=$?
        cat "$out"
        echo "doom-libc-check: $arch: status $status"
        [ "$status" -le 1 ] || worst=2
        if [ "$status" -ne 0 ] || ! grep -Fxq "$format" "$out"; then
            failed="$failed $arch"
        fi
    done
    if [ "$worst" -ne 0 ]; then
        echo "doom-libc-check: result: fail:$failed: a program ended with a status above 1"
        exit 2
    fi
    if [ -n "$failed" ]; then
        echo "doom-libc-check: result: fail:$failed"
        exit 1
    fi
    echo 'doom-libc-check: result: pass'
}

case $#:${1:-} in
    3:build)
        build "$2" "$3"
        exit 0
        ;;
    2:run)
        run "$2"
        exit 0
        ;;
    0:) ;;
    *)
        echo 'doom-libc-check: usage: doom_libc_check.sh [build SRC DIR | run DIR]' >&2
        exit 2
        ;;
esac
mkdir -p build/tests
work=
trap '[ -z "$work" ] || rm -rf "$work"' EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM
work=$(mktemp -d "$repo/build/tests/doom-libc-check.XXXXXX")
build "$repo" "$work"
run "$work"
