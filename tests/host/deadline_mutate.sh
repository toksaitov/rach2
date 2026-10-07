#!/bin/sh
# Prove the host rows on wait deadlines, the timer's tick and the kinds of
# change a wake counts with faults in a private kernel tree.
#
# Usage: sh tests/host/deadline_mutate.sh [--texts]
#
# wait_queue_hosttest requires a wait that a wake does not end to arm the
# caller's deadline again before it blocks again (entry 217), and a wake to be
# counted for the kinds of change it names and no others, each time it comes,
# every kind for wq_wake_all, the count of the kinds asked for to be read back,
# and a wake that names none to wake its waiters still (entry 246);
# poll_deadline_hosttest requires the poll, ppoll and select timeouts to end at
# the first tick at or after their end, measured from where in the tick in
# progress they began, seconds to count a hundred ticks and any part of a tick
# to round up, a zero timeout to end at once, the longest ppoll timeout to end
# 0x7FFFFFFF ticks ahead across the wrap and not be reached where it began, and
# a deadline to be reached at its tick and not before (entry 216);
# process_timer_hosttest requires a due timer to post SIGALRM to its process,
# running or asleep, and be armed again for its interval, an interval of one
# tick included, a single alarm to be left disarmed, a sleeper to wake at its
# deadline and not before, a blocked process with no deadline and a running
# process to be left alone, a tick that wakes a sleeper to post the sleeper's
# alarm and a later process's still, a timer armed again onto tick 0 across the
# wrap to be due at 1, a timer and a sleeper due past the wrap to wait for it
# and be served after it, and an alarm tick that wraps to 0 to be moved to 1
# (entries 212 and 215). Each test is compiled against a private copy of
# rach2/sys as it is, where it must pass; with each fault below, where it must
# fail every row the fault names; and restored, touched and compiled again,
# where it must pass. The trap stops the test before it restores the source.
# --texts checks that each fault's lines are once in the tree's source, and
# builds and runs nothing.
set -eu
repo=$(CDPATH='' cd -- "$(dirname -- "$0")/../.." && pwd)
cd "$repo"
texts=
[ "${1:-}" != --texts ] || texts=1
if [ -z "$texts" ]; then
    . "$repo/tests/system/runlock.sh"
    runlock_hold "$repo" deadline-mutation "$repo/tests/host/deadline_mutate.sh" "$@"
fi
sources='proc/wait_queue.c io/poll.c proc/process.c'
if [ -z "$texts" ]; then
    mkdir -p build
    work=$(mktemp -d "$repo/build/deadline-proof.XXXXXX")
    mkdir "$work/sys"
    cp -R rach2/sys/. "$work/sys/"
    for source in $sources; do
        cp "$work/sys/$source" "$work/$(basename "$source").saved"
    done
fi
child=

restore()
{
    for source in $sources; do
        cp "$work/$(basename "$source").saved" "$work/sys/$source"
        touch "$work/sys/$source"
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

# run_phase PHASE TEST EXPECTED DETAILS: compile tests/host/TEST.c against the
# private tree and run it; a pass must exit 0, a fail must exit nonzero and
# print each of DETAILS, which are separated by `|`.
run_phase()
{
    phase=$1
    test=$2
    expected=$3
    details=$4
    binary="$work/$phase"
    # The test's host-deps, a kernel source taken from the private tree.
    deps=
    listed=$(grep -oE 'host-deps:[^*]*' "tests/host/$test.c" | sed 's/host-deps://')
    for dep in $listed; do
        case $dep in
            rach2/sys/*) deps="$deps $work/sys/${dep#rach2/sys/}" ;;
            *) deps="$deps $dep" ;;
        esac
    done
    # shellcheck disable=SC2086 # deps is a list of paths without spaces
    if ! "${CC:-cc}" -std=c99 -D_DEFAULT_SOURCE -D_FORTIFY_SOURCE=0 -DRACH2_HOSTTEST \
        -fsanitize=address,undefined -fno-sanitize-recover=all -g -O1 -Wno-empty-body \
        -ffreestanding -Wno-int-to-pointer-cast -Wno-pointer-to-int-cast \
        -I "$work/sys" -I "$work/sys/arch/x86" -I "$work/sys/arch/i686" -I "$work/sys/lib" -I "$work/sys/proc" \
        "tests/host/$test.c" $deps -o "$binary" >"$binary.compile.log" 2>&1; then
        cat "$binary.compile.log" >&2
        exit 1
    fi
    # shellcheck disable=SC2086
    (cd "$work/sys" && cksum $sources) >"$binary.artifacts"
    cksum "$binary" >>"$binary.artifacts"
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
        rest=$details
        while [ -n "$rest" ]; do
            detail=${rest%%|*}
            grep -F "$detail" "$binary.log" >/dev/null || {
                echo "deadline-mutation: the row did not fail: $phase: $detail" >&2; exit 1;
            }
            case $rest in
                *'|'*) rest=${rest#*|} ;;
                *) rest= ;;
            esac
        done
    fi
    echo "deadline-mutation: result: $phase status $status"
}

# fault FILE OLD NEW: replace the lines OLD of the private FILE, which must
# occur once, with NEW.
fault()
{
    python3 - "$work/sys/$1" "$2" "$3" <<'PYTHON'
from pathlib import Path
import os
import sys

path = Path(sys.argv[1])
text = path.read_text()
old, new = sys.argv[2] + '\n', sys.argv[3] + '\n'
if text.count(old) != 1:
    raise SystemExit('deadline-mutation: the lines to change are not unique: ' + sys.argv[2])
if not os.environ.get('RACH2_FAULT_TEXTS'):
    path.write_text(text.replace(old, new))
PYTHON
}

# prove PHASE TEST FILE OLD NEW DETAILS: the fault, its failing run, and the
# restored source's passing run.
prove()
{
    fault "$3" "$4" "$5"
    run_phase "$1" "$2" fail "$6"
    restore
    run_phase "$1-restored" "$2" pass ''
}

if [ -n "$texts" ]; then
    work=rach2
    run_phase() { :; }
    restore() { :; }
    RACH2_FAULT_TEXTS=1
    export RACH2_FAULT_TEXTS
else
    echo "deadline-mutation: logs: $work"
fi
run_phase wait-fixed wait_queue_hosttest pass ''
run_phase poll-fixed poll_deadline_hosttest pass ''
run_phase tick-fixed process_timer_hosttest pass ''

prove wait-unarmed wait_queue_hosttest proc/wait_queue.c \
    '        current_proc->wakeup_tick = deadline;' '' \
    'deadline_at_block[1] == 77'
prove wake-every-kind wait_queue_hosttest proc/wait_queue.c \
    '        if (kinds & (1u << k)) {
            wq->wakes[k]++;' \
    '        if (k < WQ_KINDS) {
            wq->wakes[k]++;' \
    'wq_wakes(&wq, WQ_ROOM) == 0 && wq_wakes(&wq, WQ_HANGUP) == 0|wq_wakes(&wq, WQ_ALL) == 3'
prove wake-count-set wait_queue_hosttest proc/wait_queue.c \
    '            wq->wakes[k]++;' \
    '            wq->wakes[k] = 1;' \
    'wq_wakes(&wq, WQ_DATA) == 2'
prove wakes-every-kind wait_queue_hosttest proc/wait_queue.c \
    '        if (kinds & (1u << k)) {
            sum += wq->wakes[k];' \
    '        if (k < WQ_KINDS) {
            sum += wq->wakes[k];' \
    'wq_wakes(&wq, WQ_DATA) == 1 && wq_wakes(&wq, WQ_ROOM) == 1'
prove wake-all-one-kind wait_queue_hosttest proc/wait_queue.c \
    '    wq_wake(wq, WQ_ALL);' \
    '    wq_wake(wq, WQ_DATA);' \
    'wq_wakes(&wq, WQ_HANGUP) == 1 && wq_wakes(&wq, WQ_ALL) == 3'
prove wake-none-skipped wait_queue_hosttest proc/wait_queue.c \
    '    if (!wq) {
        return;
    }
    flags = irq_save();
    wq_lock();
    w = wq->head;
    wq->head = NULL;' \
    '    if (!wq || !kinds) {
        return;
    }
    flags = irq_save();
    wq_lock();
    w = wq->head;
    wq->head = NULL;' \
    'b.state == PROC_READY && total_queued(&wq) == 0'

prove poll-early poll_deadline_hosttest io/poll.c \
    '    ticks += (phase_ns / 1000u + us + 9999u) / 10000u;' \
    '    ticks += (phase_ns / 1000u + us) / 10000u;' \
    'a poll of 1 ms from tick 1000 ends at 1001|a ppoll of 1 ns from tick 1000 ends at 1001|a select of 1 us from tick 1000 ends at 1001'
prove poll-phase poll_deadline_hosttest io/poll.c \
    '    ticks += (phase_ns / 1000u + us + 9999u) / 10000u;' \
    '    ticks += (us + 9999u) / 10000u;' \
    'and one of 7 ms at the tick after'
prove poll-clamp poll_deadline_hosttest io/poll.c \
    '    return now + (ticks > 0x7FFFFFFFu ? 0x7FFFFFFFu : ticks);' \
    '    return now + ticks;' \
    'the longest ppoll timeout ends 0x7FFFFFFF ticks ahead across the wrap|and its deadline is not reached at the tick it began'
prove ppoll-nsec poll_deadline_hosttest io/poll.c \
    '    return tick_deadline(ticks, ((uint32_t)ts->tv_nsec % 10000000u + 999u) / 1000u);' \
    '    return tick_deadline(ticks, (uint32_t)ts->tv_nsec % 10000000u / 1000u);' \
    'a ppoll of 1 ns from tick 1000 ends at 1001'
prove ppoll-sec poll_deadline_hosttest io/poll.c \
    '    ticks = sec * 100u + (uint32_t)ts->tv_nsec / 10000000u;' \
    '    ticks = sec * 10u + (uint32_t)ts->tv_nsec / 10000000u;' \
    'a ppoll of 1 s and 11 ms from tick 1000 ends at 1102'
prove select-usec poll_deadline_hosttest io/poll.c \
    '    return (int)(tv->tv_sec * 1000 + (tv->tv_usec + 999) / 1000);' \
    '    return (int)(tv->tv_sec * 1000 + tv->tv_usec / 1000);' \
    'a select of 1 us from tick 1000 ends at 1001'
prove poll-reached poll_deadline_hosttest io/poll.c \
    '    return (int32_t)(deadline - kernel_ticks) <= 0;' \
    '    return (int32_t)(deadline - kernel_ticks) <= 1;' \
    'a deadline one tick ahead is not reached'
prove poll-late poll_deadline_hosttest io/poll.c \
    '    return (int32_t)(deadline - kernel_ticks) <= 0;' \
    '    return (int32_t)(deadline - kernel_ticks) < 0;' \
    'a deadline at the tick or behind it is'
prove poll-wrap poll_deadline_hosttest io/poll.c \
    '    return (int32_t)(deadline - kernel_ticks) <= 0;' \
    '    return deadline <= kernel_ticks;' \
    'and its deadline is not reached at the tick it began'
prove tick-nearest poll_deadline_hosttest io/poll.c \
    '    ticks += (phase_ns / 1000u + us + 9999u) / 10000u;' \
    '    ticks += (phase_ns / 1000u + us + 5000u) / 10000u;' \
    'a poll of 21 ms rounds up to three ticks|a ppoll of 1 s and 11 ms from tick 1000 ends at 1102'
prove ppoll-sec-clamp poll_deadline_hosttest io/poll.c \
    '    sec = (unsigned long)ts->tv_sec > 21474836ul ? 21474836u : (uint32_t)ts->tv_sec;' \
    '    sec = (unsigned long)ts->tv_sec > 42949672ul ? 42949672u : (uint32_t)ts->tv_sec;' \
    'the longest ppoll timeout ends 0x7FFFFFFF ticks ahead across the wrap'
prove tick-round-one poll_deadline_hosttest io/poll.c \
    '    ticks += (phase_ns / 1000u + us + 9999u) / 10000u;' \
    '    ticks += (phase_ns / 1000u + us + 9998u) / 10000u;' \
    'a ppoll of 1 ns from tick 1000 ends at 1001'
prove ppoll-nsec-one poll_deadline_hosttest io/poll.c \
    '    return tick_deadline(ticks, ((uint32_t)ts->tv_nsec % 10000000u + 999u) / 1000u);' \
    '    return tick_deadline(ticks, ((uint32_t)ts->tv_nsec % 10000000u + 998u) / 1000u);' \
    'a ppoll of 1 ns from tick 1000 ends at 1001'
prove select-zero poll_deadline_hosttest io/poll.c \
    '    return (int)(tv->tv_sec * 1000 + (tv->tv_usec + 999) / 1000);' \
    '    return (int)(tv->tv_sec * 1000 + (tv->tv_usec + 1000) / 1000);' \
    'a zero select timeout ends at once'
prove select-sec poll_deadline_hosttest io/poll.c \
    '    return (int)(tv->tv_sec * 1000 + (tv->tv_usec + 999) / 1000);' \
    '    return (int)(tv->tv_sec * 100 + (tv->tv_usec + 999) / 1000);' \
    'a select of 1 s and 11 ms from tick 1000 ends at 1102'

prove tick-running process_timer_hosttest proc/process.c \
    '        if (p->alarm_tick != 0 && (int32_t)(now - p->alarm_tick) >= 0) {' \
    '        if (p->state == PROC_BLOCKED && p->alarm_tick != 0 && (int32_t)(now - p->alarm_tick) >= 0) {' \
    'a due timer posts SIGALRM to a process that is running'
prove tick-blocked process_timer_hosttest proc/process.c \
    '        if (p->alarm_tick != 0 && (int32_t)(now - p->alarm_tick) >= 0) {' \
    '        if (p->state != PROC_BLOCKED && p->alarm_tick != 0 && (int32_t)(now - p->alarm_tick) >= 0) {' \
    'and to one that is asleep'
prove tick-alarm-early process_timer_hosttest proc/process.c \
    '        if (p->alarm_tick != 0 && (int32_t)(now - p->alarm_tick) >= 0) {' \
    '        if (p->alarm_tick != 0 && (int32_t)(now - p->alarm_tick) >= -1) {' \
    'a timer not yet due posts nothing'
prove tick-first-only process_timer_hosttest proc/process.c \
    '    for (p = proc_list; p; p = p->next) {
        if (p->state == PROC_BLOCKED && p->wakeup_tick != 0 &&' \
    '    for (p = proc_list; p; p = NULL) {
        if (p->state == PROC_BLOCKED && p->wakeup_tick != 0 &&' \
    'a due timer posts SIGALRM to a process that is running'
prove tick-rearm process_timer_hosttest proc/process.c \
    '            p->alarm_tick = p->alarm_interval != 0 ? proc_alarm_at(now, p->alarm_interval) : 0;' \
    '            p->alarm_tick = p->alarm_interval != 0 ? proc_alarm_at(now, 1u) : 0;' \
    'and each is armed again for its interval from that tick'
prove tick-single process_timer_hosttest proc/process.c \
    '            p->alarm_tick = p->alarm_interval != 0 ? proc_alarm_at(now, p->alarm_interval) : 0;' \
    '            p->alarm_tick = proc_alarm_at(now, p->alarm_interval);' \
    'a single alarm posts once and leaves the timer disarmed'
prove tick-early process_timer_hosttest proc/process.c \
    '            (int32_t)(now - p->wakeup_tick) >= 0) {' \
    '            (int32_t)(now - p->wakeup_tick) >= -1) {' \
    'a sleeper is not woken before its deadline'
prove tick-late process_timer_hosttest proc/process.c \
    '            (int32_t)(now - p->wakeup_tick) >= 0) {' \
    '            (int32_t)(now - p->wakeup_tick) > 0) {' \
    'and is made runnable at its deadline tick'
prove tick-zero process_timer_hosttest proc/process.c \
    '        if (p->state == PROC_BLOCKED && p->wakeup_tick != 0 &&' \
    '        if (p->state == PROC_BLOCKED &&' \
    'a blocked process with no deadline is not made runnable'
prove tick-state process_timer_hosttest proc/process.c \
    '        if (p->state == PROC_BLOCKED && p->wakeup_tick != 0 &&' \
    '        if (p->wakeup_tick != 0 &&' \
    'a running process is left as it is whatever its wakeup tick says'
prove tick-sentinel process_timer_hosttest proc/process.c \
    '    return at != 0 ? at : 1u;' \
    '    return at;' \
    'an alarm that wraps to 0 is due at 1'
prove tick-stop process_timer_hosttest proc/process.c \
    '            sched_make_ready(p);
        }
        if (p->alarm_tick != 0 && (int32_t)(now - p->alarm_tick) >= 0) {' \
    '            sched_make_ready(p);
            break;
        }
        if (p->alarm_tick != 0 && (int32_t)(now - p->alarm_tick) >= 0) {' \
    'a tick that wakes a sleeper still posts its alarm and a later process'
prove tick-else process_timer_hosttest proc/process.c \
    '            sched_make_ready(p);
        }
        if (p->alarm_tick != 0 && (int32_t)(now - p->alarm_tick) >= 0) {' \
    '            sched_make_ready(p);
        } else if (p->alarm_tick != 0 && (int32_t)(now - p->alarm_tick) >= 0) {' \
    'a tick that wakes a sleeper still posts its alarm and a later process'
prove tick-one process_timer_hosttest proc/process.c \
    '            p->alarm_tick = p->alarm_interval != 0 ? proc_alarm_at(now, p->alarm_interval) : 0;' \
    '            p->alarm_tick = p->alarm_interval > 1 ? proc_alarm_at(now, p->alarm_interval) : 0;' \
    'an interval of one tick is armed again'
prove tick-rearm-wrap process_timer_hosttest proc/process.c \
    '            p->alarm_tick = p->alarm_interval != 0 ? proc_alarm_at(now, p->alarm_interval) : 0;' \
    '            p->alarm_tick = p->alarm_interval != 0 ? now + p->alarm_interval : 0;' \
    'a timer armed again onto tick 0 across the wrap is due at 1'
prove tick-wake-wrap process_timer_hosttest proc/process.c \
    '            (int32_t)(now - p->wakeup_tick) >= 0) {' \
    '            now >= p->wakeup_tick) {' \
    'a timer and a sleeper due past the wrap wait for it'
prove tick-alarm-wrap process_timer_hosttest proc/process.c \
    '        if (p->alarm_tick != 0 && (int32_t)(now - p->alarm_tick) >= 0) {' \
    '        if (p->alarm_tick != 0 && now >= p->alarm_tick) {' \
    'a timer and a sleeper due past the wrap wait for it'

if [ -n "$texts" ]; then
    echo 'deadline-mutation: texts: every fault is once in its source'
    exit 0
fi
for source in $sources; do
    cmp "$work/$(basename "$source").saved" "$work/sys/$source"
done
echo 'deadline-mutation: result: pass: each fault failed its rows; the restored sources passed'
