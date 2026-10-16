#!/bin/sh
#
# tools/ci/bisect.sh: find the commit that made a ci failure, and say so on
# its issue.
#
#   tools/ci/bisect.sh [-c] [-m MINUTES] [-r RUN]
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
# has the time limit of KIND's run step in ci.yml or full.yml, 170 or 340
# minutes, so that a run too slow for ci is too slow here; timeout(1) stops
# it as it stops ci's. A test passes, fails, or cannot be made: a commit
# that cannot be checked out, that does not build when the failure is a
# suite's, or whose tests/run refuses or stops before its final
# `run: result: failed` is untestable, and an untestable commit ends the
# search with the range unresolved. BAD is tested first, unless -c says that
# an earlier run did so: a failure that does not repeat is labelled
# `intermittent` and the issue stays open. Otherwise the first-parent
# commits between GOOD and BAD are halved until one fails and its parent
# passes, and both are tested once more; the commit is named only when both
# results repeat.
#
# RUN is the ID of the ci run that started the bisection. With -r, a failure
# that does not repeat is labelled `intermittent` only when the newest
# `Run: .../actions/runs/ID` line in the issue's body and comments names
# RUN; when a later run has reported the failure again, the script says so
# on the issue, leaves it unlabelled and exits 0.
#
# With -m the run may take MINUTES. When the time left cannot hold the next
# test, or the two of the last step, at their full limits, the script comments
# with the range left, starts bisect.yml again on that range with -c and the
# same RUN, and exits 0; a run that cannot hold even its first test says the
# range is unresolved. The script checks out every commit it tests, so it
# runs from a copy outside the tree. Each test's output is kept under
# RUNNER_TEMP/bisect, one file for each commit and attempt, for the job to
# upload. GH_TOKEN, GITHUB_REPOSITORY and GITHUB_RUN_ID come from the job.
#
set -u

usage="usage: tools/ci/bisect.sh [-c] [-m MINUTES] [-r RUN] KIND ARCH GOOD BAD ISSUE ITEM..."
confirmed=""
minutes=""
run=""
while getopts cm:r: opt; do
    case $opt in
        c) confirmed=yes ;;
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

say() { # say TEXT: print and post the verdict; return 1 if the comment fails
    echo "ci: bisect: $1"
    gh issue comment "$issue" --repo "$repo" --body "bisect on $ARCH: $1

Logs: \`gh run download $GITHUB_RUN_ID --repo $repo --name bisect-$issue\`" > /dev/null ||
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
        say "unresolved: this run's $minutes minutes cannot hold its tests of $kind, $limit minutes each; narrow the range $(short "$1")..$(short "$2") by hand"
        exit 1
    fi
    if ! gh workflow run bisect.yml --repo "$repo" --ref master -f kind="$kind" \
        -f arch="$ARCH" -f good="$1" -f bad="$2" -f items="$items" -f issue="$issue" -f confirmed=yes \
        -f run="$run"; then
        say "unresolved: the run that would test $(short "$1")..$(short "$2") could not be started; narrow that range by hand"
        exit 1
    fi
    say "$(short "$1") passes and $(short "$2") fails; this run has no time for another test, so a new bisect run continues with that range" || exit 1
    exit 0
}

# Run tests/run with ARGS, its output on the log: 0 when it passes, 1 when it
# ran its suites and failed, 2 when it refused or stopped before its result.
suites() { # suites ARGS...
    limited tests/run "$@" >> "$log.log" 2>&1
    rc=$?
    case $rc/$(grep '^run: result: ' "$log.log" | tail -n 1) in
        0/*) return 0 ;;
        '1/run: result: failed') return 1 ;;
    esac
    return 2
}

# Build COMMIT and run the failing items on it: 0 when it passes, 1 when the
# failure repeats, 2 when the commit is untestable.
tested() { # tested COMMIT
    attempt=$((attempt + 1))
    log="$logs/$attempt-$(short "$1")"
    test_end=$(($(date +%s) + limit * 60))
    git checkout -q --detach "$1" > "$log.log" 2>&1 || { mv "$log.log" "$log.untestable.log"; return 2; }
    echo "ci: bisect: testing $(git log -1 --format='%h %s' "$1")"
    if [ -n "$run_kind" ]; then
        here=$run_kind
        [ "$here" != full ] || grep -q '^ *core|full)' tools/ci/run.sh || here=night
        if limited sh tools/ci/run.sh "$here" "$log.run" >> "$log.log" 2>&1; then
            mv "$log.log" "$log.passed.log"; return 0
        fi
        mv "$log.log" "$log.failed.log"; return 1
    fi
    limited make >> "$log.log" 2>&1
    case $?/$test_set$test_items in
        0/*) ;;
        124/*|137/*) mv "$log.log" "$log.untestable.log"; return 2 ;;
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

verdict=1
if [ -z "$confirmed" ]; then
    fits 1 || handoff "$good" "$bad"
    tested "$bad"; verdict=$?
fi
case $verdict in
    0) if [ -n "$run" ]; then
           newest=$(newest_run) || {
               say "unresolved: $(short "$bad") passed $items when run again, but the issue's runs could not be read, so it is not labelled intermittent"
               exit 1
           }
           if [ "$newest" != "$run" ]; then
               say "$(short "$bad") passed $items when run again, but a newer failure, run $newest, was reported since run $run, so the issue is not labelled intermittent" || exit 1
               exit 0
           fi
       fi
       status=0
       gh issue edit "$issue" --repo "$repo" --add-label intermittent > /dev/null ||
           { echo "ci: bisect: the label on #$issue failed" >&2; status=1; }
       say "$(short "$bad") passed $items when run again; the failure did not repeat, so the issue is intermittent and stays open" || status=1
       exit "$status" ;;
    2) say "unresolved: $(short "$bad") could not be tested again"; exit 1 ;;
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
        *) say "unresolved: $(short "$(nth "$mid")") could not be tested; narrow the range $(short "$good")..$(short "$bad") by hand"
           exit 1 ;;
    esac
done
culprit=$(nth "$hi")
parent=$(nth "$lo")

fits 2 || handoff "$parent" "$culprit"
tested "$culprit"; again=$?
tested "$parent"; before=$?
if [ "$again" = 1 ] && [ "$before" = 0 ]; then
    say "$(git log -1 --format='%h (%s)' "$culprit") fails $items and its parent $(short "$parent") passes, each twice. Fix forward if the fix is clear; otherwise revert $(short "$culprit") and reopen the issue it closed."
else
    say "unresolved: $(short "$culprit") and its parent $(short "$parent") did not repeat their results; narrow the range $(short "$good")..$(short "$bad") before reverting anything"
fi
