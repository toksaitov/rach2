#!/usr/bin/env sh
#
# tests/system/selftest.sh: check tests/run itself, without booting anything.
#
# The runner decides which suites exist, which tools they need, what a failure
# looks like and whose machine a leak belongs to. Every one of those decisions
# used to be checked only by the suites happening to work. This runs the runner
# against a registry and a makefile of its own, in a temporary directory, so a
# mistake in the runner is a failure here rather than a puzzling suite result.
#
# `tests/run runner` runs this. It boots no machine and reads nothing from the
# product, so `sh tests/system/selftest.sh` also runs it on a tree that does not
# build.
#
set -u
unset RACH2_TIMEOUT_FACTOR RACH2_IDLE_TIMEOUT RACH2_STOP_ON_FAIL

REPO=$(cd "$(dirname "$0")/../.." && pwd)
WORK=$(mktemp -d "${TMPDIR:-/tmp}/rach2-selftest.XXXXXX")
[ -d "$WORK" ] || { echo "selftest: no scratch directory" >&2; exit 1; }
# The stand-in machines are stopped here rather than at the end of the script,
# so an interrupted run does not leave one of them waiting out its sleep.
stop_machines() {
    [ -f "$WORK/machines" ] || return 0
    while read -r pid; do kill "$pid" 2>/dev/null; done < "$WORK/machines"
}
trap 'stop_machines; rm -rf "$WORK"' EXIT INT TERM HUP

fails=0
total=0
check() {
    total=$((total + 1))
    if [ "$2" = "$3" ]; then
        echo "  PASS: $1"
    else
        echo "  FAIL: $1: expected [$2], got [$3]"
        fails=$((fails + 1))
        printf '%s\n' "${out:-}" | tail -30
    fi
}

out=$(sh "$REPO/tests/system/runutil_group.sh" 2>&1); rc=$?
printf '%s\n' "$out"
check "a group error is accepted only with the requested membership" 0 "$rc"

out=$(sh "$REPO/tests/system/runutil_owned.sh" 2>&1); rc=$?
printf '%s\n' "$out"
check "detached work fails and is stopped before the verdict" 0 "$rc"

# A tree the runner can drive: its own registry, its own suite targets, and a
# `prepare-all` that does nothing.
mkdir -p "$WORK/tests/system/runner" "$WORK/tools/setup"
cp "$REPO/tests/run" "$WORK/tests/run"
cp "$REPO/tests/system/runner/runutil.c" "$WORK/tests/system/runner/runutil.c"
cc -O2 -Wall -Wextra -o "$WORK/runutil" "$REPO/tests/system/runner/runutil.c" || exit 1
SELFTEST_UTIL=$WORK/runutil
now() { "$SELFTEST_UTIL" now; }
sleep() { "$SELFTEST_UTIL" sleep "$1"; }
cp "$REPO/tests/system/deadline.sh" "$WORK/tests/system/deadline.sh"
cp "$REPO/tools/setup/hostcheck.c" "$WORK/tools/setup/hostcheck.c"
# Calibration must never be invoked, even when the host would measure slowly.
cat > "$WORK/tests/system/calibrate.sh" <<'CAL'
#!/bin/sh
echo called >> "$(dirname "$0")/../../calibrations.log"
exit 1
CAL
cat > "$WORK/tests/registry" <<'REG'
# name|tools|tier
passing|-|core
failing|-|full
leaky|-|full
parting|-|full
needy|a-command-that-does-not-exist|full
imagey|hdiutil|full
slow1|-|full
slow2|-|full
stuck|-|full
editing|-|full
slowish|-|full
needsprod1|-|full
needsprod2|-|full
escaper|-|full
rewriter|-|full
slowtoken|-|full
notoken|-|full
install|-|full
silent|-|full
consolebusy|-|full
outputbusy|-|full
consolestuck|-|full
style|-|full
usb|-|full
reporting|-|full
killed|-|full
REG
# A script the runner hashes, so a suite that edits one can be tried.
mkdir -p "$WORK/tests/system"
echo "# the suites' makefile, as the runner sees it" > "$WORK/tests/system/suites.mk"
cat > "$WORK/Makefile" <<'MK'
components:
	@echo components >> order.log; echo $$PPID >> components.count
prepare-all: product
	@echo prepared >> order.log; echo $$PPID >> prepared.count
product:
	@echo built >> product.count; sleep 0.3; touch product
run-silent:
	@echo suite-last-line; sleep 30
run-outputbusy:
	@i=0; while [ $$i -lt 6 ]; do echo suite-progress; sleep 1; i=$$((i+1)); done
run-consolebusy:
	@qemu-system-i386 --selftest-busy > console.log 2>&1
run-consolestuck:
	@qemu-system-i386 --selftest-stuck > console.log 2>&1
run-reporting:
	@i=0; while [ $$i -lt 63 ]; do echo reporting-progress; sleep 1; i=$$((i+1)); done
run-killed:
	@rm -f kill-ready; qemu-system-i386 --selftest-killed & p=$$!; \
	 i=0; while [ ! -s kill-ready ] && [ $$i -lt 100 ]; do sleep 0.1; i=$$((i+1)); done; \
	 kill -KILL $$p; wait $$p 2>/dev/null || :
run-style: run-silent
run-usb: run-silent
run-passing:
	@echo "passing ran"; echo passing >> order.log; echo $$PPID >> ran.pid
run-failing:
	@echo "failing ran"; exit 1
run-leaky:
	@qemu-system-i386 --selftest-hold & \
	 i=0; while [ ! -s "$$RACH2_QEMU_LEDGER" ] && [ $$i -lt 100 ]; do sleep 0.1; i=$$((i+1)); done
run-parting:
	@qemu-system-i386 --selftest-part & \
	 i=0; while [ ! -s "$$RACH2_QEMU_LEDGER" ] && [ $$i -lt 100 ]; do sleep 0.1; i=$$((i+1)); done
run-needy:
	@echo "needy ran"
run-imagey:
	@echo "imagey ran"
run-slow1:
	@echo slow1-start >> events; sleep 2; echo slow1-end >> events; touch slow1.done
run-slow2:
	@echo slow2-start >> events; sleep 2; echo slow2-end >> events
run-stuck:
	@sh -c 'trap "" TERM; sleep 60 & echo $$! > stuck.pid; wait'
run-editing:
	@echo "# edited under the run" >> tests/system/suites.mk
run-slowish:
	@sleep 8
run-needsprod1: product
	@echo "needsprod1 ran"; [ -f product ] && echo needsprod1 >> after-product
run-needsprod2: product
	@echo "needsprod2 ran"; [ -f product ] && echo needsprod2 >> after-product
run-escaper:
	@"$$RACH2_RUN_UTIL" group sh -c 'trap "" TERM; sleep 60 & echo $$! > escaper.pid; wait'
build/thing:
	@mkdir -p build; echo thing > build/thing
run-rewriter: build/thing
	@echo "rewriter ran"; echo changed >> build/thing
run-slowtoken:
	@sh bin/slowtoken.sh
run-notoken:
	@sh bin/notoken.sh
run-install:
	@sh bin/slowtoken.sh
MK
# Two suites that judge by a deadline the set scales: one whose token comes
# after four seconds under a bound written as two, so it passes only with a
# factor of two or more, by the clock and not by a count of sleeps; one
# whose token never comes, so its bound is what ends it. Each records that
# it started, so a row can say a suite never ran. `install` is the runner's
# lone-suite name, and runs the first script.
mkdir -p "$WORK/bin"
cat > "$WORK/bin/slowtoken.sh" <<'SLOW'
#!/bin/sh
. tests/system/deadline.sh
now() { "$RACH2_RUN_UTIL" now; }
sleep() { "$RACH2_RUN_UTIL" sleep "$1"; }
echo "slowtoken started" >> entered.log
bound=$(scaled 2)
rm -f token; (sleep 4; : > token) &
start=$(now)
while :; do
    [ -f token ] && [ $(( $(now) - start )) -le "$bound" ] && { echo "slowtoken ran, bound $bound"; exit 0; }
    [ $(( $(now) - start )) -ge "$bound" ] && break
    sleep 0.2
done
echo "slowtoken: no token within $bound s"; exit 1
SLOW
cat > "$WORK/bin/notoken.sh" <<'NONE'
#!/bin/sh
. tests/system/deadline.sh
now() { "$RACH2_RUN_UTIL" now; }
sleep() { "$RACH2_RUN_UTIL" sleep "$1"; }
echo "notoken started" >> entered.log
bound=$(scaled 2)
start=$(now)
while :; do
    [ -f never ] && exit 0
    [ $(( $(now) - start )) -ge "$bound" ] && break
    sleep 0.2
done
echo "notoken: no token within $bound s"; exit 1
NONE
# A stand-in for QEMU that outlives its suite, so the leak check has something
# to find. It is on PATH ahead of any real one, and it records its pid where
# this script can stop it afterwards. It must become the waiting process rather
# than fork one, because the runner blames the pid it saw, which is the pid the
# shim exec'd into.
mkdir -p "$WORK/bin"
cat > "$WORK/bin/qemu-system-i386" <<'Q'
#!/bin/sh
echo $$ >> "$(dirname "$0")/../machines"
# A machine asked to part goes a moment after the suite that started it
# returns, which is what a real one does: it flushes what it was given first.
case "$*" in
    *--selftest-part*) exec sleep 2 ;;
    *--selftest-hold*) trap '' TERM; exec sleep 60 ;;
    *--selftest-killed*) echo $$ > "$(dirname "$0")/../kill-ready"; exec sleep 30 ;;
    *--selftest-busy*)
        i=0; while [ "$i" -lt 6 ]; do echo console-progress; sleep 1; i=$((i+1)); done
        exit 0 ;;
    *--selftest-stuck*) echo console-last-line; trap '' TERM; exec sleep 30 ;;
esac
exec sleep 30
Q
chmod +x "$WORK/bin/qemu-system-i386"
cat > "$WORK/bin/sleep" <<'SLEEP'
#!/bin/sh
exec "$RACH2_SELFTEST_UTIL" sleep "$1"
SLEEP
chmod +x "$WORK/bin/sleep"
RACH2_SELFTEST_UTIL=$SELFTEST_UTIL; export RACH2_SELFTEST_UTIL
PATH="$WORK/bin:$PATH"; export PATH
: > "$WORK/machines"

run() { ( cd "$WORK" && PATH="$WORK/bin:$PATH" RACH2_WORKERS="${W:-2}" \
          sh tests/run "$@" 2>&1 ); }

out=$(run passing); rc=$?
check "a passing suite exits zero" 0 "$rc"
check "and says so" 1 "$(printf '%s\n' "$out" | grep -c 'run: passing: PASS')"
check "and starts exactly once" 1 "$(printf '%s\n' "$out" | grep -c '^run: starting:')"
check "and does not void an unchanged tree" 0 "$(printf '%s\n' "$out" | grep -c 'run: voided:')"

# The suite supervisor observes both output channels, isolates their clocks,
# and applies the same rule at every position in the schedule.
for value in 0 00 -1 nope 1.5; do
    out=$(RACH2_IDLE_TIMEOUT=$value run passing); rc=$?
    check "an invalid idle limit ($value) is refused" 2 "$rc"
done
t0=$(now)
out=$(RACH2_IDLE_TIMEOUT=2 run silent); rc=$?
check "a silent single suite fails" 1 "$rc"
check "and is named with its idle limit" 1 "$(printf '%s\n' "$out" | grep -c 'run: silent: no output for 2 s')"
check "and repeats its last output" 2 "$(printf '%s\n' "$out" | grep -c '^suite-last-line$')"
check "and stops within seconds" 1 "$([ $(($(now) - t0)) -lt 12 ] && echo 1 || echo 0)"
out=$(RACH2_IDLE_TIMEOUT=2 run consolebusy); rc=$?
check "console output keeps a quiet suite alive" 0 "$rc"
out=$(RACH2_IDLE_TIMEOUT=2 run outputbusy); rc=$?
check "suite output keeps a quiet machine alive" 0 "$rc"
out=$(RACH2_IDLE_TIMEOUT=2 run consolestuck); rc=$?
check "a stopped console fails its suite" 1 "$rc"
check "and prints the console's last output" 1 "$(printf '%s\n' "$out" | grep -c '^console-last-line$')"
out=$(RACH2_IDLE_TIMEOUT=2 run silent consolebusy passing); rc=$?
check "one suite's console cannot keep another suite alive" 1 "$(printf '%s\n' "$out" | grep -c 'run: silent: no output for 2 s')"
check "the active neighbour still passes" 1 "$(printf '%s\n' "$out" | grep -c 'run: consolebusy: PASS')"
check "and the queued suite still runs" 1 "$(printf '%s\n' "$out" | grep -c 'run: passing: PASS')"
out=$(RACH2_IDLE_TIMEOUT=2 run style passing usb); rc=$?
check "a serial suite before the pool has the idle limit" 1 "$(printf '%s\n' "$out" | grep -c 'run: style: no output for 2 s')"
check "a pooled suite has the same bound" 1 "$(printf '%s\n' "$out" | grep -c 'run: usb: no output for 2 s')"
out=$(RACH2_IDLE_TIMEOUT=2 RACH2_TIMEOUT_FACTOR=8 run silent); rc=$?
check "the idle limit is never scaled" 1 "$(printf '%s\n' "$out" | grep -c 'run: silent: no output for 2 s')"

out=$(run failing); rc=$?
check "a failing suite exits nonzero" 1 "$rc"
check "and says which" 1 "$(printf '%s\n' "$out" | grep -c 'run: failing: FAIL')"

out=$(run nosuch); rc=$?
check "an unknown name is refused" 2 "$rc"
check "and the valid names are printed" 1 "$(printf '%s\n' "$out" | grep -c 'passing')"

out=$(run needy); rc=$?
check "a missing tool refuses before building" 1 "$rc"
check "and names the tool" 1 "$(printf '%s\n' "$out" | grep -c 'a-command-that-does-not-exist')"
check "and nothing was prepared" 0 "$(printf '%s\n' "$out" | grep -c 'run: preparing')"

# The host's own refusals, tried and not looked up: a checker that refuses a
# socket bind stops the run before the build, and so does a disk image that
# cannot be attached when a suite needs one, through a formatter that fails.
cat > "$WORK/bin/hostcheck-refusing" <<'HC'
#!/bin/sh
echo "hostcheck: refused: a TCP socket bound on the loopback address" >&2
exit 1
HC
chmod +x "$WORK/bin/hostcheck-refusing"
rm -f "$WORK/prepared.count" "$WORK/components.count" "$WORK/order.log"
out=$(RACH2_HOSTCHECK="$WORK/bin/hostcheck-refusing" run passing); rc=$?
check "a host that refuses a socket bind stops the run" 1 "$rc"
check "and says so before the build" 1 "$(printf '%s\n' "$out" | grep -c 'the host refuses an operation')"
check "and no component was built after a refused bind" 0 "$(cat "$WORK/components.count" "$WORK/prepared.count" 2>/dev/null | wc -l | tr -d ' ')"
cat > "$WORK/bin/hdiutil" <<'HD'
#!/bin/sh
echo "hdiutil: create failed - Device not configured" >&2
exit 1
HD
chmod +x "$WORK/bin/hdiutil"
rm -f "$WORK/prepared.count" "$WORK/components.count" "$WORK/order.log"
out=$(run imagey); rc=$?
check "a disk image that cannot be attached stops a suite that needs one" 1 "$rc"
check "and names the operation" 1 "$(printf '%s\n' "$out" | grep -c 'a disk image created, attached and detached')"
check "and no component was built after a failed image" 0 "$(cat "$WORK/components.count" "$WORK/prepared.count" 2>/dev/null | wc -l | tr -d ' ')"
rm -f "$WORK/bin/hdiutil"
out=$(run passing); rc=$?
check "a suite that needs no image runs beside the failing formatter's absence" 0 "$rc"

out=$(run passing failing); rc=$?
check "a set with one failure exits nonzero" 1 "$rc"
check "the failure is named in the result" 1 "$(printf '%s\n' "$out" | grep -c 'result: failed: failing')"
check "the passing member is still reported" 1 "$(printf '%s\n' "$out" | grep -c 'run: passing: PASS')"

out=$(run core); rc=$?
check "the core tier exits zero" 0 "$rc"
check "the core tier runs its members" 1 "$(printf '%s\n' "$out" | grep -c 'run: passing: PASS')"
check "and not the others" 0 "$(printf '%s\n' "$out" | grep -c 'run: failing:')"

out=$(run leaky); rc=$?
check "a suite that leaks a machine fails" 1 "$rc"
check "and is named as the owner" 1 "$(printf '%s\n' "$out" | grep -c 'run: leaky: recorded group did not stop within 15 s: pid ')"

# A suite's recorded PID must still be the machine's, even with a relay.
out=$(run killed); rc=$?
check "killing the machine PID stops the actual machine" 0 "$rc"
check "and leaves no machine or console relay behind" 0 "$(printf '%s\n' "$out" | grep -c 'run: killed: left')"
# A machine on its way out is not a machine left behind. The suite's rows have
# passed and its machine has been told to stop; asking once, the instant the
# suite returns, fails a suite for a race with a process that is already
# leaving, which is what it cost on 2026-09-13.
out=$(run parting); rc=$?
check "a suite whose machine is still leaving passes" 0 "$rc"
check "and is not named as an owner" 0 "$(printf '%s\n' "$out" | grep -c 'run: parting: left')"

out=$(W=0 run passing); rc=$?
check "a worker count of zero is refused" 2 "$rc"
out=$(W=00 run passing); rc=$?
check "a worker count of zero written with two digits is refused" 2 "$rc"
out=$(W=nine run passing); rc=$?
check "a worker count that is not a number is refused" 2 "$rc"

out=$(run core passing); rc=$?
check "a set and a name together are refused" 2 "$rc"
out=$(run passing passing); rc=$?
check "the same name twice is refused" 2 "$rc"

# The product and the tools are built once for the whole run, not once per
# suite: twenty suites each running make on one tree would build the same
# object twenty times, or race over it. A suite run alone builds through its
# own target instead, so an edit far from it does not rebuild the whole
# product first.
# The components are built first and in a Make of their own, before the
# Make that prepares the rest or runs the lone suite: the fake targets record
# the order they ran in and the pid of the make that ran each.
rm -f "$WORK/prepared.count" "$WORK/components.count" "$WORK/order.log"
out=$(run passing failing)
check "a run of two suites prepares once" 1 \
    "$(wc -l < "$WORK/prepared.count" | tr -d ' ')"
check "after the components" components "$(head -n 1 "$WORK/order.log")"
check "in a make of their own" 1 \
    "$([ "$(cat "$WORK/components.count")" != "$(cat "$WORK/prepared.count")" ] && echo 1 || echo 0)"
rm -f "$WORK/prepared.count" "$WORK/components.count" "$WORK/order.log" "$WORK/ran.pid"
out=$(run passing)
check "a suite run alone does not prepare the whole product" 0 \
    "$(cat "$WORK/prepared.count" 2>/dev/null | wc -l | tr -d ' ')"
check "but builds the components first" components "$(head -n 1 "$WORK/order.log")"
check "in a make of their own" 1 \
    "$([ "$(cat "$WORK/components.count")" != "$(cat "$WORK/ran.pid")" ] && echo 1 || echo 0)"
check "and still runs" 1 "$(printf '%s\n' "$out" | grep -c 'passing ran')"

# Scheduling is judged inside the suites. Compiler startup and cleanup are
# outside the measured interval and cannot turn overlap into a failure.
rm -f "$WORK/events"
out=$(W=1 run slow1 slow2); rc=$?
check "one worker runs two suites one after the other" 'slow1-start slow1-end slow2-start slow2-end ' \
    "$(tr '\n' ' ' < "$WORK/events")"
rm -f "$WORK/events"
out=$(W=2 run slow1 slow2); rc=$?
overlap=$(awk '/-start$/ { active++; if (active > peak) peak = active }
              /-end$/ { active-- } END { print peak + 0 }' "$WORK/events")
check "two workers run them together" 2 "$overlap"
check "both concurrent fixtures complete" 4 "$(wc -l < "$WORK/events" | tr -d ' ')"
check "and each concurrent suite is announced" 2 "$(printf '%s\n' "$out" | grep -c '^run: slow[12]: starting')"

# Host speed does not decide whether a set starts. An explicit factor changes
# only completion bounds, for one suite and for every part of a set alike.
rm -f "$WORK/calibrations.log"
out=$(run passing slow1); rc=$?
check "a set runs without calibration" 0 "$rc"
check "and never invokes the calibration script" 0 "$([ -f "$WORK/calibrations.log" ] && echo 1 || echo 0)"
out=$(RACH2_TIMEOUT_FACTOR=3 run slowtoken passing install); rc=$?
check "an explicit factor applies to pooled and serial suites" 0 "$rc"
check "and is recorded at the start" 1 "$(printf '%s\n' "$out" | grep -c 'timeout factor 3.0')"
check "both suites receive the bound once" 2 "$(printf '%s\n' "$out" | grep -c 'slowtoken ran, bound 6')"
out=$(RACH2_TIMEOUT_FACTOR=3 run slowtoken); rc=$?
check "the same explicit factor applies to a single suite" 0 "$rc"
out=$(RACH2_TIMEOUT_FACTOR=8.2 run passing slow1); rc=$?
check "a factor above the former envelope still runs" 0 "$rc"
out=$(RACH2_TIMEOUT_FACTOR=08.0 run passing); rc=$?
check "a factor with leading zeros is decimal" 0 "$rc"
check "and is printed as a decimal" 1 "$(printf '%s\n' "$out" | grep -c 'timeout factor 8.0')"
out=$(RACH2_TIMEOUT_FACTOR=abc run passing); rc=$?
check "a malformed explicit factor is refused" 2 "$rc"
out=$(RACH2_TIMEOUT_FACTOR=0 run passing); rc=$?
check "a zero explicit factor is refused" 2 "$rc"
out=$(RACH2_TIMEOUT_FACTOR=65 run passing); rc=$?
check "a factor beyond the consumers' range is refused" 2 "$rc"
out=$( (export RACH2_DEADLINES=scaled RACH2_DEADLINE_FACTOR=9.0; run slowtoken) ); rc=$?
check "an inherited internal factor is ignored" 1 "$rc"
check "and the written bound is used by default" 1 "$(printf '%s\n' "$out" | grep -c 'no token within 2 s')"
out=$(RACH2_TIMEOUT_FACTOR=3 run notoken passing); rc=$?
check "a missing token fails at the explicit bound" 1 "$rc"
check "and names that bound" 1 "$(printf '%s\n' "$out" | grep -c 'notoken: no token within 6 s')"

# Driver bounds are already elapsed seconds. The enclosing supervisor keeps
# a mutation that scales them again from holding this selftest indefinitely.
cat > "$WORK/driver-bound.sh" <<'DRIVER'
#!/bin/sh
REPO=$1
. "$REPO/tests/system/drive.sh"
DRIVE_LOG=$2
: > "$DRIVE_LOG"
drive_expect missing "$(scaled 1)"
DRIVER
out=$(RACH2_DEADLINES=scaled RACH2_DEADLINE_FACTOR=2 RACH2_RUN_UTIL="$SELFTEST_UTIL" \
    "$SELFTEST_UTIL" watch driver 3 "$WORK/driver.machines" \
    sh "$WORK/driver-bound.sh" "$REPO" "$WORK/driver.log" 2>&1); rc=$?
check "a driver bound supplied by scaled is applied once" 1 "$(printf '%s\n' "$out" | grep -c "no 'missing' within 2 s")"
check "the driver's bound ends the wait" 1 "$rc"
(
    REPO=$REPO
    . "$REPO/tests/system/drive.sh"
    DRIVE_LOG=$WORK/driver.log
    printf '~ # Running in background\r\nother daemon output\r\n' > "$DRIVE_LOG"
    drive_prompt 1
); rc=$?
check "a daemon's output after a prompt leaves the prompt reachable" 0 "$rc"
(
    . "$REPO/tests/system/drive.sh"
    DRIVE_LOG=$WORK/driver.log
    printf '~ # Running in background\r\n' > "$DRIVE_LOG"
    drive_prompt 1 && drive_expect 'Running in background' 1
); rc=$?
check "matching a prompt preserves the daemon's following token" 0 "$rc"
(
    . "$REPO/tests/system/drive.sh"
    DRIVE_LOG=$WORK/driver.log
    printf 'printf x > ' > "$DRIVE_LOG"
    drive_prompt 1 2>/dev/null
); rc=$?
check "a command echo paused at redirection is not a prompt" 1 "$rc"
(
    . "$REPO/tests/system/drive.sh"
    DRIVE_LOG=$WORK/driver.log
    DRIVE_FIFO=$WORK/driver.fifo
    DRIVE_FPID=$$
    printf 'OLDTOKEN\n' > "$DRIVE_LOG"
    drive_type 'true'
    drive_expect OLDTOKEN 1 2>/dev/null
); rc=$?
check "a token before the command is not its answer" 1 "$rc"
(
    . "$REPO/tests/system/drive.sh"
    DRIVE_LOG=$WORK/driver.log
    DRIVE_FIFO=$WORK/driver.fifo
    DRIVE_FPID=$$
    : > "$DRIVE_LOG"
    drive_type 'true'
    printf 'FASTTOKEN\n# ' >> "$DRIVE_LOG"
    drive_expect FASTTOKEN 1 && drive_prompt 1
); rc=$?
check "a reply and prompt that arrive before the wait are preserved" 0 "$rc"

# The consumers of the factor, each a real program of the tree driven by a
# stand-in for what it waits on: the harness over a machine that prints its
# token after two and a half seconds, ptydrive over a command that answers
# after the same, and hostpeer's lifetime; each under a deadline written as
# one second, which a factor of four makes enough and no factor leaves
# short, with a second and a half of room each way for the host's
# scheduling; each row unsets the variables first, since this test may
# itself be running under a set's factor. The harness's own deadlines are
# the ones the suite rows use.
HARNESS_BIN="$REPO/build/tests/system/harness"
PTYDRIVE_BIN="$REPO/build/tests/system/ptydrive"
HOSTPEER_BIN="$REPO/build/tests/system/hostpeer"
if [ -x "$HARNESS_BIN" ] && [ -x "$PTYDRIVE_BIN" ] && [ -x "$HOSTPEER_BIN" ]; then
    mkdir -p "$WORK/late"
    cat > "$WORK/late/qemu-system-i386" <<'LATE'
#!/bin/sh
# A machine that prints its prompt at once and its token late, whatever it
# is asked to boot; it reads its console so the harness's typing has a sink.
printf "Type 'help'\n"
(sleep 2.5; printf 'LATETOKEN\n') &
cat > /dev/null
wait
LATE
    chmod +x "$WORK/late/qemu-system-i386"
    late_run() { ( PATH="$WORK/late:$PATH" "$HARNESS_BIN" --kernel none --initrd none --timeout 1 --steptmo 1 --grace 0.2 --ok LATETOKEN "expect=Type 'help'" "expect=LATETOKEN" > /dev/null 2>&1 ); }
    (unset RACH2_DEADLINES RACH2_DEADLINE_FACTOR; late_run); rc=$?
    check "the harness as written gives a late token one second" 1 "$([ "$rc" -ne 0 ] && echo 1 || echo 0)"
    (export RACH2_DEADLINES=scaled RACH2_DEADLINE_FACTOR=4.0; late_run); rc=$?
    check "and four under the factor, which is enough" 0 "$rc"
    (export RACH2_DEADLINES=scaled; unset RACH2_DEADLINE_FACTOR; late_run); rc=$?
    check "the mode without a factor leaves the harness as written" 1 "$([ "$rc" -ne 0 ] && echo 1 || echo 0)"
    out=$( (unset RACH2_DEADLINES RACH2_DEADLINE_FACTOR; RACH2_PASSWORD=x "$PTYDRIVE_BIN" --timeout 1 -- sh -c 'sleep 2.5; echo PTYDONE' 2>&1) )
    check "ptydrive as written times a late command out" 1 "$(printf '%s\n' "$out" | grep -c TIMEDOUT)"
    out=$( (export RACH2_DEADLINES=scaled RACH2_DEADLINE_FACTOR=4.0 RACH2_PASSWORD=x; "$PTYDRIVE_BIN" --timeout 1 -- sh -c 'sleep 2.5; echo PTYDONE' 2>&1) )
    check "and under the factor reads its answer" 1 "$(printf '%s\n' "$out" | grep -c PTYDONE)"
    check "in time" 0 "$(printf '%s\n' "$out" | grep -c TIMEDOUT)"
    t0=$(now); (unset RACH2_DEADLINES RACH2_DEADLINE_FACTOR; "$HOSTPEER_BIN" echo 47123 1 > /dev/null 2>&1); rc=$?; held=$(($(now) - t0))
    check "hostpeer as written holds its port for a second" 1 "$([ "$rc" -eq 0 ] && [ "$held" -le 2 ] && echo 1 || echo 0)"
    t0=$(now); (export RACH2_DEADLINES=scaled RACH2_DEADLINE_FACTOR=4.0; "$HOSTPEER_BIN" echo 47123 1 > /dev/null 2>&1); rc=$?; held=$(($(now) - t0))
    check "and under the factor for four" 1 "$([ "$rc" -eq 0 ] && [ "$held" -ge 4 ] && [ "$held" -le 8 ] && echo 1 || echo 0)"
else
    echo "  SKIP: the harness, ptydrive and hostpeer are not built; their factor rows did not run"
    fails=$((fails + 1))
fi

# The harness also starts its search before sending a command. Old output
# may still be in its pipe, and a fast new reply may precede expect itself.
mkdir -p "$WORK/fresh"
cat > "$WORK/fresh/qemu-system-i386" <<'FRESH'
#!/bin/sh
printf 'READY\nOLDTOKEN\n'
while IFS= read -r line; do
    case "$line" in
        fresh) printf 'FASTTOKEN\nNEXTTOKEN\n' ;;
        second) printf 'SECONDREPLY\n' ;;
    esac
done
FRESH
chmod +x "$WORK/fresh/qemu-system-i386"
(
    unset RACH2_DEADLINES RACH2_DEADLINE_FACTOR
    PATH="$WORK/fresh:$PATH" "$HARNESS_BIN" --kernel none --timeout 5 --steptmo 1 \
        --ok READY expect=READY wait=1 send=true expect=OLDTOKEN > /dev/null 2>&1
); rc=$?
check "the harness cannot use an old token after a new send" 1 "$([ "$rc" -ne 0 ] && echo 1 || echo 0)"
(
    unset RACH2_DEADLINES RACH2_DEADLINE_FACTOR
    PATH="$WORK/fresh:$PATH" "$HARNESS_BIN" --kernel none --timeout 5 --steptmo 1 \
        --ok FASTTOKEN expect=READY send=fresh wait=1 expect=FASTTOKEN expect=NEXTTOKEN > /dev/null 2>&1
); rc=$?
check "the harness preserves fast replies and consecutive tokens" 0 "$rc"

(
    unset RACH2_DEADLINES RACH2_DEADLINE_FACTOR
    PATH="$WORK/fresh:$PATH" "$HARNESS_BIN" --kernel none --timeout 5 --steptmo 1 \
        --ok FASTTOKEN expect=READY send=fresh wait=1 until=20 send=second expect=SECONDREPLY \
        > /dev/null 2>&1
); rc=$?
check "an until preserves a fast reply to the current command" 0 "$rc"

# The native lock excludes another owner even before the owner record exists.
rm -f "$WORK/lock-entered"
"$SELFTEST_UTIL" lock "$WORK/guard" "$WORK/no-owner" sh -c 'echo ready > "$1"; sleep 2' sh "$WORK/lock-entered" &
lockrun=$!
i=0
while [ ! -f "$WORK/lock-entered" ] && [ "$i" -lt 50 ]; do sleep 0.1; i=$((i+1)); done
out=$("$SELFTEST_UTIL" lock "$WORK/guard" "$WORK/no-owner" sh -c 'echo entered'); rc=$?
check "the record lock excludes a concurrent owner" 1 "$rc"
check "and the second command never starts" 0 "$(printf '%s\n' "$out" | grep -c '^entered$')"
wait "$lockrun"
out=$("$SELFTEST_UTIL" lock "$WORK/guard" "$WORK/no-owner" sh -c 'echo entered'); rc=$?
check "the record lock is released at exit" 0 "$rc"

# What a run owns. One run at a time on a tree: a second is refused with the
# first named; a lock a dead run left is taken over, unless something of that
# run's process group is still alive, which is named for the person to stop.
mkdir -p "$WORK/build"
rm -rf "$WORK/build/.run.lock"
( cd "$WORK" && PATH="$WORK/bin:$PATH" RACH2_WORKERS=2 sh tests/run slow1 > "$WORK/first.out" 2>&1 ) &
first=$!
sleep 0.7
out=$(run passing); rc=$?
check "a second run on a tree with one in flight is refused" 1 "$rc"
check "and the first is named" 1 "$(printf '%s\n' "$out" | grep -c 'another run holds this tree: [0-9]*, slow1')"
wait "$first"
check "and the lock goes with the first run" 0 "$(ls -d "$WORK/build/.run.lock" 2>/dev/null | wc -l | tr -d ' ')"
mkdir -p "$WORK/build/.run.lock"
printf 'pid 999999\npgid 999999\nsince long ago\nargs old\n' > "$WORK/build/.run.lock/owner"
out=$(run passing); rc=$?
check "a lock whose run is gone is taken over" 0 "$rc"
check "and says whose it was" 1 "$(printf '%s\n' "$out" | grep -c 'taking over the lock a run left: 999999, old')"
"$SELFTEST_UTIL" group sleep 20 &
survivor=$!
sleep 0.3
mkdir -p "$WORK/build/.run.lock"
printf 'pid 999999\npgid %s\nsince long ago\nargs old\n' "$survivor" > "$WORK/build/.run.lock/owner"
out=$(run passing); rc=$?
check "a lock whose run is gone but whose work is not is refused" 1 "$rc"
check "and the surviving work is named" 1 "$(printf '%s\n' "$out" | grep -c "left work running: pids.*$survivor")"
kill "$survivor" 2>/dev/null; wait "$survivor" 2>/dev/null
rm -rf "$WORK/build/.run.lock"

# A run stopped by a signal takes its work with it: the suite's sleep is a
# grandchild of the runner, in its process group and in no ledger, and it
# ignores TERM, so only the KILL that follows the group's TERM ends it.
( cd "$WORK" && PATH="$WORK/bin:$PATH" RACH2_WORKERS=2 sh tests/run stuck > "$WORK/stuck.out" 2>&1 ) &
stuckrun=$!
i=0
while [ ! -s "$WORK/stuck.pid" ] && [ "$i" -lt 100 ]; do sleep 0.1; i=$((i + 1)); done
runner=$(sed -n 's/^pid //p' "$WORK/build/.run.lock/owner" 2>/dev/null)
kill -TERM "$runner" 2>/dev/null
wait "$stuckrun"; rc=$?
sleep 0.5
check "a run stopped with TERM exits as the signal says" 143 "$rc"
check "and its suite's work is stopped with it" 0 "$(kill -0 "$(cat "$WORK/stuck.pid" 2>/dev/null)" 2>/dev/null && echo 1 || echo 0)"
check "and the lock goes" 0 "$(ls -d "$WORK/build/.run.lock" 2>/dev/null | wc -l | tr -d ' ')"
kill "$(cat "$WORK/stuck.pid" 2>/dev/null)" 2>/dev/null

# The same, with HUP, which has a status of its own.
rm -f "$WORK/stuck.pid"
( cd "$WORK" && PATH="$WORK/bin:$PATH" RACH2_WORKERS=2 sh tests/run stuck > "$WORK/stuck.out" 2>&1 ) &
stuckrun=$!
i=0
while [ ! -s "$WORK/stuck.pid" ] && [ "$i" -lt 100 ]; do sleep 0.1; i=$((i + 1)); done
runner=$(sed -n 's/^pid //p' "$WORK/build/.run.lock/owner" 2>/dev/null)
kill -HUP "$runner" 2>/dev/null
wait "$stuckrun"; rc=$?
check "a run stopped with HUP exits as that signal says" 129 "$rc"
kill "$(cat "$WORK/stuck.pid" 2>/dev/null)" 2>/dev/null

# A suite's process that left the runner's group, as the harness's deadline
# makes a machine do, is still the run's while the suite runs, and a signal
# stops it too; it ignores TERM, so the KILL that follows must reach it.
rm -f "$WORK/escaper.pid"
( cd "$WORK" && PATH="$WORK/bin:$PATH" RACH2_WORKERS=2 sh tests/run escaper > "$WORK/escaper.out" 2>&1 ) &
escrun=$!
i=0
while [ ! -s "$WORK/escaper.pid" ] && [ "$i" -lt 100 ]; do sleep 0.1; i=$((i + 1)); done
runner=$(sed -n 's/^pid //p' "$WORK/build/.run.lock/owner" 2>/dev/null)
kill -TERM "$runner" 2>/dev/null
wait "$escrun"; rc=$?
sleep 0.3
check "a run stopped while a suite's process is outside its group exits as the signal says" 143 "$rc"
check "and that process is stopped with it" 0 "$(kill -0 "$(cat "$WORK/escaper.pid" 2>/dev/null)" 2>/dev/null && echo 1 || echo 0)"
kill -KILL "$(cat "$WORK/escaper.pid" 2>/dev/null)" 2>/dev/null

# A lock being taken this moment, a directory with no owner file yet, is not
# a lock left behind; one left with a machine still in its ledger is.
mkdir -p "$WORK/build/.run.lock"
out=$(run passing); rc=$?
check "a lock being taken by another run is not taken over" 1 "$rc"
check "and says so" 1 "$(printf '%s\n' "$out" | grep -c 'another run is taking this tree')"
rm -rf "$WORK/build/.run.lock"
sleep 30 &
machine=$!
mkdir -p "$WORK/build/.run.lock" "$WORK/oldwork"
echo "$machine" > "$WORK/oldwork/some.machines"
printf 'pid 999999\npgid 999999\nwork %s\nsince long ago\nargs old\n' "$WORK/oldwork" > "$WORK/build/.run.lock/owner"
out=$(run passing); rc=$?
check "a lock whose run left a machine in its ledger is refused" 1 "$rc"
check "and the machine is named" 1 "$(printf '%s\n' "$out" | grep -c "left work running: pids.*$machine")"
kill "$machine" 2>/dev/null; wait "$machine" 2>/dev/null
rm -rf "$WORK/build/.run.lock" "$WORK/oldwork"

# Every way out of the runner says `run: result:`, and a run that began says
# `run: starting:`. A reader waiting for the result of a run cannot otherwise
# tell a refusal from a run still going: on 2026-09-14 a refused run left one
# line in a log, a watcher waited for a result that would never come, and a
# session sat idle for eight hours.
mkdir -p "$WORK/build/.run.lock"
printf 'pid %s\npgid %s\nwork %s\nsince now\nargs holding\n' \
    "$$" "$("$SELFTEST_UTIL" pgid $$)" "$WORK" > "$WORK/build/.run.lock/owner"
out=$(run passing); rc=$?
check "a run refused by a live lock exits non-zero" 1 "$rc"
check "and ends with a result line" 1 "$(printf '%s\n' "$out" | grep -c '^run: result: refused:')"
check "and never says it started" 0 "$(printf '%s\n' "$out" | grep -c '^run: starting:')"
rm -rf "$WORK/build/.run.lock"
out=$(run nosuchsuite); rc=$?
check "a run refused for an unknown name ends with a result line" 1 \
    "$(printf '%s\n' "$out" | grep -c '^run: result: refused:')"
out=$(run passing); rc=$?
check "a run that began says so" 1 "$(printf '%s\n' "$out" | grep -c '^run: starting: passing')"
check "and still ends with its own result" 1 "$(printf '%s\n' "$out" | grep -c '^run: passing: PASS')"

# A result is bound to what made it: a suite that changes a hashed script
# under the run voids the run, by the file's name; so does one that changes
# a product it was run against.
out=$(run editing); rc=$?
check "a run whose input changed under it fails" 1 "$rc"
check "and names the file" 1 "$(printf '%s\n' "$out" | grep -c 'voided: tests/system/suites.mk changed during the run')"

# And it stops there rather than at the end. A set is an hour and more; a
# source changed in its first minute cost the whole of it until the check ran
# as the set went, which is what it cost on 2026-09-13.
echo "# the suites' makefile, as the runner sees it" > "$WORK/tests/system/suites.mk"
out=$(run editing slowish); rc=$?
check "a set whose source changes under it is stopped there" 1 "$(printf '%s\n' "$out" | grep -c 'stopping the set: the tree changed under it')"
check "and the set fails" 1 "$rc"
check "and the live check names the changed path" 1 "$(printf '%s\n' "$out" | grep -q '^run: voided: tests/system/suites.mk changed during the run' && echo 1 || echo 0)"
echo "# the suites' makefile, as the runner sees it" > "$WORK/tests/system/suites.mk"
rm -f "$WORK/build/thing"
out=$(run rewriter); rc=$?
check "a run whose product changed under it fails" 1 "$rc"
check "and names the product" 1 "$(printf '%s\n' "$out" | grep -c 'voided: build/thing changed during the run')"

# With RACH2_STOP_ON_FAIL, the first failure stops the pool: the suite still
# running is stopped and named, and the run ends well before it would have.
rm -f "$WORK/slow1.done"
t0=$(now)
out=$(cd "$WORK" && PATH="$WORK/bin:$PATH" RACH2_WORKERS=2 RACH2_STOP_ON_FAIL=1 sh tests/run failing slow1 slow2 2>&1); rc=$?
check "a set stopped on its first failure fails" 1 "$rc"
check "the running suite is stopped and named" 1 "$(printf '%s\n' "$out" | grep -c 'stopped part way: slow1')"
check "the queued suite is named" 1 "$(printf '%s\n' "$out" | grep -c 'not started: slow2')"
check "and the run ended early" 1 "$([ $(($(now) - t0)) -lt 4 ] && echo 1 || echo 0)"
check "and the running suite did not reach its end" 0 "$(ls "$WORK/slow1.done" 2>/dev/null | wc -l | tr -d ' ')"

# A pooled suite's result is printed the moment it ends, not when the pool
# ends: the fast suite's line is seen while the slow one has not written its
# end marker.
rm -f "$WORK/slow1.done" "$WORK/stream.out"
( cd "$WORK" && PATH="$WORK/bin:$PATH" RACH2_WORKERS=2 sh tests/run slow1 passing > "$WORK/stream.out" 2>&1 ) &
streamrun=$!
seen=0
i=0
while [ "$i" -lt 30 ]; do
    if grep -q 'run: passing: PASS' "$WORK/stream.out" 2>/dev/null && [ ! -f "$WORK/slow1.done" ]; then
        seen=1; break
    fi
    sleep 0.1; i=$((i + 1))
done
wait "$streamrun"
check "a suite's result is printed while a slower one still runs" 1 "$seen"

# Progress goes to the run log while suite output is held. The bound here
# exceeds the report interval, and the fixture remains active throughout.
(cd "$WORK" && RACH2_WORKERS=2 sh tests/run reporting passing > "$WORK/progress.out" 2>&1) &
progressrun=$!
seen=0
until_time=$(($(now) + 75))
while [ "$(now)" -lt "$until_time" ]; do
    if grep -q '^run: reporting: working for ' "$WORK/progress.out" &&
       ! grep -q '^run: reporting: PASS' "$WORK/progress.out"; then
        seen=1; break
    fi
    kill -0 "$progressrun" 2>/dev/null || break
    sleep 1
done
wait "$progressrun"; rc=$?
check "progress is visible before a pooled suite finishes" 1 "$seen"
check "the reporting suite still completes" 0 "$rc"
check "each pooled suite publishes one result" 2 "$(grep -c '^run: .*: PASS in ' "$WORK/progress.out")"

# None of these optional host commands is part of the runner's runtime.
for tool in perl pgrep stat lsof md5 shasum sha256sum; do
    cat > "$WORK/bin/$tool" <<'UNAVAILABLE'
#!/bin/sh
echo "$0" >> "$RACH2_UNAVAILABLE_LOG"
exit 99
UNAVAILABLE
    chmod +x "$WORK/bin/$tool"
done
RACH2_UNAVAILABLE_LOG=$WORK/unavailable.log; export RACH2_UNAVAILABLE_LOG
out=$(run passing slow1); rc=$?
check "the runner works without platform-specific utility extensions" 0 "$rc"
check "and does not invoke the unavailable tools" 0 "$([ -f "$WORK/unavailable.log" ] && echo 1 || echo 0)"
rm -f "$WORK/bin/perl" "$WORK/bin/pgrep" "$WORK/bin/stat" "$WORK/bin/lsof" \
    "$WORK/bin/md5" "$WORK/bin/shasum" "$WORK/bin/sha256sum"

# A product two pooled suites name is built once, by the preparation, before
# either starts.
rm -f "$WORK/product" "$WORK/product.count" "$WORK/after-product"
out=$(run needsprod1 needsprod2); rc=$?
check "two suites that name one product pass" 0 "$rc"
check "the product was built once" 1 "$(wc -l < "$WORK/product.count" | tr -d ' ')"
check "and both ran after it" 2 "$(wc -l < "$WORK/after-product" | tr -d ' ')"
echo "selftest: $((total - fails)) of $total checks passed"
[ "$fails" -eq 0 ] || exit 1
