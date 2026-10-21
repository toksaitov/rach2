/*
 * installer_repair_convert.c: the scenario of machine.installer-repair.convert:
 * an MBR disk whose boot partition UEFI firmware cannot read is updated from
 * the stick and converted on the way, then boots by UEFI and by BIOS.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lib_installer_repair.h"

/**
 * Judge the disk's layout before the update, update it twice from the stick
 * in one boot, judge the converted volume on the host, and boot it by UEFI
 * and then by BIOS.
 *
 * The boot by UEFI spends the trial boot the update armed, and nothing
 * commits it, so the boot by BIOS starts the saved default, the recovery
 * system, through the same menu.
 */
int etude_scenario(struct etude_case *c)
{
    const char *p;
    char disk[4096] = "", efi[4096] = "", got[65] = "", want[65] = "", line[80], detail[300];
    uint64_t esp = 0, len = 0, now = 0;
    char *before = NULL, *after = NULL, *cfg = NULL, *text = NULL;
    struct drive d;
    int have, type, eight, ok;

    if (etude_step(c, "layout") != ETUDE_OK)
        return 1;
    p = etude_private_copy(c, "mbr");
    if (p != NULL)
        snprintf(disk, sizeof(disk), "%s", p);
    have = p != NULL && repair_entry(c, "mkmbr", disk, 1, &esp, &len) == 0;
    if (have)
        repair_volume(c, "minfo", disk, esp, (const char *const[]){"::", NULL}, "minfo.before",
                      &before);
    if (drive_halted())
        return 1;
    type = repair_byte(disk, REPAIR_MBR_TYPE1);
    eight =
        before != NULL && text_field(before, "cluster size: 8 sectors", 1, line, sizeof(line)) == 0;
    snprintf(detail, sizeof(detail),
             "the MBR disk is not the layout installed before: entry 1 has type %02x, %s",
             type & 0xff, eight ? "eight sectors to a cluster" : "not eight sectors to a cluster");
    repair_assert(c, "old-layout", have && type == 0x0c && eight, detail);
    free(before);

    if (etude_step(c, "convert") != ETUDE_OK)
        return 1;
    repair_recovery(&d, c, "recovery");
    drive_send(&d, "rach2-install --update /m/%s /dev/disk0", REPAIR_ROOT_NAME);
    drive_expect(&d, "making /dev/disk0p1's file system again");
    drive_expect(&d, "converted: /dev/disk0 keeps its MBR and boots by UEFI as well");
    drive_expect(&d, "what reached the root is the image");
    drive_expect(&d, "armed for one trial boot");
    drive_expect(&d, "updated:");
    drive_send(&d, "rach2-install --update /m/%s /dev/disk0", REPAIR_ROOT_NAME);
    drive_expect(&d, "GRUB's EFI image on /dev/disk0p1 is already this one");
    drive_expect(&d, "updated:");
    drive_send(&d, "%s", "bootctl --target /dev/disk0p1 status; umount /m; echo CONVERT\"\"DONE");
    drive_expect(&d, "CONVERTDONE");
    drive_stop(&d);
    if (drive_halted())
        return 1;
    repair_assert(c, "converted", !d.failed, d.why);

    if (etude_step(c, "inspect") != ETUDE_OK)
        return 1;
    have = have && repair_entry(c, "mkmbr", disk, 1, &now, &len) == 0 &&
           repair_path(c, "converted.efi", efi, sizeof(efi)) == 0;
    if (have) {
        repair_volume(c, "mcopy", disk, esp,
                      (const char *const[]){"-n", "::/EFI/BOOT/BOOTX64.EFI", efi, NULL},
                      "mcopy.efi", NULL);
        repair_volume(c, "minfo", disk, esp, (const char *const[]){"::", NULL}, "minfo.after",
                      &after);
        repair_volume(c, "mtype", disk, esp, (const char *const[]){"::/boot/grub/grub.cfg", NULL},
                      "grub.cfg", &cfg);
    }
    if (drive_halted())
        return 1;
    type = repair_byte(disk, REPAIR_MBR_TYPE1);
    ok = have && type == 0xef && now == esp && after != NULL &&
         text_field(after, "cluster size: 1 sectors", 1, line, sizeof(line)) == 0 &&
         text_field(after, "serial number: 1ACEF00D", 1, line, sizeof(line)) == 0 &&
         file_digest(efi, 0, UINT64_MAX, got) == 0 &&
         file_digest(etude_artifact(c, "bootx64"), 0, UINT64_MAX, want) == 0 &&
         strcmp(got, want) == 0 && cfg != NULL && strstr(cfg, "MBRID=1acef00d,3") != NULL;
    snprintf(detail, sizeof(detail),
             "the converted disk's MBR entry, file system, serial or EFI image: entry 1 has type "
             "%02x at byte %llu, was at %llu; the EFI image %s GRUB's",
             type & 0xff, (unsigned long long)now, (unsigned long long)esp,
             got[0] != '\0' && strcmp(got, want) == 0 ? "is" : "is not");
    repair_assert(c, "converted-volume", ok, detail);
    free(after);
    free(cfg);

    if (etude_step(c, "uefi") != ETUDE_OK)
        return 1;
    drive_boot(&d, c, "uefi", NULL);
    drive_expect(&d, "rach2 login:");
    if (!d.failed)
        text = drive_console(&d);
    drive_stop(&d);
    if (drive_halted()) {
        free(text);
        return 1;
    }
    repair_assert(
        c, "uefi-boots",
        text != NULL &&
            repair_line_has(text,
                            (const char *const[]){"BdsDxe: starting Boot", "HARDDISK", NULL}) &&
            strstr(text, "root: MBRID=1acef00d,3: /dev/disk0p3, ext2") != NULL,
        d.failed ? d.why : "the converted disk did not boot by UEFI from its disk to its root");
    free(text);

    if (etude_step(c, "bios") != ETUDE_OK)
        return 1;
    drive_boot(&d, c, "bios", NULL);
    drive_expect(&d, "recovery image");
    drive_stop(&d);
    if (drive_halted())
        return 1;
    repair_assert(c, "bios-boots", !d.failed, d.why);
    repair_finish(c);
    return 0;
}
