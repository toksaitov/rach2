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
 *   mmio_read8/16/32(a), mmio_write8/16/32(a, v)  a device register's read
 *                          and write at the kernel address `a`, which
 *                          map_mmio gave
 *   SC_NR(p)               the system call number in a saved frame
 *   SC_ARG(p, n)           the call's argument n, 0 to 5, as unsigned long
 *   SC_RET(p)              the register the call's result goes in (an lvalue)
 *   SC_COMPAT(p)           whether the call is of a second ABI's table,
 *                          i386's by `int $0x80` on x86-64; 0 elsewhere
 *   SC_AUDIT_ARCH(p)       the architecture seccomp and ptrace see for the
 *                          call; AUDIT_ARCH where there is one ABI
 *   IA32_SYSCALL_TABLE_SIZE  the compat table's size, where there is one
 *   FRAME_PC(p), FRAME_SP(p)  the interrupted instruction and stack pointers
 *   FRAME_USER(p)          whether the entry came from user mode
 *   SYSCALL_INSN_LEN       the bytes of the instruction a system call is
 *                          made by, which a restart rewinds over
 *   SC_RESTART_NR(p)       the number of restart_syscall in the table of the
 *                          call in `p`, which a restart through the
 *                          thread's restart block makes
 *   PAGE_SIZE, PAGE_SHIFT  the page the kernel maps
 *   USER_ADDR_MIN, USER_ADDR_MAX  the range a program may map
 *   ELF_CLASS, ELF_MACHINE the programs the kernel loads
 *   ELF_ET_DYN_BASE, USER_STACK_BASE, USER_MMAP_BASE  a new image's layout
 *   ASLR_STACK_PAGES, ASLR_MMAP_PAGES, ASLR_BRK_PAGES, ASLR_SP_BYTES  the
 *                          bounds of its random offsets, 0 where none is
 *                          drawn
 *   ARCH_GREGS             the words of Linux's `user_regs_struct`
 *   SYSCALL_TABLE_SIZE     one more than the highest system call number
 *   SC_CLONE_TLS, SC_CLONE_CTID  clone's arguments, whose order differs
 *   AUDIT_ARCH             the architecture seccomp's filters see
 *   ARCH_NAME              what uname(2) reports as the machine
 *   pte_t, PTE_*, MMU_TABLE_ENTRIES  a page table entry, its bits, and the
 *                          entries of a leaf table (mm/paging.h);
 *                          PTE_NOEXEC forbids instruction fetches from a
 *                          user page, or is 0 where the processor cannot;
 *                          PTE_PKEY holds a page's protection key from bit
 *                          PTE_PKEY_SHIFT, or is 0 where there are none
 *   HARD_MEMORY_LIMIT, KVA_*  the kernel's own layout
 *   PAGE_FAULT_VECTOR, FAULT_WRITE(p), FAULT_USER(p), FAULT_EXEC(p),
 *   FAULT_PK(p)            the page fault: a write, from user mode, an
 *                          instruction fetch, a protection key's refusal
 *   arch_cpu_id()          the running processor's index in the
 *                          per-processor table (proc/percpu.h)
 *   IRQ_VECTOR(line)       the vector a platform interrupt line arrives on
 *   ISR_DEVICE_FIRST, ISR_DEVICE_LAST  the vectors a device's interrupts take
 */
#if defined(RACH2_HOSTTEST) && defined(RACH2_HOST_X86_64)
/* A host test of x86-64's own code, natively on a 64-bit host. */
#include "x86_64/cpu.h"
#elif defined(RACH2_HOSTTEST)
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

/* An architecture with one system call ABI. */
#ifndef SC_COMPAT
#define SC_COMPAT(p) 0
#endif
#ifndef SC_AUDIT_ARCH
#define SC_AUDIT_ARCH(p) AUDIT_ARCH
#endif

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

/** Execute an invalid opcode, for the shell's `badop` probe. */
void arch_invalid_opcode(void);

/** Allow interrupts and wait for one, as one step, so that an interrupt
 * that arrives between the two cannot be slept through; interrupts are off
 * when this returns. */
void arch_idle_masked(void);

/** Whether a device's or the timer's interrupt waits to be taken on this
 * processor while its interrupts are off; the processors' own do not
 * count. */
int arch_irq_waiting(void);

/** Whether an idle processor polls before it halts: under a hypervisor
 * that says its processors are the guest's alone, as KVM's
 * KVM_HINTS_REALTIME says, or with `cpuidle_haltpoll.force`, as Linux
 * loads cpuidle-haltpoll (drivers/cpuidle/cpuidle-haltpoll.c:96-111). Read
 * once, since in a guest `cpuid` is an exit. */
int arch_idle_poll(void);

/** Hold interrupts off and stop this processor for good. */
void arch_stop(void) K_NORETURN;

/** Tell the processor the caller is spinning on a value another agent will
 * change. */
void arch_relax(void);

/** Serialize this processor: the instructions before finish, and those
 * after are fetched again, so code another processor wrote runs as
 * written, as Linux's sync_core. */
void arch_sync_core(void);

/** Store `value` at `*p` and return what was there, as one step no other
 * processor's access comes between, and as a full barrier: no load or store
 * of this processor's moves across it. */
unsigned int arch_atomic_xchg(volatile unsigned int *p, unsigned int value);

/** Store `value` at `*p` if `*p` is `old`, and return what was there, as one
 * step no other processor's access comes between, and as a full barrier. */
unsigned int arch_atomic_cmpxchg(volatile unsigned int *p, unsigned int old, unsigned int value);

/** `arch_atomic_xchg` of a word as wide as a pointer, for a lock word that
 * holds its holder's record. */
uintptr_t arch_atomic_xchg_ptr(volatile uintptr_t *p, uintptr_t value);

/** `arch_atomic_cmpxchg` of a word as wide as a pointer. */
uintptr_t arch_atomic_cmpxchg_ptr(volatile uintptr_t *p, uintptr_t old, uintptr_t value);

/**
 * Leave the current stack for the one ending at `sp`, allow interrupts, and
 * run `fn`, which does not return: how a process that ends hands its
 * processor back to the debug shell on the boot stack.
 */
void arch_run_on_stack(uintptr_t sp, void (*fn)(void)) K_NORETURN;

/**
 * Enter user mode at `pc` with the stack pointer `sp` and interrupts on:
 * how the first program, and the debug shell's own programs, start. Every
 * general register but the stack pointer is cleared. Does not return.
 */
void arch_enter_user(uintptr_t pc, uintptr_t sp) K_NORETURN;

/** Make `base` the thread pointer of the thread about to run in user mode,
 * where its TLS block starts (on i686, the base of the GDT's TLS entry). */
void arch_tls_set(uintptr_t base);

/** The running thread's thread pointer as it is now, which a fork gives its
 * child: on x86-64 FS's base, which a selector the program loaded into FS
 * changes without the kernel's knowing; on i686 the record's `tls_base`,
 * which set_thread_area and a tracer's PTRACE_SET_THREAD_AREA change. */
uintptr_t arch_tls_get(void);

/** Read clone's CLONE_SETTLS argument `arg` into the thread pointer it
 * gives the new thread: 0, or a negative errno. On i686 the argument points
 * at a struct user_desc, which -EFAULT means cannot be read and -EINVAL that
 * set_thread_area would refuse; on x86-64 it is the pointer itself, and
 * -EPERM means it is not below USER_ADDR_MAX. */
long arch_clone_tls(unsigned long arg, uintptr_t *base_out);

struct process;
struct mm;

/** Free what the architecture keeps with the address space `mm` beside its
 * pages, at the last user's put, and stop using it on this processor: the
 * x86-64's LDT (arch/x86_64/ldt.c); nothing on i686. */
void arch_mm_release(struct mm *mm);

/** Give `to`, fork's copy of the address space `from`, a copy of what the
 * architecture keeps with it: 0, or -ENOMEM, and `to` is put. */
int arch_mm_copy(struct mm *to, const struct mm *from);

/** Make the saved frame `p` start a new program image: at `entry`, with the
 * stack pointer `sp`, the general registers cleared (on i686 all but EAX,
 * where the call's result goes) and the user segments loaded where the
 * architecture has them. */
void arch_exec_frame(isr_params_t *p, uintptr_t entry, uintptr_t sp);

/** Lay out a new process's kernel stack as the context switch expects to
 * find it, so that its first switch returns into `entry`. */
void arch_context_init(struct process *proc, void (*entry)(void));

/** Lay out a forked `child`'s kernel stack for its first switch, from the
 * parent's saved frame `params`, with 0 for fork's result and `newsp`, when
 * not 0, for the user stack pointer. */
void arch_fork_frame(struct process *child, const isr_params_t *params, uintptr_t newsp);

/** Nonzero when a record that assembly reads at fixed offsets does not have
 * the layout it expects: on i686 the process record, which switch.s reads,
 * and on x86-64 the processor's `struct cpu`, which the entry code reads.
 * GCC makes the same check at build time, and `init_proc` calls this at
 * every boot, for a kernel TinyCC built, since TinyCC cannot make it. */
int arch_context_check(void);

/* A system call's handler: given the saved registers, from which it reads
 * its arguments, it returns the call's result, a value or a negative errno,
 * as wide as a register. */
typedef long (*syscall_fn_t)(isr_params_t *params);

/** Fill `table`, SYSCALL_TABLE_SIZE entries, with the handler of each system
 * call number the architecture has, and route its system call entry to
 * `dispatch`, which runs the handler the number in the saved registers
 * selects: on i686 the gate of `int $0x80`, on x86-64 the vector
 * `syscall_entry` gives its frame before it calls `_isr_router`. */
void arch_syscall_init(syscall_fn_t *table, void (*dispatch)(isr_params_t *params));

/** AT_HWCAP of every program, Linux's ELF_HWCAP on x86: the feature bits of
 * `cpuid` leaf 1 in EDX (x86/cpu.c). */
uint32_t arch_hwcap(void);

#if defined(IA32_SYSCALL_TABLE_SIZE)
/** Fill `table`, IA32_SYSCALL_TABLE_SIZE entries, with the handler of each
 * i386 number, for the calls a frame's SC_COMPAT marks (x86_64/ia32_systab.c).
 * The entry is the one `arch_syscall_init` routes. */
void ia32_syscall_init(syscall_fn_t *table);

/** Whether the kernel runs i386 programs and calls, Linux's `ia32_enabled`:
 * 1 unless the boot option `ia32_emulation=` turned them off. The second,
 * Linux's `ia32_enabled_verbose`, which the loader asks of an i386 program,
 * also logs the first refusal (x86_64/ia32_exec.c). */
int ia32_enabled(void);
int ia32_enabled_verbose(void);

/** The address of __kernel_vsyscall in the 32-bit vDSO whose image is at
 * `base`, AT_SYSINFO (x86_64/vdso32.c). */
uintptr_t vdso32_vsyscall_at(uintptr_t base);

/** clone's CLONE_SETTLS in an i386 call: the `struct user_desc` at `uptr`
 * read and checked into the TLS entry and descriptor the child gets, as
 * set_thread_area checks one; 0 or its error (x86_64/tls.c). The second
 * gives them to `child` before it first runs. */
struct process;
long ia32_clone_tls(unsigned long uptr, uint32_t *entry, uint64_t *desc);
void ia32_clone_tls_set(struct process *child, uint32_t entry, uint64_t desc);

/* The words of i386's `user_regs_struct`. */
#define IA32_GREGS 17

/** Fill `regs` with the IA32_GREGS registers of `t`, an i386 program's
 * thread stopped or dumping core, in the order of i386's
 * `user_regs_struct`, each the low 32 bits of the x86-64 register, as
 * Linux's `getreg32` gives them (x86_64/ia32_core.c). */
void ia32_regs_get(const struct process *t, uint32_t *regs);

/** The bytes of the floating-point and TLS notes of one thread of an i386
 * program's core file, at most. */
uint32_t ia32_core_fp_size(void);

/** Put the floating-point and TLS notes of the thread `t` of an i386
 * program at `*p`, with `core_note_put`, and step past them. */
void ia32_core_fp_notes(uint8_t **p, const struct process *t);

/** Whether `fn`, a compat handler of its own, answers a call Linux gives
 * EINTR in every case, as the x86-64 calls the dispatcher never restarts
 * (`syscall_restartable`), so that the i386 call it answers with the
 * arguments in `params` is not restarted either: rt_sigtimedwait,
 * epoll_pwait2 and the two returns from a handler (x86_64/ia32_time.c). */
int ia32_never_restarts(syscall_fn_t fn, const isr_params_t *params);

/*
 * The socket structures of an i386 call, which the shared socket code reads
 * and writes in place of its own while `in_compat_syscall` is true, as
 * Linux's MSG_CMSG_COMPAT has it (x86_64/ia32_net.c, whose ia32_abi.h
 * defines them): compat_msghdr, the stride of compat_mmsghdr and the offset
 * of its count, and compat_cmsghdr, whose data and successor are aligned to
 * 4 bytes.
 */
struct kernel_msghdr;
struct kernel_cmsghdr;
#define IA32_MSGHDR_LEN    28u
#define IA32_MMSGHDR_LEN   32u
#define IA32_MMSG_LEN_OFF  28u
#define IA32_CMSG_HDR      12u
#define IA32_CMSG_ALIGN(n) (((n) + 3u) & ~3u)

/** Read the i386 msghdr at the user address `u` into `h`, its pointers and
 * lengths zero-extended; 0 or -EFAULT. */
int ia32_msghdr_get(struct kernel_msghdr *h, uintptr_t u);

/** Write what a receive gives back of `h`, its name's length when it has a
 * name buffer, its control length and its flags, into the i386 msghdr at
 * `u`; 0 or -EFAULT. */
int ia32_msghdr_put(uintptr_t u, const struct kernel_msghdr *h);

/** Read the i386 control message header at `at` into `h`; 0 or -EFAULT. */
int ia32_cmsg_get(struct kernel_cmsghdr *h, uintptr_t at);

/** Write `h` as an i386 control message header at `at`; 0 or -EFAULT. A
 * length past 32 bits is cut, which no caller passes. */
int ia32_cmsg_put(uintptr_t at, const struct kernel_cmsghdr *h);
#endif

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
 * words of `struct user` and the thread pointer (i686's
 * PTRACE_GET_THREAD_AREA and PTRACE_SET_THREAD_AREA, x86-64's
 * PTRACE_ARCH_PRCTL). Returns 0 or a negative errno: -EIO for a request it
 * does not have or a word it refuses, -EFAULT for the caller's memory,
 * -ESRCH when the thread left its stop while the caller's data was copied
 * in (`pid` is how it is looked up again), -EINVAL for a floating-point
 * image the processor would fault on or a thread pointer request refused,
 * and on x86-64 -EPERM for a base at or above USER_ADDR_MAX. A
 * PTRACE_SETREGS stops at the first register refused, with those before it
 * set. */
long arch_ptrace(struct process *t, uint32_t pid, long request, unsigned long addr,
                 unsigned long data);

/*
 * The register sets of PTRACE_GETREGSET and PTRACE_SETREGSET, named by the
 * type of the core note that has each (NT_PRSTATUS and the rest). The
 * shared ptrace(2) reads the iovec, applies Linux's rules of length and
 * copies the set; the architecture says which sets it has and converts
 * each.
 */

/** The bytes of the register set `type` of the stopped thread `t`, with
 * `*unit` the size of its unit, of which a length must be a multiple:
 * -EINVAL for a set the architecture lacks, -ENODEV for one the processor
 * lacks, such as the XSAVE image without XSAVE. On x86-64 a thread that
 * runs 32-bit code has Linux's i386 sets, with their units, as Linux's
 * `task_user_regset_view` chooses them, and the set's get and set below
 * follow the same choice. */
long arch_regset_size(const struct process *t, unsigned long type, uint32_t *unit);

/** Write all of the register set `type` of `t`, a thread stopped for its
 * tracer, into `out`. */
void arch_regset_get(const struct process *t, unsigned long type, uint8_t *out);

/** Set the register set `type` of the stopped thread `t` from the first
 * `len` bytes of `in`, a multiple of its unit and at most its size: 0, or a
 * negative errno as Linux's set of that type returns it. */
long arch_regset_set(struct process *t, unsigned long type, const uint8_t *in, uint32_t len);

/** At a switch to `next`: load the hardware breakpoints `next` has, or
 * clear the processor's enables when it has none. The caller holds the big
 * lock with interrupts off. */
void arch_debug_switch(const struct process *next);

/** The bytes of the floating-point notes of one thread in a core file. */
uint32_t arch_core_fp_size(void);

/** Put the floating-point notes of the thread `t` at `*p`, with
 * `core_note_put`, and step past them. */
void arch_core_fp_notes(uint8_t **p, const struct process *t);

/* The debug shell's ring-3 exercise: `arch_user_test_size` bytes of
 * position-independent machine code with its message after it, which
 * writes the message to descriptor 1 and exits with status 0; the shell
 * copies them into a user page and runs them as a process. */
extern const unsigned char arch_user_test_code[];
extern const unsigned int arch_user_test_size;

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

/** A counter of processor cycles, for entropy and the serial port's transmit
 * wait: on x86 the time-stamp counter, read without a check that the
 * processor has one. */
uint64_t arch_cycles(void);

/** The nanoseconds the hypervisor kept the running processor ready to run
 * but did not run it, since the processor started: KVM's steal time
 * (x86/kvmsteal.c), or 0 where there is none. Interrupts are off. */
uint64_t arch_steal_ns(void);

/** The hypervisor's steal time for processor `cpu`, as arch_steal_ns reads
 * this one's. Any processor may read it; no MSR is read. */
uint64_t arch_cpu_steal_ns(int cpu);

/** Whether KVM marks processor `cpu` as preempted by its host. A hint
 * read without a lock; zero without a shared steal-time record. */
int arch_cpu_preempted(int cpu);

/** Whether the host stopped this guest since the last call on this
 * processor, as KVM's clock page says once and this clears
 * (x86/kvmclock.c), so that a stretch the guest did not run is not taken
 * for a lockup, as Linux's kvm_check_and_clear_guest_paused; 0 where no
 * hypervisor says. Interrupts are off. */
int arch_guest_paused(void);

/** Have the hypervisor flush processor `cpu`'s TLB before it runs again,
 * where it has taken that processor off the host's processors: KVM's
 * KVM_VCPU_FLUSH_TLB mark in the steal-time record, which KVM offers by
 * KVM_FEATURE_PV_TLB_FLUSH. 1 when marked, and the caller needs no answer
 * from that processor to know its TLB holds none of the translations it
 * changed; 0 otherwise, or where there is no such hypervisor. */
int arch_cpu_tlb_deferred(int cpu);

/** A value from the processor's entropy source into `out`: `seed` asks for
 * the conditioned seed source (RDSEED), otherwise the generator (RDRAND).
 * Returns 1, or 0 when the source has none to give. */
int arch_random(uint32_t *out, int seed);

/**
 * Copy `n` bytes from `src` to `dst`, one of them a user address, by the
 * processor's string moves, and return the bytes not copied, 0 when all
 * were. A fault on a user page is resolved by the page fault handler as a
 * user's fault would be, and one it cannot resolve ends the copy at the
 * first byte that faults (`arch_user_copy_fixup`), a faulting word being
 * retried byte by byte, as Linux's copy_user_generic ends at its exception
 * table's entry (arch/x86/lib/copy_user_64.S). Interrupts may be
 * on; the copy may sleep in the resolution of a fault.
 */
size_t arch_user_copy(void *dst, const void *src, size_t n);

/** For a page fault the handler could not resolve, in kernel mode: when it
 * was taken in `arch_user_copy`, point the frame at the copy's return of
 * what it left and return 1; else return 0. */
int arch_user_copy_fixup(isr_params_t *p);

/** Copy `words` machine words from `src` to `dst`, which do not overlap, by
 * the processor's string instructions; defined where cpu.h defines
 * ARCH_STRING_WORDS, for k_memcpy. */
void arch_copy_words(void *dst, const void *src, size_t words);

/** Copy `words` machine words downward, from the word at `src` to the word
 * at `dst` and on to lower addresses, for k_memmove when the destination
 * starts inside the source; `dst` and `src` address the last word of each
 * range. */
void arch_copy_words_down(void *dst, const void *src, size_t words);

/** Store `fill` in `words` machine words from `dst`, for k_memset. */
void arch_fill_words(void *dst, unsigned long fill, size_t words);

/** Write back the cache lines of the `len` bytes at `p` to memory, for a
 * device that reads them without snooping the cache, and order the writes
 * before what follows; nothing for a null `p` or a zero `len`. A processor
 * without CLFLUSH writes back its whole cache. */
void arch_dma_flush(const volatile void *p, size_t len);

/** Order the processor's earlier memory accesses before its later ones, as
 * a device reading memory sees them. */
void arch_dma_fence(void);

/** Write back the whole cache to memory, for a device about to read buffers
 * the caller has not tracked one by one. */
void arch_dma_flush_all(void);

/** The bits of a page table entry that map a page write-combining: PTE_WC
 * where the boot processor's page attribute table gives the type, and
 * uncached bits where it does not (arch/x86/pat.c on a PC). */
pte_t arch_pte_wc(void);

/* The two calls `drm_start` (sys/drm/drm.h) makes of `arch_drm_probe`: the
 * platform's native display drivers first, which take the firmware's
 * framebuffer from its driver, then that driver on what they left. */
#define ARCH_DRM_NATIVE   0
#define ARCH_DRM_FIRMWARE 1

/** Probe the platform's own display drivers of `phase`, ARCH_DRM_NATIVE
 * or ARCH_DRM_FIRMWARE; at boot, once each, in that order. Defined by the
 * platform's display code, sys/drm/drm_pc.c on a PC. */
void arch_drm_probe(int phase);

#endif /* _ARCH_H_ */
