#!/bin/sh
# Prove the GPT disk-write refusal row with recovery kernels built privately.
#
# Usage: sh tests/system/gpt_write_mutate.sh
#
# tests/system/gpt_write_refusal.sh types the recovery shell's `disk write`
# at its GPT and MBR disks and requires the refusal and an unchanged disk, or
# on the one disk whose last sector nothing holds the round-trip. The
# recovery kernel is built seven times from a private copy of rach2/ and mk/,
# never from the main tree's sources or into its build/: as it is, where the
# row must pass; with the refusal taken out, so that the command writes the
# backup header's sector, where the row must fail on the GPT disk with no
# protective MBR and that disk must differ from its copy; restored, where it
# must pass; with the read of the last sector taken out, so that a disk whose
# LBA 1 says nothing is taken for one without a table, where the row must fail
# on the GPT disk with neither a protective MBR nor a primary header (entry
# 172); restored again; with the check of the MBR's entries taken out, where
# the row must fail on the MBR disk whose partition reaches the last sector
# (entry 195); and restored once more. The GPT faults are judged on the disks
# with no protective MBR because the MBR check refuses any disk with one, and
# the row runs those two first because it stops at its first failure (entry
# 206).
# Each restored kernel must be identical to the first. The trap stops the
# row's process group before it restores the private source.
set -eu
repo=$(CDPATH='' cd -- "$(dirname -- "$0")/../.." && pwd)
cd "$repo"
[ ! -d build/.run.lock ] || {
    echo 'gpt-write-mutation: another suite owns this tree' >&2; exit 1;
}
mkdir -p build
work=$(mktemp -d "$repo/build/gpt-write-proof.XXXXXX")
tree="$work/tree"
source="$tree/rach2/kernel.c"
child=
changed=

stop_child()
{
    if [ -n "$child" ]; then
        kill -TERM "-$child" 2>/dev/null || :
        wait "$child" 2>/dev/null || :
        while kill -0 "-$child" 2>/dev/null; do
            sleep 1
        done
        child=
    fi
}

restore()
{
    cp "$work/kernel.saved" "$source"
    touch "$source"
    changed=
}

cleanup()
{
    stop_child
    if [ -n "$changed" ]; then
        restore
    fi
}
trap cleanup EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM

run_phase()
{
    phase=$1
    expected=$2
    disk=${3:-b}
    # The object goes first: a source restored within the second its object
    # was built reads as up to date to a make that compares whole seconds.
    rm -f "$tree/build/rach2-recovery/kernel.o" "$tree/build/rach2-recovery/kernel"
    make -s -j8 -C "$tree/rach2" COMPONENT=rach2-recovery KEXTRA=-DRACH2_RECOVERY \
        >"$work/$phase.build.log" 2>&1 || {
        cat "$work/$phase.build.log" >&2; exit 1;
    }
    cp "$tree/build/rach2-recovery/kernel" "$work/$phase.kernel"
    cksum "$source" "$work/$phase.kernel" >"$work/$phase.artifacts"
    build/tests/system/runutil group sh tests/system/gpt_write_refusal.sh "$work/$phase.kernel" \
        build/images/test.krffs build/tests/system/harness build/tools/image/mkgpt \
        "$work/$phase.row" >"$work/$phase.out" 2>&1 &
    child=$!
    status=0
    wait "$child" || status=$?
    stop_child
    cat "$work/$phase.out"
    if [ "$expected" = pass ]; then
        [ "$status" -eq 0 ] || exit 1
    else
        [ "$status" -ne 0 ] || {
            echo "gpt-write-mutation: the write without its refusal passed: $phase" >&2; exit 1;
        }
        grep -aq 'disk: last-sector round-trip' "$work/$phase.row.$disk.log" || exit 1
        if cmp -s "$work/$phase.row.$disk.saved" "$work/$phase.row.$disk.disk"; then
            echo "gpt-write-mutation: the disk did not change: $phase" >&2; exit 1
        fi
    fi
    echo "gpt-write-mutation: result: $phase status $status"
}

echo "gpt-write-mutation: logs: $work"
make -s test-image build/tests/system/harness build/tests/system/runutil \
    build/tools/image/mkgpt build/tools/inspect/elfcheck >"$work/prepare.log" 2>&1 || {
    cat "$work/prepare.log" >&2; exit 1;
}
# The private tree has what the kernel's own Makefile reads: rach2/ and mk/.
# Its link asks the directory above for the ELF checker, so that directory
# gets a copy of the checker and a Makefile with nothing to rebuild.
mkdir -p "$tree/build/tools/inspect"
cp -R rach2 mk "$tree/"
cp build/tools/inspect/elfcheck "$tree/build/tools/inspect/elfcheck"
printf 'build/tools/inspect/elfcheck: ;\n' >"$tree/Makefile"
cp "$source" "$work/kernel.saved"
run_phase fixed pass
changed=1
python3 - "$source" <<'PY'
from pathlib import Path
import sys

path = Path(sys.argv[1])
text = path.read_text()
start_line = "    if (is_gpt) {\n"
if text.count(start_line) != 1:
    raise SystemExit("gpt-write-mutation: the GPT refusal is not unique")
start = text.index(start_line)
end = text.index("\n    }\n", start) + len("\n    }\n")
if "return;" not in text[start:end]:
    raise SystemExit("gpt-write-mutation: the GPT refusal does not return")
path.write_text(text[:start] + "    (void)is_gpt;\n" + text[end:])
PY
touch "$source"
run_phase refusal-removed fail g
if cmp -s "$work/fixed.kernel" "$work/refusal-removed.kernel"; then
    echo 'gpt-write-mutation: the mutation did not change the kernel' >&2; exit 1
fi
restore
run_phase restored pass
cmp "$work/fixed.kernel" "$work/restored.kernel"
changed=1
python3 - "$source" <<'PY'
from pathlib import Path
import sys

path = Path(sys.argv[1])
text = path.read_text()
read = ("        if (block0->read(block0, lba, 1, disk_sector) != 0) {\n"
        "            console_printf(\"disk: the last sector is unreadable; it is not written\\n\");\n"
        "            return;\n"
        "        }\n"
        "        is_gpt = holds_gpt_header(disk_sector);\n")
if text.count(read) != 1:
    raise SystemExit("gpt-write-mutation: the read of the last sector is not unique")
path.write_text(text.replace(read, ""))
PY
touch "$source"
run_phase backup-unread fail h
if cmp -s "$work/fixed.kernel" "$work/backup-unread.kernel"; then
    echo 'gpt-write-mutation: the second mutation did not change the kernel' >&2; exit 1
fi
restore
run_phase backup-restored pass
cmp "$work/fixed.kernel" "$work/backup-restored.kernel"
changed=1
python3 - "$source" <<'PY'
from pathlib import Path
import sys

path = Path(sys.argv[1])
text = path.read_text()
check = "            return (int)i + 1;\n"
if text.count(check) != 1:
    raise SystemExit("gpt-write-mutation: the MBR check is not unique")
path.write_text(text.replace(check, "            return 0;\n"))
PY
touch "$source"
run_phase mbr-unread fail e
if cmp -s "$work/fixed.kernel" "$work/mbr-unread.kernel"; then
    echo 'gpt-write-mutation: the third mutation did not change the kernel' >&2; exit 1
fi
restore
run_phase mbr-restored pass
cmp "$work/fixed.kernel" "$work/mbr-restored.kernel"
echo 'gpt-write-mutation: result: pass: without the refusal the GPT disk with no protective MBR was written, without the read of the last sector the one with no primary header either, without the MBR check the MBR disk whose partition reaches the last sector; the fixed and restored kernels passed and matched'
