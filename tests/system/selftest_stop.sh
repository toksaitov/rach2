#!/bin/sh
# selftest_stop.sh [--mutate | --texts]: check, from the events its suites
# record, that `tests/run` with RACH2_STOP_ON_FAIL stops a set at its first
# failure.
#
# A private tree has a registry of three suites, failing, slow1 and slow2,
# run two at a time. FIFOs order them: failing waits until slow1 has started,
# and slow1 waits until slow2 has started. A correct stop fails the set,
# stops slow1 before its end and names it, and never starts slow2, which it
# names as not started. tests/run compiles runutil into the tree on every
# run; the socket preflight, which the suites do not need, is replaced with a
# program that does nothing.
#
# With --mutate two faults are put into the private copies in turn: `status`
# makes runutil report a status of 0 for any program that exits, and
# `continue` takes `stopped=1` out of tests/run. Each must fail the checks
# that slow2 never started and that slow1 did not reach its end, and the
# restored copies must pass with the helper of the first run. --texts checks
# that each fault is once in its source.
set -eu

case ${1:-} in
    '') mutate=0 ;;
    --mutate) mutate=1 ;;
    --texts) mutate=2 ;;
    *) echo 'selftest-stop: usage: selftest_stop.sh [--mutate | --texts]' >&2; exit 2 ;;
esac
[ "$#" -le 1 ] || exit 2
repo=$(cd "$(dirname "$0")/../.." && pwd)
if [ "${1:-}" != --texts ]; then
    . "$repo/tests/system/runlock.sh"
    runlock_hold "$repo" selftest-stop "$repo/tests/system/selftest_stop.sh" "$@"
fi
# once FILE TEXT: TEXT, which a fault changes, is once in FILE.
once()
{
    if [ "$(grep -Fc "$2" "$1" 2>/dev/null)" != 1 ]; then
        echo "selftest-stop: not once in $1: $2" >&2
        exit 1
    fi
}

if [ "$mutate" -eq 2 ]; then
    once "$repo/tests/system/runner/runutil.c" 'return WIFEXITED(status) ? WEXITSTATUS(status)'
    # shellcheck disable=SC2016
    once "$repo/tests/run" '[ "$rc" = 0 ] || [ -z "${RACH2_STOP_ON_FAIL:-}" ] || stopped=1'
    echo 'selftest-stop: texts: every fault is once in its source'
    exit 0
fi
mkdir -p "$repo/build"
work=$(mktemp -d "$repo/build/selftest-stop.XXXXXX")
tree=$work/tree
source=$tree/tests/system/runner/runutil.c
runner=0
mkdir -p "$tree/tests/system/runner" "$tree/tools/setup" "$tree/tmp"
cp "$repo/tests/system/runner/runutil.c" "$work/runutil.saved"
cp "$repo/tests/run" "$work/run.saved"
cp "$repo/tests/system/runlock.sh" "$repo/tests/system/runlock_exec.sh" "$tree/tests/system/"

# Put the saved runutil.c and tests/run into the private tree and touch them.
restore()
{
    cp "$work/runutil.saved" "$source"
    cp "$work/run.saved" "$tree/tests/run"
    touch "$source" "$tree/tests/run"
}

# Stop the private runner and restore the private tree.
# shellcheck disable=SC2329
cleanup()
{
    if [ "$runner" -ne 0 ]; then
        kill -TERM "$runner" 2>/dev/null || :
        wait "$runner" 2>/dev/null || :
    fi
    restore
}
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM
trap 'exit 129' HUP
restore
printf '%s\n' 'failing|-|full' 'slow1|-|full' 'slow2|-|full' > "$tree/tests/registry"
printf '%s\n' 'int main(void) { return 0; }' > "$tree/tools/setup/hostcheck.c"
cat > "$tree/Makefile" <<'MK'
components prepare-all:
	@:
prepare-tools:
	@echo -
run-failing:
	@cp "$$RACH2_RUN_UTIL" helper
	@read -r ready < ready; echo failing-start >> events; exit 1
run-slow1:
	@echo slow1-start >> events; echo ready > ready; read -r release < release; \
	 echo slow1-end >> events; touch slow1.done
run-slow2:
	@echo slow2-start >> events; echo release > release
MK
echo "selftest-stop: artifacts: $work"

# check NAME EXPECTED ACTUAL: count one check, and report it failed when
# ACTUAL is not EXPECTED.
check()
{
    total=$((total + 1))
    if [ "$2" = "$3" ]; then
        echo "selftest-stop: pass: $1"
    else
        echo "selftest-stop: fail: $1: expected $2, got $3" >&2
        fails=$((fails + 1))
    fi
}

# check_case PHASE: run the three suites in the private tree, keep the
# helper and the events, and make every check of a correct stop. Return 1
# when a check fails, and 125 when the case could not be run.
check_case()
{
    phase=$1
    rm -f "$tree/ready" "$tree/release" "$tree/events" "$tree/slow1.done" "$tree/helper"
    : > "$tree/events"
    mkfifo "$tree/ready" "$tree/release" || return 125
    (
        unset RACH2_RUN_GROUP RACH2_RUN_WORK RACH2_RUN_LOCK RACH2_HOSTCHECK
        unset RACH2_KERNEL RACH2_TIMEOUT_FACTOR
        RACH2_WORKERS=2 RACH2_MEMORY_MIB=1024 RACH2_STOP_ON_FAIL=1 RACH2_IDLE_TIMEOUT=30
        TMPDIR=$tree/tmp
        export RACH2_WORKERS RACH2_MEMORY_MIB RACH2_STOP_ON_FAIL RACH2_IDLE_TIMEOUT TMPDIR
        cd "$tree"
        exec sh tests/run failing slow1 slow2
    ) > "$work/$phase.log" 2>&1 &
    runner=$!
    rc=0
    wait "$runner" || rc=$?
    runner=0
    cat "$work/$phase.log"
    cp "$tree/helper" "$work/$phase.helper" || return 125
    cp "$tree/events" "$work/$phase.events" || return 125
    cksum "$source" "$tree/tests/run" "$work/$phase.helper" > "$work/$phase.cksum" || return 125
    total=0
    fails=0
    check 'the failing suite ran' 1 "$(grep -c '^failing-start$' "$tree/events")"
    check 'the running suite started' 1 "$(grep -c '^slow1-start$' "$tree/events")"
    check 'a set stopped on its first failure fails' 1 "$rc"
    check 'the running suite is stopped and named' 1 \
        "$(grep -c '^run: stopped on the first failure; stopped part way: slow1$' "$work/$phase.log")"
    check 'the queued suite is named' 1 \
        "$(grep -c '^run: stopped on the first failure; not started: slow2$' "$work/$phase.log")"
    check 'the running suite did not reach its end' 0 \
        "$(grep -c '^slow1-end$' "$tree/events")"
    check 'the running suite has no end marker' 0 \
        "$([ -f "$tree/slow1.done" ] && echo 1 || echo 0)"
    check 'the queued suite never started' 0 "$(grep -c '^slow2-start$' "$tree/events")"
    echo "selftest-stop: result: $phase: $((total - fails)) of $total checks passed"
    [ "$fails" -eq 0 ]
}

check_case fixed
if [ "$mutate" -eq 1 ]; then
    for fault in status continue; do
        case $fault in
            status)
                [ "$(grep -c 'return WIFEXITED(status) ? WEXITSTATUS(status)' "$source")" -eq 1 ]
                sed 's/return WIFEXITED(status) ? WEXITSTATUS(status)/return WIFEXITED(status) ? 0/' \
                    "$work/runutil.saved" > "$source"
                ;;
            continue)
                # shellcheck disable=SC2016
                [ "$(grep -Fc '[ "$rc" = 0 ] || [ -z "${RACH2_STOP_ON_FAIL:-}" ] || stopped=1' "$tree/tests/run")" -eq 1 ]
                # shellcheck disable=SC2016
                sed '/\[ "$rc" = 0 \] || \[ -z "${RACH2_STOP_ON_FAIL:-}" \] || stopped=1/s/stopped=1/:/' \
                    "$work/run.saved" > "$tree/tests/run"
                ;;
        esac
        touch "$source" "$tree/tests/run"
        status=0
        check_case "$fault" > "$work/$fault.checks" 2>&1 || status=$?
        cat "$work/$fault.checks"
        [ "$status" -eq 1 ]
        grep -q '^selftest-stop: fail: the queued suite never started:' "$work/$fault.checks"
        grep -q '^selftest-stop: fail: the running suite did not reach its end:' "$work/$fault.checks"
        if [ "$fault" = status ]; then
            if cmp -s "$work/fixed.helper" "$work/status.helper"; then
                echo 'selftest-stop: mutation did not change the helper' >&2
                exit 1
            fi
            grep -q '^run: failing: PASS in ' "$work/status.log"
        else
            grep -q '^run: failing: FAIL in ' "$work/continue.log"
        fi
        restore
        check_case "$fault-restored"
        cmp "$work/fixed.helper" "$work/$fault-restored.helper"
        cmp "$work/runutil.saved" "$source"
        cmp "$work/run.saved" "$tree/tests/run"
    done
fi
echo 'selftest-stop: result: stop-on-failure checks passed'
