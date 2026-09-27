#!/bin/sh
# Prove the KRFFS host test's rows about the fields of a node no path reaches
# yet against the faults they were written for. Entry 15 of milestone 50: a
# create writes the new node's id under the whole-unit rule again, and a node
# whose id falls across a 4 KiB unit cannot be made; krffs_hosttest.c makes
# one with the rule on. Entry 125: mkdir makes a directory whose id another
# directory has, since `bC` and `cb` mix to one id; the test requires `/cb`
# refused beside `/bC`. Entry 243: that refusal answers KRFFS_EBAD again,
# which the adapters report as a full or a failing volume; the test requires
# KRFFS_ETAKEN. Entry 244: a copy being filled for a move writes its access
# time or its length under the rule again, or a node being made its
# modification time, and a node whose field falls across a unit can be
# neither moved there nor made there with its times; the test makes and
# moves nodes at such places with the rule on.
#
# The test is compiled as tests/system/suites/host.sh compiles it, from a
# private tree laid out as the repository is, so its include of krffs.c
# resolves to the private copy: on the fixed core, where it must pass; with
# each fault in turn, where that fault's row must fail; and on the restored,
# touched core, where it must pass again.
#
# Usage: sh tests/host/krffs_id_mutate.sh
set -eu
repo=$(CDPATH='' cd -- "$(dirname -- "$0")/../.." && pwd)
cd "$repo"
[ "$#" -eq 0 ] || {
    echo 'usage: krffs_id_mutate.sh' >&2
    exit 2
}
mkdir -p build/tests
work=$(mktemp -d "$repo/build/tests/krffs-id-proof.XXXXXX")
child=
mkdir -p "$work/rach2" "$work/tests/host"
cp -R rach2/sys "$work/rach2/sys"
cp tests/host/krffs_hosttest.c tests/host/krffs_hash_vectors.h "$work/tests/host/"
source="$work/rach2/sys/fs/krffs.c"
cp "$source" "$work/krffs.saved"

restore()
{
    cp "$work/krffs.saved" "$source"
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

# run_phase PHASE pass|fail MESSAGE: compile and run the test. A passing
# phase must exit 0 with its OK line; a failing one must exit with its count
# of failures, not a signal, with MESSAGE among them.
run_phase()
{
    phase=$1
    expected=$2
    message=$3
    binary="$work/$phase"
    if ! "${CC:-cc}" -std=c99 -D_DEFAULT_SOURCE -D_FORTIFY_SOURCE=0 -DRACH2_HOSTTEST \
        -fsanitize=address,undefined -fno-sanitize-recover=all -g -O1 -Wno-empty-body \
        -ffreestanding -I "$work/rach2/sys" -I "$work/rach2/sys/lib" -I "$work/rach2/sys/proc" \
        -o "$binary" "$work/tests/host/krffs_hosttest.c" "$work/rach2/sys/lib/kstring.c" \
        "$work/rach2/sys/lib/kctypes.c" >"$binary.compile.log" 2>&1; then
        cat "$binary.compile.log" >&2
        exit 1
    fi
    cksum "$source" "$binary" >"$binary.cksum"
    perl -e 'alarm 60; exec @ARGV or die "krffs-id-proof: exec: $!\n"' \
        "$binary" >"$binary.log" 2>&1 &
    child=$!
    status=0
    wait "$child" || status=$?
    child=
    cat "$binary.log"
    if [ "$expected" = pass ]; then
        [ "$status" -eq 0 ] && grep -Fq 'krffs_hosttest: OK' "$binary.log" || exit 1
    else
        [ "$status" -gt 0 ] && [ "$status" -lt 128 ] &&
            grep -Fq "krffs_hosttest: FAIL: $message" "$binary.log" || exit 1
    fi
    echo "krffs-id-proof: result: $phase status $status"
}

echo "krffs-id-proof: logs: $work"
run_phase fixed pass ''
for fault in new-id-rule id-taken taken-is-bad mkdir-id-rule copy-times-rule copy-length-rule \
    new-times-rule; do
    python3 - "$source" "$fault" <<'PY'
from pathlib import Path
import sys

path = Path(sys.argv[1])
fault = sys.argv[2]
text = path.read_text()
# Each fault is the code as it stood before its fix, in the one function it
# lived in.
faults = {
    'new-id-rule': ('int krffs_create_node_at(krffs_mount_t *mount, const char *path, '
                    'uint16_t mode,',
                    '    rc |= krffs_put_new_u64(mount, node, KRFFS_FIELD_ID, '
                    'krffs_calculate_djb_hash(name));\n',
                    '    rc |= krffs_put_u64(mount, node, KRFFS_FIELD_ID, '
                    'krffs_calculate_djb_hash(name));\n'),
    'id-taken': ('int krffs_mkdir_at(krffs_mount_t *mount, const char *path, uint16_t mode,',
                 '    if (krffs_id_taken(mount, krffs_mix_id(parent_id, base))) {\n'
                 '        return KRFFS_ETAKEN;\n'
                 '    }\n',
                 ''),
    'taken-is-bad': ('int krffs_mkdir_at(krffs_mount_t *mount, const char *path, uint16_t mode,',
                     '    if (krffs_id_taken(mount, krffs_mix_id(parent_id, base))) {\n'
                     '        return KRFFS_ETAKEN;\n'
                     '    }\n',
                     '    if (krffs_id_taken(mount, krffs_mix_id(parent_id, base))) {\n'
                     '        return KRFFS_EBAD;\n'
                     '    }\n'),
    'mkdir-id-rule': ('int krffs_mkdir_at(krffs_mount_t *mount, const char *path, uint16_t mode,',
                      '    rc |= krffs_put_new_u64(mount, node, KRFFS_FIELD_ID, '
                      'krffs_mix_id(parent_id, base));\n',
                      '    rc |= krffs_put_u64(mount, node, KRFFS_FIELD_ID, '
                      'krffs_mix_id(parent_id, base));\n'),
    'copy-times-rule': ('static uint64_t krffs_allocate_err(krffs_mount_t *mount, uint64_t size, '
                        'uint64_t original, int *err)',
                        '                rc |= krffs_put_new(mount, offset, KRFFS_FIELD_ATIME, '
                        '&from.access_time,\n',
                        '                rc |= krffs_put(mount, offset, KRFFS_FIELD_ATIME, '
                        '&from.access_time,\n'),
    'copy-length-rule': ('static uint64_t krffs_allocate_err(krffs_mount_t *mount, uint64_t size, '
                         'uint64_t original, int *err)',
                         '            if (krffs_put_new_u64(mount, offset, KRFFS_FIELD_DATA_SIZE, '
                         'data_size) != 0 ||\n',
                         '            if (krffs_put_u64(mount, offset, KRFFS_FIELD_DATA_SIZE, '
                         'data_size) != 0 ||\n'),
    'new-times-rule': ('static int krffs_stamp_new(krffs_mount_t *mount, uint64_t node)',
                       '    rc |= krffs_put_new(mount, node, KRFFS_FIELD_MTIME, &now, '
                       '(uint32_t)sizeof(now));\n',
                       '    rc |= krffs_put(mount, node, KRFFS_FIELD_MTIME, &now, '
                       '(uint32_t)sizeof(now));\n'),
}
head, old, new = faults[fault]
if text.count(head) != 1:
    raise SystemExit('krffs-id-proof: the function for ' + fault + ' is not unique')
start = text.index(head)
end = text.index('\n}\n', start)
body = text[start:end]
if body.count(old) != 1:
    raise SystemExit('krffs-id-proof: the pattern for ' + fault + ' is not unique')
path.write_text(text[:start] + body.replace(old, new) + text[end:])
PY
    if cmp -s "$work/krffs.saved" "$source"; then
        echo "krffs-id-proof: the $fault mutation did not change the source" >&2
        exit 1
    fi
    case $fault in
        new-id-rule) message='a node whose id falls across a block is made with the whole-unit rule on' ;;
        id-taken) message="a second directory with the first one's identifier is refused" ;;
        taken-is-bad) message='with the result of a taken identifier, which names no full or failing volume' ;;
        mkdir-id-rule) message='a directory whose id falls across a block is made with the whole-unit rule on' ;;
        copy-times-rule) message='a file that has to move where its field falls across a block moves with the whole-unit rule on' ;;
        copy-length-rule) message="a node made over a removed node's length across a block is made with the whole-unit rule on" ;;
        new-times-rule) message='a node whose field falls across a block is made with the whole-unit rule on' ;;
    esac
    run_phase "$fault-mutated" fail "$message"
    restore
    run_phase "$fault-restored" pass ''
    cmp "$work/krffs.saved" "$source"
done
echo 'krffs-id-proof: result: pass: each fault failed its row and each restored core passed'
