/**
 * mkmbr: write an MBR partition table into a disk image file.
 *
 * Usage: `mkmbr [--lba] DISK START:SIZE:TYPE[:boot] ...`, one argument per
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
 *
 * `--lba` switches the units to whole sectors and writes the 0xFE 0xFF
 * 0xFF sentinel in both CHS fields, meaning "use the LBA": the form the
 * plain KRFFS disk (krffs/mkdisk.sh) has always carried, where nothing
 * ever reads a geometry.
 *
 * `mkmbr --list DISK` reads instead of writing: one line per non-empty
 * entry, `INDEX TYPE START SIZE`, the type in hex and the extent in bytes.
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

/* Print the table: one line per non-empty entry, `INDEX TYPE START SIZE`
   with the type in hex and the two byte counts in decimal. The suites read
   a partition's extent this way instead of unpacking the table
   themselves. */
static int list_table(const char *path)
{
    unsigned char sector[512];
    FILE *f = fopen(path, "rb");
    int i;

    if (f == NULL || fread(sector, 1, sizeof(sector), f) != sizeof(sector)) {
        fprintf(stderr, "mkmbr: cannot read the first sector of %s\n", path);
        return 1;
    }
    fclose(f);
    if (sector[510] != 0x55 || sector[511] != 0xAA) {
        fprintf(stderr, "mkmbr: no MBR signature in %s\n", path);
        return 1;
    }
    for (i = 0; i < 4; i++) {
        const unsigned char *e = sector + 0x1be + 16 * i;
        unsigned long start = (unsigned long) e[8] |
            (unsigned long) e[9] << 8 | (unsigned long) e[10] << 16 |
            (unsigned long) e[11] << 24;
        unsigned long count = (unsigned long) e[12] |
            (unsigned long) e[13] << 8 | (unsigned long) e[14] << 16 |
            (unsigned long) e[15] << 24;

        if (start == 0 || count == 0)
            continue;
        printf("%d %02x %lu %lu\n", i + 1, e[4], start * 512, count * 512);
    }
    return 0;
}

int main(int argc, char **argv)
{
    unsigned char sector[512];
    unsigned long ends[4], starts[4];
    unsigned long disk_sectors;
    long disk_bytes;
    FILE *f;
    int i, active = 0, lba_units = 0;

    if (argc == 3 && strcmp(argv[1], "--list") == 0)
        return list_table(argv[2]);
    if (argc >= 2 && strcmp(argv[1], "--lba") == 0) {
        lba_units = 1;
        argv++;
        argc--;
    }
    if (argc < 3 || argc > 6) {
        fprintf(stderr,
                "usage: mkmbr [--lba] DISK START:SIZE:TYPE[:boot] ...\n");
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
        if (lba_units) {
            lba = start_mib;
            count = size_mib;
        } else {
            if (start_mib > 0x1FFFFFul || size_mib > 0x200000ul) {
                fprintf(stderr, "mkmbr: partition '%s' runs past the image "
                        "(%lu sectors)\n", argv[i], disk_sectors);
                fclose(f);
                return 2;
            }
            lba = start_mib * 2048;
            count = size_mib * 2048;
        }
        if (lba > 0xFFFFFFFFul - count || lba + count > disk_sectors) {
            fprintf(stderr, "mkmbr: partition '%s' runs past the image "
                    "(%lu sectors)\n", argv[i], disk_sectors);
            fclose(f);
            return 2;
        }
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
        if (lba_units) {
            /* The "use the LBA" sentinel in both CHS fields. */
            e[1] = 0xFE; e[2] = 0xFF; e[3] = 0xFF;
            e[5] = 0xFE; e[6] = 0xFF; e[7] = 0xFF;
        } else {
            chs(lba, e + 1);
            chs(lba + count - 1, e + 5);
        }
        e[4] = (unsigned char) type;
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
