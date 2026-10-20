#!/bin/sh
# etude_serial_mutate.sh: prove etude_serial_hosttest.c with faults in a
# private copy of tools/etude/serial.c, the controller of etude-guest on a
# serial console.
#
# Each fault goes into a copy of the sources under a directory of the
# script's own; the test is built from the copy and must fail at the check
# named for the fault, and the copy restored must pass again. The tree's
# sources are not changed. The faults: no frame is sent again (resend), no
# frame is acknowledged (ack), a repeated frame is acted on again (repeat), a
# frame of another session is acted on (session), a closed console after an
# exit is taken for the end (closed), a stored digest is not checked
# (stored), the digest a stream frame gives is not checked against the output
# (output), and an etude-guest started again is not noticed (restart).
#
# Usage: sh tests/host/etude_serial_mutate.sh [--texts]
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
    'resend': ('    if (s->closed || s->nsent == 0 || now_us < s->q[0].sent_us + s->conf.resend_us)\n',
               '    if (1)\n'),
    'ack': ('    if (s->need_ack && s->live)\n', '    if (0)\n'),
    'repeat': ('    if (f.seq != s->recv_next)\n', '    if (f.seq > s->recv_next)\n'),
    'session': (' || strcmp(f.session, s->session) != 0 ||', ' ||'),
    'closed': ('    if (s->op == OP_RUN && !(s->have_exit && s->have_usage && s->have_cleaned)) {\n',
               '    if (s->op == OP_RUN && !s->have_exit) {\n'),
    'stored': ('    if (strcmp(f->field[2], s->sha) != 0) {\n', '    if (0) {\n'),
    'output': ('    if (s->sent[fd] != s->got[fd] || strcmp(hex, s->digest[fd]) != 0)\n',
               '    if (s->sent[fd] != s->got[fd])\n'),
    'restart': ('    if (!same && s->live) {\n', '    if (0) {\n'),
}
old, new = faults[sys.argv[2]]
if source.count(old) != 1:
    raise SystemExit('etude-serial-mutation: the text of fault %s is not in %s once'
                     % (sys.argv[2], path))
if not os.environ.get('RACH2_FAULT_TEXTS'):
    path.write_text(source.replace(old, new))
PY
}

# The faults: NAME|CHECK, CHECK the text of the check that must fail.
faults='resend|an unacknowledged frame is sent again with the same SEQ and bytes
ack|every sequenced frame is acknowledged
repeat|a repeated frame is not acted on again
session|a frame of another session is not acted on
closed|a closed console leaves the run'"'"'s end lost
stored|a stored with another digest is damaged
output|output that disagrees with its stream'"'"'s digest is damaged
restart|an etude-guest started again loses the run'

if [ "${1:-}" = --texts ]; then
    RACH2_FAULT_TEXTS=1
    export RACH2_FAULT_TEXTS
    printf '%s\n' "$faults" | while IFS='|' read -r name check; do
        apply_fault tools/etude/serial.c "$name"
    done
    echo 'etude-serial-mutation: texts: each fault found once'
    exit 0
fi
[ "$#" -eq 0 ] || { echo 'usage: sh tests/host/etude_serial_mutate.sh [--texts]' >&2; exit 2; }

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
        echo "etude-serial-mutation: evidence: $work" >&2
    fi
}
trap cleanup EXIT
trap 'stopped=1; exit 129' HUP
trap 'stopped=1; exit 130' INT
trap 'stopped=1; exit 143' TERM
work=$(mktemp -d "${TMPDIR:-/tmp}/rach2-etude-serial-proof.XXXXXX")
mkdir "$work/tree" "$work/tmp"
t=tests/host/etude_serial_hosttest.c
tar cf - tools/etude/include tools/etude/guest/guest.c tools/etude/guest/fixture.c \
    tools/etude/guest/fixture.h tools/etude/guest/wire.c tools/etude/guest/wire.h \
    tools/etude/serial.c tools/etude/serial.h tools/etude/capture.c \
    tools/etude/capture.h tools/etude/history_json.c tools/etude/history_json.h \
    utils/sha256.c utils/sha256.h "$t" | (cd "$work/tree" && tar xf -)

# run_phase NAME STATUS: build the test from the copy as tests/run host
# builds it, run it, and require STATUS.
run_phase()
{
    deps=$(grep -oE 'host-deps:[^*]*' "$t" | sed 's/host-deps://' | tr '\n' ' ')
    extra=$(grep -oE 'host-cflags:[^*]*' "$t" | sed 's/host-cflags://' | tr '\n' ' ')
    # shellcheck disable=SC2086 # one path or flag a word
    (cd "$work/tree" && "${CC:-cc}" -std=c99 -D_DEFAULT_SOURCE -D_FORTIFY_SOURCE=0 \
        -DRACH2_HOSTTEST -fsanitize=address,undefined -fno-sanitize-recover=all -g -O1 \
        -ffreestanding $extra -o "$work/$1" "$t" $deps) >"$work/$1.compile.log" 2>&1 || {
        cat "$work/$1.compile.log" >&2
        exit 1
    }
    status=0
    TMPDIR=$work/tmp perl -e 'alarm 300; exec @ARGV or die "etude-serial-mutation: exec: $!\n"' \
        "$work/$1" >"$work/$1.log" 2>&1 &
    child=$!
    wait "$child" || status=$?
    child=
    [ "$status" -eq "$2" ] || {
        cat "$work/$1.log"
        echo "etude-serial-mutation: $1 gave status $status, wanted $2" >&2
        exit 1
    }
    echo "etude-serial-mutation: result: $1 status $status"
}

run_phase fixed 0
printf '%s\n' "$faults" > "$work/faults"
while IFS='|' read -r name check; do
    cp tools/etude/serial.c "$work/tree/tools/etude/serial.c"
    apply_fault "$work/tree/tools/etude/serial.c" "$name"
    run_phase "$name" 1
    grep -F "fail: $t:" "$work/$name.log" | grep -F ": $check: " > /dev/null || {
        cat "$work/$name.log"
        echo "etude-serial-mutation: fault $name did not fail: $check" >&2
        exit 1
    }
    echo "etude-serial-mutation: result: $name failed: $check"
done < "$work/faults"
cp tools/etude/serial.c "$work/tree/tools/etude/serial.c"
run_phase restored 0
echo 'etude-serial-mutation: result: pass: eight faults caught and the copy restored'
