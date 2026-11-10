/*
 * i915_model_base.c: the host model's forcewake, unclaimed access, GT
 * FIFO, display power well and global GTT, the parts of the HD 4400 the
 * Intel driver's base brings up. i915_model.c includes this file and
 * lists its blocks.
 */

#include "i915_model_base.h"
#include "drm/i915/i915_reg.h"

/* The well's request and state bits in each of its registers. */
#define MODEL_PW_REQ   HSW_PWR_WELL_CTL_REQ(HSW_PW_CTL_IDX_GLOBAL)
#define MODEL_PW_STATE HSW_PWR_WELL_CTL_STATE(HSW_PW_CTL_IDX_GLOBAL)

/* The GT FIFO's entries when it is empty. */
#define MODEL_FIFO_ENTRIES 64u

/* ---- forcewake ---- */

/** Forcewake at power-on: no request, no acknowledgement. */
static void fw_reset(struct i915_model *m, void *priv)
{
    struct i915_model_fw *fw = (struct i915_model_fw *)priv;

    fw->req = 0;
    i915_model_poke(m, FORCEWAKE_MT, 0);
    i915_model_poke(m, FORCEWAKE_ACK_HSW, 0);
    i915_model_poke(m, GEN6_GT_THREAD_STATUS_REG, 0);
}

/** FORCEWAKE_MT reads back its request bits. */
static uint32_t fw_read(struct i915_model *m, void *priv, uint32_t reg)
{
    (void)m;
    (void)reg;
    return ((struct i915_model_fw *)priv)->req;
}

/** A masked write of the request: the acknowledgement follows it, at once
 * or after the delay, unless it is stuck. */
static void fw_write(struct i915_model *m, void *priv, uint32_t reg, uint32_t value)
{
    struct i915_model_fw *fw = (struct i915_model_fw *)priv;
    uint32_t mask = value >> 16;

    (void)reg;
    fw->req = (fw->req & ~mask) | (value & mask & 0xffffu);
    i915_model_poke(m, FORCEWAKE_MT, fw->req);
    if (fw->ack_stuck) {
        return;
    }
    if (fw->ack_delay_us == 0) {
        i915_model_poke(m, FORCEWAKE_ACK_HSW, fw->req);
        fw->pending = 0;
        return;
    }
    fw->pending = 1;
    fw->pending_us = fw->ack_delay_us;
}

/** Time passes: a pending acknowledgement comes once its delay is up. */
static void fw_tick(struct i915_model *m, void *priv, uint32_t us)
{
    struct i915_model_fw *fw = (struct i915_model_fw *)priv;

    if (!fw->pending || fw->ack_stuck) {
        return;
    }
    if (us < fw->pending_us) {
        fw->pending_us -= us;
        return;
    }
    fw->pending = 0;
    i915_model_poke(m, FORCEWAKE_ACK_HSW, fw->req);
}

static const struct i915_model_block i915_model_base_fw = {
    "forcewake", FORCEWAKE_MT, FORCEWAKE_MT, sizeof(struct i915_model_fw),
    fw_reset,    fw_read,      fw_write,     fw_tick};

/* ---- the unclaimed access ---- */

/** FPGA_DBG as the Lenovo's firmware leaves it, an unclaimed access
 * latched. */
static void unclaimed_reset(struct i915_model *m, void *priv)
{
    (void)priv;
    i915_model_poke(m, FPGA_DBG, LENOVO_FW_FPGA_DBG);
}

/** A write of the unclaimed bit clears it; the other bits take nothing. */
static void unclaimed_write(struct i915_model *m, void *priv, uint32_t reg, uint32_t value)
{
    (void)priv;
    (void)reg;
    i915_model_poke(m, FPGA_DBG, i915_model_peek(m, FPGA_DBG) & ~(value & FPGA_DBG_RM_NOCLAIM));
}

static const struct i915_model_block i915_model_base_unclaimed = {
    "unclaimed", FPGA_DBG, FPGA_DBG, 1, unclaimed_reset, 0, unclaimed_write, 0};

/* ---- the display power well ---- */

/** The well at power-on, as the Lenovo's firmware leaves it: requested by
 * the BIOS and on. */
static void pw_reset(struct i915_model *m, void *priv)
{
    struct i915_model_pw *pw = (struct i915_model_pw *)priv;

    (void)m;
    pw->req[0] = MODEL_PW_REQ;
    pw->req[1] = pw->req[2] = pw->req[3] = 0;
    pw->on = 1;
    pw->pending = 0;
}

/** A power well register: its own request bit and the well's state. */
static uint32_t pw_read(struct i915_model *m, void *priv, uint32_t reg)
{
    struct i915_model_pw *pw = (struct i915_model_pw *)priv;

    (void)m;
    return pw->req[(reg - HSW_PWR_WELL_CTL1) / 4] | (pw->on ? MODEL_PW_STATE : 0);
}

/** Whether any of the four registers requests the well. */
static int pw_wanted(const struct i915_model_pw *pw)
{
    return (pw->req[0] | pw->req[1] | pw->req[2] | pw->req[3]) != 0;
}

/** A request written: the state follows the requests, at once or after
 * the delay, unless it is stuck. */
static void pw_write(struct i915_model *m, void *priv, uint32_t reg, uint32_t value)
{
    struct i915_model_pw *pw = (struct i915_model_pw *)priv;

    (void)m;
    pw->req[(reg - HSW_PWR_WELL_CTL1) / 4] = value & MODEL_PW_REQ;
    if (pw->stuck || pw_wanted(pw) == pw->on) {
        pw->pending = 0;
        return;
    }
    if (pw->delay_us == 0) {
        pw->on = pw_wanted(pw);
        return;
    }
    pw->pending = 1;
    pw->pending_us = pw->delay_us;
}

/** Time passes: a pending change of state comes once its delay is up. */
static void pw_tick(struct i915_model *m, void *priv, uint32_t us)
{
    struct i915_model_pw *pw = (struct i915_model_pw *)priv;

    (void)m;
    if (!pw->pending || pw->stuck) {
        return;
    }
    if (us < pw->pending_us) {
        pw->pending_us -= us;
        return;
    }
    pw->pending = 0;
    pw->on = pw_wanted(pw);
}

static const struct i915_model_block i915_model_base_pw = {
    "power well", HSW_PWR_WELL_CTL1, HSW_PWR_WELL_CTL4, sizeof(struct i915_model_pw),
    pw_reset,     pw_read,           pw_write,          pw_tick};

/* ---- the GT FIFO ---- */

/** The FIFO at power-on: empty, every entry free, draining one a
 * microsecond. */
static void fifo_reset(struct i915_model *m, void *priv)
{
    struct i915_model_fifo *f = (struct i915_model_fifo *)priv;

    (void)m;
    f->free = f->max = MODEL_FIFO_ENTRIES;
    f->refill_per_us = 1;
}

/** GTFIFOCTL: the register file's other bits and the free entries. */
static uint32_t fifo_read(struct i915_model *m, void *priv, uint32_t reg)
{
    (void)reg;
    return (i915_model_peek(m, GTFIFOCTL) & ~GT_FIFO_FREE_ENTRIES_MASK) |
           ((struct i915_model_fifo *)priv)->free;
}

/** Time passes: the FIFO drains, its free entries growing. */
static void fifo_tick(struct i915_model *m, void *priv, uint32_t us)
{
    struct i915_model_fifo *f = (struct i915_model_fifo *)priv;
    uint64_t room = (uint64_t)f->free + (uint64_t)f->refill_per_us * us;

    (void)m;
    f->free = room > f->max ? f->max : (uint32_t)room;
}

static const struct i915_model_block i915_model_base_fifo = {
    "gt fifo",  GTFIFOCTL, GTFIFOCTL, sizeof(struct i915_model_fifo),
    fifo_reset, fifo_read, 0,         fifo_tick};

/* ---- the global GTT and the firmware's display ---- */

/**
 * The GTT as the Lenovo's firmware leaves it, as gfxtool dumped it
 * (tests/fixtures/lenovo-gpu/firmware-dump.txt): the whole of stolen
 * memory mapped in order from GTT offset 0, uncached, its first 0x408000
 * bytes the framebuffer that pipe A's primary plane scans out, and every
 * other entry all ones, where the dump has a stray bit clear here and
 * there. The display that scans the framebuffer out is the pipe block's
 * (i915_model_pipe.c).
 */
static void gtt_reset(struct i915_model *m, void *priv)
{
    uint32_t i, stolen = (LENOVO_STOLEN_END - LENOVO_STOLEN_BASE + 1u) / 4096u;

    (void)priv;
    for (i = 0; i < (I915_MODEL_BAR0 - I915_GSM_OFFSET) / 4u; i++) {
        uint32_t addr = LENOVO_STOLEN_BASE + i * 4096u;
        i915_model_poke(m, I915_GSM_OFFSET + i * 4u,
                        i < stolen ? HSW_GTT_ADDR_ENCODE(addr) | GEN6_PTE_VALID : 0xffffffffu);
    }
}

static const struct i915_model_block i915_model_base_gtt = {
    "gtt", I915_GSM_OFFSET, I915_MODEL_BAR0 - 4u, 1, gtt_reset, 0, 0, 0};

/* ---- the test's way in ---- */

/** The state of `block` in `m`, or NULL when the table lacks it. */
static void *base_priv(struct i915_model *m, const struct i915_model_block *block)
{
    int i;

    for (i = 0; i < i915_model_nblocks(); i++) {
        if (i915_model_block_at(i) == block) {
            return m->priv[i];
        }
    }
    return 0;
}

struct i915_model_fw *i915_model_forcewake(struct i915_model *m)
{
    return (struct i915_model_fw *)base_priv(m, &i915_model_base_fw);
}

struct i915_model_fifo *i915_model_gt_fifo(struct i915_model *m)
{
    return (struct i915_model_fifo *)base_priv(m, &i915_model_base_fifo);
}

struct i915_model_pw *i915_model_power(struct i915_model *m)
{
    return (struct i915_model_pw *)base_priv(m, &i915_model_base_pw);
}
