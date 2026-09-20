#!/bin/sh
# Compile fixed, faulty and restored HDA drivers against the model codec.
set -eu

repo=$(CDPATH='' cd -- "$(dirname -- "$0")/../.." && pwd)
cd "$repo"
mkdir -p build/tests
work=$(mktemp -d "$repo/build/tests/hda-proof.XXXXXX")
child=
cp -R rach2/sys "$work/sys"
cp "$work/sys/audio/hda.c" "$work/hda.saved"
cp "$work/sys/fs/procfs.c" "$work/procfs.saved"

cleanup()
{
    if [ -n "$child" ]; then
        kill -TERM "$child" 2>/dev/null || :
        wait "$child" 2>/dev/null || :
        child=
    fi
    cp "$work/hda.saved" "$work/sys/audio/hda.c"
    cp "$work/procfs.saved" "$work/sys/fs/procfs.c"
    touch "$work/sys/audio/hda.c" "$work/sys/fs/procfs.c"
}
trap cleanup EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM

run_phase()
{
    phase=$1
    expected=$2
    message=$3
    binary="$work/$phase"
    if ! "${CC:-cc}" -std=c99 -D_DEFAULT_SOURCE -D_FORTIFY_SOURCE=0 \
        -DRACH2_HOSTTEST -g -O1 -ffreestanding -Wno-empty-body \
        -fsanitize=address,undefined -fno-sanitize-recover=all \
        -I "$work/sys" -I "$work/sys/lib" -I "$work/sys/proc" \
        tests/host/hda_hosttest.c "$work/sys/lib/kstring.c" "$work/sys/lib/kctypes.c" \
        -o "$binary" >"$binary.compile.log" 2>&1; then
        cat "$binary.compile.log" >&2
        exit 1
    fi
    cksum "$work/sys/audio/hda.c" "$work/sys/fs/procfs.c" "$binary" >"$binary.cksum"
    perl -e 'alarm 10; exec @ARGV or die "hda-mutation: exec: $!\n"' \
        "$binary" >"$binary.log" 2>&1 &
    child=$!
    status=0
    wait "$child" || status=$?
    child=
    cat "$binary.log"
    [ "$status" -eq "$expected" ] && grep -Fq "$message" "$binary.log" || exit 1
    echo "hda-mutation: result: $phase status $status"
}

echo "hda-mutation: logs: $work"
run_phase fixed 0 'hda-test: result: pass'
for fault in rate32 rate96 capability inheritance fallback shortoffset longoffset width range route report underrun phase; do
    python3 - "$work/sys" "$fault" <<'PY'
import pathlib
import sys

root = pathlib.Path(sys.argv[1])
fault = sys.argv[2]
path = root / ("fs/procfs.c" if fault == "report" else "audio/hda.c")
source = path.read_text()
changes = {
    "rate32": ("{32000, 0x0A00}", "{32000, 0x4000}"),
    "rate96": ("{96000, 0x0800}", "{96000, 0x0000}"),
    "capability": ("hda_rates[i].rate == sample_rate && (pcm & (1u << i))", "hda_rates[i].rate == sample_rate"),
    "inheritance": ("(widgets & (1u << 4)) ? nid : audio_fg_nid", "nid"),
    "fallback": ("if (pcm & (1u << i)) {", "if ((pcm & (1u << i)) && i != 7 && i != 9 && i != 11) {"),
    "shortoffset": ("HDA_VERB_GET_CONN_LIST | i", "HDA_VERB_GET_CONN_LIST | (bits == 8 ? i / per_word : i)"),
    "longoffset": ("HDA_VERB_GET_CONN_LIST | i", "HDA_VERB_GET_CONN_LIST | (bits == 16 ? i / per_word : i)"),
    "width": ("node = entry & (range_bit - 1);", "node = entry & 0xFF;"),
    "range": ("first = (uint32_t)previous + 1;", "first = node;"),
    "route": ("HDA_VERB_SET_CONN_SEL | pin_conn_sel);", "HDA_VERB_SET_CONN_SEL | 0);"),
    "report": ('audio_field(t, " programmed ", r->programmed_rate);', 'audio_field(t, " programmed ", 0);'),
    "underrun": ("                frac &= 0xFFFFu;\n", ""),
    "phase": ("                frac &= 0xFFFFu;", "                frac = 0;"),
}
old, new = changes[fault]
if source.count(old) != 1:
    raise SystemExit("hda-mutation: source pattern is not unique: " + fault)
path.write_text(source.replace(old, new))
PY
    case $fault in
        rate32|rate96|fallback) row=rates ;;
        capability|inheritance) row=capabilities ;;
        shortoffset) row=short-offset ;;
        longoffset) row=long-offset ;;
        width) row=long-width ;;
        range) row=short-range ;;
        route) row=route ;;
        report) row=proc-audio ;;
        underrun|phase) row=underrun ;;
    esac
    run_phase "$fault-mutated" 1 "hda-test: fail: $row:"
    cleanup
    run_phase "$fault-restored" 0 'hda-test: result: pass'
    cmp "$work/hda.saved" "$work/sys/audio/hda.c"
    cmp "$work/procfs.saved" "$work/sys/fs/procfs.c"
done
echo 'hda-mutation: result: pass: all compiled faults failed their named rows'
