#!/bin/sh
# Prove the range checks of rach2/sys/mm/paging.c and
# rach2/sys/arch/i686/mmu.c, entry 140 of milestone 50, and the page fault
# handler's panic for a kernel stack that ran into its guard page, entry 248,
# and the batched release of the temporary mappings.
# tests/host/paging_hosttest.c is compiled against a private copy of the
# kernel's sources with each fault put in turn, and its row must fail; the
# restored copy must pass again.
#
# Usage: paging_mutate.sh [--check]
# With --check it compiles and runs the unmodified test alone. The faults:
# map_mmio_typed's page count rounded up by `+ PAGE_SIZE - 1`, which wraps
# for a span that begins in the first page and ends in the last; the loop of
# premap_kernel_window bounded by `va_start + size`, which wraps below the
# start for a window that runs past the top; the refusal without its
# exception for address 0, the regression a868ba10 fixed; the handler
# without its test of the guard page, which panics as any kernel fault; and
# four faults of the temporary mappings: a release that unmaps its slot at
# once, a search that gives a released slot before a sweep, a sweep that
# unmaps held slots too, and a sweep that changes the kernel mappings
# without telling the other processors.
set -eu
repo=$(CDPATH='' cd -- "$(dirname -- "$0")/../.." && pwd)
cd "$repo"
[ "$#" -eq 0 ] || [ "$*" = --check ] || {
    echo 'paging-proof: usage: paging_mutate.sh [--check]' >&2
    exit 2
}
mkdir -p build/tests
work=$(mktemp -d "$repo/build/tests/paging-proof.XXXXXX")
child=
mkdir "$work/rach2"
cp -R rach2/sys "$work/rach2/"
paging="$work/rach2/sys/mm/paging.c"
mmu="$work/rach2/sys/arch/i686/mmu.c"
cp "$paging" "$work/paging.saved"
cp "$mmu" "$work/mmu.saved"

cleanup()
{
    if [ -n "$child" ]; then
        kill -TERM "$child" 2>/dev/null || :
        wait "$child" 2>/dev/null || :
        child=
    fi
    cp "$work/paging.saved" "$paging"
    cp "$work/mmu.saved" "$mmu"
    touch "$paging" "$mmu"
}
trap cleanup EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM

# The test's own dependency and flag lines, as the host suite reads them,
# with its kernel sources taken from the private tree.
test=tests/host/paging_hosttest.c
deps=$(grep -oE 'host-deps:[^*]*' "$test" | sed 's/host-deps://' | tr '\n' ' ' |
    sed "s# rach2/# $work/rach2/#g")
extra=$(grep -oE 'host-cflags:[^*]*' "$test" | sed 's/host-cflags://' | tr '\n' ' ')

run_phase()
{
    phase=$1
    expected=$2
    message=$3
    binary="$work/$phase"
    # shellcheck disable=SC2086
    if ! "${CC:-cc}" -std=c99 -D_DEFAULT_SOURCE -D_FORTIFY_SOURCE=0 -DRACH2_HOSTTEST \
        -fsanitize=address,undefined -fno-sanitize-recover=all -g -O1 \
        -Wno-empty-body -ffreestanding $extra \
        -I "$work/rach2/sys" -I "$work/rach2/sys/arch/x86" -I "$work/rach2/sys/arch/i686" \
        -I "$work/rach2/sys/lib" -I "$work/rach2/sys/proc" \
        -I utils -I krffs -I tests/host/compat \
        -o "$binary" "$test" $deps >"$binary.compile.log" 2>&1; then
        cat "$binary.compile.log" >&2
        exit 1
    fi
    cksum "$paging" "$mmu" "$binary" >"$binary.cksum"
    perl -e 'alarm 10; exec @ARGV or die "paging-proof: exec: $!\n"' \
        "$binary" >"$binary.log" 2>&1 &
    child=$!
    status=0
    wait "$child" || status=$?
    child=
    cat "$binary.log"
    [ "$status" -eq "$expected" ] && grep -Fq "$message" "$binary.log" || exit 1
    echo "paging-proof: result: $phase status $status"
}

echo "paging-proof: logs: $work"
run_phase fixed 0 'paging-hosttest: result: pass'
[ "$#" -eq 0 ] || exit 0
for fault in rounding loop zero guard unmap-each stale-reuse held-swept no-change; do
    case $fault in
        loop | zero) source=$mmu ;;
        *) source=$paging ;;
    esac
    python3 - "$source" "$fault" <<'PY'
from pathlib import Path
import sys

path = Path(sys.argv[1])
source = path.read_text()
fault = sys.argv[2]
function = {
    'rounding': 'uintptr_t map_mmio_typed(uintptr_t phys_addr, size_t size, pte_t cache)\n{',
    'loop': 'int premap_kernel_window(uintptr_t va_start, size_t size)\n{',
    'zero': 'int premap_kernel_window(uintptr_t va_start, size_t size)\n{',
    'guard': 'static void _page_fault_handler(isr_params_t *params)\n{',
    'unmap-each': 'void kunmap_temp(void *va)\n{\n    uintptr_t addr',
    'stale-reuse': 'static uint32_t temp_find(void)\n{',
    'held-swept': 'static void temp_sweep(void)\n{',
    'no-change': 'static void temp_sweep(void)\n{',
}[fault]
changes = {
    'rounding': (
        '    pages = span / PAGE_SIZE + ((span % PAGE_SIZE) ? 1u : 0u);\n',
        '    pages = (span + PAGE_SIZE - 1) / PAGE_SIZE;\n'),
    'loop': (
        '    uintptr_t addr;\n'
        '    uintptr_t tables, i;\n'
        '\n'
        '    if (va_start != 0 && size > (uintptr_t)0 - va_start) {\n'
        '        return -1;\n'
        '    }\n'
        '    tables = size / (PAGE_SIZE * 1024) + ((size % (PAGE_SIZE * 1024)) ? 1u : 0u);\n'
        '\n'
        '    for (i = 0; i < tables; i++) {\n'
        '        uintptr_t table_index;\n'
        '        page_table_t *table;\n'
        '\n'
        '        addr = va_start + i * (PAGE_SIZE * 1024);\n'
        '        table_index = (addr / PAGE_SIZE) / 1024;\n',
        '    uintptr_t addr;\n'
        '    uintptr_t end = va_start + size;\n'
        '\n'
        '    for (addr = va_start; addr < end; addr += PAGE_SIZE * 1024) {\n'
        '        uintptr_t table_index = (addr / PAGE_SIZE) / 1024;\n'
        '        page_table_t *table;\n'),
    'zero': (
        '    if (va_start != 0 && size > (uintptr_t)0 - va_start) {\n',
        '    if (size > (uintptr_t)0 - va_start) {\n'),
    'guard': (
        '    pid = kstack_overrun_pid(fault_address);\n',
        '    pid = -1;\n'),
    'unmap-each': (
        '    _temp_stale[slot / 32] |= 1u << (slot % 32);\n',
        '    *kernel_pte(addr) = 0;\n'
        '    paging_flush_tlb(addr);\n'),
    'stale-reuse': (
        '        if (!((_temp_slots[slot / 32] | _temp_stale[slot / 32]) & bit)) {\n',
        '        if (!(_temp_slots[slot / 32] & bit)) {\n'),
    'held-swept': (
        '        if (_temp_stale[slot / 32] & (1u << (slot % 32))) {\n',
        '        if (1) {\n'),
    'no-change': (
        '    arch_space_load(arch_space_root());\n'
        '    smp_kernel_mapping_changed();',
        '    arch_space_load(arch_space_root());'),
}
old, new = changes[fault]
if source.count(function) != 1:
    raise SystemExit('paging-proof: the function is not unique: ' + fault)
start = source.index(function)
end = source.index('\n}\n', start)
body = source[start:end]
if body.count(old) != 1:
    raise SystemExit('paging-proof: the fault\'s pattern is not unique: ' + fault)
path.write_text(source[:start] + body.replace(old, new) + source[end:])
PY
    touch "$source"
    case $fault in
        rounding)
            message='paging-hosttest: fail: a span from the first page into the last, whose rounding would wrap, is refused'
            ;;
        loop)
            message='paging-hosttest: fail: a window that runs past the top of the address space is refused and makes no table'
            ;;
        zero)
            message='paging-hosttest: fail: a window that starts at address zero gets its table'
            ;;
        guard)
            message="paging-hosttest: fail: a supervisor fault in the guard page below the running thread's kernel stack panics as that stack's overrun"
            ;;
        unmap-each)
            message='paging-hosttest: fail: a released temporary slot stays mapped and changes no kernel mapping'
            ;;
        stale-reuse | no-change)
            message='paging-hosttest: fail: with no slot free, a mapping sweeps every released slot with one change of kernel mappings and takes the first'
            ;;
        held-swept)
            message='paging-hosttest: fail: a held temporary slot is not given again and survives two sweeps'
            ;;
    esac
    run_phase "$fault-mutated" 1 "$message"
    cleanup
    run_phase "$fault-restored" 0 'paging-hosttest: result: pass'
    cmp "$work/paging.saved" "$paging"
    cmp "$work/mmu.saved" "$mmu"
done
echo 'paging-proof: result: pass: the eight faults failed their rows and the restored source passed'
