#!/usr/bin/env sh

#
# Build the musl-linked test programs in this directory.
#
# These differ from the freestanding probes in tests/: they are ordinary hosted
# C programs linked statically against the ported musl, which `make musl`
# builds. Each <name>.c here becomes a guest binary build/tests/musl/<name>
# (out of tree, like every other component), staged into the test image under
# /musl by its manifest (so /musl/hello, /musl/mallocqsort, ...).
# `make musl-programs` runs this.
#
# The architecture is RACH2_ARCH's, i686 when it is unset, as mk/arch.sh reads
# it: the root Makefile sets it from ARCH. An x86-64 build compiles with
# x86_64-elf-gcc against the x86-64 musl and writes under
# build/x86_64/tests/musl/.
#
# A program that must link a source from elsewhere in the tree names it in a
# "link-deps:" comment, the counterpart of the "host-deps:" line the host
# suite reads from a host test, with the paths written from the repo root:
#
#     /**
#      * ...
#      *
#      * link-deps: utils/pwauth.c
#      */
#
# The utils headers are on the include path for the same reason, so a probe can
# test the library its program links. A named dependency that is not there
# stops the build with an error that names it; passed on to the compiler, it
# would give a bare "no such file", which reads as a broken toolchain.
#
# The build is incremental: a program recompiles only when its source, a
# link-deps source, a project header its previous compile recorded (-MMD),
# the installed libc, mk/arch.sh or this script is newer than the binary. An
# untouched tree therefore compiles nothing.
#
# Run `make musl` first if the installed libc is absent.
#

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
cd "$SCRIPT_DIR"

REPO="$(cd "$SCRIPT_DIR/../.." && pwd)"
. "$REPO/mk/arch.sh"
PREFIX="$MUSL_PREFIX"
CC="${CROSS}gcc $MFLAG"
# shellcheck disable=SC2086 # CC is the compiler and its word-size flag
LIBGCC="$($CC -print-libgcc-file-name)"

# Out of tree, mirroring the source path tests/musl/ (mk/build.mk).
OUT="$ARCH_BUILD/tests/musl"

if [ ! -f "$PREFIX/lib/libc.a" ]; then
    echo "musl-programs: $PREFIX/lib/libc.a not found; run make musl in the top directory first" >&2
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
    # Every link-deps: line counts, since the declaration is repeatable as the
    # host tests' is. A wrapped declaration stops the build, because the
    # `[^*]*` grep stops at the next `*` and would drop the paths past the
    # fold without a message.
    if ! awk '
        /link-deps:/ && prev ~ /link-deps:/ { prev = $0; next }
        prev ~ /link-deps:/ && $0 ~ /^[[:space:]]*\*[[:space:]]+[A-Za-z0-9_.\/-]+\.[ch]([[:space:]]|$)/ { exit 1 }
        { prev = $0 }
    ' "$src"; then
        echo "musl-programs: $src has a wrapped link-deps: line; repeat the" >&2
        echo "musl-programs: link-deps: prefix on the next line instead" >&2
        exit 1
    fi
    # shellcheck disable=SC2013 # a link-deps: line lists its sources as words
    for dep in $(grep -oE 'link-deps:[^*]*' "$src" | sed 's/link-deps://'); do
        if [ ! -f "$REPO/$dep" ]; then
            echo "musl-programs: $src names a link-deps: source that is not there: $dep" >&2
            exit 1
        fi
        deps="$deps $REPO/$dep"
    done
    # With MUSL_PRINT_COMMANDS set, print each program's compile command and
    # compile nothing: the compilation database generator reads them.
    if [ -n "${MUSL_PRINT_COMMANDS:-}" ]; then
        # shellcheck disable=SC2086
        echo "$CC -static -nostdlib -nostartfiles -isystem $PREFIX/include -I ../../utils -O2 -o $OUT/$prog $PREFIX/lib/crt1.o $PREFIX/lib/crti.o $src $deps $PREFIX/lib/libc.a $LIBGCC $PREFIX/lib/crtn.o $ARCH_LDFLAGS"
        continue
    fi
    # shellcheck disable=SC2086
    stale "$OUT/$prog" "$src" $deps "$PREFIX/lib/libc.a" \
        "$SCRIPT_DIR/build.sh" "$REPO/mk/arch.sh" || continue
    echo "  CC $prog"
    # shellcheck disable=SC2086
    $CC -static -nostdlib -nostartfiles -isystem "$PREFIX/include" -I ../../utils -O2 \
        -MMD -MF "$OUT/$prog.d" \
        -o "$OUT/$prog" \
        "$PREFIX/lib/crt1.o" "$PREFIX/lib/crti.o" "$src" $deps \
        "$PREFIX/lib/libc.a" "$LIBGCC" "$PREFIX/lib/crtn.o" \
        -Wl,-z,noexecstack $ARCH_LDFLAGS
done
