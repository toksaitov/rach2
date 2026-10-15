#!/bin/sh
# Prove the rows of tests/host/frame_hosttest.c with four faults in a
# private copy of mm/frame.c and mm/frame.h, each of which must fail the
# checks named for it and no other:
#
#   acpi-nvs-as-ram  counts every map type but "reserved" as memory, so the
#                    NVS and ACPI ranges of the Lenovo's map and of the UEFI
#                    map become usable.
#   reach-32         drops what lies at or above 4 GiB on x86-64 too, where
#                    the allocator takes every usable range.
#   ranges-short     keeps one range fewer than FRAME_MAP_RANGES says, which
#                    the bound's row needs every one of.
#   place-low        puts the frame table at the bottom of its range instead
#                    of the top.
#
# Usage: frame_mutate.sh [--check | --texts]
# With --check it compiles and runs the unmodified test alone. With --texts
# it checks that each fault's text is once in the tree's source, and builds
# and runs nothing.
set -eu
repo=$(CDPATH='' cd -- "$(dirname -- "$0")/../.." && pwd)
cd "$repo"
texts=
[ "${1:-}" != --texts ] || texts=1
if [ -z "$texts" ]; then
    . "$repo/tests/system/runlock.sh"
    runlock_hold "$repo" frame-proof "$repo/tests/host/frame_mutate.sh" "$@"
fi
[ "$#" -eq 0 ] || [ "$*" = --check ] || [ "$*" = --texts ] || {
    echo 'frame-proof: usage: frame_mutate.sh [--check | --texts]' >&2
    exit 2
}
child=
if [ -n "$texts" ]; then
    work=
    source=rach2/sys/mm/frame.c
    header=rach2/sys/mm/frame.h
else
    mkdir -p build/tests
    work=$(mktemp -d "$repo/build/tests/frame-proof.XXXXXX")
    # frame.h reaches boot.h by a relative path.
    mkdir "$work/rach2"
    cp -R rach2/sys "$work/rach2/"
    source="$work/rach2/sys/mm/frame.c"
    header="$work/rach2/sys/mm/frame.h"
    cp "$source" "$work/frame.c.saved"
    cp "$header" "$work/frame.h.saved"
fi

# restore: copy the saved frame.c and frame.h back over the private tree.
restore()
{
    cp "$work/frame.c.saved" "$source"
    cp "$work/frame.h.saved" "$header"
    touch "$source" "$header"
}

# cleanup: the exit trap; stop the running test and restore the sources.
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

test=tests/host/frame_hosttest.c
deps=$(grep -oE 'host-deps:[^*]*' "$test" | sed 's/host-deps://' | tr '\n' ' ' |
    sed "s# rach2/# $work/rach2/#g")

# expected CHECK...: the lines the test prints for the checks given by their
# text, in the order the test runs them; a check inside a loop prints once
# per pass that fails, so it is given once per failing pass.
expected()
{
    for check in "$@"; do
        [ "$(grep -cF "$check" "$test")" -eq 1 ] || {
            echo "frame-proof: the check is not unique: $check" >&2
            exit 1
        }
        echo "frame_hosttest: line $(grep -nF "$check" "$test" | cut -d: -f1)"
    done
}

# run_phase NAME: compile the test against the private tree and run it for at
# most 10 seconds; `status` is its exit status and NAME.log what it printed.
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
    cksum "$source" "$header" "$binary" >"$binary.cksum"
    perl -e 'alarm 10; exec @ARGV or die "frame-proof: exec: $!\n"' \
        "$binary" >"$binary.log" 2>&1 &
    child=$!
    status=0
    wait "$child" || status=$?
    child=
    cat "$binary.log"
    echo "frame-proof: result: $phase status $status"
}

# mutate FILE OLD NEW...: replace each unique OLD in FILE by its NEW.
mutate()
{
    python3 - "$@" <<'PY'
from pathlib import Path
import os
import sys

path = Path(sys.argv[1])
source = path.read_text()
pairs = sys.argv[2:]
for old, new in zip(pairs[0::2], pairs[1::2]):
    if source.count(old) != 1:
        raise SystemExit('frame-proof: the pattern is not unique: ' + old)
    source = source.replace(old, new)
if not os.environ.get('RACH2_FAULT_TEXTS'):
    path.write_text(source)
PY
    [ -n "$texts" ] || touch "$1"
}

# fault NAME: run the mutated tree and require the failures `want` lists.
fault()
{
    run_phase "$1"
    printf '%s\n' "$want" >"$work/$1.want"
    if [ "$status" -eq 0 ] || ! cmp -s "$work/$1.want" "$work/$1.log"; then
        echo "frame-proof: $1: the failures are not the ones expected:" >&2
        cat "$work/$1.want" >&2
        exit 1
    fi
    restore
}

if [ -n "$texts" ]; then
    trap - EXIT
    run_phase() { :; }
    restore() { :; }
    fault() { :; }
    RACH2_FAULT_TEXTS=1
    export RACH2_FAULT_TEXTS
else
    echo "frame-proof: logs: $work"
fi
if [ -z "$texts" ]; then
    run_phase fixed
    [ "$status" -eq 0 ] && [ ! -s "$work/fixed.log" ] || exit 1
    [ "$#" -eq 0 ] || exit 0
fi

mutate "$source" \
    'if (type == FRAME_MAP_USABLE && range_add' 'if (type != 2u && range_add' \
    'if (type != FRAME_MAP_USABLE &&' 'if (type == 2u &&'
want=$(expected \
    'CHECK(m.usable_bytes == KIB(2787701));' \
    'CHECK(!frame_map_usable(&m, 0xAAEBFu));' \
    'CHECK(!frame_map_usable(&m, 0xAAFBFu));' \
    'CHECK(frame_map_managed(&m) == KIB(2780712));' \
    'CHECK(m.usable_bytes == KIB(2787701) + KIB(13883392));' \
    'CHECK(!frame_map_usable(&m, 0xAAEBFu) && !frame_map_usable(&m, 0xAAFBFu));' \
    'CHECK(frame_map_managed(&m) == KIB(2780712) + KIB(13883392));' \
    'CHECK(m.count == 1 + UEFI_GROUPS);' \
    'CHECK(m.usable_bytes == low && m.unreachable_bytes == high);' \
    'CHECK(m.count == 1 + UEFI_GROUPS + 2);' \
    'CHECK(m.usable_bytes == low + high && m.unreachable_bytes == 0);' \
    'CHECK(!frame_map_usable(&m, (uefi_group(7) + MIB(16)) / FRAME_PAGE_SIZE));' \
    'CHECK(frame_map_managed(&m) == low + high - m.reserved_bytes);')
fault acpi-nvs-as-ram

mutate "$source" \
    'range_subtract(m, reach, ~(uint64_t)0,' 'range_subtract(m, FRAME_REACH_32, ~(uint64_t)0,'
q1='CHECK(m.usable_bytes + KIB(513) == MIB(sizes[i]));'
q2='CHECK(m.unreachable_bytes == 0);'
q3='CHECK(m.top_frame * FRAME_PAGE_SIZE == top);'
q4='CHECK(frame_map_usable(&m, 0x100000u) && frame_map_usable(&m, m.top_frame - 1));'
q5='CHECK(at == top - table);'
q6='CHECK(frame_map_managed(&m) == MIB(sizes[i]) - KIB(513) - m.reserved_bytes);'
want=$(expected "$q1" "$q2" "$q3" "$q4" "$q5" "$q6" "$q1" "$q2" "$q3" "$q4" "$q5" "$q6" \
    'CHECK(m.usable_bytes == KIB(2787701) + KIB(13883392));' \
    'CHECK(m.unreachable_bytes == 0 && !m.from_mem_upper);' \
    'CHECK(m.count == 5);' \
    'CHECK(m.top_frame == 0x44F600u);' \
    'CHECK(frame_map_usable(&m, 0x100000u) && frame_map_usable(&m, 0x44F5FFu));' \
    'CHECK(frame_map_managed(&m) == KIB(2780712) + KIB(13883392));' \
    'CHECK(m.usable_bytes == MIB(255) + MIB(1024 * 1024));' \
    'CHECK(m.unreachable_bytes == MIB(1024 * 1024));' \
    'CHECK(m.top_frame == REACH_X86_64 / FRAME_PAGE_SIZE);' \
    'CHECK(frame_map_usable(&m, REACH_X86_64 / FRAME_PAGE_SIZE - 1));' \
    'CHECK(m.count == 1 + UEFI_GROUPS + 2);' \
    'CHECK(m.usable_bytes == low + high && m.unreachable_bytes == 0);' \
    'CHECK(m.top_frame == 0x400000000ULL / FRAME_PAGE_SIZE);' \
    'CHECK(at == 0x400000000ULL - m.top_frame * 4u);' \
    'CHECK(frame_map_managed(&m) == low + high - m.reserved_bytes);' \
    'CHECK(frame_map_place(&m, KIB(1024), MIB(32), &at) == 0 && at == 0x100000000ULL);')
fault reach-32

mutate "$header" \
    '#define FRAME_MAP_RANGES       (BOOT_MMAP_MAX + BOOT_MODULE_MAX + 1)' \
    '#define FRAME_MAP_RANGES       (BOOT_MMAP_MAX + BOOT_MODULE_MAX)'
want=$(expected \
    'CHECK(frame_map_reserve(&m, MIB(1 + 4 * i) + KIB(512), MIB(2 + 4 * i)) == 0);')
fault ranges-short

mutate "$source" \
    'start = (hi - span) & ~(uint64_t)(FRAME_PAGE_SIZE - 1);' \
    'start = (lo + FRAME_PAGE_SIZE - 1) & ~(uint64_t)(FRAME_PAGE_SIZE - 1);'
p1='CHECK(at == top - table);'
p2='CHECK(frame_map_usable(&m, at / FRAME_PAGE_SIZE - 1));'
want=$(expected "$p1" "$p2" "$p1" "$p2" \
    'CHECK(at == 0x400000000ULL - m.top_frame * 4u);' \
    'CHECK(at == MIB(192) - MIB(1) - KIB(4));')
fault place-low

if [ -n "$texts" ]; then
    echo 'frame-proof: texts: every fault is once in its source'
    exit 0
fi
run_phase restored
[ "$status" -eq 0 ] && [ ! -s "$work/restored.log" ] || exit 1
cmp "$work/frame.c.saved" "$source"
cmp "$work/frame.h.saved" "$header"
echo 'frame-proof: result: pass: each fault failed its checks alone and the restored source passed'
