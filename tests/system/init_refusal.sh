#!/bin/sh
#
# init_refusal.sh: a root whose /sbin/init cannot be read halts the boot.
#
# The kernel falls to its debug shell, which authenticates nobody, only when the
# root it mounted has no /sbin/init at all. Any other failure to look the
# program up, a symbolic link loop or an I/O error, is a machine with an init it
# cannot reach, and the boot halts with a line saying so. The root here is an
# ext2 volume whose /sbin/init is a symbolic link to itself, which the walk
# follows until it answers ELOOP. The row asserts both halves the bootline suite
# asserts of every refusal: the refusal is printed, and the machine halts with
# no shell reached, as QEMU's monitor finds it through tests/system/monitor.sh.
#
# Usage: init_refusal.sh KERNEL HARNESS MKMBR OUT
#
# OUT is a path prefix: OUT.disk is the disk and OUT.log the console, which
# is kept, as is the monitor's OUT.monitor.log after a failure. `tests/run
# bootline` runs this on the kernel under test, and init_refusal_mutate.sh
# runs it on kernels built from a private copy.
#
set -u
KERNEL=$1; HARNESS=$2; MKMBR=$3; OUT=$4
REPO=$(cd "$(dirname "$0")/../.." && pwd)
RUNUTIL="$REPO/build/tests/system/runutil"
# shellcheck source=tests/system/deadline.sh
. "$REPO/tests/system/deadline.sh"
# shellcheck source=tests/system/monitor.sh
. "$REPO/tests/system/monitor.sh"
HPID=""
# Stop the harness, if it runs, and the monitor's reader.
stop() {
    hpid=$HPID
    HPID=""
    if [ -n "$hpid" ]; then
        kill "$hpid" 2>/dev/null || true
        wait "$hpid" 2>/dev/null || true
    fi
    monitor_close
    rm -f "$OUT.halted"
}
trap stop EXIT
trap 'stop; exit 130' INT
trap 'stop; exit 143' TERM
trap 'stop; exit 129' HUP
# Report $1 and exit 1.
fail() { echo "init-refusal: FAIL: $1" >&2; exit 1; }

. "$REPO/tools/setup/e2fsprogs_path.sh"

TREE="$OUT.tree"
VOL="$OUT.vol"
DISK="$OUT.disk"
LOG="$OUT.log"
rm -rf "$TREE" "$VOL" "$DISK" "$LOG"
mkdir -p "$TREE/sbin" || fail "the staging tree"
ln -s init "$TREE/sbin/init" || fail "the link from /sbin/init to itself"
# The profile mkext2.sh gives a root, so the driver mounts the volume
# writable, as it mounts a machine's.
PROFILE="sparse_super,large_file,filetype,^dir_index,resize_inode,^ext_attr,^has_journal,^extent,^64bit,^flex_bg,^metadata_csum,^huge_file,^dir_nlink,^extra_isize"
: > "$VOL"
E2FSPROGS_FAKE_TIME=1 mke2fs -q -F -t ext2 -b 4096 -I 128 -O "$PROFILE" -d "$TREE" "$VOL" 8M \
    > "$OUT.mke2fs.log" 2>&1 || fail "the ext2 volume; see $OUT.mke2fs.log"
{
    dd if=/dev/zero bs=1048576 count=1 2>/dev/null
    cat "$VOL"
    dd if=/dev/zero bs=1048576 count=1 2>/dev/null
} > "$DISK" || fail "the disk"
"$MKMBR" --lba "$DISK" 2048:16384:83 > /dev/null || fail "the disk's table"
rm -rf "$TREE" "$VOL"

# The refusal is the end of the boot, so its first line is waited for under
# the run's bound and its last under the step's. The refusal's last line is
# the token, and the harness then holds the machine at a barrier, which this
# script releases once the monitor finds the processor halted with its
# interrupts off: a kernel that printed the refusal and went on would not be,
# and the harness fails the run on the shell's banner or the fall-through's
# line wherever they appear.
TOKEN="is not the answer to a disk that cannot be read"
rm -f "$OUT.halted"
monitor_open "$OUT.monitor"
"$HARNESS" --kernel "$KERNEL" --qemu-arg=-cpu --qemu-arg=max \
    --qemu-arg=-monitor --qemu-arg="pipe:$OUT.monitor" \
    --drive "file=$DISK,format=raw,if=ide,index=0,media=disk" \
    --append "rach2.root=ext2" --timeout 180 --steptmo 30 --log "$LOG" \
    --ok "$TOKEN" \
    --never "Kernel debug shell active" --never "falling back to the debug shell" \
    "boot=boot: refused: /sbin/init cannot be read" \
    "expect=$TOKEN" "barrier=$OUT.halted" > "$OUT.harness.log" 2>&1 &
HPID=$!
until grep -aq "$TOKEN" "$LOG" 2>/dev/null; do
    kill -0 "$HPID" 2>/dev/null || break
    "$RUNUTIL" sleep 0.2
done
if grep -aq "$TOKEN" "$LOG" 2>/dev/null; then
    monitor_wait_halted "$(scaled 30)" || fail "the boot did not halt after its refusal: $MONITOR_WHY"
    : > "$OUT.halted"
fi
hpid=$HPID
HPID=""
if ! wait "$hpid"; then
    sed -n 's/^HARNESS: /init-refusal: /p' "$OUT.harness.log" >&2
    tail -20 "$LOG" >&2
    fail "a root whose /sbin/init is a link to itself did not halt with its refusal"
fi
grep -aq "ext2: root: /dev/disk0p1" "$LOG" || fail "the ext2 root was not mounted, so the lookup was never made"
grep -aq "boot: refused: /sbin/init cannot be read (-40)" "$LOG" ||
    fail "the refusal did not name ELOOP, -40"
rm -f "$DISK" "$OUT.monitor.log"
echo "init-refusal: a root whose /sbin/init is a symbolic link loop is refused, and the boot stops there: OK"
