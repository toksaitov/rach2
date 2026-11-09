/* kcmp(2): whether two processes share a kernel object, which Mesa asks
 * of two descriptors to learn whether they are one DRM file. */
#ifndef _KCMP_H_
#define _KCMP_H_

#include "../irq.h"

/** kcmp(pid1, pid2, type, idx1, idx2); 0 when the two are the same,
 * else their order, or -errno. */
long sys_kcmp(isr_params_t *params);

#endif /* _KCMP_H_ */
