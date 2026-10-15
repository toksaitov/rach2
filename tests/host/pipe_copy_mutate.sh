#!/bin/sh
# Prove pipe_copy_hosttest.c with private mutations of rach2/sys/ipc/pipe.c: a
# read or a write that does not fault the user's pages in before it copies
# (read, write); a transfer or a close that wakes the other end for the wrong
# kind of change (read-kind, write-kind, close-kind); and a write that wakes
# the readers only when the ring was empty (write-empty-only).
set -eu

repo=$(CDPATH='' cd -- "$(dirname -- "$0")/../.." && pwd)
cd "$repo"

# apply_fault FILE FAULT: put FAULT into FILE, a copy of pipe.c; with
# RACH2_FAULT_TEXTS set, only check its text and write nothing.
apply_fault()
{
    python3 - "$1" "$2" <<'PY'
import pathlib
import sys
import os
if os.environ.get('RACH2_FAULT_TEXTS'):
    pathlib.Path.write_text = lambda self, *args, **kwargs: 0

path = pathlib.Path(sys.argv[1])
source = path.read_text()
calls = {
    "read": ("!vm_prefault_range(addr, page_piece(addr, to_read - done), 1)", "0"),
    "write": ("!vm_prefault_range(addr, page_piece(addr, count - done), 0)", "0"),
    "read-kind": ("pipe_wake(p, &p->write_wq, WQ_ROOM);", "pipe_wake(p, &p->write_wq, WQ_ALL);"),
    "write-kind": ("pipe_wake(p, &p->read_wq, WQ_DATA);", "pipe_wake(p, &p->read_wq, WQ_ALL);"),
    "close-kind": ("pipe_wake(p, &p->read_wq, WQ_ALL);", "pipe_wake(p, &p->read_wq, WQ_DATA);"),
    "write-empty-only": ("pipe_wake(p, &p->read_wq, WQ_DATA);",
                         "if (pipe_available(p) == chunk) {\n"
                         "                pipe_wake(p, &p->read_wq, WQ_DATA);\n"
                         "            }"),
}
old, new = calls[sys.argv[2]]
if source.count(old) != 1:
    raise SystemExit("pipe-copy-mutation: source pattern is not unique")
path.write_text(source.replace(old, new))
PY
}

faults='read write read-kind write-kind close-kind write-empty-only'
if [ "${1:-}" = --texts ]; then
    for fault in $faults; do
        RACH2_FAULT_TEXTS=1 apply_fault rach2/sys/ipc/pipe.c "$fault" >/dev/null || exit 1
    done
    echo 'pipe-copy-mutation: texts: every fault is once in its source'
    exit 0
fi

. "$repo/tests/system/runlock.sh"
runlock_hold "$repo" pipe-copy-mutation "$repo/tests/host/pipe_copy_mutate.sh" "$@"

mkdir -p build/tests
work=$(mktemp -d "$repo/build/tests/pipe-copy-proof.XXXXXX")
child=
cp -R rach2/sys "$work/sys"
cp "$work/sys/ipc/pipe.c" "$work/pipe.saved"

# cleanup: stop the running test and put the saved pipe.c back; the loop calls
# it after each fault, and it is the exit trap.
cleanup()
{
    if [ -n "$child" ]; then
        kill -TERM "$child" 2>/dev/null || :
        wait "$child" 2>/dev/null || :
        child=
    fi
    cp "$work/pipe.saved" "$work/sys/ipc/pipe.c"
    touch "$work/sys/ipc/pipe.c"
}
trap cleanup EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM

# run_phase PHASE STATUS MESSAGE: compile pipe_copy_hosttest.c over the copy
# as $work/PHASE, run it for at most 10 seconds, and exit unless it ends with
# STATUS and its log contains MESSAGE.
run_phase()
{
    phase=$1
    expected=$2
    message=$3
    binary="$work/$phase"
    if ! "${CC:-cc}" -std=c99 -D_DEFAULT_SOURCE -D_FORTIFY_SOURCE=0 \
        -DRACH2_HOSTTEST -g -O1 -ffreestanding -Wall -Wextra -Werror \
        -Wno-pointer-to-int-cast -Wno-int-to-pointer-cast \
        -fsanitize=address,undefined -fno-sanitize-recover=all \
        -I "$work/sys" -I "$work/sys/lib" -I "$work/sys/proc" \
        tests/host/pipe_copy_hosttest.c -o "$binary" >"$binary.compile.log" 2>&1; then
        cat "$binary.compile.log" >&2
        exit 1
    fi
    cksum "$work/sys/ipc/pipe.c" "$binary" >"$binary.cksum"
    perl -e 'alarm 10; exec @ARGV or die "pipe-copy-mutation: exec: $!\n"' \
        "$binary" >"$binary.log" 2>&1 &
    child=$!
    status=0
    wait "$child" || status=$?
    child=
    cat "$binary.log"
    [ "$status" -eq "$expected" ] && grep -Fq "$message" "$binary.log" || exit 1
    echo "pipe-copy-mutation: result: $phase status $status"
}

echo "pipe-copy-mutation: logs: $work"
run_phase fixed 0 'pipe-copy-hosttest: result: pass:'
for fault in $faults; do
    apply_fault "$work/sys/ipc/pipe.c" "$fault"
    case $fault in
        read) message='reader returns remaining bytes' ;;
        write) message='writer preserves peer bytes' ;;
        read-kind) message='a read wakes the writers for room alone' ;;
        write-kind) message='a write wakes the readers for data alone' ;;
        close-kind) message='a close wakes the readers for a hangup' ;;
        write-empty-only) message='a write into a ring that has data wakes the readers for data' ;;
    esac
    run_phase "$fault-mutated" 1 "$message"
    cleanup
    run_phase "$fault-restored" 0 'pipe-copy-hosttest: result: pass:'
    cmp "$work/pipe.saved" "$work/sys/ipc/pipe.c"
done
