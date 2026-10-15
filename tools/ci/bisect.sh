#!/bin/sh
#
# tools/ci/bisect.sh: find the commit that made a ci failure, and say so on
# its issue.
#
#   tools/ci/bisect.sh ARCH GOOD BAD ISSUE ITEM...
#
# GOOD passed ci on ARCH and BAD failed it; ITEM... is what failed, as
# tools/ci/run.sh names it: `build`, `style`, `host`, a suite, or a set's name.
# A commit is tested by building it and running those items, the build alone
# when `build` is among them. A test passes, fails, or cannot be made: a
# commit that cannot be checked out, or that does not build when the failure
# is a suite's, is untestable, and an untestable commit ends the search with
# the range unresolved. BAD is tested first: a failure that does not repeat
# is labelled `intermittent` and the issue stays open. Otherwise the
# first-parent commits between GOOD and BAD are halved until one fails and
# its parent passes, and both are tested once more; the commit is named only
# when both results repeat. Each test's output is kept under
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
case " $items " in
    *" build "*) test_items="" ;;
    *" core "*) test_items=core ;;
    *" all "*) test_items=all ;;
    *) test_items=$items ;;
esac

say() { # say TEXT: comment on the issue and print the text
    echo "ci: bisect: $1"
    gh issue comment "$issue" --repo "$repo" --body "bisect on $ARCH: $1

Logs: \`gh run download $GITHUB_RUN_ID --repo $repo --name bisect-$issue\`" > /dev/null ||
        echo "ci: bisect: the comment on #$issue failed" >&2
}

# The abbreviated hash of a commit.
short() { git rev-parse --short "$1"; }

# Build COMMIT and run the failing items on it: 0 when it passes, 1 when the
# failure repeats, 2 when the commit is untestable.
tested() { # tested COMMIT
    attempt=$((attempt + 1))
    log="$logs/$attempt-$(short "$1")"
    git checkout -q --detach "$1" > "$log.log" 2>&1 || { mv "$log.log" "$log.untestable.log"; return 2; }
    echo "ci: bisect: testing $(git log -1 --format='%h %s' "$1")"
    if ! make >> "$log.log" 2>&1; then
        if [ -z "$test_items" ]; then
            mv "$log.log" "$log.failed.log"; return 1
        fi
        mv "$log.log" "$log.untestable.log"; return 2
    fi
    [ -n "$test_items" ] || { mv "$log.log" "$log.passed.log"; return 0; }
    # shellcheck disable=SC2086 # a suite name is one word by format
    if tests/run $test_items >> "$log.log" 2>&1; then
        mv "$log.log" "$log.passed.log"; return 0
    fi
    mv "$log.log" "$log.failed.log"
    return 1
}

tested "$bad"
case $? in
    0) gh issue edit "$issue" --repo "$repo" --add-label intermittent > /dev/null ||
           echo "ci: bisect: the label on #$issue failed" >&2
       say "$(short "$bad") passed $items when run again; the failure did not repeat, so the issue is intermittent and stays open"
       exit 0 ;;
    2) say "unresolved: $(short "$bad") could not be tested again"; exit 1 ;;
esac

# The candidates, oldest first, and the index of the last that passed (-1
# for GOOD) and of the first that failed.
candidates=$(git rev-list --reverse --first-parent "$good..$bad")
n=$(echo "$candidates" | wc -l | tr -d ' ')
lo=-1
hi=$((n - 1))
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
