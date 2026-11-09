/*
 * ia32_ipc.c: the i386 forms of System V IPC and of the POSIX message queues,
 * for the compat table (ia32_systab.c).
 */

#include "ia32.h"

/* Each answers ENOSYS until its translation is written (ia32.h). */
IA32_STUB(compat_sys_ipc)
IA32_STUB(compat_sys_mq_open)
IA32_STUB(compat_sys_mq_timedsend_time32)
IA32_STUB(compat_sys_mq_timedreceive_time32)
IA32_STUB(compat_sys_mq_notify)
IA32_STUB(compat_sys_mq_getsetattr)
IA32_STUB(compat_sys_semctl)
IA32_STUB(compat_sys_shmctl)
IA32_STUB(compat_sys_shmat)
IA32_STUB(compat_sys_msgsnd)
IA32_STUB(compat_sys_msgrcv)
IA32_STUB(compat_sys_msgctl)
