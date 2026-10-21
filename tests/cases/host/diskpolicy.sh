#!/bin/sh
#
# diskpolicy.sh: the program of the case machine.diskpolicy.check,
# tests/cases/machine.diskpolicy.json: tools/inspect/check_diskpolicy.sh reads
# each kernel's disk policy off the artifact, and refuses each kernel offered
# as the other and an empty file.
#
# mkiso.sh runs the check before it builds an ISO around a kernel, and a
# check run only on the artifact it approves has not been shown to refuse
# anything. The boots of machine.diskpolicy.bytes then show that the policy
# holds on the disk's bytes.
#
# It writes the case's report records on its standard output
# (tools/etude/Readme.md, Report records), with the check's own lines
# between them, and exits 0 when every assertion passed and 1 when one
# failed. etude runs it in the case's workspace, with ETUDE_TREE naming the
# root of the tree, to which it changes first, and ARCH the architecture
# whose kernels it reads.
#
# Usage, in the root of a built tree, or anywhere with ETUDE_TREE set to it:
#     ARCH=x86_64 sh tests/cases/host/diskpolicy.sh
#
# shellcheck disable=SC2329 # the checks are called through judge
set -u

[ "$#" -eq 0 ] || { echo "diskpolicy: usage: diskpolicy.sh" >&2; exit 2; }
if [ -n "${ETUDE_TREE-}" ]; then
    cd "$ETUDE_TREE" || exit 2
fi
[ -f tools/inspect/check_diskpolicy.sh ] || {
    echo "diskpolicy: run it in the root of the tree" >&2
    exit 2
}
case "${ARCH:-i686}" in
    i686) BUILD=build ;;
    x86_64) BUILD=build/x86_64 ;;
    *) echo "diskpolicy: no architecture ${ARCH}" >&2; exit 2 ;;
esac
RW=$BUILD/rach2/kernel
RO=$BUILD/rach2-metal/kernel
ID=machine.diskpolicy.check

RECORDS=0
FAILED=""

# Write the report record WORDS, which fits in one write of 255 bytes.
record() {
    printf '@etude 1 %s\n' "$*"
}

# judge ID DETAIL COMMAND...: report the assertion ID passed when COMMAND
# succeeds, and failed with DETAIL otherwise.
judge() {
    _id=$1
    _detail=$2
    shift 2
    if "$@"; then
        record "assert $_id pass"
    else
        record "assert $_id fail $_detail"
        FAILED="$FAILED $_id"
    fi
    RECORDS=$((RECORDS + 1))
}

# accepts KERNEL POLICY: run the check, its lines on standard output.
accepts() {
    sh tools/inspect/check_diskpolicy.sh "$1" "$2" 2>&1
}

# refuses KERNEL POLICY: run the check and succeed when it refused.
refuses() {
    ! sh tools/inspect/check_diskpolicy.sh "$1" "$2" 2>&1
}

if [ -n "${ETUDE_CASE:-}" ] && [ "$ETUDE_CASE" != "$ID" ]; then
    echo "diskpolicy: ETUDE_CASE is $ETUDE_CASE, and this program is $ID" >&2
    exit 2
fi
record "begin $ID"
judge read-write "the check refused $RW as read-write" accepts "$RW" read-write
judge read-only "the check refused $RO as read-only" accepts "$RO" read-only
judge read-write-refused "the check accepted $RW as read-only" refuses "$RW" read-only
judge read-only-refused "the check accepted $RO as read-write" refuses "$RO" read-write
judge empty-refused "the check accepted an empty file" refuses /dev/null read-only
record "end $RECORDS"
if [ -z "$FAILED" ]; then
    echo "diskpolicy: result: passed: $ID"
    exit 0
fi
echo "diskpolicy: result: failed: $ID:$FAILED"
exit 1
