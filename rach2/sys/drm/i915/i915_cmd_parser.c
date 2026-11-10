/*
 * i915_cmd_parser.c: the check of a program's batch before the render or
 * blitter ring runs it, after Linux 6.12's i915_cmd_parser.c on Haswell:
 * the batch copied to a shadow, its commands and registers checked against
 * Linux's tables, and the trampoline that runs a refused batch unprivileged.
 */

#include "i915.h"
#include "i915_reg.h"

#include "../../lib/kerrno.h"
#include "../../lib/kstring.h"
#include "../../mm/paging.h"

/*
 * Why the parser exists. Haswell's command streamer refuses some commands
 * in an unprivileged batch, every program's, by turning them into no-ops:
 * MI_LOAD_REGISTER_IMM among them, which OpenGL's transform feedback and
 * Vulkan's queries need. The parser lets those through when it finds them
 * safe and runs the batch privileged; one it does not find safe runs as
 * the chip would run it without the parser. hasvk refuses a Haswell whose
 * GETPARAM gives no parser version (anv_device.c).
 */

/* How the parser treats a command, Linux's CMD_DESC_: a fixed length, or a
 * length field the descriptor's mask takes; passed over; refused; its
 * register checked against the engine's allowlist; its dwords checked
 * against the descriptor's masks. */
#define CMD_DESC_FIXED    (1u << 0)
#define CMD_DESC_SKIP     (1u << 1)
#define CMD_DESC_REJECT   (1u << 2)
#define CMD_DESC_REGISTER (1u << 3)
#define CMD_DESC_BITMASK  (1u << 4)

/* The masked checks of one descriptor, Linux's MAX_CMD_DESC_BITMASKS. */
#define CMD_BITMASKS 3

/* What a command's length field holds: its dwords less two. */
#define LENGTH_BIAS 2u

/*
 * A command the parser handles, Linux's drm_i915_cmd_descriptor. A command
 * is the descriptor's when its first dword masked by `mask` is `value`.
 * `length` is its dwords with CMD_DESC_FIXED, else the mask of its length
 * field. With CMD_DESC_REGISTER, `reg` says where its register addresses
 * are: the dword `offset`, masked by `mask`, and every `step` dwords after
 * it while inside the command, or only the one for a `step` of 0. With
 * CMD_DESC_BITMASK, each of `bits` with a mask refuses the command unless
 * its dword `offset` masked by `mask` is `expected`; one with a
 * `condition_mask` applies only when the dword `condition_offset` has a bit
 * of that mask set.
 */
struct cmd_desc {
    uint32_t flags;
    uint32_t value;
    uint32_t mask;
    uint32_t length;
    struct {
        uint32_t offset;
        uint32_t mask;
        uint32_t step;
    } reg;
    struct {
        uint32_t offset;
        uint32_t mask;
        uint32_t expected;
        uint32_t condition_offset;
        uint32_t condition_mask;
    } bits[CMD_BITMASKS];
};

/* A table of descriptors, sorted by value. */
struct cmd_table {
    const struct cmd_desc *desc;
    uint32_t count;
};

/*
 * A register a batch may reach, Linux's drm_i915_reg_descriptor. With a
 * `mask`, only MI_LOAD_REGISTER_IMM may write it, and only a value that is
 * `value` under the mask.
 */
struct reg_desc {
    uint32_t addr;
    uint32_t mask;
    uint32_t value;
};

/* An allowlist, sorted by address. */
struct reg_table {
    const struct reg_desc *regs;
    uint32_t count;
};

/*
 * The tables of an engine, searched in order, and the mask of the length
 * field of a command they do not hold, from its client: 0 for a client the
 * engine does not run.
 */
struct cmd_engine {
    const struct cmd_table *cmds;
    uint32_t ncmds;
    const struct reg_table *regs;
    uint32_t nregs;
    uint32_t (*length_mask)(uint32_t header);
};

/* The opcode's bits of each client, Linux's STD_*_OPCODE_SHIFT, and the
 * least a command's opcode takes. */
#define SMI              (32 - 9)
#define S3D              (32 - 16)
#define S2D              (32 - 10)
#define MIN_OPCODE_SHIFT 16

/* The columns of the tables, as Linux writes them. */
#define F 1
#define S CMD_DESC_SKIP
#define R CMD_DESC_REJECT
#define W CMD_DESC_REGISTER
#define B CMD_DESC_BITMASK

#define NOREG                      {0u, 0u, 0u}
#define REGAT(off, m, step)        {(off), (m), (step)}
#define NOBIT                      {0u, 0u, 0u, 0u, 0u}
#define BIT(off, m, e)             {(off), (m), (e), 0u, 0u}
#define BITIF(off, m, e, coff, cm) {(off), (m), (e), (coff), (cm)}
#define NOBITS                     {NOBIT, NOBIT, NOBIT}
#define BITS1(a)                   {a, NOBIT, NOBIT}
#define BITS2(a, b)                {a, b, NOBIT}
#define BITS3(a, b, c)             {a, b, c}

#define CMD(op, opm, fixed, len, action, reg, bits)        \
    {(uint32_t)(action) | ((fixed) ? CMD_DESC_FIXED : 0u), \
     (uint32_t)(op) & (~0u << (opm)),                      \
     ~0u << (opm),                                         \
     (len),                                                \
     reg,                                                  \
     bits}

/* Where MI_LOAD_REGISTER_IMM, MI_STORE_REGISTER_MEM and
 * MI_LOAD_REGISTER_MEM hold their register addresses. */
#define REG_LRI REGAT(1u, 0x007ffffcu, 2u)
#define REG_ONE REGAT(1u, 0x007ffffcu, 0u)
#define REG_LRR REGAT(1u, 0x007ffffcu, 1u)

/* The global GTT bit of a command, which an unprivileged batch must not
 * set. */
#define NO_GGTT BITS1(BIT(0u, MI_USE_GGTT, 0u))

/* The checks of MI_FLUSH_DW on the blitter: no interrupt, and no store
 * through the global GTT or into the status page. */
#define FLUSH_DW_BITS                                                  \
    BITS3(BIT(0u, MI_FLUSH_DW_NOTIFY, 0u),                             \
          BITIF(1u, MI_FLUSH_DW_USE_GTT, 0u, 0u, MI_FLUSH_DW_OP_MASK), \
          BITIF(0u, MI_FLUSH_DW_STORE_INDEX, 0u, 0u, MI_FLUSH_DW_OP_MASK))

/* Linux's gen7_common_cmds: the MI commands of every engine. */
static const struct cmd_desc gen7_common_cmds[] = {
    CMD(MI_NOOP, SMI, F, 1u, S, NOREG, NOBITS),
    CMD(MI_USER_INTERRUPT, SMI, F, 1u, R, NOREG, NOBITS),
    CMD(MI_WAIT_FOR_EVENT, SMI, F, 1u, R, NOREG, NOBITS),
    CMD(MI_ARB_CHECK, SMI, F, 1u, S, NOREG, NOBITS),
    CMD(MI_REPORT_HEAD, SMI, F, 1u, S, NOREG, NOBITS),
    CMD(MI_SUSPEND_FLUSH, SMI, F, 1u, S, NOREG, NOBITS),
    CMD(MI_SEMAPHORE_MBOX, SMI, 0, 0xffu, R, NOREG, NOBITS),
    CMD(MI_STORE_DWORD_INDEX, SMI, 0, 0xffu, R, NOREG, NOBITS),
    CMD(MI_LOAD_REGISTER_IMM(1), SMI, 0, 0xffu, W, REG_LRI, NOBITS),
    CMD(MI_STORE_REGISTER_MEM, SMI, F, 3u, W | B, REG_ONE, NO_GGTT),
    CMD(MI_LOAD_REGISTER_MEM, SMI, F, 3u, W | B, REG_ONE, NO_GGTT),
    /* Passed by the tables but never followed: see i915_cmd_parser. */
    CMD(MI_BATCH_BUFFER_START, SMI, 0, 0xffu, S, NOREG, NOBITS),
};

/* Linux's gen7_render_cmds. */
static const struct cmd_desc gen7_render_cmds[] = {
    CMD(MI_FLUSH, SMI, F, 1u, S, NOREG, NOBITS),
    CMD(MI_ARB_ON_OFF, SMI, F, 1u, R, NOREG, NOBITS),
    CMD(MI_PREDICATE, SMI, F, 1u, S, NOREG, NOBITS),
    CMD(MI_TOPOLOGY_FILTER, SMI, F, 1u, S, NOREG, NOBITS),
    CMD(MI_SET_APPID, SMI, F, 1u, S, NOREG, NOBITS),
    CMD(MI_DISPLAY_FLIP, SMI, 0, 0xffu, R, NOREG, NOBITS),
    CMD(MI_SET_CONTEXT, SMI, 0, 0xffu, R, NOREG, NOBITS),
    CMD(MI_URB_CLEAR, SMI, 0, 0xffu, S, NOREG, NOBITS),
    CMD(MI_STORE_DWORD_IMM, SMI, 0, 0x3fu, B, NOREG, NO_GGTT),
    CMD(MI_UPDATE_GTT, SMI, 0, 0xffu, R, NOREG, NOBITS),
    CMD(MI_CLFLUSH, SMI, 0, 0x3ffu, B, NOREG, NO_GGTT),
    CMD(MI_REPORT_PERF_COUNT, SMI, 0, 0x3fu, B, NOREG,
        BITS1(BIT(1u, MI_REPORT_PERF_COUNT_GGTT, 0u))),
    CMD(MI_CONDITIONAL_BATCH_BUFFER_END, SMI, 0, 0xffu, B, NOREG, NO_GGTT),
    CMD(GFX_OP_3DSTATE_VF_STATISTICS, S3D, F, 1u, S, NOREG, NOBITS),
    CMD(PIPELINE_SELECT, S3D, F, 1u, S, NOREG, NOBITS),
    CMD(MEDIA_VFE_STATE, S3D, 0, 0xffffu, B, NOREG,
        BITS1(BIT(2u, MEDIA_VFE_STATE_MMIO_ACCESS_MASK, 0u))),
    CMD(GPGPU_OBJECT, S3D, 0, 0xffu, S, NOREG, NOBITS),
    CMD(GPGPU_WALKER, S3D, 0, 0xffu, S, NOREG, NOBITS),
    CMD(GFX_OP_3DSTATE_SO_DECL_LIST, S3D, 0, 0x1ffu, S, NOREG, NOBITS),
    CMD(GFX_OP_PIPE_CONTROL(5), S3D, 0, 0xffu, B, NOREG,
        BITS2(BIT(1u, PIPE_CONTROL_MMIO_WRITE | PIPE_CONTROL_NOTIFY, 0u),
              BITIF(1u, PIPE_CONTROL_GLOBAL_GTT_IVB | PIPE_CONTROL_STORE_DATA_INDEX, 0u, 1u,
                    PIPE_CONTROL_POST_SYNC_OP_MASK))),
};

/* Linux's hsw_render_cmds. */
static const struct cmd_desc hsw_render_cmds[] = {
    CMD(MI_SET_PREDICATE, SMI, F, 1u, S, NOREG, NOBITS),
    CMD(MI_RS_CONTROL, SMI, F, 1u, S, NOREG, NOBITS),
    CMD(MI_URB_ATOMIC_ALLOC, SMI, F, 1u, S, NOREG, NOBITS),
    CMD(MI_SET_APPID, SMI, F, 1u, S, NOREG, NOBITS),
    CMD(MI_RS_CONTEXT, SMI, F, 1u, S, NOREG, NOBITS),
    CMD(MI_LOAD_SCAN_LINES_INCL, SMI, 0, 0x3fu, R, NOREG, NOBITS),
    CMD(MI_LOAD_SCAN_LINES_EXCL, SMI, 0, 0x3fu, R, NOREG, NOBITS),
    CMD(MI_LOAD_REGISTER_REG, SMI, 0, 0xffu, W, REG_LRR, NOBITS),
    CMD(MI_RS_STORE_DATA_IMM, SMI, 0, 0xffu, S, NOREG, NOBITS),
    CMD(MI_LOAD_URB_MEM, SMI, 0, 0xffu, S, NOREG, NOBITS),
    CMD(MI_STORE_URB_MEM, SMI, 0, 0xffu, S, NOREG, NOBITS),
    CMD(GFX_OP_3DSTATE_DX9_CONSTANTF_VS, S3D, 0, 0x7ffu, S, NOREG, NOBITS),
    CMD(GFX_OP_3DSTATE_DX9_CONSTANTF_PS, S3D, 0, 0x7ffu, S, NOREG, NOBITS),
    CMD(GFX_OP_3DSTATE_BINDING_TABLE_EDIT_VS, S3D, 0, 0x1ffu, S, NOREG, NOBITS),
    CMD(GFX_OP_3DSTATE_BINDING_TABLE_EDIT_GS, S3D, 0, 0x1ffu, S, NOREG, NOBITS),
    CMD(GFX_OP_3DSTATE_BINDING_TABLE_EDIT_HS, S3D, 0, 0x1ffu, S, NOREG, NOBITS),
    CMD(GFX_OP_3DSTATE_BINDING_TABLE_EDIT_DS, S3D, 0, 0x1ffu, S, NOREG, NOBITS),
    CMD(GFX_OP_3DSTATE_BINDING_TABLE_EDIT_PS, S3D, 0, 0x1ffu, S, NOREG, NOBITS),
};

/* Linux's gen7_blt_cmds. */
static const struct cmd_desc gen7_blt_cmds[] = {
    CMD(MI_DISPLAY_FLIP, SMI, 0, 0xffu, R, NOREG, NOBITS),
    CMD(MI_STORE_DWORD_IMM, SMI, 0, 0x3ffu, B, NOREG, NO_GGTT),
    CMD(MI_UPDATE_GTT, SMI, 0, 0x3fu, R, NOREG, NOBITS),
    CMD(MI_FLUSH_DW, SMI, 0, 0x3fu, B, NOREG, FLUSH_DW_BITS),
    CMD(COLOR_BLT, S2D, 0, 0x3fu, S, NOREG, NOBITS),
    CMD(SRC_COPY_BLT, S2D, 0, 0x3fu, S, NOREG, NOBITS),
};

/* Linux's hsw_blt_cmds. */
static const struct cmd_desc hsw_blt_cmds[] = {
    CMD(MI_LOAD_SCAN_LINES_INCL, SMI, 0, 0x3fu, R, NOREG, NOBITS),
    CMD(MI_LOAD_SCAN_LINES_EXCL, SMI, 0, 0x3fu, R, NOREG, NOBITS),
};

/* Linux's noop_desc, where the search for each batch starts. */
static const struct cmd_desc noop_desc = CMD(MI_NOOP, SMI, F, 1u, S, NOREG, NOBITS);

#undef CMD
#undef F
#undef S
#undef R
#undef W
#undef B

/* An allowlist's rows: a 32-bit register, one written only under a mask,
 * and both halves of a 64-bit one. */
#define REG32(r)        {(r), 0u, 0u}
#define REG32M(r, m, v) {(r), (m), (v)}
#define REG64(r)        {(r), 0u, 0u}, {(r) + 4u, 0u, 0u}

/* Linux's gen7_render_regs. */
static const struct reg_desc gen7_render_regs[] = {
    REG64(GPGPU_THREADS_DISPATCHED),
    REG64(HS_INVOCATION_COUNT),
    REG64(DS_INVOCATION_COUNT),
    REG64(IA_VERTICES_COUNT),
    REG64(IA_PRIMITIVES_COUNT),
    REG64(VS_INVOCATION_COUNT),
    REG64(GS_INVOCATION_COUNT),
    REG64(GS_PRIMITIVES_COUNT),
    REG64(CL_INVOCATION_COUNT),
    REG64(CL_PRIMITIVES_COUNT),
    REG64(PS_INVOCATION_COUNT),
    REG64(PS_DEPTH_COUNT),
    REG64(RING_TIMESTAMP(RENDER_RING_BASE)),
    REG64(MI_PREDICATE_SRC0(RENDER_RING_BASE)),
    REG64(MI_PREDICATE_SRC1(RENDER_RING_BASE)),
    REG32(GEN7_3DPRIM_END_OFFSET),
    REG32(GEN7_3DPRIM_START_VERTEX),
    REG32(GEN7_3DPRIM_VERTEX_COUNT),
    REG32(GEN7_3DPRIM_INSTANCE_COUNT),
    REG32(GEN7_3DPRIM_START_INSTANCE),
    REG32(GEN7_3DPRIM_BASE_VERTEX),
    REG32(GEN7_GPGPU_DISPATCHDIMX),
    REG32(GEN7_GPGPU_DISPATCHDIMY),
    REG32(GEN7_GPGPU_DISPATCHDIMZ),
    REG64(RING_TIMESTAMP(BSD_RING_BASE)),
    REG64(GEN7_SO_NUM_PRIMS_WRITTEN(0)),
    REG64(GEN7_SO_NUM_PRIMS_WRITTEN(1)),
    REG64(GEN7_SO_NUM_PRIMS_WRITTEN(2)),
    REG64(GEN7_SO_NUM_PRIMS_WRITTEN(3)),
    REG64(GEN7_SO_PRIM_STORAGE_NEEDED(0)),
    REG64(GEN7_SO_PRIM_STORAGE_NEEDED(1)),
    REG64(GEN7_SO_PRIM_STORAGE_NEEDED(2)),
    REG64(GEN7_SO_PRIM_STORAGE_NEEDED(3)),
    REG32(GEN7_SO_WRITE_OFFSET(0)),
    REG32(GEN7_SO_WRITE_OFFSET(1)),
    REG32(GEN7_SO_WRITE_OFFSET(2)),
    REG32(GEN7_SO_WRITE_OFFSET(3)),
    REG32(GEN7_L3SQCREG1),
    REG32(GEN7_L3CNTLREG2),
    REG32(GEN7_L3CNTLREG3),
    REG64(RING_TIMESTAMP(BLT_RING_BASE)),
};

/* Linux's hsw_render_regs: the command streamer's sixteen general purpose
 * registers and the L3 atomics' chicken bits, which a batch may only clear
 * or set alone. */
static const struct reg_desc hsw_render_regs[] = {
    REG64(GEN8_RING_CS_GPR(RENDER_RING_BASE, 0)),
    REG64(GEN8_RING_CS_GPR(RENDER_RING_BASE, 1)),
    REG64(GEN8_RING_CS_GPR(RENDER_RING_BASE, 2)),
    REG64(GEN8_RING_CS_GPR(RENDER_RING_BASE, 3)),
    REG64(GEN8_RING_CS_GPR(RENDER_RING_BASE, 4)),
    REG64(GEN8_RING_CS_GPR(RENDER_RING_BASE, 5)),
    REG64(GEN8_RING_CS_GPR(RENDER_RING_BASE, 6)),
    REG64(GEN8_RING_CS_GPR(RENDER_RING_BASE, 7)),
    REG64(GEN8_RING_CS_GPR(RENDER_RING_BASE, 8)),
    REG64(GEN8_RING_CS_GPR(RENDER_RING_BASE, 9)),
    REG64(GEN8_RING_CS_GPR(RENDER_RING_BASE, 10)),
    REG64(GEN8_RING_CS_GPR(RENDER_RING_BASE, 11)),
    REG64(GEN8_RING_CS_GPR(RENDER_RING_BASE, 12)),
    REG64(GEN8_RING_CS_GPR(RENDER_RING_BASE, 13)),
    REG64(GEN8_RING_CS_GPR(RENDER_RING_BASE, 14)),
    REG64(GEN8_RING_CS_GPR(RENDER_RING_BASE, 15)),
    REG32M(HSW_SCRATCH1, ~HSW_SCRATCH1_L3_DATA_ATOMICS_DISABLE, 0u),
    REG32M(HSW_ROW_CHICKEN3,
           ~(HSW_ROW_CHICKEN3_L3_GLOBAL_ATOMICS_DISABLE << 16 |
             HSW_ROW_CHICKEN3_L3_GLOBAL_ATOMICS_DISABLE),
           0u),
};

/* Linux's gen7_blt_regs. */
static const struct reg_desc gen7_blt_regs[] = {
    REG64(RING_TIMESTAMP(RENDER_RING_BASE)),
    REG64(RING_TIMESTAMP(BSD_RING_BASE)),
    REG32(BCS_SWCTRL),
    REG64(RING_TIMESTAMP(BLT_RING_BASE)),
};

#undef REG32
#undef REG32M
#undef REG64

#define COUNT(a) ((uint32_t)(sizeof(a) / sizeof((a)[0])))

/* Linux's hsw_render_ring_cmd_table and hsw_render_reg_tables. */
static const struct cmd_table hsw_render_cmd_tables[] = {
    {gen7_common_cmds, COUNT(gen7_common_cmds)},
    {gen7_render_cmds, COUNT(gen7_render_cmds)},
    {hsw_render_cmds, COUNT(hsw_render_cmds)},
};

static const struct reg_table hsw_render_reg_tables[] = {
    {gen7_render_regs, COUNT(gen7_render_regs)},
    {hsw_render_regs, COUNT(hsw_render_regs)},
};

/* Linux's hsw_blt_ring_cmd_table and hsw_blt_reg_tables. */
static const struct cmd_table hsw_blt_cmd_tables[] = {
    {gen7_common_cmds, COUNT(gen7_common_cmds)},
    {gen7_blt_cmds, COUNT(gen7_blt_cmds)},
    {hsw_blt_cmds, COUNT(hsw_blt_cmds)},
};

static const struct reg_table hsw_blt_reg_tables[] = {
    {gen7_blt_regs, COUNT(gen7_blt_regs)},
};

/** The length field's mask of a render command no table holds, Linux's
 * gen7_render_get_cmd_length_mask: 0 for a client the ring does not run. */
static uint32_t render_length_mask(uint32_t header)
{
    uint32_t client = header >> INSTR_CLIENT_SHIFT;
    uint32_t subclient = (header & INSTR_SUBCLIENT_MASK) >> INSTR_SUBCLIENT_SHIFT;

    if (client == INSTR_MI_CLIENT) {
        return 0x3fu;
    }
    if (client == INSTR_RC_CLIENT) {
        return subclient == INSTR_MEDIA_SUBCLIENT ? 0xffffu : 0xffu;
    }
    return 0;
}

/** The length field's mask of a blitter command no table holds, Linux's
 * gen7_blt_get_cmd_length_mask: 0 for a client the ring does not run. */
static uint32_t blt_length_mask(uint32_t header)
{
    uint32_t client = header >> INSTR_CLIENT_SHIFT;

    if (client == INSTR_MI_CLIENT) {
        return 0x3fu;
    }
    if (client == INSTR_BC_CLIENT) {
        return 0xffu;
    }
    return 0;
}

static const struct cmd_engine hsw_render_engine = {
    hsw_render_cmd_tables, COUNT(hsw_render_cmd_tables), hsw_render_reg_tables,
    COUNT(hsw_render_reg_tables), render_length_mask};

static const struct cmd_engine hsw_blt_engine = {hsw_blt_cmd_tables, COUNT(hsw_blt_cmd_tables),
                                                 hsw_blt_reg_tables, COUNT(hsw_blt_reg_tables),
                                                 blt_length_mask};

/** The tables of `engine`, or NULL for one that parses nothing. */
static const struct cmd_engine *cmd_engine_of(const struct i915_engine *engine)
{
    if (engine->mmio_base == RENDER_RING_BASE) {
        return &hsw_render_engine;
    }
    if (engine->mmio_base == BLT_RING_BASE) {
        return &hsw_blt_engine;
    }
    return 0;
}

int i915_engine_using_cmd_parser(const struct i915_engine *engine)
{
    return cmd_engine_of(engine) != 0;
}

/*
 * The shadow as the parse reads and writes it: one page mapped at a time,
 * `page` its index, ~0 for none. `err` is -ENOMEM once a page could not be
 * mapped; a read then gives 0 and a write does nothing.
 */
struct cmd_cursor {
    const uintptr_t *frames;
    uint32_t page;
    uint32_t *va;
    int32_t err;
};

/** Where dword `k` of the shadow is, its page mapped; NULL after a failed
 * mapping. */
static uint32_t *cursor_at(struct cmd_cursor *c, uint32_t k)
{
    uint32_t page = k / (I915_GTT_PAGE_SIZE / 4u);

    if (c->err) {
        return 0;
    }
    if (page != c->page) {
        if (c->va) {
            kunmap_temp(c->va);
        }
        c->va = (uint32_t *)kmap_temp(c->frames[page]);
        c->page = page;
        if (!c->va) {
            c->err = -ENOMEM;
            return 0;
        }
    }
    return c->va + k % (I915_GTT_PAGE_SIZE / 4u);
}

/** Dword `k` of the shadow, or 0 after a failed mapping. */
static uint32_t cursor_get(struct cmd_cursor *c, uint32_t k)
{
    uint32_t *p = cursor_at(c, k);

    return p ? *p : 0;
}

/** Write `v` to dword `k` of the shadow. */
static void cursor_put(struct cmd_cursor *c, uint32_t k, uint32_t v)
{
    uint32_t *p = cursor_at(c, k);

    if (p) {
        *p = v;
    }
}

/** Let the cursor's page go. */
static void cursor_end(struct cmd_cursor *c)
{
    if (c->va) {
        kunmap_temp(c->va);
    }
    c->va = 0;
    c->page = ~0u;
}

/**
 * Copy the batch to the start of its shadow and zero the rest, as Linux's
 * copy_batch: the batch's lines flushed first when `clflush` says so.
 * 0, or -ENOMEM without a temporary mapping.
 */
static int32_t copy_batch(const struct i915_cmd_parse *p)
{
    uint32_t done = 0, src_at, dst_at, n, room;
    uint8_t *src, *dst;

    while (done < p->shadow_size) {
        dst_at = done % I915_GTT_PAGE_SIZE;
        n = I915_GTT_PAGE_SIZE - dst_at;
        n = n < p->shadow_size - done ? n : p->shadow_size - done;
        dst = (uint8_t *)kmap_temp(p->shadow[done / I915_GTT_PAGE_SIZE]);
        if (!dst) {
            return -ENOMEM;
        }
        if (done < p->len) {
            src_at = (p->offset + done) % I915_GTT_PAGE_SIZE;
            room = I915_GTT_PAGE_SIZE - src_at;
            n = n < room ? n : room;
            n = n < p->len - done ? n : p->len - done;
            src = (uint8_t *)kmap_temp(p->batch[(p->offset + done) / I915_GTT_PAGE_SIZE]);
            if (!src) {
                kunmap_temp(dst);
                return -ENOMEM;
            }
            if (p->clflush & I915_CLFLUSH_BEFORE) {
                arch_dma_flush(src + src_at, n);
            }
            k_memcpy(dst + dst_at, src + src_at, n);
            kunmap_temp(src);
        } else {
            k_memset(dst + dst_at, 0, n);
        }
        kunmap_temp(dst);
        done += n;
    }
    return 0;
}

/** Whether `desc` is the descriptor of the command `cmd`, Linux's
 * cmd_desc_is. */
static int cmd_desc_is(const struct cmd_desc *desc, uint32_t cmd)
{
    return desc->value == (cmd & desc->mask);
}

/**
 * The descriptor of the command whose first dword is `header`, as Linux's
 * find_cmd: `desc`, the last command's, when it matches, which makes a run
 * of one command cheap; else the last of the engine's entries that matches,
 * in the order Linux's hash table gives them; else `def`, filled as a
 * command to pass over whose length field the client's mask gives. NULL
 * for a client the engine does not run.
 */
static const struct cmd_desc *find_cmd(const struct cmd_engine *t, uint32_t header,
                                       const struct cmd_desc *desc, struct cmd_desc *def)
{
    const struct cmd_table *table;
    uint32_t i, j, mask;

    if (((header ^ desc->value) & desc->mask) == 0) {
        return desc;
    }
    for (i = t->ncmds; i-- > 0;) {
        table = &t->cmds[i];
        for (j = table->count; j-- > 0;) {
            if (((header ^ table->desc[j].value) & table->desc[j].mask) == 0) {
                return &table->desc[j];
            }
        }
    }
    mask = t->length_mask(header);
    if (!mask) {
        return 0;
    }
    def->value = header;
    def->mask = ~0u << MIN_OPCODE_SHIFT;
    def->length = mask;
    def->flags = CMD_DESC_SKIP;
    return def;
}

/** The engine's allowlist entry for the register at `addr`, or NULL, by a
 * binary search of each table, as Linux's find_reg. */
static const struct reg_desc *find_reg(const struct cmd_engine *t, uint32_t addr)
{
    const struct reg_table *table;
    uint32_t i, lo, hi, mid;

    for (i = 0; i < t->nregs; i++) {
        table = &t->regs[i];
        lo = 0;
        hi = table->count;
        while (lo < hi) {
            mid = lo + (hi - lo) / 2u;
            if (addr < table->regs[mid].addr) {
                hi = mid;
            } else if (addr > table->regs[mid].addr) {
                lo = mid + 1u;
            } else {
                return &table->regs[mid];
            }
        }
    }
    return 0;
}

/**
 * Whether the command of `length` dwords at dword `at` of the shadow
 * passes its descriptor's checks, as Linux's check_cmd: refused outright,
 * or with a register outside the allowlist, or a masked register written
 * by MI_LOAD_REGISTER_MEM or MI_LOAD_REGISTER_REG, or by
 * MI_LOAD_REGISTER_IMM with a value its mask refuses, or with a dword its
 * masks refuse.
 */
static int check_cmd(const struct cmd_engine *t, const struct cmd_desc *desc, struct cmd_cursor *c,
                     uint32_t at, uint32_t length)
{
    const struct reg_desc *reg;
    uint32_t step, offset, addr, i, dword;

    if (desc->flags & CMD_DESC_SKIP) {
        return 1;
    }
    if (desc->flags & CMD_DESC_REJECT) {
        return 0;
    }
    if (desc->flags & CMD_DESC_REGISTER) {
        step = desc->reg.step ? desc->reg.step : length;
        for (offset = desc->reg.offset; offset < length; offset += step) {
            addr = cursor_get(c, at + offset) & desc->reg.mask;
            reg = find_reg(t, addr);
            if (!reg) {
                return 0;
            }
            if (!reg->mask) {
                continue;
            }
            if (cmd_desc_is(desc, MI_LOAD_REGISTER_MEM) ||
                cmd_desc_is(desc, MI_LOAD_REGISTER_REG)) {
                return 0;
            }
            if (cmd_desc_is(desc, MI_LOAD_REGISTER_IMM(1)) &&
                (offset + 2u > length ||
                 (cursor_get(c, at + offset + 1u) & reg->mask) != reg->value)) {
                return 0;
            }
        }
    }
    if (desc->flags & CMD_DESC_BITMASK) {
        for (i = 0; i < CMD_BITMASKS && desc->bits[i].mask; i++) {
            if (desc->bits[i].condition_mask &&
                !(cursor_get(c, at + desc->bits[i].condition_offset) &
                  desc->bits[i].condition_mask)) {
                continue;
            }
            if (desc->bits[i].offset >= length) {
                return 0;
            }
            dword = cursor_get(c, at + desc->bits[i].offset) & desc->bits[i].mask;
            if (dword != desc->bits[i].expected) {
                return 0;
            }
        }
    }
    return 1;
}

/*
 * The parse, as Linux's intel_engine_cmd_parser with the trampoline that
 * graphics 7 always has. Its jump allowlist is never made there, so an
 * MI_BATCH_BUFFER_START is refused as a privileged command is: the chip
 * follows the chain in the original batch, unprivileged. A refused batch
 * is -EACCES inside, which the trampoline turns into a run of the
 * original; every other failure is the call's.
 */
int32_t i915_cmd_parser(const struct i915_engine *engine, const struct i915_cmd_parse *p)
{
    const struct cmd_engine *t = cmd_engine_of(engine);
    struct cmd_desc def = noop_desc;
    const struct cmd_desc *desc = &def;
    struct cmd_cursor c;
    uint32_t at = 0, end = p->len / 4u, length, header;
    int32_t ret = 0;

    if (!t) {
        return -EINVAL;
    }
    ret = copy_batch(p);
    if (ret != 0) {
        return ret;
    }
    c.frames = p->shadow;
    c.page = ~0u;
    c.va = 0;
    c.err = 0;
    for (;;) {
        header = cursor_get(&c, at);
        if (c.err || header == MI_BATCH_BUFFER_END) {
            break;
        }
        desc = find_cmd(t, header, desc, &def);
        if (!desc) {
            ret = -EINVAL;
            break;
        }
        if (desc->flags & CMD_DESC_FIXED) {
            length = desc->length;
        } else {
            length = (header & desc->length) + LENGTH_BIAS;
        }
        if (end - at < length) {
            ret = -EINVAL;
            break;
        }
        if (!check_cmd(t, desc, &c, at, length)) {
            ret = -EACCES;
            break;
        }
        if (cmd_desc_is(desc, MI_BATCH_BUFFER_START)) {
            ret = -EACCES;
            break;
        }
        at += length;
        if (at >= end) {
            ret = -EINVAL;
            break;
        }
    }
    /* The shadow runs twice: privileged from its start, then unprivileged
     * from `end`, which ends at once unless the batch was refused, when its
     * privileged run ends at once and the second starts the original. */
    cursor_put(&c, end, MI_BATCH_BUFFER_END);
    if (ret != 0) {
        cursor_put(&c, 0, MI_BATCH_BUFFER_END);
        if (ret == -EACCES) {
            cursor_put(&c, end, MI_BATCH_BUFFER_START | MI_BATCH_NON_SECURE_HSW);
            cursor_put(&c, end + 1u, p->addr);
            ret = 0;
        }
    }
    cursor_end(&c);
    return c.err ? c.err : ret;
}
