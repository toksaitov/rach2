/*
 * installer_reflash.c: the scenario of machine.installer.reflash: the two
 * halves of a raw write on the machine that does it. The product stick
 * boots with a blank disk beside it; 32 MiB of the sealed disk on the stick
 * are written to the disk with `reflash -p`, which says how far it has got,
 * and read back with `reflash --verify`, whose digest must be the one
 * sha256sum takes of the same bytes. Then 2 MiB are written at a later
 * offset and verified there, so that a readback that ignored the offset
 * would be found.
 */
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lib_installer.h"

/**
 * Copy into `hex` the digest of the `nth` line of `text`, counted from 1,
 * that sha256sum printed for its standard input: 64 hexadecimal digits and
 * `  -`.
 *
 * @return 0, or -1 when `text` has no such line.
 */
static int sum_line(const char *text, int nth, char hex[65])
{
    const char *line = text, *eol;
    int i;

    while (line != NULL && *line != '\0') {
        eol = strchr(line, '\n');
        for (i = 0; i < 64 && isxdigit((unsigned char)line[i]) && !isupper((unsigned char)line[i]);
             i++)
            ;
        if (i == 64 && strncmp(line + 64, "  -", 3) == 0 && --nth == 0) {
            memcpy(hex, line, 64);
            hex[64] = '\0';
            return 0;
        }
        line = eol != NULL ? eol + 1 : NULL;
    }
    return -1;
}

/**
 * Copy into `hex` the digest of the `nth` line `reflash: sha256 HEX` of
 * `text`, the readback of `reflash --verify`.
 *
 * @return 0, or -1 when `text` has no such line.
 */
static int verify_line(const char *text, int nth, char hex[65])
{
    char rest[200];

    if (text_field(text, "reflash: sha256 ", nth, rest, sizeof(rest)) != 0)
        return -1;
    rest[strcspn(rest, " ")] = '\0';
    snprintf(hex, 65, "%.64s", rest);
    return 0;
}

/**
 * Count the lines of `text` on which `reflash: `, a number and
 * ` of TOTAL bytes` follow each other: the progress lines of a write of
 * `total` bytes.
 */
static int progress_lines(const char *text, const char *total)
{
    char tail[64];
    const char *line = text, *eol, *p;
    int count = 0;

    snprintf(tail, sizeof(tail), " of %s bytes", total);
    while (line != NULL && *line != '\0') {
        eol = strchr(line, '\n');
        for (p = line; (p = strstr(p, "reflash: ")) != NULL && (eol == NULL || p < eol); p++) {
            const char *q = p + strlen("reflash: ");

            while (isdigit((unsigned char)*q))
                q++;
            if (strncmp(q, tail, strlen(tail)) == 0) {
                count++;
                break;
            }
        }
        line = eol != NULL ? eol + 1 : NULL;
    }
    return count;
}

/**
 * Boot the stick beside the blank disk, make the two writes and their
 * readbacks at the console, and judge the digests and the progress lines.
 */
int etude_scenario(struct etude_case *c)
{
    char image[128] = "", one[65] = "", two[65] = "", back_one[65] = "", back_two[65] = "";
    char detail[300];
    struct drive d;
    char *text = NULL;
    int steps;

    if (etude_step(c, "write") != ETUDE_OK)
        return 1;
    drive_boot(&d, c, "writer", NULL);
    drive_expect(&d, "recovery image");
    drive_expect(&d, "# ");
    drive_send(&d, "mkdir -p /mnt && mount -t exfat /dev/disk1p2 /mnt && echo MOUNT\"\"ED");
    drive_expect(&d, "MOUNTED");
    drive_send(&d, "cat /mnt/rach2-install.manifest; echo MANI\"\"FEST");
    drive_expect(&d, "MANIFEST");
    if (!d.failed && (text = drive_console(&d)) != NULL)
        text_field(text, "image ", 1, image, sizeof(image));
    free(text);
    text = NULL;
    if (image[0] == '\0')
        inst_refuse(&d, "the manifest names no image");
    drive_send(&d, "dd if=/mnt/%s bs=1048576 count=32 2>/dev/null | sha256sum; echo SUM\"\"ONE",
               image);
    drive_expect(&d, "SUMONE");
    drive_send(&d,
               "dd if=/mnt/%s bs=1048576 count=32 2>/dev/null | reflash -p /dev/disk0 0 33554432",
               image);
    drive_expect(&d, "wrote 33554432 bytes");
    drive_send(&d, "reflash --verify /dev/disk0 0 33554432; echo VERIFY\"\"ONE");
    drive_expect(&d, "VERIFYONE");
    drive_send(&d,
               "dd if=/mnt/%s bs=1048576 skip=40 count=2 2>/dev/null | sha256sum; echo SUM\"\"TWO",
               image);
    drive_expect(&d, "SUMTWO");
    drive_send(&d,
               "dd if=/mnt/%s bs=1048576 skip=40 count=2 2>/dev/null | "
               "reflash /dev/disk0 33554432 2097152",
               image);
    drive_expect(&d, "wrote 2097152 bytes");
    drive_send(&d, "reflash --verify /dev/disk0 33554432 2097152; echo VERIFY\"\"TWO");
    drive_expect(&d, "VERIFYTWO");
    drive_send(&d, "umount /mnt && echo REFLASH\"\"DONE");
    drive_expect(&d, "REFLASHDONE");
    text = drive_console(&d);
    drive_stop(&d);
    if (drive_halted()) {
        free(text);
        return 1;
    }
    inst_assert(c, "raw-write", !d.failed, d.why);

    if (etude_step(c, "judge") != ETUDE_OK) {
        free(text);
        return 1;
    }
    if (text != NULL) {
        sum_line(text, 1, one);
        sum_line(text, 2, two);
        verify_line(text, 1, back_one);
        verify_line(text, 2, back_two);
    }
    snprintf(detail, sizeof(detail), "sha256sum read %s and reflash read %s", one, back_one);
    inst_assert(c, "digest", one[0] != '\0' && strcmp(one, back_one) == 0, detail);
    snprintf(detail, sizeof(detail),
             "the readback at 33554432 gave %s and the bytes written there hash to %s", back_two,
             two);
    inst_assert(c, "offset",
                two[0] != '\0' && strcmp(two, back_two) == 0 && strcmp(back_two, back_one) != 0,
                detail);
    steps = text != NULL ? progress_lines(text, "33554432") : 0;
    snprintf(detail, sizeof(detail), "the progress lines were not printed (%d of them)", steps);
    inst_assert(c, "progress", steps >= 2, detail);
    inst_assert(c, "quiet", text != NULL && strstr(text, "of 2097152 bytes, ") == NULL,
                text == NULL ? "the console cannot be read"
                             : "a write that was not asked to report its progress reported it");
    free(text);
    inst_finish();
    return 0;
}
