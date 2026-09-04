#!/usr/bin/env sh
#
# krffscheck.sh: what the offline checker says about images broken on purpose.
#
# `fsck.krffs` is the independent judge of every KRFFS volume the kernel
# writes, so it must ask what the kernel asks before it mounts one. Milestone
# 44 gave it the backward link and moved its repairs after the walk; these
# rows are what say it kept them. No machine is booted: the images are built
# and broken on the host, and each case gets its own copy, because a run that
# repairs an image leaves it repaired.
#
# Usage: krffscheck.sh MKFS FSCK DIR
set -u
MKFS=$1; FSCK=$2; DIR=$3
REPO=$(cd "$(dirname "$0")/../.." && pwd)
cd "$REPO"
fails=0
say() { echo "krffscheck-test: $1"; }
bad() { echo "krffscheck-test: FAIL: $1" >&2; fails=$((fails + 1)); }

mkdir -p "$DIR"
CLEAN="$DIR/clean.krffs"
rm -f "$CLEAN"
dd if=/dev/zero of="$CLEAN" bs=1048576 count=4 2>/dev/null
"$MKFS" "$CLEAN" > /dev/null || { bad "mkfs.krffs"; exit 1; }

# The node header's fields, from krffs_node.h: the size at 325, the backward
# link at 333, the mode at 267. A test that computed them from the header
# would need the header; a test that names them says where they came from,
# and the first row below fails the moment they move.
copy_broken() {
    out="$DIR/$1.krffs"
    cp "$CLEAN" "$out"
    python3 - "$out" "$2" <<'PY'
import struct, sys
path, how = sys.argv[1], sys.argv[2]
SIZE, PREV, MODE = 325, 333, 267
b = bytearray(open(path, 'rb').read())
root = struct.unpack_from('<Q', b, SIZE)[0]
if how == 'backlink':
    struct.pack_into('<Q', b, root + PREV, root + 8)
elif how == 'rootlink':
    struct.pack_into('<Q', b, PREV, 12345)
elif how == 'dirty':
    struct.pack_into('<H', b, MODE, struct.unpack_from('<H', b, MODE)[0] | 0o4000)
elif how == 'magic':
    struct.pack_into('<H', b, root, 0x1234)
open(path, 'wb').write(b)
PY
    echo "$out"
}

status() { "$FSCK" "$1" $2 > "$1.log" 2>&1; echo $?; }

# A fresh image is clean, and says so under both modes.
[ "$(status "$CLEAN" --strict)" = 0 ] || bad "a fresh image is not clean under --strict"
[ "$(status "$CLEAN" "")" = 0 ] || bad "a fresh image is not clean"
say "a fresh image passes, strict or not: OK"

# The three repairable faults: refused under --strict, repaired without it,
# and clean afterwards.
for how in backlink rootlink dirty; do
    img=$(copy_broken "$how" "$how")
    [ "$(status "$img" --strict)" != 0 ] || bad "--strict accepted the $how fault"
    img=$(copy_broken "$how" "$how")
    [ "$(status "$img" "")" = 0 ] || bad "the $how fault was not repaired"
    [ "$(status "$img" --strict)" = 0 ] || bad "the $how fault was still there after the repair"
done
say "a wrong backward link, at a node and at the root, and the dirty flag: refused strict, repaired, then clean: OK"

# A fatal fault is refused either way and the image is left as it was.
img=$(copy_broken magic magic)
before=$(cksum < "$img")
[ "$(status "$img" --strict)" != 0 ] || bad "--strict accepted a bad signature"
[ "$(status "$img" "")" != 0 ] || bad "the checker accepted a bad signature"
[ "$before" = "$(cksum < "$img")" ] || bad "the checker changed an image it refused"
say "a bad signature is refused either way and the image is left as it was: OK"

if [ "$fails" = 0 ]; then
    say "PASS"
else
    exit 1
fi
