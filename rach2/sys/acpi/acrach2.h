/*
 * acrach2.h: ACPICA's platform header for the Rach2 kernel, which ACPICA's
 * acenv.h includes when the build defines __RACH2__ (vendor/acpica's patch).
 */
#ifndef __ACRACH2_H__
#define __ACRACH2_H__

/* GCC's acgcc.h has included <stdarg.h> by now; TinyCC has no such header
 * of ACPICA's, and its own <stdarg.h> is the compiler's. */
#ifndef va_arg
#include <stdarg.h>
#endif

/*
 * The widths. On x86-64 a 64-bit integer is a long, as `__SIZE_TYPE__` is,
 * so ACPI_SIZE and the size of the C library's declarations below agree. On
 * i686 the 64-bit division is ACPICA's own, through `divl`, since the kernel
 * links no libgcc; the shifts GCC emits inline, and TinyCC's helpers come
 * from libtcc1.a.
 */
#if defined(__x86_64__)
#define ACPI_MACHINE_WIDTH        64
#define COMPILER_DEPENDENT_INT64  long
#define COMPILER_DEPENDENT_UINT64 unsigned long
#define ACPI_USE_NATIVE_DIVIDE
#else
#define ACPI_MACHINE_WIDTH        32
#define COMPILER_DEPENDENT_INT64  long long
#define COMPILER_DEPENDENT_UINT64 unsigned long long
#define ACPI_32BIT_PHYSICAL_ADDRESS
/* The 64-bit dividend n_hi:n_lo over d32, with n_hi below d32, which the
 * callers in utmath.c see to, so the quotient fits and `divl` cannot fault. */
#define ACPI_DIV_64_BY_32(n_hi, n_lo, d32, q32, r32) \
    __asm__("divl %2" : "=a"(q32), "=d"(r32) : "r"(d32), "0"(n_lo), "1"(n_hi))
#define ACPI_SHIFT_RIGHT_64(n_hi, n_lo) \
    __asm__("shrl $1, %2\n\trcrl $1, %3" : "=r"(n_hi), "=r"(n_lo) : "0"(n_hi), "1"(n_lo))
#endif

#ifndef ACPI_USE_NATIVE_MATH64
#define ACPI_USE_NATIVE_MATH64
#endif

/*
 * The C library is the kernel's (clib.h), and ACPICA's utclib.c compiles to
 * nothing; macros that bracket statements are wrapped in do ... while (0);
 * and the caches of objects are ACPICA's own lists over AcpiOsAllocate.
 */
#define ACPI_USE_SYSTEM_CLIBRARY
#define ACPI_USE_DO_WHILE_0
#define ACPI_USE_LOCAL_CACHE

#include "clib.h"

/* The firmware's Global Lock, in the FACS, taken and given back as the ACPI
 * specification's sample code does, by compare-and-exchange (osl.c). */
int acpi_os_acquire_global_lock(volatile unsigned int *lock);
int acpi_os_release_global_lock(volatile unsigned int *lock);

#define ACPI_ACQUIRE_GLOBAL_LOCK(facs, acquired) \
    ((acquired) = (BOOLEAN)acpi_os_acquire_global_lock(&(facs)->GlobalLock))
#define ACPI_RELEASE_GLOBAL_LOCK(facs, pending) \
    ((pending) = (BOOLEAN)acpi_os_release_global_lock(&(facs)->GlobalLock))

#endif /* __ACRACH2_H__ */
