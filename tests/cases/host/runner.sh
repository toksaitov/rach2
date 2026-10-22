#!/bin/sh
#
# runner.sh: the program of the runner cases, tests/cases/host.runner.json:
# the self-tests of the old runner's programs that tests/run and the old
# drivers still use, booting nothing.
#
# Each run is one case, named by its argument: `harness`, the harness's own
# self-test; `group` and `cpu`, runutil's supervision of a command's process
# group and of the processor time of its descendants, from
# tests/system/runutil_group.sh and runutil_cpu.sh; and `status`, the test
# status reader tools/flow/test_status.py against live and stale claims of
# the tree, from tests/system/test_status_test.py. The reader reads
# /proc/locks, so on a host that is not Linux the case runs the test in
# etude's tooling machine (tests/system/tooling.sh), as an ordinary user, and
# reports the reader not run when the machine is not made or fails. The
# manifest says what each assertion checks.
#
# The program writes the case's report records on its standard output
# (tools/etude/Readme.md, Report records), with the output of what it runs
# between them, and exits 0 when every assertion passed and 1 otherwise.
# etude runs it with ETUDE_TREE naming the root of the tree and TMPDIR the
# case's workspace; the harness is the artifact the manifest's `prepare`
# lists, and runutil is compiled into TMPDIR, as tests/run compiles it.
#
# Usage, in the root of a built tree, or anywhere with ETUDE_TREE set to it:
#     sh tests/cases/host/runner.sh CASE
#

set -u

CASE=${1-}
[ "$#" -eq 1 ] && [ -n "$CASE" ] || { echo "runner: usage: runner.sh CASE" >&2; exit 2; }
if [ -n "${ETUDE_TREE-}" ]; then
    cd "$ETUDE_TREE" || exit 2
fi
unset ETUDE_TREE
[ -f tests/system/runner/runutil.c ] || { echo "runner: run it in the root of the tree" >&2; exit 2; }
REPO=$(pwd -P)
ID=host.runner.$CASE
if [ -n "${ETUDE_CASE:-}" ] && [ "$ETUDE_CASE" != "$ID" ]; then
    echo "runner: ETUDE_CASE is $ETUDE_CASE, and the argument names $ID" >&2
    exit 2
fi

WORK=
trap '[ -z "$WORK" ] || rm -rf "$WORK"' EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM

# record WORDS: one report record, which fits in one write of 255 bytes.
record() {
    printf '@etude 1 %s\n' "$*"
}

# judge ID WHAT COMMAND...: run COMMAND with its output shown, and report the
# assertion ID passed when it exits 0, and failed otherwise, naming WHAT.
judge() {
    _id=$1
    _what=$2
    shift 2
    "$@" 2>&1
    _rc=$?
    if [ "$_rc" -eq 0 ]; then
        record "assert $_id pass"
    else
        record "assert $_id fail $_what exited with status $_rc"
        FAILED=1
    fi
}

# status_elsewhere: run test_status_test.py in the tooling machine, with the
# reader, the test and runutil's source packed into WORK/status.tar, since
# the job's volume keeps no file's mode, and judge it as on Linux.
status_elsewhere() {
    # shellcheck source=tests/system/tooling.sh
    . "$REPO/tests/system/tooling.sh"
    if ! tooling_made; then
        record "notrun reader this host is not Linux and has no made tooling machine"
        FAILED=1
        return
    fi
    COPYFILE_DISABLE=1 tar -c --format ustar -f "$WORK/status.tar" -C "$REPO" tests/system/test_status_test.py \
        tools/flow/test_status.py tests/system/runner/runutil.c || {
        record "notrun reader the test's files could not be packed"
        FAILED=1
        return
    }
    cat > "$WORK/job.sh" <<'JOB'
set -u
mkdir tree && tar -x -f status.tar -C tree && chmod -R a+rX tree &&
    useradd --create-home --user-group status || exit 3
cd tree && exec setpriv --reuid=status --regid=status --init-groups env -i PATH="$PATH" \
    HOME=/home/status sh -c 'cc -O2 -Wall -Wextra -o "$HOME/runutil" tests/system/runner/runutil.c &&
        python3 tests/system/test_status_test.py "$HOME/runutil"'
JOB
    _rc=0
    tooling run -i "$WORK/status.tar" "$WORK/job.sh" > "$WORK/out" 2>&1 || _rc=$?
    cat "$WORK/out"
    case $_rc in
        0) if grep -q 'result: skipped' "$WORK/out"; then
               record "notrun reader the tooling machine skipped the test"
               FAILED=1
           else
               record "assert reader pass"
           fi ;;
        124|125|126)
            record "notrun reader the tooling machine's job did not run to its end, status $_rc"
            FAILED=1 ;;
        *) record "assert reader fail test_status_test.py failed in the tooling machine"
           FAILED=1 ;;
    esac
}

FAILED=""
case "$CASE" in
    harness|group|cpu|status) ;;
    *) echo "runner: no case $CASE" >&2; exit 2 ;;
esac
record "begin $ID"
case "$CASE" in
    harness)
        judge selftest "harness --selftest" "$REPO/build/tests/system/harness" --selftest ;;
    group)
        judge setpgid "runutil_group.sh" sh "$REPO/tests/system/runutil_group.sh" ;;
    cpu)
        judge descendants "runutil_cpu.sh" sh "$REPO/tests/system/runutil_cpu.sh" ;;
    status)
        WORK=$(mktemp -d "${TMPDIR:-/tmp}/rach2-runner.XXXXXX") || {
            record "notrun reader no scratch directory"
            record "end 1"
            exit 1
        }
        if [ "$(uname -s)" != Linux ]; then
            status_elsewhere
        elif ! cc -O2 -Wall -Wextra -o "$WORK/runutil" "$REPO/tests/system/runner/runutil.c"; then
            record "notrun reader runutil did not compile"
            FAILED=1
        elif ! python3 "$REPO/tests/system/test_status_test.py" "$WORK/runutil" >"$WORK/out" 2>&1; then
            cat "$WORK/out"
            record "assert reader fail test_status_test.py failed"
            FAILED=1
        elif grep -q 'result: skipped' "$WORK/out"; then
            cat "$WORK/out"
            record "notrun reader the reader reads /proc/locks, which this host lacks"
            FAILED=1
        else
            cat "$WORK/out"
            record "assert reader pass"
        fi ;;
esac
record "end 1"
if [ -z "$FAILED" ]; then
    echo "runner: result: passed: $ID"
    exit 0
fi
echo "runner: result: failed: $ID"
exit 1
