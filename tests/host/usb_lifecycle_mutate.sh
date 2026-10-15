#!/bin/sh
# Prove hub_hosttest.c, usb_interrupt_hosttest.c and msc_hosttest.c with private
# mutations of rach2/sys/usb/core.c, hub.c, usb.c and msc.c.
set -eu

repo=$(CDPATH='' cd -- "$(dirname -- "$0")/../.." && pwd)
cd "$repo"

# apply_fault DIR FAULT: put FAULT into DIR/core.c, hub.c, usb.c or msc.c, the
# file its name begins with; with RACH2_FAULT_TEXTS set, only check its text and
# write nothing.
apply_fault()
{
    python3 - "$1" "$2" <<'PY'
from pathlib import Path
import sys
import os
import pathlib
if os.environ.get('RACH2_FAULT_TEXTS'):
    pathlib.Path.write_text = lambda self, *args, **kwargs: 0

root = Path(sys.argv[1])
fault = sys.argv[2]
path = root / (fault.split('-')[0] + '.c')
source = path.read_text()
changes = {
    'core-halted-gone': (
        'if (d->state == ST_RUNNING || d->state == ST_HALTED) {', 'if (d->state == ST_RUNNING) {'),
    'core-describe-unlocked': (
        '    flags = kirq_save();\n    d = dev_at(index);\n    if (d->state != ST_RUNNING) {\n'
        '        kirq_restore(flags);\n        return 0;',
        '    flags = 0;\n    d = dev_at(index);\n    if (d->state != ST_RUNNING) {\n'
        '        return 0;'),
    'hub-reuse': ('if (!hubs[i]) {', 'if (!hub_at(i)->used) {'),
    'hub-changes': (
        '    p->recheck = 0;\n    p->to_clear = change & change_mask(h);',
        '    p->recheck = 0;\n    p->to_clear = change & change_mask(h);\n'
        '    if ((change & C_PORT_OVER_CURRENT_BIT) && !(status & PORT_STAT_OVER_CURRENT)) {\n'
        '        return;\n    }'),
    'hub-warm-order': (
        '        p->warm_pending = 1;',
        '        if (request(h, TYPE_CLASS_PORT_OUT, REQ_SET_FEATURE, BH_PORT_RESET, port, 0,\n'
        '                    REQ_WARM_RESET, port) != 0) {\n'
        '            p->warm = 0;\n        }'),
    'hub-power-dropped': (
        '                } else if (request(h, TYPE_CLASS_PORT_OUT, REQ_SET_FEATURE, PORT_POWER, i + 1, 0,\n'
        '                                   REQ_POWER_ON, i + 1) == 0) {\n'
        '                    p->power_on_ms = 0;\n                    return;\n                }',
        '                } else {\n                    p->power_on_ms = 0;\n'
        '                    if (request(h, TYPE_CLASS_PORT_OUT, REQ_SET_FEATURE, PORT_POWER, i + 1,\n'
        '                                0, REQ_POWER_ON, i + 1) == 0) {\n'
        '                        return;\n                    }\n                }'),
    'usb-last-source': (
        'for (n = 0; n < interrupt_source_count; n++) {\n        int irq',
        'for (n = interrupt_source_count - 1; n < interrupt_source_count; n++) {\n        int irq'),
    'usb-line-only': (
        '    for (n = 0; n < interrupt_source_count; n++) {\n        int irq',
        '    for (n = 0; n < interrupt_source_count; n++) {\n'
        '        if (interrupt_sources[n].line_irq >= 0) {\n'
        '            return isr_line_pending(interrupt_sources[n].line_irq);\n'
        '        }\n    }\n'
        '    for (n = 0; n < interrupt_source_count; n++) {\n        int irq'),
    'usb-keep-vector': ('            isr_free_vector(vector);\n', ''),
    'msc-elapsed': (
        '    m->ready_left = (m->ready_left > elapsed_ms) ? m->ready_left - elapsed_ms : 0;',
        '    if (m->state == MSC_STATE_TEST && m->stage == STAGE_IDLE) {\n'
        '        m->ready_left = (m->ready_left > elapsed_ms) ? m->ready_left - elapsed_ms : 0;\n'
        '    }'),
    'msc-unit': ('    m->lun++;\n    m->ready_left = MSC_READY_MS;', '    m->lun++;'),
}
old, new = changes[fault]
if source.count(old) != 1:
    raise SystemExit('usb-lifecycle-mutation: source pattern is not unique')
path.write_text(source.replace(old, new))
PY
}

faults='core-halted-gone core-describe-unlocked hub-reuse hub-changes
    hub-warm-order hub-power-dropped usb-last-source usb-line-only usb-keep-vector
    msc-elapsed msc-unit'
if [ "${1:-}" = --texts ]; then
    for fault in $faults; do
        RACH2_FAULT_TEXTS=1 apply_fault rach2/sys/usb "$fault" >/dev/null || exit 1
    done
    echo 'usb-lifecycle-mutation: texts: every fault is once in its source'
    exit 0
fi

. "$repo/tests/system/runlock.sh"
runlock_hold "$repo" usb-lifecycle-mutation "$repo/tests/host/usb_lifecycle_mutate.sh" "$@"

mkdir -p build/tests
work=$(mktemp -d "$repo/build/tests/usb-lifecycle-proof.XXXXXX")
child=
mkdir -p "$work/rach2" "$work/tests/host"
cp -R rach2/sys "$work/rach2/sys"
for name in hub msc usb_interrupt; do
    cp "tests/host/${name}_hosttest.c" "$work/tests/host/"
done
for name in core hub msc usb; do
    cp "$work/rach2/sys/usb/$name.c" "$work/$name.saved"
done

# cleanup: stop the running test and put the saved core.c, hub.c, msc.c and
# usb.c back; the loop calls it after each fault, and it is the exit trap.
cleanup()
{
    if [ -n "$child" ]; then
        kill -TERM "$child" 2>/dev/null || :
        wait "$child" 2>/dev/null || :
        child=
    fi
    for name in core hub msc usb; do
        cp "$work/$name.saved" "$work/rach2/sys/usb/$name.c"
        touch "$work/rach2/sys/usb/$name.c"
    done
}
trap cleanup EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM

# run_phase PHASE TEST STATUS MESSAGE: compile TEST_hosttest.c over the private
# tree as $work/PHASE, run it for at most 10 seconds, and exit unless it ends
# with STATUS and its log contains MESSAGE.
run_phase()
{
    phase=$1
    test=$2
    expected=$3
    message=$4
    binary="$work/$phase"
    if ! "${CC:-cc}" -std=c99 -D_DEFAULT_SOURCE -D_FORTIFY_SOURCE=0 -DRACH2_HOSTTEST \
        -g -O1 -Wno-empty-body -ffreestanding \
        -fsanitize=address,undefined -fno-sanitize-recover=all \
        -I "$work/rach2/sys" -I "$work/rach2/sys/lib" -I "$work/rach2/sys/proc" \
        "$work/tests/host/${test}_hosttest.c" -o "$binary" >"$binary.compile.log" 2>&1; then
        cat "$binary.compile.log" >&2
        exit 1
    fi
    cksum "$work/rach2/sys/usb/core.c" "$work/rach2/sys/usb/hub.c" \
        "$work/rach2/sys/usb/msc.c" "$work/rach2/sys/usb/usb.c" \
        "$work/tests/host/${test}_hosttest.c" "$binary" \
        >"$binary.cksum"
    perl -e 'alarm 10; exec @ARGV or die "usb-lifecycle-mutation: exec: $!\n"' \
        "$binary" >"$binary.log" 2>&1 &
    child=$!
    status=0
    wait "$child" || status=$?
    child=
    cat "$binary.log"
    [ "$status" -eq "$expected" ] && grep -Fq "$message" "$binary.log" || exit 1
    echo "usb-lifecycle-mutation: result: $phase status $status"
}

echo "usb-lifecycle-mutation: logs: $work"
run_phase hub-fixed hub 0 'hub_hosttest: OK'
run_phase usb-fixed usb_interrupt 0 'usb-interrupt-hosttest: result: pass'
run_phase msc-fixed msc 0 'msc_hosttest: OK'
for fault in $faults; do
    apply_fault "$work/rach2/sys/usb" "$fault"
    case $fault in
        core-halted-gone) test=hub; message='pulled while halted, its class driver is told' ;;
        core-describe-unlocked) test=hub; message='and reads the device table with interrupts off' ;;
        hub-reuse) test=hub; message='a detached hub record is not reused' ;;
        hub-changes) test=hub; message='cleared over-current does not suppress another change' ;;
        hub-warm-order) test=hub; message='a simultaneous connection and link fault starts a warm reset' ;;
        hub-power-dropped) test=hub; message='a power-on that cannot be queued is asked for again until it is' ;;
        usb-keep-vector) test=usb_interrupt; message='a failed MSI enable gives its vector back' ;;
        usb-*) test=usb_interrupt; message='a configured pending source prevents fallback' ;;
        msc-elapsed) test=msc; message='initialization expires after ten elapsed seconds' ;;
        msc-unit) test=msc; message='each unit gets a separate elapsed deadline' ;;
    esac
    run_phase "$fault-mutated" "$test" 1 "$message"
    cleanup
    case $test in
        hub) message='hub_hosttest: OK' ;;
        usb_interrupt) message='usb-interrupt-hosttest: result: pass' ;;
        msc) message='msc_hosttest: OK' ;;
    esac
    run_phase "$fault-restored" "$test" 0 "$message"
    for name in core hub msc usb; do
        cmp "$work/$name.saved" "$work/rach2/sys/usb/$name.c"
    done
done
echo 'usb-lifecycle-mutation: result: pass, eleven compiled faults failed and restored tests passed'
