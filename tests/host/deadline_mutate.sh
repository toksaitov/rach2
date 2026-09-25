#!/bin/sh
# Prove the host rows on wait deadlines with faults in a private kernel tree.
#
# Usage: sh tests/host/deadline_mutate.sh
#
# wait_queue_hosttest requires a wait that a wake does not end to arm the
# caller's deadline again before it blocks again, and poll_deadline_hosttest
# requires a poll or ppoll timeout of three ticks from tick 1000 to end at
# 1004 and the longest ppoll timeout to end ahead of the tick at the wrap
# (entries 216 and 217). Each test is compiled against a private copy of
# rach2/sys as it is, where it must pass; with the deadline not armed again,
# the tick under way not added, or the ppoll clamp a tick higher, where it
# must fail its row; and restored, touched and compiled again, where it must
# pass. The trap stops the test before it restores the source.
set -eu
repo=$(CDPATH='' cd -- "$(dirname -- "$0")/../.." && pwd)
cd "$repo"
mkdir -p build
work=$(mktemp -d "$repo/build/deadline-proof.XXXXXX")
mkdir "$work/sys"
cp -R rach2/sys/. "$work/sys/"
cp "$work/sys/proc/wait_queue.c" "$work/wait_queue.saved"
cp "$work/sys/io/poll.c" "$work/poll.saved"
child=

restore()
{
    cp "$work/wait_queue.saved" "$work/sys/proc/wait_queue.c"
    cp "$work/poll.saved" "$work/sys/io/poll.c"
    touch "$work/sys/proc/wait_queue.c" "$work/sys/io/poll.c"
}

cleanup()
{
    if [ -n "$child" ]; then
        kill -TERM "$child" 2>/dev/null || :
        wait "$child" 2>/dev/null || :
        child=
    fi
    restore
}
trap cleanup EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM

# run_phase PHASE TEST EXPECTED DETAIL: compile tests/host/TEST.c against the
# private tree and run it; a pass must exit 0, a fail must exit nonzero and
# print DETAIL.
run_phase()
{
    phase=$1
    test=$2
    expected=$3
    detail=$4
    binary="$work/$phase"
    if ! "${CC:-cc}" -std=c99 -D_DEFAULT_SOURCE -D_FORTIFY_SOURCE=0 -DRACH2_HOSTTEST \
        -fsanitize=address,undefined -fno-sanitize-recover=all -g -O1 -Wno-empty-body \
        -ffreestanding -Wno-int-to-pointer-cast -Wno-pointer-to-int-cast \
        -I "$work/sys" -I "$work/sys/lib" -I "$work/sys/proc" \
        "tests/host/$test.c" -o "$binary" >"$binary.compile.log" 2>&1; then
        cat "$binary.compile.log" >&2
        exit 1
    fi
    cksum "$work/sys/proc/wait_queue.c" "$work/sys/io/poll.c" "$binary" >"$binary.artifacts"
    perl -e 'alarm 10; exec @ARGV or die "deadline-mutation: exec: $!\n"' \
        "$binary" >"$binary.log" 2>&1 &
    child=$!
    status=0
    wait "$child" || status=$?
    child=
    cat "$binary.log"
    if [ "$expected" = pass ]; then
        [ "$status" -eq 0 ] || exit 1
    else
        [ "$status" -ne 0 ] || {
            echo "deadline-mutation: the test passed with its fault: $phase" >&2; exit 1;
        }
        grep -F "$detail" "$binary.log" >/dev/null || exit 1
    fi
    echo "deadline-mutation: result: $phase status $status"
}

# fault FILE OLD NEW: replace the one line OLD of the private FILE with NEW.
fault()
{
    python3 - "$work/sys/$1" "$2" "$3" <<'PYTHON'
from pathlib import Path
import sys

path = Path(sys.argv[1])
text = path.read_text()
old, new = sys.argv[2] + '\n', sys.argv[3] + '\n'
if text.count(old) != 1:
    raise SystemExit('deadline-mutation: the line to change is not unique: ' + sys.argv[2])
path.write_text(text.replace(old, new))
PYTHON
}

echo "deadline-mutation: logs: $work"
run_phase wait-fixed wait_queue_hosttest pass ''
run_phase poll-fixed poll_deadline_hosttest pass ''

fault proc/wait_queue.c '        current_proc->wakeup_tick = deadline;' ''
run_phase wait-unarmed wait_queue_hosttest fail 'deadline_at_block[1] == 77'
restore
run_phase wait-restored wait_queue_hosttest pass ''

fault io/poll.c '    return kernel_ticks + ticks + (ticks != 0 ? 1u : 0u);' \
    '    return kernel_ticks + ticks;'
run_phase poll-early poll_deadline_hosttest fail 'a poll of 30 ms from tick 1000 ends at 1004'
restore
run_phase poll-restored poll_deadline_hosttest pass ''

fault io/poll.c '    ticks = sec * 100u + nsec_ticks;
    if (ticks > 0x7FFFFFFEu) {
        ticks = 0x7FFFFFFEu;
    }' '    ticks = sec * 100u + nsec_ticks;
    if (ticks > 0x7FFFFFFFu) {
        ticks = 0x7FFFFFFFu;
    }'
run_phase poll-clamp poll_deadline_hosttest fail 'the longest ppoll timeout ends ahead of the tick across the wrap'
restore
run_phase poll-clamp-restored poll_deadline_hosttest pass ''

cmp "$work/wait_queue.saved" "$work/sys/proc/wait_queue.c"
cmp "$work/poll.saved" "$work/sys/io/poll.c"
echo 'deadline-mutation: result: pass: each fault failed its row; the restored sources passed'
