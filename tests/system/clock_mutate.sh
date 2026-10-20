#!/bin/sh
# clock_mutate.sh [--texts]: show that `tests/run clock` catches the rule that
# counts one tick for each interrupt, on x86-64, the suite's one
# architecture.
#
# Usage: sh tests/system/clock_mutate.sh [--texts]
#
# The suite runs four times: as the tree has it, where it must pass; with
# `timer_tick_clock` in rach2/sys/arch/x86/pit.c returning 1 for every
# interrupt once the reference keeps time, which is the rule before the
# reference did, where the stops lose the ticks that fell due and the suite
# must fail on the clock's difference; and restored, where it must pass
# again. The fault is put into the tree's source, which the trap restores
# and touches, and the run builds the kernel each time. A delayed-console
# phase holds each CLOCKLOAD line for two host seconds before delivery, and
# the clock fault must still fail with that delay. The interval scorer is
# also proven with a fault that accepts an overlapping interval. --texts
# checks that each replacement text is once in its source.
set -eu
repo=$(CDPATH='' cd -- "$(dirname -- "$0")/../.." && pwd)
cd "$repo"
source=rach2/sys/arch/x86/pit.c

# mutate FILE OLD NEW: replace OLD, which must be once in FILE, with NEW;
# with RACH2_FAULT_TEXTS set, only check that OLD is once in FILE.
mutate()
{
    python3 - "$1" "$2" "$3" <<'PY'
import os
import pathlib
import sys

path, old, new = pathlib.Path(sys.argv[1]), sys.argv[2], sys.argv[3]
text = path.read_text()
if text.count(old) != 1:
    raise SystemExit(f"clock-mutation: not once in {path}: {old}")
if not os.environ.get("RACH2_FAULT_TEXTS"):
    path.write_text(text.replace(old, new))
PY
}

# fault: count one tick for each interrupt once the reference keeps time.
fault()
{
    mutate "$source" '    return due;' '    return 1u;'
}

# Delay result delivery without changing any reference timestamp.
delay_transport()
{
    mutate tests/system/clockload.py \
        '            lines.put((time.monotonic(), line.decode("latin-1").rstrip("\r")))' \
        '            if line.startswith(b"CLOCKLOAD"):
                time.sleep(2)
            lines.put((time.monotonic(), line.decode("latin-1").rstrip("\r")))'
}

if [ "${1:-}" = --texts ]; then
    RACH2_FAULT_TEXTS=1
    export RACH2_FAULT_TEXTS
    fault
    mutate tests/clockref.h '    if (low >= -bound && high <= bound)' \
        '    if (low >= -bound || high <= bound)'
    delay_transport
    echo 'clock-mutation: texts: every fault is once in its source'
    exit 0
fi

. "$repo/tests/system/runlock.sh"
runlock_hold "$repo" clock-mutation "$repo/tests/system/clock_mutate.sh" "$@"
mkdir -p build
work=
child=
changed=
keep=

# Stop the running suite's process group and wait until it is gone.
stop_child()
{
    if [ -n "$child" ]; then
        kill -TERM "-$child" 2>/dev/null || :
        wait "$child" 2>/dev/null || :
        while kill -0 "-$child" 2>/dev/null; do
            sleep 1
        done
        child=
    fi
}

# Put back the saved source and touch it, so that the next build compiles it.
restore()
{
    cp "$work/pit.c.saved" "$source"
    cp "$work/clockload.py.saved" tests/system/clockload.py
    cp "$work/clockref.h.saved" tests/clockref.h
    touch "$source" tests/clockref.h
    changed=
}

# Stop the suite and, when the fault is in the source, restore it and
# rebuild the kernel. The work directory is removed too, unless the proof
# failed after printing its path, which sets `keep`; a signal clears `keep`.
cleanup()
{
    exit_status=$?
    stop_child
    if [ -n "$changed" ]; then
        restore
        make -s rach2 musl-programs build/tests/system/runtests ARCH=x86_64 >"$work/cleanup.build.log" 2>&1 || :
    fi
    if [ "$exit_status" -eq 0 ] || [ -z "$keep" ]; then
        rm -rf ${work:+"$work"}
    fi
}
trap cleanup EXIT
trap 'keep=; exit 129' HUP
trap 'keep=; exit 130' INT
trap 'keep=; exit 143' TERM
work=$(mktemp -d "$repo/build/clock-proof.XXXXXX")
cp "$source" "$work/pit.c.saved"
cp tests/system/clockload.py "$work/clockload.py.saved"
cp tests/clockref.h "$work/clockref.h.saved"

# run_phase PHASE EXPECTED MESSAGE: run the suite and keep its kernel. With
# EXPECTED `pass` it must pass, otherwise fail, and either way its output
# must have MESSAGE.
run_phase()
{
    phase=$1
    expected=$2
    message=$3
    ARCH=x86_64 build/tests/system/runutil group tests/run clock >"$work/$phase.out" 2>&1 &
    child=$!
    status=0
    wait "$child" || status=$?
    stop_child
    cat "$work/$phase.out"
    cp build/x86_64/rach2/kernel "$work/$phase.kernel"
    for log in build/x86_64/images/clock-test.*log; do
        [ ! -f "$log" ] || cp "$log" "$work/$phase.${log##*/clock-test.}"
    done
    if [ "$expected" = pass ]; then
        [ "$status" -eq 0 ] || exit 1
    else
        [ "$status" -ne 0 ] || {
            echo "clock-mutation: the fault passed: $phase" >&2; exit 1;
        }
    fi
    grep -Fq "$message" "$work/$phase.out" || {
        echo "clock-mutation: $phase did not end with: $message" >&2; exit 1;
    }
    echo "clock-mutation: result: $phase status $status"
}

echo "clock-mutation: logs: $work"
keep=1
make -s build/tests/system/runutil >"$work/prepare.log" 2>&1 || {
    cat "$work/prepare.log" >&2; exit 1;
}
make -s build/tests/system/runtests >"$work/scorer.build.log" 2>&1
build/tests/system/runtests --selftest
changed=1
mutate tests/clockref.h '    if (low >= -bound && high <= bound)' \
    '    if (low >= -bound || high <= bound)'
make -s build/tests/system/runtests >>"$work/scorer.build.log" 2>&1
if build/tests/system/runtests --selftest >"$work/scorer.out" 2>&1; then
    echo 'clock-mutation: the interval fault passed' >&2; exit 1
fi
cat "$work/scorer.out"
grep -q 'clock interval' "$work/scorer.out" || exit 1
restore
make -s build/tests/system/runtests >>"$work/scorer.build.log" 2>&1
build/tests/system/runtests --selftest
echo 'clock-mutation: result: interval fault failed; restored scorer passed'
run_phase fixed pass 'clock-test: ok: both clocks kept the host'

changed=1
delay_transport
run_phase delayed pass 'clock-test: ok: both clocks kept the host'

changed=1
fault
run_phase one-per-interrupt fail 'CLOCKLOAD FAIL (clock drift)'
restore

run_phase restored pass 'clock-test: ok: both clocks kept the host'
cmp "$work/pit.c.saved" "$source"
cmp "$work/clockload.py.saved" tests/system/clockload.py
cmp "$work/clockref.h.saved" tests/clockref.h
if cmp -s "$work/fixed.kernel" "$work/one-per-interrupt.kernel"; then
    echo 'clock-mutation: the kernel did not change' >&2; exit 1
fi
cmp "$work/fixed.kernel" "$work/restored.kernel"
echo 'clock-mutation: result: pass: the one-per-interrupt rule failed the suite; the restored kernel passed'
