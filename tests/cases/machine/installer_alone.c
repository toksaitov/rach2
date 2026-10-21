/*
 * installer_alone.c: the scenario of machine.installer.alone: the test stick
 * boots alone, with no disk to write, and the installer must say so and
 * stop, rather than offer the stick it runs from.
 */
#include "lib_installer.h"

/* The exchange at the stick's console. */
static const struct inst_line alone[] = {
    {0, "recovery image"},
    {0, "# "},
    {1, "rach2-install"},
    {0, "is the stick this is running from"},
    {0, "no disk of this machine can take the image"},
    {0, "# "},
    {1, "echo NODISK\"\"DONE"},
    {0, "NODISKDONE"},
    {0, NULL},
};

/** Boot the stick alone and run the installer at its console. */
int etude_scenario(struct etude_case *c)
{
    struct drive d;

    if (etude_step(c, "install") != ETUDE_OK)
        return 1;
    drive_boot(&d, c, "stick", NULL);
    inst_exchange(&d, alone);
    drive_stop(&d);
    if (drive_halted())
        return 1;
    inst_assert(c, "told", !d.failed, d.why);
    inst_finish();
    return 0;
}
