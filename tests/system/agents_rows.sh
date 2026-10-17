#!/bin/bash

#
# agents_rows.sh: run one row of `tests/run agents` on the machine.
#
# Usage: agents_rows.sh NAME [VERSION]
#
# The agents suite puts this script and the rows of tests/system/agents/ in
# /tmp/agents on the machine it boots, the GNU root with the agents layer,
# and runs it there as the test account under /bin/bash, once for each row,
# each run bounded on the host. It reads the row's file, NAME.sh, and runs
# its function `row` in an empty directory of the row's own, WORK, with
# VERSION, the version the package's recipe pins, in VERSION, and the path
# of the fake API server's certificate authority in CA. A row has a
# program do its job as a person would ask for it and compares what it
# printed or wrote with what it must be, through the functions below. The
# script prints `AGENTROW OK NAME` when every comparison holds, and otherwise
# `AGENTROW FAIL NAME: ` with the first that did not.
#

set -u
export PATH=/usr/bin:/bin:/usr/sbin:/sbin
export LC_ALL=C

ROW=${1:-}
# shellcheck disable=SC2034 # the rows read it
VERSION=${2:-}
WORK=/tmp/agents/work/$ROW
# shellcheck disable=SC2034 # the rows read it
CA=/tmp/agents/ca.pem
FAILED=
OUT=

# fail WHAT: record WHAT unless an earlier comparison already failed.
fail() {
    [ -n "$FAILED" ] || FAILED=$1
}

# same WHAT GOT WANT: require GOT to be WANT.
same() {
    [ "$2" = "$3" ] || fail "$1: got [$2], want [$3]"
}

# has WHAT GOT WANT: require GOT to contain WANT.
has() {
    case "$2" in
        *"$3"*) ;;
        *) fail "$1: [$(printf '%s' "$2" | tail -n 3 | tr '\n' ' ')] has no [$3]" ;;
    esac
}

# output WHAT COMMAND...: keep what COMMAND prints on both streams in OUT,
# and require it to exit with status 0.
output() {
    local what=$1
    shift
    OUT=$("$@" 2>&1) || fail "$what: status $?: $(printf '%s' "$OUT" | tail -n 3 | tr '\n' ' ')"
}

# status WHAT WANT COMMAND...: keep what COMMAND prints on both streams in
# OUT, and require it to exit with status WANT.
status() {
    local what=$1 want=$2 got
    shift 2
    OUT=$("$@" 2>&1)
    got=$?
    [ "$got" = "$want" ] ||
        fail "$what: status $got, want $want: $(printf '%s' "$OUT" | tail -n 3 | tr '\n' ' ')"
}

if [ -z "$ROW" ] || [ ! -f "/tmp/agents/$ROW.sh" ]; then
    echo "AGENTROW FAIL $ROW: no such row"
    exit 2
fi
# shellcheck disable=SC1090 # the row is chosen at run time
. "/tmp/agents/$ROW.sh"
if ! declare -F row > /dev/null; then
    echo "AGENTROW FAIL $ROW: $ROW.sh defines no function row"
    exit 2
fi
rm -rf "$WORK"
mkdir -p "$WORK"
cd "$WORK" || exit 2
row
cd / || exit 2
rm -rf "$WORK"
if [ -n "$FAILED" ]; then
    echo "AGENTROW FAIL $ROW: $FAILED"
    exit 1
fi
echo "AGENTROW OK $ROW"
