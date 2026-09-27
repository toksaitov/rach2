#!/bin/sh
# Compile private kernels and judge the scale from QEMU's PCM recording, made
# under an instruction clock, as the audio suite's row is, so that the notes'
# lengths do not depend on the host.
set -eu
repo=$(CDPATH='' cd -- "$(dirname -- "$0")/../.." && pwd)
cd "$repo"
for artifact in build/tools/inspect/elfcheck build/tests/system/harness \
    build/tests/system/wavcheck; do
    [ -x "$artifact" ] || { echo "sb16-mutation: missing $artifact" >&2; exit 1; }
done
mkdir -p build/review-proof
work=$(mktemp -d "$repo/build/review-proof/sb16.XXXXXX")
cp -R rach2 mk "$work/"
mkdir -p "$work/build/tools/inspect"
cp build/tools/inspect/elfcheck "$work/build/tools/inspect/"
cat > "$work/Makefile" <<'MAKE'
build/tools/inspect/elfcheck:
	@test -x "$@"
MAKE
source=
child=
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

run_phase()
{
    phase=$1
    base="$work/$fault-$phase"
    if ! make -C "$work/rach2" >"$base.build.log" 2>&1; then
        cat "$base.build.log" >&2
        exit 1
    fi
    cp "$work/build/rach2/kernel" "$base.kernel"
    RACH2_QEMU_ACCEL=tcg build/tests/system/harness --kernel "$base.kernel" \
        --append 'rach2.rescue rach2.nonet' --net hubport,id=net0,hubid=0 \
        --qemu-arg=-cpu --qemu-arg=max \
        --qemu-arg=-icount --qemu-arg=shift=3,sleep=off \
        --qemu-arg=-device --qemu-arg=sb16,audiodev=au0 \
        --qemu-arg=-audiodev --qemu-arg="wav,id=au0,timer-period=500,path=$base.wav" \
        --log "$base.console.log" --timeout 120 \
        --ok 'Done.' --never 'AUDIO_START failed.' \
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
    echo "sb16-mutation: result: $fault $phase status $status"
}

echo "sb16-mutation: logs: $work"
for fault in rate refill; do
    case "$fault" in
        rate) source="$work/rach2/sys/audio/audio_debug.c" ;;
        refill) source="$work/rach2/sys/audio/sb16.c" ;;
    esac
    cp "$source" "$work/saved.c"
    run_phase fixed
    python3 - "$source" "$fault" <<'PY'
import pathlib
import sys
p = pathlib.Path(sys.argv[1])
s = p.read_text()
if sys.argv[2] == "rate":
    old = "    if (audio->type == AUDIO_DRIVER_SB16) {\n" \
          "        sample_rate = 44100;\n    }"
    new = ""
else:
    old = "    if (user_callback) {\n" \
          "        user_callback(fill_buffer, SB16_BUFFER_SIZE);\n" \
          "    } else {\n" \
          "        ring_read_to_buffer(fill_buffer, SB16_BUFFER_SIZE);\n    }"
    new = "    ring_read_to_buffer(fill_buffer, SB16_BUFFER_SIZE);"
if s.count(old) != 1:
    raise SystemExit("sb16-mutation: source pattern is not unique")
p.write_text(s.replace(old, new))
PY
    run_phase mutated
    restore
    run_phase restored
    cmp "$work/saved.c" "$source"
    cmp "$work/$fault-fixed.kernel" "$work/$fault-restored.kernel"
    source=
done
