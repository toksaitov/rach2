/*
 * diskcmd.c: the disk command allowlist, decided when the kernel is built.
 *
 * Both drivers call `diskcmd_allowed` at the one place a command leaves them
 * for the device: `ahci_run_command` in ahci.c, and `ata_command` in ata.c. A
 * refusal there is final; there is no path around it and no way to ask again.
 */

#include "diskcmd.h"

#ifdef RACH2_DISK_READONLY
const char diskcmd_policy_banner[] = "rach2: disk policy: read-only";
#else
const char diskcmd_policy_banner[] = "rach2: disk policy: read-write";
#endif

int diskcmd_policy(void)
{
#ifdef RACH2_DISK_READONLY
    return DISKCMD_READ_ONLY;
#else
    return DISKCMD_READ_WRITE;
#endif
}

int diskcmd_allowed_under(int policy, uint8_t command)
{
    if (policy == DISKCMD_READ_WRITE) {
        return 1;
    }
    switch (command) {
    case DISKCMD_READ_SECTORS:
    case DISKCMD_READ_SECTORS_EXT:
    case DISKCMD_READ_DMA_EXT:
    case DISKCMD_IDENTIFY:
        return 1;
    default:
        /* Everything else, named or not. A command this file has never heard
         * of is refused for the same reason a write is: nothing knows what it
         * would do to somebody else's disk. */
        return 0;
    }
}

int diskcmd_allowed(uint8_t command)
{
    return diskcmd_allowed_under(diskcmd_policy(), command);
}
