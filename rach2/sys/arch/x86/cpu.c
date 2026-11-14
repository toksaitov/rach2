/*
 * cpu.c: the processor primitives of ../arch.h on an x86, 32-bit or 64-bit.
 *
 * Each is one instruction or a short run of them. The same source builds for
 * i686 and x86-64: the operand of a control register move is as wide as a
 * pointer, and the few whose instructions differ by mode are written twice,
 * under `#if defined(__x86_64__)`.
 */

#include "exits.h"
#include "kcpu.h"
#include "kvmclock.h"

#include "../arch.h"
#include "../../cmdline.h"

void arch_irq_enable(void)
{
    __asm__ __volatile__("sti" : : : "memory");
}

void arch_irq_disable(void)
{
    __asm__ __volatile__("cli" : : : "memory");
}

void arch_invalid_opcode(void)
{
    __asm__ __volatile__(".byte 0x0f, 0x0b"); /* ud2 */
}

/** `sti` takes effect after the instruction that follows it, so an interrupt
 * pending at the `sti` is taken once `hlt` has begun waiting, and wakes it. */
void arch_idle_masked(void)
{
    exits_count(EXIT_HLT, 0);
    __asm__ __volatile__("sti; hlt\n"
                         ".globl arch_idle_resume\n"
                         "arch_idle_resume:\n"
                         "\tcli"
                         :
                         :
                         : "memory");
}

/* KVM's hint that each of the guest's processors has a host processor of
 * its own, bit 0 of leaf 0x40000001's EDX (KVM_HINTS_REALTIME,
 * arch/x86/include/uapi/asm/kvm_para.h:40). */
#define KVM_HINTS_REALTIME (1u << 0)

/* arch_idle_poll's answer: -1 until read, then 0 or 1. Every processor
 * reads the same, so two that read it at once store the same. */
static volatile int idle_poll = -1;

/** Without the hint KVM's processors share the host's, and the host's own
 * poll before it sleeps a halted processor (halt_poll_ns) answers a quick
 * wake; a poll in the guest then costs host time and, here, the exits of
 * its reads of the APIC (`arch_irq_waiting`). */
int arch_idle_poll(void)
{
    if (idle_poll < 0) {
        idle_poll = (kcpu_kvm_hints() & KVM_HINTS_REALTIME) != 0 ||
                    kcmdline_param_bool("cpuidle_haltpoll.force", 0);
    }
    return idle_poll;
}

/** The processor takes its clock page back from KVM first
 * (`kvmclock_cpu_stop`), as Linux's kvm_guest_cpu_offline does for a
 * processor that goes offline or stops at a shutdown, so that KVM writes
 * no more into memory the processor has left. */
void arch_stop(void)
{
    __asm__ __volatile__("cli" : : : "memory");
    kvmclock_cpu_stop();
    for (;;) {
        __asm__ __volatile__("cli; hlt" : : : "memory");
    }
}

void arch_relax(void)
{
    __asm__ __volatile__("pause" : : : "memory");
}

/** An `iret` to the next instruction, which serializes on every x86, the
 * ones without `cpuid` among them, as Linux's iret_to_self; it restores the
 * flags it pushed, interrupts with them. */
void arch_sync_core(void)
{
#if defined(__x86_64__)
    unsigned long tmp;

    __asm__ __volatile__("mov %%ss, %0\n\t"
                         "pushq %0\n\t"
                         "pushq %%rsp\n\t"
                         "addq $8, (%%rsp)\n\t"
                         "pushfq\n\t"
                         "mov %%cs, %0\n\t"
                         "pushq %0\n\t"
                         "leaq 1f(%%rip), %0\n\t"
                         "pushq %0\n\t"
                         "iretq\n"
                         "1:"
                         : "=&r"(tmp)
                         :
                         : "cc", "memory");
#else
    __asm__ __volatile__("pushfl\n\t"
                         "pushl %%cs\n\t"
                         "pushl $1f\n\t"
                         "iret\n"
                         "1:"
                         :
                         :
                         : "cc", "memory");
#endif
}

/** `xchg` with a memory operand locks the bus by itself, and a locked
 * instruction is a full barrier on x86. */
unsigned int arch_atomic_xchg(volatile unsigned int *p, unsigned int value)
{
    __asm__ __volatile__("xchgl %0, %1" : "+r"(value), "+m"(*p) : : "memory");
    return value;
}

/** `cmpxchg` compares `*p` with EAX, stores `value` when they are equal, and
 * leaves what was there in EAX; the lock prefix makes it one step. */
unsigned int arch_atomic_cmpxchg(volatile unsigned int *p, unsigned int old, unsigned int value)
{
    __asm__ __volatile__("lock; cmpxchgl %2, %1"
                         : "+a"(old), "+m"(*p)
                         : "r"(value)
                         : "memory", "cc");
    return old;
}

/** The exchange of `arch_atomic_xchg` at the width of a pointer: `xchgq` on
 * x86-64 and `xchgl` on i686. */
uintptr_t arch_atomic_xchg_ptr(volatile uintptr_t *p, uintptr_t value)
{
#if defined(__x86_64__)
    __asm__ __volatile__("xchgq %0, %1" : "+r"(value), "+m"(*p) : : "memory");
#else
    __asm__ __volatile__("xchgl %0, %1" : "+r"(value), "+m"(*p) : : "memory");
#endif
    return value;
}

/** The compare-and-exchange of `arch_atomic_cmpxchg` at the width of a
 * pointer. */
uintptr_t arch_atomic_cmpxchg_ptr(volatile uintptr_t *p, uintptr_t old, uintptr_t value)
{
#if defined(__x86_64__)
    __asm__ __volatile__("lock; cmpxchgq %2, %1"
                         : "+a"(old), "+m"(*p)
                         : "r"(value)
                         : "memory", "cc");
#else
    __asm__ __volatile__("lock; cmpxchgl %2, %1"
                         : "+a"(old), "+m"(*p)
                         : "r"(value)
                         : "memory", "cc");
#endif
    return old;
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

/* The string moves of `arch_user_copy` that touch user memory, and where a
 * fault in each resumes (usercopy.s of i686 and x86_64). */
extern const char arch_user_copy_words[], arch_user_copy_words_fault[];
extern const char arch_user_copy_bytes[], arch_user_copy_bytes_fault[];

int arch_user_copy_fixup(isr_params_t *p)
{
    uintptr_t pc = (uintptr_t)FRAME_PC(p);

    if (pc == (uintptr_t)arch_user_copy_words) {
        FRAME_PC(p) = (uintptr_t)arch_user_copy_words_fault;
        return 1;
    }
    if (pc == (uintptr_t)arch_user_copy_bytes) {
        FRAME_PC(p) = (uintptr_t)arch_user_copy_bytes_fault;
        return 1;
    }
    return 0;
}

void arch_copy_words(void *dst, const void *src, size_t words)
{
#if defined(__x86_64__)
    __asm__ __volatile__("cld; rep movsq" : "+S"(src), "+D"(dst), "+c"(words) : : "memory", "cc");
#else
    __asm__ __volatile__("cld; rep movsl" : "+S"(src), "+D"(dst), "+c"(words) : : "memory", "cc");
#endif
}

/* The direction flag is set only with interrupts masked: the i686 entry does
 * not clear it, and a handler's compiled string instructions assume it clear.
 * popf puts both flags back. */
void arch_copy_words_down(void *dst, const void *src, size_t words)
{
#if defined(__x86_64__)
    __asm__ __volatile__("pushfq; cli; std; rep movsq; popfq"
                         : "+S"(src), "+D"(dst), "+c"(words)
                         :
                         : "memory", "cc");
#else
    __asm__ __volatile__("pushfl; cli; std; rep movsl; popfl"
                         : "+S"(src), "+D"(dst), "+c"(words)
                         :
                         : "memory", "cc");
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
 * (leaf 1, ECX bit 30): -1 until asked, then 0 or 1. */
static int have_rdseed = -1;
static int have_rdrand = -1;

/**
 * Set `have_rdseed` and `have_rdrand` from `cpuid`. A leaf above the highest
 * the processor has returns another leaf's data, which could advertise a bit
 * it does not have, and the instruction would then raise #UD, so each leaf
 * is asked only when the highest covers it.
 */
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

/* Whether the processor has CLFLUSH (CPUID leaf 1, EDX bit 19), -1 until
 * asked, and the bytes of the line it flushes. Without it `arch_dma_flush`
 * writes back the whole cache. */
static int have_clflush = -1;
static uint32_t clflush_size = 64;

/** Set `have_clflush` and `clflush_size` from `cpuid` leaf 1: the line size
 * is EBX bits 8 to 15 in units of 8 bytes, or 64 when they are zero. */
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

void arch_dma_flush(const volatile void *p, size_t len)
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

void arch_dma_flush_all(void)
{
    __asm__ __volatile__("wbinvd" : : : "memory");
}
