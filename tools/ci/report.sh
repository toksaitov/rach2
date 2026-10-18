#!/bin/sh
#
# tools/ci/report.sh: record a ci run's result on GitHub.
#
#   tools/ci/report.sh core|full SHA DIR
#
# DIR has one directory for each architecture, `KIND-ARCH`, written by
# tools/ci/run.sh. An architecture passed only when its directory has
# `done` and an empty `failed`; a missing or unfinished one is a failure of
# the runner, named `unfinished-core` or `unfinished-full` so that bisection
# repeats that kind of run. An architecture whose directory lacks `done`,
# as when the upload of a core run's result was lost, takes its result from
# the variable VERDICT_ARCH, which ci.yml sets from the core job's outputs,
# when that begins with the word `done`; the words after it are the lines of
# `failed`. For each architecture the commit SHA gets the status
# `ci/KIND/ARCH`, and once everything below has been done, `ci/KIND`, which
# tells a later run that this commit has been tested and reported. A full run
# of the suites that SUITES names, as `ARCH:SUITE` words, sets `ci/rerun/ARCH`
# and `ci/rerun` instead, since it did not test every suite, and an
# architecture whose directory has `skipped`, where it had nothing to run,
# gets no status and counts as passed.
#
# A failure is filed as an issue with the labels `bug` and `ci`, titled
# `Core tests fail on ARCH: ITEMS` or `Release tests fail on ARCH: ITEMS`,
# ITEMS being what failed, sorted and joined by commas, or `Core tests did
# not finish on ARCH` or `Release tests did not finish on ARCH` for an
# unfinished run. The issue says in a sentence what failed, where and at
# which commit, and gives the first lines of the log that name a failure, the
# command that fetches the kept logs, the run, its attempt, the commit, the
# end of the log, and last the commits since the last pass: the newest
# first-parent ancestor of SHA, within 50 for core and 500 for full, whose
# status `ci/core/ARCH` is success, or for full `ci/full/ARCH` or
# `ci/night/ARCH`, which the nightly runs before full runs set. An issue
# already open under that title gets a comment instead; when it has the label
# `intermittent`, the label is removed. The labels are read again after the
# comment, since a bisection that ends meanwhile may add the label after the
# first read. A new issue, and an open one that was intermittent, starts
# `bisect.yml` when the last pass is known, and gives it this run's ID, so
# that bisection does not call the failure intermittent once a later run has
# reported it again.
#
# A core job that GitHub gave to no runner, so that none of its steps ran,
# is not a failure of the commit: its architecture gets the status `error`,
# as does `ci/lost/ARCH`, nothing is filed, `ci/core` is left unset, and
# ci.yml is started again on the newest master. Nothing sets `ci/lost/ARCH`
# again, so a commit that has it, from a run whose job on ARCH was lost
# before, keeps it whatever later runs report, and its next lost job on ARCH
# is reported as unfinished. A runner that keeps losing jobs is thus filed
# and does not start run after run. When that status cannot be read or set,
# the lost job is reported as unfinished at once.
#
# A core failure of the build, style or the runner, or of the `default`
# suite, which boots the test image, opens the issue labelled `hold`, or
# comments on the oldest one open, and a core run that passes on both
# architectures closes every open one, whoever opened it and whatever its
# title. A GitHub call that fails is named, the rest are still made, and the
# exit status is 1. Each architecture's result, with the first lines that
# name a failure, the issue and the command that fetches the logs, also goes
# to the job's summary, GITHUB_STEP_SUMMARY, which GitHub shows on the run's
# page. GH_TOKEN, GITHUB_REPOSITORY, GITHUB_SERVER_URL, GITHUB_RUN_ID and
# GITHUB_RUN_ATTEMPT come from the job.
#
set -u

kind=${1-}
sha=${2-}
dir=${3-}
case "$kind" in
    core) depth=50 contexts=ci/core what="Core tests" ;;
    full) depth=500 contexts="ci/full ci/night" what="Release tests" ;;
    *) echo "usage: tools/ci/report.sh core|full SHA DIR" >&2; exit 2 ;;
esac
[ -n "$sha" ] || { echo "usage: tools/ci/report.sh core|full SHA DIR" >&2; exit 2; }
ctx=ci/$kind
[ "$kind" != full ] || [ -z "${SUITES:-}" ] || ctx=ci/rerun
repo=$GITHUB_REPOSITORY
run_url="$GITHUB_SERVER_URL/$repo/actions/runs/$GITHUB_RUN_ID"
attempt=${GITHUB_RUN_ATTEMPT:-1}
short=$(git rev-parse --short "$sha")
hold_title="Pushes are held: master does not build, pass style or boot"
statuses=
trap 'rm -f ${statuses:+"$statuses"}' EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM
statuses=$(mktemp "${TMPDIR:-/tmp}/rach2-report.XXXXXX") || exit 1
rc=0

# Append each LINE to the job's summary.
summarize() { # summarize LINE...
    [ -z "${GITHUB_STEP_SUMMARY:-}" ] || printf '%s\n' "$@" >> "$GITHUB_STEP_SUMMARY"
}

lost() { # lost WHAT: name a GitHub call that failed
    echo "ci: report: failed: $1" >&2
    rc=1
}

# Set the status CONTEXT of SHA to STATE, linked to this run; the status is
# 1 when GitHub refused it.
set_status() { # set_status CONTEXT STATE
    gh api "repos/$repo/statuses/$sha" -f state="$2" -f context="$1" \
        -f target_url="$run_url" -f description="$what of $short" > /dev/null ||
        { lost "status $1"; return 1; }
}

# The newest first-parent ancestor of SHA, within DEPTH, with a success
# status of one of CONTEXTS on ARCH; nothing when none has one, and status 1
# when a commit's statuses could not be read. Each commit's successes are
# read once, into the file STATUSES as `COMMIT CONTEXT` lines, for both
# architectures.
last_pass() { # last_pass ARCH
    for c in $(git rev-list --first-parent --max-count="$depth" "$sha^" 2> /dev/null); do
        if ! grep -q "^$c " "$statuses"; then
            passes=$(gh api "repos/$repo/commits/$c/status" \
                --jq '.statuses[] | select(.state == "success") | .context') ||
                return 1
            { echo "$c -"; for p in $passes; do echo "$c $p"; done; } >> "$statuses"
        fi
        for context in $contexts; do
            grep -qx "$c $context/$1" "$statuses" && { echo "$c"; return 0; }
        done
    done
    return 0
}

# The open issue with the label LABEL and the title TITLE, as its number
# followed by its labels, or nothing; the status is 1 when the listing could
# not be read. The listing is read rather than searched, since the search
# index lags.
open_issue() { # open_issue LABEL TITLE
    list=$(gh issue list --repo "$repo" --state open --label "$1" --limit 500 \
        --json number,title,labels \
        --jq ".[] | select(.title == \"$2\") | [.number, .labels[].name] | map(tostring) | join(\" \")") ||
        return 1
    echo "$list" | head -n 1
}

# Set `now` to the labels of the issue NUMBER, separated by spaces. When they
# cannot be read the failure is named and `now` is `intermittent`, so that a
# repeated failure loses that label and starts a bisection in any case.
labels_now() { # labels_now NUMBER
    now=$(gh issue view "$1" --repo "$repo" --json labels \
        --jq '[.labels[].name] | join(" ")') ||
        { lost "the labels of #$1"; now=intermittent; }
}

# The numbers of the open issues with the label `hold`, oldest first; the
# status is 1 when the listing could not be read.
open_holds() {
    gh issue list --repo "$repo" --state open --label hold --limit 500 \
        --json number --jq 'sort_by(.number) | .[].number'
}

# Start bisect.yml for the issue NUMBER on the range GOOD..SHA and the items
# that failed on ARCH, or say on the issue that it could not be started.
start_bisect() { # start_bisect NUMBER
    if gh workflow run bisect.yml --repo "$repo" --ref master -f kind="$kind" \
        -f arch="$arch" -f good="$good" -f bad="$sha" -f items="$items" \
        -f issue="$1" -f run="$GITHUB_RUN_ID" > /dev/null; then
        echo "ci: report: $arch: bisect started for #$1"
    else
        lost "the bisect for #$1"
        gh issue comment "$1" --repo "$repo" --body "ci could not start the bisection that would find the commit that caused this failure. Narrow the commits since the last pass by hand." > /dev/null
    fi
}

# Whether the core job of ARCH ran no step on any runner, and SHA has no
# status `ci/lost/ARCH` from an earlier run whose job on ARCH was lost.
unrun_once() { # unrun_once ARCH
    case "$unrun" in *"Test on $1"*) ;; *) return 1 ;; esac
    case " $errors " in *" ci/lost/$1 "*) return 1 ;; esac
}

unrun=""
errors=""
if [ "$kind" = core ]; then
    unrun=$(gh api "repos/$repo/actions/runs/$GITHUB_RUN_ID/attempts/$attempt/jobs" \
        --jq '[.jobs[] | select((.runner_id // 0) == 0 and (.steps | length) == 0) | .name] | join(",")') ||
        { lost "the jobs of the run"; unrun=""; }
    errors=$(gh api "repos/$repo/commits/$sha/status" \
        --jq '[.statuses[] | select(.state == "error") | .context] | join(" ")') ||
        { lost "the statuses of $short"; errors="ci/lost/i686 ci/lost/x86_64"; }
fi
passed=""
held=""
rerun=""
summarize "## $what of $short" ""
[ "$ctx" != ci/rerun ] || summarize "This run tested only these suites: $SUITES." ""
for arch in i686 x86_64; do
    result="$dir/$kind-$arch"
    case $arch in
        i686) verdict=${VERDICT_i686-} ;;
        x86_64) verdict=${VERDICT_x86_64-} ;;
    esac
    if [ ! -f "$result/done" ] && [ "${verdict%% *}" = "done" ]; then
        mkdir -p "$result"
        echo "${verdict#done}" | tr ' ' '\n' | sed '/^$/d' > "$result/failed"
        : > "$result/done"
    fi
    if [ -f "$result/done" ] && [ -f "$result/skipped" ]; then
        passed="$passed $arch"
        summarize "$arch had no suite to test." ""
        continue
    fi
    if [ -f "$result/done" ] && [ -f "$result/failed" ] && [ ! -s "$result/failed" ]; then
        set_status "$ctx/$arch" success
        passed="$passed $arch"
        summarize "$arch passed." ""
        continue
    fi
    if [ ! -f "$result/done" ] && unrun_once "$arch" &&
        set_status "ci/lost/$arch" error; then
        set_status "$ctx/$arch" error
        rerun="$rerun $arch"
        summarize "$arch: no runner took the job, so the core tests start again." ""
        continue
    fi
    set_status "$ctx/$arch" failure
    if [ -f "$result/done" ] && [ -s "$result/failed" ]; then
        items=$(sort -u "$result/failed" | tr '\n' ' ' | sed 's/ $//')
    else
        items=unfinished-$kind
    fi
    tests=$(echo "$what" | tr '[:upper:]' '[:lower:]')
    case $items in
        unfinished-*)
            title="$what did not finish on $arch"
            failed="did not finish"
            first="The $tests of commit $short did not finish on $arch: the run left no result, as when it reaches its time limit or loses its runner." ;;
        *)
            title="$what fail on $arch: $(echo "$items" | sed 's/ /, /g')"
            failed=$(echo "$items" | sed "s/^/\`/; s/ /\`, \`/g; s/\$/\`/")
            first="The $tests of commit $short failed on $arch: $failed." ;;
    esac
    case " $items " in
        *" build "*|*" style "*|*" runner "*|*" core "*|*" default "*|*" unfinished-core "*)
            [ "$kind" = core ] && held="$held
* $arch: $failed" ;;
    esac
    good=$(last_pass "$arch") || { lost "the last pass on $arch"; good=""; }
    if [ -n "$good" ]; then
        range="Commits since the last pass on $arch, $(git rev-parse --short "$good")..$short, $(git rev-list --count "$good..$sha") in all:

$(git log --format='* %h %s' "$good..$sha")"
    else
        range="The commits since the last pass are unknown: no commit in the $depth before $short is known to have passed the $tests on $arch."
    fi
    summarize "$arch failed: $items." ""
    lines=
    if [ -f "$result/log" ]; then
        lines=$(grep -E ': FAIL|: result: fail' "$result/log" | head -n 8)
        [ -z "$lines" ] || summarize '```' "$lines" '```' ""
    fi
    summarize "Logs: \`gh run download $GITHUB_RUN_ID --repo $repo --name $kind-$arch\`" ""
    body=$first
    [ -z "$lines" ] || body="$body

The first lines of the log that name a failure:

\`\`\`
$lines
\`\`\`"
    body="$body

Logs: \`gh run download $GITHUB_RUN_ID --repo $repo --name $kind-$arch\`
Run: $run_url
Attempt: $attempt
Commit: $sha"
    if [ -f "$result/log" ]; then
        body="$body

<details><summary>The last 40 lines of the log</summary>

\`\`\`
$(tail -n 40 "$result/log")
\`\`\`

</details>"
    else
        body="$body

The run's output was not kept; \`gh run view $GITHUB_RUN_ID --repo $repo --log\` prints the log of each job."
    fi
    body="$body

$range"
    if ! found=$(open_issue bug "$title"); then
        lost "the issue list for $arch"
        continue
    fi
    number=${found%% *}
    if [ -n "$number" ]; then
        gh issue comment "$number" --repo "$repo" --body "$body" > /dev/null ||
            lost "the comment on #$number"
        echo "ci: report: $arch: failure added to #$number"
        summarize "Added to issue #$number." ""
        labels_now "$number"
        case " $found $now " in
            *" intermittent "*)
                gh issue edit "$number" --repo "$repo" --remove-label intermittent > /dev/null ||
                    lost "the label of #$number"
                echo "ci: report: $arch: #$number is no longer intermittent"
                [ -z "$good" ] || start_bisect "$number" ;;
        esac
        continue
    fi
    if ! url=$(gh issue create --repo "$repo" --title "$title" --label bug --label ci --body "$body"); then
        lost "the issue for $arch"
        continue
    fi
    number=${url##*/}
    echo "ci: report: $arch: filed #$number"
    summarize "Filed as issue #$number." ""
    [ -z "$good" ] || start_bisect "$number"
done

if [ "$kind" = core ]; then
    if ! holds=$(open_holds); then
        lost "the hold issue's list"
    elif [ -n "$held" ]; then
        body="The core tests of commit $short on master failed in a way that stops every check: the build, the style checks, the test runner or the boot of the test image.
$held

Run: $run_url

While this issue is open, push to master only a repair toward a passing core run, and name it here. The next core run that passes on both architectures closes the issue."
        hold=$(echo "$holds" | head -n 1)
        if [ -n "$hold" ]; then
            gh issue comment "$hold" --repo "$repo" --body "$body" > /dev/null || lost "the comment on #$hold"
        else
            gh issue create --repo "$repo" --title "$hold_title" --label hold --body "$body" > /dev/null ||
                lost "the hold issue"
        fi
        echo "ci: report: hold opened"
    elif [ "$passed" = " i686 x86_64" ]; then
        for hold in $holds; do
            gh issue close "$hold" --repo "$repo" --comment "The core tests of commit $short passed on both architectures, so pushes to master may go on: $run_url" > /dev/null ||
                lost "the close of #$hold"
            echo "ci: report: hold #$hold closed"
        done
    fi
fi
if [ -n "$rerun" ]; then
    gh workflow run ci.yml --repo "$repo" --ref master > /dev/null ||
        lost "the core run started again"
    echo "ci: report:$rerun: no runner took the job; core started again"
fi
if [ "$rc" = 0 ] && [ -z "$rerun" ]; then
    if [ "$passed" = " i686 x86_64" ]; then
        set_status "$ctx" success
    else
        set_status "$ctx" failure
    fi
fi
exit "$rc"
