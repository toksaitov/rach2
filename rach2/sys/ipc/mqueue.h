/*
 * mqueue.h: POSIX message queues, as Linux gives them (ipc/Readme.md).
 */

#ifndef _MQUEUE_H_
#define _MQUEUE_H_

#include "../lib/kint.h"

struct isr_params;

/** mq_open, mq_unlink, mq_timedsend, mq_timedreceive, mq_notify and
 * mq_getsetattr, numbers 277 to 282, and the two calls with 64-bit
 * timeouts, 418 and 419. */
int32_t sys_mq_open(struct isr_params *params);
int32_t sys_mq_unlink(struct isr_params *params);
int32_t sys_mq_timedsend(struct isr_params *params);
int32_t sys_mq_timedreceive(struct isr_params *params);
int32_t sys_mq_notify(struct isr_params *params);
int32_t sys_mq_getsetattr(struct isr_params *params);
int32_t sys_mq_timedsend_time64(struct isr_params *params);
int32_t sys_mq_timedreceive_time64(struct isr_params *params);

/** The end of the IPC namespace `ns`: its queues lose their names, and each
 * lives while a descriptor has it. */
struct ipcns;
void mq_ns_gone(const struct ipcns *ns);

#endif /* _MQUEUE_H_ */
