/* rseq(2): restartable sequences, a thread's record of the processor it
 * runs on and the critical section that a preemption aborts. */
#ifndef _RSEQ_H_
#define _RSEQ_H_

#include "../irq.h"

/** rseq(rseq, rseq_len, flags, sig): register or unregister the
 * calling thread's area; 0 or -errno. */
long sys_rseq(isr_params_t *params);

#endif /* _RSEQ_H_ */
