#!/bin/sh
# runutil_cpu.sh [--mutate | --texts]: quiet CPU work below deadline must
# keep a watched command alive across process groups. An unrelated CPU worker
# must not keep an idle command alive. --mutate removes descendant CPU
# accounting in a private helper, requires the busy row to fail, then rebuilds
# and restores it.
set -eu
case ${1:-} in
    '') mutate=0 ;;
    --mutate) mutate=1 ;;
    --texts) mutate=2 ;;
    *) echo 'runutil-cpu: usage: runutil_cpu.sh [--mutate | --texts]' >&2; exit 2 ;;
esac
[ "$#" -le 1 ] || exit 2
repo=$(cd "$(dirname "$0")/../.." && pwd)
fault='if (list[i].owned) {'
if [ "$mutate" -eq 2 ]; then
    [ "$(grep -cF "$fault" "$repo/tests/system/runner/runutil.c")" -eq 1 ]
    echo 'runutil-cpu: texts: descendant CPU fault found once'
    exit 0
fi
. "$repo/tests/system/runlock.sh"
runlock_hold "$repo" runutil-cpu "$repo/tests/system/runutil_cpu.sh" "$@"
work=
runner=0
unrelated=0

# Stop and reap the current supervisor and the unrelated worker before
# removing the private sources, binaries and logs.
# shellcheck disable=SC2329
cleanup()
{
    if [ "$runner" -ne 0 ]; then
        kill -TERM "$runner" 2>/dev/null || :
        wait "$runner" 2>/dev/null || :
    fi
    if [ "$unrelated" -ne 0 ]; then
        kill -TERM "$unrelated" 2>/dev/null || :
        wait "$unrelated" 2>/dev/null || :
    fi
    [ -z "$work" ] || rm -rf "$work"
}
trap cleanup EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM
work=$(mktemp -d "${TMPDIR:-/tmp}/rach2-runutil-cpu.XXXXXX")
cp "$repo/tests/system/runner/runutil.c" "$work/runutil.saved"
cp "$work/runutil.saved" "$work/runutil.c"
cc -O2 -Wall -Wextra -Werror "$repo/tests/system/fixtures/runutil_cpu.c" -o "$work/worker"
cc -O2 -Wall -Wextra -Werror "$repo/tests/system/runner/deadline.c" -o "$work/deadline"

# build: compile the private helper, without reusing an earlier executable.
build()
{
    cc -O2 -Wall -Wextra -Werror "$work/runutil.c" -o "$work/runutil"
}

# busy PHASE STATUS: a silent native worker under deadline must finish with
# STATUS. Its processor interval exceeds the supervisor's three-second idle
# limit. A mutated helper must stop it for idleness, not another failure.
busy()
{
    phase=$1
    expected=$2
    "$work/runutil" watch cpu 3 \
        "$work/deadline" 60 "$work/worker" > "$work/$phase.log" 2>&1 &
    runner=$!
    status=0
    wait "$runner" || status=$?
    runner=0
    cat "$work/$phase.log"
    echo "runutil-cpu: result: $phase busy status $status"
    [ "$status" -eq "$expected" ] || return 1
    if [ "$expected" -eq 124 ]; then
        grep -q 'no output for 3 s and next to no processor time' "$work/$phase.log"
    fi
}

build
busy fixed 0
"$work/worker" &
unrelated=$!
"$work/runutil" watch idle 3 \
    "$work/deadline" 60 sleep 30 > "$work/idle.log" 2>&1 &
runner=$!
status=0
wait "$runner" || status=$?
runner=0
cat "$work/idle.log"
echo "runutil-cpu: result: unrelated busy worker, idle status $status"
[ "$status" -eq 124 ]
grep -q 'no output for 3 s and next to no processor time' "$work/idle.log"
kill -TERM "$unrelated" 2>/dev/null || :
wait "$unrelated" 2>/dev/null || :
unrelated=0
if [ "$mutate" -eq 1 ]; then
    [ "$(grep -cF "$fault" "$work/runutil.saved")" -eq 1 ]
    sed 's/if (list\[i\].owned) {/if (list[i].pid == root || list[i].group == root) {/' \
        "$work/runutil.saved" > "$work/runutil.c"
    build
    busy mutated 124
    cp "$work/runutil.saved" "$work/runutil.c"
    touch "$work/runutil.c"
    build
    busy restored 0
fi
echo 'runutil-cpu: result: activity checks passed'
