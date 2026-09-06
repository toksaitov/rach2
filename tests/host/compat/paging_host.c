/*
 * The paging calls a driver makes, on the host: a buffer's physical address
 * is its own, since the host test's buffers are the process's memory and
 * the address only goes into a table nothing reads.
 */

#include "mm/paging.h"

uintptr_t paging_virt_to_phys(uintptr_t va)
{
    return va;
}
