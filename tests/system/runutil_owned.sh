#!/bin/sh
# Check the actual runner's verdict and cleanup with unregistered workers.
# With --mutate every rule is reversed in a compiled copy of the runner and
# must be caught, and the SSH flood's cleanup proof and the rehearsal's
# group-size proof run in their mutating forms: together the proofs that
# entries 90 to 93 of the shortcomings plan name.
set -eu
case ${1:-} in
    '') mutate=0 ;;
    --mutate) mutate=1 ;;
    --texts) mutate=2 ;;
    *) echo 'runutil-owned: usage: runutil_owned.sh [--mutate | --texts]' >&2; exit 2 ;;
esac
[ "$#" -le 1 ] || exit 2
repo=$(cd "$(dirname "$0")/../.." && pwd)
work=$(mktemp -d "${TMPDIR:-/tmp}/rach2-runutil-owned.XXXXXX")

# owned_edit FAULT [SOURCE]: write $work/runutil.c as SOURCE, by default the
# saved runner helper, with the rule FAULT names reversed; fail when the edit
# changes nothing, since its text has left the helper.
owned_edit()
{
    case $1 in
        missing-check) program='s/leaked = check_token(/leaked = 0 \&\& check_token(/' ;;
        recorded-group) program='s/!list\[i\].owned && complete == 0/complete == 0/' ;;
        short-grace) program='s/now() - started >= 15/now() - started >= 2/' ;;
        unnamed-failure) program='/if (!count) {/,/^        }/s/return 0;/return 1;/' ;;
        killed-survivor) program='/recorded group did not stop within 15 s/,/failed = 1;/s/failed = 1;/signal_token(token, SIGKILL);/' ;;
        machine-reuse) program='s/id.start != start/0/' ;;
    esac
    sed "$program" "${2:-$work/runutil.saved}" > "$work/runutil.c"
    if cmp -s "${2:-$work/runutil.saved}" "$work/runutil.c"; then
        echo "runutil-owned: the $1 fault is not in the helper" >&2
        exit 1
    fi
}

if [ "$mutate" -eq 2 ]; then
    for fault in missing-check recorded-group short-grace unnamed-failure killed-survivor \
        machine-reuse; do
        owned_edit "$fault" "$repo/tests/system/runner/runutil.c"
    done
    rm -rf "$work"
    echo 'runutil-owned: texts: every fault is once in its source'
    exit 0
fi
runner=0
sentinel=0
machine_test=0


# The runner's own tests boot nothing on another kernel: the trees they
# copy have no such target, so the name a caller set is dropped here.
RACH2_KERNEL=""
export RACH2_KERNEL
# Stop test processes before restoring the private source. PID files are
# used only here, after verdict assertions; the runner never reads them.
# shellcheck disable=SC2329
cleanup()
{
    if [ "$runner" -ne 0 ]; then
        kill -TERM "$runner" 2>/dev/null || :
        wait "$runner" 2>/dev/null || :
    fi
    if [ "$machine_test" -ne 0 ]; then
        "$work/fixed/helper" killgroup "$machine_test" 15 2>/dev/null || :
        /bin/sleep 0.1
        "$work/fixed/helper" killgroup "$machine_test" 9 2>/dev/null || :
        wait "$machine_test" 2>/dev/null || :
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
    cp "$repo/tests/system/runlock.sh" "$repo/tests/system/runlock_exec.sh" \
        "$tree/tests/system/" || return 125
    cp "$work/runutil.c" "$tree/tests/system/runner/runutil.c" || return 125
    if [ "$2" = hidden ]; then
        mv "$tree/tests/system/runner/runutil.c" "$tree/tests/system/runner/runutil_source.c" || return 125
        cat > "$tree/tests/system/runner/runutil.c" <<'HIDDEN' || return 125
#define _POSIX_C_SOURCE 200809L
#ifdef __APPLE__
#define _DARWIN_C_SOURCE
#include <libproc.h>
#define proc_pidfdinfo(pid, fd, flavor, info, size) 0
#else
#include <sys/stat.h>
#define fstatat(fd, path, st, flags) (-1)
#endif
#include "runutil_source.c"
HIDDEN
    fi
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
    case "$mode" in
        interrupted)
            [ "$rc" -eq 143 ] && grep -q '^run: result: interrupted by term$' "$log" || result=1 ;;
        group|session)
            [ "$rc" -eq 1 ] && grep -q '^run: owned: FAIL' "$log" &&
                grep -q '^run: owned: holder outside recorded groups: pid ' "$log" &&
                grep -q '^run: owned: all identified pipe holders stopped$' "$log" || result=1 ;;
        stubborn)
            [ "$rc" -eq 1 ] && grep -q '^run: owned: recorded group did not stop within 15 s: pid ' "$log" || result=1 ;;
        *)
            [ "$rc" -eq 0 ] && grep -q '^run: owned: PASS' "$log" || result=1 ;;
    esac
    if [ "$mode" = hidden ]; then
        grep -q '^run: owned: token inspection found no current holder; inconclusive$' "$log" || result=1
    fi
    i=0
    while [ ! -s "$tree/worker.pid" ] && [ "$i" -lt 100 ]; do
        sleep 0.02; i=$((i + 1))
    done
    if [ -s "$tree/worker.pid" ]; then
        read -r pid < "$tree/worker.pid"
        if [ "$mode" = group ] || [ "$mode" = session ]; then
            grep -q "^run: owned: holder outside recorded groups: pid $pid, group " "$log" || result=1
        fi
        i=0
        while kill -0 "$pid" 2>/dev/null && [ "$i" -lt 100 ]; do
            sleep 0.02; i=$((i + 1))
        done
        if kill -0 "$pid" 2>/dev/null; then
            [ "$mode" = hidden ] || result=1
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

# Compile the production machine check into each fixture binary. Native
# identities distinguish a live survivor from a zombie or a reused PID.
check_machine()
{
    machine_phase=$1
    machine_case=$2
    cc -O2 -Wall -Wextra -Werror -I "$work" "$repo/tests/system/fixtures/runutil_machine.c" \
        -o "$work/$machine_phase/machine" || return 125
    "$work/fixed/helper" group "$work/$machine_phase/machine" "$machine_case" "$work/$machine_phase/$machine_case.machines" \
        > "$work/$machine_phase/$machine_case.log" 2>&1 &
    machine_test=$!
    machine_rc=0
    wait "$machine_test" || machine_rc=$?
    machine_test=0
    cat "$work/$machine_phase/$machine_case.log"
    return "$machine_rc"
}

check_phase()
{
    mkdir "$work/$1"
    cc -O2 -Wall -Wextra -Werror "$work/runutil.c" -o "$work/$1/helper"
    check_case "$1" group
    check_case "$1" session
    check_case "$1" clean
    check_case "$1" parting
    check_case "$1" ingroup
    check_case "$1" recorded
    check_case "$1" draining
    check_case "$1" stubborn
    check_case "$1" hidden
    check_case "$1" interrupted
    check_machine "$1" reused
    check_machine "$1" zombie
    check_machine "$1" live
}

# Each fault gets a fresh build and an affected restored case. The final
# restored phase repeats the complete set with matching helper bytes.
prove_mutation()
{
    label=$1
    affected=$2
    expected=$3
    if cmp -s "$work/runutil.saved" "$work/runutil.c"; then exit 1; fi
    mkdir "$work/$label"
    cc -O2 -Wall -Wextra -Werror "$work/runutil.c" -o "$work/$label/helper"
    mutation_rc=0
    check_case "$label" "$affected" || mutation_rc=$?
    if [ "$mutation_rc" -ne 1 ]; then
        echo "runutil-owned: $label was not detected" >&2
        exit 1
    fi
    grep -q "^run: owned: $expected" "$work/$label.$affected/output.log"
    echo "runutil-owned: result: $label detected"
    cp "$work/runutil.saved" "$work/runutil.c"
    touch "$work/runutil.c"
    mkdir "$work/restored-$label"
    cc -O2 -Wall -Wextra -Werror "$work/runutil.c" -o "$work/restored-$label/helper"
    check_case "restored-$label" "$affected"
    cmp "$work/fixed/helper" "$work/restored-$label/helper"
}

check_phase fixed
if [ "$mutate" -eq 1 ]; then
    owned_edit missing-check
    prove_mutation missing-check session PASS
    owned_edit recorded-group
    prove_mutation recorded-group ingroup FAIL
    owned_edit short-grace
    prove_mutation short-grace draining FAIL
    owned_edit unnamed-failure
    prove_mutation unnamed-failure hidden FAIL
    owned_edit killed-survivor
    prove_mutation killed-survivor stubborn PASS
    owned_edit machine-reuse
    mkdir "$work/machine-reuse"
    machine_rc=0
    check_machine machine-reuse reused || machine_rc=$?
    [ "$machine_rc" -eq 1 ] || { echo 'runutil-owned: machine identity mutation was not detected' >&2; exit 1; }
    echo 'runutil-owned: result: machine identity mutation detected'
    cp "$work/runutil.saved" "$work/runutil.c"
    touch "$work/runutil.c"
    check_phase restored
    cmp "$work/fixed/helper" "$work/restored/helper"
fi

if [ "$mutate" -eq 1 ]; then
    sh "$repo/tests/system/ssh_flood_test.sh" --mutate
    sh "$repo/tests/system/rehearse_group_mutate.sh"
else
    sh "$repo/tests/system/ssh_flood_test.sh"
fi
