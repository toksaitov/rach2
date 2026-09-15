#!/bin/sh
# Check the actual runner's verdict and cleanup with unregistered workers.
set -eu
case ${1:-} in
    '') mutate=0 ;;
    --mutate) mutate=1 ;;
    *) echo 'runutil-owned: usage: runutil_owned.sh [--mutate]' >&2; exit 2 ;;
esac
[ "$#" -le 1 ] || exit 2
repo=$(cd "$(dirname "$0")/../.." && pwd)
work=$(mktemp -d "${TMPDIR:-/tmp}/rach2-runutil-owned.XXXXXX")
runner=0
sentinel=0

# Stop test processes before restoring the private source. PID files are
# used only here, after verdict assertions; the runner never reads them.
# shellcheck disable=SC2329
cleanup()
{
    if [ "$runner" -ne 0 ]; then
        kill -TERM "$runner" 2>/dev/null || :
        wait "$runner" 2>/dev/null || :
    fi
    for file in "$work"/*/worker.pid; do
        [ -s "$file" ] || continue
        read -r pid < "$file"
        kill -KILL "$pid" 2>/dev/null || :
    done
    if [ "$sentinel" -ne 0 ]; then
        kill -USR1 "$sentinel" 2>/dev/null || :
        wait "$sentinel" 2>/dev/null || :
    fi
    cp "$work/runutil.saved" "$work/runutil.c"
    touch "$work/runutil.c"
}
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM
trap 'exit 129' HUP
cp "$repo/tests/system/runner/runutil.c" "$work/runutil.saved"
cp "$work/runutil.saved" "$work/runutil.c"
cc -O2 -Wall -Wextra -Werror "$repo/tests/system/fixtures/runutil_owned.c" -o "$work/worker"
"$work/worker" sentinel "$work/sentinel.pid" &
sentinel=$!
echo "runutil-owned: artifacts: $work"

# No guest or socket is used. Only the irrelevant socket preflight is a
# substitute; tests/run, its native helper and the make invocation are real.
make_tree()
{
    tree=$work/$1
    mkdir -p "$tree/tests/system/runner" "$tree/tools/setup" || return 125
    cp "$repo/tests/run" "$tree/tests/run" || return 125
    cp "$work/runutil.c" "$tree/tests/system/runner/runutil.c" || return 125
    cp "$work/worker" "$tree/worker" || return 125
    printf '%s\n' 'owned|-|core' > "$tree/tests/registry" || return 125
    printf '%s\n' 'int main(void) { return 0; }' > "$tree/tools/setup/hostcheck.c" || return 125
    printf 'components prepare-all:\n\t@:\nrun-owned:\n\t@./worker %s worker.pid\n' "$2" > "$tree/Makefile" || return 125
}

# A helper compile per case is also required by the runner's bootstrap;
# compilation failure cannot fall through to an earlier phase's executable.
check_case()
{
    phase=$1
    mode=$2
    make_tree "$phase.$mode" "$mode" || return 125
    log=$tree/output.log
    (unset RACH2_RUN_GROUP RACH2_RUN_WORK RACH2_RUN_LOCK RACH2_STOP_ON_FAIL
     cd "$tree"
     exec sh tests/run owned) > "$log" 2>&1 &
    runner=$!
    if [ "$mode" = interrupted ]; then
        i=0
        while [ ! -s "$tree/worker.pid" ] && [ "$i" -lt 200 ]; do
            sleep 0.02; i=$((i + 1))
        done
        [ -s "$tree/worker.pid" ] || return 125
        kill -TERM "$runner" || return 125
    fi
    rc=0
    wait "$runner" || rc=$?
    runner=0
    result=0
    if [ "$mode" = interrupted ]; then
        [ "$rc" -eq 143 ] && grep -q '^run: result: interrupted by term$' "$log" || result=1
    elif [ "$mode" = clean ] || [ "$mode" = parting ]; then
        [ "$rc" -eq 0 ] && grep -q '^run: owned: PASS' "$log" || result=1
    else
        [ "$rc" -eq 1 ] && grep -q '^run: owned: FAIL' "$log" &&
            grep -q '^run: owned: left its liveness token open;' "$log" &&
            grep -q '^run: owned: all pipe holders stopped$' "$log" || result=1
    fi
    i=0
    while [ ! -s "$tree/worker.pid" ] && [ "$i" -lt 100 ]; do
        sleep 0.02; i=$((i + 1))
    done
    if [ -s "$tree/worker.pid" ]; then
        read -r pid < "$tree/worker.pid"
        if [ "$mode" = group ] || [ "$mode" = session ]; then
            grep -q "^run: owned: token holder pid $pid, group " "$log" || result=1
        fi
        i=0
        while kill -0 "$pid" 2>/dev/null && [ "$i" -lt 100 ]; do
            sleep 0.02; i=$((i + 1))
        done
        if kill -0 "$pid" 2>/dev/null; then
            result=1
            kill -KILL "$pid" 2>/dev/null || :
            i=0
            while kill -0 "$pid" 2>/dev/null && [ "$i" -lt 100 ]; do
                sleep 0.02; i=$((i + 1))
            done
            if kill -0 "$pid" 2>/dev/null; then
                echo "runutil-owned: worker $pid did not stop" >&2
                return 125
            fi
        fi
        rm "$tree/worker.pid"
    else
        result=1
    fi
    kill -0 "$sentinel" 2>/dev/null || result=1
    if [ "$result" -ne 0 ]; then
        cat "$log"
        echo "runutil-owned: $phase $mode: verdict or cleanup failed (status $rc)" >&2
        return 1
    fi
    echo "runutil-owned: result: $phase $mode passed"
}

check_phase()
{
    mkdir "$work/$1"
    cc -O2 -Wall -Wextra -Werror "$work/runutil.c" -o "$work/$1/helper"
    check_case "$1" group
    check_case "$1" session
    check_case "$1" clean
    check_case "$1" parting
    check_case "$1" interrupted
}

check_phase fixed
if [ "$mutate" -eq 1 ]; then
    sed 's/leaked = check_token(/leaked = 0 \&\& check_token(/' "$work/runutil.saved" > "$work/runutil.c"
    if cmp -s "$work/runutil.saved" "$work/runutil.c"; then exit 1; fi
    cc -O2 -Wall -Wextra -Werror "$work/runutil.c" -o "$work/mutated.helper"
    mutation_rc=0
    check_case mutated session || mutation_rc=$?
    if [ "$mutation_rc" -ne 1 ]; then
        echo 'runutil-owned: missing token check was not detected' >&2
        exit 1
    fi
    grep -q '^run: owned: PASS' "$work/mutated.session/output.log"
    echo 'runutil-owned: result: missing token check detected'
    cp "$work/runutil.saved" "$work/runutil.c"
    touch "$work/runutil.c"
    check_phase restored
    cmp "$work/fixed/helper" "$work/restored/helper"
fi
