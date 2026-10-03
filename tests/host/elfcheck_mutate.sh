#!/bin/sh
# Prove the ELF header bound in a private compiled copy.
set -eu
repo=$(CDPATH='' cd -- "$(dirname -- "$0")/../.." && pwd)
cd "$repo"
mkdir -p build
work=$(mktemp -d "$repo/build/elfcheck-proof.XXXXXX")
child=
mkdir -p "$work/proc"
cp rach2/sys/proc/elf.c "$work/elf.saved"
cp "$work/elf.saved" "$work/proc/elf.c"

restore()
{
    cp "$work/elf.saved" "$work/proc/elf.c"
    touch "$work/proc/elf.c"
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
    expected=$2
    binary="$work/$phase"
    if ! "${CC:-cc}" -std=c99 -g -O1 -ffreestanding -DRACH2_HOSTTEST \
        -fsanitize=address,undefined -fno-sanitize-recover=all \
        -I "$work" -I rach2/sys -I rach2/sys/lib -I rach2/sys/proc \
        tests/host/elf_hosttest.c -o "$binary" >"$binary.compile.log" 2>&1; then
        cat "$binary.compile.log" >&2
        exit 1
    fi
    cksum "$work/proc/elf.c" "$binary" >"$binary.artifacts"
    "$binary" >"$binary.log" 2>&1 &
    child=$!
    status=0
    wait "$child" || status=$?
    child=
    echo "elfcheck-mutation: result: $phase status $status"
    if [ "$expected" = zero ]; then
        [ "$status" -eq 0 ] || exit 1
    else
        [ "$status" -ne 0 ] || exit 1
    fi
}

echo "elfcheck-mutation: logs: $work"
run_phase fixed zero
python3 - "$work" <<'PY'
from pathlib import Path
import sys
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
run_phase wrapping-sum nonzero
restore
run_phase restored zero
cmp "$work/elf.saved" "$work/proc/elf.c"
