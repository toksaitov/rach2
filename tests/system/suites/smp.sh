#!/usr/bin/env sh

#
# smp.sh: the kernel on one processor and on four, and a build that uses
# every one of them.
#
# The test image boots three times under the harness, with one processor,
# with four, and with four and `maxcpus=2`, and each boot runs musl/smpmusl
# with the number of processors it should have: the processors every way a
# program asks for them, and a stress of fork, exec, pipes, futexes and
# munmap across them, which a page unmapped on one processor and read on
# another ends (tests/musl/smpmusl.c). Each boot names its own count, which
# the runner's shim keeps, so the suite's counts are its own whatever
# RACH2_CPUS says. A fourth boot, with two processors, runs the debug shell's
# preemption test, whose two kernel threads must each be preempted by the
# tick and run again.
#
# Then the installed system boots from its disk with four processors, as
# the unprivileged test account, compiles the staged ish sources four times
# over, every compile at once, and checks in the guest that each
# processor's user ticks in /proc/stat grew while it did. The check reads
# the kernel's counts and no clock.
#
# Every assertion is made in the guest, over that command's own output, and
# reported as one token written split, so that the console's echo of the
# command cannot satisfy its own wait; a line is typed only after the one
# before it answered (tests/system/drive.sh).
#
# Usage: smp.sh <kernel> <test image> <harness> <disk> <test password>
#

QEMU=${RACH2_QEMU:-qemu-system-i386}
REPO=$(cd "$(dirname "$0")/../../.." && pwd)
set -e

KERNEL="$1"
IMAGE="$2"
HARNESS="$3"
DISK="$4"
TESTPASSWORD="$5"

if [ -z "$KERNEL" ] || [ -z "$IMAGE" ] || [ -z "$HARNESS" ] || [ -z "$DISK" ] ||
    [ -z "$TESTPASSWORD" ]; then
    echo "usage: smp.sh <kernel> <test image> <harness> <disk> <test password>" >&2
    exit 2
fi

LOG="$DISK.log"
FEED="$DISK.feed"
rm -f "$LOG" "$FEED" "$DISK".probe-*.log "$DISK.fail.log"

# shellcheck source=tests/system/drive.sh
. "$REPO/tests/system/drive.sh"

QPID=""

cleanup() {
    qpid="$QPID"
    QPID=""
    drive_stop
    if [ -n "$qpid" ]; then
        kill -9 "$qpid" 2>/dev/null || true
        wait "$qpid" 2>/dev/null || true
    fi
    rm -f "$FEED"
}

trap 'cleanup' EXIT
trap 'cleanup; exit 130' INT
trap 'cleanup; exit 143' TERM
trap 'cleanup; exit 129' HUP

fail() {
    if [ -f "$LOG" ]; then
        cp "$LOG" "$DISK.fail.log" 2>/dev/null || true
    fi
    cleanup
    echo "smp-test: FAIL: $1" >&2
    exit 1
}

# Boot the test image with $1 processors, the boot line $2 added, and run
# smpmusl expecting $3 of them.
probe() {
    plural=s
    [ "$3" = 1 ] && plural=
    "$HARNESS" --kernel "$KERNEL" --initrd "$IMAGE" \
        --qemu-arg=-cpu --qemu-arg=max --qemu-arg=-smp --qemu-arg="$1" \
        --append "rach2.rescue $2" --timeout "$(scaled 240)" --steptmo "$(scaled 200)" \
        --ok "SMPMUSL OK ($3 processor$plural)" --never "SMPMUSL FAIL" \
        --log "$DISK.probe-$1-$3.log" \
        "expect=Type 'help'" "send=exec /musl/smpmusl $3" "expect=SMPMUSL" ||
        fail "smpmusl with $1 processors and '$2' on the boot line expected $3; see $DISK.probe-$1-$3.log"
    echo "smp-test: ok: $3 of $1 processors found and stressed"
}

probe 1 "" 1
probe 4 "" 4
probe 4 "maxcpus=2" 2

# The debug shell's preemption test with two processors: two kernel threads
# that print by turns, each begun by its first switch holding the big lock
# once, so that the tick that preempts it can take the lock again.
"$HARNESS" --kernel "$KERNEL" --initrd "$IMAGE" \
    --qemu-arg=-cpu --qemu-arg=max --qemu-arg=-smp --qemu-arg=2 \
    --append "rach2.rescue" --timeout "$(scaled 120)" --steptmo "$(scaled 90)" \
    --ok "Preemptive scheduling test passed!" --never "test FAILED" \
    --log "$DISK.preempt.log" \
    "expect=Type 'help'" "send=preempt" ||
    fail "the debug shell's preemption test did not pass with two processors; see $DISK.preempt.log"
echo "smp-test: ok: the debug shell's two threads were preempted by turns"

# The installed system with four processors, at its console.
rm -f "$FEED"
mkfifo "$FEED"
"$QEMU" -cpu max -smp 4 -m 512 -display none -kernel "$KERNEL" \
    -append "rach2.fsdisk" -rtc base=utc \
    -drive "file=$DISK,format=raw,if=ide,index=0,media=disk" \
    -serial stdio < "$FEED" > "$LOG" 2>&1 &
QPID=$!
DRIVE_LOG=$LOG
DRIVE_FIFO=$FEED
drive_feed "$QPID"
drive_login test "$TESTPASSWORD" "$(scaled 90)" || fail "the machine did not log in at its console"

drive_type 'nproc | grep -qx 4 && echo NPROC_""OK'
drive_expect 'NPROC_OK' "$(scaled 30)" || fail "nproc does not count four processors"

# The ish sources four times over, every compile in the background at once,
# between two readings of /proc/stat; awk then requires each cpuN line's
# user column to have grown.
# shellcheck disable=SC2016 # the guest's shell expands these, not this one
drive_type 'cd && rm -rf smpbuild && mkdir smpbuild && cd smpbuild && cat /proc/stat > before && for i in 1 2 3 4; do for f in ish ish_cstring_utilities ish_shell_utilities ish_syscalls; do /bin/cc -c -o $i-$f.o /src/ish/$f.c & done; done; wait; cat /proc/stat > after; ls *.o | wc -l | grep -qx 16 && echo BUILD_""DONE'
drive_expect 'BUILD_DONE' "$(scaled 600)" || fail "the parallel build did not make its sixteen objects"
drive_type "awk '/^cpu[0-9]/ { if (FILENAME == \"before\") u[\$1] = \$2; else if (\$2 > u[\$1]) grew++; else stalled++ } END { exit !(grew == 4 && !stalled) }' before after && echo USER_TIME_""GREW"
drive_expect 'USER_TIME_GREW' "$(scaled 30)" || fail "a processor's user ticks did not grow during the parallel build"
echo "smp-test: ok: a parallel build ran on every one of four processors"

drive_type 'exit'
cleanup
echo "smp-test: PASS"
