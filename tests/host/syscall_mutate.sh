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

# run_phase PHASE STATUS DETAILS: compile the test against the private copies
# and run it; it must exit with STATUS and print each of DETAILS, which are
# separated by `|`.
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
    rest=$detail
    while [ -n "$rest" ]; do
        grep -F "${rest%%|*}" "$binary.log" >/dev/null || exit 1
        case $rest in
            *'|'*) rest=${rest#*|} ;;
            *) rest= ;;
        esac
    done
    echo "syscall-mutation: result: $phase status $status"
}

echo "syscall-mutation: logs: $work"
run_phase fixed 0 'syscall-test: pass:'
for mutation in connect-wait connect-result connect-nonblock framebuffer-owner framebuffer-reentry \
    clock sleep-mask sleep-block-order sleep-arm-order sleep-halt-unmasked sleep-halt-read \
    sleep-left-early sleep-switch sleep-early-return sleep-deadline sleep-zero sleep-nsec \
    sleep-nsec-one sleep-nsec-near sleep-arg-sec sleep-arg-nsec sleep-arg-nsec-neg sleep-remainder sleep-last-tick sleep-cut-short \
    sleep-signal-first sleep-rearm sleep-wrap sleep-clamp sleep-sec-clamp sleep-unmask \
    sleep-running rem-sec rem-zero timer-first timer-left timer-floor timer-disarm timer-round \
    timer-usec-one timer-usec-near \
    timer-sec-clamp timer-clamp timer-clamp-low timer-interval-read timer-interval-sec \
    timer-interval-read-sec timer-usec timer-mask getitimer-which alarm-round alarm-nearest \
    alarm-half alarm-interval alarm-mask clock-coarse clock-cpu-system clock-cpu-user \
    clock-cpu-pid clock-cpu-thread clock-descriptor clock-res clock-sleep-clock \
    clock-sleep-thread clock-sleep-due clock-sleep-epoch clock-sleep-absolute-rem \
    clock-sleep-cpu-due; do
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
        'if (fb_owner_pid != current_proc->tgid &&',
        'if (0 && fb_owner_pid != current_proc->tgid &&'),
    'framebuffer-reentry': ('syscall',
        'if (!vga_graphics_mode) {',
        'if (0) {'),
    'sleep-mask': ('syscall',
        'uint32_t flags = kirq_save();\n'
        '        int32_t left = (int32_t)(target - kernel_ticks);',
        'uint32_t flags = 0;\n'
        '        int32_t left = (int32_t)(target - kernel_ticks);'),
    'sleep-block-order': ('syscall',
        'current_proc->state = PROC_BLOCKED;\n'
        '        kirq_restore(flags);\n'
        '        schedule(); /* yield',
        'kirq_restore(flags);\n'
        '        current_proc->state = PROC_BLOCKED;\n'
        '        schedule(); /* yield'),
    'sleep-arm-order': ('syscall',
        'current_proc->wakeup_tick = target != 0 ? target : 1u;\n'
        '        current_proc->state = PROC_BLOCKED;\n'
        '        kirq_restore(flags);',
        'current_proc->state = PROC_BLOCKED;\n'
        '        kirq_restore(flags);\n'
        '        current_proc->wakeup_tick = target != 0 ? target : 1u;'),
    'sleep-halt-unmasked': ('syscall',
        '        flags = kirq_save();\n'
        '        if (current_proc->state == PROC_BLOCKED) {\n'
        '            idle_wait_irq_off();\n'
        '        }\n'
        '        kirq_restore(flags);',
        '        if (current_proc->state == PROC_BLOCKED) {\n'
        '            idle_wait_irq_off();\n'
        '        }'),
    'sleep-halt-read': ('syscall',
        '        flags = kirq_save();\n'
        '        if (current_proc->state == PROC_BLOCKED) {\n'
        '            idle_wait_irq_off();\n'
        '        }\n'
        '        kirq_restore(flags);',
        '        if (current_proc->state == PROC_BLOCKED) {\n'
        '            flags = kirq_save();\n'
        '            idle_wait_irq_off();\n'
        '            kirq_restore(flags);\n'
        '        }'),
    'sleep-left-early': ('syscall',
        '        uint32_t flags = kirq_save();\n'
        '        int32_t left = (int32_t)(target - kernel_ticks);',
        '        int32_t left = (int32_t)(target - kernel_ticks);\n'
        '        uint32_t flags = kirq_save();'),
    'sleep-switch': ('syscall',
        '        if (current_proc->state == PROC_BLOCKED) {\n'
        '            idle_wait_irq_off();',
        '        if (current_proc->state != PROC_READY) {\n'
        '            idle_wait_irq_off();'),
    'sleep-early-return': ('syscall',
        '        if (current_proc->state == PROC_BLOCKED) {\n'
        '            idle_wait_irq_off();\n'
        '        }',
        '        if (current_proc->state == PROC_BLOCKED) {\n'
        '            kirq_restore(flags);\n'
        '            break;\n'
        '        }'),
    'sleep-deadline': ('syscall',
        'target = kernel_ticks + ticks + (ticks != 0 ? 1u : 0u);',
        'target = kernel_ticks + ticks;'),
    'sleep-zero': ('syscall',
        'target = kernel_ticks + ticks + (ticks != 0 ? 1u : 0u);',
        'target = kernel_ticks + ticks + 1u;'),
    'sleep-nsec': ('syscall',
        'uint32_t nsec_ticks = ((uint32_t)req->tv_nsec + 9999999u) / 10000000u;',
        'uint32_t nsec_ticks = (uint32_t)req->tv_nsec / 10000000u;'),
    'sleep-nsec-one': ('syscall',
        'uint32_t nsec_ticks = ((uint32_t)req->tv_nsec + 9999999u) / 10000000u;',
        'uint32_t nsec_ticks = ((uint32_t)req->tv_nsec + 9999998u) / 10000000u;'),
    'sleep-nsec-near': ('syscall',
        'uint32_t nsec_ticks = ((uint32_t)req->tv_nsec + 9999999u) / 10000000u;',
        'uint32_t nsec_ticks = ((uint32_t)req->tv_nsec + 5000000u) / 10000000u;'),
    'sleep-arg-nsec-neg': ('syscall',
        'return req->tv_sec >= 0 && req->tv_nsec >= 0 && req->tv_nsec < 1000000000;',
        'return req->tv_sec >= 0 && req->tv_nsec < 1000000000;'),
    'sleep-arg-sec': ('syscall',
        'return req->tv_sec >= 0 && req->tv_nsec >= 0 && req->tv_nsec < 1000000000;',
        'return req->tv_nsec >= 0 && req->tv_nsec < 1000000000;'),
    'sleep-arg-nsec': ('syscall',
        'return req->tv_sec >= 0 && req->tv_nsec >= 0 && req->tv_nsec < 1000000000;',
        'return req->tv_sec >= 0 && req->tv_nsec >= 0 && req->tv_nsec <= 1000000000;'),
    'sleep-remainder': ('syscall',
        'uint32_t unslept = left - 1u;',
        'uint32_t unslept = left;'),
    'sleep-last-tick': ('syscall',
        'uint32_t unslept = left - 1u;',
        'uint32_t unslept = left > 1u ? left - 1u : 1u;'),
    'sleep-cut-short': ('syscall',
        '    process_count_sleep_cut_short();\n'
        '    if (urem) {',
        '    if (left > 1u)\n'
        '        process_count_sleep_cut_short();\n'
        '    if (urem) {'),
    'sleep-signal-first': ('syscall',
        '        if (left <= 0) {\n'
        '            kirq_restore(flags);\n'
        '            break;\n'
        '        }\n'
        '        if (signal_pending()) {\n'
        '            kirq_restore(flags);\n'
        '            return nanosleep_interrupted(urem, (uint32_t)left);\n'
        '        }',
        '        if (signal_pending()) {\n'
        '            kirq_restore(flags);\n'
        '            return nanosleep_interrupted(urem, (uint32_t)(left > 0 ? left : 1));\n'
        '        }\n'
        '        if (left <= 0) {\n'
        '            kirq_restore(flags);\n'
        '            break;\n'
        '        }'),
    'sleep-rearm': ('syscall',
        'current_proc->wakeup_tick = target != 0 ? target : 1u;',
        'current_proc->wakeup_tick = current_proc->state == PROC_RUNNING ? (target != 0 ? target : 1u) : 0u;'),
    'sleep-wrap': ('syscall',
        'current_proc->wakeup_tick = target != 0 ? target : 1u;',
        'current_proc->wakeup_tick = target;'),
    'sleep-clamp': ('syscall',
        '    if (ticks > 0x7FFFFFFEu) {\n'
        '        ticks = 0x7FFFFFFEu;',
        '    if (ticks > 0x7FFFFFFFu) {\n'
        '        ticks = 0x7FFFFFFFu;'),
    'sleep-sec-clamp': ('syscall',
        '    if (sec > 21474836u) {\n'
        '        sec = 21474836u; /* keeps the intermediate sec*100 below 2^31 */\n'
        '    }\n',
        ''),
    'sleep-unmask': ('syscall',
        '        if (left <= 0) {\n'
        '            kirq_restore(flags);\n'
        '            break;\n'
        '        }',
        '        if (left <= 0) {\n'
        '            break;\n'
        '        }'),
    'sleep-running': ('syscall',
        '    current_proc->wakeup_tick = 0;\n'
        '    current_proc->state = PROC_RUNNING;\n'
        '    return 0;\n'
        '}',
        '    current_proc->wakeup_tick = 0;\n'
        '    return 0;\n'
        '}'),
    'rem-zero': ('syscall',
        'if (copy_to_user(urem, &zero, sizeof(zero)) != 0) {',
        'if (0 && copy_to_user(urem, &zero, sizeof(zero)) != 0) {'),
    'rem-sec': ('syscall',
        'rem.tv_sec = (int32_t)(unslept / 100u);\n'
        '        rem.tv_nsec = (int32_t)((unslept % 100u) * 10000000u);',
        'rem.tv_sec = 0;\n'
        '        rem.tv_nsec = (int32_t)(unslept * 10000000u);'),
    'timer-first': ('syscall',
        'p->alarm_tick = proc_alarm_at(kernel_ticks, value + 1u);',
        'p->alarm_tick = proc_alarm_at(kernel_ticks, value);'),
    'timer-left': ('syscall',
        'return (int32_t)left > 1 ? left - 1u : 1u;',
        'return (int32_t)left > 0 ? left : 1u;'),
    'timer-floor': ('syscall',
        'return (int32_t)left > 1 ? left - 1u : 1u;',
        'return (int32_t)left > 1 ? left - 1u : 0u;'),
    'timer-disarm': ('syscall',
        '    if (value == 0) {\n'
        '        p->alarm_tick = 0;',
        '    if (0) {\n'
        '        p->alarm_tick = 0;'),
    'timer-round': ('syscall',
        'ticks = whole * 100u + ((uint32_t)usec + 9999u) / 10000u;',
        'ticks = whole * 100u + (uint32_t)usec / 10000u;'),
    'timer-usec-one': ('syscall',
        'ticks = whole * 100u + ((uint32_t)usec + 9999u) / 10000u;',
        'ticks = whole * 100u + ((uint32_t)usec + 9998u) / 10000u;'),
    'timer-usec-near': ('syscall',
        'ticks = whole * 100u + ((uint32_t)usec + 9999u) / 10000u;',
        'ticks = whole * 100u + ((uint32_t)usec + 5000u) / 10000u;'),
    'timer-sec-clamp': ('syscall',
        '    if (whole > 21474836u) {\n'
        '        whole = 21474836u;\n'
        '    }\n',
        ''),
    'timer-clamp': ('syscall',
        'return ticks > 0x7FFFFFFDu ? 0x7FFFFFFDu : ticks;',
        'return ticks > 0x7FFFFFFEu ? 0x7FFFFFFEu : ticks;'),
    'timer-clamp-low': ('syscall',
        'return ticks > 0x7FFFFFFDu ? 0x7FFFFFFDu : ticks;',
        'return ticks > 0x7FFFFFFCu ? 0x7FFFFFFCu : ticks;'),
    'timer-interval-sec': ('syscall',
        '               itimer_ticks(set.interval_sec, set.interval_usec));',
        '               itimer_ticks(0, set.interval_usec));'),
    'timer-interval-read-sec': ('syscall',
        'out->interval_sec = (int32_t)(current_proc->tg->leader->alarm_interval / 100u);',
        'out->interval_sec = 0;'),
    'timer-usec': ('syscall',
        'set.interval_sec < 0 || set.interval_usec < 0 || set.interval_usec >= 1000000) {',
        'set.interval_sec < 0 || set.interval_usec < 0 || set.interval_usec > 1000000) {'),
    'timer-mask': ('syscall',
        '    flags = kirq_save();\n'
        '    itimer_read(&was);',
        '    flags = 0;\n'
        '    itimer_read(&was);'),
    'getitimer-which': ('syscall',
        '    if ((int32_t)params->ebx != ITIMER_REAL) {\n'
        '        return -EINVAL;\n'
        '    }\n'
        '    flags = kirq_save();\n'
        '    itimer_read(&cur);',
        '    flags = kirq_save();\n'
        '    itimer_read(&cur);'),
    'alarm-half': ('syscall',
        '    if ((seconds == 0 && usec != 0) || usec >= 500000u) {',
        '    if ((seconds == 0 && usec != 0) || usec > 500000u) {'),
    'alarm-interval': ('syscall',
        '    itimer_set(itimer_ticks((int32_t)(params->ebx > 21474836u ? 21474836u : params->ebx), 0), 0);',
        '    itimer_set(itimer_ticks((int32_t)(params->ebx > 21474836u ? 21474836u : params->ebx), 0),\n'
        '               current_proc->alarm_interval);'),
    'alarm-mask': ('syscall',
        '    uint32_t flags = kirq_save();\n'
        '    uint32_t left = itimer_left();',
        '    uint32_t flags = 0;\n'
        '    uint32_t left = itimer_left();'),
    'timer-interval-read': ('syscall',
        'out->interval_usec = (int32_t)((current_proc->tg->leader->alarm_interval % 100u) * 10000u);',
        'out->interval_usec = (int32_t)((current_proc->tg->leader->alarm_interval % 100u) * 1000u);'),
    'alarm-round': ('syscall',
        '    if ((seconds == 0 && usec != 0) || usec >= 500000u) {\n'
        '        seconds++;\n'
        '    }\n',
        ''),
    'alarm-nearest': ('syscall',
        '    if ((seconds == 0 && usec != 0) || usec >= 500000u) {',
        '    if ((seconds == 0 && usec != 0) || usec != 0) {'),
    'clock': ('syscall',
        'out->tv_sec = (int32_t)kernel_uptime_seconds;\n'
        '        out->tv_nsec = (int32_t)(kernel_second_ticks * 10000000);',
        'out->tv_sec = (int32_t)(kernel_ticks / 100);\n'
        '        out->tv_nsec = (int32_t)((kernel_ticks % 100) * 10000000);'),
    'clock-coarse': ('syscall',
        '        rtc_coarse_realtime(&seconds, &nanoseconds);',
        '        get_current_time(&seconds, &nanoseconds);'),
    'clock-cpu-system': ('syscall',
        'return c->user_only ? user : user + sys;',
        'return user;'),
    'clock-cpu-user': ('syscall',
        'out->user_only = ((uint32_t)clk & CPUCLOCK_WHICH) == CPUCLOCK_VIRT;',
        'out->user_only = 0;'),
    'clock-cpu-pid': ('syscall',
        '    out->proc = get_process(id);\n',
        ''),
    'clock-cpu-thread': ('syscall',
        '    if (!out->proc || (out->thread && out->proc->tg != current_proc->tg) ||\n',
        '    if (!out->proc ||\n'),
    'clock-descriptor': ('syscall',
        'if (clk >= 0 || ((uint32_t)clk & CPUCLOCK_WHICH) == 3u) {',
        'if (clk >= 0) {'),
    'clock-res': ('syscall',
        'res.tv_nsec = c.kind == KCLOCK_REAL ? 1000 : 10000000;',
        'res.tv_nsec = 10000000;'),
    'clock-sleep-clock': ('syscall',
        '    if (clk == CLOCK_THREAD_CPUTIME_ID || clk == CLOCK_MONOTONIC_RAW ||\n'
        '        clk == CLOCK_REALTIME_COARSE || clk == CLOCK_MONOTONIC_COARSE) {',
        '    if (clk == CLOCK_THREAD_CPUTIME_ID || clk == CLOCK_MONOTONIC_RAW ||\n'
        '        clk == CLOCK_MONOTONIC_COARSE) {'),
    'clock-sleep-thread': ('syscall',
        '    if (clk < 0 && ((uint32_t)clk & CPUCLOCK_PERTHREAD)) {\n'
        '        return -EINVAL;\n'
        '    }\n',
        ''),
    'clock-sleep-due': ('syscall',
        'due = (uint64_t)sec * 100u + ((uint32_t)when->tv_nsec + 9999999u) / 10000000u;',
        'due = (uint64_t)sec * 100u + (uint32_t)when->tv_nsec / 10000000u;'),
    'clock-sleep-epoch': ('syscall',
        '        if (kind == KCLOCK_REAL) {\n'
        '            sec -= rtc_boot_epoch();\n'
        '        }\n',
        ''),
    'clock-sleep-absolute-rem': ('syscall',
        '    if (absolute) {\n'
        '        urem = NULL;\n'
        '    }\n',
        ''),
    'clock-sleep-cpu-due': ('syscall',
        'rem.tv_nsec = (int32_t)(((due - now) % 100u) * 10000000u);',
        'rem.tv_nsec = (int32_t)((due % 100u) * 10000000u);'),
}
name, old, new = changes[sys.argv[2]]
path = work / (name + '.c')
text = path.read_text()
if text.count(old) != 1:
    raise SystemExit('syscall-mutation: the lines to change do not occur exactly once: ' + sys.argv[2])
path.write_text(text.replace(old, new))
PY
    case "$mutation" in
        connect-wait) detail='completed handshake wakes caller' ;;
        connect-result) detail='blocking connect result' ;;
        connect-nonblock) detail='completed nonblocking connect' ;;
        framebuffer-owner) detail='stale text request refused|stale graphics request refused|stale palette request refused' ;;
        framebuffer-reentry) detail='graphics reentered' ;;
        clock) detail='monotonic survives tick wrap' ;;
        sleep-mask) detail='pending check runs with interrupts masked' ;;
        sleep-block-order) detail='and the mask is lifted with the sleeper already blocked' ;;
        sleep-arm-order) detail='and with its deadline stored' ;;
        sleep-halt-unmasked) detail='the sleep halts with interrupts masked' ;;
        sleep-halt-read) detail='a deadline tick that comes as the sleep masks after a yield is seen' ;;
        sleep-left-early) detail='a deadline tick that comes as the sleep masks after a halt is seen' ;;
        sleep-switch) detail='a sleeper switched back to after a signal runs again at once' ;;
        sleep-early-return) detail='a sleep with no signal runs to its deadline' ;;
        sleep-deadline) detail='a sleep of three ticks from tick 1000 blocks until 1004' ;;
        sleep-remainder) detail='and the whole request is left' ;;
        sleep-zero) detail='a sleep of zero does not block' ;;
        sleep-nsec) detail='a sleep of 15 ms from tick 1100 blocks until 1103' ;;
        sleep-nsec-one) detail='a sleep of 1 ns from tick 1150 blocks until 1152' ;;
        sleep-nsec-near) detail='a sleep of 1 ns from tick 1150 blocks until 1152' ;;
        sleep-arg-nsec-neg) detail='a sleep of -1 ns is refused' ;;
        sleep-arg-sec) detail='a sleep of a negative second is refused' ;;
        sleep-arg-nsec) detail='a sleep of 1000000000 ns is refused' ;;
        sleep-last-tick) detail='a signal in the last tick leaves no remainder|a sleep resumed after a signal at every tick ends within four calls' ;;
        sleep-cut-short) detail='and the sleep is counted as one a signal cut short' ;;
        sleep-signal-first) detail='a signal at the deadline ends nothing' ;;
        sleep-rearm) detail='blocks again with its deadline armed' ;;
        sleep-wrap) detail='a sleep whose deadline wraps to 0 blocks until 1' ;;
        sleep-clamp) detail='the longest sleep begun at tick 0x80000000 blocks' ;;
        sleep-sec-clamp) detail='a sleep of 42949673 s is shortened to the limit' ;;
        sleep-unmask) detail='the sleep returns with the mask lifted' ;;
        sleep-running) detail='the sleep returns running with no deadline' ;;
        rem-sec) detail='a remainder of 1.99 s is 1 s and 990 ms' ;;
        rem-zero) detail='a signal at the deadline ends nothing' ;;
        timer-first) detail='a timer of three ticks set at tick 1000 is due at 1004' ;;
        timer-left) detail='and it reads back as the three ticks set' ;;
        timer-floor) detail='a timer due at the next tick reads as one tick' ;;
        timer-disarm) detail='a value of zero disarms the timer' ;;
        timer-round) detail='a timer of 5 ms is armed, due at 1202' ;;
        timer-usec-one) detail='a timer of 1 us is armed, due at 1202' ;;
        timer-usec-near) detail='a timer of 1 us is armed, due at 1202' ;;
        timer-sec-clamp) detail='a timer of 42949673 s is shortened to the limit' ;;
        timer-clamp) detail='the longest timer set at tick 0x80000001 reads back as the longest' ;;
        timer-clamp-low) detail='the longest timer set at tick 0x80000001 reads back as the longest' ;;
        timer-interval-read) detail='and its interval' ;;
        timer-interval-sec) detail='an interval of 1.5 s reads back as 1.5 s' ;;
        timer-interval-read-sec) detail='an interval of 1.5 s reads back as 1.5 s' ;;
        timer-usec) detail='an interval of 1000000 us is refused' ;;
        timer-mask) detail='the timer is armed with interrupts masked' ;;
        getitimer-which) detail='getitimer refuses a timer other than ITIMER_REAL' ;;
        alarm-round) detail='alarm reports 4.7 s left as 5' ;;
        alarm-nearest) detail='alarm reports 4.43 s left as 4' ;;
        alarm-half) detail='alarm reports 4.5 s left as 5' ;;
        alarm-interval) detail='and leaves no interval' ;;
        alarm-mask) detail='the timer is armed with interrupts masked' ;;
        clock-coarse) detail='coarse real time is the boot'"'"'s epoch plus the uptime at the tick' ;;
        clock-cpu-system) detail='the process CPU clock reads user and system time' ;;
        clock-cpu-user) detail='a CPU clock of user time reads user time alone' ;;
        clock-cpu-pid) detail='a CPU clock by pid reads that process'"'"'s time' ;;
        clock-cpu-thread) detail='a thread clock of another process is refused' ;;
        clock-descriptor) detail='a descriptor'"'"'s clock is refused' ;;
        clock-res) detail='real time resolves to the microsecond' ;;
        clock-sleep-clock) detail='a clock Linux cannot sleep on is refused' ;;
        clock-sleep-thread) detail='a thread CPU clock by id cannot be slept on' ;;
        clock-sleep-due) detail='10.035 s since boot is reached at tick 1004' ;;
        clock-sleep-epoch) detail='real time is counted from the boot'"'"'s epoch' ;;
        clock-sleep-absolute-rem) detail='and an absolute one leaves no remainder' ;;
        clock-sleep-cpu-due) detail='and leaves the ticks still due' ;;
    esac
    run_phase "$mutation" 1 "$detail"
    restore
    run_phase "$mutation-restored" 0 'syscall-test: pass:'
done
cmp "$work/syscall.saved" "$work/syscall.c"
cmp "$work/syscall_net.saved" "$work/syscall_net.c"
