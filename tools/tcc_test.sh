#!/usr/bin/env sh

#
# tcc_test.sh: prove the machine can compile a C program on itself, and that it
# refuses the kinds of program it cannot run.
#
# It boots os-real from a disk and works as the unprivileged `test` account, not
# as root. That is the difference between a compiler on the machine and a
# compiler for the administrator: the sysroot has to be readable by an ordinary
# user, and the program has to be written somewhere an ordinary user may write.
#
# The disk rather than the RAM image, because the RAM image is a 16 MB module
# inside a 32 MB machine and the compiler wants room.
#
# Three traps this repo has already paid for, and which every phase here obeys:
# the console echoes what is typed, so a token that appears in a command would
# be matched from its own echo and each is written split; a feed written all at
# once is eaten by anything that flushes its input, so every feed is spaced; and
# a previous run's logs hold every token this run waits for, so they go before
# the boot rather than only in the trap.
#
# Usage: tcc_test.sh <kernel> <disk> <root password> <test password>
#

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
trap 'rm -f "$WORK" "$LOG" "$FEED"' EXIT INT TERM

QPID=""
FPID=""

cleanup_boot() {
    kill -9 "$QPID" 2>/dev/null || true
    wait "$QPID" 2>/dev/null || true
    kill "$FPID" 2>/dev/null || true
    rm -f "$FEED"
}

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
    i=0
    while [ "$i" -lt "$2" ]; do
        if grep -q "$1" "$LOG" 2>/dev/null; then
            return 0
        fi
        sleep 1
        i=$((i + 1))
    done
    return 1
}

boot_lines() {
    rm -f "$LOG" "$FEED"
    mkfifo "$FEED"
    (
        sleep 12
        for line in "$@"; do
            printf '%s\n' "$line"
            sleep 6
        done
        sleep 300
    ) > "$FEED" &
    FPID=$!
    qemu-system-i386 -cpu max -display none -kernel "$KERNEL" \
        -append "rach2.fsdisk rach2.trace=enosys" \
        -drive "file=$WORK,format=raw,if=ide,index=0,media=disk" \
        -serial stdio < "$FEED" > "$LOG" 2>&1 &
    QPID=$!
}

rm -f "$WORK"
cp "$DISK" "$WORK"

# The program is written with printf rather than a here-document, because the
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
    "printf 'a line this machine compiled\\n' > /tmp/in.txt" \
    "/tmp/cat /tmp/in.txt > /tmp/mine.out; busybox cat /tmp/in.txt > /tmp/theirs.out" \
    "cmp /tmp/mine.out /tmp/theirs.out && echo SAME\"\"BYTES" \
    "sync; echo DONE\"\"IT"

if ! wait_token "BUILTIT" 240; then
    fail "the compiler did not build a program on the machine"
fi
if ! wait_token "COMPILEDHERE" 90; then
    fail "the program the machine compiled did not run, or printed the wrong thing"
fi

# What it must refuse. Each is asserted twice: the status it reported, and the
# reason it gave, because a compiler that refused for the wrong reason has told
# the student something false about the machine.
if ! wait_token "RUNRC=1" 120; then
    fail "-run was not refused"
fi
if ! grep -q "mprotect" "$LOG"; then
    fail "-run was refused without saying why"
fi
if ! wait_token "SHRC=1" 90; then
    fail "-shared was not refused"
fi
if ! grep -q "shared is not available" "$LOG"; then
    fail "-shared was refused without saying why"
fi
if ! wait_token "DYRC=1" 90; then
    fail "a link without -static was not refused"
fi
if ! grep -q "must be linked with -static" "$LOG"; then
    fail "a dynamic link was refused without saying why"
fi
if ! wait_token "RWRC=1" 90; then
    fail "a raw-image link was not refused"
fi
if ! grep -q "must be an ELF" "$LOG"; then
    fail "a raw-image link was refused without saying why"
fi

# A refusal that still wrote its output would be a refusal in name only. The
# guest answers this one itself: the console ends every line with a carriage
# return, so a pattern anchored here would have matched nothing and passed
# whatever was there.
if ! wait_token "NOLEFTOVERS" 90; then
    fail "a refused compile left its output behind, or the check never ran"
fi

# Compiling to an object is not refused: it produces no program to run, and it
# is how a multi-step build works.
if ! wait_token "OBJECTOK" 120; then
    fail "compiling to an object was refused, and it must not be"
fi

# A missing syscall says nothing on its own: the dispatcher answers -ENOSYS in a
# register and prints nothing. The kernel's trace, on for this boot, turns that
# into a line, once per number.
#
# The question is what the COMPILER asked for, and not what anything on the
# machine asked for: busybox's `login` wants setitimer (104), which this kernel
# does not have and does not need. The trace names the process, and names each
# number once PER PROCESS, so the question is asked by name. An earlier version
# subtracted the numbers seen before the compile from the numbers seen after,
# which reads well and is wrong: a number `login` had already reported would
# have been suppressed for the compiler and the subtraction would come back
# empty.
if grep "not implemented" "$LOG" | grep -q "asked for by tcc"; then
    fail "the compiler asked this kernel for a syscall it does not have: $(grep -m1 'asked for by tcc' "$LOG")"
fi

# And a program of this project's own, compiled from the source the image
# carries. The proof is behavioural rather than a byte comparison of the two
# binaries: the shipped one was linked against libgcc on the host and this one
# against libtcc1.a on the machine, so they are different objects that must do
# the same thing.
#
# The reference is `busybox cat` and not `/bin/cat`, because in THIS image there
# is no such file: busybox dispatches `cat` as an applet inside itself, and the
# in-house cat lives in the other image. So what is compared is the program the
# machine just built against the program this machine actually uses.
if ! wait_token "TOOLBUILT" 240; then
    fail "the machine could not compile this project's own cat"
fi
if ! wait_token "SAMEBYTES" 120; then
    fail "the cat the machine compiled does not agree with the shipped one"
fi

if ! wait_token "DONEIT" 90; then
    fail "the boot never finished its script"
fi
cleanup_boot

echo "tcc-test: the machine compiled a program and one of its own tools, and refused what it cannot run: OK"
echo "tcc-test: PASS"
