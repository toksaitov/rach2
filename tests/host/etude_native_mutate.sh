#!/bin/sh
# etude_native_mutate.sh: prove etude_native_hosttest.c with faults in a
# private copy of etude-native, etude-guest and the controller's side of
# remote execution.
#
# Each fault goes into a copy of the sources under a directory of the
# script's own; the test is built from the copy, run in it, and must fail at
# the check named for the fault, and the copy restored must pass again. The
# tree's sources are not changed. The faults: etude-guest gives its alive
# frame over standard I/O no SEQ of the connection's (seq), opens a session
# without its lock (lock), waits for the lock without ending the etude-guest
# that holds it (supersede), settles a killed etude-guest's attempt without
# adding its end to the run's record (settle), and judges the attempt's
# group at once after SIGKILL (immediate); the controller does not say that
# it cancelled the attempt (cancelled); and etude-native takes an attempt
# stopped by a lost connection as stopped at its deadline (reconnect) and
# keeps a passing build's workspace (remove).
#
# Usage: sh tests/host/etude_native_mutate.sh [--texts]
set -eu
repo=$(CDPATH='' cd -- "$(dirname -- "$0")/../.." && pwd)
cd "$repo"

# apply_fault FILE NAME: put fault NAME into FILE, or with RACH2_FAULT_TEXTS
# set only require that its text occurs once.
apply_fault()
{
    python3 - "$1" "$2" <<'PY'
from pathlib import Path
import os
import sys
path = Path(sys.argv[1])
source = path.read_text()
faults = {
    'seq': ('    int counted = !ack && (!alive || opt.stdio);',
            '    int counted = !ack && !alive;'),
    'lock': ('        if (lock_session() < 0) {', '        if (0 && lock_session() < 0) {'),
    'supersede': ('            kill((pid_t)pid, SIGTERM);\n', ''),
    'settle': ('            cmd_append(cmd, p + 1);\n', ''),
    'immediate': ('    gone = child <= 0 || group_ended((pid_t)child, killed ? SETTLE_US : 0);\n',
                  '    gone = child <= 0 || (kill(-(pid_t)child, 0) < 0 && errno == ESRCH);\n'
                  '    (void)killed;\n    (void)group_ended;\n'),
    'cancelled': ('    c->res->cancelled = 1;\n', ''),
    'reconnect': ('        if (res->reconnects > 0) {', '        if (0) {'),
    'remove': ('    if (code == EXIT_PASS && !o->keep) {', '    if (0) {'),
}
old, new = faults[sys.argv[2]]
if source.count(old) != 1:
    raise SystemExit('etude-native-mutation: the text of fault %s is not in %s once'
                     % (sys.argv[2], path))
if not os.environ.get('RACH2_FAULT_TEXTS'):
    path.write_text(source.replace(old, new))
PY
}

# The faults: NAME FILE CHECK, CHECK the text of the check that must fail.
faults='seq|tools/etude/guest/guest.c|a build that passes passes with its output whole
lock|tools/etude/guest/guest.c|a connection held open on the machine'"'"'s side is taken over by the next
supersede|tools/etude/guest/guest.c|a connection held open on the machine'"'"'s side is taken over by the next
settle|tools/etude/guest/guest.c|a build whose etude-guest was killed has its end recorded as lost
immediate|tools/etude/guest/guest.c|the next etude-guest confirms the cleanup once the build'"'"'s processes have ended
cancelled|tools/etude/posix/remote.c|a build cancelled at the controller'"'"'s deadline is cancelled
reconnect|tools/etude/posix/etude_native.c|a build whose connection is cut is reported stopped after a new connection
remove|tools/etude/posix/etude_native.c|a passing build'"'"'s workspace is removed'

if [ "${1:-}" = --texts ]; then
    RACH2_FAULT_TEXTS=1
    export RACH2_FAULT_TEXTS
    printf '%s\n' "$faults" | while IFS='|' read -r name file check; do
        apply_fault "$file" "$name"
    done
    echo 'etude-native-mutation: texts: each fault found once'
    exit 0
fi
[ "$#" -eq 0 ] || { echo 'usage: sh tests/host/etude_native_mutate.sh [--texts]' >&2; exit 2; }

work=
child=
stopped=

# Stop the test, then remove the copy unless a failure needs its evidence.
cleanup()
{
    status=$?
    if [ -n "$child" ]; then
        kill -TERM "$child" 2>/dev/null || :
        wait "$child" 2>/dev/null || :
    fi
    if [ "$status" -eq 0 ] || [ -n "$stopped" ]; then
        [ -z "$work" ] || rm -rf "$work"
    else
        echo "etude-native-mutation: evidence: $work" >&2
    fi
}
trap cleanup EXIT
trap 'stopped=1; exit 129' HUP
trap 'stopped=1; exit 130' INT
trap 'stopped=1; exit 143' TERM
work=$(mktemp -d "${TMPDIR:-/tmp}/rach2-etude-native-proof.XXXXXX")
mkdir "$work/tree" "$work/tmp"
tar cf - tools/etude/include tools/etude/posix tools/etude/guest tools/etude/json.c \
    tools/etude/json.h tools/etude/history_json.c tools/etude/history_json.h \
    utils/sha256.c utils/sha256.h tests/native/tccbuild.sh \
    tests/host/etude_native_hosttest.c tests/host/compat/fixdir_host.c \
    tests/host/compat/fixdir_host.h | (cd "$work/tree" && tar xf -)

# run_phase NAME STATUS: build the test from the copy, run it there, and
# require STATUS.
run_phase()
{
    t=tests/host/etude_native_hosttest.c
    deps=$(grep -oE 'host-deps:[^*]*' "$t" | sed 's/host-deps://' | tr '\n' ' ')
    # shellcheck disable=SC2086 # one path a word
    (cd "$work/tree" && "${CC:-cc}" -std=c99 -D_DEFAULT_SOURCE -D_FORTIFY_SOURCE=0 \
        -DRACH2_HOSTTEST -Wall -Wextra -Werror -g -O1 -fsanitize=address,undefined \
        -fno-sanitize-recover=all -I tools/etude/include -I tools/etude -I tools/etude/guest \
        -I utils -I tests/host/compat -o "$work/$1" "$t" $deps) >"$work/$1.compile.log" 2>&1 || {
        cat "$work/$1.compile.log" >&2
        exit 1
    }
    status=0
    (cd "$work/tree" && TMPDIR="$work/tmp" exec perl -e \
        'alarm 600; exec @ARGV or die "etude-native-mutation: exec: $!\n"' "$work/$1") \
        >"$work/$1.log" 2>&1 &
    child=$!
    wait "$child" || status=$?
    child=
    [ "$status" -eq "$2" ] || {
        cat "$work/$1.log"
        echo "etude-native-mutation: $1 gave status $status, wanted $2" >&2
        exit 1
    }
    echo "etude-native-mutation: result: $1 status $status"
}

run_phase fixed 0
printf '%s\n' "$faults" > "$work/faults"
while IFS='|' read -r name file check; do
    cp "$file" "$work/tree/$file"
    apply_fault "$work/tree/$file" "$name"
    run_phase "$name" 1
    grep -qF "fail: tests/host/etude_native_hosttest.c:" "$work/$name.log"
    grep -F ": $check: " "$work/$name.log" > /dev/null || {
        cat "$work/$name.log"
        echo "etude-native-mutation: fault $name did not fail: $check" >&2
        exit 1
    }
    cp "$file" "$work/tree/$file"
done < "$work/faults"
run_phase restored 0
echo 'etude-native-mutation: result: pass: eight faults caught and the copy restored'
