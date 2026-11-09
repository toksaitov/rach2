/* get_mempolicy(2): the memory policy of a thread or an address, on a
 * machine of one memory node. */
#ifndef _MEMPOLICY_H_
#define _MEMPOLICY_H_

#include "../irq.h"

/** get_mempolicy(mode, nodemask, maxnode, addr, flags); 0 or -errno. */
long sys_get_mempolicy(isr_params_t *params);

#endif /* _MEMPOLICY_H_ */
