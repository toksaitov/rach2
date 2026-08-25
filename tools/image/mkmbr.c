/**
 * mkmbr: write an MBR partition table into a disk image file.
 *
 * Usage: `mkmbr DISK START:SIZE:TYPE[:boot] ...`, one argument per
 * partition in table order, at most four. START and SIZE are whole MiB,
 * TYPE is the hexadecimal partition type byte, and a trailing `:boot`
 * sets the active flag. Only the 64 table bytes and the 0x55aa signature
 * are written; the rest of the first sector, and everything after it, is
 * left exactly as it was, which is what lets the GRUB embed step write its
 * boot code around this table later.
 *
 * This replaces parted inside the Docker container, and it writes the same
 * bytes parted wrote, CHS fields included, so a disk built natively is
 * byte-identical to the reference the container produced. The CHS values
 * use parted's 4-head, 32-sector translation and saturate at cylinder
 * 1023; nothing boots by them (the kernel's MBR parser and GRUB read the
 * LBA fields alone), but identical bytes make the conversion provable by
 * comparison instead of by argument.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* parted's CHS for this geometry: 4 heads, 32 sectors per track. */
static void chs(unsigned long lba, unsigned char *out)
{
    unsigned long cyl = lba / (4 * 32);
    unsigned long head = (lba / 32) % 4;
    unsigned long sec = lba % 32 + 1;

    if (cyl > 1023) {
        cyl = 1023;
        head = 3;
        sec = 32;
    }
    out[0] = (unsigned char) head;
    out[1] = (unsigned char) (((cyl >> 8) << 6) | sec);
    out[2] = (unsigned char) cyl;
}

int main(int argc, char **argv)
{
    unsigned char sector[512];
    FILE *f;
    int i;

    if (argc < 3 || argc > 6) {
        fprintf(stderr, "usage: mkmbr DISK START:SIZE:TYPE[:boot] ...\n");
        return 2;
    }
    f = fopen(argv[1], "r+b");
    if (f == NULL) {
        fprintf(stderr, "mkmbr: cannot open %s\n", argv[1]);
        return 1;
    }
    if (fread(sector, 1, sizeof(sector), f) != sizeof(sector)) {
        fprintf(stderr, "mkmbr: %s is shorter than one sector\n", argv[1]);
        fclose(f);
        return 1;
    }
    memset(sector + 0x1be, 0, 64);
    for (i = 2; i < argc; i++) {
        unsigned long start_mib, size_mib, type;
        unsigned long lba, count;
        unsigned char *e = sector + 0x1be + 16 * (i - 2);
        char boot[5] = "";
        int n = sscanf(argv[i], "%lu:%lu:%lx:%4s",
                       &start_mib, &size_mib, &type, boot);

        if (n < 3 || type > 0xff || size_mib == 0 ||
            (n == 4 && strcmp(boot, "boot") != 0)) {
            fprintf(stderr, "mkmbr: bad partition '%s'\n", argv[i]);
            fclose(f);
            return 2;
        }
        lba = start_mib * 2048;
        count = size_mib * 2048;
        e[0] = n == 4 ? 0x80 : 0x00;
        chs(lba, e + 1);
        e[4] = (unsigned char) type;
        chs(lba + count - 1, e + 5);
        e[8] = (unsigned char) lba;
        e[9] = (unsigned char) (lba >> 8);
        e[10] = (unsigned char) (lba >> 16);
        e[11] = (unsigned char) (lba >> 24);
        e[12] = (unsigned char) count;
        e[13] = (unsigned char) (count >> 8);
        e[14] = (unsigned char) (count >> 16);
        e[15] = (unsigned char) (count >> 24);
    }
    sector[510] = 0x55;
    sector[511] = 0xaa;
    if (fseek(f, 0, SEEK_SET) != 0 ||
        fwrite(sector, 1, sizeof(sector), f) != sizeof(sector)) {
        fprintf(stderr, "mkmbr: cannot write the table back\n");
        fclose(f);
        return 1;
    }
    fclose(f);
    return 0;
}
