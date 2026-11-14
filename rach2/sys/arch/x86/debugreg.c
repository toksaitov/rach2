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

#if defined(__x86_64__) && !defined(RACH2_HOSTTEST)
/* The `sysenter` entry's first instruction, and the first one a single
 * step through it reaches with TF clear (isr_handler.s). */
extern const char sysenter_entry[], sysenter_stepped[];
#endif

/* DR7: the enable bits of the four breakpoints, and the bits the processor
 * is given: those, LE and GE, bit 10, which reads as one, and the conditions
 * and lengths. The rest are GD and reserved bits, which a tracer may write
 * and reads back, as on Linux, but the processor never sees; on x86-64 a one
 * in bits 32 to 63 makes the write to DR7 fault. */
#define DR7_ENABLES  0x000000FFu
#define DR7_WRITABLE 0xFFFF07FFul

/* DR6: the four breakpoints' bits and the single-step bit; and the bits
 * that read as one when no exception sets them, Linux's DR6_RESERVED. A
 * thread's copy holds the first two with the reserved bits flipped to 0,
 * Linux's `virtual_dr6`, so that a new thread's 0 reads as DR6's value
 * after reset. */
#define DR6_HITS     0x0000000Fu
#define DR6_BS       0x00004000u
#define DR6_RESERVED 0xFFFF0FF0ul

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

/** DR7 is written only when the incoming thread has enables or the
 * processor still has some loaded, so a switch between threads without
 * breakpoints writes no debug register. */
void arch_debug_switch(const process_t *next)
{
    unsigned int cpu = arch_cpu_id();
    unsigned long v;

    if (next && (next->debugreg[7] & DR7_ENABLES)) {
        v = next->debugreg[0];
        DR_WRITE(DR0_MODRM, v);
        v = next->debugreg[1];
        DR_WRITE(DR1_MODRM, v);
        v = next->debugreg[2];
        DR_WRITE(DR2_MODRM, v);
        v = next->debugreg[3];
        DR_WRITE(DR3_MODRM, v);
        v = (next->debugreg[7] & DR7_WRITABLE) | DR7_ONE;
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
 * have cleared them while this processor still had DR7 loaded. The copy
 * keeps the single-step bit and the bits of the breakpoints DR7 enables,
 * as Linux's `exc_debug_user` and `ptrace_triggered` build `virtual_dr6`.
 */
static int32_t debugreg_code(unsigned long dr6)
{
    if (current_proc) {
        unsigned long on = current_proc->debugreg[7];
        unsigned long hits = (on | (on >> 1)) & 0x55u;

        hits = (hits & 1u) | ((hits >> 1) & 2u) | ((hits >> 2) & 4u) | ((hits >> 3) & 8u);
        current_proc->debugreg[6] = dr6 & (DR6_BS | hits);
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

void debugreg_drop(void)
{
    unsigned long v = 0;

    DR_WRITE(DR6_MODRM, v);
    v = DR7_ONE;
    DR_WRITE(DR7_MODRM, v);
    dr7_live[arch_cpu_id()] = 0;
    if (current_proc && (current_proc->debugreg[7] & DR7_ENABLES)) {
        proc_work_set(current_proc, WORK_DEBUGREG);
    }
}

/** A single step through the `sysenter` entry is the program's TF, which
 * the instruction leaves set, and goes on until the entry clears it; the
 * trap comes at the return to user mode. */
int debugreg_drop_sysenter(uintptr_t pc)
{
#if defined(__x86_64__) && !defined(RACH2_HOSTTEST)
    if (pc < (uintptr_t)sysenter_entry || pc > (uintptr_t)sysenter_stepped) {
        return 0;
    }
    debugreg_drop();
    return 1;
#else
    (void)pc;
    return 0;
#endif
}

/** `arch_debug_switch` to the current thread does the work. */
void debugreg_resume(void)
{
    if (!dr7_live[arch_cpu_id()] || !current_proc || !(current_proc->debugreg[7] & DR7_ENABLES)) {
        arch_debug_switch(current_proc);
    }
}

int debugreg_loaded(void)
{
    return dr7_live[arch_cpu_id()];
}

/**
 * Whether the DR7 value `v` sets what a tracer may set with the addresses `t`
 * has in DR0 to DR3: for each breakpoint it enables an address aligned to its
 * length whose bytes are all user addresses, from USER_ADDR_MIN to the last
 * below USER_ADDR_MAX, a length of 1, 2 or 4 bytes (or 8 on x86-64), and an
 * execute breakpoint of length 1; an I/O breakpoint, which needs CR4.DE, is
 * refused. The scheduler loads what this accepts into the processor, where a
 * breakpoint on a kernel address would fire in the kernel's own code, an
 * execute breakpoint there at every return to it.
 */
static int dr7_valid(const process_t *t, unsigned long v)
{
    int i;

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

/** DR6 is the copy with its reserved bits flipped back to ones, and DR7 the
 * value last written, as Linux's `ptrace_get_debugreg` reads them. */
unsigned long debugreg_peek(const process_t *t, unsigned n)
{
    if (n == 6) {
        return t->debugreg[6] ^ DR6_RESERVED;
    }
    return t->debugreg[n];
}

/** An address is checked against the DR7 the thread has, and a DR7 against
 * the addresses, so the pair is checked whichever is written first. DR6 is
 * kept with its reserved bits flipped and DR7 as written, its bits outside
 * DR7_WRITABLE ignored, as Linux's `ptrace_set_debugreg` keeps them. */
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
        proc_work_set(t, WORK_DEBUGREG);
    } else if (n == 6) {
        t->debugreg[6] = v ^ DR6_RESERVED;
    } else {
        if (!dr7_valid(t, v)) {
            return -EINVAL;
        }
        t->debugreg[7] = v;
        proc_work_set(t, WORK_DEBUGREG);
    }
    return 0;
}
