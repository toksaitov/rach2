/*
 * cpu.c: the processor primitives of ../arch.h on an x86, 32-bit or 64-bit.
 *
 * Each is one instruction or a short run of them. The same source builds for
 * i686 and x86-64: the operand of a control register move takes the width of
 * a pointer, and `pushf` and `pop` without a size suffix take the width of
 * the mode the assembler is in. The few that differ by mode say so.
 */

#include "kcpu.h"

#include "../arch.h"

void arch_irq_enable(void)
{
    __asm__ __volatile__("sti" : : : "memory");
}

void arch_irq_disable(void)
{
    __asm__ __volatile__("cli" : : : "memory");
}

void arch_halt(void)
{
    __asm__ __volatile__("hlt" : : : "memory");
}

/* `sti` takes effect after the instruction that follows it, so an interrupt
 * pending at the `sti` is taken once `hlt` has begun waiting, and wakes it. */
void arch_idle(void)
{
    __asm__ __volatile__("sti; hlt" : : : "memory");
}

void arch_idle_masked(void)
{
    __asm__ __volatile__("sti; hlt; cli" : : : "memory");
}

void arch_stop(void)
{
    for (;;) {
        __asm__ __volatile__("cli; hlt" : : : "memory");
    }
}

void arch_relax(void)
{
    __asm__ __volatile__("pause" : : : "memory");
}

void arch_run_on_stack(uintptr_t sp, void (*fn)(void))
{
#if defined(__x86_64__)
    __asm__ __volatile__("mov %0, %%rsp\n\t"
                         "sti\n\t"
                         "jmp *%1"
                         :
                         : "r"(sp), "r"(fn)
                         : "memory");
#else
    __asm__ __volatile__("mov %0, %%esp\n\t"
                         "sti\n\t"
                         "jmp *%1"
                         :
                         : "r"(sp), "r"(fn)
                         : "memory");
#endif
    for (;;) {
    }
}

void arch_copy_words(void *dst, const void *src, size_t words)
{
#if defined(__x86_64__)
    __asm__ __volatile__("cld; rep movsq" : "+S"(src), "+D"(dst), "+c"(words) : : "memory", "cc");
#else
    __asm__ __volatile__("cld; rep movsl" : "+S"(src), "+D"(dst), "+c"(words) : : "memory", "cc");
#endif
}

void arch_fill_words(void *dst, unsigned long fill, size_t words)
{
#if defined(__x86_64__)
    __asm__ __volatile__("cld; rep stosq" : "+D"(dst), "+c"(words) : "a"(fill) : "memory", "cc");
#else
    __asm__ __volatile__("cld; rep stosl" : "+D"(dst), "+c"(words) : "a"(fill) : "memory", "cc");
#endif
}

uintptr_t arch_space_root(void)
{
    uintptr_t root;

    __asm__ __volatile__("mov %%cr3, %0" : "=r"(root));
    return root;
}

void arch_space_load(uintptr_t root)
{
    __asm__ __volatile__("mov %0, %%cr3" : : "r"(root) : "memory");
}

void arch_tlb_flush_page(uintptr_t va)
{
    __asm__ __volatile__("invlpg (%0)" : : "r"(va) : "memory");
}

uintptr_t arch_fault_address(void)
{
    uintptr_t addr;

    __asm__ __volatile__("mov %%cr2, %0" : "=r"(addr));
    return addr;
}

uint64_t arch_cycles(void)
{
    uint32_t lo, hi;

    __asm__ __volatile__("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

/* Whether the processor has RDSEED (CPUID leaf 7, EBX bit 18) and RDRAND
 * (leaf 1, ECX bit 30): -1 until asked, then 0 or 1. A leaf above the
 * highest the processor has returns another leaf's data, which could falsely
 * advertise a bit, and the instruction would then raise #UD, so each leaf is
 * asked only when the highest covers it. */
static int have_rdseed = -1;
static int have_rdrand = -1;

static void random_detect(void)
{
    uint32_t a, b, c, d, max_leaf;

    kcpu_id(0, &max_leaf, &b, &c, &d);
    have_rdseed = 0;
    have_rdrand = 0;
    if (max_leaf >= 7) {
        kcpu_id(7, &a, &b, &c, &d);
        have_rdseed = (b & (1u << 18)) != 0;
    }
    if (max_leaf >= 1) {
        kcpu_id(1, &a, &b, &c, &d);
        have_rdrand = (c & (1u << 30)) != 0;
    }
}

int arch_random(uint32_t *out, int seed)
{
    uint32_t v = 0;
    uint8_t ok = 0;

    if (have_rdseed < 0) {
        random_detect();
    }
    if (seed) {
        if (!have_rdseed) {
            return 0;
        }
        /* rdseed %eax as bytes 0f c7 f8: tcc has no mnemonic for it. */
        __asm__ __volatile__(".byte 0x0f, 0xc7, 0xf8\n\tsetc %1" : "=a"(v), "=qm"(ok) : : "cc");
    } else {
        if (!have_rdrand) {
            return 0;
        }
        /* rdrand %eax as bytes 0f c7 f0. */
        __asm__ __volatile__(".byte 0x0f, 0xc7, 0xf0\n\tsetc %1" : "=a"(v), "=qm"(ok) : : "cc");
    }
    if (!ok) {
        return 0;
    }
    *out = v;
    return 1;
}

/* Whether the processor has CLFLUSH (CPUID leaf 1, EDX bit 19), and the line
 * it flushes, from EBX bits 8 to 15 in units of 8 bytes: -1 until asked.
 * Emulators report none, and the flush then writes back the whole cache. */
static int have_clflush = -1;
static uint32_t clflush_size = 64;

static void clflush_detect(void)
{
    uint32_t eax = 0, ebx = 0, ecx = 0, edx = 0;
    uint32_t sz;

    kcpu_id(1, &eax, &ebx, &ecx, &edx);
    have_clflush = (edx & (1u << 19)) != 0;
    sz = ((ebx >> 8) & 0xFFu) * 8u;
    clflush_size = sz != 0 ? sz : 64u;
}

void arch_dma_fence(void)
{
    /* A full barrier without SSE's mfence: a locked add to the stack. */
#if defined(__x86_64__)
    __asm__ __volatile__("lock; addl $0, 0(%%rsp)" : : : "memory");
#else
    __asm__ __volatile__("lock; addl $0, 0(%%esp)" : : : "memory");
#endif
}

void arch_dma_flush(const void *p, size_t len)
{
    uintptr_t at, end;

    if (p == NULL || len == 0) {
        return;
    }
    if (have_clflush < 0) {
        clflush_detect();
    }
    if (!have_clflush) {
        __asm__ __volatile__("wbinvd" : : : "memory");
        return;
    }
    at = (uintptr_t)p & ~(uintptr_t)(clflush_size - 1u);
    end = (uintptr_t)p + len;
    while (at < end) {
        /* clflush (%eax) as bytes 0f ae 38: tcc has no mnemonic for it. */
        __asm__ __volatile__(".byte 0x0f, 0xae, 0x38" : : "a"(at) : "memory");
        at += clflush_size;
    }
    arch_dma_fence();
}
