#!/bin/sh
# Prove request ownership and controlled completion with private source faults.
# Optional arguments select faults; --texts checks every source pattern.
set -eu
repo=$(CDPATH='' cd -- "$(dirname -- "$0")/../.." && pwd)
cd "$repo"

# apply_fault SOURCE FAULT: change the private copy, or only locate its text.
apply_fault()
{
    python3 - "$1" "$2" <<'PY'
from pathlib import Path
import sys

path = Path(sys.argv[1])
faults = {
    'deadline-late': ('if ((int32_t)(req_ticks - r->deadline) < 0)',
                       'if ((int32_t)(req_ticks - r->deadline) <= 0)'),
    'late-error': ('    request->status = status;',
                   '    request->status = request->overdue ? -EIO : status;'),
    'flush-final': ('if (r->overdue < req_periods(r->device))',
                    'if (r->overdue < req_periods(r->device) || (r->flags & BLOCK_REQ_FLUSH))'),
    'flush-error': ('    request->status = status;',
                    '    request->status = request->overdue && (request->flags & BLOCK_REQ_FLUSH) ? -EIO : status;'),
    'pool-deadline': ('r->deadline = req_ticks + req_period(device);',
                      'r->deadline = (waited ? wait_from / 10000u : req_ticks) + req_period(device);'),
    'owner': ('            r->owner = owner;', '            r->owner = device;'),
    'generation': ('!req_is_live(request) || request->generation != generation',
                   '!req_is_live(request) || generation == 0'),
    'early-free': ('    if (!r->held_by_caller && !r->held_by_device) {',
                   '    if (!r->held_by_caller) {'),
    'late-leak': ('        req_let_go(request, 0);', '        (void)request;'),
    'hold': ('    if (!lag_backing || lag_held) {', '    if (!lag_backing) {'),
    'ready': ('if (!e->ready || e->delivered != (duplicate != 0))',
              'if (e->delivered != (duplicate != 0))'),
    'read-copy': ('        k_memcpy(r->page, e->staged, r->count * BLOCK_SECTOR_SIZE);',
                  '        (void)r;'),
}
text = path.read_text()
old, new = faults[sys.argv[2]]
if text.count(old) != 1:
    raise SystemExit('request-mutation: source pattern is not unique: ' + sys.argv[2])
if path.parent.name != 'block':
    path.write_text(text.replace(old, new))
PY
}

faults='owner generation early-free late-leak hold ready read-copy deadline-late late-error flush-final pool-deadline flush-error'
# source_for FAULT: print the component that the fault changes.
source_for()
{
    case $1 in
        hold|ready|read-copy) echo lag.c ;;
        *) echo request.c ;;
    esac
}
if [ "${1:-}" = --texts ]; then
    for fault in $faults; do
        apply_fault "rach2/sys/block/$(source_for "$fault")" "$fault"
    done
    echo 'request-mutation: texts: every fault is once in its source'
    exit 0
fi

. "$repo/tests/system/runlock.sh"
runlock_hold "$repo" request-mutation "$repo/tests/host/request_mutate.sh" "$@"
work=
# cleanup: remove the private sources and binaries on every exit.
cleanup()
{
    [ -z "$work" ] || rm -rf "$work"
}
trap cleanup EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM
mkdir -p build/tests
work=$(mktemp -d "$repo/build/tests/request-proof.XXXXXX")
cp rach2/sys/block/request.c "$work/request.c"
cp rach2/sys/block/lag.c "$work/lag.c"
make build/tests/system/deadline

# run_phase NAME EXPECTED MESSAGE: compile the real request test and its copies.
run_phase()
{
    phase=$1
    expected=$2
    message=$3
    binary="$work/$phase"
    "${CC:-cc}" -std=c99 -D_DEFAULT_SOURCE -D_FORTIFY_SOURCE=0 -DRACH2_HOSTTEST \
        -g -O1 -ffreestanding -Wall -Wextra -Werror \
        -fsanitize=address,undefined -fno-sanitize-recover=all \
        -I rach2/sys/block -I rach2/sys -I rach2/sys/arch/x86 \
        -I rach2/sys/arch/i686 -I rach2/sys/lib -I rach2/sys/proc \
        -I utils -I krffs -I tests/host/compat \
        "-DREQUEST_SOURCE=\"$work/request.c\"" "-DLAG_SOURCE=\"$work/lag.c\"" \
        tests/host/request_hosttest.c rach2/sys/lib/kstring.c rach2/sys/lib/kctypes.c \
        -o "$binary"
    status=0
    build/tests/system/deadline 10 "$binary" >"$binary.log" 2>&1 || status=$?
    cat "$binary.log"
    if [ "$expected" = pass ]; then
        [ "$status" -eq 0 ] || exit 1
    else
        [ "$status" -gt 0 ] && [ "$status" -lt 100 ] || exit 1
        grep -Fq "$message" "$binary.log" || exit 1
        if grep -Eq 'AddressSanitizer|runtime error:' "$binary.log"; then
            exit 1
        fi
    fi
    echo "request-mutation: result: $phase status $status"
}

run_phase baseline pass ''
[ "$#" -eq 0 ] || faults="$*"
for fault in $faults; do
    source=$(source_for "$fault")
    apply_fault "$work/$source" "$fault"
    case $fault in
        deadline-late) message='the last period quarantines the held request' ;;
        late-error) message='a read answered before quarantine succeeds with its staged bytes' ;;
        flush-final) message='the final flush deadline quarantines its DMA page' ;;
        flush-error) message='a flush answered after one deadline succeeds and releases its page' ;;
        pool-deadline) message="a queued reader's deadline starts at admission after the pool wait" ;;
        owner) message='the partition is accounted to its submitter' ;;
        generation) message='a wrong generation cannot complete or release a request' ;;
        early-free) message='caller release cannot free a page still owned by the device' ;;
        late-leak) message='late completion frees the abandoned page and clears the stall' ;;
        hold) message='timer activity cannot deliver a held read' ;;
        ready) message='an unfinished backing transfer cannot be answered' ;;
        read-copy) message='the completed read returns staged bytes' ;;
    esac
    run_phase "$fault-faulty" fail "$message"
    cp "rach2/sys/block/$source" "$work/$source"
    touch "$work/$source"
    run_phase "$fault-restored" pass ''
done
echo 'request-mutation: result: pass: faults failed; restored tests passed'
