/*
 * installer_mounted.c: the scenario of machine.installer.mounted: a disk
 * with something mounted from it is not offered. The test stick boots
 * beside a copy of the test disk, whose boot partition is mounted before
 * the installer runs, so that no disk of the machine can take the image.
 */
#include "lib_installer.h"

/* The exchange at the stick's console. */
static const struct inst_line mounted[] = {
    {0, "recovery image"},
    {0, "# "},
    {1, "mkdir -p /mnt/held && mount -t vfat /dev/disk0p1 /mnt/held && echo HELD"},
    {0, "HELD"},
    {1, "rach2-install"},
    {0, "has something mounted from it"},
    {0, "no disk of this machine can take the image"},
    {1, "umount /mnt/held; echo MOUNTED\"\"DONE"},
    {0, "MOUNTEDDONE"},
    {0, NULL},
};

/** Boot the stick beside the disk, mount the disk and run the installer. */
int etude_scenario(struct etude_case *c)
{
    struct drive d;

    if (etude_step(c, "install") != ETUDE_OK)
        return 1;
    drive_boot(&d, c, "stick", NULL);
    inst_exchange(&d, mounted);
    drive_stop(&d);
    if (drive_halted())
        return 1;
    inst_assert(c, "refused", !d.failed, d.why);
    inst_finish();
    return 0;
}
