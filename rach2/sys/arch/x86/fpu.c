/*
 * fpu.c: the floating-point state of a process, as fpu.h describes it.
 */

#include "fpu.h"

#include "../../cmdline.h"
#include "../../klog.h"
#include "kcpu.h"
#include "../../lib/kstring.h"
#include "../../mm/kheap.h"
#include "../../mm/usercopy.h"

#define CR0_MP         0x00000002u
#define CR0_EM         0x00000004u
#define CR0_TS         0x00000008u
#define CR0_NE         0x00000020u
#define CR4_OSFXSR     0x00000200u
#define CR4_OSXMMEXCPT 0x00000400u
#define CR4_OSXSAVE    0x00040000u

#define CPUID_EDX_FXSR     (1u << 24)
#define CPUID_EDX_SSE      (1u << 25)
#define CPUID_ECX_XSAVE    (1u << 26)
#define CPUID_ECX_AVX      (1u << 28)
#define CPUID7_EBX_AVX512F (1u << 16)

/* The leaf of `cpuid` that describes the XSAVE components: subleaf 0 has
 * the ones the processor supports, and subleaf i the size and the offset of
 * component i in the standard format. */
#define CPUID_XSTATE 0xDu

/* The XSAVE components by their bits in XCR0 and XSTATE_BV: x87, SSE, the
 * upper halves of the YMM registers, and AVX-512's opmask registers, upper
 * halves of ZMM0 to ZMM15, and ZMM16 to ZMM31. */
#define XFEATURE_X87     0x01u
#define XFEATURE_SSE     0x02u
#define XFEATURE_YMM     0x04u
#define XFEATURES_AVX512 0xE0u
#define XFEATURE_COUNT   8

/* Byte offsets in the `fxsave` image, which is also the XSAVE image's
 * legacy part: the x87 environment, MXCSR, the x87 and XMM registers, the
 * reserved bytes, and the 48 the processor leaves to software. Then the
 * XSAVE header, whose first eight bytes are XSTATE_BV, and the extended
 * part. */
#define FX_CWD    0
#define FX_SWD    2
#define FX_TWD    4
#define FX_FOP    6
#define FX_FIP    8
#define FX_FCS    12
#define FX_FOO    16
#define FX_FOS    20
#define FX_MXCSR  24
#define FX_ST     32
#define FX_XMM    160
#define FX_PAD    416
#define FX_SW     464
#define XSAVE_HDR 512
#define XSAVE_EXT 576

/* Linux's marks of an XSAVE image in a signal frame (`sigcontext.h`):
 * "FPXS" in the software bytes, and "FPXE" in the four bytes after the
 * image. */
#define FP_XSTATE_MAGIC1 0x46505853u
#define FP_XSTATE_MAGIC2 0x46505845u

/* The bytes of a frame's state before its `fxsave` or XSAVE image: none on
 * x86-64; on i686 the x87 part, the status word and the magic word of
 * `struct _fpstate_32`. */
#if defined(FPU_X86_64)
#define FRAME_PREFIX 0u
#else
#define FRAME_PREFIX 112u
#define LX_STATUS    108
#define LX_MAGIC     110
#endif

/* The state a process starts with: written once at boot from the unit
 * itself, after `fninit` and, with XSAVE, an `xrstor` of an empty header,
 * so the tag word, the opcode, the pointers and the extended components are
 * whatever the processor makes them and not a guess. */
static uint8_t initial_state[FPU_AREA_MAX] __attribute__((aligned(64)));
static int use_fxsr;
static int have_sse;       /* MXCSR and the XMM registers exist and are enabled */
static int use_xsave;      /* the area is the XSAVE image of `xfeatures` */
static uint64_t xfeatures; /* XCR0 */
static uint32_t area_size = FX_SIZE;
static uint32_t mxcsr_mask = 0xFFBFu; /* what `fxsave` reports, or the default */
/* Each XSAVE component's size and offset, from leaf 0xD. */
static uint32_t comp_size[XFEATURE_COUNT];
static uint32_t comp_offset[XFEATURE_COUNT];

#if !defined(FPU_X86_64)
static uint32_t get16(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8);
}

static void put16(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}
#endif

static uint32_t get32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint64_t get64(const uint8_t *p)
{
    return (uint64_t)get32(p) | ((uint64_t)get32(p + 4) << 32);
}

static void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static void put64(uint8_t *p, uint64_t v)
{
    put32(p, (uint32_t)v);
    put32(p + 4, (uint32_t)(v >> 32));
}

/** The bytes of a standard-format image of the components `mask` names:
 * the end of the last of them, and at least the legacy part and header. */
static uint32_t xstate_end(uint64_t mask)
{
    uint32_t end = XSAVE_EXT;
    int i;

    for (i = 2; i < XFEATURE_COUNT; i++) {
        if (((mask >> i) & 1u) && comp_offset[i] + comp_size[i] > end) {
            end = comp_offset[i] + comp_size[i];
        }
    }
    return end;
}

size_t fpu_area_size(void)
{
    return area_size;
}

/**
 * The heap aligns a block to 16 bytes at most, so the area is taken 64
 * bytes longer and moved up to the next 64-byte boundary, and the byte
 * before it says by how much, for `fpu_area_free`.
 */
void *fpu_area_alloc(void)
{
    uint8_t *raw = kmalloc(area_size + 64u);
    uint8_t *area;

    if (!raw) {
        return NULL;
    }
    area = (uint8_t *)(((uintptr_t)raw + 64u) & ~(uintptr_t)63u);
    area[-1] = (uint8_t)(area - raw);
    fpu_init_area(area);
    return area;
}

void fpu_area_free(void *area)
{
    uint8_t *a = area;

    if (a) {
        kfree(a - a[-1]);
    }
}

void fpu_init_area(void *area)
{
    k_memcpy(area, initial_state, area_size);
}

int fpu_has_fxsr(void)
{
    return use_fxsr;
}

uint32_t fpu_xstate_size(void)
{
    return use_xsave ? area_size : 0u;
}

/** Mark the components `bits` in use in the XSAVE header of `area`, as
 * Linux does once a tracer has written their part of the legacy image. */
static void mark_in_use(uint8_t *area, uint64_t bits)
{
    if (use_xsave) {
        put64(area + XSAVE_HDR, get64(area + XSAVE_HDR) | bits);
    }
}

void fpu_fx_get(const void *area, uint8_t *fx)
{
    k_memcpy(fx, area, FX_PAD);
    k_memset(fx + FX_PAD, 0, FX_SIZE - FX_PAD);
}

int fpu_fx_set(void *area, const uint8_t *fx)
{
    if (get32(fx + FX_MXCSR) & ~mxcsr_mask) {
        return -1;
    }
    k_memcpy(area, fx, FX_PAD);
    mark_in_use(area, XFEATURE_X87 | XFEATURE_SSE);
    return 0;
}

void fpu_xstate_view(uint8_t *copy)
{
    k_memset(copy + FX_PAD, 0, FX_SIZE - FX_PAD);
    put64(copy + FX_SW, xfeatures);
}

/** Whether the XSAVE header of the image `img` is one the standard form of
 * `xrstor` takes: XSTATE_BV within XCR0, and the rest of it zero, which
 * Linux requires of the reserved bytes as well. */
static int header_ok(const uint8_t *img)
{
    int i;

    if (get64(img + XSAVE_HDR) & ~xfeatures) {
        return 0;
    }
    for (i = XSAVE_HDR + 8; i < XSAVE_EXT; i++) {
        if (img[i] != 0) {
            return 0;
        }
    }
    return 1;
}

int fpu_xstate_set(void *area, const uint8_t *in)
{
    uint8_t *a = area;
    uint64_t bv = get64(in + XSAVE_HDR);
    int i;

    if (!header_ok(in) || (get32(in + FX_MXCSR) & ~mxcsr_mask)) {
        return -1;
    }
    k_memcpy(a, initial_state, area_size);
    if (bv & XFEATURE_X87) {
        k_memcpy(a, in, FX_MXCSR);
        k_memcpy(a + FX_ST, in + FX_ST, FX_XMM - FX_ST);
    }
    k_memcpy(a + FX_MXCSR, in + FX_MXCSR, FX_ST - FX_MXCSR);
    if (bv & XFEATURE_SSE) {
        k_memcpy(a + FX_XMM, in + FX_XMM, FX_PAD - FX_XMM);
    }
    for (i = 2; i < XFEATURE_COUNT; i++) {
        if ((bv >> i) & 1u) {
            k_memcpy(a + comp_offset[i], in + comp_offset[i], comp_size[i]);
        }
    }
    put64(a + XSAVE_HDR, bv);
    return 0;
}

#if defined(FPU_X86_64)

/** The frame's state without XSAVE, `struct _fpstate_64`: the image, with
 * the software bytes zero, so that no XSAVE image is announced. */
static void to_linux(const void *area, uint8_t *out)
{
    k_memcpy(out, area, FPU_LINUX_SIZE);
    k_memset(out + FX_SW, 0, FX_SIZE - FX_SW);
}

/** An area from a frame's `struct _fpstate_64` without XSAVE; 0, or -1 for
 * an image `fxrstor` would fault on. */
static int from_linux(const uint8_t *in, void *area)
{
    k_memcpy(area, in, FPU_LINUX_SIZE);
    return (get32(in + FX_MXCSR) & ~mxcsr_mask) ? -1 : 0;
}

#else

/* Byte offsets in the `fnsave` layout of `user_i387_struct`, which opens
 * `_fpstate_32`. */
#define FN_CWD 0
#define FN_SWD 4
#define FN_TWD 8
#define FN_FIP 12
#define FN_FCS 16
#define FN_FOO 20
#define FN_FOS 24
#define FN_ST  28

/* The full x87 tag word from the abridged one `fxsave` keeps: two bits per
 * physical register, classified from the register's contents. */
static uint32_t full_tag(const uint8_t *fx)
{
    uint32_t top = (get16(fx + FX_SWD) >> 11) & 7u;
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

void fpu_env_get(const void *area, uint8_t *env)
{
    const uint8_t *fx = area;
    uint32_t i;

    if (!use_fxsr) {
        k_memcpy(env, area, FPU_ENV_SIZE);
        return;
    }
    put32(env + FN_CWD, get16(fx + FX_CWD) | 0xFFFF0000u);
    put32(env + FN_SWD, get16(fx + FX_SWD) | 0xFFFF0000u);
    put32(env + FN_TWD, full_tag(fx));
    put32(env + FN_FIP, get32(fx + FX_FIP));
    put32(env + FN_FCS, get16(fx + FX_FCS) | (get16(fx + FX_FOP) << 16));
    put32(env + FN_FOO, get32(fx + FX_FOO));
    put32(env + FN_FOS, get16(fx + FX_FOS) | 0xFFFF0000u);
    for (i = 0; i < 8; i++) {
        k_memcpy(env + FN_ST + i * 10u, fx + FX_ST + i * 16u, 10);
    }
}

/** Fold the `user_i387_struct` `env` into the x87 part of the `fxsave`
 * image `fx`, as Linux's `convert_to_fxsr` does. */
static void env_to_fx(const uint8_t *env, uint8_t *fx)
{
    uint32_t i;

    put16(fx + FX_CWD, get32(env + FN_CWD));
    put16(fx + FX_SWD, get32(env + FN_SWD));
    fx[FX_TWD] = (uint8_t)abridged_tag(get32(env + FN_TWD));
    put16(fx + FX_FOP, get32(env + FN_FCS) >> 16);
    put32(fx + FX_FIP, get32(env + FN_FIP));
    put32(fx + FX_FCS, get32(env + FN_FCS) & 0xFFFFu);
    put32(fx + FX_FOO, get32(env + FN_FOO));
    put32(fx + FX_FOS, get32(env + FN_FOS) & 0xFFFFu);
    for (i = 0; i < 8; i++) {
        k_memcpy(fx + FX_ST + i * 16u, env + FN_ST + i * 10u, 10);
    }
}

void fpu_env_set(void *area, const uint8_t *env)
{
    if (!use_fxsr) {
        k_memcpy(area, env, FPU_ENV_SIZE);
        return;
    }
    env_to_fx(env, area);
    mark_in_use(area, XFEATURE_X87);
}

/**
 * The frame's state without XSAVE, `struct _fpstate_32`. In both formats
 * bytes 108 and 109 are the x87 status word. In the FXSR format the first
 * 108 bytes are the x87 part converted from the image, the magic word is 0,
 * and the image follows at byte 112, its software bytes zero. In the x87
 * format the first 108 bytes are the `fnsave` image, the magic word is
 * 0xFFFF, and bytes 112 to 623 are zero.
 */
static void to_linux(const void *area, uint8_t *out)
{
    k_memset(out, 0, FPU_LINUX_SIZE);
    fpu_env_get(area, out);
    if (!use_fxsr) {
        put16(out + LX_STATUS, get32(out + FN_SWD));
        put16(out + LX_MAGIC, 0xFFFFu);
        return;
    }
    put16(out + LX_STATUS, get16((const uint8_t *)area + FX_SWD));
    put16(out + LX_MAGIC, 0);
    k_memcpy(out + FRAME_PREFIX, area, FX_SW);
}

/**
 * An area from a frame's `struct _fpstate_32` without XSAVE: the SSE state
 * from the `fxsave` image and the x87 state from the 108-byte part, which a
 * handler may have changed. 0, or -1 for an image `fxrstor` would fault on.
 */
static int from_linux(const uint8_t *in, void *area)
{
    if (!use_fxsr) {
        k_memcpy(area, in, FPU_ENV_SIZE);
        return 0;
    }
    k_memcpy(area, in + FRAME_PREFIX, FX_SIZE);
    if (get32((const uint8_t *)area + FX_MXCSR) & ~mxcsr_mask) {
        return -1;
    }
    env_to_fx(in, area);
    return 0;
}

#endif

uintptr_t fpu_frame_place(uintptr_t sp)
{
    if (use_xsave) {
        return ((sp - area_size - 4u) & ~(uintptr_t)63u) - FRAME_PREFIX;
    }
    return (sp - FPU_LINUX_SIZE) & ~(uintptr_t)63u;
}

uintptr_t fpu_frame_bytes(void)
{
    if (use_xsave) {
        return area_size + 4u + 63u + FRAME_PREFIX;
    }
    return FPU_LINUX_SIZE + 63u;
}

/** Write the software bytes `sw` of a frame's XSAVE image, Linux's
 * `struct _fpx_sw_bytes`. */
static void sw_bytes_put(uint8_t *sw)
{
    k_memset(sw, 0, FX_SIZE - FX_SW);
    put32(sw, FP_XSTATE_MAGIC1);
    put32(sw + 4, FRAME_PREFIX + area_size + 4u);
    put64(sw + 8, xfeatures);
    put32(sw + 16, area_size);
}

/** The frame's state without XSAVE. */
static int frame_put_legacy(uintptr_t fp, const void *area)
{
    uint8_t img[FPU_LINUX_SIZE];

    to_linux(area, img);
    return copy_to_user((void __user *)fp, img, sizeof(img)) == 0 ? 0 : -1;
}

int fpu_frame_put(uintptr_t fp, const void *area)
{
    const uint8_t *a = area;
    uint8_t head[FRAME_PREFIX + XSAVE_EXT];
    uint8_t *img = head + FRAME_PREFIX;
    uint8_t magic2[4];

    if (!use_xsave) {
        return frame_put_legacy(fp, area);
    }
#if !defined(FPU_X86_64)
    fpu_env_get(area, head);
    put16(head + LX_STATUS, get16(a + FX_SWD));
    put16(head + LX_MAGIC, 0);
#endif
    k_memcpy(img, a, XSAVE_EXT);
    k_memset(img + FX_PAD, 0, FX_SW - FX_PAD);
    sw_bytes_put(img + FX_SW);
    put64(img + XSAVE_HDR, get64(img + XSAVE_HDR) | XFEATURE_X87 | XFEATURE_SSE);
    put32(magic2, FP_XSTATE_MAGIC2);
    if (copy_to_user((void __user *)fp, head, sizeof(head)) != 0 ||
        copy_to_user((void __user *)(fp + sizeof(head)), a + XSAVE_EXT, area_size - XSAVE_EXT) !=
            0 ||
        copy_to_user((void __user *)(fp + FRAME_PREFIX + area_size), magic2, sizeof(magic2)) != 0) {
        return -1;
    }
    return 0;
}

/**
 * The size of the XSAVE image the software bytes `sw` of a frame announce,
 * or 0 for none: Linux's `check_xstate_in_sigframe` before it looks for
 * FP_XSTATE_MAGIC2. The image must have at least the legacy part and the
 * header, at most this kernel's components, and fit the size the frame
 * says it has.
 */
static uint32_t sw_bytes_size(const uint8_t *sw)
{
    uint32_t size = get32(sw + 16);

    if (get32(sw) != FP_XSTATE_MAGIC1 || size < XSAVE_EXT || size > area_size ||
        size > get32(sw + 4)) {
        return 0;
    }
    return size;
}

/** The state of a frame without XSAVE. */
static int frame_take_legacy(uintptr_t fp, void *area)
{
    uint8_t img[FPU_LINUX_SIZE];

    if (copy_from_user(img, (const void __user *)fp, sizeof(img)) != 0) {
        return -1;
    }
    return from_linux(img, area);
}

int fpu_frame_take(uintptr_t fp, void *area)
{
    uint8_t *a = area;
    uintptr_t image = fp + FRAME_PREFIX;
    uint32_t size, magic2 = 0;
    uint64_t bv = XFEATURE_X87 | XFEATURE_SSE;
    uint64_t rfbm;
    int i;
#if !defined(FPU_X86_64)
    uint8_t env[FPU_ENV_SIZE];
#endif

    if (!use_xsave) {
        return frame_take_legacy(fp, area);
    }
#if !defined(FPU_X86_64)
    if (copy_from_user(env, (const void __user *)fp, sizeof(env)) != 0) {
        return -1;
    }
#endif
    if (copy_from_user(a, (const void __user *)image, FX_SIZE) != 0) {
        return -1;
    }
    size = sw_bytes_size(a + FX_SW);
    if (size != 0 && copy_from_user(&magic2, (const void __user *)(image + size), 4) != 0) {
        return -1;
    }
    k_memset(a + XSAVE_HDR, 0, XSAVE_EXT - XSAVE_HDR);
    if (size != 0 && magic2 == FP_XSTATE_MAGIC2) {
        if (copy_from_user(a + XSAVE_HDR, (const void __user *)(image + XSAVE_HDR),
                           size - XSAVE_HDR) != 0 ||
            !header_ok(a)) {
            return -1;
        }
        /* The components the frame's XCR0 names and its image holds. */
        rfbm = get64(a + FX_SW + 8) & xfeatures;
        for (i = 2; i < XFEATURE_COUNT; i++) {
            if (comp_offset[i] + comp_size[i] > size) {
                rfbm &= ~((uint64_t)1 << i);
            }
        }
        bv = get64(a + XSAVE_HDR) & rfbm;
    }
    put64(a + XSAVE_HDR, bv);
    if (get32(a + FX_MXCSR) & ~mxcsr_mask) {
        return -1;
    }
#if !defined(FPU_X86_64)
    env_to_fx(env, a);
#endif
    return 0;
}

#if !defined(RACH2_HOSTTEST)

/*
 * The rest drives the unit itself. A host test of the images above
 * (tests/host/fpuframe_hosttest.c) leaves it out and sets the format by
 * hand.
 */

/* The 64-bit forms of the save and the load on x86-64, which keep the x87
 * unit's 64-bit instruction and operand pointers; TinyCC spells them with a
 * `q` as GNU as also accepts. */
#if defined(__x86_64__)
#define FXSAVE  "fxsaveq"
#define FXRSTOR "fxrstorq"
#else
#define FXSAVE  "fxsave"
#define FXRSTOR "fxrstor"
#endif

/* `xsave (%edi)` and `xrstor (%edi)`, and with REX.W on x86-64 `xsave64
 * (%rdi)` and `xrstor64 (%rdi)`, as bytes: the staged TinyCC's assembler
 * has none of the XSAVE instructions. */
#if defined(__x86_64__)
#define XSAVE  ".byte 0x48, 0x0f, 0xae, 0x27"
#define XRSTOR ".byte 0x48, 0x0f, 0xae, 0x2f"
#else
#define XSAVE  ".byte 0x0f, 0xae, 0x27"
#define XRSTOR ".byte 0x0f, 0xae, 0x2f"
#endif

/**
 * Save the unit's state into `area`, which is aligned to 64 bytes.
 *
 * `xsave` takes the components its mask in EDX:EAX names, all ones here,
 * which XCR0 cuts to the ones the kernel enabled. `fnsave` leaves the unit
 * initialised, so in the x87 format the state is loaded back: the caller
 * asked for a copy, not a reset.
 */
static void save_state(void *area)
{
    if (use_xsave) {
        __asm__ __volatile__(XSAVE : : "D"(area), "a"(0xFFFFFFFFu), "d"(0xFFFFFFFFu) : "memory");
    } else if (use_fxsr) {
        __asm__ __volatile__(FXSAVE " (%0)" : : "r"(area) : "memory");
    } else {
        __asm__ __volatile__("fnsave (%0); frstor (%0)" : : "r"(area) : "memory");
    }
}

/**
 * Load the unit's state from `area`, which is aligned to 64 bytes.
 *
 * `xrstor` puts each component whose XSTATE_BV bit is clear in its initial
 * configuration. In the x87 format `frstor` raises the exception the unit
 * has pending, in the kernel, so `fnclex` clears it first. The image's own
 * pending bits are loaded with it and are raised by the process's next x87
 * instruction, where they belong.
 */
static void load_state(const void *area)
{
    if (use_xsave) {
        __asm__ __volatile__(XRSTOR : : "D"(area), "a"(0xFFFFFFFFu), "d"(0xFFFFFFFFu) : "memory");
    } else if (use_fxsr) {
        __asm__ __volatile__(FXRSTOR " (%0)" : : "r"(area) : "memory");
    } else {
        __asm__ __volatile__("fnclex; frstor (%0)" : : "r"(area) : "memory");
    }
}

/** Write `v` to XCR0: `xsetbv` with ECX 0, as bytes for the same reason. */
static void xcr0_write(uint64_t v)
{
    __asm__ __volatile__(".byte 0x0f, 0x01, 0xd1"
                         :
                         : "c"(0u), "a"((uint32_t)v), "d"((uint32_t)(v >> 32)));
}

/**
 * The components to save, as Linux takes them: x87 and SSE; AVX where
 * `cpuid` reports it and leaf 0xD lists it; and AVX-512's three where leaf
 * 0xD lists all three and `cpuid` reports AVX512F. A group joins only while
 * the image still fits FPU_AREA_MAX. 0 when leaf 0xD lacks x87 or SSE.
 */
static uint64_t xstate_choose(int avx)
{
    uint32_t lo = 0, hi = 0, ebx7 = 0;
    uint64_t supported, mask = XFEATURE_X87 | XFEATURE_SSE;
    int i;

    kcpu_id_sub(CPUID_XSTATE, 0, &lo, 0, 0, &hi);
    supported = (uint64_t)lo | ((uint64_t)hi << 32);
    if ((supported & mask) != mask) {
        return 0;
    }
    for (i = 2; i < XFEATURE_COUNT; i++) {
        if ((supported >> i) & 1u) {
            kcpu_id_sub(CPUID_XSTATE, (uint32_t)i, &comp_size[i], &comp_offset[i], 0, 0);
        }
    }
    kcpu_id_sub(7, 0, 0, &ebx7, 0, 0);
    if (avx && (supported & XFEATURE_YMM) && xstate_end(mask | XFEATURE_YMM) <= FPU_AREA_MAX) {
        mask |= XFEATURE_YMM;
    }
    if ((mask & XFEATURE_YMM) && (ebx7 & CPUID7_EBX_AVX512F) &&
        (supported & XFEATURES_AVX512) == XFEATURES_AVX512 &&
        xstate_end(mask | XFEATURES_AVX512) <= FPU_AREA_MAX) {
        mask |= XFEATURES_AVX512;
    }
    return mask;
}

void fpu_cpu_setup(void)
{
    uintptr_t cr0, cr4;

    __asm__ __volatile__("mov %%cr0, %0" : "=r"(cr0));
    cr0 = (cr0 | CR0_MP | CR0_NE) & ~(uintptr_t)(CR0_EM | CR0_TS);
    __asm__ __volatile__("mov %0, %%cr0" : : "r"(cr0));
    /* The loader's CR4 is not promised either way; set each bit where the
     * state it enables is saved, and clear it where it is not, so an
     * instruction whose registers would leak is an invalid opcode. */
    __asm__ __volatile__("mov %%cr4, %0" : "=r"(cr4));
    cr4 &= ~(uintptr_t)(CR4_OSFXSR | CR4_OSXMMEXCPT | CR4_OSXSAVE);
    if (have_sse) {
        cr4 |= CR4_OSFXSR | CR4_OSXMMEXCPT;
    }
    if (use_xsave) {
        cr4 |= CR4_OSXSAVE;
    }
    __asm__ __volatile__("mov %0, %%cr4" : : "r"(cr4));
    if (use_xsave) {
        xcr0_write(xfeatures);
    }
}

void fpu_init(void)
{
    uint32_t max = 0, c = 0, d = 0;

    kcpu_id(0, &max, 0, 0, 0);
    if (max >= 1) {
        kcpu_id(1, 0, 0, &c, &d);
    }
    use_fxsr = (d & CPUID_EDX_FXSR) != 0;
#if !defined(__x86_64__)
    /* Every x86-64 processor has FXSR and SSE, and its programs use SSE for
     * their floating point, so the switch is i686's alone. */
    if (use_fxsr && kcmdline_has("rach2.nofxsr")) {
        use_fxsr = 0;
    }
#endif
    /* A processor can have FXSR without SSE, the Pentium II for one; the
     * SSE instructions and CR4.OSFXSR are for a processor that has both. */
    have_sse = use_fxsr && (d & CPUID_EDX_SSE) != 0;
    /* The XSAVE image begins with the `fxsave` one, so XSAVE is for a
     * processor whose SSE state is saved. */
    if (have_sse && (c & CPUID_ECX_XSAVE) && max >= CPUID_XSTATE &&
        !kcmdline_has("rach2.noxsave")) {
        xfeatures = xstate_choose((c & CPUID_ECX_AVX) != 0);
        use_xsave = xfeatures != 0;
    }
    if (use_xsave) {
        area_size = xstate_end(xfeatures);
    }
    fpu_cpu_setup();

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

        __asm__ __volatile__(FXSAVE " (%0)" : : "r"(initial_state) : "memory");
        reported = get32(initial_state + 28);
        if (reported != 0) {
            mxcsr_mask = reported;
        }
    } else {
        __asm__ __volatile__("fnsave (%0); frstor (%0)" : : "r"(initial_state) : "memory");
    }
    if (use_xsave) {
        /* The header is zero after the `fxsave` image, so `xrstor` puts
         * every component in its initial configuration, MXCSR aside, which
         * it loads from the image; `xsave` then records that state. */
        load_state(initial_state);
        save_state(initial_state);
    }
    klog(KLOG_DEBUG, "fpu", "fxsr: %s, sse: %s, xsave: %s, xcr0: 0x%x, area: %d bytes",
         use_fxsr ? "on" : "off", have_sse ? "on" : "off", use_xsave ? "on" : "off",
         (unsigned)xfeatures, (int)area_size);
}

void fpu_save(void *area)
{
    save_state(area);
}

void fpu_restore(const void *area)
{
    load_state(area);
}

void fpu_reset(void)
{
    load_state(initial_state);
}

/**
 * The frame cannot be taken into the process's own area: the copy from the
 * stack may sleep for a page, and the switch away would save the registers
 * over what it had copied.
 */
int fpu_frame_load(uintptr_t fp)
{
    void *area = fpu_area_alloc();
    int rc;

    if (!area) {
        return -1;
    }
    rc = fpu_frame_take(fp, area);
    if (rc == 0) {
        load_state(area);
    }
    fpu_area_free(area);
    return rc;
}

#endif /* !RACH2_HOSTTEST */
