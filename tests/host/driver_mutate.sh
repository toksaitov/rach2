#!/bin/sh
# Prove the dma, sb16, vga_gfx, vga_text, ps2mouse, ps2kbd and power host
# tests with private mutations of rach2/sys/audio/dma.c, sb16.c,
# audio_debug.c, display/vga_gfx.c, vga_text.c and arch/x86/ps2mouse.c,
# ps2kbd.c, power.c and board.c.
#
# Usage: driver_mutate.sh [FAULT...] | --texts
# With no FAULT it proves every fault.
set -eu
repo=$(CDPATH='' cd -- "$(dirname -- "$0")/../.." && pwd)
cd "$repo"

# apply_fault FILE FAULT: put FAULT into FILE, a copy of its source; with
# RACH2_FAULT_TEXTS set, only check its text and write nothing.
apply_fault()
{
    python3 - "$1" "$2" <<'PY'
import pathlib
import sys
import os
if os.environ.get('RACH2_FAULT_TEXTS'):
    pathlib.Path.write_text = lambda self, *args, **kwargs: 0
p = pathlib.Path(sys.argv[1])
s = p.read_text()
fault = sys.argv[2]
if fault == "frames":
    old = "    to_read -= to_read % frame_bytes;"
    new = "    (void)frame_bytes;"
elif fault in ("gfx-enter", "gfx-exit", "cursor"):
    name = {"gfx-enter": "uint8_t *fb_enter(void)",
            "gfx-exit": "void fb_exit(void)",
            "cursor": "void textcon_release(void)"}[fault]
    start = s.index(name + "\n{")
    end = s.index("\n}", start) + 2
    old = s[start:end]
    if old.count("uint32_t flags = kirq_save();") != 1 or "kirq_restore(flags);" not in old:
        raise SystemExit("driver-mutation: the interrupt save is not once in " + name)
    new = old.replace("uint32_t flags = kirq_save();", "uint32_t flags = 0;")
    new = new.replace("kirq_restore(flags);", "(void)flags;")
elif fault == "setup-speaker":
    old = "        goto fail;\n    }\n\n    /* Silence both halves:"
    new = "        (void)0;\n    }\n\n    /* Silence both halves:"
elif fault == "setup-transfer":
    old = "    return dsp_write(dsp_cmd) && dsp_write(mode) && dsp_write(block_size & 0xFF) &&\n" \
          "           dsp_write((block_size >> 8) & 0xFF);"
    new = "    dsp_write(dsp_cmd);\n    dsp_write(mode);\n" \
          "    dsp_write(block_size & 0xFF);\n" \
          "    dsp_write((block_size >> 8) & 0xFF);\n    return true;"
elif fault == "setup-dma":
    old = "        return false;\n    }\n\n    /* DSP 4.xx auto-init:"
    new = "        (void)0;\n    }\n\n    /* DSP 4.xx auto-init:"
elif fault == "callback":
    old = "    if (user_callback) {\n" \
          "        user_callback(fill_buffer, SB16_BUFFER_SIZE);\n" \
          "    } else {\n" \
          "        ring_read_to_buffer(fill_buffer, SB16_BUFFER_SIZE);\n    }"
    new = "    ring_read_to_buffer(fill_buffer, SB16_BUFFER_SIZE);"
elif fault == "mouse-aux":
    old = "    if (!(status & PS2_STATUS_OUTPUT_FULL) || !(status & PS2_STATUS_MOUSE_DATA)) {\n" \
          "        return;\n    }\n    data = inb(PS2_DATA_PORT);\n"
    new = "    if (!(status & PS2_STATUS_OUTPUT_FULL)) {\n" \
          "        return;\n    }\n    data = inb(PS2_DATA_PORT);\n" \
          "    if (!(status & PS2_STATUS_MOUSE_DATA)) {\n        return;\n    }\n"
elif fault == "dma-stop":
    old = "    if (channel > 7 || channel == 4)\n"
    new = "    if (channel > 7)\n"
elif fault == "kbd-aux":
    old = "    if (!(status & PS2_STATUS_OUTPUT_FULL) || (status & PS2_STATUS_MOUSE_DATA)) {\n"
    new = "    if (!(status & PS2_STATUS_OUTPUT_FULL)) {\n"
elif fault == "insyde":
    old = '    return smbios_vendor_is("Insyde");\n'
    new = '    return smbios_vendor_is("Insyd");\n'
elif fault == "pm-answers":
    old = "        if (!board_pm_answers(pm1)) {\n"
    new = "        if (0) {\n"
elif fault == "scale":
    old = "    if (audio->type == AUDIO_DRIVER_SB16) {\n" \
          "        sample_rate = 44100;\n    }"
    new = ""
else:
    raise SystemExit("driver-mutation: unknown fault")
if s.count(old) != 1:
    raise SystemExit("driver-mutation: source pattern is not unique")
p.write_text(s.replace(old, new))
PY
}

# select_fault FAULT: set `test`, `file` (the source under rach2/sys),
# `argument` (the test's argument, or empty) and `failure` (the text its row
# prints) for FAULT.
select_fault()
{
    argument=
    case "$1" in
        frames) test=sb16; file=audio/sb16.c; argument=frames
            failure='partial frame stays queued' ;;
        setup-speaker) test=sb16; file=audio/sb16.c; argument=setup
            failure='failed setup does not start dma early' ;;
        setup-*) test=sb16; file=audio/sb16.c; argument=setup
            failure='failed setup returns failure' ;;
        callback) test=sb16; file=audio/sb16.c
            failure='callback continues after priming' ;;
        dma-stop) test=dma; file=audio/dma.c
            failure='cascade and invalid channels do no I/O' ;;
        scale) test=sb16; file=audio/audio_debug.c
            failure='sb16 scale selects 44100 hz' ;;
        gfx-enter|gfx-exit) test=vga_gfx; file=display/vga_gfx.c
            failure='programs maximum scan line\|text clear excludes interrupts' ;;
        cursor) test=vga_text; file=display/vga_text.c
            failure='echo leaves cursor at 0x0100' ;;
        mouse-aux) test=ps2mouse; file=arch/x86/ps2mouse.c
            failure='keyboard byte leaves the data port unread' ;;
        kbd-aux) test=ps2kbd; file=arch/x86/ps2kbd.c
            failure='mouse byte leaves the data port unread' ;;
        insyde) test=power; file=arch/x86/power.c
            failure='a shorter name was taken for Insyde' ;;
        pm-answers) test=power; file=arch/x86/board.c
            failure='a PIIX4 whose PM1_CNT reads all ones was written' ;;
        *) echo "driver-mutation: unknown fault: $1" >&2; exit 1 ;;
    esac
}

faults='frames gfx-enter gfx-exit setup-speaker setup-transfer setup-dma scale
    callback cursor mouse-aux dma-stop kbd-aux insyde pm-answers'
if [ "${1:-}" = --texts ]; then
    for fault in $faults; do
        select_fault "$fault"
        RACH2_FAULT_TEXTS=1 apply_fault "rach2/sys/$file" "$fault" >/dev/null || exit 1
    done
    echo 'driver-mutation: texts: every fault is once in its source'
    exit 0
fi

. "$repo/tests/system/runlock.sh"
runlock_hold "$repo" driver-mutation "$repo/tests/host/driver_mutate.sh" "$@"

mkdir -p build/review-proof
work=$(mktemp -d "$repo/build/review-proof/drivers.XXXXXX")
child=
source=
cp -R rach2/sys "$work/sys"

# restore: stop the running test and put the saved source back, if a fault is
# in progress; it is also the exit trap.
restore()
{
    if [ -n "$child" ]; then
        kill -TERM "$child" 2>/dev/null || :
        wait "$child" 2>/dev/null || :
        child=
    fi
    if [ -n "$source" ]; then
        cp "$work/saved.c" "$source"
        touch "$source"
    fi
}
trap restore EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM

# run_phase PHASE: compile $test's host test with its own host-deps lines, as
# the host suite reads them, the kernel's sources taken from the private copy,
# and run it for at most 30 seconds. PHASE `mutated` must fail with $failure in
# its log; any other must pass. Exit if not.
run_phase()
{
    phase=$1
    binary="$work/$fault-$phase"
    deps=$(sed -n 's/^ \* host-deps://p' "tests/host/${test}_hosttest.c" |
        sed "s|rach2/sys/|$work/sys/|g")
    # shellcheck disable=SC2086
    if ! "${CC:-cc}" -std=c99 -DRACH2_HOSTTEST -g -O1 -ffreestanding \
        -fsanitize=address,undefined -fno-sanitize-recover=all \
        -I "$work/sys" -I "$work/sys/arch/x86" -I "$work/sys/arch/i686" -I "$work/sys/lib" \
        "tests/host/${test}_hosttest.c" $deps -o "$binary" \
        >"$binary.compile.log" 2>&1; then
        cat "$binary.compile.log" >&2
        exit 1
    fi
    perl -e 'alarm 30; exec @ARGV or die "driver-mutation: exec: $!\n"' \
        "$binary" ${argument:+"$argument"} >"$binary.log" 2>&1 &
    child=$!
    status=0
    wait "$child" || status=$?
    child=
    cat "$binary.log"
    if [ "$phase" = mutated ]; then
        [ "$status" -eq 1 ] && grep -q "$failure" "$binary.log" || exit 1
    else
        [ "$status" -eq 0 ] || exit 1
    fi
    echo "driver-mutation: result: $fault $phase status $status"
}

if [ "$#" -eq 0 ]; then
    # shellcheck disable=SC2086 # the faults are separate words
    set -- $faults
fi
echo "driver-mutation: logs: $work"
for fault do
    select_fault "$fault"
    source="$work/sys/$file"
    cp "$source" "$work/saved.c"
    run_phase fixed
    apply_fault "$source" "$fault"
    run_phase mutated
    restore
    run_phase restored
    cmp "$work/saved.c" "$source"
    source=
done
