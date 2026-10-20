#!/bin/sh
# sb16_mutate.sh [--texts]: show that the audio suite's Sound Blaster 16 boot,
# the scale judged from QEMU's PCM recording, catches three compiled faults in
# the SB16 code whatever the guest's speed, with kernels built from a private
# copy for the architecture ARCH names and run on its emulator.
#
# Each kernel boots twice: on the accelerator RACH2_QEMU_ACCEL names, tcg by
# default, with the machine's clock the host's, and under TCG with an
# instruction clock of 512 ns an instruction, at which the guest refills the
# buffer too late and the device plays periods of it again; that boot takes
# some 20 seconds of the host's time to play the scale. `rate` takes the
# SB16's 44100 Hz rate out of audio_debug.c, so `audio test scale` must fail
# to start the audio; `refill` makes sb16.c fill its buffer from the ring and
# never from the callback, so the scale must report a stall; `stereo` leaves
# the stereo bit out of the DSP's mode, so the device plays each channel's
# sample as a frame of its own, and wavcheck must find the tones an octave
# low. For each fault the kernel is built and run as it is, with the fault,
# and restored, and the restored kernel must match the first. --texts checks
# that each fault is once in its source.
set -eu
repo=$(CDPATH='' cd -- "$(dirname -- "$0")/../.." && pwd)
cd "$repo"

# apply_fault SOURCE FAULT: put FAULT into SOURCE; with
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
if sys.argv[2] == "rate":
    old = "    if (audio->type == AUDIO_DRIVER_SB16) {\n" \
          "        sample_rate = 44100;\n    }"
    new = ""
elif sys.argv[2] == "refill":
    old = "    if (user_callback) {\n" \
          "        user_callback(fill_buffer, period_bytes);\n" \
          "    } else {\n" \
          "        ring_read_to_buffer(fill_buffer, period_bytes);\n    }"
    new = "    ring_read_to_buffer(fill_buffer, period_bytes);"
else:
    old = "        mode |= 0x20; /* stereo */"
    new = "        (void)0;"
if s.count(old) != 1:
    raise SystemExit("sb16-mutation: source pattern is not unique")
p.write_text(s.replace(old, new))
PY
}

if [ "${1:-}" = --texts ]; then
    RACH2_FAULT_TEXTS=1
    export RACH2_FAULT_TEXTS
    apply_fault rach2/sys/audio/audio_debug.c rate >/dev/null
    apply_fault rach2/sys/audio/sb16.c refill >/dev/null
    apply_fault rach2/sys/audio/sb16.c stereo >/dev/null
    echo 'sb16-mutation: texts: every fault is once in its source'
    exit 0
fi


. "$repo/tests/system/runlock.sh"
runlock_hold "$repo" sb16-mutation "$repo/tests/system/sb16_mutate.sh" "$@"
RACH2_ARCH=${RACH2_ARCH:-${ARCH:-i686}}
REPO=$repo
. "$repo/mk/arch.sh"
RACH2_QEMU=$ARCH_QEMU
export RACH2_QEMU
for artifact in build/tools/inspect/elfcheck build/tests/system/harness \
    build/tests/system/wavcheck; do
    [ -x "$artifact" ] || { echo "sb16-mutation: missing $artifact" >&2; exit 1; }
done
mkdir -p build/review-proof
work=
source=
child=
keep=
# Stop the running boot, and put back the saved source when a fault is in it.
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

# Restore once the source is saved, then remove the work directory, unless
# the proof failed after printing its path, which sets `keep`; a signal
# clears `keep`.
cleanup()
{
    exit_status=$?
    if [ -z "$source" ] || [ -f "$work/saved.c" ]; then
        restore
    fi
    if [ "$exit_status" -eq 0 ] || [ -z "$keep" ]; then
        rm -rf ${work:+"$work"}
    fi
}
trap cleanup EXIT
trap 'keep=; exit 129' HUP
trap 'keep=; exit 130' INT
trap 'keep=; exit 143' TERM
work=$(mktemp -d "$repo/build/review-proof/sb16.XXXXXX")
cp -R rach2 mk "$work/"
mkdir -p "$work/build/tools/inspect"
cp build/tools/inspect/elfcheck "$work/build/tools/inspect/"
cat > "$work/Makefile" <<'MAKE'
build/tools/inspect/elfcheck:
	@test -x "$@"
MAKE

# run_boot PHASE SPEED: run `audio test scale 2000` on the phase's kernel at
# SPEED, `host` or `slow`, with the SB16's output recorded to a WAV file. With
# the `rate` fault in, the audio must fail to start, and with `refill` the
# scale must stall; otherwise the boot must pass, and wavcheck must fail on
# the recording of a fault and pass on the others.
run_boot()
{
    phase=$1
    speed=$2
    base="$work/$fault-$phase-$speed"
    case "$speed" in
        host) set -- --qemu-arg=-accel --qemu-arg="${RACH2_QEMU_ACCEL:-tcg}" ;;
        slow) set -- --qemu-arg=-accel --qemu-arg=tcg \
            --qemu-arg=-icount --qemu-arg=shift=9,sleep=off ;;
    esac
    build/tests/system/harness --kernel "$work/$fault-$phase.kernel" \
        --append 'rach2.rescue rach2.nonet' --net hubport,id=net0,hubid=0 \
        "$@" --qemu-arg=-cpu --qemu-arg=max \
        --qemu-arg=-device --qemu-arg=sb16,audiodev=au0 \
        --qemu-arg=-audiodev --qemu-arg="wav,id=au0,timer-period=500,path=$base.wav" \
        --log "$base.console.log" --timeout 180 --steptmo 120 \
        --ok 'Done.' --never 'AUDIO_START failed.' --never 'audio: stalled' \
        "expect=Type 'help'" 'send=audio test scale 2000' 'expect=Done.' \
        >"$base.log" 2>&1 &
    child=$!
    status=0
    wait "$child" || status=$?
    child=
    cat "$base.log"
    if [ "$phase" = mutated ] && [ "$fault" = rate ]; then
        [ "$status" -eq 1 ] &&
            grep -q 'AUDIO_START failed.' "$base.console.log" || exit 1
    elif [ "$phase" = mutated ] && [ "$fault" = refill ]; then
        [ "$status" -eq 1 ] &&
            grep -q 'audio: stalled' "$base.console.log" || exit 1
    else
        [ "$status" -eq 0 ] || exit 1
        status=0
        build/tests/system/wavcheck scale "$base.wav" 262 294 330 349 392 \
            >"$base.wav.log" 2>&1 || status=$?
        cat "$base.wav.log"
        if [ "$phase" = mutated ]; then
            [ "$status" -eq 1 ] && grep -q 'notes are not' "$base.wav.log" || exit 1
        else
            [ "$status" -eq 0 ] || exit 1
        fi
    fi
    echo "sb16-mutation: result: $fault $phase $speed status $status"
}

# run_phase PHASE: build the private kernel and boot it at both speeds.
run_phase()
{
    if ! make -C "$work/rach2" "ARCH=$RACH2_ARCH" >"$work/$fault-$1.build.log" 2>&1; then
        cat "$work/$fault-$1.build.log" >&2
        exit 1
    fi
    cp "$work/build$ARCH_SUFFIX/rach2/kernel" "$work/$fault-$1.kernel"
    run_boot "$1" host
    run_boot "$1" slow
}

echo "sb16-mutation: logs: $work"
keep=1
for fault in rate refill stereo; do
    case "$fault" in
        rate) source="$work/rach2/sys/audio/audio_debug.c" ;;
        *) source="$work/rach2/sys/audio/sb16.c" ;;
    esac
    cp "$source" "$work/saved.c"
    run_phase fixed
    apply_fault "$source" "$fault"
    run_phase mutated
    restore
    run_phase restored
    cmp "$work/saved.c" "$source"
    cmp "$work/$fault-fixed.kernel" "$work/$fault-restored.kernel"
    source=
done
