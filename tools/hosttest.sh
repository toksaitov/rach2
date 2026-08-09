#!/usr/bin/env sh

#
# hosttest.sh: run the dual-target seam tests (`make verify-host`).
#
# Each tests/host/*_hosttest.c is pure kernel/util logic compiled NATIVELY on
# the host and run under AddressSanitizer + UndefinedBehaviorSanitizer. The same
# .c files are also linked into the freestanding kernel (one source, two
# targets), so a bounds or overflow bug surfaces here in milliseconds instead
# of as a mystery fault in QEMU. A test names the kernel sources it links via a
# "host-deps:" comment, and passes when it exits 0.
#
# Usage:
#     ./hosttest.sh
#

set -u

REPO="$(cd "$(dirname "$0")/.." && pwd)"
cd "$REPO" || exit 1
VDIR=/tmp/rach2_verify
mkdir -p "$VDIR"

CC="${CC:-cc}"
SAN="-fsanitize=address,undefined -fno-sanitize-recover=all"
INC="-I rach2/sys -I rach2/sys/lib -I rach2/sys/proc -I utils"
fail=0
any=0

for t in tests/host/*_hosttest.c; do
    [ -f "$t" ] || continue
    any=1
    base="$(basename "$t" .c)"
    deps="$(grep -oE 'host-deps:[^*]*' "$t" | head -1 | sed 's/host-deps://')"
    out="$VDIR/$base"
    # Name a missing dep here: passed on to the compiler it would surface as a
    # bare "no such file", which reads as a broken test. A dep under the
    # extracted musl tree has one specific cause and one specific repair, and the
    # repair names the stamp rather than `make deps`, because a copied worktree
    # carries a stamp whose tree is gone and make reads that stamp as current. An
    # in-house dep is a typo or a rename, so say only that it is missing.
    missing=""
    missing_musl=""
    for d in $deps; do
        [ -f "$d" ] && continue
        missing="$missing $d"
        case "$d" in
            vendor/musl/*) missing_musl=1 ;;
        esac
    done
    if [ -n "$missing" ]; then
        echo "  FAIL: $base: missing host-deps:$missing"
        if [ -n "$missing_musl" ]; then
            echo "        the vendored musl source is not extracted; rm vendor/musl/.stamp and rebuild (make deps)"
        fi
        fail=1
        continue
    fi
    # -Wno-empty-body tolerates the kernel's `for(...);` idiom in linked sources.
    # shellcheck disable=SC2086
    if $CC -std=c99 $SAN -g -O1 -Wno-empty-body $INC -o "$out" "$t" $deps \
         >"$VDIR/$base.clog" 2>&1; then
        if "$out" >"$VDIR/$base.rlog" 2>&1; then
            echo "  PASS: $base"
        else
            echo "  FAIL (run; see $VDIR/$base.rlog): $base"; fail=1
        fi
    else
        echo "  FAIL (compile; see $VDIR/$base.clog): $base"; fail=1
    fi
done

# Zero discovered tests is a failure, not a quiet pass: the glob breaking or the
# directory moving would otherwise turn verify-host into a silent green.
if [ "$any" -eq 0 ]; then
    echo "hosttest: no host seam tests discovered under tests/host/" >&2
    exit 1
fi
exit "$fail"
