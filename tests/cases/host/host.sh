#!/bin/sh
#
# host.sh: the program of the host cases, tests/cases/host.host.json, the
# checks of `tests/run host` that compile and run code natively on the host.
#
# Each tests/host/*_hosttest.c tests kernel or utility code compiled natively
# on the host and run under AddressSanitizer and UndefinedBehaviorSanitizer.
# The same .c files are linked into the freestanding kernel, so a bounds or
# overflow fault shows here in milliseconds and not as a fault in QEMU. A test
# names the sources it links on a "host-deps:" comment line, and passes when
# it exits 0; the lines it prints as `NAME: not run: WHAT: WHY` are repeated
# under its PASS, since a check it could not run is not a check that passed.
#
# Each run is one case, named by its argument:
#
#   tests     the host tests, but etude's own
#   etude     etude's host tests, etude_*_hosttest.c, that boot nothing
#   boots     the host tests with a `host-arch:` line, which boot the kernel
#             and test image of ARCH under QEMU
#   checkers  the checkers' selftests, compiled again under the sanitizers
#   fuzz      the fuzz corpora replayed through their harnesses
#   scripts   the scripted checks that host.list, beside this file, names
#
# The program writes the case's selection, one line for each check, and
# etude-host runs them several at once, each with a scratch directory of its
# own as its TMPDIR, and prints a result line for every one. The program then
# writes the case's report records on its standard output
# (tools/etude/Readme.md, Report records): an assertion for each check, which
# passes when etude-host's line for it is a PASS, and `listed`. The manifest
# is the list of a case's checks: an assertion that names a check the tree
# does not have fails, and `listed` fails for a check of the tree that the
# case does not name, which is not run. The program exits 0 when every
# assertion passed and 1 when one failed.
#
# etude runs it in the case's workspace, with ETUDE_TREE naming the root of
# the tree, to which it changes first, ARCH naming the architecture of the
# tests and boots cases, and TMPDIR the workspace, which etude keeps after a
# failure.
# Several scripted checks hold the tree, as a script that puts faults into
# sources must, so the program holds it too, unless an ancestor such as
# tests/run holds it already (tools/etude/hold.sh).
#
# Usage, in the root of the tree, or anywhere with ETUDE_TREE set to it:
#     sh tests/cases/host/host.sh CASE
#     HOST_PRINT_COMMANDS=1 sh tests/cases/host/host.sh
#
# With HOST_PRINT_COMMANDS set, the program takes no case, prints the compile
# command of each host test, checker and fuzz harness, and runs nothing: the
# compilation database generator and the coverage report read them. Otherwise
# the binaries and logs go to a private directory under TMPDIR, whose path
# the first line prints; a run that passes or is stopped by a signal removes
# it, and one that fails keeps it.
#

# shellcheck disable=SC2329 # the cases are called by name, at the end
set -u

CASE=${1-}
if [ -n "${HOST_PRINT_COMMANDS:-}" ]; then
    [ "$#" -eq 0 ] || { echo "host: usage: HOST_PRINT_COMMANDS=1 host.sh" >&2; exit 2; }
else
    [ "$#" -eq 1 ] && [ -n "$CASE" ] || { echo "host: usage: host.sh CASE" >&2; exit 2; }
fi
if [ -n "${ETUDE_TREE-}" ]; then
    cd "$ETUDE_TREE" || exit 2
fi
unset ETUDE_TREE
[ -f tests/cases/host.host.json ] && [ -f tests/cases/host/host.list ] || {
    echo "host: run it in the root of the tree" >&2
    exit 2
}
REPO=$(pwd -P)
MANIFEST="$REPO/tests/cases/host.host.json"
ETUDE_HOST="$REPO/build/tools/etude/etude-host"

CC="${CC:-cc}"
SAN="-fsanitize=address,undefined -fno-sanitize-recover=all"
# The kernel sources are freestanding, and -ffreestanding stops the compiler
# assuming the standard library; the comment of hosttests says why it matters.
FREESTANDING="-ffreestanding"
# tests/host/compat has stand-ins for the three musl headers a util source
# includes that a native host need not have (<crypt.h>, <shadow.h>,
# <sys/random.h>). Any -I precedes the system directories, so on a host that
# does have them these declarations are the ones used; they are musl's, so the
# interface is the same either way. See tests/host/compat/crypt.h.
INC="-I rach2/sys -I rach2/sys/arch/x86 -I rach2/sys/arch/i686 -I rach2/sys/lib -I rach2/sys/proc -I utils -I krffs -I tests/host/compat"
# A test names a source of a vendored port's extracted tree, on a
# `host-deps:` or `host-cflags:` line, by its i686 path under vendor/. The
# tree is read from the architecture ARCH names, which etude gives the tests
# case, i686 when it is unset: x86-64 extracts its own under
# build/x86_64/vendor-work/ (mk/arch.mk), and the source is the same in
# either.
case "${ARCH:-i686}" in
    x86_64) VENDOR_TREE=build/x86_64/vendor-work ;;
    *) VENDOR_TREE=vendor ;;
esac
# A test's bound when it gives none on a `host-seconds:` line. It only ends a
# hung test: one of these tests drives a device that reports an error on
# every transmit, and the first fault it found was an unbounded loop.
WATCHDOG_SECS=60
TAB=$(printf '\t')

if [ -n "${HOST_PRINT_COMMANDS:-}" ]; then
    # The binaries of the printed commands, for a person who runs one.
    W="$REPO/build/tests/host-suite"
    mkdir -p "$W" || exit 1
else
    case "$CASE" in
        tests|etude|boots|checkers|fuzz|scripts) ;;
        *) echo "host: no case $CASE" >&2; exit 2 ;;
    esac
    ID=host.host.$CASE
    if [ -n "${ETUDE_CASE:-}" ] && [ "$ETUDE_CASE" != "$ID" ]; then
        echo "host: ETUDE_CASE is $ETUDE_CASE, and the argument names $ID" >&2
        exit 2
    fi
    # shellcheck source=tools/etude/hold.sh
    . "$REPO/tools/etude/hold.sh"
    etude_hold "$REPO" host "$REPO/tests/cases/host/host.sh" "$@"
    # The exit trap keeps the private directory only after a failure.
    unset W
    status=0
    trap 'status=$?; [ "$status" -ne 0 ] || rm -rf ${W:+"$W"}; exit "$status"' EXIT
    trap 'rm -rf ${W:+"$W"}; exit 129' HUP
    trap 'rm -rf ${W:+"$W"}; exit 130' INT
    trap 'rm -rf ${W:+"$W"}; exit 143' TERM
    if ! W=$(umask 077; mktemp -d "${TMPDIR:-/tmp}/rach2-host.XXXXXX") || ! W=$(cd "$W" && pwd -P) ||
        ! mkdir "$W/scratch"; then
        echo "host: cannot create a temporary directory" >&2
        exit 1
    fi
    printf 'host: logs: %s\n' "$W"
    SELECTION="$W/selection"
    : >"$SELECTION" && : >"$W/ids" && : >"$W/found" || exit 1
    # The checks the manifest gives the case, one identity a line.
    python3 -I - "$MANIFEST" "$ID" >"$W/declared" <<'PY' || { echo "host: $MANIFEST cannot be read" >&2; exit 1; }
import json
import sys
with open(sys.argv[1]) as f:
    family = json.load(f)
for case in family['cases']:
    if case['case_id'] == sys.argv[2]:
        for assertion in case['assertions']:
            if assertion['assertion_id'] != 'listed':
                print(assertion['assertion_id'])
PY
fi

# ---- The report. ----

RECORDS=0
FAILED=""

# Write the report record WORDS, which fits in one write of 255 bytes.
record() {
    printf '@etude 1 %s\n' "$*"
}

# Make TEXT printable ASCII of at most 160 bytes with no space at either
# end, for a record's detail, which a space there would keep from parsing.
clip() {
    _clip=$(printf '%s' "$1" | tr -c ' -~' '?' | sed 's/^ *//' | cut -c1-160 | sed 's/ *$//')
    printf '%s' "${_clip:-?}"
}

# pass ID: report that the assertion ID passed.
pass() {
    record "assert $1 pass"
    RECORDS=$((RECORDS + 1))
}

# fail ID DETAIL: report that the assertion ID failed, with DETAIL.
fail() {
    record "assert $1 fail $(clip "$2")"
    RECORDS=$((RECORDS + 1))
    FAILED="$FAILED $1"
}

# ---- The selection. ----

# check ID NAME SECONDS MODE LABEL COMPILE RUN: add the check NAME, the
# assertion ID, to the selection when the case names it. MODE is `shared`,
# or `alone` for a check that runs with no other beside it. COMPILE builds
# the check, and may be empty; RUN runs it, and its exit status 0 is a pass.
# Both are commands of sh(1), run from the root of the tree. With
# HOST_PRINT_COMMANDS set, print COMPILE instead.
check()
{
    if [ -n "${HOST_PRINT_COMMANDS:-}" ]; then
        [ -z "$6" ] || echo "$6"
        return
    fi
    case "$1$2$3$4$5$6$7" in
        *"$TAB"*)
            echo "host: $2: a check's fields may not hold a tab" >&2
            exit 1 ;;
    esac
    printf '%s\t%s\n' "$1" "$5" >>"$W/found"
    grep -qxF -e "$1" "$W/declared" || return 0
    printf '%s\t%s\t%s\t%s\t%s\t%s\n' "$2" "$3" "$4" "$5" "$6" "$7" >>"$SELECTION"
    printf '%s\t%s\n' "$1" "$5" >>"$W/ids"
}

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

# group_of FILE: print the case a host test belongs to.
group_of()
{
    if grep -q 'host-arch:' "$1"; then
        echo boots
    else
        case "$(basename "$1")" in
            etude_*) echo etude ;;
            *) echo tests ;;
        esac
    fi
}

# hosttests GROUP: add each host test of the case GROUP, or of every case
# when GROUP is empty.
#
# A test's opening comment gives what its compile needs. Every `host-deps:`
# line is read, not only the first, since clang-format wraps a comment at 80
# columns and a test with more dependencies than fit has a second. A
# `host-cflags:` line gives what sources outside the kernel tree need to
# compile, their include directories and the warnings their build turns off;
# the fixed flags cover every kernel test. A `host-seconds:` line gives a
# bound longer than the default, as the file system interruption matrices
# need, and a `host-alone:` line makes a test with a window of a fraction of
# a second, which a machine busy with the other checks could miss, run alone.
#
# -Wno-empty-body tolerates the kernel's `for(...);` idiom in linked sources.
# -ffreestanding stops the compiler recognising the loops in k_memset,
# k_memcpy, and k_memmove as the standard routines they implement. Without
# it, it rewrites k_memset's loop into a call to memset, which
# rach2/sys/lib/kstring.c defines as a wrapper around k_memset: the pair
# becomes an infinite loop. The sanitizers hide that by interposing their own
# memset, so a test compiled the way this program compiles it has never shown
# the fault, and one compiled any other way hangs. The per-function
# -fno-builtin-memset is not enough on clang, whose loop-idiom pass depends on
# the whole of -fno-builtin. The cross compiler makes no such transformation,
# so the kernel is unaffected. -std=c99 is the language; -D_DEFAULT_SOURCE is
# the library namespace, which glibc narrows to ISO C under a strict -std
# unless a feature macro says otherwise, hiding popen, strnlen, syscall and
# SIGWINCH. Darwin ignores the macro and declares everything by default.
# -D_FORTIFY_SOURCE=0 is the one spelling both hosts accept for switching the
# checked string calls off: Ubuntu's compiler adds level 3 unless the command
# line sets the macro, and Darwin's headers make snprintf a macro at any level
# above zero, which a test that supplies its own snprintf cannot compile
# against.
#
# A wrapped dependency list, or a dependency that is missing, fails the test
# with the reason, since the compiler would report only "no such file". A
# dependency under the musl tree is missing because the tree is not
# extracted, and the repair removes the stamp, since a copied worktree can
# have the stamp without the tree and make then takes the tree as current.
# An in-house dependency is a typo or a rename, so the message says only that
# it is missing.
hosttests()
{
    for t in tests/host/*_hosttest.c; do
        [ -f "$t" ] || continue
        [ -z "$1" ] || [ "$(group_of "$t")" = "$1" ] || continue
        base="$(basename "$t" .c)"
        deps="$(grep -oE 'host-deps:[^*]*' "$t" | sed "s/host-deps://; s# vendor/# $VENDOR_TREE/#g" | tr '\n' ' ')"
        extra="$(grep -oE 'host-cflags:[^*]*' "$t" | sed "s/host-cflags://; s# vendor/# $VENDOR_TREE/#g" | tr '\n' ' ')"
        secs="$(grep -oE 'host-seconds: *[0-9]+' "$t" | grep -oE '[0-9]+' | head -1)"
        [ -n "$secs" ] || secs=$WATCHDOG_SECS
        mode=shared
        ! grep -q 'host-alone:' "$t" || mode=alone
        out="$W/$base"
        cmd="$CC -std=c99 -D_DEFAULT_SOURCE -D_FORTIFY_SOURCE=0 -DRACH2_HOSTTEST $SAN -g -O1 -Wno-empty-body $FREESTANDING $INC $extra -o $out $t $deps"
        missing=""
        hint=""
        for d in $deps; do
            [ -f "$d" ] && continue
            missing="$missing $d"
            case "$d" in
                "$VENDOR_TREE"/musl/*) hint="; echo 'host: the vendored musl source is not extracted; rm $VENDOR_TREE/musl/.stamp and rebuild (make)'" ;;
            esac
        done
        if [ -n "${HOST_PRINT_COMMANDS:-}" ]; then
            check "${base%_hosttest}" "$base" "$secs" "$mode" "$base" "$cmd" "$out"
        elif ! wrapped_deps "$t" "host-deps:"; then
            check "${base%_hosttest}" "$base" "$WATCHDOG_SECS" "$mode" "$base" "" \
                "echo 'host: $t: a host-deps: line is wrapped; repeat the host-deps: prefix on the next line instead'; exit 1"
        elif [ -n "$missing" ]; then
            check "${base%_hosttest}" "$base" "$WATCHDOG_SECS" "$mode" "$base" "" \
                "echo 'host: $base: missing host-deps:$missing'$hint; exit 1"
        else
            check "${base%_hosttest}" "$base" "$secs" "$mode" "$base" "$cmd" "$out"
        fi
    done
}

# The programs that decide verdicts carry their own selftests, run by the
# style cases on the binaries the build made. Here each is compiled again
# under the sanitizers and its selftest run, so a bounds or overflow fault in
# a checker is found by the cases the checker already has.
checkers()
{
    for t in tools/inspect/elfcheck.c tools/inspect/cfgcheck.c tools/inspect/setidcheck.c \
             tools/verify/prosecheck.c tools/etude/win/pecheck.c tests/system/runner/runtests.c \
             tests/system/runner/harness.c; do
        base=$(basename "$t" .c)
        out="$W/${base}_selftest"
        cmd="$CC -std=c99 -D_DEFAULT_SOURCE -D_FORTIFY_SOURCE=0 $SAN -g -O1 -Wall -Wextra -Werror -o $out $t"
        check "$base" "${base}_selftest" "$WATCHDOG_SECS" shared "$base selftest under the sanitizers" \
            "$cmd" "$out --selftest"
    done
}

# The fuzz harnesses under tests/fuzz, built as replay programs and run over
# their committed corpora: every input a fuzzer found worth keeping, and
# every minimised crash that became a regression test, passes through its
# parser under the sanitizers in seconds. A long run is started by hand, as
# tests/fuzz/Readme.md says; nothing here runs one. The loop is over the
# harnesses, not the corpus directories, so a harness whose corpus is gone
# fails its assertion.
fuzz()
{
    for src in tests/fuzz/*_fuzz.c; do
        [ -f "$src" ] || continue
        name=$(basename "$src" _fuzz.c)
        d=tests/fuzz/corpus/$name/
        inputs=$(find "$d" -type f 2>/dev/null | wc -l | tr -d ' ')
        if [ "$inputs" -eq 0 ]; then
            check "$name" "fuzz_$name" "$WATCHDOG_SECS" shared "$name corpus replayed" "" \
                "echo 'host: $name: no corpus for the harness in $d'; exit 1"
        else
            check "$name" "fuzz_$name" "$WATCHDOG_SECS" shared "$name corpus replayed ($inputs inputs)" \
                "sh tests/fuzz/build.sh replay $name" "build/tests/fuzz/$name $d*"
        fi
    done
}

# The scripted checks: the proofs' --check modes, the tests of the loops,
# ci and the tools, and TinyCC's comparisons with the GNU tools.
scripts()
{
    while IFS='|' read -r name secs label cmd; do
        case "$name" in
            ''|'#'*) continue ;;
        esac
        check "$name" "$name" "$secs" shared "$label" "" "$cmd"
    done <tests/cases/host/host.list
}

if [ -n "${HOST_PRINT_COMMANDS:-}" ]; then
    hosttests ""
    checkers
    sh tests/fuzz/build.sh replay
    exit
fi

# ---- The run. ----

record "begin $ID"
case "$CASE" in
    tests|etude|boots) hosttests "$CASE" ;;
    *) "$CASE" ;;
esac

# `listed`: the checks of the tree are the checks the case names.
cut -f1 "$W/found" | sort >"$W/found.ids"
sort "$W/declared" >"$W/declared.ids"
unlisted=$(comm -23 "$W/found.ids" "$W/declared.ids" | tr '\n' ' ' | sed 's/ $//')
absent=$(comm -13 "$W/found.ids" "$W/declared.ids" | tr '\n' ' ' | sed 's/ $//')
for u in $unlisted; do
    echo "host: listed: $u is a check of the tree that $ID does not name in $MANIFEST, so it did not run"
done
if [ -z "$unlisted$absent" ]; then
    pass listed
elif [ -z "$absent" ]; then
    fail listed "checks the case does not name: $unlisted"
elif [ -z "$unlisted" ]; then
    fail listed "checks the tree does not have: $absent"
else
    fail listed "checks the case does not name: $unlisted; checks the tree does not have: $absent"
fi

# Each label is a check's name in etude-host's result lines.
dup=$(cut -f2 "$W/ids" | sort | uniq -d | head -1)
[ -z "$dup" ] || { echo "host: two checks have the label $dup" >&2; exit 1; }

noresult="etude-host gave no result for it"
: >"$W/out"
if [ ! -s "$SELECTION" ]; then
    :
elif [ ! -x "$ETUDE_HOST" ]; then
    noresult="$ETUDE_HOST is not built; etude's preparation builds it"
    echo "host: $noresult" >&2
else
    { "$ETUDE_HOST" ${RACH2_WORKERS:+-j "$RACH2_WORKERS"} -l "$W" -s "$W/scratch" "$SELECTION"
      echo $? >"$W/status"; } | tee "$W/out"
    [ "$(cat "$W/status" 2>/dev/null)" != 2 ] || noresult="etude-host refused the selection"
fi

# Each check's result, `ID TAB pass`, `ID TAB fail TAB WHY` or `ID TAB none`,
# from the PASS and FAIL lines etude-host prints with the check's label.
awk -F'\t' '
    NR == FNR { id[$2] = $1; order[++n] = $1; next }
    substr($0, 1, 8) == "  PASS: " {
        l = substr($0, 9)
        if (l in id) res[id[l]] = "pass"
        next
    }
    substr($0, 1, 8) == "  FAIL (" {
        best = ""
        for (l in id) {
            s = "): " l
            if (length($0) > length(s) + 8 && substr($0, length($0) - length(s) + 1) == s &&
                length(l) > length(best))
                best = l
        }
        if (best != "") {
            res[id[best]] = "fail"
            why[id[best]] = substr($0, 9, length($0) - length(best) - 11)
        }
    }
    END {
        for (i = 1; i <= n; i++) {
            k = order[i]
            if (!(k in res)) print k "\tnone"
            else if (res[k] == "pass") print k "\tpass"
            else print k "\tfail\t" why[k]
        }
    }
' "$W/ids" "$W/out" >"$W/results"

while IFS="$TAB" read -r id result why; do
    case "$result" in
        pass) pass "$id" ;;
        fail) fail "$id" "$why" ;;
        *) fail "$id" "$noresult" ;;
    esac
done <"$W/results"
for a in $absent; do
    fail "$a" "the tree has no such check; tests/cases/host.host.json names it"
done

record "end $RECORDS"
if [ -z "$FAILED" ]; then
    echo "host: result: passed: $ID"
    exit 0
fi
echo "host: result: failed: $ID:$FAILED"
exit 1
