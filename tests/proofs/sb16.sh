#!/bin/sh
# sb16.sh: the test of the proof sb16.json, an SB16 boot like the case
# machine.audio.sb16's, run at two speeds on the kernel WORK/kernel, for the
# architecture ARCH names.
#
# The catalogue's test calls it from the tree's root:
#
#   sb16.sh WORK    boot WORK/kernel twice: on the accelerator
#                   RACH2_QEMU_ACCEL names, tcg by default, with the
#                   machine's clock the host's, and under TCG with an
#                   instruction clock of 512 ns an instruction, at which the
#                   guest refills the buffer too late and the device plays
#                   periods of it again
#
# Each boot runs `audio test scale 2000` with the SB16's output recorded to a
# WAV file, and wavcheck judges the recording of a boot that passes. Each
# failure is a line `sb16-proof: fail: SPEED: WHAT`, SPEED `host` or `slow`:
# the boot's status, `AUDIO_START failed.` or `audio: stalled` when the
# console has it, and each line of a recording's verdict, with `notes are not
# what was asked for` when wavcheck found wrong notes. The exit status is 1
# when either boot failed.
set -u
[ "$#" -eq 1 ] || { echo 'sb16-proof: usage: sb16.sh WORK' >&2; exit 2; }
work=$1
failed=0
repo=$(pwd)
RACH2_ARCH=${RACH2_ARCH:-${ARCH:-i686}}
REPO=$repo
. "$repo/mk/arch.sh"
RACH2_QEMU=$ARCH_QEMU
export RACH2_QEMU

# boot SPEED QEMU-ARG...: one boot at SPEED, its files WORK/SPEED.*.
boot()
{
    speed=$1
    shift
    base=$work/$speed
    rm -f "$base.wav"
    status=0
    build/tests/system/harness --kernel "$work/kernel" \
        --append 'rach2.rescue rach2.nonet' --net hubport,id=net0,hubid=0 \
        "$@" --qemu-arg=-cpu --qemu-arg=max \
        --qemu-arg=-device --qemu-arg=sb16,audiodev=au0 \
        --qemu-arg=-audiodev --qemu-arg="wav,id=au0,timer-period=500,path=$base.wav" \
        --log "$base.console.log" --timeout 180 --steptmo 120 \
        --ok 'Done.' --never 'AUDIO_START failed.' --never 'audio: stalled' \
        "expect=Type 'help'" 'send=audio test scale 2000' 'expect=Done.' \
        >"$base.log" 2>&1 </dev/null || status=$?
    cat "$base.log"
    if [ "$status" -ne 0 ]; then
        for line in 'AUDIO_START failed.' 'audio: stalled'; do
            ! grep -qF "$line" "$base.console.log" ||
                echo "sb16-proof: fail: $speed: $line"
        done
        echo "sb16-proof: fail: $speed: the boot, status $status"
        failed=1
        return
    fi
    if ! build/tests/system/wavcheck scale "$base.wav" 262 294 330 349 392 \
        >"$base.wav.log" 2>&1; then
        sed "s/^/sb16-proof: fail: $speed: /" "$base.wav.log"
        ! grep -q 'notes are not' "$base.wav.log" ||
            echo "sb16-proof: fail: $speed: notes are not what was asked for"
        failed=1
        return
    fi
    cat "$base.wav.log"
    echo "sb16-proof: $speed: pass"
}

boot host --qemu-arg=-accel --qemu-arg="${RACH2_QEMU_ACCEL:-tcg}"
boot slow --qemu-arg=-accel --qemu-arg=tcg \
    --qemu-arg=-icount --qemu-arg=shift=9,sleep=off
exit "$failed"
