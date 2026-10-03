#ifndef _ARCH_H_
#define _ARCH_H_

/*
 * The architecture the compiler builds for, and what the shared kernel asks
 * of it.
 *
 * The shared code, everything under sys/ but sys/arch/, includes this file
 * and nothing under an architecture's directory. The compiler's own
 * definition of its target picks the directory, so a source list or an
 * include path cannot name the wrong one. Each directory's `cpu.h` gives
 * the types and constants below; the functions are each architecture's to
 * define, in its directory.
 *
 * `cpu.h` defines:
 *
 *   isr_params_t           the registers an entry to the kernel saved, which
 *                          a handler may change before the return
 *   phys_addr_t            a physical address, as wide as the processor's
 *   SC_NR(p)               the system call number in a saved frame
 *   SC_ARG(p, n)           the call's argument n, 0 to 5, as unsigned long
 *   SC_RET(p)              the register the call's result goes in (an lvalue)
 *   FRAME_PC(p), FRAME_SP(p)  the interrupted instruction and stack pointers
 *   FRAME_USER(p)          whether the entry came from user mode
 *   PAGE_SIZE, PAGE_SHIFT  the page the kernel maps
 *   USER_ADDR_MIN, USER_ADDR_MAX  the range a program may map
 *   ELF_CLASS, ELF_MACHINE the programs the kernel loads
 *   AUDIT_ARCH             the architecture seccomp's filters see
 *   ARCH_NAME              what uname(2) reports as the machine
 *   IRQ_VECTOR(line)       the vector a platform interrupt line arrives on
 *   ISR_DEVICE_FIRST, ISR_DEVICE_LAST  the vectors a device's interrupts take
 */
#if defined(__x86_64__)
#include "x86_64/cpu.h"
#elif defined(__i386__)
#include "i686/cpu.h"
#elif defined(__aarch64__)
#include "arm64/cpu.h"
#elif defined(__arm__)
#include "arm/cpu.h"
#else
#error "arch.h: no architecture directory for this compiler's target"
#endif

#endif /* _ARCH_H_ */
