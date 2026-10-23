/*
 * installer_repair_convert_stopped.c: the scenario of
 * machine.installer-repair.convert-stopped: the conversion of an MBR disk
 * stopped after its boot partition's files were kept on the recovery
 * partition, finished by the update run again.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lib_installer_repair.h"
#include "lib_installer_repair_disk.h"

/**
 * Plant the stopped conversion on the host, run the update, and judge the
 * volume it made and the copy it removed on the host.
 *
 * The stopped state is made on the host, as the conversion leaves it between
 * keeping the files and making the new file system: the files and the boot
 * sector on the recovery partition, marked whole, and the boot partition's
 * first sectors zeroed. It does not depend on when anything ran.
 */
int etude_scenario(struct etude_case *c)
{
    const char *p;
    char disk[4096] = "", line[64];
    uint64_t esp = 0, at = 0, len = 0;
    char *out = NULL, *info = NULL, *cfg = NULL;
    struct drive d;
    int have, planted, kept, ok;

    if (etude_step(c, "plant") != ETUDE_OK)
        return 1;
    p = etude_private_copy(c, "mbr");
    if (p != NULL)
        snprintf(disk, sizeof(disk), "%s", p);
    have = p != NULL && repair_entry(c, "mkmbr", disk, 1, &esp, &len) == 0 &&
           repair_entry(c, "mkmbr", disk, 2, &at, &len) == 0;
    planted = 0;
    if (have) {
        struct repair_disk_request request = {"keep", disk, NULL, NULL, at / 512, esp / 512};
        planted = repair_disk(c, &request, "keep", &out) == 0;
    }
    if (drive_halted()) {
        free(out);
        return 1;
    }
    repair_assert(c, "planted", planted,
                  out != NULL && out[0] != '\0' ? out : "the disk's table could not be read");
    free(out);
    out = NULL;

    if (etude_step(c, "resume") != ETUDE_OK)
        return 1;
    repair_recovery(&d, c, "recovery");
    drive_send(&d, "rach2-install --update /m/%s /dev/disk0", REPAIR_ROOT_NAME);
    drive_expect(&d, "the conversion of /dev/disk0p1 was stopped part way; its files are on "
                     "the recovery partition");
    drive_expect(&d, "making /dev/disk0p1's file system again");
    drive_expect(&d, "converted: /dev/disk0 keeps its MBR and boots by UEFI as well");
    drive_expect(&d, "updated:");
    drive_send(&d, "%s", "umount /m; echo RESUME\"\"DONE");
    drive_expect(&d, "RESUMEDONE");
    drive_stop(&d);
    if (drive_halted())
        return 1;
    repair_assert(c, "resumed", !d.failed, d.why);

    if (etude_step(c, "inspect") != ETUDE_OK)
        return 1;
    kept = 1;
    if (have) {
        kept = repair_volume(c, "mdir", disk, at, (const char *const[]){"::/rach2-boot-keep", NULL},
                             "mdir.keep", NULL) == 0;
        repair_volume(c, "minfo", disk, esp, (const char *const[]){"::", NULL}, "minfo", &info);
        repair_volume(c, "mtype", disk, esp, (const char *const[]){"::/boot/grub/grub.cfg", NULL},
                      "grub.cfg", &cfg);
    }
    if (drive_halted()) {
        free(info);
        free(cfg);
        return 1;
    }
    ok = have && !kept && info != NULL &&
         text_field(info, "cluster size: 1 sectors", 1, line, sizeof(line)) == 0 &&
         text_field(info, "serial number: 1ACEF00D", 1, line, sizeof(line)) == 0 && cfg != NULL &&
         strstr(cfg, "MBRID=1acef00d,3") != NULL;
    repair_assert(c, "resumed-volume", ok,
                  kept ? "the copy on the recovery partition is still there"
                       : "the volume made from the copy is not the converted one, with its "
                         "serial and its menu");
    free(info);
    free(cfg);
    repair_finish(c);
    return 0;
}
