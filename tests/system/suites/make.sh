#!/usr/bin/env sh

#
# make_test.sh: prove that a build on this machine is a build, and not a
# from-scratch rebuild wearing a build's clothes.
#
# The compiler arrived in milestone 20 and `tests/run tcc` proves it compiles.
# What this proves is what a make is for: that a second run does nothing, that
# touching one thing rebuilds exactly what depends on it, that the program which
# comes out is the new one, and that all of it survives a reboot. Every one of
# those rests on the file system reporting times a build tool can order, which
# is what the first half of milestone 21 built.
#
# It runs as the unprivileged `test` account, like `tcc-test`: a build tool only
# root can use is a different thing from a build tool.
#
# Two design decisions are worth stating, because both were arrived at the hard
# way.
#
# Every assertion is made IN THE GUEST, over that command's own output file, and
# reported as one token this script waits for. Counting occurrences in the
# console log instead counts every earlier command's output too, and a check
# that reads the whole log can be satisfied by a LATER step's success.
#
# Every line is sent only after the previous one has finished. The console's
# line discipline has a bounded amount of unread input, and a build takes
# seconds to minutes, so a feed on a fixed timer silently loses commands on a
# slow host. Each command ends by printing a marker, and the feeder waits for
# it.
#
# Two mistakes this repo has already paid for, avoided by every line here: the
# console echoes what is typed, so a token that appears in a command would be
# matched from its own echo and each is written split; and a previous run's logs
# hold every token this run waits for, so they go before the boot.
#
# Usage: make_test.sh <kernel> <disk> <test password>
#

set -e

KERNEL="$1"
DISK="$2"
TESTPASSWORD="$3"

if [ -z "$KERNEL" ] || [ -z "$DISK" ] || [ -z "$TESTPASSWORD" ]; then
    echo "usage: make_test.sh <kernel> <disk> <test password>" >&2
    exit 2
fi

WORK="$DISK.work"
LOG="$DISK.work.log"
FEED="$DISK.work.feed"
STEPS="$DISK.work.steps"
rm -f "$WORK" "$LOG" "$FEED" "$STEPS"

QPID=""
FPID=""

# Everything this run started, ended, on every way out and not only the two the
# script writes. Each pid is taken and forgotten BEFORE it is signalled, so a
# trap that fires between the kill and the wait finds nothing left to kill.
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

cleanup() {
    cleanup_boot
    rm -f "$WORK" "$LOG" "$FEED" "$STEPS"
}

trap 'cleanup' EXIT
trap 'cleanup; exit 130' INT
trap 'cleanup; exit 143' TERM
trap 'cleanup; exit 129' HUP

fail() {
    if [ -f "$LOG" ]; then
        cp "$LOG" "$DISK.fail.log" 2>/dev/null || true
    fi
    cleanup_boot
    echo "make-test: FAIL: $1" >&2
    echo "make-test: the failing boot's console log: $DISK.fail.log" >&2
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

# Queue one command and the marker that says it finished. The marker is what the
# feeder waits for before typing the next line.
step() {
    printf '%s\t%s\n' "$1" "$2" >> "$STEPS"
}

# The machine is started first and the feed second, which is the order that lets
# the feed know the machine's pid: opening a fifo for reading waits for a
# writer, so QEMU sits in that open until the feed arrives. The feed then holds
# the console open only while the machine is up.
# The console is driven through tests/system/drive.sh: a line is typed when
# the output it answers has arrived, under a bound.
REPO=$(cd "$(dirname "$0")/../../.." && pwd)
# shellcheck source=tests/system/drive.sh
. "$REPO/tests/system/drive.sh"
boot_steps() {
    rm -f "$FEED"
    mkfifo "$FEED"
    qemu-system-i386 -cpu max -display none -kernel "$KERNEL" \
        -append "rach2.fsdisk" -rtc base=utc \
        -drive "file=$WORK,format=raw,if=ide,index=0,media=disk" \
        -serial stdio < "$FEED" > "$LOG" 2>&1 &
    QPID=$!
    DRIVE_LOG=$LOG
    DRIVE_FIFO=$FEED
    drive_feed "$QPID"
    FPID=$DRIVE_FPID
    drive_login test "$TESTPASSWORD" 90 || fail "the machine did not log in at its console"
    # Each step is typed once the one before it has printed its marker; a
    # full build sits behind a single marker, so a step's bound is long,
    # and the run has a budget the steps share, so a hung build fails the
    # run when the budget is gone and not an hour a marker later.
    boot_start=$(date +%s)
    while IFS='	' read -r marker command; do
        remaining=$(( boot_start + $(scaled 600) - $(date +%s) ))
        [ "$remaining" -gt 0 ] || fail "the run's budget was spent before $marker"
        drive_type "$command"
        drive_expect_raw "$marker" "$remaining" || fail "the step for $marker never printed it"
    done < "$STEPS"
}

rm -f "$WORK"
cp "$DISK" "$WORK"

# The project: two sources and a header they share, so touching the header must
# rebuild both objects and touching one source must rebuild exactly one. The
# makefile names a compiler that does not exist and every `make` here overrides
# it on the command line: a make that ignored the override would fail outright,
# where an override naming the same compiler as the built-in default would prove
# nothing at all.
: > "$STEPS"
step "SETUPDONE" "cd /tmp && rm -rf demo && mkdir demo && cd demo && echo SETUP\"\"DONE"
step "WROTEH" "printf 'int scale(int);\\n' > demo.h; echo WROTE\"\"H"
step "WROTELIB" "printf '#include \"demo.h\"\\nint scale(int x){return 2*x;}\\n' > lib.c; echo WROTE\"\"LIB"
step "WROTEMAIN" "printf '#include <stdio.h>\\n#include \"demo.h\"\\nint main(void){printf(\"ANSWER %%d\\\\n\", scale(21));return 0;}\\n' > main.c; echo WROTE\"\"MAIN"
step "WROTEMK" "printf 'CC=/no/such/compiler\\nprog: main.o lib.o\\n\\t\$(CC) \$(LDFLAGS) -o prog main.o lib.o\\nmain.o: main.c demo.h\\nlib.o: lib.c demo.h\\nclean:\\n\\trm -f prog main.o lib.o\\n.PHONY: clean\\n' > Makefile; echo WROTE\"\"MK"
step "FIRSTRC" "make CC=tcc > /tmp/first.out 2>&1; echo FIRST\"\"RC=\$?"
step "FIRSTRAN" "./prog > /tmp/first.run 2>&1; echo FIRST\"\"RAN=\$(cat /tmp/first.run)"
step "IDLE1" "make CC=tcc > /tmp/idle1.out 2>&1; echo IDLE1\"\"RC=\$? TCC=\$(grep -c tcc /tmp/idle1.out) UP=\$(grep -c 'up to date' /tmp/idle1.out)"
step "CHANGEDLIB" "sleep 1; printf '#include \"demo.h\"\\nint scale(int x){return 3*x;}\\n' > lib.c; echo CHANGED\"\"LIB"
step "ONERC" "make CC=tcc > /tmp/one.out 2>&1; echo ONE\"\"RC=\$? LIB=\$(grep -c 'c lib.c' /tmp/one.out) MAIN=\$(grep -c 'c main.c' /tmp/one.out) LINK=\$(grep -c 'o prog' /tmp/one.out)"
step "ONERAN" "./prog > /tmp/one.run 2>&1; echo ONE\"\"RAN=\$(cat /tmp/one.run)"
step "IDLE2" "make CC=tcc > /tmp/idle2.out 2>&1; echo IDLE2\"\"RC=\$? TCC=\$(grep -c tcc /tmp/idle2.out) UP=\$(grep -c 'up to date' /tmp/idle2.out)"
step "TOUCHEDH" "sleep 1; touch demo.h; echo TOUCHED\"\"H"
step "HDRRC" "make CC=tcc > /tmp/hdr.out 2>&1; echo HDR\"\"RC=\$? LIB=\$(grep -c 'c lib.c' /tmp/hdr.out) MAIN=\$(grep -c 'c main.c' /tmp/hdr.out)"
step "NOOVER" "sleep 1; touch demo.h; make > /tmp/over.out 2>&1; echo NO\"\"OVER=\$? SAW=\$(grep -q 'no/such/compiler' /tmp/over.out && echo yes || echo no)"
step "CLEANDONE" "> clean; make CC=tcc clean > /tmp/clean.out 2>&1; echo CLEAN\"\"DONE=\$? LEFT=\$(ls prog main.o 2>/dev/null | wc -l)"
step "UTILRC" "mkdir -p /tmp/u && cd /tmp/u && cp /src/utils/cat.c . && printf 'cat: cat.o\\n\\t\$(CC) \$(LDFLAGS) -o cat cat.o\\n' > Makefile && make CC=tcc > /tmp/u.out 2>&1; echo UTIL\"\"RC=\$?"
step "UTILSAME" "printf 'a line this machine built\\n' > /tmp/u/in.txt; /tmp/u/cat /tmp/u/in.txt > /tmp/u/mine; busybox cat /tmp/u/in.txt > /tmp/u/theirs; cmp /tmp/u/mine /tmp/u/theirs && echo UTIL\"\"SAME"
step "STAMPED" "cd /tmp/demo && make CC=tcc > /dev/null 2>&1; busybox stat -c %Y lib.o > lib.o.when; echo STAMP\"\"ED=\$(cat lib.o.when)"
step "FIRSTBOOTDONE" "sync; echo FIRSTBOOT\"\"DONE"

boot_steps

if ! wait_token "FIRSTRC=0" "$(scaled 300)"; then
    fail "make did not build the program from scratch"
fi
if ! wait_token "FIRSTRAN=ANSWER 42" "$(scaled 90)"; then
    fail "the program make built did not run, or printed the wrong thing"
fi

# A control, not evidence: this passes on a machine with no clock at all, where
# every timestamp is equal and nothing is ever out of date. It is here so that a
# failure below can be told from a make that never worked.
if ! wait_token "IDLE1RC=0 TCC=0 UP=1" "$(scaled 120)"; then
    fail "a second make with nothing changed did not report the program up to date"
fi

# One source changed, and changed BEHAVIOUR. Exactly one object recompiles, the
# program relinks once, and the program that runs is the new one: a make that
# rebuilt the object and never relinked would otherwise pass.
if ! wait_token "ONERC=0 LIB=1 MAIN=0 LINK=1" "$(scaled 180)"; then
    fail "changing one source did not recompile exactly it and relink once"
fi
if ! wait_token "ONERAN=ANSWER 63" "$(scaled 90)"; then
    fail "the program did not change after its source did"
fi

# The discriminator. An object nothing stamped stays older than its source, so
# the same rebuild runs forever; this is the step that fails without the
# stamping the file system gained in this milestone.
if ! wait_token "IDLE2RC=0 TCC=0 UP=1" "$(scaled 120)"; then
    fail "a make immediately after a rebuild ran the compiler again: nothing stamped the object it wrote"
fi

# The header both objects include: touching it must rebuild both.
if ! wait_token "HDRRC=0 LIB=1 MAIN=1" "$(scaled 180)"; then
    fail "touching the shared header did not rebuild both objects"
fi

# And the makefile's own CC, which names a compiler that is not there: a make
# WITHOUT the override must fail and say so. That is what makes the override on
# every other step mean something.
if ! wait_token "NOOVER=" "$(scaled 180)"; then
    fail "the run without a macro override never reported"
fi
if grep -q "NOOVER=0 " "$LOG"; then
    fail "a make whose makefile names a compiler that does not exist succeeded"
fi
if ! wait_token "SAW=yes" "$(scaled 60)"; then
    fail "the run without the override did not name the compiler it could not find"
fi

if ! wait_token "CLEANDONE=0 LEFT=0" "$(scaled 120)"; then
    fail ".PHONY clean did not run, or left its targets, with a file named clean present"
fi

# One of this project's own programs, built here from the source the image
# has and compared with the one this machine actually uses.
if ! wait_token "UTILRC=0" "$(scaled 240)"; then
    fail "make could not build this project's own cat"
fi
if ! wait_token "UTILSAME" "$(scaled 120)"; then
    fail "the cat make built does not agree with the shipped one"
fi

if ! wait_token "STAMPED=" "$(scaled 180)"; then
    fail "the object's timestamp was never recorded for the reboot"
fi
if ! wait_token "FIRSTBOOTDONE" "$(scaled 120)"; then
    fail "the first boot never finished its script"
fi
STAMP_BEFORE="$(grep -a -o 'STAMPED=[0-9]*' "$LOG" | tail -1 | cut -d= -f2)"
if [ -z "$STAMP_BEFORE" ] || [ "$STAMP_BEFORE" = "0" ]; then
    fail "the object's recorded timestamp is empty or the epoch: '$STAMP_BEFORE'"
fi
cleanup_boot

# The reboot. Two things are asked. The times must SURVIVE, exactly, which a
# mount that replaced every one of them with a single boot-time value would
# fail. And the clock must not have gone backwards: a machine whose time
# restarts makes everything written before the reboot look newer than everything
# written after, so a source touched now would be older than the object built
# from it then, and make would do nothing.
: > "$STEPS"
step "KEPT" "cd /tmp/demo; echo KEP\"\"T=\$(busybox stat -c %Y lib.o) WANTED=\$(cat lib.o.when)"
step "REBOOTIDLE" "make CC=tcc > /tmp/r1.out 2>&1; echo REBOOT\"\"IDLE=\$? TCC=\$(grep -c tcc /tmp/r1.out)"
step "REBOOTCHANGE" "sleep 1; printf '#include \"demo.h\"\\nint scale(int x){return 4*x;}\\n' > lib.c; echo REBOOT\"\"CHANGE"
step "REBOOTRC" "make CC=tcc > /tmp/r2.out 2>&1; echo REBOOT\"\"RC=\$? LIB=\$(grep -c 'c lib.c' /tmp/r2.out)"
step "REBOOTRAN" "./prog > /tmp/r2.run 2>&1; echo REBOOT\"\"RAN=\$(cat /tmp/r2.run)"
step "SECONDBOOTDONE" "sync; echo SECONDBOOT\"\"DONE"

boot_steps

if ! wait_token "KEPT=$STAMP_BEFORE WANTED=$STAMP_BEFORE" "$(scaled 300)"; then
    fail "the object's timestamp did not survive the reboot exactly (wanted $STAMP_BEFORE)"
fi
if ! wait_token "REBOOTIDLE=0 TCC=0" "$(scaled 180)"; then
    fail "a make after a reboot rebuilt something with nothing touched"
fi
if ! wait_token "REBOOTRC=0 LIB=1" "$(scaled 180)"; then
    fail "a source changed after a reboot did not rebuild: the clock restarted"
fi
if ! wait_token "REBOOTRAN=ANSWER 84" "$(scaled 90)"; then
    fail "the program rebuilt after the reboot is not the new one"
fi
if ! wait_token "SECONDBOOTDONE" "$(scaled 120)"; then
    fail "the second boot never finished its script"
fi
cleanup_boot

echo "make-test: the machine built a program, rebuilt only what changed, did nothing when nothing changed, and kept its times across a reboot: OK"
echo "make-test: PASS"
