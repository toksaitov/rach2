#!/bin/sh
# Prove the syscall state checks in private compiled copies.
set -eu
repo=$(CDPATH='' cd -- "$(dirname -- "$0")/../.." && pwd)
cd "$repo"
mkdir -p build
work=$(mktemp -d "$repo/build/syscall-proof.XXXXXX")
child=
for source in syscall syscall_net; do
    cp "rach2/sys/$source.c" "$work/$source.saved"
    cp "$work/$source.saved" "$work/$source.c"
done

restore()
{
    for source in syscall syscall_net; do
        cp "$work/$source.saved" "$work/$source.c"
        touch "$work/$source.c"
    done
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

run_phase()
{
    phase=$1
    expected=$2
    detail=$3
    binary="$work/$phase"
    if ! "${CC:-cc}" -std=c99 -g -O1 -ffreestanding -DRACH2_HOSTTEST \
        -fsanitize=address,undefined -fno-sanitize-recover=all \
        -Wno-int-to-pointer-cast -Wno-pointer-to-int-cast \
        -I "$work" -I rach2/sys -I rach2/sys/lib -I rach2/sys/proc \
        tests/host/syscall_hosttest.c rach2/sys/lib/kstring.c \
        rach2/sys/lib/kctypes.c -o "$binary" >"$binary.compile.log" 2>&1; then
        cat "$binary.compile.log" >&2
        exit 1
    fi
    cksum "$work/syscall.c" "$work/syscall_net.c" "$binary" >"$binary.artifacts"
    perl -e 'alarm 10; exec @ARGV or die "syscall-mutation: exec: $!\n"' \
        "$binary" >"$binary.log" 2>&1 &
    child=$!
    status=0
    wait "$child" || status=$?
    child=
    cat "$binary.log"
    [ "$status" -eq "$expected" ] || exit 1
    grep -F "$detail" "$binary.log" >/dev/null || exit 1
    echo "syscall-mutation: result: $phase status $status"
}

echo "syscall-mutation: logs: $work"
run_phase fixed 0 'syscall-test: pass:'
for mutation in connect-wait connect-result connect-nonblock framebuffer-owner framebuffer-reentry \
    clock sleep-mask sleep-deadline sleep-remainder sleep-last-tick sleep-wrap timer-first timer-left \
    timer-clamp; do
    python3 - "$work" "$mutation" <<'PY'
from pathlib import Path
import sys
work = Path(sys.argv[1])
changes = {
    'connect-wait': ('syscall_net',
        'sock->tcb == NULL || connect_established(sock) || sock->tcb->state == TCP_CLOSED',
        'sock->tcb == NULL || sock->tcb->state == TCP_ESTABLISHED || sock->tcb->state == TCP_CLOSED'),
    'connect-result': ('syscall_net',
        'if (!connect_established(entry->sock)) {',
        'if (!entry->sock->tcb || entry->sock->tcb->state != TCP_ESTABLISHED) {'),
    'connect-nonblock': ('syscall_net',
        'if (connect_established(entry->sock)) {',
        'if (entry->sock->tcb && entry->sock->tcb->state == TCP_ESTABLISHED) {'),
    'framebuffer-owner': ('syscall',
        'if (fb_owner_pid != current_proc->pid &&',
        'if (0 && fb_owner_pid != current_proc->pid &&'),
    'framebuffer-reentry': ('syscall',
        'if (!vga_graphics_mode) {',
        'if (0) {'),
    'sleep-mask': ('syscall',
        'uint32_t flags = kirq_save();\n'
        '\n'
        '        if (!signal_pending()) {\n'
        '            /* 0 means not sleeping, so a deadline of 0 is stored as 1. */\n'
        '            current_proc->wakeup_tick = target != 0 ? target : 1u;\n'
        '            current_proc->state = PROC_BLOCKED;\n'
        '            kirq_restore(flags);\n'
        '            schedule(); /* yield to any other READY process; the timer wakes us */\n'
        '        } else {\n'
        '            kirq_restore(flags);\n'
        '        }',
        'if (!signal_pending()) {\n'
        '            /* 0 means not sleeping, so a deadline of 0 is stored as 1. */\n'
        '            current_proc->wakeup_tick = target != 0 ? target : 1u;\n'
        '            current_proc->state = PROC_BLOCKED;\n'
        '            schedule(); /* yield to any other READY process; the timer wakes us */\n'
        '        }'),
    'sleep-deadline': ('syscall',
        'target = kernel_ticks + ticks + (ticks != 0 ? 1u : 0u);',
        'target = kernel_ticks + ticks;'),
    'sleep-remainder': ('syscall',
        'uint32_t unslept = left > 1u ? left - 1u : left;',
        'uint32_t unslept = left;'),
    'sleep-last-tick': ('syscall',
        'uint32_t unslept = left > 1u ? left - 1u : left;',
        'uint32_t unslept = left > 1u ? left - 1u : 0u;'),
    'sleep-wrap': ('syscall',
        'target = kernel_ticks + ticks + (ticks != 0 ? 1u : 0u);\n'
        '    while',
        'target = kernel_ticks + ticks + (ticks != 0 ? 1u : 0u);\n'
        '    if (target == 0) {\n'
        '        target = 1;\n'
        '    }\n'
        '    while'),
    'timer-first': ('syscall',
        'current_proc->alarm_tick = proc_alarm_at(kernel_ticks, value + 1u);',
        'current_proc->alarm_tick = proc_alarm_at(kernel_ticks, value);'),
    'timer-left': ('syscall',
        'return (int32_t)left > 1 ? left - 1u : 1u;',
        'return (int32_t)left > 0 ? left : 1u;'),
    'timer-clamp': ('syscall',
        'return ticks > 0x7FFFFFFDu ? 0x7FFFFFFDu : ticks;',
        'return ticks > 0x7FFFFFFEu ? 0x7FFFFFFEu : ticks;'),
    'clock': ('syscall',
        'kts.tv_sec = (int32_t)kernel_uptime_seconds;\n'
        '        kts.tv_nsec = (int32_t)(kernel_second_ticks * 10000000);',
        'kts.tv_sec = (int32_t)(kernel_ticks / 100);\n'
        '        kts.tv_nsec = (int32_t)((kernel_ticks % 100) * 10000000);'),
}
name, old, new = changes[sys.argv[2]]
path = work / (name + '.c')
text = path.read_text()
if text.count(old) != 1:
    raise SystemExit('syscall-mutation: target is not unique')
path.write_text(text.replace(old, new))
PY
    case "$mutation" in
        connect-wait) detail='completed handshake wakes caller' ;;
        connect-result) detail='blocking connect result' ;;
        connect-nonblock) detail='completed nonblocking connect' ;;
        framebuffer-owner) detail='second owner' ;;
        framebuffer-reentry) detail='graphics reentered' ;;
        clock) detail='monotonic survives tick wrap' ;;
        sleep-mask) detail='pending check runs with interrupts masked' ;;
        sleep-deadline) detail='a sleep of three ticks from tick 1000 blocks until 1004' ;;
        sleep-remainder) detail='and the whole request is left' ;;
        sleep-last-tick) detail='a signal in the last tick leaves one tick as the remainder' ;;
        sleep-wrap) detail='the longest sleep from tick 0x80000001 blocks' ;;
        timer-first) detail='a timer of three ticks armed at tick 1000 is due at 1004' ;;
        timer-left) detail='and it reads back as the three ticks set' ;;
        timer-clamp) detail='the longest timer armed at tick 0x80000001 reads back as the longest' ;;
    esac
    run_phase "$mutation" 1 "$detail"
    restore
    run_phase "$mutation-restored" 0 'syscall-test: pass:'
done
cmp "$work/syscall.saved" "$work/syscall.c"
cmp "$work/syscall_net.saved" "$work/syscall_net.c"
