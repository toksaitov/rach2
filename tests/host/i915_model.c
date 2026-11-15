/*
 * i915_model.c: the core of the host model of the HD 4400: BAR 0's
 * register file, the clock, the log, the configuration space, and the
 * table of the blocks that give parts of the chip their behaviour.
 */

#include "i915_model.h"
#include "i915_lenovo.h"
#include "arch/x86/portio.h"

extern void *calloc(unsigned long count, unsigned long size);
extern void free(void *p);

/** The host has no VGA I/O ports; the model backend never reaches them. */
uint8_t inb(uint16_t port)
{
    (void)port;
    return 0;
}

/** The host has no VGA I/O ports; the model backend never reaches them. */
void outb(uint16_t port, uint8_t value)
{
    (void)port;
    (void)value;
}

/*
 * The blocks, one file each, included here and listed in the table below,
 * one line each in the order of their first register (tasks.md's shared
 * files), so that every test of the model has every block. A task adds
 * its block as
 *
 *     #include "i915_model_base.c"
 *
 * above the table and `&i915_model_base,` in it. A block whose range lies
 * inside another's, as the eDP sink's inside the pipes', comes before it,
 * since the first block that holds a register answers. The table ends
 * with NULL.
 */

#include "i915_model_base.c"
#include "i915_model_ring.c"
#include "i915_model_ctx.c"
#include "i915_model_reset.c"
#include "i915_model_rc6.c"
#include "i915_model_pipe.c"
#include "i915_model_sink.c"
#include "i915_model_ddi.c"

static const struct i915_model_block *const i915_model_blocks[I915_MODEL_BLOCKS + 1] = {
    &i915_model_ring_rcs,
    &i915_model_reset_block,
    &i915_model_rc6_rps,
    &i915_model_base_fw,
    &i915_model_ring_vcs,
    &i915_model_ring_vecs,
    &i915_model_ring_bcs,
    &i915_model_base_unclaimed,
    &i915_model_ring_irq,
    &i915_model_base_pw,
    &i915_model_sink_block,
    &i915_model_ddi_ports,
    &i915_model_pipe_block,
    &i915_model_ddi_hotplug,
    &i915_model_ring_south,
    &i915_model_ddi_gmbus,
    &i915_model_ddi_aux,
    &i915_model_panel_block,
    &i915_model_base_fifo,
    &i915_model_rc6_pcu,
    &i915_model_base_gtt,
    &i915_model_reset_state_block,
    &i915_model_reset_chicken_block,
    0};

int i915_model_nblocks(void)
{
    int n = 0;

    while (n < I915_MODEL_BLOCKS && i915_model_blocks[n]) {
        n++;
    }
    return n;
}

const struct i915_model_block *i915_model_block_at(int i)
{
    return i >= 0 && i < i915_model_nblocks() ? i915_model_blocks[i] : 0;
}

/** The block that answers for `reg`, with its index in `*at`, or NULL. */
static const struct i915_model_block *block_for(uint32_t reg, int *at)
{
    int i, n = i915_model_nblocks();

    for (i = 0; i < n; i++) {
        if (reg >= i915_model_blocks[i]->first && reg <= i915_model_blocks[i]->last) {
            *at = i;
            return i915_model_blocks[i];
        }
    }
    return 0;
}

/** Add one access to the log, or count it as lost when the log is full. */
static void log_access(struct i915_model *m, int op, uint32_t reg, uint32_t value)
{
    if (m->quiet_gtt && op != I915_MODEL_DELAY && reg >= I915_GSM_OFFSET) {
        return;
    }
    if (m->nlog < I915_MODEL_LOG) {
        m->log[m->nlog].op = (uint8_t)op;
        m->log[m->nlog].reg = reg;
        m->log[m->nlog].value = value;
        m->nlog++;
    } else {
        m->log_lost++;
    }
}

uint32_t i915_model_peek(struct i915_model *m, uint32_t reg)
{
    return reg < I915_MODEL_BAR0 ? m->regs[reg / 4] : 0xffffffffu;
}

void i915_model_poke(struct i915_model *m, uint32_t reg, uint32_t value)
{
    if (reg < I915_MODEL_BAR0) {
        m->regs[reg / 4] = value;
    }
}

/** i915_hw.h's read: the block's answer, or the register file's. */
static uint32_t model_read32(struct i915_hw *hw, uint32_t reg)
{
    struct i915_model *m = (struct i915_model *)hw->priv;
    const struct i915_model_block *b;
    uint32_t value;
    int at = 0;

    b = block_for(reg, &at);
    value = b && b->read ? b->read(m, m->priv[at], reg) : i915_model_peek(m, reg);
    if (m->on_read) {
        value = m->on_read(m, reg, value);
    }
    m->reads++;
    log_access(m, I915_MODEL_READ, reg, value);
    return value;
}

/** i915_hw.h's write: to the block, or to the register file. */
static void model_write32(struct i915_hw *hw, uint32_t reg, uint32_t value)
{
    struct i915_model *m = (struct i915_model *)hw->priv;
    const struct i915_model_block *b;
    int at = 0;

    m->writes++;
    log_access(m, I915_MODEL_WRITE, reg, value);
    b = block_for(reg, &at);
    if (b && b->write) {
        b->write(m, m->priv[at], reg, value);
    } else {
        i915_model_poke(m, reg, value);
    }
}

/** i915_hw.h's wait: the clock moves, every block sees it, then the
 * test's hook runs. */
static void model_delay_us(struct i915_hw *hw, uint32_t us)
{
    struct i915_model *m = (struct i915_model *)hw->priv;
    int i, n = i915_model_nblocks();

    m->delays++;
    m->now_us += us;
    log_access(m, I915_MODEL_DELAY, 0, us);
    for (i = 0; i < n; i++) {
        if (i915_model_blocks[i]->tick) {
            i915_model_blocks[i]->tick(m, m->priv[i], us);
        }
    }
    if (m->on_delay) {
        m->on_delay(m);
    }
}

static const struct i915_hw_ops i915_model_ops = {
    "model", model_read32, model_write32, model_delay_us, 0, 0, 0, 0, 0, 0};

uint32_t i915_model_config(const struct i915_model *m, unsigned at, unsigned width)
{
    uint32_t v = 0;
    unsigned b;

    for (b = 0; b < width && at + b < 256; b++) {
        v |= (uint32_t)m->config[at + b] << (8 * b);
    }
    return v;
}

void i915_model_sync_pci(struct i915_model *m)
{
    int i;

    m->pci.vendor_id = (uint16_t)i915_model_config(m, PCI_VENDOR_ID, 2);
    m->pci.device_id = (uint16_t)i915_model_config(m, PCI_DEVICE_ID, 2);
    m->pci.revision = m->config[PCI_REVISION_ID];
    m->pci.prog_if = m->config[PCI_PROG_IF];
    m->pci.subclass = m->config[PCI_SUBCLASS];
    m->pci.class_code = m->config[PCI_CLASS];
    m->pci.bus = LENOVO_GPU_BUS;
    m->pci.device = LENOVO_GPU_DEVICE;
    m->pci.function = LENOVO_GPU_FUNCTION;
    m->pci.irq = m->config[PCI_INTERRUPT_LINE];
    for (i = 0; i < 6; i++) {
        m->pci.bar[i] = i915_model_config(m, PCI_BAR0 + 4u * (unsigned)i, 4);
    }
}

int i915_model_init(struct i915_model *m)
{
    int i, n = i915_model_nblocks();
    char *p = (char *)m;
    unsigned long k;

    for (k = 0; k < sizeof(*m); k++) {
        p[k] = 0;
    }
    m->regs = (uint32_t *)calloc(I915_MODEL_BAR0 / 4, sizeof(uint32_t));
    if (!m->regs) {
        return -1;
    }
    for (i = 0; i < n; i++) {
        m->priv[i] =
            calloc(1, i915_model_blocks[i]->priv_size ? i915_model_blocks[i]->priv_size : 1);
        if (!m->priv[i]) {
            i915_model_free(m);
            return -1;
        }
    }
    m->hw.ops = &i915_model_ops;
    m->hw.priv = m;
    m->hw.size = I915_MODEL_BAR0;
    for (k = 0; k < sizeof(lenovo_gpu_config) / sizeof(lenovo_gpu_config[0]); k++) {
        unsigned b;
        for (b = 0; b < lenovo_gpu_config[k].len; b++) {
            m->config[lenovo_gpu_config[k].at + b] =
                (uint8_t)(lenovo_gpu_config[k].value >> (8 * b));
        }
    }
    i915_model_sync_pci(m);
    for (i = 0; i < n; i++) {
        if (i915_model_blocks[i]->reset) {
            i915_model_blocks[i]->reset(m, m->priv[i]);
        }
    }
    return 0;
}

void i915_model_free(struct i915_model *m)
{
    int i;

    for (i = 0; i < I915_MODEL_BLOCKS; i++) {
        free(m->priv[i]);
        m->priv[i] = 0;
    }
    free(m->regs);
    m->regs = 0;
}

uint32_t i915_model_count(const struct i915_model *m, int op, uint32_t reg)
{
    uint32_t i, n = 0;

    for (i = 0; i < m->nlog; i++) {
        if (m->log[i].op == op && m->log[i].reg == reg) {
            n++;
        }
    }
    return n;
}

long i915_model_find_write(const struct i915_model *m, uint32_t reg, uint32_t value, uint32_t from)
{
    uint32_t i;

    for (i = from; i < m->nlog; i++) {
        if (m->log[i].op == I915_MODEL_WRITE && m->log[i].reg == reg && m->log[i].value == value) {
            return (long)i;
        }
    }
    return -1;
}
