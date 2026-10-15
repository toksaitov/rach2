/*
 * diskcmd.c: the disk command policy, chosen when the kernel is built.
 *
 * The ATA driver judges every command in `ata_command`, the AHCI driver in
 * `ahci_build_command`, and the NVMe driver its I/O commands in `nvme_build`
 * and Identify in `nvme_identify`; each is the one place such a command
 * leaves its driver, so a command not allowed there is not issued at all.
 * The NVMe admin commands that set up its queues touch no medium and are not
 * judged.
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
    case DISKCMD_READ_DMA:
    case DISKCMD_READ_DMA_EXT:
    case DISKCMD_IDENTIFY:
        return 1;
    default:
        return 0;
    }
}

int diskcmd_allowed(uint8_t command)
{
    return diskcmd_allowed_under(diskcmd_policy(), command);
}
