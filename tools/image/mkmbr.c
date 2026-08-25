/**
 * mkmbr: write an MBR partition table into a disk image file.
 *
 * Usage: `mkmbr DISK START:SIZE:TYPE[:boot] ...`, one argument per
 * partition in table order, at most four. START and SIZE are whole MiB,
 * TYPE is the hexadecimal partition type byte, and a trailing `:boot`
 * sets the active flag. A specification is refused unless it parses in
 * full, fits the 32-bit LBA fields, lies inside the image, does not
 * overlap an earlier entry, and marks at most one partition active. Only
 * the 64 table bytes and the 0x55aa signature are written; the rest of the
 * first sector, and everything after it, is left exactly as it was, which
 * is what lets the GRUB embed step write its boot code around this table
 * later.
 *
 * This replaces parted inside the Docker container, and it writes the same
 * bytes parted wrote, CHS fields included, so a disk built natively is
 * byte-identical to the reference the container produced. The CHS values
 * use parted's 4-head, 32-sector translation and saturate at cylinder
 * 1023; nothing boots by them (the kernel's MBR parser and GRUB read the
 * LBA fields alone), but identical bytes make the conversion provable by
 * comparison instead of by argument.
 */
#include <errno.h>
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

/* Parse one unsigned field strictly: at least one digit, no sign, no
   overflow, ending at a `:` or at the end of the argument. Returns nonzero
   and leaves `*p` on the delimiter (or the terminator) on success. */
static int field(const char **p, int base, unsigned long *out)
{
    const char *s = *p;
    char *end;

    if (*s == '+' || *s == '-')
        return 0;
    errno = 0;
    *out = strtoul(s, &end, base);
    if (end == s || errno == ERANGE || (*end != ':' && *end != '\0'))
        return 0;
    *p = end;
    return 1;
}

/* Require and step over a `:`. */
static int colon(const char **p)
{
    if (**p != ':')
        return 0;
    (*p)++;
    return 1;
}

int main(int argc, char **argv)
{
    unsigned char sector[512];
    unsigned long ends[4], starts[4];
    unsigned long disk_sectors;
    long disk_bytes;
    FILE *f;
    int i, active = 0;

    if (argc < 3 || argc > 6) {
        fprintf(stderr, "usage: mkmbr DISK START:SIZE:TYPE[:boot] ...\n");
        return 2;
    }
    f = fopen(argv[1], "r+b");
    if (f == NULL) {
        fprintf(stderr, "mkmbr: cannot open %s\n", argv[1]);
        return 1;
    }
    if (fseek(f, 0, SEEK_END) != 0 || (disk_bytes = ftell(f)) < 0 ||
        fseek(f, 0, SEEK_SET) != 0) {
        fprintf(stderr, "mkmbr: cannot size %s\n", argv[1]);
        fclose(f);
        return 1;
    }
    disk_sectors = (unsigned long) disk_bytes / 512;
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
        const char *p = argv[i];
        int boot = 0, j;

        if (!field(&p, 10, &start_mib) || !colon(&p) ||
            !field(&p, 10, &size_mib) || !colon(&p) ||
            !field(&p, 16, &type) ||
            type > 0xff || size_mib == 0 ||
            (*p != '\0' && strcmp(p, ":boot") != 0)) {
            fprintf(stderr, "mkmbr: bad partition '%s'\n", argv[i]);
            fclose(f);
            return 2;
        }
        boot = *p != '\0';
        /* The LBA fields are 32 bits, so the whole partition, end
           included, must sit below 2 TiB and inside the image. */
        if (start_mib > 0x1FFFFFul || size_mib > 0x200000ul ||
            (start_mib + size_mib) * 2048 > 0xFFFFFFFFul ||
            (start_mib + size_mib) * 2048 > disk_sectors) {
            fprintf(stderr, "mkmbr: partition '%s' runs past the image "
                    "(%lu sectors)\n", argv[i], disk_sectors);
            fclose(f);
            return 2;
        }
        lba = start_mib * 2048;
        count = size_mib * 2048;
        starts[i - 2] = lba;
        ends[i - 2] = lba + count;
        for (j = 0; j < i - 2; j++) {
            if (lba < ends[j] && starts[j] < lba + count) {
                fprintf(stderr, "mkmbr: partition '%s' overlaps entry %d\n",
                        argv[i], j + 1);
                fclose(f);
                return 2;
            }
        }
        if (boot && active++) {
            fprintf(stderr, "mkmbr: more than one partition marked boot\n");
            fclose(f);
            return 2;
        }
        e[0] = boot ? 0x80 : 0x00;
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
    /* fclose flushes the buffered sector; a full disk surfaces here, and
       returning success over it would bless a disk with no table. */
    if (fclose(f) != 0) {
        fprintf(stderr, "mkmbr: cannot write the table back\n");
        return 1;
    }
    return 0;
}
