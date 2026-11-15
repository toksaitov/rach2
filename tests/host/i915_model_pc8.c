/*
 * i915_model_pc8.c: the host model's LCPLL and the sideband to the PCH's
 * clock unit, the parts of the HD 4400 that the Intel driver's package C8
 * entry and exit drive. i915_model.c includes this file and lists its
 * blocks.
 */

#include "i915_model_pc8.h"
#include "drm/i915/i915_reg.h"

static const struct i915_model_block i915_model_pc8_lcpll;

/* The clock unit's registers that `iclk[]` holds, in its order. */
static const uint32_t pc8_iclk_regs[3] = {SBI_SSCCTL, SBI_GEN0, SBI_DBUFF0};

/* The status bits of LCPLL_CTL, which the chip sets. */
#define MODEL_LCPLL_STATUS (LCPLL_PLL_LOCK | LCPLL_CD_SOURCE_FCLK_DONE)

/* SBI_CTL_STAT's destination and operation, as the driver may write them. */
#define MODEL_SBI_DEST_MASK (0xfu << 16)
#define MODEL_SBI_OP_MASK   (0xfu << 8)

/* ---- LCPLL ---- */

/** The status bits that follow from LCPLL_CTL's control bits `v`. */
static uint32_t lcpll_status(uint32_t v)
{
    return (v & LCPLL_PLL_DISABLE ? 0u : LCPLL_PLL_LOCK) |
           (v & LCPLL_CD_SOURCE_FCLK ? LCPLL_CD_SOURCE_FCLK_DONE : 0u);
}

/** LCPLL locked on the CD clock's own source, and the clock unit's
 * registers, as the firmware leaves them. */
static void lcpll_reset(struct i915_model *m, void *priv)
{
    struct i915_model_pc8 *p = (struct i915_model_pc8 *)priv;

    i915_model_poke(m, LCPLL_CTL, LCPLL_PLL_LOCK);
    p->iclk[0] = 0;
    p->iclk[1] = SBI_GEN0_CFG_BUFFENABLE_DISABLE;
    p->iclk[2] = SBI_GEN0_CFG_BUFFENABLE_DISABLE;
}

/** A write keeps its control bits; the status bits follow at once, unless
 * the PLL is stuck. */
static void lcpll_write(struct i915_model *m, void *priv, uint32_t reg, uint32_t value)
{
    struct i915_model_pc8 *p = (struct i915_model_pc8 *)priv;
    uint32_t status = i915_model_peek(m, reg) & MODEL_LCPLL_STATUS;

    if (!p->lcpll_stuck) {
        status = lcpll_status(value);
    }
    i915_model_poke(m, reg, (value & ~MODEL_LCPLL_STATUS) | status);
}

static const struct i915_model_block i915_model_pc8_lcpll = {
    "lcpll", LCPLL_CTL, LCPLL_CTL, sizeof(struct i915_model_pc8), lcpll_reset, 0, lcpll_write, 0};

/* ---- the sideband ---- */

/** The sideband idle. */
static void sbi_reset(struct i915_model *m, void *priv)
{
    (void)priv;
    i915_model_poke(m, SBI_ADDR, 0);
    i915_model_poke(m, SBI_DATA, 0);
    i915_model_poke(m, SBI_CTL_STAT, 0);
}

/** The index in `iclk[]` of the clock unit's register `reg`, or -1. */
static int sbi_index(uint32_t reg)
{
    int i;

    for (i = 0; i < 3; i++) {
        if (pc8_iclk_regs[i] == reg) {
            return i;
        }
    }
    return -1;
}

/** Run the transfer that `cmd` starts: SBI_DATA written to the register
 * SBI_ADDR names, or read into SBI_DATA; 0, or SBI_RESPONSE_FAIL. */
static uint32_t sbi_run(struct i915_model *m, struct i915_model_pc8 *p, uint32_t cmd)
{
    uint32_t reg = i915_model_peek(m, SBI_ADDR) >> 16;
    int write = (cmd & SBI_CTL_OP_WRITE) != 0, at = sbi_index(reg);
    struct i915_model_sbi *e = p->nlog < I915_MODEL_SBI_LOG ? &p->log[p->nlog++] : 0;

    p->transfers++;
    if (e) {
        e->reg = reg;
        e->write = write;
        e->value = 0;
    }
    if (at < 0 || (cmd & MODEL_SBI_DEST_MASK) != SBI_CTL_DEST_ICLK ||
        (cmd & MODEL_SBI_OP_MASK & ~SBI_CTL_OP_WRITE) != SBI_CTL_OP_CRRD) {
        return SBI_RESPONSE_FAIL;
    }
    if (write) {
        p->iclk[at] = i915_model_peek(m, SBI_DATA);
    } else {
        i915_model_poke(m, SBI_DATA, p->iclk[at]);
    }
    if (e) {
        e->value = p->iclk[at];
    }
    return 0;
}

/** A write of SBI_CTL_STAT with SBI_BUSY starts a transfer, which ends at
 * once unless the sideband is stuck; other writes are stored. */
static void sbi_write(struct i915_model *m, void *priv, uint32_t reg, uint32_t value)
{
    struct i915_model_pc8 *p = i915_model_pc8(m);

    (void)priv;
    if (reg != SBI_CTL_STAT || !(value & SBI_BUSY)) {
        i915_model_poke(m, reg, value);
        return;
    }
    if (p->sbi_stuck) {
        i915_model_poke(m, reg, value);
        return;
    }
    i915_model_poke(m, reg, (value & ~(SBI_BUSY | SBI_RESPONSE_FAIL)) | sbi_run(m, p, value));
}

static const struct i915_model_block i915_model_pc8_sbi = {"sideband", SBI_ADDR, SBI_CTL_STAT, 1,
                                                           sbi_reset,  0,        sbi_write,    0};

/* ---- the test's way in ---- */

struct i915_model_pc8 *i915_model_pc8(struct i915_model *m)
{
    int i;

    for (i = 0; i < i915_model_nblocks(); i++) {
        if (i915_model_block_at(i) == &i915_model_pc8_lcpll) {
            return (struct i915_model_pc8 *)m->priv[i];
        }
    }
    return 0;
}
