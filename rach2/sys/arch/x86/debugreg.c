/*
 * debugreg.c: the hardware breakpoints a tracer sets through ptrace(2), on
 * i686 and x86-64.
 */

#include "debugreg.h"

#include "../arch.h"
#include "../../lib/kerrno.h"
#include "../../lib/ksignal.h"

/* DR7: the enable bits of the four breakpoints, and GD, which a tracer may
 * not set. */
#define DR7_ENABLES 0x000000FFu
#define DR7_GD      0x00002000u

/* DR6: the four breakpoints' bits and the single-step bit. */
#define DR6_HITS 0x0000000Fu
#define DR6_BS   0x00004000u

/*
 * The debug registers, as bytes: TinyCC's assembler has no mnemonic for
 * them. `mov %drN, %eax` is 0F 21 /r and `mov %eax, %drN` 0F 23 /r, with
 * the register N in the ModRM byte's reg field and the accumulator in its
 * r/m field; in long mode the same bytes move all 64 bits of RAX.
 */
#define DR_READ(n, v)  __asm__ __volatile__(".byte 0x0f, 0x21, " n : "=a"(v))
#define DR_WRITE(n, v) __asm__ __volatile__(".byte 0x0f, 0x23, " n : : "a"(v) : "memory")
#define DR0_MODRM      "0xc0"
#define DR1_MODRM      "0xc8"
#define DR2_MODRM      "0xd0"
#define DR3_MODRM      "0xd8"
#define DR6_MODRM      "0xf0"
#define DR7_MODRM      "0xf8"

void arch_debug_switch(const process_t *old, const process_t *next)
{
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
        v = next->debugreg[7] | DR7_ONE;
        DR_WRITE(DR7_MODRM, v);
    } else if (old && (old->debugreg[7] & DR7_ENABLES)) {
        v = DR7_ONE;
        DR_WRITE(DR7_MODRM, v);
    }
}

int32_t debugreg_status(void)
{
    unsigned long dr6;
    unsigned long zero = 0;

    DR_READ(DR6_MODRM, dr6);
    DR_WRITE(DR6_MODRM, zero);
    if (current_proc) {
        current_proc->debugreg[6] = dr6;
    }
    if (dr6 & DR6_HITS) {
        return TRAP_HWBKPT;
    }
    return (dr6 & DR6_BS) ? TRAP_TRACE : 0;
}

/**
 * Whether the DR7 value `v` sets what a tracer may set with the addresses
 * `t` has in DR0 to DR3: no general-detect bit, and for each breakpoint it
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

    if (v & DR7_GD) {
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
