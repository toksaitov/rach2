/*
 * ia32_time.c: the i386 forms of the time calls, with 32-bit times, for the
 * compat table (ia32_systab.c).
 */

#include "ia32.h"

/* Each answers ENOSYS until its translation is written (ia32.h). */
IA32_STUB(compat_sys_time32)
IA32_STUB(compat_sys_stime32)
IA32_STUB(compat_sys_utime32)
IA32_STUB(compat_sys_gettimeofday)
IA32_STUB(compat_sys_settimeofday)
IA32_STUB(compat_sys_setitimer)
IA32_STUB(compat_sys_getitimer)
IA32_STUB(compat_sys_adjtimex_time32)
IA32_STUB(compat_sys_nanosleep_time32)
IA32_STUB(compat_sys_timer_create)
IA32_STUB(compat_sys_timer_settime32)
IA32_STUB(compat_sys_timer_gettime32)
IA32_STUB(compat_sys_clock_settime32)
IA32_STUB(compat_sys_clock_gettime32)
IA32_STUB(compat_sys_clock_getres_time32)
IA32_STUB(compat_sys_clock_nanosleep_time32)
IA32_STUB(compat_sys_utimes_time32)
IA32_STUB(compat_sys_futimesat_time32)
IA32_STUB(compat_sys_utimensat_time32)
IA32_STUB(compat_sys_timerfd_settime32)
IA32_STUB(compat_sys_timerfd_gettime32)
IA32_STUB(compat_sys_clock_adjtime32)
