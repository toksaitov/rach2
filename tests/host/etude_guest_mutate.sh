#!/bin/sh
# Prove etude_guest_hosttest.c with faults in private copies of etude-guest:
# a repeated command run again, a cancellation reported as a signal, an
# acknowledgement that releases nothing, an exec that runs the program as a
# child, an output digest that misses the bytes sent, a stream's total that
# misses the bytes not kept, an orphaned attempt settled as stopped, an
# orphaned attempt's group judged at once after SIGKILL, a run's record
# given the end it lacks only for an attempt settled at the lock, a run's
# record given that end after a line cut short, an exit that carries both
# streams' sizes and digests in one frame, ids not checked against the
# accounts and the processes, on macOS ids not looked up in Directory
# Services, the ids left out of the program's arguments, a
# pseudo-terminal whose output is not read, and an attempts' directory that
# only root may search. Each must fail the test, and the restored copy must
# pass it.
set -eu
repo=$(CDPATH='' cd -- "$(dirname -- "$0")/../.." && pwd)
cd "$repo"

# Change each unique text of a fault in a private source, or just locate it.
apply_fault()
{
    python3 - "$1" "$2" <<'PY'
from pathlib import Path
import os
import sys
path = Path(sys.argv[1])
source = path.read_text()
faults = {
    'repeat': ('    if (read_file(path, buf, sizeof(buf)) < 0) {\n        char line0',
               '    if (1) {\n        char line0'),
    'stopped': ('    if (a->stopped)\n        snprintf(end,',
                '    if (0)\n        snprintf(end,'),
    'ack': ('    while (queue_head && queue_head->sequenced && queue_head->seq <= ack &&',
            '    while (0 && queue_head && queue_head->sequenced && queue_head->seq <= ack &&'),
    'exec': ('    execve(exec_next.prog, exec_next.argv, exec_next.envp);\n    _exit(127);',
             '    if (fork() == 0)\n        execve(exec_next.prog, exec_next.argv, exec_next.envp);\n'
             '    _exit(0);'),
    'digest': ('    sha256_update(&a->sent[fdn], buf, (size_t)r);\n', ''),
    'total': ('    a->total[fdn] += (unsigned long long)r;\n', ''),
    'orphan': ('"%s %s %s lost -\\n', '"%s %s %s stopped -\\n'),
    'immediate': ('    gone = child <= 0 || group_ended((pid_t)child, killed ? SETTLE_US : 0);\n',
                  '    gone = child <= 0 || (kill(-(pid_t)child, 0) < 0 && errno == ESRCH);\n'
                  '    (void)killed;\n    (void)group_ended;\n'),
    'complete': ('        settle_orphan(e->d_name, buf);\n        complete_run(cmd, buf);\n',
                 '        if (settle_orphan(e->d_name, buf))\n            complete_run(cmd, buf);\n'),
    'torn': ("    if (last[1] != '\\0') {", '    if (0) {'),
    'idcheck': ('    for (i = 0; i < fx->nids; i++)\n        if (listed(',
                '    for (i = 0; i < 0; i++)\n        if (listed('),
    'idargs': ('        pair_arg(arg, "ids", fx->id_old, fx->id_new, fx->nids);\n        argv[n++] = strdup(arg);\n',
               ''),
    'directory': ('    if (getpwuid((uid_t)id) || getgrgid((gid_t)id))\n        return 1;\n', ''),
    'search': ('{0711, 0711, 0700, 0700}', '{0700, 0700, 0700, 0700}'),
    'pty': ('    if (a->fx.pty)\n        take_terminal(a, inv->stdin_name ? input : NULL);\n', ''),
    'split': [('#include "wire.h"\n',
               '#include "wire.h"\n#undef WIRE_FIELDS_MAX\n#define WIRE_FIELDS_MAX 10\n'),
              ('    snprintf(line[0], LINE_CAP, "%s %s %s %s", ETUDE_EV_EXIT, a->cmd, a->token, end);\n'
               '    stream_event(line[1], LINE_CAP, a, 1);\n'
               '    stream_event(line[2], LINE_CAP, a, 2);\n',
               '    stream_event(line[1], LINE_CAP, a, 1);\n'
               '    stream_event(line[2], LINE_CAP, a, 2);\n'
               '    snprintf(line[0], LINE_CAP, "%s %s %s %s %s %s", ETUDE_EV_EXIT, a->cmd, a->token,\n'
               '             end, line[1] + strlen(a->cmd) + 27, line[2] + strlen(a->cmd) + 27);\n')],
}
pairs = faults[sys.argv[2]]
if isinstance(pairs, tuple):
    pairs = [pairs]
for old, new in pairs:
    if source.count(old) != 1:
        raise SystemExit('etude-guest-mutation: fault text is not unique')
    source = source.replace(old, new)
if not os.environ.get('RACH2_FAULT_TEXTS'):
    path.write_text(source)
PY
}

if [ "${1:-}" = --texts ]; then
    RACH2_FAULT_TEXTS=1
    export RACH2_FAULT_TEXTS
    for fault in repeat stopped ack exec digest total orphan immediate complete torn split pty \
        search; do
        apply_fault tools/etude/guest/guest.c "$fault"
    done
    for fault in idcheck idargs directory; do
        apply_fault tools/etude/guest/fixture.c "$fault"
    done
    echo 'etude-guest-mutation: texts: every fault found once'
    exit 0
fi
. "$repo/tests/system/runlock.sh"
runlock_hold "$repo" etude-guest-mutation "$repo/tests/host/etude_guest_mutate.sh" "$@"
work=
child=

# Stop a test before removing its private sources and evidence.
cleanup()
{
    status=$?
    if [ -n "$child" ]; then
        kill -TERM "$child" 2>/dev/null || :
        wait "$child" 2>/dev/null || :
    fi
    if [ "$status" -eq 0 ] || [ -n "${stopped:-}" ]; then
        [ -z "$work" ] || rm -rf "$work"
    else
        echo "etude-guest-mutation: evidence: $work" >&2
    fi
}
trap cleanup EXIT
trap 'stopped=1; exit 129' HUP
trap 'stopped=1; exit 130' INT
trap 'stopped=1; exit 143' TERM
work=$(mktemp -d "${TMPDIR:-/tmp}/rach2-etude-guest-proof.XXXXXX")
cp tools/etude/guest/guest.c "$work/guest.c"
cp tools/etude/guest/fixture.c "$work/fixture.c"

# Build the real host test against the private guest.c and run it under the
# sanitizers; its status must be `expected`.
run_phase()
{
    phase=$1
    expected=$2
    "${CC:-cc}" -std=c99 -D_DEFAULT_SOURCE -D_FORTIFY_SOURCE=0 -DRACH2_HOSTTEST \
        -g -O1 -fsanitize=address,undefined -fno-sanitize-recover=all \
        -I tools/etude/include -I tools/etude -I tools/etude/guest -I utils \
        tests/host/etude_guest_hosttest.c "$work/guest.c" "$work/fixture.c" \
        tools/etude/guest/wire.c \
        tools/etude/history_json.c utils/sha256.c \
        -o "$work/$phase" >"$work/$phase.compile.log" 2>&1 || {
            cat "$work/$phase.compile.log" >&2; exit 1;
        }
    status=0
    perl -e 'alarm 120; exec @ARGV or die "etude-guest-mutation: exec: $!\n"' \
        "$work/$phase" >"$work/$phase.log" 2>&1 &
    child=$!
    wait "$child" || status=$?
    child=
    cat "$work/$phase.log"
    [ "$status" -eq "$expected" ] || exit 1
    echo "etude-guest-mutation: result: $phase status $status"
}

# fault NAME TEXT [FILE]: the test fails with the fault, made in the private
# copy of FILE, guest.c unless given, and names the broken contract in its
# log.
fault()
{
    cp tools/etude/guest/guest.c "$work/guest.c"
    cp tools/etude/guest/fixture.c "$work/fixture.c"
    apply_fault "$work/${3:-guest.c}" "$1"
    run_phase "$1-mutated" 1
    grep -q "$2" "$work/$1-mutated.log"
}
run_phase fixed 0
fault repeat 'a repeated run is answered by the same events'
fault stopped 'a cancelled attempt is stopped and cleaned'
fault ack 'an acknowledged frame is not sent again'
fault exec "the program's exit is the process's"
fault digest 'the streams give the sizes and digests of the output sent'
fault total 'each stream gives the bytes it carried beyond those sent'
fault orphan "an orphaned attempt's end is lost"
fault immediate "an orphaned attempt's cleanup is confirmed once its processes have ended"
fault complete 'a run whose record lost part of its end is answered with the whole end'
fault torn 'a run whose record lost part of its end is answered with the whole end'
fault split 'the end of a long attempt comes whole on the serial console'
fault idcheck 'an id that a process has is refused' fixture.c
fault idargs 'the program is given its ids, ports and peer' fixture.c
# Directory Services are macOS's alone.
if [ "$(uname -s)" = Darwin ]; then
    fault directory 'an id that Directory Services alone has is refused' fixture.c
fi
fault pty 'a case on a pseudo-terminal reads its input there'
fault search "every user may search the attempts' directory"
cp tools/etude/guest/guest.c "$work/guest.c"
cp tools/etude/guest/fixture.c "$work/fixture.c"
run_phase restored 0
