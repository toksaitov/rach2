#!/bin/sh
# Prove the rows of tests/host/ahci_hosttest.c and tests/host/xhci_hosttest.c
# that give a controller addresses above 4 GiB, whole, or, when it takes only
# 32-bit ones, move a buffer's pieces above 4 GiB through the DMA pool. Seven
# faults go into a private copy of the two drivers, one at a time, and each
# must fail the rows named for it and no other:
#
#   ahci-direct    AHCI gives every piece as it is, S64A or not.
#   ahci-high-0    AHCI writes 0 as each entry's high half.
#   ahci-no-back   AHCI does not copy a read back from the pool's pages.
#   xhci-heap      xHCI takes its pages from the heap, AC64 or not.
#   xhci-direct    xHCI gives every piece of a transfer as it is.
#   xhci-no-back   xHCI does not copy an IN transfer back from the pages.
#   xhci-copy      xHCI copies each OUT piece from the buffer's start.
#
# Usage: dma32_mutate.sh [--check | --texts]
# With --check it compiles and runs the two unmodified tests alone. With
# --texts it checks that each fault's text is once in the tree's driver, and
# builds and runs nothing.
set -eu
repo=$(CDPATH='' cd -- "$(dirname -- "$0")/../.." && pwd)
cd "$repo"
[ "$#" -eq 0 ] || [ "$*" = --check ] || [ "$*" = --texts ] || {
    echo 'dma32-proof: usage: dma32_mutate.sh [--check | --texts]' >&2
    exit 2
}
texts=
[ "$*" != --texts ] || texts=1
if [ -z "$texts" ]; then
    . "$repo/tests/system/runlock.sh"
    runlock_hold "$repo" dma32-proof "$repo/tests/host/dma32_mutate.sh" "$@"
fi
child=
if [ -n "$texts" ]; then
    ahci=rach2/sys/block/ahci.c
    xhci=rach2/sys/usb/xhci.c
else
    mkdir -p build/tests
    work=$(mktemp -d "$repo/build/tests/dma32-proof.XXXXXX")
    # The tests include the drivers by paths relative to themselves.
    mkdir -p "$work/rach2" "$work/tests/host"
    cp -R rach2/sys "$work/rach2/"
    cp -R tests/host/compat "$work/tests/host/"
    cp tests/host/ahci_hosttest.c tests/host/xhci_hosttest.c "$work/tests/host/"
    ahci="$work/rach2/sys/block/ahci.c"
    xhci="$work/rach2/sys/usb/xhci.c"
    cp "$ahci" "$work/ahci.c.saved"
    cp "$xhci" "$work/xhci.c.saved"
fi

# restore: copy the saved ahci.c and xhci.c back over the private tree.
restore()
{
    cp "$work/ahci.c.saved" "$ahci"
    cp "$work/xhci.c.saved" "$xhci"
    touch "$ahci" "$xhci"
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

# run_phase NAME TEST: compile TEST, one of the two, against the private tree
# as tests/system/suites/host.sh does, and run it; `status` is its exit
# status, NAME.log what it printed and NAME.fails its failed rows.
run_phase()
{
    phase=$1
    test=tests/host/$2
    binary="$work/$phase"
    deps=$(grep -oE 'host-deps:[^*]*' "$work/$test" | sed 's/host-deps://' | tr '\n' ' ')
    # shellcheck disable=SC2086 # the dependency list is one path a word
    if ! (cd "$work" && "${CC:-cc}" -std=c99 -D_DEFAULT_SOURCE -D_FORTIFY_SOURCE=0 \
        -DRACH2_HOSTTEST -fsanitize=address,undefined -fno-sanitize-recover=all -g -O1 \
        -Wno-empty-body -ffreestanding -I rach2/sys -I rach2/sys/arch/x86 \
        -I rach2/sys/arch/i686 -I rach2/sys/lib -I rach2/sys/proc -I tests/host/compat \
        -o "$binary" "$test" $deps) >"$binary.compile.log" 2>&1; then
        cat "$binary.compile.log" >&2
        exit 1
    fi
    cksum "$ahci" "$xhci" "$binary" >"$binary.cksum"
    perl -e 'alarm 120; exec @ARGV or die "dma32-proof: exec: $!\n"' \
        "$binary" >"$binary.log" 2>&1 &
    child=$!
    status=0
    wait "$child" || status=$?
    child=
    grep '^  FAIL: ' "$binary.log" >"$binary.fails" || :
    cat "$binary.fails"
    echo "dma32-proof: result: $phase status $status"
}

# mutate FILE OLD NEW: replace the unique OLD in FILE by NEW; with --texts,
# only check that OLD is once in FILE.
mutate()
{
    python3 - "$@" <<'PY'
from pathlib import Path
import os
import sys

path = Path(sys.argv[1])
source = path.read_text()
if source.count(sys.argv[2]) != 1:
    raise SystemExit('dma32-proof: the pattern is not unique: ' + sys.argv[2])
if not os.environ.get('RACH2_FAULT_TEXTS'):
    path.write_text(source.replace(sys.argv[2], sys.argv[3]))
PY
    [ -n "$texts" ] || touch "$1"
}

# fault NAME TEST ROW...: run TEST on the mutated tree and require it to
# fail by its status, not by a signal, and exactly the rows named, in the
# order the test runs them.
fault()
{
    name=$1
    t=$2
    shift 2
    run_phase "$name" "$t"
    : >"$work/$name.want"
    for row in "$@"; do
        printf '  FAIL: %s\n' "$row" >>"$work/$name.want"
    done
    if [ "$status" -eq 0 ] || [ "$status" -ge 128 ] || ! cmp -s "$work/$name.want" "$work/$name.fails"; then
        echo "dma32-proof: $name: the failures are not the ones expected:" >&2
        cat "$work/$name.want" >&2
        exit 1
    fi
    restore
}

if [ -n "$texts" ]; then
    trap - EXIT
    fault() { :; }
    RACH2_FAULT_TEXTS=1
    export RACH2_FAULT_TEXTS
else
    echo "dma32-proof: logs: $work"
fi
[ -n "$texts" ] || for t in ahci_hosttest.c xhci_hosttest.c; do
    run_phase "fixed-${t%.c}" "$t"
    [ "$status" -eq 0 ] && [ ! -s "$work/fixed-${t%.c}.fails" ] || exit 1
done
[ "$#" -eq 0 ] || [ -n "$texts" ] || exit 0

mutate "$ahci" '    if (ahci_dma64 || *phys + len <= AHCI_FOUR_GIB) {' '    if (1) {'
fault ahci-direct ahci_hosttest.c 'in three pieces, each through a page of the DMA pool' \
    'the table names the pages, the high halves 0' 'the pages exist for the rows after'

mutate "$ahci" '            prdt[n].dbau = ahci_hi(phys);' '            prdt[n].dbau = 0;'
fault ahci-high-0 ahci_hosttest.c 'named by its own pages, both halves'

mutate "$ahci" '            b->to[n][i] = b->page[n][i];' '            b->to[n][i] = 0;'
fault ahci-no-back ahci_hosttest.c 'and the bytes come back from the pages into it'

mutate "$xhci" '    if (hc->dma64) {
        return kalloc_pages(1, phys);' '    if (1) {
        return kalloc_pages(1, phys);'
fault xhci-heap xhci_hosttest.c 'its context array from the DMA pool' \
    'with its context and control ring from the pool' 'and given back when it closes' \
    'the pieces that end above it through pages of the pool' 'an IN transfer is given' \
    'the pages go back with the records'

mutate "$xhci" '        need |= (uint64_t)phys[i] + len[i] > XHCI_FOUR_GIB;' '        need |= 0;'
fault xhci-direct xhci_hosttest.c 'its pieces above 4 GiB have pages' \
    "and the medium holds the buffer's bytes, moved through the pool" \
    "and the medium's bytes land in the buffer, back from the pool"

mutate "$xhci" '            b->to[i][j] = b->page[i][j];' '            b->to[i][j] = 0;'
fault xhci-no-back xhci_hosttest.c 'its bytes come back up to the bytes moved' \
    "and the medium's bytes land in the buffer, back from the pool"

mutate "$xhci" '                b->page[i][j] = at[j];' '                b->page[i][j] = transfer->buffer[j];'
fault xhci-copy xhci_hosttest.c 'holding its bytes' \
    "and the medium holds the buffer's bytes, moved through the pool"

if [ -n "$texts" ]; then
    echo 'dma32-proof: texts: every fault is once in its source'
    exit 0
fi
for t in ahci_hosttest.c xhci_hosttest.c; do
    run_phase "restored-${t%.c}" "$t"
    [ "$status" -eq 0 ] && [ ! -s "$work/restored-${t%.c}.fails" ] || exit 1
done
cmp "$work/ahci.c.saved" "$ahci"
cmp "$work/xhci.c.saved" "$xhci"
echo 'dma32-proof: result: pass: each fault failed its rows alone and the restored sources passed'
