#!/usr/bin/env sh

#
# host.sh: build and run the host tests (`tests/run host`).
#
# Each tests/host/*_hosttest.c tests kernel or utility code compiled natively
# on the host and run under AddressSanitizer and UndefinedBehaviorSanitizer.
# The same .c files are linked into the freestanding kernel, so a bounds or
# overflow fault shows here in milliseconds and not as a fault in QEMU. A test
# names the sources it links on a "host-deps:" comment line, and passes when
# it exits 0; the lines it prints as `NAME: not run: WHAT: WHY` are repeated
# under its PASS, since a check it could not run is not a check that passed.
# The suite also runs the checkers' selftests under the sanitizers, the
# scripted checks that host.list names, and the fuzz corpora.
#
# The script writes the selection, one line for each of those checks, and
# etude-host runs them several at once, each with a scratch directory of its
# own as its TMPDIR, and prints a result for every one.
#
# Usage:
#     sh tests/system/suites/host.sh
#

set -u

REPO="$(cd "$(dirname "$0")/../../.." && pwd)"
cd "$REPO" || exit 1
# The binaries and logs are the tree's own, under its build directory,
# which `tests/run`'s lock covers: runs in two trees write no file in common.
VDIR="$REPO/build/tests/host-suite"
SELECTION="$VDIR/selection"
ETUDE_HOST="$REPO/build/tools/etude/etude-host"

CC="${CC:-cc}"
SAN="-fsanitize=address,undefined -fno-sanitize-recover=all"
# The kernel sources are freestanding, and -ffreestanding stops the compiler
# assuming the standard library; the note at the compile says why it matters.
FREESTANDING="-ffreestanding"
# tests/host/compat has stand-ins for the three musl headers a util source
# includes that a native host need not have (<crypt.h>, <shadow.h>,
# <sys/random.h>). Any -I precedes the system directories, so on a host that
# does have them these declarations are the ones used; they are musl's, so the
# interface is the same either way. See tests/host/compat/crypt.h.
INC="-I rach2/sys -I rach2/sys/arch/x86 -I rach2/sys/arch/i686 -I rach2/sys/lib -I rach2/sys/proc -I utils -I krffs -I tests/host/compat"
# A test's bound when it gives none on a `host-seconds:` line. It only ends a
# hung test: one of these tests drives a device that reports an error on
# every transmit, and the first fault it found was an unbounded loop.
WATCHDOG_SECS=60
TAB=$(printf '\t')
tests=0
scratch=""

trap '[ -z "$scratch" ] || rmdir "$scratch" 2>/dev/null' EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM

# wrapped_deps FILE TAG: return 1 when a TAG line of FILE is wrapped, its list
# continued on the next comment line without the tag. The `[^*]*` grep below
# stops at the next `*`, so the paths past the fold would be dropped, and the
# test would fail to link with no reason given or go on passing because
# something else pulled the file in. Each line is checked against the one
# before it, not with `getline`, which consumes the next line and so misses a
# wrap after a second, repeated tag line.
wrapped_deps()
{
    awk -v tag="$2" '
        $0 ~ tag && prev ~ tag { prev = $0; next }
        prev ~ tag && $0 ~ /^[[:space:]]*\*[[:space:]]+[A-Za-z0-9_.\/-]+\.[ch]([[:space:]]|$)/ { exit 1 }
        { prev = $0 }
    ' "$1"
}

# check NAME SECONDS MODE LABEL COMPILE RUN: add a check to the selection.
# MODE is `shared`, or `alone` for a check that runs with no other beside it.
# COMPILE builds the check, and may be empty; RUN runs it, and its exit status
# 0 is a pass. Both are commands of sh(1), run from the root of the tree.
check()
{
    case "$1$2$3$4$5$6" in
        *"$TAB"*)
            echo "hosttest: $1: a check's fields may not hold a tab" >&2
            exit 1 ;;
    esac
    printf '%s\t%s\t%s\t%s\t%s\t%s\n' "$1" "$2" "$3" "$4" "$5" "$6" >>"$SELECTION"
}

# With HOST_PRINT_COMMANDS set, the script prints the compile command of each
# test, checker and fuzz harness and runs nothing: the compilation database
# generator and the coverage report read them.
mkdir -p "$VDIR" || exit 1
if [ -z "${HOST_PRINT_COMMANDS:-}" ]; then
    # Several scripted checks hold the tree, as a script that puts faults into
    # sources must, and run at once they could not each take it.
    # shellcheck source=tests/system/runlock.sh
    . "$REPO/tests/system/runlock.sh"
    runlock_hold "$REPO" hosttest "$REPO/tests/system/suites/host.sh" "$@"
    if [ ! -x "$ETUDE_HOST" ]; then
        echo "hosttest: $ETUDE_HOST is not built; make builds it" >&2
        exit 1
    fi
    : >"$SELECTION" || exit 1
fi

for t in tests/host/*_hosttest.c; do
    [ -f "$t" ] || continue
    tests=$((tests + 1))
    base="$(basename "$t" .c)"
    # Every host-deps: line, not only the first: clang-format wraps a comment
    # at 80 columns, so a test with more dependencies than fit has a second.
    deps="$(grep -oE 'host-deps:[^*]*' "$t" | sed 's/host-deps://' | tr '\n' ' ')"
    # A test over sources outside the kernel tree says here what they need
    # to compile: their own include directories and the warnings their build
    # turns off. Empty for every kernel test, which the fixed flags cover.
    extra="$(grep -oE 'host-cflags:[^*]*' "$t" | sed 's/host-cflags://' | tr '\n' ' ')"
    # A test whose rows take longer than the bound says so in seconds on a
    # `host-seconds:` line, as the file system interruption matrices do.
    secs="$(grep -oE 'host-seconds: *[0-9]+' "$t" | grep -oE '[0-9]+' | head -1)"
    [ -n "$secs" ] || secs=$WATCHDOG_SECS
    # A test with a window of a fraction of a second, which a machine busy
    # with the other checks could miss, runs alone, as a `host-alone:` line
    # says.
    mode=shared
    ! grep -q 'host-alone:' "$t" || mode=alone

    if ! wrapped_deps "$t" "host-deps:"; then
        echo "hosttest: $t: a host-deps: line is wrapped; repeat the" >&2
        echo "hosttest: host-deps: prefix on the next line instead" >&2
        exit 1
    fi

    out="$VDIR/$base"
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
    # loop-idiom pass depends on the whole of -fno-builtin. The cross
    # compiler makes no such transformation, so the kernel is unaffected.
    # -std=c99 is the language; -D_DEFAULT_SOURCE is the library namespace,
    # which glibc narrows to ISO C under a strict -std unless a feature macro
    # says otherwise, hiding popen, strnlen, syscall and SIGWINCH. Darwin
    # ignores the macro and declares everything by default. -D_FORTIFY_SOURCE=0
    # is the one spelling both hosts accept for switching the checked string
    # calls off: Ubuntu's compiler adds level 3 unless the command line sets
    # the macro, and Darwin's headers make snprintf a macro at any level above
    # zero, which a test that supplies its own snprintf cannot compile against.
    cmd="$CC -std=c99 -D_DEFAULT_SOURCE -D_FORTIFY_SOURCE=0 -DRACH2_HOSTTEST $SAN -g -O1 -Wno-empty-body $FREESTANDING $INC $extra -o $out $t $deps"
    if [ -n "${HOST_PRINT_COMMANDS:-}" ]; then
        echo "$cmd"
        continue
    fi
    # Name a missing dep here, since the compiler would report only "no such
    # file". A dep under the musl tree is missing because the tree is not
    # extracted, and the repair removes the stamp, since a copied worktree can
    # have the stamp without the tree and make then takes the tree as current.
    # An in-house dep is a typo or a rename, so the message says only that it
    # is missing. Either way the test stays in the selection, and fails.
    missing=""
    hint=""
    for d in $deps; do
        [ -f "$d" ] && continue
        missing="$missing $d"
        case "$d" in
            vendor/musl/*) hint="; echo 'hosttest: the vendored musl source is not extracted; rm vendor/musl/.stamp and rebuild (make)'" ;;
        esac
    done
    if [ -n "$missing" ]; then
        check "$base" "$WATCHDOG_SECS" "$mode" "$base" "" "echo 'hosttest: $base: missing host-deps:$missing'$hint; exit 1"
    else
        check "$base" "$secs" "$mode" "$base" "$cmd" "$out"
    fi
done

# No tests found is a failure: a broken glob or a moved directory would
# otherwise make the suite pass having run nothing.
if [ "$tests" -eq 0 ]; then
    echo "hosttest: no host tests discovered under tests/host/" >&2
    exit 1
fi

# The programs that decide verdicts carry their own selftests, run by the
# style suite on the binaries the build made. Here each is compiled again
# under the sanitizers and its selftest run, so a bounds or overflow fault in
# a checker is found by the cases the checker already has.
for t in tools/inspect/elfcheck.c tools/inspect/cfgcheck.c tools/inspect/setidcheck.c \
         tools/verify/prosecheck.c tools/etude/win/pecheck.c tests/system/runner/runtests.c \
         tests/system/runner/harness.c; do
    base=$(basename "$t" .c)
    out="$VDIR/${base}_selftest"
    cmd="$CC -std=c99 -D_DEFAULT_SOURCE -D_FORTIFY_SOURCE=0 $SAN -g -O1 -Wall -Wextra -Werror -o $out $t"
    if [ -n "${HOST_PRINT_COMMANDS:-}" ]; then
        echo "$cmd"
        continue
    fi
    check "${base}_selftest" "$WATCHDOG_SECS" shared "$base selftest under the sanitizers" "$cmd" "$out --selftest"
done

# The fuzz harnesses under tests/fuzz, built as replay programs and run over
# their committed corpora: every input a fuzzer found worth keeping, and
# every minimised crash that became a regression test, passes through its
# parser under the sanitizers in seconds. A long run is started by hand, as
# tests/fuzz/Readme.md says; nothing here runs one.
#
# The loop is over the harnesses, not the corpus directories, so a harness
# whose corpus is gone fails the suite.
if [ -n "${HOST_PRINT_COMMANDS:-}" ]; then
    sh tests/fuzz/build.sh replay
    exit
fi
for src in tests/fuzz/*_fuzz.c; do
    [ -f "$src" ] || continue
    name=$(basename "$src" _fuzz.c)
    d=tests/fuzz/corpus/$name/
    inputs=$(find "$d" -type f 2>/dev/null | wc -l | tr -d ' ')
    if [ "$inputs" -eq 0 ]; then
        check "fuzz_$name" "$WATCHDOG_SECS" shared "$name corpus replayed" "" "echo 'hosttest: $name: no corpus for the harness in $d'; exit 1"
    else
        check "fuzz_$name" "$WATCHDOG_SECS" shared "$name corpus replayed ($inputs inputs)" \
            "sh tests/fuzz/build.sh replay $name" "build/tests/fuzz/$name $d*"
    fi
done

# The scripted checks: the proofs' --check modes, the tests of the loops,
# ci and the tools, and TinyCC's comparisons with the GNU tools.
while IFS='|' read -r name secs label cmd; do
    case "$name" in
        ''|'#'*) continue ;;
    esac
    check "$name" "$secs" shared "$label" "" "$cmd"
done <tests/system/suites/host.list

scratch=$(mktemp -d "${TMPDIR:-/tmp}/rach2-host.XXXXXX") || exit 1
"$ETUDE_HOST" ${RACH2_WORKERS:+-j "$RACH2_WORKERS"} -l "$VDIR" -s "$scratch" "$SELECTION"
