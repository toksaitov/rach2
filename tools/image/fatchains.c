/*
 * fatchains: print the cluster chain of files on a FAT16 or FAT32 image.
 *
 *     fatchains IMAGE PATH...
 *
 * Each PATH is `::/dir/name` as mtools spells it, and its line is
 * `PATH <a-b> <c> ...`, one range per run of consecutive clusters, which
 * is the record `mshowfat` printed. That tool aborts at random on the
 * build host (mtools 4.0.49 on macOS, roughly one run in three), so the
 * disk builder verifies its staged chains with this program instead. A
 * path that is not there, a chain that ends in a bad entry, a FAT12 volume
 * and a volume whose fields make no sense all exit 1 with a line on stderr.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

static FILE *img;
static unsigned bytes_per_sector, sectors_per_cluster, reserved, nfats;
static unsigned root_entries, sectors_per_fat, root_cluster;
static unsigned long total_sectors, clusters, first_data_sector;
static unsigned active_fat; /* the FAT read: the one ExtFlags names when mirroring is off */
static int fat32;

static int fail(const char *what)
{
    fprintf(stderr, "fatchains: %s\n", what);
    return 1;
}

static int read_at(unsigned long offset, void *buffer, unsigned length)
{
    if (fseek(img, (long)offset, SEEK_SET) != 0) {
        return -1;
    }
    return fread(buffer, 1, length, img) == length ? 0 : -1;
}

static unsigned le16(const unsigned char *p)
{
    return p[0] | ((unsigned)p[1] << 8);
}

static unsigned long le32(const unsigned char *p)
{
    return le16(p) | ((unsigned long)le16(p + 2) << 16);
}

/** The FAT entry of `cluster`, or 0 on a read failure. */
static unsigned long fat_next(unsigned long cluster)
{
    unsigned char e[4];
    unsigned long offset =
        ((unsigned long)reserved + (unsigned long)active_fat * sectors_per_fat) * bytes_per_sector +
        cluster * (fat32 ? 4 : 2);

    if (read_at(offset, e, fat32 ? 4 : 2) != 0) {
        return 0;
    }
    return fat32 ? le32(e) & 0x0FFFFFFFul : le16(e);
}

static int chain_ends(unsigned long next)
{
    return fat32 ? next >= 0x0FFFFFF8ul : next >= 0xFFF8ul;
}

static unsigned long cluster_offset(unsigned long cluster)
{
    return (first_data_sector + (cluster - 2) * sectors_per_cluster) *
           (unsigned long)bytes_per_sector;
}

/**
 * The 8.3 name of `component` as a directory entry spells it, upper case;
 * -1 for a component a short entry cannot spell exactly: a base over eight
 * characters, an extension over three, a second dot, or an empty base.
 */
static int short_name(const char *component, char out[11])
{
    const char *dot = strchr(component, '.');
    unsigned n = dot ? (unsigned)(dot - component) : (unsigned)strlen(component);
    unsigned i;

    if (n == 0 || n > 8 || (dot && (strlen(dot + 1) > 3 || strchr(dot + 1, '.') != 0))) {
        return -1;
    }
    memset(out, ' ', 11);
    for (i = 0; i < n; i++) {
        out[i] = (char)toupper((unsigned char)component[i]);
    }
    if (dot) {
        for (i = 0; dot[1 + i] != 0; i++) {
            out[8 + i] = (char)toupper((unsigned char)dot[1 + i]);
        }
    }
    return 0;
}

/**
 * Find `name` (11 bytes, as a directory entry spells it) in the directory
 * whose data is `region` bytes at `offset`; set the entry's first cluster,
 * its size and whether it is a directory. 1 found, 0 not in this region,
 * 2 the directory ended (a zero entry ends the whole directory, not the
 * cluster), -1 a read failure.
 */
static int scan_region(unsigned long offset, unsigned long region, const char name[11],
                       unsigned long *first, unsigned long *size, int *is_dir)
{
    unsigned char e[32];
    unsigned long at;

    for (at = 0; at + 32 <= region; at += 32) {
        if (read_at(offset + at, e, 32) != 0) {
            return -1;
        }
        if (e[0] == 0) {
            return 2;
        }
        if (e[0] == 0xE5 || (e[11] & 0x08) || (e[11] & 0x0F) == 0x0F) {
            continue;
        }
        if (memcmp(e, name, 11) == 0) {
            *first = le16(e + 26) | (fat32 ? (unsigned long)le16(e + 20) << 16 : 0);
            *size = le32(e + 28);
            *is_dir = (e[11] & 0x10) != 0;
            return 1;
        }
    }
    return 0;
}

/** As `scan_region`, over the chain starting at `cluster`. */
static int scan_chain(unsigned long cluster, const char name[11], unsigned long *first,
                      unsigned long *size, int *is_dir)
{
    unsigned long steps = 0;

    while (cluster >= 2 && cluster < clusters + 2) {
        int r = scan_region(cluster_offset(cluster),
                            (unsigned long)sectors_per_cluster * bytes_per_sector, name, first,
                            size, is_dir);
        unsigned long next;

        if (r == 2) {
            return 0;
        }
        if (r != 0) {
            return r;
        }
        next = fat_next(cluster);
        if (chain_ends(next) || ++steps > clusters) {
            return 0;
        }
        cluster = next;
    }
    return 0;
}

/** Print the chain of `path` in mshowfat's shape; 0 done, -1 not done. */
static int print_chain(const char *path)
{
    char copy[512];
    char *component;
    unsigned long dir_cluster = fat32 ? root_cluster : 0;
    unsigned long first = 0;
    unsigned long size = 0;
    int is_dir = 1;
    unsigned long cluster, run_start, run_end, steps;

    if (strncmp(path, "::/", 3) != 0 || strlen(path) >= sizeof(copy)) {
        return fail("a path must start with ::/");
    }
    strcpy(copy, path + 3);
    for (component = strtok(copy, "/"); component != 0; component = strtok(0, "/")) {
        char name[11];
        int r;

        if (!is_dir) {
            fprintf(stderr, "fatchains: %s: not a directory on the way\n", path);
            return -1;
        }
        if (short_name(component, name) != 0) {
            fprintf(stderr, "fatchains: %s: not a short name\n", path);
            return -1;
        }
        if (!fat32 && dir_cluster == 0) {
            /* The FAT16 root directory is a fixed region after the FATs. */
            unsigned long offset =
                ((unsigned long)reserved + (unsigned long)nfats * sectors_per_fat) *
                bytes_per_sector;

            r = scan_region(offset, (unsigned long)root_entries * 32, name, &first, &size, &is_dir);
            if (r == 2) {
                r = 0;
            }
        } else {
            r = scan_chain(dir_cluster, name, &first, &size, &is_dir);
        }
        if (r < 0) {
            fprintf(stderr, "fatchains: %s: read failure\n", path);
            return -1;
        }
        if (r == 0) {
            fprintf(stderr, "fatchains: %s: not found\n", path);
            return -1;
        }
        dir_cluster = first;
    }
    if (!is_dir && size == 0 && first == 0) {
        printf("%s\n", path); /* an empty file has no chain */
        return 0;
    }
    if (first < 2 || first >= clusters + 2) {
        fprintf(stderr, "fatchains: %s: its first cluster is outside the volume\n", path);
        return -1;
    }
    printf("%s", path);
    cluster = first;
    steps = 0;
    run_start = run_end = 0;
    while (cluster >= 2 && cluster < clusters + 2) {
        unsigned long next;

        if (run_end != 0 && cluster == run_end + 1) {
            run_end = cluster;
        } else {
            if (run_end != 0) {
                printf(run_start == run_end ? " <%lu>" : " <%lu-%lu>", run_start, run_end);
            }
            run_start = run_end = cluster;
        }
        next = fat_next(cluster);
        if (chain_ends(next)) {
            break;
        }
        if (++steps > clusters || next < 2 || next >= clusters + 2) {
            printf("\n");
            fprintf(stderr, "fatchains: %s: the chain ends in a bad entry\n", path);
            return -1;
        }
        cluster = next;
    }
    if (run_end != 0) {
        printf(run_start == run_end ? " <%lu>" : " <%lu-%lu>", run_start, run_end);
    }
    printf("\n");
    return 0;
}

int main(int argc, char **argv)
{
    unsigned char bpb[64];
    unsigned long root_sectors, data_sectors;
    int i;

    if (argc < 3) {
        return fail("usage: fatchains IMAGE PATH...");
    }
    img = fopen(argv[1], "rb");
    if (img == 0) {
        return fail("cannot open the image");
    }
    if (read_at(0, bpb, sizeof(bpb)) != 0) {
        return fail("cannot read the boot sector");
    }
    bytes_per_sector = le16(bpb + 11);
    sectors_per_cluster = bpb[13];
    reserved = le16(bpb + 14);
    nfats = bpb[16];
    root_entries = le16(bpb + 17);
    total_sectors = le16(bpb + 19);
    if (total_sectors == 0) {
        total_sectors = le32(bpb + 32);
    }
    sectors_per_fat = le16(bpb + 22);
    if (sectors_per_fat == 0) {
        sectors_per_fat = (unsigned)le32(bpb + 36);
    }
    root_cluster = (unsigned)le32(bpb + 44);
    if (bytes_per_sector == 0 || sectors_per_cluster == 0 || nfats == 0 || sectors_per_fat == 0 ||
        total_sectors == 0) {
        return fail("the boot sector's fields make no sense");
    }
    root_sectors = ((unsigned long)root_entries * 32 + bytes_per_sector - 1) / bytes_per_sector;
    first_data_sector = reserved + (unsigned long)nfats * sectors_per_fat + root_sectors;
    if (first_data_sector >= total_sectors) {
        return fail("the boot sector's fields make no sense");
    }
    data_sectors = total_sectors - first_data_sector;
    clusters = data_sectors / sectors_per_cluster;
    if (clusters < 4085) {
        return fail("a FAT12 volume is not supported");
    }
    fat32 = clusters >= 65525 || root_entries == 0;
    /* The FAT read is the active one when FAT32 mirroring is off (ExtFlags
     * bit 7), else the first; and it must hold an entry per cluster. */
    active_fat = 0;
    if (fat32 && (bpb[40] & 0x80)) {
        active_fat = bpb[40] & 0x0F;
        if (active_fat >= nfats) {
            return fail("the active FAT is not one the volume has");
        }
    }
    if ((unsigned long)sectors_per_fat * bytes_per_sector < (clusters + 2) * (fat32 ? 4 : 2)) {
        return fail("the FAT is too small for the volume's clusters");
    }
    for (i = 2; i < argc; i++) {
        if (print_chain(argv[i]) != 0) {
            return 1;
        }
    }
    return 0;
}
