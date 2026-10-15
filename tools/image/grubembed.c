/**
 * grubembed: embed GRUB's i386-pc boot.img and core.img into a disk image.
 *
 * grub-bios-setup installs only on a host block device, which a plain file
 * is not, so this program does the same install on a file: it writes
 * boot.img's first 440 bytes to the MBR (keeping the disk identifier and
 * the partition table, as grub-bios-setup does), places core.img where the
 * table leaves room for it, and patches the fields below, as
 * grub-bios-setup does.
 *
 *   boot.img[0x5c]  the 8-byte LBA of core.img's first sector
 *   boot.img[0x64]  the boot drive (0xff: use the BIOS-provided drive)
 *   core.img[0x1f4] the diskboot blocklist: the LBA and count of the
 *                   sectors after core.img's first, since core.img runs
 *                   contiguously from its first sector
 *
 * On an MBR disk the room is the post-MBR gap: core.img goes at LBA 1, and
 * the program fails if it would run past the gap into a partition. On a disk
 * whose MBR is the protective one of a GUID table, the sectors after the
 * MBR are the table's, and the room is the BIOS boot partition: the entry
 * of type 21686148-6449-6E6F-744E-656564454649, read from the primary
 * header and array after both checksums agree. core.img goes at the
 * partition's first sector.
 *
 * Either room is a window that `grubslot_window` (utils/grubslot.h) lays
 * out as the machine does, LBA 1 through 2047 on an MBR disk and the whole
 * partition on a GUID one: the entry sector and two tails, so that the
 * machine can replace its own boot loader by writing the tail it is not
 * running from. core.img's first sector is the entry sector and the rest
 * must fit in one tail, or the disk would boot and could not have its boot
 * loader replaced.
 *
 * Usage: grubembed <disk.img> <boot.img> <core.img>
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../utils/grubslot.h"

#define SECTOR        512
#define MBR_WINDOW    2047u  /* the post-MBR gap, LBA 1 through 2047 */
#define GPT_ARRAY_MAX 16384u /* 128 entries of 128 bytes */

/* The BIOS boot partition's type GUID as the table stores it. */
static const unsigned char bios_boot[16] = {0x48, 0x61, 0x68, 0x21, 0x49, 0x64, 0x6f, 0x6e,
                                            0x74, 0x4e, 0x65, 0x65, 0x64, 0x45, 0x46, 0x49};

/**
 * Read the whole file `path` into a buffer from malloc and set `*n` to its
 * length. The buffer has one spare sector after the file's bytes, so that
 * the caller can pad the last sector in place, and the caller frees it.
 * Return NULL when the file cannot be opened or read.
 */
static unsigned char *read_file(const char *path, size_t *n)
{
    FILE *f = fopen(path, "rb");
    unsigned char *buf;
    long len;

    if (f == NULL)
        return NULL;
    if (fseek(f, 0, SEEK_END) != 0 || (len = ftell(f)) < 0 || fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        return NULL;
    }
    buf = malloc((size_t)len + SECTOR);
    if (buf == NULL || fread(buf, 1, (size_t)len, f) != (size_t)len) {
        free(buf);
        fclose(f);
        return NULL;
    }
    fclose(f);
    *n = (size_t)len;
    return buf;
}

/** The little-endian 32-bit value at `p`. */
static uint32_t r32(const unsigned char *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

/** The little-endian 64-bit value at `p`. */
static uint64_t r64(const unsigned char *p)
{
    return (uint64_t)r32(p) | (uint64_t)r32(p + 4) << 32;
}

/**
 * The CRC-32 of `length` bytes at `data`, the checksum a GUID table keeps
 * of its header and of its entry array.
 */
static uint32_t crc32_of(const unsigned char *data, size_t length)
{
    uint32_t crc = 0xFFFFFFFFu;
    size_t i;
    int bit;

    for (i = 0; i < length; i++) {
        crc ^= data[i];
        for (bit = 0; bit < 8; bit++) {
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
        }
    }
    return ~crc;
}

/**
 * Find the BIOS boot partition of the GUID table on `f`: set its first
 * sector in `*first` and its length in sectors in `*count`. The primary
 * header and array are read, and both checksums must match. The partition
 * must lie in the table's usable sectors and after its primary entry array,
 * since the core is written over the whole partition, and a table whose
 * checksums match can still put a partition over its own array. Return 0,
 * or 1 after a line on stderr.
 */
static int bios_boot_partition(FILE *f, unsigned long *first, unsigned long *count)
{
    static unsigned char array[GPT_ARRAY_MAX];
    unsigned char header[SECTOR];
    uint32_t entries, size, header_size, crc;
    uint64_t start, last, first_usable, last_usable, array_end;
    uint32_t i;

    if (fseek(f, SECTOR, SEEK_SET) != 0 || fread(header, 1, SECTOR, f) != SECTOR ||
        memcmp(header, "EFI PART", 8) != 0) {
        fprintf(stderr, "grubembed: the disk has a protective MBR and no GUID header\n");
        return 1;
    }
    header_size = r32(header + 12);
    crc = r32(header + 16);
    entries = r32(header + 80);
    size = r32(header + 84);
    if (header_size < 92 || header_size > SECTOR || size < 128 || entries == 0 ||
        (uint64_t)entries * size > GPT_ARRAY_MAX) {
        fprintf(stderr, "grubembed: the GUID header's sizes are not ones this reads\n");
        return 1;
    }
    memset(header + 16, 0, 4);
    if (crc32_of(header, header_size) != crc) {
        fprintf(stderr, "grubembed: the GUID header's checksum does not match\n");
        return 1;
    }
    if (fseek(f, (long)(r64(header + 72) * SECTOR), SEEK_SET) != 0 ||
        fread(array, 1, (size_t)entries * size, f) != (size_t)entries * size ||
        crc32_of(array, (size_t)entries * size) != r32(header + 88)) {
        fprintf(stderr, "grubembed: the GUID entry array does not read or check\n");
        return 1;
    }
    first_usable = r64(header + 40);
    last_usable = r64(header + 48);
    array_end = r64(header + 72) + ((uint64_t)entries * size + SECTOR - 1) / SECTOR;
    for (i = 0; i < entries; i++) {
        const unsigned char *e = array + (size_t)i * size;

        if (memcmp(e, bios_boot, 16) != 0) {
            continue;
        }
        start = r64(e + 32);
        last = r64(e + 40);
        if (start < 2 || start < first_usable || start < array_end || last < start ||
            last > last_usable || last - start + 1 > 0xFFFFFFFFu) {
            fprintf(stderr, "grubembed: the BIOS boot partition's range is not one this "
                            "writes\n");
            return 1;
        }
        *first = (unsigned long)start;
        *count = (unsigned long)(last - start + 1);
        return 0;
    }
    fprintf(stderr, "grubembed: the GUID table has no BIOS boot partition\n");
    return 1;
}

/** Store `lo` at `p` as a little-endian 64-bit value with a zero high half. */
static void w64(unsigned char *p, uint32_t lo)
{
    int i;

    p[0] = (unsigned char)lo;
    p[1] = (unsigned char)(lo >> 8);
    p[2] = (unsigned char)(lo >> 16);
    p[3] = (unsigned char)(lo >> 24);
    for (i = 4; i < 8; i++)
        p[i] = 0;
}

/**
 * Install boot.img and core.img on the disk, as the file's opening comment
 * says. Exit 0, 1 after a line on stderr, or 2 on a usage error. The disk
 * is written only after every check passes.
 */
int main(int argc, char **argv)
{
    unsigned char mbr[SECTOR];
    unsigned char *boot, *core;
    size_t boot_len, core_len;
    unsigned long core_sectors, first_lba = 0, core_lba = 1, room = MBR_WINDOW;
    struct grubslot_window w;
    char why[128];
    int i;
    FILE *f;

    if (argc != 4) {
        fprintf(stderr, "usage: grubembed <disk.img> <boot.img> <core.img>\n");
        return 2;
    }
    boot = read_file(argv[2], &boot_len);
    if (boot == NULL || boot_len < SECTOR) {
        fprintf(stderr, "grubembed: cannot read a full sector from %s\n", argv[2]);
        return 1;
    }
    core = read_file(argv[3], &core_len);
    if (core == NULL || core_len == 0) {
        fprintf(stderr, "grubembed: cannot read %s\n", argv[3]);
        return 1;
    }
    core_sectors = (core_len + SECTOR - 1) / SECTOR;
    memset(core + core_len, 0, core_sectors * SECTOR - core_len);

    f = fopen(argv[1], "r+b");
    if (f == NULL || fread(mbr, 1, SECTOR, f) != SECTOR) {
        fprintf(stderr, "grubembed: cannot read the MBR of %s\n", argv[1]);
        return 1;
    }

    if (mbr[0x1be + 4] == 0xEE) {
        if (bios_boot_partition(f, &core_lba, &room) != 0) {
            return 1;
        }
    } else {
        for (i = 0; i < 4; i++) {
            const unsigned char *e = mbr + 0x1be + 16 * i + 8;
            unsigned long start = (unsigned long)r32(e);

            if (start != 0 && (first_lba == 0 || start < first_lba))
                first_lba = start;
        }
        if (first_lba != 0 && core_sectors >= first_lba) {
            fprintf(stderr,
                    "grubembed: core.img needs %lu sectors but the gap "
                    "before LBA %lu has only %lu\n",
                    core_sectors, first_lba, first_lba - 1);
            return 1;
        }
    }
    if (grubslot_window(core_lba, room, &w, why, sizeof(why)) != 0) {
        fprintf(stderr, "grubembed: %s\n", why);
        return 1;
    }
    if (core_sectors - 1 > w.tail) {
        fprintf(stderr,
                "grubembed: core.img is %lu sectors and a tail slot after "
                "its entry sector has %lu; the disk would boot but could not "
                "have its boot loader replaced\n",
                core_sectors, (unsigned long)w.tail);
        return 1;
    }

    w64(boot + 0x5c, (uint32_t)core_lba);
    boot[0x64] = 0xff;
    w64(core + 0x1f4, (uint32_t)core_lba + 1);
    core[0x1fc] = (unsigned char)(core_sectors - 1);
    core[0x1fd] = (unsigned char)((core_sectors - 1) >> 8);

    if (fseek(f, 0, SEEK_SET) != 0 || fwrite(boot, 1, 440, f) != 440 ||
        fseek(f, (long)(core_lba * SECTOR), SEEK_SET) != 0 ||
        fwrite(core, 1, core_sectors * SECTOR, f) != core_sectors * SECTOR || fclose(f) != 0) {
        fprintf(stderr, "grubembed: cannot write %s\n", argv[1]);
        return 1;
    }
    free(boot);
    free(core);
    return 0;
}
