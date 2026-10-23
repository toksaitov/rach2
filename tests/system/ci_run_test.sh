#!/bin/sh
# ci_run_test.sh: check how tools/ci/run.sh reads what tests/run gives. From
# the runner's lines: the suites it failed, whichever of its standard output
# and standard error came first, the set's name when it names none, and
# nothing from the lines that the runner suite's own runs print.
# From etude's history: core run as one run of style, host and the core
# tier's suites of the registry, its history left in DIR/history and
# DIR/store, where etude show failures, report.sh's reader, reads it as well; a
# pass only when etude check verdict's verdict and tests/run's status both pass; the
# suites the verdict fails, a failure or an unfinished attempt before a retry
# that passed among them; and `runner`, once, for a history that is missing,
# cut off or inconsistent, a verdict that gave no result, and a status that
# disagrees with the verdict. In both, DIR/err has tests/run's standard
# error and none of its standard output. When a step is killed as GitHub's
# cleanup of a cancelled job kills it, or the run's parent is, run.sh starts
# no further step and ends without `done`. With both architectures,
# `ARCH='i686 x86_64'`, each set is one run of tests/run on both, of the
# chosen `ARCH:SUITE` words or of every suite of either, with `bigmem` alone
# after it; DIR/failed gives each name as `ARCH:NAME`, an architecture whose
# build failed is not tested, and DIR/KIND-ARCH has each architecture's
# failures, standard error and kept directories, `skipped` for one that
# SUITES names nothing of, and `done` only when the run was not stopped.
# run.sh runs in a private tree with
# stand-ins for git, make and tests/run, and with the built etude check verdict over
# the history fixtures of tools/etude/fixtures/verdict.
#
# Usage: sh tests/system/ci_run_test.sh

set -eu
[ "$#" -eq 0 ] || { echo 'ci-run-test: usage: ci_run_test.sh' >&2; exit 2; }
repo=$(CDPATH='' cd -- "$(dirname -- "$0")/../.." && pwd)
run_sh=$repo/tools/ci/run.sh
fixtures=$repo/tools/etude/fixtures/verdict
work=
trap '[ -z "$work" ] || rm -rf "$work"' EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM

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

etude=$repo/build/tools/etude/etude
[ -x "$etude" ] || { echo "ci-run-test: $etude is not built; make builds it" >&2; exit 1; }
work=$(mktemp -d "${TMPDIR:-/tmp}/rach2-ci-run-test.XXXXXX")
tree=$work/tree
mkdir -p "$work/bin" "$tree/tools/ci" "$tree/tools/etude/schemas" "$tree/tests/cases" \
    "$tree/build/tools/etude"
cp "$repo/tools/etude/schemas/case.schema.json" "$repo/tools/etude/schemas/results.schema.json" \
    "$repo/tools/etude/schemas/history.schema.json" "$tree/tools/etude/schemas/"
cp "$fixtures/cases/fixture.default.json" "$fixtures/cases/fixture.host.json" "$tree/tests/cases/"
cp "$etude" "$tree/build/tools/etude/etude"
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
[ "$#" -ne 0 ] || "$CI_RUN_TEST_CUTTER" make $$
[ "$ARCH" != "${CI_RUN_TEST_BROKEN:-}" ] || exit 1
exit "${CI_RUN_TEST_MAKE:-0}"
EOF
# The cutter, which a stand-in runs as `cut WHERE PID`, PID being its own:
# when CI_RUN_TEST_CUT is `WHERE HOW`, it kills with KILL, as GitHub's
# cleanup of a cancelled job does, for `step` the shells of run.sh between
# PID and the run's shell, and PID; for `self` PID alone; and for `parent`
# the parent of the run's shell. The run's shell is the outermost ancestor
# of PID that runs `sh tools/ci/run.sh`.
cat > "$work/cut" <<'EOF'
#!/bin/sh
[ "${CI_RUN_TEST_CUT%% *}" = "$1" ] || exit 0
chain=
top=
pid=$(ps -o ppid= -p "$2" | tr -d ' ')
while ps -o args= -p "$pid" | grep -q '^sh tools/ci/run\.sh '; do
    chain="$chain $top"
    top=$pid
    pid=$(ps -o ppid= -p "$pid" | tr -d ' ')
done
[ -n "$top" ] || { echo "cut: no shell of run.sh above $2" >&2; exit 1; }
case ${CI_RUN_TEST_CUT#* } in
    step) kill -KILL $chain "$2" ;;
    self) kill -KILL "$2" ;;
    parent) kill -KILL "$pid" ;;
esac
EOF
# The stand-in tests/run, which notes its ARCH in CALLS.arch and makes a
# directory build/tests/failed/NAME for each word of CI_RUN_TEST_KEEP: a set
# whose first suite is not CI_RUN_TEST_SET
# passes and writes no history; that set prints CI_RUN_TEST_PRINT, then
# CI_RUN_TEST_ERR on standard error and CI_RUN_TEST_LATE after it, copies the
# history streams CI_RUN_TEST_STREAMS names into RACH2_RESULTS/history and
# the fixtures' store into RACH2_RESULTS/store, which run.sh makes, and
# exits CI_RUN_TEST_STATUS.
cat > "$tree/tests/run" <<'EOF'
#!/bin/sh
echo "tests/run $* with ${RACH2_RESULTS:-no results}" >> "$CI_RUN_TEST_CALLS"
echo "$ARCH" >> "$CI_RUN_TEST_CALLS.arch"
for k in ${CI_RUN_TEST_KEEP:-}; do mkdir -p "build/tests/failed/$k"; done
"$CI_RUN_TEST_CUTTER" "$1" $$
if [ "$1" != "$CI_RUN_TEST_SET" ]; then
    echo "run: $1: PASS in 1s"
    echo 'run: result: passed'
    exit 0
fi
printf '%b' "$CI_RUN_TEST_PRINT"
printf '%b' "$CI_RUN_TEST_ERR" >&2
printf '%b' "$CI_RUN_TEST_LATE"
for s in $CI_RUN_TEST_STREAMS; do
    cp "$CI_RUN_TEST_FIXTURES/$s" "$RACH2_RESULTS/history/" || exit 3
done
[ -z "$CI_RUN_TEST_STREAMS" ] || cp "$CI_RUN_TEST_FIXTURES"/store/* "$RACH2_RESULTS/store/" || exit 3
exit "$CI_RUN_TEST_STATUS"
EOF
chmod +x "$work/bin/git" "$work/bin/make" "$work/cut" "$tree/tests/run"

pass='pass/plan.jsonl pass/item-d0.jsonl pass/item-d1.jsonl pass/item-h.jsonl pass/run.jsonl'
failure='pass/plan.jsonl item-d0-failure.jsonl pass/item-d1.jsonl pass/item-h.jsonl run-failure.jsonl'
retry='pass/plan.jsonl item-d0-retry.jsonl pass/item-d1.jsonl pass/item-h.jsonl run-retry.jsonl'
unfinished='pass/plan.jsonl item-d0-unfinished.jsonl pass/item-d1.jsonl pass/item-h.jsonl run-unfinished.jsonl'
lost='pass/plan.jsonl pass/item-d0.jsonl item-d1-lost.jsonl pass/item-h.jsonl pass/run.jsonl'
rows=0
streams=
print=
errs=
late=
make_status=0
suites=
arch=i686
keep=
broken=

# row NAME MODE KIND SET STATUS FAILED: run run.sh KIND, reading results as
# MODE says, with the set SET of tests/run exiting STATUS, and require the
# lines of DIR/failed to be the words FAILED, `done` written, and run.sh's
# status 0 for no failure and 1 for any. The stand-in tests/run prints
# `print`, `errs` on standard error and `late`, and copies the streams
# `streams` names, make exits `make_status`, or 1 on the architecture
# `broken`, ARCH is `arch`, SUITES is `suites`, and tests/run keeps `keep`;
# row then empties `print`, `errs`, `late`, `streams`, `keep` and `broken`
# and sets `make_status` to 0.
row()
{
    rows=$((rows + 1))
    out=$work/out.$rows
    install "$2" || { echo "ci-run-test: $1: tools/ci/run.sh has no line results=lines" >&2; return 1; }
    : > "$work/calls"
    : > "$work/calls.arch"
    (
        cd "$tree" &&
            PATH="$work/bin:$PATH" ARCH=$arch SUITES=$suites CI_RUN_TEST_SET=$4 \
                CI_RUN_TEST_STATUS=$5 CI_RUN_TEST_PRINT=$print CI_RUN_TEST_ERR=$errs \
                CI_RUN_TEST_LATE=$late CI_RUN_TEST_STREAMS=$streams \
                CI_RUN_TEST_MAKE=$make_status CI_RUN_TEST_FIXTURES=$fixtures \
                CI_RUN_TEST_CALLS=$work/calls CI_RUN_TEST_CUTTER=$work/cut CI_RUN_TEST_CUT='' \
                CI_RUN_TEST_KEEP=$keep CI_RUN_TEST_BROKEN=$broken \
                sh tools/ci/run.sh "$3" "$out"
    ) > "$work/row.log" 2>&1
    status=$?
    print=
    errs=
    late=
    streams=
    keep=
    broken=
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

# kept ERR: require DIR/err of the last row to be ERR, the lines that the
# stand-in tests/run printed on its standard error, with `\n` for a newline,
# and none of those it printed on its standard output.
kept()
{
    printf '%b' "$1" > "$work/err"
    cmp -s "$work/err" "$out/err" && return 0
    sed 's/^/ci-run-test: err: /' "$out/err" >&2
    echo "ci-run-test: DIR/err of row $rows is not the standard error of tests/run" >&2
    return 1
}

# The runner's lines, as run.sh reads them today.
lines_rows()
{
    suites=
    row 'lines: a pass' lines core core 0 '' || return 1
    print='run: style: PASS in 3s\n'
    errs='run: default: FAIL in 3s\nrun: result: failed: default\n'
    row 'lines: a failed suite' lines core core 1 default || return 1
    print='run: default: PASS in 3s\n'
    errs='run: result: failed: nothing-such\n'
    row 'lines: no suite named' lines core core 1 core || return 1
    print='run: runner: PASS in 9s\n'
    errs='run: result: failed: default\nrun: shutdown: INCOMPLETE in ?s\n'
    row "lines: the runner suite's own lines" lines core core 1 core || return 1
    errs='run: default: INCOMPLETE in 121s\nrun: shutdown: INCOMPLETE in 301s\n'
    errs="${errs}run: result: failed: default shutdown\n"
    late='run: runner: PASS in 9s\nrun: result: incomplete\n'
    own=$errs
    row 'lines: the standard error before the standard output' lines core core 1 \
        'default shutdown' || return 1
    kept "$own" || return 1
    make_status=1
    row 'lines: the build' lines core core 0 build || return 1
}

# agrees STATUS: require `etude show failures`, which report.sh reads a run's
# history with, to give STATUS for the history and store the last row left in
# DIR: 0 when every attempt passed and 1 when one did not.
agrees()
{
    if "$etude" show failures -s "$repo/tools/etude/schemas" -b "$out/store" "$out"/history/* \
        > "$work/failures.log" 2>&1; then
        got=0
    else
        got=$?
    fi
    [ "$got" -ne "$1" ] || return 0
    sed 's/^/ci-run-test: show failures: /' "$work/failures.log" >&2
    echo "ci-run-test: etude show failures gave $got for the history of row $rows, not $1" >&2
    return 1
}

# etude's history, judged by `etude check verdict`.
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
    print='run: failing: FAIL in 1s\nrun: result: failed: failing\n'
    errs='run: default: FAIL in 3s\nrun: result: failed: default\n'
    own=$errs
    row 'history: a failed item' history full default 1 default || return 1
    agrees 1 || return 1
    kept "$own" || return 1
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
    rm "$tree/build/tools/etude/etude"
    streams=$pass
    row 'history: no verdict' history full default 0 runner
    status=$?
    cp "$etude" "$tree/build/tools/etude/etude"
    [ "$status" -eq 0 ] || return 1
    suites=
    streams=$pass
    row 'history: style, host and the core tier as one run' history core style 0 \
        'runner style shutdown' || return 1
    grep -q '^tests/run style host core with .*/out\.[0-9]*$' "$work/calls" ||
        { echo 'ci-run-test: core did not run as one run of style, host and core' >&2; return 1; }
}

# cut NAME MODE KIND CUT SETS STATUS: run run.sh KIND, reading results as
# MODE says, under a parent shell of its own, with CI_RUN_TEST_CUT set to
# CUT, and once the run and every process it started have ended, require
# the sets that tests/run was asked for to be the words SETS, no `done`, the
# line that says the run stopped, and run.sh's status STATUS, or `none` for
# a run whose parent was killed. ARCH is `arch`.
cut()
{
    rows=$((rows + 1))
    out=$work/out.$rows
    install "$2" || { echo "ci-run-test: $1: tools/ci/run.sh has no line results=lines" >&2; return 1; }
    : > "$work/calls"
    : > "$work/calls.arch"
    rm -f "$work/status"
    # The pipe ends only when every process that holds its end has ended,
    # and the shell's note of a killed parent goes to the null device.
    { (
        cd "$tree" &&
            PATH="$work/bin:$PATH" ARCH=$arch SUITES=$suites CI_RUN_TEST_SET=none \
                CI_RUN_TEST_STATUS=0 CI_RUN_TEST_PRINT='' CI_RUN_TEST_ERR='' CI_RUN_TEST_LATE='' \
                CI_RUN_TEST_STREAMS='' CI_RUN_TEST_MAKE=0 CI_RUN_TEST_FIXTURES=$fixtures \
                CI_RUN_TEST_CALLS=$work/calls CI_RUN_TEST_CUTTER=$work/cut CI_RUN_TEST_CUT=$4 \
                sh -c 'sh tools/ci/run.sh "$1" "$2"; echo "$?" > "$3"' sh "$3" "$out" "$work/status"
    ) 2>&1 | cat > "$work/row.log"; } 2> /dev/null
    got=$(sed 's/^tests\/run \([^ ]*\) .*/\1/' "$work/calls" | tr '\n' ' ' | sed 's/ $//')
    status=none
    [ ! -f "$work/status" ] || status=$(cat "$work/status")
    if [ "$got" = "$5" ] && [ ! -f "$out/done" ] && [ "$status" = "$6" ] &&
        grep -qx "ci: $3: stopped on $arch" "$out/log"; then
        return 0
    fi
    sed 's/^/ci-run-test: log: /' "$work/row.log" "$work/calls" >&2
    echo "ci-run-test: $1: sets '$got', status $status; expected '$5', status $6, stopped without done" >&2
    return 1
}

# Steps killed as the runner kills a cancelled job's processes.
cut_rows()
{
    suites=
    cut "cut: a set's shells killed" lines core 'host step' 'style host' 143 || return 1
    cut "cut: a set killed alone" lines core 'host self' 'style host' 143 || return 1
    cut "cut: the run's parent killed" lines core 'host parent' 'style host' none || return 1
    suites='i686:default i686:host'
    cut "cut: the build's shell killed after make clean" history full 'make step' '' 143 ||
        return 1
    suites=
}

# words FILE WANT: require FILE to be the lines WANT, a word or line of
# words to each line, with WANT '' for an empty file.
words()
{
    got=$(sed 's/ with .*//' "$1" 2> /dev/null | tr '\n' ' ' | sed 's/ $//')
    [ -f "$1" ] && [ "$got" = "$2" ] && return 0
    echo "ci-run-test: row $rows: ${1#"$work"/} is '$got', not '$2'" >&2
    return 1
}

# Both architectures, ARCH='i686 x86_64', as one run of tests/run for each
# set, and a directory DIR/KIND-ARCH for each architecture.
both_rows()
{
    arch='i686 x86_64'
    suites=
    row 'both: a pass' lines core core 0 '' || return 1
    words "$work/calls" 'tests/run style tests/run host tests/run core' || return 1
    words "$work/calls.arch" 'i686 x86_64 i686 x86_64 i686 x86_64' || return 1
    for a in i686 x86_64; do
        words "$out/core-$a/failed" '' && [ -f "$out/core-$a/done" ] &&
            [ ! -f "$out/core-$a/skipped" ] || return 1
    done
    errs='run: i686:default: FAIL in 3s\n'
    errs="${errs}run: x86_64:fpu: guest.fpu.xsave on x86_64: failure: it exited 1\n"
    errs="${errs}run: x86_64:fpu: FAIL in 3s\nrun: result: failed: i686:default x86_64:fpu\n"
    row 'both: a failed suite of each' lines core core 1 'i686:default x86_64:fpu' || return 1
    words "$out/core-i686/failed" default && words "$out/core-x86_64/failed" fpu || return 1
    if grep -q 'x86_64:fpu: FAIL' "$out/core-i686/err" ||
        ! grep -q 'x86_64:fpu: FAIL' "$out/core-x86_64/err"; then
        echo "ci-run-test: row $rows: an architecture's err has the other's suites" >&2
        return 1
    fi
    errs='run: result: failed: nothing-such\n'
    row 'both: no suite named' lines core core 1 'i686:core x86_64:core' || return 1
    broken=x86_64
    row 'both: one build failed' lines core core 0 x86_64:build || return 1
    words "$work/calls.arch" 'i686 i686 i686' && words "$out/core-x86_64/failed" build &&
        words "$out/core-i686/failed" '' || return 1
    suites='i686:default x86_64:fpu'
    keep='tests.i686-x86_64.T.1.guest.default.boot.i686.r0'
    keep="$keep tests.i686-x86_64.T.1.guest.fpu.xsave.x86_64.r0 tests.i686-x86_64.T.1.host.none.r0"
    row 'both: chosen suites of each' lines full none 0 '' || return 1
    words "$work/calls" 'tests/run i686:default x86_64:fpu' || return 1
    (cd "$out/full-i686/kept" && ls) > "$work/kept.i686"
    (cd "$out/full-x86_64/kept" && ls) > "$work/kept.x86_64"
    words "$work/kept.i686" 'tests.i686-x86_64.T.1.guest.default.boot.i686.r0 tests.i686-x86_64.T.1.host.none.r0' &&
        words "$work/kept.x86_64" 'tests.i686-x86_64.T.1.guest.fpu.xsave.x86_64.r0 tests.i686-x86_64.T.1.host.none.r0' ||
        return 1
    suites='i686:default'
    row 'both: chosen suites of one' lines full none 0 '' || return 1
    words "$work/calls" 'tests/run i686:default' && words "$work/calls.arch" i686 || return 1
    [ -f "$out/full-x86_64/skipped" ] && [ ! -f "$out/full-i686/skipped" ] ||
        { echo "ci-run-test: row $rows: x86_64 is not skipped alone" >&2; return 1; }
    suites=
    row 'both: every suite' lines full none 0 '' || return 1
    words "$work/calls" 'tests/run style host default shutdown tcp fpu tests/run bigmem' &&
        words "$work/calls.arch" 'i686 x86_64 x86_64' || return 1
    cut 'cut: a run of both killed' lines core 'host self' 'style host' 143 || return 1
    [ -f "$out/core-i686/log" ] && [ ! -f "$out/core-i686/done" ] ||
        { echo "ci-run-test: row $rows: a stopped run of both left no log or a done" >&2; return 1; }
    arch=i686
}

cp "$run_sh" "$work/run.sh"
if ! { lines_rows && history_rows && cut_rows && both_rows; }; then
    echo 'ci-run-test: result: fail: ci reading of results' >&2
    exit 1
fi
echo "ci-run-test: result: pass: $rows rows: the runner's lines, the build, a passing and a failing history, a failure or an unfinished attempt before a retry that passed, a status that disagrees, a missing or cut-off history, a suite left out, a missing verdict, the core set as one run, the standard error kept apart, no step after a killed one or a killed parent, and both architectures as one run"
