/**
 * i915_parser_hosttest.c: the Intel graphics driver's command parser,
 * i915_cmd_parser.c, on batches in pages of the test's, each page its own
 * allocation so that a command crossing a page crosses memory, judged as
 * Linux 6.12's parser judges them on Haswell.
 *
 * host-deps: rach2/sys/lib/kstring.c rach2/sys/lib/kctypes.c
 *
 * A verdict is read from the shadow, as the chip would run it: a batch
 * that passes is copied whole and its trampoline ends at once, so the copy
 * runs privileged; a refused one has its copy ended at its first dword and
 * its trampoline starts the original, unprivileged; and an error is the
 * call's, which Linux gives the program.
 *
 * The rows, one for each of 78-8b's subtasks: Linux's tables sorted, every
 * entry of the render and blitter tables, every register of the allowlists
 * through each command that names one, the masked registers, each masked
 * check of a command, the lengths of commands no table holds, and the
 * refusals of a batch the parser cannot read; the copy and the trampoline;
 * GETPARAM's version; and every case of IGT's gen7_exec_parse (formerly
 * gem_exec_parse), which Linux's parser at version 10 runs, with the
 * verdict Linux gives it.
 *
 * Returns 0 when every row passes, 1 otherwise.
 */

#include "drm/i915/i915_cmd_parser.c"

extern int printf(const char *format, ...);
extern int posix_memalign(void **p, unsigned long align, unsigned long size);
extern void free(void *p);

static int fails;

#define CHECK(what, cond)                                 \
    do {                                                  \
        if (!(cond)) {                                    \
            printf("i915-parser-test: fail: %s\n", what); \
            fails++;                                      \
        }                                                 \
    } while (0)

/* IGT's registers outside the allowlists. */
#define OASTATUS2   0x2368u
#define OACONTROL   0x2360u
#define DERRMR      0x44050u
#define RING_START0 0x2038u

/* IGT's PIPE_CONTROL_LRI_POST_OP, Linux's PIPE_CONTROL_MMIO_WRITE. */
#define PIPE_CONTROL_LRI_POST_OP (1u << 23)

/* An MI command with a length that no table holds, and a blitter one. */
#define MI_ATOMIC       MI_INSTR(0x2f, 0)
#define XY_SRC_COPY_BLT ((2u << 29) | (0x53u << 22))

/* The verdicts. */
enum { V_ERROR, V_PRIV, V_UNPRIV };

/* ---- the kernel around the parser ---- */

/* The temporary mappings held, and the mapping that fails, counted from
 * 1, or 0 for none. */
static int maps_live, map_calls, map_fail_at;

void *kmap_temp(uintptr_t phys)
{
    if (map_fail_at && ++map_calls == map_fail_at) {
        return 0;
    }
    maps_live++;
    return (void *)phys;
}

void kunmap_temp(void *va)
{
    (void)va;
    maps_live--;
}

static int flushes;

void arch_dma_flush(const volatile void *p, size_t len)
{
    (void)p;
    (void)len;
    flushes++;
}

/* ---- pages ---- */

#define PAGE     4096u
#define MAXPAGES 32u
#define SCRUB    0xc5c5c5c5u
#define DIRT     0xa5a5a5a5u

/* An object's pages, each allocated alone. */
struct pages {
    uint32_t n;
    uintptr_t frame[MAXPAGES];
};

/** Make `n` pages, every dword `fill`. */
static void pages_make(struct pages *p, uint32_t n, uint32_t fill)
{
    uint32_t i, k;
    void *m;

    p->n = n;
    for (i = 0; i < n; i++) {
        m = 0;
        if (posix_memalign(&m, PAGE, PAGE) != 0) {
            printf("i915-parser-test: out of memory\n");
            fails++;
            p->n = i;
            return;
        }
        for (k = 0; k < PAGE / 4u; k++) {
            ((uint32_t *)m)[k] = fill;
        }
        p->frame[i] = (uintptr_t)m;
    }
}

/** Free the pages of `p`. */
static void pages_free(struct pages *p)
{
    uint32_t i;

    for (i = 0; i < p->n; i++) {
        free((void *)p->frame[i]);
    }
    p->n = 0;
}

/** The dword at byte `at` of the pages. */
static uint32_t *dw(struct pages *p, uint32_t at)
{
    return (uint32_t *)(p->frame[at / PAGE]) + (at % PAGE) / 4u;
}

/* ---- the parse ---- */

/* The engines, by their base as the driver's are, and one at graphics 4
 * and 5's video ring base, which Haswell has no ring at. */
static struct i915_engine rcs, bcs, none;

/* The shadow of the last parse that ran, kept for the rows that read it. */
static struct pages shadow;
static int32_t last_ret;

/**
 * Parse `len` bytes from byte `offset` of the batch `b` on `e`, the batch
 * at `0x100000 + offset` in the GTT, with `clflush`, into a new shadow of
 * the length and the trampoline, its pages dirty first. Check what every
 * verdict leaves: the copy whole and the trampoline's end, or the copy
 * ended and the trampoline's start of the original, the shadow zero past
 * them, and no mapping held. The verdict.
 */
static int parse_in(struct i915_engine *e, struct pages *b, uint32_t offset, uint32_t len,
                    uint32_t clflush)
{
    struct i915_cmd_parse p;
    uint32_t size = len + I915_CMD_PARSER_TRAMPOLINE_SIZE, k, end = len / 4u, addr;
    int same = 1, zero = 1, v;

    pages_free(&shadow);
    size = (size + PAGE - 1u) & ~(PAGE - 1u);
    pages_make(&shadow, size / PAGE, DIRT);
    addr = 0x100000u + offset;
    p.batch = b->frame;
    p.offset = offset;
    p.len = len;
    p.addr = addr;
    p.clflush = clflush;
    p.shadow = shadow.frame;
    p.shadow_size = size;
    last_ret = i915_cmd_parser(e, &p);
    CHECK("parse: no temporary mapping held", maps_live == 0);
    if (last_ret != 0) {
        return V_ERROR;
    }
    for (k = 1; k < end; k++) {
        same &= *dw(&shadow, 4u * k) == *dw(b, offset + 4u * k);
    }
    for (k = end + 2u; k < size / 4u; k++) {
        zero &= *dw(&shadow, 4u * k) == 0;
    }
    CHECK("copy: the batch copied whole past its first dword", same);
    CHECK("copy: the shadow zero past the trampoline", zero);
    if (*dw(&shadow, 4u * end) == MI_BATCH_BUFFER_END) {
        CHECK("trampoline: a batch that passes copied whole",
              *dw(&shadow, 0) == *dw(b, offset) && *dw(&shadow, 4u * end + 4u) == 0);
        v = V_PRIV;
    } else {
        CHECK("trampoline: a refused batch's copy ends at once",
              *dw(&shadow, 0) == MI_BATCH_BUFFER_END);
        CHECK("trampoline: the original started unprivileged",
              *dw(&shadow, 4u * end) == (MI_BATCH_BUFFER_START | MI_BATCH_NON_SECURE_HSW) &&
                  *dw(&shadow, 4u * end + 4u) == addr);
        v = V_UNPRIV;
    }
    return v;
}

/** The verdict on the `n` dwords `cmds`, written at byte `offset` of an
 * object of `npages` pages of SCRUB, with a length of `len` bytes, or of
 * the dwords rounded up to quadwords for 0. */
static int parse_at(struct i915_engine *e, const uint32_t *cmds, uint32_t n, uint32_t offset,
                    uint32_t len, uint32_t npages)
{
    struct pages b;
    uint32_t k;
    int v;

    pages_make(&b, npages, SCRUB);
    for (k = 0; k < n; k++) {
        *dw(&b, offset + 4u * k) = cmds[k];
    }
    if (len == 0) {
        len = (4u * n + 7u) & ~7u;
        if (len > 4u * n) {
            *dw(&b, offset + 4u * n) = MI_NOOP;
        }
    }
    v = parse_in(e, &b, offset, len, 0);
    pages_free(&b);
    return v;
}

/** The verdict on the `n` dwords `cmds` at the start of a page. */
static int parse(struct i915_engine *e, const uint32_t *cmds, uint32_t n)
{
    return parse_at(e, cmds, n, 0, 0, 1);
}

/** The verdict on one command of `ndw` dwords, its first `header` and the
 * rest `rest`, then MI_BATCH_BUFFER_END. */
static int parse_cmd(struct i915_engine *e, uint32_t header, uint32_t ndw, const uint32_t *rest)
{
    uint32_t b[300], k;

    b[0] = header;
    for (k = 1; k < ndw; k++) {
        b[k] = rest ? rest[k - 1u] : 0;
    }
    b[ndw] = MI_BATCH_BUFFER_END;
    return parse(e, b, ndw + 1u);
}

#define N(a) ((uint32_t)(sizeof(a) / sizeof((a)[0])))

/* ---- the rows ---- */

/**
 * 78-8b.1: Linux's tables: sorted as Linux's init checks them, which the
 * allowlists' binary search needs; the render and blitter rings parse and
 * an engine of no ring Haswell has does not; every entry of each ring's
 * tables judged as its action says, an MI_BATCH_BUFFER_START refused; and
 * every register of each allowlist written through MI_LOAD_REGISTER_IMM
 * and read through MI_STORE_REGISTER_MEM on its ring, and refused on the
 * other.
 */
static void tables(void)
{
    const struct cmd_engine *engines[2];
    struct i915_engine *rings[2];
    const struct cmd_desc *d;
    uint32_t e, i, j, prev, cur, rest[2];
    int sorted = 1, bad = 0, v, want;

    engines[0] = &hsw_render_engine;
    engines[1] = &hsw_blt_engine;
    rings[0] = &rcs;
    rings[1] = &bcs;
    for (e = 0; e < 2; e++) {
        for (i = 0; i < engines[e]->ncmds; i++) {
            prev = 0;
            for (j = 0; j < engines[e]->cmds[i].count; j++) {
                d = &engines[e]->cmds[i].desc[j];
                cur = d->value & d->mask;
                sorted &= cur >= prev;
                prev = cur;
            }
        }
        for (i = 0; i < engines[e]->nregs; i++) {
            prev = 0;
            for (j = 0; j < engines[e]->regs[i].count; j++) {
                cur = engines[e]->regs[i].regs[j].addr;
                sorted &= cur >= prev;
                prev = cur;
            }
        }
    }
    CHECK("tables: every command and register table sorted", sorted);
    CHECK("tables: the render and blitter rings parse",
          i915_engine_using_cmd_parser(&rcs) && i915_engine_using_cmd_parser(&bcs));
    CHECK("tables: an engine of no ring does not, and its parse is EINVAL",
          !i915_engine_using_cmd_parser(&none) && parse_cmd(&none, MI_NOOP, 1, 0) == V_ERROR &&
              last_ret == -EINVAL);

    /* Each entry, four dwords long unless its length is fixed, its fields
     * zero: a passed or masked command passes, and a refused one, one whose
     * register 0 is outside the allowlist, and MI_BATCH_BUFFER_START are
     * refused. */
    for (e = 0; e < 2; e++) {
        for (i = 0; i < engines[e]->ncmds; i++) {
            for (j = 0; j < engines[e]->cmds[i].count; j++) {
                d = &engines[e]->cmds[i].desc[j];
                if (d->flags & CMD_DESC_FIXED) {
                    v = parse_cmd(rings[e], d->value, d->length, 0);
                } else {
                    v = parse_cmd(rings[e], d->value | 2u, 4u, 0);
                }
                if (d->flags & (CMD_DESC_REJECT | CMD_DESC_REGISTER) ||
                    d->value == MI_BATCH_BUFFER_START) {
                    want = V_UNPRIV;
                } else {
                    want = V_PRIV;
                }
                if (v != want) {
                    printf("i915-parser-test: entry 0x%08x on %s: verdict %d, not %d\n",
                           (unsigned)d->value, rings[e]->name, v, want);
                    bad++;
                }
            }
        }
    }
    CHECK("tables: every entry of both rings judged by its action", bad == 0);

    /* Every register of each ring's allowlists. */
    bad = 0;
    for (e = 0; e < 2; e++) {
        for (i = 0; i < engines[e]->nregs; i++) {
            for (j = 0; j < engines[e]->regs[i].count; j++) {
                rest[0] = engines[e]->regs[i].regs[j].addr;
                rest[1] = 0;
                bad += parse_cmd(rings[e], MI_LOAD_REGISTER_IMM(1), 3, rest) != V_PRIV;
                bad += parse_cmd(rings[e], MI_STORE_REGISTER_MEM, 3, rest) != V_PRIV;
                if (find_reg(engines[1 - e], rest[0]) == 0) {
                    bad += parse_cmd(rings[1 - e], MI_LOAD_REGISTER_IMM(1), 3, rest) != V_UNPRIV;
                }
            }
        }
    }
    CHECK("tables: every allowlisted register written and read on its ring, refused on the other",
          bad == 0);
    rest[0] = BCS_SWCTRL;
    rest[1] = 0;
    CHECK("tables: the blitter's BCS_SWCTRL refused on the render ring",
          parse_cmd(&rcs, MI_LOAD_REGISTER_IMM(1), 3, rest) == V_UNPRIV);
    rest[0] = GEN8_RING_CS_GPR(RENDER_RING_BASE, 0);
    CHECK("tables: the render ring's GPR0 refused on the blitter",
          parse_cmd(&bcs, MI_LOAD_REGISTER_IMM(1), 3, rest) == V_UNPRIV);
    rest[0] = RING_TIMESTAMP(BSD_RING_BASE);
    CHECK("tables: 0x4358, Linux's video ring timestamp, allowed on both",
          parse_cmd(&rcs, MI_STORE_REGISTER_MEM, 3, rest) == V_PRIV &&
              parse_cmd(&bcs, MI_STORE_REGISTER_MEM, 3, rest) == V_PRIV);
    rest[0] = RING_TIMESTAMP(GEN6_BSD_RING_BASE);
    CHECK("tables: Haswell's own video ring timestamp refused, as Linux's tables have it",
          parse_cmd(&rcs, MI_STORE_REGISTER_MEM, 3, rest) == V_UNPRIV);
}

/**
 * 78-8b.2: the registers only MI_LOAD_REGISTER_IMM may write, and only
 * with the values their masks allow, and a command naming several
 * registers.
 */
static void checks_regs(void)
{
    uint32_t r[8];

    r[0] = HSW_SCRATCH1;
    r[1] = HSW_SCRATCH1_L3_DATA_ATOMICS_DISABLE;
    CHECK("masked: HSW_SCRATCH1 takes its atomics bit",
          parse_cmd(&rcs, MI_LOAD_REGISTER_IMM(1), 3, r) == V_PRIV);
    r[1] = 1;
    CHECK("masked: HSW_SCRATCH1 refuses any other bit",
          parse_cmd(&rcs, MI_LOAD_REGISTER_IMM(1), 3, r) == V_UNPRIV);
    r[0] = HSW_ROW_CHICKEN3;
    r[1] = HSW_ROW_CHICKEN3_L3_GLOBAL_ATOMICS_DISABLE << 16 |
           HSW_ROW_CHICKEN3_L3_GLOBAL_ATOMICS_DISABLE;
    CHECK("masked: HSW_ROW_CHICKEN3 takes its atomics bit and its mask",
          parse_cmd(&rcs, MI_LOAD_REGISTER_IMM(1), 3, r) == V_PRIV);
    r[1] = 1u << 16;
    CHECK("masked: HSW_ROW_CHICKEN3 refuses any other",
          parse_cmd(&rcs, MI_LOAD_REGISTER_IMM(1), 3, r) == V_UNPRIV);
    r[0] = HSW_SCRATCH1;
    r[1] = 0;
    CHECK("masked: MI_LOAD_REGISTER_MEM refused",
          parse_cmd(&rcs, MI_LOAD_REGISTER_MEM, 3, r) == V_UNPRIV);
    CHECK("masked: read by MI_STORE_REGISTER_MEM",
          parse_cmd(&rcs, MI_STORE_REGISTER_MEM, 3, r) == V_PRIV);
    r[0] = GEN8_RING_CS_GPR(RENDER_RING_BASE, 1);
    r[1] = HSW_SCRATCH1;
    CHECK("masked: MI_LOAD_REGISTER_REG into it refused",
          parse_cmd(&rcs, MI_LOAD_REGISTER_REG, 3, r) == V_UNPRIV);
    r[0] = HSW_SCRATCH1;
    CHECK("masked: MI_LOAD_REGISTER_REG out of it refused, as Linux checks both",
          parse_cmd(&rcs, MI_LOAD_REGISTER_REG, 3, r) == V_UNPRIV);
    CHECK("masked: MI_LOAD_REGISTER_IMM without its value refused",
          parse_cmd(&rcs, MI_INSTR(0x22, 0), 2, r) == V_UNPRIV);
    r[0] = GEN8_RING_CS_GPR(RENDER_RING_BASE, 0);
    CHECK("lri: an allowed register without its value passes, as Linux's",
          parse_cmd(&rcs, MI_INSTR(0x22, 0), 2, r) == V_PRIV);
    r[1] = 0;
    r[2] = GEN7_SO_WRITE_OFFSET(1);
    r[3] = 0;
    CHECK("lri: two allowed registers pass",
          parse_cmd(&rcs, MI_LOAD_REGISTER_IMM(2), 5, r) == V_PRIV);
    r[2] = OACONTROL;
    CHECK("lri: a second register outside the allowlist refused",
          parse_cmd(&rcs, MI_LOAD_REGISTER_IMM(2), 5, r) == V_UNPRIV);
    r[0] = GEN7_SO_WRITE_OFFSET(0) | 0x3u;
    CHECK("lri: the address's low bits masked off",
          parse_cmd(&rcs, MI_LOAD_REGISTER_IMM(1), 3, r) == V_PRIV);
}

/** 78-8b.2: the masked checks of each command that has them. */
static void checks_bits(void)
{
    uint32_t r[8];

    r[0] = 0;
    r[1] = 0x1000;
    r[2] = 1;
    CHECK("bits: MI_STORE_DWORD_IMM through the PPGTT",
          parse_cmd(&rcs, MI_STORE_DWORD_IMM_GEN4, 4, r) == V_PRIV &&
              parse_cmd(&bcs, MI_STORE_DWORD_IMM_GEN4, 4, r) == V_PRIV);
    CHECK("bits: MI_STORE_DWORD_IMM through the GGTT refused on both rings",
          parse_cmd(&rcs, MI_STORE_DWORD_IMM_GEN4 | MI_USE_GGTT, 4, r) == V_UNPRIV &&
              parse_cmd(&bcs, MI_STORE_DWORD_IMM_GEN4 | MI_USE_GGTT, 4, r) == V_UNPRIV);
    r[0] = GEN7_SO_WRITE_OFFSET(0);
    CHECK("bits: MI_STORE_REGISTER_MEM and MI_LOAD_REGISTER_MEM through the GGTT refused",
          parse_cmd(&rcs, MI_STORE_REGISTER_MEM | MI_USE_GGTT, 3, r) == V_UNPRIV &&
              parse_cmd(&rcs, MI_LOAD_REGISTER_MEM | MI_USE_GGTT, 3, r) == V_UNPRIV);
    CHECK("bits: MI_CLFLUSH and MI_CONDITIONAL_BATCH_BUFFER_END through the GGTT refused",
          parse_cmd(&rcs, MI_CLFLUSH | MI_USE_GGTT | 1u, 3, r) == V_UNPRIV &&
              parse_cmd(&rcs, MI_CONDITIONAL_BATCH_BUFFER_END | MI_USE_GGTT, 2, r) == V_UNPRIV &&
              parse_cmd(&rcs, MI_CLFLUSH | 1u, 3, r) == V_PRIV);
    r[0] = MI_REPORT_PERF_COUNT_GGTT;
    CHECK("bits: MI_REPORT_PERF_COUNT into the GGTT refused",
          parse_cmd(&rcs, MI_REPORT_PERF_COUNT | 1u, 3, r) == V_UNPRIV);
    r[0] = 0x1000;
    CHECK("bits: MI_REPORT_PERF_COUNT into the PPGTT",
          parse_cmd(&rcs, MI_REPORT_PERF_COUNT | 1u, 3, r) == V_PRIV);
    k_memset(r, 0, sizeof(r));
    r[1] = MEDIA_VFE_STATE_MMIO_ACCESS_MASK;
    CHECK("bits: MEDIA_VFE_STATE with register access refused",
          parse_cmd(&rcs, MEDIA_VFE_STATE | 6u, 8, r) == V_UNPRIV);
    r[1] = 0;
    CHECK("bits: MEDIA_VFE_STATE without it",
          parse_cmd(&rcs, MEDIA_VFE_STATE | 6u, 8, r) == V_PRIV);

    k_memset(r, 0, sizeof(r));
    r[0] = PIPE_CONTROL_NOTIFY;
    CHECK("bits: PIPE_CONTROL's interrupt refused",
          parse_cmd(&rcs, GFX_OP_PIPE_CONTROL(4), 4, r) == V_UNPRIV);
    r[0] = PIPE_CONTROL_STORE_DATA_INDEX | PIPE_CONTROL_QW_WRITE;
    CHECK("bits: PIPE_CONTROL's store into the status page refused",
          parse_cmd(&rcs, GFX_OP_PIPE_CONTROL(4), 4, r) == V_UNPRIV);
    r[0] = PIPE_CONTROL_STORE_DATA_INDEX;
    CHECK("bits: PIPE_CONTROL's status page bit without a post-sync write passes",
          parse_cmd(&rcs, GFX_OP_PIPE_CONTROL(4), 4, r) == V_PRIV);
    r[0] = PIPE_CONTROL_GLOBAL_GTT_IVB | PIPE_CONTROL_QW_WRITE;
    CHECK("bits: PIPE_CONTROL's write through the GGTT refused",
          parse_cmd(&rcs, GFX_OP_PIPE_CONTROL(4), 4, r) == V_UNPRIV);
    r[0] = PIPE_CONTROL_GLOBAL_GTT_IVB;
    CHECK("bits: PIPE_CONTROL's GGTT bit without a post-sync write passes",
          parse_cmd(&rcs, GFX_OP_PIPE_CONTROL(4), 4, r) == V_PRIV);
    r[0] = PIPE_CONTROL_CS_STALL | PIPE_CONTROL_QW_WRITE;
    CHECK("bits: a PIPE_CONTROL of five dwords is the same command",
          parse_cmd(&rcs, GFX_OP_PIPE_CONTROL(5), 5, r) == V_PRIV);

    k_memset(r, 0, sizeof(r));
    CHECK("bits: MI_FLUSH_DW alone passes on the blitter",
          parse_cmd(&bcs, MI_FLUSH_DW, 3, r) == V_PRIV);
    CHECK("bits: MI_FLUSH_DW's interrupt refused",
          parse_cmd(&bcs, MI_FLUSH_DW | MI_FLUSH_DW_NOTIFY, 3, r) == V_UNPRIV);
    r[0] = MI_FLUSH_DW_USE_GTT;
    CHECK("bits: MI_FLUSH_DW's store through the GGTT refused",
          parse_cmd(&bcs, MI_FLUSH_DW | MI_FLUSH_DW_OP_STOREDW, 3, r) == V_UNPRIV);
    CHECK("bits: MI_FLUSH_DW's GGTT bit without a store passes",
          parse_cmd(&bcs, MI_FLUSH_DW, 3, r) == V_PRIV);
    r[0] = 0;
    CHECK("bits: MI_FLUSH_DW's store into the status page refused",
          parse_cmd(&bcs, MI_FLUSH_DW | MI_FLUSH_DW_STORE_INDEX | MI_FLUSH_DW_OP_STOREDW, 3, r) ==
              V_UNPRIV);
    CHECK("bits: MI_FLUSH_DW's status page bit without a store passes",
          parse_cmd(&bcs, MI_FLUSH_DW | MI_FLUSH_DW_STORE_INDEX, 3, r) == V_PRIV);
    CHECK("bits: MI_FLUSH_DW, in no render table, passed over there",
          parse_cmd(&rcs, MI_FLUSH_DW | MI_FLUSH_DW_NOTIFY, 3, r) == V_PRIV);
}

/**
 * 78-8b.2: the lengths of commands no table holds, from each client's
 * mask, which a privileged command inside one shows; a client a ring does
 * not run, a command past the batch, and a batch without its end, which
 * the call refuses; what follows the end; the copy's flush and a mapping
 * that cannot be had.
 */
static void lengths(void)
{
    uint32_t b[0x110];
    struct pages p;

    k_memset(b, 0, sizeof(b));
    b[0] = (3u << 29) | (3u << 27) | (0u << 24) | (0x10u << 16) | 3u;
    b[5] = MI_USER_INTERRUPT;
    b[6] = MI_BATCH_BUFFER_END;
    CHECK("lengths: a 3D command's length field, then a refused command",
          parse(&rcs, b, 7) == V_UNPRIV);
    b[0] += 1u;
    CHECK("lengths: one dword longer, it holds that dword", parse(&rcs, b, 7) == V_PRIV);
    k_memset(b, 0, sizeof(b));
    b[0] = (3u << 29) | (2u << 27) | (1u << 24) | 0x100u;
    b[2] = MI_USER_INTERRUPT;
    b[0x102] = MI_BATCH_BUFFER_END;
    CHECK("lengths: a media command's 16-bit length field", parse(&rcs, b, 0x103) == V_PRIV);
    k_memset(b, 0, sizeof(b));
    b[0] = MI_ATOMIC | 0x41u;
    b[3] = MI_USER_INTERRUPT;
    b[4] = MI_BATCH_BUFFER_END;
    CHECK("lengths: an MI command's 6-bit length field", parse(&rcs, b, 5) == V_UNPRIV);
    b[0] = XY_SRC_COPY_BLT | 1u;
    CHECK("lengths: a blit's 8-bit length field", parse(&bcs, b, 5) == V_UNPRIV);
    b[0] = XY_SRC_COPY_BLT | 1u;
    CHECK("lengths: a blit on the render ring EINVAL",
          parse(&rcs, b, 5) == V_ERROR && last_ret == -EINVAL);
    b[0] = (3u << 29) | (3u << 27) | 3u;
    CHECK("lengths: a 3D command on the blitter EINVAL",
          parse(&bcs, b, 5) == V_ERROR && last_ret == -EINVAL);
    b[0] = 1u << 29;
    CHECK("lengths: client 1 on the render ring EINVAL",
          parse(&rcs, b, 5) == V_ERROR && last_ret == -EINVAL);

    b[0] = MI_NOOP;
    b[1] = MI_LOAD_REGISTER_IMM(1);
    b[2] = GEN7_SO_WRITE_OFFSET(0);
    b[3] = MI_BATCH_BUFFER_END;
    CHECK("ends: a command past the batch EINVAL",
          parse_at(&rcs, b, 4, 0, 8, 1) == V_ERROR && last_ret == -EINVAL);
    b[1] = MI_NOOP;
    CHECK("ends: a batch without its end EINVAL",
          parse_at(&rcs, b, 4, 0, 8, 1) == V_ERROR && last_ret == -EINVAL);
    b[0] = MI_BATCH_BUFFER_END;
    b[1] = MI_USER_INTERRUPT;
    CHECK("ends: what follows the end is not read", parse(&rcs, b, 2) == V_PRIV);

    pages_make(&p, 2, MI_NOOP);
    *dw(&p, 8188) = MI_BATCH_BUFFER_END;
    flushes = 0;
    CHECK("copy: two pages, flushed first",
          parse_in(&rcs, &p, 0, 8192, I915_CLFLUSH_BEFORE) == V_PRIV && flushes == 2);
    flushes = 0;
    CHECK("copy: not flushed unless asked",
          parse_in(&rcs, &p, 0, 8192, 0) == V_PRIV && flushes == 0);
    map_calls = 0;
    map_fail_at = 2;
    CHECK("copy: a mapping refused ENOMEM",
          parse_in(&rcs, &p, 0, 8192, 0) == V_ERROR && last_ret == -ENOMEM);
    map_calls = 0;
    map_fail_at = 6;
    CHECK("parse: a mapping refused ENOMEM",
          parse_in(&rcs, &p, 0, 8192, 0) == V_ERROR && last_ret == -ENOMEM);
    map_fail_at = 0;
    pages_free(&p);
}

/**
 * The cases of IGT's gen7_exec_parse that run at parser version 10, each
 * with Linux's verdict: an error the call returns, or the batch run
 * privileged from the shadow, or run unprivileged from the original, where
 * the chip turns a refused command into a no-op and the call returns 0.
 * oacontrol-tracking needs a version below 9 and does not run. The
 * basic-rejected cases on the video rings are i915_video_hosttest.c's.
 */
static void igt(void)
{
    uint32_t pc[] = {GFX_OP_PIPE_CONTROL(4), PIPE_CONTROL_QW_WRITE, 0, 0x12000000, 0,
                     MI_BATCH_BUFFER_END};
    uint32_t invalid_cmd[] = {7u << INSTR_CLIENT_SHIFT, MI_BATCH_BUFFER_END};
    uint32_t invalid_set_context[] = {MI_SET_CONTEXT | 32u, MI_BATCH_BUFFER_END};
    uint32_t lri[] = {MI_LOAD_REGISTER_IMM(1), 0, 0, MI_BATCH_BUFFER_END};
    uint32_t bitmasks[] = {
        GFX_OP_PIPE_CONTROL(4), PIPE_CONTROL_QW_WRITE | PIPE_CONTROL_LRI_POST_OP, 0, 0x12000000, 0,
        MI_BATCH_BUFFER_END};
    uint32_t noop[1024];
    uint32_t lri_ok[] = {MI_LOAD_REGISTER_IMM(1), GEN7_SO_WRITE_OFFSET(0), 0xdcbaabc0u,
                         MI_BATCH_BUFFER_END};
    uint32_t store_reg[] = {MI_STORE_REGISTER_MEM, GEN7_SO_WRITE_OFFSET(0), 0, MI_BATCH_BUFFER_END};
    uint32_t first_level[] = {MI_BATCH_BUFFER_START | MI_BATCH_NON_SECURE_I965 |
                                  MI_BATCH_NON_SECURE_HSW,
                              0, MI_BATCH_BUFFER_END, 0};
    struct pages p;
    uint32_t i, at;
    int bad = 0;

    CHECK("igt: the version is 10", I915_CMD_PARSER_VERSION == 10);
    CHECK("igt: basic-allowed runs privileged", parse(&rcs, pc, N(pc)) == V_PRIV);
    for (at = 0; at <= 4096u - sizeof(pc); at += 8) {
        bad += parse_at(&rcs, pc, N(pc), at, sizeof(pc), 1) != V_PRIV;
    }
    CHECK("igt: basic-offset runs privileged at each offset", bad == 0);
    CHECK("igt: basic-rejected, an invalid client on the render ring EINVAL",
          parse(&rcs, invalid_cmd, N(invalid_cmd)) == V_ERROR && last_ret == -EINVAL);
    CHECK("igt: basic-rejected, an invalid client on the blitter EINVAL",
          parse(&bcs, invalid_cmd, N(invalid_cmd)) == V_ERROR && last_ret == -EINVAL);
    CHECK("igt: basic-rejected, MI_SET_CONTEXT of a bad length EINVAL",
          parse(&rcs, invalid_set_context, N(invalid_set_context)) == V_ERROR &&
              last_ret == -EINVAL);

    lri[1] = OASTATUS2;
    lri[2] = 0xbeeff000u;
    CHECK("igt: test-lri-OASTATUS2 runs unprivileged, the chip refusing it",
          parse(&rcs, lri, N(lri)) == V_UNPRIV);
    lri[1] = GEN7_SO_WRITE_OFFSET(0);
    lri[2] = 0xbeefbee0u;
    CHECK("igt: test-lri-SO_WRITE_OFFSET_0 runs privileged", parse(&rcs, lri, N(lri)) == V_PRIV);
    lri[1] = OACONTROL;
    lri[2] = 0x31337000u;
    CHECK("igt: test-lri-OACONTROL runs unprivileged", parse(&rcs, lri, N(lri)) == V_UNPRIV);
    CHECK("igt: bitmasks runs unprivileged", parse(&rcs, bitmasks, N(bitmasks)) == V_UNPRIV);

    for (i = 0; i < N(noop); i++) {
        noop[i] = MI_NOOP;
    }
    CHECK("igt: batch-without-end EINVAL",
          parse(&rcs, noop, N(noop)) == V_ERROR && last_ret == -EINVAL);

    pages_make(&p, 2, MI_NOOP);
    for (i = 0; i < N(lri_ok); i++) {
        *dw(&p, 4092u + 4u * i) = lri_ok[i];
    }
    CHECK("igt: cmd-crossing-page runs privileged",
          parse_in(&rcs, &p, 4088, 24, 0) == V_PRIV && *dw(&shadow, 4) == lri_ok[0] &&
              *dw(&shadow, 8) == lri_ok[1] && *dw(&shadow, 12) == lri_ok[2]);
    pages_free(&p);
    CHECK("igt: cmd-crossing-page's read back runs privileged",
          parse(&rcs, store_reg, N(store_reg)) == V_PRIV);
    CHECK("igt: chained-batch, the first level runs unprivileged and the chip chains",
          parse(&rcs, first_level, N(first_level)) == V_UNPRIV);
    CHECK("igt: chained-batch, the second level alone runs privileged",
          parse(&rcs, pc, N(pc)) == V_PRIV);
}

/** IGT's load-register-reg and basic-allocation, as igt() above. */
static void igt_more(void)
{
    uint32_t init_gpr0[16] = {MI_LOAD_REGISTER_IMM(1), GEN8_RING_CS_GPR(RENDER_RING_BASE, 0),
                              0xabcdabc0u, MI_BATCH_BUFFER_END};
    uint32_t store_gpr0[16] = {MI_STORE_REGISTER_MEM, GEN8_RING_CS_GPR(RENDER_RING_BASE, 0), 0,
                               MI_BATCH_BUFFER_END};
    uint32_t do_lrr[16] = {MI_LOAD_REGISTER_REG, 0, GEN8_RING_CS_GPR(RENDER_RING_BASE, 0),
                           MI_BATCH_BUFFER_END};
    uint32_t allowed[] = {GEN8_RING_CS_GPR(RENDER_RING_BASE, 1), GEN7_SO_WRITE_OFFSET(0)};
    uint32_t disallowed[] = {0, OACONTROL, DERRMR, RING_START0};
    struct pages p;
    uint32_t i, size, start, k;
    int bad;

    CHECK("igt: load-register-reg's GPR0 set and read run privileged",
          parse(&rcs, init_gpr0, N(init_gpr0)) == V_PRIV &&
              parse(&rcs, store_gpr0, N(store_gpr0)) == V_PRIV);
    bad = 0;
    for (i = 0; i < N(allowed); i++) {
        do_lrr[1] = allowed[i];
        bad += parse(&rcs, do_lrr, N(do_lrr)) != V_PRIV;
    }
    CHECK("igt: load-register-reg from each allowed register runs privileged", bad == 0);
    bad = 0;
    for (i = 0; i < N(disallowed); i++) {
        do_lrr[1] = disallowed[i];
        bad += parse(&rcs, do_lrr, N(do_lrr)) != V_UNPRIV;
    }
    CHECK("igt: load-register-reg from each other register runs unprivileged", bad == 0);

    /* basic-allocation: objects of 1 to 16 pages, an end in the last dword
     * of each page, started at a page and 64-byte steps into it, to the
     * object's end. */
    bad = 0;
    for (i = 0; i <= 4; i++) {
        size = PAGE << i;
        pages_make(&p, size / PAGE, MI_NOOP);
        for (k = PAGE; k <= size; k += PAGE) {
            *dw(&p, k - 4u) = MI_BATCH_BUFFER_END;
        }
        for (start = 0; start < size; start += PAGE) {
            bad += parse_in(&rcs, &p, start, size - start, 0) != V_PRIV;
            bad += parse_in(&rcs, &p, start + 64u, size - start - 64u, 0) != V_PRIV;
            bad += parse_in(&rcs, &p, start + 63u * 64u, size - start - 63u * 64u, 0) != V_PRIV;
        }
        pages_free(&p);
    }
    CHECK("igt: basic-allocation runs privileged at each start", bad == 0);
}

int main(void)
{
    rcs.name = "rcs0";
    rcs.mmio_base = RENDER_RING_BASE;
    bcs.name = "bcs0";
    bcs.mmio_base = BLT_RING_BASE;
    none.name = "none";
    none.mmio_base = BSD_RING_BASE;
    tables();
    checks_regs();
    checks_bits();
    lengths();
    igt();
    igt_more();
    pages_free(&shadow);
    CHECK("the end: no temporary mapping held", maps_live == 0);
    printf("i915-parser-test: %s\n", fails ? "failed" : "ok");
    return fails != 0;
}
