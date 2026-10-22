#!/bin/sh
# runutil_group.sh: check how runutil's supervisor treats a failed setpgid,
# with fixtures/runutil_group.c, which compiles runutil.c with setpgid
# replaced.
#
# A command must run in its own group and exit 23 when the parent already
# put it in one (`assigned`), when its own call succeeds (`child`), and when
# it leads a session of its own (`session`); when neither call gives it a
# group (`denied`), runutil must report setpgid's failure and exit 2.
set -eu

[ "$#" -eq 0 ] || { echo 'runutil-group: usage: runutil_group.sh' >&2; exit 2; }
repo=$(cd "$(dirname "$0")/../.." && pwd)
. "$repo/tests/system/runlock.sh"
runlock_hold "$repo" runutil-group "$repo/tests/system/runutil_group.sh" "$@"
work=
child=0
keep=

# Stop the current case, then remove the work directory, unless the test
# failed after printing its path, which sets `keep`; a signal clears `keep`.
# shellcheck disable=SC2329
cleanup()
{
    exit_status=$?
    if [ "$child" -ne 0 ]; then
        kill -TERM "$child" 2>/dev/null || :
        wait "$child" 2>/dev/null || :
    fi
    [ -n "$work" ] || return 0
    if [ "$exit_status" -eq 0 ] || [ -z "$keep" ]; then
        rm -rf "$work"
    fi
}
trap cleanup EXIT
trap 'keep=; exit 130' INT
trap 'keep=; exit 143' TERM
trap 'keep=; exit 129' HUP
work=$(mktemp -d "${TMPDIR:-/tmp}/rach2-runutil-group.XXXXXX")
echo "runutil-group: artifacts: $work"
keep=1

cc -O2 -Wall -Wextra -Werror -I "$repo/tests/system/runner" \
    "$repo/tests/system/fixtures/runutil_group.c" -o "$work/runutil_group"

# run_case NAME STATUS: run the case NAME, which must exit with STATUS and
# print what that status means.
run_case()
{
    name=$1
    expected=$2
    log=$work/$name.log
    "$work/runutil_group" "$name" > "$log" 2>&1 &
    child=$!
    status=0
    wait "$child" || status=$?
    child=0
    cat "$log"
    echo "runutil-group: result: $name status $status"
    [ "$status" -eq "$expected" ] || return 1
    if [ "$expected" -eq 23 ]; then
        grep -qx 'runutil-group: result: command ran in its own group' "$log" &&
            ! grep -q 'runutil: setpgid:' "$log"
    else
        grep -Eq '^runutil: setpgid: pid [0-9]+, group [0-9]+:' "$log" &&
            ! grep -q 'command ran' "$log"
    fi
}

result=0
run_case assigned 23 || result=1
run_case denied 2 || result=1
run_case child 23 || result=1
run_case session 23 || result=1
[ "$result" -eq 0 ]
echo 'runutil-group: result: group checks passed'
