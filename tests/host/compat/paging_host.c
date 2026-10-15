/*
 * The paging call a driver makes, on the host: a buffer's physical address
 * is its virtual one, since the host test's buffers are the process's memory.
 */

#include "mm/paging.h"

/** The identity: a host address stands for itself, so the AHCI host test
 * finds its buffers' own addresses in the tables the driver fills. */
uintptr_t paging_virt_to_phys(uintptr_t va)
{
    return va;
}
