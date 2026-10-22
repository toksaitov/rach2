#!/bin/sh
# suitecheck.sh: check that the tests wait for events, assert on counts and
# report what they leave out, and that etude's case manifests are valid.
#
# A test that sleeps for a number of seconds has guessed how long a machine
# takes, and one that compares a duration with a constant has too: both pass
# on the machine that set the number and fail on a slower one, or on the same
# one under load. A test that prints that it skipped a check and then passes
# reports the skip as a pass. The check reads the rules that make the cases'
# artifacts, tests/system/suites.mk, the scripts of the host tests,
# tests/host/*.sh, the probes, tests/*.c, tests/musl/*.c and tests/host/*.c,
# and the programs and scenario code of etude's cases under tests/cases/, and
# fails on these forms:
#
#   * `sleep N` or `usleep N` in a script, on the host or typed into the guest
#   * `wait=N`, a harness step that slept
#   * a `send=` step, or a typed `step "MARKER" "COMMAND"` of drive.sh's
#     suites, whose text is longer than 240 characters: the console's line
#     discipline keeps 255 of a line typed before the shell takes the
#     terminal back, and drops the rest
#   * a call of sleep, usleep, nanosleep or clock_nanosleep in C code, or
#     of the system call of either of the last two by its number
#   * an upper bound on a duration in C code: `ms`, an identifier ending in
#     `_ms`, `_MS` or `secs`, one with `elapsed` in it, or `took`, against a
#     literal of three digits or more: `x < 3000`, `x <= 3000`, `x > 3000`,
#     `3000 > x`, `3000 >= x`. A lower bound, `slept_ms >= 130`, is the
#     contract of a sleep and passes.
#   * a line that prints that a check was skipped or not run, by printf,
#     puts, fputs or echo, with no failure in it
#
# A deliberate interval, a load applied for a period, a holder that must
# outlive the step it holds against, a file time that must advance, a poll
# of a server between tries, has `interval: REASON` in a comment on its line
# or on a line of its own above its command, where it covers the command's
# continuation lines too, and tests/system/intervals lists `FILE: REASON` in
# the same words, FILE relative to the repository, so that every interval is
# written down once and read by a person. A `wait=` step fails even when it
# is marked, since the harness has no such step.
#
# Under tests/cases/ no mark is read. There a sleep or a bound on a duration
# passes only at a line that a case's `timing` declares (tools/etude/Readme.md,
# Resources and timing): a sleep where it declares an `injected_delay`, a
# bound where it declares a `clock_reference`. A declared line with neither
# fails, and so does a line that prints a skip: a case's program reports
# `notrun`, which is never a pass.
#
# The tests that predate the case manifests still have such calls, bounds
# and skips, which their conversions remove. tests/system/unconverted counts
# them, `FILE: COUNT RULE` for the rules `sleep`, `bound` and `skip`, and a
# file fails when its count differs from the entry, so that the list only
# falls.
#
# The check also runs tools/etude/casecheck over the manifests
# tests/cases/*.json and the examples of tools/etude/schemas: their shape and
# rules, the lines their `where` names, and the examples of the results, the
# invocation, the scenario invocation, the reproduce commands, the repetition
# record, the proof catalogue and the history against their schemas and
# cases. Make builds it:
# `make build/tools/etude/casecheck`.
#
#   sh tools/verify/suitecheck.sh             check the tree
#   sh tools/verify/suitecheck.sh --selftest  run the check on fixtures
#
# The exit status is 0 when nothing is found; 1 with each finding on stderr
# as `suitecheck: FILE:LINE: WHAT`; 2 when a file cannot be read.
# SUITECHECK_ROOT, when set, is the tree to check in place of this one.

set -u
SELF=$(cd "$(dirname "$0")/../.." && pwd)
REPO=${SUITECHECK_ROOT:-$SELF}
REASONS="$REPO/tests/system/intervals"
UNCONVERTED="$REPO/tests/system/unconverted"
CASECHECK=${CASECHECK:-$SELF/build/tools/etude/casecheck}
SCHEMAS="$SELF/tools/etude/schemas"

# scan FILE KIND DECLARED: print the findings of one file, KIND `suite` for
# a script or `probe` for C code, on stdout as `FILE:LINE: RULE: WHAT`, RULE
# `sleep`, `bound` or `skip` for a finding tests/system/unconverted may count,
# and `fixed` for one it may not. DECLARED is the output of `casecheck timing`.
scan() {
    awk -v file="${1#"$REPO/"}" -v kind="$2" -v reasons="$REASONS" -v declared="$3" '
    BEGIN {
        while ((getline line < reasons) > 0) {
            known[line] = 1
        }
        close(reasons)
        n = split(declared, entries, "\n")
        for (i = 1; i <= n; i++) {
            split(entries[i], f, " ")
            if (f[1] != "") {
                timing[f[1]] = f[2]
            }
        }
        cases = file ~ /^tests\/cases\//
    }
    function reason_of(text,    r) {
        if (match(text, /interval:[ \t]*[^*]*/)) {
            r = substr(text, RSTART + 9, RLENGTH - 9)
            sub(/^[ \t]+/, "", r); sub(/[ \t]+$/, "", r)
            return r
        }
        return ""
    }
    # The text of a C line without its strings and comments, so that a word
    # in a message or a note is not taken for a call.
    function code_of(text,    t) {
        t = text
        gsub(/"([^"\\]|\\.)*"/, "\"\"", t)
        gsub(/\/\*([^*]|\*+[^*\/])*\*+\//, "", t)
        sub(/\/\*.*$/, "", t)
        if (t ~ /^[ \t]*\*/) {
            t = ""
        }
        return t
    }
    # refuse RULE WHAT KIND: report a form of RULE at this line unless a
    # case declares its line with the timing KIND, or, outside tests/cases/,
    # a listed interval marks it.
    function refuse(rule, what, want,    r, at) {
        at = file ":" NR
        if (at in timing) {
            seen[at] = 1
            if (timing[at] != want) {
                print at ": fixed: " what " where a case declares " timing[at] ", not " want
            }
            return
        }
        if (cases) {
            print at ": fixed: " what " that no case declares in its timing"
            return
        }
        r = reason_of($0)
        if (r == "" && prev_mark != "") {
            r = prev_mark
        }
        if (r == "") {
            print at ": " rule ": " what " without an interval: reason"
        } else if (!((file ": " r) in known)) {
            print at ": fixed: " what " with a reason tests/system/intervals does not list: " r
        }
    }
    {
        if (kind == "suite") {
            if ($0 ~ /wait=[0-9]/) {
                print file ":" NR ": fixed: a wait= step; wait for a token"
            }
            if (match($0, /"send=([^"\\]|\\.)*"/) && RLENGTH - 7 > 240) {
                print file ":" NR ": fixed: a typed line of " RLENGTH - 7 " characters; the console keeps 255"
            }
            if (match($0, /^step "[^"]*" "([^"\\]|\\.)*"/)) {
                typed = substr($0, 1, RLENGTH - 1)
                sub(/^step "[^"]*" "/, "", typed)
                escapes = typed
                typed_length = length(typed) - gsub(/\\/, "", escapes)
                if (typed_length > 240) {
                    print file ":" NR ": fixed: a typed step of " typed_length " characters; the console keeps 255"
                }
            }
            if ($0 ~ /(^|[^A-Za-z0-9_.\/-])u?sleep[ \t]+("?\$|[0-9])/) {
                refuse(file ~ /^tests\/system\// ? "fixed" : "sleep", "a sleep for a number of seconds", "injected_delay")
            }
        } else {
            if (code_of($0) ~ /(^|[^A-Za-z0-9_])(sleep|usleep|nanosleep|clock_nanosleep)[ \t]*\(/ ||
                code_of($0) ~ /(SYS_|__NR_)(CLOCK_|clock_)?(NANOSLEEP|nanosleep)([^A-Za-z0-9_]|$)/) {
                refuse("sleep", "a sleep for a fixed time", "injected_delay")
            }
            if ($0 ~ /(^|[^A-Za-z0-9_])(ms|[A-Za-z0-9_]*_ms|[A-Za-z0-9_]*_MS|[A-Za-z0-9_]*elapsed[A-Za-z0-9_]*|[A-Za-z0-9_]*secs|took)[ \t]*(<|<=|>)[ \t]*[0-9][0-9][0-9]/ ||
                $0 ~ /(^|[^A-Za-z0-9_.])[0-9][0-9][0-9]+[ \t]*(>|>=)[ \t]*(ms|[A-Za-z0-9_]*_ms|[A-Za-z0-9_]*_MS|[A-Za-z0-9_]*elapsed[A-Za-z0-9_]*|[A-Za-z0-9_]*secs|took)([^A-Za-z0-9_]|$)/) {
                refuse(file ~ /^tests\/[^\/]*\.c$/ ? "fixed" : "bound", "an upper bound on a duration", "clock_reference")
            }
        }
        if ($0 ~ /(printf|puts|echo)[ \t(]/ && $0 ~ /"[^"]*([Ss]kipped|[Ss]kipping|SKIP([^A-Za-z]|$)|not run)/ &&
            $0 !~ /FAIL|[Ff]ail|cannot run|did not run/) {
            if (cases) {
                print file ":" NR ": fixed: a skip printed by a case; report it as notrun"
            } else {
                print file ":" NR ": skip: a skip printed in a test that passes"
            }
        }
        # A mark alone on a line covers the next command and its continuations.
        if ($0 ~ /^[ \t]*(#|\/\*)[ \t]*interval:/) {
            prev_mark = reason_of($0)
        } else if ($0 !~ /\\$/) {
            prev_mark = ""
        }
    }
    END {
        for (at in timing) {
            split(at, f, ":")
            if (f[1] == file && !(at in seen)) {
                print at ": fixed: a case declares a timing assumption at a line with no sleep and no bound on a duration"
            }
        }
    }' "$1"
}

# judge FINDINGS: print the findings of the tree on stderr, those of a file
# that tests/system/unconverted counts as a count against its entry; return
# 1 when anything is printed.
judge() {
    printf '%s\n' "$1" | awk -v unconverted="$UNCONVERTED" >&2 '
    BEGIN {
        while ((getline line < unconverted) > 0) {
            if (line ~ /^#/ || line == "") {
                continue
            }
            split(line, f, " ")
            key = substr(f[1], 1, length(f[1]) - 1) " " f[3]
            allowed[key] = f[2]
            entry[key] = line
        }
        close(unconverted)
        bad = 0
    }
    $0 == "" { next }
    {
        split($0, f, ": ")
        split(f[1], at, ":")
        rule = f[2]
        sub(/: [a-z]*: /, ": ")
        if (rule == "fixed") {
            print "suitecheck: " $0
            bad = 1
            next
        }
        key = at[1] " " rule
        count[key]++
        held[key] = held[key] "suitecheck: " $0 "\n"
    }
    END {
        for (key in count) {
            if (count[key] != allowed[key] + 0) {
                split(key, k, " ")
                if (count[key] > allowed[key] + 0) {
                    printf "%s", held[key]
                }
                print "suitecheck: " k[1] ": " count[key] " " k[2] " findings where tests/system/unconverted counts " allowed[key] + 0
                bad = 1
            }
        }
        for (key in allowed) {
            if (!(key in count)) {
                print "suitecheck: tests/system/unconverted: no such findings: " entry[key]
                bad = 1
            }
        }
        exit bad
    }'
}

# validate: run casecheck over the manifests and the examples; print the
# timing it declares on stdout, its findings on stderr, and return its
# status. The manifests and the examples are checked apart, since an
# example may declare a case that a manifest also declares.
validate() {
    examples=
    for f in "$REPO"/tools/etude/schemas/case.*.example.json; do
        [ -e "$f" ] && examples="$examples ${f#"$REPO/"}"
    done
    set --
    for f in "$REPO"/tests/cases/*.json; do
        [ -e "$f" ] && set -- "$@" "${f#"$REPO/"}"
    done
    [ $# -eq 0 ] && [ -z "$examples" ] || [ -x "$CASECHECK" ] ||
        { echo "suitecheck: $CASECHECK: not built; make build/tools/etude/casecheck" >&2; return 2; }
    (
        cd "$REPO" || exit 2
        status=0
        if [ $# -gt 0 ]; then
            "$CASECHECK" -s "$SCHEMAS" -r . timing "$@" || status=$?
        fi
        if [ -n "$examples" ]; then
            # shellcheck disable=SC2086 # the examples' paths have no spaces
            "$CASECHECK" -s "$SCHEMAS" -r . timing $examples || status=$?
        fi
        ex=tools/etude/schemas
        if [ -e "$ex/results.example.jsonl" ]; then
            "$CASECHECK" -s "$SCHEMAS" results "$ex/results.example.jsonl" "$ex/case.guest.example.json" \
                >/dev/null || status=$?
            "$CASECHECK" -s "$SCHEMAS" schema invocation "$ex/invocation.example.json" || status=$?
            "$CASECHECK" -s "$SCHEMAS" schema scenario "$ex/scenario.example.json" || status=$?
            "$CASECHECK" -s "$SCHEMAS" schema reproduce "$ex/reproduce.example.json" || status=$?
            "$CASECHECK" -s "$SCHEMAS" schema repeat "$ex/repeat.example.json" || status=$?
            "$CASECHECK" -s "$SCHEMAS" schema proof "$ex/proof.example.json" || status=$?
            "$CASECHECK" -s "$SCHEMAS" schema budget "$ex/budget.example.json" || status=$?
            "$CASECHECK" -s "$SCHEMAS" schema history "$ex/history.example.jsonl" \
                "$ex/history.incomplete.example.jsonl" || status=$?
        fi
        exit "$status"
    )
}

# Check the tests of the tree, and that every reason the intervals file lists
# is still marked in its file; return 0, 1 or 2 as the script exits.
check_tree() {
    rc=0
    [ -r "$REASONS" ] || { echo "suitecheck: $REASONS: cannot read the reasons file" >&2; return 2; }
    [ -r "$UNCONVERTED" ] || { echo "suitecheck: $UNCONVERTED: cannot read the list of unconverted findings" >&2; return 2; }
    declared=$(validate)
    vs=$?
    [ "$vs" -eq 2 ] && return 2
    [ "$vs" -eq 0 ] || rc=1
    out=
    for f in "$REPO"/tests/system/suites.mk "$REPO"/tests/host/*.sh; do
        case "$f" in */tests/host/\*.sh) continue ;; esac
        [ -r "$f" ] || { echo "suitecheck: $f: cannot read" >&2; return 2; }
        out="$out$(scan "$f" suite "$declared")
"
    done
    for f in "$REPO"/tests/*.c "$REPO"/tests/musl/*.c "$REPO"/tests/host/*.c; do
        case "$f" in */tests/musl/\*.c|*/tests/host/\*.c) continue ;; esac
        [ -r "$f" ] || { echo "suitecheck: $f: cannot read" >&2; return 2; }
        out="$out$(scan "$f" probe "$declared")
"
    done
    if [ -d "$REPO/tests/cases" ]; then
        for f in $(find "$REPO/tests/cases" -type f \( -name '*.sh' -o -name '*.c' \) | sort); do
            case "$f" in *.sh) k=suite ;; *) k=probe ;; esac
            out="$out$(scan "$f" "$k" "$declared")
"
        done
    fi
    judge "$out" || rc=1
    # A listed reason that no line of its file marks is a finding.
    while IFS= read -r entry; do
        case "$entry" in ''|'#'*) continue ;; esac
        f=${entry%%: *}; r=${entry#*: }
        if [ ! -r "$REPO/$f" ] || ! grep -Fq "interval: $r" "$REPO/$f"; then
            echo "suitecheck: tests/system/intervals: no such interval: $entry" >&2; rc=1
        fi
    done < "$REASONS"
    return $rc
}

# Run the check over fixtures: each case is a tree under a temporary root,
# checked by this script, and its exit status compared with the one the rule
# gives. Exits 1 when a case differs.
# shellcheck disable=SC2016
selftest() {
    T=
    trap 'rm -rf ${T:+"$T"}' EXIT
    trap 'exit 129' HUP
    trap 'exit 130' INT
    trap 'exit 143' TERM
    T=$(mktemp -d "${TMPDIR:-/tmp}/rach2-suitecheck.XXXXXX") || exit 2
    n=0; bad=0
    # case_ STATUS NAME: check the tree under $T/NAME and expect STATUS.
    case_() {
        n=$((n + 1))
        SUITECHECK_ROOT="$T/$2" sh "$0" > "$T/$2.out" 2>&1
        got=$?
        if [ "$got" -ne "$1" ]; then
            echo "suitecheck selftest: $2: exit $got, expected $1"; cat "$T/$2.out"; bad=1
        fi
    }
    # tree NAME SUITE PROBE REASONS [UNCONVERTED]: write a tree under $T/NAME
    # with SUITE as its suites.mk, PROBE as its one probe, REASONS as its
    # intervals file and UNCONVERTED as its list of unconverted findings.
    tree() {
        mkdir -p "$T/$1/tests/system"
        printf '%s\n' "$2" > "$T/$1/tests/system/suites.mk"
        printf '%s\n' "$3" > "$T/$1/tests/probe.c"
        printf '%s\n' "$4" > "$T/$1/tests/system/intervals"
        printf '%s\n' "${5-}" > "$T/$1/tests/system/unconverted"
    }
    # put NAME PATH TEXT: add the file PATH, with TEXT, to the tree NAME.
    put() {
        mkdir -p "$(dirname "$T/$1/$2")"
        printf '%s\n' "$3" > "$T/$1/$2"
    }
    # timing_of NAME TIMING: give the tree NAME the host example's manifest as
    # tests/cases/host.sha256.json, with TIMING as its timing.
    timing_of() {
        mkdir -p "$T/$1/tests/cases"
        sed "s|\"timing\": \[\]|\"timing\": [$2]|" "$SELF/tools/etude/schemas/case.host.example.json" \
            > "$T/$1/tests/cases/host.sha256.json"
    }
    tree clean 'wait_serial "$LOG" READY 30' 'if (slept_ms >= 130) { ok(); }' ''
    case_ 0 clean
    tree sleep 'sleep 2' 'int x;' ''
    case_ 1 sleep
    tree sleepguest '"send=sleep 3; echo DONE"' 'int x;' ''
    case_ 1 sleepguest
    tree sleepmarked 'sleep 2 # interval: the load runs for two seconds' 'int x;' 'tests/system/suites.mk: the load runs for two seconds'
    case_ 0 sleepmarked
    tree sleepabove '# interval: the load runs for two seconds
sleep 2' 'int x;' 'tests/system/suites.mk: the load runs for two seconds'
    case_ 0 sleepabove
    tree continued '# interval: the load runs for two seconds
run "$HARNESS" \
    "send=sleep 2; echo DONE" \
    "expect=DONE"' 'int x;' 'tests/system/suites.mk: the load runs for two seconds'
    case_ 0 continued
    tree pastcommand '# interval: the load runs for two seconds
run "$HARNESS" "expect=DONE"
sleep 2' 'int x;' 'tests/system/suites.mk: the load runs for two seconds'
    case_ 1 pastcommand
    tree sleepunlisted 'sleep 2 # interval: the load runs for two seconds' 'int x;' ''
    case_ 1 sleepunlisted
    tree sleepother 'sleep 2 # interval: the load runs for two seconds' 'int x;' 'tests/system/suites.mk: the load runs for a while'
    case_ 1 sleepother
    tree stale 'echo' 'int x;' 'tests/system/suites.mk: the load runs for two seconds'
    case_ 1 stale
    tree waitstep '"expect=lease" wait=3' 'int x;' ''
    case_ 1 waitstep
    tree waitmarked '"expect=lease" wait=3 # interval: settling' 'int x;' 'tests/system/suites.mk: settling'
    case_ 1 waitmarked
    tree usleep 'usleep 200' 'int x;' ''
    case_ 1 usleep
    tree sleepscaled 'sleep "$(scaled 5)"' 'int x;' ''
    case_ 1 sleepscaled
    tree sleepword '# a fixed sleep keeps the host waiting
fail "the lag sleep boot"' 'int x;' ''
    case_ 0 sleepword
    tree longescaped '"send=echo \"'"$(printf 'x%.0s' $(seq 1 236))"'\"; echo DONE"' 'int x;' ''
    case_ 1 longescaped
    tree longline "\"send=$(printf 'x%.0s' $(seq 1 250))\"" 'int x;' ''
    case_ 1 longline
    tree shortline "\"send=$(printf 'x%.0s' $(seq 1 200))\"" 'int x;' ''
    case_ 0 shortline
    tree longstep "step \"LONG\" \"echo $(printf 'x%.0s' $(seq 1 250))\"" 'int x;' ''
    case_ 1 longstep
    tree shortstep "step \"SHORT\" \"echo \\\"$(printf 'x%.0s' $(seq 1 200))\\\"\"" 'int x;' ''
    case_ 0 shortstep
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
    tree csleep 'echo' 'usleep(1000);' ''
    case_ 1 csleep
    tree csleepword 'echo' '/* usleep(1000) waits */ puts("sleep(1)"); nap(1);' ''
    case_ 0 csleepword
    tree csleepmarked 'echo' 'usleep(1000); /* interval: the writer runs for a millisecond */' 'tests/probe.c: the writer runs for a millisecond'
    case_ 0 csleepmarked
    tree csleepcounted 'echo' 'usleep(1000);' '' 'tests/probe.c: 1 sleep'
    case_ 0 csleepcounted
    tree csleepover 'echo' 'usleep(1000);
nanosleep(&ts, NULL);' '' 'tests/probe.c: 1 sleep'
    case_ 1 csleepover
    tree csleepstale 'echo' 'int x;' '' 'tests/probe.c: 1 sleep'
    case_ 1 csleepstale
    tree csleepunder 'echo' 'usleep(1000);' '' 'tests/probe.c: 2 sleep'
    case_ 1 csleepunder
    tree skip 'echo' 'puts("live rows: skipped");' ''
    case_ 1 skip
    tree skipnotrun 'echo "exfat-test: not run: this host has no fsck"' 'int x;' ''
    case_ 1 skipnotrun
    tree skipcounted 'echo' 'puts("live rows: skipped");' '' 'tests/probe.c: 1 skip'
    case_ 0 skipcounted
    tree skipfail 'echo' 'printf("FAIL: the handler did not run\n");' ''
    case_ 0 skipfail
    tree muslsleep 'echo' 'int x;' ''
    put muslsleep tests/musl/a.c 'sleep(1);'
    case_ 1 muslsleep
    tree hostbound 'echo' 'int x;' ''
    put hostbound tests/host/a_hosttest.c 'if (elapsed_ms > 250) { fail(); }'
    case_ 1 hostbound
    tree hostboundcounted 'echo' 'int x;' '' 'tests/host/a_hosttest.c: 1 bound'
    put hostboundcounted tests/host/a_hosttest.c 'if (elapsed_ms > 250) { fail(); }'
    case_ 0 hostboundcounted
    tree probeboundfixed 'echo' 'if (ms < 3000) { fail(); }' '' 'tests/probe.c: 1 bound'
    case_ 1 probeboundfixed
    tree hostscript 'echo' 'int x;' ''
    put hostscript tests/host/a.sh 'sleep 1'
    case_ 1 hostscript
    delay='{"kind": "injected_delay", "where": "tests/cases/host/delay.c:2", "assumes": "the writer is given a millisecond", "uncertainty_us": null}'
    clock='{"kind": "clock_reference", "where": "tests/cases/host/delay.c:2", "assumes": "the clock agrees with the reference", "uncertainty_us": 1000}'
    tree casedeclared 'echo' 'int x;' ''
    put casedeclared tests/cases/host/delay.c 'int x;
usleep(1000);'
    timing_of casedeclared "$delay"
    case_ 0 casedeclared
    tree caseundeclared 'echo' 'int x;' ''
    put caseundeclared tests/cases/host/delay.c 'int x;
usleep(1000); /* interval: the writer runs for a millisecond */'
    timing_of caseundeclared ''
    put caseundeclared tests/system/intervals 'tests/cases/host/delay.c: the writer runs for a millisecond'
    case_ 1 caseundeclared
    tree casekind 'echo' 'int x;' ''
    put casekind tests/cases/host/delay.c 'int x;
usleep(1000);'
    timing_of casekind "$clock"
    case_ 1 casekind
    tree casenoform 'echo' 'int x;' ''
    put casenoform tests/cases/host/delay.c 'int x;
int y;'
    timing_of casenoform "$delay"
    case_ 1 casenoform
    tree casenoline 'echo' 'int x;' ''
    put casenoline tests/cases/host/delay.c 'int x;'
    timing_of casenoline "$delay"
    case_ 1 casenoline
    tree caseskip 'echo' 'int x;' ''
    put caseskip tests/cases/host/delay.c 'puts("the rows: skipped");'
    timing_of caseskip ''
    case_ 1 caseskip
    tree casebad 'echo' 'int x;' ''
    timing_of casebad ''
    sed 's|"source": "program"|"source": "scenario"|' "$T/casebad/tests/cases/host.sha256.json" > "$T/casebad.json"
    mv "$T/casebad.json" "$T/casebad/tests/cases/host.sha256.json"
    case_ 1 casebad
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
