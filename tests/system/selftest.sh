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
    fi
}

# A tree the runner can drive: its own registry, its own suite targets, and a
# `prepare-all` that does nothing.
mkdir -p "$WORK/tests/system" "$WORK/tools/setup"
cp "$REPO/tests/run" "$WORK/tests/run"
cp "$REPO/tests/system/deadline.sh" "$WORK/tests/system/deadline.sh"
cp "$REPO/tools/setup/hostcheck.c" "$WORK/tools/setup/hostcheck.c"
# The calibration, as a fixture: the numbers a set's calibration would
# measure are read from two files, so a row chooses the factor a set gets.
cat > "$WORK/tests/system/calibrate.sh" <<'CAL'
#!/bin/sh
# `calib.fitat` holds a compute time and the widest pool it is reported at:
# wider than that, the slow time in `calib.compute` is reported instead. It is
# how a row makes a host that is too slow for the width it was given.
root="$(dirname "$0")/../.."
echo "calibrated at width $1" >> "$root/calibrations.log"
compute=$(cat "$root/calib.compute")
if [ -f "$root/calib.fitat" ]; then
    set -- "$1" $(cat "$root/calib.fitat")
    [ "$1" -le "$3" ] && compute=$2
fi
echo "compute=$compute storage=$(cat "$root/calib.storage") width=$1"
CAL
echo 0.5 > "$WORK/calib.compute"
echo 0.5 > "$WORK/calib.storage"
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
	@sleep 2; touch slow1.done
run-slow2:
	@sleep 2
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
	@perl -e 'setpgrp(0, 0); exec "sh", "-c", "trap \"\" TERM; sleep 60 & echo \$$! > escaper.pid; wait"'
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
echo "slowtoken started" >> entered.log
bound=$(scaled 2)
rm -f token; (sleep 4; : > token) &
start=$(date +%s)
while :; do
    [ -f token ] && [ $(( $(date +%s) - start )) -le "$bound" ] && { echo "slowtoken ran, bound $bound"; exit 0; }
    [ $(( $(date +%s) - start )) -ge "$bound" ] && break
    sleep 0.2
done
echo "slowtoken: no token within $bound s"; exit 1
SLOW
cat > "$WORK/bin/notoken.sh" <<'NONE'
#!/bin/sh
. tests/system/deadline.sh
echo "notoken started" >> entered.log
bound=$(scaled 2)
start=$(date +%s)
while :; do
    [ -f never ] && exit 0
    [ $(( $(date +%s) - start )) -ge "$bound" ] && break
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
esac
exec sleep 30
Q
chmod +x "$WORK/bin/qemu-system-i386"
: > "$WORK/machines"

# The rows below judge what the runner does with one measurement, so the
# second measurement it would otherwise take is turned off; one row turns it
# back on and judges that instead.
run() { ( cd "$WORK" && PATH="$WORK/bin:$PATH" RACH2_WORKERS="${W:-2}" \
          RACH2_CALIBRATION_RETRY=0 sh tests/run "$@" 2>&1 ); }

out=$(run passing); rc=$?
check "a passing suite exits zero" 0 "$rc"
check "and says so" 1 "$(printf '%s\n' "$out" | grep -c 'run: passing: PASS')"

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
check "and is named as the owner" 1 "$(printf '%s\n' "$out" | grep -c 'run: leaky: left')"

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
check "after the components" components "$(head -1 "$WORK/order.log")"
check "in a make of their own" 1 \
    "$([ "$(cat "$WORK/components.count")" != "$(cat "$WORK/prepared.count")" ] && echo 1 || echo 0)"
rm -f "$WORK/prepared.count" "$WORK/components.count" "$WORK/order.log" "$WORK/ran.pid"
out=$(run passing)
check "a suite run alone does not prepare the whole product" 0 \
    "$(cat "$WORK/prepared.count" 2>/dev/null | wc -l | tr -d ' ')"
check "but builds the components first" components "$(head -1 "$WORK/order.log")"
check "in a make of their own" 1 \
    "$([ "$(cat "$WORK/components.count")" != "$(cat "$WORK/ran.pid")" ] && echo 1 || echo 0)"
check "and still runs" 1 "$(printf '%s\n' "$out" | grep -c 'passing ran')"

# The worker count is a limit, not a suggestion. Two suites that each take two
# seconds take four one at a time and two together, so the wall clock says
# whether the pool honoured it.
t0=$(date +%s); out=$(W=1 run slow1 slow2); one=$(($(date +%s) - t0))
t0=$(date +%s); out=$(W=2 run slow1 slow2); two=$(($(date +%s) - t0))
check "one worker runs two suites one after the other" 1 \
    "$([ "$one" -ge 4 ] && echo 1 || echo 0)"
check "two workers run them together" 1 \
    "$([ "$two" -lt 4 ] && echo 1 || echo 0)"


# The deadlines a set scales. The calibration fixture gives the pool's
# factor: with computing at twice the reference the factor is 2.0 times the
# margin, 3.0, and the slow token's one-second bound becomes three; with
# the host faster than the reference the factor is the margin alone, 1.5,
# and the token comes too late. A suite run by itself is told nothing,
# whatever the environment carries. A token that never comes ends at the
# scaled bound and not later.
echo 2.4 > "$WORK/calib.compute"; echo 0.5 > "$WORK/calib.storage"
rm -f "$WORK/calibrations.log" "$WORK/entered.log"
out=$(run slowtoken passing); rc=$?
check "a set is calibrated at its width" 1 "$(grep -c 'width 2' "$WORK/calibrations.log")"
check "and not at width 1 when no suite runs alone after the pool" 0 "$(grep -c 'width 1' "$WORK/calibrations.log")"
check "and prints the factor it measured" 1 "$(printf '%s\n' "$out" | grep -c 'run: calibration: width 2, computing 2.4 s against 1.2 (2.00), storage 0.5 s against 2.5 (1.00), margin 1.5, factor 3.0')"
check "a suite whose token comes within the scaled bound passes" 0 "$rc"
check "with the bound scaled to six" 1 "$(printf '%s\n' "$out" | grep -c 'slowtoken ran, bound 6')"
rm -f "$WORK/calibrations.log"
out=$(run slowtoken passing install); rc=$?
check "a suite that runs alone after the pool has the set calibrated once more at width 1" 1 "$(grep -c 'width 1' "$WORK/calibrations.log")"
check "and gets that factor" 1 "$(printf '%s\n' "$out" | grep -c 'run: install: PASS')"
echo 0.5 > "$WORK/calib.compute"
out=$(run slowtoken passing); rc=$?
check "a host faster than the reference gets the margin alone" 1 "$(printf '%s\n' "$out" | grep -c 'factor 1.5')"
check "under which the slow token is late" 1 "$rc"
check "by its own bound" 1 "$(printf '%s\n' "$out" | grep -c 'slowtoken: no token within 3 s')"
out=$( (export RACH2_DEADLINES=scaled RACH2_DEADLINE_FACTOR=9.0; run slowtoken) ); rc=$?
check "a suite run by itself keeps its bound whatever the environment says" 1 "$rc"
check "as written" 1 "$(printf '%s\n' "$out" | grep -c 'no token within 2 s')"
out=$( (export RACH2_CALIBRATION_FACTOR=abc; run slowtoken passing) ); rc=$?
check "a factor given by hand in the wrong form is refused" 1 "$(printf '%s\n' "$out" | grep -c 'must be a decimal')"
out=$( (export RACH2_CALIBRATION_FACTOR=08.0; run slowtoken passing) ); rc=$?
check "and one with a leading zero is read as a decimal, not as octal" 1 "$(printf '%s\n' "$out" | grep -c 'factor 8.0 given')"
out=$( (export RACH2_CALIBRATION_FACTOR=100; run slowtoken passing) ); rc=$?
check "and one past the envelope is refused" 1 "$(printf '%s\n' "$out" | grep -c 'past the envelope')"

# The second measurement, which an ordinary run takes: a host busy for a minute
# is not a host that cannot run the set, and refusing it costs everything the
# set would have told. A rate that is past the envelope both times is still
# refused, and the wait is named.
echo 100 > "$WORK/calib.compute"
out=$( (cd "$WORK" && PATH="$WORK/bin:$PATH" RACH2_WORKERS=2 \
        RACH2_CALIBRATION_RETRY_WAIT=1 sh tests/run slowtoken passing 2>&1) ); rc=$?
check "a rate past the envelope is measured again" 1 "$(printf '%s\n' "$out" | grep -c 'measuring again in 1 s')"
check "and the set is refused when the second says the same" 1 "$rc"
check "with the rate named by each measurement" 2 "$(printf '%s\n' "$out" | grep -c 'past the envelope')"

# And a host that is steadily too slow for the width it was given is not a host
# that cannot run the set: the pool narrows until the measurement fits. The
# fixture reports a rate that fits only at two or fewer, so the set runs there
# and says so.
echo 100 > "$WORK/calib.compute"
echo "2.4 2" > "$WORK/calib.fitat"
out=$( (cd "$WORK" && PATH="$WORK/bin:$PATH" RACH2_WORKERS=8 \
        RACH2_CALIBRATION_RETRY=0 RACH2_CALIBRATION_FLOOR=1 \
        sh tests/run slow1 slow2 passing 2>&1) ); rc=$?
check "a host too slow for the width narrows the pool" 1 "$(printf '%s\n' "$out" | grep -c 'narrowing the pool to')"
check "and the set then runs" 0 "$rc"
rm -f "$WORK/calib.fitat"
echo 2.4 > "$WORK/calib.compute"
# The bound that ends a suite whose token never comes: the set is run under
# the tree's own watchdog when it is built, so a bound that never ended
# would be cut and named rather than hang this test.
WATCH=""
[ -x "$REPO/build/tests/system/deadline" ] && WATCH="$REPO/build/tests/system/deadline 40"
t0=$(date +%s); out=$($WATCH sh -c 'cd "$1" && PATH="$1/bin:$PATH" RACH2_WORKERS=2 sh tests/run notoken passing 2>&1' sh "$WORK"); rc=$?; took=$(($(date +%s) - t0))
check "a token that never comes fails at the scaled bound" 1 "$rc"
check "which it names" 1 "$(printf '%s\n' "$out" | grep -c 'notoken: no token within 6 s')"
check "and which ended the suite" 1 "$([ "$took" -lt 30 ] && echo 1 || echo 0)"
echo 100 > "$WORK/calib.compute"
rm -f "$WORK/entered.log"
out=$(run slowtoken passing); rc=$?
check "a rate past the envelope fails the set by name" 1 "$(printf '%s\n' "$out" | grep -c 'past the envelope')"
check "with a failing status" 1 "$([ "$rc" -ne 0 ] && echo 1 || echo 0)"
check "before any suite starts" 0 "$(cat "$WORK/entered.log" 2>/dev/null | wc -l | tr -d ' ')"
echo 0.5 > "$WORK/calib.compute"

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
    t0=$(date +%s); (unset RACH2_DEADLINES RACH2_DEADLINE_FACTOR; "$HOSTPEER_BIN" echo 47123 1 > /dev/null 2>&1); rc=$?; held=$(($(date +%s) - t0))
    check "hostpeer as written holds its port for a second" 1 "$([ "$rc" -eq 0 ] && [ "$held" -le 2 ] && echo 1 || echo 0)"
    t0=$(date +%s); (export RACH2_DEADLINES=scaled RACH2_DEADLINE_FACTOR=4.0; "$HOSTPEER_BIN" echo 47123 1 > /dev/null 2>&1); rc=$?; held=$(($(date +%s) - t0))
    check "and under the factor for four" 1 "$([ "$rc" -eq 0 ] && [ "$held" -ge 4 ] && [ "$held" -le 8 ] && echo 1 || echo 0)"
else
    echo "  SKIP: the harness, ptydrive and hostpeer are not built; their factor rows did not run"
    fails=$((fails + 1))
fi

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
perl -e 'setpgrp(0, 0); exec "sleep", "20"' &
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
printf 'pid %s\npgid %s\nwork %s\nsince now\nargs holding\n' "$$" "$$" "$WORK" \
    > "$WORK/build/.run.lock/owner"
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
echo "# the suites' makefile, as the runner sees it" > "$WORK/tests/system/suites.mk"
rm -f "$WORK/build/thing"
out=$(run rewriter); rc=$?
check "a run whose product changed under it fails" 1 "$rc"
check "and names the product" 1 "$(printf '%s\n' "$out" | grep -c 'voided: build/thing changed during the run')"

# With RACH2_STOP_ON_FAIL, the first failure stops the pool: the suite still
# running is stopped and named, and the run ends well before it would have.
rm -f "$WORK/slow1.done"
t0=$(date +%s)
out=$(cd "$WORK" && PATH="$WORK/bin:$PATH" RACH2_WORKERS=2 RACH2_STOP_ON_FAIL=1 sh tests/run failing slow1 slow2 2>&1); rc=$?
check "a set stopped on its first failure fails" 1 "$rc"
check "the running suite is stopped and named" 1 "$(printf '%s\n' "$out" | grep -c 'stopped part way: slow1')"
check "the queued suite is named" 1 "$(printf '%s\n' "$out" | grep -c 'not started: slow2')"
check "and the run ended early" 1 "$([ $(($(date +%s) - t0)) -lt 4 ] && echo 1 || echo 0)"
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

# A product two pooled suites name is built once, by the preparation, before
# either starts.
rm -f "$WORK/product" "$WORK/product.count" "$WORK/after-product"
out=$(run needsprod1 needsprod2); rc=$?
check "two suites that name one product pass" 0 "$rc"
check "the product was built once" 1 "$(wc -l < "$WORK/product.count" | tr -d ' ')"
check "and both ran after it" 2 "$(wc -l < "$WORK/after-product" | tr -d ' ')"
echo "selftest: $((total - fails)) of $total checks passed"
[ "$fails" -eq 0 ] || exit 1
