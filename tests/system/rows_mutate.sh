#!/bin/sh
# Prove six rows that earlier milestones added with no recorded run against
# the fault they catch. Each fault is put back alone into the main tree, the
# product it lives in is rebuilt by the row's own target, and the row that
# catches it must fail with its own words:
#
#   entry 113  the runner's product check, by the runner's self-test
#   entry 120  a table entry taken for a disk node, by devprobe
#   entry 121  a one-shot handler entered at zero, by sigprobe
#   entry 138  two proc files with one inode, by procprobe
#   entry 109  the sealed root without update, by the sealed suite
#   entry 175  an arping that gets no answer, by the dhcp suite's status step
#
# The changed source is restored and touched after each fault, and the trap
# restores it and rebuilds the kernel if the run is stopped, after the row's
# processes are stopped. The kernel rebuilt at the end must be the first one,
# byte for byte, so the full set run on this tree afterwards is what shows
# each restored row passing; this script does not run the rows on the
# unmodified tree. It refuses to start while a suite holds the run lock.
#
# Usage: sh tests/system/rows_mutate.sh
set -eu
repo=$(CDPATH='' cd -- "$(dirname -- "$0")/../.." && pwd)
cd "$repo"
[ "$#" -eq 0 ] || { echo 'rows-mutation: usage: rows_mutate.sh' >&2; exit 2; }
[ ! -d build/.run.lock ] || {
    echo 'rows-mutation: another suite owns this tree' >&2; exit 1;
}
mkdir -p build/tests
work=$(mktemp -d "$repo/build/tests/rows-mutation.XXXXXX")
sources="tests/run rach2/sys/fs/devfs.c rach2/sys/syscall_proc.c rach2/sys/fs/synthfs.h
krffs/manifests/rach2-metal-disk.manifest tests/system/suites.mk"
for f in $sources; do
    mkdir -p "$work/saved/$(dirname "$f")"
    cp "$f" "$work/saved/$f"
done
cc -O2 -Wall -Wextra -Werror tests/system/runner/runutil.c -o "$work/runutil"
child=
changed=

stop_child()
{
    if [ -n "$child" ]; then
        "$work/runutil" killgroup "$child" 15 2>/dev/null || :
        wait "$child" 2>/dev/null || :
        while kill -0 "-$child" 2>/dev/null; do
            sleep 1
        done
        child=
    fi
}

restore()
{
    if [ -n "$changed" ]; then
        cp "$work/saved/$changed" "$changed"
        touch "$changed"
        changed=
    fi
}

cleanup()
{
    stop_child
    if [ -n "$changed" ]; then
        restore
        make -s rach2 test-image >"$work/cleanup.build.log" 2>&1 || :
    fi
}
trap cleanup EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM

mutate()
{
    case $1 in
        runner) changed=tests/run ;;
        devprobe) changed=rach2/sys/fs/devfs.c ;;
        sigprobe) changed=rach2/sys/syscall_proc.c ;;
        procprobe) changed=rach2/sys/fs/synthfs.h ;;
        sealed) changed=krffs/manifests/rach2-metal-disk.manifest ;;
        arping) changed=tests/system/suites.mk ;;
    esac
    python3 - "$1" <<'PY'
from pathlib import Path
import sys
changes = {
'runner': ('tests/run',
    '    if [ -f "$WORK/inventory.products" ]; then\n',
    '    if false; then\n'),
'devprobe': ('rach2/sys/fs/devfs.c',
    '    if (dev->ops != &diskwin_ops) {\n        return 0;\n    }\n    return (const struct disk_node *)dev;\n',
    '    return (const struct disk_node *)dev;\n'),
'sigprobe': ('rach2/sys/syscall_proc.c',
    '    handler = sa->handler;\n    if (sa->flags & SA_RESETHAND) {\n'
    '        sa->handler = KSIG_DFL; /* one-shot */\n    }\n',
    '    if (sa->flags & SA_RESETHAND) {\n'
    '        sa->handler = KSIG_DFL; /* one-shot */\n    }\n    handler = sa->handler;\n'),
'procprobe': ('rach2/sys/fs/synthfs.h',
    '#define SYNTH_PROC_INTERRUPTS  16\n',
    '#define SYNTH_PROC_INTERRUPTS  15\n'),
'sealed': ('krffs/manifests/rach2-metal-disk.manifest',
    'file 0755 /bin/update build/utils/update\n', ''),
'arping': ('tests/system/suites.mk',
    'send=arping -f -w 5 192.168.90.2 > /tmp/ap',
    'send=arping -f -w 5 192.168.90.9 > /tmp/ap'),
}
path, old, new = changes[sys.argv[1]]
p = Path(path)
s = p.read_text()
assert s.count(old) == 1, sys.argv[1]
p.write_text(s.replace(old, new))
PY
    touch "$changed"
}

# Run a row's command in a process group of its own; its output is kept in
# $work/NAME.log and printed, and its status is in $status.
run_row()
{
    name=$1
    shift
    "$work/runutil" group "$@" >"$work/$name.log" 2>&1 &
    child=$!
    status=0
    wait "$child" || status=$?
    stop_child
    cat "$work/$name.log"
}

# True when the row just run failed and its log says why: a non-zero status
# and a line matching the pattern.
failed_with()
{
    [ "$status" -ne 0 ] && grep -Eq -- "$1" "$2"
}

# One boot of the default suite on the rebuilt kernel; the named probe must
# fail, with a status of its own or killed by a signal, as a handler entered
# at zero is. A fault can take later probes with it, which proves nothing
# more.
probe_phase()
{
    name=$1
    mutate "$name"
    run_row "$name" make run-default
    [ ! -f build/images/default-test.log ] || cp build/images/default-test.log "$work/$name.guest.log"
    failed_with "^ *$name +(status=[0-9]+|killed by signal [0-9]+) +FAIL" "$work/$name.log" ||
        { echo "rows-mutation: $name did not fail with its fault compiled in" >&2; exit 1; }
    echo "rows-mutation: result: $name failed with its fault, status $status"
    restore
}

echo "rows-mutation: logs: $work"
make -s rach2 >"$work/first.build.log" 2>&1 || { cat "$work/first.build.log"; exit 1; }
cp build/rach2/kernel "$work/first.kernel"

mutate runner
run_row runner make run-runner
failed_with "FAIL: a suite that remakes a product of the run's target passes" "$work/runner.log" ||
    { echo 'rows-mutation: the runner check did not fail with its fault in' >&2; exit 1; }
echo "rows-mutation: result: runner failed with its fault, status $status"
restore

probe_phase devprobe
probe_phase sigprobe
probe_phase procprobe

mutate sealed
run_row sealed make run-sealed
failed_with 'the installed root runs no update process' "$work/sealed.log" ||
    { echo 'rows-mutation: the sealed row did not fail without update' >&2; exit 1; }
echo "rows-mutation: result: sealed failed without update, status $status"
restore

# Nothing answers 192.168.90.9, so arping exits 1; the guest must print that
# status and the step that wants 0 must fail.
mutate arping
run_row arping make dhcp-card
if ! failed_with 'HARNESS: [0-9]+ step\(s\) failed' "$work/arping.log" ||
    ! grep -q 'ARPING-STATUS:1' build/images/dhcp.log; then
    echo 'rows-mutation: the dhcp status step did not fail on an unanswered arping' >&2
    exit 1
fi
echo "rows-mutation: result: arping failed without an answer, status $status"
restore

make -s rach2 test-image >"$work/restored.build.log" 2>&1 ||
    { cat "$work/restored.build.log"; exit 1; }
cmp build/rach2/kernel "$work/first.kernel"
for f in $sources; do
    cmp "$f" "$work/saved/$f"
done
echo 'rows-mutation: result: pass: each fault failed its row; the restored kernel equals the first'
