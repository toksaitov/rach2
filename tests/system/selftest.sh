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
mkdir -p "$WORK/tests/system"
cp "$REPO/tests/run" "$WORK/tests/run"
cat > "$WORK/tests/registry" <<'REG'
# name|tools|tier
passing|-|core
failing|-|full
leaky|-|full
needy|a-command-that-does-not-exist|full
slow1|-|full
slow2|-|full
REG
cat > "$WORK/Makefile" <<'MK'
prepare-all:
	@echo prepared >> prepared.count
run-passing:
	@echo "passing ran"
run-failing:
	@echo "failing ran"; exit 1
run-leaky:
	@qemu-system-i386 --selftest-hold & \
	 i=0; while [ ! -s "$$RACH2_QEMU_LEDGER" ] && [ $$i -lt 100 ]; do sleep 0.1; i=$$((i+1)); done
run-needy:
	@echo "needy ran"
run-slow1:
	@sleep 2
run-slow2:
	@sleep 2
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
# object twenty times, or race over it.
rm -f "$WORK/prepared.count"
out=$(run passing failing)
check "a run of two suites prepares once" 1 \
    "$(wc -l < "$WORK/prepared.count" | tr -d ' ')"

# The worker count is a limit, not a suggestion. Two suites that each take two
# seconds take four one at a time and two together, so the wall clock says
# whether the pool honoured it.
t0=$(date +%s); out=$(W=1 run slow1 slow2); one=$(($(date +%s) - t0))
t0=$(date +%s); out=$(W=2 run slow1 slow2); two=$(($(date +%s) - t0))
check "one worker runs two suites one after the other" 1 \
    "$([ "$one" -ge 4 ] && echo 1 || echo 0)"
check "two workers run them together" 1 \
    "$([ "$two" -lt 4 ] && echo 1 || echo 0)"

echo "selftest: $((total - fails)) of $total checks passed"
[ "$fails" -eq 0 ] || exit 1
