/*
 * recovery.c: the string a built kernel has to say whether it may write its
 * own disks, and whether its boot publishes them. The write handler itself is
 * chosen in fs/devfs.c.
 */

#include "recovery.h"

#ifdef RACH2_RECOVERY
const char recovery_policy_banner[] = "rach2: disk authority: recovery";
#else
const char recovery_policy_banner[] = "rach2: disk authority: ordinary";
#endif

int recovery_publishes_disk(void)
{
#ifdef RACH2_RECOVERY
    return 1;
#else
    return 0;
#endif
}
