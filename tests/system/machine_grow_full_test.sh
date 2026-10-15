#!/bin/sh
# machine_grow_full_test.sh: replay the complete range plan for the Samsung
# disk, 488397168 sectors, through run_logged and restore_range of
# tools/dev/machine_grow.sh, with reflash built natively.
#
# growset plans the ranges a 1 GiB seed grown to 488130928 sectors at LBA
# 266240 writes: 1856 ranges of 5932200 sectors in all. Each range of the
# target is filled with 0xa5 and its backup with a pattern of its own; the
# restore must put back every byte, leave both ends of each gap zero, and log
# one completion record per range. The target is a sparse ordinary file,
# never a device node.
# shellcheck disable=SC2034,SC2154
set -eu
repo=$(CDPATH='' cd -- "$(dirname -- "$0")/../.." && pwd)
proof=$(mktemp -d "${TMPDIR:-/tmp}/rach2-grow-full.XXXXXX")
proof=$(CDPATH='' cd -- "$proof" && pwd -P)
echo "machine-grow-full-test: logs: $proof"
# Report $* and exit 1.
fail() { echo "machine-grow-full-test: failed: $*" >&2; exit 1; }
cc -O2 -Wall -Wextra -o "$proof/growset" "$repo/tools/inspect/growset.c"
cc -O2 -Wall -Wextra -I "$repo/utils" -o "$proof/reflash" \
    "$repo/utils/reflash.c" "$repo/utils/sectorcopy.c" "$repo/utils/sha256.c"
# shellcheck source=tools/setup/e2fsprogs_path.sh
. "$repo/tools/setup/e2fsprogs_path.sh"
dd if=/dev/zero of="$proof/seed.ext2" bs=1048576 count=0 seek=1024 2>"$proof/seed.log"
E2FSPROGS_FAKE_TIME=1 mke2fs -q -F -t ext2 -b 4096 -g 32768 -I 128 \
    -O sparse_super,large_file,filetype,resize_inode,^dir_index,^ext_attr,^has_journal,^extent,^64bit,^flex_bg,^metadata_csum,^huge_file,^dir_nlink,^extra_isize \
    "$proof/seed.ext2" >"$proof/mkfs.log" 2>&1
"$proof/growset" --save --ranges --lba 266240 "$proof/seed.ext2" 488130928 > "$proof/ranges.txt"
python3 - "$proof" <<'PY'
import pathlib
import struct
import sys
p = pathlib.Path(sys.argv[1])
ranges = [tuple(map(int, r.split())) for r in (p / 'ranges.txt').read_text().splitlines()]
assert len(ranges) == 1856
assert sum(n for _, n in ranges) == 5932200
with (p / 'backup.bin').open('wb') as backup, (p / 'target').open('w+b') as target:
    target.truncate(488397168 * 512)
    for first, length in ranges:
        data = struct.pack('<QQ', first, length) * 65536
        target.seek(first * 512)
        left = length * 512
        while left:
            n = min(left, len(data))
            backup.write(data[:n])
            target.write(b'\xa5' * n)
            left -= n
PY
awk '
    /^(run_logged|restore_range)\(\)/ {copy = 1}
    copy {print}
    copy && /^}/ {copy = 0}
' "$repo/tools/dev/machine_grow.sh" > "$proof/functions.sh"
# These are the only host adapters: GNU dd or the same bounded byte reader,
# and reflash compiled natively from its current source.
if command -v gdd >/dev/null 2>&1; then
    grow_dd=gdd
elif dd --version >/dev/null 2>&1; then
    grow_dd='dd'
else
    grow_dd=python
fi
cat > "$proof/read.py" <<'PY'
import sys
args = dict(arg.split('=', 1) for arg in sys.argv[1:])
with open(args['if'], 'rb') as src:
    src.seek(int(args['skip']))
    left = int(args['count'])
    while left:
        data = src.read(min(left, 1048576))
        if not data:
            sys.exit(1)
        sys.stdout.buffer.write(data)
        left -= len(data)
PY
# shellcheck disable=SC2329
dd() {
    if [ "$grow_dd" = python ]; then python3 "$proof/read.py" "$@"; else command "$grow_dd" "$@"; fi
}
# shellcheck disable=SC2329
reflash() { "$proof/reflash" "$@"; }
# shellcheck disable=SC1091
. "$proof/functions.sh"
work=$proof
saved=$proof
disk="$proof/target"
offset=0
row=0
while read -r first length; do
    amount=$((length * 512))
    run_logged restore.log restore_range
    offset=$((offset + amount))
    row=$((row + 1))
    if [ "$((row % 256))" = 0 ]; then echo "machine-grow-full-test: restored $row ranges"; fi
done < "$proof/ranges.txt"
[ "$offset" = 3037286400 ] || fail 'wrong restored length'
[ "$(wc -l < "$proof/restore.log" | tr -d ' ')" = 1856 ] || fail 'a completion record was lost'
python3 - "$proof" <<'PY'
import pathlib
import struct
import sys
p = pathlib.Path(sys.argv[1])
ranges = [tuple(map(int, r.split())) for r in (p / 'ranges.txt').read_text().splitlines()]
with (p / 'target').open('rb') as target:
    for first, length in ranges:
        data = struct.pack('<QQ', first, length) * 65536
        target.seek(first * 512)
        left = length * 512
        while left:
            n = min(left, len(data))
            assert target.read(n) == data[:n], (first, length, left)
            left -= n
    for (first, length), (following, _) in zip(ranges, ranges[1:]):
        if first + length < following:
            for sector in (first + length, following - 1):
                target.seek(sector * 512)
                assert target.read(512) == bytes(512), sector
print('machine-grow-full-test: 5932200 sectors, 1856 completion records, every byte and gap boundaries passed')
PY
# Keep the small logs and generated geometry, not six GiB of fixture data.
rm "$proof/target" "$proof/backup.bin" "$proof/seed.ext2"
