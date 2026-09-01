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
   or, with NoFatChain, as `length` bytes of consecutive clusters. */
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
            v->bitmap_first = rd32(e + 20);
            v->bitmap_length = rd64(e + 24);
            continue;
        }
        if (type == 0x82) {
            v->upcase_first = rd32(e + 20);
            v->upcase_length = rd64(e + 24);
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
                if (!possible && (fc != 0 || dl != 0)) {
                    snprintf(text, sizeof(text), "allocation impossible with clusters: %s", child);
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
            if (e[0] == 0x81 && !(e[1] & 1)) {
                v->bitmap_first = rd32(e + 20);
                v->bitmap_length = rd64(e + 24);
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
                /* Decompress: a 0xFFFF followed by a count is that many
                   identity mappings; anything else maps the next unit. */
                {
                    uint64_t at = 0;
                    uint32_t u = 0;

                    while (at + 1 < v->upcase_length && u < 65536) {
                        uint16_t w = rd16(stored + at);

                        at += 2;
                        if (w == 0xFFFF && at + 1 < v->upcase_length) {
                            uint16_t run = rd16(stored + at);

                            at += 2;
                            while (run-- > 0 && u < 65536) {
                                v->upcase[u] = (uint16_t)u;
                                u++;
                            }
                        } else {
                            v->upcase[u++] = w;
                        }
                    }
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

int main(int argc, char **argv)
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
    n = walk_chain(&v, v.bitmap_first, v.bitmap_length, 1, "(bitmap)");
    if (n >= 0)
        walk_chain(&v, v.upcase_first, v.upcase_length, 1, "(up-case table)");
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
