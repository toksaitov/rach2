#!/bin/sh
# runlock_exec.sh NAME SCRIPT ARGS...: run SCRIPT with ARGS holding the tree,
# as tests/run holds it: the record lock on build/.run.guard for as long as
# this process lives, and the directory build/.run.lock with an owner file
# that appears whole, by rename. SCRIPT runs with RACH2_TREE_HELD naming this
# process, so it and what it starts find the tree held (runlock.sh).
#
# SCRIPT runs in a process group of its own, as tests/run runs a suite, so a
# signal sent to this process's group, as a terminal's interrupt is, reaches
# the script once, through this process, and not again while its trap
# restores the sources: a TERM, HUP or INT here is passed to the script's
# group as TERM, once. The directory goes when the script has ended, and the
# exit status is the script's: a wait a signal interrupts is waited again,
# and a wait that had already ended with the script, the next returning 127,
# keeps the status it had. Its owner file says `held`, which tests/run never
# takes over: a script killed with KILL restored nothing and may have left
# work running in groups of its own, and so may one whose holder was killed,
# so only this process's own exit removes the directory.
#
# A directory left with no owner record, or one without a pid, is taken
# over once it is ten seconds old, as tests/run takes it over: a run cut off
# while it wrote its record, as a restart of the host cuts one off, leaves
# such a directory, and with no pid and no group no process of that run can
# be found. A younger one may be a run taking the tree this moment.
set -u
name=$1
shift
repo=$(CDPATH='' cd -- "$(dirname -- "$0")/../.." && pwd)
lock=$repo/build/.run.lock
if [ "${RACH2_TREE_TAKING:-}" != "$$" ]; then
    helper=$(mktemp -d "${TMPDIR:-/tmp}/rach2-runlock.XXXXXX") || exit 1
    if ! cc -O2 -Wall -Wextra -o "$helper/runutil" "$repo/tests/system/runner/runutil.c"; then
        echo "$name: the runner helper did not compile" >&2
        rm -rf "$helper"
        exit 1
    fi
    RACH2_TREE_TAKING=$$
    RACH2_RUN_WORK=$helper
    export RACH2_TREE_TAKING RACH2_RUN_WORK
    mkdir -p "$repo/build"
    exec "$helper/runutil" lock "$repo/build/.run.guard" "$lock/owner" sh "$0" "$name" "$@"
fi
helper=$RACH2_RUN_WORK
unset RACH2_TREE_TAKING RACH2_RUN_WORK
taken=
if mkdir "$lock" 2>/dev/null; then
    taken=1
elif grep -q '^held ' "$lock/owner" 2>/dev/null; then
    echo "$name: a fault script's hold on this tree was left: $lock; its fault may" \
        "be in the sources and its work may still run; restore them, stop its work" \
        "and remove $lock" >&2
elif grep -q '^pid ' "$lock/owner" 2>/dev/null; then
    echo "$name: no run holds this tree, but a run that is gone left $lock;" \
        "tests/run takes it over when nothing of that run is left" >&2
elif "$helper/runutil" young "$lock" 10; then
    echo "$name: another run is taking this tree" >&2
else
    echo "$name: taking over the lock a run left with no owner record: $lock"
    if mv "$lock" "$lock.old.$$" 2>/dev/null && mkdir "$lock" 2>/dev/null; then
        taken=1
    else
        echo "$name: another run took the tree first" >&2
    fi
    rm -rf "$lock.old.$$"
fi
if [ -z "$taken" ]; then
    rm -rf "$helper"
    exit 1
fi
printf 'pid %s\npgid %s\nsince %s\nargs %s\nheld fault script\n' "$$" \
    "$(ps -o pgid= -p $$ | tr -d ' ')" "$(date '+%Y-%m-%d %H:%M:%S')" "$name $*" > "$lock/owner.new"
mv "$lock/owner.new" "$lock/owner"
RACH2_TREE_HELD=$$
export RACH2_TREE_HELD
child=
signalled=
forwarded=
# stop_script: TERM, once, to the script's group, or to its pid while it has
# not made the group yet.
stop_script()
{
    [ -z "$forwarded" ] || return 0
    forwarded=1
    "$helper/runutil" killgroup "$child" 15 2>/dev/null || kill -TERM "$child" 2>/dev/null
}
trap 'signalled=1; [ -z "$child" ] || stop_script' TERM HUP INT
[ -z "$signalled" ] || { rm -rf "$lock" "$helper"; exit 143; }
"$helper/runutil" group sh "$@" &
child=$!
[ -z "$signalled" ] || stop_script
# An interrupted wait returns the signal's status; the next, the script's.
status=
while :; do
    signalled=
    waited=0
    wait "$child" || waited=$?
    if [ "$waited" -ne 127 ] || [ -z "$status" ]; then
        status=$waited
    fi
    [ -n "$signalled" ] || break
done
if [ "$status" -eq 137 ]; then
    echo "$name: the script was killed; its work may still run and its fault may be" \
        "in the sources; $lock stays until a person has looked" >&2
    rm -rf "$helper"
    exit "$status"
fi
rm -rf "$lock" "$helper"
exit "$status"
