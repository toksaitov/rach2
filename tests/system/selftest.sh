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
# `tests/run runner` runs this. It boots no machine: every machine it drives is
# a shell script on the PATH. It does read three built programs, the harness,
# ptydrive and hostpeer, so `sh tests/system/selftest.sh` needs a tree that has
# built those.
#
set -u

# The runner's own tests boot nothing on another kernel: the trees they
# copy have no such target, so the name a caller set is dropped here.
RACH2_KERNEL=""
export RACH2_KERNEL
unset RACH2_TIMEOUT_FACTOR RACH2_IDLE_TIMEOUT RACH2_STOP_ON_FAIL RACH2_KVM_DEVICE
# The fixture writes a few files, and asking Windows for its drive's free
# space under WSL costs a second a run, so the floor is off but where a row
# sets one.
RACH2_MIN_FREE_GIB=0
export RACH2_MIN_FREE_GIB
# The stand-in machines are named qemu-system-i386, the emulator the harness
# and the runner use when no architecture is set, so a caller's is dropped,
# with the make flags tests/run gives it.
unset ARCH RACH2_ARCH RACH2_QEMU RACH2_CPUS MAKEFLAGS MFLAGS

REPO=$(cd "$(dirname "$0")/../.." && pwd)
WORK=$(mktemp -d "${TMPDIR:-/tmp}/rach2-selftest.XXXXXX")
[ -d "$WORK" ] || { echo "selftest: no scratch directory" >&2; exit 1; }
# The stand-in machines and the port claims are stopped here rather than at
# the end of the script, so an interrupted run does not leave one of them
# waiting out its sleep or holding its block.
portclaims=""
stop_machines() {
    for pid in $portclaims; do kill "$pid" 2>/dev/null; done
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
# `prepare-all` that builds the fixture's product and its shared image.
mkdir -p "$WORK/tests/system/runner" "$WORK/tools/setup"
cp "$REPO/tests/run" "$WORK/tests/run"
cp "$REPO/tests/system/runlock.sh" "$REPO/tests/system/runlock_exec.sh" "$WORK/tests/system/"
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
remaker|-|full
stray|-|full
hungry1|-|full||3000
hungry2|-|full||3000
logging|-|full
REG
# A script the runner hashes, so a suite that edits one can be tried.
mkdir -p "$WORK/tests/system"
echo "# the suites' makefile, as the runner sees it" > "$WORK/tests/system/suites.mk"
cat > "$WORK/Makefile" <<'MK'
components:
	@echo components >> order.log; echo $$PPID >> components.count
prepare-all: product build/images/shared.img
	@echo prepared >> order.log; echo $$PPID >> prepared.count
product:
	@echo built >> product.count; sleep 0.3; touch product
build/images/shared.img: product
	@mkdir -p build/images; echo shared > $@; echo dep > $@.d
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
run-remaker: build/images/shared.img
	@echo "remaker ran"; echo shared > build/images/shared.img; echo dep > build/images/shared.img.d
run-stray:
	@mkdir -p build/images; echo stray > build/images/orphan.img; echo "stray ran"
run-failing:
	@echo "failing ran"; exit 1
run-logging:
	@n=$$(($$(cat logging.count 2>/dev/null || echo 0) + 1)); echo $$n > logging.count; \
	 mkdir -p build/images; echo "logging run $$n" > build/images/logging-test.log; \
	 qemu-system-i386 --selftest-say $$n; exit 1
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
run-hungry1:
	@echo hungry1-start >> events; sleep 2; echo hungry1-end >> events
run-hungry2:
	@echo hungry2-start >> events; sleep 2; echo hungry2-end >> events
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
# whose token never comes, so its bound is what ends it. Each records that it
# started, so a row can say a suite never ran. `install` is a name of the
# fixture's registry, not of the tree's, and runs the first script.
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
    *--selftest-say*) for word in "$@"; do said=$word; done; echo "machine said $said"; exit 0 ;;
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
# A shell with job control puts the run and the command its output is piped
# to in one process group, which the run's end must not stop.
out=$(cd "$WORK" && PATH="$WORK/bin:$PATH" RACH2_WORKERS=2 \
      bash -c 'set -m; sh tests/run passing 2>&1 | tail -1; echo "status ${PIPESTATUS[*]}"' 2>&1)
check "a run piped to tail under job control leaves tail its last line" \
    "run: result: all 1 suites passed|status 0 0" "$(printf '%s\n' "$out" | tr '\n' '|' | sed 's/|$//')"

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
out=$(RACH2_IDLE_TIMEOUT=2 run style passing); rc=$?
check "a serial suite before the pool has the idle limit" 1 "$(printf '%s\n' "$out" | grep -c 'run: style: no output for 2 s')"
check "a failed style stops the set before its pool" 1 "$(printf '%s\n' "$out" | grep -c 'run: stopped: style failed; not started: passing')"
check "so nothing in the pool starts" 0 "$(printf '%s\n' "$out" | grep -c 'run: passing: PASS')"
out=$(RACH2_IDLE_TIMEOUT=2 run usb passing); rc=$?
check "a pooled suite has the same bound" 1 "$(printf '%s\n' "$out" | grep -c 'run: usb: no output for 2 s')"
out=$(RACH2_IDLE_TIMEOUT=2 RACH2_TIMEOUT_FACTOR=8 run silent); rc=$?
check "the idle limit is never scaled" 1 "$(printf '%s\n' "$out" | grep -c 'run: silent: no output for 2 s')"

# A KVM device the user cannot open refuses the run, unless the emulator is
# asked for; a disk below the free-space floor refuses it too.
: > "$WORK/kvm-closed"
chmod 000 "$WORK/kvm-closed"
out=$(RACH2_QEMU_ACCEL='' RACH2_KVM_DEVICE="$WORK/kvm-closed" run passing); rc=$?
check "a KVM device this user cannot open refuses the run" 2 "$rc"
check "and says what to do" 1 "$(printf '%s\n' "$out" | grep -c 'cannot read and write it; add the user to its group')"
out=$(RACH2_QEMU_ACCEL=tcg RACH2_KVM_DEVICE="$WORK/kvm-closed" run passing); rc=$?
check "the emulator asked for outright still runs" 0 "$rc"
out=$(RACH2_MIN_FREE_GIB=99999999 run passing); rc=$?
check "a disk below the free-space floor refuses the run" 1 "$rc"
check "and names the floor" 1 "$(printf '%s\n' "$out" | grep -c 'below the 99999999 GiB a run needs')"
out=$(RACH2_MIN_FREE_GIB=lots run passing); rc=$?
check "a floor that is not a number is refused" 1 "$(printf '%s\n' "$out" | grep -c 'RACH2_MIN_FREE_GIB must be a whole number')"

out=$(run failing); rc=$?
check "a failing suite exits nonzero" 1 "$rc"
check "and says which" 1 "$(printf '%s\n' "$out" | grep -c 'run: failing: FAIL')"

# A failing suite's serial logs are kept where the next run does not write
# over them: each failing run copies the log it wrote under build/images and
# its machine's console into a directory of its own, which it names; the next
# run's go into another, and the first is left as it was.
kept() { # kept DIR: the files in DIR, then its log and its console
    printf '%s|%s|%s' "$(cd "$WORK/$1" 2>/dev/null && find . -type f | sed 's|^\./||' | sort | tr '\n' ' ')" \
        "$(cat "$WORK/$1/logging-test.log" 2>/dev/null)" "$(cat "$WORK/$1/console.log" 2>/dev/null)"
}
rm -rf "$WORK/build/tests/failed" "$WORK/logging.count"
out=$(run logging); rc=$?
first=$(printf '%s\n' "$out" | sed -n 's/^run: logging: logs kept in //p')
check "a failing suite that writes a log fails" 1 "$rc"
check "and says where its logs are kept" 1 "$(printf '%s\n' "$first" | grep -c '^build/tests/failed/logging\.')"
out=$(run logging)
second=$(printf '%s\n' "$out" | sed -n 's/^run: logging: logs kept in //p')
check "the next failing run keeps its logs in a directory of its own" 1 \
    "$([ -n "$first" ] && [ -n "$second" ] && [ "$first" != "$second" ] && echo 1 || echo 0)"
check "the first run's directory has its log and console alone, as it left them" \
    'console.log logging-test.log |logging run 1|machine said 1' "$(kept "$first")"
check "and the second run's has its own" \
    'console.log logging-test.log |logging run 2|machine said 2' "$(kept "$second")"

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
# passed and its machine is in its last seconds; asking once, the instant the
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

# A product of the run's own target, remade by a suite's make because the
# build left its prerequisite at the same second, is the build's and not a
# suite's: the run must not fail for it. A file under build/images that no
# product covers and no suite declares must still fail the run by name.
rm -rf "$WORK/build/images"
out=$(run remaker); rc=$?
check "a suite that remakes a product of the run's target passes" 0 "$rc"
check "and the run says nothing about images" 0 "$(printf '%s\n' "$out" | grep -c 'run: images:')"
rm -rf "$WORK/build/images"
out=$(run stray); rc=$?
check "a file no product covers and no suite declares fails the run" 1 "$rc"
check "and is named" 1 "$(printf '%s\n' "$out" | grep -c 'run: images: undeclared: build/images/orphan.img')"
rm -rf "$WORK/build/images"

# The memory budget: two suites that declare 3000 MiB each run one after the
# other under a 4000 MiB budget with two workers free, and together under
# 8000; the budget is printed with the workers.
rm -f "$WORK/events"
out=$(RACH2_MEMORY_MIB=4000 W=2 run hungry1 hungry2); rc=$?
check "two suites past the memory budget run one after the other" 'hungry1-start hungry1-end hungry2-start hungry2-end ' \
    "$(tr '\n' ' ' < "$WORK/events")"
check "and the budget is announced" 1 "$(printf '%s\n' "$out" | grep -c 'memory 4000 MiB')"
rm -f "$WORK/events"
out=$(RACH2_MEMORY_MIB=8000 W=2 run hungry1 hungry2); rc=$?
overlap=$(awk '/-start$/ { active++; if (active > peak) peak = active }
              /-end$/ { active-- } END { print peak + 0 }' "$WORK/events")
check "and together within it" 2 "$overlap"
out=$(RACH2_MEMORY_MIB=nine run passing); rc=$?
check "a budget that is not a number is refused" 2 "$rc"

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
# shellcheck disable=SC2030 # the export ends with the row's subshell
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
# shellcheck disable=SC2030,SC2031 # a row's variables end with its subshell
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
# may still be in its pipe, and a fast new reply may precede expect itself;
# `until=1` after a consumed token is the second's delay, since the harness
# has no step that only sleeps.
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
        --ok READY expect=READY until=1 send=true expect=OLDTOKEN > /dev/null 2>&1
); rc=$?
check "the harness cannot use an old token after a new send" 1 "$([ "$rc" -ne 0 ] && echo 1 || echo 0)"
(
    unset RACH2_DEADLINES RACH2_DEADLINE_FACTOR
    PATH="$WORK/fresh:$PATH" "$HARNESS_BIN" --kernel none --timeout 5 --steptmo 1 \
        --ok FASTTOKEN expect=READY send=fresh expect=FASTTOKEN expect=NEXTTOKEN > /dev/null 2>&1
); rc=$?
check "the harness preserves fast replies and consecutive tokens" 0 "$rc"

(
    unset RACH2_DEADLINES RACH2_DEADLINE_FACTOR
    PATH="$WORK/fresh:$PATH" "$HARNESS_BIN" --kernel none --timeout 5 --steptmo 1 \
        --ok FASTTOKEN expect=READY send=fresh until=20 send=second expect=SECONDREPLY \
        > /dev/null 2>&1
); rc=$?
check "an until preserves a fast reply to the current command" 0 "$rc"

# A machine that prints its last lines and leaves at once: the steps that
# waited for those lines are satisfied by them, since the output is what the
# check is about, and only a token that never came is a step that never ran.
mkdir -p "$WORK/leaving"
cat > "$WORK/leaving/qemu-system-i386" <<'LEAVING'
#!/bin/sh
printf 'READY\nFIRST\nLAST\n'
exit 0
LEAVING
chmod +x "$WORK/leaving/qemu-system-i386"
(
    unset RACH2_DEADLINES RACH2_DEADLINE_FACTOR
    PATH="$WORK/leaving:$PATH" "$HARNESS_BIN" --kernel none --timeout 5 --steptmo 1 \
        --ok LAST expect=READY expect=FIRST expect=LAST > /dev/null 2>&1
); rc=$?
check "the tokens a leaving machine printed satisfy the steps that waited for them" 0 "$rc"
(
    unset RACH2_DEADLINES RACH2_DEADLINE_FACTOR
    PATH="$WORK/leaving:$PATH" "$HARNESS_BIN" --kernel none --timeout 5 --steptmo 1 \
        --ok LAST expect=READY expect=NEVER > /dev/null 2>&1
); rc=$?
check "and a token it never printed is a step that never ran" 1 "$([ "$rc" -ne 0 ] && echo 1 || echo 0)"
(
    unset RACH2_DEADLINES RACH2_DEADLINE_FACTOR
    PATH="$WORK/leaving:$PATH" "$HARNESS_BIN" --kernel none --timeout 5 --steptmo 1 \
        --ok LAST seen=FIRST seen=READY expect=LAST > /dev/null 2>&1
); rc=$?
check "two seen steps are satisfied in either order" 0 "$rc"
(
    unset RACH2_DEADLINES RACH2_DEADLINE_FACTOR
    PATH="$WORK/leaving:$PATH" "$HARNESS_BIN" --kernel none --timeout 5 --steptmo 1 \
        --ok LAST expect=FIRST expect=READY expect=LAST > /dev/null 2>&1
); rc=$?
check "and two expect steps only in theirs" 1 "$([ "$rc" -ne 0 ] && echo 1 || echo 0)"

# A shell that did not know the command: a line typed before the shell was
# reading arrives without its first bytes, and the shell's answer is neither
# silence nor a running probe. The probe step types the line again on it.
mkdir -p "$WORK/refusing"
cat > "$WORK/refusing/qemu-system-i386" <<'REFUSING'
#!/bin/sh
printf 'READY\n'
read -r line
printf 'Unknown command: %s\n' "$line"
read -r line
printf '> %s\n<6>[0.10] process: exited with status 0\n' "$line"
exit 1
REFUSING
chmod +x "$WORK/refusing/qemu-system-i386"
(
    unset RACH2_DEADLINES RACH2_DEADLINE_FACTOR
    PATH="$WORK/refusing:$PATH" "$HARNESS_BIN" --kernel none --timeout 5 --steptmo 3 \
        execq=/hello > /dev/null 2>&1
); rc=$?
check "a probe command the shell did not know is typed again" 0 "$rc"

# A machine that prints the run's token and stays at its prompt: the probe
# step ends on the token, not at its limit, which a set's factor can put
# past the runner's idle limit.
mkdir -p "$WORK/staying"
cat > "$WORK/staying/qemu-system-i386" <<'STAYING'
#!/bin/sh
printf "Type 'help' for available commands.\n"
read -r line
printf '> %s\nBLOCKPROBE OK\n<6>[1.00] process: exited with status 0\n>' "$line"
sleep 30
STAYING
chmod +x "$WORK/staying/qemu-system-i386"
t0=$(now)
(
    unset RACH2_DEADLINES RACH2_DEADLINE_FACTOR
    PATH="$WORK/staying:$PATH" "$HARNESS_BIN" --kernel none --timeout 40 --steptmo 30 \
        --ok "BLOCKPROBE OK" probe=blockprobe > /dev/null 2>&1
); rc=$?
held=$(($(now) - t0))
check "a probe step ends on the run's token" 0 "$rc"
check "and not at its limit" 1 "$([ "$held" -lt 15 ] && echo 1 || echo 0)"

# The native lock excludes another owner even before the owner record exists.
rm -f "$WORK/lock-entered"
# shellcheck disable=SC2016 # $1 is the inner shell's argument
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

# The port claim. Two runs hold blocks 0 and 1 until the test releases them; a
# third waits while both are held, and takes block 1 and its offset once that
# block's holder exits. A block is free again once its holder exits. A holder
# writes its offset to its entered file and lives until its release file
# appears; the bound of 600 tenths of a second only ends a failed test. The
# third claim's command writes its offset only if block 1's release file
# exists when it starts, so a claim that starts while the block is held fails
# its row. A claim whose command has not started when the test is done with it
# is stopped rather than waited for.
mkdir -p "$WORK/ports"
rm -f "$WORK"/ports-entered.* "$WORK"/ports-release.* "$WORK"/ports-out.*
# shellcheck disable=SC2016 # $1 and $2 are the inner shell's arguments
holder='echo "$RACH2_PORT_OFFSET" > "$1"; i=0; while [ ! -e "$2" ] && [ "$i" -lt 600 ]; do sleep 0.1; i=$((i+1)); done'
ports_entered() {
    i=0
    while [ ! -s "$WORK/ports-entered.$1" ] && [ "$i" -lt 600 ]; do sleep 0.1; i=$((i+1)); done
}
ports_end() {
    [ -s "$WORK/ports-entered.$1" ] || kill "$2" 2> /dev/null
    wait "$2"
}
"$SELFTEST_UTIL" ports "$WORK/ports" 2 sh -c "$holder" sh "$WORK/ports-entered.0" "$WORK/ports-release.0" > /dev/null &
portrun0=$!
portclaims=$portrun0
ports_entered 0
"$SELFTEST_UTIL" ports "$WORK/ports" 2 sh -c "$holder" sh "$WORK/ports-entered.1" "$WORK/ports-release.1" > "$WORK/ports-out.1" &
portrun1=$!
portclaims="$portclaims $portrun1"
ports_entered 1
# shellcheck disable=SC2016 # $1 and $2 are the inner shell's arguments
"$SELFTEST_UTIL" ports "$WORK/ports" 2 sh -c 'if [ -e "$2" ]; then echo "$RACH2_PORT_OFFSET"; else echo "started while held"; fi > "$1"' \
    sh "$WORK/ports-entered.2" "$WORK/ports-release.1" > "$WORK/ports-out.2" &
portrun2=$!
portclaims="$portclaims $portrun2"
i=0
while ! grep -q '^run: ports: all 2 blocks are held' "$WORK/ports-out.2" 2> /dev/null && [ "$i" -lt 600 ]; do
    sleep 0.1; i=$((i+1))
done
out=$(cat "$WORK/ports-out.2")
check "the first claim takes block 0" 0 "$(cat "$WORK/ports-entered.0" 2> /dev/null)"
check "a claim beside it takes block 1" 1000 "$(cat "$WORK/ports-entered.1" 2> /dev/null)"
check "and says so" 1 "$(grep -c '^run: ports: block 1, 1000 above' "$WORK/ports-out.1")"
check "a third claim while both are held says it waits" 1 "$(grep -c '^run: ports: all 2 blocks are held by other runs; waiting' "$WORK/ports-out.2")"
check "and does not start its command" 0 "$([ -e "$WORK/ports-entered.2" ] && echo 1 || echo 0)"
: > "$WORK/ports-release.1"
ports_end 1 "$portrun1"
ports_entered 2
ports_end 2 "$portrun2"
out=$(cat "$WORK/ports-out.2")
check "once block 1 is released, the waiting claim takes it" 1000 "$(cat "$WORK/ports-entered.2" 2> /dev/null)"
check "and says so" 1 "$(grep -c '^run: ports: block 1, 1000 above' "$WORK/ports-out.2")"
: > "$WORK/ports-release.0"
ports_end 0 "$portrun0"
portclaims=""
# shellcheck disable=SC2016 # the offset is the inner shell's
out=$("$SELFTEST_UTIL" ports "$WORK/ports" 2 sh -c 'echo "offset $RACH2_PORT_OFFSET"')
check "the block is released at exit" "offset 0" "$out"

# A signal the claim receives before it locks a free block ends it, and the
# command does not start. The signal is made pending and blocked before the
# claim begins, so it is there however soon the block is locked.
cat > "$WORK/pending.c" <<'PENDING'
#include <signal.h>
#include <unistd.h>
int main(int argc, char **argv)
{
    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, SIGTERM);
    sigprocmask(SIG_BLOCK, &set, 0);
    raise(SIGTERM);
    execvp(argv[1], argv + 1);
    return 127;
}
PENDING
cc -o "$WORK/pending" "$WORK/pending.c" || exit 1
rm -f "$WORK/ports-entered"
# shellcheck disable=SC2016 # $1 is the inner shell's argument
"$WORK/pending" "$SELFTEST_UTIL" ports "$WORK/ports" 2 sh -c 'echo started > "$1"' sh "$WORK/ports-entered" > /dev/null; rc=$?
check "a claim signalled before it locks a block ends with 128 plus the signal" 143 "$rc"
check "and its command does not start" 0 "$([ -e "$WORK/ports-entered" ] && echo 1 || echo 0)"

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
check "and the lock goes with the first run" 0 "$([ -e "$WORK/build/.run.lock" ] && echo 1 || echo 0)"
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
# grandchild of the runner and ignores TERM, so only the KILL that follows
# ends it.
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
check "and the lock goes" 0 "$([ -e "$WORK/build/.run.lock" ] && echo 1 || echo 0)"
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

out=$(sh "$REPO/tests/system/selftest_stop.sh" 2>&1); rc=$?
printf '%s\n' "$out"
check "a stopped set reports its running and queued suites and stops their work" 0 "$rc"

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
# A fault script holds the tree through runlock_exec.sh, as a run holds it:
# a run started beside the script is refused with the script named, a run
# the script starts goes on under its hold and leaves the lock alone, a run
# that only claims the hold is refused, and the lock goes with the script,
# also when the script is stopped with TERM.
cat > "$WORK/holder.sh" <<'HOLDER'
#!/bin/sh
repo=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
cd "$repo" || exit 1
. "$repo/tests/system/runlock.sh"
runlock_hold "$repo" holder "$repo/holder.sh" "$@"
case $1 in
    inner) sh tests/run passing; echo "inner status $?" ;;
    wait) echo "$$" > holder-pid; touch holder-in; while [ ! -f holder-go ]; do sleep 0.1; done ;;
esac
HOLDER
rm -rf "$WORK/build/.run.lock" "$WORK/holder-in" "$WORK/holder-go"
out=$(cd "$WORK" && PATH="$WORK/bin:$PATH" RACH2_WORKERS=2 sh holder.sh inner 2>&1); rc=$?
check "a run a lock holder starts goes on under its hold" 1 \
    "$(printf '%s\n' "$out" | grep -c 'held for this run by its caller')"
check "and passes" 1 "$(printf '%s\n' "$out" | grep -c '^inner status 0$')"
check "and the holder exits as its script does" 0 "$rc"
check "and the lock goes with the holder" 0 "$([ -e "$WORK/build/.run.lock" ] && echo 1 || echo 0)"
( cd "$WORK" && sh holder.sh wait > "$WORK/holder.out" 2>&1 ) &
holdrun=$!
i=0
while [ ! -f "$WORK/holder-in" ] && [ "$i" -lt 100 ]; do sleep 0.1; i=$((i + 1)); done
holder=$(sed -n 's/^pid //p' "$WORK/build/.run.lock/owner" 2>/dev/null)
out=$(run passing); rc=$?
check "a run beside a lock holder is refused" 1 "$rc"
check "and the holder is named" 1 "$(printf '%s\n' "$out" | grep -c "another run holds this tree: $holder, holder ")"
out=$(RACH2_TREE_HELD=$holder run passing); rc=$?
check "a run that names the holder but is not its descendant is refused" 1 "$rc"
touch "$WORK/holder-go"
wait "$holdrun"
check "and the lock goes when the holder ends" 0 "$([ -e "$WORK/build/.run.lock" ] && echo 1 || echo 0)"
rm -f "$WORK/holder-in" "$WORK/holder-go"
( cd "$WORK" && sh holder.sh wait > "$WORK/holder.out" 2>&1 ) &
holdrun=$!
i=0
while [ ! -f "$WORK/holder-in" ] && [ "$i" -lt 100 ]; do sleep 0.1; i=$((i + 1)); done
holder=$(sed -n 's/^pid //p' "$WORK/build/.run.lock/owner" 2>/dev/null)
kill -TERM "$holder" 2>/dev/null
wait "$holdrun"; rc=$?
check "a holder stopped with TERM exits as its stopped script does" 143 "$rc"
check "and the lock goes with it" 0 "$([ -e "$WORK/build/.run.lock" ] && echo 1 || echo 0)"
rm -f "$WORK/holder-in" "$WORK/holder-go" "$WORK/holder-pid"
( cd "$WORK" && sh holder.sh wait > "$WORK/holder.out" 2>&1 ) &
holdrun=$!
i=0
while [ ! -f "$WORK/holder-in" ] && [ "$i" -lt 100 ]; do sleep 0.1; i=$((i + 1)); done
kill -KILL "$(cat "$WORK/holder-pid")" 2>/dev/null
wait "$holdrun"; rc=$?
check "a holder whose script was killed exits as the script did" 137 "$rc"
check "and leaves the lock, since the script's work may still run" 1 \
    "$([ -e "$WORK/build/.run.lock" ] && echo 1 || echo 0)"
out=$(run passing); rc=$?
check "which no run takes over, since its work and its fault may remain" 1 "$rc"
check "and the refusal says a fault script's hold was left" 1 \
    "$(printf '%s\n' "$out" | grep -c "a fault script's hold on this tree was left: holder ")"
rm -rf "$WORK/build/.run.lock"
rm -f "$WORK/holder-in" "$WORK/holder-go" "$WORK/holder-pid"

echo "selftest: $((total - fails)) of $total checks passed"
[ "$fails" -eq 0 ] || exit 1
