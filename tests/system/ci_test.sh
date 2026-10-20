#!/bin/sh
# ci_test.sh: check unfinished ci reports, a result whose upload was lost,
# a core job that no runner took, the labels and body of a filed issue, a
# full run's last pass, a failure repeated on an intermittent issue, the hold
# issue, bisection runs, their time limits and continuation, the run whose
# issue bisection labels intermittent, a report and a bisection that
# overlap, the kind an older commit's run.sh takes, a refused run in
# bisection, a bisection whose issue has been closed, verdict errors, the
# statuses of a full run of chosen suites, the attempts that etude's history
# of a run lists in its issue, and an intermittent issue that a passing run
# leaves open.
# The scripts run in a private tree with stand-ins for git, gh, make,
# timeout, the clock and etude's history_failures. Each row runs in a
# subshell of its own, and every row runs whatever an earlier one gave.
# CI_SCRIPTS names the directory report.sh and bisect.sh are copied from, by
# default the tree's tools/ci; tests/proofs/ci.json runs the rows against
# copies with faults in them.

set -eu
[ "$#" -eq 0 ] || { echo 'ci-test: usage: ci_test.sh' >&2; exit 2; }
repo=$(CDPATH='' cd -- "$(dirname -- "$0")/../.." && pwd)
work=
trap '[ -z "$work" ] || rm -rf "$work"' EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM
work=$(mktemp -d "${TMPDIR:-/tmp}/rach2-ci-test.XXXXXX")
mkdir -p "$work/bin" "$work/tree/tools/ci" "$work/tree/tests"
cp "${CI_SCRIPTS:-$repo/tools/ci}/report.sh" "$work/report.sh"
cp "${CI_SCRIPTS:-$repo/tools/ci}/bisect.sh" "$work/bisect.sh"

cat > "$work/bin/git" <<'EOF'
#!/bin/sh
set -eu
case $1 in
    rev-parse) echo "$3" ;;
    rev-list)
        case $2 in
            --first-parent) echo good ;;
            --reverse) echo bad ;;
            --count) echo 1 ;;
            *) exit 2 ;;
        esac ;;
    checkout) echo "$4" > "$CI_TEST_WORK/current" ;;
    log) echo 'bad test commit' ;;
    *) exit 2 ;;
esac
EOF
cat > "$work/bin/gh" <<'EOF'
#!/bin/sh
set -eu
echo "$*" >> "$CI_TEST_WORK/gh.log"
case "$1 $2" in
    'api repos/test/rach2/commits/good/status')
        case ${CI_TEST_BASE:-} in
            night) printf '%s\n' ci/night/i686 ci/night/x86_64 ;;
            *) printf '%s\n' ci/core ci/core/i686 ci/core/x86_64 ci/full/i686 ci/full/x86_64 ;;
        esac ;;
    'api repos/test/rach2/actions/runs/1/attempts/1/jobs') echo "${CI_TEST_UNRUN:-}" ;;
    'api repos/test/rach2/statuses/bad') echo "${6#context=} ${4#state=}" >> "$CI_TEST_WORK/statuses" ;;
    'api repos/test/rach2/commits/bad/status')
        [ ! -f "$CI_TEST_WORK/statuses" ] ||
            awk '{ s[$1] = $2 } END { for (c in s) if (s[c] == "error") printf "%s ", c }' "$CI_TEST_WORK/statuses" ;;
    'api '*) ;;
    'issue list')
        case "$*" in
            *'--label hold '*'--json number --jq '*)
                [ -z "${CI_TEST_HOLD:-}" ] || printf '%s\n' $CI_TEST_HOLD ;;
            *'--label bug '*'--json number,title,labels '*"select(.title == \"${CI_TEST_OPEN_TITLE:-}\")"*)
                echo "$CI_TEST_OPEN" ;;
            *' --json number --jq '*)
                all="$*" label=${all#*--label } label=${label%% *}
                case " ${CI_TEST_OPEN#* } " in *" $label "*) echo "${CI_TEST_OPEN%% *}" ;; esac ;;
        esac ;;
    'issue view')
        case "$*" in
            *' --json labels '*)
                echo "$CI_TEST_LABELS"
                exit 0 ;;
            *' --json body,comments '*) ;;
            *' --json state '*)
                echo "${CI_TEST_STATE:-OPEN}"
                exit 0 ;;
            *) exit 2 ;;
        esac
        runs=${CI_TEST_RUNS:-}
        ! grep -q -- '--add-label intermittent' "$CI_TEST_WORK/gh.log" || runs=${CI_TEST_RUNS_LABELLED:-$runs}
        for id in $runs; do
            printf 'A failure.\n\nRun: https://example.invalid/test/rach2/actions/runs/%s\nAttempt: 1\n' "$id"
        done ;;
    'issue create') echo https://example.invalid/issues/7 ;;
    'issue comment') [ "${CI_TEST_GH_FAIL:-}" != comment ] ;;
    'issue edit') [ "${CI_TEST_GH_FAIL:-}" != edit ] ;;
    'issue close'|'workflow run') ;;
    *) exit 2 ;;
esac
EOF
cat > "$work/bin/make" <<'EOF'
#!/bin/sh
echo make >> "$CI_TEST_WORK/events"
[ -z "${CI_TEST_MAKE_FAIL:-}" ]
EOF
cat > "$work/bin/history_failures" <<'EOF'
#!/bin/sh
echo "$*" >> "$CI_TEST_WORK/reader.log"
for path in "$@"; do
    [ "$path" = -b ] || [ -e "$path" ] ||
        { echo "history_failures: $path: cannot be opened" >&2; exit 2; }
done
[ -z "${CI_TEST_HISTORY_OUT:-}" ] || printf '%s\n' "$CI_TEST_HISTORY_OUT"
exit "${CI_TEST_HISTORY_STATUS:-0}"
EOF
cat > "$work/tree/tests/run" <<'EOF'
#!/bin/sh
current=$(cat "$CI_TEST_WORK/current")
echo "tests $* $current" >> "$CI_TEST_WORK/events"
case $CI_TEST_RUN/$current in
    fail/bad) echo "run: $1: FAIL in 1s"; echo 'run: result: failed'; exit 1 ;;
    refuse/bad) echo "run: result: refused: unknown suite '$1'" >&2; exit 2 ;;
esac
echo "run: $1: PASS in 1s"
echo 'run: result: all 1 suites passed'
EOF
cat > "$work/bin/timeout" <<'EOF'
#!/bin/sh
echo "$3" >> "$CI_TEST_WORK/limits"
shift 3
exec "$@"
EOF
cat > "$work/bin/date" <<'EOF'
#!/bin/sh
[ "$1" = +%s ] || exit 2
cat "$CI_TEST_WORK/clock"
EOF

# runsh KINDS: write the stand-in tools/ci/run.sh, which takes the kinds
# KINDS, written as its case pattern: `core|full` now, `core|night` before.
runsh()
{
    # shellcheck disable=SC2016 # the stand-in reads its own $1
    printf '#!/bin/sh\nset -eu\ncase $1 in\n    %s) ;;\n    *) exit 2 ;;\nesac\n' "$1" > "$work/tree/tools/ci/run.sh"
    cat >> "$work/tree/tools/ci/run.sh" <<'EOF'
current=$(cat "$CI_TEST_WORK/current")
echo "run $1 $current" >> "$CI_TEST_WORK/events"
echo $(($(cat "$CI_TEST_WORK/clock") + ${CI_TEST_STEP:-0})) > "$CI_TEST_WORK/clock"
[ "$CI_TEST_FAIL" = 0 ] || [ "$current" = good ]
EOF
}
runsh 'core|full'
chmod +x "$work/bin/"* "$work/tree/tests/run"
CI_TEST_WORK=$work
GITHUB_REPOSITORY=test/rach2
GITHUB_SERVER_URL=https://example.invalid
GITHUB_RUN_ID=1
HISTORY_FAILURES=$work/bin/history_failures
PATH=$work/bin:$PATH
export CI_TEST_WORK GITHUB_REPOSITORY GITHUB_SERVER_URL GITHUB_RUN_ID HISTORY_FAILURES PATH
cd "$work/tree"

# clear: drop the passes, the commit's statuses, the open issue, its labels
# and the runs that rows give the gh stand-in.
clear()
{
    rm -f "$work/statuses"
    CI_TEST_BASE='' CI_TEST_OPEN='' CI_TEST_OPEN_TITLE='' CI_TEST_LABELS=''
    CI_TEST_RUNS='' CI_TEST_RUNS_LABELLED='' CI_TEST_STATE=''
    export CI_TEST_BASE CI_TEST_OPEN CI_TEST_OPEN_TITLE CI_TEST_LABELS
    export CI_TEST_RUNS CI_TEST_RUNS_LABELLED CI_TEST_STATE
}

# results KIND STATE: report a run of KIND whose x86_64 half passed and
# whose i686 half ended in STATE, or for `swapped`, whose i686 half passed
# and whose x86_64 half left no result.
results()
{
    kind=$1 state=$2 verdict=
    CI_TEST_GH_FAIL=
    export CI_TEST_GH_FAIL
    rm -rf "$work/results"
    mkdir -p "$work/results/$kind-i686" "$work/results/$kind-x86_64"
    result=$work/results/$kind-i686
    : > "$work/results/$kind-x86_64/done"
    : > "$work/results/$kind-x86_64/failed"
    case $state in
        missing) rm -rf "$result" ;;
        unfinished) echo default > "$result/failed" ;;
        missing-failed) : > "$result/done" ;;
        runner) echo runner > "$result/failed"; : > "$result/done" ;;
        lost) rm -rf "$result"; verdict='done ' ;;
        lost-runner) rm -rf "$result"; verdict='done runner default' ;;
        swapped) : > "$result/done"; : > "$result/failed"; rm -rf "$work/results/$kind-x86_64" ;;
    esac
    : > "$work/gh.log"
    VERDICT_i686=$verdict sh "$work/report.sh" "$kind" bad "$work/results" > "$work/report.log" 2>&1
}

# report KIND STATE ITEM: require an issue labelled `bug` and `ci` that
# gives the attempt and the commit, a bisection of ITEM that names this
# run, and the core hold.
report()
{
    clear
    results "$1" "$2" || return 1
    grep -q '^issue create .* --label bug --label ci --body ' "$work/gh.log" || return 1
    grep -qx 'Attempt: 1' "$work/gh.log" && grep -qx 'Commit: bad' "$work/gh.log" || return 1
    grep -q "workflow run bisect.yml .* -f items=$3 -f issue=7 -f run=1" "$work/gh.log" || return 1
    if [ "$1" = core ]; then
        grep -q 'issue create .* --label hold ' "$work/gh.log" || return 1
    else
        ! grep -q 'issue create .* --label hold ' "$work/gh.log" || return 1
    fi
}

# lost: require a core run whose i686 result was not uploaded, with a
# verdict of success in its job's outputs, to pass and file nothing, and a
# verdict of failure to file the steps it names without the log.
lost()
{
    report core lost-runner 'default runner' || return 1
    grep -q " --title Core tests fail on i686: default, runner .* --body The core tests of commit bad failed on i686: \`default\`, \`runner\`\.$" "$work/gh.log" || return 1
    grep -q 'The run.s output was not kept; .gh run view 1 ' "$work/gh.log" || return 1
    clear
    results core lost || return 1
    grep -q 'statuses/bad -f state=success -f context=ci/core/i686 ' "$work/gh.log" || return 1
    grep -q 'statuses/bad -f state=success -f context=ci/core ' "$work/gh.log" || return 1
    ! grep -q '^issue create ' "$work/gh.log"
}

# unrun: require a core run whose i686 job no runner took to give i686 the
# statuses `error` and `ci/lost/i686`, file nothing, leave `ci/core` unset
# and start ci.yml again; a second run on the same commit that passes i686
# and loses x86_64 to do the same for x86_64; and a third that loses i686
# again, though its status is now a success, to report that job as
# unfinished.
unrun()
(
    clear
    CI_TEST_UNRUN='Choose the commit,Test on i686'
    export CI_TEST_UNRUN
    results core missing || return 1
    grep -q 'statuses/bad -f state=error -f context=ci/core/i686 ' "$work/gh.log" || return 1
    grep -q 'statuses/bad -f state=error -f context=ci/lost/i686 ' "$work/gh.log" || return 1
    grep -q '^workflow run ci.yml --repo test/rach2 --ref master' "$work/gh.log" || return 1
    ! grep -q '^issue create ' "$work/gh.log" || return 1
    ! grep -q 'context=ci/core ' "$work/gh.log" || return 1
    CI_TEST_UNRUN='Choose the commit,Test on x86_64'
    results core swapped || return 1
    grep -q 'statuses/bad -f state=success -f context=ci/core/i686 ' "$work/gh.log" || return 1
    grep -q 'statuses/bad -f state=error -f context=ci/lost/x86_64 ' "$work/gh.log" || return 1
    grep -q '^workflow run ci.yml ' "$work/gh.log" || return 1
    ! grep -q '^issue create ' "$work/gh.log" || return 1
    CI_TEST_UNRUN='Choose the commit,Test on i686'
    results core missing || return 1
    grep -q '^issue create --repo test/rach2 --title Core tests did not finish on i686 ' "$work/gh.log" || return 1
    ! grep -q 'context=ci/lost/' "$work/gh.log" || return 1
    ! grep -q 'workflow run ci.yml' "$work/gh.log"
)

# base: with only nightly passes on GOOD, require a full report to bisect
# from GOOD as a full run, with this run's ID, and a core report to find no
# last pass.
base()
{
    clear
    CI_TEST_BASE=night
    results full runner || return 1
    grep -q 'workflow run bisect.yml .* -f kind=full -f arch=i686 -f good=good -f bad=bad -f items=runner -f issue=7 -f run=1' "$work/gh.log" || return 1
    grep -q ' --title Release tests fail on i686: runner ' "$work/gh.log" || return 1
    results core runner || return 1
    ! grep -q 'workflow run' "$work/gh.log" || return 1
    grep -q 'are unknown: no commit in the 50 before bad' "$work/gh.log" || return 1
    CI_TEST_BASE=''
}

# again LABELS [LATER]: with #12 open under the title of the runner's
# failure on i686 and labelled LABELS when it is listed, and LATER, by
# default LABELS, when it is read after the comment, require a core report
# to comment on #12 and file no failure issue; when either has
# `intermittent`, to remove that label and bisect for #12 again, and
# otherwise to do neither.
again()
{
    clear
    CI_TEST_OPEN_TITLE='Core tests fail on i686: runner' CI_TEST_OPEN="12 $1"
    CI_TEST_LABELS=${2-$1}
    results core runner || return 1
    grep -q '^issue comment 12 ' "$work/gh.log" || return 1
    ! grep -q '^issue create .* --title Core tests ' "$work/gh.log" || return 1
    case " $1 ${2-} " in
        *' intermittent '*)
            grep -q '^issue edit 12 --repo test/rach2 --remove-label intermittent' "$work/gh.log" || return 1
            grep -q 'workflow run bisect.yml .* -f items=runner -f issue=12 -f run=1' "$work/gh.log" || return 1 ;;
        *)
            ! grep -q '^issue edit ' "$work/gh.log" || return 1
            ! grep -q 'workflow run' "$work/gh.log" || return 1 ;;
    esac
    clear
}

# hold STATE: with holds #5 and #9 open under any title, require a core run
# that fails to boot to comment on #5 alone and open no hold, and one that
# passes to close both.
hold()
{
    clear
    rm -rf "$work/results"
    for arch in i686 x86_64; do
        mkdir -p "$work/results/core-$arch"
        : > "$work/results/core-$arch/done"
        : > "$work/results/core-$arch/failed"
    done
    [ "$1" = pass ] || echo default > "$work/results/core-i686/failed"
    : > "$work/gh.log"
    CI_TEST_HOLD='5 9' sh "$work/report.sh" core bad "$work/results" > "$work/report.log" 2>&1 || return 1
    ! grep -q 'issue create .* --label hold ' "$work/gh.log" || return 1
    if [ "$1" = pass ]; then
        grep -q '^issue close 5 ' "$work/gh.log" && grep -q '^issue close 9 ' "$work/gh.log"
    else
        grep -q '^issue comment 5 ' "$work/gh.log" && ! grep -q '^issue comment 9 ' "$work/gh.log"
    fi
}

# chosen: require a full run of chosen suites, with nothing to test on i686
# and a pass on x86_64, to set `ci/rerun/x86_64` and `ci/rerun` and no other
# status.
chosen()
{
    clear
    rm -rf "$work/results"
    mkdir -p "$work/results/full-i686" "$work/results/full-x86_64"
    : > "$work/results/full-i686/done"
    : > "$work/results/full-i686/skipped"
    : > "$work/results/full-x86_64/done"
    : > "$work/results/full-x86_64/failed"
    : > "$work/gh.log"
    SUITES=x86_64:agents sh "$work/report.sh" full bad "$work/results" > "$work/report.log" 2>&1 || return 1
    [ "$(grep -o 'state=[a-z]* -f context=[a-z0-9_/]*' "$work/gh.log" | tr '\n' ,)" = \
        'state=success -f context=ci/rerun/x86_64,state=success -f context=ci/rerun,' ]
}

# start: reset the logs, the stand-ins' records and the clock.
start()
{
    clear
    RUNNER_TEMP=$work/logs
    export RUNNER_TEMP
    rm -rf "$RUNNER_TEMP"
    : > "$work/events"
    : > "$work/gh.log"
    : > "$work/limits"
    echo 0 > "$work/clock"
}

# bisect ITEM COMMAND FAIL [GH-FAIL]: check runs, their time limits and
# verdict delivery. For an unfinished run, COMMAND is the kind given to
# run.sh; for another item, it is the tests/run lines expected after the
# build, and the stand-in runner always passes. Every command runs under
# timeout(1) with the run kind's whole limit, since the stand-in clock does
# not move.
bisect()
{
    item=$1 command=$2 CI_TEST_FAIL=$3 CI_TEST_GH_FAIL=${4:-} CI_TEST_RUN=pass
    export CI_TEST_FAIL CI_TEST_GH_FAIL CI_TEST_RUN
    case $item in
        unfinished-full) kind=full seconds=20400 ;;
        unfinished-night) kind=night seconds=20400 ;;
        *) kind=core seconds=10200 ;;
    esac
    start
    status=0
    # shellcheck disable=SC2086 # the items are one word each
    sh "$work/bisect.sh" "$kind" i686 good bad 7 $item > "$work/bisect.log" 2>&1 || status=$?
    if [ -n "$CI_TEST_GH_FAIL" ]; then
        [ "$status" = 1 ] || return 1
        case $CI_TEST_GH_FAIL in
            comment) diagnostic=comment ;;
            edit) diagnostic=label ;;
        esac
        grep -q "the $diagnostic on #7 failed" "$work/bisect.log" || return 1
    else
        [ "$status" = 0 ] || return 1
    fi
    grep -q 'issue comment 7 ' "$work/gh.log" || return 1
    case $item in
        unfinished-*)
            if [ "$CI_TEST_FAIL" = 0 ]; then
                printf 'run %s bad\n' "$command" > "$work/expected"
            else
                printf 'run %s bad\nrun %s bad\nrun %s good\n' "$command" "$command" "$command" > "$work/expected"
            fi ;;
        *) printf 'make\n%b\n' "$command" > "$work/expected" ;;
    esac
    cmp -s "$work/expected" "$work/events" || return 1
    [ "$(wc -l < "$work/limits")" = "$(wc -l < "$work/events")" ] || return 1
    ! grep -qvx "${seconds}s" "$work/limits" || return 1
    if [ "$CI_TEST_FAIL" = 0 ]; then
        grep -q -- '--add-label intermittent' "$work/gh.log" || return 1
    else
        grep -q 'is the first commit that fails .*; its parent, good, passes' "$work/gh.log" || return 1
    fi
}

# closed: bisect for an issue that has been closed, and require the script
# to stop with status 0 before it tests a commit or comments.
closed()
(
    CI_TEST_FAIL=1 CI_TEST_GH_FAIL='' CI_TEST_RUN=pass
    export CI_TEST_FAIL CI_TEST_GH_FAIL CI_TEST_RUN
    start
    CI_TEST_STATE=CLOSED
    sh "$work/bisect.sh" core i686 good bad 7 unfinished-core > "$work/bisect.log" 2>&1 || return 1
    grep -q 'stopped: #7 was closed' "$work/bisect.log" || return 1
    [ ! -s "$work/events" ] || return 1
    ! grep -q 'issue comment 7 ' "$work/gh.log"
)

# older: bisect an unfinished full run on commits whose run.sh takes `night`
# but not `full`, and require run.sh to be called with `night`.
older()
{
    runsh 'core|night'
    status=0
    bisect unfinished-full night 0 || status=1
    runsh 'core|full'
    return "$status"
}

# latest RUNS [LABELLED]: bisect a failure of run 1 that does not repeat,
# with -r 1, on an issue whose Run lines name RUNS, oldest first, and
# LABELLED once the label `intermittent` has been added; require the issue
# to keep that label only when the last run is run 1 both times, and
# otherwise a comment that names the newer run, with the label removed when
# it was added.
latest()
{
    CI_TEST_FAIL=0 CI_TEST_GH_FAIL='' CI_TEST_RUN=pass
    export CI_TEST_FAIL CI_TEST_GH_FAIL CI_TEST_RUN
    start
    CI_TEST_RUNS=$1 CI_TEST_RUNS_LABELLED=${2-$1}
    status=0
    sh "$work/bisect.sh" -r 1 core i686 good bad 7 unfinished-core > "$work/bisect.log" 2>&1 || status=$?
    CI_TEST_RUNS='' CI_TEST_RUNS_LABELLED=''
    [ "$status" = 0 ] || return 1
    grep -q '^issue view 7 --repo test/rach2 --json body,comments ' "$work/gh.log" || return 1
    case " $1/ ${2-$1}" in
        *' 1/'*' 1')
            grep -q -- '--add-label intermittent' "$work/gh.log" || return 1
            ! grep -q -- '--remove-label intermittent' "$work/gh.log" ;;
        *' 1/'*)
            grep -q -- '--add-label intermittent' "$work/gh.log" || return 1
            grep -q -- '--remove-label intermittent' "$work/gh.log" || return 1
            grep -q "run ${2##* } has reported the failure since run 1" "$work/gh.log" ;;
        *)
            ! grep -q -- '--add-label intermittent' "$work/gh.log" || return 1
            grep -q "run ${1##* } has reported the failure since run 1" "$work/gh.log" ;;
    esac
}

# handoff: give a core bisection of run 1 700 minutes and let each run take
# 400 of them; require the run to test BAD alone and leave the last step to a
# new run with -c and run 1, that run to test both commits without BAD
# again, and a run of 100 minutes to test nothing and say that the range is
# unresolved.
handoff()
{
    CI_TEST_FAIL=1 CI_TEST_GH_FAIL='' CI_TEST_STEP=24000
    export CI_TEST_FAIL CI_TEST_GH_FAIL CI_TEST_STEP
    start
    sh "$work/bisect.sh" -r 1 -m 700 core i686 good bad 7 unfinished-core > "$work/bisect.log" 2>&1 || return 1
    printf 'run core bad\n' > "$work/expected"
    cmp -s "$work/expected" "$work/events" || return 1
    grep -q 'workflow run bisect.yml .* -f kind=core -f arch=i686 -f good=good -f bad=bad -f items=unfinished-core -f issue=7 -f confirmed=yes -f run=1' "$work/gh.log" || return 1
    grep -q 'good passes and bad fails' "$work/gh.log" || return 1
    start
    sh "$work/bisect.sh" -c -m 700 core i686 good bad 7 unfinished-core > "$work/bisect.log" 2>&1 || return 1
    printf 'run core bad\nrun core good\n' > "$work/expected"
    cmp -s "$work/expected" "$work/events" || return 1
    ! grep -q 'workflow run' "$work/gh.log" || return 1
    grep -q 'is the first commit that fails .*; its parent, good, passes' "$work/gh.log" || return 1
    start
    status=0
    sh "$work/bisect.sh" -m 100 core i686 good bad 7 unfinished-core > "$work/bisect.log" 2>&1 || status=$?
    [ "$status" = 1 ] && [ ! -s "$work/events" ] || return 1
    grep -q "unresolved: this run has 100 minutes" "$work/gh.log" || return 1
    CI_TEST_STEP=0
}

# suite RESULT: bisect the runner suite when tests/run on BAD passes, fails,
# or refuses, and check that only a failure it reports is counted as one.
suite()
{
    CI_TEST_RUN=$1 CI_TEST_GH_FAIL=
    export CI_TEST_RUN CI_TEST_GH_FAIL
    start
    status=0
    sh "$work/bisect.sh" core i686 good bad 7 runner > "$work/bisect.log" 2>&1 || status=$?
    case $1 in
        pass) printf 'make\ntests runner bad\n' > "$work/expected"
              verdict='--add-label intermittent' want=0 ;;
        fail) printf 'make\ntests runner bad\nmake\ntests runner bad\nmake\ntests runner good\n' > "$work/expected"
              verdict='is the first commit that fails .*; its parent, good, passes' want=0 ;;
        refuse) printf 'make\ntests runner bad\n' > "$work/expected"
                verdict='unresolved: bad could not be tested again' want=1 ;;
    esac
    [ "$status" = "$want" ] || return 1
    cmp -s "$work/expected" "$work/events" || return 1
    grep -q -- "$verdict" "$work/gh.log"
}

# attempts CASE: report a core run whose i686 half kept etude's history,
# which the stand-in reader judges, and whose x86_64 half passed with no
# history, and require for CASE:
#   flaky     `default` failed and the reader lists an attempt that failed
#             before its retry passed: the issue, the summary and the reader's
#             arguments give that attempt and the store, and bisection and
#             the hold start.
#   repeated  the same with #12 open for that failure and intermittent: the
#             comment on #12 gives the attempt, the label goes, and bisection
#             starts for #12.
#   hidden    nothing failed, but the reader lists an attempt that did not
#             pass: the runner's failure is filed with that attempt.
#   unread    nothing failed, but the reader cannot read the history: the
#             runner's failure is filed with the reader's diagnostic.
#   unbuilt   nothing failed, but make cannot build the reader: the runner's
#             failure is filed and says so.
#   kept      every attempt passed, with #12 open and intermittent: the run
#             passes and #12 is not closed, edited or commented on.
# No case closes an issue or rewrites one's body.
attempts()
(
    clear
    rm -rf "$work/results"
    for arch in i686 x86_64; do
        mkdir -p "$work/results/core-$arch"
        : > "$work/results/core-$arch/done"
        : > "$work/results/core-$arch/failed"
    done
    result=$work/results/core-i686
    mkdir "$result/history" "$result/store"
    : > "$result/history/run.jsonl"
    item='guest.default.rngprobe on i686, item r1-i0: failed, and passed on another attempt, so it may be flaky'
    assertion='    trusted-monobit: fail: 61 of 512 bits set'
    said="The attempts that did not pass, from etude's history of the run; a failure stays listed when a later attempt of its case passed:"
    CI_TEST_HISTORY_STATUS=1 CI_TEST_GH_FAIL='' CI_TEST_MAKE_FAIL=''
    CI_TEST_HISTORY_OUT="run r1: failure: of 2 attempts, 1 failed, 0 did not finish
$item
  attempt r1-i0-a0: failure: it exited 1
$assertion
  attempt r1-i0-a1, a retry of r1-i0-a0: pass"
    case $1 in
        flaky|repeated) echo default > "$result/failed" ;;
        unread)
            CI_TEST_HISTORY_STATUS=2
            # shellcheck disable=SC2016 # the diagnostic has a literal `run`
            CI_TEST_HISTORY_OUT='history_failures: run.jsonl:3: the record of kind `run` is incomplete' ;;
        unbuilt) unset HISTORY_FAILURES; CI_TEST_MAKE_FAIL=1 ;;
        kept) CI_TEST_HISTORY_STATUS=0 CI_TEST_HISTORY_OUT='' ;;
    esac
    case $1 in
        repeated|kept)
            CI_TEST_OPEN_TITLE='Core tests fail on i686: default'
            CI_TEST_OPEN='12 bug ci intermittent' CI_TEST_LABELS='bug ci intermittent' ;;
    esac
    export CI_TEST_HISTORY_STATUS CI_TEST_HISTORY_OUT CI_TEST_GH_FAIL CI_TEST_MAKE_FAIL
    export CI_TEST_OPEN_TITLE CI_TEST_OPEN CI_TEST_LABELS
    : > "$work/gh.log"
    : > "$work/reader.log"
    : > "$work/summary"
    GITHUB_STEP_SUMMARY=$work/summary sh "$work/report.sh" core bad "$work/results" > "$work/report.log" 2>&1 ||
        return 1
    ! grep -q '^issue close ' "$work/gh.log" || return 1
    ! grep -q '^issue edit .* --body' "$work/gh.log" || return 1
    if [ "$1" = kept ]; then
        grep -q 'statuses/bad -f state=success -f context=ci/core/i686 ' "$work/gh.log" || return 1
        grep -q 'statuses/bad -f state=success -f context=ci/core ' "$work/gh.log" || return 1
        ! grep -Eq '^issue (create|comment|edit) ' "$work/gh.log" || return 1
        return 0
    fi
    grep -q 'statuses/bad -f state=failure -f context=ci/core/i686 ' "$work/gh.log" || return 1
    grep -q 'issue create .* --label hold ' "$work/gh.log" || return 1
    case $1 in
        flaky|repeated|hidden)
            grep -qxF -- "-b $result/store $result/history/run.jsonl" "$work/reader.log" || return 1
            for line in "$said" "$item" "$assertion"; do
                grep -qxF "$line" "$work/gh.log" && grep -qxF "$line" "$work/summary" || return 1
            done ;;
    esac
    case $1 in
        flaky)
            grep -q '^issue create --repo test/rach2 --title Core tests fail on i686: default --label bug --label ci ' "$work/gh.log" || return 1
            grep -q 'workflow run bisect.yml .* -f items=default -f issue=7 -f run=1' "$work/gh.log" ;;
        repeated)
            grep -q '^issue comment 12 ' "$work/gh.log" || return 1
            ! grep -q '^issue create .* --title Core tests ' "$work/gh.log" || return 1
            grep -q '^issue edit 12 --repo test/rach2 --remove-label intermittent' "$work/gh.log" || return 1
            grep -q 'workflow run bisect.yml .* -f items=default -f issue=12 -f run=1' "$work/gh.log" ;;
        hidden)
            grep -q "^issue create --repo test/rach2 --title Core tests fail on i686: runner --label bug --label ci --body The core tests of commit bad failed on i686: \`runner\`\. The run listed no failure, but etude's history of the run has attempts that did not pass, so the failure is the test runner's\.\$" "$work/gh.log" || return 1
            grep -q 'workflow run bisect.yml .* -f items=runner -f issue=7 -f run=1' "$work/gh.log" ;;
        unread)
            grep -q "failed on i686: \`runner\`\. The run listed no failure, but etude's history of the run could not be read" "$work/gh.log" || return 1
            grep -qxF "Etude's history of the run could not be read:" "$work/gh.log" || return 1
            grep -qxF "history_failures: run.jsonl:3: the record of kind 'run' is incomplete" "$work/gh.log" ;;
        unbuilt)
            [ ! -s "$work/reader.log" ] || return 1
            grep -qxF 'ci: report: build/tools/etude/history_failures could not be built' "$work/gh.log" ;;
    esac
)

# rows: check both unfinished kinds, the filed issue and the hold issue,
# start core again after a job that no runner took, preserve an actual
# runner failure, take a nightly pass as a full run's last pass, bisect a
# repeated intermittent failure again, run the core set beside the other
# items of a core run that failed, take an issue's newest run before
# labelling it intermittent and again after, read an issue's labels again
# after reporting on it, repeat an unfinished full run as `night` where
# run.sh knows only that, continue a bisection in a new run, count only a
# reported failure of the runner suite as one, set the statuses of a run of
# chosen suites apart from a full run's, call no history reader for a result
# without etude's history, and report the attempts that a history lists. Each
# row that fails is printed as `ci-test: failed: ROW ARGS: the row fails`.
rows()
{
    rm -f "$work/reader.log"
    failed=0
    for kind in core full; do
        for state in missing unfinished missing-failed; do
            row report "$kind" "$state" "unfinished-$kind"
        done
        row report "$kind" runner runner
        row bisect "unfinished-$kind" "$kind" 0
        row bisect "unfinished-$kind" "$kind" 1
    done
    row bisect unfinished-night full 0
    row older
    row lost
    row unrun
    row base
    row again 'bug ci intermittent'
    row again 'bug ci'
    row again 'bug ci' 'bug ci intermittent'
    row hold fail
    row hold pass
    row bisect 'style core' 'tests style bad\ntests core bad' 0
    row bisect unfinished-core core 1 comment
    row bisect unfinished-core core 0 comment
    row bisect unfinished-core core 0 edit
    row latest 1
    row latest '1 2'
    row latest 1 '1 2'
    row handoff
    row closed
    row chosen
    for result in pass fail refuse; do
        row suite "$result"
    done
    if [ -e "$work/reader.log" ]; then
        echo 'ci-test: failed: suite: a history reader ran for a result without history' >&2
        failed=1
    fi
    for case in flaky repeated hidden unread unbuilt kept; do
        row attempts "$case"
    done
    return "$failed"
}

# row ROW ARGS...: run ROW with ARGS in a subshell, and report it when it
# fails.
row()
{
    if ! ("$@"); then
        printf 'ci-test: failed: %s: the row fails\n' "$*" >&2
        failed=1
    fi
}

if ! rows; then
    cat "$work/report.log" "$work/bisect.log" 2>/dev/null || :
    echo 'ci-test: result: fail: ci report, hold or bisection' >&2
    exit 1
fi
echo 'ci-test: result: pass: unfinished runs, lost uploads, lost jobs, issue labels, the last pass, repeated failures, the hold issue, the runner suite, core items, time limits, continuation, the newest run, overlapping reports, older run scripts, a refused run, a closed issue, verdict errors, a run of chosen suites, the attempts in etude history and an intermittent issue kept open'
