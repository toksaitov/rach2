/**
 * mkfatfixture.c: the builder of the FAT32 volumes the FAT32 driver's,
 * bootctl's and the boot tests use, and with `--check` a checker of a
 * FAT32 volume.
 *
 * It is written apart from the readers it tests: the builder writes the
 * volume from the FAT specification (BPB fields, two FAT copies, cluster
 * chains, short directory entries), while utils/bootslot.c and the
 * kernel's driver read it, so that each test compares two implementations
 * of the format. The well-formed image has the shapes a walk must skip (a
 * volume label, a long-name sequence, `.` and `..` entries in every
 * subdirectory) and the files the boot contract names; each option for a
 * malformed volume breaks one fact.
 *
 * The default geometry is a 4 MiB small FAT32 with 512-byte clusters, the
 * shape `mformat -F` gives the 16 MiB boot partition, scaled down so that
 * the tests stay fast: 32 reserved sectors, two 64-sector FATs, root
 * cluster 2.
 *
 * Usage: mkfatfixture <out> [option]
 * Options (at most one):
 *     --fats-differ --loop --out-of-range --short-chain --cross-link
 *     --bpb-sector-size --bpb-fat-oversized --bpb-root-out --bpb-overflow
 *     --no-signature --no-grubenv --small-candidate --fragmented --bad-fallback
 *     --fallback-slot
 *     --cfg-short --cluster8 --cluster8-past-end --menu-short --menu-odd
 *     --no-menu --vfs --active-second --unmasked --dir-loop
 *     --lost-chain --overlong-chain --bad-dotdot --dup-name --fsinfo
 *     --fsinfo-wrong --junk --dirty --dirty-surplus --dirty-orphan
 *     --dirty-mirror --dirty-cross --dirty-surplus-cross --dirty-empty-chain
 *
 * `mkfatfixture --check <image> [offset-sectors] [--list]` walks a volume
 * instead of building one; see the check section below. The last six
 * options are its fixtures: an allocated chain no entry names, a chain
 * four clusters longer than its size, a `..` naming the wrong directory, a
 * name twice in one directory, and FSInfo with the right or a wrong count
 * and hint. `--junk` is the FSInfo volume with every free cluster full of
 * bytes that would read as entries, for the writer's tests. The `--dirty`
 * family carries the dirty flag over a clean volume, a chain two clusters
 * past its size, orphan long-name entries, a mirror one entry behind, a
 * cross-link, and a file of size zero that keeps its cluster, which is
 * what the mount-time check must accept, repair, or refuse.
 *
 * `--vfs` is the volume the vfat driver's host test reads: the root's
 * chain is fragmented and crosses a 4 KiB boundary, and it holds a deleted
 * entry, long names (one across that boundary, one with a surrogate pair,
 * one whose checksum is wrong and must fall back to its lower-cased short
 * name, one over a short name starting in 0x05), a fragmented file and a
 * subdirectory whose `..` is the root, four long names no writer produces
 * (each shown by its alias), a file with every time field set, and a
 * directory of forty files. `--active-second` turns mirroring
 * off with the second FAT active and the first deliberately wrong;
 * `--unmasked` sets the reserved high nibble of the kernel chain's
 * entries, which a reader must mask before judging them. `--dir-loop` is
 * the `--vfs` volume with /SUB's chain linked to itself, a directory that
 * never ends. /SUB also holds two files with damaged chains, one running
 * past the data region and one shorter than its size.
 *
 * `--cluster8` builds the same files at eight sectors per cluster instead
 * of one, the geometry of the 64 MiB boot partition of an MBR disk, and
 * the one that shows bootslot's cluster arithmetic: at one sector per
 * cluster the multiplication by cluster_sectors is the identity.
 * `--cluster8-past-end` is that geometry with a chain running off the end
 * of the volume, so that the cluster count is checked as a bound and not
 * only reported. `--no-signature` leaves out the boot sector's signature.
 *
 * An unknown option builds the default volume.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SECTOR 512
#define TOTAL  8192

/* Sectors per cluster of every volume but those of `--cluster8`, at which
   the tests' cases assert their byte offsets. */
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
static int opt_known; /* nonzero once `is` has matched the option given */

/** Whether the option given is `name`. */
static int is(const char *name)
{
    if (strcmp(opt, name) != 0)
        return 0;
    opt_known = 1;
    return 1;
}

/** Store `v` at `p` as a little-endian 16-bit field. */
static void w16(unsigned char *p, unsigned v)
{
    p[0] = (unsigned char)v;
    p[1] = (unsigned char)(v >> 8);
}
/** Store `v` at `p` as a little-endian 32-bit field. */
static void w32(unsigned char *p, uint32_t v)
{
    p[0] = (unsigned char)v;
    p[1] = (unsigned char)(v >> 8);
    p[2] = (unsigned char)(v >> 16);
    p[3] = (unsigned char)(v >> 24);
}
/** The little-endian 16-bit field at `p`. */
static unsigned le16_of(const unsigned char *p)
{
    return (unsigned)p[0] | ((unsigned)p[1] << 8);
}

/** The little-endian 32-bit field at `p`. */
static uint32_t le32_of(const unsigned char *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/**
 * Write a minimal valid kernel of `size` bytes to `out`: ELF32 ET_EXEC
 * EM_386 with one PT_LOAD covering the file at 0x100000, the entry at its
 * start, the multiboot header at offset 96 and the multiboot2 header, with
 * only its end tag, at 112. A `phnum` of 0 makes an image with no loadable
 * segment.
 */
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
    w32(out + 112, 0xE85250D6u);
    w32(out + 116, 0);
    w32(out + 120, 24);
    w32(out + 124, (uint32_t)(0 - (0xE85250D6u + 24u)));
    w32(out + 128, 0);
    w32(out + 132, 8);
}

/**
 * Write a short directory entry at `p`: the 11-byte name, the attribute,
 * the first cluster and the size, with every time field zero.
 */
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

/** The checksum a long name carries for its short entry. */
static unsigned char short_sum(const char *name11)
{
    unsigned char sum = 0;
    int i;

    for (i = 0; i < 11; i++)
        sum = (unsigned char)(((sum & 1) ? 0x80 : 0) + (sum >> 1) + (unsigned char)name11[i]);
    return sum;
}

/**
 * Write one long-name entry at `p`: `order` (0x40 marks the last), the 13
 * UTF-16 units at their scattered offsets, type 0, the checksum, a zero
 * cluster.
 */
static void lfn_entry(unsigned char *p, unsigned order, unsigned char sum, const unsigned *units)
{
    static const int at[13] = {1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30};
    int i;

    memset(p, 0, 32);
    p[0] = (unsigned char)order;
    p[11] = 0x0F;
    p[13] = sum;
    for (i = 0; i < 13; i++)
        w16(p + at[i], units[i]);
}

/**
 * Write `name` (UTF-16 units, `n` of them) at `p` as the long-name
 * sequence of the short entry `short11`; returns the count of entries
 * written. A wrong checksum is written when `bad_sum` is set. The padding
 * past the terminator is 0xFFFF, as Windows writes it.
 */
static int lfn_write(unsigned char *p, const unsigned *name, int n, const char *short11,
                     int bad_sum)
{
    int entries = (n + 12) / 13;
    unsigned char sum = short_sum(short11);
    int e;

    if (bad_sum)
        sum = (unsigned char)(sum + 1);
    for (e = entries; e >= 1; e--) {
        unsigned units[13];
        int i;

        for (i = 0; i < 13; i++) {
            int k = (e - 1) * 13 + i;

            units[i] = k < n ? name[k] : (k == n ? 0 : 0xFFFF);
        }
        lfn_entry(p, (unsigned)e | (e == entries ? 0x40 : 0), sum, units);
        p += 32;
    }
    return entries;
}

/** Store the bytes of `s` in `units` as UTF-16 units; the count. */
static int ascii_units(const char *s, unsigned *units)
{
    int n = 0;

    while (s[n]) {
        units[n] = (unsigned char)s[n];
        n++;
    }
    return n;
}

/** Set the entry of `cluster` in the first FAT to `value`. */
static void link_cluster(unsigned long cluster, uint32_t value)
{
    w32(fat + cluster * 4, value);
}

/**
 * Link `count` clusters from `first` in order. The last links to
 * `cross_to`, or ends the chain when `cross_to` is 0.
 */
static void chain(unsigned long first, unsigned long count, unsigned long cross_to)
{
    unsigned long i;

    for (i = 0; i + 1 < count; i++)
        link_cluster(first + i, (uint32_t)(first + i + 1));
    link_cluster(first + count - 1, cross_to ? (uint32_t)cross_to : EOC);
}

/* ---- --check: a walk of a FAT32 volume ----
 *
 * The reader every write test relies on, written apart from the kernel's
 * driver and from mtools. It checks the boot sector field by field,
 * compares the two FATs where mirroring is on, walks every chain the
 * directory tree reaches with one owner for each cluster, and reports each
 * fault by the name it found it under: a loop, a cross-link, a chain
 * outside the data region, a chain shorter or longer than its size, a `..`
 * that is not the parent, a duplicate name, orphan long-name entries, lost
 * clusters, a wrong FSInfo, and a dirty flag in either place. `--list`
 * prints every entry it reaches beside the walk. The exit status is 0 with
 * no finding and 1 otherwise. */

/* The volume under check: its image of `bytes` bytes; from the BPB, the
   sectors per cluster, the reserved sectors, the count and size in sectors
   of the FATs, the root's cluster, the first data sector, the count of
   data clusters, the FAT read (the active one, 0 while mirroring is on)
   and the FSInfo sector; `fat`, the FAT read, in the image; `chains`, the
   last chain id given; the counts of findings, files and directories; and
   whether `--list` prints the entries. */
struct chk {
    unsigned char *img;
    size_t bytes;
    unsigned long spc, reserved, nfats, fatsz, root, data_start, clusters, active;
    unsigned long fsinfo_sector;
    unsigned char *fat;
    /* For each cluster, 0 when free or the id of the chain that reached it. */
    unsigned short *owner;
    unsigned short chains;
    int findings, files, dirs, list;
};

/** Print a finding and count it. */
static void finding(struct chk *c, const char *text)
{
    printf("check: %s\n", text);
    c->findings++;
}

/** The FAT entry of cluster `n`, its high nibble masked. */
static uint32_t chk_fat(const struct chk *c, unsigned long n)
{
    return le32_of(c->fat + n * 4) & 0x0FFFFFFFu;
}

/** Whether `n` is a cluster of the data region. */
static int chk_in_range(const struct chk *c, unsigned long n)
{
    return n >= 2 && n < c->clusters + 2;
}

/** The bytes of cluster `n` in the image. */
static unsigned char *chk_cluster(const struct chk *c, unsigned long n)
{
    return c->img + ((size_t)c->data_start + (n - 2) * c->spc) * SECTOR;
}

/**
 * Write to `out` the 8.3 entry `raw` as a name, lower-cased where the case
 * bits say so; a first byte of 0x05 stands for 0xE5.
 */
static void chk_alias(const unsigned char *raw, char *out)
{
    int i, n = 0;

    for (i = 0; i < 8 && raw[i] != ' '; i++) {
        unsigned char ch = raw[i] == 0x05 ? 0xE5 : raw[i];

        out[n++] = (char)((raw[12] & 0x08) && ch >= 'A' && ch <= 'Z' ? ch + 32 : ch);
    }
    if (raw[8] != ' ') {
        out[n++] = '.';
        for (i = 8; i < 11 && raw[i] != ' '; i++) {
            unsigned char ch = raw[i];

            out[n++] = (char)((raw[12] & 0x10) && ch >= 'A' && ch <= 'Z' ? ch + 32 : ch);
        }
    }
    out[n] = '\0';
}

/**
 * Walk the chain at `first`, marking each cluster with the current chain
 * id, `c->chains`, and report its faults under `name`. `expect` is the
 * count of clusters the size calls for, or -1 for none. Returns the count
 * of clusters, or -1 after a fault that ends the walk.
 */
static long chk_walk(struct chk *c, unsigned long first, const char *name, long expect)
{
    unsigned long n = first;
    long count = 0;
    char text[900];

    while (n != 0) {
        uint32_t next;

        if (!chk_in_range(c, n)) {
            snprintf(text, sizeof(text), "out of range: %s", name);
            finding(c, text);
            return -1;
        }
        if (c->owner[n] == c->chains) {
            snprintf(text, sizeof(text), "loop: %s", name);
            finding(c, text);
            return -1;
        }
        if (c->owner[n] != 0) {
            snprintf(text, sizeof(text), "cross-link: %s", name);
            finding(c, text);
            return -1;
        }
        c->owner[n] = c->chains;
        count++;
        next = chk_fat(c, n);
        if (next >= 0x0FFFFFF8u) {
            break;
        }
        if (next == 0x0FFFFFF7u || next < 2) {
            snprintf(text, sizeof(text), "chain ends in a %s entry: %s", next == 0 ? "free" : "bad",
                     name);
            finding(c, text);
            return -1;
        }
        n = next;
    }
    if (expect >= 0 && count < expect) {
        snprintf(text, sizeof(text), "short chain: %s", name);
        finding(c, text);
    } else if (expect >= 0 && count > expect) {
        snprintf(text, sizeof(text), "surplus: %s (%ld clusters)", name, count - expect);
        finding(c, text);
    }
    return count;
}

/** The checksum a long name carries for the short name `name11`. */
static unsigned char chk_sum(const unsigned char *name11)
{
    unsigned char sum = 0;
    int i;

    for (i = 0; i < 11; i++)
        sum = (unsigned char)(((sum & 1) ? 0x80 : 0) + (sum >> 1) + name11[i]);
    return sum;
}

#define CHK_MAX_ENTRIES      4096
#define CHK_MAX_DIR_CLUSTERS 4096 /* clusters of a directory the check reads */

/* A name seen in a directory: its short entry's name, and the name the
   kernel's lookup matches, the long name or else the alias. */
struct chk_name {
    unsigned char raw[11];
    char name[520];
};

/**
 * Whether `a` and `b` are equal with ASCII case folded, as the kernel's
 * lookup compares names.
 */
static int fold_eq(const char *a, const char *b)
{
    for (;; a++, b++) {
        int x = (unsigned char)*a, y = (unsigned char)*b;

        if (x >= 'A' && x <= 'Z')
            x += 32;
        if (y >= 'A' && y <= 'Z')
            y += 32;
        if (x != y)
            return 0;
        if (x == 0)
            return 1;
    }
}

/**
 * Write to `out`, of `cap` bytes, the UTF-16 units of a long name as UTF-8,
 * up to the terminator or the end of `count` units.
 */
static void chk_long_name(const unsigned *units, unsigned count, char *out, size_t cap)
{
    unsigned i = 0;
    size_t n = 0;

    while (i < count && units[i] != 0 && n + 5 < cap) {
        unsigned long cp = units[i++];

        if (cp >= 0xD800 && cp < 0xDC00 && i < count && units[i] >= 0xDC00 && units[i] < 0xE000)
            cp = 0x10000 + ((cp - 0xD800) << 10) + (units[i++] - 0xDC00);
        if (cp < 0x80) {
            out[n++] = (char)cp;
        } else if (cp < 0x800) {
            out[n++] = (char)(0xC0 | (cp >> 6));
            out[n++] = (char)(0x80 | (cp & 0x3F));
        } else if (cp < 0x10000) {
            out[n++] = (char)(0xE0 | (cp >> 12));
            out[n++] = (char)(0x80 | ((cp >> 6) & 0x3F));
            out[n++] = (char)(0x80 | (cp & 0x3F));
        } else {
            out[n++] = (char)(0xF0 | (cp >> 18));
            out[n++] = (char)(0x80 | ((cp >> 12) & 0x3F));
            out[n++] = (char)(0x80 | ((cp >> 6) & 0x3F));
            out[n++] = (char)(0x80 | (cp & 0x3F));
        }
    }
    out[n] = '\0';
}

/**
 * Whether a long-name entry with the last-entry flag starts a name: it is
 * the first entry pending, its sequence number is 1 to 20, which the
 * buffer of units holds, and bits 0x80 and 0x20 of its ordinal are clear.
 */
static int chk_lfn_start(unsigned char ordinal, int pending)
{
    int seq = ordinal & 0x1f;

    return pending == 1 && seq >= 1 && seq <= 20 && (ordinal & 0xa0) == 0;
}

/**
 * Walk the directory at `first`; `parent` is its parent's cluster, or zero
 * for the root. The directory's own chain is owned before its entries are
 * read. A long name has one start entry and at most twenty entries. A
 * duplicate is the same alias, or the same name as the kernel's lookup
 * matches it: the long name if there is one, else the alias, with ASCII
 * case folded. Each recursive call keeps its own cluster and name records.
 */
static void chk_dir(struct chk *c, unsigned long first, unsigned long parent, const char *path,
                    int depth)
{
    unsigned long *clusters_seen = malloc(sizeof(unsigned long) * CHK_MAX_DIR_CLUSTERS);
    struct chk_name *names = malloc(sizeof(struct chk_name) * CHK_MAX_ENTRIES);
    unsigned long n = first, nclusters = 0, i;
    int nnames = 0, lfn_ok = 0, lfn_expect = 0, lfn_pending = 0;
    unsigned char lfn_sum = 0;
    unsigned lfn_units[20 * 13];
    char longname[520];
    char text[900], alias[16], child[300];
    unsigned short id = ++c->chains;

    if (!clusters_seen || !names) {
        finding(c, "out of memory");
        goto out;
    }
    if (depth > 32) {
        snprintf(text, sizeof(text), "depth: %s", path);
        finding(c, text);
        goto out;
    }
    while (n != 0) {
        uint32_t next;

        if (!chk_in_range(c, n)) {
            snprintf(text, sizeof(text), "out of range: %s", path);
            finding(c, text);
            goto out;
        }
        if (c->owner[n] == id) {
            snprintf(text, sizeof(text), "loop: %s", path);
            finding(c, text);
            goto out;
        }
        if (c->owner[n] != 0) {
            snprintf(text, sizeof(text), "cross-link: %s", path);
            finding(c, text);
            goto out;
        }
        c->owner[n] = id;
        if (nclusters == CHK_MAX_DIR_CLUSTERS) {
            snprintf(text, sizeof(text), "directory longer than the check reads: %s", path);
            finding(c, text);
            goto out;
        }
        clusters_seen[nclusters++] = n;
        next = chk_fat(c, n);
        if (next >= 0x0FFFFFF8u)
            break;
        if (next == 0x0FFFFFF7u || next < 2) {
            snprintf(text, sizeof(text), "chain ends in a %s entry: %s", next == 0 ? "free" : "bad",
                     path);
            finding(c, text);
            goto out;
        }
        n = next;
    }
    c->dirs++;
    for (i = 0; i < nclusters; i++) {
        unsigned char *cl = chk_cluster(c, clusters_seen[i]);
        unsigned long e;

        for (e = 0; e < c->spc * SECTOR / 32; e++) {
            const unsigned char *raw = cl + e * 32;
            unsigned long fc;
            uint32_t size;
            int is_dir, j;

            if (raw[0] == 0 || raw[0] == 0xE5) {
                if (lfn_pending) {
                    snprintf(text, sizeof(text), "orphan long-name entries before %s in %s",
                             raw[0] == 0 ? "the end" : "a free entry", path[0] ? path : "/");
                    finding(c, text);
                }
                lfn_ok = 0;
                lfn_pending = 0;
                if (raw[0] == 0)
                    goto out;
                continue;
            }
            if (raw[11] == 0x0F) {
                int seq = raw[0] & 0x1F;

                lfn_pending++;
                if (raw[0] & 0x40) {
                    lfn_ok = chk_lfn_start(raw[0], lfn_pending);
                    lfn_expect = seq;
                    lfn_sum = raw[13];
                } else if (!lfn_ok || seq < 1 || seq != lfn_expect || (raw[0] & 0xE0) != 0 ||
                           raw[13] != lfn_sum) {
                    lfn_ok = 0;
                }
                if (lfn_ok) {
                    static const int at[13] = {1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30};
                    int k;

                    lfn_expect = seq - 1;
                    for (k = 0; k < 13 && seq >= 1 && seq <= 20; k++)
                        lfn_units[(seq - 1) * 13 + k] = le16_of(raw + at[k]);
                }
                continue;
            }
            if (raw[11] & 0x08) {
                lfn_ok = 0;
                lfn_pending = 0;
                continue; /* the volume label */
            }
            chk_alias(raw, alias);
            longname[0] = '\0';
            if (lfn_pending && !(lfn_ok && lfn_expect == 0 && chk_sum(raw) == lfn_sum)) {
                snprintf(text, sizeof(text), "orphan long-name entries before %s/%s", path, alias);
                finding(c, text);
            } else if (lfn_pending) {
                chk_long_name(lfn_units, (unsigned)lfn_pending * 13, longname, sizeof(longname));
            }
            lfn_ok = 0;
            lfn_expect = 0;
            lfn_pending = 0;
            fc = ((unsigned long)le16_of(raw + 20) << 16) | le16_of(raw + 26);
            size = le32_of(raw + 28);
            is_dir = (raw[11] & 0x10) != 0;
            if (memcmp(raw, ".          ", 11) == 0) {
                if (fc != first) {
                    snprintf(text, sizeof(text), "bad .: %s", path);
                    finding(c, text);
                }
                continue;
            }
            if (memcmp(raw, "..         ", 11) == 0) {
                if (fc != parent) {
                    snprintf(text, sizeof(text), "bad ..: %s", path);
                    finding(c, text);
                }
                continue;
            }
            for (j = 0; j < nnames; j++) {
                if (memcmp(names[j].raw, raw, 11) == 0 ||
                    fold_eq(names[j].name, longname[0] ? longname : alias)) {
                    snprintf(text, sizeof(text), "duplicate name: %s in %s", alias, path);
                    finding(c, text);
                    break;
                }
            }
            if (nnames == CHK_MAX_ENTRIES) {
                snprintf(text, sizeof(text),
                         "directory with more entries than the check compares: %s", path);
                finding(c, text);
                goto out;
            }
            memcpy(names[nnames].raw, raw, 11);
            snprintf(names[nnames].name, sizeof(names[nnames].name), "%s",
                     longname[0] ? longname : alias);
            nnames++;
            snprintf(child, sizeof(child), "%s/%s", path, alias);
            if (c->list)
                printf("check: entry %s %s %lu %lu%s%s\n", child, is_dir ? "dir" : "file", fc,
                       (unsigned long)size, longname[0] ? " long " : "", longname);
            if (is_dir) {
                if (fc == 0) {
                    snprintf(text, sizeof(text), "directory with no cluster: %s", child);
                    finding(c, text);
                } else {
                    chk_dir(c, fc, first == c->root ? 0 : first, child, depth + 1);
                }
            } else {
                long expect = (long)((size + c->spc * SECTOR - 1) / (c->spc * SECTOR));

                c->files++;
                if (fc == 0) {
                    if (size != 0) {
                        snprintf(text, sizeof(text), "short chain: %s", child);
                        finding(c, text);
                    }
                } else {
                    c->chains++;
                    chk_walk(c, fc, child, expect);
                }
            }
        }
    }
out:
    free(clusters_seen);
    free(names);
}

/**
 * Check the boot sector field by field and set up `c` from it, and compare
 * the two FATs when mirroring is on. Returns 0, or -1 after a finding that
 * ends the check.
 */
static int chk_bpb(struct chk *c)
{
    const unsigned char *b = c->img;
    unsigned long total, fatsz16, root_entries, version, ext_flags;

    if (c->bytes < 512 || b[510] != 0x55 || b[511] != 0xAA) {
        finding(c, "bpb: boot-sector signature");
        return -1;
    }
    fatsz16 = le16_of(b + 22);
    root_entries = le16_of(b + 17);
    version = le16_of(b + 42);
    c->fatsz = le32_of(b + 36);
    if (fatsz16 != 0 || c->fatsz == 0 || root_entries != 0 || version != 0) {
        finding(c, "bpb: not a FAT32 volume");
        return -1;
    }
    if (le16_of(b + 11) != 512) {
        finding(c, "bpb: sector size");
        return -1;
    }
    c->spc = b[13];
    if (c->spc == 0 || c->spc > 64 || (c->spc & (c->spc - 1)) != 0) {
        finding(c, "bpb: sectors per cluster");
        return -1;
    }
    c->reserved = le16_of(b + 14);
    c->nfats = b[16];
    total = le16_of(b + 19);
    if (total == 0)
        total = le32_of(b + 32);
    if (c->reserved == 0 || (c->nfats != 1 && c->nfats != 2) || total == 0 ||
        (size_t)total * SECTOR > c->bytes || c->reserved >= total ||
        c->fatsz > (total - c->reserved) / c->nfats) {
        finding(c, "bpb: reserved sectors, FAT count, total sectors or FAT regions");
        return -1;
    }
    c->data_start = c->reserved + c->nfats * c->fatsz;
    c->clusters = (total - c->data_start) / c->spc;
    if (c->clusters == 0 || c->fatsz * 128 < c->clusters + 2) {
        finding(c, "bpb: data region or FAT size");
        return -1;
    }
    c->root = le32_of(b + 44);
    if (!chk_in_range(c, c->root)) {
        finding(c, "bpb: root cluster");
        return -1;
    }
    ext_flags = le16_of(b + 40);
    c->active = (ext_flags & 0x80) ? (ext_flags & 0xF) : 0;
    if (c->active >= c->nfats) {
        finding(c, "bpb: active FAT");
        return -1;
    }
    c->fsinfo_sector = le16_of(b + 48);
    c->fat = c->img + (c->reserved + c->active * c->fatsz) * SECTOR;
    if (!(ext_flags & 0x80) && c->nfats == 2) {
        const unsigned char *other = c->img + (c->reserved + c->fatsz) * SECTOR;
        unsigned long n, differ = 0;

        for (n = 0; n < c->clusters + 2; n++)
            if ((le32_of(c->fat + n * 4) & 0x0FFFFFFFu) != (le32_of(other + n * 4) & 0x0FFFFFFFu))
                differ++;
        if (differ) {
            char text[80];

            snprintf(text, sizeof(text), "mirrors differ: %lu entries", differ);
            finding(c, text);
        }
    } else if (c->nfats == 2) {
        printf("check: mirroring off, FAT %lu active\n", c->active);
    }
    return 0;
}

/**
 * Check the volume `offset_sectors` sectors into the file `path`, printing
 * each finding and a line of result. Returns 0 with no finding, 1 with
 * findings, and 2 when the file cannot be read or memory runs out.
 */
static int check_image(const char *path, unsigned long offset_sectors, int list)
{
    struct chk c;
    FILE *f = fopen(path, "rb");
    long len;
    unsigned long n, lost = 0, first_lost = 0, freecount = 0;
    char text[120];

    memset(&c, 0, sizeof(c));
    c.list = list;
    if (f == NULL || fseek(f, 0, SEEK_END) != 0 || (len = ftell(f)) < 0) {
        fprintf(stderr, "mkfatfixture: cannot read %s\n", path);
        return 2;
    }
    if ((unsigned long)len < offset_sectors * SECTOR) {
        fprintf(stderr, "mkfatfixture: offset past the end of %s\n", path);
        return 2;
    }
    c.bytes = (size_t)len - offset_sectors * SECTOR;
    c.img = malloc(c.bytes ? c.bytes : 1);
    if (c.img == NULL || fseek(f, (long)(offset_sectors * SECTOR), SEEK_SET) != 0 ||
        fread(c.img, 1, c.bytes, f) != c.bytes) {
        fprintf(stderr, "mkfatfixture: cannot read %s\n", path);
        return 2;
    }
    fclose(f);
    if (chk_bpb(&c) != 0) {
        printf("check: FAIL: %d finding(s)\n", c.findings);
        return 1;
    }
    c.owner = calloc(c.clusters + 2, sizeof(unsigned short));
    if (c.owner == NULL)
        return 2;
    if (!(chk_fat(&c, 1) & 0x08000000u))
        finding(&c, "dirty: the FAT flag");
    if (c.img[65] & 1)
        finding(&c, "dirty: the boot-sector flag");
    chk_dir(&c, c.root, 0, "", 0);
    for (n = 2; n < c.clusters + 2; n++) {
        uint32_t v = chk_fat(&c, n);

        if (v == 0) {
            freecount++;
        } else if (v != 0x0FFFFFF7u && c.owner[n] == 0) {
            if (lost == 0)
                first_lost = n;
            lost++;
        }
    }
    if (lost) {
        snprintf(text, sizeof(text), "lost: %lu clusters (first %lu)", lost, first_lost);
        finding(&c, text);
    }
    if (c.fsinfo_sector != 0 && c.fsinfo_sector < c.reserved) {
        const unsigned char *fi = c.img + c.fsinfo_sector * SECTOR;

        if (le32_of(fi) != 0x41615252u || le32_of(fi + 484) != 0x61417272u ||
            le32_of(fi + 508) != 0xAA550000u) {
            finding(&c, "fsinfo: bad signature");
        } else {
            uint32_t said = le32_of(fi + 488), hint = le32_of(fi + 492);

            if (said != 0xFFFFFFFFu && said != freecount) {
                snprintf(text, sizeof(text), "fsinfo: free count %lu, FAT says %lu",
                         (unsigned long)said, freecount);
                finding(&c, text);
            }
            if (hint != 0xFFFFFFFFu && !chk_in_range(&c, hint)) {
                snprintf(text, sizeof(text), "fsinfo: hint %lu out of range", (unsigned long)hint);
                finding(&c, text);
            }
        }
    }
    if (c.findings == 0) {
        printf("check: OK: %d files, %d directories, %lu free clusters\n", c.files, c.dirs,
               freecount);
        return 0;
    }
    printf("check: FAIL: %d finding(s)\n", c.findings);
    return 1;
}

/**
 * Build the volume for `option` into `img` (TOTAL sectors), with `fat` and
 * `fat2` beside it; the caller writes it out and frees the three. Returns
 * 0, or 2 when memory runs out.
 *
 * The root holds a volume label, a long name for BOOT, valid so that a
 * checker finds no orphan (bootslot's walk skips it either way), and BOOT.
 * /BOOT holds the dot entries, KERNEL, FALLBACK, GRUB, EXTRA and the
 * rewritable menu slot BOOT.CFG, which bootslot_open requires as it
 * requires the kernel, the fallback and the environment block. /BOOT/GRUB
 * holds grub.cfg and grubenv, GRUB's environment block; /BOOT/EXTRA holds
 * an empty file with cluster zero. KERNEL, the candidate slot, has a padded
 * minimal kernel, and FALLBACK a minimal kernel.
 *
 * The layout follows the full candidate slot, so that a short candidate
 * leaves a hole instead of moving every file after it. With
 * `--fallback-slot` the candidate slot is as long as the fallback file, as
 * on a disk built with a rewritable fallback; the image is too small for
 * two slots of the full size. The environment block's 1024 bytes are two
 * clusters at 512 bytes and one at 4096, and its chain follows the size,
 * or the volume would fail its own check.
 *
 * `--bpb-overflow` sets a reserved count near the 16-bit edge, so that
 * arithmetic for the data region that adds without checking would wrap
 * past the total. `--cross-link` ends the candidate's chain inside
 * grub.cfg's, its length still matching its size, so that the shared
 * cluster is the one fault the walk reports. `--dirty-surplus-cross` runs
 * the fallback, walked first, past its size into grub.cfg's chain: a
 * surplus tail that is another file's clusters, which a repair before the
 * whole walk would free. `--junk` fills every free cluster with bytes
 * 0xE1, which read as neither a label nor a directory, so that a directory
 * grown without zeroing shows.
 *
 * The `--vfs` root's chain is clusters 2, 9 and 10, so that its third
 * cluster starts a new 4 KiB block. Cluster 2 is full; cluster 9 holds a
 * long name of sixteen entries whose short entry lands in cluster 10,
 * across the boundary, and cluster 10 continues with the long names no
 * writer produces, a file with every time field set (created 2020-03-04
 * 12:00:00 and 150 hundredths, modified 1999-12-31 23:59:58), a file whose
 * date names month 13, and /MANY. The data files lie above every cluster
 * bootctl's cases use. With `--dir-loop`, every slot of /SUB is taken, so
 * that no zero entry ends the listing before the chain comes round again.
 */
static int build_fat(const char *option)
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

    opt = option ? option : "";
    opt_known = 0;

    spc = (is("--cluster8") || is("--cluster8-past-end")) ? 8 : DEFAULT_SPC;
    cbytes = spc * SECTOR;
    clusters = (TOTAL - DATA_START) / spc;
    slot_clusters = KERNEL_CLUSTERS / spc;
    if (is("--fallback-slot")) {
        slot_clusters = FALLBACK_CLUSTERS / spc;
    }
    kernel_clusters = is("--small-candidate") ? (spc <= 16 ? 16 / spc : 1) : slot_clusters;
    fallback_clusters = FALLBACK_CLUSTERS / spc;
    kernel_first = KERNEL_FIRST;
    fallback_first = kernel_first + slot_clusters + 16;
    grubenv_first = fallback_first + fallback_clusters;
    env_clusters = (1024 + cbytes - 1) / cbytes;
    cfg_first = grubenv_first + env_clusters;
    extra_first = cfg_first + 1;
    menu_clusters = (MENU_BYTES + cbytes - 1) / cbytes;
    menu_first = extra_first + 1;

    img = calloc(TOTAL, SECTOR);
    fat = calloc(FATSZ, SECTOR);
    fat2 = calloc(FATSZ, SECTOR);
    if (img == NULL || fat == NULL || fat2 == NULL)
        return 2;

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
    w16(bpb + 24, 32); /* a geometry, which mtools requires to be nonzero */
    w16(bpb + 26, 4);
    fatsz = is("--bpb-fat-oversized") ? 0x40000 : FATSZ;
    w32(bpb + 36, (uint32_t)fatsz);
    root = is("--bpb-root-out") ? clusters + 100 : ROOT;
    w32(bpb + 44, (uint32_t)root);
    if (is("--active-second"))
        w16(bpb + 40, 0x0081); /* mirroring off, the second FAT active */
    if (is("--bpb-overflow")) {
        w16(bpb + 14, 0xFFF0);
    }
    if (!is("--no-signature")) {
        bpb[510] = 0x55;
        bpb[511] = 0xAA;
    }

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
    } else if (is("--overlong-chain")) {
        chain(kernel_first, kernel_clusters + 4, 0);
    } else if (is("--cross-link")) {
        chain(kernel_first, kernel_clusters - 1, cfg_first);
    } else {
        chain(kernel_first, kernel_clusters, 0);
    }
    chain(fallback_first, fallback_clusters, 0);
    chain(grubenv_first, env_clusters, 0);
    chain(cfg_first, 1, 0);
    chain(menu_first, menu_clusters, 0);
    if (is("--lost-chain"))
        chain(6000, 3, 0); /* allocated, holding bytes, named by no entry */

    if (is("--vfs") || is("--dir-loop")) {
        link_cluster(ROOT, 9);
        link_cluster(9, 10);
        link_cluster(10, EOC);
        chain(5000, 2, 5010);
        chain(5010, 1, 0);
        chain(5020, 1, 0);
        chain(5030, 1, 0);
        chain(5031, 1, 0);
        chain(5032, 1, 0);
        chain(5033, 1, 0);
        chain(5034, 1, 0);
        chain(5040, 2, clusters + 500); /* OUTRANGE.BIN: third link out */
        chain(5050, 1, 0);              /* SHORTCH.BIN: one cluster for three */
        chain(5060, 3, 0);              /* /MANY: forty-two entries */
        if (is("--dir-loop"))
            link_cluster(5020, 5020);
    }
    if (is("--unmasked")) {
        unsigned long c;

        for (c = kernel_first; c < kernel_first + kernel_clusters; c++)
            w32(fat + c * 4, le32_of(fat + c * 4) | 0xF0000000u);
    }

    memcpy(fat2, fat, (size_t)FATSZ * SECTOR);
    if (is("--fats-differ"))
        w32(fat2 + (kernel_first + 7) * 4, EOC);
    if (is("--active-second"))
        w32(fat + (kernel_first + 3) * 4, 0); /* the first FAT is wrong */
    memcpy(img + (size_t)RESERVED * SECTOR, fat, (size_t)FATSZ * SECTOR);
    memcpy(img + (size_t)(RESERVED + FATSZ) * SECTOR, fat2, (size_t)FATSZ * SECTOR);
    if (is("--dirty") || strncmp(opt, "--dirty-", 8) == 0) {
        w32(fat + 4, le32_of(fat + 4) & ~0x08000000u);
        w32(fat2 + 4, le32_of(fat2 + 4) & ~0x08000000u);
        bpb[65] |= 1;
        memcpy(img + (size_t)RESERVED * SECTOR, fat, (size_t)FATSZ * SECTOR);
        memcpy(img + (size_t)(RESERVED + FATSZ) * SECTOR, fat2, (size_t)FATSZ * SECTOR);
    }
    if (is("--dirty-surplus")) {
        /* /BOOT/GRUB/GRUB.CFG's chain runs two clusters past its size. */
        chain(cfg_first, 1, 7000);
        chain(7000, 2, 0);
        memcpy(fat2, fat, (size_t)FATSZ * SECTOR);
        memcpy(img + (size_t)RESERVED * SECTOR, fat, (size_t)FATSZ * SECTOR);
        memcpy(img + (size_t)(RESERVED + FATSZ) * SECTOR, fat2, (size_t)FATSZ * SECTOR);
    }
    if (is("--dirty-mirror")) {
        /* The mirror lags the active FAT by one entry. */
        w32(fat2 + 7100 * 4, EOC);
        memcpy(img + (size_t)(RESERVED + FATSZ) * SECTOR, fat2, (size_t)FATSZ * SECTOR);
    }
    if (is("--dirty-surplus-cross")) {
        chain(fallback_first, fallback_clusters, cfg_first);
        memcpy(fat2, fat, (size_t)FATSZ * SECTOR);
        memcpy(img + (size_t)RESERVED * SECTOR, fat, (size_t)FATSZ * SECTOR);
        memcpy(img + (size_t)(RESERVED + FATSZ) * SECTOR, fat2, (size_t)FATSZ * SECTOR);
    }
    if (is("--dirty-cross")) {
        /* /BOOT/GRUB/GRUB.CFG's chain continues into the fallback's. */
        chain(cfg_first, 1, fallback_first);
        memcpy(fat2, fat, (size_t)FATSZ * SECTOR);
        memcpy(img + (size_t)RESERVED * SECTOR, fat, (size_t)FATSZ * SECTOR);
        memcpy(img + (size_t)(RESERVED + FATSZ) * SECTOR, fat2, (size_t)FATSZ * SECTOR);
    }
    if (is("--fsinfo") || is("--fsinfo-wrong") || is("--junk")) {
        /* FSInfo in sector 1: the free count and the hint, right or wrong. */
        unsigned char *fi = img + SECTOR;
        unsigned long c, freecount = 0;

        for (c = 2; c < clusters + 2; c++)
            if ((le32_of(fat + c * 4) & 0x0FFFFFFFu) == 0)
                freecount++;
        w16(bpb + 48, 1);
        w32(fi, 0x41615252u);
        w32(fi + 484, 0x61417272u);
        w32(fi + 488, is("--fsinfo-wrong") ? 12345u : (uint32_t)freecount);
        w32(fi + 492, is("--fsinfo-wrong") ? 7u : (uint32_t)(menu_first + menu_clusters));
        w32(fi + 508, 0xAA550000u);
    }

#define CLUSTER_OFF(c) ((size_t)(DATA_START + ((c) - 2) * spc) * SECTOR)

    if (is("--junk")) {
        unsigned long c;

        for (c = 2; c < clusters + 2; c++)
            if ((le32_of(fat + c * 4) & 0x0FFFFFFFu) == 0)
                memset(img + CLUSTER_OFF(c), 0xE1, cbytes);
    }

    dir = img + CLUSTER_OFF(ROOT);
    dirent_at(dir, "FIXTURE    ", 0x08, 0, 0);
    {
        unsigned units[13];
        int i;

        for (i = 0; i < 13; i++)
            units[i] = i < 4 ? (unsigned)"boot"[i] : (i == 4 ? 0 : 0xFFFF);
        lfn_entry(dir + 32, 0x41, short_sum("BOOT       "), units);
    }
    dirent_at(dir + 64, "BOOT       ", 0x10, BOOTDIR, 0);

    if (is("--vfs") || is("--dir-loop")) {
        unsigned units[256];
        unsigned char *p = dir + 96;
        unsigned char *far;
        int n, i;
        static const char e5name[11] = {0x05, '5', 'F', 'I', 'L', 'E', ' ', ' ', 'T', 'X', 'T'};

        p[0] = 0xE5; /* a deleted entry */
        p += 32;
        n = ascii_units("long name with spaces.txt", units);
        p += 32 * lfn_write(p, units, n, "LONGNA~1TXT", 0);
        dirent_at(p, "LONGNA~1TXT", 0x20, 5030, 27);
        p += 32;
        units[0] = 0xD83D; /* U+1F600 as a surrogate pair */
        units[1] = 0xDE00;
        n = 2 + ascii_units(".txt", units + 2);
        p += 32 * lfn_write(p, units, n, "_~1     TXT", 0);
        dirent_at(p, "_~1     TXT", 0x20, 5031, 5);
        p += 32;
        n = ascii_units("should-not-appear.txt", units);
        p += 32 * lfn_write(p, units, n, "BADSUM  TXT", 1);
        dirent_at(p, "BADSUM  TXT", 0x20, 5032, 6);
        p[12] = 0x18; /* base and extension shown in lower case */
        p += 32;
        n = ascii_units("e5file.txt", units);
        p += 32 * lfn_write(p, units, n, e5name, 0);
        dirent_at(p, e5name, 0x20, 5033, 2);
        p += 32;
        dirent_at(p, "FRAG    BIN", 0x20, 5000, 1536);
        p += 32;
        dirent_at(p, "SUB        ", 0x10, 5020, 0);
        far = img + CLUSTER_OFF(9);
        for (i = 0; i < 208; i++)
            units[i] = (unsigned)('a' + (i % 26));
        units[204] = '.';
        units[205] = 't';
        units[206] = 'x';
        units[207] = 't';
        lfn_write(far, units, 208, "LONGLO~1TXT", 0);
        dirent_at(img + CLUSTER_OFF(10), "LONGLO~1TXT", 0x20, 5034, 8);
        p = img + CLUSTER_OFF(10) + 32;
        n = ascii_units("badord.txt", units);
        lfn_write(p, units, n, "BADORD  TXT", 0);
        p[0] |= 0x80;
        p += 32;
        dirent_at(p, "BADORD  TXT", 0x20, 5031, 5);
        p += 32;
        n = ascii_units("badattr.txt", units);
        lfn_write(p, units, n, "BADATTR TXT", 0);
        p[11] = 0x4F;
        p += 32;
        dirent_at(p, "BADATTR TXT", 0x20, 5031, 5);
        p += 32;
        n = ascii_units("badpad.txt", units);
        lfn_write(p, units, n, "BADPAD  TXT", 0);
        w16(p + 28, 0x0041); /* unit 11, past the terminator at unit 10 */
        p += 32;
        dirent_at(p, "BADPAD  TXT", 0x20, 5031, 5);
        p += 32;
        units[0] = 0xD83D; /* a high surrogate with no low one after it */
        n = 1 + ascii_units("lone.txt", units + 1);
        lfn_write(p, units, n, "LONE    TXT", 0);
        p += 32;
        dirent_at(p, "LONE    TXT", 0x20, 5031, 5);
        p += 32;
        dirent_at(p, "STAMP   TXT", 0x20, 5031, 5);
        p[13] = 150;
        w16(p + 14, (12u << 11) | (0u << 5) | 0u);
        w16(p + 16, (40u << 9) | (3u << 5) | 4u);
        w16(p + 22, (23u << 11) | (59u << 5) | 29u);
        w16(p + 24, (19u << 9) | (12u << 5) | 31u);
        p += 32;
        dirent_at(p, "BADDATE TXT", 0x20, 5031, 5);
        w16(p + 24, (19u << 9) | (13u << 5) | 1u);
        p += 32;
        dirent_at(p, "MANY       ", 0x10, 5060, 0);
        {
            unsigned char *m = img + CLUSTER_OFF(5060);
            char name11[12];

            dirent_at(m, ".          ", 0x10, 5060, 0);
            dirent_at(m + 32, "..         ", 0x10, 0, 0);
            for (i = 0; i < 40; i++) {
                memcpy(name11, "M00        ", 12);
                name11[1] = (char)('0' + i / 10);
                name11[2] = (char)('0' + i % 10);
                dirent_at(m + 64 + 32 * i, name11, 0x20, 5031, 5);
            }
        }
        memcpy(img + CLUSTER_OFF(5030), "the long-named file's bytes", 27);
        memcpy(img + CLUSTER_OFF(5031), "smile", 5);
        memcpy(img + CLUSTER_OFF(5032), "badsum", 6);
        memcpy(img + CLUSTER_OFF(5033), "e5", 2);
        for (i = 0; i < 1536; i++) {
            unsigned long c = i < 1024 ? 5000 + (unsigned long)(i / 512) : 5010;

            img[CLUSTER_OFF(c) + (size_t)(i % 512)] = (unsigned char)((i * 7) & 0xFF);
        }
        memcpy(img + CLUSTER_OFF(5034), "crossing", 8);
        /* /SUB: the dot entries, `..` naming the root by zero, three files. */
        dir = img + CLUSTER_OFF(5020);
        dirent_at(dir, ".          ", 0x10, 5020, 0);
        dirent_at(dir + 32, "..         ", 0x10, 0, 0);
        dirent_at(dir + 64, "INNER   TXT", 0x20, 5031, 5);
        dirent_at(dir + 96, "OUTRANGEBIN", 0x20, 5040, 1536);
        dirent_at(dir + 128, "SHORTCH BIN", 0x20, 5050, 1536);
        if (is("--dir-loop")) {
            char name11[12];

            for (i = 5; i < 16; i++) {
                memcpy(name11, "F00        ", 12);
                name11[1] = (char)('0' + i / 10);
                name11[2] = (char)('0' + i % 10);
                dirent_at(dir + 32 * i, name11, 0x20, 5031, 5);
            }
        }
    }

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

    dir = img + CLUSTER_OFF(GRUBDIR);
    dirent_at(dir, ".          ", 0x10, GRUBDIR, 0);
    dirent_at(dir + 32, "..         ", 0x10, is("--bad-dotdot") ? EXTRADIR : BOOTDIR, 0);
    dirent_at(dir + 64, "GRUB    CFG", 0x20, cfg_first, is("--cfg-short") ? 2000 : 40);
    if (is("--dirty-empty-chain"))
        w32(dir + 64 + 28, 0); /* the size, with the first cluster kept */
    if (!is("--no-grubenv"))
        dirent_at(dir + 96, "GRUBENV    ", 0x20, grubenv_first, 1024);

    dir = img + CLUSTER_OFF(EXTRADIR);
    dirent_at(dir, ".          ", 0x10, EXTRADIR, 0);
    dirent_at(dir + 32, "..         ", 0x10, BOOTDIR, 0);
    dirent_at(dir + 64, "EMPTY      ", 0x20, 0, 0);
    if (is("--dup-name"))
        dirent_at(dir + 96, "EMPTY      ", 0x20, 0, 0);
    if (is("--dirty-orphan")) {
        unsigned units[13];
        int i;

        for (i = 0; i < 13; i++)
            units[i] = i < 6 ? (unsigned)"orphan"[i] : (i == 6 ? 0 : 0xFFFF);
        lfn_entry(dir + 96, 0x42, 0x11, units);
        lfn_entry(dir + 128, 0x01, 0x11, units);
    }

    slot = calloc(1, kernel_size);
    if (slot == NULL)
        return 2;
    minimal_kernel(slot, 4096, 1);
    if (is("--fragmented")) {
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
    if (is("--lost-chain"))
        memset(img + CLUSTER_OFF(6000), 'L', 3 * cbytes);
    return 0;
}

/**
 * Build the volume for `option` as `build_fat` does, and refuse an option
 * no part of the build asks for, which is not one the maker knows. Returns
 * 0, or 2.
 */
static int build_known(const char *option)
{
    int err = build_fat(option);

    if (err == 0 && opt[0] != '\0' && !opt_known) {
        fprintf(stderr, "mkfatfixture: unknown option %s\n", opt);
        return 2;
    }
    return err;
}

/**
 * Build the volume for `option` for a caller that holds it in memory, such
 * as a fuzz harness: the image, with its length in `bytes`, or NULL when
 * memory runs out.
 */
unsigned char *mkfatfixture_build(const char *option, unsigned long *bytes)
{
    if (build_known(option) != 0) {
        return NULL;
    }
    *bytes = (unsigned long)TOTAL * SECTOR;
    return img;
}

#ifndef FIXTURE_NO_MAIN
/**
 * Build the volume the option names into the file `<out>`, or with
 * `--check`, check a volume. Exits 0; 1 when the file cannot be written, or
 * the check has findings; 2 for a usage error, a file the check cannot
 * read, or no memory.
 */
int main(int argc, char **argv)
{
    FILE *f;

    if (argc >= 3 && strcmp(argv[1], "--check") == 0) {
        unsigned long off = 0;
        int list = 0, i;

        for (i = 3; i < argc; i++) {
            if (strcmp(argv[i], "--list") == 0)
                list = 1;
            else
                off = strtoul(argv[i], NULL, 10);
        }
        return check_image(argv[2], off, list);
    }
    if (argc < 2 || argc > 3) {
        fprintf(stderr, "usage: mkfatfixture <out> [option] | mkfatfixture --check <image> "
                        "[offset-sectors] [--list]\n");
        return 2;
    }
    if (build_known(argc == 3 ? argv[2] : 0) != 0) {
        return 2;
    }
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
#endif
