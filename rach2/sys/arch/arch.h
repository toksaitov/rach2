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
 *   phys_to_virt(p)        the kernel's address of the physical address `p`:
 *                          of any memory where the architecture maps it all,
 *                          of the window below HARD_MEMORY_LIMIT on i686
 *   virt_to_phys(v)        the physical address of `v`, an address
 *                          phys_to_virt gave or one in the kernel's image:
 *                          what a device is given for a buffer of the DMA
 *                          pool (`alloc_kernel_frame`) or a driver's static
 *                          one; a heap address is `paging_virt_to_phys`'s
 *   SC_NR(p)               the system call number in a saved frame
 *   SC_ARG(p, n)           the call's argument n, 0 to 5, as unsigned long
 *   SC_RET(p)              the register the call's result goes in (an lvalue)
 *   FRAME_PC(p), FRAME_SP(p)  the interrupted instruction and stack pointers
 *   FRAME_USER(p)          whether the entry came from user mode
 *   SYSCALL_INSN_LEN       the bytes of the instruction a system call is
 *                          made by, which a restart rewinds over
 *   PAGE_SIZE, PAGE_SHIFT  the page the kernel maps
 *   FPU_STATE_SIZE         the bytes of a floating-point save area
 *   USER_ADDR_MIN, USER_ADDR_MAX  the range a program may map
 *   ELF_CLASS, ELF_MACHINE the programs the kernel loads
 *   ARCH_GREGS             the words of Linux's `user_regs_struct`
 *   SYSCALL_TABLE_SIZE     one more than the highest system call number
 *   SC_CLONE_TLS, SC_CLONE_CTID  clone's arguments, whose order differs
 *   AUDIT_ARCH             the architecture seccomp's filters see
 *   ARCH_NAME              what uname(2) reports as the machine
 *   pte_t, PTE_*, MMU_TABLE_ENTRIES  a page table entry, its bits, and the
 *                          entries of a leaf table (mm/paging.h);
 *                          PTE_NOEXEC forbids instruction fetches from a
 *                          user page, or is 0 where the processor cannot
 *   HARD_MEMORY_LIMIT, KVA_*  the kernel's own layout
 *   PAGE_FAULT_VECTOR, FAULT_WRITE(p), FAULT_USER(p), FAULT_EXEC(p)  the
 *                          page fault: a write, from user mode, an
 *                          instruction fetch
 *   arch_cpu_id()          the running processor's index in the
 *                          per-processor table (proc/percpu.h)
 *   IRQ_VECTOR(line)       the vector a platform interrupt line arrives on
 *   ISR_DEVICE_FIRST, ISR_DEVICE_LAST  the vectors a device's interrupts take
 */
#if defined(RACH2_HOSTTEST)
/* A host test checks the i686 kernel's logic natively, whatever the host. */
#include "i686/cpu.h"
#elif defined(__x86_64__)
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

struct process;

/** Make the saved frame `p` start a new program image: at `entry`, with the
 * stack pointer `sp`, the general registers cleared and the user segments
 * loaded where the architecture has them. */
void arch_exec_frame(isr_params_t *p, uintptr_t entry, uintptr_t sp);

/** Lay out a new process's kernel stack as the context switch expects to
 * find it, so that its first switch returns into `entry`. */
void arch_context_init(struct process *proc, void (*entry)(void));

/** Lay out a forked `child`'s kernel stack for its first switch, from the
 * parent's saved frame `params`, with 0 for fork's result and `newsp`, when
 * not 0, for the user stack pointer. */
void arch_fork_frame(struct process *child, const isr_params_t *params, uintptr_t newsp);

/** Nonzero when the process record's layout is not what the context switch
 * reads at fixed offsets. */
int arch_context_check(void);

/* A system call's handler: given the saved registers, from which it reads
 * its arguments, it returns the call's result, a value or a negative errno,
 * as wide as a register. */
typedef long (*syscall_fn_t)(isr_params_t *params);

/** Fill `table`, SYSCALL_TABLE_SIZE entries, with the handler of each system
 * call number the architecture has, and route its system call entry to
 * `dispatch`, which runs the handler the number in the saved registers
 * selects. */
void arch_syscall_init(syscall_fn_t *table, void (*dispatch)(isr_params_t *params));

/*
 * Tracing and core files. The shared ptrace(2) answers the requests every
 * architecture has; the registers, the floating-point state and the
 * hardware breakpoints are the architecture's.
 */

/** Fill `regs` with the ARCH_GREGS user registers of `t`, a thread stopped
 * for its tracer or dumping core, in the order of Linux's
 * `user_regs_struct`. */
void arch_regs_get(const struct process *t, unsigned long *regs);

/** Turn single-stepping of the stopped thread `t` on or off. */
void arch_ptrace_step(struct process *t, int on);

/** The ptrace(2) requests of this architecture for `t`, the thread `pid`,
 * stopped for the caller: the register sets, the floating-point state, the
 * words of `struct user` and the thread pointer. -EIO for a request it does
 * not have. */
long arch_ptrace(struct process *t, uint32_t pid, long request, unsigned long addr,
                 unsigned long data);

/** At a switch from `old` to `next`: load the hardware breakpoints `next`
 * has, or clear those `old` left. */
void arch_debug_switch(const struct process *old, const struct process *next);

/** The bytes of the floating-point notes of one thread in a core file. */
uint32_t arch_core_fp_size(void);

/** Put the floating-point notes of the thread `t` at `*p`, with
 * `core_note_put`, and step past them. */
void arch_core_fp_notes(uint8_t **p, const struct process *t);

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
void arch_dma_flush(const volatile void *p, size_t len);

/** Order the processor's earlier memory accesses before its later ones, as
 * a device reading memory sees them. */
void arch_dma_fence(void);

/** Write back the whole cache to memory, for a device about to read buffers
 * the caller has not tracked one by one. */
void arch_dma_flush_all(void);

#endif /* _ARCH_H_ */
