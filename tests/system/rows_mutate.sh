#!/bin/sh
# Prove six rows that earlier milestones added with no recorded run against
# the fault they catch. Each fault is put back alone into the main tree, the
# product it lives in is rebuilt by the row's own target, and the row that
# catches it must fail with its own words:
#
#   entry 113  the runner's product check, by the runner's self-test
#   entry 120  a table entry numbered as a disk node, by devprobe
#   entry 121  a one-shot handler entered at zero, by sigprobe
#   entry 138  two proc files with one inode, by procprobe
#   entry 109  the sealed root without update, by the sealed suite
#   entry 175  an arping that fails, by the dhcp suite's status step
#
# The changed source is restored and touched after each fault, and the trap
# restores it and rebuilds the kernel if the run is stopped, after the row's
# processes are stopped. The kernel rebuilt at the end must be the first one,
# byte for byte, so the full set run on this tree afterwards is what shows
# each restored row passing; this script does not run the rows on the
# unmodified tree. It refuses to start while a suite holds the run lock. The
# kernel, the image and the rows are those of the architecture ARCH names.
#
# Usage: sh tests/system/rows_mutate.sh [--texts]
set -eu
repo=$(CDPATH='' cd -- "$(dirname -- "$0")/../.." && pwd)
cd "$repo"

# apply_fault ARGS...: put one fault into its source; with
# RACH2_FAULT_TEXTS set, only check its text and write nothing.
apply_fault()
{
    python3 - "$1" <<'PY'
from pathlib import Path
import sys
import os
import pathlib
if os.environ.get('RACH2_FAULT_TEXTS'):
    pathlib.Path.write_text = lambda self, *args, **kwargs: 0
changes = {
'runner': ('tests/run',
    '    if [ -f "$WORK/inventory.products" ]; then\n',
    '    if false; then\n'),
'devprobe': ('rach2/sys/fs/devfs.c',
    '    const struct disk_node *node = disk_node_of(dev);\n\n    if (node) {\n        return DEVFS_NDEV',
    '    const struct disk_node *node = (const struct disk_node *)dev;\n\n    if (node) {\n        return DEVFS_NDEV'),
'sigprobe': ('rach2/sys/syscall_proc.c',
    '        if (sigframe_setup(tf, signo, sa, &info, oldmask) != 0) {\n',
    '        if (handler_flags & SA_RESETHAND) {\n'
    '            sa->handler = KSIG_DFL;\n        }\n'
    '        if (sigframe_setup(tf, signo, sa, &info, oldmask) != 0) {\n'),
'procprobe': ('rach2/sys/fs/procfs.c',
    '        f->origin_file = SYNTH_PROC_INTERRUPTS;\n',
    '        f->origin_file = SYNTH_PROC_LOOP;\n'),
'sealed': ('krffs/manifests/rach2-metal-disk.manifest',
    'file 0755 /bin/update build/utils/update\n', ''),
'arping': ('tests/system/suites.mk',
    'send=arping -f -w 5 192.168.90.2 > /tmp/ap',
    'send=arping -f -w 5 -I eth9 192.168.90.2 > /tmp/ap'),
}
path, old, new = changes[sys.argv[1]]
p = Path(path)
s = p.read_text()
assert s.count(old) == 1, sys.argv[1]
p.write_text(s.replace(old, new))
PY
}

if [ "${1:-}" = --texts ]; then
    RACH2_FAULT_TEXTS=1
    export RACH2_FAULT_TEXTS
    for name in runner devprobe sigprobe procprobe sealed arping; do
        apply_fault "$name" >/dev/null
    done
    echo 'rows-mutation: texts: every fault is once in its source'
    exit 0
fi


RACH2_ARCH=${RACH2_ARCH:-${ARCH:-i686}}
REPO=$repo
. "$repo/mk/arch.sh"
RACH2_QEMU=$ARCH_QEMU
export RACH2_QEMU ARCH_BUILD
tree=build$ARCH_SUFFIX
[ "$#" -eq 0 ] || { echo 'rows-mutation: usage: rows_mutate.sh' >&2; exit 2; }
. "$repo/tests/system/runlock.sh"
runlock_hold "$repo" rows-mutation "$repo/tests/system/rows_mutate.sh" "$@"
mkdir -p build/tests
work=$(mktemp -d "$repo/build/tests/rows-mutation.XXXXXX")
sources="tests/run rach2/sys/fs/devfs.c rach2/sys/syscall_proc.c rach2/sys/fs/procfs.c
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
        make -s rach2 test-image "ARCH=$RACH2_ARCH" >"$work/cleanup.build.log" 2>&1 || :
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
        procprobe) changed=rach2/sys/fs/procfs.c ;;
        sealed) changed=krffs/manifests/rach2-metal-disk.manifest ;;
        arping) changed=tests/system/suites.mk ;;
    esac
    apply_fault "$1"
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

# One boot of the default suite on the rebuilt kernel. The runner's line for
# the named probe must end as the second argument says, and this boot's guest
# log must hold the third: the words the probe prints only when its row
# fails, or, for a probe the fault kills before the row can report, the
# kernel's record of the kill. A fault can take later probes with it, which
# proves nothing more.
probe_phase()
{
    name=$1
    mutate "$name"
    rm -f "$tree"/images/default-test.log
    run_row "$name" make run-default "ARCH=$RACH2_ARCH"
    [ ! -f "$tree"/images/default-test.log ] ||
        cp "$tree"/images/default-test.log "$work/$name.guest.log"
    if ! failed_with "^ *$name +$2 +FAIL" "$work/$name.log" ||
        ! grep -Fqs -- "$3" "$work/$name.guest.log"; then
        echo "rows-mutation: $name did not fail its row with its fault compiled in" >&2
        exit 1
    fi
    echo "rows-mutation: result: $name failed its row with its fault, status $status"
    restore
}

echo "rows-mutation: logs: $work"
make -s rach2 "ARCH=$RACH2_ARCH" >"$work/first.build.log" 2>&1 ||
    { cat "$work/first.build.log"; exit 1; }
cp "$tree"/rach2/kernel "$work/first.kernel"

mutate runner
run_row runner make run-runner "ARCH=$RACH2_ARCH"
failed_with "FAIL: a suite that remakes a product of the run's target passes" "$work/runner.log" ||
    { echo 'rows-mutation: the runner check did not fail with its fault in' >&2; exit 1; }
echo "rows-mutation: result: runner failed with its fault, status $status"
restore

# The devprobe fault numbers a table entry as a disk node, in entry_index
# alone. Without the check in disk_node_of itself, each open of a table
# device would also add one to a word of kernel memory through the misread
# node's disk field (entry 207), and the probe could fail on that and not on
# its row. The sigprobe fault makes the first one-shot delivery jump to
# address zero, so the probe dies before its row reports.
probe_phase devprobe 'status=[0-9]+' 'fstat and the /dev listing give /dev/null one inode'
probe_phase sigprobe 'killed by signal 11' 'bad read at 0x0 (err'
probe_phase procprobe 'status=[0-9]+' 'and the open files have different inodes too'

mutate sealed
run_row sealed make run-sealed "ARCH=$RACH2_ARCH"
failed_with 'the installed root runs no update process' "$work/sealed.log" ||
    { echo 'rows-mutation: the sealed row did not fail without update' >&2; exit 1; }
echo "rows-mutation: result: sealed failed without update, status $status"
restore

# The guest has no eth9, so arping exits 2 at once; the guest must print that
# status and the step that checks for 0 must fail. The step that checks for
# the reply fails with it, so the harness's trace is what shows the status
# step among the failed ones. An address nothing answers would fail the
# status step too, but only at arping's deadline, which the dhcp suite judges
# in a step of its own.
mutate arping
run_row arping env RACH2_TRACE="$work/arping.trace" make dhcp-card "ARCH=$RACH2_ARCH"
if ! failed_with 'HARNESS: [0-9]+ step\(s\) failed' "$work/arping.log" ||
    ! grep -qs '^step expect=ARPING-STATUS:0 fail ' "$work/arping.trace" ||
    ! grep -q 'ARPING-STATUS:2' "$tree"/images/dhcp.log; then
    echo 'rows-mutation: the dhcp status step did not fail on a failed arping' >&2
    exit 1
fi
echo "rows-mutation: result: arping failed without its interface, status $status"
restore

make -s rach2 test-image "ARCH=$RACH2_ARCH" >"$work/restored.build.log" 2>&1 ||
    { cat "$work/restored.build.log"; exit 1; }
cmp "$tree"/rach2/kernel "$work/first.kernel"
for f in $sources; do
    cmp "$f" "$work/saved/$f"
done
echo 'rows-mutation: result: pass: each fault failed its row; the restored kernel equals the first'
