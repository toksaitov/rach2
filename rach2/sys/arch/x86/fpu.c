/*
 * fpu.c: the floating-point state of a process, as fpu.h describes it.
 */

#include "fpu.h"

#include "../../cmdline.h"
#include "../../klog.h"
#include "kcpu.h"
#include "../../lib/kstring.h"

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

/* Byte offsets in the `fxsave` image. */
#define FX_CWD   0
#define FX_SWD   2
#define FX_TWD   4
#define FX_FOP   6
#define FX_FIP   8
#define FX_FCS   12
#define FX_FOO   16
#define FX_FOS   20
#define FX_MXCSR 24
#define FX_ST    32

/* Byte offsets in the `fnsave` layout that opens `_fpstate_32`. */
#define FN_CWD    0
#define FN_SWD    4
#define FN_TWD    8
#define FN_FIP    12
#define FN_FCS    16
#define FN_FOO    20
#define FN_FOS    24
#define FN_ST     28
#define LX_STATUS 108
#define LX_MAGIC  110
#define LX_FXSR   112

static uint16_t get16(const uint8_t *p)
{
    return (uint16_t)(p[0] | (p[1] << 8));
}

static uint32_t get32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void put16(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

static void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

/* The full x87 tag word from the abridged one `fxsave` keeps: two bits per
 * physical register, classified from the register's contents. */
static uint32_t full_tag(const uint8_t *fx)
{
    uint32_t top = ((uint32_t)get16(fx + FX_SWD) >> 11) & 7u;
    uint32_t abridged = fx[FX_TWD];
    uint32_t tags = 0xFFFF0000u;
    uint32_t i;

    for (i = 0; i < 8; i++) {
        uint32_t tag = 3; /* empty */

        if (abridged & (1u << i)) {
            const uint8_t *st = fx + FX_ST + ((i - top) & 7u) * 16u;
            uint32_t exponent = get16(st + 8) & 0x7FFFu;

            if (exponent == 0x7FFFu) {
                tag = 2; /* special */
            } else if (exponent == 0) {
                tag = (get32(st) == 0 && get32(st + 4) == 0) ? 1u : 2u;
            } else {
                tag = (st[7] & 0x80) ? 0u : 2u;
            }
        }
        tags |= tag << (2 * i);
    }
    return tags;
}

/* The abridged tag byte from a full tag word: a register is in use unless its
 * two bits say empty. */
static uint32_t abridged_tag(uint32_t tags)
{
    uint32_t abridged = 0;
    uint32_t i;

    for (i = 0; i < 8; i++) {
        if (((tags >> (2 * i)) & 3u) != 3u) {
            abridged |= 1u << i;
        }
    }
    return abridged;
}

void fpu_to_linux(const void *area, uint8_t *out)
{
    const uint8_t *fx = (const uint8_t *)area;
    uint32_t i;

    k_memset(out, 0, FPU_LINUX_SIZE);
    if (!use_fxsr) {
        k_memcpy(out, area, 108);
        put16(out + LX_STATUS, get32(out + FN_SWD));
        put16(out + LX_MAGIC, 0xFFFFu);
        return;
    }
    put32(out + FN_CWD, get16(fx + FX_CWD) | 0xFFFF0000u);
    put32(out + FN_SWD, get16(fx + FX_SWD) | 0xFFFF0000u);
    put32(out + FN_TWD, full_tag(fx));
    put32(out + FN_FIP, get32(fx + FX_FIP));
    put32(out + FN_FCS, get16(fx + FX_FCS) | ((uint32_t)get16(fx + FX_FOP) << 16));
    put32(out + FN_FOO, get32(fx + FX_FOO));
    put32(out + FN_FOS, get16(fx + FX_FOS) | 0xFFFF0000u);
    for (i = 0; i < 8; i++) {
        k_memcpy(out + FN_ST + i * 10u, fx + FX_ST + i * 16u, 10);
    }
    put16(out + LX_STATUS, get16(fx + FX_SWD));
    put16(out + LX_MAGIC, 0);
    k_memcpy(out + LX_FXSR, area, FPU_STATE_SIZE);
}

int fpu_from_linux(const uint8_t *in, void *area)
{
    uint8_t *fx = (uint8_t *)area;
    uint32_t i;

    if (!use_fxsr) {
        k_memcpy(area, in, 108);
        return 0;
    }
    k_memcpy(area, in + LX_FXSR, FPU_STATE_SIZE);
    if (get32(fx + FX_MXCSR) & ~mxcsr_mask) {
        return -1;
    }
    put16(fx + FX_CWD, get32(in + FN_CWD));
    put16(fx + FX_SWD, get32(in + FN_SWD));
    fx[FX_TWD] = (uint8_t)abridged_tag(get32(in + FN_TWD));
    put16(fx + FX_FOP, get32(in + FN_FCS) >> 16);
    put32(fx + FX_FIP, get32(in + FN_FIP));
    put32(fx + FX_FCS, get32(in + FN_FCS) & 0xFFFFu);
    put32(fx + FX_FOO, get32(in + FN_FOO));
    put32(fx + FX_FOS, get32(in + FN_FOS) & 0xFFFFu);
    for (i = 0; i < 8; i++) {
        k_memcpy(fx + FX_ST + i * 16u, in + FN_ST + i * 10u, 10);
    }
    return 0;
}
