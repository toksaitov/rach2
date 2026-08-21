/*
 * recovery.c: the string a built kernel embeds to say whether it may write its
 * own disk, and the one thing that string implies at boot. The decision about
 * writing itself is where the handler is, in sys/fs/devfs.c.
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
