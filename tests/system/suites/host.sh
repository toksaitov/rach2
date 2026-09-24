#!/usr/bin/env sh

#
# hosttest.sh: run the dual-target host tests (`tests/run host`).
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

REPO="$(cd "$(dirname "$0")/../../.." && pwd)"
cd "$REPO" || exit 1
VDIR=/tmp/rach2_verify
mkdir -p "$VDIR"

CC="${CC:-cc}"
SAN="-fsanitize=address,undefined -fno-sanitize-recover=all"
# These are freestanding kernel sources, and saying so is what stops the
# compiler assuming the standard library. See the note at the compile.
FREESTANDING="-ffreestanding"
# tests/host/compat has stand-ins for the three musl headers a util source
# includes that a native host need not have (<crypt.h>, <shadow.h>,
# <sys/random.h>). Any -I precedes the system directories, so on a host that
# does have them these declarations are the ones used; they are musl's, so the
# interface is the same either way. See tests/host/compat/crypt.h.
INC="-I rach2/sys -I rach2/sys/lib -I rach2/sys/proc -I utils -I krffs -I tests/host/compat"
fail=0
any=0

# A test that hangs must fail the gate, not hang it. One of these tests
# drives a device that reports an error on every transmit, and the first defect
# it caught was an unbounded loop, so this is a demonstrated case and not a
# precaution. The test runs in the FOREGROUND under the project deadline
# tool: a background job in a non-interactive shell inherits SIGINT and
# SIGQUIT ignored, and pwauth_hosttest checks precisely those dispositions
# in a child it forks, so backgrounding the test makes that test fail for a
# reason of our own making. The tool is built by this suite's make target
# (and by any make) before the tests compile.
WATCHDOG_SECS=60
WATCHDOG=""
if [ -x "$REPO/build/tests/system/deadline" ]; then
    WATCHDOG="$REPO/build/tests/system/deadline $WATCHDOG_SECS"
else
    echo "hosttest: build/tests/system/deadline is not built; a hung test will not be cut short" >&2
fi

wrapped_deps()
{
    # A wrapped declaration is silent damage: the `[^*]*` grep below stops at
    # the next `*`, so paths past the fold are dropped and the test either
    # fails to link for no stated reason or keeps passing because something
    # else already pulled the file in. Checked against the PREVIOUS record,
    # not with `getline`, which consumes the next line and so misses a
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
    # A test over sources outside the kernel tree says here what they need
    # to compile: their own include directories and the warnings their build
    # turns off. Empty for every kernel test, which the fixed flags cover.
    extra="$(grep -oE 'host-cflags:[^*]*' "$t" | sed 's/host-cflags://' | tr '\n' ' ')"
    # A test whose rows take longer than the deadline says so in seconds
    # on a `host-seconds:` line; the two FAT interruption matrices do.
    secs="$(grep -oE 'host-seconds: *[0-9]+' "$t" | grep -oE '[0-9]+' | head -1)"
    [ -n "$secs" ] || secs=$WATCHDOG_SECS
    [ -z "$WATCHDOG" ] || WATCHDOG="$REPO/build/tests/system/deadline $secs"

    if ! wrapped_deps "$t" "host-deps:"; then
        echo "hosttest: $t: a host-deps: line is wrapped; repeat the" >&2
        echo "hosttest: host-deps: prefix on the next line instead" >&2
        exit 1
    fi

    out="$VDIR/$base"
    # With HOST_PRINT_COMMANDS set, print the compile command of each test
    # and compile nothing: the compilation database generator reads them.
    if [ -n "${HOST_PRINT_COMMANDS:-}" ]; then
        # shellcheck disable=SC2086
        echo "$CC -std=c99 -D_DEFAULT_SOURCE -D_FORTIFY_SOURCE=0 -DRACH2_HOSTTEST $SAN -g -O1 -Wno-empty-body $FREESTANDING $INC $extra -o $out $t $deps"
        continue
    fi
    # Name a missing dep here: passed on to the compiler it would surface as a
    # bare "no such file", which reads as a broken test. A dep under the
    # extracted musl tree has one specific cause and one specific repair, and
    # the repair names the stamp, not a make target, because a copied
    # worktree has a stamp whose tree is gone and make reads that stamp as
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
            echo "        the vendored musl source is not extracted; rm vendor/musl/.stamp and rebuild (make)"
        fi
        fail=1
        continue
    fi
    # -Wno-empty-body tolerates the kernel's `for(...);` idiom in linked
    # sources.
    #
    # -ffreestanding stops the compiler recognising the loops in k_memset,
    # k_memcpy, and k_memmove as the standard routines they implement.
    # Without it, it rewrites k_memset's loop into a call to memset, which
    # rach2/sys/lib/kstring.c defines as a wrapper around k_memset: the pair
    # becomes an infinite loop. The sanitizers below hide that by interposing
    # their own memset, so a test compiled the way this runner compiles it has
    # never shown the fault, and one compiled any other way hangs. The
    # per-function -fno-builtin-memset is not enough on clang, whose
    # loop-idiom pass is gated on the whole of -fno-builtin. The cross
    # compiler makes no such transformation, so the kernel is unaffected.
    # -std=c99 is the language; -D_DEFAULT_SOURCE is the library namespace,
    # which glibc narrows to ISO C under a strict -std unless a feature macro
    # says otherwise, hiding popen, strnlen, syscall and SIGWINCH. Darwin
    # ignores the macro and declares everything by default. -D_FORTIFY_SOURCE=0
    # is the one spelling both hosts accept for switching the checked string
    # calls off: Ubuntu's compiler adds level 3 unless the command line sets
    # the macro, and Darwin's headers make snprintf a macro at any level above
    # zero, which a test that supplies its own snprintf cannot compile against.
    # shellcheck disable=SC2086
    if $CC -std=c99 -D_DEFAULT_SOURCE -D_FORTIFY_SOURCE=0 -DRACH2_HOSTTEST $SAN -g -O1 -Wno-empty-body $FREESTANDING $INC $extra -o "$out" "$t" $deps \
         >"$VDIR/$base.clog" 2>&1; then
        # shellcheck disable=SC2086
        $WATCHDOG "$out" >"$VDIR/$base.rlog" 2>&1
        rc=$?
        if [ "$rc" -eq 0 ]; then
            echo "  PASS: $base"
        elif [ -n "$WATCHDOG" ] && [ "$rc" -eq 124 ]; then
            echo "  FAIL (hung; killed after ${secs}s; see $VDIR/$base.rlog): $base"
            fail=1
        else
            echo "  FAIL (run; see $VDIR/$base.rlog): $base"; fail=1
        fi
    else
        echo "  FAIL (compile; see $VDIR/$base.clog): $base"; fail=1
    fi
done

# The programs that decide verdicts carry their own selftests, run by the
# style suite on the binaries the build made. Here each is compiled again
# under the sanitizers and its selftest run, so a bounds or overflow fault in
# a checker is found by the cases the checker already has.
for t in tools/inspect/elfcheck.c tools/inspect/cfgcheck.c tools/inspect/setidcheck.c \
         tools/verify/prosecheck.c tests/system/runner/runtests.c tests/system/runner/harness.c; do
    base=$(basename "$t" .c)
    out="$VDIR/${base}_selftest"
    if [ -n "${HOST_PRINT_COMMANDS:-}" ]; then
        echo "$CC -std=c99 -D_DEFAULT_SOURCE -D_FORTIFY_SOURCE=0 $SAN -g -O1 -Wall -Wextra -Werror -o $out $t"
        continue
    fi
    any=1
    # shellcheck disable=SC2086
    if $CC -std=c99 -D_DEFAULT_SOURCE -D_FORTIFY_SOURCE=0 $SAN -g -O1 -Wall -Wextra -Werror -o "$out" "$t" \
         >"$VDIR/$base.clog" 2>&1; then
        # shellcheck disable=SC2086
        $WATCHDOG "$out" --selftest >"$VDIR/$base.rlog" 2>&1
        rc=$?
        if [ "$rc" -eq 0 ]; then
            echo "  PASS: $base selftest under the sanitizers"
        else
            echo "  FAIL (selftest; see $VDIR/$base.rlog): $base"; fail=1
        fi
    else
        echo "  FAIL (compile; see $VDIR/$base.clog): $base"; fail=1
    fi
done

if [ -z "${HOST_PRINT_COMMANDS:-}" ]; then
    if sh tests/host/krffs_open_mutate.sh --check >"$VDIR/krffs_open.rlog" 2>&1; then
        echo "hosttest: result: pass: krffs publication open"
    else
        cat "$VDIR/krffs_open.rlog"
        echo "hosttest: result: fail: krffs publication open" >&2
        fail=1
    fi
    if sh tests/host/krffs_root_lock_mutate.sh --check >"$VDIR/krffs_root_lock.rlog" 2>&1; then
        echo "hosttest: result: pass: krffs root data operations under the mount's lock"
    else
        cat "$VDIR/krffs_root_lock.rlog"
        echo "hosttest: result: fail: krffs root data operations under the mount's lock" >&2
        fail=1
    fi
    # Two rows over functions extracted from sources no host test can
    # compile whole: a new kernel stack cleared (syscall_proc.c) and the
    # set-id rules of a mounted chown (syscall_fs.c).
    if sh tests/host/kstack_mutate.sh --check >"$VDIR/kstack.rlog" 2>&1; then
        echo "hosttest: result: pass: a new kernel stack is cleared"
    else
        cat "$VDIR/kstack.rlog"
        echo "hosttest: result: fail: a new kernel stack is cleared" >&2
        fail=1
    fi
    if sh tests/host/chown_mutate.sh --check >"$VDIR/chown.rlog" 2>&1; then
        echo "hosttest: result: pass: the set-id rules of a mounted chown"
    else
        cat "$VDIR/chown.rlog"
        echo "hosttest: result: fail: the set-id rules of a mounted chown" >&2
        fail=1
    fi
fi

# The fuzz harnesses under tests/fuzz, built as replay programs and run over
# their committed corpora: every input a fuzzer found worth keeping, and
# every minimised crash that became a regression test, passes through its
# parser under the sanitizers in seconds. A long run is started by hand, as
# tests/fuzz/Readme.md says; nothing here runs one.
#
# The loop is over the harnesses and not over the corpus directories, so a
# harness whose corpus is gone is a failure and not a silence: a parser with
# no inputs is a parser this suite stopped covering.
if [ -n "${HOST_PRINT_COMMANDS:-}" ]; then
    sh tests/fuzz/build.sh replay
else
    any=1
    if sh tests/fuzz/build.sh replay >"$VDIR/fuzz.clog" 2>&1; then
        for src in tests/fuzz/*_fuzz.c; do
            name=$(basename "$src" _fuzz.c)
            d=tests/fuzz/corpus/$name/
            if [ ! -x "build/tests/fuzz/$name" ]; then
                echo "  FAIL (the harness did not build): $name"; fail=1
                continue
            fi
            if [ -z "$(find "$d" -type f 2>/dev/null)" ]; then
                echo "  FAIL (no corpus for the harness): $name"; fail=1
                continue
            fi
            # shellcheck disable=SC2086
            if $WATCHDOG "build/tests/fuzz/$name" "$d"* >"$VDIR/fuzz_$name.rlog" 2>&1; then
                echo "  PASS: $name corpus replayed ($(find "$d" -type f | wc -l | tr -d ' ') inputs)"
            else
                echo "  FAIL (replay; see $VDIR/fuzz_$name.rlog): $name"; fail=1
            fi
        done
    else
        echo "  FAIL (the fuzz harnesses did not build; see $VDIR/fuzz.clog)"; fail=1
    fi
fi

# Zero discovered tests is a failure, not a quiet pass: the glob breaking or the
# directory moving would otherwise turn verify-host into a silent green.
if [ "$any" -eq 0 ]; then
    echo "hosttest: no host tests discovered under tests/host/" >&2
    exit 1
fi
exit "$fail"
