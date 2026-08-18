/*
 * recovery.c: the string a built kernel carries to say whether it may write its
 * own disk. The decision itself is where the handler is, in sys/fs/devfs.c.
 */

#include "recovery.h"

#ifdef RACH2_RECOVERY
const char recovery_policy_banner[] = "rach2: disk authority: recovery";
#else
const char recovery_policy_banner[] = "rach2: disk authority: ordinary";
#endif
