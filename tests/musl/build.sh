#!/usr/bin/env sh

#
# Build the musl-linked test programs in this directory.
#
# These differ from the freestanding probes in tests/: they are ordinary hosted
# C programs linked statically against the ported musl (see
# vendor/musl/build_libc.sh). Each <name>.c here becomes a guest binary
# build/tests/musl/<name> (out of tree, like every other component), staged into
# the test image under /musl by its manifest (so /musl/hello, /musl/mallocqsort,
# ...). `make musl-programs` runs this.
#
# A program that must link a source from elsewhere in the tree names it in a
# "link-deps:" comment, the convention the host suite parses for the host
# tests, with the paths written from the repo root:
#
#     /**
#      * ...
#      *
#      * link-deps: utils/pwauth.c
#      */
#
# The utils headers are on the include path for the same reason, so a probe can
# test the library its program links. A named dependency that is not there is a
# hard error: passed on to the compiler it would surface as a bare "no such
# file", which reads as a broken toolchain, not a typo.
#
# The build is incremental: a program recompiles only when its source, a
# link-deps source, a project header its previous compile recorded (-MMD),
# the installed libc, or this script is newer than the binary. An untouched
# tree therefore compiles nothing.
#
# Run vendor/musl/build_libc.sh first if install/ is absent.
#

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
cd "$SCRIPT_DIR"

REPO="$(cd "$SCRIPT_DIR/../.." && pwd)"
PREFIX="$SCRIPT_DIR/../../vendor/musl/install"
CC="i686-elf-gcc"
LIBGCC="$($CC -print-libgcc-file-name)"

# Out of tree, mirroring the source path tests/musl/ (mk/build.mk).
OUT="$(cd "$SCRIPT_DIR/../.." && pwd)/build/tests/musl"

if [ ! -f "$PREFIX/lib/libc.a" ]; then
    echo "error: $PREFIX/lib/libc.a not found; run vendor/musl/build_libc.sh first" >&2
    exit 1
fi

# stale OUT DEP...: succeed when OUT is missing or any dependency (given, or
# recorded in OUT.d by the previous compile) is missing or newer than OUT.
stale() {
    stale_out=$1
    shift
    [ -f "$stale_out" ] || return 0
    for f in "$@"; do
        [ -e "$f" ] || return 0
        [ "$f" -nt "$stale_out" ] && return 0
    done
    if [ -f "$stale_out.d" ]; then
        # Strip the target token and continuation backslashes; every other
        # token is a recorded header path.
        for f in $(sed 's/\\//g' "$stale_out.d" | tr ' ' '\n' |
                       sed -e '/:$/d' -e '/^$/d'); do
            [ -e "$f" ] || return 0
            [ "$f" -nt "$stale_out" ] && return 0
        done
    fi
    return 1
}

mkdir -p "$OUT"
for src in *.c; do
    prog="${src%.c}"
    deps=""
    # Every link-deps: line, not just the first: the declaration is repeatable
    # for the same reason the host tests' is, and `head -1` silently dropped a
    # second one. A wrapped declaration is refused outright, because the
    # `[^*]*` grep stops at the next `*` and would drop the paths past the
    # fold without saying so.
    if ! awk '
        /link-deps:/ && prev ~ /link-deps:/ { prev = $0; next }
        prev ~ /link-deps:/ && $0 ~ /^[[:space:]]*\*[[:space:]]+[A-Za-z0-9_.\/-]+\.[ch]([[:space:]]|$)/ { exit 1 }
        { prev = $0 }
    ' "$src"; then
        echo "error: $src has a wrapped link-deps: line; repeat the" >&2
        echo "error: link-deps: prefix on the next line instead" >&2
        exit 1
    fi
    for dep in $(grep -oE 'link-deps:[^*]*' "$src" | sed 's/link-deps://'); do
        if [ ! -f "$REPO/$dep" ]; then
            echo "error: $src names a link-deps: source that is not there: $dep" >&2
            exit 1
        fi
        deps="$deps $REPO/$dep"
    done
    # shellcheck disable=SC2086
    stale "$OUT/$prog" "$src" $deps "$PREFIX/lib/libc.a" \
        "$SCRIPT_DIR/build.sh" || continue
    echo "  CC $prog"
    # shellcheck disable=SC2086
    $CC -static -nostdlib -nostartfiles -isystem "$PREFIX/include" -I ../../utils -O2 \
        -MMD -MF "$OUT/$prog.d" \
        -o "$OUT/$prog" \
        "$PREFIX/lib/crt1.o" "$PREFIX/lib/crti.o" "$src" $deps \
        "$PREFIX/lib/libc.a" "$LIBGCC" "$PREFIX/lib/crtn.o" \
        -Wl,-z,noexecstack
done
