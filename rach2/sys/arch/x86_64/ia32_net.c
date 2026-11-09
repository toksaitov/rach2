/*
 * ia32_net.c: the i386 forms of the socket calls: socketcall, the message
 * headers and the socket options whose layout differs, for the compat table
 * (ia32_systab.c).
 */

#include "ia32.h"

/* Each answers ENOSYS until its translation is written (ia32.h). */
IA32_STUB(compat_sys_socketcall)
IA32_STUB(compat_sys_recvmmsg_time32)
IA32_STUB(compat_sys_sendmmsg)
IA32_STUB(compat_sys_getsockopt)
IA32_STUB(compat_sys_setsockopt)
IA32_STUB(compat_sys_sendmsg)
IA32_STUB(compat_sys_recvfrom)
IA32_STUB(compat_sys_recvmsg)
IA32_STUB(compat_sys_recvmmsg_time64)
