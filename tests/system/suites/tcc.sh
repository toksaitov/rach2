#!/usr/bin/env sh

#
# tcc.sh: the machine compiles a C program on itself, and refuses the kinds of
# program it cannot run.
#
# The suite boots os-real from a disk and works as the unprivileged `test`
# account, not as root, since a compiler on the machine must work for an
# ordinary user: the sysroot must be readable by one, and the program written
# where one may write. The disk is used and not the RAM image, which would
# take memory the compiler needs.
#
# Every phase follows three rules. The console echoes what is typed, so a
# token that appears in a command would be matched from its own echo, and
# each is typed split. A feed typed all at once is lost to anything that
# flushes its input, so each line waits for the prompt. A previous run's logs
# have every token this run waits for, so they are removed before the boot
# and not only in the trap.
#
# A line typed at this console is edited in a buffer of `N_TTY_LINE_MAX`
# bytes (256, in rach2/sys/tty/n_tty.h), and what does not fit is dropped,
# not wrapped. The longest line here is about 180 characters; a command
# longer than the buffer must be split, or it fails in a way that looks like
# the compiler's fault.
#
# Usage: tcc.sh <kernel> <disk> <root password> <test password>
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
PASSWORD="$3"
TESTPASSWORD="$4"

if [ -z "$KERNEL" ] || [ -z "$DISK" ] || [ -z "$PASSWORD" ] || [ -z "$TESTPASSWORD" ]; then
    echo "usage: tcc_test.sh <kernel> <disk> <root password> <test password>" >&2
    exit 2
fi

WORK="$DISK.work"
LOG="$DISK.work.log"
FEED="$DISK.work.feed"
rm -f "$WORK" "$LOG" "$FEED"

QPID=""
FPID=""

# Stop the machine and the feeder, and remove the feed. It is safe to call
# twice, since the ordinary paths call it and the trap calls it again: a
# script that stopped its machine only when its own checks ended would leave
# QEMU running for a cancelled build, with the disk copy whose name has been
# removed.
#
# Each pid is taken and cleared before it is signalled, so a trap that fires
# between the kill and the wait finds nothing to kill: a pid this script no
# longer owns may since have been given to another process.
cleanup_boot() {
    qpid="$QPID"
    fpid="$FPID"
    QPID=""
    FPID=""
    if [ -n "$qpid" ]; then
        kill -9 "$qpid" 2>/dev/null || true
        wait "$qpid" 2>/dev/null || true
    fi
    if [ -n "$fpid" ]; then
        kill -9 "$fpid" 2>/dev/null || true
        wait "$fpid" 2>/dev/null || true
    fi
    rm -f "$FEED"
}

# Stop the boot, and remove the disk's copy, the log and the feed.
cleanup() {
    cleanup_boot
    rm -f "$WORK" "$LOG" "$FEED"
}

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
    echo "tcc-test: FAIL: $1" >&2
    echo "tcc-test: the failing boot's console log: $DISK.fail.log" >&2
    exit 1
}

# Wait up to $2 seconds for the token $1 in the log.
wait_token() {
    "$RUNUTIL" until "$LOG" "$1" "$2"
}

REPO=$(cd "$(dirname "$0")/../../.." && pwd)
# shellcheck source=tests/system/drive.sh
. "$REPO/tests/system/drive.sh"
# boot_lines USER PASSWORD LINE...: boot the disk's copy, log in as USER at the
# console, and type each LINE once the shell's prompt has come back, through
# tests/system/drive.sh. The machine is started before the feeder, which lets
# the feeder know the machine's pid: opening a fifo for reading waits for a
# writer, so QEMU waits in that open until the feeder starts, and neither
# misses a line. The feeder holds the fifo open only while the machine runs.
boot_lines() {
    rm -f "$LOG" "$FEED"
    mkfifo "$FEED"
    "$QEMU" -cpu max -display none -kernel "$KERNEL" \
        -append "rach2.fsdisk rach2.trace=enosys" \
        -drive "file=$WORK,format=raw,if=ide,index=0,media=disk" \
        -serial stdio < "$FEED" > "$LOG" 2>&1 &
    QPID=$!
    DRIVE_LOG=$LOG
    DRIVE_FIFO=$FEED
    drive_feed "$QPID"
    FPID=$DRIVE_FPID
    drive_login "$1" "$2" "$(scaled 90)" || fail "the machine did not log in at its console"
    shift 2
    for line in "$@"; do
        drive_type "$line"
        drive_prompt "$(scaled 120)" || true
    done
}

rm -f "$WORK"
cp "$DISK" "$WORK"

# The program is written with printf, not a here-document, because the
# console feed types one line at a time and a here-document would need the shell
# to stay in a continuation across the pause between them.
boot_lines \
    "test" \
    "$TESTPASSWORD" \
    "printf '#include <stdio.h>\\nint main(void){printf(\"COMPILED\"\"HERE\\\\n\");return 0;}\\n' > /tmp/hello.c" \
    "cat /tmp/hello.c" \
    "tcc -static -o /tmp/hello /tmp/hello.c && echo BUILT\"\"IT" \
    "/tmp/hello" \
    "tcc -run /tmp/hello.c > /tmp/run.out 2>&1; echo RUN\"\"RC=\$?; cat /tmp/run.out" \
    "tcc -shared -o /tmp/lib.so /tmp/hello.c > /tmp/sh.out 2>&1; echo SH\"\"RC=\$?; cat /tmp/sh.out" \
    "tcc -o /tmp/dyn /tmp/hello.c > /tmp/dy.out 2>&1; echo DY\"\"RC=\$?; cat /tmp/dy.out" \
    "tcc -static -Wl,-oformat=binary -o /tmp/raw /tmp/hello.c > /tmp/rw.out 2>&1; echo RW\"\"RC=\$?; cat /tmp/rw.out" \
    "[ ! -e /tmp/lib.so ] && [ ! -e /tmp/dyn ] && [ ! -e /tmp/raw ] && echo NO\"\"LEFTOVERS" \
    "tcc -c -o /tmp/hello.o /tmp/hello.c && echo OBJECT\"\"OK" \
    "tcc -static -o /tmp/cat /src/utils/cat.c && echo TOOL\"\"BUILT" \
    "busybox dd if=/bin/tcc of=/tmp/in.bin bs=1024 count=21 2>/dev/null && echo INPUT\"\"MADE" \
    "/tmp/cat /tmp/in.bin > /tmp/mine.out && busybox cat /tmp/in.bin > /tmp/theirs.out && cmp /tmp/mine.out /tmp/theirs.out && echo SAME\"\"BYTES" \
    "/tmp/cat /tmp/in.bin /tmp/hello.c > /tmp/mine2.out && busybox cat /tmp/in.bin /tmp/hello.c > /tmp/theirs2.out && cmp /tmp/mine2.out /tmp/theirs2.out && echo BOTH\"\"FILES" \
    "tcc -static -o /tmp/ping /src/utils/ping.c /src/utils/resolv.c && echo TWO\"\"UNITS" \
    "sync; echo DONE\"\"IT"

if ! wait_token "BUILTIT" 240; then
    fail "the compiler did not build a program on the machine"
fi
if ! wait_token "COMPILEDHERE" 90; then
    fail "the program the machine compiled did not run, or printed the wrong thing"
fi

# What it must refuse. Each refusal is checked twice, the status it reported
# and the reason it gave, since a compiler that refused for the wrong reason
# has told the student something false about the machine.
#
# Both checks wait. The status and the reason come from two commands (the
# shell echoes `$?`, then `cat` prints the file the compiler wrote), so a grep
# for the reason the moment the status appears would race the second command.
if ! wait_token "RUNRC=1" 120; then
    fail "-run was not refused"
fi
if ! wait_token "cannot reach the C library" 30; then
    fail "-run was refused without saying why"
fi
if ! wait_token "SHRC=1" 90; then
    fail "-shared was not refused"
fi
if ! wait_token "shared is not available" 30; then
    fail "-shared was refused without saying why"
fi
if ! wait_token "DYRC=1" 90; then
    fail "a link without -static was not refused"
fi
if ! wait_token "must be linked with -static" 30; then
    fail "a dynamic link was refused without saying why"
fi
if ! wait_token "RWRC=1" 90; then
    fail "a raw-image link was not refused"
fi
if ! wait_token "must be an ELF" 30; then
    fail "a raw-image link was refused without saying why"
fi

# A refused compile must leave no output. The guest makes this check itself,
# since the console ends every line with a carriage return and a pattern
# anchored here would match nothing.
if ! wait_token "NOLEFTOVERS" 90; then
    fail "a refused compile left its output behind, or the check never ran"
fi

# Compiling to an object is not refused: it produces no program to run, and it
# is how a multi-step build works.
if ! wait_token "OBJECTOK" 120; then
    fail "compiling to an object was refused, and it must not be"
fi

# A program of this project's own, compiled from the source the image has.
# The check is of behaviour, not a byte comparison of the two binaries: the
# shipped one was linked against libgcc on the host and this one against
# libtcc1.a on the machine, so they are different objects that must do the
# same thing.
#
# The reference is `busybox cat` and not `/bin/cat`, since this image has no
# such file: busybox runs `cat` as an applet inside itself, and the in-house
# cat is in the other image. What is compared is the program the machine
# built with the program this machine uses.
#
# The input is 21 KB of a binary file, not a line of text, and the second
# comparison passes two files. A line of text is one 4096-byte read and one
# write: a cat that returned after its first read would copy it and truncate
# anything larger, and a cat that stopped after its first argument would go
# unnoticed. The commands are chained with `&&`, so a cat that failed cannot
# reach the token by leaving an old output in place.
if ! wait_token "TOOLBUILT" 240; then
    fail "the machine could not compile this project's own cat"
fi
if ! wait_token "INPUTMADE" 120; then
    fail "the multi-block input for the comparison was not made"
fi
if ! wait_token "SAMEBYTES" 120; then
    fail "the cat the machine compiled does not agree with the shipped one"
fi
if ! wait_token "BOTHFILES" 120; then
    fail "the cat the machine compiled does not agree over two files"
fi

# Two translation units in one command, which is how the host builds this same
# program: `ping` is the one in-house program with a companion source, and its
# `#include "netdb.h"` resolves beside it because the whole directory is staged.
# It is built and not run: this boot has no network, and what the compiler is
# being asked for here is the link across two objects.
if ! wait_token "TWOUNITS" 240; then
    fail "the machine could not compile a program from two sources at once"
fi

if ! wait_token "DONEIT" 90; then
    fail "the boot never finished its script"
fi

# A missing system call says nothing on its own: the dispatcher answers
# -ENOSYS in a register and prints nothing. The kernel's trace, on for this
# boot, prints a line for each number, once for each process.
#
# The question is what the compiler asked for, not what any program on the
# machine asked for, since another program may ask for a number this kernel
# does not have and does not need. The trace names the process, so the
# question is asked by name; a difference of the numbers seen before and
# after the compile would drop a number that `login` had already reported.
#
# It is asked last, after the script's own end token, since the log is read
# once and must be read when every compile has printed what it will; asked
# in the middle, it would miss what the compile of /src/utils/cat.c asks for.
if grep "not implemented" "$LOG" | grep -q "asked for by tcc"; then
    fail "the compiler asked this kernel for a syscall it does not have: $(grep -m1 'asked for by tcc' "$LOG")"
fi

cleanup_boot

echo "tcc-test: the machine compiled a program and one of its own tools, and refused what it cannot run: OK"
echo "tcc-test: PASS"
