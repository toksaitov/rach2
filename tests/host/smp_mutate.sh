#!/bin/sh
# Prove the processors rach2/sys/proc/smp.c interrupts: a shootdown passes
# an idle processor that kept the directory by and marks it stale, and
# interrupts one that runs a thread of the directory; a release interrupts
# the idle processor that kept the directory about to be freed.
# tests/host/smp_hosttest.c is compiled against a private copy of the
# kernel's sources with each fault put in turn, and its row must fail; the
# restored copy must pass again.
#
# Usage: smp_mutate.sh [--check | --texts]
# With --check it compiles and runs the unmodified test alone. The faults:
# a shootdown that interrupts idle processors too, as before lazy TLB; one
# that passes every processor by, which leaves a running thread with a
# translation of a freed page; a release that interrupts none, which
# leaves an idle processor on a freed directory; and a stop that does not
# wait for the processors it asks, which leaves one writing the log under
# the panic's record; and a started processor that comes online whatever
# the boot decided, which leaves one online and uncounted.
set -eu
repo=$(CDPATH='' cd -- "$(dirname -- "$0")/../.." && pwd)
cd "$repo"

# apply_fault ARGS...: put one fault into its source; with
# RACH2_FAULT_TEXTS set, only check its text and write nothing.
apply_fault()
{
    python3 - "$1" "$2" <<'PY'
from pathlib import Path
import sys
import os
import pathlib
if os.environ.get('RACH2_FAULT_TEXTS'):
    pathlib.Path.write_text = lambda self, *args, **kwargs: 0

path = Path(sys.argv[1])
source = path.read_text()
fault = sys.argv[2]
function = {
    'no-skip': 'void smp_tlb_shootdown(const struct page_dir *dir, uintptr_t va)\n{',
    'skip-all': 'void smp_tlb_shootdown(const struct page_dir *dir, uintptr_t va)\n{',
    'no-release': 'void smp_space_release(const struct page_dir *dir)\n{',
    'no-stop-wait': 'void smp_stop_others(void)\n{',
    'late-admit': 'static void smp_cpu_main(int cpu)\n{',
}[fault]
changes = {
    'no-skip': (
        '        if (other->current->space_root == 0) {\n',
        '        if (0) {\n'),
    'skip-all': (
        '        if (other->current->space_root == 0) {\n',
        '        if (1) {\n'),
    'no-release': (
        '    if (cpus_online < 2) {\n',
        '    if (cpus_online > 0) {\n'),
    'no-stop-wait': (
        '        for (waited = 0; asked[c] && ',
        '        for (waited = 0; 0 && asked[c] && '),
    'late-admit': (
        '    if (arch_atomic_cmpxchg(&self->admit, CPU_STARTING, CPU_ADMITTED) != CPU_STARTING) {\n',
        '    if (0 && arch_atomic_cmpxchg(&self->admit, CPU_STARTING, CPU_ADMITTED) != CPU_STARTING) {\n'),
}
old, new = changes[fault]
if source.count(function) != 1:
    raise SystemExit('smp-proof: the function is not unique: ' + fault)
start = source.index(function)
end = source.index('\n}\n', start)
body = source[start:end]
if body.count(old) != 1:
    raise SystemExit('smp-proof: the fault\'s pattern is not unique: ' + fault)
path.write_text(source[:start] + body.replace(old, new) + source[end:])
PY
}

faults='no-skip skip-all no-release no-stop-wait late-admit'
if [ "${1:-}" = --texts ]; then
    for fault in $faults; do
        RACH2_FAULT_TEXTS=1 apply_fault rach2/sys/proc/smp.c "$fault" >/dev/null || exit 1
    done
    echo 'smp-proof: texts: every fault is once in its source'
    exit 0
fi

[ "$#" -eq 0 ] || [ "$*" = --check ] || {
    echo 'smp-proof: usage: smp_mutate.sh [--check]' >&2
    exit 2
}
mkdir -p build/tests
work=$(mktemp -d "$repo/build/tests/smp-proof.XXXXXX")
child=
mkdir "$work/rach2"
cp -R rach2/sys "$work/rach2/"
smp="$work/rach2/sys/proc/smp.c"
cp "$smp" "$work/smp.saved"

cleanup()
{
    if [ -n "$child" ]; then
        kill -TERM "$child" 2>/dev/null || :
        wait "$child" 2>/dev/null || :
        child=
    fi
    cp "$work/smp.saved" "$smp"
    touch "$smp"
}
trap cleanup EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM

# The test's own dependency and flag lines, as the host suite reads them,
# with its kernel sources taken from the private tree.
test=tests/host/smp_hosttest.c
deps=$(grep -oE 'host-deps:[^*]*' "$test" | sed 's/host-deps://' | tr '\n' ' ' |
    sed "s# rach2/# $work/rach2/#g")
extra=$(grep -oE 'host-cflags:[^*]*' "$test" | sed 's/host-cflags://' | tr '\n' ' ')

run_phase()
{
    phase=$1
    expected=$2
    message=$3
    binary="$work/$phase"
    # shellcheck disable=SC2086
    if ! "${CC:-cc}" -std=c99 -D_DEFAULT_SOURCE -D_FORTIFY_SOURCE=0 -DRACH2_HOSTTEST \
        -fsanitize=address,undefined -fno-sanitize-recover=all -g -O1 \
        -Wno-empty-body -ffreestanding $extra \
        -I "$work/rach2/sys" -I "$work/rach2/sys/arch/x86" -I "$work/rach2/sys/arch/i686" \
        -I "$work/rach2/sys/lib" -I "$work/rach2/sys/proc" \
        -I utils -I krffs -I tests/host/compat \
        -o "$binary" "$test" $deps >"$binary.compile.log" 2>&1; then
        cat "$binary.compile.log" >&2
        exit 1
    fi
    cksum "$smp" "$binary" >"$binary.cksum"
    perl -e 'alarm 10; exec @ARGV or die "smp-proof: exec: $!\n"' \
        "$binary" >"$binary.log" 2>&1 &
    child=$!
    status=0
    wait "$child" || status=$?
    child=
    cat "$binary.log"
    [ "$status" -eq "$expected" ] && grep -Fq "$message" "$binary.log" || exit 1
    echo "smp-proof: result: $phase status $status"
}

echo "smp-proof: logs: $work"
run_phase fixed 0 'smp-hosttest: result: pass'
[ "$#" -eq 0 ] || exit 0
for fault in $faults; do
    apply_fault "$smp" "$fault"
    touch "$smp"
    case $fault in
        no-skip)
            message='smp-hosttest: fail: an idle processor that kept the directory is not interrupted and has it marked stale'
            ;;
        skip-all)
            message='smp-hosttest: fail: a processor that runs a thread of the directory is interrupted to drop the page'
            ;;
        no-release)
            message="smp-hosttest: fail: before a directory is freed, an idle processor that kept it gives it up for the kernel's"
            ;;
        no-stop-wait)
            message='smp-hosttest: fail: and waits a second for the one that does not take it, none for those that do'
            ;;
        late-admit)
            message='smp-hosttest: fail: and one that comes after the wait stops, neither online nor counted'
            ;;
    esac
    run_phase "$fault-mutated" 1 "$message"
    cleanup
    run_phase "$fault-restored" 0 'smp-hosttest: result: pass'
    cmp "$work/smp.saved" "$smp"
done
echo 'smp-proof: result: pass: the five faults failed their rows and the restored source passed'
