#!/bin/sh
# runlock_exec.sh NAME SCRIPT ARGS...: run SCRIPT with ARGS holding the tree,
# as tests/run holds it: the record lock on build/.run.guard for as long as
# this process lives, and the directory build/.run.lock with an owner file
# that appears whole, by rename. SCRIPT runs as a child with RACH2_TREE_HELD
# naming this process, so it and what it starts find the tree held
# (runlock.sh). A TERM, HUP or INT is passed to the child as TERM, so its own
# trap restores the sources; the directory goes when the child has ended,
# and the exit status is the child's. A child killed with KILL could restore
# nothing and may have left work running, so its directory stays, for
# tests/run to take over once nothing of this process group is left.
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
rm -rf "$RACH2_RUN_WORK"
unset RACH2_TREE_TAKING RACH2_RUN_WORK
if ! mkdir "$lock" 2>/dev/null; then
    echo "$name: no run holds this tree, but a run that is gone left $lock;" \
        "tests/run takes it over when nothing of that run is left" >&2
    exit 1
fi
printf 'pid %s\npgid %s\nsince %s\nargs %s\n' "$$" "$(ps -o pgid= -p $$ | tr -d ' ')" \
    "$(date '+%Y-%m-%d %H:%M:%S')" "$name $*" > "$lock/owner.new"
mv "$lock/owner.new" "$lock/owner"
RACH2_TREE_HELD=$$
export RACH2_TREE_HELD
sh "$@" &
child=$!
signalled=
trap 'signalled=1; kill -TERM "$child" 2>/dev/null' TERM HUP INT
while :; do
    signalled=
    status=0
    wait "$child" || status=$?
    if [ -z "$signalled" ] || ! kill -0 "$child" 2>/dev/null; then
        break
    fi
done
if [ "$status" -eq 137 ]; then
    echo "$name: the script was killed and its work may still run; $lock stays" \
        "for tests/run to take over when nothing of it is left" >&2
    exit "$status"
fi
rm -rf "$lock"
exit "$status"
