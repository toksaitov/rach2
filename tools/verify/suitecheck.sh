#!/bin/sh
# suitecheck.sh: the suites wait for tokens and assert on counts.
#
# A suite that sleeps for a number of seconds has guessed how long a machine
# takes, and a probe that compares a duration with a constant has too: both
# pass on the machine that set the number and fail on a slower one, or on
# the same one under load. This check refuses the forms, in
# tests/system/suites/*.sh, tests/system/suites.mk and the probes tests/*.c:
#
#   * `sleep N` in a suite, on the host or typed into the guest
#   * `wait=N`, a harness step that slept
#   * a `send=` step whose text is longer than 240 characters: the console's
#     line discipline keeps 255 of a line and drops the rest
#   * an upper bound on a duration in a probe: `ms`, an identifier ending
#     in `_ms`, `_MS` or `secs`, one with `elapsed` in it, or `took`, against
#     a literal of three digits or more: `x < 3000`, `x <= 3000`, `x > 3000`,
#     `3000 > x`, `3000 >= x`. A lower bound, `slept_ms >= 130`, is the
#     contract of a sleep and is not refused.
#
# A deliberate interval, a load applied for a period, a holder that must
# outlive the step it holds against, a file time that must advance, a poll
# of a server between tries, carries `interval: REASON` in a comment on its
# line or on the line above its command, which runs to the end of the
# command's continuation lines, and
# tests/system/intervals lists `FILE: REASON` in the same words, FILE
# relative to the repository, so that every interval is written down once
# and read by a person. A `wait=` step has no reason to give: the harness
# step is gone.
#
#   sh tools/verify/suitecheck.sh             check the tree
#   sh tools/verify/suitecheck.sh --selftest  prove the check on fixtures
#
# Exit 0 when nothing is refused, 1 with each refusal on stderr as
# `suitecheck: FILE:LINE: WHAT`, 2 when a file cannot be read.

set -u
REPO=${SUITECHECK_ROOT:-$(cd "$(dirname "$0")/../.." && pwd)}
REASONS="$REPO/tests/system/intervals"

# The refusals of one file, on stdout, as `FILE:LINE: WHAT`. `$2` names
# the kind: `suite` or `probe`.
scan() {
    awk -v file="${1#"$REPO/"}" -v kind="$2" -v reasons="$REASONS" '
    BEGIN {
        while ((getline line < reasons) > 0) {
            known[line] = 1
        }
        close(reasons)
    }
    function reason_of(text,    r) {
        if (match(text, /interval:[ \t]*[^*]*/)) {
            r = substr(text, RSTART + 9, RLENGTH - 9)
            sub(/^[ \t]+/, "", r); sub(/[ \t]+$/, "", r)
            return r
        }
        return ""
    }
    function refuse(what,    r) {
        r = reason_of($0)
        if (r == "" && prev_mark != "") {
            r = prev_mark
        }
        if (r == "") {
            print file ":" NR ": " what " without an interval: reason"
        } else if (!((file ": " r) in known)) {
            print file ":" NR ": " what " with a reason tests/system/intervals does not list: " r
        }
    }
    {
        if (kind == "suite") {
            if ($0 ~ /wait=[0-9]/) {
                print file ":" NR ": a wait= step; wait for a token"
            }
            if (match($0, /"send=[^"]*"/) && RLENGTH - 7 > 240) {
                print file ":" NR ": a typed line of " RLENGTH - 7 " characters; the console keeps 255"
            }
            if ($0 ~ /(^|[^A-Za-z_.\/-])sleep[ \t]+[0-9]/) {
                refuse("a sleep for a number of seconds")
            }
        } else {
            if ($0 ~ /(^|[^A-Za-z0-9_])(ms|[A-Za-z0-9_]*_ms|[A-Za-z0-9_]*_MS|[A-Za-z0-9_]*elapsed[A-Za-z0-9_]*|[A-Za-z0-9_]*secs|took)[ \t]*(<|<=|>)[ \t]*[0-9][0-9][0-9]/ ||
                $0 ~ /(^|[^A-Za-z0-9_.])[0-9][0-9][0-9]+[ \t]*(>|>=)[ \t]*(ms|[A-Za-z0-9_]*_ms|[A-Za-z0-9_]*_MS|[A-Za-z0-9_]*elapsed[A-Za-z0-9_]*|[A-Za-z0-9_]*secs|took)([^A-Za-z0-9_]|$)/) {
                refuse("an upper bound on a duration")
            }
        }
        # A mark on a line of its own covers the command after it, to the
        # end of its continuation lines.
        if ($0 ~ /^[ \t]*(#|\/\*)[ \t]*interval:/) {
            prev_mark = reason_of($0)
        } else if ($0 !~ /\\$/) {
            prev_mark = ""
        }
    }' "$1"
}

check_tree() {
    rc=0
    [ -r "$REASONS" ] || { echo "suitecheck: $REASONS: cannot read the reasons file" >&2; return 2; }
    for f in "$REPO"/tests/system/suites/*.sh "$REPO"/tests/system/suites.mk; do
        [ -r "$f" ] || { echo "suitecheck: $f: cannot read" >&2; return 2; }
        out=$(scan "$f" suite)
        [ -z "$out" ] || { echo "$out" | sed 's/^/suitecheck: /' >&2; rc=1; }
    done
    for f in "$REPO"/tests/*.c; do
        [ -r "$f" ] || { echo "suitecheck: $f: cannot read" >&2; return 2; }
        out=$(scan "$f" probe)
        [ -z "$out" ] || { echo "$out" | sed 's/^/suitecheck: /' >&2; rc=1; }
    done
    # Every listed reason is used: a line deleted leaves its entry behind
    # otherwise, and the list stops being the inventory.
    while IFS= read -r entry; do
        case "$entry" in ''|'#'*) continue ;; esac
        f=${entry%%: *}; r=${entry#*: }
        if [ ! -r "$REPO/$f" ] || ! grep -Fq "interval: $r" "$REPO/$f"; then
            echo "suitecheck: tests/system/intervals: no such interval: $entry" >&2; rc=1
        fi
    done < "$REASONS"
    return $rc
}

# The fixtures: each case is a tree under a temporary root, checked, and
# its exit compared with the one the rule promises.
# shellcheck disable=SC2016
selftest() {
    T=$(mktemp -d "${TMPDIR:-/tmp}/suitecheck.XXXXXX") || exit 2
    trap 'rm -rf "$T"' EXIT INT TERM
    n=0; bad=0
    case_() {
        # $1 expected exit, $2 name; the tree is under $T/$2
        n=$((n + 1))
        SUITECHECK_ROOT="$T/$2" sh "$0" > "$T/$2.out" 2>&1
        got=$?
        if [ "$got" -ne "$1" ]; then
            echo "suitecheck selftest: $2: exit $got, expected $1"; cat "$T/$2.out"; bad=1
        fi
    }
    tree() {
        # $1 name, $2 suite text, $3 probe text, $4 reasons text
        mkdir -p "$T/$1/tests/system/suites"
        printf '%s\n' "$2" > "$T/$1/tests/system/suites/a.sh"
        : > "$T/$1/tests/system/suites.mk"
        printf '%s\n' "$3" > "$T/$1/tests/probe.c"
        printf '%s\n' "$4" > "$T/$1/tests/system/intervals"
    }
    tree clean 'wait_serial "$LOG" READY 30' 'if (slept_ms >= 130) { ok(); }' ''
    case_ 0 clean
    tree sleep 'sleep 2' 'int x;' ''
    case_ 1 sleep
    tree sleepguest '"send=sleep 3; echo DONE"' 'int x;' ''
    case_ 1 sleepguest
    tree sleepmarked 'sleep 2 # interval: the load runs for two seconds' 'int x;' 'tests/system/suites/a.sh: the load runs for two seconds'
    case_ 0 sleepmarked
    tree sleepabove '# interval: the load runs for two seconds
sleep 2' 'int x;' 'tests/system/suites/a.sh: the load runs for two seconds'
    case_ 0 sleepabove
    tree continued '# interval: the load runs for two seconds
run "$HARNESS" \
    "send=sleep 2; echo DONE" \
    "expect=DONE"' 'int x;' 'tests/system/suites/a.sh: the load runs for two seconds'
    case_ 0 continued
    tree pastcommand '# interval: the load runs for two seconds
run "$HARNESS" "expect=DONE"
sleep 2' 'int x;' 'tests/system/suites/a.sh: the load runs for two seconds'
    case_ 1 pastcommand
    tree sleepunlisted 'sleep 2 # interval: the load runs for two seconds' 'int x;' ''
    case_ 1 sleepunlisted
    tree sleepother 'sleep 2 # interval: the load runs for two seconds' 'int x;' 'tests/system/suites/a.sh: the load runs for a while'
    case_ 1 sleepother
    tree stale 'echo' 'int x;' 'tests/system/suites/a.sh: the load runs for two seconds'
    case_ 1 stale
    tree waitstep '"expect=lease" wait=3' 'int x;' ''
    case_ 1 waitstep
    tree waitmarked '"expect=lease" wait=3 # interval: settling' 'int x;' 'tests/system/suites/a.sh: settling'
    case_ 1 waitmarked
    tree usleep 'usleep 200' 'int x;' ''
    case_ 0 usleep
    tree longline "\"send=$(printf 'x%.0s' $(seq 1 250))\"" 'int x;' ''
    case_ 1 longline
    tree shortline "\"send=$(printf 'x%.0s' $(seq 1 200))\"" 'int x;' ''
    case_ 0 shortline
    tree upper 'echo' 'if (ms < 3000) { fail(); }' ''
    case_ 1 upper
    tree upperle 'echo' 'if (took <= 1500) { fail(); }' ''
    case_ 1 upperle
    tree uppergt 'echo' 'if (elapsed_ms > 250) { fail(); }' ''
    case_ 1 uppergt
    tree upperrev 'echo' 'if (3000 > ms) { fail(); }' ''
    case_ 1 upperrev
    tree uppermarked 'echo' 'if (ms > 250) { fail(); } /* interval: the load ran for its period */' 'tests/probe.c: the load ran for its period'
    case_ 0 uppermarked
    tree lower 'echo' 'if (slept_ms >= 130) { ok(); }' ''
    case_ 0 lower
    tree small 'echo' 'if (ms < 0) { fail(); }' ''
    case_ 0 small
    tree items 'echo' 'if (items < 1000) { ok(); }' ''
    case_ 0 items
    tree variable 'echo' 'if (ms >= limit) { fail(); }' ''
    case_ 0 variable
    if [ "$bad" -eq 0 ]; then
        echo "suitecheck selftest: $n cases OK"
    else
        echo "suitecheck selftest: FAIL"
        exit 1
    fi
}

case "${1-}" in
    --selftest) selftest ;;
    '') check_tree ;;
    *) echo "usage: suitecheck.sh [--selftest]" >&2; exit 2 ;;
esac
