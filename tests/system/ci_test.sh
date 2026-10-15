#!/bin/sh
# ci_test.sh: check unfinished ci reports, the hold issue, bisection runs,
# a refused run in bisection, and verdict errors.
# The scripts run in a private tree with stand-ins for git, gh and make.

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
        refusal)
            sed "s|'1/run: result: failed')|[1-9]*/*)|" "$repo/tools/ci/bisect.sh" > "$work/bisect.sh"
            ! cmp -s "$repo/tools/ci/bisect.sh" "$work/bisect.sh" ;;
    esac
}
if [ "$mode" = texts ]; then
    fault report
    fault hold
    fault bisect
    fault items
    fault comment
    fault label
    fault intermittent
    fault refusal
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
    'api repos/test/rach2/commits/good/status') echo success ;;
    'api '*) ;;
    'issue list')
        case "$*" in
            *'--label hold '*'--json number --jq '*)
                [ -z "${CI_TEST_HOLD:-}" ] || printf '%s\n' $CI_TEST_HOLD ;;
        esac ;;
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
cat > "$work/tree/tools/ci/run.sh" <<'EOF'
#!/bin/sh
set -eu
current=$(cat "$CI_TEST_WORK/current")
echo "run $1 $current" >> "$CI_TEST_WORK/events"
[ "$CI_TEST_FAIL" = 0 ] || [ "$current" = good ]
EOF
chmod +x "$work/bin/"* "$work/tree/tests/run"
CI_TEST_WORK=$work
GITHUB_REPOSITORY=test/rach2
GITHUB_SERVER_URL=https://example.invalid
GITHUB_RUN_ID=1
PATH=$work/bin:$PATH
export CI_TEST_WORK GITHUB_REPOSITORY GITHUB_SERVER_URL GITHUB_RUN_ID PATH
cd "$work/tree"

# report KIND STATE ITEM: require the dispatched item and the core hold.
report()
{
    kind=$1 state=$2 item=$3
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
    sh "$work/report.sh" "$kind" bad "$work/results" > "$work/report.log" 2>&1 || return 1
    grep -q "workflow run bisect.yml .* -f items=$item -f issue=7" "$work/gh.log" || return 1
    if [ "$kind" = core ]; then
        grep -q 'issue create .* --label hold ' "$work/gh.log" || return 1
    else
        ! grep -q 'issue create .* --label hold ' "$work/gh.log" || return 1
    fi
}

# hold STATE: with holds #5 and #9 open under any title, require a core run
# that fails to boot to comment on #5 alone and open no hold, and one that
# passes to close both.
hold()
{
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

# bisect ITEM COMMAND FAIL [GH-FAIL]: check runs and verdict delivery.
# For an item that is not an unfinished run, COMMAND is the tests/run lines
# expected after the build, and the stand-in runner always passes.
bisect()
{
    item=$1 command=$2 CI_TEST_FAIL=$3 CI_TEST_GH_FAIL=${4:-} CI_TEST_RUN=pass
    export CI_TEST_FAIL CI_TEST_GH_FAIL CI_TEST_RUN
    RUNNER_TEMP=$work/logs
    export RUNNER_TEMP
    rm -rf "$RUNNER_TEMP"
    : > "$work/events"
    : > "$work/gh.log"
    status=0
    sh "$work/bisect.sh" i686 good bad 7 "$item" > "$work/bisect.log" 2>&1 || status=$?
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
    if [ "$CI_TEST_FAIL" = 0 ]; then
        grep -q -- '--add-label intermittent' "$work/gh.log" || return 1
    else
        grep -q 'fails .* and its parent good passes' "$work/gh.log" || return 1
    fi
}

# suite RESULT: bisect the runner suite when tests/run on BAD passes, fails,
# or refuses, and check that only a failure it reports is counted as one.
suite()
{
    CI_TEST_RUN=$1 CI_TEST_GH_FAIL=
    RUNNER_TEMP=$work/logs
    export CI_TEST_RUN CI_TEST_GH_FAIL RUNNER_TEMP
    rm -rf "$RUNNER_TEMP"
    : > "$work/events"
    : > "$work/gh.log"
    status=0
    sh "$work/bisect.sh" i686 good bad 7 runner > "$work/bisect.log" 2>&1 || status=$?
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

# rows: check both unfinished kinds and the hold issue, preserve an actual
# runner failure, run the core set beside the other items of a core run that
# failed, and count only a reported failure of the runner suite as one.
rows()
{
    for kind in core night; do
        for state in missing unfinished missing-failed; do
            report "$kind" "$state" "unfinished-$kind" || return 1
        done
        report "$kind" runner runner || return 1
        bisect "unfinished-$kind" "$kind" 0 || return 1
        bisect "unfinished-$kind" "$kind" 1 || return 1
    done
    hold fail || return 1
    hold pass || return 1
    bisect 'style core' 'tests style bad\ntests core bad' 0 || return 1
    bisect unfinished-core core 1 comment || return 1
    bisect unfinished-core core 0 comment || return 1
    bisect unfinished-core core 0 edit || return 1
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
    cp "$repo/tools/ci/report.sh" "$work/report.sh"
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
    fault refusal
    if suite refuse; then
        echo 'ci-test: result: fail: refusal fault passed' >&2; exit 1
    fi
    cp "$repo/tools/ci/bisect.sh" "$work/bisect.sh"
    rows
fi
echo 'ci-test: result: pass: unfinished runs, the hold issue, the runner suite, core items, a refused run and verdict errors'
