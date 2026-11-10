/* membarrier(2): a memory barrier on every processor that runs a thread of
 * the caller's process, or of any process, which user-space RCU and
 * garbage collectors issue in place of a fence on their fast paths. */
#ifndef _MEMBARRIER_H_
#define _MEMBARRIER_H_

#include "../irq.h"

struct process;

/* What an address space registered for (struct mm's `membarrier`), Linux's
 * MEMBARRIER_STATE_ bits. Each command sets its own bit and its _READY bit
 * at once, since the big lock keeps the processors' threads still while it
 * does; the sync-core and rseq registrations set PRIVATE_EXPEDITED and not
 * its _READY bit, as Linux's do. */
#define MEMBARRIER_STATE_PRIVATE_EXPEDITED_READY           0x01u
#define MEMBARRIER_STATE_PRIVATE_EXPEDITED                 0x02u
#define MEMBARRIER_STATE_GLOBAL_EXPEDITED_READY            0x04u
#define MEMBARRIER_STATE_GLOBAL_EXPEDITED                  0x08u
#define MEMBARRIER_STATE_PRIVATE_EXPEDITED_SYNC_CORE_READY 0x10u
#define MEMBARRIER_STATE_PRIVATE_EXPEDITED_SYNC_CORE       0x20u
#define MEMBARRIER_STATE_PRIVATE_EXPEDITED_RSEQ_READY      0x40u
#define MEMBARRIER_STATE_PRIVATE_EXPEDITED_RSEQ            0x80u

/**
 * membarrier(cmd, flags, cpu_id), as Linux 6.12's on x86: MEMBARRIER_CMD_QUERY
 * returns the mask of the commands, 0x3FF; GLOBAL, GLOBAL_EXPEDITED,
 * PRIVATE_EXPEDITED, its SYNC_CORE and RSEQ forms and their registrations,
 * and GET_REGISTRATIONS, which returns the registration commands the
 * address space has made. Each barrier returns once every processor it
 * names has passed a full memory barrier, a serializing instruction too for
 * SYNC_CORE, and for RSEQ has its thread's critical section aborted. 0, or
 * -EINVAL for an unknown command or flags other than 0, or
 * MEMBARRIER_CMD_FLAG_CPU with the RSEQ command; -EPERM for a private
 * command before its registration.
 */
long sys_membarrier(isr_params_t *params);

/** Serialize this processor when `p`, just switched to, is of an address
 * space registered for MEMBARRIER_CMD_PRIVATE_EXPEDITED_SYNC_CORE, whose
 * barrier reached only the processors its threads ran on. Called by the
 * scheduler with the big lock held. */
void membarrier_switch(const struct process *p);

#endif /* _MEMBARRIER_H_ */
