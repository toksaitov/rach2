#ifndef _X86_LEAVE_H_
#define _X86_LEAVE_H_

/* The work of a return to user mode, which `isr_leave` (isr.c) calls and
 * leave.c defines. */

#include "../../proc/process.h"

void isr_leave_work(isr_params_t *params, process_t *p, int locked);

#endif /* _X86_LEAVE_H_ */
