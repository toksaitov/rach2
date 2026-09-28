#!/bin/sh
# Prove the three fixes of entry 27 of milestone 50 in the exFAT driver's
# directory scan, with private mutations of rach2/sys/fs/exfat.c, against
# the cost rows of tests/host/exfat_hosttest.c (its `--scan-cost` mode):
# the twenty creates after the first two hundred in one directory, and a
# lookup of the last name. On the same private copy it proves the whole
# test's row on the file the unlink-synced sequence unlinks after its fsync.
#
# Usage: exfat_scan_mutate.sh [--check]
# With --check it compiles and runs the unmodified rows alone, the cost
# rows and the whole test. The faults, the first four each an old path
# put back:
#   filler  the filler entry at a sector's last slot written at every scan
#           that passes it, which fails the creates' bound of 60 writes;
#   entry   a cache get for every 32-byte entry, the mount's copy of the
#           block ignored, which fails the bound of 4000 cache gets;
#   upcase  every up-case walked in the stored table from its start, the
#           mappings read at mount ignored, which fails the same bound on
#           this volume, whose table is read through the cache;
#   grow    the caller's set left for the directory's growth to overwrite,
#           entry 194, which fails the volume's check after the creates;
#   unlink  `ex_unlink` answering 0 once it has found the set, with nothing
#           retired or freed, which fails the whole test's two rows that
#           require the file unlinked while held and the one unlinked after a
#           sync to be gone; the phase requires both. The mkdir and
#           crossing sequences and the free-durability row fail as well,
#           since the driver has one unlink path.
set -eu

repo=$(CDPATH='' cd -- "$(dirname -- "$0")/../.." && pwd)
cd "$repo"
[ "$#" -eq 0 ] || [ "$*" = --check ] || {
    echo 'exfat-scan-proof: usage: exfat_scan_mutate.sh [--check]' >&2
    exit 2
}
mkdir -p build/tests
work=$(mktemp -d "$repo/build/tests/exfat-scan-proof.XXXXXX")
# The test's fixtures, in a directory of this run's that the trap removes: a
# test given none makes its own and keeps it when it fails, as each fault
# makes it do.
fixtures=$(mktemp -d /tmp/rach2-fix.XXXXXX)
mkdir -p "$work/rach2" "$work/tests/host"
cp -R rach2/sys "$work/rach2/sys"
cp tests/host/exfat_hosttest.c "$work/tests/host/"
source="$work/rach2/sys/fs/exfat.c"
cp "$source" "$work/source.saved"
child=

cleanup()
{
    if [ -n "$child" ]; then
        kill -TERM "-$child" 2>/dev/null || :
        wait "$child" 2>/dev/null || :
        while kill -0 "-$child" 2>/dev/null; do
            sleep 1
        done
        child=
    fi
    cp "$work/source.saved" "$source"
    touch "$source"
}
trap 'cleanup; rm -rf "$fixtures"' EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM

# The test's own dependency lines, as the host suite reads them, with the
# kernel's sources taken from the private tree.
deps=$(grep -oE 'host-deps:[^*]*' tests/host/exfat_hosttest.c | sed 's/host-deps://' |
    tr '\n' ' ' | sed "s# rach2/# $work/rach2/#g")

# run_phase PHASE STATUS MESSAGE [ARG...]: compile the test over the
# private tree and run it with ARG, `--scan-cost` or none for the whole
# test; it must exit with STATUS and print MESSAGE.
run_phase()
{
    phase=$1
    expected=$2
    message=$3
    shift 3
    binary="$work/$phase"
    # shellcheck disable=SC2086
    if ! "${CC:-cc}" -std=c99 -D_DEFAULT_SOURCE -D_FORTIFY_SOURCE=0 -DRACH2_HOSTTEST \
        -DFIXDIR="\"$fixtures\"" \
        -fsanitize=address,undefined -fno-sanitize-recover=all -g -O1 \
        -Wno-empty-body -ffreestanding \
        -I "$work/rach2/sys" -I "$work/rach2/sys/lib" -I "$work/rach2/sys/proc" \
        -I utils -I krffs -I tests/host/compat \
        -o "$binary" "$work/tests/host/exfat_hosttest.c" $deps \
        >"$binary.compile.log" 2>&1; then
        cat "$binary.compile.log" >&2
        exit 1
    fi
    cksum "$source" "$binary" >"$binary.cksum"
    perl -e 'setpgrp(0, 0) or die "exfat-scan-proof: setpgrp: $!\n";
             alarm 900; exec @ARGV or die "exfat-scan-proof: exec: $!\n"' \
        "$binary" "$@" >"$binary.log" 2>&1 &
    child=$!
    status=0
    wait "$child" || status=$?
    cleanup
    cat "$binary.log"
    [ "$status" -eq "$expected" ] || exit 1
    while IFS= read -r line; do
        grep -Fq "$line" "$binary.log" || exit 1
    done <<EOF
$message
EOF
    echo "exfat-scan-proof: result: $phase status $status"
}

echo "exfat-scan-proof: logs: $work"
run_phase fixed 0 'exfat_hosttest: OK' --scan-cost
run_phase fixed-whole 0 'exfat_hosttest: OK'
[ "$#" -eq 0 ] || exit 0
for fault in filler entry upcase grow unlink; do
    python3 - "$source" "$fault" <<'PY'
from pathlib import Path
import sys

path = Path(sys.argv[1])
source = path.read_text()
fault = sys.argv[2]
function, old, new = {
    'filler': ('static int free_slots(',
               '                    if (raw[0] == 0x00) {\n'
               '                        uint8_t filler[32];\n',
               '                    if (1) {\n'
               '                        uint8_t filler[32];\n'),
    'entry': ('static int dir_entry(',
              '    if (!em->dir_block_held || em->dir_block != blk) {\n',
              '    if (1) {\n'),
    'upcase': ('static int ex_upcase(',
               '    if (em->upcase_low_held && unit < UPCASE_LOW) {\n',
               '    if (0) {\n'),
    'grow': ('static int dir_grow(',
             '    k_memcpy(em->setbuf, em->setsave, sizeof(em->setbuf));\n',
             ''),
    'unlink': ('static int ex_unlink(',
               '    rc = set_retire(em, d, set.index, set.entries);\n'
               '    if (rc != 0) {\n'
               '        return rc;\n'
               '    }\n'
               '    return retire_chain(em, d, &set);',
               '    return 0;'),
}[fault]
if source.count(function) != 1:
    raise SystemExit('exfat-scan-proof: the function is not unique: ' + function)
start = source.index(function)
end = source.index('\n}\n', start)
body = source[start:end]
if body.count(old) != 1:
    raise SystemExit('exfat-scan-proof: the pattern is not unique: ' + fault)
path.write_text(source[:start] + body.replace(old, new) + source[end:])
PY
    touch "$source"
    mode=--scan-cost
    case $fault in
        filler)
            message='exfat_hosttest: FAIL: the twenty creates after the first two hundred write at most 60 times'
            ;;
        entry | upcase) message='exfat_hosttest: FAIL: and take at most 4000 cache gets' ;;
        grow)
            message='exfat_hosttest: FAIL: the volume is consistent after two hundred and twenty creates'
            ;;
        unlink)
            message='exfat_hosttest: FAIL: the file unlinked while held is gone
exfat_hosttest: FAIL: the file unlinked after a sync is gone'
            mode=
            ;;
    esac
    # shellcheck disable=SC2086
    run_phase "$fault-mutated" 1 "$message" $mode
    cmp "$source" "$work/source.saved"
    # shellcheck disable=SC2086
    run_phase "$fault-restored" 0 'exfat_hosttest: OK' $mode
    cmp "$source" "$work/source.saved"
done
echo 'exfat-scan-proof: result: pass: each fault failed its row and the restored source passed'
