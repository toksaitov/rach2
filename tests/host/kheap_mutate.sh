#!/bin/sh
# Prove the rows of tests/host/kheap_hosttest.c that size the heap and give
# it a region past the floor, with four faults in a private copy of
# mm/kheap.c, each of which must fail the checks named for it and no other:
#
#   no-floor   gives a small machine an eighth of its memory, under 256 MiB.
#   a-quarter  gives the heap a quarter of the memory instead of an eighth.
#   no-cap     lets the heap pass the region the layout has.
#   short-map  sizes the page map of a large region for a quarter of its
#              pages, so the map's last pages are given out as the heap's.
#
# Usage: kheap_mutate.sh [--check]
# With --check it compiles and runs the unmodified test alone.
set -eu
repo=$(CDPATH='' cd -- "$(dirname -- "$0")/../.." && pwd)
cd "$repo"
[ "$#" -eq 0 ] || [ "$*" = --check ] || {
    echo 'kheap-proof: usage: kheap_mutate.sh [--check]' >&2
    exit 2
}
mkdir -p build/tests
work=$(mktemp -d "$repo/build/tests/kheap-proof.XXXXXX")
child=
# The test includes kheap.c and its headers by paths relative to itself.
mkdir -p "$work/rach2" "$work/tests/host"
cp -R rach2/sys "$work/rach2/"
cp tests/host/kheap_hosttest.c "$work/tests/host/"
source="$work/rach2/sys/mm/kheap.c"
cp "$source" "$work/kheap.c.saved"

restore()
{
    cp "$work/kheap.c.saved" "$source"
    touch "$source"
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

test=tests/host/kheap_hosttest.c
deps=$(grep -oE 'host-deps:[^*]*' "$test" | sed 's/host-deps://' | tr '\n' ' ')

# run_phase NAME: compile the test against the private tree and run it;
# `status` is its exit status and NAME.log what it printed.
run_phase()
{
    phase=$1
    binary="$work/$phase"
    # shellcheck disable=SC2086 # the dependency list is one path a word
    if ! (cd "$work" && "${CC:-cc}" -std=c99 -D_DEFAULT_SOURCE -D_FORTIFY_SOURCE=0 \
        -DRACH2_HOSTTEST -fsanitize=address,undefined -fno-sanitize-recover=all -g -O1 \
        -Wno-empty-body -ffreestanding -I rach2/sys -I rach2/sys/lib -I rach2/sys/proc \
        -o "$binary" "$test" $deps) >"$binary.compile.log" 2>&1; then
        cat "$binary.compile.log" >&2
        exit 1
    fi
    cksum "$source" "$binary" >"$binary.cksum"
    perl -e 'alarm 60; exec @ARGV or die "kheap-proof: exec: $!\n"' \
        "$binary" >"$binary.log" 2>&1 &
    child=$!
    status=0
    wait "$child" || status=$?
    child=
    cat "$binary.log"
    echo "kheap-proof: result: $phase status $status"
}

# mutate OLD NEW: replace the unique OLD in the private kheap.c by NEW.
mutate()
{
    python3 - "$source" "$1" "$2" <<'PY'
from pathlib import Path
import sys

path = Path(sys.argv[1])
source = path.read_text()
if source.count(sys.argv[2]) != 1:
    raise SystemExit('kheap-proof: the pattern is not unique: ' + sys.argv[2])
path.write_text(source.replace(sys.argv[2], sys.argv[3]))
PY
    touch "$source"
}

# fault NAME CHECK...: run the mutated tree and require it to fail exactly
# the checks named, in the order the test runs them.
fault()
{
    name=$1
    shift
    run_phase "$name"
    : >"$work/$name.want"
    for check in "$@"; do
        printf '  FAIL: %s\n' "$check" >>"$work/$name.want"
    done
    if [ "$status" -eq 0 ] || [ "$status" -ge 128 ] || ! cmp -s "$work/$name.want" "$work/$name.log"; then
        echo "kheap-proof: $name: the failures are not the ones expected:" >&2
        cat "$work/$name.want" >&2
        exit 1
    fi
    restore
}

echo "kheap-proof: logs: $work"
run_phase fixed
[ "$status" -eq 0 ] && grep -q '^kheap_hosttest: PASS$' "$work/fixed.log" || exit 1
[ "$#" -eq 0 ] || exit 0

mutate '    if (bytes < KHEAP_FLOOR) {' '    if (0) {'
fault no-floor 'i686, 512 MiB: the region' 'x86-64, 512 MiB: the floor' \
    'x86-64, under 2 GiB: the floor'

mutate '    uint64_t bytes = managed / 8u / KHEAP_GRAIN * KHEAP_GRAIN;' \
    '    uint64_t bytes = managed / 4u / KHEAP_GRAIN * KHEAP_GRAIN;'
fault a-quarter 'x86-64, under 2 GiB: the floor' 'x86-64, 3 GiB: an eighth' \
    "x86-64, QEMU's 8 GiB: an eighth in grains of 4 MiB" 'x86-64, 64 GiB: an eighth' \
    'x86-64, just under 4 TiB: an eighth, under the cap'

mutate '    if (bytes > most) {' '    if (0) {'
fault no-cap 'i686, 4 GiB: the region' 'x86-64, 64 TiB: the cap' \
    'a region under the floor: the region'

mutate '        map_pages = (heap_pages / 8u + PAGE_SIZE - 1) / PAGE_SIZE;' \
    '        map_pages = (heap_pages / 32u + PAGE_SIZE - 1) / PAGE_SIZE;'
fault short-map 'of three pages, a frame each, for 66560 pages' 'and taken' \
    'and held by the heap, with nothing in use' 'the first page given is after the map' \
    "every page below the reserve but the map's is one run" \
    "and every frame but the map's went back"

run_phase restored
[ "$status" -eq 0 ] && grep -q '^kheap_hosttest: PASS$' "$work/restored.log" || exit 1
cmp "$work/kheap.c.saved" "$source"
echo 'kheap-proof: result: pass: each fault failed its checks alone and the restored source passed'
