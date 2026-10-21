/*
 * installer_repair_cut.c: the scenario of machine.installer-repair.cut: a
 * write of the disk image that stops after the progress line for its first
 * sixteen mebibytes leaves exactly those sixteen over the disk that was there.
 */
#include <stdio.h>
#include <string.h>

#include "lib_installer_repair.h"

/* The disk the write is stopped on, and the part of it the write was asked
 * for. */
#define CUT_DISK    (256 * REPAIR_MIB)
#define CUT_ASKED   (96 * REPAIR_MIB)
#define CUT_WRITTEN (16 * REPAIR_MIB)

/**
 * Fill the disk with the pattern, start a write of ninety-six mebibytes of
 * the image that is given only sixteen and then a FIFO nothing writes, end
 * the machine once the write has said it wrote the sixteen, and judge the
 * ninety-six mebibytes on the host.
 *
 * The write cannot pass its sixteenth mebibyte, whatever the speed of the
 * machine, and the machine is ended only after the write has said so, so a
 * fast and a slow machine leave the same disk: what a person who pulled the
 * plug during an install comes back to.
 */
int etude_scenario(struct etude_case *c)
{
    const char *image = etude_artifact(c, "disk"), *p;
    char cut[4096] = "", was[65] = "", whole[65] = "", part[65] = "", started[65] = "";
    char why[300] = "";
    struct drive d;
    int filled, wrote;

    if (etude_step(c, "fill") != ETUDE_OK)
        return 1;
    p = etude_private_copy(c, "cut");
    if (p != NULL)
        snprintf(cut, sizeof(cut), "%s", p);
    filled = p != NULL && fill_pattern(cut, CUT_DISK) == 0 &&
             file_digest(cut, 0, CUT_ASKED, was) == 0 &&
             file_digest(image, 0, CUT_ASKED, whole) == 0 &&
             repair_digest_join(image, 0, CUT_WRITTEN, cut, CUT_WRITTEN, CUT_ASKED - CUT_WRITTEN,
                                part) == 0;

    if (etude_step(c, "write") != ETUDE_OK)
        return 1;
    memset(&d, 0, sizeof(d));
    if (filled) {
        repair_recovery(&d, c, "recovery");
        drive_send(&d,
                   "mkfifo /tmp/hold && { dd if=/m/%s bs=1048576 count=16 2>/dev/null; "
                   "cat /tmp/hold; } | reflash -p /dev/disk0 0 100663296",
                   REPAIR_DISK_NAME);
        drive_expect(&d, "reflash: 16777216 of ");
        drive_stop(&d);
        if (drive_halted())
            return 1;
        wrote = !d.failed;
        snprintf(why, sizeof(why), "%s", d.why);
    } else {
        wrote = 0;
        snprintf(why, sizeof(why), "%s",
                 "the disk could not be filled with the pattern, or the image read");
    }

    if (etude_step(c, "judge") != ETUDE_OK)
        return 1;
    repair_assert(c, "distinct", filled && strcmp(part, was) != 0 && strcmp(part, whole) != 0,
                  filled ? "the image's first sixteen mebibytes leave the disk as it was or as "
                           "the image"
                         : why);
    repair_assert(c, "progress", wrote, why);
    if (!filled || file_digest(cut, 0, CUT_ASKED, started) != 0)
        repair_assert(c, "partial", 0, filled ? "the disk cannot be read" : why);
    else if (strcmp(started, was) == 0)
        repair_assert(c, "partial", 0, "the write that was stopped had not begun");
    else if (strcmp(started, whole) == 0)
        repair_assert(c, "partial", 0, "the write that was stopped had finished");
    else
        repair_assert(c, "partial", strcmp(started, part) == 0,
                      "the write that was stopped left other than the sixteen mebibytes it "
                      "reported");
    repair_finish(c);
    return 0;
}
