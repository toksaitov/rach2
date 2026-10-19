#!/bin/sh
# Prove that the replay of tests/fuzz/corpus/elf64 through elf64_fuzz.c
# catches an x86-64 loader that accepts an invalid image: four private
# mutations of rach2/sys/proc/elf.c, each the loss of one check, and the
# corpus input that the check alone rejects.
#
#   window     no bound at the top of the user window (pasttop)
#   ascending  no refusal of a segment that shares the last one's page
#              (overlap)
#   entry      an entry point at the top of the window taken (entrytop)
#   fits32     a 64-bit file offset cut to 32 bits (offset4g)
#
# Usage: elf64fuzz_mutate.sh [--texts]
set -eu
repo=$(CDPATH='' cd -- "$(dirname -- "$0")/../.." && pwd)
cd "$repo"

# apply_fault DIR FAULT: put FAULT into DIR/proc/elf.c; with
# RACH2_FAULT_TEXTS set, only check its text and write nothing.
apply_fault()
{
    python3 - "$1" "$2" <<'PY'
from pathlib import Path
import os
import sys
faults = {
    'window': (
        '        if (vaddr < ELF_USER_MIN || vaddr_end > ELF_USER_MAX) {\n',
        '        if (vaddr < ELF_USER_MIN) {\n'),
    'ascending': (
        '        if ((vaddr & ~(uintptr_t)(PAGE_SIZE - 1)) < prev_end) {\n'
        '            return -ENOEXEC;\n'
        '        }\n',
        ''),
    'entry': (
        '    if (ehdr.e_entry + bias < ELF_USER_MIN || ehdr.e_entry + bias >= ELF_USER_MAX) {\n',
        '    if (ehdr.e_entry + bias < ELF_USER_MIN || ehdr.e_entry + bias > ELF_USER_MAX) {\n'),
    'fits32': (
        '        if (vaddr < bias || vaddr_end < vaddr || ph->p_offset + ph->p_filesz < ph->p_offset ||\n'
        '            !ELF_FITS32(ph->p_offset + ph->p_filesz)) {\n',
        '        if (vaddr < bias || vaddr_end < vaddr || ph->p_offset + ph->p_filesz < ph->p_offset) {\n'),
}
path = Path(sys.argv[1]) / 'proc' / 'elf.c'
text = path.read_text()
names = list(faults) if sys.argv[2] == 'all' else [sys.argv[2]]
for name in names:
    old, new = faults[name]
    if text.count(old) != 1:
        raise SystemExit('elf64fuzz-mutation: target is not unique: ' + name)
    if not os.environ.get('RACH2_FAULT_TEXTS'):
        path.write_text(text.replace(old, new))
PY
}

if [ "${1:-}" = --texts ]; then
    RACH2_FAULT_TEXTS=1 apply_fault rach2/sys all >/dev/null
    echo 'elf64fuzz-mutation: texts: every fault is once in its source'
    exit 0
fi
if [ "$#" -gt 0 ]; then
    echo 'elf64fuzz-mutation: usage: elf64fuzz_mutate.sh [--texts]' >&2
    exit 2
fi

. "$repo/tests/system/runlock.sh"
runlock_hold "$repo" elf64fuzz-mutation "$repo/tests/host/elf64fuzz_mutate.sh" "$@"

mkdir -p build
work=
trap '[ -z "$work" ] || rm -rf "$work"' EXIT
stopped=
trap 'stopped=1; exit 129' HUP
trap 'stopped=1; exit 130' INT
trap 'stopped=1; exit 143' TERM
work=$(mktemp -d "$repo/build/elf64fuzz-proof.XXXXXX")
child=
mkdir -p "$work/proc"
cp rach2/sys/proc/elf.c "$work/elf.saved"
cp "$work/elf.saved" "$work/proc/elf.c"

# restore: copy the saved elf.c back over the copy under test.
restore()
{
    cp "$work/elf.saved" "$work/proc/elf.c"
    touch "$work/proc/elf.c"
}

# cleanup: stop the running replay and restore the source; the exit trap
# calls it, then removes $work if the script exits 0 or a signal stops it.
cleanup()
{
    if [ -n "$child" ]; then
        kill -TERM "$child" 2>/dev/null || :
        wait "$child" 2>/dev/null || :
        child=
    fi
    restore
}
st=0
trap 'st=$?; cleanup; { [ "$st" -ne 0 ] && [ -z "${stopped:-}" ]; } || [ -z "$work" ] || rm -rf "$work"' EXIT

# run_phase PHASE MESSAGE: compile elf64_fuzz.c over the copy as the replay
# program $work/PHASE, as tests/fuzz/build.sh compiles it, and replay the
# corpus through it for at most 30 seconds. With MESSAGE empty the replay
# must pass; otherwise it must fail and its log contain MESSAGE.
run_phase()
{
    phase=$1
    message=$2
    binary="$work/$phase"
    if ! "${CC:-cc}" -std=c99 -D_DEFAULT_SOURCE -D_FORTIFY_SOURCE=0 -DRACH2_HOSTTEST \
        -DRACH2_HOST_X86_64 -DFIXTURE_NO_MAIN -DFUZZ_REPLAY -g -O1 -Wno-empty-body \
        -ffreestanding -fsanitize=address,undefined -fno-sanitize-recover=all \
        -I "$work" -I rach2/sys -I rach2/sys/lib -I rach2/sys/proc -I utils -I krffs \
        -I tests/host/compat tests/fuzz/elf64_fuzz.c -o "$binary" \
        >"$binary.compile.log" 2>&1; then
        cat "$binary.compile.log" >&2
        exit 1
    fi
    cksum "$work/proc/elf.c" "$binary" >"$binary.artifacts"
    perl -e 'alarm 30; exec @ARGV or die "elf64fuzz-mutation: exec: $!\n"' \
        "$binary" tests/fuzz/corpus/elf64/* >"$binary.log" 2>&1 &
    child=$!
    status=0
    wait "$child" || status=$?
    child=
    cat "$binary.log"
    if [ -z "$message" ]; then
        [ "$status" -eq 0 ] || exit 1
        grep -q '^fuzz: replayed [1-9][0-9]* input(s)$' "$binary.log" || exit 1
    else
        { [ "$status" -ne 0 ] && [ "$status" -ne 142 ]; } || exit 1
        grep -Fq "elf64-fuzz: loader fault: $message" "$binary.log" || exit 1
    fi
    echo "elf64fuzz-mutation: result: $phase status $status"
}

echo "elf64fuzz-mutation: logs: $work"
run_phase fixed ''
for fault in window ascending entry fits32; do
    case $fault in
    window) message='a page mapped outside the user image window' ;;
    ascending) message='a page mapped twice' ;;
    entry) message='an entry point outside the user image window' ;;
    fits32) message='a segment whose file bytes lie past the end of the file' ;;
    esac
    apply_fault "$work" "$fault"
    run_phase "$fault" "$message"
    restore
done
run_phase restored ''
cmp "$work/elf.saved" "$work/proc/elf.c"
