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

/* The DR6 bits of the exceptions each processor deferred, with DEFER_USER
 * for a user hit, until a switch or `debugreg_deliver` takes them. Only its
 * own processor changes an entry. */
static volatile unsigned long deferred[NR_CPUS];

/** The processor's deferred bits go to the outgoing thread, so that they
 * follow it to another processor. DR7 is written only when the incoming
 * thread has enables or the processor still has some loaded, so a switch
 * between threads without breakpoints writes no debug register. */
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

/**
 * Store `dr6` as the current thread's DR6 and classify it as
 * `debugreg_status` returns it: TRAP_HWBKPT, TRAP_TRACE, 0, or
 * DEBUGREG_STALE for a breakpoint the thread's saved enables no longer
 * have, after which the processor's DR7 is cleared. A tracer that ended may
 * have cleared them while this processor still had DR7 loaded.
 */
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

int32_t debugreg_status(void)
{
    unsigned long dr6;
    unsigned long zero = 0;

    DR_READ(DR6_MODRM, dr6);
    DR_WRITE(DR6_MODRM, zero);
    return debugreg_code(dr6);
}

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

/** Intel can report this delayed hit without a DR6 status bit. With the
 * thread's enables saved it is a user data breakpoint: TF is clear at the
 * entry (the i686 interrupt gate clears it, and x86-64's FMASK masks it),
 * and `dr7_valid` forbids kernel addresses and GD. The mark stays with the
 * thread across switches until the system call returns to user mode. */
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

/** The system call's exit reports a single step in the kernel, so only a
 * hardware hit with the thread's enables saved posts a signal here, as in
 * i686's kernel debug handler. Both records are cleared even when a detach
 * made the hit stale. A user hit is TRAP_HWBKPT even without a DR6 status
 * bit; the thread's saved DR6 has only the bits the processor reported. */
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

/** A stale hit, of a thread whose enables a tracer cleared, no longer
 * forces a signal, and does not interrupt a wait. */
int arch_debug_pending(void)
{
    if (!current_proc || !(current_proc->debugreg[7] & DR7_ENABLES)) {
        return 0;
    }
    return ((deferred[arch_cpu_id()] | current_proc->debug_pending) & DEFER_USER) != 0;
}

/** A live user hit stays deferred until the return to user mode, so that a
 * signal wait can neither take it nor restore a blocked mask after it was
 * forced; while one is pending nothing is posted, since the kernel hits
 * share its record. */
void arch_debug_deliver(void)
{
    if (!arch_debug_pending()) {
        debugreg_deliver();
    }
}

/** `arch_debug_switch` from the thread to itself does the work, and moves
 * the processor's deferred bits into the thread. */
void debugreg_resume(void)
{
    if (!dr7_live[arch_cpu_id()] || !current_proc || !(current_proc->debugreg[7] & DR7_ENABLES)) {
        arch_debug_switch(current_proc, current_proc);
    }
}

/**
 * Whether the DR7 value `v` sets what a tracer may set with the addresses
 * `t` has in DR0 to DR3: no general-detect or reserved bit, and for each
 * breakpoint it enables an address aligned to its length whose bytes are
 * all user addresses, from USER_ADDR_MIN to the last below USER_ADDR_MAX, a
 * length of 1, 2 or 4 bytes (or 8 on x86-64), and an execute breakpoint of
 * length 1; an I/O breakpoint, which needs CR4.DE, is refused. The scheduler
 * loads what this accepts into the processor, where a breakpoint on a
 * kernel address would fire in the kernel's own code, an execute
 * breakpoint there at every return to it.
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

unsigned long debugreg_peek(const process_t *t, unsigned n)
{
    return n == 7 ? (t->debugreg[7] | DR7_ONE) : t->debugreg[n];
}

/** An address is checked against the DR7 the thread has, and a DR7 against
 * the addresses, so the pair is checked whichever is written first. */
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
