/*
 * i915_model_rc6.c: the host model's RC6 and its residency counter, the
 * pcode mailbox, and the frequency the GT runs at, the parts of the HD 4400
 * the Intel driver's RC6 and RPS drive. i915_model.c includes this file and
 * lists its blocks.
 */

#include "i915_model_rc6.h"
#include "i915_model_base.h"
#include "drm/i915/i915_reg.h"

/* GEN6_RC_STATE's software target field and its RC6. */
#define MODEL_RC_TARGET_MASK (7u << RC_SW_TARGET_STATE_SHIFT)
#define MODEL_RC_TARGET_RC6  (4u << RC_SW_TARGET_STATE_SHIFT)

/* ---- the frequency ---- */

/** The chip's limits RPn and RP0 from GEN6_RP_STATE_CAP. */
static void rps_caps(struct i915_model *m, uint32_t *rpn, uint32_t *rp0)
{
    uint32_t cap = i915_model_peek(m, GEN6_RP_STATE_CAP);

    *rp0 = cap & 0xffu;
    *rpn = (cap >> 16) & 0xffu;
}

/** A request in GEN6_RPNSWREQ: the GT runs at it, within RPn and RP0, which
 * GEN6_RPSTAT1's CAGF shows. */
static void rps_write(struct i915_model *m, void *priv, uint32_t reg, uint32_t value)
{
    uint32_t want = value >> HSW_FREQUENCY_SHIFT, rpn, rp0;

    (void)priv;
    i915_model_poke(m, reg, value);
    rps_caps(m, &rpn, &rp0);
    want = want < rpn ? rpn : want > rp0 ? rp0 : want;
    i915_model_poke(m, GEN6_RPSTAT1, want << HSW_CAGF_SHIFT);
}

/** The Lenovo's limits, and the GT at RPn as the firmware leaves it. */
static void rps_reset(struct i915_model *m, void *priv)
{
    (void)priv;
    i915_model_poke(m, GEN6_RP_STATE_CAP, LENOVO_RP_STATE_CAP);
    i915_model_poke(m, GEN6_RPNSWREQ, 0);
    i915_model_poke(m, GEN6_RPSTAT1, ((LENOVO_RP_STATE_CAP >> 16) & 0xffu) << HSW_CAGF_SHIFT);
}

static const struct i915_model_block i915_model_rc6_rps = {
    "frequency", GEN6_RPNSWREQ, GEN6_RPNSWREQ, 1, rps_reset, 0, rps_write, 0};

/* ---- RC6 and the pcode ---- */

/** RC6 off, the counter at 0, and the pcode idle with the Lenovo's
 * answers. */
static void pcu_reset(struct i915_model *m, void *priv)
{
    struct i915_model_rc6 *r = (struct i915_model_rc6 *)priv;
    char *p = (char *)r;
    unsigned long k;

    for (k = 0; k < sizeof(*r); k++) {
        p[k] = 0;
    }
    r->ddcc = LENOVO_PCODE_DDCC;
    i915_model_poke(m, GEN6_RC_CONTROL, 0);
    i915_model_poke(m, GEN6_RC_STATE, 0);
    i915_model_poke(m, GEN6_PCODE_MAILBOX, 0);
}

/** The counter reads its units; the rest of the range is the register
 * file's. */
static uint32_t pcu_read(struct i915_model *m, void *priv, uint32_t reg)
{
    struct i915_model_rc6 *r = (struct i915_model_rc6 *)priv;

    return reg == GEN6_GT_GFX_RC6 ? r->residency : i915_model_peek(m, reg);
}

/** Finish the command `cmd`: its answer in GEN6_PCODE_DATA, then the
 * ready bit cleared and the status in the low byte. */
static void pcu_finish(struct i915_model *m, struct i915_model_rc6 *r, uint32_t cmd)
{
    uint32_t status = r->error;

    r->commands++;
    r->last_cmd = cmd & 0xffu;
    switch (cmd & 0xffu) {
    case HSW_PCODE_DYNAMIC_DUTY_CYCLE_CONTROL:
        i915_model_poke(m, GEN6_PCODE_DATA, r->ddcc);
        break;
    case GEN6_READ_OC_PARAMS:
        i915_model_poke(m, GEN6_PCODE_DATA, r->oc_params);
        break;
    case GEN6_PCODE_READ_RC6VIDS:
        i915_model_poke(m, GEN6_PCODE_DATA, r->rc6vids);
        break;
    case GEN6_PCODE_WRITE_MIN_FREQ_TABLE:
        if (r->ntable < I915_MODEL_FREQ_TABLE) {
            r->table[r->ntable++] = i915_model_peek(m, GEN6_PCODE_DATA);
        }
        break;
    default:
        status = GEN6_PCODE_ILLEGAL_CMD;
        break;
    }
    i915_model_poke(m, GEN6_PCODE_MAILBOX, status & GEN6_PCODE_ERROR_MASK);
}

/** A command posted with the ready bit is taken at once or after the
 * delay, unless the pcode is stuck; other writes are stored. */
static void pcu_write(struct i915_model *m, void *priv, uint32_t reg, uint32_t value)
{
    struct i915_model_rc6 *r = (struct i915_model_rc6 *)priv;

    i915_model_poke(m, reg, value);
    if (reg != GEN6_PCODE_MAILBOX || !(value & GEN6_PCODE_READY) || r->stuck) {
        return;
    }
    if (r->delay_us == 0) {
        pcu_finish(m, r, value);
        return;
    }
    r->pending = 1;
    r->pending_us = r->delay_us;
    r->pending_cmd = value;
}

/** How many of the last `us` microseconds an idle GT spent in RC6, now
 * that it has been idle `idle_us`, by GEN6_RC_CONTROL and GEN6_RC_STATE. */
static uint32_t pcu_rc6_us(struct i915_model *m, uint32_t idle_us, uint32_t us)
{
    uint32_t ctl = i915_model_peek(m, GEN6_RC_CONTROL), threshold_us;

    if (!(ctl & GEN6_RC_CTL_RC6_ENABLE)) {
        return 0;
    }
    if (!(ctl & GEN6_RC_CTL_HW_ENABLE)) {
        return (i915_model_peek(m, GEN6_RC_STATE) & MODEL_RC_TARGET_MASK) == MODEL_RC_TARGET_RC6
                   ? us
                   : 0;
    }
    threshold_us = (uint32_t)((uint64_t)i915_model_peek(m, GEN6_RC6_THRESHOLD) * 128u / 100u);
    if (idle_us <= threshold_us) {
        return 0;
    }
    return idle_us - threshold_us < us ? idle_us - threshold_us : us;
}

/** Time passes: a pending command is taken once its delay is up, and an
 * idle GT's RC6 residency counts. */
static void pcu_tick(struct i915_model *m, void *priv, uint32_t us)
{
    struct i915_model_rc6 *r = (struct i915_model_rc6 *)priv;
    uint32_t in;

    if (r->pending && !r->stuck) {
        if (us < r->pending_us) {
            r->pending_us -= us;
        } else {
            r->pending = 0;
            pcu_finish(m, r, r->pending_cmd);
        }
    }
    if (r->busy || (i915_model_peek(m, FORCEWAKE_ACK_HSW) & 1u)) {
        r->idle_us = 0;
        r->in_rc6 = 0;
        return;
    }
    r->idle_us += us;
    in = pcu_rc6_us(m, r->idle_us, us);
    r->in_rc6 = in > 0;
    r->rc6_us += in;
    r->frac += in * 100u;
    r->residency += r->frac / 128u;
    r->frac %= 128u;
}

static const struct i915_model_block i915_model_rc6_pcu = {
    "rc6 and pcode",  GEN6_GT_GFX_RC6_LOCKED,
    GEN6_PCODE_DATA1, sizeof(struct i915_model_rc6),
    pcu_reset,        pcu_read,
    pcu_write,        pcu_tick};

/* ---- the test's way in ---- */

struct i915_model_rc6 *i915_model_rc6(struct i915_model *m)
{
    int i;

    for (i = 0; i < i915_model_nblocks(); i++) {
        if (i915_model_block_at(i) == &i915_model_rc6_pcu) {
            return (struct i915_model_rc6 *)m->priv[i];
        }
    }
    return 0;
}
