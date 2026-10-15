#!/bin/sh
#
# tools/ci/bisect.sh: find the commit that made a ci failure, and say so on
# its issue.
#
#   tools/ci/bisect.sh ARCH GOOD BAD ISSUE ITEM...
#
# GOOD passed ci on ARCH and BAD failed it; ITEM... is what failed, as
# tools/ci/run.sh names it: `build`, `style`, `host`, a suite, or a set's name.
# The report's `unfinished-core` and `unfinished-night` repeat the whole run
# through tools/ci/run.sh, including its build and the night's separate bigmem.
# A commit is tested by building it and running those items, the build alone
# when `build` is among them. tests/run takes a set only by itself, so `core`
# runs after the other items in a run of its own, and `all` runs alone since
# it covers every suite. A test passes, fails, or cannot be made: a commit
# that cannot be checked out, that does not build when the failure is a
# suite's, or whose tests/run refuses or stops before its final
# `run: result: failed` is untestable, and an untestable commit ends the
# search with the range unresolved. BAD is tested first: a failure that does
# not repeat is labelled `intermittent` and the issue stays open. Otherwise
# the first-parent commits between GOOD and BAD are halved until one fails
# and its parent passes, and both are tested once more; the commit is named
# only when both results repeat. Each test's output is kept under
# RUNNER_TEMP/bisect, one file for each commit and attempt, for the job to
# upload. GH_TOKEN, GITHUB_REPOSITORY and GITHUB_RUN_ID come from the job.
#
set -u

[ "$#" -ge 5 ] || { echo "usage: tools/ci/bisect.sh ARCH GOOD BAD ISSUE ITEM..." >&2; exit 2; }
ARCH=$1 good=$2 bad=$3 issue=$4
shift 4
items=$*
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
    *" unfinished-night "*) run_kind=night ;;
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

# Run tests/run with ARGS, its output on the log: 0 when it passes, 1 when it
# ran its suites and failed, 2 when it refused or stopped before its result.
suites() { # suites ARGS...
    tests/run "$@" >> "$log.log" 2>&1
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
    git checkout -q --detach "$1" > "$log.log" 2>&1 || { mv "$log.log" "$log.untestable.log"; return 2; }
    echo "ci: bisect: testing $(git log -1 --format='%h %s' "$1")"
    if [ -n "$run_kind" ]; then
        if sh tools/ci/run.sh "$run_kind" "$log.run" >> "$log.log" 2>&1; then
            mv "$log.log" "$log.passed.log"; return 0
        fi
        mv "$log.log" "$log.failed.log"; return 1
    fi
    if ! make >> "$log.log" 2>&1; then
        if [ -z "$test_set$test_items" ]; then
            mv "$log.log" "$log.failed.log"; return 1
        fi
        mv "$log.log" "$log.untestable.log"; return 2
    fi
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

tested "$bad"
case $? in
    0) status=0
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
# The candidate at index I.
nth() { echo "$candidates" | sed -n "$(($1 + 1))p"; }
while [ $((hi - lo)) -gt 1 ]; do
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
if [ "$lo" -ge 0 ]; then parent=$(nth "$lo"); else parent=$good; fi

tested "$culprit"; again=$?
tested "$parent"; before=$?
if [ "$again" = 1 ] && [ "$before" = 0 ]; then
    say "$(git log -1 --format='%h (%s)' "$culprit") fails $items and its parent $(short "$parent") passes, each twice. Fix forward if the fix is clear; otherwise revert $(short "$culprit") and reopen the issue it closed."
else
    say "unresolved: $(short "$culprit") and its parent $(short "$parent") did not repeat their results; narrow the range $(short "$good")..$(short "$bad") before reverting anything"
fi
