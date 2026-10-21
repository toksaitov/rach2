/*
 * bootorder_scenario.c: the scenario of machine.bootorder (tests/cases/
 * machine.bootorder.json): a GRUB disk on the second AHCI port, behind a
 * blank disk on the first, booted by SeaBIOS.
 *
 * The kernel numbers its disks in the bus's order, so the GRUB disk is disk
 * 1, and no stage of the boot may assume the first disk: the core image
 * finds the boot partition by its label, the menu finds the kernel's
 * partition by its serial, and the kernel finds the root and the boot
 * partition by the disk's MBR id, which the menu gives it. SeaBIOS boots one
 * hard disk, the one whose bootindex is 0, as its first; the order that
 * changes here is the bus's.
 */
#include <stdio.h>
#include <string.h>

#include "lifecycle.h"

/* The root the menu names, as the kernel must find it. */
#define ROOT "MBRID=1acef00d,3: /dev/disk1p3, ext2"

/**
 * Boot the machine through SeaBIOS and GRUB, report the kernel's records of
 * its two disks and of its root, wait for the login prompt, and end the
 * machine with QMP's quit.
 */
static void behind(struct lc *l)
{
    struct etude_case_machine *m;
    char line[200], detail[300];
    uint64_t at = 0;

    if (!lc_step(l, "boot") || (m = lc_boot(l, "behind")) == NULL ||
        !lc_wait(l, m, "] root: ", &at, NULL) || !lc_wait(l, m, "\n", &at, NULL))
        return;
    if (!lc_text(l, "behind.1.console", "] root: ", 0, line, sizeof(line))) {
        lc_halt(l, ETUDE_FAILED, "the root's record in the console's file");
        return;
    }
    snprintf(detail, sizeof(detail), "the kernel says root: %.180s", line);
    lc_assert(l, "root-disk1", strcmp(line, ROOT) == 0, detail);
    lc_find(l, m, "blank-disk0", "disk: disk0: ahci 0: 16384 sectors", 0, 1);
    lc_find(l, m, "grub-disk1", "disk: disk1: ahci 1: ", 0, 1);
    if (strcmp(line, ROOT) != 0)
        lc_assert(l, "login", 0, "not waited for: the root is not the GRUB disk's third partition");
    else
        lc_wait(l, m, "login:", &at, "login");
    lc_quit(l, m, NULL);
}

int etude_scenario(struct etude_case *c)
{
    static const char *const ids[] = {"root-disk1", "blank-disk0", "grub-disk1", "login", NULL};
    struct lc l;

    if (strcmp(etude_case_id(c), "machine.bootorder.behind") != 0)
        return 1;
    lc_begin(&l, c, ids);
    behind(&l);
    return lc_finish(&l);
}
