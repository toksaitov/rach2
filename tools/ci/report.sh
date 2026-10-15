#!/bin/sh
#
# tools/ci/report.sh: record a ci run's result on GitHub.
#
#   tools/ci/report.sh core|night SHA DIR
#
# DIR has one directory for each architecture, `KIND-ARCH`, written by
# tools/ci/run.sh. An architecture passed only when its directory has
# `done` and an empty `failed`; a missing or unfinished one is a failure of
# the runner, named `unfinished-core` or `unfinished-night` so that bisection
# repeats that kind of run. For each architecture the commit SHA gets the status
# `ci/KIND/ARCH`, and once everything below has been done, `ci/KIND`, which
# tells a later run that this commit has been tested and reported. A failure
# is filed as an issue with the label `bug`, titled by the run's kind, its
# architecture and what failed, with the range of commits since the last
# commit that passed and the command that fetches the kept logs. An issue
# already open under that title gets a comment instead, and only a new issue
# starts `bisect.yml`. A core failure of the build, style or the runner, or
# of the `default` suite, which boots the test image, opens the issue
# labelled `hold`, and a core run that passes on both architectures closes
# it. A GitHub call that fails is named, the rest are still made, and the
# exit status is 1. GH_TOKEN, GITHUB_REPOSITORY, GITHUB_SERVER_URL and
# GITHUB_RUN_ID come from the job.
#
set -u

kind=${1-}
sha=${2-}
dir=${3-}
case "$kind" in
    core|night) ;;
    *) echo "usage: tools/ci/report.sh core|night SHA DIR" >&2; exit 2 ;;
esac
[ -n "$sha" ] || { echo "usage: tools/ci/report.sh core|night SHA DIR" >&2; exit 2; }
repo=$GITHUB_REPOSITORY
run_url="$GITHUB_SERVER_URL/$repo/actions/runs/$GITHUB_RUN_ID"
short=$(git rev-parse --short "$sha")
hold_title="hold: master does not build, pass style or boot"
rc=0

lost() { # lost WHAT: name a GitHub call that failed
    echo "ci: report: failed: $1" >&2
    rc=1
}

# Set the status CONTEXT of SHA to STATE, linked to this run.
set_status() { # set_status CONTEXT STATE
    gh api "repos/$repo/statuses/$sha" -f state="$2" -f context="$1" \
        -f target_url="$run_url" -f description="$kind at $short" > /dev/null ||
        lost "status $1"
}

# The newest first-parent ancestor of SHA, within fifty, that passed this
# kind of run on ARCH; nothing when none did, and status 1 when a commit's
# status could not be read.
last_pass() { # last_pass ARCH
    for c in $(git rev-list --first-parent --max-count=50 "$sha^" 2> /dev/null); do
        state=$(gh api "repos/$repo/commits/$c/status" \
            --jq ".statuses[] | select(.context == \"ci/$kind/$1\") | .state") ||
            return 1
        [ "$state" = success ] && { echo "$c"; return 0; }
    done
    return 0
}

# The number of the open issue with the label LABEL and the title TITLE, or
# nothing; the status is 1 when the listing could not be read. The listing
# is read rather than searched, since the search index lags.
open_issue() { # open_issue LABEL TITLE
    list=$(gh issue list --repo "$repo" --state open --label "$1" --limit 500 \
        --json number,title --jq ".[] | select(.title == \"$2\") | .number") || return 1
    echo "$list" | head -n 1
}

passed=""
held=""
for arch in i686 x86_64; do
    result="$dir/$kind-$arch"
    if [ -f "$result/done" ] && [ -f "$result/failed" ] && [ ! -s "$result/failed" ]; then
        set_status "ci/$kind/$arch" success
        passed="$passed $arch"
        continue
    fi
    set_status "ci/$kind/$arch" failure
    if [ -f "$result/done" ] && [ -s "$result/failed" ]; then
        items=$(sort -u "$result/failed" | tr '\n' ' ' | sed 's/ $//')
    else
        items=unfinished-$kind
    fi
    case " $items " in
        *" build "*|*" style "*|*" runner "*|*" core "*|*" default "*|*" unfinished-core "*)
            [ "$kind" = core ] && held="$held $arch: $items;" ;;
    esac
    good=$(last_pass "$arch") || { lost "the last pass on $arch"; good=""; }
    if [ -n "$good" ]; then
        range="$(git rev-parse --short "$good")..$short, $(git rev-list --count "$good..$sha") commits:
$(git log --format='* %h %s' "$good..$sha")"
    else
        range="unknown: no commit in the fifty before $short is known to have passed $kind on $arch"
    fi
    title="ci: $kind: $items fails on $arch"
    body="The $kind run on \`master\` at $short failed on $arch: $items.

Run: $run_url
Logs: \`gh run download $GITHUB_RUN_ID --repo $repo --name $kind-$arch\`

Commits since the last pass: $range"
    if [ -f "$result/log" ]; then
        body="$body

The end of the log:

\`\`\`
$(tail -n 40 "$result/log")
\`\`\`"
    fi
    if ! number=$(open_issue bug "$title"); then
        lost "the issue list for $arch"
        continue
    fi
    if [ -n "$number" ]; then
        gh issue comment "$number" --repo "$repo" --body "$body" > /dev/null ||
            lost "the comment on #$number"
        echo "ci: report: $arch: failure added to #$number"
        continue
    fi
    if ! url=$(gh issue create --repo "$repo" --title "$title" --label bug --body "$body"); then
        lost "the issue for $arch"
        continue
    fi
    number=${url##*/}
    echo "ci: report: $arch: filed #$number"
    [ -n "$good" ] || continue
    if gh workflow run bisect.yml --repo "$repo" --ref master \
        -f arch="$arch" -f good="$good" -f bad="$sha" -f items="$items" -f issue="$number"; then
        echo "ci: report: $arch: bisect started for #$number"
    else
        lost "the bisect for #$number"
        gh issue comment "$number" --repo "$repo" --body "ci could not start the bisect; narrow the range by hand." > /dev/null
    fi
done

if [ "$kind" = core ]; then
    if ! hold=$(open_issue hold "$hold_title"); then
        lost "the hold issue's list"
    elif [ -n "$held" ]; then
        body="The core run at $short failed in a way that stops every check:$held

Run: $run_url

While this issue is open, push only a repair toward a passing core run, and name it here."
        if [ -n "$hold" ]; then
            gh issue comment "$hold" --repo "$repo" --body "$body" > /dev/null || lost "the comment on #$hold"
        else
            gh issue create --repo "$repo" --title "$hold_title" --label hold --body "$body" > /dev/null ||
                lost "the hold issue"
        fi
        echo "ci: report: hold opened"
    elif [ "$passed" = " i686 x86_64" ] && [ -n "$hold" ]; then
        gh issue close "$hold" --repo "$repo" --comment "The core run at $short passed on both architectures: $run_url" > /dev/null ||
            lost "the close of #$hold"
        echo "ci: report: hold #$hold closed"
    fi
fi
if [ "$rc" = 0 ]; then
    if [ "$passed" = " i686 x86_64" ]; then
        set_status "ci/$kind" success
    else
        set_status "ci/$kind" failure
    fi
fi
exit "$rc"
