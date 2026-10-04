/*
 * gptdisk: a disk's GUID table from user space. `gptdisk.h` says what it
 * reads and writes, and in what order.
 */
#define _POSIX_C_SOURCE 200809L
#include "gptdisk.h"

#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>

#ifndef BLKGETSIZE64
#define BLKGETSIZE64 _IOR(0x12, 114, size_t)
#endif

#define HEADER_MIN 92u
#define ENTRY_MIN  128u

static uint32_t le32(const unsigned char *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

uint64_t gptdisk_le64(const unsigned char *p)
{
    return (uint64_t)le32(p) | (uint64_t)le32(p + 4) << 32;
}

static void put32(unsigned char *p, uint32_t v)
{
    p[0] = (unsigned char)v;
    p[1] = (unsigned char)(v >> 8);
    p[2] = (unsigned char)(v >> 16);
    p[3] = (unsigned char)(v >> 24);
}

void gptdisk_put64(unsigned char *p, uint64_t v)
{
    put32(p, (uint32_t)v);
    put32(p + 4, (uint32_t)(v >> 32));
}

uint32_t gptdisk_crc32(const unsigned char *data, uint32_t length)
{
    uint32_t crc = 0xFFFFFFFFu;
    uint32_t i;
    int bit;

    for (i = 0; i < length; i++) {
        crc ^= data[i];
        for (bit = 0; bit < 8; bit++) {
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
        }
    }
    return ~crc;
}

int gptdisk_number(const char *path)
{
    int number = 0;
    const char *p;

    if (strncmp(path, "/dev/disk", 9) != 0 || path[9] == '\0') {
        return -1;
    }
    for (p = path + 9; *p != '\0'; p++) {
        if (*p < '0' || *p > '9' || number > 99) {
            return -1;
        }
        number = number * 10 + (*p - '0');
    }
    return number;
}

int gptdisk_sectors(int number, uint64_t *sectors)
{
    FILE *f = fopen("/proc/disk", "r");
    char line[256];
    int found = -1;

    if (!f) {
        return -1;
    }
    while (fgets(line, sizeof(line), f)) {
        int n;
        unsigned long long count;

        if (sscanf(line, "disk %d %*s %*d %llu", &n, &count) == 2 && n == number) {
            *sectors = count;
            found = 0;
        }
    }
    fclose(f);
    return found;
}

/** Read `count` sectors at `lba` into `buf`; 0, or -1. */
static int read_at(int fd, uint64_t lba, void *buf, uint32_t count)
{
    off_t at = (off_t)lba * (off_t)GPTDISK_SECTOR;
    size_t bytes = (size_t)count * GPTDISK_SECTOR;

    if (lseek(fd, at, SEEK_SET) != at || read(fd, buf, bytes) != (ssize_t)bytes) {
        return -1;
    }
    return 0;
}

/** Write `count` sectors at `lba` from `buf`; 0, or -1. */
static int write_at(int fd, uint64_t lba, const void *buf, uint32_t count)
{
    off_t at = (off_t)lba * (off_t)GPTDISK_SECTOR;
    size_t bytes = (size_t)count * GPTDISK_SECTOR;

    if (lseek(fd, at, SEEK_SET) != at || write(fd, buf, bytes) != (ssize_t)bytes) {
        return -1;
    }
    return 0;
}

/** The sectors the entry array of `g` takes. */
static uint32_t array_sectors(const struct gptdisk *g)
{
    return (g->entries * g->entry_size + GPTDISK_SECTOR - 1) / GPTDISK_SECTOR;
}

/**
 * Judge the header in `g->header`, read from `lba`, by the rules the kernel
 * applies (`gpt_header_ok`), and read and check its array. Returns 0, or -1.
 */
static int take_header(int fd, struct gptdisk *g, uint64_t lba)
{
    unsigned char copy[GPTDISK_SECTOR];
    uint32_t size = le32(g->header + 12);
    uint64_t first, last, array_lba;

    if (memcmp(g->header, "EFI PART", 8) != 0 || le32(g->header + 8) != 0x00010000u ||
        size < HEADER_MIN || size > GPTDISK_SECTOR) {
        return -1;
    }
    memcpy(copy, g->header, GPTDISK_SECTOR);
    memset(copy + 16, 0, 4);
    if (gptdisk_crc32(copy, size) != le32(g->header + 16) || gptdisk_le64(g->header + 24) != lba) {
        return -1;
    }
    g->entries = le32(g->header + 80);
    g->entry_size = le32(g->header + 84);
    first = gptdisk_le64(g->header + 40);
    last = gptdisk_le64(g->header + 48);
    array_lba = gptdisk_le64(g->header + 72);
    if (g->entry_size < ENTRY_MIN || g->entry_size % ENTRY_MIN != 0 || g->entries == 0 ||
        g->entries > GPTDISK_ARRAY_MAX / g->entry_size || first > last || last >= g->sectors ||
        array_lba < 1 || array_lba >= g->sectors || array_sectors(g) > g->sectors - array_lba) {
        return -1;
    }
    memset(g->array, 0, sizeof(g->array));
    if (read_at(fd, array_lba, g->array, array_sectors(g)) != 0 ||
        gptdisk_crc32(g->array, g->entries * g->entry_size) != le32(g->header + 88)) {
        return -1;
    }
    return 0;
}

int gptdisk_read(int fd, uint64_t sectors, struct gptdisk *g)
{
    memset(g, 0, sizeof(*g));
    g->sectors = sectors;
    if (sectors < 3 || read_at(fd, 0, g->mbr, 1) != 0) {
        return -1;
    }
    if (g->mbr[510] != 0x55 || g->mbr[511] != 0xAA || g->mbr[446 + 4] != 0xEE) {
        return 0;
    }
    if (read_at(fd, 1, g->header, 1) == 0 && take_header(fd, g, 1) == 0) {
        return 1;
    }
    g->from_backup = 1;
    if (read_at(fd, sectors - 1, g->header, 1) == 0 && take_header(fd, g, sectors - 1) == 0) {
        return 1;
    }
    return -1;
}

unsigned char *gptdisk_entry(struct gptdisk *g, int number)
{
    if (number < 1 || (uint32_t)number > g->entries) {
        return NULL;
    }
    return g->array + (size_t)(number - 1) * g->entry_size;
}

int gptdisk_used(const unsigned char *entry)
{
    int i;

    for (i = 0; i < 16; i++) {
        if (entry[i] != 0) {
            return 1;
        }
    }
    return 0;
}

int gptdisk_backup_at_end(const struct gptdisk *g)
{
    return !g->from_backup && gptdisk_le64(g->header + 32) == g->sectors - 1 &&
           gptdisk_le64(g->header + 48) == g->sectors - 2 - array_sectors(g);
}

/**
 * The bytes the descriptor `fd` reaches: a disk node's window, which
 * `BLKGETSIZE64` gives, or a regular file's size, for an image on a host.
 */
static uint64_t reach(int fd)
{
    uint64_t bytes = 0;
    struct stat st;

    if (ioctl(fd, BLKGETSIZE64, &bytes) == 0) {
        return bytes;
    }
    if (fstat(fd, &st) == 0 && S_ISREG(st.st_mode)) {
        return (uint64_t)st.st_size;
    }
    return 0;
}

/** Fill `out` as the header at `my_lba` whose array is at `array_lba`. */
static void make_header(const struct gptdisk *g, unsigned char *out, uint64_t my_lba,
                        uint64_t other_lba, uint64_t array_lba, uint64_t last_usable)
{
    uint32_t size = le32(g->header + 12);

    memcpy(out, g->header, GPTDISK_SECTOR);
    gptdisk_put64(out + 24, my_lba);
    gptdisk_put64(out + 32, other_lba);
    gptdisk_put64(out + 48, last_usable);
    gptdisk_put64(out + 72, array_lba);
    put32(out + 88, gptdisk_crc32(g->array, g->entries * g->entry_size));
    memset(out + 16, 0, 4);
    put32(out + 16, gptdisk_crc32(out, size));
}

int gptdisk_write(int fd, struct gptdisk *g, const char **why)
{
    unsigned char primary[GPTDISK_SECTOR], backup[GPTDISK_SECTOR];
    uint64_t backup_array, last_usable, first_usable;
    uint64_t primary_array = 2;
    uint32_t n;

    if (reach(fd) / GPTDISK_SECTOR < g->sectors) {
        *why = "the whole-disk node does not reach the disk's last sector";
        return -1;
    }
    backup_array = g->sectors - 1 - array_sectors(g);
    last_usable = backup_array - 1;
    first_usable = gptdisk_le64(g->header + 40);
    if (!g->from_backup) {
        primary_array = gptdisk_le64(g->header + 72);
    }
    if (primary_array + array_sectors(g) > first_usable || last_usable < first_usable) {
        *why = "the table's arrays do not fit around its usable sectors";
        return -1;
    }
    for (n = 1; n <= g->entries; n++) {
        const unsigned char *e = gptdisk_entry(g, (int)n);

        if (gptdisk_used(e) && gptdisk_le64(e + 40) > last_usable) {
            *why = "a partition reaches past where the backup table would go";
            return -1;
        }
    }
    make_header(g, backup, g->sectors - 1, 1, backup_array, last_usable);
    make_header(g, primary, 1, g->sectors - 1, primary_array, last_usable);
    if (g->from_backup) {
        if (write_at(fd, primary_array, g->array, array_sectors(g)) != 0 ||
            write_at(fd, 1, primary, 1) != 0) {
            *why = "the primary table was not written; the old backup serves";
            return -1;
        }
        if (write_at(fd, backup_array, g->array, array_sectors(g)) != 0 ||
            write_at(fd, g->sectors - 1, backup, 1) != 0) {
            *why = "the backup table was not written; the primary serves";
            return -1;
        }
    } else {
        if (write_at(fd, backup_array, g->array, array_sectors(g)) != 0 ||
            write_at(fd, g->sectors - 1, backup, 1) != 0) {
            *why = "the backup table was not written";
            return -1;
        }
        if (write_at(fd, primary_array, g->array, array_sectors(g)) != 0 ||
            write_at(fd, 1, primary, 1) != 0) {
            *why = "the primary table was not written; the backup at the disk's end serves";
            return -1;
        }
    }
    put32(g->mbr + 446 + 12,
          g->sectors - 1 > 0xFFFFFFFFu ? 0xFFFFFFFFu : (uint32_t)(g->sectors - 1));
    if (write_at(fd, 0, g->mbr, 1) != 0) {
        *why = "the protective MBR was not written";
        return -1;
    }
    memcpy(g->header, primary, GPTDISK_SECTOR);
    g->from_backup = 0;
    return 0;
}

void gptdisk_guid_text(const unsigned char *guid, char out[37])
{
    static const int order[16] = {3, 2, 1, 0, 5, 4, 7, 6, 8, 9, 10, 11, 12, 13, 14, 15};
    static const char digits[] = "0123456789abcdef";
    int i, n = 0;

    for (i = 0; i < 16; i++) {
        if (i == 4 || i == 6 || i == 8 || i == 10) {
            out[n++] = '-';
        }
        out[n++] = digits[guid[order[i]] >> 4];
        out[n++] = digits[guid[order[i]] & 0xF];
    }
    out[n] = '\0';
}

/** The value of hexadecimal digit `c`, or -1. */
static int hex_digit(int c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

int gptdisk_guid_parse(const char *text, unsigned char guid[16])
{
    static const int order[16] = {3, 2, 1, 0, 5, 4, 7, 6, 8, 9, 10, 11, 12, 13, 14, 15};
    int i, n = 0;

    for (i = 0; i < 16; i++) {
        int hi, lo;

        if (i == 4 || i == 6 || i == 8 || i == 10) {
            if (text[n++] != '-') {
                return -1;
            }
        }
        hi = hex_digit((unsigned char)text[n]);
        lo = hi < 0 ? -1 : hex_digit((unsigned char)text[n + 1]);
        if (lo < 0) {
            return -1;
        }
        guid[order[i]] = (unsigned char)(hi << 4 | lo);
        n += 2;
    }
    return text[n] == '\0' ? 0 : -1;
}
