/*
 * grow_machine_mbr.c: the scenario of machine.grow.machine-mbr: the grow of
 * the installed machine, tools/dev/machine_grow.sh, rehearsed from the
 * recovery stick on a disk of the machine's layout on an MBR, as
 * grow_machine in lib_grow.c runs it.
 */
#include "lib_grow.h"

/** Rehearse the grow on the case's target disk and store. */
int etude_scenario(struct etude_case *c)
{
    return grow_machine(c);
}
