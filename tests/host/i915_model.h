/*
 * i915_model.h: a software HD 4400 that the Intel graphics driver's host
 * tests run the driver against, through the driver's own hardware boundary.
 *
 * The model is BAR 0 as a register file, the chip's PCI configuration
 * space, a clock in microseconds that moves only when the driver waits,
 * and a log of every access in order. It implements i915_hw.h: a test
 * hands `&model.hw` to the driver as the driver's MMIO backend would be,
 * so every register the driver touches passes through here.
 *
 * Blocks. A register file alone answers what was last written. The parts
 * of the chip with behaviour, forcewake, the rings, the reset, RC6, the
 * pipes, the sinks and the DDI, are blocks, each in a file of its own
 * that one task writes (tasks.md's shared files): `i915_model_base.c`,
 * `i915_model_ring.c`, `i915_model_reset.c`, `i915_model_rc6.c`,
 * `i915_model_pipe.c`, `i915_model_sink.c` and `i915_model_ddi.c`. A block
 * is a `struct i915_model_block` that i915_model.c includes and lists in
 * its table, one line each. A block answers for a range of registers, may
 * keep state of its own, and sees time pass; the first block of the table
 * whose range holds a register answers for it, and the register file
 * answers for the rest.
 *
 * The seed. `i915_model_init` gives the chip the Lenovo's identity and
 * configuration space (i915_lenovo.h): a test that needs another chip
 * changes `config` and calls `i915_model_sync_pci`.
 *
 * Every function is for one thread, the test's.
 */
#ifndef _I915_MODEL_H_
#define _I915_MODEL_H_

#include "drm/i915/i915_hw.h"
#include "pci.h"

/* BAR 0 whole: the registers and the GTT. */
#define I915_MODEL_BAR0 0x400000u

/* The accesses the log keeps; later ones are counted in `log_lost`. */
#define I915_MODEL_LOG 8192

/* The most blocks the table holds. */
#define I915_MODEL_BLOCKS 24

enum i915_model_op { I915_MODEL_READ, I915_MODEL_WRITE, I915_MODEL_DELAY };

/* One access: a read and the value it returned, a write and the value
 * written, or a wait with its microseconds in `value` (reg 0). */
struct i915_model_access {
    uint8_t op; /* enum i915_model_op */
    uint32_t reg;
    uint32_t value;
};

struct i915_model;

/*
 * A part of the chip with behaviour. It answers for the registers `first`
 * to `last`, inclusive. `priv_size` bytes of state are given to it zeroed
 * at init, and `reset` sets them and its registers to the chip's power-on
 * values. `read` returns a register's value, and `write` takes one; both
 * may use the register file (`i915_model_peek`, `i915_model_poke`). `tick`
 * sees `us` microseconds pass. Any hook may be NULL: a block without
 * `read` or `write` leaves that to the register file.
 */
struct i915_model_block {
    const char *name;
    uint32_t first, last;
    unsigned long priv_size;
    void (*reset)(struct i915_model *m, void *priv);
    uint32_t (*read)(struct i915_model *m, void *priv, uint32_t reg);
    void (*write)(struct i915_model *m, void *priv, uint32_t reg, uint32_t value);
    void (*tick)(struct i915_model *m, void *priv, uint32_t us);
};

/*
 * The model. `hw` is the driver's way in. `regs` is BAR 0 as dwords.
 * `config` is the configuration space of 00:02.0 and `pci` the function as
 * a search finds it, made from `config`. `now_us` is the clock. The hooks
 * are the test's own: `on_delay` runs after each wait the driver makes,
 * and `on_read`, when set, may change the value a read returns.
 */
struct i915_model {
    struct i915_hw hw;
    uint32_t *regs;
    uint8_t config[256];
    pci_device_t pci;
    uint64_t now_us;
    struct i915_model_access log[I915_MODEL_LOG];
    uint32_t nlog, log_lost;
    int quiet_gtt; /* set, the GTT's entries are kept out of the log */
    uint32_t reads, writes, delays;
    void *priv[I915_MODEL_BLOCKS];
    void *test;
    void (*on_delay)(struct i915_model *m);
    uint32_t (*on_read)(struct i915_model *m, uint32_t reg, uint32_t value);
};

/** Make the Lenovo's chip, with every block reset; 0, or -1 when the host
 * has no memory for it. */
int i915_model_init(struct i915_model *m);

/** Free what `i915_model_init` took. */
void i915_model_free(struct i915_model *m);

/** Fill `pci` from `config`, as a search of the bus would. */
void i915_model_sync_pci(struct i915_model *m);

/** The register file's value at `reg`, past every block and the log. */
uint32_t i915_model_peek(struct i915_model *m, uint32_t reg);

/** Set the register file's value at `reg`, past every block and the log. */
void i915_model_poke(struct i915_model *m, uint32_t reg, uint32_t value);

/** The number of blocks in the table. */
int i915_model_nblocks(void);

/** The block at `i` of the table, or NULL. */
const struct i915_model_block *i915_model_block_at(int i);

/** How many accesses of kind `op` to `reg` the log holds. */
uint32_t i915_model_count(const struct i915_model *m, int op, uint32_t reg);

/** The log index of the first write of `value` to `reg` at or after
 * `from`, or -1. */
long i915_model_find_write(const struct i915_model *m, uint32_t reg, uint32_t value, uint32_t from);

/** A configuration word of `width` bytes at `at`, as the chip reads it. */
uint32_t i915_model_config(const struct i915_model *m, unsigned at, unsigned width);

#endif /* _I915_MODEL_H_ */
