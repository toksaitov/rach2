#!/bin/sh
# ci_test.sh: check unfinished ci reports, the labels and body of a filed
# issue, a full run's last pass, a failure repeated on an intermittent issue,
# the hold issue, bisection runs, their time limits and continuation, the run
# whose issue bisection labels intermittent, a report and a bisection that
# overlap, the kind an older commit's run.sh takes, a refused run in
# bisection, and verdict errors. The scripts
# run in a private tree with stand-ins for git, gh, make, timeout and the
# clock.

set -eu
case ${1:-} in
    '') mode=check ;;
    --mutate) mode=mutate ;;
    --texts) mode=texts ;;
    *) echo 'ci-test: usage: ci_test.sh [--mutate | --texts]' >&2; exit 2 ;;
esac
repo=$(CDPATH='' cd -- "$(dirname -- "$0")/../.." && pwd)
work=$(mktemp -d "${TMPDIR:-/tmp}/ci-test.XXXXXX")
trap 'rm -rf "$work"' EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM
mkdir -p "$work/bin" "$work/tree/tools/ci" "$work/tree/tests"
cp "$repo/tools/ci/report.sh" "$work/report.sh"
cp "$repo/tools/ci/bisect.sh" "$work/bisect.sh"

# fault NAME: change a private script, refusing a fault whose text moved.
fault()
{
    case $1 in
        report)
            # shellcheck disable=SC2016 # the script contains a literal $kind
            sed 's/items=unfinished-\$kind/items=runner/' "$repo/tools/ci/report.sh" > "$work/report.sh"
            ! cmp -s "$repo/tools/ci/report.sh" "$work/report.sh" ;;
        cilabel)
            sed 's/ --label bug --label ci / --label bug /' "$repo/tools/ci/report.sh" > "$work/report.sh"
            ! cmp -s "$repo/tools/ci/report.sh" "$work/report.sh" ;;
        baseline)
            sed 's|contexts="ci/full ci/night"|contexts=ci/full|' "$repo/tools/ci/report.sh" > "$work/report.sh"
            ! cmp -s "$repo/tools/ci/report.sh" "$work/report.sh" ;;
        again)
            sed 's/\*" intermittent "\*)/*" never "*)/' "$repo/tools/ci/report.sh" > "$work/report.sh"
            ! cmp -s "$repo/tools/ci/report.sh" "$work/report.sh" ;;
        reread)
            # shellcheck disable=SC2016 # the script contains $found and $now
            sed 's/case " $found $now " in/case " $found " in/' "$repo/tools/ci/report.sh" > "$work/report.sh"
            ! cmp -s "$repo/tools/ci/report.sh" "$work/report.sh" ;;
        hold)
            # shellcheck disable=SC2016 # the script contains $(open_holds)
            sed 's/holds=$(open_holds)/holds=$(open_issue hold "$hold_title")/' "$repo/tools/ci/report.sh" > "$work/report.sh"
            ! cmp -s "$repo/tools/ci/report.sh" "$work/report.sh" ;;
        bisect)
            sed 's/run_kind=core ;;/test_items=runner ;;/' "$repo/tools/ci/bisect.sh" > "$work/bisect.sh"
            ! cmp -s "$repo/tools/ci/bisect.sh" "$work/bisect.sh" ;;
        items)
            sed 's/core) test_set=core ;;/core) test_set=core; test_items="" ;;/' "$repo/tools/ci/bisect.sh" > "$work/bisect.sh"
            ! cmp -s "$repo/tools/ci/bisect.sh" "$work/bisect.sh" ;;
        comment)
            sed '/the comment on/s/return 1/return 0/' "$repo/tools/ci/bisect.sh" > "$work/bisect.sh"
            ! cmp -s "$repo/tools/ci/bisect.sh" "$work/bisect.sh" ;;
        label)
            sed '/the label on/s/status=1/status=0/' "$repo/tools/ci/bisect.sh" > "$work/bisect.sh"
            ! cmp -s "$repo/tools/ci/bisect.sh" "$work/bisect.sh" ;;
        intermittent)
            sed '/say .*passed .*when run again/s/|| status=1/|| :/' "$repo/tools/ci/bisect.sh" > "$work/bisect.sh"
            ! cmp -s "$repo/tools/ci/bisect.sh" "$work/bisect.sh" ;;
        newest)
            # shellcheck disable=SC2016 # the script contains $newest and $run
            sed 's/"$newest" != "$run"/-z "$newest"/' "$repo/tools/ci/bisect.sh" > "$work/bisect.sh"
            ! cmp -s "$repo/tools/ci/bisect.sh" "$work/bisect.sh" ;;
        recheck)
            sed 's/newest_or_exit labelled/:/' "$repo/tools/ci/bisect.sh" > "$work/bisect.sh"
            ! cmp -s "$repo/tools/ci/bisect.sh" "$work/bisect.sh" ;;
        older)
            sed 's/|| here=night/|| :/' "$repo/tools/ci/bisect.sh" > "$work/bisect.sh"
            ! cmp -s "$repo/tools/ci/bisect.sh" "$work/bisect.sh" ;;
        refusal)
            sed "s|'1/run: result: failed')|[1-9]*/*)|" "$repo/tools/ci/bisect.sh" > "$work/bisect.sh"
            ! cmp -s "$repo/tools/ci/bisect.sh" "$work/bisect.sh" ;;
        limit)
            sed 's/if limited sh tools/if sh tools/' "$repo/tools/ci/bisect.sh" > "$work/bisect.sh"
            ! cmp -s "$repo/tools/ci/bisect.sh" "$work/bisect.sh" ;;
        handoff)
            sed 's/fits 2 || handoff/: || handoff/' "$repo/tools/ci/bisect.sh" > "$work/bisect.sh"
            ! cmp -s "$repo/tools/ci/bisect.sh" "$work/bisect.sh" ;;
    esac
}
if [ "$mode" = texts ]; then
    for name in report cilabel baseline again reread hold bisect items comment \
        label intermittent newest recheck older refusal limit handoff; do
        fault "$name"
    done
    exit 0
fi

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
    'api '*) ;;
    'issue list')
        case "$*" in
            *'--label hold '*'--json number --jq '*)
                [ -z "${CI_TEST_HOLD:-}" ] || printf '%s\n' $CI_TEST_HOLD ;;
            *'--label bug '*'--json number,title,labels '*"select(.title == \"${CI_TEST_OPEN_TITLE:-}\")"*)
                echo "$CI_TEST_OPEN" ;;
        esac ;;
    'issue view')
        case "$*" in
            *' --json labels '*)
                echo "$CI_TEST_LABELS"
                exit 0 ;;
            *' --json body,comments '*) ;;
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
PATH=$work/bin:$PATH
export CI_TEST_WORK GITHUB_REPOSITORY GITHUB_SERVER_URL GITHUB_RUN_ID PATH
cd "$work/tree"

# clear: drop the passes, the open issue, its labels and the runs that rows
# give the gh stand-in.
clear()
{
    CI_TEST_BASE='' CI_TEST_OPEN='' CI_TEST_OPEN_TITLE='' CI_TEST_LABELS=''
    CI_TEST_RUNS='' CI_TEST_RUNS_LABELLED=''
    export CI_TEST_BASE CI_TEST_OPEN CI_TEST_OPEN_TITLE CI_TEST_LABELS
    export CI_TEST_RUNS CI_TEST_RUNS_LABELLED
}

# results KIND STATE: report a run of KIND whose x86_64 half passed and
# whose i686 half ended in STATE.
results()
{
    kind=$1 state=$2
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
    esac
    : > "$work/gh.log"
    sh "$work/report.sh" "$kind" bad "$work/results" > "$work/report.log" 2>&1
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

# base: with only nightly passes on GOOD, require a full report to bisect
# from GOOD as a full run, with this run's ID, and a core report to find no
# last pass.
base()
{
    clear
    CI_TEST_BASE=night
    results full runner || return 1
    grep -q 'workflow run bisect.yml .* -f kind=full -f arch=i686 -f good=good -f bad=bad -f items=runner -f issue=7 -f run=1' "$work/gh.log" || return 1
    results core runner || return 1
    ! grep -q 'workflow run' "$work/gh.log" || return 1
    grep -q 'unknown: no commit in the 50 before bad' "$work/gh.log" || return 1
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
    CI_TEST_OPEN_TITLE='ci: core: runner fails on i686' CI_TEST_OPEN="12 $1"
    CI_TEST_LABELS=${2-$1}
    results core runner || return 1
    grep -q '^issue comment 12 ' "$work/gh.log" || return 1
    ! grep -q '^issue create .* --title ci: ' "$work/gh.log" || return 1
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
        grep -q 'fails .* and its parent good passes' "$work/gh.log" || return 1
    fi
}

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
            grep -q "a newer failure, run ${2##* }, was reported since run 1" "$work/gh.log" ;;
        *)
            ! grep -q -- '--add-label intermittent' "$work/gh.log" || return 1
            grep -q "a newer failure, run ${1##* }, was reported since run 1" "$work/gh.log" ;;
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
    grep -q 'fails .* and its parent good passes' "$work/gh.log" || return 1
    start
    status=0
    sh "$work/bisect.sh" -m 100 core i686 good bad 7 unfinished-core > "$work/bisect.log" 2>&1 || status=$?
    [ "$status" = 1 ] && [ ! -s "$work/events" ] || return 1
    grep -q "unresolved: this run's 100 minutes" "$work/gh.log" || return 1
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
              verdict='fails .* and its parent good passes' want=0 ;;
        refuse) printf 'make\ntests runner bad\n' > "$work/expected"
                verdict='unresolved: bad could not be tested again' want=1 ;;
    esac
    [ "$status" = "$want" ] || return 1
    cmp -s "$work/expected" "$work/events" || return 1
    grep -q -- "$verdict" "$work/gh.log"
}

# rows: check both unfinished kinds, the filed issue and the hold issue,
# preserve an actual runner failure, take a nightly pass as a full run's
# last pass, bisect a repeated intermittent failure again, run the core set
# beside the other items of a core run that failed, take an issue's newest
# run before labelling it intermittent and again after, read an issue's
# labels again after reporting on it, repeat an unfinished full run as
# `night` where run.sh knows only that, continue a bisection in a new run,
# and count only a reported failure of the runner suite as one.
rows()
{
    for kind in core full; do
        for state in missing unfinished missing-failed; do
            report "$kind" "$state" "unfinished-$kind" || return 1
        done
        report "$kind" runner runner || return 1
        bisect "unfinished-$kind" "$kind" 0 || return 1
        bisect "unfinished-$kind" "$kind" 1 || return 1
    done
    bisect unfinished-night full 0 || return 1
    older || return 1
    base || return 1
    again 'bug ci intermittent' || return 1
    again 'bug ci' || return 1
    again 'bug ci' 'bug ci intermittent' || return 1
    hold fail || return 1
    hold pass || return 1
    bisect 'style core' 'tests style bad\ntests core bad' 0 || return 1
    bisect unfinished-core core 1 comment || return 1
    bisect unfinished-core core 0 comment || return 1
    bisect unfinished-core core 0 edit || return 1
    latest 1 || return 1
    latest '1 2' || return 1
    latest 1 '1 2' || return 1
    handoff || return 1
    for result in pass fail refuse; do
        suite "$result" || return 1
    done
}

if ! rows; then
    cat "$work/report.log" "$work/bisect.log" 2>/dev/null || :
    echo 'ci-test: result: fail: ci report, hold or bisection' >&2
    exit 1
fi
if [ "$mode" = mutate ]; then
    fault report
    if report core unfinished unfinished-core; then
        echo 'ci-test: result: fail: report fault passed' >&2; exit 1
    fi
    fault cilabel
    if report core unfinished unfinished-core; then
        echo 'ci-test: result: fail: cilabel fault passed' >&2; exit 1
    fi
    fault baseline
    if base; then
        echo 'ci-test: result: fail: baseline fault passed' >&2; exit 1
    fi
    fault again
    if again 'bug ci intermittent'; then
        echo 'ci-test: result: fail: again fault passed' >&2; exit 1
    fi
    fault reread
    if again 'bug ci' 'bug ci intermittent'; then
        echo 'ci-test: result: fail: reread fault passed' >&2; exit 1
    fi
    fault hold
    if hold fail || hold pass; then
        echo 'ci-test: result: fail: hold fault passed' >&2; exit 1
    fi
    cp "$repo/tools/ci/report.sh" "$work/report.sh"
    fault bisect
    if bisect unfinished-core core 0; then
        echo 'ci-test: result: fail: bisection fault passed' >&2; exit 1
    fi
    fault items
    if bisect 'style core' 'tests style bad\ntests core bad' 0; then
        echo 'ci-test: result: fail: core items fault passed' >&2; exit 1
    fi
    cp "$repo/tools/ci/bisect.sh" "$work/bisect.sh"
    for name in comment label intermittent; do
        fault "$name"
        case $name in
            comment) fail=1; gh_fail=comment ;;
            label) fail=0; gh_fail=edit ;;
            intermittent) fail=0; gh_fail=comment ;;
        esac
        if bisect unfinished-core core "$fail" "$gh_fail"; then
            echo "ci-test: result: fail: $name fault passed" >&2; exit 1
        fi
        cp "$repo/tools/ci/bisect.sh" "$work/bisect.sh"
    done
    fault newest
    if latest '1 2'; then
        echo 'ci-test: result: fail: newest fault passed' >&2; exit 1
    fi
    fault recheck
    if latest 1 '1 2'; then
        echo 'ci-test: result: fail: recheck fault passed' >&2; exit 1
    fi
    fault older
    if older; then
        echo 'ci-test: result: fail: older fault passed' >&2; exit 1
    fi
    fault refusal
    if suite refuse; then
        echo 'ci-test: result: fail: refusal fault passed' >&2; exit 1
    fi
    fault limit
    if bisect unfinished-core core 1; then
        echo 'ci-test: result: fail: limit fault passed' >&2; exit 1
    fi
    fault handoff
    if handoff; then
        echo 'ci-test: result: fail: handoff fault passed' >&2; exit 1
    fi
    cp "$repo/tools/ci/report.sh" "$work/report.sh"
    cp "$repo/tools/ci/bisect.sh" "$work/bisect.sh"
    rows
fi
echo 'ci-test: result: pass: unfinished runs, issue labels, the last pass, repeated failures, the hold issue, the runner suite, core items, time limits, continuation, the newest run, overlapping reports, older run scripts, a refused run and verdict errors'
