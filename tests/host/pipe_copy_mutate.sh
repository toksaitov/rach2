#!/bin/sh
# Compile pipe copies before, during and after each scheduling mutation, and
# each mutation of the kind of change a transfer or a close wakes the other
# end for.
set -eu

repo=$(CDPATH='' cd -- "$(dirname -- "$0")/../.." && pwd)
cd "$repo"
mkdir -p build/tests
work=$(mktemp -d "$repo/build/tests/pipe-copy-proof.XXXXXX")
child=
cp -R rach2/sys "$work/sys"
cp "$work/sys/ipc/pipe.c" "$work/pipe.saved"

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
for fault in read write read-kind write-kind close-kind; do
    python3 - "$work/sys/ipc/pipe.c" "$fault" <<'PY'
import pathlib
import sys

path = pathlib.Path(sys.argv[1])
source = path.read_text()
calls = {
    "read": ("!vm_prefault_range(addr, page_piece(addr, to_read - done), 1)", "0"),
    "write": ("!vm_prefault_range(addr, page_piece(addr, count - done), 0)", "0"),
    "read-kind": ("pipe_wake(p, &p->write_wq, WQ_ROOM);", "pipe_wake(p, &p->write_wq, WQ_ALL);"),
    "write-kind": ("pipe_wake(p, &p->read_wq, WQ_DATA);", "pipe_wake(p, &p->read_wq, WQ_ALL);"),
    "close-kind": ("pipe_wake(p, &p->read_wq, WQ_ALL);", "pipe_wake(p, &p->read_wq, WQ_DATA);"),
}
old, new = calls[sys.argv[2]]
if source.count(old) != 1:
    raise SystemExit("pipe-copy-mutation: source pattern is not unique")
path.write_text(source.replace(old, new))
PY
    case $fault in
        read) message='reader returns remaining bytes' ;;
        write) message='writer preserves peer bytes' ;;
        read-kind) message='a read wakes the writers for room alone' ;;
        write-kind) message='a write wakes the readers for data alone' ;;
        close-kind) message='a close wakes the readers for a hangup' ;;
    esac
    run_phase "$fault-mutated" 1 "$message"
    cleanup
    run_phase "$fault-restored" 0 'pipe-copy-hosttest: result: pass:'
    cmp "$work/pipe.saved" "$work/sys/ipc/pipe.c"
done
