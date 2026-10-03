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
 *   FPU_STATE_SIZE         the bytes of a floating-point save area
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

#include "../lib/kdef.h"
#include "../lib/kint.h"

/*
 * The processor's primitives, each the architecture's to define
 * (arch/x86/cpu.c for i686 and x86-64). They are functions rather than
 * macros so that the shared code holds no instruction; a call costs less
 * than any of the instructions it stands for, but the idle and fence ones.
 */

/** Allow interrupts on this processor. */
void arch_irq_enable(void);

/** Hold off interrupts on this processor. */
void arch_irq_disable(void);

/** Wait for the next interrupt, with interrupts as they stand. */
void arch_halt(void);

/** Allow interrupts and wait for one, as one step, so that an interrupt
 * that arrives between the two cannot be slept through; interrupts are on
 * when this returns. */
void arch_idle(void);

/** As `arch_idle`, then hold interrupts off again before returning. */
void arch_idle_masked(void);

/** Hold interrupts off and stop this processor for good. */
void arch_stop(void) K_NORETURN;

/** Tell the processor the caller is spinning on a value another agent will
 * change. */
void arch_relax(void);

/**
 * Leave the current stack for the one ending at `sp`, allow interrupts, and
 * run `fn`, which does not return: how a process that ends hands its
 * processor back to the debug shell on the boot stack.
 */
void arch_run_on_stack(uintptr_t sp, void (*fn)(void)) K_NORETURN;

/**
 * Enter user mode at `pc` with the stack pointer `sp`, interrupts on and the
 * general registers cleared: how the first program, and the debug shell's
 * own programs, start. Does not return.
 */
void arch_enter_user(uintptr_t pc, uintptr_t sp) K_NORETURN;

/** Make `base` the thread pointer of the thread about to run in user mode,
 * where its TLS block starts (on i686, the base of the GDT's TLS entry). */
void arch_tls_set(uintptr_t base);

/** Read clone's CLONE_SETTLS argument `arg` into the thread pointer it
 * gives the new thread: 0, or -EFAULT or -EINVAL. On i686 the argument
 * points at a struct user_desc; elsewhere it is the pointer itself. */
long arch_clone_tls(unsigned long arg, uintptr_t *base_out);

/* The debug shell's ring-3 exercise: a payload that writes this message
 * through a system call, then spins, and the message, both in pages the
 * exercise makes user-accessible. */
extern const char arch_user_test_msg[];
void arch_user_test_entry(void);

/** The physical address of the page table root the processor translates
 * through now. */
uintptr_t arch_space_root(void);

/** Translate through the page table root at physical address `root`, which
 * drops every translation of user addresses the processor had cached. */
void arch_space_load(uintptr_t root);

/** Drop the processor's cached translation of the page at `va`. */
void arch_tlb_flush_page(uintptr_t va);

/** The address whose access raised the page fault being handled. */
uintptr_t arch_fault_address(void);

/** A counter of processor cycles, for entropy and for calibration; 0 on a
 * processor without one. */
uint64_t arch_cycles(void);

/** A value from the processor's entropy source into `out`: `seed` asks for
 * the conditioned seed source (RDSEED), otherwise the generator (RDRAND).
 * Returns 1, or 0 when the source has none to give. */
int arch_random(uint32_t *out, int seed);

/** Copy `words` machine words from `src` to `dst`, which do not overlap, by
 * the processor's string instructions; defined where cpu.h defines
 * ARCH_STRING_WORDS, for k_memcpy. */
void arch_copy_words(void *dst, const void *src, size_t words);

/** Store `fill` in `words` machine words from `dst`, for k_memset. */
void arch_fill_words(void *dst, unsigned long fill, size_t words);

/** Write back the cache lines of the `len` bytes at `p` to memory, for a
 * device that reads them without snooping the cache, and order the writes
 * before what follows. */
void arch_dma_flush(const void *p, size_t len);

/** Order the processor's earlier memory accesses before its later ones, as
 * a device reading memory sees them. */
void arch_dma_fence(void);

#endif /* _ARCH_H_ */
