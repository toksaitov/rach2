/*
 * i915_model_reset.c: the host model's reset, GEN6_GDRST, which resets the
 * rings of the domains written and reads back clear unless the test holds
 * a domain stuck, and the render engine's masked state registers that the
 * driver's workarounds write and such a reset clears. i915_model.c
 * includes this file after i915_model_ring.c, whose rings it resets.
 */

#include "i915_model_reset.h"
#include "i915_model_ring.h"
#include "drm/i915/i915_reg.h"

static const struct i915_model_block i915_model_reset_block;

struct i915_model_reset *i915_model_reset_state(struct i915_model *m)
{
    return (struct i915_model_reset *)ring_priv(m, &i915_model_reset_block);
}

/** A write of CACHE_MODE_0_GEN7, CACHE_MODE_1 or GEN7_GT_MODE, each
 * masked. */
static void state_write(struct i915_model *m, void *priv, uint32_t reg, uint32_t value)
{
    (void)priv;
    i915_model_poke(m, reg, ring_masked(i915_model_peek(m, reg), value));
}

/** A write between HSW_HALF_SLICE_CHICKEN3 and HSW_ROW_CHICKEN3: those
 * two are masked, and the register file keeps any other. */
static void chicken_write(struct i915_model *m, void *priv, uint32_t reg, uint32_t value)
{
    (void)priv;
    if (reg == HSW_HALF_SLICE_CHICKEN3 || reg == HSW_ROW_CHICKEN3) {
        value = ring_masked(i915_model_peek(m, reg), value);
    }
    i915_model_poke(m, reg, value);
}

static const struct i915_model_block i915_model_reset_state_block = {
    "render state", CACHE_MODE_0_GEN7, GEN7_GT_MODE, 0, 0, 0, state_write, 0};

static const struct i915_model_block i915_model_reset_chicken_block = {
    "render chicken", HSW_HALF_SLICE_CHICKEN3, HSW_ROW_CHICKEN3, 0, 0, 0, chicken_write, 0};

/** The render engine's state after a reset of its domain, with every bit
 * a workaround of the driver's sets cleared and each it clears set, so
 * that a row sees whether they were written again. */
static void state_lost(struct i915_model *m)
{
    i915_model_poke(m, CACHE_MODE_0_GEN7, HIZ_RAW_STALL_OPT_DISABLE | RC_OP_FLUSH_ENABLE);
    i915_model_poke(m, CACHE_MODE_1, 0);
    i915_model_poke(m, GEN7_GT_MODE, 0);
    i915_model_poke(m, HSW_HALF_SLICE_CHICKEN3, 0);
}

/** The GT's state after a reset of the whole GPU, the same way. */
static void gt_state_lost(struct i915_model *m)
{
    i915_model_poke(m, GEN7_FF_THREAD_MODE, GEN7_FF_VS_REF_CNT_FFME);
    i915_model_poke(m, HSW_MI_PREDICATE_RESULT_2, LOWER_SLICE_ENABLED);
    i915_model_poke(m, HSW_SCRATCH1, 0);
    i915_model_poke(m, HSW_ROW_CHICKEN3, 0);
}

/** The reset at power-on: nothing pending, nothing stuck, nothing done. */
static void reset_reset(struct i915_model *m, void *priv)
{
    struct i915_model_reset none = {0, 0, {0, 0, 0, 0, 0}};

    *(struct i915_model_reset *)priv = none;
    i915_model_poke(m, GEN6_GDRST, 0);
}

/** A write of GEN6_GDRST: each domain written and not stuck reset, as
 * i915_model_reset.h says; the stuck bits stay set. */
static void reset_write(struct i915_model *m, void *priv, uint32_t reg, uint32_t value)
{
    struct i915_model_reset *r = (struct i915_model_reset *)priv;
    uint32_t all = (1u << I915_MODEL_RESET_DOMAINS) - 1u, done = value & all & ~r->stuck;
    int i;

    (void)reg;
    r->writes++;
    for (i = 0; i < I915_MODEL_RESET_DOMAINS; i++) {
        if (done & (1u << i)) {
            r->resets[i]++;
        }
    }
    if (done & (GEN6_GRDOM_FULL | GEN6_GRDOM_RENDER)) {
        rcs_reset(m, i915_model_ring_state(m, 0));
        state_lost(m);
    }
    if (done & (GEN6_GRDOM_FULL | GEN6_GRDOM_BLT)) {
        bcs_reset(m, i915_model_ring_state(m, 1));
    }
    if (done & (GEN6_GRDOM_FULL | GEN6_GRDOM_MEDIA)) {
        vcs_reset(m, i915_model_ring_state(m, 2));
    }
    if (done & (GEN6_GRDOM_FULL | GEN6_GRDOM_VECS)) {
        vecs_reset(m, i915_model_ring_state(m, 3));
    }
    if (done & GEN6_GRDOM_FULL) {
        gt_state_lost(m);
        for (i = 0; i < I915_FENCE_REGS_HSW; i++) {
            i915_model_poke(m, FENCE_REG_GEN6_LO(i), 0);
            i915_model_poke(m, FENCE_REG_GEN6_HI(i), 0);
        }
    }
    i915_model_poke(m, GEN6_GDRST, value & all & r->stuck);
}

static const struct i915_model_block i915_model_reset_block = {
    "reset",     GEN6_GDRST, GEN6_GDRST,  sizeof(struct i915_model_reset),
    reset_reset, 0,          reset_write, 0};
