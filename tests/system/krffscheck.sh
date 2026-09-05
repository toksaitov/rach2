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
elif how == 'datasize':
    # The root, which is a node with data like any other. The free node after
    # it is not asked: a free node keeps the length of whatever was removed
    # there, and a split publishes the smaller size before the new length.
    DATA = 341
    struct.pack_into('<Q', b, DATA, struct.unpack_from('<Q', b, SIZE)[0])
elif how == 'zerosize':
    # The root itself, whose size the walk steps by: a size of zero returns
    # the walk to the root at once, so a checker that does not judge the size
    # first calls an image clean without looking at any of it.
    struct.pack_into('<Q', b, SIZE, 0)
    struct.pack_into('<Q', b, 341, 0)
elif how == 'smallsize':
    struct.pack_into('<Q', b, root + SIZE, 8)
elif how == 'hugesize':
    struct.pack_into('<Q', b, root + SIZE, 1 << 40)
elif how == 'freedatasize':
    DATA = 341
    struct.pack_into('<Q', b, root + DATA,
                     struct.unpack_from('<Q', b, root + SIZE)[0])
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
for how in magic datasize; do
    img=$(copy_broken "$how" "$how")
    before=$(cksum < "$img")
    [ "$(status "$img" --strict)" != 0 ] || bad "--strict accepted the $how fault"
    [ "$(status "$img" "")" != 0 ] || bad "the checker accepted the $how fault"
    [ "$before" = "$(cksum < "$img")" ] || bad "the checker changed an image it refused for $how"
done
say "a bad signature and a data size larger than its node are refused either way, and the image is left as it was: OK"

# A size no region can hold, in either direction: refused, and the image left
# as it was. The walk steps by these, so it must judge them before it does.
for how in zerosize smallsize hugesize; do
    img=$(copy_broken "$how" "$how")
    before=$(cksum < "$img")
    [ "$(status "$img" --strict)" != 0 ] || bad "--strict accepted the $how fault"
    [ "$(status "$img" "")" != 0 ] || bad "the checker accepted the $how fault"
    [ "$before" = "$(cksum < "$img")" ] || bad "the checker changed an image it refused for $how"
done
say "a root of no size, a node smaller than a header and one larger than the region are refused either way: OK"

# And the same field on a free node, which is not a fault: what a split leaves
# between the size it publishes and the length it writes after it.
img=$(copy_broken freedatasize freedatasize)
[ "$(status "$img" --strict)" = 0 ] || bad "--strict refused a free node's own length"
say "a free node whose length is larger than the node is accepted, as the kernel accepts it: OK"

# A file and a directory of one name in one directory, which a rename
# interrupted between the parent it published and the name it had still to
# write leaves behind. The repair keeps the directory whatever the sizes say,
# because its children name it by its identifier; the kernel's own check makes
# the same judgement.
DUP="$DIR/dirdup.krffs"
sh "$REPO/krffs/mkimage.sh" "$REPO/krffs/manifests/krffsvol-test.manifest" \
    "$DUP" 4 >/dev/null 2>&1 || bad "the image with a directory in it"
python3 - "$DUP" <<'PYDUP' || bad "the image with a name on two nodes"
import struct, sys
NAME, MODE, SIZE, PARENT = 11, 267, 325, 349
b = bytearray(open(sys.argv[1], 'rb').read())
nodes, off = [], 0
while off < len(b):
    size = struct.unpack_from('<Q', b, off + SIZE)[0]
    if size == 0:
        break
    nodes.append((off, size, b[off + 2], bytes(b[off + NAME:off + NAME + 32]),
                  struct.unpack_from('<H', b, off + MODE)[0]))
    off += size
dirs = [n for n in nodes if n[2] == 2 and (n[4] & 0o170000) == 0o040000]
files = [n for n in nodes if n[2] == 2 and (n[4] & 0o170000) != 0o040000 and n[3][0]]
if not dirs or not files:
    raise SystemExit('the image has no directory beside a file')
d = dirs[0]
# The largest file takes the directory's name and its parent, so the rule that
# keeps the larger node would drop the directory.
f = max(files, key=lambda n: n[1])
b[f[0] + NAME:f[0] + NAME + 256] = b[d[0] + NAME:d[0] + NAME + 256]
struct.pack_into('<Q', b, f[0] + PARENT,
                 struct.unpack_from('<Q', b, d[0] + PARENT)[0])
open(sys.argv[1], 'wb').write(b)
PYDUP
[ "$(status "$DUP" --strict)" != 0 ] || bad "--strict accepted a name on two nodes"
[ "$(status "$DUP" "")" = 0 ] || bad "the repeated name was not repaired"
"$REPO/build/krffs/readfile.krffs" "$DUP" /sub/inner.txt "$DIR/inner.out" \
    > /dev/null 2>&1 || bad "the repair took the directory and not the file"
say "a file that took a directory's name loses it, and the directory keeps its children: OK"

# The update protocol. The kernel writes an image a field at a time and a
# machine can stop between any two of them, so every prefix of every mutating
# operation must leave a file system the check accepts or repairs. The model
# below is the order rach2/sys/fs/krffs.Readme.md fixes, applied to an image
# whose free space is poisoned, so a length published before its bytes shows
# up as a file claiming bytes nobody wrote. The wrong orders at the end are
# what say the rows can fail.
python3 - "$FSCK" "$DIR" <<'PY' || fails=$((fails + 1))
import os, struct, subprocess, sys

FSCK, DIR = sys.argv[1], sys.argv[2]

HDR = 357
MAGIC, TYPE, ID, NAME, MODE = 0, 2, 3, 11, 267
SIZE, PREV, DATA, PARENT = 325, 333, 341, 349
ROOT, FREE, RESERVED = 0, 1, 2
ROOT_ID = 0
POISON = 0xA5


def djb(name):
    h = 5381
    for c in name:
        h = (((h << 5) + h) ^ c) & 0xFFFFFFFFFFFFFFFF
    return h


def header(size, prev, typ, name=b'', parent=ROOT_ID, mode=0o100644):
    h = bytearray(HDR)
    struct.pack_into('<H', h, MAGIC, 0x4B52)
    h[TYPE] = typ
    struct.pack_into('<Q', h, ID, djb(name))
    h[NAME:NAME + len(name)] = name
    struct.pack_into('<H', h, MODE, mode)
    struct.pack_into('<QQQQ', h, SIZE, size, prev, 0, parent)
    return bytes(h)


def walk(img):
    out, off = [], 0
    while off < len(img):
        size, back, data_size, parent = struct.unpack_from('<QQQQ', img,
                                                           off + SIZE)
        name = bytes(img[off + NAME:off + NAME + 256]).split(b'\0')[0]
        out.append({'off': off, 'size': size, 'type': img[off + TYPE],
                    'name': name, 'data': data_size, 'parent': parent,
                    'back': back})
        if size < HDR:
            break
        off += size
    return out


def content(n):
    return bytes(((i % 0x7E) + 1) for i in range(n))


def base_image(total=1 << 20):
    img = bytearray(total)
    img[0:HDR] = header(HDR, total - HDR, ROOT, mode=0o40755)
    img[HDR:HDR + HDR] = header(total - HDR, HDR, FREE)
    for i in range(HDR + HDR, total):
        img[i] = POISON
    return img


def apply(img, stores):
    out = bytearray(img)
    for off, data in stores:
        out[off:off + len(data)] = data
    return out


def named(img, name):
    return [n for n in walk(img) if n['name'] == name]


def split_and_fill(img, name, capacity):
    """Take a node out of the last free node, published by its type."""
    free = [n for n in walk(img) if n['type'] == FREE][-1]
    start, want = free['off'], HDR + capacity
    tail = free['size'] - want
    stores = []
    if tail >= HDR:
        stores.append((start + want, header(tail, want, FREE)))
        stores.append((start + SIZE, struct.pack('<Q', want)))
        stores.append((PREV, struct.pack('<Q', tail)))
    stores.append((start + NAME, name + b'\0'))
    stores.append((start + ID, struct.pack('<Q', djb(name))))
    stores.append((start + PARENT, struct.pack('<Q', ROOT_ID)))
    stores.append((start + MODE, struct.pack('<H', 0o100644)))
    stores.append((start + DATA, struct.pack('<Q', 0)))
    stores.append((start + TYPE, bytes([RESERVED])))
    return stores


def grow(img, name, new_length):
    """Take the free node after it whole, split the surplus back off, then
    write, then publish the length.

    Two published sizes, because the tail's header cannot be written while
    the free node it lands on is still in the ring: a growth shorter than a
    header would write over that node's own header."""
    nodes = walk(img)
    index = [k for k, n in enumerate(nodes) if n['name'] == name][0]
    node, following = nodes[index], nodes[index + 1]
    want = HDR + new_length
    whole = node['size'] + following['size']
    tail = whole - want
    stores = [(node['off'] + SIZE, struct.pack('<Q', whole)),
              (PREV, struct.pack('<Q', whole))]
    if tail >= HDR:
        stores.append((node['off'] + want, header(tail, want, FREE)))
        stores.append((node['off'] + SIZE, struct.pack('<Q', want)))
        stores.append((PREV, struct.pack('<Q', tail)))
    stores.append((node['off'] + HDR + node['data'],
                   content(new_length)[node['data']:]))
    stores.append((node['off'] + DATA, struct.pack('<Q', new_length)))
    return stores


def shrink(img, name, new_length):
    """Publish the shorter length, then give the surplus back."""
    nodes = walk(img)
    index = [k for k, n in enumerate(nodes) if n['name'] == name][0]
    node = nodes[index]
    following = nodes[index + 1] if index + 1 < len(nodes) else None
    want = HDR + new_length
    surplus = node['size'] - want
    stores = [(node['off'] + DATA, struct.pack('<Q', new_length))]
    if surplus >= HDR:
        free_size = surplus
        if following and following['type'] == FREE:
            free_size += following['size']
        stores.append((node['off'] + want, header(free_size, want, FREE)))
        stores.append((node['off'] + SIZE, struct.pack('<Q', want)))
        stores.append((PREV, struct.pack('<Q', free_size)))
    return stores


def remove_at(img, off):
    """Free a node and merge it with whichever neighbours are free."""
    nodes = walk(img)
    index = [k for k, n in enumerate(nodes) if n['off'] == off][0]
    node, before = nodes[index], nodes[index - 1]
    following = nodes[index + 1] if index + 1 < len(nodes) else None
    stores = [(node['off'] + TYPE, bytes([FREE]))]
    size = node['size']
    if following and following['type'] == FREE:
        size += following['size']
        stores.append((node['off'] + SIZE, struct.pack('<Q', size)))
    if before['type'] == FREE:
        stores.append((before['off'] + SIZE,
                       struct.pack('<Q', before['size'] + size)))
    stores.append((PREV,
                   struct.pack('<Q', walk(apply(img, stores))[-1]['size'])))
    return stores


def relocate(img, name, new_length):
    """One name on two nodes, which is what an interrupted move leaves.

    The stores here are the test's own, not the allocator's: what these rows
    ask is whether the check resolves the state, in favour of the larger node,
    since a move only ever makes a node larger. That the core takes this path
    at all is the host rows' business, where a real allocator runs."""
    stores = split_and_fill(img, name, new_length)
    after = apply(img, stores)
    new = named(after, name)[-1]
    stores.append((new['off'] + HDR, content(new_length)))
    stores.append((new['off'] + DATA, struct.pack('<Q', new_length)))
    old = named(after, name)[0]
    removal = remove_at(apply(after, stores[-2:]), old['off'])
    return stores + removal, len(removal)


def check(path, strict):
    args = [FSCK, path] + (['--strict'] if strict else [])
    return subprocess.run(args, stdout=subprocess.DEVNULL,
                          stderr=subprocess.DEVNULL).returncode


def judge(img, tag, prefix, present):
    """What the check owes a prefix: a walk, a repair, and one of each name."""
    path = os.path.join(DIR, 'protocol.krffs')
    open(path, 'wb').write(img)
    seen = set()
    longest = {}
    for node in walk(img):
        if node['type'] == RESERVED and node['name']:
            key = (node['parent'], node['name'])
            if key in seen and check(path, True) == 0:
                return '%s prefix %d: --strict called a repeated name clean' % (
                    tag, prefix)
            seen.add(key)
            longest[key] = max(longest.get(key, 0), node['data'])
    if check(path, False) != 0:
        return '%s prefix %d: the checker refused the image' % (tag, prefix)
    if check(path, True) != 0:
        return '%s prefix %d: repaired once and still not clean' % (tag, prefix)
    repaired = bytearray(open(path, 'rb').read())
    names = {}
    for node in walk(repaired):
        if node['type'] != RESERVED or not node['name']:
            continue
        key = (node['parent'], node['name'])
        if key in names:
            return '%s prefix %d: two nodes named %s' % (
                tag, prefix, node['name'].decode())
        names[key] = node
        body = repaired[node['off'] + HDR:node['off'] + HDR + node['data']]
        if POISON in body:
            return '%s prefix %d: %s claims bytes nobody wrote' % (
                tag, prefix, node['name'].decode())
    for key, node in names.items():
        if node['data'] < longest.get(key, 0):
            return '%s prefix %d: %s kept the shorter of two copies' % (
                tag, prefix, node['name'].decode())
    for name in present:
        if (ROOT_ID, name) not in names:
            return '%s prefix %d: %s is gone' % (tag, prefix, name.decode())
    return None


def every_prefix(tag, img, stores, present):
    for k in range(len(stores) + 1):
        why = judge(apply(img, stores[:k]), tag, k, present)
        if why:
            return why
    return None


def caught(tag, img, stores, present):
    for k in range(len(stores) + 1):
        if judge(apply(img, stores[:k]), tag, k, present):
            return True
    return False


def swap(stores, first, second):
    """The sequence with two of its stores exchanged, named by what they do."""
    i, j = stores.index(first), stores.index(second)
    out = list(stores)
    out[i], out[j] = out[j], out[i]
    return out


def store_at(stores, offset):
    """The last store to `offset`, which is what a name in a row means."""
    return [st for st in stores if st[0] == offset][-1]


fails = []
start = base_image()

create_alpha = split_and_fill(start, b'alpha', 100)
after_create = apply(start, create_alpha)
grow_alpha = grow(after_create, b'alpha', 4000)
after_grow = apply(after_create, grow_alpha)
nudge_alpha = grow(after_grow, b'alpha', 4030)
after_nudge = apply(after_grow, nudge_alpha)
shrink_alpha = shrink(after_nudge, b'alpha', 50)
after_shrink = apply(after_nudge, shrink_alpha)
create_beta = split_and_fill(after_shrink, b'beta', 200)
after_beta = apply(after_shrink, create_beta)
remove_alpha = remove_at(after_beta, named(after_beta, b'alpha')[0]['off'])
after_remove = apply(after_beta, remove_alpha)
move_beta, removal_length = relocate(after_remove, b'beta', 900)

steps = [
    ('create', start, create_alpha, ()),
    ('grow', after_create, grow_alpha, (b'alpha',)),
    ('grow by less than a header', after_grow, nudge_alpha, (b'alpha',)),
    ('shrink', after_nudge, shrink_alpha, (b'alpha',)),
    ('create beside it', after_shrink, create_beta, (b'alpha',)),
    ('remove', after_beta, remove_alpha, (b'beta',)),
    ('name on two nodes', after_remove, move_beta, (b'beta',)),
]

for tag, img, stores, present in steps:
    why = every_prefix(tag, img, stores, present)
    if why:
        fails.append(why)
    else:
        print('krffscheck-test: every stop inside a %s leaves an image the '
              'check accepts or repairs: OK' % tag)

# One rule of the protocol each, put back to front. A row that cannot fail
# proves nothing, so every one of these must be caught.
wrong = [
    ('a split published before the tail header was written',
     start, swap(create_alpha, create_alpha[0], create_alpha[1]), ()),
    ('a length published before the bytes it counts',
     after_create, swap(grow_alpha, grow_alpha[-2], grow_alpha[-1]),
     (b'alpha',)),
    # only a growth shorter than a header overlaps: with a longer one the
    #  tail's header lands in free space and the order does not matter 
    # Only a growth shorter than a header can overlap: with a longer one the
    # tail's header lands in free space whichever order it is written in.
    ('a grow of less than a header that split first',
     after_grow, swap(nudge_alpha, nudge_alpha[0], nudge_alpha[2]),
     (b'alpha',)),
    ('a shrink published before the free header was written',
     after_nudge, swap(shrink_alpha, shrink_alpha[1], shrink_alpha[2]),
     (b'alpha',)),
    ('a copy freed before the node it replaces was published',
     after_remove,
     move_beta[-removal_length:] + move_beta[:-removal_length], (b'beta',)),
]

for why, img, stores, present in wrong:
    if caught('wrong order', img, stores, present):
        print('krffscheck-test: caught: %s: OK' % why)
    else:
        fails.append('a wrong order went unnoticed: %s' % why)

for why in fails:
    print('krffscheck-test: FAIL: ' + why, file=sys.stderr)
sys.exit(1 if fails else 0)
PY

if [ "$fails" = 0 ]; then
    say "PASS"
else
    exit 1
fi
