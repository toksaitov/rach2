#!/bin/sh
# etude_remote_mutate.sh: prove etude_remote_hosttest.c with faults in a
# private copy of etude's remote execution.
#
# Each fault goes into a copy of the sources under a directory of the
# script's own; the test is built from the copy and must fail at the check
# named for the fault, and the copy restored must pass again. The tree's
# sources are not changed. The faults: the responder acts again on a repeated
# command (replay), takes a reused command number with other fields as a new
# command (conflict), stores a file without checking its digest (seal), and
# takes frames out of order (order); the controller takes an attempt as
# complete at its exit alone (truncated), takes output without checking its
# digest (output), and takes a ready of a version it did not offer (version).
#
# Usage: sh tests/host/etude_remote_mutate.sh [--texts]
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
    'replay': ('    if (found == 1 && !fresh) {', '    if (found == 1 && !fresh && 0) {'),
    'conflict': ('    if (found == 2 || found < 0) {', '    if (found < 0) {'),
    'seal': ('        if (s->put.got != s->put.size || strcmp(hex, s->put.sha) != 0) {',
             '        if (s->put.got != s->put.size) {'),
    'order': ('    if (f->seq != (s->has_ack ? s->seq_in + 1 : 0))', '    if (0)'),
    'truncated': ('    return c->have_exit && c->have_stream[0] && c->have_stream[1] && c->have_usage &&\n'
                  '           c->have_cleaned;',
                  '    return c->have_exit;'),
    'output': ('        if (o->kept != c->sent[fd] || strcmp(hex, c->sha[fd]) != 0)',
               '        if (o->kept != c->sent[fd])'),
    'version': ('    if (strcmp(f.field[1], "1") != 0) {', '    if (0) {'),
}
old, new = faults[sys.argv[2]]
if source.count(old) != 1:
    raise SystemExit('etude-remote-mutation: the text of fault %s is not in %s once'
                     % (sys.argv[2], path))
if not os.environ.get('RACH2_FAULT_TEXTS'):
    path.write_text(source.replace(old, new))
PY
}

# The faults: NAME FILE CHECK, CHECK the text of the check that must fail.
faults='replay|tools/etude/posix/serve.c|a repeated run is answered by the same events
conflict|tools/etude/posix/serve.c|a command number reused with other fields is refused
seal|tools/etude/posix/serve.c|content whose digest is not the put'"'"'s is refused
order|tools/etude/posix/serve.c|a gap in the sequence ends the connection
truncated|tools/etude/posix/remote.c|a lost end is read again, of the same process, after a new connection
output|tools/etude/posix/remote.c|damaged output gives no complete run
version|tools/etude/posix/remote.c|an incompatible protocol is refused before anything runs'

if [ "${1:-}" = --texts ]; then
    RACH2_FAULT_TEXTS=1
    export RACH2_FAULT_TEXTS
    printf '%s\n' "$faults" | while IFS='|' read -r name file check; do
        apply_fault "$file" "$name"
    done
    echo 'etude-remote-mutation: texts: each fault found once'
    exit 0
fi
[ "$#" -eq 0 ] || { echo 'usage: sh tests/host/etude_remote_mutate.sh [--texts]' >&2; exit 2; }

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
        echo "etude-remote-mutation: evidence: $work" >&2
    fi
}
trap cleanup EXIT
trap 'stopped=1; exit 129' HUP
trap 'stopped=1; exit 130' INT
trap 'stopped=1; exit 143' TERM
work=$(mktemp -d "${TMPDIR:-/tmp}/rach2-etude-remote-proof.XXXXXX")
mkdir "$work/tree"
tar cf - tools/etude/include tools/etude/posix tools/etude/capture.c tools/etude/capture.h \
    tools/etude/guest/wire.c tools/etude/guest/wire.h tools/etude/json.c tools/etude/json.h \
    utils/sha256.c utils/sha256.h \
    tests/host/etude_remote_hosttest.c tests/host/compat/fixdir_host.c \
    tests/host/compat/fixdir_host.h | (cd "$work/tree" && tar xf -)

# run_phase NAME STATUS: build the test from the copy, run it, and require
# STATUS.
run_phase()
{
    t=tests/host/etude_remote_hosttest.c
    deps=$(grep -oE 'host-deps:[^*]*' "$t" | sed 's/host-deps://' | tr '\n' ' ')
    # shellcheck disable=SC2086 # one path a word
    (cd "$work/tree" && "${CC:-cc}" -std=c99 -D_DEFAULT_SOURCE -D_FORTIFY_SOURCE=0 \
        -DRACH2_HOSTTEST -Wall -Wextra -Werror -g -O1 -fsanitize=address,undefined \
        -fno-sanitize-recover=all -I tools/etude/include -I utils -I tests/host/compat \
        -o "$work/$1" "$t" $deps) >"$work/$1.compile.log" 2>&1 || {
        cat "$work/$1.compile.log" >&2
        exit 1
    }
    status=0
    perl -e 'alarm 300; exec @ARGV or die "etude-remote-mutation: exec: $!\n"' \
        "$work/$1" >"$work/$1.log" 2>&1 &
    child=$!
    wait "$child" || status=$?
    child=
    [ "$status" -eq "$2" ] || {
        cat "$work/$1.log"
        echo "etude-remote-mutation: $1 gave status $status, wanted $2" >&2
        exit 1
    }
    echo "etude-remote-mutation: result: $1 status $status"
}

run_phase fixed 0
printf '%s\n' "$faults" > "$work/faults"
while IFS='|' read -r name file check; do
    cp "$file" "$work/tree/$file"
    apply_fault "$work/tree/$file" "$name"
    run_phase "$name" 1
    grep -qF "fail: tests/host/etude_remote_hosttest.c:" "$work/$name.log"
    grep -F ": $check: " "$work/$name.log" > /dev/null || {
        cat "$work/$name.log"
        echo "etude-remote-mutation: fault $name did not fail: $check" >&2
        exit 1
    }
    cp "$file" "$work/tree/$file"
done < "$work/faults"
run_phase restored 0
echo 'etude-remote-mutation: result: pass: seven faults caught and the copy restored'
