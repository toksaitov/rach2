/**
 * mkfatfixture: build FAT32 fixture images for the bootctl host test.
 *
 * An independent implementation on purpose: this builder writes the volume
 * from the FAT specification (BPB fields, two FAT copies, cluster chains,
 * short directory entries), while utils/bootslot.c reads it, so the host
 * test compares two implementations of the format against each other. The
 * well-formed image has the shapes the walk must skip (a volume label, a
 * long-name sequence, `.` and `..` entries in every subdirectory) and the
 * four files the boot contract names; every malformed option breaks
 * exactly one fact.
 *
 * The default geometry is a 4 MiB small FAT32 with 512-byte clusters, the
 * shape `mformat -F` gives the real 16 MiB boot partition, scaled down so
 * the test stays fast: 32 reserved sectors, two 64-sector FATs, root
 * cluster 2.
 *
 * Usage: mkfatfixture <out> [option]
 * Options (at most one):
 *     --fats-differ --loop --out-of-range --short-chain --cross-link
 *     --bpb-sector-size --bpb-fat-oversized --bpb-root-out --bpb-overflow
 *     --no-grubenv --small-candidate --fragmented --bad-fallback
 *     --cfg-short --cluster8 --cluster8-past-end --menu-short --menu-odd
 *     --no-menu
 *
 * `--cluster8` builds the same files at eight sectors per cluster instead
 * of one, which is the geometry the sealed 64 MiB boot partition needs and
 * the only one that makes bootslot's cluster arithmetic visible: at one
 * sector per cluster the multiplication by cluster_sectors is the
 * identity. `--cluster8-past-end` is that geometry with a chain running
 * off the end of the volume, so the cluster count is checked as a bound
 * and not merely reported.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SECTOR 512
#define TOTAL  8192

/* Sectors per cluster. Every fixture was one-to-one until milestone 25,
   which is why bootslot's cluster arithmetic had never run at any other
   geometry: the sealed boot partition is 64 MiB, and at one sector per
   cluster that is past both BOOTSLOT_FAT_MAX_SECTORS and
   BOOTSLOT_MAX_CLUSTERS, so the real disk formats at eight. The default
   stays one, which keeps every existing case at the byte offsets it
   asserts on. */
#define DEFAULT_SPC 1
#define RESERVED    32
#define FATSZ       64
#define DATA_START  (RESERVED + 2 * FATSZ)
#define EOC         0x0FFFFFF8u

#define ROOT              2
#define BOOTDIR           3
#define GRUBDIR           4
#define EXTRADIR          5
#define KERNEL_FIRST      16
#define MENU_BYTES        8192
#define KERNEL_CLUSTERS   4096
#define FALLBACK_CLUSTERS 32

static unsigned char *img;
static unsigned char *fat, *fat2;
static const char *opt = "";

static int is(const char *name)
{
    return strcmp(opt, name) == 0;
}

static void w16(unsigned char *p, unsigned v)
{
    p[0] = (unsigned char)v;
    p[1] = (unsigned char)(v >> 8);
}
static void w32(unsigned char *p, uint32_t v)
{
    p[0] = (unsigned char)v;
    p[1] = (unsigned char)(v >> 8);
    p[2] = (unsigned char)(v >> 16);
    p[3] = (unsigned char)(v >> 24);
}

/* A minimal valid image: ELF32 ET_EXEC EM_386 with one PT_LOAD covering
   the file at 0x100000, the entry at its start, and the multiboot header
   at offset 96. `phnum` 0 crafts the no-loadable-segment shape. */
static void minimal_kernel(unsigned char *out, size_t size, int phnum)
{
    uint32_t magic = 0x1BADB002u, flags = 0x3u;
    size_t i;

    memset(out, 0, size);
    memcpy(out,
           "\x7f"
           "ELF",
           4);
    out[4] = 1;
    out[5] = 1;
    w16(out + 16, 2);
    w16(out + 18, 3);
    w32(out + 24, 0x100000);
    w32(out + 28, 52);
    w16(out + 42, 32);
    w16(out + 44, (unsigned)phnum);
    if (phnum) {
        w32(out + 52, 1);
        w32(out + 56, 0);
        w32(out + 60, 0x100000);
        w32(out + 64, 0x100000);
        w32(out + 68, (uint32_t)size);
        w32(out + 72, (uint32_t)size);
        w32(out + 76, 5);
        w32(out + 80, 0x1000);
    }
    w32(out + 96, magic);
    w32(out + 100, flags);
    w32(out + 104, (uint32_t)(0 - (magic + flags)));
    for (i = 128; i < size; i++)
        out[i] = (unsigned char)((i * 31) & 0xFF);
}

static void dirent_at(unsigned char *p, const char *name11, unsigned attr, unsigned long cluster,
                      uint32_t size)
{
    memset(p, 0, 32);
    memcpy(p, name11, 11);
    p[11] = (unsigned char)attr;
    w16(p + 20, (unsigned)(cluster >> 16));
    w16(p + 26, (unsigned)(cluster & 0xFFFF));
    w32(p + 28, size);
}

/* A long-name entry: attr 0x0F, otherwise arbitrary; the walk skips it. */
static void lfn_at(unsigned char *p, unsigned order)
{
    int i;

    memset(p, 0, 32);
    p[0] = (unsigned char)order;
    for (i = 1; i < 11; i++)
        p[i] = 0x41;
    p[11] = 0x0F;
}

static void link_cluster(unsigned long cluster, uint32_t value)
{
    w32(fat + cluster * 4, value);
}

static void chain(unsigned long first, unsigned long count, unsigned long cross_to)
{
    unsigned long i;

    for (i = 0; i + 1 < count; i++)
        link_cluster(first + i, (uint32_t)(first + i + 1));
    link_cluster(first + count - 1, cross_to ? (uint32_t)cross_to : EOC);
}

int main(int argc, char **argv)
{
    unsigned long spc, cbytes, clusters, slot_clusters, kernel_clusters;
    unsigned long fallback_clusters, kernel_first, fallback_first;
    unsigned long grubenv_first, env_clusters, cfg_first, extra_first;
    unsigned long menu_clusters, menu_first, kernel_size;
    unsigned char *bpb, *dir, *slot, *fb;
    unsigned sector_size;
    unsigned long fatsz, root;
    static const char env_head[] = "# GRUB Environment Block\n"
                                   "# WARNING: Do not edit this file by tools other than "
                                   "grub-editenv!!!\n"
                                   "saved_entry=candidate\n";
    unsigned char env[1024];
    FILE *f;

    if (argc < 2 || argc > 3) {
        fprintf(stderr, "usage: mkfatfixture <out> [option]\n");
        return 2;
    }
    if (argc == 3)
        opt = argv[2];

    spc = (is("--cluster8") || is("--cluster8-past-end")) ? 8 : DEFAULT_SPC;
    cbytes = spc * SECTOR;
    clusters = (TOTAL - DATA_START) / spc;
    slot_clusters = KERNEL_CLUSTERS / spc;
    kernel_clusters = is("--small-candidate") ? (spc <= 16 ? 16 / spc : 1) : slot_clusters;
    fallback_clusters = FALLBACK_CLUSTERS / spc;
    kernel_first = KERNEL_FIRST;
    /* The layout follows the full slot, so a short candidate leaves a hole
       instead of moving every file after it. */
    fallback_first = kernel_first + slot_clusters + 16;
    grubenv_first = fallback_first + fallback_clusters;
    /* 1024 bytes is two clusters at 512 bytes each and one at 4096; the
       chain has to follow the size or the volume fails its own
       consistency check. */
    env_clusters = (1024 + cbytes - 1) / cbytes;
    cfg_first = grubenv_first + env_clusters;
    extra_first = cfg_first + 1;
    /* The rewritable menu slot. bootslot_open requires it, the way it
       requires the kernel, the fallback and the environment block. */
    menu_clusters = (MENU_BYTES + cbytes - 1) / cbytes;
    menu_first = extra_first + 1;

    img = calloc(TOTAL, SECTOR);
    fat = calloc(FATSZ, SECTOR);
    fat2 = calloc(FATSZ, SECTOR);
    if (img == NULL || fat == NULL || fat2 == NULL)
        return 2;

    /* The BPB, mformat's small-FAT32 shape. */
    bpb = img;
    memcpy(bpb, "\xeb\x58\x90", 3);
    memcpy(bpb + 3, "MKFATFIX", 8);
    sector_size = is("--bpb-sector-size") ? 4096 : SECTOR;
    w16(bpb + 11, sector_size);
    bpb[13] = (unsigned char)spc;
    w16(bpb + 14, RESERVED);
    bpb[16] = 2;
    w16(bpb + 19, TOTAL);
    bpb[21] = 0xF8;
    fatsz = is("--bpb-fat-oversized") ? 0x40000 : FATSZ;
    w32(bpb + 36, (uint32_t)fatsz);
    root = is("--bpb-root-out") ? clusters + 100 : ROOT;
    w32(bpb + 44, (uint32_t)root);
    if (is("--bpb-overflow")) {
        /* A reserved count near the 16-bit edge: data_start arithmetic
           that adds without checking would wrap past the total. */
        w16(bpb + 14, 0xFFF0);
    }
    bpb[510] = 0x55;
    bpb[511] = 0xAA;

    /* The FAT: chains for the directories and files. */
    link_cluster(0, 0x0FFFFFF8u);
    link_cluster(1, 0x0FFFFFFFu);
    link_cluster(ROOT, EOC);
    link_cluster(BOOTDIR, EOC);
    link_cluster(GRUBDIR, EOC);
    link_cluster(EXTRADIR, EOC);

    if (is("--fragmented")) {
        /* Two runs with a hole between them; the chain crosses it. */
        unsigned long half = kernel_clusters / 2;

        chain(kernel_first, half, kernel_first + half + 8);
        chain(kernel_first + half + 8, kernel_clusters - half, 0);
    } else if (is("--loop")) {
        chain(kernel_first, kernel_clusters, kernel_first + 4);
    } else if (is("--out-of-range") || is("--cluster8-past-end")) {
        chain(kernel_first, kernel_clusters, clusters + 500);
    } else if (is("--short-chain")) {
        chain(kernel_first, kernel_clusters / 2, 0);
    } else if (is("--cross-link")) {
        /* The candidate's chain ends inside the grub.cfg chain, its
           length still matching its size, so the shared cluster is the
           one broken fact the walk reports. */
        chain(kernel_first, kernel_clusters - 1, cfg_first);
    } else {
        chain(kernel_first, kernel_clusters, 0);
    }
    chain(fallback_first, fallback_clusters, 0);
    chain(grubenv_first, env_clusters, 0);
    chain(cfg_first, 1, 0);
    chain(menu_first, menu_clusters, 0);

    memcpy(fat2, fat, (size_t)FATSZ * SECTOR);
    if (is("--fats-differ"))
        w32(fat2 + (kernel_first + 7) * 4, EOC);
    memcpy(img + (size_t)RESERVED * SECTOR, fat, (size_t)FATSZ * SECTOR);
    memcpy(img + (size_t)(RESERVED + FATSZ) * SECTOR, fat2, (size_t)FATSZ * SECTOR);

#define CLUSTER_OFF(c) ((size_t)(DATA_START + ((c) - 2) * spc) * SECTOR)

    /* The root: a volume label, a long-name sequence, then BOOT. */
    dir = img + CLUSTER_OFF(ROOT);
    dirent_at(dir, "FIXTURE    ", 0x08, 0, 0);
    lfn_at(dir + 32, 0x41);
    dirent_at(dir + 64, "BOOT       ", 0x10, BOOTDIR, 0);

    /* /BOOT: dot entries, the two kernel files, GRUB, and an extra
       subtree. */
    kernel_size = kernel_clusters * cbytes;
    dir = img + CLUSTER_OFF(BOOTDIR);
    dirent_at(dir, ".          ", 0x10, BOOTDIR, 0);
    dirent_at(dir + 32, "..         ", 0x10, 0, 0);
    dirent_at(dir + 64, "KERNEL     ", 0x20, kernel_first, (uint32_t)kernel_size);
    dirent_at(dir + 96, "FALLBACK   ", 0x20, fallback_first,
              (uint32_t)(fallback_clusters * cbytes));
    dirent_at(dir + 128, "GRUB       ", 0x10, GRUBDIR, 0);
    dirent_at(dir + 160, "EXTRA      ", 0x10, EXTRADIR, 0);
    if (!is("--no-menu")) {
        uint32_t menu_size = is("--menu-short") ? 0
                             : is("--menu-odd") ? MENU_BYTES - 1
                                                : MENU_BYTES;

        dirent_at(dir + 192, "BOOT    CFG", 0x20, menu_first, menu_size);
    }

    /* /BOOT/GRUB: dot entries, grub.cfg, and (unless absent) grubenv. */
    dir = img + CLUSTER_OFF(GRUBDIR);
    dirent_at(dir, ".          ", 0x10, GRUBDIR, 0);
    dirent_at(dir + 32, "..         ", 0x10, BOOTDIR, 0);
    dirent_at(dir + 64, "GRUB    CFG", 0x20, cfg_first, is("--cfg-short") ? 2000 : 40);
    if (!is("--no-grubenv"))
        dirent_at(dir + 96, "GRUBENV    ", 0x20, grubenv_first, 1024);

    /* /BOOT/EXTRA: an empty file (cluster zero) and the dot entries; the
       walk must take all of this in stride. */
    dir = img + CLUSTER_OFF(EXTRADIR);
    dirent_at(dir, ".          ", 0x10, EXTRADIR, 0);
    dirent_at(dir + 32, "..         ", 0x10, BOOTDIR, 0);
    dirent_at(dir + 64, "EMPTY      ", 0x20, 0, 0);

    /* File contents: the candidate slot has a valid padded kernel, the
       fallback a valid kernel, grubenv GRUB's block. */
    slot = calloc(1, kernel_size);
    if (slot == NULL)
        return 2;
    minimal_kernel(slot, 4096, 1);
    if (is("--fragmented")) {
        /* The content follows the chain across the hole. */
        unsigned long half = kernel_clusters / 2;
        size_t first_bytes = half * cbytes;
        unsigned long second = kernel_first + half + 8;

        memcpy(img + CLUSTER_OFF(kernel_first), slot, first_bytes);
        memcpy(img + CLUSTER_OFF(second), slot + first_bytes, kernel_size - first_bytes);
    } else {
        memcpy(img + CLUSTER_OFF(kernel_first), slot, kernel_size);
    }
    free(slot);
    fb = calloc(1, fallback_clusters * cbytes);
    if (fb == NULL)
        return 2;
    minimal_kernel(fb, fallback_clusters * cbytes, is("--bad-fallback") ? 0 : 1);
    memcpy(img + CLUSTER_OFF(fallback_first), fb, fallback_clusters * cbytes);
    free(fb);
    memset(env, '#', sizeof(env));
    memcpy(env, env_head, sizeof(env_head) - 1);
    memcpy(img + CLUSTER_OFF(grubenv_first), env, sizeof(env));
    memset(img + CLUSTER_OFF(cfg_first), 'x', 40);
    memset(img + CLUSTER_OFF(menu_first), '#', MENU_BYTES);

    f = fopen(argv[1], "wb");
    if (f == NULL || fwrite(img, 1, (size_t)TOTAL * SECTOR, f) != (size_t)TOTAL * SECTOR ||
        fclose(f) != 0) {
        fprintf(stderr, "mkfatfixture: cannot write %s\n", argv[1]);
        return 1;
    }
    free(img);
    free(fat);
    free(fat2);
    return 0;
}
