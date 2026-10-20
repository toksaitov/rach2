#!/usr/bin/env sh

#
# etudeexec.sh: Rach2 builds etude with its TinyCC recipe, and the POSIX
# executor, built the same way, passes its test on Rach2.
#
# The suite boots os-real from a copy of its disk, which has TinyCC, pdpmake
# and BusyBox, with a second disk that is a tar archive of the files the
# build and the test need: etude's source with its fixtures and schemas, the
# executor's test and its fixture directory, the probe the examples' `where`
# lines name, and tests/system/etudeexec_guest.sh. It logs in as root at the
# console, unpacks the archive from the raw disk into a tmpfs, since the
# version cache publishes by a hard link and KRFFS has none, and runs the
# guest script, whose lines it judges: `version.sh -l` builds a version and
# then reuses it, the record names the platform `rach2` and the recipe's
# `-D__rach2__`, the built `casecheck --selftest` and `history_check` pass,
# and the executor's test, compiled with the version's compiler and flags
# from its own `host-deps:` and `host-cflags:` lines, passes with every row
# run: the subreaper's, the exact processor time of a tree and the hold of
# each case before its exec among them. The boot traces each system call the
# kernel does not implement, and the suite fails if an etude program asked
# for one.
#
# Usage: etudeexec.sh <kernel> <disk> <root password> [<tree>]
#
# TREE is where the archive's files are read, the repository by default;
# tests/system/etudeexec_mutate.sh gives a private copy with a fault in it.
#

# The emulator of the architecture tests/system/suites.mk names.
QEMU=${RACH2_QEMU:-qemu-system-i386}

REPO=$(cd "$(dirname "$0")/../../.." && pwd)
# shellcheck source=tests/system/deadline.sh
. "$REPO/tests/system/deadline.sh"
set -e

KERNEL="$1"
DISK="$2"
PASSWORD="$3"
TREE="${4:-$REPO}"

if [ -z "$KERNEL" ] || [ -z "$DISK" ] || [ -z "$PASSWORD" ]; then
    echo "usage: etudeexec.sh <kernel> <disk> <root password> [<tree>]" >&2
    exit 2
fi

WORK="$DISK.work"
TAR="$DISK.tar"
LOG="$DISK.work.log"
FEED="$DISK.work.feed"
RESULTS="$DISK.work.rows"
rm -f "$WORK" "$TAR" "$LOG" "$FEED" "$RESULTS"

# The files the guest needs, by their paths in the repository.
FILES="tools/etude utils/sha256.c utils/sha256.h tests/host/etude_exec_hosttest.c
tests/host/compat/fixdir_host.c tests/host/compat/fixdir_host.h tests/auxvprobe.c
tests/system/etudeexec_guest.sh"

# The results the guest script must give, each as `etudeexec: NAME: ok`.
ROWS="built reused platform recipe casecheck history history-lost compile executor"

QPID=""

# Stop the machine and the feeder. Safe to call twice: the pid is taken and
# cleared before it is signalled, so a trap between the kill and the wait
# finds nothing to kill.
cleanup_boot() {
    qpid="$QPID"
    QPID=""
    drive_stop
    if [ -n "$qpid" ]; then
        kill -9 "$qpid" 2>/dev/null || true
        wait "$qpid" 2>/dev/null || true
    fi
    rm -f "$FEED"
}

# Stop the boot, and remove the disk's copy, the archive, the logs and the
# feed.
cleanup() {
    cleanup_boot
    rm -f "$WORK" "$TAR" "$LOG" "$FEED" "$RESULTS"
}

# shellcheck source=tests/system/drive.sh
. "$REPO/tests/system/drive.sh"

trap 'cleanup' EXIT
trap 'cleanup; exit 130' INT
trap 'cleanup; exit 143' TERM
trap 'cleanup; exit 129' HUP

# fail REASON: keep the console's log as DISK.fail.log, stop the boot, print
# REASON and exit 1.
fail() {
    if [ -f "$LOG" ]; then
        cp "$LOG" "$DISK.fail.log" 2>/dev/null || true
    fi
    cleanup_boot
    echo "etudeexec-test: FAIL: $1" >&2
    echo "etudeexec-test: the failing boot's console log: $DISK.fail.log" >&2
    exit 1
}

# shellcheck disable=SC2086 # FILES is a list of paths without spaces
(cd "$TREE" && tar -cf - $FILES) > "$TAR" || fail "cannot make the archive of $TREE"
cp "$DISK" "$WORK"

mkfifo "$FEED"
"$QEMU" -cpu max -display none -m 512 -kernel "$KERNEL" \
    -append "rach2.fsdisk rach2.trace=enosys" \
    -drive "file=$WORK,format=raw,if=ide,index=0,media=disk" \
    -drive "file=$TAR,format=raw,if=ide,index=1,media=disk" \
    -serial stdio < "$FEED" > "$LOG" 2>&1 &
QPID=$!
DRIVE_LOG=$LOG
DRIVE_FIFO=$FEED
drive_feed "$QPID"
drive_login root "$PASSWORD" "$(scaled 90)" || fail "root did not log in at the console"

# The second disk has no partition table, so its whole node is the archive.
drive_type 'mkdir /tmp/etudeexec && mount -t tmpfs tmpfs /tmp/etudeexec && tar -xf /dev/disk1 -C /tmp/etudeexec && echo UNPACK""ED'
drive_expect 'UNPACKED' "$(scaled 120)" || fail "the archive did not unpack from /dev/disk1"
drive_prompt "$(scaled 30)" || fail "no prompt after the archive unpacked"
drive_type 'sh /tmp/etudeexec/tests/system/etudeexec_guest.sh /tmp/etudeexec 2>&1'
drive_expect 'etudeexec: end' "$(scaled 1200)" || fail "the guest script did not finish"
drive_prompt "$(scaled 30)" || fail "no prompt after the guest script"

tr -d '\r' < "$LOG" | grep '^etudeexec: ' > "$RESULTS" || true
cat "$RESULTS"
bad=""
for row in $ROWS; do
    grep -qx "etudeexec: $row: ok" "$RESULTS" || bad="$bad $row"
done
[ -z "$bad" ] || fail "rows did not pass:$bad"

# A missing system call returns ENOSYS and prints nothing; the trace, on for
# this boot, names each number and the process that asked for it.
if tr -d '\r' < "$LOG" | grep "not implemented" | grep -qE "asked for by (execcheck|casecheck|history_check)"; then
    fail "an etude program asked for a system call this kernel lacks: $(tr -d '\r' < "$LOG" | grep -m1 -E 'asked for by (execcheck|casecheck|history_check)')"
fi

cleanup_boot

echo "etudeexec-test: Rach2 built etude by its TinyCC recipe, and the executor passed its test there: OK"
echo "etudeexec-test: PASS"
