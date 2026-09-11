#!/usr/bin/env sh

#
# bootline_test.sh: the boot line that contradicts itself, in four boots.
#
# `rach2.nodisk` skips the disk probe. On an image whose root is the disk that
# leaves nothing to mount, and the kernel would fall to the debug shell, which
# authenticates nobody. The kernel refuses that combination and says why; the
# same combination with `rach2.rescue` is allowed, because asking for the debug
# shell is what that token is for.
#
# Each refusal is asserted twice: the diagnostic is there, AND the machine
# never reached a shell. A test that looked only for the message would pass on
# a kernel that printed it and carried on.
#
# Usage: bootline_test.sh <kernel> <disk>
#

# shellcheck source=tests/system/deadline.sh
. "$(cd "$(dirname "$0")/../../.." && pwd)/tests/system/deadline.sh"
set -e

KERNEL="$1"
DISK="$2"

if [ -z "$KERNEL" ] || [ -z "$DISK" ]; then
    echo "usage: bootline_test.sh <kernel> <disk>" >&2
    exit 2
fi

WORK="$DISK.bootline"
LOG="$WORK.log"
QPID=""

# Everything this run started, ended. The pid is taken and forgotten before it
# is signalled, so a trap that fires between the kill and the wait finds
# nothing left to kill: a pid this script no longer owns is one the host may
# since have given to somebody else. SIGTERM first, then SIGKILL if the machine
# is still there, so a wait here is bounded.
stop_boot() {
    qpid="$QPID"
    QPID=""
    if [ -n "$qpid" ]; then
        kill "$qpid" 2>/dev/null || true
        held=0
        while [ "$held" -lt 5 ] && kill -0 "$qpid" 2>/dev/null; do
            sleep 1
            held=$((held + 1))
        done
        kill -9 "$qpid" 2>/dev/null || true
        wait "$qpid" 2>/dev/null || true
    fi
}

cleanup() {
    stop_boot
    rm -f "$WORK" "$LOG"
}
trap cleanup EXIT INT TERM HUP

fail() {
    echo "bootline-test: FAIL: $1" >&2
    if [ -f "$LOG" ]; then
        echo "--- the boot's console ---" >&2
        tail -20 "$LOG" >&2
    fi
    exit 1
}

# One boot, with the tokens under test, for a bounded time. $3 is how long to
# leave the machine running AFTER the line arrives: a kernel that printed a
# refusal and carried on would reach a shell in that time, and a test that
# killed the machine the moment it saw the line could not tell the two apart.
boot() {
    rm -f "$LOG" "$WORK"
    cp "$DISK" "$WORK"
    qemu-system-i386 -cpu max -display none -kernel "$KERNEL" \
        -append "$1" \
        -drive "file=$WORK,format=raw,if=ide,index=0,media=disk" \
        -serial stdio < /dev/null > "$LOG" 2>&1 &
    QPID=$!
    held=0
    while [ "$held" -lt "$(scaled 25)" ]; do
        if grep -q "$2" "$LOG" 2>/dev/null; then
            break
        fi
        sleep 1
        held=$((held + 1))
    done
    held=0
    while [ "$held" -lt "$3" ]; do
        sleep 1
        held=$((held + 1))
    done
    stop_boot
}

# The two spellings of a disk root, each of which must be refused.
for tokens in "rach2.nodisk rach2.fsdisk" "rach2.nodisk rach2.root=disk"; do
    boot "$tokens" "boot: refused" 10
    grep -q "boot: refused: rach2.nodisk with rach2.fsdisk or rach2.root=disk" "$LOG" ||
        fail "$tokens was not refused, or the refusal did not name both tokens"
    # And the machine stopped there, ten seconds later. The debug shell
    # announces itself, and a login prompt is the other way a boot ends up
    # somewhere a person can type.
    if grep -qE "Kernel debug shell active|rach2 login" "$LOG"; then
        fail "$tokens was refused and then reached a shell anyway"
    fi
    echo "bootline-test: refused, and stopped: $tokens"
done

# And the wider case the same fall-through opens: a disk root on a machine
# whose disk holds no file system. Nothing mounts, /sbin/init cannot be
# reached, and the shell that would open authenticates nobody, so this halts
# too. The image here is the one under test with its first megabyte zeroed,
# which is a disk that answers and has nothing on it.
rm -f "$WORK.blank"
dd if=/dev/zero of="$WORK.blank" bs=1048576 count=48 2>/dev/null
boot_blank() {
    rm -f "$LOG"
    qemu-system-i386 -cpu max -display none -kernel "$KERNEL" \
        -append "$1" \
        -drive "file=$WORK.blank,format=raw,if=ide,index=0,media=disk" \
        -serial stdio < /dev/null > "$LOG" 2>&1 &
    QPID=$!
    held=0
    while [ "$held" -lt "$(scaled 30)" ]; do
        if grep -q "boot: refused: no file system mounted" "$LOG" 2>/dev/null; then
            break
        fi
        sleep 1
        held=$((held + 1))
    done
    held=0
    while [ "$held" -lt 10 ]; do
        sleep 1
        held=$((held + 1))
    done
    stop_boot
}
boot_blank "rach2.fsdisk"
grep -q "boot: refused: no file system mounted" "$LOG" ||
    fail "a disk root with nothing on the disk was not refused"
if grep -qE "Kernel debug shell active|rach2 login" "$LOG"; then
    fail "a disk root with nothing on the disk reached a shell anyway"
fi
rm -f "$WORK.blank"
echo "bootline-test: refused, and stopped: a disk root with nothing to mount"

# And the same combination with rescue asked for, which must reach the shell:
# that is what the token is for, and the refusal must not stand in its way.
for tokens in "rach2.nodisk rach2.fsdisk rach2.rescue" \
              "rach2.nodisk rach2.root=disk init=rescue"; do
    boot "$tokens" "Kernel debug shell active" 5
    grep -q "Kernel debug shell active" "$LOG" ||
        fail "$tokens did not reach the debug shell it asked for"
    grep -q "boot: refused: rach2.nodisk" "$LOG" &&
        fail "$tokens was refused, though rescue is the exemption"
    echo "bootline-test: reached the shell it asked for: $tokens"
done

echo "bootline-test: PASS"
