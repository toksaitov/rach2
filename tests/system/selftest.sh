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
cp "$REPO/tools/setup/hostcheck.c" "$WORK/tools/setup/hostcheck.c"
cat > "$WORK/tests/registry" <<'REG'
# name|tools|tier
passing|-|core
failing|-|full
leaky|-|full
needy|a-command-that-does-not-exist|full
imagey|hdiutil|full
slow1|-|full
slow2|-|full
stuck|-|full
editing|-|full
needsprod1|-|full
needsprod2|-|full
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
run-needsprod1: product
	@echo "needsprod1 ran"; [ -f product ] && echo needsprod1 >> after-product
run-needsprod2: product
	@echo "needsprod2 ran"; [ -f product ] && echo needsprod2 >> after-product
MK
# A stand-in for QEMU that outlives its suite, so the leak check has something
# to find. It is on PATH ahead of any real one, and it records its pid where
# this script can stop it afterwards. It must become the waiting process rather
# than fork one, because the runner blames the pid it saw, which is the pid the
# shim exec'd into.
mkdir -p "$WORK/bin"
cat > "$WORK/bin/qemu-system-i386" <<'Q'
#!/bin/sh
echo $$ >> "$(dirname "$0")/../machines"
exec sleep 30
Q
chmod +x "$WORK/bin/qemu-system-i386"
: > "$WORK/machines"

run() { ( cd "$WORK" && PATH="$WORK/bin:$PATH" RACH2_WORKERS="${W:-2}" sh tests/run "$@" 2>&1 ); }

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

# A result is bound to what made it: a suite that changes a hashed script
# under the run voids the run, by the file's name.
out=$(run editing); rc=$?
check "a run whose input changed under it fails" 1 "$rc"
check "and names the file" 1 "$(printf '%s\n' "$out" | grep -c 'voided: tests/system/suites.mk changed during the run')"
echo "# the suites' makefile, as the runner sees it" > "$WORK/tests/system/suites.mk"

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
