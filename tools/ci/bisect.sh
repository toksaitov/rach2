#!/bin/sh
#
# tools/ci/bisect.sh: find the commit that made a ci failure, and say so on
# its issue.
#
#   tools/ci/bisect.sh [-c] [-e BRIDGE] [-m MINUTES] [-r RUN]
#                      KIND ARCH GOOD BAD ISSUE ITEM...
#
# GOOD passed ci on ARCH and BAD failed it in a run of KIND, `core` or
# `full`, or `night`, which issues filed before full runs name and which is
# taken as `full`. ITEM... is what failed, as tools/ci/run.sh names it:
# `build`, `style`, `host`, a suite, or a set's name. The report's
# `unfinished-core`, `unfinished-full` and `unfinished-night` repeat the
# whole run through the tested commit's tools/ci/run.sh, including its build
# and the full run's separate bigmem; a commit whose run.sh does not take
# `full` runs the same set as `night`. A commit is tested by building it and
# running those items, the build alone when `build` is among them. tests/run
# takes a set only by itself, so `core` runs after the other items in a run
# of its own, and `all` runs alone since it covers every suite. Each test
# has the time limit of one architecture's run in ci.yml or full.yml: 170
# minutes, or 340, half of full.yml's run of both architectures, so that a
# run too slow for ci is too slow here; timeout(1) stops
# it as it stops ci's. A test passes, fails, or cannot be made. tests/run
# fails a commit when it exits 1 with the result `run: result: failed`, its
# last line that begins `run: result: ` apart from the
# `run: result: failed: NAMES` line, which names the suites that failed. The
# result is on standard output and the names on standard error, and a launcher
# that passes the two through pipes of their own may put either last in the log.
# A commit that cannot be checked out, that does not build when the failure is a
# suite's, or whose tests/run refuses, stops before its result, gives another
# result, or passes without a `run: NAME: PASS in` line for each suite asked
# for, a set's as the commit's tests/registry lists them, is untestable, and an
# untestable commit ends the search with the range unresolved. A `run: result:
# failed:` line that names `runner` where no suite of that name was asked for is
# a fault of the runner, and the commit untestable as well. A commit whose
# tools/ci/run.sh has `results=history` reads etude's history: each run of
# tests/run writes it under the test's log through RACH2_RESULTS, and the
# commit's verdict, `etude check verdict` or, before etude was one command,
# `casecheck verdict`, judges it against the commit's tests/cases, as run.sh
# does. The run then passes only when the verdict passes and tests/run exits 0,
# and fails only when an item of a suite asked for failed, the verdict does not
# name `runner`, and tests/run exits with a failure. Anything else is
# untestable: an item that did not finish, a case left out of the plan, a failed
# preparation, a history missing or in doubt, no verdict, or a status that
# disagrees with it. So is an unfinished run of such a commit whose run.sh lists
# `runner` among its failures. BAD is tested first, unless -c says that an
# earlier run did so: a failure that does not repeat is labelled `intermittent`
# and the issue stays open. Otherwise the first-parent commits between GOOD and
# BAD are halved until one fails and its parent passes, and both are tested once
# more; the commit is named only when both results repeat.
#
# Every build and run of a core failure has the environment of a core run:
# each NAME=VALUE line of tools/ci/core.env, which ci.yml's core jobs also
# take, read from the tree the script starts in, master's in the job, before
# it checks out a commit. Without the file the range is unresolved. A full
# run's job sets none of it, and neither does a bisection of one.
#
# RUN is the ID of the ci run that started the bisection. With -r, a failure
# that does not repeat is labelled `intermittent` only when the newest
# `Run: .../actions/runs/ID` line in the issue's body and comments names
# RUN, both before the label is added and after; when a later run has
# reported the failure again, the script says so on the issue, leaves it
# without the label and exits 0.
#
# With -m the run may take MINUTES. When the time left cannot hold the next
# test, or the two of the last step, at their full limits, the script comments
# with the range left, starts bisect.yml again on that range with -c and the
# same RUN, and exits 0; a run that cannot hold even its first test says the
# range is unresolved. The script checks out every commit it tests, so it
# runs from a copy outside the tree. Each test's output is kept under
# RUNNER_TEMP/bisect, one file for each commit and attempt, for the job to
# upload. Before each test the script reads the issue, and once it has been
# closed, since a fix has landed, it stops with status 0 and no comment.
# GH_TOKEN, GITHUB_REPOSITORY and GITHUB_RUN_ID come from the job.
#
# An installed /etc/rach2/etude-site, or ETUDE_SITE, takes precedence over
# the private record below. Every build and run then uses its installed
# launcher, ledger and admitted capacity, with a legacy port block. A
# refused or incomplete launch leaves the commit untestable.
#
# A machine that etude admits tests on has the record `machine` that `etude
# provision machine` keeps in ETUDE_STATE, by default
# `${XDG_STATE_HOME:-$HOME/.local/state}/etude`. There each build and each run
# of a test is a claim in the record's ledger, made by `ETUDE admit`, ETUDE
# being the etude of a version built outside the tree, which -e names;
# without -e the bisection is unresolved. A claim is a quarter of the machine's
# processors, memory and disk, the disk being the file system of the ledger.
# The bridge waits for it and for the old runner's host lock, under
# RACH2_PORT_DIR or /tmp, until the test's limit, and gives a run a port
# block of its own in RACH2_PORT_OFFSET, so that the commit's tests/run
# takes neither lock and cannot wait for the host lock that its own claim
# holds. The run is given RACH2_WORKERS, the suites the claim's processors
# hold as tests/run counts them, in place of core's, and RACH2_MEMORY_MIB,
# three quarters of its memory, or the most that a suite of the test declares
# in the commit's tests/registry when that is more; the claim is four thirds
# of that bound, and a claim of more than half the machine's memory takes the
# host alone, as the bridge's own rule says. A claim that the bridge refuses
# or whose end it cannot confirm, a run of a commit whose
# tests/system/suites.mk does not read RACH2_PORT_OFFSET, and one of a commit
# without tests/registry are untestable. On a machine without the record,
# each command runs as it is, under the commit's own locks.
#
set -u

usage="usage: tools/ci/bisect.sh [-c] [-e ETUDE] [-m MINUTES] [-r RUN] KIND ARCH GOOD BAD ISSUE ITEM..."
confirmed=""
etude=""
minutes=""
run=""
while getopts ce:m:r: opt; do
    case $opt in
        c) confirmed=yes ;;
        e) etude=$OPTARG ;;
        m) minutes=$OPTARG ;;
        r) run=$OPTARG ;;
        *) echo "$usage" >&2; exit 2 ;;
    esac
done
case $run in
    *[!0-9]*) echo "$usage" >&2; exit 2 ;;
esac
shift $((OPTIND - 1))
[ "$#" -ge 6 ] || { echo "$usage" >&2; exit 2; }
kind=$1 ARCH=$2 good=$3 bad=$4 issue=$5
shift 5
items=$*
case $kind in
    core) limit=170 ;;
    full|night) limit=340 ;;
    *) echo "$usage" >&2; exit 2 ;;
esac
case $minutes in
    '') deadline="" ;;
    *[!0-9]*) echo "$usage" >&2; exit 2 ;;
    *) deadline=$(($(date +%s) + minutes * 60)) ;;
esac
export ARCH
repo=$GITHUB_REPOSITORY
logs="$RUNNER_TEMP/bisect"
mkdir -p "$logs"
attempt=0
run_kind=""
test_set=""
test_items=""
case " $items " in
    *" unfinished-core "*) run_kind=core ;;
    *" unfinished-full "*|*" unfinished-night "*) run_kind=full ;;
    *" build "*) ;;
    *" all "*) test_set=all ;;
    *) for item in $items; do
           case $item in
               core) test_set=core ;;
               *) test_items="$test_items $item" ;;
           esac
       done ;;
esac

# The subject of each verdict, and what failed as the verdicts name it: the
# items in backticks, or the whole run for an unfinished one.
on="Bisection on $ARCH"
case $run_kind in
    core) failed="the core tests" ;;
    full) failed="the release tests" ;;
    *) failed=$(echo "$items" | sed "s/^/\`/; s/ /\`, \`/g; s/\$/\`/") ;;
esac

say() { # say TEXT: print and post the verdict; return 1 if the comment fails
    echo "ci: bisect: $1"
    gh issue comment "$issue" --repo "$repo" --body "$1

The logs of each test: \`gh run download $GITHUB_RUN_ID --repo $repo --name bisect-$issue\`" > /dev/null ||
        { echo "ci: bisect: the comment on #$issue failed" >&2; return 1; }
}

# The abbreviated hash of a commit.
short() { git rev-parse --short "$1"; }

# The ID of the run that the last `Run: .../actions/runs/ID` line of ISSUE
# names, its body first and then its comments, oldest first; status 1 when
# the issue cannot be read or names no run.
newest_run() {
    text=$(gh issue view "$issue" --repo "$repo" --json body,comments \
        --jq '.body, .comments[].body') || return 1
    id=$(printf '%s\n' "$text" | tr -d '\r' |
        sed -n 's|^Run: .*/actions/runs/\([0-9][0-9]*\)$|\1|p' | tail -n 1)
    [ -n "$id" ] && echo "$id"
}

# Return when -r gave no run or when its run is still the newest that the
# issue reports. Otherwise say why the issue is not labelled intermittent and
# exit, 0 when a newer run reported the failure and 1 when the runs could not
# be read. With `labelled`, the label has just been added and is removed
# first: report.sh may have read the issue's labels before the add and
# posted its newer run after the first check.
newest_or_exit() { # newest_or_exit [labelled]
    [ -n "$run" ] || return 0
    if newest=$(newest_run); then
        [ "$newest" != "$run" ] || return 0
        why="$on: $(short "$bad") passed $failed when run again, but run $newest has reported the failure since run $run, which started this bisection"
        stop=0
    else
        why="$on is unresolved: $(short "$bad") passed $failed when run again, but the runs that the issue reports could not be read"
        stop=1
    fi
    if [ "${1-}" = labelled ]; then
        gh issue edit "$issue" --repo "$repo" --remove-label intermittent > /dev/null ||
            { echo "ci: bisect: the removal of the label on #$issue failed" >&2; stop=1; }
    fi
    say "$why, so the issue is not labelled intermittent." || stop=1
    exit "$stop"
}

# Run a command under timeout(1) until the end of the current test, which
# tested() sets in test_end, and return its status, 124 or 137 at the limit.
limited() { # limited COMMAND...
    left=$((test_end - $(date +%s)))
    [ "$left" -gt 0 ] || left=1
    timeout -k 60 "${left}s" "$@"
}

# Whether COUNT more tests fit in the time left, each at its full limit and
# the minute that timeout(1) allows it to stop; always, without -m.
fits() { # fits COUNT
    [ -z "$deadline" ] || [ $(($(date +%s) + $1 * (limit + 2) * 60)) -le "$deadline" ]
}

# Leave the range GOOD..BAD, whose BAD has failed twice, to a new run of
# bisect.yml, and exit; refuse when this run has tested nothing, since the
# new run would have no more time than this one.
handoff() { # handoff GOOD BAD
    if [ "$attempt" = 0 ]; then
        say "$on is unresolved: this run has $minutes minutes, too few for one test, which may take $limit minutes. Narrow the range $(short "$1")..$(short "$2") by hand."
        exit 1
    fi
    if ! gh workflow run bisect.yml --repo "$repo" --ref master -f kind="$kind" \
        -f arch="$ARCH" -f good="$1" -f bad="$2" -f items="$items" -f issue="$issue" -f confirmed=yes \
        -f run="$run"; then
        say "$on is unresolved: the run that would test $(short "$1")..$(short "$2") could not be started. Narrow that range by hand."
        exit 1
    fi
    say "$on continues in a new run: $(short "$1") passes and $(short "$2") fails, and this run has no time for another test." || exit 1
    exit 0
}

# Print the suites that tests/run runs for ARGS in the tested commit, one a
# line: a set's suites of ARCH, from the commit's tests/registry, or the
# names themselves; status 1 for a set in a commit without the registry.
members() { # members ARGS...
    for name in "$@"; do
        case $name in
            core|all)
                [ -f tests/registry ] || return 1
                awk -F'|' -v a="$ARCH" -v set="$name" '
                    /^[a-z]/ && (set == "all" || $3 == "core") &&
                        ($6 == "" || index("," $6 ",", "," a ",")) { print $1 }
                ' tests/registry ;;
            *) echo "$name" ;;
        esac
    done
}

# Print the most guest memory, in MiB, that a suite of the current test
# declares in the tested commit's tests/registry, 128 for one that declares
# none, as tests/run reads it; status 1 for a commit without the registry.
largest() {
    # shellcheck disable=SC2086 # a suite name is one word by format
    case $run_kind in
        core) set -- style host core ;;
        full) set -- all ;;
        *) set -- $test_items $test_set ;;
    esac
    names=$(members "$@") && [ -f tests/registry ] || {
        echo "ci: bisect: untestable: the commit has no tests/registry to size its claim" >&2
        return 1
    }
    printf '%s\n' "$names" | awk -F'|' '
        NR == FNR { asked[$0] = 1; next }
        /^[a-z]/ && ($1 in asked) { m = $5 ~ /^[0-9]+$/ ? $5 : 128; if (m > most) most = m }
        END { print most + 0 }
    ' - tests/registry
}

# Run COMMAND, a path, in the tested commit under timeout(1) until the end of
# the current test, and return its status, 124 or 137 at the limit. Where
# the machine admits tests through etude, the command runs through the
# bridge as a claim, a `run` with a port block, as the header says, and the
# status is 125 when the commit cannot be run so, or when the bridge refused
# the claim or could not confirm the command's end.
admitted() { # admitted build|run COMMAND...
    how=$1; shift
    if [ -n "$site_launcher" ]; then
        launch_kind=build
        [ "$how" != run ] || launch_kind=legacy
        rm -f "$log.bridge"
        limited "$site_launcher" -r "$log.bridge" "$launch_kind" "$@"
        rc=$?
        case $rc in 124|137) return "$rc" ;; esac
        case "$(cat "$log.bridge" 2>/dev/null)" in
            "exited $rc") return "$rc" ;;
            *) return 125 ;;
        esac
    fi
    [ -n "$ledger" ] || { limited "$@"; return; }
    step=$((step + 1))
    guest=$((share_memory * 3 / 4 / 1048576))
    ports=
    if [ "$how" = run ]; then
        grep -q RACH2_PORT_OFFSET tests/system/suites.mk 2>/dev/null || {
            echo "ci: bisect: untestable: the commit's suites do not move their ports by RACH2_PORT_OFFSET" >&2
            return 125
        }
        most=$(largest) || return 125
        [ "$most" -le "$guest" ] || guest=$most
        ports=-p
    fi
    RACH2_MEMORY_MIB=$guest; export RACH2_MEMORY_MIB
    left=$((test_end - $(date +%s)))
    [ "$left" -gt 0 ] || left=1
    rm -f "$log.bridge"
    # shellcheck disable=SC2086 # ports is empty or one option
    limited "$etude" admit $ports -d "$locks" -w "$left" -r "$log.bridge" "$ledger" \
        "bisect:$GITHUB_RUN_ID:${GITHUB_RUN_ATTEMPT:-1}:$step" "$account" shared \
        "$share_cpu,$((guest * 1048576 * 4 / 3)),$share_disk" "$capacity" "$left" "$@"
    rc=$?
    case $rc in
        124|137) return "$rc" ;;
    esac
    [ "$(cat "$log.bridge" 2>/dev/null)" = "exited $rc" ] || return 125
    return "$rc"
}

# Whether the tested commit's tools/ci/run.sh reads etude's history.
historic() {
    grep -qx 'results=history' tools/ci/run.sh 2> /dev/null
}

# Native commits retain their public preparation, context and one collection.
# The current commit's native owner supplies the purpose deadline and cleanup.
# Old commits below retain their historical runner and timeout protocol.
native_commit() {
    grep -qx 'results=native' tools/ci/run.sh 2> /dev/null
}

native_tested() {
    native_kind=${run_kind:-full}
    [ "$test_set" != core ] || native_kind=core
    native_purpose=ci
    [ "$kind" = core ] || native_purpose=release
    selected=
    if [ -z "$run_kind" ] && [ "$test_set" != all ] && [ "$test_set" != core ]; then
        # Registry suite names are individual operands.
        # shellcheck disable=SC2086
        for suite in $test_items $test_set; do
            selected="$selected $ARCH:$suite"
        done
    fi
    code=0
    ETUDE_PURPOSE=$native_purpose SUITES=$selected sh tools/ci/run.sh "$native_kind" "$log.run" \
        >> "$log.log" 2>&1 || code=$?
    native_result=$log.run/$native_kind-$ARCH
    if [ ! -f "$native_result/done" ] || [ ! -f "$native_result/native" ] ||
        [ ! -f "$native_result/failed" ]; then
        echo 'ci: bisect: untestable: native collection left no complete result' >> "$log.log"
        return 2
    fi
    if grep -qx runner "$native_result/failed"; then
        echo 'ci: bisect: untestable: native history, cleanup or budget did not validate' >> "$log.log"
        return 2
    fi
    case $code in
        0) [ ! -s "$native_result/failed" ] && return 0 ;;
        1) [ -s "$native_result/failed" ] && return 1 ;;
    esac
    echo 'ci: bisect: untestable: native status disagrees with its verdict' >> "$log.log"
    return 2
}

# Run tests/run with ARGS, its output on the log, and judge it by its status
# and its result: the last line of the run that begins `run: result: `, apart
# from the `run: result: failed: ` lines, which name the suites that failed
# and may come before the result or after it, as the header says. Return 0
# when tests/run passes with a result line for each suite of ARGS; 1 when it
# exits 1 with the result `run: result: failed`, unless a line of failed
# suites names `runner` and ARGS do not ask for that suite; and 2 when it
# refused, stopped before its result, gave another result, left a suite
# without one, or named a fault of the runner. A commit that reads etude's
# history is judged by judged() instead.
suites() { # suites ARGS...
    historic && { judged "$@"; return; }
    before=$(wc -l < "$log.log")
    admitted run tests/run "$@" >> "$log.log" 2>&1
    rc=$?
    lines=$(tail -n +"$((before + 1))" "$log.log" | grep '^run: result: ')
    case $rc/$(printf '%s\n' "$lines" | grep -v '^run: result: failed: ' | tail -n 1) in
        0/*) asked=$(members "$@") || return 2
             for s in $asked; do
                 tail -n +"$((before + 1))" "$log.log" | grep -q "^run: $s: PASS in " || {
                     echo "ci: bisect: untestable: tests/run passed without a result for $s" >> "$log.log"
                     return 2
                 }
             done
             return 0 ;;
        '1/run: result: failed')
            case " $(printf '%s\n' "$lines" | sed -n 's/^run: result: failed: //p' | tr '\n' ' ') " in
                *' runner '*)
                    members "$@" | grep -qx runner || {
                        echo "ci: bisect: untestable: tests/run reports a fault of the runner" >> "$log.log"
                        return 2
                    } ;;
            esac
            return 1 ;;
    esac
    return 2
}

# Run tests/run with ARGS as suites() does, with etude's history of the run
# going to the directory LOG.history.N, the test's Nth run, and judge it with
# the commit's verdict against its tests/cases and the suites of ARGS, `etude
# check verdict` where the commit has etude as one command and `casecheck
# verdict` before: 0 when the verdict passed and tests/run exited 0; 1 when an
# item of a suite of ARGS failed, the verdict does not name `runner` and
# tests/run failed; 2 for anything else, as the header says.
judged() { # judged ARGS...
    runs=$((runs + 1))
    results=$log.history.$runs
    mkdir -p "$results/history" "$results/store"
    RACH2_RESULTS=$results; export RACH2_RESULTS
    admitted run tests/run "$@" >> "$log.log" 2>&1
    rc=$?
    unset RACH2_RESULTS
    why=
    asked=$(members "$@") || why="the commit has no tests/registry to list its suites"
    case $rc in
        124|125|137) why="tests/run did not finish" ;;
    esac
    if [ -z "$why" ]; then
        if [ -f tools/etude/etude.c ]; then
            judge="build/tools/etude/etude check" judge_name="etude check"
        else
            judge=build/tools/etude/casecheck judge_name=casecheck
        fi
        # shellcheck disable=SC2086 # the judge is a program and its verb
        $judge verdict -b "$results/store" -c "$(git rev-parse HEAD)" "$ARCH" bisect \
            "$(echo "$asked" | awk '!seen[$0]++' | tr '\n' , | sed 's/,$//')" "$results/history" \
            > "$results/verdict" 2>&1
        verdict=$?
        cat "$results/verdict" >> "$log.log"
        names=$(sed -n "s/^$judge_name: result: failed: //p" "$results/verdict")
        if [ "$verdict" = 0 ] && grep -qx "$judge_name: result: passed" "$results/verdict"; then
            [ "$rc" != 0 ] || return 0
            why="tests/run exited $rc, and the verdict on its history passed"
        elif [ "$verdict" != 1 ] || [ -z "$names" ]; then
            why="the commit's $judge_name gave no verdict on the history"
        elif [ "$rc" = 0 ]; then
            why="tests/run exited 0, and the verdict on its history failed"
        else
            case " $names " in
                *' runner '*) why="the history cannot show what ran" ;;
                *) sed -n 's/^item .*: failure, in \([^ ]*\)$/\1/p' "$results/verdict" |
                       grep -qxF "$asked" && return 1
                   why="no item of a suite asked for failed, and the run did not pass" ;;
            esac
        fi
    fi
    echo "ci: bisect: untestable: $why" >> "$log.log"
    return 2
}

# Exit 0 when the issue has been closed; a state that cannot be read lets
# the search go on.
open_or_stop() {
    state=$(gh issue view "$issue" --repo "$repo" --json state --jq .state) || return 0
    [ "$state" != CLOSED ] || { echo "ci: bisect: stopped: #$issue was closed"; exit 0; }
}

# Build COMMIT and run the failing items on it: 0 when it passes, 1 when the
# failure repeats, 2 when the commit is untestable.
tested() { # tested COMMIT
    open_or_stop
    attempt=$((attempt + 1))
    log="$logs/$attempt-$(short "$1")"
    runs=0
    test_end=$(($(date +%s) + limit * 60))
    git checkout -q --detach "$1" > "$log.log" 2>&1 || { mv "$log.log" "$log.untestable.log"; return 2; }
    echo "ci: bisect: testing $(git log -1 --format='%h %s' "$1")"
    if native_commit; then
        native_verdict=0
        native_tested || native_verdict=$?
        case $native_verdict in
            0) mv "$log.log" "$log.passed.log" ;;
            1) mv "$log.log" "$log.failed.log" ;;
            *) mv "$log.log" "$log.untestable.log" ;;
        esac
        return "$native_verdict"
    fi
    if [ -n "$run_kind" ]; then
        here=$run_kind
        [ "$here" != full ] || grep -q '^ *core|full)' tools/ci/run.sh || here=night
        admitted run "$(command -v sh)" tools/ci/run.sh "$here" "$log.run" >> "$log.log" 2>&1
        case $? in
            0) mv "$log.log" "$log.passed.log"; return 0 ;;
            125) mv "$log.log" "$log.untestable.log"; return 2 ;;
        esac
        if historic && grep -qx runner "$log.run/failed" 2> /dev/null; then
            echo "ci: bisect: untestable: run.sh failed the runner" >> "$log.log"
            mv "$log.log" "$log.untestable.log"; return 2
        fi
        mv "$log.log" "$log.failed.log"; return 1
    fi
    admitted build "$(command -v make)" >> "$log.log" 2>&1
    case $?/$test_set$test_items in
        0/*) ;;
        124/*|125/*|137/*) mv "$log.log" "$log.untestable.log"; return 2 ;;
        */) mv "$log.log" "$log.failed.log"; return 1 ;;
        *) mv "$log.log" "$log.untestable.log"; return 2 ;;
    esac
    verdict=0
    # shellcheck disable=SC2086 # a suite name is one word by format
    [ -z "$test_items" ] || suites $test_items || verdict=$?
    [ "$verdict" != 0 ] || [ -z "$test_set" ] || suites "$test_set" || verdict=$?
    case $verdict in
        0) mv "$log.log" "$log.passed.log" ;;
        1) mv "$log.log" "$log.failed.log" ;;
        *) mv "$log.log" "$log.untestable.log" ;;
    esac
    return "$verdict"
}

# Whether TEXT is a whole number above 0.
whole() { # whole TEXT
    case $1 in ''|*[!0-9]*) return 1 ;; esac
    [ "$1" -gt 0 ]
}

# Core's environment, as the header says, from the tree that the script
# starts in, before it checks out a commit.
if [ "$kind" = core ]; then
    settings=$(grep -E '^[A-Z_][A-Z0-9_]*=' tools/ci/core.env 2> /dev/null) || {
        say "$on is unresolved: master's tools/ci/core.env gives no environment for a core run."
        exit 1
    }
    while IFS= read -r setting; do
        # shellcheck disable=SC2163 # the setting is NAME=VALUE
        export "$setting"
    done <<EOF
$settings
EOF
    echo "ci: bisect: each test runs in core's environment: $(echo "$settings" | tr '\n' ' ' | sed 's/ $//')"
fi

# The machine's admission, as the header says: the ledger that the record
# names, the machine's processors, memory and disk as the bridge's capacity,
# and the share of each that a test claims.
record=${ETUDE_STATE:-${XDG_STATE_HOME:-${HOME:-}/.local/state}/etude}/machine
ledger=""
site_launcher=""
step=0
site=${ETUDE_SITE:-/etc/rach2/etude-site}
if [ -e "$site" ] || [ -L "$site" ] || [ "${ETUDE_SITE+x}" = x ]; then
    site_launcher=$(sed -n 's/^launcher //p' "$site") || site_launcher=""
    [ -n "$site_launcher" ] && [ -x "$site_launcher" ] || {
        say "$on is unresolved: the installed etude site's launcher is unavailable."
        exit 1
    }
    echo "ci: bisect: each test uses the installed site $site"
elif [ -f "$record" ]; then
    ledger=$(sed -n 's/^ledger //p' "$record")
    cpus=$(sed -n 's/^cpus //p' "$record")
    memory=$(sed -n 's/^memory_bytes //p' "$record")
    kib=$(df -Pk "$ledger" 2>/dev/null | awk 'NR == 2 { print $2 }')
    case $ledger in /*) ;; *) kib="" ;; esac
    if ! whole "$cpus" || ! whole "$memory" || ! whole "$kib"; then
        say "$on is unresolved: the machine's record $record does not give the ledger, processors, memory and disk that a test claims."
        exit 1
    fi
    [ -n "$etude" ] && [ -x "$etude" ] || {
        say "$on is unresolved: this machine admits tests through etude, and the bisection has no etude to run them with."
        exit 1
    }
    capacity=$((cpus * 1000)),$memory,$((kib * 1024))
    share_cpu=$((cpus * 1000 / 4))
    share_memory=$((memory / 4))
    share_disk=$((kib * 1024 / 4))
    locks=${RACH2_PORT_DIR:-/tmp}
    account=$(id -un)
    machine_cpus=${RACH2_CPUS:-2}
    case $machine_cpus in ''|*[!0-9]*|0*) machine_cpus=2 ;; esac
    RACH2_WORKERS=$((cpus / (2 * machine_cpus)))
    [ "$RACH2_WORKERS" -ge 1 ] || RACH2_WORKERS=1
    export RACH2_WORKERS
    echo "ci: bisect: each test claims a quarter of the machine through $ledger"
else
    echo "ci: bisect: $record is missing, so each test runs under its commit's own locks"
fi

verdict=1
if [ -z "$confirmed" ]; then
    fits 1 || handoff "$good" "$bad"
    tested "$bad"; verdict=$?
fi
case $verdict in
    0) newest_or_exit
       status=0
       gh issue edit "$issue" --repo "$repo" --add-label intermittent > /dev/null ||
           { echo "ci: bisect: the label on #$issue failed" >&2; status=1; }
       newest_or_exit labelled
       say "$on: $(short "$bad") passed $failed when run again, so the failure did not repeat. The issue is labelled intermittent and stays open; it loses the label when the failure comes back." || status=1
       exit "$status" ;;
    2) say "$on is unresolved: $(short "$bad") could not be tested again."; exit 1 ;;
esac

# The candidates, oldest first, and the index of the last that passed (-1
# for GOOD) and of the first that failed.
candidates=$(git rev-list --reverse --first-parent "$good..$bad")
n=$(echo "$candidates" | wc -l | tr -d ' ')
lo=-1
hi=$((n - 1))
# The candidate at index I, GOOD at -1.
nth() {
    if [ "$1" -ge 0 ]; then echo "$candidates" | sed -n "$(($1 + 1))p"; else echo "$good"; fi
}
while [ $((hi - lo)) -gt 1 ]; do
    fits 1 || handoff "$(nth "$lo")" "$(nth "$hi")"
    mid=$(((lo + hi) / 2))
    tested "$(nth "$mid")"
    case $? in
        0) lo=$mid ;;
        1) hi=$mid ;;
        *) say "$on is unresolved: $(short "$(nth "$mid")") could not be tested. Narrow the range $(short "$good")..$(short "$bad") by hand."
           exit 1 ;;
    esac
done
culprit=$(nth "$hi")
parent=$(nth "$lo")

fits 2 || handoff "$parent" "$culprit"
tested "$culprit"; again=$?
tested "$parent"; before=$?
if [ "$again" = 1 ] && [ "$before" = 0 ]; then
    say "$on: $(git log -1 --format='%h (%s)' "$culprit") is the first commit that fails $failed; its parent, $(short "$parent"), passes. Each was tested twice. Fix forward if the fix is clear; otherwise revert $(short "$culprit") and reopen the issue it closed."
else
    say "$on is unresolved: $(short "$culprit") and its parent $(short "$parent") did not give the same results when tested again. Narrow the range $(short "$good")..$(short "$bad") before reverting anything."
fi
