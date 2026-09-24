/*
 * fpu.c: the floating-point state of a process, as fpu.h describes it.
 */

#include "fpu.h"

#include "../cmdline.h"
#include "../klog.h"
#include "../lib/kcpu.h"
#include "../lib/kstring.h"

#define CR0_MP         0x00000002u
#define CR0_EM         0x00000004u
#define CR0_TS         0x00000008u
#define CR0_NE         0x00000020u
#define CR4_OSFXSR     0x00000200u
#define CR4_OSXMMEXCPT 0x00000400u

#define CPUID_EDX_FXSR (1u << 24)
#define CPUID_EDX_SSE  (1u << 25)

/* The state a process starts with: written once at boot from the unit
 * itself after `fninit`, so the tag word, the opcode and the pointers are
 * whatever the processor makes them and not a guess. */
static uint8_t initial_state[FPU_STATE_SIZE] __attribute__((aligned(16)));
/* An aligned buffer for an area that is not: `fxsave` faults on one. */
static uint8_t scratch[FPU_STATE_SIZE] __attribute__((aligned(16)));
static int use_fxsr;
static int have_sse;                  /* MXCSR and the XMM registers exist and are enabled */
static uint32_t mxcsr_mask = 0xFFBFu; /* what `fxsave` reports, or the default */

static uint32_t irq_save(void)
{
    uint32_t flags;
    __asm__ __volatile__("pushfl; popl %0; cli" : "=r"(flags)::"memory");
    return flags;
}

static void irq_restore(uint32_t flags)
{
    if (flags & 0x200) {
        __asm__ __volatile__("sti");
    }
}

/**
 * Save the unit's state into `area`, which is aligned to 16 bytes.
 *
 * `fnsave` leaves the unit initialised, so in the x87 format the state is
 * loaded back: the caller asked for a copy, not a reset.
 */
static void save_aligned(void *area)
{
    if (use_fxsr) {
        __asm__ __volatile__("fxsave (%0)" : : "r"(area) : "memory");
    } else {
        __asm__ __volatile__("fnsave (%0); frstor (%0)" : : "r"(area) : "memory");
    }
}

/**
 * Load the unit's state from `area`, which is aligned to 16 bytes.
 *
 * In the x87 format `frstor` raises the exception the unit has pending, in
 * the kernel, so `fnclex` clears it first. The image's own pending bits are
 * loaded with it and are raised by the process's next x87 instruction, where
 * they belong.
 */
static void restore_aligned(const void *area)
{
    if (use_fxsr) {
        __asm__ __volatile__("fxrstor (%0)" : : "r"(area) : "memory");
    } else {
        __asm__ __volatile__("fnclex; frstor (%0)" : : "r"(area) : "memory");
    }
}

static int aligned(const void *p)
{
    return ((uint32_t)p & 15u) == 0;
}

void fpu_init(void)
{
    uint32_t a = 0, d = 0;
    uint32_t cr0, cr4;

    kcpu_id(0, &a, 0, 0, 0);
    if (a >= 1) {
        kcpu_id(1, 0, 0, 0, &d);
    }
    use_fxsr = (d & CPUID_EDX_FXSR) != 0;
    if (use_fxsr && kcmdline_has("rach2.nofxsr")) {
        use_fxsr = 0;
    }
    /* A processor can have FXSR without SSE, the Pentium II for one; the
     * SSE instructions and CR4.OSFXSR are for a processor that has both. */
    have_sse = use_fxsr && (d & CPUID_EDX_SSE) != 0;

    __asm__ __volatile__("mov %%cr0, %0" : "=r"(cr0));
    cr0 = (cr0 | CR0_MP | CR0_NE) & ~(CR0_EM | CR0_TS);
    __asm__ __volatile__("mov %0, %%cr0" : : "r"(cr0));
    /* The loader's CR4 is not promised either way; set the two bits when the
     * XMM state is saved, and clear them when it is not, so an SSE
     * instruction is an invalid opcode wherever its registers would leak. */
    __asm__ __volatile__("mov %%cr4, %0" : "=r"(cr4));
    if (have_sse) {
        cr4 |= CR4_OSFXSR | CR4_OSXMMEXCPT;
    } else {
        cr4 &= ~(CR4_OSFXSR | CR4_OSXMMEXCPT);
    }
    __asm__ __volatile__("mov %0, %%cr4" : : "r"(cr4));

    __asm__ __volatile__("fninit");
    k_memset(initial_state, 0, sizeof(initial_state));
    if (have_sse) {
        uint32_t mxcsr = 0x1F80u;

        /* `ldmxcsr (%eax)` as bytes: the staged TinyCC's assembler has
         * `fxsave` and `fxrstor` but not this one. */
        __asm__ __volatile__(".byte 0x0f, 0xae, 0x10" : : "a"(&mxcsr) : "memory");
    }
    if (use_fxsr) {
        uint32_t reported;

        __asm__ __volatile__("fxsave (%0)" : : "r"(initial_state) : "memory");
        k_memcpy(&reported, initial_state + 28, sizeof(reported));
        if (reported != 0) {
            mxcsr_mask = reported;
        }
    } else {
        __asm__ __volatile__("fnsave (%0); frstor (%0)" : : "r"(initial_state) : "memory");
    }
    klog(KLOG_DEBUG, "fpu", "fxsr: %s, sse: %s", use_fxsr ? "on" : "off", have_sse ? "on" : "off");
}

void fpu_save(void *area)
{
    uint32_t flags;

    if (aligned(area)) {
        save_aligned(area);
        return;
    }
    flags = irq_save();
    save_aligned(scratch);
    k_memcpy(area, scratch, FPU_STATE_SIZE);
    irq_restore(flags);
}

void fpu_restore(const void *area)
{
    uint32_t flags;

    if (aligned(area)) {
        restore_aligned(area);
        return;
    }
    flags = irq_save();
    k_memcpy(scratch, area, FPU_STATE_SIZE);
    restore_aligned(scratch);
    irq_restore(flags);
}

void fpu_init_area(void *area)
{
    k_memcpy(area, initial_state, FPU_STATE_SIZE);
}

void fpu_reset(void)
{
    restore_aligned(initial_state);
}

int fpu_check_area(const void *area)
{
    uint32_t mxcsr;

    if (!use_fxsr) {
        return 0;
    }
    k_memcpy(&mxcsr, (const uint8_t *)area + 24, sizeof(mxcsr));
    return (mxcsr & ~mxcsr_mask) ? -1 : 0;
}
