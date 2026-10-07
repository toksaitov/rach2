#!/bin/sh
# Rebuild fixed, faulty and restored userland sources in a private tree.
set -eu
repo=$(CDPATH='' cd -- "$(dirname -- "$0")/../.." && pwd)
cd "$repo"

# apply_fault ARGS...: put one fault into its source; with
# RACH2_FAULT_TEXTS set, only check its text and write nothing.
apply_fault()
{
    python3 - "$1" "$2" <<'PY'
import pathlib
import sys
import os
if os.environ.get('RACH2_FAULT_TEXTS'):
    pathlib.Path.write_text = lambda self, *args, **kwargs: 0

root = pathlib.Path(sys.argv[1])
mutation = sys.argv[2]

def replace(file, before, after, count=1):
    path = root / file
    source = path.read_text()
    if source.count(before) != count:
        sys.exit("userland-mutation: source no longer matches: " + file)
    path.write_text(source.replace(before, after))

music = "ports/doom/i_oplmusic.c"
if mutation.startswith("midi-"):
    path = root / "ports/doom/midifile.c"
    before = "if (!ReadByte(&b, stream) || (b & 0x80) != 0)"
    parts = path.read_text().split(before)
    if len(parts) != 3:
        sys.exit("userland-mutation: channel reads no longer match")
    reads = [before, before]
    reads[mutation == "midi-second"] = "if (!ReadByte(&b, stream))"
    path.write_text(parts[0] + reads[0] + parts[1] + reads[1] + parts[2])
elif mutation == "genmidi":
    replace(music, """if (lumpnum < 0
     || W_LumpLength(lumpnum) < 8
        + (GENMIDI_NUM_INSTRS + GENMIDI_NUM_PERCUSSION)
          * (int)sizeof(genmidi_instr_t))""", "if (lumpnum < 0)")
elif mutation == "dns":
    replace("utils/resolv.c", """    if (connect(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
        close(fd);
        return -1;
    }
""", "")
elif mutation == "redirection":
    replace("ish/ish.c", "stdout_descriptor >= 0 && stdout_descriptor != 1", "stdout_descriptor > 1")
elif mutation == "environment":
    for key in ("Home", "Path"):
        replace("ish/ish.c", '"' + key.upper() + '="', '"' + key.upper() + '"')
        replace("ish/ish.c", "sizeof(Environment_Variable_" + key + ") - 1",
                "sizeof(Environment_Variable_" + key + ")")
elif mutation == "environment-case":
    replace("ish/ish_shell_utilities.c", "ish_cstring_starts_with(key, *variable)",
            "ish_cstring_starts_with_ignoring_case(key, *variable)")
elif mutation == "tempo":
    replace(music, "                         / ((uint64_t)ticks_per_beat * 1000000);",
            "                         / (ticks_per_beat * 1000000);")
elif mutation == "tempo-minimum":
    replace(music, """        if (samples_per_tick == 0)
        {
            samples_per_tick = 1;
        }
""", "")
elif mutation == "loop":
    replace(music, "if (song_looping && !restarted)",
            "if (song_looping && !restarted && samples_generated > 0)")
PY
}

faults='midi-first midi-second genmidi dns redirection environment
    environment-case tempo tempo-minimum loop'
if [ "${1:-}" = --texts ]; then
    for mutation in $faults; do
        RACH2_FAULT_TEXTS=1 apply_fault . "$mutation" >/dev/null || exit 1
    done
    echo 'userland-mutation: texts: every fault is once in its source'
    exit 0
fi

. "$repo/tests/system/runlock.sh"
runlock_hold "$repo" userland-mutation "$repo/tests/host/userland_mutate.sh" "$@"

mkdir -p build
work=$(mktemp -d "$repo/build/userland-mutation.XXXXXX")
mkdir -p "$work/tests/host" "$work/tests/musl" "$work/ports" "$work/saved"
cp -R ish utils "$work/"
cp -R ports/doom "$work/ports/"
cp tests/host/oplmusic_hosttest.c tests/host/resolv_hosttest.c \
    tests/host/ish_hosttest.c "$work/tests/host/"
cp tests/musl/ishmusl.c "$work/tests/musl/"
sources="ports/doom/midifile.c ports/doom/i_oplmusic.c utils/resolv.c ish/ish.c ish/ish_shell_utilities.c"
for source in $sources; do
    mkdir -p "$work/saved/$(dirname "$source")"
    cp "$work/$source" "$work/saved/$source"
done
child=

restore()
{
    for source in $sources; do
        cp "$work/saved/$source" "$work/$source"
        touch "$work/$source"
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
    binary="$work/$mutation-$phase"
    case "$mutation" in
        midi-*|genmidi|tempo*|loop)
            set -- -I "$work/ports/doom" -I "$work/ports/doom/src" -DNORMALUNIX \
                -Wno-implicit-function-declaration "$work/tests/host/oplmusic_hosttest.c" \
                "$work/ports/doom/i_oplmusic.c" "$work/ports/doom/midifile.c" \
                "$work/ports/doom/mus2mid.c" "$work/ports/doom/opl3.c" \
                "$work/ports/doom/src/memio.c"
            ;;
        dns) set -- "$work/tests/host/resolv_hosttest.c" ;;
        *) set -- -DISH_USE_STDLIB "$work/tests/host/ish_hosttest.c" \
                "$work/ish/ish_shell_utilities.c" "$work/ish/ish_cstring_utilities.c" ;;
    esac
    if ! "${CC:-cc}" -std=c99 -D_DEFAULT_SOURCE -D_FORTIFY_SOURCE=0 -DRACH2_HOSTTEST \
        -g -O1 -ffreestanding -fsanitize=address,undefined -fno-sanitize-recover=all \
        "$@" -o "$binary" > "$binary.compile.log" 2>&1; then
        cat "$binary.compile.log" >&2
        exit 1
    fi
    python3 - "$binary" > "$binary.log" 2>&1 <<'PY' &
import os
import signal
import subprocess
import sys

process = None

def stop(signum=None, frame=None):
    if process is not None:
        try:
            os.killpg(process.pid, signal.SIGTERM)
            process.wait(timeout=2)
        except (ProcessLookupError, subprocess.TimeoutExpired):
            pass
        try:
            os.killpg(process.pid, signal.SIGKILL)
        except ProcessLookupError:
            pass
        process.wait()
    sys.exit(124 if signum is None else 128 + signum)

signal.signal(signal.SIGTERM, stop)
signal.signal(signal.SIGINT, stop)
signal.signal(signal.SIGHUP, stop)
process = subprocess.Popen([sys.argv[1]], start_new_session=True)
try:
    result = process.wait(timeout=20)
except subprocess.TimeoutExpired:
    stop()
sys.exit(result if result >= 0 else 128 - result)
PY
    child=$!
    status=0
    wait "$child" || status=$?
    child=
    if [ "$phase" = mutated ]; then
        if [ "$status" -eq 0 ] || [ "$status" -ge 124 ] ||
            ! grep -q "$expected" "$binary.log"; then
            cat "$binary.log" >&2
            echo "userland-mutation: unexpected result: $mutation $phase status $status" >&2
            exit 1
        fi
    elif [ "$status" -ne 0 ]; then
        cat "$binary.log" >&2
        exit 1
    fi
    echo "userland-mutation: result: $mutation $phase status $status"
}

echo "userland-mutation: logs: $work"
for mutation in $faults; do
    case "$mutation" in
        midi-*) expected='channel parameters must be seven bits' ;;
        genmidi) expected='a truncated instrument table is rejected' ;;
        dns) expected='a foreign reply cannot decide resolution' ;;
        redirection) expected='output redirection lost a standard descriptor' ;;
        environment*) expected='home matched a prefix or a different case' ;;
        tempo*) expected='tempo preserves the expected sample duration' ;;
        loop) expected='the boundary replays the delta-zero note' ;;
    esac
    run_phase fixed
    apply_fault "$work" "$mutation"
    run_phase mutated
    restore
    run_phase restored
    for source in $sources; do
        cmp "$work/saved/$source" "$work/$source"
    done
done
