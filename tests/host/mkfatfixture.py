#!/usr/bin/env python3
"""Build FAT32 fixture images for the bootctl host test.

An independent implementation on purpose: this builder writes the volume from
the FAT specification (BPB fields, two FAT copies, cluster chains, short
directory entries), while utils/bootslot.c reads it, so the host test compares
two implementations of the format against each other. The well-formed image
has the shapes the walk must skip (a volume label, a long-name sequence,
`.` and `..` entries in every subdirectory) and the four files the boot
contract names; every malformed option breaks exactly one fact.

The default geometry is a 4 MiB small FAT32 with 512-byte clusters, the shape
`mformat -F` gives the real 16 MiB boot partition, scaled down so the test
stays fast: 32 reserved sectors, two 64-sector FATs, root cluster 2.

Usage:
    mkfatfixture.py <out> [option]
Options (at most one):
    --fats-differ --loop --out-of-range --short-chain --cross-link
    --bpb-sector-size --bpb-fat-oversized --bpb-root-out --bpb-overflow
    --no-grubenv --small-candidate --fragmented --bad-fallback --cfg-short
    --cluster8 --cluster8-past-end --menu-short --menu-odd --no-menu

--cluster8 builds the same files at eight sectors per cluster instead of one,
which is the geometry the sealed 64 MiB boot partition needs and the only one
that makes bootslot's cluster arithmetic visible: at one sector per cluster the
multiplication by cluster_sectors is the identity. --cluster8-past-end is that
geometry with a chain running off the end of the volume, so the cluster count
is checked as a bound and not merely reported.
"""
import struct
import sys

SECTOR = 512
TOTAL = 8192

# Sectors per cluster. Every fixture was one-to-one until milestone 25, which is
# why bootslot's cluster arithmetic had never run at any other geometry: the
# sealed boot partition is 64 MiB, and at one sector per cluster that is past
# both BOOTSLOT_FAT_MAX_SECTORS and BOOTSLOT_MAX_CLUSTERS, so the real disk
# formats at eight. `--cluster8` builds the same files that way, at the same
# byte sizes and so at eight times fewer clusters. The default stays one, which
# keeps every existing case at the byte offsets it asserts on.
DEFAULT_SPC = 1
RESERVED = 32
FATSZ = 64
DATA_START = RESERVED + 2 * FATSZ
CLUSTERS = (TOTAL - DATA_START)
EOC = 0x0FFFFFF8

ROOT = 2
BOOTDIR = 3
GRUBDIR = 4
EXTRADIR = 5
KERNEL_FIRST = 16
MENU_BYTES = 8192
KERNEL_CLUSTERS = 4096
FALLBACK_FIRST = KERNEL_FIRST + KERNEL_CLUSTERS + 16
FALLBACK_CLUSTERS = 32
GRUBENV_FIRST = FALLBACK_FIRST + FALLBACK_CLUSTERS
CFG_FIRST = GRUBENV_FIRST + 2
EXTRA_FIRST = CFG_FIRST + 1


def minimal_kernel(size, phnum=1):
    """A minimal valid image: ELF32 ET_EXEC EM_386 with one PT_LOAD covering
    the file at 0x100000, the entry at its start, and the multiboot header at
    offset 96. phnum=0 crafts the no-loadable-segment shape."""
    img = bytearray(size)
    img[0:4] = b"\x7fELF"
    img[4] = 1  # 32-bit
    img[5] = 1  # little-endian
    struct.pack_into("<H", img, 16, 2)  # ET_EXEC
    struct.pack_into("<H", img, 18, 3)  # EM_386
    struct.pack_into("<I", img, 24, 0x100000)  # e_entry
    struct.pack_into("<I", img, 28, 52)  # e_phoff
    struct.pack_into("<H", img, 42, 32)  # e_phentsize
    struct.pack_into("<H", img, 44, phnum)
    if phnum:
        # PT_LOAD: offset 0, vaddr/paddr 0x100000, filesz = memsz = size.
        struct.pack_into("<IIIIIIII", img, 52, 1, 0, 0x100000, 0x100000,
                         size, size, 5, 0x1000)
    magic, flags = 0x1BADB002, 0x3
    struct.pack_into("<III", img, 96, magic, flags,
                     (-(magic + flags)) & 0xFFFFFFFF)
    for i in range(128, size):
        img[i] = (i * 31) & 0xFF
    return bytes(img)


def dirent(name11, attr, cluster, size):
    e = bytearray(32)
    e[0:11] = name11
    e[11] = attr
    struct.pack_into("<H", e, 20, cluster >> 16)
    struct.pack_into("<H", e, 26, cluster & 0xFFFF)
    struct.pack_into("<I", e, 28, size)
    return bytes(e)


def lfn_entry(order):
    """A long-name entry: attr 0x0F, otherwise arbitrary; the walk skips it."""
    e = bytearray(32)
    e[0] = order
    for i in range(1, 11):
        e[i] = 0x41
    e[11] = 0x0F
    return bytes(e)


def main(argv):
    if len(argv) < 2 or len(argv) > 3:
        print(__doc__, file=sys.stderr)
        return 2
    out = argv[1]
    opt = argv[2] if len(argv) == 3 else ""

    spc = 8 if opt in ("--cluster8", "--cluster8-past-end") else DEFAULT_SPC
    cbytes = spc * SECTOR
    clusters = (TOTAL - DATA_START) // spc
    slot_clusters = KERNEL_CLUSTERS // spc
    kernel_clusters = (16 // spc if spc <= 16 else 1) if opt == "--small-candidate" else slot_clusters
    fallback_clusters = FALLBACK_CLUSTERS // spc
    kernel_first = KERNEL_FIRST
    # The layout follows the full slot, so a short candidate leaves a hole
    # rather than moving every file after it.
    fallback_first = kernel_first + slot_clusters + 16
    grubenv_first = fallback_first + fallback_clusters
    # 1024 bytes is two clusters at 512 bytes each and one at 4096; the chain
    # has to follow the size or the volume fails its own consistency check.
    env_clusters = (1024 + cbytes - 1) // cbytes
    cfg_first = grubenv_first + env_clusters
    extra_first = cfg_first + 1
    # The rewritable menu slot. bootslot_open requires it, the way it requires
    # the kernel, the fallback and the environment block.
    menu_bytes = MENU_BYTES
    menu_clusters = (menu_bytes + cbytes - 1) // cbytes
    menu_first = extra_first + 1

    img = bytearray(TOTAL * SECTOR)

    # The BPB, mformat's small-FAT32 shape.
    bpb = bytearray(SECTOR)
    bpb[0:3] = b"\xeb\x58\x90"
    bpb[3:11] = b"MKFATFIX"
    sector_size = 4096 if opt == "--bpb-sector-size" else SECTOR
    struct.pack_into("<H", bpb, 11, sector_size)
    bpb[13] = spc  # sectors per cluster
    struct.pack_into("<H", bpb, 14, RESERVED)
    bpb[16] = 2  # FATs
    struct.pack_into("<H", bpb, 19, TOTAL)  # total16, like mformat
    bpb[21] = 0xF8
    fatsz = 0x40000 if opt == "--bpb-fat-oversized" else FATSZ
    struct.pack_into("<I", bpb, 36, fatsz)
    root = clusters + 100 if opt == "--bpb-root-out" else ROOT
    struct.pack_into("<I", bpb, 44, root)
    if opt == "--bpb-overflow":
        # A reserved count near the 16-bit edge: data_start arithmetic that
        # adds without checking would wrap past the total.
        struct.pack_into("<H", bpb, 14, 0xFFF0)
    bpb[510] = 0x55
    bpb[511] = 0xAA
    img[0:SECTOR] = bpb

    # The FAT: chains for the directories and files.
    fat = bytearray(FATSZ * SECTOR)

    def link(cluster, value):
        struct.pack_into("<I", fat, cluster * 4, value)

    def chain(first, count, cross_to=None):
        for i in range(count - 1):
            link(first + i, first + i + 1)
        link(first + count - 1, cross_to if cross_to else EOC)

    link(0, 0x0FFFFFF8)
    link(1, 0x0FFFFFFF)
    for d in (ROOT, BOOTDIR, GRUBDIR, EXTRADIR):
        link(d, EOC)

    if opt == "--fragmented":
        # Two runs with a hole between them; the chain crosses the hole.
        half = kernel_clusters // 2
        chain(kernel_first, half, cross_to=kernel_first + half + 8)
        chain(kernel_first + half + 8, kernel_clusters - half)
    elif opt == "--loop":
        chain(kernel_first, kernel_clusters,
              cross_to=kernel_first + 4)
    elif opt == "--out-of-range" or opt == "--cluster8-past-end":
        chain(kernel_first, kernel_clusters, cross_to=clusters + 500)
    elif opt == "--short-chain":
        chain(kernel_first, kernel_clusters // 2)
    elif opt == "--cross-link":
        # The candidate's chain ends inside the grub.cfg chain, its length
        # still matching its size, so the shared cluster is the one broken
        # fact the walk reports.
        chain(kernel_first, kernel_clusters - 1, cross_to=cfg_first)
    else:
        chain(kernel_first, kernel_clusters)
    chain(fallback_first, fallback_clusters)
    chain(grubenv_first, env_clusters)
    chain(cfg_first, 1)
    chain(menu_first, menu_clusters)

    fat2 = bytearray(fat)
    if opt == "--fats-differ":
        struct.pack_into("<I", fat2, (kernel_first + 7) * 4, EOC)
    img[RESERVED * SECTOR:(RESERVED + FATSZ) * SECTOR] = fat
    img[(RESERVED + FATSZ) * SECTOR:DATA_START * SECTOR] = fat2

    def cluster_off(c):
        return (DATA_START + (c - 2) * spc) * SECTOR

    # The root: a volume label, a long-name sequence, then BOOT.
    rootdir = bytearray(cbytes)
    ents = [dirent(b"FIXTURE    ", 0x08, 0, 0), lfn_entry(0x41),
            dirent(b"BOOT       ", 0x10, BOOTDIR, 0)]
    rootdir[0:32 * len(ents)] = b"".join(ents)
    img[cluster_off(ROOT):cluster_off(ROOT) + cbytes] = rootdir

    # /BOOT: dot entries, the two kernel files, GRUB, and an extra subtree.
    kernel_size = kernel_clusters * cbytes
    bootdir = bytearray(cbytes)
    ents = [dirent(b".          ", 0x10, BOOTDIR, 0),
            dirent(b"..         ", 0x10, 0, 0),
            dirent(b"KERNEL     ", 0x20, kernel_first, kernel_size),
            dirent(b"FALLBACK   ", 0x20, fallback_first,
                   fallback_clusters * cbytes),
            dirent(b"GRUB       ", 0x10, GRUBDIR, 0),
            dirent(b"EXTRA      ", 0x10, EXTRADIR, 0),
            dirent(b"BOOT    CFG", 0x20, menu_first,
                   {"--menu-short": 0, "--menu-odd": menu_bytes - 1}.get(opt, menu_bytes))]
    if opt == "--no-menu":
        ents = ents[:-1]
    bootdir[0:32 * len(ents)] = b"".join(ents)
    img[cluster_off(BOOTDIR):cluster_off(BOOTDIR) + cbytes] = bootdir

    # /BOOT/GRUB: dot entries, grub.cfg, and (unless absent) grubenv.
    grubdir = bytearray(cbytes)
    ents = [dirent(b".          ", 0x10, GRUBDIR, 0),
            dirent(b"..         ", 0x10, BOOTDIR, 0),
            dirent(b"GRUB    CFG", 0x20, cfg_first,
                   2000 if opt == "--cfg-short" else 40)]
    if opt != "--no-grubenv":
        ents.append(dirent(b"GRUBENV    ", 0x20, grubenv_first, 1024))
    grubdir[0:32 * len(ents)] = b"".join(ents)
    img[cluster_off(GRUBDIR):cluster_off(GRUBDIR) + cbytes] = grubdir

    # /BOOT/EXTRA: an empty file (cluster zero) and the dot entries; the walk
    # must take all of this in stride.
    extradir = bytearray(cbytes)
    ents = [dirent(b".          ", 0x10, EXTRADIR, 0),
            dirent(b"..         ", 0x10, BOOTDIR, 0),
            dirent(b"EMPTY      ", 0x20, 0, 0)]
    extradir[0:32 * len(ents)] = b"".join(ents)
    img[cluster_off(EXTRADIR):cluster_off(EXTRADIR) + cbytes] = extradir

    # File contents: the candidate slot has a valid padded kernel, the
    # fallback a valid kernel, grubenv GRUB's block.
    slot = minimal_kernel(4096) + bytes(kernel_size - 4096)
    if opt == "--fragmented":
        # The content follows the chain across the hole.
        half = kernel_clusters // 2
        first_bytes = half * cbytes
        img[cluster_off(kernel_first):
            cluster_off(kernel_first) + first_bytes] = slot[:first_bytes]
        second = kernel_first + half + 8
        img[cluster_off(second):
            cluster_off(second) + len(slot) - first_bytes] = slot[first_bytes:]
    else:
        img[cluster_off(kernel_first):
            cluster_off(kernel_first) + len(slot)] = slot
    fb = minimal_kernel(fallback_clusters * cbytes,
                        phnum=0 if opt == "--bad-fallback" else 1)
    img[cluster_off(fallback_first):cluster_off(fallback_first) + len(fb)] = fb
    env = (b"# GRUB Environment Block\n"
           b"# WARNING: Do not edit this file by tools other than "
           b"grub-editenv!!!\n"
           b"saved_entry=candidate\n")
    env = env + b"#" * (1024 - len(env))
    img[cluster_off(grubenv_first):cluster_off(grubenv_first) + 1024] = env
    img[cluster_off(cfg_first):cluster_off(cfg_first) + 40] = b"x" * 40
    img[cluster_off(menu_first):cluster_off(menu_first) + menu_bytes] = b"#" * menu_bytes

    with open(out, "wb") as f:
        f.write(img)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
