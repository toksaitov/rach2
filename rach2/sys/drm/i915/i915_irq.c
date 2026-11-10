/*
 * i915_irq.c: the Intel graphics driver's interrupts on Haswell: the reset
 * of every source, the table of handlers by bank and bit, and the device's
 * interrupt, after Linux 6.12's i915_irq.c and gt/intel_gt_irq.c.
 */

#include "i915.h"
#include "i915_reg.h"

#include "../../klog.h"
#include "../../lib/kerrno.h"

/* Each bank's mask, identity and enable registers, by I915_IRQ_ index. */
static const uint32_t i915_irq_regs[I915_IRQ_BANKS][3] = {{GTIMR, GTIIR, GTIER},
                                                          {GEN6_PMIMR, GEN6_PMIIR, GEN6_PMIER},
                                                          {DEIMR, DEIIR, DEIER},
                                                          {SDEIMR, SDEIIR, SDEIER}};

#define REG_IMR(bank) i915_irq_regs[bank][0]
#define REG_IIR(bank) i915_irq_regs[bank][1]
#define REG_IER(bank) i915_irq_regs[bank][2]

/* The bank names for the log. */
static const char *const i915_irq_names[I915_IRQ_BANKS] = {"gt", "pm", "de", "sde"};

/*
 * The display engine's bits no owner registers: the master enable, which
 * is not a source, and the PCH event, which carries the south display's
 * sources and is enabled with the first of them.
 */
#define DE_RESERVED (DE_MASTER_IRQ_CONTROL | DE_PCH_EVENT_IVB)

/**
 * Write the mask and enable of `bank` from the driver's copies. The
 * display engine's enable keeps the master bit as `i915_irq_install` set
 * it. Under the interrupt mask's lock.
 */
static void i915_irq_write_bank(struct drm_i915_private *i915, int bank)
{
    struct i915_irq *irq = &i915->irq;
    uint32_t ier = irq->ier[bank];

    if (bank == I915_IRQ_DE && irq->installed) {
        ier |= DE_MASTER_IRQ_CONTROL;
    }
    i915_write(&i915->hw, REG_IMR(bank), irq->imr[bank]);
    i915_write(&i915->hw, REG_IER(bank), ier);
    i915_posting_read(&i915->hw, REG_IMR(bank));
}

void i915_irq_reset(struct drm_i915_private *i915)
{
    struct i915_hw *hw = &i915->hw;
    struct i915_irq *irq = &i915->irq;
    uint32_t flags = spin_lock_irqsave(&irq->lock);
    int bank, bit;

    /* ilk_irq_reset's order: the display engine's, the GT's, then power
     * management's and the south display's. */
    static const int order[I915_IRQ_BANKS] = {I915_IRQ_DE, I915_IRQ_GT, I915_IRQ_PM, I915_IRQ_SDE};
    int i;

    for (i = 0; i < I915_IRQ_BANKS; i++) {
        bank = order[i];
        i915_write(hw, REG_IMR(bank), 0xffffffffu);
        i915_posting_read(hw, REG_IMR(bank));
        i915_write(hw, REG_IER(bank), 0);
        i915_write(hw, REG_IIR(bank), 0xffffffffu);
        i915_posting_read(hw, REG_IIR(bank));
        i915_write(hw, REG_IIR(bank), 0xffffffffu);
        i915_posting_read(hw, REG_IIR(bank));
        if (bank == I915_IRQ_DE) {
            i915_write(hw, GEN7_ERR_INT, 0xffffffffu);
            i915_write(hw, EDP_PSR_IMR, 0xffffffffu);
            i915_write(hw, EDP_PSR_IIR, 0xffffffffu);
        }
        irq->imr[bank] = 0xffffffffu;
        irq->ier[bank] = 0;
        for (bit = 0; bit < 32; bit++) {
            irq->fn[bank][bit] = 0;
            irq->data[bank][bit] = 0;
        }
    }
    i915_write(hw, SERR_INT, 0xffffffffu);
    irq->installed = 0;
    irq->deferred = 0;
    irq->deferred_display = 0;
    spin_unlock_irqrestore(&irq->lock, flags);
}

void i915_irq_install(struct drm_i915_private *i915)
{
    uint32_t flags = spin_lock_irqsave(&i915->irq.lock);

    i915->irq.installed = 1;
    i915_irq_write_bank(i915, I915_IRQ_DE);
    spin_unlock_irqrestore(&i915->irq.lock, flags);
}

/** Whether any bit of the south display has a handler. */
static int i915_irq_south_used(const struct i915_irq *irq)
{
    int bit;

    for (bit = 0; bit < 32; bit++) {
        if (irq->fn[I915_IRQ_SDE][bit]) {
            return 1;
        }
    }
    return 0;
}

/** Enable and unmask the display engine's PCH event while any south
 * display source has a handler, and mask and disable it otherwise. Under
 * the interrupt mask's lock. */
static void i915_irq_south_route(struct drm_i915_private *i915)
{
    struct i915_irq *irq = &i915->irq;

    if (i915_irq_south_used(irq)) {
        irq->ier[I915_IRQ_DE] |= DE_PCH_EVENT_IVB;
        irq->imr[I915_IRQ_DE] &= ~DE_PCH_EVENT_IVB;
    } else {
        irq->ier[I915_IRQ_DE] &= ~DE_PCH_EVENT_IVB;
        irq->imr[I915_IRQ_DE] |= DE_PCH_EVENT_IVB;
    }
    i915_irq_write_bank(i915, I915_IRQ_DE);
}

int32_t i915_irq_register(struct drm_i915_private *i915, int bank, uint32_t bits, i915_irq_fn fn,
                          void *data)
{
    struct i915_irq *irq = &i915->irq;
    uint32_t flags;
    int bit;

    if (bank < 0 || bank >= I915_IRQ_BANKS || bits == 0 || !fn ||
        (bank == I915_IRQ_DE && (bits & DE_RESERVED))) {
        return -EINVAL;
    }
    flags = spin_lock_irqsave(&irq->lock);
    for (bit = 0; bit < 32; bit++) {
        if ((bits & (1u << bit)) && irq->fn[bank][bit]) {
            spin_unlock_irqrestore(&irq->lock, flags);
            return -EBUSY;
        }
    }
    for (bit = 0; bit < 32; bit++) {
        if (bits & (1u << bit)) {
            irq->fn[bank][bit] = fn;
            irq->data[bank][bit] = data;
        }
    }
    irq->imr[bank] |= bits;
    irq->ier[bank] |= bits;
    i915_irq_write_bank(i915, bank);
    if (bank == I915_IRQ_SDE) {
        i915_irq_south_route(i915);
    }
    spin_unlock_irqrestore(&irq->lock, flags);
    return 0;
}

void i915_irq_unregister(struct drm_i915_private *i915, int bank, uint32_t bits)
{
    struct i915_irq *irq = &i915->irq;
    uint32_t flags;
    int bit;

    if (bank < 0 || bank >= I915_IRQ_BANKS) {
        return;
    }
    if (bank == I915_IRQ_DE) {
        bits &= ~DE_RESERVED;
    }
    flags = spin_lock_irqsave(&irq->lock);
    for (bit = 0; bit < 32; bit++) {
        if (bits & (1u << bit)) {
            irq->fn[bank][bit] = 0;
            irq->data[bank][bit] = 0;
        }
    }
    irq->imr[bank] |= bits;
    irq->ier[bank] &= ~bits;
    i915_irq_write_bank(i915, bank);
    if (bank == I915_IRQ_SDE) {
        i915_irq_south_route(i915);
    }
    spin_unlock_irqrestore(&irq->lock, flags);
}

/** The bits of `bits` in `bank` that have a handler. */
static uint32_t i915_irq_owned(const struct i915_irq *irq, int bank, uint32_t bits)
{
    uint32_t owned = 0;
    int bit;

    for (bit = 0; bit < 32; bit++) {
        if ((bits & (1u << bit)) && irq->fn[bank][bit]) {
            owned |= 1u << bit;
        }
    }
    return owned;
}

void i915_irq_unmask(struct drm_i915_private *i915, int bank, uint32_t bits)
{
    struct i915_irq *irq = &i915->irq;
    uint32_t flags;

    if (bank < 0 || bank >= I915_IRQ_BANKS) {
        return;
    }
    flags = spin_lock_irqsave(&irq->lock);
    irq->imr[bank] &= ~i915_irq_owned(irq, bank, bits);
    i915_write(&i915->hw, REG_IMR(bank), irq->imr[bank]);
    i915_posting_read(&i915->hw, REG_IMR(bank));
    spin_unlock_irqrestore(&irq->lock, flags);
}

void i915_irq_mask_locked(struct drm_i915_private *i915, int bank, uint32_t bits)
{
    struct i915_irq *irq = &i915->irq;

    if (bank < 0 || bank >= I915_IRQ_BANKS) {
        return;
    }
    if (bank == I915_IRQ_DE) {
        bits &= ~DE_RESERVED;
    }
    irq->imr[bank] |= bits;
    i915_write(&i915->hw, REG_IMR(bank), irq->imr[bank]);
    i915_posting_read(&i915->hw, REG_IMR(bank));
}

void i915_irq_mask(struct drm_i915_private *i915, int bank, uint32_t bits)
{
    uint32_t flags = spin_lock_irqsave(&i915->irq.lock);

    i915_irq_mask_locked(i915, bank, bits);
    spin_unlock_irqrestore(&i915->irq.lock, flags);
}

/**
 * Hand the acknowledged identity bits `iir` of `bank` to their handlers,
 * each handler once with all of its bits. A bit with no handler is counted
 * as unclaimed, logged, and masked and disabled in the driver's copies;
 * its mask is written here, its enable by the caller (the display
 * engine's and the south display's, whose enables the handler restores)
 * or here (the GT's and power management's). Under the interrupt mask's
 * lock.
 */
static void i915_irq_dispatch(struct drm_i915_private *i915, int bank, uint32_t iir)
{
    struct i915_irq *irq = &i915->irq;
    uint32_t done = 0, lost;
    int bit, other;

    for (bit = 0; bit < 32; bit++) {
        uint32_t mine = 0;

        if (!(iir & (1u << bit)) || (done & (1u << bit)) || !irq->fn[bank][bit]) {
            continue;
        }
        for (other = bit; other < 32; other++) {
            if ((iir & (1u << other)) && irq->fn[bank][other] == irq->fn[bank][bit] &&
                irq->data[bank][other] == irq->data[bank][bit]) {
                mine |= 1u << other;
            }
        }
        done |= mine;
        irq->fn[bank][bit](i915, mine, irq->data[bank][bit]);
    }
    lost = iir & ~done;
    if (bank == I915_IRQ_DE) {
        lost &= ~DE_PCH_EVENT_IVB;
    }
    if (lost == 0) {
        return;
    }
    for (bit = 0; bit < 32; bit++) {
        if (lost & (1u << bit)) {
            irq->unclaimed++;
        }
    }
    klog(KLOG_WARN, "i915", "interrupts: %s identity 0x%08x has no handler; masked",
         i915_irq_names[bank], (unsigned)lost);
    irq->imr[bank] |= lost;
    irq->ier[bank] &= ~lost;
    i915_write(&i915->hw, REG_IMR(bank), irq->imr[bank]);
    if (bank == I915_IRQ_GT || bank == I915_IRQ_PM) {
        i915_write(&i915->hw, REG_IER(bank), irq->ier[bank]);
    }
}

void i915_irq_handle(struct drm_i915_private *i915)
{
    struct i915_hw *hw = &i915->hw;
    struct i915_irq *irq = &i915->irq;
    uint32_t flags = spin_lock_irqsave(&irq->lock);
    uint32_t de_ier, sde_ier, gt_iir, de_iir, sde_iir, pm_iir;
    int found = 0;

    irq->messages++;
    /* The master enable off before any identity is cleared, and the south
     * display's enables off, so that an event behind one cleared raises a
     * new message once they are restored. */
    de_ier = i915_read(hw, DEIER);
    i915_write(hw, DEIER, de_ier & ~DE_MASTER_IRQ_CONTROL);
    sde_ier = i915_read(hw, SDEIER);
    i915_write(hw, SDEIER, 0);

    gt_iir = i915_read(hw, GTIIR);
    if (gt_iir) {
        i915_write(hw, GTIIR, gt_iir);
        i915_irq_dispatch(i915, I915_IRQ_GT, gt_iir);
        found = 1;
    }
    de_iir = i915_read(hw, DEIIR);
    if (de_iir) {
        i915_write(hw, DEIIR, de_iir);
        if (de_iir & DE_PCH_EVENT_IVB) {
            sde_iir = i915_read(hw, SDEIIR);
            i915_irq_dispatch(i915, I915_IRQ_SDE, sde_iir);
            /* The south display's identity before the display engine's
             * event is taken as done, as Linux's cpt_irq_handler. */
            i915_write(hw, SDEIIR, sde_iir);
        }
        i915_irq_dispatch(i915, I915_IRQ_DE, de_iir);
        found = 1;
    }
    pm_iir = i915_read(hw, GEN6_PMIIR);
    if (pm_iir) {
        i915_write(hw, GEN6_PMIIR, pm_iir);
        i915_irq_dispatch(i915, I915_IRQ_PM, pm_iir);
        found = 1;
    }
    if (!found) {
        irq->spurious++;
    }

    i915_write(hw, DEIER, (de_ier & DE_MASTER_IRQ_CONTROL) | irq->ier[I915_IRQ_DE]);
    if (sde_ier || irq->ier[I915_IRQ_SDE]) {
        i915_write(hw, SDEIER, irq->ier[I915_IRQ_SDE]);
    }
    spin_unlock_irqrestore(&irq->lock, flags);
    if (irq->deferred) {
        irq->deferred(i915);
    }
    if (irq->deferred_display) {
        irq->deferred_display(i915);
    }
}
