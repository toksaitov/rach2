#!/bin/sh
# Build each mutation from private sources and retain every binary and log.
set -eu

repo=$(CDPATH='' cd -- "$(dirname -- "$0")/../.." && pwd)
cd "$repo"
mkdir -p build/tests
work=$(mktemp -d "$repo/build/tests/tty-proof.XXXXXX")
child=
mkdir "$work/tty" "$work/saved"
cp rach2/sys/tty/*.[ch] "$work/saved/"
cp "$work/saved/"* "$work/tty/"

restore()
{
    cp "$work/saved/"* "$work/tty/"
    touch "$work/tty/"*
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

compile()
{
    binary="$work/$1/$2"
    mkdir -p "$work/$1"
    set -- -std=c99 -D_DEFAULT_SOURCE -D_FORTIFY_SOURCE=0 -DRACH2_HOSTTEST \
        -g -O1 -ffreestanding -fsanitize=address,undefined -fno-sanitize-recover=all \
        -Wno-pointer-to-int-cast -Wno-int-to-pointer-cast \
        -I "$work" -I rach2/sys -I rach2/sys/tty -I rach2/sys/lib -I rach2/sys/proc
    if ! "${CC:-cc}" "$@" -MMD -MF "$binary.test.d" -c \
        "tests/host/${test}_hosttest.c" -o "$binary.test.o" >"$binary.compile.log" 2>&1; then
        cat "$binary.compile.log" >&2
        exit 1
    fi
    if ! "${CC:-cc}" "$@" -MMD -MF "$binary.discipline.d" -c \
        "$work/tty/n_tty.c" -o "$binary.discipline.o" >>"$binary.compile.log" 2>&1; then
        cat "$binary.compile.log" >&2
        exit 1
    fi
    if ! "${CC:-cc}" "$@" "$binary.test.o" "$binary.discipline.o" \
        -o "$binary" >>"$binary.compile.log" 2>&1; then
        cat "$binary.compile.log" >&2
        exit 1
    fi
    cksum "$work/tty/"* >"$binary.sources"
    grep -F "$work/tty/n_tty.c" "$binary.discipline.d" >/dev/null
    if [ "$test" = tty ]; then
        grep -F "$work/tty/tty.c" "$binary.test.d" >/dev/null
        grep -F "$work/tty/pty.c" "$binary.test.d" >/dev/null
    fi
}

run_phase()
{
    phase=$1
    compile "$case_name-$phase" "$test"
    perl -e 'alarm 15; exec @ARGV or die "tty-mutation: exec: $!\n"' \
        "$binary" >"$binary.log" 2>&1 &
    child=$!
    status=0
    wait "$child" || status=$?
    child=
    cat "$binary.log"
    if [ "$phase" = mutated ]; then
        [ "$status" -gt 0 ] && [ "$status" -lt 124 ] || exit 1
        grep -F "$expected" "$binary.log" >/dev/null || exit 1
    else
        [ "$status" -eq 0 ] || exit 1
    fi
    echo "tty-mutation: result: $case_name $phase status $status"
}

mutate()
{
    python3 - "$work/tty" "$case_name" <<'PY'
from pathlib import Path
import sys

root = Path(sys.argv[1])
case = sys.argv[2]

def replace(name, old, new):
    path = root / name
    text = path.read_text()
    if text.count(old) != 1:
        raise SystemExit('tty-mutation: mutation does not match once: ' + case)
    path.write_text(text.replace(old, new))

if case == 'split-queue':
    replace('pty.h', '    wait_queue_t m_read_wq;',
            '    wait_queue_t m_read_wq;\n    wait_queue_t m_write_wq;')
    replace('pty.c', '    wait_queue_init(&p->m_read_wq);',
            '    wait_queue_init(&p->m_read_wq);\n    wait_queue_init(&p->m_write_wq);')
    replace('pty.c', 'wq_wait_event(&p->m_read_wq, ptm_has_room, p)',
            'wq_wait_event(&p->m_write_wq, ptm_has_room, p)')
    replace('pty.c', '    if (n > 0) {\n        wq_wake(&p->m_read_wq, WQ_ROOM);',
            '    if (n > 0) {\n        wq_wake(&p->m_write_wq, WQ_ROOM);')
elif case == 'flush':
    replace('tty.c', '    n_tty_flush(&t->ldisc);\n    wq_wake(t->space_wq, WQ_ROOM);',
            '    n_tty_flush(&t->ldisc);')
elif case == 'termios-flush':
    replace('tty.c', '    readable = n_tty_set_termios(&t->ldisc, kt);\n'
                    '    wq_wake(t->space_wq, WQ_ROOM);',
            '    readable = n_tty_set_termios(&t->ldisc, kt);')
elif case in ('partial-master', 'partial-slave'):
    path = root / 'pty.c'
    text = path.read_text()
    begin = text.index('static int32_t ' + ('ptm_write' if case == 'partial-master' else 'pts_write'))
    end = text.index('\nstatic ', begin + 1)
    part = text[begin:end]
    old = '            wake_master(p, &pushed);\n            return (done > 0)'
    if part.count(old) != 1:
        raise SystemExit('tty-mutation: copy failure branch does not match once')
    path.write_text(text[:begin] + part.replace(old, '            return (done > 0)') + text[end:])
elif case == 'latency-lock':
    replace('tty.c', '            flags = kirq_save();\n            rc = tty_ready_cond(t);',
            '            rc = tty_ready_cond(t);')
    replace('tty.c', '            rc = wq_wait_event(&t->input_wq, tty_ready_cond, t);\n'
                    '            kirq_restore(flags);',
            '            rc = wq_wait_event(&t->input_wq, tty_ready_cond, t);')
elif case == 'veol-byte':
    replace('n_tty.c', "            echo(t, out, ctx, c);\n"
                       "            if (t->line_len < N_TTY_LINE_MAX) {\n"
                       "                t->line[t->line_len++] = c;",
            "            echo(t, out, ctx, '\\n');\n"
            "            if (t->line_len < N_TTY_LINE_MAX) {\n"
            "                t->line[t->line_len++] = '\\n';")
elif case == 'line-end':
    replace('n_tty.c', 't->rq_end[(t->rq_tail - 1) & (N_TTY_READ_MAX - 1)] = 1;',
            't->rq_end[(t->rq_tail - 1) & (N_TTY_READ_MAX - 1)] = 0;')
elif case == 'slot-reset':
    replace('n_tty.c', '        t->rq_end[t->rq_tail & (N_TTY_READ_MAX - 1)] = 0;\n', '')
elif case == 'input-kind':
    replace('tty.c', '            t->ready_us = pit_now_us();\n        }\n'
                    '        wq_wake(&t->input_wq, WQ_DATA);',
            '            t->ready_us = pit_now_us();\n        }\n'
            '        wq_wake_all(&t->input_wq);')
elif case == 'echo-kind':
    replace('pty.c', 'p->m_tail != *from ? WQ_DATA : 0u', 'WQ_DATA')
elif case == 'room-kind':
    replace('pty.c', '    if (n > 0) {\n        wq_wake(&p->m_read_wq, WQ_ROOM);',
            '    if (n > 0) {\n        wq_wake_all(&p->m_read_wq);')
elif case == 'output-kind':
    replace('pty.c', '    wake_master(p, &pushed); /* output pushed into m_buf: wake the master */',
            '    wq_wake_all(&p->m_read_wq);')
elif case == 'flush-kind':
    replace('tty.c', '    n_tty_flush(&t->ldisc);\n    wq_wake(t->space_wq, WQ_ROOM);',
            '    n_tty_flush(&t->ldisc);\n    wq_wake_all(t->space_wq);')
else:
    raise SystemExit('tty-mutation: unknown mutation: ' + case)
PY
    touch "$work/tty/"*
}

echo "tty-mutation: logs: $work"
for case_name in split-queue flush termios-flush partial-slave partial-master \
                 latency-lock veol-byte line-end slot-reset input-kind echo-kind \
                 room-kind output-kind flush-kind; do
    test='tty'
    case "$case_name" in
        split-queue) expected='slave read wakes master POLLOUT' ;;
        flush) expected='TCFLSH wakes master writer' ;;
        termios-flush) expected='TCSETSF wakes master writer' ;;
        partial-slave) expected='partial slave output wakes reader' ;;
        partial-master) expected='partial master echo wakes reader' ;;
        latency-lock) expected='console pump excludes input interrupts' ;;
        veol-byte) test=n_tty; expected='VEOL byte and boundary survive termios change' ;;
        line-end) test=n_tty; expected='VEOF boundary precedes a later newline' ;;
        slot-reset) test=n_tty; expected='ring reuse clears old end markers' ;;
        input-kind) expected='input written to the master is data for the slave' ;;
        echo-kind) expected='input written with echo off is no change for the master' ;;
        room-kind) expected='a slave read is room for the master and no data' ;;
        output-kind) expected='slave output is data for the master and no room' ;;
        flush-kind) expected='an input flush is room for the master and no data' ;;
    esac
    run_phase fixed
    mutate
    run_phase mutated
    restore
    run_phase restored
    for source in "$work/saved/"*; do
        cmp "$source" "$work/tty/$(basename "$source")"
    done
done
