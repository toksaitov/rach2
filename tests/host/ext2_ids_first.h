/* The first user namespace and its capabilities, for the ext2 tests: the
 * mount options' ids and the reserved blocks' CAP_SYS_RESOURCE. */
#ifndef EXT2_IDS_FIRST_H
#define EXT2_IDS_FIRST_H

#include "ids_first.h"

/** A capability is held with effective uid 0 unless `cap_lost` has it. */
int cap_holds(const process_t *p, uint32_t cap)
{
    return p->euid == 0 && cap < 32 && !(p->cap_lost[0] & (1u << cap));
}

#endif
