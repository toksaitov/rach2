#!/bin/sh
# ci_run_test.sh: check how tools/ci/run.sh reads what tests/run gives. From
# the runner's lines: the suites it failed, the set's name when it names
# none, and nothing from the lines that the runner suite's own runs print.
# From etude's history: core run as one run of style, host and the core
# tier's suites of the registry, its history left in DIR/history and
# DIR/store, where history_failures, report.sh's reader, reads it as well; a
# pass only when casecheck's verdict and tests/run's status both pass; the
# suites the verdict fails, a failure or an unfinished attempt before a retry
# that passed among them; and `runner`, once, for a history that is missing,
# cut off or inconsistent, a verdict that gave no result, and a status that
# disagrees with the verdict. run.sh runs in a private tree with stand-ins
# for git, make and tests/run, and with the built casecheck over the history
# fixtures of tools/etude/fixtures/verdict.
#
# Usage: sh tests/system/ci_run_test.sh [--mutate | --texts]
#
# --mutate puts each fault of `fault` in a private copy of run.sh and
# requires a row to fail with it, then runs the rows again on the true copy;
# --texts checks that each fault's text is in run.sh, and runs nothing.

set -eu
case ${1:-} in
    '') mode=check ;;
    --mutate) mode=mutate ;;
    --texts) mode=texts ;;
    *) echo 'ci-run-test: usage: ci_run_test.sh [--mutate | --texts]' >&2; exit 2 ;;
esac
repo=$(CDPATH='' cd -- "$(dirname -- "$0")/../.." && pwd)
fixtures=$repo/tools/etude/fixtures/verdict
work=
trap '[ -z "$work" ] || rm -rf "$work"' EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM

# fault NAME: write the private run.sh with the fault NAME, or with none when
# NAME is empty, refusing a fault whose text moved.
fault()
{
    case ${1:-} in
        '')
            cp "$repo/tools/ci/run.sh" "$work/run.sh"
            return 0 ;;
        status)
            # shellcheck disable=SC2016 # the script contains a literal $rc
            sed 's/\[ "\$rc" != 0 \] || return 0/return 0/' "$repo/tools/ci/run.sh" > "$work/run.sh" ;;
        verdict)
            # shellcheck disable=SC2016 # the script contains a literal $verdict
            sed 's/if \[ "\$verdict" = 0 \] && grep -qx /if [ "$verdict" != 1 ] || grep -qx /' \
                "$repo/tools/ci/run.sh" > "$work/run.sh" ;;
        agree)
            # shellcheck disable=SC2016 # the script contains a literal $rc
            sed 's/ || \[ "\$rc" = 0 \]; then$/; then/' "$repo/tools/ci/run.sh" > "$work/run.sh" ;;
        nested)
            sed 's/for (i = last + 1; i <= NR; i++)/for (i = 1; i <= NR; i++)/' \
                "$repo/tools/ci/run.sh" > "$work/run.sh" ;;
        history)
            # shellcheck disable=SC2016 # the script contains a literal $results
            sed 's/if \[ "\$results" = history \]; then/if false; then/' \
                "$repo/tools/ci/run.sh" > "$work/run.sh" ;;
        tier)
            # shellcheck disable=SC2016 # the script contains a literal $3
            sed 's/\$3 == "core" && //' "$repo/tools/ci/run.sh" > "$work/run.sh" ;;
        once)
            # shellcheck disable=SC2016 # the script contains a literal $f
            sed 's/grep -qxF "\$f" "\$out\/failed" || echo/echo/' \
                "$repo/tools/ci/run.sh" > "$work/run.sh" ;;
        place)
            # shellcheck disable=SC2016 # the script contains a literal $out
            sed 's/RACH2_RESULTS=\$out tests\/run/RACH2_RESULTS=$out\/results tests\/run/' \
                "$repo/tools/ci/run.sh" > "$work/run.sh" ;;
        onerun)
            sed 's/run_history core style host core$/run_history core core/' \
                "$repo/tools/ci/run.sh" > "$work/run.sh" ;;
        *) return 1 ;;
    esac
    ! cmp -s "$repo/tools/ci/run.sh" "$work/run.sh"
}

# install lines|history: put the private run.sh in the tree, reading results
# as the word says.
install()
{
    if [ "$1" = lines ]; then
        cp "$work/run.sh" "$work/tree/tools/ci/run.sh"
        return
    fi
    sed 's/^results=lines$/results=history/' "$work/run.sh" > "$work/tree/tools/ci/run.sh"
    ! cmp -s "$work/run.sh" "$work/tree/tools/ci/run.sh"
}

faults='status verdict agree nested history tier once place onerun'
if [ "$mode" = texts ]; then
    work=$(mktemp -d "${TMPDIR:-/tmp}/rach2-ci-run-test.XXXXXX")
    mkdir -p "$work/tree/tools/ci"
    for name in $faults; do
        fault "$name" || { echo "ci-run-test: the text of the fault $name is not in tools/ci/run.sh" >&2; exit 1; }
    done
    fault ''
    install history || { echo 'ci-run-test: tools/ci/run.sh has no line results=lines' >&2; exit 1; }
    exit 0
fi

casecheck=$repo/build/tools/etude/casecheck
failures=$repo/build/tools/etude/history_failures
for program in "$casecheck" "$failures"; do
    [ -x "$program" ] || { echo "ci-run-test: $program is not built; make builds it" >&2; exit 1; }
done
work=$(mktemp -d "${TMPDIR:-/tmp}/rach2-ci-run-test.XXXXXX")
tree=$work/tree
mkdir -p "$work/bin" "$tree/tools/ci" "$tree/tools/etude/schemas" "$tree/tests/cases" \
    "$tree/build/tools/etude"
cp "$repo/tools/etude/schemas/case.schema.json" "$repo/tools/etude/schemas/results.schema.json" \
    "$repo/tools/etude/schemas/history.schema.json" "$tree/tools/etude/schemas/"
cp "$fixtures/cases/fixture.default.json" "$fixtures/cases/fixture.host.json" "$tree/tests/cases/"
cp "$casecheck" "$tree/build/tools/etude/casecheck"
cat > "$tree/tests/registry" <<'EOF'
style|-|full
host|-|full
default|qemu|core
shutdown|qemu|core
tcp|qemu|full
fpu|qemu|core|||x86_64
EOF

cat > "$work/bin/git" <<'EOF'
#!/bin/sh
case $* in
    'rev-parse --short HEAD') echo 0123456 ;;
    'rev-parse HEAD') echo 0123456789abcdef0123456789abcdef01234567 ;;
    *) echo "git stand-in: $*" >&2; exit 2 ;;
esac
EOF
cat > "$work/bin/make" <<'EOF'
#!/bin/sh
exit "${CI_RUN_TEST_MAKE:-0}"
EOF
# The stand-in tests/run: a set whose first suite is not CI_RUN_TEST_SET
# passes and writes no history; that set prints CI_RUN_TEST_PRINT, copies the
# history streams CI_RUN_TEST_STREAMS names into RACH2_RESULTS/history and
# the fixtures' store into RACH2_RESULTS/store, which run.sh makes, and
# exits CI_RUN_TEST_STATUS.
cat > "$tree/tests/run" <<'EOF'
#!/bin/sh
echo "tests/run $* with ${RACH2_RESULTS:-no results}" >> "$CI_RUN_TEST_CALLS"
if [ "$1" != "$CI_RUN_TEST_SET" ]; then
    echo "run: $1: PASS in 1s"
    echo 'run: result: passed'
    exit 0
fi
printf '%b' "$CI_RUN_TEST_PRINT"
for s in $CI_RUN_TEST_STREAMS; do
    cp "$CI_RUN_TEST_FIXTURES/$s" "$RACH2_RESULTS/history/" || exit 3
done
[ -z "$CI_RUN_TEST_STREAMS" ] || cp "$CI_RUN_TEST_FIXTURES"/store/* "$RACH2_RESULTS/store/" || exit 3
exit "$CI_RUN_TEST_STATUS"
EOF
chmod +x "$work/bin/git" "$work/bin/make" "$tree/tests/run"

pass='pass/plan.jsonl pass/item-d0.jsonl pass/item-d1.jsonl pass/item-h.jsonl pass/run.jsonl'
failure='pass/plan.jsonl item-d0-failure.jsonl pass/item-d1.jsonl pass/item-h.jsonl run-failure.jsonl'
retry='pass/plan.jsonl item-d0-retry.jsonl pass/item-d1.jsonl pass/item-h.jsonl run-retry.jsonl'
unfinished='pass/plan.jsonl item-d0-unfinished.jsonl pass/item-d1.jsonl pass/item-h.jsonl run-unfinished.jsonl'
lost='pass/plan.jsonl pass/item-d0.jsonl item-d1-lost.jsonl pass/item-h.jsonl pass/run.jsonl'
rows=0
streams=
print=
make_status=0
suites=

# row NAME MODE KIND SET STATUS FAILED: run run.sh KIND, reading results as
# MODE says, with the set SET of tests/run exiting STATUS, and require the
# lines of DIR/failed to be the words FAILED, `done` written, and run.sh's
# status 0 for no failure and 1 for any. The stand-in tests/run prints
# `print` and copies the streams `streams` names, make exits `make_status`,
# and SUITES is `suites`; row then empties `print` and `streams` and sets
# `make_status` to 0.
row()
{
    rows=$((rows + 1))
    out=$work/out.$rows
    install "$2" || { echo "ci-run-test: $1: tools/ci/run.sh has no line results=lines" >&2; return 1; }
    : > "$work/calls"
    (
        cd "$tree" &&
            PATH="$work/bin:$PATH" ARCH=i686 SUITES=$suites CI_RUN_TEST_SET=$4 \
                CI_RUN_TEST_STATUS=$5 CI_RUN_TEST_PRINT=$print CI_RUN_TEST_STREAMS=$streams \
                CI_RUN_TEST_MAKE=$make_status CI_RUN_TEST_FIXTURES=$fixtures \
                CI_RUN_TEST_CALLS=$work/calls sh tools/ci/run.sh "$3" "$out"
    ) > "$work/row.log" 2>&1
    status=$?
    print=
    streams=
    make_status=0
    got=$(tr '\n' ' ' < "$out/failed" | sed 's/ $//')
    want=0
    [ -z "$6" ] || want=1
    if [ "$got" = "$6" ] && [ -f "$out/done" ] && [ "$status" -eq "$want" ]; then
        return 0
    fi
    sed 's/^/ci-run-test: log: /' "$work/row.log" "$work/calls" >&2
    echo "ci-run-test: $1: failed '$got', status $status; expected '$6', status $want" >&2
    return 1
}

# The runner's lines, as run.sh reads them today.
lines_rows()
{
    suites=
    row 'lines: a pass' lines core core 0 '' || return 1
    print='run: default: FAIL in 3s\nrun: result: failed: default\n'
    row 'lines: a failed suite' lines core core 1 default || return 1
    print='run: default: PASS in 3s\nrun: result: failed: nothing-such\n'
    row 'lines: no suite named' lines core core 1 core || return 1
    print='run: result: failed: default\nrun: runner: PASS in 9s\n'
    row "lines: the runner suite's own lines" lines core core 1 core || return 1
    make_status=1
    row 'lines: the build' lines core core 0 build || return 1
}

# agrees STATUS: require history_failures, which report.sh reads a run's
# history with, to give STATUS for the history and store the last row left in
# DIR: 0 when every attempt passed and 1 when one did not.
agrees()
{
    if "$failures" -s "$repo/tools/etude/schemas" -b "$out/store" "$out"/history/* \
        > "$work/failures.log" 2>&1; then
        got=0
    else
        got=$?
    fi
    [ "$got" -ne "$1" ] || return 0
    sed 's/^/ci-run-test: history_failures: /' "$work/failures.log" >&2
    echo "ci-run-test: history_failures gave $got for the history of row $rows, not $1" >&2
    return 1
}

# etude's history, judged by casecheck.
history_rows()
{
    suites='i686:default i686:host'
    streams=$pass
    row 'history: a pass' history full default 0 '' || return 1
    grep -q '^tests/run default host with .*/out\.[0-9]*$' "$work/calls" ||
        { echo 'ci-run-test: tests/run was not given DIR as RACH2_RESULTS' >&2; return 1; }
    agrees 0 || return 1
    streams=$pass
    row 'history: a pass with a failed status' history full default 1 runner || return 1
    row 'history: no history' history full default 0 runner || return 1
    streams=$failure
    row 'history: a failed item' history full default 1 default || return 1
    agrees 1 || return 1
    streams=$failure
    row 'history: a failed item with a passed status' history full default 0 'runner default' ||
        return 1
    streams=$retry
    row 'history: a failure before a retry that passed' history full default 1 default || return 1
    agrees 1 || return 1
    streams=$unfinished
    row 'history: an unfinished attempt before a retry that passed' history full default 1 default ||
        return 1
    agrees 1 || return 1
    streams=$lost
    row 'history: a cut-off stream' history full default 1 'runner default' || return 1
    suites='i686:default i686:host i686:shutdown'
    streams=$pass
    row 'history: a suite left out' history full default 0 'runner shutdown' || return 1
    suites='i686:default i686:host'
    rm "$tree/build/tools/etude/casecheck"
    streams=$pass
    row 'history: no verdict' history full default 0 runner
    status=$?
    cp "$casecheck" "$tree/build/tools/etude/casecheck"
    [ "$status" -eq 0 ] || return 1
    suites=
    streams=$pass
    row 'history: style, host and the core tier as one run' history core style 0 \
        'runner style shutdown' || return 1
    grep -q '^tests/run style host core with .*/out\.[0-9]*$' "$work/calls" ||
        { echo 'ci-run-test: core did not run as one run of style, host and core' >&2; return 1; }
}

all_rows()
{
    fault ''
    lines_rows && history_rows
}

all_rows || { echo 'ci-run-test: result: fail: ci reading of results' >&2; exit 1; }
checked=$rows
if [ "$mode" = mutate ]; then
    for name in $faults; do
        fault "$name" || { echo "ci-run-test: the text of the fault $name is not in tools/ci/run.sh" >&2; exit 1; }
        case $name in
            nested) check=lines_rows ;;
            *) check=history_rows ;;
        esac
        if "$check" 2>/dev/null; then
            echo "ci-run-test: result: fail: the fault $name passed" >&2
            exit 1
        fi
        echo "ci-run-test: the fault $name failed a row"
    done
    all_rows || { echo 'ci-run-test: result: fail: the rows after the faults' >&2; exit 1; }
fi
echo "ci-run-test: result: pass: $checked rows: the runner's lines, the build, a passing and a failing history, a failure or an unfinished attempt before a retry that passed, a status that disagrees, a missing or cut-off history, a suite left out, a missing verdict and the core set as one run"
