/*
 * i915_model_ring.c: the host model's render and blitter rings and its
 * interrupts: the rings' registers, the commands the driver gives them,
 * and the identity, mask and enable registers that raise the device's
 * message. i915_model.c includes this file and lists its blocks.
 */

#include "i915_model_ring.h"
#include "drm/i915/i915.h"
#include "drm/i915/i915_reg.h"

/* The bytes of registers each ring's block answers for, from its base. */
#define MODEL_RING_SPAN 0x1000u

/* ---- the interrupts ---- */

static const struct i915_model_block i915_model_ring_irq;

/** The state of `block` in `m`, or NULL when the table lacks it. */
static void *ring_priv(struct i915_model *m, const struct i915_model_block *block)
{
    int i;

    for (i = 0; i < i915_model_nblocks(); i++) {
        if (i915_model_block_at(i) == block) {
            return m->priv[i];
        }
    }
    return 0;
}

struct i915_model_irq *i915_model_irq_state(struct i915_model *m)
{
    return (struct i915_model_irq *)ring_priv(m, &i915_model_ring_irq);
}

/* Each bank's status, mask, identity and enable, by I915_IRQ_ index. An
 * event of the rings is a pulse, so the status registers stay 0 but for
 * the display engine's PCH event, which follows the south display. */
static const uint32_t model_irq_regs[I915_IRQ_BANKS][4] = {
    {GTISR, GTIMR, GTIIR, GTIER},
    {GEN6_PMISR, GEN6_PMIMR, GEN6_PMIIR, GEN6_PMIER},
    {DEISR, DEIMR, DEIIR, DEIER},
    {SDEISR, SDEIMR, SDEIIR, SDEIER}};

/** Whether the south display asks for the display engine's PCH event. */
static int model_south_level(struct i915_model *m)
{
    return (i915_model_peek(m, SDEIIR) & i915_model_peek(m, SDEIER)) != 0;
}

/**
 * Follow a change of the identity and enable registers: a rise of the
 * south display's level raises the PCH event, and a rise of the device's
 * line raises a message.
 */
static void model_irq_eval(struct i915_model *m)
{
    struct i915_model_irq *irq = i915_model_irq_state(m);
    uint32_t de_ier = i915_model_peek(m, DEIER);
    int south = model_south_level(m), line;

    if (!irq) {
        return;
    }
    if (south && !(i915_model_peek(m, DEISR) & DE_PCH_EVENT_IVB)) {
        i915_model_poke(m, DEISR, i915_model_peek(m, DEISR) | DE_PCH_EVENT_IVB);
        if (!(i915_model_peek(m, DEIMR) & DE_PCH_EVENT_IVB)) {
            i915_model_poke(m, DEIIR, i915_model_peek(m, DEIIR) | DE_PCH_EVENT_IVB);
        }
    } else if (!south) {
        i915_model_poke(m, DEISR, i915_model_peek(m, DEISR) & ~DE_PCH_EVENT_IVB);
    }
    line = (de_ier & DE_MASTER_IRQ_CONTROL) &&
           ((i915_model_peek(m, DEIIR) & de_ier & ~DE_MASTER_IRQ_CONTROL) ||
            (i915_model_peek(m, GTIIR) & i915_model_peek(m, GTIER)) ||
            (i915_model_peek(m, GEN6_PMIIR) & i915_model_peek(m, GEN6_PMIER)));
    if (line && !irq->line) {
        irq->messages++;
        irq->pending++;
    }
    irq->line = line;
}

void i915_model_irq_event(struct i915_model *m, int bank, uint32_t bits)
{
    if (bank < 0 || bank >= I915_IRQ_BANKS) {
        return;
    }
    i915_model_poke(m, model_irq_regs[bank][2],
                    i915_model_peek(m, model_irq_regs[bank][2]) |
                        (bits & ~i915_model_peek(m, model_irq_regs[bank][1])));
    model_irq_eval(m);
}

int i915_model_take_message(struct i915_model *m)
{
    struct i915_model_irq *irq = i915_model_irq_state(m);

    if (!irq || irq->pending == 0) {
        return 0;
    }
    irq->pending--;
    return 1;
}

/** The interrupts as the Lenovo's firmware leaves them: every source
 * masked and nothing pending, but PM's sources enabled and an unclaimed
 * access latched in GEN7_ERR_INT. */
static void irq_reset(struct i915_model *m, void *priv)
{
    int bank;

    (void)priv;
    for (bank = 0; bank < I915_IRQ_BANKS; bank++) {
        i915_model_poke(m, model_irq_regs[bank][0], 0);
        i915_model_poke(m, model_irq_regs[bank][1], 0xffffffffu);
        i915_model_poke(m, model_irq_regs[bank][2], 0);
        i915_model_poke(m, model_irq_regs[bank][3], 0);
    }
    i915_model_poke(m, GEN6_PMIER, LENOVO_FW_PMIER);
    i915_model_poke(m, GEN7_ERR_INT, LENOVO_FW_ERR_INT);
}

/** A write to an interrupt register: an identity register, or the error
 * registers beside them, clears the bits written; a status register takes
 * nothing; the rest are stored. Then the line follows. */
static void irq_write(struct i915_model *m, void *priv, uint32_t reg, uint32_t value)
{
    (void)priv;
    if (reg == DEIIR || reg == GTIIR || reg == GEN6_PMIIR || reg == SDEIIR || reg == GEN7_ERR_INT ||
        reg == SERR_INT) {
        i915_model_poke(m, reg, i915_model_peek(m, reg) & ~value);
    } else if (reg != DEISR && reg != GTISR && reg != GEN6_PMISR && reg != SDEISR) {
        i915_model_poke(m, reg, value);
    }
    model_irq_eval(m);
}

static const struct i915_model_block i915_model_ring_irq = {
    "interrupts", DEISR, GEN7_ERR_INT, sizeof(struct i915_model_irq), irq_reset, 0, irq_write, 0};

static const struct i915_model_block i915_model_ring_south = {
    "south interrupts", SDEISR, SERR_INT, 1, 0, 0, irq_write, 0};

/* ---- the rings ---- */

static const struct i915_model_block i915_model_ring_rcs, i915_model_ring_bcs;

struct i915_model_ring *i915_model_ring_state(struct i915_model *m, int ring)
{
    return (struct i915_model_ring *)ring_priv(m,
                                               ring ? &i915_model_ring_bcs : &i915_model_ring_rcs);
}

/** The status page address register and the user interrupt of the ring
 * whose registers start at `base`. */
static void ring_info(uint32_t base, uint32_t *hws_pga, uint32_t *irq_bit)
{
    if (base == BLT_RING_BASE) {
        *hws_pga = BLT_HWS_PGA_GEN7;
        *irq_bit = GT_BLT_USER_INTERRUPT;
    } else {
        *hws_pga = RENDER_HWS_PGA_GEN7;
        *irq_bit = GT_RENDER_USER_INTERRUPT;
    }
}

/** MI_MODE's idle bit: set while the ring is told to stop and has nothing
 * left to run, or is disabled, unless the test holds it busy. */
static void ring_update_idle(struct i915_model *m, struct i915_model_ring *r, uint32_t base)
{
    uint32_t mode = i915_model_peek(m, RING_MI_MODE(base)) & ~MODE_IDLE;
    uint32_t head = i915_model_peek(m, RING_HEAD(base)) & HEAD_ADDR;
    uint32_t tail = i915_model_peek(m, RING_TAIL(base)) & TAIL_ADDR;
    int empty = head == tail || !(i915_model_peek(m, RING_CTL(base)) & RING_VALID);

    if ((mode & STOP_RING) && empty && !r->idle_stuck) {
        mode |= MODE_IDLE;
    }
    i915_model_poke(m, RING_MI_MODE(base), mode);
}

/** A masked register's value after `value` is written to it. */
static uint32_t ring_masked(uint32_t old, uint32_t value)
{
    uint32_t mask = value >> 16;

    return (old & ~mask) | (value & mask & 0xffffu);
}

/** The ring at power-on, as a reset leaves it: disabled, empty, not
 * stopped, its interrupt masked, and no PPGTT. */
static void ring_reset_at(struct i915_model *m, void *priv, uint32_t base)
{
    struct i915_model_ring *r = (struct i915_model_ring *)priv;

    struct i915_model_ctx none = {0, 0, 0, 0, 0, 0, 0};

    r->executed = r->fault = r->batches = 0;
    r->faulted = r->stuck = r->idle_stuck = r->start_fails = r->flush_stuck = 0;
    r->ctx = none;
    i915_model_poke(m, CCID(base), 0);
    i915_model_poke(m, RING_PP_DIR_DCLV(base), 0);
    i915_model_poke(m, RING_PP_DIR_BASE(base), 0);
    i915_model_poke(m, RING_MODE_GEN7(base), 0);
    i915_model_poke(m, RING_CTL(base), 0);
    i915_model_poke(m, RING_HEAD(base), 0);
    i915_model_poke(m, RING_TAIL(base), 0);
    i915_model_poke(m, RING_START(base), 0);
    i915_model_poke(m, RING_MI_MODE(base), 0);
    i915_model_poke(m, RING_IMR(base), 0xffffffffu);
    ring_update_idle(m, r, base);
}

/** The render ring at power-on. */
static void rcs_reset(struct i915_model *m, void *priv)
{
    ring_reset_at(m, priv, RENDER_RING_BASE);
}

/** The blitter ring at power-on. */
static void bcs_reset(struct i915_model *m, void *priv)
{
    ring_reset_at(m, priv, BLT_RING_BASE);
}

/** A write to a ring's register: MI_MODE, INSTPM and RING_MODE_GEN7 are
 * masked, INSTPM's flush is done at once unless the test holds it, and a
 * write of RING_CTL loses RING_VALID while the ring is dead. */
static void ring_write(struct i915_model *m, void *priv, uint32_t reg, uint32_t value)
{
    struct i915_model_ring *r = (struct i915_model_ring *)priv;
    uint32_t base = reg & ~(MODEL_RING_SPAN - 1u);

    if (reg == RING_MI_MODE(base) || reg == RING_MODE_GEN7(base)) {
        value = ring_masked(i915_model_peek(m, reg), value);
    } else if (reg == RING_INSTPM(base)) {
        value = ring_masked(i915_model_peek(m, reg), value);
        if (!r->flush_stuck) {
            value &= ~(INSTPM_SYNC_FLUSH | INSTPM_TLB_INVALIDATE);
        }
    } else if (reg == RING_CTL(base) && r->start_fails) {
        value &= ~RING_VALID;
    }
    i915_model_poke(m, reg, value);
    ring_update_idle(m, r, base);
}

/** The physical address the global GTT maps the GTT offset `addr` to, or
 * 0 for an entry that is not valid or an offset past the GTT. */
static uint64_t ring_ggtt_phys(struct i915_model *m, uint32_t addr)
{
    uint32_t index = addr >> 12, pte;

    if (index >= (I915_MODEL_BAR0 - I915_GSM_OFFSET) / 4u) {
        return 0;
    }
    pte = i915_model_peek(m, I915_GSM_OFFSET + index * 4u);
    if (!(pte & GEN6_PTE_VALID)) {
        return 0;
    }
    return (uint64_t)(pte & 0xfffff000u) | ((uint64_t)(pte & 0x7f0u) << 28) | (addr & 0xfffu);
}

/** The host address of the dword at the GTT offset `addr`, or NULL. */
static uint32_t *ring_dword(struct i915_model *m, uint32_t addr)
{
    struct i915_model_irq *irq = i915_model_irq_state(m);
    uint64_t phys = ring_ggtt_phys(m, addr & ~3u);
    uint8_t *page;

    if (!irq || !irq->dma || phys == 0) {
        return 0;
    }
    page = (uint8_t *)irq->dma(m, phys & ~(uint64_t)0xfffu);
    return page ? (uint32_t *)(void *)(page + (phys & 0xffcu)) : 0;
}

/**
 * The physical address the aliasing PPGTT of the ring at `base` maps the
 * address `addr` to, as the chip walks it: the directory entry among the
 * global GTT's entries at PP_DIR_BASE, whose bits 31:16 count 64 bytes,
 * then the entry of the table it names. 0 while RING_MODE_GEN7 has no
 * PPGTT bit, for an address past 2 GiB or whose 64 MiB PP_DIR_DCLV does
 * not mark valid, or where an entry is not valid or a table not reached.
 */
static uint64_t ring_ppgtt_phys(struct i915_model *m, uint32_t base, uint32_t addr)
{
    struct i915_model_irq *irq = i915_model_irq_state(m);
    uint32_t pdi = addr >> 22, pd, pde, pte;
    uint64_t pt_phys;
    uint32_t *pt;

    if (!(i915_model_peek(m, RING_MODE_GEN7(base)) & GFX_PPGTT_ENABLE) || pdi >= I915_PPGTT_PDES ||
        !(i915_model_peek(m, RING_PP_DIR_DCLV(base)) & (1u << (pdi / 16u))) || !irq || !irq->dma) {
        return 0;
    }
    pd = (i915_model_peek(m, RING_PP_DIR_BASE(base)) >> 16) * 64u;
    if (pd / 4u + pdi >= (I915_MODEL_BAR0 - I915_GSM_OFFSET) / 4u) {
        return 0;
    }
    pde = i915_model_peek(m, I915_GSM_OFFSET + pd + pdi * 4u);
    if (!(pde & GEN6_PDE_VALID)) {
        return 0;
    }
    pt_phys = (uint64_t)(pde & 0xfffff000u) | ((uint64_t)(pde & 0xff0u) << 28);
    pt = (uint32_t *)irq->dma(m, pt_phys);
    if (!pt) {
        return 0;
    }
    pte = pt[(addr >> 12) & (I915_PPGTT_PTES - 1u)];
    if (!(pte & GEN6_PTE_VALID)) {
        return 0;
    }
    return (uint64_t)(pte & 0xfffff000u) | ((uint64_t)(pte & 0x7f0u) << 28) | (addr & 0xfffu);
}

/** The host address of the dword at `addr` in the PPGTT of the ring at
 * `base` when `ppgtt` is set, else in the global GTT, or NULL. */
static uint32_t *ring_space_dword(struct i915_model *m, uint32_t base, int ppgtt, uint32_t addr)
{
    struct i915_model_irq *irq = i915_model_irq_state(m);
    uint64_t phys;
    uint8_t *page;

    if (!ppgtt) {
        return ring_dword(m, addr);
    }
    phys = ring_ppgtt_phys(m, base, addr & ~3u);
    if (!irq || !irq->dma || phys == 0) {
        return 0;
    }
    page = (uint8_t *)irq->dma(m, phys & ~(uint64_t)0xfffu);
    return page ? (uint32_t *)(void *)(page + (phys & 0xffcu)) : 0;
}

/** Stop the ring at the command `dw`, which it cannot run. */
static void ring_fault(struct i915_model_ring *r, uint32_t dw)
{
    r->faulted = 1;
    r->fault = dw;
}

/**
 * Run the batch that the MI_BATCH_BUFFER_START `d`, of `len` dwords, starts
 * on the ring at `base`, up to its MI_BATCH_BUFFER_END: through the ring's
 * PPGTT when the command's bit 8 asks for it, as the driver's unprivileged
 * batches do, else through the global GTT. Of a batch's commands the model
 * runs MI_NOOP and MI_STORE_DWORD_IMM, whose address lies in the batch's
 * own space. A store that asks for the global GTT from a PPGTT batch, a
 * command the model does not know, an address it cannot reach, a batch of
 * more than 1024 dwords or a start of the wrong length faults the ring at
 * the batch's start.
 */
static void ring_batch(struct i915_model *m, struct i915_model_ring *r, uint32_t base,
                       const uint32_t *d, uint32_t len)
{
    int ppgtt = (d[0] & MI_BATCH_PPGTT_HSW) != 0;
    uint32_t addr = d[1] & ~3u, n = 0, dw, *p, *a, *v, *out;

    r->batches++;
    while (len == 2u && n < 1024u) {
        p = ring_space_dword(m, base, ppgtt, addr + 4u * n);
        if (!p) {
            break;
        }
        dw = *p;
        if (dw == MI_BATCH_BUFFER_END) {
            return;
        }
        if (dw == MI_NOOP) {
            n++;
            continue;
        }
        if ((dw >> 29) != 0 || MI_OPCODE(dw) != 0x20u || MI_LENGTH(dw) != 4u ||
            (ppgtt && (dw & MI_USE_GGTT))) {
            break;
        }
        a = ring_space_dword(m, base, ppgtt, addr + 4u * (n + 2u));
        v = ring_space_dword(m, base, ppgtt, addr + 4u * (n + 3u));
        out = a && v ? ring_space_dword(m, base, ppgtt, *a) : 0;
        if (!out) {
            break;
        }
        *out = *v;
        n += 4u;
    }
    ring_fault(r, d[0]);
}

/* The commands of contexts and flushes, i915_model_ctx.c, which
 * i915_model.c includes after this file: 1 when the command of `len`
 * dwords `d` was one of them, run or faulted, 0 when it is not. */
static int ring_ctx_command(struct i915_model *m, struct i915_model_ring *r, uint32_t base,
                            const uint32_t *d, uint32_t len);

/**
 * Run the ring whose registers start at `base` from its head to its tail,
 * while it is enabled, not stopped, not stuck and has not faulted: each
 * command read through the GTT, run, and the head moved past it.
 */
static void ring_run(struct i915_model *m, struct i915_model_ring *r, uint32_t base)
{
    uint32_t ctl = i915_model_peek(m, RING_CTL(base));
    uint32_t size = (ctl & RING_NR_PAGES) + 4096u;
    uint32_t start = i915_model_peek(m, RING_START(base)) & ~0xfffu;
    uint32_t head = i915_model_peek(m, RING_HEAD(base)) & HEAD_ADDR;
    uint32_t tail = i915_model_peek(m, RING_TAIL(base)) & TAIL_ADDR;
    uint32_t hws_pga, irq_bit, d[8], len, i, steps = size / 4u;

    if (!(ctl & RING_VALID) || (i915_model_peek(m, RING_MI_MODE(base)) & STOP_RING) || r->stuck ||
        r->faulted) {
        return;
    }
    ring_info(base, &hws_pga, &irq_bit);
    while (head != tail && steps-- > 0) {
        uint32_t *p = ring_dword(m, start + head), *out;

        if (!p) {
            ring_fault(r, 0xffffffffu);
            break;
        }
        d[0] = *p;
        /* MI commands, and the render pipe's (type 3), whose length is
         * in their low byte. */
        if ((d[0] >> 29) != 0 && (d[0] >> 29) != 3u) {
            ring_fault(r, d[0]);
            break;
        }
        if ((d[0] >> 29) == 3u) {
            len = (d[0] & 0xffu) + 2u;
        } else {
            len = MI_OPCODE(d[0]) < 0x10u ? 1u : MI_LENGTH(d[0]);
        }
        if (len > 8u) {
            ring_fault(r, d[0]);
            break;
        }
        for (i = 1; i < len; i++) {
            p = ring_dword(m, start + ((head + 4u * i) & (size - 1u)));
            d[i] = p ? *p : 0;
        }
        switch ((d[0] >> 29) == 3u ? 0xffu : MI_OPCODE(d[0])) {
        case 0x00: /* MI_NOOP */
            break;
        case 0x02: /* MI_USER_INTERRUPT */
            if (!(i915_model_peek(m, RING_IMR(base)) & irq_bit)) {
                i915_model_irq_event(m, I915_IRQ_GT, irq_bit);
            }
            break;
        case 0x20: /* MI_STORE_DWORD_IMM: the GTT's address in d[2] */
            out = len == 4u && (d[0] & MI_USE_GGTT) ? ring_dword(m, d[2]) : 0;
            if (!out) {
                ring_fault(r, d[0]);
                break;
            }
            *out = d[3];
            break;
        case 0x21: /* MI_STORE_DWORD_INDEX: an offset into the status page */
            out = len == 3u
                      ? ring_dword(m, (i915_model_peek(m, hws_pga) & ~0xfffu) + (d[1] & 0xffcu))
                      : 0;
            if (!out) {
                ring_fault(r, d[0]);
                break;
            }
            *out = d[2];
            break;
        case 0x31: /* MI_BATCH_BUFFER_START: the batch's address in d[1] */
            ring_batch(m, r, base, d, len);
            break;
        default:
            if (!ring_ctx_command(m, r, base, d, len)) {
                ring_fault(r, d[0]);
            }
            break;
        }
        if (r->faulted) {
            break;
        }
        head = (head + 4u * len) & (size - 1u);
        r->executed++;
    }
    i915_model_poke(m, RING_HEAD(base), head);
    i915_model_poke(m, RING_ACTHD(base), start + head);
    ring_update_idle(m, r, base);
}

/** Time passes: the render ring runs. */
static void rcs_tick(struct i915_model *m, void *priv, uint32_t us)
{
    (void)us;
    ring_run(m, (struct i915_model_ring *)priv, RENDER_RING_BASE);
}

/** Time passes: the blitter ring runs. */
static void bcs_tick(struct i915_model *m, void *priv, uint32_t us)
{
    (void)us;
    ring_run(m, (struct i915_model_ring *)priv, BLT_RING_BASE);
}

static const struct i915_model_block i915_model_ring_rcs = {"render ring",
                                                            RENDER_RING_BASE,
                                                            RENDER_RING_BASE + MODEL_RING_SPAN - 4u,
                                                            sizeof(struct i915_model_ring),
                                                            rcs_reset,
                                                            0,
                                                            ring_write,
                                                            rcs_tick};

static const struct i915_model_block i915_model_ring_bcs = {"blitter ring",
                                                            BLT_RING_BASE,
                                                            BLT_RING_BASE + MODEL_RING_SPAN - 4u,
                                                            sizeof(struct i915_model_ring),
                                                            bcs_reset,
                                                            0,
                                                            ring_write,
                                                            bcs_tick};
