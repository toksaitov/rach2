#!/bin/sh
# Check group setup with the actual helper; --mutate proves the error check.
set -eu

case ${1:-} in
    '') mutate=0 ;;
    --mutate) mutate=1 ;;
    --texts) mutate=2 ;;
    *) echo 'runutil-group: usage: runutil_group.sh [--mutate | --texts]' >&2; exit 2 ;;
esac
[ "$#" -le 1 ] || exit 2
repo=$(cd "$(dirname "$0")/../.." && pwd)
if [ "$mutate" -eq 2 ]; then
    if [ "$(grep -c 'if (actual != pid)' "$repo/tests/system/runner/runutil.c")" -ne 1 ]; then
        echo 'runutil-group: the group check is not once in runutil.c' >&2
        exit 1
    fi
    echo 'runutil-group: texts: every fault is once in its source'
    exit 0
fi
work=$(mktemp -d "${TMPDIR:-/tmp}/rach2-runutil-group.XXXXXX")
child=0

# Stop the current case before restoring the private source used to build it.
# shellcheck disable=SC2329
cleanup()
{
    if [ "$child" -ne 0 ]; then
        kill -TERM "$child" 2>/dev/null || :
        wait "$child" 2>/dev/null || :
    fi
    if [ -f "$work/runutil.saved" ]; then
        cp "$work/runutil.saved" "$work/runutil.c"
        touch "$work/runutil.c"
    fi
}
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM
trap 'exit 129' HUP
cp "$repo/tests/system/runner/runutil.c" "$work/runutil.saved"
cp "$work/runutil.saved" "$work/runutil.c"
echo "runutil-group: artifacts: $work"

build()
{
    cc -O2 -Wall -Wextra -Werror -I "$work" \
        "$repo/tests/system/fixtures/runutil_group.c" -o "$work/$1"
}

run_case()
{
    phase=$1
    name=$2
    expected=$3
    log=$work/$phase.$name.log
    "$work/$phase" "$name" "$work/$phase.$name" > "$log" 2>&1 &
    child=$!
    status=0
    wait "$child" || status=$?
    child=0
    cat "$log"
    echo "runutil-group: result: $phase $name status $status"
    [ "$status" -eq "$expected" ] || return 1
    if [ "$expected" -eq 23 ]; then
        grep -qx 'runutil-group: result: command ran in its own group' "$log" &&
            ! grep -q 'runutil: setpgid:' "$log"
    else
        grep -Eq '^runutil: setpgid: pid [0-9]+, group [0-9]+:' "$log" &&
            ! grep -q 'command ran' "$log"
    fi
}

check_cases()
{
    result=0
    run_case "$1" assigned 23 || result=1
    run_case "$1" denied 2 || result=1
    run_case "$1" child 23 || result=1
    run_case "$1" session 23 || result=1
    return "$result"
}

build fixed
check_cases fixed
if [ "$mutate" -eq 1 ]; then
    [ "$(grep -c 'if (actual != pid)' "$work/runutil.c")" -eq 1 ]
    sed 's/if (actual != pid)/if (1)/' "$work/runutil.saved" > "$work/runutil.c"
    build mutated
    status=0
    check_cases mutated || status=$?
    echo "runutil-group: result: mutated checks status $status"
    [ "$status" -eq 1 ]
    run_case mutated assigned 2
    run_case mutated session 2
    cp "$work/runutil.saved" "$work/runutil.c"
    touch "$work/runutil.c"
    build restored
    check_cases restored
    cmp "$work/runutil.saved" "$work/runutil.c"
fi
echo 'runutil-group: result: group checks passed'
