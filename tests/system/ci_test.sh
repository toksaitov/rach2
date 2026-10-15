#!/bin/sh
# ci_test.sh: check unfinished ci reports and the runs bisection repeats.
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
        bisect)
            sed 's/run_kind=core; test_items=""/run_kind=""; test_items=runner/' "$repo/tools/ci/bisect.sh" > "$work/bisect.sh"
            ! cmp -s "$repo/tools/ci/bisect.sh" "$work/bisect.sh" ;;
    esac
}
if [ "$mode" = texts ]; then
    fault report
    fault bisect
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
    'issue list') ;;
    'issue create') echo https://example.invalid/issues/7 ;;
    'issue comment'|'issue edit'|'issue close'|'workflow run') ;;
    *) exit 2 ;;
esac
EOF
cat > "$work/bin/make" <<'EOF'
#!/bin/sh
echo make >> "$CI_TEST_WORK/events"
EOF
cat > "$work/tree/tests/run" <<'EOF'
#!/bin/sh
echo "tests $*" >> "$CI_TEST_WORK/events"
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

# bisect ITEM COMMAND FAIL: check the selected command and repeated verdicts.
bisect()
{
    item=$1 command=$2 CI_TEST_FAIL=$3
    export CI_TEST_FAIL
    RUNNER_TEMP=$work/logs
    export RUNNER_TEMP
    rm -rf "$RUNNER_TEMP"
    : > "$work/events"
    : > "$work/gh.log"
    sh "$work/bisect.sh" i686 good bad 7 "$item" > "$work/bisect.log" 2>&1 || return 1
    if [ "$item" = runner ]; then
        printf 'make\ntests runner\n' > "$work/expected"
    elif [ "$CI_TEST_FAIL" = 0 ]; then
        printf 'run %s bad\n' "$command" > "$work/expected"
    else
        printf 'run %s bad\nrun %s bad\nrun %s good\n' "$command" "$command" "$command" > "$work/expected"
    fi
    cmp -s "$work/expected" "$work/events" || return 1
    if [ "$CI_TEST_FAIL" = 0 ]; then
        grep -q -- '--add-label intermittent' "$work/gh.log" || return 1
    else
        grep -q 'fails .* and its parent good passes' "$work/gh.log" || return 1
    fi
}

# rows: check both unfinished kinds and preserve an actual runner failure.
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
    bisect runner runner 0
}

if ! rows; then
    cat "$work/report.log" "$work/bisect.log" 2>/dev/null || :
    echo 'ci-test: result: fail: ci report or bisection' >&2
    exit 1
fi
if [ "$mode" = mutate ]; then
    fault report
    if report core unfinished unfinished-core; then
        echo 'ci-test: result: fail: report fault passed' >&2; exit 1
    fi
    cp "$repo/tools/ci/report.sh" "$work/report.sh"
    fault bisect
    if bisect unfinished-core core 0; then
        echo 'ci-test: result: fail: bisection fault passed' >&2; exit 1
    fi
    cp "$repo/tools/ci/bisect.sh" "$work/bisect.sh"
    rows
fi
echo 'ci-test: result: pass: unfinished runs and the runner suite'
