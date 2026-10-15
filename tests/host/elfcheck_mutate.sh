#!/bin/sh
# Prove elf_hosttest.c's bound on the program header table with one private
# mutation of rach2/sys/proc/elf.c: the bound as a 32-bit sum that can wrap,
# with no check of `e_phentsize`.
set -eu
repo=$(CDPATH='' cd -- "$(dirname -- "$0")/../.." && pwd)
cd "$repo"

# apply_fault DIR: put the fault into DIR/proc/elf.c; with RACH2_FAULT_TEXTS
# set, only check its text and write nothing.
apply_fault()
{
    python3 - "$1" <<'PY'
from pathlib import Path
import sys
import os
import pathlib
if os.environ.get('RACH2_FAULT_TEXTS'):
    pathlib.Path.write_text = lambda self, *args, **kwargs: 0
path = Path(sys.argv[1]) / 'proc' / 'elf.c'
text = path.read_text()
new = ('''    if (ehdr->e_phentsize != sizeof(Elf_Phdr)) {
        return false;
    }
    if (ehdr->e_phoff > size ||
        (uint32_t)ehdr->e_phnum > (size - (uint32_t)ehdr->e_phoff) / sizeof(Elf_Phdr)) {
        return false;
    }
''')
old = ('''    if ((uint32_t)ehdr->e_phoff + (uint32_t)ehdr->e_phnum * (uint32_t)ehdr->e_phentsize > size) {
        return false;
    }
''')
if text.count(new) != 1:
    raise SystemExit('elfcheck-mutation: target is not unique')
path.write_text(text.replace(new, old))
PY
}

if [ "${1:-}" = --texts ]; then
    RACH2_FAULT_TEXTS=1 apply_fault rach2/sys >/dev/null
    echo 'elfcheck-mutation: texts: every fault is once in its source'
    exit 0
fi

. "$repo/tests/system/runlock.sh"
runlock_hold "$repo" elfcheck-mutation "$repo/tests/host/elfcheck_mutate.sh" "$@"

mkdir -p build
work=$(mktemp -d "$repo/build/elfcheck-proof.XXXXXX")
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

# cleanup: the exit trap; stop the running test and restore the source.
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

# run_phase PHASE STATUS MESSAGE...: compile elf_hosttest.c over the copy as
# $work/PHASE, run it for at most 10 seconds, and exit unless it ends with
# STATUS and its log contains every MESSAGE.
run_phase()
{
    phase=$1
    expected=$2
    shift 2
    binary="$work/$phase"
    if ! "${CC:-cc}" -std=c99 -g -O1 -ffreestanding -DRACH2_HOSTTEST \
        -fsanitize=address,undefined -fno-sanitize-recover=all \
        -I "$work" -I rach2/sys -I rach2/sys/lib -I rach2/sys/proc \
        tests/host/elf_hosttest.c -o "$binary" >"$binary.compile.log" 2>&1; then
        cat "$binary.compile.log" >&2
        exit 1
    fi
    cksum "$work/proc/elf.c" "$binary" >"$binary.artifacts"
    perl -e 'alarm 10; exec @ARGV or die "elfcheck-mutation: exec: $!\n"' \
        "$binary" >"$binary.log" 2>&1 &
    child=$!
    status=0
    wait "$child" || status=$?
    child=
    cat "$binary.log"
    [ "$status" -eq "$expected" ] || exit 1
    for message; do
        grep -Fq "$message" "$binary.log" || exit 1
    done
    echo "elfcheck-mutation: result: $phase status $status"
}

echo "elfcheck-mutation: logs: $work"
run_phase fixed 0 'elf-hosttest: result: pass: 0 failures'
apply_fault "$work"
run_phase wrapping-sum 2 'elf-hosttest: failed: elf_check: offset near 2^32' \
    'elf-hosttest: failed: elf_check: zero e_phentsize'
restore
run_phase restored 0 'elf-hosttest: result: pass: 0 failures'
cmp "$work/elf.saved" "$work/proc/elf.c"
