#!/usr/bin/env sh

#
# bootline.sh: the boot line that contradicts itself, and the other boots
# that must halt rather than open the debug shell.
#
# `rach2.nodisk` skips the disk probe. On an image whose root is the disk that
# leaves nothing to mount, and the kernel would fall to the debug shell, which
# authenticates nobody. The kernel refuses that combination and says why; the
# same combination with `rach2.rescue` is allowed, because asking for the debug
# shell is what that token is for. A root that mounted and whose /sbin/init
# cannot be read is refused the same way; tests/system/init_refusal.sh is that
# row, run here on the kernel under test.
#
# Each refusal is asserted twice: the diagnostic is there, and the machine
# never reached a shell. A test that looked only for the message would pass on
# a kernel that printed it and carried on.
#
# Usage: bootline.sh <kernel> <disk> <harness> <mkmbr>
#

# The emulator of the architecture tests/system/suites.mk names.
QEMU=${RACH2_QEMU:-qemu-system-i386}

# shellcheck source=tests/system/deadline.sh
# The runner helper, whose `until` and `gone` are the suite's waits.
RUNUTIL="$(cd "$(dirname "$0")/../../.." && pwd)/build/tests/system/runutil"
. "$(cd "$(dirname "$0")/../../.." && pwd)/tests/system/deadline.sh"
set -e

KERNEL="$1"
DISK="$2"
HARNESS="$3"
MKMBR="$4"

if [ -z "$KERNEL" ] || [ -z "$DISK" ] || [ -z "$HARNESS" ] || [ -z "$MKMBR" ]; then
    echo "usage: bootline_test.sh <kernel> <disk> <harness> <mkmbr>" >&2
    exit 2
fi

WORK="$DISK.bootline"
LOG="$WORK.log"
QPID=""

# Stop the machine this run started, if it runs. The pid is taken and cleared
# before it is signalled, so a trap that fires between the kill and the wait
# finds nothing to kill: a pid this script no longer owns may since have been
# given to another process. SIGTERM goes first, then SIGKILL if the machine is
# still there five seconds later, so the wait is bounded.
stop_boot() {
    qpid="$QPID"
    QPID=""
    if [ -n "$qpid" ]; then
        kill "$qpid" 2>/dev/null || true
        "$RUNUTIL" gone "$qpid" 5 || kill -9 "$qpid" 2>/dev/null || true
        wait "$qpid" 2>/dev/null || true
    fi
}

# Stop the machine and remove the disk's copy and the log.
cleanup() {
    stop_boot
    rm -f "$WORK" "$LOG"
}
trap cleanup EXIT INT TERM HUP

# fail REASON: print REASON and the console's last twenty lines, and exit 1.
fail() {
    echo "bootline-test: FAIL: $1" >&2
    if [ -f "$LOG" ]; then
        echo "--- the boot's console ---" >&2
        tail -20 "$LOG" >&2
    fi
    exit 1
}

# boot TOKENS LINE SECONDS: boot a copy of the disk with the boot line TOKENS,
# wait up to 25 seconds for LINE, then leave the machine running SECONDS more
# and stop it. A kernel that printed a refusal and carried on would reach a
# shell in that time, which a test that stopped the machine at the line could
# not see.
boot() {
    rm -f "$LOG" "$WORK"
    cp "$DISK" "$WORK"
    "$QEMU" -cpu max -display none -kernel "$KERNEL" \
        -append "$1" \
        -drive "file=$WORK,format=raw,if=ide,index=0,media=disk" \
        -serial stdio < /dev/null > "$LOG" 2>&1 &
    QPID=$!
    "$RUNUTIL" until "$LOG" "$2" 25 || true
    # interval: the machine runs on after the line to show it stopped there
    sleep "$3"
    stop_boot
}

# The two spellings of a disk root, each of which must be refused.
for tokens in "rach2.nodisk rach2.fsdisk" "rach2.nodisk rach2.root=disk"; do
    # The last of the refusal's four lines; under load they come seconds apart.
    boot "$tokens" "and no rach2.rescue" 10
    grep -q "boot: refused: rach2.nodisk with rach2.fsdisk or rach2.root=disk" "$LOG" ||
        fail "$tokens was not refused, or the refusal did not name both tokens"
    # Ten seconds on, neither the debug shell's banner nor a login prompt.
    if grep -qE "Kernel debug shell active|rach2 login" "$LOG"; then
        fail "$tokens was refused and then reached a shell anyway"
    fi
    echo "bootline-test: refused, and stopped: $tokens"
done

# The wider case the same fall-through opens: a disk root on a machine whose
# disk holds no file system. Nothing mounts, /sbin/init cannot be reached,
# and the shell that would open authenticates nobody, so this halts too. The
# disk is 48 MiB of zeros, a disk that answers and has nothing on it.
rm -f "$WORK.blank"
dd if=/dev/zero of="$WORK.blank" bs=1048576 count=48 2>/dev/null
# boot_blank TOKENS: boot the blank disk with the boot line TOKENS, wait up
# to 30 seconds for the refusal, leave the machine running ten seconds more
# and stop it.
boot_blank() {
    rm -f "$LOG"
    "$QEMU" -cpu max -display none -kernel "$KERNEL" \
        -append "$1" \
        -drive "file=$WORK.blank,format=raw,if=ide,index=0,media=disk" \
        -serial stdio < /dev/null > "$LOG" 2>&1 &
    QPID=$!
    "$RUNUTIL" until "$LOG" "boot: refused: no file system mounted" 30 || true
    # interval: the machine runs on after the line to show it stopped there
    sleep 10
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

# And a root that mounted and holds an init the kernel cannot read: an ext2
# root whose /sbin/init is a symbolic link to itself, so the lookup answers
# ELOOP and not the ENOENT of an image with no init. The row asserts the
# refusal and that no shell followed it.
if ! sh "$(dirname "$0")/../init_refusal.sh" "$KERNEL" "$HARNESS" "$MKMBR" "$WORK.init"; then
    echo "bootline-test: FAIL: a root whose /sbin/init cannot be read was not refused" >&2
    exit 1
fi
echo "bootline-test: refused, and stopped: a root whose /sbin/init cannot be read"

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
