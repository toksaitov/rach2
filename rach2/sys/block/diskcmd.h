#ifndef _DISKCMD_H_
#define _DISKCMD_H_

#include "../lib/kint.h"

/*
 * diskcmd: which ATA commands this kernel is allowed to issue.
 *
 * The machine this kernel is being carried to has somebody's Windows
 * installation on its SSD, and until its owner has approved a carved partition
 * after a full backup, the rule is that nothing this kernel does may write that
 * disk. A rule of that weight cannot rest on remembering not to call the write
 * path, so it is enforced where commands leave the drivers and it is decided at
 * build time, not at boot: a kernel built read-only has no way to be
 * asked nicely, and a command line edited at the GRUB prompt cannot undo it.
 *
 * It is an allowlist and not a list of forbidden commands. A list of what is
 * refused is a list somebody has to keep complete, and the cost of missing an
 * entry is the disk this exists to protect; a list of what is permitted costs
 * a refused read at worst, which is a boot that stops and says so.
 *
 * `diskcmd_policy_banner` is a string the built kernel embeds so the policy an
 * artifact was built with can be read off the artifact itself. Checking for the
 * absence of a write path would prove nothing: the commands are numbers and the
 * functions that issue them are static, so there is nothing to look for. A
 * string that says which of the two builds this is can be looked for, in both
 * directions, and `tools/inspect/check_diskpolicy.sh` does.
 */

/* The two builds. `DISKCMD_READ_ONLY` is what -DRACH2_DISK_READONLY selects. */
#define DISKCMD_READ_WRITE 0
#define DISKCMD_READ_ONLY  1

/* ATA commands this kernel knows how to issue, by their numbers on the wire. */
#define DISKCMD_READ_SECTORS     0x20u
#define DISKCMD_READ_SECTORS_EXT 0x24u
#define DISKCMD_READ_DMA_EXT     0x25u
#define DISKCMD_WRITE_SECTORS    0x30u
#define DISKCMD_WRITE_DMA_EXT    0x35u
#define DISKCMD_FLUSH_CACHE      0xE7u
#define DISKCMD_FLUSH_CACHE_EXT  0xEAu
#define DISKCMD_IDENTIFY         0xECu

/** Which policy this kernel was built with. */
int diskcmd_policy(void);

/**
 * Whether `command` may be issued to a disk.
 *
 * Under the read-write policy every command is allowed and this is a formality.
 * Under the read-only policy only the commands that read are allowed, and a
 * command that is refused is refused whatever the caller believed it was doing.
 *
 * @return 1 when the command may go to the device, 0 when it may not.
 */
int diskcmd_allowed(uint8_t command);

/**
 * As `diskcmd_allowed`, for a policy named, not compiled in.
 *
 * The table lives here so that one build can be held to both policies: the
 * kernel picks its policy at compile time, and a host test that could only
 * reach the one it was compiled with would leave the other untested, which is
 * the half that matters.
 */
int diskcmd_allowed_under(int policy, uint8_t command);

/** The string the artifact has, naming the policy it was built with. */
extern const char diskcmd_policy_banner[];

#endif /* _DISKCMD_H_ */
