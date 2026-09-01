/*
 * mkexfatfixture.c: an exFAT reader from Microsoft's published specification,
 * independent of the kernel's driver, the pair of mkfatfixture.c for FAT32.
 *
 *   mkexfatfixture --check IMAGE [OFFSET-SECTORS] [--list] [--dump]
 *
 * `--check` walks a volume: both boot regions and their checksums, the FAT,
 * the root's allocation bitmap and up-case table, every entry set the tree
 * reaches with its checksum and its name hash through the up-case table,
 * and every chain against the bitmap. A finding is one line, `check: WHAT:
 * detail`, and the last line is `check: OK: ...` or `check: FAIL: N
 * finding(s)`. `--list` prints every entry; `--dump` prints the fields of
 * the boot sector, the up-case and bitmap entries and the first file set as
 * hex beside the values the specification gives them, which is the record
 * `dev-log/38-exfat/notes.md` keeps of a volume another implementation
 * wrote. The builder half is phase 2's.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SECTOR 512u

struct vol {
    unsigned char *img;
    size_t bytes;
    unsigned bps_shift, spc_shift, nfats;
    uint64_t length;
    uint32_t fat_offset, fat_length, heap_offset, clusters, root, flags;
    uint32_t active_fat;
    unsigned char *bitmap; /* decoded: one byte per cluster, 0 free */
    uint32_t bitmap_first, upcase_first;
    uint64_t bitmap_length, upcase_length;
    uint16_t *upcase; /* 65536 mappings */
    unsigned short *owner;
    unsigned short chains;
    int findings, files, dirs, list, dump, first_set_dumped;
    unsigned bitmap_index; /* the entry the bitmap was taken from */
};

static uint16_t rd16(const unsigned char *p)
{
    return (uint16_t)(p[0] | (p[1] << 8));
}

static uint32_t rd32(const unsigned char *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint64_t rd64(const unsigned char *p)
{
    return (uint64_t)rd32(p) | ((uint64_t)rd32(p + 4) << 32);
}

static void finding(struct vol *v, const char *text)
{
    v->findings++;
    printf("check: %s\n", text);
}

static unsigned bytes_per_sector(const struct vol *v)
{
    return 1u << v->bps_shift;
}

static unsigned cluster_bytes(const struct vol *v)
{
    return 1u << (v->bps_shift + v->spc_shift);
}

static unsigned char *sector(const struct vol *v, uint64_t n)
{
    return v->img + n * bytes_per_sector(v);
}

static unsigned char *cluster(const struct vol *v, uint32_t c)
{
    return sector(v, v->heap_offset + ((uint64_t)(c - 2) << v->spc_shift));
}

static int in_heap(const struct vol *v, uint32_t c)
{
    return c >= 2 && c < v->clusters + 2;
}

static uint32_t fat_entry(const struct vol *v, uint32_t c)
{
    return rd32(sector(v, v->fat_offset + (uint64_t)v->active_fat * v->fat_length) + (size_t)c * 4);
}

/* The boot checksum over the eleven sectors before the checksum sector,
   skipping VolumeFlags and PercentInUse, which a mount changes. */
static uint32_t boot_checksum(const struct vol *v, uint64_t first)
{
    uint32_t sum = 0;
    unsigned bps = bytes_per_sector(v);
    uint64_t i;

    for (i = 0; i < (uint64_t)bps * 11; i++) {
        if (i == 106 || i == 107 || i == 112)
            continue;
        sum = ((sum & 1) ? 0x80000000u : 0) + (sum >> 1) + sector(v, first)[i];
    }
    return sum;
}

static uint16_t set_checksum(const unsigned char *entries, unsigned count)
{
    uint16_t sum = 0;
    unsigned i;

    for (i = 0; i < count * 32; i++) {
        if (i == 2 || i == 3)
            continue;
        sum = (uint16_t)(((sum & 1) ? 0x8000 : 0) + (sum >> 1) + entries[i]);
    }
    return sum;
}

static uint16_t name_hash(const struct vol *v, const uint16_t *units, unsigned n)
{
    uint16_t sum = 0;
    unsigned i;

    for (i = 0; i < n; i++) {
        uint16_t u = v->upcase[units[i]];

        sum = (uint16_t)(((sum & 1) ? 0x8000 : 0) + (sum >> 1) + (u & 0xFF));
        sum = (uint16_t)(((sum & 1) ? 0x8000 : 0) + (sum >> 1) + (u >> 8));
    }
    return sum;
}

static uint32_t table_checksum(const unsigned char *p, uint64_t n)
{
    uint32_t sum = 0;
    uint64_t i;

    for (i = 0; i < n; i++)
        sum = ((sum & 1) ? 0x80000000u : 0) + (sum >> 1) + p[i];
    return sum;
}

/* Decompress an up-case table: a 0xFFFF followed by a count is that many
   identity mappings; anything else maps the next unit. Returns the number
   of mappings filled, 65536 for a whole table. */
static uint32_t decompress_upcase(const unsigned char *stored, uint64_t length, uint16_t *table)
{
    uint64_t at = 0;
    uint32_t u = 0;

    while (at + 1 < length && u < 65536) {
        uint16_t w = rd16(stored + at);

        at += 2;
        if (w == 0xFFFF && at + 1 < length) {
            uint16_t run = rd16(stored + at);

            at += 2;
            while (run-- > 0 && u < 65536) {
                table[u] = (uint16_t)u;
                u++;
            }
        } else {
            table[u++] = w;
        }
    }
    return u;
}

static void utf8_of(const uint16_t *units, unsigned n, char *out, size_t cap)
{
    size_t o = 0;
    unsigned i;

    for (i = 0; i < n && o + 5 < cap; i++) {
        uint32_t cp = units[i];

        if (cp >= 0xD800 && cp < 0xDC00 && i + 1 < n && units[i + 1] >= 0xDC00 &&
            units[i + 1] < 0xE000) {
            cp = 0x10000 + ((cp - 0xD800) << 10) + (units[i + 1] - 0xDC00);
            i++;
        }
        if (cp < 0x80) {
            out[o++] = (char)cp;
        } else if (cp < 0x800) {
            out[o++] = (char)(0xC0 | (cp >> 6));
            out[o++] = (char)(0x80 | (cp & 0x3F));
        } else if (cp < 0x10000) {
            out[o++] = (char)(0xE0 | (cp >> 12));
            out[o++] = (char)(0x80 | ((cp >> 6) & 0x3F));
            out[o++] = (char)(0x80 | (cp & 0x3F));
        } else {
            out[o++] = (char)(0xF0 | (cp >> 18));
            out[o++] = (char)(0x80 | ((cp >> 12) & 0x3F));
            out[o++] = (char)(0x80 | ((cp >> 6) & 0x3F));
            out[o++] = (char)(0x80 | (cp & 0x3F));
        }
    }
    out[o] = 0;
}

/* A timestamp's fields as text: the 32-bit field, the 10 ms increment and
   the UTC offset byte, decoded as the specification lays them out. */
static void stamp_text(uint32_t ts, unsigned char inc, unsigned char off, char *out, size_t cap)
{
    unsigned year = 1980 + (ts >> 25), month = (ts >> 21) & 0xF, day = (ts >> 16) & 0x1F;
    unsigned hour = (ts >> 11) & 0x1F, minute = (ts >> 5) & 0x3F, second = (ts & 0x1F) * 2;
    int valid = (off & 0x80) != 0;
    int units = (int)(off & 0x7F);

    if (units >= 64)
        units -= 128; /* a signed 7-bit count of 15-minute steps */
    second += inc / 100;
    snprintf(out, cap, "%04u-%02u-%02u %02u:%02u:%02u.%02u %s%d", year, month, day, hour, minute,
             second, inc % 100, valid ? "utc" : "local", valid ? units * 15 : 0);
}

/* ---- the walk ---- */

static long walk_chain(struct vol *v, uint32_t first, uint64_t length, int nofat, const char *path);

static void check_dir(struct vol *v, uint32_t first, uint64_t length, int nofat, const char *path,
                      int depth);

/* Own every cluster of a chain and count it; a chain is read from the FAT,
   or, with NoFatChain, as `length` bytes of consecutive clusters. A length
   of zero on a FAT chain is the root's, whose length no entry records. */
static long walk_chain(struct vol *v, uint32_t first, uint64_t length, int nofat, const char *path)
{
    char text[400];
    unsigned short id = ++v->chains;
    uint32_t c = first;
    long count = 0;
    uint64_t need = (length + cluster_bytes(v) - 1) / cluster_bytes(v);

    while (c != 0 && (!nofat || (uint64_t)count < need)) {
        uint32_t next;

        if (!in_heap(v, c)) {
            snprintf(text, sizeof(text), "out of range: %s", path);
            finding(v, text);
            return -1;
        }
        if (v->owner[c] == id) {
            snprintf(text, sizeof(text), "loop: %s", path);
            finding(v, text);
            return -1;
        }
        if (v->owner[c] != 0) {
            snprintf(text, sizeof(text), "cross-link: %s", path);
            finding(v, text);
            return -1;
        }
        if (!v->bitmap[c - 2]) {
            snprintf(text, sizeof(text), "cluster free in the bitmap: %s", path);
            finding(v, text);
            return -1;
        }
        v->owner[c] = id;
        count++;
        if (nofat) {
            c++;
            continue;
        }
        next = fat_entry(v, c);
        if (next == 0xFFFFFFFFu)
            break;
        if (next == 0xFFFFFFF7u || next < 2) {
            snprintf(text, sizeof(text), "chain ends in a %s entry: %s", next == 0 ? "free" : "bad",
                     path);
            finding(v, text);
            return -1;
        }
        c = next;
    }
    if ((uint64_t)count < need) {
        snprintf(text, sizeof(text), "short chain: %s", path);
        finding(v, text);
        return -1;
    }
    if (!nofat && length != 0 && (uint64_t)count > need) {
        snprintf(text, sizeof(text), "surplus: %s (%lu clusters)", path,
                 (unsigned long)((uint64_t)count - need));
        finding(v, text);
    }
    return count;
}

/* Read entry `i` of a directory whose clusters are known. */
static const unsigned char *dir_entry(const struct vol *v, const uint32_t *cl, unsigned ncl,
                                      unsigned i)
{
    unsigned per = cluster_bytes(v) / 32;

    if (i / per >= ncl)
        return 0;
    return cluster(v, cl[i / per]) + (i % per) * 32;
}

static void dump_set(const unsigned char *e, unsigned n)
{
    unsigned i, k;

    for (i = 0; i < n; i++) {
        printf("dump: set entry %u:", i);
        for (k = 0; k < 32; k++)
            printf("%s%02x", k % 8 == 0 ? "  " : " ", e[i * 32 + k]);
        printf("\n");
    }
}

static void check_dir(struct vol *v, uint32_t first, uint64_t length, int nofat, const char *path,
                      int depth)
{
    char text[400], child[300], name[800];
    uint32_t *cl;
    unsigned ncl = 0, i;
    uint32_t c = first;
    long n;

    if (depth > 32) {
        snprintf(text, sizeof(text), "depth: %s", path);
        finding(v, text);
        return;
    }
    n = walk_chain(v, first, length, nofat, path);
    if (n < 0)
        return;
    cl = malloc(sizeof(uint32_t) * (size_t)n);
    if (!cl) {
        finding(v, "out of memory");
        return;
    }
    while (ncl < (unsigned)n) {
        cl[ncl++] = c;
        c = nofat ? c + 1 : fat_entry(v, c);
    }
    v->dirs++;
    for (i = 0;; i++) {
        const unsigned char *e = dir_entry(v, cl, ncl, i);
        unsigned type, secondaries, k, nlen, got = 0;
        uint16_t units[255];
        const unsigned char *stream = 0;
        uint16_t sum;

        if (!e || e[0] == 0x00)
            break;
        type = e[0];
        if (!(type & 0x80))
            continue; /* not in use */
        if (type == 0x81) {
            if (i > v->bitmap_index) {
                snprintf(text, sizeof(text), "a second bitmap entry in %s", path);
                finding(v, text);
            }
            continue;
        }
        if (type == 0x82) {
            continue;
        }
        if (type == 0x83 || type == 0xA0 || type == 0xA1) {
            continue; /* the label, the GUID, TexFAT padding: primaries with no set to walk */
        }
        if (type != 0x85) {
            snprintf(text, sizeof(text), "unknown primary entry 0x%02x in %s", type, path);
            finding(v, text);
            continue;
        }
        secondaries = e[1];
        if (secondaries < 2 || secondaries > 18) {
            snprintf(text, sizeof(text), "set with %u secondaries in %s", secondaries, path);
            finding(v, text);
            continue;
        }
        {
            const unsigned char *last = dir_entry(v, cl, ncl, i + secondaries);

            if (!last) {
                snprintf(text, sizeof(text), "set past the end of %s", path);
                finding(v, text);
                break;
            }
        }
        /* The set is read entry by entry, since the clusters need not be
           adjacent. */
        {
            unsigned char set[19 * 32];

            for (k = 0; k <= secondaries; k++)
                memcpy(set + k * 32, dir_entry(v, cl, ncl, i + k), 32);
            sum = set_checksum(set, secondaries + 1);
            if (sum != rd16(set + 2)) {
                snprintf(text, sizeof(text), "set checksum %04x, stored %04x, in %s", sum,
                         rd16(set + 2), path);
                finding(v, text);
                i += secondaries;
                continue;
            }
            if (set[32] != 0xC0) {
                snprintf(text, sizeof(text), "set without a stream extension second in %s", path);
                finding(v, text);
                i += secondaries;
                continue;
            }
            stream = set + 32;
            nlen = stream[3];
            for (k = 2; k <= secondaries; k++) {
                const unsigned char *ne = set + k * 32;
                unsigned m;

                if (ne[0] != 0xC1)
                    continue;
                for (m = 0; m < 15 && got < nlen && got < 255; m++)
                    units[got++] = rd16(ne + 2 + m * 2);
            }
            if (got != nlen || nlen == 0) {
                snprintf(text, sizeof(text), "name of %u units in entries for %u in %s", nlen, got,
                         path);
                finding(v, text);
                i += secondaries;
                continue;
            }
            /* The units past NameLength in the name entries are zero. */
            {
                unsigned seen = 0, bad = 0;

                for (k = 2; k <= secondaries && !bad; k++) {
                    const unsigned char *ne = set + k * 32;
                    unsigned m;

                    if (ne[0] != 0xC1)
                        continue;
                    for (m = 0; m < 15; m++, seen++)
                        if (seen >= nlen && rd16(ne + 2 + m * 2) != 0)
                            bad = 1;
                }
                if (bad) {
                    utf8_of(units, nlen, name, sizeof(name));
                    snprintf(text, sizeof(text),
                             "name entries with units past NameLength %u: %s/%s", nlen, path, name);
                    finding(v, text);
                }
            }
            if (name_hash(v, units, nlen) != rd16(stream + 4)) {
                utf8_of(units, nlen, name, sizeof(name));
                snprintf(text, sizeof(text), "name hash %04x, stored %04x: %s/%s",
                         name_hash(v, units, nlen), rd16(stream + 4), path, name);
                finding(v, text);
            }
            utf8_of(units, nlen, name, sizeof(name));
            snprintf(child, sizeof(child), "%s/%s", path, name);
            if (v->dump && !v->first_set_dumped && !(rd16(set + 4) & 0x10)) {
                char ct[64], mt[64], at[64];

                v->first_set_dumped = 1;
                printf("dump: first file set: %s\n", child);
                dump_set(set, secondaries + 1);
                stamp_text(rd32(set + 8), set[20], set[22], ct, sizeof(ct));
                stamp_text(rd32(set + 12), set[21], set[23], mt, sizeof(mt));
                stamp_text(rd32(set + 16), 0, set[24], at, sizeof(at));
                printf("dump: created %s; modified %s; accessed %s\n", ct, mt, at);
                printf("dump: stream flags %02x, name length %u, hash %04x, valid length %llu, "
                       "first %u, length %llu\n",
                       stream[1], nlen, rd16(stream + 4), (unsigned long long)rd64(stream + 8),
                       rd32(stream + 20), (unsigned long long)rd64(stream + 24));
            }
            /* A vendor allocation's clusters are the set's too. */
            for (k = 2; k <= secondaries; k++) {
                const unsigned char *ve = set + k * 32;

                if (ve[0] == 0xE1 && (ve[1] & 0x01) && rd32(ve + 20) != 0) {
                    utf8_of(units, nlen, name, sizeof(name));
                    snprintf(child, sizeof(child), "%s/%s (vendor allocation)", path, name);
                    walk_chain(v, rd32(ve + 20), rd64(ve + 24), (ve[1] & 0x02) != 0, child);
                }
            }
            {
                int is_dir = (rd16(set + 4) & 0x10) != 0;
                int nofat_child = (stream[1] & 0x02) != 0;
                int possible = (stream[1] & 0x01) != 0;
                uint32_t fc = rd32(stream + 20);
                uint64_t dl = rd64(stream + 24), vdl = rd64(stream + 8);

                if (v->list)
                    printf("check: entry %s %s %u %llu%s\n", child, is_dir ? "dir" : "file", fc,
                           (unsigned long long)dl, nofat_child ? " nofat" : "");
                if (vdl > dl) {
                    snprintf(text, sizeof(text), "valid length past the length: %s", child);
                    finding(v, text);
                }
                if (!possible) {
                    snprintf(text, sizeof(text), "stream without AllocationPossible: %s", child);
                    finding(v, text);
                }
                if (is_dir) {
                    if (fc == 0) {
                        snprintf(text, sizeof(text), "directory with no cluster: %s", child);
                        finding(v, text);
                    } else {
                        check_dir(v, fc, dl, nofat_child, child, depth + 1);
                    }
                } else {
                    v->files++;
                    if (fc == 0) {
                        if (dl != 0) {
                            snprintf(text, sizeof(text), "short chain: %s", child);
                            finding(v, text);
                        }
                    } else {
                        walk_chain(v, fc, dl, nofat_child, child);
                    }
                }
            }
            i += secondaries;
        }
    }
    free(cl);
}

/* ---- the boot regions and the root's objects ---- */

static int check_boot(struct vol *v)
{
    const unsigned char *b = v->img;
    char text[200];
    uint64_t stored, computed;
    unsigned i;

    if (memcmp(b + 3, "EXFAT   ", 8) != 0) {
        finding(v, "boot: not an exFAT boot sector");
        return -1;
    }
    for (i = 11; i < 64; i++) {
        if (b[i] != 0) {
            finding(v, "boot: MustBeZero is not zero");
            return -1;
        }
    }
    v->bps_shift = b[108];
    v->spc_shift = b[109];
    v->nfats = b[110];
    if (v->bps_shift < 9 || v->bps_shift > 12 || v->bps_shift + v->spc_shift > 25 ||
        (v->nfats != 1 && v->nfats != 2)) {
        finding(v, "boot: a shift or a FAT count out of range");
        return -1;
    }
    if (rd16(b + 510) != 0xAA55) {
        finding(v, "boot: signature");
        return -1;
    }
    v->length = rd64(b + 72);
    v->fat_offset = rd32(b + 80);
    v->fat_length = rd32(b + 84);
    v->heap_offset = rd32(b + 88);
    v->clusters = rd32(b + 92);
    v->root = rd32(b + 96);
    v->flags = rd16(b + 106);
    v->active_fat = v->flags & 1;
    if (v->length * bytes_per_sector(v) > v->bytes) {
        finding(v, "boot: volume length past the image");
        return -1;
    }
    if (v->fat_offset < 24 ||
        (uint64_t)v->fat_offset + (uint64_t)v->fat_length * v->nfats > v->heap_offset) {
        finding(v, "boot: the FAT region does not fit between the boot regions and the heap");
        return -1;
    }
    if ((uint64_t)v->fat_length * bytes_per_sector(v) < ((uint64_t)v->clusters + 2) * 4) {
        finding(v, "boot: a FAT shorter than the clusters");
        return -1;
    }
    if (v->heap_offset + ((uint64_t)v->clusters << v->spc_shift) > v->length) {
        finding(v, "boot: the heap past the volume");
        return -1;
    }
    if (!in_heap(v, v->root)) {
        finding(v, "boot: the root outside the heap");
        return -1;
    }
    if (v->active_fat >= v->nfats) {
        finding(v, "boot: the active FAT is not there");
        return -1;
    }
    if (v->flags & 4) {
        finding(v, "boot: media failure flagged");
    }
    computed = boot_checksum(v, 0);
    stored = rd32(sector(v, 11));
    if (computed != stored) {
        snprintf(text, sizeof(text), "boot: main checksum %08llx, stored %08llx",
                 (unsigned long long)computed, (unsigned long long)stored);
        finding(v, text);
    }
    computed = boot_checksum(v, 12);
    stored = rd32(sector(v, 23));
    if (computed != stored) {
        snprintf(text, sizeof(text), "boot: backup checksum %08llx, stored %08llx",
                 (unsigned long long)computed, (unsigned long long)stored);
        finding(v, text);
    }
    for (i = 0; i < bytes_per_sector(v); i++) {
        if (i == 106 || i == 107 || i == 112)
            continue;
        if (b[i] != sector(v, 12)[i]) {
            finding(v, "boot: the backup boot sector differs from the main");
            break;
        }
    }
    if (v->dump) {
        printf(
            "dump: boot: FileSystemName %.8s; VolumeLength %llu; FatOffset %u; FatLength %u; "
            "ClusterHeapOffset %u; ClusterCount %u; FirstClusterOfRootDirectory %u; VolumeFlags "
            "%04x; "
            "BytesPerSectorShift %u; SectorsPerClusterShift %u; NumberOfFats %u; PercentInUse %u; "
            "main checksum %08llx; backup checksum %08llx\n",
            (const char *)b + 3, (unsigned long long)v->length, v->fat_offset, v->fat_length,
            v->heap_offset, v->clusters, v->root, v->flags, v->bps_shift, v->spc_shift, v->nfats,
            b[112], (unsigned long long)rd32(sector(v, 11)),
            (unsigned long long)rd32(sector(v, 23)));
        printf("dump: FAT[0] %08x FAT[1] %08x\n", fat_entry(v, 0), fat_entry(v, 1));
    }
    return 0;
}

/* The bitmap and the up-case table, found in the root before anything is
   walked; the root's own clusters are read through the FAT. */
static int load_objects(struct vol *v)
{
    char text[200];
    uint32_t c = v->root;
    unsigned steps = 0;
    uint64_t i, need;
    unsigned char *stored;

    v->bitmap = calloc(v->clusters, 1);
    v->upcase = malloc(65536 * sizeof(uint16_t));
    v->owner = calloc((size_t)v->clusters + 2, sizeof(unsigned short));
    if (!v->bitmap || !v->upcase || !v->owner) {
        finding(v, "out of memory");
        return -1;
    }
    while (in_heap(v, c) && steps++ < v->clusters) {
        unsigned k, per = cluster_bytes(v) / 32;
        const unsigned char *cl = cluster(v, c);
        uint32_t next;

        for (k = 0; k < per; k++) {
            const unsigned char *e = cl + k * 32;

            if (e[0] == 0x00)
                break;
            if (e[0] == 0x81 && !(e[1] & 1) && v->bitmap_first == 0) {
                v->bitmap_first = rd32(e + 20);
                v->bitmap_length = rd64(e + 24);
                v->bitmap_index = k;
                if (v->dump)
                    printf("dump: bitmap entry: type %02x flags %02x FirstCluster %u DataLength "
                           "%llu\n",
                           e[0], e[1], v->bitmap_first, (unsigned long long)v->bitmap_length);
            } else if (e[0] == 0x82) {
                v->upcase_first = rd32(e + 20);
                v->upcase_length = rd64(e + 24);
                if (v->dump)
                    printf("dump: up-case entry: type %02x TableChecksum %08x FirstCluster %u "
                           "DataLength %llu\n",
                           e[0], rd32(e + 4), v->upcase_first,
                           (unsigned long long)v->upcase_length);
                stored = cluster(v, v->upcase_first);
                if (!in_heap(v, v->upcase_first) ||
                    (v->upcase_first - 2 +
                     (v->upcase_length + cluster_bytes(v) - 1) / cluster_bytes(v)) > v->clusters) {
                    finding(v, "up-case table outside the heap");
                    return -1;
                }
                if (table_checksum(stored, v->upcase_length) != rd32(e + 4)) {
                    snprintf(text, sizeof(text), "up-case checksum %08x, stored %08x",
                             table_checksum(stored, v->upcase_length), rd32(e + 4));
                    finding(v, text);
                }
                {
                    uint32_t u = decompress_upcase(stored, v->upcase_length, v->upcase);

                    if (u != 65536) {
                        snprintf(text, sizeof(text), "up-case table expands to %u mappings", u);
                        finding(v, text);
                        while (u < 65536) {
                            v->upcase[u] = (uint16_t)u;
                            u++;
                        }
                    }
                    if (v->dump)
                        printf("dump: up-case: %llu stored bytes, 'a' maps to %04x, U+00E9 to "
                               "%04x, U+00FF to %04x\n",
                               (unsigned long long)v->upcase_length, v->upcase['a'],
                               v->upcase[0xE9], v->upcase[0xFF]);
                }
            }
        }
        next = fat_entry(v, c);
        if (next == 0xFFFFFFFFu)
            break;
        c = next;
    }
    if (v->bitmap_first == 0 || v->upcase_first == 0) {
        finding(v, "the root has no bitmap or no up-case table");
        return -1;
    }
    need = ((uint64_t)v->clusters + 7) / 8;
    if (!in_heap(v, v->bitmap_first) || v->bitmap_length < need) {
        finding(v, "bitmap entry outside the heap or too short");
        return -1;
    }
    stored = cluster(v, v->bitmap_first);
    for (i = 0; i < v->clusters; i++)
        v->bitmap[i] = (stored[i / 8] >> (i % 8)) & 1;
    return 0;
}

static int check_main(int argc, char **argv)
{
    struct vol v;
    FILE *f;
    uint64_t offset = 0;
    const char *image = 0;
    int i;
    long n;
    uint64_t c, allocated = 0, reached = 0;
    char text[200];

    memset(&v, 0, sizeof(v));
    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--check") == 0)
            continue;
        else if (strcmp(argv[i], "--list") == 0)
            v.list = 1;
        else if (strcmp(argv[i], "--dump") == 0)
            v.dump = 1;
        else if (!image)
            image = argv[i];
        else
            offset = strtoull(argv[i], 0, 10);
    }
    if (!image) {
        fprintf(stderr, "usage: mkexfatfixture --check IMAGE [OFFSET-SECTORS] [--list] [--dump]\n");
        return 2;
    }
    f = fopen(image, "rb");
    if (!f) {
        fprintf(stderr, "mkexfatfixture: cannot open %s\n", image);
        return 2;
    }
    fseek(f, 0, SEEK_END);
    v.bytes = (size_t)ftell(f) - (size_t)(offset * SECTOR);
    v.img = malloc(v.bytes);
    if (!v.img || fseek(f, (long)(offset * SECTOR), SEEK_SET) != 0 ||
        fread(v.img, 1, v.bytes, f) != v.bytes) {
        fprintf(stderr, "mkexfatfixture: cannot read %s\n", image);
        return 2;
    }
    fclose(f);
    if (check_boot(&v) != 0 || load_objects(&v) != 0) {
        printf("check: FAIL: %d finding(s)\n", v.findings);
        return 1;
    }
    /* The system objects own their clusters first. */
    n = walk_chain(&v, v.bitmap_first, v.bitmap_length, 0, "(bitmap)");
    if (n >= 0)
        walk_chain(&v, v.upcase_first, v.upcase_length, 0, "(up-case table)");
    check_dir(&v, v.root, 0, 0, "", 0);
    for (c = 0; c < v.clusters; c++) {
        if (v.bitmap[c])
            allocated++;
        if (v.owner[c + 2])
            reached++;
    }
    if (allocated != reached) {
        snprintf(text, sizeof(text), "lost: %llu cluster(s) allocated and reached by nothing",
                 (unsigned long long)(allocated - reached));
        finding(&v, text);
    }
    if (v.dump)
        printf("dump: dirty %u, %llu of %u clusters allocated\n", (v.flags >> 1) & 1,
               (unsigned long long)allocated, v.clusters);
    if (v.findings) {
        printf("check: FAIL: %d finding(s)\n", v.findings);
        return 1;
    }
    printf("check: OK: %d file(s), %d director%s, %llu of %u clusters allocated%s\n", v.files,
           v.dirs, v.dirs == 1 ? "y" : "ies", (unsigned long long)allocated, v.clusters,
           (v.flags & 2) ? ", dirty" : "");
    return 0;
}

/* ---- the builder ----
 *
 *   mkexfatfixture IMAGE [OPTION]
 *
 * A 16 MiB volume with 4 KiB clusters, one FAT, both boot regions and their
 * checksums, the bitmap at cluster 2, the recommended up-case table
 * compressed at 3, the root at 5 chained through the FAT, a label, a GUID
 * entry, and a tree with every shape the reader must take: a contiguous
 * file with NoFatChain, a file chained through the FAT over clusters that
 * are not adjacent, a file whose ValidDataLength is short of its
 * DataLength with junk past it, an empty file with no clusters, a name of
 * four entries, a set with a vendor extension and a vendor allocation, a
 * set whose file and stream entries straddle a sector, a set that crosses a
 * cluster boundary, and a directory of two levels. An OPTION breaks one
 * rule, or changes the geometry, as the list in `variants` says.
 */

#include "exfat_upcase.inc"

#define B_BPS_SHIFT  9
#define B_SPC_SHIFT  3
#define B_SECTORS    32768u
#define B_FAT_OFFSET 24u
#define B_FIRST_FILE 6u

struct build {
    unsigned char *img;
    uint64_t sectors;
    unsigned bps_shift, spc_shift;
    uint32_t fat_offset, fat_length, heap_offset, clusters, root;
    uint32_t next; /* the bump allocator's next free cluster */
    uint16_t upcase[65536];
    const char *opt;
    /* Where the shapes went, for the variants that damage them. */
    uint32_t hello_set, chained_set, valid_set, chained_first;
    uint32_t bitmap_entry, upcase_entry;
};

/* The builder's own sums, apart from the reader's, so that a mutation in
   either is seen by the other. */
static uint16_t b_set_checksum(const unsigned char *entries, unsigned count)
{
    uint16_t sum = 0;
    unsigned i;

    for (i = 0; i < count * 32; i++) {
        if (i == 2 || i == 3)
            continue;
        sum = (uint16_t)(((sum & 1) ? 0x8000 : 0) + (sum >> 1) + entries[i]);
    }
    return sum;
}

static uint32_t b_table_checksum(const unsigned char *p, uint64_t n)
{
    uint32_t sum = 0;
    uint64_t i;

    for (i = 0; i < n; i++)
        sum = ((sum & 1) ? 0x80000000u : 0) + (sum >> 1) + p[i];
    return sum;
}

static void wr16(unsigned char *p, uint32_t v)
{
    p[0] = (unsigned char)v;
    p[1] = (unsigned char)(v >> 8);
}

static void wr32(unsigned char *p, uint32_t v)
{
    wr16(p, v & 0xFFFF);
    wr16(p + 2, v >> 16);
}

static void wr64(unsigned char *p, uint64_t v)
{
    wr32(p, (uint32_t)v);
    wr32(p + 4, (uint32_t)(v >> 32));
}

static unsigned b_bps(const struct build *b)
{
    return 1u << b->bps_shift;
}

static unsigned b_cbytes(const struct build *b)
{
    return 1u << (b->bps_shift + b->spc_shift);
}

static unsigned char *b_sector(const struct build *b, uint64_t n)
{
    return b->img + n * b_bps(b);
}

static unsigned char *b_cluster(const struct build *b, uint32_t c)
{
    return b_sector(b, b->heap_offset + ((uint64_t)(c - 2) << b->spc_shift));
}

static void b_fat(struct build *b, uint32_t c, uint32_t v)
{
    wr32(b_sector(b, b->fat_offset) + (size_t)c * 4, v);
}

static void b_bit(struct build *b, uint32_t c, int on)
{
    unsigned char *bm = b_cluster(b, 2);

    if (on)
        bm[(c - 2) / 8] |= (unsigned char)(1u << ((c - 2) % 8));
    else
        bm[(c - 2) / 8] &= (unsigned char)~(1u << ((c - 2) % 8));
}

/* Take `n` consecutive clusters, marked in the bitmap; chained through the
   FAT when `chain` is set (a NoFatChain stream leaves its entries alone). */
static uint32_t b_alloc(struct build *b, uint32_t n, int chain)
{
    uint32_t first = b->next, i;

    for (i = 0; i < n; i++) {
        b_bit(b, first + i, 1);
        if (chain)
            b_fat(b, first + i, i + 1 < n ? first + i + 1 : 0xFFFFFFFFu);
    }
    b->next += n;
    return first;
}

static int b_is(const struct build *b, const char *opt)
{
    return b->opt != 0 && strcmp(b->opt, opt) == 0;
}

/* UTF-8 to UTF-16, for the builder's own names (which are well-formed). */
static unsigned b_units(const char *name, uint16_t *units)
{
    unsigned n = 0, i = 0;

    while (name[i]) {
        uint32_t c = (unsigned char)name[i], cp, extra, k;

        if (c < 0x80) {
            cp = c;
            extra = 0;
        } else if ((c & 0xE0) == 0xC0) {
            cp = c & 0x1F;
            extra = 1;
        } else if ((c & 0xF0) == 0xE0) {
            cp = c & 0x0F;
            extra = 2;
        } else {
            cp = c & 0x07;
            extra = 3;
        }
        for (k = 0; k < extra; k++)
            cp = (cp << 6) | ((unsigned char)name[i + 1 + k] & 0x3F);
        i += 1 + extra;
        if (cp >= 0x10000) {
            units[n++] = (uint16_t)(0xD800 + ((cp - 0x10000) >> 10));
            units[n++] = (uint16_t)(0xDC00 + ((cp - 0x10000) & 0x3FF));
        } else {
            units[n++] = (uint16_t)cp;
        }
    }
    return n;
}

/* A directory being written: its clusters, chained through the FAT unless
   `nofat`, and the next entry index. */
struct dw {
    struct build *b;
    uint32_t clusters[64];
    unsigned n, used;
    int nofat;
};

static void dw_init(struct dw *d, struct build *b, int nofat, uint32_t preallocated)
{
    memset(d, 0, sizeof(*d));
    d->b = b;
    d->nofat = nofat;
    d->clusters[0] = preallocated;
    d->n = 1;
}

static unsigned char *dw_slot(struct dw *d, unsigned index)
{
    unsigned per = b_cbytes(d->b) / 32;

    while (index / per >= d->n) {
        uint32_t c = b_alloc(d->b, 1, !d->nofat);

        if (!d->nofat)
            d->b->img[0] = d->b->img[0]; /* the FAT link below */
        b_fat(d->b, d->clusters[d->n - 1], d->nofat ? 0 : c);
        b_fat(d->b, c, 0xFFFFFFFFu);
        d->clusters[d->n++] = c;
    }
    return b_cluster(d->b, d->clusters[index / per]) + (index % per) * 32;
}

static unsigned dw_put(struct dw *d, const unsigned char *entry)
{
    unsigned at = d->used++;

    memcpy(dw_slot(d, at), entry, 32);
    return at;
}

static uint32_t b_stamp(void)
{
    /* 2026-09-01 12:00:00 */
    return ((2026u - 1980u) << 25) | (9u << 21) | (1u << 16) | (12u << 11);
}

/* A file set in a buffer: the file entry, the stream extension, the name
   entries, and `vendor` benign secondaries after them; returns its entry
   count with the checksum written. */
static unsigned set_build(struct build *b, unsigned char *set, const char *name, int is_dir,
                          uint32_t first, uint64_t length, uint64_t valid, int nofat, int vendor,
                          uint32_t vendor_cluster)
{
    uint16_t units[255], upped[255];
    unsigned n = b_units(name, units), entries = 2 + (n + 14) / 15, k, i;
    unsigned char *e;
    uint16_t hash = 0;

    memset(set, 0, 19 * 32);
    for (k = 0; k < n; k++)
        upped[k] = b->upcase[units[k]];
    for (k = 0; k < n; k++) {
        hash = (uint16_t)(((hash & 1) ? 0x8000 : 0) + (hash >> 1) + (upped[k] & 0xFF));
        hash = (uint16_t)(((hash & 1) ? 0x8000 : 0) + (hash >> 1) + (upped[k] >> 8));
    }
    e = set;
    e[0] = 0x85;
    wr16(e + 4, is_dir ? 0x10 : 0x20);
    wr32(e + 8, b_stamp());
    wr32(e + 12, b_stamp());
    wr32(e + 16, b_stamp());
    e[22] = e[23] = e[24] = (unsigned char)(b_is(b, "--no-utc") ? 0x00 : 0x80);
    e = set + 32;
    e[0] = 0xC0;
    e[1] = (unsigned char)(0x01 | (nofat ? 0x02 : 0x00)); /* AllocationPossible shall be 1 */
    e[3] = (unsigned char)n;
    wr16(e + 4, hash);
    wr64(e + 8, valid);
    wr32(e + 20, first);
    wr64(e + 24, length);
    for (i = 0; i < (n + 14) / 15; i++) {
        e = set + (2 + i) * 32;
        e[0] = 0xC1;
        for (k = 0; k < 15 && i * 15 + k < n; k++)
            wr16(e + 2 + k * 2, units[i * 15 + k]);
    }
    if (vendor) {
        e = set + entries * 32;
        e[0] = 0xE0; /* a vendor extension: a GUID and 14 bytes of its own */
        for (k = 0; k < 16; k++)
            e[2 + k] = (unsigned char)(0xA0 + k);
        memcpy(e + 18, "vendor bytes  ", 14);
        entries++;
        e = set + entries * 32;
        e[0] = 0xE1; /* a vendor allocation: a GUID and one contiguous cluster */
        e[1] = 0x03;
        for (k = 0; k < 16; k++)
            e[2 + k] = (unsigned char)(0xB0 + k);
        wr32(e + 20, vendor_cluster);
        wr64(e + 24, b_cbytes(b));
        entries++;
    }
    set[1] = (unsigned char)(entries - 1);
    wr16(set + 2, b_set_checksum(set, entries));
    return entries;
}

static unsigned dw_put_set(struct dw *d, const unsigned char *set, unsigned entries)
{
    unsigned first = d->used, i;

    for (i = 0; i < entries; i++)
        dw_put(d, set + i * 32);
    return first;
}

static void fill(struct build *b, uint32_t first, uint64_t bytes, unsigned char seed)
{
    unsigned char *p = b_cluster(b, first);
    uint64_t i;

    for (i = 0; i < bytes; i++)
        p[i] = (unsigned char)(seed + i);
}

/* The stored up-case table for the volume: the recommended one compressed,
   or expanded in part or in whole for the variants that ask. */
static uint64_t upcase_bytes(struct build *b, unsigned char *out, uint64_t cap)
{
    if (b_is(b, "--big-upcase") || b_is(b, "--uncompressed-upcase")) {
        uint32_t explicit = b_is(b, "--big-upcase") ? 12288u : 65536u, u = 0;
        uint64_t at = 0;

        for (u = 0; u < explicit; u++) {
            wr16(out + at, b->upcase[u]);
            at += 2;
        }
        while (u < 65536) {
            /* From here on, identity runs compressed, the rest explicit. */
            uint32_t run = 0;

            while (u + run < 65536 && b->upcase[u + run] == u + run)
                run++;
            if (run >= 2) {
                while (run > 0) {
                    uint32_t take = run > 65535 ? 65535 : run;

                    wr16(out + at, 0xFFFF);
                    wr16(out + at + 2, take);
                    at += 4;
                    u += take;
                    run -= take;
                }
            } else {
                wr16(out + at, b->upcase[u]);
                at += 2;
                u++;
            }
            if (at + 8 > cap)
                break;
        }
        return at;
    }
    memcpy(out, upcase_stored, sizeof(upcase_stored));
    return sizeof(upcase_stored);
}

static void boot_write(struct build *b, uint64_t first)
{
    unsigned char *s0 = b_sector(b, first);
    unsigned i;
    uint32_t sum;

    memset(s0, 0, b_bps(b) * 12);
    s0[0] = 0xEB;
    s0[1] = 0x76;
    s0[2] = 0x90;
    memcpy(s0 + 3, "EXFAT   ", 8);
    wr64(s0 + 64, 0);
    wr64(s0 + 72, b->sectors);
    wr32(s0 + 80, b_is(b, "--fat-in-boot") ? 12u : b->fat_offset);
    wr32(s0 + 84, b_is(b, "--short-fat") ? b->fat_length - 1 : b->fat_length);
    wr32(s0 + 88, b->heap_offset);
    wr32(s0 + 92, b->clusters - (first != 0 && b_is(b, "--regions-disagree") ? 1u : 0u));
    wr32(s0 + 96, b->root);
    wr32(s0 + 100, 0x12345678u);
    wr16(s0 + 104, 0x0100);
    wr16(s0 + 106, (b_is(b, "--dirty") ? 2u : 0u) | (b_is(b, "--media-failure") ? 4u : 0u));
    s0[108] = (unsigned char)b->bps_shift;
    s0[109] = (unsigned char)b->spc_shift;
    s0[110] = 1;
    s0[111] = 0x80;
    s0[112] = 1;
    wr16(s0 + 510, 0xAA55);
    for (i = 1; i <= 8; i++)
        wr32(b_sector(b, first + i) + b_bps(b) - 4, 0xAA550000u);
    sum = 0;
    for (i = 0; i < b_bps(b) * 11; i++) {
        if (i == 106 || i == 107 || i == 112)
            continue;
        sum = ((sum & 1) ? 0x80000000u : 0) + (sum >> 1) + s0[i];
    }
    if ((first == 0 && b_is(b, "--bad-main-checksum")) ||
        (first != 0 && b_is(b, "--bad-backup-checksum")))
        sum++;
    for (i = 0; i < b_bps(b); i += 4)
        wr32(b_sector(b, first + 11) + i, sum);
}

static int build(const char *out, const char *opt)
{
    struct build b;
    struct dw root, dir, sub;
    unsigned char set[19 * 32], entry[32], *table;
    uint64_t table_len, bitmap_len;
    uint32_t c, vendor_cluster, big_first;
    unsigned n, i;
    FILE *f;

    memset(&b, 0, sizeof(b));
    b.opt = opt;
    b.bps_shift = b_is(&b, "--sector-4k") ? 12 : B_BPS_SHIFT;
    b.spc_shift = b_is(&b, "--sector-4k") ? 0 : B_SPC_SHIFT;
    b.sectors = B_SECTORS >> (b.bps_shift - B_BPS_SHIFT);
    b.fat_offset = B_FAT_OFFSET;
    b.img = calloc((size_t)b.sectors, b_bps(&b));
    if (!b.img)
        return 2;
    /* The heap starts on a cluster boundary after a FAT long enough for it. */
    b.heap_offset = 256u >> (b.bps_shift - B_BPS_SHIFT);
    b.clusters = (uint32_t)((b.sectors - b.heap_offset) >> b.spc_shift);
    b.fat_length = (uint32_t)(((uint64_t)(b.clusters + 2) * 4 + b_bps(&b) - 1) / b_bps(&b));
    decompress_upcase(upcase_stored, sizeof(upcase_stored), b.upcase);
    b_fat(&b, 0, 0xFFFFFFF8u);
    b_fat(&b, 1, 0xFFFFFFFFu);
    b.next = 2;
    /* Cluster 2: the bitmap; 3 and on: the up-case table; then the root. */
    bitmap_len = ((uint64_t)b.clusters + 7) / 8;
    c = b_alloc(&b, (uint32_t)((bitmap_len + b_cbytes(&b) - 1) / b_cbytes(&b)), 1);
    table = malloc(140000);
    table_len = upcase_bytes(&b, table, 140000);
    if (b_is(&b, "--truncated-upcase-run"))
        table_len -= 2; /* the last run's count is gone */
    c = b_alloc(&b, (uint32_t)((table_len + b_cbytes(&b) - 1) / b_cbytes(&b)), 1);
    memcpy(b_cluster(&b, c), table, (size_t)table_len);
    b.root = b_alloc(&b, 1, 1);
    dw_init(&root, &b, 0, b.root);
    /* The label. */
    memset(entry, 0, 32);
    entry[0] = 0x83;
    entry[1] = 7;
    for (i = 0; i < 7; i++)
        wr16(entry + 2 + i * 2, (uint32_t)"FIXTURE"[i]);
    dw_put(&root, entry);
    /* The bitmap entry. */
    memset(entry, 0, 32);
    entry[0] = 0x81;
    wr32(entry + 20, 2);
    wr64(entry + 24, b_is(&b, "--short-bitmap") ? bitmap_len - 1 : bitmap_len);
    b.bitmap_entry = dw_put(&root, entry);
    /* The up-case entry. */
    memset(entry, 0, 32);
    entry[0] = 0x82;
    wr32(entry + 4,
         b_table_checksum(table, table_len) + (b_is(&b, "--bad-upcase-checksum") ? 1u : 0u));
    wr32(entry + 20, c);
    wr64(entry + 24, table_len);
    b.upcase_entry = dw_put(&root, entry);
    if (b_is(&b, "--second-bitmap")) {
        memset(entry, 0, 32);
        entry[0] = 0x81;
        entry[1] = 1;
        wr32(entry + 20, 2);
        wr64(entry + 24, bitmap_len);
        dw_put(&root, entry);
    }
    if (b_is(&b, "--unknown-critical")) {
        memset(entry, 0, 32);
        entry[0] = 0x86;
        dw_put(&root, entry);
    }
    /* The GUID entry. */
    memset(entry, 0, 32);
    entry[0] = 0xA0;
    for (i = 0; i < 16; i++)
        entry[6 + i] = (unsigned char)(0x10 + i);
    dw_put(&root, entry);
    free(table);

    /* hello.txt: twelve bytes, contiguous, NoFatChain. */
    c = b_alloc(&b, 1, 0);
    memcpy(b_cluster(&b, c), "hello exfat\n", 12);
    n = set_build(&b, set, "hello.txt", 0, c, 12, 12, 1, 0, 0);
    b.hello_set = dw_put_set(&root, set, n);
    /* chained.bin: three clusters through the FAT, none adjacent. */
    b.chained_first = b_alloc(&b, 1, 1);
    b.next++; /* a gap */
    c = b_alloc(&b, 1, 1);
    b_fat(&b, b.chained_first, c);
    b.next++;
    n = b_alloc(&b, 1, 1);
    b_fat(&b, c, n);
    fill(&b, b.chained_first, b_cbytes(&b), 0x10);
    fill(&b, c, b_cbytes(&b), 0x20);
    fill(&b, n, b_cbytes(&b), 0x30);
    n = set_build(&b, set, "chained.bin", 0, b.chained_first, (uint64_t)b_cbytes(&b) * 3,
                  (uint64_t)b_cbytes(&b) * 3, 0, 0, 0);
    b.chained_set = dw_put_set(&root, set, n);
    /* valid.bin: two clusters, 3000 bytes valid, junk past them. */
    c = b_alloc(&b, 2, 0);
    fill(&b, c, (uint64_t)b_cbytes(&b) * 2, 0x40);
    memset(b_cluster(&b, c) + 3000, 0xEE, (size_t)b_cbytes(&b) * 2 - 3000);
    n = set_build(&b, set, "valid.bin", 0, c, (uint64_t)b_cbytes(&b) * 2, 3000, 1, 0, 0);
    b.valid_set = dw_put_set(&root, set, n);
    /* A name of four entries, crossing the first sector's end. */
    c = b_alloc(&b, 1, 0);
    memcpy(b_cluster(&b, c), "x", 1);
    n = set_build(&b, set, "A long name of some fifty characters for four entries.txt", 0, c, 1, 1,
                  1, 0, 0);
    dw_put_set(&root, set, n);
    /* vendor.txt: a set with a vendor extension and a vendor allocation. */
    c = b_alloc(&b, 1, 0);
    memcpy(b_cluster(&b, c), "vendor\n", 7);
    vendor_cluster = b_alloc(&b, 1, 0);
    n = set_build(&b, set, "vendor.txt", 0, c, 7, 7, 1, 1, vendor_cluster);
    dw_put_set(&root, set, n);
    /* empty: no clusters. */
    n = set_build(&b, set, "empty", 0, 0, 0, 0, 0, 0, 0);
    dw_put_set(&root, set, n);
    /* Pads until a set's file entry lands on a sector's last slot, so its
       stream entry is the next sector's first. */
    for (i = 0; root.used % 16 != 15; i++) {
        char name[16];

        snprintf(name, sizeof(name), "pad%02u.txt", i);
        n = set_build(&b, set, name, 0, 0, 0, 0, 0, 0, 0);
        dw_put_set(&root, set, n);
    }
    c = b_alloc(&b, 1, 0);
    memcpy(b_cluster(&b, c), "straddle\n", 9);
    n = set_build(&b, set, "straddle.txt", 0, c, 9, 9, 1, 0, 0);
    dw_put_set(&root, set, n);
    /* dir, contiguous with NoFatChain; dir/sub chained; dir/sub/leaf.txt. */
    c = b_alloc(&b, 1, 0);
    dw_init(&dir, &b, 1, c);
    n = set_build(&b, set, "dir", 1, c, b_cbytes(&b), b_cbytes(&b), 1, 0, 0);
    dw_put_set(&root, set, n);
    c = b_alloc(&b, 1, 1);
    dw_init(&sub, &b, 0, c);
    n = set_build(&b, set, "sub", 1, c, b_cbytes(&b), b_cbytes(&b), 0, 0, 0);
    dw_put_set(&dir, set, n);
    c = b_alloc(&b, 1, 0);
    memcpy(b_cluster(&b, c), "leaf\n", 5);
    n = set_build(&b, set, "leaf.txt", 0, c, 5, 5, 1, 0, 0);
    dw_put_set(&sub, set, n);
    /* Pads until a set crosses the root's first cluster boundary. */
    for (i = 100; root.used % (b_cbytes(&b) / 32) != b_cbytes(&b) / 32 - 2; i++) {
        char name[16];

        snprintf(name, sizeof(name), "pad%02u.txt", i);
        n = set_build(&b, set, name, 0, 0, 0, 0, 0, 0, 0);
        dw_put_set(&root, set, n);
    }
    c = b_alloc(&b, 1, 0);
    memcpy(b_cluster(&b, c), "crossing\n", 9);
    n = set_build(&b, set, "crossing.txt", 0, c, 9, 9, 1, 0, 0);
    dw_put_set(&root, set, n);
    big_first = c;

    /* The variants that damage a set or a chain, each with the checksum
       recomputed where the rule broken is not the checksum. */
    {
        unsigned char *hello = dw_slot(&root, b.hello_set);
        unsigned char *chained = dw_slot(&root, b.chained_set);
        unsigned char *valid = dw_slot(&root, b.valid_set);
        int resum_hello = 0, resum_chained = 0, resum_valid = 0;

        (void)big_first;
        if (b_is(&b, "--bad-set-checksum"))
            wr16(hello + 2, rd16(hello + 2) + 1);
        if (b_is(&b, "--wrong-secondary-count")) {
            hello[1] = 1;
            resum_hello = 1;
        }
        if (b_is(&b, "--wrong-name-length")) {
            hello[32 + 3] = 8;
            resum_hello = 1;
        }
        if (b_is(&b, "--false-hash")) {
            wr16(hello + 32 + 4, rd16(hello + 32 + 4) + 1);
            resum_hello = 1;
        }
        if (b_is(&b, "--vdl-past-dl")) {
            wr64(valid + 32 + 8, rd64(valid + 32 + 24) + 1);
            resum_valid = 1;
        }
        if (b_is(&b, "--alloc-impossible")) {
            hello[32 + 1] &= (unsigned char)~0x01;
            resum_hello = 1;
        }
        if (b_is(&b, "--chain-out"))
            b_fat(&b, b.chained_first, b.clusters + 5);
        if (b_is(&b, "--loop"))
            b_fat(&b, b.chained_first + 4, b.chained_first);
        if (b_is(&b, "--short-stream")) {
            wr64(chained + 32 + 24, (uint64_t)b_cbytes(&b) * 4);
            wr64(chained + 32 + 8, (uint64_t)b_cbytes(&b) * 4);
            resum_chained = 1;
        }
        if (b_is(&b, "--reached-free"))
            b_bit(&b, rd32(hello + 32 + 20), 0);
        if (b_is(&b, "--lost-bit"))
            b_bit(&b, b.next + 3, 1);
        if (b_is(&b, "--surplus")) {
            wr64(chained + 32 + 24, b_cbytes(&b));
            wr64(chained + 32 + 8, b_cbytes(&b));
            resum_chained = 1;
        }
        if (b_is(&b, "--overlap")) {
            wr32(hello + 32 + 20, 2); /* the bitmap's cluster */
            resum_hello = 1;
        }
        if (resum_hello)
            wr16(hello + 2, b_set_checksum(hello, hello[1] + 1));
        if (resum_chained)
            wr16(chained + 2, b_set_checksum(chained, chained[1] + 1));
        if (resum_valid)
            wr16(valid + 2, b_set_checksum(valid, valid[1] + 1));
    }
    boot_write(&b, 0);
    boot_write(&b, 12);
    f = fopen(out, "wb");
    if (!f || fwrite(b.img, 1, (size_t)b.sectors * b_bps(&b), f) != (size_t)b.sectors * b_bps(&b)) {
        fprintf(stderr, "mkexfatfixture: cannot write %s\n", out);
        return 2;
    }
    fclose(f);
    free(b.img);
    return 0;
}

int main(int argc, char **argv)
{
    int i;

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--check") == 0)
            return check_main(argc, argv);
    }
    if (argc < 2 || argc > 3) {
        fprintf(stderr, "usage: mkexfatfixture IMAGE [OPTION] | --check IMAGE [OFFSET-SECTORS] "
                        "[--list] [--dump]\n");
        return 2;
    }
    return build(argv[1], argc == 3 ? argv[2] : 0);
}
