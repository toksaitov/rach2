/**
 * grubembed: embed GRUB's i386-pc boot.img and core.img into a disk image.
 *
 * grub-bios-setup wants to resolve the image file to a host block device,
 * which a plain file is not, so this performs the same install directly:
 * it writes boot.img's first 440 bytes to the MBR (keeping the disk
 * identifier and the partition table, as grub-bios-setup does), places
 * core.img in the post-MBR gap at LBA 1, and patches the two pointers
 * grub-bios-setup would patch.
 *
 *   boot.img[0x5c]  the 8-byte LBA of core.img's first sector (1)
 *   boot.img[0x64]  the boot drive (0xff: use the BIOS-provided drive)
 *   core.img[0x1f4] the diskboot blocklist: the LBA (2) and count of the
 *                   sectors after core.img's first, since core.img runs
 *                   contiguously from LBA 1
 *
 * The gap is whatever lies between LBA 1 and the first partition; this
 * aborts if core.img would run past it into a partition. It also refuses
 * a core over 1024 sectors: the gap is two tail slots of 1023 sectors
 * (utils/grubslot.h) so the machine can replace its own boot loader by
 * writing the slot it is not running from, and an image whose tail ran
 * past LBA 1024 would leave no free slot.
 *
 * Usage: grubembed <disk.img> <boot.img> <core.img>
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SECTOR 512
#define MAX_IMAGE_SECTORS 1024

static unsigned char *read_file(const char *path, size_t *n)
{
    FILE *f = fopen(path, "rb");
    unsigned char *buf;
    long len;

    if (f == NULL)
        return NULL;
    if (fseek(f, 0, SEEK_END) != 0 || (len = ftell(f)) < 0 ||
        fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        return NULL;
    }
    /* One spare sector so the caller can pad in place. */
    buf = malloc((size_t) len + SECTOR);
    if (buf == NULL || fread(buf, 1, (size_t) len, f) != (size_t) len) {
        free(buf);
        fclose(f);
        return NULL;
    }
    fclose(f);
    *n = (size_t) len;
    return buf;
}

static void w64(unsigned char *p, uint32_t lo)
{
    int i;

    p[0] = (unsigned char) lo;
    p[1] = (unsigned char) (lo >> 8);
    p[2] = (unsigned char) (lo >> 16);
    p[3] = (unsigned char) (lo >> 24);
    for (i = 4; i < 8; i++)
        p[i] = 0;
}

int main(int argc, char **argv)
{
    unsigned char mbr[SECTOR];
    unsigned char *boot, *core;
    size_t boot_len, core_len;
    unsigned long core_sectors, first_lba = 0;
    int i;
    FILE *f;

    if (argc != 4) {
        fprintf(stderr, "usage: grubembed <disk.img> <boot.img> <core.img>\n");
        return 2;
    }
    boot = read_file(argv[2], &boot_len);
    if (boot == NULL || boot_len < SECTOR) {
        fprintf(stderr, "grub_embed: cannot read a full sector from %s\n",
                argv[2]);
        return 1;
    }
    core = read_file(argv[3], &core_len);
    if (core == NULL || core_len == 0) {
        fprintf(stderr, "grub_embed: cannot read %s\n", argv[3]);
        return 1;
    }
    core_sectors = (core_len + SECTOR - 1) / SECTOR;
    memset(core + core_len, 0, core_sectors * SECTOR - core_len);

    f = fopen(argv[1], "r+b");
    if (f == NULL || fread(mbr, 1, SECTOR, f) != SECTOR) {
        fprintf(stderr, "grub_embed: cannot read the MBR of %s\n", argv[1]);
        return 1;
    }

    for (i = 0; i < 4; i++) {
        const unsigned char *e = mbr + 0x1be + 16 * i + 8;
        unsigned long start = (unsigned long) e[0] | (unsigned long) e[1] << 8 |
            (unsigned long) e[2] << 16 | (unsigned long) e[3] << 24;

        if (start != 0 && (first_lba == 0 || start < first_lba))
            first_lba = start;
    }
    if (first_lba != 0 && core_sectors >= first_lba) {
        fprintf(stderr, "grub_embed: core.img needs %lu sectors but the gap "
                "before LBA %lu has only %lu\n", core_sectors, first_lba,
                first_lba - 1);
        return 1;
    }
    if (core_sectors > MAX_IMAGE_SECTORS) {
        fprintf(stderr, "grub_embed: core.img is %lu sectors and a tail slot "
                "has %d; the disk would boot but could not have its boot "
                "loader replaced\n", core_sectors, MAX_IMAGE_SECTORS - 1);
        return 1;
    }

    /* core.img lives at LBA 1; boot.img must load it from there. The
       diskboot blocklist at the end of core.img's first sector names the
       rest: starting LBA 2, count = all sectors but the first. */
    w64(boot + 0x5c, 1);
    boot[0x64] = 0xff;
    w64(core + 0x1f4, 2);
    core[0x1fc] = (unsigned char) (core_sectors - 1);
    core[0x1fd] = (unsigned char) ((core_sectors - 1) >> 8);

    if (fseek(f, 0, SEEK_SET) != 0 || fwrite(boot, 1, 440, f) != 440 ||
        fseek(f, SECTOR, SEEK_SET) != 0 ||
        fwrite(core, 1, core_sectors * SECTOR, f) != core_sectors * SECTOR ||
        fclose(f) != 0) {
        fprintf(stderr, "grub_embed: cannot write %s\n", argv[1]);
        return 1;
    }
    free(boot);
    free(core);
    return 0;
}
