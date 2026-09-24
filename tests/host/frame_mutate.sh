#!/bin/sh
# Prove the Lenovo's row of tests/host/frame_hosttest.c: the memory map
# its boot of 2026-09-12 printed, with the four numbers it printed.
#
# Usage: frame_mutate.sh [--check]
# With --check it compiles and runs the unmodified test alone. The fault
# counts every map type but "reserved" as memory, so the Lenovo's NVS and
# ACPI ranges become usable. QEMU's maps and the synthetic ones have only
# the two types the header defines, so the Lenovo's checks of its usable
# figure, of the first NVS and ACPI pages and of its managed figure must be
# the four that fail, and no other.
set -eu
repo=$(CDPATH='' cd -- "$(dirname -- "$0")/../.." && pwd)
cd "$repo"
[ "$#" -eq 0 ] || [ "$*" = --check ] || {
    echo 'frame-proof: usage: frame_mutate.sh [--check]' >&2
    exit 2
}
mkdir -p build/tests
work=$(mktemp -d "$repo/build/tests/frame-proof.XXXXXX")
child=
# frame.c reaches the vendored Multiboot header by a relative path.
mkdir "$work/rach2"
cp -R rach2/sys rach2/vendor "$work/rach2/"
source="$work/rach2/sys/mm/frame.c"
cp "$source" "$work/frame.saved"

cleanup()
{
    if [ -n "$child" ]; then
        kill -TERM "$child" 2>/dev/null || :
        wait "$child" 2>/dev/null || :
        child=
    fi
    cp "$work/frame.saved" "$source"
    touch "$source"
}
trap cleanup EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM

test=tests/host/frame_hosttest.c
deps=$(grep -oE 'host-deps:[^*]*' "$test" | sed 's/host-deps://' | tr '\n' ' ' |
    sed "s# rach2/# $work/rach2/#g")

# The lines the test prints for the checks the fault must fail, found by
# the checks' text, so the proof follows the test when its lines move.
expected_failures()
{
    for check in 'CHECK(m.usable_bytes == KIB(2787701));' \
        'CHECK(!frame_map_usable(&m, 0xAAEBFu));' \
        'CHECK(!frame_map_usable(&m, 0xAAFBFu));' \
        'CHECK(frame_map_managed(&m) == KIB(2780712));'; do
        [ "$(grep -cF "$check" "$test")" -eq 1 ] || {
            echo "frame-proof: the check is not unique: $check" >&2
            exit 1
        }
        echo "frame_hosttest: line $(grep -nF "$check" "$test" | cut -d: -f1)"
    done
}

# run_phase NAME: compile the test against the private tree and run it;
# `status` is its exit status and NAME.log what it printed.
run_phase()
{
    phase=$1
    binary="$work/$phase"
    # shellcheck disable=SC2086
    if ! "${CC:-cc}" -std=c99 -D_DEFAULT_SOURCE -D_FORTIFY_SOURCE=0 -DRACH2_HOSTTEST \
        -fsanitize=address,undefined -fno-sanitize-recover=all -g -O1 \
        -Wno-empty-body -ffreestanding \
        -I "$work/rach2/sys" -I "$work/rach2/sys/lib" -I "$work/rach2/sys/proc" \
        -I utils -I krffs -I tests/host/compat \
        -o "$binary" "$test" $deps >"$binary.compile.log" 2>&1; then
        cat "$binary.compile.log" >&2
        exit 1
    fi
    cksum "$source" "$binary" >"$binary.cksum"
    perl -e 'alarm 10; exec @ARGV or die "frame-proof: exec: $!\n"' \
        "$binary" >"$binary.log" 2>&1 &
    child=$!
    status=0
    wait "$child" || status=$?
    child=
    cat "$binary.log"
    echo "frame-proof: result: $phase status $status"
}

echo "frame-proof: logs: $work"
run_phase fixed
[ "$status" -eq 0 ] && [ ! -s "$work/fixed.log" ] || exit 1
[ "$#" -eq 0 ] || exit 0
python3 - "$source" <<'PY'
from pathlib import Path
import sys

path = Path(sys.argv[1])
source = path.read_text()
for old, new in (
        ('if (type != MULTIBOOT_MEMORY_AVAILABLE) {', 'if (type == MULTIBOOT_MEMORY_RESERVED) {'),
        ('if (type == MULTIBOOT_MEMORY_AVAILABLE) {', 'if (type != MULTIBOOT_MEMORY_RESERVED) {')):
    if source.count(old) != 1:
        raise SystemExit('frame-proof: the pattern is not unique: ' + old)
    source = source.replace(old, new)
path.write_text(source)
PY
touch "$source"
run_phase acpi-nvs-as-ram
[ "$status" -eq 4 ] && [ "$(cat "$work/acpi-nvs-as-ram.log")" = "$(expected_failures)" ] || exit 1
cleanup
run_phase restored
[ "$status" -eq 0 ] && [ ! -s "$work/restored.log" ] || exit 1
cmp "$work/frame.saved" "$source"
echo 'frame-proof: result: pass: the fault failed the four checks of the Lenovo row alone and the restored source passed'
