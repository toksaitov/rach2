#!/bin/sh
# krffsvol.sh: the KRFFS volumes and disks of the krffsmount cases,
# tests/cases/machine.krffsmount.json, which the native cases of
# tests/native/probes.cases also read.
#
# Usage: krffsvol.sh volumes
#        krffsvol.sh disks DIR
#
# `volumes` writes the three files that krffsvol-test.manifest stages into
# build/tests/system/krffsvol, builds the 4 MiB volume of that manifest,
# volume.krffs, and from it three volumes broken on purpose: broken.krffs,
# with the faults a mount that writes must repair, and badmagic.krffs and
# badchain.krffs, which the kernel must refuse.
#
# `disks` writes into DIR, from those volumes, the disks the cases boot: a
# megabyte of zeros and then a volume in an MBR partition at LBA 2048 of
# the volume's own length, for volume.disk, broken.disk, badmagic.disk and
# badchain.disk; region.disk, whose volume is followed by eight sectors of
# zeros that its partition covers; three.disk, three copies of the volume
# in three partitions; and stick.img, a 32 MiB stick with the volume in a
# partition of type 0x83.
#
# Each file is written under another name and renamed into place, so that a
# reader finds it whole or not at all.
set -u
REPO=$(cd "$(dirname "$0")/../.." && pwd)
VOLDIR="$REPO/build/tests/system/krffsvol"
VOL="$VOLDIR/volume.krffs"
MKMBR="$REPO/build/tools/image/mkmbr"
PART=2048

dir=

# fail REASON: print REASON and exit 1.
fail() { echo "krffsvol: $1" >&2; exit 1; }

# cleanup: remove the files of this run not yet renamed into place.
cleanup() {
    for f in "$VOLDIR"/*."$$" ${dir:+"$dir"/*."$$"}; do
        [ ! -e "$f" ] || rm -f "$f"
    done
}
trap cleanup EXIT
trap 'exit 1' HUP INT TERM

# volumes: build the volume and the volumes broken on purpose.
volumes() {
    mkdir -p "$VOLDIR" || fail "cannot make $VOLDIR"
    python3 - "$VOLDIR/pattern.bin.$$" <<'PY' || fail "the pattern file"
import sys
open(sys.argv[1], 'wb').write(bytes((i * 31 + 7) & 0xFF for i in range(65536)))
PY
    printf 'hello\n' > "$VOLDIR/hello.txt.$$" || fail "the hello file"
    printf 'inner\n' > "$VOLDIR/inner.txt.$$" || fail "the inner file"
    for f in pattern.bin hello.txt inner.txt; do
        mv "$VOLDIR/$f.$$" "$VOLDIR/$f" || fail "the fixture files"
    done
    sh "$REPO/krffs/mkimage.sh" "$REPO/krffs/manifests/krffsvol-test.manifest" "$VOL" 4 \
        > /dev/null || fail "building the volume"

    # The three faults the format's order of writes can leave: a backward
    # link the forward chain contradicts, a reserved node with no name that
    # no path reaches, and one name on two nodes of a directory. The dirty
    # flag is what tells the mount a check is owed.
    python3 - "$VOL" "$VOLDIR/broken.krffs.$$" <<'PY' || fail "the volume to repair"
import struct, sys

TYPE, NAME, MODE, SIZE, PREV, PARENT = 2, 11, 267, 325, 333, 349
b = bytearray(open(sys.argv[1], 'rb').read())

# A check is owed.
struct.pack_into('<H', b, MODE, struct.unpack_from('<H', b, MODE)[0] | 0o4000)

nodes, off = [], 0
while off < len(b):
    size = struct.unpack_from('<Q', b, off + SIZE)[0]
    nodes.append((off, size, b[off + TYPE], bytes(b[off + NAME:off + NAME + 32])))
    off += size

reserved = [n for n in nodes if n[2] == 2 and n[3][0] != 0]
if len(reserved) < 3:
    raise SystemExit('the volume has too few files for these rows')

# A backward link that disagrees with the forward chain.
struct.pack_into('<Q', b, nodes[1][0] + PREV, 12345)
# A node no path reaches: a reserved node whose name is gone.
b[reserved[0][0] + NAME] = 0
# One name on two nodes: the second takes the third's name and directory.
b[reserved[1][0] + NAME:reserved[1][0] + NAME + 256] = \
    b[reserved[2][0] + NAME:reserved[2][0] + NAME + 256]
struct.pack_into('<Q', b, reserved[1][0] + PARENT,
                 struct.unpack_from('<Q', b, reserved[2][0] + PARENT)[0])
open(sys.argv[2], 'wb').write(b)
PY

    python3 - "$VOL" "$VOLDIR/badmagic.krffs.$$" <<'PY' || fail "the bad-magic volume"
import sys
b = bytearray(open(sys.argv[1], 'rb').read())
b[0] = 0x00
open(sys.argv[2], 'wb').write(b)
PY

    python3 - "$VOL" "$VOLDIR/badchain.krffs.$$" <<'PY' || fail "the malformed volume"
import struct, sys
SIZE = 325
b = bytearray(open(sys.argv[1], 'rb').read())
# The root's size, made one byte longer: every node after it is then at the
# wrong place and the ring cannot close.
struct.pack_into('<Q', b, SIZE, struct.unpack_from('<Q', b, SIZE)[0] + 1)
open(sys.argv[2], 'wb').write(b)
PY
    for f in broken badmagic badchain; do
        mv "$VOLDIR/$f.krffs.$$" "$VOLDIR/$f.krffs" || fail "the broken volumes"
    done
}

# disk OUT PAD VOLUME...: write OUT as a megabyte of zeros, each VOLUME in
# turn and PAD sectors of zeros, with a partition of type 0x7f over each
# volume, the last one PAD sectors longer.
disk() {
    out=$1 pad=$2
    shift 2
    { dd if=/dev/zero bs=512 count=$PART 2>/dev/null
      cat "$@"
      dd if=/dev/zero bs=512 count="$pad" 2>/dev/null; } > "$out.$$" || fail "writing $out"
    i=0 at=$PART parts=
    for v in "$@"; do
        i=$((i + 1))
        n=$(($(wc -c < "$v") / 512))
        [ "$i" -lt "$#" ] || n=$((n + pad))
        parts="$parts $at:$n:7f"
        at=$((at + n))
    done
    # shellcheck disable=SC2086 # one word for each partition
    "$MKMBR" --lba "$out.$$" $parts > /dev/null || fail "the partition table of $out"
    mv "$out.$$" "$out" || fail "renaming $out"
}

# disks DIR: write the disks of the cases into DIR.
disks() {
    dir=$1
    mkdir -p "$dir" || fail "cannot make $dir"
    disk "$dir/volume.disk" 0 "$VOL"
    for f in broken badmagic badchain; do
        disk "$dir/$f.disk" 0 "$VOLDIR/$f.krffs"
    done
    disk "$dir/region.disk" 8 "$VOL"
    disk "$dir/three.disk" 0 "$VOL" "$VOL" "$VOL"

    # The stick's partition is the volume's own length: a ring that does not
    # fill its region is refused, which region.disk tests and the stick does
    # not.
    stick="$dir/stick.img"
    dd if=/dev/zero of="$stick.$$" bs=1048576 count=32 2>/dev/null || fail "the stick image"
    "$MKMBR" --lba "$stick.$$" "$PART:$(($(wc -c < "$VOL") / 512)):83" > /dev/null ||
        fail "the stick's partition table"
    dd if="$VOL" of="$stick.$$" bs=512 seek=$PART conv=notrunc 2>/dev/null ||
        fail "the volume on the stick"
    mv "$stick.$$" "$stick" || fail "renaming the stick"
}

case "${1:-}" in
    volumes) [ "$#" -eq 1 ] || fail "usage: krffsvol.sh volumes"; volumes ;;
    disks) [ "$#" -eq 2 ] || fail "usage: krffsvol.sh disks DIR"; disks "$2" ;;
    *) echo 'usage: krffsvol.sh volumes | disks DIR' >&2; exit 2 ;;
esac
