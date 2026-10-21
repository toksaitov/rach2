#!/bin/sh
#
# krffscheck.sh: the program of the krffscheck cases,
# tests/cases/host.krffscheck.json: what the offline checker says about KRFFS
# images broken on purpose.
#
# `fsck.krffs` is the independent judge of every KRFFS volume the kernel
# writes, so it must ask what the kernel asks before it mounts one: each
# node's backward link, and every size before the walk steps by it, with its
# repairs made after the walk. No machine is booted: the images are made and
# broken on the host, each fault in a copy of its own, since a run that
# repairs an image leaves it repaired. Each run is one case, named by its
# argument: `fresh`, `repairable`, `fatal`, `sizes`, `free`, `dirdup`,
# `prefixes` and `orders`; the manifest says what each assertion checks.
#
# The program writes the case's report records on its standard output
# (tools/etude/Readme.md, Report records), with the lines a person reads
# between them, and exits 0 when every assertion passed and 1 otherwise. An
# assertion that a failure before it leaves without its evidence is reported
# as not run. etude runs it in the case's workspace, with ETUDE_TREE naming
# the root of the tree, to which the program changes, and TMPDIR the
# workspace. The KRFFS tools it runs are the artifacts the manifest's
# `prepare` lists, which etude builds first.
#
# Usage, in the root of a built tree, or anywhere with ETUDE_TREE set to it:
#     sh tests/cases/host/krffscheck.sh CASE
#
# The images go to a private directory under TMPDIR, whose path the first
# line prints; a run that passes or is stopped by a signal removes it, and one
# that fails keeps it.
#

# shellcheck disable=SC2329 # the cases are called by name, at the end
set -u

CASE=${1-}
[ "$#" -eq 1 ] && [ -n "$CASE" ] || { echo "krffscheck: usage: krffscheck.sh CASE" >&2; exit 2; }
if [ -n "${ETUDE_TREE-}" ]; then
    cd "$ETUDE_TREE" || exit 2
fi
unset ETUDE_TREE
[ -f krffs/mkimage.sh ] || { echo "krffscheck: run it in the root of the tree" >&2; exit 2; }
REPO=$(pwd -P)

MKFS="$REPO/build/krffs/mkfs.krffs"
FSCK="$REPO/build/krffs/fsck.krffs"
READFILE="$REPO/build/krffs/readfile.krffs"

# The exit trap keeps the images only after a failure, for the path it printed.
unset WORK
status=0
trap 'status=$?; [ "$status" -ne 0 ] || rm -rf ${WORK:+"$WORK"}; exit "$status"' EXIT
trap 'rm -rf ${WORK:+"$WORK"}; exit 129' HUP
trap 'rm -rf ${WORK:+"$WORK"}; exit 130' INT
trap 'rm -rf ${WORK:+"$WORK"}; exit 143' TERM
WORK=$(umask 077; mktemp -d "${TMPDIR:-/tmp}/rach2-krffscheck.XXXXXX") || {
    echo "krffscheck: cannot create a temporary directory" >&2
    exit 1
}
printf 'krffscheck: images: %s\n' "$WORK"

# ---- The report. ----

RECORDS=0
FAILED=""
REPORTED=" "
ASSERTIONS=""

# Write the report record WORDS, which fits in one write of 255 bytes.
record() {
    printf '@etude 1 %s\n' "$*"
}

# Make TEXT printable ASCII of at most 160 bytes, for a record's detail.
clip() {
    printf '%s' "$1" | tr -c ' -~' '?' | cut -c1-160
}

# pass ID: report that the assertion ID passed.
pass() {
    record "assert $1 pass"
    RECORDS=$((RECORDS + 1))
    REPORTED="$REPORTED$1 "
}

# fail ID DETAIL: report that the assertion ID failed, with DETAIL.
fail() {
    record "assert $1 fail $(clip "$2")"
    echo "krffscheck: FAIL: $1: $2" >&2
    RECORDS=$((RECORDS + 1))
    REPORTED="$REPORTED$1 "
    FAILED="$FAILED $1"
}

# notrun ID REASON: report that the assertion ID could not be judged.
notrun() {
    record "notrun $1 $(clip "$2")"
    RECORDS=$((RECORDS + 1))
    REPORTED="$REPORTED$1 "
}

# judge ID TEXT CONDITION...: report the assertion ID passed when the command
# CONDITION succeeds, and failed with the detail TEXT when it does not.
judge() {
    _id=$1
    _text=$2
    shift 2
    if "$@"; then
        pass "$_id"
    else
        fail "$_id" "$_text"
    fi
}

# setup_failed WHAT: note that the case could not be set up, which leaves its
# assertions without their evidence.
setup_failed() {
    echo "krffscheck: FAIL: $1" >&2
    FAILED="$FAILED setup"
}

# ---- The images. ----

# clean_image: make WORK/clean.krffs, a fresh 4 MiB image; 1 when mkfs.krffs
# fails.
clean_image() {
    CLEAN="$WORK/clean.krffs"
    dd if=/dev/zero of="$CLEAN" bs=1048576 count=4 2>/dev/null &&
        "$MKFS" "$CLEAN" > /dev/null
}

# copy_broken NAME HOW: copy the clean image to NAME.krffs, break it as HOW
# says, and print its path. The node header's fields are from krffs_node.h:
# the size at 325, the backward link at 333, the mode at 267. A test that
# computed them from the header would need the header; a test that names
# them says where they came from, and the repairable case fails the moment
# they move.
copy_broken() {
    out="$WORK/$1.krffs"
    cp "$CLEAN" "$out" || return 1
    python3 -I - "$out" "$2" <<'PY' || return 1
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
else:
    raise SystemExit('no fault ' + how)
open(path, 'wb').write(b)
PY
    echo "$out"
}

# status IMAGE [FLAG]: run fsck.krffs on IMAGE, with its output in IMAGE.log,
# and print its exit status.
status() { "$FSCK" "$1" ${2:+"$2"} > "$1.log" 2>&1; echo $?; }

# reads IMAGE PATH OUT: copy the file PATH out of IMAGE to OUT, quietly; the
# status of readfile.krffs.
reads() { "$READFILE" "$1" "$2" "$3" > "$3.log" 2>&1; }

# ---- The cases. ----

# A fresh image is clean, and says so under both modes.
case_fresh() {
    ASSERTIONS="strict plain"
    clean_image || { setup_failed "mkfs.krffs did not make a fresh image"; return; }
    judge strict "a fresh image is not clean under --strict" \
        [ "$(status "$CLEAN" --strict)" = 0 ]
    judge plain "a fresh image is not clean" [ "$(status "$CLEAN" "")" = 0 ]
}

# The three repairable faults: refused under --strict, repaired without it,
# and clean afterwards.
case_repairable() {
    ASSERTIONS="backlink-refused backlink-repaired backlink-clean"
    ASSERTIONS="$ASSERTIONS rootlink-refused rootlink-repaired rootlink-clean"
    ASSERTIONS="$ASSERTIONS dirty-refused dirty-repaired dirty-clean"
    clean_image || { setup_failed "mkfs.krffs did not make a fresh image"; return; }
    for how in backlink rootlink dirty; do
        img=$(copy_broken "$how" "$how") || { setup_failed "the $how fault"; return; }
        judge "$how-refused" "--strict accepted the $how fault" \
            [ "$(status "$img" --strict)" != 0 ]
        img=$(copy_broken "$how" "$how") || { setup_failed "the $how fault"; return; }
        judge "$how-repaired" "the $how fault was not repaired" [ "$(status "$img" "")" = 0 ]
        judge "$how-clean" "the $how fault was still there after the repair" \
            [ "$(status "$img" --strict)" = 0 ]
    done
}

# refused_either HOW...: for each fault HOW, require the image to be refused
# under --strict and without it, and to be left as it was.
refused_either() {
    for how in "$@"; do
        img=$(copy_broken "$how" "$how") || { setup_failed "the $how fault"; return; }
        before=$(cksum < "$img")
        judge "$how-strict" "--strict accepted the $how fault" \
            [ "$(status "$img" --strict)" != 0 ]
        judge "$how-refused" "the checker accepted the $how fault" \
            [ "$(status "$img" "")" != 0 ]
        judge "$how-unchanged" "the checker changed an image it refused for $how" \
            [ "$before" = "$(cksum < "$img")" ]
    done
}

# A bad signature and a data size larger than its node are fatal: refused
# either way, and the image left as it was.
case_fatal() {
    ASSERTIONS="magic-strict magic-refused magic-unchanged"
    ASSERTIONS="$ASSERTIONS datasize-strict datasize-refused datasize-unchanged"
    clean_image || { setup_failed "mkfs.krffs did not make a fresh image"; return; }
    refused_either magic datasize
}

# A size no region can hold, in either direction: refused, and the image left
# as it was. The walk steps by these, so it must judge them before it does.
case_sizes() {
    ASSERTIONS="zerosize-strict zerosize-refused zerosize-unchanged"
    ASSERTIONS="$ASSERTIONS smallsize-strict smallsize-refused smallsize-unchanged"
    ASSERTIONS="$ASSERTIONS hugesize-strict hugesize-refused hugesize-unchanged"
    clean_image || { setup_failed "mkfs.krffs did not make a fresh image"; return; }
    refused_either zerosize smallsize hugesize
}

# The same field on a free node, which is not a fault: what a split leaves
# between the size it publishes and the length it writes after it.
case_free() {
    ASSERTIONS="accepted"
    clean_image || { setup_failed "mkfs.krffs did not make a fresh image"; return; }
    img=$(copy_broken freedatasize freedatasize) || { setup_failed "the free node's fault"; return; }
    judge accepted "--strict refused a free node's own length" \
        [ "$(status "$img" --strict)" = 0 ]
}

# A file and a directory of one name in one directory, which a rename
# interrupted between the parent it published and the name it had still to
# write leaves behind. The repair keeps the directory whatever the sizes say,
# because its children name it by its identifier; the kernel's own check makes
# the same judgement. The image is the one krffsvol-test.manifest describes,
# made from the case's own files.
case_dirdup() {
    ASSERTIONS="refused repaired directory-kept"
    python3 -I - "$WORK/pattern.bin" <<'PY' || { setup_failed "the pattern file"; return; }
import sys
open(sys.argv[1], 'wb').write(bytes((i * 31 + 7) & 0xFF for i in range(65536)))
PY
    if ! printf 'hello\n' > "$WORK/hello.txt" || ! printf 'inner\n' > "$WORK/inner.txt"; then
        setup_failed "the fixture files"
        return
    fi
    # setidcheck finds a manifest's allowlist entry by its file name.
    sed -e "s#build/tests/system/krffsvol/#$WORK/#" \
        "$REPO/krffs/manifests/krffsvol-test.manifest" > "$WORK/krffsvol-test.manifest" ||
        { setup_failed "the image's manifest"; return; }
    DUP="$WORK/dirdup.krffs"
    sh "$REPO/krffs/mkimage.sh" "$WORK/krffsvol-test.manifest" "$DUP" 4 > "$WORK/mkimage.log" 2>&1 ||
        { setup_failed "the image with a directory in it; see $WORK/mkimage.log"; return; }
    python3 -I - "$DUP" <<'PY' || { setup_failed "the image with a name on two nodes"; return; }
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
PY
    judge refused "--strict accepted a name on two nodes" [ "$(status "$DUP" --strict)" != 0 ]
    judge repaired "the repeated name was not repaired" [ "$(status "$DUP" "")" = 0 ]
    judge directory-kept "the repair took the directory and not the file" \
        reads "$DUP" /sub/inner.txt "$WORK/inner.out"
}

# The update protocol. The kernel writes an image a field at a time and a
# machine can stop between any two of them, so every prefix of every mutating
# operation must leave a file system the check accepts or repairs. The model
# is the order rach2/sys/fs/krffs.Readme.md fixes, applied to an image whose
# free space is poisoned, so a length published before its bytes shows up as
# a file claiming bytes nobody wrote. In `prefixes` it prints `ok ID` or
# `bad ID WHY` for each operation; in `orders` the same for each rule of the
# protocol put back to front, each of which must be caught, which shows that
# the prefixes can fail.
protocol() {
    python3 -I - "$FSCK" "$WORK" "$1" <<'PY'
import os, struct, subprocess, sys

FSCK, DIR, MODE = sys.argv[1], sys.argv[2], sys.argv[3]

HDR = 357
MAGIC, TYPE, ID, NAME, MODE_AT = 0, 2, 3, 11, 267
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
    struct.pack_into('<H', h, MODE_AT, mode)
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
    stores.append((start + MODE_AT, struct.pack('<H', 0o100644)))
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

if MODE == 'prefixes':
    steps = [
        ('prefix-create', 'create', start, create_alpha, ()),
        ('prefix-grow', 'grow', after_create, grow_alpha, (b'alpha',)),
        ('prefix-nudge', 'grow by less than a header', after_grow, nudge_alpha,
         (b'alpha',)),
        ('prefix-shrink', 'shrink', after_nudge, shrink_alpha, (b'alpha',)),
        ('prefix-beside', 'create beside it', after_shrink, create_beta,
         (b'alpha',)),
        ('prefix-remove', 'remove', after_beta, remove_alpha, (b'beta',)),
        ('prefix-twonodes', 'name on two nodes', after_remove, move_beta,
         (b'beta',)),
    ]
    for ident, tag, img, stores, present in steps:
        why = every_prefix(tag, img, stores, present)
        print('bad %s %s' % (ident, why) if why else 'ok %s' % ident,
              flush=True)
else:
    # One rule of the protocol each, put back to front.
    wrong = [
        ('order-split', start, swap(create_alpha, create_alpha[0],
                                    create_alpha[1]), ()),
        ('order-length', after_create,
         swap(grow_alpha, grow_alpha[-2], grow_alpha[-1]), (b'alpha',)),
        # Only a growth shorter than a header can overlap: with a longer one
        # the tail's header lands in free space whichever order it is written
        # in.
        ('order-nudge', after_grow,
         swap(nudge_alpha, nudge_alpha[0], nudge_alpha[2]), (b'alpha',)),
        ('order-shrink', after_nudge,
         swap(shrink_alpha, shrink_alpha[1], shrink_alpha[2]), (b'alpha',)),
        ('order-copy', after_remove,
         move_beta[-removal_length:] + move_beta[:-removal_length],
         (b'beta',)),
    ]
    for ident, img, stores, present in wrong:
        if caught('wrong order', img, stores, present):
            print('ok %s' % ident, flush=True)
        else:
            print('bad %s a wrong order went unnoticed' % ident, flush=True)
PY
}

# report_protocol MODE: run the model in MODE and report each line it prints.
report_protocol() {
    protocol "$1" > "$WORK/protocol.out" 2> "$WORK/protocol.err" ||
        setup_failed "the model of the update protocol stopped; see $WORK/protocol.err"
    while read -r verdict id why; do
        case "$verdict" in
            ok) pass "$id" ;;
            bad) fail "$id" "$why" ;;
        esac
    done < "$WORK/protocol.out"
}

# Every stop inside each mutating operation leaves an image the check accepts
# or repairs, without a repeated name, a lost file or bytes nobody wrote.
case_prefixes() {
    ASSERTIONS="prefix-create prefix-grow prefix-nudge prefix-shrink prefix-beside"
    ASSERTIONS="$ASSERTIONS prefix-remove prefix-twonodes"
    report_protocol prefixes
}

# Each rule of the protocol put back to front is caught at some prefix.
case_orders() {
    ASSERTIONS="order-split order-length order-nudge order-shrink order-copy"
    report_protocol orders
}

# ---- The run. ----

ID=host.krffscheck.$CASE
if [ -n "${ETUDE_CASE:-}" ] && [ "$ETUDE_CASE" != "$ID" ]; then
    echo "krffscheck: ETUDE_CASE is $ETUDE_CASE, and the argument names $ID" >&2
    exit 2
fi
case "$CASE" in
    fresh|repairable|fatal|sizes|free|dirdup|prefixes|orders) ;;
    *) echo "krffscheck: no case $CASE" >&2; exit 2 ;;
esac
for tool in "$MKFS" "$FSCK" "$READFILE"; do
    [ -x "$tool" ] || { echo "krffscheck: $tool is not built; etude's preparation builds it" >&2; exit 2; }
done
record "begin $ID"
"case_$CASE"
for a in $ASSERTIONS; do
    case "$REPORTED" in
        *" $a "*) ;;
        *) notrun "$a" "not judged after the failure of$FAILED" ;;
    esac
done
record "end $RECORDS"
if [ -z "$FAILED" ]; then
    echo "krffscheck: result: passed: $ID"
    exit 0
fi
echo "krffscheck: result: failed: $ID:$FAILED"
exit 1
