#!/bin/sh
#
# runner.sh: the program of the runner cases, tests/cases/host.runner.json:
# the self-tests of the old runner's programs that tests/run and the old
# drivers still use, booting nothing.
#
# Each run is one case, named by its argument: `harness`, the harness's own
# self-test; and `group` and `cpu`, runutil's supervision of a command's
# process group and of the processor time of its descendants, from
# tests/system/runutil_group.sh and runutil_cpu.sh. The manifest says what
# each assertion checks.
#
# The program writes the case's report records on its standard output
# (tools/etude/Readme.md, Report records), with the output of what it runs
# between them, and exits 0 when every assertion passed and 1 otherwise.
# etude runs it with ETUDE_TREE naming the root of the tree and TMPDIR the
# case's workspace; the harness is the artifact the manifest's `prepare`
# lists.
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

FAILED=""
case "$CASE" in
    harness|group|cpu) ;;
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
esac
record "end 1"
if [ -z "$FAILED" ]; then
    echo "runner: result: passed: $ID"
    exit 0
fi
echo "runner: result: failed: $ID"
exit 1
