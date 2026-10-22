#!/bin/sh
# runutil_cpu.sh: quiet CPU work below deadline must keep a watched command
# alive across process groups. An unrelated CPU worker must not keep an idle
# command alive.
set -eu
[ "$#" -eq 0 ] || { echo 'runutil-cpu: usage: runutil_cpu.sh' >&2; exit 2; }
repo=$(cd "$(dirname "$0")/../.." && pwd)
. "$repo/tests/system/runlock.sh"
runlock_hold "$repo" runutil-cpu "$repo/tests/system/runutil_cpu.sh" "$@"
work=
runner=0
unrelated=0

# Stop and reap the current supervisor and the unrelated worker before
# removing the private binaries and logs.
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
cc -O2 -Wall -Wextra -Werror "$repo/tests/system/runner/runutil.c" -o "$work/runutil"
cc -O2 -Wall -Wextra -Werror "$repo/tests/system/fixtures/runutil_cpu.c" -o "$work/worker"
cc -O2 -Wall -Wextra -Werror "$repo/tests/system/runner/deadline.c" -o "$work/deadline"

# A silent native worker under deadline must finish with status 0. Its
# processor interval exceeds the supervisor's three-second idle limit.
"$work/runutil" watch cpu 3 \
    "$work/deadline" 60 "$work/worker" > "$work/busy.log" 2>&1 &
runner=$!
status=0
wait "$runner" || status=$?
runner=0
cat "$work/busy.log"
echo "runutil-cpu: result: busy status $status"
[ "$status" -eq 0 ]
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
echo 'runutil-cpu: result: activity checks passed'
