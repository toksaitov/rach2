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
# tests/host/compat holds stand-ins for the three musl headers a util source
# includes that a native host need not have (<crypt.h>, <shadow.h>,
# <sys/random.h>). Any -I precedes the system directories, so on a host that
# does have them these declarations are the ones used; they are musl's, so the
# seam is the same either way. See tests/host/compat/crypt.h.
INC="-I rach2/sys -I rach2/sys/lib -I rach2/sys/proc -I utils -I krffs -I tests/host/compat"
fail=0
any=0

# A test that hangs must fail the gate rather than hang it. One of these tests
# drives a device that reports an error on every transmit, and the first defect
# it caught was an unbounded loop, so this is a demonstrated case and not a
# precaution. The test runs in the FOREGROUND under timeout(1): a background
# job in a non-interactive shell inherits SIGINT and SIGQUIT ignored, and
# pwauth_hosttest checks precisely those dispositions in a child it forks, so
# backgrounding the test makes that test fail for a reason of our own making.
WATCHDOG_SECS=60
WATCHDOG=""
for t0 in timeout gtimeout; do
    if command -v "$t0" >/dev/null 2>&1; then
        WATCHDOG="$t0 $WATCHDOG_SECS"
        break
    fi
done
if [ -z "$WATCHDOG" ]; then
    echo "hosttest: no timeout(1) or gtimeout(1); a hung test will not be cut short" >&2
fi

wrapped_deps()
{
    # A wrapped declaration is silent damage: the `[^*]*` grep below stops at
    # the next `*`, so paths past the fold are dropped and the test either
    # fails to link for no stated reason or keeps passing because something
    # else already pulled the file in. Checked against the PREVIOUS record
    # rather than with `getline`, which consumes the next line and so misses a
    # wrap that follows a second, legitimately repeated declaration.
    awk -v tag="$2" '
        $0 ~ tag && prev ~ tag { prev = $0; next }
        prev ~ tag && $0 ~ /^[[:space:]]*\*[[:space:]]+[A-Za-z0-9_.\/-]+\.[ch]([[:space:]]|$)/ { exit 1 }
        { prev = $0 }
    ' "$1"
}

for t in tests/host/*_hosttest.c; do
    [ -f "$t" ] || continue
    any=1
    base="$(basename "$t" .c)"
    # Every host-deps: line, not just the first. The line is repeatable because
    # clang-format wraps a comment at 80 columns, and a wrapped list would
    # silently lose everything past the fold; a second line is how a test with
    # more dependencies than fit says so.
    deps="$(grep -oE 'host-deps:[^*]*' "$t" | sed 's/host-deps://' | tr '\n' ' ')"

    if ! wrapped_deps "$t" "host-deps:"; then
        echo "hosttest: $t: a host-deps: line is wrapped; repeat the" >&2
        echo "hosttest: host-deps: prefix on the next line instead" >&2
        exit 1
    fi

    out="$VDIR/$base"
    # Name a missing dep here: passed on to the compiler it would surface as a
    # bare "no such file", which reads as a broken test. A dep under the
    # extracted musl tree has one specific cause and one specific repair, and
    # the repair names the stamp rather than `make deps`, because a copied
    # worktree carries a stamp whose tree is gone and make reads that stamp as
    # current. An in-house dep is a typo or a rename, so say only that it is
    # missing.
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
    # -Wno-empty-body tolerates the kernel's `for(...);` idiom in linked
    # sources.
    # shellcheck disable=SC2086
    if $CC -std=c99 $SAN -g -O1 -Wno-empty-body $INC -o "$out" "$t" $deps \
         >"$VDIR/$base.clog" 2>&1; then
        # shellcheck disable=SC2086
        $WATCHDOG "$out" >"$VDIR/$base.rlog" 2>&1
        rc=$?
        if [ "$rc" -eq 0 ]; then
            echo "  PASS: $base"
        elif [ -n "$WATCHDOG" ] && [ "$rc" -eq 124 ]; then
            echo "  FAIL (hung; killed after ${WATCHDOG_SECS}s; see $VDIR/$base.rlog): $base"
            fail=1
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
