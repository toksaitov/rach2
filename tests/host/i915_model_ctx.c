/*
 * i915_model_ctx.c: the host model's context switch and flushes, the
 * commands of requests that i915_model_ring.c hands here: MI_ARB_ON_OFF,
 * MI_SET_CONTEXT, MI_LOAD_REGISTER_IMM, MI_STORE_REGISTER_MEM,
 * MI_FLUSH_DW and PIPE_CONTROL. i915_model.c includes this file after
 * i915_model_ring.c, whose helpers it uses.
 */

#include "i915_model_ring.h"
#include "drm/i915/i915.h"
#include "drm/i915/i915_reg.h"

/** Whether a write of `reg` is masked: the high half says which of the
 * low half's bits it sets. */
static int ctx_masked(uint32_t reg)
{
    return reg == INSTPM || reg == CACHE_MODE_0_GEN7 || reg == CACHE_MODE_1 ||
           reg == RING_PSMI_CTL(RENDER_RING_BASE) || reg == RING_PSMI_CTL(BLT_RING_BASE);
}

/* The context registers an image saves, in the order it keeps them. */
static const uint32_t ctx_regs[3] = {INSTPM, CACHE_MODE_0_GEN7, CACHE_MODE_1};

/**
 * MI_SET_CONTEXT with the image and flags `dw` on the ring at `base`: the
 * old image saved and the new restored, as i915_model_ring.h says. The
 * ring faults when an image cannot be reached or holds nothing to restore,
 * when arbitration is on, or on a ring other than the render ring.
 */
static void ctx_switch(struct i915_model *m, struct i915_model_ring *r, uint32_t base, uint32_t dw)
{
    uint32_t cur = i915_model_peek(m, CCID(base));
    uint32_t *old, *img, i;

    if (base != RENDER_RING_BASE || !r->ctx.arb_off || !(dw & MI_MM_SPACE_GTT)) {
        ring_fault(r, MI_SET_CONTEXT);
        return;
    }
    if (cur && (cur & ~0xfffu) == (dw & ~0xfffu) && !(dw & MI_FORCE_RESTORE)) {
        /* The image loaded already: nothing moves. */
        i915_model_poke(m, CCID(base), dw);
        return;
    }
    r->ctx.switches++;
    if (cur) {
        for (i = 0; i < 4; i++) {
            old = ring_dword(m, (cur & ~0xfffu) + 4u * i);
            if (!old) {
                ring_fault(r, MI_SET_CONTEXT);
                return;
            }
            *old = i == 0 ? I915_MODEL_CTX_SAVED : i915_model_peek(m, ctx_regs[i - 1]);
        }
        r->ctx.saves++;
    }
    img = ring_dword(m, dw & ~0xfffu);
    if (!img) {
        ring_fault(r, MI_SET_CONTEXT);
        return;
    }
    if (dw & MI_RESTORE_INHIBIT) {
        r->ctx.inhibited++;
    } else if (*img != I915_MODEL_CTX_SAVED) {
        ring_fault(r, MI_SET_CONTEXT);
        return;
    } else {
        for (i = 0; i < 3; i++) {
            i915_model_poke(m, ctx_regs[i], *ring_dword(m, (dw & ~0xfffu) + 4u * (i + 1u)));
        }
        r->ctx.restores++;
    }
    i915_model_poke(m, CCID(base), dw);
}

/** A post-sync write of `value` to the GTT offset `addr`, or to the status
 * page's offset when `index` is set: 0, or -1 when it cannot be reached. */
static int ctx_store(struct i915_model *m, uint32_t base, int index, uint32_t addr, uint32_t value)
{
    uint32_t hws_pga, irq_bit, *out;

    ring_info(base, &hws_pga, &irq_bit);
    out = index ? ring_dword(m, (i915_model_peek(m, hws_pga) & ~0xfffu) + (addr & 0xff8u))
                : ring_dword(m, addr & ~7u);
    if (!out) {
        return -1;
    }
    *out = value;
    return 0;
}

/**
 * Run the command `d` of `len` dwords on the ring at `base`, when it is one
 * of the commands this file models: 1, or 0 for a command it does not know.
 * A command the model knows but the ring would refuse faults the ring.
 */
static int ring_ctx_command(struct i915_model *m, struct i915_model_ring *r, uint32_t base,
                            const uint32_t *d, uint32_t len)
{
    uint32_t i, *out;

    if ((d[0] >> 29) == 3u) {
        /* PIPE_CONTROL, the render ring's: a QW_WRITE post-sync write to
         * the global GTT. */
        if ((d[0] & 0xffff0000u) != (GFX_OP_PIPE_CONTROL(4) & 0xffff0000u) || len != 4u ||
            base != RENDER_RING_BASE ||
            ((d[1] & PIPE_CONTROL_QW_WRITE) &&
             (!(d[1] & PIPE_CONTROL_GLOBAL_GTT_IVB) || ctx_store(m, base, 0, d[2], d[3]) < 0))) {
            ring_fault(r, d[0]);
        }
        r->ctx.pipe_controls++;
        return 1;
    }
    switch (MI_OPCODE(d[0])) {
    case 0x08: /* MI_ARB_ON_OFF */
        r->ctx.arb_off = !(d[0] & MI_ARB_ENABLE);
        return 1;
    case 0x18: /* MI_SET_CONTEXT */
        if (len != 2u) {
            ring_fault(r, d[0]);
            return 1;
        }
        ctx_switch(m, r, base, d[1]);
        return 1;
    case 0x22: /* MI_LOAD_REGISTER_IMM: pairs of a register and a value */
        for (i = 1; i + 1u < len; i += 2u) {
            i915_model_poke(m, d[i],
                            ctx_masked(d[i]) ? ring_masked(i915_model_peek(m, d[i]), d[i + 1u])
                                             : d[i + 1u]);
        }
        return 1;
    case 0x24: /* MI_STORE_REGISTER_MEM through the global GTT */
        out = len == 3u && (d[0] & MI_SRM_LRM_GLOBAL_GTT) ? ring_dword(m, d[2]) : 0;
        if (!out) {
            ring_fault(r, d[0]);
            return 1;
        }
        *out = i915_model_peek(m, d[1]);
        return 1;
    case 0x26: /* MI_FLUSH_DW, the other rings', with its post-sync store */
        if (base == RENDER_RING_BASE || len != 3u ||
            ((d[0] & MI_FLUSH_DW_OP_STOREDW) &&
             (!(d[1] & MI_FLUSH_DW_USE_GTT) ||
              ctx_store(m, base, !!(d[0] & MI_FLUSH_DW_STORE_INDEX), d[1], d[2]) < 0))) {
            ring_fault(r, d[0]);
        }
        r->ctx.flush_dws++;
        return 1;
    default:
        return 0;
    }
}
