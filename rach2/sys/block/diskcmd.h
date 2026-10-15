/*
 * diskcmd.h: the disk command policy, the ATA commands a kernel may issue to
 * a disk, decided when the kernel is built.
 *
 * A kernel built with `-DRACH2_DISK_READONLY`, the metal kernel, is for a
 * machine whose disk has another system's data and must not be written. The
 * rule is enforced where commands leave the ATA, AHCI and NVMe drivers, and
 * it is decided at build time, not at boot, so a boot line edited at the GRUB
 * prompt cannot undo it. The USB mass-storage driver and the loop devices are
 * not judged by it.
 *
 * It is a list of what is allowed and not of what is forbidden. A list of
 * what is forbidden has to be kept complete, and an entry missing from it
 * costs a written disk; an entry missing from a list of what is allowed costs
 * a failed read at worst, which is a boot that stops and says so.
 *
 * `diskcmd_policy_banner` is a string the built kernel embeds so the policy an
 * artifact was built with can be read off the artifact itself. The absence of
 * a write path cannot be checked for: the commands are numbers and the
 * functions that issue them are static, so there is nothing to look for. A
 * string that says which of the two builds this is can be looked for, in both
 * directions, and `tools/inspect/check_diskpolicy.sh` does.
 */
#ifndef _DISKCMD_H_
#define _DISKCMD_H_

#include "../lib/kint.h"

/* The two builds. `DISKCMD_READ_ONLY` is what -DRACH2_DISK_READONLY selects. */
#define DISKCMD_READ_WRITE 0
#define DISKCMD_READ_ONLY  1

/* ATA commands by their numbers on the wire: the reads and IDENTIFY, which
   the read-only policy allows, and writes and flushes, which it does not. */
#define DISKCMD_READ_SECTORS     0x20u
#define DISKCMD_READ_SECTORS_EXT 0x24u
#define DISKCMD_READ_DMA         0xC8u
#define DISKCMD_READ_DMA_EXT     0x25u
#define DISKCMD_WRITE_SECTORS    0x30u
#define DISKCMD_WRITE_DMA_EXT    0x35u
#define DISKCMD_FLUSH_CACHE      0xE7u
#define DISKCMD_FLUSH_CACHE_EXT  0xEAu
#define DISKCMD_IDENTIFY         0xECu

/** The policy this kernel was built with: `DISKCMD_READ_WRITE` or
 * `DISKCMD_READ_ONLY`. */
int diskcmd_policy(void);

/**
 * Whether the ATA command `command` may be issued to a disk.
 *
 * Under the read-write policy every command is allowed. Under the read-only
 * policy only the four reads and IDENTIFY named above are; every other
 * command number, named here or not, is not, since what an unknown command
 * would do to the disk is not known. It reads no state and may be called
 * from any context.
 *
 * @return 1 when the command may go to the device, 0 when it may not.
 */
int diskcmd_allowed(uint8_t command);

/**
 * As `diskcmd_allowed`, under `policy` in place of the policy built in; a
 * value other than `DISKCMD_READ_WRITE` is judged as read-only.
 *
 * The table is here so that one build can be tested under both policies: a
 * host test that reached only the policy it was compiled with would leave the
 * other untested.
 */
int diskcmd_allowed_under(int policy, uint8_t command);

/** The string the kernel embeds for its policy: `rach2: disk policy:
 * read-only` or `rach2: disk policy: read-write`. */
extern const char diskcmd_policy_banner[];

#endif /* _DISKCMD_H_ */
