#!/usr/bin/env sh

#
# hosttest.sh -- run the dual-target seam tests (`make verify-host`).
#
# Each tests/host/*_hosttest.c is pure kernel/util logic compiled NATIVELY on
# the host and run under AddressSanitizer + UndefinedBehaviorSanitizer. The same
# .c files are also linked into the freestanding kernel -- one source, two
# targets -- so a bounds or overflow bug surfaces here in milliseconds instead
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

[ "$any" -eq 0 ] && echo "  (no host seam tests yet)"
exit "$fail"
