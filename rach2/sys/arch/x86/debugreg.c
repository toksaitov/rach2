/*
 * debugreg.c: the hardware breakpoints a tracer sets through ptrace(2), on
 * i686 and x86-64.
 */

#include "debugreg.h"

#include "../arch.h"
#include "../../lib/kerrno.h"
#include "../../lib/ksignal.h"
#include "../../lib/kstring.h"
#include "../../proc/percpu.h"

/* The first instruction after the user's system-call instruction. */
#if defined(__x86_64__) && !defined(RACH2_HOSTTEST)
extern void syscall_entry(void);
#define DEBUG_SYSCALL_ENTRY syscall_entry
#else
extern void isr128(void);
#define DEBUG_SYSCALL_ENTRY isr128
#endif

/* DR7: the enable bits of the four breakpoints, and the bits a tracer may
 * write: those, LE and GE, bit 10, which reads as one, and the conditions
 * and lengths. The rest are GD and reserved bits; on x86-64 a one in bits
 * 32 to 63 makes the write to DR7 fault. */
#define DR7_ENABLES  0x000000FFu
#define DR7_WRITABLE 0xFFFF07FFul

/* DR6: the four breakpoints' bits and the single-step bit. */
#define DR6_HITS 0x0000000Fu
#define DR6_BS   0x00004000u

/* A deferred hit from user code, delayed across a system call. This software
 * bit follows DR6 through a switch, but is never exposed as debug status. */
#define DEFER_USER 0x80000000ul

/*
 * The debug registers, as bytes: TinyCC's assembler has no mnemonic for
 * them. `mov %drN, %eax` is 0F 21 /r and `mov %eax, %drN` 0F 23 /r, with
 * the register N in the ModRM byte's reg field and the accumulator in its
 * r/m field; in long mode the same bytes move all 64 bits of RAX.
 */
#ifndef RACH2_HOSTTEST
#define DR_READ(n, v)  __asm__ __volatile__(".byte 0x0f, 0x21, " n : "=a"(v))
#define DR_WRITE(n, v) __asm__ __volatile__(".byte 0x0f, 0x23, " n : : "a"(v) : "memory")
#endif
#define DR0_MODRM "0xc0"
#define DR1_MODRM "0xc8"
#define DR2_MODRM "0xd0"
#define DR3_MODRM "0xd8"
#define DR6_MODRM "0xf0"
#define DR7_MODRM "0xf8"

/* Whether each processor has debug enables loaded. A tracer's exit can
 * clear a running tracee's saved DR7 without changing that processor's DR7. */
static volatile unsigned char dr7_live[NR_CPUS];
static volatile unsigned long deferred[NR_CPUS];

/** Save a deferred exception in the outgoing thread and load the next
 * thread's breakpoints. The caller holds the big lock with interrupts off. */
void arch_debug_switch(process_t *old, const process_t *next)
{
    unsigned int cpu = arch_cpu_id();
    unsigned long v;

    if (old) {
        old->debug_pending |= deferred[cpu];
    }
    deferred[cpu] = 0;
    if (next && (next->debugreg[7] & DR7_ENABLES)) {
        v = next->debugreg[0];
        DR_WRITE(DR0_MODRM, v);
        v = next->debugreg[1];
        DR_WRITE(DR1_MODRM, v);
        v = next->debugreg[2];
        DR_WRITE(DR2_MODRM, v);
        v = next->debugreg[3];
        DR_WRITE(DR3_MODRM, v);
        v = next->debugreg[7] | DR7_ONE;
        DR_WRITE(DR7_MODRM, v);
        dr7_live[cpu] = 1;
    } else if (dr7_live[cpu]) {
        v = DR7_ONE;
        DR_WRITE(DR7_MODRM, v);
        dr7_live[cpu] = 0;
    }
}

/** Classify DR6 against the current thread's saved enables. A tracer that
 * ended may have cleared them while this processor still had DR7 loaded. */
static int32_t debugreg_code(unsigned long dr6)
{
    if (current_proc) {
        current_proc->debugreg[6] = dr6;
    }
    if ((dr6 & DR6_HITS) && current_proc && !(current_proc->debugreg[7] & DR7_ENABLES)) {
        unsigned long v = DR7_ONE;

        DR_WRITE(DR7_MODRM, v);
        dr7_live[arch_cpu_id()] = 0;
        return (dr6 & DR6_BS) ? TRAP_TRACE : DEBUGREG_STALE;
    }
    if (dr6 & DR6_HITS) {
        return TRAP_HWBKPT;
    }
    return (dr6 & DR6_BS) ? TRAP_TRACE : 0;
}

/** Read and clear the processor's debug status for a user exception. */
int32_t debugreg_status(void)
{
    unsigned long dr6;
    unsigned long zero = 0;

    DR_READ(DR6_MODRM, dr6);
    DR_WRITE(DR6_MODRM, zero);
    return debugreg_code(dr6);
}

/** Save a kernel debug exception without taking a lock or using a thread's
 * stack. Only this processor changes these arrays. Disable its breakpoints
 * until the next switch or user return, so its interrupt stack cannot nest. */
void debugreg_defer(void)
{
    unsigned int cpu = arch_cpu_id();
    unsigned long dr6;
    unsigned long v = 0;

    DR_READ(DR6_MODRM, dr6);
    DR_WRITE(DR6_MODRM, v);
    v = DR7_ONE;
    DR_WRITE(DR7_MODRM, v);
    dr7_live[cpu] = 0;
    deferred[cpu] |= dr6 & (DR6_HITS | DR6_BS);
}

/** Defer a user watchpoint when pc is the first instruction of system-call
 * entry: isr128 on i686, syscall_entry on x86-64. Intel can report this
 * delayed hit without a DR6 status bit. With saved enables it is a user
 * data breakpoint: TF is clear (the i686 interrupt gate clears it; x86-64
 * FMASK masks it), and DR7 validation forbids kernel addresses and GD.
 * Its signal must be forced even when the thread blocks SIGTRAP.
 * Keep that origin across switches until the syscall returns to user mode.
 * Return 1 for that entry, or 0 without reading DR6 for any other address. */
int debugreg_defer_user(uintptr_t pc)
{
    unsigned int cpu;

    if (pc != (uintptr_t)&DEBUG_SYSCALL_ENTRY) {
        return 0;
    }
    cpu = arch_cpu_id();
    debugreg_defer();
    if (current_proc && (current_proc->debugreg[7] & DR7_ENABLES)) {
        deferred[cpu] |= DEFER_USER;
    }
    return 1;
}

/** Post a deferred debug exception at user return, or a kernel hit before
 * a pending-signal check, on the thread's stack with the big lock held and
 * interrupts off. A switch kept its status in the outgoing thread so it
 * follows that thread to another processor. The syscall exit reports kernel
 * single steps; only a hardware hit with saved enables posts a signal here,
 * as in i686's kernel debug handler. Both deferred copies are cleared even when
 * a detach made the hit stale. A hit delayed from user code forces SIGTRAP
 * even without a DR6 status bit; the saved DR6 contains only reported bits.
 * A kernel copy's hit respects the mask, so a signal wait can take it. */
void debugreg_deliver(void)
{
    unsigned int cpu = arch_cpu_id();
    unsigned long dr6;
    struct k_siginfo info;

    if (!current_proc) {
        deferred[cpu] = 0;
        return;
    }
    dr6 = deferred[cpu] | current_proc->debug_pending;
    deferred[cpu] = 0;
    current_proc->debug_pending = 0;
    if (dr6 == 0) {
        return;
    }
    k_memset(&info, 0, sizeof(info));
    info.code = debugreg_code(dr6 & ~DEFER_USER);
    if (dr6 & DEFER_USER) {
        info.code = TRAP_HWBKPT;
    }
    if (info.code == TRAP_HWBKPT && (current_proc->debugreg[7] & DR7_ENABLES)) {
        info.addr = FRAME_PC(proc_user_frame(current_proc));
        if (dr6 & DEFER_USER) {
            signal_force(current_proc, SIGTRAP, &info);
        } else {
            signal_post_thread(current_proc, SIGTRAP, &info);
        }
    }
}

/** Whether a deferred user hit must interrupt a wait regardless of the
 * signal mask or disposition. Leave its status for user-return delivery;
 * a stale hit no longer forces a signal. The caller holds the big lock
 * with interrupts off. */
int arch_debug_pending(void)
{
    if (!current_proc || !(current_proc->debugreg[7] & DR7_ENABLES)) {
        return 0;
    }
    return ((deferred[arch_cpu_id()] | current_proc->debug_pending) & DEFER_USER) != 0;
}

/** Post kernel hits before shared signal code reads the pending sets.
 * Keep a live user hit deferred until user return, so a signal wait cannot
 * take it or restore a blocked mask after it was forced. The caller runs
 * on the thread's stack with the big lock held and interrupts off. */
void arch_debug_deliver(void)
{
    if (!arch_debug_pending()) {
        debugreg_deliver();
    }
}

/** Reload enables cleared by a deferred exception, or by a tracer that
 * ended while the current thread ran on this processor. */
void debugreg_resume(void)
{
    if (!dr7_live[arch_cpu_id()] || !current_proc || !(current_proc->debugreg[7] & DR7_ENABLES)) {
        arch_debug_switch(current_proc, current_proc);
    }
}

/**
 * Whether the DR7 value `v` sets what a tracer may set with the addresses
 * `t` has in DR0 to DR3: no general-detect or reserved bit, and for each
 * breakpoint it
 * enables an address aligned to its length whose bytes are all user
 * addresses, from USER_ADDR_MIN to the last below USER_ADDR_MAX, a length of
 * 1, 2 or 4 bytes (or 8 on x86-64), and an execute breakpoint of length 1;
 * an I/O breakpoint, which needs CR4.DE, is refused. The scheduler loads
 * what this accepts into the processor, where a breakpoint on a kernel
 * address would fire in the kernel's own code, an execute breakpoint there
 * at every return to it.
 */
static int dr7_valid(const process_t *t, unsigned long v)
{
    int i;

    if ((v & ~DR7_WRITABLE) != 0) {
        return 0;
    }
    for (i = 0; i < 4; i++) {
        unsigned long rw = (v >> (16 + 4 * i)) & 3u;
        unsigned long len = (v >> (18 + 4 * i)) & 3u;
        unsigned long bytes = len == 3u ? 4u : len == 2u ? 8u : len + 1u;

        if (!(v & (3u << (2 * i)))) {
            continue;
        }
#ifndef __x86_64__
        if (len == 2u) {
            return 0;
        }
#endif
        if (rw == 2u || (rw == 0u && len != 0u) || (t->debugreg[i] & (bytes - 1u)) != 0 ||
            t->debugreg[i] < USER_ADDR_MIN || t->debugreg[i] > USER_ADDR_MAX - bytes) {
            return 0;
        }
    }
    return 1;
}

/** Read a saved debug register, including DR7's fixed bit. */
unsigned long debugreg_peek(const process_t *t, unsigned n)
{
    return n == 7 ? (t->debugreg[7] | DR7_ONE) : t->debugreg[n];
}

/** Validate a debug register write before keeping it in the stopped thread. */
int debugreg_poke(process_t *t, unsigned n, unsigned long v)
{
    if (n == 4 || n == 5 || n > 7) {
        return -EIO;
    }
    if (n < 4) {
        unsigned long old = t->debugreg[n];

        t->debugreg[n] = v;
        if (!dr7_valid(t, t->debugreg[7])) {
            t->debugreg[n] = old;
            return -EINVAL;
        }
    } else if (n == 6) {
        t->debugreg[6] = v;
    } else {
        v &= ~(unsigned long)DR7_ONE;
        if (!dr7_valid(t, v)) {
            return -EINVAL;
        }
        t->debugreg[7] = v;
    }
    return 0;
}
