/**
 * Check the floating-point state's images that Linux's interfaces show: the
 * signal frame's state, its checks on sigreturn, and the XSAVE image of
 * ptrace and core files.
 *
 * host-deps: rach2/sys/lib/kstring.c rach2/sys/lib/kctypes.c
 * host-cflags: -Wno-int-to-pointer-cast -Wno-pointer-to-int-cast
 *
 * `arch/x86/fpu.c` is included whole; RACH2_HOSTTEST leaves out its part
 * that drives the unit, and each row sets the format by hand. The frames
 * are i686's, as a host test's arch.h is, unless FPU_X86_64 is defined
 * before the include, as fpuframe64_hosttest.c does. The user's stack is a
 * buffer of the test, with a range in it that faults.
 *
 * With XSAVE in Intel's AVX layout, a frame placed below a stack pointer
 * has its image 64-byte aligned and ends at most at the pointer; the
 * written state has the legacy part, the reserved bytes zero, Linux's
 * software bytes (FP_XSTATE_MAGIC1, the bytes to the end of
 * FP_XSTATE_MAGIC2, XCR0 and the image's size), XSTATE_BV with x87 and SSE
 * added, the header's other bytes zero, the extended part, and
 * FP_XSTATE_MAGIC2 after it, and nothing outside; on i686 the x87 part
 * before it has the status word and magic 0. Taken back, the frame gives
 * the area again; on i686 the 108-byte part's control word wins over the
 * image's. A frame whose FP_XSTATE_MAGIC1, FP_XSTATE_MAGIC2, or sizes are
 * wrong gives its legacy part with XSTATE_BV x87 and SSE alone, as Linux's
 * `fx_only` restore does; its software bytes' XCR0 limits the components
 * taken. A frame whose XSTATE_BV names a component XCR0 lacks, whose
 * XCOMP_BV or reserved header bytes are not zero, or whose MXCSR sets a
 * reserved bit is refused, and so is one any part of which faults. In the
 * AVX-512 layout a frame whose image size ends before a component leaves
 * that component out.
 *
 * The NT_X86_XSTATE image has XCR0 in its software bytes and the reserved
 * bytes zero; one set by a tracer is refused for the header faults above,
 * and otherwise is the initial state with the components its XSTATE_BV
 * names in use copied in, and MXCSR with its mask always.
 *
 * Without XSAVE the frame's state is Linux's `struct _fpstate`: on i686 the
 * x87 part, the status word, magic 0 and the `fxsave` image with its
 * software bytes zero, or under the `fnsave` format the image alone with
 * magic 0xFFFF; on x86-64 the `fxsave64` image. A bad MXCSR is refused.
 */
#include "arch/x86/fpu.c"

extern void *calloc(unsigned long count, unsigned long size);
extern void free(void *ptr);
extern int printf(const char *format, ...);
extern void *memset(void *s, int c, unsigned long n);
extern void *memcpy(void *d, const void *s, unsigned long n);
extern int memcmp(const void *a, const void *b, unsigned long n);

#if defined(FPU_X86_64)
#define LAYOUT "x86-64"
#else
#define LAYOUT "i686"
#endif

static int failures;

#define CHECK(c, detail)                                                                 \
    do {                                                                                 \
        if (!(c)) {                                                                      \
            printf("fpuframe-test: " LAYOUT ": fail: %s (line %d)\n", detail, __LINE__); \
            failures++;                                                                  \
        }                                                                                \
    } while (0)

/* The user's stack: 16 KiB of it, with a guard pattern, and a range that
 * faults when `fault_len` is not zero. */
#define STACK_SIZE 16384
static uint8_t stack[STACK_SIZE] __attribute__((aligned(64)));
static uintptr_t fault_at;
static size_t fault_len;

static int faults(uintptr_t a, size_t n)
{
    return fault_len != 0 && a < fault_at + fault_len && fault_at < a + n;
}

int copy_from_user(void *dst, const void __user *src, size_t n)
{
    if (faults((uintptr_t)src, n)) {
        return -1;
    }
    memcpy(dst, (const void *)src, n);
    return 0;
}

int copy_to_user(void __user *dst, const void *src, size_t n)
{
    if (faults((uintptr_t)dst, n)) {
        return -1;
    }
    memcpy((void *)dst, src, n);
    return 0;
}

void *kmalloc(size_t size)
{
    return calloc(1, size);
}

void kfree(void *p)
{
    free(p);
}

/* The initial state as the unit makes it: control word 0x37F, MXCSR 0x1F80
 * and its mask, every other byte zero. */
static void set_initial(void)
{
    memset(initial_state, 0, sizeof(initial_state));
    put32(initial_state + FX_CWD, 0x037Fu);
    put32(initial_state + FX_MXCSR, 0x1F80u);
    put32(initial_state + FX_MXCSR + 4, mxcsr_mask);
}

/* XSAVE with x87, SSE and AVX at Intel's offsets, 832 bytes. */
static void set_avx(void)
{
    use_fxsr = 1;
    have_sse = 1;
    use_xsave = 1;
    xfeatures = 7;
    area_size = 832;
    mxcsr_mask = 0xFFBFu;
    memset(comp_size, 0, sizeof(comp_size));
    memset(comp_offset, 0, sizeof(comp_offset));
    comp_offset[2] = 576;
    comp_size[2] = 256;
    set_initial();
}

/* XSAVE with AVX-512 too, at Intel's offsets with MPX's gap, 2688 bytes. */
static void set_avx512(void)
{
    set_avx();
    xfeatures = 0xE7;
    area_size = 2688;
    comp_offset[5] = 1088;
    comp_size[5] = 64;
    comp_offset[6] = 1152;
    comp_size[6] = 512;
    comp_offset[7] = 1664;
    comp_size[7] = 1024;
    set_initial();
}

/* An area of the format with every byte a pattern of `seed`, a valid MXCSR,
 * the header's reserved bytes zero and XSTATE_BV `bv`. */
static uint8_t *pattern_area(unsigned seed, uint64_t bv)
{
    uint8_t *a = fpu_area_alloc();
    uint32_t i;

    for (i = 0; i < area_size; i++) {
        a[i] = (uint8_t)(i * 7u + seed);
    }
    put32(a + FX_MXCSR, 0x1F80u);
#if !defined(FPU_X86_64)
    /* The bytes 32-bit mode reserves beside the tag byte and the code and
     * data selectors, which the `fnsave` layout has no room for. */
    a[FX_TWD + 1] = 0;
    put16(a + FX_FCS + 2, 0);
    put16(a + FX_FOS + 2, 0);
#endif
    memset(a + FX_PAD, 0, FX_SIZE - FX_PAD);
    if (use_xsave) {
        memset(a + XSAVE_HDR, 0, XSAVE_EXT - XSAVE_HDR);
        put64(a + XSAVE_HDR, bv);
    }
    return a;
}

/* A frame of `area` placed below the stack's top, with the stack filled
 * with 0xCC first; its state's address. */
static uintptr_t frame_of(const uint8_t *area)
{
    uintptr_t fp;

    memset(stack, 0xCC, sizeof(stack));
    fp = fpu_frame_place((uintptr_t)stack + STACK_SIZE - 40);
    CHECK(fpu_frame_put(fp, area) == 0, "the frame is written");
    return fp;
}

static int all_bytes(const uint8_t *p, size_t n, uint8_t v)
{
    size_t i;

    for (i = 0; i < n; i++) {
        if (p[i] != v) {
            return 0;
        }
    }
    return 1;
}

static void row_place_and_put(void)
{
    uint8_t *a;
    uintptr_t top, fp;
    uint8_t *img;

    set_avx();
    a = pattern_area(3, 4);
    top = (uintptr_t)stack + STACK_SIZE - 40;
    fp = frame_of(a);
    img = (uint8_t *)fp + FRAME_PREFIX;
    CHECK(((uintptr_t)img & 63u) == 0, "the image is 64-byte aligned");
    CHECK((uintptr_t)img + area_size + 4 <= top, "the frame ends below the stack pointer");
    CHECK((uintptr_t)img + area_size + 4 + 64 > top,
          "the frame is placed no lower than it must be");
    CHECK(all_bytes((uint8_t *)fp - 64, 64, 0xCC), "nothing is written below the frame");
    CHECK(all_bytes(img + area_size + 4, (size_t)(top - ((uintptr_t)img + area_size + 4)), 0xCC),
          "nothing is written past FP_XSTATE_MAGIC2");
    CHECK(memcmp(img, a, FX_PAD) == 0, "the legacy part is the area's");
    CHECK(all_bytes(img + FX_PAD, FX_SW - FX_PAD, 0), "the reserved bytes are zero");
    CHECK(get32(img + FX_SW) == FP_XSTATE_MAGIC1, "FP_XSTATE_MAGIC1");
    CHECK(get32(img + FX_SW + 4) == FRAME_PREFIX + area_size + 4, "the extended size");
    CHECK(get64(img + FX_SW + 8) == xfeatures, "XCR0 in the software bytes");
    CHECK(get32(img + FX_SW + 16) == area_size, "the image's size");
    CHECK(all_bytes(img + FX_SW + 20, FX_SIZE - FX_SW - 20, 0), "the software bytes' padding");
    CHECK(get64(img + XSAVE_HDR) == (4u | XFEATURE_X87 | XFEATURE_SSE),
          "XSTATE_BV with x87 and SSE added");
    CHECK(all_bytes(img + XSAVE_HDR + 8, XSAVE_EXT - XSAVE_HDR - 8, 0), "the header's other bytes");
    CHECK(memcmp(img + XSAVE_EXT, a + XSAVE_EXT, area_size - XSAVE_EXT) == 0, "the extended part");
    CHECK(get32(img + area_size) == FP_XSTATE_MAGIC2, "FP_XSTATE_MAGIC2 after the image");
#if !defined(FPU_X86_64)
    CHECK(get16((uint8_t *)fp + LX_STATUS) == get16(a + FX_SWD), "the status word");
    CHECK(get16((uint8_t *)fp + LX_MAGIC) == 0, "the magic word of the FXSR format");
    CHECK(get32((uint8_t *)fp + FN_CWD) == (get16(a + FX_CWD) | 0xFFFF0000u),
          "the x87 part's control word");
#endif
    fpu_area_free(a);
}

/* Take the frame at `fp` into a fresh area; the result in `*rc`. */
static uint8_t *take(uintptr_t fp, int *rc)
{
    uint8_t *b = fpu_area_alloc();

    *rc = fpu_frame_take(fp, b);
    return b;
}

static void row_take(void)
{
    uint8_t *a, *b;
    uintptr_t fp;
    uint8_t *img;
    int rc;

    set_avx();
    a = pattern_area(5, 7);
    fp = frame_of(a);
    img = (uint8_t *)fp + FRAME_PREFIX;
    b = take(fp, &rc);
    CHECK(rc == 0, "a frame as written is taken");
    CHECK(memcmp(b, a, FX_PAD) == 0, "the legacy part comes back");
    CHECK(get64(b + XSAVE_HDR) == 7, "XSTATE_BV comes back");
    CHECK(all_bytes(b + XSAVE_HDR + 8, XSAVE_EXT - XSAVE_HDR - 8, 0), "the header comes back zero");
    CHECK(memcmp(b + XSAVE_EXT, a + XSAVE_EXT, area_size - XSAVE_EXT) == 0,
          "the extended part comes back");
    fpu_area_free(b);
#if !defined(FPU_X86_64)
    put32((uint8_t *)fp + FN_CWD, 0xFFFF0C7Fu);
    b = take(fp, &rc);
    CHECK(rc == 0 && get16(b + FX_CWD) == 0x0C7Fu, "the x87 part's control word wins");
    fpu_area_free(b);
    put32((uint8_t *)fp + FN_CWD, get16(a + FX_CWD) | 0xFFFF0000u);
#endif

    put32(img + FX_SW, 0);
    b = take(fp, &rc);
    CHECK(rc == 0, "a frame without FP_XSTATE_MAGIC1 is taken");
    CHECK(get64(b + XSAVE_HDR) == 3, "without FP_XSTATE_MAGIC1 x87 and SSE alone");
    CHECK(memcmp(b + FX_XMM, a + FX_XMM, FX_PAD - FX_XMM) == 0,
          "without FP_XSTATE_MAGIC1 the XMM registers come back");
    fpu_area_free(b);
    put32(img + FX_SW, FP_XSTATE_MAGIC1);

    put32(img + area_size, 0);
    b = take(fp, &rc);
    CHECK(rc == 0 && get64(b + XSAVE_HDR) == 3, "without FP_XSTATE_MAGIC2 x87 and SSE alone");
    fpu_area_free(b);
    put32(img + area_size, FP_XSTATE_MAGIC2);

    put32(img + FX_SW + 16, area_size + 64);
    b = take(fp, &rc);
    CHECK(rc == 0 && get64(b + XSAVE_HDR) == 3, "an image larger than the kernel's is fx only");
    fpu_area_free(b);
    put32(img + FX_SW + 16, 512);
    b = take(fp, &rc);
    CHECK(rc == 0 && get64(b + XSAVE_HDR) == 3, "an image without its header is fx only");
    fpu_area_free(b);
    put32(img + FX_SW + 16, area_size);
    put32(img + FX_SW + 4, area_size - 4);
    b = take(fp, &rc);
    CHECK(rc == 0 && get64(b + XSAVE_HDR) == 3, "an image past the extended size is fx only");
    fpu_area_free(b);
    put32(img + FX_SW + 4, FRAME_PREFIX + area_size + 4);

    put64(img + FX_SW + 8, 3);
    b = take(fp, &rc);
    CHECK(rc == 0 && get64(b + XSAVE_HDR) == 3, "the frame's XCR0 limits the components taken");
    fpu_area_free(b);
    put64(img + FX_SW + 8, xfeatures);

    b = take(fp, &rc);
    CHECK(rc == 0 && get64(b + XSAVE_HDR) == 7, "the frame is whole again");
    fpu_area_free(b);
    fpu_area_free(a);
}

static void row_refused(void)
{
    uint8_t *a, *b;
    uintptr_t fp;
    uint8_t *img;
    int rc;

    set_avx();
    a = pattern_area(9, 7);
    fp = frame_of(a);
    img = (uint8_t *)fp + FRAME_PREFIX;

    put64(img + XSAVE_HDR, 0x0F);
    b = take(fp, &rc);
    CHECK(rc == -1, "XSTATE_BV naming a component XCR0 lacks is refused");
    fpu_area_free(b);
    put64(img + XSAVE_HDR, 7);

    img[XSAVE_HDR + 8] = 1;
    b = take(fp, &rc);
    CHECK(rc == -1, "a frame with XCOMP_BV set is refused");
    fpu_area_free(b);
    img[XSAVE_HDR + 8] = 0;

    img[XSAVE_EXT - 1] = 1;
    b = take(fp, &rc);
    CHECK(rc == -1, "a frame with a reserved header byte set is refused");
    fpu_area_free(b);
    img[XSAVE_EXT - 1] = 0;

    put32(img + FX_MXCSR, 0x80001F80u);
    b = take(fp, &rc);
    CHECK(rc == -1, "a frame with a reserved MXCSR bit is refused");
    fpu_area_free(b);
    put32(img + FX_SW, 0);
    b = take(fp, &rc);
    CHECK(rc == -1, "an fx-only frame with a reserved MXCSR bit is refused");
    fpu_area_free(b);
    put32(img + FX_SW, FP_XSTATE_MAGIC1);
    put32(img + FX_MXCSR, 0x1F80u);

    fault_at = (uintptr_t)img + area_size;
    fault_len = 4;
    b = take(fp, &rc);
    CHECK(rc == -1, "a frame whose FP_XSTATE_MAGIC2 faults is refused");
    fpu_area_free(b);
    fault_at = (uintptr_t)img + 700;
    b = take(fp, &rc);
    CHECK(rc == -1, "a frame whose extended part faults is refused");
    fpu_area_free(b);
    fault_at = (uintptr_t)img + 100;
    b = take(fp, &rc);
    CHECK(rc == -1, "a frame whose legacy part faults is refused");
    fpu_area_free(b);
    fault_at = (uintptr_t)img + 100;
    CHECK(fpu_frame_put(fp, a) == -1, "a frame that cannot be written is refused");
    fault_len = 0;

    b = take(fp, &rc);
    CHECK(rc == 0 && get64(b + XSAVE_HDR) == 7, "the frame is whole again");
    fpu_area_free(b);
    fpu_area_free(a);
}

static void row_avx512(void)
{
    uint8_t *a, *b;
    uintptr_t fp;
    uint8_t *img;
    int rc;

    set_avx512();
    a = pattern_area(11, 0xE7);
    fp = frame_of(a);
    img = (uint8_t *)fp + FRAME_PREFIX;
    CHECK(((uintptr_t)img & 63u) == 0, "the AVX-512 image is 64-byte aligned");
    CHECK(get32(img + 2688) == FP_XSTATE_MAGIC2, "FP_XSTATE_MAGIC2 after the AVX-512 image");
    b = take(fp, &rc);
    CHECK(rc == 0 && get64(b + XSAVE_HDR) == 0xE7, "the AVX-512 frame is taken whole");
    CHECK(memcmp(b + 1664, a + 1664, 1024) == 0, "ZMM16 to ZMM31 come back");
    fpu_area_free(b);

    put32(img + FX_SW + 16, 832);
    put32(img + 832, FP_XSTATE_MAGIC2);
    b = take(fp, &rc);
    CHECK(rc == 0 && get64(b + XSAVE_HDR) == 7, "an image that ends before AVX-512 leaves it out");
    fpu_area_free(b);
    fpu_area_free(a);
}

static void row_xstate(void)
{
    uint8_t *a, *b, *in;
    uint8_t *view;
    int rc;

    set_avx512();
    a = pattern_area(13, 0xE7);
    view = fpu_area_alloc();
    memcpy(view, a, area_size);
    memset(view + FX_PAD, 0x55, FX_SIZE - FX_PAD);
    fpu_xstate_view(view);
    CHECK(all_bytes(view + FX_PAD, FX_SW - FX_PAD, 0), "the view's reserved bytes are zero");
    CHECK(get64(view + FX_SW) == xfeatures, "the view has XCR0 at byte 464");
    CHECK(all_bytes(view + FX_SW + 8, FX_SIZE - FX_SW - 8, 0), "the view's other software bytes");
    CHECK(memcmp(view + XSAVE_HDR, a + XSAVE_HDR, area_size - XSAVE_HDR) == 0,
          "the view's header and extended part are the area's");

    b = fpu_area_alloc();
    in = view;
    put64(in + XSAVE_HDR, 0x1E7);
    CHECK(fpu_xstate_set(b, in) == -1, "a tracer's XSTATE_BV beyond XCR0 is refused");
    put64(in + XSAVE_HDR, 0xE7);
    in[XSAVE_HDR + 8] = 1;
    CHECK(fpu_xstate_set(b, in) == -1, "a tracer's XCOMP_BV is refused");
    in[XSAVE_HDR + 8] = 0;
    in[XSAVE_HDR + 40] = 1;
    CHECK(fpu_xstate_set(b, in) == -1, "a tracer's reserved header byte is refused");
    in[XSAVE_HDR + 40] = 0;
    put32(in + FX_MXCSR, 0x00011F80u);
    CHECK(fpu_xstate_set(b, in) == -1, "a tracer's reserved MXCSR bit is refused");
    put32(in + FX_MXCSR, 0x1F80u);
    CHECK(fpu_xstate_set(b, in) == 0, "a tracer's whole image is taken");
    CHECK(memcmp(b, a, FX_PAD) == 0 && memcmp(b + 576, a + 576, 256) == 0 &&
              memcmp(b + 1088, a + 1088, area_size - 1088) == 0,
          "every component of a tracer's image is taken");
    CHECK(all_bytes(b + 832, 1088 - 832, 0), "the gap of a tracer's image is zero");

    put64(in + XSAVE_HDR, 0x22);
    memset(in + 960, 0x77, 128);
    rc = fpu_xstate_set(b, in);
    CHECK(rc == 0, "an image with x87 and AVX not in use is taken");
    CHECK(get32(b + FX_CWD) == 0x037Fu && all_bytes(b + FX_ST, FX_XMM - FX_ST, 0),
          "x87 not in use is its initial state");
    CHECK(memcmp(b + FX_XMM, a + FX_XMM, FX_PAD - FX_XMM) == 0, "SSE in use is taken");
    CHECK(get32(b + FX_MXCSR) == 0x1F80u, "MXCSR is taken");
    CHECK(all_bytes(b + 576, 256, 0), "AVX not in use is zero");
    CHECK(all_bytes(b + 960, 128, 0), "the gap between components is zero");
    CHECK(memcmp(b + 1088, a + 1088, 64) == 0, "the opmask registers in use are taken");
    CHECK(all_bytes(b + 1152, 1536, 0), "the ZMM components not in use are zero");
    CHECK(get64(b + XSAVE_HDR) == 0x22, "XSTATE_BV is the tracer's");
    fpu_area_free(b);
    fpu_area_free(view);
    fpu_area_free(a);
}

static void row_legacy(void)
{
    uint8_t *a, *b;
    uintptr_t fp;
    int rc;

    set_avx();
    use_xsave = 0;
    xfeatures = 0;
    area_size = FX_SIZE;
    a = pattern_area(17, 0);
    fp = frame_of(a);
    CHECK((fp & 63u) == 0, "the legacy state is 64-byte aligned");
    CHECK(all_bytes((uint8_t *)fp + FPU_LINUX_SIZE,
                    (size_t)((uintptr_t)stack + STACK_SIZE - 40 - (fp + FPU_LINUX_SIZE)), 0xCC),
          "nothing is written past the legacy state");
    CHECK(memcmp((uint8_t *)fp + FRAME_PREFIX, a, FX_PAD) == 0, "the legacy image is the area's");
    CHECK(all_bytes((uint8_t *)fp + FRAME_PREFIX + FX_SW, FX_SIZE - FX_SW, 0),
          "no XSAVE image is announced");
#if !defined(FPU_X86_64)
    CHECK(get16((uint8_t *)fp + LX_MAGIC) == 0, "the legacy magic word is 0");
#endif
    b = take(fp, &rc);
    CHECK(rc == 0 && memcmp(b, a, FX_PAD) == 0, "the legacy state comes back");
    fpu_area_free(b);
    put32((uint8_t *)fp + FRAME_PREFIX + FX_MXCSR, 0x80001F80u);
    b = take(fp, &rc);
    CHECK(rc == -1, "a legacy state with a reserved MXCSR bit is refused");
    fpu_area_free(b);
    fpu_area_free(a);
#if !defined(FPU_X86_64)
    use_fxsr = 0;
    have_sse = 0;
    a = pattern_area(19, 0);
    fp = frame_of(a);
    CHECK(memcmp((uint8_t *)fp, a, FPU_ENV_SIZE) == 0, "the fnsave image comes first");
    CHECK(get16((uint8_t *)fp + LX_MAGIC) == 0xFFFFu, "the fnsave format's magic word");
    CHECK(all_bytes((uint8_t *)fp + FRAME_PREFIX, FX_SIZE, 0), "no fxsave image follows");
    b = take(fp, &rc);
    CHECK(rc == 0 && memcmp(b, a, FPU_ENV_SIZE) == 0, "the fnsave image comes back");
    fpu_area_free(b);
    fpu_area_free(a);
#endif
}

int main(void)
{
    row_place_and_put();
    row_take();
    row_refused();
    row_avx512();
    row_xstate();
    row_legacy();
    if (failures != 0) {
        printf("fpuframe-test: " LAYOUT ": %d failures\n", failures);
        return 1;
    }
    printf("fpuframe-test: " LAYOUT ": pass: the frame's layout and checks, the XSAVE image of "
           "ptrace and the legacy frames\n");
    return 0;
}
