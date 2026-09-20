/**
 * growset: the block ranges `resize2fs` writes when it grows an ext2 volume.
 *
 * Usage: `growset [--save] [--lba START] [--map JSON] VOLUME SECTORS`,
 * where VOLUME is a file holding the volume (an image, or the partition
 * copied out of a
 * disk) and SECTORS is the size the partition will have after the grow, in
 * sectors of 512 bytes. It reads the superblock and the descriptors of the
 * volume as it is, computes the geometry the grown volume will have the
 * way `resize2fs` does, and prints one `range START COUNT WHAT` line per
 * run of sectors the grow writes, sorted and merged, relative to the
 * volume's start, or to the disk's when `--lba` gives the partition's
 * first sector. The last lines are `total SECTORS` and the geometry.
 *
 * `--map JSON` checks the set against a grow that ran: JSON is what
 * `qemu-img map --output=json` printed for the overlay the grow wrote
 * into, and every extent at depth 0, which is one the overlay holds, must
 * lie inside the set. An extent outside it is printed as `outside START
 * LENGTH`, in sectors, and the status is 1.
 *
 * `--save` includes the complete seed and requires a complete file.
 *
 * `--ranges` prints only sector pairs, without descriptions or totals.
 *
 * Recovery uses this same calculation before saving or writing.
 *
 * What a grow writes, with the resize inode's reserve holding the larger
 * descriptor table so that nothing is moved: the primary superblock and
 * every backup copy with the descriptor table and the reserve after each,
 * since the table is rewritten in every copy and the reserve's blocks in
 * group 0 are the resize inode's pointer blocks, which gain an entry for
 * every new backup group; the block and inode bitmaps of every group, old
 * and new, because `resize2fs` marks both bitmaps dirty as a whole and the
 * library then writes every one; the new groups' inode tables, zeroed; the
 * resize inode's double-indirect block and the inode-table block that
 * holds the inode. A volume whose new descriptor table would not fit the
 * reserve is refused, because that grow moves blocks and this tool does
 * not compute where to.
 *
 * This is the save set milestone 50 needs before a grow on the installed
 * machine: the sectors to copy out first. `tests/run grow` checks the set
 * against what a grow in QEMU changed.
 */
#define _POSIX_C_SOURCE 200809L
#include <sys/types.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SECTOR     512u
#define MAX_RANGES 65536 /* the ranges one save set holds; more is refused */

struct range {
    unsigned long long start, count;
    const char *what;
};

static struct range ranges[MAX_RANGES];
static int nranges;

static unsigned rd16(const unsigned char *p)
{
    return (unsigned)p[0] | ((unsigned)p[1] << 8);
}

static unsigned long rd32(const unsigned char *p)
{
    return (unsigned long)p[0] | ((unsigned long)p[1] << 8) | ((unsigned long)p[2] << 16) |
           ((unsigned long)p[3] << 24);
}

static int is_power(unsigned long g, unsigned long base)
{
    while (g % base == 0) {
        g /= base;
    }
    return g == 1;
}

/* With sparse_super, groups 0 and 1 and the powers of 3, 5 and 7 keep a
   superblock copy, each a power of one base alone. */
static int has_super(int sparse, unsigned long g)
{
    if (!sparse || g <= 1) {
        return 1;
    }
    return is_power(g, 3) || is_power(g, 5) || is_power(g, 7);
}

static void add(unsigned long long block, unsigned long long count, const char *what)
{
    if (nranges == MAX_RANGES) {
        fprintf(stderr, "growset: more than %d ranges\n", MAX_RANGES);
        exit(1);
    }
    ranges[nranges].start = block;
    ranges[nranges].count = count;
    ranges[nranges].what = what;
    nranges++;
}

static int by_start(const void *a, const void *b)
{
    const struct range *x = a, *y = b;

    return x->start < y->start ? -1 : x->start > y->start;
}

static int read_at(FILE *f, unsigned long long at, void *buf, size_t n)
{
    return fseeko(f, (off_t)at, SEEK_SET) == 0 && fread(buf, 1, n, f) == n ? 0 : -1;
}

/* Whether sectors [start, end) lie inside the merged, sorted set. */
static int inside(unsigned long long start, unsigned long long end)
{
    int i;

    for (i = 0; i < nranges; i++) {
        if (ranges[i].start <= start && start < ranges[i].start + ranges[i].count) {
            return end <= ranges[i].start + ranges[i].count;
        }
    }
    return 0;
}

/* The number after `key` in a JSON object's text, or -1. */
static long long json_number(const char *obj, const char *key)
{
    const char *p = strstr(obj, key);

    if (!p) {
        return -1;
    }
    p += strlen(key);
    while (*p == ' ' || *p == ':') {
        p++;
    }
    return strtoll(p, 0, 10);
}

/**
 * Check `qemu-img map --output=json` output in `path` against the set:
 * every extent at depth 0 must be inside it. The objects are read one at
 * a time between braces; the keys are `start`, `length` and `depth`.
 */
static int check_map(const char *path)
{
    FILE *f = fopen(path, "rb");
    char obj[1024];
    int c, n = 0, in = 0, extents = 0, outside = 0;

    if (!f) {
        perror(path);
        return 1;
    }
    while ((c = fgetc(f)) != EOF) {
        if (c == '{') {
            in = 1;
            n = 0;
        } else if (c == '}' && in) {
            long long start, length, depth;

            obj[n] = '\0';
            in = 0;
            start = json_number(obj, "\"start\"");
            length = json_number(obj, "\"length\"");
            depth = json_number(obj, "\"depth\"");
            if (start < 0 || length <= 0 || depth != 0) {
                continue;
            }
            extents++;
            if (!inside((unsigned long long)start / SECTOR,
                        ((unsigned long long)start + (unsigned long long)length + SECTOR - 1) /
                            SECTOR)) {
                printf("outside %llu %llu\n", (unsigned long long)start / SECTOR,
                       ((unsigned long long)length + SECTOR - 1) / SECTOR);
                outside++;
            }
        } else if (in && n < (int)sizeof(obj) - 1) {
            obj[n++] = (char)c;
        }
    }
    fclose(f);
    printf("coverage: %d extents written, %d outside the set\n", extents, outside);
    return outside ? 1 : 0;
}

/**
 * Compute the write ranges for a supported ext2 grow.
 *
 * Require 4096-byte blocks and a resize inode with enough descriptor reserve
 * to avoid moving blocks. Reject a recorded RAID stride or stripe width,
 * and check each existing group's bitmap positions against the plain layout:
 * the group's start after any superblock, descriptors and reserve. Clearing
 * the recorded stride does not move bitmaps; resize2fs derives its placement
 * from the layout itself.
 *
 * Drop a final partial group when it cannot fit its tables and fifty blocks,
 * as resize2fs does. Sort and merge the computed ranges and express them in
 * sectors relative to the volume or, with `--lba`, to the disk.
 * With a nonzero `--lba`, include sectors 0 through 7 because the grow test
 * records writes in 4096-byte qcow2 clusters.
 */
int main(int argc, char **argv)
{
    unsigned long long lba = 0, sectors, new_blocks, total = 0;
    unsigned char sb[1024], raw[128];
    unsigned char *gd;
    unsigned long bs, bpg, ipg, inode_size, first_data, blocks, groups, groups_new, gd_old, gd_new,
        reserved, itb, rem, overhead, g, itable0, dind;
    const char *map = 0;
    int sparse, resize, save = 0, bare = 0, arg = 1, i, merged = 0, result;
    FILE *f;

    while (argc > arg + 2 && argv[arg][0] == '-') {
        if (strcmp(argv[arg], "--save") == 0) {
            save = 1;
            arg++;
            continue;
        } else if (strcmp(argv[arg], "--ranges") == 0) {
            bare = 1;
            arg++;
            continue;
        } else if (strcmp(argv[arg], "--lba") == 0) {
            lba = strtoull(argv[arg + 1], 0, 10);
        } else if (strcmp(argv[arg], "--map") == 0) {
            map = argv[arg + 1];
        } else {
            break;
        }
        arg += 2;
    }
    if (argc != arg + 2) {
        fprintf(stderr,
                "usage: growset [--save] [--ranges] [--lba START] [--map JSON] VOLUME SECTORS\n");
        return 2;
    }
    sectors = strtoull(argv[arg + 1], 0, 10);
    f = fopen(argv[arg], "rb");
    if (!f) {
        perror(argv[arg]);
        return 1;
    }
    if (read_at(f, 1024, sb, sizeof(sb)) != 0 || rd16(sb + 56) != 0xEF53) {
        fprintf(stderr, "growset: %s: no ext2 superblock\n", argv[arg]);
        return 1;
    }
    if (rd32(sb + 24) != 2) {
        fprintf(stderr, "growset: only 4096-byte blocks are supported\n");
        return 1;
    }
    bs = 4096;
    blocks = rd32(sb + 4);
    first_data = rd32(sb + 20);
    bpg = rd32(sb + 32);
    ipg = rd32(sb + 40);
    inode_size = rd32(sb + 76) >= 1 ? rd16(sb + 88) : 128;
    sparse = (rd32(sb + 100) & 0x0001ul) != 0;
    resize = (rd32(sb + 92) & 0x0010ul) != 0;
    reserved = resize ? rd16(sb + 206) : 0;
    if (!resize) {
        fprintf(stderr,
                "growset: %s: the volume is not the 4096-byte-block, resize_inode "
                "profile this computes for\n",
                argv[arg]);
        return 1;
    }
    if (first_data != 0 || bpg == 0 || bpg > bs * 8 || ipg == 0 || ipg > bs * 8 || blocks < bpg ||
        inode_size < 128 || inode_size > bs || bs % inode_size != 0 ||
        (rd32(sb + 92) & ~0x10ul) != 0 || (rd32(sb + 96) & ~0x02ul) != 0 ||
        (rd32(sb + 100) & ~0x03ul) != 0) {
        fprintf(stderr, "growset: unsupported ext2 geometry or features\n");
        return 1;
    }
    if (save && (fseeko(f, 0, SEEK_END) != 0 ||
                 (unsigned long long)ftello(f) != (unsigned long long)blocks * bs)) {
        fprintf(stderr, "growset: the seed length differs from its block count\n");
        return 1;
    }
    if (rd16(sb + 356) != 0 || rd16(sb + 358) != 0) {
        fprintf(stderr,
                "growset: %s: the volume declares a RAID stride or stripe width, "
                "and the grow would place its bitmaps by them\n",
                argv[arg]);
        return 1;
    }
    itb = (ipg * inode_size + bs - 1) / bs;
    groups = (blocks - first_data + bpg - 1) / bpg;
    gd_old = (groups * 32 + bs - 1) / bs;
    new_blocks = sectors * SECTOR / bs;
    if (new_blocks > 0xFFFFFFFFull) {
        new_blocks = 0xFFFFFFFFull;
    }
    if (new_blocks < blocks) {
        fprintf(stderr, "growset: %llu sectors is smaller than the volume\n", sectors);
        return 1;
    }
    groups_new = (unsigned long)((new_blocks - first_data + bpg - 1) / bpg);
    gd_new = (groups_new * 32 + bs - 1) / bs;
    rem = (unsigned long)((new_blocks - first_data) % bpg);
    overhead = (has_super(sparse, groups_new - 1) ? 1 + gd_new + reserved : 0) + 2 + itb;
    if (rem != 0 && rem < overhead + 50) {
        groups_new--;
        new_blocks = first_data + (unsigned long long)groups_new * bpg;
        gd_new = (groups_new * 32 + bs - 1) / bs;
    }
    if (gd_new > gd_old + reserved) {
        fprintf(stderr,
                "growset: the descriptor table would take %lu blocks and the reserve "
                "holds %lu; that grow moves blocks\n",
                gd_new, gd_old + reserved);
        return 1;
    }
    gd = malloc(groups * 32);
    if (!gd || read_at(f, (unsigned long long)(first_data + 1) * bs, gd, groups * 32) != 0) {
        fprintf(stderr, "growset: the descriptors did not read\n");
        return 1;
    }
    for (g = 0; g < groups; g++) {
        const unsigned char *d = gd + g * 32;
        unsigned long long start = first_data + (unsigned long long)g * bpg;
        unsigned long long plain = start + (has_super(sparse, g) ? 1 + gd_old + reserved : 0);

        if (rd32(d + 0) != plain || rd32(d + 4) != plain + 1) {
            fprintf(stderr,
                    "growset: %s: group %lu has its bitmaps at %lu and %lu and the plain "
                    "layout puts them at %llu and %llu; the grow would place a new "
                    "group's by the same rule\n",
                    argv[arg], g, (unsigned long)rd32(d + 0), (unsigned long)rd32(d + 4), plain,
                    plain + 1);
            return 1;
        }
    }
    itable0 = rd32(gd + 8);
    if (read_at(f, (unsigned long long)itable0 * bs + 6 * inode_size, raw, sizeof(raw)) != 0) {
        fprintf(stderr, "growset: the resize inode did not read\n");
        return 1;
    }
    dind = rd32(raw + 40 + 13 * 4);
    fclose(f);

    if (save) {
        add(0, blocks, "the complete seed overwrite");
    }
    add(0, 1, "the primary superblock");
    add(itable0 + (6 * inode_size) / bs, 1, "the inode table block of the resize inode");
    if (dind != 0) {
        add(dind, 1, "the resize inode's double-indirect block");
    }
    for (g = 0; g < groups_new; g++) {
        unsigned long long start = first_data + (unsigned long long)g * bpg;

        if (has_super(sparse, g)) {
            add(start, 1 + gd_new + reserved, "a superblock copy, the descriptors and the reserve");
        }
        if (g < groups) {
            const unsigned char *d = gd + g * 32;

            add(rd32(d + 0), 1, "a block bitmap");
            add(rd32(d + 4), 1, "an inode bitmap");
        } else {
            unsigned long long at = start + (has_super(sparse, g) ? 1 + gd_new + reserved : 0);

            add(at, 2 + itb, "a new group's bitmaps and inode table");
        }
    }
    qsort(ranges, nranges, sizeof(ranges[0]), by_start);
    for (i = 0; i < nranges;) {
        unsigned long long start = ranges[i].start, end = start + ranges[i].count;
        const char *what = ranges[i].what;
        int j = i + 1;

        while (j < nranges && ranges[j].start <= end) {
            if (ranges[j].start + ranges[j].count > end) {
                end = ranges[j].start + ranges[j].count;
            }
            if (strcmp(ranges[j].what, what) != 0) {
                what = "several";
            }
            j++;
        }
        ranges[merged].start = lba + start * (bs / SECTOR);
        ranges[merged].count = (end - start) * (bs / SECTOR);
        ranges[merged].what = what;
        merged++;
        i = j;
    }
    nranges = merged;
    if (lba > 0) {
        add(0, 8, "the partition table");
        qsort(ranges, nranges, sizeof(ranges[0]), by_start);
    }
    for (i = 0; i < nranges; i++) {
        if (bare) {
            printf("%llu %llu\n", ranges[i].start, ranges[i].count);
        } else {
            printf("range %llu %llu %s\n", ranges[i].start, ranges[i].count, ranges[i].what);
        }
        total += ranges[i].count;
    }
    if (!bare) {
        printf("total %llu\n", total);
        printf("blocks %llu groups %lu descriptors %lu of %lu\n", new_blocks, groups_new, gd_new,
               gd_old + reserved);
    }
    result = map ? check_map(map) : 0;
    if (fflush(stdout) != 0 || ferror(stdout)) {
        fprintf(stderr, "growset: writing the range list failed: %s\n", strerror(errno));
        return 1;
    }
    return result;
}
